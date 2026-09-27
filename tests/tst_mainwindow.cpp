// GUI test suite for MainWindow (tabs, actions and shortcuts, status bar, language switch,
// state persistence, confirmation dialogs, the SSH tabs / actions / restore keys) and for the
// System Log dock (SystemLogViewer). Runs offscreen (QT_QPA_PLATFORM=offscreen); QSettings and
// every file live in a temporary directory or the QStandardPaths test-mode data directory, so
// nothing touches the user's real configuration. The built-in simulated devices (SIM:loopback,
// SIM:mcu) stand in for serial ports, no SSH connection is ever opened, and
// QDesktopServices::openUrl() is intercepted so the suite never launches a browser or file manager.
#include <QtTest>

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTextDocument>
#include <QTextEdit>
#include <QTimer>
#include <QToolBar>
#include <QUrl>

#include "Version.h"
#include "app/AppSettings.h"
#include "app/Logging.h"
#include "core/SerialConnection.h"
#include "core/Transport.h"
#include "dialogs/AboutDialog.h"
#include "dialogs/PreferencesDialog.h"
#include "dialogs/VersionDialog.h"
#include "ssh/SecretStore.h"
#include "ssh/SshConnection.h"
#include "ssh/SshProfile.h"
#include "support/TestSshServer.h"
#include "terminal/TerminalScreen.h"
#include "terminal/TerminalWidget.h"
#include "ui/CommandInput.h"
#include "ui/HexDumpView.h"
#include "ui/MainWindow.h"
#include "ui/QuickCommandBar.h"
#include "ui/SessionWidget.h"
#include "ui/SshConnectionBar.h"

#include <iterator>   // std::size
#include "ui/SystemLogViewer.h"

// =======================================================================================
// Helpers
// =======================================================================================

/// Receives QDesktopServices::openUrl() calls (file / http / https) so the suite never opens
/// a file manager or browser; records what the application tried to open.
class UrlSink : public QObject
{
    Q_OBJECT
public:
    QList<QUrl> urls;

public slots:
    void handle(const QUrl& url) { urls.append(url); }
};

/// Answers modal dialogs from inside their exec() loop: QMessageBox questions get Yes / No,
/// everything else is rejected. Counts what it dismissed so a test can assert that a dialog
/// did (or did not) appear. Polls with a timer, which keeps running inside nested event loops.
class ModalDismisser : public QObject
{
    Q_OBJECT
public:
    enum class Answer { Reject, Yes, No };

    explicit ModalDismisser(Answer answer, QObject* parent = nullptr)
        : QObject(parent)
        , m_answer(answer)
    {
        m_timer.setInterval(25);
        connect(&m_timer, &QTimer::timeout, this, &ModalDismisser::poll);
        m_timer.start();
    }

    void stop() { m_timer.stop(); }
    int count() const { return m_count; }
    QStringList classNames() const { return m_classNames; }
    QStringList windowTitles() const { return m_windowTitles; }

private:
    void poll()
    {
        QWidget* modal = QApplication::activeModalWidget();
        if (!modal || !modal->isVisible()) {
            return;
        }
        ++m_count;
        m_classNames.append(QString::fromLatin1(modal->metaObject()->className()));
        m_windowTitles.append(modal->windowTitle());
        if (auto* box = qobject_cast<QMessageBox*>(modal); box && m_answer != Answer::Reject) {
            QAbstractButton* button = box->button(m_answer == Answer::Yes ? QMessageBox::Yes : QMessageBox::No);
            if (button) {
                button->click();
                return;
            }
        }
        if (auto* dialog = qobject_cast<QDialog*>(modal)) {
            dialog->reject();
        } else {
            modal->close();
        }
    }

    Answer m_answer;
    QTimer m_timer;
    int m_count = 0;
    QStringList m_classNames;
    QStringList m_windowTitles;
};

namespace {

constexpr int kSimTimeoutMs = 10000;   ///< simulated device round trips
constexpr int kSshTimeoutMs = 15000;   ///< a connect / a round trip to the in-process SSH server

const QString kLoopback = QStringLiteral("SIM:loopback");
const QString kMcu = QStringLiteral("SIM:mcu");
const QString kSshUser = QStringLiteral("test");
const QString kSshPassword = QStringLiteral("secret");
const QString kShowCommandInputKey = QStringLiteral("ui/showCommandInput");
const QString kShowQuickCommandsKey = QStringLiteral("ui/showQuickCommands");
const QString kNewSessionTitle = QStringLiteral("New Session");

/// The action contract from MainWindow.h: objectName, shortcut (portable text) and whether
/// the action is checkable.
struct ActionSpec
{
    const char* name;
    const char* shortcut;
    bool checkable;
};

const ActionSpec kActions[] = {
    // File
    {"actionNewSession", "Ctrl+T", false},
    {"actionNewSshSession", "Ctrl+Shift+T", false},
    {"actionCloseSession", "Ctrl+Shift+W", false},
    {"actionStartLogging", "", false},
    {"actionStopLogging", "", false},
    {"actionOpenLogFolder", "", false},
    {"actionReplayLog", "Ctrl+Shift+R", false},
    {"actionStopReplay", "", false},
    {"actionQuit", "Ctrl+Shift+Q", false},
    // Session
    {"actionConnect", "F2", false},
    {"actionDisconnect", "F3", false},
    {"actionClear", "Ctrl+Shift+L", false},
    {"actionResetTerminal", "", false},
    {"actionSendFile", "Ctrl+Shift+O", false},
    {"actionUploadFile", "", false},
    {"actionDownloadFile", "", false},
    {"actionSendBreak", "", false},
    {"actionSyncTerminalSize", "", false},
    {"actionDetectBaudRate", "Ctrl+Shift+B", false},
    {"actionRefreshPorts", "F5", false},
    // Edit
    {"actionCopy", "Ctrl+Shift+C", false},
    {"actionPaste", "Ctrl+Shift+V", false},
    {"actionSelectAll", "", false},
    {"actionFind", "Ctrl+Shift+F", false},
    {"actionQuickCommands", "", false},
    {"actionSshProfiles", "", false},
    {"actionPreferences", "Ctrl+,", false},
    // View
    {"actionHexView", "Ctrl+Shift+H", true},
    {"actionShowCommandInput", "", true},
    {"actionShowQuickCommands", "", true},
    {"actionPauseWhileSelecting", "", true},
    {"actionRightClickPastes", "", true},
    {"actionSystemLog", "", true},
    {"actionZoomIn", "Ctrl++", false},
    {"actionZoomOut", "Ctrl+-", false},
    {"actionZoomReset", "Ctrl+0", false},
    // Window
    {"actionNextTab", "Ctrl+Tab", false},
    {"actionPreviousTab", "Ctrl+Shift+Tab", false},
    {"actionSelectTab1", "Alt+1", false},
    {"actionSelectTab2", "Alt+2", false},
    {"actionSelectTab3", "Alt+3", false},
    {"actionSelectTab4", "Alt+4", false},
    {"actionSelectTab5", "Alt+5", false},
    {"actionSelectTab6", "Alt+6", false},
    {"actionSelectTab7", "Alt+7", false},
    {"actionSelectTab8", "Alt+8", false},
    {"actionSelectTab9", "Alt+9", false},
    // Language
    {"actionLanguageEnglish", "", true},
    {"actionLanguageChinese", "", true},
    // Help
    {"actionAbout", "", false},
    {"actionVersion", "", false},
    {"actionHomepage", "", false},
};

QAction* action(const MainWindow& w, const char* name)
{
    return w.findChild<QAction*>(QLatin1String(name));
}

template <typename T>
T* child(const QObject* parent, const char* objectName)
{
    return parent->findChild<T*>(QLatin1String(objectName));
}

QTabWidget* tabs(const MainWindow& w)
{
    return child<QTabWidget>(&w, "tabWidget");
}

QLabel* statusLabel(const MainWindow& w, const char* name)
{
    return child<QLabel>(&w, name);
}

bool writeFile(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    return file.write(bytes) == bytes.size();
}

QString viewerText(const SystemLogViewer& viewer)
{
    const auto* edit = viewer.findChild<QTextEdit*>();
    return edit ? edit->toPlainText() : QString();
}

/// Every payload of a QByteArray signal spy (Transport::dataSent / dataReceived), in order.
QByteArray spyBytes(const QSignalSpy& spy)
{
    QByteArray all;
    for (const QList<QVariant>& args : spy) {
        all += args.at(0).toByteArray();
    }
    return all;
}

QKeySequence seq(const char* portable)
{
    return QKeySequence(QString::fromLatin1(portable), QKeySequence::PortableText);
}

/// The last line of the terminal with any text on it ("$" for the test server's shell prompt).
QString lastNonBlankLine(const TerminalWidget* terminal)
{
    const TerminalScreen* screen = terminal->screen();
    for (int i = screen->totalLines() - 1; i >= 0; --i) {
        const QString line = screen->lineText(i).trimmed();
        if (!line.isEmpty()) {
            return line;
        }
    }
    return QString();
}

/// The accelerator of a menu / action text ("&Connect" -> 'c', lower case); a null QChar when
/// the text has none ("&&" is a literal ampersand, as in the Window menu's session entries).
QChar mnemonicOf(const QString& text)
{
    for (qsizetype i = 0; i + 1 < text.size(); ++i) {
        if (text.at(i) == QLatin1Char('&')) {
            if (text.at(i + 1) == QLatin1Char('&')) {
                ++i;
                continue;
            }
            return text.at(i + 1).toLower();
        }
    }
    return QChar();
}

/// The dynamic per-session entries of the Window menu: everything after its second separator.
QList<QAction*> sessionEntries(const QMenu* windowMenu)
{
    QList<QAction*> entries;
    int separators = 0;
    const QList<QAction*> actions = windowMenu->actions();
    for (QAction* a : actions) {
        if (a->isSeparator()) {
            ++separators;
        } else if (separators >= 2) {
            entries.append(a);
        }
    }
    return entries;
}

} // namespace

// =======================================================================================
// Test class
// =======================================================================================

class Tst_mainwindow : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    // ---- Construction / actions -----------------------------------------------------
    void constructsAndShows();
    void actionsExistWithShortcuts();
    void noBareCtrlLetterShortcuts();
    void triggerEveryActionDisconnected();
    void triggerEveryActionConnected();
    void quitAction();
    void quitAsksWhenConnected();
    void publicApiForMain();

    // ---- Tabs -----------------------------------------------------------------------
    void newSessionAction();
    void newSessionShortcut();
    void closeSessionRemovesTab();
    void closeSessionNeverZeroTabs();
    void nextPreviousTabCycle();
    void tabTextAndTooltip();
    void tabChangeUpdatesTitleAndStatus();

    // ---- Keyboard (v0.4): configurable shortcuts, tab selection, Ctrl+W to the shell -----
    void closeSessionShortcutLeavesCtrlWToShell();
    void selectTabShortcuts();
    void windowMenuListsSessions();
    void middleClickClosesTab();
    void detectBaudRateAction();
    void shortcutEntriesContract();
    void storedShortcutAppliedAndLive();
    void reservedShortcutsPushedToTerminals();
    void sshTabKeyboardEndToEnd();
    void menuMnemonicsUnique();

    // ---- View -----------------------------------------------------------------------
    void hexViewToggle();
    void hexViewShortcut();
    void hexViewShortcutWhileConnected();
    void showCommandInputToggle();
    void showQuickCommandsToggle();
    void panelVisibilityPersistsAcrossWindows();
    void pauseWhileSelectingToggle();
    void pauseStatusMessage();
    void pauseHintFollowsCurrentTab();
    void rightClickPastesToggle();
    void clearActionWipesEverything();
    void systemLogDockToggle();
    void zoomActions();

    // ---- Language -------------------------------------------------------------------
    void languageSwitch();
    void languageDefault();

    // ---- Title / status bar ---------------------------------------------------------
    void windowTitleFollowsPort();
    void connectDisconnectActions();
    void statusBarConnected();
    void statusBarCounters();
    void statusBarGridAndEncoding();
    void statusBarLoggingIndicator();
    void statusMessageFromSession();

    // ---- Close confirmation ---------------------------------------------------------
    void closeConnectedTabConfirmYes();
    void closeConnectedTabConfirmNo();
    void closeConnectedTabNoConfirmWhenDisabled();
    void closeDisconnectedTabNeverAsks();

    // ---- Persistence ----------------------------------------------------------------
    void restoreLastPortsRoundTrip();
    void restoreLastPortsDisabled();
    void restoreLastPortsDeduplicates();

    // ---- SSH (no connection is ever opened) -----------------------------------------
    void sshActionsInMenusAndToolbar();
    void newSshSessionAddsTab();
    void newSshSessionShortcut();
    void newSshSessionPrefillsLastTarget();
    void sshActionEnableRules();
    void sshRestoreRoundTrip();
    void sshProfilesDialogOpens();
    void sshProfileStoreSavedAndReloaded();
    void statusBarForSshTab();

    // ---- Replay / folders / help ----------------------------------------------------
    void replayEnablesStopReplay();
    void replayCompletesAndDisablesStop();
    void replayRefusedWhileConnected();
    void openLogFolderCreatesDirectory();
    void homepageOpensUrl();
    void aboutAndVersionDialogsReused();

    // ---- SystemLogViewer ------------------------------------------------------------
    void sysLogRoutesQtMessages();
    void sysLogLevelFilterHidesDebug();
    void sysLogMaxLinesTrimming();
    void sysLogClearAndCopyAll();

private:
    bool showAndActivate(MainWindow& w);
    bool connectCurrent(MainWindow& w, const QString& port);
    QString tempPath(const QString& name) const;

    QTemporaryDir m_tempDir;
    UrlSink m_urls;
};

// =======================================================================================
// Fixture
// =======================================================================================

void Tst_mainwindow::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("BuildAI-Test"));
    QCoreApplication::setApplicationName(QStringLiteral("SerialUtilityTest-mainwindow"));
    QVERIFY(m_tempDir.isValid());

    // AppSettings uses the default QSettings() constructor: keep it out of the registry and
    // inside the temporary directory, and make sure it picked up the test names above.
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_tempDir.filePath(QStringLiteral("settings")));
    QSettings settings;
    settings.clear();
    QVERIFY(settings.fileName().contains(QStringLiteral("BuildAI-Test")));
    QVERIFY(settings.fileName().contains(QStringLiteral("SerialUtilityTest-mainwindow")));

    AppSettings::instance().setLogDirectory(m_tempDir.filePath(QStringLiteral("logs")));
    QVERIFY(settings.contains(QStringLiteral("logging/directory")));
    // Quick commands / history go to the test-mode data directory, never the real one.
    QVERIFY(!AppSettings::dataDirectory().contains(QStringLiteral("/BuildAI/SerialUtility")));

    QDesktopServices::setUrlHandler(QStringLiteral("file"), &m_urls, "handle");
    QDesktopServices::setUrlHandler(QStringLiteral("http"), &m_urls, "handle");
    QDesktopServices::setUrlHandler(QStringLiteral("https"), &m_urls, "handle");

    // The one SSH connect of this suite (sshTabKeyboardEndToEnd) must not read ~/.ssh/config.
    qputenv("SU_SSH_IGNORE_CONFIG", "1");
}

void Tst_mainwindow::cleanupTestCase()
{
    QDesktopServices::unsetUrlHandler(QStringLiteral("file"));
    QDesktopServices::unsetUrlHandler(QStringLiteral("http"));
    QDesktopServices::unsetUrlHandler(QStringLiteral("https"));
}

void Tst_mainwindow::init()
{
    QSettings().clear();
    AppSettings& app = AppSettings::instance();
    app.setLanguage(QStringLiteral("en_US"));
    app.setLogDirectory(m_tempDir.filePath(QStringLiteral("logs")));
    app.setShowSimulatedPorts(true);
    app.setAutoLog(false);
    app.setConfirmCloseWhenConnected(true);
    app.setRestoreLastPorts(true);
    app.setReconnectIntervalMs(200);
    app.setAutoBaudSampleMs(300);   // Detect Baud Rate, should a test trigger it, stays short
    m_urls.urls.clear();
}

