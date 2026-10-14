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

## 2. Non-goals

- No raw TCP/telnet transport yet (v0.3 added SSH behind the `Transport` interface, section 4.8;
  telnet would be a third implementation).
- No XMODEM/YMODEM/Kermit file transfer (planned; `FileSender` is the hook).
- No scripting language; quick commands + file send cover the automation needs for now.
- No perfect terminfo compliance; the VT100/xterm subset in `TERMINAL_EMULATION.md` is the target.

## 3. Toolchain and repository conventions

| Item | Choice |
|---|---|
| Language | C++20, Qt 6.8 (Core, Gui, Widgets, SerialPort, Network, LinguistTools, Test) |
| Build | CMake ≥ 3.22 + Ninja (≥ 3.25 for the presets); `CMakePresets.json` (Qt Creator picks it up), `scripts/build.ps1` for the CLI |
| SSH | libssh 0.11.1 fetched and built from source by CMake (`FetchContent`, SHA-256 pinned, shared `ssh.dll` / `libssh.so.4`), OpenSSL 3 as its crypto backend (`find_package(OpenSSL)`; the `libcrypto` runtime is copied next to the binaries and shipped) |
| Compiler (Windows) | MSVC 2022 x64 (`D:\Qt\6.8.3\msvc2022_64`), `/W4 /utf-8 /permissive-` |
| Layout | `src/app` (settings, logging, version), `src/core` (serial + helpers), `src/ssh` (libssh), `src/firmware` (Firmware tab core, v0.5), `src/terminal` (emulator), `src/ui` (widgets), `src/dialogs`, `third_party/` (bundled flashing tools, v0.5), `tests/` |
| Naming | PascalCase files and classes (`SerialConnection.cpp`), `m_` member prefix, Qt-style camelCase methods, `lc*` logging categories |
| Includes | Always relative to `src/`: `#include "core/HexUtils.h"`; `ui_Foo.h` for Designer forms |
| Strings | UI text wrapped in `tr()`; technical text (hex, port names) not translated |
| Ownership | Qt parent/child for QObjects; `std::unique_ptr` for non-QObject heap data; no raw `delete` outside destructors |
| Threads | Everything on the GUI thread, except SSH: every libssh call runs on a private worker thread per `SshConnection` (section 4.8); the GUI only sees queued signals. QSerialPort is asynchronous; rendering is coalesced |
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
│  QTabWidget ──► SessionWidget ×N   (Kind::Serial or Kind::Ssh)              │
│                  ├── ConnectionBar | SshConnectionBar   (strip above)       │
│                  ├── QStackedWidget                                         │
│                  │     ├── TerminalWidget  ──► AnsiParser ──► TerminalScreen│
│                  │     └── HexDumpView                                      │
│                  ├── QuickCommandBar      (QuickCommandStore, shared)       │
│                  ├── CommandInput         (CommandHistory, shared)          │
│                  ├── Transport: SerialConnection (QSerialPort + reconnect)  │
│                  │              | SshConnection (libssh worker thread)      │
│                  └── SessionLogger                                          │
│  Dialogs: About, Version, Preferences, QuickCommands, SendFile(FileSender), │
│           HostKey, AuthPrompt, SshProfiles, RemoteFile (SFTP)               │
└────────────────────────────────────────────────────────────────────────────┘
        ▲ AppSettings (QSettings façade)     ▲ SerialPortEnumerator (1 s poller)
        ▲ SshProfileStore (ssh_profiles.json) ▲ SecretStore (DPAPI / obfuscated)
```

Three CMake targets: **`su_core`** (static lib: `src/app`, `src/core`, `src/ssh`, `src/firmware`,
`src/terminal` minus `TerminalWidget`; links Core/Gui/SerialPort/Network and libssh; used by the unit tests), **`su_app`**
(static lib: `TerminalWidget`, every widget and dialog; linked by the GUI test suites) and
**`SerialUtility`** (the app: `main.cpp` + `su_app` + resources).

### 4.1 Data flow

```
device ─RX─► QSerialPort.readyRead ─► SerialConnection ─┐
server ─RX─► ssh channel (worker thread, queued) ─► SshConnection ─┴─► Transport::dataReceived(QByteArray)
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
| `Transport` | abstract byte-stream behind a session tab: state machine (Disconnected / Connecting / Connected / Reconnecting), counters, `dataReceived`/`dataSent`, `connectionLost`/`connectionRestored`, `notifyTerminalSize()` | `SerialConnection` and `SshConnection` implement it; `SessionWidget`, logger and views only use this interface |
| `SerialConnection` | one QSerialPort; open/close/write; counters; **auto-reconnect** when the device vanishes (`ResourceError`, or Read/Write/UnknownError with the port no longer enumerated, or enumerator `portRemoved`); live parameter changes; DTR/RTS; break | never blocks (`waitForBytesWritten` forbidden); public getter/signal added at integration: `pendingTxBytes()` / `txBytesWritten()` (write-buffer backpressure for `SendFileDialog`), `sendBreak()` returns success |
| `SshConnection` | SSH shell as a `Transport` (libssh on a worker thread): host-key verification against OpenSSH `known_hosts`, agent / key / password / keyboard-interactive auth with GUI prompts, PTY + window-change, keep-alive, auto-reconnect with backoff, local port forwards, SFTP upload/download | section 4.8; all signals on the GUI thread |
| `SshProfile` / `SshProfileStore` | saved targets (`ssh_profiles.json` in the data directory) and recent ad-hoc targets; `parseTarget()` for `user@host:port` / `ssh://` / `[v6]:port` | never contains secrets |
| `SecretStore` | saved passwords / passphrases: Windows DPAPI (current user), elsewhere obfuscated in QSettings | `isSecure()` tells the UI which one |
| `SshConnectionBar` / `HostKeyDialog` / `AuthPromptDialog` / `SshProfilesDialog` / `RemoteFileDialog` | the SSH strip above the terminal, the two blocking questions of a connect, the profile manager and the SFTP transfer dialog | Designer `.ui` forms except the bar |
| `TestSshServer` (tests only) | in-process libssh server: password / public-key / keyboard-interactive auth, PTY, a scripted shell, exec, a built-in minimal SFTP v3 server (`tests/support/TestSftpHandler`, because libssh compiles its own SFTP server out on Windows), direct-tcpip, abrupt drops | every SSH suite runs against it on both CI platforms |
| `SerialPortEnumerator` | singleton 1 s poller of `QSerialPortInfo`; natural sort; add/remove diffs | Windows enumeration is cheap (<5 ms) |
| `TerminalScreen` | grid + scrollback + cursor + attributes + modes; every editing op the parser needs | pure model, unit-tested |
| `AnsiParser` | bytes → decoded text → VT500 state machine → `TerminalScreen` ops; DSR/DA replies | never desyncs on garbage |
| `TerminalWidget` | paint `TerminalScreen`, selection, scrollbar, keys → bytes, IME, paste, drag&drop, zoom | `QAbstractScrollArea` |
| `HexDumpView` | read-only RX/TX hex dump with timestamps | bounded; queues while hidden, renders on show (`flushPending()` added at integration) |
| `SessionWidget` | glue for one port session; single TX path `sendBytes()`; system lines; auto-log | see header for exact wiring |
| `ConnectionBar` | port/baud/8N1/flow/DTR/RTS/Break/Connect controls | keeps selection across hot-plug |
| `CommandInput` | line-mode input with history, hex, escapes, line ending | validation feedback |
| `QuickCommandBar` / `QuickCommandStore` / `QuickCommandsDialog` | macros as buttons; JSON persistence (file version 2); editor | default set below |
| `FirmwareTab` + `src/firmware/*` | the Firmware tab (v0.5): pull `update.img`, flash the Rockchip SoC, update the CC1310 over the Node board's USB-C cable | section 4.11 |
| `FileSender` / `SendFileDialog` | paced text/binary file send | modeless dialog |
| `SessionLogger` | raw / timestamped text / hex-dump session capture | flush every write |
| `AppSettings` | typed QSettings façade + `changed(key)` | defaults documented in header |
| `SystemLogViewer` | System Log dock; Qt message handler | reference-tool look |
| `MainWindow` | tabs, actions, status bar, language switch, state persistence | actions listed in header |
| `AboutDialog` / `VersionDialog` / `PreferencesDialog` | as in the Vispek reference tool, adapted | Designer `.ui` forms |

