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
    def test_address_type_is_a_realistic_mix(self):
        # decoys must present a static/RPA/NRPA blend (like a real crowd), NOT 100% static.
        # All three types present; static is the plurality but well under 100%.
        from collections import Counter
        r = self.rows(seed=9, n=256); c = Counter(x["atype"] for x in r); n = len(r)
        self.assertGreater(c["rpa"], 0.20*n)      # RPA-looking present (~36% target)
        self.assertGreater(c["public"], 0.03*n)   # NRPA/"public" present (~12% target)
        self.assertLess(c["static"], 0.75*n)      # not a static-random monoculture
        self.assertEqual(set(c) - {"static","rpa","public"}, set())  # no stray types
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
    def test_no_mfg_seed_is_no_mfg_on_air(self):
        # A 100%-no-mfg (OTHER) model must yield decoys that carry NO manufacturer element on air.
        # synth_dump reports the company PARSED FROM THE SERIALIZED PAYLOAD (like capture_profile on
        # the real side), so a tile/eddystone decoy reads no-mfg even though its template metadata
        # names a company, and the OTHER build must not fall back to an iBeacon (Apple 0x004C on air).
        seed_path = os.path.join(HERE, "_tmp_nomfg_seed.txt")
        with open(seed_path, "w") as fh:
            fh.write("POP 12\n")
            fh.write("OTHER 1000 0 0 100 0 0 0 0\n")   # all no-mfg mass, some interval weight
        out = subprocess.check_output([EXE, "4", "256", seed_path], text=True)
        rows = [json.loads(l) for l in out.splitlines() if l.strip()]
        os.remove(seed_path)
        no_mfg = sum(1 for r in rows if not r["company"] or r["company"] == 0xFFFF) / len(rows)
        self.assertGreater(no_mfg, 0.95)
    def test_other_bucket_samples_ambient_intervals(self):
        # No-mfg (OTHER) mass with all interval weight in the >2000ms bin must yield SLOW
        # decoys, not the 100-300ms fallback. Regression guard: generate_roster must sample
        # m->other_itvl_bins for the no-specific-vendor mass (OTHER path + diversify_fill).
        seed_path = os.path.join(HERE, "_tmp_itvl_seed.txt")
        with open(seed_path, "w") as fh:
            fh.write("POP 12\n")
            fh.write("OTHER 900 0 0 0 0 0 0 900\n")   # all mass in >2000ms bin
        out = subprocess.check_output([EXE, "5", "200", seed_path], text=True)
        rows = [json.loads(l) for l in out.splitlines() if l.strip()]
        os.remove(seed_path)
        slow = sum(1 for r in rows if r["itvl_ms"] >= 2000) / len(rows)
        self.assertGreater(slow, 0.7)

if __name__ == "__main__":
    unittest.main()
