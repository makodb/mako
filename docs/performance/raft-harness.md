# The standalone Raft performance harness

Mako's Raft had no performance test. The only throughput numbers in the tree
came from `dbtest`, which measures transactions through the whole Mako stack
with Raft's cost folded invisibly into them. This harness measures Raft on its
own: a real three-process cluster on the production build, offered a controlled
rate of controlled-size log entries, reporting enqueue-to-apply latency and
applied entries per second, one structured record per run.

Built to `docs/plans/raft-perf-harness.txt`. That plan is the work order; this
document is the manual.

## The pieces

| Path | What it is |
|---|---|
| `src/deptran/raft/raft_bench.cc` | the driver — one process of the cluster |
| `examples/raft_bench.sh` | launcher — stands up three processes, produces one record |
| `scripts/raft_perf/run_sweep.sh` | sweep driver — many points, one output directory |
| `scripts/raft_perf/processing.py` | parser — a directory of records into plot-ready series |
| `scripts/raft_perf/compare.py` | the criterion — two record sets in, regression verdict out |
| `scripts/raft_perf/lattput.py` | the saturation curve: median latency against throughput |
| `scripts/raft_perf/plot_latency_cdf.py` | the latency CDF |
| `docs/plans/raft-perf-profile.txt` | what Mako actually submits to Raft, measured |

## Quick start

```bash
ninja -C build raft_bench

# One point.
examples/raft_bench.sh --out /tmp/point.json \
    --partitions 1 --payload-bytes 1024 --rate 5000 --duration-sec 10

# A few minutes, end to end, including the plots. On a healthy tree this
# exits 0; a non-zero exit means a run failed, and SUMMARY.md names it.
./scripts/raft_perf/run_sweep.sh --quick
python3 scripts/raft_perf/processing.py raft_perf_output/sweep_*/rate
python3 scripts/raft_perf/lattput.py raft_perf_output/sweep_*/rate -o /tmp/lattput.png
python3 scripts/raft_perf/plot_latency_cdf.py raft_perf_output/sweep_*/rate -o /tmp/cdf.png

# Did a change cost anything? Two sweeps, one verdict. Exits 1 on a regression.
python3 scripts/raft_perf/compare.py before/rate after/rate

# Did a change break anything? SIGKILL the leader 5 s into the load and
# require every survivor to report a gap-free, duplicate-free applied prefix.
# This produces NO record -- the process that would have written one is the
# process it kills -- so it is a correctness test, not a measurement point.
examples/raft_bench.sh --out /tmp/unused.json \
    --partitions 6 --group-mode multi --payload-bytes 1024 --rate 3000 \
    --duration-sec 20 --kill-leader-at-sec 5

# The full sweep. It prints its own runtime estimate first; see it without
# running anything:
./scripts/raft_perf/run_sweep.sh --dry-run
./scripts/raft_perf/run_sweep.sh
```

The plot scripts need `matplotlib >= 3.3` (for `Axes.set_box_aspect`). Nothing
else does; `processing.py` is standard library only, and prints a readable
table on its own, so a machine that runs the sweep does not need matplotlib.

**Run one sweep at a time on a machine.** Two concurrent sweeps do not collide
on ports — every run picks its own randomized base — but they do compete for
the CPU and loopback bandwidth that the numbers are measuring, so both sets of
results are wrong in a way nothing in the record reveals.

## Where the output lands

Nothing is committed. Records are measurement output, so they are written
outside version control and kept or archived deliberately.

| what | where |
|---|---|
| `run_sweep.sh` (default) | `raft_perf_output/sweep_<timestamp>/` under the repo root |
| `run_sweep.sh --output DIR` | `DIR`, **verbatim** — no timestamp is appended, so two sweeps into the same `DIR` share it |
| `examples/raft_bench.sh --out PATH` | exactly `PATH`, one file |
| the plots | wherever `-o` says; they are not written automatically |

`/raft_perf_output/` is in `.gitignore`, so a sweep never dirties the tree.
It is also never cleaned up. A record is ~5 KB and its three replica logs are
~12 KB, so a full 624-run sweep is a few megabytes of records and roughly
8 MB of logs — small, but it accumulates one directory per sweep. Prune old
sweeps yourself.