### 4.3 Default quick commands

`QuickCommandStore::defaults()` (group → name → command, CR unless noted). The groups appear in
this order, **Node first** since v0.5:

- **Node** (`QuickCommandStore::nodeDefaults()`, group name `nodeGroupName()` = "Node"; the Node
  board's two consoles behind one USB-C cable; button label = command, all with CR, every button
  has a tooltip): `reboot loader` (SoC console: reboot into the USB download / Loader mode for
  flashing); `rfmcu console` (SoC console: open the CC1310 CLI through the SoC; `~.` leaves it);
  then the CC1310 CLI, usable once `rfmcu console` runs, taken from the CC1310 operations
  runbook (`docs/operations/05_cli_operations.md`, Part B): `help`; `version`;
  `sys` (`sys | sys wdt_test`); `stack`; `uart` (`uart | uart baud <rate>`);
  `power` (`power | power off [delay_ms] | power soc on|off`);
  `bat` (`bat | bat sim <mV>|off | bat charge on|off`); `rx status` (`rx status | rx dump on|off`);
  `rx dump on`; `rx dump off`; `tx status`; `bootloader` (writes an update request and reboots
  the CC1310 into its UART updater); and `~.` (leave `rfmcu console`; the console wants it on its
  own line, so it ends with CR like the rest)
- **Linux**: `uname -a`; `cpuinfo` → `cat /proc/cpuinfo`; `meminfo` → `cat /proc/meminfo`;
  `df -h`; `ifconfig`; `dmesg tail` → `dmesg | tail -n 50`; `ps`; `top` → `top -n 1`;
  `ls /dev` → `ls -l /dev/tty* /dev/video* 2>/dev/null`; `date`; `reboot`
- **U-Boot**: `help`; `printenv`; `bdinfo`; `version`; `mmc info`; `boot`; `reset`
- **MCU**: `help`; `version`; `AT` (CRLF); `AT+GMR` (CRLF); `reset`
- **Control**: `Ctrl+C` (hex `03`); `Ctrl+D` (hex `04`); `Ctrl+Z` (hex `1A`); `ESC` (hex `1B`);
  `Enter` (hex `0D`); `Ctrl+L` (hex `0C`)

The set is 15 + 11 + 7 + 5 + 6 = 44 commands. **File format, version 2 (v0.5):**
`{ "version": 2, "commands": [...] }` (`QuickCommandStore::fileVersion()`). `load()` of a version-1
file - or one without a `"version"` key - that has no command whose effective group is "Node" puts
`nodeDefaults()` in front of the commands and reports it through `*migrated`, so the caller can
save the file at once (`save()` always writes version 2); a version-1 file that already has Node
commands is left as it is, a version-2 (or later) file is loaded exactly as written (a user who
deleted the Node group keeps it deleted), and `importFromFile()` never migrates (it takes what a
shared file holds). Nothing else in the file is touched.

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
| `SIM:mcu` | firmware shell with `\r\n`, `AT`/`OK`, telemetry stream (`telemetry on [ms]` makes it chatty) | CRLF/LF handling, streaming |

Native baud rates (v0.4): `SIM:linux` and `SIM:uboot` talk at 1 500 000, `SIM:mcu` at 115200,
`SIM:loopback` at any rate (`DeviceSimulator::nativeBaudRate()`). A session opened at another
rate hears deterministic wrong-rate garbage (`DeviceSimulator::wrongRateNoise()`, paced like the
text, dominated by 0x00 / 0xFF / high-bit bytes as a UART decodes a mismatch) while the scripted
device keeps running underneath; typed input is dropped meanwhile; `setNativeBaudRate()` switches
a running instance mid-session (a board that changed rate) - a reboot goes back to the kind's
native rate. This is what makes automatic baud detection testable offscreen (section 4.10).

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

### 4.8 SSH sessions (v0.3)

**Transport.** `core/Transport.h` is the byte-stream interface a session tab talks to
(`open/close/write`, `dataReceived/dataSent`, `stateChanged` over Disconnected / Connecting /
Connected / Reconnecting, `errorOccurred`, counters, `connectionLost/connectionRestored`,
`notifyTerminalSize`). `SerialConnection` implements it unchanged in behaviour (its serial-only
API - line parameters, DTR/RTS, BREAK, `portDisappeared/reconnected` - stays on top).
`SessionWidget` is created with a `Transport::Kind`: the serial strip (`ConnectionBar`) or the SSH
strip (`SshConnectionBar`) sits above the same terminal, hex view, quick commands, command input
and logger, so every feature of a serial tab works identically for SSH. Session restore persists
one key per tab: the port name for serial, `ssh:profile:<id>` / `ssh:target:<user@host:port>`
for SSH (never auto-connecting on start).

