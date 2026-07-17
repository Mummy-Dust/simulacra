#!/usr/bin/env python3
"""Real BLE crowd -> aggregate distribution profile + a model-seed for synth_dump.
DLT256 and Nordic DLT157 aware (scans for the advertising Access Address).
Privacy: emits only distributions; never addresses, names, or AD payloads."""
import sys, struct, json, statistics
from collections import defaultdict, Counter

AA = bytes.fromhex("d6be898e")
ITVL_LO = [0,50,100,200,500,1000,2000]; ITVL_HI = [50,100,200,500,1000,2000,3000]

# RSSI histogram (dBm). Fixed absolute bins; the analyzer centers on the median for
# placement-invariant comparison. -100..-30 in 5 dB steps = 14 bins.
RSSI_LO = -100; RSSI_HI = -30; RSSI_W = 5
RSSI_NBINS = (RSSI_HI - RSSI_LO) // RSSI_W   # 14

def rssi_hist(values):
    """RSSI list (dBm; may contain None) -> {rssi_bins, rssi_median, rssi_stdev, n_rssi}, or None if empty."""
    vals = [v for v in values if v is not None]
    if not vals:
        return None
    bins = [0] * RSSI_NBINS
    for v in vals:
        idx = int((v - RSSI_LO) // RSSI_W)
        idx = 0 if idx < 0 else RSSI_NBINS - 1 if idx >= RSSI_NBINS else idx
        bins[idx] += 1
    s = sum(bins) or 1
    return {"rssi_bins": [b / s for b in bins],
            "rssi_median": statistics.median(vals),
            "rssi_stdev": statistics.pstdev(vals) if len(vals) > 1 else 0.0,
            "n_rssi": len(vals)}

def _rssi_from(linktype, data, aa_off):
    """Per-record RSSI in dBm, or None if unavailable/implausible."""
    r = None
    if linktype == 157 and aa_off >= 7:            # Nordic BLE: -dBm magnitude, 7 bytes before the AA
        r = -data[aa_off - 7]
    elif linktype == 256 and len(data) >= 2:       # LE LL w/ PHDR: signed Signal_Power at offset 1
        b = data[1]; r = b - 256 if b > 127 else b
    if r is None or not (-110 <= r <= -20):        # sanity gate drops zeros/garbage (synthetic fixtures)
        return None
    return r

def itvl_bin(ms):
    for i in range(7):
        if ITVL_LO[i] <= ms < ITVL_HI[i]: return i
    return 6

def _company(ad):
    i=0
    while i+1 < len(ad):
        l=ad[i]
        if l==0 or i+1+l>len(ad): break
        if ad[i+1]==0xFF and l>=3: return ad[i+2] | (ad[i+3]<<8)
        i+=1+l
    return 0

def _ad_types(ad):
    # Ordered AD element type codes (e.g. "01,03,16"). Privacy-safe: element TYPES only,
    # never the payload values, addresses, or names.
    i=0; out=[]
    while i+1 < len(ad):
        l=ad[i]
        if l==0 or i+1+l>len(ad): break
        out.append("%02x"%ad[i+1])
        i+=1+l
    return ",".join(out)

def _atype(msb):
    return {3:"static",1:"rpa"}.get(msb>>6,"public")

def parse_adverts(path):
    out=[]
    with open(path,"rb") as f:
        gh=f.read(24)
        linktype = struct.unpack("<I", gh[20:24])[0] if len(gh) >= 24 else 0
        while True:
            rh=f.read(16)
            if len(rh)<16: break
            ts_s,ts_u,incl,_=struct.unpack("<IIII",rh); data=f.read(incl)
            if len(data)<incl: break
            # DLT256 (LE LL w/ PHDR) carries a *reference* copy of the advertising AA at record
            # offset 4; the real packet AA is at offset 10. Search past the PHDR so we lock onto
            # the packet AA, not the reference. DLT157 (Nordic) has a single AA -> search from 0.
            off = data.find(AA, 8) if linktype == 256 else data.find(AA)
            if off<0 or off+6>len(data): continue
            rssi=_rssi_from(linktype, data, off)
            pdu=data[off+4:]
            if len(pdu)<8: continue
            h0,plen=pdu[0],pdu[1]
            if (h0&0x0F) not in (0,2,6): continue
            body=pdu[2:2+plen]
            if len(body)<6: continue
            adva=body[:6]; ad=body[6:]
            out.append({"ts":ts_s+ts_u/1e6, "addr":adva[::-1].hex(),
                        "atype":_atype(adva[5]), "company":_company(ad),
                        "ad_sig":_ad_types(ad), "rssi":rssi})
    return out

def build_profile(adverts):
    at=Counter(a["atype"] for a in adverts)
    # Per-address aggregation. Co-travel correlation tracks entities, not advert volume,
    # so the vendor histogram is DEVICE-weighted: one chatty beacon must not dominate.
    ts=defaultdict(list); dev_co=defaultdict(Counter); dev_ad=defaultdict(Counter)
    for a in adverts:
        ts[a["addr"]].append(a["ts"])
        dev_co[a["addr"]][a["company"]] += 1
        dev_ad[a["addr"]][a["ad_sig"]] += 1
    # Each device's AD-structure signature = its modal ordered AD-type sequence, device-weighted
    # so one chatty beacon can't dominate the structural histogram.
    ads=Counter()
    for addr,sigs in dev_ad.items():
        ads[max(sigs.items(),key=lambda x:x[1])[0]] += 1
    # Each device's vendor = its modal non-zero mfg company; a device with no stable mfg
    # company (service-data / name-only / RPA) goes to the explicit "none" bucket.
    ven=Counter()
    for addr,cos in dev_co.items():
        nz=[(c,cnt) for c,cnt in cos.items() if c]
        ven[str(max(nz,key=lambda x:x[1])[0]) if nz else "none"] += 1
    # per-address median interval -> bin
    ibins=[0]*7
    for addr,t in ts.items():
        t.sort()
        gaps=[(t[i+1]-t[i])*1000 for i in range(len(t)-1) if 5<(t[i+1]-t[i])*1000<60000]
        if gaps: ibins[itvl_bin(statistics.median(gaps))]+=1
    # Aggregate per-address presence-duration histogram (privacy-safe: bucket counts only).
    # Buckets match PRESENCE_BINS in analyzers/rotation_audit.py: <1,5,15,30,60,120,>120 min.
    PRESENCE_BINS=[0,60000,300000,900000,1800000,3600000,7200000,10**12]
    pbins=[0]*(len(PRESENCE_BINS)-1)
    for _addr,_t in ts.items():
        span=(max(_t)-min(_t))*1000            # seconds -> ms
        for _k in range(len(pbins)):
            if PRESENCE_BINS[_k]<=span<PRESENCE_BINS[_k+1]: pbins[_k]+=1; break
    if not ts:
        sys.stderr.write("capture_profile: no addresses with timestamps; presence_ms_bins all zero\n")
    n=len(adverts) or 1
    isum=sum(ibins) or 1
    vtot=sum(ven.values()) or 1
    adtot=sum(ads.values()) or 1
    prof = {"n_adverts":len(adverts),"n_addrs":len(ts),
            "atype":{k:at[k]/n for k in ("static","rpa","public")},
            "itvl_bins":[b/isum for b in ibins],
            "vendor":{k:v/vtot for k,v in ven.items()},
            "ad_sig":{k:v/adtot for k,v in ads.items()},
            "presence_ms_bins":pbins}
    rh = rssi_hist([a.get("rssi") for a in adverts])
    if rh:
        prof.update(rh)          # rssi_bins / rssi_median / rssi_stdev / n_rssi (omitted if no RSSI)
    return prof

def write_model_seed(profile, path):
    # convert normalized vendor shares back into integer counts (scale 1000) + interval bins
    vend=profile["vendor"]; ib=profile["itvl_bins"]
    # spread the global interval histogram across buckets proportionally (coarse but sufficient)
    binc=[int(round(x*1000)) for x in ib]
    none_share=vend.get("none",0.0)
    real=sorted(((c,s) for c,s in vend.items() if c!="none"), key=lambda kv:-kv[1])[:24]
    with open(path,"w") as f:
        f.write("POP 12\n")
        for cid,share in real:
            c=int(round(share*1000))
            f.write("V %04x %d %s\n" % (int(cid), c, " ".join(str(int(share*b)) for b in binc)))
        # the "none" (no-mfg / service-data) device share drives the model's OTHER bucket, which
        # the generator turns into service-data/beacon decoys (generate.c build_for_vendor).
        oc=int(round(none_share*1000))
        f.write("OTHER %d %s\n" % (oc, " ".join(str(int(none_share*b)) for b in binc)))

def main():
    adv=parse_adverts(sys.argv[1]); prof=build_profile(adv)
    json.dump(prof, open(sys.argv[2],"w"), indent=2)
    if len(sys.argv)>3: write_model_seed(prof, sys.argv[3])
    print("adverts=%d addrs=%d"%(prof["n_adverts"],prof["n_addrs"]))

if __name__ == "__main__":
    main()
