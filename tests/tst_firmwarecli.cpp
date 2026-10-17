// UI-free test suite for FirmwareCli (src/firmware/FirmwareCli.h), the logic of the su-fw console
// tool: every command in-process through FirmwareCli::run() on a FakeToolRunner (no adb, no
// upgrade_tool is ever started), the in-process TestSshServer as the build machine and FakeMcu behind
// a FakeMcuServer as the CC1310 - the fixtures of tst_firmwarejob. One case starts the real su-fw
// executable (--help, --version, a usage error); it never runs a command that would reach a tool.
// QSettings and every file live in temporary directories.
#include <QtTest>

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTimer>

#include <algorithm>
#include <memory>
#include <optional>

#include "Version.h"
#include "app/AppSettings.h"
#include "firmware/CcPackage.h"
#include "firmware/CcProtocol.h"
#include "firmware/FirmwareCli.h"
#include "firmware/FirmwareJob.h"
#include "firmware/FleetFlasher.h"
#include "firmware/FleetReport.h"
#include "firmware/McuLink.h"
#include "firmware/SocFlasher.h"
#include "firmware/ToolRunner.h"
#include "ssh/SecretStore.h"
#include "ssh/SshProfile.h"
#include "support/FakeMcu.h"
#include "support/FakeToolRunner.h"
#include "support/FakeUsbBus.h"
#include "support/FirmwareFixtures.h"
#include "support/TestSshServer.h"

using namespace FirmwareFixtures;

namespace {

const QString kSecret = QStringLiteral("Zq7-cli-Secret-4711");
const char* const kSecretVariable = "SU_FW_TEST_SECRET";
const QString kBoardSerial = QStringLiteral("0225ce3c8d8d6e44");

QString dataFile(const char* name)
{
    return QStringLiteral(SU_TEST_DATA_DIR "/firmware/") + QLatin1String(name);
}

/// What one run printed and returned.
struct CliResult
{
    int code = -1;
    QString out;
    QString err;

    QStringList lines() const { return out.split(QLatin1Char('\n'), Qt::SkipEmptyParts); }
    /// The lines without their "HH:MM:SS " stamp (an empty string for a line without one).
    QStringList texts() const
    {
        static const QRegularExpression stamp(QStringLiteral("^\\d\\d:\\d\\d:\\d\\d (.*)$"));
        QStringList result;
        for (const QString& line : lines()) {
            const QRegularExpressionMatch match = stamp.match(line);
            result << (match.hasMatch() ? match.captured(1) : QString());
        }
        return result;
    }
    bool hasText(const QString& text) const { return texts().contains(text); }
    bool hasTextStarting(const QString& prefix) const
    {
        for (const QString& text : texts()) {
            if (text.startsWith(prefix)) {
                return true;
            }
        }
        return false;
    }
    QString lastText() const { return texts().isEmpty() ? QString() : texts().last(); }
    QString dump() const { return QStringLiteral("exit %1\n--- out:\n%2--- err:\n%3").arg(code).arg(out, err); }
};

/// The JSON lines of a --json run (every one must parse).
QList<QJsonObject> jsonLines(const CliResult& result, bool* allParse)
{
    QList<QJsonObject> objects;
    *allParse = true;
    for (const QString& line : result.lines()) {
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(line.toUtf8(), &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            *allParse = false;
            continue;
        }
        objects << document.object();
    }
    return objects;
}

/// A FirmwareCli on a FakeToolRunner with the tests' tool names, fast polls and a profile store.
struct CliRig
{
    explicit CliRig(bool fixedTools = true)
    {
        if (fixedTools) {
            cli.setToolPaths(QStringLiteral("upgrade_tool"), QStringLiteral("adb"));
        }
        cli.setProfileStore(&store);
        cli.setJobSetup([this](FirmwareJob& job) {
            job.socFlasher()->setPollIntervalMs(10);
            lastJob = &job;
            if (setup) {
                setup(job);
            }
        });
    }

    CliResult run(const QStringList& arguments)
    {
        QBuffer out;
        QBuffer err;
        out.open(QIODevice::WriteOnly);
        err.open(QIODevice::WriteOnly);
        CliResult result;
        result.code = cli.run(arguments, &out, &err);
        result.out = QString::fromUtf8(out.data());
        result.err = QString::fromUtf8(err.data());
        return result;
    }

    /// What the "user" types: one entry per line, then end of input.
    void setInput(const QStringList& typed, bool interactive)
    {
        auto queue = std::make_shared<QStringList>(typed);
        auto asked = secretReads;
        cli.setInput(
            [queue, asked](bool secret) -> std::optional<QString> {
                if (secret) {
                    ++*asked;
                }
                if (queue->isEmpty()) {
                    return std::nullopt;
                }
                return queue->takeFirst();
            },
            interactive);
    }

    SshProfileStore store;
    FakeToolRunner runner;            // outlives every job the cli creates
    FirmwareCli cli{&runner};
    std::function<void(FirmwareJob&)> setup;
    FirmwareJob* lastJob = nullptr;   // valid during a run only
    std::shared_ptr<int> secretReads = std::make_shared<int>(0);
};

/// `upgrade_tool LD` sees nothing, `adb devices` the Node board, its /proc/cmdline has the layout.
void scriptAdbBoard(FakeToolRunner& runner)
{
    runner.addScript(QRegularExpression(QStringLiteral("^upgrade_tool LD$")), "List of rockusb connected(0)\r\n");
    runner.addScript(QRegularExpression(QStringLiteral("^adb( -s \\S+)? devices$")), kAdbBoard);
    runner.addScript(QRegularExpression(QStringLiteral("cat /proc/cmdline")), kNodeCmdline);
}

/// No board anywhere: the flasher waits for Loader mode.
void scriptNoBoard(FakeToolRunner& runner)
{
    runner.addScript(QRegularExpression(QStringLiteral("^upgrade_tool LD")), "List of rockusb connected(0)\r\n", 0);
    runner.addScript(QRegularExpression(QStringLiteral("^adb( -s \\S+)? devices")), "List of devices attached\r\n\r\n", 0);
    runner.addScript(QRegularExpression(QStringLiteral("^adb( -s \\S+)? get-state")), "error: no devices/emulators found\r\n", 1);
    runner.addHandler(exfPattern(), &nodeExtractor);
}

/// (v0.7.0) A fleet on CliRig: the seven bench boards on a FakeUsbBus whose enumeration and polls
/// the job's FleetFlasher gets (`extraSetup` adds the case's own settings).
struct FleetCliRig
{
    /// `bench` false: the test composes the bus (and its own handlers) and calls script() itself.
    explicit FleetCliRig(bool bench = true)
    {
        rig.setup = [this](FirmwareJob& job) {
            job.fleetFlasher()->setUsbEnumerator([this](quint16 vid) { return bus.enumerate(vid); });
            job.fleetFlasher()->setPollIntervalMs(10);
            if (extraSetup) {
                extraSetup(job);
            }
        };
        if (bench) {
            scriptFleet(bus, rig.runner);
        }
    }

    /// The bench without scriptFleet(): `absent` boards are not attached.
    void addBench(const QSet<int>& absent = {})
    {
        for (int k = 0; k < 7; ++k) {
            bus.addBoard(QLatin1String(kFleetBench[k].serial), QLatin1String(kFleetBench[k].port),
                         absent.contains(k) ? FakeUsbBus::State::Absent : FakeUsbBus::State::Adb);
        }
        script();
    }

    void script()
    {
        rig.runner.addHandler(exfPattern(), &nodeExtractor);
        bus.script(rig.runner);
    }

    CliResult run(const QStringList& arguments) { return rig.run(arguments); }

    CliRig rig;
    FakeUsbBus bus;
    std::function<void(FirmwareJob&)> extraSetup;
};

QString benchSerial(int k)
{
    return QLatin1String(kFleetBench[k].serial);
}

QString benchPort(int k)
{
    return QLatin1String(kFleetBench[k].port);
}

/// The commands that talk to a board (everything but the plan's EXF).
const QRegularExpression& boardTools()
{
    static const QRegularExpression pattern(QStringLiteral("^upgrade_tool (LD|WL|RD|UF|-s)|^adb"));
    return pattern;
}

QString readAll(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QString();
    }
    return QString::fromUtf8(file.readAll());
}

} // namespace

// =======================================================================================
// Test class
// =======================================================================================

class Tst_firmwarecli : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    // ---- The command line ---------------------------------------------------------------------
    void helpForAnAgent();
    void helpEveryOptionOfEveryCommand();
    void versionText();
    void usageErrorsGoToStderr();
    void usageErrorsNeverEchoASecret();
    void inputChecksBeforeAnythingRuns();
    void logFileCannotBeOpened();
    void logFileOnlyForAValidCommandLine();

    // ---- status ----------------------------------------------------------------------------------
    void statusAdbBoardReadsTheLayout();
    void statusLoaderBoard();
    void statusNoBoardExits3();
    void statusOtherSerialExits3();
    void statusBoardWithoutLayoutEndsAtOnce();
    void statusWithMcu();
    void statusWithMcuJson();
    void statusMcuNeedsARunningBoard();
    void toolsFolderOption();
    void quietPrintsOnlyTheResult();

    // ---- partitions --------------------------------------------------------------------------------
    void partitionsFromTheImageLayout();
    void partitionsJson();
    void partitionsLayoutUnknown();
    void partitionsLayoutOptionAndSerial();
    void partitionsFromTheCache();
    void partitionsUnpackFails();

    // ---- pull --------------------------------------------------------------------------------------
    void pullByProfileName();
    void pullJsonCarriesTheImage();
    void pullAdHocTargetWithIdentity();
    void pullHostKeyRefusedExits4();
    void pullAcceptHostKey();
    void pullPasswordFromEnvironment();
    void pullPasswordFromStdin();
    void pullWrongPasswordExits4();
    void pullPasswordNobodyAnswersExits4();
    void pullInteractiveQuestions();
    void pullYesAsksNothing();
    void pullUnreachableExits4();
    void pullWithTheSavedPassword();
    void interruptBeforeTheJobExists();
    void interruptDuringAQuestionWaitsForIt();

    // ---- flash-soc ---------------------------------------------------------------------------------
    void flashAllInLoaderMode();
    void flashPartitionsExactly();
    void flashUnknownPartitionIsUsage();
    void flashTooBigIsRefusedBeforeWriting();
    void flashMaskromIgnoresTheChoice();
    void flashNoBoardExits3();
    void flashFailingWlExits1WithTheTail();
    void flashTimeoutExits5();
    void flashInterruptExits5();
    void flashInterruptWaitsForTheWrite();
    void flashInterruptDuringAWriteStopsAfterIt();
    void flashBoardOnAdbGoesThroughLoader();
    void flashAdbNotBackIsStillDone();
    void flashNoWait();
    void flashJsonLines();
    void flashLogFileHoldsTheSameLines();
    void anotherOperationRunningExits1();

    // ---- flash-soc, a fleet (v0.7.0) ----------------------------------------------------------
    void fleetUsageErrors();
    void fleetDryRun();
    void fleetTwoBoards();
    void fleetAllBoardsAndMaxParallelOne();
    void fleetAllBoardsNextToAPhoneExits1();
    void fleetJsonLines();
    void fleetNotBackExits1();
    void fleetInterruptDrains();
    void fleetTimeoutDuringMapping();
    void fleetTimeoutDuringTheWrites();
    void fleetNoWait();
    void fleetLayoutOption();
    void fleetMappingStopResetsAndReports();
    void fleetForeignLoaderRefused();
    void fleetBootloaderPartitionWarns();
    void fleetLockHeldExits1();
    void fleetLayoutUnknownExits1();
    void fleetQuestionOnATerminal();
    void fleetLogAndQuiet();
    void singleBoardPathUnchanged();
    void fleetHelp();

    // ---- MCU -----------------------------------------------------------------------------------------
    void mcuInfo();
    void mcuInfoNoBoardExits3();
    void mcuInfoSilentMcuExits1();
    void mcuUpdateThroughTheBridge();
    void mcuSetRole();

    // ---- pull-flash ----------------------------------------------------------------------------------
    void pullFlashSkipsMcuWhenEqual();
    void pullFlashUpdatesMcuWhenDifferent();
    void pullFlashStopsAtTheFlash();
    void pullFlashStopsAtThePull();
    void pullFlashSilentMcuNeedsTheRole();
    void passwordVariableLeavesTheEnvironment();

    // ---- Board profiles (v0.8.0) -----------------------------------------------------------------------
    void profileOptionUsage();
    void statusProfileLines();
    void partitionsPrintsImageHeaderAndProfile();
    void flashSocRefusesWrongImage();
    void flashSocLayoutMismatchNeedsForce();
    void flashSocLoaderPartitionsNeedForce();
    void mcuUpdateNeedsConfirmOnStacked();
    void fleetMixedBenchStopsThroughTheCli();
    void helpMentionsProfiles();

    // ---- The executable ------------------------------------------------------------------------------
    void realExecutable();

private:
    QString tempPath(const QString& name) const { return m_tempDir.filePath(name); }
    QString caseDir(const QString& name) const { return tempPath(QStringLiteral("case%1-%2").arg(m_caseNumber).arg(name)); }
    /// A plain image (its env.img carries no layout) or one under "withenv..." (it does).
    QString makeImage(const QString& relative) const
    {
        const QString path = caseDir(relative);
        return writeFile(path, rkfwBytes()) ? path : QString();
    }
    /// An image under "withenv-<n>": its env.img carries the Node layout (the fleet's usual case).
    QString withEnvImage(const QString& suffix = QString()) const
    {
        const QString path = tempPath(QStringLiteral("withenv-%1%2/update.img").arg(m_caseNumber).arg(suffix));
        return writeFile(path, rkfwBytes()) ? path : QString();
    }

    QTemporaryDir m_tempDir;
    int m_caseNumber = 0;
};

// =======================================================================================
// Fixture
// =======================================================================================

void Tst_firmwarecli::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("BuildAI-Test"));
    QCoreApplication::setApplicationName(QStringLiteral("SerialUtilityTest-firmwarecli"));
    QVERIFY(m_tempDir.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_tempDir.filePath(QStringLiteral("settings")));
    QSettings().clear();
    QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
    qputenv("SU_SSH_IGNORE_CONFIG", "1");   // the SSH connects of this suite must not read ~/.ssh/config
    qunsetenv("SSH_AUTH_SOCK");             // nor offer the developer's agent keys to the test server
    QLoggingCategory::setFilterRules(QStringLiteral("buildai.*.info=false"));   // as su-fw does
}

void Tst_firmwarecli::init()
{
    QSettings().clear();
    qunsetenv(kSecretVariable);
    ++m_caseNumber;
}

void Tst_firmwarecli::cleanup()
{
    qunsetenv(kSecretVariable);
}

// =======================================================================================
// The command line
// =======================================================================================

void Tst_firmwarecli::helpForAnAgent()
{
    CliRig rig;
    const CliResult global = rig.run({QStringLiteral("--help")});
    QCOMPARE(global.code, 0);
    QVERIFY(global.err.isEmpty());
    QCOMPARE(global.out, FirmwareCli::helpText());
    for (const QString& command : FirmwareCli::commands()) {
        QVERIFY2(global.out.contains(QStringLiteral("  ") + command + QLatin1Char(' ')), qPrintable(command));
    }
    for (const char* code : {"  0  done", "  1  ", "  2  usage", "  3  no board", "  4  SSH", "  5  --timeout"}) {
        QVERIFY2(global.out.contains(QLatin1String(code)), code);
    }
    QVERIFY(global.out.contains(QStringLiteral("Examples:")));
    QVERIFY(global.out.contains(QStringLiteral("su-fw status --mcu")));
    QVERIFY(global.out.contains(QStringLiteral("--password-env SU_PASSWORD")));
    QVERIFY(global.out.contains(QStringLiteral("\"event\"")));   // the JSON format is explained

    QCOMPARE(rig.run({QStringLiteral("-h")}).out, FirmwareCli::helpText());
    const CliResult perCommand = rig.run({QStringLiteral("flash-soc"), QStringLiteral("--help")});
    QCOMPARE(perCommand.code, 0);
    QCOMPARE(perCommand.out, FirmwareCli::helpText(QStringLiteral("flash-soc")));
    QCOMPARE(rig.run({QStringLiteral("help"), QStringLiteral("pull")}).out, FirmwareCli::helpText(QStringLiteral("pull")));
    QCOMPARE(rig.run({QStringLiteral("help"), QStringLiteral("nope")}).code, 2);
    QVERIFY(rig.runner.invocations().isEmpty());
}

void Tst_firmwarecli::helpEveryOptionOfEveryCommand()
{
    QCOMPARE(FirmwareCli::commands(),
             (QStringList{QStringLiteral("status"), QStringLiteral("partitions"), QStringLiteral("pull"), QStringLiteral("flash-soc"),
                          QStringLiteral("mcu-info"), QStringLiteral("mcu-update"), QStringLiteral("mcu-set-role"),
                          QStringLiteral("pull-flash")}));
    QVERIFY(FirmwareCli::commandOptions(QStringLiteral("nope")).isEmpty());
    for (const QString& command : FirmwareCli::commands()) {
        const QString help = FirmwareCli::helpText(command);
        QVERIFY2(help.startsWith(QStringLiteral("su-fw ") + command + QStringLiteral(" - ")), qPrintable(command));
        const QStringList options = FirmwareCli::commandOptions(command);
        for (const char* common : {"tools", "log", "json", "quiet", "timeout", "yes"}) {
            QVERIFY2(options.contains(QLatin1String(common)), common);
        }
        for (const QString& option : options) {
            // Explained: at the start of an option line.
            const QRegularExpression line(QStringLiteral("\\n  --%1[ ,\\n]").arg(QRegularExpression::escape(option)));
            QVERIFY2(line.match(help).hasMatch(), qPrintable(command + QStringLiteral(": --") + option));
        }
        QVERIFY2(help.contains(QStringLiteral("Exit codes:")), qPrintable(command));
        const qsizetype examples = help.indexOf(QStringLiteral("Examples:"));
        QVERIFY2(examples > 0, qPrintable(command));
        QCOMPARE(help.mid(examples).count(QStringLiteral("\n  su-fw ") + command), qsizetype(2));
    }
}

void Tst_firmwarecli::versionText()
{
    CliRig rig;
    const CliResult version = rig.run({QStringLiteral("--version")});
    QCOMPARE(version.code, 0);
    QCOMPARE(version.out, FirmwareCli::versionText() + QLatin1Char('\n'));
    QVERIFY(version.out.startsWith(QStringLiteral("su-fw " APP_VERSION " (BuildAI Serial Utility, ")));
    QCOMPARE(QStringLiteral(APP_VERSION), QStringLiteral("0.8.0"));
}

void Tst_firmwarecli::usageErrorsGoToStderr()
{
    CliRig rig;
    const struct
    {
        QStringList arguments;
        const char* message;
    } cases[] = {
        {{}, "su-fw: a command is required"},
        {{QStringLiteral("frobnicate")}, "su-fw: unknown command \"frobnicate\""},
        {{QStringLiteral("--json")}, "su-fw: the command comes first"},
        {{QStringLiteral("status"), QStringLiteral("--bogus")}, "su-fw: Unknown option 'bogus'."},
        {{QStringLiteral("status"), QStringLiteral("--image"), QStringLiteral("x")}, "su-fw: Unknown option 'image'."},
        {{QStringLiteral("mcu-info"), QStringLiteral("--serial")}, "su-fw: Missing value after '--serial'."},
        {{QStringLiteral("status"), QStringLiteral("--timeout"), QStringLiteral("soon")}, "su-fw: --timeout needs a whole number"},
        {{QStringLiteral("status"), QStringLiteral("--timeout"), QStringLiteral("-1")}, "su-fw: --timeout needs a whole number"},
        {{QStringLiteral("mcu-update"), QStringLiteral("--role"), QStringLiteral("rx")}, "su-fw: --package FILE is required"},
        {{QStringLiteral("mcu-update"), QStringLiteral("--package"), dataFile("firmware-0.8.1.pkg")},
         "su-fw: --role rx|tx is required: the MCU role is never guessed"},
        {{QStringLiteral("mcu-set-role"), QStringLiteral("--role"), QStringLiteral("both")}, "su-fw: --role must be rx or tx"},
        {{QStringLiteral("mcu-set-role")}, "su-fw: --role rx|tx is required"},
        {{QStringLiteral("pull-flash"), QStringLiteral("--target"), QStringLiteral("a@b"), QStringLiteral("--partitions"), QStringLiteral("boot")},
         "su-fw: pull-flash writes every partition"},
        {{QStringLiteral("pull")}, "su-fw: --target is required"},
        {{QStringLiteral("pull"), QStringLiteral("--target"), QStringLiteral("not a target")},
         "su-fw: --target: \"not a target\" is neither one of the app's SSH profiles nor user@host[:port]"},
        {{QStringLiteral("pull"), QStringLiteral("--target"), QStringLiteral("ssh:profile:no-such-id")},
         "su-fw: --target: the app has no SSH profile with the id no-such-id"},
        {{QStringLiteral("pull"), QStringLiteral("--target"), QStringLiteral("a@b"), QStringLiteral("--identity"), QStringLiteral("no-such-key")},
         "su-fw: --identity: no such key file"},
        {{QStringLiteral("pull"), QStringLiteral("--target"), QStringLiteral("a@b"), QStringLiteral("--password-env"), QStringLiteral("X"),
          QStringLiteral("--password-stdin")},
         "su-fw: use either --password-env or --password-stdin"},
        {{QStringLiteral("pull"), QStringLiteral("--target"), QStringLiteral("a@b"), QStringLiteral("--password-stdin")},
         "su-fw: --password-stdin: no password could be read from stdin"},
        {{QStringLiteral("status"), QStringLiteral("--tools"), QStringLiteral("no-such-folder-here")}, "su-fw: --tools: no such folder"},
    };
    for (const auto& c : cases) {
        const CliResult result = rig.run(c.arguments);
        QVERIFY2(result.code == 2, qPrintable(result.dump()));
        QVERIFY2(result.out.isEmpty(), qPrintable(result.dump()));
        QVERIFY2(result.err.startsWith(QLatin1String(c.message)), qPrintable(result.dump()));
        QVERIFY2(result.err.contains(QStringLiteral("--help")), qPrintable(result.dump()));
    }
    QVERIFY(rig.runner.invocations().isEmpty());   // nothing reached a tool
}

void Tst_firmwarecli::usageErrorsNeverEchoASecret()
{
    CliRig rig;
    // A password typed where a word does not belong, or as the "name" of the variable: never repeated.
    CliResult result = rig.run({QStringLiteral("pull"), QStringLiteral("--target"), QStringLiteral("a@b"), kSecret});
    QCOMPARE(result.code, 2);
    QVERIFY2(!result.err.contains(kSecret) && !result.out.contains(kSecret), qPrintable(result.err));
    result = rig.run({QStringLiteral("pull"), QStringLiteral("--target"), QStringLiteral("a@b"), QStringLiteral("--password-env"), kSecret});
    QCOMPARE(result.code, 2);
    QVERIFY(result.err.startsWith(QStringLiteral("su-fw: --password-env: the environment variable it names is not set or empty")));
    QVERIFY2(!result.err.contains(kSecret), qPrintable(result.err));
}

void Tst_firmwarecli::inputChecksBeforeAnythingRuns()
{
    CliRig rig;
    const QString image = makeImage(QStringLiteral("check/update.img"));
    const QString wrong = caseDir(QStringLiteral("check/wrong.img"));
    QVERIFY(writeFile(wrong, QByteArray("ABCD") + QByteArray(1024, '\0')));
    const QString missing = caseDir(QStringLiteral("check/missing.img"));
    const QString emptyCache = caseDir(QStringLiteral("empty-cache"));
    QVERIFY(QDir().mkpath(emptyCache));
    const QString badPackage = caseDir(QStringLiteral("check/bad.pkg"));
    QVERIFY(writeFile(badPackage, QByteArray(64, '\x11')));

    const struct
    {
        QStringList arguments;
        QString message;
    } cases[] = {
        {{QStringLiteral("flash-soc"), QStringLiteral("--image"), image},
         QStringLiteral("su-fw: choose what to write: --all, or --partitions NAMES")},
        {{QStringLiteral("flash-soc"), QStringLiteral("--image"), image, QStringLiteral("--all"), QStringLiteral("--partitions"), QStringLiteral("boot")},
         QStringLiteral("su-fw: choose either --all or --partitions NAMES, not both")},
        {{QStringLiteral("flash-soc"), QStringLiteral("--image"), image, QStringLiteral("--partitions"), QStringLiteral(" , ")},
         QStringLiteral("su-fw: --partitions needs names")},
        {{QStringLiteral("flash-soc"), QStringLiteral("--image"), missing, QStringLiteral("--all")},
         QStringLiteral("su-fw: Image %1 refused: file not found").arg(QDir::toNativeSeparators(missing))},
        {{QStringLiteral("partitions"), QStringLiteral("--image"), wrong},
         QStringLiteral("su-fw: Image %1 refused: ").arg(QDir::toNativeSeparators(wrong))},
        {{QStringLiteral("partitions"), QStringLiteral("--cache"), emptyCache},
         QStringLiteral("su-fw: no image: pass --image FILE, or pull one first (there is no update.img in %1)")
             .arg(QDir::toNativeSeparators(emptyCache))},
        {{QStringLiteral("partitions"), QStringLiteral("--image"), image, QStringLiteral("--layout"), QStringLiteral("garbage")},
         QStringLiteral("su-fw: --layout: \"garbage\" is not a flash layout")},
        {{QStringLiteral("mcu-update"), QStringLiteral("--package"), badPackage, QStringLiteral("--role"), QStringLiteral("rx")},
         QStringLiteral("su-fw: Package bad.pkg refused: ")},
    };
    for (const auto& c : cases) {
        const CliResult result = rig.run(c.arguments);
        QVERIFY2(result.code == 2, qPrintable(result.dump()));
        QVERIFY2(result.out.isEmpty(), qPrintable(result.dump()));
        QVERIFY2(result.err.startsWith(c.message), qPrintable(result.dump()));
    }
    QVERIFY(rig.runner.invocations().isEmpty());
}

