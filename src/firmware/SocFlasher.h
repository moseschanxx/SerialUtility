#pragma once

#include <QObject>
#include <QString>
#include <memory>

#include "firmware/AdbTool.h"
#include "firmware/BoardProfile.h"
#include "firmware/RockchipTool.h"
#include "firmware/UsbProbe.h"

/**
 * Flashing the Rockchip SoC over the USB cable, as rockchip-platforms/tools/usb_update.sh does
 * it, asynchronous and cancellable while waiting (never during UF).
 *
 * Device status (queryStatus(), used by the tab's 2 s refresh and by start()): `upgrade_tool LD`
 * first - a device in Loader / Maskrom mode wins - then `adb devices` (AdbTool::selectDevice()
 * with serial()); when neither sees a board, UsbProbe::presentDevices() decides between
 * DeviceStatus::None and DeviceStatus::NoDriver (a 0x2207 device is on the bus but no tool sees
 * it: `hint` = UsbProbe::driverHint()). statusChanged() reports every query; queries never
 * overlap (a query while one runs is dropped) and none runs while a flash is in progress.
 *
 * start() (usb_update.sh main flow):
 *  1. Detecting: queryStatus(). Loader / Maskrom -> 4. ADB device -> 2. Nothing -> 3.
 *  2. RebootingToLoader: logLine("board running <kernel> ..., rebooting it into loader mode")
 *     (`adb shell cat /proc/version` first; busybox on the Node board has no uname), then
 *     `adb shell cat /proc/cmdline` whose mtdparts word becomes mtdParts() (mtdPartsRead(); a
 *     board without one keeps the owner's value), then
 *     `adb reboot loader` (its exit status ignored, like the script) -> 4. Without a serial()
 *     the board found here is the flow's from now on: every later adb command of the flow
 *     carries `-s <its serial>` (a second ADB device appearing meanwhile changes nothing); the
 *     end of the flow restores serial().
 *  3. downloadModeNeeded(): the owner may send "reboot loader" through a session tab or tell the
 *     user to hold MASKROM and replug; the flasher waits like 4 for the board to appear (a Yes /
 *     an appearing board continues, cancel() ends it). The signal is informational: nothing
 *     blocks on it. Boards that are there but cannot be used (DeviceStatus::detail: several ADB
 *     boards, an unauthorized one) are logged first ("no usable board: <detail>").
 *  4. WaitingForLoader: `LD` every pollIntervalMs (1 s) until a Loader / Maskrom device is
 *     listed, at most waitTimeoutSeconds (30; usb_update.sh TIMEOUT) ->
 *     finished(false, "board did not enter MASKROM/Loader within 30 s"). cancel() here ->
 *     finished(false, "cancelled").
 *  5. Flashing, by one of two methods (flashMethod() after detection, logged):
 *     - Loader mode (U-Boot rockusb) -> FlashMethod::PartitionWrites, what SocToolKit's Download
 *       page does: `EXF <image> <extractDirectory>` unpacks the package (progress stage
 *       "Extracting"), `package-file` names the partition of every image, the layout comes from
 *       the image itself when its env.img carries one (RockchipTool::mtdPartsFromEnvImage(); the
 *       layout the images were built for, so a board already in Loader mode can be flashed
 *       without a prior ADB read), else from mtdParts() - read from the running board's
 *       /proc/cmdline in step 2 (mtdPartsRead()) or set by the owner; when both are known and
 *       differ, logLine() shows both and the image's wins (the env being written defines the
 *       new layout) (v0.8.0: refused unless forced - rule 2 below) - and every image whose partition is
 *       in the layout is written with `WL <offset / 512> <size / 512> <file>` (the partition size
 *       in sectors, as SocToolKit passes it; upgrade_tool needs it for jffs2 images) in package-file order (entries
 *       "package-file" and "bootloader" and names absent from the layout are skipped with a log
 *       line; an image larger than its partition stops the flow before anything is written:
 *       finished(false, "<file> (<n> bytes) does not fit partition <name> (<size> bytes)")); a
 *       `WL` that fails -> finished(false, "writing <name> failed: <tail>"); then `RD` resets
 *       the board. progress(stage "Writing <name> (k of N)", percent over all bytes to write).
 *       Without a layout: finished(false, "partition layout unknown: boot the board once so it
 *       can be read over ADB, or flash it in MASKROM mode"). This path runs no chip check,
 *       which is why it works with a U-Boot that reports no chip id (the Node board's).
 *     - Maskrom mode -> FlashMethod::UpgradeFirmware: `UF <image>` as before (the BootROM
 *       answers the tool's chip check); the success line is required:
 *       finished(false, "upgrade_tool did not report success: <tail>") otherwise (tail =
 *       RockchipTool::tailForMessage()).
 *     Neither method is cancellable: cancel() is refused with logLine("cannot cancel while
 *     upgrade_tool writes the flash"). No timeout either: after 5 min without output (and after
 *     every further 5 min) logLine() says that upgrade_tool is silent and left running - log
 *     only, the flasher never stops it.
 *  6. WaitingForAdb (unless waitForAdb() is off): `adb get-state` every pollIntervalMs until
 *     "device", at most adbReturnTimeoutSeconds (60) -> finished(true, "board flashed, but ADB did
 *     not come back within 60 s; check the serial console") - a success with a warning, as the
 *     script treats it.
 *  7. ReadingBoardInfo: `adb shell "cat /proc/version; uptime"` (and `uname -a` when it exists)
 *     -> boardInfo(text), finished(true, "Upgrade firmware ok; board is back on ADB").
 * The image must exist and carry the RKFW magic (RockchipTool::isRkfwImage()), the tool paths
 * must be set, else start() returns false with finished(false, reason) from the event loop.
 *
 * Tools: upgradeTool() (RockchipTool) and adb() (AdbTool) share the ToolRunner; the owner sets
 * their paths from ToolLocator. Every external command line and its result go to logLine(); the
 * UF output is forwarded line by line as it arrives (toolOutput()) for the log pane.
 *
 * Board profiles (v0.8.0; BoardProfile.h, docs/DESIGN.md 4.14). Step 1 learns the board's profile
 * from the PID `LD` printed for a LOADER device (DeviceStatus::pid: 0x110e the Node, 0x110c the
 * stacked board; BoardProfiles::byLoaderPid()). The PID is profile evidence for Mode::Loader ONLY:
 * a Maskrom status hands loaderPid 0 to detectBoard() - its PID is the BootROM's, unknown on both
 * boards (RV1106G2_FACTS.md row 33) - while DeviceStatus::pid stays filled for the fleet's port
 * check; so a MASKROM board's profile comes from the override or stays unknown, and UF (the only
 * recovery route of a bricked board) is never blocked by a BootROM PID that happens to equal the
 * other board's Loader PID. Step 2 learns it from the running board's flash layout (its
 * /proc/cmdline, BoardProfiles::byLayout()) or from the USB enumeration's product string when the
 * OS reports one (UsbProbe::presentDevices(): Linux sysfs; Windows reports the driver's name);
 * when the layout read yields a layout matching no profile, or none (open question 6: a G2 whose
 * cmdline carries no mtdparts), and the profile is still unknown, step 2 reads the gadget's product
 * string from the board itself, once per board and flow - `adb -s <serial> shell cat
 * /sys/kernel/config/usb_gadget/rockchip/strings/0x409/product` (the configfs path usb_adb.sh
 * writes, row 29; logged like every adb command; a failure or an empty answer leaves the profile
 * unknown) - and hands it to detectBoard() as adbProduct (Source::AdbProduct, evidence the string).
 * Conditional, so a board whose cmdline carries the Node layout runs no extra command and every
 * pinned 0.7.0 list is unchanged. THE LOADER DEVICE ADOPTED AT STEP 4 CARRIES ITS PID TOO: the
 * device the poll lists after step 2's reboot, or after step 3's wait, is detected again with its
 * PID added (detectBoard()'s order: the PID outranks the product string and the layout, so it
 * confirms a known profile, replaces "unknown", or - the board's U-Boot contradicting its own
 * /proc/cmdline - wins over the layout), the result logged and reported like step 1's whenever it
 * differs from the last one. A board that reaches Loader mode through step 2 without a readable
 * cmdline layout, or through step 3 and a session tab's "reboot loader", is therefore known by its
 * PID right before the first WL, where the rules below run for it. profileOverride() (`--profile`,
 * the tab's Board profile box) wins over the product string and the layout - but NOT over a known
 * Loader PID that names the other profile: ProfileRules::overrideRefusal() ends the flow at step 1
 * (a board met in Loader mode) or at step 4 (the adopted device) with finished(false, "the --profile
 * stacked override contradicts the board's Loader PID 0x110e (the Node (RV1106BG2)'s); drop
 * --profile") before EXF, PL / RL or any WL - an override is the attached board "when the tool
 * cannot tell it", never a way to overrule what it read. The image's profile is read from its RKFW
 * header (RockchipTool::readRkfwHeader(): the chip tag; no SFI runs, so the command lines of a
 * 0.7.0 flash stay byte for byte) and, failing that, from its env.img layout once the plan exists.
 * Both are logged with their evidence (ProfileDetection::text(): "profile: Stacked (RV1106G2) -
 * from the board's Loader PID 0x110c"; "image profile: Node (RV1106BG2) - from the image's chip
 * tag 110E") and the board's is reported by profileDetected(). The rules of ProfileRules are
 * applied at two points and nowhere else: BEFORE `adb reboot loader` (step 2, right after the
 * layout read, with the plan made first in every case - the image is never a reason to reboot a
 * board it will not be written to) and BEFORE the first WL (step 5) - for EVERY PartitionWrites
 * flow, with the then-current detection: a board met in Loader mode at step 1, one that arrived
 * during step 3's wait, AND one that came through step 2 (whose step-2 run may have been inert on
 * "unknown" and whose PID now tells; a flow whose detection did not change gets the same answer,
 * and a question of this flow is asked at most once per rule - its answer is kept): rule 1,
 * imageRefusal() (both profiles known and different -> finished(false, <its text>)); rule 2,
 * layoutRefusal() (the image's env.img layout against the board's: step 2's read, the owner's
 * setMtdParts(), or the one read in Loader mode below) - the 0.5.1 "the image's wins" is gone:
 * without forceLayout() the LayoutConflictHandler is asked once (the tab's question; no handler =
 * refuse) and a No ends the flow with the refusal text, forceLayout() or a Yes logs the note and
 * the image's layout is used; rule 3, loaderPartitionRefusal() on the rows about to be written
 * (step 2 computes them from the selection as writePartitions() will, step 5 from its queue) -
 * without forceLoaderPartitions() the LoaderPartitionsHandler is asked once (no handler = refuse).
 * MASKROM mode (UF, the whole image) applies rule 1 only: a board already in MASKROM has its
 * recovery route by evidence. An unknown profile makes rules 1 and 3 inert (logged as unknown):
 * a layout-less board is never guessed, and the Node's 0.7.0 flows stay as they were. The Loader
 * wait of step 4 stays waitTimeoutSeconds() (a profile never changes a wait); its timeout message
 * carries the profile's loaderWaitHint() once the profile is known ("board did not enter
 * MASKROM/Loader within 30 s; on the stacked board the CC1310 may have cut the SoC's power:
 * power-cycle it, then retry").
 *
 * Several devices in download mode (v0.8.0). The single-board flow's WL / RD / UF are unaddressed
 * (RockchipTool::writeLba(): with two devices present upgrade_tool silently takes one of them,
 * measured 2026-10-14), while its PL / RL reads are addressed to the device step 1 or 4 picked - so
 * with two devices the layout of one could drive a write that lands on the other, and a verify of
 * one would judge the other's write. Therefore, when the `LD` of step 1 or of a step-4 poll lists
 * MORE THAN ONE device in download mode (Loader or Maskrom, any PID - a board left in Loader mode by
 * an earlier fleet run next to the one being flashed, routine on a mixed bench), FlashMethod is
 * never decided: finished(false, "several rockusb devices are in download mode (LocationIDs 104,
 * 112): the single-board flow writes unaddressed and cannot tell them apart; reset the ones that
 * are not this board with `upgrade_tool -s <id> RD`, or flash them as a fleet with --serials")
 * before EXF, PL / RL or any WL - a check on the LD output alone, no new command; every pinned
 * case lists one device. queryStatus() is unchanged (the first device wins, as in 0.7.0): the
 * refusal is the flow's.
 *
 * The layout of a board met in Loader mode (v0.8.0, layout-pl; docs/DESIGN.md 4.15). When step 1,
 * or the wait after step 3 (no board at start, then one put into Loader / MASKROM mode by hand or
 * by a session tab's "reboot loader" - NOT a board that came through step 2, whose /proc/cmdline
 * was read), finds the board in Loader mode and no layout for it is known - mtdParts() empty:
 * nothing remembered, no --layout; the image's env.img does not count, the board's own is wanted
 * (rule 2's other side, and the only source for an image without one) - the flow reads it from the
 * board right after step 4 (after the several-devices check above, and after the override check),
 * before the plan. The ENV PARTITION FIRST, the vendor's own way for exactly these boards
 * (rkdownload.sh's `RL 0 1 /tmp/env.img`, row 26) in a format the tool already parses: "reading
 * the flash layout of the board in Loader mode (env partition, upgrade_tool RL 0 128)", then
 * `upgrade_tool -s <LocationID> RL 0 128 <extractDirectory()>/.layout/env.bin` (readLbaOn(); the
 * env partition starts at sector 0 and is 64 KiB on both boards, row 18) and mtdPartsFromEnvImage()
 * on the file -> "partition layout of the board (env partition read in Loader mode): <mtdparts>"
 * (LayoutSource::LoaderEnv). Only when the env read could not run, failed, or holds no mtdparts
 * ("the env partition of the board holds no usable layout (<not started: ...> / <exit code n:
 * tail> / <no mtdparts in the env partition> / <cannot write <file>>); reading the partition table
 * instead (upgrade_tool PL)") does `upgrade_tool -s <LocationID> PL` run
 * (RockchipTool::partitionListOn(), parsed with parsePartitionList() -> "partition layout of the
 * board (upgrade_tool PL): <mtdparts>", LayoutSource::LoaderPartitionList) - the unverified source
 * (open question 7: a PL table that parses but is a tool default would otherwise become "the
 * board's", trip rule 2 against the correct image and be remembered under "*" for every later
 * Loader-met flash; it is logged so the first G2 answers the question); both failing -> "the
 * board's layout could not be read in Loader mode: <why>" and the flow goes on as 0.7.0 did: the
 * image's env.img layout when it has one, else finished(false, "partition layout unknown: ...").
 * A layout read this way becomes mtdParts(), is reported with mtdPartsRead() (the owner stores
 * it under the Loader-mode key "*") and the writes log "flash layout in use (from the board's env
 * partition, read in Loader mode): ..." / "flash layout in use (from upgrade_tool PL in Loader
 * mode): ..."; layoutSource() says which source they used. A remembered layout (setMtdParts(),
 * the owner's "*" entry: the layout read last from ANY board on this PC, not this board's) whose
 * BoardProfiles::byLayout() profile contradicts the profile of the adopted device's Loader PID is
 * not this board's: it is logged - "the remembered layout is a Node (RV1106BG2)'s; the board's
 * Loader PID 0x110c says Stacked (RV1106G2): reading the board's own" - and the read above runs
 * as if nothing were remembered (else rule 2 would refuse the correct image, and --force-layout,
 * the one flag that disarms rule 2, would become the habit). A Maskrom board (UF) reads nothing.
 * The mtdparts rendered from a PL table is RockchipTool's normalized form ("sfc_nor:" and one
 * "<size>[@<offset>](<name>)" per entry, K / M where exact). The LocationID is the device step 1
 * or step 4 found (DeviceStatus::locationId): every PL / RL is addressed, and the several-devices
 * refusal above guarantees there is exactly one.
 *
 * Verify after write (v0.8.0, verify-rl; docs/DESIGN.md 4.16). With verifyAfterWrite() every WL
 * that exited clean is followed, before the next row, by `upgrade_tool -s <LocationID> RL
 * <offset / 512> <ceil(fileSize / 512)> <extractDirectory()>/.verify/<name>.img`
 * (RockchipTool::readLbaOn(); the device step 1 / 4 found, never unaddressed; one process at a
 * time, so an RL never runs while this board's WL is in flight) and
 * RockchipTool::compareReadBack() over the image's length: "verifying rootfs: reading 7481
 * sectors back from sector 8064 (upgrade_tool -s 104 RL)", then "rootfs verified: 3830272 bytes
 * read back equal the image (4.1 s)" (the temporary deleted) or the end of the flow -
 * finished(false, "verifying rootfs failed: byte 4660 of the image differs (flash offset
 * 0x3F1234): read 0x3c, image 0xff; the board is left in Loader mode, nothing else is written") /
 * finished(false, "verifying rootfs failed: upgrade_tool RL read 1024 of 3830272 bytes (short
 * read)") / finished(false, "verifying rootfs failed: <the RL's tail, or 'cannot run
 * upgrade_tool: ...'>") - no RD, nothing re-written (the temporary is kept for a look); the next
 * write is the owner's decision. progress("Verifying rootfs (1 of 1)", the RL's percentage over
 * the row's bytes: its "(nn%)" lines parse like WL's); partitionVerified() per row,
 * verifyResults() the rows of this flow. Maskrom mode (UF) logs "verify after writing: not
 * available in MASKROM mode (UF writes and checks the whole image itself)" and verifies nothing.
 */
