// UI-free test suite for FleetFlasher and FleetReport (src/firmware/FleetFlasher.h, FleetReport.h):
// several Node boards flashed at once, every upgrade_tool addressed with -s <LocationID>. Everything
// runs on FakeToolRunner and FakeUsbBus (tests/support): the seven boards of the owner's bench, their
// ports as Windows prints them, every misbehaviour of the mapping the design names. No adb, no
// upgrade_tool, no board; polls of 10 ms.
#include <QtTest>

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>

#include <functional>
#include <memory>

#include "firmware/AdbTool.h"
#include "firmware/FleetFlasher.h"
#include "firmware/FleetReport.h"
#include "firmware/RockchipTool.h"
#include "firmware/ToolRunner.h"
#include "firmware/UsbProbe.h"
#include "support/FakeToolRunner.h"
#include "support/FakeUsbBus.h"
#include "support/FirmwareFixtures.h"

using namespace FirmwareFixtures;

namespace {

constexpr int kBench = 7;

QString serialOf(int k) { return QLatin1String(kFleetBench[k].serial); }
QString portOf(int k) { return QLatin1String(kFleetBench[k].port); }
QString tagOf(int k) { return QLatin1String(kFleetBench[k].tag); }

QList<QPair<QString, QString>> benchBoards(const QList<int>& which = {0, 1, 2, 3, 4, 5, 6})
{
    QList<QPair<QString, QString>> boards;
    for (const int k : which) {
        boards.append({tagOf(k), serialOf(k)});
    }
    return boards;
}

QStringList benchSerials()
{
    QStringList serials;
    for (int k = 0; k < kBench; ++k) {
        serials << serialOf(k);
    }
    return serials;
}

const QRegularExpression& addressedWl()
{
    static const QRegularExpression pattern(QStringLiteral("^upgrade_tool -s \\d+ WL "));
    return pattern;
}

/// Unpacks "the Node image" (nodeExtractor) into `dir` and returns the rows of `names` in the
/// Node layout, table order.
QList<PartitionImage> nodeRows(const QString& dir, const QStringList& names)
{
    QDir().mkpath(dir);
    {
        // The extractor writes the files at once; its scripted output never runs (the object goes first).
        FakeToolInvocation exf(QStringLiteral("upgrade_tool"),
                               {QStringLiteral("EXF"), QDir::toNativeSeparators(dir + QStringLiteral("/../update.img")),
                                QDir::toNativeSeparators(dir)});
        nodeExtractor(&exf);
    }
    QList<PartitionImage> rows;
    for (const PartitionImage& row : RockchipTool::planPartitions(dir, RockchipTool::parseMtdParts(kNodeMtdParts))) {
        if (names.contains(row.name)) {
            rows << row;
        }
    }
    return rows;
}

/**
 * FakeToolRunner + FakeUsbBus + FleetFlasher: poll 10 ms, tool paths "upgrade_tool" / "adb", the
 * seven bench boards selected, the rows from nodeExtractor in `dir`, the layout kNodeMtdParts (the
 * image's). Every run line and board line ("[TAG SERIAL] text") is collected in `lines`.
 */
struct FleetRig
{
    explicit FleetRig(const QString& dir, const QStringList& rowNames = {QStringLiteral("rootfs")}, bool scriptNow = true)
        : extractDir(dir)
    {
        flasher.setToolPaths(QStringLiteral("upgrade_tool"), QStringLiteral("adb"));
        flasher.setPollIntervalMs(10);
        flasher.setUsbEnumerator([this](quint16 vid) { return bus.enumerate(vid); });
        flasher.setLayout(kNodeMtdParts, true);
        flasher.setImagePath(dir + QStringLiteral("/../update.img"));
        flasher.setPartitions(nodeRows(dir, rowNames));
        flasher.setBoards(benchBoards());
        QObject::connect(&flasher, &FleetFlasher::logLine, &flasher, [this](const QString& line) { lines << line; });
        QObject::connect(&flasher, &FleetFlasher::boardEvent, &flasher,
                         [this](const FleetBoard& board, const QString& text) { lines << board.label() + QLatin1Char(' ') + text; });
        if (scriptNow) {
            scriptFleet(bus, runner);
        }
    }

    /// The bench without scriptFleet(): `absent` boards are not attached, `unauthorized` ones are.
    void addBench(const QSet<int>& absent = {}, const QSet<int>& unauthorized = {})
    {
        for (int k = 0; k < kBench; ++k) {
            bus.addBoard(serialOf(k), portOf(k),
                         absent.contains(k)         ? FakeUsbBus::State::Absent
                         : unauthorized.contains(k) ? FakeUsbBus::State::Unauthorized
                                                    : FakeUsbBus::State::Adb);
        }
        bus.script(runner);
    }

    bool run(int capMs = 10000)
    {
        flasher.start();
        return waitFinished(capMs);
    }

    bool waitFinished(int capMs = 10000)
    {
        QElapsedTimer clock;
        clock.start();
        while (finished.isEmpty() && clock.elapsed() < capMs) {
            QTest::qWait(5);
        }
        return !finished.isEmpty();
    }

    bool ok() const { return !finished.isEmpty() && finished.first().at(0).toBool(); }
    QString message() const { return finished.isEmpty() ? QString() : finished.first().at(1).toString(); }

    FleetBoard boardOf(const QString& serial) const
    {
        for (const FleetBoard& board : flasher.boards()) {
            if (board.serial == serial) {
                return board;
            }
        }
        return FleetBoard();
    }

    int count(const QRegularExpression& pattern) const { return runner.count(pattern); }
    int lineIndex(const QString& text, int from = 0) const
    {
        for (int i = from; i < lines.size(); ++i) {
            if (lines.at(i) == text) {
                return i;
            }
        }
        return -1;
    }
    int lineStarting(const QString& prefix, int from = 0) const
    {
        for (int i = from; i < lines.size(); ++i) {
            if (lines.at(i).startsWith(prefix)) {
                return i;
            }
        }
        return -1;
    }
    bool anyLineContains(const QString& text) const
    {
        for (const QString& line : lines) {
            if (line.contains(text)) {
                return true;
            }
        }
        return false;
    }
    QString dump() const { return message() + QStringLiteral("\n") + lines.join(QLatin1Char('\n')); }

    QString extractDir;
    FakeToolRunner runner;
    FakeUsbBus bus;
    FleetFlasher flasher{&runner};
    QSignalSpy finished{&flasher, &FleetFlasher::finished};
    QStringList lines;
};

FleetFlasher::Snapshot snap(const QStringList& adb, const QMap<QString, quint16>& download = {}, const QStringList& loader = {},
                            const QStringList& maskrom = {})
{
    FleetFlasher::Snapshot s;
    s.adbPorts = adb;
    s.downloadPorts = download;
    s.loaderIds = loader;
    s.maskromIds = maskrom;
    return s;
}

UsbDeviceInfo device(quint16 pid, const QString& lastSegment, const QString& locationPath = QString())
{
    UsbDeviceInfo info;
    info.vid = 0x2207;
    info.pid = pid;
    info.instanceId = QStringLiteral("USB\\VID_2207&PID_%1\\").arg(QString::number(pid, 16).rightJustified(4, QLatin1Char('0')).toUpper())
                      + lastSegment;
    info.locationPath = locationPath;
    return info;
}

RockusbDevice rockusb(const QString& id, RockusbDevice::Mode mode)
{
    RockusbDevice d;
    d.locationId = id;
    d.mode = mode;
    d.vid = 0x2207;
    d.pid = mode == RockusbDevice::Mode::Maskrom ? 0x110c : 0x110e;
    return d;
}

/// Every line that names `--serial ` must carry the ONLY-device clause (the owner's rule 3).
bool serialHintsAreSafe(const QStringList& lines)
{
    for (const QString& line : lines) {
        if (line.contains(QStringLiteral("--serial ")) && !line.contains(QStringLiteral("ONLY device"))) {
            return false;
        }
    }
    return true;
}

} // namespace

Q_DECLARE_METATYPE(FleetBoard)
Q_DECLARE_METATYPE(FleetFlasher::Snapshot)
Q_DECLARE_METATYPE(FleetFlasher::Verdict)

class Tst_fleetflasher : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();

    // ---- The pure parts ----------------------------------------------------------------------
    void compareTable_data();
    void compareTable();
    void snapshotFrom();
    void tagFor();

    // ---- Runs ---------------------------------------------------------------------------------
    void happyPathSevenBoards();
    void wavesAndSlots();
    void ldGateSharesOneListing();
    void boardAbsentIsSkipped();
    void unauthorizedIsSkipped();
    void noneOnAdbExits3();
    void staysOnAdbIsSkipped();
    void leftAdbNoLoaderIsSkipped();
    void dragsNextStopsTheRun();
    void lateArrivalIsReconciled();
    void endOfRunLeftoverIsReported();
    void ghostLoaderStops();
    void noLdLineStopsWithTheDriverHint();
    void otherPortIsPortMismatch();
    void maskromIsFailedOthersGoOn();
    void pnpLagIsPolledThrough();
    void flipDuringLdMapsOnThatPoll();
    void ldLineOnlyStopsAtTheTimeout();
    void adbDevicesConfirmsTheSerial();
    void layoutGateSkipsBeforeReboot();
    void allBoardsSkipsABoardWithoutLayout();
    void foreignLoaderRefusesARealRun();
    void downloadDeviceWithoutLdLineRefuses();
    void recheckFailsBeforeTheWrite();
    void leftLoaderThenBackIsReportedInLoader();
    void recheckBetweenRows();
    void ldCouldNotRunFailsTheBoard();
    void writeFailureLeavesTheBoardInLoader();
    void noSuchDeviceIsAFailure();
    void resetFailureIsFailed();
    void leftLoaderBeforeResetIsNotAWriteFailure();
    void adbNotBackIsNotBack();
    void noWaitEndsAfterTheReset();
    void portMemoryMovedNote();
    void nullEnumeratorIsRefused();
    void cancelDuringMappingFinishesTheBoardInFlight();
    void cancelDuringWritesDrains();
    void drainRdWaitsForTheGateLd();
    void cancelDuringAdbWaitEndsIt();
    void dryRunTouchesNoBoard();
    void allBoardsInAdbOrder();
    void allBoardsRefusesAForeignAdbDevice();
    void rowsCheckedBeforeAnyReboot();
    void adbCannotRunSelectsNobody();
    void baselineLdCannotRunRefuses();
    void silenceLineIsPerBoard();

    // ---- Review 3: the findings and the test gaps ---------------------------------------------
    void maskromAtAMidWaveCheckIsNoLoader();
    void skippedButStillInLoaderIsCorrected();
    void resetFindsNoDevice();
    void baselineMismatchNamesEveryPortAndId();
    void retryLinesNameTheToolPath();
    void writeCannotStartLeavesTheBoardInLoader();
    void resetTimesOutIsAFailedReset();
    void cmdlineFailureSkipsBeforeTheReboot();
    void stopResetFailureLeavesTheBoardInLoader();
    void ldListsButPortShowsAdb();

    // ---- FleetReport --------------------------------------------------------------------------
    void reportTableAndJson();

    // ---- Board profiles (v0.8.0) --------------------------------------------------------------
    void profileGateStopsAMixedBench();
    void profileGateOffWhenPlanUnknown();
    void profileGateIgnoresEnvExemption();
    void overrideMakesEveryBoardThePlan();
    void profileInBoardJson();
    void dryRunSaysProfilesNotRead();
    void profileGateUsesTheMappedPid();
    void overrideContradictsMappedPidStops();

private:
    QString caseDir() const { return m_tempDir.filePath(QStringLiteral("case%1/update.extracted").arg(m_caseNumber)); }

    QTemporaryDir m_tempDir;
    int m_caseNumber = 0;
};

void Tst_fleetflasher::initTestCase()
{
    QVERIFY(m_tempDir.isValid());
    QLoggingCategory::setFilterRules(QStringLiteral("buildai.*.info=false"));
    qRegisterMetaType<FleetFlasher::Snapshot>();
    qRegisterMetaType<FleetFlasher::Verdict>();
}

void Tst_fleetflasher::init()
{
    ++m_caseNumber;
}

// =============================================================================================
// The pure parts
// =============================================================================================

void Tst_fleetflasher::compareTable_data()
{
    using Kind = FleetFlasher::Verdict::Kind;
    using Waiting = FleetFlasher::Verdict::Waiting;
    QTest::addColumn<FleetFlasher::Snapshot>("before");
    QTest::addColumn<FleetFlasher::Snapshot>("after");
    QTest::addColumn<FleetFlasher::Verdict>("expected");
    const QString p1 = QStringLiteral("6&38635FDB&0&3");
    const QString p2 = QStringLiteral("6&38635FDB&0&2");
    const QString q = QStringLiteral("6&3A944CE5&0&4");
    const FleetFlasher::Snapshot base = snap({p1, p2});
    const auto verdict = [](Kind kind, Waiting waiting, const QString& port, const QString& id, const QStringList& newIds,
                            const QString& detail) {
        FleetFlasher::Verdict v;
        v.kind = kind;
        v.waiting = waiting;
        v.port = port;
        v.locationId = id;
        v.newIds = newIds;
        v.detail = detail;
        return v;
    };

    QTest::newRow("still on ADB") << base << base
                                  << verdict(Kind::Pending, Waiting::StillOnAdb, {}, {}, {},
                                             QStringLiteral("no change on the USB bus yet: the board is still on ADB"));
    QTest::newRow("mapped") << base << snap({p2}, {{p1, 0x110e}}, {QStringLiteral("112")})
                            << verdict(Kind::Mapped, Waiting::None, p1, QStringLiteral("112"), {},
                                       QStringLiteral("in Loader mode on port 6&38635FDB&0&3, LocationID 112"));
    QTest::newRow("maskrom") << base << snap({p2}, {{p1, 0x110c}}, {}, {QStringLiteral("113")})
                             << verdict(Kind::Maskrom, Waiting::None, p1, QStringLiteral("113"), {},
                                        QStringLiteral("in MASKROM mode on port 6&38635FDB&0&3, LocationID 113"));
    QTest::newRow("two ADB instances gone") << base << snap({}, {{p1, 0x110e}}, {QStringLiteral("112")})
                                            << verdict(Kind::Ambiguous, Waiting::None, {}, {}, {QStringLiteral("112")},
                                                       QStringLiteral("two boards changed at once: ADB instances gone on ports "
                                                                      "6&38635FDB&0&2, 6&38635FDB&0&3; Loader devices appeared on "
                                                                      "ports 6&38635FDB&0&3; new LocationIDs 112"));
    QTest::newRow("two Loader devices appeared")
        << base << snap({p2}, {{p1, 0x110e}, {q, 0x110e}}, {QStringLiteral("112"), QStringLiteral("154")})
        << verdict(Kind::Ambiguous, Waiting::None, {}, {}, {QStringLiteral("112"), QStringLiteral("154")},
                   QStringLiteral("two boards changed at once: ADB instances gone on ports 6&38635FDB&0&3; Loader devices "
                                  "appeared on ports 6&38635FDB&0&3, 6&3A944CE5&0&4; new LocationIDs 112, 154"));
    QTest::newRow("two new ids only") << base << snap({p1, p2}, {}, {QStringLiteral("112"), QStringLiteral("99")})
                                      << verdict(Kind::Ambiguous, Waiting::None, {}, {}, {QStringLiteral("99"), QStringLiteral("112")},
                                                 QStringLiteral("two boards changed at once: ADB instances gone on ports none; "
                                                                "Loader devices appeared on ports none; new LocationIDs 99, 112"));
    QTest::newRow("ghost on the port") << base << snap({p1, p2}, {{p1, 0x110e}})
                                       << verdict(Kind::Pending, Waiting::GhostOnPort, p1, {}, {},
                                                  QStringLiteral("a Loader device on port 6&38635FDB&0&3 which still lists an ADB "
                                                                 "device"));
    QTest::newRow("foreign Loader with an id") << base << snap({p1, p2}, {{q, 0x110e}}, {QStringLiteral("150")})
                                               << verdict(Kind::ForeignLoader, Waiting::None, q, {}, {QStringLiteral("150")},
                                                          QStringLiteral("a device entered download mode on port 6&3A944CE5&0&4 "
                                                                         "(LocationID 150) while the board is still on ADB: not this "
                                                                         "board"));
    QTest::newRow("foreign Loader without an id") << base << snap({p1, p2}, {{q, 0x110e}})
                                                  << verdict(Kind::ForeignLoader, Waiting::None, q, {}, {},
                                                             QStringLiteral("a device entered download mode on port "
                                                                            "6&3A944CE5&0&4 (no LocationID yet) while the board is "
                                                                            "still on ADB: not this board"));
    QTest::newRow("LD line only is never foreign") << base << snap({p1, p2}, {}, {QStringLiteral("150")})
                                                   << verdict(Kind::Pending, Waiting::LdOnly, {}, QStringLiteral("150"),
                                                              {QStringLiteral("150")},
                                                              QStringLiteral("upgrade_tool LD lists a new LocationID 150 but the "
                                                                             "bus shows no new download-mode device yet"));
    QTest::newRow("port mismatch") << base << snap({p2}, {{q, 0x110e}}, {QStringLiteral("112")})
                                   << verdict(Kind::PortMismatch, Waiting::None, q, {}, {QStringLiteral("112")},
                                              QStringLiteral("the board left port 6&38635FDB&0&3 but a Loader device appeared on "
                                                             "port 6&3A944CE5&0&4"));
    QTest::newRow("left ADB, nothing yet") << base << snap({p2})
                                           << verdict(Kind::Pending, Waiting::LoaderNotEnumerated, p1, {}, {},
                                                      QStringLiteral("the board left ADB (port 6&38635FDB&0&3), waiting for its "
                                                                     "Loader device"));
    QTest::newRow("left ADB, LD ahead") << base << snap({p2}, {}, {QStringLiteral("112")})
                                        << verdict(Kind::Pending, Waiting::LoaderNotEnumerated, p1, {}, {},
                                                   QStringLiteral("the board left ADB (port 6&38635FDB&0&3), waiting for its "
                                                                  "Loader device"));
    QTest::newRow("Loader device, LD not yet") << base << snap({p2}, {{p1, 0x110e}})
                                               << verdict(Kind::Pending, Waiting::LdNotListing, p1, {}, {},
                                                          QStringLiteral("Loader device on port 6&38635FDB&0&3, waiting for "
                                                                         "upgrade_tool LD to list it"));
    const FleetFlasher::Snapshot withForeign = snap({p1, p2}, {{q, 0x110e}}, {QStringLiteral("99")});
    QTest::newRow("a device in both pictures never counts")
        << withForeign << snap({p2}, {{p1, 0x110e}, {q, 0x110e}}, {QStringLiteral("99"), QStringLiteral("112")})
        << verdict(Kind::Mapped, Waiting::None, p1, QStringLiteral("112"), {},
                   QStringLiteral("in Loader mode on port 6&38635FDB&0&3, LocationID 112"));
    QTest::newRow("a device in both pictures, nothing else")
        << withForeign << withForeign
        << verdict(Kind::Pending, Waiting::StillOnAdb, {}, {}, {}, QStringLiteral("no change on the USB bus yet: the board is still on ADB"));
}

