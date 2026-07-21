# Resharding in Production Distributed Databases — A Survey

How eight production systems handle the two questions our own §3.6
design hand-waves over: what happens to **in-flight requests** when
migration starts, and what happens to **new requests** that arrive
during migration. Grouped by underlying technique rather than by
system, because the same idea recurs.

Companion to `docs/mako-book.md` §3.6 ("Resharding (2PC-Style)") —
read that first for the Mako-side vocabulary, then come back here
for what production systems do.

## Pattern 1 — "Split is a metadata op; data isn't physically moved"

The cleanest pattern. A split commits one record (a Raft entry, or
a metadata transaction) and the children point at the same underlying
storage until eventual compaction. There is no migration window for
data because nothing moves; the cost is paid later, asynchronously,
when the children diverge.

### Then when *does* the data actually move?

The split is only step one. The actual data movement is a separate
**rebalance** step that happens later, scheduled by a balancer
process (CockroachDB allocator, TiKV's Placement Driver, YugabyteDB
master, FoundationDB data distributor) when it notices one node has
too much data or too much load. The rebalance is what physically
ships bytes between nodes.

The trick is that even rebalance doesn't block requests. It works in
three steps, all of them online:

1. **Add a learner replica on the target node.** This is a Raft
   membership-change log entry; the source range is still fully
   functional. Reads and writes continue against the existing
   replicas.
2. **Catch the learner up via Raft snapshot transfer.** The source
   leader streams a consistent snapshot (in CockroachDB this is a
   Pebble SST checkpoint; in TiKV/YugabyteDB, a RocksDB snapshot;
   in FoundationDB, a `fetchKeys` range-read transaction) plus any
   log entries committed during the transfer. Source keeps serving
   the entire time.
3. **Atomic membership change.** Once the learner is caught up,
   another Raft entry promotes the learner to voter and removes the
   old replica. This is the cutover — a single Raft commit that
   shifts ownership. Modern Raft (with joint consensus) makes this
   atomic; older versions did add-then-remove with a brief
   vulnerability window (the issue CockroachDB called out in #12768).

So the "data movement" that's missing from a metadata-only split is
not skipped — it's deferred and amortized across many independent
rebalance operations, each of which is itself non-blocking thanks to
Raft snapshot transfer + atomic membership change. The split-first /
rebalance-later separation is what lets these systems claim "splits
are instant."

A useful mental model: **splits create logical work units**;
**rebalances move them.** Yugabyte's hard-link-the-SSTs split is the
purest expression — at split time the two children share storage
verbatim; only later, when one child gets rebalanced to a different
node, do its bytes actually travel a wire.

### How does the split itself not interfere with ongoing requests?

The split is itself **a Raft log entry on the parent group**. The
leader proposes a special `AdminSplit` command containing the split
key. Once that entry is committed and applied via normal Raft
consensus, each replica's state machine atomically:

1. Carves out a new range descriptor for the right-hand side (RHS).
2. Initializes a new Raft group for the RHS with **the same set of
   replica nodes** as the parent (no data motion at this point).
3. Both children begin serving from that point forward.

Because the split is a Raft entry, it's strictly serialized with
every other write to the parent. Writes that committed before the
split are part of the parent's state; writes that arrive after the
split are routed to L or R based on key.

**Ongoing multi-key transactions:** A transaction whose intents
straddle the split key just becomes a multi-range transaction at
commit time. Cockroach/TiKV/Yugabyte already handle multi-range
commits via 2PC, so the transaction machinery doesn't need a
special "I-was-split-mid-flight" code path — its write intents
remain as MVCC writes in the underlying storage, and the txn
coordinator discovers at commit time that they now live in two
different Raft groups.

**The one small blip is on the client side:** a client whose
routing cache still says "range [0,100) lives at L" sends a request
for key 75 to L *after* the split commits. L returns an "outdated
range descriptor" error (CockroachDB calls this `NotLeaseholder` or
range-key-mismatch; TiKV calls it "stale epoch") that includes the
new RHS descriptor. The client refreshes its routing cache and
retries against R. Typical handoff is sub-millisecond.

