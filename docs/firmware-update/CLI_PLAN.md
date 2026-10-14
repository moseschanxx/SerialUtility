# Command-line interface for the firmware flows (next after 0.5.1)

**Implemented in 0.6.0**: `su-fw` (`src/cli/main.cpp`, `src/firmware/FirmwareCli.{h,cpp}`, tests
`tests/tst_firmwarecli.cpp`; README "Command line (`su-fw`)", DESIGN 4.12). The three questions
below were decided: the name is `su-fw` (the GUI does not take the subcommands), `--json` is in,
and `reboot loader` goes out over ADB without a question. Differences from this plan: `pull-flash`
refuses `--partitions` (the chain writes every partition, like the tab's Pull & Flash; pull, then
`flash-soc --partitions`), and the `tst_firmwarecli` cases run `FirmwareCli` in-process with one
case starting the real executable.

Moses, 2026-10-13: "the software eventually user will use this to manually update the firmware to our
device, and the AI agent should also be able to invoke some command, then update the firmware in the
background, in a command shell." Same flows as the Firmware tab (pull, flash the SoC with a partition
selection, update the MCU or set its role, Pull & Flash), log on stdout, exit codes, same code paths.

## Shape

1. **One console executable, `su-fw`** (`su-fw.exe`, installed next to `BuildAI-SerialUtility.exe`, same
   `tools/` folder). The GUI executable is a Windows-subsystem program and cannot write to the shell that
   started it, so the command line gets its own small `QCoreApplication` program that links `su_core` only
   (no widgets, no dialogs). Linux gets the same binary name in the tarball.
2. **The chain moves out of the tab** into `src/firmware/FirmwareJob.{h,cpp}` (`su_core`): one object that
   runs `pull`, `flash`, `mcu-update`, `set-role`, `info` and `pull-flash` exactly as `FirmwareTab::Private`
   does today (pull → Flash SoC → wait for ADB → read the MCU version → Update MCU when the package differs;
   stop at the first failure). Inputs: source target + remote folder + cache directory, image path, the
   partition selection, the package, the role, the board serial, the tool paths. Outputs: `logLine()`,
   `progress(stage, percent)`, `stepFinished(step, ok, message)`, `finished(op, ok, message)`. Decisions that
   the tab answers with dialogs become callbacks: host key (accept / remember / refuse), authentication
   prompt (password / passphrase text), download mode (send `reboot loader` through an open session, or
   fail with the MASKROM instructions). The tab keeps its widgets, dialogs and tests and drives a
   `FirmwareJob`; its behaviour does not change.
3. **Commands** (`su-fw <command> [options]`, `su-fw --help` is written for an agent to read):
   - `status [--serial S] [--mcu]`: the device line (ADB / Loader / MASKROM / no driver), with `--mcu` the
     running CC1310 version and role.
   - `partitions --image update.img [--layout <mtdparts> | --serial S]`: the table the tab shows (name,
     file, offset, partition size, image size, fits) so an agent can choose before it flashes.
   - `pull --target user@host[:port] [--remote-folder F] [--cache DIR] [--identity FILE]
     [--password-env VAR | --password-stdin] [--accept-host-key]`: the ImagePuller (skip when unchanged,
     keep one previous image), prints the image summary.
   - `flash-soc --image update.img (--all | --partitions boot,rootfs) [--serial S] [--layout <mtdparts>]
     [--no-wait]`: Loader mode writes the chosen partitions with `WL`, MASKROM writes the whole image with
     `UF`; `reboot loader` is sent through ADB when the board is on ADB.
   - `mcu-info`, `mcu-update --package file.pkg --role rx|tx [--serial S]`, `mcu-set-role --role rx|tx`.
   - `pull-flash --target ... [--package file.pkg --role rx|tx] [--partitions ...]`: the Pull & Flash chain.
   - Common: `--tools DIR` (default: `tools/` next to the executable), `--log FILE` (the same lines as the
     tab's log pane), `--json` (one JSON object per line on stdout: `{"time","step","event","text",
     "percent"}` for agents), `--quiet`, `--timeout S`, `--yes` (no questions at all: refuse instead).
   - Secrets never on the command line: `--password-env`, `--password-stdin`, `--identity`, or the saved
     credentials of a profile name (`--target <profile>` uses the app's SecretStore like the tab).
4. **Exit codes**: 0 done; 1 the operation failed (the last log line says why); 2 usage; 3 no board, or the
   board in a mode the command cannot use; 4 SSH / authentication; 5 timeout or cancelled (Ctrl+C ends the
   waiting phases, never a write in progress - the same rule as the tab).
5. **Tests**: `tst_firmwarejob` (the chain on `FakeToolRunner` + `TestSshServer`: every step, the stop at
   the first failure, the decisions), `tst_firmwarecli` (the executable through `QProcess` with fake tools:
   every command, exit codes, `--json` lines, usage errors, secrets not echoed), the env-gated real-board
   case, both compilers, the installer / zip / tarball carry `su-fw`.
6. **Docs**: README "Command line" section with the agent recipe (`su-fw status`, `su-fw partitions`,
   `su-fw pull-flash ...`), DESIGN 4.12, VERSION.txt.

## Ask Moses before coding

- The executable name (`su-fw`), and whether the GUI executable should also accept the same subcommands
  (it would start `su-fw` and relay its output).
- `--json` from the start, or plain text first.
- Whether an agent may send `reboot loader` on its own (default: yes when the board is on ADB, as the tab
  does after its question; the question is replaced by `--yes` / refuse).
