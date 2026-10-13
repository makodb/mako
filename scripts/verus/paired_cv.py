#!/usr/bin/env python3
"""Paired spread of an A/A rotation run, and the round count and bounds it supports.

    scripts/verus/paired_cv.py OUT_DIR ROUNDS A B [--point G1] [--params docs/verus/gate-params.md]
                               [--min-rounds 10] [--no-write]

Reads OUT_DIR/r<i>.<arm>.json (scripts/raft_perf/rotation_trial.sh). For every
metric of paired_stats.py: the per-round ratios B/A - 1 over complete rounds,
CV_paired = their sample standard deviation (a ratio is unitless, so this is
already relative), and the paired MDE at n rounds, 2.8 * CV_paired / sqrt(n).

For the point's bounded metrics (the "| default Gk |" row of the params file,
i.e. plan §6's table) it derives the round count: the smallest n in
[--min-rounds, 25] whose MDE is below every bound. A metric whose 25-round MDE
is not below its bound gets its bound widened to that MDE (rounded up to 0.1%),
and the widening is noted. Unless --no-write, it then replaces the point's
"| Gk |" row in the params file, which scripts/verus/gate_point.sh reads.

--min-rounds defaults to 10: with fewer rounds even a unanimous split is barely
significant under the two-sided sign test (p = 0.0625 at 5-0, 0.031 at 6-0),
so the gate's second condition could hardly ever fire.
"""
import argparse
import math
import os
import re
import statistics
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "raft_perf"))
import paired_stats as PS  # noqa: E402

MAX_ROUNDS = 25


def parse_bounds(text):
    out = {}
    for item in text.split(","):
        item = item.strip()
        if item:
            k, v = item.split("=", 1)
            out[k] = float(v)
    return out


def fmt_bounds(bounds):
    return ",".join(f"{k}={v:g}" for k, v in bounds.items())


def find_row(lines, label):
    pat = re.compile(r"^\|\s*" + re.escape(label) + r"\s*\|")
    for i, line in enumerate(lines):
        if pat.match(line):
            return i
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("rounds", type=int)
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--point")
    ap.add_argument("--params", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "..", "docs", "verus", "gate-params.md"))
    ap.add_argument("--min-rounds", type=int, default=10)
    ap.add_argument("--no-write", action="store_true")
    o = ap.parse_args()
    point = o.point or os.path.basename(os.path.normpath(o.out))

    rows = []
    for i in range(1, o.rounds + 1):
        ra, rb = PS.load(o.out, i, o.a), PS.load(o.out, i, o.b)
        if ra and rb:
            rows.append((ra, rb))
    n = len(rows)
    print(f"{point}: {n} complete rounds of {o.rounds}, B={o.b} against A={o.a}")
    if n < 3:
        print("paired_cv: too few complete rounds", file=sys.stderr)
        return 1

    cv = {}
    print(f"  {'metric':32s} {'median B/A-1':>13s} {'CV_paired':>10s} {'MDE@n':>8s} {'MDE@25':>8s}")
    for key, _better in PS.METRICS:
        pairs = [(PS.value(ra, key), PS.value(rb, key)) for ra, rb in rows]
        ratios = [y / x - 1 for x, y in pairs
                  if isinstance(x, (int, float)) and isinstance(y, (int, float)) and x > 0]
        if len(ratios) < 3:
            continue
        c = statistics.stdev(ratios)
        cv[key] = (c, len(ratios))
        print(f"  {key:32s} {statistics.median(ratios):+13.2%} {c:10.2%} "
              f"{2.8 * c / math.sqrt(len(ratios)):8.2%} {2.8 * c / math.sqrt(MAX_ROUNDS):8.2%}")

    with open(o.params) as f:
        lines = f.read().split("\n")
    di = find_row(lines, f"default {point}")
    if di is None:
        print(f"paired_cv: no '| default {point} |' row in {o.params}", file=sys.stderr)
        return 2
    defaults = parse_bounds(lines[di].split("|")[3])

    need, bounds, notes = o.min_rounds, {}, []
    for key, bound in defaults.items():
        if key not in cv:
            print(f"paired_cv: no data for bounded metric {key}", file=sys.stderr)
            return 1
        c = cv[key][0]
        k = next((r for r in range(o.min_rounds, MAX_ROUNDS + 1)
                  if 2.8 * c / math.sqrt(r) < abs(bound)), None)
        if k is None:
            mde = math.ceil(2.8 * c / math.sqrt(MAX_ROUNDS) * 1000) / 1000
            bounds[key] = math.copysign(mde, bound)
            notes.append(f"{key} widened {bound:+g} -> {bounds[key]:+g} (25-round MDE)")
            k = MAX_ROUNDS
        else:
            bounds[key] = bound
        need = max(need, k)
    note = "; ".join(notes) if notes else "default bounds hold"
    note += f"; CV_paired " + ", ".join(f"{k}={cv[k][0]:.2%}" for k in defaults) + f" (A/A n={n})"
    row = f"| {point} | {need} | {fmt_bounds(bounds)} | {note} |"
    print("row:", row)
    if not o.no_write:
        i = find_row(lines, point)
        if i is None:
            print(f"paired_cv: no '| {point} |' row in {o.params}", file=sys.stderr)
            return 2
        lines[i] = row
        with open(o.params, "w") as f:
            f.write("\n".join(lines))
        print(f"wrote {o.params}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
