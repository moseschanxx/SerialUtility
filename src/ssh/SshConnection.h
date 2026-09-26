#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QMetaType>
#include <memory>

#include "core/Transport.h"
#include "ssh/SshProfile.h"

class QThread;
class SshWorker;

/**
 * SSH shell session as a Transport (libssh, OpenSSL backend).
 *
 * Threading: all libssh calls run on a private worker thread (SshWorker, an implementation
 * detail in SshConnection.cpp). This class is the GUI-thread facade: every public method is
 * safe to call from the GUI thread, every signal is emitted on the GUI thread. Questions the
 * worker cannot answer on its own (unknown host key, password / passphrase / keyboard-
 * interactive prompts) are raised as signals; the worker waits (up to 5 minutes) until the
 * matching answer slot is called, so the session stays cancellable at all times (close()).
 *
 * Connect sequence (open()):
 *  1. Validate profile() (host, port, user defaulting to localUserName()). State -> Connecting.
 *  2. TCP + key exchange with connectTimeoutSeconds. Server version -> serverVersion().
 *  3. Host key check against knownHostsFile (default ~/.ssh/known_hosts, OpenSSH format,
 *     shared with the system ssh):
 *       Known            -> continue silently
 *       Unknown          -> hostKeyVerificationRequired(info) ; answerHostKey(true, remember)
 *                           appends the key when remember is set
 *       Changed          -> hostKeyVerificationRequired(info) with status Changed (the UI
 *                           shows the "REMOTE HOST IDENTIFICATION HAS CHANGED" warning);
 *                           accept+remember replaces the stored line(s) for that host
 *       KeyTypeChanged   -> like Unknown but says the server offers a new key type
 *       Error            -> known_hosts unreadable: treated like Unknown, with message
 *     A rejected key -> state Disconnected, errorOccurred("Host key rejected").
 *  4. Authentication in the order dictated by profile().auth (Auto = agent if available,
 *     then the profile's identityFile, then defaultIdentityFiles() that exist, then
 *     password, then keyboard-interactive), restricted to the methods the server offers.
 *     Encrypted keys raise authPromptRequired(Passphrase); passwords raise
 *     authPromptRequired(Password) unless SecretStore holds one for the profile id (a saved
 *     secret that fails is discarded and the prompt shown, attempt 2..3); keyboard-interactive
 *     raises one prompt per server question. answerPrompt(response, remember) stores the
 *     secret in SecretStore when remember is true (requires a profile id). cancelPrompt() aborts
 *     the connect. authenticated(method) reports the winning method ("publickey", "password",
 *     "keyboard-interactive", "agent").
 *  5. Channel: PTY request (terminalType, the size from the last notifyTerminalSize() or 80x24),
 *     then "shell" (or "exec" with remoteCommand). shellStarted(); state -> Connected;
 *     dataReceived() carries everything the server sends (stdout + stderr merged).
 *     startupCommand, when set, is written 300 ms after shellStarted() followed by "\r".
 *     Local forwards from the profile are set up after authentication (a QTcpServer per
 *     forward owned by the worker thread; each accepted socket is bridged to a
 *     direct-tcpip channel); forwardListening()/forwardFailed() per forward.
 *  6. Keep-alive: keepAliveSeconds > 0 sends an SSH_MSG_IGNORE every interval; 3 consecutive
 *     failures count as a dropped link.
 *  7. End: a clean shell exit (EOF with exit status) -> channelClosed(status), state
 *     Disconnected, NO reconnect. A dropped link (socket error, timeout, keep-alive failure)
 *     -> connectionLost(); when autoReconnect() is on the worker retries with backoff
 *     (2 s, 4 s, ... capped at 30 s) using only non-interactive credentials (agent, keys
 *     without passphrase or with a remembered one, saved password); if a prompt would be
 *     needed the reconnect stops with state Disconnected and an errorOccurred() explaining
 *     why. connectionRestored() after success. close() always stops everything.
 *
 * File transfer (SFTP subsystem on the same session, one transfer at a time, run inside the
 * worker's I/O loop in 64 KiB chunks so the shell keeps working): startTransfer() ->
 * transferStarted(), transferProgress() (throttled to ~10 Hz), transferFinished(ok, message).
 * Upload keeps the local permission bits when preservePermissions is set; download writes to
 * "<localPath>.part" and renames on success. cancelTransfer() aborts and removes a partial
 * download. requestRemoteHome() resolves "." through SFTP -> remoteHomeReceived("/root").
 *
 * write() while Connected sends to the channel (never blocks; returns size); while
 * Connecting/Reconnecting/Disconnected returns -1. notifyTerminalSize() sends a window-change
 * request when a PTY is open and remembers the size for the next open.
 *
 * settingsMap(): {"kind":"ssh","profileId":..., "target": "user@host:port"} for session restore.
 */
class SshConnection : public Transport
{
    Q_OBJECT
public:
    enum class HostKeyStatus { Known, Unknown, Changed, KeyTypeChanged, Error };
    Q_ENUM(HostKeyStatus)

