// Unit tests for core/BaudRateDetector: the text score, the search over a real SerialConnection
// on the simulated devices (which talk at a native rate of their own since v0.4 and deliver
// wrong-rate garbage otherwise), cancellation / restore, and the garbage watchdog. UI-free.
#include <QtTest>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <algorithm>

#include "app/AppSettings.h"
#include "core/BaudRateDetector.h"
#include "core/DeviceSimulator.h"
#include "core/SerialConnection.h"

namespace {

constexpr int kSearchTimeoutMs = 15000;

const QString kLinux = QStringLiteral("SIM:linux");
const QString kLoopback = QStringLiteral("SIM:loopback");

/// A Rockchip-style boot log fragment: timestamps, colours, a progress "\r" line, tabs.
QByteArray bootLog()
{
    QByteArray log;
    log += "\r\nU-Boot 2017.09 (Sep 17 2026 - 10:21:43 +0800)\r\nModel: Rockchip RV1106 EVB\r\nDRAM:  64 MiB\r\n";
    for (int i = 0; i < 40; ++i) {
        log += QStringLiteral("[    %1.%2] rockchip-pinctrl pinctrl: probed pinctrl subsystem, irq=%3\r\n")
                   .arg(i / 10)
                   .arg(i % 10 * 100000 + 12345, 6, 10, QLatin1Char('0'))
                   .arg(20 + i)
                   .toUtf8();
    }
    log += "[    1.302019] \x1b[33mmmc1: Failed to initialize a non-removable card\x1b[0m\r\n";
    log += "Starting network: [  \x1b[32mOK\x1b[0m  ]\r\nStarting sshd: [\x1b[31mFAILED\x1b[0m]\r\n";
    log += "\rDownloading firmware  [########........]  42% /\rDownloading firmware  [#########.......]  50% -\r\n";
    log += "processor\t: 0\r\nmodel name\t: ARMv7 Processor rev 5 (v7l)\r\n\a\r\nrv1106 login: ";
    return log;
}

/// Everything a SerialConnection received, plus the detector's signals, in one place.
struct SearchLog
{
    QByteArray received;
    QList<qint32> tried;
    QList<double> scores;
    QList<int> bytes;
    QList<qint32> startedWith;
    int finishedCount = 0;
    bool found = false;
    qint32 resultBaud = 0;
    double resultScore = -1.0;

    SearchLog(SerialConnection& connection, BaudRateDetector& detector)
    {
        QObject::connect(&connection, &Transport::dataReceived, &detector,
                         [this](const QByteArray& data) { received += data; });
        QObject::connect(&detector, &BaudRateDetector::started, &detector,
                         [this](qint32 baud) { startedWith.append(baud); });
        QObject::connect(&detector, &BaudRateDetector::candidateTried, &detector,
                         [this](qint32 baud, double score, int count) {
                             tried.append(baud);
                             scores.append(score);
                             bytes.append(count);
                         });
        QObject::connect(&detector, &BaudRateDetector::finished, &detector,
                         [this](bool ok, qint32 baud, double score) {
                             ++finishedCount;
                             found = ok;
                             resultBaud = baud;
                             resultScore = score;
                         });
    }
};

bool openSim(SerialConnection& connection, const QString& port, qint32 baud)
{
    SerialSettings s;
    s.portName = port;
    s.baudRate = baud;
    connection.setSettings(s);
    return connection.open() && connection.isOpen();
}

} // namespace

class Tst_bauddetector : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase();
    void cleanupTestCase();

    // scoring
    void scoreEmpty();
    void scoreBootLog();
    void scoreCjk();
    void scoreGarbage();
    void scoreMixed();
    void scoreTruncatedTail();
    void scoreControlsAndInvalid();

    // configuration
    void defaultsAndClamps();
    void startRefusals();

    // search over a SerialConnection
    void findsLinuxRate();
    void confirmsCurrentRateFirst();
    void silentDeviceKeepsOriginal();
    void unreadableEverywhereRestoresOriginal();
    void cancelRestores();
    void closeCancels();

    // watchdog
    void watchdog();
    void watchdogIgnoredWhileSearching();
    void watchdogHeldAfterUnreadableSearch();

