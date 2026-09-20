#include <QtTest>
#include <QSignalSpy>
#include <QSettings>

#include "app/AppSettings.h"
#include "core/DeviceSimulator.h"
#include "core/SerialConnection.h"
#include "core/SerialPortEnumerator.h"

namespace {

/// Accumulates everything a simulator (or connection) sends to the host.
struct Collector
{
    QByteArray data;

    explicit Collector(DeviceSimulator& sim)
    {
        QObject::connect(&sim, &DeviceSimulator::dataReady, [this](const QByteArray& bytes) { data += bytes; });
    }
    explicit Collector(SerialConnection& connection)
    {
        QObject::connect(&connection, &SerialConnection::dataReceived,
                         [this](const QByteArray& bytes) { data += bytes; });
    }
    bool contains(const char* text) const { return data.contains(text); }
    void clear() { data.clear(); }
};

/// Flushes the pending output and returns it (also delivered through dataReady()).
QByteArray flush(DeviceSimulator& sim)
{
    QSignalSpy spy(&sim, &DeviceSimulator::dataReady);
    sim.flushOutput();
    QByteArray all;
    for (const QList<QVariant>& args : spy) {
        all += args.at(0).toByteArray();
    }
    return all;
}

/// Sends `bytes` and returns everything the device answered (flushed).
QByteArray exchange(DeviceSimulator& sim, const QByteArray& bytes)
{
    sim.receive(bytes);
    return flush(sim);
}

const QByteArray kLinuxPrompt = QByteArrayLiteral("[root@rv1106:~]# ");

} // namespace

class Tst_devicesimulator : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase();
    void cleanupTestCase();

    // statics
    void portNames();
    void entries();
    void presence();

    // loopback + pacing
    void loopbackEchoes();
    void notStartedIgnoresInput();
    void pacingFollowsBaudRate();

    // Linux
    void linuxBootAndLogin();
    void linuxLineEditing();
    void linuxEscapeSequencesIgnored();
    void linuxCommands();
    void linuxPipes();
    void linuxUtf8Input();
    void linuxSleepAndProgress();
    void linuxLogout();
    void linuxLineTerminators();
    void linuxRebootVanishes();
    void linuxPoweroffStaysDown();

    // U-Boot
    void ubootCountdownAndPrompt();
    void ubootCountdownTicks();
    void ubootCommands();
    void ubootBootsToLinux();
    void ubootAutoboot();

    // MCU
    void mcuShell();
    void mcuAtCommands();
    void mcuTelemetry();
    void mcuResetAndVanish();

    // integration with SerialConnection / SerialPortEnumerator / AppSettings
    void connectionOpensLoopback();
    void connectionRejectsUnknownSimPort();
    void connectionReconnectsAfterSimulatedReset();
    void enumeratorListsSimulatedPorts();

private:
    static DeviceSimulator* loggedInLinux(QObject* parent);
    bool m_savedShowSimulated = true;
};

// ---------------------------------------------------------------------------------------

void Tst_devicesimulator::initTestCase()
{
    // Isolated QSettings scope so the tests never touch the real application's preferences.
    QCoreApplication::setOrganizationName(QStringLiteral("BuildAI-Tests"));
    QCoreApplication::setApplicationName(QStringLiteral("SerialUtility-tst_devicesimulator"));
    QSettings().clear();
    m_savedShowSimulated = AppSettings::instance().showSimulatedPorts();
}

void Tst_devicesimulator::cleanupTestCase()
{
    AppSettings::instance().setShowSimulatedPorts(m_savedShowSimulated);
    QSettings().clear();
    for (const SerialPortEntry& e : DeviceSimulator::entries()) {
        DeviceSimulator::markPresent(e.portName);
    }
}

DeviceSimulator* Tst_devicesimulator::loggedInLinux(QObject* parent)
{
    auto* sim = new DeviceSimulator(DeviceSimulator::Kind::Linux, 4000000, parent);
    sim->start();
    flush(*sim);
    exchange(*sim, "root\r");
    const QByteArray out = exchange(*sim, "toor\r");
    if (!out.endsWith(kLinuxPrompt)) {
        return nullptr;
    }
    return sim;
}

// ---------------------------------------------------------------------------------------
// statics
// ---------------------------------------------------------------------------------------

void Tst_devicesimulator::portNames()
{
    QCOMPARE(DeviceSimulator::portPrefix(), QStringLiteral("SIM:"));

    QVERIFY(DeviceSimulator::isSimulatedPort(QStringLiteral("SIM:linux")));
    QVERIFY(DeviceSimulator::isSimulatedPort(QStringLiteral("sim:LINUX")));
    QVERIFY(DeviceSimulator::isSimulatedPort(QStringLiteral("  SIM:mcu ")));
    QVERIFY(!DeviceSimulator::isSimulatedPort(QStringLiteral("COM8")));
    QVERIFY(!DeviceSimulator::isSimulatedPort(QStringLiteral("/dev/ttyUSB0")));
    QVERIFY(!DeviceSimulator::isSimulatedPort(QString()));
    QVERIFY(!DeviceSimulator::isSimulatedPort(QStringLiteral("SIMULATOR")));

    using Kind = DeviceSimulator::Kind;
    QCOMPARE(DeviceSimulator::kindFromPortName(QStringLiteral("SIM:loopback")).value(), Kind::Loopback);
    QCOMPARE(DeviceSimulator::kindFromPortName(QStringLiteral("SIM:linux")).value(), Kind::Linux);
    QCOMPARE(DeviceSimulator::kindFromPortName(QStringLiteral("SIM:uboot")).value(), Kind::UBoot);
    QCOMPARE(DeviceSimulator::kindFromPortName(QStringLiteral("SIM:mcu")).value(), Kind::Mcu);
    QCOMPARE(DeviceSimulator::kindFromPortName(QStringLiteral("sim:Linux")).value(), Kind::Linux);
    QVERIFY(!DeviceSimulator::kindFromPortName(QStringLiteral("SIM:nope")).has_value());
    QVERIFY(!DeviceSimulator::kindFromPortName(QStringLiteral("SIM:")).has_value());
    QVERIFY(!DeviceSimulator::kindFromPortName(QStringLiteral("COM8")).has_value());

    for (Kind kind : {Kind::Loopback, Kind::Linux, Kind::UBoot, Kind::Mcu}) {
        const QString name = DeviceSimulator::portName(kind);
        QVERIFY(name.startsWith(QStringLiteral("SIM:")));
        QCOMPARE(DeviceSimulator::kindFromPortName(name).value(), kind);
        QVERIFY(!DeviceSimulator::description(kind).isEmpty());
    }
    QCOMPARE(DeviceSimulator::portName(Kind::Linux), QStringLiteral("SIM:linux"));
    QVERIFY(DeviceSimulator::description(Kind::Linux) != DeviceSimulator::description(Kind::Mcu));
}