void Tst_firmwarecli::logFileCannotBeOpened()
{
    CliRig rig;
    const QString folder = caseDir(QStringLiteral("a-folder"));
    QVERIFY(QDir().mkpath(folder));
    const CliResult result = rig.run({QStringLiteral("status"), QStringLiteral("--log"), folder});
    QCOMPARE(result.code, 2);
    QVERIFY2(result.err.startsWith(QStringLiteral("su-fw: --log: cannot write %1").arg(QDir::toNativeSeparators(folder))),
             qPrintable(result.err));
    QVERIFY(rig.runner.invocations().isEmpty());
}

void Tst_firmwarecli::logFileOnlyForAValidCommandLine()
{
    // --log FILE is created once the whole command line is valid: a usage error leaves no file.
    CliRig rig;
    const QString image = makeImage(QStringLiteral("log/update.img"));
    const QString logPath = caseDir(QStringLiteral("usage.log"));
    CliResult result = rig.run({QStringLiteral("flash-soc"), QStringLiteral("--image"), image, QStringLiteral("--log"), logPath});
    QVERIFY2(result.code == 2, qPrintable(result.dump()));   // neither --all nor --partitions
    QVERIFY(!QFileInfo::exists(logPath));
    result = rig.run({QStringLiteral("pull"), QStringLiteral("--target"), QStringLiteral("not a target"), QStringLiteral("--log"), logPath});
    QVERIFY2(result.code == 2, qPrintable(result.dump()));
    QVERIFY(!QFileInfo::exists(logPath));
    QVERIFY(rig.runner.invocations().isEmpty());
}

// =======================================================================================
// status
// =======================================================================================

void Tst_firmwarecli::statusAdbBoardReadsTheLayout()
{
    CliRig rig;
    scriptAdbBoard(rig.runner);
    const CliResult result = rig.run({QStringLiteral("status")});
    QVERIFY2(result.code == 0, qPrintable(result.dump()));
    QVERIFY(result.err.isEmpty());
    QCOMPARE(result.texts().first(), QStringLiteral("su-fw " APP_VERSION ": status"));
    QVERIFY(result.hasText(QStringLiteral("upgrade_tool: upgrade_tool")));
    QVERIFY(result.hasText(QStringLiteral("adb: adb")));
    QVERIFY(result.hasText(QStringLiteral("Board: ADB device ") + kBoardSerial));
    // Read once from the running board and remembered in the app's settings, as the tab does.
    QVERIFY2(result.hasText(QStringLiteral("Flash layout of board %1: %2").arg(kBoardSerial, kNodeMtdParts)), qPrintable(result.dump()));
    QCOMPARE(AppSettings::instance().firmwareMtdParts(kBoardSerial), kNodeMtdParts);
    QCOMPARE(AppSettings::instance().firmwareMtdParts(QString()), kNodeMtdParts);
    QCOMPARE(result.lastText(), QStringLiteral("su-fw: exit 0 (done): ADB device ") + kBoardSerial);
    QCOMPARE(rig.runner.count(QRegularExpression(QStringLiteral("cat /proc/cmdline"))), 1);

    // Known now: not read again.
    const CliResult again = rig.run({QStringLiteral("status")});
    QCOMPARE(again.code, 0);
    QVERIFY(again.hasText(QStringLiteral("Flash layout of board %1 (remembered): %2").arg(kBoardSerial, kNodeMtdParts)));
    QCOMPARE(rig.runner.count(QRegularExpression(QStringLiteral("cat /proc/cmdline"))), 1);
}

void Tst_firmwarecli::statusLoaderBoard()
{
    CliRig rig;
    rig.runner.addScript(QRegularExpression(QStringLiteral("^upgrade_tool LD$")), kLdLoader);
    const CliResult result = rig.run({QStringLiteral("status")});
    QVERIFY2(result.code == 0, qPrintable(result.dump()));
    QVERIFY(result.hasTextStarting(QStringLiteral("Board: Loader mode")));
    QVERIFY(result.lastText().startsWith(QStringLiteral("su-fw: exit 0 (done): Loader mode")));
    QCOMPARE(rig.runner.count(QRegularExpression(QStringLiteral("devices"))), 0);   // Loader mode wins at once
}

void Tst_firmwarecli::statusNoBoardExits3()
{
    CliRig rig;
    scriptNoBoard(rig.runner);
    const CliResult result = rig.run({QStringLiteral("status")});
    QVERIFY2(result.code == 3, qPrintable(result.dump()));
    // "no board", or - when this PC's bus shows a Rockchip device none of the (fake) tools sees -
    // the driver hint; either way the status line is the result.
    const QStringList texts = result.texts();
    QVERIFY(texts.size() >= 2);
    const QString board = texts.at(texts.size() - 2);
    QVERIFY2(board.startsWith(QStringLiteral("Board: ")), qPrintable(result.dump()));
    QCOMPARE(result.lastText(), QStringLiteral("su-fw: exit 3 (no board): ") + board.mid(7));
}

void Tst_firmwarecli::statusOtherSerialExits3()
{
    CliRig rig;
    scriptAdbBoard(rig.runner);
    const CliResult result = rig.run({QStringLiteral("status"), QStringLiteral("--serial"), QStringLiteral("c3d9")});
    QVERIFY2(result.code == 3, qPrintable(result.dump()));
    QVERIFY2(result.hasText(QStringLiteral("Board: no board (board c3d9 is not attached; attached: %1)").arg(kBoardSerial)),
             qPrintable(result.dump()));
}

void Tst_firmwarecli::statusBoardWithoutLayoutEndsAtOnce()
{
    // A board whose /proc/cmdline has no mtdparts: the end of the read comes as the job's
    // layoutReadFinished() - no waiting for the 20 s cap, whatever the log says.
    CliRig rig;
    rig.runner.addScript(QRegularExpression(QStringLiteral("^upgrade_tool LD$")), "List of rockusb connected(0)\r\n");
    rig.runner.addScript(QRegularExpression(QStringLiteral("^adb( -s \\S+)? devices$")), kAdbBoard);
    rig.runner.addScript(QRegularExpression(QStringLiteral("cat /proc/cmdline")), "console=ttyFIQ0 root=/dev/mtdblock5 rootwait\n");
    QElapsedTimer clock;
    clock.start();
    const CliResult result = rig.run({QStringLiteral("status")});
    QVERIFY2(result.code == 0, qPrintable(result.dump()));
    QVERIFY2(clock.elapsed() < 10000, qPrintable(QString::number(clock.elapsed())));
    QVERIFY(result.hasText(QStringLiteral("Board %1 reports no flash layout (no mtdparts in /proc/cmdline)").arg(kBoardSerial)));
    QVERIFY(AppSettings::instance().firmwareMtdParts(kBoardSerial).isEmpty());
}

void Tst_firmwarecli::statusWithMcu()
{
    CliRig rig;
    scriptAdbBoard(rig.runner);
    rig.runner.addScript(QRegularExpression(QStringLiteral("killall rfmcu")), QByteArray());
    rig.runner.addScript(QRegularExpression(QStringLiteral("rfmcu cmd version")), "OK version=0.8.1 role=tx\r\n");
    const CliResult result = rig.run({QStringLiteral("status"), QStringLiteral("--mcu")});
    QVERIFY2(result.code == 0, qPrintable(result.dump()));
    QVERIFY(result.hasText(QStringLiteral("--- Read MCU ---")));
    QVERIFY(result.hasText(QStringLiteral("OK version=0.8.1 role=tx")));
    QVERIFY(result.hasText(QStringLiteral("Read MCU done: MCU application version=0.8.1 role=tx")));
    QCOMPARE(result.lastText(), QStringLiteral("su-fw: exit 0 (done): ADB device %1; MCU version=0.8.1 role=tx").arg(kBoardSerial));
    // The layout read ended before the MCU commands started: they never overlap.
    const QStringList commands = rig.runner.invocations();
    const qsizetype cmdline = commands.indexOf(QRegularExpression(QStringLiteral(".*cat /proc/cmdline.*")));
    const qsizetype killall = commands.indexOf(QRegularExpression(QStringLiteral(".*killall rfmcu.*")));
    QVERIFY(cmdline >= 0 && killall > cmdline);
}

void Tst_firmwarecli::statusWithMcuJson()
{
    CliRig rig;
    scriptAdbBoard(rig.runner);
    rig.runner.addScript(QRegularExpression(QStringLiteral("killall rfmcu")), QByteArray());
    rig.runner.addScript(QRegularExpression(QStringLiteral("rfmcu cmd version")), "OK version=0.8.1 role=tx\r\n");
    const CliResult result = rig.run({QStringLiteral("status"), QStringLiteral("--mcu"), QStringLiteral("--json")});
    QVERIFY2(result.code == 0, qPrintable(result.dump()));
    bool allParse = false;
    const QList<QJsonObject> objects = jsonLines(result, &allParse);
    QVERIFY2(allParse, qPrintable(result.out));
    QCOMPARE(objects.size(), result.lines().size());
    static const QRegularExpression iso(QStringLiteral("^\\d{4}-\\d\\d-\\d\\dT\\d\\d:\\d\\d:\\d\\d\\.\\d{3}([+-]\\d\\d:\\d\\d|Z)$"));
    for (const QJsonObject& object : objects) {
        QVERIFY2(iso.match(object.value(QStringLiteral("time")).toString()).hasMatch(), qPrintable(object.value(QStringLiteral("time")).toString()));
        QVERIFY(!object.value(QStringLiteral("step")).toString().isEmpty());
        QVERIFY(QStringList({QStringLiteral("log"), QStringLiteral("progress"), QStringLiteral("step"), QStringLiteral("done")})
                    .contains(object.value(QStringLiteral("event")).toString()));
        QVERIFY(object.value(QStringLiteral("text")).isString());
    }
    // The lines of the MCU read belong to its step; its end is a "step" event.
    bool sawInfoLog = false;
    bool sawInfoStep = false;
    for (const QJsonObject& object : objects) {
        const QString step = object.value(QStringLiteral("step")).toString();
        const QString event = object.value(QStringLiteral("event")).toString();
        sawInfoLog = sawInfoLog || (step == QLatin1String("info") && event == QLatin1String("log"));
        if (event == QLatin1String("step")) {
            QCOMPARE(step, QStringLiteral("info"));
            QCOMPARE(object.value(QStringLiteral("ok")).toBool(), true);
            QCOMPARE(object.value(QStringLiteral("text")).toString(), QStringLiteral("MCU application version=0.8.1 role=tx"));
            sawInfoStep = true;
        }
    }
    QVERIFY(sawInfoLog && sawInfoStep);
    const QJsonObject done = objects.last();
    QCOMPARE(done.value(QStringLiteral("event")).toString(), QStringLiteral("done"));
    QCOMPARE(done.value(QStringLiteral("step")).toString(), QStringLiteral("status"));
    QCOMPARE(done.value(QStringLiteral("ok")).toBool(), true);
    QCOMPARE(done.value(QStringLiteral("exit")).toInt(), 0);
    QCOMPARE(done.value(QStringLiteral("board")).toObject().value(QStringLiteral("mode")).toString(), QStringLiteral("adb"));
    QCOMPARE(done.value(QStringLiteral("board")).toObject().value(QStringLiteral("serial")).toString(), kBoardSerial);
    QCOMPARE(done.value(QStringLiteral("layout")).toString(), kNodeMtdParts);
    QCOMPARE(done.value(QStringLiteral("mcu")).toObject().value(QStringLiteral("version")).toString(), QStringLiteral("0.8.1"));
    QCOMPARE(done.value(QStringLiteral("mcu")).toObject().value(QStringLiteral("role")).toString(), QStringLiteral("tx"));
    // The object starts with time, step, event (readable for people too).
    QVERIFY(result.lines().last().startsWith(QStringLiteral("{\"time\":")));
}

void Tst_firmwarecli::statusMcuNeedsARunningBoard()
{
    CliRig rig;
    rig.runner.addScript(QRegularExpression(QStringLiteral("^upgrade_tool LD$")), kLdLoader);
    const CliResult result = rig.run({QStringLiteral("status"), QStringLiteral("--mcu")});
    QVERIFY2(result.code == 3, qPrintable(result.dump()));
    QVERIFY(result.hasText(QStringLiteral("MCU: not read: the board is not running (ADB)")));
    QCOMPARE(rig.runner.count(QRegularExpression(QStringLiteral("rfmcu"))), 0);
}

void Tst_firmwarecli::toolsFolderOption()
{
    // --tools DIR: the tools found there, by their platform names, with the full path.
    CliRig rig(/*fixedTools=*/false);
    const QString dir = caseDir(QStringLiteral("tools"));
    const QString upgradeTool = dir + QLatin1Char('/') + ToolLocator::fileName(ToolLocator::Tool::UpgradeTool);
    const QString adb = dir + QLatin1Char('/') + ToolLocator::fileName(ToolLocator::Tool::Adb);
    QVERIFY(writeFile(upgradeTool, "not a real tool"));
    QVERIFY(writeFile(adb, "not a real tool"));
    rig.runner.addScript(QRegularExpression(QStringLiteral("upgrade_tool(\\.exe)?\"? LD$")), "List of rockusb connected(0)\r\n");
    rig.runner.addScript(QRegularExpression(QStringLiteral("adb(\\.exe)?\"? devices$")), kAdbBoard);
    rig.runner.addScript(QRegularExpression(QStringLiteral("cat /proc/cmdline")), kNodeCmdline);
    const CliResult result = rig.run({QStringLiteral("status"), QStringLiteral("--tools"), dir});
    QVERIFY2(result.code == 0, qPrintable(result.dump()));
    QVERIFY(result.hasText(QStringLiteral("upgrade_tool: --tools ") + QDir::toNativeSeparators(QFileInfo(upgradeTool).absoluteFilePath())));
    QVERIFY(result.hasText(QStringLiteral("adb: --tools ") + QDir::toNativeSeparators(QFileInfo(adb).absoluteFilePath())));
    QVERIFY(!rig.runner.invocations().isEmpty());
    for (const QString& command : rig.runner.invocations()) {
        QVERIFY2(command.contains(QFileInfo(dir).absoluteFilePath()), qPrintable(command));
    }
}

void Tst_firmwarecli::quietPrintsOnlyTheResult()
{
    CliRig rig;
    scriptAdbBoard(rig.runner);
    const CliResult plain = rig.run({QStringLiteral("status"), QStringLiteral("--quiet")});
    QCOMPARE(plain.code, 0);
    QCOMPARE(plain.lines().size(), qsizetype(1));
    QCOMPARE(plain.lastText(), QStringLiteral("su-fw: exit 0 (done): ADB device ") + kBoardSerial);
    const CliResult json = rig.run({QStringLiteral("status"), QStringLiteral("--quiet"), QStringLiteral("--json")});
    QCOMPARE(json.lines().size(), qsizetype(1));
    QCOMPARE(QJsonDocument::fromJson(json.lines().first().toUtf8()).object().value(QStringLiteral("event")).toString(),
             QStringLiteral("done"));
}

// =======================================================================================
// partitions
// =======================================================================================

void Tst_firmwarecli::partitionsFromTheImageLayout()
{
    CliRig rig;
    rig.runner.addHandler(exfPattern(), &nodeExtractor);
    const QString image = tempPath(QStringLiteral("withenv-%1/update.img").arg(m_caseNumber));   // its env.img carries the layout
    QVERIFY(writeFile(image, rkfwBytes()));
    const CliResult result = rig.run({QStringLiteral("partitions"), QStringLiteral("--image"), image});
    QVERIFY2(result.code == 0, qPrintable(result.dump()));
    QVERIFY(result.hasText(QStringLiteral("Using the local image ") + QDir::toNativeSeparators(image)));
    QVERIFY(result.hasText(QStringLiteral("Flash layout (from the image's env.img): ") + kNodeMtdParts));
    QVERIFY(result.hasText(QStringLiteral("Partitions of update.img: 7, 7 can be written")));
    // The table: a header, then one row per partition with offset, partition size, image size, fits.
    const QStringList texts = result.texts();
    const qsizetype header = texts.indexOf(QRegularExpression(QStringLiteral("^name\\s+file\\s+offset\\s+partition\\s+image\\s+fits$")));
    QVERIFY2(header >= 0, qPrintable(result.dump()));
    for (int k = 0; k < kNodePartitionCount; ++k) {
        const QStringList cells = texts.at(header + 1 + k).split(QLatin1Char(' '), Qt::SkipEmptyParts);
        QCOMPARE(cells.size(), qsizetype(6));
        QCOMPARE(cells.at(0), QLatin1String(kNodeNames[k]));
        QCOMPARE(cells.at(1), QLatin1String(kNodeNames[k]) + QStringLiteral(".img"));
        QCOMPARE(cells.at(2), QStringLiteral("0x%1").arg(kNodeSectors[k] * 512, 0, 16));
        QCOMPARE(cells.at(3), QString::number(kNodeSizeSectors[k] * 512));
        QCOMPARE(cells.at(4), QString::number(kNodeImageBytes[k]));
        QCOMPARE(cells.at(5), QStringLiteral("yes"));
    }
    QCOMPARE(result.lastText(), QStringLiteral("su-fw: exit 0 (done): 7 partitions, 7 can be written"));
    QCOMPARE(rig.runner.count(exfPattern()), 1);
    QCOMPARE(rig.runner.count(QRegularExpression(QStringLiteral("LD|devices|WL"))), 0);   // the board is not touched
}

void Tst_firmwarecli::partitionsJson()
{
    CliRig rig;
    rig.runner.addHandler(exfPattern(), &nodeExtractor);
    const QString image = makeImage(QStringLiteral("big/big.img"));   // boot.img one byte too large
    const CliResult result = rig.run({QStringLiteral("partitions"), QStringLiteral("--image"), image, QStringLiteral("--layout"),
                                      kNodeMtdParts, QStringLiteral("--json")});
    QVERIFY2(result.code == 0, qPrintable(result.dump()));
    bool allParse = false;
    const QList<QJsonObject> objects = jsonLines(result, &allParse);
    QVERIFY(allParse);
    const QJsonObject done = objects.last();
    QCOMPARE(done.value(QStringLiteral("text")).toString(), QStringLiteral("7 partitions, 6 can be written"));
    QCOMPARE(done.value(QStringLiteral("layout")).toString(), kNodeMtdParts);
    QCOMPARE(done.value(QStringLiteral("layoutSource")).toString(), QStringLiteral("board"));
    QCOMPARE(done.value(QStringLiteral("image")).toString(), QDir::toNativeSeparators(image));
    const QJsonArray rows = done.value(QStringLiteral("partitions")).toArray();
    QCOMPARE(rows.size(), qsizetype(kNodePartitionCount));
    const QJsonObject boot = rows.at(4).toObject();
    QCOMPARE(boot.value(QStringLiteral("name")).toString(), QStringLiteral("boot"));
    QCOMPARE(boot.value(QStringLiteral("offset")).toInteger(), qint64(0xf0000));
    QCOMPARE(boot.value(QStringLiteral("size")).toInteger(), qint64(3 * 1024 * 1024));
    QCOMPARE(boot.value(QStringLiteral("imageSize")).toInteger(), qint64(3 * 1024 * 1024 + 1));
    QCOMPARE(boot.value(QStringLiteral("fits")).toBool(), false);
    QCOMPARE(boot.value(QStringLiteral("writable")).toBool(), false);
    QCOMPARE(boot.value(QStringLiteral("problem")).toString(), QStringLiteral("boot.img (3145729 bytes) does not fit the partition (3145728 bytes)"));
    QCOMPARE(rows.at(5).toObject().value(QStringLiteral("writable")).toBool(), true);
}

void Tst_firmwarecli::partitionsLayoutUnknown()
{
    CliRig rig;
    rig.runner.addHandler(exfPattern(), &nodeExtractor);
    const QString image = makeImage(QStringLiteral("plain/update.img"));
    const CliResult result = rig.run({QStringLiteral("partitions"), QStringLiteral("--image"), image});
    QVERIFY2(result.code == 0, qPrintable(result.dump()));
    QVERIFY(result.hasTextStarting(QStringLiteral("Flash layout: unknown - the image carries none")));
    QVERIFY(result.hasText(QStringLiteral("Partitions of update.img: 7, 0 can be written")));
    QVERIFY(result.hasTextStarting(QStringLiteral("boot ")));
    for (const QString& text : result.texts()) {
        if (text.startsWith(QStringLiteral("rootfs "))) {
            QVERIFY2(text.endsWith(QStringLiteral("- partition layout unknown: connect the board running once")), qPrintable(text));
        }
    }
}

void Tst_firmwarecli::partitionsLayoutOptionAndSerial()
{
    CliRig rig;
    rig.runner.addHandler(exfPattern(), &nodeExtractor);
    const QString image = makeImage(QStringLiteral("plain/update.img"));
    CliResult result = rig.run({QStringLiteral("partitions"), QStringLiteral("--image"), image, QStringLiteral("--layout"), kNodeMtdParts});
    QVERIFY2(result.code == 0, qPrintable(result.dump()));
    QVERIFY(result.hasText(QStringLiteral("Flash layout (--layout): ") + kNodeMtdParts));
    QVERIFY(result.hasText(QStringLiteral("Partitions of update.img: 7, 7 can be written")));

    AppSettings::instance().setFirmwareMtdParts(QStringLiteral("c3d9"), kNodeMtdParts);
    result = rig.run({QStringLiteral("partitions"), QStringLiteral("--image"), image, QStringLiteral("--serial"), QStringLiteral("c3d9")});
    QVERIFY2(result.code == 0, qPrintable(result.dump()));
    QVERIFY(result.hasText(QStringLiteral("Flash layout (remembered for board c3d9): ") + kNodeMtdParts));
    QVERIFY(result.hasText(QStringLiteral("Partitions of update.img: 7, 7 can be written")));

    // The layout read last from any board (what su-fw status / the tab stored) is the default.
    AppSettings::instance().setFirmwareMtdParts(QString(), kNodeMtdParts);
    result = rig.run({QStringLiteral("partitions"), QStringLiteral("--image"), image});
    QVERIFY(result.hasText(QStringLiteral("Flash layout (remembered from the board read last): ") + kNodeMtdParts));
}

void Tst_firmwarecli::partitionsFromTheCache()
{
    CliRig rig;
    rig.runner.addHandler(exfPattern(), &nodeExtractor);
    const QString cache = caseDir(QStringLiteral("cache"));
    QVERIFY(writeFile(cache + QStringLiteral("/update.img"), rkfwBytes()));
    CliResult result = rig.run({QStringLiteral("partitions"), QStringLiteral("--cache"), cache, QStringLiteral("--layout"), kNodeMtdParts});
    QVERIFY2(result.code == 0, qPrintable(result.dump()));
    QVERIFY(result.hasText(QStringLiteral("Image: %1 (the cache's)").arg(QDir::toNativeSeparators(cache + QStringLiteral("/update.img")))));
    QVERIFY(result.hasText(QStringLiteral("Partitions of update.img: 7, 7 can be written")));

    // The app's cache folder is the default.
    AppSettings::instance().setFirmwareCacheDirectory(cache);
    result = rig.run({QStringLiteral("partitions"), QStringLiteral("--layout"), kNodeMtdParts});
    QVERIFY2(result.code == 0, qPrintable(result.dump()));
}

void Tst_firmwarecli::partitionsUnpackFails()
{
    CliRig rig;
    rig.runner.addScript(exfPattern(), "Extract failed\r\n", 1);
    const QString image = makeImage(QStringLiteral("plain/update.img"));
    CliResult result = rig.run({QStringLiteral("partitions"), QStringLiteral("--image"), image});
    QVERIFY2(result.code == 1, qPrintable(result.dump()));
    QVERIFY2(result.lastText().startsWith(QStringLiteral("su-fw: exit 1 (failed): cannot list the partitions of %1: ")
                                              .arg(QDir::toNativeSeparators(image))),
             qPrintable(result.dump()));

    // No upgrade_tool at all: the job's own reason.
    CliRig without;
    without.cli.setToolPaths(QString(), QStringLiteral("adb"));
    result = without.run({QStringLiteral("partitions"), QStringLiteral("--image"), image});
    QCOMPARE(result.code, 1);
    QVERIFY(result.hasText(QStringLiteral("upgrade_tool: not found")));
    QCOMPARE(result.lastText(), QStringLiteral("su-fw: exit 1 (failed): cannot list the partitions of %1: upgrade_tool not found")
                                    .arg(QDir::toNativeSeparators(image)));
}

