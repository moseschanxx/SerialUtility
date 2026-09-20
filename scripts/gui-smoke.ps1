<#
.SYNOPSIS
    End-to-end smoke driver: runs the real (deployed) BuildAI-SerialUtility.exe on the desktop,
    drives it with keystrokes (SendKeys) and captures screenshots for visual inspection.

.DESCRIPTION
    Complements the offscreen Qt Test suites: this exercises the shipped executable with a real
    window, real focus handling and the built-in simulated devices. Scenarios:
      linux    - SIM:linux boot, login, uname/color/chinese/ls/top/progress/dmesg, reboot + auto-reconnect
      uboot    - SIM:uboot autoboot interrupt, help/printenv/bdinfo, boot into Linux
      mcu      - SIM:mcu help, AT / AT+GMR, adc, telemetry stream
      menus    - every menu, Preferences / About / Version / Quick Commands dialogs, zh_CN <-> en_US, tabs
      hardware - connect to a real port with a TX-RX loopback jumper and check the echo
      stress   - 7 MB replay at unlimited speed, 21 tabs, 2000-line paste over SIM:loopback, 10 connect cycles
    Screenshots land in <OutDir>\<scenario>-NN-<label>.png. Keep the desktop free while it runs:
    keystrokes go to the foreground window (the script refocuses the app before every key group).

.EXAMPLE
    .\scripts\gui-smoke.ps1 -Scenario linux
.EXAMPLE
    .\scripts\gui-smoke.ps1 -Scenario hardware -HardwarePort COM6
#>
param(
    [Parameter(Mandatory = $true)][ValidateSet('linux', 'uboot', 'mcu', 'menus', 'hardware', 'stress')][string]$Scenario,
    [string]$Exe = (Join-Path (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)) 'dist\Release\bin\BuildAI-SerialUtility.exe'),
    [string]$OutDir = (Join-Path (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)) 'build\gui-run'),
    [string]$HardwarePort = 'COM6'
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms, System.Drawing, Microsoft.VisualBasic
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class Win32 {
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern int GetWindowTextLength(IntPtr hWnd);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr hWnd, System.Text.StringBuilder s, int n);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
}
"@
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$script:step = 0
function Log($m) { Write-Host ("[{0}] {1}" -f (Get-Date -Format 'HH:mm:ss.fff'), $m) }
function Shot($label, [switch]$Screen) {
    $script:step++
    $file = Join-Path $OutDir ("{0}-{1:D2}-{2}.png" -f $Scenario, $script:step, $label)
    if ($Screen) {
        $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
        $bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
        $g = [System.Drawing.Graphics]::FromImage($bmp); $g.CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size); $g.Dispose()
    } else {
        $h = (Get-Process -Id $script:appPid).MainWindowHandle
        $r = New-Object Win32+RECT; [Win32]::GetWindowRect($h, [ref]$r) | Out-Null
        $bmp = New-Object System.Drawing.Bitmap ($r.Right - $r.Left), ($r.Bottom - $r.Top)
        $g = [System.Drawing.Graphics]::FromImage($bmp); $hdc = $g.GetHdc()
        [Win32]::PrintWindow($h, $hdc, 2) | Out-Null; $g.ReleaseHdc($hdc); $g.Dispose()
    }
    $bmp.Save($file, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    Log "shot $file"
}
function ForegroundPid() { $h = [Win32]::GetForegroundWindow(); [uint32]$fp = 0; [Win32]::GetWindowThreadProcessId($h, [ref]$fp) | Out-Null; return $fp }
function Keys($k, $waitMs = 400) {
    if ((ForegroundPid) -ne $script:appPid) { Log "foreground is not the app ('$(ForegroundTitle)') - refocusing"; Focus }
    [System.Windows.Forms.SendKeys]::SendWait($k); Start-Sleep -Milliseconds $waitMs
}
function Focus() {
    $p = Get-Process -Id $script:appPid
    [Win32]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
    try { [Microsoft.VisualBasic.Interaction]::AppActivate($script:appPid) } catch {}
    Start-Sleep -Milliseconds 300
}
function ForegroundTitle() {
    $h = [Win32]::GetForegroundWindow(); $sb = New-Object System.Text.StringBuilder 512
    [Win32]::GetWindowText($h, $sb, 512) | Out-Null; return $sb.ToString()
}
function StartApp([string[]]$appArgs) {
    if ($appArgs -and $appArgs.Count -gt 0) {
        $p = Start-Process -FilePath $Exe -ArgumentList $appArgs -WorkingDirectory (Split-Path $Exe) -PassThru
    } else {
        $p = Start-Process -FilePath $Exe -WorkingDirectory (Split-Path $Exe) -PassThru
    }
    $script:appPid = $p.Id
    $deadline = (Get-Date).AddSeconds(15)
    while ((Get-Date) -lt $deadline) { $p.Refresh(); if ($p.MainWindowHandle -ne 0) { break }; Start-Sleep -Milliseconds 200 }
    if ($p.MainWindowHandle -eq 0) { throw "main window did not appear" }
    Log "started pid $($p.Id) title '$($p.MainWindowTitle)'"
    Focus
}
function Alive() { try { $p = Get-Process -Id $script:appPid -ErrorAction Stop; return -not $p.HasExited } catch { return $false } }
function CloseApp() {
    if (-not (Alive)) { Log "process already exited"; return }
    Focus; Keys '%{F4}' 800
    if (Alive) { Shot 'close-confirm' -Screen; Log "foreground: '$(ForegroundTitle)'"; Keys '{ENTER}' 800 }
    $deadline = (Get-Date).AddSeconds(5); while ((Get-Date) -lt $deadline -and (Alive)) { Start-Sleep -Milliseconds 200 }
    if (Alive) { Log "still alive after close - killing"; Stop-Process -Id $script:appPid -Force } else { Log "exited cleanly" }
}

