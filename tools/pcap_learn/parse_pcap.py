#!/usr/bin/env python3
"""Extract legacy BLE advertising records from a DLT-256
(LINKTYPE_BLUETOOTH_LE_LL_WITH_PHDR) pcap. Emits one NDJSON object per advert.

Fields (superset used by both tools; keys are stable):
  ts      float seconds (capture timestamp)
  company int manufacturer company id (0 if none)      -- learn + scan
  svc     int 16-bit service-data UUID (0 if none)     -- scan
  atype   "public"|"static"|"rpa"|"nrpa" address type  -- scan
  addr    advertiser address, human byte order (hex)    -- scan (dwell grouping)
  mfg     manufacturer-specific AD value incl company (hex, "" if none) -- scan
  svcd    service-data-16 AD value incl UUID (hex, "" if none)          -- scan
  ad      full AD payload (hex)                         -- learn

Record layout (per packet): 10-byte pseudo-header, 4-byte Access Address, then
the advertising PDU: header byte0 (low nibble=type, bit6=TxAdd), byte1=len,
payload=AdvA(6)+AD. Advertising AA = 0x8E89BED6.
"""
import sys, struct, json

ADV_AA = 0x8E89BED6
AA_LE = bytes.fromhex("d6be898e")   # advertising access address, little-endian on the wire
AD_PAYLOAD_TYPES = {0: "ADV_IND", 2: "ADV_NONCONN_IND", 6: "ADV_SCAN_IND", 4: "SCAN_RSP"}


def scan_ad(ad: bytes):
    """Return (company, svc_uuid, mfg_value, svcdata_value) from an AD payload.
    mfg_value includes the 2-byte company id; svcdata_value includes the 2-byte UUID."""
    company, svc, mfg, svcd = 0, 0, b"", b""
    i = 0
    while i + 1 < len(ad):
        l = ad[i]
        if l == 0 or i + 1 + l > len(ad):
            break
        t = ad[i + 1]
        v = ad[i + 2:i + 1 + l]
        if t == 0xFF and not mfg:
            mfg = v
            if len(v) >= 2:
                company = v[0] | (v[1] << 8)
        elif t == 0x16 and not svcd:
            svcd = v
            if len(v) >= 2:
                svc = v[0] | (v[1] << 8)
        i += 1 + l
    return company, svc, mfg, svcd


def addr_type(txadd: int, adva: bytes) -> str:
    if txadd == 0:
        return "public"
    return {3: "static", 1: "rpa", 0: "nrpa"}.get(adva[5] >> 6, "static")


def main():
    if len(sys.argv) < 2:
        sys.stderr.write("usage: parse_pcap.py <file.pcap>\n")
        sys.exit(1)
    f = open(sys.argv[1], "rb")
    gh = f.read(24)
    magic, vma, vmi, tz, sig, snap, dlt = struct.unpack("<IHHIIII", gh)
    # DLT-agnostic: locate the advertising AA by scanning each record (handles Nordic DLT157 and
    # DLT256 alike), rather than assuming a fixed pseudo-header length. Matches capture_profile.py.
    # DLT256 (LE LL w/ PHDR) carries a *reference* copy of the advertising AA at record offset 4;
    # the real packet AA is at offset 10. A bare find() locks onto the reference copy and misreads
    # the PHDR's flags byte as the PDU header -> every record decodes as a bogus type. Search past
    # the PHDR for DLT256 (see capture_profile.py's identical fix, 9c3efdb).
    counts, emitted, recs = {}, 0, 0
    while True:
        rh = f.read(16)
        if len(rh) < 16:
            break
        ts_sec, ts_usec, incl, orig = struct.unpack("<IIII", rh)
        data = f.read(incl)
        if len(data) < incl:
            break
        recs += 1
        off = data.find(AA_LE, 8) if dlt == 256 else data.find(AA_LE)
        if off < 0 or off + 6 > len(data):
            continue
        pdu = data[off + 4:]                      # skip the 4-byte AA -> advertising PDU
        if len(pdu) < 8:
            continue
        h0, plen = pdu[0], pdu[1]
        ptype = h0 & 0x0F
        counts[ptype] = counts.get(ptype, 0) + 1
        if ptype not in AD_PAYLOAD_TYPES:
            continue
        pdu = pdu[2:2 + plen]                     # AdvA(6) + AD
        if len(pdu) < 6:
            continue
        adva = pdu[:6]
        ad = pdu[6:]
        if not ad:
            continue
        company, svc, mfg, svcd = scan_ad(ad)
        obj = {
            "ts": round(ts_sec + ts_usec / 1e6, 3),
            "company": company,
            "svc": svc,
            "atype": addr_type((h0 >> 6) & 1, adva),
            "addr": adva[::-1].hex(),
            "mfg": mfg.hex(),
            "svcd": svcd.hex(),
            "ad": ad.hex(),
        }
        sys.stdout.write(json.dumps(obj, separators=(",", ":")) + "\n")
        emitted += 1
    named = {AD_PAYLOAD_TYPES.get(k, f"type{k}"): v for k, v in sorted(counts.items())}
    sys.stderr.write(f"records={recs} adv_pdus={sum(counts.values())} emitted_ad={emitted}\n")
    sys.stderr.write(f"pdu_types={named}\n")


if __name__ == "__main__":
    main()