void Tst_devicesimulator::entries()
{
    const QList<SerialPortEntry> list = DeviceSimulator::entries();
    QCOMPARE(list.size(), 4);
    QCOMPARE(list.at(0).portName, QStringLiteral("SIM:loopback"));
    QCOMPARE(list.at(1).portName, QStringLiteral("SIM:linux"));
    QCOMPARE(list.at(2).portName, QStringLiteral("SIM:uboot"));
    QCOMPARE(list.at(3).portName, QStringLiteral("SIM:mcu"));
    for (const SerialPortEntry& e : list) {
        QCOMPARE(e.manufacturer, QStringLiteral("BuildAI Simulator"));
        QVERIFY(!e.description.isEmpty());
        QVERIFY(!e.hasVidPid);
        QVERIFY(e.displayText().startsWith(e.portName + QStringLiteral(" - ")));
        QVERIFY2(e.kindHint().contains(QStringLiteral("simulator"), Qt::CaseInsensitive), qPrintable(e.kindHint()));
        QVERIFY(e.toolTip().contains(QStringLiteral("BuildAI Simulator")));
    }
}

void Tst_devicesimulator::presence()
{
    const QString linux = QStringLiteral("SIM:linux");
    DeviceSimulator::markPresent(linux);
    QVERIFY(DeviceSimulator::isPresent(linux));
    QVERIFY(DeviceSimulator::isPresent(QStringLiteral("sim:LINUX")));   // case-insensitive
    QVERIFY(!DeviceSimulator::isPresent(QStringLiteral("SIM:nope")));
    QVERIFY(!DeviceSimulator::isPresent(QStringLiteral("COM8")));

    DeviceSimulator::markAbsent(linux, -1);
    QVERIFY(!DeviceSimulator::isPresent(linux));
    QTest::qWait(20);
    QVERIFY(!DeviceSimulator::isPresent(linux));   // stays down until markPresent()
    DeviceSimulator::markPresent(linux);
    QVERIFY(DeviceSimulator::isPresent(linux));

    DeviceSimulator::markAbsent(linux, 150);
    QVERIFY(!DeviceSimulator::isPresent(linux));
    QTRY_VERIFY_WITH_TIMEOUT(DeviceSimulator::isPresent(linux), 2000);
    QVERIFY(DeviceSimulator::isPresent(linux));   // the entry was dropped, still present

    // Other ports are unaffected.
    DeviceSimulator::markAbsent(QStringLiteral("SIM:mcu"), -1);
    QVERIFY(DeviceSimulator::isPresent(linux));
    QVERIFY(!DeviceSimulator::isPresent(QStringLiteral("SIM:mcu")));
    DeviceSimulator::markPresent(QStringLiteral("SIM:mcu"));
}

// ---------------------------------------------------------------------------------------
// loopback + pacing
// ---------------------------------------------------------------------------------------

void Tst_devicesimulator::loopbackEchoes()
{
    DeviceSimulator sim(DeviceSimulator::Kind::Loopback, 115200);
    QCOMPARE(sim.kind(), DeviceSimulator::Kind::Loopback);
    QCOMPARE(sim.baudRate(), 115200);
    QVERIFY(!sim.isStarted());
    sim.start();
    QVERIFY(sim.isStarted());
    QVERIFY(sim.pendingOutput().isEmpty());   // no banner

    QSignalSpy spy(&sim, &DeviceSimulator::dataReady);
    sim.receive(QByteArrayLiteral("hello \x1b[A \x03 \xE4\xB8\xAD"));
    QCOMPARE(sim.pendingOutput(), QByteArrayLiteral("hello \x1b[A \x03 \xE4\xB8\xAD"));
    QCOMPARE(spy.count(), 0);   // paced: nothing is delivered synchronously
    QVERIFY(spy.wait(500));
    QByteArray all;
    for (const QList<QVariant>& args : spy) {
        all += args.at(0).toByteArray();
    }
    QCOMPARE(all, QByteArrayLiteral("hello \x1b[A \x03 \xE4\xB8\xAD"));
    QVERIFY(sim.pendingOutput().isEmpty());

    // flushOutput() delivers everything at once.
    sim.receive(QByteArrayLiteral("abc"));
    QCOMPARE(flush(sim), QByteArrayLiteral("abc"));
    QVERIFY(flush(sim).isEmpty());
}

void Tst_devicesimulator::notStartedIgnoresInput()
{
    DeviceSimulator sim(DeviceSimulator::Kind::Loopback, 115200);
    sim.receive(QByteArrayLiteral("ignored"));
    QVERIFY(sim.pendingOutput().isEmpty());
    QVERIFY(flush(sim).isEmpty());

    DeviceSimulator mcu(DeviceSimulator::Kind::Mcu, 115200);
    mcu.receive(QByteArrayLiteral("help\r"));
    QVERIFY(flush(mcu).isEmpty());
}

void Tst_devicesimulator::pacingFollowsBaudRate()
{
    // 300 baud = 30 bytes/s -> at most a byte or two per 20 ms tick.
    DeviceSimulator slow(DeviceSimulator::Kind::Loopback, 300);
    slow.start();
    Collector slowOut(slow);
    slow.receive(QByteArray(100, 'x'));
    QTest::qWait(120);
    QVERIFY2(slowOut.data.size() >= 1, "first tick must deliver something");
    QVERIFY2(slowOut.data.size() < 30, qPrintable(QString::number(slowOut.data.size())));
    QCOMPARE(slowOut.data.size() + slow.pendingOutput().size(), 100);

    // A live baud rate change speeds the rest up.
    slow.setBaudRate(4000000);
    QCOMPARE(slow.baudRate(), 4000000);
    QTRY_COMPARE_WITH_TIMEOUT(slowOut.data.size(), 100, 1000);

    // 4 Mbaud delivers a kilobyte in the first tick.
    DeviceSimulator fast(DeviceSimulator::Kind::Loopback, 4000000);
    fast.start();
    Collector fastOut(fast);
    fast.receive(QByteArray(1000, 'y'));
    QTRY_COMPARE_WITH_TIMEOUT(fastOut.data.size(), 1000, 500);

    // Invalid rates are ignored.
    fast.setBaudRate(0);
    QCOMPARE(fast.baudRate(), 4000000);
    DeviceSimulator zero(DeviceSimulator::Kind::Loopback, -5);
    QCOMPARE(zero.baudRate(), 115200);
}