class SocFlasher : public QObject
{
    Q_OBJECT
public:
    enum class Phase {
        Idle, Detecting, RebootingToLoader, WaitingForLoader, Flashing, WaitingForAdb,
        ReadingBoardInfo, Done, Failed, Cancelled
    };
    Q_ENUM(Phase)

    struct DeviceStatus
    {
        enum class Mode { None, NoDriver, Adb, Loader, Maskrom };
        Mode mode = Mode::None;
        QString serial;          ///< ADB serial, or the rockusb SerialNo when the tool prints one
        QString detail;          ///< "DevNo=1 Vid=0x2207 Pid=0x110e LocationID=104", or the adb state
        QString hint;            ///< UsbProbe::driverHint() for NoDriver, else empty
        /// (v0.8.0) Evidence carried along for the profiles and the addressed reads, NOT compared
        /// by operator== (the 0.7.0 equality stays): the rockusb line's PID (0x110e / 0x110c; 0 for
        /// an ADB board; filled for a Maskrom device too, for the fleet's port check, but handed to
        /// the profiles only with Mode::Loader - the class comment) and its LocationID ("104"; ""
        /// for an ADB board or none). The status carries the FIRST device `LD` listed, as in 0.7.0;
        /// `detail` is that line. The flow refuses when LD listed more than one (the class comment).
        quint16 pid = 0;
        QString locationId;
        QString text() const;    ///< "ADB device 0225ce3c8d8d6e44" / "Loader mode (DevNo=1 ...)" / "Maskrom mode" / "no board" / the hint
        bool operator==(const DeviceStatus& other) const;   ///< mode, serial, detail and hint, as in 0.7.0
    };