// =======================================================================================
// pull
// =======================================================================================

void Tst_firmwarecli::pullByProfileName()
{
    CliRig rig;
    BuildMachine machine;
    QVERIFY(machine.start(&rig.store, caseDir(QStringLiteral("work"))));
    const QString cache = caseDir(QStringLiteral("cache"));
    const QStringList arguments{QStringLiteral("pull"), QStringLiteral("--target"), machine.profile.name, QStringLiteral("--remote-folder"),
                                machine.imageFolder, QStringLiteral("--cache"), cache};
    CliResult result = rig.run(arguments);
    QVERIFY2(result.code == 0, qPrintable(result.dump()));
    QVERIFY(result.hasText(QStringLiteral("--- Pull ---")));
    QVERIFY(result.hasText(QStringLiteral("Connecting to ") + machine.target()));
    QVERIFY(result.hasTextStarting(QStringLiteral("Pull done: ")));
    QVERIFY(result.hasTextStarting(QStringLiteral("Image: update.img ")));
    QVERIFY(result.hasText(QStringLiteral("Image file: ") + QDir::toNativeSeparators(cache + QStringLiteral("/update.img"))));
    QVERIFY(!result.hasTextStarting(QStringLiteral("upgrade_tool:")));   // a pull needs no tool
    QCOMPARE(QFileInfo(cache + QStringLiteral("/update.img")).size(), qint64(machine.image.size()));
    QVERIFY(result.lastText().startsWith(QStringLiteral("su-fw: exit 0 (done): ")));
    QVERIFY(rig.runner.invocations().isEmpty());

    // Unchanged on the build machine: skipped, still a success. The restore key form works too.
    result = rig.run({QStringLiteral("pull"), QStringLiteral("--target"), QStringLiteral("ssh:profile:") + machine.profile.id,
                      QStringLiteral("--remote-folder"), machine.imageFolder, QStringLiteral("--cache"), cache});
    QVERIFY2(result.code == 0, qPrintable(result.dump()));
    QVERIFY(result.out.contains(QStringLiteral("unchanged")));
}

void Tst_firmwarecli::pullJsonCarriesTheImage()
{
    CliRig rig;
    BuildMachine machine;
    QVERIFY(machine.start(&rig.store, caseDir(QStringLiteral("work"))));
    const QString cache = caseDir(QStringLiteral("cache"));
    // The tab's last build machine and folder are the defaults.
    AppSettings::instance().setFirmwareSourceTarget(QStringLiteral("ssh:profile:") + machine.profile.id);
    AppSettings::instance().setFirmwareRemoteFolder(QStringLiteral("ssh:profile:") + machine.profile.id, machine.imageFolder);
    AppSettings::instance().setFirmwareCacheDirectory(cache);
    const CliResult result = rig.run({QStringLiteral("pull"), QStringLiteral("--json")});
    QVERIFY2(result.code == 0, qPrintable(result.dump()));
    bool allParse = false;
    const QList<QJsonObject> objects = jsonLines(result, &allParse);
    QVERIFY(allParse);
    bool sawPercent = false;
    for (const QJsonObject& object : objects) {
        if (object.value(QStringLiteral("event")).toString() == QLatin1String("progress")) {
            QCOMPARE(object.value(QStringLiteral("step")).toString(), QStringLiteral("pull"));
            QVERIFY(object.value(QStringLiteral("text")).toString().startsWith(QStringLiteral("Pulling update.img")));
            const int percent = object.value(QStringLiteral("percent")).toInt(-1);
            QVERIFY(percent >= 0 && percent <= 100);
            sawPercent = true;
        }
    }
    QVERIFY(sawPercent);
    const QJsonObject image = objects.last().value(QStringLiteral("image")).toObject();
    QCOMPARE(image.value(QStringLiteral("path")).toString(), QDir::toNativeSeparators(cache + QStringLiteral("/update.img")));
    QCOMPARE(image.value(QStringLiteral("size")).toInteger(), qint64(machine.image.size()));
    QCOMPARE(image.value(QStringLiteral("target")).toString(), machine.target());
    QVERIFY(!image.value(QStringLiteral("summary")).toString().isEmpty());
}

void Tst_firmwarecli::pullAdHocTargetWithIdentity()
{
    CliRig rig;
    SshProfileStore scratch;
    BuildMachine machine;
    QVERIFY(machine.start(&scratch, caseDir(QStringLiteral("work"))));   // not in the cli's store: an ad-hoc target
    AppSettings::instance().setSshKnownHostsFile(machine.knownHostsFile);
    const CliResult result = rig.run({QStringLiteral("pull"), QStringLiteral("--target"), machine.target(), QStringLiteral("--identity"),
                                      machine.keyFile, QStringLiteral("--remote-folder"), machine.imageFolder, QStringLiteral("--cache"),
                                      caseDir(QStringLiteral("cache"))});
    QVERIFY2(result.code == 0, qPrintable(result.dump()));
    QCOMPARE(machine.server->lastAuthMethod(), QStringLiteral("publickey"));
}

void Tst_firmwarecli::pullHostKeyRefusedExits4()
{
    CliRig rig;
    BuildMachine machine;
    machine.trustServer = false;
    QVERIFY(machine.start(&rig.store, caseDir(QStringLiteral("work"))));
    const CliResult result = rig.run({QStringLiteral("pull"), QStringLiteral("--target"), machine.profile.name, QStringLiteral("--remote-folder"),
                                      machine.imageFolder, QStringLiteral("--cache"), caseDir(QStringLiteral("cache"))});
    QVERIFY2(result.code == 4, qPrintable(result.dump()));
    QVERIFY(result.hasTextStarting(QStringLiteral("The host key of 127.0.0.1:%1 is not known yet (%2 %3)")
                                       .arg(machine.server->port())
                                       .arg(machine.server->hostKeyType(), machine.server->hostKeyFingerprintSha256())));
    QVERIFY(result.hasTextStarting(QStringLiteral("Refused: pass --accept-host-key to trust it")));
    QCOMPARE(result.lastText(), QStringLiteral("su-fw: exit 4 (SSH): Host key rejected for ") + machine.target());
    QCOMPARE(QFile(machine.knownHostsFile).size(), qint64(0));
}

void Tst_firmwarecli::pullAcceptHostKey()
{
    CliRig rig;
    BuildMachine machine;
    machine.trustServer = false;
    QVERIFY(machine.start(&rig.store, caseDir(QStringLiteral("work"))));
    const QStringList arguments{QStringLiteral("pull"), QStringLiteral("--target"), machine.profile.name, QStringLiteral("--remote-folder"),
                                machine.imageFolder, QStringLiteral("--cache"), caseDir(QStringLiteral("cache")), QStringLiteral("--accept-host-key")};
    CliResult result = rig.run(arguments);
    QVERIFY2(result.code == 0, qPrintable(result.dump()));
    QVERIFY2(result.hasTextStarting(QStringLiteral("Host key accepted (--accept-host-key) and remembered in ")), qPrintable(result.dump()));
    QVERIFY(result.out.contains(QFileInfo(machine.knownHostsFile).fileName()));
    QVERIFY(readAll(machine.knownHostsFile).contains(machine.server->hostKeyPublicLine().section(QLatin1Char(' '), 1, 1)));
    // Remembered: the next pull is not asked (and needs no flag).
    result = rig.run(arguments.mid(0, arguments.size() - 1));
    QVERIFY2(result.code == 0, qPrintable(result.dump()));
    QVERIFY(!result.hasTextStarting(QStringLiteral("The host key of")));
}

void Tst_firmwarecli::pullPasswordFromEnvironment()
{
    CliRig rig;
    BuildMachine machine;
    machine.passwordAuth = true;
    machine.password = kSecret;
    QVERIFY(machine.start(&rig.store, caseDir(QStringLiteral("work"))));
    qputenv(kSecretVariable, kSecret.toUtf8());
    const QString logPath = caseDir(QStringLiteral("pull.log"));
    const CliResult result = rig.run({QStringLiteral("pull"), QStringLiteral("--target"), machine.profile.name, QStringLiteral("--remote-folder"),
                                      machine.imageFolder, QStringLiteral("--cache"), caseDir(QStringLiteral("cache")),
                                      QStringLiteral("--password-env"), QString::fromLatin1(kSecretVariable), QStringLiteral("--log"), logPath});
    QVERIFY2(result.code == 0, qPrintable(result.dump()));
    QCOMPARE(machine.server->lastAuthMethod(), QStringLiteral("password"));
    QVERIFY(result.hasText(QStringLiteral("Answering \"Password for %1:\" with the secret from --password-env").arg(machine.target())));
    QVERIFY(!result.out.contains(kSecret));
    QVERIFY(!result.err.contains(kSecret));
    QVERIFY(!readAll(logPath).contains(kSecret));
    QVERIFY(!result.out.contains(QLatin1String(kSecretVariable)));   // not even the variable's name
}

void Tst_firmwarecli::pullPasswordFromStdin()
{
    CliRig rig;
    BuildMachine machine;
    machine.passwordAuth = true;
    machine.password = kSecret;
    QVERIFY(machine.start(&rig.store, caseDir(QStringLiteral("work"))));
    rig.setInput({kSecret}, /*interactive=*/false);
    const CliResult result = rig.run({QStringLiteral("pull"), QStringLiteral("--target"), machine.profile.name, QStringLiteral("--remote-folder"),
                                      machine.imageFolder, QStringLiteral("--cache"), caseDir(QStringLiteral("cache")),
                                      QStringLiteral("--password-stdin"), QStringLiteral("--json")});
    QVERIFY2(result.code == 0, qPrintable(result.dump()));
    QCOMPARE(*rig.secretReads, 1);   // read as a secret (no echo on a terminal)
    QVERIFY(result.out.contains(QStringLiteral("with the secret from --password-stdin")));
    QVERIFY(!result.out.contains(kSecret));
    QVERIFY(!result.err.contains(kSecret));
}

void Tst_firmwarecli::pullWrongPasswordExits4()
{
    CliRig rig;
    BuildMachine machine;
    machine.passwordAuth = true;
    machine.password = QStringLiteral("the-right-one");
    QVERIFY(machine.start(&rig.store, caseDir(QStringLiteral("work"))));
    qputenv(kSecretVariable, kSecret.toUtf8());
    const CliResult result = rig.run({QStringLiteral("pull"), QStringLiteral("--target"), machine.profile.name, QStringLiteral("--remote-folder"),
                                      machine.imageFolder, QStringLiteral("--cache"), caseDir(QStringLiteral("cache")),
                                      QStringLiteral("--password-env"), QString::fromLatin1(kSecretVariable)});
    QVERIFY2(result.code == 4, qPrintable(result.dump()));
    QVERIFY(result.hasText(QStringLiteral("\"Password for %1:\" was asked again: the secret from --password-env was not accepted").arg(machine.target())));
    QVERIFY(result.lastText().startsWith(QStringLiteral("su-fw: exit 4 (SSH): ")));
    QVERIFY(!result.out.contains(kSecret));
    QVERIFY(!result.err.contains(kSecret));
}

void Tst_firmwarecli::pullPasswordNobodyAnswersExits4()
{
    CliRig rig;
    BuildMachine machine;
    machine.passwordAuth = true;
    QVERIFY(machine.start(&rig.store, caseDir(QStringLiteral("work"))));
    rig.setInput({QStringLiteral("never read")}, /*interactive=*/false);   // a pipe: no questions
    const CliResult result = rig.run({QStringLiteral("pull"), QStringLiteral("--target"), machine.profile.name, QStringLiteral("--remote-folder"),
                                      machine.imageFolder, QStringLiteral("--cache"), caseDir(QStringLiteral("cache"))});
    QVERIFY2(result.code == 4, qPrintable(result.dump()));
    QCOMPARE(result.lastText(),
             QStringLiteral("su-fw: exit 4 (SSH): authentication failed: nobody answered \"Password for %1:\"").arg(machine.target()));
    QVERIFY(result.hasTextStarting(QStringLiteral("The build machine asks for a password: pass --password-env VAR or --password-stdin")));
    QVERIFY(result.err.isEmpty());
    QCOMPARE(*rig.secretReads, 0);
}

void Tst_firmwarecli::pullInteractiveQuestions()
{
    // On a terminal without --yes su-fw asks: the host key (on stderr), then the password (no echo).
    CliRig rig;
    BuildMachine machine;
    machine.trustServer = false;
    machine.passwordAuth = true;
    machine.password = kSecret;
    QVERIFY(machine.start(&rig.store, caseDir(QStringLiteral("work"))));
    rig.setInput({QStringLiteral("a"), kSecret}, /*interactive=*/true);
    const CliResult result = rig.run({QStringLiteral("pull"), QStringLiteral("--target"), machine.profile.name, QStringLiteral("--remote-folder"),
                                      machine.imageFolder, QStringLiteral("--cache"), caseDir(QStringLiteral("cache"))});
    QVERIFY2(result.code == 0, qPrintable(result.dump()));
    QVERIFY2(result.err.contains(QStringLiteral("Trust this host key? [y] yes, once / [a] yes, and remember it in ")), qPrintable(result.err));
    QVERIFY(result.err.contains(QStringLiteral("Password for %1:").arg(machine.target())));
    QCOMPARE(*rig.secretReads, 1);
    QVERIFY2(result.hasTextStarting(QStringLiteral("Host key accepted and remembered in ")), qPrintable(result.dump()));
    QVERIFY(readAll(machine.knownHostsFile).contains(machine.server->hostKeyPublicLine().section(QLatin1Char(' '), 1, 1)));
    QVERIFY(!result.out.contains(kSecret));
    QVERIFY(!result.err.contains(kSecret));
}

void Tst_firmwarecli::pullYesAsksNothing()
{
    CliRig rig;
    BuildMachine machine;
    machine.trustServer = false;
    QVERIFY(machine.start(&rig.store, caseDir(QStringLiteral("work"))));
    rig.setInput({QStringLiteral("a")}, /*interactive=*/true);
    const CliResult result = rig.run({QStringLiteral("pull"), QStringLiteral("--target"), machine.profile.name, QStringLiteral("--remote-folder"),
                                      machine.imageFolder, QStringLiteral("--cache"), caseDir(QStringLiteral("cache")), QStringLiteral("--yes")});
    QVERIFY2(result.code == 4, qPrintable(result.dump()));
    QVERIFY(result.err.isEmpty());   // nothing asked
    QCOMPARE(QFile(machine.knownHostsFile).size(), qint64(0));
}

void Tst_firmwarecli::pullUnreachableExits4()
{
    CliRig rig;
    BuildMachine machine;
    QVERIFY(machine.start(&rig.store, caseDir(QStringLiteral("work"))));
    machine.server->setAcceptConnections(false);   // every connection is reset at once
    const CliResult result = rig.run({QStringLiteral("pull"), QStringLiteral("--target"), machine.profile.name, QStringLiteral("--remote-folder"),
                                      machine.imageFolder, QStringLiteral("--cache"), caseDir(QStringLiteral("cache"))});
    QVERIFY2(result.code == 4, qPrintable(result.dump()));
    QVERIFY(result.lastText().startsWith(QStringLiteral("su-fw: exit 4 (SSH): ")));
}

void Tst_firmwarecli::pullWithTheSavedPassword()
{
    // An agent's usual case: the profile's password was remembered in the app; no --password-*
    // option, nothing asked - SshConnection answers from SecretStore (here the test's settings).
    CliRig rig;
    BuildMachine machine;
    machine.passwordAuth = true;
    machine.password = kSecret;
    QVERIFY(machine.start(&rig.store, caseDir(QStringLiteral("work"))));
    const QString key = QStringLiteral("ssh/%1/password").arg(machine.profile.id);
    QVERIFY(SecretStore::store(key, kSecret));
    QVERIFY2(QSettings().fileName().startsWith(m_tempDir.path()), qPrintable(QSettings().fileName()));
    const CliResult result = rig.run({QStringLiteral("pull"), QStringLiteral("--target"), machine.profile.name, QStringLiteral("--remote-folder"),
                                      machine.imageFolder, QStringLiteral("--cache"), caseDir(QStringLiteral("cache"))});
    QVERIFY2(result.code == 0, qPrintable(result.dump()));
    QCOMPARE(machine.server->lastAuthMethod(), QStringLiteral("password"));
    QVERIFY(!result.out.contains(QStringLiteral("nobody answered")));
    QVERIFY(!result.out.contains(QStringLiteral("Answering")));
    QVERIFY(!result.out.contains(kSecret));
    QVERIFY(result.err.isEmpty());
    QVERIFY(SecretStore::contains(key));   // it worked, so it stays
    QVERIFY(SecretStore::remove(key));
}

void Tst_firmwarecli::interruptBeforeTheJobExists()
{
    // Ctrl+C at the --password-stdin prompt (no event loop runs during the read): remembered,
    // and the pull never starts.
    CliRig rig;
    int reads = 0;
    rig.cli.setInput(
        [&rig, &reads](bool) -> std::optional<QString> {
            ++reads;
            rig.cli.interrupt();
            return kSecret;
        },
        /*interactive=*/false);
    const QString logPath = caseDir(QStringLiteral("interrupt.log"));
    const CliResult result = rig.run({QStringLiteral("pull"), QStringLiteral("--target"), QStringLiteral("test@127.0.0.1:1"),
                                      QStringLiteral("--remote-folder"), QStringLiteral("/srv/image"), QStringLiteral("--cache"),
                                      caseDir(QStringLiteral("cache")), QStringLiteral("--password-stdin"), QStringLiteral("--log"), logPath});
    QVERIFY2(result.code == 5, qPrintable(result.dump()));
    QCOMPARE(reads, 1);
    QVERIFY(result.hasText(QStringLiteral("Interrupted (Ctrl+C)")));
    QCOMPARE(result.lastText(), QStringLiteral("su-fw: exit 5 (cancelled): interrupted before anything ran"));
    QVERIFY(!result.out.contains(QStringLiteral("Connecting to")));
    QVERIFY(!result.out.contains(kSecret));
    QCOMPARE(readAll(logPath).split(QLatin1Char('\n'), Qt::SkipEmptyParts), result.lines());
    QVERIFY(!rig.cli.isRunning());
}

void Tst_firmwarecli::interruptDuringAQuestionWaitsForIt()
{
    // Ctrl+C while the password is typed: the reader passes it on at once (main.cpp does), but the
    // cancel goes out after the question, from the event loop - not inside the job's callback.
    CliRig rig;
    BuildMachine machine;
    machine.passwordAuth = true;
    machine.password = kSecret;
    QVERIFY(machine.start(&rig.store, caseDir(QStringLiteral("work"))));
    int asked = 0;
    rig.cli.setInput(
        [&rig, &asked](bool) -> std::optional<QString> {
            ++asked;
            rig.cli.interrupt();
            return kSecret;
        },
        /*interactive=*/true);
    const CliResult result = rig.run({QStringLiteral("pull"), QStringLiteral("--target"), machine.profile.name, QStringLiteral("--remote-folder"),
                                      machine.imageFolder, QStringLiteral("--cache"), caseDir(QStringLiteral("cache"))});
    QVERIFY2(result.code == 5, qPrintable(result.dump()));
    QCOMPARE(asked, 1);
    const QStringList texts = result.texts();
    const qsizetype interrupted = texts.indexOf(QStringLiteral("Interrupted (Ctrl+C)"));
    QVERIFY2(interrupted >= 0 && texts.indexOf(QStringLiteral("Cancelling Pull")) > interrupted, qPrintable(result.dump()));
    QVERIFY(result.lastText().startsWith(QStringLiteral("su-fw: exit 5 (cancelled): interrupted: ")));
    QVERIFY(!result.out.contains(kSecret));
}

// =======================================================================================
// flash-soc
// =======================================================================================

void Tst_firmwarecli::flashAllInLoaderMode()
{
    CliRig rig;
    scriptSocFlash(rig.runner, kLdLoader);
    const QString image = makeImage(QStringLiteral("flash/update.img"));
    const CliResult result = rig.run({QStringLiteral("flash-soc"), QStringLiteral("--image"), image, QStringLiteral("--all"),
                                      QStringLiteral("--layout"), kNodeMtdParts});
    QVERIFY2(result.code == 0, qPrintable(result.dump()));
    QStringList expected;
    for (int k = 0; k < kNodePartitionCount; ++k) {
        expected << expectedWl(k, extractDirOf(image));
    }
    QCOMPARE(wlCommands(rig.runner), expected);
    QCOMPARE(rig.runner.count(QRegularExpression(QStringLiteral("^upgrade_tool RD$"))), 1);
    QCOMPARE(rig.runner.count(exfPattern()), 1);   // unpacked once: the plan and the flash share it
    QVERIFY(result.hasText(QStringLiteral("--- Flash SoC ---")));
    QVERIFY(result.hasText(QStringLiteral("Partitions checked: env, idblock, uboot, meta, boot, rootfs, userdata (Loader mode writes "
                                          "these one by one; MASKROM mode writes the whole image)")));
    QVERIFY(result.hasText(QStringLiteral("Board information:")));
    QVERIFY(result.hasText(QStringLiteral("Flash SoC done: Upgrade firmware ok; board is back on ADB")));
    QCOMPARE(result.lastText(), QStringLiteral("su-fw: exit 0 (done): Upgrade firmware ok; board is back on ADB"));
    // Progress every 10 %, not every upgrade_tool line.
    int progressLines = 0;
    for (const QString& text : result.texts()) {
        QVERIFY2(!text.contains(QStringLiteral("Write LBA from file (")), qPrintable(text));
        progressLines += text.startsWith(QStringLiteral("Flashing SoC: Writing ")) ? 1 : 0;
    }
    QVERIFY2(progressLines >= 1 && progressLines <= 12, qPrintable(QString::number(progressLines)));
}

void Tst_firmwarecli::flashPartitionsExactly()
{
    CliRig rig;
    scriptSocFlash(rig.runner, kLdLoader);
    const QString image = makeImage(QStringLiteral("flash/update.img"));
    const CliResult result = rig.run({QStringLiteral("flash-soc"), QStringLiteral("--image"), image, QStringLiteral("--partitions"),
                                      QStringLiteral("boot,rootfs"), QStringLiteral("--layout"), kNodeMtdParts});
    QVERIFY2(result.code == 0, qPrintable(result.dump()));
    const QString dir = extractDirOf(image);
    QCOMPARE(wlCommands(rig.runner), (QStringList{expectedWl(4, dir), expectedWl(5, dir)}));
    QVERIFY(result.hasText(QStringLiteral("Partitions checked: boot, rootfs (Loader mode writes these one by one; MASKROM mode writes "
                                          "the whole image)")));

    // Repeated options add up; the order is the image's.
    rig.runner.reset();
    scriptSocFlash(rig.runner, kLdLoader);
    const CliResult twice = rig.run({QStringLiteral("flash-soc"), QStringLiteral("--image"), image, QStringLiteral("--partitions"),
                                     QStringLiteral("userdata"), QStringLiteral("--partitions"), QStringLiteral("env"), QStringLiteral("--layout"),
                                     kNodeMtdParts});
    QVERIFY2(twice.code == 0, qPrintable(twice.dump()));
    QCOMPARE(wlCommands(rig.runner), (QStringList{expectedWl(0, dir), expectedWl(6, dir)}));
}

void Tst_firmwarecli::flashUnknownPartitionIsUsage()
{
    CliRig rig;
    scriptSocFlash(rig.runner, kLdLoader);
    const QString image = makeImage(QStringLiteral("flash/update.img"));
    const CliResult result = rig.run({QStringLiteral("flash-soc"), QStringLiteral("--image"), image, QStringLiteral("--partitions"),
                                      QStringLiteral("boot,kernel"), QStringLiteral("--layout"), kNodeMtdParts});
    QVERIFY2(result.code == 2, qPrintable(result.dump()));
    QVERIFY(result.err.startsWith(QStringLiteral("su-fw: --partitions: update.img has no partition \"kernel\"; it has env, idblock, uboot, "
                                                 "meta, boot, rootfs, userdata")));
    // Found after stdout began (the image had to be unpacked first): the result line follows too.
    QVERIFY2(result.lastText().startsWith(QStringLiteral("su-fw: exit 2 (usage): --partitions: update.img has no partition \"kernel\"")),
             qPrintable(result.dump()));
    QCOMPARE(rig.runner.count(QRegularExpression(QStringLiteral("^upgrade_tool (LD|WL|UF)"))), 0);   // the board was not touched
}

