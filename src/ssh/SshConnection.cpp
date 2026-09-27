#include "ssh/SshConnection.h"

#include "app/Logging.h"
#include "ssh/LibsshInclude.h"
#include "ssh/SecretStore.h"
#include "ssh/SshWorker.h"

#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QMutexLocker>
#include <QThread>
#include <QTimer>

#include <optional>
#include <utility>

namespace {

constexpr int kStartupCommandDelayMs = 300;

/// Append the SecretStore entry `key` to `list` when the store holds one (empty keys are skipped).
void appendSavedSecret(QList<SshWorker::SavedSecret>* list, const QString& key)
{
    if (key.isEmpty()) {
        return;
    }
    const std::optional<QString> value = SecretStore::load(key);
    if (value && !value->isEmpty()) {
        list->append({key, *value});
    }
}

} // namespace

struct SshConnection::Private
{
    SshProfile profile;
    std::shared_ptr<SshSharedState> shared = std::make_shared<SshSharedState>();
    QThread* thread = nullptr;
    SshWorker* worker = nullptr;
    quint64 generation = 0;     ///< generation of the current open(); worker signals tagged otherwise are dropped
    bool workerBusy = false;    ///< the worker owns a connect / session / reconnect for `generation`
    int cols = 80;
    int rows = 24;

    /// End whatever the worker is doing for the current generation and start a new one, so
    /// nothing it still reports for the old generation reaches the GUI.
    void abortWorker()
    {
        const quint64 gen = generation;
        workerBusy = false;
        worker->requestAbort(gen);
        SshWorker* w = worker;
        QMetaObject::invokeMethod(w, [w, gen] { w->close(gen); }, Qt::QueuedConnection);
        ++generation;
    }
};

