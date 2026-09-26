#include "ssh/SshWorker.h"

#include "app/Logging.h"
#include "ssh/LibsshInclude.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QSaveFile>
#include <QTcpServer>
#include <QTcpSocket>

#include <fcntl.h>
#include <algorithm>
#include <mutex>
#include <utility>

namespace {

constexpr int kTickMs = 10;
constexpr int kReadChunk = 64 * 1024;
constexpr int kMaxReadPerTick = 1024 * 1024;
constexpr int kTransferChunk = 64 * 1024;
constexpr qint64 kTransferBytesPerTick = 1024 * 1024;
constexpr qint64 kTransferMsPerTick = 50;
constexpr qint64 kProgressIntervalMs = 100;
constexpr qint64 kAnswerTimeoutMs = 5 * 60 * 1000;
constexpr int kMaxAuthAttempts = 3;
constexpr int kMaxInteractiveRounds = 12;
constexpr int kReconnectFirstSeconds = 2;
constexpr int kReconnectMaxSeconds = 30;
constexpr int kKeepAliveMaxFailures = 3;
const char kKeepAlivePayload[] = "keepalive@buildai";

// The free helpers below translate through QCoreApplication::translate() with an explicit
// context: a context-less tr() in a free function is invisible to lupdate. Member functions of
// SshWorker use the class's own tr() ("SshWorker" context).

QString fromLibssh(const char* text)
{
    return text ? QString::fromUtf8(text) : QString();
}

int configuredLogVerbosity()
{
    bool ok = false;
    const int value = qEnvironmentVariableIntValue("SU_SSH_LOG_VERBOSITY", &ok);
    return ok ? std::clamp(value, 0, 4) : 0;
}

/// SU_SSH_IGNORE_CONFIG=1 skips ~/.ssh/config and the global ssh_config (the test suites set it
/// so a developer's or a CI image's Host blocks cannot change what they observe).
bool ignoreSshConfig()
{
    return qEnvironmentVariableIntValue("SU_SSH_IGNORE_CONFIG") > 0;
}

/// The listen address of a local forward: empty or "localhost" (any case) is the loopback
/// address, anything else must be a literal IPv4 / IPv6 address ("0.0.0.0" exposes the port).
/// Host names are refused: QHostAddress does not resolve them, and the null address it would
/// yield makes QTcpServer listen on every interface - the opposite of what "localhost" means.
bool parseBindAddress(const QString& text, QHostAddress* address)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty() || trimmed.compare(QLatin1String("localhost"), Qt::CaseInsensitive) == 0) {
        *address = QHostAddress(QHostAddress::LocalHost);
        return true;
    }
    const QHostAddress parsed(trimmed);
    if (parsed.isNull()) {
        return false;
    }
    *address = parsed;
    return true;
}

void libsshLogCallback(int priority, const char* function, const char* buffer, void*)
{
    qCDebug(lcSsh).noquote() << QStringLiteral("libssh[%1] %2: %3")
                                    .arg(priority)
                                    .arg(fromLibssh(function), fromLibssh(buffer));
}

void ensureLibsshInitialised()
{
    static std::once_flag flag;
    std::call_once(flag, [] {
        if (ssh_init() != SSH_OK) {
            qCWarning(lcSsh) << "ssh_init failed";
        }
        ssh_set_log_callback(libsshLogCallback);
        const int verbosity = configuredLogVerbosity();
        if (verbosity > 0) {
            ssh_set_log_level(verbosity);
        }
    });
}

/// ssh_pki_import_privkey_file callback: never answers, only records that the key is encrypted.
int passphraseNeededCallback(const char*, char*, size_t, int, int, void* userdata)
{
    if (userdata) {
        *static_cast<bool*>(userdata) = true;
    }
    return -1;
}

/// Exit status of the shell channel, delivered by libssh's channel callbacks as soon as the
/// request arrives (reading it afterwards may block or fail once the server disconnected).
struct ExitState
{
    bool received = false;
    int status = -1;
};

void exitStatusCallback(ssh_session, ssh_channel, int exitStatus, void* userdata)
{
    auto* state = static_cast<ExitState*>(userdata);
    if (state) {
        state->received = true;
        state->status = exitStatus;
    }
}

void exitSignalCallback(ssh_session, ssh_channel, const char* signal, int, const char*, const char*, void* userdata)
{
    auto* state = static_cast<ExitState*>(userdata);
    qCInfo(lcSsh) << "remote shell terminated by signal" << fromLibssh(signal);
    if (state && !state->received) {
        state->received = true;
        state->status = -1;
    }
}

QString expandHome(const QString& path)
{
    const QString trimmed = path.trimmed();
    if (trimmed == QLatin1String("~")) {
        return QDir::homePath();
    }
    if (trimmed.startsWith(QLatin1String("~/")) || trimmed.startsWith(QLatin1String("~\\"))) {
        return QDir::homePath() + trimmed.mid(1);
    }
    return trimmed;
}

/// The host token OpenSSH and libssh write to known_hosts: "host" for port 22, "[host]:port" otherwise.
QString knownHostsToken(const QString& host, quint16 port)
{
    const QString lower = host.trimmed().toLower();
    return port == 22 ? lower : QStringLiteral("[%1]:%2").arg(lower).arg(port);
}

