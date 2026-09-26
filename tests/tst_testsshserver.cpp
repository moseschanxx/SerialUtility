// Test suite for the in-process SSH server (tests/support/TestSshServer.h), driven with the
// libssh CLIENT API directly so it is independent of SshConnection. Every file (keys,
// known_hosts, SFTP root) lives in a temporary directory; the user's ~/.ssh is never read.
#include "ssh/LibsshInclude.h"   // winsock2.h before anything that might pull in windows.h

#include <QtTest>

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QRandomGenerator>
#include <QSemaphore>
#include <QSettings>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>

#include <fcntl.h>
#include <memory>

#include "support/TestSftpHandler.h"
#include "support/TestSshServer.h"

namespace {

constexpr int kTimeoutMs = 10000;

struct SessionDeleter
{
    void operator()(ssh_session s) const
    {
        if (s) {
            ssh_disconnect(s);
            ssh_free(s);
        }
    }
};
using SessionPtr = std::unique_ptr<ssh_session_struct, SessionDeleter>;

struct ChannelDeleter
{
    void operator()(ssh_channel c) const
    {
        if (c) {
            ssh_channel_free(c);
        }
    }
};
using ChannelPtr = std::unique_ptr<ssh_channel_struct, ChannelDeleter>;

struct KeyDeleter
{
    void operator()(ssh_key k) const
    {
        if (k) {
            ssh_key_free(k);
        }
    }
};
using KeyPtr = std::unique_ptr<ssh_key_struct, KeyDeleter>;

struct SftpDeleter
{
    void operator()(sftp_session s) const
    {
        if (s) {
            sftp_free(s);
        }
    }
};
using SftpPtr = std::unique_ptr<sftp_session_struct, SftpDeleter>;

/// Where a test client keeps its known_hosts and "ssh dir" (never the real ~/.ssh).
struct ClientEnv
{
    QString knownHosts;
    QString globalKnownHosts;
    QString sshDir;
};

SessionPtr newSession(quint16 port, const ClientEnv& env, const QString& user = QStringLiteral("test"),
                      long timeoutSeconds = 10)
{
    SessionPtr session(ssh_new());
    if (!session) {
        return nullptr;
    }
    ssh_session s = session.get();
    ssh_options_set(s, SSH_OPTIONS_HOST, "127.0.0.1");
    int portValue = port;
    ssh_options_set(s, SSH_OPTIONS_PORT, &portValue);
    const QByteArray userUtf8 = user.toUtf8();
    ssh_options_set(s, SSH_OPTIONS_USER, userUtf8.constData());
    bool processConfig = false;
    ssh_options_set(s, SSH_OPTIONS_PROCESS_CONFIG, &processConfig);
    const QByteArray sshDir = QFile::encodeName(env.sshDir);
    ssh_options_set(s, SSH_OPTIONS_SSH_DIR, sshDir.constData());
    const QByteArray knownHosts = QFile::encodeName(env.knownHosts);
    ssh_options_set(s, SSH_OPTIONS_KNOWNHOSTS, knownHosts.constData());
    const QByteArray globalKnownHosts = QFile::encodeName(env.globalKnownHosts);
    ssh_options_set(s, SSH_OPTIONS_GLOBAL_KNOWNHOSTS, globalKnownHosts.constData());
    long timeout = timeoutSeconds;
    ssh_options_set(s, SSH_OPTIONS_TIMEOUT, &timeout);
    int verbosity = SSH_LOG_NOLOG;
    ssh_options_set(s, SSH_OPTIONS_LOG_VERBOSITY, &verbosity);
    return session;
}

/// ssh_new + ssh_connect; null when the connection failed (message in *error).
SessionPtr connectTo(quint16 port, const ClientEnv& env, QString* error = nullptr,
                     const QString& user = QStringLiteral("test"), long timeoutSeconds = 10)
{
    SessionPtr session = newSession(port, env, user, timeoutSeconds);
    if (!session) {
        return nullptr;
    }
    if (ssh_connect(session.get()) != SSH_OK) {
        if (error) {
            *error = QString::fromUtf8(ssh_get_error(session.get()));
        }
        return nullptr;
    }
    return session;
}

/// Read until `marker` shows up in the accumulated output, EOF, an error or the timeout.
QByteArray readUntil(ssh_channel channel, const QByteArray& marker, int timeoutMs = kTimeoutMs, bool* eof = nullptr)
{
    QByteArray out;
    QDeadlineTimer deadline(timeoutMs);
    while (!out.contains(marker) && !deadline.hasExpired()) {
        char buf[4096];
        const int n = ssh_channel_read_timeout(channel, buf, sizeof(buf), 0, 100);
        if (n > 0) {
            out.append(buf, n);
        } else if (n == SSH_ERROR) {
            break;
        } else if (n == 0) {
            if (eof) {
                *eof = true;
            }
            break;
        }
    }
    return out;
}

/// Read until EOF (or an error / the timeout); *eof tells whether EOF was reached.
QByteArray readUntilEof(ssh_channel channel, bool* eof, int timeoutMs = kTimeoutMs)
{
    QByteArray out;
    *eof = false;
    QDeadlineTimer deadline(timeoutMs);
    while (!deadline.hasExpired()) {
        char buf[4096];
        const int n = ssh_channel_read_timeout(channel, buf, sizeof(buf), 0, 100);
        if (n > 0) {
            out.append(buf, n);
        } else if (n == 0) {
            *eof = true;
            break;
        } else if (n == SSH_ERROR) {
            break;
        }
    }
    return out;
}

bool writeAll(ssh_channel channel, const QByteArray& data)
{
    qsizetype done = 0;
    while (done < data.size()) {
        const int n = ssh_channel_write(channel, data.constData() + done, static_cast<uint32_t>(data.size() - done));
        if (n <= 0) {
            return false;
        }
        done += n;
    }
    return true;
}

/// Exit status of a channel once the server sent it (blocks up to the session timeout).
int exitStatusOf(ssh_channel channel)
{
    uint32_t code = 0;
    if (ssh_channel_get_exit_state(channel, &code, nullptr, nullptr) != SSH_OK) {
        return -1;
    }
    return static_cast<int>(code);
}

/// Authenticated shell channel with a PTY; null on failure.
ChannelPtr openShell(ssh_session session, const char* term = "xterm-256color", int cols = 100, int rows = 40)
{
    ChannelPtr channel(ssh_channel_new(session));
    if (!channel || ssh_channel_open_session(channel.get()) != SSH_OK) {
        return nullptr;
    }
    if (ssh_channel_request_pty_size(channel.get(), term, cols, rows) != SSH_OK) {
        return nullptr;
    }
    if (ssh_channel_request_shell(channel.get()) != SSH_OK) {
        return nullptr;
    }
    return channel;
}

QString fingerprintOf(ssh_key key)
{
    unsigned char* hash = nullptr;
    size_t length = 0;
    if (ssh_get_publickey_hash(key, SSH_PUBLICKEY_HASH_SHA256, &hash, &length) != SSH_OK) {
        return {};
    }
    char* text = ssh_get_fingerprint_hash(SSH_PUBLICKEY_HASH_SHA256, hash, length);
    ssh_clean_pubkey_hash(&hash);
    const QString result = QString::fromUtf8(text);
    ssh_string_free_char(text);
    return result;
}

QByteArray randomBytes(qsizetype size)
{
    QByteArray bytes(size, Qt::Uninitialized);
    QRandomGenerator* generator = QRandomGenerator::global();
    quint32 word = 0;
    for (qsizetype i = 0; i < size; ++i) {
        if (i % 4 == 0) {
            word = generator->generate();
        }
        bytes[i] = static_cast<char>(word >> ((i % 4) * 8));
    }
    return bytes;
}

/// Deliver every queued signal that is already posted (a server destroyed at the end of a test
/// may have emitted its last clientDisconnected() a moment before).
void drainEvents()
{
    for (int i = 0; i < 5; ++i) {
        QCoreApplication::sendPostedEvents();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
}

/// A TCP echo server on its own thread (the test thread is busy inside blocking libssh calls).
class EchoServer : public QThread
{
public:
    ~EchoServer() override
    {
        quit();
        wait();
    }

    quint16 port()
    {
        m_ready.acquire();
        return m_port;
    }

protected:
    void run() override
    {
        QTcpServer server;
        if (server.listen(QHostAddress::LocalHost)) {
            m_port = server.serverPort();
        }
        QObject::connect(&server, &QTcpServer::newConnection, &server, [&server] {
            while (QTcpSocket* socket = server.nextPendingConnection()) {
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket] { socket->write(socket->readAll()); });
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
        m_ready.release();
        exec();
    }

private:
    QSemaphore m_ready;
    quint16 m_port = 0;
};

} // namespace

class Tst_testsshserver : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void startStopTwice();
    void knownHostsLineMatchesServer();
    void passwordAuthShell();
    void wrongPasswordRejected();
    void publicKeyAuth();
    void publicKeyAuthPassphrase();
    void keyboardInteractive();
    void banner();
    void execCommand();
    void bigOutput();
    void hangThenDrop();
    void refuseConnections();
    void sftpRoundTrip();
    void sftpHandlerUnit();
    void directTcpip();
    void regenerateHostKey();
    void threeConcurrentClients();
    void stopWhileConnected();
    void keepAliveDropAfter();

private:
    struct Counters
    {
        int connected = 0;
        int disconnected = 0;
        int shell = 0;
        int exec = 0;
        int sftp = 0;
        int forwards = 0;
        QStringList authMethods;
        QStringList execCommands;
        QList<QPair<QString, quint16>> forwardTargets;
    };

    /// Connect the server's signals to counters updated on the test thread (queued delivery).
    void attach(TestSshServer* server);
    ClientEnv env() const;
    TestSshServer::Options options() const;

    std::unique_ptr<QTemporaryDir> m_tmp;
    Counters m_counters;
};

void Tst_testsshserver::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("BuildAI-Test"));
    QCoreApplication::setApplicationName(QStringLiteral("SerialUtilityTest-testsshserver"));
    m_tmp = std::make_unique<QTemporaryDir>();
    QVERIFY(m_tmp->isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_tmp->filePath(QStringLiteral("settings")));
    QSettings settings;
    settings.clear();
    QVERIFY(settings.fileName().contains(QStringLiteral("BuildAI-Test")));
}

