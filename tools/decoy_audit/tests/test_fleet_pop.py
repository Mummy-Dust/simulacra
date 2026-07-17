import os, subprocess, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.dirname(HERE)
EXE  = os.path.join(TOOL, "synth_dump.exe" if os.name == "nt" else "synth_dump")


def share(target, k):
    out = subprocess.check_output([EXE, "--fleet-share", str(target), str(k)], text=True)
    return int(out.strip())


@unittest.skipUnless(os.path.exists(EXE), "synth_dump not built")
class FleetPop(unittest.TestCase):
    def test_k1_is_identity(self):
        for t in (0, 1, 8, 16, 255):
            self.assertEqual(share(t, 1), t)

    def test_k2_halves_rounded(self):
        self.assertEqual(share(16, 2), 8)
        self.assertEqual(share(15, 2), 8)   # 7.5 -> 8 (round to nearest)
        self.assertEqual(share(1, 2), 1)    # floored, never 0

    def test_k_ge_target_floors_at_one(self):
        self.assertEqual(share(3, 8), 1)
        self.assertEqual(share(1, 5), 1)

    def test_shares_sum_close_to_target(self):
        # K nodes each running round(target/K) sum to within K of the target (rounding slack).
        for t, k in ((24, 3), (16, 2), (30, 4)):
            self.assertAlmostEqual(share(t, k) * k, t, delta=k)

    def test_fleet_size_default_is_one(self):
        # host build passes no -DSIMULACRA_FLEET_SIZE -> #ifndef default 1
        out = subprocess.check_output([EXE, "--fleet-size"], text=True).strip()
        self.assertEqual(out, "1")


if __name__ == "__main__":
    unittest.main()
