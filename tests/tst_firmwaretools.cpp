// Unit tests for the Firmware tab's tool layer (v0.5): ToolInvocation / ProcessToolRunner /
// ToolLocator, the upgrade_tool and adb parsers, UsbProbe, and the McuLink and SocFlasher state
// machines driven by FakeToolRunner (tests/support). UI-free. No real adb / upgrade_tool is ever
// started: ProcessToolRunner is exercised with cmd.exe / sh and with this executable itself, which
// doubles as a tiny helper program ("--tool-helper <mode>", see main() at the bottom).
#include <QtTest>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPair>
#include <QPointer>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>

#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#endif

#include "firmware/AdbTool.h"
#include "firmware/McuLink.h"
#include "firmware/McuUpdater.h"
#include "firmware/RockchipTool.h"
#include "firmware/SocFlasher.h"
#include "firmware/ToolRunner.h"
#include "firmware/UsbProbe.h"
#include "support/FakeToolRunner.h"

// Defined in UsbProbe.cpp, not part of the header: the sysfs scanner takes its root as a
// parameter so a fake tree can stand in for /sys/bus/usb/devices.
namespace UsbProbe {
QList<UsbDeviceInfo> scanSysfs(const QString& sysfsRoot, quint16 vid);
}

namespace {

// ---- sample tool outputs (see docs/firmware-update/PLAN.md "Facts") ---------------------------

const QByteArray kBanner = "rockusb driver = 5.12,please upgrade it to enjoy fast download\r\n\r\n";
const QByteArray kLdEmpty = kBanner + "List of rockusb connected(0)\r\n";
const QByteArray kLdLoader = kBanner + "List of rockusb connected(1)\r\n"
                             "DevNo=1\tVid=0x2207,Pid=0x110e,LocationID=104\tMode=Loader\tSerialNo=\r\n";
const QByteArray kLdMaskrom = kBanner + "List of rockusb connected(1)\r\n"
                              "DevNo=1\tVid=0x2207,Pid=0x110c,LocationID=104\tMode=Maskrom\tSerialNo=\r\n";
const QByteArray kAdbNone = "List of devices attached\r\n\r\n";
const QByteArray kAdbBoard = "List of devices attached\r\n0225ce3c8d8d6e44\tdevice\r\n\r\n";
const QByteArray kProcVersion =
    "Linux version 5.10.209+ (builder@rk) (arm-rockchip830-linux-uclibcgnueabihf-gcc 8.3.0) #1 SMP Wed Sep 30 2026\r\n"
    " 21:50:12 up 0 min,  load average: 0.45, 0.10, 0.03\r\n";

QString sfiSample(const QString& eol, const QString& sep)
{
    QStringList lines;
    lines << QStringLiteral("rockusb driver = 5.12,please upgrade it to enjoy fast download") << QString()
          << QStringLiteral("File:C:\\Users\\moses\\code\\BuildAI\\SerialUtility\\build\\update.img") << QString()
          << QStringLiteral("Type:Update Firmware") << QString()
          << QStringLiteral("Chip Tag:110E%1Version:0.0.00%1Build Time:2026-10-13 21:45:50%1Sign:false").arg(sep)
          << QString()
          << QStringLiteral("Loader ver:1.01%1Loader Time:2026-10-13 21:43:55").arg(sep) << QString()
          << QStringLiteral("Loader Sign:false") << QString() << QStringLiteral("Entry Count:9") << QString();
    lines << QStringLiteral("    EntryNo=0 ;    file=env.img;    partition=env;    type=image;    offset=0x3d25f;    size=0x10000")
          << QStringLiteral("    EntryNo=1 ;    file=idblock.img;    partition=idblock;    type=image;    offset=0x4d25f;    size=0x40000")
          << QStringLiteral("    EntryNo=2 ;    file=uboot.img;    partition=uboot;    type=image;    offset=0x8d25f;    size=0x100000")
          << QStringLiteral("    EntryNo=3 ;    file=boot.img;    partition=boot;    type=image;    offset=0x18d25f;    size=0x500000")
          << QStringLiteral("    EntryNo=4 ;    file=rootfs.img;    partition=rootfs;    type=image;    offset=0x68d25f;    size=0x300000")
          << QStringLiteral("    EntryNo=5 ;    file=oem.img;    partition=oem;    type=image;    offset=0x98d25f;    size=0x200000")
          << QStringLiteral("    EntryNo=6 ;    file=misc.img;    partition=misc;    type=image;    offset=0xb8d25f;    size=0x8000")
          << QStringLiteral("    EntryNo=7 ;    file=package-file;    partition=userdata;    type=;    offset=0xa96a5f;    size=0xa7")
          << QStringLiteral("    EntryNo=8 ;    file=parameter.txt;    partition=parameter;    type=;    offset=0xb96a5f;    size=0x200");
    return lines.join(eol) + eol;
}

QRegularExpression exact(const QString& commandLine)
{
    return QRegularExpression(QStringLiteral("^%1$").arg(QRegularExpression::escape(commandLine)));
}

QString line(const QString& program, const QStringList& arguments = {})
{
    return ToolRunner::formatCommandLine(program, arguments);
}

/// Everything one invocation reported, in order. Parentless on purpose: it must outlive the
/// invocation (which deletes itself after finished()).
class Watch : public QObject
{
public:
    explicit Watch(ToolInvocation* invocation)
    {
        connect(invocation, &ToolInvocation::started, this, [this] {
            ++started;
            order << QStringLiteral("started");
        });
        connect(invocation, &ToolInvocation::outputReceived, this, [this](const QByteArray& chunk) {
            chunks << chunk;
            order << QStringLiteral("output");
        });
        connect(invocation, &ToolInvocation::finished, this, [this](const ToolResult& r) {
            results << r;
            order << QStringLiteral("finished");
        });
    }
    bool done() const { return !results.isEmpty(); }
    const ToolResult& result() const { return results.constFirst(); }

    int started = 0;
    QList<QByteArray> chunks;
    QList<ToolResult> results;
    QStringList order;
};

// ---- the helper program (this executable with --tool-helper) ------------------------------------

int runToolHelper(int argc, char** argv)
{
#ifdef Q_OS_WIN
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    const std::string mode = argv[2];
    auto say = [](std::FILE* stream, const char* text) {
        std::fputs(text, stream);
        std::fflush(stream);
    };
    auto sleepMs = [](int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); };

    if (mode == "echo") {
        for (int i = 3; i < argc; ++i) {
            say(stdout, argv[i]);
            say(stdout, i + 1 < argc ? " " : "\n");
        }
    } else if (mode == "args") {
        for (int i = 3; i < argc; ++i) {
            say(stdout, (std::string("[") + argv[i] + "]\n").c_str());
        }
    } else if (mode == "both") {
        say(stdout, "out-line\n");
        say(stderr, "err-line\n");
    } else if (mode == "cr") {
        say(stdout, "one\rtwo\r\nthree\n");
    } else if (mode == "exit") {
        return argc > 3 ? std::atoi(argv[3]) : 0;
    } else if (mode == "sleep") {
        say(stdout, "sleeping\n");
        sleepMs(argc > 3 ? std::atoi(argv[3]) : 1000);
        say(stdout, "woke\n");
    } else if (mode == "chunks") {
        for (int i = 1; i <= 3; ++i) {
            say(stdout, ("chunk " + std::to_string(i) + "\n").c_str());
            sleepMs(60);
        }
    } else if (mode == "stdin") {
        int c = 0;
        while ((c = std::getchar()) != EOF) {
            std::putchar(std::toupper(c));
        }
        std::fflush(stdout);
    } else if (mode == "env") {
        const char* value = argc > 3 ? std::getenv(argv[3]) : nullptr;
        say(stdout, value ? value : "<unset>");
    } else if (mode == "cwd") {
        say(stdout, std::filesystem::current_path().string().c_str());
    } else if (mode == "touchlater") {
        // <path> <ms>: wait, then create the file (proves the process outlived whoever started it).
        sleepMs(argc > 4 ? std::atoi(argv[4]) : 300);
        if (argc > 3) {
            if (std::FILE* file = std::fopen(argv[3], "wb")) {
                std::fputs("done", file);
                std::fclose(file);
            }
        }
    } else if (mode == "ignoreterm") {
#ifndef Q_OS_WIN
        std::signal(SIGTERM, SIG_IGN);   // only kill() ends this one
#endif
        say(stdout, "ready\n");
        sleepMs(30000);
    } else {
        return 99;
    }
    return 0;
}

QString helperProgram() { return QCoreApplication::applicationFilePath(); }

ToolInvocation* runHelper(ToolRunner& runner, const QStringList& modeAndArguments,
                          const ToolRunner::Options& options = ToolRunner::Options())
{
    return runner.run(helperProgram(), QStringList{QStringLiteral("--tool-helper")} + modeAndArguments, options);
}

/// A file that starts like an update.img.
QString writeImage(const QString& path, const QByteArray& head = QByteArrayLiteral("RKFW"))
{
    QFile file(path);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(head);
        file.write(QByteArray(64, '\0'));
    }
    return path;
}

} // namespace

class tst_firmwaretools : public QObject
{
    Q_OBJECT

private slots:
    // ---- ToolInvocation / FakeToolRunner ----------------------------------------------------
    void invocation_finishedOnceThenDeleted();
    void invocation_autoDeleteOff();
    void invocation_neverFinishesInsideRun();
    void invocation_cancelAndNoop();
    void invocation_cancelDropsPendingOutput();
    void invocation_failToStart();
    void fake_noHandlerFailsLoudly();
    void fake_handlersAndRecords();
    void fake_deliveriesAreSerialized();
    void fake_inputAndLiveInvocations();
    void format_commandLine();
    void result_outputTextAndSucceeded();

    // ---- ProcessToolRunner ------------------------------------------------------------------
    void process_echoThroughShell();
    void process_exitCodeThroughShell();
    void process_mergesStdoutAndStderr();
    void process_keepsCarriageReturns();
    void process_exitCodeAndSucceeded();
    void process_notStartedReportsFromEventLoop();
    void process_stdin();
    void process_environmentAndWorkingDirectory();
    void process_argumentsWithSpaces();
    void process_outputArrivesBeforeFinished();
    void process_timeout();
    void process_fastCommandIsNotTimedOut();
    void process_cancel();
    void process_cancelOnFinishedIsNoop();
    void process_cancelEscalatesToKill();
    void process_autoDelete();
    void process_concurrent();
    void process_runnerDeletedWhileRunning();
    void process_detachedOutlivesRunner();

    // ---- ToolLocator ------------------------------------------------------------------------
    void locator_fileNamesAndBundledPaths();
    void locator_overrideWins();
    void locator_bundledFolder();
    void locator_adbOnPath();
    void locator_notFound();

    // ---- RockchipTool parsers ---------------------------------------------------------------
    void rockchip_listNoDevices();
    void rockchip_listOneLoader();
    void rockchip_listTwoDevices();
    void rockchip_listOddWhitespace();
    void rockchip_listIgnoresUnknownLines();
    void rockchip_firmwareInfoNineEntries();
    void rockchip_firmwareInfoVariants();
    void rockchip_firmwareInfoEmpty();
    void rockchip_reportsSuccess();
    void rockchip_stripBanner();
    void rockchip_tailForMessage();
    void rockchip_isRkfwImage();
    void rockchip_progressWholeStream();
    void rockchip_progressSplitAnywhere();
    void rockchip_progressFlushAndEdgeCases();
    void rockchip_commandLines();

    // ---- AdbTool ----------------------------------------------------------------------------
    void adb_parseDevices();
    void adb_selectDeviceReady();
    void adb_selectDeviceMessages();
    void adb_parseForwardedPort();
    void adb_parseState();
    void adb_commandLines();

    // ---- UsbProbe ---------------------------------------------------------------------------
    void usb_serialAndHasDriver();
    void usb_driverHint();
    void usb_presentDevices();
    void usb_sysfsScan();

    // ---- McuLink ----------------------------------------------------------------------------
    void link_openAndClose();
    void link_socketCarriesData();
    void link_noRfmcu();
    void link_bridgeExits();
    void link_bridgeSilent();
    void link_oldAdb();
    void link_noBoardAndSeveralBoards();
    void link_adbCommandFails();
    void link_wantedSerial();
    void link_cliCommand();
    void link_linkLostWhenBridgeEnds();
    void link_linkLostWhenSocketDrops();
    void link_closeWhileOpening();
    void link_stateGuards();
    void link_reopenAndQueuedOpen();
    void link_destructorCleansUp();

    // ---- SocFlasher -------------------------------------------------------------------------
    void soc_statusText();
    void soc_statusModes();
    void soc_statusAdbProblems();
    void soc_statusToolMissingAndSerial();
    void soc_statusQueriesNeverOverlap();
    void soc_startValidation();
    void soc_alreadyInLoader();
    void soc_adbBoardRebootsToLoader();
    void soc_noBoardThenBoardAppears();
    void soc_noBoardThenAdbBoardAppears();
    void soc_secondAdbDeviceDuringAdbWait();
    void soc_unusableBoardsLogged();
    void soc_loaderNeverComes();
    void soc_flashWithoutSuccessLine();
    void soc_flashToolMissing();
    void soc_cancelWhileWaiting();
    void soc_cancelRefusedWhileFlashing();
    void soc_adbDoesNotComeBack();
    void soc_adbComesBackLate();
    void soc_waitForAdbOff();
    void soc_boardInfoFailureIsStillSuccess();
    void soc_cancelWhenIdleAndRestart();

    // ---- Real board (env-gated; nothing is flashed) ----
    void realBoard_statusAndMcuInfo();
};

// =============================================================================================
// ToolInvocation / FakeToolRunner
// =============================================================================================

void tst_firmwaretools::invocation_finishedOnceThenDeleted()
{
    FakeToolRunner runner;
    runner.addHandler(exact(line("tool", {"a"})), [](FakeToolInvocation* i) {
        i->emitOutput("one");
        i->emitOutput("two");
        i->finish(0);
        i->finish(5);   // a second end is ignored
    });
    ToolInvocation* invocation = runner.run(QStringLiteral("tool"), {QStringLiteral("a")});
    const QPointer<ToolInvocation> guard(invocation);
    Watch watch(invocation);
    QVERIFY(invocation->isRunning());
    QVERIFY(!invocation->isFinished());

    QTRY_VERIFY(watch.done());
    QCOMPARE(watch.results.size(), 1);
    QCOMPARE(watch.started, 1);
    QCOMPARE(watch.chunks, (QList<QByteArray>{"one", "two"}));
    QCOMPARE(watch.order, (QStringList{"started", "output", "output", "finished"}));
    QVERIFY(watch.result().succeeded());
    QCOMPARE(watch.result().exitCode, 0);
    QCOMPARE(watch.result().output, QByteArray("onetwo"));
    QCOMPARE(watch.result().commandLine, QStringLiteral("tool a"));
    QTRY_VERIFY(guard.isNull());   // scheduled with deleteLater() after finished()
}

void tst_firmwaretools::invocation_autoDeleteOff()
{
    FakeToolRunner runner;
    runner.addScript(exact(QStringLiteral("tool")), "x");
    ToolInvocation* invocation = runner.run(QStringLiteral("tool"), {});
    invocation->setAutoDelete(false);
    QVERIFY(!invocation->autoDelete());
    const QPointer<ToolInvocation> guard(invocation);
    Watch watch(invocation);
    QTRY_VERIFY(watch.done());
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
    QVERIFY(!guard.isNull());               // the owner keeps it
    QVERIFY(invocation->isFinished());
    QVERIFY(!invocation->isRunning());
    QCOMPARE(invocation->result().output, QByteArray("x"));
    QCOMPARE(invocation->output(), QByteArray("x"));
    delete invocation;
}

void tst_firmwaretools::invocation_neverFinishesInsideRun()
{
    FakeToolRunner runner;
    runner.addScript(exact(QStringLiteral("quick")), "done");
    ToolInvocation* invocation = runner.run(QStringLiteral("quick"), {});
    // Nothing has been delivered yet: not even the output.
    QVERIFY(!invocation->isFinished());
    QVERIFY(invocation->output().isEmpty());
    Watch watch(invocation);
    QTRY_VERIFY(watch.done());
}

void tst_firmwaretools::invocation_cancelAndNoop()
{
    FakeToolRunner runner;
    runner.addHandler(exact(QStringLiteral("hang")), [](FakeToolInvocation* i) { i->hang(); });
    ToolInvocation* invocation = runner.run(QStringLiteral("hang"), {});
    invocation->setAutoDelete(false);
    Watch watch(invocation);
    QTRY_COMPARE(watch.started, 1);
    QVERIFY(invocation->isRunning());

    invocation->cancel();
    invocation->cancel();   // the second one changes nothing
    QTRY_VERIFY(watch.done());
    QCOMPARE(watch.results.size(), 1);
    QVERIFY(watch.result().cancelled);
    QCOMPARE(watch.result().exitCode, -1);
    QVERIFY(!watch.result().succeeded());
    QVERIFY(!invocation->isRunning());

    invocation->cancel();   // finished: a no-op
    QCoreApplication::processEvents();
    QCOMPARE(watch.results.size(), 1);
    delete invocation;
}

void tst_firmwaretools::invocation_cancelDropsPendingOutput()
{
    FakeToolRunner runner;
    runner.addHandler(exact(QStringLiteral("slow")), [](FakeToolInvocation* i) {
        i->emitOutput("early");
        i->emitOutput("late", 400);
        i->finish(0);
    });
    ToolInvocation* invocation = runner.run(QStringLiteral("slow"), {});
    Watch watch(invocation);
    QTRY_COMPARE(watch.chunks.size(), 1);
    invocation->cancel();
    QTRY_VERIFY(watch.done());
    QCOMPARE(watch.chunks, (QList<QByteArray>{"early"}));   // "late" and the exit never happen
    QVERIFY(watch.result().cancelled);
    QCOMPARE(watch.result().output, QByteArray("early"));
}

void tst_firmwaretools::invocation_failToStart()
{
    FakeToolRunner runner;
    runner.addHandler(exact(QStringLiteral("missing")), [](FakeToolInvocation* i) { i->failToStart(QStringLiteral("no such file")); });
    ToolInvocation* invocation = runner.run(QStringLiteral("missing"), {});
    Watch watch(invocation);
    QTRY_VERIFY(watch.done());
    QCOMPARE(watch.started, 0);
    QVERIFY(!watch.result().started);
    QCOMPARE(watch.result().error, QStringLiteral("no such file"));
    QCOMPARE(watch.result().exitCode, -1);
    QVERIFY(!watch.result().succeeded());
}

void tst_firmwaretools::fake_noHandlerFailsLoudly()
{
    FakeToolRunner runner;
    ToolInvocation* invocation = runner.run(QStringLiteral("whatever"), {QStringLiteral("x y")});
    Watch watch(invocation);
    QTRY_VERIFY(watch.done());
    QCOMPARE(watch.result().exitCode, 127);
    QVERIFY(watch.result().outputText().contains(QStringLiteral("fake: no handler for whatever \"x y\"")));
    QVERIFY(!watch.result().succeeded());
}

void tst_firmwaretools::fake_handlersAndRecords()
{
    FakeToolRunner runner;
    runner.addScript(QRegularExpression(QStringLiteral("^tool ")), "generic", 0);
    runner.addScript(QRegularExpression(QStringLiteral("^tool special")), "specific", 0);   // never reached: first match wins
    runner.addScript(QRegularExpression(QStringLiteral("^other")), "boom", 3);

    ToolRunner::Options options;
    options.timeoutMs = 1234;
    ToolInvocation* a = runner.run(QStringLiteral("tool"), {QStringLiteral("special")}, options);
    ToolInvocation* b = runner.run(QStringLiteral("other"), {});
    Watch wa(a);
    Watch wb(b);
    QTRY_VERIFY(wa.done() && wb.done());
    QCOMPARE(wa.result().output, QByteArray("generic"));
    QCOMPARE(wb.result().exitCode, 3);
    QCOMPARE(runner.invocations(), (QStringList{"tool special", "other"}));
    QCOMPARE(runner.count(QRegularExpression(QStringLiteral("^tool"))), 1);
    QCOMPARE(runner.count(QRegularExpression(QStringLiteral("."))), 2);
    QCOMPARE(runner.lastInvocation(), QStringLiteral("other"));
    QCOMPARE(runner.lastOptions().timeoutMs, 0);

    runner.run(QStringLiteral("tool"), {QStringLiteral("again")}, options);
    QCOMPARE(runner.lastOptions().timeoutMs, 1234);

    // A handler may reconfigure the runner while it is called.
    FakeToolRunner other;
    other.addHandler(exact(QStringLiteral("self")), [&other](FakeToolInvocation* i) {
        other.reset();
        i->finish(0);
    });
    ToolInvocation* c = other.run(QStringLiteral("self"), {});
    Watch wc(c);
    QTRY_VERIFY(wc.done());
    QVERIFY(other.invocations().isEmpty());   // reset() inside the handler cleared the record, this call's entry too
    ToolInvocation* d = other.run(QStringLiteral("self"), {});
    Watch wd(d);
    QTRY_VERIFY(wd.done());
    QCOMPARE(wd.result().exitCode, 127);   // the handler was removed by reset()
    QCOMPARE(other.invocations(), (QStringList{"self"}));
}

