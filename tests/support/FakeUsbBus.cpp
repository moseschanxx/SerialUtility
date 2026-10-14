#include "support/FakeUsbBus.h"

#include <QDir>
#include <QFileInfo>
#include <QMap>
#include <QPointer>
#include <QRegularExpression>
#include <QTimer>

#include <algorithm>

#include "firmware/ToolRunner.h"
#include "support/FakeToolRunner.h"
#include "support/FirmwareFixtures.h"

namespace {

constexpr quint16 kVid = 0x2207;
constexpr quint16 kAdbPid = 0x0006;
constexpr quint16 kLoaderPid = 0x110e;
constexpr quint16 kMaskromPid = 0x110c;
constexpr int kCommandMs = 5;   ///< how long an RD or a refused command takes

UsbDeviceInfo instance(quint16 pid, const QString& port)
{
    UsbDeviceInfo info;
    info.vid = kVid;
    info.pid = pid;
    // The port key is the instance id's last segment exactly as printed: the port keeps its case.
    info.instanceId = QStringLiteral("USB\\VID_2207&PID_") + QString::number(pid, 16).rightJustified(4, QLatin1Char('0')).toUpper()
                      + QLatin1Char('\\') + port;
    info.description = pid == kAdbPid ? QStringLiteral("Android ADB Interface") : QStringLiteral("Rockusb Device");
    info.driverClass = pid == kAdbPid ? QStringLiteral("AndroidUsbDeviceClass") : QStringLiteral("USB");
    return info;
}

} // namespace

struct FakeUsbBus::Private
{
    struct Entry
    {
        QString serial;
        QString port;
        QString otherPort;
        QString mtdParts;
        Reboot reboot = Reboot::ToLoader;
        State state = State::Adb;
        bool adbInstance = true;      ///< the enumeration shows the ADB gadget on `port`
        bool adbListed = true;        ///< `adb devices` lists the serial
        QString adbState = QStringLiteral("device");
        bool dlInstance = false;      ///< the enumeration shows a download-mode instance on dlPort
        QString dlPort;
        quint16 dlPid = kLoaderPid;
        QString ldId;                 ///< "" = LD does not list it
        bool ldMaskrom = false;
        quint64 epoch = 0;            ///< bumped by every state change: a delayed step of an older one is dropped
        bool noAdbReturn = false;
        bool leaveBeforeWrite = false;
        int leaveAfterRows = -1;
        int rowsDone = 0;
        bool hang = false;
        QMap<QString, QPair<QByteArray, int>> writeFailures;   ///< file name -> output, exit code
        bool resetFails = false;
        QByteArray resetOutput;
        int resetExit = 0;
        QStringList writes;
        QStringList resets;
    };
    struct Foreign
    {
        QString port;
        QString id;
    };
    struct Hung
    {
        QPointer<FakeToolInvocation> invocation;
        QString serial;
    };

    explicit Private(FakeUsbBus* owner) : q(owner) {}

    FakeUsbBus* q;
    QList<Entry> boards;
    QList<Foreign> foreignLoaders;
    QStringList phones;
    QMap<QString, QString> portIds;   ///< per-location LocationIDs, like upgrade_tool's
    int nextId = 100;
    int rebootDelayMs = 30;
    int pnpLagMs = 10;
    int resetDelayMs = 30;
    int ldDurationMs = 0;
    int writeDurationMs = 40;
    int writesOrResetsInFlight = 0;
    int ldsInFlight = 0;
    int wlInFlight = 0;
    int maxWl = 0;
    int overlaps = 0;
    int unaddressed = 0;
    int cancelled = 0;
    QList<Hung> hung;

    Entry* find(const QString& serial)
    {
        for (Entry& entry : boards) {
            if (entry.serial == serial) {
                return &entry;
            }
        }
        return nullptr;
    }

    Entry* byId(const QString& id)
    {
        for (Entry& entry : boards) {
            if (!id.isEmpty() && entry.ldId == id) {
                return &entry;
            }
        }
        return nullptr;
    }

    QString idFor(const QString& port)
    {
        if (!portIds.contains(port)) {
            portIds.insert(port, QString::number(nextId++));
        }
        return portIds.value(port);
    }

