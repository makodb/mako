# Raft performance baseline

The reference measurement of Mako's Raft **before** the C++-to-Rust
conversion. Everything needed to restate it to a third party is here: the
configuration, the definitions, the formulas, the numbers, their spread, the
smallest change those numbers can detect, and the command that reproduces
them.

- **Commit:** `412c225a9e5455d148776f33b1d12a9bfe3114db`
- **Machine:** zoo-003, Linux 7.0.0-29, clang Release, `MODE=perf`
- **Date:** 2026-09-12
- **Runs:** 624 sweep runs + 40 repeatability runs, **0 failed**
- **Raw data:** `docs/performance/raft-baseline-412c225a/`

```
raft-baseline-412c225a/
  SUMMARY.md         the sweep's own record of what it ran
  records.tar.gz     all 664 JSON records (2.4 MB unpacked)
  tables/            processing.py output per phase, as committed
  plots/             the three figures
```

The records are archived rather than stored loose because the repository
ignores `*.json` globally. Unpack before using them:

```bash
tar -xzf docs/performance/raft-baseline-412c225a/records.tar.gz -C /tmp
python3 scripts/raft_perf/processing.py /tmp/records/rate
```

Every record is stamped with that commit and none is marked `-dirty`.

## The one-paragraph version

Three Raft replicas on one machine, replicating 286 KB log entries across six
partitions with one Raft group per partition — Mako's production shape — commit
and apply entries at a **median of 7.50 ms (p99 9.51 ms)** when offered the
~190 entries/s that the real TPC-C workload generates, and sustain **at least
1 588 entries/s (454 MB/s)** before saturating. Run-to-run variation at that
operating point is 1.0% on median latency and 3.4% on saturated throughput, so
three trials resolve a change of about 1.6% in latency and 5.5% in throughput,
and ten trials about 0.9% and 3.0%. Those are the numbers the Rust conversion
has to preserve.

## What was measured, and what was not

Raft alone. Three replica processes run the production `raft_bench` build; a
load generator inside the leader submits fixed-size log entries at a
controlled rate; the harness reports how long each entry took to be replicated,
committed and applied, and how many entries per second were applied. Nothing
above Raft is in the path — no Masstree, no STO concurrency control, no
transaction execution, no client RPC.

This is deliberate. The conversion being validated touches Raft and only Raft,
and `dbtest`'s end-to-end transaction throughput divides any Raft-local change
by every layer above it.

## Configuration

| | |
|---|---|
| **Servers (replicas)** | 3 — one leader, two followers; a Raft majority is 2 |
| **Processes** | 3, all on one host, over loopback TCP through Mako's `rrr` RPC |
| **Partitions** | 1 or 6 |
| **Group mode** | `single` (one Raft group serves all partitions) or `multi` (one group per partition) |
| **Clients** | none as separate processes — see below |
| **Load generator** | one thread per partition, inside the leader process |
| **Entry sizes** | 4 096 B, 286 208 B, 1 048 576 B |
| **Offered rate** | paced, 21 points per series from far below saturation to unthrottled |
| **In-flight cap** | 4 096 / 256 / 64 entries per partition, by entry size |
| **Measured window** | 10 s, after a 3 s warmup that is excluded |
| **Trials per point** | 3 (10 for the noise floor) |
| **Durability** | none — memory-only Raft, snapshots off, no disk write in the path |

### "How many clients?" — the honest answer

There are no client processes. The load generator is `offer_loop`
(`src/deptran/raft/raft_bench.cc:679`), one thread per partition running inside
the leader process, calling `add_log_to_nc()` — the same entry point
`raft_main_helper.cc` uses in production. So the count corresponding to
"clients" is **one submitting thread per partition**: 1 at one partition, 6 at
six.

This is an **open-loop** load model. Each thread offers entries on a fixed
schedule and does not wait for the previous entry to be applied before
offering the next; a bounded number may be in flight. It is not the closed-loop
"N clients each with one outstanding request" model, so there is no client
count to trade against latency.

The practical consequence: **the offered rate is the independent variable**,
not a client count. Latency is read at a fixed offered rate; capacity is read
where the offered rate stops being the limit.

