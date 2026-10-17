// UI-free test suite for the bench inventory (v0.8.0): Inventory (src/firmware/Inventory.h) - the
// record's JSON, the file with its lock, atomic writes and the corrupt-file rule, the table, md5 -
// on a QTemporaryDir per case, and InventoryRecorder (src/firmware/InventoryRecorder.h) on a
// FirmwareJob driven by FakeToolRunner (the fixtures' scripts: no adb, no upgrade_tool is ever
// started), FakeUsbBus for the fleet and the USB enumeration, the in-process TestSshServer as the
// build machine and FakeMcu behind a FakeMcuServer as the CC1310. QSettings and every file live in
// temporary directories or the QStandardPaths test-mode data directory.
#include <QtTest>

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QProcess>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QThread>

#include <cstdio>
#include <memory>
#include <optional>

#include "app/AppSettings.h"
#include "firmware/FirmwareJob.h"
#include "firmware/FleetFlasher.h"
#include "firmware/FleetReport.h"
#include "firmware/ImagePuller.h"
#include "firmware/Inventory.h"
#include "firmware/InventoryRecorder.h"
#include "firmware/SocFlasher.h"
#include "ssh/SshProfile.h"
#include "support/FakeMcu.h"
#include "support/FakeToolRunner.h"
#include "support/FakeUsbBus.h"
#include "support/FirmwareFixtures.h"
#include "support/TestSshServer.h"

using namespace FirmwareFixtures;

namespace {

constexpr int kOpTimeoutMs = 20000;
constexpr int kChainTimeoutMs = 60000;
const QString kSerial = QStringLiteral("0225ce3c8d8d6e44");
const QString kLoaderLine = QStringLiteral("inventory: the board in Loader mode has no serial; not recorded");
const QString kEmptyLd = QStringLiteral("List of rockusb connected(0)\r\n");

QString dataFile(const char* name)
{
    return QStringLiteral(SU_TEST_DATA_DIR "/firmware/") + QLatin1String(name);
}

QByteArray readBytes(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QJsonObject readJson(const QString& path)
{
    return QJsonDocument::fromJson(readBytes(path)).object();
}

QJsonObject boardJson(const QString& path, const QString& serial)
{
    return readJson(path).value(QStringLiteral("boards")).toObject().value(serial).toObject();
}

std::optional<InventoryRecord> recordAt(const QString& path, const QString& serial)
{
    Inventory inventory(path);
    if (!inventory.load()) {
        return std::nullopt;
    }
    return inventory.record(serial);
}

/// A time the file can hold exactly (to the second).
QDateTime second(const QDateTime& time)
{
    return time.addMSecs(-time.time().msec());
}

InventoryRecord fullRecord(const QString& serial)
{
    const QDateTime base = second(QDateTime(QDate(2026, 10, 16), QTime(22, 10, 31)));
    InventoryRecord record;
    record.serial = serial;
    record.profile = QStringLiteral("node");
    record.port = QStringLiteral("6&38635FDB&0&3");
    record.layout = kNodeMtdParts;
    record.gadgetSerialString = InventoryRecord::Tri::No;
    record.lastImage.file = QStringLiteral("phaselock_fix2_update.img");
    record.lastImage.chipTag = QStringLiteral("110E");
    record.lastImage.md5 = QStringLiteral("9e107d9d372bb6826bd81d3542a419d6");
    record.lastImage.gitDescribe = QStringLiteral("v1.3.0-4-ge0e0d41");
    record.lastImage.partitions = {QStringLiteral("boot"), QStringLiteral("rootfs")};
    record.lastImage.written = base;
    record.mcuVersion = QStringLiteral("0.8.1");
    record.mcuRole = QStringLiteral("rx");
    record.mcuRead = base.addSecs(31);
    record.lastOperation.name = QStringLiteral("flash");
    record.lastOperation.result = QStringLiteral("done");
    record.lastOperation.message = QStringLiteral("Upgrade firmware ok; board is back on ADB");
    record.lastOperation.time = base;
    record.firstSeen = base.addDays(-2);
    record.lastSeen = base.addSecs(31);
    return record;
}

/// A job as the firmware suites use it: fake tools with plain names, fast polls.
std::unique_ptr<FirmwareJob> makeJob(FakeToolRunner* runner, SshProfileStore* store = nullptr)
{
    auto job = std::make_unique<FirmwareJob>(runner);
    job->setProfileStore(store);
    job->setToolPaths(QStringLiteral("upgrade_tool"), QStringLiteral("adb"));
    job->socFlasher()->setPollIntervalMs(10);
    return job;
}

/// `upgrade_tool LD` sees nothing, `adb devices` the Node board, its /proc/cmdline has the layout.
void scriptAdbBoard(FakeToolRunner& runner)
{
    runner.addScript(QRegularExpression(QStringLiteral("^upgrade_tool LD$")), kEmptyLd.toLatin1());
    runner.addScript(QRegularExpression(QStringLiteral("^adb( -s \\S+)? devices$")), kAdbBoard);
    runner.addScript(QRegularExpression(QStringLiteral("cat /proc/cmdline")), kNodeCmdline);
}

/// The bench's usual flash (tst_firmwarecli's flashBoardOnAdbGoesThroughLoader): the board runs
/// (ADB), `adb reboot loader` puts it into the mode `downloadLd` lists (Loader: WL; MASKROM: UF),
/// RD / UF brings it back on ADB. Registered first, so these handlers win over scriptSocFlash()'s.
struct BoardState
{
    bool inDownload = false;
    QByteArray downloadLd = kLdLoader;
};

std::shared_ptr<BoardState> scriptAdbFlash(FakeToolRunner& runner)
{
    auto state = std::make_shared<BoardState>();
    runner.addHandler(QRegularExpression(QStringLiteral("^upgrade_tool LD$")), [state](FakeToolInvocation* i) {
        i->emitOutput(state->inDownload ? state->downloadLd : kEmptyLd.toLatin1());
        i->finish(0);
    });
    runner.addScript(QRegularExpression(QStringLiteral("^adb( -s \\S+)? devices$")), kAdbBoard);
    runner.addHandler(QRegularExpression(QStringLiteral("^adb( -s \\S+)? reboot loader$")), [state](FakeToolInvocation* i) {
        state->inDownload = true;
        i->finish(0);
    });
    runner.addHandler(QRegularExpression(QStringLiteral("^upgrade_tool RD$")), [state](FakeToolInvocation* i) {
        state->inDownload = false;
        i->emitOutput("Reset Device OK.\r\n");
        i->finish(0);
    });
    runner.addHandler(QRegularExpression(QStringLiteral("^upgrade_tool UF ")), [state](FakeToolInvocation* i) {
        state->inDownload = false;
        i->emitOutput("Download Image... (100%)\r\nUpgrade firmware ok.\r\n");
        i->finish(0);
    });
    scriptSocFlash(runner, kLdLoader);   // EXF, WL, get-state, /proc/version, /proc/cmdline
    return state;
}

UsbDeviceInfo gadget(const QString& lastSegment)
{
    UsbDeviceInfo device;
    device.vid = 0x2207;
    device.pid = 0x0006;
    device.instanceId = QStringLiteral("USB\\VID_2207&PID_0006\\") + lastSegment;
    device.description = QStringLiteral("Android ADB Interface");
    device.driverClass = QStringLiteral("AndroidUsbDeviceClass");
    return device;
}

/// One Node gadget named by its bus address (the bench's Windows view, open question 5).
QList<UsbDeviceInfo> oneBusAddressGadget(quint16)
{
    return {gadget(QStringLiteral("6&38635FDB&0&2"))};
}

bool waitForRows(const FirmwareJob& job, int rows)
{
    return QTest::qWaitFor([&]() { return job.planState() == FirmwareJob::PlanState::Ready && job.partitions().size() == rows; },
                           kOpTimeoutMs);
}

/// Every log line the recorder gave, and every recorded() event.
struct Lines : QObject
{
    explicit Lines(InventoryRecorder* recorder)
    {
        connect(recorder, &InventoryRecorder::logLine, this, [this](const QString& line) { lines << line; });
        connect(recorder, &InventoryRecorder::recorded, this, [this](const QString& serial, const QString& event) {
            events << event;
            serials << serial;
        });
        connect(recorder, &InventoryRecorder::failed, this, [this](const QString& error) { failures << error; });
    }
    QStringList lines;
    QStringList events;
    QStringList serials;
    QStringList failures;
};

// ---- (fix4, m5) two processes writing one inventory -----------------------------------------------

constexpr int kWriterRounds = 60;
const QString kWriterOption = QStringLiteral("--inventory-writer");

QString writerSerial(const QString& prefix, int round)
{
    return QStringLiteral("%1-%2").arg(prefix).arg(round, 3, 10, QLatin1Char('0'));
}

/// One writer of writersInTwoProcesses(): `rounds` updates of its own records through a fresh
/// Inventory each (as every su-fw run and the tab's recorder do), each followed by an unlocked read
/// that must parse and hold every record this writer wrote so far. Returns "" or what went wrong.
QString writeRounds(const QString& file, const QString& prefix, int rounds)
{
    for (int k = 0; k < rounds; ++k) {
        QString error;
        Inventory writer(file);
        if (!writer.update(writerSerial(prefix, k), [k](InventoryRecord& r) { r.mcuVersion = QString::number(k); }, &error)) {
            return QStringLiteral("%1: update %2: %3").arg(prefix).arg(k).arg(error);
        }
        Inventory reader(file);
        if (!reader.load(&error)) {
            return QStringLiteral("%1: read after update %2: %3").arg(prefix).arg(k).arg(error);
        }
        for (int j = 0; j <= k; ++j) {
            if (!reader.record(writerSerial(prefix, j))) {
                return QStringLiteral("%1: record %2 lost after update %3").arg(prefix).arg(j).arg(k);
            }
        }
    }
    return QString();
}

/// The second process: `tst_inventory --inventory-writer <file> <prefix> <rounds>` (main() below).
/// It says it is ready (<file>.ready), waits for the parent's go (<file>.go) so both write at once,
/// then writes. Exit 0 = every write landed and every read parsed; else the reason on stderr.
int runInventoryWriter(const QStringList& args)
{
    const QString file = args.value(2);
    const QString prefix = args.value(3);
    const int rounds = args.value(4).toInt();
    if (file.isEmpty() || prefix.isEmpty() || rounds <= 0) {
        std::fputs("usage: tst_inventory --inventory-writer <file> <prefix> <rounds>\n", stderr);
        return 2;
    }
    QFile ready(file + QStringLiteral(".ready"));
    if (!ready.open(QIODevice::WriteOnly)) {
        std::fputs("cannot write the ready file\n", stderr);
        return 3;
    }
    ready.close();
    QElapsedTimer waited;
    waited.start();
    while (!QFileInfo::exists(file + QStringLiteral(".go"))) {
        if (waited.elapsed() > 30000) {
            std::fputs("no go within 30 s\n", stderr);
            return 4;
        }
        QThread::msleep(1);
    }
    const QString problem = writeRounds(file, prefix, rounds);
    if (!problem.isEmpty()) {
        std::fputs(qPrintable(problem + QLatin1Char('\n')), stderr);
        return 1;
    }
    return 0;
}

} // namespace

// =======================================================================================
// Test class
// =======================================================================================

class Tst_inventory : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    // ---- The record and the file ------------------------------------------------------------
    void recordJsonRoundTrip();
    void triTexts();
    void defaultFilePath();
    void loadMissingIsEmpty();
    void loadCorruptReportsAndKeepsFile();
    void updateCreatesFileAndFolder();
    void updateKeepsOtherRecords();
    void updateIsAtomic();
    void updateMovesCorruptAside();
    void lockKeepsTwoWritersApart();
    void writersInTwoProcesses();   // (fix4, m5)
    void lockTimeoutIsAnError();
    void staleLockIsTakenOver();
    void lastSeenAutomatic();
    void recordsSortedBySerial();
    void tableLinesShape();
    void jsonShape();
    void md5HexStreams();
    void isoTimeRoundTrip();