void Tst_testsshserver::init()
{
    drainEvents();
    m_tmp = std::make_unique<QTemporaryDir>();
    QVERIFY(m_tmp->isValid());
    QVERIFY(QDir(m_tmp->path()).mkpath(QStringLiteral("sshdir")));
    QVERIFY(QDir(m_tmp->path()).mkpath(QStringLiteral("root")));
    m_counters = Counters();
}

void Tst_testsshserver::cleanup()
{
    drainEvents();   // the test's server is gone by now; its last signals are still queued
    m_tmp.reset();
}

ClientEnv Tst_testsshserver::env() const
{
    ClientEnv e;
    e.knownHosts = m_tmp->filePath(QStringLiteral("known_hosts"));
    e.globalKnownHosts = m_tmp->filePath(QStringLiteral("global_known_hosts"));
    e.sshDir = m_tmp->filePath(QStringLiteral("sshdir"));
    return e;
}

TestSshServer::Options Tst_testsshserver::options() const
{
    TestSshServer::Options o;
    o.rootDir = m_tmp->filePath(QStringLiteral("root"));
    return o;
}

void Tst_testsshserver::attach(TestSshServer* server)
{
    drainEvents();
    m_counters = Counters();
    connect(server, &TestSshServer::clientConnected, this, [this] { ++m_counters.connected; });
    connect(server, &TestSshServer::clientDisconnected, this, [this] { ++m_counters.disconnected; });
    connect(server, &TestSshServer::clientAuthenticated, this,
            [this](const QString& method) { m_counters.authMethods.append(method); });
    connect(server, &TestSshServer::shellRequested, this, [this] { ++m_counters.shell; });
    connect(server, &TestSshServer::execRequested, this, [this](const QString& command) {
        ++m_counters.exec;
        m_counters.execCommands.append(command);
    });
    connect(server, &TestSshServer::sftpRequested, this, [this] { ++m_counters.sftp; });
    connect(server, &TestSshServer::forwardRequested, this, [this](const QString& host, quint16 port) {
        ++m_counters.forwards;
        m_counters.forwardTargets.append({host, port});
    });
}

// ---------------------------------------------------------------------------------------

void Tst_testsshserver::startStopTwice()
{
    TestSshServer server(options());
    QVERIFY(!server.isRunning());
    QCOMPARE(server.hostKeyType(), QStringLiteral("ssh-ed25519"));
    QVERIFY(server.hostKeyFingerprintSha256().startsWith(QStringLiteral("SHA256:")));
    QVERIFY(server.hostKeyPublicLine().startsWith(QStringLiteral("ssh-ed25519 AAAA")));

    QString error;
    QVERIFY2(server.start(&error), qPrintable(error));
    QVERIFY(server.isRunning());
    const quint16 port1 = server.port();
    QVERIFY(port1 != 0);
    QCOMPARE(server.knownHostsLine(), QStringLiteral("[127.0.0.1]:%1 %2").arg(port1).arg(server.hostKeyPublicLine()));
    QVERIFY(server.start());   // already running: no-op

    {
        SessionPtr session = connectTo(port1, env(), &error);
        QVERIFY2(session, qPrintable(error));
        QCOMPARE(ssh_userauth_password(session.get(), nullptr, "secret"), SSH_AUTH_SUCCESS);
    }
    QTRY_COMPARE_WITH_TIMEOUT(server.activeConnections(), 0, kTimeoutMs);
    QCOMPARE(server.connectionCount(), 1);

    server.stop();
    QVERIFY(!server.isRunning());
    server.stop();   // idempotent

    QVERIFY2(server.start(&error), qPrintable(error));
    QVERIFY(server.isRunning());
    QCOMPARE(server.connectionCount(), 0);
    {
        SessionPtr session = connectTo(server.port(), env(), &error);
        QVERIFY2(session, qPrintable(error));
        QCOMPARE(ssh_userauth_password(session.get(), nullptr, "secret"), SSH_AUTH_SUCCESS);
    }
    QTRY_COMPARE_WITH_TIMEOUT(server.activeConnections(), 0, kTimeoutMs);
    QCOMPARE(server.connectionCount(), 1);
    server.stop();
    QVERIFY(!server.isRunning());
}