    void touched(Entry& entry)
    {
        ++entry.epoch;
        emit q->changed();
    }

    void toAdb(Entry& entry)
    {
        entry.state = State::Adb;
        entry.adbInstance = true;
        entry.adbListed = true;
        entry.adbState = QStringLiteral("device");
        entry.dlInstance = false;
        entry.ldId.clear();
        touched(entry);
    }

    void toAbsent(Entry& entry)
    {
        entry.state = State::Absent;
        entry.adbInstance = false;
        entry.adbListed = false;
        entry.dlInstance = false;
        entry.ldId.clear();
        touched(entry);
    }

    /// Download mode on `port`: LD lists it (when `listed`) at once, the enumeration after `lagMs`.
    void toDownload(Entry& entry, const QString& port, bool maskrom, bool listed, int lagMs)
    {
        entry.state = maskrom ? State::Maskrom : State::Loader;
        entry.adbInstance = false;
        entry.adbListed = false;
        entry.ldId = listed ? idFor(port) : QString();
        entry.ldMaskrom = maskrom;
        entry.dlPort = port;
        entry.dlPid = maskrom ? kMaskromPid : kLoaderPid;
        entry.dlInstance = false;
        touched(entry);
        enumerateLater(entry.serial, lagMs);
    }

    void enumerateLater(const QString& serial, int lagMs)
    {
        Entry* entry = find(serial);
        if (lagMs <= 0) {
            entry->dlInstance = true;
            emit q->changed();
            return;
        }
        const quint64 epoch = entry->epoch;
        QTimer::singleShot(lagMs, q, [this, serial, epoch] {
            Entry* e = find(serial);
            if (e && e->epoch == epoch) {
                e->dlInstance = true;
                emit q->changed();
            }
        });
    }

    void applyReboot(const QString& serial)
    {
        Entry* entry = find(serial);
        if (!entry) {
            return;
        }
        switch (entry->reboot) {
        case Reboot::StaysOnAdb:
            return;
        case Reboot::ToLoader:
            toDownload(*entry, entry->port, false, true, pnpLagMs);
            return;
        case Reboot::ToMaskrom:
            toDownload(*entry, entry->port, true, true, pnpLagMs);
            return;
        case Reboot::DragsNext: {
            const qsizetype index = entry - boards.data();
            toDownload(*entry, entry->port, false, true, pnpLagMs);
            for (qsizetype i = index + 1; i < boards.size(); ++i) {
                if (boards[i].state == State::Adb) {
                    toDownload(boards[i], boards[i].port, false, true, pnpLagMs);
                    break;
                }
            }
            return;
        }
        case Reboot::GhostLoader:
            entry->dlPort = entry->port;
            entry->dlPid = kLoaderPid;
            touched(*entry);
            enumerateLater(serial, pnpLagMs);   // the ADB instance stays, LD lists nothing
            return;
        case Reboot::NoLdLine:
            toDownload(*entry, entry->port, false, false, pnpLagMs);
            return;
        case Reboot::OtherPort:
            toDownload(*entry, entry->otherPort, false, true, pnpLagMs);
            return;
        case Reboot::AdbKeepsListing:
            toDownload(*entry, entry->port, false, true, pnpLagMs);
            entry->adbListed = true;   // adb's server holds a stale entry
            return;
        case Reboot::LdLineOnly:
            entry->ldId = idFor(entry->port);
            entry->ldMaskrom = false;
            touched(*entry);
            return;
        case Reboot::Vanishes:
            toAbsent(*entry);
            return;
        }
    }

    // ---- outputs -------------------------------------------------------------------------------

    QByteArray ldText() const
    {
        QList<QPair<QString, bool>> listed;   // id, maskrom
        for (const Entry& entry : boards) {
            if (!entry.ldId.isEmpty()) {
                listed.append({entry.ldId, entry.ldMaskrom});
            }
        }
        for (const Foreign& device : foreignLoaders) {
            listed.append({device.id, false});
        }
        QByteArray text = "rockusb driver = 5.12,please upgrade it to enjoy fast download\r\n\r\n";
        text += "List of rockusb connected(" + QByteArray::number(listed.size()) + ")\r\n";
        int devNo = 1;
        for (const auto& [id, maskrom] : listed) {
            text += "DevNo=" + QByteArray::number(devNo++) + "\tVid=0x2207,Pid=" + (maskrom ? "0x110c" : "0x110e")
                    + ",LocationID=" + id.toLatin1() + "\tMode=" + (maskrom ? "Maskrom" : "Loader") + "\tSerialNo=\r\n";
        }
        return text;
    }

