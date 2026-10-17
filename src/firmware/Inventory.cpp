#include "firmware/Inventory.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>
#include <QLockFile>
#include <QMap>
#include <QSaveFile>
#include <QThread>

#include <algorithm>

#include "app/AppSettings.h"
#include "app/Logging.h"
#include "firmware/FleetReport.h"

namespace {

// ---- JSON keys (Inventory.h, the record's shape) -------------------------------------------------
const QLatin1String kSerial("serial");
const QLatin1String kProfile("profile");
const QLatin1String kPort("port");
const QLatin1String kLayout("layout");
const QLatin1String kGadget("gadgetSerialString");
const QLatin1String kLastImage("lastImage");
const QLatin1String kFile("file");
const QLatin1String kChipTag("chipTag");
const QLatin1String kMd5("md5");
const QLatin1String kGitDescribe("gitDescribe");
const QLatin1String kPartitions("partitions");
const QLatin1String kWritten("written");
const QLatin1String kMcu("mcu");
const QLatin1String kVersionKey("version");
const QLatin1String kRole("role");
const QLatin1String kRead("read");
const QLatin1String kLastOperation("lastOperation");
const QLatin1String kName("name");
const QLatin1String kResult("result");
const QLatin1String kMessage("message");
const QLatin1String kTime("time");
const QLatin1String kFirstSeen("firstSeen");
const QLatin1String kLastSeen("lastSeen");
const QLatin1String kUpdated("updated");
const QLatin1String kBoards("boards");

/// A lock older than this whose process still runs is taken over all the same (the inventory's
/// writers hold it for milliseconds; a hung one must not stop the bench's record for good).
constexpr int kStaleLockMs = 30000;
/// A reader on Windows (a plain QFile open without FILE_SHARE_DELETE) makes the rename of a commit
/// fail for that moment: the write is tried again a few times before it is reported.
constexpr int kWriteAttempts = 5;
constexpr int kRetryPauseMs = 40;
constexpr qint64 kMd5ChunkBytes = 1024 * 1024;

QString nativePath(const QString& path)
{
    return QDir::toNativeSeparators(path);
}

/// Now, to the second: what the file can hold (Qt::ISODate), so records() after an update equals
/// records() after a load.
QDateTime nowToTheSecond()
{
    const QDateTime now = QDateTime::currentDateTime();
    return now.addMSecs(-now.time().msec());
}

QString cell(const QString& text)
{
    return text.trimmed().isEmpty() ? QStringLiteral("-") : text.trimmed();
}

QString tableTime(const QDateTime& time)
{
    return time.isValid() ? time.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")) : QStringLiteral("-");
}

/// What reading the file gave: Missing (an empty inventory), Ok, Corrupt (moved aside by the next
/// update()), Unreadable (it exists but cannot be opened: never overwritten, never moved).
struct ReadResult
{
    enum class State { Missing, Ok, Corrupt, Unreadable };
    State state = State::Missing;
    QString reason;
    QMap<QString, InventoryRecord> records;
};

ReadResult parseDocument(const QByteArray& bytes)
{
    ReadResult result;
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        result.state = ReadResult::State::Corrupt;
        result.reason = QStringLiteral("not JSON (%1 at byte %2)").arg(parseError.errorString()).arg(parseError.offset);
        return result;
    }
    if (!document.isObject()) {
        result.state = ReadResult::State::Corrupt;
        result.reason = QStringLiteral("not a JSON object");
        return result;
    }
    const QJsonObject root = document.object();
    const QJsonValue boards = root.value(kBoards);
    // An object without "boards" ("{}" typed by hand) is an empty inventory; "boards" that is
    // anything but an object is not the inventory's shape.
    if (!boards.isUndefined() && !boards.isObject()) {
        result.state = ReadResult::State::Corrupt;
        result.reason = QStringLiteral("\"boards\" is not an object");
        return result;
    }
    const QJsonObject entries = boards.toObject();
    for (auto it = entries.constBegin(); it != entries.constEnd(); ++it) {
        const QString key = it.key().trimmed();
        if (key.isEmpty() || !it.value().isObject()) {
            qCWarning(lcFirmware).noquote() << "inventory: entry" << it.key() << "ignored: not a board record";
            continue;
        }
        result.records.insert(key, InventoryRecord::fromJson(key, it.value().toObject()));
    }
    result.state = ReadResult::State::Ok;
    return result;
}