**SshConnection** (`ssh/SshConnection.h`, libssh 0.11.1). All libssh calls run on a private
worker thread per connection; the class is a GUI-thread facade whose every signal is emitted
on the GUI thread. The connect sequence is documented in the header and mirrors OpenSSH:
TCP + key exchange with the profile's timeout → host key check against an OpenSSH
`known_hosts` file (default `~/.ssh/known_hosts`, shared with the system `ssh`; Unknown / Changed
/ new key type are answered by the user through `hostKeyVerificationRequired` → `HostKeyDialog`;
"remember" appends, or for a changed key replaces the stored line(s) for that host) →
authentication in the profile's order (Auto = agent, identity file, `~/.ssh/id_ed25519|id_ecdsa|id_rsa`,
password, keyboard-interactive; each only if the server offers it; encrypted keys and passwords
raise `authPromptRequired` → `AuthPromptDialog`, up to three attempts; a password or passphrase
can be remembered in `SecretStore`: under the profile id for a stored profile, under
`ssh/target/<user@host:port>/password` for an ad-hoc target and under `ssh/key/<path>/passphrase`
for a key file, so a remembered key needs its passphrase only once whatever the target (v0.4)) → PTY (`xterm-256color`, the terminal's
current grid) + `shell` (or `exec` for a remote command) → `shellStarted`. `~/.ssh/config` is
parsed by libssh so `Host` aliases, `IdentityFile`, `User`, `Port` and `ProxyJump` from the user's
config apply; explicit profile fields win (the environment variable `SU_SSH_IGNORE_CONFIG=1` skips
the config; the test suites set it). Terminal resizes become window-change requests. A
keep-alive (`SSH_MSG_IGNORE`, 30 s default) detects dead links; a dropped link (not a clean
`exit`) goes to Reconnecting with 2 → 30 s backoff using only non-interactive credentials.
Local port forwards (`-L`) are `QTcpServer`s in the worker bridged to `direct-tcpip` channels.
File transfer uses the SFTP subsystem of the same session (`RemoteFileDialog`, drag-and-drop of a
file onto an SSH terminal = upload) in 64 KiB slices interleaved with the shell traffic. When the
server refuses the `sftp` subsystem (dropbear on a buildroot board), the transfer falls back to a
plain exec channel driving busybox tools - upload `: > 'path'` (create / truncate: a path that
cannot be written fails here and is never touched) then `cat > 'path'` fed with the raw bytes
then EOF (+ `chmod`; a command that ends without an exit status is confirmed by `wc -c` against
the local size), download `wc -c < 'path'` then `cat 'path'`, `test -e` for overwrite
protection, `rm -f` for an upload that did not complete (cancel, failure, a Disconnect during
it) - paths single-quoted with `'''` escaping, a relative `-name` written as `./-name`; the
method is reported in `TransferStatus::method` and the finished message ("..., via shell")
(v0.4).
Limitation: libssh has no ssh-agent support on Windows, so `Auth::Agent` is only useful on Linux
and macOS (key files work everywhere).

**Profiles and secrets.** `SshProfile` is a value type (host, port, user, auth method, identity
file, remote/startup command, terminal type, keep-alive, timeout, proxy jump, local forwards,
compression, known_hosts file); `SshProfileStore` persists them as JSON in the data directory
(`ssh_profiles.json`, sorted by name, plus the last ten ad-hoc targets) and never stores secrets.
`SecretStore` keeps passwords and passphrases: Windows DPAPI (only the same user on the same
machine can decrypt) or, elsewhere, an obfuscated value in QSettings that the UI honestly labels
as "not encrypted" (`isSecure()`).

**UI.** *File > New SSH Session...* (Ctrl+Shift+T) opens a tab whose bar has an editable target
combo (stored profiles, then recent ad-hoc targets; free text `user@host[:port]`, `ssh://…`,
`[v6]:port`), a gear to the profile manager and the Connect button. *Edit > SSH Profiles...*
manages profiles (import/export JSON, Connect straight from the dialog). *Session > Upload File
to Remote... / Download File from Remote...* are enabled on a connected SSH tab; Send BREAK,
DTR/RTS and Refresh Ports are serial-only. *Preferences > SSH* holds the default known_hosts
file, identity file, terminal type and keep-alive; they are applied at connect time to the
fields a profile leaves empty (the known_hosts file for every profile; identity file, terminal
type and keep-alive for ad-hoc targets only, since a stored profile carries its own). The status
bar shows `user@host:port · ssh-ed25519 · publickey`. Command line: `--ssh <target>` opens an SSH
tab with that target filled in (`--ssh <target> --connect` connects at once); a session-restore key
(`ssh:target:...`, `ssh:profile:<id>`) is accepted as the positional argument too.