void Tst_mainwindow::cleanup()
{
    // Sessions closed with deleteLater() must be gone before the next window is built.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

bool Tst_mainwindow::showAndActivate(MainWindow& w)
{
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

bool Tst_mainwindow::connectCurrent(MainWindow& w, const QString& port)
{
    SessionWidget* session = w.currentSession();
    if (!session) {
        qWarning() << "no current session";
        return false;
    }
    session->setPortName(port);
    if (session->portName() != port) {
        qWarning() << "port not selected:" << session->portName();
        return false;
    }
    QAction* connectAction = action(w, "actionConnect");
    if (!connectAction || !connectAction->isEnabled()) {
        qWarning() << "actionConnect missing or disabled";
        return false;
    }
    connectAction->trigger();
    return QTest::qWaitFor([session]() { return session->isConnected(); }, kSimTimeoutMs);
}

QString Tst_mainwindow::tempPath(const QString& name) const
{
    return m_tempDir.filePath(name);
}

// =======================================================================================
// Construction / actions
// =======================================================================================

void Tst_mainwindow::constructsAndShows()
{
    MainWindow w;
    QVERIFY(showAndActivate(w));

    QVERIFY(w.sessionCount() >= 1);
    QVERIFY(w.currentSession() != nullptr);
    QCOMPARE(w.sessionAt(0), w.currentSession());
    QVERIFY(w.sessionAt(-1) == nullptr);
    QVERIFY(w.sessionAt(w.sessionCount()) == nullptr);

    QTabWidget* tabWidget = tabs(w);
    QVERIFY(tabWidget);
    QCOMPARE(tabWidget->count(), w.sessionCount());
    QVERIFY(tabWidget->tabsClosable());
    QVERIFY(tabWidget->isMovable());
    QVERIFY(tabWidget->documentMode());
    QVERIFY(tabWidget->cornerWidget(Qt::TopRightCorner) != nullptr);
    QCOMPARE(tabWidget->tabText(0), kNewSessionTitle);

    QVERIFY(w.windowTitle().contains(QStringLiteral(APP_DISPLAY_NAME)));
    QVERIFY(w.windowTitle().contains(QStringLiteral(APP_VERSION)));

    // The menus of the contract exist (Window between View and Language) and the System Log
    // dock is present but hidden.
    for (const char* menu : {"menuFile", "menuSession", "menuEdit", "menuView", "menuWindow", "menuLanguage", "menuHelp"}) {
        QVERIFY2(child<QMenu>(&w, menu) != nullptr, menu);
    }
    const QList<QAction*> menuBarActions = w.menuBar()->actions();
    const qsizetype viewIndex = menuBarActions.indexOf(child<QMenu>(&w, "menuView")->menuAction());
    QVERIFY(viewIndex >= 0);
    QCOMPARE(menuBarActions.at(viewIndex + 1), child<QMenu>(&w, "menuWindow")->menuAction());
    QCOMPARE(menuBarActions.at(viewIndex + 2), child<QMenu>(&w, "menuLanguage")->menuAction());
    // Help opens on a local dialog (keyboard default), not on the external homepage link.
    auto* help = child<QMenu>(&w, "menuHelp");
    QCOMPARE(help->actions().first(), action(w, "actionVersion"));
    QCOMPARE(help->actions().last(), action(w, "actionAbout"));
    auto* dock = child<QDockWidget>(&w, "systemLogDock");
    QVERIFY(dock);
    QVERIFY(dock->isHidden());
    QVERIFY(child<SystemLogViewer>(&w, "systemLogViewer") != nullptr);
    QCOMPARE(SystemLogViewer::instance(), child<SystemLogViewer>(&w, "systemLogViewer"));
}

void Tst_mainwindow::actionsExistWithShortcuts()
{
    MainWindow w;
    for (const ActionSpec& spec : kActions) {
        QAction* a = action(w, spec.name);
        QVERIFY2(a != nullptr, spec.name);
        QVERIFY2(!a->text().isEmpty(), spec.name);
        const QString portable = QString::fromLatin1(spec.shortcut);
        const QKeySequence expected =
            portable.isEmpty() ? QKeySequence() : QKeySequence(portable, QKeySequence::PortableText);
        QVERIFY2(a->shortcut() == expected,
                 qPrintable(QStringLiteral("%1: shortcut is '%2', expected '%3'")
                                .arg(QLatin1String(spec.name), a->shortcut().toString(QKeySequence::PortableText),
                                     QLatin1String(spec.shortcut))));
        QVERIFY2(a->isCheckable() == spec.checkable, spec.name);
    }

    // The language actions are exclusive and exactly one is checked.
    QAction* english = action(w, "actionLanguageEnglish");
    QAction* chinese = action(w, "actionLanguageChinese");
    QVERIFY(english->actionGroup() != nullptr);
    QCOMPARE(english->actionGroup(), chinese->actionGroup());
    QVERIFY(english->actionGroup()->isExclusive());
    QVERIFY(english->isChecked() != chinese->isChecked());
}

void Tst_mainwindow::noBareCtrlLetterShortcuts()
{
    // A bare Ctrl+<letter> that an action reserves is taken away from the shell while connected
    // (Ctrl+W used to close the tab instead of deleting a word), so by default only New Session
    // (Ctrl+T) may use one; the user can still assign others in Preferences > Keyboard.
    MainWindow w;
    const QList<QAction*> actions = w.findChildren<QAction*>();
    QVERIFY(actions.size() >= static_cast<int>(std::size(kActions)));
    for (QAction* a : actions) {
        const QList<QKeySequence> shortcuts = a->shortcuts();
        for (const QKeySequence& sequence : shortcuts) {
            if (sequence.isEmpty()) {
                continue;
            }
            const QKeyCombination combo = sequence[0];
            const bool bareCtrlLetter = combo.keyboardModifiers() == Qt::ControlModifier && combo.key() >= Qt::Key_A &&
                                        combo.key() <= Qt::Key_Z;
            if (bareCtrlLetter) {
                const QString name = a->objectName();
                QVERIFY2(name == QLatin1String("actionNewSession"),
                         qPrintable(name + QStringLiteral(" uses the bare shortcut ") +
                                    sequence.toString(QKeySequence::PortableText)));
            }
        }
    }
}

void Tst_mainwindow::triggerEveryActionDisconnected()
{
    MainWindow w;
    QVERIFY(showAndActivate(w));

    for (const ActionSpec& spec : kActions) {
        if (qstrcmp(spec.name, "actionQuit") == 0) {
            continue;   // closes the window; covered by quitAction()
        }
        QAction* a = action(w, spec.name);
        QVERIFY2(a != nullptr, spec.name);
        ModalDismisser dismisser(ModalDismisser::Answer::Reject);
        a->trigger();
        QCoreApplication::processEvents();
        dismisser.stop();
        QVERIFY2(w.sessionCount() >= 1, spec.name);
        QVERIFY2(w.currentSession() != nullptr, spec.name);
        QVERIFY2(QApplication::activeModalWidget() == nullptr, spec.name);

        // Actions that open modal dialogs must actually have shown one (and had it rejected).
        const bool opensModal =
            qstrcmp(spec.name, "actionFind") == 0 || qstrcmp(spec.name, "actionQuickCommands") == 0 ||
            qstrcmp(spec.name, "actionPreferences") == 0 || qstrcmp(spec.name, "actionStartLogging") == 0 ||
            qstrcmp(spec.name, "actionReplayLog") == 0 || qstrcmp(spec.name, "actionSshProfiles") == 0;
        if (opensModal) {
            QVERIFY2(dismisser.count() >= 1, spec.name);
        }
    }
    // Language ended on Chinese (list order); switch back so later checks see English text.
    action(w, "actionLanguageEnglish")->trigger();
    QVERIFY(w.isVisible());

    // Modeless dialogs (About / Version) are children of the window: close them.
    const QList<QDialog*> dialogs = w.findChildren<QDialog*>();
    for (QDialog* dialog : dialogs) {
        dialog->close();
    }
}

void Tst_mainwindow::triggerEveryActionConnected()
{
    MainWindow w;
    QVERIFY(showAndActivate(w));
    QVERIFY(connectCurrent(w, kLoopback));
    SessionWidget* session = w.currentSession();
    AppSettings::instance().setConfirmCloseWhenConnected(false);

    for (const ActionSpec& spec : kActions) {
        // Keep the session connected and current for the whole pass.
        if (qstrcmp(spec.name, "actionQuit") == 0 || qstrcmp(spec.name, "actionCloseSession") == 0 ||
            qstrcmp(spec.name, "actionNewSession") == 0 || qstrcmp(spec.name, "actionNewSshSession") == 0 ||
            qstrcmp(spec.name, "actionDisconnect") == 0 || qstrcmp(spec.name, "actionConnect") == 0) {
            continue;
        }
        QAction* a = action(w, spec.name);
        QVERIFY2(a != nullptr, spec.name);
        ModalDismisser dismisser(ModalDismisser::Answer::Reject);
        a->trigger();
        QCoreApplication::processEvents();
        dismisser.stop();
        QVERIFY2(session->isConnected(), spec.name);
        QVERIFY2(w.currentSession() == session, spec.name);
        QVERIFY2(QApplication::activeModalWidget() == nullptr, spec.name);
    }
    action(w, "actionLanguageEnglish")->trigger();

    // Send File opened a modeless dialog while connected; the language pass toggled Hex View.
    QVERIFY(!w.findChildren<QDialog*>().isEmpty());
    const QList<QDialog*> dialogs = w.findChildren<QDialog*>();
    for (QDialog* dialog : dialogs) {
        dialog->close();
    }
    action(w, "actionDisconnect")->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(!session->isConnected(), kSimTimeoutMs);
}

void Tst_mainwindow::quitAction()
{
    MainWindow w;
    QVERIFY(showAndActivate(w));
    w.currentSession()->setPortName(kLoopback);
    QVERIFY(AppSettings::instance().mainWindowGeometry().isEmpty());

    action(w, "actionQuit")->trigger();
    QTRY_VERIFY(!w.isVisible());

    // closeEvent saved the window state and the open ports.
    QVERIFY(!AppSettings::instance().mainWindowGeometry().isEmpty());
    QVERIFY(!AppSettings::instance().mainWindowState().isEmpty());
    QCOMPARE(AppSettings::instance().lastOpenPorts(), QStringList{kLoopback});
    QCOMPARE(AppSettings::instance().lastPortName(), kLoopback);
}

void Tst_mainwindow::quitAsksWhenConnected()
{
    MainWindow w;
    QVERIFY(showAndActivate(w));
    QVERIFY(connectCurrent(w, kLoopback));
    SessionWidget* session = w.currentSession();

    {
        ModalDismisser dismisser(ModalDismisser::Answer::No);
        action(w, "actionQuit")->trigger();
        dismisser.stop();
        QCOMPARE(dismisser.count(), 1);
        QCOMPARE(dismisser.classNames().first(), QStringLiteral("QMessageBox"));
        // The quit variant says "Quit", not "Close Session" (which would read as "close this tab").
        QVERIFY(dismisser.windowTitles().first().startsWith(QStringLiteral("Quit")));
    }
    QVERIFY(w.isVisible());
    QVERIFY(session->isConnected());

    {
        ModalDismisser dismisser(ModalDismisser::Answer::Yes);
        action(w, "actionQuit")->trigger();
        dismisser.stop();
        QCOMPARE(dismisser.count(), 1);
    }
    QTRY_VERIFY(!w.isVisible());
    QVERIFY(!session->isConnected());
}

void Tst_mainwindow::publicApiForMain()
{
    // main.cpp drives the window through these: newSession(port), currentSession(),
    // sessionAt(), sessionCount() and the "tabWidget" child.
    QVERIFY(MainWindow::staticMetaObject.indexOfSlot("newSession(QString)") >= 0);
    QVERIFY(MainWindow::staticMetaObject.indexOfSlot("newSession()") >= 0);
    QVERIFY(MainWindow::staticMetaObject.indexOfSlot("closeSession(int)") >= 0);
    QVERIFY(MainWindow::staticMetaObject.indexOfSlot("closeCurrentSession()") >= 0);

    MainWindow w;
    QVERIFY(showAndActivate(w));
    SessionWidget* session = w.newSession(kMcu);
    QVERIFY(session);
    QCOMPARE(session->portName(), kMcu);
    QCOMPARE(w.currentSession(), session);
    QCOMPARE(w.sessionAt(w.sessionCount() - 1), session);
    QCOMPARE(w.sessionCount(), 2);
    QCOMPARE(tabs(w)->currentIndex(), 1);
    QVERIFY(w.windowTitle().endsWith(QStringLiteral(" - ") + kMcu));
}

// =======================================================================================
// Tabs
// =======================================================================================

void Tst_mainwindow::newSessionAction()
{
    MainWindow w;
    QVERIFY(showAndActivate(w));
    QCOMPARE(w.sessionCount(), 1);
    SessionWidget* first = w.currentSession();

    action(w, "actionNewSession")->trigger();
    QCOMPARE(w.sessionCount(), 2);
    QVERIFY(w.currentSession() != first);
    QCOMPARE(w.currentSession(), w.sessionAt(1));
    QVERIFY(w.currentSession()->portName().isEmpty());
    QCOMPARE(tabs(w)->tabText(1), kNewSessionTitle);
    QVERIFY(action(w, "actionNextTab")->isEnabled());
    QVERIFY(action(w, "actionPreviousTab")->isEnabled());
}

void Tst_mainwindow::newSessionShortcut()
{
    MainWindow w;
    QVERIFY(showAndActivate(w));
    QCOMPARE(w.sessionCount(), 1);
    // Connected and focused: the terminal has input enabled and must still pass Ctrl+T through
    // to the window's action instead of sending 0x14.
    QVERIFY(connectCurrent(w, kLoopback));
    SessionWidget* session = w.currentSession();
    session->focusTerminal();
    QTRY_COMPARE(QApplication::focusWidget(), static_cast<QWidget*>(session->terminal()));

    // The key goes to the terminal (not the window) so its ShortcutOverride decision is exercised.
    QTest::keyClick(session->terminal(), Qt::Key_T, Qt::ControlModifier);
    QTRY_COMPARE(w.sessionCount(), 2);

    // Ctrl+Shift+W closes the new (disconnected) session again (Ctrl+W belongs to the shell).
    QTest::keyClick(w.currentSession()->terminal(), Qt::Key_W, Qt::ControlModifier | Qt::ShiftModifier);
    QTRY_COMPARE(w.sessionCount(), 1);
}

void Tst_mainwindow::closeSessionRemovesTab()
{
    MainWindow w;
    QVERIFY(showAndActivate(w));
    SessionWidget* first = w.currentSession();
    w.newSession(kMcu);
    QCOMPARE(w.sessionCount(), 2);

    action(w, "actionCloseSession")->trigger();
    QCOMPARE(w.sessionCount(), 1);
    QCOMPARE(w.currentSession(), first);
    QCOMPARE(tabs(w)->tabText(0), kNewSessionTitle);
    QVERIFY(!action(w, "actionNextTab")->isEnabled());

    // Out-of-range indices are ignored.
    w.closeSession(-1);
    w.closeSession(7);
    QCOMPARE(w.sessionCount(), 1);
}

void Tst_mainwindow::closeSessionNeverZeroTabs()
{
    MainWindow w;
    QVERIFY(showAndActivate(w));
    QCOMPARE(w.sessionCount(), 1);
    SessionWidget* only = w.currentSession();
    only->setPortName(kLoopback);

    action(w, "actionCloseSession")->trigger();
    QCOMPARE(w.sessionCount(), 1);
    QVERIFY(w.currentSession() != nullptr);
    QVERIFY(w.currentSession() != only);   // a fresh, empty session replaced it
    QVERIFY(w.currentSession()->portName().isEmpty());
    QCOMPARE(tabs(w)->tabText(0), kNewSessionTitle);

    w.closeCurrentSession();
    QCOMPARE(w.sessionCount(), 1);
}

void Tst_mainwindow::nextPreviousTabCycle()
{
    MainWindow w;
    QVERIFY(showAndActivate(w));
    QVERIFY(!action(w, "actionNextTab")->isEnabled());
    QVERIFY(!action(w, "actionPreviousTab")->isEnabled());

    w.newSession(kLoopback);
    w.newSession(kMcu);
    QCOMPARE(w.sessionCount(), 3);
    QTabWidget* tabWidget = tabs(w);
    QCOMPARE(tabWidget->currentIndex(), 2);

    action(w, "actionNextTab")->trigger();
    QCOMPARE(tabWidget->currentIndex(), 0);
    action(w, "actionNextTab")->trigger();
    QCOMPARE(tabWidget->currentIndex(), 1);
    action(w, "actionPreviousTab")->trigger();
    QCOMPARE(tabWidget->currentIndex(), 0);
    action(w, "actionPreviousTab")->trigger();
    QCOMPARE(tabWidget->currentIndex(), 2);
    QCOMPARE(w.currentSession()->portName(), kMcu);

    // The shortcuts work too.
    QTest::keyClick(&w, Qt::Key_Tab, Qt::ControlModifier);
    QTRY_COMPARE(tabWidget->currentIndex(), 0);
    QTest::keyClick(&w, Qt::Key_Backtab, Qt::ControlModifier | Qt::ShiftModifier);
    QTRY_COMPARE(tabWidget->currentIndex(), 2);
}

void Tst_mainwindow::tabTextAndTooltip()
{
    MainWindow w;
    QVERIFY(showAndActivate(w));
    QTabWidget* tabWidget = tabs(w);
    QCOMPARE(tabWidget->tabText(0), kNewSessionTitle);
    QVERIFY(!tabWidget->tabIcon(0).isNull());

    w.currentSession()->setPortName(kLoopback);
    QCOMPARE(tabWidget->tabText(0), kLoopback);
    QCOMPARE(tabWidget->tabText(0), w.currentSession()->title());
    const QString tip = tabWidget->tabToolTip(0);
    QVERIFY2(tip.contains(kLoopback), qPrintable(tip));
    QVERIFY2(tip.contains(QStringLiteral("115200 8N1")), qPrintable(tip));
    QVERIFY2(tip.contains(SerialConnection::stateText(SerialConnection::State::Disconnected)), qPrintable(tip));

    QVERIFY(connectCurrent(w, kLoopback));
    QTRY_VERIFY(tabWidget->tabToolTip(0).contains(SerialConnection::stateText(SerialConnection::State::Connected)));
    QVERIFY(!tabWidget->tabIcon(0).isNull());
}

void Tst_mainwindow::tabChangeUpdatesTitleAndStatus()
{
    MainWindow w;
    QVERIFY(showAndActivate(w));
    w.currentSession()->setPortName(kLoopback);
    w.newSession(kMcu);
    QTabWidget* tabWidget = tabs(w);
    QLabel* connection = statusLabel(w, "statusConnectionLabel");
    QVERIFY(connection);

    QVERIFY(w.windowTitle().endsWith(kMcu));
    QVERIFY(connection->text().contains(kMcu));

    tabWidget->setCurrentIndex(0);
    QCOMPARE(w.currentSession()->portName(), kLoopback);
    QVERIFY(w.windowTitle().endsWith(kLoopback));
    QVERIFY(connection->text().contains(kLoopback));
    QVERIFY(!connection->text().contains(kMcu));

    tabWidget->setCurrentIndex(1);
    QVERIFY(w.windowTitle().endsWith(kMcu));
    QVERIFY(connection->text().contains(kMcu));
}

// =======================================================================================
// View
// =======================================================================================

void Tst_mainwindow::hexViewToggle()
{
    MainWindow w;
    QVERIFY(showAndActivate(w));
    SessionWidget* session = w.currentSession();
    QAction* hex = action(w, "actionHexView");
    QVERIFY(hex->isCheckable());
    QVERIFY(!hex->isChecked());
    QCOMPARE(session->viewMode(), SessionWidget::ViewMode::Terminal);

    hex->trigger();
    QVERIFY(hex->isChecked());
    QCOMPARE(session->viewMode(), SessionWidget::ViewMode::HexDump);

    hex->trigger();
    QVERIFY(!hex->isChecked());
    QCOMPARE(session->viewMode(), SessionWidget::ViewMode::Terminal);

    // The action follows a view mode changed from the session side too.
    session->setViewMode(SessionWidget::ViewMode::HexDump);
    QVERIFY(hex->isChecked());
    session->setViewMode(SessionWidget::ViewMode::Terminal);
    QVERIFY(!hex->isChecked());

    // A new tab starts in terminal mode and the action reflects the *current* tab.
    session->setViewMode(SessionWidget::ViewMode::HexDump);
    w.newSession();
    QVERIFY(!hex->isChecked());
    tabs(w)->setCurrentIndex(0);
    QVERIFY(hex->isChecked());
}

void Tst_mainwindow::hexViewShortcut()
{
    MainWindow w;
    QVERIFY(showAndActivate(w));
    SessionWidget* session = w.currentSession();
    session->focusTerminal();

    QTest::keyClick(&w, Qt::Key_H, Qt::ControlModifier | Qt::ShiftModifier);
    QTRY_COMPARE(session->viewMode(), SessionWidget::ViewMode::HexDump);
    QTest::keyClick(&w, Qt::Key_H, Qt::ControlModifier | Qt::ShiftModifier);
    QTRY_COMPARE(session->viewMode(), SessionWidget::ViewMode::Terminal);

    // Ctrl+Shift+L clears (no crash, view stays), Ctrl+Shift+F opens the Find prompt.
    QTest::keyClick(&w, Qt::Key_L, Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(session->viewMode(), SessionWidget::ViewMode::Terminal);
    ModalDismisser dismisser(ModalDismisser::Answer::Reject);
    QTest::keyClick(&w, Qt::Key_F, Qt::ControlModifier | Qt::ShiftModifier);
    dismisser.stop();
    QCOMPARE(dismisser.count(), 1);
    QCOMPARE(dismisser.classNames().first(), QStringLiteral("QInputDialog"));
}

void Tst_mainwindow::hexViewShortcutWhileConnected()
{
    MainWindow w;
    QVERIFY(showAndActivate(w));
    QVERIFY(connectCurrent(w, kLoopback));
    SessionWidget* session = w.currentSession();
    session->focusTerminal();
    QTRY_COMPARE(QApplication::focusWidget(), static_cast<QWidget*>(session->terminal()));

    // Ask the terminal whether it claims the key while input is enabled (the decision it makes
    // for the real ShortcutOverride): Ctrl+Shift+<anything> always passes through, and the key
    // is reserved anyway (the window pushed its shortcuts to the terminal).
    QKeyEvent probe(QEvent::ShortcutOverride, Qt::Key_H, Qt::ControlModifier | Qt::ShiftModifier, QStringLiteral("H"));
    probe.ignore();
    QApplication::sendEvent(session->terminal(), &probe);
    QVERIFY2(!probe.isAccepted(), "TerminalWidget claims Ctrl+Shift+H while connected");
    QVERIFY(session->terminal()->reservedShortcuts().contains(seq("Ctrl+Shift+H")));

    QTest::keyClick(&w, Qt::Key_H, Qt::ControlModifier | Qt::ShiftModifier);
    QTRY_COMPARE(session->viewMode(), SessionWidget::ViewMode::HexDump);
}

void Tst_mainwindow::showCommandInputToggle()
{
    MainWindow w;
    QVERIFY(showAndActivate(w));
    w.newSession(kMcu);
    QAction* show = action(w, "actionShowCommandInput");
    QVERIFY(show->isChecked());
    for (int i = 0; i < w.sessionCount(); ++i) {
        QVERIFY(w.sessionAt(i)->commandInput() != nullptr);
        QVERIFY(!w.sessionAt(i)->commandInput()->isHidden());
    }

    show->trigger();
    QVERIFY(!show->isChecked());
    for (int i = 0; i < w.sessionCount(); ++i) {
        QVERIFY(w.sessionAt(i)->commandInput()->isHidden());
    }
    QCOMPARE(QSettings().value(kShowCommandInputKey).toBool(), false);

    // Sessions created afterwards follow the persisted state.
    SessionWidget* later = w.newSession();
    QVERIFY(later->commandInput()->isHidden());

    show->trigger();
    QVERIFY(show->isChecked());
    for (int i = 0; i < w.sessionCount(); ++i) {
        QVERIFY(!w.sessionAt(i)->commandInput()->isHidden());
    }
    QCOMPARE(QSettings().value(kShowCommandInputKey).toBool(), true);
}

void Tst_mainwindow::showQuickCommandsToggle()
{
    MainWindow w;
    QVERIFY(showAndActivate(w));
    w.newSession(kMcu);
    QAction* show = action(w, "actionShowQuickCommands");
    QVERIFY(show->isChecked());
    for (int i = 0; i < w.sessionCount(); ++i) {
        QVERIFY(w.sessionAt(i)->quickCommandBar() != nullptr);
        QVERIFY(!w.sessionAt(i)->quickCommandBar()->isHidden());
    }

    show->trigger();
    QVERIFY(!show->isChecked());
    for (int i = 0; i < w.sessionCount(); ++i) {
        QVERIFY(w.sessionAt(i)->quickCommandBar()->isHidden());
    }
    QCOMPARE(QSettings().value(kShowQuickCommandsKey).toBool(), false);

    SessionWidget* later = w.newSession();
    QVERIFY(later->quickCommandBar()->isHidden());

    show->trigger();
    QVERIFY(show->isChecked());
    for (int i = 0; i < w.sessionCount(); ++i) {
        QVERIFY(!w.sessionAt(i)->quickCommandBar()->isHidden());
    }
    QCOMPARE(QSettings().value(kShowQuickCommandsKey).toBool(), true);
}

void Tst_mainwindow::panelVisibilityPersistsAcrossWindows()
{
    {
        MainWindow first;
        QVERIFY(showAndActivate(first));
        action(first, "actionShowCommandInput")->trigger();
        action(first, "actionShowQuickCommands")->trigger();
        QVERIFY(!action(first, "actionShowCommandInput")->isChecked());
        QVERIFY(!action(first, "actionShowQuickCommands")->isChecked());
    }
    QCOMPARE(QSettings().value(kShowCommandInputKey).toBool(), false);
    QCOMPARE(QSettings().value(kShowQuickCommandsKey).toBool(), false);

    MainWindow second;
    QVERIFY(showAndActivate(second));
    QVERIFY(!action(second, "actionShowCommandInput")->isChecked());
    QVERIFY(!action(second, "actionShowQuickCommands")->isChecked());
    QVERIFY(second.currentSession()->commandInput()->isHidden());
    QVERIFY(second.currentSession()->quickCommandBar()->isHidden());
}

void Tst_mainwindow::pauseWhileSelectingToggle()
{
    MainWindow w;
    QVERIFY(showAndActivate(w));
    w.newSession(kMcu);
    QAction* pause = action(w, "actionPauseWhileSelecting");
    QVERIFY(pause);
    QVERIFY(pause->isCheckable());
    QVERIFY(pause->isChecked());   // AppSettings default: on
    QVERIFY(AppSettings::instance().pauseWhileSelecting());
    QVERIFY(!pause->statusTip().isEmpty());
    for (int i = 0; i < w.sessionCount(); ++i) {
        QVERIFY(w.sessionAt(i)->terminal()->pauseWhileSelecting());
    }
    // View menu, right after Show Quick Commands.
    auto* view = child<QMenu>(&w, "menuView");
    QVERIFY(view);
    const QList<QAction*> viewActions = view->actions();
    const qsizetype index = viewActions.indexOf(pause);
    QVERIFY(index > 0);
    QCOMPARE(viewActions.at(index - 1), action(w, "actionShowQuickCommands"));

    // Toggling writes the setting and reaches every session's terminal.
    pause->trigger();
    QVERIFY(!pause->isChecked());
    QVERIFY(!AppSettings::instance().pauseWhileSelecting());
    QCOMPARE(QSettings().value(QStringLiteral("terminal/pauseWhileSelecting")).toBool(), false);
    for (int i = 0; i < w.sessionCount(); ++i) {
        QVERIFY(!w.sessionAt(i)->terminal()->pauseWhileSelecting());
    }
    SessionWidget* later = w.newSession();
    QVERIFY(!later->terminal()->pauseWhileSelecting());

    // A settings write from elsewhere (the Preferences dialog) re-checks the action.
    AppSettings::instance().setPauseWhileSelecting(true);
    QVERIFY(pause->isChecked());
    for (int i = 0; i < w.sessionCount(); ++i) {
        QVERIFY(w.sessionAt(i)->terminal()->pauseWhileSelecting());
    }

    // Unchecking while a terminal is paused resumes it (the selection is kept).
    TerminalWidget* terminal = w.currentSession()->terminal();
    terminal->feedData("hello\r\n");
    terminal->selectAll();
    QVERIFY(terminal->isOutputPaused());
    terminal->feedData("queued\r\n");
    QVERIFY(terminal->screen()->lineText(1).isEmpty());
    pause->trigger();
    QVERIFY(!pause->isChecked());
    QVERIFY(!terminal->isOutputPaused());
    QVERIFY(terminal->hasSelection());
    QCOMPARE(terminal->screen()->lineText(1), QStringLiteral("queued"));

    // A window built later starts from the persisted state.
    MainWindow second;
    QVERIFY(!action(second, "actionPauseWhileSelecting")->isChecked());
    QVERIFY(!second.currentSession()->terminal()->pauseWhileSelecting());
}

void Tst_mainwindow::pauseStatusMessage()
{
    MainWindow w;
    QVERIFY(showAndActivate(w));
    SessionWidget* session = w.currentSession();
    TerminalWidget* terminal = session->terminal();
    terminal->feedData("select me\r\n");
    w.statusBar()->clearMessage();

    // A persistent hint while the display is frozen ...
    terminal->selectAll();
    QVERIFY(terminal->isOutputPaused());
    QVERIFY2(w.statusBar()->currentMessage().contains(QStringLiteral("Output paused")),
             qPrintable(w.statusBar()->currentMessage()));
    terminal->feedData("queued\r\n");
    QTest::qWait(50);
    QVERIFY(w.statusBar()->currentMessage().contains(QStringLiteral("Output paused")));

    // ... cleared again on resume (an empty session message clears the status bar).
    session->focusTerminal();
    QTest::keyClick(terminal, Qt::Key_Escape);
    QVERIFY(!terminal->isOutputPaused());
    QVERIFY2(w.statusBar()->currentMessage().isEmpty(), qPrintable(w.statusBar()->currentMessage()));
    QCOMPARE(terminal->screen()->lineText(1), QStringLiteral("queued"));

    // A background tab's pause does not touch the status bar.
    w.newSession();
    QVERIFY(w.currentSession() != session);
    w.statusBar()->clearMessage();
    terminal->selectAll();
    QVERIFY(terminal->isOutputPaused());
    QVERIFY(w.statusBar()->currentMessage().isEmpty());
    terminal->clearSelection();
    QVERIFY(!terminal->isOutputPaused());
}

void Tst_mainwindow::pauseHintFollowsCurrentTab()
{
    // The persistent "Output paused" hint belongs to the tab that posted it: it leaves with a tab
    // switch, comes back with the tab, survives a transient message and dies with the tab.
    const QString hint = QStringLiteral("Output paused");
    MainWindow w;
    QVERIFY(showAndActivate(w));
    SessionWidget* first = w.currentSession();
    TerminalWidget* t1 = first->terminal();
    t1->feedData("first tab\r\n");
    w.statusBar()->clearMessage();
    t1->selectAll();
    QVERIFY(t1->isOutputPaused());
    QVERIFY(w.statusBar()->currentMessage().contains(hint));
    t1->feedData("queued while in the background\r\n");

    // Switching to another tab drops the first tab's hint (the tab itself stays paused) ...
    SessionWidget* second = w.newSession();
    QCOMPARE(w.currentSession(), second);
    QVERIFY2(!w.statusBar()->currentMessage().contains(hint), qPrintable(w.statusBar()->currentMessage()));
    QVERIFY(t1->isOutputPaused());
    QCOMPARE(t1->pendingPausedBytes(), qint64(32));
    // ... and switching back shows it again.
    tabs(w)->setCurrentIndex(0);
    QCOMPARE(w.currentSession(), first);
    QVERIFY(w.statusBar()->currentMessage().contains(hint));

    // A transient message replaces the hint for a moment; the hint returns once it expires.
    w.statusBar()->showMessage(QStringLiteral("transient"), 100);
    QCOMPARE(w.statusBar()->currentMessage(), QStringLiteral("transient"));
    QTRY_VERIFY_WITH_TIMEOUT(w.statusBar()->currentMessage().contains(hint), 3000);
    // A session message too ("Not connected", 3 s): a resume in the meantime clears everything
    // and the expired transient must not bring the hint back.
    first->sendBytes(QByteArrayLiteral("x"));
    QCOMPARE(w.statusBar()->currentMessage(), QStringLiteral("Not connected"));
    first->focusTerminal();
    QTest::keyClick(t1, Qt::Key_Escape);
    QVERIFY(!t1->isOutputPaused());
    QCOMPARE(t1->screen()->lineText(1), QStringLiteral("queued while in the background"));
    QVERIFY(!w.statusBar()->currentMessage().contains(hint));
    QTest::qWait(150);
    QVERIFY(!w.statusBar()->currentMessage().contains(hint));

    // Both tabs paused: each shows its own hint; resuming one does not clear the other's.
    TerminalWidget* t2 = second->terminal();
    t2->feedData("second tab\r\n");
    t1->selectAll();
    QVERIFY(t1->isOutputPaused());
    QVERIFY(w.statusBar()->currentMessage().contains(hint));
    tabs(w)->setCurrentIndex(1);
    QVERIFY(!w.statusBar()->currentMessage().contains(hint));
    t2->selectAll();
    QVERIFY(t2->isOutputPaused());
    QVERIFY(w.statusBar()->currentMessage().contains(hint));
    tabs(w)->setCurrentIndex(0);
    QVERIFY(w.statusBar()->currentMessage().contains(hint));   // first is still paused
    t1->clearSelection();
    QVERIFY(!w.statusBar()->currentMessage().contains(hint));
    tabs(w)->setCurrentIndex(1);
    QVERIFY(w.statusBar()->currentMessage().contains(hint));   // second still is

    // Closing the paused tab with a queue: no crash, and its hint leaves with it.
    t2->feedData(QByteArray(10000, 'q'));
    QVERIFY(t2->isOutputPaused());
    action(w, "actionCloseSession")->trigger();
    QCOMPARE(w.sessionCount(), 1);
    QCOMPARE(w.currentSession(), first);
    QVERIFY2(!w.statusBar()->currentMessage().contains(hint), qPrintable(w.statusBar()->currentMessage()));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);   // the widget with the queue is gone now
    QTest::qWait(150);   // past its badge interval: nothing fires into freed memory
    QVERIFY(!w.statusBar()->currentMessage().contains(hint));
    QVERIFY(!t1->isOutputPaused());
}

void Tst_mainwindow::rightClickPastesToggle()
{
    MainWindow w;
    QVERIFY(showAndActivate(w));
    w.newSession(kMcu);
    QAction* rightClick = action(w, "actionRightClickPastes");
    QVERIFY(rightClick);
    QVERIFY(rightClick->isCheckable());
    QVERIFY(rightClick->isChecked());   // AppSettings default: on
    QVERIFY(AppSettings::instance().rightClickPastes());
    QVERIFY(!rightClick->statusTip().isEmpty());
    QVERIFY(rightClick->text().contains(QStringLiteral("cmd.exe")));
    for (int i = 0; i < w.sessionCount(); ++i) {
        QVERIFY(w.sessionAt(i)->terminal()->rightClickPastes());
    }
    // View menu, right after Pause Output While Selecting.
    auto* view = child<QMenu>(&w, "menuView");
    QVERIFY(view);
    const QList<QAction*> viewActions = view->actions();
    const qsizetype index = viewActions.indexOf(rightClick);
    QVERIFY(index > 0);
    QCOMPARE(viewActions.at(index - 1), action(w, "actionPauseWhileSelecting"));

    // Toggling writes the setting and reaches every session's terminal, current or not.
    rightClick->trigger();
    QVERIFY(!rightClick->isChecked());
    QVERIFY(!AppSettings::instance().rightClickPastes());
    QCOMPARE(QSettings().value(QStringLiteral("terminal/rightClickPastes")).toBool(), false);
    for (int i = 0; i < w.sessionCount(); ++i) {
        QVERIFY(!w.sessionAt(i)->terminal()->rightClickPastes());
    }
    QVERIFY(!w.currentSession()->terminal()->rightClickPastes());
    SessionWidget* later = w.newSession();
    QVERIFY(!later->terminal()->rightClickPastes());

    // A settings write from elsewhere (the Preferences dialog) re-checks the action.
    AppSettings::instance().setRightClickPastes(true);
    QVERIFY(rightClick->isChecked());
    for (int i = 0; i < w.sessionCount(); ++i) {
        QVERIFY(w.sessionAt(i)->terminal()->rightClickPastes());
    }
    QVERIFY(w.currentSession()->terminal()->rightClickPastes());

    // Through the whole stack: a right click on the current (disconnected) session's terminal
    // copies its selection; the other setting is not touched by this one.
    TerminalWidget* terminal = w.currentSession()->terminal();
    terminal->feedData("copy me\r\n");
    terminal->selectAll();
    QCOMPARE(terminal->selectedText(), QStringLiteral("copy me"));
    QApplication::clipboard()->clear();
    QWidget* vp = terminal->viewport();
    QTest::mouseClick(vp, Qt::RightButton, Qt::NoModifier, QPoint(vp->width() / 2, vp->height() / 2));
    QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("copy me"));
    QVERIFY(!terminal->hasSelection());
    QVERIFY(action(w, "actionPauseWhileSelecting")->isChecked());
    QVERIFY(AppSettings::instance().pauseWhileSelecting());

    // A window built later starts from the persisted state.
    rightClick->trigger();
    QVERIFY(!AppSettings::instance().rightClickPastes());
    MainWindow second;
    QVERIFY(!action(second, "actionRightClickPastes")->isChecked());
    QVERIFY(!second.currentSession()->terminal()->rightClickPastes());
    QVERIFY(action(second, "actionPauseWhileSelecting")->isChecked());
}

void Tst_mainwindow::clearActionWipesEverything()
{
    // Session > Clear / the toolbar button (Ctrl+Shift+L) on the current tab: screen, scrollback
    // and hex view are emptied; another tab is untouched; Reset Terminal stays the full reset.
    MainWindow w;
    QVERIFY(showAndActivate(w));
    SessionWidget* first = w.currentSession();
    TerminalWidget* t1 = first->terminal();
    QByteArray burst;
    for (int i = 0; i < t1->visibleRows() + 50; ++i) {
        burst += "history " + QByteArray::number(i) + "\r\n";
    }
    t1->feedData(burst);
    t1->feedData(QByteArrayLiteral("\x1b[4mstill underlined"));
    QVERIFY(t1->screen()->scrollbackSize() > 0);
    first->hexView()->appendReceived(burst);
    first->hexView()->flushPending();
    QVERIFY(!first->hexView()->toPlainText().isEmpty());
    const Terminal::Attributes attributes = t1->screen()->currentAttributes();
    QVERIFY(attributes != Terminal::Attributes());

    SessionWidget* second = w.newSession();
    TerminalWidget* t2 = second->terminal();
    t2->feedData(burst);
    QVERIFY(t2->screen()->scrollbackSize() > 0);
    tabs(w)->setCurrentIndex(0);
    QCOMPARE(w.currentSession(), first);

    QAction* clear = action(w, "actionClear");
    QVERIFY(clear->isEnabled());
    QVERIFY(!clear->toolTip().isEmpty());
    clear->trigger();
    QCOMPARE(t1->screen()->scrollbackSize(), 0);
    QCOMPARE(t1->screen()->totalLines(), t1->screen()->rows());
    for (int r = 0; r < t1->screen()->rows(); ++r) {
        QVERIFY2(t1->screen()->line(r).text().isEmpty(), qPrintable(QStringLiteral("row %1 not blank").arg(r)));
    }
    QCOMPARE(t1->screen()->cursor().row, 0);
    QCOMPARE(t1->screen()->cursor().col, 0);
    QVERIFY(first->hexView()->toPlainText().isEmpty());
    QVERIFY(t1->screen()->currentAttributes() == attributes);   // Clear is not Reset
    QVERIFY(t2->screen()->scrollbackSize() > 0);                 // the other tab keeps its output

    // The shortcut takes the same path.
    t1->feedData(burst);
    QVERIFY(t1->screen()->scrollbackSize() > 0);
    first->focusTerminal();
    QTest::keyClick(t1, Qt::Key_L, Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(t1->screen()->scrollbackSize(), 0);
    QVERIFY(t1->screen()->line(0).text().isEmpty());

    action(w, "actionResetTerminal")->trigger();
    QVERIFY(t1->screen()->currentAttributes() == Terminal::Attributes());
}

void Tst_mainwindow::systemLogDockToggle()
{
    MainWindow w;
    QVERIFY(showAndActivate(w));
    auto* dock = child<QDockWidget>(&w, "systemLogDock");
    QVERIFY(dock);
    QAction* toggle = action(w, "actionSystemLog");
    QVERIFY(dock->isHidden());
    QVERIFY(!toggle->isChecked());

    toggle->trigger();
    QTRY_VERIFY(dock->isVisible());
    QVERIFY(toggle->isChecked());
    QCOMPARE(dock->windowTitle(), QStringLiteral("System Log"));

    // The dock's own close button unchecks the menu action.
    dock->close();
    QTRY_VERIFY(!dock->isVisible());
    QTRY_VERIFY(!toggle->isChecked());

    toggle->trigger();
    QTRY_VERIFY(dock->isVisible());
    toggle->trigger();
    QTRY_VERIFY(!dock->isVisible());
    QVERIFY(!toggle->isChecked());

    // The dock's own toggle action drives the menu action as well.
    dock->toggleViewAction()->trigger();
    QTRY_VERIFY(dock->isVisible());
    QVERIFY(toggle->isChecked());
}

void Tst_mainwindow::zoomActions()
{
    MainWindow w;
    QVERIFY(showAndActivate(w));
    TerminalWidget* terminal = w.currentSession()->terminal();
    QVERIFY(terminal);
    const int base = terminal->terminalFont().pointSize();
    QVERIFY(base > 0);

    action(w, "actionZoomIn")->trigger();
    QVERIFY(terminal->terminalFont().pointSize() > base);
    action(w, "actionZoomIn")->trigger();
    const int zoomed = terminal->terminalFont().pointSize();
    QVERIFY(zoomed > base + 1);

    action(w, "actionZoomOut")->trigger();
    QVERIFY(terminal->terminalFont().pointSize() < zoomed);

    action(w, "actionZoomReset")->trigger();
    QCOMPARE(terminal->terminalFont().pointSize(), base);

    action(w, "actionZoomOut")->trigger();
    QVERIFY(terminal->terminalFont().pointSize() < base);
    action(w, "actionZoomReset")->trigger();
    QCOMPARE(terminal->terminalFont().pointSize(), base);
}

// =======================================================================================
// Language
// =======================================================================================

void Tst_mainwindow::languageSwitch()
{
    MainWindow w;
    QVERIFY(showAndActivate(w));
    auto* fileMenu = child<QMenu>(&w, "menuFile");
    QVERIFY(fileMenu);
    QCOMPARE(fileMenu->title(), QStringLiteral("&File"));
    QVERIFY(action(w, "actionLanguageEnglish")->isChecked());
    QCOMPARE(AppSettings::instance().language(), QStringLiteral("en_US"));

    action(w, "actionLanguageChinese")->trigger();
    QTRY_COMPARE(fileMenu->title(), QStringLiteral(u"文件(&F)"));
    QVERIFY(action(w, "actionLanguageChinese")->isChecked());
    QVERIFY(!action(w, "actionLanguageEnglish")->isChecked());
    QCOMPARE(AppSettings::instance().language(), QStringLiteral("zh_CN"));
    QVERIFY(w.windowTitle().contains(QStringLiteral(APP_VERSION)));   // the title format survives
    // Qt's own dialog strings follow too (embedded qtbase_zh_CN.qm; "QPlatformTheme"/"Cancel" is
    // the context/source of the QMessageBox standard buttons).
    QVERIFY(QCoreApplication::translate("QPlatformTheme", "Cancel") != QStringLiteral("Cancel"));

    action(w, "actionLanguageEnglish")->trigger();
    QTRY_COMPARE(fileMenu->title(), QStringLiteral("&File"));
    QVERIFY(action(w, "actionLanguageEnglish")->isChecked());
    QCOMPARE(AppSettings::instance().language(), QStringLiteral("en_US"));
    QCOMPARE(QCoreApplication::translate("QPlatformTheme", "Cancel"), QStringLiteral("Cancel"));

    // Triggering the already-checked language is a no-op.
    action(w, "actionLanguageEnglish")->trigger();
    QCOMPARE(fileMenu->title(), QStringLiteral("&File"));
    QCOMPARE(AppSettings::instance().language(), QStringLiteral("en_US"));
}

void Tst_mainwindow::languageDefault()
{
    QSettings().remove(QStringLiteral("general/language"));
    // AppSettings.h: derived from QLocale::system(); "en_US" everywhere except Chinese systems.
    const QString expected = QLocale::system().language() == QLocale::Chinese ? QStringLiteral("zh_CN")
                                                                              : QStringLiteral("en_US");
    QCOMPARE(AppSettings::instance().language(), expected);
    QVERIFY(expected == QStringLiteral("en_US") || expected == QStringLiteral("zh_CN"));

    // The window pre-checks the matching language action and persists the resolved code.
    MainWindow w;
    QAction* english = action(w, "actionLanguageEnglish");
    QAction* chinese = action(w, "actionLanguageChinese");
    QCOMPARE(english->isChecked(), expected == QStringLiteral("en_US"));
    QCOMPARE(chinese->isChecked(), expected == QStringLiteral("zh_CN"));
    QCOMPARE(AppSettings::instance().language(), expected);

    // A stored Chinese preference is honoured on start (and normalised).
    AppSettings::instance().setLanguage(QStringLiteral("zh"));
    MainWindow zh;
    QVERIFY(action(zh, "actionLanguageChinese")->isChecked());
    QCOMPARE(AppSettings::instance().language(), QStringLiteral("zh_CN"));
    AppSettings::instance().setLanguage(QStringLiteral("en_US"));
}

// =======================================================================================
// Title / status bar
// =======================================================================================

void Tst_mainwindow::windowTitleFollowsPort()
{
    MainWindow w;
    QVERIFY(showAndActivate(w));
    const QString base = QStringLiteral("%1 %2").arg(QStringLiteral(APP_DISPLAY_NAME), QStringLiteral(APP_VERSION));
    QCOMPARE(w.windowTitle(), base);

    w.currentSession()->setPortName(kLoopback);
    QCOMPARE(w.windowTitle(), base + QStringLiteral(" - ") + kLoopback);

    QVERIFY(connectCurrent(w, kLoopback));
    QCOMPARE(w.windowTitle(), base + QStringLiteral(" - ") + kLoopback);

    w.newSession();
    QCOMPARE(w.windowTitle(), base);
    tabs(w)->setCurrentIndex(0);
    QCOMPARE(w.windowTitle(), base + QStringLiteral(" - ") + kLoopback);
}

void Tst_mainwindow::connectDisconnectActions()
{
    MainWindow w;
    QVERIFY(showAndActivate(w));
    SessionWidget* session = w.currentSession();
    QAction* connectAction = action(w, "actionConnect");
    QAction* disconnectAction = action(w, "actionDisconnect");

    // No port selected: nothing to connect to.
    QVERIFY(!connectAction->isEnabled());
    QVERIFY(!disconnectAction->isEnabled());
    QVERIFY(!action(w, "actionSendFile")->isEnabled());
    QVERIFY(!action(w, "actionSendBreak")->isEnabled());
    QVERIFY(!action(w, "actionSyncTerminalSize")->isEnabled());
    QVERIFY(!action(w, "actionPaste")->isEnabled());   // the terminal drops input while disconnected
    QVERIFY(action(w, "actionCopy")->isEnabled());
    QVERIFY(action(w, "actionClear")->isEnabled());
    QVERIFY(action(w, "actionStartLogging")->isEnabled());
    QVERIFY(!action(w, "actionStopLogging")->isEnabled());

    session->setPortName(kLoopback);
    QVERIFY(connectAction->isEnabled());

    connectAction->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(session->isConnected(), kSimTimeoutMs);
    QTRY_VERIFY(!connectAction->isEnabled());
    QVERIFY(disconnectAction->isEnabled());
    QVERIFY(action(w, "actionSendFile")->isEnabled());
    QVERIFY(action(w, "actionSendBreak")->isEnabled());
    QVERIFY(action(w, "actionSyncTerminalSize")->isEnabled());
    QVERIFY(action(w, "actionPaste")->isEnabled());

    disconnectAction->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(!session->isConnected(), kSimTimeoutMs);
    QTRY_VERIFY(connectAction->isEnabled());
    QVERIFY(!disconnectAction->isEnabled());
    QVERIFY(!action(w, "actionSendFile")->isEnabled());
    QVERIFY(!action(w, "actionPaste")->isEnabled());
}

void Tst_mainwindow::statusBarConnected()
{
    MainWindow w;
    QVERIFY(showAndActivate(w));
    QLabel* connection = statusLabel(w, "statusConnectionLabel");
    QVERIFY(connection);
    QCOMPARE(connection->text(), SerialConnection::stateText(SerialConnection::State::Disconnected));

    w.currentSession()->setPortName(kLoopback);
    QVERIFY2(connection->text().startsWith(SerialConnection::stateText(SerialConnection::State::Disconnected)),
             qPrintable(connection->text()));
    QVERIFY(connection->text().contains(kLoopback));
    QVERIFY(connection->text().contains(QStringLiteral("115200 8N1")));

    QVERIFY(connectCurrent(w, kLoopback));
    QTRY_VERIFY(connection->text().startsWith(SerialConnection::stateText(SerialConnection::State::Connected)));
    QVERIFY2(connection->text().contains(kLoopback), qPrintable(connection->text()));
    QVERIFY2(connection->text().contains(QStringLiteral("115200 8N1")), qPrintable(connection->text()));

    action(w, "actionDisconnect")->trigger();
    QTRY_VERIFY(connection->text().startsWith(SerialConnection::stateText(SerialConnection::State::Disconnected)));
}

void Tst_mainwindow::statusBarCounters()
{
    MainWindow w;
    QVERIFY(showAndActivate(w));
    QLabel* counters = statusLabel(w, "statusCountersLabel");
    QVERIFY(counters);
    QCOMPARE(counters->text(), QStringLiteral("RX 0 B  TX 0 B"));

    QVERIFY(connectCurrent(w, kLoopback));
    SessionWidget* session = w.currentSession();
    session->sendBytes(QByteArrayLiteral("hello"));
    QTRY_VERIFY2_WITH_TIMEOUT(counters->text().contains(QStringLiteral("TX 5 B")), qPrintable(counters->text()),
                              kSimTimeoutMs);
    QTRY_VERIFY2_WITH_TIMEOUT(counters->text().contains(QStringLiteral("RX 5 B")), qPrintable(counters->text()),
                              kSimTimeoutMs);

    // Kilobyte formatting once the counters grow.
    session->sendBytes(QByteArray(2048, 'x'));
    QTRY_VERIFY2_WITH_TIMEOUT(counters->text().contains(QStringLiteral("TX 2.0 KB")), qPrintable(counters->text()),
                              kSimTimeoutMs);
    QTRY_VERIFY2_WITH_TIMEOUT(counters->text().contains(QStringLiteral("RX 2.0 KB")), qPrintable(counters->text()),
                              kSimTimeoutMs);

    // A fresh tab shows its own (zero) counters.
    w.newSession();
    QCOMPARE(counters->text(), QStringLiteral("RX 0 B  TX 0 B"));
    tabs(w)->setCurrentIndex(0);
    QVERIFY(counters->text().contains(QStringLiteral("KB")));
}

void Tst_mainwindow::statusBarGridAndEncoding()
{
    MainWindow w;
    w.resize(1000, 700);
    QVERIFY(showAndActivate(w));
    QLabel* grid = statusLabel(w, "statusGridLabel");
    QLabel* encoding = statusLabel(w, "statusEncodingLabel");
    QVERIFY(grid);
    QVERIFY(encoding);
    TerminalWidget* terminal = w.currentSession()->terminal();

    const QRegularExpression pattern(QStringLiteral("^\\d+x\\d+$"));
    QTRY_VERIFY2(pattern.match(grid->text()).hasMatch(), qPrintable(grid->text()));
    QTRY_COMPARE(grid->text(), QStringLiteral("%1x%2").arg(terminal->columns()).arg(terminal->visibleRows()));
    QVERIFY(terminal->columns() >= 20);
    QVERIFY(terminal->visibleRows() >= 5);
    QCOMPARE(encoding->text(), terminal->encoding());
    QCOMPARE(encoding->text(), QStringLiteral("UTF-8"));

    // Zooming changes the grid; the label follows.
    const QString before = grid->text();
    action(w, "actionZoomIn")->trigger();
    action(w, "actionZoomIn")->trigger();
    QTRY_VERIFY(grid->text() != before);
    QTRY_COMPARE(grid->text(), QStringLiteral("%1x%2").arg(terminal->columns()).arg(terminal->visibleRows()));
}

void Tst_mainwindow::statusBarLoggingIndicator()
{
    MainWindow w;
    QVERIFY(showAndActivate(w));
    QLabel* logging = statusLabel(w, "statusLoggingLabel");
    QVERIFY(logging);
    QVERIFY(logging->text().isEmpty());
    SessionWidget* session = w.currentSession();

    const QString logFile = tempPath(QStringLiteral("indicator.log"));
    session->startLoggingTo(logFile);
    QTRY_VERIFY(session->isLogging());
    QTRY_VERIFY2(logging->text().contains(QStringLiteral("LOG")), qPrintable(logging->text()));
    QVERIFY2(logging->toolTip().contains(QStringLiteral("indicator.log")), qPrintable(logging->toolTip()));
    QVERIFY(!action(w, "actionStartLogging")->isEnabled());
    QVERIFY(action(w, "actionStopLogging")->isEnabled());
    QVERIFY(tabs(w)->tabToolTip(0).contains(QStringLiteral("indicator.log")));

    action(w, "actionStopLogging")->trigger();
    QTRY_VERIFY(!session->isLogging());
    QTRY_VERIFY(logging->text().isEmpty());
    QVERIFY(logging->toolTip().isEmpty());
    QVERIFY(action(w, "actionStartLogging")->isEnabled());
    QVERIFY(!action(w, "actionStopLogging")->isEnabled());
    QVERIFY(QFileInfo::exists(logFile));
}

void Tst_mainwindow::statusMessageFromSession()
{
    MainWindow w;
    QVERIFY(showAndActivate(w));
    QVERIFY(connectCurrent(w, kLoopback));
    QTRY_VERIFY2(w.statusBar()->currentMessage().contains(kLoopback), qPrintable(w.statusBar()->currentMessage()));

    // Messages of a background tab do not overwrite the current one.
    SessionWidget* background = w.currentSession();
    w.newSession();
    w.statusBar()->clearMessage();
    background->disconnectPort();
    QCoreApplication::processEvents();
    QVERIFY(w.statusBar()->currentMessage().isEmpty());
}

// =======================================================================================
// Close confirmation
// =======================================================================================

void Tst_mainwindow::closeConnectedTabConfirmYes()
{
    MainWindow w;
    QVERIFY(showAndActivate(w));
    w.newSession();
    QVERIFY(connectCurrent(w, kLoopback));
    QCOMPARE(w.sessionCount(), 2);
    QVERIFY(AppSettings::instance().confirmCloseWhenConnected());

    ModalDismisser dismisser(ModalDismisser::Answer::Yes);
    w.closeCurrentSession();
    dismisser.stop();
    QCOMPARE(dismisser.count(), 1);
    QCOMPARE(dismisser.classNames().first(), QStringLiteral("QMessageBox"));
    QCOMPARE(w.sessionCount(), 1);
    QVERIFY(w.currentSession()->portName().isEmpty());
}

void Tst_mainwindow::closeConnectedTabConfirmNo()
{
    MainWindow w;
    QVERIFY(showAndActivate(w));
    w.newSession();
    QVERIFY(connectCurrent(w, kLoopback));
    SessionWidget* session = w.currentSession();

    ModalDismisser dismisser(ModalDismisser::Answer::No);
    action(w, "actionCloseSession")->trigger();
    dismisser.stop();
    QCOMPARE(dismisser.count(), 1);
    QCOMPARE(w.sessionCount(), 2);
    QCOMPARE(w.currentSession(), session);
    QVERIFY(session->isConnected());
}

void Tst_mainwindow::closeConnectedTabNoConfirmWhenDisabled()
{
    MainWindow w;
    QVERIFY(showAndActivate(w));
    w.newSession();
    QVERIFY(connectCurrent(w, kLoopback));
    AppSettings::instance().setConfirmCloseWhenConnected(false);

    ModalDismisser dismisser(ModalDismisser::Answer::Reject);
    w.closeCurrentSession();
    dismisser.stop();
    QCOMPARE(dismisser.count(), 0);
    QCOMPARE(w.sessionCount(), 1);
}

void Tst_mainwindow::closeDisconnectedTabNeverAsks()
{
    MainWindow w;
    QVERIFY(showAndActivate(w));
    w.newSession(kLoopback);
    QVERIFY(!w.currentSession()->isConnected());

    ModalDismisser dismisser(ModalDismisser::Answer::Reject);
    w.closeCurrentSession();
    dismisser.stop();
    QCOMPARE(dismisser.count(), 0);
    QCOMPARE(w.sessionCount(), 1);
}

// =======================================================================================
// Persistence
// =======================================================================================

void Tst_mainwindow::restoreLastPortsRoundTrip()
{
    {
        MainWindow first;
        QVERIFY(showAndActivate(first));
        first.currentSession()->setPortName(kLoopback);
        first.newSession(kMcu);
        QCOMPARE(first.sessionCount(), 2);
        first.close();
        QTRY_VERIFY(!first.isVisible());
    }
    QCOMPARE(AppSettings::instance().lastOpenPorts(), (QStringList{kLoopback, kMcu}));
    QCOMPARE(AppSettings::instance().lastPortName(), kMcu);

    MainWindow second;
    QVERIFY(showAndActivate(second));
    QCOMPARE(second.sessionCount(), 2);
    QCOMPARE(second.sessionAt(0)->portName(), kLoopback);
    QCOMPARE(second.sessionAt(1)->portName(), kMcu);
    QCOMPARE(second.currentSession(), second.sessionAt(1));   // the previously active tab
    QVERIFY(!second.sessionAt(0)->isConnected());
    QVERIFY(!second.sessionAt(1)->isConnected());
    QVERIFY(second.windowTitle().endsWith(kMcu));
}

void Tst_mainwindow::restoreLastPortsDisabled()
{
    AppSettings& settings = AppSettings::instance();
    settings.setLastOpenPorts({kLoopback, kMcu});
    settings.setLastPortName(kMcu);
    settings.setRestoreLastPorts(false);

    MainWindow w;
    QVERIFY(showAndActivate(w));
    QCOMPARE(w.sessionCount(), 1);
    QCOMPARE(w.currentSession()->portName(), kMcu);   // one empty session pre-selecting lastPortName()
}

void Tst_mainwindow::restoreLastPortsDeduplicates()
{
    AppSettings& settings = AppSettings::instance();
    settings.setLastOpenPorts({kLoopback, QString(), kLoopback, kMcu});
    settings.setLastPortName(kLoopback);

    MainWindow w;
    QVERIFY(showAndActivate(w));
    QCOMPARE(w.sessionCount(), 2);
    QCOMPARE(w.sessionAt(0)->portName(), kLoopback);
    QCOMPARE(w.sessionAt(1)->portName(), kMcu);
    QCOMPARE(w.currentSession(), w.sessionAt(0));
}

// =======================================================================================
// SSH
// =======================================================================================

void Tst_mainwindow::sshActionsInMenusAndToolbar()
{
    MainWindow w;
    QAction* newSsh = action(w, "actionNewSshSession");
    QAction* profiles = action(w, "actionSshProfiles");
    QAction* upload = action(w, "actionUploadFile");
    QAction* download = action(w, "actionDownloadFile");
    QVERIFY(newSsh && profiles && upload && download);

    // Texts as documented (the mnemonic ampersand aside).
    QCOMPARE(QString(newSsh->text()).remove(QLatin1Char('&')), QStringLiteral("New SSH Session..."));
    QCOMPARE(QString(profiles->text()).remove(QLatin1Char('&')), QStringLiteral("SSH Profiles..."));
    QCOMPARE(QString(upload->text()).remove(QLatin1Char('&')), QStringLiteral("Upload File to Remote..."));
    QCOMPARE(QString(download->text()).remove(QLatin1Char('&')), QStringLiteral("Download File from Remote..."));
    QCOMPARE(newSsh->shortcut(), QKeySequence(QStringLiteral("Ctrl+Shift+T"), QKeySequence::PortableText));
    // The terminal-with-lock glyph (or the style fallback when Qt has no SVG engine) is rendered.
    QVERIFY(!newSsh->icon().isNull());
    QVERIFY(!newSsh->icon().pixmap(16).isNull());

    // File: right after New Session.
    const QList<QAction*> file = child<QMenu>(&w, "menuFile")->actions();
    const qsizetype newSession = file.indexOf(action(w, "actionNewSession"));
    QVERIFY(newSession >= 0);
    QCOMPARE(file.at(newSession + 1), newSsh);
    // Edit: after Quick Commands.
    const QList<QAction*> edit = child<QMenu>(&w, "menuEdit")->actions();
    const qsizetype quick = edit.indexOf(action(w, "actionQuickCommands"));
    QVERIFY(quick >= 0);
    QCOMPARE(edit.at(quick + 1), profiles);
    // Session: Upload, Download after Send File.
    const QList<QAction*> session = child<QMenu>(&w, "menuSession")->actions();
    const qsizetype sendFile = session.indexOf(action(w, "actionSendFile"));
    QVERIFY(sendFile >= 0);
    QCOMPARE(session.at(sendFile + 1), upload);
    QCOMPARE(session.at(sendFile + 2), download);
    // Toolbar: after New Session and after Send File.
    auto* toolbar = child<QToolBar>(&w, "mainToolBar");
    QVERIFY(toolbar);
    const QList<QAction*> tools = toolbar->actions();
    const qsizetype toolNew = tools.indexOf(action(w, "actionNewSession"));
    const qsizetype toolSend = tools.indexOf(action(w, "actionSendFile"));
    QVERIFY(toolNew >= 0 && toolSend >= 0);
    QCOMPARE(tools.at(toolNew + 1), newSsh);
    QCOMPARE(tools.at(toolSend + 1), upload);
    QCOMPARE(tools.at(toolSend + 2), download);
}

void Tst_mainwindow::newSshSessionAddsTab()
{
    MainWindow w;
    QVERIFY(showAndActivate(w));
    QCOMPARE(w.sessionCount(), 1);
    SessionWidget* serial = w.currentSession();

    action(w, "actionNewSshSession")->trigger();
    QCOMPARE(w.sessionCount(), 2);
    SessionWidget* ssh = w.currentSession();
    QVERIFY(ssh != serial);
    QVERIFY(ssh->isSsh());
    QCOMPARE(ssh->kind(), Transport::Kind::Ssh);
    QVERIFY(ssh->sshConnection() != nullptr);
    QVERIFY(ssh->connection() == nullptr);
    QCOMPARE(ssh->title(), QStringLiteral("New SSH Session"));
    QCOMPARE(tabs(w)->tabText(1), QStringLiteral("New SSH Session"));
    QVERIFY(!ssh->isConnected());
    QVERIFY(ssh->portName().isEmpty());
    // The target field has the focus so the user can type straight away.
    QVERIFY(ssh->sshConnectionBar() != nullptr);
    QTRY_VERIFY(QApplication::focusWidget() != nullptr);
    QVERIFY2(ssh->sshConnectionBar()->isAncestorOf(QApplication::focusWidget()),
             QApplication::focusWidget()->metaObject()->className());
    // The bar shares the window's profile store.
    QVERIFY(ssh->sshConnectionBar()->store() != nullptr);

    // With a target: title, restore key and window title follow the transport's display name.
    SessionWidget* targeted = w.newSshSession(QStringLiteral("root@10.0.0.24:2222"));
    QCOMPARE(w.sessionCount(), 3);
    QCOMPARE(w.currentSession(), targeted);
    QCOMPARE(targeted->title(), QStringLiteral("root@10.0.0.24:2222"));
    QCOMPARE(tabs(w)->tabText(2), QStringLiteral("root@10.0.0.24:2222"));
    QCOMPARE(targeted->portName(), QStringLiteral("ssh:target:root@10.0.0.24:2222"));
    QVERIFY(!targeted->isConnected());
    QVERIFY(w.windowTitle().endsWith(QStringLiteral(" - root@10.0.0.24:2222")));
    QVERIFY(tabs(w)->tabToolTip(2).contains(QStringLiteral("root@10.0.0.24:2222")));
    QVERIFY(!tabs(w)->tabIcon(2).isNull());
    // A restore key works the same way through newSession().
    SessionWidget* restored = w.newSession(QStringLiteral("ssh:target:pi@10.0.0.7"));
    QVERIFY(restored->isSsh());
    QCOMPARE(restored->title(), QStringLiteral("pi@10.0.0.7"));
    QCOMPARE(restored->sshConnectionBar()->targetText(), QStringLiteral("pi@10.0.0.7"));

    // Closing SSH tabs works like serial ones (never asks while disconnected).
    ModalDismisser dismisser(ModalDismisser::Answer::Reject);
    w.closeCurrentSession();
    dismisser.stop();
    QCOMPARE(dismisser.count(), 0);
    QCOMPARE(w.sessionCount(), 3);
}

void Tst_mainwindow::newSshSessionShortcut()
{
    MainWindow w;
    QVERIFY(showAndActivate(w));
    QCOMPARE(w.sessionCount(), 1);
    // Connected and focused: the terminal must still pass Ctrl+Shift+T through to the action
    // (every Ctrl+Shift+<letter> belongs to the application's shortcut map).
    QVERIFY(connectCurrent(w, kLoopback));
    SessionWidget* session = w.currentSession();
    session->focusTerminal();
    QTRY_COMPARE(QApplication::focusWidget(), static_cast<QWidget*>(session->terminal()));

    QTest::keyClick(session->terminal(), Qt::Key_T, Qt::ControlModifier | Qt::ShiftModifier);
    QTRY_COMPARE(w.sessionCount(), 2);
    QVERIFY(w.currentSession()->isSsh());
    QVERIFY(session->isConnected());   // the key never reached the device as a control byte

    tabs(w)->setCurrentIndex(0);
    action(w, "actionDisconnect")->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(!session->isConnected(), kSimTimeoutMs);
}

void Tst_mainwindow::newSshSessionPrefillsLastTarget()
{
    MainWindow w;
    QVERIFY(showAndActivate(w));

    // The last connected ad-hoc target is offered again - never connected.
    AppSettings::instance().setLastSshTarget(QStringLiteral("ssh:target:pi@10.0.0.7:2222"));
    SessionWidget* ssh = w.newSshSession();
    QVERIFY(ssh->isSsh());
    QCOMPARE(ssh->title(), QStringLiteral("pi@10.0.0.7:2222"));
    QCOMPARE(ssh->portName(), QStringLiteral("ssh:target:pi@10.0.0.7:2222"));
    QVERIFY(!ssh->isConnected());
    QCOMPARE(ssh->transport()->state(), Transport::State::Disconnected);

    // A profile key is not pre-filled (the combo lists the profiles anyway) ...
    AppSettings::instance().setLastSshTarget(QStringLiteral("ssh:profile:00000000-0000-0000-0000-000000000000"));
    QCOMPARE(w.newSshSession()->title(), QStringLiteral("New SSH Session"));
    // ... and an explicit target always wins.
    AppSettings::instance().setLastSshTarget(QStringLiteral("ssh:target:pi@10.0.0.7:2222"));
    QCOMPARE(w.newSshSession(QStringLiteral("root@10.0.0.24"))->title(), QStringLiteral("root@10.0.0.24"));
    AppSettings::instance().setLastSshTarget(QString());
    QCOMPARE(w.newSshSession()->title(), QStringLiteral("New SSH Session"));
}

void Tst_mainwindow::sshActionEnableRules()
{
    MainWindow w;
    QVERIFY(showAndActivate(w));
    SessionWidget* serial = w.currentSession();
    QAction* connectAction = action(w, "actionConnect");
    QAction* upload = action(w, "actionUploadFile");
    QAction* download = action(w, "actionDownloadFile");
    QAction* sendBreak = action(w, "actionSendBreak");
    QAction* refresh = action(w, "actionRefreshPorts");
    QAction* sync = action(w, "actionSyncTerminalSize");
    QAction* sendFile = action(w, "actionSendFile");

    // Serial tab, disconnected.
    QVERIFY(!connectAction->isEnabled());   // no port
    QVERIFY(!upload->isEnabled());
    QVERIFY(!download->isEnabled());
    QVERIFY(!sendBreak->isEnabled());
    QVERIFY(refresh->isEnabled());
    QVERIFY(!sync->isEnabled());

    // SSH tab, disconnected: Connect is offered (it asks for a target), the serial-only
    // actions are off, the SFTP actions wait for a connection.
    SessionWidget* ssh = w.newSshSession();
    QCOMPARE(w.currentSession(), ssh);
    QVERIFY(connectAction->isEnabled());
    QVERIFY(!upload->isEnabled());
    QVERIFY(!download->isEnabled());
    QVERIFY(!sendBreak->isEnabled());
    QVERIFY(!refresh->isEnabled());
    QVERIFY(!sync->isEnabled());
    QVERIFY(!sendFile->isEnabled());
    QVERIFY(!action(w, "actionPaste")->isEnabled());
    QVERIFY(action(w, "actionClear")->isEnabled());
    QVERIFY(action(w, "actionStartLogging")->isEnabled());
    QVERIFY(action(w, "actionReplayLog")->isEnabled());
    // Connect without a target: refused in the status bar, focus to the target field.
    w.statusBar()->clearMessage();
    connectAction->trigger();
    QVERIFY(!ssh->isConnected());
    QCOMPARE(w.statusBar()->currentMessage(), QStringLiteral("Enter a target such as user@host"));
    // Upload / Download on a disconnected SSH tab (a disabled QAction never fires, so through the
    // session's slots the menu forwards to): refused, no dialog.
    ssh->uploadFile();
    QCOMPARE(w.statusBar()->currentMessage(), QStringLiteral("Not connected"));
    ssh->downloadFile();
    QVERIFY(w.findChildren<QDialog*>().isEmpty());

    // Back on the serial tab: the rules flip; connected, BREAK / sync are on, SFTP stays off.
    tabs(w)->setCurrentIndex(0);
    QCOMPARE(w.currentSession(), serial);
    QVERIFY(refresh->isEnabled());
    QVERIFY(!sendBreak->isEnabled());
    QVERIFY(connectCurrent(w, kLoopback));
    QTRY_VERIFY(sendBreak->isEnabled());
    QVERIFY(sync->isEnabled());
    QVERIFY(sendFile->isEnabled());
    QVERIFY(!upload->isEnabled());
    QVERIFY(!download->isEnabled());
    w.statusBar()->clearMessage();
    serial->uploadFile();   // the slot behind the (disabled) action: refused with a message
    QVERIFY2(w.statusBar()->currentMessage().contains(QStringLiteral("SSH")),
             qPrintable(w.statusBar()->currentMessage()));
    QVERIFY(w.findChildren<QDialog*>().isEmpty());
    action(w, "actionDisconnect")->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(!serial->isConnected(), kSimTimeoutMs);
}

void Tst_mainwindow::sshRestoreRoundTrip()
{
    const QString key = QStringLiteral("ssh:target:root@10.0.0.24:2222");
    {
        MainWindow first;
        QVERIFY(showAndActivate(first));
        first.currentSession()->setPortName(kLoopback);
        SessionWidget* ssh = first.newSshSession(QStringLiteral("root@10.0.0.24:2222"));
        QCOMPARE(ssh->portName(), key);
        QCOMPARE(first.sessionCount(), 2);
        first.close();
        QTRY_VERIFY(!first.isVisible());
    }
    QCOMPARE(AppSettings::instance().lastOpenPorts(), (QStringList{kLoopback, key}));
    QCOMPARE(AppSettings::instance().lastPortName(), key);

    MainWindow second;
    QVERIFY(showAndActivate(second));
    QCOMPARE(second.sessionCount(), 2);
    QCOMPARE(second.sessionAt(0)->portName(), kLoopback);
    QVERIFY(!second.sessionAt(0)->isSsh());
    SessionWidget* ssh = second.sessionAt(1);
    QVERIFY(ssh->isSsh());
    QCOMPARE(ssh->portName(), key);
    QCOMPARE(ssh->title(), QStringLiteral("root@10.0.0.24:2222"));
    QCOMPARE(ssh->sshConnectionBar()->targetText(), QStringLiteral("root@10.0.0.24:2222"));
    QCOMPARE(tabs(second)->tabText(1), QStringLiteral("root@10.0.0.24:2222"));
    QCOMPARE(second.currentSession(), ssh);   // the previously active tab
    QVERIFY(!ssh->isConnected());             // never auto-connecting
    QCOMPARE(ssh->transport()->state(), Transport::State::Disconnected);
    QVERIFY(second.windowTitle().endsWith(QStringLiteral(" - root@10.0.0.24:2222")));

    // With restore disabled the last key still seeds the single start-up tab (an SSH one).
    AppSettings::instance().setRestoreLastPorts(false);
    MainWindow third;
    QVERIFY(showAndActivate(third));
    QCOMPARE(third.sessionCount(), 1);
    QVERIFY(third.currentSession()->isSsh());
    QCOMPARE(third.currentSession()->portName(), key);
    QVERIFY(!third.currentSession()->isConnected());
}

void Tst_mainwindow::sshProfilesDialogOpens()
{
    MainWindow w;
    QVERIFY(showAndActivate(w));

    {
        ModalDismisser dismisser(ModalDismisser::Answer::Reject);
        action(w, "actionSshProfiles")->trigger();
        dismisser.stop();
        QCOMPARE(dismisser.count(), 1);
        QCOMPARE(dismisser.classNames().first(), QStringLiteral("SshProfilesDialog"));
    }
    QCOMPARE(w.sessionCount(), 1);   // a rejected dialog opens no tab
    QVERIFY(QApplication::activeModalWidget() == nullptr);

    // The SSH bar's gear asks the window for the same dialog, pre-selecting the tab's profile.
    SessionWidget* ssh = w.newSshSession(QStringLiteral("root@10.0.0.24"));
    {
        ModalDismisser dismisser(ModalDismisser::Answer::Reject);
        emit ssh->sshProfilesEditRequested();
        dismisser.stop();
        QCOMPARE(dismisser.count(), 1);
        QCOMPARE(dismisser.classNames().first(), QStringLiteral("SshProfilesDialog"));
    }
    QCOMPARE(w.sessionCount(), 2);
    QVERIFY(w.isVisible());
}

void Tst_mainwindow::sshProfileStoreSavedAndReloaded()
{
    const QString path = SshProfileStore::defaultFilePath();
    QVERIFY2(!path.contains(QStringLiteral("/BuildAI/SerialUtility/")), qPrintable(path));   // test-mode data dir
    QFile::remove(path);

    QString id;
    {
        MainWindow w;
        QVERIFY(showAndActivate(w));
        SessionWidget* ssh = w.newSshSession();
        SshProfileStore* store = ssh->sshConnectionBar()->store();
        QVERIFY(store);
        QVERIFY(store->profiles().isEmpty());

        // A change (the profile dialog, a connect touching a profile, ...) is written shortly after.
        SshProfile profile;
        profile.name = QStringLiteral("Pico Ultra");
        profile.host = QStringLiteral("192.168.100.2");
        profile.user = QStringLiteral("root");
        id = store->upsert(profile).id;
        QVERIFY(!id.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(path), 2000);
        SshProfileStore check;
        QVERIFY(check.load(path));
        QVERIFY(check.profile(id).has_value());
        QCOMPARE(check.profile(id)->name, QStringLiteral("Pico Ultra"));

        // The tab restores by profile id and shows the profile's name.
        ssh->setSshTarget(QStringLiteral("ssh:profile:") + id);
        QCOMPARE(ssh->title(), QStringLiteral("Pico Ultra"));
        QCOMPARE(ssh->portName(), QStringLiteral("ssh:profile:") + id);
        w.close();
        QTRY_VERIFY(!w.isVisible());
    }

    // The next window loads the file and restores the profile tab from the saved key.
    MainWindow second;
    QVERIFY(showAndActivate(second));
    SessionWidget* restored = nullptr;
    for (int i = 0; i < second.sessionCount(); ++i) {
        if (second.sessionAt(i)->isSsh()) {
            restored = second.sessionAt(i);
        }
    }
    QVERIFY(restored);
    QCOMPARE(restored->title(), QStringLiteral("Pico Ultra"));
    QCOMPARE(restored->sshConnection()->profile().host, QStringLiteral("192.168.100.2"));
    QVERIFY(restored->sshConnectionBar()->store()->profile(id).has_value());
    QVERIFY(!restored->isConnected());
    QFile::remove(path);
}

void Tst_mainwindow::statusBarForSshTab()
{
    MainWindow w;
    QVERIFY(showAndActivate(w));
    QLabel* connection = statusLabel(w, "statusConnectionLabel");
    QLabel* counters = statusLabel(w, "statusCountersLabel");
    QVERIFY(connection && counters);

    SessionWidget* ssh = w.newSshSession();
    QCOMPARE(connection->text(), Transport::stateText(Transport::State::Disconnected));   // no target yet
    QCOMPARE(counters->text(), QStringLiteral("RX 0 B  TX 0 B"));

    ssh->setSshTarget(QStringLiteral("root@10.0.0.24:2222"));
    QVERIFY2(connection->text().startsWith(Transport::stateText(Transport::State::Disconnected)),
             qPrintable(connection->text()));
    QVERIFY2(connection->text().contains(QStringLiteral("root@10.0.0.24:2222")), qPrintable(connection->text()));
    QVERIFY2(connection->text().contains(ssh->transport()->summary()), qPrintable(connection->text()));
    QVERIFY2(connection->text().contains(QStringLiteral(" · ")), qPrintable(connection->text()));
    const QString tip = tabs(w)->tabToolTip(1);
    QVERIFY2(tip.contains(QStringLiteral("root@10.0.0.24:2222")), qPrintable(tip));
    QVERIFY2(tip.contains(ssh->transport()->summary()), qPrintable(tip));
    QVERIFY2(tip.contains(Transport::stateText(Transport::State::Disconnected)), qPrintable(tip));

    // Switching back to the serial tab shows that tab's own (serial) summary again.
    w.sessionAt(0)->setPortName(kLoopback);
    tabs(w)->setCurrentIndex(0);
    QVERIFY(connection->text().contains(kLoopback));
    QVERIFY(connection->text().contains(QStringLiteral("115200 8N1")));
    QVERIFY(!connection->text().contains(QStringLiteral("root@")));
}

// =======================================================================================
// Replay / folders / help
// =======================================================================================

void Tst_mainwindow::replayEnablesStopReplay()
{
    MainWindow w;
    QVERIFY(showAndActivate(w));
    SessionWidget* session = w.currentSession();
    QAction* stop = action(w, "actionStopReplay");
    QVERIFY(action(w, "actionReplayLog")->isEnabled());
    QVERIFY(!stop->isEnabled());

    QByteArray payload;
    for (int i = 0; i < 200; ++i) {
        payload += QStringLiteral("replay line %1\r\n").arg(i).toUtf8();
    }
    const QString file = tempPath(QStringLiteral("slow-replay.log"));
    QVERIFY(writeFile(file, payload));

    QSignalSpy replaySpy(session, &SessionWidget::replayStateChanged);
    session->replayLogFile(file, 300);   // ~11 s at 300 B/s: still running when we stop it
    QVERIFY(session->isReplaying());
    QCOMPARE(replaySpy.count(), 1);
    QVERIFY(stop->isEnabled());
    QVERIFY2(tabs(w)->tabText(0).startsWith(QStringLiteral("Replay:")), qPrintable(tabs(w)->tabText(0)));
    QVERIFY(tabs(w)->tabText(0).contains(QStringLiteral("slow-replay.log")));

    stop->trigger();
    QTRY_VERIFY(!session->isReplaying());
    QTRY_COMPARE(replaySpy.count(), 2);
    QVERIFY(!stop->isEnabled());
    QCOMPARE(tabs(w)->tabText(0), kNewSessionTitle);
}

void Tst_mainwindow::replayCompletesAndDisablesStop()
{
    MainWindow w;
    QVERIFY(showAndActivate(w));
    SessionWidget* session = w.currentSession();
    QAction* stop = action(w, "actionStopReplay");

    const QString file = tempPath(QStringLiteral("fast-replay.log"));
    QVERIFY(writeFile(file, QByteArrayLiteral("fast replay\r\n")));

    QSignalSpy replaySpy(session, &SessionWidget::replayStateChanged);
    session->replayLogFile(file, 0);   // unlimited speed
    QVERIFY(stop->isEnabled());
    QTRY_VERIFY_WITH_TIMEOUT(!session->isReplaying(), kSimTimeoutMs);
    QTRY_COMPARE(replaySpy.count(), 2);
    QCOMPARE(replaySpy.at(0).at(0).toBool(), true);
    QCOMPARE(replaySpy.at(1).at(0).toBool(), false);
    QVERIFY(!stop->isEnabled());
}

void Tst_mainwindow::replayRefusedWhileConnected()
{
    MainWindow w;
    QVERIFY(showAndActivate(w));
    QVERIFY(connectCurrent(w, kLoopback));
    w.statusBar()->clearMessage();

    ModalDismisser dismisser(ModalDismisser::Answer::Reject);
    action(w, "actionReplayLog")->trigger();
    dismisser.stop();
    QCOMPARE(dismisser.count(), 0);   // refused before any file dialog
    QVERIFY(!w.currentSession()->isReplaying());
    QVERIFY(!action(w, "actionStopReplay")->isEnabled());
    QVERIFY2(w.statusBar()->currentMessage().contains(kLoopback), qPrintable(w.statusBar()->currentMessage()));
}

void Tst_mainwindow::openLogFolderCreatesDirectory()
{
    const QString dir = tempPath(QStringLiteral("created/by/open-folder"));
    AppSettings::instance().setLogDirectory(dir);
    QVERIFY(!QDir(dir).exists());

    MainWindow w;
    QVERIFY(showAndActivate(w));
    action(w, "actionOpenLogFolder")->trigger();

    QVERIFY(QDir(dir).exists());
    QCOMPARE(m_urls.urls.size(), 1);
    QVERIFY(m_urls.urls.first().isLocalFile());
    QCOMPARE(QFileInfo(m_urls.urls.first().toLocalFile()).canonicalFilePath(), QFileInfo(dir).canonicalFilePath());
}

void Tst_mainwindow::homepageOpensUrl()
{
    MainWindow w;
    QVERIFY(showAndActivate(w));
    action(w, "actionHomepage")->trigger();
    QCOMPARE(m_urls.urls.size(), 1);
    QCOMPARE(m_urls.urls.first(), QUrl(QStringLiteral(APP_HOMEPAGE)));
}

void Tst_mainwindow::aboutAndVersionDialogsReused()
{
    MainWindow w;
    QVERIFY(showAndActivate(w));

    action(w, "actionAbout")->trigger();
    QCOMPARE(w.findChildren<AboutDialog*>().size(), 1);
    AboutDialog* about = w.findChildren<AboutDialog*>().first();
    QTRY_VERIFY(about->isVisible());
    QVERIFY(!about->isModal());
    action(w, "actionAbout")->trigger();
    QCOMPARE(w.findChildren<AboutDialog*>().size(), 1);   // re-raised, not duplicated

    action(w, "actionVersion")->trigger();
    QCOMPARE(w.findChildren<VersionDialog*>().size(), 1);
    VersionDialog* version = w.findChildren<VersionDialog*>().first();
    QTRY_VERIFY(version->isVisible());
    QVERIFY(!version->isModal());
    action(w, "actionVersion")->trigger();
    QCOMPARE(w.findChildren<VersionDialog*>().size(), 1);

    // Both close (and delete themselves) independently of the main window.
    about->close();
    version->close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCOMPARE(w.findChildren<AboutDialog*>().size(), 0);
    QCOMPARE(w.findChildren<VersionDialog*>().size(), 0);
    QVERIFY(w.isVisible());
}

// =======================================================================================
// Keyboard (v0.4): configurable shortcuts, tab selection, Ctrl+W to the shell
// =======================================================================================

void Tst_mainwindow::closeSessionShortcutLeavesCtrlWToShell()
{
    // Ctrl+W typed into a connected terminal is the control byte 0x17 for the device (the
    // loopback echoes it back) and the tab stays open; Ctrl+Shift+W is Close Session and asks
    // the usual question for a connected tab.
    MainWindow w;
    QVERIFY(showAndActivate(w));
    w.newSession();
    QVERIFY(connectCurrent(w, kLoopback));
    QCOMPARE(w.sessionCount(), 2);
    SessionWidget* session = w.currentSession();
    session->focusTerminal();
    QTRY_COMPARE(QApplication::focusWidget(), static_cast<QWidget*>(session->terminal()));
    QVERIFY(session->terminal()->reservedShortcuts().contains(seq("Ctrl+Shift+W")));
    QVERIFY(!session->terminal()->reservedShortcuts().contains(seq("Ctrl+W")));

    QSignalSpy sent(session->transport(), &Transport::dataSent);
    QSignalSpy received(session->transport(), &Transport::dataReceived);
    QTest::keyClick(session->terminal(), Qt::Key_W, Qt::ControlModifier);
    QTRY_COMPARE_WITH_TIMEOUT(spyBytes(received), QByteArray(1, '\x17'), kSimTimeoutMs);
    QCOMPARE(spyBytes(sent), QByteArray(1, '\x17'));
    QCOMPARE(w.sessionCount(), 2);
    QVERIFY(session->isConnected());
    QCOMPARE(w.currentSession(), session);

    ModalDismisser dismisser(ModalDismisser::Answer::Yes);
    QTest::keyClick(session->terminal(), Qt::Key_W, Qt::ControlModifier | Qt::ShiftModifier);
    QTRY_COMPARE(w.sessionCount(), 1);
    dismisser.stop();
    QCOMPARE(dismisser.count(), 1);
    QCOMPARE(dismisser.classNames().first(), QStringLiteral("QMessageBox"));
}

void Tst_mainwindow::selectTabShortcuts()
{
    MainWindow w;
    QVERIFY(showAndActivate(w));
    QAction* selectTab[9];
    for (int i = 0; i < 9; ++i) {
        selectTab[i] = action(w, qPrintable(QStringLiteral("actionSelectTab%1").arg(i + 1)));
        QVERIFY(selectTab[i]);
        QCOMPARE(selectTab[i]->shortcut(), seq(qPrintable(QStringLiteral("Alt+%1").arg(i + 1))));
        QCOMPARE(selectTab[i]->isEnabled(), i == 0);   // one tab: only Tab 1
    }
    QCOMPARE(selectTab[0]->text(), QStringLiteral("Tab &1"));

    w.newSession(kLoopback);
    w.newSession(kMcu);
    QTabWidget* tabWidget = tabs(w);
    QCOMPARE(tabWidget->currentIndex(), 2);
    for (int i = 0; i < 9; ++i) {
        QCOMPARE(selectTab[i]->isEnabled(), i < 3);
    }
    selectTab[1]->trigger();
    QCOMPARE(tabWidget->currentIndex(), 1);
    QTest::keyClick(&w, Qt::Key_3, Qt::AltModifier);
    QTRY_COMPARE(tabWidget->currentIndex(), 2);
    QTest::keyClick(&w, Qt::Key_1, Qt::AltModifier);
    QTRY_COMPARE(tabWidget->currentIndex(), 0);
    QTest::keyClick(&w, Qt::Key_4, Qt::AltModifier);   // no such tab: disabled, nothing happens
    QCoreApplication::processEvents();
    QCOMPARE(tabWidget->currentIndex(), 0);

    // Next / Previous Tab carry the fixed alternates Ctrl+PgDown / Ctrl+PgUp.
    QCOMPARE(action(w, "actionNextTab")->shortcuts(), (QList<QKeySequence>{seq("Ctrl+Tab"), seq("Ctrl+PgDown")}));
    QCOMPARE(action(w, "actionPreviousTab")->shortcuts(),
             (QList<QKeySequence>{seq("Ctrl+Shift+Tab"), seq("Ctrl+PgUp")}));
    QTest::keyClick(&w, Qt::Key_PageDown, Qt::ControlModifier);
    QTRY_COMPARE(tabWidget->currentIndex(), 1);
    QTest::keyClick(&w, Qt::Key_PageUp, Qt::ControlModifier);
    QTRY_COMPARE(tabWidget->currentIndex(), 0);
    QTest::keyClick(&w, Qt::Key_PageUp, Qt::ControlModifier);
    QTRY_COMPARE(tabWidget->currentIndex(), 2);

    // From a connected, focused terminal the same keys pass through and nothing is sent.
    tabWidget->setCurrentIndex(1);
    QVERIFY(connectCurrent(w, kLoopback));
    SessionWidget* session = w.currentSession();
    session->focusTerminal();
    QTRY_COMPARE(QApplication::focusWidget(), static_cast<QWidget*>(session->terminal()));
    QSignalSpy sent(session->transport(), &Transport::dataSent);
    QTest::keyClick(session->terminal(), Qt::Key_3, Qt::AltModifier);
    QTRY_COMPARE(tabWidget->currentIndex(), 2);
    tabWidget->setCurrentIndex(1);
    QTRY_COMPARE(QApplication::focusWidget(), static_cast<QWidget*>(session->terminal()));
    QTest::keyClick(session->terminal(), Qt::Key_PageDown, Qt::ControlModifier);
    QTRY_COMPARE(tabWidget->currentIndex(), 2);
    QCOMPARE(sent.count(), qsizetype(0));
    tabWidget->setCurrentIndex(1);
    action(w, "actionDisconnect")->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(!session->isConnected(), kSimTimeoutMs);
}

void Tst_mainwindow::windowMenuListsSessions()
{
    MainWindow w;
    QVERIFY(showAndActivate(w));
    auto* menu = child<QMenu>(&w, "menuWindow");
    QVERIFY(menu);

    // Fixed part: Next, Previous, separator, Tab 1..9, separator.
    const QList<QAction*> fixed = menu->actions();
    QVERIFY(fixed.size() >= 13);
    QCOMPARE(fixed.at(0), action(w, "actionNextTab"));
    QCOMPARE(fixed.at(1), action(w, "actionPreviousTab"));
    QVERIFY(fixed.at(2)->isSeparator());
    for (int i = 0; i < 9; ++i) {
        QCOMPARE(fixed.at(3 + i), action(w, qPrintable(QStringLiteral("actionSelectTab%1").arg(i + 1))));
    }
    QVERIFY(fixed.at(12)->isSeparator());

    // One entry per session, the current one checked.
    QList<QAction*> entries = sessionEntries(menu);
    QCOMPARE(entries.size(), qsizetype(1));
    QCOMPARE(entries.at(0)->text(), kNewSessionTitle);
    QVERIFY(entries.at(0)->isCheckable());
    QVERIFY(entries.at(0)->isChecked());
    QVERIFY(entries.at(0)->objectName().isEmpty());   // not configurable

    w.newSession(kLoopback);
    w.newSession(kMcu);
    QTabWidget* tabWidget = tabs(w);
    entries = sessionEntries(menu);
    QCOMPARE(entries.size(), qsizetype(3));
    QCOMPARE(entries.at(0)->text(), kNewSessionTitle);
    QCOMPARE(entries.at(1)->text(), kLoopback);
    QCOMPARE(entries.at(2)->text(), kMcu);
    QVERIFY(!entries.at(0)->isChecked());
    QVERIFY(!entries.at(1)->isChecked());
    QVERIFY(entries.at(2)->isChecked());

    // Triggering selects the tab; the check mark follows the current tab.
    entries.at(0)->trigger();
    QCOMPARE(tabWidget->currentIndex(), 0);
    entries = sessionEntries(menu);
    QVERIFY(entries.at(0)->isChecked());
    QVERIFY(!entries.at(2)->isChecked());
    tabWidget->setCurrentIndex(1);
    entries = sessionEntries(menu);
    QVERIFY(entries.at(1)->isChecked());
    QVERIFY(!entries.at(0)->isChecked());

    // Rename (a port selected on the first tab) and reorder (drag) rebuild the list.
    const QString linuxPort = QStringLiteral("SIM:linux");
    w.sessionAt(0)->setPortName(linuxPort);
    entries = sessionEntries(menu);
    QCOMPARE(entries.at(0)->text(), linuxPort);
    tabWidget->tabBar()->moveTab(0, 2);
    entries = sessionEntries(menu);
    QCOMPARE(entries.size(), qsizetype(3));
    QCOMPARE(entries.at(0)->text(), kLoopback);
    QCOMPARE(entries.at(1)->text(), kMcu);
    QCOMPARE(entries.at(2)->text(), linuxPort);
    QCOMPARE(tabWidget->currentIndex(), 0);   // the current widget (loopback) moved to the front
    QVERIFY(entries.at(0)->isChecked());

    // Closing a tab removes its entry.
    w.closeSession(1);
    entries = sessionEntries(menu);
    QCOMPARE(entries.size(), qsizetype(2));
    QCOMPARE(entries.at(0)->text(), kLoopback);
    QCOMPARE(entries.at(1)->text(), linuxPort);
}

void Tst_mainwindow::middleClickClosesTab()
{
    MainWindow w;
    QVERIFY(showAndActivate(w));
    w.newSession(kMcu);
    QCOMPARE(w.sessionCount(), 2);
    QTabBar* bar = tabs(w)->tabBar();
    QVERIFY(bar);

    QTest::mouseClick(bar, Qt::MiddleButton, Qt::NoModifier, bar->tabRect(1).center());
    QTRY_COMPARE(w.sessionCount(), 1);
    QCOMPARE(tabs(w)->tabText(0), kNewSessionTitle);

    // A press on one tab released elsewhere closes nothing; a left click only selects.
    w.newSession(kMcu);
    QCOMPARE(w.sessionCount(), 2);
    QTest::mousePress(bar, Qt::MiddleButton, Qt::NoModifier, bar->tabRect(0).center());
    QTest::mouseRelease(bar, Qt::MiddleButton, Qt::NoModifier, QPoint(-20, -20));
    QCoreApplication::processEvents();
    QCOMPARE(w.sessionCount(), 2);
    QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, bar->tabRect(0).center());
    QTRY_COMPARE(tabs(w)->currentIndex(), 0);
    QCOMPARE(w.sessionCount(), 2);

    // A connected tab asks first (the close button's confirmation): No keeps it, Yes closes it.
    tabs(w)->setCurrentIndex(1);
    QVERIFY(connectCurrent(w, kLoopback));
    QVERIFY(AppSettings::instance().confirmCloseWhenConnected());
    {
        ModalDismisser no(ModalDismisser::Answer::No);
        QTest::mouseClick(bar, Qt::MiddleButton, Qt::NoModifier, bar->tabRect(1).center());
        no.stop();
        QCOMPARE(no.count(), 1);
        QCOMPARE(no.classNames().first(), QStringLiteral("QMessageBox"));
        QCOMPARE(w.sessionCount(), 2);
        QVERIFY(w.sessionAt(1)->isConnected());
    }
    {
        ModalDismisser yes(ModalDismisser::Answer::Yes);
        QTest::mouseClick(bar, Qt::MiddleButton, Qt::NoModifier, bar->tabRect(1).center());
        yes.stop();
        QCOMPARE(yes.count(), 1);
        QTRY_COMPARE(w.sessionCount(), 1);
    }
}

void Tst_mainwindow::detectBaudRateAction()
{
    MainWindow w;
    QVERIFY(showAndActivate(w));
    QAction* detect = action(w, "actionDetectBaudRate");
    QVERIFY(detect);
    QCOMPARE(detect->shortcut(), seq("Ctrl+Shift+B"));
    QVERIFY(!detect->text().isEmpty());
    // Session menu: right after Sync Terminal Size.
    const QList<QAction*> session = child<QMenu>(&w, "menuSession")->actions();
    const qsizetype sync = session.indexOf(action(w, "actionSyncTerminalSize"));
    QVERIFY(sync >= 0);
    QCOMPARE(session.at(sync + 1), detect);

    QVERIFY(!detect->isEnabled());   // disconnected serial tab
    QVERIFY(connectCurrent(w, kLoopback));
    QVERIFY(detect->isEnabled());
    SessionWidget* current = w.currentSession();
    detect->trigger();   // must not crash, whatever the detector does on a loopback
    QCoreApplication::processEvents();
    QVERIFY(current->isConnected());
    QCOMPARE(w.currentSession(), current);

    // The shortcut is reserved: from the focused terminal it reaches the action, not the device.
    current->focusTerminal();
    QTRY_COMPARE(QApplication::focusWidget(), static_cast<QWidget*>(current->terminal()));
    QVERIFY(current->terminal()->reservedShortcuts().contains(seq("Ctrl+Shift+B")));
    QSignalSpy triggered(detect, &QAction::triggered);
    QSignalSpy sent(current->transport(), &Transport::dataSent);
    QTest::keyClick(current->terminal(), Qt::Key_B, Qt::ControlModifier | Qt::ShiftModifier);
    QCoreApplication::processEvents();
    QCOMPARE(triggered.count(), qsizetype(1));
    QCOMPARE(sent.count(), qsizetype(0));
    QVERIFY(current->isConnected());

    action(w, "actionDisconnect")->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(!current->isConnected(), kSimTimeoutMs);
    QVERIFY(!detect->isEnabled());

    // Never on an SSH tab.
    w.newSshSession(QStringLiteral("root@10.0.0.24"));
    QVERIFY(w.currentSession()->isSsh());
    QVERIFY(!detect->isEnabled());
}

void Tst_mainwindow::shortcutEntriesContract()
{
    MainWindow w;
    const QList<ShortcutEntry> entries = w.shortcutEntries();
    QVERIFY(entries.size() >= static_cast<qsizetype>(std::size(kActions)) - 2);   // minus the language actions

    QSet<QString> names;
    QHash<QString, qsizetype> position;
    for (const ShortcutEntry& entry : entries) {
        QVERIFY2(!entry.objectName.isEmpty(), qPrintable(entry.title));
        QVERIFY2(!entry.title.isEmpty(), qPrintable(entry.objectName));
        QVERIFY2(!names.contains(entry.objectName), qPrintable(entry.objectName));
        QVERIFY2(!entry.objectName.startsWith(QLatin1String("actionLanguage")), qPrintable(entry.objectName));
        QVERIFY2(entry.title.contains(QLatin1String(" > ")), qPrintable(entry.title));
        QVERIFY2(!entry.title.contains(QLatin1Char('&')), qPrintable(entry.title));
        QVERIFY2(!entry.title.endsWith(QLatin1String("...")), qPrintable(entry.title));
        position.insert(entry.objectName, names.size());
        names.insert(entry.objectName);
    }
    const auto find = [&entries](const char* name) -> const ShortcutEntry* {
        for (const ShortcutEntry& entry : entries) {
            if (entry.objectName == QLatin1String(name)) {
                return &entry;
            }
        }
        return nullptr;
    };
    struct Expected
    {
        const char* name;
        const char* title;
        const char* sequence;
    };
    const Expected expected[] = {{"actionNewSession", "File > New Session", "Ctrl+T"},
                                 {"actionNewSshSession", "File > New SSH Session", "Ctrl+Shift+T"},
                                 {"actionCloseSession", "File > Close Session", "Ctrl+Shift+W"},
                                 {"actionConnect", "Session > Connect", "F2"},
                                 {"actionDetectBaudRate", "Session > Detect Baud Rate", "Ctrl+Shift+B"},
                                 {"actionResetTerminal", "Session > Reset Terminal", ""},
                                 {"actionPreferences", "Edit > Preferences", "Ctrl+,"},
                                 {"actionHexView", "View > Hex View", "Ctrl+Shift+H"},
                                 {"actionNextTab", "Window > Next Tab", "Ctrl+Tab"},
                                 {"actionSelectTab1", "Window > Tab 1", "Alt+1"},
                                 {"actionSelectTab9", "Window > Tab 9", "Alt+9"},
                                 {"actionAbout", "Help > About BuildAI Serial Utility", ""}};
    for (const Expected& e : expected) {
        const ShortcutEntry* entry = find(e.name);
        QVERIFY2(entry != nullptr, e.name);
        QCOMPARE(entry->title, QString::fromLatin1(e.title));
        QCOMPARE(entry->defaultSequence, seq(e.sequence));
    }
    // Every action of the contract table is listed with its .ui default, except the language ones.
    for (const ActionSpec& spec : kActions) {
        const ShortcutEntry* entry = find(spec.name);
        if (QLatin1String(spec.name).startsWith(QLatin1String("actionLanguage"))) {
            QVERIFY2(entry == nullptr, spec.name);
            continue;
        }
        QVERIFY2(entry != nullptr, spec.name);
        QCOMPARE(entry->defaultSequence, seq(spec.shortcut));
    }
    // Menu order.
    QVERIFY(position.value(QStringLiteral("actionNewSession")) < position.value(QStringLiteral("actionConnect")));
    QVERIFY(position.value(QStringLiteral("actionConnect")) < position.value(QStringLiteral("actionCopy")));
    QVERIFY(position.value(QStringLiteral("actionCopy")) < position.value(QStringLiteral("actionHexView")));
    QVERIFY(position.value(QStringLiteral("actionHexView")) < position.value(QStringLiteral("actionNextTab")));
    QVERIFY(position.value(QStringLiteral("actionNextTab")) < position.value(QStringLiteral("actionAbout")));
    // The per-session Window entries are not listed.
    w.newSession(kLoopback);
    QCOMPARE(w.shortcutEntries().size(), entries.size());

    // The fixed shortcuts the Preferences page checks conflicts against: the tab alternates,
    // owned by their actions, and the menu bar's mnemonics, owned by nobody configurable.
    const QList<ShortcutEntry> fixed = w.fixedShortcutEntries();
    const auto fixedFor = [&fixed](const char* sequence) -> const ShortcutEntry* {
        for (const ShortcutEntry& entry : fixed) {
            if (entry.defaultSequence == seq(sequence)) {
                return &entry;
            }
        }
        return nullptr;
    };
    struct ExpectedFixed
    {
        const char* sequence;
        const char* name;
        const char* title;
    };
    const ExpectedFixed expectedFixed[] = {{"Ctrl+PgDown", "actionNextTab", "Window > Next Tab"},
                                           {"Ctrl+PgUp", "actionPreviousTab", "Window > Previous Tab"},
                                           {"Alt+F", "", "File menu"},
                                           {"Alt+S", "", "Session menu"},
                                           {"Alt+E", "", "Edit menu"},
                                           {"Alt+V", "", "View menu"},
                                           {"Alt+W", "", "Window menu"},
                                           {"Alt+L", "", "Language menu"},
                                           {"Alt+H", "", "Help menu"}};
    QCOMPARE(fixed.size(), static_cast<qsizetype>(std::size(expectedFixed)));
    for (const ExpectedFixed& e : expectedFixed) {
        const ShortcutEntry* entry = fixedFor(e.sequence);
        QVERIFY2(entry != nullptr, e.sequence);
        QCOMPARE(entry->objectName, QString::fromLatin1(e.name));
        QCOMPARE(entry->title, QString::fromLatin1(e.title));
    }

    // The default stays the .ui value even when an override is stored.
    AppSettings::instance().setShortcut(QStringLiteral("actionNewSession"), seq("Ctrl+Shift+N"));
    MainWindow second;
    const QList<ShortcutEntry> again = second.shortcutEntries();
    for (const ShortcutEntry& entry : again) {
        if (entry.objectName == QLatin1String("actionNewSession")) {
            QCOMPARE(entry.defaultSequence, seq("Ctrl+T"));
        }
    }
    QCOMPARE(action(second, "actionNewSession")->shortcut(), seq("Ctrl+Shift+N"));
}

void Tst_mainwindow::storedShortcutAppliedAndLive()
{
    AppSettings& settings = AppSettings::instance();
    settings.setShortcut(QStringLiteral("actionCloseSession"), seq("Ctrl+Shift+X"));

    MainWindow w;
    QVERIFY(showAndActivate(w));
    QAction* close = action(w, "actionCloseSession");
    QCOMPARE(close->shortcut(), seq("Ctrl+Shift+X"));   // applied at construction

    // Live: a change while the window exists is applied at once, tooltip included.
    settings.setShortcut(QStringLiteral("actionNewSession"), seq("Ctrl+Shift+N"));
    QAction* newSession = action(w, "actionNewSession");
    QCOMPARE(newSession->shortcut(), seq("Ctrl+Shift+N"));
    QWidget* plus = tabs(w)->cornerWidget(Qt::TopRightCorner);
    QVERIFY(plus);
    QVERIFY2(plus->toolTip().contains(seq("Ctrl+Shift+N").toString(QKeySequence::NativeText)), qPrintable(plus->toolTip()));

    // It works from a connected terminal: Ctrl+Shift+N opens a tab, Ctrl+T is now the device's.
    QVERIFY(connectCurrent(w, kLoopback));
    SessionWidget* session = w.currentSession();
    session->focusTerminal();
    QTRY_COMPARE(QApplication::focusWidget(), static_cast<QWidget*>(session->terminal()));
    QSignalSpy sent(session->transport(), &Transport::dataSent);
    QTest::keyClick(session->terminal(), Qt::Key_T, Qt::ControlModifier);
    QTRY_COMPARE(spyBytes(sent), QByteArray(1, '\x14'));
    QCOMPARE(w.sessionCount(), 1);
    QTest::keyClick(session->terminal(), Qt::Key_N, Qt::ControlModifier | Qt::ShiftModifier);
    QTRY_COMPARE(w.sessionCount(), 2);
    QCOMPARE(spyBytes(sent), QByteArray(1, '\x14'));

    // Clearing the override restores the default.
    settings.clearShortcut(QStringLiteral("actionNewSession"));
    QCOMPARE(newSession->shortcut(), seq("Ctrl+T"));
    QVERIFY(plus->toolTip().contains(seq("Ctrl+T").toString(QKeySequence::NativeText)));

    // An empty stored sequence means "no shortcut"; the fixed alternate stays and is never doubled.
    QAction* next = action(w, "actionNextTab");
    settings.setShortcut(QStringLiteral("actionNextTab"), QKeySequence());
    QCOMPARE(next->shortcuts(), (QList<QKeySequence>{seq("Ctrl+PgDown")}));
    settings.setShortcut(QStringLiteral("actionNextTab"), seq("Ctrl+PgDown"));
    QCOMPARE(next->shortcuts(), (QList<QKeySequence>{seq("Ctrl+PgDown")}));
    settings.setShortcut(QStringLiteral("actionNextTab"), seq("F6"));
    QCOMPARE(next->shortcuts(), (QList<QKeySequence>{seq("F6"), seq("Ctrl+PgDown")}));
    settings.clearShortcut(QStringLiteral("actionNextTab"));
    QCOMPARE(next->shortcuts(), (QList<QKeySequence>{seq("Ctrl+Tab"), seq("Ctrl+PgDown")}));

    // A language switch (retranslateUi re-sets the .ui shortcuts) keeps the configured one.
    action(w, "actionLanguageChinese")->trigger();
    QCoreApplication::processEvents();
    QTRY_COMPARE(close->shortcut(), seq("Ctrl+Shift+X"));
    QCOMPARE(next->shortcuts(), (QList<QKeySequence>{seq("Ctrl+Tab"), seq("Ctrl+PgDown")}));
    action(w, "actionLanguageEnglish")->trigger();
    QCoreApplication::processEvents();
    QTRY_COMPARE(close->shortcut(), seq("Ctrl+Shift+X"));

    tabs(w)->setCurrentIndex(0);
    action(w, "actionDisconnect")->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(!session->isConnected(), kSimTimeoutMs);
}

void Tst_mainwindow::reservedShortcutsPushedToTerminals()
{
    MainWindow w;
    QVERIFY(showAndActivate(w));
    w.newSession(kMcu);
    QCOMPARE(w.sessionCount(), 2);

    const QList<QKeySequence> expected = {seq("Ctrl+T"),  seq("Ctrl+Shift+T"), seq("Ctrl+Shift+W"), seq("F2"),
                                          seq("F3"),      seq("F5"),           seq("Ctrl+Tab"),     seq("Ctrl+PgDown"),
                                          seq("Ctrl+Shift+Tab"), seq("Ctrl+PgUp"), seq("Alt+1"),   seq("Alt+9"),
                                          seq("Ctrl+Shift+B"), seq("Ctrl+,"),  seq("Ctrl+Shift+H"), seq("Ctrl++")};
    for (int i = 0; i < w.sessionCount(); ++i) {
        const QList<QKeySequence> reserved = w.sessionAt(i)->terminal()->reservedShortcuts();
        for (const QKeySequence& sequence : expected) {
            QVERIFY2(reserved.contains(sequence), qPrintable(sequence.toString(QKeySequence::PortableText)));
        }
        QVERIFY(!reserved.contains(seq("Ctrl+W")));
        QVERIFY(!reserved.contains(QKeySequence()));
    }

    // A change reaches every terminal, existing and new.
    AppSettings::instance().setShortcut(QStringLiteral("actionFind"), seq("Ctrl+K"));
    for (int i = 0; i < w.sessionCount(); ++i) {
        const QList<QKeySequence> reserved = w.sessionAt(i)->terminal()->reservedShortcuts();
        QVERIFY(reserved.contains(seq("Ctrl+K")));
        QVERIFY(!reserved.contains(seq("Ctrl+Shift+F")));
    }
    SessionWidget* later = w.newSession();
    QVERIFY(later->terminal()->reservedShortcuts().contains(seq("Ctrl+K")));
    QVERIFY(!later->terminal()->reservedShortcuts().contains(seq("Ctrl+Shift+F")));

    // End to end: on a connected terminal Ctrl+K now opens Find instead of sending 0x0B, and
    // Ctrl+Shift+F, no longer an application shortcut, comes back as the control byte 0x06.
    tabs(w)->setCurrentIndex(0);
    QVERIFY(connectCurrent(w, kLoopback));
    SessionWidget* session = w.currentSession();
    session->focusTerminal();
    QTRY_COMPARE(QApplication::focusWidget(), static_cast<QWidget*>(session->terminal()));
    QSignalSpy sent(session->transport(), &Transport::dataSent);
    {
        ModalDismisser dismisser(ModalDismisser::Answer::Reject);
        QTest::keyClick(session->terminal(), Qt::Key_K, Qt::ControlModifier);
        dismisser.stop();
        QCOMPARE(dismisser.count(), 1);
        QCOMPARE(dismisser.classNames().first(), QStringLiteral("QInputDialog"));
    }
    QCOMPARE(sent.count(), qsizetype(0));
    QTest::keyClick(session->terminal(), Qt::Key_F, Qt::ControlModifier | Qt::ShiftModifier);
    QTRY_COMPARE(spyBytes(sent), QByteArray(1, '\x06'));

    AppSettings::instance().clearShortcut(QStringLiteral("actionFind"));
    QVERIFY(!session->terminal()->reservedShortcuts().contains(seq("Ctrl+K")));
    QVERIFY(session->terminal()->reservedShortcuts().contains(seq("Ctrl+Shift+F")));
    QTest::keyClick(session->terminal(), Qt::Key_K, Qt::ControlModifier);
    QTRY_COMPARE(spyBytes(sent), QByteArray("\x06\x0b"));

    action(w, "actionDisconnect")->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(!session->isConnected(), kSimTimeoutMs);
}

void Tst_mainwindow::sshTabKeyboardEndToEnd()
{
    // The v0.4 keyboard rules on a real SSH tab against the in-process server (cross-package:
    // keyboard + ssh-core + test server): Ctrl+W typed into the connected terminal reaches the
    // server's shell as 0x17 and the tab stays open, Alt+1 / Alt+2 switch tabs from the focused
    // terminal without sending a byte, Detect Baud Rate is never enabled for SSH, and
    // Ctrl+Shift+W closes the tab with the usual question.
    TestSshServer::Options options;
    options.user = kSshUser;
    options.password = kSshPassword;
    options.rootDir = tempPath(QStringLiteral("ssh-root"));
    QVERIFY(QDir().mkpath(options.rootDir));
    TestSshServer server(options);
    QString error;
    QVERIFY2(server.start(&error), qPrintable(error));

    MainWindow w;
    QVERIFY(showAndActivate(w));
    // A stored profile whose known_hosts already holds the server's key and whose password is
    // in SecretStore: the connect needs no dialog (tst_sshsession drives the dialogs).
    auto* store = w.findChild<SshProfileStore*>();
    QVERIFY(store);
    SshProfile p;
    p.name = QStringLiteral("Keyboard box");
    p.host = QStringLiteral("127.0.0.1");
    p.port = server.port();
    p.user = kSshUser;
    p.auth = SshProfile::Auth::Password;   // never the developer's ~/.ssh/id_* keys
    p.passwordSaved = true;
    p.connectTimeoutSeconds = 5;
    p.knownHostsFile = tempPath(QStringLiteral("ssh-known_hosts"));
    QVERIFY(writeFile(p.knownHostsFile, (server.knownHostsLine() + QLatin1Char('\n')).toUtf8()));
    AppSettings::instance().setSshKnownHostsFile(p.knownHostsFile);   // never the user's ~/.ssh
    const SshProfile stored = store->upsert(p);
    QVERIFY(!stored.id.isEmpty());
    QVERIFY(SecretStore::store(QStringLiteral("ssh/%1/password").arg(stored.id), kSshPassword));

    SessionWidget* ssh = w.newSshSession(QStringLiteral("ssh:profile:") + stored.id);
    QVERIFY(ssh && ssh->isSsh());
    QCOMPARE(w.sessionCount(), 2);
    QTabWidget* tabWidget = tabs(w);
    QCOMPARE(tabWidget->currentIndex(), 1);
    QAction* detect = action(w, "actionDetectBaudRate");
    QVERIFY(!detect->isEnabled());
    ModalDismisser dismisser(ModalDismisser::Answer::Yes);   // only the close question at the end is expected
    action(w, "actionConnect")->trigger();
    QTRY_COMPARE_WITH_TIMEOUT(ssh->transport()->state(), Transport::State::Connected, kSshTimeoutMs);
    QTRY_COMPARE_WITH_TIMEOUT(lastNonBlankLine(ssh->terminal()), QStringLiteral("$"), kSshTimeoutMs);
    QCOMPARE(dismisser.count(), 0);
    QVERIFY(!detect->isEnabled());   // connected, but SSH
    QVERIFY(action(w, "actionDisconnect")->isEnabled());
    QVERIFY(!action(w, "actionSendBreak")->isEnabled());

    ssh->focusTerminal();
    QTRY_COMPARE(QApplication::focusWidget(), static_cast<QWidget*>(ssh->terminal()));
    QVERIFY(!ssh->terminal()->reservedShortcuts().contains(seq("Ctrl+W")));
    QVERIFY(ssh->terminal()->reservedShortcuts().contains(seq("Ctrl+Shift+W")));
    QVERIFY(ssh->terminal()->reservedShortcuts().contains(seq("Alt+1")));
    QVERIFY(ssh->terminal()->reservedShortcuts().contains(seq("Ctrl+Shift+B")));

    // Ctrl+W: the shell's "delete word" byte reaches the server; nothing closes.
    const QByteArray before = server.receivedShellInput();
    QTest::keyClick(ssh->terminal(), Qt::Key_W, Qt::ControlModifier);
    QTRY_COMPARE_WITH_TIMEOUT(server.receivedShellInput(), before + QByteArray(1, '\x17'), kSshTimeoutMs);
    QCOMPARE(w.sessionCount(), 2);
    QVERIFY(ssh->isConnected());
    QCOMPARE(w.currentSession(), ssh);
    QCOMPARE(dismisser.count(), 0);

    // Alt+1 from the SSH terminal selects the serial tab and sends nothing; Alt+2 from there
    // comes back to the SSH tab (Alt+<anything> always belongs to the application).
    QTest::keyClick(ssh->terminal(), Qt::Key_1, Qt::AltModifier);
    QTRY_COMPARE(tabWidget->currentIndex(), 0);
    QVERIFY(!w.currentSession()->isSsh());
    QVERIFY(!detect->isEnabled());   // a disconnected serial tab
    QTest::keyClick(&w, Qt::Key_2, Qt::AltModifier);
    QTRY_COMPARE(tabWidget->currentIndex(), 1);
    QCOMPARE(w.currentSession(), ssh);
    QVERIFY(!detect->isEnabled());
    QTest::qWait(200);   // a byte sent by mistake would have arrived by now
    QCOMPARE(server.receivedShellInput(), before + QByteArray(1, '\x17'));

    // Ctrl+Shift+B is reserved (the action is disabled here): nothing goes to the shell.
    QTRY_COMPARE(QApplication::focusWidget(), static_cast<QWidget*>(ssh->terminal()));
    QTest::keyClick(ssh->terminal(), Qt::Key_B, Qt::ControlModifier | Qt::ShiftModifier);
    QTest::qWait(200);
    QCOMPARE(server.receivedShellInput(), before + QByteArray(1, '\x17'));

    // Ctrl+Shift+W: Close Session asks (a connected tab) and closes it; the server sees the
    // client go and the serial tab is what remains.
    QTest::keyClick(ssh->terminal(), Qt::Key_W, Qt::ControlModifier | Qt::ShiftModifier);
    QTRY_COMPARE(w.sessionCount(), 1);
    QVERIFY(!w.currentSession()->isSsh());
    QTRY_COMPARE_WITH_TIMEOUT(server.activeConnections(), 0, kSshTimeoutMs);
    dismisser.stop();
    QCOMPARE(dismisser.count(), 1);
    QCOMPARE(dismisser.classNames().first(), QStringLiteral("QMessageBox"));
    QCOMPARE(server.receivedShellInput(), before + QByteArray(1, '\x17'));
    QCOMPARE(server.connectionCount(), 1);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);   // the closed tab, before the server stops
    server.stop();
}