### Why 286 KB and 190 entries/s

`docs/plans/raft-perf-profile.txt` measured what Mako actually submits to Raft
under TPC-C: entries with a median of 286 208 B, a long thin tail into the tens
of megabytes, and roughly 50 entries/s per partition (about 303 aggregate).
286 208 B is the measured p50, not the mean — the mean (~310 KB) is dragged up
by rare 13 MB and 25 MB entries. The 4 KiB and 1 MiB sizes bracket it.

## Definitions and formulas

### Latency

Per entry, measured entirely inside the leader process:

```
latency_i = apply_time_i - enqueue_time_i
```

`enqueue_time_i` is stamped into the entry's payload when `add_log_to_nc()` is
called. `apply_time_i` is read in the leader's apply callback, which fires
after the entry has been replicated to a majority, committed, and delivered to
the state machine. Both come from the same process's `steady_clock`, so no
cross-clock correction is needed and no clock skew enters the number.

The path being timed, in order: offer thread → `RaftWorker::submit_queue_` →
`SubmitLoop` → `RaftServer::Start()` (local append) → `RequestReplication()` →
the leader's replication fiber → AppendEntries to a majority → commit →
`apply_queue_` → apply thread → callback.

Reported over all entries applied inside the measured window: mean, p50, p90,
p99, p99.9, max, and a full percentile CDF.

### Throughput

```
applied_per_sec = applied_in_window / measured_window_sec
bytes_per_sec   = applied_per_sec * payload_bytes
```

`applied_in_window` counts entries whose apply callback fired inside the
measured window. It counts entries **applied** — replicated to a majority,
committed and delivered — not entries offered and not entries sent. Entries
offered but not yet applied when the window closes are not counted, which is
why an over-driven run shows latency growth rather than throughput above the
ceiling.

`bytes_per_sec` is log goodput at one replica. Bytes crossing the network are
roughly twice that, since the leader ships each entry to two followers.

Since there is no separate client tier, this is not "bytes clients receive".
The nearest true statement is "bytes of log the state machine accepted per
second".

### Capacity is the maximum over the sweep, not the unthrottled point

The obvious way to read capacity — run with the pacer off and see what happens
— is wrong, and this baseline is what shows it. In every single-group
configuration, offering without limit produces *less* completed work than
pacing just above the knee:

| configuration | unthrottled | best paced | at offered | understated by |
|---|---|---|---|---|
| p1/single/4096B | 12 357/s | 13 100/s | 14 900/s | 6% |
| p1/single/286208B | 279/s | 340/s | 338/s | 22% |
| p1/single/1048576B | 68/s | 80/s | 81/s | 18% |
| p6/single/4096B | 12 254/s | 12 657/s | 13 100/s | 3% |
| p6/single/286208B | 248/s | 283/s | 281/s | 14% |
| p6/single/1048576B | 54/s | 69/s | 65/s | 29% |

Unpaced offering fills the in-flight bound, queue depth explodes (p50 rises
into seconds), and throughput falls — congestion collapse. So the capacity
figure quoted anywhere below is **the maximum mean `applied/s` across all
points of a series**, unthrottled included.

## Results

### Capacity

| entry size | 1 partition, single group | 6 partitions, single group | 6 partitions, per-partition groups |
|---|---|---|---|
| **4 096 B** | 13 100/s — 53.7 MB/s | 12 657/s — 51.8 MB/s | **≥ 68 769/s — 281.7 MB/s** |
| **286 208 B** | 340/s — 97.2 MB/s | 283/s — 81.1 MB/s | **≥ 1 588/s — 454.4 MB/s** |
| **1 048 576 B** | 80/s — 84.3 MB/s | 69/s — 72.8 MB/s | **≥ 451/s — 472.9 MB/s** |

The `multi` column is a **lower bound**, not a measured ceiling: the offered
rates were calibrated in single-group mode and never drove those
configurations to saturation. See "Known gaps".

### Latency at the reference operating point

The rate each configuration is held at for the non-rate phases — about 0.85 of
the single-group ceiling for that entry size.

