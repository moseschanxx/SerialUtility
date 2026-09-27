#include "support/TestSshServer.h"

// winsock2.h (through libssh.h) has to come before anything that could pull in windows.h.
#include "ssh/LibsshInclude.h"

#include <QDeadlineTimer>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSemaphore>
#include <QTcpServer>
#include <QThread>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <list>
#include <memory>
#include <utility>

#ifndef Q_OS_WIN
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "app/Logging.h"
#include "support/TestSftpHandler.h"

namespace {

constexpr int kPollMs = 50;                   ///< ssh_event_dopoll slice of every client thread
constexpr long kKeyExchangeTimeoutSeconds = 15;
constexpr int kMaxWriteChunk = 32 * 1024;     ///< one ssh_channel_write per tick and channel
constexpr int kMaxBigLines = 1000000;
constexpr int kForwardReadChunk = 16 * 1024;
constexpr qsizetype kStreamChunk = 64 * 1024;          ///< `cat 'p'`: file bytes read per refill
constexpr qsizetype kStreamHighWater = 256 * 1024;     ///< `cat 'p'`: refill the output queue below this

// ---- Exec file commands: shell words and paths -------------------------------------------------

/// Split a command line into commands (separated by an unquoted "&&") of shell words: single and
/// double quotes group (the '\'' idiom works since adjacent pieces join), a backslash escapes the
/// next character outside quotes, an unquoted ">" or "<" is a word of its own. False with *error
/// for an unterminated quote or a lone "&".
bool splitExecCommands(const QByteArray& line, QList<QStringList>* commands, QString* error)
{
    commands->clear();
    QStringList words;
    QString word;
    bool inWord = false;
    bool single = false;
    bool dbl = false;
    auto endWord = [&] {
        if (inWord) {
            words.append(word);
            word.clear();
            inWord = false;
        }
    };
    const QString text = QString::fromUtf8(line);
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar ch = text.at(i);
        if (single) {
            if (ch == QLatin1Char('\'')) {
                single = false;
            } else {
                word.append(ch);
            }
            continue;
        }
        if (dbl) {
            if (ch == QLatin1Char('"')) {
                dbl = false;
            } else {
                word.append(ch);
            }
            continue;
        }
        if (ch == QLatin1Char('\'')) {
            single = true;
            inWord = true;
        } else if (ch == QLatin1Char('"')) {
            dbl = true;
            inWord = true;
        } else if (ch == QLatin1Char('\\') && i + 1 < text.size()) {
            word.append(text.at(++i));
            inWord = true;
        } else if (ch.isSpace()) {
            endWord();
        } else if (ch == QLatin1Char('&')) {
            if (i + 1 < text.size() && text.at(i + 1) == QLatin1Char('&')) {
                endWord();
                commands->append(words);
                words.clear();
                ++i;
            } else {
                *error = QStringLiteral("syntax error near unexpected token '&'");
                return false;
            }
        } else if (ch == QLatin1Char('>') || ch == QLatin1Char('<')) {
            endWord();
            words.append(QString(ch));
        } else {
            word.append(ch);
            inWord = true;
        }
    }
    if (single || dbl) {
        *error = QStringLiteral("syntax error: unterminated quoted string");
        return false;
    }
    endWord();
    commands->append(words);
    return true;
}

/// The first word of a command line the exec file interpreter handles (everything else is the
/// scripted shell's business).
bool isFileCommand(const QStringList& words)
{
    static const QStringList kCommands = {QStringLiteral("cat"),   QStringLiteral("wc"),    QStringLiteral("test"),
                                          QStringLiteral("chmod"), QStringLiteral("rm"),    QStringLiteral("mkdir"),
                                          QStringLiteral("mv"),    QStringLiteral("pwd"),   QStringLiteral(":")};
    return !words.isEmpty() && kCommands.contains(words.first());
}

/// Resolve a path word of an exec file command: relative names land under `root` (a POSIX-style
/// "/name" too on Windows, like the SFTP handler), absolute ones are used as given, ".." is refused.
bool resolveExecPath(const QString& root, const QString& word, QString* resolved, QString* error)
{
    if (word.isEmpty()) {
        *error = QStringLiteral("empty path");
        return false;
    }
    const QStringList parts = word.split(QRegularExpression(QStringLiteral("[/\\\\]")), Qt::SkipEmptyParts);
    if (parts.contains(QLatin1String(".."))) {
        *error = QStringLiteral("%1: Permission denied").arg(word);
        return false;
    }
    QString path = word;
    if (path == QLatin1String(".") || path == QLatin1String("~")) {
        path = root;
    } else if (path.startsWith(QLatin1String("~/"))) {
        path = root + path.mid(1);
    } else if (!QDir::isAbsolutePath(path)) {
        path = QDir(root).filePath(path);
    }
    *resolved = QDir::cleanPath(path);
    return true;
}

QFileDevice::Permissions permissionsFromOctal(unsigned mode)
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

// ---- Socket helpers (SOCKET on Windows, int elsewhere; winsock is initialised by libssh) ---

void socketClose(socket_t s)
{
#ifdef Q_OS_WIN
    ::closesocket(s);
#else
    ::close(s);
#endif
}

void socketShutdownBoth(socket_t s)
{
#ifdef Q_OS_WIN
    ::shutdown(s, SD_BOTH);
#else
    ::shutdown(s, SHUT_RDWR);
#endif
}

void socketShutdownWrite(socket_t s)
{
#ifdef Q_OS_WIN
    ::shutdown(s, SD_SEND);
#else
    ::shutdown(s, SHUT_WR);
#endif
}

/// Hard close: the peer gets a RST instead of a FIN (a libssh client notices that at once,
/// while a refused TCP connect is invisible to its select-based poll emulation on Windows).
void socketResetAndClose(socket_t s)
{
    linger l{};
    l.l_onoff = 1;
    l.l_linger = 0;
#ifdef Q_OS_WIN
    ::setsockopt(s, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&l), static_cast<int>(sizeof(l)));
#else
    ::setsockopt(s, SOL_SOCKET, SO_LINGER, &l, sizeof(l));
#endif
    socketClose(s);
}

void socketSetNonBlocking(socket_t s)
{
#ifdef Q_OS_WIN
    u_long on = 1;
    ::ioctlsocket(s, FIONBIO, &on);
#else
    const int flags = ::fcntl(s, F_GETFL, 0);
    if (flags >= 0) {
        ::fcntl(s, F_SETFL, flags | O_NONBLOCK);
    }
#endif
}

