#!/usr/bin/env python3
"""Predicting Raft disk mode's performance from first principles.

This file is a worked example of bottleneck estimation ("napkin math"): price
the primitives in isolation, write down the structure of the critical path
from the code, combine the two into a prediction, and only then measure. A
prediction that misses is information -- it says a primitive or the
structure is wrong -- and the record at the end shows what each miss taught.

THE RULE: no input below comes from a disk-mode run. Primitives come from
microbenchmarks that never run Raft (scripts/raft_disk/params.rs); the parts
of a round that are not disk come from the *memory* build's trace (the
baseline the prediction is relative to). Disk-mode runs are only ever the
check (scripts/raft_disk/measure.py). Fitting a number to the run it is then
compared with proves nothing.

----------------------------------------------------------------------------
1. PRIMITIVES (what one operation costs, alone)
----------------------------------------------------------------------------
  s      write + fdatasync of 4 KiB, its fixed part, measured in the access
         pattern the structure produces -- the same call costs different
         amounts in different patterns. At low load a round's syncs come one
         at a time from different replicas, a network hop apart: st (three
         writers taking turns, 200 us apart). Saturated, every replica's
         flusher syncs back to back and they overlap: s3 (three at once).
  BW     write bandwidth against volume: bw_fast until bw_cache MB are
         written (a write cache), bw_slow after it (the media).
  cw     write cost per KiB (from 4 KiB to 1 MiB, the fixed part cancels).
  COPY   memcpy per KiB; CRC32C per KiB (SSE4.2).
  e      encoding one entry beyond its copy: the command serializer. Read off
         the memory build's trace -- a send serializes every entry it carries.
  D'     the injected delay as slept: D plus a sleep's overshoot.
  q      a condvar hand-off: a record pushed -> the flusher thread running;
         and the publish -> a held reply's send (the flusher sends it).
  w      a cross-thread wake of a fiber: the job the flusher posts to the poll
         thread. The memory build pays the same path at every Start (Start ->
         the tick: the trace's stage 2 -> 3).

  F(n, K) = q + s + D' + K*(cw + COPY + CRC) + n*e        one flush

----------------------------------------------------------------------------
2. STRUCTURE (read from the code, not from measurements)
----------------------------------------------------------------------------
Low load, one entry per round (G1, G3, G5). The heartbeat loop runs ONE
round at a time (shell/server_cc.rs, HeartbeatDriver::run):
  a. Start appends the entry and queues its record; the tick wakes and waits
     until the leader's WAL holds every record queued so far, then a fiber
     wake w, then sends (P4: nothing leaves before its records are durable).
  b. The follower appends, queues its record, and holds its reply until its
     WAL holds it, then the flusher sends the reply (q).
  c. Collect wakes on the reply (F11a) and the round end raises the commit:
     the entry is committed and goes to apply.
  d. The commit was raised after the round's send, so a FOLLOW-UP round
     announces it (heartbeat_round_end_body): the leader's commit record must
     be durable before that send, and the follower holds its reply for its
     own commit record. Two more flushes -- off this entry's latency path,
     but the loop is busy, and the next entry waits if it arrives meanwhile.
Each replica's flusher is a queue: a record queued while a flush runs waits
for the next one (group commit). `simulate` plays this out for periodic
arrivals; with no flushes it reproduces the memory build, whose median
calibrates the network and apply pieces.

Saturation, rounds of B entries (G2, G4, G6): the memory round T = B/X plus
two flushes in series. The follower's, of the batch: it holds its reply for
it. And the leader's, of one entry: the tick waits for every record queued
so far (step a), and the newest are the Starts the client sent on hearing
the last commit -- which landed just before this tick, so their flush has
only begun. The batch itself is older (it queued for many rounds) and
durable; what the tick waits for is a flush's fixed part:

  X_disk = B / (T + F(B, B*kib) + F(1, kib))

That is the latency bound. The device bounds it too (a roofline): each
entry reaches the device 3 (replicas) x A times, b = 3 * A * record bytes,
and X_device = bw_slow / b; the estimate is the lower of the two. A = 3: the
WAL, the base's memtable flush (its WAL is off), and RocksDB's L0 -> L1
compaction, which starts at 4 L0 files (256 MB a replica) and so runs
within a run. bw_slow, not bw_fast: on a device with a write cache, a run's
first seconds measure the cache, so the saturated ext4 points discard them
(measure.py STEADY_WARMUP).

----------------------------------------------------------------------------
3. USE
----------------------------------------------------------------------------
  params.rs <dir> > params.txt          primitives (its last line: PARAMS {...})
  model.py --params P.txt --memtrace M  estimates (M: a memory-build G1 trace
                                        prefix, MAKO_RAFT_TRACE_FILE, and its G2
                                        trace with --memtrace-sat)
  model.py ... --compare MEASURED.json --baseline BASE.json
      each measured value against its estimate; a miss over a quarter of the
      estimate means a primitive or the structure is wrong: find which with
      the trace kit before changing anything (plan §5).

----------------------------------------------------------------------------
4. WHAT THE MISSES TAUGHT (2026-10-09/10)
----------------------------------------------------------------------------
  - Pre-Lion, the leader's flush hid in a 1 ms poll hop. Lion wakes the poll
    thread at once, so the flush is exposed in full: a structure change
    upstream silently invalidated a term.
  - Collect slept a fixed step that Lion rounds to whole milliseconds; a
    reply held for a flush missed a step (G1 +38% at D = 1 ms). The fix was
    in the code (collect waits on the reply), and memory mode gained too.
  - The commit's follow-up round was missing from the structure: harmless
    in memory mode, two flushes in disk mode, and once a round pair outlasts
    the arrival gap, entries queue for the loop.
  - Saturated, the reply outlasted collect's deadline (one heartbeat, 5 ms;
    the follower's hold made it ~7.6 ms) and the two followers fell out of
    step: each round waited out its deadline for one while the other, its
    reply in, sat idle (1,600 of ~3,000 collects ended at the deadline; G2
    tmpfs 21-23k/s against 30k). Code, not model: collect ends when an
    earlier round's reply frees a follower (36k/s).
  - Then k, the number of batch flushes in series, went 2 -> 1 -> neither.
    k = 2 looked wrong once the collect fix landed (one run, 36k/s against
    k = 1's 36.5k) -- but that k = 1 estimate was built on memory pieces
    traced BEFORE the fix, and the fix sped the memory build too (G2 +82%).
    Re-traced, k = 1 missed G2 tmpfs by -19% and -28% (D = 0, 1 ms). The
    trace said where: 256-entry rounds, 6.4 / 9.3 ms apart, of which the
    reply round trip was 6.2 / 7.9 -- the rest, 0.2 / 1.4 ms, the leader
    waiting at the tick for its WAL before sending. That wait is the flush
    of the last few Starts (leader flushes of p50 177 us at D = 0), not of
    the batch: a fixed-cost flush, F(1). Two lessons: a model input taken
    from the baseline goes stale when a change moves the baseline -- re-take
    it; and agreement from one run on stale inputs proves nothing. What is
    left (G2 tmpfs -16 to -18%): in place, a follower's flush ran ~10% over
    F, and its AppendEntries handler 0.3 ms over the memory build's.
  - e was assumed 0.5 us; the serializer costs about 3 us an entry. At a
    256-entry batch that is most of a flush on tmpfs.
  - s measured with one writer (165 us on ext4) is not s in place, and
    neither was three writers at once (273 us): G1 on ext4 missed by +31%.
    The flusher stats put a flush in place at 365-400 us. Taking the syncs in
    turns, as a round does, gave 285 us; with an idle gap between them, 360
    us. ext4's journal batches syncs that arrive together; a lone one after
    an idle device pays in full. Measure a primitive in the structure's
    pattern.
  - G2 on ext4 came in at a third of the latency bound (-65%), with 1-2 s
    stalls. Disk stats: follower flushes of p50 3 ms, mean 15 ms;
    /proc/meminfo: 190 MB in writeback at once, three replicas' 64 MB
    memtables flushing together. The device: a rotational RAID volume whose
    cache takes ~1.5 GB fast, then ~135 MB/s. No latency term finds that:
    the miss was a missing ceiling. A first cut took A = 3 (WAL, flush,
    one compaction) and a 12 s run's volume through the cache, and missed
    again (-30%): the cache's fill is not repeatable (1.1-1.7 GB between
    params.rs runs, ~2.4 GB in place) and a 12 s window is mostly transient.
    Measure the steady state instead (20 s warm-up). A was then argued down
    to 2 (compaction as debt) from a device byte count of 3 x 2.35 x 4.1 KB
    an entry -- but that count was taken with B32 live (below), the base
    stalled and writing less. With B32 fixed it read 3 x 2.8: A = 3 stood.
    A check taken on a broken system confirms the wrong number.
  - The same G2 ext4 miss found a bug: every append wrote a RocksDB range
    delete, and their flush is superlinear in their number (bugs-found B32;
    1-2 s stalls, shutdowns past their budget). The model said "too slow by
    this much"; the stats said where.
"""
import argparse
import csv
import glob
import json
import re
import statistics

