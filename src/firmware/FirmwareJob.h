#pragma once

#include <QObject>
#include <QLatin1StringView>
#include <QList>
#include <QString>
#include <QStringList>
#include <functional>
#include <memory>
#include <optional>

#include "firmware/CcPackage.h"
#include "firmware/FleetFlasher.h"
#include "firmware/ImagePuller.h"
#include "firmware/RockchipTool.h"
#include "firmware/SocFlasher.h"
#include "ssh/SshConnection.h"

class FleetReport;
class McuLink;
class McuUpdater;
class SshProfileStore;
class ToolRunner;

/**
 * The firmware chain of the Firmware tab without any widget (v0.5.1; docs/firmware-update/CLI_PLAN.md
 * step 2): one object that runs every operation the tab offers - with the same steps, the same log
 * lines, the same rules and the same messages - so that the tab (FirmwareTab, su_app) and the
 * console tool `su-fw` (su_core only) drive the very same code.
 *
 * Operations (slots; one at a time - a second one while busy is refused with busyRefused("Busy:
 * <title> is still running") and does nothing else; every one is framed by busyChanged(true) /
 * busyChanged(false) and ends with operationFinished(op, ok, message), the op names are the kOp*
 * constants):
 *  - pull() ("pull"): resolve sourceTarget() (a restore key "ssh:profile:<id>" / "ssh:target:<t>",
 *    a profile name of profileStore(), or user@host[:port]), check the remote folder and create the
 *    cache folder, pullSourceAccepted(), connect the job's own SshConnection (setAutoReconnect off;
 *    the host key and the authentication prompts go to the callbacks below), run the ImagePuller
 *    (remote size / mtime / git describe, skip when unchanged, download, RKFW check, one-level
 *    history), close the connection again. A pull that succeeded makes the cache's update.img the
 *    image again (localImage() cleared).
 *  - flashSoc() ("flash"): the SocFlasher flow with imagePath() and the board's flash layout. Loader
 *    mode writes the selected partitions (the selection applies when a plan of this image is ready
 *    and has rows that can be written; then nothing selected is refused with "no partition is
 *    checked: ..."; without such a plan the flasher decides: every row), MASKROM mode the whole
 *    image with UF. Cancel only while waiting (flashCancellable), never while upgrade_tool writes
 *    the flash (EXF / WL / RD / UF): cancel() then logs and reports cancelRefused().
 *  - updateMcu() ("mcu") / setMcuRole() ("set-role"): McuLink::open() (bridge through the board),
 *    McuUpdater::flash(role()) with package() / McuUpdater::setRole(role()), throughSoc on (the CLI
 *    "power" reply is logged before "bootloader"), progress per DATA ACK, the restart banner, then
 *    McuLink::close() (killall rfmcu, forward --remove). The role is never guessed: without role()
 *    the step fails "choose the MCU role (rx or tx) first: it is never guessed".
 *  - readMcuInfo() ("info"): `adb shell "killall rfmcu 2>/dev/null; true"` first (an open
 *    `rfmcu console` holds the UART lock, as McuLink::open() knows), then `rfmcu cmd version`
 *    (McuLink::cliCommand(), no bridge; an application without "role=" is asked "help"):
 *    runningMcuVersion() / runningMcuRole(), mcuInfoChanged(), and the reported role becomes role()
 *    (roleChanged()). rfmcu cmd exit 2 = port busy, 3 = no reply, other = its own message.
 *  - pullAndFlash() ("pull-flash"): pull (skipped when unchanged) -> Flash SoC (every partition) ->
 *    wait for ADB -> read the MCU version -> Update MCU when a package is set and its version differs
 *    from the running one, or when the MCU does not answer the version read (rfmcu exit 3, or 2: a
 *    CC1310 left in its UART updater has no CLI - updated with role(), and refused with "... no role
 *    is confirmed ..." without one). Stops at the first failure (adb errors included): every step
 *    reports stepFinished(step, ok, message) and logs "<title>: <message>" / "<title> failed:
 *    <message>"; a failure ends the chain with operationFinished("pull-flash", false, "stopped at
 *    <title>: <message>"). The build-machine connection stays open across the chain (keepSsh) and
 *    is closed when it ends. stepFinished() is emitted for the single step of the other operations
 *    too, right before their operationFinished().
 *  - flashFleet() ("fleet-flash", v0.7.0; FleetFlasher): several boards at once - the Loader-mode
 *    partition writes of flashSoc() on every board of fleetOptions() (tagged serials, or every
 *    board on ADB), every upgrade_tool addressed with `-s <LocationID>`, the image unpacked once by
 *    this job's plan and shared. Inputs, checked first in this order and refused as a step framed
 *    like every result: imagePath() with a Ready plan of that very image ("the fleet needs the
 *    image's partition list: <plan message>"), a known layout (layoutKnown(): else "flash layout
 *    unknown: pass --layout, or run su-fw status once with a board running; the Node image carries
 *    its own" - before the selection, because selectedPartitions() is empty while the layout is
 *    unknown), a selection that names rows - the raw selection: not allPartitionsSelected() and
 *    setSelectedPartitions() given names ("a fleet writes named partitions only: --partitions
 *    NAMES; every partition would include idblock, uboot, env and meta") - with every named row
 *    writable (partitionProblem() empty: else "<name> cannot be written: <problem>; nothing was
 *    written") and selectedPartitions() then non-empty, boards or allBoards, maxParallel 0..64,
 *    and a platform that can map ports (FleetFlasher::platformRefusal()). Then the
 *    FleetFlasher run with the tool paths, the selected rows, flashLayout() / usingImageLayout(),
 *    the ports remembered per serial (setPortProvider(); the tab / su-fw answer from
 *    AppSettings::firmwareBoardPort()), loaderWaitSeconds() and adbReturnSeconds() and the
 *    flasher's waitForAdb(). Its per-board lines come as boardEvent() / boardProgress() /
 *    boardFinished() (mirrored to lcFirmware with the board's label; not through logLine(), which
 *    carries only the run's own lines), its learned ports as boardPortLearned(serial, port) and its
 *    layouts as mtdPartsRead(serial, mtdparts), for the owner to store. progress(): "Flashing fleet:
 *    <k> of <n> boards done, <w> writing" (kProgressBusy). The result is the fleet's summary ("7 of
 *    7 boards flashed in 48.3 s (mapping 9.8 s, writing 31.2 s)"; FleetReport::summary()), the
 *    operation title "Flash fleet" - beginOperation() is untouched, so a dry run's banner is the
 *    same "--- Flash fleet ---" and the flasher's "Dry run: ..." line says what it is. A
 *    flashFleet() refused before the flasher started (the lock, the inputs above) leaves
 *    fleetReport() empty (no boards, stop ""), which FleetReport::exitCode() maps to 1; the
 *    flasher's own refusals carry their text as the stop. cancel() is accepted at any moment (isCancellable() stays true
 *    for the whole fleet; never cancelRefused()): the FleetFlasher drains - boards not started are
 *    skipped and reset, a board that is being written finishes all its rows and its reset, the ADB
 *    waits end - and the operation ends with the drained summary; a second cancel() only logs how
 *    many writes are still being waited for. The lock, busyChanged(), stepFinished() and
 *    operationFinished() frame it like the other operations; one fleet per PC, by the same lock as
 *    one flash. flashSoc(), SocFlasher and the tab are untouched; the tab never calls flashFleet().
 * Every operation also checks its inputs first and fails as a step ("no image: pull one or choose a
 * local image first", "choose a valid .pkg package first", ...), framed like any other result.
 *
 * One firmware operation per machine (v0.6.0): the tab and su-fw may run side by side on one PC and
 * one board, so every operation holds a QLockFile, AppSettings::dataDirectory() + "/firmware.lock",
 * from its busyChanged(true) to its result (taken without waiting; released on every end: done,
 * failed, cancelled, the job destroyed). An operation that finds it held by another job - in this
 * process or another one - ends at once, framed like every result, with the step failure "another
 * firmware operation is running (<program>, pid <n>)" (a single-step operation reports it with
 * stepFinished() as well). While another job holds it, refreshStatus() runs no query (its tools would
 * talk to the board in the middle of the other operation): it logs "status paused: another firmware
 * operation is running (...)" once and statusPausedReason() says why; the next refreshStatus() after
 * the lock was released queries again. A lock left by a process that is gone is stale and taken over.
 * A data folder that cannot hold the file leaves the operation unguarded (logged), as before 0.6.0.
 *
 * Outputs: logLine() carries every plain log line (the owner adds the time; multi-line text is one
 * signal) - the command lines run, the tools' results, the flasher / updater / puller narration in
 * their own wording - and the job mirrors each of them to the System Log through lcFirmware; never
 * a password, passphrase or key. toolOutput() carries the raw lines upgrade_tool prints while it
 * flashes ("Download Image... (42%)" repeated: the owner collapses them; not mirrored by the job).
 * progress(stage, percent): percent 0..100, kProgressBusy (a stage without a percentage) or
 * kProgressTextOnly (only the text changes). persistentStatusMessage() is the lasting status while
 * busy ("Flashing SoC: Download Image... 42 %"), persistentStatusChanged() when it changes (it is
 * cleared silently when the operation ends: the result replaces it). isCancellable() /
 * cancellableChanged(): what a Cancel button may do right now.
 *
 * Decisions the tab answers with dialogs are callbacks set by the owner (each is copied before it is
 * called, so it may run a nested event loop; the job checks afterwards that it still exists):
 *  - setHostKeyHandler(): an unknown / changed host key -> HostKeyDecision{accept, remember}
 *    (remember writes known_hosts, so the second pull does not ask). Headless default (no handler):
 *    refused - SshConnection::answerHostKey(false, false) - so the pull fails with the very message
 *    the tab shows when the user refuses ("Host key rejected for <target>").
 *  - setAuthPromptHandler(): a password / passphrase / keyboard-interactive prompt -> the answer and
 *    whether to remember it in SecretStore, or std::nullopt = cancelled (the connect ends with the
 *    SSH stack's "Connection to <target> cancelled"). SshConnection tries a saved SecretStore secret
 *    itself first, as it does for every tab. Headless default: the prompt is cancelled and the pull
 *    fails with "authentication failed: nobody answered \"<prompt text>\"".
 *  - setDownloadModeHandler(): SocFlasher::downloadModeNeeded() (no board on ADB, Loader or Maskrom)
 *    -> DownloadModeAnswer: `send` set = "reboot loader\r" may go out through a connected session;
 *    the job calls it only when the flasher still waits for Loader mode after the question (the
 *    board may have appeared meanwhile: "The board appeared meanwhile; nothing sent") and logs "Sent
 *    \"reboot loader\" through <name>". No `send` (no session, the user said no) -> the MASKROM
 *    instructions (maskromInstructions(): logged, progress text, lasting status). Headless default:
 *    the instructions, then the flasher's own failure "board did not enter MASKROM/Loader within
 *    <n> s" unless the board appears in time.
 * The remembered flash layout of a board comes from setLayoutProvider() (the tab answers from
 * AppSettings::firmwareMtdParts()) or from setMtdParts(); a layout the job reads itself (the first
 * time the status shows an ADB board without one: `adb -s <serial> shell cat /proc/cmdline`, once
 * per board and job; or the flasher's step 2) is kept and reported with mtdPartsRead(serial,
 * mtdparts) so the owner can store it. The status's own read ends with layoutReadFinished(serial,
 * ok) whatever it found (after mtdPartsRead() when it found a layout; ok = false when adb could not
 * run or the board reports none), so an owner that waits for it needs no log text. Settings: the
 * job reads no AppSettings (only its data folder, for the lock); the owner hands in the values
 * (setSshDefaults(), setGitDescribe(), setExtraFiles(), the timeouts, the paths).
 *
 * Partitions (v0.5.1): planPartitions() / every image change unpacks the image (SocFlasher
 * planPartitions(), EXF) and lists partitions() in the layout in use (flashLayout(): the image's own
 * env.img layout when it carries one, else boardLayout()); partitionsChanged() whenever what a
 * partition table shows changes, partitionsPlanned() when a plan ends. The selection is
 * selectAllPartitions() (the default, and what a new image starts with) or setSelectedPartitions()
 * (exactly those rows); selectedPartitions() are the selected rows that can be written
 * (partitionProblem() empty).
 *
 * The tab owns a FirmwareJob, keeps its widgets, dialogs, settings and status timer, renders from
 * the getters and signals, answers the callbacks with HostKeyDialog / AuthPromptDialog / a
 * QMessageBox, and returns the job's collaborators from its own accessors (socFlasher(), ...).
 * su-fw sets the inputs from its command line, sets the callbacks it can answer (or none: the
 * headless defaults), calls one slot and waits for operationFinished().
 */
