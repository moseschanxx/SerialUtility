#pragma once

#include <QObject>
#include <QList>
#include <QMap>
#include <QPair>
#include <QString>
#include <QStringList>
#include <functional>
#include <memory>

#include "firmware/RockchipTool.h"
#include "firmware/UsbProbe.h"

class AdbTool;
class FleetReport;
class ToolRunner;

/**
 * One board of a fleet flash (FleetFlasher::boards(), FirmwareJob::fleetBoards(), FleetReport):
 * what su-fw prints per line ("[B 62ecfc6ddeb3f579] ..."), as a row of the final table and as an
 * object of the JSON `boards` array. Filled in as the run gets on: selection, mapping, writing,
 * returning. A copy is handed out; the flasher keeps the live one.
 */
struct FleetBoard
{
    enum class Stage { Selected, Mapping, Loader, Writing, Resetting, WaitingForAdb, ReadingInfo, Finished };
    /// Pending: not decided yet. Ready: a dry run's "on ADB, would be written". Done: every row
    /// written, the board reset and back on ADB (or --no-wait). NotBack: every row written and the
    /// board reset, but ADB did not come back in time (or its wait was cancelled) - a fleet counts
    /// it as a failure (the owner's acceptance is "every board back on ADB"), unlike the
    /// single-board flow. Failed: something went wrong on this board. Skipped: never written.
    enum class Result { Pending, Ready, Done, NotBack, Failed, Skipped };

    QString tag;            ///< "B": the run's label - `--serials B=<serial>`, else A, B, ... Z, AA, AB, ... in selection order
    QString serial;         ///< the ADB serial
    QString adbState;       ///< "device" / "unauthorized" / "offline" from `adb devices`; "" = not attached
    QString expectedPort;   ///< the port remembered from an earlier run (PortProvider); "" = none
    QString port;           ///< the USB port key learned in this run ("6&38635FDB&0&2"); "" until mapped
    QString locationId;     ///< upgrade_tool's LocationID learned in this run ("112"; kept after the reset for the table and the JSON); "" until mapped
    QString kernel;         ///< "5.10.209+" from /proc/version before the reboot; "" = unknown
    QString layout;         ///< the mtdparts of the board's /proc/cmdline; "" = none reported
    Stage stage = Stage::Selected;
    Result result = Result::Pending;
    QString detail;         ///< why it failed / was skipped / is not back; for a done board the warning, if any ("" = none)
    QStringList written;    ///< the partitions written completely, in order
    /// A board left in Loader mode by a failed WL (or a pre-RD `LD` that could not run): the
    /// addressed command lines that finish it by hand, in order - one "upgrade_tool -s n WL <sector>
    /// <count> <file>" per row not written (the failed row first), then "upgrade_tool -s n RD".
    /// Empty otherwise, and empty when the id is gone (exit 255 "No found specific rockusb
    /// device!": the hint then names `upgrade_tool LD`). Logged one per line, carried in the JSON.
    QStringList retry;
    bool adbBack = false;   ///< `adb -s <serial> get-state` answered "device" after the reset
    bool leftInLoader = false;   ///< the board is (still) in Loader mode by the last evidence: a failed WL gets no RD (nothing else is written to it; `retry` names the addressed commands that finish it by hand)
    QString boardInfo;      ///< `cat /proc/version; uptime` after the flash; "" = not read
    QString uptime;         ///< "up 0 min": the uptime word of boardInfo; "" = not read
    QString rfmcu;          ///< `rfmcu status` after the flash ("pid=.. uart=owned mcu=alive ..."); "" = not read / no answer
    double seconds = 0.0;         ///< from its `adb reboot loader` to its end (0 for a board never rebooted)
    double mappingSeconds = 0.0;  ///< from its `adb reboot loader` to "in Loader mode" (0 when never mapped)
    double writingSeconds = 0.0;  ///< from its first pre-write check to the end of its RD (0 when never written)

    bool ok() const { return result == Result::Done || result == Result::Ready; }
    QString resultText() const;   ///< "pending" / "ready" / "done" / "not back" / "failed" / "skipped"
    QString stageText() const;    ///< "selected" / "mapping" / "loader" / "writing" / "resetting" / "adb" / "info" / "finished"
    QString label() const { return QStringLiteral("[%1 %2]").arg(tag, serial); }
    bool operator==(const FleetBoard& other) const;
};

