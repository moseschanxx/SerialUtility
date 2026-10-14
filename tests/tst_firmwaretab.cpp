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
#include <QTableWidget>
#include <QTabWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QTranslator>

#include <algorithm>
#include <memory>

#include "app/AppSettings.h"
#include "core/CommandHistory.h"
#include "core/QuickCommand.h"
#include "core/Transport.h"
#include "firmware/CcProtocol.h"
#include "firmware/FirmwareJob.h"
#include "firmware/McuLink.h"
#include "firmware/SocFlasher.h"
#include "ssh/SshConnection.h"
#include "ssh/SshProfile.h"
#include "support/FakeMcu.h"
#include "support/FakeToolRunner.h"
#include "support/FirmwareFixtures.h"
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

// The Node board's facts, the scripted tools and the build machine (shared with tst_firmwarejob).
using namespace FirmwareFixtures;

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
    QStringList texts() const { return m_texts; }   ///< QMessageBox::text() of every box dismissed

private:
    void poll()
    {
        QWidget* modal = QApplication::activeModalWidget();
        if (!modal || !modal->isVisible()) {
            return;
        }
        ++m_count;
        m_windowTitles.append(modal->windowTitle());
        if (auto* box = qobject_cast<QMessageBox*>(modal)) {
            m_texts.append(box->text());
        }
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
    QStringList m_texts;
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

/// A stand-in upgrade_tool for a tab that MainWindow created (its runner starts real processes):
/// a script in `dir` whose `LD` lists one board in MASKROM mode (written with UF), `SFI` describes
/// an image and `UF` prints a progress line, then waits until a file "release" appears next to it,
/// prints the success line and ends. Killing it before that is what the window must never do.
/// Anything else (`EXF` for the partition table) fails.
QString writeFakeUpgradeTool(const QString& dir)
{
#ifdef Q_OS_WIN
    const QString path = dir + QStringLiteral("/upgrade_tool.cmd");
    const QByteArray script = "@echo off\r\n"
                              "if /i \"%~1\"==\"LD\" goto ld\r\n"
                              "if /i \"%~1\"==\"SFI\" goto sfi\r\n"
                              "if /i \"%~1\"==\"UF\" goto uf\r\n"
                              "exit /b 1\r\n"
                              ":ld\r\n"
                              "echo List of rockusb connected(1)\r\n"
                              "echo DevNo=1\tVid=0x2207,Pid=0x110c,LocationID=104\tMode=Maskrom\tSerialNo=\r\n"
                              "exit /b 0\r\n"
                              ":sfi\r\n"
                              "echo Type:Update Firmware\r\n"
                              "exit /b 0\r\n"
                              ":uf\r\n"
                              "echo Download Image... (10%%)\r\n"
                              ":wait\r\n"
                              "if exist \"%~dp0release\" goto done\r\n"
                              "ping -n 2 127.0.0.1 >nul\r\n"
                              "goto wait\r\n"
                              ":done\r\n"
                              "echo Upgrade firmware ok.\r\n"
                              "exit /b 0\r\n";
#else
    const QString path = dir + QStringLiteral("/upgrade_tool.sh");
    const QByteArray script = "#!/bin/sh\n"
                              "case \"$1\" in\n"
                              "LD) printf 'List of rockusb connected(1)\\nDevNo=1\\tVid=0x2207,Pid=0x110c,LocationID=104\\tMode=Maskrom\\tSerialNo=\\n'; exit 0;;\n"
                              "SFI) echo 'Type:Update Firmware'; exit 0;;\n"
                              "UF) echo 'Download Image... (10%)'\n"
                              "    while [ ! -f \"$(dirname \"$0\")/release\" ]; do sleep 0.2; done\n"
                              "    echo 'Upgrade firmware ok.'; exit 0;;\n"
                              "esac\n"
                              "exit 1\n";
#endif
    if (!writeFile(path, script)) {
        return QString();
    }
#ifndef Q_OS_WIN
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
#endif
    return path;
}

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
    void readMcuInfoStopsStaleRfmcu();
    void readMcuInfoRoleFromHelpRows();
    void pullWithoutTargetFails();
    void pullConnectsAndCloses();
    void pullDownloadsImage();         // needs ImagePuller
    void mcuUpdateAndRoleThroughFakeBridge();   // needs McuLink + McuUpdater + CcPackage
    void destroyedTabCleansUpTheBoard();
    void pullAndFlashEndToEnd();       // needs ImagePuller + SocFlasher + McuLink
    void pullAndFlashUpdatesMcuInUpdater();
    void downloadModeQuestionOnlyWhileWaiting();
    void languageChangeRetranslates();

    // ---- Partitions (v0.5.1, SocToolKit-style) ----------------------------------------------
    void partitionTableAfterImage();
    void partitionsAllNoneAndSelection();
    void partitionLayoutUnknown();
    void partitionLayoutReadOnceFromBoard();
    void partitionLayoutFromImage();
    void flashSocWritesCheckedPartitions();
    void flashSocMaskromIgnoresSelection();
    void pullAndFlashWritesEveryPartition();

    // ---- MainWindow integration -----------------------------------------------------
    void showFirmwareTabOnce();
    void sessionActionsDisabledWhileCurrent();
    void windowMenuAndAltN();
    void closeTabWhenIdle();
    void closeRefusedWhileBusyAndQuitAsks();
    void quitRefusedWhileUpgradeToolWrites();
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
                   {tab->partitionTable(), "partitionTable"},
                   {tab->partitionsAllButton(), "partitionsAllButton"},
                   {tab->partitionsNoneButton(), "partitionsNoneButton"},
                   {tab->partitionsLabel(), "partitionsLabel"},
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
    // The own runner goes after the collaborators (QObject deletes children in creation order):
    // McuLink's destructor still sends the board cleanup through it. Since 0.5.1 they are the
    // children of the tab's FirmwareJob, itself a child of the tab.
    QVERIFY(plain.firmwareJob() != nullptr);
    QCOMPARE(plain.firmwareJob()->parent(), static_cast<QObject*>(&plain));
    const QObjectList kids = plain.firmwareJob()->children();
    QCOMPARE(plain.toolRunner()->parent(), static_cast<QObject*>(plain.firmwareJob()));
    QVERIFY(kids.indexOf(plain.mcuLink()) >= 0);
    QVERIFY(kids.indexOf(plain.toolRunner()) > kids.indexOf(plain.mcuLink()));
    QVERIFY(kids.indexOf(plain.toolRunner()) > kids.indexOf(plain.socFlasher()));
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
    // Last session's role is shown, never confirmed: the board may be another one now. Update MCU
    // and Set role wait for a Read or a click in this session.
    QVERIFY(tab->roleTxRadio()->isChecked());
    QVERIFY(!tab->roleRxRadio()->isChecked());
    QCOMPARE(tab->selectedRole(), 0u);
    QVERIFY(!tab->setRoleButton()->isEnabled());
    QVERIFY(tab->logView()->toPlainText().contains(QStringLiteral("Role tx remembered from the last session")));
    QVERIFY(QFileInfo::exists(dataFile("firmware-0.8.1.pkg")));
    QVERIFY(tab->setPackagePath(dataFile("firmware-0.8.1.pkg")));
    QVERIFY(!tab->updateMcuButton()->isEnabled());
    QSignalSpy refused(tab.get(), &FirmwareTab::operationFinished);
    tab->updateMcu();   // the slot itself refuses too: nothing reaches the board
    QCOMPARE(refused.count(), qsizetype(1));
    QCOMPARE(refused.at(0).at(1).toBool(), false);
    QVERIFY(refused.at(0).at(2).toString().contains(QStringLiteral("role")));
    QCOMPARE(runner.count(QRegularExpression(QStringLiteral("rfmcu|devices"))), 0);
    QCOMPARE(QSettings().value(QStringLiteral("firmware/role")).toString(), QStringLiteral("tx"));   // kept as it was
    tab->roleTxRadio()->click();   // a click on the shown radio confirms it
    QCOMPARE(tab->selectedRole(), static_cast<quint32>(CcProtocol::RoleTx));
    QVERIFY(tab->roleTxRadio()->isChecked());
    QVERIFY(tab->setRoleButton()->isEnabled());
    QVERIFY(tab->updateMcuButton()->isEnabled());
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

void Tst_firmwaretab::readMcuInfoStopsStaleRfmcu()
{
    // The attached board's state (PLAN.md: an `rfmcu console`, pid 190, holds /dev/ttyS1): as long
    // as it runs, `rfmcu cmd version` exits 2. The Read stops a stale rfmcu first, exactly as
    // McuLink::open() does before its bridge, and then reads.
    const QString stopCommand = QStringLiteral("adb shell \"killall rfmcu 2>/dev/null; true\"");
    const QString versionCommand = QStringLiteral("adb shell \"rfmcu cmd version\"");
    FakeToolRunner runner;
    auto consoleRunning = std::make_shared<bool>(true);
    runner.addHandler(QRegularExpression(QStringLiteral("killall rfmcu")), [consoleRunning](FakeToolInvocation* i) {
        *consoleRunning = false;
        i->finish(1);   // killall's own status (1 = nothing killed) is ignored either way
    });
    runner.addHandler(QRegularExpression(QStringLiteral("rfmcu cmd version")), [consoleRunning](FakeToolInvocation* i) {
        if (*consoleRunning) {
            i->emitOutput("ERR PORT busy: /dev/ttyS1 is held by rfmcu console (pid 190)\r\n");
            i->finish(2);
        } else {
            i->emitOutput("OK version=0.8.1 role=rx\r\n");
            i->finish(0);
        }
    });
    auto tab = makeTab(&runner);
    QSignalSpy finished(tab.get(), &FirmwareTab::operationFinished);
    tab->readMcuInfo();
    QVERIFY(tab->cancelButton()->isEnabled());   // cancellable from the first command on
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), qsizetype(1), kOpTimeoutMs);
    QVERIFY2(finished.at(0).at(1).toBool(), qPrintable(finished.at(0).at(2).toString()));
    QCOMPARE(runner.invocations(), (QStringList{stopCommand, versionCommand}));   // the stop first
    QCOMPARE(tab->runningMcuVersion(), QStringLiteral("0.8.1"));
    QCOMPARE(tab->selectedRole(), static_cast<quint32>(CcProtocol::RoleRx));
    QVERIFY(tab->logView()->toPlainText().contains(stopCommand));   // the command line is logged

    // With a board chosen for the link, both commands go to it (cliCommand()'s serial).
    runner.reset();
    runner.addScript(QRegularExpression(QStringLiteral("killall rfmcu")), QByteArray());
    runner.addScript(QRegularExpression(QStringLiteral("rfmcu cmd version")), "OK version=0.8.1 role=rx\r\n");
    tab->mcuLink()->setSerial(QStringLiteral("c3d9"));
    finished.clear();
    tab->readMcuInfo();
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), qsizetype(1), kOpTimeoutMs);
    QVERIFY(finished.at(0).at(1).toBool());
    QCOMPARE(runner.invocations(), (QStringList{QStringLiteral("adb -s c3d9 shell \"killall rfmcu 2>/dev/null; true\""),
                                                QStringLiteral("adb -s c3d9 shell \"rfmcu cmd version\"")}));

    // adb that cannot run ends the Read with the reason; rfmcu cmd never runs.
    runner.reset();
    runner.addHandler(QRegularExpression(QStringLiteral("killall rfmcu")),
                      [](FakeToolInvocation* i) { i->failToStart(QStringLiteral("no adb here")); });
    finished.clear();
    tab->readMcuInfo();
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), qsizetype(1), kOpTimeoutMs);
    QCOMPARE(finished.at(0).at(1).toBool(), false);
    QVERIFY2(finished.at(0).at(2).toString().contains(QStringLiteral("no adb here")), qPrintable(finished.at(0).at(2).toString()));
    QCOMPARE(runner.count(QRegularExpression(QStringLiteral("rfmcu cmd"))), 0);

    // Cancel while the stop runs: "cancelled", nothing else is started.
    runner.reset();
    runner.addHandler(QRegularExpression(QStringLiteral("killall rfmcu")), [](FakeToolInvocation* i) { i->hang(); });
    finished.clear();
    tab->readMcuInfo();
    QTRY_COMPARE(runner.liveInvocations().size(), qsizetype(1));
    QVERIFY(tab->cancelButton()->isEnabled());
    tab->cancelButton()->click();
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), qsizetype(1), kOpTimeoutMs);
    QCOMPARE(finished.at(0).at(2).toString(), QStringLiteral("cancelled"));
    QCOMPARE(runner.count(QRegularExpression(QStringLiteral("rfmcu cmd"))), 0);
    QVERIFY(!tab->isBusy());
}

