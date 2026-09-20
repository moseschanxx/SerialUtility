<#
.SYNOPSIS
    Configure, build, test and optionally deploy BuildAI Serial Utility with MSVC + Ninja + Qt 6.

.DESCRIPTION
    Sets up the Visual Studio x64 environment (vcvars64.bat) inside this PowerShell
    process, then drives CMake. Works from any shell (no need to open a "Developer
    Command Prompt"). Qt Creator users can ignore this script and open CMakeLists.txt
    directly - the CMakePresets.json is picked up automatically.

.PARAMETER Config      Debug | Release | RelWithDebInfo (default Release)
.PARAMETER QtDir       Qt installation prefix (default: $env:QT_ROOT or D:\Qt\6.8.3\msvc2022_64)
.PARAMETER BuildDir    Build directory (default: build\<Config>)
.PARAMETER Target      CMake target(s) to build (default: all)
.PARAMETER Clean       Remove the build directory first
.PARAMETER Test        Run ctest after building
.PARAMETER Deploy      cmake --install into dist\<Config> (runs windeployqt)
.PARAMETER Run         Launch the application after building
.PARAMETER KeepGoing   Pass -k 0 to ninja so it reports every error instead of stopping at the first
.PARAMETER VerboseBuild Verbose ninja output
.PARAMETER Jobs        Ninja parallelism (-j); 0 = ninja default

.EXAMPLE
    .\scripts\build.ps1 -Config Release -Test -Deploy
.EXAMPLE
    .\scripts\build.ps1 -Config Debug -Target su_core -KeepGoing
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')]
    [string]$Config = 'Release',
    [string]$QtDir = $(if ($env:QT_ROOT) { $env:QT_ROOT } else { 'D:\Qt\6.8.3\msvc2022_64' }),
    [string]$BuildDir = '',
    [string[]]$Target = @(),
    [switch]$Clean,
    [switch]$Test,
    [switch]$Deploy,
    [switch]$Run,
    [switch]$KeepGoing,
    [switch]$VerboseBuild,
    [int]$Jobs = 0
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
if (-not $BuildDir) { $BuildDir = Join-Path $root "build\$Config" }
elseif (-not [System.IO.Path]::IsPathRooted($BuildDir)) { $BuildDir = Join-Path $root $BuildDir }
$distDir = Join-Path $root "dist\$Config"

function Write-Step($msg) { Write-Host "==> $msg" -ForegroundColor Cyan }

# --- Locate vcvars64.bat -----------------------------------------------------
function Find-VcVars {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $inst = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
        if ($inst) {
            $bat = Join-Path $inst 'VC\Auxiliary\Build\vcvars64.bat'
            if (Test-Path $bat) { return $bat }
        }
    }
    foreach ($candidate in @(
        'D:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat',
        'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat',
        'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat',
        'C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat',
        'C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat')) {
        if (Test-Path $candidate) { return $candidate }
    }
    throw 'vcvars64.bat not found. Install Visual Studio 2022+ with the C++ desktop workload.'
}

