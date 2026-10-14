#include "firmware/FirmwareJob.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QMap>
#include <QPointer>
#include <QSet>
#include <QTcpSocket>
#include <QTimer>

#include <utility>

#include "app/Logging.h"
#include "firmware/CcProtocol.h"
#include "firmware/McuLink.h"
#include "firmware/McuUpdater.h"
#include "firmware/ToolRunner.h"
#include "ssh/SshProfile.h"

namespace {

/// A collaborator whose start() refused gets this long to report the reason through its own
/// finished()/failed() (they report "from the event loop") before the job ends the step itself.
constexpr int kSilentRefusalMs = 100;
/// McuLink::close() reports closed() when adb is done; never wait longer than this for it.
constexpr int kLinkCloseTimeoutMs = 10000;
/// A cancelled download ends once the connection removed its ".part"; never wait longer than this.
constexpr int kPullCancelGraceMs = 5000;

// The restore-key prefixes of SessionWidget::restoreKey(), accepted as source targets.
const QLatin1String kProfileKeyPrefix("ssh:profile:");
const QLatin1String kTargetKeyPrefix("ssh:target:");

/// What McuLink::open() runs before its bridge (adb_link.py): an rfmcu left by an earlier session
/// (an open `rfmcu console`, a bridge of a killed task) holds the UART's flock, and `rfmcu cmd`
/// would exit 2 "port busy". Its exit status is ignored; nothing to kill is the usual case.
const QString kStopRfmcuCommand = QStringLiteral("killall rfmcu 2>/dev/null; true");

/// `rfmcu cmd` exit codes (rockchip-platforms project/app/rfmcu/README.md).
constexpr int kRfmcuPortBusy = 2;
constexpr int kRfmcuNoReply = 3;

/// Decimal units in the C locale, exactly as ImagePuller words its log lines ("11.4 MB"), so the
/// pull log never shows one image in two units.
QString formatSize(qint64 bytes)
{
    return QLocale::c().formattedDataSize(qMax<qint64>(0, bytes), 1, QLocale::DataSizeSIFormat);
}

QString formatTime(const QDateTime& time)
{
    return time.isValid() ? time.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")) : QString();
}

QString nativePath(const QString& path)
{
    return QDir::toNativeSeparators(path);
}

} // namespace

// =======================================================================================
// Private
// =======================================================================================

struct FirmwareJob::Private
{
    explicit Private(FirmwareJob* owner) : q(owner) {}

    enum class PullPhase { Idle, Connecting, Pulling };

    FirmwareJob* q;

    // ---- Collaborators ---------------------------------------------------------------------
    QPointer<SshProfileStore> profiles;
    ToolRunner* runner = nullptr;
    SocFlasher* flasher = nullptr;
    McuLink* link = nullptr;
    McuUpdater* updater = nullptr;
    ImagePuller* puller = nullptr;
    SshConnection* ssh = nullptr;
    QString upgradeToolPath;
    QString adbPath;
    QString boardSerial;
    QTimer linkCloseTimer;

    // ---- Decisions -----------------------------------------------------------------------------
    HostKeyHandler hostKeyHandler;
    AuthPromptHandler authPromptHandler;
    DownloadModeHandler downloadModeHandler;
    LayoutProvider layoutProvider;

    // ---- Inputs --------------------------------------------------------------------------------
    QString targetText;
    QString folderText;
    QString cacheDir;
    bool gitDescribeOn = true;
    QStringList extraFileNames;
    SshDefaults defaultsForSsh;

    // ---- Image, package, role, board -------------------------------------------------------------
    QString localImage;             ///< the local pick; empty = the cache's update.img
    CcPackage mcuPackage;
    QString mcuPackagePath;
    quint32 confirmedRole = 0;      ///< 0 none, 1 rx, 2 tx
    QString runningVersion;
    QString runningRole;
    SocFlasher::DeviceStatus deviceStatus;
    bool hasDeviceStatus = false;
    QString boardInfoText;

    // ---- Partitions (v0.5.1) -------------------------------------------------------------------
    PlanState planState = PlanState::NoImage;
    QString planMessage;            ///< SocFlasher::partitionsPlanned()'s message (the reason when it failed)
    QList<PartitionImage> partitionRows;   ///< the rows of plannedImage
    QString plannedImage;
    QString requestedPlanKey;       ///< image, mtime and layout of the last plan asked for
    QString selectionImageKey;      ///< image and mtime the selection belongs to (a new image selects everything)
    bool planInFlight = false;      ///< the job's own SocFlasher::planPartitions() has not reported yet
    bool planPending = false;       ///< a plan could not start (another one, a flash): asked again afterwards
    bool checkAll = true;           ///< every row that can be written is selected
    QStringList checkedNames;       ///< the selected rows while !checkAll
    QString imageLayout;            ///< the mtdparts of plannedImage's env.img ("" = it carries none)
    QString layoutSerial;           ///< the ADB board boardLayout() belongs to (the last one seen)
    QSet<QString> layoutReadTried;  ///< boards whose /proc/cmdline was read by this job
    QPointer<ToolInvocation> cmdlineInvocation;
    QMap<QString, QString> rememberedLayouts;   ///< setMtdParts() and the layouts read: serial ("" = any) -> mtdparts

    // ---- Operation state ---------------------------------------------------------------------
    QString operation;              ///< currentOperation(): the top-level operation
    QString step;                   ///< the step running right now (== operation unless Pull & Flash)
    QString persistent;             ///< persistentStatusMessage()
    bool cancelRequested = false;
    bool keepSsh = false;           ///< Pull & Flash keeps the build-machine connection until it ends
    PullPhase pullPhase = PullPhase::Idle;
    bool pullerActive = false;
    QString pullTarget;             ///< user@host:port of the running pull
    QString sshError;               ///< the connection's last errorOccurred() text
    QString promptRefusal;          ///< the headless answer to an authentication prompt (the step's message)
    bool flasherActive = false;
    bool flashCancellable = false;
    bool linkActive = false;
    bool updaterActive = false;
    bool mcuResultPending = false;  ///< waiting for McuLink::closed() before the step ends
    bool mcuOk = false;
    QString mcuMessage;
    QString linkLostMessage;
    quint32 mcuRole = 0;
    QPointer<ToolInvocation> cliInvocation;   ///< the stale-rfmcu stop or the `rfmcu cmd` under way
    QString pendingVersion;         ///< "version" answered without a role: "help" decides it
    int lastCliExit = -1;           ///< exit code of the last `rfmcu cmd` that ran to its end, else -1
    std::function<void(bool, const QString&)> cliDone;
    bool cancellableShown = false;  ///< what cancellableChanged() said last

    // ---- Construction ------------------------------------------------------------------------
    void wire();
    void log(const QString& line);
    void pushToolPaths();

    // ---- Source / SSH ------------------------------------------------------------------------
    bool resolveProfile(SshProfile* profile, QString* error) const;
    void onHostKey(const SshConnection::HostKeyInfo& info);
    void onAuthPrompt(const SshConnection::AuthPrompt& prompt);

    // ---- Partitions -----------------------------------------------------------------------------
    static QString imageKey(const QString& image);
    QString layoutFor(const QString& serial) const;
    void requestPlan(bool force);
    void onPartitionsPlanned(bool ok, const QList<PartitionImage>& rows, const QString& message);
    void pushSelection();
    void onStatus(const SocFlasher::DeviceStatus& status);
    void readBoardLayout(const QString& serial);
    void storeLayout(const QString& serial, const QString& mtdparts);

    // ---- Operations ----------------------------------------------------------------------------
    bool cancellable() const;
    void updateCancellable();
    bool refuseIfBusy();
    void beginOperation(const QString& op);
    void stepFinished(const QString& stepName, bool ok, const QString& message);
    void endOperation(const QString& stepName, bool ok, const QString& message);
    void setPersistent(const QString& text);
    void setProgressBusy(const QString& text);
    void setProgressPercent(int percent, const QString& text);
    void setProgressText(const QString& text);

    void startPull();
    void startPuller();
    void pullFinished(bool ok, const QString& message);
    void onSshStateChanged(Transport::State state);
    void closeSsh();

    void startFlash();
    void flashFinished(bool ok, const QString& message);
    void onFlasherPhase(SocFlasher::Phase phase);
    void askDownloadMode();
    void showMaskromInstructions();

    void startMcuUpdate(const QString& stepName);
    void onLinkOpened();
    void mcuDone(bool ok, const QString& message);
    void completeMcu();

    void startMcuRead(std::function<void(bool, const QString&)> done);
    void onStaleRfmcuStopped(const ToolResult& result);
    void runCli(const QString& command);
    void onCliFinished(const QString& command, const ToolResult& result);
    void finishCli(bool ok, const QString& message);
    void startChainMcu();
};

// =======================================================================================
// Construction
// =======================================================================================