void Tst_firmwaretab::readMcuInfoRoleFromHelpRows()
{
    // An application whose "version" has no role (0.8.2 on the board, 2026-10-13: "OK
    // version=0.8.2 baud=115200"): the role comes from "help", which prints one "OK <command>
    // <usage>" row per command (fw_update.py probe_application(); FakeMcu prints the same rows).
    // Every row counts, not just the first.
    const QByteArray commonRows = "OK commands:\r\nOK help [command]\r\nOK version show firmware version\r\n"
                                  "OK power show power state\r\nOK bootloader reboot into the UART updater\r\n";
    const struct
    {
        const char* name;
        QByteArray roleRow;
        QString role;
        quint32 selected;
    } cases[] = {
        {"tx", "OK tx tx sync_frame <n> | tx status\r\n", QStringLiteral("tx"), CcProtocol::RoleTx},
        {"rx", "OK rx rx status | rx dump on|off\r\n", QStringLiteral("rx"), CcProtocol::RoleRx},
        {"neither", QByteArray(), QStringLiteral("unknown"), 0},
    };
    for (const auto& c : cases) {
        FakeToolRunner runner;
        runner.addScript(QRegularExpression(QStringLiteral("killall rfmcu")), QByteArray());
        runner.addScript(QRegularExpression(QStringLiteral("rfmcu cmd version")), "OK version=0.8.2 baud=115200\r\n");
        runner.addScript(QRegularExpression(QStringLiteral("rfmcu cmd help")), commonRows + c.roleRow);
        auto tab = makeTab(&runner);
        QSignalSpy finished(tab.get(), &FirmwareTab::operationFinished);
        tab->readMcuInfo();
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), qsizetype(1), kOpTimeoutMs);
        QVERIFY2(finished.at(0).at(1).toBool(), c.name);
        QCOMPARE(runner.count(QRegularExpression(QStringLiteral("rfmcu cmd help"))), 1);
        QCOMPARE(tab->runningMcuVersion(), QStringLiteral("0.8.2"));
        QCOMPARE(tab->runningMcuRole(), c.role);
        QCOMPARE(tab->selectedRole(), c.selected);   // "unknown" pre-selects nothing
        QVERIFY2(tab->mcuStatusLabel()->text() == QStringLiteral("0.8.2 %1").arg(c.role), qPrintable(tab->mcuStatusLabel()->text()));
    }
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
    // One image, one unit: the tab's "Remote update.img: ..." line and the puller's own lines use
    // the same decimal (SI) size, never "262.1 kB" next to "256.0 KB".
    const QString size = QLocale::c().formattedDataSize(machine.image.size(), 1, QLocale::DataSizeSIFormat);
    const QString pullLog = tab->logView()->toPlainText();
    QVERIFY2(pullLog.contains(QStringLiteral("Remote update.img: %1, built").arg(size)), qPrintable(pullLog));
    QVERIFY2(pullLog.contains(QStringLiteral("Downloading update.img (%1)").arg(size)), qPrintable(pullLog));

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

void Tst_firmwaretab::destroyedTabCleansUpTheBoard()
{
    // The window goes away while an MCU update runs (the quit question was answered with Quit):
    // the board's `killall rfmcu` and the forward removal are sent detached, so they survive the
    // tool runner (a ProcessToolRunner kills its processes when it is destroyed) - otherwise the
    // board's bridge would keep the UART lock.
    FakeMcu mcu;
    mcu.setSilent(true);   // the update waits in its first probe: the link is open when the tab goes
    FakeMcuServer bridge(&mcu);
    QVERIFY(bridge.listen());
    FakeToolRunner runner;
    scriptMcuBridge(runner, bridge.port());
    auto tab = makeTab(&runner);
    QVERIFY(tab->setPackagePath(dataFile("firmware-0.8.1.pkg")));
    tab->roleRxRadio()->click();
    tab->updateMcu();
    QTRY_VERIFY_WITH_TIMEOUT(tab->mcuLink()->isOpen(), kOpTimeoutMs);
    const int before = static_cast<int>(runner.invocations().size());
    tab.reset();
    const QStringList after = runner.invocations().mid(before);
    QCOMPARE(after.size(), qsizetype(2));
    QVERIFY2(after.at(0).endsWith(QStringLiteral("shell \"killall rfmcu 2>/dev/null; true\"")), qPrintable(after.at(0)));
    QVERIFY2(after.at(1).contains(QStringLiteral("forward --remove tcp:")), qPrintable(after.at(1)));
    QVERIFY(runner.optionsAt(before).detached);
    QVERIFY(runner.optionsAt(before + 1).detached);
}

