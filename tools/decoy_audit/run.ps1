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
    [string]$Capture = "$PSScriptRoot\..\..\private\long.pcap",
    [int]$Seed       = 1,
    [int]$Count      = 256,
    [string]$OutDir  = "$PSScriptRoot\..\..\private",
    [double]$Gate    = [double]::NaN,   # NaN = no gate
    [switch]$Rebuild
)
# Native tools (python, cl) may write progress to stderr; "Stop" would abort on that.
# We check $LASTEXITCODE explicitly after each call instead.
$ErrorActionPreference = "Continue"
$tool = $PSScriptRoot
$exe  = Join-Path $tool "synth_dump.exe"

# --- resolve paths -------------------------------------------------------
if (-not (Test-Path $Capture)) { Write-Error "capture not found: $Capture"; exit 2 }
if (-not (Test-Path $OutDir))  { New-Item -ItemType Directory -Path $OutDir | Out-Null }
$Capture   = (Resolve-Path $Capture).Path
$OutDir    = (Resolve-Path $OutDir).Path
$profileJson   = Join-Path $OutDir "profile.json"
$modelSeed = Join-Path $OutDir "model.seed"
$synth     = Join-Path $OutDir "synth.ndjson"
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
           synth_dump.c ble_hs_adv.c learn_stub.c `
           ..\..\main\generate.c ..\..\main\templates.c ..\..\main\roster.c ..\..\main\ble_devices.c `
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

# --- 3. scorecard --------------------------------------------------------
Write-Host "[3/3] scoring ..." -ForegroundColor Cyan
$scArgs = @((Join-Path $tool "scorecard.py"), $synth, $profileJson, "--json", $card)
if (-not [double]::IsNaN($Gate)) { $scArgs += @("--gate", $Gate) }
python @scArgs
$rc = $LASTEXITCODE
Write-Host "outputs in $OutDir (profile.json, model.seed, synth.ndjson, card.json)" -ForegroundColor DarkGray
exit $rc