void tst_firmwaretools::fake_deliveriesAreSerialized()
{
    FakeToolRunner runner;
    runner.addHandler(exact(QStringLiteral("seq")), [](FakeToolInvocation* i) {
        i->emitOutput("1", 40);
        i->emitOutput("2", 0);   // waits for "1" although its own delay is 0
        i->emitOutput("3", 10);
        i->finish(7, 0);
    });
    QElapsedTimer clock;
    clock.start();
    ToolInvocation* invocation = runner.run(QStringLiteral("seq"), {});
    Watch watch(invocation);
    QTRY_VERIFY(watch.done());
    QCOMPARE(watch.chunks, (QList<QByteArray>{"1", "2", "3"}));
    QCOMPARE(watch.result().exitCode, 7);
    QVERIFY2(clock.elapsed() >= 40, "the first delivery is delayed by its 40 ms");
}

void tst_firmwaretools::fake_inputAndLiveInvocations()
{
    FakeToolRunner runner;
    runner.addHandler(exact(QStringLiteral("cat")), [](FakeToolInvocation* i) { i->hang(); });
    ToolInvocation* invocation = runner.run(QStringLiteral("cat"), {});
    auto* fake = static_cast<FakeToolInvocation*>(invocation);
    QCOMPARE(runner.liveInvocations().size(), 1);
    QVERIFY(runner.liveInvocations().first() == fake);
    QCOMPARE(fake->program(), QStringLiteral("cat"));
    fake->writeInput("ab");
    fake->writeInput("c");
    QCOMPARE(fake->inputReceived(), QByteArray("abc"));
    QVERIFY(!fake->inputClosed());
    fake->closeInput();
    QVERIFY(fake->inputClosed());

    Watch watch(invocation);
    invocation->cancel();
    QTRY_VERIFY(watch.done());
    QTRY_VERIFY(runner.liveInvocations().isEmpty());
}

void tst_firmwaretools::format_commandLine()
{
    QCOMPARE(ToolRunner::formatCommandLine(QStringLiteral("adb"), {QStringLiteral("devices")}), QStringLiteral("adb devices"));
    QCOMPARE(ToolRunner::formatCommandLine(QStringLiteral("adb"), {}), QStringLiteral("adb"));
    QCOMPARE(ToolRunner::formatCommandLine(QStringLiteral("C:/Program Files/adb.exe"),
                                           {QStringLiteral("shell"), QStringLiteral("cat /proc/version")}),
             QStringLiteral("\"C:/Program Files/adb.exe\" shell \"cat /proc/version\""));
    QCOMPARE(ToolRunner::formatCommandLine(QStringLiteral("adb"), {QString()}), QStringLiteral("adb \"\""));
    QCOMPARE(ToolRunner::formatCommandLine(QStringLiteral("say"), {QStringLiteral("a \"b\"")}),
             QStringLiteral("say \"a \\\"b\\\"\""));
    QCOMPARE(ToolRunner::formatCommandLine(QStringLiteral("t"), {QStringLiteral("a\tb")}), QStringLiteral("t \"a\tb\""));
}

void tst_firmwaretools::result_outputTextAndSucceeded()
{
    ToolResult result;
    result.output = "a\r\nb\rc\nd";
    QCOMPARE(result.outputText(), QStringLiteral("a\nb\nc\nd"));
    QVERIFY(!result.succeeded());   // not started
    result.started = true;
    result.exitCode = 0;
    QVERIFY(result.succeeded());
    result.exitCode = 1;
    QVERIFY(!result.succeeded());
    result.exitCode = 0;
    result.cancelled = true;
    QVERIFY(!result.succeeded());
    result.cancelled = false;
    result.timedOut = true;
    QVERIFY(!result.succeeded());
    result.timedOut = false;
    result.output = QByteArray::fromHex("c3a9");   // UTF-8 "e acute"
    QCOMPARE(result.outputText(), QString::fromUtf8("\xc3\xa9"));
}

// =============================================================================================
// ProcessToolRunner
// =============================================================================================

void tst_firmwaretools::process_echoThroughShell()
{
    ProcessToolRunner runner;
#ifdef Q_OS_WIN
    ToolInvocation* invocation = runner.run(QStringLiteral("cmd"), {QStringLiteral("/c"), QStringLiteral("echo hi")});
#else
    ToolInvocation* invocation = runner.run(QStringLiteral("sh"), {QStringLiteral("-c"), QStringLiteral("echo hi")});
#endif
    Watch watch(invocation);
    QTRY_VERIFY_WITH_TIMEOUT(watch.done(), 15000);
    QVERIFY(watch.result().started);
    QVERIFY(watch.result().succeeded());
    QCOMPARE(watch.result().outputText().trimmed(), QStringLiteral("hi"));
    QCOMPARE(watch.order.first(), QStringLiteral("started"));
    QCOMPARE(watch.order.last(), QStringLiteral("finished"));
    QVERIFY(watch.result().commandLine.contains(QStringLiteral("echo hi")));
}

void tst_firmwaretools::process_exitCodeThroughShell()
{
    ProcessToolRunner runner;
#ifdef Q_OS_WIN
    ToolInvocation* invocation = runner.run(QStringLiteral("cmd"), {QStringLiteral("/c"), QStringLiteral("exit 3")});
#else
    ToolInvocation* invocation = runner.run(QStringLiteral("sh"), {QStringLiteral("-c"), QStringLiteral("exit 3")});
#endif
    Watch watch(invocation);
    QTRY_VERIFY_WITH_TIMEOUT(watch.done(), 15000);
    QVERIFY(watch.result().started);
    QCOMPARE(watch.result().exitCode, 3);
    QVERIFY(!watch.result().succeeded());
    QVERIFY(!watch.result().cancelled);
    QVERIFY(!watch.result().timedOut);
}

void tst_firmwaretools::process_mergesStdoutAndStderr()
{
    ProcessToolRunner runner;
    Watch watch(runHelper(runner, {QStringLiteral("both")}));
    QTRY_VERIFY_WITH_TIMEOUT(watch.done(), 15000);
    QVERIFY(watch.result().succeeded());
    const QString text = watch.result().outputText();
    QVERIFY(text.contains(QStringLiteral("out-line")));
    QVERIFY(text.contains(QStringLiteral("err-line")));   // stderr arrives in the same stream
}

void tst_firmwaretools::process_keepsCarriageReturns()
{
    ProcessToolRunner runner;
    Watch watch(runHelper(runner, {QStringLiteral("cr")}));
    QTRY_VERIFY_WITH_TIMEOUT(watch.done(), 15000);
    QCOMPARE(watch.result().output, QByteArray("one\rtwo\r\nthree\n"));   // raw: UF progress needs the "\r"
    QCOMPARE(watch.result().outputText(), QStringLiteral("one\ntwo\nthree\n"));
}

void tst_firmwaretools::process_exitCodeAndSucceeded()
{
    ProcessToolRunner runner;
    Watch ok(runHelper(runner, {QStringLiteral("exit"), QStringLiteral("0")}));
    Watch bad(runHelper(runner, {QStringLiteral("exit"), QStringLiteral("7")}));
    QTRY_VERIFY_WITH_TIMEOUT(ok.done() && bad.done(), 15000);
    QVERIFY(ok.result().succeeded());
    QCOMPARE(bad.result().exitCode, 7);
    QVERIFY(!bad.result().succeeded());
    QVERIFY(bad.result().error.isEmpty());
}

void tst_firmwaretools::process_notStartedReportsFromEventLoop()
{
    ProcessToolRunner runner;
    ToolInvocation* invocation = runner.run(QStringLiteral("no-such-program-for-the-firmware-tests"), {QStringLiteral("x")});
    QVERIFY(invocation != nullptr);
    Watch watch(invocation);
    QVERIFY(!invocation->isFinished());   // never from inside run()
    QCOMPARE(watch.results.size(), 0);
    QTRY_VERIFY_WITH_TIMEOUT(watch.done(), 15000);
    QCOMPARE(watch.results.size(), 1);
    QVERIFY(!watch.result().started);
    QVERIFY(!watch.result().error.isEmpty());
    QCOMPARE(watch.result().exitCode, -1);
    QVERIFY(!watch.result().succeeded());
    QCOMPARE(watch.started, 0);
}

void tst_firmwaretools::process_stdin()
{
    ProcessToolRunner runner;
    ToolInvocation* invocation = runHelper(runner, {QStringLiteral("stdin")});
    Watch watch(invocation);
    invocation->writeInput("abc\n");
    invocation->writeInput("def\n");
    invocation->closeInput();
    QTRY_VERIFY_WITH_TIMEOUT(watch.done(), 15000);
    QVERIFY(watch.result().succeeded());
    QCOMPARE(watch.result().outputText(), QStringLiteral("ABC\nDEF\n"));
}

void tst_firmwaretools::process_environmentAndWorkingDirectory()
{
    ProcessToolRunner runner;
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    ToolRunner::Options options;
    options.environment = {QStringLiteral("SU_FIRMWARE_TEST_VAR=hello world"), QStringLiteral("no-equals-sign-here")};
    options.workingDirectory = dir.path();
    Watch env(runHelper(runner, {QStringLiteral("env"), QStringLiteral("SU_FIRMWARE_TEST_VAR")}, options));
    Watch cwd(runHelper(runner, {QStringLiteral("cwd")}, options));
    Watch unset(runHelper(runner, {QStringLiteral("env"), QStringLiteral("SU_FIRMWARE_TEST_VAR")}));
    QTRY_VERIFY_WITH_TIMEOUT(env.done() && cwd.done() && unset.done(), 20000);
    QCOMPARE(env.result().outputText(), QStringLiteral("hello world"));
    QCOMPARE(QFileInfo(cwd.result().outputText().trimmed()).canonicalFilePath(), QFileInfo(dir.path()).canonicalFilePath());
    QCOMPARE(unset.result().outputText(), QStringLiteral("<unset>"));   // the entry does not leak into other runs
}

void tst_firmwaretools::process_argumentsWithSpaces()
{
    ProcessToolRunner runner;
    Watch watch(runHelper(runner, {QStringLiteral("args"), QStringLiteral("plain"), QStringLiteral("two words"),
                                   QStringLiteral("say \"hi\""), QString()}));
    QTRY_VERIFY_WITH_TIMEOUT(watch.done(), 15000);
    QCOMPARE(watch.result().outputText(),
             QStringLiteral("[plain]\n[two words]\n[say \"hi\"]\n[]\n"));
}

void tst_firmwaretools::process_outputArrivesBeforeFinished()
{
    ProcessToolRunner runner;
    Watch watch(runHelper(runner, {QStringLiteral("chunks")}));
    QTRY_VERIFY_WITH_TIMEOUT(watch.done(), 15000);
    QVERIFY(watch.chunks.size() >= 2);   // the helper pauses between its lines
    QCOMPARE(watch.order.last(), QStringLiteral("finished"));
    QCOMPARE(watch.result().outputText(), QStringLiteral("chunk 1\nchunk 2\nchunk 3\n"));
    QCOMPARE(watch.started, 1);
}

void tst_firmwaretools::process_timeout()
{
    ProcessToolRunner runner;
    ToolRunner::Options options;
    options.timeoutMs = 300;
    QElapsedTimer clock;
    clock.start();
    Watch watch(runHelper(runner, {QStringLiteral("sleep"), QStringLiteral("30000")}, options));
    QTRY_VERIFY_WITH_TIMEOUT(watch.done(), 20000);
    QVERIFY2(clock.elapsed() < 15000, "the helper sleeps for 30 s: the timeout must have killed it");
    QVERIFY(watch.result().timedOut);
    QVERIFY(!watch.result().cancelled);
    QVERIFY(!watch.result().succeeded());
    QCOMPARE(watch.result().exitCode, -1);
    QVERIFY(watch.result().error.contains(QStringLiteral("300")));
    QVERIFY(watch.result().started);
}

void tst_firmwaretools::process_fastCommandIsNotTimedOut()
{
    ProcessToolRunner runner;
    ToolRunner::Options options;
    options.timeoutMs = 20000;
    Watch watch(runHelper(runner, {QStringLiteral("exit"), QStringLiteral("0")}, options));
    QTRY_VERIFY_WITH_TIMEOUT(watch.done(), 15000);
    QVERIFY(watch.result().succeeded());
    QVERIFY(!watch.result().timedOut);
}

void tst_firmwaretools::process_cancel()
{
    ProcessToolRunner runner;
    ToolInvocation* invocation = runHelper(runner, {QStringLiteral("sleep"), QStringLiteral("30000")});
    Watch watch(invocation);
    QTRY_VERIFY_WITH_TIMEOUT(!watch.chunks.isEmpty(), 15000);   // "sleeping": it is running
    QElapsedTimer clock;
    clock.start();
    invocation->cancel();
    invocation->cancel();
    QTRY_VERIFY_WITH_TIMEOUT(watch.done(), 15000);
    QVERIFY(clock.elapsed() < 10000);
    QCOMPARE(watch.results.size(), 1);
    QVERIFY(watch.result().cancelled);
    QVERIFY(!watch.result().timedOut);
    QCOMPARE(watch.result().exitCode, -1);
    QVERIFY(!watch.result().succeeded());
    QVERIFY(watch.result().outputText().contains(QStringLiteral("sleeping")));   // what arrived before is kept
}

void tst_firmwaretools::process_cancelOnFinishedIsNoop()
{
    ProcessToolRunner runner;
    ToolInvocation* invocation = runHelper(runner, {QStringLiteral("exit"), QStringLiteral("0")});
    invocation->setAutoDelete(false);
    Watch watch(invocation);
    QTRY_VERIFY_WITH_TIMEOUT(watch.done(), 15000);
    invocation->cancel();
    QCoreApplication::processEvents();
    QCOMPARE(watch.results.size(), 1);
    QVERIFY(!watch.result().cancelled);
    QVERIFY(watch.result().succeeded());
    delete invocation;
}

void tst_firmwaretools::process_cancelEscalatesToKill()
{
#ifdef Q_OS_WIN
    QSKIP("on Windows cancel() kills at once: terminate() cannot reach a console program");
#else
    ProcessToolRunner runner;
    ToolInvocation* invocation = runHelper(runner, {QStringLiteral("ignoreterm")});
    Watch watch(invocation);
    QTRY_VERIFY_WITH_TIMEOUT(!watch.chunks.isEmpty(), 15000);   // "ready": SIGTERM is ignored from here on
    QElapsedTimer clock;
    clock.start();
    invocation->cancel();
    QTRY_VERIFY_WITH_TIMEOUT(watch.done(), 15000);
    QVERIFY2(clock.elapsed() >= 1500, "terminate() is ignored: only the kill after 2 s ends it");
    QVERIFY(watch.result().cancelled);
#endif
}

void tst_firmwaretools::process_autoDelete()
{
    ProcessToolRunner runner;
    ToolInvocation* deleted = runHelper(runner, {QStringLiteral("exit"), QStringLiteral("0")});
    ToolInvocation* kept = runHelper(runner, {QStringLiteral("exit"), QStringLiteral("0")});
    kept->setAutoDelete(false);
    const QPointer<ToolInvocation> deletedGuard(deleted);
    const QPointer<ToolInvocation> keptGuard(kept);
    Watch w1(deleted);
    Watch w2(kept);
    QTRY_VERIFY_WITH_TIMEOUT(w1.done() && w2.done(), 15000);
    QTRY_VERIFY(deletedGuard.isNull());
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVERIFY(!keptGuard.isNull());
    delete kept;
}

void tst_firmwaretools::process_concurrent()
{
    ProcessToolRunner runner;
    Watch a(runHelper(runner, {QStringLiteral("echo"), QStringLiteral("alpha")}));
    Watch b(runHelper(runner, {QStringLiteral("echo"), QStringLiteral("beta")}));
    Watch c(runHelper(runner, {QStringLiteral("echo"), QStringLiteral("gamma")}));
    QTRY_VERIFY_WITH_TIMEOUT(a.done() && b.done() && c.done(), 20000);
    QCOMPARE(a.result().outputText(), QStringLiteral("alpha\n"));
    QCOMPARE(b.result().outputText(), QStringLiteral("beta\n"));
    QCOMPARE(c.result().outputText(), QStringLiteral("gamma\n"));
}

void tst_firmwaretools::process_runnerDeletedWhileRunning()
{
    auto* runner = new ProcessToolRunner;
    ToolInvocation* invocation = runHelper(*runner, {QStringLiteral("sleep"), QStringLiteral("30000")});
    Watch watch(invocation);
    QTRY_VERIFY_WITH_TIMEOUT(!watch.chunks.isEmpty(), 15000);
    delete runner;   // kills the child instead of leaving it behind (and prints no Qt warning)
    QVERIFY(true);
}

void tst_firmwaretools::process_detachedOutlivesRunner()
{
    // Options::detached (McuLink's board cleanup when the Firmware tab goes away): started and
    // forgotten - finished() says it started, at once and without output - and deleting the
    // runner does not kill it: the helper still writes its file afterwards.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString marker = dir.filePath(QStringLiteral("detached.txt"));
    auto* runner = new ProcessToolRunner;
    ToolRunner::Options options;
    options.detached = true;
    ToolInvocation* invocation = runHelper(*runner, {QStringLiteral("touchlater"), QDir::toNativeSeparators(marker), QStringLiteral("1500")},
                                           options);
    QVERIFY(invocation->isRunning());
    Watch watch(invocation);
    QVERIFY(!watch.done());   // never from inside run()
    QTRY_VERIFY_WITH_TIMEOUT(watch.done(), 5000);
    QVERIFY(watch.result().started);
    QVERIFY(!watch.result().cancelled);
    QCOMPARE(watch.result().exitCode, 0);
    QVERIFY(watch.result().output.isEmpty());
    QVERIFY(!QFileInfo::exists(marker));   // reported long before the command is done
    delete runner;
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(marker), 20000);

    // A program that does not exist: started = false with the reason, from the event loop.
    ProcessToolRunner second;
    Watch missing(second.run(dir.filePath(QStringLiteral("no-such-program.exe")), {}, options));
    QVERIFY(!missing.done());
    QTRY_VERIFY_WITH_TIMEOUT(missing.done(), 5000);
    QVERIFY(!missing.result().started);
    QVERIFY(!missing.result().error.isEmpty());
}

// =============================================================================================
// ToolLocator
// =============================================================================================

namespace {

/// Creates files in `<application dir>/tools` for a test and removes exactly those again.
class BundledToolsFolder
{
public:
    explicit BundledToolsFolder(const QList<ToolLocator::Tool>& tools)
    {
        const QDir dir(ToolLocator::bundledToolsDirectory());
        m_created = !dir.exists();
        if (m_created && !QDir().mkpath(dir.path())) {
            return;
        }
        for (ToolLocator::Tool tool : tools) {
            const QString path = ToolLocator::bundledPath(tool);
            if (QFileInfo::exists(path)) {
                m_blocked = true;   // a real tools folder: leave it alone
                continue;
            }
            QDir().mkpath(QFileInfo(path).absolutePath());
            QFile file(path);
            if (file.open(QIODevice::WriteOnly)) {
                file.write("x");
                m_files << path;
            }
        }
    }
    ~BundledToolsFolder()
    {
        for (const QString& path : std::as_const(m_files)) {
            QFile::remove(path);
        }
        if (m_created) {
            QDir(ToolLocator::bundledToolsDirectory()).removeRecursively();
        }
    }
    bool usable() const { return !m_blocked; }

private:
    bool m_created = false;
    bool m_blocked = false;
    QStringList m_files;
};

/// Replaces PATH for the scope.
class PathScope
{
public:
    explicit PathScope(const QString& value) : m_old(qgetenv("PATH")) { qputenv("PATH", value.toLocal8Bit()); }
    ~PathScope() { qputenv("PATH", m_old); }

private:
    QByteArray m_old;
};

void touchExecutable(const QString& path)
{
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("x");
    file.close();
    QVERIFY(file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
}

} // namespace

void tst_firmwaretools::locator_fileNamesAndBundledPaths()
{
#ifdef Q_OS_WIN
    QCOMPARE(ToolLocator::fileName(ToolLocator::Tool::UpgradeTool), QStringLiteral("upgrade_tool.exe"));
    QCOMPARE(ToolLocator::fileName(ToolLocator::Tool::Adb), QStringLiteral("adb.exe"));
#else
    QCOMPARE(ToolLocator::fileName(ToolLocator::Tool::UpgradeTool), QStringLiteral("upgrade_tool"));
    QCOMPARE(ToolLocator::fileName(ToolLocator::Tool::Adb), QStringLiteral("adb"));
#endif
    QCOMPARE(ToolLocator::fileName(ToolLocator::Tool::DriverInstaller), QStringLiteral("DriverAssistant/DriverInstall.exe"));
    QCOMPARE(ToolLocator::bundledToolsDirectory(), QCoreApplication::applicationDirPath() + QStringLiteral("/tools"));
    QCOMPARE(ToolLocator::bundledPath(ToolLocator::Tool::Adb),
             ToolLocator::bundledToolsDirectory() + QLatin1Char('/') + ToolLocator::fileName(ToolLocator::Tool::Adb));
    QVERIFY(ToolLocator::bundledPath(ToolLocator::Tool::DriverInstaller).endsWith(QStringLiteral("/tools/DriverAssistant/DriverInstall.exe")));
}

void tst_firmwaretools::locator_overrideWins()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString custom = dir.filePath(QStringLiteral("my-adb"));
    QFile file(custom);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.close();

