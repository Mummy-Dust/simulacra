<#
.SYNOPSIS
  Build the host probe-frame dumper and run the archetype test suite.
.EXAMPLE
  .\run.ps1 -Rebuild      # force a fresh compile (use after changing probe_frame.c)
#>
[CmdletBinding()] param([switch]$Rebuild)
# Native tools may write progress to stderr; "Stop" would abort on that. Gate on $LASTEXITCODE.
$ErrorActionPreference = "Continue"
$tool = $PSScriptRoot
$exe  = Join-Path $tool "probe_dump.exe"

if ($Rebuild -or -not (Test-Path $exe)) {
    if (-not (Get-Command cl -ErrorAction SilentlyContinue)) {
        Write-Error "cl (MSVC) not on PATH. Open a 'Developer PowerShell for VS' or run vcvars first."
        exit 3
    }
    Write-Host "[build] compiling probe_dump.exe ..." -ForegroundColor Cyan
    Push-Location $tool
    try {
        cl /nologo /TC /O2 /D_CRT_SECURE_NO_WARNINGS /FIportab.h `
           /Ihost_stubs /I..\..\main `
           probe_dump.c ..\..\main\probe_frame.c ..\..\main\probe_agents.c ..\..\main\uniq_id.c /Fe:probe_dump.exe | Out-Null
        if ($LASTEXITCODE -ne 0) { Write-Error "build failed"; exit 3 }
    } finally { Pop-Location }
}

python -m unittest discover -s (Join-Path $tool "tests") -v
exit $LASTEXITCODE