void Tst_mainwindow::menuMnemonicsUnique()
{
    // Every '&' accelerator is unique within its menu, and so are the menu bar's own: an
    // ambiguous one only moves the highlight in an open Qt menu instead of activating the entry
    // (found by the real-window smoke of v0.4: Disconnect / Download File both on D, Send Break /
    // Detect Baud Rate both on B).
    MainWindow w;
    QVERIFY(showAndActivate(w));
    w.newSession(kLoopback);   // a dynamic Window-menu entry with a real title in the list too
    QStringList problems;
    const auto check = [&problems](const QString& menuName, const QList<QAction*>& actions) {
        QHash<QChar, QString> seen;
        for (const QAction* a : actions) {
            if (a->isSeparator()) {
                continue;
            }
            const QChar m = mnemonicOf(a->text());
            if (m.isNull()) {
                continue;
            }
            if (seen.contains(m)) {
                problems.append(QStringLiteral("%1: '%2' and '%3' both use %4").arg(menuName, seen.value(m), a->text(), m));
            } else {
                seen.insert(m, a->text());
            }
        }
    };
    const QList<QAction*> menus = w.menuBar()->actions();
    check(QStringLiteral("menu bar"), menus);
    int checked = 0;
    for (const QAction* top : menus) {
        if (QMenu* menu = top->menu()) {
            check(menu->title(), menu->actions());
            ++checked;
        }
    }
    QCOMPARE(checked, 7);   // File, Session, Edit, View, Window, Language, Help
    QVERIFY2(problems.isEmpty(), qPrintable(problems.join(QStringLiteral("; "))));
    // The two v0.4 fixes stay in place.
    QCOMPARE(mnemonicOf(action(w, "actionDownloadFile")->text()), QChar(QLatin1Char('w')));
    QCOMPARE(mnemonicOf(action(w, "actionDetectBaudRate")->text()), QChar(QLatin1Char('t')));
    QCOMPARE(mnemonicOf(action(w, "actionDisconnect")->text()), QChar(QLatin1Char('d')));
    QCOMPARE(mnemonicOf(action(w, "actionSendBreak")->text()), QChar(QLatin1Char('b')));
    // The per-session entries carry no accelerator: their '&' is doubled.
    const QList<QAction*> entries = sessionEntries(child<QMenu>(&w, "menuWindow"));
    QCOMPARE(entries.size(), 2);
    for (const QAction* entry : entries) {
        QVERIFY2(mnemonicOf(entry->text()).isNull(), qPrintable(entry->text()));
    }
}