    // ---- The recorder ---------------------------------------------------------------------------
    void recorderStatusWritesSeenAndProfile();
    void recorderGadgetSerialString();
    void recorderLayoutAndPort();
    void recorderMcuInfo();
    void recorderFlashDoneRecordsImage();
    void recorderSameImageMergesRows();
    void recorderRecordsWrittenRowsOnly();   // (fix4, m4)
    void recorderFlashFailedKeepsImage();
    void recorderCancelledResult();
    void recorderLoaderBoardWithoutSerialNotRecorded();
    void recorderStatusWithoutSerialClears();
    void recorderPullFlashRecordsOperation();
    void recorderFleetBoards();
    void recorderDryRunRecordsSeenOnly();
    void recorderFailedWriteLogsAndContinues();
    void recorderDisabledWritesNothing();
    void recorderMovedAsideIsLogged();
    void recorderNoSecretInFile();

private:
    QString path() const { return m_dir->filePath(QStringLiteral("inventory.json")); }
    QString caseDir(const QString& name) const { return m_dir->filePath(name); }
    /// An image whose env.img carries the Node layout (nodeExtractor: "/withenv" in its path).
    QString withEnvImage(const QString& name = QStringLiteral("update.img"), int size = 4096) const
    {
        const QString image = m_dir->filePath(QStringLiteral("withenv/") + name);
        return writeFile(image, rkfwBytes(size)) ? image : QString();
    }

    std::unique_ptr<QTemporaryDir> m_dir;
    QTemporaryDir m_settingsDir;
};

// =======================================================================================
// Fixture
// =======================================================================================

void Tst_inventory::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("BuildAI-Test"));
    // (v0.8.0) The pid in the name: the test-mode data folder (firmware.lock, inventory.json) is this
    // run's own, so two runs of the suite at once (two build folders) never meet on the lock.
    QCoreApplication::setApplicationName(QStringLiteral("SerialUtilityTest-inventory-%1").arg(QCoreApplication::applicationPid()));
    QVERIFY(m_settingsDir.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settingsDir.filePath(QStringLiteral("settings")));
    QSettings().clear();
    QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
    qputenv("SU_SSH_IGNORE_CONFIG", "1");   // the SSH connects of this suite must not read ~/.ssh/config
    qunsetenv("SSH_AUTH_SOCK");             // nor offer the developer's agent keys to the test server
    QLoggingCategory::setFilterRules(QStringLiteral("buildai.*.info=false"));
}

void Tst_inventory::cleanupTestCase()
{
    // (v0.8.0) The run's own data folder goes with it (its name carries this process's pid).
    const QString data = AppSettings::dataDirectory();
    if (QStandardPaths::isTestModeEnabled() && data.endsWith(QStringLiteral("-%1").arg(QCoreApplication::applicationPid()))) {
        QDir(data).removeRecursively();
    }
}

void Tst_inventory::init()
{
    QSettings().clear();
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
}

