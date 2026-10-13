// GUI test suite for the Firmware tab (src/ui/FirmwareTab.h) and its integration into MainWindow.
// Runs offscreen (QT_QPA_PLATFORM=offscreen). The tab is driven with a FakeToolRunner (no external
// process is ever started: no adb, no upgrade_tool), the collaborators' signals are emitted by the
// tests where only the tab's reaction matters, and the image pull connects to the in-process
// TestSshServer. QSettings and every file live in a temporary directory or the QStandardPaths
// test-mode data directory, so nothing touches the user's real configuration or board.
//
// Cases marked "needs ..." exercise a collaborator whose real implementation arrives with another
// work package (CcPackage::load, RockchipTool::isRkfwImage, McuUpdater::parseCliFields,
// ImagePuller); against the scaffolding stubs they fail by design.
#include <QtTest>

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QTranslator>

#include <algorithm>
#include <memory>

#include "app/AppSettings.h"
#include "core/Transport.h"
#include "firmware/CcProtocol.h"
#include "firmware/McuLink.h"
#include "ssh/SshConnection.h"
#include "ssh/SshProfile.h"
#include "support/FakeMcu.h"
#include "support/FakeToolRunner.h"
#include "support/TestSshServer.h"
#include "terminal/TerminalWidget.h"
#include "ui/FirmwareTab.h"
#include "ui/MainWindow.h"
#include "ui/SessionWidget.h"
#include "dialogs/AuthPromptDialog.h"
#include "dialogs/HostKeyDialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QSet>
#include <QTextStream>

// =======================================================================================
// Helpers
// =======================================================================================

/// Answers modal dialogs from inside their exec() loop (as in tst_mainwindow): QMessageBox
/// questions get Yes / No, everything else is rejected; counts what it dismissed.
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
    QStringList windowTitles() const { return m_windowTitles; }

private:
    void poll()
    {
        QWidget* modal = QApplication::activeModalWidget();
        if (!modal || !modal->isVisible()) {
            return;
        }
        ++m_count;
        m_windowTitles.append(modal->windowTitle());
        if (auto* box = qobject_cast<QMessageBox*>(modal); box && m_answer != Answer::Reject) {
            if (QAbstractButton* button = box->button(m_answer == Answer::Yes ? QMessageBox::Yes : QMessageBox::No)) {
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
    QStringList m_windowTitles;
};

/// Translates every FirmwareTab string to "[zz] <source>", so a LanguageChange shows exactly which
/// texts the tab re-reads (tst_firmwaretab has no compiled .qm; tst_mainwindow checks the Chinese one).
class MarkingTranslator : public QTranslator
{
public:
    QString translate(const char* context, const char* sourceText, const char* disambiguation = nullptr,
                      int n = -1) const override
    {
        Q_UNUSED(disambiguation);
        if (qstrcmp(context, "FirmwareTab") != 0) {
            return QString();
        }
        QString text = QString::fromUtf8(sourceText);
        if (n >= 0) {
            text.replace(QStringLiteral("%n"), QString::number(n));
        }
        return QStringLiteral("[zz] ") + text;
    }
    bool isEmpty() const override { return false; }
};

namespace {

constexpr int kOpTimeoutMs = 20000;   ///< an operation against fakes / the in-process server
const QString kLoopback = QStringLiteral("SIM:loopback");
const QString kMcu = QStringLiteral("SIM:mcu");
const QString kDefaultRemoteFolder = QStringLiteral("~/rk/rockchip-platforms/output/image");

QString dataFile(const char* name)
{
    return QStringLiteral(SU_TEST_DATA_DIR "/firmware/") + QLatin1String(name);
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

/// A file that starts with the Rockchip RKFW magic, like a real update.img.
QByteArray rkfwBytes(int size = 4096)
{
    QByteArray bytes("RKFW");
    bytes.append(QByteArray(size - 4, '\x5A'));
    return bytes;
}

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

QKeySequence seq(const char* portable)
{
    return QKeySequence(QString::fromLatin1(portable), QKeySequence::PortableText);
}

/// The dynamic per-tab entries of the Window menu: everything after its second separator.
QList<QAction*> windowEntries(const QMenu* windowMenu)
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

/// A tab as the tests use it: fake tools, no status refresh timer, tool names that exist nowhere.
std::unique_ptr<FirmwareTab> makeTab(FakeToolRunner* runner, SshProfileStore* store = nullptr)
{
    auto tab = std::make_unique<FirmwareTab>(store, runner);
    tab->setStatusRefreshIntervalMs(0);
    tab->setToolPaths(QStringLiteral("upgrade_tool"), QStringLiteral("adb"));
    return tab;
}

/// MainWindow creates its tab with a ProcessToolRunner: switch the status refresh off before the
/// first query could run and point the tools nowhere, so no real process ever starts.
FirmwareTab* openFirmwareTab(MainWindow& w)
{
    FirmwareTab* tab = w.showFirmwareTab();
    if (tab) {
        tab->setStatusRefreshIntervalMs(0);
        tab->setToolPaths(QStringLiteral("upgrade_tool-not-installed"), QStringLiteral("adb-not-installed"));
    }
    return tab;
}

QString lastLogLine(const FirmwareTab& tab)
{
    const QStringList lines = tab.logView()->toPlainText().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    return lines.isEmpty() ? QString() : lines.last();
}

const QByteArray kAdbBoard = "List of devices attached\r\n0225ce3c8d8d6e44\tdevice\r\n\r\n";
const QByteArray kLdLoader = "rockusb driver = 5.12,please upgrade it to enjoy fast download\r\n\r\n"
                             "List of rockusb connected(1)\r\n"
                             "DevNo=1\tVid=0x2207,Pid=0x110e,LocationID=104\tMode=Loader\tSerialNo=\r\n";
const QByteArray kProcVersion = "Linux version 5.10.209+ (builder@rk) #1 Tue Oct 13 21:40:00 CST 2026\n"
                                " 10:42:01 up 0 min,  load average: 0.10, 0.03, 0.01\n";

/// The board side of an MCU update as adb_link.py drives it: `adb devices`, the rfmcu probe,
/// `killall`, a forward that points at a FakeMcuServer (the stand-in for `rfmcu bridge`), the
/// bridge itself (prints "bridging", then runs until cancelled) and the forward removal.
void scriptMcuBridge(FakeToolRunner& runner, quint16 bridgePort)
{
    runner.addScript(QRegularExpression(QStringLiteral("^adb( -s \\S+)? devices$")), kAdbBoard);
    runner.addScript(QRegularExpression(QStringLiteral("command -v rfmcu")), "/usr/bin/rfmcu\n");
    runner.addScript(QRegularExpression(QStringLiteral("killall rfmcu")), QByteArray());
    runner.addScript(QRegularExpression(QStringLiteral("forward tcp:0 tcp:7000$")), QByteArray::number(bridgePort) + "\n");
    runner.addHandler(QRegularExpression(QStringLiteral("shell rfmcu bridge 7000$")), [](FakeToolInvocation* i) {
        i->emitOutput("rfmcu: bridging /dev/ttyS1 (115200 baud) to 127.0.0.1:7000\n");
        i->hang();
    });
    runner.addScript(QRegularExpression(QStringLiteral("forward --remove tcp:\\d+$")), QByteArray());
}

/// The in-process build machine: TestSshServer serving `<root>/image/update.img`, a stored profile
/// with a test key (never the developer's ~/.ssh keys) and a known_hosts file holding the server.
struct BuildMachine
{
    QTemporaryDir root;
    std::unique_ptr<TestSshServer> server;
    SshProfile profile;
    QString imageFolder;
    QByteArray image = rkfwBytes(256 * 1024);

    bool start(SshProfileStore* store, const QString& workDir)
    {
        if (!root.isValid()) {
            return false;
        }
        imageFolder = root.path() + QStringLiteral("/image");
        if (!writeFile(imageFolder + QStringLiteral("/update.img"), image)) {
            return false;
        }
        if (!QDir().mkpath(workDir)) {
            return false;
        }
        const QString keyPath = workDir + QStringLiteral("/client_ed25519");
        QFile::remove(keyPath);
        QString publicKey;
        if (!TestSshServer::generateClientKeyPair(keyPath, &publicKey)) {
            return false;
        }
        TestSshServer::Options options;
        options.rootDir = root.path();
        options.authorizedPublicKey = publicKey;
        options.allowPassword = false;
        server = std::make_unique<TestSshServer>(options);
        QString error;
        if (!server->start(&error)) {
            qWarning() << "TestSshServer:" << error;
            return false;
        }
        const QString knownHosts = workDir + QStringLiteral("/known_hosts");
        if (!writeFile(knownHosts, (server->knownHostsLine() + QLatin1Char('\n')).toUtf8())) {
            return false;
        }
        SshProfile p;
        p.name = QStringLiteral("Build machine");
        p.host = QStringLiteral("127.0.0.1");
        p.port = server->port();
        p.user = options.user;
        p.auth = SshProfile::Auth::PublicKey;
        p.identityFile = keyPath;
        p.knownHostsFile = knownHosts;
        p.connectTimeoutSeconds = 5;
        profile = store->upsert(p);
        return !profile.id.isEmpty();
    }
};

} // namespace

// =======================================================================================
// Test class
// =======================================================================================

class Tst_firmwaretab : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    // ---- The tab on its own ---------------------------------------------------------
    void widgetsExistWithObjectNames();
    void initialEnableStates();
    void defaultsFromSettings();
    void appSettingsFirmwareKeys();
    void settingsRoundTrip();
    void storedProfileRememberedById();
    void packageAccepted();            // needs package protocol
    void packageRefused();
    void imageAccepted();              // needs RockchipTool::isRkfwImage
    void imageRefused();
    void appendLogTimestamps();
    void busyAroundFlash();
    void deviceStatusLine();
    void installDriversWithoutInstaller();
    void mcuNeedsPackageAndRole();
    void readMcuInfoFillsRole();       // needs McuUpdater::parseCliFields
    void readMcuInfoPortBusy();
    void pullWithoutTargetFails();
    void pullConnectsAndCloses();
    void pullDownloadsImage();         // needs ImagePuller
    void mcuUpdateAndRoleThroughFakeBridge();   // needs McuLink + McuUpdater + CcPackage
    void pullAndFlashEndToEnd();       // needs ImagePuller + SocFlasher + McuLink
    void languageChangeRetranslates();

    // ---- MainWindow integration -----------------------------------------------------
    void showFirmwareTabOnce();
    void sessionActionsDisabledWhileCurrent();
    void windowMenuAndAltN();
    void closeTabWhenIdle();
    void closeRefusedWhileBusyAndQuitAsks();
    void restoreKeysExcludeFirmwareTab();
    void shortcutListedAndReserved();
    void statusBarShowsTabMessages();

    // ---- Real board + real build machine (env-gated; the first real flash of the spec) ----
    void realBoard_pullFlashUpdate();

private:
    bool showAndActivate(MainWindow& w);
    QString tempPath(const QString& name) const;

    QTemporaryDir m_tempDir;
    int m_caseNumber = 0;
};

// =======================================================================================
// Fixture
// =======================================================================================

void Tst_firmwaretab::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("BuildAI-Test"));
    QCoreApplication::setApplicationName(QStringLiteral("SerialUtilityTest-firmwaretab"));
    QVERIFY(m_tempDir.isValid());

    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_tempDir.filePath(QStringLiteral("settings")));
    QSettings settings;
    settings.clear();
    QVERIFY(settings.fileName().contains(QStringLiteral("SerialUtilityTest-firmwaretab")));

    // "78,040 bytes" whatever the machine's locale is.
    QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
    // The SSH connects of this suite must not read ~/.ssh/config.
    qputenv("SU_SSH_IGNORE_CONFIG", "1");
    // A tab that MainWindow creates uses the real ProcessToolRunner, and ToolLocator falls back to
    // an adb on PATH: take every such directory off PATH so no case can ever reach a real board.
    QStringList path = qEnvironmentVariable("PATH").split(QDir::listSeparator(), Qt::SkipEmptyParts);
    path.erase(std::remove_if(path.begin(), path.end(),
                              [](const QString& dir) {
                                  return QFileInfo::exists(dir + QStringLiteral("/adb.exe")) ||
                                         QFileInfo::exists(dir + QStringLiteral("/adb")) ||
                                         QFileInfo::exists(dir + QStringLiteral("/upgrade_tool.exe")) ||
                                         QFileInfo::exists(dir + QStringLiteral("/upgrade_tool"));
                              }),
               path.end());
    qputenv("PATH", path.join(QDir::listSeparator()).toLocal8Bit());
}