| configuration | offered/s | delivered/s | p50 (ms) | p99 (ms) |
|---|---|---|---|---|
| p1/single/4096B | 10 100 | 10 100.5 | 4.337 ± 0.102 | 7.537 ± 1.205 |
| p6/single/4096B | 10 100 | 10 100.4 | 4.511 ± 0.086 | 8.691 ± 0.844 |
| p6/multi/4096B | 10 100 | 10 099.8 | **3.015 ± 0.006** | **4.263 ± 0.024** |
| p1/single/286208B | 191 | 191.0 | 9.537 ± 0.253 | 21.117 ± 10.778 |
| p6/single/286208B | 191 | 191.2 | 21.692 ± 0.584 | 35.302 ± 5.531 |
| p6/multi/286208B | 191 | 191.0 | **7.503 ± 0.106** | **9.508 ± 0.743** |
| p1/single/1048576B | 55 | 55.0 | 17.434 ± 0.169 | 26.324 ± 0.948 |
| p6/single/1048576B | 55 | 55.0 | 70.557 ± 3.146 | 105.385 ± 3.684 |
| p6/multi/1048576B | 55 | 55.0 | 24.641 ± 0.106 | 33.809 ± 0.682 |

Every configuration delivers the offered rate exactly, so these are latencies
at matched load, directly comparable across rows.

### Group mode is the dominant design factor

At six partitions and 286 KB entries — the production shape — giving each
partition its own Raft group instead of sharing one:

| | one shared group | group per partition | change |
|---|---|---|---|
| p50 at 190/s | 19.5 ms | 7.5 ms | **2.6× lower** |
| p99 at 190/s | 32.6 ms | 9.5 ms | **3.4× lower** |
| capacity | 283/s (81 MB/s) | ≥ 1 588/s (454 MB/s) | **≥ 5.6× higher** |
| p50 unthrottled | 6 295 ms | 994 ms | 6.3× lower |

The single-group ceiling is **not** a network limit. On the same loopback, the
same machine, per-partition groups move 454 MB/s where one shared group moves
81 MB/s. What saturates in single-group mode is one Raft group's own
serialization — one recursive mutex, one replication fiber. An earlier note in
`run_sweep.sh` read the close agreement between 1 and 6 partitions as evidence
of a shared bandwidth ceiling; that reading is false, and the agreement is
explained by both configurations sharing the same single group.

### Noise floor, and what counts as a real difference

Ten trials at four points, same window and warmup as the sweep.

| point | metric | mean ± sd | CV | detectable at n=10 | at n=3 |
|---|---|---|---|---|---|
| 4 KiB, throttled 5 000/s | p50 | 3.560 ± 0.100 ms | 2.8% | 2.5% | 4.5% |
| 4 KiB, throttled 5 000/s | p99 | 5.000 ± 0.122 ms | 2.4% | 2.2% | 3.9% |
| 4 KiB, unthrottled | throughput | 12 275 ± 241 /s | 2.0% | 1.7% | 3.2% |
| **286 KB ×6 multi, throttled 190/s** | **p50** | **7.493 ± 0.073 ms** | **1.0%** | **0.9%** | **1.6%** |
| 286 KB ×6 multi, throttled 190/s | p99 | 9.156 ± 0.438 ms | 4.8% | 4.2% | 7.7% |
| **286 KB ×6 multi, unthrottled** | **throughput** | **1 600 ± 55 /s** | **3.4%** | **3.0%** | **5.5%** |

"Detectable" is the minimum detectable effect, `2.8 × CV / √n` — the standard
z-based constant for 5% significance at 80% power. A measured delta below it is
not a small regression; it is no information.

Throughput at a *throttled* point has a CV near zero for a trivial reason —
`applied/s` echoes the offered rate — and carries no information at all.

## Plots

- `raft-baseline-412c225a/plots/lattput.png` — median latency against achieved
  throughput, all nine configurations, log-log. The hockey-stick knee is
  visible for the six single-group series; the three `multi` series are flat
  across the whole paced range because they never reached saturation.
- `raft-baseline-412c225a/plots/cdf-prod-281.png` — latency CDF at 281 entries/s of
  286 KB, the production-like operating point, for the three configurations
  that have that point. This is the plot to show alongside the headline
  numbers.