There's **no critical section, no write pause, no snapshot transfer
at split time**. The data is still in the shared underlying
storage; both new Raft groups read and write through it; the
metadata flip is the only thing that happened.

### CockroachDB — range split

A range split is a single Raft entry on the source range; the new
range descriptor is written via a transaction. Because nothing
physically moves, **there is no migration window**. In-flight
transactions on the source range continue without interruption. New
requests for the upper half are routed via the meta range to the new
range descriptor once the split commits.

The hard problem in CockroachDB isn't splits — it's **rebalancing**
(moving a replica to a different node). Historically that wasn't
atomic: adding a replica and removing the old one were two separate
steps, leaving a window where a locality failure could cause
unavailability. Recent versions use Raft **joint consensus** to make
the membership change atomic.

### TiKV — region split + Raft membership change

Same idea: region split is Raft-atomic on the source region.
Rebalancing is separate, scheduled by PD (the Placement Driver). PD
adds a learner replica first, lets it catch up via Raft snapshot,
then promotes via Raft joint consensus. The data move happens via
the snapshot transfer; the cutover is a membership-change log entry.

### YugabyteDB — tablet split with parent → children pointing at the same SSTs

YugabyteDB's tablet split is the most explicit example: the parent
tablet hard-links its SST files to two children at a hash midpoint;
no copy. Provisional records (write intents) for in-flight
distributed transactions are duplicated into both children and
filtered by original key at apply time.

In-flight requests during cutover get the most interesting
treatment of any system we surveyed: the parent **rejects** new
requests with a "tablet split" error response that includes the two
child tablet IDs. The client transaction manager retries against the
appropriate child. This is the "**moved, retry**" pattern — clients
are first-class participants in the migration, which avoids any
critical section at the data layer.

## Pattern 2 — "Background copy, then atomic ownership flip"

When the data has to physically move (rebalancing for capacity, not
just splitting hot ranges), the dominant pattern is: copy in the
background while the source keeps serving, then a brief read-only
window for the destination to catch up and ownership to flip.

### MongoDB — moveChunk with critical section

Two-phase, explicit:

1. **Clone phase** — the recipient repeatedly calls `_migrateClone`
   on the donor; the donor keeps serving reads and writes; deltas
   tracked.
2. **Critical section** — once the recipient signals ready, the
   donor enters a "critical section" and **blocks reads and writes**
   while the final delta drains. The config server commits new
   ownership. Donor exits the critical section.

Writes during the critical section either pause or fail back to the
application. MongoDB 5.0 added the
`maxCatchUpPercentageBeforeBlockingWrites` knob to bound the
catch-up backlog before entering the critical section — a write-rate
governor that keeps the critical section short.

### DynamoDB — split-for-heat

Auto-detects hot partitions and splits at a smartly chosen sort-key
boundary (sized to spread the heat, not to halve the data). Marketed
as transparent, but the docs admit roughly **one second of
internal-server-error responses** for writes and strongly-consistent
reads to that partition at cutover. Eventually-consistent reads
keep working throughout.

The mechanism is essentially Pattern 2 with the critical section
hidden behind a retryable error code rather than a request pause.
Application clients with retry policies see only an extra round trip.

### Vitess — VReplication + SwitchTraffic

Multi-stage cutover designed for MySQL-shaped failure modes:

1. **VReplication** replicates source → target in the background
   while everything stays live.
2. **SwitchReads** flips read traffic to target (reversible).
3. **SwitchWrites** stops writes on the source primary, waits for
   the target to catch up to the source's last write position, then
   flips routing.

The clever piece is on the client side: **VTGate buffers queries**
during the brief read-only window so applications usually don't see
hard errors. Default catch-up timeout is 30 seconds. SwitchWrites
also starts a **reverse replication stream** so rollback is one
command — a level of operational maturity most systems lack.

