#include "firmware/InventoryRecorder.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QHash>
#include <QPair>
#include <QPointer>

#include <optional>
#include <utility>

#include "app/Logging.h"
#include "firmware/FirmwareJob.h"
#include "firmware/Inventory.h"
#include "firmware/RockchipTool.h"
#include "firmware/SocFlasher.h"

namespace {

QString nativePath(const QString& path)
{
    return QDir::toNativeSeparators(path);
}

/// The record's form of a layout: no "mtdparts=" prefix, no blanks around it.
QString normalizedLayout(const QString& mtdparts)
{
    const QLatin1String prefix("mtdparts=");
    QString text = mtdparts.trimmed();
    if (text.startsWith(prefix)) {
        text = text.mid(prefix.size()).trimmed();
    }
    return text;
}

/// "done" / "cancelled" / "failed" of a single-board step or a chain (Inventory.h, Operation).
QString resultText(bool ok, const QString& message)
{
    if (ok) {
        return QStringLiteral("done");
    }
    const QString text = message.trimmed();
    // The flasher's / job's "cancelled", su-fw's "interrupted ..." / "timeout after ...", and the
    // chain's "stopped at <step>: cancelled".
    if (text == QLatin1String("cancelled") || text.startsWith(QLatin1String("interrupted"))
        || text.startsWith(QLatin1String("timeout after")) || text.endsWith(QLatin1String(": cancelled"))) {
        return QStringLiteral("cancelled");
    }
    return QStringLiteral("failed");
}

/// The rows of `fresh` merged into `record` when the md5 is the same (first appearance order, no
/// duplicates, `written` refreshed); a different md5 replaces the image (Inventory.h).
void mergeImage(InventoryRecord::Image& record, const InventoryRecord::Image& fresh)
{
    if (!fresh.md5.isEmpty() && record.md5.compare(fresh.md5, Qt::CaseInsensitive) == 0) {
        for (const QString& name : fresh.partitions) {
            if (!record.partitions.contains(name)) {
                record.partitions << name;
            }
        }
        record.file = fresh.file;
        record.chipTag = fresh.chipTag;
        record.gitDescribe = fresh.gitDescribe;
        record.written = fresh.written;
        return;
    }
    record = fresh;
}

} // namespace

struct InventoryRecorder::Private
{
    explicit Private(InventoryRecorder* owner) : q(owner) {}

    InventoryRecorder* q;
    QPointer<FirmwareJob> job;
    std::unique_ptr<Inventory> inventory = std::make_unique<Inventory>();
    UsbProbe::Enumerator enumerator = [](quint16 vid) { return UsbProbe::presentDevices(vid); };
    bool enabled = true;
    QString lastSerial;
    bool statusSeen = false;
    SocFlasher::DeviceStatus::Mode lastMode = SocFlasher::DeviceStatus::Mode::None;
    bool loaderLineLogged = false;              ///< per flow: reset by every statusChanged()
    QHash<QString, QPair<QDateTime, QString>> md5Cache;   ///< image path -> (mtime, md5), for the job's life
    bool loaded = false;                        ///< records() came from a load() / update() ...
    QElapsedTimer sinceRefresh;                 ///< ... this long ago
    bool failedRecently = false;                ///< a write failed: status writes wait for kSeenRefreshSeconds
    QElapsedTimer sinceFailure;

    // ---- the slots ----
    void onStatus(const SocFlasher::DeviceStatus& status);
    void onProfile(const ProfileDetection& profile);
    void onLayout(const QString& serial, const QString& mtdparts);
    void onPort(const QString& serial, const QString& port);
    void onMcuInfo();
    void onStep(const QString& step, bool ok, const QString& message);
    void onOperation(const QString& op, bool ok, const QString& message);
    void onBoard(const FleetBoard& board);

    // ---- helpers ----
    void refreshRecords();
    std::optional<InventoryRecord> current(const QString& serial);
    InventoryRecord::Tri probeGadget(const QString& serial) const;
    QString md5Of(const QString& path);
    InventoryRecord::Image imageOf(const QStringList& rows, const QDateTime& when);
    QStringList flashedRows() const;
    void loaderLine();
    void stepWithoutBoard(const char* what);
    bool write(const QString& serial, const QString& event, const Inventory::Change& change);
};

