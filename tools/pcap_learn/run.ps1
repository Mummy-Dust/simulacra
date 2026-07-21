<#
.SYNOPSIS
  One-step tracker/surveillance signature scan: parse a real BLE capture -> run it
  through the actual sig_match.c + sig_seed.c matcher -> print tracker hits, the
  AirTag selectivity check, and per-device dwell/co-travel analysis.

.EXAMPLE
  .\run.ps1 -Capture ..\..\private\errands.pcap
      Scan a capture for tracker signatures.

.EXAMPLE
  .\run.ps1 -Capture ..\..\private\errands.pcap -Rebuild
      Force a fresh build of sig_scan.exe first (use after changing sig_match.c/sig_seed.c).
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Capture,
    [switch]$Rebuild
)
$ErrorActionPreference = "Continue"
$tool = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
$exe  = Join-Path $tool "sig_scan.exe"

if (-not (Test-Path $Capture)) { Write-Error "capture not found: $Capture"; exit 2 }
$Capture = (Resolve-Path -LiteralPath $Capture).Path
$adverts = Join-Path $tool "_scan_adverts.ndjson"

if ($Rebuild -or -not (Test-Path $exe)) {
    if (-not (Get-Command cl -ErrorAction SilentlyContinue)) {
        Write-Error "cl (MSVC) not on PATH. Open a 'Developer PowerShell for VS' or run vcvars first."
        exit 3
    }
    Write-Host "[build] compiling sig_scan.exe ..." -ForegroundColor Cyan
    Push-Location $tool
    try {
        cl /nologo /TC /O2 /D_CRT_SECURE_NO_WARNINGS /FIportab.h `
           /Ihost_stubs /I..\..\components\simulacra_radar /I..\..\main `
           sig_scan.c ..\..\components\simulacra_radar\sig_match.c ..\..\components\simulacra_radar\sig_seed.c `
           /Fe:sig_scan.exe | Out-Null
        if ($LASTEXITCODE -ne 0) { Write-Error "build failed"; exit 3 }
    } finally { Pop-Location }
}

Write-Host "[1/2] parsing $Capture ..." -ForegroundColor Cyan
# PowerShell `>` writes UTF-16LE with a BOM by default -> sig_scan.exe (plain C fgets/fopen)
# can't parse it as NDJSON and silently scans 0 adverts. Force ASCII, no BOM (see
# private/TOOLING-GOTCHAS.md; same gotcha decoy_audit's run.ps1 already works around).
$rows = python (Join-Path $tool "parse_pcap.py") $Capture
if ($LASTEXITCODE -ne 0) { Write-Error "parse_pcap failed"; exit 4 }
Set-Content -Path $adverts -Value $rows -Encoding ascii

Write-Host "[2/2] scanning for tracker signatures ..." -ForegroundColor Cyan
& $exe $adverts
exit $LASTEXITCODE
