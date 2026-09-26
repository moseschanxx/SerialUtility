#include "support/TestSftpHandler.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QtEndian>

#include <algorithm>

namespace {

// SFTP version 3 wire constants (draft-ietf-secsh-filexfer-02); kept local so this file does
// not depend on libssh at all.
constexpr quint8 kFxpInit = 1;
constexpr quint8 kFxpVersion = 2;
constexpr quint8 kFxpOpen = 3;
constexpr quint8 kFxpClose = 4;
constexpr quint8 kFxpRead = 5;
constexpr quint8 kFxpWrite = 6;
constexpr quint8 kFxpLstat = 7;
constexpr quint8 kFxpFstat = 8;
constexpr quint8 kFxpSetstat = 9;
constexpr quint8 kFxpFsetstat = 10;
constexpr quint8 kFxpOpendir = 11;
constexpr quint8 kFxpReaddir = 12;
constexpr quint8 kFxpRemove = 13;
constexpr quint8 kFxpMkdir = 14;
constexpr quint8 kFxpRmdir = 15;
constexpr quint8 kFxpRealpath = 16;
constexpr quint8 kFxpStat = 17;
constexpr quint8 kFxpRename = 18;
constexpr quint8 kFxpStatus = 101;
constexpr quint8 kFxpHandle = 102;
constexpr quint8 kFxpData = 103;
constexpr quint8 kFxpName = 104;
constexpr quint8 kFxpAttrs = 105;

constexpr quint32 kFxOk = 0;
constexpr quint32 kFxEof = 1;
constexpr quint32 kFxNoSuchFile = 2;
constexpr quint32 kFxPermissionDenied = 3;
constexpr quint32 kFxFailure = 4;
constexpr quint32 kFxBadMessage = 5;
constexpr quint32 kFxOpUnsupported = 8;

constexpr quint32 kFxfRead = 0x01;
constexpr quint32 kFxfWrite = 0x02;
constexpr quint32 kFxfAppend = 0x04;
constexpr quint32 kFxfCreat = 0x08;
constexpr quint32 kFxfTrunc = 0x10;
constexpr quint32 kFxfExcl = 0x20;

constexpr quint32 kAttrSize = 0x00000001;
constexpr quint32 kAttrUidGid = 0x00000002;
constexpr quint32 kAttrPermissions = 0x00000004;
constexpr quint32 kAttrAcModTime = 0x00000008;
constexpr quint32 kAttrExtended = 0x80000000;

constexpr quint32 kModeDirectory = 0040000;
constexpr quint32 kModeRegular = 0100000;
constexpr quint32 kModeSymlink = 0120000;

constexpr quint32 kMaxPacket = 4 * 1024 * 1024;   ///< an obviously corrupt stream is dropped beyond this
constexpr int kMaxHandles = 256;
constexpr qint64 kMaxReadChunk = 256 * 1024;
constexpr int kReaddirBatch = 100;

// ---- Wire helpers -------------------------------------------------------------------------

class Reader
{
public:
    explicit Reader(const QByteArray& data)
        : m_data(data)
    {
    }

    bool ok() const { return m_ok; }

    quint8 u8()
    {
        if (!need(1)) {
            return 0;
        }
        return static_cast<quint8>(m_data.at(m_pos++));
    }

    quint32 u32()
    {
        if (!need(4)) {
            return 0;
        }
        const quint32 v = qFromBigEndian<quint32>(m_data.constData() + m_pos);
        m_pos += 4;
        return v;
    }

    quint64 u64()
    {
        if (!need(8)) {
            return 0;
        }
        const quint64 v = qFromBigEndian<quint64>(m_data.constData() + m_pos);
        m_pos += 8;
        return v;
    }

    QByteArray string()
    {
        const quint32 n = u32();
        if (!need(n)) {
            return {};
        }
        const QByteArray s = m_data.mid(m_pos, static_cast<qsizetype>(n));
        m_pos += static_cast<qsizetype>(n);
        return s;
    }

private:
    bool need(quint64 n)
    {
        if (!m_ok || static_cast<quint64>(m_data.size() - m_pos) < n) {
            m_ok = false;
            return false;
        }
        return true;
    }

