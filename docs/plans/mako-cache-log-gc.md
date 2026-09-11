# Local cache log reclamation

Status: G1-G3 implemented, G4 validation in progress as of 2026-09-09.
This work follows Milestone 1 and the backend-access fix. The starting revision
is `58120af0a`. Backward compatibility is not required. The
[validation report](../dev/mako-cache-log-gc-validation.md) records completed
checks and the remaining acceptance work.

## Decision

Make the materialized RocksDB state sufficient for recovery. Store the winning
HLC with every value and deletion marker, and store each worker lane's applied
position and maximum HLC in the same atomic batch as its row updates.

Use a configurable retention period, default five minutes. Once recovery state
is independent of old logs, periodically delete applied transaction records
whose HLC physical time is older than `now - retention`. This is the user's
2026-09-09 simplification and replaces the earlier zero-retention proposal.

STO still constructs each transaction record, Rust still applies it in the
background, and the in-memory record stays alive until application succeeds.
Write the full transaction log to RocksDB with its row updates, then retain
that log until it expires. Age selects records for GC; it does not substitute
for saving their recovery information.

This is an incremental checkpoint maintained by ordinary writeback. It does
not require copying the database to a separate snapshot directory. The logs
discussed here are Mako transaction records stored under private RocksDB keys.
RocksDB owns the lifetime of its internal WAL and SST files.

ACK remains volatile. WAL stays enabled with `sync=false`; GC adds no fsync,
`SyncWAL`, `FlushWAL`, or explicit memtable flush. A successful backend write
means applied, not synchronized to stable storage.

## Why the previous layout retained every log

At the starting revision, `ApplyCoordinator::apply` wrote a
complete transaction record and its timestamp-winning row mutations together.
There was no separately persisted, unapplied log tail.
Unapplied records are in the volatile worker queues.

Materialized values contained only user bytes, and a materialized delete
removed the row. The per-key winner timestamps lived only in memory. Recovery
therefore needed every log to rebuild:

- The current values and the winner index, including deleted keys.
- Each lane's dense local sequence and maximum applied HLC.
- The aggregate applied count and the process clock's recovery floor.
- A complete history against which to validate materialized data.

Deleting existing logs before replacing these obligations would break recovery.
The final validation obligation changes explicitly under this design: once
history has been removed, recovery validates the checkpoint and retained
suffix, but cannot independently audit every historical transaction.

## Persisted representation

Use a new private keyspace version and a format marker. Reject old or mixed
layouts with a rebuild-required error. Create the marker in an ordinary atomic
batch before opening admission on a new database. No in-place conversion or
automatic deletion of an existing database is part of this work.

Keep rows, lane metadata, and retained logs in the same RocksDB
database and column family initially. Use explicit, checked encodings.

| Entry | Contents | Purpose |
| --- | --- | --- |
| Format marker | Layout version, namespace identity | Reject incompatible or mixed recovery state |
| Materialized row | Put or tombstone, full 16-byte HLC, source lane/local sequence, value bytes for a Put | Recover state and reject delayed stale mutations |
| Lane metadata | `A`, `G`, `H`, retained logical bytes | Resume sequence allocation and locate retained logs |
| Retained log | Existing complete transaction encoding under its lane/local ID | Recent transaction history until expiration |

For each lane, including the untagged SingleProducer stream:

- `A` is the largest contiguous local sequence applied to RocksDB.
- `G` is the contiguous position through which application logs have been
  reclaimed. Require `0 <= G <= A`.
- `H` is the maximum HLC of all applied records in that lane, including records
  whose mutations were all superseded by newer timestamps.
- Precisely the suffix `(G, A]` remains as log keys. When every record in a
  lane has expired and been reclaimed, `G = A`.

Lane metadata is permanent even when a lane is idle or every user key has been
deleted. It covers all process worker slots used in this namespace, not just
currently initialized queues. Resume a lane at `A + 1`, with checked overflow;
never infer its next ID from the largest remaining log key.