bool socketWouldBlock()
{
#ifdef Q_OS_WIN
    return ::WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

/// Non-blocking send; returns bytes written, 0 when it would block, -1 on a dead socket.
int socketSend(socket_t s, const char* data, int len)
{
#ifdef Q_OS_WIN
    const int n = ::send(s, data, len, 0);
#else
    const auto n = static_cast<int>(::send(s, data, static_cast<size_t>(len), MSG_NOSIGNAL));
#endif
    if (n >= 0) {
        return n;
    }
    return socketWouldBlock() ? 0 : -1;
}

/// Non-blocking recv; returns bytes read, 0 on EOF or a dead socket, -1 when it would block.
int socketRecv(socket_t s, char* buf, int len)
{
#ifdef Q_OS_WIN
    const int n = ::recv(s, buf, len, 0);
#else
    const auto n = static_cast<int>(::recv(s, buf, static_cast<size_t>(len), 0));
#endif
    if (n > 0) {
        return n;
    }
    if (n == 0) {
        return 0;
    }
    return socketWouldBlock() ? -1 : 0;
}

/// Blocking connect to 127.0.0.1:port (loopback connects complete or fail at once).
socket_t connectLoopback(quint16 port)
{
    const socket_t s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == SSH_INVALID_SOCKET) {
        return SSH_INVALID_SOCKET;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
#ifdef Q_OS_WIN
    const int addrLen = static_cast<int>(sizeof(addr));
#else
    const socklen_t addrLen = sizeof(addr);
#endif
    if (::connect(s, reinterpret_cast<const sockaddr*>(&addr), addrLen) != 0) {
        socketClose(s);
        return SSH_INVALID_SOCKET;
    }
    socketSetNonBlocking(s);
    return s;
}

bool isLoopbackHost(const QString& host)
{
    return host == QLatin1String("127.0.0.1") || host.compare(QLatin1String("localhost"), Qt::CaseInsensitive) == 0;
}

// ---- Key helpers ----------------------------------------------------------------------------

QString fingerprintOf(ssh_key key)
{
    unsigned char* hash = nullptr;
    size_t length = 0;
    if (ssh_get_publickey_hash(key, SSH_PUBLICKEY_HASH_SHA256, &hash, &length) != SSH_OK) {
        return {};
    }
    char* text = ssh_get_fingerprint_hash(SSH_PUBLICKEY_HASH_SHA256, hash, length);
    ssh_clean_pubkey_hash(&hash);
    if (!text) {
        return {};
    }
    const QString result = QString::fromUtf8(text);
    ssh_string_free_char(text);
    return result;
}

/// "<type> <base64>" of the public half of `key` (private or public).
QString publicLineOf(ssh_key key)
{
    ssh_key pub = nullptr;
    const bool own = ssh_key_is_private(key) != 0;
    if (own) {
        if (ssh_pki_export_privkey_to_pubkey(key, &pub) != SSH_OK) {
            return {};
        }
    } else {
        pub = key;
    }
    char* b64 = nullptr;
    QString line;
    if (ssh_pki_export_pubkey_base64(pub, &b64) == SSH_OK && b64) {
        line = QString::fromLatin1(ssh_key_type_to_char(ssh_key_type(pub))) + QLatin1Char(' ') +
               QString::fromLatin1(b64);
        ssh_string_free_char(b64);
    }
    if (own) {
        ssh_key_free(pub);
    }
    return line;
}

/// Import "<type> <base64>[ comment]" (an authorized_keys / .pub line).
ssh_key importPublicLine(const QString& line)
{
    const QStringList parts = line.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (parts.size() < 2) {
        return nullptr;
    }
    const enum ssh_keytypes_e type = ssh_key_type_from_name(parts.at(0).toLatin1().constData());
    if (type == SSH_KEYTYPE_UNKNOWN) {
        return nullptr;
    }
    ssh_key key = nullptr;
    if (ssh_pki_import_pubkey_base64(parts.at(1).toLatin1().constData(), type, &key) != SSH_OK) {
        return nullptr;
    }
    return key;
}

// ---- Shared state ---------------------------------------------------------------------------

class ClientSession;
class AcceptThread;

struct ServerCore
{
    TestSshServer* q = nullptr;
    mutable QMutex mutex;

    TestSshServer::Options options;
    bool running = false;
    bool acceptConnections = true;
    quint16 port = 0;
    quint16 lastPort = 0;

    ssh_key hostKey = nullptr;
    QString hostKeyType;
    QString hostKeyFingerprint;
    QString hostKeyPublicLine;

    ssh_bind bind = nullptr;
    std::unique_ptr<AcceptThread> acceptThread;
    QList<ClientSession*> clients;

    int connectionCount = 0;
    QString lastAuthMethod;
    QString lastTerm;
    QSize lastPtySize;
    QByteArray receivedShellInput;
    QString lastExecCommand;

    bool generateHostKey();
    void freeHostKey();
    void onAccepted(qintptr descriptor);
    void pruneFinishedClients();   ///< mutex held
};

// ---- Per-channel state ----------------------------------------------------------------------

enum class ChannelMode { None, Shell, Exec, Sftp };

struct ChannelContext
{
    ClientSession* client = nullptr;
    ssh_channel channel = nullptr;
    ssh_channel_callbacks_struct callbacks{};
    ChannelMode mode = ChannelMode::None;
    QString term;
    int cols = 80;
    int rows = 24;
    QByteArray line;            ///< shell line being edited
    bool lastWasCr = false;
    bool hung = false;
    bool sleeping = false;
    QDeadlineTimer sleepDeadline;
    bool exitRequested = false;
    int exitStatus = 0;
    QByteArray output;          ///< bytes queued for ssh_channel_write
    QByteArray errorOutput;     ///< bytes queued for ssh_channel_write_stderr
    bool localClosed = false;
    bool remoteClosed = false;
    bool finished = false;
    std::unique_ptr<TestSftpHandler> sftp;
    // exec file commands (see the class comment of TestSshServer)
    QList<QStringList> chain;             ///< commands still to run (" && " chain)
    std::unique_ptr<QFile> stdinFile;     ///< `cat > 'p'`: where stdin goes until EOF
    std::unique_ptr<QFile> stdoutFile;    ///< `cat 'p'`: the file the tick streams to stdout
    bool stdinEof = false;
    qint64 stdinWritten = 0;              ///< `cat > 'p'`: bytes stored so far (Options::execUploadLimit)
    bool stdinFailed = false;             ///< `cat > 'p'`: the limit was hit, the command fails at EOF
    bool dataCommand = false;             ///< `cat > 'p'` / `cat 'p'` ran here (Options::sendExecExitStatus)
};

struct ForwardContext
{
    ClientSession* client = nullptr;
    ssh_channel channel = nullptr;
    ssh_channel_callbacks_struct callbacks{};
    socket_t socket = SSH_INVALID_SOCKET;
    bool fdRegistered = false;
    QByteArray toSocket;
    QByteArray toChannel;
    bool channelEof = false;    ///< client sent EOF: nothing more for the socket
    bool socketEof = false;     ///< the target closed or failed
    bool shutdownSent = false;
    bool localClosed = false;
    bool remoteClosed = false;
    bool finished = false;
};

// ---- One client connection = one thread ----------------------------------------------------

class ClientSession : public QThread
{
public:
    ClientSession(ServerCore* core, ssh_session session, socket_t fd);
    ~ClientSession() override;

    /// Wake the thread and make it leave; the caller holds core->mutex.
    void requestStop();

protected:
    void run() override;

private:
    // libssh server callbacks
    static int cbAuthPassword(ssh_session, const char* user, const char* password, void* userdata);
    static int cbAuthNone(ssh_session, const char* user, void* userdata);
    static int cbAuthPubkey(ssh_session, const char* user, struct ssh_key_struct* pubkey, char state, void* userdata);
    static int cbServiceRequest(ssh_session, const char* service, void* userdata);
    static ssh_channel cbChannelOpenSession(ssh_session, void* userdata);
    static int cbMessage(ssh_session, ssh_message msg, void* userdata);
    // session channel callbacks
    static int cbData(ssh_session, ssh_channel, void* data, uint32_t len, int isStderr, void* userdata);
    static void cbEof(ssh_session, ssh_channel, void* userdata);
    static void cbClose(ssh_session, ssh_channel, void* userdata);
    static int cbPty(ssh_session, ssh_channel, const char* term, int width, int height, int, int, void* userdata);
    static int cbWindowChange(ssh_session, ssh_channel, int width, int height, int, int, void* userdata);
    static int cbShell(ssh_session, ssh_channel, void* userdata);
    static int cbExec(ssh_session, ssh_channel, const char* command, void* userdata);
    static int cbEnv(ssh_session, ssh_channel, const char* name, const char* value, void* userdata);
    static int cbSubsystem(ssh_session, ssh_channel, const char* subsystem, void* userdata);
    // direct-tcpip callbacks
    static int cbForwardData(ssh_session, ssh_channel, void* data, uint32_t len, int isStderr, void* userdata);
    static void cbForwardEof(ssh_session, ssh_channel, void* userdata);
    static void cbForwardClose(ssh_session, ssh_channel, void* userdata);
    static int cbForwardSocket(socket_t fd, int revents, void* userdata);

    bool setup();
    void serviceLoop();
    void teardown();
    void tick();
    void serviceChannel(ChannelContext& c);
    void serviceForward(ForwardContext& f);
    void finishChannel(ChannelContext& c);
    void finishForward(ForwardContext& f);
    void flushChannelOutput(ChannelContext& c);
    void feedShell(ChannelContext& c, const char* data, uint32_t len);
    void runLine(ChannelContext& c, const QByteArray& line, bool exec);
    void runExecChain(ChannelContext& c);                       ///< run c.chain until it waits or ends
    int runFileCommand(ChannelContext& c, const QStringList& words);   ///< exit status, -1 = waiting (stdin / stream)
    void requestExit(ChannelContext& c, int status);
    void noteActivity();
    void noteAuthenticated(const QString& method);
    int handleKbdint(ssh_message msg);
    int handleDirectTcpip(ssh_message msg);

    ServerCore* m_core;
    TestSshServer* m_q;
    TestSshServer::Options m_opts;
    ssh_session m_session;
    socket_t m_fd;
    bool m_fdOpen = true;                       ///< guarded by m_core->mutex
    std::atomic<bool> m_stop{false};
    ssh_server_callbacks_struct m_serverCallbacks{};
    ssh_event m_event = nullptr;
    ssh_key m_authorizedKey = nullptr;
    bool m_authenticated = false;
    bool m_bannerSent = false;
    bool m_kbdintUserOk = false;
    bool m_blackHole = false;
    QElapsedTimer m_lastActivity;
    std::list<std::unique_ptr<ChannelContext>> m_channels;
    std::list<std::unique_ptr<ForwardContext>> m_forwards;
};

ClientSession::ClientSession(ServerCore* core, ssh_session session, socket_t fd)
    : m_core(core)
    , m_q(core->q)
    , m_opts(core->options)
    , m_session(session)
    , m_fd(fd)
{
    setObjectName(QStringLiteral("TestSshServer-client"));
}

ClientSession::~ClientSession()
{
    wait();
}

void ClientSession::requestStop()
{
    m_stop.store(true);
    if (m_fdOpen) {
        // Wakes the thread out of ssh_event_dopoll / the key exchange; the peer sees a FIN
        // right away, i.e. an abrupt end without SSH_MSG_DISCONNECT.
        socketShutdownBoth(m_fd);
    }
}

void ClientSession::noteActivity()
{
    m_lastActivity.start();
}

void ClientSession::noteAuthenticated(const QString& method)
{
    m_authenticated = true;
    {
        QMutexLocker lock(&m_core->mutex);
        m_core->lastAuthMethod = method;
    }
    qCInfo(lcSsh) << "TestSshServer: client authenticated with" << method;
    emit m_q->clientAuthenticated(method);
}

// ---- Authentication ------------------------------------------------------------------------

int ClientSession::cbAuthPassword(ssh_session, const char* user, const char* password, void* userdata)
{
    auto* self = static_cast<ClientSession*>(userdata);
    self->noteActivity();
    if (!self->m_opts.allowPassword || !user || !password) {
        return SSH_AUTH_DENIED;
    }
    if (QString::fromUtf8(user) == self->m_opts.user && QString::fromUtf8(password) == self->m_opts.password) {
        self->noteAuthenticated(QStringLiteral("password"));
        return SSH_AUTH_SUCCESS;
    }
    qCInfo(lcSsh) << "TestSshServer: password rejected for" << user;
    return SSH_AUTH_DENIED;
}

int ClientSession::cbAuthNone(ssh_session, const char*, void* userdata)
{
    auto* self = static_cast<ClientSession*>(userdata);
    self->noteActivity();
    return SSH_AUTH_DENIED;   // libssh answers with the enabled methods
}

int ClientSession::cbAuthPubkey(ssh_session, const char* user, struct ssh_key_struct* pubkey, char state,
                                void* userdata)
{
    auto* self = static_cast<ClientSession*>(userdata);
    self->noteActivity();
    if (!self->m_opts.allowPublicKey || !self->m_authorizedKey || !user || !pubkey) {
        return SSH_AUTH_DENIED;
    }
    if (QString::fromUtf8(user) != self->m_opts.user) {
        return SSH_AUTH_DENIED;
    }
    if (ssh_key_cmp(pubkey, self->m_authorizedKey, SSH_KEY_CMP_PUBLIC) != 0) {
        return SSH_AUTH_DENIED;
    }
    if (state == SSH_PUBLICKEY_STATE_NONE) {
        return SSH_AUTH_SUCCESS;   // probe: libssh replies PK_OK
    }
    if (state == SSH_PUBLICKEY_STATE_VALID) {
        self->noteAuthenticated(QStringLiteral("publickey"));
        return SSH_AUTH_SUCCESS;
    }
    return SSH_AUTH_DENIED;
}

int ClientSession::cbServiceRequest(ssh_session session, const char* service, void* userdata)
{
    auto* self = static_cast<ClientSession*>(userdata);
    self->noteActivity();
    if (service && std::strcmp(service, "ssh-userauth") == 0 && !self->m_bannerSent && !self->m_opts.banner.isEmpty()) {
        // SSH_MSG_USERAUTH_BANNER goes out before the service accept, exactly where OpenSSH
        // sends its /etc/issue.net text; clients read it with ssh_get_issue_banner().
        self->m_bannerSent = true;
        ssh_string banner = ssh_string_from_char(self->m_opts.banner.toUtf8().constData());
        if (banner) {
            ssh_send_issue_banner(session, banner);
            ssh_string_free(banner);
        }
    }
    return 0;
}

int ClientSession::handleKbdint(ssh_message msg)
{
    if (!m_opts.allowKeyboardInteractive) {
        return 1;
    }
    if (!ssh_message_auth_kbdint_is_response(msg)) {
        const char* user = ssh_message_auth_user(msg);
        m_kbdintUserOk = user && QString::fromUtf8(user) == m_opts.user;
        const QByteArray prompt = m_opts.kbdintPrompt.toUtf8();
        const char* prompts[1] = {prompt.constData()};
        char echo[1] = {0};
        ssh_message_auth_interactive_request(msg, "TestSshServer", "", 1, prompts, echo);
        return 0;
    }
    const char* answer = ssh_userauth_kbdint_getanswer(m_session, 0);
    if (m_kbdintUserOk && ssh_userauth_kbdint_getnanswers(m_session) == 1 && answer &&
        QString::fromUtf8(answer) == m_opts.kbdintAnswer) {
        ssh_message_auth_reply_success(msg, 0);
        noteAuthenticated(QStringLiteral("keyboard-interactive"));
        return 0;
    }
    qCInfo(lcSsh) << "TestSshServer: keyboard-interactive answer rejected";
    return 1;
}

// ---- Channels ------------------------------------------------------------------------------

ssh_channel ClientSession::cbChannelOpenSession(ssh_session session, void* userdata)
{
    auto* self = static_cast<ClientSession*>(userdata);
    self->noteActivity();
    ssh_channel channel = ssh_channel_new(session);
    if (!channel) {
        return nullptr;
    }
    auto context = std::make_unique<ChannelContext>();
    context->client = self;
    context->channel = channel;
    ssh_channel_callbacks_struct& cb = context->callbacks;
    cb.userdata = context.get();
    cb.channel_data_function = &ClientSession::cbData;
    cb.channel_eof_function = &ClientSession::cbEof;
    cb.channel_close_function = &ClientSession::cbClose;
    cb.channel_pty_request_function = &ClientSession::cbPty;
    cb.channel_pty_window_change_function = &ClientSession::cbWindowChange;
    cb.channel_shell_request_function = &ClientSession::cbShell;
    cb.channel_exec_request_function = &ClientSession::cbExec;
    cb.channel_env_request_function = &ClientSession::cbEnv;
    cb.channel_subsystem_request_function = &ClientSession::cbSubsystem;
    ssh_callbacks_init(&cb);
    ssh_set_channel_callbacks(channel, &cb);
    self->m_channels.push_back(std::move(context));
    return channel;
}

int ClientSession::cbData(ssh_session, ssh_channel, void* data, uint32_t len, int isStderr, void* userdata)
{
    auto* c = static_cast<ChannelContext*>(userdata);
    c->client->noteActivity();
    if (isStderr || len == 0) {
        return static_cast<int>(len);
    }
    const auto* bytes = static_cast<const char*>(data);
    switch (c->mode) {
    case ChannelMode::Shell:
        c->client->feedShell(*c, bytes, len);
        break;
    case ChannelMode::Sftp:
        if (c->sftp) {
            c->sftp->feed(QByteArray(bytes, static_cast<qsizetype>(len)), &c->output);
        }
        break;
    case ChannelMode::Exec:
        if (c->stdinFile) {
            // `cat > 'p'`; with a limit the file stops growing (a full flash) and the rest is
            // dropped: the failure is reported when the client's EOF ends the command.
            const qint64 limit = c->client->m_opts.execUploadLimit;
            qint64 store = static_cast<qint64>(len);
            if (limit >= 0 && c->stdinWritten + store > limit) {
                store = qMax<qint64>(0, limit - c->stdinWritten);
                c->stdinFailed = true;
            }
            if (store > 0) {
                c->stdinFile->write(bytes, store);
                c->stdinWritten += store;
            }
        }
        break;   // otherwise nothing reads it; consumed
    case ChannelMode::None:
        break;
    }
    return static_cast<int>(len);
}

void ClientSession::cbEof(ssh_session, ssh_channel, void* userdata)
{
    auto* c = static_cast<ChannelContext*>(userdata);
    c->client->noteActivity();
    c->stdinEof = true;
    if (c->stdinFile) {
        // `cat > 'p'` is complete; the rest of the chain runs now - unless the upload limit was
        // hit, in which case cat reports the write error and the chain stops there.
        c->stdinFile->close();
        c->stdinFile.reset();
        if (c->stdinFailed) {
            c->errorOutput.append("cat: write error: No space left on device\n");
            c->chain.clear();
            c->client->requestExit(*c, 1);
            return;
        }
        c->client->runExecChain(*c);
    }
}

void ClientSession::cbClose(ssh_session, ssh_channel, void* userdata)
{
    auto* c = static_cast<ChannelContext*>(userdata);
    c->client->noteActivity();
    c->remoteClosed = true;
    // Release the files right here: a client that cancelled an upload sends `rm -f 'p'` on the
    // next channel, which the same poll may deliver before the tick finishes this one.
    c->stdinFile.reset();
    c->stdoutFile.reset();
    c->chain.clear();
}

int ClientSession::cbPty(ssh_session, ssh_channel, const char* term, int width, int height, int, int, void* userdata)
{
    auto* c = static_cast<ChannelContext*>(userdata);
    c->client->noteActivity();
    c->term = QString::fromUtf8(term ? term : "");
    c->cols = width > 0 ? width : 80;
    c->rows = height > 0 ? height : 24;
    {
        QMutexLocker lock(&c->client->m_core->mutex);
        c->client->m_core->lastTerm = c->term;
        c->client->m_core->lastPtySize = QSize(c->cols, c->rows);
    }
    return 0;
}

int ClientSession::cbWindowChange(ssh_session, ssh_channel, int width, int height, int, int, void* userdata)
{
    auto* c = static_cast<ChannelContext*>(userdata);
    c->client->noteActivity();
    if (width > 0 && height > 0) {
        c->cols = width;
        c->rows = height;
        QMutexLocker lock(&c->client->m_core->mutex);
        c->client->m_core->lastPtySize = QSize(width, height);
    }
    return 0;
}

int ClientSession::cbShell(ssh_session, ssh_channel, void* userdata)
{
    auto* c = static_cast<ChannelContext*>(userdata);
    c->client->noteActivity();
    if (c->mode != ChannelMode::None) {
        return 1;
    }
    c->mode = ChannelMode::Shell;
    c->output.append("welcome\r\n$ ");
    emit c->client->m_q->shellRequested();
    return 0;
}

int ClientSession::cbExec(ssh_session, ssh_channel, const char* command, void* userdata)
{
    auto* c = static_cast<ChannelContext*>(userdata);
    c->client->noteActivity();
    if (c->mode != ChannelMode::None) {
        return 1;
    }
    c->mode = ChannelMode::Exec;
    const QByteArray line(command ? command : "");
    {
        QMutexLocker lock(&c->client->m_core->mutex);
        c->client->m_core->lastExecCommand = QString::fromUtf8(line);
    }
    QList<QStringList> commands;
    QString error;
    if (!splitExecCommands(line, &commands, &error)) {
        c->errorOutput.append(QStringLiteral("sh: %1\n").arg(error).toUtf8());
        c->client->requestExit(*c, 2);
    } else if (commands.size() > 1 || isFileCommand(commands.first())) {
        c->chain = commands;
        c->client->runExecChain(*c);
    } else {
        c->client->runLine(*c, line, true);
    }
    emit c->client->m_q->execRequested(QString::fromUtf8(line));
    return 0;
}

int ClientSession::cbEnv(ssh_session, ssh_channel, const char*, const char*, void* userdata)
{
    auto* c = static_cast<ChannelContext*>(userdata);
    c->client->noteActivity();
    return 0;
}

int ClientSession::cbSubsystem(ssh_session, ssh_channel, const char* subsystem, void* userdata)
{
    auto* c = static_cast<ChannelContext*>(userdata);
    c->client->noteActivity();
    if (!subsystem || std::strcmp(subsystem, "sftp") != 0 || c->mode != ChannelMode::None) {
        return 1;
    }
    if (!c->client->m_opts.allowSftp) {
        // A dropbear-like server: the request is refused, the client has to use exec commands.
        qCInfo(lcSsh) << "TestSshServer: sftp subsystem refused (allowSftp = false)";
        emit c->client->m_q->sftpRequested();
        return 1;
    }
    QString root = c->client->m_opts.rootDir;
    if (root.isEmpty() || !QDir(root).exists()) {
        qCWarning(lcSsh) << "TestSshServer: SFTP root" << root << "does not exist; using the current directory";
        root = QDir::currentPath();
    }
    c->mode = ChannelMode::Sftp;
    c->sftp = std::make_unique<TestSftpHandler>(root);
    emit c->client->m_q->sftpRequested();
    return 0;
}

// ---- The scripted shell --------------------------------------------------------------------

void ClientSession::feedShell(ChannelContext& c, const char* data, uint32_t len)
{
    {
        QMutexLocker lock(&m_core->mutex);
        m_core->receivedShellInput.append(data, static_cast<qsizetype>(len));
    }
    for (uint32_t i = 0; i < len; ++i) {
        const char byte = data[i];
        if (c.hung || c.exitRequested) {
            continue;
        }
        const bool afterCr = c.lastWasCr;
        c.lastWasCr = byte == '\r';
        if (byte == '\r' || (byte == '\n' && !afterCr)) {
            c.output.append("\r\n");
            const QByteArray line = c.line;
            c.line.clear();
            runLine(c, line, false);
        } else if (byte == '\n') {
            // the LF of a CR LF pair
        } else if (byte == 0x7F || byte == 0x08) {
            if (!c.line.isEmpty()) {
                c.line.chop(1);
                c.output.append("\b \b");
            }
        } else if (!c.sleeping) {
            c.line.append(byte);
            c.output.append(byte);
        }
    }
}

void ClientSession::requestExit(ChannelContext& c, int status)
{
    c.exitStatus = status;
    c.exitRequested = true;
}

void ClientSession::runLine(ChannelContext& c, const QByteArray& rawLine, bool exec)
{
    const QByteArray line = rawLine.trimmed();
    const QList<QByteArray> words = line.split(' ');
    const QByteArray command = words.isEmpty() ? QByteArray() : words.first();
    bool prompt = !exec;
    int exitStatus = 0;
    bool exits = exec;

    if (command.isEmpty()) {
        // empty line: just a new prompt
    } else if (command == "exit") {
        exits = true;
        prompt = false;
        if (words.size() > 1) {
            exitStatus = words.at(1).toInt();
        }
    } else if (command == "env") {
        c.output.append(QStringLiteral("TERM=%1 COLS=%2 ROWS=%3\r\n").arg(c.term).arg(c.cols).arg(c.rows).toUtf8());
    } else if (command == "size") {
        c.output.append(QStringLiteral("COLS=%1 ROWS=%2\r\n").arg(c.cols).arg(c.rows).toUtf8());
    } else if (command == "echo") {
        const int space = line.indexOf(' ');
        c.output.append(space < 0 ? QByteArray() : line.mid(space + 1).trimmed());
        c.output.append("\r\n");
    } else if (command == "big") {
        const int n = words.size() > 1 ? qBound(0, words.at(1).toInt(), kMaxBigLines) : 0;
        QByteArray block;
        block.reserve(n * 12);
        for (int k = 1; k <= n; ++k) {
            block.append("line ");
            block.append(QByteArray::number(k));
            block.append("\r\n");
        }
        c.output.append(block);
    } else if (command == "sleep") {
        const int ms = words.size() > 1 ? qMax(0, words.at(1).toInt()) : 0;
        c.sleeping = true;
        c.sleepDeadline = QDeadlineTimer(ms);
        prompt = false;
        exits = false;   // the tick finishes the command once the deadline passed
    } else if (command == "hang") {
        c.hung = true;
        prompt = false;
        exits = false;
    } else {
        c.output.append(command);
        c.output.append(": not found\r\n");
    }

    if (exits) {
        requestExit(c, exitStatus);
    } else if (prompt) {
        c.output.append("$ ");
    }
}

// ---- The exec file commands ------------------------------------------------------------------

void ClientSession::runExecChain(ChannelContext& c)
{
    while (!c.chain.isEmpty()) {
        const QStringList words = c.chain.takeFirst();
        const int status = runFileCommand(c, words);
        if (status < 0) {
            return;   // waiting for stdin EOF (`cat >`) or streaming a file (`cat`); resumed later
        }
        if (status != 0) {
            c.chain.clear();
            requestExit(c, status);
            return;
        }
    }
    requestExit(c, 0);
}

int ClientSession::runFileCommand(ChannelContext& c, const QStringList& words)
{
    auto fail = [&c](const QString& message) {
        c.errorOutput.append((message + QLatin1Char('\n')).toUtf8());
        return 1;
    };
    const QString root = m_opts.rootDir;
    auto resolve = [&](const QString& word, QString* path) {
        QString error;
        if (!resolveExecPath(root, word, path, &error)) {
            fail(error);
            return false;
        }
        return true;
    };
    const QString command = words.value(0);
    if (command == QLatin1String("pwd")) {
        c.output.append((QFileInfo(root).canonicalFilePath() + QLatin1Char('\n')).toUtf8());
        return 0;
    }
    if (command == QLatin1String(":")) {
        // `: > 'p'`: the shell opens the file for writing (create / truncate) and runs nothing.
        if (words.size() != 3 || words.at(1) != QLatin1String(">")) {
            return words.size() == 1 ? 0 : fail(QStringLiteral("sh: unsupported redirection"));
        }
        QString path;
        if (!resolve(words.at(2), &path)) {
            return 1;
        }
        QFile file(path);
        if (QFileInfo(path).isDir() || !file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return fail(QStringLiteral("sh: can't create '%1': %2")
                            .arg(words.at(2), QFileInfo(path).isDir() ? QStringLiteral("Is a directory") : file.errorString()));
        }
        return 0;
    }
    if (command == QLatin1String("cat")) {
        if (words.size() == 3 && words.at(1) == QLatin1String(">")) {
            QString path;
            if (!resolve(words.at(2), &path)) {
                return 1;
            }
            auto file = std::make_unique<QFile>(path);
            if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                return fail(QStringLiteral("cat: can't create '%1': %2").arg(words.at(2), file->errorString()));
            }
            c.dataCommand = true;
            if (c.stdinEof) {
                return 0;   // EOF already arrived: an empty file
            }
            c.stdinFile = std::move(file);
            return -1;
        }
        if (words.size() == 2) {
            QString path;
            if (!resolve(words.at(1), &path)) {
                return 1;
            }
            auto file = std::make_unique<QFile>(path);
            if (!QFileInfo(path).isFile() || !file->open(QIODevice::ReadOnly)) {
                return fail(QStringLiteral("cat: can't open '%1': No such file or directory").arg(words.at(1)));
            }
            c.dataCommand = true;
            c.stdoutFile = std::move(file);   // streamed by the tick
            return -1;
        }
        return fail(QStringLiteral("cat: unsupported arguments"));
    }
    if (command == QLatin1String("wc")) {
        if (words.size() != 4 || words.at(1) != QLatin1String("-c") || words.at(2) != QLatin1String("<")) {
            return fail(QStringLiteral("wc: unsupported arguments"));
        }
        QString path;
        if (!resolve(words.at(3), &path)) {
            return 1;
        }
        const QFileInfo info(path);
        if (!info.isFile()) {
            return fail(QStringLiteral("sh: can't open '%1': No such file or directory").arg(words.at(3)));
        }
        c.output.append((QString::number(info.size()) + QLatin1Char('\n')).toUtf8());
        return 0;
    }
    if (command == QLatin1String("test")) {
        if (words.size() != 3 || words.at(1) != QLatin1String("-e")) {
            return fail(QStringLiteral("test: unsupported arguments"));
        }
        QString path;
        if (!resolve(words.at(2), &path)) {
            return 1;
        }
        return QFileInfo::exists(path) ? 0 : 1;
    }
    if (command == QLatin1String("chmod")) {
        if (words.size() != 3) {
            return fail(QStringLiteral("chmod: unsupported arguments"));
        }
        bool ok = false;
        const unsigned mode = words.at(1).toUInt(&ok, 8);
        if (!ok || mode > 07777u) {
            return fail(QStringLiteral("chmod: invalid mode '%1'").arg(words.at(1)));
        }
        QString path;
        if (!resolve(words.at(2), &path)) {
            return 1;
        }
        if (!QFileInfo::exists(path)) {
            return fail(QStringLiteral("chmod: %1: No such file or directory").arg(words.at(2)));
        }
        QFile::setPermissions(path, permissionsFromOctal(mode & 0777u));   // best effort (Windows keeps the write bit only)
        return 0;
    }
    if (command == QLatin1String("rm")) {
        if (words.size() != 3 || words.at(1) != QLatin1String("-f")) {
            return fail(QStringLiteral("rm: unsupported arguments"));
        }
        QString path;
        if (!resolve(words.at(2), &path)) {
            return 1;
        }
        const QFileInfo info(path);
        if (!info.exists()) {
            return 0;   // -f: a missing file is fine
        }
        if (info.isDir()) {
            return fail(QStringLiteral("rm: can't remove '%1': Is a directory").arg(words.at(2)));
        }
        if (!QFile::remove(path)) {
            return fail(QStringLiteral("rm: can't remove '%1': Permission denied").arg(words.at(2)));
        }
        return 0;
    }
    if (command == QLatin1String("mkdir")) {
        if (words.size() != 3 || words.at(1) != QLatin1String("-p")) {
            return fail(QStringLiteral("mkdir: unsupported arguments"));
        }
        QString path;
        if (!resolve(words.at(2), &path)) {
            return 1;
        }
        if (!QDir().mkpath(path)) {
            return fail(QStringLiteral("mkdir: can't create directory '%1': Permission denied").arg(words.at(2)));
        }
        return 0;
    }
    if (command == QLatin1String("mv")) {
        if (words.size() != 3) {
            return fail(QStringLiteral("mv: unsupported arguments"));
        }
        QString from;
        QString to;
        if (!resolve(words.at(1), &from) || !resolve(words.at(2), &to)) {
            return 1;
        }
        if (!QFileInfo::exists(from)) {
            return fail(QStringLiteral("mv: can't rename '%1': No such file or directory").arg(words.at(1)));
        }
        if (QFileInfo(to).isFile() && !QFile::remove(to)) {
            return fail(QStringLiteral("mv: can't overwrite '%1': Permission denied").arg(words.at(2)));
        }
        if (!QFile::rename(from, to)) {
            return fail(QStringLiteral("mv: can't rename '%1' to '%2': Permission denied").arg(words.at(1), words.at(2)));
        }
        return 0;
    }
    return fail(QStringLiteral("sh: %1: not found").arg(command));
}