    const PathScope noPath(dir.filePath(QStringLiteral("empty")));
    QCOMPARE(ToolLocator::locate(ToolLocator::Tool::Adb, custom), QFileInfo(custom).absoluteFilePath());
    QCOMPARE(ToolLocator::describe(ToolLocator::Tool::Adb, custom), QStringLiteral("preferences ") + QFileInfo(custom).absoluteFilePath());
    QCOMPARE(ToolLocator::locate(ToolLocator::Tool::UpgradeTool, custom), QFileInfo(custom).absoluteFilePath());

    // An override that does not exist is ignored (spec: "when set and existing"); with nothing
    // bundled either, that means "not found".
    if (QFileInfo::exists(ToolLocator::bundledPath(ToolLocator::Tool::UpgradeTool))) {
        QSKIP("a bundled upgrade_tool exists next to the test executable");
    }
    QCOMPARE(ToolLocator::locate(ToolLocator::Tool::UpgradeTool, dir.filePath(QStringLiteral("missing"))), QString());
    QCOMPARE(ToolLocator::describe(ToolLocator::Tool::UpgradeTool, dir.filePath(QStringLiteral("missing"))),
             QStringLiteral("not found"));
}

void tst_firmwaretools::locator_bundledFolder()
{
    BundledToolsFolder folder({ToolLocator::Tool::UpgradeTool, ToolLocator::Tool::Adb, ToolLocator::Tool::DriverInstaller});
    if (!folder.usable()) {
        QSKIP("a tools folder already exists next to the test executable");
    }
    QTemporaryDir dir;
    const PathScope noPath(dir.filePath(QStringLiteral("empty")));
    for (ToolLocator::Tool tool : {ToolLocator::Tool::UpgradeTool, ToolLocator::Tool::Adb, ToolLocator::Tool::DriverInstaller}) {
        const QString expected = QFileInfo(ToolLocator::bundledPath(tool)).absoluteFilePath();
        QCOMPARE(ToolLocator::locate(tool), expected);
        QCOMPARE(ToolLocator::describe(tool), QStringLiteral("bundled ") + expected);
    }
    // The preferences override still beats the bundled copy.
    const QString custom = dir.filePath(QStringLiteral("custom"));
    QFile file(custom);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.close();
    QCOMPARE(ToolLocator::locate(ToolLocator::Tool::Adb, custom), QFileInfo(custom).absoluteFilePath());
}

void tst_firmwaretools::locator_adbOnPath()
{
    QTemporaryDir bin;
    QVERIFY(bin.isValid());
#ifdef Q_OS_WIN
    const QString fake = bin.filePath(QStringLiteral("adb.exe"));
    const QString upgrade = bin.filePath(QStringLiteral("upgrade_tool.exe"));
#else
    const QString fake = bin.filePath(QStringLiteral("adb"));
    const QString upgrade = bin.filePath(QStringLiteral("upgrade_tool"));
#endif
    touchExecutable(fake);
    touchExecutable(upgrade);
    if (QFileInfo::exists(ToolLocator::bundledPath(ToolLocator::Tool::Adb))) {
        QSKIP("a bundled adb exists next to the test executable");
    }
    const PathScope path(QDir::toNativeSeparators(bin.path()));
    const QString found = ToolLocator::locate(ToolLocator::Tool::Adb);
    QCOMPARE(QFileInfo(found).canonicalFilePath(), QFileInfo(fake).canonicalFilePath());
    QVERIFY(ToolLocator::describe(ToolLocator::Tool::Adb).startsWith(QStringLiteral("PATH ")));
    // "PATH for adb": upgrade_tool is never taken from there.
    if (!QFileInfo::exists(ToolLocator::bundledPath(ToolLocator::Tool::UpgradeTool))) {
        QCOMPARE(ToolLocator::locate(ToolLocator::Tool::UpgradeTool), QString());
    }
}

void tst_firmwaretools::locator_notFound()
{
    if (QFileInfo::exists(ToolLocator::bundledPath(ToolLocator::Tool::Adb))
        || QFileInfo::exists(ToolLocator::bundledPath(ToolLocator::Tool::UpgradeTool))) {
        QSKIP("a bundled tool exists next to the test executable");
    }
    QTemporaryDir empty;
    const PathScope path(QDir::toNativeSeparators(empty.path()));
    QCOMPARE(ToolLocator::locate(ToolLocator::Tool::Adb), QString());
    QCOMPARE(ToolLocator::describe(ToolLocator::Tool::Adb), QStringLiteral("not found"));
    QCOMPARE(ToolLocator::locate(ToolLocator::Tool::UpgradeTool), QString());
    QCOMPARE(ToolLocator::locate(ToolLocator::Tool::DriverInstaller), QString());
    QCOMPARE(ToolLocator::describe(ToolLocator::Tool::DriverInstaller), QStringLiteral("not found"));
}

// =============================================================================================
// RockchipTool parsers
// =============================================================================================

void tst_firmwaretools::rockchip_listNoDevices()
{
    QVERIFY(RockchipTool::parseDeviceList(QString::fromLatin1(kLdEmpty)).isEmpty());
    QVERIFY(RockchipTool::parseDeviceList(QString()).isEmpty());
    QVERIFY(RockchipTool::parseDeviceList(QStringLiteral("List of rockusb connected(0)")).isEmpty());
}

void tst_firmwaretools::rockchip_listOneLoader()
{
    const QList<RockusbDevice> devices = RockchipTool::parseDeviceList(QString::fromLatin1(kLdLoader));
    QCOMPARE(devices.size(), 1);
    const RockusbDevice& d = devices.first();
    QCOMPARE(d.devNo, 1);
    QCOMPARE(d.vid, quint16(0x2207));
    QCOMPARE(d.pid, quint16(0x110e));
    QCOMPARE(d.locationId, QStringLiteral("104"));
    QCOMPARE(d.mode, RockusbDevice::Mode::Loader);
    QCOMPARE(d.modeText(), QStringLiteral("Loader"));
    QCOMPARE(d.serial, QString());   // SerialNo= with nothing after it

    const QList<RockusbDevice> mask = RockchipTool::parseDeviceList(QString::fromLatin1(kLdMaskrom));
    QCOMPARE(mask.size(), 1);
    QCOMPARE(mask.first().mode, RockusbDevice::Mode::Maskrom);
    QCOMPARE(mask.first().modeText(), QStringLiteral("Maskrom"));
    QCOMPARE(mask.first().pid, quint16(0x110c));
    QVERIFY(!(mask.first() == d));
    QVERIFY(d == RockchipTool::parseDeviceList(QString::fromLatin1(kLdLoader)).first());
}

void tst_firmwaretools::rockchip_listTwoDevices()
{
    const QString output = QStringLiteral(
        "List of rockusb connected(2)\n"
        "DevNo=1\tVid=0x2207,Pid=0x110e,LocationID=104\tMode=Loader\tSerialNo=abc123\n"
        "DevNo=2\tVid=0x2207,Pid=0x110c,LocationID=203\tMode=Maskrom\tSerialNo=\n");
    const QList<RockusbDevice> devices = RockchipTool::parseDeviceList(output);
    QCOMPARE(devices.size(), 2);
    QCOMPARE(devices.at(0).serial, QStringLiteral("abc123"));
    QCOMPARE(devices.at(0).mode, RockusbDevice::Mode::Loader);
    QCOMPARE(devices.at(1).devNo, 2);
    QCOMPARE(devices.at(1).locationId, QStringLiteral("203"));
    QCOMPARE(devices.at(1).mode, RockusbDevice::Mode::Maskrom);
}

void tst_firmwaretools::rockchip_listOddWhitespace()
{
    // CRLF, blank lines, spaces instead of tabs, odd case, trailing blanks, no SerialNo at all.
    const QString output = QStringLiteral(
        "\r\n\r\nList of rockusb connected(3)\r\n\r\n"
        "  DevNo=3   Vid=0x2207, Pid=0x350b  LocationID=1a  MODE=maskrom  SerialNo=XYZ789  \r\n"
        "DEVNO = 4\tvid=0X2207,pid=0X110E,locationid=7f\tmode=LOADER\r\n"
        "DevNo=5\tVid=0x2207,Pid=0x110e,LocationID=9\tMode=Msc\tSerialNo=\r\n\r\n");
    const QList<RockusbDevice> devices = RockchipTool::parseDeviceList(output);
    QCOMPARE(devices.size(), 3);
    QCOMPARE(devices.at(0).devNo, 3);
    QCOMPARE(devices.at(0).vid, quint16(0x2207));
    QCOMPARE(devices.at(0).pid, quint16(0x350b));
    QCOMPARE(devices.at(0).locationId, QStringLiteral("1a"));
    QCOMPARE(devices.at(0).mode, RockusbDevice::Mode::Maskrom);
    QCOMPARE(devices.at(0).serial, QStringLiteral("XYZ789"));
    QCOMPARE(devices.at(1).devNo, 4);
    QCOMPARE(devices.at(1).pid, quint16(0x110e));
    QCOMPARE(devices.at(1).mode, RockusbDevice::Mode::Loader);
    QCOMPARE(devices.at(2).mode, RockusbDevice::Mode::Unknown);
    QCOMPARE(devices.at(2).modeText(), QStringLiteral("Unknown"));
}

void tst_firmwaretools::rockchip_listIgnoresUnknownLines()
{
    const QString output = QStringLiteral(
        "rockusb driver = 5.12,please upgrade it to enjoy fast download\n"
        "Creating Comm Object failed!\n"
        "DevNo=1\tVid=0x2207,Pid=0x110e,LocationID=104\tMode=Loader\tSerialNo=\n"
        "nothing to see here\n");
    QCOMPARE(RockchipTool::parseDeviceList(output).size(), 1);
}

void tst_firmwaretools::rockchip_firmwareInfoNineEntries()
{
    for (const QString& eol : {QStringLiteral("\r\n"), QStringLiteral("\n")}) {
        const RkFirmwareInfo info = RockchipTool::parseFirmwareInfo(sfiSample(eol, QStringLiteral("\t")));
        QVERIFY(info.isValid());
        QCOMPARE(info.file, QStringLiteral("C:\\Users\\moses\\code\\BuildAI\\SerialUtility\\build\\update.img"));
        QCOMPARE(info.type, QStringLiteral("Update Firmware"));
        QCOMPARE(info.chipTag, QStringLiteral("110E"));
        QCOMPARE(info.version, QStringLiteral("0.0.00"));
        QCOMPARE(info.buildTime, QStringLiteral("2026-10-13 21:45:50"));
        QVERIFY(!info.signedImage);
        QCOMPARE(info.loaderVersion, QStringLiteral("1.01"));
        QCOMPARE(info.loaderTime, QStringLiteral("2026-10-13 21:43:55"));
        QCOMPARE(info.entries.size(), 9);
        const RkFirmwareInfo::Entry& first = info.entries.first();
        QCOMPARE(first.no, 0);
        QCOMPARE(first.file, QStringLiteral("env.img"));
        QCOMPARE(first.partition, QStringLiteral("env"));
        QCOMPARE(first.type, QStringLiteral("image"));
        QCOMPARE(first.offset, quint64(0x3d25f));
        QCOMPARE(first.size, quint64(0x10000));
        const RkFirmwareInfo::Entry& userdata = info.entries.at(7);
        QCOMPARE(userdata.no, 7);
        QCOMPARE(userdata.file, QStringLiteral("package-file"));
        QCOMPARE(userdata.partition, QStringLiteral("userdata"));
        QCOMPARE(userdata.type, QString());
        QCOMPARE(userdata.offset, quint64(0xa96a5f));
        QCOMPARE(userdata.size, quint64(0xa7));
        QCOMPARE(info.entries.last().file, QStringLiteral("parameter.txt"));
        QCOMPARE(info.summary(),
                 QStringLiteral("Update Firmware, chip 110E, built 2026-10-13 21:45:50, loader 1.01, 9 entries"));
    }
}

void tst_firmwaretools::rockchip_firmwareInfoVariants()
{
    // Spaces instead of tabs between the fields of one line, and a signed image.
    QString output = sfiSample(QStringLiteral("\n"), QStringLiteral("   "));
    output.replace(QStringLiteral("   Sign:false"), QStringLiteral("   Sign:true"));   // the image's, not "Loader Sign"
    RkFirmwareInfo info = RockchipTool::parseFirmwareInfo(output);
    QCOMPARE(info.chipTag, QStringLiteral("110E"));
    QCOMPARE(info.version, QStringLiteral("0.0.00"));
    QCOMPARE(info.buildTime, QStringLiteral("2026-10-13 21:45:50"));   // a value with a space survives
    QVERIFY(info.signedImage);                                          // "Loader Sign:false" does not undo it
    QCOMPARE(info.loaderVersion, QStringLiteral("1.01"));
    QCOMPARE(info.entries.size(), 9);

    // Compact entry, decimal numbers, odd case.
    info = RockchipTool::parseFirmwareInfo(QStringLiteral("TYPE:Update Firmware\nEntryNo=3;file=boot.img;partition=boot;type=image;offset=256;size=0X200\n"));
    QCOMPARE(info.type, QStringLiteral("Update Firmware"));
    QCOMPARE(info.entries.size(), 1);
    QCOMPARE(info.entries.first().no, 3);
    QCOMPARE(info.entries.first().offset, quint64(256));
    QCOMPARE(info.entries.first().size, quint64(0x200));
    QCOMPARE(info.summary(), QStringLiteral("Update Firmware, 1 entry"));
}

void tst_firmwaretools::rockchip_firmwareInfoEmpty()
{
    for (const QString& output : {QString(), QStringLiteral("\r\n\r\n"), QString::fromLatin1(kBanner),
                                  QStringLiteral("Creating Comm Object failed!\n")}) {
        const RkFirmwareInfo info = RockchipTool::parseFirmwareInfo(output);
        QVERIFY(!info.isValid());
        QVERIFY(info.entries.isEmpty());
        QCOMPARE(info.summary(), QString());
    }
}

void tst_firmwaretools::rockchip_reportsSuccess()
{
    QVERIFY(RockchipTool::reportsSuccess(QStringLiteral("Reset Device Success\r\nUpgrade firmware ok.\r\n")));
    QVERIFY(RockchipTool::reportsSuccess(QStringLiteral("UPGRADE FIRMWARE OK")));
    QVERIFY(RockchipTool::reportsSuccess(QStringLiteral("Download Image... (100%)\rUpgrade firmware ok.")));
    QVERIFY(!RockchipTool::reportsSuccess(QStringLiteral("Upgrade firmware fail\r\n")));
    QVERIFY(!RockchipTool::reportsSuccess(QStringLiteral("Creating Comm Object failed!")));
    QVERIFY(!RockchipTool::reportsSuccess(QString()));
}

void tst_firmwaretools::rockchip_stripBanner()
{
    QCOMPARE(RockchipTool::stripBanner(QStringLiteral("rockusb driver = 5.12,please upgrade\r\n\r\nList of rockusb connected(0)\r\n")),
             QStringLiteral("\r\nList of rockusb connected(0)\r\n"));
    // Everything else stays byte for byte, "\r" progress lines included.
    QCOMPARE(RockchipTool::stripBanner(QStringLiteral("a\rb\r\nc")), QStringLiteral("a\rb\r\nc"));
    QCOMPARE(RockchipTool::stripBanner(QStringLiteral("ROCKUSB DRIVER = 1\nx\n")), QStringLiteral("x\n"));
    QCOMPARE(RockchipTool::stripBanner(QStringLiteral("rockusb driver = 5.12")), QString());   // no terminator at all
    QCOMPARE(RockchipTool::stripBanner(QString()), QString());
    QCOMPARE(RockchipTool::stripBanner(QStringLiteral("x rockusb driver = not at the start\n")),
             QStringLiteral("x rockusb driver = not at the start\n"));
}

void tst_firmwaretools::rockchip_tailForMessage()
{
    const QString output = QStringLiteral("rockusb driver = 5.12,please upgrade\r\n"
                                          "Using /tmp/config.ini\r\n"
                                          "Loading loader...\r\n\r\n"
                                          "Download Boot Start\rDownload Boot Success\r"
                                          "   \r\n"
                                          "Creating Comm Object failed!\r\n");
    QCOMPARE(RockchipTool::tailForMessage(output), QStringLiteral("Loading loader...\nDownload Boot Start\nDownload Boot Success\nCreating Comm Object failed!"));
    QCOMPARE(RockchipTool::tailForMessage(output, 2), QStringLiteral("Download Boot Success\nCreating Comm Object failed!"));
    QCOMPARE(RockchipTool::tailForMessage(output, 1), QStringLiteral("Creating Comm Object failed!"));
    QCOMPARE(RockchipTool::tailForMessage(output, 0), QString());
    QCOMPARE(RockchipTool::tailForMessage(QString()), QString());
    // The default is the last 8 lines, like `tail -n 8`.
    QStringList many;
    for (int i = 1; i <= 12; ++i) {
        many << QStringLiteral("line %1").arg(i);
    }
    const QStringList tail = RockchipTool::tailForMessage(many.join(QStringLiteral("\r\n"))).split(QLatin1Char('\n'));
    QCOMPARE(tail.size(), 8);
    QCOMPARE(tail.first(), QStringLiteral("line 5"));
    QCOMPARE(tail.last(), QStringLiteral("line 12"));
}

void tst_firmwaretools::rockchip_isRkfwImage()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QString error = QStringLiteral("stale");
    QVERIFY(RockchipTool::isRkfwImage(writeImage(dir.filePath(QStringLiteral("good.img"))), &error));
    QVERIFY(error.isEmpty());
    QVERIFY(RockchipTool::isRkfwImage(dir.filePath(QStringLiteral("good.img"))));   // the error pointer is optional

    QVERIFY(!RockchipTool::isRkfwImage(writeImage(dir.filePath(QStringLiteral("bad.img")), "RKAF"), &error));
    QVERIFY(error.contains(QStringLiteral("RKFW")));
    QVERIFY(error.contains(QStringLiteral("bad.img")));
    QFile shortFile(dir.filePath(QStringLiteral("short.img")));   // 2 bytes: shorter than the magic
    QVERIFY(shortFile.open(QIODevice::WriteOnly));
    shortFile.write("RK");
    shortFile.close();
    QVERIFY(!RockchipTool::isRkfwImage(shortFile.fileName(), &error));
    QVERIFY(error.contains(QStringLiteral("RKFW")));
    QFile empty(dir.filePath(QStringLiteral("empty.img")));
    QVERIFY(empty.open(QIODevice::WriteOnly));
    empty.close();
    QVERIFY(!RockchipTool::isRkfwImage(empty.fileName(), &error));
    QVERIFY(!error.isEmpty());

    QVERIFY(!RockchipTool::isRkfwImage(dir.filePath(QStringLiteral("missing.img")), &error));
    QVERIFY(error.contains(QStringLiteral("missing.img")));
    QVERIFY(!RockchipTool::isRkfwImage(dir.filePath(QStringLiteral("missing.img"))));
    QVERIFY(!RockchipTool::isRkfwImage(QString(), &error));
    QVERIFY(!error.isEmpty());
}

namespace {

const QByteArray kUfStream =
    "Loading firmware...\rDownload Boot Start\r\nDownload Boot Success\r\n"
    "Download Image... (1%)\rDownload Image... (42%)\rDownload Image... (100%)\r\n"
    "Reset Device Success\r\nUpgrade firmware ok.\r\n";

QList<RockchipTool::Progress> expectedUfProgress()
{
    return {{QStringLiteral("Loading firmware..."), -1},
            {QStringLiteral("Download Boot Start"), -1},
            {QStringLiteral("Download Boot Success"), -1},
            {QStringLiteral("Download Image..."), 1},
            {QStringLiteral("Download Image..."), 42},
            {QStringLiteral("Download Image..."), 100},
            {QStringLiteral("Reset Device Success"), -1},
            {QStringLiteral("Upgrade firmware ok."), -1}};
}

} // namespace

void tst_firmwaretools::rockchip_progressWholeStream()
{
    RockchipTool::ProgressParser parser;
    QCOMPARE(parser.lastStage(), QString());
    QCOMPARE(parser.lastPercent(), -1);
    const QList<RockchipTool::Progress> got = parser.feed(kUfStream);
    QCOMPARE(got, expectedUfProgress());
    QCOMPARE(parser.lastStage(), QStringLiteral("Upgrade firmware ok."));
    QCOMPARE(parser.lastPercent(), -1);
    QVERIFY(parser.flush().isEmpty());   // nothing left over
}

void tst_firmwaretools::rockchip_progressSplitAnywhere()
{
    // Byte by byte: the result must not depend on where the chunks end.
    RockchipTool::ProgressParser bytewise;
    QList<RockchipTool::Progress> got;
    for (char c : kUfStream) {
        got += bytewise.feed(QByteArray(1, c));
    }
    QCOMPARE(got, expectedUfProgress());

    // Every possible single split point, including inside "(42%" and between "\r" and "\n".
    for (qsizetype cut = 0; cut <= kUfStream.size(); ++cut) {
        RockchipTool::ProgressParser parser;
        QList<RockchipTool::Progress> result = parser.feed(kUfStream.left(cut));
        result += parser.feed(kUfStream.mid(cut));
        QVERIFY2(result == expectedUfProgress(), qPrintable(QStringLiteral("split at %1").arg(cut)));
    }

    // The two named cases, explicitly.
    RockchipTool::ProgressParser inside;
    QVERIFY(inside.feed("Download Image... (4").isEmpty());
    QVERIFY(inside.feed("2%").isEmpty());
    const auto closed = inside.feed(")\r");
    QCOMPARE(closed.size(), 1);
    QCOMPARE(closed.first(), (RockchipTool::Progress{QStringLiteral("Download Image..."), 42}));

    RockchipTool::ProgressParser crlf;
    QCOMPARE(crlf.feed("Download Boot Start\r").size(), 1);
    QVERIFY(crlf.feed("\n").isEmpty());   // the "\n" of "\r\n" opens an empty segment: nothing to report
    QCOMPARE(crlf.feed("Next\n").size(), 1);
}

