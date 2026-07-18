import os, subprocess, unittest
HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
EXE = os.path.join(TOOL, "probe_dump.exe" if os.name == "nt" else "probe_dump")


def wifiobs(script):
    out = subprocess.run([EXE, "--wifiobs"], input=script, capture_output=True, text=True).stdout
    return [int(x) for x in out.split()]


def _macs(n):
    return "".join("note 02000000%04x 0\n" % i for i in range(1, n + 1))


@unittest.skipUnless(os.path.exists(EXE), "probe_dump not built")
class WifiDensity(unittest.TestCase):
    def test_distinct_count(self):
        self.assertEqual(wifiobs("reset 1\n" + _macs(3) + "density 0\n"), [3])

    def test_dedup_same_mac(self):
        self.assertEqual(wifiobs("reset 1\nnote 020000000001 0\nnote 020000000001 100\ndensity 100\n"), [1])

    def test_ttl_expiry(self):
        # WIFI_OBS_TTL_MS default 180000 -> a MAC last seen at 0 is gone by 200000
        self.assertEqual(wifiobs("reset 1\nnote 020000000001 0\ndensity 200000\n"), [0])

    def test_target_floor_when_empty(self):
        self.assertEqual(wifiobs("reset 1\n" + "target 0\n" * 20)[-1], 2)     # WIFI_OBS_FLOOR

    def test_target_tracks_density(self):
        self.assertEqual(wifiobs("reset 1\n" + _macs(10) + "target 0\n" * 20)[-1], 10)

    def test_target_caps_at_max(self):
        self.assertEqual(wifiobs("reset 1\n" + _macs(24) + "target 0\n" * 20)[-1], 16)   # WIFI_OBS_CAP


if __name__ == "__main__":
    unittest.main()