void Tst_firmwarecli::flashTooBigIsRefusedBeforeWriting()
{
    CliRig rig;
    scriptSocFlash(rig.runner, kLdLoader);
    const QString image = makeImage(QStringLiteral("big/big.img"));   // boot.img one byte larger than its partition
    CliResult result = rig.run({QStringLiteral("flash-soc"), QStringLiteral("--image"), image, QStringLiteral("--partitions"), QStringLiteral("boot"),
                                QStringLiteral("--layout"), kNodeMtdParts});
    QVERIFY2(result.code == 2, qPrintable(result.dump()));
    QVERIFY(result.err.startsWith(QStringLiteral("su-fw: --partitions: boot cannot be written: boot.img (3145729 bytes) does not fit the "
                                                 "partition (3145728 bytes)")));
    result = rig.run({QStringLiteral("flash-soc"), QStringLiteral("--image"), image, QStringLiteral("--all"), QStringLiteral("--layout"), kNodeMtdParts});
    QVERIFY2(result.code == 1, qPrintable(result.dump()));
    QVERIFY(result.hasText(QStringLiteral("Partition refused: boot.img (3145729 bytes) does not fit the partition (3145728 bytes)")));
    QVERIFY(result.lastText().endsWith(QStringLiteral("; nothing was written")));
    // rootfs alone is fine.
    QCOMPARE(rig.runner.count(QRegularExpression(QStringLiteral("^upgrade_tool (LD|WL|UF)"))), 0);
    result = rig.run({QStringLiteral("flash-soc"), QStringLiteral("--image"), image, QStringLiteral("--partitions"), QStringLiteral("rootfs"),
                      QStringLiteral("--layout"), kNodeMtdParts});
    QVERIFY2(result.code == 0, qPrintable(result.dump()));
    QCOMPARE(wlCommands(rig.runner), QStringList{expectedWl(5, extractDirOf(image))});
}

void Tst_firmwarecli::flashMaskromIgnoresTheChoice()
{
    CliRig rig;
    scriptSocFlash(rig.runner, kLdMaskrom);
    const QString image = makeImage(QStringLiteral("maskrom/update.img"));
    const CliResult result = rig.run({QStringLiteral("flash-soc"), QStringLiteral("--image"), image, QStringLiteral("--partitions"),
                                      QStringLiteral("boot"), QStringLiteral("--layout"), kNodeMtdParts, QStringLiteral("--json")});
    QVERIFY2(result.code == 0, qPrintable(result.dump()));
    QCOMPARE(rig.runner.count(QRegularExpression(QStringLiteral("^upgrade_tool UF "))), 1);
    QVERIFY(wlCommands(rig.runner).isEmpty());
    QVERIFY(result.out.contains(QStringLiteral("the partition selection (boot) is ignored: UF writes every partition")));
    bool allParse = false;
    const QList<QJsonObject> objects = jsonLines(result, &allParse);
    QVERIFY(allParse);
    QCOMPARE(objects.last().value(QStringLiteral("method")).toString(), QStringLiteral("upgrade-firmware"));
    QCOMPARE(objects.last().value(QStringLiteral("partitions")).toArray().size(), qsizetype(1));
}

void Tst_firmwarecli::flashNoBoardExits3()
{
    CliRig rig;
    scriptNoBoard(rig.runner);
    rig.setup = [](FirmwareJob& job) { job.setLoaderWaitSeconds(1); };
    const QString image = makeImage(QStringLiteral("flash/update.img"));
    const CliResult result = rig.run({QStringLiteral("flash-soc"), QStringLiteral("--image"), image, QStringLiteral("--all"),
                                      QStringLiteral("--layout"), kNodeMtdParts});
    QVERIFY2(result.code == 3, qPrintable(result.dump()));
    // The headless download mode: the MASKROM instructions, then the flasher's own timeout.
    QVERIFY(result.hasText(QStringLiteral("No board found: hold the MASKROM button while plugging in the USB-C cable (or while "
                                          "power-cycling the board); waiting up to 1 s")));
    QCOMPARE(result.lastText(), QStringLiteral("su-fw: exit 3 (no board): board did not enter MASKROM/Loader within 1 s"));
    QVERIFY(!result.out.contains(QStringLiteral("Sent \"reboot loader\"")));
}

void Tst_firmwarecli::flashFailingWlExits1WithTheTail()
{
    CliRig rig;
    rig.runner.addScript(QRegularExpression(QStringLiteral("^upgrade_tool WL ")), "Write LBA failed, err=-3\r\n", 1);   // before the good one
    scriptSocFlash(rig.runner, kLdLoader);
    const QString image = makeImage(QStringLiteral("flash/update.img"));
    const CliResult result = rig.run({QStringLiteral("flash-soc"), QStringLiteral("--image"), image, QStringLiteral("--all"),
                                      QStringLiteral("--layout"), kNodeMtdParts});
    QVERIFY2(result.code == 1, qPrintable(result.dump()));
    QCOMPARE(wlCommands(rig.runner).size(), qsizetype(1));   // stopped at the first
    QVERIFY(result.hasText(QStringLiteral("Write LBA failed, err=-3")));   // upgrade_tool's own line
    QCOMPARE(result.lastText(), QStringLiteral("su-fw: exit 1 (failed): writing env failed: Write LBA failed, err=-3"));
}

void Tst_firmwarecli::flashTimeoutExits5()
{
    CliRig rig;
    scriptNoBoard(rig.runner);   // the Loader wait stays 30 s: the timeout comes first
    const QString image = makeImage(QStringLiteral("flash/update.img"));
    QElapsedTimer clock;
    clock.start();
    const CliResult result = rig.run({QStringLiteral("flash-soc"), QStringLiteral("--image"), image, QStringLiteral("--all"),
                                      QStringLiteral("--layout"), kNodeMtdParts, QStringLiteral("--timeout"), QStringLiteral("1")});
    QVERIFY2(result.code == 5, qPrintable(result.dump()));
    QVERIFY2(clock.elapsed() < 15000, qPrintable(QString::number(clock.elapsed())));
    QVERIFY(result.hasText(QStringLiteral("Timeout: 1 s passed (--timeout); cancelling")));
    QVERIFY(result.hasText(QStringLiteral("Cancelling Flash SoC")));
    QCOMPARE(result.lastText(), QStringLiteral("su-fw: exit 5 (timeout): timeout after 1 s: cancelled"));
}

void Tst_firmwarecli::flashInterruptExits5()
{
    CliRig rig;
    scriptNoBoard(rig.runner);
    // Ctrl+C once the flasher waits for the board.
    rig.setup = [&rig](FirmwareJob& job) {
        QObject::connect(job.socFlasher(), &SocFlasher::phaseChanged, &rig.cli, [&rig](SocFlasher::Phase phase) {
            if (phase == SocFlasher::Phase::WaitingForLoader) {
                QTimer::singleShot(50, &rig.cli, &FirmwareCli::interrupt);
            }
        });
    };
    const QString image = makeImage(QStringLiteral("flash/update.img"));
    const CliResult result = rig.run({QStringLiteral("flash-soc"), QStringLiteral("--image"), image, QStringLiteral("--all"),
                                      QStringLiteral("--layout"), kNodeMtdParts});
    QVERIFY2(result.code == 5, qPrintable(result.dump()));
    QVERIFY(result.hasText(QStringLiteral("Interrupted (Ctrl+C)")));
    QVERIFY(result.hasText(QStringLiteral("Cancelling Flash SoC")));
    QCOMPARE(result.lastText(), QStringLiteral("su-fw: exit 5 (cancelled): interrupted: cancelled"));
    QVERIFY(!rig.cli.isRunning());
    rig.cli.interrupt();   // idle: nothing
}

void Tst_firmwarecli::flashInterruptWaitsForTheWrite()
{
    // Ctrl+C while upgrade_tool writes: refused (the job's rule), the write goes on, and su-fw stops
    // at the next phase that can be cancelled - the wait for ADB.
    CliRig rig;
    QPointer<FakeToolInvocation> uf;
    rig.runner.addScript(QRegularExpression(QStringLiteral("^upgrade_tool LD$")), kLdMaskrom);
    rig.runner.addHandler(exfPattern(), &nodeExtractor);
    rig.runner.addHandler(QRegularExpression(QStringLiteral("^upgrade_tool UF ")), [&uf](FakeToolInvocation* i) {
        uf = i;
        i->emitOutput("Download Image... (10%)\r");
        i->hang();
    });
    rig.runner.addScript(QRegularExpression(QStringLiteral("^adb( -s \\S+)? get-state$")), "error: no devices/emulators found\n", 1);
    bool interrupted = false;
    bool finished = false;
    QTimer driver;
    driver.setInterval(20);
    QObject::connect(&driver, &QTimer::timeout, &rig.cli, [&]() {
        if (uf && !interrupted) {
            interrupted = true;
            rig.cli.interrupt();
        } else if (uf && interrupted && !finished) {
            finished = true;
            uf->emitOutput("Upgrade firmware ok.\r\n");
            uf->finish(0);
        }
    });
    driver.start();
    const QString image = makeImage(QStringLiteral("flash/update.img"));
    const CliResult result = rig.run({QStringLiteral("flash-soc"), QStringLiteral("--image"), image, QStringLiteral("--all")});
    driver.stop();
    QVERIFY2(result.code == 5, qPrintable(result.dump()));
    QCOMPARE(rig.runner.count(QRegularExpression(QStringLiteral("^upgrade_tool UF "))), 1);
    const QStringList texts = result.texts();
    const qsizetype refused = texts.indexOf(QStringLiteral("Cannot cancel while upgrade_tool writes the flash"));
    const qsizetype cancelling = texts.indexOf(QStringLiteral("Cancelling Flash SoC"));
    QVERIFY2(refused >= 0 && cancelling > refused, qPrintable(result.dump()));
    QVERIFY(result.hasText(QStringLiteral("su-fw stops at the next point where that is safe")));
    QVERIFY(finished);   // the write ended on its own before the cancel went through
    // Not "cancelled" alone: the SoC was written, only the wait for ADB was cut short.
    QCOMPARE(result.lastText(),
             QStringLiteral("su-fw: exit 5 (cancelled): interrupted after the image was written; the ADB wait was cancelled"));
}

void Tst_firmwarecli::flashInterruptDuringAWriteStopsAfterIt()
{
    // Ctrl+C during a WL: refused; sent again from the event loop once the flasher waits for ADB -
    // never inside the flasher's own phase change, so the wait starts and is then cancelled, and
    // nothing runs or is logged after the operation's own result line.
    CliRig rig;
    QPointer<FakeToolInvocation> wl;
    rig.runner.addHandler(QRegularExpression(QStringLiteral("^upgrade_tool WL ")), [&wl](FakeToolInvocation* i) {
        wl = i;
        i->emitOutput("Write LBA from file (10%)\r");
        i->hang();
    });
    rig.runner.addScript(QRegularExpression(QStringLiteral("^adb( -s \\S+)? get-state$")), "error: no devices/emulators found\n", 1);
    scriptSocFlash(rig.runner, kLdLoader);
    qsizetype commandsAtTheEnd = -1;
    rig.setup = [&rig, &commandsAtTheEnd](FirmwareJob& job) {
        QObject::connect(&job, &FirmwareJob::operationFinished, &rig.cli,
                         [&rig, &commandsAtTheEnd]() { commandsAtTheEnd = rig.runner.invocations().size(); });
    };
    bool interrupted = false;
    bool written = false;
    QTimer driver;
    driver.setInterval(20);
    QObject::connect(&driver, &QTimer::timeout, &rig.cli, [&]() {
        if (wl && !interrupted) {
            interrupted = true;
            rig.cli.interrupt();
        } else if (wl && interrupted && !written) {
            written = true;
            wl->emitOutput("Write LBA from file (100%)\r\n");
            wl->finish(0);
        }
    });
    driver.start();
    const QString image = makeImage(QStringLiteral("flash/update.img"));
    const CliResult result = rig.run({QStringLiteral("flash-soc"), QStringLiteral("--image"), image, QStringLiteral("--partitions"),
                                      QStringLiteral("boot"), QStringLiteral("--layout"), kNodeMtdParts});
    driver.stop();
    QVERIFY2(result.code == 5, qPrintable(result.dump()));
    QVERIFY(written);
    QCOMPARE(wlCommands(rig.runner), QStringList{expectedWl(4, extractDirOf(image))});
    QCOMPARE(rig.runner.count(QRegularExpression(QStringLiteral("^upgrade_tool RD$"))), 1);
    const QStringList texts = result.texts();
    const qsizetype refused = texts.indexOf(QStringLiteral("Cannot cancel while upgrade_tool writes the flash"));
    const qsizetype waiting = texts.indexOf(QRegularExpression(QStringLiteral("^waiting for the board to come back on ADB.*")));
    const qsizetype cancelling = texts.indexOf(QStringLiteral("Cancelling Flash SoC"));
    QVERIFY2(refused >= 0 && waiting > refused && cancelling > waiting, qPrintable(result.dump()));
    QVERIFY(result.hasText(QStringLiteral("stopped waiting for ADB; the image was already written")));
    // The operation's result line is the last one before su-fw's own.
    QCOMPARE(texts.at(texts.size() - 2), QStringLiteral("Flash SoC failed: cancelled"));
    QCOMPARE(result.lastText(),
             QStringLiteral("su-fw: exit 5 (cancelled): interrupted after the image was written; the ADB wait was cancelled"));
    QCOMPARE(rig.runner.invocations().size(), commandsAtTheEnd);   // no adb started after the end
    QCOMPARE(rig.runner.count(QRegularExpression(QStringLiteral("get-state"))), 1);
}

void Tst_firmwarecli::flashBoardOnAdbGoesThroughLoader()
{
    // The bench's usual case: the board runs (ADB). flash-soc reads its kernel and flash layout over
    // ADB (the layout is stored for that board in the app's settings, as the tab does), sends
    // `adb -s <serial> reboot loader` without a question and writes in Loader mode.
    CliRig rig;
    auto inLoader = std::make_shared<bool>(false);
    rig.runner.addHandler(QRegularExpression(QStringLiteral("^upgrade_tool LD$")), [inLoader](FakeToolInvocation* i) {
        i->emitOutput(*inLoader ? kLdLoader : QByteArray("List of rockusb connected(0)\r\n"));
        i->finish(0);
    });
    rig.runner.addScript(QRegularExpression(QStringLiteral("^adb( -s \\S+)? devices$")), kAdbBoard);
    rig.runner.addHandler(QRegularExpression(QStringLiteral("^adb -s %1 reboot loader$").arg(kBoardSerial)), [inLoader](FakeToolInvocation* i) {
        *inLoader = true;   // the board restarts in Loader mode
        i->finish(0);
    });
    scriptSocFlash(rig.runner, kLdLoader);   // EXF, WL, RD, get-state, /proc/version, /proc/cmdline
    const QString image = tempPath(QStringLiteral("withenv-%1/update.img").arg(m_caseNumber));   // its env.img carries the layout
    QVERIFY(writeFile(image, rkfwBytes()));
    QVERIFY(AppSettings::instance().firmwareMtdParts(kBoardSerial).isEmpty());
    const CliResult result = rig.run({QStringLiteral("flash-soc"), QStringLiteral("--image"), image, QStringLiteral("--partitions"),
                                      QStringLiteral("boot"), QStringLiteral("--serial"), kBoardSerial, QStringLiteral("--yes")});
    QVERIFY2(result.code == 0, qPrintable(result.dump()));
    QVERIFY(result.err.isEmpty());   // nothing was asked
    const QString adb = QStringLiteral("adb -s ") + kBoardSerial;
    const QStringList commands = rig.runner.invocations();
    const qsizetype version = commands.indexOf(adb + QStringLiteral(" shell \"cat /proc/version\""));
    const qsizetype cmdline = commands.indexOf(adb + QStringLiteral(" shell \"cat /proc/cmdline\""));
    const qsizetype reboot = commands.indexOf(adb + QStringLiteral(" reboot loader"));
    const qsizetype write = commands.indexOf(QRegularExpression(QStringLiteral("^upgrade_tool WL .*")));
    QVERIFY2(version >= 0 && cmdline > version && reboot > cmdline && write > reboot, qPrintable(commands.join(QLatin1Char('\n'))));
    QCOMPARE(wlCommands(rig.runner), QStringList{expectedWl(4, extractDirOf(image))});
    QVERIFY(commands.contains(adb + QStringLiteral(" get-state")));   // the ADB wait asks the same board
    QVERIFY(result.hasText(QStringLiteral("board running 5.10.209+, rebooting it into loader mode")));
    QVERIFY(result.hasText(QStringLiteral("partition layout of the board: ") + kNodeMtdParts));
    // Stored where the app keeps it: the settings file this test points QSettings at.
    QCOMPARE(AppSettings::instance().firmwareMtdParts(kBoardSerial), kNodeMtdParts);
    QCOMPARE(AppSettings::instance().firmwareMtdParts(QString()), kNodeMtdParts);
    AppSettings::instance().sync();
    QSettings stored;
    QVERIFY2(stored.fileName().startsWith(m_tempDir.path()), qPrintable(stored.fileName()));
    QCOMPARE(stored.value(QStringLiteral("firmware/mtdparts")).toMap().value(kBoardSerial).toString(), kNodeMtdParts);
    QCOMPARE(result.lastText(), QStringLiteral("su-fw: exit 0 (done): Upgrade firmware ok; board is back on ADB"));
}

void Tst_firmwarecli::flashAdbNotBackIsStillDone()
{
    // "board flashed, but ADB did not come back": a success with a warning (flash-soc --help, exit 0).
    CliRig rig;
    rig.runner.addScript(QRegularExpression(QStringLiteral("^adb( -s \\S+)? get-state$")), "error: no devices/emulators found\n", 1);
    scriptSocFlash(rig.runner, kLdLoader);
    rig.setup = [](FirmwareJob& job) { job.setAdbReturnSeconds(1); };
    const QString image = makeImage(QStringLiteral("flash/update.img"));
    const CliResult result = rig.run({QStringLiteral("flash-soc"), QStringLiteral("--image"), image, QStringLiteral("--all"),
                                      QStringLiteral("--layout"), kNodeMtdParts});
    QVERIFY2(result.code == 0, qPrintable(result.dump()));
    QCOMPARE(wlCommands(rig.runner).size(), qsizetype(kNodePartitionCount));
    const QString message = QStringLiteral("board flashed, but ADB did not come back within 1 s; check the serial console");
    QVERIFY(result.hasText(QStringLiteral("Flash SoC done: ") + message));
    QCOMPARE(result.lastText(), QStringLiteral("su-fw: exit 0 (done): ") + message);
}

void Tst_firmwarecli::flashNoWait()
{
    CliRig rig;
    scriptSocFlash(rig.runner, kLdLoader);
    const QString image = makeImage(QStringLiteral("flash/update.img"));
    const CliResult result = rig.run({QStringLiteral("flash-soc"), QStringLiteral("--image"), image, QStringLiteral("--all"),
                                      QStringLiteral("--layout"), kNodeMtdParts, QStringLiteral("--no-wait")});
    QVERIFY2(result.code == 0, qPrintable(result.dump()));
    QCOMPARE(rig.runner.count(QRegularExpression(QStringLiteral("get-state"))), 0);
    QCOMPARE(wlCommands(rig.runner).size(), qsizetype(kNodePartitionCount));
}

void Tst_firmwarecli::flashJsonLines()
{
    CliRig rig;
    scriptSocFlash(rig.runner, kLdLoader);
    const QString cache = caseDir(QStringLiteral("cache"));
    QVERIFY(writeFile(cache + QStringLiteral("/update.img"), rkfwBytes()));   // the cached image is the default
    const CliResult result = rig.run({QStringLiteral("flash-soc"), QStringLiteral("--cache"), cache, QStringLiteral("--all"), QStringLiteral("--layout"),
                                      kNodeMtdParts, QStringLiteral("--json")});
    QVERIFY2(result.code == 0, qPrintable(result.dump()));
    bool allParse = false;
    const QList<QJsonObject> objects = jsonLines(result, &allParse);
    QVERIFY2(allParse, qPrintable(result.out));
    int progress = 0;
    int lastPercent = -1;
    int steps = 0;
    for (const QJsonObject& object : objects) {
        const QString event = object.value(QStringLiteral("event")).toString();
        if (event == QLatin1String("progress")) {
            ++progress;
            QCOMPARE(object.value(QStringLiteral("step")).toString(), QStringLiteral("flash"));
            QVERIFY(object.value(QStringLiteral("percent")).isDouble());
            lastPercent = object.value(QStringLiteral("percent")).toInt();
        } else if (event == QLatin1String("step")) {
            ++steps;
            QCOMPARE(object.value(QStringLiteral("step")).toString(), QStringLiteral("flash"));
            QCOMPARE(object.value(QStringLiteral("ok")).toBool(), true);
        } else if (event == QLatin1String("log")) {
            QVERIFY(!object.contains(QStringLiteral("percent")));
        }
    }
    QVERIFY(progress >= 2);
    QCOMPARE(lastPercent, 100);
    QCOMPARE(steps, 1);
    const QJsonObject done = objects.last();
    QCOMPARE(done.value(QStringLiteral("event")).toString(), QStringLiteral("done"));
    QCOMPARE(done.value(QStringLiteral("step")).toString(), QStringLiteral("flash-soc"));
    QCOMPARE(done.value(QStringLiteral("exit")).toInt(), 0);
    QCOMPARE(done.value(QStringLiteral("method")).toString(), QStringLiteral("partition-writes"));
    QCOMPARE(done.value(QStringLiteral("partitions")).toArray().size(), qsizetype(kNodePartitionCount));
    QCOMPARE(done.value(QStringLiteral("image")).toString(), QDir::toNativeSeparators(cache + QStringLiteral("/update.img")));
    QVERIFY(done.value(QStringLiteral("boardInfo")).toString().startsWith(QStringLiteral("Linux version 5.10.209+")));
    // The result line of the operation belongs to its step, not to the command.
    bool resultLine = false;
    for (const QJsonObject& object : objects) {
        if (object.value(QStringLiteral("text")).toString().startsWith(QStringLiteral("Flash SoC done: "))) {
            QCOMPARE(object.value(QStringLiteral("step")).toString(), QStringLiteral("flash"));
            resultLine = true;
        }
    }
    QVERIFY(resultLine);
}

void Tst_firmwarecli::flashLogFileHoldsTheSameLines()
{
    CliRig rig;
    scriptSocFlash(rig.runner, kLdLoader);
    const QString image = makeImage(QStringLiteral("flash/update.img"));
    const QString logPath = caseDir(QStringLiteral("logs/flash.log"));
    QVERIFY(QDir().mkpath(QFileInfo(logPath).absolutePath()));
    const CliResult result = rig.run({QStringLiteral("flash-soc"), QStringLiteral("--image"), image, QStringLiteral("--all"),
                                      QStringLiteral("--layout"), kNodeMtdParts, QStringLiteral("--log"), logPath});
    QVERIFY2(result.code == 0, qPrintable(result.dump()));
    QCOMPARE(readAll(logPath).split(QLatin1Char('\n'), Qt::SkipEmptyParts), result.lines());
    // Appended, also with --quiet (the console then shows only the result).
    const CliResult quiet = rig.run({QStringLiteral("flash-soc"), QStringLiteral("--image"), image, QStringLiteral("--all"), QStringLiteral("--layout"),
                                     kNodeMtdParts, QStringLiteral("--log"), logPath, QStringLiteral("--quiet")});
    QCOMPARE(quiet.code, 0);
    QCOMPARE(quiet.lines().size(), qsizetype(1));
    const QStringList logged = readAll(logPath).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    QVERIFY2(logged.size() > result.lines().size() + 10, qPrintable(QString::number(logged.size())));
    QCOMPARE(logged.mid(0, result.lines().size()), result.lines());
    QCOMPARE(logged.last(), quiet.lines().first());
}

void Tst_firmwarecli::anotherOperationRunningExits1()
{
    // One firmware operation per PC: while another holds the lock (the tab, another su-fw; here a
    // QLockFile of the test), an operation fails at once and status queries nothing - exit 1.
    CliRig rig;
    scriptSocFlash(rig.runner, kLdLoader);
    rig.runner.addScript(QRegularExpression(QStringLiteral("^adb( -s \\S+)? devices$")), kAdbBoard);
    QLockFile other(AppSettings::dataDirectory() + QStringLiteral("/firmware.lock"));
    QVERIFY(other.tryLock(0));
    qint64 pid = 0;
    QString host;
    QString program;
    QVERIFY(other.getLockInfo(&pid, &host, &program));
    const QString running = QStringLiteral("another firmware operation is running (%1)")
                                .arg(program.isEmpty() ? QStringLiteral("pid %1").arg(pid) : QStringLiteral("%1, pid %2").arg(program).arg(pid));
    const QRegularExpression board(QStringLiteral("^upgrade_tool (LD|WL|UF|RD)|^adb"));

    const QString image = makeImage(QStringLiteral("flash/update.img"));
    CliResult result = rig.run({QStringLiteral("flash-soc"), QStringLiteral("--image"), image, QStringLiteral("--all"),
                                QStringLiteral("--layout"), kNodeMtdParts});
    QVERIFY2(result.code == 1, qPrintable(result.dump()));
    QVERIFY(result.hasText(QStringLiteral("Flash SoC failed: ") + running));
    QCOMPARE(result.lastText(), QStringLiteral("su-fw: exit 1 (failed): ") + running);
    QCOMPARE(rig.runner.count(board), 0);

    result = rig.run({QStringLiteral("status")});
    QVERIFY2(result.code == 1, qPrintable(result.dump()));
    QVERIFY(result.hasText(QStringLiteral("status paused: ") + running));
    QCOMPARE(result.lastText(), QStringLiteral("su-fw: exit 1 (failed): ") + running);
    result = rig.run({QStringLiteral("mcu-info")});
    QVERIFY2(result.code == 1, qPrintable(result.dump()));
    QCOMPARE(rig.runner.count(board), 0);   // the board was never touched

    // Released: su-fw works again.
    other.unlock();
    result = rig.run({QStringLiteral("flash-soc"), QStringLiteral("--image"), image, QStringLiteral("--all"), QStringLiteral("--layout"),
                      kNodeMtdParts});
    QVERIFY2(result.code == 0, qPrintable(result.dump()));
    QVERIFY(!QFileInfo::exists(AppSettings::dataDirectory() + QStringLiteral("/firmware.lock")));   // and released again
}