void Tst_firmwaretab::init()
{
    QSettings().clear();
    AppSettings& app = AppSettings::instance();
    app.setLanguage(QStringLiteral("en_US"));
    app.setLogDirectory(m_tempDir.filePath(QStringLiteral("logs")));
    app.setShowSimulatedPorts(true);
    app.setConfirmCloseWhenConnected(true);
    app.setRestoreLastPorts(true);
    app.setSshKnownHostsFile(m_tempDir.filePath(QStringLiteral("no_known_hosts")));   // never ~/.ssh
    // Every case starts with its own, empty image cache.
    app.setFirmwareCacheDirectory(m_tempDir.filePath(QStringLiteral("cache-%1").arg(++m_caseNumber)));
}

void Tst_firmwaretab::cleanup()
{
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

bool Tst_firmwaretab::showAndActivate(MainWindow& w)
{
    w.show();
    if (!QTest::qWaitForWindowExposed(&w)) {
        return false;
    }
    w.activateWindow();
    return QTest::qWaitFor([&w]() { return QApplication::activeWindow() == &w; }, 5000);
}

QString Tst_firmwaretab::tempPath(const QString& name) const
{
    return m_tempDir.filePath(name);
}

// =======================================================================================
// The tab on its own
// =======================================================================================

void Tst_firmwaretab::widgetsExistWithObjectNames()
{
    FakeToolRunner runner;
    auto tab = makeTab(&runner);
    const struct
    {
        QWidget* widget;
        const char* name;
    } widgets[] = {{tab->sourceCombo(), "sourceCombo"},
                   {tab->remoteFolderEdit(), "remoteFolderEdit"},
                   {tab->cacheDirEdit(), "cacheDirEdit"},
                   {tab->pullButton(), "pullButton"},
                   {tab->localImageButton(), "localImageButton"},
                   {tab->imageInfoLabel(), "imageInfoLabel"},
                   {tab->rollbackButton(), "rollbackButton"},
                   {tab->socStatusLabel(), "socStatusLabel"},
                   {tab->socImageLabel(), "socImageLabel"},
                   {tab->flashSocButton(), "flashSocButton"},
                   {tab->cancelButton(), "cancelButton"},
                   {tab->packageEdit(), "packageEdit"},
                   {tab->packageButton(), "packageButton"},
                   {tab->packageInfoLabel(), "packageInfoLabel"},
                   {tab->mcuStatusLabel(), "mcuStatusLabel"},
                   {tab->readMcuButton(), "readMcuButton"},
                   {tab->roleRxRadio(), "roleRxRadio"},
                   {tab->roleTxRadio(), "roleTxRadio"},
                   {tab->updateMcuButton(), "updateMcuButton"},
                   {tab->setRoleButton(), "setRoleButton"},
                   {tab->pullAndFlashButton(), "pullAndFlashButton"},
                   {tab->progressBar(), "progressBar"},
                   {tab->progressLabel(), "progressLabel"},
                   {tab->logView(), "logView"},
                   {tab->clearLogButton(), "clearLogButton"},
                   {tab->saveLogButton(), "saveLogButton"},
                   {tab->installDriversButton(), "installDriversButton"}};
    for (const auto& entry : widgets) {
        QVERIFY2(entry.widget != nullptr, entry.name);
        QCOMPARE(entry.widget->objectName(), QString::fromLatin1(entry.name));
        QVERIFY2(tab->isAncestorOf(entry.widget), entry.name);
        QCOMPARE(tab->findChild<QWidget*>(QLatin1String(entry.name)), entry.widget);
    }
    // Section titles and the main button as the layout sketch has them.
    for (const char* group : {"sourceGroup", "socGroup", "mcuGroup", "logGroup"}) {
        QVERIFY2(child<QGroupBox>(tab.get(), group) != nullptr, group);
    }
    QCOMPARE(child<QGroupBox>(tab.get(), "mcuGroup")->title(), QStringLiteral("MCU (CC1310)"));
    QCOMPARE(tab->pullAndFlashButton()->text(), QStringLiteral("Pull && Flash"));   // "&&" = a literal '&'
    QVERIFY(tab->sourceCombo()->isEditable());
    QVERIFY(tab->logView()->isReadOnly());

    QCOMPARE(tab->title(), QStringLiteral("Firmware"));
    QVERIFY(!tab->isBusy());
    QVERIFY(tab->currentOperation().isEmpty());
    QVERIFY(tab->persistentStatusMessage().isEmpty());
    QCOMPARE(tab->toolRunner(), static_cast<ToolRunner*>(&runner));
    QVERIFY(tab->socFlasher() && tab->mcuLink() && tab->mcuUpdater() && tab->imagePuller() && tab->sshConnection());
    QCOMPARE(tab->sshConnection()->state(), Transport::State::Disconnected);
    QVERIFY(tab->installDriversButton()->isHidden());   // only with the NoDriver status
    QCOMPARE(tab->statusRefreshIntervalMs(), 0);

    // The defaults of a tab nobody configured: a 2 s status refresh and its own process runner.
    FirmwareTab plain(nullptr);
    QCOMPARE(plain.statusRefreshIntervalMs(), 2000);
    QVERIFY(qobject_cast<ProcessToolRunner*>(plain.toolRunner()) != nullptr);
    plain.setStatusRefreshIntervalMs(-5);
    QCOMPARE(plain.statusRefreshIntervalMs(), 0);
}

void Tst_firmwaretab::initialEnableStates()
{
    FakeToolRunner runner;
    auto tab = makeTab(&runner);

    // Nothing to flash, no package, no role: only the "get something" buttons work.
    QVERIFY(tab->imagePath().isEmpty());
    QVERIFY(!tab->flashSocButton()->isEnabled());
    QVERIFY(!tab->updateMcuButton()->isEnabled());
    QVERIFY(!tab->setRoleButton()->isEnabled());
    QVERIFY(!tab->cancelButton()->isEnabled());
    QVERIFY(!tab->rollbackButton()->isEnabled());
    for (QWidget* enabled : {static_cast<QWidget*>(tab->pullButton()), static_cast<QWidget*>(tab->localImageButton()),
                             static_cast<QWidget*>(tab->readMcuButton()), static_cast<QWidget*>(tab->pullAndFlashButton()),
                             static_cast<QWidget*>(tab->packageButton()), static_cast<QWidget*>(tab->sourceCombo()),
                             static_cast<QWidget*>(tab->clearLogButton()), static_cast<QWidget*>(tab->saveLogButton())}) {
        QVERIFY2(enabled->isEnabled(), qPrintable(enabled->objectName()));
    }
    QCOMPARE(tab->selectedRole(), 0u);
    QVERIFY(!tab->roleRxRadio()->isChecked());
    QVERIFY(!tab->roleTxRadio()->isChecked());
    QVERIFY(tab->runningMcuVersion().isEmpty());
    QVERIFY(tab->runningMcuRole().isEmpty());

    // The role is what the user confirms: a click enables Set role; Update MCU also needs a package.
    tab->roleTxRadio()->click();
    QCOMPARE(tab->selectedRole(), static_cast<quint32>(CcProtocol::RoleTx));
    QVERIFY(tab->setRoleButton()->isEnabled());
    QVERIFY(!tab->updateMcuButton()->isEnabled());
    tab->setSelectedRole(CcProtocol::RoleRx);
    QVERIFY(tab->roleRxRadio()->isChecked());
    QVERIFY(!tab->roleTxRadio()->isChecked());
    tab->setSelectedRole(0);   // "none" again: both radios clear
    QVERIFY(!tab->roleRxRadio()->isChecked());
    QVERIFY(!tab->roleTxRadio()->isChecked());
    QVERIFY(!tab->setRoleButton()->isEnabled());
    tab->setSelectedRole(7);   // not a role
    QCOMPARE(tab->selectedRole(), 0u);

    // The cache's update.img is the image Flash SoC uses: it enables the button.
    const QString cache = tempPath(QStringLiteral("enable-cache"));
    QVERIFY(writeFile(cache + QStringLiteral("/update.img"), rkfwBytes()));
    tab->setCacheDirectory(cache);
    QCOMPARE(tab->imagePath(), cache + QStringLiteral("/update.img"));
    QVERIFY(tab->flashSocButton()->isEnabled());
    QVERIFY(tab->imageInfoLabel()->text().contains(QStringLiteral("update.img")));
    QVERIFY(!tab->rollbackButton()->isEnabled());   // no update.prev.img yet
    QVERIFY(writeFile(cache + QStringLiteral("/update.prev.img"), rkfwBytes()));
    tab->setCacheDirectory(cache);
    QVERIFY(tab->rollbackButton()->isEnabled());
    QVERIFY(tab->imageInfoLabel()->text().contains(QStringLiteral("previous")));
}

void Tst_firmwaretab::defaultsFromSettings()
{
    QSettings().remove(QStringLiteral("firmware/cacheDirectory"));
    FakeToolRunner runner;
    auto tab = makeTab(&runner);
    QVERIFY(tab->sourceTarget().isEmpty());
    QCOMPARE(tab->remoteFolder(), kDefaultRemoteFolder);
    QCOMPARE(tab->cacheDirectory(), AppSettings::dataDirectory() + QStringLiteral("/firmware"));
    QVERIFY(tab->packagePath().isEmpty());
    QVERIFY(!tab->package().isValid());
    QCOMPARE(tab->selectedRole(), 0u);
    QCOMPARE(tab->remoteFolderEdit()->placeholderText(), kDefaultRemoteFolder);
}

void Tst_firmwaretab::appSettingsFirmwareKeys()
{
    AppSettings& s = AppSettings::instance();
    QSettings().clear();
    // Defaults.
    QVERIFY(s.firmwareSourceTarget().isEmpty());
    QCOMPARE(AppSettings::defaultFirmwareRemoteFolder(), kDefaultRemoteFolder);
    QCOMPARE(s.firmwareRemoteFolder(QStringLiteral("anyone@anywhere")), kDefaultRemoteFolder);
    QVERIFY(s.firmwareLastRemoteFolder().isEmpty());
    QCOMPARE(s.firmwareCacheDirectory(), AppSettings::dataDirectory() + QStringLiteral("/firmware"));
    QVERIFY(s.firmwareLocalImage().isEmpty());
    QVERIFY(s.firmwarePackagePath().isEmpty());
    QVERIFY(s.firmwareUpgradeToolPath().isEmpty());
    QVERIFY(s.firmwareAdbPath().isEmpty());
    QCOMPARE(s.firmwareLoaderWaitSeconds(), 30);
    QCOMPARE(s.firmwareAdbReturnSeconds(), 60);
    QVERIFY(s.firmwareGitDescribe());
    QVERIFY(s.firmwareExtraFiles().isEmpty());

    // Every setter writes through and reports its key.
    QSignalSpy changed(&s, &AppSettings::changed);
    s.setFirmwareRemoteFolder(QStringLiteral("a@host"), QStringLiteral(" /srv/a "));
    QCOMPARE(s.firmwareRemoteFolder(QStringLiteral("a@host")), QStringLiteral("/srv/a"));
    QCOMPARE(s.firmwareLastRemoteFolder(), QStringLiteral("/srv/a"));
    QCOMPARE(s.firmwareRemoteFolder(QStringLiteral("new@host")), QStringLiteral("/srv/a"));   // the last one used
    s.setFirmwareRemoteFolder(QStringLiteral("b@host"), QStringLiteral("/srv/b"));
    QCOMPARE(s.firmwareRemoteFolder(QStringLiteral("a@host")), QStringLiteral("/srv/a"));     // kept per target
    QCOMPARE(s.firmwareRemoteFolder(QStringLiteral("new@host")), QStringLiteral("/srv/b"));
    QCOMPARE(QSettings().value(QStringLiteral("firmware/remoteFolders")).toMap().size(), qsizetype(2));
    QVERIFY(changed.count() >= 4);
    bool sawMap = false;
    for (const QList<QVariant>& args : std::as_const(changed)) {
        sawMap = sawMap || args.at(0).toString() == QLatin1String("firmware/remoteFolders");
    }
    QVERIFY(sawMap);

    // Clamps 5..600 s.
    s.setFirmwareLoaderWaitSeconds(1);
    QCOMPARE(s.firmwareLoaderWaitSeconds(), 5);
    s.setFirmwareLoaderWaitSeconds(100000);
    QCOMPARE(s.firmwareLoaderWaitSeconds(), 600);
    QSettings().setValue(QStringLiteral("firmware/adbReturnSeconds"), QStringLiteral("garbage"));
    QCOMPARE(s.firmwareAdbReturnSeconds(), 60);
    s.setFirmwareAdbReturnSeconds(2);
    QCOMPARE(s.firmwareAdbReturnSeconds(), 5);

    // Extra files: trimmed, empty and repeated names dropped.
    s.setFirmwareExtraFiles({QStringLiteral(" sd_update.txt "), QString(), QStringLiteral("parameter.txt"),
                             QStringLiteral("sd_update.txt")});
    QCOMPARE(s.firmwareExtraFiles(), (QStringList{QStringLiteral("sd_update.txt"), QStringLiteral("parameter.txt")}));

    // An empty cache folder means the default again.
    s.setFirmwareCacheDirectory(QStringLiteral("D:/fw"));
    QCOMPARE(s.firmwareCacheDirectory(), QStringLiteral("D:/fw"));
    s.setFirmwareCacheDirectory(QString());
    QCOMPARE(s.firmwareCacheDirectory(), AppSettings::dataDirectory() + QStringLiteral("/firmware"));
}

void Tst_firmwaretab::settingsRoundTrip()
{
    const QString cache = tempPath(QStringLiteral("roundtrip-cache"));
    const QString targetA = QStringLiteral("test@127.0.0.1:2222");
    const QString targetB = QStringLiteral("pi@10.0.0.7");
    const QString folderA = QStringLiteral("/srv/a/output/image");
    const QString folderB = QStringLiteral("/srv/b/output/image");
    {
        FakeToolRunner runner;
        auto tab = makeTab(&runner);
        tab->setSourceTarget(targetA);
        QCOMPARE(tab->sourceTarget(), targetA);
        tab->setRemoteFolder(folderA);
        tab->setSourceTarget(targetB);
        QCOMPARE(tab->remoteFolder(), folderA);   // a new target starts with the last folder used
        tab->setRemoteFolder(folderB);
        tab->setCacheDirectory(cache);
        tab->setSelectedRole(CcProtocol::RoleTx);
    }
    const AppSettings& s = AppSettings::instance();
    QCOMPARE(s.firmwareSourceTarget(), targetB);
    QCOMPARE(s.firmwareRemoteFolder(targetA), folderA);
    QCOMPARE(s.firmwareRemoteFolder(targetB), folderB);
    QCOMPARE(s.firmwareCacheDirectory(), cache);
    QCOMPARE(QSettings().value(QStringLiteral("firmware/role")).toString(), QStringLiteral("tx"));

    FakeToolRunner runner;
    auto tab = makeTab(&runner);
    QCOMPARE(tab->sourceTarget(), targetB);
    QCOMPARE(tab->remoteFolder(), folderB);
    QCOMPARE(tab->cacheDirectory(), cache);
    QCOMPARE(QDir::fromNativeSeparators(tab->cacheDirEdit()->text()), cache);
    QCOMPARE(tab->selectedRole(), static_cast<quint32>(CcProtocol::RoleTx));
    QVERIFY(tab->roleTxRadio()->isChecked());
    // Each target brings its own folder back; an unknown one gets the last folder used.
    tab->setSourceTarget(targetA);
    QCOMPARE(tab->remoteFolder(), folderA);
    tab->setSourceTarget(QStringLiteral("ssh:target:root@10.0.0.24"));   // a session restore key works too
    QCOMPARE(tab->sourceTarget(), QStringLiteral("root@10.0.0.24"));
    QCOMPARE(tab->remoteFolder(), folderB);
    // Typing into the folder field and leaving it remembers it for the current target.
    tab->remoteFolderEdit()->setText(QStringLiteral("/typed/image"));
    emit tab->remoteFolderEdit()->editingFinished();
    QCOMPARE(s.firmwareRemoteFolder(QStringLiteral("root@10.0.0.24")), QStringLiteral("/typed/image"));
}

void Tst_firmwaretab::storedProfileRememberedById()
{
    SshProfileStore store;
    SshProfile p;
    p.name = QStringLiteral("Build VM");
    p.host = QStringLiteral("10.0.10.71");
    p.user = QStringLiteral("moses");
    const SshProfile stored = store.upsert(p);
    store.addRecentTarget(QStringLiteral("root@10.0.0.24"));
    const QString itemText = QStringLiteral("Build VM - moses@10.0.10.71");
    {
        FakeToolRunner runner;
        auto tab = makeTab(&runner, &store);
        // Same items as the SSH bar: the profiles, a separator, the recent targets.
        QComboBox* combo = tab->sourceCombo();
        QVERIFY(combo->findText(itemText) >= 0);
        QVERIFY(combo->findText(QStringLiteral("root@10.0.0.24")) >= 0);
        tab->setSourceTarget(QStringLiteral("ssh:profile:") + stored.id);
        QCOMPARE(tab->sourceTarget(), itemText);
        tab->setRemoteFolder(QStringLiteral("/vm/image"));
        QCOMPARE(AppSettings::instance().firmwareSourceTarget(), QStringLiteral("ssh:profile:") + stored.id);
        // Picking the recent target from the list loads its folder (the last one used).
        combo->setCurrentIndex(combo->findText(QStringLiteral("root@10.0.0.24")));
        emit combo->activated(combo->currentIndex());
        QCOMPARE(tab->sourceTarget(), QStringLiteral("root@10.0.0.24"));
        QCOMPARE(tab->remoteFolder(), QStringLiteral("/vm/image"));
        tab->setSourceTarget(QStringLiteral("ssh:profile:") + stored.id);
    }
    // A rename in the profile manager keeps the choice: the id is what is remembered.
    SshProfile renamed = stored;
    renamed.name = QStringLiteral("Build VM 2");
    store.upsert(renamed);
    FakeToolRunner runner;
    auto tab = makeTab(&runner, &store);
    QCOMPARE(tab->sourceTarget(), QStringLiteral("Build VM 2 - moses@10.0.10.71"));
    QCOMPARE(tab->remoteFolder(), QStringLiteral("/vm/image"));
}

void Tst_firmwaretab::packageAccepted()
{
    // needs package protocol: CcPackage::load() / parse() (protocol work package).
    FakeToolRunner runner;
    auto tab = makeTab(&runner);
    const QString pkg = dataFile("firmware-0.8.1.pkg");
    QVERIFY(QFileInfo::exists(pkg));
    QVERIFY(tab->setPackagePath(pkg));
    QCOMPARE(tab->packagePath(), pkg);
    QVERIFY(tab->package().isValid());
    QCOMPARE(tab->package().versionText(), QStringLiteral("0.8.1"));
    QCOMPARE(tab->package().size, 78040u);
    QVERIFY2(tab->packageInfoLabel()->text().contains(QStringLiteral("0.8.1")), qPrintable(tab->packageInfoLabel()->text()));
    QVERIFY2(tab->packageInfoLabel()->text().contains(QStringLiteral("78,040")), qPrintable(tab->packageInfoLabel()->text()));
    QCOMPARE(QDir::fromNativeSeparators(tab->packageEdit()->text()), pkg);
    QCOMPARE(AppSettings::instance().firmwarePackagePath(), pkg);
    // With a package, a confirmed role enables Update MCU.
    QVERIFY(!tab->updateMcuButton()->isEnabled());
    tab->setSelectedRole(CcProtocol::RoleRx);
    QVERIFY(tab->updateMcuButton()->isEnabled());

    // A bad file afterwards is refused and the good package stays.
    const QString bad = tempPath(QStringLiteral("after-good.pkg"));
    QVERIFY(writeFile(bad, QByteArray(64, '\x11')));
    QVERIFY(!tab->setPackagePath(bad));
    QCOMPARE(tab->packagePath(), pkg);
    QVERIFY(tab->packageInfoLabel()->text().contains(QStringLiteral("0.8.1")));

    // The next tab loads the remembered package again.
    FakeToolRunner runner2;
    auto second = makeTab(&runner2);
    QCOMPARE(second->packagePath(), pkg);
    QVERIFY(second->package().isValid());
}

void Tst_firmwaretab::packageRefused()
{
    FakeToolRunner runner;
    auto tab = makeTab(&runner);
    QSignalSpy status(tab.get(), &FirmwareTab::statusMessage);
    const QString bad = tempPath(QStringLiteral("bad.pkg"));
    QVERIFY(writeFile(bad, QByteArray(64, '\x11')));
    QVERIFY(!tab->setPackagePath(bad));
    QVERIFY(tab->packagePath().isEmpty());
    QVERIFY(!tab->package().isValid());
    QVERIFY(!status.isEmpty());
    QVERIFY2(status.last().at(0).toString().contains(QStringLiteral("bad.pkg")), qPrintable(status.last().at(0).toString()));
    QVERIFY(lastLogLine(*tab).contains(QStringLiteral("refused")));
    QVERIFY(tab->packageInfoLabel()->text().contains(QStringLiteral("bad.pkg")));   // the reason is shown
    QVERIFY(AppSettings::instance().firmwarePackagePath().isEmpty());
    tab->setSelectedRole(CcProtocol::RoleRx);
    QVERIFY(!tab->updateMcuButton()->isEnabled());   // no package, no update

    QVERIFY(!tab->setPackagePath(tempPath(QStringLiteral("missing.pkg"))));
    QVERIFY(tab->packagePath().isEmpty());
    // Typing a bad path into the field is the same refusal; the field goes back.
    tab->packageEdit()->setText(QDir::toNativeSeparators(bad));
    emit tab->packageEdit()->editingFinished();
    QVERIFY(tab->packageEdit()->text().isEmpty());
}

void Tst_firmwaretab::imageAccepted()
{
    // needs RockchipTool::isRkfwImage() (tools work package).
    FakeToolRunner runner;
    auto tab = makeTab(&runner);
    const QString image = tempPath(QStringLiteral("local/my update.img"));
    QVERIFY(writeFile(image, rkfwBytes()));
    tab->setImagePath(image);
    QCOMPARE(tab->imagePath(), image);
    QVERIFY(tab->imageInfoLabel()->text().contains(QStringLiteral("my update.img")));
    QVERIFY(tab->flashSocButton()->isEnabled());
    QCOMPARE(AppSettings::instance().firmwareLocalImage(), image);
    {
        FakeToolRunner runner2;
        auto second = makeTab(&runner2);
        QCOMPARE(second->imagePath(), image);   // the pick is remembered
    }
    tab->setImagePath(QString());   // back to the cache (empty here)
    QVERIFY(tab->imagePath().isEmpty());
    QVERIFY(AppSettings::instance().firmwareLocalImage().isEmpty());
}

void Tst_firmwaretab::imageRefused()
{
    FakeToolRunner runner;
    auto tab = makeTab(&runner);
    QSignalSpy status(tab.get(), &FirmwareTab::statusMessage);
    const QString wrong = tempPath(QStringLiteral("wrong.img"));
    QVERIFY(writeFile(wrong, QByteArray("ABCD") + QByteArray(1024, '\0')));
    tab->setImagePath(wrong);
    QVERIFY(tab->imagePath().isEmpty());
    QVERIFY(!status.isEmpty());
    QVERIFY(status.last().at(0).toString().contains(QStringLiteral("wrong.img")));
    QVERIFY(lastLogLine(*tab).contains(QStringLiteral("refused")));
    QVERIFY(!tab->flashSocButton()->isEnabled());
    QVERIFY(AppSettings::instance().firmwareLocalImage().isEmpty());
    tab->setImagePath(tempPath(QStringLiteral("does-not-exist.img")));
    QVERIFY(tab->imagePath().isEmpty());
}

void Tst_firmwaretab::appendLogTimestamps()
{
    FakeToolRunner runner;
    auto tab = makeTab(&runner);
    tab->clearLog();
    QVERIFY(tab->logView()->toPlainText().isEmpty());
    QSignalSpy added(tab.get(), &FirmwareTab::logLineAdded);
    const QRegularExpression stamped(QStringLiteral("^\\d\\d:\\d\\d:\\d\\d hello firmware$"));

    // Mirrored into the System Log through the "buildai.firmware" category.
    static QStringList s_mirrored;
    s_mirrored.clear();
    const QtMessageHandler previous = qInstallMessageHandler(
        [](QtMsgType, const QMessageLogContext& context, const QString& message) {
            if (context.category && qstrcmp(context.category, "buildai.firmware") == 0) {
                s_mirrored.append(message);
            }
        });
    auto restore = qScopeGuard([previous]() { qInstallMessageHandler(previous); });

    tab->appendLog(QStringLiteral("hello firmware"));
    QCOMPARE(added.count(), qsizetype(1));
    const QString line = added.at(0).at(0).toString();
    QVERIFY2(stamped.match(line).hasMatch(), qPrintable(line));
    QCOMPARE(lastLogLine(*tab), line);
    QVERIFY(s_mirrored.contains(QStringLiteral("hello firmware")));

    // Multi-line text: one stamped line each, blank lines and CR LF dropped.
    added.clear();
    tab->appendLog(QStringLiteral("first\r\nsecond\n\nthird\r"));
    QCOMPARE(added.count(), qsizetype(3));
    QVERIFY(added.at(2).at(0).toString().endsWith(QStringLiteral(" third")));
    QCOMPARE(tab->logView()->blockCount(), 4);

    tab->clearLog();
    QVERIFY(tab->logView()->toPlainText().isEmpty());
    tab->clearLogButton()->click();   // the button does the same, harmlessly on an empty pane
    QVERIFY(tab->logView()->toPlainText().isEmpty());
}

void Tst_firmwaretab::busyAroundFlash()
{
    FakeToolRunner runner;
    // What the real SocFlasher asks once it lands: no board on rockusb or ADB, so it waits for
    // Loader mode (1 s here) and gives up. The scaffolding stub refuses start() outright.
    runner.addScript(QRegularExpression(QStringLiteral("^upgrade_tool LD")), "List of rockusb connected(0)\r\n", 0);
    runner.addScript(QRegularExpression(QStringLiteral("^adb( -s \\S+)? devices")), "List of devices attached\r\n\r\n", 0);
    runner.addScript(QRegularExpression(QStringLiteral("^adb( -s \\S+)? get-state")), "error: no devices/emulators found\r\n", 1);
    auto tab = makeTab(&runner);
    tab->socFlasher()->setWaitTimeoutSeconds(1);
    tab->socFlasher()->setPollIntervalMs(50);
    const QString cache = tempPath(QStringLiteral("flash-cache"));
    QVERIFY(writeFile(cache + QStringLiteral("/update.img"), rkfwBytes()));
    tab->setCacheDirectory(cache);
    QVERIFY(tab->flashSocButton()->isEnabled());

    QSignalSpy busy(tab.get(), &FirmwareTab::busyChanged);
    QSignalSpy finished(tab.get(), &FirmwareTab::operationFinished);
    QSignalSpy status(tab.get(), &FirmwareTab::statusMessage);
    tab->flashSocButton()->click();
    QCOMPARE(busy.count(), qsizetype(1));
    QCOMPARE(busy.at(0).at(0).toBool(), true);
    QVERIFY(tab->isBusy());
    QCOMPARE(tab->currentOperation(), QStringLiteral("flash"));
    QVERIFY(!tab->persistentStatusMessage().isEmpty());
    for (QWidget* off : {static_cast<QWidget*>(tab->pullButton()), static_cast<QWidget*>(tab->flashSocButton()),
                         static_cast<QWidget*>(tab->pullAndFlashButton()), static_cast<QWidget*>(tab->readMcuButton()),
                         static_cast<QWidget*>(tab->sourceCombo()), static_cast<QWidget*>(tab->localImageButton())}) {
        QVERIFY2(!off->isEnabled(), qPrintable(off->objectName()));
    }

    // Another operation is refused while one runs: a status message, no second busyChanged.
    status.clear();
    tab->pull();
    tab->readMcuInfo();
    QCOMPARE(busy.count(), qsizetype(1));
    QVERIFY(!status.isEmpty());
    QVERIFY(status.last().at(0).toString().startsWith(QStringLiteral("Busy")));
    QCOMPARE(tab->currentOperation(), QStringLiteral("flash"));

    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), qsizetype(1), kOpTimeoutMs);
    QCOMPARE(finished.at(0).at(0).toString(), QStringLiteral("flash"));
    QCOMPARE(finished.at(0).at(1).toBool(), false);
    QVERIFY(!finished.at(0).at(2).toString().isEmpty());
    QCOMPARE(busy.count(), qsizetype(2));
    QCOMPARE(busy.at(1).at(0).toBool(), false);
    QVERIFY(!tab->isBusy());
    QVERIFY(tab->currentOperation().isEmpty());
    QVERIFY(tab->persistentStatusMessage().isEmpty());
    QVERIFY(tab->flashSocButton()->isEnabled());
    QVERIFY(tab->pullButton()->isEnabled());
    QVERIFY(!tab->cancelButton()->isEnabled());
    QVERIFY(tab->logView()->toPlainText().contains(QStringLiteral("Flash SoC failed")));
    QVERIFY(tab->progressLabel()->text().contains(QStringLiteral("Flash SoC failed")));

    // Without an image the step fails at once, with the reason, still framed by busyChanged.
    busy.clear();
    finished.clear();
    tab->setCacheDirectory(tempPath(QStringLiteral("empty-cache")));
    QVERIFY(tab->imagePath().isEmpty());
    tab->flashSoc();
    QCOMPARE(busy.count(), qsizetype(2));
    QCOMPARE(finished.count(), qsizetype(1));
    QCOMPARE(finished.at(0).at(1).toBool(), false);
    QVERIFY(finished.at(0).at(2).toString().contains(QStringLiteral("image")));
    // Cancel outside an operation does nothing.
    tab->cancel();
    QVERIFY(!tab->isBusy());
}