void Tst_firmwaretab::pullAndFlashEndToEnd()
{
    // needs ImagePuller, SocFlasher and McuLink: pull -> flash SoC -> wait for ADB -> read the MCU,
    // which already runs the package's version, so the update is skipped. The board is in MASKROM
    // mode: UF and its progress lines (pullAndFlashWritesEveryPartition covers Loader mode).
    SshProfileStore store;
    BuildMachine machine;
    QVERIFY(machine.start(&store, tempPath(QStringLiteral("pull-flash"))));
    FakeToolRunner runner;
    runner.addScript(QRegularExpression(QStringLiteral("^upgrade_tool LD$")), kLdMaskrom);
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
    // The version read stopped a stale rfmcu first (the attached board runs an `rfmcu console`).
    const QStringList commands = runner.invocations();
    const qsizetype stop = commands.indexOf(QRegularExpression(QStringLiteral(".*killall rfmcu.*")));   // exact match
    QVERIFY(stop >= 0);
    QVERIFY(stop < commands.indexOf(QRegularExpression(QStringLiteral(".*rfmcu cmd version.*"))));
}

void Tst_firmwaretab::pullAndFlashUpdatesMcuInUpdater()
{
    // Runbook Part J: yesterday's MCU update was cut off, the CC1310 sits in its UART updater and
    // has no CLI, so `rfmcu cmd version` gets no reply (exit 3). Pull & Flash updates it anyway
    // (McuUpdater::flash() talks to a board in the updater) with the role the user confirmed;
    // adb-level failures and an unconfirmed role still stop the chain.
    SshProfileStore store;
    BuildMachine machine;
    QVERIFY(machine.start(&store, tempPath(QStringLiteral("pull-flash-updater"))));
    FakeMcu mcu;
    mcu.setInUpdater(true);
    FakeMcuServer bridge(&mcu);
    QVERIFY(bridge.listen());
    FakeToolRunner runner;
    runner.addScript(QRegularExpression(QStringLiteral("^upgrade_tool LD$")), kLdMaskrom);
    runner.addHandler(QRegularExpression(QStringLiteral("^upgrade_tool UF ")), [](FakeToolInvocation* i) {
        i->emitOutput("Download Image... (100%)\r\nUpgrade firmware ok.\r\n");
        i->finish(0);
    });
    runner.addScript(QRegularExpression(QStringLiteral("^upgrade_tool SFI ")), "Type:Update Firmware\r\nChip Tag:110E\r\n");
    runner.addScript(QRegularExpression(QStringLiteral("^adb( -s \\S+)? get-state$")), "device\n");
    runner.addScript(QRegularExpression(QStringLiteral("cat /proc/version")), kProcVersion);
    auto versionExit = std::make_shared<int>(3);
    runner.addHandler(QRegularExpression(QStringLiteral("rfmcu cmd version")), [versionExit](FakeToolInvocation* i) {
        if (*versionExit == 1) {
            i->emitOutput("ERR CMD unknown_command; use help\r\n");
        }
        i->finish(*versionExit);   // 3: rfmcu sent the line, nothing answered
    });
    scriptMcuBridge(runner, bridge.port());
    auto tab = makeTab(&runner, &store);
    tab->socFlasher()->setPollIntervalMs(10);
    tab->setSourceTarget(QStringLiteral("ssh:profile:") + machine.profile.id);
    tab->setRemoteFolder(machine.imageFolder);
    tab->setCacheDirectory(tempPath(QStringLiteral("pull-flash-updater-cache")));
    QVERIFY(tab->setPackagePath(dataFile("firmware-0.8.1.pkg")));
    tab->roleRxRadio()->click();   // the bench role, confirmed by the user
    QSignalSpy finished(tab.get(), &FirmwareTab::operationFinished);
    const auto runChain = [&]() {
        finished.clear();
        tab->pullAndFlash();
        return QTest::qWaitFor([&]() { return !finished.isEmpty() && finished.last().at(0).toString() == QLatin1String("pull-flash"); },
                               60000);
    };

    QVERIFY(runChain());
    QStringList steps;
    for (const QList<QVariant>& args : std::as_const(finished)) {
        QVERIFY2(args.at(1).toBool(), qPrintable(args.at(0).toString() + QStringLiteral(": ") + args.at(2).toString()));
        steps << args.at(0).toString();
    }
    QCOMPARE(steps, (QStringList{QStringLiteral("pull"), QStringLiteral("flash"), QStringLiteral("mcu"), QStringLiteral("pull-flash")}));
    QCOMPARE(mcu.image(), tab->package().image);   // every byte arrived through the updater
    QCOMPARE(mcu.role(), static_cast<quint32>(CcProtocol::RoleRx));
    QVERIFY(mcu.commands().isEmpty());              // no CLI was involved
    QVERIFY(!mcu.inUpdater());                      // the application runs again
    QVERIFY(tab->logView()->toPlainText().contains(QStringLiteral("MCU version unknown")));
    QCOMPARE(runner.count(QRegularExpression(QStringLiteral("forward tcp:0"))), 1);
    QTRY_VERIFY(!tab->mcuLink()->isOpen());

    // An rfmcu ERR (exit 1) is no "MCU without a CLI": the chain stops, nothing is sent.
    *versionExit = 1;
    QVERIFY(runChain());
    QCOMPARE(finished.last().at(1).toBool(), false);
    QVERIFY2(finished.last().at(2).toString().contains(QStringLiteral("cannot read the running MCU version")),
             qPrintable(finished.last().at(2).toString()));
    QCOMPARE(runner.count(QRegularExpression(QStringLiteral("forward tcp:0"))), 1);

    // No reply again, but no role confirmed in this session: never guessed - the chain stops and says why.
    *versionExit = 3;
    tab->setSelectedRole(0);
    QVERIFY(runChain());
    QCOMPARE(finished.last().at(1).toBool(), false);
    QVERIFY2(finished.last().at(2).toString().contains(QStringLiteral("no role is confirmed")),
             qPrintable(finished.last().at(2).toString()));
    QCOMPARE(runner.count(QRegularExpression(QStringLiteral("forward tcp:0"))), 1);
    QVERIFY(!tab->isBusy());
}