Use checksums for format and lane metadata. Version the materialized row
envelope and checksum it by default. Payload-checksum policy must be explicit
and independently tested, as it is for the current log format. These encodings
run in the background and do not add fields to the native commit critical path.

The public applied count becomes `sum(A)` over all lanes. It continues counting
applied transactions after their log keys disappear. The recovery HLC floor is
`max(H)`, also checked against the timestamps in rows and retained logs. Neither
quantity denotes a globally complete prefix of HLC timestamps.

## Applying a transaction batch

Keep the existing exclusive apply coordinator. Its lock protects the winner
comparison, the backend write, and advancement of the in-memory index. With
exclusive backend ownership, this provides the required conditional update;
GC does not require a RocksDB merge operator or TransactionDB. A future second
writer would require a new protocol.

For a dense batch in one lane:

1. Validate lane continuity and stage timestamp winners using the current index.
2. Encode each winning Put as a versioned row. Encode a winning Delete as a
   versioned tombstone rather than physically deleting its timestamp.
3. Advance `A` and `H` even when no row wins. Append each full log and keep the
   prior `G`.
4. Submit row updates, lane metadata, and logs in one `WriteBatch`.
5. After a successful return, advance the in-memory winner index and lane
   counters, then release the queue records and capacity credits.

On an ambiguous error, retain the exact operation bytes and metadata targets.
Retry that identical batch before any other apply or GC batch. Do not rebuild
a retry against newly advanced lane metadata, release its queue records, or
advance counters twice. The same rule covers caught backend panics in unwind
builds. Release builds retain their existing process-abort policy.

A transaction remains a whole unit of backend application. Per-key timestamp
filtering may suppress mutations already superseded by later transactions,
exactly as it does now. GC never separates the remaining mutations of one
transaction across successful backend batches.

## Five-minute retention and periodic GC

Add `log_retention: Duration`, default 300 seconds, and `gc_interval: Duration`,
default 10 seconds. Keep settings fixed for an open cache initially; reopening
with another retention duration changes the next cutoff without a format
transition. Increasing retention cannot restore already deleted history.
A zero retention duration is allowed and uses the same expiration path; it does
not introduce a separate log format or skip-log write mode. Reject a zero GC
interval and durations that cannot be represented by the time calculation.

Schedule GC using a monotonic timer. Sample Unix wall time once per sweep and
compute `cutoff_us = now_unix_us.saturating_sub(retention_us)`. If sampling time
fails, skip that sweep and report the error. A record is old enough when
`record.hlc.physical_us < cutoff_us`. Do not subtract time from the encoded
128-bit HLC or use logical/origin bits as elapsed time. Retain records exactly
at the boundary until a later sweep.

Use the existing background writer to schedule bounded GC batches between
apply batches. Start at `G + 1` in each lane and select the consecutive expired
records through a target `T`, with `G < T <= A`. Stop at the first unexpired
record; never create a hole in the retained suffix. Tagged worker lanes already
have increasing HLCs. Apply this conservative prefix rule to the untagged
SingleProducer stream too, without assuming its historical HLC order.

Delete the exact log keys in `(G, T]` and update `G = T` and the retained-byte
count in one atomic batch. Preserve the current `A` and `H`. The coordinator
serializes GC with application and retains the exact operations unchanged
across ambiguous errors. A delayed record can already be older than five
minutes when it reaches RocksDB. It becomes eligible only after its own atomic
row/log/metadata application is confirmed, never while it is still queued.

Start with ordinary batched Deletes supported by `Blobs`; no C++ shim is
needed. Bound a batch by both key bytes and operation count, initially at most
1 MiB of delete keys and 1,024 records. A sweep must continue through multiple
batches, round-robin across lanes and interleaved with apply work, until it
exhausts that cutoff's eligible prefixes. Do not limit total reclamation to
1,024 records per ten seconds. Resume bounded scans from lane frontiers rather
than rescanning historical keys. Consider `DeleteRange` only if measurements
justify expanding the adapter.