**Testing.** `tests/support/TestSshServer` is an in-process libssh *server* (ed25519 host key,
password / public-key / keyboard-interactive auth, PTY with window-change tracking, a scripted
shell - `echo`, `env`, `size`, `big n`, `sleep`, `hang`, `exit n` - exec, a built-in minimal SFTP v3
server (`tests/support/TestSftpHandler`; libssh's own SFTP server is compiled out on Windows) that
`Options::allowSftp = false` switches off to imitate dropbear, the exec file commands the shell
fallback needs (`cat > 'p'`, `cat 'p'`, `wc -c < 'p'`, `test -e`, `chmod`, `rm -f`, `mkdir -p`, `mv`, `pwd`),
direct-tcpip, abrupt client drops), so `tst_sshconnection` and the whole-application
`tst_sshsession` suite run on every developer machine and on both CI platforms with no external
sshd. `tst_sshsession` drives the real `MainWindow`, bar and dialogs (a timer answers the modal
questions from inside their `exec()` loops): the ad-hoc flow with Enter in the line edit, Enter on
the combo itself and the Connect button, host key remember / connect once, the password prompt,
shell typing, status bar / tab dot / title, SFTP upload and download through `RemoteFileDialog`,
a profile created in `SshProfilesDialog` with a saved password (no prompt on later connects, no
secret in the JSON), session restore through `closeEvent`, link drop → reconnect (and none with
auto-reconnect off), a `SIM:loopback` tab working next to the SSH tab with the actions flipping per
tab, the Chinese UI switch with a connected tab, and the quit confirmation with a clean worker
shutdown. A user-initiated Disconnect produces only the status-bar message, like a serial tab; a
remote `exit` writes the "connection closed (exit status N)" system line. `tst_sshconnection` additionally contains a live probe against a real OpenSSH server that
runs only when `SU_SSH_PROBE_TARGET=user@host[:port]` (and optionally `SU_SSH_PROBE_PASSWORD`)
is set; it was run against the WSL Ubuntu sshd during development.

**Packaging.** libssh is fetched from libssh.org at configure time (SHA-256 pinned, `WITH_SERVER`
and `WITH_SFTP` on, examples/zlib/gssapi off) and built as a shared library; its `ssh.dll` /
`libssh.so.4` and OpenSSL's `libcrypto` are copied next to every executable after linking and
shipped in the installer, the portable zip and the Linux tarball (the tarball's `libssh.so`
carries `RUNPATH=$ORIGIN`, so the bundled `libcrypto.so.3` is used on distros without OpenSSL 3).
Windows needs an OpenSSL 3 installation to *build* (`C:\Program Files\OpenSSL-Win64` or any
prefix passed as `-DOPENSSL_ROOT_DIR`); users need nothing.

### 4.9 Keyboard: what the device gets and what the application keeps (v0.4)

`TerminalWidget::setReservedShortcuts()` receives the current shortcuts of every `MainWindow`
action (alternates included); `MainWindow` pushes the list to each session's terminal after
`applyShortcuts()` and whenever a `shortcuts/*` setting changes. While a session is connected a
key press whose sequence is reserved is left to the application (`ShortcutOverride` not
accepted); Ctrl+Shift+*x* and Alt+*digit* (the tab accelerators) pass through regardless; every
other key that maps to a byte sequence goes to the device - bare Ctrl+letter (Ctrl+W = 0x17,
Ctrl+T = 0x14, ...), Alt+letter (ESC + letter: readline's Alt+F / Alt+B, so the menu bar's
mnemonics do not open a menu from a connected terminal - a bare Alt tap or the mouse does) and
F-keys included. Consequently F2 / F3 / F5 are application keys only because
Connect / Disconnect / Refresh Ports use them, and the defaults changed so that nothing a shell
needs is reserved: Close Session = Ctrl+Shift+W (was Ctrl+W). Shortcuts live in
`AppSettings::shortcut(actionName, uiDefault)` (`shortcuts/<objectName>`, portable text; an empty
value means none); *Preferences > Keyboard* edits them with conflict detection (rows against
each other and against the fixed sequences `MainWindow::fixedShortcutEntries()` lists: the tab
alternates and the menu mnemonics) and per-row / page-level defaults, accepts only chords that
can be application shortcuts (Ctrl / Alt / Meta with a key, or an F-key - a bare letter, Enter
or Space would steal typed text from the device), and `MainWindow::shortcutEntries()` feeds the
page. Tab selection: the
*Window* menu (Next / Previous, `Tab 1..9` = Alt+1..9, then one checkable entry per open
session), Ctrl+PageDown / Ctrl+PageUp as fixed alternates of Next / Previous, and a middle click
on a tab closes it.

### 4.10 Automatic baud-rate detection (v0.4)

`core/BaudRateDetector` scores bytes with `textScore()` (printable ASCII, common controls and
well-formed UTF-8 count as text; 0x00 / 0xFF and invalid UTF-8 as garbage; a Rockchip boot log
scores > 0.97 at the right rate and < 0.4 at a wrong one). `start()` drives an open
`SerialConnection` through the candidates from *Preferences > Connection* (current rate first,
then 115200, 1500000, 921600, 460800, 230400, 57600, 38400, 19200, 9600 by default, `sampleMs`
each, early exit at `goodScore()` 0.9); a silent device or one readable nowhere restores the
original rate and reports it; the search never writes to the device. `SerialSettings::autoBaud`
("Auto" at the top of the bar's baud list, effective rate kept in `baudRate`, summary
"Auto (1500000) 8N1") makes `SessionWidget::connectPort()` run a detection right after opening,
with the terminal muted and system lines "--- detecting baud rate ---" / "--- baud rate 1500000
detected ---", and feeds the live RX stream to the detector's watchdog (sliding window of 512
bytes, `badScore()` 0.5, at most one re-detection per 5 s) so a board that switches rates
mid-session is followed; a re-detection that finds nothing readable at any candidate puts the
watchdog on hold - the garbage stays visible, no further search - until readable output arrives
or a detection is started by hand. Between two candidates only the RX side of the port is
purged (`SerialConnection::clearBuffers(QSerialPort::Input)`): queued TX bytes keep going out
and no write in flight is aborted, and no detection starts while the session's *Send File*
dialog is sending. Choosing "Auto" while connected at a fixed rate searches from that rate (it
is tried first and kept when the device is quiet). *Session > Detect Baud Rate* (Ctrl+Shift+B,
`SessionWidget::detectBaudRate()`) runs one detection on demand and applies the result to a
fixed-rate session too. The simulators have native rates (`DeviceSimulator::nativeBaudRate()`: Linux / U-Boot
1 500 000, MCU 115200, loopback any) and emit paced garbage while the session rate differs, so the
whole flow is testable offscreen (`tst_bauddetector`, `tst_devicesimulator`, `tst_sessionwidget`).

### 4.11 Firmware tab (v0.5; partition writes in Loader mode since v0.5.1)

Spec: `docs/firmware-update/REQUIREMENTS.md`; plan, and the facts found on the real tools and the
real board before any code was written: `docs/firmware-update/PLAN.md`. The headers under
`src/firmware/` and `src/ui/FirmwareTab.h` are the contract and carry the details; this section is
the map.

**Purpose.** One tab, *File > Firmware Update* (Ctrl+Shift+U), replaces the whole Windows flashing
routine for the Node board (Rockchip RV1106BG2 SoC + TI CC1310 RF MCU): pull the newest build
from the Linux build machine, flash the SoC, update the MCU - over the board's single USB-C cable,
with no vendor GUI, no USB-serial adapter and no MASKROM button; the common case is one button,
**Pull & Flash**. Unlike the rest of the application this part is device-specific by design; it is
a tab of its own and touches no behaviour of the serial and SSH tabs.

