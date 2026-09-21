<#
.SYNOPSIS
    Build the Windows release artefacts locally: portable ZIP and Inno Setup installer.

.DESCRIPTION
    1. Runs scripts\build.ps1 -Config Release -Test -Deploy (unless -SkipBuild)
    2. Zips dist\Release\bin into dist\BuildAI-SerialUtility-<version>-windows-x64.zip
    3. Compiles packaging\windows\BuildAI-SerialUtility.iss with Inno Setup 6 (ISCC.exe) into
       dist\installer\BuildAI-SerialUtility-<version>-windows-x64-setup.exe, when ISCC is
       installed (https://jrsoftware.org/isdl.php or `winget install JRSoftware.InnoSetup`).
    4. Writes SHA256SUMS.txt next to the artefacts.
    The version is read from project(... VERSION x.y.z) in CMakeLists.txt.

.EXAMPLE
    .\scripts\package.ps1
.EXAMPLE
    .\scripts\package.ps1 -SkipBuild
#>
[CmdletBinding()]
param(
    [switch]$SkipBuild,
    [string]$QtDir = $(if ($env:QT_ROOT) { $env:QT_ROOT } else { 'D:\Qt\6.8.3\msvc2022_64' })
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$cmakeLine = Select-String -Path 'CMakeLists.txt' -Pattern '^\s*VERSION\s+(\d+\.\d+\.\d+)' | Select-Object -First 1
if (-not $cmakeLine) { throw 'Could not read the project version from CMakeLists.txt' }
$version = $cmakeLine.Matches[0].Groups[1].Value
Write-Host "==> Packaging BuildAI Serial Utility $version" -ForegroundColor Cyan

if (-not $SkipBuild) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'build.ps1') -Config Release -Test -Deploy -QtDir $QtDir
    if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
}
$binDir = Join-Path $root 'dist\Release\bin'
if (-not (Test-Path (Join-Path $binDir 'BuildAI-SerialUtility.exe'))) { throw "Deployed build not found in $binDir (run scripts\build.ps1 -Config Release -Deploy)" }

# --- portable zip ---------------------------------------------------------------------
$zip = Join-Path $root "dist\BuildAI-SerialUtility-$version-windows-x64.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $binDir '*') -DestinationPath $zip -CompressionLevel Optimal
Write-Host "zip:       $zip" -ForegroundColor Green

# --- installer ------------------------------------------------------------------------
$iscc = @(
    "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
    "$env:ProgramFiles\Inno Setup 6\ISCC.exe",
    "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) { $cmd = Get-Command ISCC.exe -ErrorAction SilentlyContinue; if ($cmd) { $iscc = $cmd.Source } }
$artefacts = @($zip)
if ($iscc) {
    $outDir = Join-Path $root 'dist\installer'
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null
    & $iscc "/DAppVersion=$version" "/DSourceDir=$binDir" "/DOutputDir=$outDir" (Join-Path $root 'packaging\windows\BuildAI-SerialUtility.iss')
    if ($LASTEXITCODE -ne 0) { throw "ISCC failed ($LASTEXITCODE)" }
    $setup = Join-Path $outDir "BuildAI-SerialUtility-$version-windows-x64-setup.exe"
    Write-Host "installer: $setup" -ForegroundColor Green
    $artefacts += $setup
} else {
    Write-Warning 'Inno Setup 6 (ISCC.exe) not found - installer skipped. Install it with: winget install JRSoftware.InnoSetup'
}

# --- checksums ------------------------------------------------------------------------
$sums = Join-Path $root 'dist\SHA256SUMS.txt'
$artefacts | ForEach-Object { "{0}  {1}" -f (Get-FileHash $_ -Algorithm SHA256).Hash.ToLower(), (Split-Path $_ -Leaf) } | Set-Content -Encoding ascii $sums
Write-Host "checksums: $sums" -ForegroundColor Green
Get-Content $sums
