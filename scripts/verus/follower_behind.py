#!/usr/bin/env python3
"""G2's reported (not gated) line: how many runs end with a follower behind.

    scripts/verus/follower_behind.py OUT_DIR ROUNDS arm...

Reads OUT_DIR/r<i>.<arm>.json from an untraced rotation
(scripts/raft_perf/rotation_trial.sh). A run ends with a follower behind when
some follower's applied count (the leader's record carries
p1_/p2_applied_total) is below the leader's when the run ends. At G2's
saturation such a follower was skipped in rounds whose early-quorum pass its
reply missed, and those runs are the fast ones
(docs/verus/reports/phase-3.md §3.1), so the count is printed beside the
throughput it explains. Each arm after the first is compared with the first:
the counts by Fisher's exact test, the medians of runs that ended caught up
by a two-sided Mann-Whitney U (normal approximation, tie-corrected). Both
unpaired. Exit status is always 0: this reports, it does not gate.
"""
import json
import math
import os
import statistics
import sys


def fisher_two_sided(a, b, c, d):
    """Two-sided Fisher exact p for the 2x2 table [[a, b], [c, d]]."""
    n1, n2, k = a + b, c + d, a + c

    def p(x):
        return math.comb(n1, x) * math.comb(n2, k - x) / math.comb(n1 + n2, k)

    obs = p(a)
    lo, hi = max(0, k - n2), min(k, n1)
    return min(1.0, sum(p(x) for x in range(lo, hi + 1) if p(x) <= obs * (1 + 1e-9)))


def mann_whitney_two_sided(a, b):
    na, nb = len(a), len(b)
    if not na or not nb:
        return float("nan")
    allv = sorted([(x, 0) for x in a] + [(y, 1) for y in b])
    ranks = [0.0] * len(allv)
    ties = 0.0
    i = 0
    while i < len(allv):
        j = i
        while j + 1 < len(allv) and allv[j + 1][0] == allv[i][0]:
            j += 1
        for k in range(i, j + 1):
            ranks[k] = (i + j) / 2 + 1
        t = j - i + 1
        ties += t ** 3 - t
        i = j + 1
    rb = sum(r for r, (_, g) in zip(ranks, allv) if g == 1)
    ub = rb - nb * (nb + 1) / 2
    n = na + nb
    var = na * nb / 12 * ((n + 1) - ties / (n * (n - 1)))
    if var <= 0:
        return 1.0
    z = (abs(ub - na * nb / 2) - 0.5) / math.sqrt(var)
    return min(1.0, math.erfc(max(z, 0.0) / math.sqrt(2)))


def load(out, i, arm):
    try:
        with open(os.path.join(out, f"r{i}.{arm}.json")) as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def behind(rec):
    a = rec["applied_total"]
    return any(a - rec.get(f"p{k}_applied_total", a) > 0 for k in (1, 2))


def main():
    if len(sys.argv) < 4:
        print(__doc__, file=sys.stderr)
        return 2
    out, rounds, arms = sys.argv[1], int(sys.argv[2]), sys.argv[3:]
    rows = {}
    for arm in arms:
        recs = [r for r in (load(out, i, arm) for i in range(1, rounds + 1)) if r]
        rows[arm] = ([r["applied_per_sec"] for r in recs if not behind(r)],
                     [r["applied_per_sec"] for r in recs if behind(r)])

    def med(v):
        return f"{statistics.median(v):,.0f}/s" if v else "-"

    first = arms[0]
    print(f"{'arm':18s} {'runs':>4s} {'behind':>6s} {'caught-up median':>17s} "
          f"{'behind median':>14s}  vs {first}")
    for arm in arms:
        up, dn = rows[arm]
        line = f"{arm:18s} {len(up) + len(dn):4d} {len(dn):6d} {med(up):>17s} {med(dn):>14s}"
        if arm != first:
            fu, fd = rows[first]
            p_count = fisher_two_sided(len(fd), len(fu), len(dn), len(up))
            line += f"  behind: Fisher p = {p_count:.3f}"
            if up and fu:
                ratio = statistics.median(up) / statistics.median(fu) - 1
                line += (f"; caught-up median {ratio:+.2%}, "
                         f"Mann-Whitney p = {mann_whitney_two_sided(fu, up):.3f}")
        print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