Inside a sweep directory, one subdirectory per phase (`rate`, `payload`,
`batch`, `groups`, `knee`), and per run three things:

```
rate/<commit>-p6-single-pb286208-b1-r190-t2.json    the record
rate/<commit>-p6-single-pb286208-b1-r190-t2.log     the launcher's output
rate/<commit>-p6-single-pb286208-b1-r190-t2.logs/   localhost/p1/p2 stdout
SUMMARY.md                                          run counts, failures, next commands
```

The filename carries the **commit**, so re-running a sweep into an existing
directory after a code change adds points rather than overwriting some and
leaving others stale. `processing.py` also groups on commit (and on host, log
level, in-flight bound and window length), so a directory holding two commits
plots as two clearly-labelled series rather than one averaged curve — it says
so on stderr when that happens.

Re-generate `SUMMARY.md` for an archived sweep with
`run_sweep.sh --phase summary --output <dir>`; it preserves the original
commit and start time and recounts from disk rather than stamping today's.

## What it measures

**Enqueue-to-apply latency**, per entry, on the leader: from the moment
`add_log_to_nc()` is called to the moment the leader's own apply callback sees
that entry. The enqueue time is written into the payload and read back in the
callback — the technique `src/mako/benchmarks/paxos_async_commit_test.cc`
already used. Both timestamps come from the same process's `steady_clock`, so
the difference needs no cross-clock correction.

**Applied entries per second** over a measured window that excludes a
configurable warmup prefix.

That path, in order, is: the driver's offer thread → `RaftWorker::submit_queue_`
→ `RaftWorker::SubmitLoop` → `RaftServer::Start()` (local append) →
`RequestReplication()` → the leader's heartbeat/replication fiber →
AppendEntries to a majority → commit → `apply_queue_` → the apply thread →
the callback.

## What it deliberately does not measure

- **Anything above Raft.** No Masstree, no STO concurrency control, no
  transaction execution, no client RPC. `dbtest`'s `agg_persist_throughput`
  (`src/mako/benchmarks/bench.cc:677`) is `n_commits / elapsed_sec` where
  `n_commits` counts *transactions*; a change confined to Raft moves it by an
  amount swamped by every layer above.
- **Network latency.** See the caveats.
- **Failure or partition behaviour.** This is a performance harness. It has no
  fault injection and makes no correctness claim.
- **Durability.** The branch this was built on is memory-only Raft;
  `MAKO_RAFT_SNAPSHOTS` is unset by default, so no snapshotting is in the path.

## Stating whether a change moved the number

This is what the harness is for. The Raft implementation is being converted to
Rust, and the conversion must be shown not to cost performance. A single
before number and a single after number cannot show that, because two runs of
the *same* build differ.

**Every metric is reported with its spread.** `processing.py` prints
`mean +- sd` across the trials at each point, and under each series a noise
floor:

```
=== p1/single/4096B/b1/repeat-throttled ===
        5000     10       5000.2 +-  0.9       3.606 +-0.132       5.100 +-0.237
             noise floor: tput CV 0.0%, p50 CV 3.7%  ->  detectable at n=10: ~0.0% / ~3.2%
```

The last line is the **minimum detectable effect**: `2.8 * CV / sqrt(n)`, the
usual z-based constant for 5% significance at 80% power. It is the smallest
change that this many trials can distinguish from noise. A measured delta
below it is not a small regression — it is no information. Raising `--trials`
lowers the bar as `1/sqrt(n)`.

**`compare.py` does the comparison.**

```bash
python3 scripts/raft_perf/compare.py before/rate after/rate
python3 scripts/raft_perf/compare.py before/rate after/rate --threshold 3
```

It matches points across the two sets by configuration — deliberately ignoring
the `[commit=...]` disambiguator that `processing.py` groups on, since the
commit is exactly what is being compared across — and for each of throughput
(higher is better), p50 and p99 latency (lower is better) reports the percent
change beside the pooled noise floor for the trial counts actually used. Each
comparison gets one of four verdicts:

