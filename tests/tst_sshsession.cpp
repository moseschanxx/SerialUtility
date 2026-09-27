// GUI end-to-end tests for the SSH feature: the whole user flow through the real MainWindow,
// SessionWidget, SshConnectionBar, HostKeyDialog, AuthPromptDialog, SshProfilesDialog and
// RemoteFileDialog against the in-process TestSshServer (tests/support/TestSshServer.h).
//
// Isolation: QSettings (AppSettings, SecretStore, the transfer dialog's paths) is redirected
// into a QTemporaryDir, QStandardPaths runs in test mode (the profile store's default file,
// which MainWindow loads and saves, lives there and is removed before every test), every
// known_hosts file is a temporary one (AppSettings::setSshKnownHostsFile() for ad-hoc targets
// and the profiles created in the dialog, the profile's own field otherwise), profiles never
// use Auth::Auto (that would read the developer's ~/.ssh/id_* keys) and SU_SSH_IGNORE_CONFIG
// keeps ~/.ssh/config out of the way. Nothing touches the user's ~/.ssh or registry.
//
// The modal questions of a connect (host key, password, the profile manager, the "still
// connected" confirmation) are answered from a timer inside their exec() loops by
// DialogResponder, the way a user would click them.

#include <QtTest>
#include <QElapsedTimer>

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QRandomGenerator>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <functional>
#include <memory>

#include "app/AppSettings.h"
#include "core/Transport.h"
#include "dialogs/AuthPromptDialog.h"
#include "dialogs/HostKeyDialog.h"
#include "dialogs/RemoteFileDialog.h"
#include "dialogs/SshProfilesDialog.h"
#include "ssh/SecretStore.h"
#include "ssh/SshConnection.h"
#include "ssh/SshProfile.h"
#include "support/TestSshServer.h"
#include "terminal/TerminalScreen.h"
#include "terminal/TerminalWidget.h"
#include "ui/MainWindow.h"
#include "ui/SessionWidget.h"
#include "ui/SshConnectionBar.h"

namespace {

using State = Transport::State;

constexpr int kSshTimeoutMs = 15000;        ///< a connect / a shell round trip to the in-process server
constexpr int kDropTimeoutMs = 5000;        ///< a network cut is noticed within this
constexpr int kTransferTimeoutMs = 60000;   ///< the SFTP transfers (256 KiB ... 50 MB)
constexpr int kSimTimeoutMs = 10000;        ///< SIM:loopback round trips
constexpr int kProbeTimeoutMs = 30000;      ///< a real OpenSSH server (WSL) answering a connect or a command

const QString kUser = QStringLiteral("test");
const QString kPassword = QStringLiteral("secret");
const QString kLoopback = QStringLiteral("SIM:loopback");
const QString kProfileKeyPrefix = QStringLiteral("ssh:profile:");
const QString kTargetKeyPrefix = QStringLiteral("ssh:target:");

// MainWindow::stateIcon() colours (the tab dot).
const QColor kDotConnected(0x3C, 0xB0, 0x43);
const QColor kDotBusy(0xF0, 0xA0, 0x30);
const QColor kDotDisconnected(0x90, 0x90, 0x90);

template <typename T>
T* child(const QObject* parent, const char* objectName)
{
    return parent->findChild<T*>(QLatin1String(objectName));
}

QAction* action(const MainWindow& w, const char* name)
{
    return w.findChild<QAction*>(QLatin1String(name));
}

QTabWidget* tabs(const MainWindow& w)
{
    return child<QTabWidget>(&w, "tabWidget");
}

QLabel* statusConnectionLabel(const MainWindow& w)
{
    return child<QLabel>(&w, "statusConnectionLabel");
}

/// Every line of the terminal (scrollback followed by the visible screen), trimmed.
QStringList allLines(const TerminalWidget* terminal)
{
    const TerminalScreen* screen = terminal->screen();
    QStringList lines;
    const int total = screen->totalLines();
    lines.reserve(total);
    for (int i = 0; i < total; ++i) {
        lines.append(screen->lineText(i).trimmed());
    }
    return lines;
}

QString allText(const TerminalWidget* terminal)
{
    return allLines(terminal).join(QLatin1Char('\n'));
}

/// The last line with any text on it ("$" for the scripted shell's prompt; lineText() trims
/// the prompt's trailing blank).
QString lastNonBlankLine(const TerminalWidget* terminal)
{
    const QStringList lines = allLines(terminal);
    for (qsizetype i = lines.size() - 1; i >= 0; --i) {
        if (!lines.at(i).isEmpty()) {
            return lines.at(i);
        }
    }
    return QString();
}

int countLines(const TerminalWidget* terminal, const QString& exact)
{
    return static_cast<int>(allLines(terminal).count(exact));
}

int countLinesStartingWith(const TerminalWidget* terminal, const QString& prefix)
{
    int n = 0;
    const QStringList lines = allLines(terminal);
    for (const QString& line : lines) {
        if (line.startsWith(prefix)) {
            ++n;
        }
    }
    return n;
}

int countLinesContaining(const TerminalWidget* terminal, const QString& text)
{
    int n = 0;
    const QStringList lines = allLines(terminal);
    for (const QString& line : lines) {
        if (line.contains(text)) {
            ++n;
        }
    }
    return n;
}

/// A real shell's prompt ("user@host:~$ ", "# ") is on the last line with text.
bool realPromptShown(const TerminalWidget* terminal)
{
    const QString last = lastNonBlankLine(terminal);
    return last.endsWith(QLatin1Char('$')) || last.endsWith(QLatin1Char('#'));
}

bool isReddish(const QColor& color)
{
    return color.red() > 150 && color.red() > color.green() + 60 && color.red() > color.blue() + 60;
}

QColor statusColor(const QLabel* label)
{
    return label->palette().color(QPalette::WindowText);
}

QString sha256Hex(const QByteArray& bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

/// Single-quoted shell word ('' escaping), like the worker's shell fallback.
QString shellQuote(const QString& path)
{
    QString quoted = path;
    quoted.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + quoted + QLatin1Char('\'');
}

/// The colour in the middle of a tab's 10x10 state dot.
QColor tabDotColor(const QTabWidget* tabWidget, int index)
{
    const QImage image = tabWidget->tabIcon(index).pixmap(QSize(10, 10)).toImage();
    if (image.isNull()) {
        return QColor();
    }
    return image.pixelColor(image.width() / 2, image.height() / 2);
}

bool writeFile(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    return file.write(bytes) == bytes.size();
}

QByteArray readFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

QByteArray randomBytes(qsizetype size)
{
    QByteArray bytes(size, Qt::Uninitialized);
    QRandomGenerator::global()->fillRange(reinterpret_cast<quint32*>(bytes.data()), size / qsizetype(sizeof(quint32)));
    return bytes;
}

/// Non-empty lines of a known_hosts file, trimmed.
QStringList knownHostsLines(const QString& path)
{
    QStringList lines;
    const QList<QByteArray> raw = readFile(path).split('\n');
    for (const QByteArray& line : raw) {
        const QString text = QString::fromUtf8(line).trimmed();
        if (!text.isEmpty()) {
            lines.append(text);
        }
    }
    return lines;
}

/// "test@127.0.0.1:<port>" - what the user types and what the tab / status bar show.
QString targetOf(const TestSshServer& server)
{
    return QStringLiteral("%1@127.0.0.1:%2").arg(kUser).arg(server.port());
}

/// Answers the modal dialogs of a connect from inside their exec() loops, the way a user
/// would: HostKeyDialog -> "Connect and remember" (or "Connect once"), AuthPromptDialog -> the
/// password typed into the response field + OK, QMessageBox -> the given standard button,
/// SshProfilesDialog -> the handler the test supplies (once; rejected without one). Records
/// what it saw so the test asserts afterwards. Polls with a timer, which keeps running inside
/// nested event loops. Every action closes its dialog synchronously, so a dialog is handled once.
class DialogResponder : public QObject
{
public:
    explicit DialogResponder(QObject* parent = nullptr)
        : QObject(parent)
    {
        m_timer.setInterval(25);
        connect(&m_timer, &QTimer::timeout, this, [this]() { poll(); });
        m_timer.start();
    }

    void stop() { m_timer.stop(); }

    // ---- configuration ----
    QString password = kPassword;
    bool rememberHostKey = true;
    bool rememberSecret = false;     ///< tick the Remember box of every AuthPromptDialog that shows one
    QMessageBox::StandardButton messageAnswer = QMessageBox::Yes;
    std::function<void(SshProfilesDialog*)> profilesHandler;

    // ---- observations ----
    int hostKeyDialogs = 0;
    int authDialogs = 0;
    int messageBoxes = 0;
    int profileDialogs = 0;
    QStringList hostKeyHosts;        ///< labelHost per HostKeyDialog ("127.0.0.1:port")
    QStringList hostKeyHeadlines;
    QStringList hostKeyFiles;        ///< labelKnownHosts per HostKeyDialog ("known_hosts file: <path>")
    QStringList authTargets;         ///< labelTarget per AuthPromptDialog ("test@127.0.0.1")
    QList<bool> authRememberVisible; ///< the "Remember" box per AuthPromptDialog
    QStringList authRememberLabels;  ///< its caption per AuthPromptDialog ("Remember password for test@127.0.0.1:port")
    QList<bool> authMasked;          ///< password echo mode per AuthPromptDialog
    QStringList messageTitles;
    QStringList unexpected;          ///< class names of dialogs nothing here expected (rejected)

private:
    void poll()
    {
        if (m_busy) {
            return;
        }
        QWidget* modal = QApplication::activeModalWidget();
        if (!modal || !modal->isVisible()) {
            return;
        }
        m_busy = true;
        if (auto* hostKey = qobject_cast<HostKeyDialog*>(modal)) {
            ++hostKeyDialogs;
            auto* host = child<QLabel>(hostKey, "labelHost");
            auto* headline = child<QLabel>(hostKey, "labelHeadline");
            auto* file = child<QLabel>(hostKey, "labelKnownHosts");
            auto* button = child<QPushButton>(hostKey, rememberHostKey ? "buttonRemember" : "buttonOnce");
            hostKeyHosts.append(host ? host->text() : QString());
            hostKeyHeadlines.append(headline ? headline->text() : QString());
            hostKeyFiles.append(file ? file->text() : QString());
            if (button) {
                button->click();
            } else {
                unexpected.append(QStringLiteral("HostKeyDialog without its buttons"));
                hostKey->reject();
            }
        } else if (auto* auth = qobject_cast<AuthPromptDialog*>(modal)) {
            ++authDialogs;
            auto* edit = child<QLineEdit>(auth, "editResponse");
            auto* remember = child<QCheckBox>(auth, "checkRemember");
            auto* target = child<QLabel>(auth, "labelTarget");
            auto* buttons = child<QDialogButtonBox>(auth, "buttonBox");
            authTargets.append(target ? target->text() : QString());
            authRememberVisible.append(remember && remember->isVisible());
            authRememberLabels.append(remember ? remember->text() : QString());
            authMasked.append(edit && edit->echoMode() == QLineEdit::Password);
            if (edit && buttons && buttons->button(QDialogButtonBox::Ok)) {
                if (rememberSecret && remember && remember->isVisible()) {
                    remember->setChecked(true);
                }
                QTest::keyClicks(edit, password);
                buttons->button(QDialogButtonBox::Ok)->click();
            } else {
                unexpected.append(QStringLiteral("AuthPromptDialog without its fields"));
                auth->reject();
            }
        } else if (auto* box = qobject_cast<QMessageBox*>(modal)) {
            ++messageBoxes;
            messageTitles.append(box->windowTitle());
            if (QAbstractButton* button = box->button(messageAnswer)) {
                button->click();
            } else {
                box->reject();
            }
        } else if (auto* profiles = qobject_cast<SshProfilesDialog*>(modal)) {
            ++profileDialogs;
            if (profilesHandler && profileDialogs == 1) {
                profilesHandler(profiles);
            } else {
                unexpected.append(QStringLiteral("SshProfilesDialog"));
                profiles->reject();
            }
        } else {
            unexpected.append(QString::fromLatin1(modal->metaObject()->className()));
            if (auto* dialog = qobject_cast<QDialog*>(modal)) {
                dialog->reject();
            } else {
                modal->close();
            }
        }
        m_busy = false;
    }

    QTimer m_timer;
    bool m_busy = false;
};

} // namespace

class Tst_sshsession : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();
    void cleanupTestCase();

    // ---- ad-hoc targets typed into the bar ------------------------------------------
    void adHocEnterConnectTransferDisconnect();
    void adHocEnterOnComboWidget();
    void adHocConnectButtonAndShellExit();
    void adHocRememberPasswordSkipsPrompt();

    // ---- stored profiles --------------------------------------------------------------
    void storedProfileFromDialog();
    void sessionRestoreAfterClose();

    // ---- file transfer through RemoteFileDialog ---------------------------------------
    void transferDialogEdgeCases();
    void transferWithoutSftpUsesShell();
    void transferMethodSftpVsShell();
    void probeRealServerTransfers();

    // ---- links dropping, other tabs, language, quitting -------------------------------
    void autoReconnectAfterDrop();
    void serialTabKeepsWorking();
    void languageSwitchWithSshTab();
    void closeWindowWhileConnected();

private:
    std::unique_ptr<TestSshServer> startServer(const QString& name, const QString& password = kPassword,
                                               bool allowSftp = true);
    /// sha256sum through the shell of the session itself (no WSL / sudo plumbing): true when a
    /// line starting with `sha256` appears; false at once on "No such file" / "Permission denied".
    bool remoteHashMatches(SessionWidget* session, const QString& remotePath, const QString& sha256);
    /// Run a command in the real shell and wait for its prompt to come back.
    bool realShellCommand(SessionWidget* session, const QString& command);
    QString tempPath(const QString& name) const;
    /// A new, absent known_hosts file made the AppSettings default (ad-hoc targets and profiles
    /// without their own file use it).
    QString freshKnownHosts(const QString& name);
    bool showAndActivate(MainWindow& w);
    /// A stored profile for `server` in the window's store, its password in SecretStore and the
    /// server's key in the profile's own temporary known_hosts: it connects without a question.
    SshProfile storeSilentProfile(MainWindow& w, const TestSshServer& server, const QString& name,
                                  int keepAliveSeconds = 0);
    /// Session > Connect on the current (SSH) tab, then wait for the shell prompt.
    bool connectFromAction(MainWindow& w, SessionWidget* session);
    /// Type a line into the terminal and wait for the scripted shell's answer line.
    bool shellRoundTrip(SessionWidget* session, const QString& text, const QString& expectedLine);

    QTemporaryDir m_tempDir;
    int m_knownHostsCounter = 0;
};

// =======================================================================================
// Fixture
// =======================================================================================