// =======================================================================================
// Helpers
// =======================================================================================

void InventoryRecorder::Private::refreshRecords()
{
    // Fresh from the last update(); another writer's records (su-fw next to the tab) are picked up
    // at most every kSeenRefreshSeconds - a status every 2 s must not read the file every 2 s.
    if (loaded && sinceRefresh.isValid() && sinceRefresh.elapsed() < qint64(kSeenRefreshSeconds) * 1000) {
        return;
    }
    QString error;
    if (!inventory->load(&error)) {
        // A corrupt file: no records now; the next write moves it aside and says so.
        qCWarning(lcFirmware).noquote() << "inventory:" << error;
    }
    loaded = true;
    sinceRefresh.start();
}

std::optional<InventoryRecord> InventoryRecorder::Private::current(const QString& serial)
{
    refreshRecords();
    return inventory->record(serial);
}

InventoryRecord::Tri InventoryRecorder::Private::probeGadget(const QString& serial) const
{
    if (!enumerator) {
        return InventoryRecord::Tri::Unknown;
    }
    QList<UsbDeviceInfo> gadgets;
    for (const UsbDeviceInfo& device : enumerator(UsbProbe::kRockchipVid)) {
        if (device.isAdbGadget()) {
            gadgets << device;
        }
    }
    if (gadgets.size() != 1) {
        return InventoryRecord::Tri::Unknown;   // several boards (or none): the mapping would be a guess
    }
    const UsbDeviceInfo& gadget = gadgets.first();
    const QString usbSerial = gadget.serial();
    if (!usbSerial.isEmpty()) {
        // Windows prints the serial in upper case; adb in the board's own case.
        return usbSerial.compare(serial, Qt::CaseInsensitive) == 0 ? InventoryRecord::Tri::Yes : InventoryRecord::Tri::Unknown;
    }
    const QString last = gadget.instanceId.section(QLatin1Char('\\'), -1);
    return last.contains(QLatin1Char('&')) ? InventoryRecord::Tri::No : InventoryRecord::Tri::Unknown;   // a bus address
}

QString InventoryRecorder::Private::md5Of(const QString& path)
{
    const QDateTime modified = QFileInfo(path).lastModified();
    const auto it = md5Cache.constFind(path);
    if (it != md5Cache.constEnd() && it.value().first == modified) {
        return it.value().second;
    }
    QString error;
    const QString md5 = Inventory::md5Hex(path, &error);
    if (md5.isEmpty()) {
        qCWarning(lcFirmware).noquote() << "inventory: no md5 for the image:" << error;
        return QString();
    }
    md5Cache.insert(path, qMakePair(modified, md5));
    return md5;
}

InventoryRecord::Image InventoryRecorder::Private::imageOf(const QStringList& rows, const QDateTime& when)
{
    InventoryRecord::Image image;
    const QString path = job ? job->imagePath() : QString();
    if (path.isEmpty()) {
        return image;
    }
    image.file = QFileInfo(path).fileName();   // never the folder: the file is meant to be pasted
    const RkfwHeader header = RockchipTool::readRkfwHeader(path);
    image.chipTag = header.valid ? header.chipTag : QString();
    image.md5 = md5Of(path);
    if (job->localImage().isEmpty()) {
        image.gitDescribe = job->cacheEntry().gitDescribe;   // the cache's image: its note says the build
    }
    image.partitions = rows;
    image.written = when;
    return image;
}

QStringList InventoryRecorder::Private::flashedRows() const
{
    QStringList rows;
    SocFlasher* flasher = job->socFlasher();
    if (flasher->flashMethod() == SocFlasher::FlashMethod::UpgradeFirmware) {
        // UF writes the whole image: every row of it.
        for (const PartitionImage& row : job->partitions()) {
            rows << row.name;
        }
        if (rows.isEmpty()) {
            for (const PartitionImage& row : flasher->partitions()) {
                rows << row.name;
            }
        }
        return rows;
    }
    // (fix4, m4) The rows the flasher wrote completely, not the selection: a row the flow skipped
    // (no image file for it in the package, not in the layout) was not written, and a record that
    // claimed it would make --expect-image skip a board that does not run that row.
    return flasher->writtenPartitions();
}

