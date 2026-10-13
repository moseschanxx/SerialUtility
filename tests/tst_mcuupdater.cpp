// McuUpdater (the asynchronous CC1310 updater) against FakeMcu / FakeMcuServer: info, flash through
// the SoC, a board already in the updater, the serial path, NACK / resume, lost frames, the retry
// limit, refusals and silence, package checks, the restart banner, set role, cancel, a closing
// device, sequence wrap and the exact log lines of runbook Parts B, C and J
// (CC1310-Firmware docs/operations/04_serial_update.md). The conversation tests that need a device
// misbehaving in one precise way use MemoryLink, a scripted in-process QIODevice; everything else
// talks to a real QTcpSocket connected to FakeMcuServer. No test sleeps: every wait is a QTRY_* or a
// QSignalSpy::wait(), the only fixed delays are the updater's own (shortened) timeouts.
#include <QtTest>

#include <QHostAddress>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTcpSocket>
#include <QTimer>
#include <QtEndian>
#include <cstring>
#include <functional>

#include "firmware/CcPackage.h"
#include "firmware/CcProtocol.h"
#include "firmware/McuUpdater.h"
#include "support/FakeMcu.h"

using namespace CcProtocol;
using Phase = McuUpdater::Phase;

namespace {

constexpr quint32 kCc1m = 0x4343314D;

QByteArray le32(quint32 value)
{
    QByteArray bytes(4, '\0');
    qToLittleEndian<quint32>(value, bytes.data());
    return bytes;
}

quint32 readLe32(const QByteArray& bytes, int offset = 0)
{
    return qFromLittleEndian<quint32>(bytes.constData() + offset);
}

QByteArray infoPayload(quint32 target, quint32 state, quint32 maxSize, quint32 imageSize, quint32 version,
                       std::optional<quint32> role = std::nullopt)
{
    QByteArray payload = le32(target) + le32(state) + le32(maxSize) + le32(imageSize) + le32(version);
    if (role)
        payload += le32(*role);
    return payload;
}

/// test_adb_link.py package(): bytes((i * 7) & 0xFF), target CC1M, version 0.8.1.
QByteArray patternImage(int size)
{
    QByteArray image;
    image.reserve(size);
    for (int i = 0; i < size; ++i)
        image.append(static_cast<char>((i * 7) & 0xFF));
    return image;
}

CcPackage makePackage(int size = 1000, quint32 target = kCc1m, quint32 version = 0x801)
{
    CcPackage package;
    CcPackage::parse(CcPackage::build(patternImage(size), target, version), &package);
    return package;
}

const QString kSummary1000 = QStringLiteral("INFO package target=0x4343314D version=0.8.1 code=0x00000801 size=1000");
const QString kRebooting = QStringLiteral("INFO application version=0.8.1 role=rx; rebooting it into the UART updater");
const QString kRestartedRx = QStringLiteral("INFO application restarted: version=0.8.1 role=rx");

} // namespace

/**
 * A scripted in-process device: bytes the updater writes are recorded and handed to `onWrite` (or to
 * the FakeMcu behind it when no hook is set); bytes for the updater are queued into the event loop
 * (inject), so nothing is answered from inside write().
 */
class MemoryLink : public QIODevice
{
    Q_OBJECT
public:
    explicit MemoryLink(FakeMcu* mcu = nullptr, QObject* parent = nullptr) : QIODevice(parent), m_mcu(mcu)
    {
        open(QIODevice::ReadWrite);
        if (m_mcu)
            connect(m_mcu, &FakeMcu::outputAvailable, this, &MemoryLink::pullOutput, Qt::QueuedConnection);
    }

    std::function<void(MemoryLink*, const QByteArray&)> onWrite;
    QList<QByteArray> writes;                         ///< every write() the updater made

    /// Hand `bytes` to the FakeMcu (what the default path does for every write).
    void forward(const QByteArray& bytes) { if (m_mcu) m_mcu->feed(bytes); }
    /// Deliver `bytes` to the updater from the event loop.
    void inject(const QByteArray& bytes)
    {
        QMetaObject::invokeMethod(this, [this, bytes] { m_buffer.append(bytes); emit readyRead(); }, Qt::QueuedConnection);
    }
    void injectAfter(int ms, const QByteArray& bytes)
    {
        QTimer::singleShot(ms, this, [this, bytes] { m_buffer.append(bytes); emit readyRead(); });
    }
    /// Deliver the FakeMcu's output at most `bytes` per readyRead (a slow UART); 0 = all at once.
    void setDeliverBytes(int bytes) { m_deliverBytes = bytes; }
    void closeRemote()
    {
        QMetaObject::invokeMethod(this, [this] { emit readChannelFinished(); }, Qt::QueuedConnection);
    }

    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override { return m_buffer.size() + QIODevice::bytesAvailable(); }

    /// Decoded frames among the writes, for assertions on what the updater sent.
    QList<Frame> frames() const
    {
        QList<Frame> result;
        for (const QByteArray& chunk : writes) {
            Frame frame;
            if (decodeFrame(chunk, &frame))
                result.append(frame);
        }
        return result;
    }

protected:
    qint64 readData(char* data, qint64 maxSize) override
    {
        const qint64 count = qMin<qint64>(maxSize, m_buffer.size());
        memcpy(data, m_buffer.constData(), static_cast<size_t>(count));
        m_buffer.remove(0, static_cast<qsizetype>(count));
        return count;
    }
    qint64 writeData(const char* data, qint64 size) override
    {
        const QByteArray chunk(data, static_cast<qsizetype>(size));
        writes.append(chunk);
        if (onWrite)
            onWrite(this, chunk);
        else
            forward(chunk);
        return size;
    }

private slots:
    void pullOutput()
    {
        if (!m_mcu || !m_mcu->hasOutput())
            return;
        m_buffer.append(m_mcu->take(m_deliverBytes > 0 ? m_deliverBytes : 1 << 20));
        emit readyRead();
        if (m_deliverBytes > 0 && m_mcu->hasOutput())
            QMetaObject::invokeMethod(this, &MemoryLink::pullOutput, Qt::QueuedConnection);
    }

private:
    FakeMcu* m_mcu;
    QByteArray m_buffer;
    int m_deliverBytes = 0;
};

namespace {

/// The updater plus a spy on every signal and the accessors the assertions share.
struct UpdaterHolder
{
    McuUpdater updater;
};

struct Observed : UpdaterHolder
{
    QSignalSpy finished{&updater, &McuUpdater::finished};
    QSignalSpy logLine{&updater, &McuUpdater::logLine};
    QSignalSpy progress{&updater, &McuUpdater::progress};
    QSignalSpy phaseChanged{&updater, &McuUpdater::phaseChanged};
    QSignalSpy deviceInfo{&updater, &McuUpdater::deviceInfo};
    QSignalSpy applicationInfo{&updater, &McuUpdater::applicationInfo};
    QSignalSpy powerInfo{&updater, &McuUpdater::powerInfo};
    QSignalSpy applicationRestarted{&updater, &McuUpdater::applicationRestarted};
    QSignalSpy bannerTimeout{&updater, &McuUpdater::bannerTimeout};

    /// Timeouts short enough for a fast suite, long enough that a loaded machine does not retry.
    void fast()
    {
        updater.setFrameTimeoutMs(200);
        updater.setCliTimeoutMs(120);
        updater.setBootloaderCommandTimeoutMs(120);
        updater.setResetSettleMs(20);
        updater.setBannerTimeoutMs(1500);
    }

    bool lastOk() const { return finished.isEmpty() ? false : finished.last().at(0).toBool(); }
    QString lastMessage() const { return finished.isEmpty() ? QString() : finished.last().at(1).toString(); }
    QStringList lines() const
    {
        QStringList result;
        for (const QList<QVariant>& args : logLine)
            result << args.at(0).toString();
        return result;
    }
    QList<qint64> progressDone() const
    {
        QList<qint64> result;
        for (const QList<QVariant>& args : progress)
            result << args.at(0).toLongLong();
        return result;
    }
    QList<Phase> phases() const
    {
        QList<Phase> result;
        for (const QList<QVariant>& args : phaseChanged)
            result << qvariant_cast<Phase>(args.at(0));
        return result;
    }
    McuUpdater::CliFields fieldsOf(const QSignalSpy& spy, int index = 0) const
    {
        return qvariant_cast<McuUpdater::CliFields>(spy.at(index).at(0));
    }
};

/// The board behind a real TCP connection: FakeMcu <- FakeMcuServer <- QTcpSocket <- McuUpdater.
struct TcpRig : Observed
{
    FakeMcu mcu;
    FakeMcuServer server{&mcu};
    QTcpSocket socket;

    bool open()
    {
        if (!server.listen())
            return false;
        socket.connectToHost(QHostAddress::LocalHost, server.port());
        if (!socket.waitForConnected(5000))
            return false;
        socket.setSocketOption(QAbstractSocket::LowDelayOption, 1);
        for (int i = 0; i < 200 && !server.hasClient(); ++i)
            QTest::qWait(10);
        updater.setDevice(&socket);
        fast();
        return server.hasClient();
    }
};

/// The board behind an in-process MemoryLink (no TCP): scripted conversations and the sequence wrap.
struct LinkRig : Observed
{
    FakeMcu mcu;
    MemoryLink link{&mcu};

    LinkRig()
    {
        updater.setDevice(&link);
        fast();
    }
};

#define WAIT_FINISHED(rig) QTRY_COMPARE_WITH_TIMEOUT((rig).finished.count(), 1, 30000)

/// A refused start() or setDevice() logs one warning (a UI that triggers it has a bug); the tests that
/// provoke them on purpose expect exactly that many.
void expectRefusals(int count, const char* what)
{
    for (int i = 0; i < count; ++i)
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("MCU updater: ") + QString::fromLatin1(what)));
}

} // namespace

