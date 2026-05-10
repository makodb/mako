# High-Performance Raft Replication for Mako

## Abstract

Mako is a high-throughput replicated transaction system whose original
replication path used Multi-Paxos. This thesis asks whether Raft can replace
that Paxos backend without sacrificing the throughput properties that make
Mako useful for replicated OLTP workloads.

The answer is not a property of Raft alone. Mako's performance comes from a
pipeline: leader-side workers execute transactions, the replication layer
orders transaction logs, and follower replicas replay committed logs into
their local state. A Raft backend must preserve that pipeline rather than
becoming a serialized ordering or replay bottleneck.

This thesis presents a Raft integration for Mako and evaluates two Raft
topologies. Multi-Raft is the direct Paxos-shaped design: it preserves
partition-level replication parallelism by using one Raft group per partition.
Single-Raft is the consolidation design: it uses one Raft group per process,
which simplifies replication structure but moves pressure onto the apply and
replay path. The final Single-Raft design keeps Raft apply lightweight and
offloads heavy follower replay to a ReplayPool.

The implementation also adds preferred leader election, a Mako-specific
leadership placement mechanism. Mako's benchmark and transaction path submit
work through the leader-side process, so random Raft leader placement is not a
minor detail. Preferred leader election biases leadership toward the process
that owns leader-side transaction submission while preserving normal Raft
failover and safety rules.

The evaluation shows that Raft can stay in Paxos's performance class when the
topology and replay path are engineered around Mako's execution model. In the
no-disk `t=11` experiment, Single-Raft with ReplayPool reaches about 526K
transactions/s with follower replay progress near 499K transactions/s;
Multi-Raft reaches about 510K transactions/s, and Paxos reaches about 472K
transactions/s. Single-Raft without ReplayPool reports higher leader-side
throughput at about 593K transactions/s, but follower replay progress is only
about 143K transactions/s, showing why leader throughput alone is not enough to
validate the consolidated design. Under the Cloud-SSD FakeDisk model, all three
backends flatten near 340K transactions/s while recording about `7.49 GB`,
24.6K writes, and `733` bytes
per committed transaction. These storage counters provide the evidence needed
to explain the simulated persistence results without overclaiming real SSD
behavior.

## 1. Introduction

Replication is necessary for fault tolerance, but in a high-throughput
transaction system it can easily become the limiting stage. Mako was designed
around the idea that transaction execution, replication, and replay should
form a pipeline rather than one blocking step after another. Its original
replication backend was Multi-Paxos, which provides an ordered log of
committed transaction records that follower replicas replay into their local
state.

This thesis studies a practical systems question:

```text
Can Raft replace Paxos in Mako while preserving high throughput?
```

Raft is attractive because it packages leader election, log replication, and
leader changes into a protocol that is easier to understand and operate than
Paxos. That advantage matters only if Raft fits the performance shape of
Mako. Mako is not a small replicated key-value service. Its replication layer
interacts with worker threads, partitioned transaction state, follower replay,
leader placement, and a benchmark harness that expects one process to produce
leader-side throughput.

The central argument of this thesis is that Raft can replace Mako's Paxos
backend without leaving Paxos's performance class, but only when the
integration preserves Mako's pipeline. Multi-Raft gives a direct
Paxos-shaped Raft replacement. Single-Raft reduces replication structure, but
it is viable only when Raft apply remains lightweight and heavy follower
replay is moved to a parallel ReplayPool. Preferred leader election makes
Raft leadership compatible with Mako's leader-oriented submission path.

This work makes four contributions.

First, it adds a runtime-selectable Raft backend for Mako. The transaction
layer submits transaction logs through a shared replication interface, allowing
Paxos, Multi-Raft, and Single-Raft to run under the same benchmark harness.

Second, it implements preferred leader election for Mako. The mechanism biases
Raft leadership toward the process that owns leader-side transaction
submission, while preserving normal Raft voting, log matching, and failover.

Third, it evaluates Multi-Raft and Single-Raft as two different Raft
integration strategies. Multi-Raft preserves Paxos-like partition-level
parallelism. Single-Raft consolidates replication into one Raft group per
process and exposes the replay path as the central systems issue.

Fourth, it adds ReplayPool offload and source-tagged FakeDisk instrumentation.
ReplayPool makes Single-Raft's follower replay path parallel. FakeDisk
counters report bytes, writes, and normalized storage work so persistence
results are tied to measured work rather than only to throughput curves.