void InventoryRecorder::Private::loaderLine()
{
    if (!enabled || loaderLineLogged) {
        return;
    }
    loaderLineLogged = true;
    qCInfo(lcFirmware) << "inventory: a board in Loader mode has no serial; nothing recorded";
    emit q->logLine(InventoryRecorder::tr("inventory: the board in Loader mode has no serial; not recorded"));
}

void InventoryRecorder::Private::stepWithoutBoard(const char* what)
{
    // The board the job met has no serial: in Loader / MASKROM mode that is said once per flow; with
    // no board at all (or several on ADB) nothing was touched that could be named.
    if (statusSeen && (lastMode == SocFlasher::DeviceStatus::Mode::Loader || lastMode == SocFlasher::DeviceStatus::Mode::Maskrom)) {
        loaderLine();
        return;
    }
    qCInfo(lcFirmware) << "inventory:" << what << "ended without a board on ADB; nothing recorded";
}

bool InventoryRecorder::Private::write(const QString& serial, const QString& event, const Inventory::Change& change)
{
    if (!enabled) {
        return false;
    }
    QString error;
    if (!inventory->update(serial, change, &error)) {
        failedRecently = true;
        sinceFailure.start();
        qCWarning(lcFirmware).noquote() << "inventory: cannot write" << nativePath(inventory->filePath()) << "-" << error;
        emit q->failed(error);
        emit q->logLine(InventoryRecorder::tr("inventory: cannot write %1: %2").arg(nativePath(inventory->filePath()), error));
        return false;
    }
    failedRecently = false;
    loaded = true;
    sinceRefresh.start();
    const QString moved = inventory->movedAsideFile();
    if (!moved.isEmpty()) {
        emit q->logLine(InventoryRecorder::tr("inventory: %1 was unreadable and moved to %2")
                            .arg(nativePath(inventory->filePath()), QFileInfo(moved).fileName()));
    }
    qCInfo(lcFirmware).noquote() << "inventory write:" << serial << event;
    emit q->recorded(serial, event);
    emit q->logLine(InventoryRecorder::tr("inventory: board %1 recorded (%2)").arg(serial, event));
    return true;
}

// =======================================================================================
// The slots
// =======================================================================================

void InventoryRecorder::Private::onStatus(const SocFlasher::DeviceStatus& status)
{
    statusSeen = true;
    lastMode = status.mode;
    loaderLineLogged = false;
    const QString serial = status.serial.trimmed();
    if (status.mode != SocFlasher::DeviceStatus::Mode::Adb || serial.isEmpty()) {
        // Not one ADB board (Loader, Maskrom, none, several): the board the job sees is no longer
        // the one it saw - nothing is guessed from the earlier status.
        lastSerial.clear();
        return;
    }
    lastSerial = serial;
    if (!enabled) {
        return;
    }
    if (failedRecently && sinceFailure.isValid() && sinceFailure.elapsed() < qint64(kSeenRefreshSeconds) * 1000) {
        return;   // a broken data folder must not log a failure every 2 s
    }
    const std::optional<InventoryRecord> record = current(serial);
    const QDateTime now = QDateTime::currentDateTime();
    if (record && record->lastSeen.isValid()) {
        const qint64 age = record->lastSeen.secsTo(now);
        if (age >= 0 && age < kSeenRefreshSeconds) {
            return;   // nothing but lastSeen would change: once a minute is enough
        }
    }
    // The enumeration only while the answer is unknown (SetupDi, on the GUI thread in the tab).
    InventoryRecord::Tri gadget = InventoryRecord::Tri::Unknown;
    if (!record || record->gadgetSerialString == InventoryRecord::Tri::Unknown) {
        gadget = probeGadget(serial);
    }
    write(serial, QStringLiteral("status"), [gadget](InventoryRecord& r) {
        if (gadget != InventoryRecord::Tri::Unknown && r.gadgetSerialString == InventoryRecord::Tri::Unknown) {
            r.gadgetSerialString = gadget;
        }
    });
}