- `raft-baseline-412c225a/plots/cdf.png` — latency CDF at each series' highest
  throttled point. Useful for tail shape under stress; not an operating point
  anyone would choose to run at.

## Known gaps

Stated so a reader does not over-read the numbers, and so the next measurement
knows what to fix.

1. **The three `multi` configurations have no measured ceiling.** Their
   offered-rate arrays were calibrated in single-group mode and top out at
   roughly half of what per-partition groups deliver, so the whole paced range
   sits below the knee: latency is flat and throughput simply tracks the
   offering. Their capacity figures are lower bounds. Fixing this needs a
   per-`(payload, group_mode)` rate array and a re-run of those series
   (~40 minutes for the 286 KB production configuration alone). Until then,
   the production configuration's regression signal is the unthrottled point
   plus flat-latency points, which is sufficient to catch a regression but does
   not locate the knee.
2. **One machine, loopback.** There is no network latency term and no NIC.
   Latencies here are lower, and achievable rates higher, than any real
   deployment. This baseline supports a *before/after* comparison on the same
   machine — the question actually being asked — not a prediction of deployed
   performance.
3. **Memory-only.** No disk write is in the path. A durable configuration
   would measure something different.
4. **Open loop.** Numbers here cannot be restated as "N clients saw X ms".
5. **Three trials** in the sweep. Enough to state a noise floor and catch a
   regression of a few percent; not enough to resolve a 1% change. The
   noise-floor points use ten.
6. **p99 at `p1/single/286208B`** carries a ±10.8 ms spread on a 21.1 ms mean —
   that point sits at its own knee and is bimodal across trials. Do not use it
   as a regression gate.

## Reproducing this baseline

```bash
git checkout 412c225a
make -j32                      # or: ninja -C build raft_bench
./scripts/raft_perf/run_sweep.sh --output raft_perf_output/rerun
python3 scripts/raft_perf/processing.py raft_perf_output/rerun/rate
```

The tree must be clean. `examples/raft_bench.sh` stamps each record with
`git rev-parse HEAD`, appending `-dirty` when `git status --porcelain` is
non-empty — and that check counts untracked files, so an unrelated scratch file
in the working tree is enough to mark a record. A record whose `commit` ends in
`-dirty` did not come from the commit it names.

Run nothing else on the machine while the sweep runs, including a second
sweep. Concurrent sweeps do not collide on ports, but they compete for exactly
the CPU and loopback bandwidth being measured, and nothing in the resulting
records reveals it.

The plot scripts need matplotlib ≥ 3.3, which is not installed system-wide on
zoo-003 and cannot be `pip install`ed into the system Python (PEP 668). Use a
virtualenv:

```bash
python3 -m venv /tmp/perfvenv && /tmp/perfvenv/bin/pip install matplotlib
/tmp/perfvenv/bin/python scripts/raft_perf/lattput.py <dir>/rate -o lattput.png
```

`processing.py` and `compare.py` are standard library only and need none of
this.

## Comparing a conversion against this baseline

```bash
# after the Rust conversion, on the same machine
./scripts/raft_perf/run_sweep.sh --output raft_perf_output/after
tar -xzf docs/performance/raft-baseline-412c225a/records.tar.gz -C /tmp
python3 scripts/raft_perf/compare.py /tmp/records/rate raft_perf_output/after/rate
```

`compare.py` exits 0 if nothing regressed beyond the threshold (5% by default)
and 1 if something did, reporting each metric's change beside the noise floor
for the trial counts actually used.

Read its output with two rules in mind:

- **Latency conclusions come from the throttled points**, where the offered
  rate is held fixed and queueing delay is free to move. The most sensitive
  single number available is p50 at `p6/multi/286208B`, offered 190/s: its
  noise floor is 1.0%, so three trials resolve a 1.6% change.
- **Throughput conclusions come from the maximum over the series**, not from
  the unthrottled point alone, for the reason given above.

A conversion that leaves both unmoved beyond those floors has not cost
performance, at this entry size, on this machine, at this scale.
