"""Regenerate fixtures/<arch>_<band>.hex from the built dumper.

Run once after intentionally changing an archetype's IE bytes:
    pwsh ../run.ps1 -Rebuild
    python make_fixtures.py
The test suite then pins these bytes; a later real-capture milestone replaces them.
"""
import os, subprocess
HERE = os.path.dirname(__file__)
TOOL = os.path.dirname(HERE)
EXE  = os.path.join(TOOL, "probe_dump.exe" if os.name == "nt" else "probe_dump")
FIX  = os.path.join(TOOL, "fixtures")
os.makedirs(FIX, exist_ok=True)

# (arch_idx, name, channel, band5)
CASES = [
    (0, "iphone", 6, 0), (0, "iphone", 36, 1),
    (1, "galaxy", 6, 0), (1, "galaxy", 36, 1),
    (2, "pixel",  6, 0), (2, "pixel",  36, 1),
    (3, "android", 6, 0), (3, "android", 36, 1),
]

for idx, name, ch, b5 in CASES:
    hexline = subprocess.check_output([EXE, str(idx), str(ch), str(b5)], text=True).strip()
    tag = "5" if b5 else "24"
    path = os.path.join(FIX, f"{name}_{tag}.hex")
    with open(path, "w") as fh:
        fh.write("# source: modeled probe request; replace with real capture later\n")
        fh.write(hexline + "\n")
    print("wrote", os.path.basename(path))