void InventoryRecorder::Private::onProfile(const ProfileDetection& profile)
{
    if (!enabled || !profile.known() || lastSerial.isEmpty()) {
        return;
    }
    // The board's own evidence only: an override (--profile, the tab's box) is the operator's claim.
    if (profile.source != ProfileDetection::Source::LoaderPid && profile.source != ProfileDetection::Source::AdbProduct
        && profile.source != ProfileDetection::Source::BoardLayout) {
        return;
    }
    const QString id = profile.profile.id;
    const std::optional<InventoryRecord> record = current(lastSerial);
    if (record && record->profile == id) {
        return;
    }
    write(lastSerial, QStringLiteral("profile"), [id](InventoryRecord& r) { r.profile = id; });
}

void InventoryRecorder::Private::onLayout(const QString& serial, const QString& mtdparts)
{
    const QString key = serial.trimmed();
    if (key.isEmpty()) {
        loaderLine();   // a layout read in Loader mode belongs to no serial
        return;
    }
    const QString layout = normalizedLayout(mtdparts);
    if (layout.isEmpty()) {
        return;
    }
    write(key, QStringLiteral("layout"), [layout](InventoryRecord& r) { r.layout = layout; });
}

void InventoryRecorder::Private::onPort(const QString& serial, const QString& port)
{
    const QString key = serial.trimmed();
    if (key.isEmpty()) {
        loaderLine();
        return;
    }
    const QString value = port.trimmed();
    if (value.isEmpty()) {
        return;
    }
    write(key, QStringLiteral("port"), [value](InventoryRecord& r) { r.port = value; });
}

void InventoryRecorder::Private::onMcuInfo()
{
    if (!job) {
        return;
    }
    const QString version = job->runningMcuVersion().trimmed();
    const QString role = job->runningMcuRole().trimmed();
    if (version.isEmpty() && role.isEmpty()) {
        return;
    }
    if (lastSerial.isEmpty()) {
        stepWithoutBoard("an MCU read");
        return;
    }
    const QDateTime now = QDateTime::currentDateTime();
    write(lastSerial, QStringLiteral("mcu"), [version, role, now](InventoryRecord& r) {
        if (!version.isEmpty()) {
            r.mcuVersion = version;
        }
        if (!role.isEmpty()) {
            r.mcuRole = role;
        }
        r.mcuRead = now;
    });
}

void InventoryRecorder::Private::onStep(const QString& step, bool ok, const QString& message)
{
    // The single-board steps that touch a board; a pull has none, a fleet reports per board.
    if (step != FirmwareJob::kOpFlash && step != FirmwareJob::kOpMcu && step != FirmwareJob::kOpSetRole
        && step != FirmwareJob::kOpInfo) {
        return;
    }
    if (!job) {
        return;
    }
    if (lastSerial.isEmpty()) {
        stepWithoutBoard("a step");
        return;
    }
    const QDateTime now = QDateTime::currentDateTime();
    InventoryRecord::Operation operation;
    operation.name = step;
    operation.result = resultText(ok, message);
    operation.message = message;
    operation.time = now;
    // The image only after a write that ended ok: a failed or cancelled write leaves what the
    // board ran before (an agent's --expect-image must never skip a board whose write was cut).
    std::optional<InventoryRecord::Image> image;
    if (step == FirmwareJob::kOpFlash && ok) {
        image = imageOf(flashedRows(), now);
    }
    write(lastSerial, step, [operation, image](InventoryRecord& r) {
        r.lastOperation = operation;
        if (image) {
            mergeImage(r.lastImage, *image);
        }
    });
}

void InventoryRecorder::Private::onOperation(const QString& op, bool ok, const QString& message)
{
    if (op != FirmwareJob::kOpPullFlash) {
        return;   // the other operations are their single step, recorded by onStep()
    }
    if (lastSerial.isEmpty()) {
        stepWithoutBoard("pull-flash");
        return;
    }
    InventoryRecord::Operation operation;
    operation.name = op;
    operation.result = resultText(ok, message);
    operation.message = message;
    operation.time = QDateTime::currentDateTime();
    write(lastSerial, op, [operation](InventoryRecord& r) { r.lastOperation = operation; });
}

