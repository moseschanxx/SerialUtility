#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <functional>
#include <memory>
#include <optional>

class FirmwareJob;
class QIODevice;
class SshProfileStore;
class ToolRunner;

/**
 * The logic of the `su-fw` console tool (v0.6.0; docs/firmware-update/CLI_PLAN.md sections 3 and
 * 4): the Firmware tab's operations for scripts and AI agents, without a window. UI-free (su_core);
 * src/cli/main.cpp is only the process around it (QCoreApplication, stdout / stderr / stdin, the
 * console's echo, Ctrl+C), so the tests drive the very same code in-process with FakeToolRunner,
 * TestSshServer and FakeMcuServer.
 *
 * run(arguments, out, err): `arguments` is the command line after the program name
 * ("flash-soc --all --json"). One QCommandLineParser per command parses it; the command
 * configures a fresh FirmwareJob (the chain the tab drives: same steps, tools, rules and
 * messages), runs one operation on the event loop until operationFinished() and returns the exit
 * code. Commands: status, partitions, pull, flash-soc, mcu-info, mcu-update, mcu-set-role,
 * pull-flash; helpText() documents every option for an agent to read (`su-fw --help`,
 * `su-fw <command> --help`). The settings are the app's: the SSH profiles and SecretStore, the
 * Preferences (tool paths, SSH defaults, Loader / ADB waits, cache folder, git describe, extra
 * files) and the flash layouts remembered per board (a layout read from a board is stored there
 * too, as the tab does); nothing else is written back.
 *
 * Output (all on `out`; only usage errors go to `err`): every log line of the job, the tab's
 * wording, as "HH:MM:SS text"; upgrade_tool's raw lines except its "(nn%)" progress, which comes
 * as the job's progress() every 10 %; the command's own lines (the board, the partition table,
 * the image summary); last a result line "su-fw: exit <n> (<meaning>): <message>". --json prints
 * one JSON object per line instead: {"time": ISO 8601 with offset, "step": the job's step or the
 * command, "event": "log" | "progress" | "step" | "done", "text", "percent" (progress with a
 * percentage), "ok" (step, done), "exit" (done)}; the "done" object is the last line and carries
 * the command's result fields ("board", "mcu", "layout", "partitions", "image", "boardInfo").
 * --quiet prints only the result line; --log FILE appends every line to FILE as well (also with
 * --quiet; opened only once the whole command line is valid). --password-env VAR is removed from
 * the environment as soon as it is read, so no tool inherits it.
 *
 * Exit codes (ExitCode): 0 done; 1 the operation failed (the last log line says why); 2 usage;
 * 3 no board, or the board in a mode the command cannot use; 4 SSH: unreachable, host key
 * refused, authentication failed; 5 --timeout or interrupt() (Ctrl+C).
 *
 * Decisions: an unknown host key is accepted (and remembered in known_hosts) only with
 * --accept-host-key, a changed one never without a terminal to ask; a password / passphrase
 * prompt is answered once from --password-env VAR or --password-stdin, else SshConnection's saved
 * SecretStore secret is all there is. On an interactive terminal without --yes su-fw asks instead
 * (questions on `err`, the password read without echo). Secrets are never on the command line and
 * never echoed: not the password, not the name of the variable holding it, not a stray argument.
 * Download mode: a board on ADB is rebooted into Loader mode by the flasher itself (`adb reboot
 * loader`); without a board the MASKROM instructions are logged and the flasher waits (the job's
 * headless default).
 *
 * interrupt() (Ctrl+C) and --timeout cancel the running operation through FirmwareJob::cancel();
 * the job refuses that while upgrade_tool writes the flash (logged), and su-fw then cancels at
 * the next point where it can (cancellableChanged(true), acted on from the event loop) - the write
 * itself is never cut; cancelled in the ADB wait after it, the result says "interrupted after the
 * image was written; the ADB wait was cancelled". A wait outside an operation (the status query,
 * the partition plan) ends at once. An interrupt() while the command line is still read (the
 * password from stdin) is remembered: nothing starts. All exit with 5. Another firmware operation
 * on the PC (the tab, another su-fw: FirmwareJob's lock) makes an operation fail at once and the
 * status query stop ("another firmware operation is running (...)", exit 1).
 *
 * flash-soc, a fleet (v0.7.0; FirmwareJob::flashFleet(), FleetFlasher, FleetReport; docs/DESIGN.md
 * 4.13): `--serials S1,S2,...` (repeatable, entries add up; `TAG=SERIAL` names a board for the
 * lines and the table, a tag is 1-8 of [A-Za-z0-9_-]; the untagged entries get the default tags
 * A, B, ... Z, AA, ... in the order given, SKIPPING every tag given explicitly anywhere in the
 * list - `--serials B=62ec...,5a33...` is B and A, `--serials A=..,..,C=..` gives the middle one B -
 * and the "used twice" check runs on the final list, so it fires only for two explicit equal tags)
 * or `--all-boards` (every board `adb devices` lists as "device", in adb's order) make the command
 * a fleet run; `--serial` without them is the 0.6.0 single-board path, byte for byte. A fleet
 * requires `--partitions NAMES` (`--all` is refused: every row would include idblock, uboot, env
 * and meta, which are written only when named; a named bootloader partition is logged "Warning:
 * idblock is a bootloader partition; written because --partitions names it"), takes
 * `--max-parallel N` (1..64; default: every board at once) and `--dry-run` (the selection, the
 * remembered ports and the plan; no reboot, no write), and `--layout` / `--no-wait` as the single
 * board. The plan is made first as cmdFlashSoc() does; the name check differs from the single
 * board's in one point: a name the image does not have is a usage error (2), a row that cannot be
 * written (no file, does not fit, not in the layout) is a usage error (2) only when the layout is
 * known, and an UNKNOWN layout is left to the job's step - "flash layout unknown: pass --layout, or
 * run su-fw status once with a board running; the Node image carries its own", exit 1 - because
 * FirmwareJob::partitionProblem()'s "connect the board running once" is the single board's advice,
 * not the fleet's. On a terminal without `--yes` a real fleet run asks once, on stderr, after the
 * plan and BEFORE the operation (nothing has talked to a board yet, so the question can only name
 * what the command line gave it): with `--serials` "Write rootfs on 7 boards (A, B, C, D, E, F, G)?
 * [y/N] " (the names joined ", "; N and the tags from the list), with `--all-boards` "Write rootfs
 * on every board adb lists (--all-boards)? [y/N] " - no `adb devices` runs outside the operation
 * (it would talk to the boards outside the firmware lock). Anything but y / yes: exit 5 "cancelled
 * before anything ran"; a non-terminal stdin asks nothing. The dry run's banner is the operation's
 * own "--- Flash fleet ---" (no " (dry run)" suffix: beginOperation() is untouched), followed by
 * the flasher's "Dry run: the selection, the remembered ports and the plan; no reboot, no write".
 * Text: one line per board per event, "HH:MM:SS [B 62ecfc6ddeb3f579] in Loader mode on port
 * 6&38635FDB&0&2, LocationID 112 (1.2 s)", the run's own lines, progress every 10 % per board, and
 * last the table of FleetReport::tableLines() before the result line. --json: a board event carries
 * "board" (the serial), "tag" and "stage" (the "in Loader mode" line also "port" and "loader"); a
 * board's end is an "event":"step" object with those fields plus "result", "ok", "seconds",
 * "mappingSeconds", "writingSeconds", "written", "retry", "adbBack"; the operation's own "step"
 * object comes without a board field, as today; the "done" object carries FleetReport::json()
 * ("fleet":true, "dryRun", "image", "partitions", "maxParallel", "seconds", "mappingSeconds",
 * "writingSeconds", "stop", "unattributed", "summary", "boards"). Exit codes of a fleet (the
 * mapping is `opOk ? 0 : aborted() ? 5 : report.exitCode()`, nothing else: FleetReport::exitCode()
 * knows an empty report): 0 every selected board flashed, reset AND back on ADB (with --no-wait:
 * reset); 1 a board not back on ADB in time, failed (no Loader mode on its port in time, left
 * Loader mode before a write, a failing WL - the board is then left in Loader mode and its lines
 * name the retry: one addressed `upgrade_tool -s <LocationID> WL ...` per missing row, then
 * `upgrade_tool -s <LocationID> RD`; `su-fw flash-soc --serial X` only once `upgrade_tool LD` lists
 * X as the ONLY device in download mode, since the single-board flow writes without -s and takes
 * any Loader device (v0.8.0: enforced - the single-board flow refuses, exit 1, with "several
 * rockusb devices are in download mode (LocationIDs ...): the single-board flow writes unaddressed
 * and cannot tell them apart; ..." before anything runs on the board) - or RD, MASKROM instead of
 * Loader: such a board is flashed alone with
 * `su-fw flash-soc --serial X` only once `upgrade_tool LD` lists it as the ONLY device in download
 * mode, and MASKROM mode writes the whole image with UF, idblock, uboot, env and meta included),
 * skipped (not on ADB, a differing layout, with --all-boards no layout in /proc/cmdline, the
 * mapping stopped), the mapping stopped (two boards changed at once, a foreign device, a late
 * arrival) or the run could not start (another firmware operation, port mapping not available on
 * this platform, flash layout unknown, adb could not be run, upgrade_tool LD could not be run,
 * `--all-boards` with more adb entries than Rockchip ADB gadgets on the bus - a phone: "name the
 * boards with --serials" - or a Loader / MASKROM device nobody accounts for on the bus); 2 usage; 3 no selected board on
 * ADB; 5 --timeout or Ctrl+C - boards whose write had not started are skipped and reset, writes in
 * progress are completed whole (every row and the reset of that board), the table says which; a
 * second Ctrl+C only logs how many writes are still being waited for. The learned port of every
 * mapped board is stored per serial (AppSettings::firmwareBoardPort()) so the next dry run prints
 * it as the expected port and the next real run reports a board that moved; the memory never
 * addresses a board.
 *
 * Board profiles (v0.8.0, profiles; BoardProfile.h, docs/DESIGN.md 4.14): every command that meets
 * a board or an image takes `--profile ID` (status, partitions, flash-soc, pull-flash, mcu-info,
 * mcu-update, mcu-set-role; ID from BoardProfiles::ids() - node, stacked - anything else is the
 * usage error "--profile: unknown profile \"x\"; known: node, stacked"), which becomes
 * FirmwareJob::setProfileOverride(); su-fw never reads the tab's Board profile setting, an agent's
 * run is steered by its command line alone. status prints, after "Board: ...", the job's
 * boardProfile() line ("profile: Node (RV1106BG2) - from the board's flash layout (7 partitions:
 * ...)" / "profile: Stacked (RV1106G2) - from the ADB gadget's product string
 * rv1106g-stacked-gc2083" - the job's product read, logged as a command line before it, runs
 * only for a board whose cmdline layout matched no profile - / "profile: unknown (...)"; a
 * Maskrom board is unknown unless --profile says otherwise; the JSON `board` object gains
 * "profile": {"id", "name", "source", "evidence"}); partitions and flash-soc print the job's image
 * lines ("image: update.img,
 * chip tag 110E, built 2026-10-13 21:45:50", "image profile: ...") and the done object gains
 * "imageHeader" {"chipTag", "version", "buildTime"} and "imageProfile" {as above}. The refusals are
 * the operation's step failure, exit 1, with the ProfileRules text: flash-soc and pull-flash take
 * `--force-layout` (rule 2) and `--force-loader-partitions` (rule 3), mcu-update, mcu-set-role and
 * pull-flash `--confirm-soc-power` (rule 4) - without them the job's headless defaults refuse (no
 * handler is installed: su-fw asks no profile question even on a terminal; the flags are the
 * answer). `flash-soc --help` carries a "Board profiles:" paragraph with one command line per
 * profile, four-blank indented and NOT under "Examples:" (the help test's "exactly two examples"
 * rule holds): the Node's daily `--partitions rootfs`, the stacked board's `--partitions boot`;
 * every help page that takes --profile explains it in one option line. A fleet passes --profile
 * to the job (every board and the plan).
 *
 * Verify after write (v0.8.0, verify-rl): `--verify` on flash-soc (single board and fleet) and
 * pull-flash -> FirmwareJob::setVerifyAfterWrite(); the per-row lines come through the log; the
 * done object gains "verify" (true when asked) and, for the single board, "verified": [{"name",
 * "ok", "bytes", "mismatch", "seconds"}] (the fleet's are in boards[]); a mismatch is exit 1 with
 * the flasher's text; in MASKROM mode the flasher's "not available" line and no field but "verify".
 *
 * Inventory (v0.8.0, inventory; Inventory.h, InventoryRecorder.h, docs/DESIGN.md 4.17):
 * `su-fw inventory [--json] [--serial S]` prints the inventory file - no job operation, no tool,
 * no board: the text table is Inventory::tableLines(), the done object carries Inventory::json();
 * `--serial S` prints that record alone (exit 3 "no record for S" when there is none); an
 * unreadable file is exit 1 with Inventory::load()'s error. Every command's job carries an
 * InventoryRecorder (one line in createJob(); its logLine() goes to the log), so status, flash-soc,
 * the MCU commands, pull-flash and a fleet record their boards. flash-soc takes `--expect-image
 * MD5` (32 hex digits; only with --serial or --serials: "--expect-image needs --serial or
 * --serials: the inventory is checked per named board before anything runs" with --all-boards or
 * no selector) and refuses, before the plan and before any tool, a named board whose record says
 * the image IS on it: lastImage.md5 equals it (case-insensitive) AND every partition the command
 * names (`--partitions NAMES`; with `--all`, every row of the image: the extracted package-file's
 * names, which the plan knows - so with `--all` the check runs right after the plan, still before
 * any board is touched) is in the record's lastImage.partitions (the rows written with that md5,
 * Inventory.h): "board S already runs the expected image (md5 <hex>, boot, rootfs written
 * 2026-10-14 12:54); pass --force to write it again" (the record's partitions joined ", "; exit 1,
 * the result line; a fleet names every such board and refuses as a whole). A record with the md5
 * but without every named row - the G2 bring-up ladder (userdata first, then boot with the same
 * build, FACTS D3), a daily `--partitions rootfs` followed by `--partitions boot,rootfs` - is NOT
 * refused: the log says "board S runs the expected image's userdata (md5 <hex>, written <time>);
 * boot is not recorded as written: writing" and the run goes on. `--force` lifts the refusal
 * (`--force` without --expect-image is the usage error "--force: only with --expect-image").
 * flash-soc also logs "image md5: <hex>" after the image lines (the done object: "imageMd5"), so
 * an agent can pass it next time. commands() gains "inventory" (helpText("inventory") documents it
 * with two examples).
 */