FirmwareJob::FirmwareJob(ToolRunner* runner, QObject* parent)
    : QObject(parent)
    , d(std::make_unique<Private>(this))
{
    // An own runner becomes a child only after the collaborators that use it: QObject deletes
    // children in creation order, and McuLink's destructor still sends the board's cleanup
    // (`killall rfmcu`, the forward removal) through the runner when the job goes away.
    auto* ownRunner = runner ? nullptr : new ProcessToolRunner;
    d->runner = runner ? runner : ownRunner;
    d->flasher = new SocFlasher(d->runner, this);
    d->link = new McuLink(d->runner, this);
    d->updater = new McuUpdater(this);
    d->puller = new ImagePuller(this);
    d->ssh = new SshConnection(this);
    if (ownRunner) {
        ownRunner->setParent(this);
    }
    // The pull connection is short-lived and on demand: a dropped link ends the pull, it is not
    // re-established behind the user's back.
    d->ssh->setAutoReconnect(false);
    d->puller->setConnection(d->ssh);
    d->updater->setThroughSoc(true);   // the CLI "power" reply is logged before "bootloader"

    d->linkCloseTimer.setSingleShot(true);
    connect(&d->linkCloseTimer, &QTimer::timeout, this, [this]() {
        if (d->mcuResultPending) {
            qCWarning(lcFirmware) << "MCU link did not report closed() in time";
            d->completeMcu();
        }
    });
    d->wire();
}

FirmwareJob::~FirmwareJob()
{
    // The collaborators may still report while they are stopped: every connection into this
    // object is cut before anything is stopped.
    d->linkCloseTimer.stop();
    const QList<QObject*> children = findChildren<QObject*>();
    for (QObject* child : children) {
        disconnect(child, nullptr, this, nullptr);
    }
    if (d->profiles) {
        disconnect(d->profiles, nullptr, this, nullptr);
    }
    for (ToolInvocation* invocation : {d->cliInvocation.data(), d->cmdlineInvocation.data()}) {
        if (invocation) {
            disconnect(invocation, nullptr, this, nullptr);
            invocation->cancel();
        }
    }
    if (d->puller->isRunning()) {
        d->puller->cancel();
    }
    if (d->updater->isRunning()) {
        d->updater->cancel();
    }
    if (d->flasher->isRunning()) {
        // Refused while upgrade_tool writes; the owners never let the job go while it writes,
        // because the runner kills its processes when it is destroyed.
        d->flasher->cancel();
    }
    d->link->close();   // McuLink's destructor (below, before the runner's) sends the cleanup detached
    d->ssh->close();
}

void FirmwareJob::Private::wire()
{
    // ---- Build machine connection (the decisions go to the owner's callbacks) ----
    connect(ssh, &Transport::stateChanged, q, [this](Transport::State state) { onSshStateChanged(state); });
    connect(ssh, &Transport::errorOccurred, q, [this](const QString& error) {
        sshError = error;
        log(FirmwareJob::tr("SSH: %1").arg(error));
    });
    connect(ssh, &SshConnection::authenticated, q, [this](const QString& method) {
        log(FirmwareJob::tr("Authenticated to %1 (%2)").arg(pullTarget, method));
    });
    connect(ssh, &SshConnection::hostKeyVerificationRequired, q,
            [this](const SshConnection::HostKeyInfo& info) { onHostKey(info); });
    connect(ssh, &SshConnection::authPromptRequired, q,
            [this](const SshConnection::AuthPrompt& prompt) { onAuthPrompt(prompt); });

    // ---- Image pull ----
    connect(puller, &ImagePuller::remoteInfo, q,
            [this](const QString& name, qint64 bytes, const QDateTime& modified, const QString& describe) {
                QString text = FirmwareJob::tr("Remote %1: %2, built %3").arg(name, formatSize(bytes), formatTime(modified));
                if (!describe.isEmpty()) {
                    text += QStringLiteral(" (%1)").arg(describe);
                }
                log(text);
                setProgressText(text);
            });
    connect(puller, &ImagePuller::progress, q, [this](qint64 done, qint64 total) {
        if (!pullerActive) {
            return;
        }
        const int percent = total > 0 ? static_cast<int>(done * 100 / total) : -1;
        const QString text = total > 0 ? FirmwareJob::tr("Pulling update.img: %1 / %2 (%3 %)")
                                             .arg(formatSize(done), formatSize(total))
                                             .arg(percent)
                                       : FirmwareJob::tr("Pulling update.img: %1").arg(formatSize(done));
        if (percent >= 0) {
            setProgressPercent(percent, text);
        } else {
            setProgressBusy(text);
        }
        setPersistent(text);
    });
    connect(puller, &ImagePuller::skipped, q, [this](const QString& reason) { log(reason); });
    connect(puller, &ImagePuller::logLine, q, [this](const QString& line) { log(line); });
    connect(puller, &ImagePuller::finished, q, [this](bool ok, const QString& message) {
        if (pullerActive) {
            pullFinished(ok, message);
        }
    });

    // ---- SoC flasher ----
    connect(flasher, &SocFlasher::statusChanged, q, [this](const SocFlasher::DeviceStatus& status) { onStatus(status); });
    connect(flasher, &SocFlasher::mtdPartsRead, q, [this](const QString& mtdparts) {
        // Step 2 of a flash read it from the board the flow found on ADB.
        const QString serial = deviceStatus.mode == SocFlasher::DeviceStatus::Mode::Adb ? deviceStatus.serial : layoutSerial;
        storeLayout(serial, mtdparts);
    });
    connect(flasher, &SocFlasher::partitionsPlanned, q,
            [this](bool ok, const QList<PartitionImage>& rows, const QString& message) { onPartitionsPlanned(ok, rows, message); });
    connect(flasher, &SocFlasher::phaseChanged, q, [this](SocFlasher::Phase phase) { onFlasherPhase(phase); });
    connect(flasher, &SocFlasher::downloadModeNeeded, q, [this]() {
        // Not inside the flasher's emit: the owner's question may run a nested event loop.
        QMetaObject::invokeMethod(q, [this]() { askDownloadMode(); }, Qt::QueuedConnection);
    });
    connect(flasher, &SocFlasher::progress, q, [this](const QString& stage, int percent) {
        if (!flasherActive) {
            return;
        }
        const QString text = percent >= 0 ? FirmwareJob::tr("Flashing SoC: %1 %2 %").arg(stage).arg(percent)
                                          : FirmwareJob::tr("Flashing SoC: %1").arg(stage);
        if (percent >= 0) {
            setProgressPercent(percent, text);
        } else {
            setProgressText(text);
        }
        setPersistent(text);
    });
    connect(flasher, &SocFlasher::toolOutput, q, [this](const QString& line) { emit q->toolOutput(line); });
    connect(flasher, &SocFlasher::logLine, q, [this](const QString& line) { log(line); });
    connect(flasher, &SocFlasher::boardInfo, q, [this](const QString& text) {
        boardInfoText = text.trimmed();
        log(FirmwareJob::tr("Board information:"));
        log(boardInfoText);
        emit q->boardInfoReceived(boardInfoText);
    });
    connect(flasher, &SocFlasher::finished, q, [this](bool ok, const QString& message) {
        if (flasherActive) {
            flashFinished(ok, message);
        } else {
            log(FirmwareJob::tr("SoC flasher: %1").arg(message));   // a late report of a step already ended
        }
    });

    // ---- MCU link / updater ----
    connect(link, &McuLink::serialSelected, q,
            [this](const QString& serial) { log(FirmwareJob::tr("MCU link: board %1").arg(serial)); });
    connect(link, &McuLink::opened, q, [this](const QString&, quint16) { onLinkOpened(); });
    connect(link, &McuLink::failed, q, [this](const QString& message) {
        if (linkActive && !updaterActive && !mcuResultPending) {
            linkActive = false;
            stepFinished(step, false, message);
        }
    });
    connect(link, &McuLink::linkLost, q, [this](const QString& message) {
        linkLostMessage = message;
        log(message);
        if (updaterActive && !updater->isRunning()) {
            updaterActive = false;
            mcuDone(false, message);
        }
    });
    connect(link, &McuLink::closed, q, [this]() {
        if (mcuResultPending) {
            completeMcu();
        }
    });
    connect(link, &McuLink::logLine, q, [this](const QString& line) { log(line); });

    connect(updater, &McuUpdater::logLine, q, [this](const QString& line) { log(line); });
    connect(updater, &McuUpdater::phaseChanged, q, [this](McuUpdater::Phase phase) {
        if (!updaterActive) {
            return;
        }
        QString text;
        switch (phase) {
        case McuUpdater::Phase::ProbeBootloader:
        case McuUpdater::Phase::ProbeApplication:
            text = FirmwareJob::tr("MCU: probing the bootloader and the application");
            break;
        case McuUpdater::Phase::EnterBootloader:
        case McuUpdater::Phase::SettleAfterReset:
        case McuUpdater::Phase::RebootProbe:
            text = FirmwareJob::tr("MCU: rebooting into the UART updater");
            break;
        case McuUpdater::Phase::Banner:
            text = FirmwareJob::tr("MCU: waiting for the application to restart");
            break;
        default:
            break;
        }
        if (!text.isEmpty()) {
            setProgressText(text);
        }
    });
    connect(updater, &McuUpdater::progress, q, [this](qint64 done, qint64 total) {
        if (!updaterActive) {
            return;
        }
        const int percent = total > 0 ? static_cast<int>(done * 100 / total) : 0;
        const QString text = FirmwareJob::tr("Updating MCU: %1 / %2 bytes (%3 %)")
                                 .arg(QLocale().toString(done), QLocale().toString(total))
                                 .arg(percent);
        setProgressPercent(percent, text);
        setPersistent(text);
    });
    connect(updater, &McuUpdater::applicationInfo, q, [this](const McuUpdater::CliFields& fields) {
        if (!fields.value(QStringLiteral("version")).isEmpty()) {
            runningVersion = fields.value(QStringLiteral("version"));
            runningRole = fields.value(QStringLiteral("role"));
            emit q->mcuInfoChanged();
        }
    });
    connect(updater, &McuUpdater::applicationRestarted, q, [this](const McuUpdater::CliFields& fields) {
        if (!fields.value(QStringLiteral("version")).isEmpty()) {
            runningVersion = fields.value(QStringLiteral("version"));
        }
        if (!fields.value(QStringLiteral("role")).isEmpty()) {
            runningRole = fields.value(QStringLiteral("role"));
        }
        emit q->mcuInfoChanged();
    });
    connect(updater, &McuUpdater::finished, q, [this](bool ok, const QString& message) {
        if (!updaterActive) {
            return;
        }
        updaterActive = false;
        mcuDone(ok, (!ok && !linkLostMessage.isEmpty()) ? linkLostMessage : message);
    });
}

