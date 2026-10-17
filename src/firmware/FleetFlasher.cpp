#include "firmware/FleetFlasher.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>

#include <algorithm>
#include <utility>
#include <vector>

#include "app/Logging.h"
#include "firmware/AdbTool.h"
#include "firmware/FleetReport.h"
#include "firmware/ToolRunner.h"

namespace {

/// SocFlasher's kUfSilenceLogMs: a WL that prints nothing for this long is reported (again after
/// every further period), never stopped. The environment variable SU_FLEET_SILENCE_MS (a number of
/// milliseconds, read once by the constructor in every build) shortens it for tst_fleetflasher;
/// nothing else sets it.
constexpr int kSilenceLogMs = 5 * 60 * 1000;

/// The gate's own `LD` answers every re-check that asks within this long after its result.
constexpr qint64 kGateReuseMs = 500;

/// upgrade_tool WL counts 512-byte sectors.
constexpr quint64 kSectorBytes = 512;

const QLatin1String kMappingStopped("mapping stopped: ");

int silencePeriodMs()
{
    bool ok = false;
    const int fromEnvironment = qEnvironmentVariableIntValue("SU_FLEET_SILENCE_MS", &ok);
    return ok && fromEnvironment > 0 ? fromEnvironment : kSilenceLogMs;
}

/// "Linux version 5.10.209+ (user@host) ..." -> "5.10.209+" (SocFlasher's rule).
QString kernelRelease(const QString& procVersion)
{
    static const QRegularExpression pattern(QStringLiteral("Linux version (\\S+)"));
    const QRegularExpressionMatch match = pattern.match(procVersion);
    if (match.hasMatch()) {
        return match.captured(1);
    }
    return procVersion.trimmed().section(QLatin1Char('\n'), 0, 0).trimmed();
}

/// " 12:54:01 up 0 min,  load average: ..." -> "up 0 min" ("" when there is no uptime line).
QString uptimeWords(const QString& text)
{
    static const QRegularExpression pattern(QStringLiteral("\\bup\\s+([^,\\n]+)"));
    const QRegularExpressionMatch match = pattern.match(text);
    return match.hasMatch() ? QStringLiteral("up ") + match.captured(1).trimmed() : QString();
}

QString normalizedLayout(const QString& mtdparts)
{
    QString text = mtdparts.trimmed();
    if (text.startsWith(QLatin1String("mtdparts="))) {
        text = text.mid(9).trimmed();
    }
    return text;
}

QString oneDecimal(double seconds)
{
    return QString::number(seconds, 'f', 1);
}

/// LocationIDs are decimal numbers: "99" before "100"; anything else in text order.
bool idLess(const QString& a, const QString& b)
{
    bool okA = false;
    bool okB = false;
    const qulonglong numberA = a.toULongLong(&okA);
    const qulonglong numberB = b.toULongLong(&okB);
    if (okA && okB && numberA != numberB) {
        return numberA < numberB;
    }
    return a < b;
}

QStringList sortedIds(QStringList ids)
{
    ids.removeAll(QString());
    std::sort(ids.begin(), ids.end(), idLess);
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

QStringList sortedPorts(QStringList ports)
{
    ports.removeAll(QString());
    std::sort(ports.begin(), ports.end());
    ports.erase(std::unique(ports.begin(), ports.end()), ports.end());
    return ports;
}

/// `a` without the entries of `b` (both sorted; the result keeps a's order).
QStringList without(const QStringList& a, const QStringList& b)
{
    QStringList rest;
    for (const QString& entry : a) {
        if (!b.contains(entry)) {
            rest << entry;
        }
    }
    return rest;
}

QString joinedOrNone(const QStringList& list)
{
    return list.isEmpty() ? QStringLiteral("none") : list.join(QStringLiteral(", "));
}

/// "PID_110E": what a download-mode instance on a port is called in the check lines.
QString pidText(quint16 pid)
{
    return QStringLiteral("PID_") + QStringLiteral("%1").arg(pid, 4, 16, QLatin1Char('0')).toUpper();
}

/// What boardCommand() reports for a WL / RD that RockchipTool refused to build without an address.
QString unaddressedRefusal()
{
    return FleetFlasher::tr("internal: unaddressed write refused");
}

/// A retry line runs as it is: `program` is the upgrade_tool the run used (native separators,
/// quoted by formatCommandLine() when it holds a blank), not the bare name - the bundled tool is
/// not on PATH.
QString retryWl(const QString& program, const QString& locationId, const PartitionImage& row)
{
    const QString count = row.size == 0 ? QStringLiteral("-1") : QString::number(row.size / kSectorBytes);
    return ToolRunner::formatCommandLine(program, {QStringLiteral("-s"), locationId, QStringLiteral("WL"),
                                                   QString::number(row.offset / kSectorBytes), count, QDir::toNativeSeparators(row.file)});
}

QString retryRd(const QString& program, const QString& locationId)
{
    return ToolRunner::formatCommandLine(program, {QStringLiteral("-s"), locationId, QStringLiteral("RD")});
}

/// The RD named inside a message: the bare form, as UnattributedDevice::hint() words it (the
/// runnable line with the tool's path follows as a "  retry: ..." line).
QString rdHint(const QString& locationId)
{
    return QStringLiteral("upgrade_tool -s %1 RD").arg(locationId);
}

} // namespace

// =============================================================================================
// Value types
// =============================================================================================

QString FleetBoard::resultText() const
{
    switch (result) {
    case Result::Pending: return QStringLiteral("pending");
    case Result::Ready: return QStringLiteral("ready");
    case Result::Done: return QStringLiteral("done");
    case Result::NotBack: return QStringLiteral("not back");
    case Result::Failed: return QStringLiteral("failed");
    case Result::Skipped: return QStringLiteral("skipped");
    }
    return QString();
}

QString FleetBoard::stageText() const
{
    switch (stage) {
    case Stage::Selected: return QStringLiteral("selected");
    case Stage::Mapping: return QStringLiteral("mapping");
    case Stage::Loader: return QStringLiteral("loader");
    case Stage::Writing: return QStringLiteral("writing");
    case Stage::Resetting: return QStringLiteral("resetting");
    case Stage::WaitingForAdb: return QStringLiteral("adb");
    case Stage::ReadingInfo: return QStringLiteral("info");
    case Stage::Finished: return QStringLiteral("finished");
    }
    return QString();
}

bool FleetBoard::operator==(const FleetBoard& o) const
{
    return tag == o.tag && serial == o.serial && adbState == o.adbState && expectedPort == o.expectedPort && port == o.port
           && locationId == o.locationId && kernel == o.kernel && layout == o.layout && profile == o.profile && stage == o.stage
           && result == o.result
           && detail == o.detail && written == o.written && verified == o.verified && verifyFailed == o.verifyFailed
           && verifyMismatch == o.verifyMismatch && retry == o.retry && adbBack == o.adbBack
           && leftInLoader == o.leftInLoader && boardInfo == o.boardInfo && uptime == o.uptime && rfmcu == o.rfmcu
           && seconds == o.seconds && mappingSeconds == o.mappingSeconds && writingSeconds == o.writingSeconds;
}

FleetFlasher::Snapshot FleetFlasher::Snapshot::from(const QList<UsbDeviceInfo>& bus, const QList<RockusbDevice>& rockusb)
{
    Snapshot snapshot;
    for (const UsbDeviceInfo& device : bus) {
        const QString key = device.portKey();
        if (key.isEmpty()) {
            continue;   // a device the mapping cannot place
        }
        if (device.isAdbGadget()) {
            snapshot.adbPorts << key;
        } else if (device.isDownloadMode()) {
            snapshot.downloadPorts.insert(key, device.pid);
        }
    }
    for (const RockusbDevice& device : rockusb) {
        if (device.mode == RockusbDevice::Mode::Loader) {
            snapshot.loaderIds << device.locationId;
        } else if (device.mode == RockusbDevice::Mode::Maskrom) {
            snapshot.maskromIds << device.locationId;
        }
    }
    snapshot.adbPorts = sortedPorts(snapshot.adbPorts);
    snapshot.loaderIds = sortedIds(snapshot.loaderIds);
    snapshot.maskromIds = sortedIds(snapshot.maskromIds);
    return snapshot;
}

bool FleetFlasher::Snapshot::operator==(const Snapshot& o) const
{
    return adbPorts == o.adbPorts && downloadPorts == o.downloadPorts && loaderIds == o.loaderIds && maskromIds == o.maskromIds;
}

bool FleetFlasher::Verdict::operator==(const Verdict& o) const
{
    return kind == o.kind && waiting == o.waiting && port == o.port && locationId == o.locationId && newIds == o.newIds
           && detail == o.detail;
}

QString FleetFlasher::UnattributedDevice::hint() const
{
    return QStringLiteral("upgrade_tool -s %1 RD").arg(locationId);
}

bool FleetFlasher::UnattributedDevice::operator==(const UnattributedDevice& o) const
{
    return locationId == o.locationId && port == o.port && mode == o.mode;
}

// =============================================================================================
// compare(): the difference method, pure
// =============================================================================================

FleetFlasher::Verdict FleetFlasher::compare(const Snapshot& before, const Snapshot& after)
{
    const QStringList vanished = without(sortedPorts(before.adbPorts), after.adbPorts);
    const QStringList appeared = without(sortedPorts(after.downloadPorts.keys()), sortedPorts(before.downloadPorts.keys()));
    const QStringList newIds = without(sortedIds(after.loaderIds + after.maskromIds), before.loaderIds + before.maskromIds);

    Verdict verdict;
    verdict.kind = Verdict::Kind::Pending;
    if (vanished.isEmpty() && appeared.isEmpty() && newIds.isEmpty()) {
        verdict.waiting = Verdict::Waiting::StillOnAdb;
        verdict.detail = FleetFlasher::tr("no change on the USB bus yet: the board is still on ADB");
        return verdict;
    }
    if (vanished.size() > 1 || appeared.size() > 1 || newIds.size() > 1) {
        verdict.kind = Verdict::Kind::Ambiguous;
        verdict.newIds = newIds;
        verdict.detail = FleetFlasher::tr("two boards changed at once: ADB instances gone on ports %1; Loader devices appeared on "
                                          "ports %2; new LocationIDs %3")
                             .arg(joinedOrNone(vanished), joinedOrNone(appeared), joinedOrNone(newIds));
        return verdict;
    }
    if (vanished.isEmpty() && appeared.size() == 1) {
        const QString port = appeared.first();
        if (after.adbPorts.contains(port)) {
            verdict.waiting = Verdict::Waiting::GhostOnPort;
            verdict.port = port;
            verdict.detail = FleetFlasher::tr("a Loader device on port %1 which still lists an ADB device").arg(port);
            return verdict;
        }
        verdict.kind = Verdict::Kind::ForeignLoader;
        verdict.port = port;
        verdict.newIds = newIds;
        const QString id = newIds.isEmpty() ? FleetFlasher::tr("no LocationID yet")
                                            : FleetFlasher::tr("LocationID %1").arg(newIds.first());
        verdict.detail = FleetFlasher::tr("a device entered download mode on port %1 (%2) while the board is still on ADB: not this board")
                             .arg(port, id);
        return verdict;
    }
    if (vanished.isEmpty()) {   // appeared = {}, newIds = {n}
        verdict.waiting = Verdict::Waiting::LdOnly;
        verdict.locationId = newIds.first();
        verdict.newIds = newIds;
        verdict.detail = FleetFlasher::tr("upgrade_tool LD lists a new LocationID %1 but the bus shows no new download-mode device yet")
                             .arg(newIds.first());
        return verdict;
    }
    const QString left = vanished.first();
    if (appeared.size() == 1 && appeared.first() != left) {
        verdict.kind = Verdict::Kind::PortMismatch;
        verdict.port = appeared.first();
        verdict.newIds = newIds;
        verdict.detail = FleetFlasher::tr("the board left port %1 but a Loader device appeared on port %2").arg(left, appeared.first());
        return verdict;
    }
    verdict.port = left;
    if (appeared.isEmpty()) {
        verdict.waiting = Verdict::Waiting::LoaderNotEnumerated;
        verdict.detail = FleetFlasher::tr("the board left ADB (port %1), waiting for its Loader device").arg(left);
        return verdict;
    }
    if (newIds.isEmpty()) {
        verdict.waiting = Verdict::Waiting::LdNotListing;
        verdict.detail = FleetFlasher::tr("Loader device on port %1, waiting for upgrade_tool LD to list it").arg(left);
        return verdict;
    }
    verdict.locationId = newIds.first();
    if (after.maskromIds.contains(verdict.locationId)) {
        verdict.kind = Verdict::Kind::Maskrom;
        verdict.detail = FleetFlasher::tr("in MASKROM mode on port %1, LocationID %2").arg(left, verdict.locationId);
    } else {
        verdict.kind = Verdict::Kind::Mapped;
        verdict.detail = FleetFlasher::tr("in Loader mode on port %1, LocationID %2").arg(left, verdict.locationId);
    }
    return verdict;
}

QString FleetFlasher::tagFor(int index)
{
    // Bijective base 26: A..Z, AA..AZ, BA, ... ZZ, AAA.
    QString tag;
    qint64 n = qint64(qMax(0, index)) + 1;
    while (n > 0) {
        --n;
        tag.prepend(QChar(QLatin1Char(char('A' + n % 26))));
        n /= 26;
    }
    return tag;
}

QStringList FleetFlasher::bootloaderPartitions()
{
    return {QStringLiteral("idblock"), QStringLiteral("uboot"), QStringLiteral("env"), QStringLiteral("meta")};
}

// =============================================================================================
// Private
// =============================================================================================

/**
 * One run is a chain of asynchronous steps on the event loop; every callback captures the run's
 * id and returns when the run was replaced or has ended. The boards live in `boards` (selection
 * order) for the whole run; callbacks address them by index. Per board at most one tool process
 * runs at a time (`busy`); the run-level tools (`adb devices`, the snapshots' `LD`, the gate's
 * `LD`) are tracked apart. inFlightUpgradeTools counts the WL / RL / RD processes of the run, which
 * is what the LD gate looks at (v0.8.0: a verify's RL counts like a WL, in both directions).
 */
struct FleetFlasher::Private
{
    struct Board
    {
        FleetBoard info;
        std::unique_ptr<AdbTool> adb;          ///< -s <serial> on every command
        QPointer<ToolInvocation> process;      ///< the board's one tool process
        bool busy = false;                     ///< a process is running or being started
        bool candidate = false;                ///< state "device" at the selection
        bool ended = false;                    ///< its result is final
        bool rebooted = false;
        bool maskrom = false;                  ///< entered MASKROM instead of Loader mode
        bool inSlot = false;                   ///< counts against maxParallel (first check to its RD)
        bool started = false;                  ///< its first WL was started: a drain lets it finish
        bool checkQueued = false;              ///< waits for the gate's LD
        bool checkBeforeRd = false;
        bool leftBeforeReset = false;          ///< the pre-RD check failed: the ADB wait decides
        bool resetOk = false;                  ///< a stop's / a cancel's RD answered OK (counted in resetCount)
        quint16 mappedPid = 0;                 ///< the pid its port showed when it was mapped (its Loader mode)
        ProfileDetection detection;            ///< (v0.8.0) its profile as step 2 b / Mapped detected it (info.profile = its id)
        bool resetOnly = false;                ///< (v0.8.0) stopped at its Mapped by the profile gate: never to be written, a reset is all it needs
        bool verifying = false;                ///< (v0.8.0, verify-rl) the read-back of rows.at(row) runs (its RL is the board's process)
        QElapsedTimer verifyClock;             ///< (v0.8.0) from that RL's start
        QString leftEvidenceNow;               ///< "port P shows ...; LD ..." of that check
        QString leftEvidencePast;              ///< the same with "showed"
        QString vanishedPort;                  ///< LoaderNotEnumerated: the port its ADB instance left
        QString pendingSkip;                   ///< not on ADB: logged once the baseline is read
        int row = 0;                           ///< the row to write next
        quint64 bytesDone = 0;                 ///< bytes of the rows written completely
        int lastPercent = -1;
        RockchipTool::ProgressParser parser;
        QByteArray lineBuffer;
        QElapsedTimer clock;                   ///< from its `reboot loader`
        QElapsedTimer writeClock;              ///< from its first pre-write check
        QElapsedTimer rowClock;
        QElapsedTimer waitClock;               ///< the Loader wait, then the ADB wait
        QElapsedTimer lastOutput;
        QTimer* silenceTimer = nullptr;
        bool firstPoll = true;                 ///< the first get-state of the ADB wait is logged
    };

