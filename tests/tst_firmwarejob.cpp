// UI-free test suite for FirmwareJob (src/firmware/FirmwareJob.h): the Firmware tab's chain without
// any widget - pull, flash, mcu, set-role, info and pull-flash - on a FakeToolRunner (no adb, no
// upgrade_tool is ever started), the in-process TestSshServer as the build machine and FakeMcu
// behind a FakeMcuServer as the CC1310 at the end of `rfmcu bridge`. The decisions the tab answers
// with dialogs are answered by the callbacks here, or left unanswered to check the headless
// defaults su-fw relies on. QSettings and every file live in temporary directories.
#include <QtTest>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QRegularExpression>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>

#include <memory>
#include <optional>

#include "app/AppSettings.h"
#include "app/Logging.h"
#include "firmware/CcProtocol.h"
#include "firmware/FirmwareJob.h"
#include "firmware/McuLink.h"
#include "firmware/McuUpdater.h"
#include "firmware/SocFlasher.h"
#include "firmware/ToolRunner.h"
#include "ssh/SshConnection.h"
#include "ssh/SshProfile.h"
#include "support/FakeMcu.h"
#include "support/FakeToolRunner.h"
#include "support/FirmwareFixtures.h"
#include "support/TestSshServer.h"

using namespace FirmwareFixtures;

namespace {

constexpr int kOpTimeoutMs = 20000;   ///< an operation against fakes / the in-process server
constexpr int kChainTimeoutMs = 60000;

QString dataFile(const char* name)
{
    return QStringLiteral(SU_TEST_DATA_DIR "/firmware/") + QLatin1String(name);
}

QString op(QLatin1StringView name)
{
    return QString(name);
}

/// One stepFinished() / operationFinished().
struct Result
{
    QString name;
    bool ok = false;
    QString message;
};

/// Everything a job reports, in order.
class Recorder : public QObject
{
public:
    explicit Recorder(FirmwareJob* job)
    {
        connect(job, &FirmwareJob::logLine, this, [this](const QString& line) { log << line; });
        connect(job, &FirmwareJob::toolOutput, this, [this](const QString& line) { tool << line; });
        connect(job, &FirmwareJob::progress, this, [this](const QString& stage, int percent) { progress.append({stage, percent}); });
        connect(job, &FirmwareJob::persistentStatusChanged, this, [this](const QString& text) { persistent << text; });
        connect(job, &FirmwareJob::busyChanged, this, [this](bool busy) { busyChanges << busy; });
        connect(job, &FirmwareJob::busyRefused, this, [this](const QString& message) { busyRefusals << message; });
        connect(job, &FirmwareJob::cancelRefused, this, [this](const QString& message) { cancelRefusals << message; });
        connect(job, &FirmwareJob::cancellableChanged, this, [this](bool on) { cancellable << on; });
        connect(job, &FirmwareJob::stepFinished, this, [this](const QString& step, bool ok, const QString& message) {
            steps.append({step, ok, message});
            order << QStringLiteral("step:") + step;
        });
        connect(job, &FirmwareJob::operationFinished, this, [this](const QString& name, bool ok, const QString& message) {
            operations.append({name, ok, message});
            order << QStringLiteral("op:") + name;
        });
    }

    QString logText() const { return log.join(QLatin1Char('\n')); }
    bool waitForOperation(int count = 1, int timeoutMs = kOpTimeoutMs) const
    {
        return QTest::qWaitFor([&]() { return operations.size() >= count; }, timeoutMs);
    }
    Result lastOperation() const { return operations.isEmpty() ? Result() : operations.last(); }
    QStringList stepNames() const
    {
        QStringList names;
        for (const Result& r : steps) {
            names << r.name;
        }
        return names;
    }

    QStringList log;
    QStringList tool;
    QList<QPair<QString, int>> progress;
    QStringList persistent;
    QList<bool> busyChanges;
    QStringList busyRefusals;
    QStringList cancelRefusals;
    QList<bool> cancellable;
    QList<Result> steps;
    QList<Result> operations;
    QStringList order;
};

/// Every Qt message of the process while it lives (the System Log sees them all), for the secret
/// check; the SSH worker thread logs too, hence the mutex.
class MessageCapture
{
public:
    MessageCapture()
    {
        const QMutexLocker lock(&s_mutex);
        s_messages.clear();
        m_previous = qInstallMessageHandler([](QtMsgType, const QMessageLogContext&, const QString& message) {
            const QMutexLocker locker(&s_mutex);
            s_messages.append(message);
        });
    }
    ~MessageCapture() { qInstallMessageHandler(m_previous); }
    QStringList messages() const
    {
        const QMutexLocker lock(&s_mutex);
        return s_messages;
    }

private:
    static inline QMutex s_mutex;
    static inline QStringList s_messages;
    QtMessageHandler m_previous = nullptr;
};

/// A job as the tests use it: fake tools with plain names, fast polls.
std::unique_ptr<FirmwareJob> makeJob(FakeToolRunner* runner, SshProfileStore* store = nullptr)
{
    auto job = std::make_unique<FirmwareJob>(runner);
    job->setProfileStore(store);
    job->setToolPaths(QStringLiteral("upgrade_tool"), QStringLiteral("adb"));
    job->socFlasher()->setPollIntervalMs(10);
    return job;
}

/// `upgrade_tool LD` and `adb` see nothing: the flasher waits for Loader mode.
void scriptNoBoard(FakeToolRunner& runner)
{
    runner.addScript(QRegularExpression(QStringLiteral("^upgrade_tool LD")), "List of rockusb connected(0)\r\n", 0);
    runner.addScript(QRegularExpression(QStringLiteral("^adb( -s \\S+)? devices")), "List of devices attached\r\n\r\n", 0);
    runner.addScript(QRegularExpression(QStringLiteral("^adb( -s \\S+)? get-state")), "error: no devices/emulators found\r\n", 1);
}

/// The flash of a MASKROM board through UF, plus the rest of scriptSocFlash().
void scriptMaskrom(FakeToolRunner& runner)
{
    scriptSocFlash(runner, kLdMaskrom);
}

bool waitForPlan(const FirmwareJob& job)
{
    return QTest::qWaitFor([&]() { return job.planState() == FirmwareJob::PlanState::Ready && job.partitions().size() == 7; },
                           kOpTimeoutMs);
}

/// The lock every firmware operation holds (FirmwareJob.h, "One firmware operation per machine").
QString firmwareLockPath()
{
    return AppSettings::dataDirectory() + QStringLiteral("/firmware.lock");
}

/// Whether anybody holds the lock right now (a fresh QLockFile can take it).
bool lockIsFree()
{
    QLockFile probe(firmwareLockPath());
    probe.setStaleLockTime(0);
    if (!probe.tryLock(0)) {
        return false;
    }
    probe.unlock();
    return true;
}

/// The refusal a job gives while `held` (a QLockFile of this process) holds the lock.
QString runningText(const QLockFile& held)
{
    qint64 pid = 0;
    QString host;
    QString program;
    if (!held.getLockInfo(&pid, &host, &program)) {
        return QStringLiteral("<no lock info>");
    }
    return QStringLiteral("another firmware operation is running (%1)")
        .arg(program.isEmpty() ? QStringLiteral("pid %1").arg(pid) : QStringLiteral("%1, pid %2").arg(program).arg(pid));
}

} // namespace

// =======================================================================================
// Test class
// =======================================================================================

class Tst_firmwarejob : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();

    // ---- The object and its inputs -------------------------------------------------------
    void namesTitlesAndRoles();
    void ownsItsCollaborators();
    void inputsReachTheCollaborators();
    void imageFromCacheOrLocalPick();
    void imagePathRefusals();
    void packageLoadedAndRefused();
    void setPackageWithoutLogLine();
    void roleIsConfirmedNotGuessed();
    void rollBackSwapsTheCache();
    void partitionProblemTexts();

    // ---- Framing: busy, refusals, names --------------------------------------------------
    void refuseIfBusyEveryOperation();
    void singleStepFraming();
    void inputsCheckedFirst();
    void persistentStatusWhileBusy();

    // ---- One firmware operation per machine (the lock) ------------------------------------
    void secondJobRefusedWhileTheFirstIsBusy();
    void refreshStatusPausedWhileTheLockIsHeld();
    void lockReleasedOnEveryEndPath();

    // ---- Pull ----------------------------------------------------------------------------
    void pullTargetErrors();
    void pullDownloadsThenSkipsUnchanged();
    void pullHostKeyAndAuthCallbacks();
    void hostKeyRefusedByTheHandler();
    void headlessHostKeyRefused();
    void headlessPromptFailsWithItsText();
    void authHandlerCancels();
    void noSecretInTheLog();
    void adHocTargetUsesSshDefaults();
    void cancelWhileConnecting();
    void disconnectSourceCancelsThePull();

    // ---- Flash SoC -----------------------------------------------------------------------
    void flashLoaderWritesSelectedRows();
    void flashLoaderWritesEveryRowByDefault();
    void flashMaskromWritesWholeImage();
    void sizeCheckStopsBeforeAnyWrite();
    void noPartitionSelectedIsRefused();
    void layoutUnknownInLoaderMode();
    void cancelWhileWaitingForLoader();
    void cancelRefusedWhileWriting();
    void downloadModeHeadless();
    void downloadModeSentThroughSession();
    void downloadModeMootWhenBoardAppeared();
    void downloadModeNothingToAsk();

    // ---- Status, layout, partitions --------------------------------------------------------
    void statusReadsTheLayoutOnce();
    void layoutReadFinishedOnBothEnds();
    void layoutProviderAndOwnMemory();
    void layoutFromTheImage();
    void refreshStatusSkippedWhileBusy();
    void planStatesAndSignals();
    void selectionApi();

    // ---- MCU -------------------------------------------------------------------------------
    void readMcuInfoStopsStaleRfmcuFirst();
    void readMcuInfoRoleFromHelp();
    void readMcuInfoFailures();
    void cancelDuringMcuRead();
    void updateMcuThroughFakeBridge();
    void setRoleThroughFakeBridge();
    void mcuLinkFailureEndsTheStep();
    void destroyedJobCleansUpTheBoard();

    // ---- Pull & Flash ------------------------------------------------------------------------
    void pullFlashSkipsMcuWhenEqual();
    void pullFlashUpdatesMcuWhenDifferent();
    void pullFlashUpdatesSilentMcu();
    void pullFlashSilentMcuWithoutRoleStops();
    void pullFlashStopsAtFirstFailure();
    void pullFlashWithoutPackage();
    void pullFlashWritesEveryPartition();

private:
    QString tempPath(const QString& name) const { return m_tempDir.filePath(name); }
    QString caseDir(const QString& name) const { return tempPath(QStringLiteral("case%1-%2").arg(m_caseNumber).arg(name)); }

    QTemporaryDir m_tempDir;
    int m_caseNumber = 0;
};

// =======================================================================================
// Fixture
// =======================================================================================

void Tst_firmwarejob::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("BuildAI-Test"));
    QCoreApplication::setApplicationName(QStringLiteral("SerialUtilityTest-firmwarejob"));
    QVERIFY(m_tempDir.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_tempDir.filePath(QStringLiteral("settings")));
    QSettings().clear();
    QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));   // "78,040" whatever the machine's locale
    qputenv("SU_SSH_IGNORE_CONFIG", "1");   // the SSH connects of this suite must not read ~/.ssh/config
    qunsetenv("SSH_AUTH_SOCK");             // nor offer the developer's agent keys to the test server
}

void Tst_firmwarejob::init()
{
    QSettings().clear();
    ++m_caseNumber;
}

// =======================================================================================
// The object and its inputs
// =======================================================================================

void Tst_firmwarejob::namesTitlesAndRoles()
{
    // The op / step names the tab and su-fw share (currentOperation(), stepFinished(), operationFinished()).
    QCOMPARE(op(FirmwareJob::kOpPull), QStringLiteral("pull"));
    QCOMPARE(op(FirmwareJob::kOpFlash), QStringLiteral("flash"));
    QCOMPARE(op(FirmwareJob::kOpMcu), QStringLiteral("mcu"));
    QCOMPARE(op(FirmwareJob::kOpSetRole), QStringLiteral("set-role"));
    QCOMPARE(op(FirmwareJob::kOpInfo), QStringLiteral("info"));
    QCOMPARE(op(FirmwareJob::kOpPullFlash), QStringLiteral("pull-flash"));
    QCOMPARE(FirmwareJob::operationTitle(QStringLiteral("pull")), QStringLiteral("Pull"));
    QCOMPARE(FirmwareJob::operationTitle(QStringLiteral("flash")), QStringLiteral("Flash SoC"));
    QCOMPARE(FirmwareJob::operationTitle(QStringLiteral("mcu")), QStringLiteral("Update MCU"));
    QCOMPARE(FirmwareJob::operationTitle(QStringLiteral("set-role")), QStringLiteral("Set role"));
    QCOMPARE(FirmwareJob::operationTitle(QStringLiteral("info")), QStringLiteral("Read MCU"));
    QCOMPARE(FirmwareJob::operationTitle(QStringLiteral("pull-flash")), QStringLiteral("Pull & Flash"));
    QCOMPARE(FirmwareJob::operationTitle(QStringLiteral("other")), QStringLiteral("other"));
    QCOMPARE(FirmwareJob::resultText(QStringLiteral("flash"), true, QStringLiteral("x")), QStringLiteral("Flash SoC done: x"));
    QCOMPARE(FirmwareJob::resultText(QStringLiteral("pull"), false, QStringLiteral("y")), QStringLiteral("Pull failed: y"));
    QCOMPARE(FirmwareJob::roleText(CcProtocol::RoleRx), QStringLiteral("rx"));
    QCOMPARE(FirmwareJob::roleText(CcProtocol::RoleTx), QStringLiteral("tx"));
    QVERIFY(FirmwareJob::roleText(9).isEmpty());
    QCOMPARE(FirmwareJob::roleFromText(QStringLiteral(" TX ")), static_cast<quint32>(CcProtocol::RoleTx));
    QCOMPARE(FirmwareJob::roleFromText(QStringLiteral("rx")), static_cast<quint32>(CcProtocol::RoleRx));
    QCOMPARE(FirmwareJob::roleFromText(QStringLiteral("unknown")), 0u);
    QVERIFY(FirmwareJob::kProgressBusy < 0 && FirmwareJob::kProgressTextOnly < 0
            && FirmwareJob::kProgressBusy != FirmwareJob::kProgressTextOnly);
}