// ---- direct-tcpip ----------------------------------------------------------------------------

int ClientSession::handleDirectTcpip(ssh_message msg)
{
    const char* destination = ssh_message_channel_request_open_destination(msg);
    const int port = ssh_message_channel_request_open_destination_port(msg);
    const QString host = QString::fromUtf8(destination ? destination : "");
    emit m_q->forwardRequested(host, static_cast<quint16>(qBound(0, port, 65535)));
    if (!isLoopbackHost(host) || port <= 0 || port > 65535) {
        qCInfo(lcSsh) << "TestSshServer: direct-tcpip to" << host << port << "refused";
        return 1;
    }
    const socket_t s = connectLoopback(static_cast<quint16>(port));
    if (s == SSH_INVALID_SOCKET) {
        qCInfo(lcSsh) << "TestSshServer: direct-tcpip connect to 127.0.0.1:" << port << "failed";
        return 1;
    }
    ssh_channel channel = ssh_message_channel_request_open_reply_accept(msg);
    if (!channel) {
        socketClose(s);
        return 1;
    }
    auto forward = std::make_unique<ForwardContext>();
    forward->client = this;
    forward->channel = channel;
    forward->socket = s;
    ssh_channel_callbacks_struct& cb = forward->callbacks;
    cb.userdata = forward.get();
    cb.channel_data_function = &ClientSession::cbForwardData;
    cb.channel_eof_function = &ClientSession::cbForwardEof;
    cb.channel_close_function = &ClientSession::cbForwardClose;
    ssh_callbacks_init(&cb);
    ssh_set_channel_callbacks(channel, &cb);
    if (ssh_event_add_fd(m_event, s, static_cast<short>(POLLIN), &ClientSession::cbForwardSocket, forward.get()) ==
        SSH_OK) {
        forward->fdRegistered = true;
    } else {
        forward->socketEof = true;
    }
    qCDebug(lcSsh) << "TestSshServer: direct-tcpip channel to 127.0.0.1:" << port;
    m_forwards.push_back(std::move(forward));
    return 0;
}