void Tst_sshsession::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("BuildAI-Test"));
    QCoreApplication::setApplicationName(QStringLiteral("SerialUtilityTest-sshsession"));
    QVERIFY(m_tempDir.isValid());

    // AppSettings / SecretStore / the transfer dialog use the default QSettings(): keep them out
    // of the registry and inside the temporary directory.
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_tempDir.filePath(QStringLiteral("settings")));
    QSettings settings;
    settings.clear();
    QVERIFY(settings.fileName().contains(QStringLiteral("BuildAI-Test")));
    QVERIFY(settings.fileName().contains(QStringLiteral("SerialUtilityTest-sshsession")));

    // The window's profile store (and history / quick commands) go to the test-mode data
    // directory, never the real one.
    QVERIFY2(SshProfileStore::defaultFilePath().contains(QStringLiteral("SerialUtilityTest-sshsession")),
             qPrintable(SshProfileStore::defaultFilePath()));
    QVERIFY(!AppSettings::dataDirectory().contains(QStringLiteral("/BuildAI/SerialUtility")));

    // ~/.ssh/config must not change a connect (a developer's Host blocks); see SshWorker.
    qputenv("SU_SSH_IGNORE_CONFIG", "1");
}

void Tst_sshsession::init()
{
    QSettings().clear();   // includes the saved secrets and the transfer dialog's paths
    QFile::remove(SshProfileStore::defaultFilePath());
    AppSettings& app = AppSettings::instance();
    app.setLanguage(QStringLiteral("en_US"));
    app.setLogDirectory(tempPath(QStringLiteral("logs")));
    app.setShowSimulatedPorts(true);
    app.setAutoLog(false);
    app.setConfirmCloseWhenConnected(true);
    app.setRestoreLastPorts(true);
    app.setAutoReconnect(true);
    app.setReconnectIntervalMs(200);
    app.setSshKnownHostsFile(freshKnownHosts(QStringLiteral("default")));
}

void Tst_sshsession::cleanup()
{
    // Sessions closed with deleteLater() must be gone before the next window is built.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

void Tst_sshsession::cleanupTestCase()
{
    QFile::remove(SshProfileStore::defaultFilePath());
}

std::unique_ptr<TestSshServer> Tst_sshsession::startServer(const QString& name, const QString& password,
                                                           bool allowSftp)
{
    TestSshServer::Options options;
    options.user = kUser;
    options.password = password;
    options.allowSftp = allowSftp;
    options.rootDir = tempPath(name + QStringLiteral("/root"));
    if (!QDir().mkpath(options.rootDir)) {
        return nullptr;
    }
    auto server = std::make_unique<TestSshServer>(options);
    QString error;
    if (!server->start(&error)) {
        qWarning() << "test SSH server did not start:" << error;
        return nullptr;
    }
    return server;
}

QString Tst_sshsession::tempPath(const QString& name) const
{
    return m_tempDir.filePath(name);
}

QString Tst_sshsession::freshKnownHosts(const QString& name)
{
    const QString path = tempPath(QStringLiteral("known_hosts/%1-%2").arg(name).arg(++m_knownHostsCounter));
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile::remove(path);
    AppSettings::instance().setSshKnownHostsFile(path);
    return path;
}

bool Tst_sshsession::showAndActivate(MainWindow& w)
{
    w.resize(1200, 800);   // wide enough that no system line of the tests wraps
    w.show();
    if (!QTest::qWaitForWindowExposed(&w)) {
        qWarning() << "main window not exposed";
        return false;
    }
    w.activateWindow();
    if (!QTest::qWaitFor([&w]() { return QApplication::activeWindow() == &w; }, 5000)) {
        qWarning() << "main window not activated";
        return false;
    }
    return true;
}

SshProfile Tst_sshsession::storeSilentProfile(MainWindow& w, const TestSshServer& server, const QString& name,
                                              int keepAliveSeconds)
{
    auto* store = w.findChild<SshProfileStore*>();
    if (!store) {
        qWarning() << "the window has no SshProfileStore";
        return {};
    }
    SshProfile p;
    p.name = name;
    p.host = QStringLiteral("127.0.0.1");
    p.port = server.port();
    p.user = kUser;
    p.auth = SshProfile::Auth::Password;
    p.passwordSaved = true;
    p.keepAliveSeconds = keepAliveSeconds;
    p.connectTimeoutSeconds = 5;
    p.knownHostsFile = tempPath(QStringLiteral("profiles/%1/known_hosts").arg(name.toLower().replace(QLatin1Char(' '), QLatin1Char('_'))));
    if (!writeFile(p.knownHostsFile, (server.knownHostsLine() + QLatin1Char('\n')).toUtf8())) {
        qWarning() << "cannot seed" << p.knownHostsFile;
        return {};
    }
    const SshProfile stored = store->upsert(p);
    if (!SecretStore::store(QStringLiteral("ssh/%1/password").arg(stored.id), kPassword)) {
        qWarning() << "SecretStore refused the password";
        return {};
    }
    return stored;
}

bool Tst_sshsession::connectFromAction(MainWindow& w, SessionWidget* session)
{
    if (!session || w.currentSession() != session || !session->isSsh()) {
        qWarning() << "the session is not the current SSH tab";
        return false;
    }
    QAction* connectAction = action(w, "actionConnect");
    if (!connectAction || !connectAction->isEnabled()) {
        qWarning() << "actionConnect missing or disabled";
        return false;
    }
    connectAction->trigger();
    if (session->transport()->state() != State::Connecting) {
        qWarning() << "connect did not start:" << Transport::stateText(session->transport()->state());
        return false;
    }
    if (!QTest::qWaitFor([session]() { return session->transport()->state() == State::Connected; }, kSshTimeoutMs)) {
        qWarning() << "not connected:" << session->transport()->errorString();
        return false;
    }
    if (!QTest::qWaitFor([session]() { return lastNonBlankLine(session->terminal()) == QStringLiteral("$"); },
                         kSshTimeoutMs)) {
        qWarning() << "no shell prompt:" << allText(session->terminal());
        return false;
    }
    return true;
}

bool Tst_sshsession::shellRoundTrip(SessionWidget* session, const QString& text, const QString& expectedLine)
{
    TerminalWidget* terminal = session->terminal();
    const int before = countLines(terminal, expectedLine);
    session->focusTerminal();
    QTest::keyClicks(terminal, text);
    QTest::keyClick(terminal, Qt::Key_Return);
    if (!QTest::qWaitFor([terminal, expectedLine, before]() { return countLines(terminal, expectedLine) > before; },
                         kSshTimeoutMs)) {
        qWarning() << "no" << expectedLine << "line after typing" << text << ":" << allText(terminal);
        return false;
    }
    // The prompt is back, so the next command may be typed.
    return QTest::qWaitFor([terminal]() { return lastNonBlankLine(terminal) == QStringLiteral("$"); }, kSshTimeoutMs);
}

bool Tst_sshsession::realShellCommand(SessionWidget* session, const QString& command)
{
    TerminalWidget* terminal = session->terminal();
    // The visible grid keeps its row count until it scrolls, so line counts say nothing: wait
    // for the echo (on the old prompt line) and then for a fresh prompt line below it.
    const QString marker = command.left(30);
    const int echoBefore = countLinesContaining(terminal, marker);
    session->sendBytes(command.toUtf8() + "\r");
    if (!QTest::qWaitFor([terminal, marker, echoBefore]() {
            return countLinesContaining(terminal, marker) > echoBefore && realPromptShown(terminal) &&
                   !lastNonBlankLine(terminal).contains(marker);
        }, kProbeTimeoutMs)) {
        qWarning() << "no prompt after" << command << ":" << allText(terminal).right(1500);
        return false;
    }
    return true;
}

bool Tst_sshsession::remoteHashMatches(SessionWidget* session, const QString& remotePath, const QString& sha256)
{
    TerminalWidget* terminal = session->terminal();
    const int before = countLinesStartingWith(terminal, sha256);
    const int errorsBefore = countLinesContaining(terminal, QStringLiteral("No such file")) +
                             countLinesContaining(terminal, QStringLiteral("Permission denied"));
    session->sendBytes(QStringLiteral("sha256sum %1\r").arg(shellQuote(remotePath)).toUtf8());
    bool matched = false;
    bool failed = false;
    const bool answered = QTest::qWaitFor([&]() {
        matched = countLinesStartingWith(terminal, sha256) > before;
        failed = (countLinesContaining(terminal, QStringLiteral("No such file")) +
                  countLinesContaining(terminal, QStringLiteral("Permission denied"))) > errorsBefore;
        return matched || failed;
    }, kProbeTimeoutMs);
    if (!answered || !matched) {
        qWarning() << "remote hash of" << remotePath << "does not match" << sha256 << ":" << allText(terminal).right(1500);
        return false;
    }
    // Let the prompt return before the next command.
    return QTest::qWaitFor([terminal]() { return realPromptShown(terminal); }, kProbeTimeoutMs);
}

// =======================================================================================
// Ad-hoc targets typed into the bar
// =======================================================================================

void Tst_sshsession::adHocEnterConnectTransferDisconnect()
{
    auto server = startServer(QStringLiteral("adhoc-enter"));
    QVERIFY(server);
    const QString target = targetOf(*server);
    const QString knownHosts = freshKnownHosts(QStringLiteral("adhoc-enter"));
    const QString rootDir = tempPath(QStringLiteral("adhoc-enter/root"));

    MainWindow w;
    QVERIFY(showAndActivate(w));
    QCOMPARE(w.sessionCount(), 1);

    // File > New SSH Session: an SSH tab with the target field focused.
    action(w, "actionNewSshSession")->trigger();
    QCOMPARE(w.sessionCount(), 2);
    SessionWidget* ssh = w.currentSession();
    QVERIFY(ssh && ssh->isSsh());
    const int tab = tabs(w)->indexOf(ssh);
    QCOMPARE(tab, 1);
    QCOMPARE(tabs(w)->tabText(tab), QStringLiteral("New SSH Session"));
    SshConnectionBar* bar = ssh->sshConnectionBar();
    QVERIFY(bar);
    auto* combo = child<QComboBox>(bar, "targetCombo");
    QVERIFY(combo && combo->lineEdit());
    QLineEdit* edit = combo->lineEdit();
    QTRY_VERIFY(QApplication::focusWidget() != nullptr);
    QVERIFY2(bar->isAncestorOf(QApplication::focusWidget()), QApplication::focusWidget()->metaObject()->className());

    QTest::keyClicks(edit, target);
    QCOMPARE(bar->targetText(), target);
    QVERIFY(bar->hasValidTarget());

    SshConnection* conn = ssh->sshConnection();
    QSignalSpy hostKeySpy(conn, &SshConnection::hostKeyVerificationRequired);
    QSignalSpy promptSpy(conn, &SshConnection::authPromptRequired);
    QSignalSpy authSpy(conn, &SshConnection::authenticated);
    QSignalSpy stateSpy(ssh, &SessionWidget::connectionStateChanged);
    QSignalSpy connectSpy(bar, &SshConnectionBar::connectRequested);
    DialogResponder responder;
    responder.rememberHostKey = true;

    // Enter in the line edit = Connect (exactly once, although the key travels on to the combo):
    // the host key question, then the password prompt, then the shell.
    QTest::keyClick(edit, Qt::Key_Return);
    QCOMPARE(connectSpy.count(), 1);
    QCOMPARE(ssh->transport()->state(), State::Connecting);
    auto* connectButton = child<QPushButton>(bar, "connectButton");
    QVERIFY(connectButton);
    QCOMPARE(connectButton->text(), QStringLiteral("Connecting..."));
    QVERIFY(!combo->isEnabled());
    QVERIFY(!action(w, "actionConnect")->isEnabled());
    QVERIFY(action(w, "actionDisconnect")->isEnabled());
    QCOMPARE(tabDotColor(tabs(w), tab), kDotBusy);

    QTRY_COMPARE_WITH_TIMEOUT(ssh->transport()->state(), State::Connected, kSshTimeoutMs);
    QCOMPARE(responder.hostKeyDialogs, 1);
    QCOMPARE(responder.authDialogs, 1);
    QCOMPARE(responder.messageBoxes, 0);
    QVERIFY2(responder.unexpected.isEmpty(), qPrintable(responder.unexpected.join(QStringLiteral(", "))));
    QCOMPARE(responder.hostKeyHosts, QStringList{QStringLiteral("127.0.0.1:%1").arg(server->port())});
    QVERIFY2(responder.hostKeyHeadlines.first().contains(QStringLiteral("can't be established")),
             qPrintable(responder.hostKeyHeadlines.first()));
    QCOMPARE(responder.authTargets, QStringList{QStringLiteral("test@127.0.0.1")});
    // v0.4 contract (SshConnection.h): an ad-hoc target can be remembered too, under the target.
    QCOMPARE(responder.authRememberVisible, QList<bool>{true});
    QCOMPARE(responder.authRememberLabels, QStringList{QStringLiteral("Remember password for %1").arg(target)});
    QCOMPARE(responder.authMasked, QList<bool>{true});
    QCOMPARE(hostKeySpy.count(), 1);
    const auto hostKeyInfo = hostKeySpy.at(0).at(0).value<SshConnection::HostKeyInfo>();
    QCOMPARE(hostKeyInfo.status, SshConnection::HostKeyStatus::Unknown);
    // Preferences > SSH names the known_hosts file of an ad-hoc connect: the temporary one, never
    // ~/.ssh/known_hosts; the dialog told the user so.
    QCOMPARE(hostKeyInfo.knownHostsFile, QFileInfo(knownHosts).absoluteFilePath());
    QCOMPARE(ssh->sshConnection()->profile().knownHostsFile, knownHosts);
    QVERIFY2(responder.hostKeyFiles.first().contains(QDir::toNativeSeparators(knownHosts)),
             qPrintable(responder.hostKeyFiles.first()));
    QCOMPARE(promptSpy.count(), 1);
    const auto prompt = promptSpy.at(0).at(0).value<SshConnection::AuthPrompt>();
    QCOMPARE(prompt.kind, SshConnection::PromptKind::Password);
    QVERIFY(prompt.canRemember);                 // v0.4 contract
    QCOMPARE(prompt.rememberTarget, target);     // "user@host:port"
    QCOMPARE(authSpy.count(), 1);
    QCOMPARE(authSpy.at(0).at(0).toString(), QStringLiteral("password"));
    // "Connect and remember" wrote exactly the server's line into the temporary file.
    QCOMPARE(knownHostsLines(knownHosts), QStringList{server->knownHostsLine()});

    // The shell greets, the connect line is shown, typing works.
    TerminalWidget* terminal = ssh->terminal();
    QTRY_VERIFY2_WITH_TIMEOUT(allLines(terminal).contains(QStringLiteral("welcome")), qPrintable(allText(terminal)),
                              kSshTimeoutMs);
    QTRY_COMPARE_WITH_TIMEOUT(lastNonBlankLine(terminal), QStringLiteral("$"), kSshTimeoutMs);
    QVERIFY2(allText(terminal).contains(QStringLiteral("--- connected to %1 (ssh-ed25519, password) ---").arg(target)),
             qPrintable(allText(terminal)));
    QVERIFY(terminal->inputEnabled());
    QVERIFY(shellRoundTrip(ssh, QStringLiteral("echo hello"), QStringLiteral("hello")));
    QTRY_VERIFY_WITH_TIMEOUT(server->receivedShellInput().contains("echo hello\r"), kSshTimeoutMs);

    // Status bar, tab, window title, bar.
    QLabel* connection = statusConnectionLabel(w);
    QVERIFY(connection);
    QCOMPARE(connection->text(), QStringLiteral("Connected   %1 · ssh-ed25519 · password").arg(target));
    QCOMPARE(tabs(w)->tabText(tab), target);
    QCOMPARE(tabDotColor(tabs(w), tab), kDotConnected);
    QVERIFY2(tabs(w)->tabToolTip(tab).contains(target), qPrintable(tabs(w)->tabToolTip(tab)));
    QVERIFY2(tabs(w)->tabToolTip(tab).contains(QStringLiteral("Connected")), qPrintable(tabs(w)->tabToolTip(tab)));
    QVERIFY2(w.windowTitle().endsWith(QStringLiteral(" - ") + target), qPrintable(w.windowTitle()));
    QCOMPARE(connectButton->text(), QStringLiteral("Disconnect"));
    auto* info = child<QLabel>(bar, "infoLabel");
    QVERIFY(info && info->isVisible());
    QCOMPARE(info->text(), QStringLiteral("ssh-ed25519 · password"));
    QCOMPARE(ssh->portName(), kTargetKeyPrefix + target);
    QCOMPARE(AppSettings::instance().lastSshTarget(), kTargetKeyPrefix + target);
    QVERIFY(action(w, "actionUploadFile")->isEnabled());
    QVERIFY(action(w, "actionDownloadFile")->isEnabled());
    QVERIFY(action(w, "actionSendFile")->isEnabled());
    QVERIFY(action(w, "actionPaste")->isEnabled());
    QVERIFY(action(w, "actionSyncTerminalSize")->isEnabled());
    QVERIFY(!action(w, "actionSendBreak")->isEnabled());
    QVERIFY(!action(w, "actionRefreshPorts")->isEnabled());
    QVERIFY(!action(w, "actionConnect")->isEnabled());
    QVERIFY(action(w, "actionDisconnect")->isEnabled());

    // Session > Upload File to Remote: the dialog proposes <remote home>/<file name>, Start
    // uploads, the file lands in the server root byte for byte.
    const QByteArray payload = randomBytes(256 * 1024);
    const QString localUp = tempPath(QStringLiteral("adhoc-enter/local/up.bin"));
    QVERIFY(writeFile(localUp, payload));
    action(w, "actionUploadFile")->trigger();
    auto* dialog = ssh->findChild<RemoteFileDialog*>();
    QVERIFY(dialog);
    QTRY_VERIFY(dialog->isVisible());
    QVERIFY(!dialog->isModal());
    QVERIFY(child<QRadioButton>(dialog, "radioUpload")->isChecked());
    auto* start = child<QPushButton>(dialog, "buttonStart");
    auto* remotePath = child<QLineEdit>(dialog, "editRemotePath");
    auto* status = child<QLabel>(dialog, "labelStatus");
    QVERIFY(start && remotePath && status);
    QCOMPARE(QString(start->text()).remove(QLatin1Char('&')), QStringLiteral("Start upload"));
    QVERIFY(!start->isEnabled());   // no paths yet
    QCOMPARE(status->text(), QStringLiteral("Ready."));
    dialog->setLocalPath(localUp);
    QTRY_VERIFY2_WITH_TIMEOUT(remotePath->text().endsWith(QStringLiteral("/up.bin")), qPrintable(remotePath->text()),
                              kSshTimeoutMs);
    const QString derivedDir = remotePath->text().chopped(QStringLiteral("/up.bin").size());
    QCOMPARE(QFileInfo(derivedDir).canonicalFilePath(), QFileInfo(rootDir).canonicalFilePath());
    QTRY_VERIFY(start->isEnabled());
    QSignalSpy finished(dialog, &RemoteFileDialog::transferFinished);
    QSignalSpy started(conn, &SshConnection::transferStarted);
    start->click();
    QVERIFY2(status->text().startsWith(QStringLiteral("Uploading up.bin")), qPrintable(status->text()));
    QVERIFY(!start->isEnabled());
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, kTransferTimeoutMs);
    QVERIFY2(finished.at(0).at(0).toBool(), qPrintable(finished.at(0).at(1).toString()));
    QCOMPARE(started.count(), 1);
    QCOMPARE(readFile(QDir(rootDir).filePath(QStringLiteral("up.bin"))), payload);
    QCOMPARE(child<QProgressBar>(dialog, "progressBar")->value(), 100);
    QVERIFY(start->isEnabled());   // idle again
    QVERIFY(!child<QPushButton>(dialog, "buttonCancel")->isEnabled());
    QVERIFY(!isReddish(statusColor(status)));
    if (!finished.at(0).at(1).toString().isEmpty()) {
        QCOMPARE(status->text(), finished.at(0).at(1).toString());   // the connection's own words
        QCOMPARE(w.statusBar()->currentMessage(), finished.at(0).at(1).toString());
    }

    // Session > Download File from Remote: the same file back, byte for byte.
    const QString localDown = tempPath(QStringLiteral("adhoc-enter/local/down.bin"));
    action(w, "actionDownloadFile")->trigger();
    QVERIFY(dialog->isVisible());
    QVERIFY(child<QRadioButton>(dialog, "radioDownload")->isChecked());
    QCOMPARE(QString(start->text()).remove(QLatin1Char('&')), QStringLiteral("Start download"));
    dialog->setRemotePath(QStringLiteral("up.bin"));   // relative: under the server root
    dialog->setLocalPath(localDown);
    QVERIFY(start->isEnabled());
    start->click();
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 2, kTransferTimeoutMs);
    QVERIFY2(finished.at(1).at(0).toBool(), qPrintable(finished.at(1).at(1).toString()));
    QCOMPARE(readFile(localDown), payload);
    QVERIFY(!QFileInfo::exists(localDown + QStringLiteral(".part")));
    child<QPushButton>(dialog, "buttonClose")->click();
    QVERIFY(!dialog->isVisible());
    // The shell kept working across the transfers.
    QVERIFY(shellRoundTrip(ssh, QStringLiteral("echo after"), QStringLiteral("after")));

    // Session > Disconnect: state, status bar, tab, bar and actions follow.
    action(w, "actionDisconnect")->trigger();
    QCOMPARE(ssh->transport()->state(), State::Disconnected);
    QCOMPARE(w.statusBar()->currentMessage(), QStringLiteral("Disconnected from %1").arg(target));
    QVERIFY(!terminal->inputEnabled());
    QCOMPARE(connectButton->text(), QStringLiteral("Connect"));
    QVERIFY(combo->isEnabled());
    QVERIFY(!info->isVisible());
    QCOMPARE(connection->text(), QStringLiteral("Disconnected   %1 · SSH").arg(target));
    QCOMPARE(tabDotColor(tabs(w), tab), kDotDisconnected);
    QCOMPARE(tabs(w)->tabText(tab), target);
    QVERIFY(action(w, "actionConnect")->isEnabled());
    QVERIFY(!action(w, "actionDisconnect")->isEnabled());
    QVERIFY(!action(w, "actionUploadFile")->isEnabled());
    QVERIFY(!action(w, "actionDownloadFile")->isEnabled());
    QVERIFY(!action(w, "actionPaste")->isEnabled());
    QTRY_COMPARE_WITH_TIMEOUT(server->activeConnections(), 0, kSshTimeoutMs);
    QCOMPARE(server->connectionCount(), 1);

    responder.stop();
    QCOMPARE(responder.hostKeyDialogs, 1);
    QCOMPARE(responder.authDialogs, 1);
    QCOMPARE(responder.messageBoxes, 0);
    QCOMPARE(stateSpy.count(), 3);
    QCOMPARE(stateSpy.at(0).at(0).value<State>(), State::Connecting);
    QCOMPARE(stateSpy.at(1).at(0).value<State>(), State::Connected);
    QCOMPARE(stateSpy.at(2).at(0).value<State>(), State::Disconnected);
}