void Tst_fleetflasher::compareTable()
{
    QFETCH(FleetFlasher::Snapshot, before);
    QFETCH(FleetFlasher::Snapshot, after);
    QFETCH(FleetFlasher::Verdict, expected);
    const FleetFlasher::Verdict got = FleetFlasher::compare(before, after);
    QCOMPARE(int(got.kind), int(expected.kind));
    QCOMPARE(int(got.waiting), int(expected.waiting));
    QCOMPARE(got.port, expected.port);
    QCOMPARE(got.locationId, expected.locationId);
    QCOMPARE(got.newIds, expected.newIds);
    QCOMPARE(got.detail, expected.detail);
    QVERIFY(got == expected);
}

void Tst_fleetflasher::snapshotFrom()
{
    const QList<UsbDeviceInfo> bus = {
        device(0x0006, QStringLiteral("6&38635FDB&0&2")),
        device(0x0006, QStringLiteral("6&38635FDB&0&1")),
        device(0x0006, QStringLiteral("6&38635FDB&0&1")),                       // listed twice: once
        device(0x110e, QStringLiteral("6&3A944CE5&0&4")),
        device(0x110c, QStringLiteral("6&3A944CE5&0&2")),                       // MASKROM: a download port too
        device(0x0006, QStringLiteral("0225CE3C8D8D6E44")),                     // a serial string, no location: unplaceable
        device(0x0006, QStringLiteral("0225CE3C8D8D6E45"), QStringLiteral("PCIROOT(0)#USB(9)")),   // placed by its location
    };
    UsbDeviceInfo other = device(0x0006, QStringLiteral("7&1&0&1"));
    other.vid = 0x18d1;                                                         // another vendor: not a Rockchip gadget
    const QList<RockusbDevice> listing = {rockusb(QStringLiteral("112"), RockusbDevice::Mode::Loader),
                                          rockusb(QStringLiteral("100"), RockusbDevice::Mode::Loader),
                                          rockusb(QStringLiteral("112"), RockusbDevice::Mode::Loader),
                                          rockusb(QStringLiteral("113"), RockusbDevice::Mode::Maskrom),
                                          rockusb(QStringLiteral("120"), RockusbDevice::Mode::Unknown)};
    const FleetFlasher::Snapshot s = FleetFlasher::Snapshot::from(bus + QList<UsbDeviceInfo>{other}, listing);
    QCOMPARE(s.adbPorts, (QStringList{QStringLiteral("6&38635FDB&0&1"), QStringLiteral("6&38635FDB&0&2"), QStringLiteral("PCIROOT(0)#USB(9)")}));
    QCOMPARE(s.downloadPorts, (QMap<QString, quint16>{{QStringLiteral("6&3A944CE5&0&4"), quint16(0x110e)},
                                                      {QStringLiteral("6&3A944CE5&0&2"), quint16(0x110c)}}));
    QCOMPARE(s.loaderIds, (QStringList{QStringLiteral("100"), QStringLiteral("112")}));   // numeric order, unique
    QCOMPARE(s.maskromIds, QStringList{QStringLiteral("113")});
    QVERIFY(s == FleetFlasher::Snapshot::from(bus + QList<UsbDeviceInfo>{other}, listing));
    QVERIFY(FleetFlasher::Snapshot::from({}, {}) == FleetFlasher::Snapshot());
}

void Tst_fleetflasher::tagFor()
{
    QCOMPARE(FleetFlasher::tagFor(0), QStringLiteral("A"));
    QCOMPARE(FleetFlasher::tagFor(1), QStringLiteral("B"));
    QCOMPARE(FleetFlasher::tagFor(25), QStringLiteral("Z"));
    QCOMPARE(FleetFlasher::tagFor(26), QStringLiteral("AA"));
    QCOMPARE(FleetFlasher::tagFor(27), QStringLiteral("AB"));
    QCOMPARE(FleetFlasher::tagFor(51), QStringLiteral("AZ"));
    QCOMPARE(FleetFlasher::tagFor(52), QStringLiteral("BA"));
    QCOMPARE(FleetFlasher::tagFor(701), QStringLiteral("ZZ"));
    QCOMPARE(FleetFlasher::tagFor(702), QStringLiteral("AAA"));
    QCOMPARE(FleetFlasher::bootloaderPartitions(),
             (QStringList{QStringLiteral("idblock"), QStringLiteral("uboot"), QStringLiteral("env"), QStringLiteral("meta")}));
}

// =============================================================================================
// Runs
// =============================================================================================

void Tst_fleetflasher::happyPathSevenBoards()
{
    FleetRig rig(caseDir());
    QSignalSpy learned(&rig.flasher, &FleetFlasher::portLearned);
    QSignalSpy layouts(&rig.flasher, &FleetFlasher::mtdPartsRead);
    QSignalSpy finishedBoards(&rig.flasher, &FleetFlasher::boardFinished);
    QVERIFY(rig.run());
    QVERIFY2(rig.ok(), qPrintable(rig.dump()));
    QVERIFY2(rig.message().startsWith(QStringLiteral("7 of 7 boards flashed in ")), qPrintable(rig.message()));
    QVERIFY(!rig.message().contains(QStringLiteral("; ")));   // nothing failed, skipped or left over
    QCOMPARE(rig.flasher.phase(), FleetFlasher::Phase::Done);
    QVERIFY(!rig.flasher.isRunning());

    const QList<FleetBoard> boards = rig.flasher.boards();
    QCOMPARE(boards.size(), qsizetype(kBench));
    QMap<QString, QString> serialById;
    for (int k = 0; k < kBench; ++k) {
        const FleetBoard& b = boards.at(k);
        QCOMPARE(b.tag, tagOf(k));                       // selection order
        QCOMPARE(b.serial, serialOf(k));
        QVERIFY2(b.result == FleetBoard::Result::Done, qPrintable(b.detail));
        QCOMPARE(b.port, portOf(k));
        QVERIFY(!b.locationId.isEmpty());
        QVERIFY(b.seconds > 0.0 && b.mappingSeconds > 0.0 && b.writingSeconds > 0.0);
        QCOMPARE(b.written, QStringList{QStringLiteral("rootfs")});
        QVERIFY(b.adbBack);
        QCOMPARE(b.rfmcu, QStringLiteral("pid=412 uart=owned mcu=alive"));
        QCOMPARE(b.uptime, QStringLiteral("up 0 min"));
        QCOMPARE(b.kernel, QStringLiteral("5.10.209+"));
        QVERIFY(b.boardInfo.startsWith(QStringLiteral("Linux version 5.10.209+")));
        QCOMPARE(b.layout, kNodeMtdParts);
        QVERIFY(b.retry.isEmpty() && !b.leftInLoader && b.detail.isEmpty());
        // Exactly one WL, addressed to its own id, and one RD.
        const QStringList writes = rig.bus.writesTo(b.serial);
        QCOMPARE(writes.size(), qsizetype(1));
        QVERIFY2(writes.first().startsWith(QStringLiteral("upgrade_tool -s %1 WL 8064 10240 ").arg(b.locationId)), qPrintable(writes.first()));
        QCOMPARE(rig.bus.resetsTo(b.serial), QStringList{QStringLiteral("upgrade_tool -s %1 RD").arg(b.locationId)});
        serialById.insert(b.locationId, b.serial);
        QCOMPARE(learned.at(k).at(0).toString(), b.serial);
        QCOMPARE(learned.at(k).at(1).toString(), portOf(k));
        QCOMPARE(layouts.at(k).at(0).toString(), b.serial);
        QVERIFY(rig.lineIndex(b.label() + QStringLiteral(" done in ") + QString::number(b.seconds, 'f', 1)
                              + QStringLiteral(" s (mapping %1 s, writing %2 s)")
                                    .arg(QString::number(b.mappingSeconds, 'f', 1), QString::number(b.writingSeconds, 'f', 1)))
                >= 0);
    }
    QCOMPARE(learned.count(), kBench);
    QCOMPARE(layouts.count(), kBench);
    QCOMPARE(finishedBoards.count(), kBench);
    QCOMPARE(serialById.size(), qsizetype(kBench));
    QCOMPARE(rig.bus.unaddressedCommands(), 0);
    QCOMPARE(rig.bus.ldDuringWrites(), 0);
    QCOMPARE(rig.bus.cancelledWrites(), 0);
    QCOMPARE(rig.count(exfPattern()), 0);   // EXF ran in the plan, never here
    // Every WL addressed, every board in a working folder of its own.
    QSet<QString> folders;
    const QStringList commands = rig.runner.invocations();
    for (int i = 0; i < commands.size(); ++i) {
        if (commands.at(i).contains(QStringLiteral(" WL "))) {
            QVERIFY2(addressedWl().match(commands.at(i)).hasMatch(), qPrintable(commands.at(i)));
            const QString id = commands.at(i).section(QLatin1Char(' '), 2, 2);
            const QString folder = rig.runner.optionsAt(i).workingDirectory;
            QVERIFY2(folder.endsWith(QStringLiteral("/.fleet/") + serialById.value(id)), qPrintable(folder));
            QVERIFY(QFileInfo(folder).isDir());
            folders.insert(folder);
            QCOMPARE(rig.runner.optionsAt(i).timeoutMs, 0);   // a WL is never timed out
        }
    }
    QCOMPARE(folders.size(), qsizetype(kBench));
    QVERIFY(rig.flasher.unattributedDevices().isEmpty());
    QVERIFY(rig.flasher.mappingSeconds() > 0.0 && rig.flasher.writingSeconds() > 0.0);
    QVERIFY(rig.flasher.elapsedSeconds() >= rig.flasher.mappingSeconds());
    const FleetReport report = rig.flasher.report();
    QVERIFY(report.allOk());
    QCOMPARE(report.exitCode(), 0);
    QCOMPARE(report.summary(), rig.message());
    QCOMPARE(rig.flasher.summary(), rig.message());
}

void Tst_fleetflasher::wavesAndSlots()
{
    {
        FleetRig rig(caseDir() + QStringLiteral("-2"));
        rig.flasher.setMaxParallel(2);
        // A slot that frees starts the next board before the wave ends: A hangs until C has written.
        rig.bus.hangWrites(serialOf(0));
        QTimer driver;
        QObject::connect(&driver, &QTimer::timeout, &driver, [&rig]() {
            if (rig.bus.writesTo(serialOf(2)).size() == 1) {
                rig.bus.releaseWrites();
            }
        });
        driver.start(5);
        QVERIFY(rig.run());
        QVERIFY2(rig.ok(), qPrintable(rig.dump()));
        QCOMPARE(rig.bus.maxConcurrentWrites(), 2);
        const int cStarted = rig.lineStarting(QStringLiteral("[C %1] $ upgrade_tool -s ").arg(serialOf(2)));
        const int aWritten = rig.lineStarting(QStringLiteral("[A %1] rootfs written: ").arg(serialOf(0)));
        QVERIFY2(cStarted >= 0 && aWritten > cStarted, qPrintable(rig.dump()));
        QCOMPARE(rig.bus.ldDuringWrites(), 0);
    }
    {
        FleetRig rig(caseDir() + QStringLiteral("-0"));
        rig.flasher.setMaxParallel(0);
        QVERIFY(rig.run());
        QVERIFY2(rig.ok(), qPrintable(rig.dump()));
        QCOMPARE(rig.bus.maxConcurrentWrites(), kBench);
    }
}

void Tst_fleetflasher::ldGateSharesOneListing()
{
    {
        FleetRig rig(caseDir());
        QVERIFY(rig.run());
        QVERIFY2(rig.ok(), qPrintable(rig.dump()));
        const int reconciled = rig.lineIndex(QStringLiteral("reconciled: 7 devices in download mode, every one a mapped board"));
        const int done = rig.lineStarting(QStringLiteral("mapping done: 7 of 7 boards in Loader mode; writing starts (up to 7 at once)"));
        QVERIFY2(reconciled >= 0 && done > reconciled, qPrintable(rig.dump()));
        QVERIFY(rig.lines.at(reconciled - 2) == QStringLiteral("$ upgrade_tool LD"));   // the reconciliation's own picture
        int firstWl = -1;
        for (int i = done; i < rig.lines.size() && firstWl < 0; ++i) {
            if (rig.lines.at(i).contains(QStringLiteral("] $ upgrade_tool -s ")) && rig.lines.at(i).contains(QStringLiteral(" WL "))) {
                firstWl = i;
            }
        }
        QVERIFY(firstWl > done);
        int lds = 0;
        for (int i = done; i < firstWl; ++i) {
            lds += rig.lines.at(i) == QStringLiteral("$ upgrade_tool LD") ? 1 : 0;
        }
        QCOMPARE(lds, 1);   // exactly one: the gate's own, shared by the whole wave
        QCOMPARE(rig.bus.ldDuringWrites(), 0);
        int listed = 0;
        for (const QString& line : std::as_const(rig.lines)) {
            listed += line.contains(QStringLiteral("] checked: LD lists LocationID ")) ? 1 : 0;
        }
        QVERIFY(listed >= kBench);   // the first wave's seven checks read that one listing
    }
    {
        FleetRig rig(caseDir() + QStringLiteral("-every"));
        rig.flasher.setLdPolicy(FleetFlasher::LdPolicy::BeforeEveryWrite);
        QCOMPARE(rig.flasher.ldPolicy(), FleetFlasher::LdPolicy::BeforeEveryWrite);
        QVERIFY(rig.run());
        QVERIFY2(rig.ok(), qPrintable(rig.dump()));
        int checks = 0;
        for (const QString& line : std::as_const(rig.lines)) {
            if (line.contains(QStringLiteral("] checked: "))) {
                ++checks;
                QVERIFY2(line.contains(QStringLiteral("checked: LD lists LocationID ")), qPrintable(line));   // an LD before every write
            }
        }
        QCOMPARE(checks, 2 * kBench);   // before every WL and every RD
        QVERIFY(rig.bus.ldDuringWrites() > 0);   // the knob works: what the bench measures
        QCOMPARE(rig.bus.unaddressedCommands(), 0);
    }
}

void Tst_fleetflasher::boardAbsentIsSkipped()
{
    FleetRig rig(caseDir(), {QStringLiteral("rootfs")}, false);
    rig.addBench({2});
    QVERIFY(rig.run());
    QVERIFY(!rig.ok());
    const FleetBoard c = rig.boardOf(serialOf(2));
    QCOMPARE(c.result, FleetBoard::Result::Skipped);
    QStringList attached = benchSerials();
    attached.removeAt(2);
    QCOMPARE(c.detail, QStringLiteral("not on ADB: board %1 is not attached; attached: %2").arg(serialOf(2), attached.join(QStringLiteral(", "))));
    QVERIFY(rig.lineIndex(c.label() + QLatin1Char(' ') + c.detail + QStringLiteral("; skipped")) >= 0);
    QCOMPARE(c.adbState, QString());
    for (int k = 0; k < kBench; ++k) {
        if (k != 2) {
            QCOMPARE(rig.boardOf(serialOf(k)).result, FleetBoard::Result::Done);
        }
    }
    QVERIFY2(rig.message().startsWith(QStringLiteral("6 of 7 boards flashed in ")) && rig.message().endsWith(QStringLiteral("; 1 skipped")),
             qPrintable(rig.message()));
    QVERIFY(!rig.flasher.report().allOk());
    QCOMPARE(rig.flasher.report().exitCode(), 1);
}

void Tst_fleetflasher::unauthorizedIsSkipped()
{
    FleetRig rig(caseDir(), {QStringLiteral("rootfs")}, false);
    rig.addBench({}, {3});
    QVERIFY(rig.run());
    const FleetBoard d = rig.boardOf(serialOf(3));
    QCOMPARE(d.result, FleetBoard::Result::Skipped);
    QCOMPARE(d.adbState, QStringLiteral("unauthorized"));
    QCOMPARE(d.detail, QStringLiteral("not on ADB: board %1 is 'unauthorized', not ready (unauthorized/offline: replug the cable)").arg(serialOf(3)));
    QCOMPARE(rig.count(QRegularExpression(QStringLiteral("^adb -s %1 ").arg(serialOf(3)))), 0);
    QCOMPARE(rig.flasher.report().totals().done, 6);
    QCOMPARE(rig.flasher.report().exitCode(), 1);
}

void Tst_fleetflasher::noneOnAdbExits3()
{
    FleetRig rig(caseDir(), {QStringLiteral("rootfs")}, false);
    rig.bus.script(rig.runner);   // an empty bus
    QVERIFY(rig.run());
    QVERIFY(!rig.ok());
    QCOMPARE(rig.message(), QStringLiteral("no selected board is on ADB (attached: none)"));
    QCOMPARE(rig.flasher.stopReason(), rig.message());
    QCOMPARE(rig.count(QRegularExpression(QStringLiteral("reboot loader"))), 0);
    QCOMPARE(rig.count(QRegularExpression(QStringLiteral("^upgrade_tool"))), 0);
    for (const FleetBoard& board : rig.flasher.boards()) {
        QCOMPARE(board.result, FleetBoard::Result::Skipped);
    }
    QVERIFY(rig.flasher.report().noneOnAdb());
    QCOMPARE(rig.flasher.report().exitCode(), 3);
}

void Tst_fleetflasher::staysOnAdbIsSkipped()
{
    FleetRig rig(caseDir());
    rig.flasher.setLoaderWaitSeconds(1);
    rig.bus.setReboot(serialOf(1), FakeUsbBus::Reboot::StaysOnAdb);
    QVERIFY(rig.run());
    const FleetBoard b = rig.boardOf(serialOf(1));
    QCOMPARE(b.result, FleetBoard::Result::Skipped);
    QCOMPARE(b.detail, QStringLiteral("reboot loader had no effect: board %1 is still on ADB after 1 s").arg(serialOf(1)));
    QVERIFY(rig.bus.writesTo(serialOf(1)).isEmpty());
    QCOMPARE(rig.flasher.report().totals().done, 6);
    QCOMPARE(rig.bus.unaddressedCommands(), 0);
}

void Tst_fleetflasher::leftAdbNoLoaderIsSkipped()
{
    FleetRig rig(caseDir());
    rig.flasher.setLoaderWaitSeconds(1);
    rig.bus.setReboot(serialOf(1), FakeUsbBus::Reboot::Vanishes);
    QVERIFY(rig.run());
    const FleetBoard b = rig.boardOf(serialOf(1));
    QCOMPARE(b.result, FleetBoard::Result::Skipped);
    QCOMPARE(b.detail, QStringLiteral("board %1 left ADB (port %2) but no Loader device appeared within 1 s; it may be rebooting: check it "
                                      "by hand")
                           .arg(serialOf(1), portOf(1)));
    QCOMPARE(rig.flasher.report().totals().done, 6);
    QVERIFY(rig.flasher.stopReason().isEmpty());
}