SshConnection::SshConnection(QObject* parent)
    : Transport(parent)
    , d(std::make_unique<Private>())
{
    qRegisterMetaType<Transport::State>();
    qRegisterMetaType<SshConnection::HostKeyInfo>();
    qRegisterMetaType<SshConnection::AuthPrompt>();
    qRegisterMetaType<SshConnection::TransferRequest>();
    qRegisterMetaType<SshConnection::TransferStatus>();
    qRegisterMetaType<SshConnection::HostKeyStatus>();
    qRegisterMetaType<SshConnection::PromptKind>();
    qRegisterMetaType<SshConnection::TransferDirection>();

    d->thread = new QThread(this);
    d->thread->setObjectName(QStringLiteral("ssh-worker"));
    d->worker = new SshWorker(d->shared);
    d->worker->moveToThread(d->thread);

    SshWorker* w = d->worker;
    const auto current = [this](quint64 gen) { return gen == d->generation; };

    connect(w, &SshWorker::stateChanged, this, [this, current](quint64 gen, Transport::State state) {
        if (!current(gen)) {
            return;
        }
        if (state == State::Disconnected) {
            d->workerBusy = false;
        }
        setState(state);
    }, Qt::QueuedConnection);
    connect(w, &SshWorker::errorOccurred, this, [this, current](quint64 gen, const QString& message) {
        if (!current(gen)) {
            return;
        }
        setErrorString(message);
        emit errorOccurred(message);
    }, Qt::QueuedConnection);
    connect(w, &SshWorker::dataReceived, this, [this, current](quint64 gen, const QByteArray& data) {
        if (!current(gen)) {
            return;
        }
        emit dataReceived(data);
        countReceived(data.size());
    }, Qt::QueuedConnection);
    connect(w, &SshWorker::txBytesWritten, this, [this, current](quint64 gen, qint64 bytes) {
        if (current(gen)) {
            emit txBytesWritten(bytes);
        }
    }, Qt::QueuedConnection);
    connect(w, &SshWorker::hostKeyVerificationRequired, this, [this, current](quint64 gen, const SshConnection::HostKeyInfo& info) {
        if (!current(gen)) {
            return;
        }
        qCInfo(lcSsh) << "host key question for" << info.host << info.keyType << info.fingerprintSha256;
        emit hostKeyVerificationRequired(info);
    }, Qt::QueuedConnection);
    connect(w, &SshWorker::authPromptRequired, this, [this, current](quint64 gen, const SshConnection::AuthPrompt& prompt) {
        if (current(gen)) {
            emit authPromptRequired(prompt);
        }
    }, Qt::QueuedConnection);
    connect(w, &SshWorker::authenticated, this, [this, current](quint64 gen, const QString& method) {
        if (current(gen)) {
            emit authenticated(method);
        }
    }, Qt::QueuedConnection);
    connect(w, &SshWorker::bannerReceived, this, [this, current](quint64 gen, const QString& text) {
        if (current(gen)) {
            emit bannerReceived(text);
        }
    }, Qt::QueuedConnection);
    connect(w, &SshWorker::shellStarted, this, [this, current](quint64 gen) {
        if (!current(gen)) {
            return;
        }
        emit shellStarted();
        const QString startup = d->profile.startupCommand.trimmed();
        if (!startup.isEmpty()) {
            QTimer::singleShot(kStartupCommandDelayMs, this, [this, gen, startup] {
                if (gen == d->generation && state() == State::Connected) {
                    qCInfo(lcSsh) << "sending startup command" << startup;
                    write((startup + QLatin1Char('\r')).toUtf8());
                }
            });
        }
    }, Qt::QueuedConnection);
    connect(w, &SshWorker::channelClosed, this, [this, current](quint64 gen, int exitStatus) {
        if (current(gen)) {
            emit channelClosed(exitStatus);
        }
    }, Qt::QueuedConnection);
    connect(w, &SshWorker::linkDropped, this, [this, current](quint64 gen, const QString& reason) {
        if (!current(gen)) {
            return;
        }
        setErrorString(reason);
        emit errorOccurred(reason);
        emit connectionLost(displayName());
        // A receiver of those two may have closed (a new generation) or re-opened the
        // connection, or turned the reconnect off; the decision below belongs to the generation
        // that dropped and is skipped once it is gone - otherwise the facade would sit in
        // Reconnecting forever with a worker that ignores beginReconnect() for a stale generation.
        if (!current(gen) || state() == State::Disconnected) {
            return;
        }
        if (autoReconnect()) {
            setState(State::Reconnecting);
            SshWorker* worker = d->worker;
            QMetaObject::invokeMethod(worker, [worker, gen] { worker->beginReconnect(gen); }, Qt::QueuedConnection);
        } else {
            d->abortWorker();
            setState(State::Disconnected);
        }
    }, Qt::QueuedConnection);
    connect(w, &SshWorker::connectionRestored, this, [this, current](quint64 gen, const QString& name) {
        if (current(gen)) {
            emit connectionRestored(name);
        }
    }, Qt::QueuedConnection);
    connect(w, &SshWorker::transferStarted, this, [this, current](quint64 gen, const SshConnection::TransferRequest& request) {
        if (current(gen)) {
            emit transferStarted(request);
        }
    }, Qt::QueuedConnection);
    connect(w, &SshWorker::transferProgress, this, [this, current](quint64 gen, qint64 done, qint64 total) {
        if (current(gen)) {
            emit transferProgress(done, total);
        }
    }, Qt::QueuedConnection);
    connect(w, &SshWorker::transferFinished, this, [this, current](quint64 gen, bool ok, const QString& message) {
        if (current(gen)) {
            emit transferFinished(ok, message);
        }
    }, Qt::QueuedConnection);
    connect(w, &SshWorker::remoteHomeReceived, this, [this, current](quint64 gen, const QString& path) {
        if (current(gen)) {
            emit remoteHomeReceived(path);
        }
    }, Qt::QueuedConnection);
    connect(w, &SshWorker::forwardListening, this,
            [this, current](quint64 gen, quint16 localPort, const QString& remoteHost, quint16 remotePort) {
                if (current(gen)) {
                    emit forwardListening(localPort, remoteHost, remotePort);
                }
            }, Qt::QueuedConnection);
    connect(w, &SshWorker::forwardFailed, this, [this, current](quint64 gen, quint16 localPort, const QString& message) {
        if (current(gen)) {
            emit forwardFailed(localPort, message);
        }
    }, Qt::QueuedConnection);
    connect(w, &SshWorker::storeSecret, this, [current](quint64 gen, const QString& key, const QString& value) {
        if (!current(gen)) {
            return;
        }
        if (SecretStore::store(key, value)) {
            qCInfo(lcSsh) << "secret remembered for" << key;
        } else {
            qCWarning(lcSsh) << "cannot remember the secret for" << key;
        }
    }, Qt::QueuedConnection);
    connect(w, &SshWorker::secretRejected, this, [current](quint64 gen, const QString& key) {
        if (!current(gen)) {
            return;
        }
        qCInfo(lcSsh) << "saved secret rejected, forgetting" << key;
        SecretStore::remove(key);
    }, Qt::QueuedConnection);

    // A transition to Disconnected that the worker did not report itself - close(), or
    // setAutoReconnect(false) while Reconnecting (Transport handles that) - ends the worker's
    // activity for the current generation.
    connect(this, &Transport::stateChanged, this, [this](Transport::State state) {
        if (state == State::Disconnected && d->workerBusy) {
            d->abortWorker();
        }
    });

    d->thread->start();
}

