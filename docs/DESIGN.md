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

**Facts the design rests on.** (v0.8.0: the Stacked core's differences - chip tag, Loader PID, layout, MASKROM, the CC1310's power - are in `docs/firmware-update/RV1106G2_FACTS.md` and 4.14.)

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
  else `mtdParts()`; a board layout that differs is refused unless --force-layout / the tab's question, 4.14 rule 2 (both strings are logged). Then every selected row is
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
<n> s" unless the board appears. Since 0.6.0 there is one firmware operation per machine: every
operation holds a `QLockFile` (`AppSettings::dataDirectory()/firmware.lock`, taken without waiting
in `beginOperation()`, stale only when its process is gone, released on every end and in the
destructor), so the tab and `su-fw` cannot drive the same board at once. An operation that finds it
held ends at once, framed as usual, with "another firmware operation is running (<program>, pid
<n>)"; `refreshStatus()` runs no query while another job holds it (logged once, "status paused:
..."; `statusPausedReason()`), and probes it only when the file exists, so an idle status timer never
holds it when the other side starts. The status's own `/proc/cmdline` read ends with
`layoutReadFinished(serial, ok)`. `tst_firmwarejob` runs the chain on `FakeToolRunner`,
`TestSshServer` and `FakeMcuServer` (62 cases); `tst_firmwaretab` checks the view on top.

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

### 4.12 Command-line interface: `su-fw` (v0.6.0)

Plan: `docs/firmware-update/CLI_PLAN.md`. Moses, 2026-10-13: users update the Node board by hand
from the app, and an AI agent must be able to run the same update in the background from a
command shell. `su-fw <command> [options]` does what the Firmware tab does, with the log on
stdout and the result as the exit code; `su-fw --help` / `su-fw <command> --help` are the
reference (written for an agent: every option, the exit codes, two examples) and README
"Command line (`su-fw`)" the user guide.

**Why a console program of its own.** The GUI executable is linked for the Windows subsystem: it
has no console, cannot write to the shell that started it, and the shell does not wait for it. A
GUI that relays to a console child would be two processes for nothing. So `su_fw` is a second
executable (`OUTPUT_NAME su-fw`, no `WIN32` keyword) that links `su_core` only - `QCoreApplication`,
no widgets, no platform plugin needed - and is installed next to the GUI (`bin/`, the same
`tools/`), deployed by its own run of the Qt deploy script, and carried by the installer, the zip
and the tarball.

**The split.**

| Layer | Where | What |
|---|---|---|
| `FirmwareJob` | `src/firmware/FirmwareJob.{h,cpp}` (0.5.1, `su_core`) | The chain: operations, steps, rules, messages, the callbacks for the decisions. Shared with the tab; 0.6.0 added the machine-wide operation lock and `layoutReadFinished()`. |
| `FirmwareCli` | `src/firmware/FirmwareCli.{h,cpp}` (`su_core`, UI-free) | The command line: `int run(arguments, out, err)` parses one command (a `QCommandLineParser` per command), checks every input before anything runs (usage errors exit 2 on `err`), configures a fresh `FirmwareJob` from the options and the app's settings, answers its callbacks, runs one operation on a local event loop until `operationFinished()`, writes the output and maps the result to the exit code. Hooks for the tests and `main.cpp`: the `ToolRunner`, the stdin line reader (and whether it is a terminal), the profile store, fixed tool paths, a job-setup callback; `interrupt()` for Ctrl+C. |
| `main.cpp` | `src/cli/main.cpp` | The process: `QCoreApplication` with the app's organisation / application names (so `AppSettings`, the SSH profiles and `SecretStore` are the GUI's), stdout / stderr / stdin, the terminal's echo while a password is typed, `SIGINT` / `SIGTERM` / `SIGHUP` (/ `SIGBREAK`) counted by the handler and passed to `interrupt()` by a 100 ms timer and after every read from stdin, the `buildai.*` logging categories muted (the job mirrors every line to `lcFirmware`; `QT_LOGGING_RULES` turns them back on), UTF-8 console output on Windows (the console's code page back on every way out of `main()`). |

**Commands** (`FirmwareCli::commands()`): `status` (the device status query, `Board: <the tab's
status line>`; a running board's layout is read once and stored like the tab does; `--mcu` adds
`readMcuInfo()`), `partitions` (the job's plan of an image: the tab's table as text or JSON rows; the
board is not touched), `pull`, `flash-soc` (`--all` or `--partitions` is required; the plan is made
first so that an unknown name, a row that cannot be written, or - with `--all` - an image that does
not fit is refused before the board is touched; `--no-wait` turns the ADB wait off), `mcu-info`,
`mcu-update` (`--role` required: never guessed), `mcu-set-role`, `pull-flash` (`pullAndFlash()`;
`--role` only for an MCU that cannot report one). The MCU commands query the board first and exit 3
unless it is running on ADB. Common: `--tools DIR`, `--log FILE`, `--json`, `--quiet`, `--timeout S`,
`--yes`.

**Inputs from the app.** Tool paths as the tab (`ToolLocator` with the Preferences overrides; or
`--tools DIR`), the Loader / ADB waits, the cache folder, `git describe`, the extra files, the SSH
defaults (known_hosts, identity, terminal type, keep-alive), the tab's last build machine and its
remote folder as defaults for `--target` / `--remote-folder`, the flash layouts per board
(`setLayoutProvider()` and `mtdPartsRead()` -> `AppSettings::firmwareMtdParts()`, exactly the tab's
wiring). The profile store is loaded but never saved, the source target and folders are not written
back: an agent's run does not change what the tab shows next time, except the layout it learnt.

**Decisions.** Host key: refused, logged with the fingerprint and the way out; `--accept-host-key`
accepts and remembers an unknown key (`accept-new`), never a changed one. Password / passphrase:
answered once from `--password-env VAR` (the variable is removed from su-fw's environment as soon as
it is read, so no tool and no adb server inherits it) or `--password-stdin` (asked again = refused,
exit 4); without either the job's headless default ("authentication failed: nobody answered ...") after
SecretStore's saved secret. On a terminal without `--yes` the questions are asked on stderr (the
password without echo) instead. Download mode: no session to send `reboot loader` through, so the
job's default - the MASKROM instructions and the flasher's wait; a running board is rebooted into
Loader mode over ADB by the flasher itself, as in the tab, with no question (CLI_PLAN "Ask Moses":
decided yes). Secrets never appear in any output: not the password, not the name of the variable,
not a stray positional argument (usage errors do not echo it).

