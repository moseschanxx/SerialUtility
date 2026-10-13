#include "firmware/SocFlasher.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QRegularExpression>
#include <QTimer>

#include <functional>
#include <utility>

#include "app/Logging.h"

namespace {

/// UF / WL are never stopped, but a USB stall can make them hang without a word: after this long
/// without output the log says so (again after every further period), so the user knows what is
/// going on.
constexpr int kUfSilenceLogMs = 5 * 60 * 1000;

/// upgrade_tool WL counts 512-byte sectors: SocToolKit passes the partition's byte offset / 512.
constexpr quint64 kSectorBytes = 512;

/// The flow waits for a plan that is already running and looks again when it reports; a plan that
/// keeps going stale (the image rewritten under it) ends the flow after this many rounds.
constexpr int kMaxPlanRounds = 3;

/// A rockusb device in Loader / Maskrom mode is one upgrade_tool can write to; anything else
/// `upgrade_tool LD` may list (a mass-storage mode, an unknown mode) is not.
bool isDownloadMode(const RockusbDevice& device)
{
    return device.mode == RockusbDevice::Mode::Loader || device.mode == RockusbDevice::Mode::Maskrom;
}

/// "Linux version 5.10.209+ (user@host) ..." -> "5.10.209+"; the first line when it has another shape.
QString kernelRelease(const QString& procVersion)
{
    static const QRegularExpression pattern(QStringLiteral("Linux version (\\S+)"));
    const QRegularExpressionMatch match = pattern.match(procVersion);
    if (match.hasMatch()) {
        return match.captured(1);
    }
    return procVersion.trimmed().section(QLatin1Char('\n'), 0, 0).trimmed();
}

/// The layout as the plan key compares it: without "mtdparts=" and surrounding blanks.
QString normalizedLayout(const QString& mtdparts)
{
    QString text = mtdparts.trimmed();
    if (text.startsWith(QLatin1String("mtdparts="))) {
        text = text.mid(9).trimmed();
    }
    return text;
}

/// A WL that went wrong says so ("Write LBA failed!", "Error:...->Fail to write lba,...", "Write
/// LBA quit, creating comm object failed!" - the texts in upgrade_tool v2.37); its exit status has
/// not been seen for every failure, so the text counts as well. The banner ("please upgrade it to
/// enjoy fast download") has none of these words.
bool reportsWriteFailure(const QString& output)
{
    static const QRegularExpression failure(QStringLiteral("\\bfail|\\bquit\\b|\\berror\\b"),
                                            QRegularExpression::CaseInsensitiveOption);
    return failure.match(output).hasMatch();
}

QString layoutUnknownMessage()
{
    return QCoreApplication::translate("SocFlasher",
                                       "partition layout unknown: boot the board once so it can be read over ADB, "
                                       "or flash it in MASKROM mode");
}

} // namespace

/**
 * Three machines share this class: the device-status query, the flash flow and the partition
 * plan. All are chains of asynchronous commands; every callback captures the id of the chain it
 * belongs to (`queryId`, `flowId`, `planId`) and does nothing when the chain has been replaced or
 * ended meanwhile, which is how cancel() and endFlow() retire commands that are still on their way.
 *
 * The plan (planPartitions(), and the flow's step 5 in Loader mode) is `EXF` into the extract
 * folder followed by RockchipTool::planPartitions() against the layout: the image's own when its
 * env.img carries a readable mtdparts (the layout the images were built for; a different board
 * layout is logged as a warning), else mtdParts(). It is kept with its key (image path, image
 * mtime, board layout, folder) and the layout it used: the flow writes from a plan whose key
 * still matches, whose files still have the planned sizes and whose layout would still be the
 * same, re-plans otherwise, and waits for a plan that is already running (the tab starts one
 * whenever its image or layout changes). A plan whose image was unpacked already (only the
 * board layout changed) skips `EXF`. Step 2 makes the plan before `adb reboot loader` when the
 * board told no layout, so a board is never rebooted into a Loader mode nothing can be written
 * in. The partition writes then run one `WL` after the other from a copy of the selected rows,
 * then `RD`.
 */
struct SocFlasher::Private
{
    struct QueryOptions
    {
        bool verbose = false;   ///< log the commands (the flow's own queries; the tab's 2 s refresh stays silent)
        bool adb = true;
        bool probe = true;      ///< UsbProbe decides None / NoDriver
    };
    struct Query
    {
        quint64 id = 0;
        QueryOptions options;
        QString problem;        ///< a tool that could not be run, for DeviceStatus::detail
        std::function<void(const DeviceStatus&)> then;
    };
    /// What a plan was made for; equal keys = the same rows.
    struct PlanKey
    {
        QString image;
        QDateTime modified;
        QString layout;         ///< normalizedLayout()
        QString directory;      ///< the extract folder
        bool operator==(const PlanKey& o) const
        {
            return image == o.image && modified == o.modified && layout == o.layout && directory == o.directory;
        }
        /// The same unpacked files: only the layout may differ.
        bool sameExtraction(const PlanKey& o) const
        {
            return image == o.image && modified == o.modified && directory == o.directory;
        }
    };
    using PlanDone = std::function<void(bool ok, const QString& message)>;

    Private(SocFlasher* owner, ToolRunner* toolRunner)
        : q(owner), runner(toolRunner), upgradeTool(new RockchipTool(toolRunner, owner)),
          adb(new AdbTool(toolRunner, owner))
    {
        pollTimer = new QTimer(owner);
        pollTimer->setSingleShot(true);
        QObject::connect(pollTimer, &QTimer::timeout, owner, [this] { onPollTimer(); });
        silenceTimer = new QTimer(owner);
        silenceTimer->setSingleShot(true);
        QObject::connect(silenceTimer, &QTimer::timeout, owner, [this] { onFlashSilence(); });
    }

    SocFlasher* q;
    QPointer<ToolRunner> runner;
    RockchipTool* upgradeTool;
    AdbTool* adb;
    QTimer* pollTimer = nullptr;
    QTimer* silenceTimer = nullptr;            ///< upgrade_tool printed nothing for kUfSilenceLogMs
    QElapsedTimer lastFlashOutput;

    QString serial;
    QString imagePath;
    int waitTimeoutSeconds = 30;
    int adbReturnTimeoutSeconds = 60;
    int pollIntervalMs = 1000;
    bool waitForAdb = true;
    QString mtdParts;                          ///< as set / read; normalizedLayout() for comparisons
    QString extractDirectory;                  ///< the owner's choice; empty = next to the image
    QStringList selectedNames;                 ///< empty = every row in the layout that fits

    DeviceStatus status;
    Phase phase = Phase::Idle;
    FlashMethod method = FlashMethod::None;
    quint64 flowId = 0;
    quint64 queryId = 0;
    bool queryBusy = false;
    Query query;
    QPointer<ToolInvocation> queryInvocation;
    QPointer<ToolInvocation> stepInvocation;   ///< the flow's current poll / adb command (never the flash)
    QElapsedTimer waitClock;
    bool waitAnyBoard = false;                 ///< step 3: an ADB board appearing also counts
    RockchipTool::ProgressParser progressParser;
    QByteArray lineBuffer;