// =======================================================================================
// SystemLogViewer
// =======================================================================================

void Tst_mainwindow::sysLogRoutesQtMessages()
{
    SystemLogViewer viewer;
    SystemLogViewer::setInstance(&viewer);
    QCOMPARE(SystemLogViewer::instance(), &viewer);
    SystemLogViewer::installMessageHandler();

    qCWarning(lcApp) << "hello-sysLog";
    QCoreApplication::processEvents();
    QTRY_VERIFY2(viewerText(viewer).contains(QStringLiteral("hello-sysLog")), qPrintable(viewerText(viewer)));
    const QString text = viewerText(viewer);
    QVERIFY2(text.contains(QStringLiteral("WARN")), qPrintable(text));
    QVERIFY2(text.contains(QStringLiteral("buildai.app")), qPrintable(text));

    qCInfo(lcSerial) << "info-sysLog" << 42;
    qCCritical(lcUi) << "critical-sysLog";
    QCoreApplication::processEvents();
    QTRY_VERIFY(viewerText(viewer).contains(QStringLiteral("info-sysLog 42")));
    QVERIFY(viewerText(viewer).contains(QStringLiteral("buildai.serial")));
    QVERIFY(viewerText(viewer).contains(QStringLiteral("critical-sysLog")));
    QVERIFY(viewerText(viewer).contains(QStringLiteral("ERROR")));

    // Messages emitted while no instance is registered do not reach the detached viewer...
    SystemLogViewer::setInstance(nullptr);
    QVERIFY(SystemLogViewer::instance() == nullptr);
    qCWarning(lcApp) << "orphan-sysLog";
    QCoreApplication::processEvents();
    QVERIFY(!viewerText(viewer).contains(QStringLiteral("orphan-sysLog")));

    // ...but are buffered and delivered to the next instance (the constructor registers it
    // because s_instance is null). This also drains the pending buffer for the later tests.
    SystemLogViewer late;
    QCoreApplication::processEvents();
    QTRY_VERIFY2(viewerText(late).contains(QStringLiteral("orphan-sysLog")), qPrintable(viewerText(late)));
    QVERIFY(!viewerText(viewer).contains(QStringLiteral("orphan-sysLog")));
}