private:
    QTemporaryDir m_tempDir;
};

// =======================================================================================

void Tst_bauddetector::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("BuildAI-Test"));
    QCoreApplication::setApplicationName(QStringLiteral("SerialUtilityTest-bauddetector"));
    QVERIFY(m_tempDir.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_tempDir.filePath(QStringLiteral("settings")));
    QSettings settings;
    settings.clear();
    QVERIFY(settings.fileName().contains(QStringLiteral("SerialUtilityTest-bauddetector")));
    AppSettings::instance().setShowSimulatedPorts(true);
}

void Tst_bauddetector::cleanupTestCase()
{
    for (const SerialPortEntry& e : DeviceSimulator::entries()) {
        DeviceSimulator::markPresent(e.portName);
    }
    QSettings().clear();
}

// ---------------------------------------------------------------------------------------
// scoring
// ---------------------------------------------------------------------------------------

void Tst_bauddetector::scoreEmpty()
{
    QCOMPARE(BaudRateDetector::textScore(QByteArray()), 1.0);
    QCOMPARE(BaudRateDetector::goodScore(), 0.90);
    QCOMPARE(BaudRateDetector::badScore(), 0.50);
}

void Tst_bauddetector::scoreBootLog()
{
    const double score = BaudRateDetector::textScore(bootLog());
    QVERIFY2(score > 0.97, qPrintable(QString::number(score)));
    QVERIFY(score <= 1.0);
    // Plain ASCII with CR/LF only is perfect.
    QCOMPARE(BaudRateDetector::textScore(QByteArrayLiteral("hello world\r\n")), 1.0);
}

void Tst_bauddetector::scoreCjk()
{
    QByteArray log = bootLog();
    log += QString::fromUtf8("本机信息：瑞芯微 RV1106 开发板，内核 5.10.160，欢迎使用 BuildAI Linux\r\n").toUtf8();
    log += QString::fromUtf8("┌──────────┬──────────┐\r\n│ 中文测试 │ ASCII    │\r\n").toUtf8();
    log += QString::fromUtf8("Wide: 漢字 かな カナ 한글 ｆｕｌｌｗｉｄｔｈ | narrow: abc, emoji 😀 done\r\n").toUtf8();
    const double score = BaudRateDetector::textScore(log);
    QVERIFY2(score > 0.97, qPrintable(QString::number(score)));
    // Pure CJK text is perfect too (4-byte sequences included).
    const QByteArray cjk = QString::fromUtf8("欢迎使用串口工具，系统启动完成 😀\r\n").toUtf8();
    QCOMPARE(BaudRateDetector::textScore(cjk), 1.0);
}

void Tst_bauddetector::scoreGarbage()
{
    const QByteArray noise = DeviceSimulator::wrongRateNoise(512);
    QCOMPARE(noise.size(), 512);
    QCOMPARE(noise, DeviceSimulator::wrongRateNoise(512));   // deterministic
    QVERIFY(noise != DeviceSimulator::wrongRateNoise(512, 7));
    QVERIFY(noise.count('\0') + noise.count('\xFF') > 512 / 4);   // dominated by 0x00 / 0xFF
    const double score = BaudRateDetector::textScore(noise);
    QVERIFY2(score < 0.4, qPrintable(QString::number(score)));
    QVERIFY(score >= 0.0);
    // Pure 0x00 / 0xFF runs score zero.
    QCOMPARE(BaudRateDetector::textScore(QByteArray(100, '\0')), 0.0);
    QCOMPARE(BaudRateDetector::textScore(QByteArray(100, '\xFF')), 0.0);
    // High-bit bytes that never form UTF-8 are bad even without 0x00/0xFF.
    QByteArray highs;
    for (int i = 0; i < 200; ++i) {
        highs += static_cast<char>(0x80 + (i * 7) % 0x3F);   // stray continuation bytes
    }
    QVERIFY(BaudRateDetector::textScore(highs) < 0.1);
}