class tst_mcuupdater : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    // ---- FakeMcu / FakeMcuServer themselves ----
    void fakeMcuApplicationMode();
    void fakeMcuSwallowsHelloBytes();
    void fakeMcuPowerAndRefusal();
    void fakeMcuSilent();
    void fakeMcuUpdaterMode();
    void fakeMcuFaultInjection();
    void fakeMcuServerServesOneClientAtATime();

    // ---- static helpers, defaults ----
    void defaults();
    void settersClamp();
    void parseCliFields();
    void powerFields();
    void formatApplication();

    // ---- info ----
    void infoInTheApplication();
    void infoInTheApplicationAsTx();
    void infoInTheUpdater();
    void infoAnswersTwiceInARow();
    void infoOnASilentDevice();
    void infoWithoutARoleAsksHelp_data();
    void infoWithoutARoleAsksHelp();
    void infoUsesThePartialLastLine();
    void probeWritesCrlfLinesAndAnEmptyLineFirst();

    // ---- flash ----
    void flashThroughTheSocFromTheApplication_data();
    void flashThroughTheSocFromTheApplication();
    void flashOfABoardAlreadyInTheUpdater();
    void flashOnTheSerialPathSendsNoPowerCommand();
    void flashWithoutAutoBootloaderRefusesTheApplication();
    void flashWithoutAutoBootloaderStillFlashesTheUpdater();
    void flashMissingPowerReplyIsNotAnError();
    void flashResumesAfterANack();
    void flashRetriesLostFramesAtTheSameOffset();
    void flashGivesUpAfterTheRetryLimit();
    void flashDefaultRetryLimitIsFive();
    void flashRefusedBootloaderMessage();
    void flashBootloaderWithoutAnyReply();
    void flashSilentDeviceMessage();
    void flashRebootThatNeverAnswers();
    void flashDiscardsWhatTheResetPrints();
    void flashBootloaderMisbehaves_data();
    void flashBootloaderMisbehaves();
    void flashIgnoresStaleAndForeignFrames();
    void flashSurvivesOneByteReads();
    void flashChunkSize();
    void flashProgressValues();
    void flashLeavesTheLastOperationVisible();

    // ---- set role ----
    void setRoleFromTheApplication();
    void setRoleInTheUpdater();
    void setRoleWithoutABanner();

    // ---- banner ----
    void bannerSeenAfterFlash();
    void bannerNotSeenWithNoBanner();
    void bannerVariants_data();
    void bannerVariants();

    // ---- cancel, closing, start conditions ----
    void cancelMidTransfer();
    void cancelBeforeTheFirstWrite();
    void cancelWhenIdleDoesNothing();
    void cancelFromInsideAProgressSlot();
    void deviceClosingMidTransfer();
    void deviceClosedLocallyFinishesTheOperation();
    void startConditions();
    void deviceCannotBeReplacedWhileRunning();

    // ---- sequence ----
    void sequenceCountsRequestsAndWraps();
};

void tst_mcuupdater::initTestCase()
{
    qRegisterMetaType<McuUpdater::CliFields>();
    qRegisterMetaType<McuUpdater::Phase>();
}

// ============================================================ FakeMcu / FakeMcuServer themselves

void tst_mcuupdater::fakeMcuApplicationMode()
{
    FakeMcu mcu;
    QSignalSpy available(&mcu, &FakeMcu::outputAvailable);
    QVERIFY(!mcu.inUpdater());
    QVERIFY(!mcu.hasOutput());
    mcu.feed("version\r\n");
    QCOMPARE(mcu.out(), QByteArray("OK version=0.8.1 role=rx\r\n"));
    QCOMPARE(available.count(), 1);
    QVERIFY(mcu.hasOutput());
    QCOMPARE(mcu.take(5), QByteArray("OK ve"));                      // take() hands out at most `size`
    QCOMPARE(mcu.take(1000), QByteArray("rsion=0.8.1 role=rx\r\n"));
    QVERIFY(!mcu.hasOutput());
    mcu.setRole(2);
    mcu.feed("version\n");                                          // a bare LF ends a line too
    QCOMPARE(mcu.take(1000), QByteArray("OK version=0.8.1 role=tx\r\n"));
    mcu.feed("ver");                                                // a line is complete only at "\n"
    QVERIFY(!mcu.hasOutput());
    mcu.feed("sion\r\nfrobnicate\r\nhelp\r\n");
    const QByteArray helpHead = "OK commands:\r\n"
                                "OK help [command]\r\n"
                                "OK version show firmware version\r\n"
                                "OK power power | power off [delay_ms] | power soc on|off\r\n"
                                "OK bootloader reboot into UART firmware updater\r\n";
    QCOMPARE(mcu.take(2000), QByteArray("OK version=0.8.1 role=tx\r\n"
                                        "ERR CMD unknown_command; use help\r\n")
                                 + helpHead + "OK tx tx sync_frame <exposure_hex> | tx sync_time <utc_hex> | tx status\r\n");
    mcu.setRole(1);
    mcu.feed("help\r\n");
    QCOMPARE(mcu.take(2000), helpHead + "OK rx rx status | rx dump on|off\r\n");
    QCOMPARE(mcu.commands(), QStringList({"version", "version", "version", "frobnicate", "help", "help"}));
}

void tst_mcuupdater::fakeMcuSwallowsHelloBytes()
{
    FakeMcu mcu;
    mcu.feed(encodeFrame(Hello, 1));                                // the application cannot parse these
    mcu.feed(encodeFrame(Hello, 1));
    mcu.feed("\r\n");                                               // the probe's empty line ends that "line"
    mcu.feed("\r\n   \r\n");                                        // empty lines are ignored too
    QVERIFY(!mcu.hasOutput());
    QVERIFY(mcu.commands().isEmpty());
    QCOMPARE(mcu.framesReceived(), 0);                              // nothing was taken for a frame
    mcu.feed("version\r\n");
    QCOMPARE(mcu.commands(), QStringList({"version"}));
}

void tst_mcuupdater::fakeMcuPowerAndRefusal()
{
    FakeMcu mcu;
    mcu.setSoc("on");
    mcu.setSupply("usb");
    mcu.feed("power\r\n");
    QCOMPARE(mcu.take(1000), QByteArray("OK power state=on supply=usb bat=ok usb=1 key=0 latch=1 soc=on charge=on\r\n"));
    mcu.setSupply("bat");
    mcu.setSoc("off");
    mcu.feed("power\r\n");
    QCOMPARE(mcu.take(1000), QByteArray("OK power state=on supply=bat bat=ok usb=0 key=0 latch=1 soc=off charge=on\r\n"));

    mcu.setRefuseBootloader(true);
    mcu.feed("bootloader\r\n");
    QCOMPARE(mcu.take(1000), QByteArray("ERR PWR busy\r\n"));
    QVERIFY(!mcu.inUpdater());
    mcu.setRefuseBootloader(false);
    mcu.feed("bootloader\r\n");
    QCOMPARE(mcu.take(1000), QByteArray("OK rebooting_to_bootloader\r\n"));
    QVERIFY(mcu.inUpdater());
    QCOMPARE(mcu.commands(), QStringList({"power", "power", "bootloader", "bootloader"}));
}

void tst_mcuupdater::fakeMcuSilent()
{
    FakeMcu mcu;
    mcu.setSilent(true);
    mcu.feed("version\r\npower\r\n");
    mcu.feed(encodeFrame(Hello, 1));
    QVERIFY(!mcu.hasOutput());
    QCOMPARE(mcu.commands(), QStringList({"version", "power"}));    // still recorded

    FakeMcu updater;                                                // silent in the updater: frames are counted only
    updater.setInUpdater(true);
    updater.setSilent(true);
    updater.feed(encodeFrame(Hello, 2));
    QVERIFY(!updater.hasOutput());
    QCOMPARE(updater.framesReceived(), 1);
}

void tst_mcuupdater::fakeMcuUpdaterMode()
{
    FakeMcu mcu;
    mcu.setInUpdater(true);
    mcu.setRole(2);
    auto exchange = [&mcu](quint8 type, quint16 sequence, const QByteArray& payload, Frame* answer) {
        mcu.feed(encodeFrame(type, sequence, payload));
        FrameSplitter splitter;
        const QList<Frame> frames = splitter.feed(mcu.take(1 << 20));
        if (frames.isEmpty())
            return false;
        *answer = frames.first();
        return true;
    };
    Frame answer;
    QVERIFY(exchange(Hello, 7, QByteArray(), &answer));
    QCOMPARE(answer.type, quint8(Info));
    QCOMPARE(answer.sequence, quint16(7));
    DeviceInfo info;
    QVERIFY(parseInfo(answer.payload, &info));
    QCOMPARE(info.target, FakeMcu::kTarget);
    QCOMPARE(info.state, 2u);
    QCOMPARE(info.maxSize, 0x17000u);
    QCOMPARE(info.imageSize, 0u);
    QCOMPARE(info.version, FakeMcu::kVersion);
    QCOMPARE(*info.role, 2u);

    const QByteArray header(32, '\x11');
    QVERIFY(exchange(Begin, 8, header + le32(1), &answer));          // the role is payload[32..36]
    QCOMPARE(answer.type, quint8(Ready));
    QCOMPARE(answer.payload, le32(0));
    QCOMPARE(mcu.role(), 1u);
    QVERIFY(exchange(Data, 9, le32(0) + QByteArray("abc"), &answer));
    QCOMPARE(answer.type, quint8(Ack));
    QCOMPARE(answer.payload, le32(3));
    QVERIFY(exchange(Data, 10, le32(0) + QByteArray("abc"), &answer));   // a stale offset: the real one comes back
    QCOMPARE(answer.type, quint8(Ack));
    QCOMPARE(answer.payload, le32(3));
    QCOMPARE(mcu.image(), QByteArray("abc"));
    QVERIFY(exchange(Data, 11, le32(3) + QByteArray("de"), &answer));
    QCOMPARE(answer.payload, le32(5));
    QCOMPARE(mcu.image(), QByteArray("abcde"));
    QVERIFY(exchange(Hello, 12, QByteArray(), &answer));
    QCOMPARE(readLe32(answer.payload, 12), 5u);                      // INFO reports the bytes received so far
    QCOMPARE(mcu.framesReceived(), 6);
    QCOMPARE(mcu.dataFramesReceived(), 3);

    mcu.feed(QByteArray("junk"));                                    // bytes that do not decode are ignored
    mcu.feed(QByteArray(1, '\0'));
    QVERIFY(!mcu.hasOutput());
    QCOMPARE(mcu.framesReceived(), 6);

    mcu.feed(encodeFrame(End, 13));
    FrameSplitter splitter;
    const QByteArray tail = mcu.take(1 << 20);
    const int zero = tail.indexOf('\0');
    QCOMPARE(splitter.feed(tail.left(zero + 1)).first().type, quint8(Complete));
    QCOMPARE(tail.mid(zero + 1), QByteArray("CC1310 boot version=0.8.1 role=rx\r\nOK cli=ready\r\n"));
    QVERIFY(!mcu.inUpdater());                                       // back to the application

    mcu.setInUpdater(true);                                          // SET_ROLE: taken, COMPLETE, banner, application
    QVERIFY(exchange(SetRole, 14, le32(2), &answer));
    QCOMPARE(answer.type, quint8(Complete));
    QCOMPARE(mcu.role(), 2u);
    QVERIFY(!mcu.inUpdater());
}

