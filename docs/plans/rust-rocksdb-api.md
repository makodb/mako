# Rust RocksDB compatible transaction API

Status: complete and qualified locally on `rust-rocksdb-compatible-api`.
Baseline: `ec345ef0a`. Implement, qualify, update the book and commit locally.
Pushing is separate.

Provide the useful Rust `rocksdb` 0.25.0 API subset over Mako's distributed
OCC + 2PC coordinator. Every operation uses the existing prepare, decision,
recovery and migration path. RocksDB remains the durable data destination with
Masstree optionally enabled. This is source compatibility for documented
operations, not compatibility with RocksDB directory contents or all its types.

The signature reference is the community-maintained
[rust-rocksdb v0.25.0](https://github.com/rust-rocksdb/rust-rocksdb/tree/v0.25.0).
In particular, transactions use shared-reference methods and consuming commit.
We implement the facade in Rust without adding another native RocksDB binding.

## Supported contract

- `DB` and `OptimisticTransactionDB`, local `open`, `open_default`, `open_cf`,
  and a Mako `open_config` extension for a distributed shard and its peers.
- `transaction`, `get`, `get_for_update`, `put`, `delete`, configured column
  families, `multi_get`, atomic `WriteBatch`, commit and rollback. Dropping a
  transaction aborts it. A failed operation poisons that attempt.
- Forward/reverse iterators, start/end/seek modes and inclusive lower/exclusive
  upper bounds. Initially iterators eagerly collect a bounded, certified range;
  exceeding the bound returns an error, never a silently truncated success.
- Explicit write options reject WAL disabling and requests that the configured
  log cannot honor. Unsupported snapshots, savepoints, merge operators,
  dynamic column-family DDL and tuning options have no accepted no-op methods.
- Each configured raw table is a column family. Default operations address the
  `default` family. Handles are tied to one database, and foreign handles fail.
  Warehouse schemas require a separate coordinate API and are rejected here.
- All reads and ranges participate in Mako OCC certification. This is stronger
  than the binding's plain transaction reads. There is no MVCC snapshot claim.
  `get_for_update` records a read dependency; locks are acquired at prepare.
- Unknown commits return the transaction identity. Resolution completes the
  saved decision without replaying the body, including after reopening the
  coordinator. Unknown cleanup fences its stream. No hidden transaction retry.

The public handle hides routing, native workers and stream identities. Bounded
coordinator lanes own their affine workers. Concurrent transactions use separate
streams; exhausting the configured limit returns Busy without deadlocking a
caller that already holds a transaction. The native runtime retains sole
ownership of listeners, storage and shutdown. Both storage configurations use
the same facade.

## Stages and exit conditions

1. Build the backend-independent facade and contract tests. Check operation
   ordering, read-your-writes, binary keys, rollback/drop, batch atomicity,
   options, CF ownership, iterator bounds and errors, and uncertain outcomes.
   Include these tests in the bounded upper gate without native dependencies.
2. Connect the facade to the production coordinator. Add captured-epoch point
   routing and saved-identity resolution, retaining existing client behavior.
   Implement opening, lane ownership, shutdown and both storage mode options.
3. Qualify native use separately. Exercise both storage modes, distributed
   transactions and conflicts, restart, migration, local/Raft durability and
   saved-decision resolution. Verify that RocksDB-only builds omit Masstree.
4. Add a compiled example and a RocksDB compatibility section in the book with
   signatures, migration advice, options, limits and semantic differences.
   Run the upper gate within five minutes, relevant native/release checks and
   strict Clippy. Record evidence and commit reviewable stages.

Performance campaigns remain manual or weekly. Adapter tests do not replace
the isolated protocol proofs. New proof targets are needed only if this work
changes a protocol invariant; the facade delegates OCC and 2PC unchanged.

## Completion

- `bac254732` implements the facade, isolated tests, native adapter, captured
  point routing, saved-identity resolution, opening options and build gates.
  DB and transaction methods use the production coordinator in both modes.
  Absent-key Delete preserves an absence read and succeeds. Unknown results
  reserve their stream until resolution; repeated rollback cannot clear them.
- `2616b062f` separates replica opening from coordinator readiness. Followers
  can open before a quorum exists and remain available across failover. A
  three-replica test resolves the original transaction identity on a new leader.
- `43bdece32` corrects the existing replica-discovery fixture to retry only
  RouteChanged aborts within a deadline. Unknown outcomes still fail the test.
  The original optimized-suite failure is retained with the evidence.
- The book's Rust RocksDB compatibility section and compiled example document
  supported signatures, options, limits, durability and semantic differences.
  The example reopens one directory in RocksDB, Masstree and RocksDB modes.
- Final qualification passed 126 optimized cases, 28 focused API debug cases,
  48 RocksDB-only cases and eight executable audits. The earlier full debug
  suite passed 130 cases; the final fixture passed 15 combined and five
  RocksDB-only replication cases. ASan and TSan each passed five selected cases
  without startup retries or new suppressions. Strict Clippy, compile-fail
  documentation and the 16-source native C++ audit passed.
- The final upper gate passed in 158.959 seconds on 32 CPUs: 49 workspace tests,
  24 sharding tests, production proofs and 29 semantic negative controls. Its
  source-input hashes still match after the test-fixture correction.

All stages are complete. The [qualification record](../testing/rust-rocksdb-api-2026-11-25/README.md)
contains raw evidence and scope limits. Performance campaigns and pushing are
separate from this feature.