void FirmwareJob::Private::log(const QString& line)
{
    // The System Log gets the lines the way the tab's pane shows them: one entry per line, the
    // blank lines of multi-line tool output dropped.
    QString text = line;
    text.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString& part : lines) {
        const QString trimmed = part.trimmed();
        if (trimmed.isEmpty() && lines.size() > 1) {
            continue;
        }
        qCInfo(lcFirmware).noquote() << trimmed;
    }
    emit q->logLine(line);
}

void FirmwareJob::Private::pushToolPaths()
{
    flasher->upgradeTool()->setToolPath(upgradeToolPath);
    flasher->adb()->setAdbPath(adbPath);
    link->adb()->setAdbPath(adbPath);
}

// =======================================================================================
// Accessors and inputs
// =======================================================================================

ToolRunner* FirmwareJob::toolRunner() const { return d->runner; }
SocFlasher* FirmwareJob::socFlasher() const { return d->flasher; }
McuLink* FirmwareJob::mcuLink() const { return d->link; }
McuUpdater* FirmwareJob::mcuUpdater() const { return d->updater; }
ImagePuller* FirmwareJob::imagePuller() const { return d->puller; }
SshConnection* FirmwareJob::sshConnection() const { return d->ssh; }

void FirmwareJob::setProfileStore(SshProfileStore* store)
{
    d->profiles = store;
}

SshProfileStore* FirmwareJob::profileStore() const
{
    return d->profiles;
}

void FirmwareJob::setToolPaths(const QString& upgradeTool, const QString& adb)
{
    d->upgradeToolPath = upgradeTool;
    d->adbPath = adb;
    d->pushToolPaths();
}

QString FirmwareJob::upgradeToolPath() const { return d->upgradeToolPath; }
QString FirmwareJob::adbPath() const { return d->adbPath; }

void FirmwareJob::setSerial(const QString& value)
{
    d->boardSerial = value.trimmed();
    d->flasher->setSerial(d->boardSerial);
    d->link->setSerial(d->boardSerial);
}

QString FirmwareJob::serial() const { return d->boardSerial; }

void FirmwareJob::setLoaderWaitSeconds(int seconds) { d->flasher->setWaitTimeoutSeconds(seconds); }
int FirmwareJob::loaderWaitSeconds() const { return d->flasher->waitTimeoutSeconds(); }
void FirmwareJob::setAdbReturnSeconds(int seconds) { d->flasher->setAdbReturnTimeoutSeconds(seconds); }
int FirmwareJob::adbReturnSeconds() const { return d->flasher->adbReturnTimeoutSeconds(); }

void FirmwareJob::setSourceTarget(const QString& target) { d->targetText = target.trimmed(); }
QString FirmwareJob::sourceTarget() const { return d->targetText; }
void FirmwareJob::setRemoteFolder(const QString& folder) { d->folderText = folder.trimmed(); }
QString FirmwareJob::remoteFolder() const { return d->folderText; }
void FirmwareJob::setCacheDirectory(const QString& dir) { d->cacheDir = QDir::fromNativeSeparators(dir.trimmed()); }
QString FirmwareJob::cacheDirectory() const { return d->cacheDir; }
void FirmwareJob::setGitDescribe(bool on) { d->gitDescribeOn = on; }
bool FirmwareJob::gitDescribe() const { return d->gitDescribeOn; }
void FirmwareJob::setExtraFiles(const QStringList& names) { d->extraFileNames = names; }
QStringList FirmwareJob::extraFiles() const { return d->extraFileNames; }
void FirmwareJob::setSshDefaults(const SshDefaults& defaults) { d->defaultsForSsh = defaults; }
FirmwareJob::SshDefaults FirmwareJob::sshDefaults() const { return d->defaultsForSsh; }

void FirmwareJob::setHostKeyHandler(HostKeyHandler handler) { d->hostKeyHandler = std::move(handler); }
void FirmwareJob::setAuthPromptHandler(AuthPromptHandler handler) { d->authPromptHandler = std::move(handler); }
void FirmwareJob::setDownloadModeHandler(DownloadModeHandler handler) { d->downloadModeHandler = std::move(handler); }

// ---- Image ----------------------------------------------------------------------------------

QString FirmwareJob::imagePath() const
{
    if (!d->localImage.isEmpty()) {
        return d->localImage;
    }
    if (d->cacheDir.isEmpty()) {
        return QString();
    }
    const QString cached = ImagePuller::imagePath(d->cacheDir);
    return QFileInfo(cached).isFile() ? cached : QString();
}

QString FirmwareJob::localImage() const
{
    return d->localImage;
}

bool FirmwareJob::setImagePath(const QString& path, QString* message)
{
    const QString clean = QDir::fromNativeSeparators(path.trimmed());
    if (clean.isEmpty()) {
        d->localImage.clear();
        refreshImage();
        return true;
    }
    QString error;
    if (!QFileInfo(clean).isFile()) {
        error = tr("file not found");
    } else if (!RockchipTool::isRkfwImage(clean, &error)) {
        if (error.isEmpty()) {
            error = tr("not a Rockchip RKFW image");
        }
    }
    if (!error.isEmpty()) {
        const QString text = tr("Image %1 refused: %2").arg(nativePath(clean), error);
        d->log(text);
        if (message) {
            *message = text;
        }
        return false;
    }
    d->localImage = clean;
    d->log(tr("Using the local image %1").arg(nativePath(clean)));
    refreshImage();
    return true;
}

void FirmwareJob::useLocalImage(const QString& path)
{
    d->localImage = QDir::fromNativeSeparators(path.trimmed());
}

ImagePuller::CacheEntry FirmwareJob::cacheEntry() const
{
    return d->cacheDir.isEmpty() ? ImagePuller::CacheEntry() : ImagePuller::cachedImage(d->cacheDir);
}

// ---- MCU ------------------------------------------------------------------------------------

bool FirmwareJob::setPackagePath(const QString& path, QString* message)
{
    const QString clean = QDir::fromNativeSeparators(path.trimmed());
    if (clean.isEmpty()) {
        d->mcuPackage = CcPackage();
        d->mcuPackagePath.clear();
        emit packageChanged();
        return false;
    }
    CcPackage loaded;
    QString error;
    if (!CcPackage::load(clean, &loaded, &error)) {
        if (error.isEmpty()) {
            error = tr("unreadable file");
        }
        const QString text = tr("Package %1 refused: %2").arg(QFileInfo(clean).fileName(), error);
        d->log(text);
        if (message) {
            *message = text;
        }
        return false;
    }
    d->mcuPackage = loaded;
    d->mcuPackagePath = clean;
    d->log(tr("Package %1: %2").arg(QFileInfo(clean).fileName(), loaded.summary()));
    emit packageChanged();
    return true;
}

void FirmwareJob::setPackage(const CcPackage& loaded)
{
    d->mcuPackage = loaded;
    d->mcuPackagePath = loaded.isValid() ? loaded.filePath : QString();
    emit packageChanged();
}

QString FirmwareJob::packagePath() const { return d->mcuPackagePath; }
CcPackage FirmwareJob::package() const { return d->mcuPackage; }

void FirmwareJob::setRole(quint32 newRole)
{
    const quint32 valid = (newRole == CcProtocol::RoleRx || newRole == CcProtocol::RoleTx) ? newRole : 0;
    if (valid == d->confirmedRole) {
        return;
    }
    d->confirmedRole = valid;
    emit roleChanged(valid);
}

quint32 FirmwareJob::role() const { return d->confirmedRole; }
QString FirmwareJob::runningMcuVersion() const { return d->runningVersion; }
QString FirmwareJob::runningMcuRole() const { return d->runningRole; }

// ---- Board ----------------------------------------------------------------------------------

bool FirmwareJob::hasStatus() const { return d->hasDeviceStatus; }
SocFlasher::DeviceStatus FirmwareJob::lastStatus() const { return d->deviceStatus; }
QString FirmwareJob::boardInfo() const { return d->boardInfoText; }

QString FirmwareJob::maskromInstructions() const
{
    return tr("No board found: hold the MASKROM button while plugging in the USB-C cable "
              "(or while power-cycling the board); waiting up to %1 s")
        .arg(d->flasher->waitTimeoutSeconds());
}

// ---- State ----------------------------------------------------------------------------------