## Pattern 3 — "Routing layer is the source of truth; data layer follows"

A third pattern, distinct enough to call out: the metadata layer is
itself a transactional store, and shard ownership changes are just
mutations through that store. The data layer doesn't need its own
critical section because the metadata transaction *is* the cutover.

### FoundationDB — shard boundary as transactional state

Shard boundaries (`serverKeys` / `keyServers`) are themselves keys
in the FDB key space, mutated by ACID transactions. To move a shard:
a transaction updates the boundary keys; the recipient storage
server calls `fetchKeys`, which reads the data through a normal
range-read transaction.

In-flight transactions read and write through the **transaction
logs**, which always know the current owner. There's no critical
section at the data layer — the metadata transaction commits the
cutover atomically with everything else.

### Spanner — directory moves

Directories (the unit of placement, finer-grained than tablets)
move between Paxos groups while client operations continue.
Coordinated by a long-running background task. Locks acquired at
the Paxos leader are durable across a leader change, but if any are
lost the participant aborts — so an in-flight 2PC transaction can
be killed by a directory move at an unlucky moment.

For full-instance moves between regions, leader handoff costs about
**3–4% throughput** during the handoff window.

## Common ground

Three observations cut across all eight systems:

### Atomic ownership flip via the consensus log

CockroachDB, TiKV, YugabyteDB, FoundationDB all converge on the same
trick: don't try to keep two copies in sync at the data layer; let
the metadata layer's consensus log be the cutover point. The flip is
a single log entry; clients either see "source owns it" or "new
owner owns it" — never both, never neither.

### Critical sections are real but bounded

MongoDB, DynamoDB, and Vitess accept a brief write-pause. They
manage it differently — MongoDB exposes it, DynamoDB hides it
behind a retryable error code, Vitess buffers at the proxy — but
the underlying pattern is the same: stop-the-world for a short
catch-up window. Total outage time for a single shard is
sub-second-to-30-seconds depending on the system.

### Nobody dual-writes at the data layer

This is the surprise. The natural-sounding option — "source
dual-writes to destination during migration, both stay in sync, no
critical section" — isn't where the consensus has landed. Production
systems prefer: keep source authoritative, copy to destination
asynchronously, then do a metadata flip that's either atomic (Raft
entry) or briefly read-only (critical section).

Two reasons emerge from the postmortems and design docs:

1. **Write amplification.** Dual-write doubles the source's write
   load during the very moment you're trying to move load off it.
2. **Divergence risk.** If source and destination disagree on the
   order of a concurrent write, you have a recovery question. The
   "let the destination read from source's authoritative state"
   approach side-steps this entirely.

### In-flight transaction handling falls into two camps

- **Finish on source** because source is authoritative until the
  flip (Cockroach, TiKV, Vitess, MongoDB).
- **Return "moved, retry" to the client** (YugabyteDB; FoundationDB
  partially, depending on which side of the new boundary a key
  lands).

The "moved, retry" pattern requires client-side cooperation but
keeps the server-side state machine simpler — there's no
"transaction-was-mid-flight-when-migration-started" case to handle.

## Architectural prerequisite: multi-Raft

Before reading "Implications for Mako," it's worth being explicit
about a prerequisite Pattern 1 quietly assumes:
**each node hosts many Raft groups, not one.**

In Cockroach/TiKV/Yugabyte, a typical storage node manages hundreds
or thousands of Raft groups simultaneously — one per range / region
/ tablet. This is called **multi-Raft**. Pattern 1's split mechanic
(metadata-only, atomic, no data motion) only works because the new
RHS Raft group can spin up on the same nodes that hosted the parent
— those nodes already host many groups; one more isn't structurally
different.

Multi-Raft requires nontrivial engineering: heartbeat coalescing
(one TCP message between two nodes carries heartbeats for all
groups they share), log batching, and a shared underlying storage
(one RocksDB / Pebble instance per node, holding all groups' data
distinguished by key prefix). TiKV's heartbeat coalescing was a
named feature when they introduced it — without it, thousands of
groups per node would drown in heartbeat traffic.

