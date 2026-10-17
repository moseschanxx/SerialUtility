#pragma once

#include <QObject>
#include <QString>
#include <memory>

#include "firmware/UsbProbe.h"

class FirmwareJob;
class Inventory;

/**
 * The one hook that feeds the inventory (v0.8.0; docs/DESIGN.md 4.17): constructed on a FirmwareJob
 * by its owner - one line in FirmwareCli::Private::createJob() and one in FirmwareTab's constructor
 * - it listens to the job's signals and writes Inventory records through Inventory::update() (one
 * atomic write per event; the job itself reads no inventory and is not changed for it). Headless,
 * no question, never blocks on anything but the inventory's lock (kLockWaitMs).
 *
 * What it records, from which signal, for which board:
 *  - statusChanged(status) with Mode::Adb and a non-empty serial: that serial becomes
 *    lastSerial(); the record gets lastSeen = now, firstSeen when new, and gadgetSerialString
 *    while the record's value is still Tri::Unknown: ONLY THEN the USB enumeration runs
 *    (setUsbEnumerator(); UsbProbe::presentDevices() is SetupDi on Windows, synchronous in the
 *    status slot - on the GUI thread every 2 s in the tab - for a value that never changes once
 *    known) and, when it shows exactly one ADB gadget, Yes when its serial() equals the status
 *    serial, No when its instance id is a bus address (the Node's "6&...": open question 5);
 *    unchanged otherwise (several gadgets, none). Throttled: a status that changes nothing but
 *    lastSeen is written only when the stored lastSeen is older than kSeenRefreshSeconds (the tab
 *    refreshes every 2 s). Event "status". A statusChanged() whose mode is NOT Adb (Loader,
 *    Maskrom, None, NoDriver), or whose serial is empty (several ADB boards, none), CLEARS
 *    lastSerial() and records nothing: the board the job sees is no longer the one it saw.
 *  - boardProfileChanged(profile) with profile.known(), a lastSerial() and a source that is NOT
 *    Source::Option: profile = profile.profile.id (unchanged when equal). The job emits
 *    statusChanged() before it recomputes the profile, so lastSerial() is set by then; the layout
 *    of a first `su-fw status` is read after the status and the profile follows it - the record
 *    of a first status therefore ends with layout AND profile set. An override (--profile, the
 *    tab's box) is the operator's claim, not a detection: it is never inventoried (a Node flashed
 *    while the box says Stacked stays "node", or "" if nothing ever detected it). Event "profile".
 *  - mtdPartsRead(serial, mtdparts) with a non-empty serial: layout (normalized). Event "layout".
 *    An EMPTY serial (a layout read in Loader mode, stored by the job under "*") is never recorded
 *    - no record is keyed "" - and logLine() says once per flow "inventory: the board in Loader
 *    mode has no serial; not recorded".
 *  - boardPortLearned(serial, port) with a non-empty serial: port. Event "port".
 *  - mcuInfoChanged(): mcuVersion, mcuRole, mcuRead = now for lastSerial(). Event "mcu".
 *  - stepFinished(step, ok, message) for a single-board step that touches a board (kOpFlash,
 *    kOpMcu, kOpSetRole, kOpInfo; never kOpPull, which has no board): lastOperation = {step, ok ?
 *    "done" : message == "cancelled" (or starting with "interrupted") ? "cancelled" : "failed",
 *    message, now}, lastSeen; a kOpFlash that ended ok also sets lastImage = {the image's file name,
 *    its RKFW chip tag (RockchipTool::readRkfwHeader()), its MD5 (Inventory::md5Hex(), computed
 *    once per path and mtime and cached for the job's life), the cache note's git describe when
 *    the image is the cache's (FirmwareJob::cacheEntry()), the rows the write put on the board,
 *    now} - the rows: (fix4, m4) SocFlasher::writtenPartitions() for a PartitionWrites flash (the
 *    rows the flow wrote completely, never a selected row it skipped), every row of the job's
 *    partitions() for a MASKROM (UF) flash;
 *    merged into the record's lastImage.partitions when its md5 is the same, replacing it
 *    otherwise (Inventory.h). The board is lastSerial() - the serial of the LAST statusChanged()
 *    before the stepFinished(): the flow's own Detecting query, which reports the ADB board the
 *    flasher tells `reboot loader`. A flash of a board met in Loader / MASKROM mode reports that
 *    status first, which clears lastSerial(): nothing is recorded, and logLine() says "inventory:
 *    the board in Loader mode has no serial; not recorded" - so a board X seen on ADB earlier (the
 *    tab's 2 s refresh), then unplugged for a bricked board Y put into Loader mode by hand, never
 *    gets Y's image on its record. operationFinished(kOpPullFlash, ...) records lastOperation =
 *    {"pull-flash", ...} on top of its steps. Event = the step name.
 *  - boardFinished(board) of a fleet: for board.serial - port, layout, profile (each when the
 *    board has it; the profile is the fleet's detection, never the override), lastSeen,
 *    lastOperation = {"fleet-flash", board.resultText(), board.detail, now}; result Done or
 *    NotBack also sets lastImage as above with board.written as the rows. A Ready board (a dry
 *    run: nothing touched it) updates lastSeen, and port when known, ONLY - no lastOperation, no
 *    image. Event "fleet-flash".
 * The inventory's file is Inventory::defaultFilePath() unless setFilePath() was called before the
 * first event (the tests point it at a temporary file). Every write ends in recorded(serial, event)
 * or failed(error), and logLine() carries one line for the owner's log: "inventory: board
 * 0225ce3c8d8d6e44 recorded (flash)" / "inventory: cannot write <native path>: <error>" /
 * "inventory: <corrupt file> was unreadable and moved to <new name>" - the owner connects it to
 * its log (FirmwareCli: log(); FirmwareTab: appendLog()). Never a secret in any of them.
 */
class InventoryRecorder : public QObject
{
    Q_OBJECT
public:
    static constexpr int kSeenRefreshSeconds = 60;

    /// Connects to `job` at once (not owned; the recorder must not outlive it).
    explicit InventoryRecorder(FirmwareJob* job, QObject* parent = nullptr);
    ~InventoryRecorder() override;

    void setFilePath(const QString& path);   ///< before the first event; "" = Inventory::defaultFilePath()
    QString filePath() const;
    Inventory* inventory() const;            ///< the store (owned), for the CLI's --expect-image check and the tests
    void setEnabled(bool on);                ///< default true; off = listen but write nothing (the tests of other things)
    bool isEnabled() const;
    QString lastSerial() const;              ///< the serial of the last statusChanged() when it showed one ADB board; "" = none yet, or the last status was not such a board (Loader / Maskrom / none / several)
    /// The USB enumeration behind gadgetSerialString: UsbProbe::presentDevices by default; the
    /// tests inject FakeUsbBus::enumerate (this PC's bus must not decide a test).
    void setUsbEnumerator(UsbProbe::Enumerator enumerator);

signals:
    void recorded(const QString& serial, const QString& event);
    void failed(const QString& error);
    void logLine(const QString& line);

private:
    struct Private;
    std::unique_ptr<Private> d;
};
