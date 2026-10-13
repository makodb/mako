#!/usr/bin/env python3
"""G2's gated metric: the time of a heartbeat round that sent to both followers.

    scripts/verus/two_follower_rounds.py summarize OUT_DIR ROUNDS arm...
    scripts/verus/two_follower_rounds.py stats OUT_DIR ROUNDS A B [--json]

Reads the per-entry stage traces of a TRACE=1 rotation
(scripts/raft_perf/rotation_trial.sh: OUT_DIR/trace.r<i>.<arm>.<pid>, one CSV
per process, from the Phase 0 trace kit, MAKO_RAFT_TRACE_FILE).

Why this metric (Phase 3 report §3.1, since removed). At saturation the
leader skips a follower whose reply missed the round's early-quorum pass, a
round that sends to one follower is far shorter than one that sends to two,
and how often that happens is a timing race. G2's throughput therefore
measured mostly the skip rate. The time of a round that sent to both
followers is the per-message cost the point is meant to measure, and the
skip rate cannot bias it.

A round is identified by its first send's stage-3 stamp (the entries of the
batch share it). A follower got the round when its handler began a batch
(stage 5, first write wins, so a batch of entries it had not seen) within
5 ms after that stamp, whatever entries the batch held: a follower that is
behind is sent older ones. A round's time is the gap to the next round's
first send. Rounds in the middle half of the run are counted, away from the
warm-up and the drain, and only full ones: rounds whose first send carried
the run's largest batch (256 entries at G2). A run can fall into a stretch
of short follow-up rounds carrying a few entries each (one Phase 3
validation run had 1,208 of them), whose times would otherwise swamp the
median.

summarize writes OUT_DIR/r<i>.<arm>.rounds.json per run (kept, so a re-run
only reads new traces) with:
  two_follower_round_us   median time of rounds that sent to both followers
  one_follower_round_us   the same for rounds that sent to one (reported)
  two_follower_share      fraction of the full rounds that sent to both
  rounds                  full rounds counted
  batch                   the full round's entry count
stats prints the paired comparison B against A in paired_stats.py's --json
shape, so scripts/verus/perf_gate.py applies the pass rule to it.
"""
import argparse
import bisect
import csv
import glob
import json
import os
import statistics
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "raft_perf"))
from paired_stats import sign_p  # noqa: E402

WINDOW_US = 5000
METRIC = "two_follower_round_us"


def trace_files(out, i, arm):
    files = glob.glob(os.path.join(out, f"trace.r{i}.{arm}.[0-9]*"))
    if len(files) > 3:
        # Leftovers of an interrupted attempt: the newest three are the run's.
        files = sorted(files, key=os.path.getmtime)[-3:]
        print(f"two_follower_rounds: r{i}.{arm}: more than 3 trace files, using the newest 3",
              file=sys.stderr)
    return files


def read_process(path):
    """(entries this process enqueued, {first-send stamp: entries it
    carried}, {handler start: handler end})."""
    enqueued = 0
    sends = {}
    batches = {}
    with open(path) as f:
        for r in csv.DictReader(f):
            if int(r["s0"]):
                enqueued += 1
            s3 = int(r["s3"])
            if s3:
                sends[s3] = sends.get(s3, 0) + 1
            h5, h6 = int(r["s5"]), int(r["s6"])
            if h5:
                batches[h5] = max(batches.get(h5, 0), h6)
    return enqueued, sends, batches


def summarize_run(files):
    procs = sorted((read_process(p) for p in files), key=lambda x: -x[0])
    if len(procs) != 3 or not procs[0][1]:
        return None
    carried = procs[0][1]
    full = max(carried.values())
    sends = sorted(carried)
    followers = [sorted(b) for _, _, b in procs[1:]]
    lo, hi = len(sends) // 4, 3 * len(sends) // 4
    both, one = [], []
    for k in range(lo, min(hi, len(sends) - 1)):
        s, nxt = sends[k], sends[k + 1]
        if carried[s] < full:
            continue
        got = 0
        for starts in followers:
            j = bisect.bisect_left(starts, s)
            if j < len(starts) and starts[j] < s + WINDOW_US:
                got += 1
        if got == 2:
            both.append(nxt - s)
        elif got == 1:
            one.append(nxt - s)
    n = len(both) + len(one)
    if not both:
        return None
    return {
        METRIC: statistics.median(both),
        "one_follower_round_us": statistics.median(one) if one else None,
        "two_follower_share": len(both) / n,
        "rounds": n,
        "batch": full,
    }


def summarize(out, rounds, arms):
    rc = 0
    for i in range(1, rounds + 1):
        for arm in arms:
            dst = os.path.join(out, f"r{i}.{arm}.rounds.json")
            if os.path.exists(dst):
                continue
            files = trace_files(out, i, arm)
            if not files:
                continue
            res = summarize_run(files)
            if res is None:
                print(f"two_follower_rounds: r{i}.{arm}: no usable trace", file=sys.stderr)
                rc = 1
                continue
            with open(dst, "w") as f:
                json.dump(res, f)
    return rc


def load(out, i, arm):
    try:
        with open(os.path.join(out, f"r{i}.{arm}.rounds.json")) as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def stats(out, rounds, a, b, as_json, min_pairs):
    pairs = []
    for i in range(1, rounds + 1):
        ra, rb = load(out, i, a), load(out, i, b)
        if ra and rb:
            pairs.append((ra[METRIC], rb[METRIC]))
    ratios = [y / x - 1 for x, y in pairs if x > 0]
    res = {"a": a, "b": b, "pairs": len(pairs), "rounds": rounds, "metrics": {}}
    if ratios:
        pos = sum(1 for r in ratios if r > 0)
        neg = sum(1 for r in ratios if r < 0)
        res["metrics"][METRIC] = {
            "median_ratio": statistics.median(ratios), "pos": pos, "neg": neg,
            "sign_p": sign_p(pos, neg), "better": "lower", "n": len(ratios),
            "median_a": statistics.median(x for x, _ in pairs),
            "median_b": statistics.median(y for _, y in pairs)}
    if as_json:
        print(json.dumps(res))
    else:
        print(f"{len(pairs)} complete rounds of {rounds}: B={b} against A={a}")
        for key, m in res["metrics"].items():
            print(f"  {key:24s} median B/A-1 = {m['median_ratio']:+.2%}  "
                  f"(A {m['median_a']:.0f} us, B {m['median_b']:.0f} us; B>A in {m['pos']}, "
                  f"B<A in {m['neg']}; sign test p = {m['sign_p']:.3f}; lower is better)")
    if len(pairs) < min_pairs:
        print(f"two_follower_rounds: only {len(pairs)} of {rounds} rounds complete "
              f"(need {min_pairs})", file=sys.stderr)
        return 1
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("command", choices=("summarize", "stats"))
    ap.add_argument("out")
    ap.add_argument("rounds", type=int)
    ap.add_argument("arms", nargs="+")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--min-pairs", type=int)
    o = ap.parse_args()
    if o.command == "summarize":
        return summarize(o.out, o.rounds, o.arms)
    if len(o.arms) != 2:
        ap.error("stats takes two arms, A and B")
    min_pairs = o.min_pairs if o.min_pairs is not None else o.rounds * 9 // 10
    return stats(o.out, o.rounds, o.arms[0], o.arms[1], o.json, min_pairs)


if __name__ == "__main__":
    sys.exit(main())