void Tst_firmwarejob::ownsItsCollaborators()
{
    // No runner given: an own ProcessToolRunner, created after the collaborators (QObject deletes
    // children in creation order: McuLink's destructor still sends the board cleanup through it).
    FirmwareJob plain(nullptr);
    QVERIFY(qobject_cast<ProcessToolRunner*>(plain.toolRunner()) != nullptr);
    QCOMPARE(plain.toolRunner()->parent(), static_cast<QObject*>(&plain));
    const QObjectList kids = plain.children();
    QVERIFY(kids.indexOf(plain.mcuLink()) >= 0);
    QVERIFY(kids.indexOf(plain.toolRunner()) > kids.indexOf(plain.mcuLink()));
    QVERIFY(kids.indexOf(plain.toolRunner()) > kids.indexOf(plain.socFlasher()));

    FakeToolRunner runner;
    FirmwareJob job(&runner);
    QCOMPARE(job.toolRunner(), static_cast<ToolRunner*>(&runner));
    QVERIFY(runner.parent() == nullptr);   // not taken over
    QVERIFY(job.socFlasher() && job.mcuLink() && job.mcuUpdater() && job.imagePuller() && job.sshConnection());
    QCOMPARE(job.socFlasher()->parent(), static_cast<QObject*>(&job));
    QCOMPARE(job.imagePuller()->connection(), job.sshConnection());
    QVERIFY(job.mcuUpdater()->throughSoc());            // "power" is logged before "bootloader"
    QVERIFY(!job.sshConnection()->autoReconnect());     // a dropped pull is not re-established behind the user's back
    QCOMPARE(job.sshConnection()->state(), Transport::State::Disconnected);
    // Idle: nothing runs, nothing to cancel, no lasting status.
    QVERIFY(!job.isBusy());
    QVERIFY(job.currentOperation().isEmpty());
    QVERIFY(job.currentStep().isEmpty());
    QVERIFY(!job.isCancellable());
    QVERIFY(job.persistentStatusMessage().isEmpty());
    QCOMPARE(job.role(), 0u);
    QVERIFY(!job.hasStatus());
    QCOMPARE(job.planState(), FirmwareJob::PlanState::NoImage);
}

void Tst_firmwarejob::inputsReachTheCollaborators()
{
    FakeToolRunner runner;
    FirmwareJob job(&runner);
    job.setToolPaths(QStringLiteral("C:/tools/upgrade_tool.exe"), QStringLiteral("C:/tools/adb.exe"));
    QCOMPARE(job.upgradeToolPath(), QStringLiteral("C:/tools/upgrade_tool.exe"));
    QCOMPARE(job.adbPath(), QStringLiteral("C:/tools/adb.exe"));
    QCOMPARE(job.socFlasher()->upgradeTool()->toolPath(), QStringLiteral("C:/tools/upgrade_tool.exe"));
    QCOMPARE(job.socFlasher()->adb()->adbPath(), QStringLiteral("C:/tools/adb.exe"));
    QCOMPARE(job.mcuLink()->adb()->adbPath(), QStringLiteral("C:/tools/adb.exe"));

    job.setSerial(QStringLiteral(" c3d9 "));
    QCOMPARE(job.serial(), QStringLiteral("c3d9"));
    QCOMPARE(job.socFlasher()->serial(), QStringLiteral("c3d9"));
    QCOMPARE(job.mcuLink()->serial(), QStringLiteral("c3d9"));

    job.setLoaderWaitSeconds(45);
    job.setAdbReturnSeconds(90);
    QCOMPARE(job.loaderWaitSeconds(), 45);
    QCOMPARE(job.socFlasher()->waitTimeoutSeconds(), 45);
    QCOMPARE(job.adbReturnSeconds(), 90);
    QCOMPARE(job.socFlasher()->adbReturnTimeoutSeconds(), 90);

    job.setSourceTarget(QStringLiteral("  ssh:target:root@10.0.0.24 "));
    QCOMPARE(job.sourceTarget(), QStringLiteral("ssh:target:root@10.0.0.24"));
    job.setRemoteFolder(QStringLiteral(" ~/rk/out/image "));
    QCOMPARE(job.remoteFolder(), QStringLiteral("~/rk/out/image"));
    // The platform's own separators plus a stray blank: the job stores the trimmed Qt form.
    job.setCacheDirectory(QDir::toNativeSeparators(QStringLiteral("D:/fw/cache")) + QLatin1Char(' '));
    QCOMPARE(job.cacheDirectory(), QStringLiteral("D:/fw/cache"));
    QVERIFY(job.gitDescribe());
    job.setGitDescribe(false);
    QVERIFY(!job.gitDescribe());
    job.setExtraFiles({QStringLiteral("sd_update.txt")});
    QCOMPARE(job.extraFiles(), QStringList{QStringLiteral("sd_update.txt")});
    FirmwareJob::SshDefaults defaults;
    defaults.knownHostsFile = QStringLiteral("kh");
    defaults.identityFile = QStringLiteral("id");
    defaults.terminalType = QStringLiteral("vt100");
    defaults.keepAliveSeconds = 0;
    job.setSshDefaults(defaults);
    QCOMPARE(job.sshDefaults().knownHostsFile, QStringLiteral("kh"));
    QCOMPARE(job.sshDefaults().keepAliveSeconds, 0);
    SshProfileStore store;
    job.setProfileStore(&store);
    QCOMPARE(job.profileStore(), &store);
    QVERIFY(runner.invocations().isEmpty());   // inputs alone start nothing
}

void Tst_firmwarejob::imageFromCacheOrLocalPick()
{
    FakeToolRunner runner;
    auto job = makeJob(&runner);
    QVERIFY(job->imagePath().isEmpty());
    const QString cache = caseDir(QStringLiteral("cache"));
    job->setCacheDirectory(cache);
    QVERIFY(job->imagePath().isEmpty());   // no update.img yet
    QVERIFY(!job->cacheEntry().isValid());
    QVERIFY(writeFile(cache + QStringLiteral("/update.img"), rkfwBytes()));
    QCOMPARE(job->imagePath(), cache + QStringLiteral("/update.img"));
    QVERIFY(job->localImage().isEmpty());

    // A local pick wins over the cache; "" goes back to it. useLocalImage() logs nothing.
    Recorder rec(job.get());
    const QString local = caseDir(QStringLiteral("local/other.img"));
    QVERIFY(writeFile(local, rkfwBytes()));
    job->useLocalImage(local);
    QCOMPARE(job->imagePath(), local);
    QCOMPARE(job->localImage(), local);
    job->useLocalImage(QString());
    QCOMPARE(job->imagePath(), cache + QStringLiteral("/update.img"));
    QVERIFY(rec.log.isEmpty());
}

void Tst_firmwarejob::imagePathRefusals()
{
    FakeToolRunner runner;
    runner.addHandler(exfPattern(), &nodeExtractor);
    auto job = makeJob(&runner);
    Recorder rec(job.get());
    QSignalSpy changed(job.get(), &FirmwareJob::imageChanged);

    QString message;
    const QString missing = caseDir(QStringLiteral("missing.img"));
    QVERIFY(!job->setImagePath(missing, &message));
    QCOMPARE(message, QStringLiteral("Image %1 refused: file not found").arg(QDir::toNativeSeparators(missing)));
    QCOMPARE(rec.log.last(), message);
    const QString wrong = caseDir(QStringLiteral("wrong.img"));
    QVERIFY(writeFile(wrong, QByteArray("ABCD") + QByteArray(1024, '\0')));
    QVERIFY(!job->setImagePath(wrong, &message));
    QVERIFY2(message.startsWith(QStringLiteral("Image %1 refused: ").arg(QDir::toNativeSeparators(wrong))), qPrintable(message));
    QVERIFY(job->imagePath().isEmpty());
    QCOMPARE(changed.count(), qsizetype(0));

    const QString good = caseDir(QStringLiteral("good/update.img"));
    QVERIFY(writeFile(good, rkfwBytes()));
    QVERIFY(job->setImagePath(good, &message));
    QCOMPARE(job->imagePath(), good);
    QVERIFY(rec.log.contains(QStringLiteral("Using the local image %1").arg(QDir::toNativeSeparators(good))));
    QCOMPARE(changed.count(), qsizetype(1));
    QCOMPARE(changed.at(0).at(0).toBool(), false);
    QCOMPARE(job->planState(), FirmwareJob::PlanState::Planning);   // a new image is unpacked at once

    QVERIFY(job->setImagePath(QString()));   // back to the cache (empty here)
    QVERIFY(job->imagePath().isEmpty());
    QCOMPARE(job->planState(), FirmwareJob::PlanState::NoImage);
}

void Tst_firmwarejob::packageLoadedAndRefused()
{
    FakeToolRunner runner;
    auto job = makeJob(&runner);
    Recorder rec(job.get());
    QSignalSpy changed(job.get(), &FirmwareJob::packageChanged);
    const QString pkg = dataFile("firmware-0.8.1.pkg");
    QString message;
    QVERIFY(job->setPackagePath(QDir::toNativeSeparators(pkg), &message));
    QCOMPARE(job->packagePath(), pkg);
    QVERIFY(job->package().isValid());
    QCOMPARE(job->package().versionText(), QStringLiteral("0.8.1"));
    QCOMPARE(job->package().size, 78040u);
    QCOMPARE(rec.log.last(), QStringLiteral("Package firmware-0.8.1.pkg: ") + job->package().summary());
    QCOMPARE(changed.count(), qsizetype(1));

    // A bad file is refused with the reason; the accepted package stays.
    const QString bad = caseDir(QStringLiteral("bad.pkg"));
    QVERIFY(writeFile(bad, QByteArray(64, '\x11')));
    QVERIFY(!job->setPackagePath(bad, &message));
    QVERIFY2(message.startsWith(QStringLiteral("Package %1 refused: ").arg(QFileInfo(bad).fileName())), qPrintable(message));
    QCOMPARE(rec.log.last(), message);
    QCOMPARE(job->packagePath(), pkg);
    QCOMPARE(changed.count(), qsizetype(1));
    QVERIFY(!job->setPackagePath(caseDir(QStringLiteral("missing.pkg")), &message));
    QVERIFY(message.contains(QStringLiteral("missing.pkg")));

    // "" clears it (no log line).
    const qsizetype lines = rec.log.size();
    QVERIFY(!job->setPackagePath(QString()));
    QVERIFY(!job->package().isValid());
    QVERIFY(job->packagePath().isEmpty());
    QCOMPARE(rec.log.size(), lines);
    QCOMPARE(changed.count(), qsizetype(2));
}

void Tst_firmwarejob::setPackageWithoutLogLine()
{
    FakeToolRunner runner;
    auto job = makeJob(&runner);
    Recorder rec(job.get());
    CcPackage loaded;
    QVERIFY(CcPackage::load(dataFile("firmware-0.8.1.pkg"), &loaded));
    job->setPackage(loaded);
    QCOMPARE(job->packagePath(), dataFile("firmware-0.8.1.pkg"));
    QVERIFY(job->package().isValid());
    QVERIFY(rec.log.isEmpty());
    job->setPackage(CcPackage());
    QVERIFY(job->packagePath().isEmpty());
}

void Tst_firmwarejob::roleIsConfirmedNotGuessed()
{
    FakeToolRunner runner;
    auto job = makeJob(&runner);
    QSignalSpy roles(job.get(), &FirmwareJob::roleChanged);
    job->setRole(CcProtocol::RoleTx);
    QCOMPARE(job->role(), static_cast<quint32>(CcProtocol::RoleTx));
    job->setRole(CcProtocol::RoleTx);   // unchanged: no signal
    job->setRole(7);                    // not a role: none
    QCOMPARE(job->role(), 0u);
    QCOMPARE(roles.count(), qsizetype(2));
    QCOMPARE(roles.at(0).at(0).toUInt(), static_cast<uint>(CcProtocol::RoleTx));
    QCOMPARE(roles.at(1).at(0).toUInt(), 0u);
}

void Tst_firmwarejob::rollBackSwapsTheCache()
{
    FakeToolRunner runner;
    auto job = makeJob(&runner);
    Recorder rec(job.get());
    const QString cache = caseDir(QStringLiteral("cache"));
    QVERIFY(writeFile(cache + QStringLiteral("/update.img"), rkfwBytes(4096)));
    job->setCacheDirectory(cache);
    QString message;
    QVERIFY(!job->rollBack(&message));
    QVERIFY2(message.startsWith(QStringLiteral("Roll back failed: ")), qPrintable(message));   // ImagePuller's reason
    QVERIFY(rec.log.contains(message));

    QVERIFY(writeFile(cache + QStringLiteral("/update.prev.img"), rkfwBytes(8192)));
    const QString local = caseDir(QStringLiteral("local.img"));
    QVERIFY(writeFile(local, rkfwBytes()));
    job->useLocalImage(local);
    QSignalSpy changed(job.get(), &FirmwareJob::imageChanged);
    QVERIFY(job->rollBack(&message));
    QCOMPARE(message, QStringLiteral("Rolled back: the previous image is the current one again"));
    QCOMPARE(QFileInfo(cache + QStringLiteral("/update.img")).size(), qint64(8192));
    QCOMPARE(QFileInfo(cache + QStringLiteral("/update.prev.img")).size(), qint64(4096));
    QVERIFY(job->localImage().isEmpty());   // the cache's image is the image again
    QCOMPARE(job->imagePath(), cache + QStringLiteral("/update.img"));
    QCOMPARE(changed.count(), qsizetype(1));
    QCOMPARE(changed.at(0).at(0).toBool(), true);   // same path, new content
}

void Tst_firmwarejob::partitionProblemTexts()
{
    PartitionImage row;
    row.name = QStringLiteral("boot");
    row.file = QStringLiteral("/x/boot.img");
    row.fileSize = 3145729;
    row.size = 3145728;
    row.inLayout = true;
    row.fits = false;
    QCOMPARE(FirmwareJob::partitionProblem(row, false), QStringLiteral("partition layout unknown: connect the board running once"));
    QCOMPARE(FirmwareJob::partitionProblem(row, true), QStringLiteral("boot.img (3145729 bytes) does not fit the partition (3145728 bytes)"));
    row.fits = true;
    QVERIFY(FirmwareJob::partitionProblem(row, true).isEmpty());
    row.fileSize = -1;
    QCOMPARE(FirmwareJob::partitionProblem(row, true), QStringLiteral("the package has no image file for boot"));
    row.inLayout = false;
    QCOMPARE(FirmwareJob::partitionProblem(row, true), QStringLiteral("boot is not in the board's partition layout"));
}

// =======================================================================================
// Framing: busy, refusals, names
// =======================================================================================