// ---------------------------------------------------------------------------------------
// Linux
// ---------------------------------------------------------------------------------------

void Tst_devicesimulator::linuxBootAndLogin()
{
    DeviceSimulator sim(DeviceSimulator::Kind::Linux, 115200);
    sim.start();
    const QByteArray boot = flush(sim);
    QVERIFY(boot.contains("Booting Linux on physical CPU 0x0"));
    QVERIFY(boot.contains("[    0.000000] Linux version 5.10.160"));
    QVERIFY(boot.contains("\x1b[32mOK\x1b[0m"));
    QVERIFY(boot.contains("\x1b[31mFAILED\x1b[0m"));
    QVERIFY(boot.contains(QString::fromUtf8("开发板").toUtf8()));   // one line of Chinese UTF-8
    QVERIFY(boot.count("[    ") >= 40);                               // ~40 kernel lines
    QVERIFY(boot.endsWith("rv1106 login: "));
    QVERIFY(!boot.contains('\n' + QByteArray()) || boot.count("\r\n") > 40);   // CRLF line endings
    QVERIFY(!QByteArray(boot).replace("\r\n", "").contains('\n'));            // no bare LF

    // Empty user name -> prompt again.
    QCOMPARE(exchange(sim, "\r"), QByteArrayLiteral("\r\nrv1106 login: "));

    // User name is echoed, the password is not.
    QCOMPARE(exchange(sim, "root\r"), QByteArrayLiteral("root\r\nPassword: "));
    const QByteArray login = exchange(sim, "s3cret\r");
    QVERIFY(!login.contains("s3cret"));
    QVERIFY(login.startsWith("\r\n"));
    QVERIFY(login.contains("Welcome to BuildAI Linux (rv1106)"));
    QVERIFY(login.endsWith(kLinuxPrompt));
}

void Tst_devicesimulator::linuxLineEditing()
{
    QObject parent;
    DeviceSimulator* sim = loggedInLinux(&parent);
    QVERIFY(sim);

    // Typing is echoed by the device.
    QCOMPARE(exchange(*sim, "unamx"), QByteArrayLiteral("unamx"));
    // DEL erases with "\b \b" ...
    QCOMPARE(exchange(*sim, "\x7f"), QByteArrayLiteral("\b \b"));
    // ... and BS too; the executed line is what is left in the device's buffer.
    QCOMPARE(exchange(*sim, "\x08"), QByteArrayLiteral("\b \b"));
    QByteArray out = exchange(*sim, "me -a\r");
    QVERIFY2(out.contains("Linux rv1106 5.10.160"), out.constData());
    QVERIFY(out.endsWith(kLinuxPrompt));

    // Backspace on an empty line does nothing.
    QVERIFY(exchange(*sim, "\x7f").isEmpty());

    // Ctrl+U clears the whole line.
    exchange(*sim, "garbage");
    out = exchange(*sim, "\x15");
    QCOMPARE(out.count("\b \b"), 7);
    out = exchange(*sim, "\r");
    QCOMPARE(out, QByteArrayLiteral("\r\n") + kLinuxPrompt);   // empty command: just a new prompt

    // Ctrl+C prints ^C and a fresh prompt, discarding the line.
    exchange(*sim, "abc");
    out = exchange(*sim, "\x03");
    QCOMPARE(out, QByteArrayLiteral("^C\r\n") + kLinuxPrompt);
    out = exchange(*sim, "pwd\r");
    QVERIFY(out.contains("\r\n/root\r\n"));   // "abc" was not prepended

    // Ctrl+L clears the screen and redraws the prompt with the pending text.
    exchange(*sim, "ls");
    out = exchange(*sim, "\x0c");
    QCOMPARE(out, QByteArrayLiteral("\x1b[H\x1b[2J") + kLinuxPrompt + QByteArrayLiteral("ls"));
    exchange(*sim, "\x15");

    // Tab is ignored, unknown commands report "not found".
    out = exchange(*sim, "fo\to\r");
    QVERIFY2(out.contains("-sh: foo: not found"), out.constData());
}

void Tst_devicesimulator::linuxEscapeSequencesIgnored()
{
    QObject parent;
    DeviceSimulator* sim = loggedInLinux(&parent);
    QVERIFY(sim);
    // Arrow keys in both normal and application mode, a function key and an Alt-letter.
    QVERIFY(exchange(*sim, "\x1b[A\x1b[B\x1bOC\x1b[15~\x1bx").isEmpty());
    const QByteArray out = exchange(*sim, "pwd\r");
    QCOMPARE(out, QByteArrayLiteral("pwd\r\n/root\r\n") + kLinuxPrompt);
}

