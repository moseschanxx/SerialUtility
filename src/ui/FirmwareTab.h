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

class QCheckBox;
class QComboBox;
class QTableWidget;
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
class FirmwareJob;

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
 * SshConnection (the tab's FirmwareJob owns one; host key and authentication go through HostKeyDialog /
 * AuthPromptDialog exactly as a session tab does, with "remember" and the DPAPI store), then
 * runs the ImagePuller and reports remote size, mtime and git describe before the transfer;
 * the connection is closed again after the pull (or kept while a Pull & Flash runs). "Use local
 * image..." picks any *.img file instead (validated with the RKFW magic; shown with its SFI
 * info). Roll back swaps the cache's update.img with update.prev.img.
 *
 * SoC, partitions (v0.5.1, like SocToolKit's Download page): once an image is selected the tab
 * unpacks it (SocFlasher::planPartitions()) and lists its partitions in partitionTable() - a
 * checkbox, the partition name, the image file, the flash offset, the partition size and the
 * image size, one row per package-file entry; rows that are not in the layout or do not fit are
 * unchecked, disabled and say why. The layout (mtdparts) comes from the board: whenever the
 * status refresh sees an ADB device whose layout is not remembered yet it reads /proc/cmdline
 * once (AppSettings::firmwareMtdParts(serial)); SocFlasher::mtdPartsRead() stores it too. Without
 * a layout the table says "partition layout unknown: connect the board running once". "All" /
 * "None" buttons set the checkboxes. Flash SoC in Loader mode writes the checked rows (SocFlasher
 * FlashMethod::PartitionWrites, the way SocToolKit does it, no chip check) and says which ones in
 * the log; in MASKROM mode it writes the whole image with UF and the log says the selection was
 * ignored. Pull & Flash writes every row. flashLayout() is the mtdparts in use.
 *
 * SoC: the device line is refreshed by SocFlasher::queryStatus() on a timer
 * (statusRefreshIntervalMs, 2000; 0 = off, the tests call refreshStatus()) while the tab is
 * visible and nothing runs; the hint for a 0x2207 device without a driver offers the bundled
 * DriverAssistant (installDrivers(): starts tools/DriverAssistant/DriverInstall.exe, or says it
 * is missing). The image line shows RockchipTool SFI info of the selected image. Flash SoC runs
 * the SocFlasher (its downloadModeNeeded() asks, through sessionProvider(), whether to send
 * "reboot loader\r" through a connected session tab - a QMessageBox naming the tab, Yes sends it
 * with SessionWidget::sendBytes() - or shows the MASKROM instructions; the question closes by
 * itself, and nothing is sent, once the flasher stops waiting for Loader mode, e.g. because the
 * user held MASKROM meanwhile); Cancel is enabled only while waiting, never during UF; after the
 * flash the board info is shown and logged.
 *
 * MCU: the package picker loads a CcPackage (header and both CRC32s checked; target, version,
 * size shown; a bad file is refused with the reason); Read stops a stale rfmcu first
 * (`killall rfmcu 2>/dev/null; true`, as McuLink::open() does: an open `rfmcu console` holds the
 * UART lock), then runs `rfmcu cmd version` through adb (McuLink::cliCommand(), no bridge) and
 * fills "Running: <version> <role>" and pre-selects the reported role; the role radios are what
 * the user confirms (never guessed silently: Update MCU is disabled until the running role was
 * read or the user picked one in this session - last session's role is shown in the radios but
 * not confirmed). Update MCU opens the McuLink, runs McuUpdater::flash(role) with throughSoc on,
 * shows progress per ACK and the restart banner, closes the link; Set role does
 * McuUpdater::setRole(role) the same way.
 *
 * Pull & Flash: pull (skipped when unchanged) -> Flash SoC -> wait for ADB -> when a package is
 * selected and its version differs from the running MCU version (read after the SoC is back),
 * Update MCU with the selected role; an MCU that does not answer the version read (rfmcu exit 3,
 * or 2: in its UART updater after an interrupted transfer, runbook Part J) is updated too, with
 * the confirmed role; stops at the first failure (adb errors included). SoC first, because the
 * MCU path needs rfmcu on the SoC.
 *
 * View and chain (v0.5.1): every operation above runs in a FirmwareJob (src/firmware/FirmwareJob.h,
 * su_core) that the tab owns - the steps, the rules, the log lines and the messages are the job's,
 * the same ones the console tool su-fw runs. The tab keeps its widgets, dialogs, settings and the
 * status timer: it hands the fields and the Preferences to the job when an operation starts,
 * renders what the job reports (log lines with the time added, progress, the lasting status, the
 * device, image, package, MCU and partition state), answers the job's questions with
 * HostKeyDialog / AuthPromptDialog / the "reboot loader" QMessageBox, remembers in AppSettings what
 * the job reports (the pull source, a board's flash layout, the cache image after a pull), and
 * returns the job's collaborators from toolRunner(), socFlasher(), mcuLink(), mcuUpdater(),
 * imagePuller() and sshConnection().
 *
 * Every step writes a time-stamped line to the log pane (and the System Log through lcFirmware)
 * - the command lines run, the tools' output, the flasher / updater narration (McuUpdater's
 * fw_update.py wording), never a secret. Save writes the pane to a text file; Clear empties it.
 * isBusy() is true from the start of any operation to its finished(); busyChanged() lets
 * MainWindow refuse to close the tab (and ask on quit, or refuse to quit while socFlasher() is
 * Flashing) while a flash is under way;
 * statusMessage() goes to the status bar like a session's.
 *
 * Tests (tst_firmwaretab, offscreen) construct the tab with a FakeToolRunner, set the tool paths
 * through setToolPaths(), reach every control by the accessors below (object names equal the
 * accessor names, e.g. "pullButton") and drive the pull against TestSshServer by pointing the
 * source target at it.
 *
 * Board profiles (v0.8.0, profiles; BoardProfile.h, docs/DESIGN.md 4.14). The SoC section's device
 * row gains the profile: profileCombo() ("Board profile:" - "Auto-detect" and one entry per
 * BoardProfiles::all(), by name; remembered in AppSettings::firmwareProfile() and handed to the
 * job as setProfileOverride() whenever it changes and before every operation) and profileLabel()
 * (the job's boardProfile() as "Profile: Node (RV1106BG2) - from the board's flash layout (...)" /
 * "Profile: unknown", refreshed on boardProfileChanged(); the image's profile goes into the image
 * line's tooltip). socStatusLabel() keeps its 0.7.0 text. The partition table's default checks
 * follow the profile's dailyPartitions when a new image is planned and the profile is known (the
 * Node: rootfs; the stacked board: boot; unknown: every row, as before) - "All" / "None" still
 * rule afterwards. The three questions are the job's handlers answered with a QMessageBox
 * (Question, the ProfileRules refusal text as the message with its "--force..." sentence
 * replaced by "Write anyway?" / "Proceed?", buttons "Write the image's layout" / "Write them" /
 * "Proceed" and "Cancel", default Cancel): setLayoutConflictHandler() (rule 2),
 * setLoaderPartitionsHandler() (rule 3), setSocPowerHandler() (rule 4); a No ends the operation
 * with the refusal as its result. A box left on a profile that contradicts the attached board's
 * Loader PID (a Node in Loader mode under "Stacked (RV1106G2)") is no question: the flash fails
 * with ProfileRules::overrideRefusal()'s text ("... drop --profile" reads, for the tab, as "set
 * the Board profile box back to Auto-detect"), shown like any failure; the box is never changed
 * by the tab. The Loader-wait hint and the MASKROM instructions of the
 * profile come through the job's lines as before. The tab reads the image's RKFW header through
 * the job (its own SFI stays for the image line's details).
 *
 * Verify after write (v0.8.0, verify-rl): verifyCheckBox() ("Verify after writing", next to Flash
 * SoC; remembered in AppSettings::firmwareVerifyAfterWrite(), handed to the job as
 * setVerifyAfterWrite() before every flash and Pull & Flash); the per-row verdicts arrive as log
 * lines and the progress bar shows "Verifying <name> (k of N)"; a failed verify is the flash's
 * failure, shown like any other.
 *
 * Inventory (v0.8.0, inventory): the tab's job carries an InventoryRecorder (one line in the
 * constructor; its logLine() goes to appendLog()), so every status and operation of the tab
 * records its board as su-fw does. The tab shows no inventory table in 0.8.0.
 */
class FirmwareTab : public QWidget
{
    Q_OBJECT
public:
    using SessionProvider = std::function<QList<SessionWidget*>()>;

    /// `runner` null = a ProcessToolRunner owned by the tab's FirmwareJob; `profiles` not owned, may be null.
    explicit FirmwareTab(SshProfileStore* profiles, ToolRunner* runner = nullptr, QWidget* parent = nullptr);
    ~FirmwareTab() override;

    QString title() const;                         ///< tr("Firmware")
    bool isBusy() const;
    QString persistentStatusMessage() const;       ///< "Flashing SoC: Download Image... 42 %" while busy, else empty
    QString currentOperation() const;              ///< "pull" / "flash" / "mcu" / "set-role" / "info" / "pull-flash" / ""

    // ---- Collaborators (fakes injected by the tests) --------------------------------------
    FirmwareJob* firmwareJob() const;              ///< the chain the tab drives (owned by the tab)
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
    QComboBox* profileCombo() const;               ///< (v0.8.0) "Auto-detect" + one item per BoardProfiles::all(); item data = the id ("" for auto)
    QLabel* profileLabel() const;                  ///< (v0.8.0) "Profile: <name> - <source>" / "Profile: unknown"
    QCheckBox* verifyCheckBox() const;             ///< (v0.8.0) "Verify after writing"
    QLabel* socImageLabel() const;
    QTableWidget* partitionTable() const;          ///< rows = SocFlasher::partitions(); column 0 = checkbox
    QPushButton* partitionsAllButton() const;
    QPushButton* partitionsNoneButton() const;
    QLabel* partitionsLabel() const;               ///< "7 partitions, 7 selected (11.4 MB)" / the layout-unknown text
    QString flashLayout() const;                   ///< the mtdparts in use (remembered or just read), "" when unknown
    QStringList selectedPartitions() const;        ///< checked rows (names)
    void setSelectedPartitions(const QStringList& names);
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
    void planPartitions();                         ///< re-run the plan for the current image and layout
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