void Tst_firmwaretab::deviceStatusLine()
{
    FakeToolRunner runner;
    auto tab = makeTab(&runner);
    QVERIFY(!tab->socStatusLabel()->text().isEmpty());   // "not checked yet"

    SocFlasher::DeviceStatus adb;
    adb.mode = SocFlasher::DeviceStatus::Mode::Adb;
    adb.serial = QStringLiteral("0225ce3c8d8d6e44");
    emit tab->socFlasher()->statusChanged(adb);
    QVERIFY2(tab->socStatusLabel()->text().contains(adb.serial), qPrintable(tab->socStatusLabel()->text()));
    QVERIFY(tab->installDriversButton()->isHidden());

    // A Rockchip device no driver claims: the hint and the Install drivers button.
    SocFlasher::DeviceStatus noDriver;
    noDriver.mode = SocFlasher::DeviceStatus::Mode::NoDriver;
    noDriver.hint = QStringLiteral("USB\\VID_2207&PID_0006: no driver bound. Install the Rockchip USB drivers "
                                   "(DriverAssistant) and replug the board.");
    emit tab->socFlasher()->statusChanged(noDriver);
    QVERIFY2(tab->socStatusLabel()->text().contains(QStringLiteral("no driver bound")),
             qPrintable(tab->socStatusLabel()->text()));
    QVERIFY(!tab->installDriversButton()->isHidden());
    QVERIFY(tab->installDriversButton()->isEnabled());

    SocFlasher::DeviceStatus loader;
    loader.mode = SocFlasher::DeviceStatus::Mode::Loader;
    loader.detail = QStringLiteral("DevNo=1 Vid=0x2207 Pid=0x110e LocationID=104");
    emit tab->socFlasher()->statusChanged(loader);
    QVERIFY2(tab->socStatusLabel()->text().contains(QStringLiteral("Loader")), qPrintable(tab->socStatusLabel()->text()));
    QVERIFY(tab->installDriversButton()->isHidden());

    emit tab->socFlasher()->statusChanged(SocFlasher::DeviceStatus());
    QVERIFY2(tab->socStatusLabel()->text().contains(QStringLiteral("no board")), qPrintable(tab->socStatusLabel()->text()));

    // refreshStatus() asks the flasher (what the 2 s timer does while the tab is visible).
    tab->refreshStatus();
    QCoreApplication::processEvents();
    QVERIFY(!tab->isBusy());
}

