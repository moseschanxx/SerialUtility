#include <QtTest>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QFile>

#include "core/LogReplayer.h"

namespace {

/// Concatenates every chunkReady() payload.
struct ChunkCollector
{
    QByteArray data;
    int chunks = 0;
    explicit ChunkCollector(LogReplayer& replayer)
    {
        QObject::connect(&replayer, &LogReplayer::chunkReady, [this](const QByteArray& bytes) {
            data += bytes;
            ++chunks;
        });
    }
};

const QByteArray kTextCapture = QByteArrayLiteral(
    "# BuildAI Serial Utility log - COM8 115200 8N1 - started 2026-09-20T10:00:00\n"
    "[2026-09-20 10:00:00.000] U-Boot 2017.09\r\n"
    "[2026-09-20 10:00:00.010] TX> printenv\n"
    "[2026-09-20 10:00:00.020] bootcmd=boot_fit\n"
    "continuation without prefix\n"
    "[2026-09-20 10:00:00.030] progress 10%\rprogress 20%\n"
    "[2026-09-20 10:00:00.040] \x1b[32mOK\x1b[0m\n"
    "[2026-09-20 10:00:00.050] partial");

const QByteArray kStripped = QByteArrayLiteral("U-Boot 2017.09\r\n"
                                               "bootcmd=boot_fit\r\n"
                                               "continuation without prefix\r\n"
                                               "progress 10%\rprogress 20%\r\n"
                                               "\x1b[32mOK\x1b[0m\r\n"
                                               "partial");

} // namespace

class Tst_logreplayer : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase();
    void detectFormat_data();
    void detectFormat();
    void stripTimestamps();
    void standardSpeeds();
    void startErrors();
    void rawReplayIsPaced();
    void unlimitedSpeed();
    void pauseResumeStop();
    void loopReplay();
    void timestampedReplay();
    void rawFormatCanBeForced();
    void liveSpeedChange();
    void emptyFile();
    void tooLargeFile();

private:
    QString writeFile(const QString& name, const QByteArray& content);
    QTemporaryDir m_dir;
};

void Tst_logreplayer::initTestCase()
{
    QVERIFY(m_dir.isValid());
}

QString Tst_logreplayer::writeFile(const QString& name, const QByteArray& content)
{
    const QString path = m_dir.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return QString();
    }
    file.write(content);
    file.close();
    return path;
}

// ---------------------------------------------------------------------------------------

void Tst_logreplayer::detectFormat_data()
{
    QTest::addColumn<QByteArray>("head");
    QTest::addColumn<bool>("timestamped");
    QTest::newRow("timestamp first") << QByteArrayLiteral("[2026-09-20 10:00:00.000] hello\n") << true;
    QTest::newRow("header first") << kTextCapture << true;
    QTest::newRow("leading blank lines") << QByteArrayLiteral("\r\n\r\n[2026-09-20 10:00:00.000] x") << true;
    QTest::newRow("no trailing newline") << QByteArrayLiteral("[2026-09-20 10:00:00.000] x") << true;
    QTest::newRow("uboot banner") << QByteArrayLiteral("U-Boot 2017.09\r\n") << false;
    QTest::newRow("empty") << QByteArray() << false;
    QTest::newRow("ansi") << QByteArrayLiteral("\x1b[32mOK\x1b[0m\r\n") << false;
    QTest::newRow("no milliseconds") << QByteArrayLiteral("[2026-09-20 10:00:00] x\n") << false;
    QTest::newRow("bracket text") << QByteArrayLiteral("[not a timestamp] x\n") << false;
    QTest::newRow("kernel log") << QByteArrayLiteral("[    0.000000] Booting Linux\n") << false;
    QTest::newRow("comment but not ours") << QByteArrayLiteral("# just a comment\n") << false;
}

void Tst_logreplayer::detectFormat()
{
    QFETCH(QByteArray, head);
    QFETCH(bool, timestamped);
    QCOMPARE(LogReplayer::detectFormat(head) == LogReplayer::Format::TimestampedText, timestamped);
}