    /// One snapshot as it came in: the pure part plus what the messages need.
    struct Picture
    {
        Snapshot snapshot;
        QList<UsbDeviceInfo> bus;
        QList<RockusbDevice> rockusb;
        QString error;                         ///< `LD` could not run / timed out: no picture
    };

    /// What the enumeration shows on one port right now (no device I/O).
    struct PortView
    {
        bool adb = false;                      ///< an ADB gadget on the port
        quint16 pid = 0;                       ///< the first download-mode instance's pid; 0 = none
    };

    Private(FleetFlasher* owner, ToolRunner* toolRunner)
        : q(owner), runner(toolRunner), upgradeTool(new RockchipTool(toolRunner, owner)), adb(new AdbTool(toolRunner, owner)),
          silenceMs(silencePeriodMs())
    {
        pollTimer = new QTimer(owner);
        pollTimer->setSingleShot(true);
        adbTimer = new QTimer(owner);
        QObject::connect(adbTimer, &QTimer::timeout, owner, [this] { adbTick(); });
        if (UsbProbe::hasPlatformProbe()) {
            enumerator = [](quint16 vid) { return UsbProbe::presentDevices(vid); };
        }
    }

    FleetFlasher* q;
    QPointer<ToolRunner> runner;
    RockchipTool* upgradeTool;
    AdbTool* adb;
    QTimer* pollTimer = nullptr;               ///< the mapping's next poll
    QTimer* adbTimer = nullptr;                ///< the ADB waits, every board from one timer
    const int silenceMs;

    // ---- inputs ----------------------------------------------------------------------------
    QString imagePath;
    QList<PartitionImage> rows;
    QString planLayout;
    bool planLayoutFromImage = false;
    QList<QPair<QString, QString>> selection;
    bool everyBoard = false;
    int parallel = 0;
    bool dry = false;
    int loaderWaitSeconds = 30;
    int adbReturnSeconds = 60;
    int pollMs = 1000;
    bool waitAdb = true;
    LdPolicy policy = LdPolicy::Gate;
    UsbProbe::Enumerator enumerator;
    PortProvider portProvider;
    ProfileDetection planProfile;              ///< (v0.8.0) known() false = the profile gate is off
    QString profileOverride;                   ///< (v0.8.0) --profile: every board's profile without detection
    bool verify = false;                       ///< (v0.8.0, verify-rl) every clean WL is read back and compared

    // ---- the run ---------------------------------------------------------------------------
    quint64 runId = 0;
    bool running = false;
    Phase phase = Phase::Idle;
    bool draining = false;
    bool closing = false;                      ///< the end-of-run snapshot is under way
    bool canClose = false;                     ///< the mapping is over (done, stopped, cancelled)
    std::vector<std::unique_ptr<Board>> boards;
    QString stopText;
    int resetCount = 0;
    QList<UnattributedDevice> foreign;
    QList<UnattributedDevice> unattributed;
    QElapsedTimer runClock;
    double finalElapsed = 0.0;
    QElapsedTimer mappingClock;
    bool mappingStarted = false;
    bool mappingEnded = false;
    double mappingSecs = 0.0;
    QElapsedTimer writingClock;
    bool writingStarted = false;
    double writingSecs = 0.0;
    bool anyRebooted = false;
    int adbEntries = 0;                        ///< `adb devices` lines of any state (the --all-boards count)
    QStringList adbSerials;                    ///< every serial `adb devices` listed
    Snapshot baseline;
    int mapIndex = -1;                         ///< the board being mapped
    Snapshot before;
    QPointer<ToolInvocation> runProcess;       ///< `adb devices` / a snapshot's LD
    bool snapshotRunning = false;
    quint64 totalBytes = 0;

    // ---- the LD gate -----------------------------------------------------------------------
    int inFlightUpgradeTools = 0;              ///< WL / RL / RD processes of this run
    bool gateLdBusy = false;
    QPointer<ToolInvocation> gateLd;
    QList<RockusbDevice> gateListing;
    bool gateListingValid = false;
    QElapsedTimer gateListingAge;
    QList<int> gateWaiting;                    ///< checks the running LD answers
    QList<int> gateNext;                       ///< BeforeEveryWrite: checks the next LD answers
    QList<std::function<void()>> afterGateLd;  ///< RDs of a stop / a drain waiting for the gate's LD

    // =========================================================================================
    // Helpers
    // =========================================================================================

    bool alive(quint64 id) const { return id == runId && running; }

    void setPhase(Phase next)
    {
        if (phase == next) {
            return;
        }
        phase = next;
        qCInfo(lcFirmware) << "fleet flasher phase" << next;
        emit q->phaseChanged(next);
    }

    void log(const QString& line)
    {
        qCInfo(lcFirmware).noquote() << line;
        emit q->logLine(line);
    }

    void boardLog(Board& board, const QString& text)
    {
        qCInfo(lcFirmware).noquote() << board.info.label() << text;
        emit q->boardEvent(board.info, text);
    }

    void changed(Board& board) { emit q->boardChanged(board.info); }

    Board& at(int index) { return *boards.at(static_cast<size_t>(index)); }
    int count() const { return static_cast<int>(boards.size()); }

    QString waitText() const { return QString::number(loaderWaitSeconds); }

    /// "$ command" now and "  exit code n" when it ends, as a run line.
    ToolInvocation* trackRun(ToolInvocation* invocation, bool verbose = true)
    {
        if (!verbose) {
            return invocation;
        }
        const quint64 id = runId;
        log(QStringLiteral("$ ") + invocation->commandLine());
        QObject::connect(invocation, &ToolInvocation::finished, q, [this, id](const ToolResult& r) {
            if (id == runId && !r.cancelled) {
                log(resultLine(r));
            }
        });
        return invocation;
    }

    /// The same as the board's lines.
    ToolInvocation* trackBoard(int index, ToolInvocation* invocation, bool verbose = true)
    {
        if (!verbose) {
            return invocation;
        }
        const quint64 id = runId;
        boardLog(at(index), QStringLiteral("$ ") + invocation->commandLine());
        QObject::connect(invocation, &ToolInvocation::finished, q, [this, id, index](const ToolResult& r) {
            if (id == runId && !r.cancelled && index < count()) {
                boardLog(at(index), resultLine(r));
            }
        });
        return invocation;
    }

    static QString resultLine(const ToolResult& r)
    {
        if (!r.started) {
            return FleetFlasher::tr("  could not run: %1").arg(r.error);
        }
        if (r.timedOut) {
            return FleetFlasher::tr("  timed out");
        }
        return FleetFlasher::tr("  exit code %1").arg(r.exitCode);
    }

    /// Runs `start()` (an adb or upgrade_tool command of board `index`) as the board's one
    /// process: `then` gets the result unless the run was replaced meanwhile.
    void boardCommand(int index, const std::function<ToolInvocation*()>& create, bool verbose,
                      std::function<void(const ToolResult&)> then)
    {
        Board& board = at(index);
        Q_ASSERT(!board.busy);
        board.busy = true;   // before the process exists: a slot of the runner may reenter
        ToolInvocation* invocation = create();
        if (!invocation) {
            board.busy = false;
            ToolResult refused;
            refused.error = unaddressedRefusal();
            then(refused);
            return;
        }
        trackBoard(index, invocation, verbose);
        board.process = invocation;
        const quint64 id = runId;
        QObject::connect(invocation, &ToolInvocation::finished, q, [this, id, index, then = std::move(then)](const ToolResult& r) {
            if (id != runId || index >= count()) {
                return;
            }
            Board& b = at(index);
            b.busy = false;
            b.process.clear();
            if (running) {
                then(r);
            }
        });
    }

    QList<UsbDeviceInfo> enumerate() const
    {
        return enumerator ? enumerator(UsbProbe::kRockchipVid) : QList<UsbDeviceInfo>();
    }

    PortView portView(const QString& port) const
    {
        PortView view;
        for (const UsbDeviceInfo& device : enumerate()) {
            if (device.portKey() != port) {
                continue;
            }
            view.adb = view.adb || device.isAdbGadget();
            if (device.isDownloadMode() && view.pid == 0) {
                view.pid = device.pid;
            }
        }
        return view;
    }

    /// The board's port shows it in its Loader mode: the pid it showed when it was mapped and no
    /// ADB device. Another download-mode pid there (MASKROM: a brown-out drops a board to its
    /// BootROM) is not Loader mode, whatever the enumeration calls it.
    static bool inLoaderByPort(const PortView& view, quint16 mappedPid)
    {
        return !view.adb && view.pid != 0 && view.pid == mappedPid;
    }

    /// "an ADB device" / "nothing" / "PID_110E" / "PID_110C, not its Loader-mode PID_110E".
    static QString shownOnPort(const PortView& view, quint16 mappedPid)
    {
        if (view.adb) {
            return FleetFlasher::tr("an ADB device");
        }
        if (view.pid == 0) {
            return FleetFlasher::tr("nothing");
        }
        if (mappedPid == 0 || view.pid == mappedPid) {
            return pidText(view.pid);
        }
        return FleetFlasher::tr("%1, not its Loader-mode %2").arg(pidText(view.pid), pidText(mappedPid));
    }

    QString retryProgram() const { return QDir::toNativeSeparators(upgradeTool->toolPath()); }
    QString retryRdLine(const QString& locationId) const { return retryRd(retryProgram(), locationId); }

    /// A snapshot in the pinned order: `LD` is started first and the enumeration is read when its
    /// result arrives, so the bus is never older than the listing (the class comment, step 2 c).
    void takeSnapshot(std::function<void(const Picture&)> then)
    {
        snapshotRunning = true;
        const quint64 id = runId;
        ToolInvocation* invocation = trackRun(upgradeTool->listDevices());
        runProcess = invocation;
        QObject::connect(invocation, &ToolInvocation::finished, q, [this, id, then = std::move(then)](const ToolResult& r) {
            if (!alive(id)) {
                return;
            }
            snapshotRunning = false;
            runProcess.clear();
            Picture picture;
            if (!r.started) {
                picture.error = FleetFlasher::tr("upgrade_tool could not be run: %1").arg(r.error);
            } else if (r.timedOut) {
                picture.error = FleetFlasher::tr("upgrade_tool LD timed out");
            } else {
                picture.rockusb = RockchipTool::parseDeviceList(r.outputText());
                picture.bus = enumerate();   // read now, at LD's result: never older than the listing
                picture.snapshot = Snapshot::from(picture.bus, picture.rockusb);
            }
            then(picture);
        });
    }

    QStringList rowNames() const
    {
        QStringList names;
        for (const PartitionImage& row : rows) {
            names << row.name;
        }
        return names;
    }

    bool rowsIncludeEnv() const { return rowNames().contains(QStringLiteral("env")); }

    /// "upgrade_tool -s n WL ..." for every row from `fromRow` that is not written yet, then the RD.
    QStringList retryLines(const Board& board, int fromRow) const
    {
        const QString program = retryProgram();
        QStringList lines;
        for (int r = qMax(0, fromRow); r < rows.size(); ++r) {
            if (!board.info.written.contains(rows.at(r).name)) {
                lines << retryWl(program, board.info.locationId, rows.at(r));
            }
        }
        lines << retryRd(program, board.info.locationId);
        return lines;
    }

    QString loaderHint(const Board& board) const
    {
        const QString n = board.info.locationId;
        return FleetFlasher::tr("left in Loader mode on port %1, LocationID %2: nothing else is written to it. Retry by hand, "
                                "addressed: one `upgrade_tool -s %2 WL <sector> <count> <file>` per missing row, then "
                                "`upgrade_tool -s %2 RD` (the lines follow); `su-fw flash-soc --serial %3` only once `upgrade_tool "
                                "LD` lists it as the ONLY device in download mode (the single-board flow writes without -s and "
                                "takes any Loader device)")
            .arg(board.info.port, n, board.info.serial);
    }

    QStringList retryLogLines(const QStringList& lines) const
    {
        QStringList logged;
        for (const QString& line : lines) {
            logged << QStringLiteral("  retry: ") + line;
        }
        return logged;
    }

    void addUnattributed(const UnattributedDevice& device)
    {
        for (const UnattributedDevice& known : std::as_const(unattributed)) {
            if (known.locationId == device.locationId) {
                return;
            }
        }
        unattributed << device;
        log(device.port.isEmpty()
                ? FleetFlasher::tr("LocationID %1 is in %2 mode and belongs to no mapped board: reset it with `%3`")
                      .arg(device.locationId, device.mode, device.hint())
                : FleetFlasher::tr("LocationID %1 on port %2 is in %3 mode and belongs to no mapped board: reset it with `%4`")
                      .arg(device.locationId, device.port, device.mode, device.hint()));
    }

    /// The ids of `ids` as unattributed devices of `picture`: a port only when one id meets one
    /// port (never paired by guessing among several).
    QList<UnattributedDevice> looseDevices(const QStringList& ids, const QStringList& ports, const Snapshot& snapshot) const
    {
        QList<UnattributedDevice> devices;
        for (const QString& id : ids) {
            UnattributedDevice device;
            device.locationId = id;
            device.port = ids.size() == 1 && ports.size() == 1 ? ports.first() : QString();
            device.mode = snapshot.maskromIds.contains(id) ? QStringLiteral("Maskrom") : QStringLiteral("Loader");
            devices << device;
        }
        return devices;
    }

    // =========================================================================================
    // Board ends
    // =========================================================================================

    void releaseSlot(Board& board)
    {
        if (board.inSlot) {
            board.inSlot = false;
            if (writingStarted) {
                writingSecs = writingClock.elapsed() / 1000.0;
            }
        }
    }