**Facts the design rests on.**

- The running firmware exposes ADB only (VID 0x2207, PID 0x0006). `adb reboot loader` sets the
  reboot-mode register, U-Boot enters `download` and shows up as rockusb in *Loader* mode (VID
  0x2207, PID 0x110x); a board that does not boot can be put into *Maskrom* with the button.
  `upgrade_tool LD` lists both and prints `Mode=Loader` / `Mode=Maskrom`; `upgrade_tool UF
  <update.img>` writes the RKFW image (about 11 MB, every partition including userdata), resets
  the board and prints `Upgrade firmware ok`. `upgrade_tool.exe` v2.37 runs without a `config.ini`
  (Linux v2.26 reads an optional one next to it).
- `UF` first compares the chip id the loader reports with the image's ("110E"); the board's
  U-Boot (rockchip-platforms up to v1.3.0) answers with zeros in Loader mode, so `UF` stops at
  `Check Chip Fail` there (TEST_REPORT section 13). Rockchip's SocToolKit flashes the same board
  in Loader mode because its Download page never runs `UF`: `upgrade_tool EXF <image> <dir>`
  unpacks the package into its partition images plus `package-file` (`<partition>\t<file>`
  lines), each image goes to the flash with `upgrade_tool WL <offset / 512> <size / 512> <file>`
  (512-byte sectors; the partition size in sectors is what SocToolKit passes and what
  upgrade_tool needs for a jffs2 image such as userdata.img - without it v2.37 answers
  "Image is jffs format,please tell the size!"; progress `Write LBA from file (nn%)`, no chip
  check) and `RD` resets the board
  (`Reset Device OK.`, back on ADB about 25 s later). The offsets are the board's flash layout,
  the `mtdparts=` word of its kernel command line: `sfc_nor:64K(env),256K@64K(idblock),
  256K(uboot),384K(meta),3M(boot),5M(rootfs),3M(userdata)` on the Node board (the SDK's
  `sd_update.txt` writes to the same offsets). The image carries the same string itself: its
  `env.img` (64 KiB, a 4-byte CRC then NUL-separated `key=value` entries) starts with
  `mtdparts=sfc_nor:...`, as Rockchip's `rkdownload.sh` reads it (`strings env.img | head -1`).
- The CC1310's UART0 is wired to the SoC's UART1 (`/dev/ttyS1`, 115200 8N1). `rfmcu bridge 7000`
  on the board joins it to 127.0.0.1:7000 (one client at a time, exclusive `flock` on the tty);
  on the PC `adb forward tcp:0 tcp:7000` prints a free local port to connect a socket to. A stale
  `rfmcu` must be killed first, and again with `adb forward --remove` when done, because adbd does
  not reliably end the board process when the host side closes.
- On USB-C power the SoC stays up through the CC1310's reset into its updater (verified on
  hardware 2026-10-13 with the Python tool); the tab always runs with the cable in, and logs the
  CLI `power` reply before sending `bootloader`, as `fw_update.py` does.

**Layers.**

```
FirmwareTab (su_app, one instance in MainWindow's QTabWidget; widgets, dialogs, settings)
  ├── FirmwareJob (su_core, since 0.5.1: the chain, also driven by the su-fw console tool)
  │     ├── ImagePuller  ─────► SshConnection (runCommand, statRemote, startTransfer: the SSH stack of 4.8)
  │     ├── SocFlasher   ─────► RockchipTool (upgrade_tool LD / SFI / EXF / WL / RD / UF) + AdbTool + UsbProbe ─► ToolRunner
  │     └── McuLink      ─────► AdbTool + QTcpSocket ──► McuUpdater ──► CcProtocol / CcPackage
  └── SessionProvider ──► SessionWidget::sendBytes()     (`reboot loader`, only after a Yes)
```

All of `src/firmware/` is part of `su_core` (UI-free, Qt Core + Network); only `FirmwareTab` is in
`su_app`. Every firmware class takes a `ToolRunner*` that it does not own, so the tests slot in a
`FakeToolRunner` with scripted outputs.

| Class | Role |
|---|---|
| `ToolRunner` / `ProcessToolRunner` / `ToolInvocation` | One external process, asynchronous (`QProcess`, merged stdout + stderr delivered raw with `\r`), cancel (terminate, kill after 2 s), `ToolResult` (started, cancelled, timed out, exit code, output). Never blocks the GUI thread; a program that cannot start reports through `finished()` from the event loop. |
| `ToolLocator` | Where `upgrade_tool`, `adb` and the DriverAssistant installer are: the Preferences override, then `tools/` next to the executable, then (adb only) the first one on `PATH`. |
| `RockchipTool` | `upgrade_tool` behind the runner and the parsers of its output: `LD` device list (Loader / Maskrom), `SFI` image info, the incremental progress parser (`\r`-separated segments, `(nn%)`; `UF` and `WL` alike), the success check (`Upgrade firmware ok`), the RKFW magic check; since 0.5.1 also `EXF` / `WL` / `RD`, the `mtdparts` parser (`<size>[@<offset>](<name>)`, K / M / G, `-`; anything malformed gives no layout), the kernel command line's `mtdparts=` word, the `mtdparts=` entry of an `env.img` (`mtdPartsFromEnvImage()`: entries from byte 4 or 5, never the word inside `sys_bootargs`), `package-file`, and `planPartitions()`: the unpacked images placed in a layout (`PartitionImage`: offset, size, in layout, fits, selected). |
| `AdbTool` | `adb devices` / `get-state` / `reboot loader` / `shell` / `forward tcp:0` / `forward --remove` and the long-running `shell rfmcu bridge`, with the device selection and error texts of `adb_link.py`. |
| `UsbProbe` | Windows: SetupAPI enumeration of `USB\VID_2207*` (present, driver class, problem code) so "a Rockchip device is plugged in but no driver claims it" can be said and the bundled DriverAssistant offered; Linux: `/sys/bus/usb/devices`; other platforms: nothing. |
| `SocFlasher` | The flow of `rockchip-platforms/tools/usb_update.sh` as a cancellable state machine (below), plus the 2 s device status (`queryStatus()`) and the partition plan (`planPartitions()`: `EXF` + `RockchipTool::planPartitions()`, kept per image path, mtime and layout). |
| `CcProtocol` | The CC1310 UART bootloader protocol exactly as `fw_protocol.py` defines it: COBS, CRC16-CCITT (0xFFFF, 0x1021), frame v2 (`<BBHH>` header + payload + CRC16), message ids HELLO ... SET_ROLE, `FrameSplitter`, the INFO payload. |
| `CcPackage` | The `.pkg` format of `fw_package.py`: the 32-byte FWPK header, header and image CRC32, address 0x8000, size up to 0x17000, version `0x00MMmmpp`; the same checks in the same order with the same reasons. |
| `McuUpdater` | The `fw_update.py` state machine over a `QIODevice` (Info, Flash, SetRole), every wait a `QTimer` deadline: HELLO, else the application CLI probe, `bootloader`, BEGIN / READY, 128-byte DATA / ACK / NACK with resume, END / COMPLETE, the restart banner; the Python timeouts and retry counts are the defaults. |
| `McuLink` | The road to the CC1310 through the SoC (`McuUpdater -- QTcpSocket -- adb forward -- USB-C -- rfmcu bridge -- /dev/ttyS1 -- CC1310`): select the board, check `rfmcu`, `killall rfmcu`, `forward tcp:0 tcp:7000`, start `rfmcu bridge 7000`, wait for "bridging", connect; `close()` undoes it all; `cliCommand()` runs `rfmcu cmd` without a bridge. |
| `ImagePuller` | Over an `SshConnection` the owner connected: stat the remote `update.img`, optional `git describe`, the skip rule, download, RKFW check, one-level history (`update.prev.img`), optional extra files, `rollBack()`. |
| `FirmwareJob` | (0.5.1) The chain without widgets: every operation of the tab (`pull`, `flash`, `mcu`, `set-role`, `info`, `pull-flash`) with its steps, rules, log lines and messages; owns the collaborators above and an `SshConnection` of its own. |
| `FirmwareTab` | The widget: the Source / SoC / MCU sections (SoC with the partition table since 0.5.1), Pull & Flash, the log pane; owns a `FirmwareJob` and renders it. |