SshConnection::~SshConnection()
{
    // An object under destruction emits nothing but destroyed(): the close() below would
    // otherwise report stateChanged(Disconnected) into receivers whose storage may already be
    // gone (a recorder declared after the connection, a lambda's captured list). close() ends
    // the worker's activity explicitly (abortWorker), so no internal slot depends on the signal.
    blockSignals(true);
    close();
    // Everything queued for the worker (the close above included) runs before this quit; the
    // worker thread is then joined and its object deleted from here.
    QThread* thread = d->thread;
    QMetaObject::invokeMethod(d->worker, [thread] { thread->quit(); }, Qt::QueuedConnection);
    disconnect(d->worker, nullptr, this, nullptr);
    thread->wait();
    delete d->worker;
    d->worker = nullptr;
}

// ---------------------------------------------------------------------------------------
// Transport interface
// ---------------------------------------------------------------------------------------

Transport::Kind SshConnection::kind() const
{
    return Kind::Ssh;
}

QString SshConnection::displayName() const
{
    if (d->profile.host.trimmed().isEmpty()) {
        return QStringLiteral("SSH");
    }
    return d->profile.displayName();
}

QString SshConnection::summary() const
{
    QString keyType;
    QString method;
    {
        QMutexLocker lock(&d->shared->mutex);
        keyType = d->shared->hostKeyType;
        method = d->shared->authMethod;
    }
    if (state() != State::Disconnected && !keyType.isEmpty() && !method.isEmpty()) {
        return QStringLiteral("%1 · %2").arg(keyType, method);
    }
    return QStringLiteral("SSH");
}

QVariantMap SshConnection::settingsMap() const
{
    QVariantMap map;
    map.insert(QStringLiteral("kind"), QStringLiteral("ssh"));
    map.insert(QStringLiteral("profileId"), d->profile.id);
    map.insert(QStringLiteral("target"), d->profile.displayTarget());
    return map;
}

SshProfile SshConnection::profile() const
{
    return d->profile;
}

void SshConnection::setProfile(const SshProfile& profile)
{
    if (state() != State::Disconnected) {
        qCWarning(lcSsh) << "setProfile ignored while" << stateText(state());
        return;
    }
    d->profile = profile;
}

// ---------------------------------------------------------------------------------------
// Facts about the current / last connection
// ---------------------------------------------------------------------------------------

QString SshConnection::serverVersion() const
{
    QMutexLocker lock(&d->shared->mutex);
    return d->shared->serverVersion;
}

QString SshConnection::hostKeyType() const
{
    QMutexLocker lock(&d->shared->mutex);
    return d->shared->hostKeyType;
}

QString SshConnection::hostKeyFingerprint() const
{
    QMutexLocker lock(&d->shared->mutex);
    return d->shared->hostKeyFingerprint;
}

QString SshConnection::authMethod() const
{
    QMutexLocker lock(&d->shared->mutex);
    return d->shared->authMethod;
}

QString SshConnection::remoteHome() const
{
    QMutexLocker lock(&d->shared->mutex);
    return d->shared->remoteHome;
}

int SshConnection::exitStatus() const
{
    QMutexLocker lock(&d->shared->mutex);
    return d->shared->exitStatus;
}

bool SshConnection::isTransferActive() const
{
    QMutexLocker lock(&d->shared->mutex);
    return d->shared->transfer.active;
}