class FirmwareJob : public QObject
{
    Q_OBJECT
public:
    // ---- Operation and step names (currentOperation(), stepFinished(), operationFinished()) ----
    static constexpr QLatin1StringView kOpPull{"pull"};
    static constexpr QLatin1StringView kOpFlash{"flash"};
    static constexpr QLatin1StringView kOpMcu{"mcu"};
    static constexpr QLatin1StringView kOpSetRole{"set-role"};
    static constexpr QLatin1StringView kOpInfo{"info"};
    static constexpr QLatin1StringView kOpPullFlash{"pull-flash"};
    static constexpr QLatin1StringView kOpFleetFlash{"fleet-flash"};   ///< (v0.7.0) flashFleet(); title "Flash fleet"

    // ---- progress() values that are no percentage --------------------------------------------
    static constexpr int kProgressBusy = -1;       ///< a stage without a percentage (an indeterminate bar)
    static constexpr int kProgressTextOnly = -2;   ///< only the text changes; a bar stays as it is

    enum class PlanState { NoImage, Planning, Ready, Failed };
    Q_ENUM(PlanState)

    // ---- Decisions (see the class comment for the headless defaults) --------------------------
    struct HostKeyDecision
    {
        bool accept = false;
        bool remember = false;   ///< append (or replace) the key in known_hosts
    };
    using HostKeyHandler = std::function<HostKeyDecision(const SshConnection::HostKeyInfo& info)>;