int ClientSession::cbForwardData(ssh_session, ssh_channel, void* data, uint32_t len, int isStderr, void* userdata)
{
    auto* f = static_cast<ForwardContext*>(userdata);
    f->client->noteActivity();
    if (!isStderr && len > 0) {
        f->toSocket.append(static_cast<const char*>(data), static_cast<qsizetype>(len));
    }
    return static_cast<int>(len);
}

void ClientSession::cbForwardEof(ssh_session, ssh_channel, void* userdata)
{
    auto* f = static_cast<ForwardContext*>(userdata);
    f->client->noteActivity();
    f->channelEof = true;
}

void ClientSession::cbForwardClose(ssh_session, ssh_channel, void* userdata)
{
    auto* f = static_cast<ForwardContext*>(userdata);
    f->client->noteActivity();
    f->remoteClosed = true;
}

int ClientSession::cbForwardSocket(socket_t, int revents, void* userdata)
{
    auto* f = static_cast<ForwardContext*>(userdata);
    if (f->socketEof) {
        return 0;
    }
    if (revents & (POLLIN | POLLHUP | POLLERR)) {
        char buf[kForwardReadChunk];
        for (;;) {
            const int n = socketRecv(f->socket, buf, kForwardReadChunk);
            if (n > 0) {
                f->toChannel.append(buf, n);
                continue;
            }
            if (n == 0) {
                f->socketEof = true;
            }
            break;
        }
    }
    return 0;
}