void Tst_sshsession::adHocEnterOnComboWidget()
{
    // A fresh tab focused through setFocusToTarget() may hold the focus on the combo itself
    // rather than on its line edit (no completer popup without recent targets); Enter delivered
    // to the combo widget must connect exactly once, like Enter in the line edit.
    auto server = startServer(QStringLiteral("adhoc-combo"));
    QVERIFY(server);
    const QString target = targetOf(*server);
    const QString knownHosts = freshKnownHosts(QStringLiteral("adhoc-combo"));

    MainWindow w;
    QVERIFY(showAndActivate(w));
    action(w, "actionNewSshSession")->trigger();
    SessionWidget* ssh = w.currentSession();
    QVERIFY(ssh && ssh->isSsh());
    SshConnectionBar* bar = ssh->sshConnectionBar();
    auto* combo = child<QComboBox>(bar, "targetCombo");
    QVERIFY(combo && combo->lineEdit());
    QTest::keyClicks(combo->lineEdit(), target);
    QVERIFY(bar->hasValidTarget());

    QSignalSpy connectSpy(bar, &SshConnectionBar::connectRequested);
    QSignalSpy stateSpy(ssh, &SessionWidget::connectionStateChanged);
    DialogResponder responder;
    QTest::keyClick(combo, Qt::Key_Return);
    QCOMPARE(connectSpy.count(), 1);
    QCOMPARE(ssh->transport()->state(), State::Connecting);
    // A second press right away while Connecting is a no-op (the bar only connects when idle).
    QTest::keyClick(combo, Qt::Key_Return);
    QCOMPARE(connectSpy.count(), 1);

    QTRY_COMPARE_WITH_TIMEOUT(ssh->transport()->state(), State::Connected, kSshTimeoutMs);
    QCOMPARE(responder.hostKeyDialogs, 1);
    QCOMPARE(responder.authDialogs, 1);
    QVERIFY2(responder.unexpected.isEmpty(), qPrintable(responder.unexpected.join(QStringLiteral(", "))));
    QCOMPARE(knownHostsLines(knownHosts), QStringList{server->knownHostsLine()});
    QTRY_COMPARE_WITH_TIMEOUT(lastNonBlankLine(ssh->terminal()), QStringLiteral("$"), kSshTimeoutMs);
    QVERIFY(shellRoundTrip(ssh, QStringLiteral("echo combo"), QStringLiteral("combo")));
    QCOMPARE(server->connectionCount(), 1);
    QCOMPARE(connectSpy.count(), 1);
    QCOMPARE(stateSpy.count(), 2);   // Connecting, Connected: one connect

    // Enter while connected connects nothing either.
    QTest::keyClick(combo, Qt::Key_Return);
    QTest::keyClick(combo->lineEdit(), Qt::Key_Return);
    QCOMPARE(connectSpy.count(), 1);

    action(w, "actionDisconnect")->trigger();
    QCOMPARE(ssh->transport()->state(), State::Disconnected);
    QTRY_COMPARE_WITH_TIMEOUT(server->activeConnections(), 0, kSshTimeoutMs);
    // Disconnected again, a later Enter (past the 150 ms guard) connects once more without a
    // question: the key is remembered and no profile id means the password is asked again.
    responder.rememberHostKey = true;
    QTest::qWait(200);
    QTest::keyClick(combo, Qt::Key_Return);
    QCOMPARE(connectSpy.count(), 2);
    QTRY_COMPARE_WITH_TIMEOUT(ssh->transport()->state(), State::Connected, kSshTimeoutMs);
    QCOMPARE(responder.hostKeyDialogs, 1);   // remembered
    QCOMPARE(responder.authDialogs, 2);      // Remember was left unchecked: asked again
    QCOMPARE(server->connectionCount(), 2);
    action(w, "actionDisconnect")->trigger();
    QTRY_COMPARE_WITH_TIMEOUT(server->activeConnections(), 0, kSshTimeoutMs);
    responder.stop();
}

void Tst_sshsession::adHocConnectButtonAndShellExit()
{
    auto server = startServer(QStringLiteral("adhoc-click"));
    QVERIFY(server);
    const QString target = targetOf(*server);
    const QString knownHosts = freshKnownHosts(QStringLiteral("adhoc-click"));

    MainWindow w;
    QVERIFY(showAndActivate(w));
    action(w, "actionNewSshSession")->trigger();
    SessionWidget* ssh = w.currentSession();
    QVERIFY(ssh && ssh->isSsh());
    SshConnectionBar* bar = ssh->sshConnectionBar();
    auto* combo = child<QComboBox>(bar, "targetCombo");
    auto* connectButton = child<QPushButton>(bar, "connectButton");
    QVERIFY(combo && combo->lineEdit() && connectButton);
    QTest::keyClicks(combo->lineEdit(), target);

    SshConnection* conn = ssh->sshConnection();
    QSignalSpy hostKeySpy(conn, &SshConnection::hostKeyVerificationRequired);
    QSignalSpy promptSpy(conn, &SshConnection::authPromptRequired);
    QSignalSpy closedSpy(conn, &SshConnection::channelClosed);
    QSignalSpy lostSpy(ssh->transport(), &Transport::connectionLost);
    QSignalSpy stateSpy(ssh, &SessionWidget::connectionStateChanged);
    DialogResponder responder;
    responder.rememberHostKey = false;   // "Connect once" this time

    // The Connect button of the bar starts the same sequence as Enter.
    QTest::mouseClick(connectButton, Qt::LeftButton);
    QCOMPARE(ssh->transport()->state(), State::Connecting);
    QTRY_COMPARE_WITH_TIMEOUT(ssh->transport()->state(), State::Connected, kSshTimeoutMs);
    QCOMPARE(responder.hostKeyDialogs, 1);
    QCOMPARE(responder.authDialogs, 1);
    QCOMPARE(responder.authRememberVisible, QList<bool>{true});   // v0.4: offered for ad-hoc targets too
    QVERIFY2(responder.unexpected.isEmpty(), qPrintable(responder.unexpected.join(QStringLiteral(", "))));
    QCOMPARE(hostKeySpy.count(), 1);
    QCOMPARE(promptSpy.count(), 1);
    QVERIFY(!QFileInfo::exists(knownHosts));   // "Connect once" leaves known_hosts alone
    TerminalWidget* terminal = ssh->terminal();
    QTRY_VERIFY_WITH_TIMEOUT(allLines(terminal).contains(QStringLiteral("welcome")), kSshTimeoutMs);
    QTRY_COMPARE_WITH_TIMEOUT(lastNonBlankLine(terminal), QStringLiteral("$"), kSshTimeoutMs);
    QCOMPARE(statusConnectionLabel(w)->text(), QStringLiteral("Connected   %1 · ssh-ed25519 · password").arg(target));
    QVERIFY(shellRoundTrip(ssh, QStringLiteral("echo clicked"), QStringLiteral("clicked")));

    // "exit" typed into the shell ends the session cleanly: the exit status is shown, the tab
    // is Disconnected and no reconnect is attempted.
    QTest::keyClicks(terminal, QStringLiteral("exit"));
    QTest::keyClick(terminal, Qt::Key_Return);
    QTRY_COMPARE_WITH_TIMEOUT(closedSpy.count(), 1, kSshTimeoutMs);
    QCOMPARE(closedSpy.at(0).at(0).toInt(), 0);
    QTRY_COMPARE_WITH_TIMEOUT(ssh->transport()->state(), State::Disconnected, kSshTimeoutMs);
    QTRY_VERIFY2(allLines(terminal).contains(QStringLiteral("--- connection closed (exit status 0) ---")),
                 qPrintable(allText(terminal)));
    QTest::qWait(3000);
    QCOMPARE(ssh->transport()->state(), State::Disconnected);
    QCOMPARE(lostSpy.count(), 0);
    QCOMPARE(stateSpy.count(), 3);
    QCOMPARE(stateSpy.at(2).at(0).value<State>(), State::Disconnected);
    for (const QList<QVariant>& args : stateSpy) {
        QVERIFY(args.at(0).value<State>() != State::Reconnecting);
    }
    QVERIFY(!terminal->inputEnabled());
    QCOMPARE(connectButton->text(), QStringLiteral("Connect"));
    QVERIFY(combo->isEnabled());
    QCOMPARE(statusConnectionLabel(w)->text(), QStringLiteral("Disconnected   %1 · SSH").arg(target));
    QCOMPARE(tabDotColor(tabs(w), tabs(w)->indexOf(ssh)), kDotDisconnected);
    QVERIFY(action(w, "actionConnect")->isEnabled());
    QVERIFY(!action(w, "actionDisconnect")->isEnabled());
    QVERIFY(!action(w, "actionUploadFile")->isEnabled());
    QTRY_COMPARE_WITH_TIMEOUT(server->activeConnections(), 0, kSshTimeoutMs);
    QCOMPARE(server->connectionCount(), 1);
    responder.stop();
    QCOMPARE(responder.messageBoxes, 0);
}

