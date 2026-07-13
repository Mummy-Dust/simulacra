$tool = $PSScriptRoot
$root = Join-Path $tool "..\.."
$rad  = Join-Path $root "components\simulacra_radar"
$cyd  = Join-Path $root "cyd\main"
cl /nologo /TC /O2 /D_CRT_SECURE_NO_WARNINGS /I $rad `
   (Join-Path $tool "ui_dump.c") (Join-Path $rad "radar_ui.c") /Fe:(Join-Path $tool "ui_dump.exe") | Out-Null
cl /nologo /TC /O2 /D_CRT_SECURE_NO_WARNINGS /I $cyd /I $rad `
   (Join-Path $tool "fleet_dump.c") (Join-Path $cyd "fleet_status.c") /Fe:(Join-Path $tool "fleet_dump.exe") | Out-Null
python -m unittest discover -s (Join-Path $tool "tests") -v