    const QByteArray& m_data;
    qsizetype m_pos = 0;
    bool m_ok = true;
};

void putU8(QByteArray& out, quint8 v)
{
    out.append(static_cast<char>(v));
}

void putU32(QByteArray& out, quint32 v)
{
    char buf[4];
    qToBigEndian<quint32>(v, buf);
    out.append(buf, 4);
}

void putU64(QByteArray& out, quint64 v)
{
    char buf[8];
    qToBigEndian<quint64>(v, buf);
    out.append(buf, 8);
}

void putString(QByteArray& out, const QByteArray& s)
{
    putU32(out, static_cast<quint32>(s.size()));
    out.append(s);
}

QByteArray packet(quint8 type, const QByteArray& body)
{
    QByteArray p;
    p.reserve(body.size() + 5);
    putU32(p, static_cast<quint32>(body.size()) + 1);
    putU8(p, type);
    p.append(body);
    return p;
}

struct Attrs
{
    quint32 flags = 0;
    quint64 size = 0;
    quint32 uid = 0;
    quint32 gid = 0;
    quint32 permissions = 0;
    quint32 atime = 0;
    quint32 mtime = 0;
};

Attrs parseAttrs(Reader& r)
{
    Attrs a;
    a.flags = r.u32();
    if (a.flags & kAttrSize) {
        a.size = r.u64();
    }
    if (a.flags & kAttrUidGid) {
        a.uid = r.u32();
        a.gid = r.u32();
    }
    if (a.flags & kAttrPermissions) {
        a.permissions = r.u32();
    }
    if (a.flags & kAttrAcModTime) {
        a.atime = r.u32();
        a.mtime = r.u32();
    }
    if (a.flags & kAttrExtended) {
        const quint32 count = r.u32();
        for (quint32 i = 0; i < count && r.ok(); ++i) {
            r.string();
            r.string();
        }
    }
    return a;
}

void putAttrs(QByteArray& out, const Attrs& a)
{
    const quint32 flags = a.flags & (kAttrSize | kAttrUidGid | kAttrPermissions | kAttrAcModTime);
    putU32(out, flags);
    if (flags & kAttrSize) {
        putU64(out, a.size);
    }
    if (flags & kAttrUidGid) {
        putU32(out, a.uid);
        putU32(out, a.gid);
    }
    if (flags & kAttrPermissions) {
        putU32(out, a.permissions);
    }
    if (flags & kAttrAcModTime) {
        putU32(out, a.atime);
        putU32(out, a.mtime);
    }
}

quint32 posixBits(QFileDevice::Permissions p)
{
    quint32 bits = 0;
    if (p & QFileDevice::ReadOwner) {
        bits |= 0400;
    }
    if (p & QFileDevice::WriteOwner) {
        bits |= 0200;
    }
    if (p & QFileDevice::ExeOwner) {
        bits |= 0100;
    }
    if (p & QFileDevice::ReadGroup) {
        bits |= 040;
    }
    if (p & QFileDevice::WriteGroup) {
        bits |= 020;
    }
    if (p & QFileDevice::ExeGroup) {
        bits |= 010;
    }
    if (p & QFileDevice::ReadOther) {
        bits |= 04;
    }
    if (p & QFileDevice::WriteOther) {
        bits |= 02;
    }
    if (p & QFileDevice::ExeOther) {
        bits |= 01;
    }
    return bits;
}

#ifndef Q_OS_WIN
QFileDevice::Permissions qtPermissions(quint32 bits)
{
    QFileDevice::Permissions p;
    if (bits & 0400) {
        p |= QFileDevice::ReadOwner | QFileDevice::ReadUser;
    }
    if (bits & 0200) {
        p |= QFileDevice::WriteOwner | QFileDevice::WriteUser;
    }
    if (bits & 0100) {
        p |= QFileDevice::ExeOwner | QFileDevice::ExeUser;
    }
    if (bits & 040) {
        p |= QFileDevice::ReadGroup;
    }
    if (bits & 020) {
        p |= QFileDevice::WriteGroup;
    }
    if (bits & 010) {
        p |= QFileDevice::ExeGroup;
    }
    if (bits & 04) {
        p |= QFileDevice::ReadOther;
    }
    if (bits & 02) {
        p |= QFileDevice::WriteOther;
    }
    if (bits & 01) {
        p |= QFileDevice::ExeOther;
    }
    return p;
}
#endif

/// Apply SETSTAT-style permission bits; a no-op on Windows where the POSIX bits have no
/// useful mapping (and a read-only attribute would break the temporary-directory cleanup).
void applyPermissions(const QString& path, quint32 bits)
{
#ifdef Q_OS_WIN
    Q_UNUSED(path)
    Q_UNUSED(bits)
#else
    QFile::setPermissions(path, qtPermissions(bits & 07777));
#endif
}

quint32 secondsOf(const QDateTime& dt)
{
    if (!dt.isValid()) {
        return 0;
    }
    const qint64 s = dt.toSecsSinceEpoch();
    if (s < 0) {
        return 0;
    }
    if (s > 0xFFFFFFFFLL) {
        return 0xFFFFFFFFu;
    }
    return static_cast<quint32>(s);
}

Attrs attrsFor(const QFileInfo& info, qint64 sizeOverride = -1)
{
    Attrs a;
    a.flags = kAttrSize | kAttrUidGid | kAttrPermissions | kAttrAcModTime;
    a.size = static_cast<quint64>(sizeOverride >= 0 ? sizeOverride : std::max<qint64>(0, info.size()));
    a.uid = 1000;
    a.gid = 1000;
    quint32 perm = posixBits(info.permissions());
    if (info.isSymLink()) {
        perm |= kModeSymlink;
    } else if (info.isDir()) {
        perm |= kModeDirectory | 0700;
    } else {
        perm |= kModeRegular;
    }
    a.permissions = perm;
    a.atime = secondsOf(info.lastRead());
    a.mtime = secondsOf(info.lastModified());
    return a;
}

QString modeString(const Attrs& a)
{
    QString s;
    const quint32 type = a.permissions & 0170000;
    s += type == kModeDirectory ? QLatin1Char('d') : (type == kModeSymlink ? QLatin1Char('l') : QLatin1Char('-'));
    static const char* const bits = "rwxrwxrwx";
    for (int i = 0; i < 9; ++i) {
        const quint32 mask = 1u << (8 - i);
        s += (a.permissions & mask) ? QLatin1Char(bits[i]) : QLatin1Char('-');
    }
    return s;
}

/// "ls -l"-style line as the SFTP v3 longname.
QByteArray longnameFor(const QString& name, const QFileInfo& info, const Attrs& a)
{
    const QString date = info.lastModified().toString(QStringLiteral("MMM dd HH:mm"));
    const QString line = QStringLiteral("%1 1 user user %2 %3 %4")
                             .arg(modeString(a), QString::number(a.size).rightJustified(10), date, name);
    return line.toUtf8();
}

void replyStatus(QByteArray* out, quint32 id, quint32 code, const QString& message)
{
    QByteArray body;
    putU32(body, id);
    putU32(body, code);
    putString(body, message.toUtf8());
    putString(body, QByteArray());
    out->append(packet(kFxpStatus, body));
}

void replyOk(QByteArray* out, quint32 id)
{
    replyStatus(out, id, kFxOk, QStringLiteral("Success"));
}

void replyBadMessage(QByteArray* out, quint32 id)
{
    replyStatus(out, id, kFxBadMessage, QStringLiteral("Bad message"));
}

void replyAttrs(QByteArray* out, quint32 id, const Attrs& a)
{
    QByteArray body;
    putU32(body, id);
    putAttrs(body, a);
    out->append(packet(kFxpAttrs, body));
}

bool parseHandle(const QByteArray& handle, quint32* key)
{
    if (handle.size() != 4) {
        return false;
    }
    *key = qFromBigEndian<quint32>(handle.constData());
    return true;
}

} // namespace

