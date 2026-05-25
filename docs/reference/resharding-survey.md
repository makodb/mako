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

## Implications for Mako

The dual-write approach floated in earlier design discussion isn't
where the industry has converged. For Mako's speculative-2PC model,
the closest fit is **Pattern 1 with a metadata flip via shard 0**:

- Source stays authoritative until the flip.
- Destination catches up via Raft snapshot transfer (we already have
  this primitive — `ReplicatedDB::CreateSnapshot` / `OnInstallSnapshot`).
- An atomic version bump on shard 0's `__mako_config__` flips
  routing for every node, because every node's `ConfigWatcher`
  polls the same `__version__` key.
- In-flight speculative transactions either drain before the flip
  (Vitess-style stop-and-wait, bounded by the watermark) or get a
  YugabyteDB-style "moved, retry" error if they straddle the
  cutover.

The watermark math stays single-source-of-truth per range at any
moment — which preserves the speculative-2PC correctness argument
in mako-book §4. Compare this to dual-write, which would force the
watermark logic to consider both source and destination clocks for
the migrating range, breaking the single-shard-clock invariant.

The YugabyteDB "moved, retry" pattern is the closest analog to our
existing speculative-2PC retry path — worth a deeper look before
finalizing §3.6.

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
