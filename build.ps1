<#
.SYNOPSIS
  Build pipeline for the SPL datalogger firmware (EFR32BG22 / BRD4184B).

.DESCRIPTION
  Wraps the three build stages:
    1. slc generate  — resolves SLC components into a CMake project under
       autogen_project/ (only needed when app.slcp or config/ change; -Generate)
    2. cmake + ninja — compiles the firmware (CMake presets from the
       generated project)
    3. commander     — flashes the board over the on-board J-Link (-Flash)

.EXAMPLE
  ./build.ps1 -Generate        # regenerate + build
  ./build.ps1                  # incremental build only
  ./build.ps1 -Flash           # build + flash
  ./build.ps1 -Clean -Generate # from scratch
#>
param(
  [switch]$Generate,
  [switch]$Flash,
  [switch]$Clean,
  # Thunderboard BG22 do usuário: BRD4184A rev A02 (mics em PC6/PC7, enable PA0).
  # A BRD4184B usa PB0/PB1 com enable em PC7.
  [string]$Board = 'brd4184a'
)

$ErrorActionPreference = 'Stop'

# Portable tools (cmake, ninja, java, arm-gcc, slc, commander) under %USERPROFILE%\silabs.
. (Join-Path $PSScriptRoot 'tools\env.ps1')

$RepoRoot = $PSScriptRoot
$GenDir   = Join-Path $RepoRoot 'autogen_project'
$CMakeDir = Join-Path $GenDir 'spl_datalogger_cmake'
$OutDir   = Join-Path $CMakeDir 'build\default_config'

if ($Clean -and (Test-Path $GenDir)) {
  Remove-Item -Recurse -Force $GenDir
  Write-Host "Cleaned $GenDir"
}

# --- 1. Generate --------------------------------------------------------------
if ($Generate -or -not (Test-Path $CMakeDir)) {
  Write-Host "== slc generate ==" -ForegroundColor Cyan
  slc generate (Join-Path $RepoRoot 'app.slcp') -d $GenDir --with $Board -o cmake
  if ($LASTEXITCODE -ne 0) { throw "slc generate failed" }
}

# --- 2. Build ------------------------------------------------------------------
Write-Host "== cmake build ==" -ForegroundColor Cyan
Push-Location $CMakeDir
try {
  if (-not (Test-Path (Join-Path $CMakeDir 'build\CMakeCache.txt'))) {
    cmake --preset project
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }
  }
  cmake --build --preset default_config
  if ($LASTEXITCODE -ne 0) { throw "build failed" }
} finally {
  Pop-Location
}

arm-none-eabi-size (Join-Path $OutDir 'spl_datalogger.out')
Write-Host "Artifacts in $OutDir" -ForegroundColor Green

# --- 3. Flash ------------------------------------------------------------------
if ($Flash) {
  Write-Host "== commander flash ==" -ForegroundColor Cyan
  commander-cli flash (Join-Path $OutDir 'spl_datalogger.s37') --device EFR32BG22C224F512IM40
  if ($LASTEXITCODE -ne 0) { throw "flash failed" }
}