void Tst_fleetflasher::dragsNextStopsTheRun()
{
    FleetRig rig(caseDir());
    rig.bus.setReboot(serialOf(2), FakeUsbBus::Reboot::DragsNext);   // C takes D into Loader mode with it
    QVERIFY(rig.run());
    QVERIFY(!rig.ok());
    QCOMPARE(rig.count(QRegularExpression(QStringLiteral(" WL "))), 0);
    const QString reason = QStringLiteral("while rebooting %1: two boards changed at once").arg(serialOf(2));
    QVERIFY2(rig.message().startsWith(QStringLiteral("mapping stopped: ") + reason), qPrintable(rig.message()));
    QVERIFY2(rig.message().endsWith(QStringLiteral("; nothing was written; 2 boards reset back to ADB; 2 Loader devices left unattributed")),
             qPrintable(rig.message()));
    QVERIFY(rig.flasher.stopReason().startsWith(QStringLiteral("mapping stopped: ") + reason));
    // The mapped boards are reset with their own ids; the two that changed at once are never reset.
    for (const int k : {0, 1}) {
        const FleetBoard mapped = rig.boardOf(serialOf(k));
        QCOMPARE(rig.bus.resetsTo(serialOf(k)), QStringList{QStringLiteral("upgrade_tool -s %1 RD").arg(mapped.locationId)});
        QCOMPARE(mapped.result, FleetBoard::Result::Skipped);
        QVERIFY(mapped.detail.startsWith(QStringLiteral("mapping stopped: ") + reason));
    }
    QVERIFY(rig.bus.resetsTo(serialOf(2)).isEmpty());
    QVERIFY(rig.bus.resetsTo(serialOf(3)).isEmpty());
    const FleetBoard c = rig.boardOf(serialOf(2));
    QCOMPARE(c.result, FleetBoard::Result::Failed);
    QVERIFY(c.detail.startsWith(reason));
    for (int k = 3; k < kBench; ++k) {
        QCOMPARE(rig.boardOf(serialOf(k)).result, FleetBoard::Result::Skipped);
        QVERIFY(rig.boardOf(serialOf(k)).detail.startsWith(QStringLiteral("mapping stopped: ") + reason));
    }
    const QString dragged = rig.bus.board(serialOf(3)).locationId;
    bool found = false;
    for (const FleetFlasher::UnattributedDevice& device : rig.flasher.unattributedDevices()) {
        found = found || device.locationId == dragged;
        QCOMPARE(device.hint(), QStringLiteral("upgrade_tool -s %1 RD").arg(device.locationId));
    }
    QVERIFY(found);
    QCOMPARE(rig.flasher.unattributedDevices().size(), qsizetype(2));
    QVERIFY(rig.anyLineContains(QStringLiteral("LocationID %1 is in Loader mode and belongs to no mapped board: reset it with `upgrade_tool -s %1 RD`").arg(dragged)));
    QCOMPARE(rig.flasher.report().resetCount(), 2);
    QCOMPARE(rig.bus.unaddressedCommands(), 0);
}

void Tst_fleetflasher::lateArrivalIsReconciled()
{
    {
        FleetRig rig(caseDir());
        rig.flasher.setLoaderWaitSeconds(1);
        rig.bus.setReboot(serialOf(6), FakeUsbBus::Reboot::Vanishes);
        // G enters Loader mode after its own wait ended: after every `before`, before the reconciliation.
        QObject::connect(&rig.flasher, &FleetFlasher::boardFinished, &rig.flasher, [&rig](const FleetBoard& board) {
            if (board.serial == serialOf(6)) {
                rig.bus.moveToLoader(serialOf(6));
            }
        });
        QVERIFY(rig.run());
        QVERIFY(!rig.ok());
        const QString n = rig.bus.board(serialOf(6)).locationId;
        QVERIFY(!n.isEmpty());
        const QString reason = QStringLiteral("a Loader device appeared late on port %1 (LocationID %2) and belongs to no mapped board").arg(portOf(6), n);
        QCOMPARE(rig.message(), QStringLiteral("mapping stopped: ") + reason
                                    + QStringLiteral("; nothing was written; 6 boards reset back to ADB; 1 Loader device left unattributed"));
        QCOMPARE(rig.count(QRegularExpression(QStringLiteral(" WL "))), 0);
        for (int k = 0; k < 6; ++k) {
            QCOMPARE(rig.bus.resetsTo(serialOf(k)).size(), qsizetype(1));
        }
        QVERIFY(rig.bus.resetsTo(serialOf(6)).isEmpty());   // an identity the fleet did not learn: never reset
        QCOMPARE(rig.flasher.unattributedDevices().size(), qsizetype(1));
        QCOMPARE(rig.flasher.unattributedDevices().first().locationId, n);
        QCOMPARE(rig.flasher.unattributedDevices().first().port, portOf(6));
        QVERIFY(rig.boardOf(serialOf(6)).detail.endsWith(QStringLiteral("; it is in Loader mode now (port %1, LocationID %2)").arg(portOf(6), n)));
    }
    {
        FleetRig rig(caseDir() + QStringLiteral("-quiet"));
        rig.flasher.setLoaderWaitSeconds(1);
        rig.bus.setReboot(serialOf(6), FakeUsbBus::Reboot::Vanishes);
        QVERIFY(rig.run());
        QVERIFY(rig.lineIndex(QStringLiteral("reconciled: 6 devices in download mode, every one a mapped board")) >= 0);
        QCOMPARE(rig.flasher.report().totals().done, 6);
        QCOMPARE(rig.boardOf(serialOf(6)).result, FleetBoard::Result::Skipped);
    }
}

void Tst_fleetflasher::endOfRunLeftoverIsReported()
{
    FleetRig rig(caseDir());
    const QString stray = QStringLiteral("9&FFFF0000&0&1");
    int waits = 0;
    QObject::connect(&rig.flasher, &FleetFlasher::boardEvent, &rig.flasher, [&rig, &waits, &stray](const FleetBoard&, const QString& text) {
        if (text.startsWith(QStringLiteral("waiting for ADB")) && ++waits == kBench) {
            rig.bus.addForeignLoader(stray);   // during the last board's ADB wait
        }
    });
    QVERIFY(rig.run());
    QVERIFY2(rig.ok(), qPrintable(rig.dump()));
    QVERIFY2(rig.message().startsWith(QStringLiteral("7 of 7 boards flashed in ")), qPrintable(rig.message()));
    QVERIFY2(rig.message().endsWith(QStringLiteral("; 1 Loader device left unattributed")), qPrintable(rig.message()));
    QCOMPARE(rig.flasher.unattributedDevices().size(), qsizetype(1));
    const FleetFlasher::UnattributedDevice leftover = rig.flasher.unattributedDevices().first();
    QCOMPARE(leftover.port, stray);
    QCOMPARE(leftover.mode, QStringLiteral("Loader"));
    QCOMPARE(rig.flasher.report().unattributed(), rig.flasher.unattributedDevices());
    QVERIFY(rig.flasher.report().allOk());
    QVERIFY(rig.anyLineContains(QStringLiteral("LocationID %1 on port %2 is in Loader mode and belongs to no mapped board: reset it with `%3`")
                                    .arg(leftover.locationId, stray, leftover.hint())));
    const QStringList commands = rig.runner.invocations();
    QVERIFY(commands.lastIndexOf(QStringLiteral("upgrade_tool LD")) > commands.lastIndexOf(QRegularExpression(QStringLiteral(".*rfmcu status.*"))));
}

void Tst_fleetflasher::ghostLoaderStops()
{
    FleetRig rig(caseDir());
    rig.flasher.setLoaderWaitSeconds(1);
    rig.bus.setReboot(serialOf(1), FakeUsbBus::Reboot::GhostLoader);
    QVERIFY(rig.run());
    QCOMPARE(rig.message(), QStringLiteral("mapping stopped: a Loader device on port %1 still lists an ADB device after 1 s: cannot tell whose it "
                                           "is; nothing was written; 1 board reset back to ADB")
                                .arg(portOf(1)));
    QCOMPARE(rig.bus.resetsTo(serialOf(0)).size(), qsizetype(1));
    QCOMPARE(rig.boardOf(serialOf(1)).result, FleetBoard::Result::Failed);
    QCOMPARE(rig.count(QRegularExpression(QStringLiteral(" WL "))), 0);
}

void Tst_fleetflasher::noLdLineStopsWithTheDriverHint()
{
    FleetRig rig(caseDir());
    rig.flasher.setLoaderWaitSeconds(1);
    rig.bus.setReboot(serialOf(1), FakeUsbBus::Reboot::NoLdLine);
    QVERIFY(rig.run());
    QList<UsbDeviceInfo> instances;
    for (const UsbDeviceInfo& d : rig.bus.enumerate(0x2207)) {
        if (d.isDownloadMode() && d.portKey() == portOf(1)) {
            instances << d;
        }
    }
    QCOMPARE(instances.size(), qsizetype(1));
    const QString reason = QStringLiteral("a Loader device appeared on port %1 but upgrade_tool LD never listed it within 1 s: %2; board %3 is in "
                                          "Loader mode and must be power-cycled by hand")
                               .arg(portOf(1), UsbProbe::driverHint(instances), serialOf(1));
    QCOMPARE(rig.boardOf(serialOf(1)).detail, reason);
    QVERIFY(rig.message().startsWith(QStringLiteral("mapping stopped: ") + reason));
    QCOMPARE(rig.bus.resetsTo(serialOf(0)).size(), qsizetype(1));   // the mapped board is reset
    QVERIFY(rig.bus.resetsTo(serialOf(1)).isEmpty());
}

void Tst_fleetflasher::otherPortIsPortMismatch()
{
    FleetRig rig(caseDir());
    const QString other = QStringLiteral("7&ABCDEF&0&9");
    rig.bus.setReboot(serialOf(1), FakeUsbBus::Reboot::OtherPort, other);
    QVERIFY(rig.run());
    const QString reason = QStringLiteral("while rebooting %1: the board left port %2 but a Loader device appeared on port %3").arg(serialOf(1), portOf(1), other);
    QVERIFY2(rig.message().startsWith(QStringLiteral("mapping stopped: ") + reason), qPrintable(rig.message()));
    QCOMPARE(rig.boardOf(serialOf(1)).detail, reason);
    QCOMPARE(rig.flasher.unattributedDevices().size(), qsizetype(1));
    QCOMPARE(rig.flasher.unattributedDevices().first().port, other);
    QCOMPARE(rig.count(QRegularExpression(QStringLiteral(" WL "))), 0);
}

void Tst_fleetflasher::maskromIsFailedOthersGoOn()
{
    FleetRig rig(caseDir());
    rig.bus.setReboot(serialOf(2), FakeUsbBus::Reboot::ToMaskrom);
    QVERIFY(rig.run());
    const FleetBoard c = rig.boardOf(serialOf(2));
    QCOMPARE(c.result, FleetBoard::Result::Failed);
    QCOMPARE(c.detail, QStringLiteral("entered MASKROM instead of Loader mode: not flashed in a fleet; flash it alone with `su-fw flash-soc "
                                      "--serial %1` only once `upgrade_tool LD` lists it as the ONLY device in download mode (MASKROM mode "
                                      "writes the whole image with UF: idblock, uboot, env and meta included)")
                           .arg(serialOf(2)));
    QCOMPARE(c.port, portOf(2));
    QVERIFY(rig.bus.writesTo(serialOf(2)).isEmpty() && rig.bus.resetsTo(serialOf(2)).isEmpty());   // left as it is
    QCOMPARE(rig.flasher.report().totals().done, 6);
    QVERIFY2(rig.message().endsWith(QStringLiteral("; 1 failed")), qPrintable(rig.message()));
    QVERIFY(rig.lineIndex(QStringLiteral("reconciled: 7 devices in download mode: 6 mapped boards, 1 in MASKROM mode (not written)")) >= 0);
    QVERIFY(rig.flasher.unattributedDevices().isEmpty());
    QVERIFY(serialHintsAreSafe(rig.lines));
}

void Tst_fleetflasher::pnpLagIsPolledThrough()
{
    FleetRig rig(caseDir());
    rig.bus.setPnpLagMs(50);
    rig.bus.setRebootDelayMs(30);
    QVERIFY(rig.run());
    QVERIFY2(rig.ok(), qPrintable(rig.dump()));
}

void Tst_fleetflasher::flipDuringLdMapsOnThatPoll()
{
    FleetRig rig(caseDir());
    rig.bus.setLdDurationMs(60);
    rig.bus.setRebootDelayMs(30);
    rig.bus.setPnpLagMs(0);
    QVERIFY(rig.run(20000));
    QVERIFY2(rig.ok(), qPrintable(rig.dump()));
    for (int k = 0; k < kBench; ++k) {
        const QString label = QStringLiteral("[%1 %2] ").arg(tagOf(k), serialOf(k));
        const int reboot = rig.lineIndex(label + QStringLiteral("$ adb -s %1 reboot loader").arg(serialOf(k)));
        const int mapped = rig.lineStarting(label + QStringLiteral("in Loader mode on port "), reboot);
        QVERIFY2(reboot >= 0 && mapped > reboot, qPrintable(label));
        int lds = 0;
        for (int i = reboot; i < mapped; ++i) {
            lds += rig.lines.at(i) == QStringLiteral("$ upgrade_tool LD") ? 1 : 0;
        }
        QVERIFY2(lds == 1, qPrintable(label + QString::number(lds)));   // the enumeration is read at LD's result
    }
}

void Tst_fleetflasher::ldLineOnlyStopsAtTheTimeout()
{
    FleetRig rig(caseDir());
    rig.flasher.setLoaderWaitSeconds(1);
    rig.bus.setReboot(serialOf(1), FakeUsbBus::Reboot::LdLineOnly);
    QVERIFY(rig.run());
    const QString n = rig.bus.board(serialOf(1)).locationId;
    QVERIFY(!n.isEmpty());
    QCOMPARE(rig.boardOf(serialOf(1)).detail,
             QStringLiteral("upgrade_tool LD lists LocationID %1 but no download-mode device appeared on the bus within 1 s: cannot tell whose "
                            "it is; board %2 is still on ADB")
                 .arg(n, serialOf(1)));
    QCOMPARE(rig.flasher.unattributedDevices().size(), qsizetype(1));
    QCOMPARE(rig.flasher.unattributedDevices().first().locationId, n);
    QVERIFY(rig.bus.resetsTo(serialOf(1)).isEmpty());
    QCOMPARE(rig.bus.resetsTo(serialOf(0)).size(), qsizetype(1));
    QCOMPARE(rig.count(QRegularExpression(QStringLiteral(" WL "))), 0);
}

void Tst_fleetflasher::adbDevicesConfirmsTheSerial()
{
    FleetRig rig(caseDir());
    rig.bus.setReboot(serialOf(1), FakeUsbBus::Reboot::AdbKeepsListing);
    QVERIFY(rig.run());
    const QString reason = QStringLiteral("board %1 is still on ADB although a Loader device appeared on port %2: not this board").arg(serialOf(1), portOf(1));
    QVERIFY2(rig.message().startsWith(QStringLiteral("mapping stopped: ") + reason), qPrintable(rig.message()));
    QCOMPARE(rig.bus.resetsTo(serialOf(0)).size(), qsizetype(1));
    QVERIFY(rig.bus.resetsTo(serialOf(1)).isEmpty());
    QCOMPARE(rig.flasher.unattributedDevices().size(), qsizetype(1));
    QCOMPARE(rig.flasher.unattributedDevices().first().locationId, rig.bus.board(serialOf(1)).locationId);
    QCOMPARE(rig.flasher.unattributedDevices().first().port, portOf(1));
}

void Tst_fleetflasher::layoutGateSkipsBeforeReboot()
{
    const QString other = QStringLiteral("sfc_nor:64K(env),256K@64K(idblock),256K(uboot),384K(meta),4M(boot),4M(rootfs),3M(userdata)");
    {
        FleetRig rig(caseDir());
        rig.bus.setMtdParts(serialOf(1), other);
        QVERIFY(rig.run());
        const FleetBoard b = rig.boardOf(serialOf(1));
        QCOMPARE(b.result, FleetBoard::Result::Skipped);
        QCOMPARE(b.detail, QStringLiteral("flash layout differs from the plan's (board: %1; plan: %2): not rebooted").arg(other, kNodeMtdParts));
        QCOMPARE(rig.count(QRegularExpression(QStringLiteral("^adb -s %1 reboot loader$").arg(serialOf(1)))), 0);
        QCOMPARE(rig.flasher.report().totals().done, 6);
    }
    {
        // With env among the rows the env written defines the next layout: the board is written.
        FleetRig rig(caseDir() + QStringLiteral("-env"), {QStringLiteral("env"), QStringLiteral("rootfs")});
        rig.bus.setMtdParts(serialOf(1), other);
        QVERIFY(rig.run());
        QVERIFY2(rig.ok(), qPrintable(rig.dump()));
        QCOMPARE(rig.bus.writesTo(serialOf(1)).size(), qsizetype(2));
        QCOMPARE(rig.boardOf(serialOf(1)).written, (QStringList{QStringLiteral("env"), QStringLiteral("rootfs")}));
    }
}

void Tst_fleetflasher::allBoardsSkipsABoardWithoutLayout()
{
    {
        FleetRig rig(caseDir());
        rig.flasher.setAllBoards(true);
        rig.bus.setMtdParts(serialOf(2), QString());   // an eMMC board, another product: no mtdparts
        QVERIFY(rig.run());
        const FleetBoard c = rig.boardOf(serialOf(2));
        QCOMPARE(c.result, FleetBoard::Result::Skipped);
        QCOMPARE(c.detail, QStringLiteral("no flash layout in its /proc/cmdline: not a board this plan can place its rows on; name it with "
                                          "--serials to write it anyway"));
        QCOMPARE(rig.count(QRegularExpression(QStringLiteral("^adb -s %1 reboot loader$").arg(serialOf(2)))), 0);
        QCOMPARE(rig.flasher.report().totals().done, 6);
    }
    {
        FleetRig rig(caseDir() + QStringLiteral("-named"));   // --serials: the owner's explicit choice
        rig.bus.setMtdParts(serialOf(2), QString());
        QVERIFY(rig.run());
        QVERIFY2(rig.ok(), qPrintable(rig.dump()));
        QCOMPARE(rig.count(QRegularExpression(QStringLiteral("^adb -s %1 reboot loader$").arg(serialOf(2)))), 1);
    }
}

