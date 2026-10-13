# BuildAI Serial Utility - Quick Start

One page to get from a fresh checkout to a Rockchip boot log. Details: [`../README.md`](../README.md).

## 1. Build (Windows, 2 minutes)

```powershell
git clone <repo-url> SerialUtility
cd SerialUtility
.\scripts\build.ps1 -Config Release -Test      # needs Qt 6.8.3 MSVC2022 at D:\Qt (or set QT_ROOT)
.\scripts\run.ps1   -Config Release
```

Linux: install Qt >= 6.8 first (see README "Plain CMake (Linux)": Debian 13 / Ubuntu 25.10+ apt packages incl. `qt6-l10n-tools`, or aqtinstall 6.8.3 gcc_64 + `-DCMAKE_PREFIX_PATH`), then `cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build && ./build/BuildAI-SerialUtility`

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
| Baud | **Auto** (the tool finds it while the board boots), or **1500000** (RV1106 / RV1106B, recent SDKs) / **115200** (older boards) - 8N1, no flow |
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

## 4. Connect over SSH

**Ctrl+Shift+T** (*File > New SSH Session...*) opens an SSH tab. Type `root@192.168.100.2`
(or `user@host:port`, an `ssh://` URL, or a `Host` alias from your `~/.ssh/config`) and press
Enter. Confirm the host key once (*Connect and remember* writes it to `~/.ssh/known_hosts`), enter
the password or key passphrase, and you are in the same terminal as on the serial port: quick
commands, logging, mark mode, hex view all work. The gear button (*Edit > SSH Profiles...*) saves
named targets with a key file, a remembered password, a startup command (`cd /oem && ls`),
port forwards and more. *Session > Upload File to Remote...* copies a file over SFTP; dropping a
file on the terminal does the same. A reflashed board shows the "identification has changed"
warning - tick the box and *Replace key and connect*.

## 5. Flash the Node board (Firmware tab)

One USB-C cable from the Node board to the PC, nothing else. The first time on a Windows PC, the
Rockchip USB drivers must be installed once (the installer's optional driver task, or the tab's
**Install drivers** button - a GUI installer you click through).

1. **Ctrl+Shift+U** (*File > Firmware Update*) opens the Firmware tab. The SoC line shows what the
   PC sees: *ADB device ...* for a running board.
2. **Source**: pick the build machine (an SSH profile, or type `user@host`), keep the remote folder
   (`~/rk/rockchip-platforms/output/image`) and the cache folder, press **Pull**. The remote image's
   size, time and `git describe` appear first; an unchanged image is not downloaded again. No
   network? **Use local image...** takes an `update.img` from disk. **Roll back** goes back to the
   previous pulled image.
3. **SoC**: press **Flash SoC**. The tab reboots the board into Loader mode over ADB, runs
   `upgrade_tool`, shows the progress, waits for the board to come back and prints its kernel
   version and uptime. A board that does not boot: hold the MASKROM button while plugging it in -
   the tab flashes it from Maskrom the same way. If it sees no board at all, it offers to send
   `reboot loader` through a connected serial / SSH tab (asks first). Do not unplug while the
   progress bar runs; Cancel only works while it is still waiting for the board.
4. **MCU (CC1310)**: pick the `firmware-x.y.z.pkg` (the tab checks it and shows target, version and
   size), press **Read** to see the running version and role, confirm **rx** or **tx** (pre-selected
   from what the MCU reports), then **Update MCU**; the banner of the restarted firmware ends it.
   Flash the SoC first: the MCU is reached through `rfmcu` on the SoC.
5. **Everything at once**: pick the package and press **Pull & Flash**: pull, flash the SoC, wait for
   ADB, update the MCU if its version differs. It stops at the first failure and the log pane says
   which step and why (*Save...* writes it to a file for a ticket).

Afterwards the quick command bar's **Node** group (first in the list) is the console side of the
same board: on the SoC's serial / SSH tab `reboot loader` and `rfmcu console`, inside the console
the CC1310 commands (`help`, `version`, `sys`, `stack`, `uart`, `power`, `bat`, `rx status`,
`rx dump on|off`, `tx status`, `bootloader`) and `~.` to leave it. Details: README "Firmware tab".

## 6. Everyday features