    QByteArray adbText() const
    {
        QByteArray text = "List of devices attached\r\n";
        for (const Entry& entry : boards) {
            if (entry.adbListed) {
                text += entry.serial.toLatin1() + '\t' + entry.adbState.toLatin1() + "\r\n";
            }
        }
        for (const QString& phone : phones) {
            text += phone.toLatin1() + "\tdevice\r\n";
        }
        return text + "\r\n";
    }

    // ---- handlers ------------------------------------------------------------------------------

    void onLd(FakeToolInvocation* i)
    {
        ++ldsInFlight;
        if (writesOrResetsInFlight > 0) {
            ++overlaps;
        }
        i->hang();
        const QPointer<FakeToolInvocation> guard(i);
        QTimer::singleShot(ldDurationMs, q, [this, guard] {
            --ldsInFlight;
            if (!guard) {
                return;
            }
            guard->emitOutput(ldText());   // the state at the END of the listing
            guard->finish(0);
        });
    }

    void countWriteOrReset(FakeToolInvocation* i, bool isWrite)
    {
        if (ldsInFlight > 0) {
            ++overlaps;
        }
        ++writesOrResetsInFlight;
        if (isWrite) {
            maxWl = qMax(maxWl, ++wlInFlight);
        }
        QObject::connect(i, &ToolInvocation::finished, q, [this, isWrite](const ToolResult& result) {
            --writesOrResetsInFlight;
            if (isWrite) {
                --wlInFlight;
            }
            if (result.cancelled) {
                ++cancelled;
            }
        });
    }

    void refuseUnknownId(FakeToolInvocation* i)
    {
        i->emitOutput("rockusb driver = 5.12,please upgrade it to enjoy fast download\r\n\r\nNo found specific rockusb device!\r\n",
                      kCommandMs);
        i->finish(255);
    }

    void onWl(FakeToolInvocation* i)
    {
        const QStringList args = i->arguments();   // -s ID WL start count file
        const QString id = args.value(1);
        Entry* entry = byId(id);
        const bool foreignId = std::any_of(foreignLoaders.cbegin(), foreignLoaders.cend(), [&id](const Foreign& f) { return f.id == id; });
        countWriteOrReset(i, true);
        if ((!entry || entry->ldMaskrom) && !foreignId) {
            refuseUnknownId(i);
            return;
        }
        if (!entry) {
            i->emitOutput("Write LBA from file (100%)\r\n", writeDurationMs);   // a foreign device: never the fleet's
            i->finish(0);
            return;
        }
        entry->writes << i->commandLine();
        const QString fileName = QFileInfo(QDir::fromNativeSeparators(args.value(5))).fileName();
        if (entry->writeFailures.contains(fileName)) {
            const auto failure = entry->writeFailures.value(fileName);
            i->emitOutput(failure.first, writeDurationMs);
            i->finish(failure.second);
            return;
        }
        if (entry->hang) {
            i->emitOutput("Write LBA from file (10%)\r");
            i->hang();
            hung.append({QPointer<FakeToolInvocation>(i), entry->serial});
            return;
        }
        const int step = qMax(1, writeDurationMs / 10);
        for (int percent = 10; percent < 100; percent += 10) {
            i->emitOutput("Write LBA from file (" + QByteArray::number(percent) + "%)\r", step);
        }
        const QPointer<FakeToolInvocation> guard(i);
        const QString serial = entry->serial;
        QTimer::singleShot(writeDurationMs, q, [this, guard, serial] { completeWrite(guard, serial); });
    }

    /// The last 10 % of a WL: the row counts as written, then the board may leave Loader mode.
    void completeWrite(const QPointer<FakeToolInvocation>& guard, const QString& serial)
    {
        if (!guard) {
            return;
        }
        if (Entry* entry = find(serial)) {
            ++entry->rowsDone;
            if (entry->leaveAfterRows >= 0 && entry->rowsDone == entry->leaveAfterRows) {
                entry->leaveAfterRows = -1;
                if (entry->noAdbReturn) {
                    toAbsent(*entry);
                } else {
                    toAdb(*entry);
                }
            }
        }
        guard->emitOutput("Write LBA from file (100%)\r\n");
        guard->finish(0);
    }

