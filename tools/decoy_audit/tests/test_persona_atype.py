import json, os, subprocess, unittest
from collections import Counter

HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
EXE  = os.path.join(TOOL, "synth_dump.exe" if os.name == "nt" else "synth_dump")


def persona_pop(seed, nph=16, ndev=24, ticks=4000, tick_ms=1000):
    out = subprocess.check_output(
        [EXE, "--persona-pop", str(seed), str(nph), str(ndev), str(ticks), str(tick_ms)], text=True)
    return [json.loads(l) for l in out.splitlines() if l.strip()]


@unittest.skipUnless(os.path.exists(EXE), "synth_dump not built")
class PersonaAtype(unittest.TestCase):
    def test_pop_has_both_rpa_and_non_rpa(self):
        rows = persona_pop(1)
        self.assertGreaterEqual(len(rows), 24, "persona-pop did not emit the full population")
        atypes = Counter(r["atype"] for r in rows)
        self.assertIn("rpa", atypes, "no RPA rows (bound personas missing)")
        self.assertTrue(atypes.get("static", 0) or atypes.get("public", 0),
                        "no non-RPA rows (unbound static/NRPA crowd missing)")

    def test_rpa_fraction_elevated_by_personas(self):
        rows = persona_pop(1)
        rpa = sum(1 for r in rows if r["atype"] == "rpa") / len(rows)
        # personas force 16 bound RPA + 8 unbound (~52/36/12) => RPA well above the roster's ~0.36
        self.assertGreater(rpa, 0.6, f"RPA fraction {rpa:.2f} not persona-elevated")

    def test_atype_detail_prints_decoy_and_real_fractions(self):
        import tempfile
        rows = persona_pop(1)
        with tempfile.TemporaryDirectory() as td:
            synth = os.path.join(td, "pop.ndjson")
            with open(synth, "w") as f:
                for r in rows: f.write(json.dumps(r) + "\n")
            prof = os.path.join(td, "profile.json")
            with open(prof, "w") as f:
                json.dump({"atype": {"static": 0.52, "rpa": 0.36, "public": 0.12},
                           "itvl_bins": [0, 0, 1, 0, 0, 0, 0], "vendor": {"none": 1.0}}, f)
            out = subprocess.check_output(
                ["C:/Program Files/Python312/python.exe" if os.name == "nt" else "python3",
                 os.path.join(TOOL, "scorecard.py"), synth, prof, "--atype-detail"], text=True)
        self.assertIn("atype detail", out)
        self.assertIn("decoy", out); self.assertIn("real", out)


if __name__ == "__main__":
    unittest.main()