void Tst_firmwaretab::installDriversWithoutInstaller()
{
    FakeToolRunner runner;
    auto tab = makeTab(&runner);   // setToolPaths() without a driver installer
    QSignalSpy status(tab.get(), &FirmwareTab::statusMessage);
    tab->installDrivers();
    QVERIFY(!status.isEmpty());
    QVERIFY2(status.last().at(0).toString().contains(QStringLiteral("missing")), qPrintable(status.last().at(0).toString()));
    QCOMPARE(runner.count(QRegularExpression(QStringLiteral("DriverInstall"))), 0);

    // A bundled installer is started through the ToolRunner, in its own folder.
    const QString installer = tempPath(QStringLiteral("DriverAssistant/DriverInstall.exe"));
    QVERIFY(writeFile(installer, "MZ"));
    runner.addScript(QRegularExpression(QStringLiteral("DriverInstall")), QByteArray(), 0);
    tab->setToolPaths(QStringLiteral("upgrade_tool"), QStringLiteral("adb"), installer);
    tab->installDrivers();
    QCOMPARE(runner.count(QRegularExpression(QStringLiteral("DriverInstall"))), 1);
    QCOMPARE(QDir::fromNativeSeparators(runner.lastOptions().workingDirectory), QFileInfo(installer).absolutePath());
    QTRY_VERIFY(tab->logView()->toPlainText().contains(QStringLiteral("exit code 0")));
}