void tst_firmwaretools::rockchip_progressFlushAndEdgeCases()
{
    RockchipTool::ProgressParser parser;
    QVERIFY(parser.feed("Download Image... (7").isEmpty());
    const auto tail = parser.flush();
    QCOMPARE(tail.size(), 1);
    QCOMPARE(tail.first().stage, QStringLiteral("Download Image... (7"));
    QCOMPARE(tail.first().percent, -1);
    QVERIFY(parser.flush().isEmpty());   // flushed once

    // A bare "(42%)" continues the running stage; the percentage is clamped; leading zeros read as decimal.
    RockchipTool::ProgressParser bare;
    bare.feed("Download Image... (10%)\r");
    auto next = bare.feed("(42%)\r");
    QCOMPARE(next.size(), 1);
    QCOMPARE(next.first(), (RockchipTool::Progress{QStringLiteral("Download Image..."), 42}));
    QCOMPARE(bare.lastPercent(), 42);
    next = bare.feed("Test (250%)\rTest (042%)\r");
    QCOMPARE(next.size(), 2);
    QCOMPARE(next.at(0).percent, 100);
    QCOMPARE(next.at(1).percent, 42);

    // Blank segments and white space are dropped, the last "(nn%)" wins, text after it is not part of the stage.
    RockchipTool::ProgressParser odd;
    const auto list = odd.feed("\r\n  \r\n\r  Writing (3%) (4%) trailing  \r\n");
    QCOMPARE(list.size(), 1);
    QCOMPARE(list.first().percent, 4);
    QCOMPARE(list.first().stage, QStringLiteral("Writing (3%)"));

    // A stage-only segment after a percentage resets lastPercent.
    RockchipTool::ProgressParser reset;
    reset.feed("A (5%)\r");
    QCOMPARE(reset.lastPercent(), 5);
    reset.feed("B\r");
    QCOMPARE(reset.lastStage(), QStringLiteral("B"));
    QCOMPARE(reset.lastPercent(), -1);
}

void tst_firmwaretools::rockchip_commandLines()
{
    FakeToolRunner runner;
    RockchipTool tool(&runner);
    tool.setToolPath(QStringLiteral("C:/tools/upgrade_tool.exe"));
    QCOMPARE(tool.toolPath(), QStringLiteral("C:/tools/upgrade_tool.exe"));
    QCOMPARE(tool.timeoutMs(), 15000);
    QCOMPARE(tool.runner(), &runner);

    tool.listDevices();
    QCOMPARE(runner.lastInvocation(), QStringLiteral("C:/tools/upgrade_tool.exe LD"));
    QCOMPARE(runner.lastOptions().timeoutMs, 15000);
    QVERIFY(runner.lastOptions().workingDirectory.isEmpty());   // no image, no folder of its own
    tool.setTimeoutMs(2500);
    // The image commands: the path in native separators (what upgrade_tool.exe has been seen
    // with), run in the image's folder.
    const QString spaced = QStringLiteral("C:/My Images/update.img");
    tool.firmwareInfo(spaced);
    QCOMPARE(runner.lastInvocation(),
             QStringLiteral("C:/tools/upgrade_tool.exe SFI \"%1\"").arg(QDir::toNativeSeparators(spaced)));
    QCOMPARE(runner.lastOptions().timeoutMs, 2500);
    QCOMPARE(runner.lastOptions().workingDirectory, QFileInfo(spaced).absolutePath());
    const QString plain = QStringLiteral("D:/update.img");
    tool.upgradeFirmware(plain);
    QCOMPARE(runner.lastInvocation(), QStringLiteral("C:/tools/upgrade_tool.exe UF ") + QDir::toNativeSeparators(plain));
    QCOMPARE(runner.lastOptions().timeoutMs, 0);   // UF is never timed out
    QCOMPARE(runner.lastOptions().workingDirectory, QFileInfo(plain).absolutePath());
#ifdef Q_OS_WIN
    QCOMPARE(runner.lastInvocation(), QStringLiteral("C:/tools/upgrade_tool.exe UF D:\\update.img"));
#endif
}

// =============================================================================================
// AdbTool
// =============================================================================================

void tst_firmwaretools::adb_parseDevices()
{
    const QString listing = QStringLiteral("List of devices attached\nc3d9b8674f4b94f6\tdevice\n\n");
    QCOMPARE(AdbTool::parseDevices(listing), (QList<AdbDevice>{{QStringLiteral("c3d9b8674f4b94f6"), QStringLiteral("device")}}));

    const QString noisy = QStringLiteral("* daemon not running; starting now at tcp:5037\n* daemon started successfully\n") + listing;
    QCOMPARE(AdbTool::parseDevices(noisy), AdbTool::parseDevices(listing));

    QCOMPARE(AdbTool::parseDevices(QString::fromLatin1(kAdbBoard)),
             (QList<AdbDevice>{{QStringLiteral("0225ce3c8d8d6e44"), QStringLiteral("device")}}));   // CRLF
    QVERIFY(AdbTool::parseDevices(QString::fromLatin1(kAdbNone)).isEmpty());
    QVERIFY(AdbTool::parseDevices(QString()).isEmpty());

    const QList<AdbDevice> several = AdbTool::parseDevices(
        QStringLiteral("List of devices attached\nA1\tunauthorized\nB2   offline\nC3\tdevice\nbroken\n"));
    QCOMPARE(several, (QList<AdbDevice>{{QStringLiteral("A1"), QStringLiteral("unauthorized")},
                                        {QStringLiteral("B2"), QStringLiteral("offline")},
                                        {QStringLiteral("C3"), QStringLiteral("device")}}));   // "broken" has no state

    // Notices from an adb that restarts its server are no devices.
    const QString restart = QStringLiteral("adb server version (41) doesn't match this client (40); killing...\n"
                                           "* daemon started successfully\nList of devices attached\nX9\tdevice\n");
    QCOMPARE(AdbTool::parseDevices(restart), (QList<AdbDevice>{{QStringLiteral("X9"), QStringLiteral("device")}}));
    QVERIFY(AdbTool::parseDevices(QStringLiteral("error: cannot connect to daemon\n")).isEmpty());
}

void tst_firmwaretools::adb_selectDeviceReady()
{
    QString error = QStringLiteral("stale");
    // The only ready board, whatever the others are (DeviceSelectionTests).
    QCOMPARE(AdbTool::selectDevice({{QStringLiteral("A1"), QStringLiteral("unauthorized")}, {QStringLiteral("B2"), QStringLiteral("device")}},
                                   QString(), &error),
             QStringLiteral("B2"));
    QVERIFY(error.isEmpty());
    // The wanted one when it is ready, even among several.
    QCOMPARE(AdbTool::selectDevice({{QStringLiteral("A1"), QStringLiteral("device")}, {QStringLiteral("B2"), QStringLiteral("device")}},
                                   QStringLiteral("B2"), &error),
             QStringLiteral("B2"));
    QVERIFY(error.isEmpty());
    QCOMPARE(AdbTool::selectDevice({{QStringLiteral("A1"), QStringLiteral("device")}}, QString(), nullptr), QStringLiteral("A1"));   // no error pointer
}

void tst_firmwaretools::adb_selectDeviceMessages()
{
    QString error;
    const QString hint = AdbTool::socFirmwareHint();
    QVERIFY(hint.contains(QStringLiteral("rfmcu")));
    QVERIFY(hint.contains(QStringLiteral("rockchip-platforms rv1106-bg2-main v1.2.0 or later")));
    QCOMPARE(hint, QStringLiteral("the SoC firmware needs ADB and rfmcu: rockchip-platforms rv1106-bg2-main v1.2.0 or later"));

    // No board at all.
    QVERIFY(AdbTool::selectDevice({}, QString(), &error).isEmpty());
    QCOMPARE(error, QStringLiteral("no ADB board attached: connect the Node board's USB-C port to this PC "
                                   "(Windows needs the ADB driver from DriverAssistant once); ") + hint);
    // Boards that are not ready are named.
    QVERIFY(AdbTool::selectDevice({{QStringLiteral("A1"), QStringLiteral("unauthorized")}, {QStringLiteral("B2"), QStringLiteral("offline")}},
                                  QString(), &error).isEmpty());
    QCOMPARE(error, QStringLiteral("no ADB board attached in a ready state, found A1 (unauthorized), B2 (offline): connect the "
                                   "Node board's USB-C port to this PC (Windows needs the ADB driver from DriverAssistant "
                                   "once); ") + hint);
    // Several ready boards need a choice.
    QVERIFY(AdbTool::selectDevice({{QStringLiteral("A1"), QStringLiteral("device")}, {QStringLiteral("B2"), QStringLiteral("device")}},
                                  QString(), &error).isEmpty());
    QCOMPARE(error, QStringLiteral("several ADB boards attached (A1, B2): choose one"));
    // A wanted board that is not ready.
    QVERIFY(AdbTool::selectDevice({{QStringLiteral("A1"), QStringLiteral("offline")}}, QStringLiteral("A1"), &error).isEmpty());
    QCOMPARE(error, QStringLiteral("board A1 is 'offline', not ready (unauthorized/offline: replug the cable)"));
    QVERIFY(AdbTool::selectDevice({{QStringLiteral("A1"), QStringLiteral("unauthorized")}}, QStringLiteral("A1"), &error).isEmpty());
    QCOMPARE(error, QStringLiteral("board A1 is 'unauthorized', not ready (unauthorized/offline: replug the cable)"));
    // A wanted board that is not attached.
    QVERIFY(AdbTool::selectDevice({{QStringLiteral("A1"), QStringLiteral("device")}}, QStringLiteral("Z9"), &error).isEmpty());
    QCOMPARE(error, QStringLiteral("board Z9 is not attached; attached: A1"));
    QVERIFY(AdbTool::selectDevice({{QStringLiteral("A1"), QStringLiteral("device")}, {QStringLiteral("B2"), QStringLiteral("device")}},
                                  QStringLiteral("Z9"), &error).isEmpty());
    QCOMPARE(error, QStringLiteral("board Z9 is not attached; attached: A1, B2"));
    QVERIFY(AdbTool::selectDevice({}, QStringLiteral("Z9"), &error).isEmpty());
    QCOMPARE(error, QStringLiteral("board Z9 is not attached; attached: none"));
    QVERIFY(AdbTool::selectDevice({}, QString(), nullptr).isEmpty());   // no error pointer: no crash
}

void tst_firmwaretools::adb_parseForwardedPort()
{
    QCOMPARE(AdbTool::parseForwardedPort(QStringLiteral("41234\n")), quint16(41234));
    QCOMPARE(AdbTool::parseForwardedPort(QStringLiteral("41234\r\n")), quint16(41234));
    QCOMPARE(AdbTool::parseForwardedPort(QStringLiteral("  7000  ")), quint16(7000));
    QCOMPARE(AdbTool::parseForwardedPort(QStringLiteral("* daemon started successfully\n5555\n")), quint16(5555));
    QCOMPARE(AdbTool::parseForwardedPort(QString()), quint16(0));
    QCOMPARE(AdbTool::parseForwardedPort(QStringLiteral("\n")), quint16(0));
    QCOMPARE(AdbTool::parseForwardedPort(QStringLiteral("error: more than one device/emulator\n")), quint16(0));
    QCOMPARE(AdbTool::parseForwardedPort(QStringLiteral("12ab")), quint16(0));
    QCOMPARE(AdbTool::parseForwardedPort(QStringLiteral("+80")), quint16(0));
    QCOMPARE(AdbTool::parseForwardedPort(QStringLiteral("-5")), quint16(0));
    QCOMPARE(AdbTool::parseForwardedPort(QStringLiteral("0")), quint16(0));
    QCOMPARE(AdbTool::parseForwardedPort(QStringLiteral("70000")), quint16(0));
    QCOMPARE(AdbTool::parseForwardedPort(QStringLiteral("65535")), quint16(65535));
}

void tst_firmwaretools::adb_parseState()
{
    QCOMPARE(AdbTool::parseState(QStringLiteral("device\n")), QStringLiteral("device"));
    QCOMPARE(AdbTool::parseState(QStringLiteral("device\r\n")), QStringLiteral("device"));
    QCOMPARE(AdbTool::parseState(QStringLiteral("* daemon started successfully\ndevice\n")), QStringLiteral("device"));
    QCOMPARE(AdbTool::parseState(QStringLiteral("error: no devices/emulators found\n")), QStringLiteral("error: no devices/emulators found"));
    QCOMPARE(AdbTool::parseState(QStringLiteral("  unknown \r\n")), QStringLiteral("unknown"));
    QCOMPARE(AdbTool::parseState(QStringLiteral("offline")), QStringLiteral("offline"));
    QCOMPARE(AdbTool::parseState(QString()), QString());
}

void tst_firmwaretools::adb_commandLines()
{
    FakeToolRunner runner;
    AdbTool adb(&runner);
    adb.setAdbPath(QStringLiteral("adb"));
    QCOMPARE(adb.timeoutMs(), 15000);
    QCOMPARE(adb.runner(), &runner);
    QCOMPARE(adb.adbPath(), QStringLiteral("adb"));
    QVERIFY(adb.serial().isEmpty());

    adb.devices();
    QCOMPARE(runner.lastInvocation(), QStringLiteral("adb devices"));
    QCOMPARE(runner.lastOptions().timeoutMs, 15000);
    adb.getState();
    QCOMPARE(runner.lastInvocation(), QStringLiteral("adb get-state"));
    adb.rebootLoader();
    QCOMPARE(runner.lastInvocation(), QStringLiteral("adb reboot loader"));
    adb.shell(QStringLiteral("cat /proc/version; uptime"));
    QCOMPARE(runner.lastInvocation(), QStringLiteral("adb shell \"cat /proc/version; uptime\""));   // one argument
    adb.forwardAnyPort(7000);
    QCOMPARE(runner.lastInvocation(), QStringLiteral("adb forward tcp:0 tcp:7000"));
    adb.removeForward(41234);
    QCOMPARE(runner.lastInvocation(), QStringLiteral("adb forward --remove tcp:41234"));
    adb.startBridge(7000);
    QCOMPARE(runner.lastInvocation(), QStringLiteral("adb shell rfmcu bridge 7000"));
    QCOMPARE(runner.lastOptions().timeoutMs, 0);   // the bridge runs until it is cancelled
    adb.run({QStringLiteral("features")}, 900);
    QCOMPARE(runner.lastInvocation(), QStringLiteral("adb features"));
    QCOMPARE(runner.lastOptions().timeoutMs, 900);

    adb.setSerial(QStringLiteral("0225ce3c8d8d6e44"));
    adb.setTimeoutMs(4000);
    QCOMPARE(adb.serial(), QStringLiteral("0225ce3c8d8d6e44"));
    adb.getState();
    QCOMPARE(runner.lastInvocation(), QStringLiteral("adb -s 0225ce3c8d8d6e44 get-state"));
    QCOMPARE(runner.lastOptions().timeoutMs, 4000);
    adb.shell(QStringLiteral("killall rfmcu 2>/dev/null; true"));
    QCOMPARE(runner.lastInvocation(), QStringLiteral("adb -s 0225ce3c8d8d6e44 shell \"killall rfmcu 2>/dev/null; true\""));
    adb.startBridge(7000);
    QCOMPARE(runner.lastInvocation(), QStringLiteral("adb -s 0225ce3c8d8d6e44 shell rfmcu bridge 7000"));
    adb.setSerial(QString());
    adb.devices();
    QCOMPARE(runner.lastInvocation(), QStringLiteral("adb devices"));   // no -s once the serial is cleared

    // The adb path is the program as given, quoted in the log line when it has spaces.
    adb.setAdbPath(QStringLiteral("C:/Program Files/adb.exe"));
    adb.devices();
    QCOMPARE(runner.lastInvocation(), QStringLiteral("\"C:/Program Files/adb.exe\" devices"));
}

// =============================================================================================
// UsbProbe
// =============================================================================================

namespace {

UsbDeviceInfo usbDevice(const QString& instanceId, const QString& driverClass, unsigned problem = 0,
                        const QString& description = QStringLiteral("Android ADB Interface"))
{
    UsbDeviceInfo info;
    info.vid = 0x2207;
    info.pid = 0x0006;
    info.instanceId = instanceId;
    info.description = description;
    info.driverClass = driverClass;
    info.problem = problem;
    return info;
}

} // namespace

void tst_firmwaretools::usb_serialAndHasDriver()
{
    QCOMPARE(usbDevice(QStringLiteral("USB\\VID_2207&PID_0006\\0225CE3C8D8D6E44"), {}).serial(), QStringLiteral("0225CE3C8D8D6E44"));
    QCOMPARE(usbDevice(QStringLiteral("USB\\VID_2207&PID_0006&MI_00\\6&1D2F3C4&0&0000"), {}).serial(), QString());   // a bus address
    QCOMPARE(usbDevice(QStringLiteral("USB\\VID_2207&PID_0006\\5&2A1B&0&3"), {}).serial(), QString());
    QCOMPARE(usbDevice(QStringLiteral("USB\\VID_2207&PID_0006"), {}).serial(), QString());
    QCOMPARE(usbDevice(QString(), {}).serial(), QString());

    QVERIFY(usbDevice(QStringLiteral("x"), QStringLiteral("AndroidUsbDeviceClass")).hasDriver());
    QVERIFY(!usbDevice(QStringLiteral("x"), QString()).hasDriver());
    QVERIFY(!usbDevice(QStringLiteral("x"), QStringLiteral("USB"), 28).hasDriver());   // CM_PROB_FAILED_INSTALL
    QVERIFY(UsbDeviceInfo().driverClass.isEmpty());
    QCOMPARE(UsbProbe::kRockchipVid, quint16(0x2207));
    QCOMPARE(UsbProbe::kAdbPid, quint16(0x0006));
}

void tst_firmwaretools::usb_driverHint()
{
    QCOMPARE(UsbProbe::driverHint({}), QString());

    const QString id = QStringLiteral("USB\\VID_2207&PID_0006\\0225CE3C8D8D6E44");
    QCOMPARE(UsbProbe::driverHint({usbDevice(id, QString())}),
             QStringLiteral("USB\\VID_2207&PID_0006\\0225CE3C8D8D6E44: no driver bound. Install the Rockchip USB drivers "
                            "(DriverAssistant) and replug the board."));
    const QString problem = UsbProbe::driverHint({usbDevice(id, QStringLiteral("USB"), 28)});
    QVERIFY(problem.contains(id));
    QVERIFY(problem.contains(QStringLiteral("problem code 28")));
    QVERIFY(problem.contains(QStringLiteral("DriverAssistant")));
    const QString bound = UsbProbe::driverHint({usbDevice(id, QStringLiteral("AndroidUsbDeviceClass"))});
    QVERIFY(bound.contains(id));
    QVERIFY(bound.contains(QStringLiteral("AndroidUsbDeviceClass is bound")));
    QVERIFY(bound.contains(QStringLiteral("neither adb nor upgrade_tool")));

    // A device with a driver next to one without: the one that needs help is named, once each.
    const QString other = QStringLiteral("USB\\VID_2207&PID_110E\\ABC");
    const QString mixed = UsbProbe::driverHint({usbDevice(id, QStringLiteral("AndroidUsbDeviceClass")), usbDevice(other, QString()),
                                                usbDevice(other, QString())});
    QVERIFY(mixed.contains(other));
    QVERIFY(!mixed.contains(id));
    QVERIFY(!mixed.contains(QStringLiteral("is bound")));
    QCOMPARE(mixed.count(QStringLiteral("no driver bound")), 1);
    // Two different devices without drivers: both sentences.
    const QString both = UsbProbe::driverHint({usbDevice(id, QString()), usbDevice(other, QString())});
    QVERIFY(both.contains(id) && both.contains(other));
    QCOMPARE(both.count(QStringLiteral("no driver bound")), 2);
}

void tst_firmwaretools::usb_presentDevices()
{
    // Whatever is plugged into the machine running the test: the contract holds for any content.
    const QList<UsbDeviceInfo> devices = UsbProbe::presentDevices();
    for (const UsbDeviceInfo& device : devices) {
        qInfo().noquote() << "present Rockchip USB device:" << device.instanceId << device.description << "class" << device.driverClass
                          << "problem" << device.problem;
        QCOMPARE(device.vid, UsbProbe::kRockchipVid);
        QVERIFY(device.instanceId.startsWith(QStringLiteral("USB\\VID_2207"), Qt::CaseInsensitive));
        QVERIFY(!device.description.isEmpty());
        QVERIFY(!device.instanceId.isEmpty());
    }
    for (qsizetype i = 1; i < devices.size(); ++i) {
        QVERIFY(devices.at(i - 1).instanceId <= devices.at(i).instanceId);   // sorted: a stable status line
    }
    // A vendor id nobody uses finds nothing, on every platform.
    QVERIFY(UsbProbe::presentDevices(0xFFFF).isEmpty());
    // The hint of the real list is empty exactly when the list is.
    QCOMPARE(UsbProbe::driverHint(devices).isEmpty(), devices.isEmpty());
}