    // ---- the plan ------------------------------------------------------------------------
    QList<PartitionImage> partitions;          ///< rows of planKey (valid while planValid)
    PlanKey planKey;
    bool planValid = false;
    QString planLayout;                        ///< the layout the rows were placed in (normalized); empty = none
    bool planLayoutFromImage = false;          ///< planLayout came from the image's env.img
    PlanKey extractedKey;                      ///< what the extract folder holds (valid while extracted)
    bool extracted = false;
    bool planRunning = false;
    quint64 planId = 0;
    QPointer<ToolInvocation> planInvocation;
    PlanDone planThen;                         ///< the flow waiting for the running plan, if any

    // ---- the partition writes (step 5, Loader mode) --------------------------------------
    QList<PartitionImage> writeQueue;          ///< a copy: a re-plan cannot change what is being written
    qsizetype writeIndex = 0;
    quint64 writeTotal = 0;                    ///< bytes of the whole queue
    quint64 writeDone = 0;                     ///< bytes of the partitions written completely
    QString writeStage;                        ///< "Writing boot (5 of 7)"

    // ---- helpers -------------------------------------------------------------------------

    bool isRunning() const
    {
        switch (phase) {
        case Phase::Detecting:
        case Phase::RebootingToLoader:
        case Phase::WaitingForLoader:
        case Phase::Flashing:
        case Phase::WaitingForAdb:
        case Phase::ReadingBoardInfo:
            return true;
        case Phase::Idle:
        case Phase::Done:
        case Phase::Failed:
        case Phase::Cancelled:
            break;
        }
        return false;
    }

    void setPhase(Phase next)
    {
        if (phase == next) {
            return;
        }
        phase = next;
        qCInfo(lcFirmware) << "soc flasher phase" << next;
        emit q->phaseChanged(next);
    }

    void log(const QString& line)
    {
        qCInfo(lcFirmware).noquote() << line;
        emit q->logLine(line);
    }

    /// Log the command line now and its result when it ends ("every external command line and its
    /// result go to logLine()").
    ToolInvocation* tracked(ToolInvocation* invocation, bool verbose = true)
    {
        if (verbose) {
            log(QStringLiteral("$ ") + invocation->commandLine());
            QObject::connect(invocation, &ToolInvocation::finished, q, [this](const ToolResult& result) {
                if (result.cancelled) {
                    return;
                }
                if (!result.started) {
                    log(QCoreApplication::translate("SocFlasher", "  could not run: %1").arg(result.error));
                } else if (result.timedOut) {
                    log(QCoreApplication::translate("SocFlasher", "  timed out"));
                } else {
                    log(QCoreApplication::translate("SocFlasher", "  exit code %1").arg(result.exitCode));
                }
            });
        }
        return invocation;
    }

    /// The raw tool lines as they arrive (split at '\r' and '\n'), for the log pane.
    void forwardToolLines(const QByteArray& chunk)
    {
        lineBuffer.append(chunk);
        qsizetype start = 0;
        for (qsizetype i = 0; i < lineBuffer.size(); ++i) {
            const char c = lineBuffer.at(i);
            if (c == '\r' || c == '\n') {
                emitToolLine(lineBuffer.mid(start, i - start));
                start = i + 1;
            }
        }
        lineBuffer.remove(0, start);
    }

    void flushToolLines()
    {
        emitToolLine(lineBuffer);
        lineBuffer.clear();
    }

    void emitToolLine(const QByteArray& raw)
    {
        const QString line = QString::fromUtf8(raw);
        if (!line.trimmed().isEmpty()) {
            emit q->toolOutput(line);
        }
    }

    // ---- device status -------------------------------------------------------------------

    static DeviceStatus statusFromRockusb(const RockusbDevice& device)
    {
        DeviceStatus result;
        result.mode = device.mode == RockusbDevice::Mode::Maskrom ? DeviceStatus::Mode::Maskrom : DeviceStatus::Mode::Loader;
        result.serial = device.serial;
        result.detail = QStringLiteral("DevNo=%1 Vid=0x%2 Pid=0x%3 LocationID=%4")
                            .arg(device.devNo)
                            .arg(device.vid, 4, 16, QLatin1Char('0'))
                            .arg(device.pid, 4, 16, QLatin1Char('0'))
                            .arg(device.locationId);
        return result;
    }

    void startQuery(const QueryOptions& options, std::function<void(const DeviceStatus&)> then)
    {
        queryBusy = true;
        query = Query{++queryId, options, QString(), std::move(then)};
        const quint64 id = query.id;
        ToolInvocation* invocation = tracked(upgradeTool->listDevices(), options.verbose);
        queryInvocation = invocation;
        QObject::connect(invocation, &ToolInvocation::finished, q, [this, id](const ToolResult& result) {
            if (id == query.id && queryBusy && !result.cancelled) {
                onListDevices(result);
            }
        });
    }

    void abortQuery()
    {
        if (!queryBusy) {
            return;
        }
        queryBusy = false;
        ++queryId;   // its callbacks are stale
        if (queryInvocation && !queryInvocation->isFinished()) {
            queryInvocation->cancel();
        }
        queryInvocation.clear();
        query.then = nullptr;
    }

    void onListDevices(const ToolResult& result)
    {
        if (!result.started) {
            query.problem = QCoreApplication::translate("SocFlasher", "upgrade_tool could not be run: %1").arg(result.error);
        } else if (result.timedOut) {
            query.problem = QCoreApplication::translate("SocFlasher", "upgrade_tool LD timed out");
        }
        // A device in Loader / Maskrom mode wins over everything else (usb_update.sh).
        for (const RockusbDevice& device : RockchipTool::parseDeviceList(result.outputText())) {
            if (isDownloadMode(device)) {
                finishQuery(statusFromRockusb(device));
                return;
            }
        }
        if (!query.options.adb) {
            finishQuery(DeviceStatus());
            return;
        }
        const quint64 id = query.id;
        ToolInvocation* invocation = tracked(adb->devices(), query.options.verbose);
        queryInvocation = invocation;
        QObject::connect(invocation, &ToolInvocation::finished, q, [this, id](const ToolResult& adbResult) {
            if (id == query.id && queryBusy && !adbResult.cancelled) {
                onAdbDevices(adbResult);
            }
        });
    }

    void onAdbDevices(const ToolResult& result)
    {
        if (!result.started) {
            const QString problem = QCoreApplication::translate("SocFlasher", "adb could not be run: %1").arg(result.error);
            query.problem = query.problem.isEmpty() ? problem : query.problem + QStringLiteral("; ") + problem;
        } else if (result.timedOut) {
            const QString problem = QCoreApplication::translate("SocFlasher", "adb devices timed out");
            query.problem = query.problem.isEmpty() ? problem : query.problem + QStringLiteral("; ") + problem;
        }
        const QList<AdbDevice> devices = AdbTool::parseDevices(result.outputText());
        QString error;
        const QString picked = AdbTool::selectDevice(devices, serial, &error);
        if (!picked.isEmpty()) {
            DeviceStatus found;
            found.mode = DeviceStatus::Mode::Adb;
            found.serial = picked;
            found.detail = QStringLiteral("device");
            finishQuery(found);
            return;
        }

        DeviceStatus none;
        if (!query.problem.isEmpty()) {
            none.detail = query.problem;
        } else if (!devices.isEmpty()) {
            none.detail = error;   // boards are there but not ready (unauthorized, offline) or ambiguous
        }
        // Only a bus that shows a Rockchip device none of the tools can see is a driver problem.
        if (query.options.probe && devices.isEmpty() && query.problem.isEmpty()) {
            const QList<UsbDeviceInfo> present = UsbProbe::presentDevices();
            if (!present.isEmpty()) {
                none.mode = DeviceStatus::Mode::NoDriver;
                none.hint = UsbProbe::driverHint(present);
                none.detail = present.first().instanceId;
            }
        }
        finishQuery(none);
    }

