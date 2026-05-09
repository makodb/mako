# High-Performance Raft Replication for Mako

## Abstract

Mako is a high-throughput distributed transaction system whose original
replication path used Multi-Paxos. This thesis asks whether Raft can replace
that Paxos path without losing the throughput properties that make Mako useful
for replicated OLTP workloads.

The answer is yes, but not by dropping in textbook Raft alone. A direct
per-partition Multi-Raft design matches the shape of Mako's original Paxos
replication layer and provides a clean baseline. A consolidated Single-Raft
design reduces replication complexity by placing all partitions in a process
behind one Raft instance, but it exposes a replay bottleneck: committed entries
can be ordered quickly while follower-side database replay remains expensive.
The final design separates lightweight Raft apply staging from heavy Mako
replay by offloading replay into a configurable `ReplayPool`. With one replay
worker per benchmark worker, Single-Raft recovers scalability and reaches the
same performance class as Multi-Raft and Paxos.

The thesis evaluates this design with no-persistence and simulated-persistence
TPC-C sweeps. The persistence experiments use a shared queued FakeDisk model
and report explicit byte/write counters so disk slowdowns can be tied to
measured storage work rather than inferred from throughput alone. The Raft
integration also adds preferred leader election so Mako can place leadership
on the process that owns leader-side transaction submission, while retaining
ordinary Raft failover when that preferred process is unavailable.

## 1. Introduction

Distributed transaction systems need replication for fault tolerance, but
replication can easily become the throughput limiter. Mako was designed to keep
transaction execution moving while replication is in flight. Its original
replication backend was Multi-Paxos, following the broader ROLIS/Mako design
idea that transaction execution, replication, and replay should be pipelined
rather than serialized behind one critical path.

This thesis studies a practical systems question:

> Can Mako use Raft instead of Paxos while preserving high throughput?

Raft is attractive because it packages leader election, log replication, and
leader changes into a clearer protocol structure than Paxos. That operational
simplicity is only useful if it does not cost Mako its central property: high
throughput under replicated transactional workloads. The core challenge is that
Mako is not a small key-value store. Its replication layer must interact with
worker threads, partitioned transaction state, follower replay, and the
benchmark harness used to measure throughput.

This work makes six contributions.

1. It implements a Raft backend that plugs into Mako's existing replication
   interface.
2. It adds runtime dispatch so the same Mako benchmark path can run Paxos,
   Multi-Raft, or Single-Raft.
3. It implements a preferred leader mechanism that makes Raft leadership
   compatible with Mako's leader-oriented transaction submission path while
   preserving normal Raft failover.
4. It evaluates a direct Multi-Raft design that mirrors Mako's per-partition
   Paxos structure.
5. It evaluates a consolidated Single-Raft design and fixes its replay
   bottleneck with a parallel `ReplayPool`.
6. It adds disk-proof instrumentation so simulated persistence results report
   bytes, writes, and modeled wait time.

The main lesson is that consensus protocol choice is only part of the story.
Raft can replace Paxos in Mako, but the surrounding execution and replay path
must be engineered to preserve parallelism.

## 2. Background: Mako and the Original Paxos Path

Mako executes OLTP transactions over partitioned in-memory state and uses
replication to order committed transaction logs across replicas. The upper
transaction path submits a serialized log entry to an atomic broadcast layer.
Once that entry is committed, Mako applies or replays the entry so each replica
advances through the same logical history.

The original replication layer used Multi-Paxos. For the purpose of this
thesis, the important Paxos property is not the full protocol mechanics, but
the topology it gives Mako: partitioned transaction work can be mapped onto
partitioned replication streams. That shape matters because Mako's throughput
depends on keeping worker-side execution, replication commit, and follower
replay from collapsing into one serial bottleneck.

Mako's replication interface makes this replacement possible. The benchmark and
transaction code call a shared helper API. At runtime, the helper dispatches to
the selected backend. This means Paxos and Raft can be compared under the same
outer benchmark harness, workload, CPU accounting, and result extraction.

This thesis uses that shared harness to compare three backends:

- **Paxos**: the original Multi-Paxos path.
- **Multi-Raft**: one Raft group per partition, matching the Paxos-shaped
  baseline.
- **Single-Raft**: one Raft group per process, with partition identity carried
  inside each command.

## 3. Raft as a Pluggable Mako Backend

The first design goal is compatibility: Raft should replace Paxos without
rewriting Mako's transaction layer. The implementation meets this goal through
a runtime dispatch layer. Mako submits transaction logs through the same helper
interface, and the helper calls the Paxos or Raft implementation selected by
configuration.