void Tst_firmwarejob::refuseIfBusyEveryOperation()
{
    FakeToolRunner runner;
    scriptNoBoard(runner);
    auto job = makeJob(&runner);
    const QString cache = caseDir(QStringLiteral("cache"));
    QVERIFY(writeFile(cache + QStringLiteral("/update.img"), rkfwBytes()));
    job->setCacheDirectory(cache);
    Recorder rec(job.get());
    job->flashSoc();
    QVERIFY(job->isBusy());
    QCOMPARE(job->currentOperation(), op(FirmwareJob::kOpFlash));
    QCOMPARE(job->currentStep(), op(FirmwareJob::kOpFlash));
    QCOMPARE(rec.busyChanges, QList<bool>{true});

    // Every operation is refused while one runs: busyRefused(), nothing else.
    job->pull();
    job->flashSoc();
    job->updateMcu();
    job->setMcuRole();
    job->readMcuInfo();
    job->pullAndFlash();
    QString message = QStringLiteral("untouched");
    QVERIFY(!job->rollBack(&message));
    QCOMPARE(message, QStringLiteral("untouched"));
    QCOMPARE(rec.busyRefusals.size(), qsizetype(7));
    for (const QString& refusal : std::as_const(rec.busyRefusals)) {
        QCOMPARE(refusal, QStringLiteral("Busy: Flash SoC is still running"));
    }
    QCOMPARE(rec.busyChanges, QList<bool>{true});
    QCOMPARE(job->currentOperation(), op(FirmwareJob::kOpFlash));
    QVERIFY(rec.operations.isEmpty());

    job->cancel();
    QVERIFY(rec.waitForOperation());
    QCOMPARE(rec.lastOperation().message, QStringLiteral("cancelled"));
    QCOMPARE(rec.busyChanges, (QList<bool>{true, false}));
    job->cancel();   // idle: nothing
    QCOMPARE(rec.operations.size(), qsizetype(1));
}

void Tst_firmwarejob::singleStepFraming()
{
    // One step: busyChanged(true), the log's title line, stepFinished(), the result line,
    // busyChanged(false), operationFinished() - in that order, with the same name.
    FakeToolRunner runner;
    auto job = makeJob(&runner);
    Recorder rec(job.get());
    job->updateMcu();   // no package: fails at once
    QCOMPARE(rec.order, (QStringList{QStringLiteral("step:mcu"), QStringLiteral("op:mcu")}));
    QCOMPARE(rec.steps.first().ok, false);
    QCOMPARE(rec.lastOperation().name, QStringLiteral("mcu"));
    QCOMPARE(rec.lastOperation().message, QStringLiteral("choose a valid .pkg package first"));
    QCOMPARE(rec.busyChanges, (QList<bool>{true, false}));
    QCOMPARE(rec.log.first(), QStringLiteral("--- Update MCU ---"));
    QCOMPARE(rec.log.last(), QStringLiteral("Update MCU failed: choose a valid .pkg package first"));
    QCOMPARE(rec.persistent.first(), QStringLiteral("Update MCU..."));
    QCOMPARE(rec.progress.first().first, QStringLiteral("Update MCU"));
    QCOMPARE(rec.progress.first().second, FirmwareJob::kProgressBusy);
    QVERIFY(!job->isBusy());
    QVERIFY(job->persistentStatusMessage().isEmpty());
    QVERIFY(runner.invocations().isEmpty());
}

void Tst_firmwarejob::inputsCheckedFirst()
{
    FakeToolRunner runner;
    auto job = makeJob(&runner);
    Recorder rec(job.get());
    job->flashSoc();
    QCOMPARE(rec.lastOperation().message, QStringLiteral("no image: pull one or choose a local image first"));
    QVERIFY(job->setPackagePath(dataFile("firmware-0.8.1.pkg")));
    job->updateMcu();
    QCOMPARE(rec.lastOperation().message, QStringLiteral("choose the MCU role (rx or tx) first: it is never guessed"));
    job->setMcuRole();
    QCOMPARE(rec.lastOperation().name, QStringLiteral("set-role"));
    QCOMPARE(rec.lastOperation().message, QStringLiteral("choose the MCU role (rx or tx) first: it is never guessed"));
    QCOMPARE(rec.operations.size(), qsizetype(3));
    QCOMPARE(runner.count(QRegularExpression(QStringLiteral("rfmcu|devices|LD"))), 0);   // nothing reached the board
}

void Tst_firmwarejob::persistentStatusWhileBusy()
{
    FakeToolRunner runner;
    scriptNoBoard(runner);
    auto job = makeJob(&runner);
    const QString cache = caseDir(QStringLiteral("cache"));
    QVERIFY(writeFile(cache + QStringLiteral("/update.img"), rkfwBytes()));
    job->setCacheDirectory(cache);
    Recorder rec(job.get());
    job->flashSoc();
    QTRY_COMPARE_WITH_TIMEOUT(job->socFlasher()->phase(), SocFlasher::Phase::WaitingForLoader, kOpTimeoutMs);
    QVERIFY(!job->persistentStatusMessage().isEmpty());
    QCOMPARE(job->persistentStatusMessage(), rec.persistent.last());
    QVERIFY(rec.persistent.contains(QStringLiteral("Flash SoC...")));
    QVERIFY(rec.persistent.contains(QStringLiteral("Flashing SoC: starting...")));
    QVERIFY(rec.cancellable.contains(true));
    QVERIFY(job->isCancellable());
    job->cancel();
    QVERIFY(rec.waitForOperation());
    QVERIFY(job->persistentStatusMessage().isEmpty());
    QCOMPARE(rec.cancellable.last(), false);
}

// =======================================================================================
// One firmware operation per machine (the lock)
// =======================================================================================

void Tst_firmwarejob::secondJobRefusedWhileTheFirstIsBusy()
{
    // The tab and su-fw are two jobs on one PC and one board: while one runs an operation, the
    // other's operation ends at once (framed as usual) and its status query does nothing.
    FakeToolRunner runner;
    scriptNoBoard(runner);
    auto first = makeJob(&runner);
    const QString cache = caseDir(QStringLiteral("cache"));
    QVERIFY(writeFile(cache + QStringLiteral("/update.img"), rkfwBytes()));
    first->setCacheDirectory(cache);
    Recorder firstRec(first.get());
    first->flashSoc();
    QTRY_COMPARE_WITH_TIMEOUT(first->socFlasher()->phase(), SocFlasher::Phase::WaitingForLoader, kOpTimeoutMs);
    QVERIFY(QFileInfo::exists(firmwareLockPath()));

    FakeToolRunner otherRunner;   // what the second job runs, on its own
    scriptNoBoard(otherRunner);
    auto second = makeJob(&otherRunner);
    second->setCacheDirectory(cache);
    Recorder rec(second.get());
    const QString running = runningText(QLockFile(firmwareLockPath()));   // what the first job wrote into it
    QVERIFY2(running.endsWith(QStringLiteral("pid %1)").arg(QCoreApplication::applicationPid())), qPrintable(running));
    second->flashSoc();
    QCOMPARE(rec.order, (QStringList{QStringLiteral("step:flash"), QStringLiteral("op:flash")}));
    QCOMPARE(rec.lastOperation().ok, false);
    QCOMPARE(rec.lastOperation().message, running);
    QCOMPARE(rec.busyChanges, (QList<bool>{true, false}));
    QCOMPARE(rec.log.first(), QStringLiteral("--- Flash SoC ---"));
    QCOMPARE(rec.log.last(), QStringLiteral("Flash SoC failed: ") + running);
    QVERIFY(!second->isBusy());
    second->pullAndFlash();   // a chain is refused the same way, without a step of its own
    QCOMPARE(rec.order.mid(2), QStringList{QStringLiteral("op:pull-flash")});
    QCOMPARE(rec.lastOperation().message, running);

    second->refreshStatus();
    second->refreshStatus();
    QCOMPARE(second->statusPausedReason(), running);
    QCOMPARE(rec.log.count(QStringLiteral("status paused: ") + running), qsizetype(1));   // said once
    QCoreApplication::processEvents();
    QVERIFY(otherRunner.invocations().isEmpty());   // nothing of the second job reached a tool
    QVERIFY(first->isBusy());                       // the first one goes on undisturbed

    // The first one ends: the second one runs again.
    first->cancel();
    QVERIFY(firstRec.waitForOperation());
    QVERIFY(lockIsFree());
    second->refreshStatus();
    QVERIFY(second->statusPausedReason().isEmpty());
    QVERIFY(otherRunner.invocations().value(0).startsWith(QStringLiteral("upgrade_tool LD")));
    second->flashSoc();
    QVERIFY(second->isBusy());
    QVERIFY(!lockIsFree());
    second->cancel();
    QVERIFY(rec.waitForOperation(3));
    QVERIFY(lockIsFree());
}

void Tst_firmwarejob::refreshStatusPausedWhileTheLockIsHeld()
{
    // Another process's operation (a QLockFile of this test stands in for su-fw): the status timer
    // runs no tool until it ends; an operation is refused with who holds it.
    FakeToolRunner runner;
    scriptNoBoard(runner);
    auto job = makeJob(&runner);
    Recorder rec(job.get());
    QLockFile other(firmwareLockPath());
    QVERIFY(other.tryLock(0));
    const QString running = runningText(other);
    QVERIFY2(running.contains(QString::number(QCoreApplication::applicationPid())), qPrintable(running));

    job->refreshStatus();
    job->refreshStatus();
    QCoreApplication::processEvents();
    QVERIFY(runner.invocations().isEmpty());
    QVERIFY(!job->hasStatus());
    QCOMPARE(job->statusPausedReason(), running);
    QCOMPARE(rec.log, QStringList{QStringLiteral("status paused: ") + running});

    QVERIFY(job->setPackagePath(dataFile("firmware-0.8.1.pkg")));
    job->setRole(CcProtocol::RoleRx);
    job->readMcuInfo();
    QCOMPARE(rec.lastOperation().name, QStringLiteral("info"));
    QCOMPARE(rec.lastOperation().message, running);
    job->updateMcu();
    QCOMPARE(rec.lastOperation().message, running);
    QVERIFY(runner.invocations().isEmpty());

    // Released: the next refresh queries again.
    other.unlock();
    job->refreshStatus();
    QVERIFY(job->statusPausedReason().isEmpty());
    QTRY_VERIFY(job->hasStatus());
    QVERIFY(runner.invocations().first().startsWith(QStringLiteral("upgrade_tool LD")));
    // Held again: paused again, and said again.
    QVERIFY(other.tryLock(0));
    job->refreshStatus();
    QCOMPARE(rec.log.count(QStringLiteral("status paused: ") + running), qsizetype(2));
    other.unlock();

    // A lock file left by a process that is gone is stale: it pauses nothing and is taken over.
    QVERIFY(writeFile(firmwareLockPath(), "999999999\nno-such-program\n\n"));
    const qsizetype before = runner.invocations().size();
    job->refreshStatus();
    QVERIFY(job->statusPausedReason().isEmpty());
    QCOMPARE(runner.invocations().size(), before + 1);
    QVERIFY(lockIsFree());
}

void Tst_firmwarejob::lockReleasedOnEveryEndPath()
{
    FakeToolRunner runner;
    scriptNoBoard(runner);
    runner.addScript(QRegularExpression(QStringLiteral("killall rfmcu")), QByteArray());
    runner.addScript(QRegularExpression(QStringLiteral("rfmcu cmd version")), "OK version=0.8.1 role=tx\r\n");
    auto job = makeJob(&runner);
    Recorder rec(job.get());
    QVERIFY(lockIsFree());

    // Done.
    job->readMcuInfo();
    QVERIFY(!lockIsFree());   // held while it runs
    QVERIFY(rec.waitForOperation(1));
    QVERIFY2(rec.lastOperation().ok, qPrintable(rec.lastOperation().message));
    QVERIFY(lockIsFree());

    // Failed (at once: no package).
    job->updateMcu();
    QCOMPARE(rec.operations.size(), qsizetype(2));
    QVERIFY(!rec.lastOperation().ok);
    QVERIFY(lockIsFree());

    // Cancelled.
    const QString cache = caseDir(QStringLiteral("cache"));
    QVERIFY(writeFile(cache + QStringLiteral("/update.img"), rkfwBytes()));
    job->setCacheDirectory(cache);
    job->flashSoc();
    QTRY_COMPARE_WITH_TIMEOUT(job->socFlasher()->phase(), SocFlasher::Phase::WaitingForLoader, kOpTimeoutMs);
    QVERIFY(!lockIsFree());
    job->cancel();
    QVERIFY(rec.waitForOperation(3));
    QCOMPARE(rec.lastOperation().message, QStringLiteral("cancelled"));
    QVERIFY(lockIsFree());

    // Destroyed while busy.
    job->flashSoc();
    QTRY_COMPARE_WITH_TIMEOUT(job->socFlasher()->phase(), SocFlasher::Phase::WaitingForLoader, kOpTimeoutMs);
    QVERIFY(!lockIsFree());
    job.reset();
    QVERIFY(lockIsFree());
    QVERIFY(!QFileInfo::exists(firmwareLockPath()));
}

// =======================================================================================
// Pull
// =======================================================================================

void Tst_firmwarejob::pullTargetErrors()
{
    SshProfileStore store;
    FakeToolRunner runner;
    auto job = makeJob(&runner, &store);
    Recorder rec(job.get());
    job->setCacheDirectory(caseDir(QStringLiteral("cache")));
    job->setRemoteFolder(QStringLiteral("/srv/image"));

    job->pull();
    QCOMPARE(rec.lastOperation().message, QStringLiteral("enter the build machine (an SSH profile or user@host)"));
    job->setSourceTarget(QStringLiteral("ssh:profile:no-such-id"));
    job->pull();
    QCOMPARE(rec.lastOperation().message, QStringLiteral("the SSH profile of the build machine no longer exists"));
    job->setSourceTarget(QStringLiteral("not a target"));
    job->pull();
    QCOMPARE(rec.lastOperation().message, QStringLiteral("\"not a target\" is not an SSH target such as user@host[:port]"));
    job->setSourceTarget(QStringLiteral("ssh:target:root@10.0.0.24"));
    job->setRemoteFolder(QString());
    job->pull();
    QCOMPARE(rec.lastOperation().message, QStringLiteral("enter the remote folder that holds update.img"));
    job->setRemoteFolder(QStringLiteral("/srv/image"));
    job->setCacheDirectory(QString());
    job->pull();
    QCOMPARE(rec.lastOperation().message, QStringLiteral("cannot create the cache folder "));
    for (const Result& r : std::as_const(rec.operations)) {
        QCOMPARE(r.name, QStringLiteral("pull"));
        QVERIFY(!r.ok);
    }
    QCOMPARE(rec.operations.size(), qsizetype(5));
    QCOMPARE(job->sshConnection()->state(), Transport::State::Disconnected);
    QVERIFY(store.recentTargets().isEmpty());   // nothing accepted, nothing remembered
}