    void finishQuery(const DeviceStatus& result)
    {
        queryBusy = false;
        queryInvocation.clear();
        status = result;
        auto then = std::move(query.then);
        query.then = nullptr;
        emit q->statusChanged(result);
        if (then) {
            then(result);
        }
    }

    // ---- the partition plan --------------------------------------------------------------

    QString effectiveExtractDirectory() const
    {
        if (!extractDirectory.isEmpty()) {
            return QDir::cleanPath(extractDirectory);
        }
        if (imagePath.isEmpty()) {
            return QString();
        }
        const QFileInfo image(imagePath);
        return image.absolutePath() + QLatin1Char('/') + image.completeBaseName() + QStringLiteral(".extracted");
    }

    PlanKey currentKey() const
    {
        return PlanKey{imagePath, QFileInfo(imagePath).lastModified(), normalizedLayout(mtdParts),
                       effectiveExtractDirectory()};
    }

    /// Every file of the kept plan is still there with the planned size.
    bool filesIntact() const
    {
        for (const PartitionImage& row : partitions) {
            if (row.fileSize >= 0) {
                const QFileInfo info(row.file);
                if (!info.isFile() || info.size() != row.fileSize) {
                    return false;
                }
            }
        }
        return true;
    }

    /// The layout a plan of `key` uses: the image's own (the mtdparts of its unpacked env.img: the
    /// layout the images were built for, and the one the env being written makes the board use)
    /// when it carries a readable one, else the board's / owner's (key.layout).
    struct ResolvedLayout
    {
        QString layout;         ///< normalized; empty = none known
        bool fromImage = false;
        QString image;          ///< what env.img says, readable or not ("" = nothing)
    };
    static ResolvedLayout resolveLayout(const PlanKey& key)
    {
        ResolvedLayout resolved;
        resolved.image = normalizedLayout(RockchipTool::mtdPartsFromEnvImage(key.directory + QStringLiteral("/env.img")));
        if (!RockchipTool::parseMtdParts(resolved.image).isEmpty()) {
            resolved.layout = resolved.image;
            resolved.fromImage = true;
        } else if (!RockchipTool::parseMtdParts(key.layout).isEmpty()) {
            resolved.layout = key.layout;
        }
        return resolved;
    }

    /// The kept plan still describes what the flash would write: same key (image, mtime, board
    /// layout, folder), intact files (an unpacked file changed by hand means unpack again) and the
    /// same layout in use.
    bool planIsCurrent() const
    {
        return planValid && planKey == currentKey() && filesIntact() && resolveLayout(planKey).layout == planLayout;
    }

    /// `selected` per setSelectedPartitions(): the default is every row in the layout that fits;
    /// a named row counts when it is in the layout (whether it fits is the flash's size check).
    void applySelection(QList<PartitionImage>& rows) const
    {
        for (PartitionImage& row : rows) {
            row.selected = selectedNames.isEmpty() ? (row.inLayout && row.fits)
                                                   : (row.inLayout && selectedNames.contains(row.name));
        }
    }

    /// Unpack (unless the folder already holds this image) and plan against mtdParts. `then` is
    /// the flow's continuation (null for planPartitions()); `inFlow` forwards the EXF output and
    /// reports the "Extracting" stage.
    void startPlan(PlanDone then, bool inFlow)
    {
        planRunning = true;
        const quint64 id = ++planId;
        planThen = std::move(then);
        const PlanKey key = currentKey();
        const QString packageFile = key.directory + QStringLiteral("/package-file");
        const bool keptFilesIntact = !planValid || !planKey.sameExtraction(key) || filesIntact();
        if (extracted && extractedKey.sameExtraction(key) && QFileInfo::exists(packageFile) && keptFilesIntact) {
            // Only the layout changed: the rows are computed again from the files already there,
            // reported from the event loop like every other plan.
            QTimer::singleShot(0, q, [this, id, key] {
                if (id == planId && planRunning) {
                    finishPlan(key);
                }
            });
            return;
        }
        extracted = false;
        if (key.directory.isEmpty() || !QDir().mkpath(key.directory)) {
            const QString message = QCoreApplication::translate("SocFlasher", "cannot create the folder %1 to unpack the image in")
                                        .arg(QDir::toNativeSeparators(key.directory));
            QTimer::singleShot(0, q, [this, id, message] {
                if (id == planId && planRunning) {
                    failPlan(message);
                }
            });
            return;
        }
        // The previous image's list must not survive an extraction that stops half way.
        QFile::remove(packageFile);
        if (inFlow) {
            emit q->progress(QCoreApplication::translate("SocFlasher", "Extracting"), -1);
            progressParser = RockchipTool::ProgressParser();
            lineBuffer.clear();
        }
        ToolInvocation* invocation = tracked(upgradeTool->extractFirmware(key.image, key.directory));
        planInvocation = invocation;
        if (inFlow) {
            QObject::connect(invocation, &ToolInvocation::outputReceived, q, [this, id](const QByteArray& chunk) {
                if (id == planId && planRunning) {
                    forwardToolLines(chunk);
                }
            });
        }
        QObject::connect(invocation, &ToolInvocation::finished, q, [this, id, key, inFlow](const ToolResult& result) {
            if (id != planId || !planRunning) {
                return;
            }
            planInvocation.clear();
            if (inFlow) {
                flushToolLines();
            }
            onExtracted(key, result);
        });
    }

    void onExtracted(const PlanKey& key, const ToolResult& result)
    {
        const QString image = QFileInfo(key.image).fileName();
        if (!result.started) {
            failPlan(QCoreApplication::translate("SocFlasher", "cannot run upgrade_tool: %1").arg(result.error));
            return;
        }
        if (result.cancelled) {
            failPlan(QCoreApplication::translate("SocFlasher", "cancelled"));
            return;
        }
        if (result.timedOut) {
            failPlan(QCoreApplication::translate("SocFlasher", "unpacking %1 timed out").arg(image));
            return;
        }
        if (result.exitCode != 0) {
            const QString tail = RockchipTool::tailForMessage(result.outputText());
            failPlan(QCoreApplication::translate("SocFlasher", "unpacking %1 failed: %2")
                         .arg(image, tail.isEmpty() ? QCoreApplication::translate("SocFlasher", "exit code %1").arg(result.exitCode)
                                                    : tail));
            return;
        }
        extracted = true;
        extractedKey = key;
        finishPlan(key);
    }