void Tst_logreplayer::stripTimestamps()
{
    QCOMPARE(LogReplayer::stripTimestamps(kTextCapture), kStripped);
    QCOMPARE(LogReplayer::stripTimestamps(QByteArray()), QByteArray());
    QCOMPARE(LogReplayer::stripTimestamps(QByteArrayLiteral("plain\n")), QByteArrayLiteral("plain\r\n"));
    QCOMPARE(LogReplayer::stripTimestamps(QByteArrayLiteral("plain")), QByteArrayLiteral("plain"));
    QCOMPARE(LogReplayer::stripTimestamps(QByteArrayLiteral("# header only\n")), QByteArray());
    QCOMPARE(LogReplayer::stripTimestamps(QByteArrayLiteral("[2026-09-20 10:00:00.000] TX> a\n"
                                                            "[2026-09-20 10:00:00.001] \n")),
             QByteArrayLiteral("\r\n"));
    // A raw byte stream survives (nothing to strip) apart from LF -> CRLF normalisation.
    QCOMPARE(LogReplayer::stripTimestamps(QByteArrayLiteral("a\r\nb\nc")), QByteArrayLiteral("a\r\nb\r\nc"));
}

void Tst_logreplayer::standardSpeeds()
{
    const QList<QPair<QString, qint64>> speeds = LogReplayer::standardSpeeds();
    QVERIFY(speeds.size() >= 6);
    QCOMPARE(speeds.first().second, qint64(960));
    QVERIFY(speeds.first().first.contains(QStringLiteral("9600")));
    QCOMPARE(speeds.last().second, qint64(0));
    QVERIFY(!speeds.last().first.isEmpty());
    bool has115200 = false;
    for (int i = 0; i + 1 < speeds.size(); ++i) {
        QVERIFY(speeds.at(i).second > 0);
        if (i + 2 < speeds.size()) {
            QVERIFY(speeds.at(i).second < speeds.at(i + 1).second);   // ascending before "Unlimited"
        }
        if (speeds.at(i).second == 11520) {
            has115200 = true;
            QVERIFY(speeds.at(i).first.contains(QStringLiteral("115200")));
        }
    }
    QVERIFY(has115200);
}

void Tst_logreplayer::startErrors()
{
    LogReplayer replayer;
    QSignalSpy finished(&replayer, &LogReplayer::finished);
    LogReplayer::Options options;

    QVERIFY(!replayer.start(options));   // no file
    QVERIFY(!replayer.lastError().isEmpty());
    QVERIFY(!replayer.isRunning());

    options.filePath = m_dir.filePath(QStringLiteral("does-not-exist.log"));
    QVERIFY(!replayer.start(options));
    QVERIFY2(replayer.lastError().contains(QStringLiteral("does-not-exist.log")), qPrintable(replayer.lastError()));
    QVERIFY(!replayer.isRunning());

    options.filePath = writeFile(QStringLiteral("small.log"), QByteArray(500, 'a'));
    options.bytesPerSecond = 100;   // slow, so it is still running for the second start()
    QVERIFY(replayer.start(options));
    QVERIFY(replayer.isRunning());
    QVERIFY(replayer.lastError().isEmpty());
    QVERIFY(!replayer.start(options));
    QVERIFY(replayer.lastError().contains(QStringLiteral("already running")));
    QVERIFY(replayer.isRunning());   // the running replay is unaffected
    replayer.stop();
    QCOMPARE(finished.count(), 1);
    QCOMPARE(finished.first().first().toBool(), false);
    QVERIFY(!replayer.isRunning());
    replayer.stop();   // idempotent
    QCOMPARE(finished.count(), 1);
}

