<#
.SYNOPSIS
  One-step decoy detectability audit: build (if needed) -> profile a capture ->
  generate a matched synthetic decoy crowd -> print the ranked scorecard.

.EXAMPLE
  .\run.ps1
      Audit the default bench capture (..\..\private\long.pcap).

.EXAMPLE
  .\run.ps1 -Capture ..\..\private\other.pcap -Count 512 -Gate 0.4
      Audit a different capture with 512 decoys and fail (exit 1) if headline > 0.4.

.EXAMPLE
  .\run.ps1 -Rebuild
      Force a fresh build of synth_dump before running (use after changing firmware).
#>
[CmdletBinding()]
param(
    [string]$Capture,                   # default: <repo>\private\long.pcap (resolved below)
    [int]$Seed       = 1,
    [int]$Count      = 256,
    [string]$OutDir,                    # default: <repo>\private (resolved below)
    [double]$Gate    = [double]::NaN,   # NaN = no gate
    [switch]$Rebuild
)
# Native tools (python, cl) may write progress to stderr; "Stop" would abort on that.
# We check $LASTEXITCODE explicitly after each call instead.
$ErrorActionPreference = "Continue"
# $PSScriptRoot is empty in some invocation modes (dot-source / -File with a relative path),
# which made the param-time defaults resolve to a bogus 'C:\..\..\private'. Derive the tool dir
# robustly here and fill any unset defaults from it.
$tool = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
if (-not $Capture) { $Capture = Join-Path $tool "..\..\private\long.pcap" }
if (-not $OutDir)  { $OutDir  = Join-Path $tool "..\..\private" }
$exe  = Join-Path $tool "synth_dump.exe"

# --- resolve paths -------------------------------------------------------
if (-not (Test-Path $Capture)) { Write-Error "capture not found: $Capture"; exit 2 }
New-Item -ItemType Directory -Force -Path $OutDir -ErrorAction SilentlyContinue | Out-Null
$Capture = (Resolve-Path -LiteralPath $Capture).Path
$OutDir  = (Resolve-Path -LiteralPath $OutDir).Path
$profileJson   = Join-Path $OutDir "profile.json"
$modelSeed = Join-Path $OutDir "model.seed"
$synth     = Join-Path $OutDir "synth.ndjson"
$devices   = Join-Path $OutDir "devices.txt"
$card      = Join-Path $OutDir "card.json"

# --- build synth_dump if missing or -Rebuild -----------------------------
if ($Rebuild -or -not (Test-Path $exe)) {
    if (-not (Get-Command cl -ErrorAction SilentlyContinue)) {
        Write-Error "cl (MSVC) not on PATH. Open a 'Developer PowerShell for VS' or run vcvars first."
        exit 3
    }
    Write-Host "[build] compiling synth_dump.exe ..." -ForegroundColor Cyan
    Push-Location $tool
    try {
        cl /nologo /TC /O2 /D_CRT_SECURE_NO_WARNINGS /FIportab.h `
           /Ihost_stubs /I..\..\main /I..\..\components\simulacra_radar `
           synth_dump.c ble_hs_adv.c roster_stub.c `
           ..\..\main\generate.c ..\..\main\templates.c ..\..\main\roster.c ..\..\main\ble_devices.c `
           ..\..\main\learn.c ..\..\components\simulacra_radar\law3.c ..\..\components\simulacra_radar\learn_wire.c `
           ..\..\main\uniq_id.c `
           /Fe:synth_dump.exe | Out-Null
        if ($LASTEXITCODE -ne 0) { Write-Error "build failed"; exit 3 }
    } finally { Pop-Location }
}

# --- 1. real capture -> profile + matched model-seed ---------------------
Write-Host "[1/3] profiling $Capture ..." -ForegroundColor Cyan
python (Join-Path $tool "capture_profile.py") $Capture $profileJson $modelSeed
if ($LASTEXITCODE -ne 0) { Write-Error "capture_profile failed"; exit 4 }

# --- 2. generate synthetic decoy crowd (ASCII, no UTF-16 BOM) ------------
Write-Host "[2/3] generating $Count decoys (seed $Seed) ..." -ForegroundColor Cyan
$rows = & $exe $Seed $Count $modelSeed
if ($LASTEXITCODE -ne 0) { Write-Error "synth_dump failed"; exit 5 }
Set-Content -Path $synth -Value $rows -Encoding ascii
# Temporal run for the presence-duration tell: ~6 h at 1 s/tick so the >120 min bin is reachable.
$devrows = & $exe --devices $Seed 24 22000 1000
if ($LASTEXITCODE -ne 0) { Write-Error "synth_dump --devices failed"; exit 5 }
Set-Content -Path $devices -Value $devrows -Encoding ascii

# --- 3. scorecard --------------------------------------------------------
Write-Host "[3/3] scoring ..." -ForegroundColor Cyan
$scArgs = @((Join-Path $tool "scorecard.py"), $synth, $profileJson, "--devices", $devices, "--json", $card)
if (-not [double]::IsNaN($Gate)) { $scArgs += @("--gate", $Gate) }
python @scArgs
$rc = $LASTEXITCODE
Write-Host "outputs in $OutDir (profile.json, model.seed, synth.ndjson, card.json)" -ForegroundColor DarkGray
exit $rc