**SoC flow** (`SocFlasher::start()`, as `usb_update.sh`): *Detecting* (`LD`, then `adb devices`;
Loader / Maskrom wins) -> an ADB device is told `adb reboot loader` (*RebootingToLoader*, after
logging its `/proc/version` - busybox on the board has no `uname` - and reading its
`/proc/cmdline`, whose `mtdparts=` becomes the board's layout: `mtdPartsRead()`; without one and
without a layout from the owner the image is unpacked right there, and when its `env.img` carries
no layout either the flow stops with "partition layout unknown" before the board is rebooted) ->
with no board at all `downloadModeNeeded()` lets the tab offer `reboot loader\r`
through a connected session tab (asked every time) or show the MASKROM instructions ->
*WaitingForLoader* (`LD` every second, 30 s) -> *Flashing*, **not cancellable**, by the method the
mode decides (`flashMethod()`, logged):
- Loader mode, `PartitionWrites` (0.5.1, SocToolKit's Download page): the plan of the image (made
  by the tab when the image or the layout changed, reused while image path, mtime, board layout,
  the unpacked files and the layout in use are unchanged; else `EXF` again, stage "Extracting").
  The layout in use is the image's own when its `env.img` carries a readable one (the layout the
  images were built for, which the env being written makes the board use from its next boot),
  else `mtdParts()`; a board layout that differs is logged as a warning with both strings. Then every selected row is
  checked (in the layout, file present, not larger than its partition, sector-aligned) before
  the first write, then one `WL <offset / 512> <size / 512> <file>` per row in package-file order (progress
  "Writing boot (5 of 7)", the percentage over all bytes; rows not selected or not in the layout
  are logged as skipped; a failing `WL` stops with its last lines), then `RD`. The selection is
  `setSelectedPartitions()` (names; empty = every row in the layout, and then an image that does
  not fit stops the flash instead of being left out). Without a layout: "partition layout
  unknown: boot the board once so it can be read over ADB, or flash it in MASKROM mode".
- Maskrom mode, `UpgradeFirmware`: `UF`, progress, the success line is required; a partition
  selection does not apply (logged).

-> *WaitingForAdb* (`adb get-state`, 60 s; a late board is a success with a warning) ->
*ReadingBoardInfo* (`/proc/version`, `uptime`). The image must exist and carry the RKFW magic.