void Tst_testsshserver::knownHostsLineMatchesServer()
{
    TestSshServer server(options());
    QVERIFY(server.start());
    const ClientEnv e = env();
    {
        QFile file(e.knownHosts);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write(server.knownHostsLine().toUtf8() + "\n");
    }
    QString error;
    SessionPtr session = connectTo(server.port(), e, &error);
    QVERIFY2(session, qPrintable(error));
    QCOMPARE(ssh_session_is_known_server(session.get()), SSH_KNOWN_HOSTS_OK);

    ssh_key key = nullptr;
    QCOMPARE(ssh_get_server_publickey(session.get(), &key), SSH_OK);
    KeyPtr serverKey(key);
    QCOMPARE(fingerprintOf(serverKey.get()), server.hostKeyFingerprintSha256());
    QCOMPARE(QString::fromLatin1(ssh_key_type_to_char(ssh_key_type(serverKey.get()))), server.hostKeyType());
}

void Tst_testsshserver::passwordAuthShell()
{
    TestSshServer server(options());
    attach(&server);
    QString error;
    QVERIFY2(server.start(&error), qPrintable(error));

    SessionPtr session = connectTo(server.port(), env(), &error);
    QVERIFY2(session, qPrintable(error));
    QTRY_COMPARE_WITH_TIMEOUT(m_counters.connected, 1, kTimeoutMs);

    QCOMPARE(ssh_userauth_none(session.get(), nullptr), SSH_AUTH_DENIED);
    const int methods = ssh_userauth_list(session.get(), nullptr);
    QVERIFY(methods & SSH_AUTH_METHOD_PASSWORD);
    QVERIFY(methods & SSH_AUTH_METHOD_PUBLICKEY);
    QVERIFY(!(methods & SSH_AUTH_METHOD_INTERACTIVE));
    QCOMPARE(ssh_userauth_password(session.get(), nullptr, "secret"), SSH_AUTH_SUCCESS);
    QTRY_COMPARE_WITH_TIMEOUT(server.lastAuthMethod(), QStringLiteral("password"), kTimeoutMs);
    QTRY_COMPARE_WITH_TIMEOUT(m_counters.authMethods, QStringList{QStringLiteral("password")}, kTimeoutMs);

    ChannelPtr channel = openShell(session.get(), "xterm-256color", 100, 40);
    QVERIFY(channel);
    QTRY_COMPARE_WITH_TIMEOUT(m_counters.shell, 1, kTimeoutMs);
    QTRY_COMPARE_WITH_TIMEOUT(server.lastTerm(), QStringLiteral("xterm-256color"), kTimeoutMs);
    QTRY_COMPARE_WITH_TIMEOUT(server.lastPtySize(), QSize(100, 40), kTimeoutMs);

    QByteArray out = readUntil(channel.get(), "$ ");
    QVERIFY2(out.contains("welcome\r\n$ "), out.constData());

    QVERIFY(writeAll(channel.get(), "echo hi\r"));
    out = readUntil(channel.get(), "hi\r\n$ ");
    QCOMPARE(out, QByteArray("echo hi\r\nhi\r\n$ "));
    QTRY_VERIFY_WITH_TIMEOUT(server.receivedShellInput().contains("echo hi\r"), kTimeoutMs);

    // 0x7F erases: "sizX<DEL>e" runs "size".
    QVERIFY(writeAll(channel.get(), "sizX\x7f" "e\r"));
    out = readUntil(channel.get(), "$ ");
    QCOMPARE(out, QByteArray("sizX\b \be\r\nCOLS=100 ROWS=40\r\n$ "));

    QCOMPARE(ssh_channel_change_pty_size(channel.get(), 132, 50), SSH_OK);
    QTRY_COMPARE_WITH_TIMEOUT(server.lastPtySize(), QSize(132, 50), kTimeoutMs);
    QVERIFY(writeAll(channel.get(), "size\r"));
    out = readUntil(channel.get(), "$ ");
    QCOMPARE(out, QByteArray("size\r\nCOLS=132 ROWS=50\r\n$ "));

    QVERIFY(writeAll(channel.get(), "env\r"));
    out = readUntil(channel.get(), "$ ");
    QCOMPARE(out, QByteArray("env\r\nTERM=xterm-256color COLS=132 ROWS=50\r\n$ "));

    QVERIFY(writeAll(channel.get(), "frobnicate now\r"));
    out = readUntil(channel.get(), "$ ");
    QCOMPARE(out, QByteArray("frobnicate now\r\nfrobnicate: not found\r\n$ "));

    // A bare LF runs the line too; the LF of a CR LF pair does not produce an extra prompt.
    QVERIFY(writeAll(channel.get(), "echo a\r\necho b\n"));
    out = readUntil(channel.get(), "b\r\n$ ");
    QCOMPARE(out, QByteArray("echo a\r\na\r\n$ echo b\r\nb\r\n$ "));

    QElapsedTimer sleepTimer;
    sleepTimer.start();
    QVERIFY(writeAll(channel.get(), "sleep 300\r"));
    out = readUntil(channel.get(), "$ ");
    QCOMPARE(out, QByteArray("sleep 300\r\n$ "));
    QVERIFY(sleepTimer.elapsed() >= 250);

    QVERIFY(writeAll(channel.get(), "exit 3\r"));
    bool eof = false;
    out = readUntilEof(channel.get(), &eof);
    QVERIFY2(eof, out.constData());
    QCOMPARE(out, QByteArray("exit 3\r\n"));
    QCOMPARE(exitStatusOf(channel.get()), 3);
    QTRY_VERIFY_WITH_TIMEOUT(server.receivedShellInput().endsWith("exit 3\r"), kTimeoutMs);

    channel.reset();
    session.reset();   // ssh_disconnect + ssh_free
    QTRY_COMPARE_WITH_TIMEOUT(m_counters.disconnected, 1, kTimeoutMs);
    QCOMPARE(server.activeConnections(), 0);
    QCOMPARE(server.connectionCount(), 1);
}

void Tst_testsshserver::wrongPasswordRejected()
{
    TestSshServer server(options());
    attach(&server);
    QVERIFY(server.start());
    QString error;
    SessionPtr session = connectTo(server.port(), env(), &error);
    QVERIFY2(session, qPrintable(error));
    QCOMPARE(ssh_userauth_password(session.get(), nullptr, "wrong"), SSH_AUTH_DENIED);
    QTest::qWait(100);
    QVERIFY(server.lastAuthMethod().isEmpty());
    QVERIFY(m_counters.authMethods.isEmpty());

    // The wrong user is refused as well, the right pair still works on the same session.
    SessionPtr other = connectTo(server.port(), env(), &error, QStringLiteral("nobody"));
    QVERIFY2(other, qPrintable(error));
    QCOMPARE(ssh_userauth_password(other.get(), nullptr, "secret"), SSH_AUTH_DENIED);
    QCOMPARE(ssh_userauth_password(session.get(), nullptr, "secret"), SSH_AUTH_SUCCESS);
    QTRY_COMPARE_WITH_TIMEOUT(server.lastAuthMethod(), QStringLiteral("password"), kTimeoutMs);
}

