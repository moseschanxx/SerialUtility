# Firmware tab (0.5.0): implementation plan

Written 2026-10-13 before any code, as the requirements ask. Paths are relative to the repository
root unless they name another repository.

## Facts established before the plan

- Spec: `docs/firmware-update/REQUIREMENTS.md` (commit e6fe031, pulled from origin/main).
- Reference repos, cloned next to this one as sparse, reference-only checkouts taken from the
  rk VM (10.0.10.71, `~/rk/...`) over SSH, because `gh` is not logged in on this PC:
  - `../rockchip-platforms`: branch `rv1106-bg2-main`, `v1.3.0-4-ge0e0d41`; `README.md`,
    `tools/usb_update.sh`, `tools/windows/` (the binaries are real files, copied from the VM's
    working tree because they are Git LFS objects; `DriverAssitant_v5.14.zip` is untracked on the
    VM and was copied as a plain file), `tools/linux/Linux_Upgrade_Tool/`, `project/app/rfmcu/`.
  - `../CC1310-Firmware`: branch `feature/adb-update`, 4e60798; `tools/`, `docs/`, `releases/`,
    `bootloader/`, `app/`.
  Nothing on the VM was modified.
- `upgrade_tool.exe` v2.37 (Windows) runs on this PC without a `config.ini` next to it
  (`LD` and `SFI` verified). Output seen:
  - banner line `rockusb driver = 5.12,please upgrade it to enjoy fast download`
  - `LD`: `List of rockusb connected(0)`; with a device (from the Linux tool and the SocToolKit
    source of truth): `DevNo=1	Vid=0x2207,Pid=0x110e,LocationID=...	Mode=Loader	SerialNo=...`
  - `SFI`: `Type:Update Firmware`, `Chip Tag:110E	Version:0.0.00	Build Time:2026-10-13 21:45:50
    Sign:false`, `Loader ver:1.01	Loader Time:...`, `Entry Count:9`, one `EntryNo=n ; file=...;
    partition=...; type=...; offset=0x...; size=0x...` per entry.
  - `UF`: `\r`-separated progress such as `Download Image... (42%)`, success line
    `Upgrade firmware ok` (per `usb_update.sh`; to be confirmed on the first real run).
- Board 0225ce3c8d8d6e44 (USB\VID_2207&PID_0006) is attached in ADB mode: kernel 5.10.209+ built
  2026-09-30, `rfmcu` at `/usr/bin/rfmcu`, `ao_record_demo` running, an `rfmcu console` (pid 190)
  holding `/dev/ttyS1`. busybox has `ps`, `killall`, but no `uname` and no `pgrep`: the tab reads
  `/proc/version` and `uptime` after a flash.
- The VM holds `~/rk/rockchip-platforms/output/image/update.img` (11,354,755 bytes, built
  2026-10-13 21:45, RKFW, 9 entries) and `CC1310-Firmware/releases/firmware-0.8.1.pkg`
  (78,072 bytes; the MCU on the board reports 0.8.1).
- Rockchip's `license.txt` next to upgrade_tool is the LGPL v3 text; it ships with the tool.

## Architecture

### Core (`src/firmware/`, part of `su_core`, UI-free, Qt Core + Network only)