The direct Raft design is **Multi-Raft**. It keeps one Raft worker and one Raft
server per partition. This preserves the same broad topology as the original
Paxos path: partitioned transaction work maps to partitioned replication work.
Multi-Raft is therefore the cleanest test of whether the Raft protocol and Mako
integration can compete with Paxos when given similar parallelism.

The implementation must preserve three boundaries:

1. **Submission boundary**: Mako workers submit log entries without depending on
   the concrete consensus protocol.
2. **Commit boundary**: the consensus layer decides when a log entry is safe.
3. **Replay boundary**: Mako applies committed entries in partition order while
   respecting its watermark and deferred replay rules.

Standard Raft details such as terms, votes, log matching, and leader election
belong in the appendix unless they explain a Mako-specific design choice. The
body should focus on how Raft fits into a high-throughput transaction system.
One Mako-specific choice is leader placement: during Raft setup, the local
benchmark process is configured as the preferred leader for each relevant
partition. This keeps the Raft path aligned with Mako's original leader-side
submission model, where the throughput-producing process submits transaction
logs and the other replicas primarily replay committed work.

## 4. Single-Raft and Replay Offload

Multi-Raft preserves parallelism, but it keeps many Raft groups alive. Each
partition has its own Raft state, service path, and heartbeat behavior. A
natural systems question is whether a process can use one Raft instance for all
partitions.

Single-Raft answers that question by collapsing all partitions in a process
onto one Raft worker and one Raft server. Partition identity is still preserved
inside each replicated command, so callback routing can dispatch the committed
entry to the correct Mako partition. This design reduces replication structure,
but it creates a new risk: if the single apply path also performs expensive
database replay, throughput will flatten as worker count increases.

The final implementation separates these responsibilities.

- The Raft consensus path orders and commits entries.
- The `raft_apply` thread performs lightweight staging and callback dispatch.
- The Mako callback copies the committed log payload.
- `ReplayPool` workers perform heavy follower-side replay on `replay_*`
  threads.

This distinction is essential. The current design should not be described as
"one apply thread does replay." The apply thread is a staging point. The
expensive Masstree replay work is offloaded.

The benchmark setting called **1:1** means:

```text
MAKO_REPLAY_THREADS = worker_threads
```

It does not mean one Raft group per worker, one process per worker, or one
apply thread per worker.

The evaluation must show both halves of the story:

1. Single-Raft without enough replay parallelism flattens.
2. Single-Raft with a 1:1 replay pool scales with the reference backends.

## 5. Preferred Leader Election for Mako

Preferred leader election is not only an operational convenience in this
system. It is the bridge between Raft's dynamic election model and Mako's
leader-oriented execution path.

### 5.1 Motivation: Mako Submits Work to Leaders

Mako's benchmark and transaction path are asymmetric across process roles. The
`localhost` process is the throughput-producing role: its worker threads run
transactions, build replicated transaction logs, and submit those logs through
the leader-side replication path. The follower processes mainly consume
committed logs and replay them into their local database state.

That asymmetry is natural for the original Paxos setup, where the configured
leader role is stable and known to the benchmark harness. It becomes a systems
problem when replacing Paxos with Raft. Standard Raft can elect any caught-up
replica as leader, depending on random timeouts and scheduling. Correctness is
preserved, but the wrong leader placement makes the local Mako workers submit
into a process that is no longer the leader, or forces the benchmark to depend
on accidental election outcomes. In that situation, a throughput result can
measure orchestration mismatch rather than the cost of Raft replication.

Preferred leader election addresses that mismatch. The design biases Raft
toward electing the process that Mako expects to own leader-side submission.
For the benchmark configuration, that means `localhost` should become the Raft
leader when it is live and sufficiently up to date, while `p1` and `p2` remain
available to take over if the preferred process fails.

This is also useful outside the benchmark. A deployment may want leaders near
clients, near colocated coordinators, or on machines provisioned for
leader-side CPU and network load. Those placement goals matter more in Mako
than in a simple replicated key-value service because Mako couples leadership
to transaction execution, log generation, and cross-partition coordination.

### 5.2 Design: Bias, Failover, and Failback

Raft includes leader election, but ordinary randomized elections do not
guarantee that the leader is placed where Mako wants it. The implementation
adds a preferred leader setting to each Raft server. During setup, the helper
selects the site whose locale corresponds to the local leader role and installs
that site as the preferred leader for the partition. In Multi-Raft this happens
per partition. In Single-Raft the single worker uses the preferred site for the
consolidated group.

The mechanism has three behaviors.

First, it biases startup election. The preferred replica uses a shorter
election timeout. Non-preferred replicas use longer timeouts during startup, so
they give the preferred replica the first practical chance to become leader.
This keeps normal Raft elections, but makes the common startup outcome
deterministic enough for Mako's harness.

