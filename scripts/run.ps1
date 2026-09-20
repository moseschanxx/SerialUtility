<#
.SYNOPSIS  Launch the built application with the Qt bin directory on PATH.
.EXAMPLE   .\scripts\run.ps1 -Config Debug
#>
param(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')][string]$Config = 'Release',
    [string]$QtDir = $(if ($env:QT_ROOT) { $env:QT_ROOT } else { 'D:\Qt\6.8.3\msvc2022_64' })
)
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$exe = Join-Path $root "build\$Config\BuildAI-SerialUtility.exe"
if (-not (Test-Path $exe)) { throw "Not built yet: $exe  (run scripts\build.ps1 -Config $Config)" }
$env:PATH = "$(Join-Path $QtDir 'bin');$env:PATH"
Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe)