/**
 * Several Node boards flashed at once (v0.7.0; the owner's request
 * Firmware/cc1310-firmware/analysis/node_inventory/prompt_parallel_soc_flash.md; docs/DESIGN.md
 * 4.13): the Loader-mode partition writes of SocFlasher for N boards, every upgrade_tool that
 * touches a board addressed with `-s <LocationID>`. A class of its own next to SocFlasher - not a
 * second entry point into it - so the single-board flow of 0.6.0 stays byte for byte. It is owned
 * by FirmwareJob (operation "fleet-flash"), which frames it like every operation (the machine-wide
 * firmware.lock, stepFinished / operationFinished); su-fw is its only user in 0.7.0.
 *
 * Facts it rests on (measured 2026-10-14 on this PC): `adb devices -l` prints no `usb:` path and
 * `adb -s <serial> get-devpath` answers "unknown", so ADB alone cannot tell the port of a serial.
 * Windows names the ADB gadget `USB\VID_2207&PID_0006\<hub>&0&<port>` and the same board in Loader
 * mode `USB\VID_2207&PID_110E\<same hub>&0&<same port>` (UsbDeviceInfo::portKey()). `upgrade_tool
 * LD` lists every rockusb device with a LocationID assigned per USB location (112 / 154 here;
 * opaque: learned, never decoded); `-s <LocationID>` addresses exactly that device; a wrong id
 * answers "No found specific rockusb device!" (exit 255, nothing written); WITHOUT `-s` a command
 * with two devices present silently takes one of them. A board keeps its LocationID until it leaves
 * Loader mode; `-s <id> RD` brings exactly that board back on ADB in 2-3 s. One rootfs WL takes
 * ~27 s on one board.
 *
 * The owner's rules, hard: only complete writes (a WL is never killed, timed out or cancelled; a
 * cancel waits for the writes in progress and skips the boards not started); idblock / uboot / env /
 * meta only when named (the owner's `--partitions`; `--all` is refused for a fleet by the CLI); a
 * board that does not show up in Loader mode on its port in time is skipped and reported, never
 * guessed; a dry run touches no board beyond `adb devices`, the USB enumeration and `LD`; never
 * two upgrade_tool processes on one device; every per-board command carries `-s`.
 *
 * start() - every step asynchronous on the event loop, every callback carrying the run's id:
 *  1. Selecting. platformRefusal() first: non-empty -> finished(false, "port mapping not available
 *     on this platform") without any command. `adb devices` -> the boards of setBoards() in that
 *     order (or, with setAllBoards(), every entry in state "device" in adb's order, tagged A, B,
 *     ...): state "device" = a candidate; "unauthorized" / "offline" = Skipped "not on ADB: board X
 *     is 'unauthorized', not ready (unauthorized/offline: replug the cable)"; absent = Skipped "not
 *     on ADB: board X is not attached; attached: A, B" (AdbTool::selectDevice()'s texts). An
 *     `adb devices` that could not start or timed out selects nobody: finished(false, "adb could
 *     not be run: <error>" / "adb devices timed out") - SocFlasher's words; the CLI's exit 1, never
 *     the "not attached" skips and never exit 3. No candidate -> finished(false, "no selected board
 *     is on ADB (attached: <list or none>)") - the CLI's exit 3. Then the baseline: the USB
 *     enumeration (VID 2207) and one `LD`, logged as UsbProbe::describe() and "upgrade_tool LD: n
 *     devices"; a baseline `LD` that could not start or timed out ends the run before any reboot,
 *     finished(false, "upgrade_tool could not be run: <error>" / "upgrade_tool LD timed out")
 *     (SocFlasher's words; the CLI's exit 1 through the report's stop text). With setAllBoards()
 *     the entries `adb devices` listed (any state) must not outnumber the ADB gadgets
 *     (isAdbGadget()) the enumeration shows: more -> finished(false, "adb lists N devices but the
 *     USB bus shows M Rockchip ADB gadgets: a non-Rockchip ADB device is attached; name the boards
 *     with --serials") in a dry run and a real run alike - the check that keeps a phone on this PC
 *     (its serial matches no PnP instance, but the counts tell) from being told `reboot loader`;
 *     another Rockchip board passes it and is held back by step 2 b. A download-mode instance or an LD line
 *     that no selected board can account for (a Loader / MASKROM device that was there before the
 *     run: a board left in Loader mode by an earlier run, or someone else's) is FOREIGN. A dry run
 *     lists it ("foreign Loader device on port P, LocationID n: not part of the fleet; reset it with
 *     `upgrade_tool -s n RD` or replug it") and goes on; a real run REFUSES before any reboot -
 *     finished(false, "a rockusb device is already in Loader mode on port P (LocationID n) that no
 *     selected board accounts for; reset it with `upgrade_tool -s n RD` or replug it, then run
 *     again") - the owner's "never guess" in its strictest form: nothing is written next to a device
 *     the fleet cannot name (MASKROM: the same with "MASKROM mode"). A download-mode instance LD
 *     does not list (and the other way round) is refused with UsbProbe::driverHint(): the driver.
 *     An enumeration without any ADB gadget while adb lists candidates -> finished(false, "the USB
 *     enumeration shows no Rockchip ADB device although adb lists boards: port mapping impossible
 *     (driver?)"). Every candidate gets its remembered port (PortProvider) and the line "on ADB;
 *     expected port <key> (remembered)" / "on ADB; expected port unknown (first run)". The rows
 *     are checked once, before any reboot, with SocFlasher::writePartitions()'s rules (file present,
 *     fits its partition, 512-byte aligned; a failure ends the run with that wording). A dry run
 *     ends here: every candidate Ready, finished(allOk, "dry run: k of n boards ready[; m skipped]").
 *  2. Mapping, one board at a time in selection order - one reboot in flight, ever, and no
 *     upgrade_tool but `LD` runs during this phase:
 *     a. `adb -s X shell "cat /proc/version"` -> kernel ("an unknown kernel" when it fails; not a
 *        failure), the line "board running 5.10.209+, rebooting it into Loader mode".
 *     b. `adb -s X shell "cat /proc/cmdline"` -> layout, mtdPartsRead(X, layout) for the owner's
 *        memory. The layout gate: a layout that parses and differs from the plan's (layout()),
 *        whichever layout the plan uses, skips the board BEFORE its reboot unless "env" is among the
 *        rows (then the env written defines the next layout): Skipped "flash layout differs from the
 *        plan's (board: <x>; plan: <y>): not rebooted". Stricter than the single-board warning;
 *        inert on a bench whose boards run the image's own layout. With setAllBoards() a board
 *        whose /proc/cmdline carries no parsable mtdparts (an eMMC / SD board, another Rockchip
 *        product: its ADB gadget passes the count of step 1) is Skipped before its reboot too, "no
 *        flash layout in its /proc/cmdline: not a board this plan can place its rows on; name it
 *        with --serials to write it anyway" - so --all-boards reboots only boards that report a
 *        layout equal to the plan's (or any layout when env is written); --serials keeps the
 *        owner's explicit choice. adb that cannot read the board -> Skipped "adb could not read
 *        board X: <reason>" (no reboot).
 *     c. `before` := Snapshot::from(enumeration, LD). `adb -s X reboot loader` (its exit status
 *        ignored, as the single flow and usb_update.sh). X.stage = Mapping, its clock starts.
 *        THE ORDER OF THE TWO READS OF A SNAPSHOT IS PINNED: `LD` is started first and the
 *        enumeration is read when its result arrives, never before `LD` is started - after.bus is
 *        never older than after.rockusb. (`LD` takes ~1 s and a board enters Loader mode within
 *        ~1 s of `reboot loader`, i.e. right inside the first poll's `LD`: an enumeration taken
 *        before it would still show X's ADB instance next to X's new LocationID and read as a
 *        foreign device. An `LD` that missed the flip gives LoaderNotEnumerated / LdNotListing and
 *        polls on; an `LD` that caught it meets the Loader instance in the enumeration: Mapped.)
 *     d. Every pollIntervalMs up to loaderWaitSeconds: `after` := a fresh snapshot (`LD` first, the
 *        enumeration at its result), verdict := compare(before, after) (the pure table below).
 *        Mapped -> one `adb devices` more: X must
 *        be gone from it (the one cross-check the enumeration cannot make: the serial is what the
 *        fleet knows, the port is what it learns; X still listed = the Loader device that appeared
 *        belongs to another board -> the run stops, "board X is still on ADB although a Loader
 *        device appeared on port P: not this board"), then X.port, X.locationId, X.stage = Loader,
 *        portLearned(X, P), "in Loader mode on port P, LocationID 112 (1.2 s)" plus " (moved:
 *        expected Q)" when the memory differs. Maskrom -> X Failed "entered MASKROM instead of
 *        Loader mode: not flashed in a fleet; flash it alone with `su-fw flash-soc --serial X` only
 *        once `upgrade_tool LD` lists it as the ONLY device in download mode (MASKROM mode writes
 *        the whole image with UF: idblock, uboot, env and meta included)", the board left as it
 *        is, next board. Ambiguous / ForeignLoader / PortMismatch -> the run STOPS
 *        (see "A stop" below). Pending -> poll on; at the timeout the last Pending picture decides:
 *        StillOnAdb -> Skipped "reboot loader had no effect: board X is still on ADB after 30 s";
 *        LoaderNotEnumerated -> Skipped "board X left ADB (port P) but no Loader device appeared
 *        within 30 s; it may be rebooting: check it by hand" (a late arrival is caught by the next
 *        board's difference as Ambiguous, or by the reconciliation of step e: the run stops,
 *        reported, never guessed); LdNotListing -> STOP "a Loader device appeared on port P but
 *        upgrade_tool LD never listed it within 30 s: <UsbProbe::driverHint()>; board X is in
 *        Loader mode and must be power-cycled by hand" (a driver problem hits every board alike);
 *        GhostOnPort -> STOP "a Loader device on port P still lists an ADB device after 30 s:
 *        cannot tell whose it is"; LdOnly -> STOP "upgrade_tool LD lists LocationID n but no
 *        download-mode device appeared on the bus within 30 s: cannot tell whose it is; board X is
 *        still on ADB" (n unattributed, never reset).
 *     e. Next board. After the last, when at least one board was rebooted, the RECONCILIATION: one
 *        more snapshot (the same order), and every download-mode port and every LD id in it must
 *        belong to a mapped board, a MASKROM-failed board or the run's baseline of step 1. Anything
 *        else is a late arrival - a board skipped as LoaderNotEnumerated that entered Loader mode
 *        during the next board's steps a-b or after the last mapping, absorbed into a later
 *        `before` and so invisible to the differences - and it STOPS the run exactly as Ambiguous
 *        does: "a Loader device appeared late on port Q (LocationID n) and belongs to no mapped
 *        board", every mapped board reset, n in unattributedDevices() with the RD hint, the
 *        skipped board's detail extended "; it is in Loader mode now (port Q, LocationID n)". Then:
 *        no board mapped -> finished(false, summary()); else the run line "mapping done: k of n
 *        boards in Loader mode; writing starts (up to N at once)". The same snapshot closes every
 *        run that rebooted a board (after the last board ended and no WL / RD is in flight, before
 *        finished(); a drain included): every download-mode device that is no board of the run (by
 *        port or by LocationID: a board left in Loader mode, a MASKROM board, a failed RD) and not
 *        in the baseline goes to unattributedDevices() and is logged with the hint - that is what
 *        report().setUnattributed() carries for a real run, so the owner is never told "clean"
 *        while a device he was not told about sits in Loader mode for the next run to refuse on.
 *        The same snapshot corrects the run's own ended boards: a Failed / NotBack board (not a
 *        MASKROM one, not one already leftInLoader) whose port shows a download-mode instance or
 *        whose LocationID `LD` still lists in Loader mode (a re-enumeration gap seen by a re-check,
 *        a failed RD) gets leftInLoader, its detail extended "; it is in Loader mode now (port P,
 *        LocationID n)" and - when LD lists its id - the addressed retry lines (one WL per row
 *        not in written, then the RD), logged as a failed WL's are.
 *     A stop ("mapping stopped: <reason>; nothing was written"): nothing has been written; every
 *     mapped board gets `upgrade_tool -s <id> RD` ("reset back to ADB; nothing was written") and is
 *     Skipped "mapping stopped: <reason>"; the board being mapped is Failed with the reason; every
 *     later board is Skipped "mapping stopped: <reason>"; a LocationID that appeared but belongs
 *     to nobody the fleet can name is NEVER reset (an identity it did not learn): it is listed in
 *     unattributedDevices() and logged with the way out ("LocationID 154 on port Q is in Loader mode
 *     and belongs to no mapped board: reset it with `upgrade_tool -s 154 RD`"). finished(false, ...).
 *  3. Writing, in waves: up to maxParallel() boards at once (0 = all of them; a slot that frees
 *     starts the next mapped board - the scheduler runs after the mapping, after every WL / RD end
 *     and after every check). Each board writes its rows in row order, one `upgrade_tool -s <id> WL
 *     <sector> <count> <file>` at a time (RockchipTool::writeLbaOn()), in a working folder of its
 *     own, "<extract folder>/.fleet/<serial>" (created on demand; upgrade_tool writes a Log folder
 *     into its cwd; a folder that cannot be created fails the board before any write). The files
 *     are the plan's unpacked images, shared read-only by every board: EXF ran once, in the owner's
 *     plan, and never runs here. Before every WL and before the RD the board is re-checked:
 *     the enumeration (no device I/O) must still show a download-mode instance on its port and no
 *     ADB instance there; and - the LD gate - whenever no WL / RD of this run is in flight, one
 *     `LD` must still list its LocationID in Loader mode. The gate keeps a listing of its own: a
 *     single-flight `LD` answers every board that asks while it runs, and its result answers every
 *     board that asks within 500 ms after it arrived (counted from the result, not the start);
 *     the mapping's and the reconciliation's snapshot LDs never answer a re-check, so the first
 *     wave always runs exactly one `LD` after "mapping done" (the test counts it). `LD` opens every
 *     rockusb device, and an `LD` next to a running WL is UNMEASURED, so the two never overlap in
 *     either direction: no `LD` starts while a WL / RD of this run is in flight, and no WL / RD
 *     starts while the gate's `LD` is in flight - a check, a board's RD, a stop's and a drain's RDs
 *     all wait for its result (FakeUsbBus::ldDuringWrites() counts both directions). Mid-wave the
 *     brief's "LD before every WL" is therefore not met: a vanished id then fails the next WL as
 *     exit 255 with nothing written. setLdPolicy(LdPolicy::BeforeEveryWrite) runs `LD` before
 *     every write regardless, for the bench measurement and only after it proved harmless. The
 *     check's line: "checked: LD lists LocationID n in Loader mode; port P shows PID_110E" /
 *     "checked: port P shows PID_110E (LD not run: k writes in flight)". A failed check before a
 *     WL -> Failed "left Loader mode before <name> was written (port P shows <an ADB device /
 *     nothing / MASKROM>; LD <lists / does not list> LocationID n | LD not run: k writes in
 *     flight): nothing written for it" (written lists the rows already on it; no RD; leftInLoader
 *     false: not in Loader mode by the evidence), the other boards continue. A gate `LD` that could
 *     not start or timed out is no evidence of anything: Failed "upgrade_tool LD could not run
 *     before <name>: <error>; nothing written for it" (no RD, leftInLoader, the retry lines below),
 *     never the "left Loader mode" text. A failed check before the RD is another thing - every
 *     row IS on the board: no RD (not in Loader mode by the evidence), written stays complete, the
 *     board goes to step 4 as a reset one does and the ADB poll decides: back on ADB in time ->
 *     Done with the warning detail "partitions written; the board left Loader mode before its
 *     reset (port P showed <...>; LD <...> LocationID n) and came back on ADB by itself"; not back
 *     in time, or --no-wait -> NotBack "partitions written, but the board left Loader mode before
 *     its reset (port P shows <...>; LD <...> LocationID n): not reset by the fleet; check it by
 *     hand". A pre-RD gate `LD` that could not run -> Failed "partitions written, but upgrade_tool
 *     LD could not run before the reset: <error>; reset it by hand with `upgrade_tool -s n RD`"
 *     (leftInLoader, retry = {that RD}). A WL that is not clean (not started, exit code != 0, or
 *     RockchipTool::reportsWriteFailure()) -> Failed "writing <name> failed: <tail>", NO RD: the
 *     board stays in Loader mode (leftInLoader) so nothing else is written to it, and the board's
 *     lines name the ONLY safe retry - "left in Loader mode on port P, LocationID n: nothing else is
 *     written to it. Retry by hand, addressed: one `upgrade_tool -s n WL <sector> <count> <file>`
 *     per missing row, then `upgrade_tool -s n RD` (the lines follow); `su-fw flash-soc --serial X`
 *     only once `upgrade_tool LD` lists X as the ONLY device in download mode (the single-board
 *     flow writes without -s and takes any Loader device)" followed by one "  retry: <command
 *     line>" per entry of `retry` (the failed row first, the rows after it, then the RD). After a
 *     hub brown-out several boards sit in Loader mode at once and each gets its own addressed
 *     lines; `--serial` is never the hint while another device is in download mode (that is the
 *     owner's rule 3: an unaddressed WL with two devices present silently takes one of them).
 *     reportsNoSuchDevice() ("writing <name> failed: No found specific rockusb device! (LocationID
 *     n is gone)") has no valid id to put into a line: retry stays empty, leftInLoader = the
 *     enumeration still shows download mode on P, and the hint reads "port P shows <...>: find its
 *     LocationID with `upgrade_tool LD` before any retry, and address it with -s". partitionWritten()
 *     per row ("rootfs written: 3830272 bytes in 27.1 s"), boardProgress() every percent of the
 *     board's bytes (the owner prints every 10 %). A board whose rows are written is reset at once:
 *     `upgrade_tool -s <id> RD` ("partitions written; resetting the board"); an RD that is not clean
 *     (not started, timed out, or neither exit 0 nor RockchipTool::reportsResetOk()) -> Failed
 *     "partitions written, but upgrade_tool RD did not reset the board (<tail>); replug it".
 *     Per board at most one tool process ever; a WL is never cancelled and never timed out; an RD
 *     carries the LD timeout as in 0.6.0 (a timed-out RD is a failed reset, never a cut write);
 *     after 5 min without output (and after every further 5 min) the board's line says "upgrade_tool
 *     has printed nothing for n min; it is left running ..." as SocFlasher's.
 *  4. Returning: `adb -s X get-state` every pollIntervalMs up to adbReturnTimeoutSeconds per board
 *     (every returning board polled from one timer, at most one adb per board in flight; adb's
 *     server serializes them), then `adb -s X shell "cat /proc/version; uptime"` -> boardInfo and
 *     uptime, then `adb -s X shell "rfmcu status"` -> rfmcu (its exit status ignored; the owner's
 *     acceptance check, recorded, never judged), "back on ADB: 5.10.209+, up 0 min; rfmcu: pid=..
 *     uart=owned mcu=alive" -> Done, "done in 38.2 s (mapping 1.9 s, writing 29.4 s)". ADB not back
 *     in time -> NotBack with detail "partitions written and the board reset, but ADB did not come
 *     back within 60 s; check the serial console" (adbBack false). setWaitForAdb(false) ends a board
 *     Done right after its RD with detail "ADB not waited for (--no-wait)".
 *  5. finished(allOk, summary()): allOk = at least one board selected and every selected board
 *     Done (Ready in a dry run) - never vacuously true for an empty selection (FleetReport::allOk()
 *     says the same).
 *
 * compare(before, after), the pure part of the mapping (table-driven unit tests, no event loop),
 * with vanished = before.adbPorts not in after.adbPorts, appeared = after.downloadPorts not in
 * before.downloadPorts, newIds = after's Loader / Maskrom ids not in before's - everything that was
 * there at the baseline (foreign devices included) is excluded by the differences themselves:
 *   vanished = appeared = newIds = {}                        -> Pending / StillOnAdb
 *   |vanished| > 1 or |appeared| > 1 or |newIds| > 1        -> Ambiguous "two boards changed at once:
 *       ADB instances gone on ports ...; Loader devices appeared on ports ...; new LocationIDs ..."
 *       (newIds listed, for the unattributed report)
 *   vanished = {}, appeared = {P}, P in after.adbPorts        -> Pending / GhostOnPort "a Loader device
 *       on port P which still lists an ADB device" (a stale enumeration, polled through)
 *   vanished = {}, appeared = {Q}, Q not in after.adbPorts    -> ForeignLoader "a device entered download
 *       mode on port Q (LocationID n / no LocationID yet) while the board is still on ADB: not this
 *       board" (a PORT appeared: the enumeration is the younger read, so this is never skew)
 *   vanished = {}, appeared = {}, newIds = {n}                -> Pending / LdOnly "upgrade_tool LD lists a
 *       new LocationID n but the bus shows no new download-mode device yet" (polled through: on Windows
 *       an LD line without a PnP instance can only be the listing running ahead of the enumeration)
 *   vanished = {P}, appeared = {Q}, P != Q                   -> PortMismatch "the board left port P but a
 *       Loader device appeared on port Q"
 *   vanished = {P}, appeared = {}                            -> Pending / LoaderNotEnumerated "the board left
 *       ADB (port P), waiting for its Loader device" (whether or not LD lists a new id already)
 *   vanished = {P}, appeared = {P}, newIds = {}              -> Pending / LdNotListing "Loader device on port
 *       P, waiting for upgrade_tool LD to list it"
 *   vanished = {P}, appeared = {P}, newIds = {n} in loaderIds -> Mapped (port P, locationId n)
 *   vanished = {P}, appeared = {P}, newIds = {n} in maskromIds -> Maskrom (port P, locationId n)
 *
 * cancel(): a drain, accepted at any moment (FirmwareJob::isCancellable() stays true for a fleet)
 * and never a cut: Selecting -> the run ends after the command in flight, nothing touched. Mapping
 * -> the board already told `reboot loader` has its wait finished first (mapped, skipped or a
 * stop; a Ctrl+C must never leave a board in Loader mode with an id nobody recorded), no further
 * board is rebooted, then every mapped board gets `-s <id> RD` and is Skipped "cancelled before its
 * write started; reset back to ADB". Writing -> the scheduler starts nothing more; mapped boards not
 * started get `-s <id> RD` and that Skipped text (those RDs wait for a gate `LD` in flight exactly
 * as a re-check does: a cancel at a wave boundary never puts an RD next to the `LD` that opens every
 * rockusb device); a board that has started finishes EVERY one of its
 * rows and its RD (board granularity: a board with one new and one old partition is the state the
 * owner must never get; with --partitions rootfs the two readings coincide); the ADB waits end at
 * once (NotBack, "the ADB wait was cancelled; the partitions were written"). The run ends when the
 * last process of the run has ended: finished(false, summary()). A second cancel() only logs "still
 * waiting for %1 write(s) to finish; the writes are never interrupted". isDraining() tells.
 *
 * Tools: one RockchipTool for `LD` and the addressed WL / RD (the address is in the call, never on
 * the object), adb per board with `-s <serial>` on every command and one plain `adb devices`.
 * Every external command line and its result go to boardEvent() (the board's: "$ upgrade_tool -s
 * 113 WL ...", "  exit code 0") or logLine() (the run's); upgrade_tool's raw lines come as
 * toolOutput() per board. Messages are QCoreApplication::translate("FleetFlasher", ...) and are
 * mirrored to lcFirmware with the board's label. Headless by construction: no question, nothing
 * waits for an owner.
 */
