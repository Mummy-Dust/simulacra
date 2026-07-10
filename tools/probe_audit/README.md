# probe_audit — host verification for archetype probe requests

Host-compiles the firmware's pure probe-frame builder (`main/probe_frame.c`) and checks that the
injected 802.11 probe requests are **faithful to real modern phones** and **Law-3 safe**.

## What it checks

For each archetype (iPhone / Galaxy / Pixel / generic Android) × band (2.4 / 5 GHz):

- valid probe request (frame control `40 00`, broadcast DA + BSSID);
- **SSID IE present and wildcard (len 0)** — the Law-3 regression guard;
- DS-Parameter channel byte equals the requested channel;
- capability-IE diversity (HT everywhere; VHT on 5 GHz; generic Android omits HE, iPhone carries it);
- total length ≤ `PROBE_FRAME_MAX`;
- **byte-for-byte match to a checked-in reference fixture** (`fixtures/<arch>_<band>.hex`).

It also checks `probe_pick_archetype` produces the weighted 40/25/15/20 crowd mix.

## Run

```
pwsh run.ps1 -Rebuild        # build probe_dump.exe (MSVC) + run the tests
```

Non-Windows: `make && python -m unittest discover -s tests -v`.

## Fixtures

`fixtures/*.hex` are the modeled reference bytes. Regenerate them only after an intentional change
to an archetype's IE bytes:

```
pwsh run.ps1 -Rebuild
python tests/make_fixtures.py
```

Each fixture carries a `# source:` line. In the capture-driven enrichment milestone, real
(structure-only) captures replace these modeled bytes and the tests re-pin to them.