    void finishPlan(const PlanKey& key)
    {
        const ResolvedLayout resolved = resolveLayout(key);
        QString error;
        QList<PartitionImage> rows = RockchipTool::planPartitions(key.directory, RockchipTool::parseMtdParts(resolved.layout), &error);
        if (rows.isEmpty()) {
            extracted = false;
            failPlan(error.isEmpty() ? QCoreApplication::translate("SocFlasher", "the image lists no partition images") : error);
            return;
        }
        // Where the layout came from, every plan: the image's env.img wins, because the env written
        // with the partitions makes the board's kernel use that layout from the next boot on.
        const QList<MtdPartition> board = RockchipTool::parseMtdParts(key.layout);
        if (resolved.fromImage) {
            log(QCoreApplication::translate("SocFlasher", "layout from the image's env.img: %1").arg(resolved.layout));
            if (!board.isEmpty() && board != RockchipTool::parseMtdParts(resolved.layout)) {
                const QString warning =
                    QCoreApplication::translate("SocFlasher",
                                                "warning: the image's flash layout differs from the board's; the image's is "
                                                "used. image: %1; board: %2")
                        .arg(resolved.layout, key.layout);
                qCWarning(lcFirmware).noquote() << warning;
                emit q->logLine(warning);
            }
        } else if (!resolved.image.isEmpty()) {
            log(QCoreApplication::translate("SocFlasher", "the image's env.img has an unreadable mtdparts (%1); using the "
                                                          "board's layout")
                    .arg(resolved.image));
        }
        applySelection(rows);
        partitions = rows;
        planKey = key;
        planValid = true;
        planLayout = resolved.layout;
        planLayoutFromImage = resolved.fromImage;
        int inLayout = 0;
        for (const PartitionImage& row : std::as_const(rows)) {
            inLayout += row.inLayout ? 1 : 0;
        }
        const QString message = resolved.layout.isEmpty()
                                    ? layoutUnknownMessage()
                                    : QCoreApplication::translate("SocFlasher", "%1 partition images, %2 in the layout")
                                          .arg(rows.size())
                                          .arg(inLayout);
        qCInfo(lcFirmware).noquote() << "partition plan of" << key.image << ":" << message;
        endPlan(true, rows, message);
    }

    void failPlan(const QString& message)
    {
        planValid = false;
        partitions.clear();
        planLayout.clear();
        planLayoutFromImage = false;
        qCInfo(lcFirmware).noquote() << "partition plan failed:" << message;
        endPlan(false, {}, message);
    }

    /// The plan is over before anyone hears of it: a slot of partitionsPlanned() may start the
    /// next one, and the flow's continuation may start writing.
    void endPlan(bool ok, const QList<PartitionImage>& rows, const QString& message)
    {
        planRunning = false;
        PlanDone then = std::move(planThen);
        planThen = nullptr;
        emit q->partitionsPlanned(ok, rows, message);
        if (then) {
            then(ok, message);
        }
    }

    // ---- the flash flow ------------------------------------------------------------------

    QString validateStart() const
    {
        QString error;
        if (!RockchipTool::isRkfwImage(imagePath, &error)) {
            return error;
        }
        if (upgradeTool->toolPath().isEmpty()) {
            return QCoreApplication::translate("SocFlasher", "upgrade_tool not found: set its path in Preferences");
        }
        if (adb->adbPath().isEmpty()) {
            return QCoreApplication::translate("SocFlasher", "adb not found: set its path in Preferences");
        }
        if (!runner) {
            return QCoreApplication::translate("SocFlasher", "no tool runner");
        }
        return QString();
    }

    bool start()
    {
        if (isRunning()) {
            return false;
        }
        abortQuery();   // a refresh in flight must not overlap the flow's own query
        const quint64 id = ++flowId;
        method = FlashMethod::None;
        const QString problem = validateStart();
        if (!problem.isEmpty()) {
            // The caller gets false at once and the reason from the event loop, never from inside start().
            QTimer::singleShot(0, q, [this, id, problem] {
                if (id == flowId && !isRunning()) {
                    endFlow(Phase::Failed, false, problem);
                }
            });
            return false;
        }
        progressParser = RockchipTool::ProgressParser();
        lineBuffer.clear();
        waitAnyBoard = false;
        setPhase(Phase::Detecting);
        startQuery({true, true, true}, [this, id](const DeviceStatus& detected) {
            if (id == flowId && phase == Phase::Detecting) {
                onDetected(detected);
            }
        });
        return true;
    }

    /// The board the flow found on ADB is the one it talks to from then on (`adb -s <serial>
    /// reboot loader`, the get-state polls, the board info), as usb_update.sh does: a second ADB
    /// device appearing meanwhile (a phone plugged in during the 60 s wait) would otherwise make
    /// every serial-less command fail with "more than one device/emulator". The owner's own
    /// choice (setSerial()) wins; endFlow() puts it back.
    void adoptSerial(const DeviceStatus& seen)
    {
        if (serial.isEmpty() && seen.mode == DeviceStatus::Mode::Adb && !seen.serial.isEmpty()) {
            adb->setSerial(seen.serial);
        }
    }

    void endFlow(Phase endPhase, bool ok, const QString& message)
    {
        ++flowId;   // retire every callback of this flow
        pollTimer->stop();
        silenceTimer->stop();
        abortQuery();
        if (stepInvocation && !stepInvocation->isFinished()) {
            stepInvocation->cancel();
        }
        stepInvocation.clear();
        adb->setSerial(serial);
        writeQueue.clear();
        qCInfo(lcFirmware).noquote() << "soc flash finished:" << ok << message;
        setPhase(endPhase);
        emit q->finished(ok, message);
    }

    void cancel()
    {
        switch (phase) {
        case Phase::Flashing:
            log(QCoreApplication::translate("SocFlasher", "cannot cancel while upgrade_tool writes the flash"));
            return;
        case Phase::Idle:
        case Phase::Done:
        case Phase::Failed:
        case Phase::Cancelled:
            return;
        case Phase::WaitingForAdb:
            log(QCoreApplication::translate("SocFlasher", "stopped waiting for ADB; the image was already written"));
            break;
        case Phase::Detecting:
        case Phase::RebootingToLoader:
        case Phase::WaitingForLoader:
        case Phase::ReadingBoardInfo:
            break;
        }
        endFlow(Phase::Cancelled, false, QCoreApplication::translate("SocFlasher", "cancelled"));
    }

    // Step 1 -> 2 / 3 / 4
    void onDetected(const DeviceStatus& detected)
    {
        switch (detected.mode) {
        case DeviceStatus::Mode::Loader:
        case DeviceStatus::Mode::Maskrom:
            enterWaitingForLoader(false);
            break;
        case DeviceStatus::Mode::Adb:
            adoptSerial(detected);
            rebootToLoader();
            break;
        case DeviceStatus::Mode::NoDriver:
            log(detected.hint);
            enterWaitingForLoader(true);
            break;
        case DeviceStatus::Mode::None:
            if (!detected.detail.isEmpty()) {
                // Boards that are there but cannot be used (two ADB boards, an unauthorized one) or
                // a tool that could not run: said before the MASKROM advice, which does not help then.
                log(QCoreApplication::translate("SocFlasher", "no usable board: %1").arg(detected.detail));
            }
            enterWaitingForLoader(true);
            break;
        }
    }