| verdict | meaning |
|---|---|
| `within noise` | the delta is smaller than the noise floor; this run count cannot see it |
| `better` | moved the good way, beyond the floor |
| `worse, under threshold` | real but smaller than `--threshold` |
| `REGRESSION` | worse than `--threshold` *and* beyond the floor |

Points present on only one side are named and skipped rather than silently
dropped. Exit status is 0 if nothing regressed, 1 if something did, 2 on a
usage or data error — so it works as a gate.

There are no p-values. With three trials a t-test mostly reports the smallness
of n; the noise floor states the same thing without the false precision.

### The trap: a throttled point cannot show a throughput regression

At any offered rate below saturation, `applied/s` simply echoes the offered
rate — that is what throttling means. Its standard deviation across trials is
near zero, and a conversion that made Raft 20% slower would still show
`5000.2 +- 0.9` until it became slow enough to miss the pace entirely. The
throughput CV printed for such a point is tautological, not evidence.

So read the two metrics at different points:

- **Latency regressions** show up at throttled points, where the offered rate
  is held fixed and the queueing delay is free to move.
- **Throughput regressions** show up only at or above the knee. A comparison
  that covers only throttled points below the knee has not tested throughput
  at all.

### Capacity is the maximum over the sweep, not the unthrottled point

It is tempting to treat the unthrottled point (`offered = 0`) as "the
ceiling", since nothing is holding the offer rate back. The baseline data
shows that this is false for every single-group configuration measured:

| configuration | unthrottled | best paced | at offered |
|---|---|---|---|
| p1/single/4096B | 12 357/s | 13 100/s | 14 900/s |
| p1/single/286208B | 279/s | 340/s | 338/s |
| p1/single/1048576B | 68/s | 80/s | 81/s |
| p6/single/4096B | 12 254/s | 12 657/s | 13 100/s |
| p6/single/286208B | 248/s | 283/s | 281/s |
| p6/single/1048576B | 54/s | 69/s | 65/s |

Removing the pace does not reach capacity — it overshoots into congestion
collapse. Offering without limit fills the in-flight bound, the queue depth
explodes (p50 rises to seconds), and *less* work completes per second than
when the load is paced just above the knee. The unthrottled point understates
capacity by 3-29% here.

So the throughput number to compare is **the maximum mean `applied/s` across
all points of a series**, unthrottled included, not the unthrottled point
alone. Where that maximum lands also matters: if it is the top paced rate of
the array rather than an interior point, the array never reached saturation
and the series has no measured ceiling at all — only a lower bound. That is
the present state of all three per-partition (`multi`) configurations, whose
`RATES` arrays were calibrated in single-group mode and stop well below what
per-partition groups can deliver.

### Conditions the comparison assumes

The two sides must differ in the code and in nothing else. `processing.py`
enforces what it can — it refuses to average records that disagree on the
fields in `COMPARABILITY_FIELDS` (payload size, partition count, group mode,
batch size, log level, outstanding cap, host, commit) — but it cannot see the
machine's state. In particular:

- Measure both sides on the same machine, with nothing else running. See the
  warning above about concurrent sweeps.
- Prefer interleaving before and after over running all of one and then all of
  the other, if the machine is shared or long-running thermal drift is
  plausible.
- Keep `--trials` at 3 or more. `compare.py` flags comparisons that exceed the
  noise floor but rest on fewer than 3 trials rather than trusting them.

## Exit codes

A run that had to be discarded says so in its exit status, so a sweep or a CI
step does not silently count it. `examples/raft_bench.sh` propagates whichever
is worse: its own verdict, or the worst of the three replicas'.

| code | meaning |
|---|---|
| 0 | a measurement |
| 1 | no process wrote a record — nobody became leader, or the leader aborted |
| 2 | bad arguments, missing build, missing config |
| 3 | leadership was lost mid-window (`offer_rejected > 0`) |
| 4 | one process led some partitions but not all — a partial cluster |
| 5 | the run exceeded its wall-clock budget and was killed |
| 6 | nothing was applied inside the measured window |
| 7 | no leader anywhere: this process led nothing and applied nothing |
| 8 | **log integrity violation** — the applied stream lost, duplicated or reordered an entry, or applied one this harness did not write. A correctness failure, not a slow measurement. |