    /// The board's result is final: its end line (and `extra` lines after it), then boardFinished().
    void endBoard(int index, FleetBoard::Result result, const QString& detail, const QStringList& extra = QStringList())
    {
        Board& board = at(index);
        if (board.ended) {
            return;
        }
        board.ended = true;
        board.checkQueued = false;
        gateWaiting.removeAll(index);
        gateNext.removeAll(index);
        if (board.silenceTimer) {
            board.silenceTimer->stop();
        }
        releaseSlot(board);
        board.info.result = result;
        board.info.detail = detail;
        board.info.stage = FleetBoard::Stage::Finished;
        if (board.rebooted) {
            board.info.seconds = board.clock.elapsed() / 1000.0;
        }
        switch (result) {
        case FleetBoard::Result::Done:
            boardLog(board, FleetFlasher::tr("done in %1 s (mapping %2 s, writing %3 s)")
                                .arg(oneDecimal(board.info.seconds), oneDecimal(board.info.mappingSeconds),
                                     oneDecimal(board.info.writingSeconds)));
            break;
        case FleetBoard::Result::Skipped:
            boardLog(board, FleetFlasher::tr("%1; skipped").arg(detail));
            break;
        case FleetBoard::Result::Failed:
        case FleetBoard::Result::NotBack:
            boardLog(board, detail);
            break;
        case FleetBoard::Result::Ready:
        case FleetBoard::Result::Pending:
            break;
        }
        for (const QString& line : extra) {
            boardLog(board, line);
        }
        qCInfo(lcFirmware).noquote() << board.info.label() << "finished:" << board.info.resultText();
        changed(board);
        emit q->boardFinished(board.info);
    }

    // =========================================================================================
    // Start, selection, baseline
    // =========================================================================================

    QString validate() const
    {
        if (selection.isEmpty() && !everyBoard) {
            return FleetFlasher::tr("no board selected: name the boards (--serials) or take every board on ADB (--all-boards)");
        }
        if (rows.isEmpty()) {
            return FleetFlasher::tr("nothing to write: no partition selected");
        }
        if (upgradeTool->toolPath().isEmpty()) {
            return FleetFlasher::tr("upgrade_tool not found: set its path in Preferences");
        }
        if (adb->adbPath().isEmpty()) {
            return FleetFlasher::tr("adb not found: set its path in Preferences");
        }
        if (RockchipTool::parseMtdParts(planLayout).isEmpty()) {
            return FleetFlasher::tr("flash layout unknown: pass --layout, or run su-fw status once with a board running; the "
                                    "Node image carries its own");
        }
        if (!runner) {
            return FleetFlasher::tr("no tool runner");
        }
        return QString();
    }

    void resetRun()
    {
        pollTimer->stop();
        adbTimer->stop();
        for (const auto& board : boards) {
            if (board->silenceTimer) {
                board->silenceTimer->stop();
                board->silenceTimer->deleteLater();
            }
        }
        boards.clear();
        draining = false;
        closing = false;
        canClose = false;
        stopText.clear();
        resetCount = 0;
        foreign.clear();
        unattributed.clear();
        finalElapsed = 0.0;
        mappingStarted = false;
        mappingEnded = false;
        mappingSecs = 0.0;
        writingStarted = false;
        writingSecs = 0.0;
        anyRebooted = false;
        adbEntries = 0;
        adbSerials.clear();
        baseline = Snapshot();
        mapIndex = -1;
        before = Snapshot();
        runProcess.clear();
        snapshotRunning = false;
        inFlightUpgradeTools = 0;
        gateLdBusy = false;
        gateLd.clear();
        gateListing.clear();
        gateListingValid = false;
        gateWaiting.clear();
        gateNext.clear();
        afterGateLd.clear();
        totalBytes = 0;
    }

    bool start()
    {
        if (running) {
            return false;
        }
        ++runId;
        resetRun();
        runClock.start();
        const QString problem = validate();
        if (!problem.isEmpty()) {
            stopText = problem;
            const quint64 id = runId;
            QTimer::singleShot(0, q, [this, id, problem] {
                if (id == runId && !running) {
                    qCInfo(lcFirmware).noquote() << "fleet flash refused:" << problem;
                    setPhase(Phase::Failed);
                    emit q->finished(false, problem);
                }
            });
            return false;
        }
        running = true;
        for (const PartitionImage& row : std::as_const(rows)) {
            const QFileInfo file(row.file);
            totalBytes += file.isFile() ? static_cast<quint64>(file.size()) : 0;
        }
        setPhase(Phase::Selecting);
        if (dry) {
            log(FleetFlasher::tr("Dry run: the selection, the remembered ports and the plan; no reboot, no write"));
            // (v0.8.0) It reads no /proc/cmdline and maps nothing: a mixed bench is not told here.
            log(FleetFlasher::tr("Dry run: the boards' profiles are read before their reboot in a real run"));
        }
        log(fleetLine());
        const quint64 id = runId;
        QTimer::singleShot(0, q, [this, id] {
            if (alive(id)) {
                selectBoards();
            }
        });
        return true;
    }

    QString fleetLine() const
    {
        const QString what = FleetFlasher::tr("%1 (%2 bytes)").arg(rowNames().join(QStringLiteral(", "))).arg(totalBytes);
        if (everyBoard) {
            return parallel > 0 ? FleetFlasher::tr("Fleet: every board adb lists (--all-boards); writing %1 on up to %2 at once")
                                      .arg(what)
                                      .arg(parallel)
                                : FleetFlasher::tr("Fleet: every board adb lists (--all-boards); writing %1 on all of them at once")
                                      .arg(what);
        }
        QStringList tags;
        for (const auto& entry : selection) {
            tags << entry.first;
        }
        const int n = static_cast<int>(selection.size());
        return FleetFlasher::tr("Fleet: %1 boards (%2); writing %3 on up to %4 at once")
            .arg(n)
            .arg(tags.join(QStringLiteral(", ")), what)
            .arg(parallel > 0 ? qMin(parallel, n) : n);
    }

    /// A refusal: the run ends before anything was touched (or, from the baseline on, before any
    /// reboot); every board not decided yet is Skipped with the reason.
    void refuse(const QString& reason)
    {
        stopText = reason;   // the result line says it (the job's "Flash fleet failed: <reason>")
        for (int i = 0; i < count(); ++i) {
            Board& board = at(i);
            if (!board.ended) {
                endBoard(i, FleetBoard::Result::Skipped,
                         board.pendingSkip.isEmpty() ? FleetFlasher::tr("not touched: %1").arg(reason) : board.pendingSkip);
            }
        }
        finishRun();
    }

    void selectBoards()
    {
        const QString platform = q->platformRefusal();
        if (!platform.isEmpty()) {
            refuse(platform);
            return;
        }
        if (draining) {
            refuse(FleetFlasher::tr("cancelled before any board was touched"));
            return;
        }
        const quint64 id = runId;
        ToolInvocation* invocation = trackRun(adb->devices());
        runProcess = invocation;
        QObject::connect(invocation, &ToolInvocation::finished, q, [this, id](const ToolResult& r) {
            if (alive(id)) {
                runProcess.clear();
                onAdbDevices(r);
            }
        });
    }

    void addBoard(const QString& tag, const QString& serial, const QString& state)
    {
        auto board = std::make_unique<Board>();
        board->info.tag = tag;
        board->info.serial = serial;
        board->info.adbState = state;
        board->info.expectedPort = portProvider ? portProvider(serial).trimmed() : QString();
        board->adb = std::make_unique<AdbTool>(runner, nullptr);
        board->adb->setAdbPath(adb->adbPath());
        board->adb->setTimeoutMs(adb->timeoutMs());
        board->adb->setSerial(serial);
        board->silenceTimer = new QTimer(q);
        board->silenceTimer->setSingleShot(true);
        const int index = count();
        QObject::connect(board->silenceTimer, &QTimer::timeout, q, [this, index, id = runId] {
            if (alive(id) && index < count()) {
                onSilence(index);
            }
        });
        boards.push_back(std::move(board));
    }

    void onAdbDevices(const ToolResult& r)
    {
        if (draining) {
            refuse(FleetFlasher::tr("cancelled before any board was touched"));
            return;
        }
        if (!r.started) {
            refuse(FleetFlasher::tr("adb could not be run: %1").arg(r.error));
            return;
        }
        if (r.timedOut) {
            refuse(FleetFlasher::tr("adb devices timed out"));
            return;
        }
        const QList<AdbDevice> devices = AdbTool::parseDevices(r.outputText());
        adbEntries = static_cast<int>(devices.size());
        for (const AdbDevice& device : devices) {
            adbSerials << device.serial;
        }
        if (everyBoard) {
            int next = 0;
            for (const AdbDevice& device : devices) {
                if (device.state != QLatin1String("device")) {
                    log(FleetFlasher::tr("adb lists %1 as '%2': not selected (--all-boards takes the boards in state \"device\")")
                            .arg(device.serial, device.state));
                    continue;
                }
                addBoard(FleetFlasher::tagFor(next++), device.serial, device.state);
                boards.back()->candidate = true;
            }
        } else {
            for (const auto& [tag, serial] : std::as_const(selection)) {
                QString state;
                for (const AdbDevice& device : devices) {
                    if (device.serial == serial) {
                        state = device.state;
                        break;
                    }
                }
                addBoard(tag, serial, state);
                QString error;
                if (!AdbTool::selectDevice(devices, serial, &error).isEmpty()) {
                    boards.back()->candidate = true;
                } else {
                    boards.back()->pendingSkip = FleetFlasher::tr("not on ADB: %1").arg(error);
                }
            }
        }
        for (const auto& board : boards) {
            emit q->boardChanged(board->info);
        }
        bool anyCandidate = false;
        for (const auto& board : boards) {
            anyCandidate = anyCandidate || board->candidate;
        }
        if (!anyCandidate) {
            refuse(FleetFlasher::tr("no selected board is on ADB (attached: %1)").arg(joinedOrNone(adbSerials)));
            return;
        }
        readBaseline();
    }

    void readBaseline()
    {
        log(UsbProbe::describe(enumerate()));
        const quint64 id = runId;
        snapshotRunning = true;
        ToolInvocation* invocation = trackRun(upgradeTool->listDevices());
        runProcess = invocation;
        QObject::connect(invocation, &ToolInvocation::finished, q, [this, id](const ToolResult& r) {
            if (!alive(id)) {
                return;
            }
            snapshotRunning = false;
            runProcess.clear();
            onBaseline(r);
        });
    }

    void onBaseline(const ToolResult& r)
    {
        if (draining) {
            for (int i = 0; i < count(); ++i) {
                if (!at(i).ended && at(i).candidate) {
                    endBoard(i, FleetBoard::Result::Skipped, FleetFlasher::tr("cancelled before the mapping; not touched"));
                }
            }
            refuse(FleetFlasher::tr("cancelled before any board was rebooted"));
            return;
        }
        if (!r.started) {
            refuse(FleetFlasher::tr("upgrade_tool could not be run: %1").arg(r.error));
            return;
        }
        if (r.timedOut) {
            refuse(FleetFlasher::tr("upgrade_tool LD timed out"));
            return;
        }
        const QList<RockusbDevice> rockusb = RockchipTool::parseDeviceList(r.outputText());
        log(rockusb.size() == 1 ? FleetFlasher::tr("upgrade_tool LD: 1 device")
                                : FleetFlasher::tr("upgrade_tool LD: %1 devices").arg(rockusb.size()));
        const QList<UsbDeviceInfo> bus = enumerate();   // at LD's result, as every snapshot
        baseline = Snapshot::from(bus, rockusb);
        int gadgets = 0;
        for (const UsbDeviceInfo& device : bus) {
            gadgets += device.isAdbGadget() ? 1 : 0;
        }
        if (everyBoard && adbEntries > gadgets) {
            refuse(FleetFlasher::tr("adb lists %1 devices but the USB bus shows %2 Rockchip ADB gadgets: a non-Rockchip ADB device "
                                    "is attached; name the boards with --serials")
                       .arg(adbEntries)
                       .arg(gadgets));
            return;
        }
        if (gadgets == 0) {
            refuse(FleetFlasher::tr("the USB enumeration shows no Rockchip ADB device although adb lists boards: port mapping "
                                    "impossible (driver?)"));
            return;
        }
        if (!checkBaselineDownloadDevices(bus)) {
            return;
        }
        logSelection();
        if (!checkRows()) {
            return;
        }
        logPlan();
        if (dry) {
            for (int i = 0; i < count(); ++i) {
                if (!at(i).ended) {
                    endBoard(i, FleetBoard::Result::Ready, QString());
                }
            }
            finishRun();
            return;
        }
        setPhase(Phase::Mapping);
        mapNext();
    }

    /// Download-mode devices at the baseline: every one is foreign. A mismatch between the bus and
    /// the listing is a driver problem; a dry run lists the foreign ones, a real run refuses.
    bool checkBaselineDownloadDevices(const QList<UsbDeviceInfo>& bus)
    {
        const QStringList ports = sortedPorts(baseline.downloadPorts.keys());
        const QStringList ids = sortedIds(baseline.loaderIds + baseline.maskromIds);
        // Which port lacks its LD line (or which id its port) cannot be told from two counts: with
        // both sides non-empty the refusal names every port and every id, never one by position.
        const QString portText = ports.join(QStringLiteral(", "));
        const QString idText = ids.join(QStringLiteral(", "));
        if (ports.size() > ids.size()) {
            QList<UsbDeviceInfo> instances;
            for (const UsbDeviceInfo& device : bus) {
                if (device.isDownloadMode() && ports.contains(device.portKey())) {
                    instances << device;
                }
            }
            const QString hint = UsbProbe::driverHint(instances);
            refuse(ids.isEmpty()
                       ? FleetFlasher::tr("a Rockchip device on port %1 is in download mode but upgrade_tool LD does not list it: %2")
                             .arg(portText, hint)
                       : FleetFlasher::tr("ports %1 are in download mode but upgrade_tool LD lists only LocationID %2: one of these "
                                          "devices is not listed; %3")
                             .arg(portText, idText, hint));
            return false;
        }
        if (ids.size() > ports.size()) {
            refuse(ports.isEmpty()
                       ? FleetFlasher::tr("upgrade_tool LD lists LocationID %1 in download mode but the USB enumeration shows no such "
                                          "device: port mapping impossible (driver?)")
                             .arg(idText)
                       : FleetFlasher::tr("upgrade_tool LD lists LocationID %1 in download mode but the USB enumeration shows only "
                                          "port %2: one of these LocationIDs has no device on the bus; port mapping impossible "
                                          "(driver?)")
                             .arg(idText, portText));
            return false;
        }
        if (ids.isEmpty()) {
            return true;
        }
        foreign = looseDevices(ids, ports, baseline);
        if (dry) {
            return true;   // listed after the boards' lines
        }
        if (foreign.size() == 1) {
            const UnattributedDevice& device = foreign.first();
            const QString mode = device.mode == QLatin1String("Maskrom") ? QStringLiteral("MASKROM") : QStringLiteral("Loader");
            refuse(device.port.isEmpty()
                       ? FleetFlasher::tr("a rockusb device is already in %1 mode (LocationID %2) that no selected board accounts "
                                          "for; reset it with `%3` or replug it, then run again")
                             .arg(mode, device.locationId, device.hint())
                       : FleetFlasher::tr("a rockusb device is already in %1 mode on port %2 (LocationID %3) that no selected "
                                          "board accounts for; reset it with `%4` or replug it, then run again")
                             .arg(mode, device.port, device.locationId, device.hint()));
            return false;
        }
        QStringList hints;
        for (const UnattributedDevice& device : std::as_const(foreign)) {
            hints << QStringLiteral("`%1`").arg(device.hint());
        }
        refuse(FleetFlasher::tr("%1 rockusb devices are already in download mode (LocationIDs %2) that no selected board accounts "
                                "for; reset them with %3 or replug them, then run again")
                   .arg(foreign.size())
                   .arg(ids.join(QStringLiteral(", ")), hints.join(QStringLiteral(", "))));
        return false;
    }

