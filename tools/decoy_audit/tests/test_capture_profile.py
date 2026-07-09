import os, sys, unittest
HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
sys.path.insert(0, TOOL); sys.path.insert(0, HERE)
import make_fixtures, capture_profile as cp

class Profile(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.pcap = os.path.join(HERE, "sample_nordic.pcap")
        make_fixtures.sample_pcap(cls.pcap)
        cls.adv = cp.parse_adverts(cls.pcap)
        cls.prof = cp.build_profile(cls.adv)
    def test_parses_all_five(self):
        self.assertEqual(len(self.adv), 5)
    def test_address_types(self):
        at = self.prof["atype"]
        self.assertAlmostEqual(at["static"], 3/5)
        self.assertAlmostEqual(at["rpa"], 1/5)
        self.assertAlmostEqual(at["public"], 1/5)
    def test_vendor_distribution_sums_to_one(self):
        self.assertAlmostEqual(sum(self.prof["vendor"].values()), 1.0, places=6)
        self.assertIn(str(0x0075), self.prof["vendor"])
    def test_model_seed_roundtrip(self):
        seed = os.path.join(HERE, "_seed.txt")
        cp.write_model_seed(self.prof, seed)
        txt = open(seed).read(); os.remove(seed)
        self.assertIn("POP", txt); self.assertIn("V 0075", txt)
    def test_vendor_is_device_weighted_with_none_bucket(self):
        # a chatty device (10 adverts) must not dominate; co-travel is about entities.
        p = os.path.join(HERE, "sample_weighted.pcap")
        make_fixtures.sample_pcap_weighted(p)
        v = cp.build_profile(cp.parse_adverts(p))["vendor"]
        self.assertAlmostEqual(v[str(0x75)], 1/3, places=2)     # not ~0.9 (advert-weighted)
        self.assertAlmostEqual(v.get("none", 0), 1/3, places=2)  # no-mfg device -> none bucket
        self.assertAlmostEqual(sum(v.values()), 1.0, places=6)
    def test_model_seed_maps_none_to_other(self):
        p = os.path.join(HERE, "sample_weighted.pcap")
        make_fixtures.sample_pcap_weighted(p)
        prof = cp.build_profile(cp.parse_adverts(p))
        seed = os.path.join(HERE, "_seedw.txt")
        cp.write_model_seed(prof, seed)
        txt = open(seed).read(); os.remove(seed)
        other = [l for l in txt.splitlines() if l.startswith("OTHER")][0]
        self.assertGreater(int(other.split()[1]), 0)  # the no-mfg device share -> nonzero OTHER count
    def test_dlt256_decodes_identically(self):
        # the scan-for-AA reader must yield the same adverts for DLT256 framing
        p256 = os.path.join(HERE, "sample_dlt256.pcap")
        make_fixtures.sample_pcap_dlt256(p256)
        adv256 = cp.parse_adverts(p256)
        key = lambda a: (a["atype"], a["company"])
        self.assertEqual(sorted(map(key, adv256)), sorted(map(key, self.adv)))

if __name__ == "__main__":
    unittest.main()