class FleetFlasher : public QObject
{
    Q_OBJECT
public:
    enum class Phase { Idle, Selecting, Mapping, Writing, Returning, Done, Failed, Cancelled };
    Q_ENUM(Phase)

    /// When `upgrade_tool LD` re-checks a board before a WL / the RD: Gate (default) = only while
    /// no WL / RD of this run is in flight (never next to a write; one shared listing per 500 ms);
    /// BeforeEveryWrite = before every write regardless (the brief's wording; unmeasured next to a
    /// running WL - for the bench measurement, and the default only once that proved harmless).
    enum class LdPolicy { Gate, BeforeEveryWrite };
    Q_ENUM(LdPolicy)

    /// The port remembered for a serial ("" = none): AppSettings::firmwareBoardPort() by the owner.
    using PortProvider = std::function<QString(const QString& serial)>;

    /// The bus and the tool at one moment, as compare() reads them. Built by Snapshot::from() from
    /// the enumeration and parseDeviceList(`LD`): lists sorted and unique. The two reads are
    /// ordered: `LD` is started first and the enumeration is read when its result arrives, never
    /// before `LD` is started, so `bus` is never older than `rockusb` (the class comment, step 2 c:
    /// an older enumeration shows a board's ADB instance next to its own new LocationID).
    struct Snapshot
    {
        QStringList adbPorts;                   ///< portKey() of every ADB gadget (isAdbGadget())
        QMap<QString, quint16> downloadPorts;   ///< portKey() -> pid of every other VID 2207 instance (isDownloadMode())
        QStringList loaderIds;                  ///< LocationIDs `LD` lists with Mode=Loader
        QStringList maskromIds;                 ///< LocationIDs `LD` lists with Mode=Maskrom
        static Snapshot from(const QList<UsbDeviceInfo>& bus, const QList<RockusbDevice>& rockusb);
        bool operator==(const Snapshot& other) const;
    };