    struct AuthAnswer
    {
        QString response;        ///< the password / passphrase / answer; never logged
        bool remember = false;   ///< store it in SecretStore once it worked
    };
    /// std::nullopt = the prompt was cancelled.
    using AuthPromptHandler = std::function<std::optional<AuthAnswer>(const SshConnection::AuthPrompt& prompt)>;

    struct DownloadModeAnswer
    {
        /// A question was put to the user (it may have taken a while): before anything is sent the
        /// job checks that the flasher still waits for Loader mode.
        bool asked = false;
        /// Sends "reboot loader\r" through a session and returns its name for the log (empty = it
        /// could not send). Null = nothing to send: the MASKROM instructions instead.
        std::function<QString()> send;
    };
    using DownloadModeHandler = std::function<DownloadModeAnswer()>;

    /// The remembered flash layout (mtdparts) of the board `serial`; "" = the layout for a board
    /// whose serial is unknown (one met in Loader mode: rockusb reports no ADB serial).
    using LayoutProvider = std::function<QString(const QString& serial)>;

    /// (v0.7.0) What a fleet flash writes and on which boards (flashFleet()).
    struct FleetOptions
    {
        QList<QPair<QString, QString>> boards;   ///< (tag, serial) in selection order, tags and serials unique (the CLI assigns the default tags: FirmwareCli.h); ignored with allBoards
        bool allBoards = false;                  ///< every board `adb devices` lists as "device", tagged A, B, ... in that order
        int maxParallel = 0;                     ///< boards written at once; 0 = all of them (1..64 otherwise)
        bool dryRun = false;                     ///< the selection, the port memory and the plan only: no reboot, no write
    };
    /// (v0.7.0) The USB port remembered per serial ("" = none): the tab / su-fw answer from
    /// AppSettings::firmwareBoardPort(), as setLayoutProvider() does for the layout.
    using PortProvider = std::function<QString(const QString& serial)>;

