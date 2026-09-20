# BuildAI Serial Utility — Design Specification

**Status:** authoritative spec for the initial implementation (v0.1.0).
**Audience:** implementers of the individual modules and reviewers.
**Companion documents:** `docs/TERMINAL_EMULATION.md` (escape sequences, key mapping),
`README.md` (user-facing).

---

## 1. Purpose

A Qt 6 desktop tool that opens serial ports and acts as an interactive shell/terminal for
the devices BuildAI works with every day:

| Device class | Typical link | What the tool must handle well |
|---|---|---|
| Rockchip Linux boards (RV1106/RV1106B "Luckfox Pico", RK3xxx) | UART debug console, 115200 or **1500000** baud, 8N1 | U-Boot prompt and Linux login shell: VT100 line editing, colours from `ls`/`dmesg`, `\r` progress lines, board reboots (port vanishes / returns), long boot logs, Chinese UTF-8 output |
| MCU firmware shells (STM32/ESP32/RP2040 via CH34x, CP210x, FTDI, J-Link CDC) | 9600–921600, 8N1, sometimes bare `\n` | Bare-LF output, AT-style CR/LF commands, hex payloads, control characters (Ctrl+C / Ctrl+D), DTR/RTS for reset/boot pins, "send break" |

It replaces ad-hoc use of PuTTY/minicom/SecureCRT with something that is scriptable
(quick commands, file send), logs everything, and lives in the same Qt/CMake ecosystem
as the team's other tools. It intentionally has **no device-specific protocol logic**;
it is a general terminal that speaks bytes.

## 2. Non-goals (v0.1)

- No TCP/telnet/SSH transports (architecture allows adding them behind `SerialConnection`).
- No XMODEM/YMODEM/Kermit file transfer (planned; `FileSender` is the hook).
- No scripting language; quick commands + file send cover the automation needs for now.
- No perfect terminfo compliance; the VT100/xterm subset in `TERMINAL_EMULATION.md` is the target.

## 3. Toolchain and repository conventions

| Item | Choice |
|---|---|
| Language | C++20, Qt 6.8 (Core, Gui, Widgets, SerialPort, LinguistTools, Test) |
| Build | CMake ≥ 3.21 + Ninja; `CMakePresets.json` (Qt Creator picks it up), `scripts/build.ps1` for the CLI |
| Compiler (Windows) | MSVC 2022 x64 (`D:\Qt\6.8.3\msvc2022_64`), `/W4 /utf-8 /permissive-` |
| Layout | `src/app` (settings, logging, version), `src/core` (serial + helpers), `src/terminal` (emulator), `src/ui` (widgets), `src/dialogs`, `tests/` |
| Naming | PascalCase files and classes (`SerialConnection.cpp`), `m_` member prefix, Qt-style camelCase methods, `lc*` logging categories |
| Includes | Always relative to `src/`: `#include "core/HexUtils.h"`; `ui_Foo.h` for Designer forms |
| Strings | UI text wrapped in `tr()`; technical text (hex, port names) not translated |
| Ownership | Qt parent/child for QObjects; `std::unique_ptr` for non-QObject heap data; no raw `delete` outside destructors |
| Threads | Everything on the GUI thread. QSerialPort is asynchronous; rendering is coalesced. No worker threads in v0.1 |
| Logging | `qCInfo(lcSerial) << ...`; routed to the System Log dock by `SystemLogViewer::installMessageHandler()` |
| Formatting | `.clang-format` (LLVM base, 4 spaces, 120 cols, braces on new line for functions/classes) |

The executable is `BuildAI-SerialUtility.exe`; QSettings uses organization `BuildAI`,
application `SerialUtility`. Per-user data (quick commands JSON, history) lives in
`QStandardPaths::AppConfigLocation` (`AppSettings::dataDirectory()`).

## 4. Architecture

