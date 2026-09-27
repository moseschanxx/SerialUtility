#pragma once

#include <QObject>
#include <QString>
#include <QByteArray>
#include <QSize>
#include <memory>

/**
 * In-process SSH server for the test suites (libssh server API), so SshConnection and the
 * SSH session UI are tested end to end on every platform without a real sshd.
 *
 *  - Listens on 127.0.0.1 (port 0 = ephemeral; port() after start()).
 *  - Host key: an ed25519 key generated per instance (regenerateHostKey() makes a new one to
 *    simulate a changed server identity). knownHostsLine() is the OpenSSH known_hosts entry.
 *  - Authentication per Options: password (user/password), public key (authorizedPublicKey,
 *    one "<type> <base64>" line; the probe and the signed request both compare the key),
 *    keyboard-interactive (one prompt kbdintPrompt, name "TestSshServer", empty instruction,
 *    whose only accepted answer is kbdintAnswer). "none" is always rejected but reports the
 *    enabled methods. Optional pre-auth banner (SSH_MSG_USERAUTH_BANNER, sent when the client
 *    requests the ssh-userauth service; libssh clients read it with ssh_get_issue_banner()).
 *  - Session channel: pty-req (records term + size; window-change updates lastPtySize()),
 *    env, shell, exec and the "sftp" subsystem. The shell is a small in-process program
 *    (no fork): it prints "welcome\r\n$ ", echoes typed bytes (0x7F/0x08 erase with "\b \b"),
 *    and on "\r" (a bare "\n" too; the "\n" of a CR LF pair is ignored) echoes "\r\n" and
 *    runs the line: "exit [n]" sends that exit status (default 0), EOF and close,
 *    "env" prints "TERM=<term> COLS=<c> ROWS=<r>", "size" prints "COLS=<c> ROWS=<r>",
 *    "echo <text>" prints <text>, "big <n>" prints n lines "line 1" .. "line n",
 *    "sleep <ms>" waits that long before the next prompt, "hang" stops responding for good
 *    (input is still recorded; for keep-alive / drop tests), anything else prints
 *    "<cmd>: not found". Every line ends in "\r\n" and is followed by the prompt "$ ".
 *    Every byte received from the client is appended to receivedShellInput(). exec runs the
 *    same command interpreter once (no welcome, no prompt) and exits with status 0, or n
 *    for "exit n".
 *  - exec file commands (what SshConnection's non-SFTP transfer fallback needs; paths are
 *    single-quoted shell words - the '\'' idiom works - resolved relative to Options::rootDir
 *    (absolute paths are used as given, like the SFTP handler), ".." refused, exit status 1 on
 *    error with a message on stderr): `cat > 'p'` (stdin until EOF -> file, exit 0; with
 *    Options::execUploadLimit >= 0 only that many bytes reach the file, the rest is dropped and
 *    the command ends with "cat: write error: No space left on device" and exit status 1, like
 *    a full flash), `: > 'p'` (create / truncate, what the fallback runs before `cat >` to learn
 *    whether the file can be written at all), `cat 'p'` (file -> stdout, streamed in 64 KiB
 *    pieces as the window allows), `wc -c < 'p'` (size + LF), `test -e 'p'` (exit 0 / 1),
 *    `chmod NNN 'p'`, `rm -f 'p'`, `mkdir -p 'p'`, `mv 'a' 'b'`, `pwd` (the canonical rootDir +
 *    LF, what the fallback's remote-home lookup runs); commands may be joined with " && " (stop
 *    at the first failure, whose status is the exit status). Any other first word runs the
 *    scripted shell command once, as before. Options::sendExecExitStatus = false ends the data
 *    commands `cat > 'p'` and `cat 'p'` with EOF and close but no exit-status message (a
 *    server whose status for the long command is late or never comes; the fallback then
 *    verifies an upload's size with `wc -c`) - the short helper commands keep their status.
 *    Options::allowSftp = false refuses the sftp subsystem (sftpRequested() still fires).
 *  - SFTP: a small built-in SFTP v3 server (tests/support/TestSftpHandler.h) serving the
 *    real file system: relative paths, "." and "~" resolve against Options::rootDir (a
 *    temporary directory the tests keep their files in), absolute paths are used as given.
 *    libssh's own default server (sftp_channel_default_subsystem_request) is compiled out on
 *    Windows, so it is not used on any platform. sftpRequested() fires per "sftp" subsystem
 *    request.
 *  - direct-tcpip channels (local forwards): connected to the requested host:port with a plain
 *    socket and bridged; only 127.0.0.1 / localhost targets are allowed (forwardRequested()
 *    fires for every request, refused ones included).
 *  - dropAllClients() shuts every client socket down abruptly (a FIN with no
 *    SSH_MSG_DISCONNECT, i.e. a simulated network cut). setAcceptConnections(false) keeps the
 *    port open but resets (RST) every new connection right after the TCP accept, so a client's
 *    connect fails at once on every platform (libssh's select-based poll emulation on Windows
 *    does not notice a refused TCP connect before SSH_OPTIONS_TIMEOUT expires; a closed
 *    listener would cost that timeout per attempt); (true) accepts again (reconnect tests).
 *
 * Threading: the accept loop and every client run on their own threads; all getters are
 * mutex-protected, signals are emitted from those threads (queued to receivers living in
 * the test thread). stop() joins everything.
 */