void Tst_firmwaretab::mcuNeedsPackageAndRole()
{
    FakeToolRunner runner;
    auto tab = makeTab(&runner);
    QSignalSpy finished(tab.get(), &FirmwareTab::operationFinished);
    QSignalSpy busy(tab.get(), &FirmwareTab::busyChanged);

    tab->updateMcu();   // no package
    QCOMPARE(finished.count(), qsizetype(1));
    QCOMPARE(finished.at(0).at(0).toString(), QStringLiteral("mcu"));
    QCOMPARE(finished.at(0).at(1).toBool(), false);
    QVERIFY(finished.at(0).at(2).toString().contains(QStringLiteral("package")));

    tab->setMcuRole();   // no role: never guessed
    QCOMPARE(finished.count(), qsizetype(2));
    QCOMPARE(finished.at(1).at(0).toString(), QStringLiteral("set-role"));
    QCOMPARE(finished.at(1).at(1).toBool(), false);
    QVERIFY(finished.at(1).at(2).toString().contains(QStringLiteral("role")));
    QCOMPARE(busy.count(), qsizetype(4));   // each framed by busy on / off
    QVERIFY(!tab->isBusy());
    QCOMPARE(runner.count(QRegularExpression(QStringLiteral("rfmcu"))), 0);   // nothing reached the board
}

void Tst_firmwaretab::readMcuInfoFillsRole()
{
    // needs McuUpdater::parseCliFields() (protocol work package).
    FakeToolRunner runner;
    runner.addScript(QRegularExpression(QStringLiteral("rfmcu cmd version")), "OK version=0.8.1 role=tx\r\n", 0);
    auto tab = makeTab(&runner);
    QSignalSpy finished(tab.get(), &FirmwareTab::operationFinished);
    tab->readMcuButton()->click();
    QVERIFY(tab->isBusy());
    QCOMPARE(tab->currentOperation(), QStringLiteral("info"));
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), qsizetype(1), kOpTimeoutMs);
    QCOMPARE(finished.at(0).at(0).toString(), QStringLiteral("info"));
    QVERIFY2(finished.at(0).at(1).toBool(), qPrintable(finished.at(0).at(2).toString()));
    QCOMPARE(tab->runningMcuVersion(), QStringLiteral("0.8.1"));
    QCOMPARE(tab->runningMcuRole(), QStringLiteral("tx"));
    QCOMPARE(tab->selectedRole(), static_cast<quint32>(CcProtocol::RoleTx));   // pre-selected from the reply
    QVERIFY(tab->roleTxRadio()->isChecked());
    QVERIFY(tab->mcuStatusLabel()->text().contains(QStringLiteral("0.8.1")));
    QVERIFY(tab->setRoleButton()->isEnabled());
}

void Tst_firmwaretab::readMcuInfoPortBusy()
{
    FakeToolRunner runner;
    // rfmcu cmd exits 2 when another rfmcu holds the UART (rfmcu README).
    runner.addScript(QRegularExpression(QStringLiteral("rfmcu cmd version")),
                     "ERR PORT busy: /dev/ttyS1 is held by rfmcu console (pid 190)\r\n", 2);
    auto tab = makeTab(&runner);
    QSignalSpy finished(tab.get(), &FirmwareTab::operationFinished);
    tab->readMcuInfo();
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), qsizetype(1), kOpTimeoutMs);
    QCOMPARE(finished.at(0).at(0).toString(), QStringLiteral("info"));
    QCOMPARE(finished.at(0).at(1).toBool(), false);
    QVERIFY2(finished.at(0).at(2).toString().contains(QStringLiteral("busy")), qPrintable(finished.at(0).at(2).toString()));
    QVERIFY(tab->runningMcuVersion().isEmpty());
    QCOMPARE(tab->selectedRole(), 0u);
    QCOMPARE(runner.count(QRegularExpression(QStringLiteral("rfmcu cmd version"))), 1);
    QVERIFY(tab->logView()->toPlainText().contains(QStringLiteral("pid 190")));   // the tool's output is logged
}

void Tst_firmwaretab::pullWithoutTargetFails()
{
    FakeToolRunner runner;
    auto tab = makeTab(&runner);
    QSignalSpy finished(tab.get(), &FirmwareTab::operationFinished);
    QSignalSpy busy(tab.get(), &FirmwareTab::busyChanged);
    tab->pullButton()->click();
    QCOMPARE(finished.count(), qsizetype(1));
    QCOMPARE(finished.at(0).at(0).toString(), QStringLiteral("pull"));
    QCOMPARE(finished.at(0).at(1).toBool(), false);
    QVERIFY(finished.at(0).at(2).toString().contains(QStringLiteral("build machine")));
    QCOMPARE(busy.count(), qsizetype(2));
    QCOMPARE(tab->sshConnection()->state(), Transport::State::Disconnected);

    // Pull & Flash stops at the failed pull: one "pull" and the final "pull-flash".
    finished.clear();
    tab->pullAndFlash();
    QCOMPARE(finished.count(), qsizetype(2));
    QCOMPARE(finished.at(0).at(0).toString(), QStringLiteral("pull"));
    QCOMPARE(finished.at(1).at(0).toString(), QStringLiteral("pull-flash"));
    QCOMPARE(finished.at(1).at(1).toBool(), false);
    QVERIFY(runner.invocations().isEmpty());   // no flash, no adb: nothing ran
}

void Tst_firmwaretab::pullConnectsAndCloses()
{
    SshProfileStore store;
    BuildMachine machine;
    QVERIFY(machine.start(&store, tempPath(QStringLiteral("pull-connect"))));
    FakeToolRunner runner;
    auto tab = makeTab(&runner, &store);
    tab->setSourceTarget(QStringLiteral("ssh:profile:") + machine.profile.id);
    tab->setRemoteFolder(machine.imageFolder);
    tab->setCacheDirectory(tempPath(QStringLiteral("pull-connect-cache")));

    QSignalSpy finished(tab.get(), &FirmwareTab::operationFinished);
    QSignalSpy busy(tab.get(), &FirmwareTab::busyChanged);
    ModalDismisser unexpected(ModalDismisser::Answer::Reject);   // no host key / password question may appear
    tab->pull();
    QVERIFY(tab->isBusy());
    QCOMPARE(tab->currentOperation(), QStringLiteral("pull"));
    QVERIFY(tab->cancelButton()->isEnabled());   // a pull can always be cancelled
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), qsizetype(1), kOpTimeoutMs);
    unexpected.stop();
    QCOMPARE(unexpected.count(), 0);
    QCOMPARE(finished.at(0).at(0).toString(), QStringLiteral("pull"));
    QVERIFY(machine.server->connectionCount() >= 1);
    QCOMPARE(machine.server->lastAuthMethod(), QStringLiteral("publickey"));
    QVERIFY(tab->logView()->toPlainText().contains(QStringLiteral("Connected to test@127.0.0.1:%1").arg(machine.server->port())));
    // The connection is only held while pulling.
    QTRY_COMPARE(tab->sshConnection()->state(), Transport::State::Disconnected);
    QVERIFY(!tab->isBusy());
    QCOMPARE(busy.count(), qsizetype(2));
    QCOMPARE(AppSettings::instance().firmwareRemoteFolder(QStringLiteral("ssh:profile:") + machine.profile.id),
             machine.imageFolder);
}

void Tst_firmwaretab::pullDownloadsImage()
{
    // needs ImagePuller (ssh-pull work package): stat, download to .part, RKFW check, the cache note.
    SshProfileStore store;
    BuildMachine machine;
    QVERIFY(machine.start(&store, tempPath(QStringLiteral("pull-download"))));
    FakeToolRunner runner;
    auto tab = makeTab(&runner, &store);
    const QString cache = tempPath(QStringLiteral("pull-download-cache"));
    tab->setSourceTarget(QStringLiteral("ssh:profile:") + machine.profile.id);
    tab->setRemoteFolder(machine.imageFolder);
    tab->setCacheDirectory(cache);

    QSignalSpy finished(tab.get(), &FirmwareTab::operationFinished);
    tab->pull();
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), qsizetype(1), kOpTimeoutMs);
    QVERIFY2(finished.at(0).at(1).toBool(), qPrintable(finished.at(0).at(2).toString()));
    const QString pulled = cache + QStringLiteral("/update.img");
    QCOMPARE(QFileInfo(pulled).size(), qint64(machine.image.size()));
    QCOMPARE(tab->imagePath(), pulled);
    QVERIFY(tab->flashSocButton()->isEnabled());

    // Unchanged on the build machine: the second pull skips the transfer.
    finished.clear();
    tab->pull();
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), qsizetype(1), kOpTimeoutMs);
    QVERIFY(finished.at(0).at(1).toBool());
    QVERIFY(tab->logView()->toPlainText().contains(QStringLiteral("unchanged")));
}