void Tst_firmwarejob::pullDownloadsThenSkipsUnchanged()
{
    SshProfileStore store;
    BuildMachine machine;
    QVERIFY(machine.start(&store, caseDir(QStringLiteral("work"))));
    FakeToolRunner runner;
    auto job = makeJob(&runner, &store);
    const QString cache = caseDir(QStringLiteral("cache"));
    const QString key = QStringLiteral("ssh:profile:") + machine.profile.id;
    job->setSourceTarget(key);
    job->setRemoteFolder(machine.imageFolder);
    job->setCacheDirectory(cache);
    Recorder rec(job.get());
    QSignalSpy accepted(job.get(), &FirmwareJob::pullSourceAccepted);
    QSignalSpy images(job.get(), &FirmwareJob::imageChanged);

    job->pull();
    QVERIFY(job->isBusy());
    QVERIFY(job->isCancellable());   // a pull can always be cancelled
    QVERIFY(rec.waitForOperation());
    QVERIFY2(rec.lastOperation().ok, qPrintable(rec.lastOperation().message));
    QCOMPARE(rec.lastOperation().name, QStringLiteral("pull"));
    QCOMPARE(accepted.count(), qsizetype(1));
    QCOMPARE(accepted.at(0).at(0).toString(), key);
    QCOMPARE(accepted.at(0).at(1).toString(), machine.imageFolder);
    QCOMPARE(accepted.at(0).at(2).toString(), cache);
    QCOMPARE(job->imagePath(), cache + QStringLiteral("/update.img"));
    QCOMPARE(QFileInfo(job->imagePath()).size(), qint64(machine.image.size()));
    QVERIFY(job->cacheEntry().isValid());
    QVERIFY(!images.isEmpty());
    QCOMPARE(images.last().at(0).toBool(), true);   // the content changed in place
    const QString size = QLocale::c().formattedDataSize(machine.image.size(), 1, QLocale::DataSizeSIFormat);
    const QString log = rec.logText();
    QVERIFY2(log.contains(QStringLiteral("Connecting to %1").arg(machine.target())), qPrintable(log));
    QVERIFY(log.contains(QStringLiteral("Connected to %1").arg(machine.target())));
    QVERIFY(log.contains(QStringLiteral("Remote update.img: %1, built").arg(size)));
    QVERIFY(log.contains(QStringLiteral("Pull done: ")));
    QTRY_COMPARE(job->sshConnection()->state(), Transport::State::Disconnected);   // only held while pulling
    QCOMPARE(machine.server->lastAuthMethod(), QStringLiteral("publickey"));
    QVERIFY(store.profile(machine.profile.id)->lastUsed.isValid());   // touched like a session's connect
    bool sawPercent = false;
    for (const auto& p : std::as_const(rec.progress)) {
        sawPercent = sawPercent || (p.first.startsWith(QStringLiteral("Pulling update.img")) && p.second >= 0);
    }
    QVERIFY(sawPercent);

    // Unchanged on the build machine: skipped, still a success.
    job->pull();
    QVERIFY(rec.waitForOperation(2));
    QVERIFY(rec.lastOperation().ok);
    QVERIFY(rec.logText().contains(QStringLiteral("unchanged")));
}

void Tst_firmwarejob::pullHostKeyAndAuthCallbacks()
{
    // An unknown host and a password: both reach the callbacks; the key is remembered in
    // known_hosts, so a fresh connection does not ask again (the password is asked every time).
    SshProfileStore store;
    BuildMachine machine;
    machine.trustServer = false;
    machine.passwordAuth = true;
    QVERIFY(machine.start(&store, caseDir(QStringLiteral("work"))));
    QVERIFY(QFile(machine.knownHostsFile).size() == 0);
    FakeToolRunner runner;
    QList<SshConnection::HostKeyInfo> keys;
    QList<SshConnection::AuthPrompt> prompts;
    const auto configure = [&](FirmwareJob& job) {
        job.setSourceTarget(QStringLiteral("ssh:profile:") + machine.profile.id);
        job.setRemoteFolder(machine.imageFolder);
        job.setCacheDirectory(caseDir(QStringLiteral("cache")));
        job.setHostKeyHandler([&](const SshConnection::HostKeyInfo& info) {
            keys.append(info);
            return FirmwareJob::HostKeyDecision{true, true};
        });
        job.setAuthPromptHandler([&](const SshConnection::AuthPrompt& prompt) -> std::optional<FirmwareJob::AuthAnswer> {
            prompts.append(prompt);
            return FirmwareJob::AuthAnswer{machine.password, false};
        });
    };
    {
        auto job = makeJob(&runner, &store);
        configure(*job);
        Recorder rec(job.get());
        job->pull();
        QVERIFY(rec.waitForOperation());
        QVERIFY2(rec.lastOperation().ok, qPrintable(rec.lastOperation().message));
        QCOMPARE(keys.size(), qsizetype(1));
        QCOMPARE(keys.first().status, SshConnection::HostKeyStatus::Unknown);
        QCOMPARE(keys.first().keyType, machine.server->hostKeyType());
        QCOMPARE(keys.first().fingerprintSha256, machine.server->hostKeyFingerprintSha256());
        QCOMPARE(prompts.size(), qsizetype(1));
        QCOMPARE(prompts.first().kind, SshConnection::PromptKind::Password);
        QVERIFY2(prompts.first().prompt.contains(machine.target()), qPrintable(prompts.first().prompt));
        QCOMPARE(machine.server->lastAuthMethod(), QStringLiteral("password"));
        QVERIFY(rec.logText().contains(QStringLiteral("Authenticated to %1 (password)").arg(machine.target())));
    }
    QFile knownHosts(machine.knownHostsFile);
    QVERIFY(knownHosts.open(QIODevice::ReadOnly));
    QVERIFY(QString::fromUtf8(knownHosts.readAll()).contains(machine.server->hostKeyPublicLine().section(QLatin1Char(' '), 1, 1)));

    auto again = makeJob(&runner, &store);   // a new connection: only known_hosts can know the key
    configure(*again);
    Recorder rec(again.get());
    again->pull();
    QVERIFY(rec.waitForOperation());
    QVERIFY2(rec.lastOperation().ok, qPrintable(rec.lastOperation().message));
    QCOMPARE(keys.size(), qsizetype(1));      // the remembered key
    QCOMPARE(prompts.size(), qsizetype(2));   // the password was not remembered
}

void Tst_firmwarejob::hostKeyRefusedByTheHandler()
{
    SshProfileStore store;
    BuildMachine machine;
    machine.trustServer = false;
    QVERIFY(machine.start(&store, caseDir(QStringLiteral("work"))));
    FakeToolRunner runner;
    auto job = makeJob(&runner, &store);
    job->setSourceTarget(QStringLiteral("ssh:profile:") + machine.profile.id);
    job->setRemoteFolder(machine.imageFolder);
    job->setCacheDirectory(caseDir(QStringLiteral("cache")));
    int asked = 0;
    job->setHostKeyHandler([&](const SshConnection::HostKeyInfo&) {
        ++asked;
        return FirmwareJob::HostKeyDecision{false, true};   // "remember" without "accept" means nothing
    });
    Recorder rec(job.get());
    job->pull();
    QVERIFY(rec.waitForOperation());
    QCOMPARE(asked, 1);
    QCOMPARE(rec.lastOperation().ok, false);
    QCOMPARE(rec.lastOperation().message, QStringLiteral("Host key rejected for %1").arg(machine.target()));
    QVERIFY(rec.log.contains(QStringLiteral("SSH: Host key rejected for %1").arg(machine.target())));
    QCOMPARE(QFile(machine.knownHostsFile).size(), qint64(0));
}

void Tst_firmwarejob::headlessHostKeyRefused()
{
    // No handler (su-fw without --accept-host-key): refused with the message the tab shows when
    // the user refuses.
    SshProfileStore store;
    BuildMachine machine;
    machine.trustServer = false;
    QVERIFY(machine.start(&store, caseDir(QStringLiteral("work"))));
    FakeToolRunner runner;
    auto job = makeJob(&runner, &store);
    job->setSourceTarget(QStringLiteral("ssh:profile:") + machine.profile.id);
    job->setRemoteFolder(machine.imageFolder);
    job->setCacheDirectory(caseDir(QStringLiteral("cache")));
    Recorder rec(job.get());
    job->pull();
    QVERIFY(rec.waitForOperation());
    QCOMPARE(rec.lastOperation().ok, false);
    QCOMPARE(rec.lastOperation().message, QStringLiteral("Host key rejected for %1").arg(machine.target()));
    QCOMPARE(QFile(machine.knownHostsFile).size(), qint64(0));
    QVERIFY(!QFileInfo::exists(caseDir(QStringLiteral("cache")) + QStringLiteral("/update.img")));
}

void Tst_firmwarejob::headlessPromptFailsWithItsText()
{
    SshProfileStore store;
    BuildMachine machine;
    machine.passwordAuth = true;
    QVERIFY(machine.start(&store, caseDir(QStringLiteral("work"))));
    FakeToolRunner runner;
    auto job = makeJob(&runner, &store);
    job->setSourceTarget(QStringLiteral("ssh:profile:") + machine.profile.id);
    job->setRemoteFolder(machine.imageFolder);
    job->setCacheDirectory(caseDir(QStringLiteral("cache")));
    Recorder rec(job.get());
    job->pull();
    QVERIFY(rec.waitForOperation());
    QCOMPARE(rec.lastOperation().ok, false);
    QCOMPARE(rec.lastOperation().message,
             QStringLiteral("authentication failed: nobody answered \"Password for %1:\"").arg(machine.target()));
    QCOMPARE(rec.log.last(), QStringLiteral("Pull failed: ") + rec.lastOperation().message);
    QTRY_COMPARE(job->sshConnection()->state(), Transport::State::Disconnected);
}

void Tst_firmwarejob::authHandlerCancels()
{
    SshProfileStore store;
    BuildMachine machine;
    machine.passwordAuth = true;
    QVERIFY(machine.start(&store, caseDir(QStringLiteral("work"))));
    FakeToolRunner runner;
    auto job = makeJob(&runner, &store);
    job->setSourceTarget(QStringLiteral("ssh:profile:") + machine.profile.id);
    job->setRemoteFolder(machine.imageFolder);
    job->setCacheDirectory(caseDir(QStringLiteral("cache")));
    int asked = 0;
    job->setAuthPromptHandler([&](const SshConnection::AuthPrompt&) -> std::optional<FirmwareJob::AuthAnswer> {
        ++asked;
        return std::nullopt;
    });
    Recorder rec(job.get());
    job->pull();
    QVERIFY(rec.waitForOperation());
    QCOMPARE(asked, 1);
    QCOMPARE(rec.lastOperation().ok, false);
    QVERIFY2(rec.lastOperation().message.contains(QStringLiteral("cancelled")), qPrintable(rec.lastOperation().message));
    QVERIFY(!rec.lastOperation().message.contains(QStringLiteral("nobody answered")));
}

void Tst_firmwarejob::noSecretInTheLog()
{
    // The password typed into the prompt appears nowhere: not in logLine(), not in toolOutput(),
    // not in any Qt message (the System Log).
    MessageCapture capture;
    SshProfileStore store;
    BuildMachine machine;
    machine.passwordAuth = true;
    machine.password = QStringLiteral("Zq7-unique-Secret-4711");
    QVERIFY(machine.start(&store, caseDir(QStringLiteral("work"))));
    FakeToolRunner runner;
    auto job = makeJob(&runner, &store);
    job->setSourceTarget(QStringLiteral("ssh:profile:") + machine.profile.id);
    job->setRemoteFolder(machine.imageFolder);
    job->setCacheDirectory(caseDir(QStringLiteral("cache")));
    job->setAuthPromptHandler([&](const SshConnection::AuthPrompt&) -> std::optional<FirmwareJob::AuthAnswer> {
        return FirmwareJob::AuthAnswer{machine.password, false};
    });
    Recorder rec(job.get());
    job->pull();
    QVERIFY(rec.waitForOperation());
    QVERIFY2(rec.lastOperation().ok, qPrintable(rec.lastOperation().message));
    QVERIFY(!rec.log.isEmpty());
    const QStringList everything = rec.log + rec.tool + rec.persistent + capture.messages();
    for (const QString& line : everything) {
        QVERIFY2(!line.contains(machine.password), qPrintable(line));
    }
    for (const auto& p : std::as_const(rec.progress)) {
        QVERIFY(!p.first.contains(machine.password));
    }
}

void Tst_firmwarejob::adHocTargetUsesSshDefaults()
{
    // A user@host target typed by hand: the SSH defaults fill the known_hosts file and the key;
    // the target becomes a recent one of the store.
    SshProfileStore scratch;
    BuildMachine machine;
    QVERIFY(machine.start(&scratch, caseDir(QStringLiteral("work"))));
    SshProfileStore store;
    FakeToolRunner runner;
    auto job = makeJob(&runner, &store);
    job->setSourceTarget(QStringLiteral("ssh:target:") + machine.target());
    job->setRemoteFolder(machine.imageFolder);
    job->setCacheDirectory(caseDir(QStringLiteral("cache")));
    FirmwareJob::SshDefaults defaults;
    defaults.knownHostsFile = machine.knownHostsFile;
    defaults.identityFile = machine.keyFile;
    job->setSshDefaults(defaults);
    Recorder rec(job.get());
    job->pull();
    QVERIFY(rec.waitForOperation());
    QVERIFY2(rec.lastOperation().ok, qPrintable(rec.lastOperation().message));
    QCOMPARE(machine.server->lastAuthMethod(), QStringLiteral("publickey"));
    QCOMPARE(store.recentTargets(), QStringList{machine.target()});
    QCOMPARE(job->sshConnection()->profile().knownHostsFile, machine.knownHostsFile);
    QCOMPARE(job->sshConnection()->profile().keepAliveSeconds, 30);
}

void Tst_firmwarejob::cancelWhileConnecting()
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
    FakeToolRunner runner;
    auto job = makeJob(&runner);
    job->setSourceTarget(QStringLiteral("test@127.0.0.1:%1").arg(silent.serverPort()));
    job->setRemoteFolder(QStringLiteral("/srv/image"));
    job->setCacheDirectory(caseDir(QStringLiteral("cache")));
    Recorder rec(job.get());
    job->pull();
    QTRY_VERIFY_WITH_TIMEOUT(!held.isEmpty(), kOpTimeoutMs);
    QVERIFY(job->persistentStatusMessage().contains(QStringLiteral("connecting to test@127.0.0.1")));
    QVERIFY(job->isCancellable());
    job->cancel();
    QVERIFY(rec.waitForOperation());
    QCOMPARE(rec.lastOperation().message, QStringLiteral("cancelled"));
    QVERIFY(rec.log.contains(QStringLiteral("Cancelling Pull")));
    QCOMPARE(job->sshConnection()->state(), Transport::State::Disconnected);
    qDeleteAll(held);
}

void Tst_firmwarejob::disconnectSourceCancelsThePull()
{
    QTcpServer silent;
    QVERIFY(silent.listen(QHostAddress::LocalHost));
    QList<QTcpSocket*> held;
    connect(&silent, &QTcpServer::newConnection, &silent, [&silent, &held]() {
        while (QTcpSocket* socket = silent.nextPendingConnection()) {
            held.append(socket);
        }
    });
    FakeToolRunner runner;
    auto job = makeJob(&runner);
    job->setSourceTarget(QStringLiteral("test@127.0.0.1:%1").arg(silent.serverPort()));
    job->setRemoteFolder(QStringLiteral("/srv/image"));
    job->setCacheDirectory(caseDir(QStringLiteral("cache")));
    Recorder rec(job.get());
    job->pull();
    QTRY_VERIFY_WITH_TIMEOUT(!held.isEmpty(), kOpTimeoutMs);
    job->disconnectSource();
    QVERIFY(rec.waitForOperation());
    QCOMPARE(rec.lastOperation().message, QStringLiteral("cancelled"));
    QCOMPARE(job->sshConnection()->state(), Transport::State::Disconnected);
    job->disconnectSource();   // idle and closed: nothing
    QCOMPARE(rec.operations.size(), qsizetype(1));
    qDeleteAll(held);
}