void Tst_fleetflasher::foreignLoaderRefusesARealRun()
{
    const QString port = QStringLiteral("9&FFFF0000&0&1");
    {
        FleetRig rig(caseDir());
        rig.bus.addForeignLoader(port);
        QVERIFY(rig.run());
        const QString n = QStringLiteral("100");   // the bus's first per-location id
        QCOMPARE(rig.message(), QStringLiteral("a rockusb device is already in Loader mode on port %1 (LocationID %2) that no selected board "
                                               "accounts for; reset it with `upgrade_tool -s %2 RD` or replug it, then run again")
                                    .arg(port, n));
        QCOMPARE(rig.count(QRegularExpression(QStringLiteral("reboot loader"))), 0);
        QCOMPARE(rig.count(QRegularExpression(QStringLiteral(" WL "))), 0);
        QCOMPARE(rig.flasher.report().exitCode(), 1);
    }
    {
        FleetRig rig(caseDir() + QStringLiteral("-dry"));
        rig.bus.addForeignLoader(port);
        rig.flasher.setDryRun(true);
        QVERIFY(rig.run());
        QVERIFY2(rig.ok(), qPrintable(rig.dump()));
        QCOMPARE(rig.message(), QStringLiteral("dry run: 7 of 7 boards ready"));
        QCOMPARE(rig.flasher.foreignDevices().size(), qsizetype(1));
        QCOMPARE(rig.flasher.foreignDevices().first().port, port);
        QCOMPARE(rig.flasher.report().unattributed(), rig.flasher.foreignDevices());
        QVERIFY(rig.lineIndex(QStringLiteral("foreign Loader device on port %1, LocationID 100: not part of the fleet; reset it with `upgrade_tool "
                                             "-s 100 RD` or replug it")
                                  .arg(port))
                >= 0);
        QCOMPARE(rig.count(QRegularExpression(QStringLiteral("reboot loader"))), 0);
    }
}

void Tst_fleetflasher::downloadDeviceWithoutLdLineRefuses()
{
    const QString port = QStringLiteral("9&EEEE0000&0&2");
    const QString extra = QStringLiteral("eeee000000000001");
    for (const FakeUsbBus::Reboot behaviour : {FakeUsbBus::Reboot::NoLdLine, FakeUsbBus::Reboot::LdLineOnly}) {
        FleetRig rig(caseDir() + QString::number(int(behaviour)));
        // An eighth board, not selected, put into a broken download mode before the run.
        rig.bus.addBoard(extra, port);
        rig.bus.setReboot(extra, behaviour);
        rig.runner.run(QStringLiteral("adb"), {QStringLiteral("-s"), extra, QStringLiteral("reboot"), QStringLiteral("loader")});
        QTRY_VERIFY(behaviour == FakeUsbBus::Reboot::NoLdLine ? rig.bus.board(extra).state == FakeUsbBus::State::Loader
                                                                  : rig.bus.board(extra).ldListed);
        QTest::qWait(30);
        QVERIFY(rig.run());
        if (behaviour == FakeUsbBus::Reboot::NoLdLine) {
            QVERIFY2(rig.message().startsWith(QStringLiteral("a Rockchip device on port %1 is in download mode but upgrade_tool LD does not "
                                                             "list it: ")
                                                  .arg(port)),
                     qPrintable(rig.message()));
        } else {
            QCOMPARE(rig.message(), QStringLiteral("upgrade_tool LD lists LocationID %1 in download mode but the USB enumeration shows no such "
                                                   "device: port mapping impossible (driver?)")
                                        .arg(rig.bus.board(extra).locationId));
        }
        QCOMPARE(rig.count(QRegularExpression(QStringLiteral("^adb -s \\S+ reboot loader$"))), 1);   // only the test's own
    }
}

void Tst_fleetflasher::recheckFailsBeforeTheWrite()
{
    FleetRig rig(caseDir());
    rig.bus.leaveLoaderBeforeWrite(serialOf(1));
    QVERIFY(rig.run());
    const FleetBoard b = rig.boardOf(serialOf(1));
    QCOMPARE(b.result, FleetBoard::Result::Failed);
    QCOMPARE(b.detail, QStringLiteral("left Loader mode before rootfs was written (port %1 shows an ADB device; LD does not list LocationID %2): "
                                      "nothing written for it")
                           .arg(portOf(1), b.locationId));
    QVERIFY(rig.bus.writesTo(serialOf(1)).isEmpty());
    QVERIFY(rig.bus.resetsTo(serialOf(1)).isEmpty());
    QVERIFY(!b.leftInLoader);
    QVERIFY(b.retry.isEmpty());
    QVERIFY(b.written.isEmpty());
    QCOMPARE(rig.flasher.report().totals().done, 6);
}

void Tst_fleetflasher::leftLoaderThenBackIsReportedInLoader()
{
    FleetRig rig(caseDir());
    rig.bus.leaveLoaderBeforeWrite(serialOf(1));
    QObject::connect(&rig.flasher, &FleetFlasher::boardFinished, &rig.flasher, [&rig](const FleetBoard& board) {
        if (board.serial == serialOf(1)) {
            rig.bus.moveToLoader(serialOf(1));   // back in Loader mode on its port: a re-enumeration gap, say
        }
    });
    QVERIFY(rig.run());
    const FleetBoard b = rig.boardOf(serialOf(1));
    QCOMPARE(b.result, FleetBoard::Result::Failed);
    QVERIFY(b.leftInLoader);
    QVERIFY2(b.detail.endsWith(QStringLiteral("; it is in Loader mode now (port %1, LocationID %2)").arg(portOf(1), b.locationId)), qPrintable(b.detail));
    const PartitionImage row = rig.flasher.partitions().first();
    QCOMPARE(b.retry, (QStringList{QStringLiteral("upgrade_tool -s %1 WL 8064 10240 %2").arg(b.locationId, QDir::toNativeSeparators(row.file)),
                                   QStringLiteral("upgrade_tool -s %1 RD").arg(b.locationId)}));
    const QJsonObject json = FleetReport::boardJson(b);
    QCOMPARE(json.value(QStringLiteral("leftInLoader")).toBool(), true);
    QCOMPARE(json.value(QStringLiteral("retry")).toArray().size(), qsizetype(2));
    QVERIFY(rig.flasher.unattributedDevices().isEmpty());   // its own board, by port and id
    QVERIFY(rig.lineIndex(b.label() + QStringLiteral("   retry: ") + b.retry.last()) >= 0);
}

void Tst_fleetflasher::recheckBetweenRows()
{
    FleetRig rig(caseDir(), {QStringLiteral("boot"), QStringLiteral("rootfs")});
    rig.bus.leaveLoaderAfterRows(serialOf(1), 1);
    QVERIFY(rig.run());
    const FleetBoard b = rig.boardOf(serialOf(1));
    QCOMPARE(b.result, FleetBoard::Result::Failed);
    QCOMPARE(b.written, QStringList{QStringLiteral("boot")});
    QCOMPARE(rig.bus.writesTo(serialOf(1)).size(), qsizetype(1));   // the second WL never ran
    const QRegularExpression evidence(QStringLiteral("^left Loader mode before rootfs was written \\(port %1 shows an ADB device; LD not run: \\d+ "
                                                     "writes in flight\\): nothing written for it$")
                                          .arg(QRegularExpression::escape(portOf(1))));
    QVERIFY2(evidence.match(b.detail).hasMatch(), qPrintable(b.detail));
    QVERIFY(rig.bus.resetsTo(serialOf(1)).isEmpty());
    QCOMPARE(rig.flasher.report().totals().done, 6);
    QCOMPARE(rig.boardOf(serialOf(0)).written, (QStringList{QStringLiteral("boot"), QStringLiteral("rootfs")}));
}

void Tst_fleetflasher::ldCouldNotRunFailsTheBoard()
{
    {
        FleetRig rig(caseDir(), {QStringLiteral("rootfs")}, false);
        bool failGate = false;
        QObject::connect(&rig.flasher, &FleetFlasher::logLine, &rig.flasher, [&failGate](const QString& line) {
            if (line.startsWith(QStringLiteral("mapping done:"))) {
                failGate = true;
            }
        });
        rig.runner.addHandler(QRegularExpression(QStringLiteral("^upgrade_tool LD$")), [&rig, &failGate](FakeToolInvocation* i) {
            if (failGate) {
                i->failToStart(QStringLiteral("fake: no upgrade_tool"));
            } else {
                i->emitOutput(rig.bus.ldOutput());
                i->finish(0);
            }
        });
        scriptFleet(rig.bus, rig.runner);
        QVERIFY(rig.run());
        QCOMPARE(rig.count(QRegularExpression(QStringLiteral(" WL "))), 0);
        QCOMPARE(rig.count(QRegularExpression(QStringLiteral("^upgrade_tool -s \\d+ RD$"))), 0);
        const PartitionImage row = rig.flasher.partitions().first();
        for (const FleetBoard& b : rig.flasher.boards()) {
            QCOMPARE(b.result, FleetBoard::Result::Failed);
            QCOMPARE(b.detail, QStringLiteral("upgrade_tool LD could not run before rootfs: fake: no upgrade_tool; nothing written for it"));
            QVERIFY(b.leftInLoader);
            QCOMPARE(b.retry, (QStringList{QStringLiteral("upgrade_tool -s %1 WL 8064 10240 %2").arg(b.locationId, QDir::toNativeSeparators(row.file)),
                                           QStringLiteral("upgrade_tool -s %1 RD").arg(b.locationId)}));
            QVERIFY(rig.lineIndex(b.label() + QStringLiteral("   retry: ") + b.retry.first()) >= 0);
        }
        QVERIFY(!rig.anyLineContains(QStringLiteral("left Loader mode before")));
        QVERIFY(serialHintsAreSafe(rig.lines));
    }
    {
        // The pre-RD form: the LD fails only after the writes (one board, its listing older than 500 ms).
        FleetRig rig(caseDir() + QStringLiteral("-rd"), {QStringLiteral("rootfs")}, false);
        rig.flasher.setBoards(benchBoards({0}));
        bool failGate = false;
        QObject::connect(&rig.flasher, &FleetFlasher::boardEvent, &rig.flasher, [&failGate](const FleetBoard&, const QString& text) {
            if (text.startsWith(QStringLiteral("rootfs written:"))) {
                failGate = true;
            }
        });
        rig.runner.addHandler(QRegularExpression(QStringLiteral("^upgrade_tool LD$")), [&rig, &failGate](FakeToolInvocation* i) {
            if (failGate) {
                i->failToStart(QStringLiteral("fake: no upgrade_tool"));
            } else {
                i->emitOutput(rig.bus.ldOutput());
                i->finish(0);
            }
        });
        scriptFleet(rig.bus, rig.runner);
        rig.bus.setWriteDurationMs(600);
        QVERIFY(rig.run());
        const FleetBoard a = rig.boardOf(serialOf(0));
        QCOMPARE(a.result, FleetBoard::Result::Failed);
        QCOMPARE(a.detail, QStringLiteral("partitions written, but upgrade_tool LD could not run before the reset: fake: no upgrade_tool; reset it "
                                          "by hand with `upgrade_tool -s %1 RD`")
                               .arg(a.locationId));
        QCOMPARE(a.retry, QStringList{QStringLiteral("upgrade_tool -s %1 RD").arg(a.locationId)});
        QCOMPARE(a.written, QStringList{QStringLiteral("rootfs")});
        QVERIFY(a.leftInLoader);
        QVERIFY(rig.bus.resetsTo(serialOf(0)).isEmpty());
    }
}

void Tst_fleetflasher::writeFailureLeavesTheBoardInLoader()
{
    {
        FleetRig rig(caseDir());
        rig.bus.setWriteFailure(serialOf(1), QStringLiteral("rootfs.img"), "Write LBA failed, err=-3\r\n", 1);
        QVERIFY(rig.run());
        const FleetBoard b = rig.boardOf(serialOf(1));
        const QString n = b.locationId;
        QCOMPARE(b.result, FleetBoard::Result::Failed);
        QCOMPARE(b.detail, QStringLiteral("writing rootfs failed: Write LBA failed, err=-3"));
        QVERIFY(b.leftInLoader);
        QVERIFY(rig.bus.resetsTo(serialOf(1)).isEmpty());   // no RD: nothing else is written to it
        const PartitionImage row = rig.flasher.partitions().first();
        const QStringList retry = {QStringLiteral("upgrade_tool -s %1 WL 8064 10240 %2").arg(n, QDir::toNativeSeparators(row.file)),
                                   QStringLiteral("upgrade_tool -s %1 RD").arg(n)};
        QCOMPARE(b.retry, retry);
        const int failed = rig.lineIndex(b.label() + QStringLiteral(" writing rootfs failed: Write LBA failed, err=-3"));
        const QString hint = QStringLiteral("left in Loader mode on port %1, LocationID %2: nothing else is written to it. Retry by hand, addressed: "
                                            "one `upgrade_tool -s %2 WL <sector> <count> <file>` per missing row, then `upgrade_tool -s %2 RD` "
                                            "(the lines follow); `su-fw flash-soc --serial %3` only once `upgrade_tool LD` lists it as the ONLY "
                                            "device in download mode (the single-board flow writes without -s and takes any Loader device)")
                                 .arg(portOf(1), n, serialOf(1));
        QVERIFY2(failed >= 0, qPrintable(rig.dump()));
        QCOMPARE(rig.lines.at(failed + 1), b.label() + QLatin1Char(' ') + hint);
        QCOMPARE(rig.lines.at(failed + 2), b.label() + QStringLiteral("   retry: ") + retry.at(0));
        QCOMPARE(rig.lines.at(failed + 3), b.label() + QStringLiteral("   retry: ") + retry.at(1));
        QVERIFY(serialHintsAreSafe(rig.lines));
        QCOMPARE(rig.flasher.report().totals().done, 6);
        for (int k = 0; k < kBench; ++k) {
            if (k != 1) {
                QCOMPARE(rig.bus.resetsTo(serialOf(k)).size(), qsizetype(1));
            }
        }
    }
    {
        FleetRig rig(caseDir() + QStringLiteral("-boot"), {QStringLiteral("boot"), QStringLiteral("rootfs")});
        rig.bus.setWriteFailure(serialOf(1), QStringLiteral("boot.img"), "Error:...->Fail to write lba,ret=-1\r\n", 0);
        QVERIFY(rig.run());
        const FleetBoard b = rig.boardOf(serialOf(1));
        QCOMPARE(b.result, FleetBoard::Result::Failed);   // the text counts: exit 0 with a failure line
        QCOMPARE(b.retry.size(), qsizetype(3));
        QVERIFY(b.retry.at(0).startsWith(QStringLiteral("upgrade_tool -s %1 WL 1920 6144 ").arg(b.locationId)));
        QVERIFY(b.retry.at(1).startsWith(QStringLiteral("upgrade_tool -s %1 WL 8064 10240 ").arg(b.locationId)));
        QCOMPARE(b.retry.at(2), QStringLiteral("upgrade_tool -s %1 RD").arg(b.locationId));
        QCOMPARE(rig.bus.writesTo(serialOf(1)).size(), qsizetype(1));
    }
}

void Tst_fleetflasher::noSuchDeviceIsAFailure()
{
    // A hangs, so its WL is in flight when C (the second wave) is checked: the gate's LD is skipped,
    // and C re-enumerated meanwhile - its WL with the old id finds no device. `strayPort`: a foreign
    // Loader device appears as C ends, so the end-of-run picture has two unknown ids.
    for (const bool strayPort : {false, true}) {
        FleetRig rig(caseDir() + (strayPort ? QStringLiteral("-stray") : QString()));
        rig.flasher.setBoards(benchBoards({0, 1, 2}));
        rig.flasher.setMaxParallel(2);
        rig.bus.hangWrites(serialOf(0));
        bool renumbered = false;
        QObject::connect(&rig.runner, &FakeToolRunner::invocationStarted, &rig.runner, [&rig, &renumbered](FakeToolInvocation* i) {
            const QString a = rig.bus.board(serialOf(0)).locationId;
            if (!renumbered && !a.isEmpty() && i->commandLine().startsWith(QStringLiteral("upgrade_tool -s %1 WL ").arg(a))) {
                renumbered = true;
                rig.bus.reenumerate(serialOf(2));
            }
        });
        FleetBoard cAtItsEnd;
        QObject::connect(&rig.flasher, &FleetFlasher::boardFinished, &rig.flasher, [&rig, &cAtItsEnd, strayPort](const FleetBoard& board) {
            if (board.serial == serialOf(2)) {
                cAtItsEnd = board;
                if (strayPort) {
                    rig.bus.addForeignLoader(QStringLiteral("9&FFFF0000&0&1"));
                }
                rig.bus.releaseWrites();
            }
        });
        QVERIFY(rig.run());
        QVERIFY(renumbered);
        // At its end the id it was written with is gone: no addressed line can be given yet.
        const QString gone = cAtItsEnd.locationId;
        QCOMPARE(cAtItsEnd.result, FleetBoard::Result::Failed);
        QCOMPARE(cAtItsEnd.detail, QStringLiteral("writing rootfs failed: No found specific rockusb device! (LocationID %1 is gone)").arg(gone));
        QVERIFY(cAtItsEnd.retry.isEmpty());
        QVERIFY(cAtItsEnd.leftInLoader);
        QVERIFY(rig.lineIndex(cAtItsEnd.label() + QStringLiteral(" port %1 shows PID_110E: find its LocationID with `upgrade_tool LD` before any "
                                                                 "retry, and address it with -s")
                                                      .arg(portOf(2)))
                >= 0);
        QVERIFY(rig.anyLineContains(QStringLiteral("checked: port %1 shows PID_110E (LD not run: ").arg(portOf(2))));
        QCOMPARE(rig.boardOf(serialOf(0)).result, FleetBoard::Result::Done);
        QCOMPARE(rig.boardOf(serialOf(1)).result, FleetBoard::Result::Done);
        QCOMPARE(rig.bus.cancelledWrites(), 0);
        QVERIFY(serialHintsAreSafe(rig.lines));
        const FleetBoard c = rig.boardOf(serialOf(2));
        const QString fresh = rig.bus.board(serialOf(2)).locationId;
        QVERIFY(!fresh.isEmpty() && fresh != gone);
        if (strayPort) {
            // Two ids nobody owns and an unknown port: never paired by guessing.
            QCOMPARE(c.locationId, gone);
            QVERIFY(c.retry.isEmpty());
            QCOMPARE(c.detail, cAtItsEnd.detail);
            QCOMPARE(rig.flasher.unattributedDevices().size(), qsizetype(2));
            QVERIFY2(rig.message().endsWith(QStringLiteral("; 2 Loader devices left unattributed")), qPrintable(rig.message()));
            QVERIFY(!rig.anyLineContains(QStringLiteral(" is gone; port ")));
            continue;
        }
        // The end-of-run picture: one board re-enumerated, one id nobody owns - one device, so the
        // board takes the id and its retry lines are addressed (finding 5).
        QCOMPARE(c.locationId, fresh);
        QVERIFY(c.leftInLoader);
        QCOMPARE(c.detail, cAtItsEnd.detail + QStringLiteral("; it is in Loader mode now (port %1, LocationID %2)").arg(portOf(2), fresh));
        const PartitionImage row = rig.flasher.partitions().first();
        QCOMPARE(c.retry, (QStringList{QStringLiteral("upgrade_tool -s %1 WL 8064 10240 %2").arg(fresh, QDir::toNativeSeparators(row.file)),
                                       QStringLiteral("upgrade_tool -s %1 RD").arg(fresh)}));
        QVERIFY2(rig.lineIndex(c.label() + QStringLiteral(" LocationID %1 is gone; port %2 shows its Loader device with LocationID %3, the only new "
                                                          "one on the bus: it is this board's")
                                               .arg(gone, portOf(2), fresh))
                     >= 0,
                 qPrintable(rig.dump()));
        QVERIFY(rig.lineIndex(c.label() + QStringLiteral("   retry: ") + c.retry.first()) >= 0);
        QVERIFY(rig.lineIndex(c.label() + QStringLiteral("   retry: ") + c.retry.last()) >= 0);
        QVERIFY(rig.flasher.unattributedDevices().isEmpty());   // the new id is on C's port: C's
        QCOMPARE(FleetReport::boardJson(c).value(QStringLiteral("loader")).toString(), fresh);
    }
}