void tst_mcuupdater::fakeMcuFaultInjection()
{
    FakeMcu mcu;
    mcu.setInUpdater(true);
    mcu.feed(encodeFrame(Begin, 1, QByteArray(36, '\0')));
    mcu.take(1 << 20);
    auto data = [&mcu](quint16 sequence, quint32 offset, int length) -> std::optional<Frame> {
        mcu.feed(encodeFrame(Data, sequence, le32(offset) + QByteArray(length, 'x')));
        FrameSplitter splitter;
        const QList<Frame> frames = splitter.feed(mcu.take(1 << 20));
        return frames.isEmpty() ? std::nullopt : std::optional<Frame>(frames.first());
    };

    mcu.setDropFrames(2);                                           // the first two DATA frames are lost
    QVERIFY(!data(2, 0, 128).has_value());
    QVERIFY(!data(2, 0, 128).has_value());
    QVERIFY(mcu.image().isEmpty());                                 // a lost frame had no effect
    const std::optional<Frame> third = data(2, 0, 128);
    QVERIFY(third.has_value());
    QCOMPARE(third->payload, le32(128));
    QCOMPARE(mcu.dataFramesReceived(), 3);                          // the lost ones were seen by the fake

    mcu.setNackAt(256);                                             // only the first DATA at that offset
    QCOMPARE(data(3, 128, 128)->payload, le32(256));
    const std::optional<Frame> nack = data(4, 256, 128);
    QCOMPARE(nack->type, quint8(Nack));
    QCOMPARE(nack->payload, le32(128));                             // offset minus the chunk
    QCOMPARE(mcu.image().size(), 128);                              // truncated to that point
    QCOMPARE(data(5, 128, 128)->payload, le32(256));                // resumed from there
    QCOMPARE(data(6, 256, 128)->type, quint8(Ack));                 // the second DATA at 256 is accepted
    QCOMPARE(mcu.image().size(), 384);

    mcu.setNoBanner(true);
    mcu.feed(encodeFrame(End, 7));
    FrameSplitter splitter;
    const QList<Frame> frames = splitter.feed(mcu.take(1 << 20));
    QCOMPARE(frames.size(), 1);
    QVERIFY(!mcu.hasOutput());                                      // nothing after COMPLETE
}

void tst_mcuupdater::fakeMcuServerServesOneClientAtATime()
{
    FakeMcu mcu;
    FakeMcuServer server(&mcu);
    QVERIFY(server.listen());
    QVERIFY(server.port() != 0);
    QCOMPARE(server.mcu(), &mcu);
    QVERIFY(!server.hasClient());
    QCOMPARE(server.connectionCount(), 0);
    QSignalSpy connected(&server, &FakeMcuServer::clientConnected);
    QSignalSpy disconnected(&server, &FakeMcuServer::clientDisconnected);

    QTcpSocket first;
    first.connectToHost(QHostAddress::LocalHost, server.port());
    QTRY_VERIFY(server.hasClient());
    QCOMPARE(server.connectionCount(), 1);
    QCOMPARE(connected.count(), 1);
    first.write("version\r\n");
    QTRY_COMPARE(first.bytesAvailable(), qint64(26));
    QCOMPARE(first.readAll(), QByteArray("OK version=0.8.1 role=rx\r\n"));

    QTcpSocket second;                                              // the bridge's flock: it has to wait
    second.connectToHost(QHostAddress::LocalHost, server.port());
    QVERIFY(second.waitForConnected(3000));
    second.write("power\r\n");
    QTest::qWait(100);                                              // negative check: nothing is served yet
    QCOMPARE(server.connectionCount(), 1);
    QCOMPARE(second.bytesAvailable(), qint64(0));

    server.dropClient();                                            // a bridge that died
    QTRY_COMPARE(disconnected.count(), 1);
    QTRY_COMPARE(first.state(), QAbstractSocket::UnconnectedState);
    QTRY_COMPARE(server.connectionCount(), 2);                      // the waiting client is served now
    QTRY_VERIFY(server.hasClient());
    QTRY_VERIFY(mcu.commands().contains("power"));                  // what it wrote while it waited arrives now
    second.write("version\r\n");
    QByteArray got;
    auto answered = [&] { got += second.readAll(); return got.contains("OK version"); };
    QTRY_VERIFY(answered());
    QVERIFY(got.contains("OK power state=on"));
}

// ================================================================= static helpers, defaults

void tst_mcuupdater::defaults()
{
    McuUpdater updater;
    QCOMPARE(updater.frameTimeoutMs(), 500);
    QCOMPARE(updater.frameRetries(), 5);
    QCOMPARE(updater.probeRetries(), 2);
    QCOMPARE(updater.rebootRetries(), 10);
    QCOMPARE(updater.chunkSize(), 128);
    QCOMPARE(updater.cliTimeoutMs(), 1000);
    QCOMPARE(updater.bootloaderCommandTimeoutMs(), 2000);
    QCOMPARE(updater.resetSettleMs(), 500);
    QCOMPARE(updater.bannerTimeoutMs(), 8000);
    QVERIFY(updater.autoBootloader());
    QVERIFY(!updater.throughSoc());
    QVERIFY(updater.device() == nullptr);
    QVERIFY(!updater.package().isValid());
    QVERIFY(!updater.isRunning());
    QCOMPARE(updater.operation(), McuUpdater::Operation::None);
    QCOMPARE(updater.phase(), Phase::Idle);
    QCOMPARE(updater.sequence(), quint16(0));
}

void tst_mcuupdater::settersClamp()
{
    McuUpdater updater;
    updater.setFrameTimeoutMs(321);
    updater.setFrameRetries(3);
    updater.setProbeRetries(4);
    updater.setRebootRetries(6);
    updater.setChunkSize(64);
    updater.setCliTimeoutMs(111);
    updater.setBootloaderCommandTimeoutMs(222);
    updater.setResetSettleMs(33);
    updater.setBannerTimeoutMs(444);
    updater.setThroughSoc(true);
    updater.setAutoBootloader(false);
    QCOMPARE(updater.frameTimeoutMs(), 321);
    QCOMPARE(updater.frameRetries(), 3);
    QCOMPARE(updater.probeRetries(), 4);
    QCOMPARE(updater.rebootRetries(), 6);
    QCOMPARE(updater.chunkSize(), 64);
    QCOMPARE(updater.cliTimeoutMs(), 111);
    QCOMPARE(updater.bootloaderCommandTimeoutMs(), 222);
    QCOMPARE(updater.resetSettleMs(), 33);
    QCOMPARE(updater.bannerTimeoutMs(), 444);
    QVERIFY(updater.throughSoc());
    QVERIFY(!updater.autoBootloader());
    updater.setFrameRetries(0);                                     // a request is sent at least once
    updater.setProbeRetries(-3);
    updater.setRebootRetries(0);
    updater.setChunkSize(0);
    updater.setFrameTimeoutMs(-1);
    QCOMPARE(updater.frameRetries(), 1);
    QCOMPARE(updater.probeRetries(), 1);
    QCOMPARE(updater.rebootRetries(), 1);
    QCOMPARE(updater.chunkSize(), 1);
    QCOMPARE(updater.frameTimeoutMs(), 0);
    updater.setChunkSize(100000);                                   // a payload is a u16 long: offset + data
    QCOMPARE(updater.chunkSize(), 65531);
}

void tst_mcuupdater::parseCliFields()
{
    McuUpdater::CliFields fields = McuUpdater::parseCliFields(QStringLiteral("OK version=0.2.1 role=tx"));
    QCOMPARE(fields.size(), 2);
    QCOMPARE(fields.value("version"), QStringLiteral("0.2.1"));
    QCOMPARE(fields.value("role"), QStringLiteral("tx"));
    fields = McuUpdater::parseCliFields(QStringLiteral("  a=1\t b=x=y   flag  c=  =d "));
    QCOMPARE(fields.value("a"), QStringLiteral("1"));
    QCOMPARE(fields.value("b"), QStringLiteral("x=y"));              // partition("=") keeps the rest
    QVERIFY(fields.contains("c"));
    QCOMPARE(fields.value("c"), QString());
    QCOMPARE(fields.value(""), QStringLiteral("d"));                 // an empty key is a key in Python too
    QVERIFY(!fields.contains("flag"));
    QVERIFY(McuUpdater::parseCliFields(QString()).isEmpty());
    QVERIFY(McuUpdater::parseCliFields(QStringLiteral("OK help")).isEmpty());
}

void tst_mcuupdater::powerFields()
{
    // test_adb_link.py test_power_reply_parsing
    McuUpdater::CliFields fields = McuUpdater::powerFields(
        {QStringLiteral("PWR event=soc soc=on"), QStringLiteral("OK power state=on supply=usb usb=1 soc=on")});
    QCOMPARE(fields.value("soc"), QStringLiteral("on"));
    QCOMPARE(fields.value("supply"), QStringLiteral("usb"));
    QCOMPARE(fields.value("usb"), QStringLiteral("1"));
    QVERIFY(McuUpdater::powerFields({QStringLiteral("ERR PWR unavailable")}).isEmpty());
    QVERIFY(McuUpdater::powerFields({}).isEmpty());
    QVERIFY(McuUpdater::powerFields({QStringLiteral("OK power")}).isEmpty());   // needs "OK power " and a field
    fields = McuUpdater::powerFields({QStringLiteral("OK power soc=off"), QStringLiteral("OK power soc=on")});
    QCOMPARE(fields.value("soc"), QStringLiteral("off"));            // the first matching line
}

void tst_mcuupdater::formatApplication()
{
    QCOMPARE(McuUpdater::formatApplication({}), QStringLiteral("INFO application version=unknown role=unknown"));
    McuUpdater::CliFields fields;
    fields.insert("version", "0.7.2");
    QCOMPARE(McuUpdater::formatApplication(fields), QStringLiteral("INFO application version=0.7.2 role=unknown"));
    fields.insert("role", "tx");
    QCOMPARE(McuUpdater::formatApplication(fields), QStringLiteral("INFO application version=0.7.2 role=tx"));
}

// ===================================================================================== info