void Tst_testsshserver::publicKeyAuth()
{
    const QString keyPath = m_tmp->filePath(QStringLiteral("id_ed25519"));
    QString publicLine;
    QVERIFY(TestSshServer::generateClientKeyPair(keyPath, &publicLine));
    QVERIFY(publicLine.startsWith(QStringLiteral("ssh-ed25519 ")));
    QVERIFY(QFileInfo::exists(keyPath));

    TestSshServer::Options o = options();
    o.authorizedPublicKey = publicLine;
    o.allowPassword = false;
    TestSshServer server(o);
    attach(&server);
    QVERIFY(server.start());

    ssh_key raw = nullptr;
    QCOMPARE(ssh_pki_import_privkey_file(QFile::encodeName(keyPath).constData(), nullptr, nullptr, nullptr, &raw),
             SSH_OK);
    KeyPtr key(raw);

    QString error;
    SessionPtr session = connectTo(server.port(), env(), &error);
    QVERIFY2(session, qPrintable(error));
    QCOMPARE(ssh_userauth_none(session.get(), nullptr), SSH_AUTH_DENIED);
    const int methods = ssh_userauth_list(session.get(), nullptr);
    QVERIFY(methods & SSH_AUTH_METHOD_PUBLICKEY);
    QVERIFY(!(methods & SSH_AUTH_METHOD_PASSWORD));
    QCOMPARE(ssh_userauth_try_publickey(session.get(), nullptr, key.get()), SSH_AUTH_SUCCESS);
    QCOMPARE(ssh_userauth_publickey(session.get(), nullptr, key.get()), SSH_AUTH_SUCCESS);
    QTRY_COMPARE_WITH_TIMEOUT(server.lastAuthMethod(), QStringLiteral("publickey"), kTimeoutMs);
    QTRY_COMPARE_WITH_TIMEOUT(m_counters.authMethods, QStringList{QStringLiteral("publickey")}, kTimeoutMs);

    ChannelPtr channel = openShell(session.get());
    QVERIFY(channel);
    QVERIFY(readUntil(channel.get(), "$ ").contains("welcome"));

    // Another key of the same type is refused, both by the probe and by the signed request.
    const QString otherPath = m_tmp->filePath(QStringLiteral("id_other"));
    QString otherLine;
    QVERIFY(TestSshServer::generateClientKeyPair(otherPath, &otherLine));
    QVERIFY(otherLine != publicLine);
    ssh_key otherRaw = nullptr;
    QCOMPARE(ssh_pki_import_privkey_file(QFile::encodeName(otherPath).constData(), nullptr, nullptr, nullptr,
                                         &otherRaw),
             SSH_OK);
    KeyPtr otherKey(otherRaw);
    SessionPtr second = connectTo(server.port(), env(), &error);
    QVERIFY2(second, qPrintable(error));
    QCOMPARE(ssh_userauth_try_publickey(second.get(), nullptr, otherKey.get()), SSH_AUTH_DENIED);
    QCOMPARE(ssh_userauth_publickey(second.get(), nullptr, otherKey.get()), SSH_AUTH_DENIED);
    QCOMPARE(ssh_userauth_password(second.get(), nullptr, "secret"), SSH_AUTH_DENIED);   // not offered
}

void Tst_testsshserver::publicKeyAuthPassphrase()
{
    const QString keyPath = m_tmp->filePath(QStringLiteral("id_protected"));
    QString publicLine;
    QVERIFY(TestSshServer::generateClientKeyPair(keyPath, &publicLine, QStringLiteral("correct horse")));

    ssh_key raw = nullptr;
    // Without the passphrase the key cannot be read...
    QVERIFY(ssh_pki_import_privkey_file(QFile::encodeName(keyPath).constData(), nullptr, nullptr, nullptr, &raw) !=
            SSH_OK);
    if (raw) {
        ssh_key_free(raw);
        raw = nullptr;
    }
    // ...with a wrong one neither...
    QVERIFY(ssh_pki_import_privkey_file(QFile::encodeName(keyPath).constData(), "wrong", nullptr, nullptr, &raw) !=
            SSH_OK);
    if (raw) {
        ssh_key_free(raw);
        raw = nullptr;
    }
    // ...with the right one it is.
    QCOMPARE(ssh_pki_import_privkey_file(QFile::encodeName(keyPath).constData(), "correct horse", nullptr, nullptr,
                                         &raw),
             SSH_OK);
    KeyPtr key(raw);

    TestSshServer::Options o = options();
    o.authorizedPublicKey = publicLine;
    TestSshServer server(o);
    QVERIFY(server.start());
    QString error;
    SessionPtr session = connectTo(server.port(), env(), &error);
    QVERIFY2(session, qPrintable(error));
    QCOMPARE(ssh_userauth_publickey(session.get(), nullptr, key.get()), SSH_AUTH_SUCCESS);
    QTRY_COMPARE_WITH_TIMEOUT(server.lastAuthMethod(), QStringLiteral("publickey"), kTimeoutMs);
}

void Tst_testsshserver::keyboardInteractive()
{
    TestSshServer::Options o = options();
    o.allowPassword = false;
    o.allowPublicKey = false;
    o.allowKeyboardInteractive = true;
    o.kbdintPrompt = QStringLiteral("Token: ");
    o.kbdintAnswer = QStringLiteral("424242");
    TestSshServer server(o);
    attach(&server);
    QVERIFY(server.start());

    QString error;
    SessionPtr session = connectTo(server.port(), env(), &error);
    QVERIFY2(session, qPrintable(error));
    QCOMPARE(ssh_userauth_none(session.get(), nullptr), SSH_AUTH_DENIED);
    QCOMPARE(ssh_userauth_list(session.get(), nullptr), static_cast<int>(SSH_AUTH_METHOD_INTERACTIVE));

    int rc = ssh_userauth_kbdint(session.get(), nullptr, nullptr);
    QCOMPARE(rc, SSH_AUTH_INFO);
    QCOMPARE(ssh_userauth_kbdint_getnprompts(session.get()), 1);
    QCOMPARE(QString::fromUtf8(ssh_userauth_kbdint_getname(session.get())), QStringLiteral("TestSshServer"));
    char echo = 1;
    QCOMPARE(QString::fromUtf8(ssh_userauth_kbdint_getprompt(session.get(), 0, &echo)), QStringLiteral("Token: "));
    QCOMPARE(echo, static_cast<char>(0));
    QCOMPARE(ssh_userauth_kbdint_setanswer(session.get(), 0, "424242"), 0);
    rc = ssh_userauth_kbdint(session.get(), nullptr, nullptr);
    // libssh may report an empty info round before the success.
    for (int i = 0; i < 3 && rc == SSH_AUTH_INFO && ssh_userauth_kbdint_getnprompts(session.get()) == 0; ++i) {
        rc = ssh_userauth_kbdint(session.get(), nullptr, nullptr);
    }
    QCOMPARE(rc, SSH_AUTH_SUCCESS);
    QTRY_COMPARE_WITH_TIMEOUT(server.lastAuthMethod(), QStringLiteral("keyboard-interactive"), kTimeoutMs);
    QTRY_COMPARE_WITH_TIMEOUT(m_counters.authMethods, QStringList{QStringLiteral("keyboard-interactive")}, kTimeoutMs);
    ChannelPtr channel = openShell(session.get());
    QVERIFY(channel);
    QVERIFY(readUntil(channel.get(), "$ ").contains("welcome"));

    // The wrong answer is rejected and the server offers the method again.
    SessionPtr second = connectTo(server.port(), env(), &error);
    QVERIFY2(second, qPrintable(error));
    rc = ssh_userauth_kbdint(second.get(), nullptr, nullptr);
    QCOMPARE(rc, SSH_AUTH_INFO);
    QCOMPARE(ssh_userauth_kbdint_setanswer(second.get(), 0, "000000"), 0);
    rc = ssh_userauth_kbdint(second.get(), nullptr, nullptr);
    for (int i = 0; i < 3 && rc == SSH_AUTH_INFO && ssh_userauth_kbdint_getnprompts(second.get()) == 0; ++i) {
        rc = ssh_userauth_kbdint(second.get(), nullptr, nullptr);
    }
    QCOMPARE(rc, SSH_AUTH_DENIED);
    QVERIFY(ssh_userauth_list(second.get(), nullptr) & SSH_AUTH_METHOD_INTERACTIVE);
}