    struct HostKeyInfo
    {
        QString host;
        quint16 port = 22;
        QString keyType;                ///< "ssh-ed25519", "ecdsa-sha2-nistp256", "rsa-sha2-512"...
        QString fingerprintSha256;      ///< "SHA256:..." (OpenSSH format)
        QString fingerprintMd5;         ///< "MD5:aa:bb:..."
        QString publicKeyLine;          ///< "<type> <base64>" as stored in known_hosts
        HostKeyStatus status = HostKeyStatus::Unknown;
        QString knownHostsFile;
        QString message;                ///< extra explanation (e.g. why known_hosts could not be read)
    };

    enum class PromptKind { Password, Passphrase, KeyboardInteractive };
    Q_ENUM(PromptKind)

    struct AuthPrompt
    {
        PromptKind kind = PromptKind::Password;
        QString title;                  ///< dialog title
        QString instruction;            ///< keyboard-interactive instruction text (may be empty)
        QString prompt;                 ///< "Password:" / "Enter passphrase for key '...':" / server prompt
        bool echo = false;              ///< show typed characters
        int attempt = 1;                ///< 1..3
        QString user;
        QString host;
        QString keyFile;                ///< Passphrase prompts only
        bool canRemember = false;       ///< true when the profile has an id (SecretStore possible)
    };

    enum class TransferDirection { Upload, Download };
    Q_ENUM(TransferDirection)

    struct TransferRequest
    {
        TransferDirection direction = TransferDirection::Upload;
        QString localPath;
        QString remotePath;
        bool overwrite = true;
        bool preservePermissions = true;
    };

    struct TransferStatus
    {
        bool active = false;
        TransferDirection direction = TransferDirection::Upload;
        QString localPath;
        QString remotePath;
        qint64 done = 0;
        qint64 total = -1;              ///< -1 when unknown
    };

    explicit SshConnection(QObject* parent = nullptr);
    ~SshConnection() override;

    Kind kind() const override;
    QString displayName() const override;      ///< profile().displayName()
    QString summary() const override;          ///< "ssh-ed25519 · publickey" once connected, else "SSH"
    QVariantMap settingsMap() const override;

    SshProfile profile() const;
    void setProfile(const SshProfile& profile);   ///< ignored (logged) while not Disconnected

    // ---- Facts about the current / last connection -----------------------------------
    QString serverVersion() const;             ///< "SSH-2.0-OpenSSH_9.6"
    QString hostKeyType() const;
    QString hostKeyFingerprint() const;        ///< "SHA256:..."
    QString authMethod() const;
    QString remoteHome() const;                ///< cached result of requestRemoteHome()
    int exitStatus() const;                    ///< last shell exit status, -1 when unknown
    bool isTransferActive() const;
    TransferStatus transferStatus() const;
    QString lastTerminalSize() const;          ///< "cols x rows" as last notified

    // ---- Environment helpers -----------------------------------------------------------
    static QString defaultKnownHostsFile();    ///< <home>/.ssh/known_hosts
    static QStringList defaultIdentityFiles(); ///< existing ones among <home>/.ssh/id_ed25519, id_ecdsa, id_rsa
    static bool agentAvailable();              ///< SSH_AUTH_SOCK set (POSIX) or the OpenSSH agent pipe exists (Windows)
    static QString localUserName();
    static QString libraryVersion();           ///< "libssh 0.11.1/openssl"

public slots:
    bool open() override;
    void close() override;
    qint64 write(const QByteArray& data) override;
    void notifyTerminalSize(int cols, int rows) override;

    void answerHostKey(bool accept, bool remember);
    void answerPrompt(const QString& response, bool remember);
    void cancelPrompt();

    bool startTransfer(const SshConnection::TransferRequest& request);   ///< false when busy / not connected
    void cancelTransfer();
    void requestRemoteHome();

signals:
    void hostKeyVerificationRequired(const SshConnection::HostKeyInfo& info);
    void authPromptRequired(const SshConnection::AuthPrompt& prompt);
    void authenticated(const QString& method);
    void bannerReceived(const QString& text);       ///< pre-authentication banner (shown dim in the terminal)
    void shellStarted();
    void channelClosed(int exitStatus);
    void transferStarted(const SshConnection::TransferRequest& request);
    void transferProgress(qint64 done, qint64 total);
    void transferFinished(bool ok, const QString& message);
    void remoteHomeReceived(const QString& path);
    void forwardListening(quint16 localPort, const QString& remoteHost, quint16 remotePort);
    void forwardFailed(quint16 localPort, const QString& message);

private:
    friend class SshWorker;
    struct Private;
    std::unique_ptr<Private> d;
};

Q_DECLARE_METATYPE(SshConnection::HostKeyInfo)
Q_DECLARE_METATYPE(SshConnection::AuthPrompt)
Q_DECLARE_METATYPE(SshConnection::TransferRequest)
Q_DECLARE_METATYPE(SshConnection::TransferStatus)