void Tst_bauddetector::scoreMixed()
{
    // Half text, half garbage: neither good nor clearly bad, but below the good threshold.
    const QByteArray text = bootLog().left(512);
    const QByteArray noise = DeviceSimulator::wrongRateNoise(512);
    const double mixed = BaudRateDetector::textScore(text + noise);
    QVERIFY2(mixed < BaudRateDetector::goodScore(), qPrintable(QString::number(mixed)));
    QVERIFY2(mixed > 0.1, qPrintable(QString::number(mixed)));
    // A few garbage bytes in a lot of text (a glitch) keep the text good.
    const double glitch = BaudRateDetector::textScore(bootLog() + noise.left(8) + bootLog());
    QVERIFY2(glitch > BaudRateDetector::goodScore(), qPrintable(QString::number(glitch)));
    // The 0x00/0xFF penalty: 10 % zeros in otherwise perfect text drop it well below 0.9.
    QByteArray zeros = QByteArray(900, 'a') + QByteArray(100, '\0');
    const double penalised = BaudRateDetector::textScore(zeros);
    QVERIFY2(penalised < 0.9 && penalised > 0.5, qPrintable(QString::number(penalised)));
    // ... while 2 % is within the tolerance (a stray NUL after a reset is common).
    zeros = QByteArray(980, 'a') + QByteArray(20, '\0');
    QCOMPARE(BaudRateDetector::textScore(zeros), 0.98);
}

void Tst_bauddetector::scoreTruncatedTail()
{
    // A multi-byte character cut by the end of a chunk is one small penalty, not a bad run.
    const QByteArray line = QStringLiteral("[    1.234567] a perfectly normal kernel line of text\r\n").toUtf8();
    const QByteArray cjk = QString::fromUtf8("欢").toUtf8();   // E6 AC A2
    QCOMPARE(cjk.size(), 3);
    const double whole = BaudRateDetector::textScore(line + cjk);
    QCOMPARE(whole, 1.0);
    const double truncated = BaudRateDetector::textScore(line + cjk.left(2));
    QVERIFY2(truncated > 0.97, qPrintable(QString::number(truncated)));
    QVERIFY(truncated < 1.0);
    const double leadOnly = BaudRateDetector::textScore(line + cjk.left(1));
    QVERIFY2(leadOnly > 0.97, qPrintable(QString::number(leadOnly)));
    // The same bytes in the middle of the data (not a tail) are an invalid sequence: worse.
    const double broken = BaudRateDetector::textScore(cjk.left(2) + line);
    QVERIFY(broken < truncated);
    QVERIFY(broken > 0.9);
}

void Tst_bauddetector::scoreControlsAndInvalid()
{
    // Text controls are good; other C0 controls and DEL are bad.
    QCOMPARE(BaudRateDetector::textScore(QByteArrayLiteral("\r\n\t\x1b\b\a\f")), 1.0);
    QCOMPARE(BaudRateDetector::textScore(QByteArrayLiteral("ab\x01\x02")), 0.5);
    QCOMPARE(BaudRateDetector::textScore(QByteArrayLiteral("abc\x7f")), 0.75);
    // Overlong / surrogate / out-of-range sequences are rejected byte by byte.
    QCOMPARE(BaudRateDetector::textScore(QByteArrayLiteral("\xC0\x80")), 0.0);          // overlong NUL
    QCOMPARE(BaudRateDetector::textScore(QByteArrayLiteral("\xE0\x80\x80")), 0.0);      // overlong
    QCOMPARE(BaudRateDetector::textScore(QByteArrayLiteral("\xED\xA0\x80")), 0.0);      // surrogate
    QCOMPARE(BaudRateDetector::textScore(QByteArrayLiteral("\xF4\x90\x80\x80")), 0.0);  // > U+10FFFF
    QCOMPARE(BaudRateDetector::textScore(QByteArrayLiteral("\xF5\x80\x80\x80")), 0.0);  // invalid lead
    // A lead byte followed by a non-continuation byte costs only the lead.
    QCOMPARE(BaudRateDetector::textScore(QByteArrayLiteral("\xE6" "abc")), 0.75);
}