// =======================================================================================
// MCU
// =======================================================================================

void Tst_firmwarecli::mcuInfo()
{
    CliRig rig;
    scriptAdbBoard(rig.runner);
    rig.runner.addScript(QRegularExpression(QStringLiteral("killall rfmcu")), QByteArray());
    rig.runner.addScript(QRegularExpression(QStringLiteral("rfmcu cmd version")), "OK version=0.8.2 baud=115200\r\n");
    rig.runner.addScript(QRegularExpression(QStringLiteral("rfmcu cmd help")),
                         "OK commands:\r\nOK help [command]\r\nOK version show firmware version\r\nOK rx rx status | rx dump on|off\r\n");
    const CliResult result = rig.run({QStringLiteral("mcu-info"), QStringLiteral("--json")});
    QVERIFY2(result.code == 0, qPrintable(result.dump()));
    bool allParse = false;
    const QList<QJsonObject> objects = jsonLines(result, &allParse);
    QVERIFY(allParse);
    const QJsonObject done = objects.last();
    QCOMPARE(done.value(QStringLiteral("text")).toString(), QStringLiteral("MCU application version=0.8.2 role=rx"));
    QCOMPARE(done.value(QStringLiteral("mcu")).toObject().value(QStringLiteral("version")).toString(), QStringLiteral("0.8.2"));
    QCOMPARE(done.value(QStringLiteral("mcu")).toObject().value(QStringLiteral("role")).toString(), QStringLiteral("rx"));
    QCOMPARE(done.value(QStringLiteral("board")).toObject().value(QStringLiteral("mode")).toString(), QStringLiteral("adb"));
}

void Tst_firmwarecli::mcuInfoNoBoardExits3()
{
    CliRig rig;
    scriptNoBoard(rig.runner);
    CliResult result = rig.run({QStringLiteral("mcu-info")});
    QVERIFY2(result.code == 3, qPrintable(result.dump()));
    QVERIFY(result.lastText().startsWith(QStringLiteral("su-fw: exit 3 (no board): mcu-info needs the board running (ADB); the PC sees: ")));
    QCOMPARE(rig.runner.count(QRegularExpression(QStringLiteral("rfmcu"))), 0);

    // A board in Loader mode cannot run rfmcu either.
    rig.runner.reset();
    rig.runner.addScript(QRegularExpression(QStringLiteral("^upgrade_tool LD$")), kLdLoader);
    result = rig.run({QStringLiteral("mcu-set-role"), QStringLiteral("--role"), QStringLiteral("tx")});
    QVERIFY2(result.code == 3, qPrintable(result.dump()));
    QVERIFY(result.lastText().startsWith(QStringLiteral("su-fw: exit 3 (no board): mcu-set-role needs the board running (ADB); the PC sees: Loader mode")));
}

void Tst_firmwarecli::mcuInfoSilentMcuExits1()
{
    CliRig rig;
    scriptAdbBoard(rig.runner);
    rig.runner.addScript(QRegularExpression(QStringLiteral("killall rfmcu")), QByteArray());
    rig.runner.addScript(QRegularExpression(QStringLiteral("rfmcu cmd version")), QByteArray(), 3);
    const CliResult result = rig.run({QStringLiteral("mcu-info")});
    QVERIFY2(result.code == 1, qPrintable(result.dump()));
    QCOMPARE(result.lastText(), QStringLiteral("su-fw: exit 1 (failed): the MCU did not answer \"version\""));
}

void Tst_firmwarecli::mcuUpdateThroughTheBridge()
{
    FakeMcu mcu;
    FakeMcuServer bridge(&mcu);
    QVERIFY(bridge.listen());
    CliRig rig;
    rig.runner.addScript(QRegularExpression(QStringLiteral("^upgrade_tool LD$")), "List of rockusb connected(0)\r\n");
    rig.runner.addScript(QRegularExpression(QStringLiteral("cat /proc/cmdline")), kNodeCmdline);
    scriptMcuBridge(rig.runner, bridge.port());
    const CliResult result = rig.run({QStringLiteral("mcu-update"), QStringLiteral("--package"), dataFile("firmware-0.8.1.pkg"),
                                      QStringLiteral("--role"), QStringLiteral("rx")});
    QVERIFY2(result.code == 0, qPrintable(result.dump()));
    CcPackage package;
    QVERIFY(CcPackage::load(dataFile("firmware-0.8.1.pkg"), &package));
    QCOMPARE(mcu.image(), package.image);   // every byte arrived, in order
    QCOMPARE(mcu.role(), static_cast<quint32>(CcProtocol::RoleRx));
    QVERIFY(result.hasText(QStringLiteral("Package firmware-0.8.1.pkg: ") + package.summary()));
    QVERIFY(result.hasText(QStringLiteral("Updating the MCU with firmware-0.8.1.pkg (role rx)")));
    QVERIFY(result.hasText(QStringLiteral("COMPLETE")));
    QVERIFY(result.hasTextStarting(QStringLiteral("INFO application restarted: version=0.8.1")));
    QVERIFY(result.lastText().startsWith(QStringLiteral("su-fw: exit 0 (done): ")));
    // The transfer's progress every 10 %: about eleven lines, the last one at 100 %.
    QStringList progress;
    for (const QString& text : result.texts()) {
        if (text.startsWith(QStringLiteral("Updating MCU: "))) {
            progress << text;
        }
    }
    QVERIFY2(progress.size() >= 10 && progress.size() <= 12, qPrintable(progress.join(QLatin1Char('\n'))));
    QVERIFY(progress.last().endsWith(QStringLiteral("(100 %)")));
    QTRY_VERIFY(rig.runner.count(QRegularExpression(QStringLiteral("forward --remove"))) >= 1);
}

void Tst_firmwarecli::mcuSetRole()
{
    FakeMcu mcu;
    FakeMcuServer bridge(&mcu);
    QVERIFY(bridge.listen());
    CliRig rig;
    rig.runner.addScript(QRegularExpression(QStringLiteral("^upgrade_tool LD$")), "List of rockusb connected(0)\r\n");
    rig.runner.addScript(QRegularExpression(QStringLiteral("cat /proc/cmdline")), kNodeCmdline);
    scriptMcuBridge(rig.runner, bridge.port());
    const CliResult result = rig.run({QStringLiteral("mcu-set-role"), QStringLiteral("--role"), QStringLiteral("tx"), QStringLiteral("--json")});
    QVERIFY2(result.code == 0, qPrintable(result.dump()));
    QCOMPARE(mcu.role(), static_cast<quint32>(CcProtocol::RoleTx));
    QVERIFY(mcu.image().isEmpty());
    QVERIFY(result.out.contains(QStringLiteral("Setting the MCU role to tx")));
    bool allParse = false;
    const QList<QJsonObject> objects = jsonLines(result, &allParse);
    QVERIFY(allParse);
    QCOMPARE(objects.last().value(QStringLiteral("mcu")).toObject().value(QStringLiteral("role")).toString(), QStringLiteral("tx"));
}

// =======================================================================================
// pull-flash
// =======================================================================================

namespace {

/// The tab's Pull & Flash rig of tst_firmwarejob, on the command line: the build machine, a MASKROM
/// board flashed with UF, the version read answered by `versionReply`, the bridge to `mcu`.
struct ChainRig
{
    CliRig rig;
    BuildMachine machine;
    FakeMcu mcu;
    std::unique_ptr<FakeMcuServer> bridge;
    std::shared_ptr<QByteArray> versionReply = std::make_shared<QByteArray>("OK version=0.8.1 role=rx\n");
    std::shared_ptr<int> versionExit = std::make_shared<int>(0);

    bool start(const QString& workDir)
    {
        if (!machine.start(&rig.store, workDir)) {
            return false;
        }
        bridge = std::make_unique<FakeMcuServer>(&mcu);
        if (!bridge->listen()) {
            return false;
        }
        scriptSocFlash(rig.runner, kLdMaskrom);
        auto reply = versionReply;
        auto exitCode = versionExit;
        rig.runner.addHandler(QRegularExpression(QStringLiteral("rfmcu cmd version")), [reply, exitCode](FakeToolInvocation* i) {
            if (!reply->isEmpty()) {
                i->emitOutput(*reply);
            }
            i->finish(*exitCode);
        });
        scriptMcuBridge(rig.runner, bridge->port());
        return true;
    }

    QStringList arguments(const QString& cache) const
    {
        return {QStringLiteral("pull-flash"), QStringLiteral("--target"), machine.profile.name, QStringLiteral("--remote-folder"),
                machine.imageFolder, QStringLiteral("--cache"), cache, QStringLiteral("--package"), dataFile("firmware-0.8.1.pkg")};
    }
};

} // namespace

void Tst_firmwarecli::pullFlashSkipsMcuWhenEqual()
{
    ChainRig chain;
    QVERIFY(chain.start(caseDir(QStringLiteral("work"))));
    const CliResult result = chain.rig.run(chain.arguments(caseDir(QStringLiteral("cache"))) + QStringList{QStringLiteral("--json")});
    QVERIFY2(result.code == 0, qPrintable(result.dump()));
    bool allParse = false;
    const QList<QJsonObject> objects = jsonLines(result, &allParse);
    QVERIFY(allParse);
    QStringList steps;
    for (const QJsonObject& object : objects) {
        if (object.value(QStringLiteral("event")).toString() == QLatin1String("step")) {
            steps << object.value(QStringLiteral("step")).toString();
            QVERIFY(object.value(QStringLiteral("ok")).toBool());
        }
    }
    QCOMPARE(steps, (QStringList{QStringLiteral("pull"), QStringLiteral("flash"), QStringLiteral("mcu")}));
    const QJsonObject done = objects.last();
    QCOMPARE(done.value(QStringLiteral("step")).toString(), QStringLiteral("pull-flash"));
    QCOMPARE(done.value(QStringLiteral("text")).toString(), QStringLiteral("the MCU already runs 0.8.1; update skipped"));
    QCOMPARE(done.value(QStringLiteral("mcu")).toObject().value(QStringLiteral("version")).toString(), QStringLiteral("0.8.1"));
    QVERIFY(done.value(QStringLiteral("image")).toObject().value(QStringLiteral("size")).toInteger() > 0);
    QVERIFY(done.value(QStringLiteral("boardInfo")).toString().startsWith(QStringLiteral("Linux version")));
    QCOMPARE(chain.rig.runner.count(QRegularExpression(QStringLiteral("^upgrade_tool UF "))), 1);
    QCOMPARE(chain.rig.runner.count(QRegularExpression(QStringLiteral("forward tcp:0"))), 0);   // no MCU transfer
}

void Tst_firmwarecli::pullFlashUpdatesMcuWhenDifferent()
{
    ChainRig chain;
    *chain.versionReply = "OK version=0.8.0 role=rx\n";
    QVERIFY(chain.start(caseDir(QStringLiteral("work"))));
    // --role only covers an MCU that cannot report one; this one reports rx.
    const CliResult result = chain.rig.run(chain.arguments(caseDir(QStringLiteral("cache")))
                                           + QStringList{QStringLiteral("--role"), QStringLiteral("tx")});
    QVERIFY2(result.code == 0, qPrintable(result.dump()));
    CcPackage package;
    QVERIFY(CcPackage::load(dataFile("firmware-0.8.1.pkg"), &package));
    QCOMPARE(chain.mcu.image(), package.image);
    QCOMPARE(chain.mcu.role(), static_cast<quint32>(CcProtocol::RoleRx));
    QVERIFY(result.hasText(QStringLiteral("The MCU reports role rx: --role tx is not used (su-fw mcu-set-role changes the role)")));
    QVERIFY(result.hasTextStarting(QStringLiteral("Pull & Flash done: ")));
    QCOMPARE(chain.rig.runner.count(QRegularExpression(QStringLiteral("forward tcp:0"))), 1);
}

void Tst_firmwarecli::pullFlashStopsAtTheFlash()
{
    ChainRig chain;
    QVERIFY(chain.start(caseDir(QStringLiteral("work"))));
    chain.rig.runner.reset();
    scriptNoBoard(chain.rig.runner);
    chain.rig.setup = [](FirmwareJob& job) { job.setLoaderWaitSeconds(1); };
    const CliResult result = chain.rig.run(chain.arguments(caseDir(QStringLiteral("cache"))));
    QVERIFY2(result.code == 3, qPrintable(result.dump()));
    QVERIFY(result.hasTextStarting(QStringLiteral("Pull: ")));
    QVERIFY(result.hasText(QStringLiteral("Flash SoC failed: board did not enter MASKROM/Loader within 1 s")));
    QCOMPARE(result.lastText(), QStringLiteral("su-fw: exit 3 (no board): stopped at Flash SoC: board did not enter MASKROM/Loader within 1 s"));
    QCOMPARE(chain.rig.runner.count(QRegularExpression(QStringLiteral("rfmcu"))), 0);
}

void Tst_firmwarecli::pullFlashStopsAtThePull()
{
    ChainRig chain;
    chain.machine.trustServer = false;
    QVERIFY(chain.start(caseDir(QStringLiteral("work"))));
    const CliResult result = chain.rig.run(chain.arguments(caseDir(QStringLiteral("cache"))));
    QVERIFY2(result.code == 4, qPrintable(result.dump()));
    QCOMPARE(result.lastText(), QStringLiteral("su-fw: exit 4 (SSH): stopped at Pull: Host key rejected for ") + chain.machine.target());
    QCOMPARE(chain.rig.runner.count(QRegularExpression(QStringLiteral("^upgrade_tool (LD|UF|WL)"))), 0);   // the board was not touched
}

void Tst_firmwarecli::pullFlashSilentMcuNeedsTheRole()
{
    // A CC1310 that cannot report its role (no CLI: it sits in its updater) and no --role: the chain
    // stops, and su-fw names its own way out (the job's text names the tab's button).
    ChainRig chain;
    *chain.versionReply = QByteArray();
    *chain.versionExit = 3;
    QVERIFY(chain.start(caseDir(QStringLiteral("work"))));
    const CliResult result = chain.rig.run(chain.arguments(caseDir(QStringLiteral("cache"))));
    QVERIFY2(result.code == 1, qPrintable(result.dump()));
    QVERIFY(result.lastText().startsWith(QStringLiteral("su-fw: exit 1 (failed): stopped at Update MCU: cannot read the running MCU "
                                                        "version (the MCU did not answer \"version\") and no role is confirmed")));
    QVERIFY2(result.hasText(QStringLiteral("The MCU cannot report its role (it may sit in its UART updater): pass --role rx|tx, the "
                                           "role it should run with")),
             qPrintable(result.dump()));
    QCOMPARE(chain.rig.runner.count(QRegularExpression(QStringLiteral("forward tcp:0"))), 0);
}

void Tst_firmwarecli::passwordVariableLeavesTheEnvironment()
{
    // --password-env VAR: read once, then removed from su-fw's environment - no tool (adb,
    // upgrade_tool, the adb server adb starts and leaves running) inherits the password.
    ChainRig chain;
    chain.machine.passwordAuth = true;
    chain.machine.password = kSecret;
    QStringList seenByTheTools;
    chain.rig.runner.addHandler(QRegularExpression(QStringLiteral("^upgrade_tool LD$")), [&seenByTheTools](FakeToolInvocation* i) {
        seenByTheTools << QProcessEnvironment::systemEnvironment().value(QString::fromLatin1(kSecretVariable), QStringLiteral("<unset>"));
        i->emitOutput(kLdMaskrom);
        i->finish(0);
    });
    QVERIFY(chain.start(caseDir(QStringLiteral("work"))));
    qputenv(kSecretVariable, kSecret.toUtf8());
    const CliResult result = chain.rig.run(chain.arguments(caseDir(QStringLiteral("cache")))
                                           + QStringList{QStringLiteral("--password-env"), QString::fromLatin1(kSecretVariable)});
    QVERIFY2(result.code == 0, qPrintable(result.dump()));
    QCOMPARE(chain.machine.server->lastAuthMethod(), QStringLiteral("password"));   // the secret did its job first
    QVERIFY(!seenByTheTools.isEmpty());
    for (const QString& seen : std::as_const(seenByTheTools)) {
        QCOMPARE(seen, QStringLiteral("<unset>"));
    }
    QVERIFY(!qEnvironmentVariableIsSet(kSecretVariable));
}

// =======================================================================================
// flash-soc, a fleet (v0.7.0)
// =======================================================================================

void Tst_firmwarecli::fleetUsageErrors()
{
    CliRig rig;
    const QString image = makeImage(QStringLiteral("fleet/update.img"));
    const QStringList base{QStringLiteral("flash-soc"), QStringLiteral("--image"), image};
    const auto with = [&base](const QStringList& more) { return base + more; };
    const struct
    {
        QStringList arguments;
        QString message;
    } cases[] = {
        {with({QStringLiteral("--serials"), QStringLiteral("aaa"), QStringLiteral("--all-boards"), QStringLiteral("--partitions"), QStringLiteral("rootfs")}),
         QStringLiteral("su-fw: --serials and --all-boards: choose one")},
        {with({QStringLiteral("--serials"), QStringLiteral("aaa"), QStringLiteral("--serial"), QStringLiteral("bbb"), QStringLiteral("--partitions"),
               QStringLiteral("rootfs")}),
         QStringLiteral("su-fw: --serial is for one board; for a fleet use --serials S1,S2,... or --all-boards")},
        {with({QStringLiteral("--all-boards"), QStringLiteral("--all")}),
         QStringLiteral("su-fw: --all is refused for a fleet: name the partitions with --partitions NAMES (idblock, uboot, env and meta are "
                        "written only when named)")},
        {with({QStringLiteral("--all-boards")}), QStringLiteral("su-fw: --partitions NAMES is required for a fleet (su-fw partitions lists them)")},
        {with({QStringLiteral("--serials"), QStringLiteral("aaa"), QStringLiteral("--partitions"), QStringLiteral(" , ")}),
         QStringLiteral("su-fw: --partitions NAMES is required for a fleet")},
        {with({QStringLiteral("--serials"), QStringLiteral(" , "), QStringLiteral("--partitions"), QStringLiteral("rootfs")}),
         QStringLiteral("su-fw: --serials needs serials, comma separated (e.g. 62ecfc6ddeb3f579,5a33c81a6e996266; TAG=SERIAL names a board)")},
        {with({QStringLiteral("--serials"), QStringLiteral("aa-bb"), QStringLiteral("--partitions"), QStringLiteral("rootfs")}),
         QStringLiteral("su-fw: --serials: \"aa-bb\" is not a serial")},
        {with({QStringLiteral("--serials"), QStringLiteral("aaa,bbb"), QStringLiteral("--serials"), QStringLiteral("aaa"), QStringLiteral("--partitions"),
               QStringLiteral("rootfs")}),
         QStringLiteral("su-fw: --serials: aaa is listed twice")},
        {with({QStringLiteral("--serials"), QStringLiteral("B=aaa,B=bbb"), QStringLiteral("--partitions"), QStringLiteral("rootfs")}),
         QStringLiteral("su-fw: --serials: tag \"B\" is used twice")},
        {with({QStringLiteral("--serials"), QStringLiteral("TOOLONGTAG=aaa"), QStringLiteral("--partitions"), QStringLiteral("rootfs")}),
         QStringLiteral("su-fw: --serials: tag \"TOOLONGTAG\" must be 1 to 8 letters, digits, - or _")},
        {with({QStringLiteral("--serials"), QStringLiteral("=aaa"), QStringLiteral("--partitions"), QStringLiteral("rootfs")}),
         QStringLiteral("su-fw: --serials: tag \"\" must be 1 to 8 letters, digits, - or _")},
        {with({QStringLiteral("--serials"), QStringLiteral("aaa"), QStringLiteral("--partitions"), QStringLiteral("rootfs"), QStringLiteral("--max-parallel"),
               QStringLiteral("0")}),
         QStringLiteral("su-fw: --max-parallel needs a whole number from 1 to 64")},
        {with({QStringLiteral("--all-boards"), QStringLiteral("--partitions"), QStringLiteral("rootfs"), QStringLiteral("--max-parallel"), QStringLiteral("65")}),
         QStringLiteral("su-fw: --max-parallel needs a whole number from 1 to 64")},
        {with({QStringLiteral("--all-boards"), QStringLiteral("--partitions"), QStringLiteral("rootfs"), QStringLiteral("--max-parallel"), QStringLiteral("x")}),
         QStringLiteral("su-fw: --max-parallel needs a whole number from 1 to 64")},
        {with({QStringLiteral("--partitions"), QStringLiteral("rootfs"), QStringLiteral("--dry-run")}),
         QStringLiteral("su-fw: --dry-run: a fleet option (--serials or --all-boards)")},
        {with({QStringLiteral("--partitions"), QStringLiteral("rootfs"), QStringLiteral("--max-parallel"), QStringLiteral("3")}),
         QStringLiteral("su-fw: --max-parallel: a fleet option (--serials or --all-boards)")},
        {with({QStringLiteral("--all"), QStringLiteral("--dry-run"), QStringLiteral("--max-parallel"), QStringLiteral("3")}),
         QStringLiteral("su-fw: --dry-run, --max-parallel: a fleet option (--serials or --all-boards)")},
        {with({QStringLiteral("--dry-run")}), QStringLiteral("su-fw: --dry-run: a fleet option (--serials or --all-boards)")},
    };
    for (const auto& c : cases) {
        const CliResult result = rig.run(c.arguments);
        QVERIFY2(result.code == 2, qPrintable(result.dump()));
        QVERIFY2(result.out.isEmpty(), qPrintable(result.dump()));
        QVERIFY2(result.err.startsWith(c.message), qPrintable(result.dump()));
        QVERIFY2(result.err.contains(QStringLiteral("--help")), qPrintable(result.dump()));
    }
    QVERIFY(rig.runner.invocations().isEmpty());

    // The default tags skip the explicit ones (a dry run of serials nobody attached shows them).
    FleetCliRig fleet;
    const QString env = withEnvImage();
    CliResult result = fleet.run({QStringLiteral("flash-soc"), QStringLiteral("--serials"), QStringLiteral("B=aaa,bbb"), QStringLiteral("--partitions"),
                                  QStringLiteral("rootfs"), QStringLiteral("--dry-run"), QStringLiteral("--image"), env});
    QVERIFY2(result.code == 3, qPrintable(result.dump()));
    QVERIFY2(result.hasTextStarting(QStringLiteral("[B aaa] not on ADB: board aaa is not attached; attached: ")), qPrintable(result.dump()));
    QVERIFY(result.hasTextStarting(QStringLiteral("[A bbb] not on ADB: board bbb is not attached; attached: ")));
    result = fleet.run({QStringLiteral("flash-soc"), QStringLiteral("--serials"), QStringLiteral("A=aaa,bbb,C=ccc"), QStringLiteral("--partitions"),
                        QStringLiteral("rootfs"), QStringLiteral("--dry-run"), QStringLiteral("--image"), env});
    QVERIFY(result.hasTextStarting(QStringLiteral("[B bbb] not on ADB: ")));
    QVERIFY(result.hasTextStarting(QStringLiteral("[C ccc] not on ADB: ")));
}