    void onRd(FakeToolInvocation* i)
    {
        const QString id = i->arguments().value(1);
        Entry* entry = byId(id);
        countWriteOrReset(i, false);
        for (qsizetype f = 0; f < foreignLoaders.size(); ++f) {
            if (foreignLoaders.at(f).id == id) {
                foreignLoaders.removeAt(f);
                emit q->changed();
                i->emitOutput("Reset Device OK.\r\n", kCommandMs);
                i->finish(0);
                return;
            }
        }
        if (!entry) {
            refuseUnknownId(i);
            return;
        }
        entry->resets << i->commandLine();
        if (entry->resetFails) {
            i->emitOutput(entry->resetOutput, kCommandMs);
            i->finish(entry->resetExit);
            return;
        }
        const QString serial = entry->serial;
        const QPointer<FakeToolInvocation> guard(i);
        QTimer::singleShot(kCommandMs, q, [this, guard, serial] {
            Entry* e = find(serial);
            if (e) {
                toAbsent(*e);   // out of Loader mode; it boots
                if (!e->noAdbReturn) {
                    const quint64 epoch = e->epoch;
                    QTimer::singleShot(resetDelayMs, q, [this, serial, epoch] {
                        Entry* back = find(serial);
                        if (back && back->epoch == epoch) {
                            toAdb(*back);
                        }
                    });
                }
            }
            if (guard) {
                guard->emitOutput("Reset Device OK.\r\n");
                guard->finish(0);
            }
        });
    }

    void onAdbDevices(FakeToolInvocation* i)
    {
        i->emitOutput(adbText());
        i->finish(0);
        // A board told to leave Loader mode before its writes does so once its mapping is
        // confirmed: the first `adb devices` that runs while it is in Loader mode.
        for (Entry& entry : boards) {
            if (entry.leaveBeforeWrite && entry.state == State::Loader) {
                entry.leaveBeforeWrite = false;
                const QString serial = entry.serial;
                QTimer::singleShot(1, q, [this, serial] {
                    if (Entry* e = find(serial)) {
                        toAdb(*e);
                    }
                });
            }
        }
    }

    void onAdbSerial(FakeToolInvocation* i)
    {
        const QStringList args = i->arguments();   // -s S <command...>
        const QString serial = args.value(1);
        const QStringList command = args.mid(2);
        Entry* entry = find(serial);
        const bool phone = phones.contains(serial);
        const bool online = phone || (entry && entry->adbListed && entry->adbState == QLatin1String("device") && entry->adbInstance);
        if (!online) {
            i->emitOutput("error: device '" + serial.toLatin1() + "' not found\r\n");
            i->finish(1);
            return;
        }
        if (command == QStringList{QStringLiteral("get-state")}) {
            i->emitOutput("device\n");
            i->finish(0);
            return;
        }
        if (command == QStringList{QStringLiteral("reboot"), QStringLiteral("loader")}) {
            i->finish(0);
            if (entry) {
                QTimer::singleShot(rebootDelayMs, q, [this, serial] { applyReboot(serial); });
            }
            return;
        }
        if (command.size() == 2 && command.first() == QLatin1String("shell")) {
            const QString what = command.at(1);
            if (what == QLatin1String("cat /proc/version")) {
                i->emitOutput(FirmwareFixtures::kProcVersion.left(FirmwareFixtures::kProcVersion.indexOf('\n') + 1));
            } else if (what == QLatin1String("cat /proc/cmdline")) {
                const QString mtd = entry ? entry->mtdParts : QString();
                i->emitOutput(mtd.isEmpty() ? QByteArray("console=ttyFIQ0 root=/dev/mmcblk0p5 rootwait\n")
                                            : "console=ttyFIQ0 root=/dev/mtdblock5 rootfstype=squashfs rootwait mtdparts=" + mtd.toLatin1()
                                                  + " rk_dma_heap_cma=72M\n");
            } else if (what == QLatin1String("cat /proc/version; uptime")) {
                i->emitOutput(FirmwareFixtures::kProcVersion);
            } else if (what == QLatin1String("rfmcu status")) {
                i->emitOutput("pid=412 uart=owned mcu=alive\n");
            } else {
                i->emitOutput("sh: not found\n");
                i->finish(127);
                return;
            }
            i->finish(0);
            return;
        }
        i->emitOutput("fake: unknown adb command\n");
        i->finish(1);
    }