`scripts/raft_perf/run_sweep.sh` exits 1 if any run failed, and `SUMMARY.md`
names them.

## Reading a record

One flat JSON object per run, written by the leader only. Every key:

### Provenance

| key | meaning |
|---|---|
| `commit` | git HEAD when the run started, `-dirty` if the tree was modified. Set by the launcher via `MAKO_BENCH_COMMIT`; `"unknown"` if the driver was run by hand without it. |
| `date` | UTC, ISO 8601, when the record was written |
| `host` | `gethostname()` |
| `build_flavour` | CMake `MODE` (`perf`, `debug`, …), baked in at configure time |
| `cmake_build_type` | CMake `CMAKE_BUILD_TYPE` |
| `raft_test_coro` | whether `RAFT_TEST_CORO` was defined. **Must be `false` for a number you intend to believe** — see trap T1 below. |
| `raft_default_single_group` | whether `RAFT_DEFAULT_SINGLE_GROUP` was defined, i.e. what the group mode would have been without an explicit flag |
| `config` | the config files passed with `-f`, comma-joined |
| `proc` | `localhost`, `p1` or `p2` |
| `role` | always `leader` — only the leader writes a record |
| `label` | free-form; the sweep driver puts the phase name here |

### Inputs

| key | meaning |
|---|---|
| `partitions` | Raft groups driven. In the generated configs this is also the worker-thread count (trap T4). |
| `replicas` | recorded for provenance; not derived from the config |
| `group_mode` | `single` or `multi` — see below |
| `payload_bytes` | total entry size offered, including the 40-byte stamped header |
| `batch` | the fourth argument of `add_log_to_nc`. **Not entry coalescing** — see below. |
| `offered_rate` | entries/sec across all partitions that the pacer aimed for; `0` means unthrottled |
| `duration_sec` | requested measured-window length |
| `warmup_sec` | discarded prefix |
| `max_outstanding` | driver-side in-flight bound, **per partition** |
| `leader_wait_sec` | how long the process was willing to wait for leadership |
| `max_samples` | cap on retained latency samples per partition. Read it together with `samples_dropped`: a drop count means nothing without the cap it was measured against. |
| `follower_linger_sec` | how long a follower kept serving past the end markers |
| `log_level` | rrr log level in force (`0`=FATAL … `4`=DEBUG). Default 2. Raising it changes the number — see caveats. |

### Results