Second, it preserves failover. If the preferred replica is down or cannot
communicate with a quorum, another replica can still time out, request votes,
and become leader. The preferred leader mechanism does not disable ordinary
Raft availability.

Third, it supports failback. When a non-preferred replica is leader, it monitors
whether the preferred replica has caught up. Once the preferred replica is safe
to lead, the current leader initiates a transfer by notifying replicas and
stepping down, allowing the preferred replica to start an election promptly.
The thesis should describe this as a controlled leadership handoff, not as a
new commit protocol.

### 5.3 Safety Boundary

The preferred leader mechanism changes leader placement policy, not Raft's
safety rules. The vote-granting rules are unchanged. The log matching check is
unchanged. The commit rule is unchanged. A preferred replica still has to win a
normal Raft election before it can lead.

The critical extra condition is catch-up before transfer. A non-preferred
leader may transfer only when its replication state shows that the preferred
replica has reached the committed prefix required to lead safely. This prevents
the system from moving leadership to a stale replica merely because that
replica is preferred. If the preferred replica is unavailable or behind, the
current leader continues serving until normal Raft behavior changes the term.

The thesis should make this boundary explicit because it is the difference
between a safe systems extension and an unsafe shortcut. Preferred leader does
not guarantee that the preferred replica is always leader; it guarantees that
the implementation continually biases leadership back to that replica when
Raft's own safety conditions allow it.

### 5.4 Validation

Preferred leader validation should be presented as placement and safety
evidence rather than as a throughput graph. The dedicated tests cover three
questions:

- **Startup placement**: the configured preferred replica becomes leader and
  non-preferred replicas remain followers when the preferred replica is live.
- **Log replication after placement**: entries submitted through the preferred
  leader replicate to all replicas and trigger the expected leader/follower
  callbacks.
- **No-op and watermark behavior**: leadership placement does not break the
  no-op and replay synchronization behavior that Mako relies on before normal
  transaction logs flow.

The body should include this summary and one compact validation row. Detailed
test scripts, timing constants, and callback traces belong in the appendix.

## 6. Persistence and FakeDisk

Raft safety requires durable term, vote, and log state. The implementation
contains RocksDB-backed persistence hooks for consensus metadata and log
entries. The thesis should describe these hooks briefly, but it should not
claim a full production storage evaluation unless that evaluation is actually
run.

The performance evaluation uses a FakeDisk model for simulated persistence. The
model provides a shared queued device with configurable bandwidth and latency.
The important thesis-facing improvement is explicit accounting:

- total fake-disk writes,
- total fake-disk bytes,
- Mako payload bytes,
- Raft log bytes,
- Raft metadata bytes,
- modeled service and wait time.

These counters allow disk results to be explained with measured storage work.
Without them, a Cloud-SSD throughput flattening result is plausible but not
well defended.

## 7. Correctness and Validation

The validation section should answer one question: what evidence shows that the
Raft integration behaves correctly enough to trust the performance results?

Use a compact matrix rather than a test-by-test manual.

| Evidence class | What it checks | Thesis role |
| --- | --- | --- |
| Standalone Raft tests | election, agreement, failure, rejoin, concurrent starts | protocol-level sanity |
| Preferred leader tests | deterministic startup placement, safe failback, log replication after placement | validates Mako-compatible leader placement |
| Mako end-to-end tests | replicated transaction execution through `dbtest` | integration correctness |
| CI scripts | repeatable build and process orchestration | regression protection |
| Benchmark smoke tests | throughput extraction, replay counters, FakeDisk counters | evaluation validity |

The body should not walk through every helper, script, and config file. Those
details belong in appendices.

## 8. Evaluation

The evaluation is organized around claims.

### Claim 1: Raft can match Paxos when given comparable parallelism.

Compare Paxos, Multi-Raft, and Single-Raft with 1:1 ReplayPool under no-disk
conditions. The main metric is leader-side `throughput_ops_sec`. CPU columns
explain whether workers, replay threads, or apply threads are the limiting
resource.

Expected figure: no-disk scalability graph over worker threads.

### Claim 2: Single-Raft exposes a replay bottleneck without replay offload.

Use the four-way no-disk sweep:

- Single-Raft without replay pool,
- Single-Raft with 1:1 ReplayPool,
- Multi-Raft,
- Paxos.

The expected result is a visible gap between no-pool Single-Raft and the
ReplayPool version. The interpretation should focus on the critical path:
consensus can commit entries, but follower replay must also scale.

Expected figure: four-way bottleneck and fix graph.

### Claim 3: ReplayPool size explains the recovery in Single-Raft scalability.