| Class | Reference | Role |
|---|---|---|
| `ToolRunner` / `ProcessToolRunner` | - | One external process, asynchronous (QProcess), merged stdout+stderr delivered raw (with `\r`), cancel, exit code. `FakeToolRunner` in tests scripts outputs per command. |
| `ToolLocator` | spec §5 | Finds `upgrade_tool` and `adb`: Preferences override, then `tools/` next to the executable, then PATH (adb only). |
| `RockchipTool` | `usb_update.sh`, tool output | `LD` device list parser (Mode Loader / Maskrom), `SFI` parser, `UF` progress parser (`\r` chunks, `(nn%)`), result check (`Upgrade firmware ok`). |
| `AdbTool` | `adb_link.py` | `devices` / `get-state` parsing, `reboot loader`, `shell`, `forward tcp:0` (local port), `forward --remove`, long-running `shell rfmcu bridge`. |
| `UsbProbe` | spec §4 | Windows: SetupAPI enumeration of `USB\VID_2207*` (present, driver class) so "device present but no adb/rockusb driver" can be reported and the bundled DriverAssistant offered. Other platforms: no-op. |
| `CcProtocol` | `fw_protocol.py` | COBS encode/decode, CRC16-CCITT (0xFFFF, 0x1021), frame v2 `<BBHH>` + payload + CRC16, message ids HELLO..SET_ROLE. |
| `CcPackage` | `fw_package.py` | FWPK header `<IHHIIIIII>`, header CRC32 and image CRC32 (zlib), target, address 0x8000, size <= 0x17000, version `0x00MMmmpp`. |
| `McuUpdater` | `fw_update.py` | State machine over a `QIODevice`: HELLO x2 -> INFO; else CLI probe (`""`, `version`), `power` logged when through the SoC, `bootloader` -> `OK rebooting_to_bootloader`, 500 ms settle, HELLO x10; BEGIN(package[:32] + role) -> READY offset 0; DATA 128-byte chunks -> ACK/NACK (NACK offset <= sent); END -> COMPLETE; banner wait (`version=` ... `cli=ready`, 8 s). Timeouts 0.5 s per frame, 5 retries, CLI 1 s, bootloader command 2 s. `info()`, `flash()`, `setRole()`. |
| `McuLink` | `adb_link.py` | Select the board, check `rfmcu`, `killall rfmcu`, `forward tcp:0 tcp:7000`, start `rfmcu bridge 7000`, wait for "bridging", connect a QTcpSocket; close = terminate the bridge, `killall rfmcu`, `forward --remove`. |
| `SocFlasher` | `usb_update.sh` | `LD` -> already Loader/Maskrom: flash; else ADB device -> `reboot loader`; else ask the UI (session tab / MASKROM instructions); wait for Loader/Maskrom (timeout, cancellable); `UF`; require `Upgrade firmware ok`; wait for ADB; read `/proc/version` + `uptime`. Cancel only while waiting. |
| `ImagePuller` | the old Python puller, spec §1 | Over an `SshConnection`: stat the remote `update.img` (size, mtime), skip when the cache matches, download (the existing `.part` + rename path), RKFW magic check, one-level history (`update.prev.img`), optional `git -C <sdk> describe --tags --dirty`, optional extra extensions. |

### SSH stack additions (no behaviour change for tabs)

- `SshConnection::runCommand(command) -> commandFinished(id, exitStatus, stdout, stderr)` and
  `SshConnection::statRemote(path) -> remoteStatReceived(path, exists, size, mtime)`; both
  asynchronous, run by the worker (`runExec()` exists already; SFTP stat with an exec
  `stat -c '%s %Y'` fallback).
- `tests/support/TestSshServer`: `stat -c '%s %Y' 'p'` and a scripted `git -C 'p' describe --tags --dirty`.

### UI (`src/ui/FirmwareTab.*`, `su_app`)

- File > Firmware Update (Ctrl+Shift+U, icon `:/icons/firmware.svg`), one instance: a second
  invocation selects the existing tab.
- Sections: Source (target combo from the SSH profiles + recent targets + free text; remote folder
  remembered per target; cache folder; Pull; local file picker; image line with size, mtime, RKFW
  check, git describe; previous image with Roll back), SoC (status line refreshed every 2 s:
  ADB serial / Loader / Maskrom / none / driver hint; SFI info; Flash SoC; Cancel), MCU (package
  picker with header/CRC/target/version/size; version and role read through ADB; role rx/tx
  defaulting to the reported role, confirmed by the user; Update MCU; Set role; progress per ACK),
  Pull & Flash with an overall progress, and a log pane.
- `MainWindow`: `currentSession()` is already null-safe; session actions are disabled while the
  Firmware tab is current, the tab is not saved/restored as a port, the Window menu lists it,
  closing it while a flash runs asks first. "Send `reboot loader` through a connected tab" asks
  once per use and uses `SessionWidget::sendBytes()`.
- `AppSettings`: `firmware/lastTarget`, `firmware/remoteFolder/<target>`, `firmware/cacheDir`,
  `firmware/lastPackage`, `firmware/upgradeToolPath`, `firmware/adbPath`, `firmware/waitSeconds`.
- Preferences > Firmware: tool paths (auto or custom), wait timeouts, cache folder.

### Packaging