void Tst_mainwindow::sysLogLevelFilterHidesDebug()
{
    SystemLogViewer viewer;
    viewer.appendLog(LogLevel::Debug, QStringLiteral("dbg-entry"));
    viewer.appendLog(LogLevel::Info, QStringLiteral("info-entry"));
    viewer.appendLog(LogLevel::Warning, QStringLiteral("warn-entry"));
    viewer.appendLog(LogLevel::Error, QStringLiteral("err-entry"));
    viewer.appendLog(LogLevel::Log, QStringLiteral("log-entry"));
    auto* filter = viewer.findChild<QComboBox*>();
    QVERIFY(filter);
    QCOMPARE(filter->count(), 4);
    QCOMPARE(filter->currentIndex(), 0);   // All

    QString text = viewerText(viewer);
    for (const char* entry : {"dbg-entry", "info-entry", "warn-entry", "err-entry", "log-entry"}) {
        QVERIFY2(text.contains(QLatin1String(entry)), entry);
    }

    filter->setCurrentIndex(1);   // Info+
    text = viewerText(viewer);
    QVERIFY(!text.contains(QStringLiteral("dbg-entry")));
    QVERIFY(text.contains(QStringLiteral("info-entry")));
    QVERIFY(text.contains(QStringLiteral("log-entry")));

    filter->setCurrentIndex(2);   // Warning+
    text = viewerText(viewer);
    QVERIFY(!text.contains(QStringLiteral("info-entry")));
    QVERIFY(text.contains(QStringLiteral("warn-entry")));
    QVERIFY(text.contains(QStringLiteral("err-entry")));
    QVERIFY(text.contains(QStringLiteral("log-entry")));   // LOG is always visible

    filter->setCurrentIndex(3);   // Error
    text = viewerText(viewer);
    QVERIFY(!text.contains(QStringLiteral("warn-entry")));
    QVERIFY(text.contains(QStringLiteral("err-entry")));

    // Entries appended while filtered are retained and reappear with "All".
    viewer.appendLog(LogLevel::Debug, QStringLiteral("late-dbg"));
    QVERIFY(!viewerText(viewer).contains(QStringLiteral("late-dbg")));
    filter->setCurrentIndex(0);
    text = viewerText(viewer);
    QVERIFY(text.contains(QStringLiteral("dbg-entry")));
    QVERIFY(text.contains(QStringLiteral("late-dbg")));
}