void Tst_firmwarecli::fleetDryRun()
{
    {
        FleetCliRig fleet;
        const QString image = withEnvImage();
        const CliResult result = fleet.run({QStringLiteral("flash-soc"), QStringLiteral("--all-boards"), QStringLiteral("--partitions"), QStringLiteral("rootfs"),
                                            QStringLiteral("--dry-run"), QStringLiteral("--image"), image});
        QVERIFY2(result.code == 0, qPrintable(result.dump()));
        const QStringList texts = result.texts();
        const qsizetype banner = texts.indexOf(QStringLiteral("--- Flash fleet ---"));
        QVERIFY2(banner >= 0 && banner + 1 < texts.size(), qPrintable(result.dump()));
        QCOMPARE(texts.at(banner + 1), QStringLiteral("Dry run: the selection, the remembered ports and the plan; no reboot, no write"));
        QVERIFY(!result.out.contains(QStringLiteral("(dry run)")));
        for (const FleetBench& b : kFleetBench) {
            QVERIFY2(result.hasText(QStringLiteral("[%1 %2] on ADB; expected port unknown (first run)").arg(QLatin1String(b.tag), QLatin1String(b.serial))),
                     qPrintable(result.dump()));
        }
        QVERIFY(result.hasText(QStringLiteral("Plan: write rootfs (3830272 bytes, sector 8064, 10240 sectors) on 7 boards, up to 7 at once")));
        const qsizetype header = texts.indexOf(QRegularExpression(QStringLiteral("^board  serial .*expected port.*")));
        QVERIFY2(header >= 0, qPrintable(result.dump()));
        int ready = 0;
        for (qsizetype i = header + 1; i < texts.size(); ++i) {
            ready += texts.at(i).contains(QStringLiteral("  ready")) ? 1 : 0;
        }
        QCOMPARE(ready, 7);
        QCOMPARE(fleet.rig.runner.count(QRegularExpression(QStringLiteral("reboot loader| WL | RD$"))), 0);
        QCOMPARE(result.lastText(), QStringLiteral("su-fw: exit 0 (done): dry run: 7 of 7 boards ready"));
    }
    {
        FleetCliRig fleet(false);
        fleet.addBench({2});
        const CliResult result = fleet.run({QStringLiteral("flash-soc"), QStringLiteral("--all-boards"), QStringLiteral("--partitions"), QStringLiteral("rootfs"),
                                            QStringLiteral("--dry-run"), QStringLiteral("--image"), withEnvImage(QStringLiteral("b"))});
        QVERIFY2(result.code == 0, qPrintable(result.dump()));   // --all-boards: an absent board is not selected at all
        FleetCliRig named(false);
        named.addBench({2});
        const CliResult skipped = named.run({QStringLiteral("flash-soc"), QStringLiteral("--serials"),
                                             QStringLiteral("%1,%2,%3").arg(benchSerial(0), benchSerial(1), benchSerial(2)), QStringLiteral("--partitions"),
                                             QStringLiteral("rootfs"), QStringLiteral("--dry-run"), QStringLiteral("--image"), withEnvImage(QStringLiteral("c"))});
        QVERIFY2(skipped.code == 1, qPrintable(skipped.dump()));
        bool skippedRow = false;
        for (const QString& text : skipped.texts()) {
            skippedRow = skippedRow || (text.startsWith(QStringLiteral("C  ")) && text.contains(QStringLiteral("  skipped  ")));
        }
        QVERIFY2(skippedRow, qPrintable(skipped.dump()));
        QCOMPARE(skipped.lastText(), QStringLiteral("su-fw: exit 1 (failed): dry run: 2 of 3 boards ready; 1 skipped"));
    }
    {
        FleetCliRig fleet(false);
        fleet.script();   // no board at all
        const CliResult result = fleet.run({QStringLiteral("flash-soc"), QStringLiteral("--all-boards"), QStringLiteral("--partitions"), QStringLiteral("rootfs"),
                                            QStringLiteral("--dry-run"), QStringLiteral("--image"), withEnvImage(QStringLiteral("d"))});
        QVERIFY2(result.code == 3, qPrintable(result.dump()));
        QCOMPARE(result.lastText(), QStringLiteral("su-fw: exit 3 (no board): no selected board is on ADB (attached: none)"));
    }
    {
        FleetCliRig fleet;
        const CliResult result = fleet.run({QStringLiteral("flash-soc"), QStringLiteral("--all-boards"), QStringLiteral("--partitions"), QStringLiteral("rootfs"),
                                            QStringLiteral("--dry-run"), QStringLiteral("--image"), withEnvImage(QStringLiteral("e")), QStringLiteral("--json")});
        QVERIFY2(result.code == 0, qPrintable(result.dump()));
        bool allParse = false;
        const QList<QJsonObject> objects = jsonLines(result, &allParse);
        QVERIFY2(allParse, qPrintable(result.out));
        const QJsonObject done = objects.last();
        QCOMPARE(done.value(QStringLiteral("event")).toString(), QStringLiteral("done"));
        QCOMPARE(done.value(QStringLiteral("dryRun")).toBool(), true);
        QCOMPARE(done.value(QStringLiteral("fleet")).toBool(), true);
        QCOMPARE(done.value(QStringLiteral("boards")).toArray().size(), qsizetype(7));
        QCOMPARE(done.value(QStringLiteral("summary")).toObject().value(QStringLiteral("ready")).toInt(), 7);
    }
}

void Tst_firmwarecli::fleetTwoBoards()
{
    FleetCliRig fleet;
    const QString image = withEnvImage();
    const QStringList arguments{QStringLiteral("flash-soc"), QStringLiteral("--serials"), QStringLiteral("F=c4130950110960e0,G=5a33c81a6e996266"),
                                QStringLiteral("--partitions"), QStringLiteral("rootfs"), QStringLiteral("--image"), image, QStringLiteral("--yes")};
    CliResult result = fleet.run(arguments);
    QVERIFY2(result.code == 0, qPrintable(result.dump()));
    QVERIFY(result.err.isEmpty());
    QVERIFY2(result.hasTextStarting(QStringLiteral("[F c4130950110960e0] in Loader mode on port 6&3A944CE5&0&1, LocationID ")), qPrintable(result.dump()));
    QVERIFY(result.hasTextStarting(QStringLiteral("[G 5a33c81a6e996266] in Loader mode on port 6&3A944CE5&0&4, LocationID ")));
    QVERIFY(result.hasTextStarting(QStringLiteral("[F c4130950110960e0] rootfs written: 3830272 bytes in ")));
    QVERIFY(result.hasTextStarting(QStringLiteral("[F c4130950110960e0] done in ")));
    QVERIFY(result.hasTextStarting(QStringLiteral("[F c4130950110960e0] back on ADB: 5.10.209+, up 0 min; rfmcu: pid=412 uart=owned mcu=alive")));
    QVERIFY(result.hasTextStarting(QStringLiteral("Flash fleet done: 2 of 2 boards flashed in ")));
    const QStringList texts = result.texts();
    const qsizetype header = texts.indexOf(QRegularExpression(QStringLiteral("^board  serial .*loader  result.*")));
    QVERIFY2(header >= 0 && header + 3 == texts.size() - 1, qPrintable(result.dump()));   // two rows, then su-fw's line
    QVERIFY(texts.at(header + 1).startsWith(QStringLiteral("F      c4130950110960e0  6&3A944CE5&0&1  ")));
    QVERIFY(texts.at(header + 2).startsWith(QStringLiteral("G      5a33c81a6e996266  6&3A944CE5&0&4  ")));
    QVERIFY(result.lastText().startsWith(QStringLiteral("su-fw: exit 0 (done): 2 of 2 boards flashed in ")));
    for (const QString& command : fleet.rig.runner.invocations()) {
        if (command.contains(QStringLiteral(" WL ")) || command.endsWith(QStringLiteral(" RD"))) {
            QVERIFY2(command.startsWith(QStringLiteral("upgrade_tool -s ")), qPrintable(command));
        }
    }
    QCOMPARE(fleet.rig.runner.count(QRegularExpression(QStringLiteral("^upgrade_tool -s \\d+ WL "))), 2);
    QCOMPARE(fleet.bus.unaddressedCommands(), 0);
    // The learned ports are remembered per board, where the app keeps its settings.
    QCOMPARE(AppSettings::instance().firmwareBoardPort(QStringLiteral("c4130950110960e0")), QStringLiteral("6&3A944CE5&0&1"));
    QCOMPARE(AppSettings::instance().firmwareBoardPort(QStringLiteral("5a33c81a6e996266")), QStringLiteral("6&3A944CE5&0&4"));
    AppSettings::instance().sync();
    QCOMPARE(QSettings().value(QStringLiteral("firmware/boardPorts")).toMap().value(QStringLiteral("c4130950110960e0")).toString(),
             QStringLiteral("6&3A944CE5&0&1"));
    result = fleet.run(arguments);
    QVERIFY2(result.code == 0, qPrintable(result.dump()));
    QVERIFY(result.hasText(QStringLiteral("[F c4130950110960e0] on ADB; expected port 6&3A944CE5&0&1 (remembered)")));
    QVERIFY(!result.out.contains(QStringLiteral("(moved")));
}

void Tst_firmwarecli::fleetAllBoardsAndMaxParallelOne()
{
    FleetCliRig fleet;
    const CliResult result = fleet.run({QStringLiteral("flash-soc"), QStringLiteral("--all-boards"), QStringLiteral("--partitions"), QStringLiteral("rootfs"),
                                        QStringLiteral("--image"), withEnvImage(), QStringLiteral("--max-parallel"), QStringLiteral("1")});
    QVERIFY2(result.code == 0, qPrintable(result.dump()));
    for (int k = 0; k < 7; ++k) {
        QVERIFY(result.hasTextStarting(QStringLiteral("[%1 %2] done in ").arg(FleetFlasher::tagFor(k), benchSerial(k))));
        QCOMPARE(fleet.bus.writesTo(benchSerial(k)).size(), qsizetype(1));
    }
    QCOMPARE(fleet.bus.maxConcurrentWrites(), 1);
    QVERIFY(result.hasText(QStringLiteral("mapping done: 7 of 7 boards in Loader mode; writing starts (up to 1 at once)")));
}

void Tst_firmwarecli::fleetAllBoardsNextToAPhoneExits1()
{
    const QString refusal = QStringLiteral("adb lists 8 devices but the USB bus shows 7 Rockchip ADB gadgets: a non-Rockchip ADB device is attached; "
                                           "name the boards with --serials");
    FleetCliRig fleet;
    fleet.bus.addForeignAdb(QStringLiteral("ZY22PHONE"));
    const QString image = withEnvImage();
    CliResult result = fleet.run({QStringLiteral("flash-soc"), QStringLiteral("--all-boards"), QStringLiteral("--partitions"), QStringLiteral("rootfs"),
                                  QStringLiteral("--image"), image});
    QVERIFY2(result.code == 1, qPrintable(result.dump()));
    QCOMPARE(result.lastText(), QStringLiteral("su-fw: exit 1 (failed): ") + refusal);
    QCOMPARE(fleet.rig.runner.count(QRegularExpression(QStringLiteral("reboot loader"))), 0);
    result = fleet.run({QStringLiteral("flash-soc"), QStringLiteral("--all-boards"), QStringLiteral("--partitions"), QStringLiteral("rootfs"),
                        QStringLiteral("--image"), image, QStringLiteral("--dry-run")});
    QVERIFY2(result.code == 1, qPrintable(result.dump()));
    QCOMPARE(result.lastText(), QStringLiteral("su-fw: exit 1 (failed): ") + refusal);
    QStringList serials;
    for (int k = 0; k < 7; ++k) {
        serials << benchSerial(k);
    }
    result = fleet.run({QStringLiteral("flash-soc"), QStringLiteral("--serials"), serials.join(QLatin1Char(',')), QStringLiteral("--partitions"),
                        QStringLiteral("rootfs"), QStringLiteral("--image"), image});
    QVERIFY2(result.code == 0, qPrintable(result.dump()));
    QCOMPARE(fleet.rig.runner.count(QRegularExpression(QStringLiteral("ZY22PHONE"))), 0);
}

void Tst_firmwarecli::fleetJsonLines()
{
    FleetCliRig fleet(false);
    fleet.addBench({2});
    QStringList serials;
    for (int k = 0; k < 7; ++k) {
        serials << benchSerial(k);
    }
    const CliResult result = fleet.run({QStringLiteral("flash-soc"), QStringLiteral("--serials"), serials.join(QLatin1Char(',')),
                                        QStringLiteral("--partitions"), QStringLiteral("rootfs"), QStringLiteral("--image"), withEnvImage(),
                                        QStringLiteral("--json")});
    QVERIFY2(result.code == 1, qPrintable(result.dump()));
    bool allParse = false;
    const QList<QJsonObject> objects = jsonLines(result, &allParse);
    QVERIFY2(allParse, qPrintable(result.out));
    QMap<QString, int> progressPerBoard;
    int boardSteps = 0;
    int operationSteps = 0;
    int loaderLines = 0;
    for (const QJsonObject& object : objects) {
        const QString event = object.value(QStringLiteral("event")).toString();
        const bool board = object.contains(QStringLiteral("board"));
        if (board) {
            QVERIFY(object.contains(QStringLiteral("tag")));
            QVERIFY(object.contains(QStringLiteral("stage")));
            QCOMPARE(object.value(QStringLiteral("step")).toString(), QStringLiteral("fleet-flash"));
        }
        if (event == QLatin1String("log") && board && object.value(QStringLiteral("text")).toString().startsWith(QStringLiteral("in Loader mode on port "))) {
            ++loaderLines;
            QVERIFY(!object.value(QStringLiteral("port")).toString().isEmpty());
            QVERIFY(!object.value(QStringLiteral("loader")).toString().isEmpty());
            QCOMPARE(object.value(QStringLiteral("stage")).toString(), QStringLiteral("loader"));
        } else if (event == QLatin1String("progress") && board) {
            ++progressPerBoard[object.value(QStringLiteral("board")).toString()];
            QVERIFY(object.value(QStringLiteral("percent")).isDouble());
            QCOMPARE(object.value(QStringLiteral("stage")).toString(), QStringLiteral("writing"));
        } else if (event == QLatin1String("step") && board) {
            ++boardSteps;
            for (const char* key : {"result", "ok", "seconds", "mappingSeconds", "writingSeconds", "written", "retry", "adbBack", "text"}) {
                QVERIFY2(object.contains(QLatin1String(key)), key);
            }
            if (object.value(QStringLiteral("result")).toString() == QLatin1String("done")) {
                QCOMPARE(object.value(QStringLiteral("written")).toArray(), QJsonArray{QStringLiteral("rootfs")});
                QCOMPARE(object.value(QStringLiteral("adbBack")).toBool(), true);
                QVERIFY(object.value(QStringLiteral("text")).toString().startsWith(QStringLiteral("done in ")));
            }
        } else if (event == QLatin1String("step")) {
            ++operationSteps;
            QCOMPARE(object.value(QStringLiteral("step")).toString(), QStringLiteral("fleet-flash"));
            QCOMPARE(object.value(QStringLiteral("ok")).toBool(), false);
        }
    }
    QCOMPARE(loaderLines, 6);
    QCOMPARE(boardSteps, 7);
    QCOMPARE(operationSteps, 1);
    QCOMPARE(progressPerBoard.size(), qsizetype(6));
    for (auto it = progressPerBoard.cbegin(); it != progressPerBoard.cend(); ++it) {
        QVERIFY2(it.value() >= 2 && it.value() <= 11, qPrintable(it.key() + QStringLiteral(": ") + QString::number(it.value())));   // every 10 %
    }
    const QJsonObject done = objects.last();
    QCOMPARE(done.value(QStringLiteral("event")).toString(), QStringLiteral("done"));
    QCOMPARE(done.value(QStringLiteral("step")).toString(), QStringLiteral("flash-soc"));
    QCOMPARE(done.value(QStringLiteral("exit")).toInt(), 1);
    QCOMPARE(done.value(QStringLiteral("fleet")).toBool(), true);
    QCOMPARE(done.value(QStringLiteral("dryRun")).toBool(), false);
    QCOMPARE(done.value(QStringLiteral("partitions")).toArray(), QJsonArray{QStringLiteral("rootfs")});
    QCOMPARE(done.value(QStringLiteral("summary")).toObject(),
             (QJsonObject{{QStringLiteral("selected"), 7}, {QStringLiteral("done"), 6}, {QStringLiteral("notBack"), 0}, {QStringLiteral("failed"), 0},
                          {QStringLiteral("skipped"), 1}}));
    QCOMPARE(done.value(QStringLiteral("stop")).toString(), QString());
    QVERIFY(done.value(QStringLiteral("unattributed")).toArray().isEmpty());
    const QJsonArray boards = done.value(QStringLiteral("boards")).toArray();
    QCOMPARE(boards.size(), qsizetype(7));
    const QJsonObject first = boards.first().toObject();
    for (const char* key : {"tag", "serial", "adb", "expectedPort", "port", "loader", "stage", "result", "ok", "seconds", "mappingSeconds",
                            "writingSeconds", "detail", "written", "retry", "adbBack", "leftInLoader", "kernel", "uptime", "boardInfo", "rfmcu"}) {
        QVERIFY2(first.contains(QLatin1String(key)), key);
    }
    QCOMPARE(first.value(QStringLiteral("tag")).toString(), QStringLiteral("A"));
    QCOMPARE(first.value(QStringLiteral("port")).toString(), benchPort(0));
    QCOMPARE(first.value(QStringLiteral("rfmcu")).toString(), QStringLiteral("pid=412 uart=owned mcu=alive"));
    QCOMPARE(boards.at(2).toObject().value(QStringLiteral("result")).toString(), QStringLiteral("skipped"));
}

void Tst_firmwarecli::fleetNotBackExits1()
{
    FleetCliRig fleet;
    fleet.bus.setNoAdbReturn(benchSerial(1));
    fleet.extraSetup = [](FirmwareJob& job) { job.setAdbReturnSeconds(1); };
    const CliResult result = fleet.run({QStringLiteral("flash-soc"), QStringLiteral("--all-boards"), QStringLiteral("--partitions"), QStringLiteral("rootfs"),
                                        QStringLiteral("--image"), withEnvImage()});
    QVERIFY2(result.code == 1, qPrintable(result.dump()));
    bool row = false;
    for (const QString& text : result.texts()) {
        row = row || (text.startsWith(QStringLiteral("B      ") + benchSerial(1)) && text.contains(QStringLiteral("  not back  ")));
    }
    QVERIFY2(row, qPrintable(result.dump()));
    QVERIFY2(result.lastText().endsWith(QStringLiteral("; 1 not back on ADB")), qPrintable(result.lastText()));
}

void Tst_firmwarecli::fleetInterruptDrains()
{
    FleetCliRig fleet;
    fleet.bus.hangWrites(benchSerial(0));
    fleet.bus.hangWrites(benchSerial(1));
    qsizetype commandsAtTheEnd = -1;
    fleet.extraSetup = [&fleet, &commandsAtTheEnd](FirmwareJob& job) {
        QObject::connect(&job, &FirmwareJob::operationFinished, &fleet.rig.cli,
                         [&fleet, &commandsAtTheEnd]() { commandsAtTheEnd = fleet.rig.runner.invocations().size(); });
    };
    bool interrupted = false;
    bool released = false;
    QTimer driver;
    driver.setInterval(10);
    QObject::connect(&driver, &QTimer::timeout, &fleet.rig.cli, [&]() {
        if (!interrupted && fleet.bus.writesTo(benchSerial(0)).size() == 1 && fleet.bus.writesTo(benchSerial(1)).size() == 1) {
            interrupted = true;
            fleet.rig.cli.interrupt();
        } else if (interrupted && !released) {
            int reset = 0;
            for (int k = 2; k < 7; ++k) {
                reset += fleet.bus.resetsTo(benchSerial(k)).size() == 1 ? 1 : 0;
            }
            if (reset == 5) {
                released = true;
                fleet.rig.cli.interrupt();   // a second Ctrl+C: only what it still waits for
                fleet.bus.releaseWrites();
            }
        }
    });
    driver.start();
    const CliResult result = fleet.run({QStringLiteral("flash-soc"), QStringLiteral("--all-boards"), QStringLiteral("--partitions"), QStringLiteral("rootfs"),
                                        QStringLiteral("--image"), withEnvImage(), QStringLiteral("--max-parallel"), QStringLiteral("2")});
    driver.stop();
    QVERIFY2(result.code == 5, qPrintable(result.dump()));
    QVERIFY(released);
    QVERIFY(result.hasText(QStringLiteral("Cancelling Flash fleet")));
    QVERIFY(result.hasText(QStringLiteral("still waiting for 2 write(s) to finish; the writes are never interrupted")));
    for (int k = 0; k < 2; ++k) {
        QVERIFY(result.hasTextStarting(QStringLiteral("[%1 %2] done in ").arg(FleetFlasher::tagFor(k), benchSerial(k))));
        QCOMPARE(fleet.bus.resetsTo(benchSerial(k)).size(), qsizetype(1));
    }
    for (int k = 2; k < 7; ++k) {
        QVERIFY(fleet.bus.writesTo(benchSerial(k)).isEmpty());
        QVERIFY(result.hasText(QStringLiteral("[%1 %2] cancelled before its write started; reset back to ADB; skipped").arg(FleetFlasher::tagFor(k), benchSerial(k))));
    }
    QVERIFY2(result.lastText().startsWith(QStringLiteral("su-fw: exit 5 (cancelled): interrupted: 2 of 7 boards flashed in ")), qPrintable(result.lastText()));
    QVERIFY(result.lastText().endsWith(QStringLiteral("; 5 skipped")));
    QCOMPARE(fleet.rig.runner.invocations().size(), commandsAtTheEnd);   // nothing ran after the operation's result
    QCOMPARE(fleet.bus.cancelledWrites(), 0);
}

void Tst_firmwarecli::fleetTimeoutDuringMapping()
{
    FleetCliRig fleet;
    fleet.bus.setRebootDelayMs(400);
    fleet.bus.setPnpLagMs(0);
    fleet.extraSetup = [](FirmwareJob& job) { job.fleetFlasher()->setPollIntervalMs(50); };
    const CliResult result = fleet.run({QStringLiteral("flash-soc"), QStringLiteral("--all-boards"), QStringLiteral("--partitions"), QStringLiteral("rootfs"),
                                        QStringLiteral("--image"), withEnvImage(), QStringLiteral("--timeout"), QStringLiteral("1")});
    QVERIFY2(result.code == 5, qPrintable(result.dump()));
    // The 1 s run from the job's creation (the plan, adb devices, the baseline LD come first): on a
    // loaded runner the first board may not be mapped yet, so only "not every board" is pinned.
    QVERIFY2(result.lastText().startsWith(QStringLiteral("su-fw: exit 5 (timeout): timeout after 1 s")), qPrintable(result.lastText()));
    QCOMPARE(fleet.rig.runner.count(QRegularExpression(QStringLiteral(" WL "))), 0);
    int mapped = 0;
    for (int k = 0; k < 7; ++k) {
        const bool inLoader = result.hasTextStarting(QStringLiteral("[%1 %2] in Loader mode on port ").arg(FleetFlasher::tagFor(k), benchSerial(k)));
        mapped += inLoader ? 1 : 0;
        QCOMPARE(fleet.bus.resetsTo(benchSerial(k)).size(), qsizetype(inLoader ? 1 : 0));   // every mapped board reset, no other
    }
    QVERIFY(mapped < 7);
    QCOMPARE(fleet.bus.cancelledWrites(), 0);
}

void Tst_firmwarecli::fleetTimeoutDuringTheWrites()
{
    // --timeout while A writes (its WL hangs) and B waits for the one slot: B is reset and skipped,
    // A finishes its write and its reset, exit 5.
    FleetCliRig fleet;
    fleet.bus.hangWrites(benchSerial(0));
    QTimer driver;
    driver.setInterval(10);
    bool released = false;
    QObject::connect(&driver, &QTimer::timeout, &fleet.rig.cli, [&]() {
        if (!released && fleet.bus.resetsTo(benchSerial(1)).size() == 1) {   // the drain reached B
            released = true;
            fleet.bus.releaseWrites();
        }
    });
    driver.start();
    const CliResult result = fleet.run({QStringLiteral("flash-soc"), QStringLiteral("--serials"), QStringLiteral("%1,%2").arg(benchSerial(0), benchSerial(1)),
                                        QStringLiteral("--partitions"), QStringLiteral("rootfs"), QStringLiteral("--image"), withEnvImage(),
                                        QStringLiteral("--max-parallel"), QStringLiteral("1"), QStringLiteral("--timeout"), QStringLiteral("2")});
    driver.stop();
    QVERIFY2(result.code == 5, qPrintable(result.dump()));
    QVERIFY(released);
    QVERIFY(result.hasText(QStringLiteral("Timeout: 2 s passed (--timeout); cancelling")));
    QVERIFY(result.hasText(QStringLiteral("Cancelling Flash fleet")));
    QVERIFY(result.hasText(QStringLiteral("cancel: the boards not started are reset and skipped; 1 write(s) in progress finish first (every row and "
                                          "the reset)")));
    QVERIFY(result.hasTextStarting(QStringLiteral("[A %1] rootfs written: ").arg(benchSerial(0))));
    QVERIFY(result.hasTextStarting(QStringLiteral("[A %1] done in ").arg(benchSerial(0))));
    QCOMPARE(fleet.bus.writesTo(benchSerial(0)).size(), qsizetype(1));
    QCOMPARE(fleet.bus.resetsTo(benchSerial(0)).size(), qsizetype(1));
    QVERIFY(fleet.bus.writesTo(benchSerial(1)).isEmpty());
    QVERIFY(result.hasText(QStringLiteral("[B %1] cancelled before its write started; reset back to ADB; skipped").arg(benchSerial(1))));
    QVERIFY2(result.lastText().startsWith(QStringLiteral("su-fw: exit 5 (timeout): timeout after 2 s: 1 of 2 boards flashed in ")),
             qPrintable(result.lastText()));
    QVERIFY(result.lastText().endsWith(QStringLiteral("; 1 skipped")));
    QCOMPARE(fleet.bus.cancelledWrites(), 0);
    QCOMPARE(fleet.bus.unaddressedCommands(), 0);
}