// ---------------------------------------------------------------------------------------
// configuration
// ---------------------------------------------------------------------------------------

void Tst_bauddetector::defaultsAndClamps()
{
    AppSettings::instance().setAutoBaudCandidates({115200, 1500000});
    BaudRateDetector detector;
    QCOMPARE(detector.candidates(), (QList<qint32>{115200, 1500000}));   // AppSettings by default
    QCOMPARE(detector.sampleMs(), 1500);
    QCOMPARE(detector.minBytes(), 64);
    QVERIFY(!detector.isRunning());
    QCOMPARE(detector.currentCandidate(), 0);
    QVERIFY(!detector.connection());

    detector.setSampleMs(50);
    QCOMPARE(detector.sampleMs(), 200);
    detector.setSampleMs(99999);
    QCOMPARE(detector.sampleMs(), 10000);
    detector.setSampleMs(400);
    QCOMPARE(detector.sampleMs(), 400);
    detector.setMinBytes(0);
    QCOMPARE(detector.minBytes(), 1);
    detector.setMinBytes(32);
    QCOMPARE(detector.minBytes(), 32);
    detector.setCandidates({9600});
    QCOMPARE(detector.candidates(), QList<qint32>{9600});
    AppSettings::instance().setAutoBaudCandidates({});
    QCOMPARE(AppSettings::instance().autoBaudCandidates(), AppSettings::defaultAutoBaudCandidates());
}

void Tst_bauddetector::startRefusals()
{
    BaudRateDetector detector;
    QSignalSpy started(&detector, &BaudRateDetector::started);
    QSignalSpy finished(&detector, &BaudRateDetector::finished);
    QVERIFY(!detector.start());   // no connection

    SerialConnection connection;
    detector.setConnection(&connection);
    QCOMPARE(detector.connection(), &connection);
    QVERIFY(!detector.start());   // not open

    QVERIFY(openSim(connection, kLoopback, 115200));
    detector.setCandidates({});
    QVERIFY(!detector.start());   // no candidates
    QCOMPARE(started.count(), 0);
    QCOMPARE(finished.count(), 0);

    detector.setCandidates({9600});
    detector.setSampleMs(300);
    QVERIFY(detector.start());
    QVERIFY(detector.isRunning());
    QCOMPARE(detector.currentCandidate(), 115200);   // the current rate is tried first
    QVERIFY(!detector.start());                      // already running
    QCOMPARE(started.count(), 1);
    detector.cancel();
    QVERIFY(!detector.isRunning());
    QCOMPARE(finished.count(), 1);
    detector.cancel();   // idle: nothing happens
    QCOMPARE(finished.count(), 1);
    connection.close();
}

// ---------------------------------------------------------------------------------------
// search over a SerialConnection
// ---------------------------------------------------------------------------------------

