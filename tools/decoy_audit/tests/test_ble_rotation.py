import os, subprocess, unittest
HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
EXE = os.path.join(TOOL, "synth_dump.exe" if os.name == "nt" else "synth_dump")


def blerot(seed=1, ticks=45, tickms=60000):
    out = subprocess.check_output([EXE, "--blerot", str(seed), str(ticks), str(tickms)], text=True)
    return [(int(t), a, int(g)) for t, a, g in (ln.split() for ln in out.splitlines())]


@unittest.skipUnless(os.path.exists(EXE), "synth_dump not built")
class BleRotation(unittest.TestCase):
    def test_bound_rpa_rotates_intra_life(self):
        rows = blerot()                                   # 45 min, one bound slot (life 40min, gen 1)
        addrs = [a for _, a, _ in rows]
        self.assertGreaterEqual(len(rows), 3)             # initial addr + >=2 rotations
        self.assertEqual(len(addrs), len(set(addrs)))     # every rotation a fresh unique address
        self.assertEqual({g for _, _, g in rows}, {1})    # intra-life: generation never changed

    def test_rotation_spacing_in_band(self):
        times = [t for t, _, _ in blerot()]
        for a, b in zip(times, times[1:]):
            self.assertTrue(480000 <= b - a < 960000, f"gap {b-a} ms outside 8-15min (+1 tick)")


if __name__ == "__main__":
    unittest.main()