void Tst_firmwaretab::mcuUpdateAndRoleThroughFakeBridge()
{
    // needs McuLink, McuUpdater and CcPackage (protocol and tools work packages): the whole MCU
    // path of the tab - adb, the bridge, the fw_update protocol - against FakeMcu.
    FakeMcu mcu;   // the application CLI first, role rx
    FakeMcuServer bridge(&mcu);
    QVERIFY(bridge.listen());
    FakeToolRunner runner;
    scriptMcuBridge(runner, bridge.port());
    auto tab = makeTab(&runner);
    QVERIFY(tab->setPackagePath(dataFile("firmware-0.8.1.pkg")));
    tab->setSelectedRole(CcProtocol::RoleRx);
    QVERIFY(tab->updateMcuButton()->isEnabled());

    QSignalSpy finished(tab.get(), &FirmwareTab::operationFinished);
    tab->updateMcuButton()->click();
    QVERIFY(tab->isBusy());
    QCOMPARE(tab->currentOperation(), QStringLiteral("mcu"));
    QVERIFY(tab->cancelButton()->isEnabled());
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), qsizetype(1), 60000);
    QCOMPARE(finished.at(0).at(0).toString(), QStringLiteral("mcu"));
    QVERIFY2(finished.at(0).at(1).toBool(), qPrintable(finished.at(0).at(2).toString()));
    QCOMPARE(mcu.image(), tab->package().image);   // every byte arrived, in order
    QCOMPARE(mcu.role(), static_cast<quint32>(CcProtocol::RoleRx));
    // fw_update.py's order through the SoC: "power" is asked (and logged) before "bootloader".
    const QStringList commands = mcu.commands();
    QVERIFY(commands.contains(QStringLiteral("power")));
    QVERIFY(commands.indexOf(QStringLiteral("power")) < commands.indexOf(QStringLiteral("bootloader")));
    QCOMPARE(tab->progressBar()->value(), 100);
    QVERIFY(tab->logView()->toPlainText().contains(QStringLiteral("COMPLETE")));
    QCOMPARE(tab->runningMcuVersion(), QStringLiteral("0.8.1"));   // from the restart banner
    // The link was taken down again: the board's rfmcu stopped and the forward removed.
    QTRY_VERIFY(!tab->mcuLink()->isOpen());
    QVERIFY(runner.count(QRegularExpression(QStringLiteral("forward --remove"))) >= 1);
    QVERIFY(!tab->isBusy());

    // Set role: the same road, SET_ROLE instead of the transfer.
    finished.clear();
    tab->roleTxRadio()->click();
    tab->setRoleButton()->click();
    QCOMPARE(tab->currentOperation(), QStringLiteral("set-role"));
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), qsizetype(1), 60000);
    QCOMPARE(finished.at(0).at(0).toString(), QStringLiteral("set-role"));
    QVERIFY2(finished.at(0).at(1).toBool(), qPrintable(finished.at(0).at(2).toString()));
    QCOMPARE(mcu.role(), static_cast<quint32>(CcProtocol::RoleTx));
    QCOMPARE(tab->runningMcuRole(), QStringLiteral("tx"));
}

void Tst_firmwaretab::pullAndFlashEndToEnd()
{
    // needs ImagePuller, SocFlasher and McuLink: pull -> flash SoC -> wait for ADB -> read the MCU,
    // which already runs the package's version, so the update is skipped.
    SshProfileStore store;
    BuildMachine machine;
    QVERIFY(machine.start(&store, tempPath(QStringLiteral("pull-flash"))));
    FakeToolRunner runner;
    runner.addScript(QRegularExpression(QStringLiteral("^upgrade_tool LD$")), kLdLoader);
    runner.addHandler(QRegularExpression(QStringLiteral("^upgrade_tool UF ")), [](FakeToolInvocation* i) {
        i->emitOutput("Loading firmware...\r\nDownload Boot Start\r\nDownload Boot Success\r\n");
        i->emitOutput("Download Image... (1%)\rDownload Image... (42%)\rDownload Image... (100%)\r");
        i->emitOutput("\nReset Device Success\r\nUpgrade firmware ok.\r\n");
        i->finish(0);
    });
    runner.addScript(QRegularExpression(QStringLiteral("^upgrade_tool SFI ")), "Type:Update Firmware\r\nChip Tag:110E\r\n");
    runner.addScript(QRegularExpression(QStringLiteral("^adb( -s \\S+)? get-state$")), "device\n");
    runner.addScript(QRegularExpression(QStringLiteral("cat /proc/version")), kProcVersion);
    runner.addScript(QRegularExpression(QStringLiteral("^adb( -s \\S+)? devices$")), kAdbBoard);
    runner.addScript(QRegularExpression(QStringLiteral("rfmcu cmd version")), "OK version=0.8.1 role=rx\n");
    auto tab = makeTab(&runner, &store);
    tab->socFlasher()->setPollIntervalMs(10);
    tab->setSourceTarget(QStringLiteral("ssh:profile:") + machine.profile.id);
    tab->setRemoteFolder(machine.imageFolder);
    tab->setCacheDirectory(tempPath(QStringLiteral("pull-flash-cache")));
    QVERIFY(tab->setPackagePath(dataFile("firmware-0.8.1.pkg")));

    QSignalSpy finished(tab.get(), &FirmwareTab::operationFinished);
    QSignalSpy busy(tab.get(), &FirmwareTab::busyChanged);
    ModalDismisser unexpected(ModalDismisser::Answer::Reject);
    tab->pullAndFlashButton()->click();
    QCOMPARE(tab->currentOperation(), QStringLiteral("pull-flash"));
    QTRY_VERIFY_WITH_TIMEOUT(!finished.isEmpty() && finished.last().at(0).toString() == QLatin1String("pull-flash"),
                             60000);
    unexpected.stop();
    QCOMPARE(unexpected.count(), 0);
    QStringList steps;
    for (const QList<QVariant>& args : std::as_const(finished)) {
        QVERIFY2(args.at(1).toBool(), qPrintable(args.at(0).toString() + QStringLiteral(": ") + args.at(2).toString()));
        steps << args.at(0).toString();
    }
    QCOMPARE(steps, (QStringList{QStringLiteral("pull"), QStringLiteral("flash"), QStringLiteral("mcu"),
                                 QStringLiteral("pull-flash")}));
    QVERIFY(finished.at(2).at(2).toString().contains(QStringLiteral("0.8.1")));   // already running: skipped
    QCOMPARE(busy.count(), qsizetype(2));   // one busy period for the whole chain
    QCOMPARE(runner.count(QRegularExpression(QStringLiteral("^upgrade_tool UF "))), 1);
    QCOMPARE(runner.count(QRegularExpression(QStringLiteral("forward tcp:0"))), 0);   // no MCU transfer
    // The flashed image is the one just pulled; the build-machine connection is closed at the end.
    QVERIFY(runner.invocations().filter(QStringLiteral("UF ")).first().contains(QStringLiteral("update.img")));
    QTRY_COMPARE(tab->sshConnection()->state(), Transport::State::Disconnected);
    // upgrade_tool's repeated progress lines collapse into one log line per stage.
    const QString log = tab->logView()->toPlainText();
    QCOMPARE(log.count(QStringLiteral("Download Image...")), 1);
    QVERIFY(log.contains(QStringLiteral("(100%)")));
    QVERIFY(log.contains(QStringLiteral("Linux version 5.10.209+")));   // the board information
    QVERIFY(!tab->isBusy());
}

void Tst_firmwaretab::languageChangeRetranslates()
{
    FakeToolRunner runner;
    auto tab = makeTab(&runner);
    QSignalSpy titles(tab.get(), &FirmwareTab::titleChanged);
    MarkingTranslator translator;
    QVERIFY(qApp->installTranslator(&translator));
    auto removeTranslator = qScopeGuard([&translator]() { qApp->removeTranslator(&translator); });

    QTRY_COMPARE(tab->pullButton()->text(), QStringLiteral("[zz] Pull"));
    QCOMPARE(tab->title(), QStringLiteral("[zz] Firmware"));
    QVERIFY(!titles.isEmpty());
    QCOMPARE(titles.last().at(0).toString(), QStringLiteral("[zz] Firmware"));
    QCOMPARE(tab->flashSocButton()->text(), QStringLiteral("[zz] Flash SoC"));
    QCOMPARE(tab->readMcuButton()->text(), QStringLiteral("[zz] Read"));
    QCOMPARE(tab->saveLogButton()->text(), QStringLiteral("[zz] Save..."));
    QCOMPARE(child<QGroupBox>(tab.get(), "sourceGroup")->title(), QStringLiteral("[zz] Source"));
    QVERIFY(tab->packageInfoLabel()->text().startsWith(QStringLiteral("[zz] ")));   // dynamic texts too

    removeTranslator.dismiss();
    qApp->removeTranslator(&translator);
    QTRY_COMPARE(tab->pullButton()->text(), QStringLiteral("Pull"));
    QCOMPARE(tab->title(), QStringLiteral("Firmware"));
    QCOMPARE(child<QGroupBox>(tab.get(), "sourceGroup")->title(), QStringLiteral("Source"));
}

// =======================================================================================
// MainWindow integration
// =======================================================================================

void Tst_firmwaretab::showFirmwareTabOnce()
{
    MainWindow w;
    QVERIFY(showAndActivate(w));
    QCOMPARE(w.sessionCount(), 1);
    QVERIFY(w.firmwareTab() == nullptr);
    SessionWidget* serial = w.currentSession();
    QVERIFY(serial);

    FirmwareTab* tab = openFirmwareTab(w);
    QVERIFY(tab);
    QCOMPARE(w.firmwareTab(), tab);
    QCOMPARE(w.sessionCount(), 2);
    QCOMPARE(tabs(w)->currentWidget(), static_cast<QWidget*>(tab));
    QVERIFY(w.currentSession() == nullptr);
    QVERIFY(w.sessionAt(1) == nullptr);
    QCOMPARE(w.sessionAt(0), serial);
    QCOMPARE(tabs(w)->tabText(1), QStringLiteral("Firmware"));
    QVERIFY(!tabs(w)->tabIcon(1).isNull());
    QVERIFY(!tabs(w)->tabToolTip(1).isEmpty());
    QVERIFY2(w.windowTitle().endsWith(QStringLiteral(" - Firmware")), qPrintable(w.windowTitle()));
    QVERIFY(w.connectedSessions().isEmpty());

    // One instance: the slot, the action and its shortcut all select the same tab.
    tabs(w)->setCurrentIndex(0);
    QCOMPARE(w.showFirmwareTab(), tab);
    QCOMPARE(w.sessionCount(), 2);
    QCOMPARE(tabs(w)->currentWidget(), static_cast<QWidget*>(tab));
    tabs(w)->setCurrentIndex(0);
    action(w, "actionFirmwareUpdate")->trigger();
    QCOMPARE(tabs(w)->currentWidget(), static_cast<QWidget*>(tab));
    tabs(w)->setCurrentIndex(0);
    QTest::keyClick(&w, Qt::Key_U, Qt::ControlModifier | Qt::ShiftModifier);
    QTRY_COMPARE(tabs(w)->currentWidget(), static_cast<QWidget*>(tab));
    QCOMPARE(w.sessionCount(), 2);
    QCOMPARE(w.firmwareTab(), tab);
}

