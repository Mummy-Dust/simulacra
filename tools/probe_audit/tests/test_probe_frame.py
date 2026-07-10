import os, subprocess, unittest
from collections import Counter

HERE = os.path.dirname(__file__)
TOOL = os.path.dirname(HERE)
EXE  = os.path.join(TOOL, "probe_dump.exe" if os.name == "nt" else "probe_dump")


def build_arch(idx, ch, b5):
    out = subprocess.check_output([EXE, str(idx), str(ch), str(b5)], text=True).strip()
    return bytes.fromhex(out)


def ies(frame):
    """{id: value_bytes} walking the IE body after the 24-byte MAC header (which includes seq)."""
    body, i, out = frame[24:], 0, {}
    while i + 2 <= len(body):
        eid, ln = body[i], body[i + 1]
        out.setdefault(eid, body[i + 2:i + 2 + ln])
        i += 2 + ln
    return out


def fixture(name):
    p = os.path.join(TOOL, "fixtures", name)
    with open(p) as fh:
        line = [l for l in fh if not l.startswith("#")][0].strip()
    return bytes.fromhex(line)


# (name, arch_idx, channel, band5, fixture_file)
CASES = [
    ("iphone",  0, 6, 0, "iphone_24.hex"),  ("iphone",  0, 36, 1, "iphone_5.hex"),
    ("galaxy",  1, 6, 0, "galaxy_24.hex"),  ("galaxy",  1, 36, 1, "galaxy_5.hex"),
    ("pixel",   2, 6, 0, "pixel_24.hex"),   ("pixel",   2, 36, 1, "pixel_5.hex"),
    ("android", 3, 6, 0, "android_24.hex"), ("android", 3, 36, 1, "android_5.hex"),
]


@unittest.skipUnless(os.path.exists(EXE), "probe_dump not built")
class ProbeFrame(unittest.TestCase):
    def test_valid_probe_and_law3(self):
        for name, idx, ch, b5, _ in CASES:
            f = build_arch(idx, ch, b5)
            self.assertEqual(f[0:2], b"\x40\x00", f"{name} FC probe-req")
            self.assertEqual(f[4:10], b"\xff" * 6, f"{name} DA broadcast")
            self.assertEqual(f[16:22], b"\xff" * 6, f"{name} BSSID broadcast")
            d = ies(f)
            self.assertIn(0x00, d, f"{name} SSID present")
            self.assertEqual(len(d[0x00]), 0, f"{name} SSID must be wildcard (Law 3)")
            self.assertEqual(d[0x03], bytes([ch]), f"{name} DS channel patched")
            self.assertLessEqual(len(f), 256, f"{name} within PROBE_FRAME_MAX")

    def test_capability_ies_show_diversity(self):
        # HT present everywhere; modern phones carry HE; generic android intentionally omits HE.
        for name, idx, ch, b5, _ in CASES:
            d = ies(build_arch(idx, ch, b5))
            self.assertIn(0x2d, d, f"{name} HT caps present")
            if b5:
                self.assertIn(0xbf, d, f"{name} VHT caps on 5 GHz")
        self.assertNotIn(0xff, ies(build_arch(3, 6, 0)), "generic android 2.4 omits HE (diversity)")
        self.assertIn(0xff, ies(build_arch(0, 6, 0)), "iphone 2.4 carries HE")

    def test_matches_fixture(self):
        for name, idx, ch, b5, fx in CASES:
            self.assertEqual(build_arch(idx, ch, b5), fixture(fx), f"{name} byte-exact fixture")

    def test_weighted_pick_distribution(self):
        out = subprocess.check_output([EXE, "--pick", "7", "4000"], text=True).split()
        c = Counter(int(x) for x in out); n = len(out)
        self.assertEqual(set(c) - {0, 1, 2, 3}, set(), "only valid archetype indices")
        self.assertEqual(len(c), 4, "every archetype appears")
        self.assertGreater(c[0], c[1])                 # iphone (40) is the plurality
        self.assertGreater(c[0] / n, 0.33)             # ~0.40
        self.assertLess(c[0] / n, 0.47)
        self.assertGreater(c[3] / n, 0.13)             # android ~0.20
        self.assertLess(c[3] / n, 0.27)


if __name__ == "__main__":
    unittest.main()