// =======================================================================================
// Stored profiles
// =======================================================================================

void Tst_sshsession::storedProfileFromDialog()
{
    const QString password = QStringLiteral("s3cret-e2e!");
    auto server = startServer(QStringLiteral("profile"), password);
    QVERIFY(server);
    const QString target = targetOf(*server);
    const QString knownHosts = freshKnownHosts(QStringLiteral("profile"));
    const QString name = QStringLiteral("E2E box");

    MainWindow w;
    QVERIFY(showAndActivate(w));
    auto* store = w.findChild<SshProfileStore*>();
    QVERIFY(store);
    QVERIFY(store->profiles().isEmpty());

    // Edit > SSH Profiles: New, fill in the connection, save the password, Connect.
    bool passwordFieldEnabled = false;
    bool connectEnabled = false;
    bool identityFieldEnabled = true;
    DialogResponder responder;
    responder.password = QStringLiteral("never asked");   // the saved password must be used
    responder.profilesHandler = [&](SshProfilesDialog* dialog) {
        child<QPushButton>(dialog, "buttonNew")->click();
        auto* nameEdit = child<QLineEdit>(dialog, "editName");
        nameEdit->selectAll();
        QTest::keyClicks(nameEdit, name);
        QTest::keyClicks(child<QLineEdit>(dialog, "editHost"), QStringLiteral("127.0.0.1"));
        child<QSpinBox>(dialog, "spinPort")->setValue(server->port());
        QTest::keyClicks(child<QLineEdit>(dialog, "editUser"), kUser);
        auto* auth = child<QComboBox>(dialog, "comboAuth");
        auth->setCurrentIndex(auth->findData(static_cast<int>(SshProfile::Auth::Password)));
        identityFieldEnabled = child<QLineEdit>(dialog, "editIdentityFile")->isEnabled();
        child<QCheckBox>(dialog, "checkSavePassword")->click();
        auto* passwordEdit = child<QLineEdit>(dialog, "editPassword");
        passwordFieldEnabled = passwordEdit->isEnabled();
        QTest::keyClicks(passwordEdit, password);
        auto* connectButton = child<QPushButton>(dialog, "buttonConnect");
        connectEnabled = connectButton->isEnabled();
        connectButton->click();
    };
    action(w, "actionSshProfiles")->trigger();
    QCOMPARE(responder.profileDialogs, 1);
    QVERIFY(!identityFieldEnabled);   // password auth needs no key file
    QVERIFY(passwordFieldEnabled);
    QVERIFY(connectEnabled);

    // "Connect" saved the profile and opened a connecting tab on it.
    QCOMPARE(w.sessionCount(), 2);
    SessionWidget* ssh = w.currentSession();
    QVERIFY(ssh && ssh->isSsh());
    QCOMPARE(ssh->transport()->state(), State::Connecting);
    QCOMPARE(store->profiles().size(), 1);
    const SshProfile stored = store->profiles().first();
    QCOMPARE(stored.name, name);
    QCOMPARE(stored.host, QStringLiteral("127.0.0.1"));
    QCOMPARE(stored.port, server->port());
    QCOMPARE(stored.user, kUser);
    QCOMPARE(stored.auth, SshProfile::Auth::Password);
    QVERIFY(stored.passwordSaved);
    QVERIFY(!stored.id.isEmpty());
    const QString secretKey = QStringLiteral("ssh/%1/password").arg(stored.id);
    QVERIFY(SecretStore::contains(secretKey));
    QCOMPARE(ssh->portName(), kProfileKeyPrefix + stored.id);
    QCOMPARE(ssh->title(), name);
    SshConnection* conn = ssh->sshConnection();
    QSignalSpy hostKeySpy(conn, &SshConnection::hostKeyVerificationRequired);
    QSignalSpy promptSpy(conn, &SshConnection::authPromptRequired);

    // The host key is asked once and remembered; the password comes from SecretStore.
    QTRY_COMPARE_WITH_TIMEOUT(ssh->transport()->state(), State::Connected, kSshTimeoutMs);
    QCOMPARE(responder.hostKeyDialogs, 1);
    QCOMPARE(responder.authDialogs, 0);
    QCOMPARE(responder.messageBoxes, 0);
    QVERIFY2(responder.unexpected.isEmpty(), qPrintable(responder.unexpected.join(QStringLiteral(", "))));
    QCOMPARE(hostKeySpy.count(), 1);
    QCOMPARE(promptSpy.count(), 0);
    // The profile left the known_hosts field empty: Preferences > SSH supplies the temporary file.
    QCOMPARE(hostKeySpy.at(0).at(0).value<SshConnection::HostKeyInfo>().knownHostsFile,
             QFileInfo(knownHosts).absoluteFilePath());
    QCOMPARE(knownHostsLines(knownHosts), QStringList{server->knownHostsLine()});
    TerminalWidget* terminal = ssh->terminal();
    QTRY_VERIFY_WITH_TIMEOUT(allLines(terminal).contains(QStringLiteral("welcome")), kSshTimeoutMs);
    QTRY_COMPARE_WITH_TIMEOUT(lastNonBlankLine(terminal), QStringLiteral("$"), kSshTimeoutMs);
    QCOMPARE(statusConnectionLabel(w)->text(), QStringLiteral("Connected   %1 · ssh-ed25519 · password").arg(name));
    QCOMPARE(tabs(w)->tabText(tabs(w)->indexOf(ssh)), name);
    QVERIFY2(w.windowTitle().endsWith(QStringLiteral(" - ") + name), qPrintable(w.windowTitle()));
    SshConnectionBar* bar = ssh->sshConnectionBar();
    QCOMPARE(bar->currentProfile().id, stored.id);
    QCOMPARE(child<QComboBox>(bar, "targetCombo")->currentText(), QStringLiteral("%1 - %2").arg(name, target));
    QVERIFY(shellRoundTrip(ssh, QStringLiteral("echo stored"), QStringLiteral("stored")));
    QVERIFY(store->profile(stored.id)->lastUsed.isValid());   // touched by the connect

    // Disconnect and reconnect from the bar with the profile still selected: not a single question.
    auto* connectButton = child<QPushButton>(bar, "connectButton");
    QCOMPARE(connectButton->text(), QStringLiteral("Disconnect"));
    QTest::mouseClick(connectButton, Qt::LeftButton);
    QCOMPARE(ssh->transport()->state(), State::Disconnected);
    QTRY_COMPARE_WITH_TIMEOUT(server->activeConnections(), 0, kSshTimeoutMs);
    QCOMPARE(bar->currentProfile().id, stored.id);
    QCOMPARE(connectButton->text(), QStringLiteral("Connect"));
    QTest::mouseClick(connectButton, Qt::LeftButton);
    QCOMPARE(ssh->transport()->state(), State::Connecting);
    QTRY_COMPARE_WITH_TIMEOUT(ssh->transport()->state(), State::Connected, kSshTimeoutMs);
    QCOMPARE(responder.hostKeyDialogs, 1);
    QCOMPARE(responder.authDialogs, 0);
    QCOMPARE(hostKeySpy.count(), 1);
    QCOMPARE(promptSpy.count(), 0);
    QTRY_COMPARE_WITH_TIMEOUT(countLines(terminal, QStringLiteral("welcome")), 2, kSshTimeoutMs);
    QTRY_COMPARE_WITH_TIMEOUT(lastNonBlankLine(terminal), QStringLiteral("$"), kSshTimeoutMs);
    QCOMPARE(server->connectionCount(), 2);
    QVERIFY(shellRoundTrip(ssh, QStringLiteral("echo again"), QStringLiteral("again")));

    // The profile file on disk carries the profile but never the password.
    const QString file = SshProfileStore::defaultFilePath();
    QVERIFY(QFile::exists(file));
    const QByteArray json = readFile(file);
    QVERIFY(!json.isEmpty());
    QVERIFY(json.contains(name.toUtf8()));
    QVERIFY(json.contains(stored.id.toUtf8()));
    QVERIFY(json.contains("\"passwordSaved\": true"));
    QVERIFY(!json.contains(password.toUtf8()));
    QVERIFY(!json.contains("s3cret"));

    action(w, "actionDisconnect")->trigger();
    QCOMPARE(ssh->transport()->state(), State::Disconnected);
    QTRY_COMPARE_WITH_TIMEOUT(server->activeConnections(), 0, kSshTimeoutMs);
    responder.stop();
    QCOMPARE(responder.messageBoxes, 0);
}

void Tst_sshsession::sessionRestoreAfterClose()
{
    auto server = startServer(QStringLiteral("restore"));
    QVERIFY(server);
    const QString name = QStringLiteral("Restore box");
    QString key;
    QString id;
    DialogResponder responder;   // nothing may be asked in this test
    {
        MainWindow first;
        QVERIFY(showAndActivate(first));
        const SshProfile stored = storeSilentProfile(first, *server, name);
        QVERIFY(!stored.id.isEmpty());
        id = stored.id;
        key = kProfileKeyPrefix + id;
        SessionWidget* ssh = first.newSshSession(key);
        QVERIFY(ssh && ssh->isSsh());
        QCOMPARE(first.currentSession(), ssh);
        QCOMPARE(ssh->portName(), key);
        QVERIFY(connectFromAction(first, ssh));
        QCOMPARE(responder.hostKeyDialogs, 0);
        QCOMPARE(responder.authDialogs, 0);

        // The closeEvent path saves the open tabs (the empty serial tab has no key) and shuts
        // the session down.
        AppSettings::instance().setConfirmCloseWhenConnected(false);
        first.close();
        QTRY_VERIFY(!first.isVisible());
        QCOMPARE(ssh->transport()->state(), State::Disconnected);
        QCOMPARE(AppSettings::instance().lastOpenPorts(), QStringList{key});
        QCOMPARE(AppSettings::instance().lastPortName(), key);
        QCOMPARE(AppSettings::instance().lastSshTarget(), key);
        QTRY_COMPARE_WITH_TIMEOUT(server->activeConnections(), 0, kSshTimeoutMs);
    }
    QCOMPARE(server->connectionCount(), 1);
    QVERIFY(QFile::exists(SshProfileStore::defaultFilePath()));

    // The next window brings the SSH tab back on the profile, not connected.
    MainWindow second;
    QVERIFY(showAndActivate(second));
    QCOMPARE(second.sessionCount(), 1);
    SessionWidget* restored = second.currentSession();
    QVERIFY(restored && restored->isSsh());
    QCOMPARE(restored->portName(), key);
    QCOMPARE(restored->title(), name);
    QCOMPARE(tabs(second)->tabText(0), name);
    QCOMPARE(restored->sshConnection()->profile().id, id);
    QCOMPARE(restored->sshConnection()->profile().host, QStringLiteral("127.0.0.1"));
    SshConnectionBar* bar = restored->sshConnectionBar();
    QCOMPARE(bar->currentProfile().id, id);
    QCOMPARE(child<QComboBox>(bar, "targetCombo")->currentText(), QStringLiteral("%1 - %2").arg(name, targetOf(*server)));
    QVERIFY(!restored->isConnected());
    QCOMPARE(restored->transport()->state(), State::Disconnected);
    QVERIFY2(statusConnectionLabel(second)->text().startsWith(QStringLiteral("Disconnected   ") + name),
             qPrintable(statusConnectionLabel(second)->text()));
    QVERIFY2(second.windowTitle().endsWith(QStringLiteral(" - ") + name), qPrintable(second.windowTitle()));
    QCOMPARE(tabDotColor(tabs(second), 0), kDotDisconnected);
    QTest::qWait(300);   // never auto-connecting
    QCOMPARE(server->connectionCount(), 1);
    QCOMPARE(restored->transport()->state(), State::Disconnected);

    // And it connects again from the restored tab without a question.
    QVERIFY(connectFromAction(second, restored));
    QCOMPARE(server->connectionCount(), 2);
    QCOMPARE(responder.hostKeyDialogs, 0);
    QCOMPARE(responder.authDialogs, 0);
    QCOMPARE(responder.messageBoxes, 0);
    QVERIFY2(responder.unexpected.isEmpty(), qPrintable(responder.unexpected.join(QStringLiteral(", "))));
    action(second, "actionDisconnect")->trigger();
    QTRY_COMPARE_WITH_TIMEOUT(server->activeConnections(), 0, kSshTimeoutMs);
    responder.stop();
}

