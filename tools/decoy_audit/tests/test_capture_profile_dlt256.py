import os, struct, sys, tempfile, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.dirname(HERE)
sys.path.insert(0, TOOL)
import capture_profile as cp  # noqa: E402

AA = bytes.fromhex("d6be898e")


def _dlt256_realistic_record(adva6, ad, sig_dbm=-55):
    """A DLT256 (LE LL w/ PHDR) record shaped like a real capture:
    PHDR = RF-chan, signal-power(signed), noise, AA-offenses, REFERENCE-AA(4), flags(2);
    then the real packet AA(4) and the ADV_NONCONN_IND PDU. The reference AA at offset 4
    is what makes a naive find(AA) misfire."""
    phdr = bytearray(10)
    phdr[0] = 0x25                       # RF channel
    phdr[1] = sig_dbm & 0xFF             # signal power (signed) -> RSSI
    phdr[2] = 0x80                       # noise -128
    phdr[3] = 0x00                       # AA offenses
    phdr[4:8] = AA                       # REFERENCE Access Address (the trap)
    phdr[8:10] = b"\x13\x0c"             # flags
    pdu = bytes([0x02, 6 + len(ad)]) + adva6 + ad   # ADV_NONCONN_IND
    return bytes(phdr) + AA + pdu


def _write_pcap(path, records, linktype):
    with open(path, "wb") as f:
        f.write(struct.pack("<IHHIIII", 0xA1B2C3D4, 2, 4, 0, 0, 65535, linktype))
        for r in records:
            f.write(struct.pack("<IIII", 0, 0, len(r), len(r)))
            f.write(r)


class DLT256(unittest.TestCase):
    def setUp(self):
        self.path = tempfile.NamedTemporaryFile(suffix=".pcap", delete=False).name
        advas = [(bytes([1, 2, 3, 4, 5, 0xC0]), 0x0075),
                 (bytes([9, 9, 9, 9, 9, 0xD0]), 0x0087),
                 (bytes([1, 2, 3, 4, 5, 0x40]), 0x004C)]   # C0/D0 static, 40 RPA
        mfg = lambda c: bytes([0x02, 0x01, 0x06, 0x04, 0xFF, c & 0xFF, c >> 8, 0x11])
        _write_pcap(self.path, [_dlt256_realistic_record(a, mfg(c)) for a, c in advas], 256)

    def tearDown(self):
        os.remove(self.path)

    def test_realistic_dlt256_parses_past_reference_aa(self):
        adverts = cp.parse_adverts(self.path)
        self.assertEqual(len(adverts), 3, "reference AA in PHDR must not swallow the record")

    def test_realistic_dlt256_populates_rssi(self):
        prof = cp.build_profile(cp.parse_adverts(self.path))
        self.assertIn("rssi_bins", prof)
        self.assertGreater(prof["n_rssi"], 0)
        self.assertTrue(-110 <= prof["rssi_median"] <= -20)


_NEWLONG = os.path.join(TOOL, "..", "..", "private", "newlong.pcap")


@unittest.skipUnless(os.path.exists(_NEWLONG), "private/newlong.pcap not present")
class NewlongRealCapture(unittest.TestCase):
    def test_parses_with_rssi(self):
        prof = cp.build_profile(cp.parse_adverts(_NEWLONG))
        self.assertGreater(prof["n_adverts"], 0)
        self.assertGreater(prof["n_rssi"], 0)
        self.assertTrue(5.0 <= prof["rssi_stdev"] <= 20.0,
                        f"real-crowd RSSI stdev out of sane band: {prof['rssi_stdev']}")


if __name__ == "__main__":
    unittest.main()
