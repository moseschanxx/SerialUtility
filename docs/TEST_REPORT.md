# BuildAI Serial Utility — Test Report (v0.1.0 2026-09-21, v0.2.0 addendum 2026-09-27, v0.3.0 addendum 2026-09-27, v0.4.0 addendum 2026-09-27, v0.5.0 addendum 2026-10-13)

This report records how the first release of BuildAI Serial Utility was verified on
2026-09-20/21, what was tested at each layer, what was found and fixed, and what could
not be verified on the build machine. Re-run instructions are at the end.

**Build machine:** Windows 10 22H2, MSVC 19.50 (VS 18 Community), Qt 6.8.3 msvc2022_64,
CMake 4.2.3, Ninja 1.13, 4 cores. Final tree: commit `25d3cb4` (code) plus the commit adding this report.

## 1. Summary

| Layer | What | Result |
|---|---|---|
| 1 | Unit tests, UI-free modules (`su_core`) | 11 suites, all green (Release + Debug) |
| 2 | GUI tests, real widgets offscreen (`su_app`) | 5 suites, all green, stable over repeated runs |
| 3 | Multi-lens code review + adversarial verification | 72 findings raised, 67 confirmed, all fixed (22 medium, 45 low) |
| 4 | Deployed executable driven with keystrokes on a real display | 6 scenarios, ~60 screenshots inspected, no crash / hang / error dialog |
| 5 | Stress and throughput | 7 MB replay, 21 tabs, 141 KB paste, 10 reconnect cycles: stable; RX pipeline 0.14 MB/s -> 9.8 MB/s end-to-end after the performance pass (real window) |
| HW | Real serial hardware | J-Link CDC (COM6) loopback verified byte-exact from PowerShell at 9600/115200; the probe vanished before the app could be run against it |

Total automated checks: 660 passed, 0 failed, 2 skipped (environment: no GB18030 codec) (QtTest assertions across 16 suites).

## 2. Layer 1 — unit tests (`tests/tst_*.cpp`, linked to `su_core`)

| Suite | Covers | Checks |
|---|---|---|
| tst_hexutils | hex parsing/dumping, C escapes, display escaping | 32 |
| tst_lineending | CR/LF/CRLF/None mapping and settings keys | 8 |
| tst_commandhistory | bash-like navigation, dedup, cap, draft, load/save | 18 |
| tst_quickcommand | payload resolution, JSON round trips, store load/save/import/export, defaults | 26 |
| tst_charwidth | ASCII/CJK/combining/control/BOM widths | 68 |
| tst_terminalscreen | grid, scrollback, cursor, erase/insert/delete, scroll regions, alt screen, resize, wide chars, selection text | 56 |
| tst_ansiparser | VT500 state machine, split sequences and UTF-8, SGR 16/256/truecolor, DEC modes, DSR/DA replies, OSC, DEC line drawing, garbage resync, realistic boot-log/busybox snippets | 109 |
| tst_serialsettings | SerialSettings map/summary/bauds, port entries, natural sort, connection state | 27 |
| tst_devicesimulator | SIM:loopback/linux/uboot/mcu behaviour, pacing, presence table, reboot → reconnect through SerialConnection | 36 |
| tst_logreplayer | format detection, timestamp stripping, pacing, unlimited, pause/resume/stop/loop | 26 |
| tst_sessionlogger | raw/text/hex-dump formats, header, line-start timestamps, error paths | 8 |

## 3. Layer 2 — GUI tests (real widgets, `QT_QPA_PLATFORM=offscreen`, linked to `su_app`)

| Suite | Covers | Checks |
|---|---|---|
| tst_terminalwidget | every key of the mapping table (incl. DECCKM arrows, F-keys, Ctrl/Alt combos, IME text), copy/paste incl. bracketed paste, mouse selection (drag, double/triple click, Shift-extend, middle paste), drag & drop, scrollback follow/freeze, zoom bounds, DSR/DA replies, bell throttle, find, painting with CJK and every SGR attribute, repaint coalescing, HexDumpView formatting/trimming | 89 (+1 skipped) |
| tst_sessionwidget | connect/disconnect over SIM:loopback, typing/echo, command input (text, hex, escapes, history), quick commands, logging to file (header, TX lines), hex view, send-file via the dialog, stty sync, live baud change, SIM:linux login → `uname -a` → `reboot` → auto-reconnect with system lines, replay (title, refusal while connected, stop), ConnectionBar/CommandInput/QuickCommandBar unit behaviour | 55 (+1 skipped) |
| tst_mainwindow | every action exists with its shortcut and checkable state and triggers without crashing, tabs (new/close/cycle, never zero), hex view, strip visibility persistence, System Log dock sync, zoom, zh_CN ↔ en_US switch incl. Qt base translations, window title, status bar after traffic, confirm-on-close (Yes/No paths), session restore round trip, replay enable states, SystemLogViewer message handler / filter / trimming | 51 |
| tst_dialogs | Preferences load/save for every control, Restore Defaults, QuickCommandModel/QuickCommandsDialog editing and persistence, SendFileDialog state machine and backpressure, About/Version content and copy | 44 |
| tst_terminalperf | end-to-end RX throughput floor and event-loop responsiveness under a multi-MB feed | 7 |

Test hygiene: each GUI suite isolates QSettings (test organisation/application names, IniFormat),
uses a temporary log directory, and waits with `QTRY_*` rather than fixed sleeps.

## 4. Layer 3 — review pass

Seven independent review lenses (terminal emulation, serial robustness, simulator/replay,
Qt correctness/lifetimes, terminal widget, UX/spec compliance, build/CI hygiene) produced 72
findings; each was adversarially verified by a separate agent (67 confirmed, 3 refuted) and
fixed by file group, followed by a full rebuild with all suites on Release and Debug
(commit `2106ace`). Highlights of what was fixed:

- Emulation: alternate-screen resize losing primary content; chunk-boundary UTF-8 repair;
  DEC Special Graphics (ncurses box drawing) implemented; garbage `ESC P/X/^/_` no longer
  swallows output; sub-parameter overflow; U+FEFF width; `?1049` idempotence; CUU/CUD
  clamping to DECSTBM margins.
- Serial: settings rolled back when a live parameter change is rejected; I/O errors on a dead
  handle now trigger the disconnect/reconnect path; reconnect polling reuses the enumerator
  snapshot; clearer disappearance messages; file send has write-buffer backpressure
  (progress reflects bytes on the wire, cancel/disconnect handle the tail); logger header
  newline/failure handling; escape-mode payloads not transcoded.
- Simulator/replay: CRLF `reset` no longer cancels the U-Boot countdown; shutdown lines and
  countdown pacing at low baud; Ctrl+C discards queued output; unlimited replay speed;
  prompt/echo joining when replaying text captures; connect refused while replaying;
  powered-off pseudo-ports can be brought back.
- UI: quick-command group filter with translated group names; language change handled by
  the modeless dialogs; message handler installed before the main window; Preferences
  baud range/defaults/encoding fallback; scrolled-up view stable while scrollback trims;
  one coalesced repaint per 16 ms; hex-dump trimming keeps the scroll position; dirty-row
  painting; Find in the context menu; selectAll without trailing blank lines; BEL burst
  throttle; drag auto-scroll; quit dialog wording; Help menu order; duplicate mnemonics;
  quick-command shortcut safety; Paste enable state; placeholder texts.
