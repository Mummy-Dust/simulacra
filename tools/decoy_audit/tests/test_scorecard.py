import io, json, os, sys, unittest
HERE=os.path.dirname(__file__); TOOL=os.path.dirname(HERE); sys.path.insert(0,TOOL)
import scorecard as SC

class Card(unittest.TestCase):
    def setUp(self):
        self.synth=[{"atype":"static","itvl_ms":150,"company":0x75} for _ in range(30)]
        self.profile={"atype":{"static":0.4,"rpa":0.4,"public":0.2},
                      "itvl_bins":[0,0,1.0,0,0,0,0],
                      "vendor":{str(0x75):1.0}}
    def test_headline_is_max(self):
        card=SC.build_scorecard(self.synth,self.profile)
        self.assertEqual(card["headline"], max(d["separability"] for d in card["discriminators"]))
    def test_ranked_descending(self):
        ds=SC.build_scorecard(self.synth,self.profile)["discriminators"]
        self.assertEqual(ds, sorted(ds, key=lambda d:-d["separability"]))
    def test_headline_tell_matches(self):
        card=SC.build_scorecard(self.synth,self.profile)
        self.assertEqual(card["headline_tell"], card["discriminators"][0]["name"])
    def test_interval_and_vendor_indistinguishable_here(self):
        # synth matches real on interval(100-200) and vendor(0x75) -> those tells ~0
        card=SC.build_scorecard(self.synth,self.profile)
        s={d["name"]:d["separability"] for d in card["discriminators"]}
        self.assertLess(s["interval_distribution"],1e-6)
        self.assertLess(s["vendor_histogram"],1e-6)

class Rssi(unittest.TestCase):
    def setUp(self):
        # decoys tight in tx; real crowd broad in RSSI -> rssi_physical should be a real tell.
        self.synth=[{"atype":"static","itvl_ms":150,"company":0x75,"tx":t}
                    for t in ([-12,-9,-6,-3,0,3]*20)]
        import capture_profile as cp
        broad=cp.rssi_hist(list(range(-90,-40)))
        self.profile={"atype":{"static":1.0,"rpa":0.0,"public":0.0},
                      "itvl_bins":[0,0,1.0,0,0,0,0],"vendor":{str(0x75):1.0},
                      "rssi_bins":broad["rssi_bins"],"rssi_median":broad["rssi_median"]}
    def test_rssi_row_present_and_modeled(self):
        card=SC.build_scorecard(self.synth,self.profile)
        row=next((d for d in card["discriminators"] if d["name"]=="rssi_physical"), None)
        self.assertIsNotNone(row, "rssi_physical row missing")
        self.assertEqual(row["visibility"], "modeled")
        self.assertTrue(0.0 < row["separability"] <= 1.0)
    def test_rssi_absent_without_profile_rssi(self):
        prof={k:v for k,v in self.profile.items() if k not in ("rssi_bins","rssi_median")}
        names=[d["name"] for d in SC.build_scorecard(self.synth,prof)["discriminators"]]
        self.assertNotIn("rssi_physical", names)
    def test_rssi_headline_eligible(self):
        # headline is still the max across all rows, rssi included.
        card=SC.build_scorecard(self.synth,self.profile)
        self.assertEqual(card["headline"], max(d["separability"] for d in card["discriminators"]))


if __name__=="__main__":
    unittest.main()
