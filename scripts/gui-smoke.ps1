<#
.SYNOPSIS
    End-to-end smoke driver: runs the real (deployed) BuildAI-SerialUtility.exe on the desktop,
    drives it with keystrokes (SendKeys) and captures screenshots for visual inspection.

.DESCRIPTION
    Complements the offscreen Qt Test suites: this exercises the shipped executable with a real
    window, real focus handling and the built-in simulated devices. Scenarios:
      linux    - SIM:linux boot (--baud auto: the board talks at 1500000, the app detects it), login,
                 uname/color/chinese/ls/top/progress/dmesg, reboot + auto-reconnect
      uboot    - SIM:uboot at its native 1500000 (--baud 1500000: the autoboot countdown must be interrupted
                 before a detection could finish), help/printenv/bdinfo, boot into Linux
      mcu      - SIM:mcu help, AT / AT+GMR, adc, telemetry stream
      menus    - every menu, Preferences / About / Version / Quick Commands dialogs, zh_CN <-> en_US, tabs
      hardware - connect to a real port with a TX-RX loopback jumper and check the echo
      stress   - 7 MB replay at unlimited speed, 21 tabs, 2000-line paste over SIM:loopback, 10 connect cycles
      markmode - mouse-select while SIM:mcu streams telemetry: display pauses, Enter copies + resumes, Esc cancels,
                 right-click copies a selection / pastes the clipboard (cmd.exe QuickEdit)
      ssh      - New SSH Session (Ctrl+Shift+T) to a real OpenSSH server (-SshTarget user@host[:port], password in
                 -SshPasswordFile): host-key dialog, password dialog, shell commands, window resize -> stty size,
                 hex view, Clear, disconnect + reconnect (host already known), "exit" closes the channel.
                 The app is pointed at a temporary known_hosts file for the run (registry value ssh\knownHostsFile,
                 restored afterwards) so the user's ~/.ssh/known_hosts is never touched.
      sshfiles - file transfer against the same real OpenSSH server: connect as in 'ssh', open Session > Upload File
                 to Remote through the menu bar (Alt+S, U), paste a local 3 MB file whose name has a space and a
                 non-ASCII character and the remote directory "/tmp/", Start (Alt+S), screenshot the finished state,
                 verify the file in WSL (-WslDistro, sha256sum), upload a 60 MB file and Cancel it half-way (Alt+C)
                 with a progress screenshot, download the 3 MB file back (Alt+D, Alt+R, Alt+T) and compare hashes,
                 try a remote file that does not exist (red error line), close the dialog (Alt+E) and exit.
    Screenshots land in <OutDir>\<scenario>-NN-<label>.png. Keep the desktop free while it runs:
    keystrokes go to the foreground window (the script refocuses the app before every key group).

.EXAMPLE
    .\scripts\gui-smoke.ps1 -Scenario linux
.EXAMPLE
    .\scripts\gui-smoke.ps1 -Scenario hardware -HardwarePort COM6
.EXAMPLE
    .\scripts\gui-smoke.ps1 -Scenario ssh -SshTarget sshprobe@localhost -SshPasswordFile build\wf-core\probe-credentials.txt
.EXAMPLE
    .\scripts\gui-smoke.ps1 -Scenario sshfiles -SshPasswordFile build\wf-core\probe-credentials.txt -WslDistro Ubuntu-22.04