ReadResult readFile(const QString& path)
{
    ReadResult result;
    if (!QFileInfo::exists(path)) {
        return result;   // Missing: an empty inventory
    }
    QFile file(path);
    bool opened = false;
    for (int attempt = 0; attempt < kWriteAttempts && !opened; ++attempt) {
        if (attempt > 0) {
            QThread::msleep(kRetryPauseMs);   // a writer's rename in that very moment (Windows)
        }
        opened = file.open(QIODevice::ReadOnly);
    }
    if (!opened) {
        result.state = ReadResult::State::Unreadable;
        result.reason = QStringLiteral("cannot read it: %1").arg(file.errorString());
        return result;
    }
    const QByteArray bytes = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        result.state = ReadResult::State::Unreadable;
        result.reason = QStringLiteral("cannot read it: %1").arg(file.errorString());
        return result;
    }
    return parseDocument(bytes);
}

/// `<path>.corrupt-yyyyMMdd-HHmmss`, with "-2", "-3", ... when that name is taken.
QString asideName(const QString& path)
{
    const QString base = path + QStringLiteral(".corrupt-") + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    QString candidate = base;
    for (int n = 2; QFileInfo::exists(candidate); ++n) {
        candidate = base + QStringLiteral("-%1").arg(n);
    }
    return candidate;
}

QJsonArray stringArray(const QStringList& list)
{
    QJsonArray array;
    for (const QString& text : list) {
        array.append(text);
    }
    return array;
}

QStringList stringList(const QJsonValue& value)
{
    QStringList list;
    const QJsonArray array = value.toArray();
    for (const QJsonValue& entry : array) {
        const QString text = entry.toString().trimmed();
        if (!text.isEmpty() && !list.contains(text)) {
            list << text;
        }
    }
    return list;
}

} // namespace

// =======================================================================================
// InventoryRecord
// =======================================================================================

bool InventoryRecord::Image::operator==(const Image& other) const
{
    return file == other.file && chipTag == other.chipTag && md5 == other.md5 && gitDescribe == other.gitDescribe
           && partitions == other.partitions && written == other.written;
}

bool InventoryRecord::Operation::operator==(const Operation& other) const
{
    return name == other.name && result == other.result && message == other.message && time == other.time;
}

bool InventoryRecord::operator==(const InventoryRecord& other) const
{
    return serial == other.serial && profile == other.profile && port == other.port && layout == other.layout
           && gadgetSerialString == other.gadgetSerialString && lastImage == other.lastImage && mcuVersion == other.mcuVersion
           && mcuRole == other.mcuRole && mcuRead == other.mcuRead && lastOperation == other.lastOperation
           && firstSeen == other.firstSeen && lastSeen == other.lastSeen;
}

QString InventoryRecord::triText(Tri value)
{
    switch (value) {
    case Tri::Yes: return QStringLiteral("yes");
    case Tri::No: return QStringLiteral("no");
    case Tri::Unknown: break;
    }
    return QStringLiteral("unknown");
}

InventoryRecord::Tri InventoryRecord::triFromText(const QString& text)
{
    const QString clean = text.trimmed().toLower();
    if (clean == QLatin1String("yes")) {
        return Tri::Yes;
    }
    if (clean == QLatin1String("no")) {
        return Tri::No;
    }
    return Tri::Unknown;
}