void Tst_mainwindow::sysLogMaxLinesTrimming()
{
    SystemLogViewer viewer;
    QCOMPARE(viewer.maxLines(), 2000);
    auto* edit = viewer.findChild<QTextEdit*>();
    QVERIFY(edit);

    viewer.setMaxLines(3);
    QCOMPARE(viewer.maxLines(), 3);
    for (int i = 1; i <= 5; ++i) {
        viewer.appendLog(LogLevel::Info, QStringLiteral("entry-%1").arg(i));
    }
    QVERIFY(edit->document()->blockCount() <= 3);
    QString text = viewerText(viewer);
    QVERIFY(!text.contains(QStringLiteral("entry-1")));
    QVERIFY(!text.contains(QStringLiteral("entry-2")));
    QVERIFY(text.contains(QStringLiteral("entry-3")));
    QVERIFY(text.contains(QStringLiteral("entry-5")));

    // Lowering the limit trims retained entries and the document; 0 clamps to 1.
    viewer.setMaxLines(0);
    QCOMPARE(viewer.maxLines(), 1);
    QCOMPARE(edit->document()->blockCount(), 1);
    text = viewerText(viewer);
    QVERIFY(!text.contains(QStringLiteral("entry-4")));
    QVERIFY(text.contains(QStringLiteral("entry-5")));
}