// ---------------------------------------------------------------------------------------
// TestSftpHandler
// ---------------------------------------------------------------------------------------

TestSftpHandler::TestSftpHandler(const QString& rootDir)
    : m_root(QDir::cleanPath(QDir(rootDir).absolutePath()))
{
}

TestSftpHandler::~TestSftpHandler() = default;

QString TestSftpHandler::resolvePath(const QString& path) const
{
    QString p = path;
    if (p.isEmpty() || p == QLatin1String(".") || p == QLatin1String("~")) {
        return m_root;
    }
    if (p.startsWith(QLatin1String("~/"))) {
        p = m_root + p.mid(1);
    }
    if (!QDir::isAbsolutePath(p)) {
        p = QDir(m_root).filePath(p);
    }
    return QDir::cleanPath(p);
}

void TestSftpHandler::feed(const QByteArray& data, QByteArray* out)
{
    m_input.append(data);
    while (m_input.size() >= 4) {
        const quint32 length = qFromBigEndian<quint32>(m_input.constData());
        if (length == 0 || length > kMaxPacket) {
            // Not an SFTP stream any more: drop it, the client will run into a timeout.
            m_input.clear();
            return;
        }
        if (static_cast<quint64>(m_input.size()) < 4ull + length) {
            break;
        }
        const quint8 type = static_cast<quint8>(m_input.at(4));
        const QByteArray payload = m_input.mid(5, static_cast<qsizetype>(length) - 1);
        m_input.remove(0, static_cast<qsizetype>(length) + 4);
        handlePacket(type, payload, out);
    }
}