    void logSelection()
    {
        for (int i = 0; i < count(); ++i) {
            Board& board = at(i);
            if (!board.candidate) {
                endBoard(i, FleetBoard::Result::Skipped, board.pendingSkip);
                continue;
            }
            boardLog(board, board.info.expectedPort.isEmpty()
                                ? FleetFlasher::tr("on ADB; expected port unknown (first run)")
                                : FleetFlasher::tr("on ADB; expected port %1 (remembered)").arg(board.info.expectedPort));
        }
        for (const UnattributedDevice& device : std::as_const(foreign)) {
            const QString mode = device.mode == QLatin1String("Maskrom") ? QStringLiteral("MASKROM") : QStringLiteral("Loader");
            log(device.port.isEmpty()
                    ? FleetFlasher::tr("foreign %1 device, LocationID %2: not part of the fleet; reset it with `%3` or replug it")
                          .arg(mode, device.locationId, device.hint())
                    : FleetFlasher::tr("foreign %1 device on port %2, LocationID %3: not part of the fleet; reset it with `%4` or "
                                       "replug it")
                          .arg(mode, device.port, device.locationId, device.hint()));
        }
    }

    /// SocFlasher::writePartitions()'s rules, once, before any reboot.
    bool checkRows()
    {
        for (PartitionImage& row : rows) {
            const QFileInfo file(row.file);
            if (!file.isFile()) {
                refuse(FleetFlasher::tr("the image of partition %1 is missing: %2").arg(row.name, QDir::toNativeSeparators(row.file)));
                return false;
            }
            if (!row.inLayout) {
                refuse(FleetFlasher::tr("%1 is not in the board's partition layout").arg(row.name));
                return false;
            }
            const quint64 bytes = static_cast<quint64>(file.size());
            if (row.size != 0 && bytes > row.size) {
                refuse(FleetFlasher::tr("%1 (%2 bytes) does not fit partition %3 (%4 bytes)")
                           .arg(file.fileName())
                           .arg(bytes)
                           .arg(row.name)
                           .arg(row.size));
                return false;
            }
            if (row.offset % kSectorBytes != 0) {
                refuse(FleetFlasher::tr("partition %1 does not start on a 512-byte sector (offset %2)").arg(row.name).arg(row.offset));
                return false;
            }
            row.fileSize = static_cast<qint64>(bytes);
        }
        return true;
    }

    int candidateCount() const
    {
        int n = 0;
        for (const auto& board : boards) {
            n += board->candidate && !board->ended ? 1 : 0;
        }
        return n;
    }

    void logPlan()
    {
        QStringList parts;
        for (const PartitionImage& row : std::as_const(rows)) {
            parts << FleetFlasher::tr("%1 (%2 bytes, sector %3, %4 sectors)")
                         .arg(row.name)
                         .arg(row.fileSize)
                         .arg(row.offset / kSectorBytes)
                         .arg(row.size == 0 ? QStringLiteral("-1") : QString::number(row.size / kSectorBytes));
        }
        const int n = candidateCount();
        log(FleetFlasher::tr("Plan: write %1 on %2 boards, up to %3 at once")
                .arg(parts.join(QStringLiteral(", ")))
                .arg(n)
                .arg(parallel > 0 ? qMin(parallel, n) : n));
        // (v0.8.0) The profile the plan is for: the gate of step 2 b and Mapped compares with it.
        log(planProfile.known() ? FleetFlasher::tr("plan profile: %1 - %2").arg(planProfile.profile.name, planProfile.sourceText())
                                : FleetFlasher::tr("plan profile: unknown; the profile gate is off"));
    }

    /// (v0.8.0) The board's detection: logged as its "profile: ..." line when it changed (always
    /// the first time), FleetBoard::profile = its id.
    void setBoardProfile(Board& board, const ProfileDetection& detection, bool always)
    {
        const bool differs = !(detection == board.detection);
        board.detection = detection;
        board.info.profile = detection.known() ? detection.profile.id : QString();
        if (always || differs) {
            boardLog(board, detection.text());
        }
        if (differs) {
            changed(board);
        }
    }

    // =========================================================================================
    // The mapping, one board at a time
    // =========================================================================================

    void startMappingClock()
    {
        if (!mappingStarted) {
            mappingStarted = true;
            mappingClock.start();
        }
    }

    void endMappingClock()
    {
        if (mappingStarted && !mappingEnded) {
            mappingEnded = true;
            mappingSecs = mappingClock.elapsed() / 1000.0;
        }
    }

    void mapNext()
    {
        if (draining) {
            endMappingByCancel();
            return;
        }
        int next = -1;
        for (int i = mapIndex + 1; i < count(); ++i) {
            if (!at(i).ended && at(i).candidate) {
                next = i;
                break;
            }
        }
        if (next < 0) {
            reconcile();
            return;
        }
        mapIndex = next;
        readKernel(next);
    }

    /// A board the cancel caught before its reboot: nothing was done to it.
    bool skipIfDraining(int index)
    {
        if (!draining) {
            return false;
        }
        endBoard(index, FleetBoard::Result::Skipped, FleetFlasher::tr("cancelled before its reboot; not touched"));
        mapNext();
        return true;
    }

    // Step a
    void readKernel(int index)
    {
        Board& board = at(index);
        boardCommand(index, [&board] { return board.adb->shell(QStringLiteral("cat /proc/version")); }, true,
                     [this, index](const ToolResult& r) {
                         Board& b = at(index);
                         const QString kernel = r.started && !r.timedOut && r.exitCode == 0 ? kernelRelease(r.outputText()) : QString();
                         b.info.kernel = kernel;
                         boardLog(b, FleetFlasher::tr("board running %1, rebooting it into Loader mode")
                                         .arg(kernel.isEmpty() ? FleetFlasher::tr("an unknown kernel") : kernel));
                         if (!skipIfDraining(index)) {
                             readLayout(index);
                         }
                     });
    }

    // Step b: the layout, and the gate that keeps a board of another layout out of Loader mode
    void readLayout(int index)
    {
        Board& board = at(index);
        boardCommand(index, [&board] { return board.adb->shell(QStringLiteral("cat /proc/cmdline")); }, true,
                     [this, index](const ToolResult& r) { onLayout(index, r); });
    }

    void onLayout(int index, const ToolResult& r)
    {
        Board& board = at(index);
        if (!r.started || r.timedOut || r.exitCode != 0) {
            const QString tail = RockchipTool::tailForMessage(r.outputText(), 2);
            const QString reason = !r.started  ? r.error
                                   : r.timedOut ? FleetFlasher::tr("adb timed out")
                                   : tail.isEmpty() ? FleetFlasher::tr("exit code %1").arg(r.exitCode)
                                                    : tail;
            endBoard(index, FleetBoard::Result::Skipped, FleetFlasher::tr("adb could not read board %1: %2").arg(board.info.serial, reason));
            mapNext();
            return;
        }
        const QString found = RockchipTool::mtdPartsFromCommandLine(r.outputText());
        board.info.layout = found;
        if (!found.isEmpty()) {
            boardLog(board, FleetFlasher::tr("flash layout of the board: %1").arg(found));
            const quint64 id = runId;
            emit q->mtdPartsRead(board.info.serial, found);
            if (!alive(id)) {
                return;
            }
        }
        // (v0.8.0) The profile gate, before the layout gate: a board of another profile than the
        // plan's stops the run (one profile per run; never a skip, never bypassed by env).
        ProfileEvidence evidence;
        evidence.overrideId = profileOverride;
        evidence.layout = found;
        setBoardProfile(board, BoardProfiles::detectBoard(evidence), /*always=*/true);
        const QString mixed = ProfileRules::mixedBenchStop(board.info.serial, board.detection, planProfile);
        if (!mixed.isEmpty()) {
            stopMapping(index, mixed, {});
            return;
        }
        const QList<MtdPartition> boardLayout = RockchipTool::parseMtdParts(found);
        if (everyBoard && boardLayout.isEmpty()) {
            endBoard(index, FleetBoard::Result::Skipped,
                     FleetFlasher::tr("no flash layout in its /proc/cmdline: not a board this plan can place its rows on; name it "
                                      "with --serials to write it anyway"));
            mapNext();
            return;
        }
        if (!rowsIncludeEnv() && !boardLayout.isEmpty() && boardLayout != RockchipTool::parseMtdParts(planLayout)) {
            endBoard(index, FleetBoard::Result::Skipped,
                     FleetFlasher::tr("flash layout differs from the plan's (board: %1; plan: %2): not rebooted")
                         .arg(normalizedLayout(found), normalizedLayout(planLayout)));
            mapNext();
            return;
        }
        if (skipIfDraining(index)) {
            return;
        }
        // Step c: the picture before, then the reboot
        takeSnapshot([this, index](const Picture& picture) {
            if (!picture.error.isEmpty()) {
                endBoard(index, FleetBoard::Result::Skipped, FleetFlasher::tr("not rebooted: %1").arg(picture.error));
                stopMapping(-1, picture.error, {});
                return;
            }
            if (skipIfDraining(index)) {
                return;
            }
            before = picture.snapshot;
            rebootIntoLoader(index);
        });
    }

    void rebootIntoLoader(int index)
    {
        Board& board = at(index);
        startMappingClock();
        anyRebooted = true;
        board.rebooted = true;
        board.clock.start();
        board.waitClock.start();
        board.info.stage = FleetBoard::Stage::Mapping;
        changed(board);
        boardCommand(index, [&board] { return board.adb->rebootLoader(); }, true,
                     [this, index](const ToolResult&) { poll(index); });   // its exit status is ignored, as usb_update.sh does
    }

    // Step d
    void poll(int index)
    {
        takeSnapshot([this, index](const Picture& picture) {
            if (!picture.error.isEmpty()) {
                stopMapping(index, FleetFlasher::tr("while rebooting %1: %2").arg(at(index).info.serial, picture.error), {});
                return;
            }
            onVerdict(index, compare(before, picture.snapshot), picture);
        });
    }

    void onVerdict(int index, const Verdict& verdict, const Picture& picture)
    {
        Board& board = at(index);
        const QString serial = board.info.serial;
        switch (verdict.kind) {
        case Verdict::Kind::Mapped:
            confirmMapped(index, verdict, picture);
            return;
        case Verdict::Kind::Maskrom:
            board.maskrom = true;
            board.info.port = verdict.port;
            board.info.locationId = verdict.locationId;
            board.info.mappingSeconds = board.clock.elapsed() / 1000.0;
            endBoard(index, FleetBoard::Result::Failed,
                     FleetFlasher::tr("entered MASKROM instead of Loader mode: not flashed in a fleet; flash it alone with `su-fw "
                                      "flash-soc --serial %1` only once `upgrade_tool LD` lists it as the ONLY device in download "
                                      "mode (MASKROM mode writes the whole image with UF: idblock, uboot, env and meta included)")
                         .arg(serial));
            mapNext();
            return;
        case Verdict::Kind::Ambiguous:
        case Verdict::Kind::ForeignLoader:
        case Verdict::Kind::PortMismatch: {
            const QStringList appeared =
                without(sortedPorts(picture.snapshot.downloadPorts.keys()), sortedPorts(before.downloadPorts.keys()));
            stopMapping(index, FleetFlasher::tr("while rebooting %1: %2").arg(serial, verdict.detail),
                        looseDevices(verdict.newIds, appeared, picture.snapshot));
            return;
        }
        case Verdict::Kind::Pending:
            break;
        }
        if (board.waitClock.elapsed() < qint64(loaderWaitSeconds) * 1000) {
            const quint64 id = runId;
            pollTimer->disconnect();
            QObject::connect(pollTimer, &QTimer::timeout, q, [this, id, index] {
                if (alive(id)) {
                    poll(index);
                }
            });
            pollTimer->start(pollMs);
            return;
        }
        onLoaderTimeout(index, verdict, picture);
    }

    void onLoaderTimeout(int index, const Verdict& verdict, const Picture& picture)
    {
        Board& board = at(index);
        const QString serial = board.info.serial;
        switch (verdict.waiting) {
        case Verdict::Waiting::StillOnAdb:
        case Verdict::Waiting::None:
            endBoard(index, FleetBoard::Result::Skipped,
                     FleetFlasher::tr("reboot loader had no effect: board %1 is still on ADB after %2 s").arg(serial, waitText()));
            mapNext();
            return;
        case Verdict::Waiting::LoaderNotEnumerated:
            board.vanishedPort = verdict.port;
            endBoard(index, FleetBoard::Result::Skipped,
                     FleetFlasher::tr("board %1 left ADB (port %2) but no Loader device appeared within %3 s; it may be rebooting: "
                                      "check it by hand")
                         .arg(serial, verdict.port, waitText()));
            mapNext();
            return;
        case Verdict::Waiting::LdNotListing: {
            QList<UsbDeviceInfo> instances;
            for (const UsbDeviceInfo& device : picture.bus) {
                if (device.isDownloadMode() && device.portKey() == verdict.port) {
                    instances << device;
                }
            }
            stopMapping(index,
                        FleetFlasher::tr("a Loader device appeared on port %1 but upgrade_tool LD never listed it within %2 s: %3; "
                                         "board %4 is in Loader mode and must be power-cycled by hand")
                            .arg(verdict.port, waitText(), UsbProbe::driverHint(instances), serial),
                        {});
            return;
        }
        case Verdict::Waiting::GhostOnPort:
            stopMapping(index,
                        FleetFlasher::tr("a Loader device on port %1 still lists an ADB device after %2 s: cannot tell whose it is")
                            .arg(verdict.port, waitText()),
                        {});
            return;
        case Verdict::Waiting::LdOnly:
            stopMapping(index,
                        FleetFlasher::tr("upgrade_tool LD lists LocationID %1 but no download-mode device appeared on the bus within "
                                         "%2 s: cannot tell whose it is; board %3 is still on ADB")
                            .arg(verdict.locationId, waitText(), serial),
                        looseDevices(verdict.newIds, {}, picture.snapshot));
            return;
        }
    }

