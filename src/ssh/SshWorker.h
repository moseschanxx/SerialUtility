#pragma once

#include <QObject>
#include <QByteArray>
#include <QHash>
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
 *    local-forward sockets and runs the active file transfer in slices; a second timer sends
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
 *
 * Remembered credentials (SecretStore keys; the store itself is GUI-thread only, so the facade
 * loads the candidates into the OpenRequest and stores / removes on the storeSecret() /
 * secretRejected() signals):
 *    "ssh/<profile id>/password"                  password or keyboard-interactive answer of a
 *                                                 stored profile
 *    "ssh/target/<user@host:port>/password"       the same for an ad-hoc target (no profile id;
 *                                                 <target> is SshProfile::displayTarget())
 *    "ssh/<profile id>/passphrase"                passphrase of the profile's identity file
 *    "ssh/key/<absolute key path>/passphrase"     passphrase of that key file, whatever the target
 *                                                 (<path> is normalizedKeyPath())
 * Lookup order when authenticating: the profile-id secret, then the target / key secret; a stored
 * secret the server (or the key file) rejects is removed - that key only - and the user is
 * prompted. A prompt answered with "remember" is stored under the id key when the profile has
 * an id and under the target key otherwise; a passphrase is stored under the key-path key as well
 * (for profiles and ad-hoc targets alike), so a key needs its passphrase only once. Secret
 * values are never logged. Reconnects reuse the credentials that worked during the connect.
 *
 * File transfer (SshConnection.h has the user-visible contract): the SFTP subsystem is probed
 * once per session; when it is refused (dropbear without sftp-server) or fails to initialise,
 * transfers run through exec channels driving the remote shell tools (`: > 'p'` then
 * `cat > 'p'` for an upload, `cat 'p'`, `wc -c < 'p'`, `test -e 'p'`, `chmod NNN 'p'`,
 * `rm -f 'p'`, `pwd`). The data command runs in
 * slices inside the tick like SFTP does (64 KiB reads / writes, at most 1 MiB or 50 ms per
 * tick); the short helper commands run synchronously with a bounded wait. Every path is
 * single-quoted with '\'' escaping and a relative path starting with '-' gets "./" in front
 * (shellPath(): the utilities would read it as an option, and `cat -` as stdin). The `: > 'p'`
 * probe runs before the data command so a file that cannot be created or truncated fails with
 * the shell's reason and is never touched; once it succeeded the file is the transfer's, and
 * finishTransfer() removes it (`rm -f`, ExecMode::Cleanup) whenever the upload does not
 * complete - cancel, a write or read error, a non-zero exit status such as "No space left on
 * device", close() - on a link that is still alive. An upload whose exec channel ends without
 * an exit status (the deadline after EOF passed, or the channel closed without one) is
 * confirmed by `wc -c < 'p'` against the local size: only a matching size is a success, since
 * Transfer::done counts bytes the channel accepted, not bytes cat wrote. SFTP keeps up to 16 requests in flight
 * (libssh's sftp_aio_begin_read / _write, replies collected oldest first) so a transfer is not
 * bounded by one round trip per 64 KiB; a short read reply drops the requests queued behind it
 * and continues from the byte after it, EOF ends the download once every reply is collected,
 * and a cancel or failure collects (live link) or frees (dead link) the outstanding requests
 * before the remote handle is closed. Transfer progress counts acknowledged bytes.
 */
class SshWorker : public QObject
{
    Q_OBJECT
public:
    /// One SecretStore entry loaded by the facade: the key it lives under (reported back through
    /// secretRejected() when the server refuses it) and the secret itself.
    struct SavedSecret
    {
        QString key;
        QString value;
    };

    struct OpenRequest
    {
        quint64 generation = 0;
        SshProfile profile;
        /// Saved passwords to try before prompting, in lookup order: the profile's
        /// "ssh/<id>/password", then "ssh/target/<target>/password" - only entries that exist.
        QList<SavedSecret> savedPasswords;
        /// Saved passphrases per normalizedKeyPath(), each list in lookup order: the profile's
        /// "ssh/<id>/passphrase" (for its identity file only), then "ssh/key/<path>/passphrase".
        QHash<QString, QList<SavedSecret>> savedPassphrases;
        int cols = 80;
        int rows = 24;
    };

    explicit SshWorker(std::shared_ptr<SshSharedState> shared, QObject* parent = nullptr);
    ~SshWorker() override;

    // ---- SecretStore key scheme (see the class comment) ---------------------------------
    static QString normalizedKeyPath(const QString& keyFile);        ///< "~" expanded, absolute, cleaned
    static QString profilePasswordKey(const SshProfile& profile);    ///< "ssh/<id>/password", empty without an id
    static QString profilePassphraseKey(const SshProfile& profile);  ///< "ssh/<id>/passphrase", empty without an id
    static QString targetPasswordKey(const SshProfile& profile);     ///< "ssh/target/<displayTarget>/password"
    static QString keyPassphraseKey(const QString& keyFile);         ///< "ssh/key/<normalizedKeyPath>/passphrase"
    /// `'path'` with every embedded quote written as '\'' - the only way a path reaches a command line.
    static QString shellQuote(const QString& path);
    /// shellQuote() of the path as the remote utilities must see it: a relative path starting
    /// with '-' is prefixed with "./" so `cat`, `rm`, `chmod` do not parse it as an option (and
    /// `cat -` does not read stdin); every other path is quoted as it is.
    static QString shellPath(const QString& path);

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
    struct ExecRun;          // one exec channel of the shell fallback

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
    QString rememberPasswordKey() const;   ///< where a remembered password goes: the id key or the target key

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
    /// SFTP pipeline helpers (serviceTransfer): each returns false after finishTransfer() ran on
    /// an error. awaitSftpWrite() collects the oldest acknowledgement into Transfer::done;
    /// awaitSftpRead() the oldest data reply into m_readBuffer (*bytes = 0 at EOF, *requested =
    /// what that request asked for); discardSftpReplies() collects every reply still in flight
    /// and drops it.
    bool awaitSftpWrite();
    bool awaitSftpRead(qint64* bytes, qint64* requested);
    bool discardSftpReplies();
    void completeTransfer();
    /// Ends the transfer (both methods) and reports it: closes the remote handle / exec channel
    /// and the local file, removes the ".part" of a failed download and the remote file of a
    /// failed or cancelled upload (SFTP unlink / `rm -f`) on a live link, then emits
    /// transferFinished(). A cancel is a failure with "Transfer cancelled".
    void finishTransfer(bool ok, const QString& message);
    void publishTransfer();
    void emitProgress(bool force);
    bool probeSftp();                             ///< true when the session has a usable SFTP subsystem (probed once)
    void sendKeepAlive();

    // Shell fallback (exec channels)
    void startShellTransfer(const SshConnection::TransferRequest& request);
    void serviceShellTransfer();
    void completeShellTransfer();
    std::unique_ptr<ExecRun> startExec(const QString& command, QString* error);
    void closeExec(ExecRun& run);
    void pumpExecStderr(ExecRun& run);
    /// How runExec() behaves around the session's life: Normal helper commands stop waiting
    /// when an abort (close()) is requested and tear the session down on a dead link
    /// (handleConnectionLost); Cleanup - the `rm -f` of a broken upload, possibly issued from
    /// inside teardownLive() - runs despite an abort request, waits at most
    /// kCleanupExecTimeoutMs and only reports a dead link, never tears down.
    enum class ExecMode { Normal, Cleanup };
    /// Run `command` to its end (bounded wait): false with *error on a transport failure,
    /// otherwise *exitStatus (-1 when the server sent none), stdout in *output, stderr in *errorText.
    bool runExec(const QString& command, QByteArray* output, QString* errorText, int* exitStatus, QString* error,
                 ExecMode mode = ExecMode::Normal);
    QString execFailureText(const ExecRun& run) const;   ///< stderr text, or "exit status N"

    QString target() const;                ///< "user@host:port" for messages
    QString displayName() const;
    QString libsshError() const;
    QString sftpError() const;

    std::shared_ptr<SshSharedState> m_shared;
    std::unique_ptr<Live> m_live;
    std::unique_ptr<Pending> m_pending;
    OpenRequest m_request;
    quint64 m_generation = 0;
    std::atomic<quint64> m_abortGeneration{0};

    // Credentials that worked during this open(); reused by reconnects only.
    QString m_password;
    QHash<QString, QString> m_passphrases;   ///< normalizedKeyPath() -> passphrase that opened the key
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