    /// What a pull fills in where the profile leaves it open (the tab: Preferences > SSH): the
    /// known_hosts file for every profile without one; identity file, terminal type and keep-alive
    /// for an ad-hoc user@host target.
    struct SshDefaults
    {
        QString knownHostsFile;      ///< "" = SshConnection::defaultKnownHostsFile()
        QString identityFile;        ///< "" = agent / ~/.ssh/id_* discovery
        QString terminalType;        ///< "" = the profile's default
        int keepAliveSeconds = 30;
    };

    /// `runner` null = a ProcessToolRunner owned by the job (created after the collaborators, so
    /// McuLink's destructor can still send the board's cleanup through it).
    explicit FirmwareJob(ToolRunner* runner, QObject* parent = nullptr);
    ~FirmwareJob() override;

    // ---- Collaborators (owned; the same objects for the job's whole life) ---------------------
    ToolRunner* toolRunner() const;
    SocFlasher* socFlasher() const;
    /// (v0.7.0) The fleet's flasher, created with the job on the same runner; the tests reach its
    /// enumerator, polls and LD policy through it (FirmwareCli::setJobSetup()).
    FleetFlasher* fleetFlasher() const;
    McuLink* mcuLink() const;
    McuUpdater* mcuUpdater() const;
    ImagePuller* imagePuller() const;
    SshConnection* sshConnection() const;

