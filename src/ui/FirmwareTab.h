#pragma once

#include <QWidget>
#include <QList>
#include <QString>
#include <QStringList>
#include <functional>
#include <memory>

#include "firmware/CcPackage.h"
#include "firmware/ImagePuller.h"
#include "firmware/McuUpdater.h"
#include "firmware/SocFlasher.h"

class QComboBox;
class QLineEdit;
class QLabel;
class QPushButton;
class QRadioButton;
class QProgressBar;
class QPlainTextEdit;
class SessionWidget;
class SshProfileStore;
class SshConnection;
class ToolRunner;
class McuLink;

/**
 * The Firmware tab (File > Firmware Update, Ctrl+Shift+U; one instance in the main window's
 * QTabWidget next to the session tabs; docs/firmware-update/REQUIREMENTS.md): pull the latest
 * build from the Linux build machine, flash the Rockchip SoC and update the CC1310 over the
 * board's USB-C cable - the Pull & Flash button does all three.
 *
 *  ┌ Source ──────────────────────────────────────────────────────────────────────────┐
 *  │ Build machine [profiles / recent / user@host v] [⚙]  Remote folder [~/rk/.../output/image]  │
 *  │ Local cache [<data>/firmware] [...]   [Pull]  [Use local image...]                │
 *  │ Image: update.img 11.4 MB, built 2026-10-13 21:45 (v1.3.0-4-ge0e0d41) · previous: ... [Roll back] │
 *  ├ SoC ─────────────────────────────────────────────────────────────────────────────┤
 *  │ Device: ADB device 0225ce3c8d8d6e44           Image: Update Firmware, chip 110E, 9 entries │
 *  │ [Flash SoC] [Cancel]                                                              │
 *  ├ MCU (CC1310) ────────────────────────────────────────────────────────────────────┤
 *  │ Package [releases/firmware-0.8.1.pkg] [...]  CC1M 0.8.1, 78,040 bytes, CRC OK      │
 *  │ Running: 0.8.1 rx [Read]   Role: (•) rx ( ) tx   [Update MCU] [Set role]          │
 *  ├──────────────────────────────────────────────────────────────────────────────────┤
 *  │ [Pull & Flash]  [########------] 42 %  Download Image...                          │
 *  ├ Log ─────────────────────────────────────────────────────────────────────────────┤
 *  │ 10:41:02 upgrade_tool.exe LD                                        [Clear] [Save] │
 *  └──────────────────────────────────────────────────────────────────────────────────┘
 *
 * Source: the combo lists the SSH profiles and recent targets of the SshProfileStore (same
 * texts and parsing as SshConnectionBar: a profile name, "user@host[:port]", a restore key);
 * the gear opens the profile manager (sshProfilesEditRequested()). The remote folder is
 * remembered per target (AppSettings::firmwareRemoteFolder(target)); a new target starts with
 * the last folder used, initially "~/rk/rockchip-platforms/output/image". Pull connects its own
 * SshConnection (the tab owns one; host key and authentication go through HostKeyDialog /
 * AuthPromptDialog exactly as a session tab does, with "remember" and the DPAPI store), then
 * runs the ImagePuller and reports remote size, mtime and git describe before the transfer;
 * the connection is closed again after the pull (or kept while a Pull & Flash runs). "Use local
 * image..." picks any *.img file instead (validated with the RKFW magic; shown with its SFI
 * info). Roll back swaps the cache's update.img with update.prev.img.
 *
 * SoC: the device line is refreshed by SocFlasher::queryStatus() on a timer
 * (statusRefreshIntervalMs, 2000; 0 = off, the tests call refreshStatus()) while the tab is
 * visible and nothing runs; the hint for a 0x2207 device without a driver offers the bundled
 * DriverAssistant (installDrivers(): starts tools/DriverAssistant/DriverInstall.exe, or says it
 * is missing). The image line shows RockchipTool SFI info of the selected image. Flash SoC runs
 * the SocFlasher (its downloadModeNeeded() asks, through sessionProvider(), whether to send
 * "reboot loader\r" through a connected session tab - a QMessageBox naming the tab, Yes sends it
 * with SessionWidget::sendBytes() - or shows the MASKROM instructions); Cancel is enabled only
 * while waiting, never during UF; after the flash the board info is shown and logged.
 *
 * MCU: the package picker loads a CcPackage (header and both CRC32s checked; target, version,
 * size shown; a bad file is refused with the reason); Read runs `rfmcu cmd version` through adb
 * (McuLink::cliCommand(), no bridge) and fills "Running: <version> <role>" and pre-selects the
 * reported role; the role radios are what the user confirms (never guessed silently: Update MCU
 * is disabled until the running role was read or the user picked one). Update MCU opens the
 * McuLink, runs McuUpdater::flash(role) with throughSoc on, shows progress per ACK and the
 * restart banner, closes the link; Set role does McuUpdater::setRole(role) the same way.
 *
 * Pull & Flash: pull (skipped when unchanged) -> Flash SoC -> wait for ADB -> when a package is
 * selected and its version differs from the running MCU version (read after the SoC is back),
 * Update MCU with the selected role; stops at the first failure. SoC first, because the MCU path
 * needs rfmcu on the SoC.
 *
 * Every step writes a time-stamped line to the log pane (and the System Log through lcFirmware)
 * - the command lines run, the tools' output, the flasher / updater narration (McuUpdater's
 * fw_update.py wording), never a secret. Save writes the pane to a text file; Clear empties it.
 * isBusy() is true from the start of any operation to its finished(); busyChanged() lets
 * MainWindow refuse to close the tab (and ask on quit) while a flash is under way;
 * statusMessage() goes to the status bar like a session's.
 *
 * Tests (tst_firmwaretab, offscreen) construct the tab with a FakeToolRunner, set the tool paths
 * through setToolPaths(), reach every control by the accessors below (object names equal the
 * accessor names, e.g. "pullButton") and drive the pull against TestSshServer by pointing the
 * source target at it.
 */
