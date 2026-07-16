import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import capture_profile as cp  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))


def test_rssi_hist_basic():
    r = cp.rssi_hist([-64] * 10 + [-70] * 10)
    assert r["n_rssi"] == 20
    assert abs(sum(r["rssi_bins"]) - 1.0) < 1e-9
    assert -71 <= r["rssi_median"] <= -63
    assert r["rssi_stdev"] > 0


def test_rssi_hist_empty_is_none():
    assert cp.rssi_hist([]) is None
    assert cp.rssi_hist([None, None]) is None


def test_rssi_hist_clamps_out_of_range():
    # -120 clamps into bin 0, -10 clamps into the top bin; both counted, hist still sums to 1
    r = cp.rssi_hist([-120, -10, -64])
    assert r["n_rssi"] == 3
    assert abs(sum(r["rssi_bins"]) - 1.0) < 1e-9


def test_parse_nordic_sample_has_rssi():
    prof = cp.build_profile(cp.parse_adverts(os.path.join(HERE, "sample_nordic.pcap")))
    assert "rssi_bins" in prof and prof["n_rssi"] > 0
    assert -110 <= prof["rssi_median"] <= -20


def test_parse_dlt256_sample_omits_rssi():
    # the synthetic DLT256 fixture has an all-zero PHDR -> no plausible RSSI -> key omitted
    prof = cp.build_profile(cp.parse_adverts(os.path.join(HERE, "sample_dlt256.pcap")))
    assert "rssi_bins" not in prof
