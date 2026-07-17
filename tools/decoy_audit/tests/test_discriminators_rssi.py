import os, sys, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.dirname(HERE)
sys.path.insert(0, TOOL)
import discriminators as D  # noqa: E402
import capture_profile as cp  # noqa: E402

DITHER = [-12, -9, -6, -3, 0, 3]                       # the real generator's tx set


def _synth(tx_values):
    return [{"atype": "static", "itvl_ms": 150, "company": 0, "tx": t} for t in tx_values]


class ModeledRssi(unittest.TestCase):
    def test_hist_stdev_matches_dither_plus_jitter(self):
        # uniform over the real dither set (stdev ~5.1) + sigma=4 jitter -> ~6.5 dB, deterministic.
        synth = _synth(DITHER * 1000)
        h = D.modeled_decoy_rssi_hist(synth)
        self.assertIsNotNone(h)
        self.assertTrue(5.6 <= h["rssi_stdev"] <= 7.4, f"modeled stdev {h['rssi_stdev']}")

    def test_deterministic(self):
        synth = _synth(DITHER * 50)
        self.assertEqual(D.modeled_decoy_rssi_hist(synth), D.modeled_decoy_rssi_hist(synth))

    def test_no_tx_rows_returns_none(self):
        self.assertIsNone(D.modeled_decoy_rssi_hist([{"atype": "static", "itvl_ms": 150}]))

    def test_d_rssi_zero_when_profile_has_no_rssi(self):
        self.assertEqual(D.d_rssi(_synth(DITHER * 10), {"atype": {}}), 0.0)

    def test_d_rssi_separates_tight_decoys_from_broad_real(self):
        # decoys tight (dither+jitter ~6.5 dB) vs a broad real crowd (~17 dB) -> a real tell.
        broad = cp.rssi_hist(list(range(-90, -40)))     # ~50 dB uniform, stdev ~14.5
        profile = {"rssi_bins": broad["rssi_bins"], "rssi_median": broad["rssi_median"]}
        sep = D.d_rssi(_synth(DITHER * 200), profile)
        self.assertTrue(0.0 < sep <= 1.0, f"expected a real in-range tell, got {sep}")
        self.assertGreater(sep, 0.15)

    def test_d_rssi_bounded_with_constant_tx(self):
        # even with no dither (constant tx), jitter keeps it finite & bounded (regression-gate safe).
        broad = cp.rssi_hist(list(range(-90, -40)))
        profile = {"rssi_bins": broad["rssi_bins"], "rssi_median": broad["rssi_median"]}
        sep = D.d_rssi(_synth([0] * 500), profile)
        self.assertTrue(0.0 <= sep <= 1.0)


if __name__ == "__main__":
    unittest.main()
