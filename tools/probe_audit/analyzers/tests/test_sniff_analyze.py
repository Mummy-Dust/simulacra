import os
import sys

# import the analyzer module (sibling package dir), without running main()
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import sniff_analyze as sa  # noqa: E402


def test_seq_independence_pass_independent_counters():
    # Each MAC carries its OWN counter; no cross-MAC run of +1 steps. -> PASS (True)
    frames = [
        (0, "02:00:00:00:00:01", 100, -50),
        (1, "02:00:00:00:00:02", 500, -50),
        (2, "02:00:00:00:00:01", 101, -50),
        (3, "02:00:00:00:00:02", 501, -50),
        (4, "02:00:00:00:00:03", 900, -50),
    ]
    assert sa.check_seq_independence(frames) is True


def test_seq_independence_fail_shared_counter():
    # One shared HW counter: seq +1 on every frame while the MAC changes. -> FAIL (False)
    frames = [
        (0, "02:00:00:00:00:01", 10, -50),
        (1, "02:00:00:00:00:02", 11, -50),
        (2, "02:00:00:00:00:03", 12, -50),
        (3, "02:00:00:00:00:04", 13, -50),
    ]
    assert sa.check_seq_independence(frames) is False


def test_seq_independence_wraps_12bit():
    # 802.11 seq is 12-bit; a shared counter wrapping 4095->0 across MACs must still FAIL.
    frames = [
        (0, "02:00:00:00:00:01", 4094, -50),
        (1, "02:00:00:00:00:02", 4095, -50),
        (2, "02:00:00:00:00:03", 0, -50),
        (3, "02:00:00:00:00:04", 1, -50),
    ]
    assert sa.check_seq_independence(frames) is False