    void onUnaddressed(FakeToolInvocation* i)
    {
        ++unaddressed;
        i->emitOutput("fake: unaddressed write refused\r\n");
        i->finish(99);
    }
};

// ---------------------------------------------------------------------------------------------

FakeUsbBus::FakeUsbBus(QObject* parent) : QObject(parent), d(new Private(this)) {}

FakeUsbBus::~FakeUsbBus()
{
    delete d;
}

void FakeUsbBus::addBoard(const QString& serial, const QString& port, State state)
{
    Private::Entry entry;
    entry.serial = serial;
    entry.port = port;
    entry.mtdParts = FirmwareFixtures::kNodeMtdParts;
    d->boards.append(entry);
    Private::Entry& added = d->boards.last();
    switch (state) {
    case State::Absent:
        d->toAbsent(added);
        break;
    case State::Adb:
        break;
    case State::Unauthorized:
        added.state = State::Unauthorized;
        added.adbState = QStringLiteral("unauthorized");
        break;
    case State::Loader:
    case State::Maskrom:
        d->toDownload(added, port, state == State::Maskrom, true, 0);
        break;
    }
}

void FakeUsbBus::setReboot(const QString& serial, Reboot behaviour, const QString& otherPort)
{
    if (Private::Entry* entry = d->find(serial)) {
        entry->reboot = behaviour;
        entry->otherPort = otherPort;
    }
}

void FakeUsbBus::setMtdParts(const QString& serial, const QString& mtdparts)
{
    if (Private::Entry* entry = d->find(serial)) {
        entry->mtdParts = mtdparts;
    }
}

void FakeUsbBus::setRebootDelayMs(int ms) { d->rebootDelayMs = ms; }
void FakeUsbBus::setPnpLagMs(int ms) { d->pnpLagMs = ms; }
void FakeUsbBus::setResetDelayMs(int ms) { d->resetDelayMs = ms; }
void FakeUsbBus::setLdDurationMs(int ms) { d->ldDurationMs = ms; }
void FakeUsbBus::setWriteDurationMs(int ms) { d->writeDurationMs = ms; }

void FakeUsbBus::setWriteFailure(const QString& serial, const QString& fileName, const QByteArray& output, int exitCode)
{
    if (Private::Entry* entry = d->find(serial)) {
        entry->writeFailures.insert(fileName, {output, exitCode});
    }
}

void FakeUsbBus::setResetFailure(const QString& serial, const QByteArray& output, int exitCode)
{
    if (Private::Entry* entry = d->find(serial)) {
        entry->resetFails = true;
        entry->resetOutput = output;
        entry->resetExit = exitCode;
    }
}

void FakeUsbBus::setNoAdbReturn(const QString& serial)
{
    if (Private::Entry* entry = d->find(serial)) {
        entry->noAdbReturn = true;
    }
}

void FakeUsbBus::leaveLoaderBeforeWrite(const QString& serial)
{
    if (Private::Entry* entry = d->find(serial)) {
        entry->leaveBeforeWrite = true;
    }
}

void FakeUsbBus::leaveLoaderAfterRows(const QString& serial, int rows)
{
    if (Private::Entry* entry = d->find(serial)) {
        entry->leaveAfterRows = rows;
    }
}

void FakeUsbBus::hangWrites(const QString& serial)
{
    if (Private::Entry* entry = d->find(serial)) {
        entry->hang = true;
    }
}

void FakeUsbBus::releaseWrites()
{
    for (Private::Entry& entry : d->boards) {
        entry.hang = false;
    }
    const QList<Private::Hung> hung = d->hung;
    d->hung.clear();
    for (const Private::Hung& h : hung) {
        d->completeWrite(h.invocation, h.serial);
    }
}

