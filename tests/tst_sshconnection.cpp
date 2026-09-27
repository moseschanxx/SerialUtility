#include <QtTest>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTcpServer>
#include <QTcpSocket>
#include <QHostAddress>
#include <QPointer>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QThread>

#include <memory>
#include <utility>

#include "ssh/SecretStore.h"
#include "ssh/SshConnection.h"
#include "ssh/SshProfile.h"
#include "support/TestSshServer.h"

/*
 * SshConnection:
 *  (a) everything that needs no server: statics, argument validation, state rules, the worker's
 *      connect-failure path against a closed local port, generation handling after close();
 *  (b) a live probe against a real OpenSSH server, run only when SU_SSH_PROBE_TARGET is set:
 *        SU_SSH_PROBE_TARGET    user@host[:port]
 *        SU_SSH_PROBE_PASSWORD  password for that user (optional; without it a key or agent is used)
 *        SU_SSH_PROBE_IDENTITY  private key file for the key-login probe (optional)
 *      It uses a temporary known_hosts, never the user's ~/.ssh.
 *  (c) end-to-end tests against the in-process TestSshServer (tests/support): every auth
 *      method, host-key handling with a temporary known_hosts, banner, terminal size, startup
 *      and remote commands, bursts, keep-alive / drop / reconnect (also when a slot closes the
 *      connection from within connectionLost()), local forwards (bind addresses, the local or
 *      the remote side closing first while the session keeps running), SFTP, the remote home,
 *      close()/destroy at every stage and parallel connections. Profiles never use Auth::Auto
 *      so the user's real ~/.ssh/id_* keys are never read, and SU_SSH_IGNORE_CONFIG keeps the
 *      worker from reading ~/.ssh/config, so a developer's Host blocks cannot change a result.
 */

namespace {

struct ProbeEnv
{
    bool enabled = false;
    SshProfile profile;
    QString password;
    QString identity;
};

ProbeEnv probeEnv()
{
    ProbeEnv env;
    const QString target = qEnvironmentVariable("SU_SSH_PROBE_TARGET").trimmed();
    if (target.isEmpty() || !SshProfile::parseTarget(target, env.profile)) {
        return env;
    }
    env.password = qEnvironmentVariable("SU_SSH_PROBE_PASSWORD");
    env.identity = qEnvironmentVariable("SU_SSH_PROBE_IDENTITY").trimmed();
    env.enabled = true;
    return env;
}

const char kSkipMessage[] = "set SU_SSH_PROBE_TARGET=user@host[:port] (and SU_SSH_PROBE_PASSWORD / SU_SSH_PROBE_IDENTITY) to run the live probe";

/// One connection wired for the probe: host keys answered with acceptKey/rememberKey, prompts
/// with `responses` (falling back to the password; prompt number `cancelAt` is cancelled), every
/// signal of interest recorded.
struct ProbeSession
{
    ProbeSession(const ProbeEnv& env, const QString& knownHosts)
        : password(env.password)
    {
        profile = env.profile;
        profile.knownHostsFile = knownHosts;
        profile.connectTimeoutSeconds = 20;
        profile.keepAliveSeconds = 2;
        if (!env.password.isEmpty()) {
            profile.auth = SshProfile::Auth::Password;
        } else if (!env.identity.isEmpty()) {
            profile.auth = SshProfile::Auth::PublicKey;
            profile.identityFile = env.identity;
        } else {
            profile.auth = SshProfile::Auth::Auto;
        }
        QObject::connect(&conn, &Transport::dataReceived, &conn, [this](const QByteArray& data) { received += data; });
        QObject::connect(&conn, &SshConnection::hostKeyVerificationRequired, &conn,
                         [this](const SshConnection::HostKeyInfo& info) {
                             hostKeys.append(info);
                             conn.answerHostKey(acceptKey, rememberKey);
                         });
        QObject::connect(&conn, &SshConnection::authPromptRequired, &conn, [this](const SshConnection::AuthPrompt& prompt) {
            prompts.append(prompt);
            const int number = prompts.size();
            if (cancelAt == number) {
                conn.cancelPrompt();
                return;
            }
            conn.answerPrompt(number <= responses.size() ? responses.at(number - 1) : password, rememberResponse);
        });
        QObject::connect(&conn, &Transport::errorOccurred, &conn, [this](const QString& message) { errors.append(message); });
        QObject::connect(&conn, &Transport::stateChanged, &conn, [this](Transport::State state) { states.append(state); });
        QObject::connect(&conn, &Transport::connectionLost, &conn, [this](const QString&) { ++lost; });
        QObject::connect(&conn, &SshConnection::channelClosed, &conn, [this](int status) { closed.append(status); });
        QObject::connect(&conn, &SshConnection::authenticated, &conn, [this](const QString& method) { methods.append(method); });
    }

    void apply()
    {
        conn.setProfile(profile);
    }

    bool rememberResponse = false;   ///< answer every prompt with "remember"

    // The recorders come first so they outlive `conn` (members are destroyed in reverse order):
    // a signal delivered while the connection is torn down must never append to freed storage.
    SshProfile profile;
    QString password;
    QByteArray received;
    QList<SshConnection::HostKeyInfo> hostKeys;
    QList<SshConnection::AuthPrompt> prompts;
    QStringList errors;
    QList<Transport::State> states;
    QStringList methods;
    QList<int> closed;
    int lost = 0;
    bool acceptKey = true;
    bool rememberKey = true;
    QStringList responses;
    int cancelAt = 0;
    SshConnection conn;   ///< last: destroyed first
};

QString knownHostsToken(const SshProfile& profile)
{
    const QString lower = profile.host.trimmed().toLower();
    return profile.port == 22 ? lower : QStringLiteral("[%1]:%2").arg(lower).arg(profile.port);
}

/// Lines of `path` whose host token and key type match; `base64` receives the key of the first one.
int countKnownHostsEntries(const QString& path, const QString& token, const QString& keyType, QString* base64)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return 0;
    }
    int count = 0;
    const QList<QByteArray> lines = file.readAll().split('\n');
    for (const QByteArray& raw : lines) {
        const QList<QByteArray> fields = raw.simplified().split(' ');
        if (fields.size() >= 3 && fields.at(0) == token.toUtf8() && fields.at(1) == keyType.toUtf8()) {
            if (count == 0 && base64) {
                *base64 = QString::fromLatin1(fields.at(2));
            }
            ++count;
        }
    }
    return count;
}

/// Replace the stored key of the first matching entry with a different key of the same type
/// (last byte of the blob flipped), simulating a reinstalled server.
bool tamperKnownHosts(const QString& path, const QString& token, const QString& keyType)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    QList<QByteArray> lines = file.readAll().split('\n');
    file.close();
    bool done = false;
    for (QByteArray& line : lines) {
        QList<QByteArray> fields = line.simplified().split(' ');
        if (fields.size() < 3 || fields.at(0) != token.toUtf8() || fields.at(1) != keyType.toUtf8()) {
            continue;
        }
        QByteArray blob = QByteArray::fromBase64(fields.at(2));
        if (blob.isEmpty()) {
            return false;
        }
        blob[blob.size() - 1] = static_cast<char>(blob.at(blob.size() - 1) ^ 0x55);
        fields[2] = blob.toBase64();
        line = fields.join(' ');
        done = true;
        break;
    }
    if (!done || !file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    file.write(lines.join('\n'));
    return true;
}

QByteArray randomBytes(qsizetype size)
{
    QByteArray bytes(size, Qt::Uninitialized);
    QRandomGenerator::global()->fillRange(reinterpret_cast<quint32*>(bytes.data()), static_cast<qsizetype>(size / 4));
    return bytes;
}

QByteArray readAll(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QByteArray();
    }
    return file.readAll();
}

// ---- (c) end to end against the in-process TestSshServer --------------------------------

constexpr int kE2eTimeoutMs = 15000;
const QString kServerUser = QStringLiteral("test");
const QString kServerPassword = QStringLiteral("secret");

using State = Transport::State;

/// One SshConnection wired to the in-process server: records every signal of interest and
/// answers the questions automatically - host keys with acceptKey / rememberKey, prompts with
/// `responses` in order (then `password`; prompt number `cancelAt` is cancelled instead).
/// With autoAnswer == false the questions stay open (close-while-asking tests).
struct TestClient
{
    explicit TestClient(const SshProfile& p)
        : profile(p)
    {
        QObject::connect(&conn, &Transport::dataReceived, &conn, [this](const QByteArray& data) { received += data; });
        QObject::connect(&conn, &SshConnection::hostKeyVerificationRequired, &conn,
                         [this](const SshConnection::HostKeyInfo& info) {
                             hostKeys.append(info);
                             if (autoAnswer) {
                                 conn.answerHostKey(acceptKey, rememberKey);
                             }
                         });
        QObject::connect(&conn, &SshConnection::authPromptRequired, &conn, [this](const SshConnection::AuthPrompt& prompt) {
            prompts.append(prompt);
            if (!autoAnswer) {
                return;
            }
            const int number = prompts.size();
            if (cancelAt == number) {
                conn.cancelPrompt();
                return;
            }
            conn.answerPrompt(number <= responses.size() ? responses.at(number - 1) : password, rememberResponse);
        });
        QObject::connect(&conn, &Transport::errorOccurred, &conn, [this](const QString& message) { errors.append(message); });
        QObject::connect(&conn, &Transport::stateChanged, &conn, [this](Transport::State state) { states.append(state); });
        QObject::connect(&conn, &Transport::connectionLost, &conn, [this](const QString&) { ++lost; });
        QObject::connect(&conn, &Transport::connectionRestored, &conn, [this](const QString&) { ++restored; });
        QObject::connect(&conn, &SshConnection::channelClosed, &conn, [this](int status) { closed.append(status); });
        QObject::connect(&conn, &SshConnection::authenticated, &conn, [this](const QString& method) { methods.append(method); });
        QObject::connect(&conn, &SshConnection::bannerReceived, &conn, [this](const QString& text) { banners.append(text); });
        QObject::connect(&conn, &SshConnection::shellStarted, &conn, [this] { ++shells; });
        conn.setProfile(profile);
    }

    // The recorders come first so they outlive `conn` (members are destroyed in reverse order):
    // the lambdas above write into them, and a connection destroyed while Connected or
    // Reconnecting tears its worker down from the destructor (e2eDestroyWhileConnected). The
    // destructor itself emits nothing since it blocks its signals, but nothing here may rely on
    // that ordering either.
    SshProfile profile;
    QString password = kServerPassword;
    QByteArray received;
    QList<SshConnection::HostKeyInfo> hostKeys;
    QList<SshConnection::AuthPrompt> prompts;
    QStringList errors;
    QList<Transport::State> states;
    QStringList methods;
    QStringList banners;
    QList<int> closed;
    int lost = 0;
    int restored = 0;
    int shells = 0;
    bool autoAnswer = true;
    bool acceptKey = true;
    bool rememberKey = true;
    QStringList responses;
    bool rememberResponse = false;
    int cancelAt = 0;
    SshConnection conn;   ///< last: destroyed first, while every recorder above is still alive
};

/// Non-empty lines of a known_hosts file, trimmed.
QStringList knownHostsLines(const QString& path)
{
    QStringList lines;
    const QList<QByteArray> raw = readAll(path).split('\n');
    for (const QByteArray& line : raw) {
        const QString text = QString::fromUtf8(line).trimmed();
        if (!text.isEmpty()) {
            lines.append(text);
        }
    }
    return lines;
}

bool writeFile(const QString& path, const QByteArray& content)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    return file.write(content) == content.size();
}

/// 'a'..'z' only: nothing the scripted shell treats specially (no CR, LF, BS, DEL).
QByteArray letterBurst(qsizetype size)
{
    QByteArray bytes(size, Qt::Uninitialized);
    for (qsizetype i = 0; i < size; ++i) {
        bytes[i] = static_cast<char>('a' + (i % 26));
    }
    return bytes;
}

/// "line 1\r\n" .. "line n\r\n" exactly as the scripted shell prints them for "big n".
QByteArray bigOutput(int n)
{
    QByteArray block;
    block.reserve(n * 12);
    for (int k = 1; k <= n; ++k) {
        block += "line " + QByteArray::number(k) + "\r\n";
    }
    return block;
}

/// A TCP echo server on the test thread (serviced whenever the test waits in QTRY_*).
class LocalEchoServer : public QTcpServer
{
public:
    LocalEchoServer()
    {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket* socket = nextPendingConnection()) {
                ++accepted;
                clients.append(socket);
                connect(socket, &QTcpSocket::readyRead, socket, [socket] { socket->write(socket->readAll()); });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }

    /// The echo server ends every connection from its side (the "remote closes first" case).
    void closeClients()
    {
        for (const QPointer<QTcpSocket>& socket : std::as_const(clients)) {
            if (socket) {
                socket->disconnectFromHost();
            }
        }
        clients.clear();
    }

    int accepted = 0;
    QList<QPointer<QTcpSocket>> clients;
};

/// The server's signals come from its worker threads. A QSignalSpy on them records the
/// emissions on those threads (Qt::DirectConnection) while the test thread reads the list - a
/// data race. This object lives in the test thread, so the connections below are queued and the
/// counters are only ever touched here; poll them with QTRY_*.
class ServerSpy : public QObject
{
public:
    explicit ServerSpy(TestSshServer* server)
    {
        connect(server, &TestSshServer::clientConnected, this, [this] { ++connected; });
        connect(server, &TestSshServer::clientAuthenticated, this,
                [this](const QString& method) { authMethods.append(method); });
        connect(server, &TestSshServer::clientDisconnected, this, [this] { ++disconnected; });
        connect(server, &TestSshServer::shellRequested, this, [this] { ++shells; });
        connect(server, &TestSshServer::execRequested, this, [this](const QString& command) { execCommands.append(command); });
        connect(server, &TestSshServer::sftpRequested, this, [this] { ++sftp; });
        connect(server, &TestSshServer::forwardRequested, this,
                [this](const QString& host, quint16 port) { forwards.append({host, port}); });
    }

    int connected = 0;
    int disconnected = 0;
    int shells = 0;
    int sftp = 0;
    QStringList authMethods;
    QStringList execCommands;
    QList<QPair<QString, quint16>> forwards;
};

} // namespace

class Tst_sshconnection : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase();

    // ---- (a) no server needed ---------------------------------------------------------
    void statics();
    void openWithoutHostFails();
    void openWithZeroPortFails();
    void writeWhileDisconnected();
    void terminalSize();
    void settingsMapContents();
    void summaryAndDisplayName();
    void closeWhileDisconnectedIsNoOp();
    void transferAndHomeWhileDisconnected();
    void answersWithoutQuestionAreIgnored();
    void destroyNeverOpened();
    void connectionRefusedReportsError();
    void setProfileIgnoredWhileConnecting();
    void closeWhileConnectingDropsLateSignals();
    void destroyWhileConnecting();

    // ---- (c) end to end against the in-process TestSshServer --------------------------
    void e2ePasswordShellExit();
    void e2ePublicKey();
    void e2ePublicKeyPassphrase();
    void e2eKeyboardInteractive();
    void e2eWrongPasswordThenCancel();
    void e2eHostKeyUnknownRememberThenSilent();
    void e2eHostKeyRejected();
    void e2eHostKeyChangedReplaced();
    void e2eSavedPassword();
    void e2eBanner();
    void e2eTerminalSize();
    void e2eStartupCommand();
    void e2eRemoteCommandExec();
    void e2eBurstAndBigOutput();
    void e2eDropReconnect();
    void e2eDropNoReconnect();
    void e2eCloseFromConnectionLostSlot();
    void e2eLocalForward();
    void e2eLocalForwardClientClosesFirst();
    void e2eSftpTransfers();
    void e2eSftpPipelineSizes();
    void e2eRemoteHome();
    void e2eCloseDuringHostKeyQuestion();
    void e2eCloseDuringAuthPrompt();
    void e2eCloseWhileConnecting();
    void e2eDestroyWhileConnected();
    void e2eTwoConnectionsInParallel();
    // v0.4: remembered credentials for ad-hoc targets / key files, transfers without SFTP
    void e2eAdHocRememberPassword();
    void e2ePassphraseRememberedPerKey();
    void e2eShellFallbackTransfers();
    void e2eShellFallbackRemoteHome();
    void e2eShellFallbackFailedUploadRemoved();
    void e2eShellFallbackUploadWithoutExitStatus();
    void e2eCloseDuringUploadRemovesRemote();

    // ---- (b) live probe against a real sshd -------------------------------------------
    void probeShellResizeAndSftp();
    void probeWrongPasswordThenCancel();
    void probeChangedHostKey();
    void probeKeyLogin();
    void probeRemoteExit();
    void probeAdHocRemember();

private:
    quint16 closedLocalPort();
    SshProfile refusedProfile();
    TestSshServer::Options serverOptions(const QString& name);   ///< SFTP root under m_dir/<name>/root
    SshProfile serverProfile(const TestSshServer& server, const QString& name);   ///< password auth, known_hosts under m_dir/<name>
    void openAndWaitForPrompt(TestClient& c);                    ///< open() -> Connected -> the first shell prompt
    QTemporaryDir m_dir;
};

void Tst_sshconnection::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("BuildAI-Test"));
    QCoreApplication::setApplicationName(QStringLiteral("SerialUtilityTest-sshconnection"));
    QVERIFY(m_dir.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_dir.filePath(QStringLiteral("settings")));
    QSettings settings;
    settings.clear();
    QVERIFY(settings.fileName().startsWith(m_dir.path()));
    // The worker parses ~/.ssh/config and the global ssh_config before the profile fields; a
    // "Host *" block on a developer's box (ProxyJump, Port, IdentitiesOnly...) would change or
    // break every end-to-end test below without a hint. Read by SshWorker::applySessionOptions().
    qputenv("SU_SSH_IGNORE_CONFIG", "1");
}

quint16 Tst_sshconnection::closedLocalPort()
{
    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost, 0)) {
        return 0;
    }
    const quint16 port = server.serverPort();
    server.close();
    return port;
}

SshProfile Tst_sshconnection::refusedProfile()
{
    SshProfile p;
    p.host = QStringLiteral("127.0.0.1");
    p.port = closedLocalPort();
    p.user = QStringLiteral("nobody");
    p.connectTimeoutSeconds = 5;
    p.knownHostsFile = m_dir.filePath(QStringLiteral("known_hosts_refused"));
    return p;
}

// ---------------------------------------------------------------------------------------
// (a) no server
// ---------------------------------------------------------------------------------------

void Tst_sshconnection::statics()
{
    QVERIFY(SshConnection::defaultKnownHostsFile().endsWith(QStringLiteral(".ssh/known_hosts")));
    QVERIFY(SshConnection::defaultKnownHostsFile().startsWith(QDir::homePath()));
    QVERIFY(!SshConnection::localUserName().isEmpty());
    QVERIFY2(SshConnection::libraryVersion().startsWith(QStringLiteral("libssh 0.11")),
             qPrintable(SshConnection::libraryVersion()));
    const QStringList identities = SshConnection::defaultIdentityFiles();
    for (const QString& file : identities) {
        QVERIFY(QFileInfo(file).isFile());
        QVERIFY(file.contains(QStringLiteral("/.ssh/id_")));
    }
#if defined(Q_OS_WIN)
    QVERIFY(!SshConnection::agentAvailable());
#else
    QCOMPARE(SshConnection::agentAvailable(), !qEnvironmentVariableIsEmpty("SSH_AUTH_SOCK"));
#endif
}

void Tst_sshconnection::openWithoutHostFails()
{
    SshConnection c;
    QSignalSpy errors(&c, &Transport::errorOccurred);
    QSignalSpy states(&c, &Transport::stateChanged);
    QCOMPARE(c.kind(), Transport::Kind::Ssh);
    QCOMPARE(c.state(), Transport::State::Disconnected);
    QVERIFY(!c.open());
    QCOMPARE(errors.count(), 1);
    QVERIFY(!c.errorString().isEmpty());
    QCOMPARE(c.state(), Transport::State::Disconnected);
    QCOMPARE(states.count(), 0);

    SshProfile p;
    p.host = QStringLiteral("   ");
    c.setProfile(p);
    QVERIFY(!c.open());
    QCOMPARE(errors.count(), 2);
    QCOMPARE(c.state(), Transport::State::Disconnected);
}

void Tst_sshconnection::openWithZeroPortFails()
{
    SshConnection c;
    SshProfile p;
    p.host = QStringLiteral("board");
    p.port = 0;
    c.setProfile(p);
    QSignalSpy errors(&c, &Transport::errorOccurred);
    QVERIFY(!c.open());
    QCOMPARE(errors.count(), 1);
    QCOMPARE(c.state(), Transport::State::Disconnected);
}

