import json, os, subprocess, sys, unittest
HERE = os.path.dirname(__file__)
TOOL = os.path.dirname(HERE)
EXE  = os.path.join(TOOL, "synth_dump.exe" if os.name == "nt" else "synth_dump")

@unittest.skipUnless(os.path.exists(EXE), "synth_dump not built")
class SynthDump(unittest.TestCase):
    def rows(self, seed=1, n=64):
        out = subprocess.check_output([EXE, str(seed), str(n)], text=True)
        return [json.loads(l) for l in out.splitlines() if l.strip()]
    def test_emits_n_rows_with_keys(self):
        r = self.rows(n=64)
        self.assertEqual(len(r), 64)
        for k in ("addr","atype","company","itvl_ms","tx","arch","plen"):
            self.assertIn(k, r[0])
    def test_all_addresses_static_random(self):
        # decoys always use make_random_static_addr_pub -> static
        self.assertTrue(all(x["atype"] == "static" for x in self.rows()))
    def test_intervals_and_company_populated(self):
        r = self.rows()
        self.assertTrue(all(x["itvl_ms"] > 0 for x in r))
        self.assertTrue(any(x["company"] in (0x0075, 0x004C) for x in r))
    def test_deterministic_for_seed(self):
        self.assertEqual(self.rows(seed=7), self.rows(seed=7))

if __name__ == "__main__":
    unittest.main()