    /// Mapped by the picture; the one cross-check the enumeration cannot make: X must be gone from
    /// `adb devices`, else the Loader device that appeared is another board's.
    void confirmMapped(int index, const Verdict& verdict, const Picture& picture)
    {
        const quint64 id = runId;
        ToolInvocation* invocation = trackRun(adb->devices());
        runProcess = invocation;
        QObject::connect(invocation, &ToolInvocation::finished, q, [this, id, index, verdict, picture](const ToolResult& r) {
            if (!alive(id)) {
                return;
            }
            runProcess.clear();
            Board& board = at(index);
            const QString serial = board.info.serial;
            const QList<UnattributedDevice> loose = looseDevices(verdict.newIds, {verdict.port}, picture.snapshot);
            if (!r.started || r.timedOut) {
                stopMapping(index,
                            FleetFlasher::tr("while rebooting %1: adb could not confirm that the board left ADB (%2)")
                                .arg(serial, r.started ? FleetFlasher::tr("adb devices timed out") : r.error),
                            loose);
                return;
            }
            for (const AdbDevice& device : AdbTool::parseDevices(r.outputText())) {
                if (device.serial == serial) {
                    stopMapping(index,
                                FleetFlasher::tr("board %1 is still on ADB although a Loader device appeared on port %2: not this board")
                                    .arg(serial, verdict.port),
                                loose);
                    return;
                }
            }
            board.info.port = verdict.port;
            board.info.locationId = verdict.locationId;
            board.mappedPid = picture.snapshot.downloadPorts.value(verdict.port);   // its Loader mode, for every re-check
            board.info.stage = FleetBoard::Stage::Loader;
            board.info.mappingSeconds = board.clock.elapsed() / 1000.0;
            changed(board);
            emit q->portLearned(serial, verdict.port);
            if (!alive(id)) {
                return;
            }
            QString line = FleetFlasher::tr("in Loader mode on port %1, LocationID %2 (%3 s)")
                               .arg(verdict.port, verdict.locationId, oneDecimal(board.info.mappingSeconds));
            if (!board.info.expectedPort.isEmpty() && board.info.expectedPort != verdict.port) {
                line += FleetFlasher::tr(" (moved: expected %1)").arg(board.info.expectedPort);
            }
            boardLog(board, line);
            if (!alive(id)) {
                return;
            }
            // (v0.8.0) The second run of the profile gate (step 2 d): the board's Loader PID - the
            // pid its port showed when mapped - confirms its profile, or tells it when its
            // /proc/cmdline did not. Before the next board is rebooted; nothing was written.
            ProfileEvidence evidence;
            evidence.overrideId = profileOverride;
            evidence.loaderPid = board.mappedPid;
            evidence.layout = board.info.layout;
            setBoardProfile(board, BoardProfiles::detectBoard(evidence), /*always=*/false);
            QString stop = ProfileRules::overrideRefusal(profileOverride, board.mappedPid);
            if (stop.isEmpty()) {
                stop = ProfileRules::mixedBenchStop(serial, board.detection, planProfile);
            }
            if (!stop.isEmpty()) {
                stopMapping(index, stop, {});   // this board is mapped: it is reset with the others
                return;
            }
            mapNext();
        });
    }

    /// A stop: nothing was written. The board being mapped fails, the later ones are skipped, every
    /// mapped board is reset with its own address, and an id nobody owns is reported, never reset.
    void stopMapping(int failing, const QString& reason, const QList<UnattributedDevice>& loose)
    {
        pollTimer->stop();
        endMappingClock();
        canClose = true;
        stopText = kMappingStopped + reason;
        log(stopText);
        for (const UnattributedDevice& device : loose) {
            addUnattributed(device);
        }
        if (failing >= 0 && !at(failing).ended) {
            if (at(failing).info.stage == FleetBoard::Stage::Loader) {
                // (v0.8.0) Stopped at its Mapped (the profile gate's second run): it is in Loader
                // mode with an id the run learned, so it gets its RD like every mapped board and
                // ends Failed with the reason.
                at(failing).resetOnly = true;
                resetUnwritten(failing, reason, reason, FleetFlasher::tr("mapping stopped: resetting the board back to ADB"),
                               FleetBoard::Result::Failed);
            } else {
                endBoard(failing, FleetBoard::Result::Failed, reason);
            }
        }
        const QString skipped = kMappingStopped + reason;
        for (int i = 0; i < count(); ++i) {
            Board& board = at(i);
            if (board.ended || i == failing) {
                continue;
            }
            if (board.info.stage == FleetBoard::Stage::Loader) {
                resetUnwritten(i, skipped, skipped, FleetFlasher::tr("mapping stopped: resetting the board back to ADB"));
            } else {
                endBoard(i, FleetBoard::Result::Skipped, skipped);
            }
        }
        maybeFinishRun();
    }

    /// The cancel reached the mapping: the board in flight has had its wait; nothing more is
    /// rebooted, every mapped board is reset.
    void endMappingByCancel()
    {
        pollTimer->stop();
        endMappingClock();
        canClose = true;
        for (int i = 0; i < count(); ++i) {
            Board& board = at(i);
            if (board.ended) {
                continue;
            }
            if (board.info.stage == FleetBoard::Stage::Loader) {
                resetCancelled(i);
            } else {
                endBoard(i, FleetBoard::Result::Skipped, FleetFlasher::tr("cancelled before its reboot; not touched"));
            }
        }
        maybeFinishRun();
    }

    /// Step e: before "mapping done", one more picture; every download-mode device in it must be a
    /// mapped board, a MASKROM board or the baseline's. Anything else arrived late and stops the run.
    void reconcile()
    {
        if (!anyRebooted) {
            endMappingClock();
            canClose = true;
            finishRun();   // nobody was rebooted: nothing to reconcile, nothing to write
            return;
        }
        takeSnapshot([this](const Picture& picture) {
            if (draining) {
                endMappingByCancel();
                return;
            }
            if (!picture.error.isEmpty()) {
                stopMapping(-1, picture.error, {});
                return;
            }
            QStringList knownPorts = sortedPorts(baseline.downloadPorts.keys());
            QStringList knownIds = baseline.loaderIds + baseline.maskromIds;
            int mapped = 0;
            int maskrom = 0;
            for (const auto& board : boards) {
                if (board->info.stage == FleetBoard::Stage::Loader && !board->ended) {
                    ++mapped;
                } else if (board->maskrom) {
                    ++maskrom;
                } else {
                    continue;
                }
                knownPorts << board->info.port;
                knownIds << board->info.locationId;
            }
            const QStringList latePorts = without(sortedPorts(picture.snapshot.downloadPorts.keys()), knownPorts);
            const QStringList lateIds = without(sortedIds(picture.snapshot.loaderIds + picture.snapshot.maskromIds), knownIds);
            if (!latePorts.isEmpty() || !lateIds.isEmpty()) {
                onLateArrival(latePorts, lateIds, picture);
                return;
            }
            const int devices = static_cast<int>(picture.snapshot.loaderIds.size() + picture.snapshot.maskromIds.size());
            log(maskrom == 0 ? FleetFlasher::tr("reconciled: %1 devices in download mode, every one a mapped board").arg(devices)
                             : FleetFlasher::tr("reconciled: %1 devices in download mode: %2 mapped boards, %3 in MASKROM mode (not "
                                                "written)")
                                   .arg(devices)
                                   .arg(mapped)
                                   .arg(maskrom));
            endMappingClock();
            canClose = true;
            if (mapped == 0) {
                finishRun();   // this picture was the run's closing one
                return;
            }
            log(FleetFlasher::tr("mapping done: %1 of %2 boards in Loader mode; writing starts (up to %3 at once)")
                    .arg(mapped)
                    .arg(count())
                    .arg(parallel > 0 ? parallel : count()));
            setPhase(Phase::Writing);
            fill();
        });
    }

    void onLateArrival(const QStringList& latePorts, const QStringList& lateIds, const Picture& picture)
    {
        QString where;
        const QString portText = latePorts.join(QStringLiteral(", "));
        const QString idText = lateIds.join(QStringLiteral(", "));
        if (!latePorts.isEmpty() && !lateIds.isEmpty()) {
            where = FleetFlasher::tr("on port %1 (LocationID %2)").arg(portText, idText);
        } else if (!latePorts.isEmpty()) {
            where = FleetFlasher::tr("on port %1 (no LocationID yet)").arg(portText);
        } else {
            where = FleetFlasher::tr("(LocationID %1)").arg(idText);
        }
        // The board that left ADB without a Loader device in time is in Loader mode now: say so.
        for (int i = 0; i < count(); ++i) {
            Board& board = at(i);
            if (!board.vanishedPort.isEmpty() && latePorts.contains(board.vanishedPort)) {
                board.info.detail += lateIds.size() == 1
                                         ? FleetFlasher::tr("; it is in Loader mode now (port %1, LocationID %2)").arg(board.vanishedPort, lateIds.first())
                                         : FleetFlasher::tr("; it is in Loader mode now (port %1)").arg(board.vanishedPort);
                changed(board);
            }
        }
        stopMapping(-1, FleetFlasher::tr("a Loader device appeared late %1 and belongs to no mapped board").arg(where),
                    looseDevices(lateIds, latePorts, picture.snapshot));
    }

    /// `-s <id> RD` for a mapped board that was never written (a stop, a cancel): Skipped with
    /// `doneDetail` when the RD answered OK, else `failBase` and what went wrong. Waits for the
    /// gate's LD when one is in flight. (v0.8.0) `result` is Failed for the board a stop was
    /// decided at its Mapped (the profile gate): it still gets its RD and counts as reset.
    void resetUnwritten(int index, const QString& doneDetail, const QString& failBase, const QString& line,
                        FleetBoard::Result result = FleetBoard::Result::Skipped)
    {
        Board& board = at(index);
        board.info.stage = FleetBoard::Stage::Resetting;
        board.checkQueued = false;
        gateWaiting.removeAll(index);
        gateNext.removeAll(index);
        releaseSlot(board);
        changed(board);
        boardLog(board, line);
        const quint64 id = runId;
        whenGateFree([this, id, index, doneDetail, failBase, result] {
            if (!alive(id)) {
                return;
            }
            Board& b = at(index);
            const QString n = b.info.locationId;
            ++inFlightUpgradeTools;
            boardCommand(index, [this, n] { return upgradeTool->resetDeviceOn(n); }, true,
                         [this, index, doneDetail, failBase, n, result](const ToolResult& r) {
                             --inFlightUpgradeTools;
                             const QString output = r.outputText();
                             Board& b2 = at(index);
                             if (r.started && RockchipTool::reportsNoSuchDevice(output)) {
                                 // The id is gone (exit 255): nothing was reset, and no RD line can address it.
                                 const PortView view = portView(b2.info.port);
                                 b2.info.leftInLoader = view.pid != 0 && view.pid == b2.mappedPid;
                                 b2.info.retry.clear();
                                 endBoard(index, result, failBase + goneHint(n),
                                          {FleetFlasher::tr("port %1 shows %2").arg(b2.info.port, shownOnPort(view, b2.mappedPid))});
                             } else if (r.started && !r.timedOut && (r.exitCode == 0 || RockchipTool::reportsResetOk(output))) {
                                 ++resetCount;
                                 b2.resetOk = true;
                                 boardLog(b2, FleetFlasher::tr("reset back to ADB; nothing was written"));
                                 endBoard(index, result, doneDetail);
                             } else {
                                 const QString tail = !r.started  ? r.error
                                                      : r.timedOut ? FleetFlasher::tr("timed out")
                                                                   : RockchipTool::tailForMessage(output);
                                 b2.info.leftInLoader = true;
                                 b2.info.retry = {retryRdLine(n)};
                                 endBoard(index, result,
                                          FleetFlasher::tr("%1; upgrade_tool RD did not reset it (%2): reset it by hand with `%3`")
                                              .arg(failBase, tail.isEmpty() ? FleetFlasher::tr("no output") : tail, rdHint(n)),
                                          retryLogLines(b2.info.retry));
                             }
                             afterProcess();
                         });
        });
    }

    /// A mapped board the cancel caught before its first WL: reset, Skipped.
    void resetCancelled(int index)
    {
        resetUnwritten(index, FleetFlasher::tr("cancelled before its write started; reset back to ADB"),
                       FleetFlasher::tr("cancelled before its write started"), FleetFlasher::tr("cancelled: resetting the board back to ADB"));
    }

    /// "; upgrade_tool RD found no device with LocationID n (it is gone): ..." - an RD answered
    /// "No found specific rockusb device!": the id names nothing any more.
    static QString goneHint(const QString& locationId)
    {
        return FleetFlasher::tr("; upgrade_tool RD found no device with LocationID %1 (it is gone): find its LocationID with "
                                "`upgrade_tool LD` before any retry, and address it with -s")
            .arg(locationId);
    }

    // =========================================================================================
    // The writing: the scheduler, the re-checks and the LD gate
    // =========================================================================================

    int slotsInUse() const
    {
        int used = 0;
        for (const auto& board : boards) {
            used += board->inSlot ? 1 : 0;
        }
        return used;
    }

    /// Starts mapped boards while slots are free (0 = every board at once) and nothing drains.
    void fill()
    {
        if (!running || draining || closing || phase == Phase::Selecting || phase == Phase::Mapping) {
            maybeFinishRun();
            return;
        }
        const quint64 id = runId;
        for (int i = 0; i < count() && alive(id) && !draining; ++i) {
            Board& board = at(i);
            if (board.ended || board.inSlot || board.info.stage != FleetBoard::Stage::Loader) {
                continue;
            }
            if (parallel > 0 && slotsInUse() >= parallel) {
                break;
            }
            board.inSlot = true;
            board.info.stage = FleetBoard::Stage::Writing;
            board.row = 0;
            if (!writingStarted) {
                writingStarted = true;
                writingClock.start();
            }
            board.writeClock.start();
            changed(board);
            requestCheck(i, false);
        }
        if (alive(id)) {
            maybeFinishRun();
        }
    }

    /// The re-check before a WL / the RD (the class comment, step 3): the enumeration always; the
    /// gate's LD whenever no WL / RD of the run is in flight (BeforeEveryWrite: always).
    void requestCheck(int index, bool beforeRd)
    {
        Board& board = at(index);
        board.checkBeforeRd = beforeRd;
        if (policy == LdPolicy::Gate) {
            if (inFlightUpgradeTools > 0 && !gateLdBusy) {
                evaluateCheck(index, nullptr, QString(), inFlightUpgradeTools);
                return;
            }
            if (!gateLdBusy && gateListingValid && gateListingAge.elapsed() < kGateReuseMs) {
                const QList<RockusbDevice> listing = gateListing;
                evaluateCheck(index, &listing, QString(), 0);
                return;
            }
            board.checkQueued = true;
            gateWaiting << index;
            if (!gateLdBusy) {
                startGateLd();
            }
            return;
        }
        board.checkQueued = true;
        if (gateLdBusy) {
            gateNext << index;   // an LD that started before this check asked does not answer it
        } else {
            gateWaiting << index;
            startGateLd();
        }
    }

    void startGateLd()
    {
        gateLdBusy = true;   // before the process exists: a slot of the runner may cancel the run
        const quint64 id = runId;
        ToolInvocation* invocation = trackRun(upgradeTool->listDevices());
        gateLd = invocation;
        QObject::connect(invocation, &ToolInvocation::finished, q, [this, id](const ToolResult& r) {
            if (alive(id)) {
                onGateLd(r);
            }
        });
    }