// ---- Message callback (everything the typed callbacks do not cover) -------------------------

int ClientSession::cbMessage(ssh_session, ssh_message msg, void* userdata)
{
    auto* self = static_cast<ClientSession*>(userdata);
    self->noteActivity();
    const int type = ssh_message_type(msg);
    const int subtype = ssh_message_subtype(msg);
    if (type == SSH_REQUEST_AUTH && subtype == SSH_AUTH_METHOD_INTERACTIVE) {
        return self->handleKbdint(msg);
    }
    if (type == SSH_REQUEST_CHANNEL_OPEN && subtype == SSH_CHANNEL_DIRECT_TCPIP) {
        return self->handleDirectTcpip(msg);
    }
    return 1;   // default reply: auth failure with the enabled methods, request failure, ...
}

// ---- Thread body ------------------------------------------------------------------------------

bool ClientSession::setup()
{
    socketSetNonBlocking(m_fd);
    long timeout = kKeyExchangeTimeoutSeconds;
    ssh_options_set(m_session, SSH_OPTIONS_TIMEOUT, &timeout);

    if (!m_opts.authorizedPublicKey.isEmpty()) {
        m_authorizedKey = importPublicLine(m_opts.authorizedPublicKey);
        if (!m_authorizedKey) {
            qCWarning(lcSsh) << "TestSshServer: cannot import authorizedPublicKey" << m_opts.authorizedPublicKey;
        }
    }

    m_serverCallbacks.userdata = this;
    m_serverCallbacks.auth_password_function = &ClientSession::cbAuthPassword;
    m_serverCallbacks.auth_none_function = &ClientSession::cbAuthNone;
    m_serverCallbacks.auth_pubkey_function = &ClientSession::cbAuthPubkey;
    m_serverCallbacks.service_request_function = &ClientSession::cbServiceRequest;
    m_serverCallbacks.channel_open_request_session_function = &ClientSession::cbChannelOpenSession;
    ssh_callbacks_init(&m_serverCallbacks);
    ssh_set_server_callbacks(m_session, &m_serverCallbacks);
    ssh_set_message_callback(m_session, &ClientSession::cbMessage, this);

    if (ssh_handle_key_exchange(m_session) != SSH_OK) {
        qCInfo(lcSsh) << "TestSshServer: key exchange failed:" << ssh_get_error(m_session);
        return false;
    }

    int methods = 0;
    if (m_opts.allowPassword) {
        methods |= SSH_AUTH_METHOD_PASSWORD;
    }
    if (m_opts.allowPublicKey) {
        methods |= SSH_AUTH_METHOD_PUBLICKEY;
    }
    if (m_opts.allowKeyboardInteractive) {
        methods |= SSH_AUTH_METHOD_INTERACTIVE;
    }
    ssh_set_auth_methods(m_session, methods);
    ssh_set_blocking(m_session, 0);   // writes return what fits; nothing ever blocks the loop

    m_event = ssh_event_new();
    if (!m_event || ssh_event_add_session(m_event, m_session) != SSH_OK) {
        qCWarning(lcSsh) << "TestSshServer: cannot create the event context";
        return false;
    }
    m_lastActivity.start();
    return true;
}

