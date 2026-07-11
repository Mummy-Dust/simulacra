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

@unittest.skipUnless(os.path.exists(EXE), "synth_dump not built")
class Rotation(unittest.TestCase):
    def segments(self, rows):
        # group each slot's events into life-segments delimited by 'born'
        by_slot = defaultdict(list)
        for r in sorted(rows, key=lambda r: (r[1], r[0])):
            by_slot[r[1]].append(r)
        segs = []
        for slot, evs in by_slot.items():
            cur = None
            for e in evs:
                if e[5] == "born":
                    if cur: segs.append(cur)
                    cur = [e]
                elif cur:
                    cur.append(e)
            if cur: segs.append(cur)
        return segs

    def test_static_never_rotates(self):
        segs = self.segments(sim(11, n=24, ticks=6000, tick_ms=1000))
        static_segs = [s for s in segs if s[0][3] == "static"]
        self.assertTrue(static_segs, "no static devices sampled")
        for s in static_segs:
            self.assertEqual([e for e in s if e[5] == "rotate"], [], "a static device rotated")

    def test_rpa_rotation_in_band(self):
        segs = self.segments(sim(12, n=24, ticks=8000, tick_ms=1000))
        rots = []
        for s in segs:
            if s[0][3] != "rpa": continue
            ts = [e[0] for e in s if e[5] in ("born", "rotate")]
            rots += [b - a for a, b in zip(ts, ts[1:])]
        self.assertTrue(rots, "no RPA rotations observed")
        # every observed inter-rotation gap sits in the 10-20 min band (ms), allowing tick slack
        for g in rots:
            self.assertGreaterEqual(g, 600000 - 1000, f"RPA rotated too fast: {g} ms")
            self.assertLessEqual(g, 1200000 + 1000, f"RPA rotated too slow: {g} ms")

    def test_behaviour_preserved_across_rotation(self):
        segs = self.segments(sim(13, n=24, ticks=8000, tick_ms=1000))
        rotated = [s for s in segs if any(e[5] == "rotate" for e in s)]
        self.assertTrue(rotated, "no rotations to check continuity on")
        for s in rotated:
            companies = set(e[6] for e in s); itvls = set(e[7] for e in s)
            self.assertEqual(len(companies), 1, "company changed across a rotation")
            self.assertEqual(len(itvls), 1, "interval changed across a rotation")
            addrs = [e[2] for e in s]
            self.assertEqual(len(addrs), len(set(addrs)), "a rotation reused an address")

    def test_rotation_phase_independent(self):
        rows = sim(14, n=24, ticks=8000, tick_ms=1000)
        per_tick_rot = Counter(e[0] for e in rows if e[5] == "rotate")
        self.assertTrue(per_tick_rot, "no rotations observed")
        # no single instant rotates a large share of the population (no synchronized volley)
        self.assertLess(max(per_tick_rot.values()), 24 // 2, "synchronized rotation volley")

@unittest.skipUnless(os.path.exists(EXE), "synth_dump not built")
class Lifecycle(unittest.TestCase):
    def slot_births(self, rows):
        by_slot = defaultdict(list)
        for r in sorted(rows, key=lambda r: (r[1], r[0])):
            if r[5] == "born": by_slot[r[1]].append(r)
        return by_slot

    def test_population_turns_over(self):
        rows = sim(21, n=16, ticks=8000, tick_ms=1000)     # ~133 min simulated
        births = self.slot_births(rows)
        total_births = sum(len(v) for v in births.values())
        self.assertGreater(total_births, 16, "no rebirths — population never turned over")

    def test_lifetime_bounded_by_role(self):
        rows = sim(22, n=24, ticks=12000, tick_ms=1000)
        births = self.slot_births(rows)
        for slot, bs in births.items():
            for a, b in zip(bs, bs[1:]):
                span = b[0] - a[0]                          # birth-to-rebirth ≈ life_ms
                role = a[4]
                cap = 720000 if role == "transient" else 5400000
                self.assertLessEqual(span, cap + 1000, f"{role} slot {slot} outlived its band: {span} ms")

    def test_no_address_resurrection(self):
        rows = sim(23, n=24, ticks=10000, tick_ms=1000)
        seen = set()
        for _, _, addr, *_ in rows:
            # an address may repeat only as consecutive same-slot events (already unique per emit);
            # here every emitted address must be globally unique (fresh 46-bit random each time)
            self.assertNotIn(addr, seen, "an address reappeared after use")
            seen.add(addr)

    def test_rebirth_is_fresh(self):
        rows = sim(24, n=24, ticks=10000, tick_ms=1000)
        births = self.slot_births(rows)
        multi = {s: bs for s, bs in births.items() if len(bs) >= 2}
        self.assertTrue(multi, "no slot was reborn to compare")
        for slot, bs in multi.items():
            addrs = [b[2] for b in bs]
            self.assertEqual(len(addrs), len(set(addrs)), f"slot {slot} reused an address on rebirth")