    void onGateLd(const ToolResult& r)
    {
        gateLdBusy = false;
        gateLd.clear();
        const QString error = !r.started ? r.error : r.timedOut ? FleetFlasher::tr("timed out") : QString();
        QList<RockusbDevice> listing;
        if (error.isEmpty()) {
            listing = RockchipTool::parseDeviceList(r.outputText());
            gateListing = listing;
            gateListingValid = true;
            gateListingAge.start();
        } else {
            gateListingValid = false;
        }
        const quint64 id = runId;
        const QList<int> answered = gateWaiting;
        gateWaiting.clear();
        for (const int index : answered) {
            if (!alive(id)) {
                return;
            }
            Board& board = at(index);
            if (board.ended || !board.checkQueued) {
                continue;
            }
            board.checkQueued = false;
            evaluateCheck(index, error.isEmpty() ? &listing : nullptr, error, 0);
        }
        if (!alive(id)) {
            return;
        }
        const QList<std::function<void()>> launches = afterGateLd;
        afterGateLd.clear();
        for (const auto& launch : launches) {
            whenGateFree(launch);   // a check answered above may have started the next LD already
            if (!alive(id)) {
                return;
            }
        }
        if (!gateNext.isEmpty() && !gateLdBusy) {
            gateWaiting = gateNext;
            gateNext.clear();
            startGateLd();
        }
        maybeFinishRun();
    }

    /// Runs `launch` (a WL / RD start) now, or right after the gate's LD when one is in flight:
    /// no WL / RD ever starts next to the LD that opens every rockusb device.
    void whenGateFree(std::function<void()> launch)
    {
        if (gateLdBusy) {
            afterGateLd << std::move(launch);
        } else {
            launch();
        }
    }

    /// The evidence of one re-check: `listing` null = no LD ran (`ldError` set: it could not run;
    /// else `writesInFlight` WL / RD kept it from running).
    void evaluateCheck(int index, const QList<RockusbDevice>* listing, const QString& ldError, int writesInFlight)
    {
        Board& board = at(index);
        if (board.ended) {
            return;
        }
        const QString port = board.info.port;
        const QString n = board.info.locationId;
        if (!ldError.isEmpty()) {
            onGateLdFailed(index, ldError);
            return;
        }
        // Mid-wave this is the only evidence: the pid of its Loader mode, not any download pid
        // (a MASKROM pid on the port would take an addressed WL that cannot work).
        const PortView view = portView(port);
        const QString shown = shownOnPort(view, board.mappedPid);
        const bool portOk = inLoaderByPort(view, board.mappedPid);
        bool ldOk = true;
        QString ldText;
        if (listing) {
            const std::optional<RockusbDevice> device = RockchipTool::deviceAt(*listing, n);
            ldOk = device && device->mode == RockusbDevice::Mode::Loader;
            ldText = ldOk ? FleetFlasher::tr("LD lists LocationID %1").arg(n)
                     : device ? FleetFlasher::tr("LD lists LocationID %1 in MASKROM mode").arg(n)
                              : FleetFlasher::tr("LD does not list LocationID %1").arg(n);
        } else {
            ldText = FleetFlasher::tr("LD not run: %1 writes in flight").arg(writesInFlight);
        }
        if (portOk && ldOk) {
            boardLog(board, listing ? FleetFlasher::tr("checked: LD lists LocationID %1 in Loader mode; port %2 shows %3").arg(n, port, shown)
                                    : FleetFlasher::tr("checked: port %1 shows %2 (%3)").arg(port, shown, ldText));
            const quint64 id = runId;
            whenGateFree([this, id, index] {
                if (!alive(id) || at(index).ended) {
                    return;
                }
                if (draining && !at(index).started) {
                    // Answered after the cancel (it waited for the gate's LD): never started, so reset.
                    resetCancelled(index);
                    return;
                }
                if (at(index).checkBeforeRd) {
                    startReset(index);
                } else {
                    startWrite(index);
                }
            });
            return;
        }
        const QString now = FleetFlasher::tr("port %1 shows %2; %3").arg(port, shown, ldText);
        if (board.checkBeforeRd) {
            board.leftEvidenceNow = now;
            board.leftEvidencePast = FleetFlasher::tr("port %1 showed %2; %3").arg(port, shown, ldText);
            onLeftBeforeReset(index);
            return;
        }
        endBoard(index, FleetBoard::Result::Failed,
                 FleetFlasher::tr("left Loader mode before %1 was written (%2): nothing written for it").arg(rows.at(board.row).name, now));
        afterProcess();
    }

    /// A gate LD that could not run is no evidence of anything: the board may well be in Loader mode.
    void onGateLdFailed(int index, const QString& error)
    {
        Board& board = at(index);
        board.info.leftInLoader = true;
        if (board.checkBeforeRd) {
            board.info.writingSeconds = board.writeClock.elapsed() / 1000.0;
            board.info.retry = {retryRdLine(board.info.locationId)};
            endBoard(index, FleetBoard::Result::Failed,
                     FleetFlasher::tr("partitions written, but upgrade_tool LD could not run before the reset: %1; reset it by hand "
                                      "with `%2`")
                         .arg(error, rdHint(board.info.locationId)),
                     retryLogLines(board.info.retry));
        } else {
            board.info.retry = retryLines(board, board.row);
            endBoard(index, FleetBoard::Result::Failed,
                     FleetFlasher::tr("upgrade_tool LD could not run before %1: %2; nothing written for it")
                         .arg(rows.at(board.row).name, error),
                     QStringList{loaderHint(board)} + retryLogLines(board.info.retry));
        }
        afterProcess();
    }

    /// Every row is on the board, but it is not in Loader mode by the evidence: no RD; the ADB wait
    /// decides between a done board (it came back by itself) and one that is not back.
    void onLeftBeforeReset(int index)
    {
        Board& board = at(index);
        board.leftBeforeReset = true;
        board.info.writingSeconds = board.writeClock.elapsed() / 1000.0;
        releaseSlot(board);
        if (!waitAdb) {
            endBoard(index, FleetBoard::Result::NotBack, leftBeforeResetNotBack(board));
            afterProcess();
            return;
        }
        enterAdbWait(index);
        afterProcess();
    }

    QString leftBeforeResetNotBack(const Board& board) const
    {
        return FleetFlasher::tr("partitions written, but the board left Loader mode before its reset (%1): not reset by the fleet; "
                                "check it by hand")
            .arg(board.leftEvidenceNow);
    }

    // ---- WL -----------------------------------------------------------------------------------

    QString workingFolder(const Board& board) const
    {
        return QFileInfo(rows.first().file).absolutePath() + QStringLiteral("/.fleet/") + board.info.serial;
    }

    int overallPercent(const Board& board, int filePercent) const
    {
        if (totalBytes == 0) {
            return 100;
        }
        quint64 done = board.bytesDone;
        if (board.row < rows.size() && filePercent > 0) {
            done += static_cast<quint64>(rows.at(board.row).fileSize) * static_cast<quint64>(qMin(filePercent, 100)) / 100;
        }
        // Rounded, so a row's "(20%)" reads 20 % of the board's bytes, not 19.
        return static_cast<int>(qMin<quint64>(100, (done * 200 + totalBytes) / (2 * totalBytes)));
    }

    QString writeStage(const Board& board) const
    {
        if (board.verifying) {
            return FleetFlasher::tr("Verifying %1 (%2 of %3)").arg(rows.at(board.row).name).arg(board.row + 1).arg(rows.size());
        }
        return FleetFlasher::tr("Writing %1 (%2 of %3)").arg(rows.at(board.row).name).arg(board.row + 1).arg(rows.size());
    }

    void reportProgress(int index, int filePercent)
    {
        Board& board = at(index);
        // (v0.8.0) A read-back reports the row's own percentage, a write the board's bytes.
        const int percent = board.verifying ? qBound(0, filePercent, 100) : overallPercent(board, filePercent);
        if (percent != board.lastPercent) {
            board.lastPercent = percent;
            emit q->boardProgress(board.info, writeStage(board), percent);
        }
    }

    void startWrite(int index)
    {
        Board& board = at(index);
        const PartitionImage row = rows.at(board.row);
        const QString folder = workingFolder(board);
        if (!QDir().mkpath(folder)) {
            board.info.leftInLoader = true;
            board.info.retry = retryLines(board, board.row);
            endBoard(index, FleetBoard::Result::Failed,
                     FleetFlasher::tr("cannot create the working folder %1: nothing written for it").arg(QDir::toNativeSeparators(folder)),
                     QStringList{loaderHint(board)} + retryLogLines(board.info.retry));
            afterProcess();
            return;
        }
        boardLog(board, FleetFlasher::tr("writing %1 (%2 of %3): %4 (%5 bytes) at sector %6, partition of %7 sectors")
                            .arg(row.name)
                            .arg(board.row + 1)
                            .arg(rows.size())
                            .arg(QFileInfo(row.file).fileName())
                            .arg(row.fileSize)
                            .arg(row.offset / kSectorBytes)
                            .arg(row.size == 0 ? QStringLiteral("-1") : QString::number(row.size / kSectorBytes)));
        if (verify) {
            board.lastPercent = -1;   // (v0.8.0) the stage changes after a read-back: its first value goes out
        }
        reportProgress(index, 0);
        board.parser = RockchipTool::ProgressParser();
        board.lineBuffer.clear();
        board.rowClock.start();
        board.lastOutput.start();
        board.silenceTimer->start(silenceMs);
        board.started = true;
        ++inFlightUpgradeTools;   // before the process exists: the gate must already see it
        const QString n = board.info.locationId;
        boardCommand(index, [this, n, row, folder] { return upgradeTool->writeLbaOn(n, row.offset / kSectorBytes, row.size / kSectorBytes, row.file, folder); },
                     true, [this, index](const ToolResult& r) { onWriteFinished(index, r); });
        if (Board& b = at(index); b.process) {
            const quint64 id = runId;
            QObject::connect(b.process, &ToolInvocation::outputReceived, q, [this, id, index](const QByteArray& chunk) {
                if (alive(id)) {
                    onWriteOutput(index, chunk);
                }
            });
        }
    }

    void onWriteOutput(int index, const QByteArray& chunk)
    {
        Board& board = at(index);
        board.lastOutput.restart();
        board.silenceTimer->start(silenceMs);
        for (const RockchipTool::Progress& progress : board.parser.feed(chunk)) {
            if (progress.percent >= 0) {
                reportProgress(index, progress.percent);
            }
        }
        forwardToolLines(index, chunk);
    }

    void forwardToolLines(int index, const QByteArray& chunk)
    {
        Board& board = at(index);
        board.lineBuffer.append(chunk);
        qsizetype begin = 0;
        for (qsizetype i = 0; i < board.lineBuffer.size(); ++i) {
            const char c = board.lineBuffer.at(i);
            if (c == '\r' || c == '\n') {
                emitToolLine(board, board.lineBuffer.mid(begin, i - begin));
                begin = i + 1;
            }
        }
        board.lineBuffer.remove(0, begin);
    }

    void flushToolLines(Board& board)
    {
        emitToolLine(board, board.lineBuffer);
        board.lineBuffer.clear();
    }

    void emitToolLine(Board& board, const QByteArray& raw)
    {
        const QString line = QString::fromUtf8(raw);
        if (!line.trimmed().isEmpty()) {
            emit q->toolOutput(board.info, line);
        }
    }

    /// Log only: a WL is never stopped by the flasher (a half-written flash needs MASKROM recovery).
    void onSilence(int index)
    {
        Board& board = at(index);
        if (board.ended || !board.busy || board.info.stage != FleetBoard::Stage::Writing) {
            return;
        }
        const qint64 minutes = board.lastOutput.elapsed() / 60000;
        qCWarning(lcFirmware).noquote() << board.info.label() << "upgrade_tool silent for" << minutes << "min";
        boardLog(board, FleetFlasher::tr("upgrade_tool has printed nothing for %1 min; it is left running, because stopping it half "
                                         "way needs MASKROM recovery (if it hangs for good, end the upgrade_tool process by hand)")
                            .arg(minutes));
        board.silenceTimer->start(silenceMs);
    }

    void onWriteFinished(int index, const ToolResult& r)
    {
        --inFlightUpgradeTools;
        Board& board = at(index);
        board.silenceTimer->stop();
        for (const RockchipTool::Progress& progress : board.parser.flush()) {
            if (progress.percent >= 0) {
                reportProgress(index, progress.percent);
            }
        }
        flushToolLines(board);
        const PartitionImage row = rows.at(board.row);
        const QString output = r.outputText();
        if (r.started && RockchipTool::reportsNoSuchDevice(output)) {
            onNoSuchDevice(index);
            return;
        }
        if (!r.started && r.error == unaddressedRefusal()) {
            board.info.leftInLoader = true;   // in Loader mode, untouched: nothing ran
            endBoard(index, FleetBoard::Result::Failed, r.error);
            afterProcess();
            return;
        }
        if (!r.started || r.timedOut || r.exitCode != 0 || RockchipTool::reportsWriteFailure(output)) {
            const QString tail = !r.started ? r.error : RockchipTool::tailForMessage(output);
            board.info.leftInLoader = true;
            board.info.writingSeconds = board.writeClock.elapsed() / 1000.0;
            board.info.retry = retryLines(board, board.row);
            endBoard(index, FleetBoard::Result::Failed,
                     FleetFlasher::tr("writing %1 failed: %2")
                         .arg(row.name, tail.isEmpty() ? FleetFlasher::tr("no output (exit code %1)").arg(r.exitCode) : tail),
                     QStringList{loaderHint(board)} + retryLogLines(board.info.retry));
            afterProcess();
            return;
        }
        // (v0.8.0) With verify the row counts as written only once its read-back equalled the image
        // (a failed verify leaves it out of `written`, so the retry lines start with it).
        if (!verify) {
            board.info.written << row.name;
        }
        board.bytesDone += static_cast<quint64>(row.fileSize);
        const qint64 ms = board.rowClock.elapsed();
        changed(board);
        const quint64 id = runId;
        emit q->partitionWritten(board.info, row.name, row.fileSize, ms);
        if (!alive(id)) {
            return;
        }
        boardLog(board, FleetFlasher::tr("%1 written: %2 bytes in %3 s").arg(row.name).arg(row.fileSize).arg(oneDecimal(ms / 1000.0)));
        if (verify) {
            // The board's next process: the RL never runs next to its own WL (one process per
            // board), and never next to the gate's LD.
            whenGateFree([this, id, index] {
                if (alive(id) && !at(index).ended) {
                    startVerify(index);
                }
            });
            return;
        }
        ++board.row;
        requestCheck(index, board.row >= rows.size());
        if (alive(id)) {
            fill();
        }
    }

    // ---- RL: verify after write (v0.8.0, verify-rl) ----------------------------------------------