    /// What compare() decided (the class comment's table).
    struct Verdict
    {
        enum class Kind { Pending, Mapped, Maskrom, Ambiguous, ForeignLoader, PortMismatch };
        /// What a Pending verdict waits for; decides the text and the outcome at the timeout.
        /// LdOnly: `LD` lists a new id but the bus shows no new download-mode device yet.
        enum class Waiting { None, StillOnAdb, LoaderNotEnumerated, LdNotListing, GhostOnPort, LdOnly };
        Kind kind = Kind::Pending;
        Waiting waiting = Waiting::None;
        QString port;            ///< Mapped / Maskrom / Pending (when a port is known): the board's port
        QString locationId;      ///< Mapped / Maskrom: its LocationID
        QStringList newIds;      ///< every LocationID that appeared (Ambiguous / ForeignLoader / PortMismatch: the unattributed ones)
        QString detail;          ///< the reason, or what is still missing while Pending (without the board's label)
        bool operator==(const Verdict& other) const;
    };

    /// A rockusb device in download mode that no board of the run accounts for: foreign at the
    /// baseline (foreignDevices()), or appeared during a stop, arrived late and was caught by the
    /// reconciliation, or sits on the bus when the run ends (unattributedDevices()). Never
    /// addressed by the fleet.
    struct UnattributedDevice
    {
        QString locationId;      ///< "154"
        QString port;            ///< the port key when the enumeration shows one; "" when only LD lists it
        QString mode;            ///< "Loader" / "Maskrom"
        QString hint() const;    ///< "upgrade_tool -s 154 RD" (also for MASKROM: an RD there is harmless)
        bool operator==(const UnattributedDevice& other) const;
    };

