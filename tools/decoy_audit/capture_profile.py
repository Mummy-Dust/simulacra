#!/usr/bin/env python3
"""Real BLE crowd -> aggregate distribution profile + a model-seed for synth_dump.
DLT256 and Nordic DLT157 aware (scans for the advertising Access Address).
Privacy: emits only distributions; never addresses, names, or AD payloads."""
import sys, struct, json, statistics
from collections import defaultdict, Counter

AA = bytes.fromhex("d6be898e")
ITVL_LO = [0,50,100,200,500,1000,2000]; ITVL_HI = [50,100,200,500,1000,2000,3000]

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

def _atype(msb):
    return {3:"static",1:"rpa"}.get(msb>>6,"public")

def parse_adverts(path):
    out=[]
    with open(path,"rb") as f:
        f.read(24)
        while True:
            rh=f.read(16)
            if len(rh)<16: break
            ts_s,ts_u,incl,_=struct.unpack("<IIII",rh); data=f.read(incl)
            if len(data)<incl: break
            off=data.find(AA)
            if off<0 or off+6>len(data): continue
            pdu=data[off+4:]
            if len(pdu)<8: continue
            h0,plen=pdu[0],pdu[1]
            if (h0&0x0F) not in (0,2,6): continue
            body=pdu[2:2+plen]
            if len(body)<6: continue
            adva=body[:6]; ad=body[6:]
            out.append({"ts":ts_s+ts_u/1e6, "addr":adva[::-1].hex(),
                        "atype":_atype(adva[5]), "company":_company(ad)})
    return out

def build_profile(adverts):
    at=Counter(a["atype"] for a in adverts)
    ven=Counter(a["company"] for a in adverts if a["company"])
    # per-address median interval -> bin
    ts=defaultdict(list)
    for a in adverts: ts[a["addr"]].append(a["ts"])
    ibins=[0]*7
    for addr,t in ts.items():
        t.sort()
        gaps=[(t[i+1]-t[i])*1000 for i in range(len(t)-1) if 5<(t[i+1]-t[i])*1000<60000]
        if gaps: ibins[itvl_bin(statistics.median(gaps))]+=1
    n=len(adverts) or 1
    def norm(counter, keys=None):
        tot=sum(counter.values()) or 1
        return {str(k):counter[k]/tot for k in (keys or counter)}
    isum=sum(ibins) or 1
    return {"n_adverts":len(adverts),"n_addrs":len(ts),
            "atype":{k:at[k]/n for k in ("static","rpa","public")},
            "itvl_bins":[b/isum for b in ibins],
            "vendor":norm(ven)}

def write_model_seed(profile, path):
    # convert normalized vendor shares back into integer counts (scale 1000) + interval bins
    vend=profile["vendor"]; ib=profile["itvl_bins"]
    # spread the global interval histogram across vendors proportionally (coarse but sufficient)
    binc=[int(round(x*1000)) for x in ib]
    with open(path,"w") as f:
        f.write("POP 12\n")
        top=sorted(vend.items(), key=lambda kv:-kv[1])[:24]
        for cid,share in top:
            c=int(round(share*1000))
            f.write("V %04x %d %s\n" % (int(cid), c, " ".join(str(int(share*b)) for b in binc)))
        f.write("OTHER 0 " + " ".join(str(b) for b in binc) + "\n")

def main():
    adv=parse_adverts(sys.argv[1]); prof=build_profile(adv)
    json.dump(prof, open(sys.argv[2],"w"), indent=2)
    if len(sys.argv)>3: write_model_seed(prof, sys.argv[3])
    sys.stderr.write("adverts=%d addrs=%d\n"%(prof["n_adverts"],prof["n_addrs"]))

if __name__ == "__main__":
    main()
