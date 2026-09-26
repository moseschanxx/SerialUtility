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
| Build | CMake ≥ 3.22 + Ninja (≥ 3.25 for the presets); `CMakePresets.json` (Qt Creator picks it up), `scripts/build.ps1` for the CLI |
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

Three CMake targets: **`su_core`** (static lib: `src/app`, `src/core`, `src/terminal`
minus `TerminalWidget`; links Core/Gui/SerialPort; used by the unit tests), **`su_app`**
(static lib: `TerminalWidget`, every widget and dialog; linked by the GUI test suites) and
**`SerialUtility`** (the app: `main.cpp` + `su_app` + resources).

### 4.1 Data flow

```
device ─RX─► QSerialPort.readyRead ─► SerialConnection::dataReceived(QByteArray)
   ├─► TerminalWidget::feedData ─► AnsiParser::feed (one screen batch) ─► TerminalScreen ops ─► dirty rows ─► coalesced repaint
   ├─► HexDumpView::appendReceived (queued while the hex page is hidden)
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
| `SerialConnection` | one QSerialPort; open/close/write; counters; **auto-reconnect** when the device vanishes (`ResourceError`, or Read/Write/UnknownError with the port no longer enumerated, or enumerator `portRemoved`); live parameter changes; DTR/RTS; break | never blocks (`waitForBytesWritten` forbidden); public getter/signal added at integration: `pendingTxBytes()` / `txBytesWritten()` (write-buffer backpressure for `SendFileDialog`), `sendBreak()` returns success |
| `SerialPortEnumerator` | singleton 1 s poller of `QSerialPortInfo`; natural sort; add/remove diffs | Windows enumeration is cheap (<5 ms) |
| `TerminalScreen` | grid + scrollback + cursor + attributes + modes; every editing op the parser needs | pure model, unit-tested |
| `AnsiParser` | bytes → decoded text → VT500 state machine → `TerminalScreen` ops; DSR/DA replies | never desyncs on garbage |
| `TerminalWidget` | paint `TerminalScreen`, selection, scrollbar, keys → bytes, IME, paste, drag&drop, zoom | `QAbstractScrollArea` |
| `HexDumpView` | read-only RX/TX hex dump with timestamps | bounded; queues while hidden, renders on show (`flushPending()` added at integration) |
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
theme `dark`; font Consolas 10 (Windows) / Monospace 10; pause output while selecting **on**;
right click pastes (cmd.exe style) **on**;
log dir `<Documents>/BuildAI/SerialLogs`; log format `text`, include TX; confirm close when
connected; restore last ports.

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
| `SIM:linux` | Rockchip-style boot log → `login:` → busybox-like shell with device-side line editing, coloured `dmesg`, `top`, `progress`, `color`, `chinese`, `reboot` (port vanishes 3 s), `poweroff` (device stays listed but is down until you reconnect) | full emulation, quick commands, auto-reconnect |
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
- High-rate output (1.5 Mbaud boot log ≈ 150 KB/s) must keep the UI responsive, and the RX
  pipeline is sized with headroom for it. Measured on the 4-core reference machine (Release,
  `tests/tst_terminalperf.cpp`: coloured 120-byte lines with six SGR sequences each, 4 KB
  chunks, 1000×700 window): AnsiParser + TerminalScreen alone ≈ 16 MB/s; TerminalWidget shown
  ≈ 9 MB/s (real window) / 11 MB/s (offscreen); the full SessionWidget RX path with the terminal
  in front ≈ 9.8 MB/s real window / 10.5 MB/s offscreen (before this work: 0.14 / 0.31 MB/s, i.e.
  ~1200 lines/s); LogReplayer "Unlimited" through a session ≈ 11.6 MB/s real window (was 0.13);
  hex view in front ≈ 0.4 MB/s (was 0.13). Throughout, a 50 ms QTimer in the same event loop
  keeps firing ≈ 19×/s with a longest gap of ≈ 60 ms. The mechanisms, in order of impact:
  1. `HexDumpView` renders nothing while it is hidden (the normal case during a boot log):
     chunks are queued with their formatting captured on arrival, the queue is bounded to what
     `maxLines()` can show, and `showEvent()` / `flushPending()` render it in one batch, giving
     the same document as per-chunk rendering. Shown, it still renders per chunk, but
     `trimToMaxLines()` finds the cut with `findBlockByNumber()` instead of stepping `NextBlock`
     over every block (each step laid the block out). One QTextDocument insertion + trim per
     4 KB chunk cost ≈ 12 ms - more than the whole terminal pipeline.
  2. `TerminalWidget` draws ASCII runs as pre-shaped glyph runs (`QRawFont` glyph-index cache
     per render font, `QPainter::drawGlyphRun`), so a full 40×120 frame costs ≈ 2.6 ms instead
     of ≈ 7 ms in a real window. The repaint coalescer measures its window from the end of the
     previous paint and never makes it shorter than that paint took, so painting is capped at
     half of the wall time even when a frame outlasts 16 ms (previously every chunk became a
     frame once painting was slower than the timer); the first change after an idle period is
     painted at once.
  3. `TerminalScreen::beginBatch()/endBatch()` wrap every `AnsiParser::feed()`: contentChanged /
     scrollbackChanged / cursorMoved - and with them the scrollbar range update - fire once per
     chunk instead of once per text run and control character.
  4. `LogReplayer` at unlimited speed feeds 4 KB chunks for an ≈ 8 ms slice per event-loop
     iteration instead of one chunk per iteration.
  Parser work stays O(bytes); scrollback pushes move lines (QList front removal is O(1) in
  Qt 6); the logger flushes but never fsyncs. `tst_terminalperf` asserts only generous floors
  (≥ 0.3 MB/s offscreen, ≥ 1 UI-timer turn per 100 ms, no stall > 500 ms) and prints the rates.
- Keyboard-first: every action has a shortcut (listed in `MainWindow.h`); Tab is sent to
  the device when the terminal has focus (never moves focus). Bare `Ctrl+<letter>` belongs
  to the device while connected, so MainWindow actions never use one: Hex View, Find, Send
  File, Clear, Replay Log and Quit are `Ctrl+Shift+<letter>` (H / F / O / L / R / Q). The
  terminal leaves every `Ctrl+Shift+<letter>` plus `Ctrl+T`, `Ctrl+W`, `Ctrl+Tab`, `Ctrl+,`
  and F2 / F3 / F5 to the application's shortcut map and claims everything else.
- Pause output while selecting (cmd.exe QuickEdit / mark mode; `AppSettings::pauseWhileSelecting()`,
  **on** by default, *View > Pause Output While Selecting* and *Preferences > Terminal*): a
  streaming console cannot be copied from if the text keeps scrolling under the pointer, so as
  soon as a selection becomes non-empty (drag past the pressed cell, double/triple click,
  Shift+click, Select All, Find) `TerminalWidget` stops parsing and queues every byte from
  `feedData()` (`isOutputPaused()`, `pendingPausedBytes()`). Screen, scrollback and selection are
  frozen; the hex view and the session logger keep receiving because `SessionWidget` feeds them
  separately. Enter copies the selection and resumes, Esc cancels, any copy (Ctrl+Shift+C,
  Ctrl+Insert, Ctrl+C with a selection, context menu) copies and resumes, a plain click or
  `clearSelection()` resumes; every other key press and every paste is swallowed (nothing reaches
  the device, no local echo) except the application shortcuts above; wheel / scrollbar / zoom keep
  working. Resuming parses the queue in one feed (one coalesced repaint) and keeps the
  follow-output state; `clearScreen()` / `clearAll()` / `resetTerminal()` apply first and flush afterwards. A
  translucent badge in the top-right corner shows the queued size ("Output paused  12.3 KB waiting
  Enter: copy  Esc: cancel", repainted at most every 100 ms) and `SessionWidget` shows a
  persistent status-bar hint via `outputPausedChanged()` (`persistentStatusMessage()`; MainWindow
  swaps it for the new tab's own state on every tab switch or close and brings it back once a
  transient message expired, so a background tab's hint never stays on screen). The queue is bounded by
  `pauseBufferLimit()` (64 MiB): exceeding it resumes automatically with the selection kept,
  flushes everything (`pauseBufferOverflow()`, a status message) and never drops a byte. Turning
  the feature off while paused resumes too.
- cmd.exe-style right click (`AppSettings::rightClickPastes()`, **on** by default, *View > Right
  Click Pastes (cmd.exe style)* and *Preferences > Terminal*; `TerminalWidget::setRightClickPastes()`,
  pushed by `SessionWidget::applyPreferences()` like the pause flag): a plain right-button press in
  the terminal copies the selection when one exists (`copySelection()` + `clearSelection()`, i.e.
  exactly Enter in mark mode - the paused display resumes, nothing is pasted) and pastes the
  clipboard otherwise (the `paste()` path: CR/LF conversion, bracketed paste, dropped while
  disconnected); no menu opens. The context menu moves to Shift+right click, the Menu key and
  Shift+F10: `contextMenuEvent()` swallows a mouse-triggered event without Shift while the feature
  is on (whatever the platform's trigger, press or release) and appends a disabled hint line
  "Right click: paste / copy selection - Shift+right click: this menu" so the change is
  discoverable; with the feature off a plain right click opens the menu as before. A right-button
  press never starts, extends or drops a left-button selection (a chorded right press during a
  left drag is ignored). The View action and the Preferences checkbox are kept in step through
  `AppSettings::changed`, exactly like *Pause Output While Selecting*.
- Clear (toolbar button next to Disconnect, *Session > Clear*, Ctrl+Shift+L;
  `SessionWidget::clearTerminal()` → `TerminalWidget::clearAll()` + `HexDumpView::clearAll()`) wipes
  the screen *and* the scrollback and the hex view, homes the cursor and keeps attributes, modes and
  the parser state; *Reset Terminal* (`resetTerminal()`) remains the full RIS. While an
  alternate-screen program (`top`, `vi`, `menuconfig`) is running, `TerminalScreen::clearAll()` also
  blanks the primary grid saved behind it, so nothing of the old output comes back when the program
  exits (the program keeps the alternate screen and redraws itself). While paused the
  clear applies first and the queued bytes are parsed afterwards on the empty screen. The terminal's
  own context menu keeps the finer-grained *Clear Screen (keep scrollback)* (`clearScreen()`: the
  screen is pushed into the scrollback, like Ctrl+L in a shell - the label says so because it is
  not the toolbar's Clear) and *Clear Scrollback* (`clearScrollback()`).

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

Tests are Qt Test executables in `tests/`: unit suites linked against `su_core` and GUI suites
(`tst_terminalwidget`, `tst_sessionwidget`, `tst_mainwindow`, `tst_dialogs`, `tst_terminalperf`)
linked against `su_app`, driving the real widgets against the `SIM:` pseudo-ports. ctest runs every suite with
`QT_QPA_PLATFORM=offscreen`, so they pass headless; run with
`ctest --test-dir build/Release -C Release --output-on-failure`.

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