Mako today is **one shard = one Raft group, with replicas pinned
to specific hosts via `shard/<id>/replicas` in `__mako_config__`**.
A given host runs one Raft group (per shard role). Pattern 1's
"split is metadata-only" trick doesn't transfer directly, because
there's no infrastructure to host the newly created child group on
the same nodes as the parent without rearchitecting the storage
layer.

This means the closest architectural analog for Mako isn't
CockroachDB or TiKV — it's MongoDB.

## Why MongoDB is the closest architectural analog for Mako

MongoDB occupies the middle ground: **a small number of consensus
groups, each owning many key ranges.** That's exactly Mako's shape.

| MongoDB | Mako analog |
|---|---|
| Config server replica set | Shard 0 + `ConfigManager` |
| Chunk → shard map (in config server) | `shard/<id>/range_start` / `range_end` in `__mako_config__` |
| Replica set (one Raft group, owns many chunks) | Existing Mako shard (one Raft group, currently owns one static key range) |
| `moveChunk` clone phase | New cross-group "ship key range" RPC (does not exist in Mako today) |
| `moveChunk` critical section | Brief read-only window on source shard during final catch-up |
| Config server commits new ownership | `ConfigManager` writes new `range_start` / `range_end` and bumps `__version__` |
| `mongos` routing layer reads config | `ConfigWatcher`s on every node refresh from `__version__` |

The clean reading: **Mako already has most of MongoDB's resharding
architecture.** Shard 0 is the config server. `ConfigManager` is
the chunk-to-shard map. `ConfigWatcher` is the routing-refresh path.
What's missing is the bytes-mover — a `moveChunk`-style protocol
that:

1. Ships a key-range payload from a source shard's Raft group to a
   destination shard's Raft group via plain RPC, while source
   continues serving.
2. Coordinates a brief critical section on the source shard for
   the final catch-up (the source shard's Raft group transitions
   to read-only-for-that-range; reads against the migrating range
   either pause briefly or get a "moved, retry" error).
3. Commits the new range ownership via a `ConfigManager` write,
   bumping `__version__` so every `ConfigWatcher` updates routing.

There is no Raft membership change anywhere in this protocol —
both groups stay intact. What moves is **payload bytes**, shipped
between groups by an application-level RPC.

## The fork in the road: multi-Raft or cross-group transfer?

These are the two viable paths for true online resharding in Mako;
they correspond to two different system shapes. Both are
implementable; they have different costs.

**Path A — adopt multi-Raft (the Cockroach/TiKV/Yugabyte path).**
Each Mako host runs many Raft groups; shards become small and
numerous; a balancer process moves Raft groups between hosts when
load shifts. Resharding decomposes into the two operations
described above: split (metadata only, atomic Raft entry on the
parent group) and rebalance (add learner replica on destination,
catch up via Raft snapshot, atomic membership change to commit).

  - **Pros:** Automatic load balancing falls out for free. The
    well-understood, battle-tested approach. Pattern 1's
    sub-millisecond split-handoff property is unlocked. Speculative
    2PC machinery generally composes — every shard is still its
    own Raft group, just smaller.
  - **Cons:** Significant rewrite of the storage / consensus layer.
    Heartbeat coalescing, log batching, shared underlying storage
    across groups. The speculative-watermark math has to scale to
    thousands of groups per node (watermarks per group are fine,
    but the global watermark = `min(all groups)` needs to be
    efficient at this scale).