void tst_firmwaretools::usb_sysfsScan()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    auto put = [&root](const QString& relative, const QString& text) {
        const QString path = root.filePath(relative);
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(text.toUtf8());
    };
    put(QStringLiteral("1-2/idVendor"), QStringLiteral("2207\n"));
    put(QStringLiteral("1-2/idProduct"), QStringLiteral("0006\n"));
    put(QStringLiteral("1-2/product"), QStringLiteral("Android ADB Interface\n"));
    put(QStringLiteral("1-2/serial"), QStringLiteral("0225ce3c8d8d6e44\n"));
    put(QStringLiteral("1-3/idVendor"), QStringLiteral("1d6b\n"));          // another vendor
    put(QStringLiteral("1-3/idProduct"), QStringLiteral("0002\n"));
    put(QStringLiteral("2-1/idVendor"), QStringLiteral("2207\n"));
    put(QStringLiteral("2-1/idProduct"), QStringLiteral("110e\n"));         // no serial, no product
#ifndef Q_OS_WIN   // a ':' cannot be part of a Windows file name
    QVERIFY(QDir().mkpath(root.filePath(QStringLiteral("1-2:1.0"))));        // an interface directory: no idVendor
#endif
    QVERIFY(QDir().mkpath(root.filePath(QStringLiteral("no-vendor-here"))));   // a directory without idVendor
    put(QStringLiteral("usb1/idVendor"), QStringLiteral("zzzz\n"));         // not hexadecimal
#ifndef Q_OS_WIN
    QVERIFY(QDir().mkpath(root.filePath(QStringLiteral("drivers/usb"))));
    QVERIFY(QFile::link(root.filePath(QStringLiteral("drivers/usb")), root.filePath(QStringLiteral("1-2/driver"))));
#endif

    const QList<UsbDeviceInfo> found = UsbProbe::scanSysfs(root.path(), UsbProbe::kRockchipVid);
    QCOMPARE(found.size(), 2);
    QCOMPARE(found.at(0).pid, quint16(0x0006));
    QCOMPARE(found.at(0).instanceId, QStringLiteral("USB\\VID_2207&PID_0006\\0225CE3C8D8D6E44"));
    QCOMPARE(found.at(0).serial(), QStringLiteral("0225CE3C8D8D6E44"));
    QCOMPARE(found.at(0).description, QStringLiteral("Android ADB Interface"));
#ifndef Q_OS_WIN
    QCOMPARE(found.at(0).driverClass, QStringLiteral("usb"));
    QVERIFY(found.at(0).hasDriver());
#endif
    QCOMPARE(found.at(1).pid, quint16(0x110e));
    QCOMPARE(found.at(1).serial(), QString());   // no serial: a bus address
    QVERIFY(found.at(1).instanceId.startsWith(QStringLiteral("USB\\VID_2207&PID_110E\\")));
    QVERIFY(!found.at(1).description.isEmpty());
    QVERIFY(found.at(1).driverClass.isEmpty());
    QCOMPARE(UsbProbe::scanSysfs(root.path(), 0x1d6b).size(), 1);
    QVERIFY(UsbProbe::scanSysfs(root.filePath(QStringLiteral("nowhere")), UsbProbe::kRockchipVid).isEmpty());
}

// =============================================================================================
// McuLink
// =============================================================================================

namespace {

/// The fake board behind a McuLink: scripts every adb command of adb_link.py's AdbLink, with the
/// forward pointing at a real QTcpServer (the stand-in for `rfmcu bridge` behind `adb forward`).
struct LinkRig
{
    LinkRig()
    {
        server.listen(QHostAddress::LocalHost, 0);
        port = server.serverPort();
        link.adb()->setAdbPath(QStringLiteral("adb"));
        link.setBridgeStartTimeoutMs(2000);
    }

    /// Register the scripted commands; the knobs below must be set first.
    void install() { install(runner); }
    void install(FakeToolRunner& target)
    {
        target.addScript(exact(line(QStringLiteral("adb"), {QStringLiteral("devices")})), devicesOutput, devicesExit);
        const QString s = QStringLiteral("c3d9");
        target.addScript(exact(line(QStringLiteral("adb"), {QStringLiteral("-s"), s, QStringLiteral("shell"),
                                                            QStringLiteral("command -v rfmcu || which rfmcu || true")})),
                         hasRfmcu ? QByteArray("/usr/bin/rfmcu\n") : QByteArray());
        target.addScript(exact(line(QStringLiteral("adb"), {QStringLiteral("-s"), s, QStringLiteral("shell"),
                                                            QStringLiteral("killall rfmcu 2>/dev/null; true")})),
                         QByteArray());
        if (hangForward) {
            target.addHandler(exact(line(QStringLiteral("adb"), {QStringLiteral("-s"), s, QStringLiteral("forward"),
                                                                 QStringLiteral("tcp:0"), QStringLiteral("tcp:7000")})),
                              [](FakeToolInvocation* i) { i->hang(); });
        } else {
            target.addScript(exact(line(QStringLiteral("adb"), {QStringLiteral("-s"), s, QStringLiteral("forward"),
                                                                QStringLiteral("tcp:0"), QStringLiteral("tcp:7000")})),
                             customForward ? forwardOutput : QByteArray::number(port) + "\n");
        }
        target.addHandler(exact(line(QStringLiteral("adb"), {QStringLiteral("-s"), s, QStringLiteral("shell"),
                                                             QStringLiteral("rfmcu"), QStringLiteral("bridge"),
                                                             QStringLiteral("7000")})),
                          [this](FakeToolInvocation* i) {
                              bridge = i;
                              if (!bridgeOutput.isEmpty()) {
                                  i->emitOutput(bridgeOutput);
                              }
                              if (bridgeExits) {
                                  i->finish(2);
                              } else {
                                  i->hang();
                              }
                          });
        target.addScript(QRegularExpression(QStringLiteral("^adb -s c3d9 forward --remove tcp:\\d+$")), QByteArray());
        target.addScript(exact(line(QStringLiteral("adb"), {QStringLiteral("-s"), s, QStringLiteral("shell"),
                                                            QStringLiteral("rfmcu cmd version")})),
                         "OK version=0.8.1 role=rx\n");
    }

    QString devices() const { return line(QStringLiteral("adb"), {QStringLiteral("devices")}); }
    QString probe() const
    {
        return line(QStringLiteral("adb"), {QStringLiteral("-s"), QStringLiteral("c3d9"), QStringLiteral("shell"),
                                            QStringLiteral("command -v rfmcu || which rfmcu || true")});
    }
    QString killall() const
    {
        return line(QStringLiteral("adb"), {QStringLiteral("-s"), QStringLiteral("c3d9"), QStringLiteral("shell"),
                                            QStringLiteral("killall rfmcu 2>/dev/null; true")});
    }
    QString forward() const
    {
        return line(QStringLiteral("adb"), {QStringLiteral("-s"), QStringLiteral("c3d9"), QStringLiteral("forward"),
                                            QStringLiteral("tcp:0"), QStringLiteral("tcp:7000")});
    }
    QString bridgeCommand() const
    {
        return line(QStringLiteral("adb"), {QStringLiteral("-s"), QStringLiteral("c3d9"), QStringLiteral("shell"),
                                            QStringLiteral("rfmcu"), QStringLiteral("bridge"), QStringLiteral("7000")});
    }
    QString removeForward() const
    {
        return line(QStringLiteral("adb"), {QStringLiteral("-s"), QStringLiteral("c3d9"), QStringLiteral("forward"),
                                            QStringLiteral("--remove"), QStringLiteral("tcp:%1").arg(port)});
    }

    QByteArray devicesOutput = "List of devices attached\nc3d9\tdevice\n\n";
    int devicesExit = 0;
    bool hasRfmcu = true;
    bool hangForward = false;
    bool customForward = false;   // false: the forward prints the server's port
    QByteArray forwardOutput;
    QByteArray bridgeOutput = "rfmcu: bridging /dev/ttyS1 (115200 baud) to 127.0.0.1:7000; on the PC run "
                              "\"adb forward tcp:7000 tcp:7000\" and connect to socket://127.0.0.1:7000. Ctrl+C stops.\n";
    bool bridgeExits = false;
    QPointer<FakeToolInvocation> bridge;

    FakeToolRunner runner;
    QTcpServer server;
    quint16 port = 0;
    McuLink link{&runner};
    QSignalSpy opened{&link, &McuLink::opened};
    QSignalSpy failed{&link, &McuLink::failed};
    QSignalSpy lost{&link, &McuLink::linkLost};
    QSignalSpy closed{&link, &McuLink::closed};
    QSignalSpy selected{&link, &McuLink::serialSelected};
    QSignalSpy logs{&link, &McuLink::logLine};
};

} // namespace

void tst_firmwaretools::link_openAndClose()
{
    LinkRig rig;
    rig.install();
    QVERIFY(!rig.link.isOpen());
    QVERIFY(rig.link.serial().isEmpty());
    QVERIFY(rig.link.open());
    QVERIFY(rig.link.isOpening());
    QTRY_COMPARE(rig.opened.count(), 1);
    QVERIFY(rig.failed.isEmpty());
    QCOMPARE(rig.opened.first().at(0).toString(), QStringLiteral("c3d9"));
    QCOMPARE(rig.opened.first().at(1).toUInt(), uint(rig.port));
    QCOMPARE(rig.selected.count(), 1);
    QCOMPARE(rig.selected.first().at(0).toString(), QStringLiteral("c3d9"));
    QVERIFY(rig.link.isOpen());
    QVERIFY(!rig.link.isOpening());
    QCOMPARE(rig.link.serial(), QStringLiteral("c3d9"));
    QCOMPARE(rig.link.localPort(), rig.port);
    QVERIFY(rig.link.socket() != nullptr);
    QTRY_COMPARE(rig.logs.count(), 1);
    QCOMPARE(rig.logs.first().at(0).toString(),
             QStringLiteral("INFO adb board=c3d9 bridge=socket://127.0.0.1:%1 (rfmcu bridge, /dev/ttyS1)").arg(rig.port));

    // Up to here, in the order of the Python AdbLink.open(): devices, rfmcu check, stale kill, forward, bridge.
    QCOMPARE(rig.runner.invocations(), (QStringList{rig.devices(), rig.probe(), rig.killall(), rig.forward(), rig.bridgeCommand()}));
    QVERIFY(rig.runner.lastOptions().timeoutMs == 0);   // the bridge has no timeout

    rig.link.close();
    QVERIFY(!rig.link.isOpen());
    QVERIFY(rig.link.socket() == nullptr);
    QTRY_COMPARE(rig.closed.count(), 1);
    // Cleanup, as close(): the bridge ended, then `killall rfmcu` again, then the forward removed - last.
    QCOMPARE(rig.runner.invocations(),
             (QStringList{rig.devices(), rig.probe(), rig.killall(), rig.forward(), rig.bridgeCommand(), rig.killall(), rig.removeForward()}));
    QCOMPARE(rig.runner.count(exact(rig.killall())), 2);
    QCOMPARE(rig.link.localPort(), quint16(0));
    QTRY_VERIFY(rig.runner.liveInvocations().isEmpty());   // the bridge was cancelled, nothing is left running
    QVERIFY(rig.failed.isEmpty());
    QVERIFY(rig.lost.isEmpty());
    QCOMPARE(rig.link.serial(), QStringLiteral("c3d9"));   // the board stays selected for cliCommand()
}

void tst_firmwaretools::link_socketCarriesData()
{
    LinkRig rig;
    rig.install();
    QVERIFY(rig.link.open());
    QTRY_COMPARE(rig.opened.count(), 1);
    QTRY_VERIFY(rig.server.hasPendingConnections());
    QTcpSocket* peer = rig.server.nextPendingConnection();
    QVERIFY(peer != nullptr);
    QTcpSocket* socket = rig.link.socket();
    QVERIFY(socket != nullptr);
    QCOMPARE(socket->state(), QAbstractSocket::ConnectedState);
    QCOMPARE(socket->peerPort(), rig.port);
    QCOMPARE(socket->socketOption(QAbstractSocket::LowDelayOption).toInt(), 1);

    peer->write("OK version=0.8.1 role=rx\r\n");
    QTRY_COMPARE(socket->bytesAvailable(), qint64(QByteArray("OK version=0.8.1 role=rx\r\n").size()));
    QCOMPARE(socket->readAll(), QByteArray("OK version=0.8.1 role=rx\r\n"));
    socket->write("version\r\n");
    QTRY_COMPARE(peer->bytesAvailable(), qint64(9));
    QCOMPARE(peer->readAll(), QByteArray("version\r\n"));

    rig.link.close();
    QTRY_COMPARE(rig.closed.count(), 1);
    QTRY_COMPARE(peer->state(), QAbstractSocket::UnconnectedState);   // the board side sees the client go
}

void tst_firmwaretools::link_noRfmcu()
{
    LinkRig rig;
    rig.hasRfmcu = false;
    rig.install();
    QVERIFY(rig.link.open());
    QTRY_COMPARE(rig.failed.count(), 1);
    QCOMPARE(rig.failed.first().at(0).toString(),
             QStringLiteral("board c3d9 has no rfmcu: ") + AdbTool::socFirmwareHint());
    QVERIFY(rig.failed.first().at(0).toString().contains(QStringLiteral("v1.2.0")));
    QTRY_COMPARE(rig.closed.count(), 1);
    QVERIFY(rig.opened.isEmpty());
    // The board was only looked at: nothing to kill, nothing forwarded.
    QCOMPARE(rig.runner.invocations(), (QStringList{rig.devices(), rig.probe()}));
    QCOMPARE(rig.selected.count(), 1);   // the board was selected before the check
    QVERIFY(!rig.link.isOpen());
    QVERIFY(!rig.link.isOpening());
}

void tst_firmwaretools::link_bridgeExits()
{
    LinkRig rig;
    rig.bridgeOutput = "rfmcu: /dev/ttyS1 is locked by another process (pid 412 syncd)\n";
    rig.bridgeExits = true;
    rig.install();
    QVERIFY(rig.link.open());
    QTRY_COMPARE(rig.failed.count(), 1);
    QCOMPARE(rig.failed.first().at(0).toString(),
             QStringLiteral("rfmcu bridge did not start on board c3d9: rfmcu: /dev/ttyS1 is locked by another process (pid 412 syncd)"));
    QTRY_COMPARE(rig.closed.count(), 1);
    QVERIFY(rig.opened.isEmpty());
    // close() ran: killall again and the forward is removed, last (Python test_a_bridge_that_exits...).
    QCOMPARE(rig.runner.invocations().last(), rig.removeForward());
    QCOMPARE(rig.runner.count(exact(rig.killall())), 2);
    // The "held by" message of a console that is still attached.
    LinkRig held;
    held.bridgeOutput = "rfmcu: /dev/ttyS1 is held by pid 190 (rfmcu)\n";
    held.bridgeExits = true;
    held.install();
    QVERIFY(held.link.open());
    QTRY_COMPARE(held.failed.count(), 1);
    QVERIFY(held.failed.first().at(0).toString().endsWith(QStringLiteral("is held by pid 190 (rfmcu)")));
}

void tst_firmwaretools::link_bridgeSilent()
{
    LinkRig rig;
    rig.bridgeOutput.clear();   // starts, says nothing
    rig.install();
    rig.link.setBridgeStartTimeoutMs(150);
    QCOMPARE(rig.link.bridgeStartTimeoutMs(), 150);
    QVERIFY(rig.link.open());
    QTRY_COMPARE(rig.failed.count(), 1);
    QCOMPARE(rig.failed.first().at(0).toString(), QStringLiteral("rfmcu bridge did not start on board c3d9: no output"));
    QTRY_COMPARE(rig.closed.count(), 1);
    QCOMPARE(rig.runner.invocations().last(), rig.removeForward());
    QTRY_VERIFY(rig.runner.liveInvocations().isEmpty());   // the silent bridge was cancelled
}

void tst_firmwaretools::link_oldAdb()
{
    LinkRig rig;
    rig.customForward = true;
    rig.forwardOutput.clear();   // an adb without tcp:0 prints nothing useful
    rig.install();
    QVERIFY(rig.link.open());
    QTRY_COMPARE(rig.failed.count(), 1);
    QCOMPARE(rig.failed.first().at(0).toString(), QStringLiteral("adb forward did not report a local port (got ''); update adb"));
    QTRY_COMPARE(rig.closed.count(), 1);
    QCOMPARE(rig.runner.count(exact(rig.bridgeCommand())), 0);   // never got as far as the bridge
    QCOMPARE(rig.runner.count(QRegularExpression(QStringLiteral("--remove"))), 0);   // and there was no forward to remove

    LinkRig junk;
    junk.customForward = true;
    junk.forwardOutput = QByteArrayLiteral("error: unknown host service\n");
    junk.install();
    QVERIFY(junk.link.open());
    QTRY_COMPARE(junk.failed.count(), 1);
    QCOMPARE(junk.failed.first().at(0).toString(),
             QStringLiteral("adb forward did not report a local port (got 'error: unknown host service'); update adb"));
}

void tst_firmwaretools::link_noBoardAndSeveralBoards()
{
    LinkRig none;
    none.devicesOutput = kAdbNone;
    none.install();
    QVERIFY(none.link.open());
    QTRY_COMPARE(none.failed.count(), 1);
    QVERIFY(none.failed.first().at(0).toString().startsWith(QStringLiteral("no ADB board attached: connect the Node board's USB-C port")));
    QVERIFY(none.failed.first().at(0).toString().endsWith(AdbTool::socFirmwareHint()));
    QTRY_COMPARE(none.closed.count(), 1);
    QCOMPARE(none.runner.invocations(), (QStringList{none.devices()}));   // nothing else was run
    QVERIFY(none.selected.isEmpty());

    LinkRig two;
    two.devicesOutput = "List of devices attached\nc3d9\tdevice\nzz11\tdevice\n";
    two.install();
    QVERIFY(two.link.open());
    QTRY_COMPARE(two.failed.count(), 1);
    QCOMPARE(two.failed.first().at(0).toString(), QStringLiteral("several ADB boards attached (c3d9, zz11): choose one"));

    LinkRig unauthorized;
    unauthorized.devicesOutput = "List of devices attached\nc3d9\tunauthorized\n";
    unauthorized.install();
    QVERIFY(unauthorized.link.open());
    QTRY_COMPARE(unauthorized.failed.count(), 1);
    QVERIFY(unauthorized.failed.first().at(0).toString().contains(QStringLiteral("found c3d9 (unauthorized)")));
}

void tst_firmwaretools::link_adbCommandFails()
{
    LinkRig exitCode;
    exitCode.devicesOutput = "error: cannot connect to daemon\n";
    exitCode.devicesExit = 1;
    exitCode.install();
    QVERIFY(exitCode.link.open());
    QTRY_COMPARE(exitCode.failed.count(), 1);
    QCOMPARE(exitCode.failed.first().at(0).toString(), QStringLiteral("adb devices failed (exit 1): error: cannot connect to daemon"));

    LinkRig silent;
    silent.devicesOutput.clear();
    silent.devicesExit = 1;
    silent.install();
    QVERIFY(silent.link.open());
    QTRY_COMPARE(silent.failed.count(), 1);
    QCOMPARE(silent.failed.first().at(0).toString(), QStringLiteral("adb devices failed (exit 1): no output"));

    // adb itself cannot be started.
    FakeToolRunner runner;
    runner.addHandler(exact(QStringLiteral("adb devices")), [](FakeToolInvocation* i) { i->failToStart(QStringLiteral("not found")); });
    McuLink link(&runner);
    link.adb()->setAdbPath(QStringLiteral("adb"));
    QSignalSpy failed(&link, &McuLink::failed);
    QVERIFY(link.open());
    QTRY_COMPARE(failed.count(), 1);
    QCOMPARE(failed.first().at(0).toString(), QStringLiteral("cannot run adb: not found"));
}

void tst_firmwaretools::link_wantedSerial()
{
    LinkRig rig;
    rig.devicesOutput = "List of devices attached\nc3d9\tdevice\nzz11\tdevice\n";
    rig.link.setSerial(QStringLiteral("c3d9"));
    QCOMPARE(rig.link.serial(), QStringLiteral("c3d9"));   // the wanted one before any open()
    rig.install();
    QVERIFY(rig.link.open());
    QTRY_COMPARE(rig.opened.count(), 1);
    QCOMPARE(rig.runner.invocations().first(), rig.devices());   // `adb devices` lists every board: no -s
    QCOMPARE(rig.opened.first().at(0).toString(), QStringLiteral("c3d9"));
    rig.link.close();
    QTRY_COMPARE(rig.closed.count(), 1);

    LinkRig absent;
    absent.link.setSerial(QStringLiteral("Z9"));
    absent.install();
    QVERIFY(absent.link.open());
    QTRY_COMPARE(absent.failed.count(), 1);
    QCOMPARE(absent.failed.first().at(0).toString(), QStringLiteral("board Z9 is not attached; attached: c3d9"));

    LinkRig offline;
    offline.devicesOutput = "List of devices attached\nc3d9\toffline\n";
    offline.link.setSerial(QStringLiteral("c3d9"));
    offline.install();
    QVERIFY(offline.link.open());
    QTRY_COMPARE(offline.failed.count(), 1);
    QCOMPARE(offline.failed.first().at(0).toString(),
             QStringLiteral("board c3d9 is 'offline', not ready (unauthorized/offline: replug the cable)"));
}