bool FirmwareJob::isBusy() const { return !d->operation.isEmpty(); }
QString FirmwareJob::currentOperation() const { return d->operation; }
QString FirmwareJob::currentStep() const { return d->step; }
bool FirmwareJob::isCancellable() const { return d->cancellable(); }

QString FirmwareJob::persistentStatusMessage() const
{
    return isBusy() ? d->persistent : QString();
}

QString FirmwareJob::operationTitle(const QString& op)
{
    if (op == kOpPull) {
        return tr("Pull");
    }
    if (op == kOpFlash) {
        return tr("Flash SoC");
    }
    if (op == kOpMcu) {
        return tr("Update MCU");
    }
    if (op == kOpSetRole) {
        return tr("Set role");
    }
    if (op == kOpInfo) {
        return tr("Read MCU");
    }
    if (op == kOpPullFlash) {
        return tr("Pull & Flash");
    }
    return op;
}

QString FirmwareJob::resultText(const QString& op, bool ok, const QString& message)
{
    return ok ? tr("%1 done: %2").arg(operationTitle(op), message) : tr("%1 failed: %2").arg(operationTitle(op), message);
}

QString FirmwareJob::roleText(quint32 value)
{
    switch (value) {
    case CcProtocol::RoleRx:
        return QStringLiteral("rx");
    case CcProtocol::RoleTx:
        return QStringLiteral("tx");
    default:
        return QString();
    }
}

quint32 FirmwareJob::roleFromText(const QString& text)
{
    const QString t = text.trimmed().toLower();
    if (t == QLatin1String("rx")) {
        return CcProtocol::RoleRx;
    }
    if (t == QLatin1String("tx")) {
        return CcProtocol::RoleTx;
    }
    return 0;
}

// =======================================================================================
// Source / SSH
// =======================================================================================

bool FirmwareJob::Private::resolveProfile(SshProfile* profile, QString* error) const
{
    QString text = targetText;
    QString id;
    if (text.startsWith(kProfileKeyPrefix)) {
        id = text.mid(kProfileKeyPrefix.size());
    }
    if (!id.isEmpty()) {
        const std::optional<SshProfile> stored = profiles ? profiles->profile(id) : std::nullopt;
        if (stored) {
            *profile = *stored;
            return true;
        }
        *error = FirmwareJob::tr("the SSH profile of the build machine no longer exists");
        return false;
    }
    if (text.isEmpty()) {
        *error = FirmwareJob::tr("enter the build machine (an SSH profile or user@host)");
        return false;
    }
    if (text.startsWith(kTargetKeyPrefix)) {
        text = text.mid(kTargetKeyPrefix.size());
    }
    if (profiles) {
        if (const std::optional<SshProfile> byName = profiles->profileByName(text)) {
            *profile = *byName;
            return true;
        }
    }
    SshProfile adHoc;
    if (!SshProfile::parseTarget(text, adHoc)) {
        *error = FirmwareJob::tr("\"%1\" is not an SSH target such as user@host[:port]").arg(text);
        return false;
    }
    *profile = adHoc;
    return true;
}

void FirmwareJob::Private::onHostKey(const SshConnection::HostKeyInfo& info)
{
    // A copy: the owner's question may run a nested event loop during which the handler is replaced.
    const HostKeyHandler handler = hostKeyHandler;
    HostKeyDecision decision;   // headless: refused, exactly as when the user refuses
    if (handler) {
        const QPointer<FirmwareJob> self(q);
        decision = handler(info);
        if (!self) {
            return;   // the owner went away during the question
        }
    }
    const bool accepted = decision.accept;
    const bool remember = accepted && decision.remember;
    qCInfo(lcFirmware) << "host key" << info.keyType << info.fingerprintSha256 << (accepted ? "accepted" : "rejected")
                       << (remember ? "(remembered)" : "");
    ssh->answerHostKey(accepted, remember);
}

void FirmwareJob::Private::onAuthPrompt(const SshConnection::AuthPrompt& prompt)
{
    const AuthPromptHandler handler = authPromptHandler;
    if (!handler) {
        // Headless: nobody can answer, so the connect ends; the step says which question it was.
        const QString question = prompt.prompt.trimmed().isEmpty() ? prompt.title.trimmed() : prompt.prompt.trimmed();
        promptRefusal = FirmwareJob::tr("authentication failed: nobody answered \"%1\"").arg(question);
        qCInfo(lcFirmware) << "authentication prompt left unanswered: no handler";
        ssh->cancelPrompt();
        return;
    }
    const QPointer<FirmwareJob> self(q);
    const std::optional<AuthAnswer> answer = handler(prompt);
    if (!self) {
        return;
    }
    if (answer) {
        ssh->answerPrompt(answer->response, answer->remember);
    } else {
        qCInfo(lcFirmware) << "authentication prompt cancelled";
        ssh->cancelPrompt();
    }
}

// =======================================================================================
// Partitions (v0.5.1): SocFlasher::planPartitions() on image / layout changes
// =======================================================================================

QString FirmwareJob::Private::imageKey(const QString& image)
{
    return image + QLatin1Char('\n') + QString::number(QFileInfo(image).lastModified().toMSecsSinceEpoch());
}

QString FirmwareJob::Private::layoutFor(const QString& serial) const
{
    // What this job read or was given first (the freshest), then the owner's memory.
    const QString own = rememberedLayouts.value(serial);
    if (!own.isEmpty() || !layoutProvider) {
        return own;
    }
    return layoutProvider(serial);
}

void FirmwareJob::Private::requestPlan(bool force)
{
    const QString image = q->imagePath();
    if (image.isEmpty()) {
        requestedPlanKey.clear();
        planState = PlanState::NoImage;
        partitionRows.clear();
        plannedImage.clear();
        emit q->partitionsChanged();
        return;
    }
    // The board's layout is what the flasher gets; the image's own (env.img) is its business.
    const QString layout = q->boardLayout();
    const QString key = imageKey(image) + QLatin1Char('\n') + layout;
    if (!force && key == requestedPlanKey) {
        return;
    }
    requestedPlanKey = key;
    if (imageKey(image) != selectionImageKey) {
        selectionImageKey = imageKey(image);   // another image: everything that can be written is selected again
        checkAll = true;
        checkedNames.clear();
    }
    if (upgradeToolPath.isEmpty()) {
        planState = PlanState::Failed;
        planMessage = FirmwareJob::tr("upgrade_tool not found");
        partitionRows.clear();
        emit q->partitionsChanged();
        return;
    }
    if (planInFlight || flasher->isRunning()) {
        planPending = true;   // asked again when that plan / the flash reports
        return;
    }
    pushToolPaths();
    flasher->setImagePath(image);
    flasher->setMtdParts(layout);
    pushSelection();
    if (!flasher->planPartitions()) {
        planPending = true;
        return;
    }
    planPending = false;
    planInFlight = true;
    planState = PlanState::Planning;
    emit q->partitionsChanged();
}

void FirmwareJob::Private::onPartitionsPlanned(bool ok, const QList<PartitionImage>& rows, const QString& message)
{
    planInFlight = false;
    planState = ok ? PlanState::Ready : PlanState::Failed;
    planMessage = message;
    partitionRows = ok ? rows : QList<PartitionImage>();
    plannedImage = flasher->imagePath();
    // The layout the image carries itself, read the way the flasher read it for these rows.
    imageLayout = ok ? RockchipTool::mtdPartsFromEnvImage(flasher->extractDirectory() + QStringLiteral("/env.img")) : QString();
    emit q->partitionsChanged();
    const QString imageName = QFileInfo(plannedImage).fileName();
    if (ok) {
        QStringList list;
        for (const PartitionImage& row : rows) {
            list << (row.inLayout ? QStringLiteral("%1@0x%2").arg(row.name).arg(row.offset, 0, 16) : row.name);
        }
        log(FirmwareJob::tr("Partitions of %1: %2").arg(imageName, list.join(QStringLiteral(", "))));
        if (!q->layoutKnown()) {
            log(FirmwareJob::tr("The flash layout is not known yet: the image carries none, and it is read from "
                                "the board once it runs (ADB); MASKROM mode needs none"));
        }
    } else {
        log(FirmwareJob::tr("Cannot list the partitions of %1: %2").arg(imageName, message));
    }
    emit q->partitionsPlanned();
    if (planPending && !flasher->isRunning()) {
        planPending = false;
        requestPlan(true);
    }
}

void FirmwareJob::Private::pushSelection()
{
    // "None" has no SocFlasher form (an empty list means every row): Flash SoC refuses it instead.
    if (checkAll) {
        flasher->setSelectedPartitions({});
    } else if (!checkedNames.isEmpty()) {
        flasher->setSelectedPartitions(checkedNames);
    }
}

void FirmwareJob::Private::onStatus(const SocFlasher::DeviceStatus& status)
{
    deviceStatus = status;
    hasDeviceStatus = true;
    emit q->statusChanged(status);
    if (status.mode == SocFlasher::DeviceStatus::Mode::Adb && !status.serial.isEmpty()) {
        // A running board can tell its flash layout; Loader mode later cannot. Read it once per
        // board and job, only while nothing else uses adb.
        const bool otherBoard = status.serial != layoutSerial;
        layoutSerial = status.serial;
        if (operation.isEmpty() && !cmdlineInvocation && !layoutReadTried.contains(status.serial)
            && layoutFor(status.serial).isEmpty()) {
            readBoardLayout(status.serial);
        }
        if (otherBoard) {
            requestPlan(false);   // flashLayout() follows the board
        }
    }
}