class FirmwareTab : public QWidget
{
    Q_OBJECT
public:
    using SessionProvider = std::function<QList<SessionWidget*>()>;

    /// `runner` null = a ProcessToolRunner owned by the tab; `profiles` not owned, may be null.
    explicit FirmwareTab(SshProfileStore* profiles, ToolRunner* runner = nullptr, QWidget* parent = nullptr);
    ~FirmwareTab() override;

    QString title() const;                         ///< tr("Firmware")
    bool isBusy() const;
    QString persistentStatusMessage() const;       ///< "Flashing SoC: Download Image... 42 %" while busy, else empty
    QString currentOperation() const;              ///< "pull" / "flash" / "mcu" / "set-role" / "info" / "pull-flash" / ""

    // ---- Collaborators (fakes injected by the tests) --------------------------------------
    ToolRunner* toolRunner() const;
    SocFlasher* socFlasher() const;
    McuLink* mcuLink() const;
    McuUpdater* mcuUpdater() const;
    ImagePuller* imagePuller() const;
    SshConnection* sshConnection() const;
    void setToolPaths(const QString& upgradeTool, const QString& adb, const QString& driverInstaller = QString());
    void setSessionProvider(SessionProvider provider);
    void setStatusRefreshIntervalMs(int ms);
    int statusRefreshIntervalMs() const;

    // ---- State -----------------------------------------------------------------------------
    QString sourceTarget() const;                  ///< the combo's text (profile name / user@host / restore key)
    void setSourceTarget(const QString& keyOrTarget);
    QString remoteFolder() const;
    void setRemoteFolder(const QString& folder);
    QString cacheDirectory() const;
    void setCacheDirectory(const QString& dir);
    QString imagePath() const;                     ///< the image Flash SoC would use (cache or local pick)
    void setImagePath(const QString& path);        ///< validated like "Use local image..."
    QString packagePath() const;
    bool setPackagePath(const QString& path);      ///< false (with the reason in the log and status) when invalid
    CcPackage package() const;
    quint32 selectedRole() const;                  ///< 0 while none chosen / read
    void setSelectedRole(quint32 role);
    QString runningMcuVersion() const;             ///< last Read result ("0.8.1"), empty when unknown
    QString runningMcuRole() const;

    // ---- Widgets (object names = accessor names) --------------------------------------------
    QComboBox* sourceCombo() const;
    QLineEdit* remoteFolderEdit() const;
    QLineEdit* cacheDirEdit() const;
    QPushButton* pullButton() const;
    QPushButton* localImageButton() const;
    QLabel* imageInfoLabel() const;
    QPushButton* rollbackButton() const;
    QLabel* socStatusLabel() const;
    QLabel* socImageLabel() const;
    QPushButton* flashSocButton() const;
    QPushButton* cancelButton() const;
    QLineEdit* packageEdit() const;
    QPushButton* packageButton() const;
    QLabel* packageInfoLabel() const;
    QLabel* mcuStatusLabel() const;
    QPushButton* readMcuButton() const;
    QRadioButton* roleRxRadio() const;
    QRadioButton* roleTxRadio() const;
    QPushButton* updateMcuButton() const;
    QPushButton* setRoleButton() const;
    QPushButton* pullAndFlashButton() const;
    QProgressBar* progressBar() const;
    QLabel* progressLabel() const;
    QPlainTextEdit* logView() const;
    QPushButton* clearLogButton() const;
    QPushButton* saveLogButton() const;
    QPushButton* installDriversButton() const;     ///< shown only with the NoDriver status

public slots:
    void pull();
    void flashSoc();
    void updateMcu();
    void setMcuRole();
    void readMcuInfo();
    void pullAndFlash();
    void cancel();
    void rollBack();
    void chooseLocalImage();
    void choosePackage();
    void refreshStatus();
    void installDrivers();
    void appendLog(const QString& line);           ///< time-stamped
    void clearLog();
    void saveLog();
    void applyPreferences();                       ///< tool paths, timeouts, cache dir from AppSettings
    void disconnectSource();                       ///< close the tab's SshConnection (no-op when closed)

signals:
    void statusMessage(const QString& message, int timeoutMs);
    void busyChanged(bool busy);
    void titleChanged(const QString& title);
    void sshProfilesEditRequested();
    void logLineAdded(const QString& line);
    /// operation = currentOperation() of the step that ended; a Pull & Flash emits one per step
    /// ("pull", "flash", "mcu") and a final "pull-flash".
    void operationFinished(const QString& operation, bool ok, const QString& message);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    struct Private;
    std::unique_ptr<Private> d;
};