# --- Import the MSVC environment into this process ---------------------------
function Import-VcEnv([string]$vcvars) {
    if ($env:VSCMD_ARG_TGT_ARCH -eq 'x64' -and (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        Write-Step 'MSVC x64 environment already active'
        return
    }
    Write-Step "Importing MSVC environment from $vcvars"
    $lines = cmd /c "`"$vcvars`" >nul 2>&1 && set"
    foreach ($line in $lines) {
        if ($line -match '^([^=]+)=(.*)$') {
            [System.Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
        }
    }
    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) { throw 'cl.exe not on PATH after vcvars64.bat' }
}

# --- Validate Qt -------------------------------------------------------------
if (-not (Test-Path (Join-Path $QtDir 'lib\cmake\Qt6\Qt6Config.cmake'))) {
    throw "Qt 6 not found at '$QtDir'. Pass -QtDir or set QT_ROOT."
}
$qtBin = Join-Path $QtDir 'bin'
$env:PATH = "$qtBin;$env:PATH"

Import-VcEnv (Find-VcVars)

if (-not (Get-Command cmake.exe -ErrorAction SilentlyContinue)) {
    $qtCmake = Join-Path (Split-Path -Parent (Split-Path -Parent $QtDir)) 'Tools\CMake_64\bin\cmake.exe'
    if (Test-Path $qtCmake) { $env:PATH = "$(Split-Path $qtCmake);$env:PATH" } else { throw 'cmake.exe not found' }
}
if (-not (Get-Command ninja.exe -ErrorAction SilentlyContinue)) {
    $qtNinja = Join-Path (Split-Path -Parent (Split-Path -Parent $QtDir)) 'Tools\Ninja\ninja.exe'
    if (Test-Path $qtNinja) { $env:PATH = "$(Split-Path $qtNinja);$env:PATH" } else { throw 'ninja.exe not found' }
}

# --- Clean -------------------------------------------------------------------
if ($Clean -and (Test-Path $BuildDir)) {
    Write-Step "Removing $BuildDir"
    Remove-Item -Recurse -Force $BuildDir
}

# --- Configure ---------------------------------------------------------------
Write-Step "Configuring ($Config) -> $BuildDir"
& cmake -S $root -B $BuildDir -G Ninja `
    "-DCMAKE_BUILD_TYPE=$Config" `
    "-DCMAKE_PREFIX_PATH=$QtDir" `
    "-DCMAKE_CXX_COMPILER=cl.exe" `
    "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed ($LASTEXITCODE)" }

# --- Build -------------------------------------------------------------------
$buildArgs = @('--build', $BuildDir, '--config', $Config)
if ($Target.Count -gt 0) { $buildArgs += '--target'; $buildArgs += $Target }
$ninjaArgs = @()
if ($KeepGoing) { $ninjaArgs += @('-k', '0') }
if ($VerboseBuild) { $ninjaArgs += '-v' }
if ($Jobs -gt 0) { $ninjaArgs += @('-j', "$Jobs") }
if ($ninjaArgs.Count -gt 0) { $buildArgs += '--'; $buildArgs += $ninjaArgs }

Write-Step "Building: cmake $($buildArgs -join ' ')"
& cmake @buildArgs
$buildRc = $LASTEXITCODE
if ($buildRc -ne 0) {
    Write-Host "Build FAILED ($buildRc)" -ForegroundColor Red
    exit $buildRc
}
Write-Host 'Build OK' -ForegroundColor Green

# --- Test --------------------------------------------------------------------
if ($Test) {
    Write-Step 'Running unit tests'
    & ctest --test-dir $BuildDir --output-on-failure -C $Config
    if ($LASTEXITCODE -ne 0) { Write-Host "Tests FAILED ($LASTEXITCODE)" -ForegroundColor Red; exit $LASTEXITCODE }
    Write-Host 'Tests OK' -ForegroundColor Green
}

# --- Deploy ------------------------------------------------------------------
if ($Deploy) {
    Write-Step "Deploying to $distDir (cmake --install + windeployqt)"
    if (Test-Path $distDir) { Remove-Item -Recurse -Force $distDir }
    & cmake --install $BuildDir --prefix $distDir --config $Config
    if ($LASTEXITCODE -ne 0) { throw "Install/deploy failed ($LASTEXITCODE)" }
    Write-Host "Deployed: $distDir\bin" -ForegroundColor Green
}

# --- Run ---------------------------------------------------------------------
if ($Run) {
    $exe = Join-Path $BuildDir 'BuildAI-SerialUtility.exe'
    if (-not (Test-Path $exe)) { throw "Executable not found: $exe" }
    Write-Step "Launching $exe"
    Start-Process -FilePath $exe -WorkingDirectory $BuildDir
}
