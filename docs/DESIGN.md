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
| Layout | `src/app` (settings, logging, version), `src/core` (serial + helpers), `src/terminal` (emulator), `src/ui` (widgets), `src/dialogs`, `tests/` |
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

Three CMake targets: **`su_core`** (static lib: `src/app`, `src/core`, `src/terminal`
minus `TerminalWidget`; links Core/Gui/SerialPort; used by the unit tests), **`su_app`**
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
can be remembered in `SecretStore` under the profile id) → PTY (`xterm-256color`, the terminal's
current grid) + `shell` (or `exec` for a remote command) → `shellStarted`. `~/.ssh/config` is
parsed by libssh so `Host` aliases, `IdentityFile`, `User`, `Port` and `ProxyJump` from the user's
config apply; explicit profile fields win (the environment variable `SU_SSH_IGNORE_CONFIG=1` skips
the config; the test suites set it). Terminal resizes become window-change requests. A
keep-alive (`SSH_MSG_IGNORE`, 30 s default) detects dead links; a dropped link (not a clean
`exit`) goes to Reconnecting with 2 → 30 s backoff using only non-interactive credentials.
Local port forwards (`-L`) are `QTcpServer`s in the worker bridged to `direct-tcpip` channels.
File transfer uses the SFTP subsystem of the same session (`RemoteFileDialog`, drag-and-drop of a
file onto an SSH terminal = upload) in 64 KiB slices interleaved with the shell traffic.
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
server (`tests/support/TestSftpHandler`; libssh's own SFTP server is compiled out on Windows),
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

## 7. Future work

TCP/telnet transport; XMODEM/YMODEM; search in scrollback UI; split view; Linux packaging
(AppImage/deb); auto-update check; SSH: agent authentication on Windows (libssh has no agent
support there), remote port forwards, SFTP browser.
