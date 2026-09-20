# BuildAI Serial Utility - Quick Start

One page to get from a fresh checkout to a Rockchip boot log. Details: [`../README.md`](../README.md).

## 1. Build (Windows, 2 minutes)

```powershell
git clone <repo-url> SerialUtility
cd SerialUtility
.\scripts\build.ps1 -Config Release -Test      # needs Qt 6.8.3 MSVC2022 at D:\Qt (or set QT_ROOT)
.\scripts\run.ps1   -Config Release
```

Linux: `cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build && ./build/BuildAI-SerialUtility`

Qt Creator: open `CMakeLists.txt`, pick the `Release` preset, press Run.

**No board on the desk?** Pick `SIM:linux` in the port list and press **Connect**: a
Rockchip-style boot log scrolls by, log in with any name, then try `help`, `top -n 1`,
`dmesg | tail -n 50`, `color`, `chinese` and `reboot` (the port vanishes and auto-reconnect
brings it back). `SIM:uboot` gives you the autoboot countdown, `SIM:mcu` an AT-style shell,
`SIM:loopback` echoes everything. Uncheck *Preferences > Connection > Show simulated devices*
to hide them.

## 2. Connect a Rockchip board

| Step | What to do |
|---|---|
| Plug in | USB-UART adapter (CH343 / CP2102) on the board's debug header |
| Port | Pick it in the connection bar - the list updates live, **F5** rescans |
| Baud | **1500000** (RV1106 / RV1106B, recent SDKs) or **115200** (older boards) - 8N1, no flow |
| Connect | **F2** (or the Connect button). Power / reset the board |
| U-Boot | Hold **Ctrl+C** during "Hit any key to stop autoboot" - or click *Control > Ctrl+C* |
| Linux | Log in; Enter sends CR and Backspace sends DEL by default (what a tty expects) |

If the text is garbage, you have the wrong baud rate. If lines step to the right like a
staircase, enable *Preferences > Terminal > Implicit carriage return on line feed*.

## 3. Connect an MCU shell

- Baud 9600 - 921600, usually **CRLF** for AT-style shells (change it in the command input's
  line-ending box or in Preferences > Input > Enter sends).
- **DTR** / **RTS** in the connection bar drive reset / boot pins; **Send BREAK** holds the line low.
- Quick commands *MCU > AT*, *AT+GMR*, *reset* and *Control > Ctrl+C / Ctrl+D / ESC* are shipped.

## 4. Everyday features

| Want to... | Do this |
|---|---|
| Open another board in parallel | **Ctrl+T** new tab, **Ctrl+Tab** to switch |
| Keep a boot log | *File > Start Logging* (or turn on auto-log in *Preferences > Logging*) |
| Look at a log captured in the field | *File > Replay Log File...* (or `--replay boot.log --speed 1500000`) - rendered like the live session |
| Paste a script without overrunning the console | *Session > Send File...* (**Ctrl+Shift+O**), text mode, 50 ms per line |
| Send raw bytes | Command input in **HEX** mode (`AA 55 0D`) or a quick command with the HEX flag |
| See exactly what goes over the wire | *View > Hex View* (**Ctrl+Shift+H**) |
| Change baud while connected | Just pick another value - it is applied to the open port |
| Survive a board reboot | Nothing - auto-reconnect is on by default (amber tab dot while waiting) |
| Add your own buttons | *Edit > Quick Commands...* - Add, set name / command / group / line ending, OK |
| Bigger font | **Ctrl++** / **Ctrl+-**, or pick a monospace font in *Preferences > Terminal* |
| Copy text | Select with the mouse, **Ctrl+Shift+C** (plain Ctrl+C goes to the device) |
| Report a bug | *Help > Version* -> **Copy**, paste into the ticket together with the System Log |

## 5. Where things live

| Item | Location |
|---|---|
| Settings | Registry (Windows) / `~/.config/BuildAI/SerialUtility.conf` (Linux) |
| Quick commands, history | `QStandardPaths::AppConfigLocation` -> `quick_commands.json`, `history.txt` |
| Logs | `<Documents>/BuildAI/SerialLogs/<port>_<date>.log` (change in Preferences > Logging) |
| Deployed app | `dist\Release\bin` after `build.ps1 -Deploy` |

## 6. Shortcuts

`Ctrl+T` new tab, `Ctrl+W` close, `Ctrl+Tab`/`Ctrl+Shift+Tab` next/previous tab, `F2`/`F3`
connect/disconnect, `F5` rescan ports, `Ctrl+Shift+L` clear, `Ctrl+Shift+O` send file,
`Ctrl+Shift+H` hex view, `Ctrl+Shift+F` find, `Ctrl+Shift+R` replay log file,
`Ctrl+Shift+C/V` copy/paste, `Ctrl++`/`Ctrl+-`/`Ctrl+0` zoom, `Ctrl+,` preferences,
`Ctrl+Shift+Q` quit.
Everything else (Tab, arrows, bare Ctrl+letters, F-keys) goes to the device.