The rest of the thesis follows the structure of that argument. Chapter 2
describes the Mako pipeline and original Paxos path. Chapter 3 explains the
Raft backend and the topology choices. Chapter 4 presents preferred leader
election. Chapter 5 explains the Single-Raft replay bottleneck and ReplayPool.
Chapter 6 describes persistence and FakeDisk. Chapter 7 summarizes validation.
Chapter 8 evaluates the implementation. Chapter 9 discusses limitations and
tradeoffs. Chapter 10 relates the work to prior systems, and Chapter 11
concludes.

## 2. Background: Mako and the Original Paxos Path

Mako is a replicated transaction system for high-throughput OLTP workloads.
For this thesis, the relevant property is the replication pipeline, not every
detail of Mako's storage engine or concurrency control.

Leader-side Mako workers execute transactions and produce serialized
transaction logs. Those logs are submitted to a replication backend. Once the
backend commits a log record, follower replicas replay the committed record
into their local database state. The original backend was Multi-Paxos.

The original Paxos path matters because it gives Mako a performance shape.
Partitioned transaction work can map onto partitioned replication streams.
This shape is sympathetic to a multicore transaction system: multiple workers
generate work, and the replication layer can preserve parallelism rather than
forcing all work through one serial point.

The baseline path is:

```text
leader workers -> serialized transaction logs -> Paxos ordering -> follower replay
```

**Figure 1:** `doc/thesis/Thesis Tips and Figures/figures/manual/fig01_mako_baseline_replication.pdf`.
This figure shows leader-side workers, the common replication interface, the
original Paxos backend, and follower replay into local database state.

The ability to replace Paxos comes from Mako's replication boundary. The upper
transaction path does not need to know whether Paxos or Raft orders the log.
It submits a record to a helper layer, and the configured backend handles
replication. That shared boundary makes a fair comparison possible: workload,
thread counts, CPU accounting, and log extraction can remain constant while
the backend changes.

The thesis compares four modes:

| Mode | Meaning |
| --- | --- |
| Paxos | Original Multi-Paxos backend |
| Multi-Raft | One Raft group per partition |
| Single-Raft without ReplayPool | One Raft group per process, no replay offload |
| Single-Raft with ReplayPool | One Raft group per process, follower replay offloaded |

The last two modes must be interpreted carefully. Single-Raft without
ReplayPool exposes what happens when the consolidated log feeds a narrow
replay path. Single-Raft with ReplayPool shows the engineered design.

## 3. Raft as a Pluggable Mako Backend

The first goal of the Raft integration is compatibility. Raft should replace
Paxos behind the replication boundary without rewriting Mako's transaction
execution layer. Mako workers continue to submit transaction logs through a
shared helper interface, and the helper dispatches to Paxos or Raft based on
configuration.

The second goal is comparability. A performance result is meaningful only if
the outer experiment stays the same. The Raft backend therefore uses the same
benchmark driver, the same TPC-C workload, and the same extraction path for
leader throughput and worker CPU metrics.

The third goal is pipeline preservation. Raft is not placed after Mako as an
opaque logging service. It sits inside an end-to-end path:

```text
worker execution -> log submission -> consensus commit -> apply staging -> replay
```

The design choice is not simply "Paxos or Raft." The design choice is how Raft
is shaped around that pipeline.

### 3.1 Multi-Raft: Direct Paxos-Shaped Replacement

Multi-Raft is the conservative design. It keeps one Raft group per partition,
matching the broad shape of the original Paxos path. If Paxos had partitioned
replication streams, Multi-Raft gives partitioned Raft groups.

This design isolates a clean question:

```text
If Raft is given a topology similar to Paxos, can it support similar Mako throughput?
```

Multi-Raft has more Raft machinery than Single-Raft because each partition has
its own Raft state. The benefit is parallelism. Work from different partitions
does not share one Raft log. Multi-Raft is therefore the natural baseline for
arguing that Raft is a valid Paxos replacement in Mako.

### 3.2 Single-Raft: Consolidating Replication

Single-Raft asks a different question:

```text
Can a process use one Raft group for all partitions?
```

The appeal is simplicity. Instead of many Raft groups per process, one Raft
group carries commands that include partition identity. The Raft log orders
all commands, and callback routing sends each committed command to the correct
Mako partition.