void Tst_bauddetector::findsLinuxRate()
{
    SerialConnection connection;
    BaudRateDetector detector;
    detector.setConnection(&connection);
    detector.setCandidates({115200, 921600, 1500000});
    detector.setSampleMs(400);
    SearchLog log(connection, detector);

    // The board talks at 1500000; the port is opened at 115200 and hears garbage.
    QCOMPARE(DeviceSimulator::nativeBaudRate(DeviceSimulator::Kind::Linux), 1500000);
    QVERIFY(openSim(connection, kLinux, 115200));
    QVERIFY(detector.start());
    QCOMPARE(log.startedWith, QList<qint32>{115200});
    QCOMPARE(detector.currentCandidate(), 115200);

    QTRY_COMPARE_WITH_TIMEOUT(log.finishedCount, 1, kSearchTimeoutMs);
    QVERIFY(log.found);
    QCOMPARE(log.resultBaud, 1500000);
    QVERIFY2(log.resultScore > 0.9, qPrintable(QString::number(log.resultScore)));
    QVERIFY(!detector.isRunning());
    QCOMPARE(detector.currentCandidate(), 0);

    // Current rate first (once), then the other candidates in order; each judged on real bytes.
    QCOMPARE(log.tried, (QList<qint32>{115200, 921600, 1500000}));
    QCOMPARE(log.bytes.size(), 3);
    for (int i = 0; i < 3; ++i) {
        QVERIFY2(log.bytes.at(i) >= detector.minBytes(), qPrintable(QString::number(log.bytes.at(i))));
    }
    QVERIFY2(log.scores.at(0) < BaudRateDetector::badScore(), qPrintable(QString::number(log.scores.at(0))));
    QVERIFY2(log.scores.at(1) < BaudRateDetector::badScore(), qPrintable(QString::number(log.scores.at(1))));
    QCOMPARE(log.scores.at(2), log.resultScore);

    // The rate found stays applied on the connection (and on the simulated device).
    QCOMPARE(connection.settings().baudRate, 1500000);
    QVERIFY(connection.isOpen());
    auto* sim = connection.findChild<DeviceSimulator*>();
    QVERIFY(sim);
    QCOMPARE(sim->baudRate(), 1500000);
    QVERIFY(sim->baudRateMatches());

    // From here on the console is readable: the login prompt arrives as text.
    log.received.clear();
    QTRY_VERIFY_WITH_TIMEOUT(log.received.contains("rv1106 login: "), kSearchTimeoutMs);
    connection.close();
}

void Tst_bauddetector::confirmsCurrentRateFirst()
{
    SerialConnection connection;
    BaudRateDetector detector;
    detector.setConnection(&connection);
    detector.setCandidates({9600, 115200, 1500000});
    detector.setSampleMs(400);
    SearchLog log(connection, detector);

    QVERIFY(openSim(connection, kLinux, 1500000));
    auto* sim = connection.findChild<DeviceSimulator*>();
    QVERIFY(sim);
    QVERIFY(detector.start());
    QTRY_COMPARE_WITH_TIMEOUT(log.finishedCount, 1, kSearchTimeoutMs);
    QVERIFY(log.found);
    QCOMPARE(log.resultBaud, 1500000);
    QCOMPARE(log.tried, QList<qint32>{1500000});   // confirmed on the first try: no other candidate
    QVERIFY(log.resultScore > 0.9);
    QCOMPARE(connection.settings().baudRate, 1500000);
    QCOMPARE(sim->baudRate(), 1500000);
    // Nothing the host heard was garbage: the rate was never changed during the search.
    QVERIFY(BaudRateDetector::textScore(log.received) > 0.9);
    QVERIFY(log.received.contains("Booting Linux on physical CPU 0x0"));
    connection.close();
}

