import os, sys, unittest
HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.dirname(HERE); sys.path.insert(0, TOOL)
import capture_profile as cp          # noqa: E402
import discriminators as D            # noqa: E402


def _profile():
    # a broad real crowd (spatially spread) so the single-node decoy is separable
    real = cp.rssi_hist(list(range(-95, -35)))
    return {"rssi_bins": real["rssi_bins"], "rssi_median": real["rssi_median"]}


def _synth(n=256):
    # decoys carry a tx-dither field, like synth_dump rows
    dither = [-12, -9, -6, -3, 0, 3]
    return [{"tx": dither[i % len(dither)]} for i in range(n)]


class RssiFleet(unittest.TestCase):
    def test_k1_reproduces_single_node(self):
        synth, prof = _synth(), _profile()
        self.assertEqual(D.d_rssi_k(synth, prof, 1), D.d_rssi(synth, prof))

    def test_more_nodes_lower_separability(self):
        synth, prof = _synth(), _profile()
        d1 = D.d_rssi_k(synth, prof, 1)
        d8 = D.d_rssi_k(synth, prof, 8)
        self.assertLess(d8, d1)                     # spreading the crowd across nodes helps
        self.assertLess(d8, d1 - 0.02)              # by a clear margin, not just noise

    def test_monotonic_non_increasing(self):
        synth, prof = _synth(), _profile()
        vals = [D.d_rssi_k(synth, prof, k) for k in (1, 2, 3, 4, 6, 8)]
        for a, b in zip(vals, vals[1:]):
            self.assertLessEqual(b, a + 1e-9)       # non-increasing (tolerance for equal shapes)

    def test_bases_drawn_from_real_bins(self):
        import random
        centers = [cp.RSSI_LO + (i + 0.5) * cp.RSSI_W for i in range(cp.RSSI_NBINS)]
        # a real dist concentrated in one bin -> every sampled base is that bin's center
        bins = [0.0] * cp.RSSI_NBINS
        bins[5] = 1.0
        got = D._sample_bases_from_bins(bins, 20, random.Random(1))
        self.assertTrue(all(b == centers[5] for b in got))

    def test_missing_profile_rssi_returns_zero(self):
        self.assertEqual(D.d_rssi_k(_synth(), {}, 4), 0.0)


if __name__ == "__main__":
    unittest.main()