void Tst_fleetflasher::resetFailureIsFailed()
{
    FleetRig rig(caseDir());
    rig.bus.setResetFailure(serialOf(1), "Reset Device Fail!\r\n", 1);
    QVERIFY(rig.run());
    const FleetBoard b = rig.boardOf(serialOf(1));
    QCOMPARE(b.result, FleetBoard::Result::Failed);
    QVERIFY2(b.detail.startsWith(QStringLiteral("partitions written, but upgrade_tool RD did not reset the board (Reset Device Fail!); replug it")),
             qPrintable(b.detail));
    QCOMPARE(b.written, QStringList{QStringLiteral("rootfs")});
    // The end-of-run picture finds it in Loader mode: recorded, with the addressed RD.
    QVERIFY(b.leftInLoader);
    QCOMPARE(b.retry, QStringList{QStringLiteral("upgrade_tool -s %1 RD").arg(b.locationId)});
    QVERIFY(b.detail.endsWith(QStringLiteral("; it is in Loader mode now (port %1, LocationID %2)").arg(portOf(1), b.locationId)));
}

void Tst_fleetflasher::leftLoaderBeforeResetIsNotAWriteFailure()
{
    const auto rigFor = [this](const QString& suffix) {
        auto rig = std::make_unique<FleetRig>(caseDir() + suffix);
        rig->flasher.setBoards(benchBoards({0, 1}));
        rig->flasher.setMaxParallel(1);
        rig->bus.setWriteDurationMs(600);   // longer than the gate's 500 ms: B's pre-RD check runs an LD
        rig->bus.leaveLoaderAfterRows(serialOf(1), 1);
        return rig;
    };
    {
        auto rig = rigFor(QStringLiteral("-back"));
        QVERIFY(rig->run());
        const FleetBoard b = rig->boardOf(serialOf(1));
        QCOMPARE(b.result, FleetBoard::Result::Done);
        QCOMPARE(b.detail, QStringLiteral("partitions written; the board left Loader mode before its reset (port %1 showed an ADB device; LD does "
                                          "not list LocationID %2) and came back on ADB by itself")
                               .arg(portOf(1), b.locationId));
        QCOMPARE(b.written, QStringList{QStringLiteral("rootfs")});
        QVERIFY(rig->bus.resetsTo(serialOf(1)).isEmpty());
        QVERIFY2(rig->ok(), qPrintable(rig->dump()));
        QVERIFY(!rig->anyLineContains(QStringLiteral("left Loader mode before rootfs was written")));
    }
    {
        auto rig = rigFor(QStringLiteral("-gone"));
        rig->bus.setNoAdbReturn(serialOf(1));
        rig->flasher.setAdbReturnTimeoutSeconds(1);
        QVERIFY(rig->run());
        const FleetBoard b = rig->boardOf(serialOf(1));
        QCOMPARE(b.result, FleetBoard::Result::NotBack);
        QCOMPARE(b.detail, QStringLiteral("partitions written, but the board left Loader mode before its reset (port %1 shows nothing; LD does not "
                                          "list LocationID %2): not reset by the fleet; check it by hand")
                               .arg(portOf(1), b.locationId));
        QVERIFY(!b.adbBack);
    }
    {
        auto rig = rigFor(QStringLiteral("-nowait"));
        rig->flasher.setWaitForAdb(false);
        QVERIFY(rig->run());
        const FleetBoard b = rig->boardOf(serialOf(1));
        QCOMPARE(b.result, FleetBoard::Result::NotBack);
        QCOMPARE(b.detail, QStringLiteral("partitions written, but the board left Loader mode before its reset (port %1 shows an ADB device; LD "
                                          "does not list LocationID %2): not reset by the fleet; check it by hand")
                               .arg(portOf(1), b.locationId));
        QCOMPARE(rig->count(QRegularExpression(QStringLiteral("get-state"))), 0);
    }
}

void Tst_fleetflasher::adbNotBackIsNotBack()
{
    FleetRig rig(caseDir());
    rig.bus.setNoAdbReturn(serialOf(1));
    rig.flasher.setAdbReturnTimeoutSeconds(1);
    QVERIFY(rig.run());
    const FleetBoard b = rig.boardOf(serialOf(1));
    QCOMPARE(b.result, FleetBoard::Result::NotBack);
    QCOMPARE(b.detail, QStringLiteral("partitions written and the board reset, but ADB did not come back within 1 s; check the serial console"));
    QVERIFY(!b.adbBack);
    QCOMPARE(b.written, QStringList{QStringLiteral("rootfs")});
    QVERIFY(!rig.ok());
    QVERIFY2(rig.message().endsWith(QStringLiteral("; 1 not back on ADB")), qPrintable(rig.message()));
    QCOMPARE(rig.flasher.report().exitCode(), 1);
    QCOMPARE(rig.flasher.report().totals().done, 6);
}

void Tst_fleetflasher::noWaitEndsAfterTheReset()
{
    FleetRig rig(caseDir());
    rig.flasher.setWaitForAdb(false);
    QVERIFY(rig.run());
    QVERIFY2(rig.ok(), qPrintable(rig.dump()));
    for (const FleetBoard& b : rig.flasher.boards()) {
        QCOMPARE(b.result, FleetBoard::Result::Done);
        QCOMPARE(b.detail, QStringLiteral("ADB not waited for (--no-wait)"));
        QVERIFY(!b.adbBack);
    }
    QCOMPARE(rig.count(QRegularExpression(QStringLiteral("get-state"))), 0);
    QCOMPARE(rig.flasher.report().exitCode(), 0);
}

void Tst_fleetflasher::portMemoryMovedNote()
{
    {
        FleetRig rig(caseDir());
        rig.flasher.setPortProvider([](const QString&) { return QStringLiteral("9&0&0&9"); });
        QSignalSpy learned(&rig.flasher, &FleetFlasher::portLearned);
        QVERIFY(rig.run());
        QVERIFY2(rig.ok(), qPrintable(rig.dump()));
        for (int k = 0; k < kBench; ++k) {
            const FleetBoard b = rig.boardOf(serialOf(k));
            QCOMPARE(b.expectedPort, QStringLiteral("9&0&0&9"));
            const int line = rig.lineStarting(b.label() + QStringLiteral(" in Loader mode on port %1, LocationID %2 (").arg(portOf(k), b.locationId));
            QVERIFY(line >= 0);
            QVERIFY2(rig.lines.at(line).endsWith(QStringLiteral(" (moved: expected 9&0&0&9)")), qPrintable(rig.lines.at(line)));
            QVERIFY(rig.lineIndex(b.label() + QStringLiteral(" on ADB; expected port 9&0&0&9 (remembered)")) >= 0);
            QCOMPARE(learned.at(k).at(1).toString(), portOf(k));
        }
    }
    {
        FleetRig rig(caseDir() + QStringLiteral("-same"));
        rig.flasher.setPortProvider([](const QString& serial) {
            for (const FleetBench& b : kFleetBench) {
                if (serial == QLatin1String(b.serial)) {
                    return QString::fromLatin1(b.port);
                }
            }
            return QString();
        });
        QVERIFY(rig.run());
        QVERIFY(!rig.anyLineContains(QStringLiteral("(moved")));
    }
    {
        FleetRig rig(caseDir() + QStringLiteral("-none"));
        QVERIFY(rig.run());
        QVERIFY(rig.lineIndex(QStringLiteral("[A %1] on ADB; expected port unknown (first run)").arg(serialOf(0))) >= 0);
    }
}

void Tst_fleetflasher::nullEnumeratorIsRefused()
{
    FleetRig rig(caseDir());
    rig.flasher.setUsbEnumerator(nullptr);
    QCOMPARE(rig.flasher.platformRefusal(), QStringLiteral("port mapping not available on this platform"));
    QVERIFY(rig.run());
    QVERIFY(!rig.ok());
    QCOMPARE(rig.message(), QStringLiteral("port mapping not available on this platform"));
    QVERIFY(rig.runner.invocations().isEmpty());
    QCOMPARE(rig.flasher.report().exitCode(), 1);
}

void Tst_fleetflasher::cancelDuringMappingFinishesTheBoardInFlight()
{
    FleetRig rig(caseDir());
    QObject::connect(&rig.runner, &FakeToolRunner::invocationStarted, &rig.runner, [&rig](FakeToolInvocation* i) {
        if (i->commandLine() == QStringLiteral("adb -s %1 reboot loader").arg(serialOf(1))) {
            rig.flasher.cancel();   // right as the second board is told `reboot loader`
        }
    });
    QVERIFY(rig.run());
    QVERIFY(!rig.ok());
    QCOMPARE(rig.flasher.phase(), FleetFlasher::Phase::Cancelled);
    QVERIFY(rig.lineStarting(QStringLiteral("[B %1] in Loader mode on port %2").arg(serialOf(1), portOf(1))) >= 0);
    QCOMPARE(rig.count(QRegularExpression(QStringLiteral("reboot loader"))), 2);
    for (const int k : {0, 1}) {
        const FleetBoard b = rig.boardOf(serialOf(k));
        QCOMPARE(rig.bus.resetsTo(serialOf(k)), QStringList{QStringLiteral("upgrade_tool -s %1 RD").arg(b.locationId)});
        QCOMPARE(b.result, FleetBoard::Result::Skipped);
        QCOMPARE(b.detail, QStringLiteral("cancelled before its write started; reset back to ADB"));
    }
    for (int k = 2; k < kBench; ++k) {
        QCOMPARE(rig.boardOf(serialOf(k)).detail, QStringLiteral("cancelled before its reboot; not touched"));
    }
    QCOMPARE(rig.count(QRegularExpression(QStringLiteral(" WL "))), 0);
    QCOMPARE(rig.bus.cancelledWrites(), 0);
    QCOMPARE(rig.flasher.report().resetCount(), 2);
}

void Tst_fleetflasher::cancelDuringWritesDrains()
{
    FleetRig rig(caseDir());
    rig.flasher.setMaxParallel(3);
    for (const int k : {0, 1, 2}) {
        rig.bus.hangWrites(serialOf(k));
    }
    bool cancelled = false;
    bool released = false;
    QTimer driver;
    QObject::connect(&driver, &QTimer::timeout, &driver, [&]() {
        if (!cancelled && rig.bus.writesTo(serialOf(0)).size() == 1 && rig.bus.writesTo(serialOf(1)).size() == 1
            && rig.bus.writesTo(serialOf(2)).size() == 1) {
            cancelled = true;
            QVERIFY(rig.flasher.isRunning());
            rig.flasher.cancel();
            QVERIFY(rig.flasher.isDraining());
            rig.flasher.cancel();   // a second one only says what it waits for
        } else if (cancelled && !released) {
            int skipped = 0;
            for (const FleetBoard& b : rig.flasher.boards()) {
                skipped += b.result == FleetBoard::Result::Skipped ? 1 : 0;
            }
            if (skipped == 4) {
                released = true;
                rig.bus.releaseWrites();
            }
        }
    });
    driver.start(5);
    QVERIFY(rig.run());
    QVERIFY(released);
    QVERIFY(rig.lineIndex(QStringLiteral("still waiting for 3 write(s) to finish; the writes are never interrupted")) >= 0);
    for (int k = 0; k < kBench; ++k) {
        const FleetBoard b = rig.boardOf(serialOf(k));
        if (k < 3) {
            QVERIFY2(b.result == FleetBoard::Result::Done, qPrintable(b.detail));
            QCOMPARE(rig.bus.resetsTo(serialOf(k)).size(), qsizetype(1));
        } else {
            QCOMPARE(b.result, FleetBoard::Result::Skipped);
            QCOMPARE(b.detail, QStringLiteral("cancelled before its write started; reset back to ADB"));
            QVERIFY(rig.bus.writesTo(serialOf(k)).isEmpty());
            QCOMPARE(rig.bus.resetsTo(serialOf(k)).size(), qsizetype(1));
        }
    }
    QVERIFY(!rig.ok());
    QVERIFY2(rig.message().startsWith(QStringLiteral("3 of 7 boards flashed in ")) && rig.message().endsWith(QStringLiteral("; 4 skipped")),
             qPrintable(rig.message()));
    QCOMPARE(rig.bus.cancelledWrites(), 0);
    QCOMPARE(rig.bus.ldDuringWrites(), 0);
}

void Tst_fleetflasher::drainRdWaitsForTheGateLd()
{
    FleetRig rig(caseDir());
    rig.flasher.setMaxParallel(1);
    rig.bus.setLdDurationMs(80);
    rig.bus.setWriteDurationMs(600);
    bool written = false;
    bool fired = false;
    QObject::connect(&rig.flasher, &FleetFlasher::partitionWritten, &rig.flasher, [&written]() { written = true; });
    QObject::connect(&rig.runner, &FakeToolRunner::invocationStarted, &rig.runner, [&](FakeToolInvocation* i) {
        if (written && !fired && i->commandLine() == QStringLiteral("upgrade_tool LD")) {
            fired = true;   // the first LD after a WL ended: A's pre-RD gate listing
            rig.flasher.cancel();
        }
    });
    QVERIFY(rig.run(20000));
    QVERIFY(fired);
    QCOMPARE(rig.bus.ldDuringWrites(), 0);   // no RD started next to that LD (nor any LD next to an RD)
    QCOMPARE(rig.boardOf(serialOf(0)).result, FleetBoard::Result::Done);
    for (int k = 1; k < kBench; ++k) {
        QCOMPARE(rig.boardOf(serialOf(k)).result, FleetBoard::Result::Skipped);
        QCOMPARE(rig.boardOf(serialOf(k)).detail, QStringLiteral("cancelled before its write started; reset back to ADB"));
        QCOMPARE(rig.bus.resetsTo(serialOf(k)).size(), qsizetype(1));
    }
    QCOMPARE(rig.bus.cancelledWrites(), 0);
}

void Tst_fleetflasher::cancelDuringAdbWaitEndsIt()
{
    FleetRig rig(caseDir());
    rig.bus.setNoAdbReturn(serialOf(1));
    rig.flasher.setAdbReturnTimeoutSeconds(30);
    int done = 0;
    QObject::connect(&rig.flasher, &FleetFlasher::boardFinished, &rig.flasher, [&rig, &done](const FleetBoard& board) {
        if (board.result == FleetBoard::Result::Done && ++done == kBench - 1) {
            QTimer::singleShot(20, &rig.flasher, [&rig]() { rig.flasher.cancel(); });
        }
    });
    QVERIFY(rig.run());
    const FleetBoard b = rig.boardOf(serialOf(1));
    QCOMPARE(b.result, FleetBoard::Result::NotBack);
    QCOMPARE(b.detail, QStringLiteral("the ADB wait was cancelled; the partitions were written"));
    QVERIFY(!rig.ok());
    QCOMPARE(rig.flasher.phase(), FleetFlasher::Phase::Cancelled);
    QVERIFY(rig.flasher.elapsedSeconds() < 20.0);
}

void Tst_fleetflasher::dryRunTouchesNoBoard()
{
    FleetRig rig(caseDir(), {QStringLiteral("rootfs")}, false);
    rig.addBench({2});
    rig.flasher.setDryRun(true);
    rig.flasher.setPortProvider([](const QString& serial) { return serial == serialOf(0) ? portOf(0) : QString(); });
    QVERIFY(rig.run());
    QVERIFY(!rig.ok());
    QCOMPARE(rig.message(), QStringLiteral("dry run: 6 of 7 boards ready; 1 skipped"));
    QCOMPARE(rig.lines.first(), QStringLiteral("Dry run: the selection, the remembered ports and the plan; no reboot, no write"));
    for (const QString& command : rig.runner.invocations()) {
        QVERIFY2(command == QStringLiteral("adb devices") || command == QStringLiteral("upgrade_tool LD"), qPrintable(command));
    }
    for (int k = 0; k < kBench; ++k) {
        const FleetBoard b = rig.boardOf(serialOf(k));
        QCOMPARE(b.result, k == 2 ? FleetBoard::Result::Skipped : FleetBoard::Result::Ready);
    }
    QCOMPARE(rig.boardOf(serialOf(0)).expectedPort, portOf(0));
    QVERIFY(rig.lineIndex(QStringLiteral("Plan: write rootfs (3830272 bytes, sector 8064, 10240 sectors) on 6 boards, up to 6 at once")) >= 0);
    QCOMPARE(rig.flasher.report().exitCode(), 1);
    QVERIFY(rig.flasher.report().dryRun());
}

void Tst_fleetflasher::allBoardsInAdbOrder()
{
    FleetRig rig(caseDir());
    rig.flasher.setBoards({});
    rig.flasher.setAllBoards(true);
    QVERIFY(rig.flasher.allBoards());
    QVERIFY(rig.run());
    QVERIFY2(rig.ok(), qPrintable(rig.dump()));
    const QList<FleetBoard> boards = rig.flasher.boards();
    QCOMPARE(boards.size(), qsizetype(kBench));
    for (int k = 0; k < kBench; ++k) {
        QCOMPARE(boards.at(k).tag, FleetFlasher::tagFor(k));
        QCOMPARE(boards.at(k).serial, serialOf(k));
    }
    QCOMPARE(rig.count(addressedWl()), kBench);
}

void Tst_fleetflasher::allBoardsRefusesAForeignAdbDevice()
{
    const QString refusal = QStringLiteral("adb lists 8 devices but the USB bus shows 7 Rockchip ADB gadgets: a non-Rockchip ADB device is attached; "
                                           "name the boards with --serials");
    for (const bool dry : {false, true}) {
        FleetRig rig(caseDir() + (dry ? QStringLiteral("-dry") : QString()));
        rig.bus.addForeignAdb(QStringLiteral("ZY22PHONE"));
        rig.flasher.setAllBoards(true);
        rig.flasher.setDryRun(dry);
        QVERIFY(rig.run());
        QCOMPARE(rig.message(), refusal);
        QCOMPARE(rig.count(QRegularExpression(QStringLiteral("reboot loader"))), 0);
        QCOMPARE(rig.count(QRegularExpression(QStringLiteral("^adb -s "))), 0);
        QCOMPARE(rig.flasher.report().exitCode(), 1);
    }
    FleetRig rig(caseDir() + QStringLiteral("-named"));
    rig.bus.addForeignAdb(QStringLiteral("ZY22PHONE"));
    QVERIFY(rig.run());   // --serials: the phone is nobody's business
    QVERIFY2(rig.ok(), qPrintable(rig.dump()));
    QCOMPARE(rig.count(QRegularExpression(QStringLiteral("ZY22PHONE"))), 0);
}