    explicit SocFlasher(ToolRunner* runner, QObject* parent = nullptr);
    ~SocFlasher() override;

    RockchipTool* upgradeTool() const;
    AdbTool* adb() const;
    void setSerial(const QString& serial);          ///< pick one ADB board; empty = the only one
    QString serial() const;
    void setImagePath(const QString& path);
    QString imagePath() const;
    void setWaitTimeoutSeconds(int seconds);        ///< default 30 (Loader wait)
    int waitTimeoutSeconds() const;
    void setAdbReturnTimeoutSeconds(int seconds);   ///< default 60
    int adbReturnTimeoutSeconds() const;
    void setPollIntervalMs(int ms);                 ///< default 1000
    int pollIntervalMs() const;
    void setWaitForAdb(bool on);                    ///< default true (usb_update.sh NO_WAIT=0)
    bool waitForAdb() const;

    /// The flash layout for FlashMethod::PartitionWrites (see step 5): the mtdparts string of
    /// the board, with or without the "mtdparts=" prefix. Step 2 overwrites it with what the
    /// running board reports; the owner sets a remembered one for a board already in Loader mode.
    void setMtdParts(const QString& mtdparts);
    QString mtdParts() const;
    /// Where `EXF` unpacks the image; empty (default) = "<image folder>/<image base name>.extracted",
    /// created on demand and reused (the files are overwritten by every flash).
    void setExtractDirectory(const QString& dir);
    QString extractDirectory() const;

