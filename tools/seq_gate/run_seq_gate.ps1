<#
run_seq_gate.ps1 -- post-flash en_sys_seq regression gate.

Verifies this IDF/chip still honors esp_wifi_80211_tx(..., en_sys_seq=false): one C5 injects
probe requests pinned to a single channel; a second C5 sniffs that channel; the analyzer flags the
shared-hardware-counter signature. See docs/superpowers/specs/2026-07-15-en-sys-seq-gate-design.md.

PREREQUISITE: activate your ESP-IDF 5.5 environment first (so `idf.py` is on PATH). Run from anywhere.

  # normal run -> expect PASS:
  .\tools\seq_gate\run_seq_gate.ps1 -InjPort COM12 -SniffPort COM16
  # simulated regression -> expect FAIL (proves the gate discriminates):
  .\tools\seq_gate\run_seq_gate.ps1 -InjPort COM12 -SniffPort COM16 -Shared

NOTE: both boards are left in PROBE/SNIFF mode. Reflash them with your normal decoy build afterward.
#>
param(
  [Parameter(Mandatory)][string]$InjPort,
  [Parameter(Mandatory)][string]$SniffPort,
  [switch]$Shared,
  [int]$Channel = 1,
  [int]$CaptureSeconds = 30
)

if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
  Write-Output "ERR: idf.py not on PATH -- activate your ESP-IDF 5.5 env first (export.ps1)."
  exit 3
}
$repo = (Resolve-Path "$PSScriptRoot\..\..").Path
$analyzer = Join-Path $repo "tools\probe_audit\analyzers\sniff_analyze.py"
$log = Join-Path $env:TEMP "seq_gate_$SniffPort.log"
Set-Location $repo

function Clean-Build { Remove-Item "$repo\build","$repo\sdkconfig" -Recurse -Force -ErrorAction SilentlyContinue }

# --- 1) injector: single-channel PROBE, optionally forcing the shared-counter regression ---
Write-Output "[1/4] building + flashing injector ($InjPort, ch$Channel, shared=$($Shared.IsPresent))..."
Clean-Build
idf.py set-target esp32c5 *> "$env:TEMP\seq_gate_inj.log"
$injDefs = @("-DSIMULACRA_PROBE=1", "-DPROBE_FIX_CH=$Channel")
if ($Shared) { $injDefs += "-DPROBE_FORCE_SHARED=1" }
idf.py @injDefs build *>> "$env:TEMP\seq_gate_inj.log"
if ($LASTEXITCODE -ne 0) { Write-Output "ERR: injector build failed -- see $env:TEMP\seq_gate_inj.log"; exit 3 }
idf.py @injDefs -p $InjPort flash *>> "$env:TEMP\seq_gate_inj.log"
if ($LASTEXITCODE -ne 0) { Write-Output "ERR: injector flash failed -- see $env:TEMP\seq_gate_inj.log"; exit 3 }

# --- 2) sniffer: park on the SAME channel (clean build so the cached PROBE flag can't leak in) ---
Write-Output "[2/4] building + flashing sniffer ($SniffPort, ch$Channel)..."
Clean-Build
idf.py set-target esp32c5 *> "$env:TEMP\seq_gate_sniff.log"
$snfDefs = @("-DSIMULACRA_SNIFF=1", "-DSNIFF_FIXED_CH=$Channel")
idf.py @snfDefs build *>> "$env:TEMP\seq_gate_sniff.log"
if ($LASTEXITCODE -ne 0) { Write-Output "ERR: sniffer build failed -- see $env:TEMP\seq_gate_sniff.log"; exit 3 }
idf.py @snfDefs -p $SniffPort flash *>> "$env:TEMP\seq_gate_sniff.log"
if ($LASTEXITCODE -ne 0) { Write-Output "ERR: sniffer flash failed -- see $env:TEMP\seq_gate_sniff.log"; exit 3 }

# --- 3) capture the sniffer's serial (no reset: don't toggle DTR/RTS, just read the running board) ---
Write-Output "[3/4] capturing $SniffPort for ${CaptureSeconds}s..."
Start-Sleep -Seconds 2   # let the sniffer finish booting after the flash-reset
$sp = New-Object System.IO.Ports.SerialPort $SniffPort, 115200, ([System.IO.Ports.Parity]::None), 8, ([System.IO.Ports.StopBits]::One)
$sp.DtrEnable = $false; $sp.RtsEnable = $false; $sp.ReadTimeout = 1000
$sw = [System.IO.StreamWriter]::new($log, $false)
try {
  $sp.Open()
  $deadline = (Get-Date).AddSeconds($CaptureSeconds)
  while ((Get-Date) -lt $deadline) {
    try { $sw.WriteLine($sp.ReadLine()) } catch [TimeoutException] { }
  }
} finally {
  $sw.Close(); if ($sp.IsOpen) { $sp.Close() }
}

# --- 4) verdict ---
Write-Output "[4/4] analyzing..."
python $analyzer --seq-only $log
$code = $LASTEXITCODE
if     ($code -eq 0) { Write-Output "`nSEQ GATE: PASS  (en_sys_seq=false honored -- per-MAC counters independent)" }
elseif ($code -eq 1) { Write-Output "`nSEQ GATE: FAIL  (shared-counter signature -- en_sys_seq REGRESSED)" }
else                 { Write-Output "`nSEQ GATE: INCONCLUSIVE  (no frames parsed -- check channel match / wiring; log: $log)" }
Write-Output "reminder: both boards are in PROBE/SNIFF mode -- reflash them with your normal decoy build."
exit $code