void Tst_devicesimulator::linuxCommands()
{
    QObject parent;
    DeviceSimulator* sim = loggedInLinux(&parent);
    QVERIFY(sim);

    QByteArray out = exchange(*sim, "cd /oem\r");
    QVERIFY(out.endsWith("[root@rv1106:/oem]# "));
    QVERIFY(exchange(*sim, "pwd\r").contains("\r\n/oem\r\n"));
    out = exchange(*sim, "cd nowhere\r");
    QVERIFY2(out.contains("can't cd to nowhere"), out.constData());
    out = exchange(*sim, "cd ..\r");
    QVERIFY(out.endsWith("[root@rv1106:/]# "));
    out = exchange(*sim, "cd\r");
    QVERIFY(out.endsWith(kLinuxPrompt));
    out = exchange(*sim, "cd ~/../etc\r");
    QVERIFY(out.endsWith("[root@rv1106:/etc]# "));
    QVERIFY(exchange(*sim, "cat hostname\r").contains("\r\nrv1106\r\n"));   // relative path
    exchange(*sim, "cd\r");

    QVERIFY(exchange(*sim, "cat /proc/cpuinfo\r").contains("ARMv7 Processor rev 5"));
    QVERIFY(exchange(*sim, "cat /proc/meminfo\r").contains("MemTotal:          62896 kB"));
    QVERIFY(exchange(*sim, "cat /proc/version\r").contains("Linux version 5.10.160"));
    out = exchange(*sim, "cat /nope\r");
    QVERIFY2(out.contains("cat: can't open '/nope': No such file or directory"), out.constData());
    QVERIFY(exchange(*sim, "cat /etc\r").contains("Is a directory"));

    out = exchange(*sim, "ls /dev\r");
    QVERIFY2(out.contains("ttyFIQ0"), out.constData());
    QVERIFY(out.contains("video0"));
    out = exchange(*sim, "ls -l /dev/tty* /dev/video* 2>/dev/null\r");   // shipped quick command
    QVERIFY2(out.contains("/dev/ttyS2"), out.constData());
    QVERIFY(out.contains("/dev/video0"));
    QVERIFY(out.contains("crw"));
    QVERIFY(!out.contains("not found"));
    out = exchange(*sim, "ls -l /\r");
    QVERIFY(out.contains("drwxr-xr-x"));
    QVERIFY(out.contains(" oem\r\n"));
    out = exchange(*sim, "ls /missing\r");
    QVERIFY(out.contains("ls: /missing: No such file or directory"));

    QVERIFY(exchange(*sim, "df -h\r").contains("/dev/mmcblk0p7"));
    QVERIFY(exchange(*sim, "free\r").contains("Mem:"));
    QVERIFY(exchange(*sim, "ifconfig\r").contains("inet addr:192.168.1.120"));
    QVERIFY(exchange(*sim, "ip addr\r").contains("192.168.1.120/24"));
    QVERIFY(exchange(*sim, "ps\r").contains("rkipc"));
    out = exchange(*sim, "uname -r\r");
    QVERIFY(out.contains("\r\n5.10.160\r\n"));
    QVERIFY(exchange(*sim, "hostname\r").contains("\r\nrv1106\r\n"));
    QVERIFY(exchange(*sim, "whoami\r").contains("\r\nroot\r\n"));
    QVERIFY(exchange(*sim, "id\r").contains("uid=0(root)"));
    QVERIFY(exchange(*sim, "date\r").contains(QByteArray::number(QDate::currentDate().year())));
    QVERIFY(exchange(*sim, "uptime\r").contains("load average"));
    QVERIFY(exchange(*sim, "help\r").contains("Built-in commands"));

    // echo / environment
    QVERIFY(exchange(*sim, "echo hi $HOME there\r").contains("\r\nhi /root there\r\n"));
    exchange(*sim, "export FOO=bar\r");
    QVERIFY(exchange(*sim, "echo ${FOO}!\r").contains("\r\nbar!\r\n"));
    QVERIFY(exchange(*sim, "env\r").contains("FOO=bar\r\n"));
    QVERIFY(exchange(*sim, "export\r").contains("export PATH='/bin:/sbin"));

    // stty is acknowledged silently; "stty size" reports rows cols.
    QCOMPARE(exchange(*sim, "stty cols 132 rows 43\r"), QByteArrayLiteral("stty cols 132 rows 43\r\n") + kLinuxPrompt);
    QVERIFY(exchange(*sim, "stty size\r").contains("\r\n43 132\r\n"));

    // Screen-oriented output.
    out = exchange(*sim, "top -n 1\r");
    QVERIFY(out.contains("\x1b[H\x1b[2J"));
    QVERIFY(out.contains("\x1b[4;1H"));
    QVERIFY(out.contains("rkipc"));
    QVERIFY(out.endsWith(kLinuxPrompt));
    out = exchange(*sim, "clear\r");
    QCOMPARE(out, QByteArrayLiteral("clear\r\n\x1b[H\x1b[2J") + kLinuxPrompt);
    out = exchange(*sim, "color\r");
    QVERIFY(out.contains("\x1b[48;5;16m"));
    QVERIFY(out.contains("\x1b[48;2;"));
    QVERIFY(out.contains("\x1b[1mbold\x1b[0m"));
    out = exchange(*sim, "chinese\r");
    QVERIFY(out.contains(QString::fromUtf8("欢迎使用 BuildAI 串口工具").toUtf8()));
    out = exchange(*sim, "wide\r");
    QVERIFY(out.contains(QString::fromUtf8("┌").toUtf8()));
    QVERIFY(out.contains(QString::fromUtf8("你好世界").toUtf8()));

    // Empty line and "true" produce just a prompt.
    QCOMPARE(exchange(*sim, "\r"), QByteArrayLiteral("\r\n") + kLinuxPrompt);
    QCOMPARE(exchange(*sim, "true\r"), QByteArrayLiteral("true\r\n") + kLinuxPrompt);
}

void Tst_devicesimulator::linuxPipes()
{
    QObject parent;
    DeviceSimulator* sim = loggedInLinux(&parent);
    QVERIFY(sim);

    QByteArray out = exchange(*sim, "dmesg\r");
    QVERIFY(out.contains("Booting Linux on physical CPU"));
    QVERIFY(out.contains("\x1b[33m"));   // coloured warning line
    QVERIFY(out.count("\r\n") > 60);

    out = exchange(*sim, "dmesg | tail -n 3\r");
    QVERIFY(!out.contains("Booting Linux on physical CPU"));
    QVERIFY2(out.contains("no sensor found"), out.constData());
    QVERIFY(out.contains("random: crng init done"));
    // echo + 3 lines + prompt -> exactly 4 CRLFs
    QCOMPARE(out.count("\r\n"), 4);

    out = exchange(*sim, "dmesg | head -n 2\r");
    QCOMPARE(out.count("\r\n"), 3);
    QVERIFY(out.contains("Booting Linux on physical CPU"));

    out = exchange(*sim, "dmesg | grep mmc\r");
    QVERIFY(out.contains("mmcblk0"));
    QVERIFY(!out.contains("Booting Linux"));

    out = exchange(*sim, "dmesg | tail -n 50\r");   // the shipped quick command
    QCOMPARE(out.count("\r\n"), 51);

    out = exchange(*sim, "ps | wc -l\r");
    QVERIFY(out.contains("\r\n18\r\n"));
}