void Tst_firmwaretab::downloadModeQuestionOnlyWhileWaiting()
{
    // No board visible: the tab asks whether to send "reboot loader" through a connected session.
    // The answer only counts while the flasher still waits for Loader mode; the question closes by
    // itself when the board appears meanwhile (the user held MASKROM), so a late "Send" can never
    // reboot a freshly flashed board back into Loader mode.
    QuickCommandStore quickCommands;
    CommandHistory history;
    SessionWidget session(&quickCommands, &history);
    session.setPortName(kLoopback);
    QVERIFY(session.connectPort());
    QTRY_VERIFY_WITH_TIMEOUT(session.isConnected(), kOpTimeoutMs);
    QSignalSpy sent(session.transport(), &Transport::dataSent);

    FakeToolRunner runner;
    auto present = std::make_shared<bool>(false);
    runner.addHandler(QRegularExpression(QStringLiteral("^upgrade_tool LD$")), [present](FakeToolInvocation* i) {
        i->emitOutput(*present ? kLdMaskrom : QByteArray("List of rockusb connected(0)\r\n"));
        i->finish(0);
    });
    runner.addScript(QRegularExpression(QStringLiteral("^adb( -s \\S+)? devices$")), "List of devices attached\r\n\r\n");
    QPointer<FakeToolInvocation> uf;
    runner.addHandler(QRegularExpression(QStringLiteral("^upgrade_tool UF ")), [&uf](FakeToolInvocation* i) {
        uf = i;
        i->emitOutput("Download Image... (10%)\r");
        i->hang();
    });
    runner.addScript(QRegularExpression(QStringLiteral("^upgrade_tool SFI ")), "Type:Update Firmware\r\n");
    runner.addScript(QRegularExpression(QStringLiteral("^adb( -s \\S+)? get-state$")), "device\n");
    runner.addScript(QRegularExpression(QStringLiteral("cat /proc/version")), kProcVersion);
    auto tab = makeTab(&runner);
    tab->socFlasher()->setPollIntervalMs(20);
    tab->setSessionProvider([&session]() { return QList<SessionWidget*>{&session}; });
    const QString cache = tempPath(QStringLiteral("download-mode-cache"));
    QVERIFY(writeFile(cache + QStringLiteral("/update.img"), rkfwBytes()));
    tab->setCacheDirectory(cache);
    QSignalSpy finished(tab.get(), &FirmwareTab::operationFinished);

    // What the user does while the question is open; it runs inside the question's own event loop.
    enum class Reaction { HoldMaskrom, ClickSend };
    Reaction reaction = Reaction::HoldMaskrom;
    int questions = 0;
    QString questionText;
    QTimer user;
    user.setInterval(20);
    connect(&user, &QTimer::timeout, &user, [&]() {
        auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (!box || !box->isVisible() || questions > 0) {
            return;
        }
        ++questions;
        questionText = box->text();
        if (reaction == Reaction::ClickSend) {
            box->button(QMessageBox::Yes)->click();   // "Send through <tab>" while still waiting
        }
        *present = true;   // the board shows up in download mode (MASKROM here: the UF method)
    });
    user.start();
    const auto finishFlash = [&]() {
        if (!QTest::qWaitFor([&]() { return !uf.isNull(); }, kOpTimeoutMs)) {
            return false;
        }
        uf->emitOutput("Upgrade firmware ok.\r\n");
        uf->finish(0);
        return QTest::qWaitFor([&]() { return !finished.isEmpty(); }, kOpTimeoutMs);
    };

    // 1. The board appears with the question open: it closes by itself, nothing is sent.
    tab->flashSoc();
    QTRY_COMPARE_WITH_TIMEOUT(questions, 1, kOpTimeoutMs);
    QVERIFY2(questionText.contains(session.title()), qPrintable(questionText));
    QTRY_COMPARE_WITH_TIMEOUT(tab->socFlasher()->phase(), SocFlasher::Phase::Flashing, kOpTimeoutMs);
    QTRY_VERIFY(QApplication::activeModalWidget() == nullptr);
    QTRY_VERIFY(tab->logView()->toPlainText().contains(QStringLiteral("The board appeared meanwhile; nothing sent")));
    QVERIFY(finishFlash());
    QVERIFY2(finished.last().at(1).toBool(), qPrintable(finished.last().at(2).toString()));
    QCOMPARE(sent.count(), qsizetype(0));
    QVERIFY(!tab->logView()->toPlainText().contains(QStringLiteral("Sent \"reboot loader\"")));

    // 2. "Send" while the flasher still waits: the session gets the command, once.
    *present = false;
    questions = 0;
    uf.clear();
    finished.clear();
    reaction = Reaction::ClickSend;
    tab->flashSoc();
    QTRY_COMPARE_WITH_TIMEOUT(questions, 1, kOpTimeoutMs);
    QTRY_COMPARE_WITH_TIMEOUT(sent.count(), qsizetype(1), kOpTimeoutMs);
    QCOMPARE(sent.at(0).at(0).toByteArray(), QByteArrayLiteral("reboot loader\r"));
    QVERIFY(tab->logView()->toPlainText().contains(QStringLiteral("Sent \"reboot loader\" through %1").arg(session.title())));
    QVERIFY(finishFlash());
    QVERIFY2(finished.last().at(1).toBool(), qPrintable(finished.last().at(2).toString()));
    QCOMPARE(sent.count(), qsizetype(1));
    user.stop();
    session.disconnectPort();
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
// Partitions (v0.5.1): the image's partitions listed and written like SocToolKit's Download page
// =======================================================================================

namespace {

/// The tab showing the seven rows of a freshly selected image (the label says so).
bool waitForTable(const FirmwareTab& tab, const QString& labelStart = QStringLiteral("7 partitions"))
{
    return QTest::qWaitFor([&]() {
        return tab.partitionTable()->rowCount() == 7 && tab.partitionsLabel()->text().startsWith(labelStart);
    }, kOpTimeoutMs);
}

} // namespace

void Tst_firmwaretab::partitionTableAfterImage()
{
    // An image and a remembered layout: the tab unpacks the image (EXF through the SocFlasher) and
    // lists its seven partitions - checkbox, name, image file, offset, partition size, image size.
    FakeToolRunner runner;
    scriptSocFlash(runner, kLdLoader);
    AppSettings::instance().setFirmwareMtdParts(QString(), QStringLiteral("mtdparts=") + kNodeMtdParts);
    auto tab = makeTab(&runner);
    QCOMPARE(tab->flashLayout(), QStringLiteral("mtdparts=") + kNodeMtdParts);
    QTableWidget* table = tab->partitionTable();
    QCOMPARE(table->rowCount(), 0);
    QCOMPARE(table->columnCount(), 6);
    QCOMPARE(table->horizontalHeaderItem(1)->text(), QStringLiteral("Partition"));
    QCOMPARE(table->horizontalHeaderItem(3)->text(), QStringLiteral("Offset"));
    QCOMPARE(tab->partitionsLabel()->text(), QStringLiteral("no image selected"));
    QVERIFY(!tab->partitionsAllButton()->isEnabled());

    const QString image = tempPath(QStringLiteral("partitions/update.img"));
    QVERIFY(writeFile(image, rkfwBytes()));
    tab->setImagePath(image);
    QCOMPARE(tab->partitionsLabel()->text(), QStringLiteral("unpacking the image to list its partitions..."));
    QVERIFY(waitForTable(*tab));
    QCOMPARE(runner.count(exfPattern()), 1);
    QVERIFY(runner.invocations().contains(ToolRunner::formatCommandLine(
        QStringLiteral("upgrade_tool"), {QStringLiteral("EXF"), QDir::toNativeSeparators(image),
                                         QDir::toNativeSeparators(extractDirOf(image))})));
    const QStringList offsets{QStringLiteral("0x0"),     QStringLiteral("0x10000"),  QStringLiteral("0x50000"),
                              QStringLiteral("0x90000"), QStringLiteral("0xf0000"),  QStringLiteral("0x3f0000"),
                              QStringLiteral("0x8f0000")};
    const QStringList sizes{QStringLiteral("64 KiB"),  QStringLiteral("256 KiB"), QStringLiteral("256 KiB"), QStringLiteral("384 KiB"),
                            QStringLiteral("3 MiB"),   QStringLiteral("5 MiB"),   QStringLiteral("3 MiB")};
    for (int k = 0; k < 7; ++k) {
        const QTableWidgetItem* check = table->item(k, 0);
        QVERIFY(check->flags() & Qt::ItemIsUserCheckable);
        QVERIFY(check->flags() & Qt::ItemIsEnabled);
        QCOMPARE(check->checkState(), Qt::Checked);
        QCOMPARE(check->data(Qt::UserRole).toString(), QLatin1String(kNodeNames[k]));
        QCOMPARE(table->item(k, 1)->text(), QLatin1String(kNodeNames[k]));
        QCOMPARE(table->item(k, 2)->text(), QLatin1String(kNodeNames[k]) + QStringLiteral(".img"));
        QCOMPARE(table->item(k, 3)->text(), offsets.at(k));
        QCOMPARE(table->item(k, 4)->text(), sizes.at(k));
        QCOMPARE(table->item(k, 5)->text(), QLocale().toString(kNodeImageBytes[k]));
    }
    QCOMPARE(table->item(4, 5)->text(), QStringLiteral("3,043,840"));
    QCOMPARE(table->item(0, 1)->toolTip(), QDir::toNativeSeparators(extractDirOf(image) + QStringLiteral("/env.img")));
    QCOMPARE(tab->selectedPartitions(), nodeNames());
    QCOMPARE(tab->partitionsLabel()->text(), QStringLiteral("7 partitions, 7 selected (10.8 MB) - layout from the board"));
    QCOMPARE(tab->partitionsLabel()->toolTip(), QStringLiteral("Flash layout from the board: mtdparts=") + kNodeMtdParts);
    QVERIFY(tab->partitionsAllButton()->isEnabled());
    QVERIFY(tab->partitionsNoneButton()->isEnabled());
    QCOMPARE(tab->socFlasher()->partitions().size(), qsizetype(7));
    QVERIFY(tab->logView()->toPlainText().contains(
        QStringLiteral("Partitions of update.img: env@0x0, idblock@0x10000, uboot@0x50000, meta@0x90000, boot@0xf0000, "
                       "rootfs@0x3f0000, userdata@0x8f0000")));

    // Asked again for the same image: the rows again, nothing unpacked again.
    tab->planPartitions();
    QCOMPARE(tab->partitionsLabel()->text(), QStringLiteral("unpacking the image to list its partitions..."));
    QVERIFY(waitForTable(*tab));
    QCOMPARE(runner.count(exfPattern()), 1);

    // Rows that cannot be written - a name the layout lacks, an image too big for its partition -
    // are unchecked and disabled, with the reason as the tooltip.
    AppSettings::instance().setFirmwareMtdParts(QString(), QString(kNodeMtdParts).replace(QStringLiteral("(meta)"), QStringLiteral("(misc)")));
    const QString big = tempPath(QStringLiteral("partitions/big.img"));
    QVERIFY(writeFile(big, rkfwBytes()));
    tab->setImagePath(big);
    QVERIFY(waitForTable(*tab));
    QVERIFY(!(table->item(3, 0)->flags() & Qt::ItemIsEnabled));
    QCOMPARE(table->item(3, 0)->checkState(), Qt::Unchecked);
    QCOMPARE(table->item(3, 1)->toolTip(), QStringLiteral("meta is not in the board's partition layout"));
    QCOMPARE(table->item(3, 3)->text(), QString());   // no partition, no offset
    QVERIFY(!(table->item(4, 0)->flags() & Qt::ItemIsEnabled));
    QCOMPARE(table->item(4, 0)->checkState(), Qt::Unchecked);
    QCOMPARE(table->item(4, 2)->toolTip(), QStringLiteral("boot.img (3145729 bytes) does not fit the partition (3145728 bytes)"));
    QCOMPARE(tab->selectedPartitions(), (QStringList{QStringLiteral("env"), QStringLiteral("idblock"), QStringLiteral("uboot"),
                                                     QStringLiteral("rootfs"), QStringLiteral("userdata")}));
    QCOMPARE(tab->partitionsLabel()->text(), QStringLiteral("7 partitions, 5 selected (7.4 MB) - layout from the board"));
}

void Tst_firmwaretab::partitionsAllNoneAndSelection()
{
    FakeToolRunner runner;
    scriptSocFlash(runner, kLdLoader);
    AppSettings::instance().setFirmwareMtdParts(QString(), kNodeMtdParts);
    auto tab = makeTab(&runner);
    const QString image = tempPath(QStringLiteral("selection/update.img"));
    QVERIFY(writeFile(image, rkfwBytes()));
    tab->setImagePath(image);
    QVERIFY(waitForTable(*tab));

    tab->partitionsNoneButton()->click();
    QVERIFY(tab->selectedPartitions().isEmpty());
    QVERIFY2(tab->partitionsLabel()->text().startsWith(QStringLiteral("7 partitions, 0 selected")), qPrintable(tab->partitionsLabel()->text()));
    tab->partitionsAllButton()->click();
    QCOMPARE(tab->selectedPartitions(), nodeNames());
    QVERIFY(tab->socFlasher()->selectedPartitions().isEmpty());   // "every row": the flasher's default

    // A click on a checkbox is handed to the flasher at once.
    tab->partitionTable()->item(4, 0)->setCheckState(Qt::Unchecked);
    QStringList withoutBoot = nodeNames();
    withoutBoot.removeAll(QStringLiteral("boot"));
    QCOMPARE(tab->selectedPartitions(), withoutBoot);
    QCOMPARE(tab->socFlasher()->selectedPartitions(), withoutBoot);
    QCOMPARE(tab->partitionsLabel()->text(), QStringLiteral("7 partitions, 6 selected (7.8 MB) - layout from the board"));

    // setSelectedPartitions(): exactly those rows (an unknown name checks nothing).
    tab->setSelectedPartitions({QStringLiteral("boot"), QStringLiteral("rootfs"), QStringLiteral("nonexistent")});
    const QStringList two{QStringLiteral("boot"), QStringLiteral("rootfs")};
    QCOMPARE(tab->selectedPartitions(), two);
    QVERIFY(tab->socFlasher()->selectedPartitions().contains(QStringLiteral("boot")));
    QVERIFY(tab->socFlasher()->selectedPartitions().contains(QStringLiteral("rootfs")));
    for (const PartitionImage& row : tab->socFlasher()->partitions()) {
        QCOMPARE(row.selected, two.contains(row.name));
    }
    QCOMPARE(tab->partitionTable()->item(4, 0)->checkState(), Qt::Checked);
    QCOMPARE(tab->partitionTable()->item(0, 0)->checkState(), Qt::Unchecked);

    // A new plan of the same image keeps the checks; another image starts with every row checked.
    tab->planPartitions();
    QVERIFY(waitForTable(*tab, QStringLiteral("7 partitions, 2 selected")));
    QCOMPARE(tab->selectedPartitions(), two);
    const QString second = tempPath(QStringLiteral("selection/second.img"));
    QVERIFY(writeFile(second, rkfwBytes()));
    tab->setImagePath(second);
    QVERIFY(waitForTable(*tab, QStringLiteral("7 partitions, 7 selected")));
    QCOMPARE(tab->selectedPartitions(), nodeNames());
}

void Tst_firmwaretab::partitionLayoutUnknown()
{
    // No board seen yet: the rows are listed, none can be checked, the label says what to do.
    FakeToolRunner runner;
    scriptSocFlash(runner, kLdLoader);
    auto tab = makeTab(&runner);
    QVERIFY(tab->flashLayout().isEmpty());
    const QString image = tempPath(QStringLiteral("unknown/update.img"));
    QVERIFY(writeFile(image, rkfwBytes()));
    tab->setImagePath(image);
    const QString unknown = QStringLiteral("partition layout unknown: connect the board running once");
    QVERIFY(waitForTable(*tab, unknown));
    QCOMPARE(tab->partitionsLabel()->text(), unknown);
    for (int k = 0; k < 7; ++k) {
        QVERIFY(!(tab->partitionTable()->item(k, 0)->flags() & Qt::ItemIsEnabled));
        QCOMPARE(tab->partitionTable()->item(k, 0)->checkState(), Qt::Unchecked);
        QCOMPARE(tab->partitionTable()->item(k, 1)->toolTip(), unknown);
    }
    QVERIFY(tab->selectedPartitions().isEmpty());
    QVERIFY(!tab->partitionsAllButton()->isEnabled());
    QVERIFY(!tab->partitionsNoneButton()->isEnabled());
    QVERIFY(tab->flashSocButton()->isEnabled());   // MASKROM mode needs no layout
    QVERIFY(tab->logView()->toPlainText().contains(QStringLiteral("The flash layout is not known yet")));

    // Flash SoC with a board in Loader mode: the flasher says why it cannot write.
    tab->socFlasher()->setPollIntervalMs(10);
    QSignalSpy finished(tab.get(), &FirmwareTab::operationFinished);
    tab->flashSoc();
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), qsizetype(1), kOpTimeoutMs);
    QCOMPARE(finished.at(0).at(1).toBool(), false);
    QCOMPARE(finished.at(0).at(2).toString(), QStringLiteral("partition layout unknown: boot the board once so it can be read "
                                                             "over ADB, or flash it in MASKROM mode"));
    QVERIFY(wlCommands(runner).isEmpty());
}