void FirmwareJob::Private::readBoardLayout(const QString& serial)
{
    layoutReadTried.insert(serial);
    pushToolPaths();
    AdbTool* tool = flasher->adb();
    if (tool->adbPath().isEmpty()) {
        return;
    }
    // The board the status shows, named like every adb command of the flow.
    ToolInvocation* invocation =
        tool->serial().isEmpty()
            ? tool->run({QStringLiteral("-s"), serial, QStringLiteral("shell"), QStringLiteral("cat /proc/cmdline")}, tool->timeoutMs())
            : tool->shell(QStringLiteral("cat /proc/cmdline"));
    cmdlineInvocation = invocation;
    log(invocation->commandLine());
    connect(invocation, &ToolInvocation::finished, q, [this, invocation, serial](const ToolResult& result) {
        if (cmdlineInvocation != invocation) {
            return;
        }
        cmdlineInvocation = nullptr;
        if (result.cancelled) {
            return;
        }
        if (!result.started) {
            log(FirmwareJob::tr("Cannot read the flash layout of board %1: %2").arg(serial, result.error));
            return;
        }
        const QString mtdparts = RockchipTool::mtdPartsFromCommandLine(result.outputText());
        if (mtdparts.isEmpty()) {
            log(FirmwareJob::tr("Board %1 reports no flash layout (no mtdparts in /proc/cmdline)").arg(serial));
            return;
        }
        log(FirmwareJob::tr("Flash layout of board %1: %2").arg(serial, mtdparts));
        storeLayout(serial, mtdparts);
    });
}

void FirmwareJob::Private::storeLayout(const QString& serial, const QString& mtdparts)
{
    if (!serial.isEmpty()) {
        rememberedLayouts.insert(serial, mtdparts);
        layoutSerial = serial;
    }
    rememberedLayouts.insert(QString(), mtdparts);   // the layout for a board met in Loader mode
    emit q->mtdPartsRead(serial, mtdparts);           // before the plan: the owner stores it first
    requestPlan(false);
}

QString FirmwareJob::partitionProblem(const PartitionImage& row, bool layoutIsKnown)
{
    if (!layoutIsKnown) {
        return tr("partition layout unknown: connect the board running once");
    }
    if (!row.inLayout) {
        return tr("%1 is not in the board's partition layout").arg(row.name);
    }
    if (row.fileSize < 0) {
        return tr("the package has no image file for %1").arg(row.name);
    }
    if (!row.fits) {
        return tr("%1 (%2 bytes) does not fit the partition (%3 bytes)")
            .arg(QFileInfo(row.file).fileName())
            .arg(row.fileSize)
            .arg(row.size);
    }
    return QString();
}

FirmwareJob::PlanState FirmwareJob::planState() const { return d->planState; }
QString FirmwareJob::planMessage() const { return d->planMessage; }

QList<PartitionImage> FirmwareJob::partitions() const
{
    return d->planState == PlanState::Ready ? d->partitionRows : QList<PartitionImage>();
}

QString FirmwareJob::plannedImage() const { return d->plannedImage; }

QString FirmwareJob::flashLayout() const
{
    // What the flasher places the partitions in (SocFlasher step 5): the image's own layout when
    // its env.img carries one, else the board's.
    return usingImageLayout() ? d->imageLayout : boardLayout();
}

QString FirmwareJob::boardLayout() const
{
    // The board's own layout when it is known; else the one read last (""), which is the best guess
    // for a board that is already in Loader mode (rockusb tells no ADB serial).
    if (!d->layoutSerial.isEmpty()) {
        const QString own = d->layoutFor(d->layoutSerial);
        if (!own.isEmpty()) {
            return own;
        }
    }
    return d->layoutFor(QString());
}

QString FirmwareJob::imageLayout() const { return d->imageLayout; }

bool FirmwareJob::usingImageLayout() const
{
    return d->planState == PlanState::Ready && d->plannedImage == imagePath()
           && !RockchipTool::parseMtdParts(d->imageLayout).isEmpty();
}

bool FirmwareJob::layoutKnown() const
{
    return !RockchipTool::parseMtdParts(flashLayout()).isEmpty();
}

void FirmwareJob::setLayoutProvider(LayoutProvider provider)
{
    d->layoutProvider = std::move(provider);
}

void FirmwareJob::setMtdParts(const QString& mtdparts, const QString& serial)
{
    if (mtdparts.trimmed().isEmpty()) {
        d->rememberedLayouts.remove(serial);
    } else {
        d->rememberedLayouts.insert(serial, mtdparts.trimmed());
    }
}

QString FirmwareJob::mtdParts(const QString& serial) const
{
    return d->layoutFor(serial);
}

void FirmwareJob::selectAllPartitions()
{
    d->checkAll = true;
    d->checkedNames.clear();
    emit partitionsChanged();
    d->pushSelection();
}

void FirmwareJob::setSelectedPartitions(const QStringList& names)
{
    QStringList cleaned;
    for (const QString& name : names) {
        const QString trimmed = name.trimmed();
        if (!trimmed.isEmpty() && !cleaned.contains(trimmed)) {
            cleaned << trimmed;
        }
    }
    d->checkAll = false;
    d->checkedNames = cleaned;
    emit partitionsChanged();
    d->pushSelection();
}

bool FirmwareJob::allPartitionsSelected() const
{
    return d->checkAll;
}

QStringList FirmwareJob::selectedPartitions() const
{
    QStringList names;
    if (d->planState != PlanState::Ready) {
        return names;
    }
    const bool known = layoutKnown();
    for (const PartitionImage& row : std::as_const(d->partitionRows)) {
        if (partitionProblem(row, known).isEmpty() && (d->checkAll || d->checkedNames.contains(row.name))) {
            names << row.name;
        }
    }
    return names;
}

// =======================================================================================
// Operations: framework
// =======================================================================================

bool FirmwareJob::Private::cancellable() const
{
    if (operation.isEmpty()) {
        return false;
    }
    if (step == kOpFlash) {
        return flashCancellable;   // never while upgrade_tool writes the flash
    }
    return step == kOpPull || step == kOpMcu || step == kOpSetRole || step == kOpInfo || cliInvocation;
}

void FirmwareJob::Private::updateCancellable()
{
    const bool now = cancellable();
    if (now != cancellableShown) {
        cancellableShown = now;
        emit q->cancellableChanged(now);
    }
}

bool FirmwareJob::Private::refuseIfBusy()
{
    if (operation.isEmpty()) {
        return false;
    }
    emit q->busyRefused(FirmwareJob::tr("Busy: %1 is still running").arg(FirmwareJob::operationTitle(operation)));
    return true;
}

void FirmwareJob::Private::beginOperation(const QString& op)
{
    operation = op;
    step.clear();
    cancelRequested = false;
    log(FirmwareJob::tr("--- %1 ---").arg(FirmwareJob::operationTitle(op)));
    setProgressBusy(FirmwareJob::operationTitle(op));
    setPersistent(QStringLiteral("%1...").arg(FirmwareJob::operationTitle(op)));
    updateCancellable();
    qCInfo(lcFirmware) << "operation" << op << "started";
    emit q->busyChanged(true);
}

void FirmwareJob::Private::stepFinished(const QString& stepName, bool ok, const QString& message)
{
    if (operation.isEmpty()) {
        return;   // a late report of an operation that already ended
    }
    step.clear();
    if (operation != kOpPullFlash) {
        emit q->stepFinished(stepName, ok, message);
        endOperation(stepName, ok, message);
        return;
    }
    // Pull & Flash: report the step, then go on or stop at the first failure.
    log(ok ? FirmwareJob::tr("%1: %2").arg(FirmwareJob::operationTitle(stepName), message)
           : FirmwareJob::tr("%1 failed: %2").arg(FirmwareJob::operationTitle(stepName), message));
    emit q->stepFinished(stepName, ok, message);
    if (!ok) {
        keepSsh = false;
        closeSsh();
        endOperation(kOpPullFlash, false,
                     FirmwareJob::tr("stopped at %1: %2").arg(FirmwareJob::operationTitle(stepName), message));
        return;
    }
    if (stepName == kOpPull) {
        startFlash();
    } else if (stepName == kOpFlash) {
        startChainMcu();
    } else {
        keepSsh = false;
        closeSsh();
        endOperation(kOpPullFlash, true, message);
    }
}

void FirmwareJob::Private::endOperation(const QString& stepName, bool ok, const QString& message)
{
    const QString op = operation;
    operation.clear();
    step.clear();
    persistent.clear();   // the result message replaces the hint
    cancelRequested = false;
    flashCancellable = false;
    log(FirmwareJob::resultText(op, ok, message));
    qCInfo(lcFirmware) << "operation" << op << (ok ? "succeeded" : "failed");
    updateCancellable();
    emit q->busyChanged(false);
    emit q->operationFinished(stepName, ok, message);
    if (ok && (stepName == kOpFlash || op == kOpPullFlash)) {
        q->refreshStatus();   // the board as it came back
    }
}

void FirmwareJob::Private::setPersistent(const QString& text)
{
    if (text == persistent) {
        return;
    }
    persistent = text;
    emit q->persistentStatusChanged(text);
}

