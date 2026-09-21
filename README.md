# BuildAI Serial Utility

CI: `.github/workflows/build.yml` builds and tests on Ubuntu and Windows for every push and publishes a release on `v*` tags.

A multi-tab serial-port terminal for the boards BuildAI works with every day: **Rockchip
Linux boards** (U-Boot prompt and Linux console at 115200 or 1 500 000 baud) and **MCU firmware
shells** (STM32 / ESP32 / RP2040 behind CH34x, CP210x, FTDI or J-Link CDC adapters). It replaces
ad-hoc use of PuTTY, minicom and SecureCRT with a tool that logs everything, remembers your
commands, survives board reboots and lives in the same Qt / CMake ecosystem as the team's
other tools.

It intentionally contains **no device-specific protocol logic**: it is a general terminal that
speaks bytes.

## Features

- **Multi-tab sessions** - one port per tab, movable and closable, with a coloured connection
  dot (green connected, amber reconnecting, grey disconnected). Tabs from the previous run are
  reopened on start.
- **Real terminal emulation** - VT100 / xterm subset: cursor movement, erase/insert/delete,
  scroll regions, 16 / 256 / true colours, alternate screen (`top`, `vi`, `menuconfig`),
  bracketed paste, DSR / DA replies, window title. Chinese and other CJK text occupies two
  cells. Unknown sequences are swallowed, never desynchronise the output.
- **Built for boot logs** - handles 1.5 Mbaud output (about 150 KB/s) without freezing the UI,
  with plenty of headroom: the RX pipeline renders a coloured boot log at around 10 MB/s on a
  4-core desktop while the window stays responsive. Repaints are coalesced and drawn from a glyph
  cache, the hidden hex view queues instead of rendering, scrollback is bounded (default 10 000 lines).
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
- **Quick commands** - user-defined macros shown as buttons, grouped (Linux, U-Boot, MCU,
  Control), with optional shortcuts, HEX payloads (`03` for Ctrl+C) and C-style escapes. Import
  / export as JSON.
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
| Qt | **6.8** with modules Core, Gui, Widgets, SerialPort, LinguistTools (Test for the unit tests) |
| CMake | 3.22 or newer (3.25+ for the presets) |
| Generator | Ninja |
| Compiler (Windows) | MSVC 2022 x64 (`D:\Qt\6.8.3\msvc2022_64` is the default Qt prefix) |
| Compiler (Linux) | GCC 12+ or Clang 15+ |
| Linux distro Qt packages | Debian 13+ / Ubuntu 25.10+ (older releases: install Qt 6.8.3 via aqtinstall) |

No third-party libraries beyond Qt are required.

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
  `tst_serialsettings`, `tst_devicesimulator`, `tst_logreplayer`).
- **GUI tests** linked against `su_app`, which drive the real widgets and dialogs
  (`tst_terminalwidget`, `tst_sessionwidget`, `tst_mainwindow`, `tst_dialogs`). They talk to the
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

`cmake --install build --prefix dist` (or `build.ps1 -Deploy`) copies the executable to
`dist/bin` and runs `windeployqt` / the Qt deploy script so the folder is self-contained.
The Linux tarball published by CI is built the same way (`bin/`, `lib/`, `plugins/`, `qt.conf`)
and is self-contained except for the system X11/xcb libraries, which users install via apt:
`sudo apt install libxcb-cursor0 libxkbcommon-x11-0 libgl1`.

### Packaging (Windows zip + installer)

```powershell
.\scripts\package.ps1            # build + test + deploy, then zip and Inno Setup installer
.\scripts\package.ps1 -SkipBuild # package an existing dist\Release\bin
```

