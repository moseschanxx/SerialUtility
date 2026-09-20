<#
.SYNOPSIS  Re-scan sources for tr() strings and update translations/*.ts (lupdate).
.EXAMPLE   .\scripts\update-translations.ps1
#>
param([string]$QtDir = $(if ($env:QT_ROOT) { $env:QT_ROOT } else { 'D:\Qt\6.8.3\msvc2022_64' }))
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$lupdate = Join-Path $QtDir 'bin\lupdate.exe'
if (-not (Test-Path $lupdate)) { throw "lupdate not found at $lupdate" }
& $lupdate -no-obsolete -locations relative -recursive (Join-Path $root 'src') `
    -ts (Join-Path $root 'translations\zh_CN.ts') (Join-Path $root 'translations\en_US.ts')