    explicit FleetFlasher(ToolRunner* runner, QObject* parent = nullptr);
    ~FleetFlasher() override;

    // ---- Inputs ----------------------------------------------------------------------------
    void setToolPaths(const QString& upgradeTool, const QString& adb);
    QString upgradeToolPath() const;
    QString adbPath() const;
    RockchipTool* upgradeTool() const;              ///< the run's one RockchipTool (LD, writeLbaOn, resetDeviceOn)
    AdbTool* adb() const;                           ///< the plain one (`adb devices`); per-board commands carry -s
    void setImagePath(const QString& path);         ///< for the log lines and the per-board working folders
    QString imagePath() const;
    /// The rows to write on every board: the selected, writable rows of the owner's plan
    /// (FirmwareJob::selectedPartitions() on its partitions()); their files are the unpacked
    /// images, shared by every board (EXF runs once, in the plan, never here).
    void setPartitions(const QList<PartitionImage>& rows);
    QList<PartitionImage> partitions() const;
    /// The layout the rows were placed in (normalized mtdparts) and whether it is the image's own
    /// (env.img) rather than a board's (--layout / remembered). Step 2's layout gate compares every
    /// board's own layout with it.
    void setLayout(const QString& mtdparts, bool fromImage);
    QString layout() const;
    bool layoutFromImage() const;
    /// (tag, serial) in selection order; the owner checks that tags and serials are unique (the
    /// CLI assigns the default tags around the explicit ones: FirmwareCli.h).
    void setBoards(const QList<QPair<QString, QString>>& taggedSerials);
    QList<QPair<QString, QString>> taggedSerials() const;
    /// Every board `adb devices` lists in state "device", tagged A, B, ... in adb's order; the boards
    /// of setBoards() are ignored then. Refused at the baseline when adb lists more entries than the
    /// bus shows Rockchip ADB gadgets (a non-Rockchip ADB device: the class comment, step 1).
    void setAllBoards(bool on);
    bool allBoards() const;
    void setMaxParallel(int count);                 ///< boards written at once; 0 (default) = all of them
    int maxParallel() const;
    void setDryRun(bool on);                        ///< selection, port memory and plan only: no reboot, no write
    bool dryRun() const;
    void setLoaderWaitSeconds(int seconds);         ///< per board, default 30
    int loaderWaitSeconds() const;
    void setAdbReturnTimeoutSeconds(int seconds);   ///< per board, default 60
    int adbReturnTimeoutSeconds() const;
    void setPollIntervalMs(int ms);                 ///< default 1000 (the tests: 10)
    int pollIntervalMs() const;
    void setWaitForAdb(bool on);                    ///< default true
    bool waitForAdb() const;
    void setLdPolicy(LdPolicy policy);              ///< default LdPolicy::Gate
    LdPolicy ldPolicy() const;
    /// The USB enumeration behind the mapping: UsbProbe::presentDevices by default (and only when
    /// UsbProbe::hasPlatformProbe()); the tests inject FakeUsbBus::enumerate. A null enumerator =
    /// platformRefusal() for this instance.
    void setUsbEnumerator(UsbProbe::Enumerator enumerator);
    void setPortProvider(PortProvider provider);