| key | meaning |
|---|---|
| `measured_window_sec` | the window actually achieved. Shorter than `duration_sec` means the offer loop stopped early. |
| `applied_per_sec` | `applied_in_window / measured_window_sec`. **The throughput number.** |
| `offered_per_sec` | the same for offers, so a pacer that could not keep up is visible |
| `offered_total` / `applied_total` | whole-run counts, warmup and leadership probes included on both sides. They should agree closely; `applied_total` materially below `offered_total` means Raft took entries it never applied. |
| `offered_in_window` / `applied_in_window` | counts inside the measured window. A large gap is a shortfall, not noise. |
| `offer_rejected` | offers `add_log_to_nc` refused because leadership was lost. **Non-zero invalidates the point**; the launcher exits 3 and `processing.py` drops the record. |
| `offer_stalled_sec` | seconds the offer threads spent blocked on `max_outstanding`, summed across partitions. Large relative to `measured_window_sec × partitions` means the run never offered what it was asked to. Past the knee that is the expected result; below it, the bound leaked because Raft accepted entries and then dropped them on a leadership flap, and the point is dead. `processing.py` names such records. |
| `leadership_changes` | leadership transitions this process was notified of. More than the initial election means the cluster flapped during the run. |
| `foreign_applied` | applied entries that were not this harness's (Raft's internal no-ops, mostly). Counted, and now also **fatal**: see the integrity block below. |
| `out_of_order` | breaks in this partition's applied sequence — the number of applied entries whose sequence number was not the previous one plus one. Zero on a correct run. |
| `gaps` | entries apparently skipped, summed: when an applied sequence number is ahead of the expected one, the shortfall is added here. |
| `duplicates` | applied entries whose sequence number had already been seen. |
| `probes_applied` | leadership probes (sequence 0) applied. Outside the ordered stream, counted so the other three are readable against the total. |
| `samples_used` / `samples_dropped` | retained latency samples, and how many exceeded `--max-samples`. Dropped samples cost percentile resolution only; throughput is counted separately and is always exact. |
| `latency_mean_us`, `latency_p50_us`, `latency_p90_us`, `latency_p99_us`, `latency_p999_us`, `latency_max_us` | enqueue-to-apply latency, microseconds, over in-window samples. Nearest-rank percentiles over the sorted samples. |
| `peak_outstanding` | the largest value `get_outstanding_logs()` returned during the run, sampled every 64th offer |
| `applied_per_sec_per_partition` | `applied_per_sec / partitions`, the analogue of bench.cc's `avg_per_core_persist_throughput` |
| `min_partition_applied_in_window` / `max_partition_applied_in_window` | the slowest and fastest partition. One straggler among six is invisible in the aggregate and diluted to a sixth of its weight in the pooled percentiles; these two make it legible. |
| `latency_cdf_us` | percentile → microseconds, 1..99 plus `99.9` and `100`. What the CDF plot draws; the raw samples are not written to disk. This is the **one** nested value in an otherwise flat record — the plan asked for "flat, no nesting", and a hundred `latency_p37_us` keys would be worse. `processing.py` special-cases it. |

### Log integrity: the one thing here that is a correctness check

Every offered entry carries a per-partition sequence number, numbered from 1,
in the header field the apply callback used to skip over. The callback reads it
back and compares against the last one it saw, which gives ordering, no-loss
and no-duplicate for that partition's applied stream in a single pass. Both
roles run it: a follower registers the same callback, so a follower's verdict
is evidence about **replication**, not just about the leader's local apply.

`out_of_order`, `gaps`, `duplicates` and `foreign_applied` must all be zero.
Any non-zero value exits 8 from both the driver and the launcher, and that
verdict is assigned last so no performance verdict can overwrite it. A number
measured over a log that lost an entry is not a slower number, it is a wrong
one.

What it deliberately does NOT catch is truncation at the END of the stream:
entries still in flight when the drain window closes, or an offer rejected
when leadership moves, leave no gap between two applied entries. That case is
what `offered_total` versus `applied_total` is for. The property is exactly
what makes the same check usable across a leadership flap
(`examples/raft_bench.sh --kill-leader-at-sec S`), where losing the dead
leader's uncommitted tail is legitimate and a gap in the committed prefix is
not.

### Two keys that mean less than they look like

**`batch` is not entry coalescing.** It sets `RaftWorker::batch_limit_`
(`raft_worker.cc:710`), a single member with last-writer-wins semantics across
every partition and every caller. All it does is bound how many already-queued
payloads `SubmitLoop` drains per acquisition of `submit_mutex_`
(`raft_worker.cc:1135-1143`); each drained payload still gets its own
`Submit()` → `RaftServer::Start()` → its own Raft log slot. It is lock
amortisation. Real wire-level coalescing happens later, on the leader's
replication path, bounded by `MAKO_RAFT_APPEND_BATCH_MAX_ENTRIES` (default 256,
`server.cc:553-560`) — which this harness does not sweep.

**`peak_outstanding` means different things in the two group modes.** In
single-group mode one `RaftServer` carries every partition, so all partitions
read the same process-wide number; in per-partition mode each partition reads
its own. The two modes' values are therefore not comparable with each other —
which is awkward, because comparing those two modes is what D5 exists for. Use
`applied_per_sec` and the latency percentiles for that comparison, not this.

**`peak_outstanding` is a trend indicator, not a queue depth.**
`get_outstanding_logs()` returns `worker->n_tot - raft_server->commitIndex`
(`raft_main_helper.cc:947-960`). `n_tot` counts successful `Start()` calls since
process launch; `commitIndex` is an absolute Raft log index that also counts
no-ops. It never observes `submit_queue_` at all — an entry waiting to be
submitted is invisible to it — and the read of `commitIndex` is unsynchronised.
The driver's own in-flight bound (`max_outstanding`, offered minus applied) is
the number that actually shapes the run.