    // ---- Inputs ----------------------------------------------------------------------------------
    void setProfileStore(SshProfileStore* store);   ///< not owned, may be null (only user@host targets then)
    SshProfileStore* profileStore() const;
    void setToolPaths(const QString& upgradeTool, const QString& adb);   ///< empty = not found
    QString upgradeToolPath() const;
    QString adbPath() const;
    void setSerial(const QString& serial);          ///< the ADB board for the flasher and the MCU link; "" = the only one
    QString serial() const;
    void setLoaderWaitSeconds(int seconds);         ///< SocFlasher::setWaitTimeoutSeconds() (default 30)
    int loaderWaitSeconds() const;
    void setAdbReturnSeconds(int seconds);          ///< SocFlasher::setAdbReturnTimeoutSeconds() (default 60)
    int adbReturnSeconds() const;

    void setSourceTarget(const QString& target);    ///< a restore key, a profile name or user@host[:port]
    QString sourceTarget() const;
    void setRemoteFolder(const QString& folder);    ///< "~/rk/rockchip-platforms/output/image" ("~" = the remote home)
    QString remoteFolder() const;
    void setCacheDirectory(const QString& dir);     ///< the local cache (update.img, update.prev.img, notes)
    QString cacheDirectory() const;
    void setGitDescribe(bool on);                   ///< ImagePuller::setGitDescribe() (default true)
    bool gitDescribe() const;
    void setExtraFiles(const QStringList& names);   ///< ImagePuller::setExtraFiles()
    QStringList extraFiles() const;
    void setSshDefaults(const SshDefaults& defaults);
    SshDefaults sshDefaults() const;