void Tst_fleetflasher::rowsCheckedBeforeAnyReboot()
{
    FleetRig rig(caseDir());
    const PartitionImage row = rig.flasher.partitions().first();
    QVERIFY(QFile::remove(row.file));
    QVERIFY(rig.run());
    QCOMPARE(rig.message(), QStringLiteral("the image of partition rootfs is missing: %1").arg(QDir::toNativeSeparators(row.file)));
    QCOMPARE(rig.count(QRegularExpression(QStringLiteral("reboot loader"))), 0);
    QCOMPARE(rig.flasher.report().exitCode(), 1);
}

void Tst_fleetflasher::adbCannotRunSelectsNobody()
{
    {
        FleetRig rig(caseDir(), {QStringLiteral("rootfs")}, false);
        rig.runner.addHandler(QRegularExpression(QStringLiteral("^adb devices$")), [](FakeToolInvocation* i) { i->failToStart(QStringLiteral("fake: no adb")); });
        scriptFleet(rig.bus, rig.runner);
        QVERIFY(rig.run());
        QCOMPARE(rig.message(), QStringLiteral("adb could not be run: fake: no adb"));
        QVERIFY(rig.flasher.boards().isEmpty());
        QCOMPARE(rig.count(QRegularExpression(QStringLiteral("reboot loader"))), 0);
        QCOMPARE(rig.flasher.report().exitCode(), 1);   // not 3: nobody was looked at
    }
    {
        FleetRig rig(caseDir() + QStringLiteral("-hung"), {QStringLiteral("rootfs")}, false);
        rig.flasher.adb()->setTimeoutMs(50);
        rig.runner.addHandler(QRegularExpression(QStringLiteral("^adb devices$")), [](FakeToolInvocation* i) { i->timeOut(50); });
        scriptFleet(rig.bus, rig.runner);
        QVERIFY(rig.run());
        QCOMPARE(rig.message(), QStringLiteral("adb devices timed out"));
        QCOMPARE(rig.flasher.report().exitCode(), 1);
    }
}

void Tst_fleetflasher::baselineLdCannotRunRefuses()
{
    {
        FleetRig rig(caseDir(), {QStringLiteral("rootfs")}, false);
        rig.runner.addHandler(QRegularExpression(QStringLiteral("^upgrade_tool LD$")),
                              [](FakeToolInvocation* i) { i->failToStart(QStringLiteral("fake: no upgrade_tool")); });
        scriptFleet(rig.bus, rig.runner);
        QVERIFY(rig.run());
        QCOMPARE(rig.message(), QStringLiteral("upgrade_tool could not be run: fake: no upgrade_tool"));
        QCOMPARE(rig.count(QRegularExpression(QStringLiteral("reboot loader"))), 0);
        QCOMPARE(rig.flasher.report().exitCode(), 1);
    }
    {
        FleetRig rig(caseDir() + QStringLiteral("-hung"), {QStringLiteral("rootfs")}, false);
        rig.runner.addHandler(QRegularExpression(QStringLiteral("^upgrade_tool LD$")), [](FakeToolInvocation* i) { i->timeOut(20); });
        scriptFleet(rig.bus, rig.runner);
        QVERIFY(rig.run());
        QCOMPARE(rig.message(), QStringLiteral("upgrade_tool LD timed out"));
        QCOMPARE(rig.count(QRegularExpression(QStringLiteral("reboot loader"))), 0);
    }
}

void Tst_fleetflasher::silenceLineIsPerBoard()
{
    qputenv("SU_FLEET_SILENCE_MS", "60");   // read by the constructor
    FleetRig rig(caseDir());
    qunsetenv("SU_FLEET_SILENCE_MS");
    rig.flasher.setBoards(benchBoards({0, 1}));
    rig.bus.hangWrites(serialOf(0));
    const QString silence = QStringLiteral("[A %1] upgrade_tool has printed nothing for 0 min; it is left running, because stopping it half way "
                                           "needs MASKROM recovery (if it hangs for good, end the upgrade_tool process by hand)")
                                .arg(serialOf(0));
    bool sawIt = false;
    bool stillRunning = false;
    QTimer driver;
    QObject::connect(&driver, &QTimer::timeout, &driver, [&]() {
        if (!sawIt && rig.lines.contains(silence)) {
            sawIt = true;
            for (FakeToolInvocation* live : rig.runner.liveInvocations()) {
                stillRunning = stillRunning || live->commandLine().contains(QStringLiteral(" WL "));
            }
            rig.bus.releaseWrites();
        }
    });
    driver.start(10);
    QVERIFY(rig.run());
    QVERIFY(sawIt);
    QVERIFY(stillRunning);   // reported, never stopped
    QCOMPARE(rig.boardOf(serialOf(0)).result, FleetBoard::Result::Done);
    QCOMPARE(rig.bus.cancelledWrites(), 0);
}

// =============================================================================================
// Review 3: the findings and the test gaps
// =============================================================================================

void Tst_fleetflasher::maskromAtAMidWaveCheckIsNoLoader()
{
    // A hangs, so its WL is in flight when C (the second wave) is checked by the enumeration alone;
    // C drops to MASKROM meanwhile (a brown-out: pid 0x110c on its port, LD says Maskrom).
    FleetRig rig(caseDir());
    rig.flasher.setBoards(benchBoards({0, 1, 2}));
    rig.flasher.setMaxParallel(2);
    rig.bus.hangWrites(serialOf(0));
    bool dropped = false;
    QObject::connect(&rig.runner, &FakeToolRunner::invocationStarted, &rig.runner, [&rig, &dropped](FakeToolInvocation* i) {
        const QString a = rig.bus.board(serialOf(0)).locationId;
        if (!dropped && !a.isEmpty() && i->commandLine().startsWith(QStringLiteral("upgrade_tool -s %1 WL ").arg(a))) {
            dropped = true;
            rig.bus.moveToMaskrom(serialOf(2));
        }
    });
    QObject::connect(&rig.flasher, &FleetFlasher::boardFinished, &rig.flasher, [&rig](const FleetBoard& board) {
        if (board.serial == serialOf(2)) {
            rig.bus.releaseWrites();
        }
    });
    QVERIFY(rig.run());
    QVERIFY(dropped);
    const FleetBoard c = rig.boardOf(serialOf(2));
    QCOMPARE(c.result, FleetBoard::Result::Failed);
    const QRegularExpression evidence(QStringLiteral("^left Loader mode before rootfs was written \\(port %1 shows PID_110C, not its Loader-mode "
                                                     "PID_110E; LD not run: \\d+ writes in flight\\): nothing written for it$")
                                          .arg(QRegularExpression::escape(portOf(2))));
    QVERIFY2(evidence.match(c.detail).hasMatch(), qPrintable(c.detail));   // and the end-of-run picture does not call it Loader mode
    QCOMPARE(rig.count(QRegularExpression(QStringLiteral("^upgrade_tool -s %1 WL ").arg(c.locationId))), 0);   // no WL to a MASKROM device
    QVERIFY(rig.bus.resetsTo(serialOf(2)).isEmpty());
    QVERIFY(!c.leftInLoader);
    QVERIFY(c.retry.isEmpty());   // no addressed WL line that cannot work on it
    QVERIFY(!rig.anyLineContains(QStringLiteral("checked: port %1 shows PID_110C").arg(portOf(2))));
    QCOMPARE(rig.boardOf(serialOf(0)).result, FleetBoard::Result::Done);
    QCOMPARE(rig.boardOf(serialOf(1)).result, FleetBoard::Result::Done);
    QVERIFY(rig.flasher.unattributedDevices().isEmpty());   // its port and its id are C's
    QVERIFY(serialHintsAreSafe(rig.lines));
    QCOMPARE(rig.bus.unaddressedCommands(), 0);
}

void Tst_fleetflasher::skippedButStillInLoaderIsCorrected()
{
    // A stop's RD to A answers "Reset Device OK." and does not take: the end-of-run picture finds
    // A in Loader mode, so it is neither "reset back to ADB" nor counted as such.
    {
        FleetRig rig(caseDir());
        rig.bus.setReboot(serialOf(2), FakeUsbBus::Reboot::DragsNext);
        rig.bus.setResetFailure(serialOf(0), "Reset Device OK.\r\n", 0);
        QVERIFY(rig.run());
        const FleetBoard a = rig.boardOf(serialOf(0));
        const QString n = a.locationId;
        QCOMPARE(a.result, FleetBoard::Result::Skipped);
        QVERIFY(a.leftInLoader);
        QVERIFY2(a.detail.startsWith(QStringLiteral("mapping stopped: while rebooting %1: two boards changed at once").arg(serialOf(2))), qPrintable(a.detail));
        QVERIFY2(a.detail.endsWith(QStringLiteral("; it is still in Loader mode (port %1, LocationID %2): reset it by hand with `upgrade_tool -s %2 RD`")
                                       .arg(portOf(0), n)),
                 qPrintable(a.detail));
        QCOMPARE(a.retry, QStringList{QStringLiteral("upgrade_tool -s %1 RD").arg(n)});
        QVERIFY(rig.lineIndex(a.label() + QStringLiteral(" still in Loader mode on port %1, LocationID %2: reset it by hand with `upgrade_tool -s %2 "
                                                         "RD` (the line follows)")
                                              .arg(portOf(0), n))
                >= 0);
        QVERIFY(rig.lineIndex(a.label() + QStringLiteral("   retry: upgrade_tool -s %1 RD").arg(n)) >= 0);
        QVERIFY2(rig.message().endsWith(QStringLiteral("; nothing was written; 1 board reset back to ADB; 2 Loader devices left unattributed")),
                 qPrintable(rig.message()));
        QCOMPARE(rig.flasher.report().resetCount(), 1);
        const FleetBoard b = rig.boardOf(serialOf(1));   // B really went back
        QVERIFY(!b.leftInLoader && b.retry.isEmpty());
        QVERIFY(b.detail.startsWith(QStringLiteral("mapping stopped: ")) && !b.detail.contains(QStringLiteral("Loader mode (port")));
        for (const FleetFlasher::UnattributedDevice& device : rig.flasher.unattributedDevices()) {
            QVERIFY(device.locationId != n);   // A's port and id are A's
        }
        QCOMPARE(rig.count(QRegularExpression(QStringLiteral(" WL "))), 0);
    }
    {
        // The same after a cancel during the mapping.
        FleetRig rig(caseDir() + QStringLiteral("-cancel"));
        rig.bus.setResetFailure(serialOf(0), "Reset Device OK.\r\n", 0);
        QObject::connect(&rig.runner, &FakeToolRunner::invocationStarted, &rig.runner, [&rig](FakeToolInvocation* i) {
            if (i->commandLine() == QStringLiteral("adb -s %1 reboot loader").arg(serialOf(1))) {
                rig.flasher.cancel();
            }
        });
        QVERIFY(rig.run());
        const FleetBoard a = rig.boardOf(serialOf(0));
        QCOMPARE(a.detail, QStringLiteral("cancelled before its write started; reset back to ADB; it is still in Loader mode (port %1, LocationID %2): "
                                          "reset it by hand with `upgrade_tool -s %2 RD`")
                               .arg(portOf(0), a.locationId));
        QVERIFY(a.leftInLoader);
        QCOMPARE(a.retry, QStringList{QStringLiteral("upgrade_tool -s %1 RD").arg(a.locationId)});
        const FleetBoard b = rig.boardOf(serialOf(1));
        QCOMPARE(b.detail, QStringLiteral("cancelled before its write started; reset back to ADB"));
        QVERIFY(!b.leftInLoader);
        QCOMPARE(rig.flasher.report().resetCount(), 1);
        QVERIFY(rig.flasher.unattributedDevices().isEmpty());
    }
}

void Tst_fleetflasher::resetFindsNoDevice()
{
    // A cancel's RD meets a gone id (A re-enumerated once B was mapped): exit 255, nothing reset.
    {
        FleetRig rig(caseDir());
        QObject::connect(&rig.runner, &FakeToolRunner::invocationStarted, &rig.runner, [&rig](FakeToolInvocation* i) {
            if (i->commandLine() == QStringLiteral("adb -s %1 reboot loader").arg(serialOf(1))) {
                rig.flasher.cancel();
            }
        });
        QObject::connect(&rig.flasher, &FleetFlasher::boardEvent, &rig.flasher, [&rig](const FleetBoard& board, const QString& text) {
            if (board.serial == serialOf(1) && text.startsWith(QStringLiteral("in Loader mode on port "))) {
                rig.bus.reenumerate(serialOf(0));   // before the cancel's RDs start
            }
        });
        FleetBoard aAtItsEnd;
        QObject::connect(&rig.flasher, &FleetFlasher::boardFinished, &rig.flasher, [&aAtItsEnd](const FleetBoard& board) {
            if (board.serial == serialOf(0)) {
                aAtItsEnd = board;
            }
        });
        QVERIFY(rig.run());
        const QString gone = aAtItsEnd.locationId;
        QCOMPARE(rig.bus.resetsTo(serialOf(0)), QStringList());   // the RD named no device the bus knows
        QCOMPARE(rig.count(QRegularExpression(QStringLiteral("^upgrade_tool -s %1 RD$").arg(gone))), 1);
        QCOMPARE(aAtItsEnd.result, FleetBoard::Result::Skipped);
        QCOMPARE(aAtItsEnd.detail, QStringLiteral("cancelled before its write started; upgrade_tool RD found no device with LocationID %1 (it is "
                                                  "gone): find its LocationID with `upgrade_tool LD` before any retry, and address it with -s")
                                       .arg(gone));
        QVERIFY(aAtItsEnd.retry.isEmpty());   // never `-s <gone id> RD` as the way out
        QVERIFY(aAtItsEnd.leftInLoader);      // its port still shows its Loader mode
        QVERIFY(rig.lineIndex(aAtItsEnd.label() + QStringLiteral(" port %1 shows PID_110E").arg(portOf(0))) >= 0);
        QVERIFY(!rig.anyLineContains(QStringLiteral("reset it by hand with `upgrade_tool -s %1 RD`").arg(gone)));
        QCOMPARE(rig.flasher.report().resetCount(), 1);   // B only
        // The end-of-run picture pairs A with its new id: its one RD line is addressed again.
        const FleetBoard a = rig.boardOf(serialOf(0));
        const QString fresh = rig.bus.board(serialOf(0)).locationId;
        QVERIFY(!fresh.isEmpty() && fresh != gone);
        QCOMPARE(a.locationId, fresh);
        QCOMPARE(a.retry, QStringList{QStringLiteral("upgrade_tool -s %1 RD").arg(fresh)});
        QCOMPARE(a.detail, aAtItsEnd.detail + QStringLiteral("; it is still in Loader mode (port %1, LocationID %2): reset it by hand with "
                                                             "`upgrade_tool -s %2 RD`")
                                                  .arg(portOf(0), fresh));
        QVERIFY(rig.flasher.unattributedDevices().isEmpty());
        QCOMPARE(rig.count(QRegularExpression(QStringLiteral(" WL "))), 0);
    }
    {
        // A written board's RD meets a gone id: B re-enumerates right after its WL, while A's WL is
        // in flight (so B's pre-RD check is the enumeration alone and passes).
        FleetRig rig(caseDir() + QStringLiteral("-written"));
        rig.flasher.setBoards(benchBoards({0, 1}));
        rig.flasher.setMaxParallel(2);
        rig.bus.hangWrites(serialOf(0));
        QObject::connect(&rig.flasher, &FleetFlasher::partitionWritten, &rig.flasher, [&rig](const FleetBoard& board) {
            if (board.serial == serialOf(1)) {
                rig.bus.reenumerate(serialOf(1));
            }
        });
        FleetBoard bAtItsEnd;
        QObject::connect(&rig.flasher, &FleetFlasher::boardFinished, &rig.flasher, [&rig, &bAtItsEnd](const FleetBoard& board) {
            if (board.serial == serialOf(1)) {
                bAtItsEnd = board;
                rig.bus.releaseWrites();
            }
        });
        QVERIFY(rig.run());
        const QString gone = bAtItsEnd.locationId;
        QCOMPARE(rig.count(QRegularExpression(QStringLiteral("^upgrade_tool -s %1 RD$").arg(gone))), 1);
        QCOMPARE(bAtItsEnd.result, FleetBoard::Result::Failed);
        QCOMPARE(bAtItsEnd.detail, QStringLiteral("partitions written, but upgrade_tool RD found no device with LocationID %1 (it is gone): find its "
                                                  "LocationID with `upgrade_tool LD` before any retry, and address it with -s")
                                       .arg(gone));
        QVERIFY(bAtItsEnd.retry.isEmpty());
        QVERIFY(bAtItsEnd.leftInLoader);
        QCOMPARE(bAtItsEnd.written, QStringList{QStringLiteral("rootfs")});
        QVERIFY(rig.lineIndex(bAtItsEnd.label() + QStringLiteral(" port %1 shows PID_110E").arg(portOf(1))) >= 0);
        QVERIFY(!rig.anyLineContains(QStringLiteral("did not reset the board")));
        QCOMPARE(rig.boardOf(serialOf(0)).result, FleetBoard::Result::Done);
        // Paired at the end: every row is written, so the RD alone, addressed to the new id.
        const FleetBoard b = rig.boardOf(serialOf(1));
        const QString fresh = rig.bus.board(serialOf(1)).locationId;
        QCOMPARE(b.locationId, fresh);
        QCOMPARE(b.retry, QStringList{QStringLiteral("upgrade_tool -s %1 RD").arg(fresh)});
        QCOMPARE(b.detail, bAtItsEnd.detail + QStringLiteral("; it is in Loader mode now (port %1, LocationID %2)").arg(portOf(1), fresh));
        QVERIFY(rig.flasher.unattributedDevices().isEmpty());
        QCOMPARE(rig.bus.cancelledWrites(), 0);
    }
}

void Tst_fleetflasher::baselineMismatchNamesEveryPortAndId()
{
    const QString listedPort = QStringLiteral("9&FFFF0000&0&1");   // a foreign Loader device: an instance and an LD line
    const QString extraPort = QStringLiteral("9&AAAA0000&0&2");    // sorts first: blamed by position before the fix
    const QString extra = QStringLiteral("aaaa000000000001");
    for (const FakeUsbBus::Reboot behaviour : {FakeUsbBus::Reboot::NoLdLine, FakeUsbBus::Reboot::LdLineOnly}) {
        FleetRig rig(caseDir() + QString::number(int(behaviour)));
        rig.bus.addForeignLoader(listedPort);   // LocationID 100
        rig.bus.addBoard(extra, extraPort);
        rig.bus.setReboot(extra, behaviour);
        rig.runner.run(QStringLiteral("adb"), {QStringLiteral("-s"), extra, QStringLiteral("reboot"), QStringLiteral("loader")});
        QTRY_VERIFY(behaviour == FakeUsbBus::Reboot::NoLdLine ? rig.bus.board(extra).state == FakeUsbBus::State::Loader
                                                                  : rig.bus.board(extra).ldListed);
        QTest::qWait(30);   // the PnP lag
        QVERIFY(rig.run());
        if (behaviour == FakeUsbBus::Reboot::NoLdLine) {
            QList<UsbDeviceInfo> instances;
            for (const UsbDeviceInfo& d : rig.bus.enumerate(0x2207)) {
                if (d.isDownloadMode()) {
                    instances << d;
                }
            }
            QCOMPARE(instances.size(), qsizetype(2));
            QCOMPARE(rig.message(), QStringLiteral("ports %1, %2 are in download mode but upgrade_tool LD lists only LocationID 100: one of these devices "
                                                   "is not listed; %3")
                                        .arg(extraPort, listedPort, UsbProbe::driverHint(instances)));
        } else {
            const QString n = rig.bus.board(extra).locationId;
            QCOMPARE(n, QStringLiteral("101"));
            QCOMPARE(rig.message(), QStringLiteral("upgrade_tool LD lists LocationID 100, 101 in download mode but the USB enumeration shows only port "
                                                   "%1: one of these LocationIDs has no device on the bus; port mapping impossible (driver?)")
                                        .arg(listedPort));
        }
        QCOMPARE(rig.count(QRegularExpression(QStringLiteral("^adb -s \\S+ reboot loader$"))), 1);   // only the test's own
        QCOMPARE(rig.flasher.report().exitCode(), 1);
    }
}