void Tst_inventory::cleanup()
{
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

// =======================================================================================
// The record and the file
// =======================================================================================

void Tst_inventory::recordJsonRoundTrip()
{
    const InventoryRecord record = fullRecord(kSerial);
    const QJsonObject object = record.toJson();
    QCOMPARE(object.value(QStringLiteral("serial")).toString(), kSerial);
    QCOMPARE(object.value(QStringLiteral("gadgetSerialString")).toString(), QStringLiteral("no"));
    const QJsonObject image = object.value(QStringLiteral("lastImage")).toObject();
    QCOMPARE(image.value(QStringLiteral("partitions")).toArray(), (QJsonArray{QStringLiteral("boot"), QStringLiteral("rootfs")}));
    QCOMPARE(image.value(QStringLiteral("md5")).toString(), QStringLiteral("9e107d9d372bb6826bd81d3542a419d6"));
    QCOMPARE(object.value(QStringLiteral("mcu")).toObject().value(QStringLiteral("version")).toString(), QStringLiteral("0.8.1"));
    QCOMPARE(object.value(QStringLiteral("lastOperation")).toObject().value(QStringLiteral("result")).toString(), QStringLiteral("done"));
    QCOMPARE(InventoryRecord::fromJson(kSerial, object), record);
    // Through the text form as well (what the file holds).
    const QJsonObject reparsed = QJsonDocument::fromJson(QJsonDocument(object).toJson()).object();
    QCOMPARE(InventoryRecord::fromJson(kSerial, reparsed), record);

    // The key wins over "serial"; an unknown field is ignored; a missing field keeps its default.
    QJsonObject other = object;
    other.insert(QStringLiteral("serial"), QStringLiteral("someone-else"));
    other.insert(QStringLiteral("bogus"), 42);
    InventoryRecord expected = record;
    expected.serial = QStringLiteral("62ecfc6ddeb3f579");
    QCOMPARE(InventoryRecord::fromJson(QStringLiteral("62ecfc6ddeb3f579"), other), expected);
    InventoryRecord bare;
    bare.serial = kSerial;
    QCOMPARE(InventoryRecord::fromJson(kSerial, QJsonObject()), bare);
    QVERIFY(InventoryRecord::fromJson(kSerial, QJsonObject()).lastImage.partitions.isEmpty());   // absent -> empty
    // Never learned = not written.
    const QJsonObject bareJson = bare.toJson();
    QCOMPARE(bareJson.keys(), QStringList{QStringLiteral("serial")});
    // A bad time is invalid, nothing else is lost.
    QJsonObject badTime = object;
    badTime.insert(QStringLiteral("lastSeen"), QStringLiteral("yesterday-ish"));
    const InventoryRecord withBadTime = InventoryRecord::fromJson(kSerial, badTime);
    QVERIFY(!withBadTime.lastSeen.isValid());
    QCOMPARE(withBadTime.profile, QStringLiteral("node"));
    // An image written without a known md5 is kept (the file name says what was written).
    InventoryRecord noMd5 = bare;
    noMd5.lastImage.file = QStringLiteral("update.img");
    QVERIFY(noMd5.toJson().contains(QStringLiteral("lastImage")));
    QCOMPARE(InventoryRecord::fromJson(kSerial, noMd5.toJson()), noMd5);
}

void Tst_inventory::triTexts()
{
    using Tri = InventoryRecord::Tri;
    QCOMPARE(InventoryRecord::triText(Tri::Unknown), QStringLiteral("unknown"));
    QCOMPARE(InventoryRecord::triText(Tri::Yes), QStringLiteral("yes"));
    QCOMPARE(InventoryRecord::triText(Tri::No), QStringLiteral("no"));
    for (const Tri value : {Tri::Unknown, Tri::Yes, Tri::No}) {
        QCOMPARE(InventoryRecord::triFromText(InventoryRecord::triText(value)), value);
    }
    QCOMPARE(InventoryRecord::triFromText(QStringLiteral(" YES ")), Tri::Yes);
    QCOMPARE(InventoryRecord::triFromText(QStringLiteral("maybe")), Tri::Unknown);
    QCOMPARE(InventoryRecord::triFromText(QString()), Tri::Unknown);
}

void Tst_inventory::defaultFilePath()
{
    const QString expected = AppSettings::dataDirectory() + QStringLiteral("/inventory.json");
    QCOMPARE(Inventory::defaultFilePath(), expected);
    QVERIFY(Inventory::defaultFilePath().endsWith(QStringLiteral("/inventory.json")));
    QCOMPARE(Inventory().filePath(), expected);
    QCOMPARE(Inventory(QString()).filePath(), expected);
    QCOMPARE(Inventory(path()).filePath(), QFileInfo(path()).absoluteFilePath());
}

void Tst_inventory::loadMissingIsEmpty()
{
    Inventory inventory(path());
    QString error = QStringLiteral("untouched");
    QVERIFY(inventory.load(&error));
    QVERIFY(error.isEmpty());
    QVERIFY(inventory.records().isEmpty());
    QVERIFY(!inventory.record(kSerial));
    QVERIFY(!QFileInfo::exists(path()));   // reading creates nothing
}

void Tst_inventory::loadCorruptReportsAndKeepsFile()
{
    for (const QByteArray& bytes : {QByteArray("not json"), QByteArray("[1,2]"), QByteArray("{\"boards\":5}")}) {
        // A good file first: a corrupt one clears what was loaded.
        Inventory inventory(path());
        QFile::remove(path());
        QVERIFY(inventory.update(kSerial, nullptr));
        QCOMPARE(inventory.records().size(), qsizetype(1));
        QVERIFY(writeFile(path(), bytes));
        QString error;
        QVERIFY2(!inventory.load(&error), bytes.constData());
        QVERIFY2(error.startsWith(QDir::toNativeSeparators(path()) + QStringLiteral(": ")), qPrintable(error));
        QVERIFY(inventory.records().isEmpty());
        QCOMPARE(readBytes(path()), bytes);   // never touched by a load
    }
}

void Tst_inventory::updateCreatesFileAndFolder()
{
    const QString file = m_dir->filePath(QStringLiteral("new/sub/inventory.json"));
    QVERIFY(!QFileInfo::exists(QFileInfo(file).absolutePath()));
    Inventory inventory(file);
    QString error;
    const QDateTime before = second(QDateTime::currentDateTime());
    QVERIFY2(inventory.update(kSerial, [](InventoryRecord& r) { r.profile = QStringLiteral("node"); }, &error), qPrintable(error));
    QVERIFY(QFileInfo::exists(file));
    const QJsonObject root = readJson(file);
    QCOMPARE(root.value(QStringLiteral("version")).toInt(), Inventory::kVersion);
    QVERIFY(Inventory::timeFromIso(root.value(QStringLiteral("updated")).toString()).isValid());
    const QJsonObject board = root.value(QStringLiteral("boards")).toObject().value(kSerial).toObject();
    QCOMPARE(board.value(QStringLiteral("serial")).toString(), kSerial);
    QCOMPARE(board.value(QStringLiteral("profile")).toString(), QStringLiteral("node"));
    const std::optional<InventoryRecord> record = inventory.record(kSerial);
    QVERIFY(record);
    QVERIFY(record->firstSeen.isValid() && record->firstSeen >= before);
    QCOMPARE(record->lastSeen, record->firstSeen);
    // A second Inventory on the same path reads the same.
    Inventory reader(file);
    QVERIFY(reader.load());
    QCOMPARE(reader.records(), inventory.records());
    // firstSeen stays, lastSeen moves.
    QVERIFY(inventory.update(kSerial, [](InventoryRecord& r) { r.port = QStringLiteral("6&38635FDB&0&3"); }));
    QCOMPARE(inventory.record(kSerial)->firstSeen, record->firstSeen);
    QCOMPARE(inventory.record(kSerial)->profile, QStringLiteral("node"));   // never cleared by a later change
}

void Tst_inventory::updateKeepsOtherRecords()
{
    Inventory inventory(path());
    const QStringList serials{QStringLiteral("aaa"), QStringLiteral("bbb"), QStringLiteral("ccc")};
    for (const QString& serial : serials) {
        QVERIFY(inventory.update(serial, [serial](InventoryRecord& r) {
            r = fullRecord(serial);   // every field, the serial put back by update()
        }));
    }
    const QJsonObject before = readJson(path()).value(QStringLiteral("boards")).toObject();
    QCOMPARE(before.size(), qsizetype(3));
    QTest::qWait(1100);   // a second later: an unchanged record must not carry a new time
    QVERIFY(inventory.update(QStringLiteral("bbb"), [](InventoryRecord& r) { r.mcuVersion = QStringLiteral("0.8.2"); }));
    const QJsonObject after = readJson(path()).value(QStringLiteral("boards")).toObject();
    for (const char* other : {"aaa", "ccc"}) {
        QCOMPARE(QJsonDocument(after.value(QLatin1String(other)).toObject()).toJson(),
                 QJsonDocument(before.value(QLatin1String(other)).toObject()).toJson());
    }
    QCOMPARE(after.value(QStringLiteral("bbb")).toObject().value(QStringLiteral("mcu")).toObject().value(QStringLiteral("version")).toString(),
             QStringLiteral("0.8.2"));
}

void Tst_inventory::updateIsAtomic()
{
    Inventory inventory(path());
    for (int k = 0; k < 50; ++k) {
        const QString serial = QStringLiteral("board%1").arg(k % 5);
        QVERIFY(inventory.update(serial, [k](InventoryRecord& r) { r.mcuVersion = QString::number(k); }));
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(readBytes(path()), &error);
        QVERIFY2(error.error == QJsonParseError::NoError && document.isObject(), qPrintable(QString::number(k)));
    }
    // Nothing next to the file: no QSaveFile temporary, no lock file left.
    QCOMPARE(QDir(m_dir->path()).entryList(QDir::Files | QDir::Hidden | QDir::System), QStringList{QStringLiteral("inventory.json")});
    Inventory reader(path());
    QVERIFY(reader.load());
    QCOMPARE(reader.records().size(), qsizetype(5));
}

void Tst_inventory::updateMovesCorruptAside()
{
    const QByteArray corrupt("{\"boards\": [\"hand-edited\"");
    QVERIFY(writeFile(path(), corrupt));
    Inventory inventory(path());
    QString error;
    QVERIFY2(inventory.update(kSerial, nullptr, &error), qPrintable(error));
    const QString aside = inventory.movedAsideFile();
    QVERIFY2(aside.startsWith(path() + QStringLiteral(".corrupt-")), qPrintable(aside));
    QVERIFY(QRegularExpression(QStringLiteral("\\.corrupt-\\d{8}-\\d{6}(-\\d+)?$")).match(aside).hasMatch());
    QCOMPARE(readBytes(aside), corrupt);
    QCOMPARE(readJson(path()).value(QStringLiteral("boards")).toObject().keys(), QStringList{kSerial});
    // The next update moves nothing.
    QVERIFY(inventory.update(kSerial, nullptr));
    QVERIFY(inventory.movedAsideFile().isEmpty());
}

void Tst_inventory::lockKeepsTwoWritersApart()
{
    Inventory first(path());
    Inventory second(path());
    for (int k = 0; k < 20; ++k) {
        QVERIFY(first.update(QStringLiteral("aaa"), [k](InventoryRecord& r) { r.mcuVersion = QString::number(k); }));
        QVERIFY(second.update(QStringLiteral("bbb"), [k](InventoryRecord& r) { r.mcuVersion = QString::number(k); }));
        // Every write re-reads under the lock: neither one loses the other's record.
        Inventory reader(path());
        QVERIFY(reader.load());
        QCOMPARE(reader.records().size(), qsizetype(2));
        QCOMPARE(reader.record(QStringLiteral("aaa"))->mcuVersion, QString::number(k));
        QCOMPARE(reader.record(QStringLiteral("bbb"))->mcuVersion, QString::number(k));
    }
}

void Tst_inventory::writersInTwoProcesses()
{
    // (fix4, m5) The claim of decision 1 - the tab and any number of su-fw write one file at once -
    // with two real processes: this one and a second tst_inventory started as a plain writer (main()
    // below). Both start on one signal and hammer update() with their own records; every unlocked
    // read in between must parse (QSaveFile's atomic rename under the lock, Windows sharing retries),
    // and at the end every record of both is there, the file is valid and no lock or temporary is left.
    const QString file = path();
    QProcess child;
    child.setProgram(QCoreApplication::applicationFilePath());
    child.setArguments({kWriterOption, file, QStringLiteral("child"), QString::number(kWriterRounds)});
    child.setProcessChannelMode(QProcess::MergedChannels);
    child.start();
    QVERIFY2(child.waitForStarted(30000), qPrintable(child.errorString()));
    QVERIFY2(QTest::qWaitFor([&file]() { return QFileInfo::exists(file + QStringLiteral(".ready")); }, 120000),
             qPrintable(QString::fromUtf8(child.readAll())));
    QVERIFY(writeFile(file + QStringLiteral(".go"), "go"));
    const QString problem = writeRounds(file, QStringLiteral("parent"), kWriterRounds);
    if (!child.waitForFinished(300000)) {
        child.kill();
        child.waitForFinished(5000);
        QFAIL(qPrintable(QStringLiteral("the second writer did not end within 300 s: %1").arg(QString::fromUtf8(child.readAll()))));
    }
    const QString childOutput = QString::fromUtf8(child.readAll());
    QVERIFY2(problem.isEmpty(), qPrintable(problem));
    QCOMPARE(child.exitStatus(), QProcess::NormalExit);
    QVERIFY2(child.exitCode() == 0, qPrintable(QStringLiteral("exit %1: %2").arg(child.exitCode()).arg(childOutput)));

    Inventory merged(file);
    QString error;
    QVERIFY2(merged.load(&error), qPrintable(error));
    QCOMPARE(merged.records().size(), qsizetype(2 * kWriterRounds));
    for (int k = 0; k < kWriterRounds; ++k) {
        for (const QString& prefix : {QStringLiteral("parent"), QStringLiteral("child")}) {
            const std::optional<InventoryRecord> record = merged.record(writerSerial(prefix, k));
            QVERIFY2(record, qPrintable(writerSerial(prefix, k)));
            QCOMPARE(record->mcuVersion, QString::number(k));
        }
    }
    QCOMPARE(QJsonDocument::fromJson(readBytes(file)).object().value(QStringLiteral("boards")).toObject().size(), 2 * kWriterRounds);
    QStringList left = QDir(QFileInfo(file).absolutePath()).entryList(QDir::Files | QDir::Hidden);
    left.removeAll(QStringLiteral("inventory.json.ready"));
    left.removeAll(QStringLiteral("inventory.json.go"));
    QCOMPARE(left, QStringList{QStringLiteral("inventory.json")});   // no lock, no QSaveFile temporary
}

void Tst_inventory::lockTimeoutIsAnError()
{
    Inventory inventory(path());
    QLockFile held(path() + QStringLiteral(".lock"));
    QVERIFY(held.tryLock(0));
    QElapsedTimer clock;
    clock.start();
    QString error;
    QVERIFY(!inventory.update(kSerial, nullptr, &error));
    const qint64 elapsed = clock.elapsed();
    QVERIFY2(elapsed < Inventory::kLockWaitMs + 15000, qPrintable(QString::number(elapsed)));   // a loaded CI runner overshoots
    QVERIFY2(elapsed >= Inventory::kLockWaitMs - 200, qPrintable(QString::number(elapsed)));
    QVERIFY2(error.contains(QStringLiteral("locked")), qPrintable(error));
    QVERIFY(!QFileInfo::exists(path()));   // nothing written
    held.unlock();
    QVERIFY2(inventory.update(kSerial, nullptr, &error), qPrintable(error));
}

void Tst_inventory::staleLockIsTakenOver()
{
    // A lock left by a process that is gone (pid 999999 never exists on Windows; the program name
    // differs from every running one on Linux), and older than the stale time anyway.
    const QString lockPath = path() + QStringLiteral(".lock");
    QVERIFY(writeFile(lockPath, QByteArray("999999\nno-such-program-inventory-test\n") + QSysInfo::machineHostName().toUtf8() + '\n'));
    {
        QFile file(lockPath);
        QVERIFY(file.open(QIODevice::ReadWrite));
        QVERIFY(file.setFileTime(QDateTime::currentDateTime().addSecs(-3600), QFileDevice::FileModificationTime));
    }
    Inventory inventory(path());
    QElapsedTimer clock;
    clock.start();
    QString error;
    QVERIFY2(inventory.update(kSerial, nullptr, &error), qPrintable(error));
    QVERIFY(clock.elapsed() < Inventory::kLockWaitMs);
    QVERIFY(!QFileInfo::exists(lockPath));   // released after the write
}

void Tst_inventory::lastSeenAutomatic()
{
    Inventory inventory(path());
    const QDateTime before = second(QDateTime::currentDateTime());
    QVERIFY(inventory.update(kSerial, nullptr));
    const QDateTime seen = inventory.record(kSerial)->lastSeen;
    QVERIFY(seen >= before && seen <= QDateTime::currentDateTime());
    const QDateTime earlier = second(QDateTime::currentDateTime().addDays(-3));
    QVERIFY(inventory.update(kSerial, [earlier](InventoryRecord& r) { r.lastSeen = earlier; }));
    QCOMPARE(inventory.record(kSerial)->lastSeen, earlier);   // the change's own value wins
    QVERIFY(recordAt(path(), kSerial));
    QCOMPARE(recordAt(path(), kSerial)->lastSeen, earlier);
}

void Tst_inventory::recordsSortedBySerial()
{
    Inventory inventory(path());
    for (const char* serial : {"b", "a", "c"}) {
        QVERIFY(inventory.update(QLatin1String(serial), nullptr));
    }
    QStringList order;
    for (const InventoryRecord& record : inventory.records()) {
        order << record.serial;
    }
    QCOMPARE(order, (QStringList{QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")}));
    Inventory reader(path());
    QVERIFY(reader.load());
    QCOMPARE(reader.records(), inventory.records());
}

void Tst_inventory::tableLinesShape()
{
    const InventoryRecord full = fullRecord(kSerial);
    InventoryRecord empty;
    empty.serial = QStringLiteral("62ecfc6ddeb3f579");
    const QStringList lines = Inventory::tableLines({empty, full});   // sorted by serial in the table
    const auto time = [](const QDateTime& t) { return t.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")); };
    const QList<QStringList> rows{
        {QStringLiteral("serial"), QStringLiteral("profile"), QStringLiteral("port"), QStringLiteral("image"), QStringLiteral("tag"),
         QStringLiteral("md5"), QStringLiteral("written"), QStringLiteral("mcu"), QStringLiteral("last operation"), QStringLiteral("result"),
         QStringLiteral("time"), QStringLiteral("last seen")},
        {kSerial, QStringLiteral("node"), QStringLiteral("6&38635FDB&0&3"), QStringLiteral("phaselock_fix2_update.img"), QStringLiteral("110E"),
         QStringLiteral("9e107d9d"), time(full.lastImage.written), QStringLiteral("0.8.1 rx"), QStringLiteral("flash"), QStringLiteral("done"),
         time(full.lastOperation.time), time(full.lastSeen)},
        {QStringLiteral("62ecfc6ddeb3f579"), QStringLiteral("-"), QStringLiteral("-"), QStringLiteral("-"), QStringLiteral("-"),
         QStringLiteral("-"), QStringLiteral("-"), QStringLiteral("-"), QStringLiteral("-"), QStringLiteral("-"), QStringLiteral("-"),
         QStringLiteral("-")},
    };
    QCOMPARE(lines, FleetReport::alignColumns(rows));
    QCOMPARE(lines.size(), qsizetype(3));
    QVERIFY(lines.first().startsWith(QStringLiteral("serial            profile  port            image  ")));
    QVERIFY(lines.at(1).contains(QStringLiteral("  9e107d9d  2026-10-16 22:10  0.8.1 rx  flash  ")));
    QVERIFY(lines.at(2).startsWith(QStringLiteral("62ecfc6ddeb3f579  -        -")));
    QCOMPARE(Inventory::tableLines({}), FleetReport::alignColumns({rows.first()}));   // the header alone
}

void Tst_inventory::jsonShape()
{
    const InventoryRecord a = fullRecord(QStringLiteral("aaa"));
    InventoryRecord b;
    b.serial = QStringLiteral("bbb");
    const QJsonObject json = Inventory::json({b, a}, path());
    QCOMPARE(json.value(QStringLiteral("file")).toString(), QDir::toNativeSeparators(path()));
    const QJsonArray boards = json.value(QStringLiteral("boards")).toArray();
    QCOMPARE(boards.size(), qsizetype(2));
    QCOMPARE(boards.at(0).toObject(), a.toJson());
    QCOMPARE(boards.at(1).toObject(), b.toJson());
    QCOMPARE(json.keys(), (QStringList{QStringLiteral("boards"), QStringLiteral("file")}));
}

void Tst_inventory::md5HexStreams()
{
    QByteArray bytes(5 * 1024 * 1024 + 123, Qt::Uninitialized);
    QRandomGenerator generator(4711);
    for (qsizetype k = 0; k < bytes.size(); ++k) {
        bytes[k] = static_cast<char>(generator.bounded(256));
    }
    const QString file = caseDir(QStringLiteral("big.img"));
    QVERIFY(writeFile(file, bytes));
    QString error = QStringLiteral("untouched");
    QCOMPARE(Inventory::md5Hex(file, &error), QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Md5).toHex()));
    QVERIFY(error.isEmpty());
    const QString missing = caseDir(QStringLiteral("missing.img"));
    QCOMPARE(Inventory::md5Hex(missing, &error), QString());
    QVERIFY2(error.contains(QDir::toNativeSeparators(missing)), qPrintable(error));
    QCOMPARE(Inventory::md5Hex(QString(), &error), QString());
    QVERIFY(!error.isEmpty());
}

void Tst_inventory::isoTimeRoundTrip()
{
    const QDateTime time(QDate(2026, 10, 16), QTime(22, 10, 31));
    const QString text = Inventory::isoTime(time);
    QVERIFY2(QRegularExpression(QStringLiteral("^2026-10-16T22:10:31([+-]\\d\\d:\\d\\d|Z)$")).match(text).hasMatch(), qPrintable(text));
    QCOMPARE(Inventory::timeFromIso(text), time);
    // An instant given in another offset is the same instant, written with the local offset.
    const QDateTime utc = time.toUTC();
    QCOMPARE(Inventory::isoTime(utc), text);
    QCOMPARE(Inventory::isoTime(QDateTime()), QString());
    QVERIFY(!Inventory::timeFromIso(QStringLiteral("garbage")).isValid());
    QVERIFY(!Inventory::timeFromIso(QString()).isValid());
    QCOMPARE(Inventory::timeFromIso(QStringLiteral("2026-10-16T14:10:31Z")), QDateTime(QDate(2026, 10, 16), QTime(14, 10, 31), QTimeZone::UTC));
}

// =======================================================================================
// The recorder
// =======================================================================================

void Tst_inventory::recorderStatusWritesSeenAndProfile()
{
    {
        FakeToolRunner runner;
        scriptAdbBoard(runner);
        auto job = makeJob(&runner);
        InventoryRecorder recorder(job.get());
        recorder.setFilePath(path());
        recorder.setUsbEnumerator(&oneBusAddressGadget);
        QCOMPARE(recorder.filePath(), QFileInfo(path()).absoluteFilePath());
        Lines seen(&recorder);
        QSignalSpy layoutDone(job.get(), &FirmwareJob::layoutReadFinished);
        QSignalSpy status(job.get(), &FirmwareJob::statusChanged);
        job->refreshStatus();
        QTRY_COMPARE_WITH_TIMEOUT(layoutDone.count(), qsizetype(1), kOpTimeoutMs);
        QCOMPARE(recorder.lastSerial(), kSerial);
        QCOMPARE(seen.events, (QStringList{QStringLiteral("status"), QStringLiteral("layout"), QStringLiteral("profile")}));
        QCOMPARE(seen.lines, (QStringList{QStringLiteral("inventory: board %1 recorded (status)").arg(kSerial),
                                          QStringLiteral("inventory: board %1 recorded (layout)").arg(kSerial),
                                          QStringLiteral("inventory: board %1 recorded (profile)").arg(kSerial)}));
        const std::optional<InventoryRecord> record = recordAt(path(), kSerial);
        QVERIFY(record);
        QVERIFY(record->lastSeen.isValid());
        QCOMPARE(record->layout, kNodeMtdParts);
        QCOMPARE(record->profile, QStringLiteral("node"));
        QCOMPARE(record->gadgetSerialString, InventoryRecord::Tri::No);
        QVERIFY(record->lastOperation.name.isEmpty());   // a status is no operation
        QVERIFY(!record->lastImage.isValid());

        // A second status within the minute: no write at all.
        const QByteArray bytes = readBytes(path());
        seen.events.clear();
        job->refreshStatus();
        QTRY_COMPARE_WITH_TIMEOUT(status.count(), qsizetype(2), kOpTimeoutMs);
        QTest::qWait(50);
        QVERIFY(seen.events.isEmpty());
        QCOMPARE(readBytes(path()), bytes);   // "updated" included
    }
    {
        // THE OVERRIDE (decision 14): the box says Stacked, the board's layout says Node - nothing
        // is inventoried from the claim; dropping it, the detection is.
        const QString file = caseDir(QStringLiteral("override/inventory.json"));
        FakeToolRunner runner;
        scriptAdbBoard(runner);
        auto job = makeJob(&runner);
        job->setProfileOverride(QStringLiteral("stacked"));
        InventoryRecorder recorder(job.get());
        recorder.setFilePath(file);
        recorder.setUsbEnumerator(&oneBusAddressGadget);
        Lines seen(&recorder);
        QSignalSpy layoutDone(job.get(), &FirmwareJob::layoutReadFinished);
        job->refreshStatus();
        QTRY_COMPARE_WITH_TIMEOUT(layoutDone.count(), qsizetype(1), kOpTimeoutMs);
        QCOMPARE(job->boardProfile().source, ProfileDetection::Source::Option);
        QVERIFY(recordAt(file, kSerial));
        QCOMPARE(recordAt(file, kSerial)->profile, QString());
        QVERIFY(!seen.events.contains(QStringLiteral("profile")));
        QCOMPARE(recordAt(file, kSerial)->layout, kNodeMtdParts);
        job->setProfileOverride(QString());
        QCOMPARE(job->boardProfile().source, ProfileDetection::Source::BoardLayout);
        QCOMPARE(recordAt(file, kSerial)->profile, QStringLiteral("node"));
        QCOMPARE(seen.events.last(), QStringLiteral("profile"));
    }
}

void Tst_inventory::recorderGadgetSerialString()
{
    const auto run = [this](const QString& name, const QList<UsbDeviceInfo>& bus, InventoryRecord::Tri expected, int callsAfterThree) {
        const QString file = caseDir(name + QStringLiteral("/inventory.json"));
        FakeToolRunner runner;
        scriptAdbBoard(runner);
        auto job = makeJob(&runner);
        InventoryRecorder recorder(job.get());
        recorder.setFilePath(file);
        auto calls = std::make_shared<int>(0);
        recorder.setUsbEnumerator([calls, bus](quint16 vid) {
            ++*calls;
            return vid == UsbProbe::kRockchipVid ? bus : QList<UsbDeviceInfo>();
        });
        QSignalSpy status(job.get(), &FirmwareJob::statusChanged);
        QSignalSpy layoutDone(job.get(), &FirmwareJob::layoutReadFinished);
        job->refreshStatus();
        QTRY_COMPARE_WITH_TIMEOUT(layoutDone.count(), qsizetype(1), kOpTimeoutMs);
        QCOMPARE(*calls, 1);   // the first status
        QCOMPARE(recordAt(file, kSerial)->gadgetSerialString, expected);
        // Within the minute: nothing written, nothing enumerated.
        job->refreshStatus();
        QTRY_COMPARE_WITH_TIMEOUT(status.count(), qsizetype(2), kOpTimeoutMs);
        QCOMPARE(*calls, 1);
        // The throttle elapsed: the status is written; the bus is asked only while the answer is unknown.
        QVERIFY(recorder.inventory()->update(kSerial, [](InventoryRecord& r) { r.lastSeen = QDateTime::currentDateTime().addSecs(-120); }));
        Lines seen(&recorder);
        job->refreshStatus();
        QTRY_COMPARE_WITH_TIMEOUT(status.count(), qsizetype(3), kOpTimeoutMs);
        QTRY_COMPARE(seen.events, QStringList{QStringLiteral("status")});
        QCOMPARE(*calls, callsAfterThree);
        QCOMPARE(recordAt(file, kSerial)->gadgetSerialString, expected);
    };
    run(QStringLiteral("bus-address"), {gadget(QStringLiteral("6&38635FDB&0&2"))}, InventoryRecord::Tri::No, 1);
    run(QStringLiteral("serial-string"), {gadget(QStringLiteral("0225CE3C8D8D6E44"))}, InventoryRecord::Tri::Yes, 1);
    run(QStringLiteral("two-gadgets"), {gadget(QStringLiteral("6&38635FDB&0&2")), gadget(QStringLiteral("6&38635FDB&0&3"))},
        InventoryRecord::Tri::Unknown, 2);
}

void Tst_inventory::recorderLayoutAndPort()
{
    FakeToolRunner runner;
    auto job = makeJob(&runner);
    InventoryRecorder recorder(job.get());
    recorder.setFilePath(path());
    Lines seen(&recorder);
    emit job->mtdPartsRead(kSerial, QStringLiteral("mtdparts=") + kNodeMtdParts);
    emit job->boardPortLearned(kSerial, QStringLiteral("6&38635FDB&0&3"));
    QCOMPARE(seen.events, (QStringList{QStringLiteral("layout"), QStringLiteral("port")}));
    QCOMPARE(recordAt(path(), kSerial)->layout, kNodeMtdParts);   // normalized: no "mtdparts="
    QCOMPARE(recordAt(path(), kSerial)->port, QStringLiteral("6&38635FDB&0&3"));
    // No serial (a layout read in Loader mode): no record keyed "", the line once.
    const QByteArray bytes = readBytes(path());
    emit job->mtdPartsRead(QString(), kNodeMtdParts);
    emit job->boardPortLearned(QString(), QStringLiteral("6&38635FDB&0&3"));
    emit job->mtdPartsRead(QString(), kNodeMtdParts);
    QCOMPARE(readBytes(path()), bytes);
    QCOMPARE(seen.lines.count(kLoaderLine), qsizetype(1));
    QCOMPARE(seen.events.size(), qsizetype(2));
}

void Tst_inventory::recorderMcuInfo()
{
    FakeToolRunner runner;
    scriptAdbBoard(runner);
    runner.addScript(QRegularExpression(QStringLiteral("killall rfmcu")), QByteArray());
    runner.addScript(QRegularExpression(QStringLiteral("rfmcu cmd version")), "OK version=0.8.1 role=tx\r\n");
    auto job = makeJob(&runner);
    InventoryRecorder recorder(job.get());
    recorder.setFilePath(path());
    recorder.setUsbEnumerator(&oneBusAddressGadget);
    QSignalSpy layoutDone(job.get(), &FirmwareJob::layoutReadFinished);
    job->refreshStatus();   // su-fw's MCU commands query the status first
    QTRY_COMPARE_WITH_TIMEOUT(layoutDone.count(), qsizetype(1), kOpTimeoutMs);
    Lines seen(&recorder);
    QSignalSpy finished(job.get(), &FirmwareJob::operationFinished);
    const QDateTime before = second(QDateTime::currentDateTime());
    job->readMcuInfo();
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), qsizetype(1), kOpTimeoutMs);
    QVERIFY2(finished.first().at(1).toBool(), qPrintable(finished.first().at(2).toString()));
    QCOMPARE(seen.events, (QStringList{QStringLiteral("mcu"), QStringLiteral("info")}));
    const std::optional<InventoryRecord> record = recordAt(path(), kSerial);
    QCOMPARE(record->mcuVersion, QStringLiteral("0.8.1"));
    QCOMPARE(record->mcuRole, QStringLiteral("tx"));
    QVERIFY(record->mcuRead >= before);
    QCOMPARE(record->lastOperation.name, QStringLiteral("info"));
    QCOMPARE(record->lastOperation.result, QStringLiteral("done"));
}

void Tst_inventory::recorderFlashDoneRecordsImage()
{
    for (const bool all : {true, false}) {
        const QString file = caseDir(all ? QStringLiteral("all/inventory.json") : QStringLiteral("rootfs/inventory.json"));
        FakeToolRunner runner;
        scriptAdbFlash(runner);
        auto job = makeJob(&runner);
        const QString image = withEnvImage();
        QVERIFY(job->setImagePath(image));
        QVERIFY(waitForRows(*job, kNodePartitionCount));
        if (!all) {
            job->setSelectedPartitions({QStringLiteral("rootfs")});
        }
        InventoryRecorder recorder(job.get());
        recorder.setFilePath(file);
        recorder.setUsbEnumerator(&oneBusAddressGadget);
        Lines seen(&recorder);
        QStringList rowsAtTheEnd;
        connect(job.get(), &FirmwareJob::stepFinished, &recorder, [&](const QString&) { rowsAtTheEnd = job->selectedPartitions(); });
        QSignalSpy finished(job.get(), &FirmwareJob::operationFinished);
        const QDateTime before = second(QDateTime::currentDateTime());
        job->flashSoc();
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), qsizetype(1), kOpTimeoutMs);
        QVERIFY2(finished.first().at(1).toBool(), qPrintable(finished.first().at(2).toString()));
        QCOMPARE(rowsAtTheEnd, all ? nodeNames() : QStringList{QStringLiteral("rootfs")});
        QVERIFY(seen.events.contains(QStringLiteral("flash")));
        QVERIFY(seen.lines.contains(QStringLiteral("inventory: board %1 recorded (flash)").arg(kSerial)));
        const std::optional<InventoryRecord> record = recordAt(file, kSerial);
        QVERIFY(record);
        QCOMPARE(record->lastOperation.name, QStringLiteral("flash"));
        QCOMPARE(record->lastOperation.result, QStringLiteral("done"));
        QCOMPARE(record->lastOperation.message, QStringLiteral("Upgrade firmware ok; board is back on ADB"));
        QVERIFY(record->lastOperation.time >= before);
        QCOMPARE(record->lastImage.file, QStringLiteral("update.img"));
        QCOMPARE(record->lastImage.chipTag, QStringLiteral("110E"));
        QCOMPARE(record->lastImage.md5, Inventory::md5Hex(image));
        QCOMPARE(record->lastImage.gitDescribe, QString());   // a local image
        QCOMPARE(record->lastImage.partitions, rowsAtTheEnd);
        QVERIFY(record->lastImage.written >= before);
        QCOMPARE(record->layout, kNodeMtdParts);   // step 2's read
        QCOMPARE(record->profile, QStringLiteral("node"));
    }
}

