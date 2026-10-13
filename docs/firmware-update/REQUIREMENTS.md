# Firmware tab: requirements (0.5.0)

All paths in this file are relative to the repository root unless they name another repository.

Add a **Firmware** tab to BuildAI Serial Utility (currently 0.4.0) that
replaces our whole Windows flashing routine for the Node board (Rockchip RV1106BG2 SoC + TI CC1310 RF MCU):
pull the latest build from the Linux build machine, flash the SoC, update the MCU, all over the board's single
USB-C cable, with no vendor GUI (RKDevTool / SocToolKit / FactoryTool), no USB-serial adapter and no MASKROM
button. Target release 0.5.0.

## Why
Today: build on Linux → run a small Python SFTP GUI (host/port/user/password, remote and local folder, extension filter `.img,.bin,.txt`,
recursive, overwrite or skip same-size files, progress, cancel; nothing else) to copy
`output/image/*.img` to Windows → open Rockchip's GUI tool, click through it → for the MCU, plug a USB-serial
adapter into the CC1310 and run `fw_update.py`. We want one tab and, for the common case, one button:
**Pull & Flash**.

## Read first (SerialUtility)
- docs/DESIGN.md (architecture, module contracts, UX rules: never block the GUI thread, status-bar errors,
  `tr()` on UI text, keyboard shortcuts, tests offscreen; §4.8 SSH), README.md, VERSION.txt, CMakeLists.txt,
  .github/workflows/build.yml, packaging/windows/BuildAI-SerialUtility.iss.
- src/ui/MainWindow.{h,cpp}: the QTabWidget holds only SessionWidgets, and `currentSession()` / `sessionAt()` /
  tab restore assume that. A non-session tab must not break those paths (null checks, session actions disabled
  while it is current, not saved/restored as a port).
- src/ssh/SshConnection.h, SshWorker.h, SshProfile.h, SecretStore.h: the image pull must reuse this stack
  (profiles, known_hosts, DPAPI-remembered passwords, SFTP with the shell fallback). This project is pure C++:
  no Python, no new SSH/SFTP library, no helper scripts.

## Read first (the other two repos; clone them next to this one, e.g. `../rockchip-platforms` and `../CC1310-Firmware`;
## rockchip-platforms is internal: if `gh` cannot see it, ask me)
- build-ai-ego/rockchip-platforms, branch `rv1106-bg2-main` (tag v1.3.0 and later): README.md,
  `tools/usb_update.sh` (the Linux reference of the SoC flow below, port its logic), `project/app/rfmcu/README.md`
  and `rfmcu.c` (the on-board MCU bridge). Windows binaries, Git LFS: `tools/windows/SocToolKit/bin/windows/`
  (`upgrade_tool.exe`, a native Win32 console tool, v2.37; `license.txt` one level up),
  `tools/windows/FactoryTool_v1.91.zip` (`bin/adb.exe`, `AdbWinApi.dll`, `AdbWinUsbApi.dll`),
  `tools/windows/DriverAssitant_v5.14.zip` (rockusb + ADB WinUSB drivers).