void Tst_mainwindow::sysLogClearAndCopyAll()
{
    SystemLogViewer viewer;
    viewer.appendLog(LogLevel::Info, QStringLiteral("copy-one"));
    viewer.appendLog(LogLevel::Warning, QStringLiteral("copy-two"));
    QApplication::clipboard()->setText(QStringLiteral("stale"));

    viewer.copyAll();
    const QString clip = QApplication::clipboard()->text();
    QCOMPARE(clip, viewerText(viewer));
    QVERIFY(clip.contains(QStringLiteral("copy-one")));
    QVERIFY(clip.contains(QStringLiteral("copy-two")));

    // The buttons drive the same slots.
    QPushButton* copyButton = nullptr;
    QPushButton* clearButton = nullptr;
    const QList<QPushButton*> buttons = viewer.findChildren<QPushButton*>();
    for (QPushButton* button : buttons) {
        if (button->text() == QStringLiteral("Copy All")) {
            copyButton = button;
        } else if (button->text() == QStringLiteral("Clear")) {
            clearButton = button;
        }
    }
    QVERIFY(copyButton);
    QVERIFY(clearButton);

    clearButton->click();
    QVERIFY(viewerText(viewer).isEmpty());
    viewer.appendLog(LogLevel::Info, QStringLiteral("after-clear"));
    QVERIFY(!viewerText(viewer).contains(QStringLiteral("copy-one")));
    copyButton->click();
    QCOMPARE(QApplication::clipboard()->text(), viewerText(viewer));
    QVERIFY(QApplication::clipboard()->text().contains(QStringLiteral("after-clear")));

    viewer.clearLogs();
    QVERIFY(viewerText(viewer).isEmpty());
}

QTEST_MAIN(Tst_mainwindow)
#include "tst_mainwindow.moc"
