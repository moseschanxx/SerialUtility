# third_party: bundled flashing tools

Binaries that BuildAI Serial Utility (Firmware tab, v0.5) starts as separate processes. They are
**not** linked into the application and are not built from source here. They are committed as plain
files (about 20 MB in total) so that a checkout builds a complete package without network access.

CMake assembles them into `tools/` next to the executable after every build of `SerialUtility` and
`cmake --install` ships the same folder as `bin/tools/` (see "Packaging" in `CMakeLists.txt`). The
application looks for `tools/upgrade_tool.exe`, `tools/adb.exe` and
`tools/DriverAssistant/DriverInstall.exe` (Linux: `tools/upgrade_tool`) there; Preferences >
Firmware can point to other copies.

## Where everything came from

All files were copied on 2026-10-13 from the internal repository `rk/rockchip-platforms`
(`moses@10.0.10.71:rk/rockchip-platforms`), branch `rv1106-bg2-main`, commit `e0e0d41`
(`v1.3.0-4-ge0e0d41`), directory `tools/`. In that repository the Windows binaries are Git LFS
objects (`/tools/windows/**`) and `DriverAssitant_v5.14.zip` is untracked; here every file is a plain
file, byte for byte what the repository's working tree holds. Nothing was modified.

| Path here | Source (relative to `tools/` of that repository) | Size (bytes) | Licence |
|---|---|---|---|
| `rockchip/windows/upgrade_tool.exe` | `windows/SocToolKit/bin/windows/upgrade_tool.exe`, Rockchip upgrade_tool v2.37, native Win32 console tool | 1,440,768 | the SocToolKit's `license.txt` (below) |
| `rockchip/windows/license.txt` | `windows/SocToolKit/license.txt` | 7,652 | text of the GNU Lesser General Public License v3, as shipped by Rockchip with the tool |
| `rockchip/windows/revison.log` | `windows/SocToolKit/revison.log` (the file name is Rockchip's spelling), SocToolKit change log v1.99 and older | 2,927 | - |
| `rockchip/linux/upgrade_tool` | `linux/Linux_Upgrade_Tool/upgrade_tool`, Rockchip upgrade_tool v2.26 for Linux x86-64 (ELF) | 3,379,152 | no licence file ships with the Linux tool; same Rockchip origin as the Windows one |
| `rockchip/linux/config.ini` | `linux/Linux_Upgrade_Tool/config.ini`, optional settings, all commented out | 128 | - |
| `rockchip/linux/88-rockusb.rules` | `linux/Linux_Upgrade_Tool/88-rockusb.rules`, udev rule giving users access to USB vendor 2207 | 55 | - |
| `rockchip/linux/readme.txt` | `linux/Linux_Upgrade_Tool/readme.txt` (Rockchip's note on installing the rule, partly Chinese, UTF-8) | 117 | - |
| `rockchip/linux/revision.txt` | `linux/Linux_Upgrade_Tool/revision.txt`, upgrade_tool change log | 2,636 | - |
| `adb/adb.exe` | `windows/FactoryTool_v1.91.zip`, `FactoryTool_v1.91/bin/adb.exe`; adb 1.0.41, "Version 31.0.2-eng.lsh.20220809.085517 RockChip modified" | 5,938,176 | Apache License 2.0, see `adb/NOTICE.md` |
| `adb/AdbWinApi.dll` | same archive, `FactoryTool_v1.91/bin/AdbWinApi.dll` | 97,792 | Apache License 2.0 |
| `adb/AdbWinUsbApi.dll` | same archive, `FactoryTool_v1.91/bin/AdbWinUsbApi.dll` | 62,976 | Apache License 2.0 |
| `DriverAssistant/DriverAssitant_v5.14.zip` | `windows/DriverAssitant_v5.14.zip` (Rockchip "Driver Assistant" 5.14: rockusb and ADB WinUSB drivers) | 9,786,934 | Rockchip's installer; the archive contains no licence file |

Why these particular files:

- `upgrade_tool` flashes `update.img` (`LD` lists devices, `SFI` shows the image, `UF` upgrades). The
  Windows build runs without a `config.ini` next to it (checked on the development PC with `LD`
  and `SFI`); the Linux one reads the optional `config.ini` from its own folder, so it is shipped.
- `adb.exe` and its two DLLs are the FactoryTool v1.91 copy. Rockchip's build supports
  `adb forward tcp:0` and shell_v2, which the MCU path (`rfmcu bridge` over a forwarded port) and
  the SoC path need. Only these three files were taken from the archive (the FactoryTool GUI itself
  is not part of the package).
- `DriverAssitant_v5.14.zip` (the folder inside is `DriverAssitant_v5.14/` with `DriverInstall.exe`,
  `ADBDriver/`, `Driver/`, `bin/`, `config.ini`; "Assitant" is Rockchip's spelling) is kept as a zip in
  the source tree and extracted at configure time (`file(ARCHIVE_EXTRACT)`), so the repository
  holds one file instead of sixty. The installer task "Install the Rockchip USB drivers" and the
  Firmware tab's driver hint start `tools/DriverAssistant/DriverInstall.exe`, a GUI installer the
  user clicks through (installing a driver needs administrator rights; Windows asks).

## Licence notes for whoever ships a release

- The only licence text Rockchip ships next to its tool is the LGPL v3 in `rockchip/windows/license.txt`;
  it is installed next to `upgrade_tool.exe` (`tools/license.txt`) so the text travels with the
  binary. The tool itself is distributed by Rockchip as a binary; this repository does not contain
  its source and BuildAI has not modified it.
- adb and its DLLs are Apache 2.0 (Android Open Source Project) and unmodified from FactoryTool.
- DriverAssistant is Rockchip's proprietary installer; it is redistributed as Rockchip supplies it.

Updating a tool means replacing the file here, changing the size and version in this table and
the provenance line, and checking `cmake --install` plus the CI checks in `.github/workflows/build.yml`.