void tst_firmwaretools::link_cliCommand()
{
    LinkRig rig;
    rig.install();
    // Without a serial: the only board.
    ToolInvocation* anonymous = rig.link.cliCommand(QStringLiteral("version"));
    QCOMPARE(rig.runner.lastInvocation(), QStringLiteral("adb shell \"rfmcu cmd version\""));
    anonymous->cancel();
    rig.link.setSerial(QStringLiteral("c3d9"));
    ToolInvocation* named = rig.link.cliCommand(QStringLiteral("version"));
    QCOMPARE(rig.runner.lastInvocation(), QStringLiteral("adb -s c3d9 shell \"rfmcu cmd version\""));
    Watch watch(named);
    QTRY_VERIFY(watch.done());
    QCOMPARE(watch.result().outputText(), QStringLiteral("OK version=0.8.1 role=rx\n"));
}

void tst_firmwaretools::link_linkLostWhenBridgeEnds()
{
    LinkRig rig;
    rig.install();
    QVERIFY(rig.link.open());
    QTRY_COMPARE(rig.opened.count(), 1);
    QVERIFY(!rig.bridge.isNull());
    rig.bridge->emitOutput("rfmcu: client connected\n");
    rig.bridge->finish(1);   // cable pulled, rfmcu killed by someone
    QTRY_COMPARE(rig.lost.count(), 1);
    const QString message = rig.lost.first().at(0).toString();
    QVERIFY(message.contains(QStringLiteral("board c3d9")));
    QVERIFY(message.contains(QStringLiteral("ended")));
    QVERIFY(message.contains(QStringLiteral("client connected")));
    QVERIFY(!rig.link.isOpen());
    QVERIFY(rig.link.socket() == nullptr);
    QTRY_COMPARE(rig.closed.count(), 1);
    QCOMPARE(rig.runner.count(exact(rig.killall())), 2);          // the board is cleaned up like on close()
    QCOMPARE(rig.runner.invocations().last(), rig.removeForward());
    QVERIFY(rig.failed.isEmpty());
}

void tst_firmwaretools::link_linkLostWhenSocketDrops()
{
    LinkRig rig;
    rig.install();
    QVERIFY(rig.link.open());
    QTRY_COMPARE(rig.opened.count(), 1);
    QTRY_VERIFY(rig.server.hasPendingConnections());
    QTcpSocket* peer = rig.server.nextPendingConnection();
    peer->disconnectFromHost();
    QTRY_COMPARE(rig.lost.count(), 1);
    QVERIFY(rig.lost.first().at(0).toString().contains(QStringLiteral("connection")));
    QTRY_COMPARE(rig.closed.count(), 1);
    QCOMPARE(rig.runner.invocations().last(), rig.removeForward());
    QTRY_VERIFY(rig.runner.liveInvocations().isEmpty());   // the bridge was ended by the cleanup
}

void tst_firmwaretools::link_closeWhileOpening()
{
    LinkRig rig;
    rig.hangForward = true;
    rig.install();
    QVERIFY(rig.link.open());
    QTRY_COMPARE(rig.runner.count(exact(rig.forward())), 1);
    rig.link.close();
    QVERIFY(!rig.link.isOpening());
    QTRY_COMPARE(rig.closed.count(), 1);
    QVERIFY(rig.opened.isEmpty());
    QVERIFY(rig.failed.isEmpty());   // closing is not a failure
    QCOMPARE(rig.runner.count(exact(rig.bridgeCommand())), 0);
    QCOMPARE(rig.runner.count(exact(rig.killall())), 2);   // the stale kill had been sent: it is repeated
    QCOMPARE(rig.runner.count(QRegularExpression(QStringLiteral("--remove"))), 0);   // no port was ever reported
    QTRY_VERIFY(rig.runner.liveInvocations().isEmpty());   // the hanging forward was cancelled
    QCoreApplication::processEvents();
    QCOMPARE(rig.opened.count(), 0);

    // Closing during the first command: nothing was touched on the board, so nothing is cleaned up.
    FakeToolRunner runner;
    runner.addHandler(exact(QStringLiteral("adb devices")), [](FakeToolInvocation* i) { i->hang(); });
    McuLink link(&runner);
    link.adb()->setAdbPath(QStringLiteral("adb"));
    QSignalSpy closed(&link, &McuLink::closed);
    QVERIFY(link.open());
    QTRY_COMPARE(runner.liveInvocations().size(), 1);
    link.close();
    QTRY_COMPARE(closed.count(), 1);
    QCOMPARE(runner.invocations(), (QStringList{QStringLiteral("adb devices")}));
}

void tst_firmwaretools::link_stateGuards()
{
    LinkRig rig;
    rig.install();
    rig.link.close();   // closing a closed link: nothing happens
    QCoreApplication::processEvents();
    QVERIFY(rig.closed.isEmpty());

    QVERIFY(rig.link.open());
    QVERIFY(!rig.link.open());   // already opening
    QTRY_COMPARE(rig.opened.count(), 1);
    QVERIFY(!rig.link.open());   // already open
    QCOMPARE(rig.runner.count(exact(rig.devices())), 1);

    rig.link.close();
    rig.link.close();   // twice: one closed()
    QTRY_COMPARE(rig.closed.count(), 1);
    QCoreApplication::processEvents();
    QCOMPARE(rig.closed.count(), 1);
    QVERIFY(rig.link.socket() == nullptr);
}

void tst_firmwaretools::link_reopenAndQueuedOpen()
{
    LinkRig rig;
    rig.install();
    QVERIFY(rig.link.open());
    QTRY_COMPARE(rig.opened.count(), 1);
    rig.link.close();
    QTRY_COMPARE(rig.closed.count(), 1);
    QVERIFY(rig.link.open());   // a closed link opens again
    QTRY_COMPARE(rig.opened.count(), 2);
    QCOMPARE(rig.runner.count(exact(rig.forward())), 2);

    // open() right after close(): accepted, started when the cleanup of the old link is done.
    rig.link.close();
    QVERIFY(!rig.link.isOpen());
    QVERIFY(rig.link.open());
    QVERIFY(rig.link.isOpening());
    QVERIFY(!rig.link.open());   // only one open() can be queued
    QTRY_COMPARE(rig.opened.count(), 3);
    QCOMPARE(rig.closed.count(), 2);
    // The new link only started after the old cleanup's forward removal.
    const QStringList commands = rig.runner.invocations();
    const qsizetype lastRemove = commands.lastIndexOf(rig.removeForward());
    QVERIFY(lastRemove > 0);
    QVERIFY(commands.lastIndexOf(rig.devices()) > lastRemove);

    // An explicit close() withdraws a queued open().
    rig.link.close();
    QVERIFY(rig.link.open());
    rig.link.close();
    QTRY_COMPARE(rig.closed.count(), 3);
    QCoreApplication::processEvents();
    QCOMPARE(rig.opened.count(), 3);
    QVERIFY(!rig.link.isOpen());
}

void tst_firmwaretools::link_destructorCleansUp()
{
    LinkRig rig;
    rig.install();
    auto* link = new McuLink(&rig.runner);
    link->adb()->setAdbPath(QStringLiteral("adb"));
    QSignalSpy opened(link, &McuLink::opened);
    QVERIFY(link->open());
    QTRY_COMPARE(opened.count(), 1);
    QCOMPARE(rig.runner.count(exact(rig.killall())), 1);
    delete link;   // an open link: the bridge is ended and the board cleaned up, without waiting
    QCOMPARE(rig.runner.count(exact(rig.killall())), 2);
    QCOMPARE(rig.runner.invocations().last(), rig.removeForward());
    // Both cleanup commands are detached: the owner deletes the runner right after the link, and a
    // runner kills its processes (the board would keep the bridge's UART lock and the forward).
    const int count = static_cast<int>(rig.runner.invocations().size());
    QCOMPARE(rig.runner.invocations().at(count - 2), rig.killall());
    QVERIFY(rig.runner.optionsAt(count - 2).detached);
    QVERIFY(rig.runner.optionsAt(count - 1).detached);
    for (int i = 0; i < count - 2; ++i) {
        QVERIFY2(!rig.runner.optionsAt(i).detached, qPrintable(rig.runner.invocations().at(i)));   // the open sequence is not
    }
    QTRY_VERIFY(rig.runner.liveInvocations().isEmpty());

    // A runner that is gone first must not be used by the link's destructor (the owner of both
    // deletes children in creation order, so this happens in real trees).
    auto* shortLived = new FakeToolRunner;
    rig.install(*shortLived);
    auto* orphan = new McuLink(shortLived);
    orphan->adb()->setAdbPath(QStringLiteral("adb"));
    QSignalSpy orphanOpened(orphan, &McuLink::opened);
    QVERIFY(orphan->open());
    QTRY_COMPARE(orphanOpened.count(), 1);
    delete shortLived;
    delete orphan;   // must not touch the runner
}

// =============================================================================================
// SocFlasher
// =============================================================================================

namespace {

/// A SocFlasher on a FakeToolRunner with an image, tool paths and short polls. The scripts for
/// the commands of a normal flash are installed by installFlashScripts(); a test registers its
/// own handlers first (the first matching handler wins) for whatever differs.
struct FlashRig
{
    FlashRig()
    {
        image = writeImage(dir.filePath(QStringLiteral("update.img")));
        flasher.setImagePath(image);
        flasher.upgradeTool()->setToolPath(QStringLiteral("upgrade_tool"));
        flasher.adb()->setAdbPath(QStringLiteral("adb"));
        flasher.setPollIntervalMs(10);
    }

    static QRegularExpression ld() { return exact(QStringLiteral("upgrade_tool LD")); }
    static QRegularExpression uf() { return QRegularExpression(QStringLiteral("^upgrade_tool UF ")); }
    static QRegularExpression adbDevices() { return exact(QStringLiteral("adb devices")); }
    /// An adb command of the flow: with `-s <serial>` once the flow found its board on ADB.
    static QRegularExpression flowAdb(const QString& rest)
    {
        return QRegularExpression(QStringLiteral("^adb( -s \\S+)? %1$").arg(QRegularExpression::escape(rest)));
    }
    static QRegularExpression getState() { return flowAdb(QStringLiteral("get-state")); }
    static QRegularExpression reboot() { return flowAdb(QStringLiteral("reboot loader")); }
    static QRegularExpression catVersion() { return flowAdb(QStringLiteral("shell \"cat /proc/version\"")); }
    static QRegularExpression boardInfoCommand() { return flowAdb(QStringLiteral("shell \"cat /proc/version; uptime\"")); }
    /// The UF command line as upgrade_tool gets it (the image path in native separators).
    QString ufCommand() const { return QStringLiteral("upgrade_tool UF ") + QDir::toNativeSeparators(image); }

    /// UF with progress chunks split inside "(42%" and across "\r\n", ending with the success line.
    static void successfulUf(FakeToolInvocation* i)
    {
        i->emitOutput(kBanner);
        i->emitOutput("Loading firmware...\r\nDownload Boot Start\r\nDownload Boot Success\r\n");
        i->emitOutput("Download Image... (1%)\rDownload Image... (4");
        i->emitOutput("2%)\rDownload Image... (100%)\r");
        i->emitOutput("\nReset Device Success\r\nUpgrade firmware ok.\r\n");
        i->finish(0);
    }

    void installFlashScripts()
    {
        runner.addHandler(uf(), &FlashRig::successfulUf);
        runner.addScript(getState(), "device\n");
        runner.addScript(catVersion(), kProcVersion);
        runner.addScript(boardInfoCommand(), kProcVersion);
        runner.addScript(reboot(), "");
    }

    QList<SocFlasher::Phase> phaseList() const
    {
        QList<SocFlasher::Phase> list;
        for (const QList<QVariant>& args : phases) {
            list << args.at(0).value<SocFlasher::Phase>();
        }
        return list;
    }
    bool logged(const QString& fragment) const
    {
        for (const QList<QVariant>& args : logs) {
            if (args.at(0).toString().contains(fragment)) {
                return true;
            }
        }
        return false;
    }
    SocFlasher::DeviceStatus statusAt(qsizetype index) const { return statuses.at(index).at(0).value<SocFlasher::DeviceStatus>(); }

    QTemporaryDir dir;
    QString image;
    FakeToolRunner runner;
    SocFlasher flasher{&runner};
    QSignalSpy finished{&flasher, &SocFlasher::finished};
    QSignalSpy phases{&flasher, &SocFlasher::phaseChanged};
    QSignalSpy progress{&flasher, &SocFlasher::progress};
    QSignalSpy toolOutput{&flasher, &SocFlasher::toolOutput};
    QSignalSpy logs{&flasher, &SocFlasher::logLine};
    QSignalSpy boardInfo{&flasher, &SocFlasher::boardInfo};
    QSignalSpy statuses{&flasher, &SocFlasher::statusChanged};
    QSignalSpy downloadNeeded{&flasher, &SocFlasher::downloadModeNeeded};
};

} // namespace

void tst_firmwaretools::soc_statusText()
{
    using Mode = SocFlasher::DeviceStatus::Mode;
    SocFlasher::DeviceStatus status;
    QCOMPARE(status.mode, Mode::None);
    QCOMPARE(status.text(), QStringLiteral("no board"));
    status.detail = QStringLiteral("no ADB board attached in a ready state, found A1 (offline)");
    QCOMPARE(status.text(), QStringLiteral("no board (no ADB board attached in a ready state, found A1 (offline))"));

    SocFlasher::DeviceStatus adb;
    adb.mode = Mode::Adb;
    adb.serial = QStringLiteral("0225ce3c8d8d6e44");
    adb.detail = QStringLiteral("device");
    QCOMPARE(adb.text(), QStringLiteral("ADB device 0225ce3c8d8d6e44"));
    adb.serial.clear();
    QCOMPARE(adb.text(), QStringLiteral("ADB device"));

    SocFlasher::DeviceStatus loader;
    loader.mode = Mode::Loader;
    QCOMPARE(loader.text(), QStringLiteral("Loader mode"));
    loader.detail = QStringLiteral("DevNo=1 Vid=0x2207 Pid=0x110e LocationID=104");
    QCOMPARE(loader.text(), QStringLiteral("Loader mode (DevNo=1 Vid=0x2207 Pid=0x110e LocationID=104)"));
    SocFlasher::DeviceStatus mask;
    mask.mode = Mode::Maskrom;
    QCOMPARE(mask.text(), QStringLiteral("Maskrom mode"));

    SocFlasher::DeviceStatus driver;
    driver.mode = Mode::NoDriver;
    QVERIFY(!driver.text().isEmpty());
    driver.hint = QStringLiteral("USB\\VID_2207&PID_0006: no driver bound.");
    QCOMPARE(driver.text(), driver.hint);

    QVERIFY(loader == loader);
    QVERIFY(!(loader == mask));
    SocFlasher::DeviceStatus other = loader;
    other.serial = QStringLiteral("x");
    QVERIFY(!(loader == other));
    other = loader;
    other.hint = QStringLiteral("h");
    QVERIFY(!(loader == other));
}

void tst_firmwaretools::soc_statusModes()
{
    using Mode = SocFlasher::DeviceStatus::Mode;
    // Loader wins over everything: adb is not even asked.
    {
        FlashRig rig;
        rig.runner.addScript(FlashRig::ld(), kLdLoader);
        rig.runner.addScript(FlashRig::adbDevices(), kAdbBoard);
        rig.flasher.queryStatus();
        QTRY_COMPARE(rig.statuses.count(), 1);
        const SocFlasher::DeviceStatus s = rig.statusAt(0);
        QCOMPARE(s.mode, Mode::Loader);
        QCOMPARE(s.detail, QStringLiteral("DevNo=1 Vid=0x2207 Pid=0x110e LocationID=104"));
        QCOMPARE(s.text(), QStringLiteral("Loader mode (DevNo=1 Vid=0x2207 Pid=0x110e LocationID=104)"));
        QVERIFY(s.hint.isEmpty());
        QCOMPARE(rig.flasher.lastStatus(), s);
        QCOMPARE(rig.runner.count(FlashRig::adbDevices()), 0);
    }
    // Maskrom, with the serial the tool printed.
    {
        FlashRig rig;
        rig.runner.addScript(FlashRig::ld(), kBanner + "List of rockusb connected(1)\r\nDevNo=2\tVid=0x2207,Pid=0x110c,LocationID=203\tMode=Maskrom\tSerialNo=S1\r\n");
        rig.flasher.queryStatus();
        QTRY_COMPARE(rig.statuses.count(), 1);
        QCOMPARE(rig.statusAt(0).mode, Mode::Maskrom);
        QCOMPARE(rig.statusAt(0).serial, QStringLiteral("S1"));
        QCOMPARE(rig.statusAt(0).text(), QStringLiteral("Maskrom mode (DevNo=2 Vid=0x2207 Pid=0x110c LocationID=203)"));
    }
    // A rockusb device in a mode UF cannot write to does not count; adb is asked next.
    {
        FlashRig rig;
        rig.runner.addScript(FlashRig::ld(), kBanner + "List of rockusb connected(1)\r\nDevNo=1\tVid=0x2207,Pid=0x110e,LocationID=104\tMode=Msc\tSerialNo=\r\n");
        rig.runner.addScript(FlashRig::adbDevices(), kAdbBoard);
        rig.flasher.queryStatus();
        QTRY_COMPARE(rig.statuses.count(), 1);
        QCOMPARE(rig.statusAt(0).mode, Mode::Adb);
    }
    // ADB board, nothing in download mode.
    {
        FlashRig rig;
        rig.runner.addScript(FlashRig::ld(), kLdEmpty);
        rig.runner.addScript(FlashRig::adbDevices(), kAdbBoard);
        rig.flasher.queryStatus();
        QTRY_COMPARE(rig.statuses.count(), 1);
        const SocFlasher::DeviceStatus s = rig.statusAt(0);
        QCOMPARE(s.mode, Mode::Adb);
        QCOMPARE(s.serial, QStringLiteral("0225ce3c8d8d6e44"));
        QCOMPARE(s.detail, QStringLiteral("device"));
        QCOMPARE(s.text(), QStringLiteral("ADB device 0225ce3c8d8d6e44"));
    }
    // Nothing at all: "no board", unless the machine running the test has a Rockchip device on
    // its bus (the probe cannot be faked): then the driver hint takes its place.
    {
        FlashRig rig;
        rig.runner.addScript(FlashRig::ld(), kLdEmpty);
        rig.runner.addScript(FlashRig::adbDevices(), kAdbNone);
        const QList<UsbDeviceInfo> present = UsbProbe::presentDevices();
        rig.flasher.queryStatus();
        QTRY_COMPARE(rig.statuses.count(), 1);
        const SocFlasher::DeviceStatus s = rig.statusAt(0);
        if (present.isEmpty()) {
            QCOMPARE(s.mode, Mode::None);
            QCOMPARE(s.text(), QStringLiteral("no board"));
            QVERIFY(s.hint.isEmpty());
        } else {
            QCOMPARE(s.mode, Mode::NoDriver);
            QCOMPARE(s.hint, UsbProbe::driverHint(present));
            QCOMPARE(s.text(), s.hint);
            QCOMPARE(s.detail, present.first().instanceId);
        }
    }
}

void tst_firmwaretools::soc_statusAdbProblems()
{
    using Mode = SocFlasher::DeviceStatus::Mode;
    {
        FlashRig rig;   // boards that are there but not ready: the status says so, no driver guess
        rig.runner.addScript(FlashRig::ld(), kLdEmpty);
        rig.runner.addScript(FlashRig::adbDevices(), "List of devices attached\nA1\tunauthorized\n");
        rig.flasher.queryStatus();
        QTRY_COMPARE(rig.statuses.count(), 1);
        QCOMPARE(rig.statusAt(0).mode, Mode::None);
        QVERIFY(rig.statusAt(0).detail.contains(QStringLiteral("found A1 (unauthorized)")));
        QVERIFY(rig.statusAt(0).hint.isEmpty());
    }
    {
        FlashRig rig;   // two ready boards and no choice
        rig.runner.addScript(FlashRig::ld(), kLdEmpty);
        // `adb devices` and, once a board is chosen, `adb -s B2 devices`
        rig.runner.addScript(QRegularExpression(QStringLiteral("^adb (-s B2 )?devices$")),
                             "List of devices attached\nA1\tdevice\nB2\tdevice\n");
        rig.flasher.queryStatus();
        QTRY_COMPARE(rig.statuses.count(), 1);
        QCOMPARE(rig.statusAt(0).mode, Mode::None);
        QCOMPARE(rig.statusAt(0).detail, QStringLiteral("several ADB boards attached (A1, B2): choose one"));
        QCOMPARE(rig.statusAt(0).text(), QStringLiteral("no board (several ADB boards attached (A1, B2): choose one)"));
        rig.flasher.setSerial(QStringLiteral("B2"));   // the choice resolves it
        rig.flasher.queryStatus();
        QTRY_COMPARE(rig.statuses.count(), 2);
        QCOMPARE(rig.statusAt(1).mode, Mode::Adb);
        QCOMPARE(rig.statusAt(1).serial, QStringLiteral("B2"));
        QVERIFY(rig.runner.lastInvocation().contains(QStringLiteral("-s B2")));
    }
}