    // ---- State ---------------------------------------------------------------------------------
    bool isRunning() const;
    bool isDraining() const;                        ///< cancel() was accepted: writes in progress finish, nothing new starts
    Phase phase() const;
    QList<FleetBoard> boards() const;               ///< every selected board, selection order, as far as the run got
    QList<UnattributedDevice> foreignDevices() const;        ///< download-mode devices present at the baseline (a dry run lists them; a real run refused)
    QList<UnattributedDevice> unattributedDevices() const;   ///< download-mode devices that belong to nobody known: appeared during a stop, caught by the reconciliation, or left on the bus at the end of the run
    QString stopReason() const;                     ///< why the mapping stopped / the run was refused; "" = it was not
    double elapsedSeconds() const;                  ///< from start() to the end (or now)
    double mappingSeconds() const;                  ///< from the first `reboot loader` to "mapping done" (0 before)
    double writingSeconds() const;                  ///< from the first pre-write check to the last RD's end (0 before)
    /// The run as FleetReport::summary() words it: "7 of 7 boards flashed in 48.3 s (mapping 9.8 s,
    /// writing 31.2 s)" / "5 of 7 boards flashed in 61.0 s (...); 1 not back on ADB; 1 skipped" /
    /// "dry run: 6 of 7 boards ready; 1 skipped" / "mapping stopped: <reason>; nothing was written"
    /// / "no selected board is on ADB (attached: none)". finished()'s message.
    QString summary() const;
    /// The run's record (FleetReport) as far as it got: boards(), the timing, dryRun(), the stop,
    /// the unattributed devices, imagePath(), the row names, maxParallel().
    FleetReport report() const;