The implemented adapter uses one inclusive iterator seek at `G + 1`, then
sequential reads up to the batch limit or first unexpired record. It checks
every expected log ID and releases the iterator before the deletion batch.
A missing record, truncated scan, or read error discards the staged deletes.
This replaces repeated per-log point lookups, which the first sustained run
identified as a collection bottleneck.

Wall-clock rollback or a future HLC delays expiration. A forward clock jump
makes more records eligible. Neither authorizes deleting recovery information:
the versioned rows/tombstones and lane metadata remain regardless of log age.
Five minutes is an age policy based on HLC physical time, not proof that no
older transaction remains in flight, nor a guarantee of five real elapsed
minutes after RocksDB application. Do not assume all workers finish within
the retention period.

Backend errors stop progress and use the same retry and bounded-queue
backpressure rules as ordinary writeback. Report retained bytes, oldest
retained timestamp, expired backlog, reclaimed records per second, GC errors,
and actual database disk use. A time window does not impose a byte limit:
storage depends on write rate, record size, and GC/compaction lag. RocksDB
compaction controls when obsolete bytes
leave SST files. GC must not delete RocksDB files directly or force a
compaction on the foreground path.

## Recovery

Keep the cache unavailable until all reconstruction and validation succeeds.
The recovered RocksDB state is the checkpoint; no online snapshot API is needed
while exclusive startup owns the backend.

1. Read and validate the format marker and all lane metadata. Reject unknown
   lanes, invalid origins, overflows, or `G > A`.
2. Iterate versioned materialized rows and tombstones, validating encodings,
   table IDs, checksums, and source positions covered by lane `A`. Rebuild the
   winner index from both row kinds. Check that row timestamps do not exceed
   the corresponding lane `H`. In a tagged lane, `H` belongs exactly to `A`;
   validate this identity even if its log has already been reclaimed.
3. Validate retained logs exactly in `(G, A]` for each lane. Reject a
   gap, an extra record outside that suffix, or an uncovered row source. Check
   retained mutations against the winner envelopes: the envelope must have
   at least that HLC, and equal versions must agree on identity and contents.
4. Floor the process HLC above every recovered lane `H` before accepting any
   new transaction, even if the database contains no live keys or log records.
5. Load current Put values into a fresh, private Silo table in bounded native
   transactions. Tombstones stay absent in the table and present in the winner
   index. Publish the cache only after loading completes successfully.
6. Initialize acknowledged/applied recovery counters from lane metadata and
   start new lane IDs at `A + 1`.

Extend the private backend adapter with a streaming key/value visitor or a
bounded iterator. The current collect-all-keys plus one `get` per key path
should not survive this rewrite. Avoid holding a second full set of values
while filling Silo; the winner index and live Silo dataset still consume RAM.

Loading a private checkpoint does not need to reenact every old transaction.
The checkpoint reflects complete backend batches, and no reader can observe a
partially loaded table. Do not describe this as historical HLC snapshot
support, causal-prefix recovery, or recovery of acknowledged memory queues.
Those are stronger contracts than the existing cache provides.

The old all-history duplicate-HLC and materialization audit cannot be retained
after deleting history. Continue checking identity conflicts among surviving
rows, lane metadata, and retained records where evidence remains. Format
checksums and atomic-batch tests replace reliance on permanent redundant logs;
they do not make arbitrary corruption reconstructible.

## Ordering and crash cases

A delayed Put at HLC 90 must not overwrite a Put at HLC 100, even if the log
for 100 was reclaimed. The versioned row retains 100. A delayed Put at 90 must
also remain suppressed after a Delete at 100; the tombstone retains 100.