void Tst_testsshserver::banner()
{
    TestSshServer::Options o = options();
    o.banner = QStringLiteral("Authorized use only.\nThis is a test server.\n");
    TestSshServer server(o);
    QVERIFY(server.start());
    QString error;
    SessionPtr session = connectTo(server.port(), env(), &error);
    QVERIFY2(session, qPrintable(error));
    QCOMPARE(ssh_userauth_none(session.get(), nullptr), SSH_AUTH_DENIED);
    char* text = ssh_get_issue_banner(session.get());
    QVERIFY2(text, "no banner received");
    const QString received = QString::fromUtf8(text);
    ssh_string_free_char(text);
    QCOMPARE(received, o.banner);
    QCOMPARE(ssh_userauth_password(session.get(), nullptr, "secret"), SSH_AUTH_SUCCESS);

    // Without a banner nothing is sent.
    TestSshServer plain(options());
    QVERIFY(plain.start());
    SessionPtr other = connectTo(plain.port(), env(), &error);
    QVERIFY2(other, qPrintable(error));
    QCOMPARE(ssh_userauth_none(other.get(), nullptr), SSH_AUTH_DENIED);
    QVERIFY(ssh_get_issue_banner(other.get()) == nullptr);
}

void Tst_testsshserver::execCommand()
{
    TestSshServer server(options());
    attach(&server);
    QVERIFY(server.start());
    QString error;
    SessionPtr session = connectTo(server.port(), env(), &error);
    QVERIFY2(session, qPrintable(error));
    QCOMPARE(ssh_userauth_password(session.get(), nullptr, "secret"), SSH_AUTH_SUCCESS);

    {
        ChannelPtr channel(ssh_channel_new(session.get()));
        QVERIFY(channel);
        QCOMPARE(ssh_channel_open_session(channel.get()), SSH_OK);
        QCOMPARE(ssh_channel_request_exec(channel.get(), "echo exec-ok"), SSH_OK);
        bool eof = false;
        const QByteArray out = readUntilEof(channel.get(), &eof);
        QVERIFY(eof);
        QCOMPARE(out, QByteArray("exec-ok\r\n"));
        QCOMPARE(exitStatusOf(channel.get()), 0);
        QTRY_COMPARE_WITH_TIMEOUT(server.lastExecCommand(), QStringLiteral("echo exec-ok"), kTimeoutMs);
        QTRY_COMPARE_WITH_TIMEOUT(m_counters.execCommands, QStringList{QStringLiteral("echo exec-ok")}, kTimeoutMs);
    }
    {
        ChannelPtr channel(ssh_channel_new(session.get()));
        QVERIFY(channel);
        QCOMPARE(ssh_channel_open_session(channel.get()), SSH_OK);
        QCOMPARE(ssh_channel_request_pty_size(channel.get(), "vt100", 80, 24), SSH_OK);
        QCOMPARE(ssh_channel_request_exec(channel.get(), "exit 7"), SSH_OK);
        bool eof = false;
        const QByteArray out = readUntilEof(channel.get(), &eof);
        QVERIFY(eof);
        QVERIFY(out.isEmpty());
        QCOMPARE(exitStatusOf(channel.get()), 7);
    }
    {
        ChannelPtr channel(ssh_channel_new(session.get()));
        QVERIFY(channel);
        QCOMPARE(ssh_channel_open_session(channel.get()), SSH_OK);
        QCOMPARE(ssh_channel_request_exec(channel.get(), "bogus"), SSH_OK);
        bool eof = false;
        const QByteArray out = readUntilEof(channel.get(), &eof);
        QVERIFY(eof);
        QCOMPARE(out, QByteArray("bogus: not found\r\n"));
        QCOMPARE(exitStatusOf(channel.get()), 0);
    }
    QTRY_COMPARE_WITH_TIMEOUT(m_counters.exec, 3, kTimeoutMs);
    QCOMPARE(m_counters.shell, 0);
}

void Tst_testsshserver::bigOutput()
{
    TestSshServer server(options());
    QVERIFY(server.start());
    QString error;
    SessionPtr session = connectTo(server.port(), env(), &error);
    QVERIFY2(session, qPrintable(error));
    QCOMPARE(ssh_userauth_password(session.get(), nullptr, "secret"), SSH_AUTH_SUCCESS);
    ChannelPtr channel = openShell(session.get());
    QVERIFY(channel);
    QVERIFY(readUntil(channel.get(), "$ ").endsWith("$ "));

    QVERIFY(writeAll(channel.get(), "big 2000\r"));
    const QByteArray out = readUntil(channel.get(), "line 2000\r\n$ ", 30000);
    QVERIFY(out.startsWith("big 2000\r\n"));
    QVERIFY(out.endsWith("line 2000\r\n$ "));
    const QList<QByteArray> lines = out.mid(10).split('\n');
    QCOMPARE(lines.size(), 2001);   // 2000 lines + the prompt
    for (int k = 1; k <= 2000; ++k) {
        QCOMPARE(lines.at(k - 1), QByteArray("line ") + QByteArray::number(k) + "\r");
    }
    QCOMPARE(lines.last(), QByteArray("$ "));

    // The shell is still responsive afterwards.
    QVERIFY(writeAll(channel.get(), "echo after\r"));
    QCOMPARE(readUntil(channel.get(), "after\r\n$ "), QByteArray("echo after\r\nafter\r\n$ "));
}

