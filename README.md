# BuildAI Serial Utility

CI: `.github/workflows/build.yml` builds and tests on Ubuntu and Windows for every push and publishes a release on `v*` tags.

A multi-tab serial-port terminal for the boards BuildAI works with every day: **Rockchip
Linux boards** (U-Boot prompt and Linux console at 115200 or 1 500 000 baud) and **MCU firmware
shells** (STM32 / ESP32 / RP2040 behind CH34x, CP210x, FTDI or J-Link CDC adapters). It replaces
ad-hoc use of PuTTY, minicom and SecureCRT with a tool that logs everything, remembers your
commands, survives board reboots and lives in the same Qt / CMake ecosystem as the team's
other tools.

The terminal itself contains **no device-specific protocol logic**: it is a general terminal that
speaks bytes. The one device-specific part is the **Firmware tab** (v0.5), a separate tab that
flashes the BuildAI Node board (Rockchip RV1106 SoC + TI CC1310 RF MCU) over its USB-C cable.

## Features

- **Multi-tab sessions** - one port per tab, movable and closable, with a coloured connection
  dot (green connected, amber connecting / reconnecting, grey disconnected). Tabs from the
  previous run are reopened on start.
- **SSH sessions** - the same terminal over SSH (libssh, OpenSSL): type `root@192.168.100.2`
  and connect, or keep named profiles (key file, password, keyboard-interactive or agent
  authentication, startup command, port forwards). Host keys are verified against your
  `~/.ssh/known_hosts` with a proper "identification has changed" warning, `~/.ssh/config`
  aliases are honoured, passwords can be remembered (Windows DPAPI), the shell follows the
  window size, keep-alives detect dead links and a dropped connection reconnects by itself.
  Upload / download files over SFTP (drag a file onto the terminal to upload).