```
┌────────────────────────────────────────────────────────────────────────────┐
│ MainWindow (QMainWindow, MainWindow.ui)                                    │
│  menus / toolbar / status bar / System Log dock / Language menu             │
│  QTabWidget ──► SessionWidget ×N                                            │
│                  ├── ConnectionBar        (port + line parameters + connect)│
│                  ├── QStackedWidget                                         │
│                  │     ├── TerminalWidget  ──► AnsiParser ──► TerminalScreen│
│                  │     └── HexDumpView                                      │
│                  ├── QuickCommandBar      (QuickCommandStore, shared)       │
│                  ├── CommandInput         (CommandHistory, shared)          │
│                  ├── SerialConnection     (QSerialPort + reconnect)         │
│                  └── SessionLogger                                          │
│  Dialogs: About, Version, Preferences, QuickCommands, SendFile(FileSender)  │
└────────────────────────────────────────────────────────────────────────────┘
        ▲ AppSettings (QSettings façade)     ▲ SerialPortEnumerator (1 s poller)
```

Two CMake targets: **`su_core`** (static lib: `src/app`, `src/core`, `src/terminal`
minus `TerminalWidget`; links Core/Gui/SerialPort; used by tests) and
**`SerialUtility`** (the app: widgets, dialogs, `TerminalWidget`, `main.cpp`).

### 4.1 Data flow

```
device ─RX─► QSerialPort.readyRead ─► SerialConnection::dataReceived(QByteArray)
   ├─► TerminalWidget::feedData ─► AnsiParser::feed ─► TerminalScreen ops ─► dirty rows ─► 16 ms repaint
   ├─► HexDumpView::appendReceived
   └─► SessionLogger::logReceived

keyboard/IME ─► TerminalWidget::keyToBytes ─► sendData ─┐
CommandInput::sendRequested ────────────────────────────┼─► SessionWidget::sendBytes ─► SerialConnection::write ─► device
QuickCommandBar::commandTriggered ─► QuickCommand::payload ┘        └─► dataSent ─► HexDumpView / SessionLogger
FileSender::chunkReady ─────────────────────────────────┘
AnsiParser::responseRequested (DSR/DA replies) ─► TerminalWidget::sendData
```

### 4.2 Module contracts

The headers in `src/**/*.h` are the contract. Each carries a doc comment describing
behaviour, defaults and edge cases. **Public methods and signals are fixed**; implementers
may add private members, private slots and helper functions freely, and may add
`public` getters if strictly needed (note it in the summary). Key modules:

| Module | Responsibility | Notes |
|---|---|---|
| `SerialConnection` | one QSerialPort; open/close/write; counters; **auto-reconnect** on `ResourceError`; live parameter changes; DTR/RTS; break | never blocks (`waitForBytesWritten` forbidden) |
| `SerialPortEnumerator` | singleton 1 s poller of `QSerialPortInfo`; natural sort; add/remove diffs | Windows enumeration is cheap (<5 ms) |
| `TerminalScreen` | grid + scrollback + cursor + attributes + modes; every editing op the parser needs | pure model, unit-tested |
| `AnsiParser` | bytes → decoded text → VT500 state machine → `TerminalScreen` ops; DSR/DA replies | never desyncs on garbage |
| `TerminalWidget` | paint `TerminalScreen`, selection, scrollbar, keys → bytes, IME, paste, drag&drop, zoom | `QAbstractScrollArea` |
| `HexDumpView` | read-only RX/TX hex dump with timestamps | bounded |
| `SessionWidget` | glue for one port session; single TX path `sendBytes()`; system lines; auto-log | see header for exact wiring |
| `ConnectionBar` | port/baud/8N1/flow/DTR/RTS/Break/Connect controls | keeps selection across hot-plug |
| `CommandInput` | line-mode input with history, hex, escapes, line ending | validation feedback |
| `QuickCommandBar` / `QuickCommandStore` / `QuickCommandsDialog` | macros as buttons; JSON persistence; editor | default set below |
| `FileSender` / `SendFileDialog` | paced text/binary file send | modeless dialog |
| `SessionLogger` | raw / timestamped text / hex-dump session capture | flush every write |
| `AppSettings` | typed QSettings façade + `changed(key)` | defaults documented in header |
| `SystemLogViewer` | System Log dock; Qt message handler | reference-tool look |
| `MainWindow` | tabs, actions, status bar, language switch, state persistence | actions listed in header |
| `AboutDialog` / `VersionDialog` / `PreferencesDialog` | as in the Vispek reference tool, adapted | Designer `.ui` forms |

