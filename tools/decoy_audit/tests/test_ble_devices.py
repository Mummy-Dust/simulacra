import os, subprocess, unittest
from collections import defaultdict, Counter

HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
EXE  = os.path.join(TOOL, "synth_dump.exe" if os.name == "nt" else "synth_dump")

def sim(seed, n=16, ticks=4000, tick_ms=1000):
    out = subprocess.check_output([EXE, "--devices", str(seed), str(n), str(ticks), str(tick_ms)], text=True)
    rows = []
    for ln in out.splitlines():
        p = ln.split()
        if len(p) == 9 and p[0] == "D":
            # t, slot, addr, atype, role, event, company, itvl
            rows.append((int(p[1]), int(p[2]), p[3], p[4], p[5], p[6], int(p[7]), int(p[8])))
    return rows

@unittest.skipUnless(os.path.exists(EXE), "synth_dump not built")
class Spawn(unittest.TestCase):
    def test_initial_births(self):
        rows = sim(1, n=16)
        born0 = [r for r in rows if r[0] == 0 and r[5] == "born"]
        self.assertEqual(len(born0), 16, "expected one initial birth per slot at t=0")

    def test_addr_subtype_matches_atype(self):
        rows = sim(2, n=16)
        self.assertTrue(rows, "no events produced")
        for _, _, addr, at, _, _, _, _ in rows:
            top2 = int(addr[10:12], 16) >> 6   # addr[5] is the last octet (chars 10-11); MSB top-2-bits
            want = {"static": 3, "rpa": 1, "nrpa": 0}[at]
            self.assertEqual(top2, want, f"{at} address {addr} has wrong top-2-bits {top2}")

    def test_subtype_mix_realistic(self):
        rows = [r for r in sim(9, n=32) if r[5] == "born"]   # sample over all births/rebirths
        c = Counter(r[3] for r in rows); tot = sum(c.values())
        self.assertGreater(c["rpa"],  0.20 * tot, "RPA subtype under-represented")
        self.assertGreater(c["nrpa"], 0.03 * tot, "NRPA subtype under-represented")
        self.assertLess(c["static"],  0.75 * tot, "static subtype monoculture")

    def test_role_split_about_70_30(self):
        rows = [r for r in sim(5, n=32) if r[5] == "born"]
        c = Counter(r[4] for r in rows); tot = sum(c.values())
        self.assertGreater(c["transient"], 0.55 * tot, "transient share too low")
        self.assertGreater(c["resident"],  0.15 * tot, "resident share too low")

    def test_behaviour_populated(self):
        rows = sim(3, n=16)
        self.assertTrue(all(itvl > 0 for *_, itvl in rows), "a device has zero advertising interval")