QJsonObject InventoryRecord::toJson() const
{
    // "missing = never learned": a field the tool never learned is not written. A learned image,
    // MCU or operation is written whole (its fields "" when that part was not known).
    QJsonObject object;
    object.insert(kSerial, serial);
    if (!profile.isEmpty()) {
        object.insert(kProfile, profile);
    }
    if (!port.isEmpty()) {
        object.insert(kPort, port);
    }
    if (!layout.isEmpty()) {
        object.insert(kLayout, layout);
    }
    if (gadgetSerialString != Tri::Unknown) {
        object.insert(kGadget, triText(gadgetSerialString));
    }
    if (lastImage.isValid() || !lastImage.file.isEmpty()) {
        object.insert(kLastImage, QJsonObject{{kFile, lastImage.file},
                                              {kChipTag, lastImage.chipTag},
                                              {kMd5, lastImage.md5},
                                              {kGitDescribe, lastImage.gitDescribe},
                                              {kPartitions, stringArray(lastImage.partitions)},
                                              {kWritten, Inventory::isoTime(lastImage.written)}});
    }
    if (!mcuVersion.isEmpty() || !mcuRole.isEmpty() || mcuRead.isValid()) {
        object.insert(kMcu, QJsonObject{{kVersionKey, mcuVersion}, {kRole, mcuRole}, {kRead, Inventory::isoTime(mcuRead)}});
    }
    if (!lastOperation.name.isEmpty()) {
        object.insert(kLastOperation, QJsonObject{{kName, lastOperation.name},
                                                  {kResult, lastOperation.result},
                                                  {kMessage, lastOperation.message},
                                                  {kTime, Inventory::isoTime(lastOperation.time)}});
    }
    if (firstSeen.isValid()) {
        object.insert(kFirstSeen, Inventory::isoTime(firstSeen));
    }
    if (lastSeen.isValid()) {
        object.insert(kLastSeen, Inventory::isoTime(lastSeen));
    }
    return object;
}

InventoryRecord InventoryRecord::fromJson(const QString& key, const QJsonObject& object)
{
    InventoryRecord record;
    record.serial = key.trimmed();   // the key is the truth, whatever "serial" says
    record.profile = object.value(kProfile).toString();
    record.port = object.value(kPort).toString();
    record.layout = object.value(kLayout).toString();
    record.gadgetSerialString = triFromText(object.value(kGadget).toString());
    const QJsonObject image = object.value(kLastImage).toObject();
    record.lastImage.file = image.value(kFile).toString();
    record.lastImage.chipTag = image.value(kChipTag).toString();
    record.lastImage.md5 = image.value(kMd5).toString().trimmed().toLower();
    record.lastImage.gitDescribe = image.value(kGitDescribe).toString();
    record.lastImage.partitions = stringList(image.value(kPartitions));
    record.lastImage.written = Inventory::timeFromIso(image.value(kWritten).toString());
    const QJsonObject mcu = object.value(kMcu).toObject();
    record.mcuVersion = mcu.value(kVersionKey).toString();
    record.mcuRole = mcu.value(kRole).toString();
    record.mcuRead = Inventory::timeFromIso(mcu.value(kRead).toString());
    const QJsonObject operation = object.value(kLastOperation).toObject();
    record.lastOperation.name = operation.value(kName).toString();
    record.lastOperation.result = operation.value(kResult).toString();
    record.lastOperation.message = operation.value(kMessage).toString();
    record.lastOperation.time = Inventory::timeFromIso(operation.value(kTime).toString());
    record.firstSeen = Inventory::timeFromIso(object.value(kFirstSeen).toString());
    record.lastSeen = Inventory::timeFromIso(object.value(kLastSeen).toString());
    return record;
}

// =======================================================================================
// Inventory
// =======================================================================================

struct Inventory::Private
{
    QString path;
    QMap<QString, InventoryRecord> records;   ///< serial order (QMap's key order)
    QString movedAside;                       ///< the corrupt file's new name after the last update()
    QString lastError;
};

Inventory::Inventory(const QString& path)
    : d(std::make_unique<Private>())
{
    const QString clean = QDir::fromNativeSeparators(path.trimmed());
    d->path = clean.isEmpty() ? defaultFilePath() : QFileInfo(clean).absoluteFilePath();
}

Inventory::~Inventory() = default;

QString Inventory::defaultFilePath()
{
    return AppSettings::dataDirectory() + QLatin1Char('/') + QLatin1String(kFileName);
}

QString Inventory::filePath() const
{
    return d->path;
}

bool Inventory::load(QString* error)
{
    const ReadResult read = readFile(d->path);
    if (read.state == ReadResult::State::Corrupt || read.state == ReadResult::State::Unreadable) {
        d->records.clear();
        d->lastError = QStringLiteral("%1: %2").arg(nativePath(d->path), read.reason);
        if (error) {
            *error = d->lastError;
        }
        return false;
    }
    d->records = read.records;
    d->lastError.clear();
    if (error) {
        error->clear();
    }
    return true;
}

QList<InventoryRecord> Inventory::records() const
{
    return d->records.values();
}

