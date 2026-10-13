#include "firmware/SocFlasher.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QPointer>
#include <QRegularExpression>
#include <QTimer>

#include <functional>
#include <utility>

#include "app/Logging.h"

namespace {

/// UF is never stopped, but a USB stall can make it hang without a word: after this long without
/// output the log says so (again after every further period), so the user knows what is going on.
constexpr int kUfSilenceLogMs = 5 * 60 * 1000;

/// A rockusb device in Loader / Maskrom mode is one `UF` can write to; anything else `upgrade_tool LD`
/// may list (a mass-storage mode, an unknown mode) is not.
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

} // namespace

/**
 * Two machines share this class: the device-status query and the flash flow. Both are chains of
 * asynchronous commands; every callback captures the id of the chain it belongs to (`queryId`,
 * `flowId`) and does nothing when the chain has been replaced or ended meanwhile, which is how
 * cancel() and endFlow() retire commands that are still on their way.
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
    QTimer* silenceTimer = nullptr;            ///< UF printed nothing for kUfSilenceLogMs
    QElapsedTimer lastFlashOutput;

    QString serial;
    QString imagePath;
    int waitTimeoutSeconds = 30;
    int adbReturnTimeoutSeconds = 60;
    int pollIntervalMs = 1000;
    bool waitForAdb = true;

    DeviceStatus status;
    Phase phase = Phase::Idle;
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

    // Step 2: adb reboot loader
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
            ToolInvocation* reboot = tracked(adb->rebootLoader());
            stepInvocation = reboot;
            QObject::connect(reboot, &ToolInvocation::finished, q, [this, id](const ToolResult& rebootResult) {
                if (id == flowId && !rebootResult.cancelled) {
                    enterWaitingForLoader(false);   // its exit status is ignored, like the script's `|| true`
                }
            });
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
                    startFlash(seen.mode == DeviceStatus::Mode::Maskrom ? QStringLiteral("Maskrom") : QStringLiteral("Loader"));
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
                    startFlash(device.modeText());
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

    // Step 5
    void startFlash(const QString& modeText)
    {
        setPhase(Phase::Flashing);
        const quint64 id = flowId;
        // The path as upgrade_tool gets it (RockchipTool: native separators).
        log(QCoreApplication::translate("SocFlasher", "flashing %1 (%2 mode)").arg(QDir::toNativeSeparators(imagePath), modeText));
        progressParser = RockchipTool::ProgressParser();
        lineBuffer.clear();
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

    void emitToolLine(const QByteArray& raw)
    {
        const QString line = QString::fromUtf8(raw);
        if (!line.trimmed().isEmpty()) {
            emit q->toolOutput(line);
        }
    }

    /// Log only: UF is never stopped by the flasher (a half-written flash needs MASKROM recovery),
    /// the user decides whether to end the process by hand.
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

    void onFlashFinished(const ToolResult& result)
    {
        silenceTimer->stop();
        for (const RockchipTool::Progress& progress : progressParser.flush()) {
            emitProgress(progress);
        }
        emitToolLine(lineBuffer);
        lineBuffer.clear();

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
        if (!waitForAdb) {
            endFlow(Phase::Done, true, QCoreApplication::translate("SocFlasher", "Upgrade firmware ok"));
            return;
        }
        enterWaitingForAdb();
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
    // Waiting commands go; a running UF does not (RockchipTool::upgradeFirmware(): never cancelled).
    d->pollTimer->stop();
    d->silenceTimer->stop();
    d->abortQuery();
    if (d->stepInvocation && !d->stepInvocation->isFinished()) {
        d->stepInvocation->cancel();
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

bool SocFlasher::start() { return d->start(); }
void SocFlasher::cancel() { d->cancel(); }