void Tst_firmwaretab::sessionActionsDisabledWhileCurrent()
{
    MainWindow w;
    QVERIFY(showAndActivate(w));
    SessionWidget* session = w.currentSession();
    session->setPortName(kLoopback);
    action(w, "actionConnect")->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(session->isConnected(), kOpTimeoutMs);
    QVERIFY(action(w, "actionDisconnect")->isEnabled());
    QVERIFY(action(w, "actionCopy")->isEnabled());
    QCOMPARE(w.connectedSessions(), QList<SessionWidget*>{session});

    FirmwareTab* tab = openFirmwareTab(w);
    QVERIFY(tab);
    const char* sessionActions[] = {"actionConnect",       "actionDisconnect",   "actionClear",
                                    "actionResetTerminal", "actionSendFile",     "actionUploadFile",
                                    "actionDownloadFile",  "actionSendBreak",    "actionSyncTerminalSize",
                                    "actionDetectBaudRate", "actionRefreshPorts", "actionStartLogging",
                                    "actionStopLogging",   "actionReplayLog",    "actionStopReplay",
                                    "actionCopy",          "actionPaste",        "actionSelectAll",
                                    "actionFind",          "actionHexView",      "actionZoomIn",
                                    "actionZoomOut",       "actionZoomReset"};
    for (const char* name : sessionActions) {
        QAction* a = action(w, name);
        QVERIFY2(a, name);
        QVERIFY2(!a->isEnabled(), name);
    }
    for (const char* name : {"actionCloseSession", "actionNewSession", "actionNewSshSession", "actionFirmwareUpdate",
                             "actionPreferences", "actionNextTab", "actionPreviousTab", "actionQuit"}) {
        QVERIFY2(action(w, name)->isEnabled(), name);
    }
    // The status bar shows "no session", not the serial tab's state.
    QCOMPARE(child<QLabel>(&w, "statusConnectionLabel")->text(), QStringLiteral("No session"));
    // A Ctrl+Shift+L for Clear does nothing while the Firmware tab is current.
    QTest::keyClick(&w, Qt::Key_L, Qt::ControlModifier | Qt::ShiftModifier);
    QVERIFY(session->isConnected());

    // Back on the session tab everything is as before.
    tabs(w)->setCurrentIndex(0);
    QCOMPARE(w.currentSession(), session);
    QVERIFY(action(w, "actionDisconnect")->isEnabled());
    QVERIFY(action(w, "actionCopy")->isEnabled());
    QVERIFY(action(w, "actionClear")->isEnabled());
    QVERIFY(child<QLabel>(&w, "statusConnectionLabel")->text().contains(kLoopback));
    action(w, "actionDisconnect")->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(!session->isConnected(), kOpTimeoutMs);
}

void Tst_firmwaretab::windowMenuAndAltN()
{
    MainWindow w;
    QVERIFY(showAndActivate(w));
    w.newSession(kLoopback);
    FirmwareTab* tab = openFirmwareTab(w);
    QCOMPARE(w.sessionCount(), 3);
    QCOMPARE(tabs(w)->indexOf(tab), 2);

    auto* menu = child<QMenu>(&w, "menuWindow");
    QList<QAction*> entries = windowEntries(menu);
    QCOMPARE(entries.size(), qsizetype(3));
    QCOMPARE(entries.at(2)->text(), QStringLiteral("Firmware"));
    QVERIFY(entries.at(2)->isChecked());
    QVERIFY(action(w, "actionSelectTab3")->isEnabled());

    // The entry, Alt+3 and Next Tab reach it like any tab.
    tabs(w)->setCurrentIndex(0);
    windowEntries(menu).at(2)->trigger();
    QCOMPARE(tabs(w)->currentWidget(), static_cast<QWidget*>(tab));
    tabs(w)->setCurrentIndex(0);
    QTest::keyClick(&w, Qt::Key_3, Qt::AltModifier);
    QTRY_COMPARE(tabs(w)->currentWidget(), static_cast<QWidget*>(tab));
    tabs(w)->setCurrentIndex(1);
    action(w, "actionNextTab")->trigger();
    QCOMPARE(tabs(w)->currentWidget(), static_cast<QWidget*>(tab));
    action(w, "actionNextTab")->trigger();
    QCOMPARE(tabs(w)->currentIndex(), 0);
    // Moving the tab keeps the menu in tab order.
    tabs(w)->tabBar()->moveTab(2, 0);
    entries = windowEntries(menu);
    QCOMPARE(entries.at(0)->text(), QStringLiteral("Firmware"));
}

void Tst_firmwaretab::closeTabWhenIdle()
{
    MainWindow w;
    QVERIFY(showAndActivate(w));
    QPointer<FirmwareTab> tab = openFirmwareTab(w);
    QVERIFY(tab);
    QVERIFY(action(w, "actionCloseSession")->isEnabled());

    // Close Session on the idle tab: no question, the tab goes, the session tab is current again.
    ModalDismisser dismisser(ModalDismisser::Answer::Reject);
    action(w, "actionCloseSession")->trigger();
    dismisser.stop();
    QCOMPARE(dismisser.count(), 0);
    QCOMPARE(w.sessionCount(), 1);
    QVERIFY(w.firmwareTab() == nullptr);
    QVERIFY(w.currentSession() != nullptr);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVERIFY(tab.isNull());
    QCOMPARE(windowEntries(child<QMenu>(&w, "menuWindow")).size(), qsizetype(1));
    QVERIFY(!w.windowTitle().endsWith(QStringLiteral(" - Firmware")));

    // A middle click and the tab's close button close it as well; a new one is created next time.
    tab = openFirmwareTab(w);
    QTabBar* bar = tabs(w)->tabBar();
    QTest::mouseClick(bar, Qt::MiddleButton, Qt::NoModifier, bar->tabRect(1).center());
    QTRY_COMPARE(w.sessionCount(), 1);
    QVERIFY(w.firmwareTab() == nullptr);
    tab = openFirmwareTab(w);
    emit tabs(w)->tabCloseRequested(tabs(w)->indexOf(tab));
    QCOMPARE(w.sessionCount(), 1);
    QVERIFY(w.firmwareTab() == nullptr);

    // The last session tab closed with the Firmware tab still open: no empty session is forced in.
    tab = openFirmwareTab(w);
    w.closeSession(0);
    QCOMPARE(w.sessionCount(), 1);
    QCOMPARE(w.firmwareTab(), tab.data());
    QVERIFY(w.currentSession() == nullptr);
    QVERIFY(!action(w, "actionConnect")->isEnabled());
}

void Tst_firmwaretab::closeRefusedWhileBusyAndQuitAsks()
{
    // A "build machine" that accepts TCP and never speaks SSH: the pull stays in "connecting".
    QTcpServer silent;
    QVERIFY(silent.listen(QHostAddress::LocalHost));
    QList<QTcpSocket*> held;
    connect(&silent, &QTcpServer::newConnection, &silent, [&silent, &held]() {
        while (QTcpSocket* socket = silent.nextPendingConnection()) {
            held.append(socket);
        }
    });

    MainWindow w;
    QVERIFY(showAndActivate(w));
    FirmwareTab* tab = openFirmwareTab(w);
    const int index = tabs(w)->indexOf(tab);
    tab->setSourceTarget(QStringLiteral("test@127.0.0.1:%1").arg(silent.serverPort()));
    QSignalSpy finished(tab, &FirmwareTab::operationFinished);
    tab->pull();
    QVERIFY(tab->isBusy());
    QCOMPARE(tab->currentOperation(), QStringLiteral("pull"));
    QTRY_VERIFY_WITH_TIMEOUT(!held.isEmpty(), kOpTimeoutMs);

    // The tab shows it is busy and its lasting message is in the status bar.
    QVERIFY(tabs(w)->tabToolTip(index).contains(QStringLiteral("connecting")));
    QVERIFY2(w.statusBar()->currentMessage().contains(QStringLiteral("connecting to test@127.0.0.1")),
             qPrintable(w.statusBar()->currentMessage()));

    // Closing is refused with a status message, whichever way it is asked for.
    ModalDismisser none(ModalDismisser::Answer::Reject);
    w.closeSession(index);
    action(w, "actionCloseSession")->trigger();
    QTabBar* bar = tabs(w)->tabBar();
    QTest::mouseClick(bar, Qt::MiddleButton, Qt::NoModifier, bar->tabRect(index).center());
    none.stop();
    QCOMPARE(none.count(), 0);
    QCOMPARE(w.firmwareTab(), tab);
    QCOMPARE(w.sessionCount(), 2);
    QVERIFY2(w.statusBar()->currentMessage().contains(QStringLiteral("busy")), qPrintable(w.statusBar()->currentMessage()));

    // Quitting asks once; No keeps the window and the operation.
    {
        ModalDismisser no(ModalDismisser::Answer::No);
        w.close();
        no.stop();
        QCOMPARE(no.count(), 1);
        QVERIFY(no.windowTitles().first().startsWith(QStringLiteral("Quit")));
    }
    QVERIFY(w.isVisible());
    QVERIFY(tab->isBusy());

    // Cancel ends the pull; then the tab closes normally.
    QVERIFY(tab->cancelButton()->isEnabled());
    tab->cancelButton()->click();
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), qsizetype(1), kOpTimeoutMs);
    QCOMPARE(finished.at(0).at(0).toString(), QStringLiteral("pull"));
    QCOMPARE(finished.at(0).at(1).toBool(), false);
    QCOMPARE(finished.at(0).at(2).toString(), QStringLiteral("cancelled"));
    QVERIFY(!tab->isBusy());
    QCOMPARE(tab->sshConnection()->state(), Transport::State::Disconnected);
    w.closeSession(index);
    QVERIFY(w.firmwareTab() == nullptr);
    QCOMPARE(w.sessionCount(), 1);
    qDeleteAll(held);
}

void Tst_firmwaretab::restoreKeysExcludeFirmwareTab()
{
    {
        MainWindow w;
        QVERIFY(showAndActivate(w));
        w.currentSession()->setPortName(kLoopback);
        w.newSession(kMcu);
        QVERIFY(openFirmwareTab(w));   // current while the window closes
        action(w, "actionQuit")->trigger();
        QTRY_VERIFY(!w.isVisible());
    }
    QCOMPARE(AppSettings::instance().lastOpenPorts(), (QStringList{kLoopback, kMcu}));
    QVERIFY(AppSettings::instance().lastPortName().isEmpty());   // nothing recorded for the Firmware tab

    MainWindow again;
    QCOMPARE(again.sessionCount(), 2);
    QVERIFY(again.firmwareTab() == nullptr);
    for (int i = 0; i < again.sessionCount(); ++i) {
        QVERIFY(again.sessionAt(i) != nullptr);
    }
}