void Tst_logreplayer::rawReplayIsPaced()
{
    const QByteArray content = QByteArray(1000, 'x');
    LogReplayer replayer;
    ChunkCollector collected(replayer);
    QSignalSpy finished(&replayer, &LogReplayer::finished);
    QSignalSpy progress(&replayer, &LogReplayer::progress);

    LogReplayer::Options options;
    options.filePath = writeFile(QStringLiteral("raw.bin"), content);
    options.bytesPerSecond = 5000;   // 100 bytes per 20 ms tick -> ~200 ms
    QVERIFY(replayer.start(options));
    QVERIFY(replayer.isRunning());
    QVERIFY(!replayer.isPaused());
    QCOMPARE(replayer.totalBytes(), qint64(1000));
    QCOMPARE(replayer.sentBytes(), qint64(0));
    QCOMPARE(collected.chunks, 0);   // nothing synchronous

    QTRY_VERIFY_WITH_TIMEOUT(collected.chunks > 0, 1000);
    QVERIFY2(collected.data.size() < 1000, "paced: not everything in the first tick");
    QVERIFY(collected.data.size() >= 100);

    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 3000);
    QCOMPARE(finished.first().first().toBool(), true);
    QCOMPARE(collected.data, content);
    QCOMPARE(collected.chunks, 10);
    QCOMPARE(progress.count(), 10);
    QCOMPARE(progress.last().at(0).toLongLong(), qint64(1000));
    QCOMPARE(progress.last().at(1).toLongLong(), qint64(1000));
    QCOMPARE(replayer.sentBytes(), qint64(1000));
    QVERIFY(!replayer.isRunning());
    QCOMPARE(replayer.options().format, LogReplayer::Format::Raw);

    QTest::qWait(50);
    QCOMPARE(finished.count(), 1);   // exactly once
}

void Tst_logreplayer::unlimitedSpeed()
{
    const QByteArray content = QByteArray(10000, 'u');
    LogReplayer replayer;
    ChunkCollector collected(replayer);
    QSignalSpy finished(&replayer, &LogReplayer::finished);
    LogReplayer::Options options;
    options.filePath = writeFile(QStringLiteral("big.bin"), content);
    options.bytesPerSecond = 0;
    QVERIFY(replayer.start(options));
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 2000);
    QCOMPARE(collected.data, content);
    QCOMPARE(collected.chunks, 3);   // 4096 + 4096 + 1808
    QCOMPARE(finished.first().first().toBool(), true);
}

void Tst_logreplayer::pauseResumeStop()
{
    const QByteArray content = QByteArray(5000, 'p');
    LogReplayer replayer;
    ChunkCollector collected(replayer);
    QSignalSpy finished(&replayer, &LogReplayer::finished);
    LogReplayer::Options options;
    options.filePath = writeFile(QStringLiteral("pause.bin"), content);
    options.bytesPerSecond = 5000;   // 100 bytes per tick, 1 s in total

    replayer.pause();    // no-ops while idle
    replayer.resume();
    QVERIFY(!replayer.isPaused());

    QVERIFY(replayer.start(options));
    QTRY_VERIFY_WITH_TIMEOUT(collected.data.size() >= 100, 1000);
    replayer.pause();
    QVERIFY(replayer.isPaused());
    QVERIFY(replayer.isRunning());
    const int atPause = collected.data.size();
    QTest::qWait(120);
    QCOMPARE(collected.data.size(), atPause);   // frozen
    replayer.pause();                            // idempotent
    QVERIFY(replayer.isPaused());

    replayer.resume();
    QVERIFY(!replayer.isPaused());
    QTRY_VERIFY_WITH_TIMEOUT(collected.data.size() > atPause, 1000);

    replayer.stop();
    QCOMPARE(finished.count(), 1);
    QCOMPARE(finished.first().first().toBool(), false);
    QVERIFY(!replayer.isRunning());
    QVERIFY(!replayer.isPaused());
    const int atStop = collected.data.size();
    QTest::qWait(80);
    QCOMPARE(collected.data.size(), atStop);   // nothing after stop()
    QVERIFY(atStop < 5000);

    // A new start() runs to completion again.
    options.bytesPerSecond = 0;
    QVERIFY(replayer.start(options));
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 2, 2000);
    QCOMPARE(finished.last().first().toBool(), true);
    QCOMPARE(collected.data.size(), atStop + 5000);
}

void Tst_logreplayer::loopReplay()
{
    const QByteArray content = QByteArrayLiteral("0123456789");
    LogReplayer replayer;
    ChunkCollector collected(replayer);
    QSignalSpy finished(&replayer, &LogReplayer::finished);
    QSignalSpy progress(&replayer, &LogReplayer::progress);
    LogReplayer::Options options;
    options.filePath = writeFile(QStringLiteral("loop.bin"), content);
    options.bytesPerSecond = 0;
    options.loop = true;
    QVERIFY(replayer.start(options));
    QTRY_VERIFY_WITH_TIMEOUT(collected.data.size() >= 40, 2000);   // wrapped around several times
    QCOMPARE(finished.count(), 0);
    QVERIFY(replayer.isRunning());
    for (const QList<QVariant>& args : progress) {
        QVERIFY(args.at(0).toLongLong() <= args.at(1).toLongLong());
        QCOMPARE(args.at(1).toLongLong(), qint64(10));
    }
    QVERIFY(collected.data.startsWith("01234567890123456789"));
    replayer.stop();
    QCOMPARE(finished.count(), 1);
    QCOMPARE(finished.first().first().toBool(), false);
}