void Tst_firmwarecli::fleetNoWait()
{
    // --no-wait goes to the job's SocFlasher, which hands it to the fleet: done right after the RD.
    FleetCliRig fleet;
    const CliResult result = fleet.run({QStringLiteral("flash-soc"), QStringLiteral("--serials"), QStringLiteral("F=c4130950110960e0,G=5a33c81a6e996266"),
                                        QStringLiteral("--partitions"), QStringLiteral("rootfs"), QStringLiteral("--image"), withEnvImage(),
                                        QStringLiteral("--no-wait"), QStringLiteral("--json")});
    QVERIFY2(result.code == 0, qPrintable(result.dump()));
    QCOMPARE(fleet.rig.runner.count(QRegularExpression(QStringLiteral("get-state|rfmcu status|cat /proc/version; uptime"))), 0);
    QCOMPARE(fleet.rig.runner.count(QRegularExpression(QStringLiteral("^upgrade_tool -s \\d+ RD$"))), 2);
    bool allParse = false;
    const QList<QJsonObject> objects = jsonLines(result, &allParse);
    QVERIFY2(allParse, qPrintable(result.out));
    const QJsonObject done = objects.last();
    QCOMPARE(done.value(QStringLiteral("exit")).toInt(), 0);
    const QJsonArray boards = done.value(QStringLiteral("boards")).toArray();
    QCOMPARE(boards.size(), qsizetype(2));
    for (const QJsonValue& value : boards) {
        const QJsonObject board = value.toObject();
        QCOMPARE(board.value(QStringLiteral("result")).toString(), QStringLiteral("done"));
        QCOMPARE(board.value(QStringLiteral("detail")).toString(), QStringLiteral("ADB not waited for (--no-wait)"));
        QCOMPARE(board.value(QStringLiteral("adbBack")).toBool(), false);
        QCOMPARE(board.value(QStringLiteral("written")).toArray(), QJsonArray{QStringLiteral("rootfs")});
    }
}

void Tst_firmwarecli::fleetLayoutOption()
{
    // A plain image (no layout of its own), nothing remembered: --layout is the plan's layout, and
    // the layout gate compares every board with it (applyLayout() without a serial).
    FleetCliRig fleet;
    const QString other = QStringLiteral("sfc_nor:64K(env),256K@64K(idblock),256K(uboot),384K(meta),4M(boot),4M(rootfs),3M(userdata)");
    fleet.bus.setMtdParts(benchSerial(1), other);
    const QString image = makeImage(QStringLiteral("plain/update.img"));
    const CliResult result = fleet.run({QStringLiteral("flash-soc"), QStringLiteral("--serials"), QStringLiteral("%1,%2").arg(benchSerial(0), benchSerial(1)),
                                        QStringLiteral("--partitions"), QStringLiteral("rootfs"), QStringLiteral("--image"), image,
                                        QStringLiteral("--layout"), kNodeMtdParts});
    QVERIFY2(result.code == 1, qPrintable(result.dump()));   // B skipped
    QVERIFY(result.err.isEmpty());
    const QStringList writes = fleet.bus.writesTo(benchSerial(0));
    QCOMPARE(writes.size(), qsizetype(1));
    QVERIFY2(writes.first().contains(QStringLiteral(" WL 8064 10240 ")), qPrintable(writes.first()));   // placed in the --layout
    QVERIFY(result.hasTextStarting(QStringLiteral("[A %1] done in ").arg(benchSerial(0))));
    QVERIFY2(result.hasText(QStringLiteral("[B %1] flash layout differs from the plan's (board: %2; plan: %3): not rebooted; skipped")
                                .arg(benchSerial(1), other, kNodeMtdParts)),
             qPrintable(result.dump()));
    QCOMPARE(fleet.rig.runner.count(QRegularExpression(QStringLiteral("^adb -s %1 reboot loader$").arg(benchSerial(1)))), 0);
    QVERIFY(fleet.bus.writesTo(benchSerial(1)).isEmpty());
    QVERIFY2(result.lastText().startsWith(QStringLiteral("su-fw: exit 1 (failed): 1 of 2 boards flashed in ")), qPrintable(result.lastText()));
    QVERIFY(result.lastText().endsWith(QStringLiteral("; 1 skipped")));
}

void Tst_firmwarecli::fleetMappingStopResetsAndReports()
{
    FleetCliRig fleet;
    fleet.bus.setReboot(benchSerial(2), FakeUsbBus::Reboot::DragsNext);
    const CliResult result = fleet.run({QStringLiteral("flash-soc"), QStringLiteral("--all-boards"), QStringLiteral("--partitions"), QStringLiteral("rootfs"),
                                        QStringLiteral("--image"), withEnvImage()});
    QVERIFY2(result.code == 1, qPrintable(result.dump()));
    QCOMPARE(fleet.rig.runner.count(QRegularExpression(QStringLiteral(" WL "))), 0);
    QCOMPARE(fleet.bus.resetsTo(benchSerial(0)).size(), qsizetype(1));
    QCOMPARE(fleet.bus.resetsTo(benchSerial(1)).size(), qsizetype(1));
    QVERIFY(result.hasTextStarting(QStringLiteral("[A %1] $ upgrade_tool -s ").arg(benchSerial(0))));
    bool hint = false;
    int stoppedRows = 0;
    for (const QString& text : result.texts()) {
        hint = hint || (text.startsWith(QStringLiteral("LocationID ")) && text.contains(QStringLiteral(" is in Loader mode and belongs to no mapped board: reset it with `upgrade_tool -s ")));
        stoppedRows += text.contains(QStringLiteral("  skipped  ")) && text.contains(QStringLiteral("mapping stopped: while rebooting ")) ? 1 : 0;
    }
    QVERIFY2(hint, qPrintable(result.dump()));
    QCOMPARE(stoppedRows, 6);   // A, B reset; D..G never rebooted
    QVERIFY(result.lastText().startsWith(QStringLiteral("su-fw: exit 1 (failed): mapping stopped: while rebooting %1: two boards changed at once").arg(benchSerial(2))));
}

void Tst_firmwarecli::fleetForeignLoaderRefused()
{
    FleetCliRig fleet;
    fleet.bus.addForeignLoader(QStringLiteral("9&FFFF0000&0&1"));
    const QString image = withEnvImage();
    CliResult result = fleet.run({QStringLiteral("flash-soc"), QStringLiteral("--all-boards"), QStringLiteral("--partitions"), QStringLiteral("rootfs"),
                                  QStringLiteral("--image"), image});
    QVERIFY2(result.code == 1, qPrintable(result.dump()));
    QCOMPARE(result.lastText(), QStringLiteral("su-fw: exit 1 (failed): a rockusb device is already in Loader mode on port 9&FFFF0000&0&1 (LocationID 100) "
                                               "that no selected board accounts for; reset it with `upgrade_tool -s 100 RD` or replug it, then run again"));
    QCOMPARE(fleet.rig.runner.count(QRegularExpression(QStringLiteral("reboot loader"))), 0);
    result = fleet.run({QStringLiteral("flash-soc"), QStringLiteral("--all-boards"), QStringLiteral("--partitions"), QStringLiteral("rootfs"),
                        QStringLiteral("--image"), image, QStringLiteral("--dry-run")});
    QVERIFY2(result.code == 0, qPrintable(result.dump()));
    QVERIFY(result.hasText(QStringLiteral("foreign Loader device on port 9&FFFF0000&0&1, LocationID 100: not part of the fleet; reset it with "
                                          "`upgrade_tool -s 100 RD` or replug it")));
}

void Tst_firmwarecli::fleetBootloaderPartitionWarns()
{
    FleetCliRig fleet;
    const CliResult result = fleet.run({QStringLiteral("flash-soc"), QStringLiteral("--serials"), benchSerial(0), QStringLiteral("--partitions"),
                                        QStringLiteral("idblock,rootfs"), QStringLiteral("--image"), withEnvImage()});
    QVERIFY2(result.code == 0, qPrintable(result.dump()));
    QVERIFY(result.hasText(QStringLiteral("Warning: idblock is a bootloader partition; written because --partitions names it")));
    const QStringList writes = fleet.bus.writesTo(benchSerial(0));
    QCOMPARE(writes.size(), qsizetype(2));
    QVERIFY2(writes.first().contains(QStringLiteral(" WL 128 512 ")), qPrintable(writes.first()));   // named: written
}

void Tst_firmwarecli::fleetLockHeldExits1()
{
    FleetCliRig fleet;
    QLockFile other(AppSettings::dataDirectory() + QStringLiteral("/firmware.lock"));
    QVERIFY(other.tryLock(0));
    qint64 pid = 0;
    QString host;
    QString program;
    QVERIFY(other.getLockInfo(&pid, &host, &program));
    const QString running = QStringLiteral("another firmware operation is running (%1)")
                                .arg(program.isEmpty() ? QStringLiteral("pid %1").arg(pid) : QStringLiteral("%1, pid %2").arg(program).arg(pid));
    const CliResult result = fleet.run({QStringLiteral("flash-soc"), QStringLiteral("--all-boards"), QStringLiteral("--partitions"), QStringLiteral("rootfs"),
                                        QStringLiteral("--image"), withEnvImage()});
    QVERIFY2(result.code == 1, qPrintable(result.dump()));
    QVERIFY(result.hasText(QStringLiteral("Flash fleet failed: ") + running));
    QCOMPARE(result.lastText(), QStringLiteral("su-fw: exit 1 (failed): ") + running);
    QCOMPARE(fleet.rig.runner.count(boardTools()), 0);
    other.unlock();
}

void Tst_firmwarecli::fleetLayoutUnknownExits1()
{
    FleetCliRig fleet;
    const QString image = makeImage(QStringLiteral("plain/update.img"));   // its env.img carries no layout; none remembered
    const CliResult result = fleet.run({QStringLiteral("flash-soc"), QStringLiteral("--all-boards"), QStringLiteral("--partitions"), QStringLiteral("rootfs"),
                                        QStringLiteral("--image"), image});
    QVERIFY2(result.code == 1, qPrintable(result.dump()));
    QVERIFY(result.err.isEmpty());
    QCOMPARE(result.lastText(), QStringLiteral("su-fw: exit 1 (failed): flash layout unknown: pass --layout, or run su-fw status once with a board running; "
                                               "the Node image carries its own"));
    QCOMPARE(fleet.rig.runner.count(boardTools()), 0);
    // The single board keeps its own advice and exit 2.
    CliRig single;
    scriptSocFlash(single.runner, kLdLoader);
    const CliResult one = single.run({QStringLiteral("flash-soc"), QStringLiteral("--image"), image, QStringLiteral("--partitions"), QStringLiteral("rootfs")});
    QVERIFY2(one.code == 2, qPrintable(one.dump()));
    QVERIFY(one.err.startsWith(QStringLiteral("su-fw: --partitions: rootfs cannot be written: partition layout unknown: connect the board running once")));
}

void Tst_firmwarecli::fleetQuestionOnATerminal()
{
    FleetCliRig fleet;
    const QString image = withEnvImage();
    const QStringList two{QStringLiteral("flash-soc"), QStringLiteral("--serials"), QStringLiteral("F=c4130950110960e0,G=5a33c81a6e996266"),
                          QStringLiteral("--partitions"), QStringLiteral("rootfs"), QStringLiteral("--image"), image};
    fleet.rig.setInput({QStringLiteral("n")}, /*interactive=*/true);
    CliResult result = fleet.run(two);
    QVERIFY2(result.code == 5, qPrintable(result.dump()));
    QCOMPARE(result.err, QStringLiteral("Write rootfs on 2 boards (F, G)? [y/N] "));
    QCOMPARE(result.lastText(), QStringLiteral("su-fw: exit 5 (cancelled): cancelled before anything ran"));
    QCOMPARE(fleet.rig.runner.invocations().size(), qsizetype(1));   // the plan's EXF only
    QVERIFY(exfPattern().match(fleet.rig.runner.invocations().first()).hasMatch());

    fleet.rig.setInput({QStringLiteral("no")}, true);
    result = fleet.run({QStringLiteral("flash-soc"), QStringLiteral("--all-boards"), QStringLiteral("--partitions"), QStringLiteral("rootfs"),
                        QStringLiteral("--image"), image});
    QCOMPARE(result.code, 5);
    QCOMPARE(result.err, QStringLiteral("Write rootfs on every board adb lists (--all-boards)? [y/N] "));
    QCOMPARE(fleet.rig.runner.count(boardTools()), 0);

    fleet.rig.setInput({QStringLiteral("y")}, true);
    result = fleet.run(two);
    QVERIFY2(result.code == 0, qPrintable(result.dump()));
    QCOMPARE(result.err, QStringLiteral("Write rootfs on 2 boards (F, G)? [y/N] "));

    fleet.rig.setInput({QStringLiteral("n")}, true);
    result = fleet.run(two + QStringList{QStringLiteral("--yes")});   // --yes: nothing asked
    QVERIFY2(result.code == 0, qPrintable(result.dump()));
    QVERIFY(result.err.isEmpty());
    fleet.rig.setInput({QStringLiteral("n")}, false);                // a pipe: nothing asked
    result = fleet.run(two);
    QVERIFY2(result.code == 0, qPrintable(result.dump()));
    QVERIFY(result.err.isEmpty());
    fleet.rig.setInput({QStringLiteral("n")}, true);                 // a dry run is never asked about
    result = fleet.run(two + QStringList{QStringLiteral("--dry-run")});
    QVERIFY2(result.code == 0, qPrintable(result.dump()));
    QVERIFY(result.err.isEmpty());
}

void Tst_firmwarecli::fleetLogAndQuiet()
{
    FleetCliRig fleet;
    const QString image = withEnvImage();
    const QString logPath = caseDir(QStringLiteral("logs/fleet.log"));
    QVERIFY(QDir().mkpath(QFileInfo(logPath).absolutePath()));
    const QStringList arguments{QStringLiteral("flash-soc"), QStringLiteral("--serials"), QStringLiteral("F=c4130950110960e0,G=5a33c81a6e996266"),
                                QStringLiteral("--partitions"), QStringLiteral("rootfs"), QStringLiteral("--image"), image, QStringLiteral("--log"), logPath};
    const CliResult result = fleet.run(arguments);
    QVERIFY2(result.code == 0, qPrintable(result.dump()));
    QCOMPARE(readAll(logPath).split(QLatin1Char('\n'), Qt::SkipEmptyParts), result.lines());
    const CliResult quiet = fleet.run(arguments + QStringList{QStringLiteral("--quiet")});
    QCOMPARE(quiet.code, 0);
    QCOMPARE(quiet.lines().size(), qsizetype(1));
    QVERIFY(quiet.lastText().startsWith(QStringLiteral("su-fw: exit 0 (done): 2 of 2 boards flashed in ")));
    QCOMPARE(readAll(logPath).split(QLatin1Char('\n'), Qt::SkipEmptyParts).last(), quiet.lines().first());
}

void Tst_firmwarecli::singleBoardPathUnchanged()
{
    // 0.6.0's command lines, byte for byte: --serial goes the single-board way (no -s on WL / RD).
    const QString s = kBoardSerial;
    {
        CliRig rig;
        scriptSocFlash(rig.runner, kLdLoader);
        const QString image = makeImage(QStringLiteral("flash/update.img"));
        const QString dir = extractDirOf(image);
        const CliResult result = rig.run({QStringLiteral("flash-soc"), QStringLiteral("--image"), image, QStringLiteral("--all"), QStringLiteral("--layout"),
                                          kNodeMtdParts, QStringLiteral("--serial"), s});
        QVERIFY2(result.code == 0, qPrintable(result.dump()));
        QStringList expected{ToolRunner::formatCommandLine(QStringLiteral("upgrade_tool"),
                                                           {QStringLiteral("EXF"), QDir::toNativeSeparators(image), QDir::toNativeSeparators(dir)}),
                             QStringLiteral("upgrade_tool LD"), QStringLiteral("upgrade_tool LD")};
        for (int k = 0; k < kNodePartitionCount; ++k) {
            expected << expectedWl(k, dir);
        }
        expected << QStringLiteral("upgrade_tool RD") << QStringLiteral("adb -s %1 get-state").arg(s)
                 << QStringLiteral("adb -s %1 shell \"cat /proc/version; uptime\"").arg(s);
        QCOMPARE(rig.runner.invocations().mid(0, expected.size()), expected);
        for (const QString& text : result.texts()) {
            QVERIFY2(!text.startsWith(QLatin1Char('[')), qPrintable(text));
        }
    }
    {
        CliRig rig;
        auto inLoader = std::make_shared<bool>(false);
        rig.runner.addHandler(QRegularExpression(QStringLiteral("^upgrade_tool LD$")), [inLoader](FakeToolInvocation* i) {
            i->emitOutput(*inLoader ? kLdLoader : QByteArray("List of rockusb connected(0)\r\n"));
            i->finish(0);
        });
        rig.runner.addScript(QRegularExpression(QStringLiteral("^adb( -s \\S+)? devices$")), kAdbBoard);
        rig.runner.addHandler(QRegularExpression(QStringLiteral("^adb -s %1 reboot loader$").arg(s)), [inLoader](FakeToolInvocation* i) {
            *inLoader = true;
            i->finish(0);
        });
        scriptSocFlash(rig.runner, kLdLoader);
        const QString image = tempPath(QStringLiteral("withenv-%1-single/update.img").arg(m_caseNumber));
        QVERIFY(writeFile(image, rkfwBytes()));
        const QString dir = extractDirOf(image);
        const CliResult result = rig.run({QStringLiteral("flash-soc"), QStringLiteral("--image"), image, QStringLiteral("--partitions"), QStringLiteral("boot"),
                                          QStringLiteral("--serial"), s, QStringLiteral("--yes"), QStringLiteral("--json")});
        QVERIFY2(result.code == 0, qPrintable(result.dump()));
        const QStringList expected{
            ToolRunner::formatCommandLine(QStringLiteral("upgrade_tool"), {QStringLiteral("EXF"), QDir::toNativeSeparators(image), QDir::toNativeSeparators(dir)}),
            QStringLiteral("upgrade_tool LD"),
            QStringLiteral("adb -s %1 devices").arg(s),
            QStringLiteral("adb -s %1 shell \"cat /proc/version\"").arg(s),
            QStringLiteral("adb -s %1 shell \"cat /proc/cmdline\"").arg(s),
            QStringLiteral("adb -s %1 reboot loader").arg(s),
            QStringLiteral("upgrade_tool LD"),
            expectedWl(4, dir),
            QStringLiteral("upgrade_tool RD"),
            QStringLiteral("adb -s %1 get-state").arg(s),
            QStringLiteral("adb -s %1 shell \"cat /proc/version; uptime\"").arg(s),
        };
        QCOMPARE(rig.runner.invocations().mid(0, expected.size()), expected);
        bool allParse = false;
        const QList<QJsonObject> objects = jsonLines(result, &allParse);
        QVERIFY(allParse);
        QVERIFY(!objects.last().contains(QStringLiteral("fleet")));
        for (const QJsonObject& object : objects) {
            QVERIFY(!object.contains(QStringLiteral("board")) || object.value(QStringLiteral("event")).toString() == QLatin1String("done"));
        }
    }
}

void Tst_firmwarecli::fleetHelp()
{
    const QString help = FirmwareCli::helpText(QStringLiteral("flash-soc"));
    for (const char* option : {"serials", "all-boards", "max-parallel", "dry-run"}) {
        QVERIFY2(FirmwareCli::commandOptions(QStringLiteral("flash-soc")).contains(QLatin1String(option)), option);
        QVERIFY2(!FirmwareCli::commandOptions(QStringLiteral("partitions")).contains(QLatin1String(option)), option);
    }
    QVERIFY(help.contains(QStringLiteral("\n  --serials SERIALS ")));
    QVERIFY(help.contains(QStringLiteral("\n  --max-parallel N ")));
    QVERIFY(help.contains(QStringLiteral("\n       su-fw flash-soc (--serials S1,S2,... | --all-boards) --partitions NAMES")));
    const qsizetype examples = help.indexOf(QStringLiteral("Examples:"));
    const qsizetype dryRun = help.indexOf(QStringLiteral("\n    su-fw flash-soc --all-boards --partitions rootfs --image update.img --dry-run --json"));
    QVERIFY(dryRun > 0 && dryRun < examples);   // in the fleet paragraph, not under Examples
    QVERIFY(help.contains(QStringLiteral("Exit codes of a fleet (--serials / --all-boards):")));
    QVERIFY(help.contains(QStringLiteral("  3  no selected board is on ADB (--all-boards: no board at all)")));
    QVERIFY(help.mid(examples).contains(QStringLiteral("\n  su-fw flash-soc --serials F=")));
    const QString global = FirmwareCli::helpText();
    QVERIFY(global.contains(QStringLiteral("several boards at once with --serials / --all-boards")));
    QVERIFY(global.contains(QStringLiteral("flash-soc with --serials / --all-boards: no selected board is on ADB")));
    QVERIFY(global.contains(QStringLiteral("flash-soc with --serials / --all-boards: the boards not started are skipped and reset")));

#ifdef Q_OS_WIN
    const QString name = QStringLiteral("su-fw.exe");
#else
    const QString name = QStringLiteral("su-fw");
#endif
    QString program = QCoreApplication::applicationDirPath() + QLatin1Char('/') + name;
    if (!QFileInfo(program).isFile()) {
        program = QStringLiteral(SU_FW_EXECUTABLE);
    }
    QProcess process;
    process.start(program, {QStringLiteral("flash-soc"), QStringLiteral("--help")});
    QVERIFY(process.waitForFinished(30000));
    QCOMPARE(process.exitCode(), 0);
    QVERIFY(process.readAllStandardOutput().contains("--serials SERIALS"));
}

// =======================================================================================
// The executable
// =======================================================================================

void Tst_firmwarecli::realExecutable()
{
#ifdef Q_OS_WIN
    const QString name = QStringLiteral("su-fw.exe");
#else
    const QString name = QStringLiteral("su-fw");
#endif
    QString program = QCoreApplication::applicationDirPath() + QLatin1Char('/') + name;   // next to this test
    if (!QFileInfo(program).isFile()) {
        program = QStringLiteral(SU_FW_EXECUTABLE);   // where CMake built it (another folder on some generators)
    }
    QVERIFY2(QFileInfo(program).isFile(), qPrintable(program));
    const auto runIt = [&program](const QStringList& arguments, QByteArray* out, QByteArray* err) {
        QProcess process;
        process.start(program, arguments);
        if (!process.waitForFinished(30000)) {
            process.kill();
            return -1;
        }
        *out = process.readAllStandardOutput();
        *err = process.readAllStandardError();
        return process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -2;
    };
    QByteArray out;
    QByteArray err;
    QCOMPARE(runIt({QStringLiteral("--help")}, &out, &err), 0);
    QVERIFY(out.contains("Usage:"));
    QVERIFY(out.contains("Exit codes:"));
    QVERIFY(out.contains("pull-flash"));
    QVERIFY2(err.isEmpty(), err.constData());
    QCOMPARE(runIt({QStringLiteral("flash-soc"), QStringLiteral("--help")}, &out, &err), 0);
    QVERIFY(out.contains("--partitions NAMES"));
    QCOMPARE(runIt({QStringLiteral("--version")}, &out, &err), 0);
    QVERIFY2(out.startsWith("su-fw " APP_VERSION " (BuildAI Serial Utility, "), out.constData());
    QCOMPARE(runIt({QStringLiteral("status"), QStringLiteral("--bogus")}, &out, &err), 2);
    QVERIFY2(out.isEmpty(), out.constData());
    QVERIFY2(err.startsWith("su-fw: Unknown option 'bogus'."), err.constData());
}

// =======================================================================================
// Board profiles (v0.8.0)
// =======================================================================================

namespace {

const QString kCliPowerRefusal =
    QStringLiteral("the CC1310 of a Stacked (RV1106G2) board holds the SoC's power (SOC_PWR_EN): rebooting it into its bootloader can "
                   "switch the SoC off and cut the update; refused until confirmed - --confirm-soc-power (the tab: confirm the question) "
                   "once the carrier's CC1310 bootloader is known to hold SOC_PWR_EN (the 14 Oct 2026 build or later)");

/// The `board.profile` object of a --json status run.
QJsonObject boardProfileOf(const CliResult& result)
{
    bool allParse = false;
    const QList<QJsonObject> objects = jsonLines(result, &allParse);
    if (!allParse || objects.isEmpty()) {
        return QJsonObject();
    }
    return objects.last().value(QStringLiteral("board")).toObject().value(QStringLiteral("profile")).toObject();
}

QJsonObject profileObject(const char* id, const char* name, const char* source, const QString& evidence)
{
    return QJsonObject{{QStringLiteral("id"), QLatin1String(id)},
                       {QStringLiteral("name"), QLatin1String(name)},
                       {QStringLiteral("source"), QLatin1String(source)},
                       {QStringLiteral("evidence"), evidence}};
}

const QString kNodeEvidence = QStringLiteral("7 partitions: env, idblock, uboot, meta, boot, rootfs, userdata");

} // namespace

