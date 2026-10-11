#!/usr/bin/env python3
"""G7: election and failover time, parent arm A against child arm B.

    scripts/verus/election_times.py DIR A B [--params docs/verus/gate-params.md] [--aa]

DIR holds <i>.<arm>.json records written by examples/raft_bench.sh
--kill-leader-at-sec (one per leader kill). Each carries three wall-clock
(CLOCK_REALTIME, same host) microsecond stamps:

  leader_loss_us   the launcher's clock just before it SIGKILLs the leader
  new_leader_us    a survivor's leadership callback reporting "became leader"
  first_commit_us  that survivor applying the first entry it proposed as leader

Two durations per kill: new_leader = new_leader_us - leader_loss_us and
first_commit = first_commit_us - leader_loss_us. A record missing a field, or
a missing record, is a failed kill; more than 2 failed kills in an arm fails
the gate.

Gate (docs/verus/modification-plan.md §6, "G7 rule"): for each duration and
statistic (median, p90), fail if B's statistic exceeds A's by more than the
bound in the "| G7 |" row of the params file AND a one-sided Mann-Whitney U
test (B slower) gives p < 0.05. Exits 1 on failure.

With --aa (the Phase 0 A/A run) it instead sets each bound to
max(10%, |A - B| / A) and rewrites the "| G7 |" row.
"""
import argparse
import glob
import json
import math
import os
import re
import statistics
import sys

DURATIONS = ("new_leader", "first_commit")
STATS = ("median", "p90")
MAX_FAILED = 2
FLOOR = 0.10


def load(dirname, arm):
    durs = {d: [] for d in DURATIONS}
    failed, total = 0, 0
    # A kill is any index with a record or a log: a run that died before
    # writing its record still left <i>.<arm>.log behind.
    pat = re.compile(r"^(\d+)\." + re.escape(arm) + r"\.(?:json|log)$")
    kills = set()
    for p in glob.glob(os.path.join(dirname, f"*.{arm}.*")):
        m = pat.match(os.path.basename(p))
        if m:
            kills.add(int(m.group(1)))
    for i in sorted(kills):
        total += 1
        try:
            with open(os.path.join(dirname, f"{i}.{arm}.json")) as f:
                r = json.load(f)
            loss = r["leader_loss_us"]
            vals = {"new_leader": r["new_leader_us"] - loss, "first_commit": r["first_commit_us"] - loss}
        except (OSError, ValueError, KeyError, TypeError):
            failed += 1
            continue
        if any(v < 0 for v in vals.values()):
            failed += 1
            continue
        for d in DURATIONS:
            durs[d].append(vals[d])
    return durs, failed, total


def stat(xs, which):
    if which == "median":
        return statistics.median(xs)
    return statistics.quantiles(xs, n=10, method="inclusive")[8]


def mann_whitney_greater(a, b):
    """One-sided p that b is stochastically larger than a (normal approximation,
    tie and continuity corrected)."""
    na, nb = len(a), len(b)
    allv = sorted([(x, 0) for x in a] + [(y, 1) for y in b])
    ranks = [0.0] * len(allv)
    ties = 0.0
    i = 0
    while i < len(allv):
        j = i
        while j + 1 < len(allv) and allv[j + 1][0] == allv[i][0]:
            j += 1
        r = (i + j) / 2 + 1
        for k in range(i, j + 1):
            ranks[k] = r
        t = j - i + 1
        ties += t ** 3 - t
        i = j + 1
    rb = sum(r for r, (_, g) in zip(ranks, allv) if g == 1)
    ub = rb - nb * (nb + 1) / 2
    n = na + nb
    mean = na * nb / 2
    var = na * nb / 12 * ((n + 1) - ties / (n * (n - 1)))
    if var <= 0:
        return 1.0
    z = (ub - mean - 0.5) / math.sqrt(var)
    return 0.5 * math.erfc(z / math.sqrt(2))


def g7_row_index(lines):
    for i, line in enumerate(lines):
        if re.match(r"^\|\s*G7\s*\|", line):
            return i
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--params", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "..", "docs", "verus", "gate-params.md"))
    ap.add_argument("--aa", action="store_true")
    ap.add_argument("--alpha", type=float, default=0.05)
    o = ap.parse_args()

    data = {}
    bad = False
    for arm in (o.a, o.b):
        durs, failed, total = load(o.dir, arm)
        data[arm] = durs
        print(f"{arm}: {total} kills, {failed} failed")
        if failed > MAX_FAILED:
            print(f"G7 FAIL: {arm} has {failed} failed kills (> {MAX_FAILED})")
            bad = True
        if len(durs["new_leader"]) < 3:
            print(f"G7 FAIL: {arm} has too few usable kills")
            return 1

    with open(o.params) as f:
        lines = f.read().split("\n")
    ri = g7_row_index(lines)
    if ri is None:
        print("no '| G7 |' row in params", file=sys.stderr)
        return 2
    cells = lines[ri].split("|")
    bounds = dict((k, float(v)) for k, v in (x.split("=", 1) for x in cells[3].strip().split(",") if x))

    print(f"  {'duration.stat':22s} {'A ms':>9s} {'B ms':>9s} {'B/A-1':>8s} {'bound':>7s} {'MWU p':>7s}  verdict")
    new_bounds = {}
    for d in DURATIONS:
        xa, xb = data[o.a][d], data[o.b][d]
        p = mann_whitney_greater(xa, xb)
        for s in STATS:
            key = f"{d}.{s}"
            sa, sb = stat(xa, s), stat(xb, s)
            rel = sb / sa - 1 if sa > 0 else float("inf")
            if o.aa:
                new_bounds[key] = max(FLOOR, math.ceil(abs(rel) * 1000) / 1000)
                verdict = f"bound -> {new_bounds[key]:.3f}"
            else:
                bound = bounds.get(key)
                if bound is None:
                    print(f"no bound for {key}", file=sys.stderr)
                    return 2
                if rel > bound and p < o.alpha:
                    verdict = "FAIL"
                    bad = True
                elif rel > bound:
                    verdict = "pass (past bound, not significant)"
                else:
                    verdict = "pass"
            print(f"  {key:22s} {sa / 1000:9.1f} {sb / 1000:9.1f} {rel:+8.1%} "
                  f"{bounds.get(key, float('nan')):7.3f} {p:7.3f}  {verdict}")
    if o.aa:
        n = min(len(data[o.a]["new_leader"]), len(data[o.b]["new_leader"]))
        cells[3] = " " + ",".join(f"{k}={v:g}" for k, v in new_bounds.items()) + " "
        cells[4] = f" from A/A run, n={n} per arm; max(10%, abs(A-B)/A) "
        lines[ri] = "|".join(cells)
        with open(o.params, "w") as f:
            f.write("\n".join(lines))
        print(f"wrote {o.params}")
        return 1 if bad else 0
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