void Tst_firmwaretab::partitionLayoutReadOnceFromBoard()
{
    // The status refresh sees a running board whose layout is not remembered: the tab reads its
    // /proc/cmdline once (adb, never blocking), stores the mtdparts per board and plans again.
    FakeToolRunner runner;
    runner.addScript(QRegularExpression(QStringLiteral("^adb -s c3d9 shell ")), "console=ttyFIQ0 root=/dev/mmcblk0p5\n");
    scriptSocFlash(runner, kLdLoader);
    auto tab = makeTab(&runner);
    const QString image = tempPath(QStringLiteral("layout-read/update.img"));
    QVERIFY(writeFile(image, rkfwBytes()));
    tab->setImagePath(image);
    QVERIFY(waitForTable(*tab, QStringLiteral("partition layout unknown")));

    SocFlasher::DeviceStatus board;
    board.mode = SocFlasher::DeviceStatus::Mode::Adb;
    board.serial = QStringLiteral("0225ce3c8d8d6e44");
    board.detail = QStringLiteral("device");
    const QRegularExpression read(QStringLiteral("^adb -s 0225ce3c8d8d6e44 shell \"cat /proc/cmdline\"$"));
    emit tab->socFlasher()->statusChanged(board);
    QCOMPARE(runner.count(read), 1);
    QTRY_COMPARE(tab->flashLayout(), kNodeMtdParts);
    QCOMPARE(AppSettings::instance().firmwareMtdParts(board.serial), kNodeMtdParts);
    QCOMPARE(AppSettings::instance().firmwareMtdParts(QString()), kNodeMtdParts);   // "*": for a board met in Loader mode
    QCOMPARE(QSettings().value(QStringLiteral("firmware/mtdparts")).toMap().value(board.serial).toString(), kNodeMtdParts);
    QVERIFY(tab->logView()->toPlainText().contains(QStringLiteral("Flash layout of board 0225ce3c8d8d6e44: ") + kNodeMtdParts));
    QVERIFY(waitForTable(*tab, QStringLiteral("7 partitions, 7 selected")));   // planned again with it
    QCOMPARE(runner.count(exfPattern()), 1);                                  // the layout alone changed

    // Seen again by the 2 s refresh: no second read.
    emit tab->socFlasher()->statusChanged(board);
    emit tab->socFlasher()->statusChanged(board);
    QCoreApplication::processEvents();
    QCOMPARE(runner.count(read), 1);

    // The next session knows it already: no read at all.
    {
        FakeToolRunner runner2;
        scriptSocFlash(runner2, kLdLoader);
        auto second = makeTab(&runner2);
        emit second->socFlasher()->statusChanged(board);
        QCOMPARE(runner2.count(QRegularExpression(QStringLiteral("cat /proc/cmdline"))), 0);
        QCOMPARE(second->flashLayout(), kNodeMtdParts);
    }

    // A board whose kernel has no mtdparts: said once, asked once; the last layout read stays in use.
    SocFlasher::DeviceStatus other = board;
    other.serial = QStringLiteral("c3d9");
    emit tab->socFlasher()->statusChanged(other);
    QTRY_VERIFY(tab->logView()->toPlainText().contains(QStringLiteral("Board c3d9 reports no flash layout (no mtdparts in /proc/cmdline)")));
    emit tab->socFlasher()->statusChanged(other);
    QCOMPARE(runner.count(QRegularExpression(QStringLiteral("^adb -s c3d9 shell "))), 1);
    QVERIFY(AppSettings::instance().firmwareMtdParts(other.serial).isEmpty());
    QCOMPARE(tab->flashLayout(), kNodeMtdParts);

    // What a flash reads in its step 2 (SocFlasher::mtdPartsRead()) is stored for the flow's board.
    emit tab->socFlasher()->mtdPartsRead(QStringLiteral("x:1M(a)"));
    QCOMPARE(AppSettings::instance().firmwareMtdParts(other.serial), QStringLiteral("x:1M(a)"));
    QCOMPARE(tab->flashLayout(), QStringLiteral("x:1M(a)"));
}