void Tst_fleetflasher::retryLinesNameTheToolPath()
{
    // The bundled tool is not on PATH: a retry line carries the path the run used, so it runs as it is.
    FleetRig rig(caseDir());
    const QString tool = QStringLiteral("C:/Program Files/BuildAI/SerialUtility/tools/upgrade_tool.exe");
    rig.flasher.setToolPaths(tool, QStringLiteral("adb"));
    rig.flasher.setBoards(benchBoards({0, 1}));
    rig.bus.setWriteFailure(serialOf(1), QStringLiteral("rootfs.img"), "Write LBA failed, err=-3\r\n", 1);
    QVERIFY(rig.run());
    QCOMPARE(rig.boardOf(serialOf(0)).result, FleetBoard::Result::Done);
    const FleetBoard b = rig.boardOf(serialOf(1));
    const QString n = b.locationId;
    QCOMPARE(b.result, FleetBoard::Result::Failed);
    const QString program = QDir::toNativeSeparators(tool);
    const PartitionImage row = rig.flasher.partitions().first();
    const QStringList retry = {ToolRunner::formatCommandLine(program, {QStringLiteral("-s"), n, QStringLiteral("WL"), QStringLiteral("8064"),
                                                                       QStringLiteral("10240"), QDir::toNativeSeparators(row.file)}),
                               ToolRunner::formatCommandLine(program, {QStringLiteral("-s"), n, QStringLiteral("RD")})};
    QCOMPARE(b.retry, retry);
#ifdef Q_OS_WIN
    QCOMPARE(b.retry.last(), QStringLiteral("\"C:\\Program Files\\BuildAI\\SerialUtility\\tools\\upgrade_tool.exe\" -s %1 RD").arg(n));
#endif
    QVERIFY(rig.lineIndex(b.label() + QStringLiteral("   retry: ") + retry.at(0)) >= 0);
    QVERIFY(rig.lineIndex(b.label() + QStringLiteral("   retry: ") + retry.at(1)) >= 0);
    // The hint keeps the recipe's words; the runnable lines follow it.
    QVERIFY(rig.anyLineContains(QStringLiteral("then `upgrade_tool -s %1 RD` (the lines follow)").arg(n)));
    QCOMPARE(FleetReport::boardJson(b).value(QStringLiteral("retry")).toArray().first().toString(), retry.first());
    QCOMPARE(rig.bus.unaddressedCommands(), 0);
}

void Tst_fleetflasher::writeCannotStartLeavesTheBoardInLoader()
{
    FleetRig rig(caseDir());
    rig.bus.setWriteStartFailure(serialOf(1), QStringLiteral("fake: upgrade_tool vanished"));
    QVERIFY(rig.run());
    const FleetBoard b = rig.boardOf(serialOf(1));
    const QString n = b.locationId;
    QCOMPARE(b.result, FleetBoard::Result::Failed);
    QCOMPARE(b.detail, QStringLiteral("writing rootfs failed: fake: upgrade_tool vanished"));
    QVERIFY(b.leftInLoader);   // nothing reached the board: it is in Loader mode, untouched
    QVERIFY(b.written.isEmpty());
    const PartitionImage row = rig.flasher.partitions().first();
    const QStringList retry = {QStringLiteral("upgrade_tool -s %1 WL 8064 10240 %2").arg(n, QDir::toNativeSeparators(row.file)),
                               QStringLiteral("upgrade_tool -s %1 RD").arg(n)};
    QCOMPARE(b.retry, retry);
    QVERIFY(rig.lineIndex(b.label() + QStringLiteral("   could not run: fake: upgrade_tool vanished")) >= 0);
    const int failed = rig.lineIndex(b.label() + QStringLiteral(" writing rootfs failed: fake: upgrade_tool vanished"));
    QVERIFY2(failed >= 0, qPrintable(rig.dump()));
    QVERIFY(rig.lines.at(failed + 1).startsWith(b.label() + QStringLiteral(" left in Loader mode on port %1, LocationID %2: ").arg(portOf(1), n)));
    QCOMPARE(rig.lines.at(failed + 2), b.label() + QStringLiteral("   retry: ") + retry.at(0));
    QCOMPARE(rig.lines.at(failed + 3), b.label() + QStringLiteral("   retry: ") + retry.at(1));
    QVERIFY(rig.bus.resetsTo(serialOf(1)).isEmpty());   // no RD: nothing else is written to it
    QCOMPARE(rig.flasher.report().totals().done, 6);
    QVERIFY(serialHintsAreSafe(rig.lines));
    QCOMPARE(rig.bus.unaddressedCommands(), 0);
}

void Tst_fleetflasher::resetTimesOutIsAFailedReset()
{
    FleetRig rig(caseDir());
    rig.bus.setResetTimeout(serialOf(1));
    QVERIFY(rig.run());
    const FleetBoard b = rig.boardOf(serialOf(1));
    QCOMPARE(b.result, FleetBoard::Result::Failed);
    QVERIFY2(b.detail.startsWith(QStringLiteral("partitions written, but upgrade_tool RD did not reset the board (timed out); replug it")),
             qPrintable(b.detail));
    QCOMPARE(b.written, QStringList{QStringLiteral("rootfs")});
    QCOMPARE(rig.bus.resetsTo(serialOf(1)).size(), qsizetype(1));
    QVERIFY(rig.lineIndex(b.label() + QStringLiteral("   timed out")) >= 0);
    // The RD carries the LD timeout (a timed-out RD is a failed reset), the WL none.
    const QStringList commands = rig.runner.invocations();
    int rds = 0;
    for (int i = 0; i < commands.size(); ++i) {
        if (commands.at(i) == QStringLiteral("upgrade_tool -s %1 RD").arg(b.locationId)) {
            ++rds;
            QCOMPARE(rig.runner.optionsAt(i).timeoutMs, rig.flasher.upgradeTool()->timeoutMs());
            QVERIFY(rig.runner.optionsAt(i).timeoutMs > 0);
        } else if (commands.at(i).startsWith(QStringLiteral("upgrade_tool -s %1 WL ").arg(b.locationId))) {
            QCOMPARE(rig.runner.optionsAt(i).timeoutMs, 0);
        }
    }
    QCOMPARE(rds, 1);
    // Still in Loader mode at the end: recorded, with the addressed RD.
    QVERIFY(b.leftInLoader);
    QCOMPARE(b.retry, QStringList{QStringLiteral("upgrade_tool -s %1 RD").arg(b.locationId)});
    QVERIFY(b.detail.endsWith(QStringLiteral("; it is in Loader mode now (port %1, LocationID %2)").arg(portOf(1), b.locationId)));
    QCOMPARE(rig.flasher.report().totals().done, 6);
    QCOMPARE(rig.bus.cancelledWrites(), 0);
}

void Tst_fleetflasher::cmdlineFailureSkipsBeforeTheReboot()
{
    // `adb devices` listed B, but its /proc/cmdline cannot be read: skipped, never rebooted.
    FleetRig rig(caseDir(), {QStringLiteral("rootfs")}, false);
    rig.runner.addHandler(QRegularExpression(QStringLiteral("^adb -s %1 shell \"cat /proc/cmdline\"$").arg(serialOf(1))), [](FakeToolInvocation* i) {
        i->emitOutput("error: closed\r\n");
        i->finish(1);
    });
    scriptFleet(rig.bus, rig.runner);
    QSignalSpy layouts(&rig.flasher, &FleetFlasher::mtdPartsRead);
    QVERIFY(rig.run());
    const FleetBoard b = rig.boardOf(serialOf(1));
    QCOMPARE(b.result, FleetBoard::Result::Skipped);
    QCOMPARE(b.detail, QStringLiteral("adb could not read board %1: error: closed").arg(serialOf(1)));
    QVERIFY(rig.lineIndex(b.label() + QLatin1Char(' ') + b.detail + QStringLiteral("; skipped")) >= 0);
    QCOMPARE(rig.count(QRegularExpression(QStringLiteral("^adb -s %1 reboot loader$").arg(serialOf(1)))), 0);
    QVERIFY(b.layout.isEmpty() && b.port.isEmpty() && b.locationId.isEmpty());
    QCOMPARE(layouts.count(), kBench - 1);
    QCOMPARE(rig.flasher.report().totals().done, 6);
    QVERIFY(rig.flasher.stopReason().isEmpty());   // one board skipped, the run goes on
}

void Tst_fleetflasher::stopResetFailureLeavesTheBoardInLoader()
{
    FleetRig rig(caseDir());
    rig.bus.setReboot(serialOf(2), FakeUsbBus::Reboot::DragsNext);
    rig.bus.setResetFailure(serialOf(0), "Reset Device Fail!\r\n", 1);
    QVERIFY(rig.run());
    const FleetBoard a = rig.boardOf(serialOf(0));
    const QString n = a.locationId;
    QCOMPARE(a.result, FleetBoard::Result::Skipped);
    QVERIFY(a.leftInLoader);
    QCOMPARE(a.retry, QStringList{QStringLiteral("upgrade_tool -s %1 RD").arg(n)});
    QVERIFY2(a.detail.startsWith(QStringLiteral("mapping stopped: while rebooting %1: two boards changed at once").arg(serialOf(2))), qPrintable(a.detail));
    QVERIFY2(a.detail.endsWith(QStringLiteral("; upgrade_tool RD did not reset it (Reset Device Fail!): reset it by hand with `upgrade_tool -s %1 RD`")
                                   .arg(n)),
             qPrintable(a.detail));
    QVERIFY(rig.lineIndex(a.label() + QStringLiteral("   retry: upgrade_tool -s %1 RD").arg(n)) >= 0);
    QVERIFY2(rig.message().endsWith(QStringLiteral("; nothing was written; 1 board reset back to ADB; 2 Loader devices left unattributed")),
             qPrintable(rig.message()));
    QCOMPARE(rig.flasher.report().resetCount(), 1);
    for (const FleetFlasher::UnattributedDevice& device : rig.flasher.unattributedDevices()) {
        QVERIFY(device.locationId != n);   // A is a board of the run, never unattributed
    }
    QCOMPARE(rig.count(QRegularExpression(QStringLiteral(" WL "))), 0);
    QCOMPARE(rig.bus.unaddressedCommands(), 0);
}

void Tst_fleetflasher::ldListsButPortShowsAdb()
{
    // The first wave's one gate listing lists B in Loader mode, but B is back on ADB when its check
    // reads the enumeration: the port decides.
    FleetRig rig(caseDir(), {QStringLiteral("rootfs")}, false);
    bool armed = false;
    bool moved = false;
    QObject::connect(&rig.flasher, &FleetFlasher::logLine, &rig.flasher, [&armed](const QString& line) {
        if (line.startsWith(QStringLiteral("mapping done:"))) {
            armed = true;
        }
    });
    rig.runner.addHandler(QRegularExpression(QStringLiteral("^upgrade_tool LD$")), [&rig, &armed, &moved](FakeToolInvocation* i) {
        const QByteArray listing = rig.bus.ldOutput();
        if (armed && !moved) {
            moved = true;
            rig.bus.moveToAdb(serialOf(1));   // after the listing was taken, before the checks read the bus
        }
        i->emitOutput(listing);
        i->finish(0);
    });
    scriptFleet(rig.bus, rig.runner);
    QVERIFY(rig.run());
    QVERIFY(moved);
    const FleetBoard b = rig.boardOf(serialOf(1));
    QCOMPARE(b.result, FleetBoard::Result::Failed);
    QCOMPARE(b.detail, QStringLiteral("left Loader mode before rootfs was written (port %1 shows an ADB device; LD lists LocationID %2): nothing "
                                      "written for it")
                           .arg(portOf(1), b.locationId));
    QCOMPARE(rig.count(QRegularExpression(QStringLiteral("^upgrade_tool -s %1 WL ").arg(b.locationId))), 0);
    QVERIFY(rig.bus.resetsTo(serialOf(1)).isEmpty());
    QVERIFY(!b.leftInLoader);
    QVERIFY(b.retry.isEmpty());
    QCOMPARE(rig.flasher.report().totals().done, 6);
    QCOMPARE(rig.bus.unaddressedCommands(), 0);
}

// =============================================================================================
// FleetReport
// =============================================================================================