void Tst_sshsession::adHocRememberPasswordSkipsPrompt()
{
    // v0.4: Remember on an ad-hoc target stores the password under
    // "ssh/target/<user@host:port>/password", so the reconnect from the bar needs no prompt.
    // Depends on the ssh-core package: the worker fills AuthPrompt.rememberTarget and honours
    // answerPrompt(..., remember = true) without a profile id.
    auto server = startServer(QStringLiteral("adhoc-remember"));
    QVERIFY(server);
    const QString target = targetOf(*server);
    freshKnownHosts(QStringLiteral("adhoc-remember"));

    MainWindow w;
    QVERIFY(showAndActivate(w));
    action(w, "actionNewSshSession")->trigger();
    SessionWidget* ssh = w.currentSession();
    QVERIFY(ssh && ssh->isSsh());
    SshConnectionBar* bar = ssh->sshConnectionBar();
    auto* combo = child<QComboBox>(bar, "targetCombo");
    auto* connectButton = child<QPushButton>(bar, "connectButton");
    QVERIFY(combo && combo->lineEdit() && connectButton);
    QTest::keyClicks(combo->lineEdit(), target);

    SshConnection* conn = ssh->sshConnection();
    QSignalSpy promptSpy(conn, &SshConnection::authPromptRequired);
    DialogResponder responder;
    responder.rememberSecret = true;   // the user ticks "Remember password for test@127.0.0.1:port"
    QTest::keyClick(combo->lineEdit(), Qt::Key_Return);
    QTRY_COMPARE_WITH_TIMEOUT(ssh->transport()->state(), State::Connected, kSshTimeoutMs);
    QCOMPARE(responder.hostKeyDialogs, 1);
    QCOMPARE(responder.authDialogs, 1);
    QCOMPARE(responder.authRememberVisible, QList<bool>{true});
    QCOMPARE(responder.authRememberLabels, QStringList{QStringLiteral("Remember password for %1").arg(target)});
    QVERIFY2(responder.unexpected.isEmpty(), qPrintable(responder.unexpected.join(QStringLiteral(", "))));
    QCOMPARE(promptSpy.count(), 1);
    const auto prompt = promptSpy.at(0).at(0).value<SshConnection::AuthPrompt>();
    QVERIFY(prompt.canRemember);
    QCOMPARE(prompt.rememberTarget, target);
    const QString secretKey = QStringLiteral("ssh/target/%1/password").arg(target);
    QTRY_VERIFY2(SecretStore::contains(secretKey), qPrintable(QStringLiteral("SecretStore has no ") + secretKey));
    QCOMPARE(SecretStore::load(secretKey).value_or(QString()), kPassword);
    QTRY_COMPARE_WITH_TIMEOUT(lastNonBlankLine(ssh->terminal()), QStringLiteral("$"), kSshTimeoutMs);
    QVERIFY(shellRoundTrip(ssh, QStringLiteral("echo first"), QStringLiteral("first")));

    // Disconnect from the bar, reconnect from the bar: the host key and the password are known.
    QTest::mouseClick(connectButton, Qt::LeftButton);
    QCOMPARE(ssh->transport()->state(), State::Disconnected);
    QTRY_COMPARE_WITH_TIMEOUT(server->activeConnections(), 0, kSshTimeoutMs);
    QCOMPARE(connectButton->text(), QStringLiteral("Connect"));
    QTest::mouseClick(connectButton, Qt::LeftButton);
    QCOMPARE(ssh->transport()->state(), State::Connecting);
    QTRY_COMPARE_WITH_TIMEOUT(ssh->transport()->state(), State::Connected, kSshTimeoutMs);
    QCOMPARE(responder.hostKeyDialogs, 1);
    QCOMPARE(responder.authDialogs, 1);   // not asked again
    QCOMPARE(promptSpy.count(), 1);
    QCOMPARE(server->connectionCount(), 2);
    QCOMPARE(server->lastAuthMethod(), QStringLiteral("password"));
    QTRY_COMPARE_WITH_TIMEOUT(countLines(ssh->terminal(), QStringLiteral("welcome")), 2, kSshTimeoutMs);
    QTRY_COMPARE_WITH_TIMEOUT(lastNonBlankLine(ssh->terminal()), QStringLiteral("$"), kSshTimeoutMs);
    QVERIFY(shellRoundTrip(ssh, QStringLiteral("echo remembered"), QStringLiteral("remembered")));

    // The secret is in SecretStore only: never in the profile store's JSON.
    if (QFile::exists(SshProfileStore::defaultFilePath())) {
        QVERIFY(!readFile(SshProfileStore::defaultFilePath()).contains(kPassword.toUtf8()));
    }

    // The same target from a second tab: the remembered host key and password serve every
    // session to it, so the connect from Session > Connect runs without a single question.
    SessionWidget* second = w.newSshSession(target);
    QVERIFY(second && second != ssh && second->isSsh());
    QCOMPARE(w.currentSession(), second);
    QCOMPARE(second->sshConnectionBar()->currentProfile().displayTarget(), target);
    QSignalSpy secondPrompts(second->sshConnection(), &SshConnection::authPromptRequired);
    QSignalSpy secondHostKeys(second->sshConnection(), &SshConnection::hostKeyVerificationRequired);
    QVERIFY(connectFromAction(w, second));
    QCOMPARE(secondPrompts.count(), 0);
    QCOMPARE(secondHostKeys.count(), 0);
    QCOMPARE(responder.hostKeyDialogs, 1);
    QCOMPARE(responder.authDialogs, 1);
    QCOMPARE(server->connectionCount(), 3);
    QCOMPARE(server->activeConnections(), 2);
    QCOMPARE(server->lastAuthMethod(), QStringLiteral("password"));
    QVERIFY(shellRoundTrip(second, QStringLiteral("echo second"), QStringLiteral("second")));
    QVERIFY(ssh->isConnected());   // the first tab is untouched

    action(w, "actionDisconnect")->trigger();   // the second tab (current)
    QCOMPARE(second->transport()->state(), State::Disconnected);
    tabs(w)->setCurrentWidget(ssh);
    QCOMPARE(w.currentSession(), ssh);
    action(w, "actionDisconnect")->trigger();
    QTRY_COMPARE_WITH_TIMEOUT(server->activeConnections(), 0, kSshTimeoutMs);
    responder.stop();
    QCOMPARE(responder.messageBoxes, 0);
    QVERIFY2(responder.unexpected.isEmpty(), qPrintable(responder.unexpected.join(QStringLiteral(", "))));
}

// =======================================================================================
// File transfer through RemoteFileDialog
// =======================================================================================

void Tst_sshsession::transferDialogEdgeCases()
{
    // The situations behind "upload/download is not working", against the in-process server
    // (SFTP on, real files under its root): a Windows path in the remote field, a directory
    // typed and started without leaving the field, "~", a missing remote file, Cancel half-way,
    // a second transfer without reopening, the drag-and-drop entry, and a disconnect while the
    // modeless dialog is open.
    auto server = startServer(QStringLiteral("edge"));
    QVERIFY(server);
    const QString name = QStringLiteral("Edge box");
    const QString rootDir = tempPath(QStringLiteral("edge/root"));

    MainWindow w;
    QVERIFY(showAndActivate(w));
    const SshProfile stored = storeSilentProfile(w, *server, name);
    QVERIFY(!stored.id.isEmpty());
    SessionWidget* ssh = w.newSshSession(kProfileKeyPrefix + stored.id);
    QVERIFY(connectFromAction(w, ssh));
    SshConnection* conn = ssh->sshConnection();
    DialogResponder responder;   // nothing may be asked (saved password, seeded known_hosts)

    const QByteArray payload = randomBytes(256 * 1024);
    const QString localUp = tempPath(QStringLiteral("edge/local/edge.bin"));
    QVERIFY(writeFile(localUp, payload));
    action(w, "actionUploadFile")->trigger();
    auto* dialog = ssh->findChild<RemoteFileDialog*>();
    QVERIFY(dialog);
    QTRY_VERIFY(dialog->isVisible());
    auto* start = child<QPushButton>(dialog, "buttonStart");
    auto* cancel = child<QPushButton>(dialog, "buttonCancel");
    auto* remote = child<QLineEdit>(dialog, "editRemotePath");
    auto* local = child<QLineEdit>(dialog, "editLocalPath");
    auto* status = child<QLabel>(dialog, "labelStatus");
    QVERIFY(start && cancel && remote && local && status);
    QSignalSpy finished(dialog, &RemoteFileDialog::transferFinished);
    QSignalSpy started(conn, &SshConnection::transferStarted);
    dialog->setLocalPath(localUp);
    QTRY_VERIFY2_WITH_TIMEOUT(remote->text().endsWith(QStringLiteral("/edge.bin")), qPrintable(remote->text()),
                              kSshTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(!conn->remoteHome().isEmpty(), kSshTimeoutMs);
    const QString home = conn->remoteHome();

    // 1. A Windows path in the remote field (the two fields swapped): refused with a readable
    //    line in red, nothing sent, Start still usable.
    remote->clear();
    QTest::keyClicks(remote, QStringLiteral("C:\\boards\\edge.bin"));
    QVERIFY(start->isEnabled());
    start->click();
    QVERIFY2(status->text().contains(QStringLiteral("Windows path")), qPrintable(status->text()));
    QVERIFY(isReddish(statusColor(status)));
    QCOMPARE(started.count(), 0);
    QCOMPARE(finished.count(), 0);
    QVERIFY(start->isEnabled());
    // 1b. A folder as the local file (a build output directory dropped or typed): named as such,
    //     not "not found".
    local->clear();
    QTest::keyClicks(local, QDir::toNativeSeparators(tempPath(QStringLiteral("edge/local"))));
    remote->clear();
    QTest::keyClicks(remote, QStringLiteral("/tmp/"));
    start->click();
    QVERIFY2(status->text().contains(QStringLiteral("is a folder")), qPrintable(status->text()));
    QVERIFY(isReddish(statusColor(status)));
    QCOMPARE(started.count(), 0);
    QVERIFY(start->isEnabled());
    dialog->setLocalPath(localUp);

    // 2. A directory typed and Start clicked without leaving the field (what Alt+S does): the
    //    field and the request both get the file name.
    const QString subDir = rootDir + QStringLiteral("/sub");
    QVERIFY(QDir().mkpath(subDir));
    remote->clear();
    QTest::keyClicks(remote, subDir + QLatin1Char('/'));
    start->click();   // a programmatic click moves no focus: no editingFinished
    QCOMPARE(remote->text(), subDir + QStringLiteral("/edge.bin"));
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, kTransferTimeoutMs);
    QVERIFY2(finished.at(0).at(0).toBool(), qPrintable(finished.at(0).at(1).toString()));
    QCOMPARE(started.count(), 1);
    QCOMPARE(started.at(0).at(0).value<SshConnection::TransferRequest>().remotePath, subDir + QStringLiteral("/edge.bin"));
    QCOMPARE(readFile(subDir + QStringLiteral("/edge.bin")), payload);
    QCOMPARE(status->text(), finished.at(0).at(1).toString());
    QVERIFY(!isReddish(statusColor(status)));
    QVERIFY(start->isEnabled());

    // 3. "~/<name>" goes to the remote home.
    remote->clear();
    QTest::keyClicks(remote, QStringLiteral("~/tilde.bin"));
    start->click();
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 2, kTransferTimeoutMs);
    QVERIFY2(finished.at(1).at(0).toBool(), qPrintable(finished.at(1).at(1).toString()));
    QCOMPARE(started.at(1).at(0).value<SshConnection::TransferRequest>().remotePath, home + QStringLiteral("/tilde.bin"));
    QCOMPARE(readFile(QDir(rootDir).filePath(QStringLiteral("tilde.bin"))), payload);

    // 4. A remote file that does not exist: the connection's message in red, the dialog usable.
    const QString downDir = tempPath(QStringLiteral("edge/down"));
    QVERIFY(QDir().mkpath(downDir));
    dialog->setDirection(SshConnection::TransferDirection::Download);
    dialog->setRemotePath(rootDir + QStringLiteral("/missing.bin"));
    dialog->setLocalPath(downDir + QStringLiteral("/missing.bin"));
    QVERIFY(start->isEnabled());
    start->click();
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 3, kTransferTimeoutMs);
    QVERIFY(!finished.at(2).at(0).toBool());
    const QString missingMessage = finished.at(2).at(1).toString();
    QVERIFY2(missingMessage.contains(QStringLiteral("missing.bin")), qPrintable(missingMessage));
    QCOMPARE(status->text(), missingMessage);
    QVERIFY(isReddish(statusColor(status)));
    QVERIFY(start->isEnabled());
    QVERIFY(!cancel->isEnabled());
    QVERIFY(remote->isEnabled());
    QVERIFY(!QFileInfo::exists(downDir + QStringLiteral("/missing.bin")));
    QVERIFY(!QFileInfo::exists(downDir + QStringLiteral("/missing.bin.part")));
    QCOMPARE(w.statusBar()->currentMessage(), missingMessage);

    // 5. Cancel half-way through a large download: no .part left, the dialog usable at once.
    const QByteArray big = randomBytes(16 * 1024 * 1024);
    QVERIFY(writeFile(QDir(rootDir).filePath(QStringLiteral("big.bin")), big));
    dialog->setRemotePath(rootDir + QStringLiteral("/big.bin"));
    dialog->setLocalPath(downDir + QStringLiteral("/big.bin"));
    bool cancelled = false;
    QObject hook;
    connect(conn, &SshConnection::transferProgress, &hook, [&](qint64 done, qint64 total) {
        if (!cancelled && done > 0 && done < total) {
            cancelled = true;
            cancel->click();
        }
    });
    start->click();
    QVERIFY(cancel->isEnabled());
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 4, kTransferTimeoutMs);
    QObject::disconnect(conn, &SshConnection::transferProgress, &hook, nullptr);
    QVERIFY(cancelled);
    QVERIFY(!finished.at(3).at(0).toBool());
    QVERIFY2(finished.at(3).at(1).toString().contains(QStringLiteral("cancel"), Qt::CaseInsensitive),
             qPrintable(finished.at(3).at(1).toString()));
    QVERIFY(isReddish(statusColor(status)));
    QVERIFY(!QFileInfo::exists(downDir + QStringLiteral("/big.bin.part")));
    QVERIFY(!QFileInfo::exists(downDir + QStringLiteral("/big.bin")));
    QVERIFY(start->isEnabled());
    QVERIFY(!cancel->isEnabled());

    // 6. The same download again, complete this time, in the same dialog.
    start->click();
    QVERIFY2(status->text().startsWith(QStringLiteral("Downloading big.bin")), qPrintable(status->text()));
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 5, kTransferTimeoutMs);
    QVERIFY2(finished.at(4).at(0).toBool(), qPrintable(finished.at(4).at(1).toString()));
    QCOMPARE(readFile(downDir + QStringLiteral("/big.bin")), big);
    QVERIFY(!QFileInfo::exists(downDir + QStringLiteral("/big.bin.part")));
    QCOMPARE(child<QProgressBar>(dialog, "progressBar")->value(), 100);

    // 7. The drag-and-drop entry: uploadFile(path) presets the local file, the remote name
    //    follows it into the directory used last, Start works.
    child<QPushButton>(dialog, "buttonClose")->click();
    QVERIFY(!dialog->isVisible());
    const QString dropped = tempPath(QStringLiteral("edge/local/dropped.bin"));
    QVERIFY(writeFile(dropped, payload));
    ssh->uploadFile(dropped);
    QTRY_VERIFY(dialog->isVisible());
    QVERIFY(child<QRadioButton>(dialog, "radioUpload")->isChecked());
    QCOMPARE(QDir::fromNativeSeparators(local->text()), dropped);
    QTRY_COMPARE(remote->text(), rootDir + QStringLiteral("/dropped.bin"));
    QVERIFY(start->isEnabled());
    start->click();
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 6, kTransferTimeoutMs);
    QVERIFY2(finished.at(5).at(0).toBool(), qPrintable(finished.at(5).at(1).toString()));
    QCOMPARE(readFile(QDir(rootDir).filePath(QStringLiteral("dropped.bin"))), payload);

    // 8. The modeless dialog outlives a disconnect and works again after the reconnect.
    action(w, "actionDisconnect")->trigger();
    QCOMPARE(ssh->transport()->state(), State::Disconnected);
    QTRY_COMPARE(status->text(), QStringLiteral("Not connected."));
    QVERIFY(!start->isEnabled());
    QCOMPARE(start->toolTip(), QStringLiteral("Connect the session first."));
    QVERIFY(dialog->isVisible());
    QTRY_COMPARE_WITH_TIMEOUT(server->activeConnections(), 0, kSshTimeoutMs);
    QVERIFY(connectFromAction(w, ssh));
    QTRY_COMPARE(status->text(), QStringLiteral("Ready."));
    QTRY_VERIFY(start->isEnabled());
    dialog->setRemotePath(rootDir + QStringLiteral("/again.bin"));
    start->click();
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 7, kTransferTimeoutMs);
    QVERIFY2(finished.at(6).at(0).toBool(), qPrintable(finished.at(6).at(1).toString()));
    QCOMPARE(readFile(QDir(rootDir).filePath(QStringLiteral("again.bin"))), payload);
    QCOMPARE(started.count(), 6);   // seven Starts; the missing remote file failed before transferStarted()

    child<QPushButton>(dialog, "buttonClose")->click();
    action(w, "actionDisconnect")->trigger();
    QTRY_COMPARE_WITH_TIMEOUT(server->activeConnections(), 0, kSshTimeoutMs);
    responder.stop();
    QCOMPARE(responder.hostKeyDialogs, 0);
    QCOMPARE(responder.authDialogs, 0);
    QCOMPARE(responder.messageBoxes, 0);
}

