#pragma once

#include <QObject>
#include <QString>
#include <memory>

#include "firmware/AdbTool.h"
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
 *       new layout) - and every image whose partition is
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
        QString text() const;    ///< "ADB device 0225ce3c8d8d6e44" / "Loader mode (DevNo=1 ...)" / "Maskrom mode" / "no board" / the hint
        bool operator==(const DeviceStatus& other) const;
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
    void mtdPartsRead(const QString& mtdparts);         ///< step 2 read the layout from the running board
    void partitionsPlanned(bool ok, const QList<PartitionImage>& partitions, const QString& message);
    void finished(bool ok, const QString& message);

private:
    struct Private;
    std::unique_ptr<Private> d;
};