void TestSftpHandler::handlePacket(quint8 type, const QByteArray& payload, QByteArray* out)
{
    ++m_handledRequests;
    if (type == kFxpInit) {
        m_initialised = true;
        QByteArray body;
        putU32(body, 3);
        out->append(packet(kFxpVersion, body));
        return;
    }

    Reader r(payload);
    const quint32 id = r.u32();
    if (!r.ok()) {
        return;   // no request id to answer to
    }
    const QByteArray rest = payload.mid(4);
    switch (type) {
    case kFxpOpen:
        handleOpen(id, rest, out);
        break;
    case kFxpClose:
        handleClose(id, rest, out);
        break;
    case kFxpRead:
        handleRead(id, rest, out);
        break;
    case kFxpWrite:
        handleWrite(id, rest, out);
        break;
    case kFxpLstat:
    case kFxpStat:
        handleStat(id, rest, out);
        break;
    case kFxpFstat:
        handleFstat(id, rest, out);
        break;
    case kFxpSetstat:
        handleSetstat(id, rest, out);
        break;
    case kFxpFsetstat:
        handleFsetstat(id, rest, out);
        break;
    case kFxpOpendir:
        handleOpendir(id, rest, out);
        break;
    case kFxpReaddir:
        handleReaddir(id, rest, out);
        break;
    case kFxpRemove:
        handleRemove(id, rest, out);
        break;
    case kFxpMkdir:
        handleMkdir(id, rest, out);
        break;
    case kFxpRmdir:
        handleRmdir(id, rest, out);
        break;
    case kFxpRealpath:
        handleRealpath(id, rest, out);
        break;
    case kFxpRename:
        handleRename(id, rest, out);
        break;
    default:
        replyStatus(out, id, kFxOpUnsupported, QStringLiteral("Operation not supported"));
        break;
    }
}

TestSftpHandler::Handle* TestSftpHandler::handleFor(const QByteArray& handle)
{
    quint32 key = 0;
    if (!parseHandle(handle, &key)) {
        return nullptr;
    }
    const auto it = m_handles.find(key);
    return it == m_handles.end() ? nullptr : it->second.get();
}

quint32 TestSftpHandler::allocateHandle(std::unique_ptr<Handle> handle)
{
    const quint32 key = m_nextHandle++;
    m_handles.emplace(key, std::move(handle));
    return key;
}

void TestSftpHandler::handleOpen(quint32 id, const QByteArray& payload, QByteArray* out)
{
    Reader r(payload);
    const QByteArray name = r.string();
    const quint32 pflags = r.u32();
    const Attrs attrs = parseAttrs(r);
    if (!r.ok()) {
        replyBadMessage(out, id);
        return;
    }
    if (m_handles.size() >= static_cast<size_t>(kMaxHandles)) {
        replyStatus(out, id, kFxFailure, QStringLiteral("Too many open handles"));
        return;
    }

    const QString path = resolvePath(QString::fromUtf8(name));
    const QFileInfo info(path);
    const bool existed = info.exists();
    if (existed && info.isDir()) {
        replyStatus(out, id, kFxFailure, QStringLiteral("Is a directory"));
        return;
    }
    const bool read = (pflags & kFxfRead) != 0;
    const bool write = (pflags & kFxfWrite) != 0;
    const bool append = (pflags & kFxfAppend) != 0;
    const bool create = (pflags & kFxfCreat) != 0;
    const bool truncate = (pflags & kFxfTrunc) != 0;
    const bool exclusive = (pflags & kFxfExcl) != 0;
    if (!existed && !create) {
        replyStatus(out, id, kFxNoSuchFile, QStringLiteral("No such file"));
        return;
    }
    if (existed && exclusive) {
        replyStatus(out, id, kFxFailure, QStringLiteral("File exists"));
        return;
    }

    QIODevice::OpenMode mode;
    if (write && append) {
        mode = QIODevice::Append;
        if (read) {
            mode |= QIODevice::ReadOnly;
        }
    } else if (write && truncate) {
        mode = QIODevice::WriteOnly | QIODevice::Truncate;
        if (read) {
            mode |= QIODevice::ReadOnly;
        }
    } else if (write) {
        mode = QIODevice::ReadWrite;   // WriteOnly alone would truncate
    } else {
        mode = QIODevice::ReadOnly;
    }
    if (!create) {
        mode |= QIODevice::ExistingOnly;
    }
    if (exclusive) {
        mode |= QIODevice::NewOnly;
    }

    auto file = std::make_unique<QFile>(path);
    if (!file->open(mode)) {
        const QString message = file->errorString();
        replyStatus(out, id, QFileInfo::exists(path) ? kFxPermissionDenied : kFxNoSuchFile, message);
        return;
    }
    if (!existed && (attrs.flags & kAttrPermissions)) {
        applyPermissions(path, attrs.permissions);
    }

    auto handle = std::make_unique<Handle>();
    handle->path = path;
    handle->file = std::move(file);
    handle->append = append;
    const quint32 key = allocateHandle(std::move(handle));

    QByteArray body;
    putU32(body, id);
    QByteArray handleBytes;
    putU32(handleBytes, key);
    putString(body, handleBytes);
    out->append(packet(kFxpHandle, body));
}