**Path B — keep one-shard-per-host and add cross-group transfer
(the MongoDB path).** Each Mako shard stays a single Raft group on
fixed hosts. A new `moveRange` RPC ships a key range between two
existing shards' Raft groups using a clone-then-critical-section
protocol; the cutover is a `ConfigManager` write on shard 0.

  - **Pros:** Smaller addition. Existing one-shard-one-group model
    is preserved. The pieces Mako needs to add are mostly
    self-contained: the `moveRange` RPC + critical-section
    coordination + `ConfigManager.UpdateShardRange()`. The
    speculative-watermark math doesn't change shape because the
    number of Raft groups doesn't change.
  - **Cons:** No automatic load balancing falls out — that has to
    be designed and implemented separately. The critical section
    on the source shard during catch-up is real (sub-second in
    MongoDB's experience, but it's there). Resharding remains a
    coarse-grained operator-driven operation rather than the
    fine-grained continuous self-tuning of multi-Raft systems.

The two paths are not mutually exclusive — many production
systems started with Path B (MongoDB itself, also Vitess) and
later added more sophisticated load-balancing machinery on top.
But Path A's "automatic continuous rebalance" is a very different
operational story from Path B's "operator triggers a `moveRange`
when they see a hot shard."

**The watermark trade-off is the deepest concern for Mako
specifically.** Speculative 2PC depends on a global watermark
`W = min(shard_i.local_timestamp)`. In Path A, the number of
shards grows by orders of magnitude, so the global-min computation
has to scale; the existing `ConfigWatcher`-driven aggregation
would not survive without rework. In Path B, the number of shards
stays constant, so the watermark math doesn't change shape —
which is a substantial implementation advantage even before
considering the rest.

A reasonable plan would be: pursue Path B first to unlock online
resharding without restructuring consensus, leave Path A as a
later option if/when load-balancing automation becomes the
critical need.

## References

- CockroachDB
    - [rebalancing tech notes](https://github.com/cockroachdb/cockroach/blob/master/docs/tech-notes/rebalancing.md)
    - [splitting / merging ranges](https://smazumder05.gitbooks.io/design-and-architecture-of-cockroachdb/content/architecture/splitting__merging_ranges.html)
    - [rebalances must be atomic (#12768)](https://github.com/cockroachdb/cockroach/issues/12768)
- TiKV
    - [PD scheduling introduction](https://github.com/tikv/pd/wiki/Scheduling-Introduction)
    - [region merge config](https://tikv.org/docs/7.1/deploy/configure/region-merge/)
    - [A deep dive into TiKV](https://pingcap.medium.com/a-deep-dive-into-tikv-b27989993d19)
- YugabyteDB
    - [automatic tablet splitting design](https://github.com/yugabyte/yugabyte-db/blob/master/architecture/design/docdb-automatic-tablet-splitting.md)
    - [retry post-split transactions (#4942)](https://github.com/yugabyte/yugabyte-db/issues/4942)
- Spanner
    - [OSDI'12 paper](https://www.usenix.org/system/files/conference/osdi12/osdi12-final-16.pdf)
    - [Move a Spanner instance](https://cloud.google.com/spanner/docs/move-instance)
- FoundationDB
    - [data distributor internals](https://github.com/apple/foundationdb/blob/main/design/data-distributor-internals.md)
    - [storage server shard boundary change](https://github.com/apple/foundationdb/wiki/Storage-Server-Shard-Boundary-Change)
- MongoDB
    - [moveChunk command](https://www.mongodb.com/docs/manual/reference/command/movechunk/)
    - [sharding internals](https://github.com/mongodb/mongo/wiki/Sharding-Internals/ea79e3172b630d02be84d20c804c25fd20fced28)
- Vitess
    - [how traffic is switched](https://vitess.io/docs/22.0/reference/vreplication/internal/cutover/)
    - [SwitchWrites](https://vitess.io/docs/archive/14.0/reference/vreplication/v1/switchwrites/)
- DynamoDB
    - [partitions and split-for-heat](https://aws.amazon.com/blogs/database/part-2-scaling-dynamodb-how-partitions-hot-keys-and-split-for-heat-impact-performance/)
    - [partitions and data distribution](https://docs.aws.amazon.com/amazondynamodb/latest/developerguide/HowItWorks.Partitions.html)