void Tst_devicesimulator::linuxUtf8Input()
{
    QObject parent;
    DeviceSimulator* sim = loggedInLinux(&parent);
    QVERIFY(sim);

    const QByteArray hello = QString::fromUtf8("你好").toUtf8();
    // Typed multi-byte characters are echoed once complete, then echoed back by `echo`.
    QByteArray out = exchange(*sim, "echo " + hello + "\r");
    QCOMPARE(out.count(hello), 2);

    // Bytes split across receive() calls still form the characters: nothing is echoed for a
    // partial sequence, the rest of the chunk completes both characters.
    sim->receive(hello.left(2));
    QVERIFY(flush(*sim).isEmpty());
    QCOMPARE(exchange(*sim, hello.mid(2)), hello);

    // Backspace over a wide (2-cell) character erases two cells - once per character.
    QCOMPARE(exchange(*sim, "\x7f"), QByteArrayLiteral("\b\b  \b\b"));
    QCOMPARE(exchange(*sim, "\x7f"), QByteArrayLiteral("\b\b  \b\b"));
    QVERIFY(exchange(*sim, "\x7f").isEmpty());   // line is empty again

    // A stray continuation byte and an invalid sequence are dropped silently.
    QVERIFY(exchange(*sim, "\x80").isEmpty());
    QVERIFY(exchange(*sim, "\xC3\x28").isEmpty());
    out = exchange(*sim, "\r");
    QCOMPARE(out, QByteArrayLiteral("\r\n") + kLinuxPrompt);
}

void Tst_devicesimulator::linuxSleepAndProgress()
{
    QObject parent;
    DeviceSimulator* sim = loggedInLinux(&parent);
    QVERIFY(sim);

    // sleep: no prompt until the delay elapsed; typed bytes are ignored meanwhile.
    QByteArray out = exchange(*sim, "sleep 1\r");
    QCOMPARE(out, QByteArrayLiteral("sleep 1\r\n"));
    QVERIFY(exchange(*sim, "x").isEmpty());
    Collector collected(*sim);
    QTRY_VERIFY_WITH_TIMEOUT(collected.data.endsWith(kLinuxPrompt), 3000);
    QVERIFY(!collected.data.contains('x'));

    // sleep can be interrupted with Ctrl+C.
    exchange(*sim, "sleep 20\r");
    out = exchange(*sim, "\x03");
    QCOMPARE(out, QByteArrayLiteral("^C\r\n") + kLinuxPrompt);
    QVERIFY(exchange(*sim, "sleep abc\r").contains("invalid number"));

    // progress: \r-driven bar delivered over time (paced); Ctrl+C aborts it.
    Collector progress(*sim);
    sim->receive("progress\r");
    QTRY_VERIFY_WITH_TIMEOUT(progress.data.count("\rDownloading firmware  [") >= 3, 2000);
    QVERIFY(progress.data.contains("] "));
    QVERIFY(!progress.data.contains("Download complete"));
    out = exchange(*sim, "\x03");
    QVERIFY(out.contains("^C\r\n"));
    QVERIFY(out.endsWith(kLinuxPrompt));
    QVERIFY(exchange(*sim, "pwd\r").contains("/root"));   // shell is usable again
}

void Tst_devicesimulator::linuxLogout()
{
    QObject parent;
    DeviceSimulator* sim = loggedInLinux(&parent);
    QVERIFY(sim);

    QByteArray out = exchange(*sim, "exit\r");
    QVERIFY(out.contains("logout"));
    QVERIFY(out.endsWith("rv1106 login: "));
    exchange(*sim, "admin\r");
    out = exchange(*sim, "\r");
    QVERIFY(out.endsWith(kLinuxPrompt));
    QVERIFY(exchange(*sim, "whoami\r").contains("\r\nadmin\r\n"));

    // Ctrl+D at an empty line logs out; with text pending it does nothing.
    exchange(*sim, "abc");
    QVERIFY(exchange(*sim, "\x04").isEmpty());
    exchange(*sim, "\x15");
    out = exchange(*sim, "\x04");
    QVERIFY(out.endsWith("rv1106 login: "));
}

void Tst_devicesimulator::linuxLineTerminators()
{
    QObject parent;
    DeviceSimulator* sim = loggedInLinux(&parent);
    QVERIFY(sim);

    // CRLF executes once (the LF is swallowed).
    QByteArray out = exchange(*sim, "pwd\r\n");
    QCOMPARE(out.count("/root\r\n"), 1);
    QCOMPARE(out.count(kLinuxPrompt), 1);
    // A bare LF executes as well (many MCU tools send LF).
    out = exchange(*sim, "pwd\n");
    QCOMPARE(out.count("/root\r\n"), 1);
    // CR, then unrelated byte, then LF: the LF is a real line terminator.
    out = exchange(*sim, "pwd\rx\n");
    QCOMPARE(out.count("/root\r\n"), 1);
    QVERIFY(out.contains("-sh: x: not found"));
}

void Tst_devicesimulator::linuxRebootVanishes()
{
    QObject parent;
    DeviceSimulator* sim = loggedInLinux(&parent);
    QVERIFY(sim);
    DeviceSimulator::markPresent(QStringLiteral("SIM:linux"));

    QSignalSpy vanished(sim, &DeviceSimulator::vanished);
    QByteArray out = exchange(*sim, "reboot\r");
    QVERIFY(out.contains("The system is going down for reboot NOW!"));
    QVERIFY(out.contains("reboot: Restarting system"));
    QCOMPARE(vanished.count(), 0);   // the message gets a head start
    QVERIFY(vanished.wait(2000));
    QCOMPARE(vanished.count(), 1);
    QCOMPARE(vanished.first().first().toInt(), 3000);
    QVERIFY(!DeviceSimulator::isPresent(QStringLiteral("SIM:linux")));

    // A device that is down ignores input.
    QVERIFY(exchange(*sim, "pwd\r").isEmpty());

    DeviceSimulator::markPresent(QStringLiteral("SIM:linux"));
    QVERIFY(DeviceSimulator::isPresent(QStringLiteral("SIM:linux")));
}

void Tst_devicesimulator::linuxPoweroffStaysDown()
{
    QObject parent;
    DeviceSimulator* sim = loggedInLinux(&parent);
    QVERIFY(sim);
    QSignalSpy vanished(sim, &DeviceSimulator::vanished);
    QVERIFY(exchange(*sim, "poweroff\r").contains("system halt NOW!"));
    QVERIFY(vanished.wait(2000));
    QCOMPARE(vanished.first().first().toInt(), -1);
    QVERIFY(!DeviceSimulator::isPresent(QStringLiteral("SIM:linux")));
    QTest::qWait(30);
    QVERIFY(!DeviceSimulator::isPresent(QStringLiteral("SIM:linux")));
    DeviceSimulator::markPresent(QStringLiteral("SIM:linux"));
    QVERIFY(DeviceSimulator::isPresent(QStringLiteral("SIM:linux")));
}

