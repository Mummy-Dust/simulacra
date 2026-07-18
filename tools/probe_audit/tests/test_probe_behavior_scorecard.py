import os, sys, unittest
HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.dirname(HERE)
sys.path.insert(0, TOOL)
import probe_behavior_scorecard as S   # noqa: E402


class Axes(unittest.TestCase):
    def test_density_dominance_is_count_ratio(self):
        decoy = {"n_probing": 30, "wildcard_fraction": 1.0, "mult_counts": [10, 10, 10]}
        ref   = {"n_probing": 40, "wildcard_fraction": 0.8, "mult_counts": [1, 1, 2]}
        card = S.score(decoy, ref)
        self.assertAlmostEqual(card["density_dominance"], 30 / 40, places=6)

    def test_wildcard_axis_is_abs_diff(self):
        decoy = {"n_probing": 10, "wildcard_fraction": 1.0, "mult_counts": [5]}
        ref   = {"n_probing": 10, "wildcard_fraction": 0.6, "mult_counts": [5]}
        self.assertAlmostEqual(S.score(decoy, ref)["wildcard_fraction"], 0.4, places=6)

    def test_multiplicity_zero_when_shapes_match(self):
        prof = {"n_probing": 8, "wildcard_fraction": 1.0, "mult_counts": [9, 9]}
        self.assertLess(S.score(prof, dict(prof))["fingerprint_multiplicity"], 1e-9)

    def test_multiplicity_positive_when_shapes_differ(self):
        decoy = {"n_probing": 20, "wildcard_fraction": 1.0, "mult_counts": [20]}        # one huge cluster
        ref   = {"n_probing": 20, "wildcard_fraction": 1.0, "mult_counts": [1] * 20}      # 20 singletons
        self.assertGreater(S.score(decoy, ref)["fingerprint_multiplicity"], 0.3)

    def test_headline_is_max(self):
        decoy = {"n_probing": 30, "wildcard_fraction": 1.0, "mult_counts": [20]}
        ref   = {"n_probing": 40, "wildcard_fraction": 0.5, "mult_counts": [1] * 20}
        card = S.score(decoy, ref)
        self.assertEqual(card["headline"], max(card["density_dominance"],
                                               card["wildcard_fraction"],
                                               card["fingerprint_multiplicity"]))
        self.assertIn(card["headline_tell"],
                      ("density_dominance", "wildcard_fraction", "fingerprint_multiplicity"))

    def test_decoy_profile_from_agents(self):
        # A-records: (arch, born, wildcard, mac); 3 of arch 0, 1 of arch 1
        rows = [(0, 100, 1, "a"), (0, 200, 1, "b"), (0, 300, 1, "c"), (1, 100, 1, "d")]
        prof = S.decoy_profile_from_agents(rows)
        self.assertEqual(prof["n_probing"], 4)
        self.assertEqual(prof["wildcard_fraction"], 1.0)
        self.assertEqual(sorted(prof["mult_counts"]), [1, 3])


if __name__ == "__main__":
    unittest.main()