## The three configurations, and why those three

Decision D5 of the plan. Not a sweep of partition counts; three shapes.

| partitions | group mode | why |
|---|---|---|
| 1 | single | the low-variance regression gate. One Raft group, one worker thread, nothing to contend. |
| 6 | single | what production runs. |
| 6 | multi | brackets the cost of the shared recursive mutex. |

All three are in `CONFIGS`, so every phase iterates all three.

Group mode selects how many `RaftServer` instances a process creates.

- **single** (`--group-mode single`, or `--raft-groups=single`): one
  `RaftServer` carries every partition. The other partitions' ports are served
  by stub servers, each with its own poll thread, all dispatching into that one
  `RaftServer` behind one recursive mutex — so the mutex becomes genuine
  cross-thread contention. This is the default: CMake `SINGLE_RAFT_INSTANCE` is
  `ON`, which defines `RAFT_DEFAULT_SINGLE_GROUP`.
- **multi** (`--group-mode multi`): one `RaftServer` and one poll thread per
  partition, no shared lock.

`raft_bench` always passes the mode explicitly rather than inheriting the
compile-time default, and records what it passed.

## Caveats — do not over-read a number

**Loopback means no network latency.** `config/1leader_2followers/raft*.yml`
maps `localhost`, `p1` and `p2` all to `127.0.0.1`. Three processes buy
address-space isolation, not network delay. Every number from this harness
**understates replication latency for a geo-distributed deployment**, which is
the deployment Mako exists for. Recovering that term needs multi-machine
deployment, which is out of scope here.

**There is a latency floor of roughly a millisecond, and it is polling, not
Raft.** The apply thread has no condition variable: when `apply_queue_` is
empty it sleeps a fixed 1 ms (`server.cc:1478`). And a replication wake that is
missed falls back to the heartbeat tick, 5 ms in production
(`HEARTBEAT_INTERVAL`, `server.h:1163-1165`; override with
`MAKO_RAFT_HEARTBEAT_INTERVAL_US`). At low offered rates the measured p50 is
dominated by those two constants. A measured 2-3 ms p50 on an idle loopback
cluster is that floor, not the cost of consensus.

**The unthrottled point's latency is Little's law, not a property of Raft.**
At `--rate 0` the only thing shaping the offer is `--max-outstanding`, so
latency converges on `max_outstanding × partitions / throughput`. Measured:
1 partition, 1 KiB entries, bound 4096 → 39 853 entries/s and a p50 of 100 ms,
which is 4096/39853 to three digits. Read the unthrottled point for its
*throughput*; its latency is an artefact of the bound you chose.

**The in-flight bound can leak, and `offer_stalled_sec` is how you see it.**
`add_log_to_nc()` returning true is not a promise that the entry will be
applied: it pre-checks leadership and queues the payload on
`RaftWorker::submit_queue_`, and the submit thread can find that leadership has
since moved and drop the entry with no counter anywhere
(`raft_worker.cc:769-771`). The driver's own in-flight accounting — offered
minus applied — then never gets that unit back. A leadership flap that recovers
inside the run therefore leaves the bound permanently short, throughput
collapses, and `offer_rejected` stays 0 because the *next* offer succeeds. The
symptom is a large `offer_stalled_sec` at an offered rate well below the knee;
`processing.py` names any record in that state. Past the knee a large
`offer_stalled_sec` is simply what saturation looks like — the record cannot
tell the two apart, only the offered rate can.

**`--max-outstanding` is per partition.** Six partitions at the same value put
six times as much in flight, so a 1-partition and a 6-partition unthrottled
point are not comparable on latency. It is a memory bound as much as a tuning
knob: `RaftWorker::submit_queue_` is an unbounded `std::deque` holding a *copy*
of every payload, so 4096 in flight at 286 KB would be 1.2 GB per partition.
`run_sweep.sh` scales it down as the payload grows.

