<#
.SYNOPSIS
  Verifica se o ambiente de desenvolvimento do firmware está completo.
  Ver docs/setup.md para instruções de instalação.
#>
$ErrorActionPreference = 'Continue'
$ok = $true

function Check($name, $test, $hint) {
  $result = & $test
  if ($result) {
    Write-Host ("[OK]   {0}: {1}" -f $name, $result) -ForegroundColor Green
  } else {
    Write-Host ("[FALTA] {0} — {1}" -f $name, $hint) -ForegroundColor Red
    $script:ok = $false
  }
}

Check 'CMake'  { (Get-Command cmake -ErrorAction SilentlyContinue)?.Source } 'winget install Kitware.CMake'
Check 'Ninja'  { (Get-Command ninja -ErrorAction SilentlyContinue)?.Source } 'winget install Ninja-build.Ninja'
Check 'Java'   { (Get-Command java  -ErrorAction SilentlyContinue)?.Source } 'winget install Microsoft.OpenJDK.17'

$armGcc = ($env:ARM_GCC_DIR ?? "$env:USERPROFILE\silabs\arm-gnu-toolchain") + '\bin\arm-none-eabi-gcc.exe'
Check 'ARM GCC' { if (Test-Path $armGcc) { $armGcc } } 'baixar ARM GNU Toolchain 12.2.Rel1 (ver docs/setup.md)'

$slc = $env:SLC_PATH ?? "$env:USERPROFILE\silabs\slc_cli\slc.bat"
Check 'slc-cli' { if (Test-Path $slc) { $slc } } 'baixar slc_cli_windows.zip (ver docs/setup.md)'

$sdk = ($env:SISDK_ROOT ?? "$env:USERPROFILE\silabs\simplicity_sdk") + '\simplicity_sdk.slcs'
Check 'Simplicity SDK' { if (Test-Path $sdk) { $sdk } } 'git clone simplicity_sdk (ver docs/setup.md)'

$cmd = $env:COMMANDER ?? "$env:USERPROFILE\silabs\commander\Simplicity Commander CLI\commander-cli.exe"
Check 'Commander' { if (Test-Path $cmd) { $cmd } } 'baixar SimplicityCommander-Windows.zip (ver docs/setup.md)'

if ($ok) { Write-Host "`nAmbiente completo." -ForegroundColor Green } else { exit 1 }