void Tst_sshsession::transferWithoutSftpUsesShell()
{
    // A server that refuses the "sftp" subsystem (dropbear on a buildroot board): the transfer
    // goes through the exec-channel fallback and the dialog says so. Depends on the ssh-core
    // package (the SshWorker fallback) and the test-server package (Options::allowSftp).
    auto server = startServer(QStringLiteral("noSftp"), kPassword, /*allowSftp=*/false);
    QVERIFY(server);
    const QString name = QStringLiteral("Dropbear box");
    const QString rootDir = tempPath(QStringLiteral("noSftp/root"));

    MainWindow w;
    QVERIFY(showAndActivate(w));
    const SshProfile stored = storeSilentProfile(w, *server, name);
    QVERIFY(!stored.id.isEmpty());
    SessionWidget* ssh = w.newSshSession(kProfileKeyPrefix + stored.id);
    int sftpRequests = 0;
    QObject sftpHook;
    connect(server.get(), &TestSshServer::sftpRequested, &sftpHook, [&sftpRequests]() { ++sftpRequests; },
            Qt::QueuedConnection);   // emitted on the server's client thread
    QVERIFY(connectFromAction(w, ssh));
    SshConnection* conn = ssh->sshConnection();
    DialogResponder responder;

    const QByteArray payload = randomBytes(256 * 1024);
    const QString localUp = tempPath(QStringLiteral("noSftp/local/shell-up.bin"));
    QVERIFY(writeFile(localUp, payload));
    action(w, "actionUploadFile")->trigger();
    auto* dialog = ssh->findChild<RemoteFileDialog*>();
    QVERIFY(dialog);
    QTRY_VERIFY(dialog->isVisible());
    auto* start = child<QPushButton>(dialog, "buttonStart");
    auto* remote = child<QLineEdit>(dialog, "editRemotePath");
    auto* status = child<QLabel>(dialog, "labelStatus");
    QVERIFY(start && remote && status);
    QSignalSpy finished(dialog, &RemoteFileDialog::transferFinished);
    QStringList methods;
    QStringList statusAtStart;
    QObject hook;
    connect(conn, &SshConnection::transferStarted, &hook, [&](const SshConnection::TransferRequest&) {
        methods.append(conn->transferStatus().method);
        statusAtStart.append(status->text());   // the dialog's slot ran first (connected earlier)
    });

    // No SFTP: when the remote home cannot be resolved the dialog proposes the bare file name
    // (relative to the login directory = the server root) instead of leaving Start disabled; a
    // worker that resolves the home through the shell proposes <home>/<name> as usual.
    dialog->setLocalPath(localUp);
    QTRY_VERIFY2_WITH_TIMEOUT(remote->text().endsWith(QStringLiteral("shell-up.bin")), qPrintable(remote->text()),
                              kSshTimeoutMs);
    qInfo() << "no-SFTP upload destination proposed:" << remote->text();
    QTRY_VERIFY(start->isEnabled());
    start->click();
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, kTransferTimeoutMs);
    QVERIFY2(finished.at(0).at(0).toBool(), qPrintable(finished.at(0).at(1).toString()));
    QCOMPARE(methods, QStringList{QStringLiteral("shell")});
    QCOMPARE(statusAtStart, QStringList{QStringLiteral("Uploading shell-up.bin via shell (cat)...")});
    QVERIFY2(finished.at(0).at(1).toString().contains(QStringLiteral("shell")), qPrintable(finished.at(0).at(1).toString()));
    QCOMPARE(status->text(), finished.at(0).at(1).toString());
    QCOMPARE(readFile(QDir(rootDir).filePath(QStringLiteral("shell-up.bin"))), payload);
    QVERIFY2(server->lastExecCommand().contains(QStringLiteral("shell-up.bin'")), qPrintable(server->lastExecCommand()));

    // Download it back through the shell.
    const QString localDown = tempPath(QStringLiteral("noSftp/local/shell-down.bin"));
    dialog->setDirection(SshConnection::TransferDirection::Download);
    dialog->setRemotePath(QStringLiteral("shell-up.bin"));
    dialog->setLocalPath(localDown);
    start->click();
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 2, kTransferTimeoutMs);
    QVERIFY2(finished.at(1).at(0).toBool(), qPrintable(finished.at(1).at(1).toString()));
    QCOMPARE(methods.size(), 2);
    QCOMPARE(methods.at(1), QStringLiteral("shell"));
    QCOMPARE(statusAtStart.at(1), QStringLiteral("Downloading shell-up.bin via shell (cat)..."));
    QCOMPARE(readFile(localDown), payload);
    QVERIFY(!QFileInfo::exists(localDown + QStringLiteral(".part")));
    // The SFTP probe is made exactly once per session (before the first transfer), not once
    // per transfer - and it did happen: a count of 0 would mean the fallback was never probed.
    QTRY_COMPARE_WITH_TIMEOUT(sftpRequests, 1, kSshTimeoutMs);   // queued counts from the server thread
    // The shell kept working next to the exec channels.
    QVERIFY(shellRoundTrip(ssh, QStringLiteral("echo shell-ok"), QStringLiteral("shell-ok")));

    child<QPushButton>(dialog, "buttonClose")->click();
    action(w, "actionDisconnect")->trigger();
    QTRY_COMPARE_WITH_TIMEOUT(server->activeConnections(), 0, kSshTimeoutMs);
    responder.stop();
    QCOMPARE(responder.authDialogs, 0);
    QCOMPARE(responder.messageBoxes, 0);
}

void Tst_sshsession::transferMethodSftpVsShell()
{
    // The two transfer methods side by side, through the real RemoteFileDialog of two tabs in
    // one window: the server with SFTP reports "via SFTP" / "(..., SFTP)", the one refusing the
    // subsystem (Options::allowSftp = false, a dropbear-like box) "via shell (cat)" /
    // "(..., via shell)"; both land the same file byte for byte and bring it back. Cross-package:
    // the ssh-core fallback, the test server's allowSftp and the dialog's method texts.
    auto sftpServer = startServer(QStringLiteral("method-sftp"));
    auto shellServer = startServer(QStringLiteral("method-shell"), kPassword, /*allowSftp=*/false);
    QVERIFY(sftpServer && shellServer);
    MainWindow w;
    QVERIFY(showAndActivate(w));
    const SshProfile sftpProfile = storeSilentProfile(w, *sftpServer, QStringLiteral("SFTP box"));
    const SshProfile shellProfile = storeSilentProfile(w, *shellServer, QStringLiteral("Shell box"));
    QVERIFY(!sftpProfile.id.isEmpty() && !shellProfile.id.isEmpty());
    DialogResponder responder;
    const QByteArray payload = randomBytes(300 * 1024 + 17);
    const QString localUp = tempPath(QStringLiteral("method/local/fw.bin"));
    QVERIFY(writeFile(localUp, payload));

    struct Method
    {
        const char* label;
        const SshProfile* profile;
        QString rootDir;
        QString name;      ///< SshConnection::transferStatus().method
        QString running;   ///< the tail of the dialog's status line once the transfer started
        QString suffix;    ///< the end of the connection's finished message
    };
    const Method methods[] = {
        {"sftp", &sftpProfile, tempPath(QStringLiteral("method-sftp/root")), QStringLiteral("sftp"),
         QStringLiteral("via SFTP..."), QStringLiteral(", SFTP)")},
        {"shell", &shellProfile, tempPath(QStringLiteral("method-shell/root")), QStringLiteral("shell"),
         QStringLiteral("via shell (cat)..."), QStringLiteral(", via shell)")},
    };
    for (const Method& m : methods) {
        SessionWidget* ssh = w.newSshSession(kProfileKeyPrefix + m.profile->id);
        QVERIFY2(connectFromAction(w, ssh), m.label);
        SshConnection* conn = ssh->sshConnection();
        action(w, "actionUploadFile")->trigger();
        auto* dialog = ssh->findChild<RemoteFileDialog*>();
        QVERIFY(dialog);
        QTRY_VERIFY(dialog->isVisible());
        auto* start = child<QPushButton>(dialog, "buttonStart");
        auto* status = child<QLabel>(dialog, "labelStatus");
        QVERIFY(start && status);
        QSignalSpy finished(dialog, &RemoteFileDialog::transferFinished);
        QStringList methodsSeen;
        QStringList runningLines;
        QObject hook;
        connect(conn, &SshConnection::transferStarted, &hook, [&](const SshConnection::TransferRequest&) {
            methodsSeen.append(conn->transferStatus().method);
            runningLines.append(status->text());   // the dialog's own slot ran first (connected earlier)
        });

        dialog->setLocalPath(localUp);
        dialog->setRemotePath(QStringLiteral("fw.bin"));   // relative: the login directory = the server root
        QTRY_VERIFY(start->isEnabled());
        start->click();
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, kTransferTimeoutMs);
        QVERIFY2(finished.at(0).at(0).toBool(), qPrintable(finished.at(0).at(1).toString()));
        const QString uploaded = finished.at(0).at(1).toString();
        QVERIFY2(uploaded.startsWith(QStringLiteral("Uploaded fw.bin to fw.bin (")) && uploaded.endsWith(m.suffix),
                 qPrintable(uploaded));
        QCOMPARE(status->text(), uploaded);
        QVERIFY(!isReddish(statusColor(status)));
        QCOMPARE(methodsSeen, QStringList{m.name});
        QCOMPARE(runningLines, QStringList{QStringLiteral("Uploading fw.bin ") + m.running});
        QCOMPARE(readFile(QDir(m.rootDir).filePath(QStringLiteral("fw.bin"))), payload);

        const QString localDown = tempPath(QStringLiteral("method/local/%1-down.bin").arg(QLatin1String(m.label)));
        dialog->setDirection(SshConnection::TransferDirection::Download);
        dialog->setRemotePath(QStringLiteral("fw.bin"));
        dialog->setLocalPath(localDown);
        QVERIFY(start->isEnabled());
        start->click();
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 2, kTransferTimeoutMs);
        QVERIFY2(finished.at(1).at(0).toBool(), qPrintable(finished.at(1).at(1).toString()));
        const QString downloaded = finished.at(1).at(1).toString();
        QVERIFY2(downloaded.startsWith(QStringLiteral("Downloaded fw.bin to ")) && downloaded.endsWith(m.suffix),
                 qPrintable(downloaded));
        QCOMPARE(methodsSeen.size(), 2);
        QCOMPARE(methodsSeen.at(1), m.name);
        QCOMPARE(runningLines.at(1), QStringLiteral("Downloading fw.bin ") + m.running);
        QCOMPARE(readFile(localDown), payload);
        QVERIFY(!QFileInfo::exists(localDown + QStringLiteral(".part")));
        child<QPushButton>(dialog, "buttonClose")->click();
        QVERIFY(shellRoundTrip(ssh, QStringLiteral("echo ") + QLatin1String(m.label), QLatin1String(m.label)));
    }

    // Both tabs are connected; Session > Disconnect on each.
    QCOMPARE(w.sessionCount(), 3);
    for (int i = 2; i >= 1; --i) {
        tabs(w)->setCurrentIndex(i);
        SessionWidget* session = w.currentSession();
        QVERIFY(session->isSsh() && session->isConnected());
        action(w, "actionDisconnect")->trigger();
        QCOMPARE(session->transport()->state(), State::Disconnected);
    }
    QTRY_COMPARE_WITH_TIMEOUT(sftpServer->activeConnections(), 0, kSshTimeoutMs);
    QTRY_COMPARE_WITH_TIMEOUT(shellServer->activeConnections(), 0, kSshTimeoutMs);
    responder.stop();
    QCOMPARE(responder.hostKeyDialogs, 0);
    QCOMPARE(responder.authDialogs, 0);
    QCOMPARE(responder.messageBoxes, 0);
}