That consolidation changes the pressure point. A single ordered path can feed
apply and replay faster than follower database replay can consume entries. If
committed entries are staged and replayed serially, increasing the number of
worker threads will not necessarily increase useful replicated progress. The
consensus layer may still order entries, while follower replay falls behind.

**Figure 2:** `doc/thesis/Thesis Tips and Figures/figures/manual/fig02_replication_topologies.pdf`.
This figure contrasts partition-level Paxos streams, one Raft group per
partition in Multi-Raft, and one process-wide Raft group in Single-Raft.

### 3.3 Raft Details Kept Behind the Systems Boundary

The implementation still needs ordinary Raft behavior: terms, votes, log
matching, leader election, AppendEntries, commit advancement, and safe
application. Those details are necessary for correctness, but they are not the
main thesis contribution. The body focuses on the Mako-specific engineering:
backend dispatch, leadership placement, topology choice, apply/replay
separation, metric extraction, and persistence accounting.

## 4. Preferred Leader Election for Mako

Preferred leader election solves a real mismatch between standard Raft and
Mako's execution model. Standard Raft can elect any replica whose log is safe
to lead. That is correct for Raft safety, but Mako's benchmark and transaction
path are leader-oriented: the throughput-producing process is expected to
submit transaction logs through the leader-side replication path.

If Raft elects a different process, the benchmark no longer measures the
intended system. The local workers may submit into a non-leader, or the result
may depend on accidental timeout timing. That is not a fair Paxos/Raft
comparison and not a clean deployment story.

Preferred leader election makes leadership placement explicit.

### 4.1 Motivation

In the benchmark configuration, `localhost` is the leader-side process. Its
workers execute transactions, build replicated logs, and submit those logs.
Follower processes such as `p1` and `p2` primarily replay committed logs. This
asymmetry matches the original Paxos setup.

Raft's default election mechanism does not know about this Mako role. It only
knows which servers are alive, which logs are up to date, and which candidate
can win a majority. Preferred leader election adds placement policy around
Raft without changing Raft safety.

The benchmark motivation is immediate, but the same design issue appears in
real deployments. A system may want leaders near clients, near transaction
coordinators, or on machines provisioned for leader-side CPU and network load.
Mako makes placement especially visible because leadership is connected to
transaction execution and log generation.

### 4.2 Design

The implementation assigns a preferred replica for each Raft group. During
startup, the preferred replica uses a shorter election timeout, while
non-preferred replicas use longer startup/failover timeouts. This biases the
common startup case toward the desired leader.

If the preferred replica is unavailable, normal Raft failover still works.
Another replica can time out, become a candidate, collect votes, and lead. The
system does not sacrifice availability to preserve placement.

When the preferred replica returns, leadership can transfer back only after
the preferred replica has caught up enough to lead safely. Preference is not
authority. The preferred replica still has to satisfy Raft's normal election
and log-safety conditions.

**Figure 3:** `doc/thesis/Thesis Tips and Figures/figures/manual/fig03_preferred_leader_election.pdf`.
This figure shows the transaction-submitting process as the preferred leader
and backup replicas as failover candidates.

### 4.3 Safety Boundary

Preferred leader election changes placement policy, not consensus safety. A
server still grants at most one vote per term. Candidates still satisfy Raft's
log up-to-date checks. Committed entries still require the normal majority
rule. A preferred replica cannot lead unless it wins a valid Raft election.

The additional engineering condition is safe catch-up before failback. A
non-preferred leader should not step aside merely because the preferred
replica is alive. It should transfer only when the preferred replica has the
committed prefix needed to lead safely. If the preferred replica is behind,
the current leader continues to serve.

This boundary is central to the contribution. Preferred leader election biases
leadership toward a Mako-compatible process. It does not guarantee that the
preferred replica is always leader, and it does not weaken Raft's safety
rules.

### 4.4 Validation

Preferred leader validation is placement and safety evidence, not a throughput
graph. The relevant tests cover preferred startup, preferred log replication,
no-op/watermark behavior, and failover/failback when available. In the thesis
body, these tests belong in a compact validation matrix rather than a
line-by-line walkthrough.

## 5. Single-Raft and ReplayPool

Multi-Raft is the direct replacement for Paxos, but Single-Raft is the more
interesting systems design. It reduces replication structure by placing all
partitions in one Raft group per process. That consolidation is attractive,
but it creates a new pressure point: one ordered log can feed one apply path
faster than follower database replay can consume entries.

The thesis separates three concepts that are easy to conflate:

| Stage | Meaning |
| --- | --- |
| Raft commit | consensus decides that a log entry is safe |
| Apply staging | `raft_apply` dispatches committed entries into Mako |
| Follower replay | Mako replays transaction effects into database state |

The final implementation does not make `raft_apply` perform all heavy replay.
Instead, `raft_apply` is kept lightweight. It stages committed entries and
invokes callbacks. The Mako callback copies the committed payload and enqueues
heavy work into ReplayPool. Replay workers then execute follower-side replay
in parallel.

### 5.1 The Bottleneck

Without replay offload, Single-Raft risks a fan-in bottleneck. Many Mako
workers can submit work, and one Raft group can order the work, but the
apply/replay path can become narrow. As worker count increases, follower
replay work accumulates. A benchmark may report high leader-side throughput
while followers make much less replay progress.

**Figure 4:** `doc/thesis/Thesis Tips and Figures/figures/manual/fig04_single_raft_replay_bottleneck.pdf`.
This figure shows many workers and one Raft group feeding a narrow
apply/replay stage.

The important claim is not that Single-Raft is fundamentally slow. The claim
is that Single-Raft changes where parallelism must be preserved. If replay is
kept serial, Single-Raft can look healthy from leader throughput alone while
follower state lags behind. If replay is parallelized, the design can recover
useful replicated progress.

### 5.2 ReplayPool Design

ReplayPool moves heavy follower-side replay out of the Raft apply staging
thread. The pipeline becomes:

```text
Raft commit -> raft_apply staging -> Mako callback -> ReplayPool queue -> replay workers
```

The benchmark configuration called `1:1` means:

```text
MAKO_REPLAY_THREADS = worker_threads
```

It does not mean one Raft group per worker. It does not mean one process per
worker. It does not mean one apply thread per worker.

**Figure 5:** `doc/thesis/Thesis Tips and Figures/figures/manual/fig05_replay_pool_pipeline.pdf`.
This figure shows Raft apply as a lightweight staging path and ReplayPool
workers doing the heavy follower replay.

### 5.3 Why ReplayPool Is a Systems Contribution

Consensus protocols are often evaluated as if state-machine application cost
were small. Mako makes that assumption unsafe. The replicated command is a
transaction log, and follower replay updates database state. That work can be
substantial.

ReplayPool is therefore not a cosmetic optimization. It is the mechanism that
allows Single-Raft to preserve Mako's multicore structure after consolidating
the consensus log. The evaluation supports this by comparing leader-side
throughput with follower replay progress and by showing that larger replay
pools let follower replay catch up to the leader-side rate.

## 6. Persistence and Simulated Disk

A production consensus system must persist enough state to recover safely
after crashes. For Raft, that includes term, vote, and log state. The Mako
Raft integration includes persistence hooks for Raft log storage and metadata.
This thesis discusses those hooks only as much as needed to explain the
evaluation. It does not claim a full production storage-stack evaluation.

The evaluation uses simulated persistence through FakeDisk. FakeDisk models a
queued device with configurable bandwidth and latency. It does not model every
detail of SSD firmware, filesystems, Linux block scheduling, RocksDB
compaction, or device-level writeback. Its purpose is narrower:

```text
When persistence is enabled, how much storage work did the benchmark generate?
```

The instrumentation reports total FakeDisk writes, total FakeDisk bytes,
source-tagged payload bytes, modeled service time, and wait-time diagnostics.
For the thesis body, the most useful metrics are aggregate bytes, write count,
and bytes per committed transaction. Max wait is retained as a diagnostic
counter, but it is a worst-case statistic and is expected to be noisier than
aggregate byte/write counters.

These counters are needed because disk throughput curves alone are not enough.
If Cloud-SSD-like persistence flattens as worker count increases, the thesis
must show whether the benchmark also generated measured persisted data and
write work. Otherwise the result could be dismissed as a benchmark artifact.

**Figure 6:** `doc/thesis/Thesis Tips and Figures/figures/manual/fig06_simulated_disk_proof_path.pdf`.
This figure shows Mako/Raft persistence work flowing through the simulated-disk
model, with counters for bytes, writes, bytes per transaction, and source
breakdown.

## 7. Correctness and Validation

The validation chapter answers one question:

```text
Why should the reader trust the performance results?
```

It should not become a test manual. The body needs a compact validation matrix
that connects test evidence to the thesis claims.

