#pragma once

#include <QObject>
#include <QByteArray>
#include <QList>
#include <QMutex>
#include <QString>
#include <QTimer>
#include <QWaitCondition>

#include <atomic>
#include <memory>

#include "ssh/SshConnection.h"

/**
 * Facts about the current / last SSH connection, written by the worker thread and read by the
 * SshConnection getters on the GUI thread. Every access is under `mutex`.
 */
struct SshSharedState
{
    mutable QMutex mutex;
    QString serverVersion;
    QString hostKeyType;
    QString hostKeyFingerprint;
    QString authMethod;
    QString remoteHome;
    int exitStatus = -1;
    SshConnection::TransferStatus transfer;
};

/**
 * The libssh side of SshConnection (implementation detail; SshConnection.h is the behaviour
 * contract). One SshWorker lives on a private QThread with an event loop; the GUI-thread facade
 * posts every request as a queued invocation and receives results through the signals below,
 * each tagged with the generation of the open() that produced it so answers that arrive after a
 * close() are dropped by the facade.
 *
 *  - open(): the whole connect sequence (TCP, key exchange, host key check, authentication,
 *    channel + PTY + shell) runs inside this one slot. Questions for the user are emitted as
 *    signals while the worker waits on a QWaitCondition; deliverHostKeyAnswer() /
 *    deliverPromptAnswer() / cancelPending() / requestAbort() wake it (5 minute timeout).
 *  - After the shell started a 10 ms tick reads stdout/stderr without blocking (up to 1 MiB per
 *    tick; an exit status ends the shell only once the read loop ran dry, so a tail buffered
 *    behind it is never lost), drains the outgoing queue (a short ssh_channel_write() caused by
 *    a closed remote window re-queues the rest instead of dropping it), bridges the
 *    local-forward sockets and runs the active SFTP transfer in slices; a second timer sends
 *    the keep-alive.
 *  - Local forwards: one QTcpServer per profile forward, bound to the loopback address for an
 *    empty / "localhost" bind address and to a literal IP address otherwise (a host name is
 *    reported through forwardFailed()). Each accepted socket and its direct-tcpip channel form
 *    a Bridge owned by the listener; only closeBridge() releases the socket, so the tick never
 *    touches a deleted one.
 *  - The environment variable SU_SSH_IGNORE_CONFIG=1 skips ~/.ssh/config and the global
 *    ssh_config (used by the test suites); SU_SSH_LOG_VERBOSITY=1..4 turns on libssh logging.
 *  - A dropped link tears the session down and emits linkDropped(); the facade decides (with
 *    Transport::autoReconnect()) whether to call beginReconnect(), after which the worker retries
 *    with 2/4/8..30 s backoff using only the credentials that worked during the first connect
 *    until it succeeds, hits a hard failure, or close() arrives.
 *  - close() ends everything for the given generation.
 */
class SshWorker : public QObject
{
    Q_OBJECT
public:
    struct OpenRequest
    {
        quint64 generation = 0;
        SshProfile profile;
        QString savedPassword;      ///< SecretStore "ssh/<id>/password" (empty = none)
        QString savedPassphrase;    ///< SecretStore "ssh/<id>/passphrase" (empty = none)
        int cols = 80;
        int rows = 24;
    };

    explicit SshWorker(std::shared_ptr<SshSharedState> shared, QObject* parent = nullptr);
    ~SshWorker() override;