void Tst_logreplayer::timestampedReplay()
{
    LogReplayer replayer;
    ChunkCollector collected(replayer);
    QSignalSpy finished(&replayer, &LogReplayer::finished);
    LogReplayer::Options options;
    options.filePath = writeFile(QStringLiteral("capture.log"), kTextCapture);
    options.bytesPerSecond = 0;
    options.format = LogReplayer::Format::Raw;   // overridden by auto-detection
    options.autoDetectFormat = true;
    QVERIFY(replayer.start(options));
    QCOMPARE(replayer.options().format, LogReplayer::Format::TimestampedText);
    QCOMPARE(replayer.totalBytes(), qint64(kStripped.size()));
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 2000);
    QCOMPARE(collected.data, kStripped);
    QVERIFY(!collected.data.contains("TX>"));
    QVERIFY(!collected.data.contains("# BuildAI"));
}

void Tst_logreplayer::rawFormatCanBeForced()
{
    LogReplayer replayer;
    ChunkCollector collected(replayer);
    QSignalSpy finished(&replayer, &LogReplayer::finished);
    LogReplayer::Options options;
    options.filePath = writeFile(QStringLiteral("capture-raw.log"), kTextCapture);
    options.bytesPerSecond = 0;
    options.format = LogReplayer::Format::Raw;
    options.autoDetectFormat = false;
    QVERIFY(replayer.start(options));
    QCOMPARE(replayer.options().format, LogReplayer::Format::Raw);
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 2000);
    QCOMPARE(collected.data, kTextCapture);   // byte for byte
}

void Tst_logreplayer::liveSpeedChange()
{
    LogReplayer replayer;
    ChunkCollector collected(replayer);
    QSignalSpy finished(&replayer, &LogReplayer::finished);
    LogReplayer::Options options;
    options.filePath = writeFile(QStringLiteral("speed.bin"), QByteArray(20000, 's'));
    options.bytesPerSecond = 1000;   // 20 bytes per tick: 20 s at this rate
    QVERIFY(replayer.start(options));
    QTRY_VERIFY_WITH_TIMEOUT(collected.chunks >= 2, 1000);
    QVERIFY(collected.data.size() < 1000);
    replayer.setBytesPerSecond(0);
    QCOMPARE(replayer.options().bytesPerSecond, qint64(0));
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 2000);
    QCOMPARE(collected.data.size(), 20000);
    replayer.setBytesPerSecond(-5);   // clamped
    QCOMPARE(replayer.options().bytesPerSecond, qint64(0));
}

void Tst_logreplayer::emptyFile()
{
    LogReplayer replayer;
    ChunkCollector collected(replayer);
    QSignalSpy finished(&replayer, &LogReplayer::finished);
    LogReplayer::Options options;
    options.filePath = writeFile(QStringLiteral("empty.log"), QByteArray());
    QVERIFY(replayer.start(options));
    QVERIFY(replayer.isRunning());
    QCOMPARE(replayer.totalBytes(), qint64(0));
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 1000);
    QCOMPARE(finished.first().first().toBool(), true);
    QCOMPARE(collected.chunks, 0);
    QVERIFY(!replayer.isRunning());
}

void Tst_logreplayer::tooLargeFile()
{
    const QString path = m_dir.filePath(QStringLiteral("huge.bin"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    if (!file.resize(64LL * 1024 * 1024 + 1)) {
        file.close();
        QSKIP("file system does not support resize()");
    }
    file.close();
    LogReplayer replayer;
    LogReplayer::Options options;
    options.filePath = path;
    QVERIFY(!replayer.start(options));
    QVERIFY2(replayer.lastError().contains(QStringLiteral("too large")), qPrintable(replayer.lastError()));
    QVERIFY(!replayer.isRunning());
    QFile::remove(path);
}

QTEST_GUILESS_MAIN(Tst_logreplayer)
#include "tst_logreplayer.moc"