- `third_party/rockchip/windows/upgrade_tool.exe` + `license.txt` + `revison.log` excerpt,
  `third_party/rockchip/linux/upgrade_tool` + `config.ini` + `88-rockusb.rules`,
  `third_party/adb/` (`adb.exe`, `AdbWinApi.dll`, `AdbWinUsbApi.dll` from FactoryTool 1.91; if that
  adb cannot `forward tcp:0`, Google's platform-tools adb instead, recorded in the README),
  `third_party/DriverAssistant/DriverAssitant_v5.14.zip` extracted at build time
  (`file(ARCHIVE_EXTRACT)`). Everything installs to `bin/tools/`; the Inno Setup script gets an
  optional task "Install the Rockchip USB drivers"; both CI jobs check the installed files.

### Tests

- `tst_ccprotocol`: known-answer vectors generated once with the CC1310 repo's Python tools on
  this PC and committed under `tests/data/firmware/` (COBS, CRC16 `123456789` = 0x29B1, frames,
  a small package, `firmware-0.8.1.pkg`); neither the build nor the tests run Python.
- `tst_mcuupdater`: C++ `FakeMcu` (application CLI then updater) behind a QTcpServer: full
  update, already in the updater, NACK/resume, lost frame retries, banner after COMPLETE,
  set role, power logged before `bootloader`.
- `tst_firmwaretools`: LD/SFI/UF/adb parsers, `SocFlasher` and `McuLink` with fake tools,
  driver hint.
- `tst_imagepuller`: against `TestSshServer` (stat, skip, `.part`, magic, history, git describe).
- `tst_firmwaretab`: GUI, offscreen: the action, single instance, enable states, a pull, a flash
  and an MCU update with the fakes, MainWindow actions while the tab is current, close/restore.

## Work packages

Headers (contracts) first, then packages in their own `build/wf-<pkg>` directories, two at a
time on this 4-core machine, then integration, a review pass, fixes, docs, the real-board run.

| Package | Files | Model |
|---|---|---|
| protocol | `CcProtocol`, `CcPackage`, `McuUpdater`, `tests/support/FakeMcu`, `tst_ccprotocol`, `tst_mcuupdater`, test data | Sonnet |
| tools | `ToolRunner`, `ToolLocator`, `RockchipTool`, `AdbTool`, `UsbProbe`, `McuLink`, `SocFlasher`, `tst_firmwaretools` | Sonnet |
| ssh-pull | `SshConnection`/`SshWorker` exec + stat API, `ImagePuller`, `TestSshServer` commands, `tst_imagepuller` | Sonnet |
| ui | `FirmwareTab`, `MainWindow` integration, `AppSettings`, Preferences page, icon, `tst_firmwaretab`, translations | Opus |
| packaging-docs | `third_party/`, CMake install, CI checks, Inno Setup task, README, DESIGN §4.11, VERSION.txt | Sonnet |
| review | pre-hand-over review of the integrated tree | Fable |

## "Ask me before" (from the requirements)

1. Native rockusb / ADB: not applicable. upgrade_tool and adb are bundled.
2. Existing serial/SSH tab behaviour: no change. Two touch points: the Firmware tab writes
   `reboot loader\r` into a connected tab only after a Yes in a dialog, each time; SshConnection
   gains two asynchronous calls that do not touch connect, shell or transfer.
3. First real flash: applies. Moses said on 2026-10-13 that the attached board may be used for
   testing directly. What will run on 0225ce3c8d8d6e44 once the fake-tool tests pass:
   `upgrade_tool LD`; `adb -s 0225ce3c8d8d6e44 reboot loader`; poll `LD` up to 30 s for
   Mode=Loader/Maskrom; `upgrade_tool UF <cache>\update.img` (the VM's 2026-10-13 build);
   require `Upgrade firmware ok`; wait for ADB; read `/proc/version` and `uptime`. Then the
   MCU: `adb shell killall rfmcu` (ends the `rfmcu console` pid 190), `adb forward tcp:0 tcp:7000`,
   `adb shell rfmcu bridge 7000`, CLI `version` and `power` logged, `bootloader`, BEGIN/DATA/END
   with `firmware-0.8.1.pkg` role rx (the role the MCU reports), restart banner, `killall rfmcu`
   and `forward --remove`. The result is reported exactly as seen.
