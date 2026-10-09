#!/usr/bin/env python3
"""The disk plan's cost model (docs/verus/disk-persistence-plan.md §5), Lion era.

One flush of n entries, K KiB in all, costs F(n, K) = s + D + c*K + e*n, where
s is the sync's fixed cost, D the injected delay (MAKO_RAFT_FLUSH_DELAY_US), c
the per-KiB cost (write, copy, CRC32C) and e the per-entry encoding.

Low load (one entry a round; G1, G3, G5). The leader's flush starts at Start
and, since Lion wakes the tick at once (no 1 ms poll hop), is exposed in
full: the tick waits for it, plus one cross-thread wake w (flusher -> poll
thread -> fiber). The follower holds its reply for its own flush; collect
sees replies at its q-microsecond steps, so the reply's delay costs whole
steps: dC = q*(ceil((r + w + F1)/q) - ceil(r/q)), r the memory round trip.

  L_disk = L + (F1 + w) + dC

Saturation (rounds of B entries; G2, G4, G6). Each round puts k flushes of the
round's batch in series with the memory round T = B/X: the follower's (its
reply waits) and, under the tail rule, the leader's burst (k = 2).

  X_disk = B / (T + k*F_B)

  model.py [--fs tmpfs|ext4] [--baseline FILE.json]       print the estimates
  model.py --compare MEASURED.json [--fs ...] [--baseline ...]
      MEASURED.json: {"D": us, "G1_p50": us, "G2": per_s, ...} -> each against
      its estimate; a miss over a quarter of the estimate names a wrong term.

A baseline file holds the memory build's medians: {"G1_p50": us, "G2": per_s,
...}; without one the bug-fix gate's pre-Lion numbers are used (stale).
"""
import argparse
import json
import math

# Measured on zoo-003 (scripts/raft_disk/params.rs, 2026-10-09): write plus
# fdatasync, p50. s is fdatasync alone at 4 KiB; cw the write per KiB.
FS = {
    "tmpfs": {"s": 1.0, "cw": 0.57},
    "ext4": {"s": 165.0, "cw": 1.41},
}
COPY, CRC, E = 0.07, 0.13, 0.5     # us per KiB, per KiB (SSE4.2), per entry
W = 50.0                           # us: a cross-thread wake to the fiber (Lion)
Q = 1000.0                         # us: collect's poll step

# Points: (kind, KiB per entry, entries per round B, partitions, round trip r)
POINTS = {
    "G1": ("low", 4.0, 1, 1, 100.0),
    "G3": ("low", 279.5, 1, 6, 400.0),
    "G5": ("low", 1024.0, 1, 1, None),
    "G2": ("sat", 4.0, 256, 1, None),
    "G4": ("sat", 279.5, 58, 6, None),
    "G6": ("sat", 1024.0, 16, 1, None),
}
# Pre-Lion memory baselines (bug-fix gate 0633e1ffc): stale; pass --baseline.
OLD_BASE = {"G1_p50": 2641, "G3_p50": 3362, "G5_p50": 8489, "G2": 34112, "G4": 2859, "G6": 187}


def F(kib, n, D, fs):
    p = FS[fs]
    return p["s"] + D + kib * (p["cw"] + COPY + CRC) + n * E


def estimate(point, D, fs, base, k=2):
    kind, kib, B, parts, r = POINTS[point]
    if kind == "low":
        L = base[f"{point}_p50"]
        F1 = F(kib, 1, D, fs)
        if r is None:  # a round trip of several collect steps: the flush adds itself
            dC = F1
        else:
            dC = Q * (math.ceil((r + W + F1) / Q) - math.ceil(r / Q))
        return L + F1 + W + dC
    X = base[point]
    T = parts * B / X * 1e6
    FB = F(kib * B, B, D, fs)
    return parts * B / (T + k * FB) * 1e6


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--fs", default="tmpfs", choices=FS)
    ap.add_argument("--baseline")
    ap.add_argument("--compare")
    ap.add_argument("--k", type=int, default=2)
    a = ap.parse_args()
    base = json.load(open(a.baseline)) if a.baseline else OLD_BASE
    if a.compare:
        m = json.load(open(a.compare))
        D = m["D"]
        bad = 0
        for key, got in m.items():
            if key == "D":
                continue
            point = key.split("_")[0]
            est = estimate(point, D, a.fs, base, a.k)
            off = (got - est) / est
            ok = abs(off) <= 0.25
            bad += not ok
            print(f"{key:8s} D={D:5.0f}us measured {got:10.1f} estimate {est:10.1f} ({off:+6.1%}) "
                  f"{'ok' if ok else 'MISS: a term is wrong'}")
        return 1 if bad else 0
    print(f"fs={a.fs} k={a.k} baseline={'given' if a.baseline else 'pre-Lion (stale)'}")
    for D in (0, 200, 1000):
        row = []
        for point in POINTS:
            est = estimate(point, D, a.fs, base, a.k)
            mem = base[f"{point}_p50"] if POINTS[point][0] == "low" else base[point]
            unit = "us" if POINTS[point][0] == "low" else "/s"
            row.append(f"{point} {est:8.0f}{unit} ({est / mem - 1:+6.1%})")
        print(f"D={D:5d}us  " + "  ".join(row))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