| Evidence class | What it checks | Thesis role |
| --- | --- | --- |
| Standalone Raft tests | election, agreement, failure, rejoin, concurrent starts | protocol sanity |
| Preferred leader tests | startup placement, safe failback, log replication | Mako-compatible leadership |
| Mako end-to-end tests | replicated transaction execution through the benchmark path | integration correctness |
| Persistence smoke tests | FakeDisk lines and CSV counters populate | disk-evaluation validity |
| Benchmark harness checks | throughput, worker CPU, replay, and disk metrics parse consistently | measurement credibility |

The most important validation rule is metric discipline. The thesis uses
`throughput_ops_sec` as the leader-side benchmark rate. The `replay_batch_p1`
and `replay_batch_p2` counters are follower-progress sanity counters. They
are essential for interpreting Single-Raft, but they are not the headline
metric.

## 8. Evaluation

The evaluation is organized around claims. Each graph answers one question and
is interpreted immediately.

All numeric claims in this chapter come from the frozen data snapshot in:

```text
doc/thesis/Thesis Tips and Figures/data_snapshot/
```

The snapshot records the exact CSV and Markdown files used to generate the
current graph set. This avoids quoting numbers from mutable `latest` symlinks.

### 8.1 Methodology

All backends run under the same outer benchmark harness. The workload is
TPC-C. The sweep varies Mako worker thread count from `t=1` to `t=11`. The
main no-disk comparison includes Single-Raft without ReplayPool, Single-Raft
with `1:1` ReplayPool, Multi-Raft, and Paxos.

The headline metric is `throughput_ops_sec`, extracted from the leader-side
benchmark process. Worker CPU metrics show whether transaction workers are
active. Replay counters show follower progress. FakeDisk counters are used
only for persistence experiments.

The evaluation uses these generated figures:

| Figure | Role |
| --- | --- |
| `fig07_no_disk_scalability.pdf` | no-disk leader throughput and follower replay progress |
| `fig08_worker_cpu_utilization.pdf` | worker CPU utilization |
| `fig09_replay_pool_matrix.pdf` | ReplayPool sensitivity across worker counts |
| `fig10_replay_pool_matrix_heatmap.pdf` | follower replay completeness across worker and replay thread counts |
| `fig11_disk_persistence_throughput.pdf` | no-disk, NVMe, and Cloud-SSD throughput |
| `fig12_simulated_disk_proof_cloudssd.pdf` | Cloud-SSD flattening and Simulated-disk proof |
| `fig13_cloudssd_bytes_per_txn.pdf` | normalized bytes per committed transaction |

### 8.2 No-Disk Scalability

This experiment asks whether Raft can stay in Paxos's performance class when
disk persistence is disabled. Multi-Raft tests the conservative hypothesis:
Raft should match Paxos when given a similar partition-level topology.
Single-Raft tests the consolidation hypothesis: one Raft group per process can
be viable if the replay path is engineered correctly.

**Figure 7:** `doc/thesis/Thesis Tips and Figures/figures/graphs/fig07_no_disk_scalability.pdf`

The left panel reports leader-side throughput. At `t=11`, Single-Raft without
ReplayPool reports about 593K transactions/s, Single-Raft with ReplayPool
reports about 526K transactions/s, Multi-Raft reports about 510K transactions/s,
and Paxos reports about 472K transactions/s. A throughput-only reading would make the no-ReplayPool
configuration look best.

The right panel shows why that reading is wrong. At `t=11`, Single-Raft
without ReplayPool reaches only about 143K transactions/s of follower replay
progress. Single-Raft with ReplayPool reaches about 499K transactions/s;
Multi-Raft reaches about 484K transactions/s; Paxos reaches about
450K transactions/s. ReplayPool therefore makes
Single-Raft much closer to a fully replayed replicated path. The result is not
that ReplayPool maximizes leader-side throughput. The result is that
ReplayPool makes the Single-Raft measurement credible as replicated execution.

This distinction is central to the thesis. Leader-side throughput is the
headline benchmark metric, but follower replay progress determines whether
the replicated system is actually keeping up with the committed history.

### 8.3 Worker CPU

The next question is whether the benchmark workers are doing real work or
whether the replication path causes the system to idle. Worker CPU does not
prove correctness by itself, but it helps interpret throughput results.

**Figure 8:** `doc/thesis/Thesis Tips and Figures/figures/graphs/fig08_worker_cpu_utilization.pdf`