void Tst_firmwarecli::profileOptionUsage()
{
    CliRig rig;
    const QStringList commands{QStringLiteral("status"),   QStringLiteral("partitions"),   QStringLiteral("flash-soc"),
                               QStringLiteral("pull-flash"), QStringLiteral("mcu-info"),   QStringLiteral("mcu-update"),
                               QStringLiteral("mcu-set-role")};
    for (const QString& command : commands) {
        QVERIFY2(FirmwareCli::commandOptions(command).contains(QStringLiteral("profile")), qPrintable(command));
        const CliResult result = rig.run({command, QStringLiteral("--profile"), QStringLiteral("x")});
        QVERIFY2(result.code == 2, qPrintable(command + QLatin1Char('\n') + result.dump()));
        QVERIFY2(result.err.startsWith(QStringLiteral("su-fw: --profile: unknown profile \"x\"; known: node, stacked")), qPrintable(result.dump()));
        QVERIFY(result.out.isEmpty());
    }
    QVERIFY(!FirmwareCli::commandOptions(QStringLiteral("pull")).contains(QStringLiteral("profile")));
    for (const char* option : {"force-layout", "force-loader-partitions"}) {
        QVERIFY(FirmwareCli::commandOptions(QStringLiteral("flash-soc")).contains(QLatin1String(option)));
        QVERIFY(FirmwareCli::commandOptions(QStringLiteral("pull-flash")).contains(QLatin1String(option)));
        QVERIFY(!FirmwareCli::commandOptions(QStringLiteral("mcu-update")).contains(QLatin1String(option)));
    }
    for (const char* command : {"mcu-update", "mcu-set-role", "pull-flash"}) {
        QVERIFY(FirmwareCli::commandOptions(QLatin1String(command)).contains(QStringLiteral("confirm-soc-power")));
    }
    QVERIFY(!FirmwareCli::commandOptions(QStringLiteral("flash-soc")).contains(QStringLiteral("confirm-soc-power")));
    // A fleet skips a board whose layout differs: --force-layout is the single board's.
    const QString image = makeImage(QStringLiteral("usage/update.img"));
    const CliResult fleet = rig.run({QStringLiteral("flash-soc"), QStringLiteral("--serials"), kBoardSerial, QStringLiteral("--partitions"),
                                     QStringLiteral("rootfs"), QStringLiteral("--image"), image, QStringLiteral("--force-layout")});
    QVERIFY2(fleet.code == 2, qPrintable(fleet.dump()));
    QVERIFY(fleet.err.startsWith(QStringLiteral("su-fw: --force-layout: the single-board option; a fleet skips a board whose layout differs "
                                                "(name env among --partitions to write a new layout)")));
    QVERIFY(rig.runner.invocations().isEmpty());   // nothing ran

    // Accepted: no board answers here, so the 0.7.0 exit codes stand.
    scriptNoBoard(rig.runner);
    QCOMPARE(rig.run({QStringLiteral("status"), QStringLiteral("--profile"), QStringLiteral("node")}).code, 3);
    QCOMPARE(rig.run({QStringLiteral("mcu-info"), QStringLiteral("--profile"), QStringLiteral(" Stacked ")}).code, 3);
}

void Tst_firmwarecli::statusProfileLines()
{
    {
        CliRig rig;   // the Node's layout from its /proc/cmdline
        scriptAdbBoard(rig.runner);
        const CliResult result = rig.run({QStringLiteral("status")});
        QVERIFY2(result.code == 0, qPrintable(result.dump()));
        const QStringList texts = result.texts();
        const qsizetype layout = texts.indexOf(QStringLiteral("Flash layout of board %1: %2").arg(kBoardSerial, kNodeMtdParts));
        QVERIFY2(layout >= 0, qPrintable(result.dump()));
        // The layout read may end before or after the Board line: the profile line follows both.
        const qsizetype board = texts.indexOf(QStringLiteral("Board: ADB device ") + kBoardSerial);
        QVERIFY2(board >= 0, qPrintable(result.dump()));
        const qsizetype profile = texts.indexOf(QStringLiteral("profile: Node (RV1106BG2) - from the board's flash layout (") + kNodeEvidence
                                                + QLatin1Char(')'));
        QVERIFY2(profile == std::max(layout, board) + 1, qPrintable(result.dump()));
        QCOMPARE(result.lastText(), QStringLiteral("su-fw: exit 0 (done): ADB device ") + kBoardSerial);   // unchanged
        const CliResult json = rig.run({QStringLiteral("status"), QStringLiteral("--json")});
        QCOMPARE(boardProfileOf(json), profileObject("node", "Node (RV1106BG2)", "boardLayout", kNodeEvidence));
    }
    QSettings().clear();
    {
        CliRig rig;   // a board in Loader mode: its PID, right after the Board line
        rig.runner.addScript(QRegularExpression(QStringLiteral("^upgrade_tool LD$")), kLdLoaderStacked);
        const CliResult result = rig.run({QStringLiteral("status")});
        QVERIFY2(result.code == 0, qPrintable(result.dump()));
        const QStringList texts = result.texts();
        const qsizetype board = texts.indexOf(QRegularExpression(QStringLiteral("^Board: Loader mode .*")));
        QVERIFY(board >= 0);
        QCOMPARE(texts.at(board + 1), QStringLiteral("profile: Stacked (RV1106G2) - from the board's Loader PID 0x110c"));
        QCOMPARE(boardProfileOf(rig.run({QStringLiteral("status"), QStringLiteral("--json")})),
                 profileObject("stacked", "Stacked (RV1106G2)", "loaderPid", QStringLiteral("0x110c")));
    }
    {
        CliRig rig;   // MASKROM: its PID is the BootROM's - no profile from it
        rig.runner.addScript(QRegularExpression(QStringLiteral("^upgrade_tool LD$")), kLdMaskrom);
        const CliResult result = rig.run({QStringLiteral("status")});
        QVERIFY2(result.code == 0, qPrintable(result.dump()));
        QVERIFY(result.hasText(QStringLiteral("profile: unknown (no --profile, no Loader PID, product string or flash layout matched a profile)")));
        QCOMPARE(boardProfileOf(rig.run({QStringLiteral("status"), QStringLiteral("--json")})), profileObject("", "", "", QString()));
    }
    for (const bool answers : {false, true}) {
        QSettings().clear();
        CliRig rig;   // no mtdparts: the product read, logged, then the line it makes
        rig.runner.addScript(QRegularExpression(QStringLiteral("^upgrade_tool LD$")), "List of rockusb connected(0)\r\n");
        rig.runner.addScript(QRegularExpression(QStringLiteral("^adb( -s \\S+)? devices$")), kAdbBoard);
        rig.runner.addScript(QRegularExpression(QStringLiteral("cat /proc/cmdline")), "console=ttyFIQ0 root=/dev/rd0 rootwait\n");
        if (answers) {
            rig.runner.addScript(productPattern(), kProductStacked);
        }
        const CliResult result = rig.run({QStringLiteral("status"), QStringLiteral("--json")});
        QVERIFY2(result.code == 0, qPrintable(result.dump()));
        QVERIFY(result.out.contains(QStringLiteral("cat /sys/kernel/config/usb_gadget/rockchip/strings/0x409/product")));
        QCOMPARE(rig.runner.count(productPattern()), 1);
        const QString line = answers ? QStringLiteral("profile: Stacked (RV1106G2) - from the ADB gadget's product string "
                                                      "rv1106g-stacked-gc2083")
                                     : QStringLiteral("profile: unknown (no --profile, no Loader PID, product string or flash layout "
                                                      "matched a profile)");
        QVERIFY2(result.out.contains(QStringLiteral("\"text\":\"") + line + QLatin1Char('"')), qPrintable(result.dump()));
        QCOMPARE(boardProfileOf(result), answers ? profileObject("stacked", "Stacked (RV1106G2)", "adbProduct", QStringLiteral("rv1106g-stacked-gc2083"))
                                                 : profileObject("", "", "", QString()));
    }
}

void Tst_firmwarecli::partitionsPrintsImageHeaderAndProfile()
{
    {
        CliRig rig;
        rig.runner.addHandler(exfPattern(), &nodeExtractor);
        const QString image = withEnvImage();
        const CliResult result = rig.run({QStringLiteral("partitions"), QStringLiteral("--image"), image});
        QVERIFY2(result.code == 0, qPrintable(result.dump()));
        const QStringList texts = result.texts();
        const qsizetype using_ = texts.indexOf(QStringLiteral("Using the local image ") + QDir::toNativeSeparators(image));
        QVERIFY(using_ >= 0);
        QCOMPARE(texts.at(using_ + 1), QStringLiteral("image: update.img, chip tag 110E, built 2026-10-13 21:45:50"));
        QCOMPARE(texts.at(using_ + 2), QStringLiteral("image profile: Node (RV1106BG2) - from the image's chip tag 110E"));
        const CliResult json = rig.run({QStringLiteral("partitions"), QStringLiteral("--image"), image, QStringLiteral("--json")});
        bool allParse = false;
        const QJsonObject done = jsonLines(json, &allParse).last();
        QVERIFY(allParse);
        QCOMPARE(done.value(QStringLiteral("imageHeader")).toObject(),
                 (QJsonObject{{QStringLiteral("chipTag"), QStringLiteral("110E")},
                              {QStringLiteral("version"), QStringLiteral("0.0.00")},
                              {QStringLiteral("buildTime"), QStringLiteral("2026-10-13T21:45:50")}}));
        QCOMPARE(done.value(QStringLiteral("imageProfile")).toObject(),
                 profileObject("node", "Node (RV1106BG2)", "chipTag", QStringLiteral("110E")));
    }
    {
        // The stacked fixture: six rows at the stacked sectors (boot 0xF0000 / 12 MiB, userdata 0xCF0000).
        CliRig rig;
        rig.runner.addHandler(exfPattern(), &stackedExtractor);
        const QString image = tempPath(QStringLiteral("stacked-withenv-%1/update.img").arg(m_caseNumber));
        QVERIFY(writeFile(image, rkfwBytes(4096, QStringLiteral("1106"))));
        const CliResult result = rig.run({QStringLiteral("partitions"), QStringLiteral("--image"), image, QStringLiteral("--json")});
        QVERIFY2(result.code == 0, qPrintable(result.dump()));
        bool allParse = false;
        const QJsonObject done = jsonLines(result, &allParse).last();
        QVERIFY(allParse);
        QCOMPARE(done.value(QStringLiteral("layout")).toString(), kStackedMtdParts);
        QCOMPARE(done.value(QStringLiteral("imageProfile")).toObject().value(QStringLiteral("id")).toString(), QStringLiteral("stacked"));
        const QJsonArray rows = done.value(QStringLiteral("partitions")).toArray();
        QCOMPARE(rows.size(), qsizetype(kStackedPartitionCount));
        for (int k = 0; k < kStackedPartitionCount; ++k) {
            const QJsonObject row = rows.at(k).toObject();
            QCOMPARE(row.value(QStringLiteral("name")).toString(), QLatin1String(kStackedNames[k]));
            QCOMPARE(row.value(QStringLiteral("offset")).toInteger(), qint64(kStackedSectors[k]) * 512);
            QCOMPARE(row.value(QStringLiteral("size")).toInteger(), qint64(kStackedSizeSectors[k]) * 512);
            QVERIFY(row.value(QStringLiteral("writable")).toBool());
        }
    }
}

void Tst_firmwarecli::flashSocRefusesWrongImage()
{
    CliRig rig;
    scriptSocFlash(rig.runner, kLdLoaderStacked);
    const QString image = makeImage(QStringLiteral("node/update.img"));   // chip tag 110E
    const CliResult result = rig.run({QStringLiteral("flash-soc"), QStringLiteral("--image"), image, QStringLiteral("--partitions"),
                                      QStringLiteral("boot"), QStringLiteral("--layout"), kStackedMtdParts});
    QVERIFY2(result.code == 1, qPrintable(result.dump()));
    const QString refusal = QStringLiteral("the image is for another board: its chip tag 110E is the Node (RV1106BG2)'s, the board is Stacked "
                                           "(RV1106G2) (from the board's Loader PID 0x110c); nothing was written");
    QCOMPARE(result.lastText(), QStringLiteral("su-fw: exit 1 (failed): ") + refusal);
    QVERIFY(result.hasText(QStringLiteral("profile: Stacked (RV1106G2) - from the board's Loader PID 0x110c")));
    QVERIFY(wlCommands(rig.runner).isEmpty());
}

void Tst_firmwarecli::flashSocLayoutMismatchNeedsForce()
{
    const QString older = QStringLiteral("sfc_nor:64K(env),256K@64K(idblock),256K(uboot),384K(meta),4M(boot),5M(rootfs),2M(userdata)");
    CliRig rig;
    scriptSocFlash(rig.runner, kLdLoader);
    const QString image = withEnvImage();
    const QStringList arguments{QStringLiteral("flash-soc"), QStringLiteral("--image"), image, QStringLiteral("--all"),
                                QStringLiteral("--layout"), older};
    CliResult result = rig.run(arguments);
    QVERIFY2(result.code == 1, qPrintable(result.dump()));
    QVERIFY2(result.lastText().startsWith(QStringLiteral("su-fw: exit 1 (failed): the image's flash layout differs from the board's (image: %1; "
                                                         "board: %2): refused")
                                              .arg(kNodeMtdParts, older)),
             qPrintable(result.lastText()));
    QVERIFY(wlCommands(rig.runner).isEmpty());
    result = rig.run(arguments + QStringList{QStringLiteral("--force-layout")});
    QVERIFY2(result.code == 0, qPrintable(result.dump()));
    QVERIFY(result.hasText(QStringLiteral("warning: the image's flash layout differs from the board's; the image's is used (--force-layout). "
                                          "image: %1; board: %2")
                               .arg(kNodeMtdParts, older)));
    QStringList expected;
    for (int k = 0; k < kNodePartitionCount; ++k) {
        expected << expectedWl(k, extractDirOf(image));   // the image's sectors
    }
    QCOMPARE(wlCommands(rig.runner), expected);
}

void Tst_firmwarecli::flashSocLoaderPartitionsNeedForce()
{
    CliRig rig;
    scriptSocFlash(rig.runner, kLdLoaderStacked);
    const QString image = tempPath(QStringLiteral("stacked-withenv-%1/update.img").arg(m_caseNumber));
    QVERIFY(writeFile(image, rkfwBytes(4096, QStringLiteral("1106"))));
    const QString dir = extractDirOf(image);
    const QStringList two{QStringLiteral("flash-soc"), QStringLiteral("--image"), image, QStringLiteral("--partitions"), QStringLiteral("idblock,boot")};
    CliResult result = rig.run(two);
    QVERIFY2(result.code == 1, qPrintable(result.dump()));
    QVERIFY2(result.lastText().startsWith(QStringLiteral("su-fw: exit 1 (failed): idblock: loader partition(s) on a Stacked (RV1106G2) "
                                                         "board, whose MASKROM entry is not documented")),
             qPrintable(result.lastText()));
    QVERIFY(wlCommands(rig.runner).isEmpty());
    result = rig.run(two + QStringList{QStringLiteral("--force-loader-partitions")});
    QVERIFY2(result.code == 0, qPrintable(result.dump()));
    QVERIFY(result.hasText(QStringLiteral("warning: writing loader partition(s) idblock on a Stacked (RV1106G2) board without a documented "
                                          "MASKROM entry (--force-loader-partitions)")));
    QCOMPARE(wlCommands(rig.runner), (QStringList{expectedStackedWl(1, dir), expectedStackedWl(4, dir)}));
    rig.runner.reset();
    scriptSocFlash(rig.runner, kLdLoaderStacked);
    result = rig.run({QStringLiteral("flash-soc"), QStringLiteral("--image"), image, QStringLiteral("--partitions"), QStringLiteral("boot")});
    QVERIFY2(result.code == 0, qPrintable(result.dump()));
    QVERIFY(!result.out.contains(QStringLiteral("loader partition(s)")));
    QCOMPARE(wlCommands(rig.runner), QStringList{expectedStackedWl(4, dir)});
}

void Tst_firmwarecli::mcuUpdateNeedsConfirmOnStacked()
{
    FakeMcu mcu;
    FakeMcuServer bridge(&mcu);
    QVERIFY(bridge.listen());
    const QRegularExpression forward(QStringLiteral("forward tcp:0"));
    const QStringList update{QStringLiteral("mcu-update"), QStringLiteral("--package"), dataFile("firmware-0.8.1.pkg"), QStringLiteral("--role"),
                             QStringLiteral("rx")};
    const QStringList setRole{QStringLiteral("mcu-set-role"), QStringLiteral("--role"), QStringLiteral("rx")};
    const auto board = [&bridge](CliRig& rig, const QByteArray& cmdline, bool product) {
        rig.runner.addScript(QRegularExpression(QStringLiteral("^upgrade_tool LD$")), "List of rockusb connected(0)\r\n");
        rig.runner.addScript(QRegularExpression(QStringLiteral("cat /proc/cmdline")), cmdline);
        if (product) {
            rig.runner.addScript(productPattern(), kProductStacked);
        }
        scriptMcuBridge(rig.runner, bridge.port());
    };
    for (const QStringList& command : {update, setRole}) {
        QSettings().clear();
        CliRig rig;   // the stacked layout in /proc/cmdline
        board(rig, kStackedCmdline, false);
        CliResult result = rig.run(command);
        QVERIFY2(result.code == 1, qPrintable(result.dump()));
        QCOMPARE(result.lastText(), QStringLiteral("su-fw: exit 1 (failed): ") + kCliPowerRefusal);
        QCOMPARE(rig.runner.count(forward), 0);
        result = rig.run(command + QStringList{QStringLiteral("--confirm-soc-power")});
        QVERIFY2(result.code == 0, qPrintable(result.dump()));
        QVERIFY(result.hasText(QStringLiteral("warning: the CC1310 of a Stacked (RV1106G2) board holds the SoC's power; proceeding as confirmed "
                                              "(--confirm-soc-power)")));
        QCOMPARE(rig.runner.count(forward), 1);
    }
    {
        QSettings().clear();
        CliRig rig;   // no mtdparts, but the board answers its product string
        board(rig, "console=ttyFIQ0 root=/dev/rd0 rootwait\n", true);
        const CliResult result = rig.run(update);
        QVERIFY2(result.code == 1, qPrintable(result.dump()));
        QVERIFY(result.hasText(QStringLiteral("profile: Stacked (RV1106G2) - from the ADB gadget's product string rv1106g-stacked-gc2083")));
        QCOMPARE(result.lastText(), QStringLiteral("su-fw: exit 1 (failed): ") + kCliPowerRefusal);
    }
    {
        QSettings().clear();
        CliRig rig;   // neither tells: unknown, the 0.7.0 run
        board(rig, "console=ttyFIQ0 root=/dev/rd0 rootwait\n", false);
        const CliResult result = rig.run(update);
        QVERIFY2(result.code == 0, qPrintable(result.dump()));
        QCOMPARE(rig.runner.count(forward), 1);
    }
    {
        // pull-flash: a stacked image, the board told by --profile; the MCU step stops with the text.
        QSettings().clear();
        ChainRig chain;
        chain.machine.image = rkfwBytes(256 * 1024, QStringLiteral("1106"));
        *chain.versionReply = "OK version=0.8.0 role=rx\n";
        QVERIFY(chain.start(caseDir(QStringLiteral("work"))));
        const CliResult result =
            chain.rig.run(chain.arguments(caseDir(QStringLiteral("cache"))) + QStringList{QStringLiteral("--profile"), QStringLiteral("stacked")});
        QVERIFY2(result.code == 1, qPrintable(result.dump()));
        QCOMPARE(result.lastText(), QStringLiteral("su-fw: exit 1 (failed): stopped at Update MCU: ") + kCliPowerRefusal);
        QCOMPARE(chain.rig.runner.count(QRegularExpression(QStringLiteral("^upgrade_tool UF "))), 1);
        QCOMPARE(chain.rig.runner.count(forward), 0);
    }
}

void Tst_firmwarecli::fleetMixedBenchStopsThroughTheCli()
{
    FleetCliRig fleet;
    fleet.bus.setMtdParts(benchSerial(2), kStackedMtdParts);
    const QString image = withEnvImage();   // chip tag 110E: the plan is the Node's
    const QStringList arguments{QStringLiteral("flash-soc"), QStringLiteral("--serials"),
                                benchSerial(0) + QLatin1Char(',') + benchSerial(1) + QLatin1Char(',') + benchSerial(2),
                                QStringLiteral("--partitions"), QStringLiteral("rootfs"), QStringLiteral("--image"), image};
    const QRegularExpression addressedWl(QStringLiteral("^upgrade_tool -s \\S+ WL "));
    CliResult result = fleet.run(arguments);
    QVERIFY2(result.code == 1, qPrintable(result.dump()));
    QVERIFY(result.hasText(QStringLiteral("[C %1] profile: Stacked (RV1106G2) - from the board's flash layout (6 partitions: env, idblock, "
                                          "uboot, meta, boot, userdata)")
                               .arg(benchSerial(2))));
    QVERIFY(result.hasText(QStringLiteral("plan profile: Node (RV1106BG2) - from the image's chip tag 110E")));
    QVERIFY2(result.lastText().startsWith(QStringLiteral("su-fw: exit 1 (failed): mapping stopped: mixed bench: board %1 is Stacked "
                                                         "(RV1106G2)")
                                              .arg(benchSerial(2))),
             qPrintable(result.lastText()));
    QCOMPARE(fleet.rig.runner.count(addressedWl), 0);
    // The stop reset A and B: back on ADB before the next run.
    QTRY_VERIFY(fleet.bus.board(benchSerial(0)).state == FakeUsbBus::State::Adb && fleet.bus.board(benchSerial(1)).state == FakeUsbBus::State::Adb);
    // --profile node: every board is the plan's; the layout gate skips C instead (exit 1 for the skip).
    result = fleet.run(arguments + QStringList{QStringLiteral("--profile"), QStringLiteral("node")});
    QVERIFY2(result.code == 1, qPrintable(result.dump()));
    QVERIFY(!result.lastText().contains(QStringLiteral("mixed bench")));
    QVERIFY(result.out.contains(QStringLiteral("flash layout differs from the plan's")));
    QCOMPARE(fleet.rig.runner.count(addressedWl), 2);   // A and B written
}

void Tst_firmwarecli::helpMentionsProfiles()
{
    const QString help = FirmwareCli::helpText(QStringLiteral("flash-soc"));
    QVERIFY(help.contains(QStringLiteral("\nBoard profiles: the tool tells the Node (RV1106BG2: chip tag 110E, Loader PID 0x110e, 7\n")));
    QVERIFY(help.contains(QStringLiteral("\n    su-fw flash-soc --image update.img --partitions rootfs            (Node)\n")));
    QVERIFY(help.contains(QStringLiteral("\n    su-fw flash-soc --image update.img --partitions boot --profile stacked   (Stacked core)\n")));
    QCOMPARE(help.count(QStringLiteral("\n  su-fw flash-soc")), qsizetype(2));   // the examples alone
    const qsizetype examples = help.indexOf(QStringLiteral("Examples:"));
    QVERIFY(help.indexOf(QStringLiteral("Board profiles:")) < examples);
    QCOMPARE(help.mid(examples).count(QStringLiteral("\n  su-fw flash-soc")), qsizetype(2));
    for (const char* option : {"\n  --profile ID ", "\n  --force-layout ", "\n  --force-loader-partitions\n"}) {
        QVERIFY2(help.contains(QLatin1String(option)), option);
    }
    QVERIFY(help.contains(QStringLiteral("a refusal of the board profiles (the wrong image,")));
    for (const char* command : {"mcu-update", "mcu-set-role", "pull-flash"}) {
        const QString page = FirmwareCli::helpText(QLatin1String(command));
        QVERIFY2(page.contains(QStringLiteral("\n  --confirm-soc-power  The CC1310 of the Stacked core holds the SoC's power (SOC_PWR_EN)")), command);
        QVERIFY2(page.contains(QStringLiteral("without --confirm-soc-power")), command);   // its exit code 1 says so
    }
    for (const char* command : {"status", "partitions", "mcu-info"}) {
        QVERIFY2(FirmwareCli::helpText(QLatin1String(command)).contains(QStringLiteral("\n  --profile ID ")), command);
    }
    QVERIFY(FirmwareCli::helpText().startsWith(
        QStringLiteral("su-fw - flash the BuildAI Node and Stacked boards (RV1106 SoC + CC1310 RF MCU) from a command shell\n")));
}

QTEST_GUILESS_MAIN(Tst_firmwarecli)
#include "tst_firmwarecli.moc"