void TestSftpHandler::handleClose(quint32 id, const QByteArray& payload, QByteArray* out)
{
    Reader r(payload);
    const QByteArray handle = r.string();
    quint32 key = 0;
    if (!r.ok() || !parseHandle(handle, &key) || m_handles.find(key) == m_handles.end()) {
        replyStatus(out, id, kFxFailure, QStringLiteral("Invalid handle"));
        return;
    }
    m_handles.erase(key);   // QFile destructor flushes and closes
    replyOk(out, id);
}

void TestSftpHandler::handleRead(quint32 id, const QByteArray& payload, QByteArray* out)
{
    Reader r(payload);
    const QByteArray handle = r.string();
    const quint64 offset = r.u64();
    const quint32 length = r.u32();
    if (!r.ok()) {
        replyBadMessage(out, id);
        return;
    }
    Handle* h = handleFor(handle);
    if (!h || !h->file) {
        replyStatus(out, id, kFxFailure, QStringLiteral("Invalid handle"));
        return;
    }
    if (!h->file->seek(static_cast<qint64>(offset))) {
        replyStatus(out, id, kFxEof, QStringLiteral("End of file"));
        return;
    }
    const QByteArray data = h->file->read(std::min<qint64>(static_cast<qint64>(length), kMaxReadChunk));
    if (data.isEmpty()) {
        replyStatus(out, id, kFxEof, QStringLiteral("End of file"));
        return;
    }
    QByteArray body;
    putU32(body, id);
    putString(body, data);
    out->append(packet(kFxpData, body));
}

void TestSftpHandler::handleWrite(quint32 id, const QByteArray& payload, QByteArray* out)
{
    Reader r(payload);
    const QByteArray handle = r.string();
    const quint64 offset = r.u64();
    const QByteArray data = r.string();
    if (!r.ok()) {
        replyBadMessage(out, id);
        return;
    }
    Handle* h = handleFor(handle);
    if (!h || !h->file) {
        replyStatus(out, id, kFxFailure, QStringLiteral("Invalid handle"));
        return;
    }
    if (!h->append && !h->file->seek(static_cast<qint64>(offset))) {
        replyStatus(out, id, kFxFailure, h->file->errorString());
        return;
    }
    const qint64 written = h->file->write(data);
    if (written != data.size()) {
        replyStatus(out, id, kFxFailure, h->file->errorString());
        return;
    }
    h->file->flush();   // STAT by path from the same client must see the new size
    replyOk(out, id);
}

void TestSftpHandler::handleStat(quint32 id, const QByteArray& payload, QByteArray* out)
{
    Reader r(payload);
    const QByteArray name = r.string();
    if (!r.ok()) {
        replyBadMessage(out, id);
        return;
    }
    const QFileInfo info(resolvePath(QString::fromUtf8(name)));
    if (!info.exists() && !info.isSymLink()) {
        replyStatus(out, id, kFxNoSuchFile, QStringLiteral("No such file"));
        return;
    }
    replyAttrs(out, id, attrsFor(info));
}

