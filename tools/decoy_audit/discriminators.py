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
    ven=Counter(x["company"] for x in synth if x.get("company"))
    vt=sum(ven.values()) or 1
    return {"atype":[at.get(k,0)/n for k in ("static","rpa","public")],
            "itvl_bins":_norm(ib),
            "vendor":{str(k):v/vt for k,v in ven.items()}}

def _vendor_vectors(sv, pv):
    keys=sorted(set(sv)|set(pv))
    return [sv.get(k,0) for k in keys],[pv.get(k,0) for k in keys]

def d_address_type(sd, prof):
    p=sd["atype"]; q=[prof["atype"].get(k,0) for k in ("static","rpa","public")]
    return js_divergence(p,q)
def d_interval(sd, prof):
    return js_divergence(sd["itvl_bins"], prof["itvl_bins"])
def d_vendor(sd, prof):
    s,p=_vendor_vectors(sd["vendor"], prof["vendor"]); return js_divergence(s,p)

DISCRIMINATORS=[("address_type_mix",d_address_type),
                ("interval_distribution",d_interval),
                ("vendor_histogram",d_vendor)]

def score_all(synth, profile):
    sd=synth_distributions(synth)
    return [{"name":n,"separability":round(fn(sd,profile),4),"visibility":"logic"}
            for n,fn in DISCRIMINATORS]