void tst_mcuupdater::infoInTheApplication()
{
    TcpRig rig;
    QVERIFY(rig.open());
    QVERIFY(rig.updater.info());
    QVERIFY(rig.updater.isRunning());
    QCOMPARE(rig.updater.operation(), McuUpdater::Operation::Info);
    WAIT_FINISHED(rig);
    const QString expected = QStringLiteral("INFO application version=0.8.1 role=rx state=running (bootloader inactive; "
                                            "'flash' and 'set-role' reboot into it automatically)");
    QVERIFY(rig.lastOk());
    QCOMPARE(rig.lastMessage(), expected);
    QCOMPARE(rig.lines(), QStringList({expected}));
    QCOMPARE(rig.applicationInfo.count(), 1);
    QCOMPARE(rig.fieldsOf(rig.applicationInfo).value("version"), QStringLiteral("0.8.1"));
    QCOMPARE(rig.fieldsOf(rig.applicationInfo).value("role"), QStringLiteral("rx"));
    QCOMPARE(rig.deviceInfo.count(), 0);
    QCOMPARE(rig.mcu.commands(), QStringList({"version"}));          // the HELLO bytes and the empty line left no trace
    QVERIFY(!rig.mcu.inUpdater());                                   // `info` leaves the application running
    QCOMPARE(rig.phases(), QList<Phase>({Phase::ProbeBootloader, Phase::ProbeApplication, Phase::Done}));
    QVERIFY(!rig.updater.isRunning());
    QCOMPARE(rig.updater.phase(), Phase::Done);
}

void tst_mcuupdater::infoInTheApplicationAsTx()
{
    TcpRig rig;
    QVERIFY(rig.open());
    rig.mcu.setRole(2);
    QVERIFY(rig.updater.info());
    WAIT_FINISHED(rig);
    QVERIFY(rig.lastMessage().startsWith(QStringLiteral("INFO application version=0.8.1 role=tx state=running")));
}

void tst_mcuupdater::infoInTheUpdater()
{
    TcpRig rig;
    QVERIFY(rig.open());
    rig.mcu.setInUpdater(true);
    QVERIFY(rig.updater.info());
    WAIT_FINISHED(rig);
    const QString expected = QStringLiteral("INFO target=CC1M(0x4343314D) state=update_requested(2) max_size=94208 "
                                            "image_size=0 version=0.8.0(0x00000800) role=rx(1)");
    QVERIFY(rig.lastOk());
    QCOMPARE(rig.lastMessage(), expected);
    QCOMPARE(rig.lines(), QStringList({expected}));
    QCOMPARE(rig.deviceInfo.count(), 1);
    const DeviceInfo info = qvariant_cast<DeviceInfo>(rig.deviceInfo.first().at(0));
    QCOMPARE(info.state, 2u);
    QCOMPARE(info.maxSize, 94208u);
    QCOMPARE(info.toString(), expected);
    QCOMPARE(rig.applicationInfo.count(), 0);
    QVERIFY(rig.mcu.commands().isEmpty());                           // no CLI traffic at all
    QCOMPARE(rig.mcu.framesReceived(), 1);                           // one HELLO was enough
    QCOMPARE(rig.updater.sequence(), quint16(1));
    QCOMPARE(rig.phases(), QList<Phase>({Phase::ProbeBootloader, Phase::Done}));
}

void tst_mcuupdater::infoAnswersTwiceInARow()
{
    TcpRig rig;
    QVERIFY(rig.open());
    rig.mcu.setInUpdater(true);
    QVERIFY(rig.updater.info());
    expectRefusals(1, "an operation is already running");
    QVERIFY(!rig.updater.info());                                    // busy: refused, nothing queued
    WAIT_FINISHED(rig);
    QVERIFY(rig.updater.info());                                     // free again
    QTRY_COMPARE(rig.finished.count(), 2);
    QCOMPARE(rig.updater.sequence(), quint16(2));                    // the sequence goes on counting
    QCOMPARE(rig.deviceInfo.count(), 2);
    QCOMPARE(rig.mcu.framesReceived(), 2);
}

void tst_mcuupdater::infoOnASilentDevice()
{
    LinkRig rig;
    rig.mcu.setSilent(true);
    QVERIFY(rig.updater.info());
    WAIT_FINISHED(rig);
    QVERIFY(!rig.lastOk());
    // `info` re-raises the updater's timeout (Part B: "no acceptable response after 2 retries")
    QCOMPARE(rig.lastMessage(), QStringLiteral("no acceptable response after 2 retries"));
    QCOMPARE(rig.link.frames().size(), 2);                           // two HELLO attempts
    QCOMPARE(rig.mcu.commands(), QStringList({"version"}));          // then the CLI probe
    QVERIFY(rig.lines().isEmpty());
    QCOMPARE(rig.phases(), QList<Phase>({Phase::ProbeBootloader, Phase::ProbeApplication, Phase::Done}));
    // the probe-retries setter changes the number
    rig.finished.clear();
    rig.updater.setProbeRetries(3);
    QVERIFY(rig.updater.info());
    WAIT_FINISHED(rig);
    QCOMPARE(rig.lastMessage(), QStringLiteral("no acceptable response after 3 retries"));
}

void tst_mcuupdater::infoWithoutARoleAsksHelp_data()
{
    QTest::addColumn<QByteArray>("helpReply");
    QTest::addColumn<QString>("role");
    // The real CLI prints one "OK <name> <usage>" line per command; the role is the one whose
    // command (tx or rx) is listed, taken from the second word of each line (fw_update.py).
    const QByteArray head = "OK commands:\r\nOK help [command]\r\nOK version\r\nOK power\r\n";
    QTest::newRow("tx listed") << QByteArray(head + "OK tx status\r\n") << QStringLiteral("tx");
    QTest::newRow("rx listed") << QByteArray(head + "OK rx status\r\n") << QStringLiteral("rx");
    QTest::newRow("both listed: tx wins") << QByteArray(head + "OK rx status\r\nOK tx status\r\n") << QStringLiteral("tx");
    QTest::newRow("neither listed") << QByteArray(head) << QStringLiteral("unknown");
    QTest::newRow("help refused") << QByteArray("ERR CMD unknown_command; use help\r\n") << QStringLiteral("unknown");
    QTest::newRow("no reply") << QByteArray() << QStringLiteral("unknown");
    // FakeMcu's one-line help puts "rx" in the sixth word, not the second: Python reads "unknown" too
    QTest::newRow("one-line help") << QByteArray("OK help version power bootloader rx\r\n") << QStringLiteral("unknown");
}

void tst_mcuupdater::infoWithoutARoleAsksHelp()
{
    QFETCH(QByteArray, helpReply);
    QFETCH(QString, role);
    LinkRig rig;
    rig.link.onWrite = [helpReply](MemoryLink* link, const QByteArray& chunk) {
        if (chunk == "version\r\n")
            link->inject("pending: noise\r\nOK version=0.7.2\r\n");   // an older application: no role=
        else if (chunk == "help\r\n" && !helpReply.isEmpty())
            link->inject(helpReply);
    };
    QVERIFY(rig.updater.info());
    WAIT_FINISHED(rig);
    QVERIFY(rig.lastOk());
    QCOMPARE(rig.lastMessage(), QStringLiteral("INFO application version=0.7.2 role=%1 state=running (bootloader inactive; "
                                               "'flash' and 'set-role' reboot into it automatically)").arg(role));
    QCOMPARE(rig.fieldsOf(rig.applicationInfo).value("role"), role);
}

void tst_mcuupdater::infoUsesThePartialLastLine()
{
    // _iter_lines() hands over the text that stopped short of a newline when the window closes.
    LinkRig rig;
    rig.link.onWrite = [](MemoryLink* link, const QByteArray& chunk) {
        if (chunk == "version\r\n")
            link->inject("OK version=0.8.1 role=tx");
    };
    QVERIFY(rig.updater.info());
    WAIT_FINISHED(rig);
    QVERIFY(rig.lastOk());
    QVERIFY(rig.lastMessage().startsWith(QStringLiteral("INFO application version=0.8.1 role=tx")));
}

void tst_mcuupdater::probeWritesCrlfLinesAndAnEmptyLineFirst()
{
    LinkRig rig;
    rig.link.onWrite = [](MemoryLink* link, const QByteArray& chunk) {
        if (chunk == "version\r\n")
            link->inject("OK version=0.7.2\r\n");
        else if (chunk == "help\r\n")
            link->inject("OK commands:\r\nOK tx status\r\n");
    };
    QVERIFY(rig.updater.info());
    WAIT_FINISHED(rig);
    // two HELLO attempts (same frame), the empty CRLF line that clears the CLI, then the commands
    const QByteArray hello = encodeFrame(Hello, 1);
    QCOMPARE(rig.link.writes, QList<QByteArray>({hello, hello, "\r\n", "version\r\n", "help\r\n"}));
}

// ==================================================================================== flash

void tst_mcuupdater::flashThroughTheSocFromTheApplication_data()
{
    QTest::addColumn<QString>("soc");
    QTest::addColumn<QString>("supply");
    QTest::addColumn<QString>("usb");
    // test_adb_link.py: every power state goes ahead, power is logged before the reset
    auto row = [](const char* name, const char* soc, const char* supply, const char* usb) {
        QTest::newRow(name) << QString::fromLatin1(soc) << QString::fromLatin1(supply) << QString::fromLatin1(usb);
    };
    row("soc on, usb", "on", "usb", "1");
    row("soc on, supply unknown", "on", "unknown", "0");
    row("soc na, usb", "na", "usb", "1");
    row("soc off, usb", "off", "usb", "1");
}