// =======================================================================================
// Flash SoC
// =======================================================================================

void Tst_firmwarejob::flashLoaderWritesSelectedRows()
{
    // Loader mode: EXF (the plan), one WL per selected row with the offset and the partition size
    // in sectors, RD, then the ADB wait and the board information.
    FakeToolRunner runner;
    scriptSocFlash(runner, kLdLoader);
    auto job = makeJob(&runner);
    job->setMtdParts(kNodeMtdParts);
    const QString image = caseDir(QStringLiteral("flash/update.img"));
    QVERIFY(writeFile(image, rkfwBytes()));
    QVERIFY(job->setImagePath(image));
    QVERIFY(waitForPlan(*job));
    job->setSelectedPartitions({QStringLiteral("boot"), QStringLiteral("rootfs")});
    Recorder rec(job.get());
    QSignalSpy boardInfo(job.get(), &FirmwareJob::boardInfoReceived);
    job->flashSoc();
    QVERIFY(rec.waitForOperation());
    QVERIFY2(rec.lastOperation().ok, qPrintable(rec.lastOperation().message));
    QCOMPARE(rec.lastOperation().message, QStringLiteral("Upgrade firmware ok; board is back on ADB"));
    const QString dir = extractDirOf(image);
    QCOMPARE(wlCommands(runner), (QStringList{expectedWl(4, dir), expectedWl(5, dir)}));
    QCOMPARE(runner.count(QRegularExpression(QStringLiteral("^upgrade_tool RD$"))), 1);
    QCOMPARE(runner.count(QRegularExpression(QStringLiteral("^upgrade_tool UF "))), 0);
    QCOMPARE(runner.count(exfPattern()), 1);   // the plan was current: not unpacked again
    QVERIFY(runner.count(QRegularExpression(QStringLiteral("get-state"))) >= 1);   // ADB back
    QCOMPARE(job->socFlasher()->flashMethod(), SocFlasher::FlashMethod::PartitionWrites);
    const QString log = rec.logText();
    QVERIFY(log.contains(QStringLiteral("Partitions checked: boot, rootfs (Loader mode writes these one by one; "
                                        "MASKROM mode writes the whole image)")));
    QVERIFY(log.contains(QStringLiteral("Flashing %1").arg(QDir::toNativeSeparators(image))));
    QVERIFY(log.contains(QStringLiteral("Loader mode: writing partition by partition with upgrade_tool WL")));
    QVERIFY(log.contains(QStringLiteral("Board information:")));
    QCOMPARE(boardInfo.count(), qsizetype(1));
    QVERIFY(job->boardInfo().startsWith(QStringLiteral("Linux version 5.10.209+")));
    QCOMPARE(rec.stepNames(), QStringList{QStringLiteral("flash")});
    // The board as it came back: a status query after the success.
    QTRY_VERIFY(job->hasStatus());
}

void Tst_firmwarejob::flashLoaderWritesEveryRowByDefault()
{
    FakeToolRunner runner;
    scriptSocFlash(runner, kLdLoader);
    auto job = makeJob(&runner);
    job->setMtdParts(kNodeMtdParts);
    const QString image = caseDir(QStringLiteral("flash/update.img"));
    QVERIFY(writeFile(image, rkfwBytes()));
    QVERIFY(job->setImagePath(image));
    QVERIFY(waitForPlan(*job));
    QVERIFY(job->allPartitionsSelected());
    Recorder rec(job.get());
    job->flashSoc();
    QVERIFY(rec.waitForOperation());
    QVERIFY2(rec.lastOperation().ok, qPrintable(rec.lastOperation().message));
    QStringList expected;
    for (int k = 0; k < kNodePartitionCount; ++k) {
        expected << expectedWl(k, extractDirOf(image));
    }
    QCOMPARE(wlCommands(runner), expected);
    QVERIFY(rec.logText().contains(QStringLiteral("Partitions checked: env, idblock, uboot, meta, boot, rootfs, userdata")));
    // WL's progress went to the owner as tool lines.
    QVERIFY(rec.tool.join(QLatin1Char('\n')).contains(QStringLiteral("Write LBA from file")));
}

void Tst_firmwarejob::flashMaskromWritesWholeImage()
{
    FakeToolRunner runner;
    scriptMaskrom(runner);
    auto job = makeJob(&runner);
    job->setMtdParts(kNodeMtdParts);
    const QString image = caseDir(QStringLiteral("maskrom/update.img"));
    QVERIFY(writeFile(image, rkfwBytes()));
    QVERIFY(job->setImagePath(image));
    QVERIFY(waitForPlan(*job));
    job->setSelectedPartitions({QStringLiteral("boot")});
    Recorder rec(job.get());
    job->flashSoc();
    QVERIFY(rec.waitForOperation());
    QVERIFY2(rec.lastOperation().ok, qPrintable(rec.lastOperation().message));
    QCOMPARE(runner.count(QRegularExpression(QStringLiteral("^upgrade_tool UF "))), 1);
    QVERIFY(wlCommands(runner).isEmpty());
    QCOMPARE(job->socFlasher()->flashMethod(), SocFlasher::FlashMethod::UpgradeFirmware);
    const QString log = rec.logText();
    QVERIFY(log.contains(QStringLiteral("Partitions checked: boot")));
    QVERIFY(log.contains(QStringLiteral("MASKROM mode: writing the whole image with upgrade_tool UF")));
    QVERIFY(log.contains(QStringLiteral("the partition selection (boot) is ignored: UF writes every partition")));
    QVERIFY(rec.tool.join(QLatin1Char('\n')).contains(QStringLiteral("Download Image... (100%)")));
    // Progress: a busy stage while waiting, a percentage while writing.
    bool sawBusy = false;
    bool sawPercent = false;
    for (const auto& p : std::as_const(rec.progress)) {
        sawBusy = sawBusy || p.second == FirmwareJob::kProgressBusy;
        sawPercent = sawPercent || (p.second >= 0 && p.first.startsWith(QStringLiteral("Flashing SoC")));
    }
    QVERIFY(sawBusy);
    QVERIFY(sawPercent);
}

void Tst_firmwarejob::sizeCheckStopsBeforeAnyWrite()
{
    // An image larger than its partition: with every row to write (no plan of its own yet), the
    // flasher stops before the first WL.
    FakeToolRunner runner;
    scriptSocFlash(runner, kLdLoader);
    auto job = makeJob(&runner);
    job->setMtdParts(kNodeMtdParts);
    const QString image = caseDir(QStringLiteral("size/big.img"));
    QVERIFY(writeFile(image, rkfwBytes()));
    job->useLocalImage(image);
    QCOMPARE(job->planState(), FirmwareJob::PlanState::NoImage);   // no plan: the flasher decides
    Recorder rec(job.get());
    job->flashSoc();
    QVERIFY(rec.waitForOperation());
    QCOMPARE(rec.lastOperation().ok, false);
    QVERIFY2(rec.lastOperation().message.contains(QStringLiteral("does not fit partition boot")), qPrintable(rec.lastOperation().message));
    QVERIFY(wlCommands(runner).isEmpty());
    QCOMPARE(runner.count(QRegularExpression(QStringLiteral("^upgrade_tool RD$"))), 0);
    QVERIFY(rec.logText().contains(QStringLiteral("Partitions: every partition of the image")));
}

void Tst_firmwarejob::noPartitionSelectedIsRefused()
{
    FakeToolRunner runner;
    scriptSocFlash(runner, kLdLoader);
    auto job = makeJob(&runner);
    job->setMtdParts(kNodeMtdParts);
    const QString image = caseDir(QStringLiteral("none/update.img"));
    QVERIFY(writeFile(image, rkfwBytes()));
    QVERIFY(job->setImagePath(image));
    QVERIFY(waitForPlan(*job));
    job->setSelectedPartitions({});
    QVERIFY(job->selectedPartitions().isEmpty());
    QTRY_VERIFY(runner.liveInvocations().isEmpty());
    const qsizetype before = runner.invocations().size();
    Recorder rec(job.get());
    job->flashSoc();
    QCOMPARE(rec.operations.size(), qsizetype(1));
    QCOMPARE(rec.lastOperation().ok, false);
    QCOMPARE(rec.lastOperation().message, QStringLiteral("no partition is checked: check the ones to write (All checks every one)"));
    QCOMPARE(runner.invocations().size(), before);
}

void Tst_firmwarejob::layoutUnknownInLoaderMode()
{
    FakeToolRunner runner;
    scriptSocFlash(runner, kLdLoader);
    auto job = makeJob(&runner);
    const QString image = caseDir(QStringLiteral("unknown/update.img"));
    QVERIFY(writeFile(image, rkfwBytes()));
    Recorder rec(job.get());
    QVERIFY(job->setImagePath(image));
    QVERIFY(waitForPlan(*job));
    QVERIFY(!job->layoutKnown());
    QVERIFY(job->flashLayout().isEmpty());
    QVERIFY(job->selectedPartitions().isEmpty());   // nothing can be written without a layout
    QVERIFY(rec.logText().contains(QStringLiteral("The flash layout is not known yet")));
    job->flashSoc();
    QVERIFY(rec.waitForOperation());
    QCOMPARE(rec.lastOperation().ok, false);
    QCOMPARE(rec.lastOperation().message, QStringLiteral("partition layout unknown: boot the board once so it can be read "
                                                         "over ADB, or flash it in MASKROM mode"));
    QVERIFY(wlCommands(runner).isEmpty());
}

void Tst_firmwarejob::cancelWhileWaitingForLoader()
{
    FakeToolRunner runner;
    scriptNoBoard(runner);
    auto job = makeJob(&runner);
    const QString cache = caseDir(QStringLiteral("cache"));
    QVERIFY(writeFile(cache + QStringLiteral("/update.img"), rkfwBytes()));
    job->setCacheDirectory(cache);
    Recorder rec(job.get());
    job->flashSoc();
    QTRY_COMPARE_WITH_TIMEOUT(job->socFlasher()->phase(), SocFlasher::Phase::WaitingForLoader, kOpTimeoutMs);
    QVERIFY(job->isCancellable());
    job->cancel();
    QVERIFY(rec.waitForOperation());
    QCOMPARE(rec.lastOperation().name, QStringLiteral("flash"));
    QCOMPARE(rec.lastOperation().message, QStringLiteral("cancelled"));
    QVERIFY(rec.log.contains(QStringLiteral("Cancelling Flash SoC")));
    QVERIFY(rec.cancelRefusals.isEmpty());
    QVERIFY(!job->isBusy());
}

void Tst_firmwarejob::cancelRefusedWhileWriting()
{
    // Never while upgrade_tool writes: cancel() is refused (logged, cancelRefused()), the flash
    // goes on and ends normally.
    FakeToolRunner runner;
    QPointer<FakeToolInvocation> uf;
    runner.addScript(QRegularExpression(QStringLiteral("^upgrade_tool LD$")), kLdMaskrom);
    runner.addHandler(QRegularExpression(QStringLiteral("^upgrade_tool UF ")), [&uf](FakeToolInvocation* i) {
        uf = i;
        i->emitOutput("Download Image... (10%)\r");
        i->hang();
    });
    runner.addScript(QRegularExpression(QStringLiteral("^adb( -s \\S+)? get-state$")), "device\n");
    runner.addScript(QRegularExpression(QStringLiteral("cat /proc/version")), kProcVersion);
    auto job = makeJob(&runner);
    const QString cache = caseDir(QStringLiteral("cache"));
    QVERIFY(writeFile(cache + QStringLiteral("/update.img"), rkfwBytes()));
    job->setCacheDirectory(cache);
    Recorder rec(job.get());
    job->flashSoc();
    QTRY_VERIFY_WITH_TIMEOUT(!uf.isNull(), kOpTimeoutMs);
    QCOMPARE(job->socFlasher()->phase(), SocFlasher::Phase::Flashing);
    QVERIFY(!job->isCancellable());
    QCOMPARE(rec.cancellable.last(), false);
    job->cancel();
    QCOMPARE(rec.cancelRefusals, QStringList{QStringLiteral("Cannot cancel while upgrade_tool writes the flash")});
    QCOMPARE(rec.log.last(), QStringLiteral("Cannot cancel while upgrade_tool writes the flash"));
    QTest::qWait(150);
    QVERIFY(job->isBusy());
    QVERIFY(rec.operations.isEmpty());
    uf->emitOutput("Upgrade firmware ok.\r\n");
    uf->finish(0);
    QVERIFY(rec.waitForOperation());
    QVERIFY2(rec.lastOperation().ok, qPrintable(rec.lastOperation().message));
}

void Tst_firmwarejob::downloadModeHeadless()
{
    // No board and no handler: the MASKROM instructions (log, progress text, lasting status), then
    // the flasher's own timeout.
    FakeToolRunner runner;
    scriptNoBoard(runner);
    auto job = makeJob(&runner);
    job->setLoaderWaitSeconds(1);
    const QString cache = caseDir(QStringLiteral("cache"));
    QVERIFY(writeFile(cache + QStringLiteral("/update.img"), rkfwBytes()));
    job->setCacheDirectory(cache);
    QCOMPARE(job->maskromInstructions(),
             QStringLiteral("No board found: hold the MASKROM button while plugging in the USB-C cable (or while "
                            "power-cycling the board); waiting up to 1 s"));
    Recorder rec(job.get());
    job->flashSoc();
    QVERIFY(rec.waitForOperation());
    QCOMPARE(rec.lastOperation().ok, false);
    QCOMPARE(rec.lastOperation().message, QStringLiteral("board did not enter MASKROM/Loader within 1 s"));
    QVERIFY(rec.log.contains(job->maskromInstructions()));
    QVERIFY(rec.persistent.contains(job->maskromInstructions()));
    QVERIFY(rec.progress.contains(qMakePair(job->maskromInstructions(), FirmwareJob::kProgressTextOnly)));
    QVERIFY(!rec.logText().contains(QStringLiteral("Sent \"reboot loader\"")));
}