The figure shows that worker CPU remains above one full core per active worker
through the high-thread part of the sweep. At `t=11`, the mean worker CPU is
about `166%` for Single-Raft with ReplayPool, `253%` for Multi-Raft, and
`222%` for Paxos. These numbers support the interpretation that the benchmark
is exercising Mako's transaction workers rather than measuring an idle
replication harness.

The CPU curves also explain why throughput comparisons should not be reduced
to one line. Multi-Raft and Paxos spend more CPU per active worker at high
thread counts, while Single-Raft with ReplayPool keeps the consolidated Raft
path competitive. The thesis claim is not that one backend is universally
faster. The claim is that Raft can remain in the same performance class when
the pipeline is preserved.

### 8.4 ReplayPool Mechanism

This experiment isolates the ReplayPool mechanism by varying both Mako worker
threads and `MAKO_REPLAY_THREADS`. The replay thread counts are `0`, `1`, `2`,
`4`, `8`, and `11`, and each configuration is swept from `t=1` to `t=11`.
The question is whether replay parallelism changes useful follower progress,
not whether more replay threads monotonically increase leader-side throughput.

**Figure 9:** `doc/thesis/Thesis Tips and Figures/figures/graphs/fig09_replay_pool_matrix.pdf`

The matrix shows that leader-side throughput stays in a relatively narrow band
at high thread counts, even when the replay pool is too small. At `t=11`,
throughput ranges from about 563K transactions/s to 584K transactions/s across
the tested replay-pool sizes. A leader-throughput-only graph would therefore
miss the actual mechanism.

The replay panels show the missing evidence. At `t=11`, zero replay threads
produce about 137K transactions/s of follower replay progress, or
about `23.5%` of the leader-side throughput. One replay thread is similar at
about 133K transactions/s and `22.8%` completeness. Two replay
threads improve follower progress to about 257K transactions/s, but
that is still only about `44.6%` completeness. Four replay threads reach
about 461K transactions/s and `81.5%` completeness. Eight and eleven
replay threads reach about 541K transactions/s and 534K transactions/s,
respectively, and both stay near `95%` completeness.

The same pattern appears before the largest endpoint. At `t=6`, zero and one
replay thread keep only about `40%` of the leader's committed history replayed
on the follower. Two replay threads reach about `79%`. Four, eight, and eleven
replay threads are all near `95%`. The threshold therefore grows with worker
parallelism: a small replay pool may look adequate at low thread counts, but
falls behind as the leader submits more committed work.

This is the reason the thesis uses the `1:1` ReplayPool configuration for the
main Single-Raft comparison. `1:1` means `MAKO_REPLAY_THREADS` equals the Mako
worker-thread count. It is a conservative setting that keeps follower replay
close to leader-side commit throughput across the tested sweep. ReplayPool is
therefore best understood as a replay-completeness mechanism, not as a
monotonic leader-throughput optimization.

**Figure 10:** `doc/thesis/Thesis Tips and Figures/figures/graphs/fig10_replay_pool_matrix_heatmap.pdf`

The heatmap is a compact way to read the same result. The dark low-completeness
region is concentrated where worker count is high and replay-pool size is too
small. The high-completeness region appears when the replay pool is large
enough to track the leader's committed history. This figure is the mechanism
evidence behind the statement that Single-Raft is viable only after replay
parallelism is engineered explicitly.

### 8.5 Simulated Persistence Throughput

This experiment asks how the backends behave when persistence cost is modeled.
The three conditions are no disk, an NVMe-like FakeDisk model, and a
Cloud-SSD-like FakeDisk model.

**Figure 11:** `doc/thesis/Thesis Tips and Figures/figures/graphs/fig11_disk_persistence_throughput.pdf`

The NVMe model stays close to the no-disk curves. At `t=11`, Single-Raft with
ReplayPool reaches about 545K transactions/s under NVMe, Multi-Raft about
509K transactions/s, and Paxos about 475K transactions/s. Those values are close to the no-disk
ordering and support the interpretation that this disk model does not dominate
the high-thread runs.

The Cloud-SSD model behaves differently. At high thread counts, all three
backends converge near 340K transactions/s. At `t=11`, Single-Raft
with ReplayPool reports about 340K transactions/s, Multi-Raft about
341K transactions/s, and Paxos about 341K transactions/s. This convergence is important: it
does not distinguish the consensus backends. It suggests that, under the
Cloud-SSD FakeDisk model, the storage path becomes the shared limiting factor.