void tst_mcuupdater::flashThroughTheSocFromTheApplication()
{
    QFETCH(QString, soc);
    QFETCH(QString, supply);
    QFETCH(QString, usb);
    TcpRig rig;
    QVERIFY(rig.open());
    rig.mcu.setSoc(soc);
    rig.mcu.setSupply(supply);
    const CcPackage package = makePackage(1000);
    QVERIFY(package.isValid());
    rig.updater.setPackage(package);
    rig.updater.setThroughSoc(true);
    QVERIFY(rig.updater.flash(1));
    WAIT_FINISHED(rig);

    QVERIFY2(rig.lastOk(), qPrintable(rig.lastMessage()));
    QCOMPARE(rig.lastMessage(), QStringLiteral("COMPLETE"));
    QCOMPARE(rig.mcu.image(), package.image);                        // len(mcu.image) == 1000 and the same bytes
    QCOMPARE(rig.mcu.role(), 1u);
    QCOMPARE(rig.mcu.commands(), QStringList({"version", "power", "bootloader"}));
    QVERIFY(rig.mcu.commands().indexOf("power") < rig.mcu.commands().indexOf("bootloader"));
    QCOMPARE(rig.lines(), QStringList({kSummary1000,
                                       QStringLiteral("INFO power soc=%1 supply=%2 usb=%3").arg(soc, supply, usb),
                                       kRebooting, QStringLiteral("COMPLETE"), kRestartedRx}));
    QCOMPARE(rig.powerInfo.count(), 1);
    QCOMPARE(rig.fieldsOf(rig.powerInfo).value("soc"), soc);
    QCOMPARE(rig.fieldsOf(rig.powerInfo).value("supply"), supply);
    QCOMPARE(rig.fieldsOf(rig.powerInfo).value("usb"), usb);
    QCOMPARE(rig.applicationInfo.count(), 1);
    QCOMPARE(rig.deviceInfo.count(), 1);                             // the INFO that came after the reboot
    QCOMPARE(qvariant_cast<DeviceInfo>(rig.deviceInfo.first().at(0)).maxSize, 0x17000u);
    QCOMPARE(rig.applicationRestarted.count(), 1);
    QCOMPARE(rig.fieldsOf(rig.applicationRestarted).value("version"), QStringLiteral("0.8.1"));
    QCOMPARE(rig.bannerTimeout.count(), 0);
    QCOMPARE(rig.phases(), QList<Phase>({Phase::ProbeBootloader, Phase::ProbeApplication, Phase::ReadPower,
                                         Phase::EnterBootloader, Phase::SettleAfterReset, Phase::RebootProbe,
                                         Phase::Begin, Phase::Transfer, Phase::End, Phase::Banner, Phase::Done}));
    QVERIFY(!rig.mcu.inUpdater());                                   // COMPLETE returned it to the application
}

void tst_mcuupdater::flashOfABoardAlreadyInTheUpdater()
{
    TcpRig rig;
    QVERIFY(rig.open());
    rig.mcu.setInUpdater(true);
    const CcPackage package = makePackage(1000);
    rig.updater.setPackage(package);
    rig.updater.setThroughSoc(true);                                 // even through the SoC: nothing to ask
    QVERIFY(rig.updater.flash(1));
    WAIT_FINISHED(rig);
    QVERIFY2(rig.lastOk(), qPrintable(rig.lastMessage()));
    QVERIFY(rig.mcu.commands().isEmpty());                           // no CLI command was sent
    QCOMPARE(rig.mcu.image(), package.image);
    QCOMPARE(rig.lines(), QStringList({kSummary1000, QStringLiteral("COMPLETE"), kRestartedRx}));   // Part J: no "rebooting" line
    QCOMPARE(rig.powerInfo.count(), 0);
    QCOMPARE(rig.applicationInfo.count(), 0);
    QCOMPARE(rig.phases(), QList<Phase>({Phase::ProbeBootloader, Phase::Begin, Phase::Transfer, Phase::End,
                                         Phase::Banner, Phase::Done}));
    QCOMPARE(rig.mcu.framesReceived(), 1 + 1 + 8 + 1);               // HELLO, BEGIN, 8 DATA, END
}

void tst_mcuupdater::flashOnTheSerialPathSendsNoPowerCommand()
{
    // test_adb_link.py test_the_serial_path_keeps_its_old_behaviour (role 2 = tx)
    TcpRig rig;
    QVERIFY(rig.open());
    rig.mcu.setSoc("on");
    rig.updater.setPackage(makePackage(1000));
    QVERIFY(rig.updater.flash(2));
    WAIT_FINISHED(rig);
    QVERIFY2(rig.lastOk(), qPrintable(rig.lastMessage()));
    QVERIFY(!rig.mcu.commands().contains("power"));
    QCOMPARE(rig.mcu.commands(), QStringList({"version", "bootloader"}));
    QCOMPARE(rig.mcu.role(), 2u);
    QCOMPARE(rig.lines(), QStringList({kSummary1000,
                                       QStringLiteral("INFO application version=0.8.1 role=rx; rebooting it into the UART updater"),
                                       QStringLiteral("COMPLETE"),
                                       QStringLiteral("INFO application restarted: version=0.8.1 role=tx")}));
    QCOMPARE(rig.powerInfo.count(), 0);
}

void tst_mcuupdater::flashWithoutAutoBootloaderRefusesTheApplication()
{
    TcpRig rig;
    QVERIFY(rig.open());
    rig.updater.setPackage(makePackage(1000));
    rig.updater.setAutoBootloader(false);
    QVERIFY(rig.updater.flash(1));
    WAIT_FINISHED(rig);
    QVERIFY(!rig.lastOk());
    QCOMPARE(rig.lastMessage(), QStringLiteral("no bootloader response; put the device in UART update mode "
                                               "(CLI command 'bootloader') or drop --no-auto-bootloader"));
    QVERIFY(rig.mcu.commands().isEmpty());                           // the application was never asked
    QVERIFY(!rig.mcu.inUpdater());
    QCOMPARE(rig.updater.phase(), Phase::Done);
}

void tst_mcuupdater::flashWithoutAutoBootloaderStillFlashesTheUpdater()
{
    TcpRig rig;
    QVERIFY(rig.open());
    rig.mcu.setInUpdater(true);
    const CcPackage package = makePackage(512);
    rig.updater.setPackage(package);
    rig.updater.setAutoBootloader(false);
    QVERIFY(rig.updater.flash(1));
    WAIT_FINISHED(rig);
    QVERIFY2(rig.lastOk(), qPrintable(rig.lastMessage()));
    QCOMPARE(rig.mcu.image(), package.image);
}

void tst_mcuupdater::flashMissingPowerReplyIsNotAnError()
{
    LinkRig rig;
    rig.link.onWrite = [](MemoryLink* link, const QByteArray& chunk) {
        if (chunk != "power\r\n")                                    // the CLI has no `power` (an older firmware)
            link->forward(chunk);
    };
    rig.updater.setPackage(makePackage(1000));
    rig.updater.setThroughSoc(true);
    QVERIFY(rig.updater.flash(1));
    WAIT_FINISHED(rig);
    QVERIFY2(rig.lastOk(), qPrintable(rig.lastMessage()));
    QCOMPARE(rig.powerInfo.count(), 0);
    QVERIFY(!rig.lines().join('\n').contains(QStringLiteral("INFO power")));
    QCOMPARE(rig.mcu.image().size(), 1000);
}

void tst_mcuupdater::flashResumesAfterANack()
{
    TcpRig rig;
    QVERIFY(rig.open());
    rig.mcu.setInUpdater(true);
    rig.mcu.setNackAt(512);
    rig.updater.setFrameTimeoutMs(1000);                             // no spurious retry may disturb the counts
    const CcPackage package = makePackage(1024);
    rig.updater.setPackage(package);
    QVERIFY(rig.updater.flash(1));
    WAIT_FINISHED(rig);
    QVERIFY2(rig.lastOk(), qPrintable(rig.lastMessage()));
    QCOMPARE(rig.mcu.image(), package.image);                        // truncated at 384, re-sent from there
    // a NACK carries the offset the device expects: the loop continues from it
    QCOMPARE(rig.progressDone(), QList<qint64>({128, 256, 384, 512, 384, 512, 640, 768, 896, 1024}));
    QCOMPARE(rig.mcu.dataFramesReceived(), 10);
}

void tst_mcuupdater::flashRetriesLostFramesAtTheSameOffset()
{
    TcpRig rig;
    QVERIFY(rig.open());
    rig.mcu.setInUpdater(true);
    rig.mcu.setDropFrames(2);
    rig.updater.setFrameTimeoutMs(150);
    const CcPackage package = makePackage(1000);
    rig.updater.setPackage(package);
    QVERIFY(rig.updater.flash(1));
    WAIT_FINISHED(rig);
    QVERIFY2(rig.lastOk(), qPrintable(rig.lastMessage()));
    QCOMPARE(rig.mcu.image(), package.image);
    QCOMPARE(rig.mcu.dataFramesReceived(), 8 + 2);                   // the first chunk was sent three times
    QCOMPARE(rig.progressDone().first(), qint64(128));               // and acknowledged once, at the same offset
    QCOMPARE(rig.progressDone().size(), 8);
    // a retry re-sends the same frame, so the sequence number counts requests, not attempts:
    // HELLO, BEGIN, 8 DATA, END
    QCOMPARE(rig.updater.sequence(), quint16(11));
}

void tst_mcuupdater::flashGivesUpAfterTheRetryLimit()
{
    TcpRig rig;
    QVERIFY(rig.open());
    rig.mcu.setInUpdater(true);
    rig.mcu.setDropFrames(1000);
    rig.updater.setFrameTimeoutMs(40);
    rig.updater.setFrameRetries(3);
    rig.updater.setPackage(makePackage(1000));
    QVERIFY(rig.updater.flash(1));
    WAIT_FINISHED(rig);
    QVERIFY(!rig.lastOk());
    QCOMPARE(rig.lastMessage(), QStringLiteral("no acceptable response after 3 retries"));
    QCOMPARE(rig.mcu.dataFramesReceived(), 3);
    QCOMPARE(rig.progress.count(), 0);
    QVERIFY(!rig.updater.isRunning());
    QCOMPARE(rig.updater.phase(), Phase::Done);
    QVERIFY(rig.lines().last() != QStringLiteral("COMPLETE"));
    QCOMPARE(rig.applicationRestarted.count(), 0);                   // no banner wait after a failure
}

void tst_mcuupdater::flashDefaultRetryLimitIsFive()
{
    TcpRig rig;
    QVERIFY(rig.open());
    rig.mcu.setInUpdater(true);
    rig.mcu.setDropFrames(1000);
    rig.updater.setFrameTimeoutMs(40);
    rig.updater.setPackage(makePackage(1000));
    QCOMPARE(rig.updater.frameRetries(), 5);
    QVERIFY(rig.updater.flash(1));
    WAIT_FINISHED(rig);
    QCOMPARE(rig.lastMessage(), QStringLiteral("no acceptable response after 5 retries"));   // Part C's table
    QCOMPARE(rig.mcu.dataFramesReceived(), 5);
}