void Tst_firmwaretab::partitionLayoutFromImage()
{
    // The image carries its layout (env.img): planned without any remembered layout, the label
    // says where it came from; a board layout that differs is shown and logged, the image's wins.
    FakeToolRunner runner;
    scriptSocFlash(runner, kLdLoader);
    auto tab = makeTab(&runner);
    tab->socFlasher()->setPollIntervalMs(10);
    QVERIFY(tab->flashLayout().isEmpty());
    const QString image = tempPath(QStringLiteral("withenv/update.img"));
    QVERIFY(writeFile(image, rkfwBytes()));
    tab->setImagePath(image);
    QVERIFY(waitForTable(*tab));
    QCOMPARE(tab->partitionsLabel()->text(), QStringLiteral("7 partitions, 7 selected (10.8 MB) - layout from the image"));
    QCOMPARE(tab->partitionsLabel()->toolTip(), QStringLiteral("Flash layout from the image's env.img: ") + kNodeMtdParts);
    QCOMPARE(tab->flashLayout(), kNodeMtdParts);
    QCOMPARE(tab->selectedPartitions(), nodeNames());
    QCOMPARE(tab->partitionTable()->item(5, 3)->text(), QStringLiteral("0x3f0000"));
    QVERIFY(tab->logView()->toPlainText().contains(QStringLiteral("layout from the image's env.img: ") + kNodeMtdParts));
    QVERIFY(AppSettings::instance().firmwareMtdParts(QString()).isEmpty());   // nothing is remembered from an image

    // A remembered board layout that differs (an older build: boot 4 MiB, userdata 2 MiB).
    const QString older =
        QStringLiteral("sfc_nor:64K(env),256K@64K(idblock),256K(uboot),384K(meta),4M(boot),5M(rootfs),2M(userdata)");
    AppSettings::instance().setFirmwareMtdParts(QString(), older);
    tab->planPartitions();
    QVERIFY(waitForTable(*tab));
    QCOMPARE(tab->partitionsLabel()->text(),
             QStringLiteral("7 partitions, 7 selected (10.8 MB) - layout from the image (the board's differs)"));
    QVERIFY(tab->partitionsLabel()->toolTip().contains(QStringLiteral("The board's layout: ") + older));
    QCOMPARE(tab->flashLayout(), kNodeMtdParts);
    QVERIFY(tab->logView()->toPlainText().contains(QStringLiteral("warning: the image's flash layout differs from the board's")));
    QCOMPARE(tab->partitionTable()->item(5, 3)->text(), QStringLiteral("0x3f0000"));   // the image's rootfs offset

    // Flash SoC in Loader mode: the image's offsets.
    QSignalSpy finished(tab.get(), &FirmwareTab::operationFinished);
    tab->flashSoc();
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), qsizetype(1), kOpTimeoutMs);
    QVERIFY2(finished.at(0).at(1).toBool(), qPrintable(finished.at(0).at(2).toString()));
    QStringList expected;
    for (int k = 0; k < 7; ++k) {
        expected << expectedWl(k, extractDirOf(image));
    }
    QCOMPARE(wlCommands(runner), expected);
    QVERIFY(tab->logView()->toPlainText().contains(QStringLiteral("flash layout in use (from the image's env.img)")));
}

