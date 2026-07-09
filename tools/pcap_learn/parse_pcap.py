#!/usr/bin/env python3
"""Extract legacy BLE advertising AD payloads from a DLT-256
(LINKTYPE_BLUETOOTH_LE_LL_WITH_PHDR) pcap. Emits one NDJSON object per advert:
{"company": <int>, "ad": "<hex of AD bytes>"}. Stdlib only.

Record layout (per packet): 10-byte pseudo-header, 4-byte Access Address, then
the advertising PDU: header byte0 (low nibble = PDU type), byte1 = payload len,
payload = AdvA(6) + AD structures. Advertising AA = 0x8E89BED6.
"""
import sys, struct, json

ADV_AA = 0x8E89BED6
# PDU types (low nibble of PDU header byte0) that carry an AD payload after AdvA(6):
AD_PAYLOAD_TYPES = {0: "ADV_IND", 2: "ADV_NONCONN_IND", 6: "ADV_SCAN_IND", 4: "SCAN_RSP"}


def company_of(ad: bytes) -> int:
    """First manufacturer-specific (0xFF) AD structure's company id (LE16), else 0."""
    i = 0
    while i + 1 < len(ad):
        l = ad[i]
        if l == 0 or i + 1 + l > len(ad):
            break
        if ad[i + 1] == 0xFF and l >= 3:
            return ad[i + 2] | (ad[i + 3] << 8)
        i += 1 + l
    return 0


def main():
    if len(sys.argv) < 2:
        sys.stderr.write("usage: parse_pcap.py <file.pcap>\n")
        sys.exit(1)
    f = open(sys.argv[1], "rb")
    gh = f.read(24)
    magic, vma, vmi, tz, sig, snap, dlt = struct.unpack("<IHHIIII", gh)
    if dlt != 256:
        sys.stderr.write(f"warning: DLT={dlt}, expected 256 (LE_LL_WITH_PHDR)\n")
    counts, emitted, recs = {}, 0, 0
    while True:
        rh = f.read(16)
        if len(rh) < 16:
            break
        ts, tu, incl, orig = struct.unpack("<IIII", rh)
        data = f.read(incl)
        if len(data) < incl:
            break
        recs += 1
        if len(data) < 16:
            continue
        aa = struct.unpack("<I", data[10:14])[0]
        if aa != ADV_AA:
            continue
        h0, plen = data[14], data[15]
        ptype = h0 & 0x0F
        counts[ptype] = counts.get(ptype, 0) + 1
        if ptype not in AD_PAYLOAD_TYPES:
            continue
        pdu = data[16:16 + plen]                 # AdvA(6) + AD
        if len(pdu) < 6:
            continue
        ad = pdu[6:]
        if not ad:
            continue
        sys.stdout.write(json.dumps({"company": company_of(ad), "ad": ad.hex()},
                                    separators=(",", ":")) + "\n")
        emitted += 1
    named = {AD_PAYLOAD_TYPES.get(k, f"type{k}"): v for k, v in sorted(counts.items())}
    sys.stderr.write(f"records={recs} adv_pdus={sum(counts.values())} emitted_ad={emitted}\n")
    sys.stderr.write(f"pdu_types={named}\n")


if __name__ == "__main__":
    main()