POINTS = {  # kind, KiB per entry, entries per saturated round B, offered rate (/s)
    "G1": ("low", 4.0, 1, 240.0),
    "G3": ("low", 279.5, 1, 190.0),
    "G5": ("low", 1024.0, 1, 55.0),
    "G2": ("sat", 4.0, 256, None),
    "G4": ("sat", 279.5, 58, None),
    "G6": ("sat", 1024.0, 16, None),
}
COPY, CRC = 0.07, 0.13   # us per KiB: memcpy 15 GB/s, CRC32C SSE4.2 8 GB/s (params.rs)
REPLICAS = 3
AMP = 3                  # device writes per entry per replica: WAL, memtable flush, L0 -> L1 compaction (§2)
REC_KIB = 0.1            # a WAL record's bytes beyond the payload (frame, record header, command)


def read_params(path):
    for line in open(path):
        if line.startswith("PARAMS "):
            return json.loads(line[len("PARAMS "):])
    raise SystemExit(f"{path}: no PARAMS line (run scripts/raft_disk/params.rs)")


def trace_rows(prefix):
    rows = {}
    for f in glob.glob(prefix + ".*"):
        for r in csv.DictReader(open(f)):
            cur = rows.setdefault(int(r["idx"]), {})
            for s in range(12):
                v = int(r[f"s{s}"])
                if v and (s not in cur or (s in (5, 6) and v < cur[s])):
                    cur[s] = v
    return rows