This is a simulated persistence result. It should be read as evidence about
the modeled storage pressure, not as a claim about production SSD behavior.

### 8.6 Simulated-Disk Proof

The disk-throughput graph is only persuasive if it is paired with storage-work
evidence. The simulated-disk instrumentation provides that evidence by
recording the bytes and writes charged during the benchmark.

**Figure 12:** `doc/thesis/Thesis Tips and Figures/figures/graphs/fig12_simulated_disk_proof_cloudssd.pdf`

At high thread counts, the Cloud-SSD throughput curves flatten near
340K transactions/s while the FakeDisk counters report real measured storage
work. At `t=11`, all three backends write about `7.49 GB` through FakeDisk and
perform about 24.6K writes. The normalized value is about `733` FakeDisk
bytes per committed transaction.

**Figure 13:** `doc/thesis/Thesis Tips and Figures/figures/graphs/fig13_cloudssd_bytes_per_txn.pdf`

The normalized bytes-per-transaction graph shows that the storage work per
committed transaction is stable across backends and rises modestly at high
thread counts. This supports the explanation that the Cloud-SSD flattening is
consistent with measured persistence work. It does not prove real SSD
behavior; it proves that the controlled FakeDisk model was doing comparable
measured work across the three backends when the throughput curves converged.

### 8.7 Evaluation Summary

The evaluation supports four conclusions.

First, Raft can replace Paxos in Mako without falling out of Paxos's
performance class. Multi-Raft is the direct Paxos-shaped replacement and
tracks the original backend closely.

Second, Single-Raft can be competitive, but only if interpreted with follower
replay progress. Without ReplayPool, high leader-side throughput hides the
fact that followers replay much more slowly. With ReplayPool, follower replay
progress approaches leader-side throughput.

Third, worker CPU and replay CPU counters make the mechanism visible. The
benchmark workers are active, and ReplayPool moves replay work out of the
apply path.

Fourth, disk claims require Simulated-disk proof statistics. The Cloud-SSD model
flattens near 340K transactions/s, and that flattening is accompanied
by measured bytes, writes, and bytes per committed transaction. The thesis can
therefore explain the disk result without pretending that FakeDisk is a full
production storage evaluation.

Three no-disk `t=11` reruns had coefficient of variation below `0.6%` for
Single-Raft, Multi-Raft, and Paxos. This repeatability result is supporting
evidence, not a main thesis figure.

## 9. Discussion and Limitations

The results should not be read as "Raft is always faster than Paxos." That is
not the thesis claim. The claim is that Raft can replace Paxos in Mako without
falling out of the same performance class, provided the integration preserves
the pipeline that Mako depends on.

Multi-Raft is the conservative design. It keeps the original per-partition
parallelism and is easier to compare against Paxos. It is the right design
when preserving topology matters more than reducing replication structure.

Single-Raft is the consolidation design. It is attractive because it reduces
the number of Raft groups in a process, but it shifts pressure onto apply and
replay. The design becomes compelling only after ReplayPool moves heavy replay
off the apply path.

Preferred leader election is a Mako-specific contribution rather than a new
consensus protocol. It does not change Raft safety. It makes Raft leadership
compatible with the process that Mako expects to submit leader-side
transaction logs.

FakeDisk is useful but limited. It lets the thesis connect throughput changes
to measured bytes, writes, and normalized storage work. It does not model
every aspect of SSD firmware, filesystems, Linux block scheduling, or RocksDB
compaction. Production persistence claims would require a separate storage
evaluation.

The thesis also does not claim a complete failure-recovery performance
evaluation. Preferred leader tests and Raft correctness tests validate the
mechanism, but recovery throughput under failures is separate work.

## 10. Related Work

This thesis is closest to three lines of work: high-throughput replicated
transactions, consensus protocols, and storage-aware replicated logs.

ROLIS and Mako motivate the performance model. They show that replicated
transaction systems must treat replication as part of a multicore pipeline,
not as a small final step after execution. This thesis follows that lesson:
the question is not only whether Raft is correct, but whether Raft can fit
into the transaction/replay pipeline without destroying parallelism.

Paxos and Raft provide the consensus background. Paxos is the original backend
in Mako. Raft provides a leader-based protocol with election and log
replication. This thesis does not introduce a new consensus protocol. It
adapts Raft to a specific high-throughput transaction system and studies the
systems issues around that adaptation.