void tst_mcuupdater::flashRefusedBootloaderMessage()
{
    TcpRig rig;
    QVERIFY(rig.open());
    rig.mcu.setRefuseBootloader(true);
    rig.updater.setPackage(makePackage(1000));
    QVERIFY(rig.updater.flash(1));
    WAIT_FINISHED(rig);
    QVERIFY(!rig.lastOk());
    QCOMPARE(rig.lastMessage(), QStringLiteral("application did not enter the bootloader: ERR PWR busy"));
    QVERIFY(!rig.mcu.inUpdater());
    QCOMPARE(rig.mcu.commands(), QStringList({"version", "bootloader"}));
    QCOMPARE(rig.lines(), QStringList({kSummary1000, kRebooting}));
}

void tst_mcuupdater::flashBootloaderWithoutAnyReply()
{
    LinkRig rig;
    rig.link.onWrite = [](MemoryLink* link, const QByteArray& chunk) {
        if (chunk != "bootloader\r\n")
            link->forward(chunk);                                    // `bootloader` gets no answer at all
    };
    rig.updater.setPackage(makePackage(1000));
    QVERIFY(rig.updater.flash(1));
    WAIT_FINISHED(rig);
    QVERIFY(!rig.lastOk());
    QCOMPARE(rig.lastMessage(), QStringLiteral("application did not enter the bootloader: no reply"));
}

void tst_mcuupdater::flashSilentDeviceMessage()
{
    LinkRig rig;
    rig.mcu.setSilent(true);
    rig.updater.setPackage(makePackage(1000));
    QVERIFY(rig.updater.flash(1));
    WAIT_FINISHED(rig);
    QVERIFY(!rig.lastOk());
    QCOMPARE(rig.lastMessage(), QStringLiteral("neither the bootloader nor the application CLI answered on this port"));
    QCOMPARE(rig.lines(), QStringList({kSummary1000}));
    QCOMPARE(rig.link.frames().size(), 2);                           // two HELLO attempts, then the CLI probe
    QCOMPARE(rig.mcu.commands(), QStringList({"version"}));
}

void tst_mcuupdater::flashRebootThatNeverAnswers()
{
    // the application accepts `bootloader`, the updater never answers HELLO: Part C's
    // "no acceptable response after 10 retries" (here after the configured 3)
    LinkRig rig;
    rig.link.onWrite = [](MemoryLink* link, const QByteArray& chunk) {
        Frame frame;
        if (decodeFrame(chunk, &frame))
            return;                                                  // HELLO into the void
        link->forward(chunk);
    };
    rig.updater.setPackage(makePackage(1000));
    rig.updater.setFrameTimeoutMs(40);
    rig.updater.setRebootRetries(3);
    QVERIFY(rig.updater.flash(1));
    WAIT_FINISHED(rig);
    QVERIFY(!rig.lastOk());
    QCOMPARE(rig.lastMessage(), QStringLiteral("no acceptable response after 3 retries"));
    QCOMPARE(rig.updater.phase(), Phase::Done);
    int hellos = 0;
    for (const Frame& frame : rig.link.frames())
        hellos += frame.type == Hello ? 1 : 0;
    QCOMPARE(hellos, 2 + 3);                                         // probeRetries before, rebootRetries after
}

void tst_mcuupdater::flashDiscardsWhatTheResetPrints()
{
    // The reset prints noise (even something that looks like a frame) while the device settles; the
    // input is dropped before the first HELLO, so none of it can be taken for an answer.
    LinkRig rig;
    rig.updater.setResetSettleMs(120);
    // "bootloader" is written at t0; its reply window lasts 120 ms, then the 120 ms settle begins: the
    // noise arrives inside the settle. The INFO is the answer the first HELLO after the reset (sequence
    // 2) would match, but it claims a 512 byte App slot, so taking it for real would fail the flash.
    rig.link.onWrite = [](MemoryLink* link, const QByteArray& chunk) {
        link->forward(chunk);
        if (chunk == "bootloader\r\n") {
            link->injectAfter(150, QByteArray("\xFF\xFE" "boot noise\r\n"));
            link->injectAfter(180, encodeFrame(Info, 2, infoPayload(kCc1m, 2, 512, 0, 0x800, 1)));
        }
    };
    rig.updater.setPackage(makePackage(1000));
    QVERIFY(rig.updater.flash(1));
    WAIT_FINISHED(rig);
    QVERIFY2(rig.lastOk(), qPrintable(rig.lastMessage()));
    QCOMPARE(rig.deviceInfo.count(), 1);                             // only the real answer
    QCOMPARE(rig.mcu.image().size(), 1000);
}

void tst_mcuupdater::flashBootloaderMisbehaves_data()
{
    QTest::addColumn<int>("mode");
    QTest::addColumn<QString>("message");
    auto row = [](const char* name, int mode, const char* message) {
        QTest::newRow(name) << mode << QString::fromLatin1(message);
    };
    row("INFO of 10 bytes", 0, "invalid INFO response");
    row("READY of 2 bytes", 1, "invalid READY response");
    row("READY with a resume offset", 2, "bootloader requested unsupported resume offset");
    row("ACK of 3 bytes", 3, "invalid ACK/NACK response");
    row("NACK beyond what was sent", 4, "invalid NACK offset");
    row("package target mismatch", 5, "package target does not match device");
    row("package larger than the App slot", 6, "package exceeds device App slot");
    row("END never answered", 7, "no acceptable response after 3 retries");
    row("BEGIN never answered", 8, "no acceptable response after 3 retries");
}

void tst_mcuupdater::flashBootloaderMisbehaves()
{
    QFETCH(int, mode);
    QFETCH(QString, message);
    LinkRig rig;
    rig.link.onWrite = [mode](MemoryLink* link, const QByteArray& chunk) {
        Frame f;
        if (!decodeFrame(chunk, &f))
            return;
        auto say = [&](quint8 type, const QByteArray& payload) { link->inject(encodeFrame(type, f.sequence, payload)); };
        switch (f.type) {
        case Hello:
            say(Info, mode == 0 ? QByteArray(10, '\0')
                                : infoPayload(mode == 5 ? 0x43433152 : kCc1m, 2, mode == 6 ? 512 : 0x17000, 0, 0x800, 1));
            break;
        case Begin:
            if (mode == 8)
                break;
            say(Ready, mode == 1 ? QByteArray(2, '\0') : mode == 2 ? le32(5) : le32(0));
            break;
        case Data:
            say(mode == 4 ? Nack : Ack,
                mode == 3 ? QByteArray(3, '\0') : mode == 4 ? le32(128) : le32(readLe32(f.payload) + f.payload.size() - 4));
            break;
        case End:
            if (mode != 7)
                say(Complete, QByteArray());
            break;
        default:
            break;
        }
    };
    rig.updater.setPackage(makePackage(1000));
    rig.updater.setFrameTimeoutMs(40);
    rig.updater.setFrameRetries(3);
    QVERIFY(rig.updater.flash(1));
    WAIT_FINISHED(rig);
    QVERIFY(!rig.lastOk());
    QCOMPARE(rig.lastMessage(), message);
    QCOMPARE(rig.updater.phase(), Phase::Done);
    if (mode == 5 || mode == 6) {                                    // refused before BEGIN: the device was not touched
        for (const Frame& frame : rig.link.frames())
            QVERIFY(frame.type == Hello);
    }
    if (mode == 7 || mode == 8) {                                    // a retry re-sends the very same frame
        const quint8 type = mode == 7 ? End : Begin;
        QList<Frame> sent;
        for (const Frame& frame : rig.link.frames())
            if (frame.type == type)
                sent.append(frame);
        QCOMPARE(sent.size(), 3);
        QCOMPARE(sent.at(0), sent.at(1));
        QCOMPARE(sent.at(1), sent.at(2));
    }
}

void tst_mcuupdater::flashIgnoresStaleAndForeignFrames()
{
    // An INFO with the wrong sequence, an ERROR frame with the right one, noise, and a frame cut in
    // two reads are all dealt with without costing a retry.
    LinkRig rig;
    rig.link.onWrite = [](MemoryLink* link, const QByteArray& chunk) {
        Frame f;
        if (!decodeFrame(chunk, &f))
            return;
        const QByteArray good = encodeFrame(Info, f.sequence, infoPayload(kCc1m, 3, 94208, 0, 0, 0));
        link->inject(QByteArray("garbage-before-a-zero") + QByteArray(1, '\0'));
        link->inject(encodeFrame(Info, static_cast<quint16>(f.sequence + 5), infoPayload(1, 1, 1, 1, 1, 1)));
        link->inject(encodeFrame(Error, f.sequence, QByteArray(4, '\x01')));
        link->inject(good.left(7));
        link->inject(good.mid(7));
    };
    QVERIFY(rig.updater.info());
    WAIT_FINISHED(rig);
    QVERIFY2(rig.lastOk(), qPrintable(rig.lastMessage()));
    QCOMPARE(rig.deviceInfo.count(), 1);
    QCOMPARE(qvariant_cast<DeviceInfo>(rig.deviceInfo.first().at(0)).state, 3u);
    QCOMPARE(rig.link.writes.size(), 1);                             // the first attempt was answered
    QCOMPARE(rig.lastMessage(), QStringLiteral("INFO target=CC1M(0x4343314D) state=update_in_progress(3) max_size=94208 "
                                               "image_size=0 version=0.0.0(0x00000000) role=unset(0)"));
}

void tst_mcuupdater::flashSurvivesOneByteReads()
{
    // COMPLETE and the banner text arrive glued together, and here one byte at a time
    LinkRig rig;
    rig.mcu.setInUpdater(true);
    rig.link.setDeliverBytes(1);
    const CcPackage package = makePackage(256);
    rig.updater.setPackage(package);
    rig.updater.setFrameTimeoutMs(2000);
    QVERIFY(rig.updater.flash(1));
    WAIT_FINISHED(rig);
    QVERIFY2(rig.lastOk(), qPrintable(rig.lastMessage()));
    QCOMPARE(rig.mcu.image(), package.image);
    QCOMPARE(rig.lines().last(), kRestartedRx);
}

void tst_mcuupdater::flashChunkSize()
{
    TcpRig rig;
    QVERIFY(rig.open());
    rig.mcu.setInUpdater(true);
    rig.updater.setChunkSize(64);
    const CcPackage package = makePackage(1000);
    rig.updater.setPackage(package);
    QVERIFY(rig.updater.flash(1));
    WAIT_FINISHED(rig);
    QVERIFY2(rig.lastOk(), qPrintable(rig.lastMessage()));
    QCOMPARE(rig.mcu.image(), package.image);
    QCOMPARE(rig.mcu.dataFramesReceived(), 16);                      // ceil(1000 / 64)
    QCOMPARE(rig.progressDone().first(), qint64(64));
    QCOMPARE(rig.progressDone().last(), qint64(1000));
}