void ClientSession::serviceLoop()
{
    while (!m_stop.load()) {
        if (m_blackHole) {
            QThread::msleep(kPollMs);
            continue;
        }
        const int rc = ssh_event_dopoll(m_event, kPollMs);
        if (rc == SSH_ERROR) {
            break;
        }
        if (ssh_get_status(m_session) & (SSH_CLOSED | SSH_CLOSED_ERROR)) {
            break;
        }
        tick();
        if (m_opts.keepAliveDropAfter > 0 && m_authenticated &&
            m_lastActivity.elapsed() > static_cast<qint64>(m_opts.keepAliveDropAfter) * 1000) {
            qCInfo(lcSsh) << "TestSshServer: idle for" << m_opts.keepAliveDropAfter
                          << "s, going silent (keepAliveDropAfter)";
            m_blackHole = true;
        }
    }
}

void ClientSession::tick()
{
    for (auto& c : m_channels) {
        serviceChannel(*c);
    }
    m_channels.remove_if([](const std::unique_ptr<ChannelContext>& c) { return c->finished; });
    for (auto& f : m_forwards) {
        serviceForward(*f);
    }
    m_forwards.remove_if([](const std::unique_ptr<ForwardContext>& f) { return f->finished; });
}

void ClientSession::flushChannelOutput(ChannelContext& c)
{
    // stderr first (short messages), then stdout; both stop at an exhausted remote window and
    // continue next tick (the session is non-blocking, so ssh_channel_write returns 0 then).
    while (!c.errorOutput.isEmpty()) {
        if (c.localClosed || !ssh_channel_is_open(c.channel)) {
            c.errorOutput.clear();
            c.output.clear();
            return;
        }
        const int chunk = static_cast<int>(qMin<qsizetype>(c.errorOutput.size(), kMaxWriteChunk));
        const int n = ssh_channel_write_stderr(c.channel, c.errorOutput.constData(), static_cast<uint32_t>(chunk));
        if (n < 0) {
            c.errorOutput.clear();
            c.output.clear();
            return;
        }
        if (n == 0) {
            return;
        }
        c.errorOutput.remove(0, n);
    }
    while (!c.output.isEmpty()) {
        if (c.localClosed || !ssh_channel_is_open(c.channel)) {
            c.output.clear();
            return;
        }
        const int chunk = static_cast<int>(qMin<qsizetype>(c.output.size(), kMaxWriteChunk));
        const int n = ssh_channel_write(c.channel, c.output.constData(), static_cast<uint32_t>(chunk));
        if (n < 0) {
            c.output.clear();
            return;
        }
        if (n == 0) {
            return;   // remote window exhausted: retry next tick
        }
        c.output.remove(0, n);
    }
}

