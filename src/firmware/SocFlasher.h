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
 *     (`adb shell cat /proc/version` first; busybox on the Node board has no uname) then
 *     `adb reboot loader` (its exit status ignored, like the script) -> 4.
 *  3. downloadModeNeeded(): the owner may send "reboot loader" through a session tab or tell the
 *     user to hold MASKROM and replug; the flasher waits like 4 for the board to appear (a Yes /
 *     an appearing board continues, cancel() ends it). The signal is informational: nothing
 *     blocks on it.
 *  4. WaitingForLoader: `LD` every pollIntervalMs (1 s) until a Loader / Maskrom device is
 *     listed, at most waitTimeoutSeconds (30; usb_update.sh TIMEOUT) ->
 *     finished(false, "board did not enter MASKROM/Loader within 30 s"). cancel() here ->
 *     finished(false, "cancelled").
 *  5. Flashing: logLine("flashing <image> (<mode> mode)"), `UF <image>`; progress(stage, percent)
 *     from RockchipTool::ProgressParser; the output's success line is required:
 *     finished(false, "upgrade_tool did not report success: <tail>") otherwise (tail =
 *     RockchipTool::tailForMessage()). Not cancellable: cancel() is refused with
 *     logLine("cannot cancel while upgrade_tool writes the flash").
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

    bool isRunning() const;
    Phase phase() const;
    DeviceStatus lastStatus() const;

public slots:
    void queryStatus();          ///< asynchronous; statusChanged() follows (dropped while one runs or while flashing)
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
    void finished(bool ok, const QString& message);

private:
    struct Private;
    std::unique_ptr<Private> d;
};