void Tst_sshsession::probeRealServerTransfers()
{
    // The user's report reproduced against a real OpenSSH server (the WSL sshd during
    // development): New SSH Session, target + Enter, host key remembered into a temporary
    // known_hosts, password typed into the prompt, then every transfer path of the dialog.
    // Runs only with SU_SSH_PROBE_TARGET / SU_SSH_PROBE_PASSWORD set. Remote files are
    // verified through the session's own shell (sha256sum), so no WSL / sudo plumbing is needed.
    const QString target = qEnvironmentVariable("SU_SSH_PROBE_TARGET").trimmed();
    const QString password = qEnvironmentVariable("SU_SSH_PROBE_PASSWORD");
    if (target.isEmpty() || password.isEmpty()) {
        QSKIP("set SU_SSH_PROBE_TARGET=user@host[:port] and SU_SSH_PROBE_PASSWORD to probe a real OpenSSH server");
    }
    const QString knownHosts = freshKnownHosts(QStringLiteral("probe"));

    MainWindow w;
    QVERIFY(showAndActivate(w));
    action(w, "actionNewSshSession")->trigger();
    SessionWidget* ssh = w.currentSession();
    QVERIFY(ssh && ssh->isSsh());
    SshConnectionBar* bar = ssh->sshConnectionBar();
    auto* combo = child<QComboBox>(bar, "targetCombo");
    auto* connectButton = child<QPushButton>(bar, "connectButton");
    QVERIFY(combo && combo->lineEdit() && connectButton);
    QTest::keyClicks(combo->lineEdit(), target);
    QVERIFY(bar->hasValidTarget());
    SshConnection* conn = ssh->sshConnection();
    QStringList errors;
    QObject errorHook;
    connect(conn, &Transport::errorOccurred, &errorHook, [&errors](const QString& message) { errors.append(message); });
    DialogResponder responder;
    responder.password = password;
    responder.rememberHostKey = true;

    QTest::keyClick(combo->lineEdit(), Qt::Key_Return);
    QCOMPARE(ssh->transport()->state(), State::Connecting);
    QTRY_COMPARE_WITH_TIMEOUT(ssh->transport()->state(), State::Connected, kProbeTimeoutMs);
    QCOMPARE(responder.hostKeyDialogs, 1);
    QCOMPARE(responder.authDialogs, 1);
    QVERIFY2(responder.unexpected.isEmpty(), qPrintable(responder.unexpected.join(QStringLiteral(", "))));
    QCOMPARE(knownHostsLines(knownHosts).size(), 1);
    TerminalWidget* terminal = ssh->terminal();
    QTRY_VERIFY2_WITH_TIMEOUT(realPromptShown(terminal), qPrintable(allText(terminal).right(1000)), kProbeTimeoutMs);
    QTRY_VERIFY2_WITH_TIMEOUT(!conn->remoteHome().isEmpty(), "remote home not resolved", kProbeTimeoutMs);
    const QString home = conn->remoteHome();
    QVERIFY2(home.startsWith(QLatin1Char('/')), qPrintable(home));
    qInfo() << "probe: connected to" << target << "-" << conn->serverVersion() << "- home" << home;

    // 1. Upload a 3 MB random file with a space and a non-ASCII character in its name; the remote
    //    path is the dialog's default (<home>/<name>).
    const QString name = QStringLiteral(u"probe upload \u00FC.bin");
    const QByteArray payload = randomBytes(3 * 1024 * 1024);
    const QString sha = sha256Hex(payload);
    const QString localUp = tempPath(QStringLiteral("probe/local/") + name);
    QVERIFY(writeFile(localUp, payload));
    action(w, "actionUploadFile")->trigger();
    auto* dialog = ssh->findChild<RemoteFileDialog*>();
    QVERIFY(dialog);
    QTRY_VERIFY(dialog->isVisible());
    auto* start = child<QPushButton>(dialog, "buttonStart");
    auto* cancel = child<QPushButton>(dialog, "buttonCancel");
    auto* remote = child<QLineEdit>(dialog, "editRemotePath");
    auto* local = child<QLineEdit>(dialog, "editLocalPath");
    auto* status = child<QLabel>(dialog, "labelStatus");
    auto* progress = child<QProgressBar>(dialog, "progressBar");
    QVERIFY(start && cancel && remote && local && status && progress);
    QSignalSpy finished(dialog, &RemoteFileDialog::transferFinished);
    QSignalSpy started(conn, &SshConnection::transferStarted);
    QSignalSpy progressSpy(conn, &SshConnection::transferProgress);
    dialog->setLocalPath(localUp);
    QTRY_COMPARE_WITH_TIMEOUT(remote->text(), home + QLatin1Char('/') + name, kProbeTimeoutMs);
    QTRY_VERIFY(start->isEnabled());
    start->click();
    QVERIFY2(status->text().startsWith(QStringLiteral("Uploading ") + name), qPrintable(status->text()));
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, kTransferTimeoutMs);
    QVERIFY2(finished.at(0).at(0).toBool(), qPrintable(finished.at(0).at(1).toString()));
    qInfo() << "probe: upload 1:" << finished.at(0).at(1).toString() << "method" << conn->transferStatus().method;
    QCOMPARE(status->text(), finished.at(0).at(1).toString());
    QVERIFY(!isReddish(statusColor(status)));
    QCOMPARE(progress->value(), 100);
    QVERIFY(progressSpy.count() >= 2);   // 0 and the end at least
    QVERIFY(start->isEnabled());
    QVERIFY(remoteHashMatches(ssh, home + QLatin1Char('/') + name, sha));

    // 2. An explicit directory with a trailing slash, typed, then Start.
    remote->clear();
    QTest::keyClicks(remote, QStringLiteral("/tmp/"));
    QTest::mouseClick(start, Qt::LeftButton);
    QCOMPARE(remote->text(), QStringLiteral("/tmp/") + name);
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 2, kTransferTimeoutMs);
    QVERIFY2(finished.at(1).at(0).toBool(), qPrintable(finished.at(1).at(1).toString()));
    QCOMPARE(started.at(1).at(0).value<SshConnection::TransferRequest>().remotePath, QStringLiteral("/tmp/") + name);
    QVERIFY(remoteHashMatches(ssh, QStringLiteral("/tmp/") + name, sha));

    // 3. Download it back into a temporary directory, byte for byte, no .part left behind.
    const QString downDir = tempPath(QStringLiteral("probe/down"));
    QVERIFY(QDir().mkpath(downDir));
    dialog->setDirection(SshConnection::TransferDirection::Download);
    dialog->setRemotePath(QStringLiteral("/tmp/") + name);
    dialog->setLocalPath(downDir + QLatin1Char('/') + name);
    start->click();
    QVERIFY2(status->text().startsWith(QStringLiteral("Downloading ") + name), qPrintable(status->text()));
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 3, kTransferTimeoutMs);
    QVERIFY2(finished.at(2).at(0).toBool(), qPrintable(finished.at(2).at(1).toString()));
    qInfo() << "probe: download:" << finished.at(2).at(1).toString();
    QCOMPARE(readFile(downDir + QLatin1Char('/') + name), payload);
    QVERIFY(!QFileInfo::exists(downDir + QLatin1Char('/') + name + QStringLiteral(".part")));

    // 4. A remote path that does not exist: a readable error in red, the dialog usable afterwards.
    const QString missing = QStringLiteral("/tmp/probe-missing-%1.bin").arg(QRandomGenerator::global()->generate());
    dialog->setRemotePath(missing);
    dialog->setLocalPath(downDir + QStringLiteral("/missing.bin"));
    start->click();
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 4, kTransferTimeoutMs);
    QVERIFY(!finished.at(3).at(0).toBool());
    const QString missingMessage = finished.at(3).at(1).toString();
    qInfo() << "probe: missing file:" << missingMessage;
    QVERIFY2(missingMessage.contains(missing), qPrintable(missingMessage));
    QCOMPARE(status->text(), missingMessage);
    QVERIFY(isReddish(statusColor(status)));
    QVERIFY(start->isEnabled());
    QVERIFY(!cancel->isEnabled());
    QVERIFY(!QFileInfo::exists(downDir + QStringLiteral("/missing.bin")));
    QVERIFY(!QFileInfo::exists(downDir + QStringLiteral("/missing.bin.part")));

    // 5. Cancel half-way through a 50 MB upload to /tmp/, then upload it completely in the same
    //    dialog (a second transfer without reopening), then cancel half-way through its download.
    const QByteArray big = randomBytes(50 * 1024 * 1024);
    const QString shaBig = sha256Hex(big);
    const QString localBig = tempPath(QStringLiteral("probe/local/probe-big.bin"));
    QVERIFY(writeFile(localBig, big));
    dialog->setDirection(SshConnection::TransferDirection::Upload);
    dialog->setLocalPath(localBig);
    dialog->setRemotePath(QStringLiteral("/tmp/"));
    QCOMPARE(remote->text(), QStringLiteral("/tmp/probe-big.bin"));
    bool cancelled = false;
    QObject hook;
    connect(conn, &SshConnection::transferProgress, &hook, [&](qint64 done, qint64 total) {
        if (!cancelled && done > 0 && done < total) {
            cancelled = true;
            cancel->click();
        }
    });
    start->click();
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 5, kTransferTimeoutMs);
    QVERIFY(cancelled);
    QVERIFY(!finished.at(4).at(0).toBool());
    qInfo() << "probe: cancelled upload:" << finished.at(4).at(1).toString();
    QVERIFY2(finished.at(4).at(1).toString().contains(QStringLiteral("cancel"), Qt::CaseInsensitive),
             qPrintable(finished.at(4).at(1).toString()));
    QVERIFY(isReddish(statusColor(status)));
    QVERIFY(start->isEnabled());
    QVERIFY(!cancel->isEnabled());
    cancelled = true;   // the hook stays connected but idle for the complete upload
    QElapsedTimer uploadClock;
    uploadClock.start();
    start->click();
    QVERIFY(!start->isEnabled());
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 6, kTransferTimeoutMs);
    const qint64 uploadMs = qMax<qint64>(1, uploadClock.elapsed());
    QVERIFY2(finished.at(5).at(0).toBool(), qPrintable(finished.at(5).at(1).toString()));
    // Informational (the pipelined SFTP path of v0.4): the rate to this server, no floor asserted.
    qInfo() << "probe: 50 MB upload:" << finished.at(5).at(1).toString() << "in" << uploadMs << "ms ="
            << QString::number(static_cast<double>(big.size()) / 1048576.0 / (static_cast<double>(uploadMs) / 1000.0), 'f', 1)
            << "MiB/s";
    QVERIFY(remoteHashMatches(ssh, QStringLiteral("/tmp/probe-big.bin"), shaBig));
    dialog->setDirection(SshConnection::TransferDirection::Download);
    dialog->setRemotePath(QStringLiteral("/tmp/probe-big.bin"));
    dialog->setLocalPath(downDir + QStringLiteral("/probe-big.bin"));
    cancelled = false;
    start->click();
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 7, kTransferTimeoutMs);
    QObject::disconnect(conn, &SshConnection::transferProgress, &hook, nullptr);
    QVERIFY(cancelled);
    QVERIFY(!finished.at(6).at(0).toBool());
    QVERIFY(!QFileInfo::exists(downDir + QStringLiteral("/probe-big.bin.part")));
    QVERIFY(!QFileInfo::exists(downDir + QStringLiteral("/probe-big.bin")));
    QVERIFY(start->isEnabled());

    // 6. The drag-and-drop entry: SessionWidget::uploadFile(path) with the local path preset.
    child<QPushButton>(dialog, "buttonClose")->click();
    QVERIFY(!dialog->isVisible());
    ssh->uploadFile(localUp);
    QTRY_VERIFY(dialog->isVisible());
    QVERIFY(child<QRadioButton>(dialog, "radioUpload")->isChecked());
    QCOMPARE(QDir::fromNativeSeparators(local->text()), localUp);
    QTRY_COMPARE(remote->text(), QStringLiteral("/tmp/") + name);   // the directory used last
    QVERIFY(start->isEnabled());
    start->click();
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 8, kTransferTimeoutMs);
    QVERIFY2(finished.at(7).at(0).toBool(), qPrintable(finished.at(7).at(1).toString()));
    QVERIFY(remoteHashMatches(ssh, QStringLiteral("/tmp/") + name, sha));

    // 7. The modeless dialog survives a disconnect; after the reconnect from the bar it works again.
    action(w, "actionDisconnect")->trigger();
    QCOMPARE(ssh->transport()->state(), State::Disconnected);
    QTRY_COMPARE(status->text(), QStringLiteral("Not connected."));
    QVERIFY(!start->isEnabled());
    QVERIFY(dialog->isVisible());
    QTest::qWait(300);
    QTest::mouseClick(connectButton, Qt::LeftButton);
    QTRY_COMPARE_WITH_TIMEOUT(ssh->transport()->state(), State::Connected, kProbeTimeoutMs);
    QCOMPARE(responder.hostKeyDialogs, 1);   // remembered in the temporary file
    QTRY_COMPARE(status->text(), QStringLiteral("Ready."));
    QTRY_VERIFY(start->isEnabled());
    QTRY_VERIFY2_WITH_TIMEOUT(realPromptShown(terminal), qPrintable(allText(terminal).right(1000)), kProbeTimeoutMs);

    // Clean up on the remote and finish.
    QVERIFY(realShellCommand(ssh, QStringLiteral("rm -f %1 %2 %3")
                                      .arg(shellQuote(home + QLatin1Char('/') + name), shellQuote(QStringLiteral("/tmp/") + name),
                                           shellQuote(QStringLiteral("/tmp/probe-big.bin")))));
    child<QPushButton>(dialog, "buttonClose")->click();
    action(w, "actionDisconnect")->trigger();
    QTRY_COMPARE_WITH_TIMEOUT(ssh->transport()->state(), State::Disconnected, kProbeTimeoutMs);
    responder.stop();
    QCOMPARE(responder.messageBoxes, 0);
    if (!errors.isEmpty()) {
        qInfo() << "probe: errorOccurred during the run:" << errors;
    }
}

// =======================================================================================
// Links dropping, other tabs, language, quitting
// =======================================================================================