void Tst_firmwaretab::flashSocWritesCheckedPartitions()
{
    // Loader mode: Flash SoC writes exactly the checked rows, one WL each, then RD.
    FakeToolRunner runner;
    scriptSocFlash(runner, kLdLoader);
    AppSettings::instance().setFirmwareMtdParts(QString(), kNodeMtdParts);
    auto tab = makeTab(&runner);
    tab->socFlasher()->setPollIntervalMs(10);
    const QString image = tempPath(QStringLiteral("flash-partitions/update.img"));
    QVERIFY(writeFile(image, rkfwBytes()));
    tab->setImagePath(image);
    QVERIFY(waitForTable(*tab));
    tab->setSelectedPartitions({QStringLiteral("boot"), QStringLiteral("rootfs")});

    QSignalSpy finished(tab.get(), &FirmwareTab::operationFinished);
    tab->flashSoc();
    QVERIFY(tab->isBusy());
    QVERIFY(!tab->partitionTable()->isEnabled());   // no changes while the flash runs
    QVERIFY(!tab->partitionsAllButton()->isEnabled());
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), qsizetype(1), kOpTimeoutMs);
    QCOMPARE(finished.at(0).at(0).toString(), QStringLiteral("flash"));
    QVERIFY2(finished.at(0).at(1).toBool(), qPrintable(finished.at(0).at(2).toString()));
    const QString dir = extractDirOf(image);
    QCOMPARE(wlCommands(runner), (QStringList{expectedWl(4, dir), expectedWl(5, dir)}));
    QCOMPARE(runner.count(QRegularExpression(QStringLiteral("^upgrade_tool RD$"))), 1);
    QCOMPARE(runner.count(QRegularExpression(QStringLiteral("^upgrade_tool UF "))), 0);
    QCOMPARE(runner.count(exfPattern()), 1);   // the table's plan was current: not unpacked again
    QCOMPARE(tab->socFlasher()->selectedPartitions(), (QStringList{QStringLiteral("boot"), QStringLiteral("rootfs")}));
    QCOMPARE(tab->socFlasher()->flashMethod(), SocFlasher::FlashMethod::PartitionWrites);
    const QString log = tab->logView()->toPlainText();
    QVERIFY(log.contains(QStringLiteral("Partitions checked: boot, rootfs")));
    QVERIFY(log.contains(QStringLiteral("Loader mode: writing partition by partition with upgrade_tool WL")));
    QVERIFY(log.contains(QStringLiteral("writing 2 of 7 partitions")));
    QVERIFY(log.contains(QStringLiteral("skipping env: not selected")));
    QVERIFY(tab->partitionTable()->isEnabled());
    QCOMPARE(tab->selectedPartitions(), (QStringList{QStringLiteral("boot"), QStringLiteral("rootfs")}));   // the checks stay

    // Nothing checked: refused at once, nothing runs.
    tab->partitionsNoneButton()->click();
    finished.clear();
    QTRY_VERIFY(runner.liveInvocations().isEmpty());
    const qsizetype before = runner.invocations().size();
    tab->flashSoc();
    QCOMPARE(finished.count(), qsizetype(1));
    QCOMPARE(finished.at(0).at(1).toBool(), false);
    QVERIFY2(finished.at(0).at(2).toString().contains(QStringLiteral("no partition is checked")), qPrintable(finished.at(0).at(2).toString()));
    QCOMPARE(runner.invocations().size(), before);
}

void Tst_firmwaretab::flashSocMaskromIgnoresSelection()
{
    // MASKROM mode: UF writes the whole image; the checks do not apply, and the log says so.
    FakeToolRunner runner;
    scriptSocFlash(runner, kLdMaskrom);
    AppSettings::instance().setFirmwareMtdParts(QString(), kNodeMtdParts);
    auto tab = makeTab(&runner);
    tab->socFlasher()->setPollIntervalMs(10);
    const QString image = tempPath(QStringLiteral("flash-maskrom/update.img"));
    QVERIFY(writeFile(image, rkfwBytes()));
    tab->setImagePath(image);
    QVERIFY(waitForTable(*tab));
    tab->setSelectedPartitions({QStringLiteral("boot")});
    QSignalSpy finished(tab.get(), &FirmwareTab::operationFinished);
    tab->flashSoc();
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), qsizetype(1), kOpTimeoutMs);
    QVERIFY2(finished.at(0).at(1).toBool(), qPrintable(finished.at(0).at(2).toString()));
    QCOMPARE(runner.count(QRegularExpression(QStringLiteral("^upgrade_tool UF "))), 1);
    QVERIFY(wlCommands(runner).isEmpty());
    QCOMPARE(tab->socFlasher()->flashMethod(), SocFlasher::FlashMethod::UpgradeFirmware);
    const QString log = tab->logView()->toPlainText();
    QVERIFY(log.contains(QStringLiteral("Partitions checked: boot")));
    QVERIFY(log.contains(QStringLiteral("MASKROM mode: writing the whole image with upgrade_tool UF")));
    QVERIFY(log.contains(QStringLiteral("the partition selection (boot) is ignored: UF writes every partition")));
}

