import os, subprocess, unittest
from collections import defaultdict

HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
EXE  = os.path.join(TOOL, "probe_dump.exe" if os.name == "nt" else "probe_dump")


def wbind(seed, n=12, ticks=4000, tick_ms=1000):
    out = subprocess.check_output([EXE, "--wbind", str(seed), str(n), str(ticks), str(tick_ms)], text=True)
    rows = []
    for ln in out.splitlines():
        p = ln.split()
        if len(p) == 6 and p[0] == "W":
            # W <t> <idx> <mac> <arch> <generation>
            rows.append((int(p[1]), int(p[2]), p[3], int(p[4]), int(p[5])))
    return rows


@unittest.skipUnless(os.path.exists(EXE), "probe_dump not built")
class WifiBinding(unittest.TestCase):
    def test_agent_arch_follows_phantom_family(self):
        rows = wbind(1)
        self.assertTrue(rows, "no bound-agent events")
        # For each (idx, generation) the emitted arch must be stable (adopted from the phantom).
        arch_of = {}
        for _t, idx, _mac, arch, gen in rows:
            key = (idx, gen)
            if key in arch_of:
                self.assertEqual(arch_of[key], arch, "arch changed within one persona life")
            arch_of[key] = arch

    def test_bound_macs_unique_and_reincarnate_on_new_generation(self):
        rows = wbind(2)
        macs = [m for _t, _i, m, _a, _g in rows]
        self.assertEqual(len(macs), len(set(macs)), "a bound agent reused a MAC")
        # each agent index should show more than one generation (turnover happened)
        gens = defaultdict(set)
        for _t, idx, _m, _a, g in rows: gens[idx].add(g)
        self.assertTrue(any(len(v) > 1 for v in gens.values()), "no persona turnover observed")


if __name__ == "__main__":
    unittest.main()