    /// "" when this instance can map a USB port to a board (a platform probe or an injected
    /// enumerator), else "port mapping not available on this platform".
    QString platformRefusal() const;
    /// idblock, uboot, env, meta: never written unless named in --partitions (the owner's rule; the
    /// CLI logs "Warning: idblock is a bootloader partition; written because --partitions names it").
    static QStringList bootloaderPartitions();
    /// The pure difference method (the class comment's table). Pending verdicts carry what they
    /// wait for; Ambiguous / ForeignLoader / PortMismatch carry the new ids for the report.
    static Verdict compare(const Snapshot& before, const Snapshot& after);
    /// "A", "B", ... "Z", "AA", "AB", ... for index 0, 1, ... 25, 26, 27, ...: the default tags.
    static QString tagFor(int index);

public slots:
    /// False while running, or without boards / rows / tool paths / a layout (the reason comes as
    /// finished(false, reason) from the event loop, never from inside start()).
    bool start();
    /// The drain of the class comment: accepted at any moment, never a cut. A second call only
    /// logs "still waiting for %1 write(s) to finish; the writes are never interrupted".
    void cancel();

signals:
    void phaseChanged(FleetFlasher::Phase phase);
    void boardChanged(const FleetBoard& board);                       ///< stage, result, port or locationId changed
    void boardEvent(const FleetBoard& board, const QString& text);    ///< one line per board per event (the owner prefixes label())
    void boardProgress(const FleetBoard& board, const QString& stage, int percent);   ///< "Writing rootfs (1 of 1)", 0..100 over the board's bytes
    void partitionWritten(const FleetBoard& board, const QString& name, qint64 bytes, qint64 milliseconds);
    void boardFinished(const FleetBoard& board);                      ///< result Done / NotBack / Failed / Skipped (Ready in a dry run)
    void logLine(const QString& line);                                ///< the run's own lines (no board)
    void toolOutput(const FleetBoard& board, const QString& line);    ///< upgrade_tool's raw lines, per board
    void portLearned(const QString& serial, const QString& port);     ///< remember it (AppSettings::setFirmwareBoardPort)
    void mtdPartsRead(const QString& serial, const QString& mtdparts);
    void finished(bool allOk, const QString& message);               ///< message = summary()

private:
    struct Private;
    std::unique_ptr<Private> d;
};