    /// `upgrade_tool -s <id> RL <sector> <ceil(bytes / 512)> <the board's folder>/<name>.img` in the
    /// board's working folder, as its one process; it counts as an in-flight upgrade_tool for the LD
    /// gate. Like a WL: no timeout, never cancelled (a drain lets it finish), the silence line.
    void startVerify(int index)
    {
        Board& board = at(index);
        const PartitionImage row = rows.at(board.row);
        const QString folder = workingFolder(board);
        const QString temp = folder + QLatin1Char('/') + row.name + QStringLiteral(".img");
        const quint64 sector = row.offset / kSectorBytes;
        const quint64 sectors = qMax<quint64>(1, (static_cast<quint64>(row.fileSize) + kSectorBytes - 1) / kSectorBytes);
        const QString n = board.info.locationId;
        boardLog(board, FleetFlasher::tr("verifying %1: reading %2 sectors back from sector %3 (upgrade_tool -s %4 RL)")
                            .arg(row.name)
                            .arg(sectors)
                            .arg(sector)
                            .arg(n));
        ReadBackResult result;
        result.name = row.name;
        result.bytes = row.fileSize;
        // A file kept from an earlier failed verify is never compared as this read's.
        if (!QDir().mkpath(folder) || (QFileInfo::exists(temp) && !QFile::remove(temp))) {
            result.error = FleetFlasher::tr("cannot write %1").arg(QDir::toNativeSeparators(temp));
            failVerify(index, result, verifyFailedText(row.name, result.error));
            return;
        }
        board.verifying = true;
        board.lastPercent = -1;
        reportProgress(index, 0);
        board.parser = RockchipTool::ProgressParser();
        board.lineBuffer.clear();
        board.verifyClock.start();
        board.lastOutput.start();
        board.silenceTimer->start(silenceMs);
        ++inFlightUpgradeTools;   // before the process exists: the gate must already see it
        boardCommand(index, [this, n, sector, sectors, temp, folder] { return upgradeTool->readLbaOn(n, sector, sectors, temp, folder); },
                     true, [this, index, temp](const ToolResult& r) { onVerifyFinished(index, r, temp); });
        if (Board& b = at(index); b.process) {
            const quint64 id = runId;
            QObject::connect(b.process, &ToolInvocation::outputReceived, q, [this, id, index](const QByteArray& chunk) {
                if (alive(id)) {
                    onWriteOutput(index, chunk);
                }
            });
        }
    }

    static QString verifyFailedText(const QString& name, const QString& why)
    {
        return FleetFlasher::tr("verifying %1 failed: %2").arg(name, why);
    }

    void onVerifyFinished(int index, const ToolResult& r, const QString& temp)
    {
        --inFlightUpgradeTools;
        Board& board = at(index);
        board.silenceTimer->stop();
        for (const RockchipTool::Progress& progress : board.parser.flush()) {
            if (progress.percent >= 0) {
                reportProgress(index, progress.percent);
            }
        }
        flushToolLines(board);
        board.verifying = false;
        const PartitionImage row = rows.at(board.row);
        ReadBackResult result;
        result.name = row.name;
        result.bytes = row.fileSize;
        result.milliseconds = board.verifyClock.elapsed();
        const QString output = r.outputText();
        if (r.started && RockchipTool::reportsNoSuchDevice(output)) {
            onVerifyNoSuchDevice(index, result);
            return;
        }
        if (!r.started && r.error == unaddressedRefusal()) {
            result.error = r.error;   // nothing ran: the board sits in Loader mode untouched by it
            failVerify(index, result, verifyFailedText(row.name, r.error));
            return;
        }
        if (!r.started || r.timedOut || r.exitCode != 0 || RockchipTool::reportsWriteFailure(output)) {
            const QString tail = RockchipTool::tailForMessage(output);
            result.error = !r.started      ? FleetFlasher::tr("cannot run upgrade_tool: %1").arg(r.error)
                           : tail.isEmpty() ? FleetFlasher::tr("no output (exit code %1)").arg(r.exitCode)
                                            : tail;
            failVerify(index, result, verifyFailedText(row.name, result.error));
            return;
        }
        ReadBackResult compared = RockchipTool::compareReadBack(row.file, temp);
        compared.name = row.name;
        compared.milliseconds = result.milliseconds;
        if (compared.ok) {
            QFile::remove(temp);   // served; kept only after a failure, for a look
            board.info.written << row.name;
            board.info.verified << row.name;
            changed(board);
            boardLog(board, FleetFlasher::tr("%1 verified: %2 bytes read back equal the image (%3 s)")
                                .arg(row.name)
                                .arg(compared.bytes)
                                .arg(oneDecimal(compared.milliseconds / 1000.0)));
            const quint64 id = runId;
            emit q->partitionVerified(board.info, compared);
            if (!alive(id)) {
                return;
            }
            ++board.row;
            requestCheck(index, board.row >= rows.size());   // the next row's re-check first, as without verify
            if (alive(id)) {
                fill();
            }
            return;
        }
        const qint64 got = QFileInfo(temp).isFile() ? QFileInfo(temp).size() : -1;
        QString text;
        if (compared.mismatchOffset >= 0) {
            compared.error = FleetFlasher::tr("byte %1 of the image differs (flash offset 0x%2): read 0x%3, image 0x%4")
                                 .arg(compared.mismatchOffset)
                                 .arg(QString::number(row.offset + static_cast<quint64>(compared.mismatchOffset), 16).toUpper())
                                 .arg(static_cast<uint>(compared.readByte), 2, 16, QLatin1Char('0'))
                                 .arg(static_cast<uint>(compared.imageByte), 2, 16, QLatin1Char('0'));
            text = verifyFailedText(row.name, compared.error);
        } else if (got >= 0 && got < compared.bytes) {
            text = verifyFailedText(row.name, FleetFlasher::tr("upgrade_tool RL read %1 of %2 bytes (short read)").arg(got).arg(compared.bytes));
        } else {
            text = verifyFailedText(row.name, compared.error);
        }
        failVerify(index, compared, text);
    }

    /// A failed verify is a failed WL (decision 5): no RD, leftInLoader, the row not in `written`,
    /// the "left in Loader mode ..." line and the addressed retry lines - that row first - so the
    /// owner's recipe for a failed write covers it; the fleet never writes the row again.
    void failVerify(int index, ReadBackResult result, const QString& detail)
    {
        Board& board = at(index);
        result.ok = false;
        board.verifying = false;
        board.info.verifyFailed = result.name;
        board.info.verifyMismatch = result.mismatchOffset;
        board.info.leftInLoader = true;
        board.info.writingSeconds = board.writeClock.elapsed() / 1000.0;
        board.info.retry = retryLines(board, board.row);
        const quint64 id = runId;
        emit q->partitionVerified(board.info, result);
        if (!alive(id)) {
            return;
        }
        endBoard(index, FleetBoard::Result::Failed, detail, QStringList{loaderHint(board)} + retryLogLines(board.info.retry));
        afterProcess();
    }

    /// "No found specific rockusb device!" from the RL: the id is gone, so no addressed retry line
    /// can be given (as onNoSuchDevice() for a WL).
    void onVerifyNoSuchDevice(int index, ReadBackResult result)
    {
        Board& board = at(index);
        const QString port = board.info.port;
        const PortView view = portView(port);
        result.ok = false;
        result.error = FleetFlasher::tr("No found specific rockusb device! (LocationID %1 is gone)").arg(board.info.locationId);
        board.info.verifyFailed = result.name;
        board.info.verifyMismatch = -1;
        board.info.leftInLoader = view.pid != 0 && view.pid == board.mappedPid;
        board.info.retry.clear();
        board.info.writingSeconds = board.writeClock.elapsed() / 1000.0;
        const quint64 id = runId;
        emit q->partitionVerified(board.info, result);
        if (!alive(id)) {
            return;
        }
        endBoard(index, FleetBoard::Result::Failed, verifyFailedText(result.name, result.error),
                 {FleetFlasher::tr("port %1 shows %2: find its LocationID with `upgrade_tool LD` before any retry, and address it with -s")
                      .arg(port, shownOnPort(view, board.mappedPid))});
        afterProcess();
    }

    /// "No found specific rockusb device!": the id is gone, so no addressed retry line can be given.
    void onNoSuchDevice(int index)
    {
        Board& board = at(index);
        const QString port = board.info.port;
        const PortView view = portView(port);
        const QString shown = shownOnPort(view, board.mappedPid);
        board.info.leftInLoader = view.pid != 0 && view.pid == board.mappedPid;
        board.info.retry.clear();
        board.info.writingSeconds = board.writeClock.elapsed() / 1000.0;
        endBoard(index, FleetBoard::Result::Failed,
                 FleetFlasher::tr("writing %1 failed: No found specific rockusb device! (LocationID %2 is gone)")
                     .arg(rows.at(board.row).name, board.info.locationId),
                 {FleetFlasher::tr("port %1 shows %2: find its LocationID with `upgrade_tool LD` before any retry, and address it with -s")
                      .arg(port, shown)});
        afterProcess();
    }

    // ---- RD -----------------------------------------------------------------------------------

    void startReset(int index)
    {
        Board& board = at(index);
        board.info.stage = FleetBoard::Stage::Resetting;
        changed(board);
        boardLog(board, FleetFlasher::tr("partitions written; resetting the board"));
        board.lineBuffer.clear();
        ++inFlightUpgradeTools;
        const QString n = board.info.locationId;
        boardCommand(index, [this, n] { return upgradeTool->resetDeviceOn(n); }, true, [this, index](const ToolResult& r) {
            --inFlightUpgradeTools;
            Board& b = at(index);
            flushToolLines(b);
            const QString output = r.outputText();
            for (const QString& line : output.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
                if (!line.trimmed().isEmpty()) {
                    emit q->toolOutput(b.info, line);
                }
            }
            b.info.writingSeconds = b.writeClock.elapsed() / 1000.0;
            releaseSlot(b);
            if (r.started && RockchipTool::reportsNoSuchDevice(output)) {
                // The id is gone (exit 255): nothing was reset, and no RD line can address it.
                const PortView view = portView(b.info.port);
                b.info.leftInLoader = view.pid != 0 && view.pid == b.mappedPid;
                b.info.retry.clear();
                endBoard(index, FleetBoard::Result::Failed,
                         FleetFlasher::tr("partitions written, but upgrade_tool RD found no device with LocationID %1 (it is gone): find "
                                          "its LocationID with `upgrade_tool LD` before any retry, and address it with -s")
                             .arg(b.info.locationId),
                         {FleetFlasher::tr("port %1 shows %2").arg(b.info.port, shownOnPort(view, b.mappedPid))});
                afterProcess();
                return;
            }
            if (!r.started || r.timedOut || !(r.exitCode == 0 || RockchipTool::reportsResetOk(output))) {
                const QString tail = !r.started ? r.error : r.timedOut ? FleetFlasher::tr("timed out") : RockchipTool::tailForMessage(output);
                endBoard(index, FleetBoard::Result::Failed,
                         FleetFlasher::tr("partitions written, but upgrade_tool RD did not reset the board (%1); replug it")
                             .arg(tail.isEmpty() ? FleetFlasher::tr("no output") : tail));
                afterProcess();
                return;
            }
            if (!waitAdb) {
                endBoard(index, FleetBoard::Result::Done, FleetFlasher::tr("ADB not waited for (--no-wait)"));
                afterProcess();
                return;
            }
            enterAdbWait(index);
            afterProcess();
        });
    }

    // =========================================================================================
    // Returning: the ADB waits, the board info
    // =========================================================================================

    void enterAdbWait(int index)
    {
        Board& board = at(index);
        board.info.stage = FleetBoard::Stage::WaitingForAdb;
        board.waitClock.start();
        board.firstPoll = true;
        changed(board);
        boardLog(board, FleetFlasher::tr("waiting for ADB (up to %1 s)").arg(adbReturnSeconds));
        if (!adbTimer->isActive()) {
            adbTimer->start(pollMs);
        }
    }

    void adbTick()
    {
        if (!running) {
            adbTimer->stop();
            return;
        }
        bool anyWaiting = false;
        const quint64 id = runId;
        for (int i = 0; i < count() && alive(id); ++i) {
            Board& board = at(i);
            if (board.ended || board.info.stage != FleetBoard::Stage::WaitingForAdb) {
                continue;
            }
            anyWaiting = true;
            if (board.busy) {
                continue;   // at most one get-state per board in flight
            }
            if (board.waitClock.elapsed() >= qint64(adbReturnSeconds) * 1000) {
                endBoard(i, FleetBoard::Result::NotBack,
                         board.leftBeforeReset ? leftBeforeResetNotBack(board)
                                               : FleetFlasher::tr("partitions written and the board reset, but ADB did not come "
                                                                  "back within %1 s; check the serial console")
                                                     .arg(adbReturnSeconds));
                afterProcess();
                continue;
            }
            const bool verbose = board.firstPoll;
            board.firstPoll = false;
            boardCommand(i, [&board] { return board.adb->getState(); }, verbose, [this, i](const ToolResult& r) {
                Board& b = at(i);
                if (r.cancelled || b.ended || b.info.stage != FleetBoard::Stage::WaitingForAdb) {
                    afterProcess();
                    return;
                }
                if (r.started && AdbTool::parseState(r.outputText()) == QLatin1String("device")) {
                    b.info.adbBack = true;
                    readBoardInfo(i);
                }
            });
        }
        if (!anyWaiting && alive(id)) {
            adbTimer->stop();
        }
    }

    void readBoardInfo(int index)
    {
        Board& board = at(index);
        board.info.stage = FleetBoard::Stage::ReadingInfo;
        changed(board);
        boardCommand(index, [&board] { return board.adb->shell(QStringLiteral("cat /proc/version; uptime")); }, true,
                     [this, index](const ToolResult& r) {
                         Board& b = at(index);
                         if (r.started && !r.timedOut) {
                             b.info.boardInfo = r.outputText().trimmed();
                             b.info.uptime = uptimeWords(b.info.boardInfo);
                         }
                         readRfmcu(index);
                     });
    }

    void readRfmcu(int index)
    {
        Board& board = at(index);
        boardCommand(index, [&board] { return board.adb->shell(QStringLiteral("rfmcu status")); }, true,
                     [this, index](const ToolResult& r) {
                         Board& b = at(index);
                         if (r.started && !r.timedOut) {
                             b.info.rfmcu = r.outputText().trimmed();   // recorded, never judged
                         }
                         QString line = FleetFlasher::tr("back on ADB");
                         if (!b.info.boardInfo.isEmpty()) {
                             line += QStringLiteral(": ") + kernelRelease(b.info.boardInfo);
                             if (!b.info.uptime.isEmpty()) {
                                 line += QStringLiteral(", ") + b.info.uptime;
                             }
                         }
                         if (!b.info.rfmcu.isEmpty()) {
                             line += FleetFlasher::tr("; rfmcu: %1").arg(b.info.rfmcu.simplified());
                         }
                         boardLog(b, line);
                         endBoard(index, FleetBoard::Result::Done,
                                  b.leftBeforeReset ? FleetFlasher::tr("partitions written; the board left Loader mode before its reset (%1) "
                                                                       "and came back on ADB by itself")
                                                          .arg(b.leftEvidencePast)
                                                    : QString());
                         afterProcess();
                     });
    }

    // =========================================================================================
    // The drain, the end
    // =========================================================================================

    int writesInProgress() const
    {
        int n = 0;
        for (const auto& board : boards) {
            n += !board->ended && board->started
                         && (board->info.stage == FleetBoard::Stage::Writing || board->info.stage == FleetBoard::Stage::Resetting)
                     ? 1
                     : 0;
        }
        return n;
    }