    enum class FlashMethod { None, PartitionWrites, UpgradeFirmware };
    Q_ENUM(FlashMethod)
    FlashMethod flashMethod() const;                ///< decided when the download mode is known; None before

    /// The partition rows of the current image in the current layout (RockchipTool::planPartitions()
    /// on the extracted image): empty until planPartitions() ran for this image. The flash in
    /// Loader mode writes the rows whose `selected` is set (setSelectedPartitions()); Maskrom mode
    /// writes the whole image with UF and ignores the selection (logged).
    QList<PartitionImage> partitions() const;
    /// Which rows the next PartitionWrites flash writes: names; an empty list = every row that is
    /// inLayout and fits (the default). Unknown names are ignored.
    void setSelectedPartitions(const QStringList& names);
    QStringList selectedPartitions() const;

    // ---- Board profiles (v0.8.0, profiles; BoardProfile.h) -------------------------------------
    /// --profile ID / the tab's Board profile box: the board's profile without detection; "" (the
    /// default) = detect. An id BoardProfiles::byId() does not know counts as "" (the owners
    /// validate: su-fw refuses it as a usage error).
    void setProfileOverride(const QString& id);
    QString profileOverride() const;
    void setForceLayout(bool on);               ///< rule 2's override (--force-layout): write the image's layout over a differing board layout; default false
    bool forceLayout() const;
    void setForceLoaderPartitions(bool on);     ///< rule 3's override (--force-loader-partitions): write idblock / uboot / env / meta on a profile without a documented MASKROM entry; default false
    bool forceLoaderPartitions() const;
    /// Rule 2's question when forceLayout() is off: (the image's layout, the board's) -> true
    /// writes the image's layout anyway. Asked at most once per flow, from the event loop; the
    /// handler is copied before the call and may run a dialog's nested loop (the flow checks
    /// afterwards that it is still the same flow). No handler = refuse.
    using LayoutConflictHandler = std::function<bool(const QString& imageLayout, const QString& boardLayout)>;
    void setLayoutConflictHandler(LayoutConflictHandler handler);
    /// Rule 3's question when forceLoaderPartitions() is off: (the board's profile, the loader
    /// partitions about to be written, in row order) -> true writes them. The same rules.
    using LoaderPartitionsHandler = std::function<bool(const BoardProfile& profile, const QStringList& names)>;
    void setLoaderPartitionsHandler(LoaderPartitionsHandler handler);
    ProfileDetection detectedProfile() const;   ///< the board's profile as the current / last flow or query decided it (known() false before, or when nothing matched)
    ProfileDetection imageProfile() const;      ///< the image's: its RKFW header's chip tag, else the plan's env.img layout; known() false without an image

