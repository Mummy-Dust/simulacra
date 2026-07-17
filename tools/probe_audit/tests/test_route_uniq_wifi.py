import os, subprocess, unittest
HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
EXE  = os.path.join(TOOL, "probe_dump.exe" if os.name == "nt" else "probe_dump")

@unittest.skipUnless(os.path.exists(EXE), "probe_dump not built")
class Route(unittest.TestCase):
    def test_probe_random_mac_records_via_uniq(self):
        out = subprocess.check_output([EXE, "--routecheck", "1"], text=True).strip()
        self.assertEqual(out, "0", "probe_random_mac did not record via uniq_try (routing missing)")

if __name__ == "__main__":
    unittest.main()