void Tst_inventory::recorderSameImageMergesRows()
{
    FakeToolRunner runner;
    const std::shared_ptr<BoardState> board = scriptAdbFlash(runner);
    auto job = makeJob(&runner);
    InventoryRecorder recorder(job.get());
    recorder.setFilePath(path());
    recorder.setUsbEnumerator(&oneBusAddressGadget);
    QSignalSpy finished(job.get(), &FirmwareJob::operationFinished);
    const auto flash = [&](const QString& image, const QStringList& rows) {
        if (job->imagePath() != image) {
            if (!job->setImagePath(image) || !waitForRows(*job, kNodePartitionCount)) {
                return false;
            }
        }
        if (!QTest::qWaitFor([&]() { return runner.liveInvocations().isEmpty(); }, kOpTimeoutMs)) {   // the last flash's status
            return false;
        }
        job->setSelectedPartitions(rows);
        const qsizetype before = finished.count();
        job->flashSoc();
        if (!QTest::qWaitFor([&]() { return finished.count() > before; }, kOpTimeoutMs)) {
            return false;
        }
        return finished.last().at(1).toBool();
    };
    const QString m = withEnvImage(QStringLiteral("m.img"), 4096);
    const QString n = withEnvImage(QStringLiteral("n.img"), 8192);
    const QString md5M = Inventory::md5Hex(m);
    const QString md5N = Inventory::md5Hex(n);
    QVERIFY(md5M != md5N);

    QVERIFY(flash(m, {QStringLiteral("rootfs")}));
    QCOMPARE(recordAt(path(), kSerial)->lastImage.partitions, QStringList{QStringLiteral("rootfs")});
    // An hour back, so a newer `written` shows.
    const QDateTime old = second(QDateTime::currentDateTime().addSecs(-3600));
    QVERIFY(recorder.inventory()->update(kSerial, [old](InventoryRecord& r) { r.lastImage.written = old; }));

    QVERIFY(flash(m, {QStringLiteral("boot")}));
    InventoryRecord record = *recordAt(path(), kSerial);
    QCOMPARE(record.lastImage.partitions, (QStringList{QStringLiteral("rootfs"), QStringLiteral("boot")}));
    QCOMPARE(record.lastImage.md5, md5M);
    QVERIFY(record.lastImage.written > old);

    QVERIFY(flash(m, {QStringLiteral("rootfs")}));
    QCOMPARE(recordAt(path(), kSerial)->lastImage.partitions, (QStringList{QStringLiteral("rootfs"), QStringLiteral("boot")}));

    QVERIFY(flash(n, {QStringLiteral("boot")}));
    record = *recordAt(path(), kSerial);
    QCOMPARE(record.lastImage.file, QStringLiteral("n.img"));
    QCOMPARE(record.lastImage.md5, md5N);
    QCOMPARE(record.lastImage.partitions, QStringList{QStringLiteral("boot")});

    // `reboot loader` lands in MASKROM: UF writes the whole image, every row of it.
    board->downloadLd = kLdMaskrom;
    QVERIFY(flash(m, {QStringLiteral("boot")}));
    QCOMPARE(job->socFlasher()->flashMethod(), SocFlasher::FlashMethod::UpgradeFirmware);
    record = *recordAt(path(), kSerial);
    QCOMPARE(record.lastImage.md5, md5M);
    QStringList every;
    for (const PartitionImage& row : job->partitions()) {
        every << row.name;
    }
    QCOMPARE(every, nodeNames());
    QCOMPARE(record.lastImage.partitions, every);
}