void Tst_sshconnection::writeWhileDisconnected()
{
    SshConnection c;
    QSignalSpy sent(&c, &Transport::dataSent);
    QCOMPARE(c.write(QByteArrayLiteral("ls\r")), qint64(-1));
    QCOMPARE(c.write(QByteArray()), qint64(-1));
    QCOMPARE(sent.count(), 0);
    QCOMPARE(c.bytesSent(), quint64(0));
    QCOMPARE(c.pendingTxBytes(), qint64(0));
    QVERIFY(!c.isOpen());
}

void Tst_sshconnection::terminalSize()
{
    SshConnection c;
    QCOMPARE(c.lastTerminalSize(), QStringLiteral("80 x 24"));
    c.notifyTerminalSize(132, 40);
    QCOMPARE(c.lastTerminalSize(), QStringLiteral("132 x 40"));
    c.notifyTerminalSize(0, 10);
    c.notifyTerminalSize(10, -1);
    QCOMPARE(c.lastTerminalSize(), QStringLiteral("132 x 40"));
}

void Tst_sshconnection::settingsMapContents()
{
    SshConnection c;
    QVariantMap map = c.settingsMap();
    QCOMPARE(map.value(QStringLiteral("kind")).toString(), QStringLiteral("ssh"));
    QVERIFY(map.contains(QStringLiteral("profileId")));
    QVERIFY(map.contains(QStringLiteral("target")));
    QVERIFY(map.value(QStringLiteral("profileId")).toString().isEmpty());

    SshProfile p;
    p.id = QStringLiteral("abc-123");
    p.name = QStringLiteral("Luckfox");
    p.host = QStringLiteral("192.168.100.2");
    p.user = QStringLiteral("root");
    p.port = 2222;
    c.setProfile(p);
    map = c.settingsMap();
    QCOMPARE(map.size(), 3);
    QCOMPARE(map.value(QStringLiteral("kind")).toString(), QStringLiteral("ssh"));
    QCOMPARE(map.value(QStringLiteral("profileId")).toString(), QStringLiteral("abc-123"));
    QCOMPARE(map.value(QStringLiteral("target")).toString(), QStringLiteral("root@192.168.100.2:2222"));
    QCOMPARE(c.profile(), p);
}

void Tst_sshconnection::summaryAndDisplayName()
{
    SshConnection c;
    QCOMPARE(c.displayName(), QStringLiteral("SSH"));
    QCOMPARE(c.summary(), QStringLiteral("SSH"));
    QVERIFY(c.serverVersion().isEmpty());
    QVERIFY(c.hostKeyType().isEmpty());
    QVERIFY(c.hostKeyFingerprint().isEmpty());
    QVERIFY(c.authMethod().isEmpty());
    QVERIFY(c.remoteHome().isEmpty());
    QCOMPARE(c.exitStatus(), -1);
    QVERIFY(!c.isTransferActive());
    QVERIFY(!c.transferStatus().active);

    SshProfile p;
    p.host = QStringLiteral("10.0.0.24");
    p.user = QStringLiteral("root");
    c.setProfile(p);
    QCOMPARE(c.displayName(), QStringLiteral("root@10.0.0.24"));
    p.name = QStringLiteral("Pico");
    c.setProfile(p);
    QCOMPARE(c.displayName(), QStringLiteral("Pico"));
    QCOMPARE(c.summary(), QStringLiteral("SSH"));
}

void Tst_sshconnection::closeWhileDisconnectedIsNoOp()
{
    SshConnection c;
    QSignalSpy states(&c, &Transport::stateChanged);
    QSignalSpy errors(&c, &Transport::errorOccurred);
    c.close();
    c.close();
    QCOMPARE(states.count(), 0);
    QCOMPARE(errors.count(), 0);
    QCOMPARE(c.state(), Transport::State::Disconnected);
}

void Tst_sshconnection::transferAndHomeWhileDisconnected()
{
    SshConnection c;
    QSignalSpy finished(&c, &SshConnection::transferFinished);
    QSignalSpy home(&c, &SshConnection::remoteHomeReceived);
    SshConnection::TransferRequest request;
    request.localPath = m_dir.filePath(QStringLiteral("nothing.bin"));
    request.remotePath = QStringLiteral("/tmp/nothing.bin");
    QVERIFY(!c.startTransfer(request));
    QVERIFY(!c.isTransferActive());
    c.cancelTransfer();
    c.requestRemoteHome();
    QTest::qWait(50);
    QCOMPARE(finished.count(), 0);
    QCOMPARE(home.count(), 0);
}

void Tst_sshconnection::answersWithoutQuestionAreIgnored()
{
    SshConnection c;
    QSignalSpy states(&c, &Transport::stateChanged);
    c.answerHostKey(true, true);
    c.answerPrompt(QStringLiteral("nothing"), true);
    c.cancelPrompt();
    QTest::qWait(20);
    QCOMPARE(states.count(), 0);
    QCOMPARE(c.state(), Transport::State::Disconnected);
}

void Tst_sshconnection::destroyNeverOpened()
{
    {
        SshConnection c;
        Q_UNUSED(c);
    }
    {
        auto* c = new SshConnection(this);
        SshProfile p;
        p.host = QStringLiteral("board");
        c->setProfile(p);
        delete c;
    }
    QVERIFY(true);
}

void Tst_sshconnection::connectionRefusedReportsError()
{
    const SshProfile p = refusedProfile();
    QVERIFY(p.port != 0);
    SshConnection c;
    c.setProfile(p);
    QSignalSpy errors(&c, &Transport::errorOccurred);
    QSignalSpy states(&c, &Transport::stateChanged);
    QSignalSpy lost(&c, &Transport::connectionLost);
    QSignalSpy hostKeys(&c, &SshConnection::hostKeyVerificationRequired);

    QVERIFY(c.open());
    QCOMPARE(c.state(), Transport::State::Connecting);
    QCOMPARE(states.count(), 1);
    QVERIFY(!c.open());                      // already connecting
    QTRY_COMPARE_WITH_TIMEOUT(c.state(), Transport::State::Disconnected, 30000);
    QCOMPARE(errors.count(), 1);
    QVERIFY(!c.errorString().isEmpty());
    QVERIFY(c.errorString().contains(QStringLiteral("127.0.0.1")));
    QCOMPARE(lost.count(), 0);
    QCOMPARE(hostKeys.count(), 0);
    QCOMPARE(states.count(), 2);

    // The connection is reusable afterwards (a new generation).
    QVERIFY(c.open());
    QTRY_COMPARE_WITH_TIMEOUT(c.state(), Transport::State::Disconnected, 30000);
    QCOMPARE(errors.count(), 2);
    QCOMPARE(states.count(), 4);
    QVERIFY(!QFile::exists(p.knownHostsFile));
}

void Tst_sshconnection::setProfileIgnoredWhileConnecting()
{
    const SshProfile p = refusedProfile();
    QVERIFY(p.port != 0);
    SshConnection c;
    c.setProfile(p);
    QVERIFY(c.open());
    QCOMPARE(c.state(), Transport::State::Connecting);

    SshProfile other = p;
    other.name = QStringLiteral("changed");
    other.host = QStringLiteral("other.host");
    c.setProfile(other);
    QCOMPARE(c.profile(), p);
    QCOMPARE(c.displayName(), p.displayName());

    c.close();
    QCOMPARE(c.state(), Transport::State::Disconnected);
    c.setProfile(other);
    QCOMPARE(c.profile(), other);
}

void Tst_sshconnection::closeWhileConnectingDropsLateSignals()
{
    const SshProfile p = refusedProfile();
    QVERIFY(p.port != 0);
    SshConnection c;
    c.setProfile(p);
    QSignalSpy errors(&c, &Transport::errorOccurred);
    QSignalSpy states(&c, &Transport::stateChanged);
    QVERIFY(c.open());
    c.close();
    QCOMPARE(c.state(), Transport::State::Disconnected);
    QCOMPARE(states.count(), 2);
    QTest::qWait(1500);
    QCOMPARE(errors.count(), 0);
    QCOMPARE(states.count(), 2);
    QCOMPARE(c.state(), Transport::State::Disconnected);
    QVERIFY(c.errorString().isEmpty());
}

void Tst_sshconnection::destroyWhileConnecting()
{
    const SshProfile p = refusedProfile();
    QVERIFY(p.port != 0);
    {
        SshConnection c;
        c.setProfile(p);
        QVERIFY(c.open());
        QCOMPARE(c.state(), Transport::State::Connecting);
    }
    QVERIFY(true);
}

// ---------------------------------------------------------------------------------------
// (c) end to end against the in-process TestSshServer
// ---------------------------------------------------------------------------------------

/// The end-to-end helpers use QVERIFY/QTRY_* and therefore return void; callers stop with
/// this macro when one of them failed.
#define E2E_CONNECT(client)                                                                        \
    do {                                                                                           \
        openAndWaitForPrompt(client);                                                              \
        if (QTest::currentTestFailed()) {                                                          \
            return;                                                                                \
        }                                                                                          \
    } while (false)

TestSshServer::Options Tst_sshconnection::serverOptions(const QString& name)
{
    TestSshServer::Options options;
    options.rootDir = m_dir.filePath(name + QStringLiteral("/root"));
    QDir().mkpath(options.rootDir);
    return options;
}

SshProfile Tst_sshconnection::serverProfile(const TestSshServer& server, const QString& name)
{
    SshProfile p;
    p.host = QStringLiteral("127.0.0.1");
    p.port = server.port();
    p.user = kServerUser;
    p.auth = SshProfile::Auth::Password;   // never Auto here: that would read the real ~/.ssh/id_* keys
    p.keepAliveSeconds = 0;
    p.connectTimeoutSeconds = 5;
    p.knownHostsFile = m_dir.filePath(name + QStringLiteral("/known_hosts"));
    return p;
}

void Tst_sshconnection::openAndWaitForPrompt(TestClient& c)
{
    QCOMPARE(c.conn.state(), State::Disconnected);
    QVERIFY(c.conn.open());
    QCOMPARE(c.conn.state(), State::Connecting);
    QTRY_COMPARE_WITH_TIMEOUT(c.conn.state(), State::Connected, kE2eTimeoutMs);
    QVERIFY2(c.errors.isEmpty(), qPrintable(c.errors.join(QStringLiteral(" | "))));
    QTRY_VERIFY2_WITH_TIMEOUT(c.received.contains("welcome\r\n$ "), c.received.constData(), kE2eTimeoutMs);
}