**Log level changes the number.** The apply path emits one `Log_info` line per
applied entry (`[APPLY-LOGS] site=… applying index=…`, `server.cc:1400-1403`
region) and janus's static initialiser leaves the level at INFO
(`src/deptran/__dep__.h:110-115`). Measuring at INFO measures the logger, so
`raft_bench` lowers it to WARN before `setup()` and records the level it used.
Do not compare records with different `log_level`.

**Partition count equals thread count.** The generated configs derive both from
the same `N` (`config/1leader_2followers/raft_generator.py`), so
`raft6_shardidx0.yml` means six partitions *and* six worker threads. They cannot
be varied independently without regenerating configs.

**Do not build this on `RaftLab`.** There is a five-replica in-process harness
at `build_raftlab/deptran_server -f config/raft_lab_test.yml`, and it is
tempting because it is one process. It is compiled with `RAFT_TEST_CORO`, where
the leader no-op is compiled out and the heartbeat interval is 100 ms against
production's 5 ms. Its numbers describe a different system. Every record carries
`raft_test_coro` so this cannot be confused after the fact.

**Three runs minimum.** A single run is not a measurement. `run_sweep.sh`
defaults to `--trials 3`; `processing.py` reports the median throughput, takes
the latency and CDF from that same trial rather than mixing statistics from
different runs, and reports the spread as a population standard deviation.

**If Mako already has a capacity knob you need, use it.** CPU throttling exists
and is CI-tested (`ci/test_cpu_throttling_scaling.sh`, `MAKO_CPU_LIMIT` /
`MAKO_THROTTLE_CYCLE_MS`). Use that to vary capacity rather than inventing a
mechanism.

## The sweep axes

All declared as arrays at the top of `scripts/raft_perf/run_sweep.sh`. Change
them there, nowhere else.

- **`RATES`** — the saturation axis, one array per payload size because the
  achievable rate is bandwidth-bound and no single array brackets the knee for
  a 4 KB entry and a 1 MB entry at once. The shape is ported from jetpack's
  Raft concurrency array (dense around the knee, sparse above it), re-expressed
  as fractions of the measured unthrottled rate, with `0` (unthrottled) last.
  Re-derive the anchors on a new machine with `--phase knee`.
- **`PAYLOAD_BYTES`** — brackets the profile: 4 KiB, the measured p50 of a real
  TPC-C Raft entry (286 208 B), and 1 MiB.
- **`BATCH_SIZES`** — 1 and 400, the value `src/mako/sto/Transaction.cc` passes.
  Expect a flat curve; see the note above on what `batch` does.
- **`CONFIGS`** — the three shapes from D5, all of which every phase iterates.
- **`GROUPS_MODES` / `GROUPS_PARTITIONS`** — the `groups` phase's own axes,
  which compare the two group modes head to head at one partition count.
- **`FIXED_RATE_FOR`** — the rate every non-rate phase is held at, *per
  payload*: about 0.85 of that payload's measured unthrottled ceiling
  (10 000 / 190 / 55 entries/sec). One global constant cannot serve three
  payload sizes whose ceilings span two orders of magnitude — a single value
  chosen for the 286 KB knee would drive the 1 MB point three times past
  saturation, and the payload phase would then show a latency cliff at 1 MB
  that a reader would attribute to entry size.

## Where the numbers came from

`docs/plans/raft-perf-profile.txt` records what Mako actually submits: entries
of about 286 KB at the median with a long rare tail into the tens of megabytes,
roughly 50 per second per partition, and a `SubmitLoop` batch of 1.04. Those
measurements set the payload and rate axes above. That file also documents the
temporary instrumentation used to obtain them, which was removed afterwards.

## Not part of this harness

Raised rather than started, per plan section 12:

- A `dbtest` end-to-end cross-check that the standalone numbers predict real
  behaviour. Worth doing, as validation of the instrument.
- Multi-machine deployment, to recover the network-latency term.
- Any change to the Raft implementation, including the locking that
  single-group mode implicates.
- Partition counts beyond 1 and 6.
- Failure or partition injection.