    // ---- The layout's source (v0.8.0, layout-pl) --------------------------------------------------
    /// Where the layout the writes use came from: Owner = setMtdParts() (remembered / --layout),
    /// Board = step 2's /proc/cmdline, Image = the plan's env.img, LoaderEnv = the env partition
    /// read in Loader mode (`RL 0 128` + mtdPartsFromEnvImage(); the first source tried there),
    /// LoaderPartitionList = `upgrade_tool -s <id> PL` in Loader mode (the fallback when the env
    /// partition holds no layout; unverified, open question 7); None before step 5 decided, and in
    /// Maskrom mode. (The enum order is not the order of the sources.)
    enum class LayoutSource { None, Owner, Board, Image, LoaderPartitionList, LoaderEnv };
    Q_ENUM(LayoutSource)
    LayoutSource layoutSource() const;
    /// "the layout given for the board" / "the board's /proc/cmdline" / "the image's env.img" /
    /// "upgrade_tool PL in Loader mode" / "the board's env partition, read in Loader mode"; "" for None.
    static QString layoutSourceText(LayoutSource source);

    // ---- Verify after write (v0.8.0, verify-rl) --------------------------------------------------
    void setVerifyAfterWrite(bool on);          ///< default false; the tab's "Verify after writing", su-fw --verify
    bool verifyAfterWrite() const;
    QList<ReadBackResult> verifyResults() const;   ///< one per row verified in the current / last flow, row order (a failed one last)

