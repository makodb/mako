#!/usr/bin/env python3
"""Paired statistics of arm B against arm A over a rotation_trial.sh run.

    scripts/raft_perf/paired_stats.py OUT_DIR ROUNDS A B [--min-pairs N] [--json]

For each metric: the median of the per-round ratio B/A - 1 and a two-sided
sign test over the rounds, as paired_trial.sh prints them. A round counts
only when both arms produced a record. Exits 1 if fewer than --min-pairs
rounds (default 23 of 25, scaled) are complete. With --json, prints one JSON
object instead, for a summary script to read.
"""
import argparse
import json
import math
import os
import statistics
import sys

METRICS = (("applied_per_sec", "higher"), ("latency_p50_us", "lower"),
           ("latency_p99_us", "lower"), ("latency_p999_us", "lower"),
           ("latency_max_us", "lower"), ("max_apply_gap_us", "lower"),
           ("snapshot_create_us_p50", "lower"), ("install_rpcs_sent", "lower"),
           ("install_bytes_sent", "lower"), ("rss_peak_kb", "lower"),
           ("stalled_snapshot_install_us_p50", "lower"), ("stalled_catchup_ms", "lower"),
           ("stalled_install_rpcs_received", "lower"), ("stalled_rss_peak_kb", "lower"))


def value(r, key):
    # "stalled_x": the stalled follower's merged side-record field.
    if key.startswith("stalled_"):
        v = r.get("stalled_follower")
        return r.get(f"{v}_{key[len('stalled_'):]}") if v else None
    return r.get(key)


def sign_p(pos, neg):
    n = pos + neg
    if n == 0:
        return 1.0
    k = min(pos, neg)
    return min(1.0, 2 * sum(math.comb(n, j) for j in range(k + 1)) / 2 ** n)


def load(out, i, arm):
    try:
        with open(os.path.join(out, f"r{i}.{arm}.json")) as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("rounds", type=int)
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--min-pairs", type=int)
    ap.add_argument("--json", action="store_true")
    o = ap.parse_args()
    min_pairs = o.min_pairs if o.min_pairs is not None else o.rounds * 23 // 25
    rows = []
    for i in range(1, o.rounds + 1):
        ra, rb = load(o.out, i, o.a), load(o.out, i, o.b)
        if ra and rb:
            rows.append((ra, rb))
    res = {"a": o.a, "b": o.b, "pairs": len(rows), "rounds": o.rounds, "metrics": {}}
    for key, better in METRICS:
        pairs = [(value(ra, key), value(rb, key)) for ra, rb in rows]
        pairs = [(x, y) for x, y in pairs if isinstance(x, (int, float)) and isinstance(y, (int, float))]
        ratios = [y / x - 1 for x, y in pairs if x > 0]
        if not ratios:
            continue
        pos = sum(1 for r in ratios if r > 0)
        neg = sum(1 for r in ratios if r < 0)
        res["metrics"][key] = {
            "median_ratio": statistics.median(ratios), "pos": pos, "neg": neg,
            "sign_p": sign_p(pos, neg), "better": better, "n": len(ratios),
            "median_a": statistics.median(x for x, _ in pairs),
            "median_b": statistics.median(y for _, y in pairs)}
    if o.json:
        print(json.dumps(res))
    else:
        print(f"{len(rows)} complete rounds of {o.rounds}: B={o.b} against A={o.a}")
        for key, m in res["metrics"].items():
            print(f"  {key:32s} median B/A-1 = {m['median_ratio']:+.2%}  "
                  f"(A {m['median_a']:.6g}, B {m['median_b']:.6g}; B>A in {m['pos']}, "
                  f"B<A in {m['neg']}; sign test p = {m['sign_p']:.3f}; {m['better']} is better)")
    if len(rows) < min_pairs:
        print(f"paired_stats: FAILED -- only {len(rows)} of {o.rounds} rounds complete "
              f"(need {min_pairs})", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