Different lanes may contain lower HLCs still waiting in memory after a higher
HLC reaches RocksDB. Only confirmed lane continuity determines `A` and `G`.
The wall-time cutoff selects old records within that coverage; it does not
establish a globally applied HLC prefix. The greatest observed HLC, aggregate
applied count, and RocksDB sequence number are not substitutes for coverage.

The supported storage contract is a coherent RocksDB recovered state with WAL
enabled. Async writes may lose an unsynced suffix. A later GC operation cannot
be recovered while its prerequisite row application is selectively removed
from that coherent history. Per-batch atomicity alone would not establish this
property for an arbitrary custom backend.

Require ordinary ordered writes, and explicitly configure a recovery policy
that preserves a consistent point in time or fails. Do not use
`kSkipAnyCorruptedRecords`, repair tools that silently discard arbitrary
records, or WAL-disabled mode as a production GC configuration. The first
implementation can reject GC-enabled `Durability::None`; disposable benchmark
mode is a separate choice. A backend error during open leaves the cache closed.

If the process dies before an apply succeeds, its queue record may be lost
under the existing contract. If the backend recovered that batch, its rows and
lane metadata recover together. If GC fails before application, old logs remain
available. If GC applied but reported an error, its idempotent retry removes
the same keys. GC does not require knowing a passive last-synced WAL position.

## Tombstones and other consumers

Log reclamation does not reclaim application tombstones. Retain those in the
first implementation, including their winner-index entries. Otherwise a
delayed old Put could resurrect a deleted key. This means key churn still grows
tombstone storage, and live data must still fit in Silo. Neither value eviction
nor tombstone GC should be claimed complete by the log-GC release.

An eventual tombstone cleanup can run during exclusive recovery after all
retained log suffixes have expired and been reclaimed, so every lane has
`G = A`. All old process queues are gone, and lane metadata retains the HLC
floor. Finish validation and establish the floor before deleting tombstones or admitting new
transactions. Stage cleanup in bounded batches; retain metadata even after the
last tombstone is gone. Keeping a retained Delete log after removing its
tombstone would fail the retained-log validation on the next reopen, so that
combination is forbidden without a further recovery-format change.

Online tombstone cleanup needs an additional proven barrier. Close transaction
admission, resolve every transaction that could have allocated an old HLC,
drain all resulting records, and establish a clock floor before removing the
covered tombstones. Retained logs covered by the cleanup must also have expired
and been reclaimed. Either keep admission closed through all deletes, or
recheck each tombstone's captured version under the coordinator before deleting
it after admission resumes. The
existing `wait_applied()` only snapshots acknowledged lane positions and is
insufficient by itself. Do not add this barrier to every foreground transaction
just to ship application-log GC.

There are currently no replication, backup-stream, change-feed, or historical
read consumers of these private logs. If one is added, its recovery/retention
contract must be registered before enabling it. Five-minute retention is not
a replication log or an incremental-backup guarantee.

## Implementation milestones and acceptance

### G1. Persist recovery state, with GC disabled

Add versioned rows/tombstones, the format marker, lane metadata, and exact
metadata retry handling. Retain all logs temporarily. Differential tests must
show that recovery from materialized state equals the existing full-log oracle
under random lane interleavings, overlapping keys, multi-key commits, deletes,
and all-stale batches. Keep native logging, HLC allocation, and ACK unchanged.

### G2. Recover without historical logs

Implement streaming checkpoint recovery and cold Silo loading. In tests,
remove all logs with matching `G = A` metadata and reopen with the same state,
lane IDs, counters, and HLC floor. Verify delete-last-key, never-used lanes,
worker-slot reuse, retention changes across reopen, corrupt metadata, and partial
startup failures. No test may make the cache readable before loading finishes.

### G3. Enable five-minute retention

Once G1 and G2 pass, add the configurable 300-second retention period and
10-second GC interval. Continue appending transaction logs and reclaim expired
applied prefixes in bounded background batches. Test exact age boundaries,
multiple GC batches per sweep, lane fairness, and GC/apply error interleavings.
Verify that an ambiguous GC blocks conflicting metadata updates and that
counters advance exactly once. This completes implementation before G4's
release validation.