    bool isRunning() const;
    Phase phase() const;
    DeviceStatus lastStatus() const;

public slots:
    void queryStatus();          ///< asynchronous; statusChanged() follows (dropped while one runs or while flashing)
    /// Unpack the current image (EXF into extractDirectory()) and compute partitions() from
    /// mtdParts(); asynchronous: partitionsPlanned(ok, rows, message). False (nothing emitted)
    /// while a flash or another plan runs or without an image / tool path. A plan is kept per
    /// (image path, image mtime, layout); start() reuses it when still valid and re-plans otherwise.
    bool planPartitions();
    bool start();                ///< false while running
    void cancel();               ///< ends the waiting phases; refused while Flashing

signals:
    void statusChanged(const SocFlasher::DeviceStatus& status);
    void phaseChanged(SocFlasher::Phase phase);
    void downloadModeNeeded();   ///< step 3: no board on LD or ADB
    void progress(const QString& stage, int percent);   ///< UF progress; percent -1 = stage only
    void toolOutput(const QString& line);               ///< raw tool lines as they arrive
    void logLine(const QString& line);                  ///< the flasher's own narration
    void boardInfo(const QString& text);                ///< /proc/version + uptime after the flash
    /// Step 2 read the layout from the running board; (v0.8.0) also a layout read in Loader mode
    /// (the env partition, or PL) - the owner stores that one under the Loader-mode key "*" and
    /// under no serial: the flow's status is not Adb then, and the last ADB serial the owner saw is
    /// another board's (FirmwareJob's mtdPartsRead wiring passes "" in that case, never a stale
    /// serial).
    void mtdPartsRead(const QString& mtdparts);
    void partitionsPlanned(bool ok, const QList<PartitionImage>& partitions, const QString& message);
    void finished(bool ok, const QString& message);
    void profileDetected(const ProfileDetection& profile);   ///< (v0.8.0) the board's profile with its evidence, whenever a query or the flow decided it (also when unknown)
    void partitionVerified(const ReadBackResult& result);    ///< (v0.8.0) after every verify read-back of the flow, ok or not

private:
    struct Private;
    std::unique_ptr<Private> d;
};