SshConnection::TransferStatus SshConnection::transferStatus() const
{
    QMutexLocker lock(&d->shared->mutex);
    return d->shared->transfer;
}

QString SshConnection::lastTerminalSize() const
{
    return QStringLiteral("%1 x %2").arg(d->cols).arg(d->rows);
}

// ---------------------------------------------------------------------------------------
// Environment helpers
// ---------------------------------------------------------------------------------------

QString SshConnection::defaultKnownHostsFile()
{
    return QDir::homePath() + QStringLiteral("/.ssh/known_hosts");
}

QStringList SshConnection::defaultIdentityFiles()
{
    QStringList files;
    const QString sshDir = QDir::homePath() + QStringLiteral("/.ssh/");
    for (const char* name : {"id_ed25519", "id_ecdsa", "id_rsa"}) {
        const QString path = sshDir + QLatin1String(name);
        if (QFileInfo(path).isFile()) {
            files.append(path);
        }
    }
    return files;
}

bool SshConnection::agentAvailable()
{
#if defined(Q_OS_WIN)
    // libssh compiles its agent client out on Windows (src/agent.c: #ifndef _WIN32).
    return false;
#else
    return !qEnvironmentVariableIsEmpty("SSH_AUTH_SOCK");
#endif
}

QString SshConnection::localUserName()
{
    for (const char* variable : {"USERNAME", "USER", "LOGNAME"}) {
        const QString value = qEnvironmentVariable(variable).trimmed();
        if (!value.isEmpty()) {
            return value;
        }
    }
    return QDir::home().dirName();
}

QString SshConnection::libraryVersion()
{
    return QStringLiteral("libssh ") + QString::fromLatin1(ssh_version(0));
}

// ---------------------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------------------

bool SshConnection::open()
{
    if (state() != State::Disconnected) {
        qCWarning(lcSsh) << "open() ignored while" << stateText(state());
        return false;
    }
    SshProfile profile = d->profile;
    profile.host = profile.host.trimmed();
    if (profile.host.isEmpty()) {
        const QString message = tr("No SSH host given");
        setErrorString(message);
        qCWarning(lcSsh) << message;
        emit errorOccurred(message);
        return false;
    }
    if (profile.port == 0) {
        const QString message = tr("Invalid SSH port for %1").arg(profile.host);
        setErrorString(message);
        qCWarning(lcSsh) << message;
        emit errorOccurred(message);
        return false;
    }
    if (d->workerBusy) {
        d->abortWorker();
    }
    setErrorString(QString());
    {
        QMutexLocker lock(&d->shared->mutex);
        d->shared->serverVersion.clear();
        d->shared->hostKeyType.clear();
        d->shared->hostKeyFingerprint.clear();
        d->shared->authMethod.clear();
        d->shared->remoteHome.clear();
        d->shared->exitStatus = -1;
        d->shared->transfer = TransferStatus();
    }

    SshWorker::OpenRequest request;
    request.generation = ++d->generation;
    request.profile = profile;
    request.cols = d->cols;
    request.rows = d->rows;
    // Remembered credentials (SecretStore is GUI-thread only, so they travel with the request;
    // the key scheme is documented in SshWorker.h): the profile-id entry first, then the
    // target / key-file entry.
    appendSavedSecret(&request.savedPasswords, SshWorker::profilePasswordKey(profile));
    appendSavedSecret(&request.savedPasswords, SshWorker::targetPasswordKey(profile));
    QStringList keyFiles;
    const QString identity = profile.identityFile.trimmed();
    if (!identity.isEmpty()) {
        keyFiles.append(identity);
    }
    if (profile.auth == SshProfile::Auth::Auto || (profile.auth == SshProfile::Auth::PublicKey && identity.isEmpty())) {
        keyFiles.append(defaultIdentityFiles());
    }
    for (const QString& keyFile : std::as_const(keyFiles)) {
        const QString normalized = SshWorker::normalizedKeyPath(keyFile);
        if (normalized.isEmpty() || request.savedPassphrases.contains(normalized)) {
            continue;
        }
        QList<SshWorker::SavedSecret> candidates;
        if (keyFile == identity) {
            appendSavedSecret(&candidates, SshWorker::profilePassphraseKey(profile));
        }
        appendSavedSecret(&candidates, SshWorker::keyPassphraseKey(keyFile));
        if (!candidates.isEmpty()) {
            request.savedPassphrases.insert(normalized, candidates);
        }
    }
    d->workerBusy = true;
    qCInfo(lcSsh) << "opening" << profile.displayTarget();
    setState(State::Connecting);
    SshWorker* worker = d->worker;
    QMetaObject::invokeMethod(worker, [worker, request] { worker->open(request); }, Qt::QueuedConnection);
    return true;
}