def memory_pieces(prefix):
    """The non-disk pieces of a G1 round, from the memory build's trace:
    Start -> send (the tick's wake: w), send -> follower accepted, follower
    -> leader's reply, reply -> commit, commit -> apply."""
    rows = trace_rows(prefix)

    def med(a, b):
        return statistics.median(r[b] - r[a] for r in rows.values() if a in r and b in r and r[b] >= r[a])
    return {"w": med(2, 3), "out": med(3, 6), "back": med(6, 7), "end": med(7, 8), "apply": med(8, 11)}


def encode_per_entry(prefix):
    """e: a send's duration (trace stages 3 -> 4) per entry it carried, in the
    memory build's saturated run, less the entry's copy."""
    rows = trace_rows(prefix)
    sends = {}
    for r in rows.values():
        if 3 in r and 4 in r:
            sends.setdefault((r[3], r[4]), 0)
            sends[(r[3], r[4])] += 1
    per = [(b - a) / n for (a, b), n in sends.items() if n >= 32]
    return statistics.median(per) - 4.0 * COPY


class Model:
    def __init__(self, params, pieces, e, fs_writers=3):
        p = params
        self.s_low, self.s_sat = p["st"], p["s3"]
        self.cw = p["cw3"] if fs_writers == 3 else p["cw1"]
        self.overshoot, self.q, self.e = p["overshoot"], p["handoff"], e
        self.bw = (p["bw_fast"], p["bw_cache"], p["bw_slow"])
        self.m = pieces

    def F(self, kib, n, D, s=None):
        s = self.s_low if s is None else s
        return self.q + s + (D + self.overshoot if D > 0 else 0.0) + kib * (self.cw + COPY + CRC) + n * self.e

    def device_bound(self, kib):
        """Entries/s the device sustains (the roofline), past any write cache."""
        b = REPLICAS * AMP * (kib + REC_KIB) * 1024 / 1e6  # MB per entry
        return self.bw[2] / b

    def simulate(self, rate, kib, D, disk=True, n=2000):
        """Median Start -> apply (us) for periodic arrivals: the structure of §2."""
        A, m = 1e6 / rate, self.m

        def flush_end(state, at, entries):
            # A replica's flusher: one flush at a time; a record queued while
            # one runs goes in the next.
            start = max(state[0], at)
            state[0] = start + (self.F(kib * entries, entries, D) if disk else 0.0)
            return state[0]

        leader, follower = [0.0], [0.0]
        loop_free, lat, i = 0.0, [], 0
        while i < n:
            start = max(loop_free, i * A)
            batch = [j for j in range(i, n) if j * A <= start]
            ready = max(flush_end(leader, j * A, 1) for j in batch) if disk else start
            send = max(start, ready) + m["w"]
            accepted = send + m["out"]
            replied = (flush_end(follower, accepted, len(batch)) + self.q) if disk else accepted
            commit = replied + m["back"] + m["end"]
            lat += [commit + m["apply"] - j * A for j in batch]
            i = batch[-1] + 1
            # d. the follow-up round announcing the commit
            f_send = (flush_end(leader, commit, 0) if disk else commit) + m["w"]
            f_acc = f_send + m["out"]
            f_rep = (flush_end(follower, f_acc, 0) + self.q) if disk else f_acc
            loop_free = f_rep + m["back"] + m["end"]
        return statistics.median(lat)

    def estimate(self, point, D, base):
        kind, kib, B, rate = POINTS[point]
        if kind == "low":
            return base[f"{point}_p50"] + self.simulate(rate, kib, D) - self.simulate(rate, kib, D, disk=False)
        T = B / base[point] * 1e6
        follower = self.F(kib * B, B, D, self.s_sat)  # the batch, held reply
        leader = self.F(kib, 1, D, self.s_sat)          # the newest Starts, at the tick
        latency_bound = B / (T + follower + leader) * 1e6
        return min(latency_bound, self.device_bound(kib))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--params", required=True, help="params.rs output for the store's filesystem")
    ap.add_argument("--memtrace", required=True, help="memory-build G1 trace prefix")
    ap.add_argument("--memtrace-sat", required=True, help="memory-build G2 trace prefix (for e)")
    ap.add_argument("--baseline", required=True, help='memory medians, {"G1_p50": us, "G2": per_s}')
    ap.add_argument("--compare")
    a = ap.parse_args()
    params = read_params(a.params)
    pieces = memory_pieces(a.memtrace)
    e = encode_per_entry(a.memtrace_sat)
    model = Model(params, pieces, e)
    base = json.load(open(a.baseline))
    print(f"inputs: params {params}; memory pieces { {k: round(v) for k, v in pieces.items()} }; e {e:.2f} us")
    if a.compare:
        m = json.load(open(a.compare))
        D, bad = m["D"], 0
        for key, got in m.items():
            if key == "D":
                continue
            point = re.split(r"_", key)[0]
            est = model.estimate(point, D, base)
            off = (got - est) / est
            ok = abs(off) <= 0.25
            bad += not ok
            print(f"{key:8s} D={D:5.0f}us measured {got:10.1f} estimate {est:10.1f} ({off:+6.1%}) "
                  f"{'ok' if ok else 'MISS: a primitive or the structure is wrong'}")
        return 1 if bad else 0
    for D in (0, 200, 1000):
        row = []
        for point in ("G1", "G2"):
            est = model.estimate(point, D, base)
            mem = base[f"{point}_p50"] if POINTS[point][0] == "low" else base[point]
            row.append(f"{point} {est:8.0f}{'us' if POINTS[point][0] == 'low' else '/s'} ({est / mem - 1:+6.1%})")
        print(f"D={D:5d}us  " + "  ".join(row))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