void Tst_inventory::recorderRecordsWrittenRowsOnly()
{
    // (fix4, m4) The record holds the rows the flow wrote (SocFlasher::writtenPartitions()), not the
    // job's selection.
    {
        // The user checked rootfs, but the image is being planned again when Flash SoC is pressed: the
        // job hands the flasher no selection ("without such a plan the flasher decides: every
        // partition"), so every row is written. 0.8.0 recorded the checks - rootfs only, a board
        // recorded as running the old build in six rows it had just been written with.
        FakeToolRunner runner;
        scriptAdbFlash(runner);
        auto job = makeJob(&runner);
        const QString image = withEnvImage();
        QVERIFY(job->setImagePath(image));
        QVERIFY(waitForRows(*job, kNodePartitionCount));
        job->setSelectedPartitions({QStringLiteral("rootfs")});
        InventoryRecorder recorder(job.get());
        recorder.setFilePath(path());
        recorder.setUsbEnumerator(&oneBusAddressGadget);
        QStringList selectedAtTheEnd;
        connect(job.get(), &FirmwareJob::stepFinished, &recorder, [&](const QString&) { selectedAtTheEnd = job->selectedPartitions(); });
        QSignalSpy finished(job.get(), &FirmwareJob::operationFinished);
        QSignalSpy logs(job.get(), &FirmwareJob::logLine);
        job->planPartitions();   // a plan in flight
        QCOMPARE(job->planState(), FirmwareJob::PlanState::Planning);
        job->flashSoc();
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), qsizetype(1), kOpTimeoutMs);
        QVERIFY2(finished.first().at(1).toBool(), qPrintable(finished.first().at(2).toString()));
        bool everyRow = false;
        for (const QList<QVariant>& args : std::as_const(logs)) {
            everyRow = everyRow || args.at(0).toString().startsWith(QLatin1String("Partitions: every partition of the image"));
        }
        QVERIFY(everyRow);
        QCOMPARE(selectedAtTheEnd, QStringList{QStringLiteral("rootfs")});   // the checks ...
        QCOMPARE(job->socFlasher()->writtenPartitions(), nodeNames());       // ... and what was written
        const std::optional<InventoryRecord> record = recordAt(path(), kSerial);
        QVERIFY(record);
        QCOMPARE(record->lastImage.partitions, nodeNames());
        QCOMPARE(record->lastImage.md5, Inventory::md5Hex(image));
    }
    {
        // A row the flow skips is not recorded: the board's /proc/cmdline tells a layout without meta
        // and the image carries none, so meta is "not in the board's partition layout".
        const QString file = caseDir(QStringLiteral("skip/inventory.json"));
        FakeToolRunner runner;
        runner.addScript(QRegularExpression(QStringLiteral("cat /proc/cmdline")),
                         "console=ttyFIQ0 mtdparts=sfc_nor:64K(env),256K@64K(idblock),256K(uboot),3M@960K(boot),5M(rootfs),3M(userdata) "
                         "rootwait\n");
        scriptAdbFlash(runner);
        auto job = makeJob(&runner);
        job->setMtdParts(kNodeMtdParts);
        const QString image = m_dir->filePath(QStringLiteral("plain/update.img"));   // its env.img carries no layout
        QVERIFY(writeFile(image, rkfwBytes()));
        QVERIFY(job->setImagePath(image));
        QVERIFY(waitForRows(*job, kNodePartitionCount));
        InventoryRecorder recorder(job.get());
        recorder.setFilePath(file);
        recorder.setUsbEnumerator(&oneBusAddressGadget);
        QSignalSpy finished(job.get(), &FirmwareJob::operationFinished);
        QSignalSpy logs(job.get(), &FirmwareJob::logLine);
        job->flashSoc();
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), qsizetype(1), kOpTimeoutMs);
        QVERIFY2(finished.first().at(1).toBool(), qPrintable(finished.first().at(2).toString()));
        bool skipped = false;
        for (const QList<QVariant>& args : std::as_const(logs)) {
            skipped = skipped || args.at(0).toString().startsWith(QLatin1String("skipping meta: "));
        }
        QVERIFY(skipped);
        QStringList written = nodeNames();
        written.removeAll(QStringLiteral("meta"));
        QCOMPARE(job->socFlasher()->writtenPartitions(), written);
        const std::optional<InventoryRecord> record = recordAt(file, kSerial);
        QVERIFY(record);
        QCOMPARE(record->lastImage.partitions, written);
    }
}

