#!/usr/bin/env python3
"""Aggregate discriminators into a ranked detectability scorecard. The headline
score is the MAX separability (an adversary uses their single best tell), so the
worst tell defines exposure. Exit non-zero if headline exceeds a regression gate."""
import argparse, json, sys
import discriminators as D

def build_scorecard(synth, profile):
    ds=sorted(D.score_all(synth, profile), key=lambda d:-d["separability"])
    headline=ds[0]["separability"] if ds else 0.0
    return {"discriminators":ds,"headline":headline,
            "headline_tell":ds[0]["name"] if ds else None}

def _load_ndjson(path):
    return [json.loads(l) for l in open(path) if l.strip()]

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("synth"); ap.add_argument("profile")
    ap.add_argument("--json"); ap.add_argument("--gate", type=float, default=1.1)
    a=ap.parse_args()
    synth=_load_ndjson(a.synth); profile=json.load(open(a.profile))
    card=build_scorecard(synth, profile)
    print("%-24s %12s %s" % ("DISCRIMINATOR","SEPARABILITY","VISIBILITY"))
    for d in card["discriminators"]:
        print("%-24s %12.4f %s" % (d["name"], d["separability"], d["visibility"]))
    print("-"*50)
    print("HEADLINE (max) %.4f  worst tell: %s" % (card["headline"], card["headline_tell"]))
    if a.json: json.dump(card, open(a.json,"w"), indent=2)
    sys.exit(1 if card["headline"] > a.gate else 0)

if __name__=="__main__":
    main()