void Tst_firmwarejob::downloadModeSentThroughSession()
{
    FakeToolRunner runner;
    auto present = std::make_shared<bool>(false);
    runner.addHandler(QRegularExpression(QStringLiteral("^upgrade_tool LD$")), [present](FakeToolInvocation* i) {
        i->emitOutput(*present ? kLdMaskrom : QByteArray("List of rockusb connected(0)\r\n"));
        i->finish(0);
    });
    runner.addScript(QRegularExpression(QStringLiteral("^adb( -s \\S+)? devices$")), "List of devices attached\r\n\r\n");
    scriptMaskrom(runner);
    auto job = makeJob(&runner);
    const QString cache = caseDir(QStringLiteral("cache"));
    QVERIFY(writeFile(cache + QStringLiteral("/update.img"), rkfwBytes()));
    job->setCacheDirectory(cache);
    int asked = 0;
    int sent = 0;
    job->setDownloadModeHandler([&]() {
        ++asked;
        FirmwareJob::DownloadModeAnswer answer;
        answer.asked = true;
        answer.send = [&, present]() {
            ++sent;
            *present = true;   // the board restarts in download mode
            return QStringLiteral("COM3");
        };
        return answer;
    });
    Recorder rec(job.get());
    job->flashSoc();
    QVERIFY(rec.waitForOperation());
    QVERIFY2(rec.lastOperation().ok, qPrintable(rec.lastOperation().message));
    QCOMPARE(asked, 1);
    QCOMPARE(sent, 1);
    QVERIFY(rec.log.contains(QStringLiteral("Sent \"reboot loader\" through COM3")));
    QVERIFY(!rec.log.contains(job->maskromInstructions()));
}

void Tst_firmwarejob::downloadModeMootWhenBoardAppeared()
{
    // The board appears while the question is open (the user held MASKROM): nothing is sent.
    FakeToolRunner runner;
    auto present = std::make_shared<bool>(false);
    runner.addHandler(QRegularExpression(QStringLiteral("^upgrade_tool LD$")), [present](FakeToolInvocation* i) {
        i->emitOutput(*present ? kLdMaskrom : QByteArray("List of rockusb connected(0)\r\n"));
        i->finish(0);
    });
    runner.addScript(QRegularExpression(QStringLiteral("^adb( -s \\S+)? devices$")), "List of devices attached\r\n\r\n");
    scriptMaskrom(runner);
    auto job = makeJob(&runner);
    const QString cache = caseDir(QStringLiteral("cache"));
    QVERIFY(writeFile(cache + QStringLiteral("/update.img"), rkfwBytes()));
    job->setCacheDirectory(cache);
    int sent = 0;
    bool appeared = false;
    FirmwareJob* raw = job.get();
    job->setDownloadModeHandler([&, present, raw]() {
        *present = true;
        // The question's own event loop: the flasher goes on meanwhile.
        appeared = QTest::qWaitFor([raw]() { return raw->socFlasher()->phase() != SocFlasher::Phase::WaitingForLoader; },
                                   kOpTimeoutMs);
        FirmwareJob::DownloadModeAnswer answer;
        answer.asked = true;
        answer.send = [&]() {
            ++sent;
            return QStringLiteral("COM3");
        };
        return answer;
    });
    Recorder rec(job.get());
    job->flashSoc();
    QVERIFY(rec.waitForOperation());
    QVERIFY2(rec.lastOperation().ok, qPrintable(rec.lastOperation().message));
    QVERIFY(appeared);
    QCOMPARE(sent, 0);
    QVERIFY(rec.log.contains(QStringLiteral("The board appeared meanwhile; nothing sent")));
}

void Tst_firmwarejob::downloadModeNothingToAsk()
{
    // A handler with nothing to ask (no connected session): the MASKROM instructions at once.
    FakeToolRunner runner;
    scriptNoBoard(runner);
    auto job = makeJob(&runner);
    job->setLoaderWaitSeconds(1);
    const QString cache = caseDir(QStringLiteral("cache"));
    QVERIFY(writeFile(cache + QStringLiteral("/update.img"), rkfwBytes()));
    job->setCacheDirectory(cache);
    int asked = 0;
    job->setDownloadModeHandler([&]() {
        ++asked;
        return FirmwareJob::DownloadModeAnswer();
    });
    Recorder rec(job.get());
    job->flashSoc();
    QVERIFY(rec.waitForOperation());
    QCOMPARE(asked, 1);
    QVERIFY(rec.log.contains(job->maskromInstructions()));
    QVERIFY(!rec.log.contains(QStringLiteral("The board appeared meanwhile; nothing sent")));
    QCOMPARE(rec.lastOperation().message, QStringLiteral("board did not enter MASKROM/Loader within 1 s"));
}

// =======================================================================================
// Status, layout, partitions
// =======================================================================================

void Tst_firmwarejob::statusReadsTheLayoutOnce()
{
    // The status shows a running board whose layout nobody knows: its /proc/cmdline is read once,
    // reported with mtdPartsRead(serial, mtdparts) and kept; the plan follows.
    FakeToolRunner runner;
    scriptSocFlash(runner, kLdLoader);
    auto job = makeJob(&runner);
    QSignalSpy statuses(job.get(), &FirmwareJob::statusChanged);
    QSignalSpy layouts(job.get(), &FirmwareJob::mtdPartsRead);
    Recorder rec(job.get());
    SocFlasher::DeviceStatus board;
    board.mode = SocFlasher::DeviceStatus::Mode::Adb;
    board.serial = QStringLiteral("0225ce3c8d8d6e44");
    board.detail = QStringLiteral("device");
    const QRegularExpression read(QStringLiteral("^adb -s 0225ce3c8d8d6e44 shell \"cat /proc/cmdline\"$"));
    emit job->socFlasher()->statusChanged(board);
    QCOMPARE(statuses.count(), qsizetype(1));
    QVERIFY(job->hasStatus());
    QCOMPARE(job->lastStatus().serial, board.serial);
    QCOMPARE(runner.count(read), 1);
    QTRY_COMPARE(layouts.count(), qsizetype(1));
    QCOMPARE(layouts.at(0).at(0).toString(), board.serial);
    QCOMPARE(layouts.at(0).at(1).toString(), kNodeMtdParts);
    QCOMPARE(job->boardLayout(), kNodeMtdParts);
    QCOMPARE(job->mtdParts(board.serial), kNodeMtdParts);
    QCOMPARE(job->mtdParts(), kNodeMtdParts);   // "" too: the layout for a board met in Loader mode
    QVERIFY(rec.log.contains(QStringLiteral("Flash layout of board 0225ce3c8d8d6e44: ") + kNodeMtdParts));
    emit job->socFlasher()->statusChanged(board);
    emit job->socFlasher()->statusChanged(board);
    QCoreApplication::processEvents();
    QCOMPARE(runner.count(read), 1);   // once per board

    // The owner's memory knows another board: no read for it.
    FakeToolRunner runner2;
    auto second = makeJob(&runner2);
    second->setLayoutProvider([](const QString& serial) {
        return serial == QLatin1String("c3d9") ? QStringLiteral("x:1M(a)") : QString();
    });
    board.serial = QStringLiteral("c3d9");
    emit second->socFlasher()->statusChanged(board);
    QCOMPARE(runner2.count(QRegularExpression(QStringLiteral("cat /proc/cmdline"))), 0);
    QCOMPARE(second->boardLayout(), QStringLiteral("x:1M(a)"));

    // What a flash reads in its step 2 is kept for the flow's board.
    emit second->socFlasher()->mtdPartsRead(QStringLiteral("y:2M(b)"));
    QCOMPARE(second->mtdParts(QStringLiteral("c3d9")), QStringLiteral("y:2M(b)"));
    QCOMPARE(second->boardLayout(), QStringLiteral("y:2M(b)"));   // the job's own memory first
}

void Tst_firmwarejob::layoutReadFinishedOnBothEnds()
{
    // The status's /proc/cmdline read ends with layoutReadFinished(serial, ok) whatever came of it,
    // so an owner (su-fw status) knows when it is over without reading log texts.
    SocFlasher::DeviceStatus board;
    board.mode = SocFlasher::DeviceStatus::Mode::Adb;
    board.serial = QStringLiteral("0225ce3c8d8d6e44");
    board.detail = QStringLiteral("device");
    const QRegularExpression cmdline(QStringLiteral("cat /proc/cmdline"));
    {
        FakeToolRunner runner;
        runner.addScript(cmdline, kNodeCmdline);
        auto job = makeJob(&runner);
        QStringList order;
        connect(job.get(), &FirmwareJob::mtdPartsRead, job.get(), [&order]() { order << QStringLiteral("mtdPartsRead"); });
        QSignalSpy finished(job.get(), &FirmwareJob::layoutReadFinished);
        connect(job.get(), &FirmwareJob::layoutReadFinished, job.get(), [&order]() { order << QStringLiteral("layoutReadFinished"); });
        emit job->socFlasher()->statusChanged(board);
        QTRY_COMPARE(finished.count(), qsizetype(1));
        QCOMPARE(finished.at(0).at(0).toString(), board.serial);
        QCOMPARE(finished.at(0).at(1).toBool(), true);
        QCOMPARE(order, (QStringList{QStringLiteral("mtdPartsRead"), QStringLiteral("layoutReadFinished")}));
    }
    {
        FakeToolRunner runner;
        runner.addScript(cmdline, "console=ttyFIQ0 root=/dev/mtdblock5 rootwait\n");   // no mtdparts
        auto job = makeJob(&runner);
        QSignalSpy layouts(job.get(), &FirmwareJob::mtdPartsRead);
        QSignalSpy finished(job.get(), &FirmwareJob::layoutReadFinished);
        emit job->socFlasher()->statusChanged(board);
        QTRY_COMPARE(finished.count(), qsizetype(1));
        QCOMPARE(finished.at(0).at(1).toBool(), false);
        QCOMPARE(layouts.count(), qsizetype(0));
    }
    {
        FakeToolRunner runner;
        runner.addHandler(cmdline, [](FakeToolInvocation* i) { i->failToStart(QStringLiteral("no adb here")); });
        auto job = makeJob(&runner);
        Recorder rec(job.get());
        QSignalSpy finished(job.get(), &FirmwareJob::layoutReadFinished);
        emit job->socFlasher()->statusChanged(board);
        QTRY_COMPARE(finished.count(), qsizetype(1));
        QCOMPARE(finished.at(0).at(1).toBool(), false);
        QVERIFY(rec.log.contains(QStringLiteral("Cannot read the flash layout of board 0225ce3c8d8d6e44: no adb here")));
    }
}

void Tst_firmwarejob::layoutProviderAndOwnMemory()
{
    FakeToolRunner runner;
    auto job = makeJob(&runner);
    QVERIFY(job->boardLayout().isEmpty());
    QString remembered = QStringLiteral("a:1M(one)");
    job->setLayoutProvider([&remembered](const QString& serial) { return serial.isEmpty() ? remembered : QString(); });
    QCOMPARE(job->boardLayout(), remembered);
    remembered = QStringLiteral("a:2M(two)");   // asked live, never copied
    QCOMPARE(job->boardLayout(), QStringLiteral("a:2M(two)"));
    job->setMtdParts(QStringLiteral(" b:3M(three) "));
    QCOMPARE(job->mtdParts(), QStringLiteral("b:3M(three)"));
    QCOMPARE(job->boardLayout(), QStringLiteral("b:3M(three)"));
    job->setMtdParts(QString());   // forgotten: the provider again
    QCOMPARE(job->boardLayout(), QStringLiteral("a:2M(two)"));
    job->setMtdParts(QStringLiteral("c:1M(c)"), QStringLiteral("s1"));
    QCOMPARE(job->mtdParts(QStringLiteral("s1")), QStringLiteral("c:1M(c)"));
    QCOMPARE(job->boardLayout(), QStringLiteral("a:2M(two)"));   // s1 is no board seen on ADB
}

void Tst_firmwarejob::layoutFromTheImage()
{
    FakeToolRunner runner;
    scriptSocFlash(runner, kLdLoader);
    auto job = makeJob(&runner);
    const QString older =
        QStringLiteral("sfc_nor:64K(env),256K@64K(idblock),256K(uboot),384K(meta),4M(boot),5M(rootfs),2M(userdata)");
    job->setMtdParts(older);
    // nodeExtractor() gives an image under a folder "withenv..." the Node layout in its env.img.
    const QString image = tempPath(QStringLiteral("withenv-%1/update.img").arg(m_caseNumber));
    QVERIFY(writeFile(image, rkfwBytes()));
    QVERIFY(job->setImagePath(image));
    QVERIFY(waitForPlan(*job));
    QVERIFY(job->usingImageLayout());
    QCOMPARE(job->imageLayout(), kNodeMtdParts);
    QCOMPARE(job->flashLayout(), kNodeMtdParts);   // the image's wins
    QCOMPARE(job->boardLayout(), older);
    QCOMPARE(job->plannedImage(), image);
    QCOMPARE(job->selectedPartitions(), nodeNames());
    Recorder rec(job.get());
    job->flashSoc();
    QVERIFY(rec.waitForOperation());
    QVERIFY2(rec.lastOperation().ok, qPrintable(rec.lastOperation().message));
    QCOMPARE(wlCommands(runner).size(), qsizetype(7));
    QVERIFY(rec.logText().contains(QStringLiteral("flash layout in use (from the image's env.img)")));
}

void Tst_firmwarejob::refreshStatusSkippedWhileBusy()
{
    FakeToolRunner runner;
    scriptNoBoard(runner);
    auto job = makeJob(&runner);
    job->refreshStatus();
    QTRY_VERIFY(job->hasStatus());
    const QString cache = caseDir(QStringLiteral("cache"));
    QVERIFY(writeFile(cache + QStringLiteral("/update.img"), rkfwBytes()));
    job->setCacheDirectory(cache);
    Recorder rec(job.get());
    job->flashSoc();
    QTRY_COMPARE_WITH_TIMEOUT(job->socFlasher()->phase(), SocFlasher::Phase::WaitingForLoader, kOpTimeoutMs);
    // A status query starts its first command at once; none may start while the flash owns the tools.
    const qsizetype before = runner.invocations().size();
    job->refreshStatus();
    QCOMPARE(runner.invocations().size(), before);
    job->cancel();
    QVERIFY(rec.waitForOperation());
    QTRY_VERIFY(runner.liveInvocations().isEmpty());
    const qsizetype idle = runner.invocations().size();
    job->refreshStatus();   // idle again: it runs
    QVERIFY(runner.invocations().size() > idle);
    QVERIFY(runner.invocations().at(idle).startsWith(QStringLiteral("upgrade_tool LD")));
}