void TestSftpHandler::handleFstat(quint32 id, const QByteArray& payload, QByteArray* out)
{
    Reader r(payload);
    const QByteArray handle = r.string();
    Handle* h = r.ok() ? handleFor(handle) : nullptr;
    if (!h) {
        replyStatus(out, id, kFxFailure, QStringLiteral("Invalid handle"));
        return;
    }
    qint64 size = -1;
    if (h->file) {
        h->file->flush();
        size = h->file->size();
    }
    replyAttrs(out, id, attrsFor(QFileInfo(h->path), size));
}

void TestSftpHandler::handleSetstat(quint32 id, const QByteArray& payload, QByteArray* out)
{
    Reader r(payload);
    const QByteArray name = r.string();
    const Attrs attrs = parseAttrs(r);
    if (!r.ok()) {
        replyBadMessage(out, id);
        return;
    }
    const QString path = resolvePath(QString::fromUtf8(name));
    if (!QFileInfo::exists(path)) {
        replyStatus(out, id, kFxNoSuchFile, QStringLiteral("No such file"));
        return;
    }
    if ((attrs.flags & kAttrSize) && !QFile::resize(path, static_cast<qint64>(attrs.size))) {
        replyStatus(out, id, kFxFailure, QStringLiteral("Cannot resize"));
        return;
    }
    if (attrs.flags & kAttrPermissions) {
        applyPermissions(path, attrs.permissions);
    }
    replyOk(out, id);
}

void TestSftpHandler::handleFsetstat(quint32 id, const QByteArray& payload, QByteArray* out)
{
    Reader r(payload);
    const QByteArray handle = r.string();
    const Attrs attrs = parseAttrs(r);
    Handle* h = r.ok() ? handleFor(handle) : nullptr;
    if (!h) {
        replyStatus(out, id, kFxFailure, QStringLiteral("Invalid handle"));
        return;
    }
    if ((attrs.flags & kAttrSize) && h->file && !h->file->resize(static_cast<qint64>(attrs.size))) {
        replyStatus(out, id, kFxFailure, QStringLiteral("Cannot resize"));
        return;
    }
    if (attrs.flags & kAttrPermissions) {
        applyPermissions(h->path, attrs.permissions);
    }
    replyOk(out, id);
}

void TestSftpHandler::handleOpendir(quint32 id, const QByteArray& payload, QByteArray* out)
{
    Reader r(payload);
    const QByteArray name = r.string();
    if (!r.ok()) {
        replyBadMessage(out, id);
        return;
    }
    if (m_handles.size() >= static_cast<size_t>(kMaxHandles)) {
        replyStatus(out, id, kFxFailure, QStringLiteral("Too many open handles"));
        return;
    }
    const QString path = resolvePath(QString::fromUtf8(name));
    const QFileInfo info(path);
    if (!info.exists()) {
        replyStatus(out, id, kFxNoSuchFile, QStringLiteral("No such file"));
        return;
    }
    if (!info.isDir()) {
        replyStatus(out, id, kFxFailure, QStringLiteral("Not a directory"));
        return;
    }
    auto handle = std::make_unique<Handle>();
    handle->path = path;
    handle->isDir = true;
    handle->entries = QDir(path).entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System, QDir::Name);
    const quint32 key = allocateHandle(std::move(handle));

    QByteArray body;
    putU32(body, id);
    QByteArray handleBytes;
    putU32(handleBytes, key);
    putString(body, handleBytes);
    out->append(packet(kFxpHandle, body));
}

void TestSftpHandler::handleReaddir(quint32 id, const QByteArray& payload, QByteArray* out)
{
    Reader r(payload);
    const QByteArray handle = r.string();
    Handle* h = r.ok() ? handleFor(handle) : nullptr;
    if (!h || !h->isDir) {
        replyStatus(out, id, kFxFailure, QStringLiteral("Invalid handle"));
        return;
    }
    if (h->nextEntry >= h->entries.size()) {
        replyStatus(out, id, kFxEof, QStringLiteral("End of directory"));
        return;
    }
    const int end = std::min(h->nextEntry + kReaddirBatch, static_cast<int>(h->entries.size()));
    QByteArray body;
    putU32(body, id);
    putU32(body, static_cast<quint32>(end - h->nextEntry));
    for (int i = h->nextEntry; i < end; ++i) {
        const QFileInfo& entry = h->entries.at(i);
        const QString fileName = entry.fileName();
        const Attrs attrs = attrsFor(entry);
        putString(body, fileName.toUtf8());
        putString(body, longnameFor(fileName, entry, attrs));
        putAttrs(body, attrs);
    }
    h->nextEntry = end;
    out->append(packet(kFxpName, body));
}

