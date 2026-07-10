# Probe behavioral-realism analyzers

Two validators that score the same three metrics from the spec's validation bar
(`docs/design/specs/2026-07-10-probe-behavioral-realism-design.md` §7). Both exit non-zero on FAIL.

| Metric | PASS condition |
|---|---|
| **seq-independence** | no run of ≥3 probes increments the 802.11 sequence number by exactly 1 across changing source MACs (that pattern = one shared hardware counter = the fingerprinting tell we killed) |
| **decorrelation** | no tight time window shows a big simultaneous volley of distinct MACs (no lockstep) |
| **constellation** | many distinct MACs over the run, low first-third ⇄ last-third overlap (the set turns over, so it can't become a portable fingerprint) |

## `sniff_analyze.py` — fast inner loop (our own C5 sniffer)

Flash a spare C5 with `SIMULACRA_SNIFF=1` (see `main/sniff.c`, `SNIFF_LOG_FRAMES=1`, parked ch1),
save its serial to a file, and:

```
python sniff_analyze.py sniff.log
python sniff_analyze.py sniff.log --rssi-min -60 --rssi-max -45   # isolate co-located decoys
```

This is the on-desk verifier used during development — no Kismet needed. One C5 sniffs while the
other C5 + the C6 act as decoys.

## `pcap_analyze.py` — attacker's-eye final check (Kismet)

Run a Kismet capture near the fleet with pcapng logging, then:

```
python pcap_analyze.py capture.pcapng
```

This reflects what a passive external surveillance tool actually sees. Use it for the milestone's
final sign-off. It correctly reports pre-fix captures as FAIL (shared consecutive seq runs).

## Ground rules

- **Captures are PII.** They live only in the gitignored `private/`. **Never commit a `.pcapng` /
  `.kismet` / sniffer log.** These scripts embed no capture data and are safe to commit.
- Neither analyzer measures the physical layer. Per the spec's honest ceiling, RSSI / AoA /
  co-location are **not** defeated by this milestone; a green board here does not mean invisible.