void Tst_firmwarejob::planStatesAndSignals()
{
    FakeToolRunner runner;
    scriptSocFlash(runner, kLdLoader);
    auto job = makeJob(&runner);
    job->setMtdParts(kNodeMtdParts);
    QSignalSpy changed(job.get(), &FirmwareJob::partitionsChanged);
    QSignalSpy planned(job.get(), &FirmwareJob::partitionsPlanned);
    Recorder rec(job.get());
    const QString image = caseDir(QStringLiteral("plan/update.img"));
    QVERIFY(writeFile(image, rkfwBytes()));
    QVERIFY(job->setImagePath(image));
    QCOMPARE(job->planState(), FirmwareJob::PlanState::Planning);
    QVERIFY(changed.count() >= 1);
    QVERIFY(job->partitions().isEmpty());   // nothing until it is ready
    QVERIFY(waitForPlan(*job));
    QCOMPARE(planned.count(), qsizetype(1));
    QCOMPARE(job->plannedImage(), image);
    QCOMPARE(runner.count(exfPattern()), 1);
    QVERIFY(rec.log.contains(QStringLiteral("Partitions of update.img: env@0x0, idblock@0x10000, uboot@0x50000, "
                                            "meta@0x90000, boot@0xf0000, rootfs@0x3f0000, userdata@0x8f0000")));
    QCOMPARE(job->partitions().at(4).offset, quint64(0xf0000));

    // Planned again for the same image: the flasher reuses its unpacked files.
    job->planPartitions();
    QCOMPARE(job->planState(), FirmwareJob::PlanState::Planning);
    QVERIFY(waitForPlan(*job));
    QCOMPARE(planned.count(), qsizetype(2));
    QCOMPARE(runner.count(exfPattern()), 1);

    // No image: NoImage; no upgrade_tool: Failed with the reason.
    QVERIFY(job->setImagePath(QString()));
    QCOMPARE(job->planState(), FirmwareJob::PlanState::NoImage);
    job->setToolPaths(QString(), QStringLiteral("adb"));
    QVERIFY(job->setImagePath(image));
    QCOMPARE(job->planState(), FirmwareJob::PlanState::Failed);
    QCOMPARE(job->planMessage(), QStringLiteral("upgrade_tool not found"));
}

void Tst_firmwarejob::selectionApi()
{
    FakeToolRunner runner;
    scriptSocFlash(runner, kLdLoader);
    auto job = makeJob(&runner);
    job->setMtdParts(kNodeMtdParts);
    const QString image = caseDir(QStringLiteral("selection/update.img"));
    QVERIFY(writeFile(image, rkfwBytes()));
    QVERIFY(job->setImagePath(image));
    QVERIFY(waitForPlan(*job));
    QSignalSpy changed(job.get(), &FirmwareJob::partitionsChanged);
    QCOMPARE(job->selectedPartitions(), nodeNames());
    QVERIFY(job->socFlasher()->selectedPartitions().isEmpty());   // "every row": the flasher's default

    job->setSelectedPartitions({QStringLiteral(" boot "), QStringLiteral("rootfs"), QStringLiteral("boot"), QStringLiteral("nope")});
    QVERIFY(!job->allPartitionsSelected());
    QCOMPARE(job->selectedPartitions(), (QStringList{QStringLiteral("boot"), QStringLiteral("rootfs")}));
    QVERIFY(job->socFlasher()->selectedPartitions().contains(QStringLiteral("boot")));
    QVERIFY(job->socFlasher()->selectedPartitions().contains(QStringLiteral("rootfs")));
    QCOMPARE(changed.count(), qsizetype(1));

    job->setSelectedPartitions({});   // "None": kept by the job, the flasher keeps its last list
    QVERIFY(job->selectedPartitions().isEmpty());
    job->selectAllPartitions();
    QVERIFY(job->allPartitionsSelected());
    QCOMPARE(job->selectedPartitions(), nodeNames());
    QVERIFY(job->socFlasher()->selectedPartitions().isEmpty());
    QCOMPARE(changed.count(), qsizetype(3));

    // Another image starts with every row selected again.
    job->setSelectedPartitions({QStringLiteral("boot")});
    const QString second = caseDir(QStringLiteral("selection/second.img"));
    QVERIFY(writeFile(second, rkfwBytes()));
    QVERIFY(job->setImagePath(second));
    QVERIFY(waitForPlan(*job));
    QVERIFY(job->allPartitionsSelected());
    QCOMPARE(job->selectedPartitions(), nodeNames());
}

// =======================================================================================
// MCU
// =======================================================================================

void Tst_firmwarejob::readMcuInfoStopsStaleRfmcuFirst()
{
    const QString stopCommand = QStringLiteral("adb shell \"killall rfmcu 2>/dev/null; true\"");
    const QString versionCommand = QStringLiteral("adb shell \"rfmcu cmd version\"");
    FakeToolRunner runner;
    auto consoleRunning = std::make_shared<bool>(true);
    runner.addHandler(QRegularExpression(QStringLiteral("killall rfmcu")), [consoleRunning](FakeToolInvocation* i) {
        *consoleRunning = false;
        i->finish(1);   // killall's own status is ignored
    });
    runner.addHandler(QRegularExpression(QStringLiteral("rfmcu cmd version")), [consoleRunning](FakeToolInvocation* i) {
        i->emitOutput(*consoleRunning ? QByteArray("ERR PORT busy\r\n") : QByteArray("OK version=0.8.1 role=tx\r\n"));
        i->finish(*consoleRunning ? 2 : 0);
    });
    auto job = makeJob(&runner);
    QSignalSpy roles(job.get(), &FirmwareJob::roleChanged);
    QSignalSpy info(job.get(), &FirmwareJob::mcuInfoChanged);
    Recorder rec(job.get());
    job->readMcuInfo();
    QCOMPARE(job->currentOperation(), op(FirmwareJob::kOpInfo));
    QVERIFY(job->isCancellable());   // from the first command on
    QVERIFY(rec.waitForOperation());
    QVERIFY2(rec.lastOperation().ok, qPrintable(rec.lastOperation().message));
    QCOMPARE(rec.lastOperation().name, QStringLiteral("info"));
    QCOMPARE(rec.lastOperation().message, QStringLiteral("MCU application version=0.8.1 role=tx"));
    QCOMPARE(runner.invocations(), (QStringList{stopCommand, versionCommand}));
    QCOMPARE(job->runningMcuVersion(), QStringLiteral("0.8.1"));
    QCOMPARE(job->runningMcuRole(), QStringLiteral("tx"));
    QCOMPARE(job->role(), static_cast<quint32>(CcProtocol::RoleTx));   // the reported role is pre-selected
    QCOMPARE(roles.count(), qsizetype(1));
    QCOMPARE(info.count(), qsizetype(1));
    QVERIFY(rec.log.contains(stopCommand));
    QVERIFY(rec.log.contains(QStringLiteral("OK version=0.8.1 role=tx")));   // the tool's answer is logged
}

void Tst_firmwarejob::readMcuInfoRoleFromHelp()
{
    FakeToolRunner runner;
    runner.addScript(QRegularExpression(QStringLiteral("killall rfmcu")), QByteArray());
    runner.addScript(QRegularExpression(QStringLiteral("rfmcu cmd version")), "OK version=0.8.2 baud=115200\r\n");
    runner.addScript(QRegularExpression(QStringLiteral("rfmcu cmd help")),
                     "OK commands:\r\nOK help [command]\r\nOK version show firmware version\r\nOK rx rx status | rx dump on|off\r\n");
    auto job = makeJob(&runner);
    Recorder rec(job.get());
    job->readMcuInfo();
    QVERIFY(rec.waitForOperation());
    QVERIFY(rec.lastOperation().ok);
    QCOMPARE(runner.count(QRegularExpression(QStringLiteral("rfmcu cmd help"))), 1);
    QCOMPARE(job->runningMcuVersion(), QStringLiteral("0.8.2"));
    QCOMPARE(job->runningMcuRole(), QStringLiteral("rx"));
    QCOMPARE(job->role(), static_cast<quint32>(CcProtocol::RoleRx));
}

void Tst_firmwarejob::readMcuInfoFailures()
{
    const struct
    {
        const char* name;
        QByteArray output;
        int exit;
        QString message;
    } cases[] = {
        {"busy", "ERR PORT busy: /dev/ttyS1 is held by rfmcu console (pid 190)\r\n", 2,
         QStringLiteral("the MCU port is busy (another rfmcu holds /dev/ttyS1): ERR PORT busy: /dev/ttyS1 is held by rfmcu console (pid 190)")},
        {"no reply", QByteArray(), 3, QStringLiteral("the MCU did not answer \"version\"")},
        {"error", "ERR CMD unknown_command; use help\r\n", 1,
         QStringLiteral("rfmcu cmd version failed (exit code 1): ERR CMD unknown_command; use help")},
        {"garbage", "hello\r\n", 0, QStringLiteral("unexpected reply to \"version\": hello")},
    };
    for (const auto& c : cases) {
        FakeToolRunner runner;
        runner.addScript(QRegularExpression(QStringLiteral("killall rfmcu")), QByteArray());
        runner.addScript(QRegularExpression(QStringLiteral("rfmcu cmd version")), c.output, c.exit);
        auto job = makeJob(&runner);
        Recorder rec(job.get());
        job->readMcuInfo();
        QVERIFY2(rec.waitForOperation(), c.name);
        QCOMPARE(rec.lastOperation().ok, false);
        QCOMPARE(rec.lastOperation().message, c.message);
        QVERIFY(job->runningMcuVersion().isEmpty());
        QCOMPARE(job->role(), 0u);
    }
    // adb that cannot run: the reason; rfmcu cmd never runs.
    FakeToolRunner runner;
    runner.addHandler(QRegularExpression(QStringLiteral("killall rfmcu")),
                      [](FakeToolInvocation* i) { i->failToStart(QStringLiteral("no adb here")); });
    auto job = makeJob(&runner);
    Recorder rec(job.get());
    job->readMcuInfo();
    QVERIFY(rec.waitForOperation());
    QCOMPARE(rec.lastOperation().message, QStringLiteral("adb could not be started: no adb here"));
    QCOMPARE(runner.count(QRegularExpression(QStringLiteral("rfmcu cmd"))), 0);
}

void Tst_firmwarejob::cancelDuringMcuRead()
{
    FakeToolRunner runner;
    runner.addHandler(QRegularExpression(QStringLiteral("killall rfmcu")), [](FakeToolInvocation* i) { i->hang(); });
    auto job = makeJob(&runner);
    Recorder rec(job.get());
    job->readMcuInfo();
    QTRY_COMPARE(runner.liveInvocations().size(), qsizetype(1));
    QVERIFY(job->isCancellable());
    job->cancel();
    QVERIFY(rec.waitForOperation());
    QCOMPARE(rec.lastOperation().message, QStringLiteral("cancelled"));
    QVERIFY(rec.log.contains(QStringLiteral("Cancelling Read MCU")));
    QCOMPARE(runner.count(QRegularExpression(QStringLiteral("rfmcu cmd"))), 0);
    QVERIFY(!job->isBusy());
}

void Tst_firmwarejob::updateMcuThroughFakeBridge()
{
    // The whole MCU path: adb, the bridge, the fw_update protocol against FakeMcu; progress per ACK,
    // the restart banner, the link taken down again.
    FakeMcu mcu;
    FakeMcuServer bridge(&mcu);
    QVERIFY(bridge.listen());
    FakeToolRunner runner;
    scriptMcuBridge(runner, bridge.port());
    auto job = makeJob(&runner);
    QVERIFY(job->setPackagePath(dataFile("firmware-0.8.1.pkg")));
    job->setRole(CcProtocol::RoleRx);
    Recorder rec(job.get());
    QSignalSpy info(job.get(), &FirmwareJob::mcuInfoChanged);
    job->updateMcu();
    QCOMPARE(job->currentOperation(), op(FirmwareJob::kOpMcu));
    QVERIFY(job->isCancellable());
    QVERIFY(rec.waitForOperation(1, kChainTimeoutMs));
    QVERIFY2(rec.lastOperation().ok, qPrintable(rec.lastOperation().message));
    QCOMPARE(mcu.image(), job->package().image);   // every byte arrived, in order
    QCOMPARE(mcu.role(), static_cast<quint32>(CcProtocol::RoleRx));
    const QStringList commands = mcu.commands();
    QVERIFY(commands.indexOf(QStringLiteral("power")) >= 0);
    QVERIFY(commands.indexOf(QStringLiteral("power")) < commands.indexOf(QStringLiteral("bootloader")));
    // Progress per acknowledged DATA frame: one percentage per 128-byte chunk, ending at 100.
    int perAck = 0;
    int last = -1;
    for (const auto& p : std::as_const(rec.progress)) {
        if (p.first.startsWith(QStringLiteral("Updating MCU: "))) {
            ++perAck;
            last = p.second;
        }
    }
    QVERIFY2(perAck >= int(job->package().size / 128), qPrintable(QString::number(perAck)));
    QCOMPARE(last, 100);
    QVERIFY(rec.persistent.last().startsWith(QStringLiteral("Updating MCU: ")));
    QVERIFY(rec.logText().contains(QStringLiteral("Updating the MCU with firmware-0.8.1.pkg (role rx)")));
    QVERIFY(rec.logText().contains(QStringLiteral("COMPLETE")));
    QVERIFY(rec.logText().contains(QStringLiteral("INFO application restarted: version=0.8.1")));
    QCOMPARE(job->runningMcuVersion(), QStringLiteral("0.8.1"));   // from the restart banner
    QVERIFY(info.count() >= 1);
    QTRY_VERIFY(!job->mcuLink()->isOpen());
    QVERIFY(runner.count(QRegularExpression(QStringLiteral("forward --remove"))) >= 1);
}

void Tst_firmwarejob::setRoleThroughFakeBridge()
{
    FakeMcu mcu;
    FakeMcuServer bridge(&mcu);
    QVERIFY(bridge.listen());
    FakeToolRunner runner;
    scriptMcuBridge(runner, bridge.port());
    auto job = makeJob(&runner);
    job->setRole(CcProtocol::RoleTx);
    Recorder rec(job.get());
    job->setMcuRole();
    QCOMPARE(job->currentOperation(), op(FirmwareJob::kOpSetRole));
    QVERIFY(rec.waitForOperation(1, kChainTimeoutMs));
    QCOMPARE(rec.lastOperation().name, QStringLiteral("set-role"));
    QVERIFY2(rec.lastOperation().ok, qPrintable(rec.lastOperation().message));
    QCOMPARE(mcu.role(), static_cast<quint32>(CcProtocol::RoleTx));
    QCOMPARE(job->runningMcuRole(), QStringLiteral("tx"));
    QVERIFY(mcu.image().isEmpty());   // no transfer
    QVERIFY(rec.log.contains(QStringLiteral("Setting the MCU role to tx")));
}

void Tst_firmwarejob::mcuLinkFailureEndsTheStep()
{
    FakeToolRunner runner;
    runner.addScript(QRegularExpression(QStringLiteral("^adb( -s \\S+)? devices$")), kAdbBoard);
    runner.addScript(QRegularExpression(QStringLiteral("command -v rfmcu")), QByteArray());   // an old SoC firmware
    auto job = makeJob(&runner);
    QVERIFY(job->setPackagePath(dataFile("firmware-0.8.1.pkg")));
    job->setRole(CcProtocol::RoleRx);
    Recorder rec(job.get());
    job->updateMcu();
    QVERIFY(rec.waitForOperation());
    QCOMPARE(rec.lastOperation().ok, false);
    QVERIFY2(rec.lastOperation().message.startsWith(QStringLiteral("board 0225ce3c8d8d6e44 has no rfmcu")),
             qPrintable(rec.lastOperation().message));
    QCOMPARE(runner.count(QRegularExpression(QStringLiteral("forward tcp:0"))), 0);
}