class TestSshServer : public QObject
{
    Q_OBJECT
public:
    struct Options
    {
        QString user = QStringLiteral("test");
        QString password = QStringLiteral("secret");
        QString authorizedPublicKey;       ///< "<type> <base64>" accepted for `user`; empty = no pubkey auth
        bool allowPassword = true;
        bool allowPublicKey = true;
        bool allowKeyboardInteractive = false;
        QString kbdintPrompt = QStringLiteral("Token: ");
        QString kbdintAnswer = QStringLiteral("424242");
        QString banner;                    ///< pre-auth banner text (empty = none)
        QString rootDir;                   ///< SFTP root (must exist); also the working directory of the exec file commands
        /// false = the "sftp" subsystem request is refused (a dropbear-like server), so clients
        /// must fall back to the exec channel commands (see the class comment).
        bool allowSftp = true;
        /// false = the data commands (`cat > 'p'`, `cat 'p'`) end without an exit-status message
        /// (EOF + close only); the helper commands keep theirs (see the class comment).
        bool sendExecExitStatus = true;
        /// >= 0: bytes a `cat > 'p'` upload may store before "No space left on device" (see above).
        qint64 execUploadLimit = -1;
        quint16 port = 0;                  ///< 0 = ephemeral
        /// >0: seconds of idle after which the server stops answering (keep-alive tests).
        /// libssh gives a server no hook for SSH_MSG_IGNORE, so keep-alives cannot be counted:
        /// once an authenticated session has seen no channel traffic from the client (requests
        /// or data) for this many seconds it is no longer serviced at all - not polled, nothing
        /// read or written, TCP socket left open - so a keep-alive that expects an answer
        /// (global request with want_reply) times out; SSH_MSG_IGNORE alone gets no reaction.
        int keepAliveDropAfter = 0;
    };

    explicit TestSshServer(const Options& options, QObject* parent = nullptr);
    ~TestSshServer() override;

    bool start(QString* error = nullptr);
    void stop();
    bool isRunning() const;

    quint16 port() const;
    QString hostKeyType() const;                 ///< "ssh-ed25519"
    QString hostKeyFingerprintSha256() const;    ///< "SHA256:..."
    QString hostKeyPublicLine() const;           ///< "ssh-ed25519 AAAA..."
    QString knownHostsLine() const;              ///< "[127.0.0.1]:<port> ssh-ed25519 AAAA..."
    void regenerateHostKey();                    ///< only while stopped

    int connectionCount() const;                 ///< total accepted since start()
    int activeConnections() const;
    QString lastAuthMethod() const;
    QString lastTerm() const;
    QSize lastPtySize() const;
    QByteArray receivedShellInput() const;
    QString lastExecCommand() const;
    void dropAllClients();
    void setAcceptConnections(bool accept);

    /// Generate an ed25519 key pair; returns false on failure. `publicKeyLine` gets "<type> <base64>".
    static bool generateClientKeyPair(const QString& privateKeyPath, QString* publicKeyLine,
                                      const QString& passphrase = QString());

signals:
    void clientConnected();
    void clientAuthenticated(const QString& method);
    void clientDisconnected();
    void shellRequested();
    void execRequested(const QString& command);
    void sftpRequested();
    void forwardRequested(const QString& host, quint16 port);

private:
    struct Private;
    std::unique_ptr<Private> d;
};