class FirmwareCli : public QObject
{
    Q_OBJECT
public:
    enum ExitCode : int {
        ExitOk = 0,
        ExitFailed = 1,      ///< the operation failed; the last log line says why
        ExitUsage = 2,       ///< unknown command / option, a missing or invalid value, an unusable file
        ExitNoBoard = 3,     ///< no board, or the board in a mode the command cannot use
        ExitSsh = 4,         ///< the build machine: unreachable, host key refused, authentication failed
        ExitCancelled = 5    ///< --timeout passed or interrupt() (Ctrl+C)
    };

    /// Reads one line the user typed (stdin): `secret` = without echo on a terminal. std::nullopt =
    /// end of input. Questions are written to `err` before it is called.
    using LineReader = std::function<std::optional<QString>(bool secret)>;

    /// `runner` null = real processes (the job's own ProcessToolRunner); the tests pass a fake.
    explicit FirmwareCli(ToolRunner* runner = nullptr, QObject* parent = nullptr);
    ~FirmwareCli() override;

    /// Runs one command line (without the program name) to its end on the event loop; returns the
    /// exit code. Needs a QCoreApplication. Not re-entrant (a nested call returns ExitUsage).
    int run(const QStringList& arguments, QIODevice* out, QIODevice* err);
    bool isRunning() const;