void Tst_sshconnection::e2ePasswordShellExit()
{
    TestSshServer server(serverOptions(QStringLiteral("password")));
    QVERIFY(server.start());
    ServerSpy spy(&server);
    TestClient c(serverProfile(server, QStringLiteral("password")));
    QSignalSpy shellStarted(&c.conn, &SshConnection::shellStarted);
    QSignalSpy dataSent(&c.conn, &Transport::dataSent);
    QSignalSpy txWritten(&c.conn, &Transport::txBytesWritten);
    QSignalSpy counters(&c.conn, &Transport::countersChanged);

    E2E_CONNECT(c);
    QCOMPARE(c.states, QList<State>({State::Connecting, State::Connected}));
    QCOMPARE(shellStarted.count(), 1);
    QCOMPARE(c.shells, 1);
    QCOMPARE(c.methods, QStringList{QStringLiteral("password")});
    QCOMPARE(c.conn.authMethod(), QStringLiteral("password"));
    QTRY_COMPARE_WITH_TIMEOUT(spy.authMethods.size(), 1, kE2eTimeoutMs);
    QCOMPARE(spy.authMethods.first(), QStringLiteral("password"));
    QCOMPARE(server.lastAuthMethod(), QStringLiteral("password"));
    QCOMPARE(server.lastTerm(), QStringLiteral("xterm-256color"));

    QCOMPARE(c.prompts.size(), 1);
    const SshConnection::AuthPrompt prompt = c.prompts.first();
    QCOMPARE(prompt.kind, SshConnection::PromptKind::Password);
    QCOMPARE(prompt.attempt, 1);
    QCOMPARE(prompt.user, kServerUser);
    QCOMPARE(prompt.host, QStringLiteral("127.0.0.1"));
    QVERIFY(prompt.canRemember);   // an ad-hoc target remembers under "ssh/target/<target>/password"
    QCOMPARE(prompt.rememberTarget, c.profile.displayTarget());
    QVERIFY2(prompt.prompt.contains(QStringLiteral("127.0.0.1")), qPrintable(prompt.prompt));

    QCOMPARE(c.hostKeys.size(), 1);
    QCOMPARE(c.hostKeys.first().status, SshConnection::HostKeyStatus::Unknown);
    QVERIFY2(c.conn.serverVersion().startsWith(QStringLiteral("SSH-2.0")), qPrintable(c.conn.serverVersion()));
    QCOMPARE(c.conn.hostKeyType(), server.hostKeyType());
    QCOMPARE(c.conn.hostKeyFingerprint(), server.hostKeyFingerprintSha256());
    QCOMPARE(c.conn.summary(), QStringLiteral("ssh-ed25519 · password"));
    QCOMPARE(c.conn.displayName(), QStringLiteral("test@127.0.0.1:%1").arg(server.port()));
    QVERIFY(c.conn.isOpen());
    QCOMPARE(c.conn.exitStatus(), -1);

    // Echo round trip: what is typed shows up on the server and comes back with the result.
    const QByteArray command = QByteArrayLiteral("echo E2E-OK\r");
    QCOMPARE(c.conn.write(command), qint64(command.size()));
    QCOMPARE(dataSent.count(), 1);
    QCOMPARE(dataSent.at(0).at(0).toByteArray(), command);
    QTRY_VERIFY2_WITH_TIMEOUT(c.received.contains("echo E2E-OK\r\nE2E-OK\r\n$ "), c.received.constData(), kE2eTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(txWritten.count() >= 1, kE2eTimeoutMs);
    QCOMPARE(txWritten.at(0).at(0).toLongLong(), qint64(command.size()));
    QTRY_VERIFY_WITH_TIMEOUT(server.receivedShellInput().contains("echo E2E-OK\r"), kE2eTimeoutMs);
    QCOMPARE(c.conn.bytesSent(), quint64(command.size()));
    QCOMPARE(c.conn.bytesReceived(), quint64(c.received.size()));
    QVERIFY(counters.count() >= 2);

    // A clean exit: channelClosed(status), Disconnected, no reconnect, no error.
    c.conn.write(QByteArrayLiteral("exit 7\r"));
    QTRY_COMPARE_WITH_TIMEOUT(c.closed.size(), 1, kE2eTimeoutMs);
    QCOMPARE(c.closed.first(), 7);
    QCOMPARE(c.conn.exitStatus(), 7);
    QTRY_COMPARE_WITH_TIMEOUT(c.conn.state(), State::Disconnected, kE2eTimeoutMs);
    QCOMPARE(c.states, QList<State>({State::Connecting, State::Connected, State::Disconnected}));
    QCOMPARE(c.lost, 0);
    QCOMPARE(c.restored, 0);
    QVERIFY2(c.errors.isEmpty(), qPrintable(c.errors.join(QStringLiteral(" | "))));
    QCOMPARE(c.conn.summary(), QStringLiteral("SSH"));
    QCOMPARE(c.conn.write(QByteArrayLiteral("x")), qint64(-1));
    QTRY_COMPARE_WITH_TIMEOUT(server.activeConnections(), 0, kE2eTimeoutMs);
    QCOMPARE(server.connectionCount(), 1);
}

void Tst_sshconnection::e2ePublicKey()
{
    const QString keyPath = m_dir.filePath(QStringLiteral("pubkey/id_plain"));
    QDir().mkpath(QFileInfo(keyPath).absolutePath());
    QString publicLine;
    QVERIFY(TestSshServer::generateClientKeyPair(keyPath, &publicLine));
    QVERIFY(publicLine.startsWith(QStringLiteral("ssh-ed25519 ")));

    TestSshServer::Options options = serverOptions(QStringLiteral("pubkey"));
    options.authorizedPublicKey = publicLine;
    options.allowPassword = false;
    TestSshServer server(options);
    QVERIFY(server.start());

    SshProfile p = serverProfile(server, QStringLiteral("pubkey"));
    p.auth = SshProfile::Auth::PublicKey;
    p.identityFile = keyPath;
    TestClient c(p);
    E2E_CONNECT(c);
    QCOMPARE(c.prompts.size(), 0);
    QCOMPARE(c.methods, QStringList{QStringLiteral("publickey")});
    QCOMPARE(c.conn.authMethod(), QStringLiteral("publickey"));
    QTRY_COMPARE_WITH_TIMEOUT(server.lastAuthMethod(), QStringLiteral("publickey"), kE2eTimeoutMs);
    QCOMPARE(c.conn.summary(), QStringLiteral("ssh-ed25519 · publickey"));
    c.conn.write(QByteArrayLiteral("echo KEY-OK\r"));
    QTRY_VERIFY_WITH_TIMEOUT(c.received.contains("KEY-OK\r\n$ "), kE2eTimeoutMs);
    c.conn.close();
    QCOMPARE(c.conn.state(), State::Disconnected);

    // A key the server does not know is rejected without a prompt (the server offers no
    // password).
    const QString otherPath = m_dir.filePath(QStringLiteral("pubkey/id_other"));
    QString otherLine;
    QVERIFY(TestSshServer::generateClientKeyPair(otherPath, &otherLine));
    SshProfile wrong = p;
    wrong.identityFile = otherPath;
    TestClient d(wrong);
    QVERIFY(d.conn.open());
    QTRY_COMPARE_WITH_TIMEOUT(d.conn.state(), State::Disconnected, kE2eTimeoutMs);
    QCOMPARE(d.prompts.size(), 0);
    QCOMPARE(d.errors.size(), 1);
    QVERIFY2(d.errors.first().contains(QStringLiteral("Authentication failed")), qPrintable(d.errors.first()));
    QVERIFY(d.conn.authMethod().isEmpty());
}

void Tst_sshconnection::e2ePublicKeyPassphrase()
{
    const QString keyPath = m_dir.filePath(QStringLiteral("passphrase/id_enc"));
    QDir().mkpath(QFileInfo(keyPath).absolutePath());
    const QString passphrase = QStringLiteral("hunter2");
    QString publicLine;
    QVERIFY(TestSshServer::generateClientKeyPair(keyPath, &publicLine, passphrase));

    TestSshServer::Options options = serverOptions(QStringLiteral("passphrase"));
    options.authorizedPublicKey = publicLine;
    options.allowPassword = false;
    TestSshServer server(options);
    QVERIFY(server.start());

    SshProfile p = serverProfile(server, QStringLiteral("passphrase"));
    p.auth = SshProfile::Auth::PublicKey;
    p.identityFile = keyPath;

    // The wrong passphrase first, then the right one: attempt 2 of the same key.
    TestClient c(p);
    c.responses = {QStringLiteral("not-it"), passphrase};
    E2E_CONNECT(c);
    QCOMPARE(c.prompts.size(), 2);
    QCOMPARE(c.prompts.at(0).kind, SshConnection::PromptKind::Passphrase);
    QCOMPARE(c.prompts.at(0).attempt, 1);
    QCOMPARE(c.prompts.at(0).keyFile, keyPath);
    QVERIFY2(c.prompts.at(0).prompt.contains(QStringLiteral("passphrase")), qPrintable(c.prompts.at(0).prompt));
    QVERIFY(!c.prompts.at(0).echo);
    QVERIFY(c.prompts.at(0).canRemember);   // remembered per key file, whatever the target
    QCOMPARE(c.prompts.at(0).rememberTarget, QDir::cleanPath(QFileInfo(keyPath).absoluteFilePath()));
    QCOMPARE(c.prompts.at(1).kind, SshConnection::PromptKind::Passphrase);
    QCOMPARE(c.prompts.at(1).attempt, 2);
    QCOMPARE(c.methods, QStringList{QStringLiteral("publickey")});
    QTRY_COMPARE_WITH_TIMEOUT(server.lastAuthMethod(), QStringLiteral("publickey"), kE2eTimeoutMs);
    c.conn.close();

    // Three wrong passphrases exhaust the key; nothing else is offered.
    TestClient d(p);
    d.responses = {QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")};
    QVERIFY(d.conn.open());
    QTRY_COMPARE_WITH_TIMEOUT(d.conn.state(), State::Disconnected, kE2eTimeoutMs);
    QCOMPARE(d.prompts.size(), 3);
    QCOMPARE(d.prompts.at(2).attempt, 3);
    QCOMPARE(d.errors.size(), 1);
    QVERIFY2(d.errors.first().contains(QStringLiteral("passphrase"), Qt::CaseInsensitive), qPrintable(d.errors.first()));
}

void Tst_sshconnection::e2eKeyboardInteractive()
{
    TestSshServer::Options options = serverOptions(QStringLiteral("kbdint"));
    options.allowPassword = false;
    options.allowPublicKey = false;
    options.allowKeyboardInteractive = true;
    TestSshServer server(options);
    QVERIFY(server.start());

    SshProfile p = serverProfile(server, QStringLiteral("kbdint"));
    p.auth = SshProfile::Auth::KeyboardInteractive;

    // A wrong token first (round rejected -> attempt 2), then the right one.
    TestClient c(p);
    c.responses = {QStringLiteral("000000"), options.kbdintAnswer};
    E2E_CONNECT(c);
    QCOMPARE(c.prompts.size(), 2);
    QCOMPARE(c.prompts.at(0).kind, SshConnection::PromptKind::KeyboardInteractive);
    QCOMPARE(c.prompts.at(0).title, QStringLiteral("TestSshServer"));
    QCOMPARE(c.prompts.at(0).prompt, options.kbdintPrompt);
    QVERIFY(c.prompts.at(0).instruction.isEmpty());
    QVERIFY(!c.prompts.at(0).echo);
    QCOMPARE(c.prompts.at(0).attempt, 1);
    QCOMPARE(c.prompts.at(1).attempt, 2);
    QCOMPARE(c.methods, QStringList{QStringLiteral("keyboard-interactive")});
    QCOMPARE(c.conn.authMethod(), QStringLiteral("keyboard-interactive"));
    QTRY_COMPARE_WITH_TIMEOUT(server.lastAuthMethod(), QStringLiteral("keyboard-interactive"), kE2eTimeoutMs);
    c.conn.close();

    // Auth::Password against a server that only offers keyboard-interactive (PAM style) falls
    // back to it and asks the server's question.
    SshProfile viaPassword = p;
    viaPassword.auth = SshProfile::Auth::Password;
    TestClient d(viaPassword);
    d.responses = {options.kbdintAnswer};
    E2E_CONNECT(d);
    QCOMPARE(d.prompts.size(), 1);
    QCOMPARE(d.prompts.first().kind, SshConnection::PromptKind::KeyboardInteractive);
    QCOMPARE(d.conn.authMethod(), QStringLiteral("keyboard-interactive"));
    d.conn.close();

    // Three rejected rounds end the method.
    TestClient e(p);
    e.responses = {QStringLiteral("1"), QStringLiteral("2"), QStringLiteral("3")};
    QVERIFY(e.conn.open());
    QTRY_COMPARE_WITH_TIMEOUT(e.conn.state(), State::Disconnected, kE2eTimeoutMs);
    QCOMPARE(e.prompts.size(), 3);
    QCOMPARE(e.errors.size(), 1);
    QVERIFY2(e.errors.first().contains(QStringLiteral("Authentication failed")), qPrintable(e.errors.first()));

    // The (first) hidden prompt of a round can be remembered like a password: the answer lands
    // under the ad-hoc target key and the next connect asks nothing.
    const QString key = QStringLiteral("ssh/target/%1/password").arg(p.displayTarget());
    QVERIFY(!SecretStore::contains(key));
    QVERIFY(c.prompts.at(0).canRemember);
    QCOMPARE(c.prompts.at(0).rememberTarget, p.displayTarget());
    TestClient f(p);
    f.responses = {options.kbdintAnswer};
    f.rememberResponse = true;
    E2E_CONNECT(f);
    QCOMPARE(f.prompts.size(), 1);
    QTRY_COMPARE_WITH_TIMEOUT(SecretStore::load(key).value_or(QString()), options.kbdintAnswer, kE2eTimeoutMs);
    f.conn.close();
    TestClient g(p);
    E2E_CONNECT(g);
    QCOMPARE(g.prompts.size(), 0);
    QCOMPARE(g.conn.authMethod(), QStringLiteral("keyboard-interactive"));
    g.conn.close();
    // A stale saved answer is dropped before the prompt (attempt 2).
    QVERIFY(SecretStore::store(key, QStringLiteral("000000")));
    TestClient h(p);
    h.responses = {options.kbdintAnswer};
    E2E_CONNECT(h);
    QCOMPARE(h.prompts.size(), 1);
    QCOMPARE(h.prompts.first().attempt, 2);
    QTRY_VERIFY_WITH_TIMEOUT(!SecretStore::contains(key), kE2eTimeoutMs);
    h.conn.close();
}

void Tst_sshconnection::e2eWrongPasswordThenCancel()
{
    TestSshServer server(serverOptions(QStringLiteral("wrongpw")));
    QVERIFY(server.start());
    const SshProfile p = serverProfile(server, QStringLiteral("wrongpw"));

    TestClient c(p);
    c.responses = {QStringLiteral("definitely-wrong")};
    c.cancelAt = 2;
    QVERIFY(c.conn.open());
    QTRY_COMPARE_WITH_TIMEOUT(c.conn.state(), State::Disconnected, kE2eTimeoutMs);
    QCOMPARE(c.prompts.size(), 2);
    QCOMPARE(c.prompts.at(0).attempt, 1);
    QCOMPARE(c.prompts.at(1).attempt, 2);
    QCOMPARE(c.prompts.at(1).kind, SshConnection::PromptKind::Password);
    QCOMPARE(c.errors.size(), 1);
    QVERIFY2(c.errors.first().contains(QStringLiteral("cancel"), Qt::CaseInsensitive), qPrintable(c.errors.first()));
    QCOMPARE(c.conn.errorString(), c.errors.first());
    QCOMPARE(c.states, QList<State>({State::Connecting, State::Disconnected}));
    QCOMPARE(c.methods.size(), 0);
    QVERIFY(c.conn.authMethod().isEmpty());
    QCOMPARE(c.shells, 0);
    QCOMPARE(c.lost, 0);
    QVERIFY(server.lastAuthMethod().isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(server.activeConnections(), 0, kE2eTimeoutMs);

    // Three wrong passwords exhaust the method; the host key is known from the first try.
    TestClient d(p);
    d.responses = {QStringLiteral("w1"), QStringLiteral("w2"), QStringLiteral("w3")};
    QVERIFY(d.conn.open());
    QTRY_COMPARE_WITH_TIMEOUT(d.conn.state(), State::Disconnected, kE2eTimeoutMs);
    QCOMPARE(d.prompts.size(), 3);
    QCOMPARE(d.prompts.at(2).attempt, 3);
    QCOMPARE(d.hostKeys.size(), 0);
    QCOMPARE(d.errors.size(), 1);
    QVERIFY2(d.errors.first().contains(QStringLiteral("Authentication failed")), qPrintable(d.errors.first()));

    // The connection object is reusable afterwards.
    d.responses.clear();
    QVERIFY(d.conn.open());
    QTRY_COMPARE_WITH_TIMEOUT(d.conn.state(), State::Connected, kE2eTimeoutMs);
    QCOMPARE(d.prompts.size(), 4);
    d.conn.close();
}

void Tst_sshconnection::e2eHostKeyUnknownRememberThenSilent()
{
    TestSshServer server(serverOptions(QStringLiteral("hostkey")));
    QVERIFY(server.start());
    const SshProfile p = serverProfile(server, QStringLiteral("hostkey"));
    QVERIFY(!QFileInfo::exists(p.knownHostsFile));

    TestClient c(p);
    E2E_CONNECT(c);
    QCOMPARE(c.hostKeys.size(), 1);
    const SshConnection::HostKeyInfo info = c.hostKeys.first();
    QCOMPARE(info.status, SshConnection::HostKeyStatus::Unknown);
    QCOMPARE(info.host, QStringLiteral("127.0.0.1"));
    QCOMPARE(info.port, server.port());
    QCOMPARE(info.keyType, QStringLiteral("ssh-ed25519"));
    QCOMPARE(info.fingerprintSha256, server.hostKeyFingerprintSha256());
    QVERIFY(info.fingerprintMd5.startsWith(QStringLiteral("MD5:")));
    QCOMPARE(info.publicKeyLine, server.hostKeyPublicLine());
    QCOMPARE(info.knownHostsFile, QFileInfo(p.knownHostsFile).absoluteFilePath());
    QVERIFY(info.message.isEmpty());
    // "remember" appended exactly the line the server expects.
    QCOMPARE(knownHostsLines(p.knownHostsFile), QStringList{server.knownHostsLine()});
    c.conn.close();

    // The second connect is silent.
    TestClient d(p);
    E2E_CONNECT(d);
    QCOMPARE(d.hostKeys.size(), 0);
    QCOMPARE(d.conn.hostKeyFingerprint(), server.hostKeyFingerprintSha256());
    d.conn.close();
    QCOMPARE(knownHostsLines(p.knownHostsFile), QStringList{server.knownHostsLine()});

    // "Connect once" against a fresh file: connects, writes nothing.
    SshProfile once = p;
    once.knownHostsFile = m_dir.filePath(QStringLiteral("hostkey/known_hosts_once"));
    TestClient e(once);
    e.rememberKey = false;
    E2E_CONNECT(e);
    QCOMPARE(e.hostKeys.size(), 1);
    e.conn.close();
    QVERIFY(!QFileInfo::exists(once.knownHostsFile));
}

void Tst_sshconnection::e2eHostKeyRejected()
{
    TestSshServer server(serverOptions(QStringLiteral("reject")));
    QVERIFY(server.start());
    const SshProfile p = serverProfile(server, QStringLiteral("reject"));
    TestClient c(p);
    c.acceptKey = false;
    QVERIFY(c.conn.open());
    QTRY_COMPARE_WITH_TIMEOUT(c.conn.state(), State::Disconnected, kE2eTimeoutMs);
    QCOMPARE(c.hostKeys.size(), 1);
    QCOMPARE(c.prompts.size(), 0);
    QCOMPARE(c.errors.size(), 1);
    QVERIFY2(c.errors.first().contains(QStringLiteral("Host key rejected")), qPrintable(c.errors.first()));
    QCOMPARE(c.states, QList<State>({State::Connecting, State::Disconnected}));
    QVERIFY(c.conn.authMethod().isEmpty());
    QCOMPARE(c.conn.hostKeyFingerprint(), server.hostKeyFingerprintSha256());   // the fact stays readable
    QVERIFY(!QFileInfo::exists(p.knownHostsFile));
    QTRY_COMPARE_WITH_TIMEOUT(server.activeConnections(), 0, kE2eTimeoutMs);
    QVERIFY(server.lastAuthMethod().isEmpty());
}

void Tst_sshconnection::e2eHostKeyChangedReplaced()
{
    TestSshServer server(serverOptions(QStringLiteral("changed")));
    QVERIFY(server.start());
    const SshProfile p = serverProfile(server, QStringLiteral("changed"));
    const QString token = QStringLiteral("[127.0.0.1]:%1").arg(server.port());

    // known_hosts holds another ed25519 key for this host:port (a reinstalled board), plus an
    // unrelated entry that must survive the rewrite untouched.
    const QString otherKeyPath = m_dir.filePath(QStringLiteral("changed/other_key"));
    QString otherLine;
    QVERIFY(TestSshServer::generateClientKeyPair(otherKeyPath, &otherLine));
    QVERIFY(otherLine != server.hostKeyPublicLine());
    const QString stale = token + QLatin1Char(' ') + otherLine;
    const QString unrelated = QStringLiteral("[10.0.0.99]:2222 ") + otherLine;
    QVERIFY(writeFile(p.knownHostsFile, (stale + QLatin1Char('\n') + unrelated + QLatin1Char('\n')).toUtf8()));

    TestClient c(p);
    E2E_CONNECT(c);
    QCOMPARE(c.hostKeys.size(), 1);
    QCOMPARE(c.hostKeys.first().status, SshConnection::HostKeyStatus::Changed);
    QCOMPARE(c.hostKeys.first().keyType, QStringLiteral("ssh-ed25519"));
    QCOMPARE(c.hostKeys.first().publicKeyLine, server.hostKeyPublicLine());
    c.conn.close();
    // Accept + remember replaced the stale line: exactly one line for the host, the real key.
    QCOMPARE(knownHostsLines(p.knownHostsFile), QStringList({unrelated, server.knownHostsLine()}));

    // Known now.
    TestClient d(p);
    E2E_CONNECT(d);
    QCOMPARE(d.hostKeys.size(), 0);
    d.conn.close();

    // Changed and rejected: no connection, the file untouched.
    QVERIFY(writeFile(p.knownHostsFile, (stale + QLatin1Char('\n')).toUtf8()));
    TestClient e(p);
    e.acceptKey = false;
    QVERIFY(e.conn.open());
    QTRY_COMPARE_WITH_TIMEOUT(e.conn.state(), State::Disconnected, kE2eTimeoutMs);
    QCOMPARE(e.hostKeys.size(), 1);
    QCOMPARE(e.hostKeys.first().status, SshConnection::HostKeyStatus::Changed);
    QCOMPARE(e.errors.size(), 1);
    QVERIFY2(e.errors.first().contains(QStringLiteral("rejected")), qPrintable(e.errors.first()));
    QCOMPARE(e.prompts.size(), 0);
    QCOMPARE(knownHostsLines(p.knownHostsFile), QStringList{stale});

    // Changed and "connect once": connects, leaves the stale line alone.
    TestClient f(p);
    f.rememberKey = false;
    E2E_CONNECT(f);
    QCOMPARE(f.hostKeys.size(), 1);
    QCOMPARE(f.hostKeys.first().status, SshConnection::HostKeyStatus::Changed);
    f.conn.close();
    QCOMPARE(knownHostsLines(p.knownHostsFile), QStringList{stale});
}

void Tst_sshconnection::e2eSavedPassword()
{
    TestSshServer server(serverOptions(QStringLiteral("saved")));
    QVERIFY(server.start());
    SshProfile p = serverProfile(server, QStringLiteral("saved"));
    p.id = QStringLiteral("e2e-saved-password");
    p.passwordSaved = true;
    const QString key = QStringLiteral("ssh/%1/password").arg(p.id);
    QVERIFY(SecretStore::store(key, kServerPassword));

    // A saved password connects without any prompt.
    TestClient c(p);
    E2E_CONNECT(c);
    QCOMPARE(c.prompts.size(), 0);
    QCOMPARE(c.conn.authMethod(), QStringLiteral("password"));
    QCOMPARE(SecretStore::load(key).value_or(QString()), kServerPassword);
    c.conn.close();

    // A stale saved password is discarded before the prompt (attempt 2, rememberable), and the
    // answer given with "remember" replaces it.
    QVERIFY(SecretStore::store(key, QStringLiteral("stale-password")));
    TestClient d(p);
    d.rememberResponse = true;
    bool storeHadKeyAtPrompt = true;
    connect(&d.conn, &SshConnection::authPromptRequired, &d.conn,
            [&storeHadKeyAtPrompt, key](const SshConnection::AuthPrompt&) { storeHadKeyAtPrompt = SecretStore::contains(key); });
    E2E_CONNECT(d);
    QCOMPARE(d.prompts.size(), 1);
    QCOMPARE(d.prompts.first().kind, SshConnection::PromptKind::Password);
    QCOMPARE(d.prompts.first().attempt, 2);
    QVERIFY(d.prompts.first().canRemember);
    QVERIFY(!storeHadKeyAtPrompt);
    QTRY_COMPARE_WITH_TIMEOUT(SecretStore::load(key).value_or(QString()), kServerPassword, kE2eTimeoutMs);
    d.conn.close();

    // Without "remember" nothing is written back.
    QVERIFY(SecretStore::remove(key));
    TestClient e(p);
    E2E_CONNECT(e);
    QCOMPARE(e.prompts.size(), 1);
    QCOMPARE(e.prompts.first().attempt, 1);
    QTest::qWait(100);
    QVERIFY(!SecretStore::contains(key));
    e.conn.close();
}

void Tst_sshconnection::e2eBanner()
{
    TestSshServer::Options options = serverOptions(QStringLiteral("banner"));
    options.banner = QStringLiteral("Welcome to the test box\nAuthorised access only\n");
    TestSshServer server(options);
    QVERIFY(server.start());
    TestClient c(serverProfile(server, QStringLiteral("banner")));
    QStringList order;
    connect(&c.conn, &SshConnection::bannerReceived, &c.conn, [&order](const QString&) { order.append(QStringLiteral("banner")); });
    connect(&c.conn, &SshConnection::authenticated, &c.conn, [&order](const QString&) { order.append(QStringLiteral("auth")); });
    connect(&c.conn, &SshConnection::shellStarted, &c.conn, [&order] { order.append(QStringLiteral("shell")); });
    E2E_CONNECT(c);
    QCOMPARE(c.banners.size(), 1);
    QVERIFY2(c.banners.first().contains(QStringLiteral("Welcome to the test box")), qPrintable(c.banners.first()));
    QVERIFY(c.banners.first().contains(QStringLiteral("Authorised access only")));
    QCOMPARE(order, QStringList({QStringLiteral("banner"), QStringLiteral("auth"), QStringLiteral("shell")}));
    c.conn.close();

    // No banner configured: no signal.
    TestSshServer plain(serverOptions(QStringLiteral("banner-none")));
    QVERIFY(plain.start());
    TestClient d(serverProfile(plain, QStringLiteral("banner-none")));
    E2E_CONNECT(d);
    QCOMPARE(d.banners.size(), 0);
    d.conn.close();
}

void Tst_sshconnection::e2eTerminalSize()
{
    TestSshServer server(serverOptions(QStringLiteral("size")));
    QVERIFY(server.start());
    SshProfile p = serverProfile(server, QStringLiteral("size"));
    p.terminalType = QStringLiteral("vt220");
    TestClient c(p);
    c.conn.notifyTerminalSize(132, 43);   // before the connect: the PTY is requested with it
    QCOMPARE(c.conn.lastTerminalSize(), QStringLiteral("132 x 43"));
    E2E_CONNECT(c);
    QCOMPARE(server.lastPtySize(), QSize(132, 43));
    QCOMPARE(server.lastTerm(), QStringLiteral("vt220"));
    c.conn.write(QByteArrayLiteral("size\r"));
    QTRY_VERIFY2_WITH_TIMEOUT(c.received.contains("COLS=132 ROWS=43\r\n$ "), c.received.constData(), kE2eTimeoutMs);

    // During the session: a window-change request, then the shell sees the new size.
    c.received.clear();
    c.conn.notifyTerminalSize(100, 30);
    QCOMPARE(c.conn.lastTerminalSize(), QStringLiteral("100 x 30"));
    c.conn.write(QByteArrayLiteral("size\r"));
    QTRY_VERIFY2_WITH_TIMEOUT(c.received.contains("COLS=100 ROWS=30\r\n$ "), c.received.constData(), kE2eTimeoutMs);
    QCOMPARE(server.lastPtySize(), QSize(100, 30));
    c.received.clear();
    c.conn.write(QByteArrayLiteral("env\r"));
    QTRY_VERIFY2_WITH_TIMEOUT(c.received.contains("TERM=vt220 COLS=100 ROWS=30\r\n"), c.received.constData(), kE2eTimeoutMs);
    c.conn.close();
}

void Tst_sshconnection::e2eStartupCommand()
{
    TestSshServer server(serverOptions(QStringLiteral("startup")));
    QVERIFY(server.start());
    SshProfile p = serverProfile(server, QStringLiteral("startup"));
    p.startupCommand = QStringLiteral("echo STARTUP-OK");
    TestClient c(p);
    QSignalSpy dataSent(&c.conn, &Transport::dataSent);
    E2E_CONNECT(c);
    QTRY_VERIFY2_WITH_TIMEOUT(c.received.contains("echo STARTUP-OK\r\nSTARTUP-OK\r\n$ "), c.received.constData(), kE2eTimeoutMs);
    QCOMPARE(dataSent.count(), 1);   // the startup command flows through dataSent like typed input
    QCOMPARE(dataSent.at(0).at(0).toByteArray(), QByteArrayLiteral("echo STARTUP-OK\r"));
    QTRY_VERIFY_WITH_TIMEOUT(server.receivedShellInput().contains("echo STARTUP-OK\r"), kE2eTimeoutMs);
    c.conn.close();
}

void Tst_sshconnection::e2eRemoteCommandExec()
{
    TestSshServer server(serverOptions(QStringLiteral("exec")));
    QVERIFY(server.start());
    ServerSpy spy(&server);
    SshProfile p = serverProfile(server, QStringLiteral("exec"));
    p.remoteCommand = QStringLiteral("echo EXEC-OUT");
    TestClient c(p);
    QVERIFY(c.conn.open());
    QTRY_COMPARE_WITH_TIMEOUT(c.closed.size(), 1, kE2eTimeoutMs);
    QCOMPARE(c.closed.first(), 0);
    QCOMPARE(c.conn.exitStatus(), 0);
    QTRY_COMPARE_WITH_TIMEOUT(c.conn.state(), State::Disconnected, kE2eTimeoutMs);
    QVERIFY2(c.received.contains("EXEC-OUT\r\n"), c.received.constData());
    QVERIFY(!c.received.contains("welcome"));
    QVERIFY(!c.received.contains("$ "));
    QCOMPARE(c.shells, 1);   // shellStarted() also marks the start of an exec channel
    QCOMPARE(c.states, QList<State>({State::Connecting, State::Connected, State::Disconnected}));
    QCOMPARE(c.lost, 0);
    QVERIFY2(c.errors.isEmpty(), qPrintable(c.errors.join(QStringLiteral(" | "))));
    QTRY_COMPARE_WITH_TIMEOUT(spy.execCommands.size(), 1, kE2eTimeoutMs);
    QCOMPARE(spy.execCommands.first(), QStringLiteral("echo EXEC-OUT"));
    QCOMPARE(server.lastExecCommand(), QStringLiteral("echo EXEC-OUT"));
    QCOMPARE(spy.shells, 0);

    // The exit status of the command is reported.
    p.remoteCommand = QStringLiteral("exit 5");
    TestClient d(p);
    QVERIFY(d.conn.open());
    QTRY_COMPARE_WITH_TIMEOUT(d.closed.size(), 1, kE2eTimeoutMs);
    QCOMPARE(d.closed.first(), 5);
    QTRY_COMPARE_WITH_TIMEOUT(d.conn.state(), State::Disconnected, kE2eTimeoutMs);
    QCOMPARE(d.conn.exitStatus(), 5);
    QCOMPARE(d.lost, 0);
}

void Tst_sshconnection::e2eBurstAndBigOutput()
{
    TestSshServer server(serverOptions(QStringLiteral("big")));
    QVERIFY(server.start());
    TestClient c(serverProfile(server, QStringLiteral("big")));
    QSignalSpy txWritten(&c.conn, &Transport::txBytesWritten);
    E2E_CONNECT(c);

    // 64 KiB typed at once arrive intact and in order (the shell echoes every byte, too).
    const QByteArray burst = letterBurst(64 * 1024);
    QCOMPARE(c.conn.write(burst), qint64(burst.size()));
    QTRY_VERIFY_WITH_TIMEOUT(server.receivedShellInput().endsWith(burst), 30000);
    QCOMPARE(server.receivedShellInput().size(), burst.size());
    QTRY_VERIFY_WITH_TIMEOUT(c.received.contains(burst), 30000);
    QTRY_VERIFY_WITH_TIMEOUT(txWritten.count() >= 1, kE2eTimeoutMs);
    qint64 written = 0;
    for (const QList<QVariant>& args : txWritten) {
        written += args.at(0).toLongLong();
    }
    QCOMPARE(written, qint64(burst.size()));
    QCOMPARE(c.conn.bytesSent(), quint64(burst.size()));
    c.conn.write(QByteArrayLiteral("\r"));   // runs the (nonsense) line, the prompt returns
    QTRY_VERIFY_WITH_TIMEOUT(c.received.endsWith(": not found\r\n$ "), kE2eTimeoutMs);

    // "big 20000" (about 230 KB) arrives complete and ordered.
    c.received.clear();
    c.conn.write(QByteArrayLiteral("big 20000\r"));
    QTRY_VERIFY_WITH_TIMEOUT(c.received.endsWith("line 20000\r\n$ "), 60000);
    const QByteArray expected = QByteArrayLiteral("big 20000\r\n") + bigOutput(20000) + QByteArrayLiteral("$ ");
    QCOMPARE(c.received.size(), expected.size());
    QVERIFY(c.received == expected);
    QVERIFY(c.conn.bytesReceived() > quint64(expected.size()));
    c.conn.close();
}

void Tst_sshconnection::e2eDropReconnect()
{
    TestSshServer server(serverOptions(QStringLiteral("drop")));
    QVERIFY(server.start());
    SshProfile p = serverProfile(server, QStringLiteral("drop"));
    p.keepAliveSeconds = 1;
    TestClient c(p);
    QVERIFY(c.conn.autoReconnect());
    QSignalSpy lostSpy(&c.conn, &Transport::connectionLost);
    QSignalSpy restoredSpy(&c.conn, &Transport::connectionRestored);
    E2E_CONNECT(c);

    // A hung shell: keep-alives keep flowing for a while without any effect on the state.
    c.conn.write(QByteArrayLiteral("hang\r"));
    QTRY_VERIFY_WITH_TIMEOUT(c.received.contains("hang\r\n"), kE2eTimeoutMs);
    QTest::qWait(2500);
    QCOMPARE(c.conn.state(), State::Connected);
    QCOMPARE(c.lost, 0);
    QVERIFY2(c.errors.isEmpty(), qPrintable(c.errors.join(QStringLiteral(" | "))));

    // The network cut is noticed within 5 s: connectionLost, Reconnecting, then the reconnect
    // (2 s backoff) restores the session with the same password and a fresh shell prompt.
    c.received.clear();
    QElapsedTimer timer;
    timer.start();
    server.dropAllClients();
    QTRY_COMPARE_WITH_TIMEOUT(c.lost, 1, 5000);
    QVERIFY2(timer.elapsed() < 5000, qPrintable(QString::number(timer.elapsed())));
    QCOMPARE(lostSpy.count(), 1);
    QCOMPARE(lostSpy.at(0).at(0).toString(), c.conn.displayName());
    QCOMPARE(c.conn.state(), State::Reconnecting);
    QCOMPARE(c.errors.size(), 1);
    QVERIFY(!c.conn.errorString().isEmpty());
    QCOMPARE(c.closed.size(), 0);
    QCOMPARE(c.conn.write(QByteArrayLiteral("x")), qint64(-1));
    QTRY_COMPARE_WITH_TIMEOUT(c.restored, 1, kE2eTimeoutMs);
    QCOMPARE(restoredSpy.at(0).at(0).toString(), c.conn.displayName());
    QCOMPARE(c.conn.state(), State::Connected);
    QCOMPARE(c.shells, 2);
    QCOMPARE(c.prompts.size(), 1);    // the password that worked is reused
    QCOMPARE(c.hostKeys.size(), 1);   // the remembered host key needs no question
    QCOMPARE(c.states, QList<State>({State::Connecting, State::Connected, State::Reconnecting, State::Connected}));
    QTRY_VERIFY2_WITH_TIMEOUT(c.received.contains("welcome\r\n$ "), c.received.constData(), kE2eTimeoutMs);
    c.conn.write(QByteArrayLiteral("echo AGAIN\r"));
    QTRY_VERIFY_WITH_TIMEOUT(c.received.contains("AGAIN\r\n$ "), kE2eTimeoutMs);
    QCOMPARE(server.connectionCount(), 2);
    QCOMPARE(c.errors.size(), 1);
    c.conn.close();
    QCOMPARE(c.conn.state(), State::Disconnected);
    QTRY_COMPARE_WITH_TIMEOUT(server.activeConnections(), 0, kE2eTimeoutMs);
}

void Tst_sshconnection::e2eDropNoReconnect()
{
    TestSshServer server(serverOptions(QStringLiteral("dropoff")));
    QVERIFY(server.start());
    SshProfile p = serverProfile(server, QStringLiteral("dropoff"));
    p.keepAliveSeconds = 1;

    // autoReconnect off: the drop ends in Disconnected.
    TestClient c(p);
    c.conn.setAutoReconnect(false);
    E2E_CONNECT(c);
    server.dropAllClients();
    QTRY_COMPARE_WITH_TIMEOUT(c.conn.state(), State::Disconnected, 5000);
    QCOMPARE(c.lost, 1);
    QCOMPARE(c.restored, 0);
    QCOMPARE(c.closed.size(), 0);
    QCOMPARE(c.errors.size(), 1);
    QCOMPARE(c.states, QList<State>({State::Connecting, State::Connected, State::Disconnected}));
    QTest::qWait(500);
    QCOMPARE(c.conn.state(), State::Disconnected);
    QCOMPARE(server.connectionCount(), 1);

    // Turning autoReconnect off while a reconnect is pending ends it, too.
    TestClient d(p);
    E2E_CONNECT(d);
    server.setAcceptConnections(false);
    server.dropAllClients();
    QTRY_COMPARE_WITH_TIMEOUT(d.conn.state(), State::Reconnecting, 5000);
    d.conn.setAutoReconnect(false);
    QCOMPARE(d.conn.state(), State::Disconnected);
    server.setAcceptConnections(true);
    QTest::qWait(2500);   // past the first backoff step
    QCOMPARE(d.conn.state(), State::Disconnected);
    QCOMPARE(d.restored, 0);
    QCOMPARE(d.states, QList<State>({State::Connecting, State::Connected, State::Reconnecting, State::Disconnected}));
    QCOMPARE(server.connectionCount(), 2);
}

void Tst_sshconnection::e2eCloseFromConnectionLostSlot()
{
    // A receiver of connectionLost() / errorOccurred() may close the connection or turn the
    // reconnect off synchronously (a tab closing on the error). The facade used to decide about
    // the reconnect afterwards without looking again and ended up in Reconnecting for good,
    // with a worker that ignores beginReconnect() for a closed generation.
    TestSshServer server(serverOptions(QStringLiteral("lostslot")));
    QVERIFY(server.start());
    SshProfile p = serverProfile(server, QStringLiteral("lostslot"));
    p.keepAliveSeconds = 1;

    TestClient c(p);
    connect(&c.conn, &Transport::connectionLost, &c.conn, [&c] { c.conn.close(); });
    E2E_CONNECT(c);
    server.dropAllClients();
    QTRY_COMPARE_WITH_TIMEOUT(c.lost, 1, 5000);
    QCOMPARE(c.conn.state(), State::Disconnected);
    QCOMPARE(c.states, QList<State>({State::Connecting, State::Connected, State::Disconnected}));
    QTest::qWait(2500);   // past the first backoff step: nothing reconnects
    QCOMPARE(c.conn.state(), State::Disconnected);
    QCOMPARE(c.restored, 0);
    QCOMPARE(c.states.size(), 3);
    QCOMPARE(server.connectionCount(), 1);

    // Usable again afterwards.
    QVERIFY(c.conn.open());
    QTRY_COMPARE_WITH_TIMEOUT(c.conn.state(), State::Connected, kE2eTimeoutMs);
    c.conn.close();
    QTRY_COMPARE_WITH_TIMEOUT(server.activeConnections(), 0, kE2eTimeoutMs);

    // setAutoReconnect(false) from the error slot ends in Disconnected as well.
    TestClient d(p);
    connect(&d.conn, &Transport::errorOccurred, &d.conn, [&d] { d.conn.setAutoReconnect(false); });
    E2E_CONNECT(d);
    server.dropAllClients();
    QTRY_COMPARE_WITH_TIMEOUT(d.conn.state(), State::Disconnected, 5000);
    QCOMPARE(d.lost, 1);
    QTest::qWait(2500);
    QCOMPARE(d.conn.state(), State::Disconnected);
    QCOMPARE(d.restored, 0);
    QCOMPARE(d.states, QList<State>({State::Connecting, State::Connected, State::Disconnected}));
    QCOMPARE(server.connectionCount(), 3);
}

void Tst_sshconnection::e2eLocalForward()
{
    LocalEchoServer echo;
    QVERIFY(echo.listen(QHostAddress::LocalHost, 0));
    const quint16 localPort = closedLocalPort();
    QVERIFY(localPort != 0);
    quint16 localhostPort = closedLocalPort();
    for (int attempt = 0; localhostPort == localPort && attempt < 5; ++attempt) {
        localhostPort = closedLocalPort();
    }
    QVERIFY(localhostPort != 0 && localhostPort != localPort);
    QTcpServer busy;   // a forward whose local port is taken must fail without harming the others
    QVERIFY(busy.listen(QHostAddress::LocalHost, 0));

    TestSshServer server(serverOptions(QStringLiteral("forward")));
    QVERIFY(server.start());
    ServerSpy spy(&server);
    SshProfile p = serverProfile(server, QStringLiteral("forward"));
    SshLocalForward forward;   // bindAddress at its default "127.0.0.1"
    forward.localPort = localPort;
    forward.remoteHost = QStringLiteral("127.0.0.1");
    forward.remotePort = echo.serverPort();
    SshLocalForward taken = forward;
    taken.localPort = busy.serverPort();
    // "localhost" (any case) is not something QHostAddress parses; it must mean the loopback
    // address, not the null address that would bind every interface.
    SshLocalForward viaLocalhost = forward;
    viaLocalhost.localPort = localhostPort;
    viaLocalhost.bindAddress = QStringLiteral("LocalHost");
    // Any other name is refused before listening (same port: the refusal comes first).
    SshLocalForward badBind = forward;
    badBind.localPort = localhostPort;
    badBind.bindAddress = QStringLiteral("nowhere.invalid");
    p.localForwards = {forward, taken, viaLocalhost, badBind};
    TestClient c(p);
    QSignalSpy listening(&c.conn, &SshConnection::forwardListening);
    QSignalSpy failed(&c.conn, &SshConnection::forwardFailed);
    E2E_CONNECT(c);
    QTRY_COMPARE_WITH_TIMEOUT(listening.count(), 2, kE2eTimeoutMs);
    QCOMPARE(listening.at(0).at(0).value<quint16>(), localPort);
    QCOMPARE(listening.at(0).at(1).toString(), QStringLiteral("127.0.0.1"));
    QCOMPARE(listening.at(0).at(2).value<quint16>(), echo.serverPort());
    QCOMPARE(listening.at(1).at(0).value<quint16>(), localhostPort);
    QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 2, kE2eTimeoutMs);
    QCOMPARE(failed.at(0).at(0).value<quint16>(), busy.serverPort());
    QVERIFY(!failed.at(0).at(1).toString().isEmpty());
    QCOMPARE(failed.at(1).at(0).value<quint16>(), localhostPort);
    QVERIFY2(failed.at(1).at(1).toString().contains(QStringLiteral("bind address")), qPrintable(failed.at(1).at(1).toString()));
    QVERIFY(failed.at(1).at(1).toString().contains(QStringLiteral("nowhere.invalid")));

    // The "localhost" forward answers on the loopback address.
    {
        QTcpSocket viaLoopback;
        QByteArray back;
        connect(&viaLoopback, &QTcpSocket::readyRead, &viaLoopback, [&viaLoopback, &back] { back += viaLoopback.readAll(); });
        viaLoopback.connectToHost(QHostAddress::LocalHost, localhostPort);
        QTRY_COMPARE_WITH_TIMEOUT(viaLoopback.state(), QAbstractSocket::ConnectedState, kE2eTimeoutMs);
        viaLoopback.write(QByteArrayLiteral("via-localhost"));
        QTRY_COMPARE_WITH_TIMEOUT(back, QByteArrayLiteral("via-localhost"), kE2eTimeoutMs);
        viaLoopback.disconnectFromHost();
    }
    QTRY_COMPARE_WITH_TIMEOUT(spy.forwards.size(), 1, kE2eTimeoutMs);
    QTRY_COMPARE_WITH_TIMEOUT(echo.accepted, 1, kE2eTimeoutMs);

    // 100 KiB through the forward reach the echo server and come back intact.
    QTcpSocket socket;
    QByteArray echoed;
    connect(&socket, &QTcpSocket::readyRead, &socket, [&socket, &echoed] { echoed += socket.readAll(); });
    socket.connectToHost(QHostAddress::LocalHost, localPort);
    QTRY_COMPARE_WITH_TIMEOUT(socket.state(), QAbstractSocket::ConnectedState, kE2eTimeoutMs);
    QTRY_COMPARE_WITH_TIMEOUT(spy.forwards.size(), 2, kE2eTimeoutMs);
    QCOMPARE(spy.forwards.at(1).first, QStringLiteral("127.0.0.1"));
    QCOMPARE(spy.forwards.at(1).second, echo.serverPort());
    QTRY_COMPARE_WITH_TIMEOUT(echo.accepted, 2, kE2eTimeoutMs);
    const QByteArray payload = randomBytes(100 * 1024);
    QCOMPARE(socket.write(payload), qint64(payload.size()));
    QTRY_COMPARE_WITH_TIMEOUT(echoed.size(), payload.size(), 30000);
    QVERIFY(echoed == payload);

    // The shell still works next to the forward.
    c.conn.write(QByteArrayLiteral("echo WITH-FORWARD\r"));
    QTRY_VERIFY_WITH_TIMEOUT(c.received.contains("WITH-FORWARD\r\n$ "), kE2eTimeoutMs);

    socket.disconnectFromHost();
    if (socket.state() != QAbstractSocket::UnconnectedState) {
        QTRY_COMPARE_WITH_TIMEOUT(socket.state(), QAbstractSocket::UnconnectedState, kE2eTimeoutMs);
    }
    c.conn.close();
    QCOMPARE(c.conn.state(), State::Disconnected);
    // The listener is gone with the session.
    QTcpSocket probe;
    probe.connectToHost(QHostAddress::LocalHost, localPort);
    QTRY_VERIFY_WITH_TIMEOUT(probe.state() == QAbstractSocket::UnconnectedState, kE2eTimeoutMs);
    QVERIFY(probe.error() != QAbstractSocket::UnknownSocketError);
}

void Tst_sshconnection::e2eLocalForwardClientClosesFirst()
{
    // Bridge lifetimes. (1) The local client ends its bridged connection first (an HTTP client
    // finishing) while the session and its 10 ms tick keep running: the worker used to arm
    // deleteLater() on the socket's disconnected() while its Bridge still held the raw pointer,
    // so the next tick and the teardown in close() dereferenced a deleted QTcpSocket - a
    // use-after-free that segfaulted on Linux. (2) The remote side closes first: the channel's
    // EOF has to end the local connection. (3) A client that resets instead of closing. In
    // every case the listener stays and the next client is bridged again.
    LocalEchoServer echo;
    QVERIFY(echo.listen(QHostAddress::LocalHost, 0));
    const quint16 localPort = closedLocalPort();
    QVERIFY(localPort != 0);
    TestSshServer server(serverOptions(QStringLiteral("forward-close")));
    QVERIFY(server.start());
    ServerSpy spy(&server);
    SshProfile p = serverProfile(server, QStringLiteral("forward-close"));
    SshLocalForward forward;
    forward.localPort = localPort;
    forward.remoteHost = QStringLiteral("127.0.0.1");
    forward.remotePort = echo.serverPort();
    p.localForwards = {forward};
    TestClient c(p);
    QSignalSpy listening(&c.conn, &SshConnection::forwardListening);
    E2E_CONNECT(c);
    QTRY_COMPARE_WITH_TIMEOUT(listening.count(), 1, kE2eTimeoutMs);

    // Connect through the forward and get `payload` echoed back (void: QTRY_* inside).
    auto roundTrip = [&](QTcpSocket& socket, const QByteArray& payload) {
        QByteArray echoed;
        connect(&socket, &QTcpSocket::readyRead, &socket, [&socket, &echoed] { echoed += socket.readAll(); });
        socket.connectToHost(QHostAddress::LocalHost, localPort);
        QTRY_COMPARE_WITH_TIMEOUT(socket.state(), QAbstractSocket::ConnectedState, kE2eTimeoutMs);
        QCOMPARE(socket.write(payload), qint64(payload.size()));
        QTRY_COMPARE_WITH_TIMEOUT(echoed.size(), payload.size(), kE2eTimeoutMs);
        QVERIFY(echoed == payload);
    };

    // (1) The local client closes first; the tick keeps running over the finished bridge.
    {
        QTcpSocket first;
        roundTrip(first, letterBurst(8 * 1024));
        if (QTest::currentTestFailed()) {
            return;
        }
        first.disconnectFromHost();
        if (first.state() != QAbstractSocket::UnconnectedState) {
            QTRY_COMPARE_WITH_TIMEOUT(first.state(), QAbstractSocket::UnconnectedState, kE2eTimeoutMs);
        }
    }
    QTRY_COMPARE_WITH_TIMEOUT(echo.accepted, 1, kE2eTimeoutMs);
    QTest::qWait(600);   // about 60 ticks after the local close
    c.conn.write(QByteArrayLiteral("echo STILL-UP\r"));
    QTRY_VERIFY2_WITH_TIMEOUT(c.received.contains("STILL-UP\r\n$ "), c.received.constData(), kE2eTimeoutMs);
    QCOMPARE(c.conn.state(), State::Connected);

    // (2) The next client is bridged again; then the echo server ends the connection.
    {
        QTcpSocket second;
        roundTrip(second, letterBurst(8 * 1024));
        if (QTest::currentTestFailed()) {
            return;
        }
        QTRY_COMPARE_WITH_TIMEOUT(spy.forwards.size(), 2, kE2eTimeoutMs);
        QTRY_COMPARE_WITH_TIMEOUT(echo.accepted, 2, kE2eTimeoutMs);
        echo.closeClients();
        QTRY_COMPARE_WITH_TIMEOUT(second.state(), QAbstractSocket::UnconnectedState, kE2eTimeoutMs);
    }
    QTest::qWait(300);
    c.conn.write(QByteArrayLiteral("echo AFTER-REMOTE-CLOSE\r"));
    QTRY_VERIFY2_WITH_TIMEOUT(c.received.contains("AFTER-REMOTE-CLOSE\r\n$ "), c.received.constData(), kE2eTimeoutMs);

    // (3) A third client resets the connection (RST) instead of closing it.
    {
        QTcpSocket third;
        roundTrip(third, QByteArrayLiteral("reset-me"));
        if (QTest::currentTestFailed()) {
            return;
        }
        third.abort();
    }
    QTRY_COMPARE_WITH_TIMEOUT(spy.forwards.size(), 3, kE2eTimeoutMs);
    QTest::qWait(300);
    c.conn.write(QByteArrayLiteral("echo AFTER-RESET\r"));
    QTRY_VERIFY2_WITH_TIMEOUT(c.received.contains("AFTER-RESET\r\n$ "), c.received.constData(), kE2eTimeoutMs);
    QVERIFY2(c.errors.isEmpty(), qPrintable(c.errors.join(QStringLiteral(" | "))));
    QCOMPARE(c.lost, 0);

    // close() tears the listener down with every bridge already gone.
    c.conn.close();
    QCOMPARE(c.conn.state(), State::Disconnected);
    QTRY_COMPARE_WITH_TIMEOUT(server.activeConnections(), 0, kE2eTimeoutMs);
    QTcpSocket probe;
    probe.connectToHost(QHostAddress::LocalHost, localPort);
    QTRY_VERIFY_WITH_TIMEOUT(probe.state() == QAbstractSocket::UnconnectedState, kE2eTimeoutMs);
}

void Tst_sshconnection::e2eSftpTransfers()
{
    const TestSshServer::Options options = serverOptions(QStringLiteral("sftp"));
    TestSshServer server(options);
    QVERIFY(server.start());
    ServerSpy spy(&server);
    TestClient c(serverProfile(server, QStringLiteral("sftp")));
    E2E_CONNECT(c);
    QSignalSpy started(&c.conn, &SshConnection::transferStarted);
    QSignalSpy progress(&c.conn, &SshConnection::transferProgress);
    QSignalSpy finished(&c.conn, &SshConnection::transferFinished);

    const QByteArray payload = randomBytes(2 * 1024 * 1024);
    const QString upPath = m_dir.filePath(QStringLiteral("sftp/local/up.bin"));
    const QString downPath = m_dir.filePath(QStringLiteral("sftp/local/down.bin"));
    QVERIFY(writeFile(upPath, payload));
    const QString remoteName = QStringLiteral("up.bin");   // relative: lands under Options::rootDir
    const QString remoteFile = QDir(options.rootDir).filePath(remoteName);

    // Upload 2 MiB with monotonic progress ending at the total.
    SshConnection::TransferRequest upload;
    upload.direction = SshConnection::TransferDirection::Upload;
    upload.localPath = upPath;
    upload.remotePath = remoteName;
    QVERIFY(c.conn.startTransfer(upload));
    QVERIFY(c.conn.isTransferActive());
    QVERIFY(!c.conn.startTransfer(upload));   // busy
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 60000);
    QVERIFY2(finished.at(0).at(0).toBool(), qPrintable(finished.at(0).at(1).toString()));
    QVERIFY(!c.conn.isTransferActive());
    QCOMPARE(c.conn.transferStatus().method, QStringLiteral("sftp"));
    QVERIFY2(finished.at(0).at(1).toString().endsWith(QStringLiteral(", SFTP)")), qPrintable(finished.at(0).at(1).toString()));
    QCOMPARE(started.count(), 1);
    QCOMPARE(started.at(0).at(0).value<SshConnection::TransferRequest>().remotePath, remoteName);
    QVERIFY(progress.count() >= 2);
    qint64 previous = -1;
    for (const QList<QVariant>& args : progress) {
        const qint64 done = args.at(0).toLongLong();
        QVERIFY2(done >= previous, qPrintable(QStringLiteral("%1 < %2").arg(done).arg(previous)));
        QCOMPARE(args.at(1).toLongLong(), qint64(payload.size()));
        previous = done;
    }
    QCOMPARE(previous, qint64(payload.size()));
    QCOMPARE(c.conn.transferStatus().done, qint64(payload.size()));
    QCOMPARE(readAll(remoteFile), payload);
    QTRY_COMPARE_WITH_TIMEOUT(spy.sftp, 1, kE2eTimeoutMs);

    // overwrite = false refuses an existing remote file.
    upload.overwrite = false;
    QVERIFY(c.conn.startTransfer(upload));
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 2, kE2eTimeoutMs);
    QVERIFY(!finished.at(1).at(0).toBool());
    QVERIFY2(finished.at(1).at(1).toString().contains(QStringLiteral("exists")), qPrintable(finished.at(1).at(1).toString()));
    QCOMPARE(readAll(remoteFile), payload);

    // Download it back byte-identical, no .part left behind.
    SshConnection::TransferRequest download;
    download.direction = SshConnection::TransferDirection::Download;
    download.localPath = downPath;
    download.remotePath = remoteName;
    progress.clear();
    QVERIFY(c.conn.startTransfer(download));
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 3, 60000);
    QVERIFY2(finished.at(2).at(0).toBool(), qPrintable(finished.at(2).at(1).toString()));
    QVERIFY2(finished.at(2).at(1).toString().endsWith(QStringLiteral(", SFTP)")), qPrintable(finished.at(2).at(1).toString()));
    QCOMPARE(c.conn.transferStatus().method, QStringLiteral("sftp"));
    QVERIFY(QFileInfo::exists(downPath));
    QVERIFY(!QFileInfo::exists(downPath + QStringLiteral(".part")));
    QCOMPARE(readAll(downPath), payload);
    QVERIFY(progress.count() >= 2);
    QCOMPARE(progress.last().at(0).toLongLong(), qint64(payload.size()));
    QCOMPARE(progress.last().at(1).toLongLong(), qint64(payload.size()));

    // overwrite = false refuses an existing local file.
    download.overwrite = false;
    QVERIFY(c.conn.startTransfer(download));
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 4, kE2eTimeoutMs);
    QVERIFY(!finished.at(3).at(0).toBool());
    QVERIFY2(finished.at(3).at(1).toString().contains(QStringLiteral("exists")), qPrintable(finished.at(3).at(1).toString()));

    // A missing remote file fails cleanly.
    download.remotePath = QStringLiteral("does-not-exist.bin");
    download.localPath = m_dir.filePath(QStringLiteral("sftp/local/missing.bin"));
    QVERIFY(c.conn.startTransfer(download));
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 5, kE2eTimeoutMs);
    QVERIFY(!finished.at(4).at(0).toBool());
    QVERIFY(!QFileInfo::exists(download.localPath));
    QVERIFY(!QFileInfo::exists(download.localPath + QStringLiteral(".part")));

    // cancelTransfer(): an upload and a download cancelled right after the start; the
    // download leaves no .part file.
    SshConnection::TransferRequest cancelledUpload = upload;
    cancelledUpload.overwrite = true;
    cancelledUpload.remotePath = QStringLiteral("cancelled-up.bin");
    QVERIFY(c.conn.startTransfer(cancelledUpload));
    c.conn.cancelTransfer();
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 6, kE2eTimeoutMs);
    QVERIFY(!finished.at(5).at(0).toBool());
    QVERIFY2(finished.at(5).at(1).toString().contains(QStringLiteral("cancel"), Qt::CaseInsensitive),
             qPrintable(finished.at(5).at(1).toString()));
    QVERIFY(!c.conn.isTransferActive());
    // v0.4: the truncated remote file of a cancelled SFTP upload is removed (unlinked once the
    // handle is closed, before transferFinished), like the .part of a cancelled download.
    QVERIFY(!QFileInfo::exists(QDir(options.rootDir).filePath(QStringLiteral("cancelled-up.bin"))));
    SshConnection::TransferRequest cancelledDownload;
    cancelledDownload.direction = SshConnection::TransferDirection::Download;
    cancelledDownload.localPath = m_dir.filePath(QStringLiteral("sftp/local/cancelled.bin"));
    cancelledDownload.remotePath = remoteName;
    QVERIFY(c.conn.startTransfer(cancelledDownload));
    c.conn.cancelTransfer();
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 7, kE2eTimeoutMs);
    QVERIFY(!finished.at(6).at(0).toBool());
    QVERIFY(!QFileInfo::exists(cancelledDownload.localPath));
    QVERIFY(!QFileInfo::exists(cancelledDownload.localPath + QStringLiteral(".part")));

    // A transfer while the shell keeps echoing.
    c.received.clear();
    SshConnection::TransferRequest during = upload;
    during.overwrite = true;
    during.remotePath = QStringLiteral("during.bin");
    QVERIFY(c.conn.startTransfer(during));
    c.conn.write(QByteArrayLiteral("echo DURING\r"));
    QTRY_VERIFY2_WITH_TIMEOUT(c.received.contains("DURING\r\n$ "), c.received.constData(), kE2eTimeoutMs);
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 8, 60000);
    QVERIFY2(finished.at(7).at(0).toBool(), qPrintable(finished.at(7).at(1).toString()));
    QCOMPARE(readAll(QDir(options.rootDir).filePath(QStringLiteral("during.bin"))), payload);
    c.received.clear();
    c.conn.write(QByteArrayLiteral("echo AFTER\r"));
    QTRY_VERIFY_WITH_TIMEOUT(c.received.contains("AFTER\r\n$ "), kE2eTimeoutMs);
    QCOMPARE(spy.sftp, 1);   // one SFTP session serves every transfer
    c.conn.close();
    QVERIFY(!c.conn.isTransferActive());
}