void FakeUsbBus::moveToAdb(const QString& serial)
{
    if (Private::Entry* entry = d->find(serial)) {
        d->toAdb(*entry);
    }
}

void FakeUsbBus::moveToLoader(const QString& serial)
{
    if (Private::Entry* entry = d->find(serial)) {
        d->toDownload(*entry, entry->port, false, true, 0);
    }
}

void FakeUsbBus::addForeignLoader(const QString& port)
{
    d->foreignLoaders.append({port, d->idFor(port)});
    emit changed();
}

void FakeUsbBus::addForeignAdb(const QString& serial)
{
    d->phones << serial;
    emit changed();
}

void FakeUsbBus::reenumerate(const QString& serial)
{
    Private::Entry* entry = d->find(serial);
    if (!entry || entry->ldId.isEmpty()) {
        return;
    }
    const QString fresh = QString::number(d->nextId++);
    d->portIds.insert(entry->dlPort, fresh);
    entry->ldId = fresh;
    emit changed();
}

QList<UsbDeviceInfo> FakeUsbBus::enumerate(quint16 vid) const
{
    QList<UsbDeviceInfo> devices;
    if (vid != kVid) {
        return devices;
    }
    for (const Private::Entry& entry : d->boards) {
        if (entry.adbInstance) {
            devices << instance(kAdbPid, entry.port);
        }
        if (entry.dlInstance) {
            devices << instance(entry.dlPid, entry.dlPort);
        }
    }
    for (const Private::Foreign& device : d->foreignLoaders) {
        devices << instance(kLoaderPid, device.port);
    }
    std::sort(devices.begin(), devices.end(), [](const UsbDeviceInfo& a, const UsbDeviceInfo& b) { return a.instanceId < b.instanceId; });
    return devices;
}

QByteArray FakeUsbBus::ldOutput() const { return d->ldText(); }
QByteArray FakeUsbBus::adbDevicesOutput() const { return d->adbText(); }

void FakeUsbBus::script(FakeToolRunner& runner)
{
    runner.addHandler(QRegularExpression(QStringLiteral("^upgrade_tool LD$")), [this](FakeToolInvocation* i) { d->onLd(i); });
    runner.addHandler(QRegularExpression(QStringLiteral("^upgrade_tool -s \\S+ WL ")), [this](FakeToolInvocation* i) { d->onWl(i); });
    runner.addHandler(QRegularExpression(QStringLiteral("^upgrade_tool -s \\S+ RD$")), [this](FakeToolInvocation* i) { d->onRd(i); });
    runner.addHandler(QRegularExpression(QStringLiteral("^upgrade_tool (WL|RD|UF)\\b")), [this](FakeToolInvocation* i) { d->onUnaddressed(i); });
    runner.addHandler(QRegularExpression(QStringLiteral("^adb devices$")), [this](FakeToolInvocation* i) { d->onAdbDevices(i); });
    runner.addHandler(QRegularExpression(QStringLiteral("^adb -s \\S+ ")), [this](FakeToolInvocation* i) { d->onAdbSerial(i); });
}

FakeUsbBus::Board FakeUsbBus::board(const QString& serial) const
{
    Board view;
    if (const Private::Entry* entry = d->find(serial)) {
        view.serial = entry->serial;
        view.port = entry->port;
        view.state = entry->state;
        view.locationId = entry->ldId;
        view.reboot = entry->reboot;
        view.otherPort = entry->otherPort;
        view.mtdParts = entry->mtdParts;
        view.ldListed = !entry->ldId.isEmpty();
    }
    return view;
}

QStringList FakeUsbBus::writesTo(const QString& serial) const
{
    const Private::Entry* entry = d->find(serial);
    return entry ? entry->writes : QStringList();
}

QStringList FakeUsbBus::resetsTo(const QString& serial) const
{
    const Private::Entry* entry = d->find(serial);
    return entry ? entry->resets : QStringList();
}

int FakeUsbBus::maxConcurrentWrites() const { return d->maxWl; }
int FakeUsbBus::ldDuringWrites() const { return d->overlaps; }
int FakeUsbBus::unaddressedCommands() const { return d->unaddressed; }
int FakeUsbBus::cancelledWrites() const { return d->cancelled; }
