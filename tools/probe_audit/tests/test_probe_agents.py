import os, subprocess, unittest
from collections import defaultdict, Counter

HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
EXE  = os.path.join(TOOL, "probe_dump.exe" if os.name == "nt" else "probe_dump")


def sim(seed, n=8, ticks=2000, tick_ms=2000):
    out = subprocess.check_output([EXE, "--agents", str(seed), str(n), str(ticks), str(tick_ms)], text=True)
    rows = []
    for ln in out.splitlines():
        p = ln.split()
        if len(p) == 4 and p[0] == "E":
            rows.append((int(p[1]), p[2], int(p[3])))   # t_ms, mac, seq
    return rows


@unittest.skipUnless(os.path.exists(EXE), "probe_dump not built")
class Agents(unittest.TestCase):
    # --- Task 2: sequence numbers ---
    def test_seq_monotonic_per_mac(self):
        rows = sim(2)
        seqs = defaultdict(list)
        for _, mac, sq in rows: seqs[mac].append(sq)
        self.assertTrue(seqs, "no emissions produced")
        for mac, ss in seqs.items():
            for a, b in zip(ss, ss[1:]):
                self.assertEqual(b, (a + 1) & 0x0FFF, f"{mac} seq not monotonic {a}->{b}")

    def test_seq_independent_across_macs(self):
        rows = sim(3)
        firsts = {}
        for _, mac, sq in rows: firsts.setdefault(mac, sq)
        self.assertGreater(len(set(firsts.values())), 1, "all agents share a seq base (not independent)")

    # --- Task 3: lifecycle turnover ---
    def test_constellation_turns_over(self):
        rows = sim(4, n=8, ticks=2000, tick_ms=2000)   # ~66 min simulated
        macs = set(m for _, m, _ in rows)
        self.assertGreater(len(macs), 8 * 2, "population did not turn over (stable constellation)")

    def test_mac_lifetime_bounded(self):
        rows = sim(6)
        span = defaultdict(lambda: [10**12, -1])
        for t, mac, _ in rows:
            s = span[mac]; s[0] = min(s[0], t); s[1] = max(s[1], t)
        for mac, (lo, hi) in span.items():
            self.assertLessEqual(hi - lo, 600000 + 180000, f"{mac} outlived its lifetime (stable constellation)")

    # --- Task 4: due selection + duty cadence ---
    def test_never_all_fire_same_tick(self):
        rows = sim(1, n=8)
        per_tick = Counter(t for t, _, _ in rows)
        self.assertTrue(per_tick, "no emissions produced")
        self.assertLess(max(per_tick.values()), 8, "a tick fired ALL agents at once (lockstep)")

    def test_active_idle_cadence_separation(self):
        rows = sim(7)
        cnt = Counter(m for _, m, _ in rows)
        counts = sorted(cnt.values(), reverse=True)
        self.assertGreater(len(counts), 1, "too few distinct agents to compare cadence")
        self.assertGreater(counts[0], counts[-1], "no active/idle cadence separation")


if __name__ == "__main__":
    unittest.main()