    // Step 2: the kernel, the flash layout, then adb reboot loader
    void rebootToLoader()
    {
        setPhase(Phase::RebootingToLoader);
        const quint64 id = flowId;
        ToolInvocation* version = tracked(adb->shell(QStringLiteral("cat /proc/version")));
        stepInvocation = version;
        QObject::connect(version, &ToolInvocation::finished, q, [this, id](const ToolResult& result) {
            if (id != flowId || result.cancelled) {
                return;
            }
            // busybox on the Node board has no uname: the release comes from /proc/version.
            const QString kernel = result.started ? kernelRelease(result.outputText()) : QString();
            log(QCoreApplication::translate("SocFlasher", "board running %1, rebooting it into loader mode")
                    .arg(kernel.isEmpty() ? QCoreApplication::translate("SocFlasher", "an unknown kernel") : kernel));
            readFlashLayout();
        });
    }

    /// The running board knows its flash layout (the kernel's mtdparts), which Loader mode needs
    /// and cannot tell: read it now, while the board still runs.
    void readFlashLayout()
    {
        const quint64 id = flowId;
        ToolInvocation* cmdline = tracked(adb->shell(QStringLiteral("cat /proc/cmdline")));
        stepInvocation = cmdline;
        QObject::connect(cmdline, &ToolInvocation::finished, q, [this, id](const ToolResult& result) {
            if (id != flowId || result.cancelled) {
                return;
            }
            const QString found = result.started ? RockchipTool::mtdPartsFromCommandLine(result.outputText()) : QString();
            if (!found.isEmpty()) {
                mtdParts = found;
                log(QCoreApplication::translate("SocFlasher", "partition layout of the board: %1").arg(found));
                emit q->mtdPartsRead(found);
                if (id != flowId) {
                    return;   // a slot ended the flow
                }
            } else if (!normalizedLayout(mtdParts).isEmpty()) {
                log(QCoreApplication::translate("SocFlasher", "the board's /proc/cmdline has no mtdparts; using the layout "
                                                              "remembered for it: %1")
                        .arg(normalizedLayout(mtdParts)));
            } else {
                log(QCoreApplication::translate("SocFlasher", "the board's /proc/cmdline has no mtdparts"));
            }
            if (!RockchipTool::parseMtdParts(mtdParts).isEmpty()) {
                rebootIntoLoader();
                return;
            }
            // No layout from the board: the image may carry its own (env.img). Unpack it now, while
            // the board still runs its firmware: after `reboot loader` it would be in Loader mode,
            // where nothing can be written without a layout.
            ensurePlan([this](bool ok, const QString& message) {
                if (!ok) {
                    endFlow(Phase::Failed, false, message);
                } else if (planLayout.isEmpty()) {
                    endFlow(Phase::Failed, false, layoutUnknownMessage());
                } else {
                    rebootIntoLoader();
                }
            });
        });
    }

    void rebootIntoLoader()
    {
        const quint64 id = flowId;
        ToolInvocation* reboot = tracked(adb->rebootLoader());
        stepInvocation = reboot;
        QObject::connect(reboot, &ToolInvocation::finished, q, [this, id](const ToolResult& rebootResult) {
            if (id == flowId && !rebootResult.cancelled) {
                enterWaitingForLoader(false);   // its exit status is ignored, like the script's `|| true`
            }
        });
    }

    // Steps 3 and 4
    void enterWaitingForLoader(bool anyBoard)
    {
        setPhase(Phase::WaitingForLoader);
        waitAnyBoard = anyBoard;
        waitClock.start();
        if (anyBoard) {
            log(QCoreApplication::translate("SocFlasher",
                                            "no board found in ADB, MASKROM or Loader mode: send 'reboot loader' through a "
                                            "session, or hold MASKROM and replug the cable"));
        }
        log(QCoreApplication::translate("SocFlasher", "waiting for the board to enter MASKROM/Loader mode (up to %1 s)")
                .arg(waitTimeoutSeconds));
        if (anyBoard) {
            emit q->downloadModeNeeded();
            if (phase != Phase::WaitingForLoader) {
                return;   // a slot cancelled
            }
        }
        pollLoader(true);
    }

    void pollLoader(bool first)
    {
        const quint64 id = flowId;
        if (waitAnyBoard) {
            // Nothing was attached: the board may appear in any mode (the user holds MASKROM, or
            // just plugs the cable in with the running firmware, which then gets step 2).
            startQuery({first, true, false}, [this, id](const DeviceStatus& seen) {
                if (id != flowId || phase != Phase::WaitingForLoader) {
                    return;
                }
                if (seen.mode == DeviceStatus::Mode::Loader || seen.mode == DeviceStatus::Mode::Maskrom) {
                    log(QCoreApplication::translate("SocFlasher", "board is in download mode"));
                    startFlash(seen.mode == DeviceStatus::Mode::Maskrom ? RockusbDevice::Mode::Maskrom : RockusbDevice::Mode::Loader);
                } else if (seen.mode == DeviceStatus::Mode::Adb) {
                    adoptSerial(seen);
                    rebootToLoader();
                } else {
                    pollAgain();
                }
            });
            return;
        }
        ToolInvocation* invocation = tracked(upgradeTool->listDevices(), first);
        stepInvocation = invocation;
        QObject::connect(invocation, &ToolInvocation::finished, q, [this, id](const ToolResult& result) {
            if (id != flowId || phase != Phase::WaitingForLoader || result.cancelled) {
                return;
            }
            for (const RockusbDevice& device : RockchipTool::parseDeviceList(result.outputText())) {
                if (isDownloadMode(device)) {
                    log(QCoreApplication::translate("SocFlasher", "board is in download mode"));
                    startFlash(device.mode);
                    return;
                }
            }
            pollAgain();
        });
    }

    void pollAgain()
    {
        if (phase == Phase::WaitingForLoader) {
            if (waitClock.elapsed() >= qint64(waitTimeoutSeconds) * 1000) {
                endFlow(Phase::Failed, false,
                        QCoreApplication::translate("SocFlasher", "board did not enter MASKROM/Loader within %1 s")
                            .arg(waitTimeoutSeconds));
                return;
            }
        } else if (phase == Phase::WaitingForAdb) {
            if (waitClock.elapsed() >= qint64(adbReturnTimeoutSeconds) * 1000) {
                const QString message =
                    QCoreApplication::translate("SocFlasher",
                                                "board flashed, but ADB did not come back within %1 s; check the serial console")
                        .arg(adbReturnTimeoutSeconds);
                log(message);
                endFlow(Phase::Done, true, message);   // a success with a warning, as the script treats it
                return;
            }
        } else {
            return;
        }
        pollTimer->start(pollIntervalMs);
    }

    void onPollTimer()
    {
        if (phase == Phase::WaitingForLoader) {
            pollLoader(false);
        } else if (phase == Phase::WaitingForAdb) {
            pollAdb(false);
        }
    }