void Tst_testsshserver::hangThenDrop()
{
    TestSshServer server(options());
    attach(&server);
    QVERIFY(server.start());
    QString error;
    SessionPtr session = connectTo(server.port(), env(), &error);
    QVERIFY2(session, qPrintable(error));
    QCOMPARE(ssh_userauth_password(session.get(), nullptr, "secret"), SSH_AUTH_SUCCESS);
    ChannelPtr channel = openShell(session.get());
    QVERIFY(channel);
    QVERIFY(readUntil(channel.get(), "$ ").endsWith("$ "));
    QTRY_COMPARE_WITH_TIMEOUT(server.activeConnections(), 1, kTimeoutMs);

    QVERIFY(writeAll(channel.get(), "hang\r"));
    QCOMPARE(readUntil(channel.get(), "hang\r\n"), QByteArray("hang\r\n"));
    // Nothing comes back any more, not even for further commands...
    QVERIFY(writeAll(channel.get(), "echo x\r"));
    char buf[256];
    QCOMPARE(ssh_channel_read_timeout(channel.get(), buf, sizeof(buf), 0, 500), SSH_AGAIN);
    QTRY_VERIFY_WITH_TIMEOUT(server.receivedShellInput().endsWith("echo x\r"), kTimeoutMs);
    QVERIFY(ssh_is_connected(session.get()));

    // ...until the server cuts the connection: the read fails within two seconds.
    QElapsedTimer timer;
    timer.start();
    server.dropAllClients();
    const int rc = ssh_channel_read_timeout(channel.get(), buf, sizeof(buf), 0, 5000);
    QVERIFY2(rc == SSH_ERROR || rc == 0, "read did not fail after the drop");
    QVERIFY2(timer.elapsed() < 2000, qPrintable(QString::number(timer.elapsed())));
    QVERIFY(!ssh_is_connected(session.get()));
    QTRY_COMPARE_WITH_TIMEOUT(server.activeConnections(), 0, kTimeoutMs);
    QTRY_COMPARE_WITH_TIMEOUT(m_counters.disconnected, 1, kTimeoutMs);

    // The server is still there for the next client.
    SessionPtr again = connectTo(server.port(), env(), &error);
    QVERIFY2(again, qPrintable(error));
    QCOMPARE(ssh_userauth_password(again.get(), nullptr, "secret"), SSH_AUTH_SUCCESS);
}

void Tst_testsshserver::refuseConnections()
{
    TestSshServer server(options());
    QVERIFY(server.start());
    const quint16 port = server.port();

    server.setAcceptConnections(false);
    QString error;
    QElapsedTimer timer;
    timer.start();
    SessionPtr refused = connectTo(port, env(), &error);
    QVERIFY2(!refused, "connection was accepted although refused");
    QVERIFY2(timer.elapsed() < 5000, qPrintable(QString::number(timer.elapsed())));
    QVERIFY(server.isRunning());
    QCOMPARE(server.port(), port);
    QCOMPARE(server.connectionCount(), 0);

    server.setAcceptConnections(true);
    SessionPtr session = connectTo(port, env(), &error);
    QVERIFY2(session, qPrintable(error));
    QCOMPARE(ssh_userauth_password(session.get(), nullptr, "secret"), SSH_AUTH_SUCCESS);
    QCOMPARE(server.connectionCount(), 1);

    // Set before start(): honoured by start().
    TestSshServer late(options());
    late.setAcceptConnections(false);
    QVERIFY(late.start());
    QVERIFY(!connectTo(late.port(), env(), &error));
    late.setAcceptConnections(true);
    QVERIFY2(connectTo(late.port(), env(), &error), qPrintable(error));
}

