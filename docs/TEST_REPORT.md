# BuildAI Serial Utility — Test Report (v0.1.0, 2026-09-21)

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