void Tst_fleetflasher::reportTableAndJson()
{
    FleetBoard a;
    a.tag = QStringLiteral("A");
    a.serial = serialOf(0);
    a.adbState = QStringLiteral("device");
    a.expectedPort = portOf(0);
    a.port = portOf(0);
    a.locationId = QStringLiteral("113");
    a.kernel = QStringLiteral("5.10.209+");
    a.stage = FleetBoard::Stage::Finished;
    a.result = FleetBoard::Result::Done;
    a.written = {QStringLiteral("rootfs")};
    a.adbBack = true;
    a.boardInfo = QStringLiteral("Linux version 5.10.209+ (builder@rk) #1\n 12:54:01 up 0 min,  load average: 0.10");
    a.uptime = QStringLiteral("up 0 min");
    a.rfmcu = QStringLiteral("pid=412 uart=owned mcu=alive");
    a.seconds = 48.1;
    a.mappingSeconds = 1.8;
    a.writingSeconds = 27.9;
    a.profile = QStringLiteral("node");   // (v0.8.0)
    FleetBoard b = a;
    b.tag = QStringLiteral("B");
    b.serial = serialOf(1);
    b.port = portOf(1);
    b.locationId = QStringLiteral("112");
    b.expectedPort = portOf(5);   // moved
    FleetBoard c;
    c.tag = QStringLiteral("C");
    c.serial = serialOf(2);
    c.stage = FleetBoard::Stage::Finished;
    c.result = FleetBoard::Result::Skipped;
    c.detail = QStringLiteral("not on ADB: board %1 is not attached; attached: none").arg(serialOf(2));
    FleetBoard d = a;
    d.tag = QStringLiteral("D");
    d.serial = serialOf(3);
    d.port = portOf(3);
    d.locationId = QStringLiteral("111");
    d.result = FleetBoard::Result::NotBack;
    d.adbBack = false;
    d.boardInfo.clear();
    d.uptime.clear();
    d.rfmcu.clear();
    d.detail = QStringLiteral("partitions written and the board reset, but ADB did not come back within 60 s; check the serial console");
    FleetBoard e = d;
    e.tag = QStringLiteral("E");
    e.serial = serialOf(4);
    e.port = portOf(4);
    e.locationId = QStringLiteral("114");
    e.result = FleetBoard::Result::Failed;
    e.written.clear();
    e.leftInLoader = true;
    e.retry = {QStringLiteral("upgrade_tool -s 114 WL 8064 10240 C:\\x\\rootfs.img"), QStringLiteral("upgrade_tool -s 114 RD")};
    e.detail = QStringLiteral("writing rootfs failed: Write LBA failed, err=-3");

    FleetReport report;
    report.setBoards({a, b, c, d, e});
    report.setImage(QStringLiteral("C:/img/update.img"));
    report.setPartitions({QStringLiteral("rootfs")});
    report.setSeconds(53.0, 11.2, 29.8);
    QCOMPARE(report.totals(), (FleetReport::Totals{5, 0, 2, 1, 1, 1}));
    QCOMPARE(report.summary(), QStringLiteral("2 of 5 boards flashed in 53.0 s (mapping 11.2 s, writing 29.8 s); 1 not back on ADB; 1 failed; 1 skipped"));
    QVERIFY(!report.allOk());
    QCOMPARE(report.exitCode(), 1);
    FleetFlasher::UnattributedDevice stray;
    stray.locationId = QStringLiteral("154");
    stray.port = portOf(6);
    stray.mode = QStringLiteral("Loader");
    QCOMPARE(stray.hint(), QStringLiteral("upgrade_tool -s 154 RD"));
    report.setUnattributed({stray});
    QVERIFY(report.summary().endsWith(QStringLiteral("; 1 skipped; 1 Loader device left unattributed")));

    // The table: the header, aligned columns, "-" cells, the detail of a done board.
    const QStringList table = report.tableLines();
    QCOMPARE(table.size(), qsizetype(6));
    QVERIFY(table.first().startsWith(QStringLiteral("board  serial            port            loader  result    mapping  writing  seconds  detail")));
    const qsizetype resultColumn = table.first().indexOf(QStringLiteral("result"));
    const qsizetype detailColumn = table.first().indexOf(QStringLiteral("detail"));
    for (const QString& line : table) {
        QVERIFY(!line.endsWith(QLatin1Char(' ')));
    }
    QCOMPARE(table.at(1).mid(resultColumn, 4), QStringLiteral("done"));
    QCOMPARE(table.at(1).mid(detailColumn), QStringLiteral("5.10.209+, up 0 min, rfmcu: pid=412 uart=owned mcu=alive"));
    QCOMPARE(table.at(2).mid(detailColumn), QStringLiteral("5.10.209+, up 0 min, rfmcu: pid=412 uart=owned mcu=alive (moved)"));
    QVERIFY(table.at(3).contains(QStringLiteral("  -               -       skipped   -        -        -        not on ADB: ")));
    QVERIFY(table.at(4).contains(QStringLiteral("  111     not back  1.8      27.9     48.1     partitions written and the board reset")));
    QCOMPARE(table.at(5).mid(resultColumn, 6), QStringLiteral("failed"));
    QVERIFY(table.at(5).contains(QStringLiteral("  114  ")));   // the loader column keeps the id after the end

    // The JSON fields.
    const QJsonObject json = report.json();
    for (const char* key : {"fleet", "dryRun", "image", "partitions", "maxParallel", "seconds", "mappingSeconds", "writingSeconds", "stop",
                            "unattributed", "summary", "boards"}) {
        QVERIFY2(json.contains(QLatin1String(key)), key);
    }
    QCOMPARE(json.value(QStringLiteral("fleet")).toBool(), true);
    QCOMPARE(json.value(QStringLiteral("image")).toString(), QDir::toNativeSeparators(QStringLiteral("C:/img/update.img")));
    QCOMPARE(json.value(QStringLiteral("seconds")).toDouble(), 53.0);
    QCOMPARE(json.value(QStringLiteral("summary")).toObject(),
             (QJsonObject{{QStringLiteral("selected"), 5}, {QStringLiteral("done"), 2}, {QStringLiteral("notBack"), 1},
                          {QStringLiteral("failed"), 1}, {QStringLiteral("skipped"), 1}}));
    QCOMPARE(json.value(QStringLiteral("unattributed")).toArray().first().toObject(),
             (QJsonObject{{QStringLiteral("loader"), QStringLiteral("154")}, {QStringLiteral("port"), portOf(6)},
                          {QStringLiteral("mode"), QStringLiteral("Loader")}, {QStringLiteral("hint"), QStringLiteral("upgrade_tool -s 154 RD")}}));
    QCOMPARE(json.value(QStringLiteral("boards")).toArray().size(), qsizetype(5));
    const QJsonObject aJson = FleetReport::boardJson(a);
    for (const char* key : {"tag", "serial", "adb", "expectedPort", "port", "loader", "stage", "result", "ok", "seconds", "mappingSeconds",
                            "writingSeconds", "detail", "written", "retry", "adbBack", "leftInLoader", "kernel", "uptime", "boardInfo", "rfmcu",
                            "profile"}) {
        QVERIFY2(aJson.contains(QLatin1String(key)), key);
    }
    QCOMPARE(aJson.value(QStringLiteral("profile")).toString(), QStringLiteral("node"));   // (v0.8.0)
    QCOMPARE(FleetReport::boardJson(c).value(QStringLiteral("profile")).toString(), QString());
    QCOMPARE(aJson.value(QStringLiteral("result")).toString(), QStringLiteral("done"));
    QCOMPARE(aJson.value(QStringLiteral("stage")).toString(), QStringLiteral("finished"));
    QCOMPARE(aJson.value(QStringLiteral("loader")).toString(), QStringLiteral("113"));
    QCOMPARE(aJson.value(QStringLiteral("seconds")).toDouble(), 48.1);
    QVERIFY(aJson.value(QStringLiteral("retry")).toArray().isEmpty());
    QCOMPARE(FleetReport::boardJson(e).value(QStringLiteral("retry")).toArray().size(), qsizetype(2));
    QCOMPARE(FleetReport::boardJson(e).value(QStringLiteral("leftInLoader")).toBool(), true);
    QCOMPARE(FleetReport::boardJson(d).value(QStringLiteral("result")).toString(), QStringLiteral("not back"));

    // A stop, a refusal, aborted.
    FleetReport stopped = report;
    stopped.setStop(QStringLiteral("mapping stopped: while rebooting X: two boards changed at once"));
    stopped.setResetCount(2);
    QCOMPARE(stopped.summary(), QStringLiteral("mapping stopped: while rebooting X: two boards changed at once; nothing was written; 2 boards reset back "
                                               "to ADB; 1 Loader device left unattributed"));
    QCOMPARE(stopped.exitCode(), 1);
    FleetReport refused;
    refused.setStop(QStringLiteral("a rockusb device is already in Loader mode on port P (LocationID 1) that no selected board accounts for"));
    QCOMPARE(refused.summary(), refused.stop());
    QCOMPARE(refused.exitCode(), 1);   // an empty board list is never 0
    QCOMPARE(refused.tableLines().size(), qsizetype(1));
    FleetReport empty;
    QVERIFY(!empty.allOk());
    QCOMPARE(empty.exitCode(), 1);
    FleetReport none;
    none.setStop(QStringLiteral("no selected board is on ADB (attached: none)"));
    QVERIFY(none.noneOnAdb());
    QCOMPARE(none.exitCode(), 3);
    FleetReport aborted = report;
    aborted.setAborted(true);
    QCOMPARE(aborted.exitCode(), 5);

    // All done: 0; one not back: 1.
    FleetReport good;
    good.setBoards({a, b});
    good.setSeconds(10.0, 2.0, 5.0);
    QVERIFY(good.allOk());
    QCOMPARE(good.exitCode(), 0);
    QCOMPARE(good.summary(), QStringLiteral("2 of 2 boards flashed in 10.0 s (mapping 2.0 s, writing 5.0 s)"));
    good.setBoards({a, d});
    QCOMPARE(good.exitCode(), 1);

    // A dry run: its own table and counts.
    FleetBoard readyBoard = a;
    readyBoard.result = FleetBoard::Result::Ready;
    readyBoard.expectedPort.clear();
    FleetReport dry;
    dry.setDryRun(true);
    dry.setBoards({readyBoard, c});
    QCOMPARE(dry.summary(), QStringLiteral("dry run: 1 of 2 boards ready; 1 skipped"));
    QCOMPARE(dry.exitCode(), 1);
    QCOMPARE(dry.tableLines().first(), QStringLiteral("board  serial            adb     expected port  result   detail"));
    QVERIFY(dry.tableLines().at(1).contains(QStringLiteral("device  unknown        ready")));
    QCOMPARE(dry.json().value(QStringLiteral("summary")).toObject(),
             (QJsonObject{{QStringLiteral("selected"), 2}, {QStringLiteral("ready"), 1}, {QStringLiteral("skipped"), 1}}));
    dry.setBoards({readyBoard});
    QCOMPARE(dry.summary(), QStringLiteral("dry run: 1 of 1 boards ready"));
    QCOMPARE(dry.exitCode(), 0);

    QCOMPARE(FleetReport::alignColumns({{QStringLiteral("a"), QStringLiteral("bb")}, {QStringLiteral("ccc"), QStringLiteral("d")}}),
             (QStringList{QStringLiteral("a    bb"), QStringLiteral("ccc  d")}));
    QCOMPARE(FleetReport::alignColumns({{QStringLiteral("x"), QString()}}), QStringList{QStringLiteral("x")});   // no trailing blanks
}

// =============================================================================================
// Board profiles (v0.8.0): the profile gate at step 2 b and at Mapped
// =============================================================================================

namespace {

ProfileDetection nodePlan()
{
    ProfileEvidence evidence;
    evidence.chipTag = QStringLiteral("110E");
    return BoardProfiles::detectImage(evidence);
}

ProfileDetection overridePlan(const QString& id)
{
    ProfileEvidence evidence;
    evidence.overrideId = id;
    return BoardProfiles::detectBoard(evidence);
}

QString stackedLayoutLine()
{
    return QStringLiteral("profile: Stacked (RV1106G2) - from the board's flash layout (6 partitions: env, idblock, uboot, meta, boot, "
                          "userdata)");
}

} // namespace

void Tst_fleetflasher::profileGateStopsAMixedBench()
{
    FleetRig rig(caseDir());
    rig.flasher.setPlanProfile(nodePlan());
    QCOMPARE(rig.flasher.planProfile().profile.id, QStringLiteral("node"));
    rig.bus.setMtdParts(serialOf(2), kStackedMtdParts);
    QVERIFY(rig.run());
    QVERIFY(!rig.ok());
    ProfileEvidence stacked;
    stacked.layout = kStackedMtdParts;
    const QString reason = ProfileRules::mixedBenchStop(serialOf(2), BoardProfiles::detectBoard(stacked), nodePlan());
    QCOMPARE(reason, QStringLiteral("mixed bench: board fe89cc526a354343 is Stacked (RV1106G2) (from the board's flash layout (6 "
                                    "partitions: env, idblock, uboot, meta, boot, userdata)) but the plan is for Node (RV1106BG2) (from "
                                    "the image's chip tag 110E); one profile per run: name boards of one profile with --serials"));
    QVERIFY2(rig.message().startsWith(QStringLiteral("mapping stopped: ") + reason + QStringLiteral("; nothing was written")),
             qPrintable(rig.dump()));
    QVERIFY(rig.message().contains(QStringLiteral("2 boards reset back to ADB")));
    QVERIFY(rig.lineIndex(QStringLiteral("plan profile: Node (RV1106BG2) - from the image's chip tag 110E")) >= 0);
    const int line = rig.lineIndex(rig.boardOf(serialOf(2)).label() + QLatin1Char(' ') + stackedLayoutLine());
    QVERIFY2(line >= 0, qPrintable(rig.dump()));
    QVERIFY(rig.lineIndex(QStringLiteral("mapping stopped: ") + reason) > line);
    QCOMPARE(rig.count(addressedWl()), 0);
    QCOMPARE(rig.bus.resetsTo(serialOf(0)).size(), qsizetype(1));
    QCOMPARE(rig.bus.resetsTo(serialOf(1)).size(), qsizetype(1));
    QCOMPARE(rig.count(QRegularExpression(QStringLiteral("^adb -s %1 reboot loader$").arg(serialOf(2)))), 0);   // stopped before its reboot
    const FleetBoard c = rig.boardOf(serialOf(2));
    QCOMPARE(c.result, FleetBoard::Result::Failed);
    QCOMPARE(c.detail, reason);
    QCOMPARE(c.profile, QStringLiteral("stacked"));
    QCOMPARE(rig.boardOf(serialOf(0)).profile, QStringLiteral("node"));
    for (int k = 3; k < kBench; ++k) {
        const FleetBoard later = rig.boardOf(serialOf(k));
        QCOMPARE(later.result, FleetBoard::Result::Skipped);
        QCOMPARE(later.detail, QStringLiteral("mapping stopped: ") + reason);
    }
    QCOMPARE(rig.flasher.report().exitCode(), 1);
}

void Tst_fleetflasher::profileGateOffWhenPlanUnknown()
{
    FleetRig rig(caseDir());   // no plan profile: the gate is off, the layout gate decides
    rig.bus.setMtdParts(serialOf(2), kStackedMtdParts);
    QVERIFY(rig.run());
    QVERIFY(rig.lineIndex(QStringLiteral("plan profile: unknown; the profile gate is off")) >= 0);
    const FleetBoard c = rig.boardOf(serialOf(2));
    QCOMPARE(c.result, FleetBoard::Result::Skipped);
    QCOMPARE(c.detail, QStringLiteral("flash layout differs from the plan's (board: %1; plan: %2): not rebooted").arg(kStackedMtdParts, kNodeMtdParts));
    QCOMPARE(c.profile, QStringLiteral("stacked"));   // detected all the same, and logged
    QVERIFY(rig.lineIndex(c.label() + QLatin1Char(' ') + stackedLayoutLine()) >= 0);
    QCOMPARE(rig.flasher.report().totals().done, 6);
}

void Tst_fleetflasher::profileGateIgnoresEnvExemption()
{
    // env among the rows lifts the layout gate (the env written defines the next layout) - never the profile gate.
    FleetRig rig(caseDir(), {QStringLiteral("env"), QStringLiteral("rootfs")});
    rig.flasher.setPlanProfile(nodePlan());
    rig.bus.setMtdParts(serialOf(2), kStackedMtdParts);
    QVERIFY(rig.run());
    QVERIFY(!rig.ok());
    QVERIFY2(rig.message().startsWith(QStringLiteral("mapping stopped: mixed bench: board fe89cc526a354343 is Stacked (RV1106G2)")),
             qPrintable(rig.dump()));
    QCOMPARE(rig.count(addressedWl()), 0);
    QCOMPARE(rig.boardOf(serialOf(2)).result, FleetBoard::Result::Failed);
}

void Tst_fleetflasher::overrideMakesEveryBoardThePlan()
{
    // --profile stacked with a stacked plan: every board is the plan's profile, so the profile gate
    // cannot trip; the Node layouts differ from the plan's, so the layout gate skips them all.
    FleetRig rig(caseDir());
    rig.flasher.setProfileOverride(QStringLiteral("stacked"));
    QCOMPARE(rig.flasher.profileOverride(), QStringLiteral("stacked"));
    rig.flasher.setPlanProfile(overridePlan(QStringLiteral("stacked")));
    rig.flasher.setLayout(kStackedMtdParts, true);
    QVERIFY(rig.run());
    QVERIFY(!rig.message().startsWith(QStringLiteral("mapping stopped")));
    QCOMPARE(rig.count(QRegularExpression(QStringLiteral("reboot loader$"))), 0);
    for (const FleetBoard& board : rig.flasher.boards()) {
        QCOMPARE(board.profile, QStringLiteral("stacked"));
        QCOMPARE(board.result, FleetBoard::Result::Skipped);
        QVERIFY2(board.detail.startsWith(QStringLiteral("flash layout differs from the plan's")), qPrintable(board.detail));
    }
    QVERIFY(rig.anyLineContains(QStringLiteral("profile: Stacked (RV1106G2) - set by --profile stacked (or the Board profile box)")));
    FleetFlasher plain(nullptr);
    plain.setProfileOverride(QStringLiteral("nope"));
    QVERIFY(plain.profileOverride().isEmpty());   // an id nobody knows counts as ""
    QVERIFY(!plain.planProfile().known());
}

void Tst_fleetflasher::profileInBoardJson()
{
    FleetBoard a;
    a.tag = QStringLiteral("A");
    a.serial = serialOf(0);
    a.profile = QStringLiteral("node");
    FleetBoard u = a;
    u.profile.clear();
    QCOMPARE(FleetReport::boardJson(a).value(QStringLiteral("profile")).toString(), QStringLiteral("node"));
    QVERIFY(FleetReport::boardJson(u).contains(QStringLiteral("profile")));
    QCOMPARE(FleetReport::boardJson(u).value(QStringLiteral("profile")).toString(), QString());
    QVERIFY(!(a == u));   // FleetBoard::operator== compares the profile too
}

void Tst_fleetflasher::dryRunSaysProfilesNotRead()
{
    const QString sentence = QStringLiteral("Dry run: the boards' profiles are read before their reboot in a real run");
    FleetRig dry(caseDir());
    dry.flasher.setDryRun(true);
    dry.flasher.setPlanProfile(nodePlan());
    QVERIFY(dry.run());
    QVERIFY2(dry.ok(), qPrintable(dry.dump()));
    QCOMPARE(dry.lines.count(sentence), qsizetype(1));
    const int first = dry.lineStarting(QStringLiteral("Dry run: the selection"));
    QCOMPARE(dry.lineIndex(sentence), first + 1);
    QCOMPARE(dry.count(QRegularExpression(QStringLiteral("cat /proc/cmdline"))), 0);
    FleetRig real(caseDir() + QStringLiteral("-real"));
    QVERIFY(real.run());
    QCOMPARE(real.lines.count(sentence), qsizetype(0));
}

void Tst_fleetflasher::profileGateUsesTheMappedPid()
{
    // --serials names a G2 whose /proc/cmdline carries no mtdparts: step 2 b cannot tell it and the
    // layout gate has nothing to compare - its Loader PID tells it once it is mapped, before the
    // next board is rebooted and before anything is written.
    FleetRig rig(caseDir());
    rig.flasher.setBoards(benchBoards({0, 1, 2, 3}));
    rig.flasher.setPlanProfile(nodePlan());
    rig.bus.setMtdParts(serialOf(2), QString());
    rig.bus.setLoaderPid(serialOf(2), 0x110c);
    QCOMPARE(rig.bus.board(serialOf(2)).loaderPid, quint16(0x110c));
    QVERIFY(rig.run());
    QVERIFY(!rig.ok());
    const FleetBoard c = rig.boardOf(serialOf(2));
    ProfileEvidence pid;
    pid.loaderPid = 0x110c;
    const QString reason = ProfileRules::mixedBenchStop(serialOf(2), BoardProfiles::detectBoard(pid), nodePlan());
    QVERIFY(reason.contains(QStringLiteral("(from the board's Loader PID 0x110c)")));
    const int unknown = rig.lineIndex(c.label() + QStringLiteral(" profile: unknown (no --profile, no Loader PID, product string or flash "
                                                                 "layout matched a profile)"));
    const int mapped = rig.lineStarting(c.label() + QStringLiteral(" in Loader mode on port ") + portOf(2) + QStringLiteral(", LocationID "));
    const int known = rig.lineIndex(c.label() + QStringLiteral(" profile: Stacked (RV1106G2) - from the board's Loader PID 0x110c"));
    const int stop = rig.lineIndex(QStringLiteral("mapping stopped: ") + reason);
    QVERIFY2(unknown >= 0 && mapped > unknown && known > mapped && stop > known, qPrintable(rig.dump()));
    QCOMPARE(c.result, FleetBoard::Result::Failed);
    QCOMPARE(c.detail, reason);
    QCOMPARE(c.profile, QStringLiteral("stacked"));
    QCOMPARE(rig.boardOf(serialOf(0)).profile, QStringLiteral("node"));
    // A, B and C - it was mapped - reset with their own address; D never rebooted; nothing written.
    for (int k = 0; k < 3; ++k) {
        QCOMPARE(rig.bus.resetsTo(serialOf(k)).size(), qsizetype(1));
    }
    QCOMPARE(rig.count(QRegularExpression(QStringLiteral("^adb -s %1 reboot loader$").arg(serialOf(3)))), 0);
    QCOMPARE(rig.boardOf(serialOf(3)).result, FleetBoard::Result::Skipped);
    QCOMPARE(rig.boardOf(serialOf(3)).detail, QStringLiteral("mapping stopped: ") + reason);
    QCOMPARE(rig.count(addressedWl()), 0);
    QCOMPARE(rig.flasher.report().exitCode(), 1);
    QVERIFY2(rig.message().endsWith(QStringLiteral("; nothing was written; 3 boards reset back to ADB")), qPrintable(rig.message()));
    QCOMPARE(rig.bus.unaddressedCommands(), 0);
}

void Tst_fleetflasher::overrideContradictsMappedPidStops()
{
    // --profile stacked: every board is the plan's at step 2 b; B's Loader PID says Node.
    FleetRig rig(caseDir());
    rig.flasher.setProfileOverride(QStringLiteral("stacked"));
    rig.flasher.setPlanProfile(overridePlan(QStringLiteral("stacked")));
    for (int k = 0; k < kBench; ++k) {
        rig.bus.setLoaderPid(serialOf(k), k == 1 ? 0x110e : 0x110c);
    }
    QVERIFY(rig.run());
    QVERIFY(!rig.ok());
    const QString reason =
        QStringLiteral("the --profile stacked override contradicts the board's Loader PID 0x110e (the Node (RV1106BG2)'s); drop --profile");
    QVERIFY2(rig.message().startsWith(QStringLiteral("mapping stopped: ") + reason), qPrintable(rig.dump()));
    QCOMPARE(rig.boardOf(serialOf(1)).result, FleetBoard::Result::Failed);
    QCOMPARE(rig.boardOf(serialOf(1)).detail, reason);
    QCOMPARE(rig.bus.resetsTo(serialOf(0)).size(), qsizetype(1));
    QCOMPARE(rig.bus.resetsTo(serialOf(1)).size(), qsizetype(1));
    QCOMPARE(rig.count(QRegularExpression(QStringLiteral("^adb -s %1 reboot loader$").arg(serialOf(2)))), 0);
    QCOMPARE(rig.count(addressedWl()), 0);
}

QTEST_GUILESS_MAIN(Tst_fleetflasher)
#include "tst_fleetflasher.moc"