- moseschanxx/CC1310-Firmware, branch `feature/adb-update`: `tools/fw_protocol.py` (COBS + CRC16 framing),
  `tools/fw_package.py` (the .pkg header, CRC32), `tools/fw_update.py` (the updater state machine: HELLO/INFO,
  CLI `bootloader`, BEGIN/READY, DATA/ACK/NACK, END/COMPLETE, SET_ROLE, restart banner), `tools/adb_link.py`
  (adb + rfmcu bridge plumbing), `tools/tests/test_adb_link.py` (`FakeMcu`: a byte-level
  fake CC1310, port it to C++ for your tests), `docs/operations/04_serial_update.md` ("Over the USB-C cable
  (ADB)", Part J). These Python tools (read them there, copy nothing into this repo) are the golden reference: same protocol,
  same timeouts, same messages.

## What the board does (facts)
- Running firmware exposes **ADB only** on USB-C: VID 0x2207, PID 0x0006. `adb shell` works (verified on Windows
  with the DriverAssistant ADB driver).
- `adb reboot loader` → reboot-mode register → U-Boot `download` → **U-Boot rockusb** (Loader mode, VID 0x2207,
  PID 0x110x; read the mode from `upgrade_tool LD`, which prints `... Mode=Loader` or `Mode=Maskrom`). Verified on
  hardware 2026-10-13 (v1.2.0) with Rockchip's GUI tool; `upgrade_tool.exe UF` against it is **not** verified
  on Windows yet: treat your first real run as the integration test and report what you see.
- `update.img` is a Rockchip RKFW package (magic `RKFW`, ~11 MB). `UF` rewrites every partition incl. userdata,
  then resets the board; success is the line `Upgrade firmware ok`. Find out whether the Windows
  `upgrade_tool.exe` needs a `config.ini` next to it like the Linux one does.
- MCU link: CC1310 UART0 ↔ SoC UART1 `/dev/ttyS1`, 115200 8N1. `rfmcu bridge 7000` on the board joins it to
  127.0.0.1:7000 (one client at a time, exclusive flock on the tty; a busy port names the holder and exits 2).
  On the PC: `adb forward tcp:0 tcp:7000` prints a free local port; connect a QTcpSocket to it and speak the
  fw_update protocol. Kill a stale `rfmcu` first (`adb shell killall rfmcu`), and again plus
  `adb forward --remove` when done: adbd does not reliably end the board process when the host side closes.
- **SoC power during an MCU update.** The CC1310 switches part of the SoC's power, and its reset into the updater
  releases that switch; on USB power the board's power path keeps the SoC up, and this path always runs with the
  USB-C cable in, so no guard is needed. Log the CLI `power` reply before sending `bootloader`, as `fw_update.py`
  does.
- **Verified on hardware 2026-10-13:** a full CC1310 update over USB-C from Windows (VS Code task "Flash app (ADB)",
  `fw_update.py --port adb`, firmware.pkg 0.8.1, 78,040 bytes, role rx): adb, `rfmcu bridge`,
  CLI `bootloader`, the whole COBS transfer and the restart; the SoC stayed up through the CC1310 reset on USB
  power. The C++ updater must behave exactly like that run.

## What to build
1. **Firmware tab** (File > Firmware Update, own icon, one instance), three sections plus a log pane:
   - **Source**: a build-machine target (an existing SSH profile or user@host, reusing the SSH stack and its
     prompts) and a remote image folder (default `<sdk>/output/image`, e.g. `~/rk/rockchip-platforms/output/image`;
     remember the last one per target), a local cache folder,
     a **Pull** button, and a local-file picker as the offline alternative. Show the remote `update.img` size and
     mtime before pulling; skip the download when size and mtime match the cached copy; download to `.part`
     then rename; validate the RKFW magic; keep the previous image (one-level history) so a bad build can be
     rolled back from the tab. Optionally run `git -C <sdk> describe --tags --dirty` over the same connection and
     show it next to the image (add a small exec-command API to SshConnection/SshWorker if there is none; it
     already opens exec channels for the shell transfer fallback). Cover what the old Python puller did
     (update.img, plus optional extensions); no recursive tree copy UI unless trivial.
   - **SoC**: device status line refreshed on its own (ADB device with serial / Loader / Maskrom / none), SFI
     info of the selected image, **Flash SoC**, progress parsed from upgrade_tool output (`\r`-separated, e.g.
     `Download Image... (42%)`), Cancel only while waiting, never during UF.
   - **MCU (CC1310)**: package picker (`.pkg`, validate header and both CRC32s, show target/version/size),
     current MCU version/role read through ADB, role choice rx/tx (default: the role the MCU reports;
     the user confirms, never guess silently), **Update MCU**, **Set role**, progress per DATA ACK.
   - **Pull & Flash**: pull → flash SoC → wait for ADB to come back → (if an MCU package is selected and differs
     from the running version) update the MCU. SoC first, because the MCU path needs `rfmcu` on the SoC.
2. **Getting the SoC into download mode**, in order: already Loader/Maskrom → flash (also the recovery path for a
   bricked board); ADB device → `adb -s <serial> reboot loader`; a connected serial or SSH session tab →
   offer to send `reboot loader\r` through it (SessionWidget send path), ask first; otherwise tell the user to hold
   MASKROM and plug in. Wait (timeout, cancellable) for Loader/Maskrom, run `UF`, require `Upgrade firmware ok`,
   wait for the board to return on ADB and show `uname -a`/uptime.
3. **External tools behind a small interface** (a ToolRunner over QProcess, asynchronous, scripted fakes in
   tests). Never block the GUI thread. The CC1310 protocol is ours and small: implement it natively in C++
   (COBS, CRC16-CCITT 0xFFFF, package CRC32, the fw_update state machine and retries) over QTcpSocket, unit-tested
   against known-answer vectors (frames, CRCs, a small .pkg) taken from the CC1310 repo's Python tools and
   committed as test data; neither the build nor the tests run Python.
4. **Drivers**: if a 0x2207 device is enumerated but adb/upgrade_tool cannot see it, say so and offer to launch the
   bundled DriverAssistant; consider an optional installer task for it.
5. **Packaging**: ship upgrade_tool.exe (+ config.ini if needed, + Rockchip license.txt), adb.exe + the two DLLs
   in a `tools/` folder next to the exe via CMake install + the Inno Setup script; prefer a user's newer adb on
   PATH only if the bundled one is missing. On Linux use the Linux upgrade_tool and system adb so CI still builds
   and tests run.
6. Translations (zh_CN + en_US), README section, VERSION.txt entry (0.5.0), DESIGN.md section (new §4.11), and
   remove nothing from the existing tabs' behaviour.
7. **Tests** in the existing style: output parsing (LD, SFI, UF progress and result, adb devices/forward), the SoC
   state machine and the MCU updater with fake tools and a C++ FakeMcu (incl. a CC1310 already in the updater, NACK/resume, retries on a lost frame, banner after COMPLETE), the pull logic against the
   project's existing SSH test server, and a GUI test for the tab. Keep both CI jobs green with warnings as errors.

## Ask me before
- Implementing the rockusb or ADB protocols natively (libusb/WinUSB) instead of bundling upgrade_tool/adb.
- Anything that changes how existing serial/SSH tabs behave.
- Flashing a real board for the first time: tell me what will run, then report exactly what happened.
