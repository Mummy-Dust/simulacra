$tool = $PSScriptRoot
$root = Join-Path $tool "..\.."
$rad  = Join-Path $root "components\simulacra_radar"
cl /nologo /TC /O2 /D_CRT_SECURE_NO_WARNINGS /I $rad `
   (Join-Path $tool "ui_dump.c") (Join-Path $rad "radar_ui.c") /Fe:(Join-Path $tool "ui_dump.exe") | Out-Null
python -m unittest discover -s (Join-Path $tool "tests") -v