void Tst_sshconnection::e2eSftpPipelineSizes()
{
    // v0.4: SFTP keeps 16 requests in flight. Sizes on and around the request boundaries (libssh
    // caps one request at 32 KiB against a server without the limits extension, the worker's
    // chunk is 64 KiB, the pipeline 1 MiB), an empty file, a single byte and a prime tail go up
    // and come back byte for byte: the short-read reply of the last block drops the requests
    // queued behind it, EOF ends the download once every reply is collected, and the progress
    // ends at the total in both directions.
    const TestSshServer::Options options = serverOptions(QStringLiteral("sizes"));
    TestSshServer server(options);
    QVERIFY(server.start());
    TestClient c(serverProfile(server, QStringLiteral("sizes")));
    E2E_CONNECT(c);
    QSignalSpy finished(&c.conn, &SshConnection::transferFinished);
    QSignalSpy progress(&c.conn, &SshConnection::transferProgress);
    int expected = 0;
    const qsizetype sizes[] = {0, 1, 32 * 1024, 32 * 1024 + 1, 64 * 1024 - 1, 64 * 1024, 64 * 1024 + 1,
                               16 * 64 * 1024 + 3, 3 * 1024 * 1024 + 7919};
    for (const qsizetype size : sizes) {
        const QByteArray payload = randomBytes(size);
        const QString name = QStringLiteral("size-%1.bin").arg(size);
        const QString upPath = m_dir.filePath(QStringLiteral("sizes/local/up-") + name);
        const QString downPath = m_dir.filePath(QStringLiteral("sizes/local/down-") + name);
        QVERIFY(writeFile(upPath, payload));

        SshConnection::TransferRequest upload;
        upload.direction = SshConnection::TransferDirection::Upload;
        upload.localPath = upPath;
        upload.remotePath = name;
        progress.clear();
        QVERIFY2(c.conn.startTransfer(upload), qPrintable(name));
        ++expected;   // outside the macro: QTRY_* re-evaluates its arguments on every poll
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), expected, 60000);
        QVERIFY2(finished.last().at(0).toBool(), qPrintable(name + QStringLiteral(": ") + finished.last().at(1).toString()));
        QCOMPARE(c.conn.transferStatus().method, QStringLiteral("sftp"));
        QCOMPARE(c.conn.transferStatus().done, qint64(size));
        QVERIFY(!progress.isEmpty());
        QCOMPARE(progress.last().at(0).toLongLong(), qint64(size));
        QCOMPARE(readAll(QDir(options.rootDir).filePath(name)), payload);

        SshConnection::TransferRequest download;
        download.direction = SshConnection::TransferDirection::Download;
        download.localPath = downPath;
        download.remotePath = name;
        progress.clear();
        QVERIFY2(c.conn.startTransfer(download), qPrintable(name));
        ++expected;   // outside the macro: QTRY_* re-evaluates its arguments on every poll
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), expected, 60000);
        QVERIFY2(finished.last().at(0).toBool(), qPrintable(name + QStringLiteral(": ") + finished.last().at(1).toString()));
        QCOMPARE(c.conn.transferStatus().done, qint64(size));
        QVERIFY(!progress.isEmpty());
        QCOMPARE(progress.last().at(0).toLongLong(), qint64(size));
        QCOMPARE(progress.last().at(1).toLongLong(), qint64(size));
        QVERIFY(!QFileInfo::exists(downPath + QStringLiteral(".part")));
        QCOMPARE(readAll(downPath), payload);
    }

    // The shell answered nothing in between and still works after 18 pipelined transfers.
    c.received.clear();
    c.conn.write(QByteArrayLiteral("echo SIZES\r"));
    QTRY_VERIFY2_WITH_TIMEOUT(c.received.contains("SIZES\r\n$ "), c.received.constData(), kE2eTimeoutMs);
    QVERIFY2(c.errors.isEmpty(), qPrintable(c.errors.join(QStringLiteral(" | "))));
    c.conn.close();
}