void InventoryRecorder::Private::onBoard(const FleetBoard& board)
{
    const QString serial = board.serial.trimmed();
    if (serial.isEmpty() || !job || board.result == FleetBoard::Result::Pending) {
        return;
    }
    const QString event = QString(FirmwareJob::kOpFleetFlash);
    if (board.result == FleetBoard::Result::Ready || job->fleetOptions().dryRun) {
        // A dry run touched nothing: an attached board was seen (and where, when that is known);
        // no operation, no image, and a board that is not there is not recorded at all.
        if (board.result != FleetBoard::Result::Ready) {
            return;
        }
        const QString port = !board.port.isEmpty() ? board.port : board.expectedPort;
        write(serial, event, [port](InventoryRecord& r) {
            if (!port.isEmpty()) {
                r.port = port;
            }
        });
        return;
    }
    const QDateTime now = QDateTime::currentDateTime();
    // A board adb did not list (absent) was not seen by this run: its lastSeen stays.
    const bool attached = !board.adbState.isEmpty();
    QDateTime previousSeen;
    if (!attached) {
        if (const std::optional<InventoryRecord> record = current(serial)) {
            previousSeen = record->lastSeen;
        }
    }
    InventoryRecord::Operation operation;
    operation.name = event;
    operation.result = board.resultText();
    operation.message = board.detail;
    operation.time = now;
    std::optional<InventoryRecord::Image> image;
    if (board.result == FleetBoard::Result::Done || board.result == FleetBoard::Result::NotBack) {
        image = imageOf(board.written, now);
    }
    // The fleet fills FleetBoard::profile from the override too: inventoried only without one.
    const QString profile = job->profileOverride().isEmpty() ? board.profile.trimmed() : QString();
    const QString layout = normalizedLayout(board.layout);
    const QString port = board.port.trimmed();
    write(serial, event, [port, layout, profile, attached, previousSeen, operation, image](InventoryRecord& r) {
        if (!port.isEmpty()) {
            r.port = port;
        }
        if (!layout.isEmpty()) {
            r.layout = layout;
        }
        if (!profile.isEmpty()) {
            r.profile = profile;
        }
        if (!attached) {
            r.lastSeen = previousSeen;
        }
        r.lastOperation = operation;
        if (image) {
            mergeImage(r.lastImage, *image);
        }
    });
}

// =======================================================================================
// InventoryRecorder
// =======================================================================================

InventoryRecorder::InventoryRecorder(FirmwareJob* job, QObject* parent)
    : QObject(parent)
    , d(std::make_unique<Private>(this))
{
    d->job = job;
    if (!job) {
        return;
    }
    connect(job, &FirmwareJob::statusChanged, this, [this](const SocFlasher::DeviceStatus& status) { d->onStatus(status); });
    connect(job, &FirmwareJob::boardProfileChanged, this, [this](const ProfileDetection& profile) { d->onProfile(profile); });
    connect(job, &FirmwareJob::mtdPartsRead, this,
            [this](const QString& serial, const QString& mtdparts) { d->onLayout(serial, mtdparts); });
    connect(job, &FirmwareJob::boardPortLearned, this, [this](const QString& serial, const QString& port) { d->onPort(serial, port); });
    connect(job, &FirmwareJob::mcuInfoChanged, this, [this]() { d->onMcuInfo(); });
    connect(job, &FirmwareJob::stepFinished, this,
            [this](const QString& step, bool ok, const QString& message) { d->onStep(step, ok, message); });
    connect(job, &FirmwareJob::operationFinished, this,
            [this](const QString& op, bool ok, const QString& message) { d->onOperation(op, ok, message); });
    connect(job, &FirmwareJob::boardFinished, this, [this](const FleetBoard& board) { d->onBoard(board); });
}

InventoryRecorder::~InventoryRecorder() = default;

void InventoryRecorder::setFilePath(const QString& path)
{
    d->inventory = std::make_unique<Inventory>(path);
    d->loaded = false;
    d->failedRecently = false;
}

QString InventoryRecorder::filePath() const
{
    return d->inventory->filePath();
}

Inventory* InventoryRecorder::inventory() const
{
    return d->inventory.get();
}

void InventoryRecorder::setEnabled(bool on)
{
    d->enabled = on;
}

bool InventoryRecorder::isEnabled() const
{
    return d->enabled;
}

QString InventoryRecorder::lastSerial() const
{
    return d->lastSerial;
}

void InventoryRecorder::setUsbEnumerator(UsbProbe::Enumerator enumerator)
{
    d->enumerator = std::move(enumerator);
}
