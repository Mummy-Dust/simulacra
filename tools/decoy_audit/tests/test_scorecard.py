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

if __name__=="__main__":
    unittest.main()