void Tst_sshconnection::e2eRemoteHome()
{
    const TestSshServer::Options options = serverOptions(QStringLiteral("home"));
    TestSshServer server(options);
    QVERIFY(server.start());
    TestClient c(serverProfile(server, QStringLiteral("home")));
    E2E_CONNECT(c);
    QSignalSpy home(&c.conn, &SshConnection::remoteHomeReceived);
    QVERIFY(c.conn.remoteHome().isEmpty());
    c.conn.requestRemoteHome();
    QTRY_COMPARE_WITH_TIMEOUT(home.count(), 1, kE2eTimeoutMs);
    const QString path = home.at(0).at(0).toString();
    QVERIFY2(!path.isEmpty(), "remote home is empty");
    QCOMPARE(QFileInfo(path).canonicalFilePath(), QFileInfo(options.rootDir).canonicalFilePath());
    QCOMPARE(c.conn.remoteHome(), path);
    QVERIFY2(c.errors.isEmpty(), qPrintable(c.errors.join(QStringLiteral(" | "))));
    c.conn.close();
    QCOMPARE(c.conn.remoteHome(), path);   // the fact of the last connection stays readable
}

void Tst_sshconnection::e2eCloseDuringHostKeyQuestion()
{
    TestSshServer server(serverOptions(QStringLiteral("closehk")));
    QVERIFY(server.start());
    const SshProfile p = serverProfile(server, QStringLiteral("closehk"));
    TestClient c(p);
    c.autoAnswer = false;
    QVERIFY(c.conn.open());
    QTRY_COMPARE_WITH_TIMEOUT(c.hostKeys.size(), 1, kE2eTimeoutMs);
    QCOMPARE(c.conn.state(), State::Connecting);
    QElapsedTimer timer;
    timer.start();
    c.conn.close();
    QVERIFY(timer.elapsed() < 1000);
    QCOMPARE(c.conn.state(), State::Disconnected);
    QCOMPARE(c.states, QList<State>({State::Connecting, State::Disconnected}));
    QTest::qWait(500);   // nothing arrives late
    QVERIFY2(c.errors.isEmpty(), qPrintable(c.errors.join(QStringLiteral(" | "))));
    QCOMPARE(c.states.size(), 2);
    QCOMPARE(c.prompts.size(), 0);
    QVERIFY(!QFileInfo::exists(p.knownHostsFile));
    QTRY_COMPARE_WITH_TIMEOUT(server.activeConnections(), 0, kE2eTimeoutMs);
    c.conn.answerHostKey(true, true);   // a late answer is ignored
    QTest::qWait(100);
    QCOMPARE(c.conn.state(), State::Disconnected);

    // Still usable afterwards.
    c.autoAnswer = true;
    E2E_CONNECT(c);
    QCOMPARE(c.hostKeys.size(), 2);
    c.conn.close();
}

void Tst_sshconnection::e2eCloseDuringAuthPrompt()
{
    TestSshServer server(serverOptions(QStringLiteral("closeauth")));
    QVERIFY(server.start());
    const SshProfile p = serverProfile(server, QStringLiteral("closeauth"));
    TestClient c(p);
    connect(&c.conn, &SshConnection::hostKeyVerificationRequired, &c.conn, [&c] { c.conn.answerHostKey(true, true); });
    c.autoAnswer = false;
    QVERIFY(c.conn.open());
    QTRY_COMPARE_WITH_TIMEOUT(c.prompts.size(), 1, kE2eTimeoutMs);
    QCOMPARE(c.conn.state(), State::Connecting);
    QElapsedTimer timer;
    timer.start();
    c.conn.close();
    QVERIFY(timer.elapsed() < 1000);
    QCOMPARE(c.conn.state(), State::Disconnected);
    QTest::qWait(500);
    QVERIFY2(c.errors.isEmpty(), qPrintable(c.errors.join(QStringLiteral(" | "))));
    QCOMPARE(c.states, QList<State>({State::Connecting, State::Disconnected}));
    QCOMPARE(c.methods.size(), 0);
    QTRY_COMPARE_WITH_TIMEOUT(server.activeConnections(), 0, kE2eTimeoutMs);
    QVERIFY(server.lastAuthMethod().isEmpty());
    c.conn.answerPrompt(kServerPassword, false);   // late answers are ignored
    c.conn.cancelPrompt();
    QTest::qWait(100);
    QCOMPARE(c.conn.state(), State::Disconnected);
}