- Build/CI: Linux artifact now self-contained; Qt base translations embedded (`qtbase_zh_CN`);
  CMake minimum 3.22; `QT_ROOT` preset no longer overrides the environment; warnings as
  errors in CI; `-Test` with `-Target`; README placeholders and inaccuracies.

## 5. Layer 4 — deployed executable on a real display (`scripts/gui-smoke.ps1`)

| Scenario | Steps | Outcome |
|---|---|---|
| `linux` | `--connect SIM:linux`, boot log, `root` login, `uname -a`, `color`, `chinese`, `ls -l /`, `top -n 1`, `progress`, `dmesg`, unknown command, `reboot`, quit | colours (16/256/truecolor, attributes), CJK, full-screen `top`, `\r` progress bar all correct; port vanished → "Reconnecting..." with placeholder port entry → reconnected with fresh boot log; quit dialog names the connected session |
| `uboot` | `--connect SIM:uboot`, interrupt autoboot, `help`, `printenv`, `bdinfo`, `boot` | countdown interrupted to `=>`; env/board info; boot into Linux login |
| `mcu` | `help`, `AT`, `AT+GMR`, `adc`, `telemetry on/off` | `OK` responses, ADC table, 2 s telemetry lines |
| `menus` | every menu, Preferences, About, Version, Quick Commands, Chinese → English, Ctrl+T/Ctrl+W | all dialogs open and close; complete zh_CN UI; session tabs restored from the previous run |
| `hardware` | `--connect COM6` | the J-Link had disappeared from the system by then ("Port COM6 not found" shown correctly); see §7 |
| `stress` | see §6 | see §6 |

## 6. Layer 5 — stress and throughput

| Test | Result |
|---|---|
| 7.3 MB / 60 000-line coloured log, File > Replay at unlimited speed | UI responsive throughout (Windows "Responding"), memory 102 → 120 MB; throughput about 0.14 MB/s (~1200 lines/s, 52 s for the whole file; the hidden hex-dump view re-laid out its document on every 4 KB chunk) before the performance pass, about 9.8 MB/s in a real window / 10.5 MB/s offscreen (~85 000 lines/s; whole file in under 10 s) with a 50 ms UI timer still firing 18-20 times per second; parser alone 15-17 MB/s after |
| 21 tabs opened with Ctrl+T, 20 closed with Ctrl+W | 135 MB peak, always ≥ 1 tab |
| 2000-line paste (141 KB) into SIM:loopback | transmitted immediately, echoed at the paced 115200 rate, counters correct |
| 10 rapid F3/F2 disconnect/connect cycles then typing | alive, responsive, echo correct |
| tst_terminalperf (automated) | 7/7 passed in 3 s: parser >= 1 MB/s, widget/session/replay paths >= 0.3 MB/s, hex view shown >= 0.05 MB/s, UI timer keeps firing, no stall > 500 ms (measured values printed with qInfo) |

## 7. Real hardware

| Port | Device | Result |
|---|---|---|
| COM6 | SEGGER J-Link CDC UART, TX–RX shorted by the user | 41/41 bytes echoed byte-exact at 9600 and 115200 (DTR/RTS on and off) from a PowerShell probe; 921600 not supported by the probe. Windows had reported "device not functioning" until the J-Link was replugged. The probe vanished from the system again before the application-level run, so the in-app loopback is **not verified** and should be repeated with `scripts\gui-smoke.ps1 -Scenario hardware -HardwarePort COM6`. |
| COM8 | CH343 (Rockchip board console) | Board silent at 115200 and 1 500 000 (powered off or not on the console header); later held by another program on the PC ("access denied") |
| COM9 | CH340 adapter (appeared during the night) | Opens fine; no loopback short |

## 8. Not verified / known limitations

- GB18030 encoding: the installed Qt build has no ICU, so the codec is unavailable and the
  corresponding tests are skipped (UTF-8, ISO-8859-1 verified).
- Right-click context menu of the terminal and QFileDialog-driven flows are exercised only as
  "opens and can be dismissed" (Qt Test cannot drive native modal dialogs).
- Real USB unplug of a live adapter (driver-specific error codes) was not reproducible without
  hardware; the code paths are covered by the simulated `reboot`/`poweroff` tests.
- GitHub Actions (2026-09-21, `moseschanxx/SerialUtility`): the first runs after pushing failed on
  GCC-only warnings (`-Werror=comment`, `-Werror=shadow`), on Qt's deploy script rejecting a
  relative install prefix, on a test-order hazard in `tst_sessionwidget` that GCC turned into a
  `std::bad_alloc`, and on an `ldd` check that measured the runner's `LD_LIBRARY_PATH` instead of
  the package. All fixed (commits `f478bc8`, `b6a3157`); run 35557400491 is green on Ubuntu (GCC 11,
  warnings as errors, 16/16 suites, self-contained tarball started under xvfb) and Windows (MSVC 2022,
  16/16 suites, windeployqt, portable zip and Inno Setup installer). The `release` job runs on `v*` tags.
- `tst_devicesimulator` takes ~30 s (real-time pacing scenarios).

## 9. How to re-run everything

```powershell
.\scripts\build.ps1 -Config Release -Test          # build + all suites (offscreen)
.\scripts\build.ps1 -Config Debug -Test            # same with Q_ASSERTs
ctest --test-dir build\Release -C Release -R tst_terminalwidget --output-on-failure
.\scripts\build.ps1 -Config Release -Deploy        # dist\Release\bin
.\scripts\gui-smoke.ps1 -Scenario linux            # real window + screenshots (keep the desktop free)
.\scripts\gui-smoke.ps1 -Scenario hardware -HardwarePort COM6
```

## 10. v0.2.0 — cmd.exe-style mark mode, right-click paste, Clear (2026-09-27)

Added in 0.2.0: **Pause output while selecting** (a mouse selection freezes the display while bytes queue
up; Enter copies + resumes, Esc cancels; badge + status-bar hint; overflow-safe), **right-click pastes /
copies the selection** (context menu on Shift+right-click or the Menu key), and **Clear wipes screen +
scrollback + hex view** (Reset Terminal stays the full VT reset). Both mouse behaviours are on by default
and switchable in View and Preferences > Terminal.

| Layer | Result |
|---|---|
| Unit + GUI suites, Windows (MSVC, Release, clean build) | 16/16 suites, 706 checks passed, 2 environment skips (GB18030) |
| Unit + GUI suites, Ubuntu 22.04 WSL (GCC 11, `-DSU_WARNINGS_AS_ERRORS=ON`) | zero warnings/errors, 16/16 suites |
| New tests | terminal widget 89 → 120 checks (pause state machine, keys swallowed, overflow, badge painting, right-click copy/paste/Shift-menu, Clear), session widget 55 → 62 (logger/hex view keep flowing while paused, system lines queued, Clear, right-click over SIM:loopback), main window 51 → 56 (actions, two-way sync), dialogs 44 → 46, terminal screen 56 → 57 (clearAll with alternate screen) |
| Adversarial reviews | 2 passes; fixed: status hint leaking across tabs, Clear not wiping the saved primary screen behind an alternate-screen program, a stale drag state disabling right-click after a popup, stale header contracts |
| Real window (`gui-smoke.ps1 -Scenario markmode`, SIM:mcu streaming telemetry) | drag-select → badge "Output paused, N bytes waiting" + status hint, lines held; Enter → clipboard = selection, 5 queued lines appear; Esc → resumes, clipboard untouched; right-click on a selection → copied; right-click without selection → clipboard pasted and echoed by the device |
| Menu tour | View menu shows "Pause Output While Selecting" and "Right Click Pastes (cmd.exe style)", both checked |