    // Step 5: the method follows from the download mode
    void startFlash(RockusbDevice::Mode mode)
    {
        setPhase(Phase::Flashing);
        const bool maskrom = mode == RockusbDevice::Mode::Maskrom;
        // The path as upgrade_tool gets it (RockchipTool: native separators).
        log(QCoreApplication::translate("SocFlasher", "flashing %1 (%2 mode)")
                .arg(QDir::toNativeSeparators(imagePath), maskrom ? QStringLiteral("Maskrom") : QStringLiteral("Loader")));
        progressParser = RockchipTool::ProgressParser();
        lineBuffer.clear();
        if (maskrom) {
            method = FlashMethod::UpgradeFirmware;
            qCInfo(lcFirmware) << "flash method" << method;
            log(QCoreApplication::translate("SocFlasher", "MASKROM mode: writing the whole image with upgrade_tool UF"));
            if (!selectedNames.isEmpty()) {
                log(QCoreApplication::translate("SocFlasher", "the partition selection (%1) is ignored: UF writes every partition")
                        .arg(selectedNames.join(QStringLiteral(", "))));
            }
            startUpgradeFirmware();
            return;
        }
        method = FlashMethod::PartitionWrites;
        qCInfo(lcFirmware) << "flash method" << method;
        log(QCoreApplication::translate("SocFlasher",
                                        "Loader mode: writing partition by partition with upgrade_tool WL, as SocToolKit's "
                                        "Download page does (no chip check)"));
        startPartitionWrites();
    }

    // ---- step 5, Maskrom: UF ------------------------------------------------------------

    void startUpgradeFirmware()
    {
        const quint64 id = flowId;
        lastFlashOutput.start();
        silenceTimer->start(kUfSilenceLogMs);
        // Not kept in stepInvocation: endFlow() and ~SocFlasher() cancel that one, and UF is never cancelled.
        ToolInvocation* invocation = tracked(upgradeTool->upgradeFirmware(imagePath));
        QObject::connect(invocation, &ToolInvocation::outputReceived, q, [this, id](const QByteArray& chunk) {
            if (id == flowId) {
                onFlashOutput(chunk);
            }
        });
        QObject::connect(invocation, &ToolInvocation::finished, q, [this, id](const ToolResult& result) {
            if (id == flowId) {
                onFlashFinished(result);
            }
        });
    }

    void emitProgress(const RockchipTool::Progress& progress)
    {
        if (progress.stage.startsWith(QLatin1String("rockusb driver"), Qt::CaseInsensitive)) {
            return;   // the banner is not a stage
        }
        emit q->progress(progress.stage, progress.percent);
    }

    /// Log only: UF / WL are never stopped by the flasher (a half-written flash needs MASKROM
    /// recovery), the user decides whether to end the process by hand.
    void onFlashSilence()
    {
        if (phase != Phase::Flashing) {
            return;
        }
        const qint64 minutes = lastFlashOutput.elapsed() / 60000;
        qCWarning(lcFirmware) << "upgrade_tool silent for" << minutes << "min";
        log(QCoreApplication::translate("SocFlasher",
                                        "upgrade_tool has printed nothing for %1 min; it is left running, because "
                                        "stopping it half way needs MASKROM recovery (if it hangs for good, end the "
                                        "upgrade_tool process by hand)")
                .arg(minutes));
        silenceTimer->start(kUfSilenceLogMs);
    }

    void onFlashOutput(const QByteArray& chunk)
    {
        lastFlashOutput.restart();
        silenceTimer->start(kUfSilenceLogMs);
        for (const RockchipTool::Progress& progress : progressParser.feed(chunk)) {
            emitProgress(progress);
        }
        forwardToolLines(chunk);
    }

    void onFlashFinished(const ToolResult& result)
    {
        silenceTimer->stop();
        for (const RockchipTool::Progress& progress : progressParser.flush()) {
            emitProgress(progress);
        }
        flushToolLines();

        if (!result.started) {
            endFlow(Phase::Failed, false,
                    QCoreApplication::translate("SocFlasher", "cannot run upgrade_tool: %1").arg(result.error));
            return;
        }
        const QString output = result.outputText();
        if (!RockchipTool::reportsSuccess(output)) {
            const QString tail = RockchipTool::tailForMessage(output);
            endFlow(Phase::Failed, false,
                    QCoreApplication::translate("SocFlasher", "upgrade_tool did not report success: %1")
                        .arg(tail.isEmpty() ? QCoreApplication::translate("SocFlasher", "no output") : tail));
            return;
        }
        afterFlash(QCoreApplication::translate("SocFlasher", "Upgrade firmware ok"));
    }

    /// Step 6 or the end, whichever method wrote the flash.
    void afterFlash(const QString& doneMessage)
    {
        if (!waitForAdb) {
            endFlow(Phase::Done, true, doneMessage);
            return;
        }
        enterWaitingForAdb();
    }

    // ---- step 5, Loader: the plan, WL per partition, RD -------------------------------

    /// The flow needs a current plan (step 5, and step 2 when the board told no layout): `done`
    /// at once when the kept plan is current, after the running one (the owner's, started for
    /// this image a moment ago or for an older one) when there is one, else after a plan of the
    /// flow's own. Nothing is called back once the flow left the phase it asked in.
    void ensurePlan(PlanDone done, int round = 0)
    {
        const quint64 id = flowId;
        const Phase asked = phase;
        if (planIsCurrent()) {
            done(true, QString());
            return;
        }
        if (round >= kMaxPlanRounds) {
            done(false, QCoreApplication::translate("SocFlasher", "the image changed while it was being unpacked; flash again"));
            return;
        }
        if (planRunning) {
            // Whatever the running plan finds, look again when it reports.
            log(QCoreApplication::translate("SocFlasher", "waiting for the image to be unpacked"));
            planThen = [this, id, asked, done, round](bool, const QString&) {
                if (id == flowId && phase == asked) {
                    ensurePlan(done, round + 1);
                }
            };
            return;
        }
        startPlan(
            [this, id, asked, done, round](bool ok, const QString& message) {
                if (id != flowId || phase != asked) {
                    return;
                }
                if (!ok) {
                    done(false, message);
                    return;
                }
                ensurePlan(done, round + 1);
            },
            true);
    }

    /// Step 5, Loader mode: the plan (with its layout), then the writes.
    void startPartitionWrites()
    {
        ensurePlan([this](bool ok, const QString& message) {
            if (!ok) {
                endFlow(Phase::Failed, false, message);
            } else if (planLayout.isEmpty()) {
                endFlow(Phase::Failed, false, layoutUnknownMessage());
            } else {
                log((planLayoutFromImage ? QCoreApplication::translate("SocFlasher", "flash layout in use (from the image's env.img): %1")
                                         : QCoreApplication::translate("SocFlasher", "flash layout in use (from the board): %1"))
                        .arg(planLayout));
                writePartitions();
            }
        });
    }