void Tst_inventory::recorderFlashFailedKeepsImage()
{
    FakeToolRunner runner;
    runner.addScript(QRegularExpression(QStringLiteral("^upgrade_tool WL ")), "Write LBA failed, err=-3\r\n", 1);   // before the good one
    scriptAdbFlash(runner);
    auto job = makeJob(&runner);
    QVERIFY(job->setImagePath(withEnvImage()));
    QVERIFY(waitForRows(*job, kNodePartitionCount));
    job->setSelectedPartitions({QStringLiteral("rootfs")});
    InventoryRecorder recorder(job.get());
    recorder.setFilePath(path());
    recorder.setUsbEnumerator(&oneBusAddressGadget);
    const InventoryRecord::Image seeded = fullRecord(kSerial).lastImage;
    QVERIFY(recorder.inventory()->update(kSerial, [seeded](InventoryRecord& r) { r.lastImage = seeded; }));
    QSignalSpy finished(job.get(), &FirmwareJob::operationFinished);
    job->flashSoc();
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), qsizetype(1), kOpTimeoutMs);
    QVERIFY(!finished.first().at(1).toBool());
    const std::optional<InventoryRecord> record = recordAt(path(), kSerial);
    QCOMPARE(record->lastOperation.name, QStringLiteral("flash"));
    QCOMPARE(record->lastOperation.result, QStringLiteral("failed"));
    QCOMPARE(record->lastOperation.message, QStringLiteral("writing rootfs failed: Write LBA failed, err=-3"));
    QCOMPARE(record->lastImage, seeded);   // what the board ran before: a cut write is no image
}

void Tst_inventory::recorderCancelledResult()
{
    FakeToolRunner runner;
    // The board leaves ADB on `reboot loader` but never shows up in Loader mode: the flow waits.
    runner.addScript(QRegularExpression(QStringLiteral("^upgrade_tool LD$")), kEmptyLd.toLatin1());
    runner.addScript(QRegularExpression(QStringLiteral("^adb( -s \\S+)? devices$")), kAdbBoard);
    runner.addScript(QRegularExpression(QStringLiteral("^adb( -s \\S+)? reboot loader$")), QByteArray());
    scriptSocFlash(runner, kLdLoader);
    auto job = makeJob(&runner);
    QVERIFY(job->setImagePath(withEnvImage()));
    QVERIFY(waitForRows(*job, kNodePartitionCount));
    InventoryRecorder recorder(job.get());
    recorder.setFilePath(path());
    recorder.setUsbEnumerator(&oneBusAddressGadget);
    QSignalSpy finished(job.get(), &FirmwareJob::operationFinished);
    job->flashSoc();
    QTRY_COMPARE_WITH_TIMEOUT(job->socFlasher()->phase(), SocFlasher::Phase::WaitingForLoader, kOpTimeoutMs);
    QVERIFY(job->isCancellable());
    job->cancel();
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), qsizetype(1), kOpTimeoutMs);
    const std::optional<InventoryRecord> record = recordAt(path(), kSerial);
    QCOMPARE(record->lastOperation.name, QStringLiteral("flash"));
    QCOMPARE(record->lastOperation.result, QStringLiteral("cancelled"));
    QCOMPARE(record->lastOperation.message, QStringLiteral("cancelled"));
    QVERIFY(!record->lastImage.isValid());
}

void Tst_inventory::recorderLoaderBoardWithoutSerialNotRecorded()
{
    {
        // A board in Loader mode from the start: no serial, nothing recorded, the line once - the
        // Loader-mode layout read (layout-pl's mtdPartsRead("", ...), simulated at the write) and
        // the step's end do not repeat it.
        FakeToolRunner runner;
        scriptSocFlash(runner, kLdLoader);
        auto job = makeJob(&runner);
        job->setMtdParts(kNodeMtdParts);
        const QString image = caseDir(QStringLiteral("loader/update.img"));
        QVERIFY(writeFile(image, rkfwBytes()));
        QVERIFY(job->setImagePath(image));
        QVERIFY(waitForRows(*job, kNodePartitionCount));
        InventoryRecorder recorder(job.get());
        recorder.setFilePath(path());
        Lines seen(&recorder);
        FirmwareJob* j = job.get();
        connect(job->socFlasher(), &SocFlasher::phaseChanged, &recorder, [j](SocFlasher::Phase phase) {
            if (phase == SocFlasher::Phase::Flashing) {
                emit j->mtdPartsRead(QString(), kNodeMtdParts);
            }
        });
        QSignalSpy finished(job.get(), &FirmwareJob::operationFinished);
        QSignalSpy status(job.get(), &FirmwareJob::statusChanged);
        job->flashSoc();
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), qsizetype(1), kOpTimeoutMs);
        QVERIFY2(finished.first().at(1).toBool(), qPrintable(finished.first().at(2).toString()));
        QTRY_VERIFY_WITH_TIMEOUT(status.count() >= 2, kOpTimeoutMs);   // the status after the flash
        QTRY_VERIFY(runner.liveInvocations().isEmpty());
        QVERIFY(!QFileInfo::exists(path()));
        QCOMPARE(seen.lines, QStringList{kLoaderLine});
        QVERIFY(seen.events.isEmpty());
        QVERIFY(recorder.lastSerial().isEmpty());
    }
    {
        // The same job saw board X on ADB first (its record exists); then a board met in Loader
        // mode is flashed: X's record gets nothing of it.
        const QString file = caseDir(QStringLiteral("second/inventory.json"));
        FakeToolRunner runner;
        auto inLoader = std::make_shared<bool>(false);
        runner.addHandler(QRegularExpression(QStringLiteral("^upgrade_tool LD$")), [inLoader](FakeToolInvocation* i) {
            i->emitOutput(*inLoader ? kLdLoader : kEmptyLd.toLatin1());
            i->finish(0);
        });
        runner.addScript(QRegularExpression(QStringLiteral("^adb( -s \\S+)? devices$")), kAdbBoard);
        scriptSocFlash(runner, kLdLoader);
        auto job = makeJob(&runner);
        InventoryRecorder recorder(job.get());
        recorder.setFilePath(file);
        recorder.setUsbEnumerator(&oneBusAddressGadget);
        Lines seen(&recorder);
        QSignalSpy layoutDone(job.get(), &FirmwareJob::layoutReadFinished);
        job->refreshStatus();
        QTRY_COMPARE_WITH_TIMEOUT(layoutDone.count(), qsizetype(1), kOpTimeoutMs);
        QCOMPARE(recorder.lastSerial(), kSerial);
        const QJsonObject before = boardJson(file, kSerial);
        QVERIFY(!before.isEmpty());

        *inLoader = true;   // X unplugged, a bricked board Y put into Loader mode by hand
        const QString image = caseDir(QStringLiteral("second/update.img"));
        QVERIFY(writeFile(image, rkfwBytes()));
        QVERIFY(job->setImagePath(image));
        QVERIFY(waitForRows(*job, kNodePartitionCount));
        seen.lines.clear();
        seen.events.clear();
        QSignalSpy finished(job.get(), &FirmwareJob::operationFinished);
        job->flashSoc();
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), qsizetype(1), kOpTimeoutMs);
        QVERIFY2(finished.first().at(1).toBool(), qPrintable(finished.first().at(2).toString()));
        QTRY_VERIFY(runner.liveInvocations().isEmpty());
        QVERIFY(recorder.lastSerial().isEmpty());
        QCOMPARE(boardJson(file, kSerial), before);   // no lastOperation, no lastImage, not even lastSeen
        QVERIFY(!boardJson(file, kSerial).contains(QStringLiteral("lastOperation")));
        QVERIFY(!boardJson(file, kSerial).contains(QStringLiteral("lastImage")));
        QCOMPARE(seen.lines, QStringList{kLoaderLine});
        QVERIFY(seen.events.isEmpty());
    }
}