void Tst_firmwarejob::destroyedJobCleansUpTheBoard()
{
    // The job goes away while an MCU update runs: `killall rfmcu` and the forward removal are sent
    // detached, so they survive the tool runner.
    FakeMcu mcu;
    mcu.setSilent(true);   // the update waits in its first probe: the link is open when the job goes
    FakeMcuServer bridge(&mcu);
    QVERIFY(bridge.listen());
    FakeToolRunner runner;
    scriptMcuBridge(runner, bridge.port());
    auto job = makeJob(&runner);
    QVERIFY(job->setPackagePath(dataFile("firmware-0.8.1.pkg")));
    job->setRole(CcProtocol::RoleRx);
    job->updateMcu();
    QTRY_VERIFY_WITH_TIMEOUT(job->mcuLink()->isOpen(), kOpTimeoutMs);
    const int before = static_cast<int>(runner.invocations().size());
    job.reset();
    const QStringList after = runner.invocations().mid(before);
    QCOMPARE(after.size(), qsizetype(2));
    QVERIFY2(after.at(0).endsWith(QStringLiteral("shell \"killall rfmcu 2>/dev/null; true\"")), qPrintable(after.at(0)));
    QVERIFY2(after.at(1).contains(QStringLiteral("forward --remove tcp:")), qPrintable(after.at(1)));
    QVERIFY(runner.optionsAt(before).detached);
    QVERIFY(runner.optionsAt(before + 1).detached);
}

// =======================================================================================
// Pull & Flash
// =======================================================================================

namespace {

/// A Pull & Flash rig: the build machine, a MASKROM board flashed with UF, the version read
/// answered by `versionReply` / `versionExit`, the bridge to `mcu`.
struct ChainRig
{
    SshProfileStore store;
    BuildMachine machine;
    FakeMcu mcu;
    std::unique_ptr<FakeMcuServer> bridge;
    FakeToolRunner runner;
    std::unique_ptr<FirmwareJob> job;
    std::shared_ptr<QByteArray> versionReply = std::make_shared<QByteArray>("OK version=0.8.1 role=rx\n");
    std::shared_ptr<int> versionExit = std::make_shared<int>(0);

    bool start(const QString& workDir, const QString& cache, const QByteArray& ld = kLdMaskrom)
    {
        if (!machine.start(&store, workDir)) {
            return false;
        }
        bridge = std::make_unique<FakeMcuServer>(&mcu);
        if (!bridge->listen()) {
            return false;
        }
        scriptSocFlash(runner, ld);
        auto reply = versionReply;
        auto exitCode = versionExit;
        runner.addHandler(QRegularExpression(QStringLiteral("rfmcu cmd version")), [reply, exitCode](FakeToolInvocation* i) {
            if (!reply->isEmpty()) {
                i->emitOutput(*reply);
            }
            i->finish(*exitCode);
        });
        scriptMcuBridge(runner, bridge->port());
        job = makeJob(&runner, &store);
        job->setSourceTarget(QStringLiteral("ssh:profile:") + machine.profile.id);
        job->setRemoteFolder(machine.imageFolder);
        job->setCacheDirectory(cache);
        return true;
    }
};

} // namespace

void Tst_firmwarejob::pullFlashSkipsMcuWhenEqual()
{
    ChainRig rig;
    QVERIFY(rig.start(caseDir(QStringLiteral("work")), caseDir(QStringLiteral("cache"))));
    FirmwareJob* job = rig.job.get();
    QVERIFY(job->setPackagePath(dataFile("firmware-0.8.1.pkg")));
    Recorder rec(job);
    Transport::State sshAfterPull = Transport::State::Disconnected;
    connect(job, &FirmwareJob::stepFinished, &rec, [&](const QString& step) {
        if (step == QLatin1String("pull")) {
            sshAfterPull = job->sshConnection()->state();   // keepSsh: held across the chain
        }
    });
    job->pullAndFlash();
    QCOMPARE(job->currentOperation(), op(FirmwareJob::kOpPullFlash));
    QVERIFY(rec.waitForOperation(1, kChainTimeoutMs));
    QCOMPARE(rec.stepNames(), (QStringList{QStringLiteral("pull"), QStringLiteral("flash"), QStringLiteral("mcu")}));
    for (const Result& r : std::as_const(rec.steps)) {
        QVERIFY2(r.ok, qPrintable(r.name + QStringLiteral(": ") + r.message));
    }
    QCOMPARE(rec.steps.at(2).message, QStringLiteral("the MCU already runs 0.8.1; update skipped"));
    QCOMPARE(rec.lastOperation().name, QStringLiteral("pull-flash"));
    QVERIFY(rec.lastOperation().ok);
    QCOMPARE(rec.lastOperation().message, rec.steps.at(2).message);
    QCOMPARE(rec.busyChanges, (QList<bool>{true, false}));   // one busy period for the whole chain
    QCOMPARE(sshAfterPull, Transport::State::Connected);
    QTRY_COMPARE(job->sshConnection()->state(), Transport::State::Disconnected);
    QCOMPARE(rig.runner.count(QRegularExpression(QStringLiteral("^upgrade_tool UF "))), 1);
    QCOMPARE(rig.runner.count(QRegularExpression(QStringLiteral("forward tcp:0"))), 0);   // no MCU transfer
    const QString log = rec.logText();
    QVERIFY(log.contains(QStringLiteral("Pull & Flash: pull the image, flash the SoC, wait for ADB, then update the MCU if needed")));
    QVERIFY(log.contains(QStringLiteral("Pull: ")));
    QVERIFY(log.contains(QStringLiteral("Flash SoC: Upgrade firmware ok; board is back on ADB")));
    QVERIFY(log.contains(QStringLiteral("Pull & Flash done: the MCU already runs 0.8.1; update skipped")));
    // The version read stopped a stale rfmcu first.
    const QStringList commands = rig.runner.invocations();
    const qsizetype stop = commands.indexOf(QRegularExpression(QStringLiteral(".*killall rfmcu.*")));
    QVERIFY(stop >= 0);
    QVERIFY(stop < commands.indexOf(QRegularExpression(QStringLiteral(".*rfmcu cmd version.*"))));
    QVERIFY(job->localImage().isEmpty());
    QVERIFY(rig.runner.invocations().filter(QStringLiteral("UF ")).first().contains(QStringLiteral("update.img")));
}

void Tst_firmwarejob::pullFlashUpdatesMcuWhenDifferent()
{
    ChainRig rig;
    *rig.versionReply = "OK version=0.8.0 role=rx\n";
    QVERIFY(rig.start(caseDir(QStringLiteral("work")), caseDir(QStringLiteral("cache"))));
    FirmwareJob* job = rig.job.get();
    QVERIFY(job->setPackagePath(dataFile("firmware-0.8.1.pkg")));
    Recorder rec(job);
    job->pullAndFlash();
    QVERIFY(rec.waitForOperation(1, kChainTimeoutMs));
    QVERIFY2(rec.lastOperation().ok, qPrintable(rec.lastOperation().message));
    QCOMPARE(rec.stepNames(), (QStringList{QStringLiteral("pull"), QStringLiteral("flash"), QStringLiteral("mcu")}));
    QCOMPARE(job->role(), static_cast<quint32>(CcProtocol::RoleRx));   // the reported role, read after the SoC came back
    QCOMPARE(rig.mcu.image(), job->package().image);
    QCOMPARE(rig.runner.count(QRegularExpression(QStringLiteral("forward tcp:0"))), 1);
    QCOMPARE(job->runningMcuVersion(), QStringLiteral("0.8.1"));   // the banner after the update
    QTRY_VERIFY(!job->mcuLink()->isOpen());
}

void Tst_firmwarejob::pullFlashUpdatesSilentMcu()
{
    // Runbook Part J: the CC1310 sits in its UART updater and has no CLI (rfmcu cmd exits 3): it
    // is updated anyway, with the role confirmed beforehand.
    ChainRig rig;
    rig.mcu.setInUpdater(true);
    *rig.versionReply = QByteArray();
    *rig.versionExit = 3;
    QVERIFY(rig.start(caseDir(QStringLiteral("work")), caseDir(QStringLiteral("cache"))));
    FirmwareJob* job = rig.job.get();
    QVERIFY(job->setPackagePath(dataFile("firmware-0.8.1.pkg")));
    job->setRole(CcProtocol::RoleRx);
    Recorder rec(job);
    job->pullAndFlash();
    QVERIFY(rec.waitForOperation(1, kChainTimeoutMs));
    QVERIFY2(rec.lastOperation().ok, qPrintable(rec.lastOperation().message));
    QCOMPARE(rig.mcu.image(), job->package().image);
    QVERIFY(rig.mcu.commands().isEmpty());   // no CLI involved
    QVERIFY(!rig.mcu.inUpdater());           // the application runs again
    QVERIFY(rec.logText().contains(QStringLiteral("MCU version unknown (the MCU did not answer \"version\"): it may sit in "
                                                  "its UART updater; updating it")));
}

void Tst_firmwarejob::pullFlashSilentMcuWithoutRoleStops()
{
    ChainRig rig;
    *rig.versionReply = QByteArray();
    *rig.versionExit = 3;
    QVERIFY(rig.start(caseDir(QStringLiteral("work")), caseDir(QStringLiteral("cache"))));
    FirmwareJob* job = rig.job.get();
    QVERIFY(job->setPackagePath(dataFile("firmware-0.8.1.pkg")));
    Recorder rec(job);
    job->pullAndFlash();
    QVERIFY(rec.waitForOperation(1, kChainTimeoutMs));
    QCOMPARE(rec.lastOperation().ok, false);
    QCOMPARE(rec.lastOperation().message,
             QStringLiteral("stopped at Update MCU: cannot read the running MCU version (the MCU did not answer \"version\") "
                            "and no role is confirmed: choose rx or tx, then press Update MCU"));
    QCOMPARE(rig.runner.count(QRegularExpression(QStringLiteral("forward tcp:0"))), 0);
    QCOMPARE(rec.steps.last().name, QStringLiteral("mcu"));
    QCOMPARE(rec.steps.last().ok, false);
}

void Tst_firmwarejob::pullFlashStopsAtFirstFailure()
{
    // The flash fails (no board within the wait): the chain stops there, the MCU is not touched,
    // the build-machine connection is closed.
    ChainRig rig;
    QVERIFY(rig.start(caseDir(QStringLiteral("work")), caseDir(QStringLiteral("cache"))));
    FirmwareJob* job = rig.job.get();
    rig.runner.reset();
    scriptNoBoard(rig.runner);
    job->setLoaderWaitSeconds(1);
    QVERIFY(job->setPackagePath(dataFile("firmware-0.8.1.pkg")));
    Recorder rec(job);
    job->pullAndFlash();
    QVERIFY(rec.waitForOperation(1, kChainTimeoutMs));
    QCOMPARE(rec.stepNames(), (QStringList{QStringLiteral("pull"), QStringLiteral("flash")}));
    QVERIFY(rec.steps.at(0).ok);
    QCOMPARE(rec.steps.at(1).ok, false);
    QCOMPARE(rec.lastOperation().name, QStringLiteral("pull-flash"));
    QCOMPARE(rec.lastOperation().ok, false);
    QCOMPARE(rec.lastOperation().message, QStringLiteral("stopped at Flash SoC: board did not enter MASKROM/Loader within 1 s"));
    QVERIFY(rec.log.contains(QStringLiteral("Flash SoC failed: board did not enter MASKROM/Loader within 1 s")));
    QCOMPARE(rig.runner.count(QRegularExpression(QStringLiteral("rfmcu"))), 0);
    QTRY_COMPARE(job->sshConnection()->state(), Transport::State::Disconnected);
    QVERIFY(!job->isBusy());

    // A failing pull stops before anything reaches the board.
    rig.runner.reset();
    job->setSourceTarget(QString());
    Recorder second(job);
    job->pullAndFlash();
    QCOMPARE(second.order, (QStringList{QStringLiteral("step:pull"), QStringLiteral("op:pull-flash")}));
    QCOMPARE(second.lastOperation().message,
             QStringLiteral("stopped at Pull: enter the build machine (an SSH profile or user@host)"));
    QVERIFY(rig.runner.invocations().isEmpty());
}

void Tst_firmwarejob::pullFlashWithoutPackage()
{
    ChainRig rig;
    QVERIFY(rig.start(caseDir(QStringLiteral("work")), caseDir(QStringLiteral("cache"))));
    FirmwareJob* job = rig.job.get();
    Recorder rec(job);
    job->pullAndFlash();
    QVERIFY(rec.waitForOperation(1, kChainTimeoutMs));
    QVERIFY2(rec.lastOperation().ok, qPrintable(rec.lastOperation().message));
    QCOMPARE(rec.lastOperation().message, QStringLiteral("SoC flashed; no MCU package chosen"));
    QCOMPARE(rec.stepNames(), (QStringList{QStringLiteral("pull"), QStringLiteral("flash")}));
    QVERIFY(rec.log.contains(QStringLiteral("No MCU package chosen: the MCU update is skipped")));
    QCOMPARE(rig.runner.count(QRegularExpression(QStringLiteral("rfmcu"))), 0);
}

void Tst_firmwarejob::pullFlashWritesEveryPartition()
{
    // Loader mode: Pull & Flash writes every row, whatever was selected before.
    ChainRig rig;
    QVERIFY(rig.start(caseDir(QStringLiteral("work")), caseDir(QStringLiteral("cache")), kLdLoader));
    FirmwareJob* job = rig.job.get();
    job->setMtdParts(kNodeMtdParts);
    Recorder rec(job);
    job->pull();
    QVERIFY(rec.waitForOperation());
    QVERIFY2(rec.lastOperation().ok, qPrintable(rec.lastOperation().message));
    QVERIFY(waitForPlan(*job));
    job->setSelectedPartitions({QStringLiteral("boot")});
    job->pullAndFlash();
    QVERIFY(rec.waitForOperation(2, kChainTimeoutMs));
    QVERIFY2(rec.lastOperation().ok, qPrintable(rec.lastOperation().message));
    const QString dir = extractDirOf(caseDir(QStringLiteral("cache")) + QStringLiteral("/update.img"));
    QStringList expected;
    for (int k = 0; k < kNodePartitionCount; ++k) {
        expected << expectedWl(k, dir);
    }
    QCOMPARE(wlCommands(rig.runner), expected);
    QCOMPARE(rig.runner.count(exfPattern()), 1);   // unpacked once, after the first pull
    QVERIFY(job->allPartitionsSelected());
    QVERIFY(job->socFlasher()->selectedPartitions().isEmpty());
    QVERIFY(rec.logText().contains(QStringLiteral("Partitions: every partition of the image")));
}

QTEST_GUILESS_MAIN(Tst_firmwarejob)
#include "tst_firmwarejob.moc"
