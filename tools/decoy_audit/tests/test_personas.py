import os, subprocess, unittest
from collections import defaultdict

HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
EXE  = os.path.join(TOOL, "synth_dump.exe" if os.name == "nt" else "synth_dump")

APPLE = 0x004C


def personas(seed, n=12, ndev=24, ticks=4000, tick_ms=1000):
    out = subprocess.check_output(
        [EXE, "--personas", str(seed), str(n), str(ndev), str(ticks), str(tick_ms)], text=True)
    ble, wifi = [], []
    for ln in out.splitlines():
        p = ln.split()
        if p and p[0] == "B":     # B <t> <persona_idx> <addr> <atype> <company> <gen> <itvl_ms>
            ble.append((int(p[1]), int(p[2]), p[3], p[4], int(p[5], 16), int(p[6]), int(p[7])))
        elif p and p[0] == "W":   # W <t> <persona_idx> <mac> <arch> <gen>
            wifi.append((int(p[1]), int(p[2]), p[3], int(p[4]), int(p[5])))
    return ble, wifi


@unittest.skipUnless(os.path.exists(EXE), "synth_dump not built")
class Personas(unittest.TestCase):
    def test_every_wifi_has_a_co_present_ble_twin(self):
        ble, wifi = personas(1)
        self.assertTrue(wifi and ble, "no persona events")
        ble_keys = {(i, g) for _t, i, _a, _at, _c, g, _itvl in ble}
        wifi_keys = {(i, g) for _t, i, _m, _a, g in wifi}
        # dual-radio coverage: every Wi-Fi (persona,generation) has a BLE twin
        missing = wifi_keys - ble_keys
        self.assertFalse(missing, f"Wi-Fi identities with no BLE twin: {len(missing)}")

    def test_twins_co_appear_same_tick(self):
        ble, wifi = personas(2)
        ble_born = {(i, g): t for t, i, _a, _at, _c, g, _itvl in ble}
        wifi_born = {(i, g): t for t, i, _m, _a, g in wifi}
        for key in wifi_born.keys() & ble_born.keys():
            self.assertEqual(ble_born[key], wifi_born[key], f"twin {key} born on different ticks")

    def test_ble_members_are_rpa_and_law3_safe(self):
        ble, _ = personas(3)
        for _t, _i, _addr, atype, comp, _g, _itvl in ble:
            self.assertEqual(atype, "rpa", "persona BLE member is not RPA")
            self.assertNotEqual(comp, APPLE, "persona BLE member emitted Apple mfg data (Law-3)")

    def test_samsung_google_families_are_vendor_matched(self):
        ble, wifi = personas(4)
        # arch 1=GALAXY expects company 0x0075; arch 2=PIXEL expects 0x00E0 (when present in roster)
        arch_by_key = {(i, g): a for _t, i, _m, a, g in wifi}
        seen_match = False
        for _t, i, _addr, _atype, comp, g, _itvl in ble:
            a = arch_by_key.get((i, g))
            if a == 1 and comp == 0x0075: seen_match = True
            if a == 2 and comp == 0x00E0: seen_match = True
            # a matched Galaxy/Pixel persona must not carry the *other* vendor's id
            if a == 1: self.assertNotEqual(comp, 0x00E0, "Galaxy persona carried Google id")
        self.assertTrue(seen_match, "no vendor-matched Samsung/Google persona observed")

    def test_all_addresses_unique_across_both_radios(self):
        ble, wifi = personas(5)
        addrs = [a for _t, _i, a, _at, _c, _g, _itvl in ble] + [m for _t, _i, m, _a, _g in wifi]
        self.assertEqual(len(addrs), len(set(addrs)), "an address collided across the fleet")

    def test_same_vendor_personas_get_diverse_payloads(self):
        ble, _ = personas(1)
        itvls = defaultdict(set)
        for _t, _i, _a, _at, comp, _g, itvl in ble:
            itvls[comp].add(itvl)
        # roster_pick_company picks a RANDOM same-vendor roster row (not always the first), and
        # template payloads/intervals are randomized -> a busy vendor's personas must NOT all share
        # one interval. With always-first cloning every vendor would collapse to a single interval.
        self.assertTrue(any(len(v) > 1 for v in itvls.values()),
                        f"no vendor shows payload diversity (byte-identical clones): "
                        f"{ {k: len(v) for k, v in itvls.items()} }")


if __name__ == "__main__":
    unittest.main()