void Tst_inventory::recorderStatusWithoutSerialClears()
{
    struct Bench
    {
        QByteArray ld;
        QByteArray adb;
    };
    auto bench = std::make_shared<Bench>(Bench{kEmptyLd.toLatin1(), kAdbBoard});
    FakeToolRunner runner;
    runner.addHandler(QRegularExpression(QStringLiteral("^upgrade_tool LD$")), [bench](FakeToolInvocation* i) {
        i->emitOutput(bench->ld);
        i->finish(0);
    });
    runner.addHandler(QRegularExpression(QStringLiteral("^adb( -s \\S+)? devices$")), [bench](FakeToolInvocation* i) {
        i->emitOutput(bench->adb);
        i->finish(0);
    });
    runner.addScript(QRegularExpression(QStringLiteral("cat /proc/cmdline")), kNodeCmdline);
    auto job = makeJob(&runner);
    InventoryRecorder recorder(job.get());
    recorder.setFilePath(path());
    recorder.setUsbEnumerator(&oneBusAddressGadget);
    Lines seen(&recorder);
    QSignalSpy status(job.get(), &FirmwareJob::statusChanged);
    QSignalSpy layoutDone(job.get(), &FirmwareJob::layoutReadFinished);
    qsizetype statuses = 0;
    const auto query = [&]() {
        job->refreshStatus();
        ++statuses;
        return QTest::qWaitFor([&]() { return status.count() == statuses; }, kOpTimeoutMs);
    };

    QVERIFY(query());
    QTRY_COMPARE_WITH_TIMEOUT(layoutDone.count(), qsizetype(1), kOpTimeoutMs);
    QCOMPARE(recorder.lastSerial(), kSerial);
    const qsizetype written = seen.events.size();
    QCOMPARE(written, qsizetype(3));   // status, layout, profile
    const QByteArray bytes = readBytes(path());

    const struct
    {
        QByteArray ld;
        QByteArray adb;
        const char* what;
    } clearing[] = {
        {kLdLoader, kAdbBoard, "Loader"},
        {kLdMaskrom, kAdbBoard, "Maskrom"},
        {kEmptyLd.toLatin1(), QByteArray("List of devices attached\r\n\r\n"), "no board"},
        {kEmptyLd.toLatin1(), QByteArray("List of devices attached\r\n0225ce3c8d8d6e44\tdevice\r\n62ecfc6ddeb3f579\tdevice\r\n\r\n"), "two boards"},
    };
    for (const auto& c : clearing) {
        bench->ld = c.ld;
        bench->adb = c.adb;
        QVERIFY2(query(), c.what);
        QVERIFY2(recorder.lastSerial().isEmpty(), c.what);
        QCOMPARE(seen.events.size(), written);
        QCOMPARE(readBytes(path()), bytes);
    }
    QVERIFY(job->lastStatus().serial.isEmpty());   // the two boards: no serial, the detail says why
    QVERIFY2(job->lastStatus().detail.contains(QStringLiteral("several ADB boards")), qPrintable(job->lastStatus().detail));

    bench->ld = kEmptyLd.toLatin1();
    bench->adb = kAdbBoard;
    QVERIFY(query());
    QCOMPARE(recorder.lastSerial(), kSerial);
    QCOMPARE(seen.events.size(), written);   // within the minute: throttled
}

void Tst_inventory::recorderPullFlashRecordsOperation()
{
    SshProfileStore store;
    BuildMachine machine;
    QVERIFY(machine.start(&store, caseDir(QStringLiteral("work"))));
    FakeMcu mcu;
    FakeMcuServer bridge(&mcu);
    QVERIFY(bridge.listen());
    FakeToolRunner runner;
    scriptAdbFlash(runner);
    runner.addScript(QRegularExpression(QStringLiteral("rfmcu cmd version")), "OK version=0.8.1 role=rx\n");
    scriptMcuBridge(runner, bridge.port());
    auto job = makeJob(&runner, &store);
    const QString cache = caseDir(QStringLiteral("cache"));
    job->setSourceTarget(QStringLiteral("ssh:profile:") + machine.profile.id);
    job->setRemoteFolder(machine.imageFolder);
    job->setCacheDirectory(cache);
    QVERIFY(job->setPackagePath(dataFile("firmware-0.8.1.pkg")));
    InventoryRecorder recorder(job.get());
    recorder.setFilePath(path());
    recorder.setUsbEnumerator(&oneBusAddressGadget);
    Lines seen(&recorder);
    QSignalSpy finished(job.get(), &FirmwareJob::operationFinished);
    job->pullAndFlash();
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), qsizetype(1), kChainTimeoutMs);
    QVERIFY2(finished.first().at(1).toBool(), qPrintable(finished.first().at(2).toString()));
    QCOMPARE(seen.events.last(), QStringLiteral("pull-flash"));
    QVERIFY(seen.events.contains(QStringLiteral("flash")));
    InventoryRecord record = *recordAt(path(), kSerial);
    QCOMPARE(record.lastOperation.name, QStringLiteral("pull-flash"));
    QCOMPARE(record.lastOperation.result, QStringLiteral("done"));
    QCOMPARE(record.lastOperation.message, QStringLiteral("the MCU already runs 0.8.1; update skipped"));
    QCOMPARE(record.mcuVersion, QStringLiteral("0.8.1"));   // the chain's version read
    const QString cached = ImagePuller::imagePath(cache);
    QCOMPARE(record.lastImage.file, QStringLiteral("update.img"));
    QCOMPARE(record.lastImage.md5, Inventory::md5Hex(cached));
    QCOMPARE(record.lastImage.gitDescribe, job->cacheEntry().gitDescribe);
    QCOMPARE(record.lastImage.partitions, nodeNames());   // Pull & Flash writes every partition

    // The cache note's git describe is the image's (the build machine here prints none: the note
    // is given one, as a pull from a git checkout writes it).
    ImagePuller::CacheEntry entry = job->cacheEntry();
    QVERIFY(entry.isValid());
    entry.gitDescribe = QStringLiteral("v1.3.0-4-ge0e0d41");
    QVERIFY(ImagePuller::writeNote(entry));
    QCOMPARE(job->cacheEntry().gitDescribe, QStringLiteral("v1.3.0-4-ge0e0d41"));
    QTRY_VERIFY_WITH_TIMEOUT(runner.liveInvocations().isEmpty(), kOpTimeoutMs);
    job->flashSoc();
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), qsizetype(2), kOpTimeoutMs);
    QVERIFY2(finished.last().at(1).toBool(), qPrintable(finished.last().at(2).toString()));
    record = *recordAt(path(), kSerial);
    QCOMPARE(record.lastImage.gitDescribe, QStringLiteral("v1.3.0-4-ge0e0d41"));
    QCOMPARE(record.lastImage.md5, Inventory::md5Hex(cached));
}

void Tst_inventory::recorderFleetBoards()
{
    for (const bool withOverride : {false, true}) {
        const QString file = caseDir(withOverride ? QStringLiteral("override/inventory.json") : QStringLiteral("plain/inventory.json"));
        FakeToolRunner runner;
        FakeUsbBus bus;
        for (int k = 0; k < 3; ++k) {
            bus.addBoard(QLatin1String(kFleetBench[k].serial), QLatin1String(kFleetBench[k].port),
                         k == 2 ? FakeUsbBus::State::Absent : FakeUsbBus::State::Adb);
        }
        runner.addHandler(exfPattern(), &nodeExtractor);
        bus.script(runner);
        auto job = makeJob(&runner);
        job->fleetFlasher()->setUsbEnumerator([&bus](quint16 vid) { return bus.enumerate(vid); });
        job->fleetFlasher()->setPollIntervalMs(10);
        const QString image = withEnvImage(withOverride ? QStringLiteral("o.img") : QStringLiteral("p.img"));
        QVERIFY(job->setImagePath(image));
        QVERIFY(waitForRows(*job, kNodePartitionCount));
        job->setSelectedPartitions({QStringLiteral("rootfs")});
        FirmwareJob::FleetOptions options;
        for (int k = 0; k < 3; ++k) {
            options.boards.append({QLatin1String(kFleetBench[k].tag), QLatin1String(kFleetBench[k].serial)});
        }
        job->setFleetOptions(options);
        if (withOverride) {
            job->setProfileOverride(QStringLiteral("node"));
        }
        InventoryRecorder recorder(job.get());
        recorder.setFilePath(file);
        recorder.setUsbEnumerator([&bus](quint16 vid) { return bus.enumerate(vid); });
        if (withOverride) {
            // A record detected as "node" earlier: unchanged by the run; the others get nothing from the override.
            QVERIFY(recorder.inventory()->update(QLatin1String(kFleetBench[0].serial), [](InventoryRecord& r) { r.profile = QStringLiteral("node"); }));
        }
        QSignalSpy finished(job.get(), &FirmwareJob::operationFinished);
        job->flashFleet();
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), qsizetype(1), kOpTimeoutMs);
        QVERIFY(!finished.first().at(1).toBool());   // one board skipped: not every board flashed
        const QList<FleetBoard> boards = job->fleetBoards();
        QCOMPARE(boards.size(), qsizetype(3));
        for (int k = 0; k < 2; ++k) {
            const QString serial = QLatin1String(kFleetBench[k].serial);
            QCOMPARE(boards.at(k).result, FleetBoard::Result::Done);
            const std::optional<InventoryRecord> record = recordAt(file, serial);
            QVERIFY2(record, qPrintable(serial));
            QCOMPARE(record->port, QLatin1String(kFleetBench[k].port));
            QCOMPARE(record->layout, kNodeMtdParts);
            QCOMPARE(record->profile, withOverride && k == 1 ? QString() : QStringLiteral("node"));
            QCOMPARE(record->lastOperation.name, QStringLiteral("fleet-flash"));
            QCOMPARE(record->lastOperation.result, QStringLiteral("done"));
            QCOMPARE(record->lastOperation.message, boards.at(k).detail);
            QCOMPARE(record->lastImage.md5, Inventory::md5Hex(image));
            QCOMPARE(record->lastImage.partitions, boards.at(k).written);
            QCOMPARE(record->lastImage.partitions, QStringList{QStringLiteral("rootfs")});
            QVERIFY(record->lastSeen.isValid());
        }
        const QString absent = QLatin1String(kFleetBench[2].serial);
        QCOMPARE(boards.at(2).result, FleetBoard::Result::Skipped);
        const std::optional<InventoryRecord> record = recordAt(file, absent);
        QVERIFY(record);
        QCOMPARE(record->lastOperation.name, QStringLiteral("fleet-flash"));
        QCOMPARE(record->lastOperation.result, QStringLiteral("skipped"));
        QVERIFY2(record->lastOperation.message.startsWith(QStringLiteral("not on ADB: ")), qPrintable(record->lastOperation.message));
        QVERIFY(!record->lastImage.isValid() && record->lastImage.file.isEmpty());
        QVERIFY(record->profile.isEmpty());
        QVERIFY(!record->lastSeen.isValid());   // never seen: absent from adb devices
    }
}