Produces `dist\BuildAI-SerialUtility-<version>-windows-x64.zip`,
`dist\installer\BuildAI-SerialUtility-<version>-windows-x64-setup.exe` (needs
[Inno Setup 6](https://jrsoftware.org/isdl.php): `winget install JRSoftware.InnoSetup`) and
`dist\SHA256SUMS.txt`. The installer script lives in `packaging/windows/`.

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

### Quick commands

*Edit > Quick Commands...* opens the editor. Each row has a name, the command text, a group
(the quick command bar below the terminal shows one group at a time, chosen in its group
selector; *All* shows every command), the line ending appended, a **HEX** flag (the text is
parsed as hex bytes, no line ending), an **Escapes** flag (`\n`, `\r`, `\t`, `\xHH`, `\uHHHH`),
an optional shortcut and a tooltip. *Import...* / *Export...* exchange the JSON file with
colleagues; *Restore Defaults* brings back the Linux / U-Boot / MCU / Control starter set. The
list is stored at `AppSettings::dataDirectory()/quick_commands.json`.

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

### Keyboard shortcuts

| Shortcut | Action |
|---|---|
| Ctrl+T / Ctrl+W | New session tab / close tab |
| Ctrl+Tab / Ctrl+Shift+Tab | Next / previous tab |
| F2 / F3 | Connect / disconnect |
| F5 | Refresh port list |
| Ctrl+Shift+L | Clear screen |
| Ctrl+Shift+O | Send file... |
| Ctrl+Shift+H | Toggle hex view |
| Ctrl+Shift+C / Ctrl+Shift+V | Copy / paste (plain Ctrl+C / Ctrl+V go to the device) |
| Ctrl+Shift+F | Find in scrollback |
| Ctrl+Shift+R | Replay log file... |
| Ctrl++ / Ctrl+- / Ctrl+0 | Zoom in / out / reset |
| Ctrl+, | Preferences |
| Ctrl+Shift+Q | Quit |
| Shift+PageUp / PageDown, mouse wheel | Scroll the scrollback |

Everything else - Tab, arrows, Home / End, F-keys, Ctrl+letter - is sent to the device
(see `docs/TERMINAL_EMULATION.md` for the exact byte sequences). Exceptions: F3 and F5 always
belong to the application (Disconnect / Refresh Ports) and are never sent; F2 is Connect while
disconnected and reaches the device (`ESC O Q`) only while connected.

## Architecture

```
MainWindow (tabs, menus, status bar, System Log dock)
  └── SessionWidget x N
        ├── ConnectionBar        port + line parameters + connect
        ├── TerminalWidget  ──►  AnsiParser  ──►  TerminalScreen   (or HexDumpView)
        ├── QuickCommandBar      QuickCommandStore (shared)
        ├── CommandInput         CommandHistory (shared)
        ├── SerialConnection     QSerialPort + auto-reconnect
        └── SessionLogger
Dialogs: About, Version, Preferences, QuickCommands, SendFile (FileSender)
```

Three CMake targets: `su_core` (static library: `src/app`, `src/core`, `src/terminal` minus the
widget; UI-free, unit-tested), `su_app` (static library: every widget and dialog, linked by the
GUI test suites) and `SerialUtility` (`main.cpp` + `su_app` + resources). Everything runs on the
GUI thread; `QSerialPort` is asynchronous and rendering is coalesced to 16 ms.

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
│   │                   DeviceSimulator (SIM: pseudo-ports)
│   ├── terminal/       TerminalScreen, AnsiParser, CharWidth, TerminalTheme, TerminalWidget
│   ├── ui/             MainWindow, SessionWidget, ConnectionBar, CommandInput,
│   │                   QuickCommandBar, HexDumpView, SystemLogViewer
│   ├── dialogs/        About, Version, Preferences, QuickCommands, SendFile (.ui + .cpp)
│   └── main.cpp
├── tests/              Qt Test suites: unit tests (su_core) and offscreen GUI tests (su_app)
├── translations/       en_US.ts, zh_CN.ts
├── resources/          icons, resources.qrc, resources.rc
└── docs/               DESIGN.md, TERMINAL_EMULATION.md, QUICKSTART.md
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

Qt is used under the terms of the GNU LGPL v3 (<https://www.qt.io/licensing>).