    // ---- Image -------------------------------------------------------------------------------------
    QString imagePath() const;                      ///< what Flash SoC uses: localImage(), else the cache's update.img when it exists
    QString localImage() const;                     ///< the local pick; "" = the cache's image
    /// The checked local pick (as "Use local image..."): a missing file or one without the RKFW magic
    /// is refused - logLine("Image <path> refused: <reason>"), false, *message = that line; else
    /// logLine("Using the local image <path>"), imageChanged(), a new plan. "" = back to the cache.
    bool setImagePath(const QString& path, QString* message = nullptr);
    /// The pick without checks or a log line (an owner restoring one it validated before); "" = the cache.
    void useLocalImage(const QString& path);
    ImagePuller::CacheEntry cacheEntry() const;     ///< ImagePuller::cachedImage(cacheDirectory())

    // ---- MCU -------------------------------------------------------------------------------------
    /// Loads and checks a .pkg (CcPackage::load()): refused -> logLine("Package <file> refused:
    /// <reason>"), false, *message = that line, the previous package stays; accepted ->
    /// logLine("Package <file>: <summary>"), packageChanged(). "" clears the package (false, no log).
    bool setPackagePath(const QString& path, QString* message = nullptr);
    /// A package the caller loaded and checked itself (no log line); packagePath() = its filePath.
    void setPackage(const CcPackage& package);
    QString packagePath() const;
    CcPackage package() const;
    void setRole(quint32 role);                     ///< CcProtocol::RoleRx / RoleTx; anything else = none (0)
    quint32 role() const;                           ///< the confirmed role, 0 = none
    QString runningMcuVersion() const;              ///< the last read / banner ("0.8.1"), "" when unknown
    QString runningMcuRole() const;                 ///< "rx" / "tx" / "unknown", "" when unknown

    // ---- Board -----------------------------------------------------------------------------------
    bool hasStatus() const;                         ///< a status query reported at least once
    SocFlasher::DeviceStatus lastStatus() const;
    QString boardInfo() const;                      ///< /proc/version + uptime after the last flash ("" before)
    QString maskromInstructions() const;            ///< "No board found: hold the MASKROM button ..."

    // ---- Partitions (v0.5.1) ---------------------------------------------------------------------
    PlanState planState() const;
    QString planMessage() const;                    ///< SocFlasher::partitionsPlanned()'s message (the reason when it failed)
    QList<PartitionImage> partitions() const;       ///< the rows of plannedImage() (empty unless Ready)
    QString plannedImage() const;
    QString flashLayout() const;                    ///< the layout in use: the image's own when it carries one, else boardLayout()
    QString boardLayout() const;                    ///< remembered for the board last seen on ADB, else for "" (Loader mode)
    QString imageLayout() const;                    ///< the mtdparts of the planned image's env.img ("" = none)
    bool usingImageLayout() const;
    bool layoutKnown() const;                       ///< flashLayout() parses
    void setLayoutProvider(LayoutProvider provider);
    void setMtdParts(const QString& mtdparts, const QString& serial = QString());   ///< the job's own memory
    QString mtdParts(const QString& serial = QString()) const;
    void selectAllPartitions();                     ///< every row that can be written (the default)
    void setSelectedPartitions(const QStringList& names);   ///< exactly these rows; unknown names are ignored
    bool allPartitionsSelected() const;
    QStringList selectedPartitions() const;         ///< the selected rows that can be written, in table order
    /// Why `row` cannot be written ("partition layout unknown: connect the board running once", "<name>
    /// is not in the board's partition layout", ...); empty = it can.
    static QString partitionProblem(const PartitionImage& row, bool layoutIsKnown);

    // ---- Fleet (v0.7.0) --------------------------------------------------------------------------
    void setFleetOptions(const FleetOptions& options);
    FleetOptions fleetOptions() const;
    void setPortProvider(PortProvider provider);
    QList<FleetBoard> fleetBoards() const;       ///< the boards of the last flashFleet() (FleetFlasher::boards(); empty before one ran)
    FleetReport fleetReport() const;             ///< FleetFlasher::report() of the last flashFleet(): the table, the JSON fields, the exit code

