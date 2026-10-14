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
#include <QPointer>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTimer>

#include <memory>
#include <optional>

#include "Version.h"
#include "app/AppSettings.h"
#include "firmware/CcPackage.h"
#include "firmware/CcProtocol.h"
#include "firmware/FirmwareCli.h"
#include "firmware/FirmwareJob.h"
#include "firmware/McuLink.h"
#include "firmware/SocFlasher.h"
#include "firmware/ToolRunner.h"
#include "ssh/SshProfile.h"
#include "support/FakeMcu.h"
#include "support/FakeToolRunner.h"
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

    // ---- status ----------------------------------------------------------------------------------
    void statusAdbBoardReadsTheLayout();
    void statusLoaderBoard();
    void statusNoBoardExits3();
    void statusOtherSerialExits3();
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
    void flashNoWait();
    void flashJsonLines();
    void flashLogFileHoldsTheSameLines();

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
    QCOMPARE(QStringLiteral(APP_VERSION), QStringLiteral("0.6.0"));
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
    QCOMPARE(result.lastText(), QStringLiteral("su-fw: exit 5 (cancelled): interrupted: cancelled"));
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
        rig.runner.addHandler(QRegularExpression(QStringLiteral("rfmcu cmd version")), [reply](FakeToolInvocation* i) {
            i->emitOutput(*reply);
            i->finish(0);
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

QTEST_GUILESS_MAIN(Tst_firmwarecli)
#include "tst_firmwarecli.moc"
