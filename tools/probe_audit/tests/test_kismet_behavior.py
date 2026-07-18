import json, os, sqlite3, sys, tempfile, unittest
HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(TOOL, "analyzers"))
import kismet_behavior as kb   # noqa: E402


def _dev(mac, fp=0, named=None, assoc=False):
    """Build a minimal Kismet device JSON blob. `named`=a list of probed SSID strings (None=wildcard)."""
    if named is not None:
        pm = [{"dot11.probedssid.ssid": s} for s in named]
    else:
        pm = [{"dot11.probedssid.ssid": ""}]   # wildcard entry
    return json.dumps({
        "kismet.device.base.macaddr": mac,
        "dot11.device": {
            "dot11.device.probe_fingerprint": fp,
            "dot11.device.probed_ssid_map": pm,
            "dot11.device.last_bssid": "11:22:33:44:55:66" if assoc else "00:00:00:00:00:00",
        },
    })


def _write_kismet(path, blobs):
    db = sqlite3.connect(path)
    db.execute("CREATE TABLE devices (device TEXT)")
    db.executemany("INSERT INTO devices (device) VALUES (?)", [(b,) for b in blobs])
    db.commit(); db.close()


class KismetBehavior(unittest.TestCase):
    def setUp(self):
        self.path = tempfile.NamedTemporaryFile(suffix=".kismet", delete=False).name
        blobs = [
            _dev("02:00:00:00:00:01", fp=111),                        # LA wildcard, fp 111
            _dev("02:00:00:00:00:02", fp=111),                        # LA wildcard, fp 111 (mult=2)
            _dev("02:00:00:00:00:03", fp=222, named=["homenet"]),      # LA named -> not wildcard
            _dev("02:00:00:00:00:04", fp=333, assoc=True),            # LA but associated -> excluded
            _dev("3c:5a:b4:00:00:05", fp=444),                        # global OUI (0x3c, LA bit clear) -> excluded
        ]
        _write_kismet(self.path, blobs)

    def tearDown(self):
        os.remove(self.path)

    def test_counts_only_randomized_probe_only(self):
        prof = kb.reference_profile(self.path)
        self.assertEqual(prof["n_probing"], 3)          # macs 01,02,03 (04 assoc, 05 global excluded)

    def test_wildcard_fraction(self):
        prof = kb.reference_profile(self.path)
        self.assertAlmostEqual(prof["wildcard_fraction"], 2 / 3, places=6)   # 01,02 wildcard; 03 named

    def test_multiplicity_counts(self):
        prof = kb.reference_profile(self.path)
        self.assertEqual(sorted(prof["mult_counts"]), [1, 2])   # fp111 has 2 macs, fp222 has 1

    def test_emits_no_pii(self):
        prof = kb.reference_profile(self.path)
        blob = json.dumps(prof)
        self.assertNotIn("homenet", blob)
        self.assertNotIn("02:00:00", blob)


_REAL = os.path.join(TOOL, "..", "..", "private", "Kismetscannew.kismet")


@unittest.skipUnless(os.path.exists(_REAL), "private/Kismetscannew.kismet not present")
class RealCapture(unittest.TestCase):
    def test_sane_ranges(self):
        prof = kb.reference_profile(_REAL)
        self.assertGreater(prof["n_probing"], 0)
        self.assertTrue(0.0 <= prof["wildcard_fraction"] <= 1.0)
        self.assertTrue(all(c >= 1 for c in prof["mult_counts"]))


if __name__ == "__main__":
    unittest.main()