- **Firmware tab** (*File > Firmware Update*, Ctrl+Shift+U) - one tab that replaces the whole
  Windows flashing routine for the Node board: **pull** the newest `update.img` from the Linux
  build machine over the SSH stack (size / time / `git describe` shown first, unchanged images
  skipped, one-level history with *Roll back*), **flash the SoC** with Rockchip's `upgrade_tool`
  (the tab puts the board into Loader mode over ADB by itself and writes the image partition by
  partition like SocToolKit's Download page - all of them, or only the ones you check), **update the CC1310** with a
  native C++ implementation of its UART updater protocol through the SoC, and **Pull & Flash**
  to do all three with one button - everything over the board's single USB-C cable, with
  `upgrade_tool`, `adb` and the Rockchip USB drivers bundled in the package.
- **`su-fw` command line** (v0.6) - the Firmware tab's operations for a shell, scripts and AI
  agents: `su-fw status`, `partitions`, `pull`, `flash-soc`, `mcu-info`, `mcu-update`,
  `mcu-set-role`, `pull-flash`, the log on stdout (text or one JSON object per line), the result
  as the exit code; the very same code as the tab.
- **Real terminal emulation** - VT100 / xterm subset: cursor movement, erase/insert/delete,
  scroll regions, 16 / 256 / true colours, alternate screen (`top`, `vi`, `menuconfig`),
  bracketed paste, DSR / DA replies, window title. Chinese and other CJK text occupies two
  cells. Unknown sequences are swallowed, never desynchronise the output.
- **Copy from a streaming console** - like the Windows console's mark mode: start selecting
  with the mouse and the display freezes while the incoming bytes queue up (a badge shows how
  much is waiting); **Enter** copies the selection to the clipboard and lets the output catch up,
  **Esc** cancels. Logging and the hex view keep receiving meanwhile. On by default, switchable
  in *View > Pause Output While Selecting*.
- **cmd.exe-style right click** - a right click pastes the clipboard, or copies the selected
  text when there is a selection (and lets a paused display continue); the context menu moves to
  **Shift+right click** and the Menu key. On by default, switchable in *View > Right Click
  Pastes (cmd.exe style)*.
- **Built for boot logs** - handles 1.5 Mbaud output (about 150 KB/s) without freezing the UI,
  with plenty of headroom: the RX pipeline renders a coloured boot log at around 10 MB/s on a
  4-core desktop while the window stays responsive. Repaints are coalesced and drawn from a glyph
  cache, the hidden hex view queues instead of rendering, scrollback is bounded (default 10 000 lines).
- **Auto baud rate** - pick *Auto* in the baud list: the tool listens at the candidate rates
  (115200, 1500000, 921600, ... configurable) until the output reads as text, applies the rate
  and tells you; if the board switches rates mid-session (U-Boot at 1 500 000, a kernel at 115200)
  the garbage is noticed and the rate re-detected. *Session > Detect Baud Rate* (Ctrl+Shift+B)
  does it on demand for a fixed rate too.
- **Keyboard that belongs to the shell** - while a session is connected, every key that is not
  an application shortcut goes to the device, bare Ctrl+letter combinations included (Ctrl+W
  deletes a word in bash instead of closing the tab). Every shortcut is configurable in
  *Preferences > Keyboard*; a *Window* menu, Alt+1..9 and Ctrl+PageUp/PageDown switch tabs.
- **Auto-reconnect** - when the board reboots or the USB adapter is re-plugged the port vanishes;
  the session shows a dim system line and re-opens the port as soon as it comes back.
- **Built-in device simulator** - four `SIM:` pseudo-ports (`SIM:loopback`, `SIM:linux`,
  `SIM:uboot`, `SIM:mcu`) are listed next to the real ports so the whole tool can be tried
  without hardware: a Rockchip-style boot log with login and a busybox-like shell (`top`,
  coloured `dmesg`, `progress`, `chinese`, `wide`, `reboot` - the port vanishes for 3 s and
  auto-reconnect kicks in), a U-Boot autoboot countdown that boots into Linux, and an AT-style
  MCU shell with telemetry. Hide them in *Preferences > Connection* for production use.
- **Line parameters on the fly** - baud, data bits, parity, stop bits and flow control are
  applied to an open port immediately. DTR / RTS toggles (reset / boot pins) and **Send BREAK**.
- **Quick commands** - user-defined macros shown as buttons, grouped (Node, Linux, U-Boot, MCU,
  Control), with optional shortcuts, HEX payloads (`03` for Ctrl+C) and C-style escapes. Import
  / export as JSON. The *Node* group (v0.5) holds the SoC console's `reboot loader` and
  `rfmcu console` and the CC1310 CLI commands.
- **Command input line** - line-mode input with history (persisted), hex mode, escape
  interpretation and a per-line line-ending selector (none / CR / LF / CRLF).
- **Send file** - paced transfer of shell scripts or U-Boot environments line by line, or raw
  binaries in chunks, with pause / resume and progress.
- **Session logging** - raw capture, timestamped text or hex dump; flushed after every write so
  the log survives a board crash. Auto-start on connect if you want.
- **Log replay** - *File > Replay Log File...* (or `--replay <file> [--speed <baud>]`) streams a
  captured log - a raw capture or a timestamped text capture, detected automatically - through
  the terminal, hex view and logger at the chosen line rate, exactly as if the device were
  sending it. Inspect a field capture with full colour rendering, reproduce a rendering bug
  from a customer log, or demo the tool without hardware.
- **Hex view** - toggle a bounded RX / TX hex dump with timestamps for the current session.
- **System Log dock** - every Qt message (`qCInfo(lcSerial)` and friends) colour-coded by
  severity, like the Vispek reference tool.
- **Keyboard first** - every action has a shortcut; Tab is delivered to the device when the
  terminal has focus.
- **English / Chinese UI**, switchable at run time.

## Prerequisites

| Component | Version |
|---|---|
| Qt | **6.8** with modules Core, Gui, Widgets, SerialPort, Network, LinguistTools (Test for the unit tests) |
| OpenSSL | 3.x development files for libssh's crypto backend: Windows `C:\Program Files\OpenSSL-Win64` (Shining Light installer) or any prefix via `-DOPENSSL_ROOT_DIR`; Linux `libssl-dev` |
| CMake | 3.22 or newer (3.25+ for the presets); internet access at the first configure to fetch libssh 0.11.1 |
| Generator | Ninja |
| Compiler (Windows) | MSVC 2022 x64 (`D:\Qt\6.8.3\msvc2022_64` is the default Qt prefix) |
| Compiler (Linux) | GCC 12+ or Clang 15+ |
| Linux distro Qt packages | Debian 13+ / Ubuntu 25.10+ (older releases: install Qt 6.8.3 via aqtinstall) |

The only third-party library besides Qt is **libssh 0.11.1**, which CMake downloads (SHA-256
pinned) and builds from source at configure time; it needs OpenSSL. The resulting `ssh.dll` /
`libssh.so.4` and `libcrypto` are copied next to the executables and shipped with every package,
so end users install nothing.

The Firmware tab drives three third-party **programs** (not libraries), kept as plain files in
`third_party/` with their provenance and licences (`third_party/README.md`, about 20 MB): Rockchip's
`upgrade_tool` (Windows v2.37, Linux v2.26), Rockchip's build of `adb` with its two DLLs, and
the Rockchip USB driver installer (DriverAssistant 5.14). The build copies them to `tools/` next to
the executable and `cmake --install` ships that folder as `bin/tools/`; nothing is downloaded.

## Building

### Windows - `scripts/build.ps1` (recommended)

The script imports the MSVC environment itself, so it works from any PowerShell, no
"Developer Command Prompt" needed.

```powershell
# from the repository root
.\scripts\build.ps1 -Config Release -Test           # configure + build + ctest
.\scripts\build.ps1 -Config Debug  -Target su_core   # just the core library
.\scripts\build.ps1 -Config Release -Deploy          # dist\Release\bin with windeployqt
.\scripts\run.ps1   -Config Release                  # launch the built application
```

Useful switches: `-QtDir <prefix>` (or set `QT_ROOT`), `-Clean`, `-KeepGoing` (report every
error), `-Jobs N`, `-VerboseBuild`.

### Running the tests

```powershell
.\scripts\build.ps1 -Config Release -Test            # build everything, then ctest
ctest --test-dir build\Release -C Release --output-on-failure            # re-run all suites
ctest --test-dir build\Release -C Release -R tst_terminalwidget --output-on-failure   # one suite
```

There are two kinds of Qt Test suites under `tests/`:

- **Unit tests** linked against `su_core` (`tst_hexutils`, `tst_lineending`, `tst_commandhistory`,
  `tst_quickcommand`, `tst_charwidth`, `tst_terminalscreen`, `tst_ansiparser`,
  `tst_serialsettings`, `tst_devicesimulator`, `tst_logreplayer`, and for the Firmware tab
  `tst_ccprotocol`, `tst_mcuupdater`, `tst_firmwaretools`, `tst_imagepuller`, `tst_firmwarejob`
  and `tst_firmwarecli` (su-fw): scripted fake tools and a C++ fake CC1310, no hardware and no
  Python).
- **GUI tests** linked against `su_app`, which drive the real widgets and dialogs
  (`tst_terminalwidget`, `tst_sessionwidget`, `tst_mainwindow`, `tst_dialogs`, `tst_firmwaretab`). They talk to the
  built-in `SIM:` pseudo-ports, never to a real COM port, and they run **offscreen**: ctest sets
  `QT_QPA_PLATFORM=offscreen` for every test, so no window appears and the suites also pass on a
  headless CI runner. Each GUI suite uses its own QSettings scope (`BuildAI-Test` /
  `SerialUtilityTest-<suite>`, INI format in a temporary directory) and a temporary log
  directory, so running them never touches your real settings or logs. To run a suite by hand:

```powershell
$env:PATH = "D:\Qt\6.8.3\msvc2022_64\bin;$env:PATH"; $env:QT_QPA_PLATFORM = "offscreen"
.\build\Release\tst_mainwindow.exe          # the test executables live in the build root
```

### CMake presets (any platform)

```bash
cmake --preset Release          # Windows MSVC preset; use linux-release on Linux
cmake --build --preset Release
ctest --preset Release
```

The presets live in `CMakePresets.json`. They pick up Qt from the `QT_ROOT` environment variable
(via `$penv{QT_ROOT}`, so your own value is always honoured); when it is unset, `CMakeLists.txt`
falls back to `D:/Qt/6.8.3/msvc2022_64` on Windows and to the system Qt on Linux. To pin a
different prefix per machine without touching the shared file, create a gitignored
`CMakeUserPresets.json`:

```json
{ "version": 6, "configurePresets": [
  { "name": "Release-local", "inherits": "Release",
    "cacheVariables": { "CMAKE_PREFIX_PATH": "E:/Qt/6.8.3/msvc2022_64" } } ] }
```

### Plain CMake (Linux)

The project requires Qt **6.8 or newer** (`find_package(Qt6 6.8 ...)` in `CMakeLists.txt`).
Pick whichever of the two routes matches your distro:

**A. Qt from the distro (Debian 13 "trixie" / Ubuntu 25.10 or newer only).** Ubuntu 22.04 (Qt 6.2)
and 24.04 (Qt 6.4) are too old for this route - use B. `qt6-l10n-tools` is required: it provides
`lrelease`/`lupdate`, which `qt6-tools-dev` does not pull in.

```bash
sudo apt install build-essential cmake ninja-build \
    qt6-base-dev qt6-serialport-dev qt6-tools-dev qt6-tools-dev-tools qt6-l10n-tools
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
QT_QPA_PLATFORM=offscreen ctest --test-dir build --output-on-failure
./build/BuildAI-SerialUtility
```

**B. Qt 6.8.3 from the Qt online installer or `aqtinstall` (what CI does; works on any distro).**
Install the `gcc_64` desktop kit with the `qtserialport` module, then point CMake at it with
`CMAKE_PREFIX_PATH` (or export `QT_ROOT` and use `cmake --preset linux-release`).

```bash
sudo apt install build-essential cmake ninja-build libgl1-mesa-dev libxkbcommon-dev libxkbcommon-x11-dev libxcb-cursor0
pipx install aqtinstall            # or: pip install --user aqtinstall
aqt install-qt linux desktop 6.8.3 linux_gcc_64 -m qtserialport -O ~/Qt
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=~/Qt/6.8.3/gcc_64
cmake --build build
QT_QPA_PLATFORM=offscreen ctest --test-dir build --output-on-failure
./build/BuildAI-SerialUtility
```

### Qt Creator

*File > Open File or Project* -> `CMakeLists.txt`, then pick the `Release` or `Debug` preset
(kit "Desktop Qt 6.8.3 MSVC2022 64bit"). Nothing else to configure.

### Deploying

`cmake --install build --prefix dist` (or `build.ps1 -Deploy`) copies the executables
(`BuildAI-SerialUtility` and the `su-fw` console tool) to `dist/bin` and runs `windeployqt` / the
Qt deploy script for each, so the folder is self-contained.
The Linux tarball published by CI is built the same way (`bin/`, `lib/`, `plugins/`, `qt.conf`)
and is self-contained except for the system X11/xcb libraries, which users install via apt:
`sudo apt install libxcb-cursor0 libxkbcommon-x11-0 libgl1`.

Both packages carry the flashing tools in `bin/tools/` next to the executable (Windows:
`upgrade_tool.exe`, `adb.exe`, `AdbWinApi.dll`, `AdbWinUsbApi.dll`, `license.txt`, `revison.log`,
`DriverAssistant\DriverInstall.exe` and its drivers; Linux: `upgrade_tool`, `config.ini`,
`88-rockusb.rules`, `license.txt`; on Linux `adb` comes from the system `PATH`). CI checks that
they landed after `cmake --install`.

### Packaging (Windows zip + installer)

```powershell
.\scripts\package.ps1            # build + test + deploy, then zip and Inno Setup installer
.\scripts\package.ps1 -SkipBuild # package an existing dist\Release\bin
```

Produces `dist\BuildAI-SerialUtility-<version>-windows-x64.zip`,
`dist\installer\BuildAI-SerialUtility-<version>-windows-x64-setup.exe` (needs
[Inno Setup 6](https://jrsoftware.org/isdl.php): `winget install JRSoftware.InnoSetup`) and
`dist\SHA256SUMS.txt`. The installer script lives in `packaging/windows/`. Its optional task
**Install the Rockchip USB drivers (DriverAssistant)** (unchecked by default) starts the bundled
`tools\DriverAssistant\DriverInstall.exe` at the end of the installation; it is a GUI installer
you click through, needed once per PC before the Firmware tab can see a Node board. The
uninstaller removes `{app}\tools`.

### Releasing (CI/CD)

Every push and pull request runs `.github/workflows/build.yml`: Ubuntu (GCC) and Windows
(MSVC 2022) builds with `-DSU_WARNINGS_AS_ERRORS=ON`, all Qt Test suites offscreen, a deploy
check, and the packaged artefacts (Linux tarball, Windows zip and installer) attached to the
run for 14 days.

A release is cut by tagging. The tag must match `project(... VERSION x.y.z)` in
`CMakeLists.txt`, and `VERSION.txt` must have a `Vx.y.z` entry (its bullet list becomes the
release notes):

```powershell
# 1. bump the version in CMakeLists.txt and add the Vx.y.z block to VERSION.txt, commit
# 2. tag and push
git tag -a v0.1.0 -m "BuildAI Serial Utility 0.1.0"
git push origin v0.1.0
```

The `release` job then publishes a GitHub Release named "BuildAI Serial Utility x.y.z" with
`BuildAI-SerialUtility-x.y.z-windows-x64-setup.exe`, `...-windows-x64.zip`,
`...-linux-x64.tar.gz` and `SHA256SUMS.txt`. A tag containing a hyphen (`v0.2.0-rc1`) is
published as a pre-release.

## Usage guide

### Trying it without hardware (SIM: ports)

By default the port list also contains four simulated devices (manufacturer "BuildAI Simulator"):

| Port | What it does | Try |
|---|---|---|
| `SIM:loopback` | echoes every byte | hex view, logging, encodings, file send |
| `SIM:linux` | Rockchip RV1106-style boot log, `login:` (any user / password), busybox-like shell with device-side line editing | `help`, `top -n 1`, `dmesg \| tail -n 50`, `color`, `chinese`, `wide`, `progress`, `stty size`, `reboot` (port vanishes 3 s, auto-reconnect), `poweroff` (device stays listed but is down until you reconnect), `exit` |
| `SIM:uboot` | U-Boot banner + "Hit any key to stop autoboot" countdown, `=>` prompt, boots into the Linux simulation | press a key during the countdown, `printenv`, `setenv`, `bdinfo`, `mmc info`, `md 02008000`, `boot`, `reset` |
| `SIM:mcu` | "BuildAI MCU shell" with `\r\n` lines and an AT command set | `help`, `led on`, `adc`, `telemetry on`, `AT`, `AT+GMR`, `ATE0`, `AT+RST` (device resets, port vanishes 1.5 s) |

Output is paced to the selected baud rate (pick 1500000 to see a boot log fly by, 300 to
watch bytes trickle). Uncheck *Preferences > Connection > Show simulated devices* to hide
them.

### Finding the baud rate automatically

Pick **Auto** at the top of the baud list and press Connect. The tool opens the port at the
last known rate and, while the board talks, listens at each candidate rate (Preferences >
Connection: default 115200, 1500000, 921600, 460800, 230400, 57600, 38400, 19200, 9600, about
1.5 s each) until the output reads as text; the terminal stays quiet meanwhile and then shows
"--- baud rate 1500000 detected ---" followed by the live output. A silent board cannot be
detected (nothing to listen to): the previous rate is kept and the status bar says so - reset
the board or press Enter on its console and try *Session > Detect Baud Rate* (Ctrl+Shift+B),
which works for a fixed rate as well. With Auto selected the session keeps watching: when the
output turns into garbage because the board switched rates, it re-detects by itself (switch this
off in Preferences > Connection). The `SIM:linux` / `SIM:uboot` simulators talk at 1 500 000 and
`SIM:mcu` at 115200 and produce real-looking garbage at any other rate, so you can try it without
hardware.

### Connecting to a Rockchip board

1. Plug the USB-UART adapter in (CH343 / CP2102 on the debug header) and press **Ctrl+T** for
   a new session tab if the first one is already used.
2. Pick the port in the connection bar (ports appear and disappear live; F5 forces a rescan).
3. Choose the baud rate:
   - **1500000** for RV1106 / RV1106B ("Luckfox Pico") and most recent Rockchip SDKs;
   - **115200** for older boards or boards whose `rk_uart` / `console=` was changed.
   8 data bits, no parity, 1 stop bit, no flow control (the default) is correct for both.
4. Press **Connect** (F2). Reset or power the board; the boot log appears immediately.

**Interrupting U-Boot:** hold **Ctrl+C** (or use the *Control > Ctrl+C* quick command) while the
"Hit any key to stop autoboot" countdown runs. On boards with a very short `bootdelay` start
holding the key before you power the board. Once at the `=>` prompt the *U-Boot* quick commands
(`printenv`, `bdinfo`, `mmc info`, `boot`, `reset`) are one click away.

**Logging in:** the Linux console expects **CR** for Enter and **DEL (0x7F)** for Backspace,
which are the defaults. If Backspace prints `^H` instead of erasing, uncheck *Preferences >
Input > Backspace sends DEL*.

**Board reboots:** the session prints a dim `port COMx disappeared` line, switches to amber
"Reconnecting..." and re-opens the port when it is back. Disable this in *Preferences >
Connection* if you prefer manual control.

### MCU shells

- Typical rates: 9600 - 921600. AT-style firmware usually wants **CRLF** for Enter; set it in the
  command input's line-ending selector or per quick command (`AT`, `AT+GMR` are shipped with CRLF).
- Firmware that prints bare `\n` produces "staircase" text; *Preferences > Terminal > Implicit
  carriage return on line feed* (on by default) fixes it.
- **DTR / RTS** buttons in the connection bar drive reset / boot pins on ESP32 and similar
  boards. **Send BREAK** asserts the line for 250 ms.
- Control characters: type them in the terminal (Ctrl+C, Ctrl+D, Ctrl+Z, ESC) or use the
  *Control* quick commands, which send the raw byte.

### SSH sessions

*File > New SSH Session...* (Ctrl+Shift+T, or the toolbar button) opens a tab with an SSH bar
instead of the serial one. Type a target and press Enter or **Connect**:

```
root@192.168.100.2          user@host, port 22
root@192.168.100.2:2222     with a port
ssh://root@10.0.0.24:22     URL form
[fe80::1%eth0]:22           IPv6 in brackets
luckfox                     a Host alias from ~/.ssh/config (user, port, key and ProxyJump apply)
```

- **Host key** - the first connection to a host shows its key type and SHA-256 / MD5
  fingerprints; *Connect and remember* appends it to `~/.ssh/known_hosts` (the same file the
  `ssh` command uses, so a host you trusted in a terminal is silently accepted here and vice
  versa). If a known host suddenly presents a different key you get the red *REMOTE HOST
  IDENTIFICATION HAS CHANGED* warning; a reflashed dev board is the usual reason, and *Replace
  key and connect* (after ticking the checkbox) updates the stored key.
- **Authentication** - automatic order: ssh-agent (Linux / macOS), the profile's key file,
  `~/.ssh/id_ed25519`, `id_ecdsa`, `id_rsa`, then a password or keyboard-interactive prompt. A
  key passphrase or password is asked in a dialog (three attempts); tick *Remember* and it is
  stored for that target (`user@host:port`) or that key file, so the next connection - profile or
  ad-hoc - needs no typing. On Windows the secret is encrypted with DPAPI for your user account,
  elsewhere it is only obfuscated and the dialog says so.
- **Profiles** - the gear button or *Edit > SSH Profiles...* keeps named targets with the
  authentication method, key file, saved password, a remote command instead of the shell, a
  startup command typed after login (`cd /oem && ls`), terminal type, keep-alive and timeout,
  compression, a *ProxyJump* host, an alternative `known_hosts` file and local port forwards
  (`8080 -> 127.0.0.1:80` makes the board's web UI reachable at `localhost:8080` while the
  session is open). Import / export as JSON (secrets are never exported). The list in the bar
  shows your profiles followed by the last ten ad-hoc targets.
- **In the session** - everything a serial tab offers: quick commands, the command input line,
  hex view, logging, log replay, mark mode, right-click paste. The shell learns the terminal
  size automatically (no `stty` needed; *Sync Terminal Size* re-sends it). A keep-alive every
  30 s detects a dead link; when the link drops (board rebooted, Wi-Fi hiccup) the tab shows
  `connection lost` and reconnects with backoff, exactly like a re-plugged serial adapter,
  using the saved credentials only - a clean `exit` closes the tab's connection without
  reconnecting.
- **Files** - *Session > Upload File to Remote...* / *Download File from Remote...* transfer a
  file over the session's SFTP channel with progress and cancel; dropping a file on an SSH
  terminal opens the upload dialog with that file. A board whose SSH server has no SFTP
  (dropbear on a buildroot image) works too: the transfer falls back to plain shell commands
  over a separate exec channel (`cat > file`, `cat file`, `wc -c`, `test -e`) and the dialog says
  "via shell". *Send File* still exists and pastes the file into the shell like on a serial port.
- Session tabs are restored on the next start with their target selected, never auto-connected.
  From the command line, `BuildAI-SerialUtility --ssh root@192.168.100.2` opens an SSH tab with
  that target (add `--connect` to connect immediately).
- *Preferences > SSH* holds the defaults used when a profile or ad-hoc target leaves a field
  empty: known_hosts file, identity file, terminal type and keep-alive interval.
- Windows limitation: libssh has no ssh-agent support on Windows, so use key files (or Pageant's
  keys exported to files) there; on Linux the agent from `SSH_AUTH_SOCK` is used.

### Firmware tab (flash the Node board)

*File > Firmware Update* (**Ctrl+Shift+U**) opens the Firmware tab, one instance next to the
session tabs (a second invocation selects it). It replaces the old routine - a Python SFTP puller,
Rockchip's GUI flashing tool and a USB-serial adapter plus `fw_update.py` for the RF MCU - with
one tab, and everything runs over the Node board's single USB-C cable: the running firmware
shows up as an **ADB** device (USB 2207:0006), `adb reboot loader` puts the SoC into the U-Boot
**Loader** (rockusb) mode that `upgrade_tool` flashes, and the CC1310 is reached through the SoC
(`adb forward` to an `rfmcu bridge` on the board). No vendor GUI, no serial adapter, no MASKROM
button. The tab has three sections and a log pane:

- **Source** - where the image comes from. *Build machine* is an SSH profile, a recent target or
  a free `user@host[:port]` (the same prompts as an SSH tab: host key, password / passphrase,
  *Remember*); *Remote folder* defaults to `~/rk/rockchip-platforms/output/image` and is
  remembered per target; *Local cache* is where the images are kept. **Pull** shows the remote
  `update.img` (size, time, `git describe` of the SDK) and downloads it unless the cached copy
  has the same size and time; the download goes to a `.part` file, is checked for the RKFW magic
  and only then replaces the cached image, which becomes the *previous* one (**Roll back** swaps
  them, so a bad build is one click away from the last good one). **Use local image...** takes any
  `*.img` file instead - the offline route.
- **SoC** - a status line refreshed every 2 s tells what the PC sees: *ADB device <serial>*,
  *Loader mode*, *Maskrom mode*, *no board*, or - when a Rockchip USB device is plugged in but no
  driver claims it - the hint to install the drivers. The image line shows what `upgrade_tool`
  says about the selected image (chip, build time, entries); its tooltip adds the image's RKFW
  header (chip tag, version, build time) and the image's profile.

  *Board profile* (v0.8.0) - the tool tells the two boards apart: the **Node** (RV1106BG2) and the
  **Stacked core** (RV1106G2). Next to the status line the *Board profile* box (*Auto-detect*,
  *Node (RV1106BG2)*, *Stacked (RV1106G2)*; remembered) and a label with what was detected and why:
  *Profile: Node (RV1106BG2) - from the board's flash layout (7 partitions: ...)*, *... - from the
  board's Loader PID 0x110c*, *... - from the ADB gadget's product string ...* or *Profile: unknown*.
  Leave the box on *Auto-detect* unless the tool cannot tell the board; a box that contradicts the
  Loader PID the board shows fails the flash with *the --profile ... override contradicts the
  board's Loader PID ...; drop --profile* (set the box back to *Auto-detect*). When a new image is
  planned and the board's profile is known, the partition table checks the profile's daily
  partitions only - *rootfs* on the Node, *boot* on the Stacked core (its rootfs lives inside a
  12 MiB boot) - instead of every row; *All* / *None* and your own checks rule afterwards.

  *Partitions* (v0.5.1) lists what the image holds, as SocToolKit's Download page does: the tab
  unpacks it (`upgrade_tool EXF`, into `<image folder>/update.extracted`) and shows one row per
  partition - a checkbox, the name, the image file, the flash offset, the partition size and the
  image size. The offsets come from the flash layout (an `mtdparts=` string). First choice is the
  image's own: its `env.img` (the U-Boot environment written with it) starts with the layout the
  build was made for, and that is the layout the board uses after the flash. An image without one
  falls back to the board's: the tab reads `/proc/cmdline` once from a running board it sees on ADB
  and remembers it per board, and every flash reads it again before `adb reboot loader`. When both
  are known and differ (a build with a changed partition layout), the flash is refused unless
  --force-layout (the tab asks); the log shows both. The label ends with where the layout came
  from (*layout from the image* / *layout from the image (the board's differs)* / *layout from the
  board*); its tooltip shows the string. A row the layout does not have, or whose image
  is larger than its partition, is unchecked and disabled with the reason as its tooltip; with
  neither layout the label says *partition layout unknown: connect the board running once*.
  **All** / **None** set the checkboxes; the label counts the checked rows and their size.

  **Flash SoC** gets the board into download mode in this order: already Loader / Maskrom (also
  the recovery path for a board that does not boot) -> ADB device (`adb reboot loader`) -> a
  connected serial or SSH tab (the tab asks, every time, whether to send `reboot loader` through
  it) -> otherwise it tells you to hold MASKROM and plug the board in. It then waits (30 s by
  default) for Loader / Maskrom and writes:
  - **Loader mode** (the normal case, after `adb reboot loader`): the checked partitions, one
    `upgrade_tool WL <offset / 512> <size / 512> <image>` each in the image's order, then `RD` to restart the
    board - the commands SocToolKit's Download page runs. Every image is checked against its
    partition before the first byte is written; a write that fails stops the flash with
    upgrade_tool's last lines. This runs no chip check, so it works with the Node board's U-Boot.
  - **MASKROM mode**: the whole image with `upgrade_tool UF` as before (the partition checkboxes
    do not apply; the log says so), requiring the line `Upgrade firmware ok`.

  The progress bar follows the bytes written (or UF's own percentage); afterwards the tab waits
  for the board to come back on ADB (60 s) and shows its kernel version and uptime. **Cancel**
  works while waiting, never while `upgrade_tool` writes the flash.

  Before anything is written (and before `adb reboot loader`) the board profiles' rules run
  (v0.8.0): an image built for the other board is refused (its RKFW chip tag - 110E the Node,
  1106 the Stacked core - against the board's profile); an image whose flash layout differs from
  the board's asks *Write anyway?* (**Write the image's layout** / **Cancel**, Cancel is the
  default); idblock, uboot, env or meta on the Stacked core - which has no documented MASKROM
  entry, so a bad loader write could not be recovered - ask *Write them anyway?*. Two devices in
  download mode at once refuse a single-board flash (it writes unaddressed); flash them as a fleet.
- **MCU (CC1310)** - pick a firmware package (`firmware-x.y.z.pkg`); the header and both CRC32s
  are checked and target, version and size are shown (a bad file is refused with the reason).
  **Read** asks the running MCU for its version and role through ADB; the role (**rx** or **tx**)
  is pre-selected from what the MCU reports and **you confirm it** - the tool never guesses
  silently. **Update MCU** reboots the CC1310 into its UART updater, sends the package (progress
  per acknowledged block), waits for the restart banner and shows the new version; **Set role**
  changes only the role. The updater protocol (COBS framing, CRC16, the `fw_update.py` state
  machine and its retries) is implemented natively in C++ - nothing runs Python. The MCU path
  needs `rfmcu` on the SoC, so flash the SoC first. On the Stacked core the CC1310 holds the
  SoC's power (SOC_PWR_EN): **Update MCU** and **Set role** ask *Proceed?* first - confirm only
  once the carrier's CC1310 bootloader is known to hold it (the 14 Oct 2026 build or later).
- **Pull & Flash** - pull (skipped when unchanged) -> Flash SoC with every partition checked (in
  Loader mode it stops before writing anything when one image does not fit) -> wait for ADB -> if
  a package is selected and its version differs from the running MCU version, Update MCU. It stops
  at the first failure and says which step it was.
- **Log** - every command line that runs, the tools' output and the steps' narration, time
  stamped and without secrets (also in the System Log); *Save...* writes it to a text file.

While a flash runs the tab cannot be closed (the status bar says why) and quitting asks first;
session actions such as Connect or Send File are disabled while the Firmware tab is the current
tab. The tab is not restored on the next start like a port is.

**Why Loader mode writes partitions (0.5.1).** `upgrade_tool UF` (Windows v2.37 and Linux v2.26
alike) checks the chip id the loader reports against the one in `update.img` ("110E" for the
RV1106B) before it writes anything, and the U-Boot of rockchip-platforms up to `v1.3.0` answers
that question with zeros in Loader mode: `UF` stops at `Check Chip Fail` (the first real run on
2026-10-13 ended there). Writing partition by partition with `WL`, as SocToolKit does, has no such
check, so the tab flashes these boards in Loader mode without any change to them. The one-line
U-Boot change in `docs/firmware-update/uboot-rockusb-chip-info.patch` (the rockusb chip-info
answer) is optional now: it only makes `UF` itself, and other tools that check the chip, work in
Loader mode too.

**Drivers (Windows).** The PC needs the Rockchip USB drivers once: the ADB driver for the running
board and the rockusb driver for Loader / Maskrom mode. When a 2207:xxxx device is on the bus and
neither `adb` nor `upgrade_tool` sees it, the SoC status line says so and **Install drivers**
starts the bundled DriverAssistant (a GUI installer you click through); the Windows
installer also offers it as an optional task. By hand: `tools\DriverAssistant\DriverInstall.exe`.

**Where the tools live.** In `tools\` next to the executable (see *Deploying*). The tab uses, in
this order, the path set in *Preferences > Firmware*, the bundled copy, and - for `adb` only -
the first `adb` on `PATH`. *Preferences > Firmware* also holds the Loader / ADB wait times, the
cache folder and whether to run `git describe` on the build machine. On Linux `upgrade_tool` needs
a udev rule to open the USB device without root (then re-plug the board):

```bash
sudo cp tools/88-rockusb.rules /etc/udev/rules.d/ && sudo udevadm control --reload-rules
```

**Node quick commands.** The first quick-command group, *Node*, belongs to this board:
`reboot loader` (on the SoC's Linux console: reboot into the download mode, the manual
alternative to the tab's automatic step), `rfmcu console` (open the CC1310's CLI through the SoC)
and, once inside it, the CC1310 commands - `help`, `version`, `sys`, `stack`, `uart`, `power`,
`bat`, `rx status`, `rx dump on|off`, `tx status`, `bootloader` (reboot the CC1310 into its UART
updater) - and `~.`, which leaves the `rfmcu console` again. Their tooltips carry the usage.

### Command line (`su-fw`)

`su-fw` (`su-fw.exe`) is the Firmware tab without a window, for a command shell, scripts and AI
agents that update a board in the background. It is installed next to `BuildAI-SerialUtility` in
every package (the same `bin/` folder, the same `tools/`) and runs the very same code as the tab
(`FirmwareJob`): the same steps, tools, rules and messages. The log goes to stdout, the result is
the exit code. It uses the app's settings - the SSH profiles and remembered passwords,
*Preferences > Firmware* (tool paths, cache folder, Loader / ADB waits) and the flash layouts
remembered per board - and writes back only a board's flash layout once it has read one, as the
tab does. One firmware operation runs at a time on a PC: su-fw and the Firmware tab share a lock
(`firmware.lock` in the app's data folder), so while one of them pulls, flashes or talks to the
CC1310, the other's operation is refused with
`another firmware operation is running (<program>, pid <n>)` (su-fw exits 1) and its board status
pauses.

| Command | What it does |
|---|---|
| `su-fw status [--serial S] [--mcu] [--profile ID]` | The board the PC sees: *ADB device ...*, *Loader mode*, *Maskrom mode*, *no board* or the driver hint; a running board's flash layout; (0.8.0) the board's profile with its evidence; `--mcu` adds the CC1310's version and role |
| `su-fw partitions [--image F] [--layout M \| --serial S] [--profile ID]` | The partition table of an image (name, file, offset, partition size, image size, fits, why a row cannot be written) - to choose before flashing; (0.8.0) the image's RKFW header and profile; the board is not touched |
| `su-fw pull [--target T] [--remote-folder F] [--cache D] [--identity K] [--password-env VAR \| --password-stdin] [--accept-host-key]` | Pull `update.img` from the build machine (skipped when unchanged, the previous one kept) |
| `su-fw flash-soc [--image F] (--all \| --partitions boot,rootfs) [--serial S] [--layout M] [--no-wait] [--profile ID] [--force-layout] [--force-loader-partitions]` | Flash the SoC: Loader mode writes the chosen partitions with `WL`, MASKROM mode the whole image with `UF`; a running board gets `adb reboot loader` without a question |
| `su-fw flash-soc (--serials S1,S2,... \| --all-boards) --partitions rootfs [--image F] [--max-parallel N] [--dry-run] [--layout M] [--no-wait]` | (0.7.0) Flash several boards at once: each one's USB port is learned, then the named partitions are written on up to N boards in parallel, every `upgrade_tool` addressed with `-s <LocationID>`; a table per board at the end (see *Flashing several boards*) |
| `su-fw mcu-info [--serial S] [--profile ID]` | The running CC1310's version and role |
| `su-fw mcu-update --package P --role rx\|tx [--serial S] [--profile ID] [--confirm-soc-power]` | Update the CC1310 (the role is never guessed) |
| `su-fw mcu-set-role --role rx\|tx [--serial S] [--profile ID] [--confirm-soc-power]` | Set the CC1310's role |
| `su-fw pull-flash [pull options] [--package P] [--role rx\|tx] [--serial S] [--layout M] [--profile ID] [--force-layout] [--force-loader-partitions] [--confirm-soc-power]` | The tab's Pull & Flash: pull, flash every partition, wait for ADB, update the CC1310 when the package's version differs; stops at the first failure |
| `--profile ID` / `--force-layout` / `--force-loader-partitions` / `--confirm-soc-power` | (0.8.0) The attached board when the tool cannot tell it (`node`, `stacked`; anything else is usage error 2) / write an image whose flash layout differs from the board's / write idblock, uboot, env or meta on the Stacked core / update or re-role the Stacked core's CC1310 - see *Board profiles* below |

Every command also takes `--tools DIR` (default: the app's tool paths, then `tools/` next to
su-fw), `--log FILE` (append every line, also with `--quiet`), `--json`, `--quiet` (only the
result line), `--timeout S` and `--yes` (never ask: an unknown host key or a password prompt is
refused - also what happens when stdin is not a terminal). `su-fw --help` and
`su-fw <command> --help` explain every option and the exit codes, with examples.

**Exit codes**: `0` done; `1` the operation failed (the last log lines say why); `2` usage (the
message on stderr); `3` no board, or the board in a mode the command cannot use (`mcu-info` in Loader mode);
`4` SSH (unreachable, host key refused, authentication failed); `5` `--timeout` or Ctrl+C (also
SIGTERM, SIGHUP, Ctrl+Break). Ctrl+C ends the waiting phases, never a flash write in progress:
su-fw then stops at the next safe point, and when that is the wait for ADB after the write the
result says `interrupted after the image was written; the ADB wait was cancelled`. The tools run
without su-fw's console (Windows) and in a process group of their own (Linux), so no Ctrl+C,
Ctrl+Break, closed console window or terminal, or signal sent to su-fw reaches upgrade_tool. A
harness that kills the whole su-fw process tree on its own timeout still cuts a write: its timeout
must exceed `--timeout` plus the longest write (about two minutes for a full image). `--timeout` is
no hard limit either: a write in progress is waited for, and a silent upgrade_tool is never killed.

**Output**: one line per event, `HH:MM:SS text` - the tab's log lines and progress every 10 % - and
last `su-fw: exit <code> (<meaning>): <message>`. With `--json` one object per line:

```json
{"time":"2026-10-13T18:42:01.123+08:00","step":"flash","event":"progress","text":"Flashing SoC: Writing boot (5 of 7) 42 %","percent":42}
{"time":"2026-10-13T18:42:30.004+08:00","step":"flash-soc","event":"done","text":"Upgrade firmware ok; board is back on ADB","ok":true,"exit":0,"boardInfo":"Linux version ...","image":"...","method":"partition-writes","partitions":["boot","rootfs"]}
```

`event` is `log`, `progress` (with `percent` when there is one), `step` (a step ended, with `ok`) or
`done`: the last line, with `ok`, `exit` and the command's results (`board`, `layout`, `mcu`,
`partitions`, `image`, `boardInfo`).

**Secrets** are never taken from the command line: `--password-env VAR` (the value of the variable),
`--password-stdin` (the first line of stdin), `--identity FILE`, or the password a profile saved in
the app (*Remember*); nothing typed as a secret is ever printed, and the `--password-env` variable is
removed from su-fw's environment at once, so no tool inherits it. A host key that is not known yet is
accepted only with `--accept-host-key` (and then remembered in known_hosts); a changed one is
refused. On a terminal without `--yes` su-fw asks instead (the host key; the password without echo).

**Agent recipe** - check the exit code, read the JSON `done` line:

```powershell
su-fw status --mcu --json                       # 0: a board (and the CC1310) answered; 3: no board
su-fw partitions --image update.img --json      # the rows, their offsets, whether they fit
$env:SU_PASSWORD = '...'
su-fw pull-flash --target builder@10.0.10.71 --password-env SU_PASSWORD --accept-host-key `
      --package firmware-0.8.1.pkg --json --timeout 900   # the harness's own timeout: 900 s + 2 min or more
```

`pull-flash` always writes every partition, like the tab's Pull & Flash; for some partitions run
`su-fw pull ...`, then `su-fw flash-soc --partitions boot,rootfs`. su-fw prints English only (its
output is read by scripts); `QT_LOGGING_RULES="buildai.*=true"` adds the internal diagnostics on
stderr.

**Board profiles (v0.8.0).** su-fw knows two boards and tells them apart by evidence, never by
guessing:

- **Node** (`node`, RV1106BG2): RKFW chip tag `110E`, U-Boot Loader PID `0x110e`, 7 partitions
  (`...,3M(boot),5M(rootfs),3M(userdata)`), a daily update writes `rootfs`, MASKROM button.
- **Stacked core** (`stacked`, RV1106G2): chip tag `1106`, Loader PID `0x110c`, 6 partitions with
  the erofs rootfs inside a 12 MiB `boot` (`...,12M(boot),3M(userdata)`), a daily update writes
  `boot`; no documented MASKROM entry; its CC1310 holds the SoC's power (SOC_PWR_EN).

The board's profile comes from `--profile` (when given), its Loader PID (a board in Loader mode;
a MASKROM device's PID is the BootROM's and is no evidence), the ADB gadget's product string (read
with `adb shell cat /sys/kernel/config/usb_gadget/rockchip/strings/0x409/product` only when the
board's `/proc/cmdline` layout tells nothing) or its flash layout; the image's from its RKFW header
(no tool runs for it) or its `env.img` layout. Both are logged with their evidence -
`profile: Node (RV1106BG2) - from the board's flash layout (7 partitions: ...)`,
`image profile: Stacked (RV1106G2) - from the image's chip tag 1106` - and an unknown profile makes
the rules that need it inert (logged as `profile: unknown (...)`). Four refusals, each exit 1 with
its text as the result:

1. an image for the other board (no override: a wrong image is never written);
2. an image whose flash layout differs from the board's - `--force-layout` writes it anyway (the
   0.5.1 "the image's wins" is gone);
3. idblock / uboot / env / meta on the Stacked core - `--force-loader-partitions`;
4. `mcu-update` / `mcu-set-role` / pull-flash's MCU step on the Stacked core -
   `--confirm-soc-power`, once its CC1310 bootloader is known to hold SOC_PWR_EN (14 Oct 2026 or
   later).

`--profile` that contradicts a Loader PID the board shows is refused too (`... drop --profile`), and
a fleet stops before anything is written when a board's profile differs from the plan's
(`mapping stopped: mixed bench: ...`; name boards of one profile with `--serials`). The JSON:
`status`'s `board.profile` = `{"id","name","source","evidence"}` (source `option` / `loaderPid` /
`adbProduct` / `boardLayout`; all `""` when unknown); `partitions`, `flash-soc` and `pull-flash`
carry `imageHeader` (`chipTag`, `version`, `buildTime`) and `imageProfile`; a fleet's
`boards[].profile` is the id. Agent recipe:

```powershell
su-fw status --json                               # board.profile.id: "node", "stacked" or ""
su-fw partitions --image update.img --json        # imageProfile.id must equal board.profile.id
su-fw flash-soc --image update.img --partitions boot --json                      # the Stacked core's daily update
su-fw flash-soc --image update.img --partitions boot --profile stacked --json    # only when status said ""
```

#### Flashing several boards (`--serials` / `--all-boards`, v0.7.0)

`su-fw flash-soc --serials F=c4130950110960e0,G=5a33c81a6e996266 --partitions rootfs` (or
`--all-boards`: every board `adb devices` lists as `device`) flashes a bench of Node boards in about
the time of one. ADB cannot tell which USB port a serial is on, so the port is **learned by
difference, one board at a time**: a picture of the bus (the Rockchip devices Windows enumerates and
`upgrade_tool LD`'s list), `adb -s <serial> reboot loader`, and new pictures until exactly that
board's ADB device vanished and one Loader device with one new LocationID appeared on the same
port - anything else (two boards changing at once, a Loader device on another port, a device nobody
can name) stops the run before a byte is written. Then the named partitions are written on up to
`--max-parallel` boards at once (default: all), every `upgrade_tool` command addressed with
`-s <LocationID>` and every board re-checked before each write; each board is reset with
`upgrade_tool -s <LocationID> RD`, waited for on ADB, and its kernel, uptime and `rfmcu status` are
printed. The rules: only `--partitions` (`--all` is refused: idblock, uboot, env and meta are
written only when named, with a warning line); a board that does not show up in Loader mode on its
port in time is skipped and reported, never guessed; a Loader / MASKROM device on the bus that no
selected board accounts for refuses the run (the message names the `upgrade_tool -s <n> RD` that
clears it); `--all-boards` refuses next to a non-Rockchip ADB device (a phone) and skips a board
whose `/proc/cmdline` reports no flash layout. A write is never cut: Ctrl+C or `--timeout` skips and
resets the boards not started, while a board being written finishes all its partitions and its reset
(exit 5). The learned ports are remembered per board (`--dry-run` shows them as "expected port", a
real run notes a board that moved); they never address a board.

**Exit 0 means every selected board is back on ADB** (with `--no-wait`: reset). `1` a board was
skipped, failed or did not come back (or the run stopped / could not start), `2` usage, `3` no
selected board is on ADB, `5` `--timeout` / Ctrl+C. The output has one line per board and event
(`HH:MM:SS [F c4130950110960e0] in Loader mode on port 6&3A944CE5&0&1, LocationID 112 (1.2 s)`),
progress every 10 % per board, and last a table (board, serial, port, LocationID, result, mapping /
writing / total seconds, detail). With `--json` a board's lines carry `board`, `tag` and `stage`, each
board ends with a `step` object (`result`, `seconds`, `written`, `retry`, `adbBack`), and the `done`
object carries the whole record:

```json
{"time":"...","step":"flash-soc","event":"done","text":"2 of 2 boards flashed in 41.0 s (mapping 3.1 s, writing 29.8 s)","ok":true,"exit":0,
 "fleet":true,"dryRun":false,"image":"C:\\...\\update.img","partitions":["rootfs"],"maxParallel":0,"seconds":41.0,"mappingSeconds":3.1,
 "writingSeconds":29.8,"stop":"","unattributed":[],"summary":{"selected":2,"done":2,"notBack":0,"failed":0,"skipped":0},
 "boards":[{"tag":"F","serial":"c4130950110960e0","adb":"device","expectedPort":"","port":"6&3A944CE5&0&1","loader":"112","stage":"finished",
            "result":"done","ok":true,"seconds":38.2,"mappingSeconds":1.2,"writingSeconds":29.4,"detail":"","written":["rootfs"],"retry":[],
            "adbBack":true,"leftInLoader":false,"kernel":"5.10.209+","uptime":"up 0 min","boardInfo":"Linux version 5.10.209+ ...",
            "rfmcu":"pid=412 uart=owned mcu=alive"}, ...]}
```

**Agent recipe**: look first, then two boards, then all of them; read `done.boards[]`, and tell
whoever else uses the boards when you start and when you are done.

```powershell
su-fw flash-soc --all-boards --partitions rootfs --image update.img --dry-run --json   # no reboot, no write
su-fw flash-soc --serials F=c4130950110960e0,G=5a33c81a6e996266 --partitions rootfs --image update.img --yes --json
su-fw flash-soc --all-boards --partitions rootfs --image update.img --yes --json --log fleet.log
```

**A board left in Loader mode** (a write that failed - after a hub brown-out several at once) is
reported with `leftInLoader` and the exact commands that finish it, addressed to it alone: run the
`retry` lines of its board object (the log's `  retry: ...` lines) as they are - one
`upgrade_tool -s <LocationID> WL <sector> <count> <file>` per missing partition, then
`upgrade_tool -s <LocationID> RD`. Each line names the `upgrade_tool` su-fw ran by its full path
(the bundled `tools\upgrade_tool.exe` is not on PATH), quoted when the path holds a blank: it runs
as it is in `cmd.exe`; in PowerShell put `& ` in front. Use `su-fw flash-soc --serial X` for it
**only once `upgrade_tool LD` lists X as the only device in download mode**: the single-board flow
writes without `-s` and takes any Loader device. When the LocationID is gone (`No found specific
rockusb device!`, a WL's or an RD's) the board's line says so: find it with `upgrade_tool LD` first
and address it with `-s` - unless the run's closing picture shows that board's Loader device on its
port with the only new LocationID on the bus: then the board takes that id and its `retry` lines are
addressed to it. A board a stop or a cancel reset that is still in Loader mode at the end of the run
(its RD did not take) gets `leftInLoader` and the RD line alone.

### Quick commands

*Edit > Quick Commands...* opens the editor. Each row has a name, the command text, a group
(the quick command bar below the terminal shows one group at a time, chosen in its group
selector; *All* shows every command), the line ending appended, a **HEX** flag (the text is
parsed as hex bytes, no line ending), an **Escapes** flag (`\n`, `\r`, `\t`, `\xHH`, `\uHHHH`),
an optional shortcut and a tooltip. *Import...* / *Export...* exchange the JSON file with
colleagues; *Restore Defaults* brings back the Node / Linux / U-Boot / MCU / Control starter set.
The list is stored at `AppSettings::dataDirectory()/quick_commands.json` (file version 2).
When 0.5 starts with a `quick_commands.json` written by an earlier release (version 1) that
has no *Node* group, the group is added in front of your commands once and the file is saved as
version 2; nothing else in it changes, and a group you delete later stays deleted.

### Sending a file

*Session > Send File...* (**Ctrl+Shift+O**) or drop a file onto the terminal. **Text mode** sends the
file line by line, strips the file's own line endings and appends the chosen one, waiting
(default 50 ms) between lines - the safe way to paste a shell script or a `setenv` block into a
console that cannot keep up with a raw paste. **Binary mode** sends fixed-size chunks with a
delay. The dialog is modeless: watch the terminal while it sends, pause / resume, cancel. The last
options are remembered.

### Logging

*File > Start Logging* creates `<log dir>/<port>_<yyyy-MM-dd_HH-mm-ss>.log` in the format
chosen in *Preferences > Logging* (raw bytes, timestamped text with optional `TX>` lines, or hex
dump). The status bar shows `● LOG` while active; *File > Open Log Folder* opens the directory.
Enable *Start logging automatically when a session connects* to never miss a boot log.

### Replaying a captured log

*File > Replay Log File...* picks a file and a speed (9600 baud ... 1500000 baud, or
unlimited) and streams it through the current tab as if the device were sending it - the
terminal renders colours and cursor movement, the hex view and an active session log receive
the bytes too. Raw captures are streamed byte for byte; a timestamped text capture (the
default logging format) is recognised from its first line: the `[timestamp] ` prefixes and the
`TX> ` lines (your own input) are removed and every line ends in CR LF again. *File > Stop
Replay* aborts. From the command line: `BuildAI-SerialUtility.exe --replay boot.log --speed 1500000`
(`--speed 0` = as fast as possible). Replay only runs while the tab is disconnected - it never
writes to the port, a request while a port is open is refused in the status bar, and connecting
while a replay streams stops the replay first; while it streams, the tab is titled
`Replay: <file>`.

### Hex view

*View > Hex View* (**Ctrl+Shift+H**) replaces the terminal of the current tab with a timestamped
RX / TX hex dump. The view is bounded so it stays responsive during long sessions.

### Copying text (pause output while selecting)

Select with the mouse as usual: click-drag, double-click a word, triple-click a line, Shift+click
to extend, *Edit > Select All*, or *Find* (**Ctrl+Shift+F**). Because a boot log or a telemetry
stream would keep scrolling under your selection, the terminal behaves like the Windows console
(cmd.exe QuickEdit / mark mode) by default: the moment a selection appears the display
**freezes** and every byte that arrives is queued instead of drawn - a translucent badge in the
top-right corner ("Output paused  12.3 KB waiting  Enter: copy  Esc: cancel") and the status bar
tell you so. The port, the session log and the hex view are not affected; only the drawing waits.

- **Enter** copies the selection to the clipboard, clears it and resumes: the queued output is
  rendered at once, exactly as if you had never paused.
- **Esc** cancels without touching the clipboard.
- **Ctrl+Shift+C**, **Ctrl+Insert**, **Ctrl+C** (while text is selected) and the context menu's
  *Copy* copy and resume too; a plain click resumes.
- A **right click** copies the selection as well (and resumes). With nothing selected it
  **pastes** the clipboard instead - cmd.exe QuickEdit style, so a boot-log session works like the
  Windows console: select, right-click to copy, right-click again to paste. The context menu
  (Copy, Paste, Select All, Clear Screen (keep scrollback), Clear Scrollback, Reset Terminal, Sync
  Terminal Size, Find...) is on **Shift+right click**, the Menu key or **Shift+F10**. Uncheck
  *View > Right Click Pastes (cmd.exe style)* (or *Preferences > Terminal*) to get the menu back
  on a plain right click.
- Every other key and any paste is ignored while paused (nothing reaches the device), except the
  application shortcuts (tabs, connect / disconnect, hex view, find, ...). Scrolling with the
  wheel, the scrollbar or **Shift+PageUp / PageDown** and zooming keep working.
- The queue holds up to 64 MB; beyond that the display resumes by itself (the selection stays
  so you can still copy it) and nothing is lost.

Uncheck *View > Pause Output While Selecting* (or *Preferences > Terminal > Pause output while
selecting text*) to get the classic behaviour: the output keeps flowing under the selection and
Enter is sent to the device.

### Clearing the terminal

*Clear* (the toolbar button next to Disconnect, *Session > Clear*, **Ctrl+Shift+L**) wipes
everything: the screen, the scrollback and the hex view. No previous text remains, the cursor
goes home, and the emulator keeps its state (colours, modes) - *Session > Reset Terminal* is the
full VT reset that also drops those. Pressed while `top`, `vi` or `menuconfig` is running, Clear
blanks the program's screen (it redraws itself) and the shell screen hidden behind it, so nothing
old reappears when the program exits. The terminal's context menu keeps the finer-grained
*Clear Screen (keep scrollback)* (the screen moves into the scrollback, like Ctrl+L in a shell)
and *Clear Scrollback* (the history only). While the display is paused for a selection, Clear
applies first and the queued output then continues on the empty screen.

### Keyboard shortcuts

| Shortcut | Action |
|---|---|
| Ctrl+T / Ctrl+Shift+W | New session tab / close tab (Ctrl+W goes to the device: it deletes a word in a shell) |
| Ctrl+Shift+T | New SSH session tab |
| Ctrl+Shift+U | Firmware tab (File > Firmware Update): pull, flash the SoC, update the CC1310 |
| Ctrl+Tab / Ctrl+Shift+Tab, Ctrl+PageDown / Ctrl+PageUp | Next / previous tab |
| Alt+1 .. Alt+9 | Select tab 1..9 (also in the *Window* menu); a middle click on a tab closes it |
| Ctrl+Shift+B | Detect the serial baud rate |
| F2 / F3 | Connect / disconnect |
| F5 | Refresh port list |
| Ctrl+Shift+L | Clear (screen, scrollback and hex view) |
| Ctrl+Shift+O | Send file... |
| Ctrl+Shift+H | Toggle hex view |
| Ctrl+Shift+C / Ctrl+Shift+V | Copy / paste (plain Ctrl+C / Ctrl+V go to the device) |
| Enter / Esc while the output is paused | Copy the selection and resume / cancel the selection and resume (see *Copying text*) |
| Right click | Paste the clipboard, or copy the selection when text is selected (cmd.exe style; *View > Right Click Pastes*) |
| Shift+right click, Menu key, Shift+F10 | Terminal context menu |
| Middle click | Paste |
| Ctrl+Shift+F | Find in scrollback |
| Ctrl+Shift+R | Replay log file... |
| Ctrl++ / Ctrl+- / Ctrl+0 | Zoom in / out / reset |
| Ctrl+, | Preferences |
| Ctrl+Shift+Q | Quit |
| Shift+PageUp / PageDown, mouse wheel | Scroll the scrollback |

Every shortcut above is configurable in *Preferences > Keyboard*. While a session is
connected the rule is simple: a key that is one of the application's shortcuts goes to the
application, everything else - Tab, arrows, Home / End, F-keys, every bare Ctrl+letter - is sent
to the device (see `docs/TERMINAL_EMULATION.md` for the exact byte sequences). F2 / F3 / F5 are
therefore application keys only because Connect / Disconnect / Refresh Ports use them; reassign
those actions and the keys reach the shell.

## Architecture

```
MainWindow (tabs, menus, status bar, System Log dock)
  ├── SessionWidget x N   (serial or SSH)
  │     ├── ConnectionBar | SshConnectionBar     port + line parameters | target + profiles
  │     ├── TerminalWidget  ──►  AnsiParser  ──►  TerminalScreen   (or HexDumpView)
  │     ├── QuickCommandBar      QuickCommandStore (shared)
  │     ├── CommandInput         CommandHistory (shared)
  │     ├── Transport            SerialConnection (QSerialPort + auto-reconnect)
  │     │                        | SshConnection (libssh on a worker thread, SFTP, forwards)
  │     └── SessionLogger
  └── FirmwareTab (v0.5, one)
        └── FirmwareJob (v0.5.1, UI-free: the chain)
              ├── ImagePuller          SshConnection: stat, download, history
              ├── SocFlasher           RockchipTool (upgrade_tool LD / EXF / WL / RD / UF) + AdbTool, UsbProbe
              └── McuLink + McuUpdater adb forward -> rfmcu bridge -> CC1310 (CcProtocol, CcPackage)
Dialogs: About, Version, Preferences, QuickCommands, SendFile (FileSender),
         HostKey, AuthPrompt, SshProfiles, RemoteFile (SFTP)

su-fw (v0.6, console)  FirmwareCli ──► FirmwareJob (the same chain as the tab)
```

CMake targets: `su_core` (static library: `src/app`, `src/core`, `src/ssh`, `src/firmware`,
`src/terminal` minus the widget; UI-free, unit-tested), `su_app` (static library: every widget and
dialog, linked by the GUI test suites), `SerialUtility` (`main.cpp` + `su_app` + resources) and
`su_fw` (the `su-fw` console program: `src/cli/main.cpp` + `su_core`), plus libssh built from
source. Everything runs on the GUI thread except the SSH worker thread
behind each `SshConnection`; `QSerialPort` is asynchronous, rendering is coalesced to 16 ms, and
the external flashing tools run as asynchronous `QProcess` children (never waited for on the GUI
thread).

- [`docs/DESIGN.md`](docs/DESIGN.md) - architecture, conventions, module contracts, work packages.
- [`docs/TERMINAL_EMULATION.md`](docs/TERMINAL_EMULATION.md) - supported escape sequences and key mapping.
- [`docs/QUICKSTART.md`](docs/QUICKSTART.md) - one-page getting started.

## Project layout

```
SerialUtility/
├── CMakeLists.txt, CMakePresets.json
├── scripts/            build.ps1, run.ps1, update-translations.ps1
├── src/
│   ├── app/            AppSettings, Logging, Version.h.in
│   ├── core/           SerialConnection, SerialPortEnumerator, LineEnding, HexUtils,
│   │                   CommandHistory, QuickCommand(Store), FileSender, SessionLogger,
│   │                   DeviceSimulator (SIM: pseudo-ports), Transport (serial | SSH interface)
│   ├── ssh/            SshConnection (libssh worker), SshProfile(Store), SecretStore
│   ├── firmware/       Firmware tab core: ToolRunner / ToolLocator, RockchipTool (upgrade_tool),
│   │                   AdbTool, UsbProbe, SocFlasher, McuLink, McuUpdater, CcProtocol, CcPackage,
│   │                   ImagePuller, FirmwareJob (the chain), FirmwareCli (su-fw's command line)
│   │                   (UI-free, unit-tested with fake tools)
│   ├── cli/            main.cpp of the su-fw console program
│   ├── terminal/       TerminalScreen, AnsiParser, CharWidth, TerminalTheme, TerminalWidget
│   ├── ui/             MainWindow, SessionWidget, ConnectionBar, SshConnectionBar, CommandInput,
│   │                   QuickCommandBar, HexDumpView, SystemLogViewer, FirmwareTab
│   ├── dialogs/        About, Version, Preferences, QuickCommands, SendFile,
│   │                   HostKey, AuthPrompt, SshProfiles, RemoteFile (.ui + .cpp)
│   └── main.cpp
├── third_party/        bundled flashing tools with provenance and licences (README.md): Rockchip
│                       upgrade_tool (Windows, Linux), adb + DLLs, DriverAssistant zip; CMake copies
│                       them to tools/ next to the executable and installs them as bin/tools/
├── tests/              Qt Test suites: unit tests (su_core) and offscreen GUI tests (su_app);
│                       support/TestSshServer - in-process libssh server for the SSH suites,
│                       FakeToolRunner / FakeMcu - scripted tools and a fake CC1310 for the firmware suites;
│                       data/ - known-answer vectors and sample packages (tests/data/firmware/)
├── packaging/windows/  Inno Setup script and licence for the installer
├── translations/       en_US.ts, zh_CN.ts
├── resources/          icons, resources.qrc, resources.rc
└── docs/               DESIGN.md, TERMINAL_EMULATION.md, QUICKSTART.md, firmware-update/
```

Settings are stored under organisation `BuildAI`, application `SerialUtility` (registry on
Windows). Per-user data files (quick commands, history) live in
`QStandardPaths::AppConfigLocation`.

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| "Port COM8 busy or access denied" | Another program (PuTTY, a flashing tool, a previous instance) holds the port. Close it, or unplug / re-plug the adapter. |
| Garbled text, `�` or random symbols | Wrong **baud rate** (try 1500000 vs 115200) or wrong **encoding** (*Preferences > Input*). UTF-8 is right for Rockchip Linux and U-Boot. For legacy Chinese firmware that prints GBK, pick **System** on a Chinese-locale Windows (ANSI code page 936 = GBK), or **GB18030** where the list offers it (only Qt builds with ICU, e.g. most Linux distro packages; the official Windows Qt binaries do not, so the entry is hidden there). *Help > Version* lists the codecs your build actually has. |
| Staircase text (each line starts further right) | Device sends bare `\n`. Turn on *Implicit carriage return on line feed*. |
| Nothing appears after connecting | Board not powered or TX/RX swapped; check the connection bar shows **Connected** and RX counter moves in the status bar. Try DTR / RTS toggles - some adapters hold the board in reset. |
| Backspace prints `^H` / `^?` | Toggle *Backspace sends DEL* in *Preferences > Input*. |
| Enter does nothing / double prompt | Change the line ending: CR for Linux and U-Boot, CRLF for AT-style MCU shells. |
| Colours or `top` look wrong | Try *Session > Reset Terminal* and *Sync Terminal Size* (sends the current rows x columns to the shell via `stty`-compatible resize). |
| Lost characters at 1.5 Mbaud | Use a CH343 / FTDI adapter (CH340 tops out at ~2 Mbaud but drops bytes); avoid USB hubs. |
| Port disappears on every reboot and never returns | Auto-reconnect is off - enable it in *Preferences > Connection* - or the adapter re-enumerates with a different COM number; pick it again. |
| SSH: "REMOTE HOST IDENTIFICATION HAS CHANGED" | The board was reflashed (new host key) or something sits between you and it. If you expect the change, tick the checkbox and *Replace key and connect*; the old line in `~/.ssh/known_hosts` is replaced. |
| SSH: connects but authentication fails with a key | The server does not accept that key (check `authorized_keys` on the board), or the key needs a passphrase you cancelled. Set *Authentication: Public key* with the file in the profile to see libssh's exact error in the System Log. |
| SSH: prompt appears but typing shows nothing | The startup / remote command is still running, or the server did not grant a PTY (some restricted shells); try *Session > Reset Terminal*, then reconnect with an empty remote command. |
| SSH: the app does not start, "ssh.dll" or "libcrypto-3-x64.dll" missing | The portable zip was unpacked incompletely - the two DLLs must sit next to the executable (the installer always places them). |
| Firmware: the SoC line says "no board" | Plug the USB-C cable into the Node board's data port and power it. The running firmware appears as an ADB device; on Windows that needs the Rockchip drivers once (next row). A board that does not boot needs MASKROM: hold the button, plug in. |
| Firmware: the SoC line says a Rockchip device is present but has no driver | Press **Install drivers** (or run `tools\DriverAssistant\DriverInstall.exe`, or tick the installer's driver task), then re-plug the board. |
| Firmware: "board did not enter MASKROM/Loader within 30 s" | `adb reboot loader` did not take: send `reboot loader` from a serial / SSH tab (the *Node* quick command), or hold MASKROM and replug; the wait time is in *Preferences > Firmware*. |
| Firmware: "upgrade_tool did not report success" | The message carries the last lines of its output (also in the log pane). Do not unplug during the write; run Flash SoC again - a board in Loader or Maskrom mode is flashed directly. |
| Firmware: "partition layout unknown" | Loader mode needs a flash layout: the image's `env.img` has none, and no running board told its own yet. Boot the board (replug, or `upgrade_tool RD`), let the SoC line show it as an ADB device once, then flash again - or flash in MASKROM mode, which writes the whole image. |
| Firmware: "... does not fit partition ..." / "writing <name> failed" | An image of the build is larger than its partition in the layout in use (with the board's layout: the build changed it and its `env.img` carries none - flash the whole image in MASKROM mode), or `upgrade_tool WL` failed (its last lines follow; replug the board and flash again - a board left in Loader mode is flashed directly). |
| Firmware: the MCU step says "no rfmcu" or "no ADB board attached" | The CC1310 is reached through `rfmcu` on the SoC, so the SoC image must be `rockchip-platforms` `rv1106-bg2-main` v1.2.0 or later and the board must be back on ADB: flash the SoC first. |
| Firmware (Linux): `upgrade_tool` cannot open the USB device | Install the udev rule: `sudo cp tools/88-rockusb.rules /etc/udev/rules.d/ && sudo udevadm control --reload-rules`, then re-plug. |

The **System Log** dock (*View > System Log*) shows every port open / close / error with a
timestamp; *Help > Version* has a **Copy** button that collects everything needed for a bug
report (versions, compiler, OS, codecs).

## Contributing

- Follow `.clang-format` (LLVM base, 4 spaces, 120 columns, braces on their own line for
  functions and classes) and the conventions in `docs/DESIGN.md`.
- The headers under `src/` are the contract; keep public APIs stable and documented.
- Code must compile warning-free with `/W4` (MSVC) and `-Wall -Wextra -Wpedantic -Wshadow` (GCC);
  CI configures with `-DSU_WARNINGS_AS_ERRORS=ON`, so any warning fails the build.
- Run `scripts\build.ps1 -Config Debug -Test` before pushing; CI builds Ubuntu and Windows on
  every push and publishes a release on `v*` tags.

## License

Copyright 2026 BuildAI - all rights reserved.

Qt is used under the terms of the GNU LGPL v3 (<https://www.qt.io/licensing>). libssh
(<https://www.libssh.org>) is used as a shared library under the GNU LGPL v2.1; OpenSSL
(<https://www.openssl.org>) under the Apache License 2.0.