// ---------------------------------------------------------------------------------------
// U-Boot
// ---------------------------------------------------------------------------------------

void Tst_devicesimulator::ubootCountdownAndPrompt()
{
    DeviceSimulator sim(DeviceSimulator::Kind::UBoot, 115200);
    sim.start();
    const QByteArray banner = flush(sim);
    QVERIFY(banner.contains("U-Boot 2017.09"));
    QVERIFY(banner.contains("DRAM:  64 MiB"));
    QVERIFY(banner.endsWith("Hit any key to stop autoboot:  3"));

    // Any byte stops the countdown; the byte itself is discarded.
    const QByteArray out = exchange(sim, "q");
    QCOMPARE(out, QByteArrayLiteral("\r\n=> "));
    QCOMPARE(exchange(sim, "\r"), QByteArrayLiteral("\r\n=> "));   // "q" was not buffered
    QTest::qWait(1100);
    QVERIFY(flush(sim).isEmpty());   // no more countdown output
}

void Tst_devicesimulator::ubootCountdownTicks()
{
    DeviceSimulator sim(DeviceSimulator::Kind::UBoot, 115200);
    Collector out(sim);
    sim.start();
    QTRY_VERIFY_WITH_TIMEOUT(out.data.contains("autoboot:  3"), 2000);
    QVERIFY(!out.data.contains("\b2"));
    QTRY_VERIFY_WITH_TIMEOUT(out.data.contains("\b2"), 2500);
    QVERIFY(!out.data.contains("\b1"));
    sim.receive(" ");
    QTRY_VERIFY_WITH_TIMEOUT(out.data.endsWith("=> "), 1000);
}

void Tst_devicesimulator::ubootCommands()
{
    DeviceSimulator sim(DeviceSimulator::Kind::UBoot, 4000000);
    sim.start();
    flush(sim);
    exchange(sim, " ");

    QByteArray out = exchange(sim, "printenv bootcmd\r");
    QCOMPARE(out, QByteArrayLiteral("printenv bootcmd\r\nbootcmd=boot_fit\r\n=> "));
    out = exchange(sim, "printenv\r");
    QVERIFY(out.contains("baudrate=115200\r\n"));
    QVERIFY(out.contains("Environment size:"));
    QVERIFY(out.indexOf("arch=arm") < out.indexOf("vendor=rockchip"));   // sorted
    exchange(sim, "setenv foo bar baz\r");
    QVERIFY(exchange(sim, "printenv foo\r").contains("foo=bar baz\r\n"));
    exchange(sim, "setenv foo\r");
    QVERIFY(exchange(sim, "printenv foo\r").contains("## Error: \"foo\" not defined"));
    QVERIFY(exchange(sim, "saveenv\r").contains("Saving Environment to MMC... Writing to MMC(0)... OK"));

    QVERIFY(exchange(sim, "bdinfo\r").contains("relocaddr   = 0x03b2c000"));
    QVERIFY(exchange(sim, "mmc info\r").contains("Capacity: 7.4 GiB"));
    QVERIFY(exchange(sim, "mmc list\r").contains("dwmmc@ffaa0000: 0 (SD)"));
    QVERIFY(exchange(sim, "mmc part\r").contains("\"rootfs\""));
    out = exchange(sim, "md 02008000\r");
    QVERIFY(out.contains("\r\n02008000: "));
    QVERIFY(out.contains("\r\n02008030: "));
    QCOMPARE(out.count("\r\n"), 5);
    QVERIFY(exchange(sim, "md zz\r").contains("Usage:"));
    QVERIFY(exchange(sim, "version\r").contains("U-Boot 2017.09"));
    QVERIFY(exchange(sim, "help\r").contains("printenv- print environment variables"));
    QVERIFY(exchange(sim, "echo hi there\r").contains("\r\nhi there\r\n"));
    out = exchange(sim, "xyz\r");
    QVERIFY2(out.contains("Unknown command 'xyz' - try 'help'"), out.constData());
    QVERIFY(out.endsWith("=> "));
    QCOMPARE(exchange(sim, "\r"), QByteArrayLiteral("\r\n=> "));

    // Line editing works at the U-Boot prompt as well.
    exchange(sim, "helx");
    QCOMPARE(exchange(sim, "\x7f"), QByteArrayLiteral("\b \b"));
    QVERIFY(exchange(sim, "p\r").contains("bdinfo  - print Board Info structure"));

    // reset -> banner and countdown again.
    out = exchange(sim, "reset\r");
    QVERIFY(out.contains("resetting ..."));
    QVERIFY(out.contains("U-Boot 2017.09"));
    QVERIFY(out.endsWith("Hit any key to stop autoboot:  3"));
}

void Tst_devicesimulator::ubootBootsToLinux()
{
    DeviceSimulator sim(DeviceSimulator::Kind::UBoot, 4000000);
    sim.start();
    flush(sim);
    exchange(sim, " ");
    QByteArray out = exchange(sim, "boot\r");
    QVERIFY(out.contains("## Booting kernel from Legacy Image at 02008000 ..."));
    QVERIFY(out.contains("Starting kernel ..."));
    QVERIFY(out.contains("Booting Linux on physical CPU 0x0"));
    QVERIFY(out.endsWith("rv1106 login: "));

    // Now it behaves like the Linux console.
    exchange(sim, "root\r");
    QVERIFY(exchange(sim, "x\r").endsWith(kLinuxPrompt));
    QVERIFY(exchange(sim, "uname -a\r").contains("Linux rv1106"));
    QVERIFY(exchange(sim, "printenv\r").contains("-sh: printenv: not found"));

    // A Linux reboot of the U-Boot kind vanishes like the Linux kind does.
    QSignalSpy vanished(&sim, &DeviceSimulator::vanished);
    exchange(sim, "reboot\r");
    QVERIFY(vanished.wait(2000));
    DeviceSimulator::markPresent(QStringLiteral("SIM:uboot"));
}

void Tst_devicesimulator::ubootAutoboot()
{
    DeviceSimulator sim(DeviceSimulator::Kind::UBoot, 4000000);
    Collector out(sim);
    sim.start();
    // Not interrupted: 3, 2, 1, 0 then the kernel boots and the login prompt appears.
    QTRY_VERIFY_WITH_TIMEOUT(out.data.contains("rv1106 login: "), 9000);
    QVERIFY(out.data.contains("\b0\r\n"));
    QVERIFY(out.data.contains("Starting kernel ..."));
}