#>
param(
    [Parameter(Mandatory = $true)][ValidateSet('linux', 'uboot', 'mcu', 'menus', 'hardware', 'stress', 'markmode', 'ssh', 'sshfiles')][string]$Scenario,
    [string]$Exe = '',      # default: <repo>\dist\Release\bin\BuildAI-SerialUtility.exe
    [string]$OutDir = '',   # default: <repo>\build\gui-run
    [string]$HardwarePort = 'COM6',
    [string]$SshTarget = 'sshprobe@localhost',   # ssh / sshfiles scenarios: user@host[:port] of a reachable OpenSSH server
    [string]$SshPasswordFile = '',               # ssh / sshfiles scenarios: text file whose first line is that user's password
    [string]$WslDistro = 'Ubuntu-22.04'          # sshfiles scenario: the WSL distribution running that server ('' = skip the WSL check)
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot   # $PSScriptRoot is not usable inside param() defaults on PowerShell 5.1
if (-not $Exe) { $Exe = Join-Path $repoRoot 'dist\Release\bin\BuildAI-SerialUtility.exe' }
if (-not $OutDir) { $OutDir = Join-Path $repoRoot 'build\gui-run' }
elseif (-not [System.IO.Path]::IsPathRooted($OutDir)) { $OutDir = Join-Path (Get-Location).Path $OutDir }   # the ssh scenario hands paths to the app, which runs in another directory
if (-not (Test-Path $Exe)) { throw "Executable not found: $Exe (run scripts\build.ps1 -Config Release -Deploy first)" }
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
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string title);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint flags, int dx, int dy, uint data, IntPtr extra);
  public const uint LEFTDOWN = 0x0002, LEFTUP = 0x0004, RIGHTDOWN = 0x0008, RIGHTUP = 0x0010;
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
}
"@
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$script:step = 0
$script:failures = 0   # Check() failures (sshfiles); a non-zero count makes the script exit with 1
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
function Drag($x0, $y0, $x1, $y1) {   # window-relative pixels
    $h = (Get-Process -Id $script:appPid).MainWindowHandle
    $r = New-Object Win32+RECT; [Win32]::GetWindowRect($h, [ref]$r) | Out-Null
    [Win32]::SetCursorPos($r.Left + $x0, $r.Top + $y0) | Out-Null; Start-Sleep -Milliseconds 120
    [Win32]::mouse_event([Win32]::LEFTDOWN, 0, 0, 0, [IntPtr]::Zero); Start-Sleep -Milliseconds 120
    $steps = 12
    for ($i = 1; $i -le $steps; $i++) {
        [Win32]::SetCursorPos($r.Left + $x0 + [int](($x1 - $x0) * $i / $steps), $r.Top + $y0 + [int](($y1 - $y0) * $i / $steps)) | Out-Null
        Start-Sleep -Milliseconds 25
    }
    [Win32]::mouse_event([Win32]::LEFTUP, 0, 0, 0, [IntPtr]::Zero); Start-Sleep -Milliseconds 200
    Log "dragged ($x0,$y0) -> ($x1,$y1) window-relative"
}
function RightClick($x, $y) {   # window-relative pixels
    $h = (Get-Process -Id $script:appPid).MainWindowHandle
    $r = New-Object Win32+RECT; [Win32]::GetWindowRect($h, [ref]$r) | Out-Null
    [Win32]::SetCursorPos($r.Left + $x, $r.Top + $y) | Out-Null; Start-Sleep -Milliseconds 120
    [Win32]::mouse_event([Win32]::RIGHTDOWN, 0, 0, 0, [IntPtr]::Zero); Start-Sleep -Milliseconds 80
    [Win32]::mouse_event([Win32]::RIGHTUP, 0, 0, 0, [IntPtr]::Zero); Start-Sleep -Milliseconds 300
    Log "right-clicked ($x,$y) window-relative"
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
    # v0.4: SIM:linux talks at 1500000; "--baud auto" selects the bar's Auto rate, so the app opens at 115200,
    # hears garbage, tries the candidates and replays the boot log once 1500000 is found (a few seconds).
    StartApp @('--connect', 'SIM:linux', '--baud', 'auto')
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
    # The countdown is interrupted 300 ms after start: the app must listen at the board's rate from the first
    # byte (a detection needs the 1.5 s sample at 115200 first, during which typed keys are dropped).
    StartApp @('--connect', 'SIM:uboot', '--baud', '1500000')
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
  'markmode' {
    # cmd.exe-style mark mode: select with the mouse while the device streams; Enter copies + resumes.
    StartApp @('--connect', 'SIM:mcu')
    Start-Sleep -Seconds 2
    Keys 'telemetry on{ENTER}' 500
    Start-Sleep -Seconds 3; Shot 'streaming'
    [System.Windows.Forms.Clipboard]::SetText('sentinel-before')
    Drag 20 160 420 160
    Start-Sleep -Seconds 5; Shot 'paused-with-selection'       # badge visible, no new telemetry lines
    Keys '{ENTER}' 800; Shot 'after-enter-resumed'             # copied + resumed: queued lines appear
    $clip = [System.Windows.Forms.Clipboard]::GetText(); Log "clipboard after Enter: '$clip'"
    if ($clip -eq 'sentinel-before' -or [string]::IsNullOrWhiteSpace($clip)) { Log 'FAIL: Enter did not copy the selection' } else { Log 'OK: Enter copied the selection' }
    Start-Sleep -Seconds 2
    [System.Windows.Forms.Clipboard]::SetText('sentinel-esc')
    Drag 20 200 300 230
    Start-Sleep -Seconds 4; Shot 'paused-again'
    Keys '{ESC}' 800; Shot 'after-esc-resumed'
    $clip2 = [System.Windows.Forms.Clipboard]::GetText(); Log "clipboard after Esc: '$clip2' (expected sentinel-esc)"
    if ($clip2 -eq 'sentinel-esc') { Log 'OK: Esc resumed without copying' } else { Log 'FAIL: Esc changed the clipboard' }
    # right-click with a selection = copy (and resume); right-click without one = paste (the MCU echoes it)
    [System.Windows.Forms.Clipboard]::SetText('sentinel-rclick')
    Drag 20 160 200 160
    Start-Sleep -Seconds 3; Shot 'paused-before-rightclick'
    RightClick 600 400; Shot 'after-rightclick-copy'
    $clip3 = [System.Windows.Forms.Clipboard]::GetText(); Log "clipboard after right-click on selection: '$clip3'"
    if ($clip3 -ne 'sentinel-rclick' -and -not [string]::IsNullOrWhiteSpace($clip3)) { Log 'OK: right-click copied the selection' } else { Log 'FAIL: right-click did not copy' }
    Keys 'telemetry off{ENTER}' 500; Start-Sleep -Seconds 1
    [System.Windows.Forms.Clipboard]::SetText('echo PASTED-BY-RIGHT-CLICK')
    RightClick 600 400; Start-Sleep -Seconds 1; Shot 'after-rightclick-paste'   # expect "> echo PASTED-BY-RIGHT-CLICK" echoed by the MCU
    Keys '{ENTER}' 800; Shot 'pasted-command-executed'
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
  'ssh' {
    # SendKeys treats + ^ % ~ ( ) { } [ ] as control characters: wrap them in braces.
    function EscapeKeys([string]$s) { return ($s -replace '([+^%~(){}\[\]])', '{$1}') }
    if (-not $SshPasswordFile -or -not (Test-Path $SshPasswordFile)) { throw "ssh scenario needs -SshPasswordFile <file with the password on its first line>" }
    $password = (Get-Content $SshPasswordFile -TotalCount 1).Trim()
    if (-not $password) { throw "empty password in $SshPasswordFile" }
    # Point the app at a temporary known_hosts so the run is deterministic (the host-key dialog
    # appears on the first connect) and the user's real ~/.ssh/known_hosts stays untouched.
    $regKey = 'HKCU:\Software\BuildAI\SerialUtility\ssh'
    if (-not (Test-Path $regKey)) { New-Item -Path $regKey -Force | Out-Null }
    $oldKnownHosts = (Get-ItemProperty -Path $regKey -Name knownHostsFile -ErrorAction SilentlyContinue).knownHostsFile
    $tmpKnownHosts = Join-Path $OutDir 'smoke_known_hosts'
    Remove-Item $tmpKnownHosts -ErrorAction SilentlyContinue
    Set-ItemProperty -Path $regKey -Name knownHostsFile -Value $tmpKnownHosts
    try {
      StartApp @()
      Keys '^+t' 800; Shot 'new-ssh-tab'                                   # focus is in the target field
      Keys ((EscapeKeys $SshTarget) + '{ENTER}') 2500
      Shot 'hostkey-dialog' -Screen; Log "foreground: '$(ForegroundTitle)'"   # "Host Key Verification", default = Connect and remember
      Keys '{ENTER}' 1500
      Shot 'password-dialog' -Screen; Log "foreground: '$(ForegroundTitle)'"
      Keys ((EscapeKeys $password) + '{ENTER}') 3000
      Shot 'shell'                                                          # remote prompt, status bar "user@host · ssh-ed25519 · password"
      if (-not (Test-Path $tmpKnownHosts)) { Log 'FAIL: known_hosts was not written' } else { Log ("OK: known_hosts written: " + (Get-Content $tmpKnownHosts | Select-Object -First 1)) }
      Keys 'echo SMOKE-OK-$((40+2)){ENTER}' 800; Shot 'echo'                # expect SMOKE-OK-42
      Keys 'uname -a; id{ENTER}' 800; Shot 'uname'
      Keys 'stty size{ENTER}' 800; Shot 'stty-before'
      Keys '% x' 1500                                                       # maximise (system menu: Alt+Space, x) -> window-change request
      Keys 'stty size{ENTER}' 800; Shot 'stty-after-maximise'
      Keys 'ls --color=always /{ENTER}' 800; Shot 'ls-color'
      Keys 'seq 1 300{ENTER}' 1200; Shot 'scrolled'
      Keys '^+h' 800; Shot 'hex-view'; Keys '^+h' 500
      Keys '^+l' 500; Shot 'cleared'
      Keys '{F3}' 1500; Shot 'disconnected'                                 # Disconnect (F3)
      Keys '{F2}' 2500; Shot 'reconnect-password-dialog' -Screen           # host already known: only the password is asked
      Keys ((EscapeKeys $password) + '{ENTER}') 3000; Shot 'reconnected'
      Keys 'exit{ENTER}' 2000; Shot 'after-exit'                            # clean exit: "connection closed", no reconnect
      Keys '^t' 500; Shot 'serial-tab-after-ssh'                            # a serial tab still works next to it
      CloseApp
    } finally {
      if ($null -ne $oldKnownHosts) { Set-ItemProperty -Path $regKey -Name knownHostsFile -Value $oldKnownHosts } else { Remove-ItemProperty -Path $regKey -Name knownHostsFile -ErrorAction SilentlyContinue }
    }
  }
  'sshfiles' {
    # Upload / download through Session > Upload File to Remote... against the real OpenSSH server.
    # Paths go into the fields through the clipboard (Ctrl+A, Ctrl+V): SendKeys cannot type the
    # non-ASCII file name and a pasted path is exactly what a user does with a long one.
    function EscapeKeys([string]$s) { return ($s -replace '([+^%~(){}\[\]])', '{$1}') }
    # Keys meant for the modeless transfer dialog: another application's popup (a chat notification)
    # can take the foreground at any moment, and Focus() would bring the main window up instead.
    $dialogTitle = 'Remote File Transfer'
    function FocusDialog() {
        $h = [Win32]::FindWindow($null, $dialogTitle)
        if ($h -eq [IntPtr]::Zero) { return $false }
        [Win32]::SetForegroundWindow($h) | Out-Null
        try { [Microsoft.VisualBasic.Interaction]::AppActivate($dialogTitle) } catch {}
        Start-Sleep -Milliseconds 250
        return ((ForegroundTitle) -eq $dialogTitle)
    }
    function DialogKeys($k, $waitMs = 400) {
        if ((ForegroundTitle) -ne $dialogTitle) {
            Log "foreground is '$(ForegroundTitle)' - refocusing the transfer dialog"
            if (-not (FocusDialog)) { Focus; FocusDialog | Out-Null }
        }
        [System.Windows.Forms.SendKeys]::SendWait($k); Start-Sleep -Milliseconds $waitMs
    }
    function Paste([string]$text, $waitMs = 500) { [System.Windows.Forms.Clipboard]::SetText($text); DialogKeys '^a' 150; DialogKeys '^v' $waitMs }
    function RandomFile([string]$path, [int]$bytes) {
        $buffer = New-Object byte[] $bytes; (New-Object System.Random).NextBytes($buffer)
        [System.IO.File]::WriteAllBytes($path, $buffer)
        return (Get-FileHash $path -Algorithm SHA256).Hash.ToLower()
    }
    function WslHashes() {   # "<hash>  <path>" lines for the smoke files in /tmp (world-readable there)
        if (-not $WslDistro) { return @() }
        try { return @(& wsl.exe -d $WslDistro -- sh -c 'sha256sum /tmp/smoke-*.bin /tmp/smoke\ *.bin 2>/dev/null' | ForEach-Object { "$_" }) } catch { Log "WSL check failed: $_"; return @() }
    }
    # Every failed check is counted; the script exits with 1 after the scenario (see the end).
    function Check([bool]$ok, [string]$what) { if ($ok) { Log "OK: $what" } else { $script:failures++; Log "FAIL: $what" } }
    if (-not $SshPasswordFile -or -not (Test-Path $SshPasswordFile)) { throw "sshfiles scenario needs -SshPasswordFile <file with the password on its first line>" }
    $password = (Get-Content $SshPasswordFile -TotalCount 1).Trim()
    if (-not $password) { throw "empty password in $SshPasswordFile" }
    $regKey = 'HKCU:\Software\BuildAI\SerialUtility\ssh'
    if (-not (Test-Path $regKey)) { New-Item -Path $regKey -Force | Out-Null }
    $oldKnownHosts = (Get-ItemProperty -Path $regKey -Name knownHostsFile -ErrorAction SilentlyContinue).knownHostsFile
    $tmpKnownHosts = Join-Path $OutDir 'smoke_known_hosts'
    Remove-Item $tmpKnownHosts -ErrorAction SilentlyContinue
    Set-ItemProperty -Path $regKey -Name knownHostsFile -Value $tmpKnownHosts
    # The dialog remembers its last paths in the same registry hive: keep the user's own.
    $pathsKey = 'HKCU:\Software\BuildAI\SerialUtility\remoteFile'
    $oldPaths = Get-ItemProperty -Path $pathsKey -ErrorAction SilentlyContinue
    # The local files: a 3 MB one with a space and a non-ASCII character in its name, a 60 MB one to cancel.
    $smallName = "smoke upload " + [char]0x00FC + ".bin"
    $small = Join-Path $OutDir $smallName
    $big = Join-Path $OutDir 'smoke-big.bin'
    $smallHash = RandomFile $small (3 * 1024 * 1024)
    $bigHash = RandomFile $big (60 * 1024 * 1024)
    $downloaded = Join-Path $OutDir ('downloaded ' + $smallName)
    Remove-Item $downloaded -ErrorAction SilentlyContinue
    Log "local files: $small ($smallHash), $big"
    if ($WslDistro) { try { & wsl.exe -d $WslDistro -- sh -c 'rm -f /tmp/smoke-*.bin /tmp/smoke\ *.bin' | Out-Null } catch {} }
    try {
      StartApp @()
      Keys '^+t' 800; Shot 'new-ssh-tab'
      Keys ((EscapeKeys $SshTarget) + '{ENTER}') 2500
      Shot 'hostkey-dialog' -Screen; Log "foreground: '$(ForegroundTitle)'"
      Keys '{ENTER}' 1500
      Shot 'password-dialog' -Screen; Log "foreground: '$(ForegroundTitle)'"   # v0.4: "Remember password for user@host:22" (left unchecked)
      Keys ((EscapeKeys $password) + '{ENTER}') 3000
      Shot 'shell'
      # Session > Upload File to Remote... through the menu bar: Alt+S opens the menu, U is the accelerator.
      Keys '%s' 700; Shot 'session-menu' -Screen
      Keys 'u' 1500; Shot 'upload-dialog' -Screen; Log "foreground: '$(ForegroundTitle)'"
      DialogKeys '%l' 300; Paste $small 600                 # Alt+L: the local file field
      DialogKeys '%r' 600; Shot 'upload-default-remote' -Screen   # leaving the field derived <remote home>/<name>
      Paste '/tmp/' 400                                # an explicit directory with a trailing slash
      DialogKeys '%s' 300                                    # Alt+S = Start upload (from the field: no editingFinished)
      Start-Sleep -Milliseconds 1500; Shot 'upload-3mb-finished' -Screen   # "Uploaded ... (3.1 MB, SFTP)", remote path completed with the name
      $hashes = WslHashes; Log ("WSL: " + ($hashes -join ' | '))
      Check (($hashes | Where-Object { $_ -like "$smallHash*" }).Count -ge 1) "3 MB upload verified in WSL by sha256sum"
      # A 60 MB upload cancelled half-way: progress line with the method, then "Transfer cancelled" in red.
      DialogKeys '%l' 300; Paste $big 600
      DialogKeys '%r' 400; Paste '/tmp/' 400
      DialogKeys '%s' 200
      Start-Sleep -Milliseconds 1200; Shot 'upload-60mb-progress' -Screen
      DialogKeys '%c' 1200; Shot 'upload-cancelled' -Screen   # Alt+C = Cancel
      # Download the 3 MB file back: Alt+D (direction), Alt+R (remote file), Alt+T (save to), Alt+S.
      # (Alt+A is WeChat's global screenshot hotkey: it froze the whole desktop in an earlier run.)
      DialogKeys '%d' 600
      DialogKeys '%r' 300; Paste "/tmp/$smallName" 500
      DialogKeys '%t' 300; Paste $downloaded 500; Shot 'download-dialog' -Screen
      DialogKeys '%s' 300
      Start-Sleep -Milliseconds 2000; Shot 'download-finished' -Screen
      if (Test-Path $downloaded) {
        $downHash = (Get-FileHash $downloaded -Algorithm SHA256).Hash.ToLower()
        Check ($downHash -eq $smallHash) "downloaded file matches the upload ($downHash)"
      } else { Check $false "$downloaded was written" }
      Check (-not (Test-Path "$downloaded.part")) "no .part file left behind"
      # A remote file that does not exist: the error in red, the dialog still usable.
      DialogKeys '%r' 300; Paste '/tmp/smoke-does-not-exist.bin' 500
      DialogKeys '%s' 300
      Start-Sleep -Milliseconds 1500; Shot 'download-missing-error' -Screen
      # Cancel a large download half-way (the 60 MB upload may have been partial: whatever /tmp/smoke-big.bin holds).
      DialogKeys '%r' 300; Paste '/tmp/smoke-big.bin' 500
      DialogKeys '%t' 300; Paste (Join-Path $OutDir 'downloaded-big.bin') 500
      DialogKeys '%s' 200
      Start-Sleep -Milliseconds 800; Shot 'download-60mb-progress' -Screen
      DialogKeys '%c' 1200; Shot 'download-cancelled' -Screen
      Check (-not (Test-Path (Join-Path $OutDir 'downloaded-big.bin.part'))) "no .part after the cancelled download"
      DialogKeys '%e' 800; Shot 'dialog-closed'              # Alt+E = Close
      Keys 'ls -l /tmp/smoke*{ENTER}' 1200; Shot 'ls-remote'
      Keys 'rm -f /tmp/smoke*{ENTER}' 800
      Keys 'exit{ENTER}' 2000; Shot 'after-exit'
      CloseApp
    } finally {
      if ($null -ne $oldKnownHosts) { Set-ItemProperty -Path $regKey -Name knownHostsFile -Value $oldKnownHosts } else { Remove-ItemProperty -Path $regKey -Name knownHostsFile -ErrorAction SilentlyContinue }
      foreach ($name in @('lastLocalPath', 'lastRemotePath')) {
        if ($null -ne $oldPaths -and $null -ne $oldPaths.$name) { Set-ItemProperty -Path $pathsKey -Name $name -Value $oldPaths.$name }
        elseif (Test-Path $pathsKey) { Remove-ItemProperty -Path $pathsKey -Name $name -ErrorAction SilentlyContinue }
      }
    }
  }
  default { throw "unknown scenario $Scenario" }
}
if ($script:failures -gt 0) {
  Log ("done with {0} failed check(s)" -f $script:failures)
  exit 1
}
Log "done"
