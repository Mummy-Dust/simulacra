#!/usr/bin/env python3
"""Aggregate discriminators into a ranked detectability scorecard. The headline
score is the MAX separability (an adversary uses their single best tell), so the
worst tell defines exposure. Exit non-zero if headline exceeds a regression gate."""
import argparse, json, sys
import discriminators as D

def build_scorecard(synth, profile, devices_text=None):
    ds=D.score_all(synth, profile)
    # The presence-duration tell needs a temporal `synth_dump --devices` run (per-address lifespan),
    # not the static roster; include it when that run is supplied.
    if devices_text is not None:
        pb=D.presence_bins_from_devices(devices_text)
        ds.append({"name":"presence_duration","separability":round(D.d_presence(pb,profile),4),"visibility":"logic"})
    ds=sorted(ds, key=lambda d:-d["separability"])
    headline=ds[0]["separability"] if ds else 0.0
    return {"discriminators":ds,"headline":headline,
            "headline_tell":ds[0]["name"] if ds else None}

def _load_ndjson(path):
    # utf-8-sig tolerates a BOM (e.g. if the file was written by a shell that adds one)
    with open(path, encoding="utf-8-sig") as f:
        return [json.loads(l) for l in f if l.strip()]

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("synth"); ap.add_argument("profile")
    ap.add_argument("--devices", help="a `synth_dump --devices` dump -> adds the presence-duration tell")
    ap.add_argument("--json"); ap.add_argument("--gate", type=float, default=1.1)
    ap.add_argument("--atype-detail", action="store_true",
                    help="print decoy vs real static/rpa/public fractions under the scorecard")
    a=ap.parse_args()
    synth=_load_ndjson(a.synth); profile=json.load(open(a.profile))
    devices_text=open(a.devices, encoding="utf-8-sig").read() if a.devices else None
    card=build_scorecard(synth, profile, devices_text)
    print("%-24s %12s %s" % ("DISCRIMINATOR","SEPARABILITY","VISIBILITY"))
    for d in card["discriminators"]:
        print("%-24s %12.4f %s" % (d["name"], d["separability"], d["visibility"]))
    if a.atype_detail:
        dec = D.synth_distributions(synth)["atype"]                       # [static, rpa, public]
        real = [profile["atype"].get(k, 0) for k in ("static", "rpa", "public")]
        print("atype detail   decoy static/rpa/public = %.2f/%.2f/%.2f   real = %.2f/%.2f/%.2f"
              % (dec[0], dec[1], dec[2], real[0], real[1], real[2]))
    print("-"*50)
    print("HEADLINE (max) %.4f  worst tell: %s" % (card["headline"], card["headline_tell"]))
    if a.json: json.dump(card, open(a.json,"w"), indent=2)
    sys.exit(1 if card["headline"] > a.gate else 0)

if __name__=="__main__":
    main()