    /// The rows to write in package-file order, every one checked before the first byte is written.
    void writePartitions()
    {
        const bool named = !selectedNames.isEmpty();
        QList<PartitionImage> queue;
        quint64 total = 0;
        for (const PartitionImage& row : std::as_const(partitions)) {
            if (named && !selectedNames.contains(row.name)) {
                log(QCoreApplication::translate("SocFlasher", "skipping %1: not selected").arg(row.name));
                continue;
            }
            if (!row.inLayout) {
                log(QCoreApplication::translate("SocFlasher", "skipping %1: not in the board's partition layout").arg(row.name));
                continue;
            }
            const QFileInfo info(row.file);
            if (!info.isFile()) {
                if (named) {
                    endFlow(Phase::Failed, false,
                            QCoreApplication::translate("SocFlasher", "the image of partition %1 is missing: %2")
                                .arg(row.name, QDir::toNativeSeparators(row.file)));
                    return;
                }
                log(QCoreApplication::translate("SocFlasher", "skipping %1: the package has no image file for it").arg(row.name));
                continue;
            }
            const quint64 bytes = static_cast<quint64>(info.size());
            if (row.size != 0 && bytes > row.size) {
                // Written anyway, the image would run into the next partition: WL knows no boundaries.
                endFlow(Phase::Failed, false,
                        QCoreApplication::translate("SocFlasher", "%1 (%2 bytes) does not fit partition %3 (%4 bytes)")
                            .arg(info.fileName())
                            .arg(bytes)
                            .arg(row.name)
                            .arg(row.size));
                return;
            }
            if (row.offset % kSectorBytes != 0) {
                endFlow(Phase::Failed, false,
                        QCoreApplication::translate("SocFlasher", "partition %1 does not start on a 512-byte sector (offset %2)")
                            .arg(row.name)
                            .arg(row.offset));
                return;
            }
            PartitionImage item = row;
            item.fileSize = static_cast<qint64>(bytes);
            queue.append(item);
            total += bytes;
        }
        if (queue.isEmpty()) {
            endFlow(Phase::Failed, false,
                    QCoreApplication::translate("SocFlasher", "nothing to write: no selected partition image is in the layout"));
            return;
        }
        QStringList names;
        for (const PartitionImage& row : std::as_const(queue)) {
            names << row.name;
        }
        log(QCoreApplication::translate("SocFlasher", "writing %1 of %2 partitions (%3 bytes): %4")
                .arg(queue.size())
                .arg(partitions.size())
                .arg(total)
                .arg(names.join(QStringLiteral(", "))));
        writeQueue = queue;
        writeIndex = 0;
        writeTotal = total;
        writeDone = 0;
        writeNext();
    }

    int overallPercent(int filePercent) const
    {
        if (writeTotal == 0) {
            return 100;
        }
        quint64 done = writeDone;
        if (writeIndex < writeQueue.size() && filePercent > 0) {
            done += static_cast<quint64>(writeQueue.at(writeIndex).fileSize) * static_cast<quint64>(qMin(filePercent, 100)) / 100;
        }
        return static_cast<int>(qMin<quint64>(100, done * 100 / writeTotal));
    }

    void writeNext()
    {
        if (writeIndex >= writeQueue.size()) {
            resetBoard();
            return;
        }
        const quint64 id = flowId;
        const PartitionImage row = writeQueue.at(writeIndex);
        writeStage = QCoreApplication::translate("SocFlasher", "Writing %1 (%2 of %3)")
                         .arg(row.name)
                         .arg(writeIndex + 1)
                         .arg(writeQueue.size());
        log(QCoreApplication::translate("SocFlasher",
                                        "writing %1: %2 (%3 bytes) at sector %4 (offset 0x%5), partition of %6 sectors")
                .arg(row.name, QFileInfo(row.file).fileName())
                .arg(row.fileSize)
                .arg(row.offset / kSectorBytes)
                .arg(row.offset, 0, 16)
                .arg(row.size == 0 ? QStringLiteral("-1") : QString::number(row.size / kSectorBytes)));
        emit q->progress(writeStage, overallPercent(0));
        if (id != flowId) {
            return;
        }
        progressParser = RockchipTool::ProgressParser();
        lineBuffer.clear();
        lastFlashOutput.start();
        silenceTimer->start(kUfSilenceLogMs);
        // Not kept in stepInvocation, like UF: a WL is never cancelled.
        ToolInvocation* invocation =
            tracked(upgradeTool->writeLba(row.offset / kSectorBytes, row.size / kSectorBytes, row.file));
        QObject::connect(invocation, &ToolInvocation::outputReceived, q, [this, id](const QByteArray& chunk) {
            if (id == flowId) {
                onWriteOutput(chunk);
            }
        });
        QObject::connect(invocation, &ToolInvocation::finished, q, [this, id](const ToolResult& result) {
            if (id == flowId) {
                onWriteFinished(result);
            }
        });
    }

    void reportWriteProgress(const QList<RockchipTool::Progress>& segments)
    {
        for (const RockchipTool::Progress& progress : segments) {
            if (progress.percent >= 0) {   // "Write LBA from file (42%)"; other lines are log only
                emit q->progress(writeStage, overallPercent(progress.percent));
            }
        }
    }

    void onWriteOutput(const QByteArray& chunk)
    {
        lastFlashOutput.restart();
        silenceTimer->start(kUfSilenceLogMs);
        reportWriteProgress(progressParser.feed(chunk));
        forwardToolLines(chunk);
    }

    void onWriteFinished(const ToolResult& result)
    {
        silenceTimer->stop();
        reportWriteProgress(progressParser.flush());
        flushToolLines();
        const PartitionImage row = writeQueue.at(writeIndex);
        if (!result.started) {
            endFlow(Phase::Failed, false,
                    QCoreApplication::translate("SocFlasher", "cannot run upgrade_tool: %1").arg(result.error));
            return;
        }
        const QString output = result.outputText();
        if (result.timedOut || result.exitCode != 0 || reportsWriteFailure(RockchipTool::stripBanner(output))) {
            const QString tail = RockchipTool::tailForMessage(output);
            endFlow(Phase::Failed, false,
                    QCoreApplication::translate("SocFlasher", "writing %1 failed: %2")
                        .arg(row.name, tail.isEmpty() ? QCoreApplication::translate("SocFlasher", "no output (exit code %1)")
                                                            .arg(result.exitCode)
                                                      : tail));
            return;
        }
        writeDone += static_cast<quint64>(row.fileSize);
        ++writeIndex;
        writeNext();
    }

    /// `RD` after the last WL: the board leaves Loader mode and boots what was just written.
    void resetBoard()
    {
        const quint64 id = flowId;
        emit q->progress(QCoreApplication::translate("SocFlasher", "Resetting the board"), 100);
        if (id != flowId) {
            return;
        }
        lineBuffer.clear();
        ToolInvocation* invocation = tracked(upgradeTool->resetDevice());
        QObject::connect(invocation, &ToolInvocation::outputReceived, q, [this, id](const QByteArray& chunk) {
            if (id == flowId) {
                forwardToolLines(chunk);
            }
        });
        QObject::connect(invocation, &ToolInvocation::finished, q, [this, id](const ToolResult& result) {
            if (id != flowId) {
                return;
            }
            flushToolLines();
            const QString output = result.outputText();
            const bool resetOk = result.started && !result.timedOut
                                 && (result.exitCode == 0 || output.contains(QLatin1String("Reset Device OK"), Qt::CaseInsensitive));
            if (!resetOk) {
                const QString tail = result.started ? RockchipTool::tailForMessage(output) : result.error;
                endFlow(Phase::Failed, false,
                        QCoreApplication::translate("SocFlasher",
                                                    "the partitions were written, but upgrade_tool RD did not reset the "
                                                    "board (%1); replug it")
                            .arg(tail.isEmpty() ? QCoreApplication::translate("SocFlasher", "no output") : tail));
                return;
            }
            log(QCoreApplication::translate("SocFlasher", "partitions written; the board restarts"));
            afterFlash(QCoreApplication::translate("SocFlasher", "partitions written; board reset"));
        });
    }

