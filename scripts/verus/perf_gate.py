#!/usr/bin/env python3
"""Apply the Tier 2 pass rule to one paired_stats.py --json result.

    scripts/verus/perf_gate.py STATS.json --bounds latency_p50_us=0.02,latency_p99_us=0.05

A bound is a signed fraction: positive for a metric where higher is worse
(latency: fail when the median ratio B/A - 1 exceeds it), negative for a metric
where lower is worse (throughput: fail when the median ratio falls below it).
A metric fails only if all three hold (docs/verus/modification-plan.md §6):
the median paired ratio is past its bound in the bad direction, the two-sided
sign test gives p < 0.05, and the majority of rounds moved in the bad
direction. A shift past the bound in the good direction is "improved". Exits 1
on any failure or if the JSON lacks a bounded metric.
"""
import argparse
import json
import sys


def parse_bounds(text):
    out = {}
    for item in text.split(","):
        item = item.strip()
        if not item:
            continue
        name, value = item.split("=", 1)
        out[name] = float(value)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("stats")
    ap.add_argument("--bounds", required=True)
    ap.add_argument("--alpha", type=float, default=0.05)
    o = ap.parse_args()
    with open(o.stats) as f:
        res = json.load(f)
    bounds = parse_bounds(o.bounds)
    print(f"{res.get('b')} vs {res.get('a')}: {res.get('pairs')} of {res.get('rounds')} rounds")
    print(f"  {'metric':24s} {'median B/A-1':>13s} {'bound':>8s} {'B>A':>4s} {'B<A':>4s} {'sign p':>7s}  verdict")
    failed = False
    for name, bound in bounds.items():
        m = res.get("metrics", {}).get(name)
        if m is None:
            print(f"  {name:24s} {'missing':>13s} {bound:+8.2%}                    FAIL (no data)")
            failed = True
            continue
        med, p, pos, neg = m["median_ratio"], m["sign_p"], m["pos"], m["neg"]
        if bound >= 0:
            past_bad, past_good, bad_majority = med > bound, med < -bound, pos > neg
        else:
            past_bad, past_good, bad_majority = med < bound, med > -bound, neg > pos
        if past_bad and p < o.alpha and bad_majority:
            verdict = "FAIL"
            failed = True
        elif past_bad:
            verdict = "pass (past bound, not significant)"
        elif past_good and p < o.alpha:
            verdict = "improved"
        elif p < o.alpha:
            verdict = "pass (consistent shift within bound)"
        else:
            verdict = "pass"
        print(f"  {name:24s} {med:+13.2%} {bound:+8.2%} {pos:4d} {neg:4d} {p:7.3f}  {verdict}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