### 4.3 Default quick commands

`QuickCommandStore::defaults()` (group → name → command, CR unless noted):

- **Linux**: `uname -a`; `cpuinfo` → `cat /proc/cpuinfo`; `meminfo` → `cat /proc/meminfo`;
  `df -h`; `ifconfig`; `dmesg tail` → `dmesg | tail -n 50`; `ps`; `top` → `top -n 1`;
  `ls /dev` → `ls -l /dev/tty* /dev/video* 2>/dev/null`; `date`; `reboot`
- **U-Boot**: `help`; `printenv`; `bdinfo`; `version`; `mmc info`; `boot`; `reset`
- **MCU**: `help`; `version`; `AT` (CRLF); `AT+GMR` (CRLF); `reset`
- **Control**: `Ctrl+C` (hex `03`); `Ctrl+D` (hex `04`); `Ctrl+Z` (hex `1A`); `ESC` (hex `1B`);
  `Enter` (hex `0D`); `Ctrl+L` (hex `0C`)

### 4.4 Settings defaults (see `AppSettings.h`)

115200 8N1, no flow, DTR/RTS asserted; Enter → CR; Backspace → DEL (0x7F); UTF-8;
implicit CR on LF **on**; local echo off; auto-reconnect on (1000 ms); scrollback 10 000;
theme `dark`; font Consolas 10 (Windows) / Monospace 10; log dir `<Documents>/BuildAI/SerialLogs`;
log format `text`, include TX; confirm close when connected; restore last ports.

### 4.5 Terminal emulation scope

See `docs/TERMINAL_EMULATION.md`. Summary: C0 controls, ESC 7/8/D/E/H/M/c/#8, CSI
cursor/erase/edit/scroll/SGR (16/256/truecolor)/DECSTBM/DECSET-DECRST (1, 6, 7, 25, 47,
1047, 1049, 2004)/DSR/DA/DECSTR, OSC 0/2 title. Wide (CJK) characters occupy two cells.
Unknown sequences are consumed silently (debug log).

### 4.6 Built-in device simulator ("SIM:" pseudo-ports)

`src/core/DeviceSimulator` provides four simulated devices that are listed alongside the
real ports (manufacturer "BuildAI Simulator") when `AppSettings::showSimulatedPorts()` is
on (default on; a Preferences checkbox hides them for production use):

| Port | Behaviour | Exercises |
|---|---|---|
| `SIM:loopback` | echoes every byte | TX/RX path, hex view, logger, encodings |
| `SIM:linux` | Rockchip-style boot log → `login:` → busybox-like shell with device-side line editing, coloured `dmesg`, `top`, `progress`, `color`, `chinese`, `reboot` (port vanishes 3 s) | full emulation, quick commands, auto-reconnect |
| `SIM:uboot` | U-Boot banner, "Hit any key to stop autoboot" countdown, `=>` prompt, `boot` → Linux | autoboot interrupt workflow |
| `SIM:mcu` | firmware shell with `\r\n`, `AT`/`OK`, telemetry stream | CRLF/LF handling, streaming |

Integration points: `SerialSettings::isSimulatedPort()`, `SerialConnection::open()` creates a
`DeviceSimulator` instead of using `QSerialPort` (same signals, same counters, `vanished()`
mapped onto the `portDisappeared` → reconnect flow via `DeviceSimulator::isPresent()`),
`SerialPortEnumerator::refresh()` appends `DeviceSimulator::entries()`. Output is paced to
the selected baud rate. See the header for the exact command set. Unit tests:
`tests/tst_devicesimulator.cpp`.