void Tst_firmwaretab::pullAndFlashWritesEveryPartition()
{
    // Pull & Flash in Loader mode writes every partition, whatever was checked before.
    SshProfileStore store;
    BuildMachine machine;
    QVERIFY(machine.start(&store, tempPath(QStringLiteral("pull-flash-partitions"))));
    FakeToolRunner runner;
    scriptSocFlash(runner, kLdLoader);
    AppSettings::instance().setFirmwareMtdParts(QString(), kNodeMtdParts);
    auto tab = makeTab(&runner, &store);
    tab->socFlasher()->setPollIntervalMs(10);
    tab->setSourceTarget(QStringLiteral("ssh:profile:") + machine.profile.id);
    tab->setRemoteFolder(machine.imageFolder);
    const QString cache = tempPath(QStringLiteral("pull-flash-partitions-cache"));
    tab->setCacheDirectory(cache);
    QSignalSpy finished(tab.get(), &FirmwareTab::operationFinished);
    ModalDismisser unexpected(ModalDismisser::Answer::Reject);
    tab->pull();
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), qsizetype(1), kOpTimeoutMs);
    QVERIFY2(finished.at(0).at(1).toBool(), qPrintable(finished.at(0).at(2).toString()));
    QVERIFY(waitForTable(*tab));
    tab->setSelectedPartitions({QStringLiteral("boot")});

    finished.clear();
    tab->pullAndFlash();
    QTRY_VERIFY_WITH_TIMEOUT(!finished.isEmpty() && finished.last().at(0).toString() == QLatin1String("pull-flash"), 60000);
    unexpected.stop();
    QCOMPARE(unexpected.count(), 0);
    for (const QList<QVariant>& args : std::as_const(finished)) {
        QVERIFY2(args.at(1).toBool(), qPrintable(args.at(0).toString() + QStringLiteral(": ") + args.at(2).toString()));
    }
    const QString dir = extractDirOf(cache + QStringLiteral("/update.img"));
    QStringList expected;
    for (int k = 0; k < 7; ++k) {
        expected << expectedWl(k, dir);
    }
    QCOMPARE(wlCommands(runner), expected);
    QCOMPARE(runner.count(QRegularExpression(QStringLiteral("^upgrade_tool RD$"))), 1);
    QCOMPARE(runner.count(exfPattern()), 1);   // unpacked once, after the first pull
    QCOMPARE(tab->selectedPartitions(), nodeNames());
    QVERIFY(tab->socFlasher()->selectedPartitions().isEmpty());
    QVERIFY(tab->logView()->toPlainText().contains(QStringLiteral("Partitions: every partition of the image")));
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

    // Quitting asks once (a pull is no flash: it may be interrupted); No keeps the window and the
    // operation. The question names what an interruption leaves behind on the board.
    {
        ModalDismisser no(ModalDismisser::Answer::No);
        w.close();
        no.stop();
        QCOMPARE(no.count(), 1);
        QVERIFY(no.windowTitles().first().startsWith(QStringLiteral("Quit")));
        QVERIFY2(no.texts().first().contains(QStringLiteral("UART updater")), qPrintable(no.texts().first()));
        QVERIFY(no.texts().first().contains(QStringLiteral("Quit anyway?")));
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

void Tst_firmwaretab::quitRefusedWhileUpgradeToolWrites()
{
    // The review's BLOCKER: "Quit anyway?" during UF destroyed the tab and its ProcessToolRunner,
    // which kills upgrade_tool half way through the flash. Now quitting is refused outright while
    // the flasher is in Flashing - no question that could be answered with Quit. MainWindow's tab
    // runs real processes, so a script stands in for upgrade_tool (writeFakeUpgradeTool()).
    const QString toolDir = tempPath(QStringLiteral("fake-upgrade-tool"));
    QVERIFY(QDir().mkpath(toolDir));
    QFile::remove(toolDir + QStringLiteral("/release"));
    const QString tool = writeFakeUpgradeTool(toolDir);
    QVERIFY(!tool.isEmpty());

    MainWindow w;
    QVERIFY(showAndActivate(w));
    FirmwareTab* tab = openFirmwareTab(w);
    QVERIFY(tab);
    tab->setToolPaths(tool, QStringLiteral("adb-not-installed"));   // the ADB wait then ends at once
    const QString cache = tempPath(QStringLiteral("quit-flash-cache"));
    QVERIFY(writeFile(cache + QStringLiteral("/update.img"), rkfwBytes()));
    tab->setCacheDirectory(cache);
    QSignalSpy finished(tab, &FirmwareTab::operationFinished);
    tab->flashSoc();
    QTRY_COMPARE_WITH_TIMEOUT(tab->socFlasher()->phase(), SocFlasher::Phase::Flashing, kOpTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(tab->logView()->toPlainText().contains(QStringLiteral("Download Image...")), kOpTimeoutMs);

    // Alt+F4 / the close button, and File > Quit: refused with a status message, no question.
    ModalDismisser yes(ModalDismisser::Answer::Yes);   // would answer "Quit" if anything asked
    w.close();
    QVERIFY2(w.statusBar()->currentMessage().contains(QStringLiteral("upgrade_tool is writing the flash")),
             qPrintable(w.statusBar()->currentMessage()));
    action(w, "actionQuit")->trigger();
    QTest::qWait(100);
    yes.stop();
    QCOMPARE(yes.count(), 0);
    QVERIFY(w.isVisible());
    QCOMPARE(w.firmwareTab(), tab);
    QVERIFY(tab->isBusy());
    QCOMPARE(tab->socFlasher()->phase(), SocFlasher::Phase::Flashing);
    QVERIFY(finished.isEmpty());

    // upgrade_tool was left alone: it ends on its own and reports success.
    QVERIFY(writeFile(toolDir + QStringLiteral("/release"), "go"));
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), qsizetype(1), kOpTimeoutMs);
    QCOMPARE(finished.at(0).at(0).toString(), QStringLiteral("flash"));
    QVERIFY2(finished.at(0).at(1).toBool(), qPrintable(finished.at(0).at(2).toString()));
    QVERIFY(tab->logView()->toPlainText().contains(QStringLiteral("Upgrade firmware ok.")));
    QVERIFY(!tab->isBusy());

    // Idle again: the window closes without a question.
    ModalDismisser none(ModalDismisser::Answer::Reject);
    w.close();
    none.stop();
    QCOMPARE(none.count(), 0);
    QVERIFY(!w.isVisible());
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
//   SU_FIRMWARE_REAL_SKIP_SOC=1    run Pull alone instead of Pull & Flash (a board whose loader
//                                  cannot be flashed by upgrade_tool yet; the MCU step still runs)
//   SU_FIRMWARE_REAL_MCU_ACTION    "update" (default: Update MCU with the package), "set-role" (Set role
//                                  to the role the MCU reported: the updater round trip without a new
//                                  image, for a board whose package is not at hand) or "none"
//   SU_FIRMWARE_REAL_PARTITIONS    comma-separated partition names, e.g. "boot,rootfs" (v0.5.1; ignored
//                                  with SKIP_SOC=1): instead of Pull & Flash, Pull (the cache is the
//                                  test's own temporary folder, so the image has to be pulled first),
//                                  wait for the partition table, check exactly these partitions
//                                  (FirmwareTab::setSelectedPartitions()), log every row of
//                                  partitionTable() (name, checked, offset, size) and run Flash SoC on
//                                  that image - Loader mode writes only these, MASKROM mode the whole image
// What it does, in this order: Read (MCU version / role), Pull & Flash (pull update.img, flash the
// SoC - Loader mode: EXF, one upgrade_tool WL per partition, RD; MASKROM mode: UF -, wait for ADB,
// update the MCU only when the package version differs), then Update MCU with the package
// explicitly. The host key is accepted into the test's own known_hosts file; nothing is remembered
// in the user's settings.
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
    //    step runs only when the package version differs from the running one). With
    //    SU_FIRMWARE_REAL_SKIP_SOC=1 only the pull runs; with SU_FIRMWARE_REAL_PARTITIONS the pull,
    //    then Flash SoC with exactly those partitions checked.
    const bool skipSoc = qEnvironmentVariable("SU_FIRMWARE_REAL_SKIP_SOC") == QLatin1String("1");
    const QStringList partitions =
        skipSoc ? QStringList()
                : qEnvironmentVariable("SU_FIRMWARE_REAL_PARTITIONS").split(QLatin1Char(','), Qt::SkipEmptyParts);
    const bool partitionRun = !partitions.isEmpty();
    const QString chainName = skipSoc || partitionRun ? QStringLiteral("pull") : QStringLiteral("pull-flash");
    const qsizetype before = finished.count();
    if (skipSoc || partitionRun) {
        tab->pull();
    } else {
        tab->pullAndFlash();
    }
    QVERIFY(tab->isBusy());
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() > before && finished.last().at(0).toString() == chainName,
                             15 * 60 * 1000);
    for (qsizetype i = before; i < finished.count(); ++i) {
        qInfo() << "real run: step" << finished.at(i).at(0).toString() << finished.at(i).at(1).toBool()
                << finished.at(i).at(2).toString();
    }
    qInfo() << "real run: dialogs" << answerer.seen();
    QVERIFY2(finished.last().at(1).toBool(), qPrintable(finished.last().at(2).toString()));
    QVERIFY(!tab->isBusy());

    if (partitionRun) {
        // The table of the pulled image (EXF), in the layout in use: the image's env.img, else the
        // one read from the running board in step 1.
        QTRY_VERIFY_WITH_TIMEOUT(tab->partitionTable()->rowCount() > 0
                                     && !tab->partitionsLabel()->text().startsWith(QStringLiteral("unpacking")),
                                 5 * 60 * 1000);
        qInfo() << "real run: flash layout" << tab->flashLayout() << "-" << tab->partitionsLabel()->text() << "-"
                << tab->partitionsLabel()->toolTip();
        QVERIFY2(!tab->flashLayout().isEmpty(), "no flash layout: neither the image's env.img nor the board's /proc/cmdline has mtdparts");
        QStringList trimmed;
        for (const QString& name : partitions) {
            trimmed << name.trimmed();
        }
        tab->setSelectedPartitions(trimmed);
        QTableWidget* table = tab->partitionTable();
        for (int row = 0; row < table->rowCount(); ++row) {
            const QTableWidgetItem* check = table->item(row, 0);
            qInfo().noquote() << "real run: partition" << table->item(row, 1)->text()
                              << (check->checkState() == Qt::Checked ? "checked" : "-") << "offset"
                              << table->item(row, 3)->text() << "size" << table->item(row, 4)->text() << "image"
                              << table->item(row, 5)->text() << check->toolTip();
        }
        QCOMPARE(tab->selectedPartitions(), trimmed);   // every name is a row that can be written
        const qsizetype beforeFlash = finished.count();
        tab->flashSoc();
        QVERIFY(tab->isBusy());
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() > beforeFlash, 15 * 60 * 1000);
        qInfo() << "real run: flash SoC ->" << finished.last().at(0).toString() << finished.last().at(1).toBool()
                << finished.last().at(2).toString() << "method" << tab->socFlasher()->flashMethod();
        QVERIFY2(finished.last().at(1).toBool(), qPrintable(finished.last().at(2).toString()));
        QVERIFY(!tab->isBusy());
    }

    // 3. The MCU path once explicitly: Update MCU with the package (the full CC1310 transfer and
    //    restart), or Set role to the reported role (bootloader entry, SET_ROLE, restart, no new image).
    const QString mcuAction = qEnvironmentVariable("SU_FIRMWARE_REAL_MCU_ACTION", QStringLiteral("update"));
    if (mcuAction == QLatin1String("update") && !package.isEmpty()) {
        const qsizetype beforeMcu = finished.count();
        tab->updateMcu();
        QVERIFY(tab->isBusy());
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() > beforeMcu, 5 * 60 * 1000);
        qInfo() << "real run: update MCU ->" << finished.last().at(0).toString() << finished.last().at(1).toBool()
                << finished.last().at(2).toString();
        QVERIFY2(finished.last().at(1).toBool(), qPrintable(finished.last().at(2).toString()));
    } else if (mcuAction == QLatin1String("set-role")) {
        const qsizetype beforeMcu = finished.count();
        qInfo() << "real run: set role" << tab->selectedRole() << "(the role the MCU reported)";
        tab->setMcuRole();
        QVERIFY(tab->isBusy());
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() > beforeMcu, 5 * 60 * 1000);
        qInfo() << "real run: set role ->" << finished.last().at(0).toString() << finished.last().at(1).toBool()
                << finished.last().at(2).toString();
        QVERIFY2(finished.last().at(1).toBool(), qPrintable(finished.last().at(2).toString()));
    }
    answerer.stop();
    dumpLog();
}

QTEST_MAIN(Tst_firmwaretab)
#include "tst_firmwaretab.moc"