void Tst_sshconnection::e2eCloseWhileConnecting()
{
    TestSshServer server(serverOptions(QStringLiteral("closeconn")));
    QVERIFY(server.start());
    const SshProfile p = serverProfile(server, QStringLiteral("closeconn"));
    TestClient c(p);
    QVERIFY(c.conn.open());
    QCOMPARE(c.conn.state(), State::Connecting);
    c.conn.close();   // right away: the worker may be anywhere in TCP / key exchange
    QCOMPARE(c.conn.state(), State::Disconnected);
    QCOMPARE(c.states, QList<State>({State::Connecting, State::Disconnected}));
    QTest::qWait(1000);
    QCOMPARE(c.states.size(), 2);
    QVERIFY2(c.errors.isEmpty(), qPrintable(c.errors.join(QStringLiteral(" | "))));
    QCOMPARE(c.shells, 0);
    QTRY_COMPARE_WITH_TIMEOUT(server.activeConnections(), 0, kE2eTimeoutMs);

    // open() -> close() -> open() in a row ends connected exactly once.
    QVERIFY(c.conn.open());
    c.conn.close();
    QVERIFY(c.conn.open());
    QTRY_COMPARE_WITH_TIMEOUT(c.conn.state(), State::Connected, kE2eTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(c.received.contains("welcome\r\n$ "), kE2eTimeoutMs);
    QCOMPARE(c.shells, 1);
    QCOMPARE(c.received.count("welcome"), 1);
    c.conn.close();
    QTRY_COMPARE_WITH_TIMEOUT(server.activeConnections(), 0, kE2eTimeoutMs);
}

void Tst_sshconnection::e2eDestroyWhileConnected()
{
    TestSshServer server(serverOptions(QStringLiteral("destroy")));
    QVERIFY(server.start());
    ServerSpy spy(&server);
    // Declared before the clients so they outlive them: a destructor that still emitted would
    // write here, not into freed memory (which glibc reports as bad_alloc / abort).
    int lateSignals = 0;
    auto c = std::make_unique<TestClient>(serverProfile(server, QStringLiteral("destroy")));
    E2E_CONNECT(*c);
    c->conn.write(QByteArrayLiteral("echo BEFORE\r"));
    QTRY_VERIFY_WITH_TIMEOUT(c->received.contains("BEFORE\r\n$ "), kE2eTimeoutMs);
    QPointer<QThread> workerThread = c->conn.findChild<QThread*>(QStringLiteral("ssh-worker"));
    QVERIFY(workerThread);
    QVERIFY(workerThread->isRunning());
    connect(&c->conn, &Transport::stateChanged, this, [&lateSignals](Transport::State) { ++lateSignals; });
    connect(&c->conn, &Transport::errorOccurred, this, [&lateSignals](const QString&) { ++lateSignals; });

    // Delete while connected (with a transfer in flight): the worker thread is joined
    // promptly, the server sees a disconnect, nothing crashes and the destructor emits nothing
    // (the connection's own recorders are members declared before it, so they are alive).
    const QString upPath = m_dir.filePath(QStringLiteral("destroy/up.bin"));
    QVERIFY(writeFile(upPath, randomBytes(512 * 1024)));
    SshConnection::TransferRequest upload;
    upload.direction = SshConnection::TransferDirection::Upload;
    upload.localPath = upPath;
    upload.remotePath = QStringLiteral("up.bin");
    QVERIFY(c->conn.startTransfer(upload));
    QElapsedTimer timer;
    timer.start();
    c.reset();
    QVERIFY2(timer.elapsed() < 5000, qPrintable(QString::number(timer.elapsed())));
    QVERIFY(workerThread.isNull());   // joined and deleted by the destructor
    QCOMPARE(lateSignals, 0);
    QTRY_COMPARE_WITH_TIMEOUT(server.activeConnections(), 0, kE2eTimeoutMs);
    QTRY_COMPARE_WITH_TIMEOUT(spy.disconnected, 1, kE2eTimeoutMs);
    QCOMPARE(server.connectionCount(), 1);

    // Destroyed while Reconnecting (the server refuses): the pending reconnect is dropped,
    // again without a final stateChanged() from the destructor.
    auto d = std::make_unique<TestClient>(serverProfile(server, QStringLiteral("destroy")));
    E2E_CONNECT(*d);
    server.setAcceptConnections(false);
    server.dropAllClients();
    QTRY_COMPARE_WITH_TIMEOUT(d->conn.state(), State::Reconnecting, 5000);
    workerThread = d->conn.findChild<QThread*>(QStringLiteral("ssh-worker"));
    QVERIFY(workerThread);
    connect(&d->conn, &Transport::stateChanged, this, [&lateSignals](Transport::State) { ++lateSignals; });
    timer.restart();
    d.reset();
    QVERIFY2(timer.elapsed() < 5000, qPrintable(QString::number(timer.elapsed())));
    QVERIFY(workerThread.isNull());
    QCOMPARE(lateSignals, 0);
    server.setAcceptConnections(true);
    QTest::qWait(2500);
    QCOMPARE(server.connectionCount(), 2);
}

void Tst_sshconnection::e2eTwoConnectionsInParallel()
{
    TestSshServer server(serverOptions(QStringLiteral("parallel")));
    QVERIFY(server.start());
    SshProfile pa = serverProfile(server, QStringLiteral("parallel/a"));
    SshProfile pb = serverProfile(server, QStringLiteral("parallel/b"));
    pb.terminalType = QStringLiteral("vt100");
    TestClient a(pa);
    TestClient b(pb);
    QVERIFY(a.conn.open());
    QVERIFY(b.conn.open());
    QTRY_COMPARE_WITH_TIMEOUT(a.conn.state(), State::Connected, kE2eTimeoutMs);
    QTRY_COMPARE_WITH_TIMEOUT(b.conn.state(), State::Connected, kE2eTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(a.received.contains("welcome\r\n$ "), kE2eTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(b.received.contains("welcome\r\n$ "), kE2eTimeoutMs);
    QCOMPARE(server.activeConnections(), 2);
    QCOMPARE(server.connectionCount(), 2);
    QCOMPARE(a.hostKeys.size(), 1);
    QCOMPARE(b.hostKeys.size(), 1);

    a.conn.write(QByteArrayLiteral("echo FROM-A\r"));
    b.conn.write(QByteArrayLiteral("echo FROM-B\r"));
    QTRY_VERIFY_WITH_TIMEOUT(a.received.contains("FROM-A\r\n$ "), kE2eTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(b.received.contains("FROM-B\r\n$ "), kE2eTimeoutMs);
    QVERIFY(!a.received.contains("FROM-B"));
    QVERIFY(!b.received.contains("FROM-A"));

    // One ends cleanly, the other keeps working.
    a.conn.write(QByteArrayLiteral("exit 3\r"));
    QTRY_COMPARE_WITH_TIMEOUT(a.conn.state(), State::Disconnected, kE2eTimeoutMs);
    QCOMPARE(a.closed, QList<int>{3});
    QCOMPARE(b.conn.state(), State::Connected);
    b.conn.write(QByteArrayLiteral("echo STILL-B\r"));
    QTRY_VERIFY_WITH_TIMEOUT(b.received.contains("STILL-B\r\n$ "), kE2eTimeoutMs);
    QCOMPARE(b.lost, 0);
    b.conn.close();
    QTRY_COMPARE_WITH_TIMEOUT(server.activeConnections(), 0, kE2eTimeoutMs);
    QVERIFY(a.errors.isEmpty());
    QVERIFY(b.errors.isEmpty());
}

// ---------------------------------------------------------------------------------------
// (b) live probe
// ---------------------------------------------------------------------------------------

void Tst_sshconnection::e2eAdHocRememberPassword()
{
    TestSshServer server(serverOptions(QStringLiteral("adhoc")));
    QVERIFY(server.start());
    const SshProfile p = serverProfile(server, QStringLiteral("adhoc"));
    QVERIFY(p.id.isEmpty());
    const QString key = QStringLiteral("ssh/target/%1/password").arg(p.displayTarget());
    QVERIFY(!SecretStore::contains(key));

    // The prompt of an ad-hoc target can be remembered for that target; the answer given with
    // "remember" is stored under it once it worked.
    TestClient c(p);
    c.rememberResponse = true;
    E2E_CONNECT(c);
    QCOMPARE(c.prompts.size(), 1);
    QVERIFY(c.prompts.first().canRemember);
    QCOMPARE(c.prompts.first().rememberTarget, p.displayTarget());
    QTRY_COMPARE_WITH_TIMEOUT(SecretStore::load(key).value_or(QString()), kServerPassword, kE2eTimeoutMs);
    c.conn.close();

    // A NEW connection to the same ad-hoc target connects without any prompt.
    TestClient d(p);
    E2E_CONNECT(d);
    QCOMPARE(d.prompts.size(), 0);
    QCOMPARE(d.conn.authMethod(), QStringLiteral("password"));
    d.conn.close();

    // A stale stored password is removed before the prompt (attempt 2); answered without
    // "remember", nothing is written back.
    QVERIFY(SecretStore::store(key, QStringLiteral("stale-password")));
    TestClient e(p);
    bool storeHadKeyAtPrompt = true;
    connect(&e.conn, &SshConnection::authPromptRequired, &e.conn,
            [&storeHadKeyAtPrompt, key](const SshConnection::AuthPrompt&) { storeHadKeyAtPrompt = SecretStore::contains(key); });
    E2E_CONNECT(e);
    QCOMPARE(e.prompts.size(), 1);
    QCOMPARE(e.prompts.first().kind, SshConnection::PromptKind::Password);
    QCOMPARE(e.prompts.first().attempt, 2);
    QVERIFY(!storeHadKeyAtPrompt);
    QTest::qWait(100);
    QVERIFY(!SecretStore::contains(key));
    e.conn.close();

    // A stored profile for the same target: its own entry wins over the target entry, which
    // is not even tried (so it stays as it is).
    QVERIFY(SecretStore::store(key, QStringLiteral("stale-password")));
    SshProfile stored = p;
    stored.id = QStringLiteral("e2e-adhoc-profile");
    const QString profileKey = QStringLiteral("ssh/%1/password").arg(stored.id);
    QVERIFY(SecretStore::store(profileKey, kServerPassword));
    TestClient f(stored);
    E2E_CONNECT(f);
    QCOMPARE(f.prompts.size(), 0);
    QCOMPARE(SecretStore::load(key).value_or(QString()), QStringLiteral("stale-password"));
    f.conn.close();

    // A profile without its own entry falls back to the target entry.
    QVERIFY(SecretStore::remove(profileKey));
    QVERIFY(SecretStore::store(key, kServerPassword));
    TestClient g(stored);
    E2E_CONNECT(g);
    QCOMPARE(g.prompts.size(), 0);
    g.conn.close();

    // A remembered answer of a profile goes under its id, not under the target.
    QVERIFY(SecretStore::remove(key));
    TestClient h(stored);
    h.rememberResponse = true;
    E2E_CONNECT(h);
    QCOMPARE(h.prompts.size(), 1);
    QVERIFY(h.prompts.first().canRemember);
    QCOMPARE(h.prompts.first().rememberTarget, p.displayTarget());
    QTRY_COMPARE_WITH_TIMEOUT(SecretStore::load(profileKey).value_or(QString()), kServerPassword, kE2eTimeoutMs);
    QVERIFY(!SecretStore::contains(key));
    h.conn.close();
    QVERIFY(SecretStore::remove(profileKey));
}

void Tst_sshconnection::e2ePassphraseRememberedPerKey()
{
    const QString keyPath = m_dir.filePath(QStringLiteral("keyremember/id_enc"));
    QDir().mkpath(QFileInfo(keyPath).absolutePath());
    const QString passphrase = QStringLiteral("open-sesame");
    QString publicLine;
    QVERIFY(TestSshServer::generateClientKeyPair(keyPath, &publicLine, passphrase));

    TestSshServer::Options options = serverOptions(QStringLiteral("keyremember"));
    options.authorizedPublicKey = publicLine;
    options.allowPassword = false;
    TestSshServer first(options);
    QVERIFY(first.start());
    TestSshServer second(options);   // another target (port) that accepts the same key
    QVERIFY(second.start());
    QVERIFY(first.port() != second.port());

    const QString normalized = QDir::cleanPath(QFileInfo(keyPath).absoluteFilePath());
    const QString keySecret = QStringLiteral("ssh/key/%1/passphrase").arg(normalized);
    QVERIFY(!SecretStore::contains(keySecret));

    // Ad-hoc target one: the passphrase prompt is rememberable for the key file; "remember"
    // stores it under the key path.
    SshProfile p = serverProfile(first, QStringLiteral("keyremember"));
    p.auth = SshProfile::Auth::PublicKey;
    p.identityFile = keyPath;
    TestClient c(p);
    c.responses = {passphrase};
    c.rememberResponse = true;
    E2E_CONNECT(c);
    QCOMPARE(c.prompts.size(), 1);
    QCOMPARE(c.prompts.first().kind, SshConnection::PromptKind::Passphrase);
    QVERIFY(c.prompts.first().canRemember);
    QCOMPARE(c.prompts.first().rememberTarget, normalized);
    QCOMPARE(c.prompts.first().keyFile, keyPath);
    QTRY_COMPARE_WITH_TIMEOUT(SecretStore::load(keySecret).value_or(QString()), passphrase, kE2eTimeoutMs);
    c.conn.close();

    // Ad-hoc target two with the same key: no prompt at all.
    SshProfile q = serverProfile(second, QStringLiteral("keyremember2"));
    q.auth = SshProfile::Auth::PublicKey;
    q.identityFile = keyPath;
    TestClient d(q);
    E2E_CONNECT(d);
    QCOMPARE(d.prompts.size(), 0);
    QCOMPARE(d.conn.authMethod(), QStringLiteral("publickey"));
    d.conn.close();

    // A stored profile: its own entry wins over the key entry (the stale key entry is never
    // tried and stays).
    QVERIFY(SecretStore::store(keySecret, QStringLiteral("stale")));
    SshProfile stored = p;
    stored.id = QStringLiteral("e2e-key-profile");
    const QString profileKey = QStringLiteral("ssh/%1/passphrase").arg(stored.id);
    QVERIFY(SecretStore::store(profileKey, passphrase));
    TestClient e(stored);
    E2E_CONNECT(e);
    QCOMPARE(e.prompts.size(), 0);
    QCOMPARE(SecretStore::load(keySecret).value_or(QString()), QStringLiteral("stale"));
    e.conn.close();

    // The other way round: the stale profile entry is dropped, the key entry serves, no prompt.
    QVERIFY(SecretStore::store(profileKey, QStringLiteral("stale")));
    QVERIFY(SecretStore::store(keySecret, passphrase));
    TestClient f(stored);
    E2E_CONNECT(f);
    QCOMPARE(f.prompts.size(), 0);
    QTRY_VERIFY_WITH_TIMEOUT(!SecretStore::contains(profileKey), kE2eTimeoutMs);
    QCOMPARE(SecretStore::load(keySecret).value_or(QString()), passphrase);
    f.conn.close();

    // Both stale: both dropped, the prompt is attempt 3, and "remember" for a profile stores
    // under the profile id AND the key path.
    QVERIFY(SecretStore::store(profileKey, QStringLiteral("stale")));
    QVERIFY(SecretStore::store(keySecret, QStringLiteral("stale-too")));
    TestClient g(stored);
    g.responses = {passphrase};
    g.rememberResponse = true;
    E2E_CONNECT(g);
    QCOMPARE(g.prompts.size(), 1);
    QCOMPARE(g.prompts.first().attempt, 3);
    QTRY_COMPARE_WITH_TIMEOUT(SecretStore::load(profileKey).value_or(QString()), passphrase, kE2eTimeoutMs);
    QTRY_COMPARE_WITH_TIMEOUT(SecretStore::load(keySecret).value_or(QString()), passphrase, kE2eTimeoutMs);
    g.conn.close();
    QVERIFY(SecretStore::remove(profileKey));
    QVERIFY(SecretStore::remove(keySecret));
}

void Tst_sshconnection::e2eShellFallbackTransfers()
{
    // A dropbear-like server: the sftp subsystem is refused, so every transfer runs through
    // exec channels (cat / wc / test / chmod / rm) - the same expectations as e2eSftpTransfers.
    TestSshServer::Options options = serverOptions(QStringLiteral("shellxfer"));
    options.allowSftp = false;
    TestSshServer server(options);
    QVERIFY(server.start());
    ServerSpy spy(&server);
    TestClient c(serverProfile(server, QStringLiteral("shellxfer")));
    E2E_CONNECT(c);
    QSignalSpy started(&c.conn, &SshConnection::transferStarted);
    QSignalSpy progress(&c.conn, &SshConnection::transferProgress);
    QSignalSpy finished(&c.conn, &SshConnection::transferFinished);

    const QByteArray payload = randomBytes(2 * 1024 * 1024);
    const QString upPath = m_dir.filePath(QStringLiteral("shellxfer/local/up.bin"));
    const QString downPath = m_dir.filePath(QStringLiteral("shellxfer/local/down.bin"));
    QVERIFY(writeFile(upPath, payload));
    const QString remoteName = QStringLiteral("it's up.bin");   // a quote in the name: shell quoting
    const QString remoteFile = QDir(options.rootDir).filePath(remoteName);

    // Upload 2 MiB with monotonic progress ending at the total; the method is "shell".
    SshConnection::TransferRequest upload;
    upload.direction = SshConnection::TransferDirection::Upload;
    upload.localPath = upPath;
    upload.remotePath = remoteName;
    QVERIFY(c.conn.startTransfer(upload));
    QVERIFY(c.conn.isTransferActive());
    QVERIFY(!c.conn.startTransfer(upload));   // busy
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 60000);
    QVERIFY2(finished.at(0).at(0).toBool(), qPrintable(finished.at(0).at(1).toString()));
    QVERIFY(!c.conn.isTransferActive());
    QCOMPARE(c.conn.transferStatus().method, QStringLiteral("shell"));
    const QString uploadMessage = finished.at(0).at(1).toString();
    QVERIFY2(uploadMessage.endsWith(QStringLiteral(", via shell)")), qPrintable(uploadMessage));
    QVERIFY2(uploadMessage.startsWith(QStringLiteral("Uploaded up.bin to it's up.bin (")), qPrintable(uploadMessage));
    QCOMPARE(started.count(), 1);
    QCOMPARE(started.at(0).at(0).value<SshConnection::TransferRequest>().remotePath, remoteName);
    QVERIFY(progress.count() >= 2);
    qint64 previous = -1;
    for (const QList<QVariant>& args : progress) {
        const qint64 done = args.at(0).toLongLong();
        QVERIFY2(done >= previous, qPrintable(QStringLiteral("%1 < %2").arg(done).arg(previous)));
        QCOMPARE(args.at(1).toLongLong(), qint64(payload.size()));
        previous = done;
    }
    QCOMPARE(previous, qint64(payload.size()));
    QCOMPARE(c.conn.transferStatus().done, qint64(payload.size()));
    QCOMPARE(readAll(remoteFile), payload);
    QTRY_COMPARE_WITH_TIMEOUT(spy.sftp, 1, kE2eTimeoutMs);   // the probe, refused
    QTRY_VERIFY_WITH_TIMEOUT(spy.execCommands.contains(QStringLiteral("cat > 'it'\\''s up.bin'")), kE2eTimeoutMs);
    QVERIFY2(spy.execCommands.filter(QRegularExpression(QStringLiteral("^chmod [0-7]{3} 'it'\\\\''s up\\.bin'$"))).size() == 1,
             qPrintable(spy.execCommands.join(QStringLiteral(" | "))));

    // overwrite = false refuses an existing remote file (test -e).
    upload.overwrite = false;
    QVERIFY(c.conn.startTransfer(upload));
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 2, kE2eTimeoutMs);
    QVERIFY(!finished.at(1).at(0).toBool());
    QVERIFY2(finished.at(1).at(1).toString().contains(QStringLiteral("exists")), qPrintable(finished.at(1).at(1).toString()));
    QCOMPARE(readAll(remoteFile), payload);
    QTRY_VERIFY_WITH_TIMEOUT(spy.execCommands.contains(QStringLiteral("test -e 'it'\\''s up.bin'")), kE2eTimeoutMs);

    // Download it back byte-identical (wc -c for the size, then cat), no .part left behind.
    SshConnection::TransferRequest download;
    download.direction = SshConnection::TransferDirection::Download;
    download.localPath = downPath;
    download.remotePath = remoteName;
    progress.clear();
    QVERIFY(c.conn.startTransfer(download));
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 3, 60000);
    QVERIFY2(finished.at(2).at(0).toBool(), qPrintable(finished.at(2).at(1).toString()));
    const QString downloadMessage = finished.at(2).at(1).toString();
    QVERIFY2(downloadMessage.endsWith(QStringLiteral(", via shell)")), qPrintable(downloadMessage));
    QCOMPARE(c.conn.transferStatus().method, QStringLiteral("shell"));
    QCOMPARE(c.conn.transferStatus().total, qint64(payload.size()));
    QVERIFY(QFileInfo::exists(downPath));
    QVERIFY(!QFileInfo::exists(downPath + QStringLiteral(".part")));
    QCOMPARE(readAll(downPath).size(), payload.size());
    QCOMPARE(readAll(downPath), payload);
    QVERIFY(progress.count() >= 2);
    previous = -1;
    for (const QList<QVariant>& args : progress) {
        const qint64 done = args.at(0).toLongLong();
        QVERIFY2(done >= previous, qPrintable(QStringLiteral("%1 < %2").arg(done).arg(previous)));
        QCOMPARE(args.at(1).toLongLong(), qint64(payload.size()));
        previous = done;
    }
    QCOMPARE(previous, qint64(payload.size()));
    QTRY_VERIFY_WITH_TIMEOUT(spy.execCommands.contains(QStringLiteral("wc -c < 'it'\\''s up.bin'")), kE2eTimeoutMs);
    QVERIFY(spy.execCommands.contains(QStringLiteral("cat 'it'\\''s up.bin'")));

    // overwrite = false refuses an existing local file.
    download.overwrite = false;
    QVERIFY(c.conn.startTransfer(download));
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 4, kE2eTimeoutMs);
    QVERIFY(!finished.at(3).at(0).toBool());
    QVERIFY2(finished.at(3).at(1).toString().contains(QStringLiteral("exists")), qPrintable(finished.at(3).at(1).toString()));

    // A missing remote file gives a readable error (the shell's stderr) and leaves nothing local.
    download.remotePath = QStringLiteral("does-not-exist.bin");
    download.localPath = m_dir.filePath(QStringLiteral("shellxfer/local/missing.bin"));
    QVERIFY(c.conn.startTransfer(download));
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 5, kE2eTimeoutMs);
    QVERIFY(!finished.at(4).at(0).toBool());
    const QString missingMessage = finished.at(4).at(1).toString();
    QVERIFY2(missingMessage.contains(QStringLiteral("does-not-exist.bin")), qPrintable(missingMessage));
    QVERIFY2(missingMessage.contains(QStringLiteral("No such file"), Qt::CaseInsensitive), qPrintable(missingMessage));
    QVERIFY(!QFileInfo::exists(download.localPath));
    QVERIFY(!QFileInfo::exists(download.localPath + QStringLiteral(".part")));

    // cancelTransfer(): a cancelled upload leaves no remote file (rm -f after the close), a
    // cancelled download leaves no .part file.
    SshConnection::TransferRequest cancelledUpload = upload;
    cancelledUpload.overwrite = true;
    cancelledUpload.remotePath = QStringLiteral("cancelled-up.bin");
    QVERIFY(c.conn.startTransfer(cancelledUpload));
    c.conn.cancelTransfer();
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 6, kE2eTimeoutMs);
    QVERIFY(!finished.at(5).at(0).toBool());
    QVERIFY2(finished.at(5).at(1).toString().contains(QStringLiteral("cancel"), Qt::CaseInsensitive),
             qPrintable(finished.at(5).at(1).toString()));
    QVERIFY(!c.conn.isTransferActive());
    QTRY_VERIFY_WITH_TIMEOUT(spy.execCommands.contains(QStringLiteral("rm -f 'cancelled-up.bin'")), kE2eTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(QDir(options.rootDir).filePath(QStringLiteral("cancelled-up.bin"))), kE2eTimeoutMs);
    SshConnection::TransferRequest cancelledDownload;
    cancelledDownload.direction = SshConnection::TransferDirection::Download;
    cancelledDownload.localPath = m_dir.filePath(QStringLiteral("shellxfer/local/cancelled.bin"));
    cancelledDownload.remotePath = remoteName;
    QVERIFY(c.conn.startTransfer(cancelledDownload));
    c.conn.cancelTransfer();
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 7, kE2eTimeoutMs);
    QVERIFY(!finished.at(6).at(0).toBool());
    QVERIFY(!QFileInfo::exists(cancelledDownload.localPath));
    QVERIFY(!QFileInfo::exists(cancelledDownload.localPath + QStringLiteral(".part")));

    // A transfer while the shell keeps echoing.
    c.received.clear();
    SshConnection::TransferRequest during = upload;
    during.overwrite = true;
    during.remotePath = QStringLiteral("during.bin");
    QVERIFY(c.conn.startTransfer(during));
    c.conn.write(QByteArrayLiteral("echo DURING\r"));
    QTRY_VERIFY2_WITH_TIMEOUT(c.received.contains("DURING\r\n$ "), c.received.constData(), kE2eTimeoutMs);
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 8, 60000);
    QVERIFY2(finished.at(7).at(0).toBool(), qPrintable(finished.at(7).at(1).toString()));
    QCOMPARE(readAll(QDir(options.rootDir).filePath(QStringLiteral("during.bin"))), payload);
    c.received.clear();
    c.conn.write(QByteArrayLiteral("echo AFTER\r"));
    QTRY_VERIFY_WITH_TIMEOUT(c.received.contains("AFTER\r\n$ "), kE2eTimeoutMs);

    // A remote name starting with '-' is written as "./-name" so cat / rm / chmod do not read
    // it as an option (and `cat -` does not read the channel's stdin, which never ends): upload,
    // download and the cleanup of a cancelled upload.
    const QString dashName = QStringLiteral("-dash.bin");
    SshConnection::TransferRequest dashUp = upload;
    dashUp.overwrite = true;
    dashUp.remotePath = dashName;
    QVERIFY(c.conn.startTransfer(dashUp));
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 9, 60000);
    QVERIFY2(finished.at(8).at(0).toBool(), qPrintable(finished.at(8).at(1).toString()));
    QCOMPARE(readAll(QDir(options.rootDir).filePath(dashName)), payload);
    QTRY_VERIFY_WITH_TIMEOUT(spy.execCommands.contains(QStringLiteral(": > './-dash.bin'")), kE2eTimeoutMs);
    QVERIFY(spy.execCommands.contains(QStringLiteral("cat > './-dash.bin'")));
    QVERIFY2(spy.execCommands.filter(QRegularExpression(QStringLiteral("^chmod [0-7]{3} '\\./-dash\\.bin'$"))).size() == 1,
             qPrintable(spy.execCommands.join(QStringLiteral(" | "))));
    SshConnection::TransferRequest dashDown;
    dashDown.direction = SshConnection::TransferDirection::Download;
    dashDown.localPath = m_dir.filePath(QStringLiteral("shellxfer/local/dash-down.bin"));
    dashDown.remotePath = dashName;
    QVERIFY(c.conn.startTransfer(dashDown));
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 10, 60000);
    QVERIFY2(finished.at(9).at(0).toBool(), qPrintable(finished.at(9).at(1).toString()));
    QCOMPARE(readAll(dashDown.localPath), payload);
    QTRY_VERIFY_WITH_TIMEOUT(spy.execCommands.contains(QStringLiteral("wc -c < './-dash.bin'")), kE2eTimeoutMs);
    QVERIFY(spy.execCommands.contains(QStringLiteral("cat './-dash.bin'")));
    QVERIFY(c.conn.startTransfer(dashUp));
    c.conn.cancelTransfer();
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 11, kE2eTimeoutMs);
    QVERIFY(!finished.at(10).at(0).toBool());
    QTRY_VERIFY_WITH_TIMEOUT(spy.execCommands.contains(QStringLiteral("rm -f './-dash.bin'")), kE2eTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(QDir(options.rootDir).filePath(dashName)), kE2eTimeoutMs);

    // The remote home comes from `pwd` over an exec channel.
    QSignalSpy home(&c.conn, &SshConnection::remoteHomeReceived);
    c.conn.requestRemoteHome();
    QTRY_COMPARE_WITH_TIMEOUT(home.count(), 1, kE2eTimeoutMs);
    const QString path = home.at(0).at(0).toString();
    QVERIFY2(!path.isEmpty(), "remote home is empty");
    QCOMPARE(QFileInfo(path).canonicalFilePath(), QFileInfo(options.rootDir).canonicalFilePath());
    QCOMPARE(c.conn.remoteHome(), path);
    QTRY_VERIFY_WITH_TIMEOUT(spy.execCommands.contains(QStringLiteral("pwd")), kE2eTimeoutMs);

    QVERIFY2(c.errors.isEmpty(), qPrintable(c.errors.join(QStringLiteral(" | "))));
    QCOMPARE(c.lost, 0);
    QCOMPARE(spy.sftp, 1);   // probed exactly once per session
    c.conn.close();
    QVERIFY(!c.conn.isTransferActive());
}