### 4.7 Error handling and UX rules

- Never block the GUI thread on I/O. No modal dialogs for routine serial errors: they go
  to the status bar (5 s) and the System Log; the terminal shows a dim system line for
  port disappearance / reconnection.
- Every user action that cannot proceed says why in the status bar ("Not connected",
  "Port COM8 busy or access denied").
- Closing anything that loses data (connected tab, active log) asks once, unless disabled.
- High-rate output (1.5 Mbaud boot log ≈ 150 KB/s) must keep the UI responsive: parser
  work is O(bytes), repaints coalesced, hex view bounded, logger flushes but never fsyncs.
- Keyboard-first: every action has a shortcut (listed in `MainWindow.h`); Tab is sent to
  the device when the terminal has focus (never moves focus).

## 5. Build, run, test

```powershell
# from the repo root, any PowerShell (no Developer Prompt needed)
.\scripts\build.ps1 -Config Release -Test           # configure + build + ctest
.\scripts\build.ps1 -Config Debug  -Target su_core   # just the core library
.\scripts\build.ps1 -Config Release -Deploy          # dist\Release\bin with windeployqt
.\scripts\run.ps1 -Config Release
```

Qt Creator: *File > Open File or Project* → `CMakeLists.txt`; choose the `Release` or
`Debug` preset (kit "Desktop Qt 6.8.3 MSVC2022 64bit").

Tests are Qt Test executables in `tests/` linked against `su_core`; run with
`ctest --test-dir build/Release --output-on-failure`.

## 6. Implementation work packages

Each package owns exactly the listed files (the `.h` files already exist and are fixed).
Packages build in isolation with `scripts/build.ps1 -BuildDir build/<pkg> -KeepGoing
-Target <targets>` and ignore errors in files they do not own.

| Package | Files owned | Build target to check |
|---|---|---|
| **core** | `src/app/Logging.cpp`, `src/app/AppSettings.cpp`, `src/core/*.cpp`, `tests/tst_hexutils.cpp`, `tst_lineending.cpp`, `tst_commandhistory.cpp`, `tst_quickcommand.cpp`, `tst_serialsettings.cpp` | `su_core`, the five tests |
| **term-core** | `src/terminal/CharWidth.cpp`, `TerminalScreen.cpp`, `AnsiParser.cpp`, `TerminalTheme.cpp`, `tests/tst_charwidth.cpp`, `tst_terminalscreen.cpp`, `tst_ansiparser.cpp`, `docs/TERMINAL_EMULATION.md` | `su_core`, the three tests |
| **term-widget** | `src/terminal/TerminalWidget.cpp`, `src/ui/HexDumpView.cpp` | `SerialUtility` (compile only) |
| **ui-session** | `src/ui/SessionWidget.cpp`, `ConnectionBar.cpp`, `CommandInput.cpp`, `QuickCommandBar.cpp` | `SerialUtility` (compile only) |
| **ui-main** | `src/main.cpp`, `src/ui/MainWindow.cpp`, `MainWindow.ui`, `src/ui/SystemLogViewer.cpp` | `SerialUtility` (compile only) |
| **dialogs-docs** | `src/dialogs/*.cpp`, `src/dialogs/*.ui`, `README.md`, `docs/QUICKSTART.md`, `.github/workflows/build.yml` | `SerialUtility` (compile only) |

After integration: full build, tests green, `windeployqt` deploy, manual run against
COM8 (CH343 → Rockchip board) and COM6 (J-Link CDC → MCU).

## 7. Future work

TCP/telnet transport; XMODEM/YMODEM; search in scrollback UI; split view; session
profiles (named connection presets); Linux packaging (AppImage/deb); auto-update check.
