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
    def test_no_mfg_decoys_bucket_to_none(self):
        # service-data decoys carry company 0xFFFF (RF_VENDOR_UNKNOWN); they must map to the
        # same "none" bucket the real profile uses, not appear as a literal "65535" vendor.
        sd = D.synth_distributions([{"atype":"static","itvl_ms":150,"company":65535}
                                    for _ in range(10)])
        self.assertIn("none", sd["vendor"])
        self.assertNotIn("65535", sd["vendor"])
    def test_all_scores_in_unit_range(self):
        for d in D.score_all(self.synth, self.profile):
            self.assertGreaterEqual(d["separability"],0.0)
            self.assertLessEqual(d["separability"],1.0)
            self.assertEqual(d["visibility"],"logic")

class ADStructure(unittest.TestCase):
    def _prof(self, ad_sig):
        return {"atype":{},"itvl_bins":[0]*7,"vendor":{},"ad_sig":ad_sig}
    def test_synth_distributions_has_ad_sig(self):
        sd=D.synth_distributions([{"atype":"static","itvl_ms":150,"company":0x75,"ad":"01,ff"}])
        self.assertAlmostEqual(sd["ad_sig"]["01,ff"], 1.0, places=6)
    def test_identical_structure_scores_zero(self):
        synth=[{"atype":"static","itvl_ms":150,"company":0x75,"ad":"01,ff"} for _ in range(10)]
        s={d["name"]:d["separability"] for d in D.score_all(synth, self._prof({"01,ff":1.0}))}
        self.assertAlmostEqual(s["ad_structure"], 0.0, places=6)
    def test_disjoint_structure_scores_high(self):
        # decoys only ever emit "01,ff"; the real crowd only "01,03,16" -> trivially separable
        synth=[{"atype":"static","itvl_ms":150,"company":0x75,"ad":"01,ff"} for _ in range(10)]
        s={d["name"]:d["separability"] for d in D.score_all(synth, self._prof({"01,03,16":1.0}))}
        self.assertGreater(s["ad_structure"], 0.9)
    def test_missing_profile_ad_sig_scores_zero(self):
        # an older profile.json without ad_sig -> no evidence, not a false positive
        synth=[{"atype":"static","itvl_ms":150,"company":0x75,"ad":"01,ff"}]
        prof={"atype":{},"itvl_bins":[0]*7,"vendor":{}}
        s={d["name"]:d["separability"] for d in D.score_all(synth, prof)}
        self.assertEqual(s["ad_structure"], 0.0)

class Presence(unittest.TestCase):
    def test_presence_bins_consecutive_diff(self):
        # slot 0: an address at t=0 replaced at t=600000 (10 min) -> first address present 10 min
        # (5-15m bin); the last address runs to sim end (here 0 ms -> <1m bin).
        text = ("D 0 0 aa rpa transient born 0 100\n"
                "D 600000 0 bb rpa transient rotate 0 100\n")
        b = D.presence_bins_from_devices(text)
        self.assertEqual(b[2], 1)   # 10-min presence -> 5-15m
        self.assertEqual(b[0], 1)   # last address, 0-length -> <1m
    def test_presence_identical_zero(self):
        self.assertAlmostEqual(D.d_presence([0,10,0,0,0,0,0], {"presence_ms_bins":[0,10,0,0,0,0,0]}), 0.0, places=6)
    def test_presence_disjoint_high(self):
        self.assertGreater(D.d_presence([0,0,0,0,0,0,10], {"presence_ms_bins":[10,0,0,0,0,0,0]}), 0.9)
    def test_presence_missing_profile_scores_zero(self):
        self.assertEqual(D.d_presence([1,2,3,0,0,0,0], {}), 0.0)

if __name__=="__main__":
    unittest.main()
