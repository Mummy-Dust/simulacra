import os, subprocess, unittest
HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
EXE = os.path.join(TOOL, "probe_dump.exe" if os.name == "nt" else "probe_dump")


def agentrot(seed=1, ticks=35, tickms=60000):
    out = subprocess.check_output([EXE, "--agentrot", str(seed), str(ticks), str(tickms)], text=True)
    return [(int(t), m, int(g)) for t, m, g in (ln.split() for ln in out.splitlines())]


@unittest.skipUnless(os.path.exists(EXE), "probe_dump not built")
class AgentRotation(unittest.TestCase):
    def test_bound_mac_rotates_intra_life(self):
        rows = agentrot()                                 # 35 min < 40 min life -> no reincarnation
        macs = [m for _, m, _ in rows]
        self.assertGreaterEqual(len(rows), 2)             # initial MAC + >=1 rotation
        self.assertEqual(len(macs), len(set(macs)))       # each rotation a fresh unique MAC
        self.assertEqual({g for _, _, g in rows}, {1})    # intra-life: generation unchanged

    def test_spacing_in_band(self):
        times = [t for t, _, _ in agentrot()]
        for a, b in zip(times, times[1:]):
            self.assertTrue(480000 <= b - a < 960000, f"gap {b-a} ms outside 8-15min (+1 tick)")


if __name__ == "__main__":
    unittest.main()