void ClientSession::serviceChannel(ChannelContext& c)
{
    if (c.finished) {
        return;
    }
    if (c.sleeping && c.sleepDeadline.hasExpired()) {
        c.sleeping = false;
        if (c.mode == ChannelMode::Exec) {
            requestExit(c, 0);
        } else {
            c.output.append("$ ");
        }
    }
    // `cat 'p'`: refill the output queue from the file while the window drains it, so a big file
    // never sits in memory at once; at its end the chain continues.
    while (c.stdoutFile && c.output.size() < kStreamHighWater) {
        const QByteArray chunk = c.stdoutFile->read(kStreamChunk);
        if (chunk.isEmpty()) {
            c.stdoutFile.reset();
            runExecChain(c);
            break;
        }
        c.output.append(chunk);
    }
    flushChannelOutput(c);
    if (c.exitRequested && c.output.isEmpty() && c.errorOutput.isEmpty() && !c.localClosed
        && ssh_channel_is_open(c.channel)) {
        if (c.mode != ChannelMode::Exec || !c.dataCommand || m_opts.sendExecExitStatus) {
            ssh_channel_request_send_exit_status(c.channel, c.exitStatus);
        }
        ssh_channel_send_eof(c.channel);
        ssh_channel_close(c.channel);
        c.localClosed = true;
    }
    if (c.remoteClosed) {
        finishChannel(c);
    }
}

void ClientSession::finishChannel(ChannelContext& c)
{
    if (c.finished) {
        return;
    }
    c.stdinFile.reset();
    c.stdoutFile.reset();
    c.chain.clear();
    ssh_remove_channel_callbacks(c.channel, &c.callbacks);
    ssh_channel_free(c.channel);   // answers the peer's close when we have not closed yet
    c.channel = nullptr;
    c.finished = true;
}

void ClientSession::serviceForward(ForwardContext& f)
{
    if (f.finished) {
        return;
    }
    // target socket -> channel
    while (!f.toChannel.isEmpty() && !f.localClosed && ssh_channel_is_open(f.channel)) {
        const int chunk = static_cast<int>(qMin<qsizetype>(f.toChannel.size(), kMaxWriteChunk));
        const int n = ssh_channel_write(f.channel, f.toChannel.constData(), static_cast<uint32_t>(chunk));
        if (n <= 0) {
            if (n < 0) {
                f.toChannel.clear();
            }
            break;
        }
        f.toChannel.remove(0, n);
    }
    if (f.socketEof) {
        if (f.fdRegistered) {
            ssh_event_remove_fd(m_event, f.socket);
            f.fdRegistered = false;
        }
        if (f.toChannel.isEmpty() && !f.localClosed && ssh_channel_is_open(f.channel)) {
            ssh_channel_send_eof(f.channel);
            ssh_channel_close(f.channel);
            f.localClosed = true;
        }
    }
    // channel -> target socket
    while (!f.toSocket.isEmpty() && !f.socketEof) {
        const int n = socketSend(f.socket, f.toSocket.constData(), static_cast<int>(f.toSocket.size()));
        if (n < 0) {
            f.socketEof = true;
            f.toSocket.clear();
            break;
        }
        if (n == 0) {
            break;
        }
        f.toSocket.remove(0, n);
    }
    if (f.channelEof && f.toSocket.isEmpty() && !f.shutdownSent && !f.socketEof) {
        socketShutdownWrite(f.socket);
        f.shutdownSent = true;
    }
    if (f.remoteClosed && (f.localClosed || !ssh_channel_is_open(f.channel))) {
        finishForward(f);
    }
}

void ClientSession::finishForward(ForwardContext& f)
{
    if (f.finished) {
        return;
    }
    if (f.fdRegistered) {
        ssh_event_remove_fd(m_event, f.socket);
        f.fdRegistered = false;
    }
    if (f.socket != SSH_INVALID_SOCKET) {
        socketClose(f.socket);
        f.socket = SSH_INVALID_SOCKET;
    }
    if (f.channel) {
        ssh_remove_channel_callbacks(f.channel, &f.callbacks);
        ssh_channel_free(f.channel);
        f.channel = nullptr;
    }
    f.finished = true;
}

void ClientSession::teardown()
{
    for (auto& f : m_forwards) {
        finishForward(*f);
    }
    m_forwards.clear();
    for (auto& c : m_channels) {
        if (!c->finished && c->channel) {
            ssh_remove_channel_callbacks(c->channel, &c->callbacks);   // ssh_free frees the channel
            c->channel = nullptr;
            c->finished = true;
        }
    }
    if (m_event) {
        ssh_event_remove_session(m_event, m_session);
        ssh_event_free(m_event);
        m_event = nullptr;
    }
    {
        QMutexLocker lock(&m_core->mutex);
        m_fdOpen = false;
    }
    ssh_free(m_session);   // closes the socket
    m_session = nullptr;
    m_channels.clear();
    if (m_authorizedKey) {
        ssh_key_free(m_authorizedKey);
        m_authorizedKey = nullptr;
    }
}

void ClientSession::run()
{
    if (setup()) {
        serviceLoop();
    }
    teardown();
    qCDebug(lcSsh) << "TestSshServer: client thread finished";
    emit m_q->clientDisconnected();
}

// ---- Accept thread: a QTcpServer with its own event loop ----------------------------------------

class ListenServer : public QTcpServer
{
public:
    explicit ListenServer(ServerCore* core)
        : m_core(core)
    {
    }

protected:
    void incomingConnection(qintptr descriptor) override { m_core->onAccepted(descriptor); }

private:
    ServerCore* m_core;
};

class AcceptThread : public QThread
{
public:
    AcceptThread(ServerCore* core, quint16 port)
        : m_core(core)
        , m_requestedPort(port)
    {
        setObjectName(QStringLiteral("TestSshServer-accept"));
    }

    ~AcceptThread() override
    {
        quit();
        wait();
    }

    /// Blocks until the listener is up (or failed); returns the bound port, 0 on failure.
    quint16 waitReady()
    {
        m_ready.acquire();
        return m_boundPort;
    }

    QString error() const { return m_error; }

protected:
    void run() override
    {
        ListenServer server(m_core);
        if (!server.listen(QHostAddress::LocalHost, m_requestedPort)) {
            m_error = server.errorString();
            m_boundPort = 0;
            m_ready.release();
            return;
        }
        m_boundPort = server.serverPort();
        m_ready.release();
        exec();
        server.close();
    }

private:
    ServerCore* m_core;
    quint16 m_requestedPort;
    quint16 m_boundPort = 0;
    QString m_error;
    QSemaphore m_ready;
};

// ---- ServerCore ------------------------------------------------------------------------------

bool ServerCore::generateHostKey()
{
    ssh_key key = nullptr;
    if (ssh_pki_generate(SSH_KEYTYPE_ED25519, 0, &key) != SSH_OK || !key) {
        qCWarning(lcSsh) << "TestSshServer: ssh_pki_generate failed";
        return false;
    }
    freeHostKey();
    hostKey = key;
    hostKeyType = QString::fromLatin1(ssh_key_type_to_char(ssh_key_type(key)));
    hostKeyPublicLine = publicLineOf(key);
    hostKeyFingerprint = fingerprintOf(key);
    return !hostKeyPublicLine.isEmpty() && !hostKeyFingerprint.isEmpty();
}

void ServerCore::freeHostKey()
{
    if (hostKey) {
        ssh_key_free(hostKey);
        hostKey = nullptr;
    }
}

void ServerCore::pruneFinishedClients()
{
    for (auto it = clients.begin(); it != clients.end();) {
        if ((*it)->isFinished()) {
            delete *it;
            it = clients.erase(it);
        } else {
            ++it;
        }
    }
}

