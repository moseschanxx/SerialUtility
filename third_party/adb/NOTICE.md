# adb (Android Debug Bridge) - bundled binary

| | |
|---|---|
| Files | `adb.exe`, `AdbWinApi.dll`, `AdbWinUsbApi.dll` |
| What | Android platform-tools `adb`, version 1.0.41 ("Version 31.0.2-eng.lsh.20220809.085517 RockChip modified") |
| Origin | Rockchip FactoryTool v1.91 (`FactoryTool_v1.91/bin/`, taken from `tools/windows/FactoryTool_v1.91.zip` of the `rockchip-platforms` repository, commit e0e0d41) |
| Licence | Apache License, Version 2.0 (Android Open Source Project, <https://www.apache.org/licenses/LICENSE-2.0>) |

BuildAI Serial Utility starts this `adb.exe` as a separate process (it is never linked into the
application) to reach the RV1106 SoC of the Node board over USB: `adb shell`, `adb pull`, and
`adb forward tcp:0 ...` for the SSH path through USB. The binary is Rockchip's build of the AOSP
sources; it is redistributed unmodified, exactly as shipped in FactoryTool v1.91. It supports
`forward tcp:0` and shell_v2, which the Firmware tab relies on.

Copyright (C) The Android Open Source Project. Licensed under the Apache License, Version 2.0; you may
not use these files except in compliance with the License. Unless required by applicable law or agreed
to in writing, software distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the License for the specific
language governing permissions and limitations under the License.
