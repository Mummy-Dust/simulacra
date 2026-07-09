import os, sys, unittest
HERE=os.path.dirname(__file__); TOOL=os.path.dirname(HERE); sys.path.insert(0,TOOL)
import discriminators as D

class JS(unittest.TestCase):
    def test_identical_is_zero(self):
        self.assertAlmostEqual(D.js_divergence([0.5,0.5],[0.5,0.5]), 0.0, places=6)
    def test_disjoint_is_one(self):
        self.assertAlmostEqual(D.js_divergence([1,0],[0,1]), 1.0, places=6)

class Scores(unittest.TestCase):
    def setUp(self):
        # synth: all static-random, all 100-200ms, all vendor 0x75
        self.synth=[{"atype":"static","itvl_ms":150,"company":0x75} for _ in range(50)]
        self.profile={"atype":{"static":0.4,"rpa":0.4,"public":0.2},
                      "itvl_bins":[0,0,0.2,0.2,0.2,0.2,0.2],
                      "vendor":{str(0x75):0.3, str(0x4c):0.7}}
    def test_address_type_tell_is_high(self):
        s={d["name"]:d["separability"] for d in D.score_all(self.synth, self.profile)}
        self.assertGreater(s["address_type_mix"], 0.3)  # 100% static vs 40/40/20 real
    def test_all_scores_in_unit_range(self):
        for d in D.score_all(self.synth, self.profile):
            self.assertGreaterEqual(d["separability"],0.0)
            self.assertLessEqual(d["separability"],1.0)
            self.assertEqual(d["visibility"],"logic")

if __name__=="__main__":
    unittest.main()