void ServerCore::onAccepted(qintptr descriptor)
{
    const auto fd = static_cast<socket_t>(descriptor);
    ClientSession* client = nullptr;
    int number = 0;
    {
        QMutexLocker lock(&mutex);
        pruneFinishedClients();
        if (!running || !bind) {
            socketClose(fd);
            return;
        }
        if (!acceptConnections) {
            qCDebug(lcSsh) << "TestSshServer: refusing a connection";
            socketResetAndClose(fd);
            return;
        }
        ssh_session session = ssh_new();
        if (!session) {
            socketClose(fd);
            return;
        }
        if (ssh_bind_accept_fd(bind, session, fd) != SSH_OK) {
            qCWarning(lcSsh) << "TestSshServer: ssh_bind_accept_fd failed:" << ssh_get_error(bind);
            ssh_free(session);
            return;
        }
        client = new ClientSession(this, session, fd);
        clients.append(client);
        number = ++connectionCount;
    }
    qCDebug(lcSsh) << "TestSshServer: accepted client" << number;
    client->start();
    emit q->clientConnected();
}

} // namespace

// ---------------------------------------------------------------------------------------
// TestSshServer
// ---------------------------------------------------------------------------------------

struct TestSshServer::Private : public ServerCore
{
};

TestSshServer::TestSshServer(const Options& options, QObject* parent)
    : QObject(parent)
    , d(std::make_unique<Private>())
{
    d->q = this;
    d->options = options;
    d->generateHostKey();
}

TestSshServer::~TestSshServer()
{
    stop();
    d->freeHostKey();
}

bool TestSshServer::start(QString* error)
{
    {
        QMutexLocker lock(&d->mutex);
        if (d->running) {
            return true;
        }
        if (!d->hostKey && !d->generateHostKey()) {
            if (error) {
                *error = QStringLiteral("cannot generate the host key");
            }
            return false;
        }

        ssh_bind sshBind = ssh_bind_new();
        if (!sshBind) {
            if (error) {
                *error = QStringLiteral("ssh_bind_new failed");
            }
            return false;
        }
        ssh_key bindKey = ssh_key_dup(d->hostKey);   // the bind owns the key it is given
        if (!bindKey || ssh_bind_options_set(sshBind, SSH_BIND_OPTIONS_IMPORT_KEY, bindKey) != SSH_OK) {
            if (error) {
                *error =
                    QStringLiteral("cannot import the host key: %1").arg(QString::fromUtf8(ssh_get_error(sshBind)));
            }
            ssh_key_free(bindKey);
            ssh_bind_free(sshBind);
            return false;
        }
        ssh_bind_options_set(sshBind, SSH_BIND_OPTIONS_BINDADDR, "127.0.0.1");

        auto listener = std::make_unique<AcceptThread>(d.get(), d->options.port);
        listener->start();
        const quint16 boundPort = listener->waitReady();
        if (boundPort == 0) {
            if (error) {
                *error = QStringLiteral("cannot listen on 127.0.0.1:%1: %2")
                             .arg(d->options.port)
                             .arg(listener->error());
            }
            listener.reset();
            ssh_bind_free(sshBind);
            return false;
        }

        d->bind = sshBind;
        d->port = boundPort;
        d->lastPort = boundPort;
        d->running = true;
        d->connectionCount = 0;
        d->lastAuthMethod.clear();
        d->lastTerm.clear();
        d->lastPtySize = QSize();
        d->receivedShellInput.clear();
        d->lastExecCommand.clear();
        d->acceptThread = std::move(listener);
        qCInfo(lcSsh) << "TestSshServer: listening on 127.0.0.1:" << boundPort << d->hostKeyFingerprint;
    }
    return true;
}

void TestSshServer::stop()
{
    std::unique_ptr<AcceptThread> listener;
    QList<ClientSession*> clients;
    ssh_bind sshBind = nullptr;
    {
        QMutexLocker lock(&d->mutex);
        if (!d->running && d->clients.isEmpty() && !d->acceptThread) {
            return;
        }
        d->running = false;
        listener = std::move(d->acceptThread);
        clients = d->clients;
        d->clients.clear();
        for (ClientSession* client : std::as_const(clients)) {
            client->requestStop();
        }
        sshBind = d->bind;
        d->bind = nullptr;
        d->port = 0;
    }
    listener.reset();   // quits the loop and joins
    for (ClientSession* client : std::as_const(clients)) {
        client->wait();
        delete client;
    }
    if (sshBind) {
        ssh_bind_free(sshBind);
    }
    qCInfo(lcSsh) << "TestSshServer: stopped";
}

bool TestSshServer::isRunning() const
{
    QMutexLocker lock(&d->mutex);
    return d->running;
}

quint16 TestSshServer::port() const
{
    QMutexLocker lock(&d->mutex);
    return d->running ? d->port : d->lastPort;
}

QString TestSshServer::hostKeyType() const
{
    QMutexLocker lock(&d->mutex);
    return d->hostKeyType;
}

QString TestSshServer::hostKeyFingerprintSha256() const
{
    QMutexLocker lock(&d->mutex);
    return d->hostKeyFingerprint;
}

QString TestSshServer::hostKeyPublicLine() const
{
    QMutexLocker lock(&d->mutex);
    return d->hostKeyPublicLine;
}

QString TestSshServer::knownHostsLine() const
{
    QMutexLocker lock(&d->mutex);
    return QStringLiteral("[127.0.0.1]:%1 %2").arg(d->running ? d->port : d->lastPort).arg(d->hostKeyPublicLine);
}

void TestSshServer::regenerateHostKey()
{
    QMutexLocker lock(&d->mutex);
    if (d->running) {
        qCWarning(lcSsh) << "TestSshServer: regenerateHostKey() ignored while running";
        return;
    }
    d->generateHostKey();
}

int TestSshServer::connectionCount() const
{
    QMutexLocker lock(&d->mutex);
    return d->connectionCount;
}

int TestSshServer::activeConnections() const
{
    QMutexLocker lock(&d->mutex);
    int active = 0;
    for (const ClientSession* client : std::as_const(d->clients)) {
        if (!client->isFinished()) {
            ++active;
        }
    }
    return active;
}

QString TestSshServer::lastAuthMethod() const
{
    QMutexLocker lock(&d->mutex);
    return d->lastAuthMethod;
}

QString TestSshServer::lastTerm() const
{
    QMutexLocker lock(&d->mutex);
    return d->lastTerm;
}

QSize TestSshServer::lastPtySize() const
{
    QMutexLocker lock(&d->mutex);
    return d->lastPtySize;
}

QByteArray TestSshServer::receivedShellInput() const
{
    QMutexLocker lock(&d->mutex);
    return d->receivedShellInput;
}

QString TestSshServer::lastExecCommand() const
{
    QMutexLocker lock(&d->mutex);
    return d->lastExecCommand;
}

void TestSshServer::dropAllClients()
{
    QMutexLocker lock(&d->mutex);
    int dropped = 0;
    for (ClientSession* client : std::as_const(d->clients)) {
        if (!client->isFinished()) {
            client->requestStop();
            ++dropped;
        }
    }
    qCInfo(lcSsh) << "TestSshServer: dropped" << dropped << "client(s)";
}

void TestSshServer::setAcceptConnections(bool accept)
{
    QMutexLocker lock(&d->mutex);
    if (d->acceptConnections == accept) {
        return;
    }
    d->acceptConnections = accept;
    qCInfo(lcSsh) << "TestSshServer:" << (accept ? "accepting connections again" : "refusing connections");
}

bool TestSshServer::generateClientKeyPair(const QString& privateKeyPath, QString* publicKeyLine,
                                          const QString& passphrase)
{
    ssh_key key = nullptr;
    if (ssh_pki_generate(SSH_KEYTYPE_ED25519, 0, &key) != SSH_OK || !key) {
        return false;
    }
    const QByteArray path = QFile::encodeName(privateKeyPath);
    const QByteArray pass = passphrase.toUtf8();
    const int rc = ssh_pki_export_privkey_file(key, passphrase.isEmpty() ? nullptr : pass.constData(), nullptr,
                                               nullptr, path.constData());
    const QString line = rc == SSH_OK ? publicLineOf(key) : QString();
    ssh_key_free(key);
    if (rc != SSH_OK || line.isEmpty()) {
        return false;
    }
    if (publicKeyLine) {
        *publicKeyLine = line;
    }
    return true;
}