Work on leader placement and leadership transfer is also relevant. Preferred
leader election is related to operational leader placement, but its motivation
here is specifically Mako's leader-oriented submission path. The preferred
replica is favored only when Raft safety allows it.

Finally, persistence evaluation relates to work on durable replicated logs and
storage bottlenecks. The thesis uses FakeDisk to model disk pressure and
expose byte/write statistics, while keeping the claim narrower than a full
storage-stack evaluation.

## 11. Conclusion

Raft can be integrated into Mako as a high-performance replication backend,
but the consensus protocol is only one part of the system. Mako's throughput
depends on preserving parallelism across worker execution, consensus commit,
apply staging, follower replay, and persistence.

Multi-Raft shows the direct replacement path: keep the Paxos-shaped
per-partition topology and implement Raft behind the existing replication
interface. Single-Raft shows the consolidation path: reduce replication
structure, but then engineer the replay path so the apply thread does not
become the bottleneck. ReplayPool is the mechanism that makes the Single-Raft
design credible as replicated execution rather than only high leader-side log
submission.

Preferred leader election solves a separate mismatch between Raft and Mako.
Raft may elect any safe leader, but Mako's benchmark and deployment model
expect leader-side workers to submit transaction logs through a chosen
process. Preferred leader election biases leadership toward that process
while preserving ordinary Raft failover.

The final answer is therefore bounded but strong: Raft is a viable replacement
for Paxos in Mako when the integration respects Mako's pipeline. The successful
unit of design is not the consensus protocol alone, but the full path from
leader-side transaction execution through consensus, replay, leadership
placement, and persistence accounting.

## Appendix A. Figure Checklist

Final manual architecture figures:

1. `doc/thesis/Thesis Tips and Figures/figures/manual/fig01_mako_baseline_replication.pdf`
2. `doc/thesis/Thesis Tips and Figures/figures/manual/fig02_replication_topologies.pdf`
3. `doc/thesis/Thesis Tips and Figures/figures/manual/fig03_preferred_leader_election.pdf`
4. `doc/thesis/Thesis Tips and Figures/figures/manual/fig04_single_raft_replay_bottleneck.pdf`
5. `doc/thesis/Thesis Tips and Figures/figures/manual/fig05_replay_pool_pipeline.pdf`
6. `doc/thesis/Thesis Tips and Figures/figures/manual/fig06_simulated_disk_proof_path.pdf`

Generated evaluation figures:

7. `doc/thesis/Thesis Tips and Figures/figures/graphs/fig07_no_disk_scalability.pdf`
8. `doc/thesis/Thesis Tips and Figures/figures/graphs/fig08_worker_cpu_utilization.pdf`
9. `doc/thesis/Thesis Tips and Figures/figures/graphs/fig09_replay_pool_matrix.pdf`
10. `doc/thesis/Thesis Tips and Figures/figures/graphs/fig10_replay_pool_matrix_heatmap.pdf`
11. `doc/thesis/Thesis Tips and Figures/figures/graphs/fig11_disk_persistence_throughput.pdf`
12. `doc/thesis/Thesis Tips and Figures/figures/graphs/fig12_simulated_disk_proof_cloudssd.pdf`
13. `doc/thesis/Thesis Tips and Figures/figures/graphs/fig13_cloudssd_bytes_per_txn.pdf`

Manual drawing instructions live in:

```text
doc/thesis/Thesis Tips and Figures/README.md
```

## Appendix B. Evaluation Artifact Discipline

The clean result index is:

```text
results/thesis_results/
```

The frozen graph data snapshot is:

```text
doc/thesis/Thesis Tips and Figures/data_snapshot/
```

Use the frozen snapshot when quoting evaluation numbers. Later benchmark runs
may update symlinks under `results/thesis_results/`, but the thesis prose
should match the data used to generate the figures.

## Appendix C. Terminology

**Paxos:** the original Multi-Paxos replication backend in Mako.

**Multi-Raft:** one Raft group per partition.

**Single-Raft:** one Raft group per process, with partition identity carried in
each command.

**ReplayPool:** pool of Mako replay workers that execute heavy follower replay
outside the Raft apply staging thread.

**1:1 ReplayPool:** benchmark setting where `MAKO_REPLAY_THREADS` equals the
number of Mako worker threads.

**Preferred leader:** a configured Raft replica that the system biases toward
leadership when it is live and sufficiently caught up.

**FakeDisk:** a source-tagged simulated disk model that charges writes by
bytes, bandwidth, latency, and queueing delay.