void Tst_inventory::recorderDryRunRecordsSeenOnly()
{
    FakeToolRunner runner;
    FakeUsbBus bus;
    for (int k = 0; k < 3; ++k) {
        bus.addBoard(QLatin1String(kFleetBench[k].serial), QLatin1String(kFleetBench[k].port),
                     k == 2 ? FakeUsbBus::State::Absent : FakeUsbBus::State::Adb);
    }
    runner.addHandler(exfPattern(), &nodeExtractor);
    bus.script(runner);
    auto job = makeJob(&runner);
    job->fleetFlasher()->setUsbEnumerator([&bus](quint16 vid) { return bus.enumerate(vid); });
    job->fleetFlasher()->setPollIntervalMs(10);
    // The port remembered for A only.
    const QString remembered = QLatin1String(kFleetBench[0].port);
    const QString a = QLatin1String(kFleetBench[0].serial);
    job->setPortProvider([a, remembered](const QString& serial) { return serial == a ? remembered : QString(); });
    QVERIFY(job->setImagePath(withEnvImage()));
    QVERIFY(waitForRows(*job, kNodePartitionCount));
    job->setSelectedPartitions({QStringLiteral("rootfs")});
    FirmwareJob::FleetOptions options;
    for (int k = 0; k < 3; ++k) {
        options.boards.append({QLatin1String(kFleetBench[k].tag), QLatin1String(kFleetBench[k].serial)});
    }
    options.dryRun = true;
    job->setFleetOptions(options);
    InventoryRecorder recorder(job.get());
    recorder.setFilePath(path());
    recorder.setUsbEnumerator([&bus](quint16 vid) { return bus.enumerate(vid); });
    QSignalSpy finished(job.get(), &FirmwareJob::operationFinished);
    job->flashFleet();
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), qsizetype(1), kOpTimeoutMs);
    QCOMPARE(job->fleetBoards().at(0).result, FleetBoard::Result::Ready);
    for (int k = 0; k < 2; ++k) {
        const std::optional<InventoryRecord> record = recordAt(path(), QLatin1String(kFleetBench[k].serial));
        QVERIFY(record);
        QVERIFY(record->lastSeen.isValid());
        QCOMPARE(record->port, k == 0 ? remembered : QString());
        QVERIFY(record->lastOperation.name.isEmpty());
        QVERIFY(record->lastOperation.result.isEmpty());
        QVERIFY(!record->lastImage.isValid());
    }
    QVERIFY(!recordAt(path(), QLatin1String(kFleetBench[2].serial)));   // not there, not touched: nothing
}

void Tst_inventory::recorderFailedWriteLogsAndContinues()
{
    const QString blocker = caseDir(QStringLiteral("a-file"));
    QVERIFY(writeFile(blocker, "not a folder"));
    FakeToolRunner runner;
    scriptAdbFlash(runner);
    auto job = makeJob(&runner);
    QVERIFY(job->setImagePath(withEnvImage()));
    QVERIFY(waitForRows(*job, kNodePartitionCount));
    job->setSelectedPartitions({QStringLiteral("rootfs")});
    InventoryRecorder recorder(job.get());
    recorder.setFilePath(blocker + QStringLiteral("/inventory.json"));
    recorder.setUsbEnumerator(&oneBusAddressGadget);
    Lines seen(&recorder);
    QSignalSpy finished(job.get(), &FirmwareJob::operationFinished);
    job->flashSoc();
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), qsizetype(1), kOpTimeoutMs);
    QVERIFY2(finished.first().at(1).toBool(), qPrintable(finished.first().at(2).toString()));   // the flash is not failed by it
    QVERIFY(!seen.failures.isEmpty());
    QVERIFY(seen.events.isEmpty());
    bool line = false;
    for (const QString& text : std::as_const(seen.lines)) {
        line = line || text.startsWith(QStringLiteral("inventory: cannot write %1: ").arg(QDir::toNativeSeparators(recorder.filePath())));
    }
    QVERIFY2(line, qPrintable(seen.lines.join(QLatin1Char('\n'))));
    QVERIFY(QFileInfo(blocker).isFile());
}

void Tst_inventory::recorderDisabledWritesNothing()
{
    FakeToolRunner runner;
    scriptAdbFlash(runner);
    auto job = makeJob(&runner);
    QVERIFY(job->setImagePath(withEnvImage()));
    QVERIFY(waitForRows(*job, kNodePartitionCount));
    InventoryRecorder recorder(job.get());
    recorder.setFilePath(path());
    recorder.setUsbEnumerator(&oneBusAddressGadget);
    QVERIFY(recorder.isEnabled());
    recorder.setEnabled(false);
    QVERIFY(!recorder.isEnabled());
    Lines seen(&recorder);
    QSignalSpy finished(job.get(), &FirmwareJob::operationFinished);
    job->flashSoc();
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), qsizetype(1), kOpTimeoutMs);
    QVERIFY(finished.first().at(1).toBool());
    QTRY_VERIFY(runner.liveInvocations().isEmpty());
    QVERIFY(!QFileInfo::exists(path()));
    QVERIFY(seen.lines.isEmpty());
    QVERIFY(seen.events.isEmpty());
    QCOMPARE(recorder.lastSerial(), kSerial);   // it still listens
}

void Tst_inventory::recorderMovedAsideIsLogged()
{
    QVERIFY(writeFile(path(), "not json at all"));
    FakeToolRunner runner;
    scriptAdbBoard(runner);
    auto job = makeJob(&runner);
    InventoryRecorder recorder(job.get());
    recorder.setFilePath(path());
    recorder.setUsbEnumerator(&oneBusAddressGadget);
    Lines seen(&recorder);
    QSignalSpy layoutDone(job.get(), &FirmwareJob::layoutReadFinished);
    job->refreshStatus();
    QTRY_COMPARE_WITH_TIMEOUT(layoutDone.count(), qsizetype(1), kOpTimeoutMs);
    QVERIFY(!seen.lines.isEmpty());
    const QRegularExpression moved(QStringLiteral("^inventory: %1 was unreadable and moved to inventory\\.json\\.corrupt-\\d{8}-\\d{6}(-\\d+)?$")
                                       .arg(QRegularExpression::escape(QDir::toNativeSeparators(path()))));
    QVERIFY2(moved.match(seen.lines.first()).hasMatch(), qPrintable(seen.lines.join(QLatin1Char('\n'))));
    QCOMPARE(seen.lines.at(1), QStringLiteral("inventory: board %1 recorded (status)").arg(kSerial));
    const QStringList aside = QDir(m_dir->path()).entryList({QStringLiteral("inventory.json.corrupt-*")}, QDir::Files);
    QCOMPARE(aside.size(), qsizetype(1));
    QCOMPARE(readBytes(m_dir->filePath(aside.first())), QByteArray("not json at all"));
    QVERIFY(recordAt(path(), kSerial));
}

void Tst_inventory::recorderNoSecretInFile()
{
    SshProfileStore store;
    BuildMachine machine;
    machine.passwordAuth = true;
    QVERIFY(machine.start(&store, caseDir(QStringLiteral("work"))));
    FakeMcu mcu;
    FakeMcuServer bridge(&mcu);
    QVERIFY(bridge.listen());
    FakeToolRunner runner;
    scriptAdbFlash(runner);
    runner.addScript(QRegularExpression(QStringLiteral("rfmcu cmd version")), "OK version=0.8.1 role=rx\n");
    scriptMcuBridge(runner, bridge.port());
    auto job = makeJob(&runner, &store);
    const QString cache = caseDir(QStringLiteral("cache"));
    job->setSourceTarget(QStringLiteral("ssh:profile:") + machine.profile.id);
    job->setRemoteFolder(machine.imageFolder);
    job->setCacheDirectory(cache);
    QVERIFY(job->setPackagePath(dataFile("firmware-0.8.1.pkg")));
    const QString password = machine.password;
    job->setAuthPromptHandler([password](const SshConnection::AuthPrompt&) -> std::optional<FirmwareJob::AuthAnswer> {
        return FirmwareJob::AuthAnswer{password, false};
    });
    InventoryRecorder recorder(job.get());
    recorder.setFilePath(path());
    recorder.setUsbEnumerator(&oneBusAddressGadget);
    QSignalSpy finished(job.get(), &FirmwareJob::operationFinished);
    job->pullAndFlash();
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), qsizetype(1), kChainTimeoutMs);
    QVERIFY2(finished.first().at(1).toBool(), qPrintable(finished.first().at(2).toString()));
    QCOMPARE(machine.server->lastAuthMethod(), QStringLiteral("password"));
    const QString text = QString::fromUtf8(readBytes(path()));
    QVERIFY(text.contains(kSerial));
    QVERIFY(text.contains(QStringLiteral("\"update.img\"")));
    for (const QString& secret : {password, machine.target(), QStringLiteral("127.0.0.1"), machine.imageFolder, cache,
                                  QDir::toNativeSeparators(cache), m_dir->path(), QDir::toNativeSeparators(m_dir->path()),
                                  QStringLiteral(":/"), QStringLiteral(":\\\\")}) {
        QVERIFY2(!text.contains(secret, Qt::CaseInsensitive), qPrintable(secret));
    }
}

// QTEST_GUILESS_MAIN's body, plus (fix4, m5) the second process of writersInTwoProcesses(): started
// with --inventory-writer it writes and exits, no test runs in it (tst_firmwaretools' --tool-helper
// does the same).
int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    // (v0.8.0) ctest pipes stdout, which the C runtime then buffers: a crash on a CI runner would
    // swallow every line this suite printed. Unbuffered, the last line always reaches the log.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const QStringList args = QCoreApplication::arguments();
    if (args.size() >= 2 && args.at(1) == kWriterOption) {
        return runInventoryWriter(args);
    }
    app.setAttribute(Qt::AA_Use96Dpi, true);
    Tst_inventory tc;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_inventory.moc"