// ---------------------------------------------------------------------------------------
// MCU
// ---------------------------------------------------------------------------------------

void Tst_devicesimulator::mcuShell()
{
    DeviceSimulator sim(DeviceSimulator::Kind::Mcu, 115200);
    sim.start();
    const QByteArray banner = flush(sim);
    QVERIFY(banner.contains("BuildAI MCU shell v1.0 (STM32F4 @168MHz)"));
    QVERIFY(banner.endsWith("> "));

    QByteArray out = exchange(sim, "help\r");
    QVERIFY(out.contains("telemetry on|off"));
    QVERIFY(out.endsWith("> "));
    QVERIFY(exchange(sim, "version\r").contains("BuildAI MCU shell v1.0"));
    QVERIFY(exchange(sim, "led on\r").contains("LED is ON"));
    QVERIFY(exchange(sim, "led toggle\r").contains("LED is OFF"));
    QVERIFY(exchange(sim, "led\r").contains("usage: led on|off|toggle"));
    out = exchange(sim, "adc\r");
    QCOMPARE(out.count("ADC"), 8);
    QVERIFY(out.contains("ADC7:"));
    QVERIFY(exchange(sim, "temp\r").contains("Temperature: 3"));
    QVERIFY(exchange(sim, "uptime\r").contains("Uptime: "));
    QVERIFY(exchange(sim, "echo a  b\r").contains("\r\na b\r\n"));
    out = exchange(sim, "bogus\r");
    QVERIFY2(out.contains("Unknown command: bogus (try 'help')"), out.constData());

    // LF-terminated lines are accepted, CRLF executes once.
    out = exchange(sim, "version\n");
    QCOMPARE(out.count("BuildAI MCU shell v1.0"), 1);
    out = exchange(sim, "version\r\n");
    QCOMPARE(out.count("BuildAI MCU shell v1.0"), 1);
    QCOMPARE(out.count("> "), 1);

    // Ctrl+C / Ctrl+D / Ctrl+L mean nothing to the MCU shell.
    QVERIFY(exchange(sim, "\x03\x04\x0c").isEmpty());
}

void Tst_devicesimulator::mcuAtCommands()
{
    DeviceSimulator sim(DeviceSimulator::Kind::Mcu, 115200);
    sim.start();
    flush(sim);

    QCOMPARE(exchange(sim, "AT\r\n"), QByteArrayLiteral("AT\r\nOK\r\n> "));
    QByteArray out = exchange(sim, "at+gmr\r");
    QVERIFY(out.contains("SDK version: v1.0.0-buildai"));
    QVERIFY(out.endsWith("OK\r\n> "));
    QVERIFY(exchange(sim, "AT+FOO=1\r").contains("\r\nERROR\r\n"));

    // ATE0 turns the echo off: the typed command is not repeated.
    QVERIFY(exchange(sim, "ATE0\r").contains("OK"));
    QCOMPARE(exchange(sim, "AT\r"), QByteArrayLiteral("OK\r\n> "));
    QVERIFY(exchange(sim, "AT\x7f\x7f").isEmpty());   // no echo of typing or erasing either
    QCOMPARE(exchange(sim, "ATE1\r"), QByteArrayLiteral("OK\r\n> "));
    QCOMPARE(exchange(sim, "AT\r"), QByteArrayLiteral("AT\r\nOK\r\n> "));
}

void Tst_devicesimulator::mcuTelemetry()
{
    DeviceSimulator sim(DeviceSimulator::Kind::Mcu, 115200);
    sim.start();
    flush(sim);
    Collector out(sim);
    QVERIFY(exchange(sim, "telemetry on\r").contains("Telemetry enabled"));
    out.clear();
    QTRY_VERIFY_WITH_TIMEOUT(out.data.contains("temp="), 4000);
    QVERIFY(out.data.contains("vbat="));
    QVERIFY(out.data.contains("dBm"));
    QVERIFY(exchange(sim, "telemetry\r").contains("Telemetry is on"));
    QVERIFY(exchange(sim, "telemetry off\r").contains("Telemetry disabled"));
    out.clear();
    QTest::qWait(2300);
    QVERIFY(!out.data.contains("temp="));
}

void Tst_devicesimulator::mcuResetAndVanish()
{
    DeviceSimulator sim(DeviceSimulator::Kind::Mcu, 4000000);
    sim.start();
    flush(sim);
    QByteArray out = exchange(sim, "reset\r");
    QVERIFY(out.contains("Resetting..."));
    QVERIFY(out.contains("BuildAI MCU shell v1.0"));
    QVERIFY(out.endsWith("> "));

    QSignalSpy vanished(&sim, &DeviceSimulator::vanished);
    out = exchange(sim, "AT+RST\r");
    QVERIFY(out.contains("OK"));
    QVERIFY(vanished.wait(2000));
    QCOMPARE(vanished.first().first().toInt(), 1500);
    QVERIFY(!DeviceSimulator::isPresent(QStringLiteral("SIM:mcu")));
    QTRY_VERIFY_WITH_TIMEOUT(DeviceSimulator::isPresent(QStringLiteral("SIM:mcu")), 4000);
}

// ---------------------------------------------------------------------------------------
// Integration
// ---------------------------------------------------------------------------------------

void Tst_devicesimulator::connectionOpensLoopback()
{
    SerialConnection c;
    SerialSettings s;
    s.portName = QStringLiteral("SIM:loopback");
    QVERIFY(s.isSimulatedPort());
    s.baudRate = 4000000;
    c.setSettings(s);

    QSignalSpy stateSpy(&c, &SerialConnection::stateChanged);
    QSignalSpy errorSpy(&c, &SerialConnection::errorOccurred);
    QSignalSpy sentSpy(&c, &SerialConnection::dataSent);
    QSignalSpy pinSpy(&c, &SerialConnection::pinsChanged);
    Collector rx(c);

    QVERIFY(c.open());
    QVERIFY(c.isOpen());
    QCOMPARE(c.state(), SerialConnection::State::Connected);
    QCOMPARE(stateSpy.count(), 1);
    QCOMPARE(pinSpy.count(), 1);
    QCOMPARE(errorSpy.count(), 0);
    QVERIFY(c.open());   // idempotent

    QCOMPARE(c.write(QByteArrayLiteral("abc")), qint64(3));
    QCOMPARE(sentSpy.count(), 1);
    QCOMPARE(c.bytesSent(), quint64(3));
    QTRY_COMPARE_WITH_TIMEOUT(rx.data, QByteArrayLiteral("abc"), 1000);
    QCOMPARE(c.bytesReceived(), quint64(3));

    // Live settings changes and pin toggles are accepted without errors.
    s.baudRate = 9600;
    s.dtr = false;
    c.setSettings(s);
    QCOMPARE(pinSpy.count(), 2);
    c.setRts(false);
    c.sendBreak(10);
    c.clearBuffers();
    QCOMPARE(errorSpy.count(), 0);

    c.close();
    QCOMPARE(c.state(), SerialConnection::State::Disconnected);
    QCOMPARE(c.write(QByteArrayLiteral("x")), qint64(-1));
    QCOMPARE(errorSpy.count(), 0);
    QCoreApplication::processEvents();   // deleteLater of the simulator

    // Reopen works and starts a fresh device.
    QVERIFY(c.open());
    c.close();
}