    void cancel()
    {
        if (!running) {
            return;
        }
        if (draining) {
            log(FleetFlasher::tr("still waiting for %1 write(s) to finish; the writes are never interrupted").arg(writesInProgress()));
            return;
        }
        draining = true;
        const int writes = writesInProgress();
        log(writes > 0 ? FleetFlasher::tr("cancel: the boards not started are reset and skipped; %1 write(s) in progress finish "
                                          "first (every row and the reset)")
                             .arg(writes)
                       : FleetFlasher::tr("cancel: the boards not started are reset and skipped"));
        if (phase == Phase::Writing || phase == Phase::Returning) {
            drain();
        }
        // Selecting / Mapping: the step in flight ends first (a board told `reboot loader` has its
        // wait finished), then the chain sees `draining`.
    }

    void drain()
    {
        const quint64 id = runId;
        for (int i = 0; i < count() && alive(id); ++i) {
            Board& board = at(i);
            if (board.ended) {
                continue;
            }
            if (board.info.stage == FleetBoard::Stage::WaitingForAdb) {
                if (board.process) {
                    board.process->cancel();   // an adb get-state, never an upgrade_tool
                }
                endBoard(i, FleetBoard::Result::NotBack, FleetFlasher::tr("the ADB wait was cancelled; the partitions were written"));
            } else if (board.info.stage == FleetBoard::Stage::Loader || (board.inSlot && !board.started && board.checkQueued)) {
                resetCancelled(i);
            }
        }
        if (alive(id)) {
            maybeFinishRun();
        }
    }

    /// After every process of a board ended: the next board gets the slot, and the run may end.
    void afterProcess()
    {
        if (!running) {
            return;
        }
        if (phase == Phase::Writing || phase == Phase::Returning) {
            fill();
        } else {
            maybeFinishRun();
        }
    }

    bool quiescent() const
    {
        if (inFlightUpgradeTools > 0 || gateLdBusy || snapshotRunning || runProcess) {
            return false;
        }
        for (const auto& board : boards) {
            if (board->busy || !board->ended) {
                return false;
            }
        }
        return true;
    }

    void maybeFinishRun()
    {
        if (!running || closing || !canClose) {
            return;
        }
        bool writing = false;
        for (const auto& board : boards) {
            writing = writing || board->inSlot;
        }
        if (phase == Phase::Writing && !writing) {
            setPhase(Phase::Returning);
        }
        if (!quiescent()) {
            return;
        }
        closeRun();
    }

    /// The end of a run that rebooted a board: one more picture; what sits in download mode and
    /// belongs to nobody is reported, and the run's own failed boards found in Loader mode say so.
    void closeRun()
    {
        closing = true;
        adbTimer->stop();
        if (!anyRebooted) {
            finishRun();
            return;
        }
        takeSnapshot([this](const Picture& picture) {
            if (!picture.error.isEmpty()) {
                log(FleetFlasher::tr("the end-of-run check could not run: %1").arg(picture.error));
                finishRun();
                return;
            }
            QStringList boardPorts;
            QStringList boardIds;
            for (const auto& board : boards) {
                boardPorts << board->info.port;
                boardIds << board->info.locationId;
            }
            const QStringList listed = sortedIds(picture.snapshot.loaderIds + picture.snapshot.maskromIds);
            const QStringList strayPorts =
                without(without(sortedPorts(picture.snapshot.downloadPorts.keys()), sortedPorts(baseline.downloadPorts.keys())), boardPorts);
            const QStringList strayIds = without(without(listed, baseline.loaderIds + baseline.maskromIds), boardIds);
            // A board of the run whose port shows a download-mode device without its own id being
            // listed re-enumerated: a new id on a port that is a board's is that board's (by port),
            // as long as no unknown port is on the bus that the id could belong to as well.
            int renumbered = 0;
            for (const auto& board : boards) {
                renumbered += !board->info.port.isEmpty() && picture.snapshot.downloadPorts.contains(board->info.port)
                                      && !listed.contains(board->info.locationId)
                                  ? 1
                                  : 0;
            }
            // Exactly one such board and exactly one id nobody owns, no unknown port: one id, one
            // port - the id is that board's, and its retry lines can be addressed.
            const bool paired = strayPorts.isEmpty() && renumbered == 1 && strayIds.size() == 1
                                && picture.snapshot.loaderIds.contains(strayIds.first());
            correctEndedBoards(picture, paired ? strayIds.first() : QString());
            if (!(strayPorts.isEmpty() && strayIds.size() <= renumbered)) {
                for (const UnattributedDevice& device : looseDevices(strayIds, strayPorts, picture.snapshot)) {
                    addUnattributed(device);
                }
            }
            if (strayIds.isEmpty() && !strayPorts.isEmpty()) {
                log(FleetFlasher::tr("a device in download mode on port %1 that upgrade_tool LD does not list belongs to no board of "
                                     "the run")
                        .arg(strayPorts.join(QStringLiteral(", "))));
            }
            finishRun();
        });
    }

    /// The end-of-run picture against the run's own ended boards - Failed, NotBack, and Skipped
    /// after a stop's / a cancel's reset (it has a LocationID): one found in its Loader mode (a
    /// re-enumeration gap, a failed RD, an RD that answered OK and did not take) is recorded as
    /// such, with its addressed retry lines. `pairedId` (closeRun()): the one new LocationID that
    /// belongs to the one board of the run that re-enumerated; "" = none.
    void correctEndedBoards(const Picture& picture, const QString& pairedId)
    {
        const Snapshot& now = picture.snapshot;
        const QStringList listedAny = sortedIds(now.loaderIds + now.maskromIds);
        for (int i = 0; i < count(); ++i) {
            Board& board = at(i);
            const FleetBoard::Result result = board.info.result;
            const bool skipped = result == FleetBoard::Result::Skipped || board.resetOnly;
            const bool candidateResult = result == FleetBoard::Result::Failed || result == FleetBoard::Result::NotBack
                                         || (skipped && !board.info.locationId.isEmpty());
            if (!candidateResult || board.maskrom || board.info.port.isEmpty()) {
                continue;
            }
            const QString port = board.info.port;
            const quint16 pidNow = now.downloadPorts.value(port, 0);
            const bool loaderOnPort = pidNow != 0 && pidNow == board.mappedPid;
            const bool paired = !pairedId.isEmpty() && loaderOnPort && !listedAny.contains(board.info.locationId);
            if (board.info.leftInLoader && !paired) {
                continue;   // recorded already, with its lines or with the LD hint
            }
            if (paired) {
                boardLog(board, FleetFlasher::tr("LocationID %1 is gone; port %2 shows its Loader device with LocationID %3, the only "
                                                 "new one on the bus: it is this board's")
                                    .arg(board.info.locationId, port, pairedId));
                board.info.locationId = pairedId;
            }
            const QString n = board.info.locationId;
            const bool listed = !n.isEmpty() && now.loaderIds.contains(n);
            // An RD that answered OK moments ago: both reads must still show the board in Loader mode.
            if (board.resetOk ? !(loaderOnPort && listed) : !(loaderOnPort || listed)) {
                continue;
            }
            if (board.resetOk) {
                board.resetOk = false;
                --resetCount;   // not reset back to ADB after all
            }
            board.info.leftInLoader = true;
            if (listed && skipped) {
                // Never written: only the reset is missing.
                board.info.detail += FleetFlasher::tr("; it is still in Loader mode (port %1, LocationID %2): reset it by hand with `%3`")
                                         .arg(port, n, rdHint(n));
                board.info.retry = {retryRdLine(n)};
                boardLog(board, FleetFlasher::tr("still in Loader mode on port %1, LocationID %2: reset it by hand with `%3` (the line "
                                                 "follows)")
                                    .arg(port, n, rdHint(n)));
            } else if (listed) {
                board.info.detail += FleetFlasher::tr("; it is in Loader mode now (port %1, LocationID %2)").arg(port, n);
                board.info.retry = retryLines(board, 0);
                boardLog(board, loaderHint(board));
            } else {
                board.info.detail += FleetFlasher::tr("; it is in Loader mode now (port %1)").arg(port);
                boardLog(board, FleetFlasher::tr("port %1 shows %2: find its LocationID with `upgrade_tool LD` before any retry, and "
                                                 "address it with -s")
                                    .arg(port, pidText(pidNow)));
            }
            for (const QString& line : retryLogLines(board.info.retry)) {
                boardLog(board, line);
            }
            changed(board);
        }
    }

    void finishRun()
    {
        if (!running) {
            return;
        }
        endMappingClock();
        pollTimer->stop();
        adbTimer->stop();
        finalElapsed = runClock.elapsed() / 1000.0;
        running = false;
        closing = false;
        const FleetReport record = q->report();
        const bool ok = record.allOk() && !draining;
        const QString message = record.summary();
        qCInfo(lcFirmware).noquote() << "fleet flash finished:" << ok << message;
        setPhase(draining ? Phase::Cancelled : ok ? Phase::Done : Phase::Failed);
        emit q->finished(ok, message);
    }
};

// =============================================================================================
// FleetFlasher
// =============================================================================================

FleetFlasher::FleetFlasher(ToolRunner* runner, QObject* parent) : QObject(parent), d(std::make_unique<Private>(this, runner)) {}

FleetFlasher::~FleetFlasher()
{
    // Waits go; a WL / RD in flight does not (never cancelled): the owners never let the flasher
    // go while it writes, because the runner kills its processes when it is destroyed.
    d->pollTimer->stop();
    d->adbTimer->stop();
    for (const auto& board : d->boards) {
        if (board->silenceTimer) {
            board->silenceTimer->stop();
        }
    }
}

void FleetFlasher::setToolPaths(const QString& upgradeTool, const QString& adb)
{
    d->upgradeTool->setToolPath(upgradeTool);
    d->adb->setAdbPath(adb);
}

QString FleetFlasher::upgradeToolPath() const { return d->upgradeTool->toolPath(); }
QString FleetFlasher::adbPath() const { return d->adb->adbPath(); }
RockchipTool* FleetFlasher::upgradeTool() const { return d->upgradeTool; }
AdbTool* FleetFlasher::adb() const { return d->adb; }
void FleetFlasher::setImagePath(const QString& path) { d->imagePath = path; }
QString FleetFlasher::imagePath() const { return d->imagePath; }
void FleetFlasher::setPartitions(const QList<PartitionImage>& rows) { d->rows = rows; }
QList<PartitionImage> FleetFlasher::partitions() const { return d->rows; }

void FleetFlasher::setLayout(const QString& mtdparts, bool fromImage)
{
    d->planLayout = normalizedLayout(mtdparts);
    d->planLayoutFromImage = fromImage;
}

QString FleetFlasher::layout() const { return d->planLayout; }
bool FleetFlasher::layoutFromImage() const { return d->planLayoutFromImage; }
void FleetFlasher::setBoards(const QList<QPair<QString, QString>>& taggedSerials) { d->selection = taggedSerials; }
QList<QPair<QString, QString>> FleetFlasher::taggedSerials() const { return d->selection; }
void FleetFlasher::setAllBoards(bool on) { d->everyBoard = on; }
bool FleetFlasher::allBoards() const { return d->everyBoard; }
void FleetFlasher::setMaxParallel(int count) { d->parallel = qMax(0, count); }
int FleetFlasher::maxParallel() const { return d->parallel; }
void FleetFlasher::setDryRun(bool on) { d->dry = on; }
bool FleetFlasher::dryRun() const { return d->dry; }
void FleetFlasher::setLoaderWaitSeconds(int seconds) { d->loaderWaitSeconds = qMax(1, seconds); }
int FleetFlasher::loaderWaitSeconds() const { return d->loaderWaitSeconds; }
void FleetFlasher::setAdbReturnTimeoutSeconds(int seconds) { d->adbReturnSeconds = qMax(1, seconds); }
int FleetFlasher::adbReturnTimeoutSeconds() const { return d->adbReturnSeconds; }
void FleetFlasher::setPollIntervalMs(int ms) { d->pollMs = qMax(1, ms); }
int FleetFlasher::pollIntervalMs() const { return d->pollMs; }
void FleetFlasher::setWaitForAdb(bool on) { d->waitAdb = on; }
bool FleetFlasher::waitForAdb() const { return d->waitAdb; }
void FleetFlasher::setLdPolicy(LdPolicy policy) { d->policy = policy; }
FleetFlasher::LdPolicy FleetFlasher::ldPolicy() const { return d->policy; }
void FleetFlasher::setPlanProfile(const ProfileDetection& profile) { d->planProfile = profile; }
ProfileDetection FleetFlasher::planProfile() const { return d->planProfile; }

void FleetFlasher::setProfileOverride(const QString& id)
{
    const std::optional<BoardProfile> profile = BoardProfiles::byId(id);
    d->profileOverride = profile ? profile->id : QString();
}

QString FleetFlasher::profileOverride() const { return d->profileOverride; }
void FleetFlasher::setVerifyAfterWrite(bool on) { d->verify = on; }
bool FleetFlasher::verifyAfterWrite() const { return d->verify; }
void FleetFlasher::setUsbEnumerator(UsbProbe::Enumerator enumerator) { d->enumerator = std::move(enumerator); }
void FleetFlasher::setPortProvider(PortProvider provider) { d->portProvider = std::move(provider); }

bool FleetFlasher::isRunning() const { return d->running; }
bool FleetFlasher::isDraining() const { return d->running && d->draining; }
FleetFlasher::Phase FleetFlasher::phase() const { return d->phase; }

QList<FleetBoard> FleetFlasher::boards() const
{
    QList<FleetBoard> list;
    for (const auto& board : d->boards) {
        list << board->info;
    }
    return list;
}

QList<FleetFlasher::UnattributedDevice> FleetFlasher::foreignDevices() const { return d->foreign; }
QList<FleetFlasher::UnattributedDevice> FleetFlasher::unattributedDevices() const { return d->unattributed; }
QString FleetFlasher::stopReason() const { return d->stopText; }

double FleetFlasher::elapsedSeconds() const
{
    return d->running ? d->runClock.elapsed() / 1000.0 : d->finalElapsed;
}

double FleetFlasher::mappingSeconds() const
{
    if (d->mappingEnded) {
        return d->mappingSecs;
    }
    return d->mappingStarted ? d->mappingClock.elapsed() / 1000.0 : 0.0;
}

double FleetFlasher::writingSeconds() const { return d->writingSecs; }

QString FleetFlasher::summary() const { return report().summary(); }

FleetReport FleetFlasher::report() const
{
    FleetReport record;
    record.setBoards(boards());
    record.setDryRun(d->dry);
    record.setImage(d->imagePath);
    record.setPartitions(d->rowNames());
    record.setMaxParallel(d->parallel);
    record.setSeconds(elapsedSeconds(), mappingSeconds(), writingSeconds());
    record.setStop(d->stopText);
    record.setResetCount(d->resetCount);
    record.setUnattributed(d->dry ? d->foreign : d->unattributed);
    record.setVerifyAfterWrite(d->verify);
    return record;
}

QString FleetFlasher::platformRefusal() const
{
    return d->enumerator ? QString() : tr("port mapping not available on this platform");
}

bool FleetFlasher::start() { return d->start(); }
void FleetFlasher::cancel() { d->cancel(); }
