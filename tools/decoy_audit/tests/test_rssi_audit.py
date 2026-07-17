import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, ROOT)
import capture_profile as cp  # noqa: E402

_spec = importlib.util.spec_from_file_location(
    "rssi_audit", os.path.join(ROOT, "analyzers", "rssi_audit.py"))
ra = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(ra)


def test_narrow_vs_broad_is_separable():
    decoy = ra.load_rssi(cp.rssi_hist([-64, -63, -65, -64, -66, -64, -63, -65] * 5))  # tight ~2 dB
    real = ra.load_rssi(cp.rssi_hist(list(range(-100, -40))))                          # broad 60 dB
    assert ra.rssi_separability(decoy, real) >= 0.3


def test_placement_invariant():
    shape = [-5, -3, -2, 0, 2, 3, 5]
    real = ra.load_rssi(cp.rssi_hist([-80 + s for s in shape] * 10))     # same shape,
    decoy = ra.load_rssi(cp.rssi_hist([-55 + s for s in shape] * 10))    # shifted 25 dB, both in range
    assert ra.rssi_separability(decoy, real) < 0.1


def test_missing_rssi_returns_none():
    assert ra.load_rssi({"atype": {}}) is None


def test_separability_shared_from_discriminators():
    # rssi_audit must reuse discriminators.rssi_separability, not its own copy.
    import discriminators as Dmod
    assert ra.rssi_separability is Dmod.rssi_separability


def test_read_log_rssi_parses_lines():
    p = os.path.join(HERE, "_tmp_obs.log")
    with open(p, "w") as f:
        f.write("W (100) observe: obs rssi=-40 company=0x004c\n")
        f.write("noise line\n")
        f.write("W (110) observe: obs rssi=-72 company=0xffff\n")
    try:
        assert ra.read_log_rssi(p) == [-40, -72]
    finally:
        os.remove(p)