**Output.** Text: `HH:MM:SS text`, the job's lines split like the tab's pane, upgrade_tool's raw
lines without its `(nn%)` progress (the job's `progress()` every 10 % instead), the command's own
lines, last `su-fw: exit <code> (<meaning>): <message>`. `--json`: one object per line, fields in
the order `time` (ISO 8601 with offset), `step` (the job's step, else the command), `event`
(`log` / `progress` / `step` / `done`), `text`, `percent`, `ok`, `exit`, then the command's results
on the `done` line. `--log FILE` appends the same lines (also with `--quiet`); it is opened only
once the whole command line is valid, so a usage error leaves no file behind. English only:
the output is read by scripts, and the CLI's help is not translated; `FirmwareCli` has no `tr()`
strings.

**Exit codes** (`FirmwareCli::ExitCode`): 0 done; 1 failed; 2 usage; 3 no board / wrong mode
(the flasher's "board did not enter MASKROM/Loader", an MCU step without an ADB board, the MCU
commands' status check, `status` without a board); 4 SSH (the pull failed and the connection never
reached Connected); 5 `--timeout` or Ctrl+C. "Board flashed, but ADB did not come back" is 0 with
the warning as the result.

**Ctrl+C and the write in progress.** `interrupt()` and `--timeout` call `FirmwareJob::cancel()`.
The job refuses while upgrade_tool writes the flash (logged); su-fw remembers the request and
cancels when `cancellableChanged(true)` comes (the ADB wait after the write) - from the event loop,
never inside that emission, which comes from the flasher entering the phase - so the result is 5,
the write was never cut, and the result says "interrupted after the image was written; the ADB
wait was cancelled" (an agent must not flash again). An `interrupt()` while the command line is
still read (`--password-stdin`) is remembered and nothing starts; one during a question is deferred
to the event loop. `SIGTERM`, `SIGHUP` (the terminal closed) and Ctrl+Break count as Ctrl+C, so su-fw
stays until the write is over and keeps the lock.

**Nothing meant for su-fw reaches a tool.** A console delivers Ctrl+C, Ctrl+Break and the close of
its window to every process attached to it; a terminal sends Ctrl+C, Ctrl+\ and SIGHUP to its
foreground process group; an agent's harness may signal su-fw's group. upgrade_tool killed during a
write leaves a board that does not boot, so `ProcessToolRunner` (every tool, the GUI's and su-fw's
alike) starts the tools apart from their caller: on Windows with `CREATE_NO_WINDOW` (a hidden console
of their own - what Qt already did for the GUI, which has no console), on POSIX as the leader of a
process group of their own (`setpgid(0, 0)` in the child) that ignores `SIGPIPE` (su-fw gone, its
pipe closed: the write goes on). The output still comes through the pipes. `main.cpp` adds the
POSIX shield for the moment between `fork()` and `setpgid()`: `SIGINT` and `SIGHUP` are ignored while
a tool is started, and the child keeps them ignored. What still cuts a write is a kill of the whole
process tree (or of a job object) - a harness's own timeout must exceed `--timeout` plus the longest
write (about two minutes for a full image) - and an upgrade_tool that hangs silently is never killed
(`--timeout` is no hard limit; the log says so every 5 min).

**One firmware operation per machine.** `FirmwareJob`'s lock (4.11) covers su-fw and the tab alike:
an operation of su-fw while the tab flashes (or the other way round, or two su-fw) fails at once
with "another firmware operation is running (<program>, pid <n>)" (exit 1), and `status` / the MCU
commands' status check stop with the same message instead of querying the board in the middle of
the other's write.

**Tests.** `tst_firmwarecli` (68 cases) drives `FirmwareCli::run()` in-process on the fixtures of
`tst_firmwarejob` (`FakeToolRunner`, `TestSshServer` via `BuildMachine`, `FakeMcuServer`): the help of
every command (every option explained, the exit codes, two examples), usage errors on stderr (and
no secret echoed), every command's happy path, `--json` lines that parse and carry their fields,
no board (3), host key / password / unreachable (4), a failing `WL` with upgrade_tool's tail (1),
`--timeout` and `interrupt()` (5, also during a `UF` and a `WL`: the queued re-sent cancel, the
"image was written" result, nothing logged or run after the operation's end; before the job exists;
during a question), `--partitions` exactly / `--all`, a board on ADB flashed through `cat
/proc/version`, `cat /proc/cmdline` (stored in the settings) and `adb -s <serial> reboot loader`, a
pull with the password saved in SecretStore, "ADB did not come back" (0), the lock held by another
operation (1), `--password-env` gone from the environment the tools see, `pull-flash` updating the
MCU only when the version differs (and the `--role` hint for a silent MCU), `--log` (not created on
a usage error) and `--quiet`, and the built `su-fw` executable through `QProcess` (`--help`,
`--version`, a usage error). `tst_firmwaretools` checks the separation of the tools: a helper started
in a hidden console of its own (su-fw in a terminal) sees its `ProcessToolRunner` child without a
console window and surviving a Ctrl+Break sent to the console, while a plain `QProcess` child shares
the console and dies of it; on POSIX the child leads its own process group and ignores `SIGPIPE`.
Not testable here: a real terminal, a real `WL` - one check on the board before an unattended run
(Ctrl+C, Ctrl+Break and closing the window during a `WL`).

**Several boards at once** (`--serials` / `--all-boards`, v0.7.0) are a separate operation of the
job and a class of their own: 4.13. The single-board path above is unchanged byte for byte.

### 4.13 Parallel SoC flashing (v0.7.0)

The owner's request (`Firmware/cc1310-firmware/analysis/node_inventory/prompt_parallel_soc_flash.md`):
seven Node boards on two hubs, one board takes ~35 s (mostly the reboot into Loader mode and back),
seven one after the other ~4 min; in parallel it should be 40-60 s. Hard rules: only complete
writes (a `WL` is never killed, timed out or cancelled; a cancel waits for the writes in progress),
idblock / uboot / env / meta only when named, a board that does not show up in Loader mode on its
port in time is skipped and reported - never guessed, a dry run first, never two upgrade_tool
processes on one device, every per-board command addressed, the single-board path unchanged.

**Facts it rests on** (measured 2026-10-14 on the bench PC, read-only commands): `adb devices -l`
prints no `usb:` path and `adb get-devpath` answers `unknown` - ADB cannot tell a serial's port.
Windows names the Node board's ADB gadget `USB\VID_2207&PID_0006\<hub>&0&<port>` (no USB serial
string, so a bus address) and the same board in Loader mode `USB\VID_2207&PID_110E\<same hub>&0&<same
port>`: the suffix is the one name of a board across modes (`UsbDeviceInfo::portKey()`).
`upgrade_tool LD` lists every rockusb device with a `LocationID` per USB location (112, 154 here;
opaque - learned, never decoded); `upgrade_tool -s <LocationID> <command>` addresses exactly that
device (SocToolKit passes `-s` with every command); a wrong id answers `No found specific rockusb
device!` with exit 255; **without `-s` a WL / RD with two devices present silently takes one of
them**. `-s <id> RD` brought exactly that board back on ADB in 2-3 s. One rootfs `WL` ~27 s.

**The mapping method** (`FleetFlasher`, one board at a time): a *snapshot* is `LD` and the USB
enumeration, read in that order - `LD` is started first and the enumeration is read when its result
arrives, never before (`LD` takes ~1 s and a board enters Loader mode within ~1 s of `reboot loader`:
an older enumeration would show its ADB instance next to its own new LocationID and read as a foreign
device). Per board: `cat /proc/version`, `cat /proc/cmdline` (the layout gate, below), `before :=
snapshot`, `adb -s X reboot loader`, then a snapshot every poll until `compare(before, after)` decides
or the Loader wait (30 s) runs out. `compare()` is pure and table-driven (vanished = ADB ports gone,
appeared = download ports new, newIds = LocationIDs new; whatever was there before is excluded by the
differences themselves):

| vanished | appeared | newIds | verdict |
|---|---|---|---|
| {} | {} | {} | Pending / StillOnAdb (at the timeout: skipped "reboot loader had no effect") |
| more than one in any | | | Ambiguous: stop ("two boards changed at once") |
| {} | {P}, P still has an ADB instance | | Pending / GhostOnPort (timeout: stop) |
| {} | {Q}, no ADB instance there | | ForeignLoader: stop |
| {} | {} | {n} | Pending / LdOnly (timeout: stop, n never reset) |
| {P} | {Q}, Q != P | | PortMismatch: stop |
| {P} | {} | | Pending / LoaderNotEnumerated (timeout: skipped, "it may be rebooting") |
| {P} | {P} | {} | Pending / LdNotListing (timeout: stop with the driver hint) |
| {P} | {P} | {n} Loader | Mapped (then `adb devices` must no longer list X, else stop) |
| {P} | {P} | {n} Maskrom | Maskrom: that board fails ("not flashed in a fleet") |

A **stop** writes nothing: every mapped board is reset with its own `-s <id> RD`, the board being
mapped fails, the later ones are skipped, and a LocationID nobody can name is listed with the way out
(`upgrade_tool -s <n> RD`) but never reset - an identity the fleet did not learn. After the last
board, the **reconciliation**: one more snapshot; every download-mode port and LocationID must be a
mapped board, a MASKROM board or the baseline's - a board skipped as LoaderNotEnumerated that entered
Loader mode later (during the next board's reads, or after the last mapping) is otherwise absorbed
into a later `before` and invisible; it stops the run like Ambiguous. The same snapshot closes every
run that rebooted a board: whatever sits in download mode and belongs to nobody is reported as
`unattributed` (the summary says so; the boards' results stay), and the run's own failed / not-back
boards found in Loader mode get `leftInLoader` and their addressed retry lines - so does a board a
stop or a cancel reset whose RD answered OK and did not take (both reads must agree; it no longer
counts as reset back to ADB). When exactly one board shows its Loader device on its port without its
id being listed (it re-enumerated) and exactly one LocationID on the bus is nobody's, with no unknown
download-mode port, the two are one device: the board takes the id and its lines are addressed to it
(a WL or an RD that met the gone id, exit 255, ends with runnable lines instead of "find it with LD").

**Before any reboot.** `adb devices` selects (unauthorized / offline / absent: skipped with
`AdbTool::selectDevice()`'s words; an adb that cannot run selects nobody, exit 1; nobody on ADB, exit
3). The baseline snapshot: a Loader / MASKROM device already on the bus that no selected board
accounts for **refuses a real run** (the message names `upgrade_tool -s <n> RD`); the dry run lists it
and goes on. `--all-boards` refuses when adb lists more entries than the bus shows Rockchip ADB
gadgets (a phone would otherwise be told `reboot loader`), and skips a board whose `/proc/cmdline`
reports no layout (another Rockchip product passes the count; it is rebooted only when named with
`--serials`). The **layout gate**: a board whose layout parses and differs from the plan's is skipped
before its reboot, unless `env` is among the rows (the env written defines the next layout) - stricter
than the single board's warning, inert on a bench that runs the image's own layout. The rows are
checked once with `SocFlasher::writePartitions()`'s rules (file, fits, sector-aligned).

**Writing.** Up to `--max-parallel` boards at once (0 = all), each its rows in order, one
`upgrade_tool -s <id> WL <sector> <count> <file>` at a time in a working folder of its own
(`<extract folder>/.fleet/<serial>`: upgrade_tool writes a Log folder into its cwd); the files are the
plan's unpacked images, shared read-only (EXF ran once, in the job's plan). A slot that frees starts
the next mapped board. Before every WL and before the RD the board is **re-checked**: the enumeration
must show the download-mode instance of its Loader mode on its port - the pid it showed when it was
mapped; another pid there (MASKROM after a brown-out) fails the check, so no addressed WL goes to a
device that cannot take it - and no ADB instance; and - **the LD gate** - whenever
no WL / RD of the run is in flight, `LD` must list its id in Loader mode. The gate keeps a
single-flight listing of its own (it answers every board that asks while it runs and for 500 ms after
its result; the mapping's and the reconciliation's LDs never answer a re-check). `LD` opens every
rockusb device and its effect on a running WL is unmeasured, so the two never overlap in either
direction: no LD while a WL / RD runs, and no WL / RD (a check's, a reset's, a stop's or a drain's)
while the gate's LD runs. Mid-wave the enumeration alone re-checks; a re-enumerated id then fails the
next WL with exit 255 and nothing written. `LdPolicy::BeforeEveryWrite` (an LD before every write,
the brief's wording) exists for the bench measurement only - not a CLI option in 0.7.0.

A failed check before a WL fails that board ("left Loader mode before <name> was written (port P
shows ...; LD ...): nothing written for it"); before the RD every row is already on the board, so it
is not reset and the ADB wait decides (back: done with a warning; not back: NotBack). A gate LD that
could not run is no evidence: the board fails with its retry lines. **A failing WL leaves the board
in Loader mode** (no RD: nothing else is written to it) and its lines name the only safe retry -
addressed: one `upgrade_tool -s <id> WL ...` per missing row, then `upgrade_tool -s <id> RD`
(`FleetBoard::retry`, the JSON `retry`; each line names upgrade_tool by the full path the run used -
the bundled tool is not on PATH - so it runs as it is in cmd.exe); `su-fw flash-soc --serial X` only
once `LD` lists X as the only device in download mode, because the single-board flow writes without
`-s` and takes the first Loader device `LD` lists (after a hub brown-out several boards sit in Loader
mode at once). A board whose rows are written is reset at once with `-s <id> RD` (the LD timeout
applies: a timed-out RD is a failed reset, never a cut write; an RD that finds no device - the id is
gone - says so and names no `-s <gone id>` line) and waited for on ADB (`adb -s X get-state` from
one timer, at most one per board), then `cat /proc/version; uptime` and `rfmcu status` (recorded,
never judged).

**Cancel = drain** (`FleetFlasher::cancel()`, Ctrl+C / `--timeout`; `FirmwareJob::isCancellable()`
stays true): selecting - the run ends after the command in flight; mapping - the board already told
`reboot loader` has its wait finished (mapped, skipped or a stop), then every mapped board is reset;
writing - nothing new starts, mapped boards not started are reset (their RDs wait for a gate LD in
flight), a board that has started finishes **every** row and its reset, ADB waits in progress end
(NotBack, "the ADB wait was cancelled; the partitions were written"). A second cancel only says how
many writes are still waited for. **NotBack is a failure** in a fleet (exit 1): the owner's acceptance
is "every board back on ADB", unlike the single board's "flashed, ADB did not come back" (0).

**Classes.**

| Class | What |
|---|---|
| `FleetFlasher` (`src/firmware/FleetFlasher.{h,cpp}`) | The run above: `FleetBoard` (one board as the table and the JSON show it), `Snapshot`, `Verdict` / `compare()`, `UnattributedDevice`; `start()`, `cancel()`, the per-board signals, `report()`. SocFlasher's shape: a run id in every callback, one tool process per board, every line mirrored to `lcFirmware`. `SU_FLEET_SILENCE_MS` (environment, read by the constructor) shortens the 5-minute silence line for the tests. |
| `FleetReport` (`FleetReport.{h,cpp}`) | A value class: the boards, the timing, the stop, the unattributed devices -> `summary()`, `tableLines()`, `json()` / `boardJson()`, `exitCode()` (0 / 1 / 3 / 5, decided from the record alone). |
| `RockchipTool` | `writeLbaOn(id, ...)` / `resetDeviceOn(id)`: the address is in the signature, never state on the object; an empty id is a `Q_ASSERT` and, in a release build, null (nothing runs: the board fails "internal: unaddressed write refused"). `deviceAt()`, `reportsNoSuchDevice()`, and `reportsWriteFailure()` / `reportsResetOk()` hoisted from SocFlasher.cpp (one definition; SocFlasher calls them, behaviour unchanged). The plain `writeLba()` / `resetDevice()` are untouched. |
| `UsbProbe` | `UsbDeviceInfo::portKey()`, `locationPath` (Windows `DEVPKEY_Device_LocationPaths`, Linux the sysfs name; log only), `isAdbGadget()` / `isDownloadMode()`, `Enumerator`, `hasPlatformProbe()`, `describe()`. |
| `FirmwareJob` | Operation `fleet-flash` (`flashFleet()`, "Flash fleet"): its inputs in order (a Ready plan of the image, a known layout - before the selection, which is empty while the layout is unknown -, named rows that can be written, boards, `maxParallel` 0..64, a platform that maps ports), then the flasher with the plan's rows; `FleetOptions`, `PortProvider`, `fleetReport()`, the four board signals. The lock frames it like every operation. |
| `FirmwareCli` | `flash-soc --serials` / `--all-boards` / `--max-parallel` / `--dry-run`: the usage rules (exit 2), the default tags around the explicit ones, the bootloader warning, the one question on a terminal (before the operation, no adb of its own), the board lines and the table, `exit = opOk ? 0 : aborted ? 5 : report.exitCode()`. |
| `AppSettings` | `firmwareBoardPort()` / `setFirmwareBoardPort()` ("firmware/boardPorts"): advisory - the next dry run's "expected port", a real run's "(moved)" note; never addresses a board. |

**Decisions, with their reasons.** `-s` is a signature, never a state (a forgotten reset of a
"current device" would leak an address into the single-board path). One definition of a failed WL
and a done RD. Foreign download-mode devices refuse a real run (nothing is written next to a device
the fleet cannot name). The difference method is a pure table, unit-tested without an event loop,
plus the one cross-check it cannot make (`adb devices` must no longer list X). The snapshot's two
reads are ordered (an LD line without a bus change is polled through as LdOnly, never a foreign
device). The LD gate is the default; mid-wave the enumeration alone re-checks. A failed WL is not
reset and its retry is addressed - never "retry with --serial" alone. A board not back on ADB fails
the run; `--no-wait` ends a board right after its RD. Timeouts: StillOnAdb / LoaderNotEnumerated skip
the board and go on; LdNotListing (a driver problem hits every board) and GhostOnPort stop. Unknown
LocationIDs are never reset. `--max-parallel` defaults to every board. A terminal is asked once; a
pipe is not. The dry run's banner is the operation's own. A board left Loader mode between its last
WL and its RD is fully written: no RD, the ADB wait decides.

**Tests.** `tst_fleetflasher` (59 functions, 77 results with the data rows): `compare()` row by row,
`Snapshot::from()`, `tagFor()`, `FleetReport` (table, JSON, summaries, exit codes) by hand, and runs
on `FakeToolRunner` + `FakeUsbBus` (tests/support: the seven bench boards with their Windows port
keys; every misbehaviour - stays on ADB, vanishes, MASKROM, drags the next board, ghost, no LD line,
another port, adb keeps listing, LD line only, re-enumeration, failing WL / RD, no ADB return, a
phone, a foreign Loader device - and the counters a fleet must keep at zero: unaddressed writes,
LD / write overlaps, cancelled writes): the happy path (one addressed WL and RD per board, a folder of
its own, no EXF), waves and slots, the LD gate (exactly one LD for the first wave), every skip and stop,
the reconciliation and the end-of-run report, the re-checks before a WL, between rows and before the
RD, failing WL / RD with the retry lines, cancel in every phase (RDs wait for the gate's LD), the dry
run, `--all-boards`, the silence line; after review 3 also a MASKROM pid at a mid-wave check, a
stop's / a cancel's RD that answered OK and did not take, an RD that finds no device (and the
pairing with the new id at the end), the baseline refusal naming every port and id, the retry lines
with the tool's path, a WL that cannot start, an RD that times out, a failing `cat /proc/cmdline`, a
stop's failing RD, and an LD listing that a board leaves before its check. `tst_firmwarecli` (+21):
the usage errors, dry run, two boards, `--all-boards`, a phone, JSON lines, not back, Ctrl+C drain,
`--timeout` while mapping and during the writes, `--no-wait`, `--layout`, a stop, a
foreign device, the bootloader warning, the lock, an unknown layout (exit 1, not 2), the question,
`--log` / `--quiet`, and the single-board command lines compared byte for byte with literal lists.
`tst_firmwaretools` (+5): `portKey()`, `describe()`, the addressed command lines, `deviceAt()` /
`reportsNoSuchDevice()`, the hoisted parsers.

**Unmeasured** (decided on the bench, not here): `upgrade_tool LD` next to a running WL (hence the
gate); upgrade_tool's own per-process state with seven instances at once (the Log folder per board is
handled; a shared config file or named mutex is unknown until two run at once - the F+G log shows
whether the two WLs overlapped); USB bandwidth and hub power with seven WLs on two hubs (a brown-out
looks like a failing WL: that board is left in Loader mode with its retry lines); LocationID
stability on a re-enumeration of the same port (mid-wave only the port is re-checked; a changed id
fails the next WL with exit 255 and nothing written); a skipped board entering Loader mode late costs
the run (a stop, exit 1, rerun); a concurrent adb user on the PC shows only as Ambiguous /
ForeignLoader verdicts; Linux (sysfs port keys, the Linux upgrade_tool's `-s` and numbering) is
tested with fakes only.

**Verify after write (v0.8.0, 4.16).** With `--verify` each clean WL of a board is followed by its
read-back, `upgrade_tool -s <id> RL <sector> <ceil(bytes / 512)> <file>` in the board's folder - the
board's one process, counted like a WL for the LD gate in both directions - and a read-back that
differs fails the board exactly as a failed WL does (no RD, `leftInLoader`, the addressed retry lines
with that row first); the table gains the column "verify".

### 4.14 Board profiles (v0.8.0)

The Node (RV1106BG2) and the Stacked core (RV1106G2, `port/integration`) share the medium (SPI NOR,
thunderboot, the same tools) and nothing else that matters to a flash: another partition table,
another chip tag, another Loader PID, no documented MASKROM entry, a CC1310 that holds the SoC's
power. `docs/firmware-update/RV1106G2_FACTS.md` is the source (its rows are cited below); nothing
here is measured, and no stacked fact has run on hardware yet (open questions: FACTS section 3).
`src/firmware/BoardProfile.h` is the contract.

**The facts** (one static table, `BoardProfiles::all()`; a JSON profile file is not 0.8.0):

| field | Node (`node`) | Stacked (`stacked`) | row |
|---|---|---|---|
| name / SoC | Node (RV1106BG2) | Stacked (RV1106G2) | 1 |
| RKFW chip tag | 110E | 1106 | 20 |
| U-Boot rockusb (Loader) PID | 0x110e | 0x110c | 32 |
| ADB gadget product string | rv1106bg2-gc2083 | rv1106g-stacked-gc2083 | 29 |
| flash layout | `sfc_nor:64K(env),256K@64K(idblock),256K(uboot),384K(meta),3M(boot),5M(rootfs),3M(userdata)` | `sfc_nor:64K(env),256K@64K(idblock),256K(uboot),384K(meta),12M(boot),3M(userdata)` | 9, 10 |
| daily partitions | rootfs | boot (the erofs rootfs lives inside, row 11) | 11 |
| MASKROM entry | button | unknown (not documented anywhere) | 42 |
| CC1310 reset cuts the SoC's power | no | yes (DIO8 = SOC_PWR_EN) | 44 |
| Loader wait (a recommendation only) | 30 s | 45 s | 40, D7 |

The loader partitions (idblock, uboot, env, meta) are the same on both and stay
`FleetFlasher::bootloaderPartitions()`. The MASKROM (BootROM) PID is not known on either board (row 33).

**Detection** (`BoardProfiles::detectBoard()` / `detectImage()`, each result with its source and
evidence, `ProfileDetection::text()` the log line). The board: `--profile` / the tab's box (Option) >
the Loader PID `LD` printed for a device in **Loader** mode (LoaderPid) > the ADB gadget's product
string (AdbProduct) > the board's flash layout (BoardLayout: the partition *names* in order; sizes
may change with a build). The image: its RKFW chip tag (ChipTag; `RockchipTool::readRkfwHeader()`, a
file read - no SFI runs, so the pinned 0.7.0 command lists stay) > its env.img layout once the plan
exists (ImageLayout). The override never applies to an image (it names the attached board; the image
says what it is for). A **Maskrom** status hands PID 0 to the detection: its PID is the BootROM's
(row 33), and `kLdMaskrom`'s 0x110c must not read as the stacked board - else rule 1 would block
UF, the only recovery route of a bricked board; `DeviceStatus::pid` stays filled for the fleet's port
check. Where the product string comes from: the USB enumeration on Linux (sysfs `product`); on
Windows the enumeration names the driver, so SocFlasher step 2 and FirmwareJob's status read
`adb -s <serial> shell cat /sys/kernel/config/usb_gadget/rockchip/strings/0x409/product` (the file
usb_adb.sh writes) once per board and flow / job - only when the board's `/proc/cmdline` layout
matched no profile (or there is none, open question 6), so no Node list gains a command. Unknown is
logged (`profile: unknown (...)`) and never guessed: rules 1 and 3 are inert, rule 2 needs both
layouts, and the 0.7.0 behaviour stays.

**Where the rules run** (single board; `SocFlasher::Private::applyProfileRules()`, the one place):
before `adb reboot loader` (step 2, after the layout and product reads, with the plan made first in
**every** case - 0.7.0 only when the board told no layout; the tab and su-fw plan at image selection,
so no EXF is added there) and before the first WL (step 5) - for **every** PartitionWrites flow,
with the detection as it stands then: step 4 detects the adopted Loader device again with its PID
(decision 16), so a G2 that came through step 2 without a readable layout, or through step 3 and a
session's `reboot loader`, is known right before the first WL. A question is asked at most once per
rule and flow (its answer is kept). Never later: a refusal after a WL would be a half-written board.
MASKROM mode applies rule 1 only, before UF (the board has its recovery route by evidence; UF writes
the whole image). Every refusal ends the flow Failed with no RD (nothing was written).

**The rules** (`ProfileRules`, one wording each, `translate("ProfileRules", ...)`):

1. `imageRefusal()`: an image for the other board - "the image is for another board: its chip tag
   110E is the Node (RV1106BG2)'s, the board is Stacked (RV1106G2) (from the board's Loader PID
   0x110c); nothing was written". No override.
2. `layoutRefusal()`: the image's env.img layout against the board's (step 2's read, the owner's
   remembered / `--layout`) - replaces 0.5.1's "the image's wins" everywhere (the plan's line now
   says "... a board laid out that way is refused unless --force-layout (the tab: the question); a
   running board is judged by its own /proc/cmdline" - fix4: the plan's board layout may be the
   Loader-mode memory, which a running board's own layout replaces). `--force-layout`
   / the tab's **Write the image's layout** writes it, with the note.
3. `loaderPartitionRefusal()`: idblock / uboot / env / meta on a profile whose MASKROM entry is
   unknown (the Stacked core) - `--force-loader-partitions` / **Write them**.
4. `socPowerRefusal()`: `mcu-update`, `mcu-set-role` and pull-flash's MCU step on a profile whose
   CC1310 reset cuts the SoC's power - `--confirm-soc-power` / **Proceed**, "once the carrier's
   CC1310 bootloader is known to hold SOC_PWR_EN (the 14 Oct 2026 build or later)". The job's,
   before `McuLink::open()`. No minimum CC1310 version is checked (that fact is in no repo, FACTS D6).

Two more refusals keep the flow honest: `overrideRefusal()` - an override that contradicts a Loader
PID the tool read ("the --profile stacked override contradicts the board's Loader PID 0x110e (the
Node (RV1106BG2)'s); drop --profile"; step 1, step 4, the fleet at Mapped) - because an override is
"the attached board when the tool cannot tell it", never a way to overrule its U-Boot; and the
several-devices refusal (decision 17): when the `LD` of step 1 or a step-4 poll lists more than one
device in download mode, "several rockusb devices are in download mode (LocationIDs 104, 112): the
single-board flow writes unaddressed and cannot tell them apart; ..." before EXF or any WL (a check
on the LD output, no new command; `queryStatus()` keeps 0.7.0's first-device status).

**The fleet's profile gate** (FleetFlasher): the plan's profile (`setPlanProfile()`: the override's,
else the image's) is logged at the baseline ("plan profile: ..." / "plan profile: unknown; the
profile gate is off"); step 2 b detects every board from its layout (and the override), logs its
"profile: ..." line and, before the layout gate, stops the run when a known board profile differs
from a known plan profile - `mixedBenchStop()`, "mapping stopped: mixed bench: ...; nothing was
written" with the 0.7.0 stop machinery (mapped boards reset, later boards skipped, exit 1). A stop,
not a skip: a second profile among the selected boards is a mistake to be told, not worked around
(one plan per profile is not 0.8.0), and the env exemption of the layout gate never bypasses it. At
Mapped (step 2 d) the board is detected again with its mapped Loader PID: a G2 named with `--serials`
whose cmdline carries no mtdparts is stopped there (it IS mapped, so it gets its `-s <id> RD` and
counts as reset) - and `overrideRefusal()` runs at the same point. Rules 1 (with an override) and 3
(on the plan's profile) are `FirmwareJob::flashFleet()`'s input checks, before any adb. A dry run
reads no layout and says so once.

**The Loader wait** stays `waitTimeoutSeconds()` (Preferences): a profile never changes a wait, it
adds `loaderWaitHint()` to the timeout message ("; on the stacked board the CC1310 may have cut the
SoC's power: power-cycle it, then retry", D7). `loaderWaitSeconds` is documentation only (a max()
rule would stretch every fast test and override the user's setting).

**Classes.** `BoardProfile` / `ProfileEvidence` / `ProfileDetection` / `BoardProfiles` /
`ProfileRules` (BoardProfile.cpp, no QObject). `RockchipTool::readRkfwHeader()` / `RkfwHeader`.
SocFlasher: `DeviceStatus::pid` / `locationId` (not compared), `setProfileOverride()`,
`setForceLayout()`, `setForceLoaderPartitions()`, the two handlers, `detectedProfile()`,
`imageProfile()`, `profileDetected()`. FirmwareJob: the override, the three flags, the three handlers
(copied before the call; headless default: refuse), `imageHeader()` / `imageProfile()` (the image
lines "image: update.img, chip tag 110E, built ..." and "image profile: ..." once per image),
`boardProfile()` (the flow's detection wins from a flash until the next status), `boardProfileChanged()`,
`maskromInstructions()` per override. FleetFlasher: `setPlanProfile()`, `setProfileOverride()`,
`FleetBoard::profile` (the JSON's "profile"). su-fw: `--profile` (usage error 2 for an unknown id;
never read from the tab's setting), `--force-layout`, `--force-loader-partitions`,
`--confirm-soc-power`; no profile handler, even on a terminal (the flags are the answers); the
`profile:` line after "Board: ...", `board.profile`, `imageHeader`, `imageProfile`. FirmwareTab: the
*Board profile* box (`AppSettings::firmwareProfile()`), the profile label, the three questions
(QMessageBox, default Cancel, closed by the operation's end), the daily partitions as the default
checks of a new image planned with a known profile; the status line keeps its 0.7.0 text.

**Decisions and why** (the package prompt's list): the image's tag from the RKFW header, never a
second SFI (a pinned list would break); the override is the board's identity, never the image's, and
never against a Loader PID; unknown is inert, rule 2 needs both layouts; the rules at exactly two
points; rule 2 replaces "the image's wins" everywhere but not the fleet's layout gate (env exemption
as in 0.7.0); MASKROM: rule 1 only and no PID evidence; the mixed bench is a stop at step 2 b (a
pre-pass would reorder the pinned fleet sequence, a skip would write around a foreign board); rule 4
is the job's; a profile never changes a wait; the questions are job handlers, answered by the tab
and headless-refused by su-fw; the product string is read from the board only when its layout tells
nothing; the tab's status line keeps its text; `--profile` validated at parse time; the fixtures
carry a real stacked image (six partitions, boot 2 MiB as a stand-in) and every test image an RKFW
header ("110E" by default); the adopted Loader device is detected again (decision 16); two devices in
download mode refuse the single-board flow (decision 17); the fleet's gate runs at 2 b and at Mapped
(decision 18).

**Tests.** `tst_boardprofile` (19: the table, every lookup, the detection order and texts, every
rule's wording, the override refusal); `tst_firmwaretools` (+17, one replacing
`soc_layoutMismatchImageWins`: the RKFW header, pid / locationId, each detection source, the rules
at step 2 / step 5 / before UF, the questions once per flow, the product read, the hint, the plan
before the reboot, the override against the PID, the PID after the reboot, two devices);
`tst_fleetflasher` (+8: the gate at 2 b and at Mapped, the override, the env exemption, the JSON, the
dry run); `tst_firmwarejob` (+8: the image lines, boardProfile() and its signal, the fleet's rules 1
and 3, rule 4 with flag / question / pull-flash / the product string, the MASKROM text, the handlers);
`tst_firmwarecli` (+9: `--profile` usage, the status lines and JSON, the image fields, the three flash
refusals and their flags, rule 4, the mixed bench, the help); `tst_firmwaretab` (+4: the box and
label, the daily checks, the two dialogs). The 0.7.0 pins (`singleBoardPathUnchanged`,
`flashAllInLoaderMode`, `flashBoardOnAdbGoesThroughLoader`, `statusAdbBoardReadsTheLayout`,
`fleetQuestionOnATerminal`) pass unchanged.

**Not in 0.8.0.** One plan per profile in a fleet (a mixed bench stops); the Windows bus-reported
product string (`DEVPKEY_Device_BusReportedDeviceDesc` in UsbProbe; until then the product is read
from the board over adb); a JSON profile file; a CC1310 bootloader minimum version for rule 4; a
per-profile SSH profile / remote folder (FACTS D8).

### 4.15 The layout of a board met in Loader mode (v0.8.0)

A board met in Loader mode - at step 1, or arriving during the wait after step 3 (no board at the
start, then one put into Loader / MASKROM mode by hand or by a session tab's `reboot loader`) -
cannot tell its `/proc/cmdline`. 0.7.0 then used the image's env.img layout blindly or failed
"partition layout unknown"; 0.8.0's rule 2 (4.14) needs the board's own layout to compare with the
image's, and an image without an env layout needs it to place its rows at all. The contract is the
paragraph "The layout of a board met in Loader mode" of `src/firmware/SocFlasher.h` and "Reads from
the board" of `src/firmware/RockchipTool.h`; the facts are `docs/firmware-update/RV1106G2_FACTS.md`
rows 9, 10, 17, 18, 26, 27, 32.

**When** (`SocFlasher::Private::toStepFive()`): the Loader device the step-4 poll adopts after step 1
or after step 3's wait - after the several-devices refusal and the override refusal (4.14), so there
is exactly one device and it is this one - goes through the read first when the flow did not come
through step 2 and the board's own layout is unknown: `mtdParts()` empty (nothing remembered for
"*", no `--layout`), or a remembered layout whose `BoardProfiles::byLayout()` profile contradicts
the adopted device's Loader PID (`byLoaderPid()`). The "*" memory is the layout read last from ANY
board on the PC: after a Node's status a G2 met in Loader mode would inherit the Node's layout, rule 2
would refuse the correct G2 image and `--force-layout` would become the habit. So that case is
logged ("the remembered layout is a Node (RV1106BG2)'s; the board's Loader PID 0x110c says Stacked
(RV1106G2): reading the board's own") and the read runs as if nothing were remembered. The image's
env.img layout does not count as known. A board rebooted by step 2 (its `/proc/cmdline` was read) and
a MASKROM board (UF needs no layout) read nothing. The read runs before the plan: the board's layout
is an input of the plan key and of the rules, so the order at step 5 is read, `ensurePlan()`, rules,
writes. The pinned 0.7.0 command lists pass `--layout`, so none of them gains a PL / RL.

**How** (all addressed with the adopted device's `DeviceStatus::locationId`; a device whose `LD` line
carried no LocationID is never read - "the board's layout could not be read in Loader mode:
upgrade_tool LD printed no LocationID for it"):

1. The env partition, the vendor's own way for exactly these boards (`rkdownload.sh`'s
   `RL 0 1 /tmp/env.img`, row 26), 128 sectors because the env is 64 KiB at sector 0 on both boards
   (row 18): "reading the flash layout of the board in Loader mode (env partition, upgrade_tool RL 0
   128)", `upgrade_tool -s <id> RL 0 128 <extractDirectory()>/.layout/env.bin`
   (`RockchipTool::readLbaOn()`, no timeout, run in the `.layout` folder), then
   `mtdPartsFromEnvImage()` + `parseMtdParts()` -> "partition layout of the board (env partition read
   in Loader mode): <mtdparts>" (`LayoutSource::LoaderEnv`). A file left from an earlier read is
   removed before the read (it is never parsed as this board's); the file is deleted after a
   successful parse and kept otherwise, for a look. Otherwise: "the env partition of the board holds no
   usable layout (<why>); reading the partition table instead (upgrade_tool PL)", why = "not started:
   <error>" / "exit code <n>: <the last lines>" (also an exit 0 whose text says it failed) / "no
   mtdparts in the env partition" / "an unreadable mtdparts in the env partition: <text>" / "cannot
   write <file>".
2. The partition table, `upgrade_tool -s <id> PL` (`partitionListOn()`, the LD timeout): its shape on a
   NOR board whose table lives in env.img is UNVERIFIED (open question 7), so it is tried only when the
   verified source yields nothing, and `RockchipTool::parsePartitionList()` is strict: rows "<index>
   <offset> <size> <name>", "<offset> <size> <name>", "<index> <name> <offset> <size>" or "<name>
   <offset> <size>" (numbers in 512-byte sectors, hex `0x` / `0X` or decimal; names `[A-Za-z0-9_-]+`;
   a size `-` or 0 = "to the end", last row only); the banner, blank lines, "List of ..." lines and
   lines with fewer than two numbers are skipped; every other line must be a row; the table needs two
   rows at least, the first at sector 0, offsets strictly ascending without overlap, no name twice -
   anything else is "no usable table", never a wrong layout. -> "partition layout of the board
   (upgrade_tool PL): <mtdparts>" (`LoaderPartitionList`). Its raw output goes to `toolOutput()` (the
   log pane, su-fw's log) and to `lcFirmware`, so the first G2 answers the question.
3. Both failing is not a failure of the flow: "the board's layout could not be read in Loader mode:
   <why>" (not started / "upgrade_tool PL timed out" / "exit code <n>" / "no partition line"), then
   0.7.0's path - the image's env.img layout when it has one, else "partition layout unknown: ...".

**The rendered layout** (`renderMtdParts()` in SocFlasher.cpp, next to `normalizedLayout()`):
"<device>:" (the env's own device id, "sfc_nor" for a PL table - both boards' device, row 9) and one
"<size>[@<offset>](<name>)" per partition, sizes and offsets in K / M where exact, else bytes, `-` for
"to the end"; `@<offset>` where a partition does not start where the previous one ended, and always on
idblock, whose place the BootROM fixes and every vendor string spells out (`256K@64K(idblock)` on both
boards). So the Node's PL table renders exactly `sfc_nor:64K(env),256K@64K(idblock),256K(uboot),
384K(meta),3M(boot),5M(rootfs),3M(userdata)`, and an env value with "mtdparts=" or "@0" comes out the
same: the plan key and the "*" memory see one form.

**After the read** the layout is `mtdParts()`, reported with `mtdPartsRead(layout)`; the flow's
detection is run again with it as evidence (it only tells something when the PID did not), then
step 5: the plan, the rules with this layout as the board's (rule 2 refuses a differing image unless
forced), the writes. FirmwareJob stores it under the Loader-mode key "" (the "*" memory) and under NO
serial: its `mtdPartsRead` wiring passes the status's serial only when the status is Adb, else "" -
0.7.0 passed `layoutSerial`, the last ADB serial the job had seen, which in the tab (2 s refresh) or
in su-fw after a status of another board is a guess from no evidence.

**Which source the writes used** (`layoutSource()`, decided at step 5, `None` before and in MASKROM
mode): the Loader-mode read when the plan's layout parses equal to it (also when the image's env.img
says the same: the board's own was looked for, and both agree); else `Image` when the plan placed the
rows in the image's env.img layout; else `Board` when step 2's `/proc/cmdline` carried it; else `Owner`
(remembered / `--layout`). The line before the writes follows it: "flash layout in use (from the
image's env.img)" / "(from the board)" (Board and Owner, 0.7.0's texts) / "(from the board's env
partition, read in Loader mode)" / "(from upgrade_tool PL in Loader mode)". `layoutSourceText()` gives
"the layout given for the board" / "the board's /proc/cmdline" / "the image's env.img" / "upgrade_tool
PL in Loader mode" / "the board's env partition, read in Loader mode" ("" for None).

**Cancel and silence.** A read is no write: the phase stays `WaitingForLoader` while it runs (no new
`Phase` value: the tab's phase texts stay), the RL / PL is the flow's `stepInvocation`, and `cancel()`
ends the flow with "cancelled" and cancels the invocation. The RL has no timeout (a slow read is no
failed one); after 5 min without output the log says "upgrade_tool has printed nothing for %1 min
while reading the board's layout; it is left running (Cancel ends it: a read changes nothing on the
board)". The PL has the LD timeout.

**Unverified** (no board has run any of it; everything ran on FakeToolRunner): the shape of
`upgrade_tool PL` on a NOR board whose table lives in env.img (open question 7; the parser is written
for the tool's documented table and refuses anything else); whether the Linux upgrade_tool v2.26 takes
`-s` with PL / RL; RL's progress line ("Read LBA from file (nn%)" is assumed from WL's; nothing
depends on it).

**First-board check** (with the owner): the first G2 in Loader mode (`adb reboot loader` by hand,
`upgrade_tool LD` lists it as the only device): run `upgrade_tool -s <id> PL` by hand once, before any
write, and record its raw output in TEST_REPORT (open question 7); then, with nothing remembered for
"*", `su-fw flash-soc --serial <G2> --partitions userdata --image <G2 update.img>` - the log must show
the env-read line and a parsed layout (or the PL fallback), then "flash layout in use (from ...)" equal
to the image's env layout.

**Classes.** `RockchipTool::partitionListOn()`, `readLbaOn()`, `parsePartitionList()`,
`kEnvPartitionSectors`; `SocFlasher::LayoutSource`, `layoutSource()`, `layoutSourceText()`; the one
FirmwareJob wiring change above. Test fixtures: `kPlNode`, `kPlStacked`, `kPlDecimal`, `kPlGarbage`,
`kPlHeaderOnly`, `scriptLoaderLayoutRead()` (registered before `scriptSocFlash()`, which itself has no
PL / RL).

**Decisions and why** (the package prompt's list): every PL / RL is addressed (a wrong device's table
is a wrong layout); the read only for a board met in Loader mode whose own layout is unknown, a
remembered layout of the other profile counting as unknown; env first (verified), PL second
(unverified, strict), the image's third, unknown last; the read before the plan and every rule; a
layout read in Loader mode remembered under "*" and no serial; cancellable, no new phase; RL without a
timeout, PL with the LD one; the temporary under `<extract folder>/.layout`; one rendered form; MASKROM
and step-2 boards read nothing. Choices of the implementation: `@` always on idblock (else the
rendering could not equal the vendor's string); the parser also refuses overlap, a repeated name, a
fifth column; a stale `env.bin` is removed before the read; a Loader read that equals the image's
layout reports the Loader source.

**Tests.** `tst_firmwaretools` (+27): the PL parser (documented shape, every order and radix, size 0 /
`-`, the rejects, the stacked table), the two command lines, the env constant; the flow: env read, env
failing three ways then PL, PL timing out / failing / garbage / not starting then the image's layout
or "unknown", a known layout and MASKROM and ADB boards reading nothing, rule 2 fed by the read, the
normalized string, every read addressed, cancel during RL and PL, the source texts, a board arriving
after the wait, a remembered layout of the other profile; `tst_firmwarejob` (+1): the read stored
under "" only, also after a status showed another board.

**Not in 0.8.0.** The fleet reads no layout in Loader mode (its boards come through ADB); su-fw's
`layoutSource` JSON field and the tab's partition label still say "board" / "image" (FirmwareCli /
FirmwareTab could show `SocFlasher::layoutSourceText()`).

### 4.16 Verify after write (v0.8.0)

**What and why.** A `WL` that exits 0 says that upgrade_tool wrote; it does not prove that the flash
holds the image - a flaky cable, a hub brown-out or a tool that lies all look the same. On the
stacked board, which has no documented MASKROM entry (4.14), a boot partition that is silently wrong
is expensive. With *verify after write* every partition written in Loader mode is read back right
after its write and compared with the image byte for byte. It is opt-in: the daily Node update stays
as fast as it was. The contracts are the "Verify after write" paragraphs of
`src/firmware/SocFlasher.h`, `FleetFlasher.h`, `FirmwareJob.h`, `FirmwareCli.h` and
`src/ui/FirmwareTab.h`, FleetReport.h's "v0.8.0 additions", and `RockchipTool::readLbaOn()` /
`compareReadBack()` / `ReadBackResult`; the facts are `docs/firmware-update/RV1106G2_FACTS.md` rows
10, 13, 15, 26, 27 and D10.

**The command.** `upgrade_tool -s <LocationID> RL <offset / 512> <ceil(bytes / 512)> <file>`
(`RockchipTool::readLbaOn()`, the read-back of `WL`; the vendor's rkdownload.sh reads the env
partition the same way). Always addressed - no unaddressed read exists: the single-board flow uses
the LocationID of the device its `LD` adopted (`DeviceStatus::locationId`, kept by `toStepFive()` for
the whole flow), the fleet the id it mapped for the board. Its WL / RD stay unaddressed and byte for
byte: the profiles package refuses the single-board flow when `LD` lists more than one device in
download mode (4.14), so the one device the RL addresses is the one the WL wrote. No timeout, never
cancelled, the silence line as for a WL (a timed-out read would call a slow one "not verified").

**The compare** (`RockchipTool::compareReadBack(image, readBack)`): over the image's length, never the
partition's - a jffs2 userdata (row 15) or an erofs boot (rows 11, 13) is smaller than its partition,
and what lies behind its last byte on the flash is not the image's business. The RL reads ceil(bytes /
512) sectors (at least one); the last sector's tail is ignored. Both files are streamed in 1 MiB
blocks (a 12 MiB boot never sits in memory twice). The result: ok, or the first differing byte as an
offset into the image with the byte read and the image's, or "short read: <n> of <m> bytes" (a
shorter read-back), or "cannot read <native path>: <reason>".

**The single board** (`SocFlasher`). A clean WL is followed, before the next row, by `verifyRow()`:
"verifying rootfs: reading 7481 sectors back from sector 8064 (upgrade_tool -s 104 RL)", the RL into
`<extractDirectory()>/.verify/<name>.img` (a stale file there is removed first: it is never compared
as this read's), in the Flashing phase, outside `stepInvocation` exactly like a WL; progress
"Verifying rootfs (6 of 7)" with the RL's own percentage (its "(nn%)" lines parse like WL's). Then
"rootfs verified: 3830272 bytes read back equal the image (4.1 s)", the temporary deleted,
`partitionVerified()`, `verifyResults()`, and only then the next WL. Every failure ends the flow
Failed with no RD - the board stays in Loader mode, nothing else is written, the owner decides:

- a mismatch: "verifying rootfs failed: byte 4660 of the image differs (flash offset 0x3F1234): read
  0x3c, image 0xff; the board is left in Loader mode, nothing else is written";
- a short read: "verifying rootfs failed: upgrade_tool RL read 1024 of 3830272 bytes (short read)";
- an RL that failed: "verifying env failed: <its last lines>" (exit != 0, or exit 0 with a failure
  text, `reportsWriteFailure()`), or "... cannot run upgrade_tool: <error>";
- a gone id: "verifying env failed: No found specific rockusb device! (LocationID 104 is gone)";
- no id at all (upgrade_tool v2.37 always prints one; guarded anyway): "... no LocationID for the
  board (upgrade_tool LD printed none): the read-back needs -s", before any RL.

The temporary stays after a failure, for a look. MASKROM mode (UF) logs "verify after writing: not
available in MASKROM mode (UF writes and checks the whole image itself)" and reads nothing back.
`cancel()` during the RL is refused with the WL's text ("cannot cancel while upgrade_tool writes the
flash"), and FirmwareJob keeps the step uncancellable as during a WL.

**The fleet** (`FleetFlasher`). After a clean WL of row k the "rootfs written: ..." line and
`partitionWritten()` come as before; then, once no gate `LD` is in flight, `startVerify()`: "verifying
rootfs: reading 7481 sectors back from sector 8064 (upgrade_tool -s 113 RL)", the RL into
`<extract folder>/.fleet/<serial>/<name>.img` and run in that folder (where upgrade_tool already
writes its Log), as the board's one process (so never next to its own WL), counted in
`inFlightUpgradeTools`: no gate `LD` starts while it runs and it does not start while one runs - an
RL is counted like a WL in both directions. `boardProgress("Verifying rootfs (1 of 1)", the RL's
percentage)`. A good read-back adds the row to `written` and `verified` (`written` only now: a row
whose read-back failed is not written completely), logs "rootfs verified: ...", emits
`partitionVerified(board, result)` and goes to the next row's re-check, as without verify. A failure
is the failed-WL path: Failed with "verifying rootfs failed: <...>" (the fleet's wording has no "the
board is left in Loader mode ..." sentence: the board's own "left in Loader mode on port P,
LocationID n: nothing else is written to it. Retry by hand, addressed: ..." line follows), no RD,
`leftInLoader`, `verifyFailed` = the row, `verifyMismatch` = the offset (-1 unless a mismatch), and the
addressed retry lines - that row's WL first (it is not in `written`), the rows after it, then the RD.
A gone id gets the WL's gone-id handling (retry empty, "port P shows ...: find its LocationID with
`upgrade_tool LD` before any retry, and address it with -s"). The drain finishes an RL as it
finishes a WL (a board that started finishes every row, its read-backs and its RD). The end-of-run
correction and pairing treat a board failed by its verify as one failed by a WL: a `leftInLoader`
board keeps its lines (nothing doubled); one whose id re-enumerated is paired with the new id and its
lines start with the row that was not verified.

**What the user reads.** FleetReport's real-run table gains the column "verify" between "writing" and
"seconds": "-" when the run did not verify or the board wrote nothing, "ok" when every row in
`written` is in `verified`, "FAIL <row>" when a read-back failed (the detail says why); the dry-run
table is unchanged. `json()` gains "verify", `boardJson()` "verified" / "verifyFailed" /
"verifyMismatch", and su-fw's per-board "step" event carries the same three. A single board's done
object carries "verify" always and, with `--verify` after partition writes, "verified":
[{"name","ok","bytes","mismatch","seconds"}] (absent in MASKROM mode). The exit codes are unchanged: a
failed verify is 1.

**Inputs.** `su-fw flash-soc ... --verify` (one board or a fleet) and `su-fw pull-flash ... --verify`
(`FirmwareCli` -> `FirmwareJob::setVerifyAfterWrite()` before the operation); the Firmware tab's
"Verify after writing" checkbox next to Flash SoC (`AppSettings::firmwareVerifyAfterWrite()`,
"firmware/verifyAfterWrite", default off, stored on toggle, handed to the job by `syncJob()` before
Flash SoC and Pull & Flash, disabled while an operation runs). The job hands it to the SocFlasher in
`startFlash()` and to the FleetFlasher in `startFleet()`, and mirrors the single board's
`partitionVerified()` (to `lcFirmware` as well); the fleet's verdicts are in its boards. su-fw prints
English only, as before.

**Decisions and why** (the package prompt's list). Every RL is addressed (`readLbaOn()` only), with
the device the flow adopted; no second several-devices check (4.14's refusal is the guarantee). An RL
never runs while a WL of the same board is in flight (the owner's rule; the single board is
sequential, the fleet keeps one process per board); across boards an RL may overlap another board's
WL, as WLs overlap today. An RL is counted like a WL for the LD gate, in both directions (`LD` opens
every rockusb device, and `LD` next to an RL is as unmeasured as next to a WL). The compare is over
the image's length. A mismatch is a failed board and is never re-written by the tool: a flipped byte
after a WL that said OK means the flash, the cable or the tool lied, and writing again blindly could
lie again; the fleet handles it exactly as a failed WL, so the owner's recipe for a failed write
covers a failed verify. No re-check before an RL: the re-check guards writes, and an RL fails loudly
on a gone id (exit 255). Cancel during an RL is refused with the existing text (single board) or
drained (fleet): the RL is part of the board's write session, cutting it would be harmless but would
leave "verified" unknown, and one rule is simpler than two. The temporary is per row, deleted on
success, kept on failure. Verify is opt-in and not available in MASKROM mode (UF checks itself). One
table column, the dry-run table unchanged, JSON fields added and none renamed. Choices of the
implementation: the read-back's progress is the row's own percentage in both flows (a write keeps its
percentage over all bytes); in a fleet a row counts as written only once it is verified;
`partitionVerified()` carries the failure text in `error` for every failure, a mismatch included; at
least one sector is read; a stale temporary is removed before the read.

**Tests** (35 new cases, all on fakes). `tst_firmwaretools` (+18): `compareReadBack()` - equal (also a longer,
padded read-back), a mismatch at 0, at 1 MiB - 1, at 1 MiB and at the end (the first of two), short,
unreadable, 12 MiB; the single-board flow on `FakeFlash` (tests/support/FirmwareFixtures: a flash that
remembers what each WL wrote and answers every RL with it) - every row read back right after its WL
(invocation order WL0 RL0 ... RD, the RL's options), the sector counts (ceil, a 1-byte row), the
mismatch (the flash offset computed from the Node layout), the short read, a failing / text-failing /
unstartable RL, the gone id, off by default (0.7.0's list), MASKROM, the progress stage, the
LocationID of the step-4 poll, a jffs2 image shorter than its partition, cancel refused while the RL
runs, never unaddressed. `tst_fleetflasher` (+11, and `reportTableAndJson` extended for the column and
the fields): the happy path on seven boards, a mismatch / short read / failing RL / gone id (each the
failed-WL path), two rows, the LD gate counting an RL, off by default, a drain that waits for a hung
RL, the progress lines, the end-of-run correction and pairing; `FakeUsbBus` got the same read-back
model per board (`setReadBack()`, `readsTo()`, `readsDuringOwnWrite()`, RLs in `ldDuringWrites()` and
`unaddressedCommands()`). `tst_firmwarecli` (+5): `--verify` lines and JSON, a mismatch exits 1,
pull-flash (Loader and MASKROM), the fleet's column and fields, the help. `tst_firmwaretab` (+1): the
checkbox, its setting, and a Flash SoC that reads back; `settingsRoundTrip` / `appSettingsFirmwareKeys`
cover the key. Every 0.7.0 pin (`singleBoardPathUnchanged`, `flashAllInLoaderMode`,
`happyPathSevenBoards`, `ldGateSharesOneListing`, ...) passes unchanged: verify is off there.

**Unmeasured** (no board has run any of it). The RL's cost per MiB on either board - the Node's
3.8 MiB rootfs, the G2's 8-10 MiB boot (D10): the log prints the seconds per row, and the first-board
ladder measures it; RL's progress line text (assumed "Read LBA from file (nn%)" like WL's: only the
percentage parse depends on it, and a missing percentage only means no progress ticks); an RL next
to another board's WL (as unmeasured as two WLs, which the bench measured only in 0.7.0's F+G log);
the Linux upgrade_tool v2.26's RL (and its `-s`).

**First boards** (the integrator with the owner). The Node bench: `su-fw flash-soc --serial F
--partitions rootfs --verify --image <known-good>` (the RL seconds for 3.8 MiB), then the fleet with
`--verify` on two boards, then seven (the column). The first G2, after the bring-up ladder's
`userdata` step: `--partitions userdata --verify`, then `--partitions boot --verify` - the RL seconds
for the 8-10 MiB boot, the number D10 asks for.

**Not in 0.8.0.** A read-back of a MASKROM `UF` flash; a verify-only command (read back without
writing); re-writing a row after a failed verify (deliberately never).

### 4.17 The bench inventory (v0.8.0)

**What and why.** One record per board, keyed by its ADB serial: what the tool knows about it. An
agent reads it before a flash ("which image does board X run?") and the owner after ("what happened
to each board?"). The contracts are `src/firmware/Inventory.h` and `src/firmware/InventoryRecorder.h`.
Nothing in it is measured by the inventory itself; it records what the operations report. A field
keeps its last known value: a record is only ever extended, never cleared by an operation that did
not learn the field.

**The file.** `AppSettings::dataDirectory()/inventory.json`, shared by the tab and every su-fw on the
PC, never between PCs:
`{"version":1,"updated":"<ISO 8601 + offset>","boards":{"<serial>":{...}}}`. A record holds:

- `serial`, `profile`, `port`, `layout`, `gadgetSerialString` (`yes` / `no`);
- `lastImage` {`file`, `chipTag`, `md5`, `gitDescribe`, `partitions`, `written`};
- `mcu` {`version`, `role`, `read`};
- `lastOperation` {`name`, `result`, `message`, `time`};
- `firstSeen`, `lastSeen`.

A field that was never learned is absent. Times carry the local offset, so a file read on another
PC still says when. No secret and no path except the image's file name: the file is meant to be
pasted into a chat.

**Writes.** One JSON file, one lock, one atomic rename (decision 1):

1. `QLockFile <file>.lock` is waited for up to 3 s. A lock left by a dead process, or older than
   30 s, is taken over.
2. The file is read again under the lock.
3. The change is applied to that fresh copy.
4. `QSaveFile` writes the whole file. Reads and commits are retried briefly on Windows sharing
   races.

Two su-fw processes ending at once both land, in some order, and nothing is lost. A corrupt file
(not JSON, not an object, "boards" not an object) makes `load()` false with "<path>: <reason>". The
next write moves it to `inventory.json.corrupt-yyyyMMdd-HHmmss` (never overwritten silently; the
owner logs it) and starts afresh. A newer "version" is read; its unknown fields are dropped by the
next write (accepted in 0.8.0).

**The recorder.** `InventoryRecorder` sits outside the job: FirmwareJob reads no inventory and
changes nothing for it. FirmwareCli creates one in `createJob()` and resets it before the job.
FirmwareTab creates one in its constructor, parented to the tab. It listens to the job's signals and
writes one update per event. A failed write is logged ("inventory: cannot write ...") and never
fails an operation. The events:

- `statusChanged`:
  - One ADB board with a serial: it becomes `lastSerial()` and the record gets lastSeen.
  - Throttled: a write only when the stored lastSeen is a minute old. The tab refreshes every 2 s.
  - While `gadgetSerialString` is unknown, and only when writing, the USB enumeration runs. Exactly
    one ADB gadget gives `yes` (its serial is the status's) or `no` (a bus address, the Node's
    `6&...`, FACTS open question 5).
  - Any other status (Loader, MASKROM, none, several boards) clears `lastSerial()`. Nothing is
    guessed from an earlier status (decision 3).
- `boardProfileChanged`: the profile, only from a detection (Loader PID, product string, layout),
  never from the override (decision 14). It comes after the status, so a first `su-fw status` ends
  with layout and profile.
- `mtdPartsRead` and `boardPortLearned` with a serial set the layout and the port. With an empty
  serial (a layout read in Loader mode) nothing is written, and "inventory: the board in Loader mode
  has no serial; not recorded" is logged once per flow.
- `mcuInfoChanged` sets the version, role and read time.
- `stepFinished` of flash / mcu / set-role / info, for `lastSerial()`:
  - `lastOperation` is set (done / failed / cancelled).
  - A flash that ended ok also sets `lastImage`. Its rows are (fix4) `SocFlasher::writtenPartitions()`
    - the rows the flow wrote completely, never the job's selection -, or every row
    of the image for UF. Its MD5 is the whole file, streamed, cached per path and mtime.
  - The same MD5 merges the rows (first-appearance order) and refreshes `written`. Another MD5
    replaces the image. A failed or cancelled write keeps the old image (decision 4).
- `operationFinished(pull-flash)` sets `lastOperation`.
- `boardFinished` (fleet):
  - Port, layout, profile (only without an override) and `lastOperation` {fleet-flash,
    resultText(), detail}.
  - Done or NotBack also set the image with `FleetBoard::written`.
  - A dry run's Ready boards get lastSeen and the port only. Its other boards get nothing
    (decision 16).
  - A board adb did not list keeps its old lastSeen.

**su-fw.**

- `su-fw inventory [--serial S] [--json]`:
  - It needs no job, no tool, no lock (decision 9).
  - The table has the columns serial, profile, port, image, tag, md5 (8 digits), written, mcu, last
    operation, result, time, last seen; `FleetReport::alignColumns()` lays it out.
  - The done object carries `"inventory": {"file","boards"}`.
  - Exit codes: 0 listed, 1 unreadable, 2 usage, 3 no record for S.
- Every flash logs "image md5: <hex>" after the image lines, and its done object carries `imageMd5`.
  pull-flash logs it after its pull step.
- `flash-soc --expect-image MD5 [--force]`:
  - Only with `--serial` / `--serials` (decision 6). With `--all-boards` the serials are known only
    inside the operation, after adb talked to the boards.
  - The inventory file is checked before `applyLayout()`; for `--all`, right after the plan. No tool
    and no lock are involved, so a refused run has done nothing.
  - A board is refused when its record's MD5 matches (case-insensitive) and its `partitions` hold
    every named row (with `--all`: every row of the image). Exit 1.
  - The MD5 with rows missing is written, with a line naming the recorded and the missing rows. This
    is the daily `--partitions rootfs` / `boot`, and the G2 ladder.
  - A fleet refuses as a whole. `--force` writes anyway.
- Usage errors (2): a value that is not 32 hex digits, no named board, `--force` alone.

**Not in 0.8.0** (decision 10):

- an inventory table in the tab;
- absorbing `firmware/boardPorts` and the per-serial `firmware/mtdparts`. The fleet's PortProvider
  and the layout provider read those settings in pinned paths. The inventory is a report of what
  happened; the settings stay the tool's working memory. Merging them is a later step.

**Tests:**

- `tst_inventory` (35): the file, the lock, corrupt files, the table, md5, and the recorder on a job
  over FakeToolRunner / FakeUsbBus / the in-process SSH server;
- 14 cases in `tst_firmwarecli` and 1 in `tst_firmwaretab`.

**First board:** after the first G2 `su-fw status`, `su-fw inventory --json` answers open question 5
(`gadgetSerialString`) and shows profile "stacked".

### 4.18 The pre-hand-over review's fixes (v0.8.0, fix4)

The Fable review of the integrated 0.8.0 tree (1 blocker, 2 major, 6 minor) before the first G2 board;
each fix is pinned by a test that fails without it.

- **B1 - the "*" layout memory is never a running board's layout nor profile evidence.** "*" (the
  `firmware/mtdparts` entry for a board met in Loader mode) is the layout read last from ANY board on
  the PC. 0.8.0 handed it to the flasher as the board's layout on the ADB path too: a G2 whose
  `/proc/cmdline` carries no `mtdparts` (open question 6) became "Node (from the board's flash
  layout)" after any Node's status - its product read skipped, the G2 image refused by rule 1, the
  false profile written into the inventory - and rule 2 compared the G2 image with the Node layout.
  Now: `SocFlasher::layoutEvidence()` - a layout names a profile only when THIS flow read it (step 2's
  cmdline, the read in Loader mode); a plain status query gives a layout no evidence at all. Step 2
  without `mtdparts` asks `SocFlasher::setLayoutProvider()` - FirmwareJob's per-serial memory
  (`layoutFor(serial)`: its own, then the app's; never "") - for the board's own remembered layout
  ("...; using the layout remembered for it: ..."), else drops the owner's `setMtdParts()` value
  ("...; the layout remembered for a board met in Loader mode (...) is not this board's and is not
  used") and goes on as without a layout (the image's env.img layout, else "partition layout
  unknown"). `setMtdParts()` stays the Loader-mode layout (`boardLayout()` with its "*" fallback),
  checked against the Loader PID by layout-pl. su-fw's `--layout` without `--serial` is that
  Loader-mode layout; with `--serial` it is also the running board's (`applyLayout()` stores both).
  The plan-time warning no longer promises a refusal: "warning: the image's flash layout differs
  from the board layout known so far: a board laid out that way is refused unless --force-layout
  (...); a running board is judged by its own /proc/cmdline" - the job plans with the Loader-mode
  memory before it knows where the board is.
- **M1 - Update MCU / Set role wait for the status's profile.** `FirmwareJob::startMcuUpdate()` fails
  the step with "the board's profile is still being read (its flash layout and gadget product
  string); try again in a moment" while the status's `/proc/cmdline` or product read is in flight
  (`cmdlineInvocation` / `productInvocation`), before rule 4 and `McuLink::open()`: a G2 that appeared
  a second ago is not "unknown" for rule 4. su-fw already waited (`queryBoard()`); refusing (not
  deferring) keeps the step's frame and the lock simple - the user clicks again.
- **M2 - the override cannot change under a running flow.** The Board profile box is disabled while
  an operation runs (`updateControls()`), and `loaderLayoutFound()` runs
  `ProfileRules::overrideRefusal()` against the read device's PID before its re-detection (the
  override outranks the PID in `detectBoard()`).
- **m1** - `SocFlasher::finishQuery()` clears the enumeration's product string for any status that is
  not Adb (Linux: a Node's string never names a G2 met later in MASKROM mode). `setUsbEnumerator()`
  injects the enumeration (the tests, every platform).
- **m2** - `FirmwareJob::computeBoardProfile()` gives a Loader status no layout (a PID the table does
  not know leaves the board unknown).
- **m3** - the tab's daily-partition default is keyed by image, mtime AND profile id and is applied
  from `boardProfileChanged()` too (the image is planned at start-up, the profile arrives with a
  status later); never while an operation runs; `allPartitionsSelected()` still guards the user's own
  checks.
- **m4** - `--expect-image --all` names only the rows `partitionProblem()` allows (a row outside the
  layout is never written, so never recorded); the inventory records `SocFlasher::writtenPartitions()`
  (the rows the flow wrote completely, with `--verify` an equal read-back) instead of the selection.
- **m5** - `tst_inventory::writersInTwoProcesses`: a second `tst_inventory --inventory-writer`
  process and this one write 60 records each at once; every read in between parses, every record is
  there at the end, no lock or temporary is left.
- **m6** - the README's fleet row names `--profile`, `--force-loader-partitions` and
  `--expect-image MD5 [--force]`; `tst_firmwarecli::readmeRowsMatchTheHelp` compares every su-fw row
  of the README with the options the command takes and the two flash-soc rows with `--help`'s synopsis.

Tests: tst_firmwaretools +4 (`soc_starLayoutIsNoEvidenceOnAdb`, `soc_overrideChangedDuringLoaderRead`,
`soc_statusProductNotInherited`, `soc_writtenPartitions`; `soc_layoutUnknown` and
`soc_profileLinesOncePerResult` adapted to B1), tst_firmwarejob +3, tst_firmwarecli +3, tst_firmwaretab
+2 (`busyAroundFlash` also checks the box; `partitionLayoutReadOnceFromBoard` now expects the daily
rootfs check once the read layout made the board a Node, m3), tst_inventory +2.

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