void FirmwareJob::Private::setProgressBusy(const QString& text)
{
    emit q->progress(text, kProgressBusy);
}

void FirmwareJob::Private::setProgressPercent(int percent, const QString& text)
{
    emit q->progress(text, qBound(0, percent, 100));
}

void FirmwareJob::Private::setProgressText(const QString& text)
{
    emit q->progress(text, kProgressTextOnly);
}

// =======================================================================================
// Pull
// =======================================================================================

void FirmwareJob::Private::startPull()
{
    step = kOpPull;
    promptRefusal.clear();
    updateCancellable();
    SshProfile profile;
    QString error;
    if (!resolveProfile(&profile, &error)) {
        stepFinished(kOpPull, false, error);
        return;
    }
    const QString folder = folderText;
    if (folder.isEmpty()) {
        stepFinished(kOpPull, false, FirmwareJob::tr("enter the remote folder that holds update.img"));
        return;
    }
    const QString cache = cacheDir;
    if (cache.isEmpty() || !QDir().mkpath(cache)) {
        stepFinished(kOpPull, false, FirmwareJob::tr("cannot create the cache folder %1").arg(nativePath(cache)));
        return;
    }
    emit q->pullSourceAccepted(targetText, folder, cache);

    // As SessionWidget::connectSsh(): the store first (its changed() lets a view rebuild its list),
    // then the SSH defaults fill what the profile leaves open.
    if (profiles) {
        if (!profile.id.isEmpty()) {
            profiles->touch(profile.id);
        } else {
            profiles->addRecentTarget(profile.displayTarget());
        }
    }
    if (profile.knownHostsFile.trimmed().isEmpty()) {
        profile.knownHostsFile = defaultsForSsh.knownHostsFile;
    }
    if (profile.id.isEmpty()) {
        if (profile.identityFile.trimmed().isEmpty()) {
            profile.identityFile = defaultsForSsh.identityFile;
        }
        if (!defaultsForSsh.terminalType.isEmpty()) {
            profile.terminalType = defaultsForSsh.terminalType;
        }
        profile.keepAliveSeconds = defaultsForSsh.keepAliveSeconds;
    }
    profile.remoteCommand.clear();    // a plain session: the pull runs its own exec / SFTP channels
    profile.startupCommand.clear();
    profile.localForwards.clear();

    pullTarget = profile.displayTarget();
    if (ssh->state() == Transport::State::Connected && ssh->profile().displayTarget() == pullTarget) {
        startPuller();   // still open (a Pull & Flash, or a pull right after a cancelled one)
        return;
    }
    closeSsh();
    sshError.clear();
    ssh->setProfile(profile);
    pullPhase = PullPhase::Connecting;
    const QString text = FirmwareJob::tr("Pull: connecting to %1...").arg(pullTarget);
    setProgressBusy(text);
    setPersistent(text);
    updateCancellable();
    log(FirmwareJob::tr("Connecting to %1").arg(pullTarget));
    qCInfo(lcFirmware) << "pull: connecting to" << pullTarget;
    if (!ssh->open()) {
        pullPhase = PullPhase::Idle;
        const QString reason = ssh->errorString();
        stepFinished(kOpPull, false, reason.isEmpty() ? FirmwareJob::tr("cannot connect to %1").arg(pullTarget) : reason);
    }
}

void FirmwareJob::Private::onSshStateChanged(Transport::State state)
{
    if (pullPhase == PullPhase::Connecting) {
        if (state == Transport::State::Connected) {
            log(FirmwareJob::tr("Connected to %1 (%2)").arg(pullTarget, ssh->summary()));
            startPuller();
        } else if (state == Transport::State::Disconnected) {
            pullPhase = PullPhase::Idle;
            const QString reason = cancelRequested           ? FirmwareJob::tr("cancelled")
                                   : !promptRefusal.isEmpty() ? promptRefusal
                                   : sshError.isEmpty()       ? FirmwareJob::tr("the connection to %1 closed").arg(pullTarget)
                                                              : sshError;
            stepFinished(kOpPull, false, reason);
        }
        return;
    }
    if (pullPhase == PullPhase::Pulling && state == Transport::State::Disconnected) {
        // The puller reports the dropped connection itself; make sure the step ends regardless.
        QTimer::singleShot(kSilentRefusalMs, q, [this]() {
            if (pullerActive && ssh->state() == Transport::State::Disconnected) {
                puller->cancel();
                pullFinished(false, sshError.isEmpty() ? FirmwareJob::tr("the connection to %1 closed").arg(pullTarget) : sshError);
            }
        });
    }
}

void FirmwareJob::Private::startPuller()
{
    pullPhase = PullPhase::Pulling;
    puller->setConnection(ssh);
    puller->setRemoteFolder(folderText);
    puller->setCacheDirectory(cacheDir);
    puller->setSdkDirectory(QString());   // derived from the remote folder
    puller->setGitDescribe(gitDescribeOn);
    puller->setExtraFiles(extraFileNames);
    puller->setForceDownload(false);
    const QString text = FirmwareJob::tr("Pull: checking update.img on %1...").arg(pullTarget);
    setProgressBusy(text);
    setPersistent(text);
    pullerActive = true;
    if (!puller->start()) {
        QTimer::singleShot(kSilentRefusalMs, q, [this]() {
            if (pullerActive && !puller->isRunning()) {
                pullFinished(false, FirmwareJob::tr("the image pull did not start"));
            }
        });
    }
}

void FirmwareJob::Private::pullFinished(bool ok, const QString& message)
{
    pullerActive = false;
    pullPhase = PullPhase::Idle;
    if (!keepSsh) {
        closeSsh();   // the connection is only needed while pulling
    }
    if (ok) {
        localImage.clear();   // the freshly pulled (or unchanged) cache image is what Flash SoC uses from now on
    }
    q->refreshImage(/*reread=*/ok);
    stepFinished(kOpPull, ok, message);
}

void FirmwareJob::Private::closeSsh()
{
    if (ssh->state() == Transport::State::Disconnected) {
        return;
    }
    if (pullPhase == PullPhase::Connecting) {
        pullPhase = PullPhase::Idle;   // the Disconnected that close() reports is no pull result
    }
    ssh->close();
    qCInfo(lcFirmware) << "build machine connection closed";
}

// =======================================================================================
// SoC
// =======================================================================================

void FirmwareJob::Private::startFlash()
{
    step = kOpFlash;
    updateCancellable();
    const QString image = q->imagePath();
    if (image.isEmpty()) {
        stepFinished(kOpFlash, false, FirmwareJob::tr("no image: pull one or choose a local image first"));
        return;
    }
    pushToolPaths();
    flasher->setImagePath(image);
    flasher->setMtdParts(q->boardLayout());   // a board already in Loader mode cannot tell its layout
    if (operation == kOpPullFlash) {
        // Pull & Flash writes every partition: the SocFlasher default, which also stops before
        // writing anything when one image does not fit.
        checkAll = true;
        checkedNames.clear();
        emit q->partitionsChanged();
        flasher->setSelectedPartitions({});
        log(FirmwareJob::tr("Partitions: every partition of the image (Loader mode writes them one by one; "
                            "MASKROM mode writes the whole image)"));
    } else {
        // The selection applies to a plan of this very image with rows that can be written;
        // without one (still unpacking, no layout) the flasher decides: every partition in Loader
        // mode, the whole image in MASKROM mode.
        bool anyWritable = false;
        if (planState == PlanState::Ready) {
            const bool known = q->layoutKnown();
            for (const PartitionImage& row : std::as_const(partitionRows)) {
                if (FirmwareJob::partitionProblem(row, known).isEmpty()) {
                    anyWritable = true;
                    break;
                }
            }
        }
        const bool selectionApplies = planState == PlanState::Ready && plannedImage == image && anyWritable;
        const QStringList checked = q->selectedPartitions();
        if (selectionApplies && checked.isEmpty()) {
            stepFinished(kOpFlash, false, FirmwareJob::tr("no partition is checked: check the ones to write (All checks every one)"));
            return;
        }
        flasher->setSelectedPartitions(selectionApplies ? checked : QStringList());
        log(selectionApplies ? FirmwareJob::tr("Partitions checked: %1 (Loader mode writes these one by one; "
                                               "MASKROM mode writes the whole image)")
                                   .arg(checked.join(QStringLiteral(", ")))
                             : FirmwareJob::tr("Partitions: every partition of the image (Loader mode writes them "
                                               "one by one; MASKROM mode writes the whole image)"));
    }
    boardInfoText.clear();
    log(FirmwareJob::tr("Flashing %1").arg(nativePath(image)));
    const QString text = FirmwareJob::tr("Flashing SoC: starting...");
    setProgressBusy(text);
    setPersistent(text);
    flasherActive = true;
    flashCancellable = true;   // detecting the board is a waiting phase
    updateCancellable();
    if (!flasher->start()) {
        // A refusal reports its reason through finished() from the event loop (handled above);
        // one that stays silent (already running) ends the step here.
        QTimer::singleShot(kSilentRefusalMs, q, [this]() {
            if (flasherActive && !flasher->isRunning()) {
                flashFinished(false, FirmwareJob::tr("the SoC flasher did not start"));
            }
        });
    }
}

