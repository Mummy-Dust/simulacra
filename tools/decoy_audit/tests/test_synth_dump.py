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
    def test_model_seed_biases_vendor_mix(self):
        seed_path = os.path.join(HERE, "_tmp_seed.txt")
        with open(seed_path, "w") as fh:
            fh.write("POP 10\n")
            fh.write("V 0087 100 0 0 0 0 0 40 0\n")   # Garmin-heavy, 1000-2000ms bin
            fh.write("OTHER 0 0 0 0 0 0 0 0\n")
        out = subprocess.check_output([EXE, "3", "128", seed_path], text=True)
        rows = [json.loads(l) for l in out.splitlines() if l.strip()]
        os.remove(seed_path)
        share = sum(1 for r in rows if r["company"] == 0x0087) / len(rows)
        # A Garmin-dominated seed yields a Garmin-heavy crowd. The realized share is
        # capped below GEN_MAX_VENDOR_PCT (~40%): generate.c redirects a proportional
        # fraction of any over-represented vendor's draws to varied templates (diversity
        # floor), so a single-vendor seed lands ~0.35, well above the ~0.03 unbiased mix.
        self.assertGreater(share, 0.25)

if __name__ == "__main__":
    unittest.main()