void Tst_bauddetector::silentDeviceKeepsOriginal()
{
    SerialConnection connection;
    BaudRateDetector detector;
    detector.setConnection(&connection);
    detector.setCandidates({9600, 1500000, 115200});   // 115200 is the current rate: tried first, not again
    detector.setSampleMs(300);
    SearchLog log(connection, detector);

    QVERIFY(openSim(connection, kLoopback, 115200));   // nothing is ever sent: the device stays silent
    QElapsedTimer clock;
    clock.start();
    QVERIFY(detector.start());
    QTRY_COMPARE_WITH_TIMEOUT(log.finishedCount, 1, kSearchTimeoutMs);
    QVERIFY(!log.found);
    QCOMPARE(log.resultBaud, 115200);
    QCOMPARE(log.resultScore, 0.0);
    QCOMPARE(log.tried, (QList<qint32>{115200, 9600, 1500000}));
    QCOMPARE(log.bytes, (QList<int>{0, 0, 0}));
    QCOMPARE(log.scores, (QList<double>{0.0, 0.0, 0.0}));
    QVERIFY2(clock.elapsed() >= 3 * 300 - 50, qPrintable(QString::number(clock.elapsed())));   // full sample each
    QVERIFY(log.received.isEmpty());
    // The original rate is restored on the connection and on the device.
    QCOMPARE(connection.settings().baudRate, 115200);
    auto* sim = connection.findChild<DeviceSimulator*>();
    QVERIFY(sim);
    QCOMPARE(sim->baudRate(), 115200);
    QVERIFY(connection.isOpen());
    connection.close();
}

void Tst_bauddetector::unreadableEverywhereRestoresOriginal()
{
    SerialConnection connection;
    BaudRateDetector detector;
    detector.setConnection(&connection);
    detector.setCandidates({19200, 38400});   // the board talks at 1500000: garbage everywhere
    detector.setSampleMs(300);
    SearchLog log(connection, detector);

    QVERIFY(openSim(connection, kLinux, 9600));
    QVERIFY(detector.start());
    QTRY_COMPARE_WITH_TIMEOUT(log.finishedCount, 1, kSearchTimeoutMs);
    QVERIFY(!log.found);
    QCOMPARE(log.tried, (QList<qint32>{9600, 19200, 38400}));
    QVERIFY(log.tried.contains(log.resultBaud));   // the best of the bad ones
    QVERIFY2(log.resultScore > 0.0 && log.resultScore < BaudRateDetector::goodScore(),
             qPrintable(QString::number(log.resultScore)));
    QCOMPARE(log.resultScore, *std::max_element(log.scores.cbegin(), log.scores.cend()));
    for (const int count : log.bytes) {
        QVERIFY(count >= detector.minBytes());
    }
    QCOMPARE(connection.settings().baudRate, 9600);   // restored
    auto* sim = connection.findChild<DeviceSimulator*>();
    QVERIFY(sim);
    QCOMPARE(sim->baudRate(), 9600);
    connection.close();
}

void Tst_bauddetector::cancelRestores()
{
    SerialConnection connection;
    BaudRateDetector detector;
    detector.setConnection(&connection);
    detector.setCandidates({9600, 1500000});
    detector.setSampleMs(300);
    SearchLog log(connection, detector);

    QVERIFY(openSim(connection, kLoopback, 115200));
    QVERIFY(detector.start());
    // Wait until the search moved on to the second rate, then cancel.
    QTRY_COMPARE_WITH_TIMEOUT(log.tried.size(), 1, kSearchTimeoutMs);
    QCOMPARE(detector.currentCandidate(), 9600);
    QCOMPARE(connection.settings().baudRate, 9600);
    detector.cancel();
    QVERIFY(!detector.isRunning());
    QCOMPARE(log.finishedCount, 1);
    QVERIFY(!log.found);
    QCOMPARE(log.resultBaud, 115200);
    QCOMPARE(log.resultScore, 0.0);
    QCOMPARE(connection.settings().baudRate, 115200);
    auto* sim = connection.findChild<DeviceSimulator*>();
    QVERIFY(sim);
    QCOMPARE(sim->baudRate(), 115200);
    QCOMPARE(log.tried.size(), 1);   // no further candidate is reported after cancel()
    QTest::qWait(400);
    QCOMPARE(log.tried.size(), 1);
    QCOMPARE(log.finishedCount, 1);

    // A new search can start afterwards.
    QVERIFY(detector.start());
    QVERIFY(detector.isRunning());
    detector.cancel();
    QCOMPARE(log.finishedCount, 2);
    connection.close();
}