void tst_mcuupdater::flashProgressValues()
{
    TcpRig rig;
    QVERIFY(rig.open());
    rig.mcu.setInUpdater(true);
    rig.updater.setPackage(makePackage(1000));
    QVERIFY(rig.updater.flash(1));
    WAIT_FINISHED(rig);
    QCOMPARE(rig.progressDone(), QList<qint64>({128, 256, 384, 512, 640, 768, 896, 1000}));
    for (const QList<QVariant>& args : rig.progress)
        QCOMPARE(args.at(1).toLongLong(), qint64(1000));             // the total is the image size
    // progress is emitted per answer, before the next DATA: it precedes COMPLETE in the log
    QCOMPARE(rig.progress.count(), 8);
}

void tst_mcuupdater::flashLeavesTheLastOperationVisible()
{
    TcpRig rig;
    QVERIFY(rig.open());
    rig.mcu.setInUpdater(true);
    rig.updater.setPackage(makePackage(1000));
    QCOMPARE(rig.updater.operation(), McuUpdater::Operation::None);
    QCOMPARE(rig.updater.phase(), Phase::Idle);
    QVERIFY(rig.updater.flash(1));
    QCOMPARE(rig.updater.operation(), McuUpdater::Operation::Flash);
    QCOMPARE(rig.updater.phase(), Phase::ProbeBootloader);           // set before start() returns
    QVERIFY(rig.updater.isRunning());
    WAIT_FINISHED(rig);
    QCOMPARE(rig.updater.operation(), McuUpdater::Operation::Flash);
    QCOMPARE(rig.updater.phase(), Phase::Done);
    QVERIFY(!rig.updater.isRunning());
    QCOMPARE(rig.updater.package().size, 1000u);
}

// ================================================================================== set role

void tst_mcuupdater::setRoleFromTheApplication()
{
    TcpRig rig;
    QVERIFY(rig.open());
    QVERIFY(rig.updater.setRole(2));
    QCOMPARE(rig.updater.operation(), McuUpdater::Operation::SetRole);
    WAIT_FINISHED(rig);
    QVERIFY2(rig.lastOk(), qPrintable(rig.lastMessage()));
    QCOMPARE(rig.lastMessage(), QStringLiteral("COMPLETE"));
    QCOMPARE(rig.mcu.role(), 2u);                                    // the role the fake took from SET_ROLE
    QCOMPARE(rig.mcu.commands(), QStringList({"version", "bootloader"}));
    QCOMPARE(rig.lines(), QStringList({kRebooting, QStringLiteral("COMPLETE"),
                                       QStringLiteral("INFO application restarted: version=0.8.1 role=tx")}));
    QCOMPARE(rig.phases(), QList<Phase>({Phase::ProbeBootloader, Phase::ProbeApplication, Phase::EnterBootloader,
                                         Phase::SettleAfterReset, Phase::RebootProbe, Phase::SetRole, Phase::Banner,
                                         Phase::Done}));
    QCOMPARE(rig.fieldsOf(rig.applicationRestarted).value("role"), QStringLiteral("tx"));
}

void tst_mcuupdater::setRoleInTheUpdater()
{
    TcpRig rig;
    QVERIFY(rig.open());
    rig.mcu.setInUpdater(true);
    rig.mcu.setRole(2);
    QVERIFY(rig.updater.setRole(1));
    WAIT_FINISHED(rig);
    QVERIFY2(rig.lastOk(), qPrintable(rig.lastMessage()));
    QCOMPARE(rig.mcu.role(), 1u);
    QVERIFY(rig.mcu.commands().isEmpty());
    QCOMPARE(rig.lines(), QStringList({QStringLiteral("COMPLETE"), kRestartedRx}));
    QCOMPARE(rig.mcu.framesReceived(), 2);                           // HELLO and SET_ROLE
    QCOMPARE(rig.deviceInfo.count(), 1);
}

void tst_mcuupdater::setRoleWithoutABanner()
{
    TcpRig rig;
    QVERIFY(rig.open());
    rig.mcu.setInUpdater(true);
    rig.mcu.setNoBanner(true);
    rig.updater.setBannerTimeoutMs(300);
    QVERIFY(rig.updater.setRole(1));
    WAIT_FINISHED(rig);
    QVERIFY(rig.lastOk());                                           // the role was set; only the banner is missing
    QCOMPARE(rig.lastMessage(), QStringLiteral("COMPLETE"));
    QCOMPARE(rig.bannerTimeout.count(), 1);
    QCOMPARE(rig.applicationRestarted.count(), 0);
    QCOMPARE(rig.lines().last(), QStringLiteral("INFO application banner not seen within 0 s; check the device manually"));
}

// ===================================================================================== banner

void tst_mcuupdater::bannerSeenAfterFlash()
{
    TcpRig rig;
    QVERIFY(rig.open());
    rig.mcu.setInUpdater(true);
    rig.mcu.setRole(2);
    rig.updater.setPackage(makePackage(256));
    rig.updater.setBannerTimeoutMs(30000);                           // a banner that is seen must not wait for this
    QVERIFY(rig.updater.flash(2));
    QTRY_COMPARE_WITH_TIMEOUT(rig.finished.count(), 1, 5000);
    QVERIFY(rig.lastOk());
    QCOMPARE(rig.applicationRestarted.count(), 1);
    const McuUpdater::CliFields fields = rig.fieldsOf(rig.applicationRestarted);
    QCOMPARE(fields.value("version"), QStringLiteral("0.8.1"));
    QCOMPARE(fields.value("role"), QStringLiteral("tx"));
    QVERIFY(!fields.contains("CC1310"));                             // only key=value tokens
    QCOMPARE(rig.bannerTimeout.count(), 0);
    QCOMPARE(rig.lines().last(), QStringLiteral("INFO application restarted: version=0.8.1 role=tx"));
}

void tst_mcuupdater::bannerNotSeenWithNoBanner()
{
    TcpRig rig;
    QVERIFY(rig.open());
    rig.mcu.setInUpdater(true);
    rig.mcu.setNoBanner(true);
    rig.updater.setPackage(makePackage(256));
    rig.updater.setBannerTimeoutMs(1000);
    QVERIFY(rig.updater.flash(1));
    WAIT_FINISHED(rig);
    QVERIFY2(rig.lastOk(), qPrintable(rig.lastMessage()));           // the image is in; the banner is a note
    QCOMPARE(rig.lastMessage(), QStringLiteral("COMPLETE"));
    QCOMPARE(rig.bannerTimeout.count(), 1);
    QCOMPARE(rig.applicationRestarted.count(), 0);
    QCOMPARE(rig.lines().mid(rig.lines().size() - 2),
             QStringList({QStringLiteral("COMPLETE"),
                          QStringLiteral("INFO application banner not seen within 1 s; check the device manually")}));
    QCOMPARE(rig.phases().mid(rig.phases().size() - 2), QList<Phase>({Phase::Banner, Phase::Done}));
}

void tst_mcuupdater::bannerVariants_data()
{
    QTest::addColumn<QList<QByteArray>>("chunks");
    QTest::addColumn<QString>("line");   // the restarted line, or empty when no banner is recognised
    QTest::addColumn<bool>("early");     // finishes well before the banner deadline

    // The first bytes after a reset can be garbled: the banner is recognised by its fields.
    QTest::newRow("garbled start, ready")
        << QList<QByteArray>({QByteArray("\xFF\xFE" "junk CC1310 boot version=0.9.0 role=tx\r\nOK cli=ready\r\n")})
        << QStringLiteral("INFO application restarted: version=0.9.0 role=tx") << true;
    QTest::newRow("ready arrives before the version line is ignored")
        << QList<QByteArray>({QByteArray("OK cli=ready\r\n"), QByteArray("CC1310 boot version=0.9.0 role=rx\r\n")})
        << QStringLiteral("INFO application restarted: version=0.9.0 role=rx") << false;
    QTest::newRow("last line without a newline")
        << QList<QByteArray>({QByteArray("CC1310 boot version=0.9.1 role=tx")})
        << QStringLiteral("INFO application restarted: version=0.9.1 role=tx") << false;
    QTest::newRow("no role in the banner")
        << QList<QByteArray>({QByteArray("CC1310 boot version=0.8.1\r\nOK cli=ready\r\n")})
        << QStringLiteral("INFO application restarted: version=0.8.1 role=unknown") << true;
    // fields.update(): a line with "version=" adds its fields (a later one overrides), a line
    // without it contributes nothing
    QTest::newRow("second version line overrides")
        << QList<QByteArray>({QByteArray("CC1310 boot version=0.8.1\r\nrole=tx version=0.8.2\r\nOK cli=ready\r\n")})
        << QStringLiteral("INFO application restarted: version=0.8.2 role=tx") << true;
    QTest::newRow("line without version= is ignored")
        << QList<QByteArray>({QByteArray("CC1310 boot version=0.8.1\r\nrole=tx\r\nOK cli=ready\r\n")})
        << QStringLiteral("INFO application restarted: version=0.8.1 role=unknown") << true;
    QTest::newRow("a banner split in two reads")
        << QList<QByteArray>({QByteArray("CC1310 boot ver"), QByteArray("sion=1.0.0 role=rx\r\nOK cli="), QByteArray("ready\r\n")})
        << QStringLiteral("INFO application restarted: version=1.0.0 role=rx") << true;
    QTest::newRow("only noise")
        << QList<QByteArray>({QByteArray("\xFF\xFE\r\nnothing useful\r\n")}) << QString() << false;
}