/// Drop `token` from every non-hashed known_hosts line whose key type is `keyType` (the whole
/// line when no other host is listed on it). Hashed ("|1|") lines, comments, other hosts and other
/// key types are kept. Returns false when the file could not be rewritten.
bool removeKnownHostsEntries(const QString& file, const QString& token, const QString& keyType, int* removed)
{
    *removed = 0;
    QFile in(file);
    if (!in.exists()) {
        return true;
    }
    if (!in.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QByteArray content = in.readAll();
    in.close();

    QList<QByteArray> lines = content.split('\n');
    if (content.endsWith('\n') && !lines.isEmpty()) {
        lines.removeLast();
    }
    const QByteArray tokenUtf8 = token.toUtf8();
    const QByteArray typeUtf8 = keyType.toUtf8();
    QByteArray out;
    for (const QByteArray& raw : lines) {
        QByteArray line = raw;
        if (line.endsWith('\r')) {
            line.chop(1);
        }
        const QByteArray simplified = line.simplified();
        if (simplified.isEmpty() || simplified.startsWith('#') || simplified.startsWith("|1|")) {
            out += line + '\n';
            continue;
        }
        QList<QByteArray> fields = simplified.split(' ');
        const int hostIndex = fields.value(0).startsWith('@') ? 1 : 0;   // "@cert-authority" / "@revoked"
        if (fields.size() < hostIndex + 3 || fields.at(hostIndex + 1) != typeUtf8) {
            out += line + '\n';
            continue;
        }
        const QList<QByteArray> hosts = fields.at(hostIndex).split(',');
        QList<QByteArray> kept;
        for (const QByteArray& h : hosts) {
            if (h.toLower() != tokenUtf8) {
                kept.append(h);
            }
        }
        if (kept.size() == hosts.size()) {
            out += line + '\n';
            continue;
        }
        ++*removed;
        if (kept.isEmpty()) {
            continue;
        }
        fields[hostIndex] = kept.join(',');
        out += fields.join(' ') + '\n';
    }
    if (*removed == 0) {
        return true;
    }
    QSaveFile save(file);
    if (!save.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    save.write(out);
    return save.commit();
}

int modeFromPermissions(QFileDevice::Permissions permissions)
{
    int mode = 0;
    if (permissions & QFileDevice::ReadOwner) {
        mode |= 0400;
    }
    if (permissions & QFileDevice::WriteOwner) {
        mode |= 0200;
    }
    if (permissions & QFileDevice::ExeOwner) {
        mode |= 0100;
    }
    if (permissions & QFileDevice::ReadGroup) {
        mode |= 0040;
    }
    if (permissions & QFileDevice::WriteGroup) {
        mode |= 0020;
    }
    if (permissions & QFileDevice::ExeGroup) {
        mode |= 0010;
    }
    if (permissions & QFileDevice::ReadOther) {
        mode |= 0004;
    }
    if (permissions & QFileDevice::WriteOther) {
        mode |= 0002;
    }
    if (permissions & QFileDevice::ExeOther) {
        mode |= 0001;
    }
    return mode == 0 ? 0644 : mode;
}

QFileDevice::Permissions permissionsFromMode(unsigned mode)
{
    QFileDevice::Permissions permissions;
    if (mode & 0400) {
        permissions |= QFileDevice::ReadOwner | QFileDevice::ReadUser;
    }
    if (mode & 0200) {
        permissions |= QFileDevice::WriteOwner | QFileDevice::WriteUser;
    }
    if (mode & 0100) {
        permissions |= QFileDevice::ExeOwner | QFileDevice::ExeUser;
    }
    if (mode & 0040) {
        permissions |= QFileDevice::ReadGroup;
    }
    if (mode & 0020) {
        permissions |= QFileDevice::WriteGroup;
    }
    if (mode & 0010) {
        permissions |= QFileDevice::ExeGroup;
    }
    if (mode & 0004) {
        permissions |= QFileDevice::ReadOther;
    }
    if (mode & 0002) {
        permissions |= QFileDevice::WriteOther;
    }
    if (mode & 0001) {
        permissions |= QFileDevice::ExeOther;
    }
    return permissions;
}

QString sizeText(qint64 bytes)
{
    if (bytes < 0) {
        return QCoreApplication::translate("SshConnection", "unknown size");
    }
    if (bytes < 1024) {
        return QCoreApplication::translate("SshConnection", "%1 B").arg(bytes);
    }
    if (bytes < 1024 * 1024) {
        return QCoreApplication::translate("SshConnection", "%1 KB").arg(static_cast<double>(bytes) / 1024.0, 0, 'f', 1);
    }
    return QCoreApplication::translate("SshConnection", "%1 MB")
        .arg(static_cast<double>(bytes) / (1024.0 * 1024.0), 0, 'f', 1);
}

QString offeredMethodsText(int methods)
{
    QStringList names;
    if (methods & SSH_AUTH_METHOD_PUBLICKEY) {
        names << QStringLiteral("publickey");
    }
    if (methods & SSH_AUTH_METHOD_PASSWORD) {
        names << QStringLiteral("password");
    }
    if (methods & SSH_AUTH_METHOD_INTERACTIVE) {
        names << QStringLiteral("keyboard-interactive");
    }
    if (methods & SSH_AUTH_METHOD_HOSTBASED) {
        names << QStringLiteral("hostbased");
    }
    if (methods & SSH_AUTH_METHOD_GSSAPI_MIC) {
        names << QStringLiteral("gssapi-with-mic");
    }
    return names.isEmpty() ? QCoreApplication::translate("SshConnection", "none") : names.join(QLatin1String(", "));
}

} // namespace

// ---------------------------------------------------------------------------------------
// Private state
// ---------------------------------------------------------------------------------------

/// One accepted local connection bridged to a direct-tcpip channel. The bridge owns both. The
/// socket (a child of the listener's QTcpServer) is released by closeBridge() and nowhere else:
/// no deleteLater() is ever armed on it while a Bridge holds the pointer, so pumpForwards() and
/// teardownForwards() can dereference it safely after the local client closed its side first.
struct SshWorker::Bridge
{
    QTcpSocket* socket = nullptr;
    ssh_channel channel = nullptr;
};

struct SshWorker::ForwardListener
{
    SshLocalForward spec;
    QTcpServer* server = nullptr;
    QList<Bridge> bridges;
};

struct SshWorker::Transfer
{
    SshConnection::TransferRequest request;
    sftp_file remote = nullptr;
    QFile local;
    QString partPath;
    qint64 done = 0;
    qint64 total = -1;
    unsigned remoteMode = 0;
    bool haveRemoteMode = false;
    QElapsedTimer progressTimer;
};

struct SshWorker::Live
{
    ssh_session session = nullptr;
    ssh_channel channel = nullptr;
    sftp_session sftp = nullptr;
    struct ssh_channel_callbacks_struct channelCallbacks {};
    bool callbacksSet = false;
    bool ptyOpen = false;
    bool shellOpen = false;
    ExitState exit;
    QString user;                   ///< user name the session ended up with (profile, ssh config or local)
    QString configIdentity;         ///< first IdentityFile from the user's ssh config (empty = none)
    QString knownHostsFile;
    QList<ForwardListener*> forwards;
    std::unique_ptr<Transfer> transfer;
    bool cancelTransfer = false;
};

struct SshWorker::Pending
{
    QMutex mutex;
    QWaitCondition condition;
    bool active = false;
    bool answered = false;
    bool cancelled = false;
    bool accept = false;
    bool remember = false;
    QString response;
};

// ---------------------------------------------------------------------------------------
// Construction / thread-safe entry points
// ---------------------------------------------------------------------------------------

SshWorker::SshWorker(std::shared_ptr<SshSharedState> shared, QObject* parent)
    : QObject(parent)
    , m_shared(std::move(shared))
    , m_pending(std::make_unique<Pending>())
{
    m_readBuffer.resize(kReadChunk);

    m_tickTimer = new QTimer(this);
    m_tickTimer->setInterval(kTickMs);
    m_tickTimer->setTimerType(Qt::PreciseTimer);
    connect(m_tickTimer, &QTimer::timeout, this, &SshWorker::tick);

    m_keepAliveTimer = new QTimer(this);
    connect(m_keepAliveTimer, &QTimer::timeout, this, &SshWorker::sendKeepAlive);

    m_reconnectTimer = new QTimer(this);
    m_reconnectTimer->setSingleShot(true);
    connect(m_reconnectTimer, &QTimer::timeout, this, &SshWorker::onReconnectTimer);
}

SshWorker::~SshWorker()
{
    // Only reached after the worker thread stopped (SshConnection runs close() on it first and
    // joins it), so nothing is live any more; free defensively without emitting.
    if (m_live) {
        blockSignals(true);
        teardownLive();
    }
}

void SshWorker::requestAbort(quint64 generation)
{
    // Only the GUI thread calls this, so a plain max() store is race-free.
    if (m_abortGeneration.load() < generation) {
        m_abortGeneration.store(generation);
    }
    QMutexLocker lock(&m_pending->mutex);
    m_pending->condition.wakeAll();
}

void SshWorker::deliverHostKeyAnswer(bool accept, bool remember)
{
    QMutexLocker lock(&m_pending->mutex);
    if (!m_pending->active || m_pending->answered) {
        return;
    }
    m_pending->answered = true;
    m_pending->accept = accept;
    m_pending->remember = remember;
    m_pending->condition.wakeAll();
}

void SshWorker::deliverPromptAnswer(const QString& response, bool remember)
{
    QMutexLocker lock(&m_pending->mutex);
    if (!m_pending->active || m_pending->answered) {
        return;
    }
    m_pending->answered = true;
    m_pending->response = response;
    m_pending->remember = remember;
    m_pending->condition.wakeAll();
}

void SshWorker::cancelPending()
{
    QMutexLocker lock(&m_pending->mutex);
    if (!m_pending->active) {
        return;
    }
    m_pending->cancelled = true;
    m_pending->condition.wakeAll();
}

void SshWorker::enqueueOutgoing(const QByteArray& data)
{
    if (data.isEmpty()) {
        return;
    }
    QMutexLocker lock(&m_outgoingMutex);
    m_outgoing.append(data);
    m_outgoingBytes += data.size();
}

qint64 SshWorker::pendingOutgoingBytes() const
{
    QMutexLocker lock(&m_outgoingMutex);
    return m_outgoingBytes;
}

bool SshWorker::aborted() const
{
    return m_generation != 0 && m_abortGeneration.load() >= m_generation;
}

// ---------------------------------------------------------------------------------------
// Slots (worker thread)
// ---------------------------------------------------------------------------------------

void SshWorker::open(const SshWorker::OpenRequest& request)
{
    if (m_live) {
        qCWarning(lcSsh) << "open() while a session is live - closing it first";
        stopTimers();
        teardownLive();
    }
    m_request = request;
    m_generation = request.generation;
    m_password = request.savedPassword;
    m_passphrase = request.savedPassphrase;
    m_acceptedKeyLine.clear();
    m_cols = request.cols > 0 ? request.cols : 80;
    m_rows = request.rows > 0 ? request.rows : 24;
    m_reconnectDelaySeconds = 0;
    m_keepAliveFailures = 0;
    {
        QMutexLocker lock(&m_outgoingMutex);
        m_outgoing.clear();
        m_outgoingBytes = 0;
    }
    if (aborted()) {
        emit stateChanged(m_generation, Transport::State::Disconnected);
        return;
    }
    runConnect(false);
}

void SshWorker::close(quint64 generation)
{
    if (generation != m_generation) {
        qCDebug(lcSsh) << "close for generation" << generation << "ignored (current" << m_generation << ")";
        return;
    }
    stopTimers();
    if (m_live) {
        qCInfo(lcSsh) << "closing" << target();
        teardownLive();
    }
    emit stateChanged(generation, Transport::State::Disconnected);
}

void SshWorker::beginReconnect(quint64 generation)
{
    if (generation != m_generation || aborted() || m_live) {
        return;
    }
    m_reconnectDelaySeconds = 0;
    scheduleReconnect();
}

void SshWorker::flushOutgoing()
{
    tick();
}

void SshWorker::resize(int cols, int rows)
{
    if (cols <= 0 || rows <= 0) {
        return;
    }
    m_cols = cols;
    m_rows = rows;
    if (m_live && m_live->ptyOpen && m_live->channel) {
        if (ssh_channel_change_pty_size(m_live->channel, cols, rows) != SSH_OK) {
            qCWarning(lcSsh) << "window change request failed:" << libsshError();
        }
    }
}

void SshWorker::startTransfer(const SshConnection::TransferRequest& request)
{
    const quint64 gen = m_generation;
    auto fail = [this, gen](const QString& message) {
        {
            QMutexLocker lock(&m_shared->mutex);
            m_shared->transfer.active = false;
        }
        qCWarning(lcSsh) << "transfer not started:" << message;
        emit transferFinished(gen, false, message);
    };
    if (!m_live || !m_live->shellOpen) {
        fail(tr("Not connected"));
        return;
    }
    if (m_live->transfer) {
        fail(tr("Another transfer is still running"));
        return;
    }
    QString error;
    if (!ensureSftp(&error)) {
        fail(error);
        return;
    }

    auto transfer = std::make_unique<Transfer>();
    transfer->request = request;
    const QByteArray remotePath = request.remotePath.toUtf8();
    if (request.direction == SshConnection::TransferDirection::Upload) {
        const QFileInfo info(request.localPath);
        if (!info.isFile()) {
            fail(tr("Local file not found: %1").arg(QDir::toNativeSeparators(request.localPath)));
            return;
        }
        transfer->total = info.size();
        if (!request.overwrite) {
            sftp_attributes attributes = sftp_stat(m_live->sftp, remotePath.constData());
            if (attributes) {
                sftp_attributes_free(attributes);
                fail(tr("Remote file already exists: %1").arg(request.remotePath));
                return;
            }
        }
        const int mode = request.preservePermissions ? modeFromPermissions(QFile::permissions(request.localPath)) : 0644;
        transfer->local.setFileName(request.localPath);
        if (!transfer->local.open(QIODevice::ReadOnly)) {
            fail(tr("Cannot read %1: %2").arg(QDir::toNativeSeparators(request.localPath), transfer->local.errorString()));
            return;
        }
        transfer->remote = sftp_open(m_live->sftp, remotePath.constData(), O_WRONLY | O_CREAT | O_TRUNC, mode);
        if (!transfer->remote) {
            fail(tr("Cannot create remote file %1: %2").arg(request.remotePath, sftpError()));
            return;
        }
    } else {
        if (!request.overwrite && QFileInfo::exists(request.localPath)) {
            fail(tr("Local file already exists: %1").arg(QDir::toNativeSeparators(request.localPath)));
            return;
        }
        sftp_attributes attributes = sftp_stat(m_live->sftp, remotePath.constData());
        if (!attributes) {
            fail(tr("Remote file not found: %1 (%2)").arg(request.remotePath, sftpError()));
            return;
        }
        if (attributes->flags & SSH_FILEXFER_ATTR_SIZE) {
            transfer->total = static_cast<qint64>(attributes->size);
        }
        if (attributes->flags & SSH_FILEXFER_ATTR_PERMISSIONS) {
            transfer->remoteMode = attributes->permissions;
            transfer->haveRemoteMode = true;
        }
        sftp_attributes_free(attributes);
        transfer->remote = sftp_open(m_live->sftp, remotePath.constData(), O_RDONLY, 0);
        if (!transfer->remote) {
            fail(tr("Cannot open remote file %1: %2").arg(request.remotePath, sftpError()));
            return;
        }
        transfer->partPath = request.localPath + QStringLiteral(".part");
        transfer->local.setFileName(transfer->partPath);
        if (!transfer->local.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            const QString message = transfer->local.errorString();
            sftp_close(transfer->remote);
            transfer->remote = nullptr;
            fail(tr("Cannot write %1: %2").arg(QDir::toNativeSeparators(transfer->partPath), message));
            return;
        }
    }
    transfer->progressTimer.start();
    m_live->transfer = std::move(transfer);
    m_live->cancelTransfer = false;
    publishTransfer();
    qCInfo(lcSsh) << (request.direction == SshConnection::TransferDirection::Upload ? "uploading" : "downloading")
                  << request.localPath << "<->" << request.remotePath << "(" << m_live->transfer->total << "bytes )";
    emit transferStarted(gen, request);
    emit transferProgress(gen, 0, m_live->transfer->total);
}

void SshWorker::cancelTransfer()
{
    if (!m_live || !m_live->transfer) {
        return;
    }
    m_live->cancelTransfer = true;
    if (!m_inTick) {
        finishTransfer(false, tr("Transfer cancelled"));
    }
}

void SshWorker::requestRemoteHome()
{
    const quint64 gen = m_generation;
    if (!m_live || !m_live->shellOpen) {
        emit errorOccurred(gen, tr("Not connected"));
        emit remoteHomeReceived(gen, QString());
        return;
    }
    QString error;
    if (!ensureSftp(&error)) {
        emit errorOccurred(gen, error);
        emit remoteHomeReceived(gen, QString());
        return;
    }
    char* path = sftp_canonicalize_path(m_live->sftp, ".");
    if (!path) {
        emit errorOccurred(gen, tr("Cannot resolve the remote home directory: %1").arg(sftpError()));
        emit remoteHomeReceived(gen, QString());
        return;
    }
    const QString home = QString::fromUtf8(path);
    ssh_string_free_char(path);
    {
        QMutexLocker lock(&m_shared->mutex);
        m_shared->remoteHome = home;
    }
    emit remoteHomeReceived(gen, home);
}

// ---------------------------------------------------------------------------------------
// Connect sequence
// ---------------------------------------------------------------------------------------

void SshWorker::runConnect(bool reconnect)
{
    const quint64 gen = m_generation;
    QString error;
    const Step step = connectSequence(reconnect, &error);
    if (step == Step::Ok) {
        m_reconnectDelaySeconds = 0;
        m_keepAliveFailures = 0;
        qCInfo(lcSsh) << (reconnect ? "reconnected to" : "connected to") << target();
        emit stateChanged(gen, Transport::State::Connected);
        if (reconnect) {
            emit connectionRestored(gen, displayName());
        }
        m_tickTimer->start();
        const int keepAlive = m_request.profile.keepAliveSeconds;
        if (keepAlive > 0) {
            m_keepAliveTimer->start(keepAlive * 1000);
        }
        setupForwards();
        return;
    }

    teardownLive();
    switch (step) {
    case Step::Aborted:
        qCInfo(lcSsh) << "connect to" << target() << "aborted";
        emit stateChanged(gen, Transport::State::Disconnected);
        return;
    case Step::NetworkFailed:
        if (reconnect && !aborted()) {
            qCWarning(lcSsh) << "reconnect attempt failed:" << error;
            scheduleReconnect();
            return;
        }
        break;
    case Step::Ok:
    case Step::Denied:
    case Step::Cancelled:
    case Step::Failed:
    case Step::NeedsInteraction:
        break;
    }
    qCWarning(lcSsh) << "connect to" << target() << "failed:" << error;
    emit errorOccurred(gen, error);
    emit stateChanged(gen, Transport::State::Disconnected);
}

SshWorker::Step SshWorker::connectSequence(bool reconnect, QString* error)
{
    ensureLibsshInitialised();
    m_live = std::make_unique<Live>();
    m_live->session = ssh_new();
    if (!m_live->session) {
        *error = tr("Cannot create an SSH session");
        return Step::Failed;
    }
    if (!applySessionOptions(error)) {
        return Step::Failed;
    }
    if (aborted()) {
        return Step::Aborted;
    }

    qCInfo(lcSsh) << (reconnect ? "reconnecting to" : "connecting to") << target();
    if (ssh_connect(m_live->session) != SSH_OK) {
        *error = tr("Cannot connect to %1: %2").arg(target(), libsshError());
        return aborted() ? Step::Aborted : Step::NetworkFailed;
    }
    if (aborted()) {
        return Step::Aborted;
    }

    {
        QMutexLocker lock(&m_shared->mutex);
        m_shared->serverVersion = fromLibssh(ssh_get_serverbanner(m_live->session));
    }
    char* user = nullptr;
    if (ssh_options_get(m_live->session, SSH_OPTIONS_USER, &user) == SSH_OK && user) {
        m_live->user = QString::fromUtf8(user);
        ssh_string_free_char(user);
    }
    if (m_live->user.isEmpty()) {
        m_live->user = m_request.profile.user.isEmpty() ? SshConnection::localUserName() : m_request.profile.user;
    }
    qCDebug(lcSsh) << "server" << fromLibssh(ssh_get_serverbanner(m_live->session)) << "user" << m_live->user;

    Step step = verifyHostKey(reconnect, error);
    if (step != Step::Ok) {
        return step;
    }
    step = authenticate(reconnect, error);
    if (step != Step::Ok) {
        return step;
    }
    if (aborted()) {
        return Step::Aborted;
    }

    char* banner = ssh_get_issue_banner(m_live->session);
    if (banner) {
        const QString text = QString::fromUtf8(banner);
        ssh_string_free_char(banner);
        if (!text.trimmed().isEmpty()) {
            emit bannerReceived(m_generation, text);
        }
    }
    QString method;
    {
        QMutexLocker lock(&m_shared->mutex);
        method = m_shared->authMethod;
    }
    emit authenticated(m_generation, method);

    return openShellChannel(error);
}

bool SshWorker::applySessionOptions(QString* error)
{
    const SshProfile& profile = m_request.profile;
    ssh_session session = m_live->session;
    auto fail = [this, error](const char* what) {
        *error = tr("Cannot set SSH option %1: %2").arg(QLatin1String(what), libsshError());
        return false;
    };

    const QByteArray host = profile.host.trimmed().toUtf8();
    if (ssh_options_set(session, SSH_OPTIONS_HOST, host.constData()) < 0) {
        return fail("host");
    }
    // The user's ~/.ssh/config first (Host aliases, HostName, User, Port, IdentityFile,
    // ProxyJump), then the explicit profile fields on top so they win. Parsing here marks the
    // config as processed, so ssh_connect() does not apply it a second time; with the parse
    // skipped (SU_SSH_IGNORE_CONFIG) the flag below keeps ssh_connect() from reading it either.
    if (ignoreSshConfig()) {
        qCDebug(lcSsh) << "SU_SSH_IGNORE_CONFIG is set: the ssh client configuration is not read";
    } else if (ssh_options_parse_config(session, nullptr) < 0) {
        qCWarning(lcSsh) << "cannot parse the ssh client configuration:" << libsshError();
    }
    bool processConfig = false;
    ssh_options_set(session, SSH_OPTIONS_PROCESS_CONFIG, &processConfig);

    // An IdentityFile from the config sits at the head of libssh's identity list (libssh's own
    // defaults are "%d/id_*" templates and are covered by defaultIdentityFiles()).
    char* configIdentity = nullptr;
    if (ssh_options_get(session, SSH_OPTIONS_IDENTITY, &configIdentity) == SSH_OK && configIdentity) {
        QString path = QString::fromUtf8(configIdentity);
        ssh_string_free_char(configIdentity);
        if (!path.startsWith(QLatin1String("%d/"))) {
            path.replace(QLatin1String("%d"), QDir::homePath() + QStringLiteral("/.ssh"));
            m_live->configIdentity = expandHome(path);
            qCDebug(lcSsh) << "identity from ssh config:" << m_live->configIdentity;
        }
    }

    if (profile.port != 22) {
        int port = profile.port;
        if (ssh_options_set(session, SSH_OPTIONS_PORT, &port) < 0) {
            return fail("port");
        }
    }
    if (!profile.user.trimmed().isEmpty()) {
        const QByteArray user = profile.user.trimmed().toUtf8();
        if (ssh_options_set(session, SSH_OPTIONS_USER, user.constData()) < 0) {
            return fail("user");
        }
    }
    long timeout = std::clamp(profile.connectTimeoutSeconds, 1, 600);
    if (ssh_options_set(session, SSH_OPTIONS_TIMEOUT, &timeout) < 0) {
        return fail("timeout");
    }

    QString knownHosts = expandHome(profile.knownHostsFile);
    if (knownHosts.isEmpty()) {
        knownHosts = SshConnection::defaultKnownHostsFile();
    }
    const QFileInfo knownHostsInfo(knownHosts);
    const QDir knownHostsDir = knownHostsInfo.absoluteDir();
    if (!knownHostsDir.exists()) {
        if (QDir().mkpath(knownHostsDir.absolutePath())) {
#ifndef Q_OS_WIN
            QFile::setPermissions(knownHostsDir.absolutePath(),
                                  QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
#endif
        } else {
            qCWarning(lcSsh) << "cannot create" << knownHostsDir.absolutePath();
        }
    }
    m_live->knownHostsFile = knownHostsInfo.absoluteFilePath();
    const QByteArray knownHostsUtf8 = m_live->knownHostsFile.toUtf8();
    if (ssh_options_set(session, SSH_OPTIONS_KNOWNHOSTS, knownHostsUtf8.constData()) < 0) {
        return fail("known_hosts");
    }

    if (ssh_options_set(session, SSH_OPTIONS_COMPRESSION, profile.compression ? "yes" : "no") < 0) {
        return fail("compression");
    }
    if (!profile.proxyJump.trimmed().isEmpty()) {
        const QByteArray jump = profile.proxyJump.trimmed().toUtf8();
        if (ssh_options_set(session, SSH_OPTIONS_PROXYJUMP, jump.constData()) < 0) {
            return fail("proxy jump");
        }
    }
    if (!profile.identityFile.trimmed().isEmpty()) {
        const QByteArray identity = expandHome(profile.identityFile).toUtf8();
        if (ssh_options_set(session, SSH_OPTIONS_IDENTITY, identity.constData()) < 0) {
            return fail("identity");
        }
    }
    int verbosity = configuredLogVerbosity();
    if (verbosity > 0) {
        ssh_options_set(session, SSH_OPTIONS_LOG_VERBOSITY, &verbosity);
    }
    return true;
}

SshWorker::Step SshWorker::verifyHostKey(bool reconnect, QString* error)
{
    ssh_session session = m_live->session;
    SshConnection::HostKeyInfo info;
    info.host = m_request.profile.host.trimmed();
    info.port = m_request.profile.port;
    info.knownHostsFile = m_live->knownHostsFile;

    ssh_key key = nullptr;
    if (ssh_get_server_publickey(session, &key) != SSH_OK || !key) {
        *error = tr("Cannot read the host key of %1: %2").arg(target(), libsshError());
        return ssh_is_connected(session) ? Step::Failed : Step::NetworkFailed;
    }
    info.keyType = fromLibssh(ssh_key_type_to_char(ssh_key_type(key)));
    unsigned char* hash = nullptr;
    size_t hashLength = 0;
    if (ssh_get_publickey_hash(key, SSH_PUBLICKEY_HASH_SHA256, &hash, &hashLength) == SSH_OK) {
        char* fingerprint = ssh_get_fingerprint_hash(SSH_PUBLICKEY_HASH_SHA256, hash, hashLength);
        info.fingerprintSha256 = fromLibssh(fingerprint);
        ssh_string_free_char(fingerprint);
        ssh_clean_pubkey_hash(&hash);
    }
    if (ssh_get_publickey_hash(key, SSH_PUBLICKEY_HASH_MD5, &hash, &hashLength) == SSH_OK) {
        char* hexa = ssh_get_hexa(hash, hashLength);
        info.fingerprintMd5 = QStringLiteral("MD5:") + fromLibssh(hexa);
        ssh_string_free_char(hexa);
        ssh_clean_pubkey_hash(&hash);
    }
    char* base64 = nullptr;
    if (ssh_pki_export_pubkey_base64(key, &base64) == SSH_OK && base64) {
        info.publicKeyLine = info.keyType + QLatin1Char(' ') + QString::fromLatin1(base64);
        ssh_string_free_char(base64);
    }
    ssh_key_free(key);

    switch (ssh_session_is_known_server(session)) {
    case SSH_KNOWN_HOSTS_OK:
        info.status = SshConnection::HostKeyStatus::Known;
        break;
    case SSH_KNOWN_HOSTS_CHANGED:
        info.status = SshConnection::HostKeyStatus::Changed;
        break;
    case SSH_KNOWN_HOSTS_OTHER:
        info.status = SshConnection::HostKeyStatus::KeyTypeChanged;
        break;
    case SSH_KNOWN_HOSTS_NOT_FOUND:
    case SSH_KNOWN_HOSTS_UNKNOWN:
        info.status = SshConnection::HostKeyStatus::Unknown;
        break;
    case SSH_KNOWN_HOSTS_ERROR:
        info.status = SshConnection::HostKeyStatus::Error;
        info.message = libsshError();
        break;
    }
    {
        QMutexLocker lock(&m_shared->mutex);
        m_shared->hostKeyType = info.keyType;
        m_shared->hostKeyFingerprint = info.fingerprintSha256;
    }
    qCInfo(lcSsh) << "host key of" << target() << info.keyType << info.fingerprintSha256 << "status"
                  << static_cast<int>(info.status);
    if (info.status == SshConnection::HostKeyStatus::Known) {
        return Step::Ok;
    }
    if (!m_acceptedKeyLine.isEmpty() && m_acceptedKeyLine == info.publicKeyLine) {
        qCInfo(lcSsh) << "host key was accepted earlier in this session";
        return Step::Ok;
    }
    if (reconnect) {
        *error = tr("The host key of %1 is not known (%2) - reconnect cancelled").arg(target(), info.fingerprintSha256);
        return Step::NeedsInteraction;
    }

    bool accept = false;
    bool remember = false;
    const Step step = askHostKey(info, &accept, &remember, error);
    if (step != Step::Ok) {
        return step;
    }
    if (!accept) {
        *error = tr("Host key rejected for %1").arg(target());
        return Step::Cancelled;
    }
    m_acceptedKeyLine = info.publicKeyLine;
    if (remember) {
        if (info.status == SshConnection::HostKeyStatus::Changed) {
            int removed = 0;
            const QString token = knownHostsToken(info.host, info.port);
            if (!removeKnownHostsEntries(m_live->knownHostsFile, token, info.keyType, &removed)) {
                qCWarning(lcSsh) << "cannot rewrite" << m_live->knownHostsFile;
            } else {
                qCInfo(lcSsh) << "removed" << removed << "old" << info.keyType << "entries for" << token;
            }
        }
        if (ssh_session_update_known_hosts(session) != SSH_OK) {
            qCWarning(lcSsh) << "cannot update" << m_live->knownHostsFile << ":" << libsshError();
        } else {
            qCInfo(lcSsh) << "host key of" << target() << "stored in" << m_live->knownHostsFile;
        }
    }
    return Step::Ok;
}

SshWorker::Step SshWorker::methodError(QString* error)
{
    *error = libsshError();
    if (!ssh_is_connected(m_live->session)) {
        return Step::NetworkFailed;
    }
    qCWarning(lcSsh) << "authentication method failed:" << *error;
    return Step::Denied;
}

void SshWorker::setAuthMethod(const QString& method)
{
    QMutexLocker lock(&m_shared->mutex);
    m_shared->authMethod = method;
}

SshConnection::AuthPrompt SshWorker::makePrompt(SshConnection::PromptKind kind, int attempt) const
{
    SshConnection::AuthPrompt prompt;
    prompt.kind = kind;
    prompt.attempt = attempt;
    prompt.user = m_live ? m_live->user : m_request.profile.user;
    prompt.host = m_request.profile.host.trimmed();
    prompt.canRemember = !m_request.profile.id.isEmpty();
    return prompt;
}

SshWorker::Step SshWorker::authenticate(bool reconnect, QString* error)
{
    ssh_session session = m_live->session;
    const SshProfile& profile = m_request.profile;

    int rc = ssh_userauth_none(session, nullptr);
    if (rc == SSH_AUTH_ERROR) {
        *error = tr("Authentication with %1 failed: %2").arg(target(), libsshError());
        return ssh_is_connected(session) ? Step::Failed : Step::NetworkFailed;
    }
    if (rc == SSH_AUTH_SUCCESS) {
        setAuthMethod(QStringLiteral("none"));
        return Step::Ok;
    }
    const int methods = ssh_userauth_list(session, nullptr);
    const bool offersPublicKey = (methods & SSH_AUTH_METHOD_PUBLICKEY) != 0;
    const bool offersPassword = (methods & SSH_AUTH_METHOD_PASSWORD) != 0;
    const bool offersInteractive = (methods & SSH_AUTH_METHOD_INTERACTIVE) != 0;
    qCDebug(lcSsh) << "server offers" << offeredMethodsText(methods) << "- profile auth" << SshProfile::authKey(profile.auth);

    enum class Method { Agent, Key, Password, Interactive };
    struct Attempt
    {
        Method method;
        QString keyFile;
    };
    QList<Attempt> plan;
    const QString identity = expandHome(profile.identityFile);
    auto addKeys = [&](bool includeDefaults) {
        if (!offersPublicKey) {
            return;
        }
        if (!identity.isEmpty()) {
            plan.append({Method::Key, identity});
        }
        const QString configIdentity = m_live->configIdentity;
        if (includeDefaults && !configIdentity.isEmpty() && configIdentity != identity) {
            plan.append({Method::Key, configIdentity});
        }
        if (includeDefaults) {
            const QStringList defaults = SshConnection::defaultIdentityFiles();
            for (const QString& file : defaults) {
                if (file != identity && file != configIdentity) {
                    plan.append({Method::Key, file});
                }
            }
        }
    };
    switch (profile.auth) {
    case SshProfile::Auth::Auto:
        if (offersPublicKey && SshConnection::agentAvailable()) {
            plan.append({Method::Agent, QString()});
        }
        addKeys(true);
        if (offersPassword) {
            plan.append({Method::Password, QString()});
        }
        if (offersInteractive) {
            plan.append({Method::Interactive, QString()});
        }
        break;
    case SshProfile::Auth::PublicKey:
        addKeys(identity.isEmpty());
        break;
    case SshProfile::Auth::Password:
        if (offersPassword) {
            plan.append({Method::Password, QString()});
        } else if (offersInteractive) {
            // Servers with PAM often offer only keyboard-interactive for what users call "password".
            plan.append({Method::Interactive, QString()});
        }
        break;
    case SshProfile::Auth::KeyboardInteractive:
        if (offersInteractive) {
            plan.append({Method::Interactive, QString()});
        }
        break;
    case SshProfile::Auth::Agent:
        if (offersPublicKey && SshConnection::agentAvailable()) {
            plan.append({Method::Agent, QString()});
        }
        break;
    }
    if (plan.isEmpty()) {
        *error = tr("No usable authentication method for %1: the server offers %2, the profile uses %3")
                     .arg(target(), offeredMethodsText(methods), SshProfile::authText(profile.auth));
        return Step::Failed;
    }

    QString lastError;
    for (const Attempt& attempt : plan) {
        if (aborted()) {
            return Step::Aborted;
        }
        Step step = Step::Denied;
        QString stepError;
        switch (attempt.method) {
        case Method::Agent:
            step = authenticateAgent();
            break;
        case Method::Key:
            step = authenticateWithKey(attempt.keyFile, reconnect, &stepError);
            break;
        case Method::Password:
            step = authenticateWithPassword(reconnect, &stepError);
            break;
        case Method::Interactive:
            step = authenticateInteractive(reconnect, &stepError);
            break;
        }
        if (step == Step::Ok) {
            return Step::Ok;
        }
        if (step == Step::Denied) {
            if (!stepError.isEmpty()) {
                lastError = stepError;
            }
            continue;
        }
        *error = stepError;
        return step;
    }
    *error = tr("Authentication failed for %1: %2")
                 .arg(target(), lastError.isEmpty() ? tr("every method was rejected") : lastError);
    return Step::Failed;
}

SshWorker::Step SshWorker::authenticateAgent()
{
    const int rc = ssh_userauth_agent(m_live->session, nullptr);
    if (rc == SSH_AUTH_SUCCESS) {
        setAuthMethod(QStringLiteral("agent"));
        return Step::Ok;
    }
    if (rc == SSH_AUTH_ERROR && !ssh_is_connected(m_live->session)) {
        return Step::NetworkFailed;
    }
    qCDebug(lcSsh) << "agent authentication did not succeed:" << libsshError();
    return Step::Denied;
}

SshWorker::Step SshWorker::authenticateWithKey(const QString& keyFile, bool reconnect, QString* error)
{
    if (!QFileInfo::exists(keyFile)) {
        qCDebug(lcSsh) << "identity file does not exist:" << keyFile;
        return Step::Denied;
    }
    const bool profileKey = (keyFile == expandHome(m_request.profile.identityFile));
    const QByteArray fileUtf8 = keyFile.toUtf8();

    QString passphrase = profileKey ? m_passphrase : QString();
    bool fromStore = !passphrase.isEmpty() && passphrase == m_request.savedPassphrase;
    bool remember = false;
    int attempt = 0;
    ssh_key privateKey = nullptr;
    for (;;) {
        bool needsPassphrase = false;
        const QByteArray pass = passphrase.toUtf8();
        const int rc = ssh_pki_import_privkey_file(fileUtf8.constData(), pass.isEmpty() ? nullptr : pass.constData(),
                                                   passphraseNeededCallback, &needsPassphrase, &privateKey);
        if (rc == SSH_OK && privateKey) {
            break;
        }
        privateKey = nullptr;
        if (rc == SSH_EOF) {
            qCWarning(lcSsh) << "cannot read identity file" << keyFile;
            return Step::Denied;
        }
        if (!pass.isEmpty()) {
            // A passphrase was supplied and rejected.
            qCInfo(lcSsh) << "passphrase for" << keyFile << "rejected";
            if (fromStore) {
                emit secretRejected(m_generation, passphraseSecretKey());
                m_passphrase.clear();
                fromStore = false;
            }
            ++attempt;
        } else if (!needsPassphrase) {
            qCWarning(lcSsh) << "cannot import identity file" << keyFile << ":" << libsshError();
            *error = tr("Cannot read the key %1").arg(QDir::toNativeSeparators(keyFile));
            return Step::Denied;
        }
        if (attempt >= kMaxAuthAttempts) {
            *error = tr("Wrong passphrase for %1").arg(QDir::toNativeSeparators(keyFile));
            return Step::Denied;
        }
        if (reconnect) {
            *error = tr("The key %1 needs a passphrase - reconnect cancelled").arg(QDir::toNativeSeparators(keyFile));
            return Step::NeedsInteraction;
        }
        SshConnection::AuthPrompt prompt = makePrompt(SshConnection::PromptKind::Passphrase, attempt + 1);
        prompt.title = tr("Key passphrase");
        prompt.prompt = tr("Enter passphrase for key '%1':").arg(QDir::toNativeSeparators(keyFile));
        prompt.keyFile = keyFile;
        const Step step = askPrompt(prompt, &passphrase, &remember, error);
        if (step != Step::Ok) {
            return step;
        }
        fromStore = false;
        if (passphrase.isEmpty()) {
            ++attempt;
        }
    }

    ssh_key publicKey = nullptr;
    int rc = ssh_pki_export_privkey_to_pubkey(privateKey, &publicKey);
    if (rc != SSH_OK || !publicKey) {
        ssh_key_free(privateKey);
        qCWarning(lcSsh) << "cannot derive the public key of" << keyFile;
        return Step::Denied;
    }
    rc = ssh_userauth_try_publickey(m_live->session, nullptr, publicKey);
    ssh_key_free(publicKey);
    if (rc == SSH_AUTH_SUCCESS) {
        rc = ssh_userauth_publickey(m_live->session, nullptr, privateKey);
    }
    ssh_key_free(privateKey);
    if (rc == SSH_AUTH_SUCCESS) {
        qCInfo(lcSsh) << "authenticated with key" << keyFile;
        if (profileKey && !passphrase.isEmpty()) {
            m_passphrase = passphrase;
            if (remember && m_request.profile.id.size() > 0) {
                emit storeSecret(m_generation, passphraseSecretKey(), passphrase);
            }
        }
        setAuthMethod(QStringLiteral("publickey"));
        return Step::Ok;
    }
    if (rc == SSH_AUTH_ERROR) {
        return methodError(error);
    }
    qCDebug(lcSsh) << "key" << keyFile << "rejected by the server";
    return Step::Denied;
}

SshWorker::Step SshWorker::authenticateWithPassword(bool reconnect, QString* error)
{
    int attempt = 0;
    QString password = m_password;
    bool havePassword = !password.isEmpty();
    bool fromStore = havePassword && password == m_request.savedPassword;
    bool remember = false;
    for (;;) {
        if (!havePassword) {
            if (attempt >= kMaxAuthAttempts) {
                *error = tr("Wrong password for %1").arg(target());
                return Step::Denied;
            }
            if (reconnect) {
                *error = tr("A password is needed for %1 - reconnect cancelled").arg(target());
                return Step::NeedsInteraction;
            }
            SshConnection::AuthPrompt prompt = makePrompt(SshConnection::PromptKind::Password, attempt + 1);
            prompt.title = tr("Password");
            prompt.prompt = tr("Password for %1:").arg(target());
            const Step step = askPrompt(prompt, &password, &remember, error);
            if (step != Step::Ok) {
                return step;
            }
            havePassword = true;
            fromStore = false;
        }
        const QByteArray utf8 = password.toUtf8();
        const int rc = ssh_userauth_password(m_live->session, nullptr, utf8.constData());
        if (rc == SSH_AUTH_SUCCESS) {
            m_password = password;
            if (remember && !m_request.profile.id.isEmpty()) {
                emit storeSecret(m_generation, passwordSecretKey(), password);
            }
            setAuthMethod(QStringLiteral("password"));
            return Step::Ok;
        }
        if (rc == SSH_AUTH_ERROR) {
            return methodError(error);
        }
        ++attempt;
        qCInfo(lcSsh) << "password rejected (attempt" << attempt << ")";
        if (fromStore) {
            emit secretRejected(m_generation, passwordSecretKey());
            m_password.clear();
            fromStore = false;
        }
        havePassword = false;
    }
}

SshWorker::Step SshWorker::authenticateInteractive(bool reconnect, QString* error)
{
    ssh_session session = m_live->session;
    int attempt = 1;
    int rounds = 0;
    bool savedAvailable = !m_password.isEmpty();
    bool usedSavedThisRound = false;
    bool savedWorked = false;
    QString rememberValue;
    int rc = ssh_userauth_kbdint(session, nullptr, nullptr);
    while (rc == SSH_AUTH_INFO) {
        if (++rounds > kMaxInteractiveRounds) {
            *error = tr("Keyboard-interactive authentication did not finish");
            return Step::Denied;
        }
        const int count = ssh_userauth_kbdint_getnprompts(session);
        const QString name = fromLibssh(ssh_userauth_kbdint_getname(session)).trimmed();
        const QString instruction = fromLibssh(ssh_userauth_kbdint_getinstruction(session)).trimmed();
        usedSavedThisRound = false;
        for (int i = 0; i < count; ++i) {
            char echo = 0;
            const QString text = fromLibssh(ssh_userauth_kbdint_getprompt(session, static_cast<unsigned int>(i), &echo));
            QString response;
            bool remember = false;
            if (savedAvailable && !echo) {
                response = m_password;
                savedAvailable = false;
                usedSavedThisRound = true;
            } else {
                if (reconnect) {
                    *error = tr("%1 asks for interactive input - reconnect cancelled").arg(target());
                    return Step::NeedsInteraction;
                }
                SshConnection::AuthPrompt prompt = makePrompt(SshConnection::PromptKind::KeyboardInteractive, attempt);
                prompt.title = name.isEmpty() ? tr("Keyboard-interactive authentication") : name;
                prompt.instruction = instruction;
                prompt.prompt = text.isEmpty() ? tr("Response:") : text;
                prompt.echo = echo != 0;
                const Step step = askPrompt(prompt, &response, &remember, error);
                if (step != Step::Ok) {
                    return step;
                }
                if (remember && !echo) {
                    rememberValue = response;
                }
            }
            const QByteArray utf8 = response.toUtf8();
            if (ssh_userauth_kbdint_setanswer(session, static_cast<unsigned int>(i), utf8.constData()) < 0) {
                return methodError(error);
            }
        }
        rc = ssh_userauth_kbdint(session, nullptr, nullptr);
        if (rc == SSH_AUTH_DENIED) {
            if (usedSavedThisRound) {
                emit secretRejected(m_generation, passwordSecretKey());
                m_password.clear();
            }
            if (++attempt > kMaxAuthAttempts) {
                *error = tr("Keyboard-interactive authentication failed for %1").arg(target());
                return Step::Denied;
            }
            qCInfo(lcSsh) << "keyboard-interactive round rejected, attempt" << attempt;
            rc = ssh_userauth_kbdint(session, nullptr, nullptr);
        } else if (rc == SSH_AUTH_SUCCESS && usedSavedThisRound) {
            savedWorked = true;
        }
    }
    if (rc == SSH_AUTH_SUCCESS) {
        if (!rememberValue.isEmpty()) {
            m_password = rememberValue;
            if (!m_request.profile.id.isEmpty()) {
                emit storeSecret(m_generation, passwordSecretKey(), rememberValue);
            }
        } else if (!savedWorked && usedSavedThisRound) {
            m_password.clear();
        }
        setAuthMethod(QStringLiteral("keyboard-interactive"));
        return Step::Ok;
    }
    if (rc == SSH_AUTH_ERROR) {
        return methodError(error);
    }
    return Step::Denied;
}

SshWorker::Step SshWorker::openShellChannel(QString* error)
{
    ssh_session session = m_live->session;
    ssh_channel channel = ssh_channel_new(session);
    if (!channel) {
        *error = tr("Cannot create a channel: %1").arg(libsshError());
        return Step::Failed;
    }
    if (ssh_channel_open_session(channel) != SSH_OK) {
        *error = tr("Cannot open the session channel on %1: %2").arg(target(), libsshError());
        ssh_channel_free(channel);
        return ssh_is_connected(session) ? Step::Failed : Step::NetworkFailed;
    }
    m_live->channel = channel;

    ssh_callbacks_init(&m_live->channelCallbacks);
    m_live->channelCallbacks.userdata = &m_live->exit;
    m_live->channelCallbacks.channel_exit_status_function = exitStatusCallback;
    m_live->channelCallbacks.channel_exit_signal_function = exitSignalCallback;
    if (ssh_set_channel_callbacks(channel, &m_live->channelCallbacks) == SSH_OK) {
        m_live->callbacksSet = true;
    }

    QString terminalType = m_request.profile.terminalType.trimmed();
    if (terminalType.isEmpty()) {
        terminalType = QStringLiteral("xterm-256color");
    }
    const QByteArray term = terminalType.toUtf8();
    if (ssh_channel_request_pty_size(channel, term.constData(), m_cols, m_rows) != SSH_OK) {
        *error = tr("PTY request on %1 failed: %2").arg(target(), libsshError());
        return ssh_is_connected(session) ? Step::Failed : Step::NetworkFailed;
    }
    m_live->ptyOpen = true;

    const QString command = m_request.profile.remoteCommand.trimmed();
    int rc = SSH_ERROR;
    if (command.isEmpty()) {
        rc = ssh_channel_request_shell(channel);
    } else {
        const QByteArray utf8 = command.toUtf8();
        rc = ssh_channel_request_exec(channel, utf8.constData());
    }
    if (rc != SSH_OK) {
        *error = command.isEmpty() ? tr("Cannot start a shell on %1: %2").arg(target(), libsshError())
                                   : tr("Cannot run '%1' on %2: %3").arg(command, target(), libsshError());
        return ssh_is_connected(session) ? Step::Failed : Step::NetworkFailed;
    }
    m_live->shellOpen = true;
    qCInfo(lcSsh) << (command.isEmpty() ? "shell" : "command") << "started on" << target() << "with" << terminalType
                  << m_cols << "x" << m_rows;
    emit shellStarted(m_generation);
    return Step::Ok;
}

// ---------------------------------------------------------------------------------------
// Questions for the GUI
// ---------------------------------------------------------------------------------------

SshWorker::Step SshWorker::waitForAnswer(QString* error)
{
    QMutexLocker lock(&m_pending->mutex);
    const QDeadlineTimer deadline(kAnswerTimeoutMs);
    while (!m_pending->answered && !m_pending->cancelled && !aborted() && !deadline.hasExpired()) {
        m_pending->condition.wait(&m_pending->mutex, deadline);
    }
    m_pending->active = false;
    if (aborted()) {
        return Step::Aborted;
    }
    if (m_pending->answered) {
        return Step::Ok;
    }
    if (m_pending->cancelled) {
        *error = tr("Connection to %1 cancelled").arg(target());
        return Step::Cancelled;
    }
    *error = tr("No answer within 5 minutes - connection to %1 cancelled").arg(target());
    return Step::Cancelled;
}

SshWorker::Step SshWorker::askHostKey(const SshConnection::HostKeyInfo& info, bool* accept, bool* remember, QString* error)
{
    {
        QMutexLocker lock(&m_pending->mutex);
        m_pending->active = true;
        m_pending->answered = false;
        m_pending->cancelled = false;
        m_pending->accept = false;
        m_pending->remember = false;
        m_pending->response.clear();
    }
    emit hostKeyVerificationRequired(m_generation, info);
    const Step step = waitForAnswer(error);
    if (step != Step::Ok) {
        return step;
    }
    QMutexLocker lock(&m_pending->mutex);
    *accept = m_pending->accept;
    *remember = m_pending->remember;
    return Step::Ok;
}

SshWorker::Step SshWorker::askPrompt(const SshConnection::AuthPrompt& prompt, QString* response, bool* remember, QString* error)
{
    {
        QMutexLocker lock(&m_pending->mutex);
        m_pending->active = true;
        m_pending->answered = false;
        m_pending->cancelled = false;
        m_pending->accept = false;
        m_pending->remember = false;
        m_pending->response.clear();
    }
    emit authPromptRequired(m_generation, prompt);
    const Step step = waitForAnswer(error);
    if (step != Step::Ok) {
        return step;
    }
    QMutexLocker lock(&m_pending->mutex);
    *response = m_pending->response;
    *remember = m_pending->remember;
    m_pending->response.clear();
    return Step::Ok;
}

// ---------------------------------------------------------------------------------------
// Steady state: the tick
// ---------------------------------------------------------------------------------------

void SshWorker::tick()
{
    if (m_inTick || !m_live) {
        return;
    }
    m_inTick = true;
    pumpShell();
    if (m_live) {
        pumpForwards();
    }
    if (m_live) {
        serviceTransfer();
    }
    m_inTick = false;
}

void SshWorker::pumpShell()
{
    ssh_channel channel = m_live->channel;
    if (!channel || !m_live->shellOpen) {
        return;
    }
    const quint64 gen = m_generation;
    QByteArray received;
    bool readError = false;
    for (int stream = 0; stream < 2 && !readError; ++stream) {
        while (received.size() < kMaxReadPerTick) {
            const int n = ssh_channel_read_nonblocking(channel, m_readBuffer.data(),
                                                       static_cast<uint32_t>(m_readBuffer.size()), stream);
            if (n > 0) {
                received.append(m_readBuffer.constData(), n);
                continue;
            }
            if (n == SSH_ERROR) {
                readError = true;
            }
            break;
        }
    }
    if (!received.isEmpty()) {
        emit dataReceived(gen, received);
    }
    // A clean end of the shell is an EOF / close sent by the server (usually after the exit
    // status) while the transport itself is still alive. Once the TCP link is gone libssh reports
    // the channel as closed as well (ssh_channel_is_closed() is true whenever the session died),
    // so that alone must not count as a clean exit: it is a dropped link.
    //
    // The read loop above stops at kMaxReadPerTick. The exit status arrives through the channel
    // callback as soon as its request packet is seen, which can be ahead of more than 1 MiB
    // still buffered in libssh (a burst that ends in "exit", pulled in while a blocking SFTP call
    // ran); ending the shell on it then would drop that tail. So the status (and a close) only
    // count once the loop ran dry - otherwise the next tick keeps reading first. EOF needs no
    // such guard: ssh_channel_is_eof() is false while unread data remains, and libssh delays the
    // close itself until the data was read.
    const bool drained = received.size() < kMaxReadPerTick;
    const bool sessionAlive = ssh_is_connected(m_live->session) != 0;
    if (ssh_channel_is_eof(channel)
        || (drained && (m_live->exit.received || (sessionAlive && ssh_channel_is_closed(channel))))) {
        finishShell();
        return;
    }
    if (readError || !sessionAlive) {
        handleConnectionLost(tr("Connection to %1 lost: %2").arg(target(), libsshError()));
        return;
    }

    QList<QByteArray> chunks;
    {
        QMutexLocker lock(&m_outgoingMutex);
        chunks.swap(m_outgoing);
        m_outgoingBytes = 0;
    }
    for (qsizetype c = 0; c < chunks.size(); ++c) {
        const QByteArray& chunk = chunks.at(c);
        qsizetype offset = 0;
        while (offset < chunk.size()) {
            const int n = ssh_channel_write(channel, chunk.constData() + offset,
                                            static_cast<uint32_t>(chunk.size() - offset));
            if (n == SSH_ERROR) {
                handleConnectionLost(tr("Write to %1 failed: %2").arg(target(), libsshError()));
                return;
            }
            if (n <= 0) {
                break;   // the remote window stayed closed for the whole session timeout
            }
            offset += n;
            emit txBytesWritten(gen, n);
        }
        if (offset < chunk.size()) {
            // ssh_channel_write() returns a short count once the remote window has been closed
            // for SSH_OPTIONS_TIMEOUT (the foreground process is not reading its PTY). Nothing
            // may be dropped: the rest of this chunk and every later one go back to the head of
            // the queue, ahead of what write() appended meanwhile, and the next tick retries.
            QList<QByteArray> rest;
            rest.append(chunk.mid(offset));
            for (qsizetype k = c + 1; k < chunks.size(); ++k) {
                rest.append(chunks.at(k));
            }
            qint64 restBytes = 0;
            for (const QByteArray& pending : std::as_const(rest)) {
                restBytes += pending.size();
            }
            QMutexLocker lock(&m_outgoingMutex);
            m_outgoing = rest + m_outgoing;
            m_outgoingBytes += restBytes;
            qCWarning(lcSsh) << "remote window of" << target() << "closed:" << restBytes << "bytes queued again";
            break;
        }
    }
    if (!ssh_is_connected(m_live->session)) {
        handleConnectionLost(tr("Connection to %1 lost").arg(target()));
    }
}

void SshWorker::finishShell()
{
    const quint64 gen = m_generation;
    const int status = m_live->exit.received ? m_live->exit.status : -1;
    {
        QMutexLocker lock(&m_shared->mutex);
        m_shared->exitStatus = status;
    }
    qCInfo(lcSsh) << "shell on" << target() << "closed with exit status" << status;
    stopTimers();
    teardownLive();
    emit channelClosed(gen, status);
    emit stateChanged(gen, Transport::State::Disconnected);
}

void SshWorker::handleConnectionLost(const QString& reason)
{
    const quint64 gen = m_generation;
    qCWarning(lcSsh) << "connection lost:" << reason;
    stopTimers();
    teardownLive();
    emit linkDropped(gen, reason);
}

void SshWorker::scheduleReconnect()
{
    m_reconnectDelaySeconds = m_reconnectDelaySeconds == 0
        ? kReconnectFirstSeconds
        : std::min(m_reconnectDelaySeconds * 2, kReconnectMaxSeconds);
    qCInfo(lcSsh) << "reconnecting to" << target() << "in" << m_reconnectDelaySeconds << "s";
    m_reconnectTimer->start(m_reconnectDelaySeconds * 1000);
}

void SshWorker::onReconnectTimer()
{
    if (aborted() || m_live) {
        return;
    }
    runConnect(true);
}

void SshWorker::sendKeepAlive()
{
    if (!m_live || !m_live->session) {
        return;
    }
    if (ssh_send_ignore(m_live->session, kKeepAlivePayload) == SSH_ERROR) {
        ++m_keepAliveFailures;
        qCWarning(lcSsh) << "keep-alive to" << target() << "failed (" << m_keepAliveFailures << ")";
        if (m_keepAliveFailures >= kKeepAliveMaxFailures) {
            handleConnectionLost(tr("Keep-alive to %1 failed %2 times").arg(target()).arg(kKeepAliveMaxFailures));
        }
    } else {
        m_keepAliveFailures = 0;
    }
}

void SshWorker::stopTimers()
{
    m_tickTimer->stop();
    m_keepAliveTimer->stop();
    m_reconnectTimer->stop();
}

void SshWorker::teardownLive()
{
    if (!m_live) {
        return;
    }
    teardownForwards();
    if (m_live->transfer) {
        finishTransfer(false, tr("Transfer aborted: the connection was closed"));
    }
    const bool connected = m_live->session && ssh_is_connected(m_live->session);
    if (m_live->sftp) {
        sftp_free(m_live->sftp);
        m_live->sftp = nullptr;
    }
    if (m_live->channel) {
        if (m_live->callbacksSet) {
            ssh_remove_channel_callbacks(m_live->channel, &m_live->channelCallbacks);
        }
        if (connected && ssh_channel_is_open(m_live->channel)) {
            ssh_channel_send_eof(m_live->channel);
            ssh_channel_close(m_live->channel);
        }
        ssh_channel_free(m_live->channel);
        m_live->channel = nullptr;
    }
    if (m_live->session) {
        if (connected) {
            ssh_disconnect(m_live->session);
        }
        ssh_free(m_live->session);
        m_live->session = nullptr;
    }
    m_live.reset();
}

// ---------------------------------------------------------------------------------------
// Local port forwards
// ---------------------------------------------------------------------------------------

void SshWorker::setupForwards()
{
    const quint64 gen = m_generation;
    const QList<SshLocalForward> forwards = m_request.profile.localForwards;
    for (const SshLocalForward& spec : forwards) {
        if (spec.localPort == 0 || spec.remotePort == 0 || spec.remoteHost.trimmed().isEmpty()) {
            emit forwardFailed(gen, spec.localPort, tr("Invalid port forward %1").arg(spec.displayText()));
            continue;
        }
        QHostAddress bind;
        if (!parseBindAddress(spec.bindAddress, &bind)) {
            const QString message =
                tr("Invalid bind address \"%1\" for the port forward %2").arg(spec.bindAddress.trimmed(), spec.displayText());
            qCWarning(lcSsh) << message;
            emit forwardFailed(gen, spec.localPort, message);
            continue;
        }
        auto* listener = new ForwardListener;
        listener->spec = spec;
        listener->server = new QTcpServer(this);
        if (!listener->server->listen(bind, spec.localPort)) {
            const QString message = listener->server->errorString();
            qCWarning(lcSsh) << "cannot listen for forward" << spec.displayText() << ":" << message;
            emit forwardFailed(gen, spec.localPort, message);
            delete listener->server;
            delete listener;
            continue;
        }
        connect(listener->server, &QTcpServer::newConnection, this, [this, listener] { acceptForward(listener); });
        m_live->forwards.append(listener);
        qCInfo(lcSsh) << "forward listening:" << spec.displayText();
        emit forwardListening(gen, spec.localPort, spec.remoteHost, spec.remotePort);
    }
}

void SshWorker::acceptForward(ForwardListener* listener)
{
    if (!m_live || !listener->server) {
        return;
    }
    while (QTcpSocket* socket = listener->server->nextPendingConnection()) {
        // A child of the QTcpServer. Once bridged it belongs to the Bridge and is released by
        // closeBridge() only; sockets refused here are dropped right away instead.
        if (!m_live->session || !ssh_is_connected(m_live->session)) {
            socket->abort();
            socket->deleteLater();
            continue;
        }
        ssh_channel channel = ssh_channel_new(m_live->session);
        const QByteArray remoteHost = listener->spec.remoteHost.trimmed().toUtf8();
        const int rc = channel ? ssh_channel_open_forward(channel, remoteHost.constData(), listener->spec.remotePort,
                                                          "127.0.0.1", listener->spec.localPort)
                               : SSH_ERROR;
        if (rc != SSH_OK) {
            qCWarning(lcSsh) << "forward" << listener->spec.displayText() << "refused:" << libsshError();
            if (channel) {
                ssh_channel_free(channel);
            }
            socket->abort();
            socket->deleteLater();
            continue;
        }
        qCDebug(lcSsh) << "forward" << listener->spec.displayText() << "bridged";
        listener->bridges.append({socket, channel});
    }
}

void SshWorker::pumpForwards()
{
    for (ForwardListener* listener : std::as_const(m_live->forwards)) {
        for (int i = 0; i < listener->bridges.size();) {
            Bridge& bridge = listener->bridges[i];
            QTcpSocket* socket = bridge.socket;   // valid until closeBridge(): the bridge owns it
            bool done = (socket == nullptr || bridge.channel == nullptr);
            // Local client -> channel; a socket the peer already closed still yields its tail.
            while (!done && socket->bytesAvailable() > 0) {
                const QByteArray data = socket->read(kReadChunk);
                if (data.isEmpty()) {
                    break;
                }
                if (ssh_channel_write(bridge.channel, data.constData(), static_cast<uint32_t>(data.size())) == SSH_ERROR) {
                    done = true;
                }
            }
            // Channel -> local client.
            while (!done) {
                const int n = ssh_channel_read_nonblocking(bridge.channel, m_readBuffer.data(),
                                                           static_cast<uint32_t>(m_readBuffer.size()), 0);
                if (n > 0) {
                    socket->write(m_readBuffer.constData(), n);
                    continue;
                }
                if (n == SSH_ERROR) {
                    done = true;
                }
                break;
            }
            if (!done
                && (ssh_channel_is_eof(bridge.channel) || ssh_channel_is_closed(bridge.channel)
                    || socket->state() == QAbstractSocket::UnconnectedState)) {
                done = true;
            }
            if (done) {
                closeBridge(bridge);
                listener->bridges.removeAt(i);
            } else {
                ++i;
            }
        }
    }
}

void SshWorker::closeBridge(Bridge& bridge)
{
    if (bridge.channel) {
        if (m_live && m_live->session && ssh_is_connected(m_live->session) && ssh_channel_is_open(bridge.channel)) {
            ssh_channel_send_eof(bridge.channel);
            ssh_channel_close(bridge.channel);
        }
        ssh_channel_free(bridge.channel);
        bridge.channel = nullptr;
    }
    // The only place that lets go of the socket (see Bridge). Arming deleteLater() on
    // disconnected() while the Bridge still held the pointer let the worker loop delete the
    // socket between two ticks when the local client closed first, and the next
    // pumpForwards() / teardownForwards() dereferenced freed memory.
    if (QTcpSocket* socket = std::exchange(bridge.socket, nullptr)) {
        if (socket->state() == QAbstractSocket::UnconnectedState) {
            socket->deleteLater();
        } else {
            // Let the bytes still queued for the local client leave first; the socket deletes
            // itself once its side is down (at once when nothing is pending). Should the peer
            // never drain, the listener's QTcpServer - its parent - takes it down with the
            // session in teardownForwards().
            connect(socket, &QAbstractSocket::disconnected, socket, &QObject::deleteLater);
            socket->disconnectFromHost();
        }
    }
}

void SshWorker::teardownForwards()
{
    for (ForwardListener* listener : std::as_const(m_live->forwards)) {
        for (Bridge& bridge : listener->bridges) {
            closeBridge(bridge);
        }
        listener->bridges.clear();
        if (listener->server) {
            listener->server->close();
            delete listener->server;
        }
        delete listener;
    }
    m_live->forwards.clear();
}

// ---------------------------------------------------------------------------------------
// SFTP transfers
// ---------------------------------------------------------------------------------------

bool SshWorker::ensureSftp(QString* error)
{
    if (m_live->sftp) {
        return true;
    }
    sftp_session sftp = sftp_new(m_live->session);
    if (!sftp) {
        *error = tr("Cannot start SFTP on %1: %2").arg(target(), libsshError());
        return false;
    }
    if (sftp_init(sftp) != SSH_OK) {
        *error = tr("SFTP initialisation on %1 failed: %2 (code %3)").arg(target(), libsshError()).arg(sftp_get_error(sftp));
        sftp_free(sftp);
        return false;
    }
    m_live->sftp = sftp;
    qCInfo(lcSsh) << "SFTP subsystem started on" << target();
    return true;
}

void SshWorker::serviceTransfer()
{
    Transfer* transfer = m_live->transfer.get();
    if (!transfer) {
        return;
    }
    if (m_live->cancelTransfer) {
        finishTransfer(false, tr("Transfer cancelled"));
        return;
    }
    QElapsedTimer slice;
    slice.start();
    qint64 moved = 0;
    while (moved < kTransferBytesPerTick && slice.elapsed() < kTransferMsPerTick) {
        if (transfer->request.direction == SshConnection::TransferDirection::Upload) {
            const QByteArray chunk = transfer->local.read(kTransferChunk);
            if (chunk.isEmpty()) {
                if (transfer->local.error() != QFileDevice::NoError) {
                    finishTransfer(false, tr("Cannot read %1: %2").arg(QDir::toNativeSeparators(transfer->request.localPath),
                                                                       transfer->local.errorString()));
                    return;
                }
                completeTransfer();
                return;
            }
            qsizetype offset = 0;
            while (offset < chunk.size()) {
                const ssize_t n = sftp_write(transfer->remote, chunk.constData() + offset, static_cast<size_t>(chunk.size() - offset));
                if (n <= 0) {
                    finishTransfer(false, tr("Write to %1 failed: %2").arg(transfer->request.remotePath, sftpError()));
                    return;
                }
                offset += static_cast<qsizetype>(n);
            }
            transfer->done += chunk.size();
            moved += chunk.size();
        } else {
            const ssize_t n = sftp_read(transfer->remote, m_readBuffer.data(), static_cast<size_t>(kTransferChunk));
            if (n < 0) {
                finishTransfer(false, tr("Read from %1 failed: %2").arg(transfer->request.remotePath, sftpError()));
                return;
            }
            if (n == 0) {
                completeTransfer();
                return;
            }
            if (transfer->local.write(m_readBuffer.constData(), static_cast<qint64>(n)) != static_cast<qint64>(n)) {
                finishTransfer(false, tr("Cannot write %1: %2").arg(QDir::toNativeSeparators(transfer->partPath),
                                                                    transfer->local.errorString()));
                return;
            }
            transfer->done += static_cast<qint64>(n);
            moved += static_cast<qint64>(n);
        }
    }
    emitProgress(false);
}

void SshWorker::completeTransfer()
{
    Transfer* transfer = m_live->transfer.get();
    const SshConnection::TransferRequest request = transfer->request;
    const qint64 done = transfer->done;
    if (transfer->remote) {
        const int rc = sftp_close(transfer->remote);
        transfer->remote = nullptr;
        if (rc != SSH_OK) {
            finishTransfer(false, tr("Closing %1 failed: %2").arg(request.remotePath, sftpError()));
            return;
        }
    }
    if (request.direction == SshConnection::TransferDirection::Upload) {
        transfer->local.close();
        finishTransfer(true, tr("Uploaded %1 to %2 (%3)")
                                 .arg(QFileInfo(request.localPath).fileName(), request.remotePath, sizeText(done)));
        return;
    }
    if (!transfer->local.flush()) {
        finishTransfer(false, tr("Cannot write %1: %2").arg(QDir::toNativeSeparators(transfer->partPath),
                                                            transfer->local.errorString()));
        return;
    }
    transfer->local.close();
    if (QFileInfo::exists(request.localPath) && !QFile::remove(request.localPath)) {
        finishTransfer(false, tr("Cannot replace %1").arg(QDir::toNativeSeparators(request.localPath)));
        return;
    }
    if (!QFile::rename(transfer->partPath, request.localPath)) {
        finishTransfer(false, tr("Cannot rename %1 to %2").arg(QDir::toNativeSeparators(transfer->partPath),
                                                               QDir::toNativeSeparators(request.localPath)));
        return;
    }
    if (request.preservePermissions && transfer->haveRemoteMode) {
        QFile::setPermissions(request.localPath, permissionsFromMode(transfer->remoteMode & 0777u));
    }
    finishTransfer(true, tr("Downloaded %1 to %2 (%3)")
                             .arg(request.remotePath, QDir::toNativeSeparators(request.localPath), sizeText(done)));
}

void SshWorker::finishTransfer(bool ok, const QString& message)
{
    Transfer* transfer = m_live ? m_live->transfer.get() : nullptr;
    if (!transfer) {
        return;
    }
    const quint64 gen = m_generation;
    if (transfer->remote) {
        sftp_close(transfer->remote);
        transfer->remote = nullptr;
    }
    if (transfer->local.isOpen()) {
        transfer->local.close();
    }
    if (!ok && transfer->request.direction == SshConnection::TransferDirection::Download && !transfer->partPath.isEmpty()) {
        QFile::remove(transfer->partPath);
    }
    if (ok) {
        emitProgress(true);
    }
    const qint64 done = transfer->done;
    m_live->transfer.reset();
    m_live->cancelTransfer = false;
    {
        QMutexLocker lock(&m_shared->mutex);
        m_shared->transfer.active = false;
        m_shared->transfer.done = done;
    }
    if (ok) {
        qCInfo(lcSsh) << message;
    } else {
        qCWarning(lcSsh) << "transfer failed:" << message;
    }
    emit transferFinished(gen, ok, message);
}

void SshWorker::publishTransfer()
{
    QMutexLocker lock(&m_shared->mutex);
    if (!m_live || !m_live->transfer) {
        m_shared->transfer.active = false;
        return;
    }
    const Transfer* transfer = m_live->transfer.get();
    m_shared->transfer.active = true;
    m_shared->transfer.direction = transfer->request.direction;
    m_shared->transfer.localPath = transfer->request.localPath;
    m_shared->transfer.remotePath = transfer->request.remotePath;
    m_shared->transfer.done = transfer->done;
    m_shared->transfer.total = transfer->total;
}

void SshWorker::emitProgress(bool force)
{
    Transfer* transfer = m_live->transfer.get();
    if (!transfer) {
        return;
    }
    if (!force && transfer->progressTimer.elapsed() < kProgressIntervalMs) {
        return;
    }
    transfer->progressTimer.restart();
    publishTransfer();
    emit transferProgress(m_generation, transfer->done, transfer->total);
}

// ---------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------

QString SshWorker::target() const
{
    SshProfile p = m_request.profile;
    p.name.clear();
    if (p.user.isEmpty() && m_live) {
        p.user = m_live->user;
    }
    return p.displayTarget();
}

QString SshWorker::displayName() const
{
    return m_request.profile.displayName();
}

QString SshWorker::libsshError() const
{
    if (!m_live || !m_live->session) {
        return QString();
    }
    return fromLibssh(ssh_get_error(m_live->session));
}

QString SshWorker::sftpError() const
{
    if (!m_live || !m_live->sftp) {
        return libsshError();
    }
    const int code = sftp_get_error(m_live->sftp);
    QString text;
    switch (code) {
    case SSH_FX_OK:
        text = libsshError();
        break;
    case SSH_FX_EOF:
        text = tr("end of file");
        break;
    case SSH_FX_NO_SUCH_FILE:
    case SSH_FX_NO_SUCH_PATH:
        text = tr("no such file");
        break;
    case SSH_FX_PERMISSION_DENIED:
        text = tr("permission denied");
        break;
    case SSH_FX_FAILURE:
        text = tr("operation failed");
        break;
    case SSH_FX_BAD_MESSAGE:
        text = tr("bad message");
        break;
    case SSH_FX_NO_CONNECTION:
    case SSH_FX_CONNECTION_LOST:
        text = tr("connection lost");
        break;
    case SSH_FX_OP_UNSUPPORTED:
        text = tr("operation not supported");
        break;
    case SSH_FX_INVALID_HANDLE:
        text = tr("invalid handle");
        break;
    case SSH_FX_FILE_ALREADY_EXISTS:
        text = tr("file already exists");
        break;
    case SSH_FX_WRITE_PROTECT:
        text = tr("write protected");
        break;
    case SSH_FX_NO_MEDIA:
        text = tr("no media");
        break;
    default:
        text = tr("SFTP error %1").arg(code);
        break;
    }
    if (text.isEmpty()) {
        text = tr("SFTP error %1").arg(code);
    }
    return text;
}

QString SshWorker::passwordSecretKey() const
{
    return QStringLiteral("ssh/%1/password").arg(m_request.profile.id);
}

QString SshWorker::passphraseSecretKey() const
{
    return QStringLiteral("ssh/%1/passphrase").arg(m_request.profile.id);
}