void Tst_firmwaretab::shortcutListedAndReserved()
{
    MainWindow w;
    const QList<ShortcutEntry> entries = w.shortcutEntries();
    qsizetype firmware = -1;
    qsizetype newSsh = -1;
    qsizetype close = -1;
    for (qsizetype i = 0; i < entries.size(); ++i) {
        if (entries.at(i).objectName == QLatin1String("actionFirmwareUpdate")) {
            firmware = i;
        } else if (entries.at(i).objectName == QLatin1String("actionNewSshSession")) {
            newSsh = i;
        } else if (entries.at(i).objectName == QLatin1String("actionCloseSession")) {
            close = i;
        }
    }
    QVERIFY(firmware >= 0);
    QCOMPARE(entries.at(firmware).title, QStringLiteral("File > Firmware Update"));
    QCOMPARE(entries.at(firmware).defaultSequence, seq("Ctrl+Shift+U"));
    QVERIFY(newSsh < firmware && firmware < close);   // menu order

    // Reserved from a connected terminal: Ctrl+Shift+U brings the tab up, nothing reaches the
    // device. (The tab is created first through openFirmwareTab(): a tab created by the key
    // itself would carry the real tools of the build folder for a moment.)
    QVERIFY(showAndActivate(w));
    SessionWidget* session = w.currentSession();
    FirmwareTab* tab = openFirmwareTab(w);
    QVERIFY(tab);
    tabs(w)->setCurrentIndex(0);
    session->setPortName(kLoopback);
    action(w, "actionConnect")->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(session->isConnected(), kOpTimeoutMs);
    QVERIFY(session->terminal()->reservedShortcuts().contains(seq("Ctrl+Shift+U")));
    session->focusTerminal();
    QTRY_COMPARE(QApplication::focusWidget(), static_cast<QWidget*>(session->terminal()));
    QSignalSpy sent(session->transport(), &Transport::dataSent);
    QTest::keyClick(session->terminal(), Qt::Key_U, Qt::ControlModifier | Qt::ShiftModifier);
    QTRY_COMPARE(tabs(w)->currentWidget(), static_cast<QWidget*>(tab));
    QCOMPARE(w.firmwareTab(), tab);
    QCOMPARE(sent.count(), qsizetype(0));
    // A configured shortcut replaces the default live.
    AppSettings::instance().setShortcut(QStringLiteral("actionFirmwareUpdate"), seq("Ctrl+Shift+Y"));
    QCOMPARE(action(w, "actionFirmwareUpdate")->shortcut(), seq("Ctrl+Shift+Y"));
    QVERIFY(session->terminal()->reservedShortcuts().contains(seq("Ctrl+Shift+Y")));
    tabs(w)->setCurrentIndex(0);
    action(w, "actionDisconnect")->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(!session->isConnected(), kOpTimeoutMs);
}

void Tst_firmwaretab::statusBarShowsTabMessages()
{
    MainWindow w;
    QVERIFY(showAndActivate(w));
    FirmwareTab* tab = openFirmwareTab(w);
    const QString bad = tempPath(QStringLiteral("status.pkg"));
    QVERIFY(writeFile(bad, QByteArray(40, '\x01')));

    // The current Firmware tab's messages reach the status bar ...
    tab->setPackagePath(bad);
    QVERIFY2(w.statusBar()->currentMessage().contains(QStringLiteral("status.pkg")), qPrintable(w.statusBar()->currentMessage()));
    // ... a background one's do not.
    tabs(w)->setCurrentIndex(0);
    w.statusBar()->clearMessage();
    tab->setPackagePath(bad);
    QVERIFY(!w.statusBar()->currentMessage().contains(QStringLiteral("status.pkg")));
}

// =======================================================================================
// Real board + real build machine (docs/firmware-update/REQUIREMENTS.md "Ask me before", item 3:
// the first real flash is the integration test). Runs only with SU_FIRMWARE_REAL_RUN=1 and:
//   SU_FIRMWARE_REAL_TARGET        user@host[:port] of the Linux build machine
//   SU_FIRMWARE_REAL_PASSWORD      its SSH password (answered into the prompt, never logged)
//   SU_FIRMWARE_REAL_REMOTE_FOLDER remote image folder (default ~/rk/rockchip-platforms/output/image)
//   SU_FIRMWARE_REAL_PACKAGE       a CC1310 .pkg to install (Update MCU runs once explicitly)
//   SU_FIRMWARE_REAL_UPGRADE_TOOL, SU_FIRMWARE_REAL_ADB   tool paths (adb may come from PATH)
//   SU_FIRMWARE_REAL_LOG           file that receives the tab's whole log pane at the end
// What it does, in this order: Read (MCU version / role), Pull & Flash (pull update.img, flash the
// SoC with upgrade_tool UF, wait for ADB, update the MCU only when the package version differs),
// then Update MCU with the package explicitly. The host key is accepted into the test's own
// known_hosts file; nothing is remembered in the user's settings.
// =======================================================================================
namespace {

/// Answers the SSH dialogs of the real run from a timer: host key -> "Connect and remember"
/// (into the test's known_hosts file), password prompt -> the env password, Ok; any
/// QMessageBox -> Yes.
class RealRunAnswerer : public QObject
{
public:
    explicit RealRunAnswerer(QString password, QObject* parent = nullptr)
        : QObject(parent), m_password(std::move(password))
    {
        m_timer.setInterval(50);
        connect(&m_timer, &QTimer::timeout, this, &RealRunAnswerer::poll);
        m_timer.start();
    }
    void stop() { m_timer.stop(); }
    QStringList seen() const { return m_seen; }

private:
    void poll()
    {
        QWidget* modal = QApplication::activeModalWidget();
        if (!modal || !modal->isVisible() || m_busy.contains(modal)) {
            return;
        }
        m_busy.insert(modal);
        if (auto* hostKey = qobject_cast<HostKeyDialog*>(modal)) {
            auto* host = child<QLabel>(hostKey, "labelHost");
            m_seen << QStringLiteral("host key: %1").arg(host ? host->text() : QString());
            if (auto* button = child<QPushButton>(hostKey, "buttonRemember")) {
                button->click();
            } else {
                hostKey->accept();
            }
        } else if (auto* auth = qobject_cast<AuthPromptDialog*>(modal)) {
            m_seen << QStringLiteral("auth prompt");
            auto* edit = child<QLineEdit>(auth, "editResponse");
            auto* buttons = child<QDialogButtonBox>(auth, "buttonBox");
            if (edit) {
                edit->setText(m_password);
            }
            if (buttons && buttons->button(QDialogButtonBox::Ok)) {
                buttons->button(QDialogButtonBox::Ok)->click();
            } else {
                auth->accept();
            }
        } else if (auto* box = qobject_cast<QMessageBox*>(modal)) {
            m_seen << QStringLiteral("message box: %1").arg(box->text());
            if (QAbstractButton* yes = box->button(QMessageBox::Yes)) {
                yes->click();
            } else if (QAbstractButton* ok = box->button(QMessageBox::Ok)) {
                ok->click();
            } else {
                box->accept();
            }
        } else {
            m_seen << QStringLiteral("other modal: %1").arg(modal->windowTitle());
            modal->close();
        }
    }

    QString m_password;
    QTimer m_timer;
    QSet<QWidget*> m_busy;
    QStringList m_seen;
};

} // namespace

void Tst_firmwaretab::realBoard_pullFlashUpdate()
{
    if (qEnvironmentVariable("SU_FIRMWARE_REAL_RUN") != QLatin1String("1")) {
        QSKIP("set SU_FIRMWARE_REAL_RUN=1 (+ SU_FIRMWARE_REAL_TARGET/PASSWORD/PACKAGE/UPGRADE_TOOL/ADB/LOG) to flash the attached board");
    }
    const QString target = qEnvironmentVariable("SU_FIRMWARE_REAL_TARGET");
    const QString password = qEnvironmentVariable("SU_FIRMWARE_REAL_PASSWORD");
    const QString remoteFolder = qEnvironmentVariable("SU_FIRMWARE_REAL_REMOTE_FOLDER", kDefaultRemoteFolder);
    const QString package = qEnvironmentVariable("SU_FIRMWARE_REAL_PACKAGE");
    const QString upgradeTool = qEnvironmentVariable("SU_FIRMWARE_REAL_UPGRADE_TOOL");
    const QString adb = ToolLocator::locate(ToolLocator::Tool::Adb, qEnvironmentVariable("SU_FIRMWARE_REAL_ADB"));
    const QString logFile = qEnvironmentVariable("SU_FIRMWARE_REAL_LOG");
    QVERIFY2(!target.isEmpty() && !upgradeTool.isEmpty() && !adb.isEmpty(), "target / tool paths missing");
    AppSettings::instance().setSshKnownHostsFile(tempPath(QStringLiteral("real-run-known_hosts")));
    AppSettings::instance().setFirmwareGitDescribe(true);

    auto* runner = new ProcessToolRunner(this);
    auto tab = std::make_unique<FirmwareTab>(nullptr, runner);
    tab->setStatusRefreshIntervalMs(0);
    tab->setToolPaths(upgradeTool, adb);
    tab->setSourceTarget(target);
    tab->setRemoteFolder(remoteFolder);
    tab->setCacheDirectory(tempPath(QStringLiteral("real-run-cache")));
    tab->show();
    QVERIFY(QTest::qWaitForWindowExposed(tab.get()));

    const auto dumpLog = [&] {
        if (logFile.isEmpty()) {
            return;
        }
        QFile file(logFile);
        if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream out(&file);
            out << tab->logView()->toPlainText() << '\n';
        }
    };
    const auto guard = qScopeGuard(dumpLog);
    RealRunAnswerer answerer(password);
    QSignalSpy finished(tab.get(), &FirmwareTab::operationFinished);

    // 1. Device status and the running MCU version / role (fills the role radios).
    tab->refreshStatus();
    QTRY_VERIFY_WITH_TIMEOUT(!tab->socStatusLabel()->text().isEmpty()
                             && tab->socStatusLabel()->text() != QStringLiteral("not checked yet"), 30000);
    qInfo() << "real run: SoC status" << tab->socStatusLabel()->text();
    tab->readMcuInfo();
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 60000);
    qInfo() << "real run: read MCU ->" << finished.last().at(0).toString() << finished.last().at(1).toBool()
            << finished.last().at(2).toString() << "running" << tab->runningMcuVersion() << tab->runningMcuRole()
            << "role chosen" << tab->selectedRole();
    QVERIFY2(finished.last().at(1).toBool(), qPrintable(finished.last().at(2).toString()));
    QVERIFY(tab->selectedRole() != 0);
    if (!package.isEmpty()) {
        QVERIFY2(tab->setPackagePath(package), qPrintable(lastLogLine(*tab)));
        qInfo() << "real run: package" << tab->package().summary();
    }

    // 2. Pull & Flash: pull update.img from the build machine, flash the SoC, wait for ADB (the MCU
    //    step runs only when the package version differs from the running one).
    const qsizetype before = finished.count();
    tab->pullAndFlash();
    QVERIFY(tab->isBusy());
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() > before
                                 && finished.last().at(0).toString() == QStringLiteral("pull-flash"),
                             15 * 60 * 1000);
    for (qsizetype i = before; i < finished.count(); ++i) {
        qInfo() << "real run: step" << finished.at(i).at(0).toString() << finished.at(i).at(1).toBool()
                << finished.at(i).at(2).toString();
    }
    qInfo() << "real run: dialogs" << answerer.seen();
    QVERIFY2(finished.last().at(1).toBool(), qPrintable(finished.last().at(2).toString()));
    QVERIFY(!tab->isBusy());

    // 3. Update MCU once explicitly with the package (the full CC1310 transfer and restart).
    if (!package.isEmpty()) {
        const qsizetype beforeMcu = finished.count();
        tab->updateMcu();
        QVERIFY(tab->isBusy());
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() > beforeMcu, 5 * 60 * 1000);
        qInfo() << "real run: update MCU ->" << finished.last().at(0).toString() << finished.last().at(1).toBool()
                << finished.last().at(2).toString();
        QVERIFY2(finished.last().at(1).toBool(), qPrintable(finished.last().at(2).toString()));
    }
    answerer.stop();
    dumpLog();
}

QTEST_MAIN(Tst_firmwaretab)
#include "tst_firmwaretab.moc"
