import os, subprocess, unittest

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

    def test_persona_ble_is_realistic_phone_shape(self):
        ble, _ = personas(4)
        self.assertTrue(ble, "no persona BLE events")
        # A persona presents a terse PHONE shape: no manufacturer data at all, so the on-air
        # company id is always 0 (never earbuds vendor mfg, never Apple Continuity 0x004C).
        for _t, _i, _addr, atype, comp, _g, _itvl in ble:
            self.assertEqual(atype, "rpa", "persona BLE member is not RPA")
            self.assertEqual(comp, 0, f"persona emitted manufacturer data (company {comp:#06x})")
        # Widened phone interval band -> personas spread across intervals instead of clustering on
        # the single 120-180 ms accessory band (the measured interval monoculture tell).
        itvls = {itvl for _t, _i, _a, _at, _c, _g, itvl in ble}
        self.assertGreater(len(itvls), 1, "persona intervals collapsed to one value (monoculture)")

    def test_all_addresses_unique_across_both_radios(self):
        ble, wifi = personas(5)
        addrs = [a for _t, _i, a, _at, _c, _g, _itvl in ble] + [m for _t, _i, m, _a, _g in wifi]
        self.assertEqual(len(addrs), len(set(addrs)), "an address collided across the fleet")


if __name__ == "__main__":
    unittest.main()