**Partition table** (0.5.1, `FirmwareTab`): one row per `package-file` entry of the unpacked
image (`EXF` into `<image folder>/<image base name>.extracted`; upgrade_tool's own entries
`package-file` and `bootloader` are not partitions), a checkbox, name, file, offset, partition
size, image size; rows not in the layout, without a file or too large are unchecked and disabled
with the reason as the tooltip. The layout in use (`flashLayout()`) is the image's own (its
`env.img`) when it carries one, else the board's: `AppSettings::firmwareMtdParts(serial)`, read
once per session with `adb shell cat /proc/cmdline` when the status refresh sees an ADB board whose
layout is not known yet, and stored from `mtdPartsRead()` too; the entry `*` is the layout read
last, used for a board met in Loader mode (rockusb reports no ADB serial). The board's is what the
tab hands to `SocFlasher::setMtdParts()`; the label says which one is in use ("layout from the
image" / "from the board", "the board's differs"). Flash SoC hands the
checked names to the flasher (refused when none is checked); Pull & Flash checks every row and
hands over the empty list (every row); MASKROM mode ignores the selection.

**MCU flow.** `McuLink::open()` brings the bridge up (above); `McuUpdater::flash(role)` runs the
connect sequence (HELLO twice; otherwise probe the CLI, log `power` when `throughSoc`, `bootloader`,
wait 500 ms, HELLO up to ten times), checks target and size, sends BEGIN (the package header plus
the role), the DATA chunks, END, and waits for the restart banner (`version=` ... `cli=ready`, 8 s);
a NACK resumes from the offset the device reports. The role is chosen by the user, pre-selected
from what the MCU reports, never guessed silently. `setRole()` sends SET_ROLE only.

**Pull flow** (`ImagePuller::start()`): stat -> `git describe` (never an error) -> skip when the
cached note matches size and mtime -> download to `update.img.new` (the transfer writes
`<path>.part` and renames) -> RKFW check -> rotate the old image to `update.prev.img` -> write the
note `update.img.json`.

**Pull & Flash** chains pull (skipped when unchanged), Flash SoC (every partition), the wait for ADB and, when a
package is selected whose version differs from the running MCU version (read after the SoC is
back), Update MCU. SoC first because the MCU path needs `rfmcu` on the SoC; it stops at the first
failure.

**FirmwareJob** (0.5.1, `src/firmware/FirmwareJob.h`, `su_core`; step 2 of
`docs/firmware-update/CLI_PLAN.md`). The chain above lives in a UI-free `QObject` so that the tab and
the console tool `su-fw` run the very same code: the tab is the view, the job the chain. The job owns
the collaborators (`SocFlasher`, `McuLink`, `McuUpdater`, `ImagePuller`, its `SshConnection`, and a
`ProcessToolRunner` when it is given no runner) and runs the operations as slots (`pull()`,
`flashSoc()`, `updateMcu()`, `setMcuRole()`, `readMcuInfo()`, `pullAndFlash()`, `cancel()`, each
refused while another runs with "Busy: <title> is still running"). It reports plain log lines (the
tab adds the time; the job mirrors them to `lcFirmware`, never a secret), the raw upgrade_tool lines
(`toolOutput()`, collapsed by the tab), `progress(stage, percent)` (a percentage, "busy" or "text
only"), the lasting status, `stepFinished()` for every step and `operationFinished()` at the end
(the op names `pull` ... `pull-flash` are public constants), and the state the tab shows (device,
image, package, running MCU version and role, the partition plan and selection, the layout in use).
It reads no settings: the tab hands the fields and the Preferences in when an operation starts
(target, folders, SSH defaults, `git describe`, extra files, tool paths, timeouts), answers
`setLayoutProvider()` from `AppSettings::firmwareMtdParts()` and stores what the job reports
(`pullSourceAccepted()`, `mtdPartsRead(serial, mtdparts)`, the cache image after a pull, the role
it pre-selected). The three decisions the tab asks with dialogs are callbacks: the host key
(`HostKeyDecision{accept, remember}`), the authentication prompt (an answer with "remember", or none
= cancelled) and the download mode (`DownloadModeAnswer`: whether a question was asked, and a
function that sends `reboot loader\r` through a session - called only when the flasher still waits
for Loader mode afterwards). Without a callback (su-fw without a terminal to ask) the job takes the
headless defaults: an unknown or changed host key is refused, so the pull fails with the message
the tab shows on a refusal ("Host key rejected for <target>"); a prompt is cancelled and the pull
fails with "authentication failed: nobody answered "<prompt>"" (a secret saved in SecretStore is
still tried by `SshConnection` first); download mode shows the MASKROM instructions
(`maskromInstructions()`) and ends with the flasher's own "board did not enter MASKROM/Loader within
<n> s" unless the board appears. `tst_firmwarejob` runs the chain on `FakeToolRunner`,
`TestSshServer` and `FakeMcuServer` (58 cases); `tst_firmwaretab` checks the view on top.

**SSH stack additions** (no behaviour change for session tabs): `SshConnection::runCommand()`
(an exec channel without PTY, `commandFinished(id, exitStatus, output, errorText)`) and
`statRemote()` (SFTP stat, falling back to `stat -L -c '%s %Y %F'`, which follows a symlinked image like
SFTP does; `remoteStatReceived(...)`), both
asynchronous on the worker; `tests/support/TestSshServer` answers the stat and the scripted
`git describe` commands.

**Main window integration.** The `QTabWidget` may now hold one non-session tab: `currentSession()`
and `sessionAt()` are null-safe, session actions (Connect, Send File, Upload, Hex View, ...) are
disabled while the Firmware tab is current, it is not saved or restored as a port, the Window menu
lists it, closing it is refused while it is busy and quitting asks once like a connected session.
`File > Firmware Update` selects the existing tab rather than opening a second one. The tab's
`statusMessage()` goes to the status bar like a session's.

**Rules it follows.** Nothing blocks the GUI thread (every tool is a `QProcess`, every wait a
timer); errors go to the status bar and the log pane, not to modal boxes - the only questions
asked are the ones that have to be (the SSH host-key and password prompts of 4.8, sending
`reboot loader` through a session tab, quitting while busy); every UI string is `tr()`; every
external command line and its output go to the log pane and to the System Log (`lcFirmware`),
never a password or key; Cancel exists only where it is safe (waiting for the Loader, never while
`EXF` / `WL` / `RD` / `UF` run in the Flashing phase). The first real flash of a board is an integration test of its own: what will run on it is
announced before it runs (`docs/firmware-update/PLAN.md`, "Ask me before").

**Settings** (`firmware/` group of `AppSettings`, edited in *Preferences > Firmware*): the last
source target, the remote folder per target (default `~/rk/rockchip-platforms/output/image`), the
cache folder (default `<data>/firmware`), the last local image and package, the `upgrade_tool` and
`adb` path overrides, the Loader wait (30 s) and ADB-return wait (60 s), `git describe` on or off,
extra remote file names; since 0.5.1 `firmware/mtdparts` (a map: board serial -> flash layout,
`*` = the one read last; written by the tab, not shown in Preferences).

**Packaging.** The tools are plain files in `third_party/` (provenance, versions and licences:
`third_party/README.md`): `rockchip/windows/` (`upgrade_tool.exe` v2.37, `license.txt` - the LGPL
v3 text Rockchip ships with it - and `revison.log`), `rockchip/linux/` (`upgrade_tool` v2.26,
`config.ini`, `88-rockusb.rules`), `adb/` (`adb.exe`, `AdbWinApi.dll`, `AdbWinUsbApi.dll` from
FactoryTool v1.91: its adb 1.0.41 supports `forward tcp:0` and shell_v2) and
`DriverAssistant/DriverAssitant_v5.14.zip`. CMake assembles `tools/` next to the executable after
each link of `SerialUtility` (the zip is extracted once at configure time and flattened so that
`tools/DriverAssistant/DriverInstall.exe` exists; Windows only) and `cmake --install` ships it as
`bin/tools/`; the portable zip, the tarball and the installer take `bin/` recursively. The Inno
Setup script has an optional, unchecked task "Install the Rockchip USB drivers (DriverAssistant)"
that runs `DriverInstall.exe` (a GUI installer) and removes `{app}\tools` on uninstall. Both CI
jobs check after the install that the tools landed. On Linux the bundled Linux `upgrade_tool` is
used with the system `adb`, so CI builds and tests run without any hardware. A native
implementation of the rockusb / ADB protocols (libusb / WinUSB) instead of the bundled tools was
deliberately not done: the requirements make it a decision to ask for first.

**Tests** (all offscreen / headless, no board, neither the build nor the tests run Python):
`tst_ccprotocol` (known-answer vectors generated once with the CC1310 repository's Python tools and
committed under `tests/data/firmware/`: COBS, CRC16 `123456789` = 0x29B1, frames, a small `.pkg`,
`firmware-0.8.1.pkg`), `tst_mcuupdater` (the C++ `FakeMcu` behind a `QTcpServer`: full update, a
CC1310 already in its updater, NACK and resume, a lost frame, the banner after COMPLETE, set role,
`power` logged before `bootloader`), `tst_firmwaretools` (the `LD` / `SFI` / `UF` / adb parsers,
`SocFlasher` and `McuLink` with fake tools, the driver hint; since 0.5.1 the `mtdparts` /
command-line / `package-file` parsers, `planPartitions()`, and the Loader-mode flow against a fake
`EXF` that unpacks the Node image: the seven `WL` sectors 0, 128, 640, 1152, 1920, 8064, 18304,
a selection, an image too big, a failing `WL` / `RD`, the layout unknown, the plan signal and its
staleness, the `env.img` reader and the image's layout winning over a different board layout),
`tst_imagepuller` (against
`TestSshServer`: stat, skip, `.part`, magic, history, `git describe`), `tst_firmwarejob` (0.5.1: the
chain without widgets, every operation, the callbacks and their headless defaults; the Node board's
fixtures shared with the tab's suite live in `tests/support/FirmwareFixtures`), `tst_firmwaretab` (the GUI:
the action, single instance, enable states, a pull, a flash and an MCU update with the fakes, the
main window's actions while the tab is current, close and restore; since 0.5.1 the partition
table, All / None, the layout read once from a board, the layout from the image, Flash SoC writing the checked rows, MASKROM
ignoring them, Pull & Flash writing every row) and `tst_quickcommand` (the
Node group and the version-1 migration of 4.3).

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

### 6.1 v0.3.0 work packages (SSH)

Same rules; every package built in its own `build/wf-<pkg>` directory while the others ran.
Compilable stubs of every new class existed from the start so each package linked and tested on
its own.

| Package | Files owned |
|---|---|
| **transport-session** | `SerialConnection` on top of `Transport`; `SessionWidget` in serial or SSH mode; `MainWindow` actions (New SSH Session, SSH Profiles, Upload/Download); `AppSettings` SSH keys; Preferences SSH page; the related test suites |
| **ssh-core** | `SshConnection.cpp` (worker thread), `SecretStore.cpp`, `tst_sshprofile`, `tst_secretstore`, `tst_sshconnection` (incl. a live probe against a real sshd, env-gated) |
| **test-server** | `tests/support/TestSshServer.cpp`, `tst_testsshserver` |
| **ssh-dialogs** | `SshConnectionBar.cpp`, the four SSH dialogs and their `.ui`, `tst_sshdialogs` |
| **integrate / gui-e2e** | build everything together, `tst_sshconnection` end to end, `tst_sshsession` (whole-app flow) |

### 6.2 v0.4.0 work packages

| Package | Files owned |
|---|---|
| **keyboard** | `TerminalWidget` reserved shortcuts, `MainWindow` shortcut application / Window menu / Detect Baud Rate action, *Preferences > Keyboard* and the Connection page auto-baud fields, their suites |
| **ssh-core** | `SshWorker` remembered ad-hoc credentials and the exec-channel transfer fallback, test server exec commands / `allowSftp`, `tst_sshconnection`, `tst_testsshserver` |
| **ssh-ui** | prompt checkbox labels, transfer method text, the real-server transfer investigation (env-gated GUI probe + `gui-smoke.ps1 -Scenario sshfiles`), `tst_sshdialogs`, `tst_sshsession` |
| **autobaud** | `BaudRateDetector`, `SerialSettings::autoBaud`, simulator native rates / garbage, the bar's Auto item, session detection + watchdog, their suites |

### 6.3 v0.5.0 work packages (Firmware tab)

Same rules as 6.1. The headers of every new class (`src/firmware/*.h`, `src/ui/FirmwareTab.h`, the
two new `SshConnection` calls, the `QuickCommandStore` additions) were written first as the
contracts, with compile-only stubs behind them; then five packages ran side by side, each in its
own `build/wf-<pkg>` directory (two at a time on a 4-core machine), followed by integration, a
review of the integrated tree and the run against the real board
(`docs/firmware-update/PLAN.md`, "Work packages").

| Package | Files owned |
|---|---|
| **protocol** | `CcProtocol`, `CcPackage`, `McuUpdater`, `tests/support/FakeMcu`, `tst_ccprotocol`, `tst_mcuupdater`, the known-answer data under `tests/data/firmware/` |
| **tools** | `ToolRunner` / `ProcessToolRunner` / `ToolLocator`, `RockchipTool`, `AdbTool`, `UsbProbe`, `McuLink`, `SocFlasher`, `tests/support/FakeToolRunner`, `tst_firmwaretools` |
| **ssh-pull** | `SshConnection` / `SshWorker` `runCommand()` and `statRemote()`, `ImagePuller`, the `TestSshServer` stat / `git describe` commands, `tst_imagepuller` |
| **ui** | `FirmwareTab`, the `MainWindow` integration (one non-session tab, actions, close rules), the `AppSettings` firmware keys, *Preferences > Firmware*, the icon, `tst_firmwaretab`, translations |
| **packaging-docs** | `third_party/` (tools, provenance, licences), the CMake packaging step and version 0.5.0, the CI checks, the Inno Setup driver task, the `QuickCommandStore` Node group and version-2 file + `tst_quickcommand`, `README.md`, this document (4.3, 4.11, 6.3, 7), `docs/QUICKSTART.md`, `VERSION.txt` |

## 7. Future work

TCP/telnet transport; XMODEM/YMODEM; split view; Linux packaging
(AppImage/deb); auto-update check; SSH: agent authentication on Windows (libssh has no agent
support there), remote port forwards, SFTP browser. Firmware tab: the rockusb and ADB protocols
natively (libusb / WinUSB) instead of the bundled tools (needs a decision first, see 4.11); a
macOS build of the tab (no `upgrade_tool` is bundled for it); updating a CC1310 that sits behind
a USB-serial adapter instead of the SoC (`McuUpdater` already works on any `QIODevice`).