std::optional<InventoryRecord> Inventory::record(const QString& serial) const
{
    const auto it = d->records.constFind(serial.trimmed());
    if (it == d->records.constEnd()) {
        return std::nullopt;
    }
    return it.value();
}

bool Inventory::update(const QString& serial, const Change& change, QString* error)
{
    d->movedAside.clear();
    const auto fail = [this, error](const QString& text) {
        d->lastError = text;
        if (error) {
            *error = text;
        }
        return false;
    };
    const QString key = serial.trimmed();
    if (key.isEmpty()) {
        return fail(QStringLiteral("no serial: a record is keyed by the board's ADB serial"));
    }
    const QString folder = QFileInfo(d->path).absolutePath();
    if (!QDir().mkpath(folder)) {
        return fail(QStringLiteral("cannot create the folder %1").arg(nativePath(folder)));
    }

    // One writer at a time, across processes: the tab and every su-fw on this PC.
    QLockFile lock(d->path + QStringLiteral(".lock"));
    lock.setStaleLockTime(kStaleLockMs);
    if (!lock.tryLock(kLockWaitMs)) {
        if (lock.error() == QLockFile::LockFailedError) {
            qint64 pid = 0;
            QString host;
            QString program;
            if (lock.getLockInfo(&pid, &host, &program) && pid > 0) {
                return fail(QStringLiteral("inventory is locked by %1 (pid %2) for more than %3 s")
                                .arg(program.isEmpty() ? QStringLiteral("another program") : program)
                                .arg(pid)
                                .arg(kLockWaitMs / 1000));
            }
            return fail(QStringLiteral("inventory is locked by another program for more than %1 s").arg(kLockWaitMs / 1000));
        }
        return fail(QStringLiteral("cannot lock %1 (%2)")
                        .arg(nativePath(d->path + QStringLiteral(".lock")),
                             lock.error() == QLockFile::PermissionError ? QStringLiteral("permission denied")
                                                                         : QStringLiteral("unknown error")));
    }

    // Re-read under the lock: another writer may have landed since this object last looked.
    ReadResult read = readFile(d->path);
    if (read.state == ReadResult::State::Unreadable) {
        return fail(QStringLiteral("%1: %2").arg(nativePath(d->path), read.reason));   // never overwritten blind
    }
    if (read.state == ReadResult::State::Corrupt) {
        // Never overwritten silently: its bytes are kept next to it for the owner.
        const QString aside = asideName(d->path);
        if (!QFile::rename(d->path, aside)) {
            return fail(QStringLiteral("%1 is unreadable (%2) and cannot be moved aside to %3")
                            .arg(nativePath(d->path), read.reason, nativePath(aside)));
        }
        d->movedAside = aside;
        qCWarning(lcFirmware).noquote() << "inventory:" << nativePath(d->path) << "was unreadable (" << read.reason << ") and moved to"
                                        << nativePath(aside);
        read.records.clear();
    }

    QMap<QString, InventoryRecord> fresh = read.records;
    const QDateTime now = nowToTheSecond();
    InventoryRecord entry = fresh.value(key);
    if (!entry.isValid()) {
        entry.serial = key;
        entry.firstSeen = now;
    }
    entry.lastSeen = now;   // before the change: a change may set it itself
    if (change) {
        change(entry);
    }
    entry.serial = key;
    fresh.insert(key, entry);

    QJsonObject boards;
    for (auto it = fresh.constBegin(); it != fresh.constEnd(); ++it) {
        boards.insert(it.key(), it.value().toJson());
    }
    const QJsonObject root{{kVersionKey, Inventory::kVersion},
                           {kUpdated, isoTime(now)},
                           {kBoards, boards}};
    const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Indented);

    QString writeError;
    bool written = false;
    for (int attempt = 0; attempt < kWriteAttempts && !written; ++attempt) {
        if (attempt > 0) {
            QThread::msleep(kRetryPauseMs);
        }
        QSaveFile file(d->path);
        if (!file.open(QIODevice::WriteOnly)) {
            writeError = file.errorString();
            continue;
        }
        if (file.write(bytes) != bytes.size()) {
            writeError = file.errorString();
            file.cancelWriting();
            continue;
        }
        written = file.commit();
        if (!written) {
            writeError = file.errorString();
        }
    }
    if (!written) {
        return fail(QStringLiteral("%1 (%2)").arg(writeError.isEmpty() ? QStringLiteral("cannot write the file") : writeError,
                                                  nativePath(d->path)));
    }

    // What a load() would give now: the records through the file's own form.
    d->records.clear();
    for (auto it = boards.constBegin(); it != boards.constEnd(); ++it) {
        d->records.insert(it.key(), InventoryRecord::fromJson(it.key(), it.value().toObject()));
    }
    d->lastError.clear();
    if (error) {
        error->clear();
    }
    return true;
}