    // ---- What main.cpp (and the tests) hand in ----------------------------------------------------
    /// stdin: --password-stdin reads its first line; `interactive` (a terminal) allows questions
    /// unless --yes. Default: no input, not interactive.
    void setInput(LineReader reader, bool interactive);
    /// Not owned. Default: a store of the job's own, loaded from SshProfileStore::defaultFilePath()
    /// at every run (and never saved: su-fw does not change the app's profiles).
    void setProfileStore(SshProfileStore* store);
    /// Fixed tool paths instead of --tools / ToolLocator (the tests' "upgrade_tool" / "adb").
    void setToolPaths(const QString& upgradeTool, const QString& adb);
    /// Called with the configured job right before its operation starts (the tests shorten polls
    /// and waits here).
    void setJobSetup(std::function<void(FirmwareJob& job)> setup);

    // ---- The command line ---------------------------------------------------------------------------
    static QStringList commands();                            ///< "status", "partitions", ... "pull-flash"; (v0.8.0) "inventory" last
    /// Every option `command` accepts, without the dashes (the common ones included).
    static QStringList commandOptions(const QString& command);
    /// `su-fw --help` (empty `command`) or `su-fw <command> --help`.
    static QString helpText(const QString& command = QString());
    static QString versionText();                             ///< "su-fw 0.6.0 (BuildAI Serial Utility, <git hash>)"

public slots:
    /// Ctrl+C / SIGINT: cancels the running operation (see the class comment); remembered while the
    /// command line is read; deferred to the event loop while a question waits for stdin; a no-op
    /// while idle.
    void interrupt();

private:
    struct Private;
    std::unique_ptr<Private> d;
};