void SshConnection::close()
{
    if (state() == State::Disconnected && !d->workerBusy) {
        return;
    }
    qCInfo(lcSsh) << "closing" << displayName();
    d->abortWorker();
    bool transferWasActive = false;
    {
        QMutexLocker lock(&d->shared->mutex);
        transferWasActive = d->shared->transfer.active;
        d->shared->transfer.active = false;
    }
    if (transferWasActive) {
        // The worker ends the transfer (and removes an incomplete upload) for the generation
        // that was just closed, so its report is dropped by the relay: tell the listeners here,
        // before the state change, so a dialog can keep the reason next to "Not connected".
        emit transferFinished(false, tr("Transfer aborted: the connection was closed"));
    }
    setState(State::Disconnected);
}

qint64 SshConnection::write(const QByteArray& data)
{
    if (state() != State::Connected) {
        return -1;
    }
    if (data.isEmpty()) {
        return 0;
    }
    d->worker->enqueueOutgoing(data);
    QMetaObject::invokeMethod(d->worker, &SshWorker::flushOutgoing, Qt::QueuedConnection);
    emit dataSent(data);
    countSent(data.size());   // emits countersChanged()
    return data.size();
}

void SshConnection::notifyTerminalSize(int cols, int rows)
{
    if (cols <= 0 || rows <= 0) {
        return;
    }
    d->cols = cols;
    d->rows = rows;
    if (state() != State::Disconnected) {
        SshWorker* worker = d->worker;
        QMetaObject::invokeMethod(worker, [worker, cols, rows] { worker->resize(cols, rows); }, Qt::QueuedConnection);
    }
}

void SshConnection::answerHostKey(bool accept, bool remember)
{
    qCInfo(lcSsh) << "host key" << (accept ? "accepted" : "rejected") << (remember ? "(remember)" : "(once)");
    d->worker->deliverHostKeyAnswer(accept, remember);
}

void SshConnection::answerPrompt(const QString& response, bool remember)
{
    // "Remember" is valid for every prompt: the worker picks the SecretStore key (profile id,
    // target or key file) and reports it through storeSecret() once the credential worked.
    d->worker->deliverPromptAnswer(response, remember);
}

void SshConnection::cancelPrompt()
{
    qCInfo(lcSsh) << "prompt cancelled";
    d->worker->cancelPending();
}

bool SshConnection::startTransfer(const SshConnection::TransferRequest& request)
{
    if (state() != State::Connected) {
        qCWarning(lcSsh) << "startTransfer: not connected";
        return false;
    }
    if (request.localPath.trimmed().isEmpty() || request.remotePath.trimmed().isEmpty()) {
        qCWarning(lcSsh) << "startTransfer: local and remote path are required";
        return false;
    }
    {
        QMutexLocker lock(&d->shared->mutex);
        if (d->shared->transfer.active) {
            qCWarning(lcSsh) << "startTransfer: a transfer is already running";
            return false;
        }
        d->shared->transfer = TransferStatus();
        d->shared->transfer.active = true;
        d->shared->transfer.direction = request.direction;
        d->shared->transfer.localPath = request.localPath;
        d->shared->transfer.remotePath = request.remotePath;
    }
    SshWorker* worker = d->worker;
    QMetaObject::invokeMethod(worker, [worker, request] { worker->startTransfer(request); }, Qt::QueuedConnection);
    return true;
}

void SshConnection::cancelTransfer()
{
    QMetaObject::invokeMethod(d->worker, &SshWorker::cancelTransfer, Qt::QueuedConnection);
}

void SshConnection::requestRemoteHome()
{
    if (state() != State::Connected) {
        qCWarning(lcSsh) << "requestRemoteHome: not connected";
        return;
    }
    QMetaObject::invokeMethod(d->worker, &SshWorker::requestRemoteHome, Qt::QueuedConnection);
}