void Tst_sshconnection::e2eShellFallbackRemoteHome()
{
    // The remote home over the fallback (`pwd` on an exec channel) and the SFTP probe: refused
    // once, the session never asks again; a new session probes anew.
    TestSshServer::Options options = serverOptions(QStringLiteral("shellhome"));
    options.allowSftp = false;
    TestSshServer server(options);
    QVERIFY(server.start());
    ServerSpy spy(&server);
    TestClient c(serverProfile(server, QStringLiteral("shellhome")));
    E2E_CONNECT(c);
    QSignalSpy home(&c.conn, &SshConnection::remoteHomeReceived);
    c.conn.requestRemoteHome();
    QTRY_COMPARE_WITH_TIMEOUT(home.count(), 1, kE2eTimeoutMs);
    QCOMPARE(QFileInfo(home.at(0).at(0).toString()).canonicalFilePath(), QFileInfo(options.rootDir).canonicalFilePath());
    QTRY_COMPARE_WITH_TIMEOUT(spy.sftp, 1, kE2eTimeoutMs);
    c.conn.requestRemoteHome();
    QTRY_COMPARE_WITH_TIMEOUT(home.count(), 2, kE2eTimeoutMs);
    QCOMPARE(spy.sftp, 1);   // not probed again within the session
    c.conn.close();

    // A new session probes again.
    E2E_CONNECT(c);
    c.conn.requestRemoteHome();
    QTRY_COMPARE_WITH_TIMEOUT(home.count(), 3, kE2eTimeoutMs);
    QTRY_COMPARE_WITH_TIMEOUT(spy.sftp, 2, kE2eTimeoutMs);
    QVERIFY2(c.errors.isEmpty(), qPrintable(c.errors.join(QStringLiteral(" | "))));
    c.conn.close();
}

void Tst_sshconnection::e2eShellFallbackFailedUploadRemoved()
{
    // SshConnection.h: a shell upload that fails - here `cat > 'p'` ending with "No space left
    // on device" after part of the data - removes the truncated remote file on the live link,
    // exactly like a cancel does, so nothing that looks complete is left on the board; the
    // failure names cat's reason. A file that cannot be created fails at the `: > 'p'` probe
    // and is never touched.
    TestSshServer::Options options = serverOptions(QStringLiteral("shellfull"));
    options.allowSftp = false;
    options.execUploadLimit = 300 * 1024;
    TestSshServer server(options);
    QVERIFY(server.start());
    ServerSpy spy(&server);
    TestClient c(serverProfile(server, QStringLiteral("shellfull")));
    E2E_CONNECT(c);
    QSignalSpy finished(&c.conn, &SshConnection::transferFinished);
    const QByteArray payload = randomBytes(1024 * 1024);
    const QString upPath = m_dir.filePath(QStringLiteral("shellfull/local/up.bin"));
    QVERIFY(writeFile(upPath, payload));
    const QString remoteFile = QDir(options.rootDir).filePath(QStringLiteral("full.bin"));

    SshConnection::TransferRequest upload;
    upload.direction = SshConnection::TransferDirection::Upload;
    upload.localPath = upPath;
    upload.remotePath = QStringLiteral("full.bin");
    QVERIFY(c.conn.startTransfer(upload));
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 60000);
    QVERIFY(!finished.at(0).at(0).toBool());
    const QString message = finished.at(0).at(1).toString();
    QVERIFY2(message.startsWith(QStringLiteral("Upload to full.bin failed: ")), qPrintable(message));
    QVERIFY2(message.contains(QStringLiteral("No space left on device")), qPrintable(message));
    QVERIFY(!c.conn.isTransferActive());
    QTRY_VERIFY_WITH_TIMEOUT(spy.execCommands.contains(QStringLiteral("rm -f 'full.bin'")), kE2eTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(remoteFile), kE2eTimeoutMs);
    // The order of the commands: the truncation probe, the data command, the removal.
    const qsizetype probe = spy.execCommands.indexOf(QStringLiteral(": > 'full.bin'"));
    const qsizetype data = spy.execCommands.indexOf(QStringLiteral("cat > 'full.bin'"));
    const qsizetype removal = spy.execCommands.indexOf(QStringLiteral("rm -f 'full.bin'"));
    QVERIFY2(probe >= 0 && probe < data && data < removal, qPrintable(spy.execCommands.join(QStringLiteral(" | "))));

    // A path that cannot be written: refused by the probe with the shell's reason, no data
    // command, nothing to remove.
    upload.remotePath = QStringLiteral("missing-dir/full.bin");
    QVERIFY(c.conn.startTransfer(upload));
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 2, kE2eTimeoutMs);
    QVERIFY(!finished.at(1).at(0).toBool());
    const QString refused = finished.at(1).at(1).toString();
    QVERIFY2(refused.startsWith(QStringLiteral("Cannot create remote file missing-dir/full.bin: ")), qPrintable(refused));
    QTRY_VERIFY_WITH_TIMEOUT(spy.execCommands.contains(QStringLiteral(": > 'missing-dir/full.bin'")), kE2eTimeoutMs);
    QVERIFY(!spy.execCommands.contains(QStringLiteral("cat > 'missing-dir/full.bin'")));
    QVERIFY(!spy.execCommands.contains(QStringLiteral("rm -f 'missing-dir/full.bin'")));
    QVERIFY(!c.conn.isTransferActive());

    // The link is alive and the shell still answers.
    c.received.clear();
    c.conn.write(QByteArrayLiteral("echo STILL\r"));
    QTRY_VERIFY_WITH_TIMEOUT(c.received.contains("STILL\r\n$ "), kE2eTimeoutMs);
    QVERIFY2(c.errors.isEmpty(), qPrintable(c.errors.join(QStringLiteral(" | "))));
    QCOMPARE(c.lost, 0);
    c.conn.close();
}

void Tst_sshconnection::e2eShellFallbackUploadWithoutExitStatus()
{
    // SshConnection.h: a server that ends an exec channel without an exit status (EOF + close
    // only). Transfer::done counts what the channel accepted, not what cat wrote, so the upload
    // is confirmed by `wc -c` against the local size before it is called a success; a download
    // was always counted against wc. On a full disk the failure is reported and the file removed.
    TestSshServer::Options options = serverOptions(QStringLiteral("shellnostatus"));
    options.allowSftp = false;
    options.sendExecExitStatus = false;
    TestSshServer server(options);
    QVERIFY(server.start());
    ServerSpy spy(&server);
    TestClient c(serverProfile(server, QStringLiteral("shellnostatus")));
    E2E_CONNECT(c);
    QSignalSpy finished(&c.conn, &SshConnection::transferFinished);
    const QByteArray payload = randomBytes(256 * 1024 + 17);
    const QString upPath = m_dir.filePath(QStringLiteral("shellnostatus/local/up.bin"));
    QVERIFY(writeFile(upPath, payload));
    const QString remoteFile = QDir(options.rootDir).filePath(QStringLiteral("nostatus.bin"));

    SshConnection::TransferRequest upload;
    upload.direction = SshConnection::TransferDirection::Upload;
    upload.localPath = upPath;
    upload.remotePath = QStringLiteral("nostatus.bin");
    QVERIFY(c.conn.startTransfer(upload));
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 60000);
    QVERIFY2(finished.at(0).at(0).toBool(), qPrintable(finished.at(0).at(1).toString()));
    QVERIFY2(finished.at(0).at(1).toString().endsWith(QStringLiteral(", via shell)")), qPrintable(finished.at(0).at(1).toString()));
    QCOMPARE(readAll(remoteFile), payload);
    QTRY_VERIFY_WITH_TIMEOUT(spy.execCommands.contains(QStringLiteral("wc -c < 'nostatus.bin'")), kE2eTimeoutMs);
    QVERIFY(spy.execCommands.indexOf(QStringLiteral("cat > 'nostatus.bin'")) <
            spy.execCommands.indexOf(QStringLiteral("wc -c < 'nostatus.bin'")));
    QVERIFY(!spy.execCommands.contains(QStringLiteral("rm -f 'nostatus.bin'")));

    SshConnection::TransferRequest download;
    download.direction = SshConnection::TransferDirection::Download;
    download.localPath = m_dir.filePath(QStringLiteral("shellnostatus/local/down.bin"));
    download.remotePath = QStringLiteral("nostatus.bin");
    QVERIFY(c.conn.startTransfer(download));
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 2, 60000);
    QVERIFY2(finished.at(1).at(0).toBool(), qPrintable(finished.at(1).at(1).toString()));
    QCOMPARE(readAll(download.localPath), payload);
    QVERIFY(!QFileInfo::exists(download.localPath + QStringLiteral(".part")));
    QVERIFY2(c.errors.isEmpty(), qPrintable(c.errors.join(QStringLiteral(" | "))));
    QCOMPARE(c.lost, 0);
    c.conn.close();

    // The same server on a full disk: cat's stderr names the reason although no status came,
    // and the truncated file is removed.
    TestSshServer::Options full = serverOptions(QStringLiteral("shellnostatusfull"));
    full.allowSftp = false;
    full.sendExecExitStatus = false;
    full.execUploadLimit = 100 * 1024;
    TestSshServer fullServer(full);
    QVERIFY(fullServer.start());
    ServerSpy fullSpy(&fullServer);
    TestClient d(serverProfile(fullServer, QStringLiteral("shellnostatusfull")));
    E2E_CONNECT(d);
    QSignalSpy fullFinished(&d.conn, &SshConnection::transferFinished);
    QVERIFY(d.conn.startTransfer(upload));
    QTRY_COMPARE_WITH_TIMEOUT(fullFinished.count(), 1, 60000);
    QVERIFY(!fullFinished.at(0).at(0).toBool());
    const QString failure = fullFinished.at(0).at(1).toString();
    QVERIFY2(failure.contains(QStringLiteral("No space left on device")), qPrintable(failure));
    QTRY_VERIFY_WITH_TIMEOUT(fullSpy.execCommands.contains(QStringLiteral("rm -f 'nostatus.bin'")), kE2eTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(QDir(full.rootDir).filePath(QStringLiteral("nostatus.bin"))), kE2eTimeoutMs);
    QCOMPARE(d.lost, 0);
    d.conn.close();
}

void Tst_sshconnection::e2eCloseDuringUploadRemovesRemote()
{
    // SshConnection.h: close() during an upload reports it at once with transferFinished(false,
    // "Transfer aborted: the connection was closed") - the worker's own report belongs to the
    // generation that was closed and is dropped - and the worker still removes the incomplete
    // remote file on the live link, through SFTP and through the shell fallback alike.
    for (const bool sftp : {true, false}) {
        const QString name = sftp ? QStringLiteral("closesftp") : QStringLiteral("closeshell");
        TestSshServer::Options options = serverOptions(name);
        options.allowSftp = sftp;
        TestSshServer server(options);
        QVERIFY(server.start());
        ServerSpy spy(&server);
        TestClient c(serverProfile(server, name));
        E2E_CONNECT(c);
        const QByteArray payload = randomBytes(24 * 1024 * 1024);
        const QString upPath = m_dir.filePath(name + QStringLiteral("/local/up.bin"));
        QVERIFY(writeFile(upPath, payload));
        const QString remoteFile = QDir(options.rootDir).filePath(QStringLiteral("closed.bin"));
        QSignalSpy started(&c.conn, &SshConnection::transferStarted);
        QSignalSpy finished(&c.conn, &SshConnection::transferFinished);

        SshConnection::TransferRequest upload;
        upload.direction = SshConnection::TransferDirection::Upload;
        upload.localPath = upPath;
        upload.remotePath = QStringLiteral("closed.bin");
        QVERIFY(c.conn.startTransfer(upload));
        QTRY_COMPARE_WITH_TIMEOUT(started.count(), 1, kE2eTimeoutMs);
        QVERIFY2(c.conn.isTransferActive(), sftp ? "sftp" : "shell");
        QCOMPARE(c.conn.transferStatus().method, sftp ? QStringLiteral("sftp") : QStringLiteral("shell"));
        c.conn.close();
        QCOMPARE(finished.count(), 1);
        QVERIFY(!finished.at(0).at(0).toBool());
        QCOMPARE(finished.at(0).at(1).toString(), QStringLiteral("Transfer aborted: the connection was closed"));
        QVERIFY(!c.conn.isTransferActive());
        QCOMPARE(c.conn.state(), Transport::State::Disconnected);
        QTRY_COMPARE_WITH_TIMEOUT(server.activeConnections(), 0, kE2eTimeoutMs);
        QTRY_VERIFY2_WITH_TIMEOUT(!QFileInfo::exists(remoteFile), sftp ? "sftp left the file" : "shell left the file",
                                  kE2eTimeoutMs);
        if (!sftp) {
            QTRY_VERIFY_WITH_TIMEOUT(spy.execCommands.contains(QStringLiteral("rm -f 'closed.bin'")), kE2eTimeoutMs);
        }
        QTest::qWait(200);
        QCOMPARE(finished.count(), 1);   // the worker's report for the closed generation was dropped
        QCOMPARE(c.lost, 0);
    }
}