void Tst_sshsession::autoReconnectAfterDrop()
{
    auto server = startServer(QStringLiteral("drop"));
    QVERIFY(server);
    const QString name = QStringLiteral("Drop box");

    MainWindow w;
    QVERIFY(showAndActivate(w));
    const SshProfile stored = storeSilentProfile(w, *server, name, /*keepAliveSeconds=*/1);
    QVERIFY(!stored.id.isEmpty());
    SessionWidget* ssh = w.newSshSession(kProfileKeyPrefix + stored.id);
    QVERIFY(connectFromAction(w, ssh));
    QVERIFY(ssh->transport()->autoReconnect());
    const int tab = tabs(w)->indexOf(ssh);
    TerminalWidget* terminal = ssh->terminal();
    SshConnectionBar* bar = ssh->sshConnectionBar();
    auto* connectButton = child<QPushButton>(bar, "connectButton");
    QSignalSpy lostSpy(ssh->transport(), &Transport::connectionLost);
    QSignalSpy restoredSpy(ssh->transport(), &Transport::connectionRestored);
    QSignalSpy stateSpy(ssh, &SessionWidget::connectionStateChanged);
    DialogResponder responder;   // a reconnect uses the saved password: nothing may be asked

    // The server cuts the link: status message, system line, Reconnecting.
    server->dropAllClients();
    QTRY_COMPARE_WITH_TIMEOUT(lostSpy.count(), 1, kDropTimeoutMs);
    QCOMPARE(lostSpy.at(0).at(0).toString(), name);
    QCOMPARE(ssh->transport()->state(), State::Reconnecting);
    QCOMPARE(w.statusBar()->currentMessage(), QStringLiteral("Connection to %1 lost - reconnecting").arg(name));
    QTRY_VERIFY2(allLines(terminal).contains(QStringLiteral("--- connection to %1 lost, waiting to reconnect ---").arg(name)),
                 qPrintable(allText(terminal)));
    QCOMPARE(connectButton->text(), QStringLiteral("Reconnecting..."));
    QCOMPARE(tabDotColor(tabs(w), tab), kDotBusy);
    QVERIFY2(statusConnectionLabel(w)->text().startsWith(QStringLiteral("Reconnecting...")),
             qPrintable(statusConnectionLabel(w)->text()));
    QVERIFY(!terminal->inputEnabled());
    QVERIFY(action(w, "actionDisconnect")->isEnabled());   // cancels the reconnect
    QVERIFY(!action(w, "actionConnect")->isEnabled());
    QVERIFY(!action(w, "actionUploadFile")->isEnabled());

    // The reconnect (2 s backoff) restores the session: the prompt is back and the shell works.
    QTRY_COMPARE_WITH_TIMEOUT(restoredSpy.count(), 1, kSshTimeoutMs);
    QCOMPARE(ssh->transport()->state(), State::Connected);
    QCOMPARE(w.statusBar()->currentMessage(), QStringLiteral("Reconnected to %1").arg(name));
    QTRY_VERIFY2(allLines(terminal).contains(QStringLiteral("--- reconnected ---")), qPrintable(allText(terminal)));
    QTRY_COMPARE_WITH_TIMEOUT(countLines(terminal, QStringLiteral("welcome")), 2, kSshTimeoutMs);
    QTRY_COMPARE_WITH_TIMEOUT(lastNonBlankLine(terminal), QStringLiteral("$"), kSshTimeoutMs);
    QVERIFY(terminal->inputEnabled());
    QCOMPARE(connectButton->text(), QStringLiteral("Disconnect"));
    QCOMPARE(tabDotColor(tabs(w), tab), kDotConnected);
    QVERIFY(action(w, "actionUploadFile")->isEnabled());
    QCOMPARE(server->connectionCount(), 2);
    QVERIFY(shellRoundTrip(ssh, QStringLiteral("echo back"), QStringLiteral("back")));
    QCOMPARE(responder.hostKeyDialogs, 0);
    QCOMPARE(responder.authDialogs, 0);

    // Auto-reconnect off in the preferences: the next drop ends the session.
    AppSettings::instance().setAutoReconnect(false);
    QVERIFY(!ssh->transport()->autoReconnect());
    const int statesBefore = stateSpy.count();
    server->dropAllClients();
    QTRY_COMPARE_WITH_TIMEOUT(ssh->transport()->state(), State::Disconnected, kDropTimeoutMs);
    QCOMPARE(lostSpy.count(), 2);
    QCOMPARE(restoredSpy.count(), 1);
    QCOMPARE(w.statusBar()->currentMessage(), QStringLiteral("Connection to %1 lost").arg(name));
    QTRY_VERIFY2(allLines(terminal).contains(QStringLiteral("--- connection to %1 lost ---").arg(name)),
                 qPrintable(allText(terminal)));
    QCOMPARE(stateSpy.count(), statesBefore + 1);
    QCOMPARE(stateSpy.last().at(0).value<State>(), State::Disconnected);
    QCOMPARE(connectButton->text(), QStringLiteral("Connect"));
    QCOMPARE(tabDotColor(tabs(w), tab), kDotDisconnected);
    QVERIFY(action(w, "actionConnect")->isEnabled());
    QVERIFY(!action(w, "actionDisconnect")->isEnabled());
    QTest::qWait(500);
    QCOMPARE(ssh->transport()->state(), State::Disconnected);
    QCOMPARE(server->connectionCount(), 2);
    responder.stop();
    QCOMPARE(responder.messageBoxes, 0);
    QVERIFY2(responder.unexpected.isEmpty(), qPrintable(responder.unexpected.join(QStringLiteral(", "))));
}

void Tst_sshsession::serialTabKeepsWorking()
{
    auto server = startServer(QStringLiteral("serial"));
    QVERIFY(server);
    const QString name = QStringLiteral("Serial box");

    MainWindow w;
    QVERIFY(showAndActivate(w));
    SessionWidget* serial = w.currentSession();
    QVERIFY(serial && !serial->isSsh());
    serial->setPortName(kLoopback);
    action(w, "actionConnect")->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(serial->isConnected(), kSimTimeoutMs);
    QByteArray echoed;
    QObject echoSink;
    connect(serial->transport(), &Transport::dataReceived, &echoSink,
            [&echoed](const QByteArray& bytes) { echoed += bytes; });

    const SshProfile stored = storeSilentProfile(w, *server, name);
    QVERIFY(!stored.id.isEmpty());
    SessionWidget* ssh = w.newSshSession(kProfileKeyPrefix + stored.id);
    QVERIFY(connectFromAction(w, ssh));
    QVERIFY(serial->isConnected());
    QCOMPARE(w.sessionCount(), 2);

    // SSH tab current: SFTP on, the serial-only actions off.
    QCOMPARE(w.currentSession(), ssh);
    QVERIFY(action(w, "actionUploadFile")->isEnabled());
    QVERIFY(action(w, "actionDownloadFile")->isEnabled());
    QVERIFY(!action(w, "actionSendBreak")->isEnabled());
    QVERIFY(!action(w, "actionRefreshPorts")->isEnabled());
    QVERIFY(action(w, "actionSendFile")->isEnabled());
    QVERIFY(action(w, "actionDisconnect")->isEnabled());
    QVERIFY2(statusConnectionLabel(w)->text().contains(name), qPrintable(statusConnectionLabel(w)->text()));

    // Serial tab current: it still echoes, BREAK is on, SFTP is off.
    tabs(w)->setCurrentIndex(0);
    QCOMPARE(w.currentSession(), serial);
    QVERIFY(action(w, "actionSendBreak")->isEnabled());
    QVERIFY(action(w, "actionRefreshPorts")->isEnabled());
    QVERIFY(!action(w, "actionUploadFile")->isEnabled());
    QVERIFY(!action(w, "actionDownloadFile")->isEnabled());
    QVERIFY(action(w, "actionSendFile")->isEnabled());
    QVERIFY2(statusConnectionLabel(w)->text().contains(kLoopback), qPrintable(statusConnectionLabel(w)->text()));
    QVERIFY(statusConnectionLabel(w)->text().contains(QStringLiteral("115200 8N1")));
    serial->focusTerminal();
    QTest::keyClicks(serial->terminal(), QStringLiteral("ping"));
    QTRY_COMPARE_WITH_TIMEOUT(echoed, QByteArrayLiteral("ping"), kSimTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(allText(serial->terminal()).contains(QStringLiteral("ping")), kSimTimeoutMs);

    // The SSH shell keeps running in the background tab.
    QCOMPARE(ssh->transport()->state(), State::Connected);
    const int before = countLines(ssh->terminal(), QStringLiteral("bg"));
    ssh->sendBytes(QByteArrayLiteral("echo bg\r"));
    QTRY_VERIFY_WITH_TIMEOUT(countLines(ssh->terminal(), QStringLiteral("bg")) > before, kSshTimeoutMs);

    // Back and forth once more: each tab keeps its own rules and its own device.
    tabs(w)->setCurrentIndex(1);
    QCOMPARE(w.currentSession(), ssh);
    QVERIFY(action(w, "actionUploadFile")->isEnabled());
    QVERIFY(!action(w, "actionSendBreak")->isEnabled());
    QVERIFY(shellRoundTrip(ssh, QStringLiteral("echo fg"), QStringLiteral("fg")));
    tabs(w)->setCurrentIndex(0);
    QVERIFY(!action(w, "actionUploadFile")->isEnabled());
    QVERIFY(action(w, "actionSendBreak")->isEnabled());
    QTest::keyClicks(serial->terminal(), QStringLiteral("pong"));
    QTRY_COMPARE_WITH_TIMEOUT(echoed, QByteArrayLiteral("pingpong"), kSimTimeoutMs);
    QVERIFY(serial->isConnected());
    QVERIFY(ssh->isConnected());

    action(w, "actionDisconnect")->trigger();   // the serial tab
    QTRY_VERIFY_WITH_TIMEOUT(!serial->isConnected(), kSimTimeoutMs);
    QVERIFY(ssh->isConnected());
    tabs(w)->setCurrentIndex(1);
    action(w, "actionDisconnect")->trigger();   // the SSH tab
    QCOMPARE(ssh->transport()->state(), State::Disconnected);
    QTRY_COMPARE_WITH_TIMEOUT(server->activeConnections(), 0, kSshTimeoutMs);
}

void Tst_sshsession::languageSwitchWithSshTab()
{
    auto server = startServer(QStringLiteral("lang"));
    QVERIFY(server);
    const QString name = QStringLiteral("Lang box");

    MainWindow w;
    QVERIFY(showAndActivate(w));
    const SshProfile stored = storeSilentProfile(w, *server, name);
    QVERIFY(!stored.id.isEmpty());
    SessionWidget* ssh = w.newSshSession(kProfileKeyPrefix + stored.id);
    QVERIFY(connectFromAction(w, ssh));
    SshConnectionBar* bar = ssh->sshConnectionBar();
    auto* connectButton = child<QPushButton>(bar, "connectButton");
    QLabel* targetLabel = nullptr;
    const QList<QLabel*> labels = bar->findChildren<QLabel*>();
    for (QLabel* label : labels) {
        if (label->text() == QStringLiteral("Target:")) {
            targetLabel = label;
        }
    }
    QVERIFY(targetLabel);
    auto* fileMenu = child<QMenu>(&w, "menuFile");
    QVERIFY(fileMenu);
    // The modeless transfer dialog retranslates itself too.
    action(w, "actionUploadFile")->trigger();
    auto* dialog = ssh->findChild<RemoteFileDialog*>();
    QVERIFY(dialog);
    QTRY_VERIFY(dialog->isVisible());
    auto* start = child<QPushButton>(dialog, "buttonStart");
    QCOMPARE(QString(start->text()).remove(QLatin1Char('&')), QStringLiteral("Start upload"));

    // To Chinese: menus, the bar and the status bar follow; the session is untouched.
    action(w, "actionLanguageChinese")->trigger();
    QTRY_COMPARE(fileMenu->title(), QStringLiteral(u"文件(&F)"));
    QCOMPARE(AppSettings::instance().language(), QStringLiteral("zh_CN"));
    QCOMPARE(targetLabel->text(), QStringLiteral(u"目标："));
    QCOMPARE(connectButton->text(), QStringLiteral(u"断开"));
    const QString connectedZh = QCoreApplication::translate("SerialConnection", "Connected");
    QVERIFY2(connectedZh != QStringLiteral("Connected"), "the state text is not translated");
    QVERIFY2(statusConnectionLabel(w)->text().startsWith(connectedZh), qPrintable(statusConnectionLabel(w)->text()));
    QVERIFY(statusConnectionLabel(w)->text().contains(name));
    QVERIFY(dialog->isVisible());
    QVERIFY(ssh->isConnected());
    QVERIFY(shellRoundTrip(ssh, QStringLiteral("echo zh"), QStringLiteral("zh")));
    QVERIFY(action(w, "actionUploadFile")->isEnabled());

    // And back.
    action(w, "actionLanguageEnglish")->trigger();
    QTRY_COMPARE(fileMenu->title(), QStringLiteral("&File"));
    QCOMPARE(targetLabel->text(), QStringLiteral("Target:"));
    QCOMPARE(connectButton->text(), QStringLiteral("Disconnect"));
    QCOMPARE(QString(start->text()).remove(QLatin1Char('&')), QStringLiteral("Start upload"));
    QVERIFY2(statusConnectionLabel(w)->text().startsWith(QStringLiteral("Connected   ") + name),
             qPrintable(statusConnectionLabel(w)->text()));
    QVERIFY(ssh->isConnected());
    QVERIFY(shellRoundTrip(ssh, QStringLiteral("echo en"), QStringLiteral("en")));

    child<QPushButton>(dialog, "buttonClose")->click();
    action(w, "actionDisconnect")->trigger();
    QCOMPARE(ssh->transport()->state(), State::Disconnected);
    QCOMPARE(connectButton->text(), QStringLiteral("Connect"));
    QTRY_COMPARE_WITH_TIMEOUT(server->activeConnections(), 0, kSshTimeoutMs);
}

void Tst_sshsession::closeWindowWhileConnected()
{
    auto server = startServer(QStringLiteral("close"));
    QVERIFY(server);
    const QString name = QStringLiteral("Close box");

    auto w = std::make_unique<MainWindow>();
    QVERIFY(showAndActivate(*w));
    const SshProfile stored = storeSilentProfile(*w, *server, name);
    QVERIFY(!stored.id.isEmpty());
    const QString key = kProfileKeyPrefix + stored.id;
    SessionWidget* ssh = w->newSshSession(key);
    QVERIFY(connectFromAction(*w, ssh));
    QPointer<SessionWidget> sessionPtr(ssh);
    QPointer<QThread> worker = ssh->sshConnection()->findChild<QThread*>(QStringLiteral("ssh-worker"));
    QVERIFY(worker);
    QVERIFY(worker->isRunning());
    QVERIFY(AppSettings::instance().confirmCloseWhenConnected());

    // File > Quit asks; "No" keeps everything as it is.
    {
        DialogResponder responder;
        responder.messageAnswer = QMessageBox::No;
        action(*w, "actionQuit")->trigger();
        responder.stop();
        QCOMPARE(responder.messageBoxes, 1);
        QVERIFY2(responder.messageTitles.first().startsWith(QStringLiteral("Quit")),
                 qPrintable(responder.messageTitles.first()));
        QVERIFY2(responder.unexpected.isEmpty(), qPrintable(responder.unexpected.join(QStringLiteral(", "))));
    }
    QVERIFY(w->isVisible());
    QVERIFY(ssh->isConnected());
    QCOMPARE(server->activeConnections(), 1);
    QVERIFY(shellRoundTrip(ssh, QStringLiteral("echo still"), QStringLiteral("still")));

    // Confirmed: the window closes, the session is shut down cleanly.
    {
        DialogResponder responder;
        responder.messageAnswer = QMessageBox::Yes;
        w->close();
        responder.stop();
        QCOMPARE(responder.messageBoxes, 1);
    }
    QTRY_VERIFY(!w->isVisible());
    QCOMPARE(ssh->transport()->state(), State::Disconnected);
    QCOMPARE(AppSettings::instance().lastOpenPorts(), QStringList{key});
    QTRY_COMPARE_WITH_TIMEOUT(server->activeConnections(), 0, kSshTimeoutMs);
    QCOMPARE(server->connectionCount(), 1);
    QVERIFY(worker->isRunning());   // the thread lives with the connection, idle now

    // Destroying the window joins the worker thread; nothing crashes.
    w.reset();
    QVERIFY(sessionPtr.isNull());
    QVERIFY(worker.isNull());
    QTest::qWait(200);
    QCOMPARE(server->activeConnections(), 0);
}

QTEST_MAIN(Tst_sshsession)
#include "tst_sshsession.moc"