| Want to... | Do this |
|---|---|
| Open another board in parallel | **Ctrl+T** new tab (**Ctrl+Shift+T** for SSH), **Ctrl+Tab** / **Alt+1..9** / the *Window* menu to switch, **Ctrl+Shift+W** to close (Ctrl+W goes to the shell) |
| Keep a boot log | *File > Start Logging* (or turn on auto-log in *Preferences > Logging*) |
| Look at a log captured in the field | *File > Replay Log File...* (or `--replay boot.log --speed 1500000`) - rendered like the live session |
| Paste a script without overrunning the console | *Session > Send File...* (**Ctrl+Shift+O**), text mode, 50 ms per line |
| Send raw bytes | Command input in **HEX** mode (`AA 55 0D`) or a quick command with the HEX flag |
| See exactly what goes over the wire | *View > Hex View* (**Ctrl+Shift+H**) |
| Change baud while connected | Just pick another value - it is applied to the open port |
| Wrong baud rate (garbage on screen) | Pick **Auto** in the baud list, or *Session > Detect Baud Rate* (**Ctrl+Shift+B**) |
| Survive a board reboot | Nothing - auto-reconnect is on by default (amber tab dot while waiting); SSH tabs reconnect the same way after a dropped link |
| Copy a file to the board over SSH | *Session > Upload File to Remote...* (or drop the file on the SSH terminal); *Download File from Remote...* for the other direction |
| Add your own buttons | *Edit > Quick Commands...* - Add, set name / command / group / line ending, OK |
| Bigger font | **Ctrl++** / **Ctrl+-**, or pick a monospace font in *Preferences > Terminal* |
| Copy text | Select with the mouse - the display pauses like the Windows console - then **Enter**, a **right click** or **Ctrl+Shift+C** copies and resumes, **Esc** cancels; plain Ctrl+C goes to the device. Turn the pause off in *View > Pause Output While Selecting* |
| Paste | **Right click** with nothing selected (cmd.exe style; *View > Right Click Pastes*), **Ctrl+Shift+V**, **Shift+Insert** or a middle click; plain Ctrl+V goes to the device. The terminal's context menu is on **Shift+right click** (or the Menu key) |
| Wipe the terminal | The **Clear** toolbar button / **Ctrl+Shift+L** empties the screen, the scrollback and the hex view (colours and modes stay); *Session > Reset Terminal* is the full VT reset |
| Report a bug | *Help > Version* -> **Copy**, paste into the ticket together with the System Log |

## 7. Where things live

| Item | Location |
|---|---|
| Settings | Registry (Windows) / `~/.config/BuildAI/SerialUtility.conf` (Linux) |
| Quick commands, history, SSH profiles | `QStandardPaths::AppConfigLocation` -> `quick_commands.json`, `history.txt`, `ssh_profiles.json` (no secrets; saved passwords go to DPAPI-protected settings) |
| Pulled firmware images | the Firmware tab's *Local cache* folder (default `<data dir>/firmware`: `update.img`, the previous `update.prev.img` and their `.json` notes) |
| Flashing tools | `tools\` next to the executable (`upgrade_tool.exe`, `adb.exe`, `DriverAssistant\`); other copies via *Preferences > Firmware* |
| SSH host keys | `~/.ssh/known_hosts`, shared with the `ssh` command |
| Logs | `<Documents>/BuildAI/SerialLogs/<port>_<date>.log` (change in Preferences > Logging) |
| Deployed app | `dist\Release\bin` after `build.ps1 -Deploy` |

## 8. Shortcuts

`Ctrl+T` new tab, `Ctrl+Shift+T` new SSH tab, `Ctrl+Shift+U` Firmware tab, `Ctrl+Shift+W` close (Ctrl+W goes to the shell), `Ctrl+Tab`/`Ctrl+Shift+Tab` or `Ctrl+PgDn`/`Ctrl+PgUp` next/previous tab, `Alt+1`..`Alt+9` select a tab (also the *Window* menu; a middle click on a tab closes it), `Ctrl+Shift+B` detect the baud rate, `F2`/`F3`
connect/disconnect, `F5` rescan ports, `Ctrl+Shift+L` clear (screen + scrollback + hex view), `Ctrl+Shift+O` send file,
`Ctrl+Shift+H` hex view, `Ctrl+Shift+F` find, `Ctrl+Shift+R` replay log file,
`Ctrl+Shift+C/V` copy/paste, `Ctrl++`/`Ctrl+-`/`Ctrl+0` zoom, `Ctrl+,` preferences,
`Ctrl+Shift+Q` quit.
Every shortcut is configurable in *Edit > Preferences > Keyboard*. While connected, a key that is
an application shortcut goes to the application and everything else (Tab, arrows, every bare
Ctrl+letter, F-keys) goes to the device; F2 / F3 / F5 are application keys only because Connect /
Disconnect / Refresh Ports use them.