void FirmwareJob::Private::onFlasherPhase(SocFlasher::Phase phase)
{
    if (!flasherActive) {
        return;
    }
    QString text;
    bool waiting = false;
    switch (phase) {
    case SocFlasher::Phase::Detecting:
        text = FirmwareJob::tr("Flashing SoC: looking for the board");
        waiting = true;
        break;
    case SocFlasher::Phase::RebootingToLoader:
        text = FirmwareJob::tr("Flashing SoC: rebooting the board into Loader mode");
        waiting = true;
        break;
    case SocFlasher::Phase::WaitingForLoader:
        text = FirmwareJob::tr("Flashing SoC: waiting for the board in Loader / Maskrom mode");
        waiting = true;
        break;
    case SocFlasher::Phase::Flashing:
        text = FirmwareJob::tr("Flashing SoC: writing the flash - do not unplug the board");
        break;
    case SocFlasher::Phase::WaitingForAdb:
        text = FirmwareJob::tr("Flashing SoC: waiting for the board to come back on ADB");
        waiting = true;
        break;
    case SocFlasher::Phase::ReadingBoardInfo:
        text = FirmwareJob::tr("Flashing SoC: reading the board information");
        break;
    default:
        break;
    }
    flashCancellable = waiting;
    updateCancellable();
    if (!text.isEmpty()) {
        if (phase == SocFlasher::Phase::Flashing) {
            setProgressPercent(0, text);
        } else {
            setProgressBusy(text);
        }
        setPersistent(text);
    }
}

void FirmwareJob::Private::flashFinished(bool ok, const QString& message)
{
    flasherActive = false;
    flashCancellable = false;
    stepFinished(kOpFlash, ok, message);
    if (planPending && !flasher->isRunning()) {
        planPending = false;
        requestPlan(true);   // a layout read during the flash, or an image changed meanwhile
    }
}

void FirmwareJob::Private::askDownloadMode()
{
    if (!flasherActive) {
        return;
    }
    const DownloadModeHandler handler = downloadModeHandler;   // a copy: the question may run an event loop
    DownloadModeAnswer answer;
    if (handler) {
        const QPointer<FirmwareJob> self(q);
        answer = handler();
        if (!self) {
            return;   // the owner went away meanwhile
        }
    }
    if (answer.asked || answer.send) {
        const SocFlasher::Phase now = flasher->phase();
        if (!flasherActive || now != SocFlasher::Phase::WaitingForLoader) {
            // The flasher stopped waiting meanwhile: the board appeared (the user held MASKROM and
            // replugged; the flash may even be done - "reboot loader" now would take a freshly
            // flashed board back into Loader mode and the ADB wait would time out), or the wait ended.
            if (now != SocFlasher::Phase::Failed && now != SocFlasher::Phase::Cancelled) {
                log(FirmwareJob::tr("The board appeared meanwhile; nothing sent"));
            }
            return;
        }
    }
    if (answer.send) {
        const QString through = answer.send();
        if (!through.isEmpty()) {
            log(FirmwareJob::tr("Sent \"reboot loader\" through %1").arg(through));
            return;
        }
    }
    showMaskromInstructions();
}

void FirmwareJob::Private::showMaskromInstructions()
{
    const QString text = q->maskromInstructions();
    log(text);
    setProgressText(text);
    setPersistent(text);
}

// =======================================================================================
// MCU
// =======================================================================================

void FirmwareJob::Private::startMcuUpdate(const QString& stepName)
{
    step = stepName;
    updateCancellable();
    if (stepName == kOpMcu && !mcuPackage.isValid()) {
        stepFinished(stepName, false, FirmwareJob::tr("choose a valid .pkg package first"));
        return;
    }
    if (confirmedRole == 0) {
        stepFinished(stepName, false, FirmwareJob::tr("choose the MCU role (rx or tx) first: it is never guessed"));
        return;
    }
    mcuRole = confirmedRole;
    mcuResultPending = false;
    linkLostMessage.clear();
    if (stepName == kOpMcu) {
        updater->setPackage(mcuPackage);
        log(FirmwareJob::tr("Updating the MCU with %1 (role %2)")
                .arg(QFileInfo(mcuPackagePath).fileName(), FirmwareJob::roleText(mcuRole)));
    } else {
        log(FirmwareJob::tr("Setting the MCU role to %1").arg(FirmwareJob::roleText(mcuRole)));
    }
    updater->setThroughSoc(true);
    pushToolPaths();
    const QString text = FirmwareJob::tr("%1: opening the link through the board...").arg(FirmwareJob::operationTitle(stepName));
    setProgressBusy(text);
    setPersistent(text);
    linkActive = true;
    updateCancellable();
    if (!link->open()) {
        QTimer::singleShot(kSilentRefusalMs, q, [this, stepName]() {
            if (linkActive && !updaterActive && !link->isOpening() && !link->isOpen() && step == stepName) {
                linkActive = false;
                stepFinished(stepName, false, FirmwareJob::tr("the MCU link could not be opened"));
            }
        });
    }
}

void FirmwareJob::Private::onLinkOpened()
{
    if (!linkActive || updaterActive || mcuResultPending) {
        return;
    }
    updater->setDevice(link->socket());
    updaterActive = true;
    const bool started = (step == kOpMcu) ? updater->flash(mcuRole) : updater->setRole(mcuRole);
    if (!started) {
        updaterActive = false;
        mcuDone(false, FirmwareJob::tr("the MCU updater did not start"));
    }
}

void FirmwareJob::Private::mcuDone(bool ok, const QString& message)
{
    mcuOk = ok;
    mcuMessage = message;
    mcuResultPending = true;   // before close(): closed() may come from inside it
    if (link->isOpen() || link->isOpening()) {
        linkCloseTimer.start(kLinkCloseTimeoutMs);
        link->close();
    } else {
        completeMcu();
    }
}

void FirmwareJob::Private::completeMcu()
{
    if (!mcuResultPending) {
        return;
    }
    linkCloseTimer.stop();
    mcuResultPending = false;
    linkActive = false;
    const QString stepName = step;
    if (mcuOk && stepName == kOpSetRole) {
        runningRole = FirmwareJob::roleText(mcuRole);
        emit q->mcuInfoChanged();
    }
    stepFinished(stepName, mcuOk, mcuMessage);
}

void FirmwareJob::Private::startMcuRead(std::function<void(bool, const QString&)> done)
{
    cliDone = std::move(done);
    pendingVersion.clear();
    lastCliExit = -1;
    pushToolPaths();
    const QString text = FirmwareJob::tr("Reading the MCU version through ADB...");
    setProgressBusy(text);
    setPersistent(text);
    // First the stale rfmcu, exactly as McuLink::open() does before its bridge (same board: the
    // serial cliCommand() uses); `rfmcu cmd version` follows when it is done.
    link->adb()->setSerial(link->serial());
    ToolInvocation* invocation = link->adb()->shell(kStopRfmcuCommand);
    cliInvocation = invocation;   // cancellable like the command itself
    log(invocation->commandLine());
    connect(invocation, &ToolInvocation::finished, q, [this, invocation](const ToolResult& result) {
        if (cliInvocation != invocation) {
            return;
        }
        cliInvocation = nullptr;
        onStaleRfmcuStopped(result);
    });
    updateCancellable();
}

void FirmwareJob::Private::onStaleRfmcuStopped(const ToolResult& result)
{
    // Its exit status is ignored (nothing to kill is the usual case); a dead adb is not.
    if (result.cancelled || cancelRequested) {
        finishCli(false, FirmwareJob::tr("cancelled"));
        return;
    }
    if (!result.started) {
        finishCli(false, FirmwareJob::tr("adb could not be started: %1").arg(result.error));
        return;
    }
    if (result.timedOut) {
        finishCli(false, FirmwareJob::tr("adb did not answer in time"));
        return;
    }
    runCli(QStringLiteral("version"));
}

void FirmwareJob::Private::runCli(const QString& command)
{
    ToolInvocation* invocation = link->cliCommand(command);
    cliInvocation = invocation;
    log(invocation->commandLine().isEmpty() ? FirmwareJob::tr("rfmcu cmd %1").arg(command) : invocation->commandLine());
    connect(invocation, &ToolInvocation::finished, q, [this, invocation, command](const ToolResult& result) {
        if (cliInvocation != invocation) {
            return;
        }
        cliInvocation = nullptr;
        onCliFinished(command, result);
    });
    updateCancellable();
}

