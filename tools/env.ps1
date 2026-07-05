# Adds the portable toolchain locations under %USERPROFILE%\silabs to PATH
# for the current session. Dot-source this (or let build.ps1 do it):
#   . .\tools\env.ps1
$silabs = "$env:USERPROFILE\silabs"

$env:ARM_GCC_DIR = $env:ARM_GCC_DIR ?? "$silabs\arm-gnu-toolchain"
# slc-cli requires Java 21+ (jdk17 remains for tools that prefer it).
$env:JAVA_HOME   = $env:JAVA_HOME   ?? "$silabs\jdk21"
$env:SISDK_ROOT  = $env:SISDK_ROOT  ?? "$silabs\simplicity_sdk"

$toolPaths = @(
  "$silabs\tools\cmake\bin",
  "$silabs\tools\ninja",
  "$env:JAVA_HOME\bin",
  "$env:ARM_GCC_DIR\bin",
  "$silabs\slc_cli",
  "$silabs\commander\Simplicity Commander CLI"
)
foreach ($p in $toolPaths) {
  if ((Test-Path $p) -and (($env:Path -split ';') -notcontains $p)) {
    $env:Path = "$p;$env:Path"
  }
}