void tst_firmwaretools::soc_statusToolMissingAndSerial()
{
    using Mode = SocFlasher::DeviceStatus::Mode;
    {
        FlashRig rig;   // upgrade_tool cannot be started: adb still answers, and the tab is told why when nothing does
        rig.runner.addHandler(FlashRig::ld(), [](FakeToolInvocation* i) { i->failToStart(QStringLiteral("file not found")); });
        rig.runner.addScript(FlashRig::adbDevices(), kAdbNone);
        rig.flasher.queryStatus();
        QTRY_COMPARE(rig.statuses.count(), 1);
        QCOMPARE(rig.statusAt(0).mode, Mode::None);   // a tool problem is not a driver problem: no probe
        QCOMPARE(rig.statusAt(0).detail, QStringLiteral("upgrade_tool could not be run: file not found"));
    }
    {
        FlashRig rig;
        rig.runner.addScript(FlashRig::ld(), kLdEmpty);
        rig.runner.addHandler(FlashRig::adbDevices(), [](FakeToolInvocation* i) { i->failToStart(QStringLiteral("no adb")); });
        rig.flasher.queryStatus();
        QTRY_COMPARE(rig.statuses.count(), 1);
        QCOMPARE(rig.statusAt(0).mode, Mode::None);
        QCOMPARE(rig.statusAt(0).detail, QStringLiteral("adb could not be run: no adb"));
    }
    {
        FlashRig rig;   // the tool path goes to the command line; upgrade_tool working while adb is missing still finds a loader
        rig.flasher.upgradeTool()->setToolPath(QStringLiteral("C:/Program Files/rk/upgrade_tool.exe"));
        rig.runner.addScript(QRegularExpression(QStringLiteral("upgrade_tool.exe\" LD$")), kLdMaskrom);
        rig.flasher.queryStatus();
        QTRY_COMPARE(rig.statuses.count(), 1);
        QCOMPARE(rig.statusAt(0).mode, Mode::Maskrom);
    }
}

void tst_firmwaretools::soc_statusQueriesNeverOverlap()
{
    FlashRig rig;
    rig.runner.addScript(FlashRig::ld(), kLdLoader, 0, 30);
    rig.flasher.queryStatus();
    rig.flasher.queryStatus();   // dropped: one is running
    rig.flasher.queryStatus();
    QTRY_COMPARE(rig.statuses.count(), 1);
    QCOMPARE(rig.runner.count(FlashRig::ld()), 1);
    rig.flasher.queryStatus();   // free again
    QTRY_COMPARE(rig.statuses.count(), 2);
    QCOMPARE(rig.runner.count(FlashRig::ld()), 2);
    QCOMPARE(rig.statusAt(0), rig.statusAt(1));   // every query reports, changed or not
    QVERIFY(rig.logs.isEmpty());                  // the tab's refresh does not spam the log
}

void tst_firmwaretools::soc_startValidation()
{
    const QRegularExpression anything(QStringLiteral("."));
    {
        FlashRig rig;   // no image selected
        rig.flasher.setImagePath(QString());
        QVERIFY(!rig.flasher.start());
        QVERIFY(rig.finished.isEmpty());   // never from inside start()
        QTRY_COMPARE(rig.finished.count(), 1);
        QCOMPARE(rig.finished.first().at(0).toBool(), false);
        QCOMPARE(rig.finished.first().at(1).toString(), QStringLiteral("no firmware image selected"));
        QCOMPARE(rig.flasher.phase(), SocFlasher::Phase::Failed);
        QVERIFY(!rig.flasher.isRunning());
        QCOMPARE(rig.runner.count(anything), 0);   // nothing was run
    }
    {
        FlashRig rig;   // image does not exist
        rig.flasher.setImagePath(rig.dir.filePath(QStringLiteral("missing.img")));
        QVERIFY(!rig.flasher.start());
        QTRY_COMPARE(rig.finished.count(), 1);
        QVERIFY(rig.finished.first().at(1).toString().contains(QStringLiteral("no firmware image at")));
    }
    {
        FlashRig rig;   // not an RKFW package
        rig.flasher.setImagePath(writeImage(rig.dir.filePath(QStringLiteral("bad.img")), "RKAF"));
        QVERIFY(!rig.flasher.start());
        QTRY_COMPARE(rig.finished.count(), 1);
        QVERIFY(rig.finished.first().at(1).toString().contains(QStringLiteral("RKFW")));
    }
    {
        FlashRig rig;   // tool paths unset
        rig.flasher.upgradeTool()->setToolPath(QString());
        QVERIFY(!rig.flasher.start());
        QTRY_COMPARE(rig.finished.count(), 1);
        QVERIFY(rig.finished.first().at(1).toString().startsWith(QStringLiteral("upgrade_tool not found")));
        rig.flasher.upgradeTool()->setToolPath(QStringLiteral("upgrade_tool"));
        rig.flasher.adb()->setAdbPath(QString());
        QVERIFY(!rig.flasher.start());
        QTRY_COMPARE(rig.finished.count(), 2);
        QVERIFY(rig.finished.last().at(1).toString().startsWith(QStringLiteral("adb not found")));
        QCOMPARE(rig.runner.count(anything), 0);
    }
    {
        FlashRig rig;   // start() while a flash runs is refused without a finished()
        QPointer<FakeToolInvocation> uf;
        rig.runner.addScript(FlashRig::ld(), kLdLoader);
        rig.runner.addHandler(FlashRig::uf(), [&uf](FakeToolInvocation* i) {
            uf = i;
            i->hang();
        });
        QVERIFY(rig.flasher.start());
        QVERIFY(rig.flasher.isRunning());
        QVERIFY(!rig.flasher.start());
        QTRY_COMPARE(rig.flasher.phase(), SocFlasher::Phase::Flashing);
        QVERIFY(!rig.flasher.start());
        QVERIFY(rig.finished.isEmpty());
        // End the hanging UF (without the success line) so the rig can be torn down in order.
        QVERIFY(!uf.isNull());
        uf->finish(1);
        QTRY_COMPARE(rig.finished.count(), 1);
        QCOMPARE(rig.finished.first().at(0).toBool(), false);
    }
}

void tst_firmwaretools::soc_alreadyInLoader()
{
    FlashRig rig;
    rig.runner.addScript(FlashRig::ld(), kLdLoader);
    rig.installFlashScripts();
    QVERIFY(rig.flasher.start());
    QVERIFY(rig.flasher.isRunning());
    QCOMPARE(rig.flasher.phase(), SocFlasher::Phase::Detecting);
    QTRY_COMPARE(rig.finished.count(), 1);
    QCOMPARE(rig.finished.first().at(0).toBool(), true);
    QCOMPARE(rig.finished.first().at(1).toString(), QStringLiteral("Upgrade firmware ok; board is back on ADB"));
    QVERIFY(!rig.flasher.isRunning());
    QCOMPARE(rig.flasher.phase(), SocFlasher::Phase::Done);

    using P = SocFlasher::Phase;
    QCOMPARE(rig.phaseList(), (QList<P>{P::Detecting, P::WaitingForLoader, P::Flashing, P::WaitingForAdb, P::ReadingBoardInfo, P::Done}));
    QCOMPARE(rig.downloadNeeded.count(), 0);
    QCOMPARE(rig.runner.count(FlashRig::reboot()), 0);      // already in download mode: no reboot
    QCOMPARE(rig.runner.count(FlashRig::adbDevices()), 0);  // and adb is not even asked
    QCOMPARE(rig.runner.count(FlashRig::uf()), 1);
    QVERIFY(rig.runner.invocations().contains(rig.ufCommand()));
    QVERIFY(rig.logged(QStringLiteral("flashing %1 (Loader mode)").arg(QDir::toNativeSeparators(rig.image))));
    QVERIFY(rig.logged(QStringLiteral("$ upgrade_tool UF")));   // command lines go to the log
    QVERIFY(rig.logged(QStringLiteral("exit code 0")));

    // Progress: stages and percentages, the banner is not a stage, "(42%" split across chunks.
    QList<QPair<QString, int>> seen;
    for (const QList<QVariant>& args : rig.progress) {
        seen.append({args.at(0).toString(), args.at(1).toInt()});
    }
    QVERIFY(seen.contains({QStringLiteral("Loading firmware..."), -1}));
    QVERIFY(seen.contains({QStringLiteral("Download Boot Success"), -1}));
    QVERIFY(seen.contains({QStringLiteral("Download Image..."), 1}));
    QVERIFY(seen.contains({QStringLiteral("Download Image..."), 42}));
    QVERIFY(seen.contains({QStringLiteral("Download Image..."), 100}));
    QVERIFY(seen.contains({QStringLiteral("Upgrade firmware ok."), -1}));
    for (const auto& entry : std::as_const(seen)) {
        QVERIFY(!entry.first.startsWith(QStringLiteral("rockusb driver")));
    }
    QVERIFY(seen.indexOf({QStringLiteral("Download Image..."), 42}) < seen.indexOf({QStringLiteral("Download Image..."), 100}));

    // The raw tool lines, as they arrive, banner included, no empty lines.
    QStringList raw;
    for (const QList<QVariant>& args : rig.toolOutput) {
        raw << args.at(0).toString();
    }
    QVERIFY(raw.contains(QStringLiteral("Upgrade firmware ok.")));
    QVERIFY(raw.contains(QStringLiteral("Download Image... (42%)")));
    QVERIFY(raw.first().startsWith(QStringLiteral("rockusb driver")));
    QVERIFY(!raw.contains(QString()));

    QCOMPARE(rig.boardInfo.count(), 1);
    QVERIFY(rig.boardInfo.first().at(0).toString().contains(QStringLiteral("Linux version 5.10.209+")));
    QVERIFY(rig.boardInfo.first().at(0).toString().contains(QStringLiteral("up 0 min")));
    QVERIFY(!rig.boardInfo.first().at(0).toString().contains(QLatin1Char('\r')));
    QTRY_VERIFY(rig.runner.liveInvocations().isEmpty());
    // Order: LD, then the wait's LD, UF, then adb.
    const QStringList commands = rig.runner.invocations();
    QVERIFY(commands.indexOf(QStringLiteral("upgrade_tool LD")) < commands.indexOf(rig.ufCommand()));
    QVERIFY(commands.indexOf(rig.ufCommand()) < commands.indexOf(QStringLiteral("adb get-state")));
    QVERIFY(commands.indexOf(QStringLiteral("adb get-state")) < commands.indexOf(QStringLiteral("adb shell \"cat /proc/version; uptime\"")));
    // Found in Loader mode, the board's ADB serial is unknown: the ADB wait runs without -s.
    QCOMPARE(rig.runner.count(QRegularExpression(QStringLiteral("^adb -s "))), 0);
    // UF runs in the image's folder (upgrade_tool may write its Log folder into the working directory).
    const int ufIndex = static_cast<int>(commands.indexOf(rig.ufCommand()));
    QCOMPARE(QDir::fromNativeSeparators(rig.runner.optionsAt(ufIndex).workingDirectory), QFileInfo(rig.image).absolutePath());
    QCOMPARE(rig.runner.optionsAt(ufIndex).timeoutMs, 0);
}

void tst_firmwaretools::soc_adbBoardRebootsToLoader()
{
    FlashRig rig;
    auto ldCalls = std::make_shared<int>(0);
    rig.runner.addHandler(FlashRig::ld(), [ldCalls](FakeToolInvocation* i) {
        // 1: Detecting, 2-3: the first polls after `reboot loader`, then the board shows up.
        ++*ldCalls;
        i->emitOutput(*ldCalls >= 4 ? kLdLoader : kLdEmpty);
        i->finish(0);
    });
    rig.runner.addScript(FlashRig::adbDevices(), kAdbBoard);
    rig.installFlashScripts();
    QVERIFY(rig.flasher.start());
    QTRY_COMPARE(rig.finished.count(), 1);
    QCOMPARE(rig.finished.first().at(0).toBool(), true);

    using P = SocFlasher::Phase;
    QCOMPARE(rig.phaseList(), (QList<P>{P::Detecting, P::RebootingToLoader, P::WaitingForLoader, P::Flashing, P::WaitingForAdb,
                                        P::ReadingBoardInfo, P::Done}));
    QCOMPARE(rig.downloadNeeded.count(), 0);
    QCOMPARE(*ldCalls, 4);
    // The order of usb_update.sh: LD, adb devices, the kernel read, reboot, LD polls, UF. From the
    // moment `adb devices` found the board, every adb command names it (`adb -s <serial> ...`).
    const QStringList commands = rig.runner.invocations();
    const QString board = QStringLiteral("adb -s 0225ce3c8d8d6e44 ");
    QVERIFY(commands.indexOf(QStringLiteral("adb devices")) < commands.indexOf(board + QStringLiteral("shell \"cat /proc/version\"")));
    QVERIFY(commands.indexOf(board + QStringLiteral("shell \"cat /proc/version\"")) < commands.indexOf(board + QStringLiteral("reboot loader")));
    QVERIFY(commands.indexOf(board + QStringLiteral("reboot loader")) < commands.lastIndexOf(QStringLiteral("upgrade_tool LD")));
    QVERIFY(commands.lastIndexOf(QStringLiteral("upgrade_tool LD")) < commands.indexOf(rig.ufCommand()));
    QVERIFY(commands.indexOf(rig.ufCommand()) < commands.indexOf(board + QStringLiteral("get-state")));
    QVERIFY(commands.contains(board + QStringLiteral("shell \"cat /proc/version; uptime\"")));
    for (const QString& command : commands) {
        QVERIFY2(!command.startsWith(QStringLiteral("adb ")) || command == QLatin1String("adb devices") || command.startsWith(board),
                 qPrintable(command));
    }
    // The owner's choice (none) is back once the flow ended: the next status query lists every board.
    QVERIFY(rig.flasher.serial().isEmpty());
    QVERIFY(rig.flasher.adb()->serial().isEmpty());
    QCOMPARE(rig.runner.count(FlashRig::reboot()), 1);
    QVERIFY(rig.logged(QStringLiteral("board running 5.10.209+, rebooting it into loader mode")));
    QVERIFY(rig.logged(QStringLiteral("board is in download mode")));
    QVERIFY(rig.logged(QStringLiteral("board is back on ADB")));
    QCOMPARE(rig.statuses.count(), 1);   // the detection was a status query too
}

void tst_firmwaretools::soc_noBoardThenBoardAppears()
{
    FlashRig rig;
    auto present = std::make_shared<bool>(false);
    rig.runner.addHandler(FlashRig::ld(), [present](FakeToolInvocation* i) {
        i->emitOutput(*present ? kLdMaskrom : kLdEmpty);
        i->finish(0);
    });
    rig.runner.addScript(FlashRig::adbDevices(), kAdbNone);
    rig.installFlashScripts();
    QVERIFY(rig.flasher.start());
    QTRY_COMPARE(rig.downloadNeeded.count(), 1);   // the owner may now offer "reboot loader" / MASKROM instructions
    QCOMPARE(rig.flasher.phase(), SocFlasher::Phase::WaitingForLoader);
    QVERIFY(rig.flasher.isRunning());
    QVERIFY(rig.finished.isEmpty());
    QCOMPARE(rig.runner.count(FlashRig::uf()), 0);
    QVERIFY(rig.logged(QStringLiteral("no board found in ADB, MASKROM or Loader mode")));
    QVERIFY(rig.logged(QStringLiteral("waiting for the board to enter MASKROM/Loader mode (up to 30 s)")));
    const int pollsBefore = rig.runner.count(FlashRig::ld());
    QTRY_VERIFY(rig.runner.count(FlashRig::ld()) > pollsBefore + 1);   // it keeps polling
    QCOMPARE(rig.downloadNeeded.count(), 1);   // told once

    *present = true;   // the user holds MASKROM and replugs
    QTRY_COMPARE(rig.finished.count(), 1);
    QCOMPARE(rig.finished.first().at(0).toBool(), true);
    QVERIFY(rig.logged(QStringLiteral("flashing %1 (Maskrom mode)").arg(QDir::toNativeSeparators(rig.image))));
    QCOMPARE(rig.runner.count(FlashRig::uf()), 1);
    QCOMPARE(rig.runner.count(FlashRig::reboot()), 0);
}

void tst_firmwaretools::soc_noBoardThenAdbBoardAppears()
{
    FlashRig rig;
    auto stage = std::make_shared<int>(0);   // 0 nothing, 1 an ADB board, 2 loader mode
    rig.runner.addHandler(FlashRig::ld(), [stage](FakeToolInvocation* i) {
        i->emitOutput(*stage >= 2 ? kLdLoader : kLdEmpty);
        i->finish(0);
    });
    rig.runner.addHandler(FlashRig::adbDevices(), [stage](FakeToolInvocation* i) {
        i->emitOutput(*stage == 1 ? kAdbBoard : kAdbNone);
        i->finish(0);
    });
    rig.runner.addHandler(FlashRig::reboot(), [stage](FakeToolInvocation* i) {
        *stage = 2;   // `adb reboot loader` takes the board to Loader mode
        i->finish(0);
    });
    rig.installFlashScripts();
    QVERIFY(rig.flasher.start());
    QTRY_COMPARE(rig.downloadNeeded.count(), 1);
    *stage = 1;   // the user plugs the cable in; the board runs its firmware (ADB)
    QTRY_COMPARE(rig.finished.count(), 1);
    QCOMPARE(rig.finished.first().at(0).toBool(), true);
    QCOMPARE(rig.runner.count(FlashRig::reboot()), 1);   // an appearing ADB board gets step 2
    QVERIFY(rig.logged(QStringLiteral("board running 5.10.209+, rebooting it into loader mode")));
    using P = SocFlasher::Phase;
    const QList<P> phases = rig.phaseList();
    QVERIFY(phases.contains(P::RebootingToLoader));
    QCOMPARE(phases.first(), P::Detecting);
    QCOMPARE(phases.last(), P::Done);
    // The board that appeared on ADB is the flow's from then on.
    QCOMPARE(rig.runner.count(QRegularExpression(QStringLiteral("^adb -s 0225ce3c8d8d6e44 reboot loader$"))), 1);
    QVERIFY(rig.flasher.adb()->serial().isEmpty());
}

void tst_firmwaretools::soc_secondAdbDeviceDuringAdbWait()
{
    // A phone plugged in while the flashed board reboots: adb refuses every serial-less command
    // ("more than one device/emulator"). The flow names its board (found on ADB at the start), so
    // the wait still ends when the board is back.
    FlashRig rig;
    auto ldCalls = std::make_shared<int>(0);
    rig.runner.addHandler(FlashRig::ld(), [ldCalls](FakeToolInvocation* i) {
        ++*ldCalls;
        i->emitOutput(*ldCalls >= 2 ? kLdLoader : kLdEmpty);   // 1: Detecting, 2: after `reboot loader`
        i->finish(0);
    });
    rig.runner.addScript(FlashRig::adbDevices(), kAdbBoard);
    auto polls = std::make_shared<int>(0);
    rig.runner.addHandler(FlashRig::getState(), [polls](FakeToolInvocation* i) {
        ++*polls;
        if (!i->arguments().contains(QStringLiteral("0225ce3c8d8d6e44"))) {
            i->emitOutput("error: more than one device/emulator\r\n");
            i->finish(1);
        } else if (*polls >= 3) {
            i->emitOutput("device\r\n");
            i->finish(0);
        } else {
            i->emitOutput("error: device '0225ce3c8d8d6e44' not found\r\n");
            i->finish(1);
        }
    });
    rig.installFlashScripts();
    rig.flasher.setAdbReturnTimeoutSeconds(10);
    QVERIFY(rig.flasher.start());
    QTRY_COMPARE_WITH_TIMEOUT(rig.finished.count(), 1, 20000);
    QCOMPARE(rig.finished.first().at(0).toBool(), true);
    QCOMPARE(rig.finished.first().at(1).toString(), QStringLiteral("Upgrade firmware ok; board is back on ADB"));
    QCOMPARE(rig.boardInfo.count(), 1);
    QCOMPARE(*polls, 3);
    QCOMPARE(rig.runner.count(QRegularExpression(QStringLiteral("^adb get-state$"))), 0);
    QCOMPARE(rig.runner.count(exact(QStringLiteral("adb -s 0225ce3c8d8d6e44 shell \"cat /proc/version; uptime\""))), 1);
    QVERIFY(rig.flasher.adb()->serial().isEmpty());   // the owner's choice (none) again

    // An owner's serial wins over the board the flow finds.
    FlashRig chosen;
    chosen.runner.addHandler(FlashRig::ld(), [calls = std::make_shared<int>(0)](FakeToolInvocation* i) {
        ++*calls;
        i->emitOutput(*calls >= 2 ? kLdLoader : kLdEmpty);
        i->finish(0);
    });
    chosen.runner.addScript(QRegularExpression(QStringLiteral("^adb -s B2 devices$")),
                            "List of devices attached\nA1\tdevice\nB2\tdevice\n");
    chosen.installFlashScripts();
    chosen.flasher.setSerial(QStringLiteral("B2"));
    QVERIFY(chosen.flasher.start());
    QTRY_COMPARE_WITH_TIMEOUT(chosen.finished.count(), 1, 20000);
    QVERIFY2(chosen.finished.first().at(0).toBool(), qPrintable(chosen.finished.first().at(1).toString()));
    QCOMPARE(chosen.runner.count(exact(QStringLiteral("adb -s B2 reboot loader"))), 1);
    QCOMPARE(chosen.flasher.adb()->serial(), QStringLiteral("B2"));
}