void Tst_testsshserver::sftpRoundTrip()
{
    TestSshServer::Options o = options();
    TestSshServer server(o);
    attach(&server);
    QVERIFY(server.start());
    QString error;
    SessionPtr session = connectTo(server.port(), env(), &error);
    QVERIFY2(session, qPrintable(error));
    QCOMPARE(ssh_userauth_password(session.get(), nullptr, "secret"), SSH_AUTH_SUCCESS);

    SftpPtr sftp(sftp_new(session.get()));
    QVERIFY(sftp);
    QVERIFY2(sftp_init(sftp.get()) == SSH_OK, ssh_get_error(session.get()));
    QTRY_COMPARE_WITH_TIMEOUT(m_counters.sftp, 1, kTimeoutMs);
    QCOMPARE(sftp_server_version(sftp.get()), 3);

    const QString rootDir = o.rootDir;
    const QByteArray remotePath = QDir(rootDir).filePath(QStringLiteral("big.bin")).toUtf8();
    const QByteArray payload = randomBytes(300 * 1024);

    // Upload with sftp_open/sftp_write.
    {
        sftp_file file = sftp_open(sftp.get(), remotePath.constData(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        QVERIFY2(file, ssh_get_error(session.get()));
        qsizetype done = 0;
        while (done < payload.size()) {
            const ssize_t n = sftp_write(file, payload.constData() + done, static_cast<size_t>(payload.size() - done));
            QVERIFY2(n > 0, ssh_get_error(session.get()));
            done += static_cast<qsizetype>(n);
        }
        QCOMPARE(sftp_close(file), SSH_OK);
    }
    {
        QFile local(QString::fromUtf8(remotePath));
        QVERIFY(local.open(QIODevice::ReadOnly));
        QCOMPARE(local.readAll(), payload);
    }

    // Stat, read back, canonicalize.
    {
        sftp_attributes attrs = sftp_stat(sftp.get(), remotePath.constData());
        QVERIFY2(attrs, ssh_get_error(session.get()));
        QCOMPARE(static_cast<qint64>(attrs->size), static_cast<qint64>(payload.size()));
        QCOMPARE(static_cast<int>(attrs->type), SSH_FILEXFER_TYPE_REGULAR);
        sftp_attributes_free(attrs);
    }
    {
        sftp_file file = sftp_open(sftp.get(), remotePath.constData(), O_RDONLY, 0);
        QVERIFY2(file, ssh_get_error(session.get()));
        QByteArray back;
        char buf[16384];
        for (;;) {
            const ssize_t n = sftp_read(file, buf, sizeof(buf));
            QVERIFY2(n >= 0, ssh_get_error(session.get()));
            if (n == 0) {
                break;
            }
            back.append(buf, static_cast<qsizetype>(n));
        }
        QCOMPARE(sftp_close(file), SSH_OK);
        QCOMPARE(back.size(), payload.size());
        QCOMPARE(back, payload);
    }
    {
        char* canonical = sftp_canonicalize_path(sftp.get(), ".");
        QVERIFY2(canonical, ssh_get_error(session.get()));
        const QString home = QString::fromUtf8(canonical);
        ssh_string_free_char(canonical);
        QCOMPARE(home, QFileInfo(rootDir).canonicalFilePath());
        QVERIFY(!home.isEmpty());
    }
    {
        // A relative name lands under the root; directories work too.
        sftp_attributes attrs = sftp_stat(sftp.get(), "big.bin");
        QVERIFY(attrs);
        QCOMPARE(static_cast<qint64>(attrs->size), static_cast<qint64>(payload.size()));
        sftp_attributes_free(attrs);
        QCOMPARE(sftp_mkdir(sftp.get(), "sub", 0755), SSH_OK);
        QVERIFY(QFileInfo(QDir(rootDir).filePath(QStringLiteral("sub"))).isDir());
        attrs = sftp_stat(sftp.get(), "sub");
        QVERIFY(attrs);
        QCOMPARE(static_cast<int>(attrs->type), SSH_FILEXFER_TYPE_DIRECTORY);
        sftp_attributes_free(attrs);
        QCOMPARE(sftp_rename(sftp.get(), "big.bin", "sub/moved.bin"), SSH_OK);
        QVERIFY(QFileInfo::exists(QDir(rootDir).filePath(QStringLiteral("sub/moved.bin"))));

        sftp_dir dir = sftp_opendir(sftp.get(), "sub");
        QVERIFY(dir);
        QStringList names;
        while (sftp_attributes entry = sftp_readdir(sftp.get(), dir)) {
            names.append(QString::fromUtf8(entry->name));
            sftp_attributes_free(entry);
        }
        QVERIFY(sftp_dir_eof(dir));
        QCOMPARE(sftp_closedir(dir), SSH_OK);
        QVERIFY(names.contains(QStringLiteral("moved.bin")));
        QVERIFY(names.contains(QStringLiteral(".")));

        QCOMPARE(sftp_unlink(sftp.get(), "sub/moved.bin"), SSH_OK);
        QVERIFY(!QFileInfo::exists(QDir(rootDir).filePath(QStringLiteral("sub/moved.bin"))));
        QCOMPARE(sftp_rmdir(sftp.get(), "sub"), SSH_OK);
    }
    {
        QVERIFY(sftp_stat(sftp.get(), "does-not-exist") == nullptr);
        QCOMPARE(sftp_get_error(sftp.get()), SSH_FX_NO_SUCH_FILE);
        QVERIFY(sftp_open(sftp.get(), "does-not-exist", O_RDONLY, 0) == nullptr);
        QCOMPARE(sftp_get_error(sftp.get()), SSH_FX_NO_SUCH_FILE);
    }

    // The shell keeps working on the same session while SFTP is open.
    ChannelPtr shell = openShell(session.get());
    QVERIFY(shell);
    QVERIFY(readUntil(shell.get(), "$ ").endsWith("$ "));
    QVERIFY(writeAll(shell.get(), "echo both\r"));
    QCOMPARE(readUntil(shell.get(), "both\r\n$ "), QByteArray("echo both\r\nboth\r\n$ "));
    QCOMPARE(m_counters.sftp, 1);
}

void Tst_testsshserver::sftpHandlerUnit()
{
    const QString root = m_tmp->filePath(QStringLiteral("root"));
    TestSftpHandler handler(root);
    QCOMPARE(handler.resolvePath(QStringLiteral(".")), QDir::cleanPath(root));
    QCOMPARE(handler.resolvePath(QStringLiteral("a/b.txt")), QDir::cleanPath(root + QStringLiteral("/a/b.txt")));
    QCOMPARE(handler.resolvePath(QStringLiteral("~/c")), QDir::cleanPath(root + QStringLiteral("/c")));
    QCOMPARE(handler.resolvePath(root + QStringLiteral("/x")), QDir::cleanPath(root + QStringLiteral("/x")));

    // INIT arrives in two fragments and is answered with VERSION 3.
    QByteArray out;
    handler.feed(QByteArray::fromHex("00000005"), &out);
    QVERIFY(out.isEmpty());
    handler.feed(QByteArray::fromHex("0100000003"), &out);
    QCOMPARE(out, QByteArray::fromHex("000000050200000003"));
    QVERIFY(handler.initialised());

    // REALPATH "." -> NAME with one entry naming the root.
    out.clear();
    const QByteArray realpath = QByteArray::fromHex("0000000a") + "\x10" + QByteArray::fromHex("00000007")
                                + QByteArray::fromHex("00000001") + ".";
    handler.feed(realpath, &out);
    QVERIFY(out.size() > 13);
    QCOMPARE(static_cast<quint8>(out.at(4)), static_cast<quint8>(104));   // SSH_FXP_NAME
    QVERIFY(out.contains(QFileInfo(root).canonicalFilePath().toUtf8()));

    // An unknown request type gets SSH_FX_OP_UNSUPPORTED (8) with the request id echoed.
    out.clear();
    handler.feed(QByteArray::fromHex("00000005") + "\x63" + QByteArray::fromHex("00000042"), &out);
    QCOMPARE(out.mid(4, 1), QByteArray("\x65"));   // SSH_FXP_STATUS
    QCOMPARE(out.mid(5, 4), QByteArray::fromHex("00000042"));
    QCOMPARE(out.mid(9, 4), QByteArray::fromHex("00000008"));
    QCOMPARE(handler.handledRequests(), 3);
    QCOMPARE(handler.openHandleCount(), 0);
}

void Tst_testsshserver::directTcpip()
{
    EchoServer echo;
    echo.start();
    const quint16 echoPort = echo.port();
    QVERIFY(echoPort != 0);

    TestSshServer server(options());
    attach(&server);
    QVERIFY(server.start());
    QString error;
    SessionPtr session = connectTo(server.port(), env(), &error);
    QVERIFY2(session, qPrintable(error));
    QCOMPARE(ssh_userauth_password(session.get(), nullptr, "secret"), SSH_AUTH_SUCCESS);

    ChannelPtr channel(ssh_channel_new(session.get()));
    QVERIFY(channel);
    QCOMPARE(ssh_channel_open_forward(channel.get(), "127.0.0.1", echoPort, "127.0.0.1", 40000), SSH_OK);
    QTRY_COMPARE_WITH_TIMEOUT(m_counters.forwards, 1, kTimeoutMs);
    QCOMPARE(m_counters.forwardTargets.first().first, QStringLiteral("127.0.0.1"));
    QCOMPARE(m_counters.forwardTargets.first().second, echoPort);

    QVERIFY(writeAll(channel.get(), "ping"));
    QCOMPARE(readUntil(channel.get(), "ping"), QByteArray("ping"));

    const QByteArray blob = randomBytes(64 * 1024);
    QVERIFY(writeAll(channel.get(), blob));
    QByteArray back;
    QDeadlineTimer deadline(kTimeoutMs);
    while (back.size() < blob.size() && !deadline.hasExpired()) {
        char buf[8192];
        const int n = ssh_channel_read_timeout(channel.get(), buf, sizeof(buf), 0, 100);
        QVERIFY(n != SSH_ERROR);
        if (n > 0) {
            back.append(buf, n);
        }
    }
    QCOMPARE(back.size(), blob.size());
    QCOMPARE(back, blob);

    // EOF from the client reaches the target; closing the channel closes the socket.
    QCOMPARE(ssh_channel_send_eof(channel.get()), SSH_OK);
    QCOMPARE(ssh_channel_close(channel.get()), SSH_OK);
    channel.reset();

    // Only loopback targets, and only ports that answer.
    ChannelPtr refused(ssh_channel_new(session.get()));
    QVERIFY(refused);
    QVERIFY(ssh_channel_open_forward(refused.get(), "10.1.2.3", 80, "127.0.0.1", 40001) != SSH_OK);
    QTRY_COMPARE_WITH_TIMEOUT(m_counters.forwards, 2, kTimeoutMs);
    QCOMPARE(m_counters.forwardTargets.last().first, QStringLiteral("10.1.2.3"));
    QCOMPARE(m_counters.forwardTargets.last().second, static_cast<quint16>(80));

    QTcpServer closedPortProbe;
    QVERIFY(closedPortProbe.listen(QHostAddress::LocalHost));
    const quint16 closedPort = closedPortProbe.serverPort();
    closedPortProbe.close();
    ChannelPtr dead(ssh_channel_new(session.get()));
    QVERIFY(dead);
    QVERIFY(ssh_channel_open_forward(dead.get(), "127.0.0.1", closedPort, "127.0.0.1", 40002) != SSH_OK);
    QTRY_COMPARE_WITH_TIMEOUT(m_counters.forwards, 3, kTimeoutMs);

    // The session is still healthy.
    ChannelPtr shell = openShell(session.get());
    QVERIFY(shell);
    QVERIFY(readUntil(shell.get(), "$ ").contains("welcome"));
}

void Tst_testsshserver::regenerateHostKey()
{
    TestSshServer server(options());
    const QString fp1 = server.hostKeyFingerprintSha256();
    const QString line1 = server.hostKeyPublicLine();
    QVERIFY(server.start());
    server.regenerateHostKey();   // ignored while running
    QCOMPARE(server.hostKeyFingerprintSha256(), fp1);
    server.stop();

    server.regenerateHostKey();
    const QString fp2 = server.hostKeyFingerprintSha256();
    QVERIFY(fp2.startsWith(QStringLiteral("SHA256:")));
    QVERIFY(fp2 != fp1);
    QVERIFY(server.hostKeyPublicLine() != line1);
    QCOMPARE(server.hostKeyType(), QStringLiteral("ssh-ed25519"));

    QVERIFY(server.start());
    QString error;
    SessionPtr session = connectTo(server.port(), env(), &error);
    QVERIFY2(session, qPrintable(error));
    ssh_key raw = nullptr;
    QCOMPARE(ssh_get_server_publickey(session.get(), &raw), SSH_OK);
    KeyPtr key(raw);
    QCOMPARE(fingerprintOf(key.get()), fp2);
    QVERIFY(fingerprintOf(key.get()) != fp1);
}

void Tst_testsshserver::threeConcurrentClients()
{
    TestSshServer server(options());
    attach(&server);
    QVERIFY(server.start());
    QString error;
    SessionPtr sessions[3];
    ChannelPtr channels[3];
    for (int i = 0; i < 3; ++i) {
        sessions[i] = connectTo(server.port(), env(), &error);
        QVERIFY2(sessions[i], qPrintable(error));
        QCOMPARE(ssh_userauth_password(sessions[i].get(), nullptr, "secret"), SSH_AUTH_SUCCESS);
        channels[i] = openShell(sessions[i].get(), "xterm", 80 + i, 24);
        QVERIFY(channels[i]);
        QVERIFY(readUntil(channels[i].get(), "$ ").contains("welcome"));
    }
    QTRY_COMPARE_WITH_TIMEOUT(server.activeConnections(), 3, kTimeoutMs);
    QCOMPARE(server.connectionCount(), 3);
    QTRY_COMPARE_WITH_TIMEOUT(m_counters.shell, 3, kTimeoutMs);
    QTRY_COMPARE_WITH_TIMEOUT(m_counters.authMethods.size(), 3, kTimeoutMs);

    for (int i = 0; i < 3; ++i) {
        const QByteArray tag = "client" + QByteArray::number(i);
        QVERIFY(writeAll(channels[i].get(), "echo " + tag + "\r"));
    }
    for (int i = 0; i < 3; ++i) {
        const QByteArray tag = "client" + QByteArray::number(i);
        QCOMPARE(readUntil(channels[i].get(), tag + "\r\n$ "), "echo " + tag + "\r\n" + tag + "\r\n$ ");
        QVERIFY(writeAll(channels[i].get(), "size\r"));
        QCOMPARE(readUntil(channels[i].get(), "$ "), "size\r\nCOLS=" + QByteArray::number(80 + i) + " ROWS=24\r\n$ ");
    }

    for (int i = 0; i < 3; ++i) {
        channels[i].reset();
        sessions[i].reset();
    }
    QTRY_COMPARE_WITH_TIMEOUT(server.activeConnections(), 0, kTimeoutMs);
    QTRY_COMPARE_WITH_TIMEOUT(m_counters.disconnected, 3, kTimeoutMs);
    QCOMPARE(m_counters.connected, 3);
}

void Tst_testsshserver::stopWhileConnected()
{
    TestSshServer server(options());
    attach(&server);
    QVERIFY(server.start());
    QString error;
    SessionPtr session = connectTo(server.port(), env(), &error);
    QVERIFY2(session, qPrintable(error));
    QCOMPARE(ssh_userauth_password(session.get(), nullptr, "secret"), SSH_AUTH_SUCCESS);
    ChannelPtr channel = openShell(session.get());
    QVERIFY(channel);
    QVERIFY(readUntil(channel.get(), "$ ").contains("welcome"));
    QTRY_COMPARE_WITH_TIMEOUT(server.activeConnections(), 1, kTimeoutMs);

    QElapsedTimer timer;
    timer.start();
    server.stop();
    QVERIFY2(timer.elapsed() < 2000, qPrintable(QString::number(timer.elapsed())));
    QVERIFY(!server.isRunning());
    QCOMPARE(server.activeConnections(), 0);

    char buf[64];
    const int rc = ssh_channel_read_timeout(channel.get(), buf, sizeof(buf), 0, 3000);
    QVERIFY2(rc == SSH_ERROR || rc == 0, "client read did not fail after stop()");
    QTRY_COMPARE_WITH_TIMEOUT(m_counters.disconnected, 1, kTimeoutMs);

    // Nobody listens any more (a short timeout: libssh on Windows only notices a refused
    // TCP connect when its connect timeout expires).
    QVERIFY(!connectTo(server.port(), env(), &error, QStringLiteral("test"), 1));
}

void Tst_testsshserver::keepAliveDropAfter()
{
    TestSshServer::Options o = options();
    o.keepAliveDropAfter = 1;
    TestSshServer server(o);
    QVERIFY(server.start());
    QString error;
    SessionPtr session = connectTo(server.port(), env(), &error);
    QVERIFY2(session, qPrintable(error));
    QCOMPARE(ssh_userauth_password(session.get(), nullptr, "secret"), SSH_AUTH_SUCCESS);
    ChannelPtr channel = openShell(session.get());
    QVERIFY(channel);
    QVERIFY(readUntil(channel.get(), "$ ").contains("welcome"));
    QVERIFY(writeAll(channel.get(), "echo alive\r"));
    QCOMPARE(readUntil(channel.get(), "alive\r\n$ "), QByteArray("echo alive\r\nalive\r\n$ "));

    // After a second without any client traffic the session is no longer serviced: the
    // TCP connection stays up but nothing is answered any more.
    QTest::qWait(1600);
    QVERIFY(ssh_is_connected(session.get()));
    QVERIFY(writeAll(channel.get(), "echo late\r"));
    char buf[256];
    QCOMPARE(ssh_channel_read_timeout(channel.get(), buf, sizeof(buf), 0, 1000), SSH_AGAIN);
    QVERIFY(ssh_is_connected(session.get()));
    QCOMPARE(server.activeConnections(), 1);

    QElapsedTimer timer;
    timer.start();
    server.stop();
    QVERIFY(timer.elapsed() < 2000);
}

QTEST_GUILESS_MAIN(Tst_testsshserver)
#include "tst_testsshserver.moc"
