#!/usr/bin/env python3
"""Tier 3 Jetpack gate: candidate arm against baseline arm, failing on a regression.

    scripts/verus/jetpack_gate.py ROOT --baseline ARM --candidate ARM

ROOT is either one setting directory (holding round<k>/ and summary.json) or
the directory above several (ROOT/<setting>/summary.json), as
scripts/raft_perf/jetpack/run_jetpack_sweep.sh and jetpack_compare.py lay them
out. It reads summary.json, which jetpack_compare.py writes from the same
per-round files with Jetpack's own aggregate(); run that first.

Rules (docs/verus/modification-plan.md §6, Tier 3), on the median over rounds
of the per-round paired ratio candidate/baseline - 1:
  throughput  fail below -2% at any N
  p50         fail above +2% at any N
  p90         fail above +5% at any N
  p99         fail above +5% at N > 10; reported but exempt at N <= 10
  knee        fail if the candidate's median knee N is below the baseline's
With 3-5 rounds a sign test cannot reach p < 0.05, so unlike Tier 2 there is
no significance condition. Prints one row per setting and N; exits 1 on any
violation, 2 if no summary is found.
"""
import argparse
import glob
import json
import os
import statistics
import sys

BOUNDS = {"tput": -0.02, "p50": 0.02, "p90": 0.05, "p99": 0.05}
P99_EXEMPT_MAX_N = 10


def summaries(root):
    own = os.path.join(root, "summary.json")
    if os.path.exists(own):
        return [own]
    return sorted(glob.glob(os.path.join(root, "*", "summary.json")))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("root")
    ap.add_argument("--baseline", required=True)
    ap.add_argument("--candidate", required=True)
    o = ap.parse_args()
    files = summaries(o.root)
    if not files:
        print(f"jetpack_gate: no summary.json under {o.root}; run jetpack_compare.py first", file=sys.stderr)
        return 2
    failed = False
    for path in files:
        with open(path) as f:
            s = json.load(f)
        setting = s.get("setting", os.path.basename(os.path.dirname(path)))
        rows = {(p["arm"], p["workload"], p["n"]): {r["round"]: r for r in p["rounds"]} for p in s["points"]}
        points = sorted({(w, n) for (_a, w, n) in rows}, key=lambda x: (x[0] != "rw_1000000", x[0], x[1]))
        print(f"## {setting} ({s.get('delay_label', '')}), {s.get('rounds')} rounds: "
              f"{o.candidate} vs {o.baseline}")
        print(f"  {'workload':14s} {'N':>4s} {'tput':>8s} {'p50':>8s} {'p90':>8s} {'p99':>8s}  verdict")
        for wl, n in points:
            base = rows.get((o.baseline, wl, n), {})
            cand = rows.get((o.candidate, wl, n), {})
            common = sorted(set(base) & set(cand))
            if not common:
                print(f"  {wl:14s} {n:4d}  missing data  FAIL")
                failed = True
                continue
            cells, bad = [], []
            for k, bound in BOUNDS.items():
                ratios = [cand[r][k] / base[r][k] - 1 for r in common if base[r].get(k)]
                if not ratios:
                    cells.append(f"{'n/a':>8s}")
                    continue
                med = statistics.median(ratios)
                cells.append(f"{med:+8.2%}")
                if k == "p99" and n <= P99_EXEMPT_MAX_N:
                    continue
                if (bound < 0 and med < bound) or (bound > 0 and med > bound):
                    bad.append(k)
            verdict = "FAIL " + ",".join(bad) if bad else "pass"
            failed |= bool(bad)
            print(f"  {wl:14s} {n:4d} " + " ".join(cells) + f"  {verdict}")
        knees = s.get("knees", {})
        for wl in sorted({w for w, _ in points}):
            kb, kc = knees.get(f"{o.baseline}/{wl}"), knees.get(f"{o.candidate}/{wl}")
            if not kb or not kc:
                print(f"  knee {wl}: not derivable (baseline {kb}, candidate {kc})")
                continue
            mb, mc = statistics.median(kb), statistics.median(kc)
            ok = mc >= mb
            failed |= not ok
            print(f"  knee {wl}: baseline {mb:g} {kb}, candidate {mc:g} {kc}  {'pass' if ok else 'FAIL'}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