Run a sensitivity sweep at fixed worker count, preferably `t=11`, varying
`MAKO_REPLAY_THREADS` across `0,1,2,4,8,11`. This directly supports the 1:1
choice instead of treating it as a magic setting.

Expected figure: throughput versus replay-pool size.

### Claim 4: Disk results are explained by measured storage work.

Use the focused disk proof sweep at `t=1,6,11` for:

- no disk,
- NVMe-like FakeDisk,
- Cloud-SSD-like FakeDisk.

Report throughput alongside:

- `fake_cluster_total_bytes`,
- `fake_cluster_total_writes`,
- average bytes per fake-disk write,
- bytes per committed transaction,
- `fake_max_wait_us`.

The thesis should say that Cloud-SSD flattening is consistent with measured
storage work. It should not imply the FakeDisk model is a complete substitute
for production SSD measurements.

### Evaluation Artifacts

The thesis should use fresh result directories produced on the current commit
when available. Historical May 1 results can be used for intuition, but final
disk claims should come from the new proof sweep because it includes byte/write
counters.

## 9. Discussion and Limitations

Multi-Raft is the conservative Raft replacement for Paxos. It keeps
partition-level replication parallelism and is easier to compare directly with
the original Paxos architecture.

Single-Raft is attractive when reducing replication structure matters, but it
needs replay offload. Its main lesson is that a simpler consensus topology can
move the bottleneck into the application callback path unless replay is
parallelized.

FakeDisk provides controlled disk-class pressure, not a complete storage-stack
benchmark. It models shared bandwidth, fixed latency, queueing, and charged
bytes. It does not model every behavior of a real SSD, filesystem, block
device, or RocksDB compaction path.

The thesis should also be explicit about what it does not claim:

- it does not claim Raft is always faster than Paxos;
- it does not claim Single-Raft is always preferable to Multi-Raft;
- it does not claim full production recovery evaluation unless additional
  recovery experiments are run;
- it does not treat follower replay counters as leader throughput.

## 10. Conclusion

Raft can serve as a high-performance replication backend for Mako. The direct
Multi-Raft design demonstrates that Raft can fit Mako's existing replication
shape. The Single-Raft design shows that replication topology can be simplified,
but only if the replay path is engineered carefully. Offloading heavy replay to
`ReplayPool` restores scalability and makes Single-Raft competitive with the
reference backends. Finally, disk persistence results must be supported by
measured byte/write statistics, not just throughput curves.

The broader systems lesson is that replacing a consensus protocol in a
high-throughput transaction system is not only a protocol exercise. It is an
end-to-end pipeline exercise: execution, consensus, apply staging, replay, and
persistence all have to preserve parallelism.

## Appendix A. Raft Protocol Reference

This appendix should contain the textbook Raft material currently spread across
the old thesis notes: terms, votes, RequestVote, AppendEntries, log matching,
commit index, and safety properties.

## Appendix B. Test Details

This appendix should contain the detailed standalone test descriptions, config
files, and CI script walkthroughs.

## Appendix C. Build and Configuration Reference

This appendix should document build directories, runtime flags, YAML config
fields, port ranges, and sweep commands.

## Appendix D. Persistence and Recovery Details

This appendix should describe `LogStorage`, RocksDB integration, recovery
manager behavior, and snapshot support. The body should cite only the parts
that are used in the evaluation.

## Appendix E. Figure Checklist

| Figure | Claim | Source |
| --- | --- | --- |
| Mako/Paxos baseline path | establishes original architecture | draw from docs and source |
| Multi-Raft vs Single-Raft topology | explains design alternatives | draw from implementation |
| Preferred leader placement | explains why Mako needs deterministic Raft leadership | draw from implementation and tests |
| Replay bottleneck before pool | explains why Single-Raft can flatten | four-way sweep |
| ReplayPool pipeline | explains the fix | draw from implementation |
| No-disk scalability | Raft reaches Paxos-like throughput | non-persistence sweep |
| Four-way bottleneck/fix | no-pool vs 1:1 pool | `overnight_four_way.sh` |
| ReplayPool sensitivity | justifies 1:1 setting | `sweep_replay_pool.sh` |
| Disk proof table | explains persistence flattening | focused disk proof sweep |
| Validation matrix | summarizes correctness evidence | thesis test docs |

## Appendix F. Writing Rules

- Start each section with a claim.
- Explain only mechanisms that support that claim.
- Put source inventories and helper walkthroughs in appendices.
- Avoid generic claims like "production-grade" unless backed by evidence.
- Do not describe current `raft_apply` as heavy replay.
- Do not define 1:1 as one Raft group per worker.
- Do not use `replay_batch_p1` or `replay_batch_p2` as leader throughput.
- Do not overclaim persistence or snapshot completeness beyond the measured
  artifacts.