void Tst_bauddetector::closeCancels()
{
    SerialConnection connection;
    BaudRateDetector detector;
    detector.setConnection(&connection);
    detector.setCandidates({9600, 1500000});
    detector.setSampleMs(300);
    SearchLog log(connection, detector);

    QVERIFY(openSim(connection, kLoopback, 115200));
    QVERIFY(detector.start());
    QTRY_COMPARE_WITH_TIMEOUT(log.tried.size(), 1, kSearchTimeoutMs);
    QCOMPARE(connection.settings().baudRate, 9600);
    connection.close();   // stateChanged(Disconnected) cancels the search
    QVERIFY(!detector.isRunning());
    QCOMPARE(log.finishedCount, 1);
    QVERIFY(!log.found);
    QCOMPARE(log.resultBaud, 115200);
    QCOMPARE(connection.settings().baudRate, 115200);   // restored into the closed connection's settings
    QVERIFY(!detector.start());                          // not open any more
}

// ---------------------------------------------------------------------------------------
// watchdog
// ---------------------------------------------------------------------------------------

void Tst_bauddetector::watchdog()
{
    BaudRateDetector detector;
    detector.setQuietMs(600);
    QSignalSpy garbage(&detector, &BaudRateDetector::garbageDetected);
    const QByteArray noise = DeviceSimulator::wrongRateNoise(600);
    const QByteArray text = bootLog() + bootLog();
    QVERIFY(text.size() >= 600);

    // Less than a window: nothing yet.
    detector.feed(noise.left(300));
    QCOMPARE(garbage.count(), 0);
    // The window (512 bytes) fills: one report, with the score of the window.
    detector.feed(noise.mid(300));
    QCOMPARE(garbage.count(), 1);
    QVERIFY(garbage.first().first().toDouble() < BaudRateDetector::badScore());
    // More garbage within quietMs: silent.
    detector.feed(noise);
    detector.feed(noise);
    QCOMPARE(garbage.count(), 1);
    // Readable text resets the watchdog: the next garbage window reports again at once.
    detector.feed(text.left(600));
    QCOMPARE(garbage.count(), 1);
    detector.feed(noise);
    QCOMPARE(garbage.count(), 2);
    // Without a good window in between, the quiet period applies ...
    detector.feed(noise);
    QCOMPARE(garbage.count(), 2);
    // ... and ends after quietMs.
    QTest::qWait(700);
    detector.feed(noise);
    QCOMPARE(garbage.count(), 3);

    // resetWatchdog() forgets the window: a partial garbage chunk after it is not judged.
    detector.feed(text.left(600));
    detector.resetWatchdog();
    detector.feed(noise.left(400));
    QCOMPARE(garbage.count(), 3);
    detector.feed(noise.left(200));
    QCOMPARE(garbage.count(), 4);

    // The window slides: text after garbage becomes good once it fills the window.
    detector.setWindowBytes(10);   // clamped to the minimum of 64
    detector.resetWatchdog();
    detector.feed(text.left(64));
    QCOMPARE(garbage.count(), 4);
    detector.feed(noise.left(64));
    QCOMPARE(garbage.count(), 5);
    detector.setWindowBytes(1 << 20);   // clamped to 65536: never fills here
    detector.resetWatchdog();
    detector.feed(noise);
    QCOMPARE(garbage.count(), 5);
    // Empty chunks are ignored.
    detector.setWindowBytes(64);
    detector.feed(QByteArray());
    QCOMPARE(garbage.count(), 5);
}

