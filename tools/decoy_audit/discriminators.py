#!/usr/bin/env python3
"""Separability discriminators: how distinguishable is the synthetic decoy
population from the real crowd, per feature. Each returns a score in [0,1]
(0 = indistinguishable, 1 = trivially separable) via Jensen-Shannon divergence."""
import math
from collections import Counter

ITVL_LO=[0,50,100,200,500,1000,2000]; ITVL_HI=[50,100,200,500,1000,2000,3000]
def _itvl_bin(ms):
    for i in range(7):
        if ITVL_LO[i] <= ms < ITVL_HI[i]: return i
    return 6

# Per-address presence-duration bins (ms), matching capture_profile.presence_ms_bins:
# <1, 5, 15, 30, 60, 120, >120 min. The passively-observable projection of rotation + lifetime.
PRESENCE_BINS=[0,60000,300000,900000,1800000,3600000,7200000,10**12]
def presence_bins_from_devices(text):
    # Per-ADDRESS presence from a `synth_dump --devices` dump: the gap until the SAME slot's next
    # address replaces it (last address in a slot -> to sim end). This is what a passive sniffer
    # sees; NOT the born->last-rotate span (which scores a static non-rotating address as 0).
    from collections import defaultdict as _dd
    rows=[]
    for ln in text.splitlines():
        p=ln.split()
        if len(p)==9 and p[0]=="D": rows.append((int(p[1]), int(p[2])))   # (t, slot)
    bins=[0]*(len(PRESENCE_BINS)-1)
    if not rows: return bins
    tmax=max(r[0] for r in rows); slot=_dd(list)
    for t,s in rows: slot[s].append(t)
    for ts in slot.values():
        ts.sort()
        for i in range(len(ts)):
            d=(ts[i+1] if i+1<len(ts) else tmax)-ts[i]
            for k in range(len(PRESENCE_BINS)-1):
                if PRESENCE_BINS[k]<=d<PRESENCE_BINS[k+1]: bins[k]+=1; break
    return bins
def d_presence(pbins, profile):
    real=profile.get("presence_ms_bins")
    if not real: return 0.0            # older profile without presence data -> no evidence
    return js_divergence(pbins, real)

def _norm(v):
    s=sum(v)
    return [x/s for x in v] if s else [0.0]*len(v)

def js_divergence(p, q):
    p=_norm(list(p)); q=_norm(list(q))
    m=[(pi+qi)/2 for pi,qi in zip(p,q)]
    def kl(a,b):
        s=0.0
        for ai,bi in zip(a,b):
            if ai>0 and bi>0: s+=ai*math.log2(ai/bi)
        return s
    jsd=0.5*kl(p,m)+0.5*kl(q,m)
    return max(0.0, min(1.0, jsd))

def synth_distributions(synth):
    at=Counter(x["atype"] for x in synth)
    n=len(synth) or 1
    ib=[0]*7
    for x in synth: ib[_itvl_bin(x["itvl_ms"])]+=1
    # Bucket decoys with no stable mfg company (service-data templates carry 0xFFFF =
    # RF_VENDOR_UNKNOWN) into the same "none" category the real profile uses, so the two
    # histograms are compared like-for-like.
    ven=Counter()
    for x in synth:
        c=x.get("company")
        ven["none" if (not c or c==0xFFFF) else str(c)] += 1
    vt=sum(ven.values()) or 1
    # AD-structure signature: the ordered AD element type codes each decoy advertises
    # (e.g. "01,03,16"). One row = one device, so this is already device-weighted.
    ad=Counter(x.get("ad","") for x in synth)
    adt=sum(ad.values()) or 1
    return {"atype":[at.get(k,0)/n for k in ("static","rpa","public")],
            "itvl_bins":_norm(ib),
            "vendor":{k:v/vt for k,v in ven.items()},
            "ad_sig":{k:v/adt for k,v in ad.items()}}

def _hist_vectors(sv, pv):
    keys=sorted(set(sv)|set(pv))
    return [sv.get(k,0) for k in keys],[pv.get(k,0) for k in keys]

def d_address_type(sd, prof):
    p=sd["atype"]; q=[prof["atype"].get(k,0) for k in ("static","rpa","public")]
    return js_divergence(p,q)
def d_interval(sd, prof):
    return js_divergence(sd["itvl_bins"], prof["itvl_bins"])
def d_vendor(sd, prof):
    s,p=_hist_vectors(sd["vendor"], prof["vendor"]); return js_divergence(s,p)
def d_ad_structure(sd, prof):
    # No real AD-structure data in the profile (older profile.json) -> no evidence, score 0.
    pa=prof.get("ad_sig")
    if not pa: return 0.0
    s,p=_hist_vectors(sd["ad_sig"], pa); return js_divergence(s,p)

DISCRIMINATORS=[("address_type_mix",d_address_type),
                ("interval_distribution",d_interval),
                ("vendor_histogram",d_vendor),
                ("ad_structure",d_ad_structure)]

def score_all(synth, profile):
    sd=synth_distributions(synth)
    return [{"name":n,"separability":round(fn(sd,profile),4),"visibility":"logic"}
            for n,fn in DISCRIMINATORS]
