import os, subprocess, unittest

HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
EXE  = os.path.join(TOOL, "probe_dump.exe" if os.name == "nt" else "probe_dump")


def gen(seed, n, mode="--uniq"):
    out = subprocess.check_output([EXE, mode, str(seed), str(n)], text=True)
    return [ln.strip() for ln in out.splitlines() if ln.strip()]


@unittest.skipUnless(os.path.exists(EXE), "probe_dump not built")
class Uniq(unittest.TestCase):
    def test_no_duplicates_within_history(self):
        addrs = gen(1, 1500)                 # < UNIQ_HISTORY (2048): must be all-distinct
        self.assertEqual(len(addrs), 1500)
        self.assertEqual(len(set(addrs)), 1500, "uniq_try issued a duplicate within history")

    def test_reset_allows_reissue(self):
        # --uniqreset resets between the two halves and re-seeds identically -> the same
        # sequence reappears, proving uniq_reset clears history.
        a = gen(2, 200, "--uniqreset")
        self.assertEqual(len(a), 200)
        self.assertEqual(a[:100], a[100:], "uniq_reset did not clear history")


if __name__ == "__main__":
    unittest.main()