void Tst_devicesimulator::connectionRejectsUnknownSimPort()
{
    SerialConnection c;
    SerialSettings s;
    s.portName = QStringLiteral("SIM:nope");
    c.setSettings(s);
    QSignalSpy errorSpy(&c, &SerialConnection::errorOccurred);
    QVERIFY(!c.open());
    QCOMPARE(c.state(), SerialConnection::State::Disconnected);
    QCOMPARE(errorSpy.count(), 1);
    const QString message = errorSpy.first().first().toString();
    QVERIFY2(message.contains(QStringLiteral("SIM:nope")), qPrintable(message));
    QVERIFY(message.contains(QStringLiteral("not found")));
    QCOMPARE(c.errorString(), message);
}

void Tst_devicesimulator::connectionReconnectsAfterSimulatedReset()
{
    SerialConnection c;
    SerialSettings s;
    s.portName = QStringLiteral("SIM:mcu");
    s.baudRate = 4000000;
    c.setSettings(s);
    c.setReconnectIntervalMs(200);

    QSignalSpy disappeared(&c, &SerialConnection::portDisappeared);
    QSignalSpy reconnected(&c, &SerialConnection::reconnected);
    QSignalSpy errors(&c, &SerialConnection::errorOccurred);
    Collector rx(c);

    QVERIFY(c.open());
    QTRY_VERIFY_WITH_TIMEOUT(rx.data.endsWith("> "), 2000);
    rx.clear();

    QCOMPARE(c.write(QByteArrayLiteral("AT+RST\r")), qint64(7));
    QTRY_COMPARE_WITH_TIMEOUT(disappeared.count(), 1, 3000);
    QCOMPARE(disappeared.first().first().toString(), QStringLiteral("SIM:mcu"));
    QCOMPARE(c.state(), SerialConnection::State::Reconnecting);
    QVERIFY(!c.isOpen());
    QCOMPARE(errors.count(), 1);
    QVERIFY(errors.first().first().toString().contains(QStringLiteral("disconnected")));
    QCOMPARE(c.write(QByteArrayLiteral("x")), qint64(-1));
    QVERIFY(!DeviceSimulator::isPresent(QStringLiteral("SIM:mcu")));

    // The device comes back after 1.5 s and the connection reopens it (new banner).
    rx.clear();
    QTRY_COMPARE_WITH_TIMEOUT(reconnected.count(), 1, 6000);
    QCOMPARE(c.state(), SerialConnection::State::Connected);
    QTRY_VERIFY_WITH_TIMEOUT(rx.data.contains("BuildAI MCU shell v1.0"), 2000);
    QCOMPARE(errors.count(), 1);   // reconnect attempts never spam errorOccurred()

    // Cancelling a reconnect: open again, vanish again, then close() while Reconnecting.
    QCOMPARE(c.write(QByteArrayLiteral("AT+RST\r")), qint64(7));
    QTRY_COMPARE_WITH_TIMEOUT(disappeared.count(), 2, 3000);
    c.close();
    QCOMPARE(c.state(), SerialConnection::State::Disconnected);
    QTest::qWait(2000);
    QCOMPARE(reconnected.count(), 1);   // nothing reconnects after close()
    DeviceSimulator::markPresent(QStringLiteral("SIM:mcu"));
}

void Tst_devicesimulator::enumeratorListsSimulatedPorts()
{
    AppSettings& settings = AppSettings::instance();
    SerialPortEnumerator& e = SerialPortEnumerator::instance();

    settings.setShowSimulatedPorts(true);
    QVERIFY(settings.showSimulatedPorts());
    e.refresh();
    QVERIFY(e.contains(QStringLiteral("SIM:loopback")));
    QVERIFY(e.contains(QStringLiteral("SIM:linux")));
    QVERIFY(e.contains(QStringLiteral("SIM:uboot")));
    QVERIFY(e.contains(QStringLiteral("SIM:mcu")));
    const QStringList names = e.portNames();
    for (int i = 1; i < names.size(); ++i) {
        QVERIFY(!SerialPortEnumerator::naturalLess(names.at(i), names.at(i - 1)));   // still sorted
    }

    // A "rebooting" device disappears from the list and comes back.
    QSignalSpy removed(&e, &SerialPortEnumerator::portRemoved);
    QSignalSpy added(&e, &SerialPortEnumerator::portAdded);
    DeviceSimulator::markAbsent(QStringLiteral("SIM:uboot"), -1);
    e.refresh();
    QVERIFY(!e.contains(QStringLiteral("SIM:uboot")));
    QCOMPARE(removed.count(), 1);
    QCOMPARE(removed.first().first().toString(), QStringLiteral("SIM:uboot"));
    DeviceSimulator::markPresent(QStringLiteral("SIM:uboot"));
    e.refresh();
    QVERIFY(e.contains(QStringLiteral("SIM:uboot")));
    QCOMPARE(added.count(), 1);

    // Hidden by preference.
    settings.setShowSimulatedPorts(false);
    QVERIFY(!settings.showSimulatedPorts());
    e.refresh();
    QVERIFY(!e.contains(QStringLiteral("SIM:loopback")));
    QVERIFY(!e.contains(QStringLiteral("SIM:mcu")));
    settings.setShowSimulatedPorts(true);
    e.refresh();
    QVERIFY(e.contains(QStringLiteral("SIM:mcu")));
}

QTEST_GUILESS_MAIN(Tst_devicesimulator)
#include "tst_devicesimulator.moc"