    // ---- State -------------------------------------------------------------------------------------
    bool isBusy() const;
    QString currentOperation() const;               ///< a kOp* name while busy, else ""
    QString currentStep() const;                    ///< the step running right now ("" between the steps of a chain)
    bool isCancellable() const;
    QString persistentStatusMessage() const;        ///< "" while idle
    /// "" unless the last refreshStatus() ran no query because another job holds the firmware lock:
    /// then "another firmware operation is running (<program>, pid <n>)".
    QString statusPausedReason() const;

    void setHostKeyHandler(HostKeyHandler handler);
    void setAuthPromptHandler(AuthPromptHandler handler);
    void setDownloadModeHandler(DownloadModeHandler handler);

    static QString operationTitle(const QString& op);   ///< "Pull", "Flash SoC", "Update MCU", "Set role", "Read MCU", "Pull & Flash", "Flash fleet"
    static QString resultText(const QString& op, bool ok, const QString& message);   ///< "<title> done: <message>" / "<title> failed: <message>"
    static QString roleText(quint32 role);          ///< "rx" / "tx" / ""
    static quint32 roleFromText(const QString& text);

public slots:
    void pull();
    void flashSoc();
    void updateMcu();
    void setMcuRole();
    void readMcuInfo();
    void pullAndFlash();
    void flashFleet();                              ///< (v0.7.0) the fleet flash of the class comment; refused while busy like every operation
    void cancel();
    /// Swaps update.img and update.prev.img in the cache (ImagePuller::rollBack()); refused while busy
    /// (busyRefused(), false, *message untouched). Logged; *message = the line; the cache image is
    /// the image again.
    bool rollBack(QString* message = nullptr);
    void refreshStatus();                           ///< SocFlasher::queryStatus() unless busy or another job holds the lock
    void planPartitions();                          ///< plan the current image again
    /// Re-reads the image: imageChanged(reread), then a new plan when image, mtime or layout changed
    /// (reread: the file changed in place, describe and plan it again).
    void refreshImage(bool reread = false);
    void disconnectSource();                        ///< cancels a running pull step, closes the build-machine connection

signals:
    void logLine(const QString& line);
    void toolOutput(const QString& line);
    void progress(const QString& stage, int percent);
    void stepFinished(const QString& step, bool ok, const QString& message);
    void operationFinished(const QString& op, bool ok, const QString& message);
    void busyChanged(bool busy);
    void cancellableChanged(bool cancellable);
    void persistentStatusChanged(const QString& message);
    void busyRefused(const QString& message);       ///< an operation slot was called while busy
    void cancelRefused(const QString& message);     ///< cancel() while upgrade_tool writes (also logged)
    /// pull() accepted its inputs and is about to connect: what the owner may remember.
    void pullSourceAccepted(const QString& target, const QString& remoteFolder, const QString& cacheDirectory);
    void statusChanged(const SocFlasher::DeviceStatus& status);
    void boardInfoReceived(const QString& text);
    void imageChanged(bool reread);
    void packageChanged();
    void roleChanged(quint32 role);
    void mcuInfoChanged();
    void partitionsChanged();
    void partitionsPlanned();
    void mtdPartsRead(const QString& serial, const QString& mtdparts);
    /// The status's /proc/cmdline read of `serial` ended: ok = a layout was read (mtdPartsRead() came first).
    void layoutReadFinished(const QString& serial, bool ok);
    // ---- Fleet (v0.7.0): FleetFlasher's per-board signals, forwarded while flashFleet() runs ----
    void boardEvent(const FleetBoard& board, const QString& text);                   ///< one line per board per event (the owner prefixes board.label())
    void boardProgress(const FleetBoard& board, const QString& stage, int percent);  ///< 0..100 over the board's bytes
    void boardFinished(const FleetBoard& board);                                     ///< its result is final
    void boardPortLearned(const QString& serial, const QString& port);               ///< store it (AppSettings::setFirmwareBoardPort)

private:
    struct Private;
    std::unique_ptr<Private> d;
};