void tst_firmwaretools::soc_unusableBoardsLogged()
{
    // Step 3 says why nothing usable was found before it advises MASKROM: with two boards (or an
    // unauthorized one) unplugging one is the remedy, not the button.
    FlashRig rig;
    rig.runner.addScript(FlashRig::ld(), kLdEmpty);
    rig.runner.addScript(FlashRig::adbDevices(), "List of devices attached\nA1\tdevice\nB2\tdevice\n");
    rig.installFlashScripts();
    QVERIFY(rig.flasher.start());
    QTRY_COMPARE(rig.downloadNeeded.count(), 1);
    qsizetype detailAt = -1;
    qsizetype adviceAt = -1;
    for (qsizetype i = 0; i < rig.logs.size(); ++i) {
        const QString text = rig.logs.at(i).at(0).toString();
        if (detailAt < 0 && text == QLatin1String("no usable board: several ADB boards attached (A1, B2): choose one")) {
            detailAt = i;
        }
        if (adviceAt < 0 && text.startsWith(QLatin1String("no board found in ADB, MASKROM or Loader mode"))) {
            adviceAt = i;
        }
    }
    QVERIFY(detailAt >= 0);
    QVERIFY(adviceAt > detailAt);
    rig.flasher.cancel();
    QTRY_COMPARE(rig.finished.count(), 1);
    QCOMPARE(rig.runner.count(FlashRig::reboot()), 0);   // nothing was chosen for the user

    // Nothing attached at all: no such line, only the advice.
    FlashRig empty;
    empty.runner.addScript(FlashRig::ld(), kLdEmpty);
    empty.runner.addScript(FlashRig::adbDevices(), kAdbNone);
    empty.installFlashScripts();
    QVERIFY(empty.flasher.start());
    QTRY_COMPARE(empty.downloadNeeded.count(), 1);
    QVERIFY(!empty.logged(QStringLiteral("no usable board")));
    empty.flasher.cancel();
    QTRY_COMPARE(empty.finished.count(), 1);
}

void tst_firmwaretools::soc_loaderNeverComes()
{
    FlashRig rig;
    rig.runner.addScript(FlashRig::ld(), kLdEmpty);
    rig.runner.addScript(FlashRig::adbDevices(), kAdbNone);
    rig.installFlashScripts();
    rig.flasher.setWaitTimeoutSeconds(1);
    QCOMPARE(rig.flasher.waitTimeoutSeconds(), 1);
    QCOMPARE(rig.flasher.pollIntervalMs(), 10);
    QVERIFY(rig.flasher.start());
    QTRY_COMPARE_WITH_TIMEOUT(rig.finished.count(), 1, 15000);
    QCOMPARE(rig.finished.first().at(0).toBool(), false);
    QCOMPARE(rig.finished.first().at(1).toString(), QStringLiteral("board did not enter MASKROM/Loader within 1 s"));
    QCOMPARE(rig.flasher.phase(), SocFlasher::Phase::Failed);
    QCOMPARE(rig.runner.count(FlashRig::uf()), 0);   // never flashed
    QVERIFY(!rig.flasher.isRunning());
    QCOMPARE(rig.phaseList().last(), SocFlasher::Phase::Failed);

    // After `adb reboot loader` the wait is LD only, same message and limit.
    FlashRig rebooted;
    rebooted.runner.addScript(FlashRig::ld(), kLdEmpty);
    rebooted.runner.addScript(FlashRig::adbDevices(), kAdbBoard);
    rebooted.installFlashScripts();
    rebooted.flasher.setWaitTimeoutSeconds(1);
    QVERIFY(rebooted.flasher.start());
    QTRY_COMPARE_WITH_TIMEOUT(rebooted.finished.count(), 1, 15000);
    QCOMPARE(rebooted.finished.first().at(1).toString(), QStringLiteral("board did not enter MASKROM/Loader within 1 s"));
    QCOMPARE(rebooted.runner.count(FlashRig::reboot()), 1);
    QCOMPARE(rebooted.runner.count(FlashRig::adbDevices()), 1);   // asked once, not on every poll
}

void tst_firmwaretools::soc_flashWithoutSuccessLine()
{
    FlashRig rig;
    rig.runner.addScript(FlashRig::ld(), kLdLoader);
    rig.runner.addHandler(FlashRig::uf(), [](FakeToolInvocation* i) {
        i->emitOutput(kBanner);
        i->emitOutput("Using /home/u/config.ini\r\nLoading firmware...\r\nDownload Boot Start\r\n");
        i->emitOutput("Download Image... (12%)\rCreating Comm Object failed!\r\n");
        i->finish(0);   // exit status 0 does not make it a success: the line decides (usb_update.sh)
    });
    rig.installFlashScripts();
    QVERIFY(rig.flasher.start());
    QTRY_COMPARE(rig.finished.count(), 1);
    QCOMPARE(rig.finished.first().at(0).toBool(), false);
    QCOMPARE(rig.finished.first().at(1).toString(),
             QStringLiteral("upgrade_tool did not report success: Loading firmware...\nDownload Boot Start\n"
                            "Download Image... (12%)\nCreating Comm Object failed!"));
    QCOMPARE(rig.flasher.phase(), SocFlasher::Phase::Failed);
    QCOMPARE(rig.runner.count(FlashRig::getState()), 0);   // no waiting for ADB after a failed flash
    QCOMPARE(rig.boardInfo.count(), 0);

    FlashRig silent;   // a UF that prints nothing
    silent.runner.addScript(FlashRig::ld(), kLdLoader);
    silent.runner.addScript(FlashRig::uf(), "", 1);
    silent.installFlashScripts();
    QVERIFY(silent.flasher.start());
    QTRY_COMPARE(silent.finished.count(), 1);
    QCOMPARE(silent.finished.first().at(1).toString(), QStringLiteral("upgrade_tool did not report success: no output"));
}

void tst_firmwaretools::soc_flashToolMissing()
{
    FlashRig rig;
    rig.runner.addScript(FlashRig::ld(), kLdLoader);
    rig.runner.addHandler(FlashRig::uf(), [](FakeToolInvocation* i) { i->failToStart(QStringLiteral("access denied")); });
    rig.installFlashScripts();
    QVERIFY(rig.flasher.start());
    QTRY_COMPARE(rig.finished.count(), 1);
    QCOMPARE(rig.finished.first().at(0).toBool(), false);
    QCOMPARE(rig.finished.first().at(1).toString(), QStringLiteral("cannot run upgrade_tool: access denied"));
    QVERIFY(rig.logged(QStringLiteral("could not run: access denied")));
}

void tst_firmwaretools::soc_cancelWhileWaiting()
{
    FlashRig rig;
    rig.runner.addScript(FlashRig::ld(), kLdEmpty);
    rig.runner.addScript(FlashRig::adbDevices(), kAdbNone);
    rig.installFlashScripts();
    QVERIFY(rig.flasher.start());
    QTRY_COMPARE(rig.downloadNeeded.count(), 1);
    rig.flasher.cancel();
    QTRY_COMPARE(rig.finished.count(), 1);
    QCOMPARE(rig.finished.first().at(0).toBool(), false);
    QCOMPARE(rig.finished.first().at(1).toString(), QStringLiteral("cancelled"));
    QCOMPARE(rig.flasher.phase(), SocFlasher::Phase::Cancelled);
    QVERIFY(!rig.flasher.isRunning());
    QCOMPARE(rig.phaseList().last(), SocFlasher::Phase::Cancelled);
    const int polls = rig.runner.count(FlashRig::ld());
    QTRY_VERIFY(rig.runner.liveInvocations().isEmpty());
    QTest::qWait(60);   // a few poll intervals: it stays stopped
    QCOMPARE(rig.runner.count(FlashRig::ld()), polls);
    QCOMPARE(rig.runner.count(FlashRig::uf()), 0);
    QCOMPARE(rig.finished.count(), 1);

    // Cancel while the loader wait is a plain LD poll that is still running.
    FlashRig polling;
    polling.runner.addHandler(FlashRig::ld(), [](FakeToolInvocation* i) { i->hang(); });
    QVERIFY(polling.flasher.start());
    QTRY_COMPARE(polling.runner.liveInvocations().size(), 1);
    polling.flasher.cancel();
    QTRY_COMPARE(polling.finished.count(), 1);
    QCOMPARE(polling.finished.first().at(1).toString(), QStringLiteral("cancelled"));
    QTRY_VERIFY(polling.runner.liveInvocations().isEmpty());   // the pending command was cancelled with it
}

void tst_firmwaretools::soc_cancelRefusedWhileFlashing()
{
    FlashRig rig;
    QPointer<FakeToolInvocation> uf;
    rig.runner.addScript(FlashRig::ld(), kLdLoader);
    rig.runner.addHandler(FlashRig::uf(), [&uf](FakeToolInvocation* i) {
        uf = i;
        i->emitOutput("Download Image... (10%)\r");
        i->hang();   // still writing the flash
    });
    rig.installFlashScripts();
    QVERIFY(rig.flasher.start());
    QTRY_COMPARE(rig.progress.count(), 1);
    QCOMPARE(rig.flasher.phase(), SocFlasher::Phase::Flashing);

    rig.flasher.cancel();
    QVERIFY(rig.logged(QStringLiteral("cannot cancel while upgrade_tool writes the flash")));
    QCOMPARE(rig.flasher.phase(), SocFlasher::Phase::Flashing);
    QVERIFY(rig.flasher.isRunning());
    QVERIFY(rig.finished.isEmpty());
    QVERIFY(!uf.isNull());
    QVERIFY(!uf->isFinished());   // UF was not cancelled

    // The flash may also not be disturbed by a status refresh (the USB device belongs to upgrade_tool).
    const int lds = rig.runner.count(FlashRig::ld());
    rig.flasher.queryStatus();
    QCoreApplication::processEvents();
    QCOMPARE(rig.runner.count(FlashRig::ld()), lds);
    QCOMPARE(rig.statuses.count(), 1);   // only the detection before the flash reported

    uf->emitOutput("Upgrade firmware ok.\r\n");
    uf->finish(0);
    QTRY_COMPARE(rig.finished.count(), 1);
    QCOMPARE(rig.finished.first().at(0).toBool(), true);
    QCOMPARE(rig.flasher.phase(), SocFlasher::Phase::Done);
}

void tst_firmwaretools::soc_adbDoesNotComeBack()
{
    FlashRig rig;
    rig.runner.addScript(FlashRig::ld(), kLdLoader);
    rig.runner.addScript(FlashRig::getState(), "error: no devices/emulators found\n", 1);
    rig.installFlashScripts();
    rig.flasher.setAdbReturnTimeoutSeconds(1);
    QCOMPARE(rig.flasher.adbReturnTimeoutSeconds(), 1);
    QVERIFY(rig.flasher.start());
    QTRY_COMPARE_WITH_TIMEOUT(rig.finished.count(), 1, 15000);
    // A success with a warning, as the script treats it.
    QCOMPARE(rig.finished.first().at(0).toBool(), true);
    QCOMPARE(rig.finished.first().at(1).toString(), QStringLiteral("board flashed, but ADB did not come back within 1 s; check the serial console"));
    QCOMPARE(rig.flasher.phase(), SocFlasher::Phase::Done);
    QCOMPARE(rig.boardInfo.count(), 0);
    QVERIFY(rig.runner.count(FlashRig::getState()) > 3);   // it polled
    QCOMPARE(rig.runner.count(FlashRig::boardInfoCommand()), 0);
    QVERIFY(rig.logged(QStringLiteral("ADB did not come back")));
}

void tst_firmwaretools::soc_adbComesBackLate()
{
    FlashRig rig;
    auto polls = std::make_shared<int>(0);
    rig.runner.addScript(FlashRig::ld(), kLdLoader);
    rig.runner.addHandler(FlashRig::getState(), [polls](FakeToolInvocation* i) {
        ++*polls;
        if (*polls >= 4) {
            i->emitOutput("device\r\n");
            i->finish(0);
        } else {
            i->emitOutput("error: no devices/emulators found\r\n");
            i->finish(1);
        }
    });
    rig.installFlashScripts();
    QVERIFY(rig.flasher.start());
    QTRY_COMPARE(rig.finished.count(), 1);
    QCOMPARE(rig.finished.first().at(0).toBool(), true);
    QCOMPARE(rig.finished.first().at(1).toString(), QStringLiteral("Upgrade firmware ok; board is back on ADB"));
    QCOMPARE(*polls, 4);
    QCOMPARE(rig.boardInfo.count(), 1);
}

void tst_firmwaretools::soc_waitForAdbOff()
{
    FlashRig rig;
    rig.runner.addScript(FlashRig::ld(), kLdLoader);
    rig.installFlashScripts();
    rig.flasher.setWaitForAdb(false);
    QVERIFY(!rig.flasher.waitForAdb());
    QVERIFY(rig.flasher.start());
    QTRY_COMPARE(rig.finished.count(), 1);
    QCOMPARE(rig.finished.first().at(0).toBool(), true);
    QCOMPARE(rig.finished.first().at(1).toString(), QStringLiteral("Upgrade firmware ok"));
    QCOMPARE(rig.runner.count(FlashRig::getState()), 0);   // NO_WAIT=1: nothing after the flash
    QCOMPARE(rig.boardInfo.count(), 0);
    using P = SocFlasher::Phase;
    QCOMPARE(rig.phaseList(), (QList<P>{P::Detecting, P::WaitingForLoader, P::Flashing, P::Done}));
}

void tst_firmwaretools::soc_boardInfoFailureIsStillSuccess()
{
    FlashRig rig;
    rig.runner.addScript(FlashRig::ld(), kLdLoader);
    rig.runner.addScript(FlashRig::boardInfoCommand(), "", 1);   // nothing to show
    rig.installFlashScripts();
    QVERIFY(rig.flasher.start());
    QTRY_COMPARE(rig.finished.count(), 1);
    QCOMPARE(rig.finished.first().at(0).toBool(), true);
    QCOMPARE(rig.boardInfo.count(), 0);
}

void tst_firmwaretools::soc_cancelWhenIdleAndRestart()
{
    FlashRig rig;
    rig.flasher.cancel();   // nothing runs: no signal
    QVERIFY(rig.finished.isEmpty());
    QVERIFY(rig.phases.isEmpty());
    QCOMPARE(rig.flasher.phase(), SocFlasher::Phase::Idle);
    QVERIFY(rig.flasher.lastStatus() == SocFlasher::DeviceStatus());

    // A finished flasher can run again, and a refresh works again.
    rig.runner.addScript(FlashRig::ld(), kLdLoader);
    rig.installFlashScripts();
    QVERIFY(rig.flasher.start());
    QTRY_COMPARE(rig.finished.count(), 1);
    rig.flasher.cancel();   // after Done: still nothing
    QCOMPARE(rig.finished.count(), 1);
    QCOMPARE(rig.flasher.phase(), SocFlasher::Phase::Done);
    const int statusesBefore = rig.statuses.count();
    rig.flasher.queryStatus();
    QTRY_COMPARE(rig.statuses.count(), statusesBefore + 1);
    QVERIFY(rig.flasher.start());
    QCOMPARE(rig.flasher.phase(), SocFlasher::Phase::Detecting);
    QTRY_COMPARE(rig.finished.count(), 2);
    QCOMPARE(rig.finished.last().at(0).toBool(), true);
    QCOMPARE(rig.runner.count(FlashRig::uf()), 2);
}

// ---------------------------------------------------------------------------------------------

int main(int argc, char** argv)
{
    // This executable doubles as the "tiny real process" the ProcessToolRunner tests start: it
    // exists on every platform and lets the tests control output, exit code, stdin and sleeping.
    if (argc >= 3 && std::string(argv[1]) == "--tool-helper") {
        return runToolHelper(argc, argv);
    }
    QCoreApplication app(argc, argv);
    app.setAttribute(Qt::AA_Use96Dpi, true);
    tst_firmwaretools test;
    return QTest::qExec(&test, argc, argv);
}

// ---------------------------------------------------------------------------------------------
// Real board probe. Runs only when SU_FIRMWARE_PROBE_SERIAL names an attached Node board (its
// adb serial); SU_FIRMWARE_PROBE_ADB / SU_FIRMWARE_PROBE_UPGRADE_TOOL give the tool paths (adb
// falls back to ToolLocator, i.e. PATH). Read-only on the SoC side: `upgrade_tool LD`,
// `adb devices`, then the CC1310 is reached through `rfmcu bridge` and asked for its version
// (McuUpdater::info() never enters the updater). A stale `rfmcu` on the board is ended, as every
// link open does. Nothing is written to the flash.
// ---------------------------------------------------------------------------------------------
void tst_firmwaretools::realBoard_statusAndMcuInfo()
{
    const QString serial = qEnvironmentVariable("SU_FIRMWARE_PROBE_SERIAL");
    if (serial.isEmpty()) {
        QSKIP("set SU_FIRMWARE_PROBE_SERIAL=<adb serial> (and SU_FIRMWARE_PROBE_UPGRADE_TOOL, SU_FIRMWARE_PROBE_ADB) to probe a real board");
    }
    const QString adb = ToolLocator::locate(ToolLocator::Tool::Adb, qEnvironmentVariable("SU_FIRMWARE_PROBE_ADB"));
    const QString upgradeTool = ToolLocator::locate(ToolLocator::Tool::UpgradeTool,
                                                    qEnvironmentVariable("SU_FIRMWARE_PROBE_UPGRADE_TOOL"));
    QVERIFY2(!adb.isEmpty(), "adb not found (SU_FIRMWARE_PROBE_ADB or PATH)");
    QVERIFY2(!upgradeTool.isEmpty(), "upgrade_tool not found (SU_FIRMWARE_PROBE_UPGRADE_TOOL)");
    qInfo() << "probe: adb" << adb << "upgrade_tool" << upgradeTool << "serial" << serial;

    ProcessToolRunner runner;

    // 1. The SoC status line the tab shows: an ADB device with that serial (the board runs).
    SocFlasher flasher(&runner);
    flasher.upgradeTool()->setToolPath(upgradeTool);
    flasher.adb()->setAdbPath(adb);
    flasher.setSerial(serial);
    QSignalSpy statuses(&flasher, &SocFlasher::statusChanged);
    QSignalSpy flasherLog(&flasher, &SocFlasher::logLine);
    flasher.queryStatus();
    QTRY_VERIFY_WITH_TIMEOUT(statuses.count() >= 1, 30000);
    const auto status = statuses.last().at(0).value<SocFlasher::DeviceStatus>();
    qInfo() << "probe: status" << status.text() << status.detail << status.hint;
    QCOMPARE(status.mode, SocFlasher::DeviceStatus::Mode::Adb);
    QCOMPARE(status.serial, serial);

    // 2. The MCU through the SoC: McuLink (killall rfmcu, forward, bridge, socket) + info().
    McuLink link(&runner);
    link.adb()->setAdbPath(adb);
    link.setSerial(serial);
    QSignalSpy opened(&link, &McuLink::opened);
    QSignalSpy failed(&link, &McuLink::failed);
    QSignalSpy closed(&link, &McuLink::closed);
    QSignalSpy linkLog(&link, &McuLink::logLine);
    QVERIFY(link.open());
    QTRY_VERIFY_WITH_TIMEOUT(opened.count() + failed.count() >= 1, 30000);
    for (const auto& row : linkLog) {
        qInfo() << "probe: link" << row.at(0).toString();
    }
    QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().at(0).toString()));
    QVERIFY(link.isOpen());
    QVERIFY(link.socket());
    qInfo() << "probe: bridge on local port" << link.localPort();

    McuUpdater updater;
    updater.setDevice(link.socket());
    updater.setThroughSoc(true);
    QSignalSpy finished(&updater, &McuUpdater::finished);
    QSignalSpy appInfo(&updater, &McuUpdater::applicationInfo);
    QSignalSpy devInfo(&updater, &McuUpdater::deviceInfo);
    QSignalSpy updaterLog(&updater, &McuUpdater::logLine);
    QVERIFY(updater.info());
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() == 1, 30000);
    for (const auto& row : updaterLog) {
        qInfo() << "probe: updater" << row.at(0).toString();
    }
    qInfo() << "probe: info finished" << finished.first().at(0).toBool() << finished.first().at(1).toString();
    QVERIFY2(finished.first().at(0).toBool(), qPrintable(finished.first().at(1).toString()));
    // A running application answers the CLI probe; a board left in the updater answers INFO.
    QVERIFY(appInfo.count() == 1 || devInfo.count() >= 1);
    if (appInfo.count() == 1) {
        const auto fields = appInfo.first().at(0).value<McuUpdater::CliFields>();
        qInfo() << "probe: application" << fields;
        QVERIFY(!fields.value(QStringLiteral("version")).isEmpty());
    }

    link.close();
    QTRY_VERIFY_WITH_TIMEOUT(closed.count() == 1, 30000);
    QVERIFY(!link.isOpen());
}

#include "tst_firmwaretools.moc"