QString Inventory::movedAsideFile() const
{
    return d->movedAside;
}

QStringList Inventory::tableLines(const QList<InventoryRecord>& list)
{
    QList<InventoryRecord> sorted = list;
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const InventoryRecord& a, const InventoryRecord& b) { return a.serial < b.serial; });
    QList<QStringList> rows;
    rows.append(QStringList{QStringLiteral("serial"), QStringLiteral("profile"), QStringLiteral("port"), QStringLiteral("image"),
                            QStringLiteral("tag"), QStringLiteral("md5"), QStringLiteral("written"), QStringLiteral("mcu"),
                            QStringLiteral("last operation"), QStringLiteral("result"), QStringLiteral("time"),
                            QStringLiteral("last seen")});
    for (const InventoryRecord& record : std::as_const(sorted)) {
        QStringList mcu;
        if (!record.mcuVersion.trimmed().isEmpty()) {
            mcu << record.mcuVersion.trimmed();
        }
        if (!record.mcuRole.trimmed().isEmpty()) {
            mcu << record.mcuRole.trimmed();
        }
        const bool imageKnown = record.lastImage.isValid() || !record.lastImage.file.isEmpty();
        const bool operationKnown = !record.lastOperation.name.isEmpty();
        rows.append(QStringList{cell(record.serial), cell(record.profile), cell(record.port), cell(record.lastImage.file),
                                cell(record.lastImage.chipTag), cell(record.lastImage.md5.left(8)),
                                imageKnown ? tableTime(record.lastImage.written) : QStringLiteral("-"), cell(mcu.join(QLatin1Char(' '))),
                                cell(record.lastOperation.name), operationKnown ? cell(record.lastOperation.result) : QStringLiteral("-"),
                                operationKnown ? tableTime(record.lastOperation.time) : QStringLiteral("-"), tableTime(record.lastSeen)});
    }
    return FleetReport::alignColumns(rows);
}

QJsonObject Inventory::json(const QList<InventoryRecord>& list, const QString& path)
{
    QList<InventoryRecord> sorted = list;
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const InventoryRecord& a, const InventoryRecord& b) { return a.serial < b.serial; });
    QJsonArray boards;
    for (const InventoryRecord& record : std::as_const(sorted)) {
        boards.append(record.toJson());
    }
    return QJsonObject{{QStringLiteral("file"), nativePath(path)}, {QStringLiteral("boards"), boards}};
}

QString Inventory::md5Hex(const QString& path, QString* error)
{
    QFile file(path);
    if (path.trimmed().isEmpty() || !file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = QStringLiteral("cannot read %1: %2")
                         .arg(nativePath(path), path.trimmed().isEmpty() ? QStringLiteral("no file") : file.errorString());
        }
        return QString();
    }
    QCryptographicHash hash(QCryptographicHash::Md5);
    for (;;) {
        const QByteArray chunk = file.read(kMd5ChunkBytes);
        if (chunk.isEmpty()) {
            break;
        }
        hash.addData(chunk);
    }
    if (file.error() != QFileDevice::NoError) {
        if (error) {
            *error = QStringLiteral("cannot read %1: %2").arg(nativePath(path), file.errorString());
        }
        return QString();
    }
    if (error) {
        error->clear();
    }
    return QString::fromLatin1(hash.result().toHex());
}

QString Inventory::isoTime(const QDateTime& time)
{
    if (!time.isValid()) {
        return QString();
    }
    const QDateTime local = time.toLocalTime();
    return local.toOffsetFromUtc(local.offsetFromUtc()).toString(Qt::ISODate);
}

QDateTime Inventory::timeFromIso(const QString& text)
{
    const QString clean = text.trimmed();
    if (clean.isEmpty()) {
        return QDateTime();
    }
    return QDateTime::fromString(clean, Qt::ISODate);
}
