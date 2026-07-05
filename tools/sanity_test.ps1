<#
.SYNOPSIS
  Testes de sanidade do firmware na placa, via CLI serial (VCOM).

.DESCRIPTION
  Pré-requisito: firmware gravado (./build.ps1 -Flash) e placa no USB.
  Testa: banner de boot, status, medição periódica, persistência NVM3 e
  recuperação por watchdog (comando hang).

.EXAMPLE
  ./tools/sanity_test.ps1 -Port COM7
#>
param(
  [string]$Port = 'COM7'
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'env.ps1')

$script:failures = 0

function Assert($name, $cond, $detail = '') {
  if ($cond) {
    Write-Host "[PASS] $name" -ForegroundColor Green
  } else {
    Write-Host "[FAIL] $name  $detail" -ForegroundColor Red
    $script:failures++
  }
}

function Open-Serial {
  $sp = [System.IO.Ports.SerialPort]::new($Port, 115200, 'None', 8, 'One')
  $sp.NewLine = "`n"
  $sp.ReadTimeout = 500
  $sp.Open()
  return $sp
}

function Read-For($sp, [int]$seconds) {
  $buf = ''
  $deadline = (Get-Date).AddSeconds($seconds)
  while ((Get-Date) -lt $deadline) {
    try { $buf += $sp.ReadExisting() } catch {}
    Start-Sleep -Milliseconds 100
  }
  return $buf
}

function Send-Cmd($sp, $cmd, [int]$waitSeconds = 2) {
  $sp.DiscardInBuffer()
  $sp.Write("$cmd`r`n")
  return Read-For $sp $waitSeconds
}

function Get-Field($text, $name) {
  if ($text -match "(?m)^\s*$name\s*:\s*(\S+)") { return $Matches[1] }
  return $null
}

Write-Host "== Sanity test na porta $Port ==" -ForegroundColor Cyan
$sp = Open-Serial
try {
  # --- 1. Boot banner (reset via debugger) ------------------------------------
  Write-Host "-- resetando a placa --"
  commander-cli device reset --device EFR32BG22C224F512IM40 | Out-Null
  $boot = Read-For $sp 6
  Assert 'banner de boot' ($boot -match 'SPL datalogger booting') ($boot -replace "`r?`n", ' | ')
  Assert 'microfone streaming' ($boot -match 'microphone streaming')
  Assert 'BLE advertising' ($boot -match 'BLE stack booted')
  Assert 'sem falha de NVM3 no boot' ($boot -notmatch 'NVM3 datalog init failed')
  Assert 'sem falha do microfone' ($boot -notmatch 'microphone start failed')

  # --- 2. status ----------------------------------------------------------------
  $st = Send-Cmd $sp 'status' 3
  Assert 'comando status responde' ($st -match 'uptime_s')
  $bootId0 = [uint64](Get-Field $st 'boot_id')
  $wdog0   = [uint64](Get-Field $st 'wdog_resets')
  $recs0   = [uint64]((Get-Field $st 'records') ?? '0')
  Write-Host ("     boot_id=$bootId0 wdog_resets=$wdog0 records=$recs0")

  # --- 3. medição periódica + validação exata do DSP -----------------------------
  # Tom sintético de 1 kHz a -26 dBFS no lugar do microfone: por definição da
  # calibração, LAeq TEM que dar 94.00 dB(A) (ponderação A = 0 dB em 1 kHz).
  # Funciona inclusive em placas BRD4184A (sem microfone).
  $null = Send-Cmd $sp 'interval 5' 2
  $null = Send-Cmd $sp 'testtone 1000 -2600' 2
  Write-Host '-- tom 1 kHz @ -26 dBFS; aguardando 2 intervalos de 5 s --'
  $meas = Read-For $sp 12
  Assert 'registros periodicos aparecem no log' ($meas -match 'rec \d+: LAeq')
  Assert 'DSP exato: LAeq = 94.0 dB(A) com tom de 1 kHz' ($meas -match 'LAeq 9[34]\.\d+ dB\(A\)  LAFmax 9[34]\.9\d')
  $null = Send-Cmd $sp 'testtone 0 0' 2
  $st2 = Send-Cmd $sp 'status' 3
  $recs1 = [uint64]((Get-Field $st2 'records') ?? '0')
  Assert 'contagem de registros cresceu' ($recs1 -gt $recs0) "antes=$recs0 depois=$recs1"
  Assert 'sem overruns de audio' (((Get-Field $st2 'mic_overruns') ?? '0') -eq '0')

  # --- 4. watchdog (hang) ---------------------------------------------------------
  Write-Host '-- comando hang: watchdog deve resetar em ~2 s --'
  $sp.Write("hang`r`n")
  $reboot = Read-For $sp 8
  Assert 'placa reiniciou apos hang' ($reboot -match 'SPL datalogger booting')
  Assert 'causa de reset = watchdog' ($reboot -match 'WATCHDOG RESET')

  $st3 = Send-Cmd $sp 'status' 3
  $bootId1 = [uint64](Get-Field $st3 'boot_id')
  $wdog1   = [uint64](Get-Field $st3 'wdog_resets')
  $recs2   = [uint64]((Get-Field $st3 'records') ?? '0')
  Assert 'boot_id incrementou' ($bootId1 -eq $bootId0 + 1) "antes=$bootId0 depois=$bootId1"
  Assert 'contador de watchdog incrementou' ($wdog1 -eq $wdog0 + 1) "antes=$wdog0 depois=$wdog1"
  Assert 'registros sobreviveram ao reset' ($recs2 -ge $recs1) "antes=$recs1 depois=$recs2"
  Assert 'intervalo persistiu (5 s)' ($st3 -match 'interval_s: 5')

  # --- 5. dump -----------------------------------------------------------------------
  $dump = Send-Cmd $sp 'dump' 6
  Assert 'dump imprime CSV' ($dump -match 'seq,boot_id,uptime_s,laeq_db,lafmax_db')
  Assert 'dump termina com end' ($dump -match '(?m)^end')

  # --- restaura configuracao padrao ---------------------------------------------------
  $null = Send-Cmd $sp 'interval 20' 2
} finally {
  $sp.Close()
}

Write-Host ''
if ($script:failures -eq 0) {
  Write-Host 'TODOS OS TESTES PASSARAM' -ForegroundColor Green
} else {
  Write-Host "$($script:failures) TESTE(S) FALHARAM" -ForegroundColor Red
  exit 1
}