Known behaviour choices: a DSR/DA query received while paused is answered when the display resumes (the
query is not parsed earlier); double-clicking a word while already paused first resumes (as conhost does).

## 11. v0.3.0 — SSH sessions (2026-09-27)

Added in 0.3.0: SSH sessions over libssh 0.11.1 (built from source, OpenSSL backend) behind a new
`Transport` interface shared with the serial connection: host-key verification against OpenSSH
`known_hosts`, agent / key / password / keyboard-interactive authentication with dialogs, remembered
secrets (DPAPI), PTY that follows the terminal size, keep-alive, auto-reconnect with backoff, local port
forwards, SFTP upload/download, SSH profiles with import/export, `~/.ssh/config` aliases, Preferences > SSH,
session restore, `--ssh <target>`.

How it was built and verified (one night, four implementation packages in parallel on a stubbed tree,
then integration, adversarial review and fixes):

| Layer | Result |
|---|---|
| Contracts | 9 new headers written first (`Transport`, `SshProfile`/`SshProfileStore`, `SecretStore`, `SshConnection`, `TestSshServer`, `SshConnectionBar`, `HostKeyDialog`, `AuthPromptDialog`, `SshProfilesDialog`, `RemoteFileDialog`); compilable stubs let every package build and test in isolation |
| New unit suites (`su_core`) | `tst_sshprofile` 47 checks (JSON round trips, `parseTarget` table incl. IPv6 and `ssh://`, store load/save/upsert/import/export), `tst_secretstore` 13 (DPAPI round trips, tampered values, keys with `/`, unicode) |
| In-process SSH server (`tests/support/TestSshServer`, libssh server API + own SFTP v3 handler) | `tst_testsshserver` 21 checks driven with the raw libssh client API: password / public-key (plain and passphrase) / keyboard-interactive auth, wrong password, banner, PTY + window change, scripted shell, exec + exit status, 2000-line output, hang + abrupt drop, refused connections, SFTP write/read/stat/realpath/mkdir/rename/readdir/unlink, direct-tcpip echo, host-key regeneration, three concurrent clients, stop while connected; 3 identical runs |
| `SshConnection` end to end against the in-process server | `tst_sshconnection` 41 checks: state sequence, every auth method, wrong password + cancel, host key Unknown / rejected / Changed with `known_hosts` file assertions, saved and stale `SecretStore` passwords, banner, terminal size before and during the session, startup command, exec + exit status, 64 KiB typed burst and 20 000-line output intact, keep-alive, link drop → `connectionLost` → reconnect (and no reconnect when disabled), local forward with 100 KiB echo round trip and `forwardFailed`, 2 MiB SFTP upload/download byte-identical with monotonic progress, overwrite refusal, cancel without `.part`, transfer interleaved with the shell, remote home, `close()` during a host-key question / auth prompt / connect, destruction while connected and while reconnecting, two connections in parallel; ~35 s, 4 identical runs |
| Live probe against a real OpenSSH 8.9 server (WSL Ubuntu 22.04, user `sshprobe`; env-gated part of `tst_sshconnection`, `SU_SSH_PROBE_TARGET`) | 22/22: unknown host key accept + remember, password login, `echo PROBE-OK-42`, resize confirmed by `stty size`, 1 MiB SFTP round trip, overwrite refusals, cancel with `.part` removal, remote home, wrong password → attempt 2 → cancel, three wrong passwords, Changed host key (tampered `known_hosts`) accept + remember leaving one corrected line, Changed + reject leaving the file untouched, connect-once not writing, ed25519 key login via *Public key* and *Automatic*, remote `exit 7` → `channelClosed(7)` with no reconnect; run twice |
| Dialog / bar suite (`tst_sshdialogs`, offscreen) | 44 checks: bar items/order/separator, validation colours, connect button per state, `profileChanged` de-duplication, store rebuild keeping the selection; `HostKeyDialog` per status incl. the checkbox-gated *Replace key*; `AuthPromptDialog` masking / Show / Remember / attempt label / Enter / Esc; `SshProfilesDialog` new / duplicate / delete / validation / forwards / Apply / Connect / Cancel / import / export; `RemoteFileDialog` defaults, `request()`, Start gating, path persistence, retranslate |
| Existing GUI suites extended | session widget 62 → 72 (SSH mode construction, restore keys, `setSshTarget`, connect without target, upload/download refused while disconnected, `connectionLost`/`connectionRestored` lines unchanged for serial, `notifyTerminalSize`), main window 56 → 65 (new actions and shortcuts, `newSshSession`, action enable rules per tab kind, SSH restore round trip, status bar), dialogs 46 → 48 (Preferences SSH page round trip, Restore Defaults), serial settings and device simulator (Transport facade, signal ordering) |
| Whole-application flow (`tst_sshsession`, offscreen, real widgets against the in-process server) | 11 cases (a timer answers the modal dialogs from inside their event loops): ad-hoc connect with Enter in the line edit, Enter on the combo widget and the Connect button (exactly one connect each), host key *remember* / *connect once*, masked password prompt without a Remember box, shell typing, status bar / tab dot / title / bar summary, 256 KiB SFTP upload to the derived `<home>/<name>` path and download back byte for byte, Disconnect; `exit` → `channelClosed` → Disconnected with no reconnect; a profile created in *SSH Profiles* with a saved password connects with one host-key question and no password prompt, reconnects silently, and its JSON holds no secret; restore of a profile tab in a second window; link drop → lost / reconnecting / restored prompt, then Disconnected with auto-reconnect off; a `SIM:loopback` tab echoing next to the connected SSH tab with the action rules flipping per tab; Chinese UI and back with a connected tab and an open transfer dialog; Quit confirmation No / Yes with a clean shutdown and the worker thread joined. ~10 s, 3 identical runs. Its first run caught a real bug: the recent-target / profile-touch update re-applied the bar's bare profile after the Preferences > SSH defaults had been filled in, so every ad-hoc connect ignored the configured known_hosts, identity file, terminal type and keep-alive (and wrote to the real `~/.ssh/known_hosts`); fixed in `SessionWidget::connectSsh()` with a regression test in `tst_sessionwidget` |
| Real window against a real OpenSSH server (`scripts/gui-smoke.ps1 -Scenario ssh`, deployed exe, WSL Ubuntu 22.04 sshd, user `sshprobe`) | Ctrl+Shift+T → target typed + Enter → *Host Key Verification* dialog (SHA-256 / MD5 fingerprints) → *Connect and remember* writes the key into the known_hosts file from Preferences (the user's `~/.ssh/known_hosts` untouched) → *Password* dialog → Ubuntu MOTD and prompt; `echo`, `uname`, `stty size` = 55 × 271 after maximising the window (window-change request), `ls --color`, 300 lines scrolled, hex view, Clear; Disconnect; reconnect from the bar asks only for the password (host known); `exit` → "connection closed (exit status 0)" with no reconnect; a serial tab keeps working; clean exit. The first run of this scenario found a real bug the offscreen suites could not see: on a fresh tab Enter in the target field never connected (the Return key was swallowed at the combo before reaching its line edit when focus sat on the combo itself); fixed in `SshConnectionBar` (Return handled at the combo with a duplicate guard) |
| Serial regression | every previous serial test unchanged and green; a reviewer diffed `SerialConnection` / `SessionWidget` / `MainWindow` against v0.2.0 line by line (signal order, reconnect timer semantics, counters, texts, DTR/RTS/BREAK, live parameter changes, auto-log, replay guard, actions, tab icons, close confirmation) |
| Adversarial reviews | 2 passes (core + test server; UI + dialogs), 12 findings, all verified before fixing: a use-after-free on local-forward sockets closed by the local client, a TCP drop reported as a clean exit (found while writing the end-to-end tests), tail loss when an exit status arrives with output still buffered, partial `ssh_channel_write` dropped, forward bind address `localhost` bound on all interfaces, a re-entrancy hole after `connectionLost`, cross-thread `QSignalSpy` in a test, the real `~/.ssh/config` read by the test suite, restored SFTP paths proposing the previous remote file name (overwrite risk), the serial state text losing its Chinese translation, remote home requested too early, stale tab title after a profile rename, SSH auto-log opened before the connection succeeded; the Linux run additionally exposed two GCC/glibc-only crashes that MSVC tolerated (the forward-socket use-after-free as a segfault in `teardownForwards`, and a `std::bad_alloc` abort from `stateChanged` emitted by `~SshConnection` into a destroyed recorder; the destructor now blocks its signals) |
| GCC 11 (`-Wall -Wextra -Wpedantic -Wshadow -Werror`, WSL Ubuntu 22.04, Qt 6.8.3) | zero warnings/errors; 22/22 suites green on Linux including the SSH suites; Linux tarball check: `libssh.so.4` and `libcrypto.so.3` bundled, `libssh.so` resolves the bundled `libcrypto` through its own `RUNPATH=$ORIGIN` |
| Windows package | `ssh.dll`, `libcrypto-3-x64.dll`, `Qt6Network.dll`, `Qt6Svg.dll` + `iconengines/qsvgicon.dll` deployed next to the executable by `cmake --install`; CI asserts their presence |

Final full runs on Windows (MSVC 2022, Release, fresh configure) and on Linux, on the tree that was tagged:

| Platform | Suites | Result |
|---|---|---|
| Windows MSVC 2022 Release, fresh configure (`build\wf-final`) | 22 | 100 % passed in two consecutive full runs (158 s each; `tst_sshconnection` 47 s, `tst_sessionwidget` 27 s, `tst_sshsession` 10 s); 0 compiler warnings in project files at `/W4` |
| Ubuntu 22.04 WSL GCC 11 `-Werror`, Qt 6.8.3 (aqtinstall), fresh configure | 22 | 100 % passed (0 warnings, 0 errors; `tst_sshconnection` 38.7 s, `tst_sshsession` 15.1 s); `cmake --install` bundle self-contained (`libssh.so.4`, `libcrypto.so.3` resolved from `lib/`, `--version` starts under xvfb) |

CI: run 36274255360 (main, a3c6f0f) and the tag run 36274619537 (v0.3.0) green on Ubuntu (GCC `-Werror`)
and Windows (MSVC `/WX`); the release workflow published `BuildAI-SerialUtility-0.3.0-windows-x64-setup.exe`,
`-windows-x64.zip`, `-linux-x64.tar.gz` and `SHA256SUMS.txt` at
<https://github.com/moseschanxx/SerialUtility/releases/tag/v0.3.0>.

Known limitations (documented in README / DESIGN): no ssh-agent on Windows (libssh), keep-alive
detects a dead link only when the TCP write fails (`SSH_MSG_IGNORE` has no reply), a blocking libssh
phase (connect / key exchange / auth) ends only with the profile's timeout, a refused connect costs the
full timeout on Windows (libssh's select-based poll), only the first `IdentityFile` of a `~/.ssh/config`
block is used, SFTP rename does not overwrite (plain v3), remote port forwards are not offered.

## 12. v0.4.0 — keyboard, remembered SSH credentials, transfer fallback, auto baud (2026-09-27)

Added in 0.4.0: **Ctrl+W and every bare Ctrl+letter go to the shell** while connected (Close Session =
Ctrl+Shift+W), **configurable shortcuts** (*Preferences > Keyboard*), a **Window menu** with Alt+1..9 and
Ctrl+PageUp/PageDown, **Remember** for ad-hoc SSH passwords and key passphrases, an **exec-channel file
transfer fallback** for servers without SFTP (dropbear boards) plus SFTP request pipelining and clean-up of
cancelled uploads, and **automatic baud-rate detection** ("Auto" in the baud list, a garbage watchdog,
*Session > Detect Baud Rate*). Same process as 0.3.0: four packages in parallel on a stubbed tree, integration
with cross-package tests, adversarial review, fixes.

| Layer | Result |
|---|---|
| New unit suite | `tst_bauddetector` 19 checks: text-score vectors (boot log, CJK UTF-8, simulator garbage < 0.4, truncated UTF-8 tail), detection over a real `SerialConnection` on `SIM:linux` opened at 115200 finds 1 500 000, current rate confirmed first, silent device restores the rate, cancel, watchdog quiet period |
| Simulator | `tst_devicesimulator` 45 checks (native rates, deterministic wrong-rate garbage at the same pacing, mid-session rate switch, loopback unaffected, typed input dropped while mismatched) |
| Serial session | `tst_sessionwidget` 83 checks: Auto connect on `SIM:linux` with the Preferences candidates (muted terminal, "detecting" / "1500000 detected" system lines, summary "Auto (1500000)"), watchdog re-detection after the simulator switches to 115200, manual Detect Baud Rate on a fixed-rate `SIM:mcu`, refusals on SSH / disconnected |
| Keyboard | `tst_terminalwidget` 121 checks (reserved-shortcut semantics: reserved Ctrl+X passes to the app, unreserved sends 0x18, Ctrl+Shift always passes, F2 per reservation), `tst_mainwindow` 75 (defaults incl. Ctrl+Shift+W, Alt+1..9, Ctrl+PgUp/PgDn, Window menu, middle click, stored shortcuts applied live, reserved list pushed to terminals, Ctrl+W echoed as 0x17 by `SIM:loopback` and by the in-process SSH server with the tab staying open, Detect Baud Rate enable rules, unique menu mnemonics), `tst_dialogs` 55 (Keyboard page edit / clear / restore / conflict, Connection auto-baud fields) |
| SSH core | `tst_sshconnection` 48 checks + 6 live probes: ad-hoc password remembered under `ssh/target/<user@host:port>/password` (a new connection needs no prompt, a stale one is removed and prompted), passphrase remembered per key file across targets, shell-fallback upload/download of 2 MiB byte-identical with monotonic progress, overwrite refusal, cancel without remote leftovers, remote home via the fallback, SFTP pipelining sizes 0 B .. 3 MiB ± 1, cancelled SFTP upload unlinked; `tst_testsshserver` 24 (exec file commands, 2 MiB `cat` round trip, refused sftp subsystem) |
| SSH UI | `tst_sshdialogs` 55 (remember captions per prompt kind, method texts), `tst_sshsession` 15 + 1 probe: Remember ticked in the real prompt → disconnect → reconnect with no prompt, the same target from a second session, a transfer through `RemoteFileDialog` with `allowSftp = false` ("via shell", byte-identical) and the SFTP path still "SFTP" |
| Real OpenSSH server (WSL Ubuntu 22.04, user `sshprobe`) | env-gated `tst_sshsession::probeRealServerTransfers` through the real `MainWindow`: 3 MB file with a space and a non-ASCII name uploaded to "~" and to "/tmp/", downloaded back byte for byte, missing remote path → readable error, Cancel half-way through 50 MB up and down (no `.part`), second transfer without reopening, drop-target entry, dialog surviving a disconnect; `gui-smoke.ps1 -Scenario sshfiles` (real window, 18 screenshots read): the same flow through the menus. The user's saved source path pointed at a folder (not a supported source); folders are now refused with a clear message, and a dropbear board without SFTP is handled by the fallback |
| Real window on the release build (`gui-smoke.ps1`) | `linux` with `--baud auto`: the bar shows *Auto*, the status bar "Auto (1500000) 8N1", the boot log is readable, the reboot/reconnect cycle works; `ssh`: host-key dialog, key written to the configured known_hosts, password dialog, shell, resize, hex view, Clear, disconnect and reconnect (password only), clean `exit`. The `sshfiles` scenario could not be completed on this build: the desktop was in use (Slack and the Claude window took the foreground mid-typing) and the scenario opened the Session menu with Alt+S, which now goes to the shell while connected; the script was changed to use the toolbar button. Transfers against the real OpenSSH server are covered instead by the env-gated `probeRealServerTransfers` (real `MainWindow` and dialogs, offscreen): PASS on the release build, together with the six connection probes |
| Adversarial reviews | 2 passes on the integrated tree, 12 findings, all verified and fixed: per-candidate buffer purge that made Windows drivers report the port as vanished during detection (critical for real UARTs; now an input-only purge), selecting Auto on a connected fixed-rate session changing the live rate too early, the watchdog re-detecting forever on an unreadable stream (now held after an unreadable search), Alt+letter menu mnemonics stealing readline's Alt+F/B, modifier-less keys and fixed alternates accepted as shortcuts / missed by the conflict check, a spurious bar re-emission cancelling a manual detection, a failed shell-fallback upload leaving a truncated remote file, remote paths starting with "-" parsed as options by cat / rm / chmod, an exit-deadline expiry reported as a successful upload, a disconnect overwriting the transfer error text, and the sshfiles smoke script that could not fail |
| GCC 11 `-Werror` (WSL Ubuntu 22.04, Qt 6.8.3) | zero warnings / errors; 23/23 suites green on Linux |
| Windows MSVC 2022 Release, fresh configure | 23/23 suites green; the eight timing-sensitive suites passed three consecutive runs each (`--repeat until-fail:3`) |

Final numbers on the tagged tree:

| Platform | Suites | Result |
|---|---|---|
| Windows MSVC 2022 Release, fresh configure (`build\wf-final`) | 23 | 100 % passed in two consecutive full runs (236 s each); 0 compiler warnings in project files at `/W4` |
| Ubuntu 22.04 WSL GCC 11 `-Werror`, Qt 6.8.3, fresh configure | 23 | 100 % passed (0 warnings, 0 errors); tarball self-contained (`libssh.so.4`, `libcrypto.so.3` from `lib/`, `--version` under xvfb prints 0.4.0) |

Known limitations: a silent device cannot be detected (the detector never writes; press Enter on the board
or run Detect Baud Rate once it talks); the first 512 garbage bytes of a rate switch reach the terminal
before the watchdog reacts, re-detections are limited to one per 5 s, and after a re-detection that finds
nothing readable at any candidate the watchdog waits for readable output (or a manual Detect Baud Rate)
before it reports again; only single-chord shortcuts are configurable, and Ctrl+Shift+*x* / Alt+*digit*
always belong to the application (Alt+*letter* goes to the shell, so the menu bar is reached with a bare
Alt tap from a connected terminal); a key file named only in
`~/.ssh/config` is prompted for its passphrase per session; the shell fallback cannot preserve remote
permission bits on download; folder upload is not supported.


## 13. v0.5.0 — Firmware tab: pull, SoC flash, CC1310 update over USB-C (2026-10-13)

Added in 0.5.0 (docs/firmware-update/REQUIREMENTS.md, plan and facts in docs/firmware-update/PLAN.md): the
**Firmware tab** (*File > Firmware Update*, Ctrl+Shift+U) pulls `update.img` from the Linux build machine over
the existing SSH stack, flashes the Rockchip SoC with the bundled `upgrade_tool` (the flow of the SDK's
`tools/usb_update.sh`: `LD`, `adb reboot loader`, wait for Loader/Maskrom, `UF`, wait for ADB), and updates the
CC1310 through the board's `rfmcu bridge` with a native C++ port of `fw_update.py` (COBS, CRC16-CCITT, package
CRC32, HELLO/INFO, CLI probe, `power` logged, `bootloader`, BEGIN/READY, DATA/ACK/NACK, END/COMPLETE, SET_ROLE,
restart banner). `adb`, `upgrade_tool` and the DriverAssistant ship in `tools/` next to the executable. The quick
commands gain a **Node** group (`reboot loader` first, `rfmcu console`, the CC1310 CLI commands). Process: five
packages in parallel on a tree of fixed contract headers and compile-only stubs (protocol, tools, ssh-pull, ui,
packaging-docs), integration on both compilers, a read-only review, a fix round, two real-board runs.

| Layer | Result |
|---|---|
| Known-answer vectors | `tests/data/firmware/vectors.json`, `package_small.pkg`, `firmware-0.8.1.pkg`: generated once with the CC1310 repo's `fw_protocol.py` / `fw_package.py` (scratchpad `gen_vectors.py`); neither the build nor the tests run Python |
| Protocol | `tst_ccprotocol` 129 cases: every vector (CRC16 `123456789` = 0x29B1, COBS both ways incl. 254-byte blocks and invalid input, frames and the four rejection texts, INFO 20/24 bytes), `FrameSplitter` at every split point and with garbage between frames, package parse in the Python check order (header CRC32, image CRC32, length, address, version byte), build round trip, the 0.8.1 release package (78,040-byte image, CRC 5FE8FB87) |
| Updater | `tst_mcuupdater` 82 cases against the C++ `FakeMcu` behind a TCP server: info on a running application and on a board in the updater, flash through the SoC (power logged before `bootloader`), serial path without `power`, NACK/resume offsets, lost frames re-sent at the same offset, the retry texts, refused `bootloader`, silent device, target mismatch / oversize, banner seen and `setNoBanner`, set role, cancel mid-transfer then a Part-J re-flash from byte 0, device closed mid-transfer, 16-bit sequence wrap (94,211 requests), the exact `logLine` texts of runbook Part C |
| Tools | `tst_firmwaretools` 97 cases + 1 env-gated probe: `ToolInvocation` lifetimes, `ProcessToolRunner` on real processes (`cmd /c` / `sh -c`, the test exe as a helper: stdin, environment, cwd, timeout, cancel, terminate→kill on Linux), `ToolLocator`, `LD` / `SFI` / `UF`-progress parsers with the outputs seen on this PC, `adb devices` / `forward tcp:0` / `get-state` parsers and every `select_device` message, `UsbProbe` (SetupAPI sees the attached board), `McuLink` open/close with the exact adb_link.py command order, `SocFlasher` phases (already Loader, ADB → reboot loader, no board → downloadModeNeeded, timeouts, success line required, cancel rules, ADB back late / never) |
| Pull | `tst_imagepuller` 21 cases against `TestSshServer` (SFTP and the shell fallback): stat, skip on same size + mtime, forced re-pull, one-level history and roll back, wrong RKFW magic never replaces the image, missing remote file, git describe present / absent, extra files, cancel mid-transfer, dropped connection; `tst_sshconnection` + 4 cases for `runCommand` / `statRemote` (exit status, stderr, order, timeout, missing path, directory, both methods); `tst_testsshserver` 24 |
| Quick commands | `tst_quickcommand` 37 cases: the Node group first, tooltips, file version 2, one-time migration of version-1 files (no double migration, user lists untouched) |
| GUI | `tst_firmwaretab` 33 cases + 1 env-gated real run: widgets and object names, enable rules, settings round trip (folder per target), package / image validation, log time stamps, device status line and driver hint, MCU read (role from `version` or the `help` rows), port-busy and no-reply replies, pull through the in-process build machine (key auth, no prompt), Update MCU and Set role through a fake adb bridge to `FakeMcu`, Pull & Flash end to end with fake tools, retranslation; MainWindow: one tab, session actions off while current, Window menu / Alt+N, close rules while busy, restore keys exclude it, Ctrl+Shift+U listed and reserved, status bar; `tst_mainwindow` / `tst_dialogs` + 2 each (Firmware preferences page); `tst_sessionwidget` adapted to the Node group leading the defaults |
| Review (Fable, read-only) | 1 BLOCKER (quitting during `UF` killed upgrade_tool mid-write), 4 MAJOR (no `killall rfmcu` before the tab's `rfmcu cmd`, the chain gave up on an MCU already in the updater, a stale "send reboot loader" answer acted on after the flash, a remembered role counted as confirmed), 10 MINOR — closed in the fix round (see reports in the session scratchpad) |
| Real board 0225ce3c8d8d6e44, probe (`tst_firmwaretools::realBoard_statusAndMcuInfo`) | `upgrade_tool LD` + `adb devices` → "ADB device 0225ce3c8d8d6e44"; `McuLink` killed the stale `rfmcu console` (pid 190), forwarded a port, started `rfmcu bridge 7000` ("bridging" seen), connected; `McuUpdater::info()`: HELLO unanswered (application running), CLI probe → `version=0.8.2 role=rx` (the role from the `help` rows: 0.8.2's `version` reply carries no `role=`); link cleaned up (`killall rfmcu`, `forward --remove`) |
| Real board, Firmware tab (`tst_firmwaretab::realBoard_pullFlashUpdate`, real `ProcessToolRunner`, real VM) | Read MCU → 0.8.2 rx; Pull: host-key and password dialogs answered from a timer, `update.img` 11.4 MB (built 2026-10-14 00:51 VM time, `git describe` v1.3.0-5-gbff7fca34-dirty) downloaded in 2 s and RKFW-checked; Flash SoC: `LD`, `cat /proc/version` (5.10.209+), `adb reboot loader`, Loader mode after 1-2 s, `UF`: `Test Device Success` then **`Check Chip Fail`** (exit 255, nothing written); Set role rx→rx: bridge, `INFO power soc=on supply=usb usb=1`, "rebooting it into the UART updater", `COMPLETE`, "application restarted: version=0.8.2 role=rx" |
| Why `UF` failed, and the fix | the tool's log (`log_dir=` in a `config.ini` next to it) says `CheckChip-->Chip is not match,firmware(0x31313045),device(0x0)` and `upgrade_tool RCI` reads 16 zero bytes from the loader: the board's U-Boot (`arch/arm/mach-rockchip/chip_info.c`) has no rockusb chip-info case for `CONFIG_ROCKCHIP_RV1103B`. The Linux `upgrade_tool` v2.26 carries the same check; the SDK's own tag notes verified Loader-mode flashing with the SocToolKit GUI only. Proposed one-line U-Boot change: `docs/firmware-update/uboot-rockusb-chip-info.patch` (`chip_info[0] = 0x31313045`, "110E"). `upgrade_tool RD` brought the board back to its firmware after each attempt. The Firmware tab's own flow is unchanged by this: with that U-Boot on the board (flashed once by SocToolKit or MASKROM), or with a board in MASKROM mode, `UF` passes Check Chip |
| GCC 11 `-Werror` (WSL Ubuntu 22.04, Qt 6.8.3) | two GCC-only defects found and fixed before CI (an in-class `= Options()` default argument; stdin written while a `QProcess` is still Starting); full tree 0 warnings; see the final table |
| Windows MSVC 2022 Release | full tree 0 warnings at `/W4`; see the final table |

After the fix round both real-board cases were run again on the released build (the board had been unplugged
for a while and was plugged back at 14:2x): the tools probe passed (ADB device, bridge, `version=0.8.2 role=rx`),
the Firmware tab run passed (Read with `killall rfmcu` first, pull from the VM with the host-key and password
dialogs, Set role rx->rx: power logged, bootloader entry, `COMPLETE`, restart banner, 12 s in all). The loader's
chip id read `0 0 0 ...` again (the U-Boot change is not on the board yet), so the SoC flash was not retried.

Not re-flashed: the CC1310 runs 0.8.2 and the only 0.8.2 package disappeared from the local CC1310 checkout
while the run was prepared (a branch switch there), so the real MCU test used Set role instead of Update MCU;
Update MCU with a package is covered by the fake-bridge GUI case and the updater suite, and the DATA path is the
same code the Python tool's verified run used.

Final numbers on the tagged tree:

| Platform | Suites | Result |
|---|---|---|
| Windows MSVC 2022 Release (`build\Release`, after the fix round) | 28 | 100 % passed (338 s); 0 compiler warnings in project files at `/W4 /WX` |
| Ubuntu 22.04 WSL GCC 11 `-Werror`, Qt 6.8.3 (`~/build/su05`, after the fix round) | 28 | 100 % passed; 0 warnings, 0 errors |

Known limitations: `UF` in U-Boot Loader mode needs the U-Boot chip-info change above (until then: MASKROM mode,
or the Rockchip GUI tool); the SoC flash cannot be cancelled once `UF` writes (the window refuses to close
during it); a hung `upgrade_tool` is logged after five minutes without output but not killed; the driver installer
is offered only in an administrator install; the pull keeps one previous image; extra files are plain names in
the remote folder; `rfmcu` on the board is stopped before every MCU command, so an interactive `rfmcu console`
someone left open ends when the tab talks to the MCU.

## 14. v0.5.1 — SoC flash by partition, the way SocToolKit's Download page does it (2026-10-13)

Why: in 0.5.0 the SoC flash ended at `upgrade_tool UF`, whose chip check the Node board's U-Boot fails in Loader
mode (section 13). Moses: the program must work like the official tool - "flash by partition, it should be like
options you can select ... you just want to flash a certain partition". Rockchip's SocToolKit (a PyInstaller /
PySide6 application; its Python sources carry the LGPL header) was unpacked and read: its Download page lists the
device with `upgrade_tool ld`, writes each partition with `-s <dev> wl <offset / 512> <size / 512> <file>` (no
chip check) and resets with `rd`; `uf` runs only behind its Upgrade Firmware button.

What changed (4503b41 + the WL size fix): `RockchipTool` gained `EXF` / `WL` / `RD` and the parsers (`mtdparts`
from `/proc/cmdline` or the image's `env.img`, `package-file`, `planPartitions()`); `SocFlasher`'s Loader-mode
method is now `PartitionWrites` (`EXF` into `<image>.extracted`, plan, size check of every checked row before the
first write, one `WL` per checked row in package-file order with the partition size in sectors, `RD`, wait for
ADB; MASKROM mode keeps `UF`; layout precedence image `env.img` > board `/proc/cmdline` > remembered per serial,
a mismatch is logged); the Firmware tab lists the image's partitions with a checkbox each (name, file, offset,
partition size, image size; All / None; rows not in the layout or too big are disabled with the reason), Flash
SoC writes the checked rows, Pull & Flash writes all; `AppSettings` remembers the layout per board serial;
README, DESIGN 4.11, VERSION.txt, translations.

| Layer | Result |
|---|---|
| Tools | `tst_firmwaretools` 114 cases + 1 env-gated probe (was 97): `parseMtdParts` (with / without `mtdparts=` and the `sfc_nor:` prefix, `@` offsets, K/M/G and bytes, `-` size, malformed → empty), the real cmdline, `package-file` (tabs, spaces, comments), `planPartitions` on a temp tree (sizes, order, the two skipped entries, a name missing from the layout, an image too big → fits false), the command lines (`EXF <img> <dir>` with the directory created, `WL 1920 6144 <file>` / `WL 0 128 ...` / `WL 18304 -1 ...` for "to the end", `RD`), the Loader flow end to end on `FakeToolRunner` (`EXF` handler creating the files, 7 `WL` at sectors 0 / 128 / 640 / 1152 / 1920 / 8064 / 18304 with the right files, `RD`, ADB back, the "Writing <name> (k of 7)" stages with the overall percentage), a selection of two rows writes only those, an image too big stops before any `WL`, a failing `WL` stops with its tail, layout unknown, MASKROM → `UF` unchanged, `mtdPartsRead`, plan staleness |
| GUI | `tst_firmwaretab` 47 cases + 1 env-gated real run (was 33): the table after `setImagePath()` with a fake `EXF` and a remembered layout (rows, offsets, checks, label), All / None, the selection handed to the flasher, the layout-unknown text, the layout read once from a fake ADB device and stored per serial, Flash SoC in Loader mode writes exactly the checked rows (`WL` lines), MASKROM ignores the selection (log line) |
| Real board 62ecfc6ddeb3f579, first Pull & Flash on 4503b41 | Read MCU 0.8.2 rx, pull, `EXF`, layout from the image's `env.img` (= the board's cmdline), `adb reboot loader`, Loader mode after 1 s, `WL` of env, idblock, uboot, meta, boot, rootfs OK, then `WL 18304 userdata.img` refused: `Image is jffs format,please tell the size!` (exit 255) - the flow stopped as designed, nothing more written. Fix: `WL` now carries the partition size in sectors (`-1` for "to the end"), exactly what SocToolKit passes; userdata was then written by hand with `WL 18304 6144 userdata.img` (100 %), `RD`, board back on ADB after 3 s |
| Flash verification | Loader-mode readback `upgrade_tool RL <start> <count> <file>` of env (128 sectors), boot (6144) and rootfs (10240): byte-identical to the image files over the image length (`cmp`). Linux-side reads on the board are NOT repeatable: `md5sum /dev/mtd0` and `/dev/mtdblock0` give a different value on every read (a few bytes differ at 4 KiB boundaries; `adb exec-out` is not supported by the board's adbd, so copies went through `/tmp` + `adb pull`) - a flash cannot be judged from the running system, only from Loader mode. The kernel inside the current build image is still the Sep 30 20:24 build, so `/proc/version` does not change after a flash |
| Real board, Pull & Flash all seven (fixed `WL`) | PASS, 105 s (`tst_firmwaretab::realBoard_pullFlashUpdate`, `MCU_ACTION=none`): Read MCU 0.8.2 rx; pull with the host-key and password dialogs; `EXF`; layout from the image's `env.img`; `adb reboot loader`; `WL` env 0/128, idblock 128/512, uboot 640/512, meta 1152/768, boot 1920/6144 (23 s), rootfs 8064/10240 (27 s), userdata 18304/6144; `RD`; "board is back on ADB" after 3 s; 16:50:36 → 16:52:20 |
| Real board, one partition (`SU_FIRMWARE_REAL_PARTITIONS=boot`) | PASS, 35 s: the table shows 7 rows with boot alone checked ("7 partitions, 1 selected"); six `skipping <name>: not selected` lines; one `WL 1920 6144 boot.img` (23 s); `RD`; ADB back after 3 s; method `PartitionWrites` |
| GCC 11 `-Werror` (WSL) on 4503b41 | 28 / 28 passed, 0 warnings (before the `WL` size change; the rerun is in the final table) |

Final numbers on the tagged tree:

| Platform | Suites | Result |
|---|---|---|
| Windows MSVC 2022 Release (`build\Release`, 679b7f2) | 28 | 100 % passed (375 s); 0 compiler warnings in project files at `/W4 /WX` |
| Ubuntu 22.04 WSL GCC 11 `-Werror`, Qt 6.8.3 (`~/build/su05`, 679b7f2) | 28 | 100 % passed; 0 warnings, 0 errors (16:56 - 17:05) |

Known limitations: a `WL` cannot be cancelled once it writes (like `UF`); the U-Boot chip-info change is only
needed for `UF` (MASKROM mode) now; the CC1310 package re-flash is still not covered on hardware (no 0.8.2
package at hand); the partition table trusts the image's `env.img` when the board's layout differs (the env being
written defines the new layout) - the mismatch is logged, not refused.

## 15. v0.6.0 — `su-fw`: the firmware flows from a command shell, for agents and scripts (2026-10-13)

Why: Moses - "the AI agent should also be able to invoke some command, then update the firmware in the
background, in a command shell". Plan: docs/firmware-update/CLI_PLAN.md (the three open questions were
decided by default: the tool is `su-fw`, `--json` is included, `reboot loader` goes through ADB
automatically). Two packages on Opus: **job** moved the tab's chain into the UI-free `FirmwareJob`
(su_core; the dialogs became callbacks with headless defaults; the tab is a view over it, every tab case
kept), **cli** added `FirmwareCli` + the `su-fw` console executable (console subsystem, `QCoreApplication`,
su_core only; installed next to the GUI in every package; CI checks `su-fw --version` / `--help` on both
platforms). Commands: `status`, `partitions`, `pull`, `flash-soc`, `mcu-info`, `mcu-update`,
`mcu-set-role`, `pull-flash`; exit codes 0 done / 1 failed / 2 usage / 3 no board / 4 SSH / 5 timeout or
Ctrl+C; secrets only through `--password-env`, `--password-stdin`, `--identity` or a saved profile.

| Layer | Result |
|---|---|
| Job | `tst_firmwarejob` 60 cases: inputs reach the collaborators, refuse-while-busy, step / operation order, pull with the host-key and auth callbacks (key remembered for a fresh job), the headless refusals ("Host key rejected for <target>", `authentication failed: nobody answered "<prompt>"`), no secret in any log, cancel and disconnect; Loader flash of selected rows (`EXF`, `WL` with sectors and counts, `RD`, ADB back) and of all rows, MASKROM `UF`, the size check before any `WL`, nothing checked, layout unknown, cancel while waiting and refused during a write, four download-mode outcomes; layout read once per board, provider versus own memory, layout from the image, plan states; MCU info (`killall rfmcu` first, role from `help`, exits 2 / 3 / 1, garbage), update through the fake bridge (progress per ACK, banner), set-role, link failure, detached cleanup; Pull & Flash: MCU skipped when versions match (SSH kept across the chain), updated when they differ, updated when silent with a confirmed role, stops when silent without one, stop at the first failure, no package, every partition written |
| CLI | `tst_firmwarecli` 57 cases in-process (`FirmwareCli::run()` with `FakeToolRunner`, `TestSshServer`, `FakeMcuServer`) + 1 on the real executable: every command's happy path and lines, `--json` lines parse with the fields, usage errors exit 2 on stderr, no board 3, host key refused / wrong password 4, a failing `WL` 1 with the tail, `--timeout` 5, `--password-env` / `--password-stdin` reach the prompt and never the output, `--partitions` exact rows and `--all`, `pull-flash` runs the MCU step only when the package differs, `--log` holds the same lines, `--help` / `--version` / a usage error through `QProcess` |
| GUI | `tst_firmwaretab` 47 cases kept (one changed line: the collaborators are checked on the tab's job) |
| Real board 62ecfc6ddeb3f579 through `su-fw` (build/Release, bundled tools) | `status --mcu` → `ADB device 62ecfc6ddeb3f579; MCU version=0.8.2 role=rx` (exit 0, also as JSON); `partitions --image` (the local copy of the VM's current update.img) → 7 rows, 7 writable, layout from `env.img` (text and JSON); `flash-soc --partitions nope` → exit 2 naming the seven real names; `flash-soc` without a selection → exit 2 "choose what to write"; `flash-soc --partitions boot --yes --json` → 48 progress objects, `reboot loader`, `WL 1920 6144 boot.img`, `RD`, back on ADB, exit 0 in 30 s; `flash-soc --all --yes` → 7 `WL` with the sizes, exit 0 in 100 s (19:26:40 → 19:28:19); `status` after each flash; `mcu-info` exit 0; `mcu-info --serial nosuchboard` → exit 3 naming the attached board; `--timeout 1` → exit 5 "timeout after 1 s: cancelled"; grep of every output and log file for the password: 0 hits |
| Real board through the fixed `su-fw` (14f6008, tools started apart from the console) | `status --mcu` exit 0; `flash-soc --partitions userdata --yes` → `reboot loader`, `WL 18304 6144 userdata.img`, `RD`, back on ADB, exit 0 in 39 s (20:36:34 → 20:37:13); a second `su-fw status` started 6 s into that flash → `status paused: another firmware operation is running (su-fw, pid 13584)`, exit 1; `status --mcu` afterwards exit 0; password grep over every output: 0 hits. Not exercised: Ctrl+C / Ctrl+Break / closing the window during a write (needs a person at a real console) |
| Real VM pull through `su-fw` | Not run on hardware: the build VM 10.0.10.71 became unreachable at 19:23 (ping lost, `No route to host`) and stayed so; against the unreachable host `su-fw pull` and `pull-flash` answered `exit 4 (SSH): Cannot connect to moses@10.0.10.71: Timeout connecting to 10.0.10.71` in 15 s, `--timeout 1` → exit 5, a wrong password → exit 4. The pull path is the same `ImagePuller` / `SshConnection` the tab ran against the real VM at 16:50 (section 14) and is covered in-process by the CLI suite (host key accepted / refused, saved password, `--password-env`, `--password-stdin`, unchanged skip). To run when the VM is back: `su-fw pull --target moses@10.0.10.71 --password-env SU_PASSWORD --accept-host-key --yes`, then `su-fw pull-flash ... --yes` |
| Review (Fable, read-only) and the fix round | 0 BLOCKER, 4 MAJOR, 8 MINOR (scratchpad reports/review2.md). MAJOR: the tools were reachable through su-fw's console and process (Ctrl+Break, a closed window, SIGHUP, a process-tree kill ended upgrade_tool mid-write) - now every tool starts apart from its caller (`CREATE_NO_WINDOW` on Windows, own process group with SIGINT / SIGHUP / SIGPIPE ignored around `start()` on POSIX; a harness that kills the whole tree still cuts a write, said in README and `--help`); the cancel re-sent after a refused one re-entered the flasher and reported "cancelled" for a written image - queued now, result "interrupted after the image was written; the ADB wait was cancelled"; no guard between the GUI tab and su-fw - a machine-wide `QLockFile` (`firmware.lock`) now refuses a second operation ("another firmware operation is running (<program>, pid <n>)", su-fw exit 1) and pauses the tab's status queries; the ADB-board flash and the saved-password pull were untested through su-fw - tests added. MINOR fixed: `--password-env` variable unset after reading (children never inherit it), early Ctrl+C kept, `--role` hint, `layoutReadFinished()` signal instead of matching log texts, console code page restored on every path, `--log` opened only after validation, `--timeout` help. Suites after the round: `tst_firmwaretools` 119, `tst_firmwarejob` 62, `tst_firmwarecli` 68, `tst_firmwaretab` 49 (14f6008) |
| GCC 11 `-Werror` (WSL) | 30 / 30 passed, 0 warnings (0b2d0e9; the fix round's first commit 14f6008 failed GCC's `-Werror=comment` on one comment line ending in a backslash - CI caught it on Linux, fixed in 0b2d0e9) |
| Windows MSVC 2022 Release | 30 / 30 passed (450 s), 0 warnings (14f6008; 442 s on dcfe214 before the fix round) |

Known limitations: `pull-flash --partitions` is refused (Pull & Flash always writes every partition; use
`pull`, then `flash-soc --partitions`); `flash-soc --all` refuses an image that does not fit its partition
where the tab's "All" would skip that row; Ctrl+C during a write is handled (upgrade_tool never sees it,
su-fw stops at the next safe point) but was not exercised on the board; the GUI's quick commands and
dialogs are untouched by 0.6.0.