    // ---- Thread-safe entry points (called from the GUI thread while a slot may be blocking) ----
    void requestAbort(quint64 generation);                 ///< close(): abort the blocking connect phase of `generation`
    void deliverHostKeyAnswer(bool accept, bool remember);
    void deliverPromptAnswer(const QString& response, bool remember);
    void cancelPending();                                  ///< cancelPrompt(): abort the connect
    void enqueueOutgoing(const QByteArray& data);          ///< drained by the tick / flushOutgoing()
    qint64 pendingOutgoingBytes() const;

public slots:
    // Every slot runs on the worker thread (queued from SshConnection).
    void open(const SshWorker::OpenRequest& request);
    void close(quint64 generation);
    void beginReconnect(quint64 generation);
    void flushOutgoing();
    void resize(int cols, int rows);
    void startTransfer(const SshConnection::TransferRequest& request);
    void cancelTransfer();
    void requestRemoteHome();

signals:
    void stateChanged(quint64 generation, Transport::State state);
    void errorOccurred(quint64 generation, const QString& message);
    void dataReceived(quint64 generation, const QByteArray& data);
    void txBytesWritten(quint64 generation, qint64 bytes);
    void hostKeyVerificationRequired(quint64 generation, const SshConnection::HostKeyInfo& info);
    void authPromptRequired(quint64 generation, const SshConnection::AuthPrompt& prompt);
    void authenticated(quint64 generation, const QString& method);
    void bannerReceived(quint64 generation, const QString& text);
    void shellStarted(quint64 generation);
    void channelClosed(quint64 generation, int exitStatus);
    /// The link dropped unexpectedly; the session is already torn down. The facade answers with
    /// beginReconnect() (autoReconnect on) or close().
    void linkDropped(quint64 generation, const QString& reason);
    void connectionRestored(quint64 generation, const QString& name);
    void transferStarted(quint64 generation, const SshConnection::TransferRequest& request);
    void transferProgress(quint64 generation, qint64 done, qint64 total);
    void transferFinished(quint64 generation, bool ok, const QString& message);
    void remoteHomeReceived(quint64 generation, const QString& path);
    void forwardListening(quint64 generation, quint16 localPort, const QString& remoteHost, quint16 remotePort);
    void forwardFailed(quint64 generation, quint16 localPort, const QString& message);
    /// A credential entered with "remember" worked; the facade stores it in SecretStore.
    void storeSecret(quint64 generation, const QString& secretKey, const QString& value);
    /// A saved secret was rejected by the server; the facade removes it from SecretStore.
    void secretRejected(quint64 generation, const QString& secretKey);

private:
    struct Live;             // libssh handles, forwards, transfer state (SshWorker.cpp)
    struct Pending;          // the question currently waiting for the GUI
    struct Bridge;
    struct ForwardListener;
    struct Transfer;

    enum class Step {
        Ok,
        Denied,              ///< this authentication method did not work; try the next one
        Aborted,             ///< close() arrived
        Cancelled,           ///< the user rejected the host key / cancelled a prompt / no answer in time
        Failed,              ///< hard failure (message in *error)
        NetworkFailed,       ///< TCP / transport failure (a reconnect may retry)
        NeedsInteraction     ///< a reconnect would have to ask the user
    };

    void runConnect(bool reconnect);
    Step connectSequence(bool reconnect, QString* error);
    bool applySessionOptions(QString* error);
    Step verifyHostKey(bool reconnect, QString* error);
    Step authenticate(bool reconnect, QString* error);
    Step authenticateAgent();
    Step authenticateWithKey(const QString& keyFile, bool reconnect, QString* error);
    Step authenticateWithPassword(bool reconnect, QString* error);
    Step authenticateInteractive(bool reconnect, QString* error);
    Step openShellChannel(QString* error);
    Step methodError(QString* error);
    void setAuthMethod(const QString& method);
    SshConnection::AuthPrompt makePrompt(SshConnection::PromptKind kind, int attempt) const;

    void setupForwards();
    void acceptForward(ForwardListener* listener);
    void closeBridge(Bridge& bridge);
    void teardownForwards();
    void teardownLive();
    void stopTimers();
    void handleConnectionLost(const QString& reason);
    void scheduleReconnect();
    void onReconnectTimer();
    void finishShell();

    Step waitForAnswer(QString* error);
    Step askHostKey(const SshConnection::HostKeyInfo& info, bool* accept, bool* remember, QString* error);
    Step askPrompt(const SshConnection::AuthPrompt& prompt, QString* response, bool* remember, QString* error);
    bool aborted() const;

    void tick();
    void pumpShell();
    void pumpForwards();
    void serviceTransfer();
    void completeTransfer();
    void finishTransfer(bool ok, const QString& message);
    void publishTransfer();
    void emitProgress(bool force);
    bool ensureSftp(QString* error);
    void sendKeepAlive();

    QString target() const;                ///< "user@host:port" for messages
    QString displayName() const;
    QString libsshError() const;
    QString sftpError() const;
    QString passwordSecretKey() const;
    QString passphraseSecretKey() const;

    std::shared_ptr<SshSharedState> m_shared;
    std::unique_ptr<Live> m_live;
    std::unique_ptr<Pending> m_pending;
    OpenRequest m_request;
    quint64 m_generation = 0;
    std::atomic<quint64> m_abortGeneration{0};

    // Credentials that worked during this open(); reused by reconnects only.
    QString m_password;
    QString m_passphrase;
    QString m_acceptedKeyLine;    ///< host key the user accepted (also without "remember")

    mutable QMutex m_outgoingMutex;
    QList<QByteArray> m_outgoing;
    qint64 m_outgoingBytes = 0;
    QByteArray m_readBuffer;

    QTimer* m_tickTimer = nullptr;
    QTimer* m_keepAliveTimer = nullptr;
    QTimer* m_reconnectTimer = nullptr;
    int m_reconnectDelaySeconds = 0;
    int m_keepAliveFailures = 0;
    int m_cols = 80;
    int m_rows = 24;
    bool m_inTick = false;
};