void FirmwareJob::Private::onCliFinished(const QString& command, const ToolResult& result)
{
    const QString output = result.outputText().trimmed();
    if (!output.isEmpty()) {
        log(output);
    }
    if (result.cancelled || cancelRequested) {
        finishCli(false, FirmwareJob::tr("cancelled"));
        return;
    }
    if (!result.started) {
        finishCli(false, FirmwareJob::tr("adb could not be started: %1").arg(result.error));
        return;
    }
    if (result.timedOut) {
        finishCli(false, FirmwareJob::tr("adb did not answer in time"));
        return;
    }
    // rfmcu cmd: 0 OK, 1 ERR, 2 port busy, 3 no reply (project/app/rfmcu/README.md). The chain
    // tells "the MCU is there but not talking" (2, 3) from adb-level failures by lastCliExit.
    lastCliExit = result.exitCode;
    switch (result.exitCode) {
    case 0:
        break;
    case kRfmcuPortBusy:
        finishCli(false, FirmwareJob::tr("the MCU port is busy (another rfmcu holds /dev/ttyS1): %1").arg(output));
        return;
    case kRfmcuNoReply:
        finishCli(false, FirmwareJob::tr("the MCU did not answer \"%1\"").arg(command));
        return;
    default:
        finishCli(false, FirmwareJob::tr("rfmcu cmd %1 failed (exit code %2): %3").arg(command).arg(result.exitCode).arg(output));
        return;
    }
    QString okLine;
    const QStringList lines = output.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString& line : lines) {
        if (line.trimmed().startsWith(QLatin1String("OK"))) {
            okLine = line.trimmed();
            break;
        }
    }
    if (command == QLatin1String("version")) {
        const McuUpdater::CliFields fields = McuUpdater::parseCliFields(okLine);
        const QString version = fields.value(QStringLiteral("version"));
        if (version.isEmpty()) {
            finishCli(false, FirmwareJob::tr("unexpected reply to \"version\": %1")
                                 .arg(output.isEmpty() ? FirmwareJob::tr("nothing") : output));
            return;
        }
        const QString reportedRole = fields.value(QStringLiteral("role"));
        if (reportedRole.isEmpty()) {
            pendingVersion = version;   // an older application: its "help" lists the role
            runCli(QStringLiteral("help"));
            return;
        }
        runningVersion = version;
        runningRole = reportedRole;
    } else {
        // fw_update.py probe_application(): the command words are the second word of every
        // "OK <name> <usage>" row of "help" (the real firmware prints one row per command, the
        // first one "OK commands:"); "tx" among them -> tx, "rx" -> rx, else unknown.
        // (Seen on the board 2026-10-13: 0.8.2 answers "OK version=0.8.2 baud=115200" without a
        // role, and its help lists "OK rx rx status | ...".)
        QStringList commands;
        for (const QString& line : lines) {
            const QStringList tokens = line.trimmed().split(QLatin1Char(' '), Qt::SkipEmptyParts);
            if (tokens.size() > 1 && tokens.first() == QLatin1String("OK")) {
                commands << tokens.at(1);
            }
        }
        runningVersion = pendingVersion;
        runningRole = commands.contains(QLatin1String("tx"))   ? QStringLiteral("tx")
                      : commands.contains(QLatin1String("rx")) ? QStringLiteral("rx")
                                                               : QStringLiteral("unknown");
    }
    emit q->mcuInfoChanged();
    const quint32 reported = FirmwareJob::roleFromText(runningRole);
    if (reported != 0) {
        q->setRole(reported);   // pre-selected: what the user confirms (or changes) from now on
    }
    finishCli(true, FirmwareJob::tr("MCU application version=%1 role=%2").arg(runningVersion, runningRole));
}

void FirmwareJob::Private::finishCli(bool ok, const QString& message)
{
    std::function<void(bool, const QString&)> done = std::move(cliDone);
    cliDone = nullptr;
    if (done) {
        done(ok, message);
    }
}

void FirmwareJob::Private::startChainMcu()
{
    if (!mcuPackage.isValid()) {
        log(FirmwareJob::tr("No MCU package chosen: the MCU update is skipped"));
        keepSsh = false;
        closeSsh();
        endOperation(kOpPullFlash, true, FirmwareJob::tr("SoC flashed; no MCU package chosen"));
        return;
    }
    step = kOpMcu;
    startMcuRead([this](bool ok, const QString& message) {
        if (!ok && !cancelRequested && pendingVersion.isEmpty()
            && (lastCliExit == kRfmcuNoReply || lastCliExit == kRfmcuPortBusy)) {
            // rfmcu ran on the board but the application CLI did not answer: a CC1310 left in its
            // UART updater (an interrupted transfer, a bootloader-only J-Link flash: runbook
            // Part B / Part J) has no CLI. McuUpdater::flash() handles exactly that board (HELLO is
            // answered by INFO), so the chain updates it instead of stopping - with the role the
            // user confirmed, since such a board cannot report one. (Port busy after the stale
            // rfmcu was stopped: the link's open stops it again right before its bridge.)
            if (confirmedRole == 0) {
                stepFinished(kOpMcu, false,
                             FirmwareJob::tr("cannot read the running MCU version (%1) and no role is confirmed: "
                                             "choose rx or tx, then press Update MCU")
                                 .arg(message));
                return;
            }
            log(FirmwareJob::tr("MCU version unknown (%1): it may sit in its UART updater; updating it").arg(message));
            startMcuUpdate(kOpMcu);
            return;
        }
        if (!ok) {
            stepFinished(kOpMcu, false, FirmwareJob::tr("cannot read the running MCU version: %1").arg(message));
            return;
        }
        if (runningVersion == mcuPackage.versionText()) {
            stepFinished(kOpMcu, true, FirmwareJob::tr("the MCU already runs %1; update skipped").arg(runningVersion));
            return;
        }
        startMcuUpdate(kOpMcu);
    });
}

// =======================================================================================
// Public slots
// =======================================================================================

void FirmwareJob::pull()
{
    if (d->refuseIfBusy()) {
        return;
    }
    d->beginOperation(kOpPull);
    d->keepSsh = false;
    d->startPull();
}

void FirmwareJob::flashSoc()
{
    if (d->refuseIfBusy()) {
        return;
    }
    d->beginOperation(kOpFlash);
    d->startFlash();
}

void FirmwareJob::updateMcu()
{
    if (d->refuseIfBusy()) {
        return;
    }
    d->beginOperation(kOpMcu);
    d->startMcuUpdate(kOpMcu);
}

void FirmwareJob::setMcuRole()
{
    if (d->refuseIfBusy()) {
        return;
    }
    d->beginOperation(kOpSetRole);
    d->startMcuUpdate(kOpSetRole);
}

void FirmwareJob::readMcuInfo()
{
    if (d->refuseIfBusy()) {
        return;
    }
    d->beginOperation(kOpInfo);
    d->step = kOpInfo;
    d->updateCancellable();
    d->startMcuRead([this](bool ok, const QString& message) { d->stepFinished(kOpInfo, ok, message); });
}

void FirmwareJob::pullAndFlash()
{
    if (d->refuseIfBusy()) {
        return;
    }
    d->beginOperation(kOpPullFlash);
    d->keepSsh = true;
    d->log(tr("Pull & Flash: pull the image, flash the SoC, wait for ADB, then update the MCU if needed"));
    d->startPull();
}

void FirmwareJob::cancel()
{
    if (!isBusy()) {
        return;
    }
    if (d->step == kOpFlash && !d->flashCancellable) {
        const QString message = tr("Cannot cancel while upgrade_tool writes the flash");
        d->log(message);
        emit cancelRefused(message);
        return;
    }
    d->cancelRequested = true;
    d->log(tr("Cancelling %1").arg(operationTitle(d->step.isEmpty() ? d->operation : d->step)));
    if (d->step == kOpPull) {
        if (d->pullPhase == Private::PullPhase::Connecting) {
            d->ssh->close();   // reported as Disconnected -> the step ends "cancelled"
        } else if (d->pullerActive) {
            d->puller->cancel();   // reports finished(false, "cancelled") once the transfer is undone
            QTimer::singleShot(kPullCancelGraceMs, this, [this]() {
                if (d->pullerActive) {
                    d->pullFinished(false, tr("cancelled"));
                }
            });
        }
        return;
    }
    if (d->step == kOpFlash) {
        d->flasher->cancel();
        QTimer::singleShot(kSilentRefusalMs, this, [this]() {
            if (d->flasherActive && !d->flasher->isRunning()) {
                d->flashFinished(false, tr("cancelled"));
            }
        });
        return;
    }
    if (d->cliInvocation) {
        d->cliInvocation->cancel();   // its finished() reports "cancelled"
        return;
    }
    if (d->step == kOpMcu || d->step == kOpSetRole) {
        if (d->updaterActive) {
            d->updater->cancel();   // finished(false, "cancelled") closes the link
        } else if (d->linkActive && !d->mcuResultPending) {
            d->mcuDone(false, tr("cancelled"));
        }
    }
}

bool FirmwareJob::rollBack(QString* message)
{
    if (d->refuseIfBusy()) {
        return false;
    }
    QString error;
    if (!ImagePuller::rollBack(d->cacheDir, &error)) {
        const QString text = tr("Roll back failed: %1").arg(error.isEmpty() ? tr("there is no previous image") : error);
        d->log(text);
        if (message) {
            *message = text;
        }
        refreshImage();
        return false;
    }
    d->localImage.clear();
    const QString text = tr("Rolled back: the previous image is the current one again");
    d->log(text);
    if (message) {
        *message = text;
    }
    refreshImage(/*reread=*/true);
    return true;
}

void FirmwareJob::refreshStatus()
{
    if (isBusy()) {
        return;   // upgrade_tool / adb belong to the running operation
    }
    d->flasher->queryStatus();
}

void FirmwareJob::planPartitions()
{
    d->requestPlan(true);
}

void FirmwareJob::refreshImage(bool reread)
{
    emit imageChanged(reread);
    d->requestPlan(reread);   // a no-op while image, mtime and layout are what the plan shows
}

void FirmwareJob::disconnectSource()
{
    if (d->step == kOpPull && isBusy()) {
        cancel();
    }
    d->keepSsh = false;
    d->closeSsh();
}