    // Step 6
    void enterWaitingForAdb()
    {
        setPhase(Phase::WaitingForAdb);
        waitClock.start();
        log(QCoreApplication::translate("SocFlasher", "waiting for the board to come back on ADB (up to %1 s)")
                .arg(adbReturnTimeoutSeconds));
        pollAdb(true);
    }

    void pollAdb(bool first)
    {
        const quint64 id = flowId;
        ToolInvocation* invocation = tracked(adb->getState(), first);
        stepInvocation = invocation;
        QObject::connect(invocation, &ToolInvocation::finished, q, [this, id](const ToolResult& result) {
            if (id != flowId || phase != Phase::WaitingForAdb || result.cancelled) {
                return;
            }
            if (result.started && AdbTool::parseState(result.outputText()) == QLatin1String("device")) {
                log(QCoreApplication::translate("SocFlasher", "board is back on ADB"));
                readBoardInfo();
                return;
            }
            if (!result.started) {
                const QString message = QCoreApplication::translate("SocFlasher",
                                                                    "board flashed, but adb could not be run (%1); "
                                                                    "check the serial console")
                                            .arg(result.error);
                log(message);
                endFlow(Phase::Done, true, message);
                return;
            }
            pollAgain();
        });
    }

    // Step 7
    void readBoardInfo()
    {
        setPhase(Phase::ReadingBoardInfo);
        const quint64 id = flowId;
        ToolInvocation* invocation = tracked(adb->shell(QStringLiteral("cat /proc/version; uptime")));
        stepInvocation = invocation;
        QObject::connect(invocation, &ToolInvocation::finished, q, [this, id](const ToolResult& result) {
            if (id != flowId || result.cancelled) {
                return;
            }
            const QString info = result.outputText().trimmed();
            if (!info.isEmpty()) {
                emit q->boardInfo(info);
            }
            endFlow(Phase::Done, true, QCoreApplication::translate("SocFlasher", "Upgrade firmware ok; board is back on ADB"));
        });
    }
};

// ---------------------------------------------------------------------------------------------
// DeviceStatus
// ---------------------------------------------------------------------------------------------

QString SocFlasher::DeviceStatus::text() const
{
    auto withDetail = [this](const QString& base) {
        return detail.isEmpty() ? base : base + QStringLiteral(" (") + detail + QLatin1Char(')');
    };
    switch (mode) {
    case Mode::Adb:
        return serial.isEmpty() ? QCoreApplication::translate("SocFlasher", "ADB device")
                                : QCoreApplication::translate("SocFlasher", "ADB device %1").arg(serial);
    case Mode::Loader:
        return withDetail(QCoreApplication::translate("SocFlasher", "Loader mode"));
    case Mode::Maskrom:
        return withDetail(QCoreApplication::translate("SocFlasher", "Maskrom mode"));
    case Mode::NoDriver:
        return hint.isEmpty() ? QCoreApplication::translate("SocFlasher", "Rockchip USB device without a driver") : hint;
    case Mode::None:
        break;
    }
    return withDetail(QCoreApplication::translate("SocFlasher", "no board"));
}

bool SocFlasher::DeviceStatus::operator==(const DeviceStatus& other) const
{
    return mode == other.mode && serial == other.serial && detail == other.detail && hint == other.hint;
}

// ---------------------------------------------------------------------------------------------
// SocFlasher
// ---------------------------------------------------------------------------------------------

SocFlasher::SocFlasher(ToolRunner* runner, QObject* parent) : QObject(parent), d(new Private(this, runner)) {}

SocFlasher::~SocFlasher()
{
    // Waiting commands and an extraction go; a running UF / WL does not (never cancelled).
    d->pollTimer->stop();
    d->silenceTimer->stop();
    d->abortQuery();
    if (d->stepInvocation && !d->stepInvocation->isFinished()) {
        d->stepInvocation->cancel();
    }
    if (d->planInvocation && !d->planInvocation->isFinished()) {
        d->planInvocation->cancel();
    }
}

RockchipTool* SocFlasher::upgradeTool() const { return d->upgradeTool; }
AdbTool* SocFlasher::adb() const { return d->adb; }

void SocFlasher::setSerial(const QString& serial)
{
    d->serial = serial;
    d->adb->setSerial(serial);
}

QString SocFlasher::serial() const { return d->serial; }
void SocFlasher::setImagePath(const QString& path) { d->imagePath = path; }
QString SocFlasher::imagePath() const { return d->imagePath; }
void SocFlasher::setWaitTimeoutSeconds(int seconds) { d->waitTimeoutSeconds = seconds; }
int SocFlasher::waitTimeoutSeconds() const { return d->waitTimeoutSeconds; }
void SocFlasher::setAdbReturnTimeoutSeconds(int seconds) { d->adbReturnTimeoutSeconds = seconds; }
int SocFlasher::adbReturnTimeoutSeconds() const { return d->adbReturnTimeoutSeconds; }
void SocFlasher::setPollIntervalMs(int ms) { d->pollIntervalMs = ms; }
int SocFlasher::pollIntervalMs() const { return d->pollIntervalMs; }
void SocFlasher::setWaitForAdb(bool on) { d->waitForAdb = on; }
bool SocFlasher::waitForAdb() const { return d->waitForAdb; }
void SocFlasher::setMtdParts(const QString& mtdparts) { d->mtdParts = mtdparts.trimmed(); }
QString SocFlasher::mtdParts() const { return d->mtdParts; }
void SocFlasher::setExtractDirectory(const QString& dir) { d->extractDirectory = QDir::fromNativeSeparators(dir.trimmed()); }
QString SocFlasher::extractDirectory() const { return d->effectiveExtractDirectory(); }
SocFlasher::FlashMethod SocFlasher::flashMethod() const { return d->method; }

QList<PartitionImage> SocFlasher::partitions() const
{
    // The rows belong to one image: none for another one until it has been planned.
    if (!d->planValid || d->planKey.image != d->imagePath) {
        return {};
    }
    return d->partitions;
}

void SocFlasher::setSelectedPartitions(const QStringList& names)
{
    QStringList cleaned;
    for (const QString& name : names) {
        const QString trimmed = name.trimmed();
        if (!trimmed.isEmpty() && !cleaned.contains(trimmed)) {
            cleaned << trimmed;
        }
    }
    d->selectedNames = cleaned;
    d->applySelection(d->partitions);
}

QStringList SocFlasher::selectedPartitions() const { return d->selectedNames; }
bool SocFlasher::isRunning() const { return d->isRunning(); }
SocFlasher::Phase SocFlasher::phase() const { return d->phase; }
SocFlasher::DeviceStatus SocFlasher::lastStatus() const { return d->status; }

void SocFlasher::queryStatus()
{
    if (d->isRunning() || d->queryBusy) {
        return;   // never overlap, never while the flash owns the USB device
    }
    d->startQuery({}, nullptr);
}

bool SocFlasher::planPartitions()
{
    if (d->isRunning() || d->planRunning || d->imagePath.isEmpty() || d->upgradeTool->toolPath().isEmpty() || !d->runner) {
        return false;   // the flow plans for itself; one plan at a time
    }
    d->startPlan(nullptr, false);
    return true;
}

bool SocFlasher::start() { return d->start(); }
void SocFlasher::cancel() { d->cancel(); }