void Tst_sshconnection::probeShellResizeAndSftp()
{
    const ProbeEnv env = probeEnv();
    if (!env.enabled) {
        QSKIP(kSkipMessage);
    }
    const QString knownHosts = m_dir.filePath(QStringLiteral("probe/known_hosts"));
    ProbeSession s(env, knownHosts);
    s.apply();
    QSignalSpy shellStarted(&s.conn, &SshConnection::shellStarted);
    QSignalSpy dataSent(&s.conn, &Transport::dataSent);
    QSignalSpy txWritten(&s.conn, &Transport::txBytesWritten);
    QSignalSpy counters(&s.conn, &Transport::countersChanged);

    QVERIFY(s.conn.open());
    QCOMPARE(s.conn.state(), Transport::State::Connecting);
    QTRY_COMPARE_WITH_TIMEOUT(s.conn.state(), Transport::State::Connected, 40000);
    QVERIFY2(s.errors.isEmpty(), qPrintable(s.errors.join(QStringLiteral(" | "))));
    QCOMPARE(s.states, QList<Transport::State>({Transport::State::Connecting, Transport::State::Connected}));
    QCOMPARE(shellStarted.count(), 1);
    QCOMPARE(s.methods.size(), 1);
    QCOMPARE(s.methods.first(), s.conn.authMethod());
    if (!env.password.isEmpty()) {
        QCOMPARE(s.prompts.size(), 1);
        QCOMPARE(s.prompts.first().kind, SshConnection::PromptKind::Password);
        QCOMPARE(s.prompts.first().attempt, 1);
        QCOMPARE(s.prompts.first().user, env.profile.user);
        QVERIFY(s.prompts.first().canRemember);        // ad-hoc: remembered under the target
        QCOMPARE(s.prompts.first().rememberTarget, env.profile.displayTarget());
        QCOMPARE(s.conn.authMethod(), QStringLiteral("password"));
    }

    // Host key: unknown on the first contact, stored in the temporary known_hosts.
    QCOMPARE(s.hostKeys.size(), 1);
    const SshConnection::HostKeyInfo key = s.hostKeys.first();
    QCOMPARE(key.status, SshConnection::HostKeyStatus::Unknown);
    QCOMPARE(key.host, env.profile.host);
    QCOMPARE(key.port, env.profile.port);
    QCOMPARE(key.knownHostsFile, QFileInfo(knownHosts).absoluteFilePath());
    QVERIFY(key.fingerprintSha256.startsWith(QStringLiteral("SHA256:")));
    QVERIFY(key.fingerprintMd5.startsWith(QStringLiteral("MD5:")));
    QVERIFY(key.publicKeyLine.startsWith(key.keyType + QLatin1Char(' ')));
    QVERIFY(QFile::exists(knownHosts));
    QString storedKey;
    QCOMPARE(countKnownHostsEntries(knownHosts, knownHostsToken(env.profile), key.keyType, &storedKey), 1);
    QCOMPARE(key.keyType + QLatin1Char(' ') + storedKey, key.publicKeyLine);
    QCOMPARE(s.conn.hostKeyType(), key.keyType);
    QCOMPARE(s.conn.hostKeyFingerprint(), key.fingerprintSha256);
    QVERIFY2(s.conn.serverVersion().startsWith(QStringLiteral("SSH-2.0")), qPrintable(s.conn.serverVersion()));
    QCOMPARE(s.conn.summary(), key.keyType + QStringLiteral(" · ") + s.conn.authMethod());

    // Shell round trip.
    QTRY_VERIFY_WITH_TIMEOUT(!s.received.isEmpty(), 15000);   // banner / prompt
    s.received.clear();
    const QByteArray command = QByteArrayLiteral("echo PROBE-OK-$((40+2))\r");
    QCOMPARE(s.conn.write(command), qint64(command.size()));
    QCOMPARE(dataSent.count(), 1);
    QTRY_VERIFY_WITH_TIMEOUT(s.received.contains("PROBE-OK-42"), 15000);
    QTRY_VERIFY_WITH_TIMEOUT(txWritten.count() >= 1, 5000);
    QCOMPARE(s.conn.bytesSent(), quint64(command.size()));
    QVERIFY(s.conn.bytesReceived() > 0);
    QVERIFY(counters.count() >= 2);

    // Window change reaches the remote PTY.
    s.conn.notifyTerminalSize(132, 40);
    QCOMPARE(s.conn.lastTerminalSize(), QStringLiteral("132 x 40"));
    QTest::qWait(200);
    s.received.clear();
    s.conn.write(QByteArrayLiteral("stty size\r"));
    QTRY_VERIFY2_WITH_TIMEOUT(s.received.contains("40 132"), s.received.constData(), 15000);

    // SFTP: 1 MiB up, refuse overwrite, 1 MiB down, compare.
    const QString upPath = m_dir.filePath(QStringLiteral("probe/up.bin"));
    const QString downPath = m_dir.filePath(QStringLiteral("probe/down.bin"));
    const QByteArray payload = randomBytes(1024 * 1024);
    {
        QFile up(upPath);
        QVERIFY(up.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(up.write(payload), qint64(payload.size()));
    }
    const QString remotePath = QStringLiteral("/tmp/su-probe-%1.bin").arg(QCoreApplication::applicationPid());
    QSignalSpy started(&s.conn, &SshConnection::transferStarted);
    QSignalSpy progress(&s.conn, &SshConnection::transferProgress);
    QSignalSpy finished(&s.conn, &SshConnection::transferFinished);

    SshConnection::TransferRequest upload;
    upload.direction = SshConnection::TransferDirection::Upload;
    upload.localPath = upPath;
    upload.remotePath = remotePath;
    QVERIFY(s.conn.startTransfer(upload));
    QVERIFY(s.conn.isTransferActive());
    QVERIFY(!s.conn.startTransfer(upload));                // busy
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 60000);
    QVERIFY2(finished.at(0).at(0).toBool(), qPrintable(finished.at(0).at(1).toString()));
    QVERIFY(!s.conn.isTransferActive());
    QCOMPARE(started.count(), 1);
    QVERIFY(progress.count() >= 1);
    QCOMPARE(progress.last().at(0).toLongLong(), qint64(payload.size()));
    QCOMPARE(progress.last().at(1).toLongLong(), qint64(payload.size()));
    QCOMPARE(s.conn.transferStatus().done, qint64(payload.size()));

    upload.overwrite = false;
    QVERIFY(s.conn.startTransfer(upload));
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 2, 30000);
    QVERIFY(!finished.at(1).at(0).toBool());
    QVERIFY(finished.at(1).at(1).toString().contains(QStringLiteral("exists")));

    SshConnection::TransferRequest download;
    download.direction = SshConnection::TransferDirection::Download;
    download.localPath = downPath;
    download.remotePath = remotePath;
    QVERIFY(s.conn.startTransfer(download));
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 3, 60000);
    QVERIFY2(finished.at(2).at(0).toBool(), qPrintable(finished.at(2).at(1).toString()));
    QVERIFY(QFile::exists(downPath));
    QVERIFY(!QFile::exists(downPath + QStringLiteral(".part")));
    const QByteArray downloaded = readAll(downPath);
    QCOMPARE(downloaded.size(), payload.size());
    QVERIFY(downloaded == payload);

    download.overwrite = false;
    QVERIFY(s.conn.startTransfer(download));
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 4, 30000);
    QVERIFY(!finished.at(3).at(0).toBool());

    // Cancel: an upload cancelled right after it started, a download cancelled removes its .part.
    QVERIFY(s.conn.startTransfer(upload = SshConnection::TransferRequest{SshConnection::TransferDirection::Upload, upPath,
                                                                          remotePath + QStringLiteral(".cancel"), true, true}));
    s.conn.cancelTransfer();
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 5, 30000);
    QVERIFY(!finished.at(4).at(0).toBool());
    QVERIFY(finished.at(4).at(1).toString().contains(QStringLiteral("cancel"), Qt::CaseInsensitive));
    QVERIFY(!s.conn.isTransferActive());
    const QString cancelledDownload = m_dir.filePath(QStringLiteral("probe/cancelled.bin"));
    QVERIFY(s.conn.startTransfer(SshConnection::TransferRequest{SshConnection::TransferDirection::Download, cancelledDownload,
                                                                remotePath, true, true}));
    s.conn.cancelTransfer();
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 6, 30000);
    QVERIFY(!finished.at(5).at(0).toBool());
    QVERIFY(!QFile::exists(cancelledDownload));
    QVERIFY(!QFile::exists(cancelledDownload + QStringLiteral(".part")));

    // The shell is still alive after the transfers, and the remote home resolves.
    QSignalSpy home(&s.conn, &SshConnection::remoteHomeReceived);
    s.conn.requestRemoteHome();
    QTRY_COMPARE_WITH_TIMEOUT(home.count(), 1, 15000);
    QVERIFY2(home.at(0).at(0).toString().startsWith(QLatin1Char('/')), qPrintable(home.at(0).at(0).toString()));
    QCOMPARE(s.conn.remoteHome(), home.at(0).at(0).toString());
    s.received.clear();
    s.conn.write(QByteArrayLiteral("rm -f ") + remotePath.toUtf8() + QByteArrayLiteral("*; echo PROBE-CLEAN-$((1+1))\r"));
    QTRY_VERIFY_WITH_TIMEOUT(s.received.contains("PROBE-CLEAN-2"), 15000);

    // Clean close.
    s.conn.close();
    QCOMPARE(s.conn.state(), Transport::State::Disconnected);
    QCOMPARE(s.conn.summary(), QStringLiteral("SSH"));
    QTest::qWait(300);
    QVERIFY2(s.errors.isEmpty(), qPrintable(s.errors.join(QStringLiteral(" | "))));
    QCOMPARE(s.lost, 0);
    QCOMPARE(s.closed.size(), 0);
    QCOMPARE(s.states.last(), Transport::State::Disconnected);
}

void Tst_sshconnection::probeWrongPasswordThenCancel()
{
    const ProbeEnv env = probeEnv();
    if (!env.enabled) {
        QSKIP(kSkipMessage);
    }
    if (env.password.isEmpty()) {
        QSKIP("SU_SSH_PROBE_PASSWORD not set");
    }
    ProbeSession s(env, m_dir.filePath(QStringLiteral("probe/known_hosts_wrongpw")));
    s.responses = {QStringLiteral("definitely-not-") + env.password};
    s.cancelAt = 2;
    s.apply();
    QVERIFY(s.conn.open());
    QTRY_COMPARE_WITH_TIMEOUT(s.conn.state(), Transport::State::Disconnected, 40000);
    QCOMPARE(s.prompts.size(), 2);
    QCOMPARE(s.prompts.at(0).attempt, 1);
    QCOMPARE(s.prompts.at(1).attempt, 2);
    QCOMPARE(s.prompts.at(1).kind, SshConnection::PromptKind::Password);
    QCOMPARE(s.errors.size(), 1);
    QVERIFY(s.errors.first().contains(QStringLiteral("cancel"), Qt::CaseInsensitive));
    QCOMPARE(s.states, QList<Transport::State>({Transport::State::Connecting, Transport::State::Disconnected}));
    QVERIFY(s.conn.authMethod().isEmpty());

    // Three wrong answers exhaust the method.
    ProbeSession t(env, m_dir.filePath(QStringLiteral("probe/known_hosts_wrongpw")));
    const QString wrong = QStringLiteral("wrong-") + env.password;
    t.responses = {wrong, wrong, wrong};
    t.apply();
    QVERIFY(t.conn.open());
    QTRY_COMPARE_WITH_TIMEOUT(t.conn.state(), Transport::State::Disconnected, 60000);
    QCOMPARE(t.prompts.size(), 3);
    QCOMPARE(t.prompts.at(2).attempt, 3);
    QCOMPARE(t.errors.size(), 1);
    QVERIFY2(t.errors.first().contains(QStringLiteral("Authentication failed")), qPrintable(t.errors.first()));
    QCOMPARE(t.hostKeys.size(), 0);   // the key was remembered by the previous session
}

void Tst_sshconnection::probeChangedHostKey()
{
    const ProbeEnv env = probeEnv();
    if (!env.enabled) {
        QSKIP(kSkipMessage);
    }
    const QString knownHosts = m_dir.filePath(QStringLiteral("probe/known_hosts_changed"));
    const QString token = knownHostsToken(env.profile);

    // 1. First contact: unknown, accepted and remembered.
    QString keyType;
    QString realKey;
    {
        ProbeSession s(env, knownHosts);
        s.apply();
        QVERIFY(s.conn.open());
        QTRY_COMPARE_WITH_TIMEOUT(s.conn.state(), Transport::State::Connected, 40000);
        QCOMPARE(s.hostKeys.size(), 1);
        QCOMPARE(s.hostKeys.first().status, SshConnection::HostKeyStatus::Unknown);
        keyType = s.hostKeys.first().keyType;
        s.conn.close();
        QCOMPARE(countKnownHostsEntries(knownHosts, token, keyType, &realKey), 1);
    }

    // 2. The stored key is replaced by another one of the same type: Changed, accepted and
    //    remembered -> the old line is dropped and the real key stored, exactly once.
    QVERIFY(tamperKnownHosts(knownHosts, token, keyType));
    QString tamperedKey;
    QCOMPARE(countKnownHostsEntries(knownHosts, token, keyType, &tamperedKey), 1);
    QVERIFY(tamperedKey != realKey);
    {
        ProbeSession s(env, knownHosts);
        s.apply();
        QVERIFY(s.conn.open());
        QTRY_COMPARE_WITH_TIMEOUT(s.conn.state(), Transport::State::Connected, 40000);
        QCOMPARE(s.hostKeys.size(), 1);
        QCOMPARE(s.hostKeys.first().status, SshConnection::HostKeyStatus::Changed);
        QCOMPARE(s.hostKeys.first().keyType, keyType);
        QVERIFY2(s.errors.isEmpty(), qPrintable(s.errors.join(QStringLiteral(" | "))));
        s.conn.close();
        QString storedKey;
        QCOMPARE(countKnownHostsEntries(knownHosts, token, keyType, &storedKey), 1);
        QCOMPARE(storedKey, realKey);
    }

    // 3. Known now: no question at all.
    {
        ProbeSession s(env, knownHosts);
        s.apply();
        QVERIFY(s.conn.open());
        QTRY_COMPARE_WITH_TIMEOUT(s.conn.state(), Transport::State::Connected, 40000);
        QCOMPARE(s.hostKeys.size(), 0);
        s.conn.close();
    }

    // 4. Changed again and rejected: no connection, the file stays as it was.
    QVERIFY(tamperKnownHosts(knownHosts, token, keyType));
    {
        ProbeSession s(env, knownHosts);
        s.acceptKey = false;
        s.apply();
        QVERIFY(s.conn.open());
        QTRY_COMPARE_WITH_TIMEOUT(s.conn.state(), Transport::State::Disconnected, 40000);
        QCOMPARE(s.hostKeys.size(), 1);
        QCOMPARE(s.hostKeys.first().status, SshConnection::HostKeyStatus::Changed);
        QCOMPARE(s.errors.size(), 1);
        QVERIFY2(s.errors.first().contains(QStringLiteral("rejected")), qPrintable(s.errors.first()));
        QCOMPARE(s.prompts.size(), 0);
        QVERIFY(s.conn.authMethod().isEmpty());
        QString storedKey;
        QCOMPARE(countKnownHostsEntries(knownHosts, token, keyType, &storedKey), 1);
        QVERIFY(storedKey != realKey);
    }

    // 5. "Connect once" (accept without remember) connects but leaves the file untouched.
    {
        ProbeSession s(env, knownHosts);
        s.rememberKey = false;
        s.apply();
        QVERIFY(s.conn.open());
        QTRY_COMPARE_WITH_TIMEOUT(s.conn.state(), Transport::State::Connected, 40000);
        QCOMPARE(s.hostKeys.size(), 1);
        s.conn.close();
        QString storedKey;
        QCOMPARE(countKnownHostsEntries(knownHosts, token, keyType, &storedKey), 1);
        QVERIFY(storedKey != realKey);
    }
}

void Tst_sshconnection::probeKeyLogin()
{
    const ProbeEnv env = probeEnv();
    if (!env.enabled) {
        QSKIP(kSkipMessage);
    }
    if (env.identity.isEmpty()) {
        QSKIP("SU_SSH_PROBE_IDENTITY not set");
    }
    QVERIFY2(QFileInfo(env.identity).isFile(), qPrintable(env.identity));
    ProbeSession s(env, m_dir.filePath(QStringLiteral("probe/known_hosts_key")));
    s.profile.auth = SshProfile::Auth::PublicKey;
    s.profile.identityFile = env.identity;
    s.password.clear();
    s.apply();
    QVERIFY(s.conn.open());
    QTRY_COMPARE_WITH_TIMEOUT(s.conn.state(), Transport::State::Connected, 40000);
    QVERIFY2(s.errors.isEmpty(), qPrintable(s.errors.join(QStringLiteral(" | "))));
    QCOMPARE(s.prompts.size(), 0);
    QCOMPARE(s.conn.authMethod(), QStringLiteral("publickey"));
    QCOMPARE(s.methods, QStringList{QStringLiteral("publickey")});
    QTRY_VERIFY_WITH_TIMEOUT(!s.received.isEmpty(), 15000);
    s.received.clear();
    s.conn.write(QByteArrayLiteral("echo KEY-OK-$((20+1))\r"));
    QTRY_VERIFY_WITH_TIMEOUT(s.received.contains("KEY-OK-21"), 15000);
    s.conn.close();
    QCOMPARE(s.conn.state(), Transport::State::Disconnected);

    // Auto also finds the key when it is the profile's identity file.
    ProbeSession t(env, m_dir.filePath(QStringLiteral("probe/known_hosts_key")));
    t.profile.auth = SshProfile::Auth::Auto;
    t.profile.identityFile = env.identity;
    t.password.clear();
    t.apply();
    QVERIFY(t.conn.open());
    QTRY_COMPARE_WITH_TIMEOUT(t.conn.state(), Transport::State::Connected, 40000);
    QCOMPARE(t.conn.authMethod(), QStringLiteral("publickey"));
    QCOMPARE(t.prompts.size(), 0);
    t.conn.close();
}

void Tst_sshconnection::probeRemoteExit()
{
    const ProbeEnv env = probeEnv();
    if (!env.enabled) {
        QSKIP(kSkipMessage);
    }
    ProbeSession s(env, m_dir.filePath(QStringLiteral("probe/known_hosts_exit")));
    s.apply();
    QSignalSpy restored(&s.conn, &Transport::connectionRestored);
    QVERIFY(s.conn.open());
    QTRY_COMPARE_WITH_TIMEOUT(s.conn.state(), Transport::State::Connected, 40000);
    QTRY_VERIFY_WITH_TIMEOUT(!s.received.isEmpty(), 15000);
    s.conn.write(QByteArrayLiteral("exit 7\r"));
    QTRY_COMPARE_WITH_TIMEOUT(s.closed.size(), 1, 20000);
    QCOMPARE(s.closed.first(), 7);
    QCOMPARE(s.conn.exitStatus(), 7);
    QTRY_COMPARE_WITH_TIMEOUT(s.conn.state(), Transport::State::Disconnected, 5000);
    QTest::qWait(500);
    QCOMPARE(s.lost, 0);
    QCOMPARE(restored.count(), 0);
    QVERIFY2(s.errors.isEmpty(), qPrintable(s.errors.join(QStringLiteral(" | "))));
    QCOMPARE(s.states, QList<Transport::State>({Transport::State::Connecting, Transport::State::Connected,
                                                Transport::State::Disconnected}));
    QVERIFY(!s.states.contains(Transport::State::Reconnecting));
}

void Tst_sshconnection::probeAdHocRemember()
{
    // Ad-hoc remember round trip against the real sshd: the password answered with "remember"
    // lands under the target key (in this suite's isolated settings), the next session asks
    // nothing, and the entry is removed at the end. The value is never printed.
    const ProbeEnv env = probeEnv();
    if (!env.enabled) {
        QSKIP(kSkipMessage);
    }
    if (env.password.isEmpty()) {
        QSKIP("SU_SSH_PROBE_PASSWORD not set");
    }
    const QString key = QStringLiteral("ssh/target/%1/password").arg(env.profile.displayTarget());
    SecretStore::remove(key);
    QVERIFY(!SecretStore::contains(key));
    {
        ProbeSession s(env, m_dir.filePath(QStringLiteral("probe/known_hosts_remember")));
        s.rememberResponse = true;
        s.apply();
        QVERIFY(s.conn.open());
        QTRY_COMPARE_WITH_TIMEOUT(s.conn.state(), Transport::State::Connected, 40000);
        QCOMPARE(s.prompts.size(), 1);
        QVERIFY(s.prompts.first().canRemember);
        QCOMPARE(s.prompts.first().rememberTarget, env.profile.displayTarget());
        QTRY_VERIFY_WITH_TIMEOUT(SecretStore::load(key).value_or(QString()) == env.password, 15000);
        s.conn.close();
    }
    {
        ProbeSession t(env, m_dir.filePath(QStringLiteral("probe/known_hosts_remember")));
        t.apply();
        QVERIFY(t.conn.open());
        QTRY_COMPARE_WITH_TIMEOUT(t.conn.state(), Transport::State::Connected, 40000);
        QCOMPARE(t.prompts.size(), 0);
        QCOMPARE(t.conn.authMethod(), QStringLiteral("password"));
        QVERIFY2(t.errors.isEmpty(), qPrintable(t.errors.join(QStringLiteral(" | "))));
        t.conn.close();
    }
    // A stale entry is dropped and the prompt comes back as attempt 2.
    QVERIFY(SecretStore::store(key, QStringLiteral("definitely-stale")));
    {
        ProbeSession u(env, m_dir.filePath(QStringLiteral("probe/known_hosts_remember")));
        u.apply();
        QVERIFY(u.conn.open());
        QTRY_COMPARE_WITH_TIMEOUT(u.conn.state(), Transport::State::Connected, 40000);
        QCOMPARE(u.prompts.size(), 1);
        QCOMPARE(u.prompts.first().attempt, 2);
        QTRY_VERIFY_WITH_TIMEOUT(!SecretStore::contains(key), 15000);
        u.conn.close();
    }
    QVERIFY(!SecretStore::contains(key));
}

QTEST_GUILESS_MAIN(Tst_sshconnection)
#include "tst_sshconnection.moc"