void Tst_bauddetector::watchdogIgnoredWhileSearching()
{
    SerialConnection connection;
    BaudRateDetector detector;
    detector.setConnection(&connection);
    detector.setCandidates({9600});
    detector.setSampleMs(300);
    detector.setQuietMs(0);
    QSignalSpy garbage(&detector, &BaudRateDetector::garbageDetected);
    const QByteArray noise = DeviceSimulator::wrongRateNoise(600);

    QVERIFY(openSim(connection, kLoopback, 115200));
    QVERIFY(detector.start());
    detector.feed(noise);
    detector.feed(noise);
    QCOMPARE(garbage.count(), 0);   // ignored while the search runs
    detector.cancel();
    detector.feed(noise);
    QCOMPARE(garbage.count(), 1);   // and the window was forgotten by the search: a full new one reports
    connection.close();
}

void Tst_bauddetector::watchdogHeldAfterUnreadableSearch()
{
    // Header "Hold": a search that received bytes but found nothing readable at any candidate
    // (the board talks at a rate outside the list) holds the watchdog. Without the hold the
    // next garbage window would start the same failing search again every quiet period, with
    // the session muted for its whole duration.
    SerialConnection connection;
    BaudRateDetector detector;
    detector.setConnection(&connection);
    detector.setCandidates({19200});   // the board talks at 1500000: garbage everywhere
    detector.setSampleMs(300);
    detector.setQuietMs(0);            // no quiet period: only the hold can keep the watchdog silent
    SearchLog log(connection, detector);
    QSignalSpy garbage(&detector, &BaudRateDetector::garbageDetected);
    const QByteArray noise = DeviceSimulator::wrongRateNoise(600);
    const QByteArray text = bootLog() + bootLog();
    QVERIFY(text.size() >= 600);

    QVERIFY(!detector.isWatchdogHeld());
    QVERIFY(openSim(connection, kLinux, 9600));
    QVERIFY(detector.start());
    QVERIFY(!detector.isWatchdogHeld());
    QTRY_COMPARE_WITH_TIMEOUT(log.finishedCount, 1, kSearchTimeoutMs);
    QVERIFY(!log.found);
    QVERIFY(log.bytes.first() >= detector.minBytes());   // bytes arrived: not a silent device
    QVERIFY(detector.isWatchdogHeld());
    // Garbage windows are ignored while held.
    detector.feed(noise);
    detector.feed(noise);
    QCOMPARE(garbage.count(), 0);
    // Readable output releases the hold; the next garbage window reports again at once.
    detector.feed(text.left(600));
    QVERIFY(!detector.isWatchdogHeld());
    detector.feed(noise);
    QCOMPARE(garbage.count(), 1);

    // A search started by other means releases the hold as well, and its outcome decides again.
    detector.feed(noise);
    QCOMPARE(garbage.count(), 2);   // quietMs 0: every garbage window reports while not held
    QVERIFY(detector.start());
    QVERIFY(!detector.isWatchdogHeld());
    QTRY_COMPARE_WITH_TIMEOUT(log.finishedCount, 2, kSearchTimeoutMs);
    QVERIFY(!log.found);
    QVERIFY(detector.isWatchdogHeld());
    detector.feed(noise);
    QCOMPARE(garbage.count(), 2);
    // A cancelled search never holds it.
    QVERIFY(detector.start());
    QVERIFY(!detector.isWatchdogHeld());
    detector.cancel();
    QCOMPARE(log.finishedCount, 3);
    QVERIFY(!detector.isWatchdogHeld());
    detector.feed(noise);
    QCOMPARE(garbage.count(), 3);
    connection.close();

    // Neither does a silent device (score 0): its garbage, should it start, is reported.
    QVERIFY(openSim(connection, kLoopback, 115200));
    QVERIFY(detector.start());
    QTRY_COMPARE_WITH_TIMEOUT(log.finishedCount, 4, kSearchTimeoutMs);
    QVERIFY(!log.found);
    QCOMPARE(log.resultScore, 0.0);
    QVERIFY(!detector.isWatchdogHeld());
    detector.feed(noise);
    QCOMPARE(garbage.count(), 4);
    connection.close();
}

QTEST_GUILESS_MAIN(Tst_bauddetector)
#include "tst_bauddetector.moc"