### G4. Crash, capacity, and performance acceptance

Use model tests that enumerate complete backend batch prefixes, including
apply, GC, and ambiguous success/error outcomes. Reopen each prefix and compare
against a timestamp-aware whole-transaction oracle. Include these adversarial
cases:

- Pause a low-HLC lane, apply and GC a higher-HLC Put or Delete elsewhere, then
  resume the older record. Its stale value must not win.
- Reclaim every log, delete the final live key, move wall time backward, and
  reopen. HLC allocation and lane IDs must still advance past recovered state.
- Interrupt multi-key application and GC before and after public RocksDB batch
  calls. The recovered state must reflect whole batches, never partial metadata
  and rows. Test apply-then-error and GC-then-error followed by identical retry.
- Exercise persistent write errors, ENOSPC, queue backpressure, reopen during
  partial cleanup, and corrupt envelopes/frontiers. No error may authorize GC
  beyond confirmed coverage or open a partially reconstructed cache.
- With an injected clock, verify retention just below, at, and above five
  minutes; clock rollback, forward jumps, future HLCs, retention changes on
  reopen, and records arriving after their expiration time. An old queued
  transaction must remain untouched until its own apply succeeds.
- Exercise reversed timestamps in the untagged stream. Stop at an unexpired
  record even when expired records follow it, preserving a dense suffix.
- Check both checksum policies, default five-minute and zero retention, and
  rejection of unsupported durability/recovery combinations.

Run a sustained 1/4/8/16/32-worker sweep on zoo-002 with boost disabled and the
chosen production configuration. Measure ACK and applied throughput, ACK p99,
queue occupancy/age, GC CPU, bytes written per transaction, retained log bytes,
actual disk usage through compaction, and restart time. Use enough work to fill
queues and run through several five-minute retention windows and compaction
cycles. Drain and reopen every accepted run.

For a fixed live key set and write rate, increasing the run duration tenfold
after steady state must keep retained history proportional to the retention
window plus measured GC lag, rather than total lifetime updates. Recovery must
not replay expired history. Physical disk use must settle within the measured
RocksDB compaction envelope. Record the envelope and overload limits; do not
claim an exact SST disk bound from time-based expiration.

Compare with the same source before GC on the same configuration. Report
foreground and backend costs separately; the earlier queue-abandoning ACK
benchmark is not this acceptance gate. Repeat the relevant sanitizer and
mutation gates for the changed recovery and retry paths.

## Files expected to change during implementation

- `crates/mako-cache/src/record.rs` or a new private checkpoint module for
  key classification, row envelopes, metadata, and decoding.
- `crates/mako-cache/src/writeback.rs` for atomic row/metadata application,
  exact retry operations, and GC coordination.
- `crates/mako-cache/src/writeback_set.rs` for lane seeds, retained-byte
  accounting, and bounded scheduling.
- `crates/mako-cache/src/lib.rs` for recovery, options, and status counters.
- `crates/mako-cache/src/recovery.rs` for streaming checkpoint validation and
  private Silo hydration without historical replay.
- `crates/mrx-core` and `crates/mrx-rocks` for private streaming reads and
  explicit supported RocksDB recovery options through the existing C API.
- The crash, mutation, recovery-oracle, and sustained benchmark suites.

## RocksDB contract references

RocksDB documents ordered atomic batches and the behavior of asynchronous
writes in [Basic operations](https://github.com/facebook/rocksdb/wiki/Basic-Operations).
Those guarantees are the storage assumptions here; this design adds no forced
sync. Its [WAL recovery modes](https://github.com/facebook/rocksdb/wiki/WAL-Recovery-Modes)
distinguish recovery to a consistent point from skipping arbitrary corrupt
records. GC requires the former or a failed open. These references were checked
on 2026-09-08; implementation must verify the deployed RocksDB build and options.