void tst_mcuupdater::bannerVariants()
{
    QFETCH(QList<QByteArray>, chunks);
    QFETCH(QString, line);
    QFETCH(bool, early);
    LinkRig rig;
    rig.mcu.setInUpdater(true);
    rig.mcu.setNoBanner(true);
    rig.updater.setPackage(makePackage(256));
    rig.updater.setBannerTimeoutMs(early ? 20000 : 700);
    QVERIFY(rig.updater.flash(1));
    // the fake sends no banner of its own; the chunks come while the updater listens for it
    int delay = 150;
    for (const QByteArray& chunk : chunks) {
        rig.link.injectAfter(delay, chunk);
        delay += 30;
    }
    if (early) {
        QTRY_COMPARE_WITH_TIMEOUT(rig.finished.count(), 1, 5000);
    } else {
        WAIT_FINISHED(rig);
    }
    QVERIFY2(rig.lastOk(), qPrintable(rig.lastMessage()));
    if (line.isEmpty()) {
        QCOMPARE(rig.bannerTimeout.count(), 1);
        QVERIFY(rig.lines().last().startsWith(QStringLiteral("INFO application banner not seen within")));
    } else {
        QCOMPARE(rig.bannerTimeout.count(), 0);
        QCOMPARE(rig.lines().last(), line);
    }
}

// ====================================================================== cancel, closing, start

void tst_mcuupdater::cancelMidTransfer()
{
    TcpRig rig;
    QVERIFY(rig.open());
    rig.mcu.setInUpdater(true);
    const CcPackage package = makePackage(4096);
    rig.updater.setPackage(package);
    const QMetaObject::Connection canceller =
        connect(&rig.updater, &McuUpdater::progress, &rig.updater, [&rig](qint64 done, qint64) {
            if (done >= 512 && rig.updater.isRunning())
                rig.updater.cancel();
        });
    QVERIFY(rig.updater.flash(1));
    WAIT_FINISHED(rig);
    QVERIFY(!rig.lastOk());
    QCOMPARE(rig.lastMessage(), QStringLiteral("cancelled"));
    QVERIFY(!rig.updater.isRunning());
    QCOMPARE(rig.updater.phase(), Phase::Done);
    const int sent = rig.mcu.dataFramesReceived();
    QVERIFY(rig.mcu.image().size() < package.image.size());          // stopped half-way
    QTest::qWait(150);                                               // negative check: no more frames, no more signals
    QCOMPARE(rig.mcu.dataFramesReceived(), sent);
    QCOMPARE(rig.finished.count(), 1);
    QVERIFY(!rig.lines().contains(QStringLiteral("COMPLETE")));

    // Part J: a device left in the updater is flashed again from byte 0
    QObject::disconnect(canceller);
    rig.finished.clear();
    rig.progress.clear();
    QVERIFY(rig.mcu.inUpdater());
    QVERIFY(rig.updater.flash(1));
    WAIT_FINISHED(rig);
    QVERIFY2(rig.lastOk(), qPrintable(rig.lastMessage()));
    QCOMPARE(rig.mcu.image(), package.image);
    QCOMPARE(rig.progressDone().first(), qint64(128));
}

void tst_mcuupdater::cancelBeforeTheFirstWrite()
{
    TcpRig rig;
    QVERIFY(rig.open());
    rig.mcu.setInUpdater(true);
    QVERIFY(rig.updater.info());
    rig.updater.cancel();                                            // start() queued its first write; it must not happen
    QCOMPARE(rig.finished.count(), 1);                               // finished() is synchronous
    QVERIFY(!rig.lastOk());
    QCOMPARE(rig.lastMessage(), QStringLiteral("cancelled"));
    QTest::qWait(100);                                               // negative check
    QCOMPARE(rig.mcu.framesReceived(), 0);
    QCOMPARE(rig.finished.count(), 1);
    QVERIFY(rig.updater.info());                                     // and a new operation runs normally
    QTRY_COMPARE(rig.finished.count(), 2);
    QVERIFY(rig.lastOk());
}

void tst_mcuupdater::cancelWhenIdleDoesNothing()
{
    McuUpdater updater;
    QSignalSpy finished(&updater, &McuUpdater::finished);
    QSignalSpy phases(&updater, &McuUpdater::phaseChanged);
    updater.cancel();
    QCOMPARE(finished.count(), 0);
    QCOMPARE(phases.count(), 0);
    QCOMPARE(updater.phase(), Phase::Idle);
}

void tst_mcuupdater::cancelFromInsideAProgressSlot()
{
    // cancel() from a slot of a signal the state machine is emitting must stop it right there: no
    // further DATA is written after the slot returned.
    LinkRig rig;
    rig.mcu.setInUpdater(true);
    rig.updater.setPackage(makePackage(1000));
    connect(&rig.updater, &McuUpdater::progress, &rig.updater, [&rig](qint64 done, qint64) {
        if (done >= 256)
            rig.updater.cancel();
    });
    QVERIFY(rig.updater.flash(1));
    WAIT_FINISHED(rig);
    QCOMPARE(rig.lastMessage(), QStringLiteral("cancelled"));
    int data = 0;
    for (const Frame& frame : rig.link.frames())
        data += frame.type == Data ? 1 : 0;
    QCOMPARE(data, 2);                                               // DATA@0 and DATA@128 only
    QCOMPARE(rig.progress.count(), 2);
}

void tst_mcuupdater::deviceClosingMidTransfer()
{
    TcpRig rig;
    QVERIFY(rig.open());
    rig.mcu.setInUpdater(true);
    rig.updater.setPackage(makePackage(4096));
    bool dropped = false;
    connect(&rig.updater, &McuUpdater::progress, &rig.updater, [&](qint64 done, qint64) {
        if (done >= 256 && !dropped) {
            dropped = true;
            rig.server.dropClient();                                 // the bridge died
        }
    });
    QVERIFY(rig.updater.flash(1));
    WAIT_FINISHED(rig);
    QVERIFY(dropped);
    QVERIFY(!rig.lastOk());
    QCOMPARE(rig.lastMessage(), QStringLiteral("connection closed"));
    QVERIFY(!rig.updater.isRunning());
    QCOMPARE(rig.updater.phase(), Phase::Done);
    QCOMPARE(rig.finished.count(), 1);
}

void tst_mcuupdater::deviceClosedLocallyFinishesTheOperation()
{
    LinkRig rig;
    rig.mcu.setSilent(true);                                         // the operation sits waiting for an answer
    QVERIFY(rig.updater.info());
    QCOMPARE(rig.finished.count(), 0);
    rig.link.close();                                                // aboutToClose
    QCOMPARE(rig.finished.count(), 1);
    QCOMPARE(rig.lastMessage(), QStringLiteral("connection closed"));
    QVERIFY(!rig.lastOk());
    expectRefusals(1, "no open device");
    QVERIFY(!rig.updater.info());                                    // a closed device cannot start anything

    LinkRig remote;                                                  // and the remote side ending the stream
    remote.mcu.setSilent(true);
    QVERIFY(remote.updater.info());
    remote.link.closeRemote();
    QTRY_COMPARE(remote.finished.count(), 1);
    QCOMPARE(remote.lastMessage(), QStringLiteral("connection closed"));
}

void tst_mcuupdater::startConditions()
{
    McuUpdater updater;
    expectRefusals(3, "no open device");
    QVERIFY(!updater.info());                                        // no device
    QVERIFY(!updater.flash(1));
    QVERIFY(!updater.setRole(1));
    QVERIFY(!updater.isRunning());

    LinkRig rig;
    expectRefusals(1, "no valid package set");
    QVERIFY(!rig.updater.flash(1));                                  // no package set
    QCOMPARE(rig.updater.phase(), Phase::Idle);
    rig.updater.setPackage(makePackage(256));
    rig.mcu.setInUpdater(true);
    QVERIFY(rig.updater.flash(1));
    expectRefusals(3, "an operation is already running");
    QVERIFY(!rig.updater.info());                                    // busy
    QVERIFY(!rig.updater.flash(2));
    QVERIFY(!rig.updater.setRole(2));
    WAIT_FINISHED(rig);
    QVERIFY(rig.lastOk());
    QCOMPARE(rig.mcu.role(), 1u);                                    // the refused calls changed nothing
    QCOMPARE(rig.finished.count(), 1);
}

void tst_mcuupdater::deviceCannotBeReplacedWhileRunning()
{
    LinkRig rig;
    rig.mcu.setInUpdater(true);
    QVERIFY(rig.updater.info());
    MemoryLink other;
    expectRefusals(1, "the device is replaced only while idle");
    rig.updater.setDevice(&other);                                   // ignored: "replaced only while idle"
    QCOMPARE(rig.updater.device(), static_cast<QIODevice*>(&rig.link));
    WAIT_FINISHED(rig);
    QVERIFY(rig.lastOk());
    rig.updater.setDevice(&other);
    QCOMPARE(rig.updater.device(), static_cast<QIODevice*>(&other));
    rig.updater.setDevice(nullptr);
    QVERIFY(rig.updater.device() == nullptr);
}

// ===================================================================================== sequence

void tst_mcuupdater::sequenceCountsRequestsAndWraps()
{
    // One-byte chunks of a maximal image: 1 HELLO + 1 BEGIN + 94208 DATA + 1 END = 94211 requests,
    // so the 16-bit sequence number wraps once (65535 -> 0) and ends at 94211 - 65536.
    FakeMcu mcu;
    mcu.setInUpdater(true);
    MemoryLink link(&mcu);
    bool wrapped = false;
    quint16 last = 0;
    bool zeroUsed = false;
    link.onWrite = [&](MemoryLink* l, const QByteArray& chunk) {
        Frame frame;
        if (decodeFrame(chunk, &frame)) {
            if (frame.sequence == 0)
                zeroUsed = true;
            if (last == 0xFFFF && frame.sequence == 0)
                wrapped = true;
            last = frame.sequence;
        }
        l->forward(chunk);
    };
    McuUpdater updater;
    updater.setDevice(&link);
    updater.setFrameTimeoutMs(5000);
    updater.setChunkSize(1);
    updater.setBannerTimeoutMs(5000);
    const CcPackage package = makePackage(int(CcPackage::kMaxImageSize));
    QVERIFY(package.isValid());
    updater.setPackage(package);
    QSignalSpy finished(&updater, &McuUpdater::finished);
    QVERIFY(updater.flash(1));
    QVERIFY(finished.wait(120000) || finished.count() == 1);
    QCOMPARE(finished.count(), 1);
    QVERIFY2(finished.first().at(0).toBool(), qPrintable(finished.first().at(1).toString()));
    QCOMPARE(mcu.image(), package.image);
    QVERIFY(wrapped);
    QVERIFY(zeroUsed);
    QCOMPARE(updater.sequence(), quint16((1 + 1 + int(CcPackage::kMaxImageSize) + 1) & 0xFFFF));
    QCOMPARE(updater.sequence(), quint16(28675));
}

QTEST_GUILESS_MAIN(tst_mcuupdater)
#include "tst_mcuupdater.moc"