void TestSftpHandler::handleRemove(quint32 id, const QByteArray& payload, QByteArray* out)
{
    Reader r(payload);
    const QByteArray name = r.string();
    if (!r.ok()) {
        replyBadMessage(out, id);
        return;
    }
    const QString path = resolvePath(QString::fromUtf8(name));
    const QFileInfo info(path);
    if (!info.exists()) {
        replyStatus(out, id, kFxNoSuchFile, QStringLiteral("No such file"));
        return;
    }
    if (info.isDir()) {
        replyStatus(out, id, kFxFailure, QStringLiteral("Is a directory"));
        return;
    }
    if (!QFile::remove(path)) {
        replyStatus(out, id, kFxPermissionDenied, QStringLiteral("Cannot remove"));
        return;
    }
    replyOk(out, id);
}

void TestSftpHandler::handleMkdir(quint32 id, const QByteArray& payload, QByteArray* out)
{
    Reader r(payload);
    const QByteArray name = r.string();
    const Attrs attrs = parseAttrs(r);
    if (!r.ok()) {
        replyBadMessage(out, id);
        return;
    }
    const QString path = resolvePath(QString::fromUtf8(name));
    if (QFileInfo::exists(path)) {
        replyStatus(out, id, kFxFailure, QStringLiteral("File exists"));
        return;
    }
    if (!QDir().mkdir(path)) {
        replyStatus(out, id, kFxFailure, QStringLiteral("Cannot create directory"));
        return;
    }
    if (attrs.flags & kAttrPermissions) {
        applyPermissions(path, attrs.permissions);
    }
    replyOk(out, id);
}

void TestSftpHandler::handleRmdir(quint32 id, const QByteArray& payload, QByteArray* out)
{
    Reader r(payload);
    const QByteArray name = r.string();
    if (!r.ok()) {
        replyBadMessage(out, id);
        return;
    }
    const QString path = resolvePath(QString::fromUtf8(name));
    const QFileInfo info(path);
    if (!info.exists()) {
        replyStatus(out, id, kFxNoSuchFile, QStringLiteral("No such file"));
        return;
    }
    if (!info.isDir()) {
        replyStatus(out, id, kFxFailure, QStringLiteral("Not a directory"));
        return;
    }
    if (!QDir().rmdir(path)) {
        replyStatus(out, id, kFxFailure, QStringLiteral("Cannot remove directory"));
        return;
    }
    replyOk(out, id);
}

void TestSftpHandler::handleRealpath(quint32 id, const QByteArray& payload, QByteArray* out)
{
    Reader r(payload);
    const QByteArray name = r.string();
    if (!r.ok()) {
        replyBadMessage(out, id);
        return;
    }
    const QFileInfo info(resolvePath(QString::fromUtf8(name)));
    const QString canonical = info.exists() ? info.canonicalFilePath() : QDir::cleanPath(info.absoluteFilePath());
    Attrs attrs;
    if (info.exists()) {
        attrs = attrsFor(info);
    }
    QByteArray body;
    putU32(body, id);
    putU32(body, 1);
    putString(body, canonical.toUtf8());
    putString(body, info.exists() ? longnameFor(canonical, info, attrs) : canonical.toUtf8());
    putAttrs(body, attrs);
    out->append(packet(kFxpName, body));
}

void TestSftpHandler::handleRename(quint32 id, const QByteArray& payload, QByteArray* out)
{
    Reader r(payload);
    const QByteArray oldName = r.string();
    const QByteArray newName = r.string();
    if (!r.ok()) {
        replyBadMessage(out, id);
        return;
    }
    const QString from = resolvePath(QString::fromUtf8(oldName));
    const QString to = resolvePath(QString::fromUtf8(newName));
    if (!QFileInfo::exists(from)) {
        replyStatus(out, id, kFxNoSuchFile, QStringLiteral("No such file"));
        return;
    }
    if (QFileInfo::exists(to)) {
        replyStatus(out, id, kFxFailure, QStringLiteral("File exists"));
        return;
    }
    if (!QFile::rename(from, to) && !QDir().rename(from, to)) {
        replyStatus(out, id, kFxFailure, QStringLiteral("Cannot rename"));
        return;
    }
    replyOk(out, id);
}