switch ($Scenario) {
  'linux' {
    StartApp @('--connect', 'SIM:linux')
    Start-Sleep -Seconds 7; Shot 'boot-login'
    Keys 'root{ENTER}' 600; Keys '{ENTER}' 900; Shot 'shell-prompt'
    Keys 'uname -a{ENTER}' 800; Shot 'uname'
    Keys 'color{ENTER}' 1200; Shot 'color'
    Keys 'chinese{ENTER}' 900; Shot 'chinese'
    Keys 'ls -l /{ENTER}' 900; Shot 'ls'
    Keys 'top -n 1{ENTER}' 1500; Shot 'top'
    Keys 'progress{ENTER}' 1200; Shot 'progress-mid'; Start-Sleep -Seconds 3; Shot 'progress-done'
    Keys 'dmesg{ENTER}' 1500; Shot 'dmesg'
    Keys 'nonexistentcmd{ENTER}' 600; Shot 'not-found'
    Keys 'reboot{ENTER}' 1200; Shot 'reboot-disappeared'; Start-Sleep -Seconds 7; Shot 'reboot-reconnected'
    CloseApp
  }
  'uboot' {
    StartApp @('--connect', 'SIM:uboot')
    Keys ' ' 300; Shot 'interrupted'
    Keys 'help{ENTER}' 900; Shot 'help'
    Keys 'printenv{ENTER}' 900; Shot 'printenv'
    Keys 'bdinfo{ENTER}' 900; Shot 'bdinfo'
    Keys 'boot{ENTER}' 500; Start-Sleep -Seconds 8; Shot 'booted-login'
    CloseApp
  }
  'mcu' {
    StartApp @('--connect', 'SIM:mcu')
    Start-Sleep -Seconds 2; Shot 'banner'
    Keys 'help{ENTER}' 800; Shot 'help'
    Keys 'AT{ENTER}' 600; Keys 'AT{+}GMR{ENTER}' 600; Keys 'adc{ENTER}' 600; Shot 'at-adc'
    Keys 'telemetry on{ENTER}' 500; Start-Sleep -Seconds 5; Shot 'telemetry'
    Keys 'telemetry off{ENTER}' 500
    CloseApp
  }
  'menus' {
    StartApp @()
    Start-Sleep -Seconds 2; Shot 'main'
    foreach ($m in @('f','s','e','v','l','h')) { Keys "%$m" 700; Shot "menu-$m" -Screen; Keys '{ESC}' 300 }
    Keys '^,' 1200; Shot 'preferences' -Screen; Log "foreground: '$(ForegroundTitle)'"; Keys '{ESC}' 500
    Keys '%h' 500; Keys '{UP}{ENTER}' 1200; Shot 'about' -Screen; Log "foreground: '$(ForegroundTitle)'"; Keys '{ESC}' 500
    Keys '%h' 500; Keys '{ENTER}' 1200; Shot 'version' -Screen; Log "foreground: '$(ForegroundTitle)'"; Keys '{ESC}' 500
    Keys '%e' 500; Keys '{DOWN}{DOWN}{DOWN}{DOWN}{ENTER}' 1200; Shot 'quick-commands-dialog' -Screen; Log "foreground: '$(ForegroundTitle)'"; Keys '{ESC}' 500
    Keys '%s' 500; Shot 'menu-session-open' -Screen; Keys '{ESC}' 300
    Keys '%e' 500; Shot 'menu-edit-again' -Screen; Keys '{ESC}' 300
    Keys '%l' 500; Keys '{DOWN}{ENTER}' 1200; Shot 'chinese-ui'
    Keys '%l' 500; Keys '{ENTER}' 1200; Shot 'english-ui'
    Keys '^t' 800; Shot 'new-tab'
    Keys '^w' 800; Shot 'closed-tab'
    CloseApp
  }
  'hardware' {
    StartApp @('--connect', $HardwarePort)
    Start-Sleep -Seconds 2; Shot 'connected'
    Keys 'hello loopback 0123456789{ENTER}' 1000; Shot 'echo'
    Keys 'BuildAI Serial Utility{ENTER}' 1000; Shot 'echo2'
    CloseApp
  }
  'stress' {
    # 1) unlimited-speed replay of a 60k-line coloured log
    $big = Join-Path $OutDir 'big-boot.log'
    if (-not (Test-Path $big)) {
      $sb = New-Object System.Text.StringBuilder
      $esc = [char]27
      for ($i = 0; $i -lt 60000; $i++) {
        $t = ($i * 0.001234).ToString('0.000000').PadLeft(12)
        [void]$sb.Append("[$t] ${esc}[1;32mrockchip-driver${esc}[0m subsystem ${i}: ${esc}[33mstatus${esc}[0m ok, ${esc}[36mlatency${esc}[0m=$($i % 97) us, buffer 0x$(($i * 4096).ToString('x8'))`r`n")
      }
      [System.IO.File]::WriteAllText($big, $sb.ToString(), [System.Text.Encoding]::UTF8)
    }
    Log ("big log: {0:N0} bytes" -f (Get-Item $big).Length)
    StartApp @('--replay', $big, '--speed', '0')
    $sw = [Diagnostics.Stopwatch]::StartNew()
    Start-Sleep -Seconds 3; Shot 'replay-running'
    $p = Get-Process -Id $script:appPid; Log ("after 3 s: working set {0:N0} MB, responding={1}" -f ($p.WorkingSet64/1MB), $p.Responding)
    Start-Sleep -Seconds 9; Shot 'replay-later'
    $p.Refresh(); Log ("after 12 s: working set {0:N0} MB, responding={1}" -f ($p.WorkingSet64/1MB), $p.Responding)
    Keys '{ESC}' 300   # a key while replaying: must be swallowed, never crash
    # 2) 20 tabs open / close
    for ($i = 0; $i -lt 20; $i++) { Keys '^t' 120 }
    Shot 'twenty-tabs'
    $p.Refresh(); Log ("21 tabs: working set {0:N0} MB, responding={1}" -f ($p.WorkingSet64/1MB), $p.Responding)
    for ($i = 0; $i -lt 20; $i++) { Keys '^w' 120 }
    Shot 'tabs-closed'
    CloseApp
    # 3) large paste through the loopback + rapid connect/disconnect
    StartApp @('--connect', 'SIM:loopback')
    Start-Sleep -Seconds 1
    $text = (1..2000 | ForEach-Object { "paste line $_ : the quick brown fox jumps over the lazy dog 0123456789" }) -join "`r`n"
    [System.Windows.Forms.Clipboard]::SetText($text)
    Keys '^+v' 200
    Start-Sleep -Seconds 6; Shot 'big-paste'
    $p = Get-Process -Id $script:appPid; Log ("after paste: working set {0:N0} MB, responding={1}" -f ($p.WorkingSet64/1MB), $p.Responding)
    for ($i = 0; $i -lt 10; $i++) { Keys '{F3}' 150; Keys '{F2}' 150 }
    Start-Sleep -Milliseconds 800; Shot 'reconnect-cycles'
    $p.Refresh(); Log ("after 10 connect/disconnect cycles: responding={0}, alive={1}" -f $p.Responding, (Alive))
    Keys 'still alive{ENTER}' 800; Shot 'after-cycles-echo'
    CloseApp
  }
  default { throw "unknown scenario $Scenario" }
}
Log "done"
