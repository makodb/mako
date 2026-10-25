# Production migration metadata: implementation plan

**Approved and implemented scope: metadata persistence only.** Primary MBTA/STO
storage and application TP remain unchanged. Retained startup can inspect
migration bookkeeping; it cannot restore data service or resume a migration.

The [end-to-end design](../../architecture/sharding-migration.md) explains the
algorithm and the stronger conditional durable library. This document records
the selected integration plan, its implementation boundaries, and its acceptance
evidence. Full production row/TP recovery was explicitly not selected.

## 1. Required behavior

- Persist coordinator plans, generations, nonce bindings, decisions, receipts,
  and administrative reply bookkeeping before exposing successful mutations.
- Persist successful participant controls and transfer intent/result bookkeeping
  before acknowledging those operations; native epoch pins remain volatile.
- Retain an exact committed journal prefix across process restart. Never
  interpret a storage error as absence or silently replace retained state with
  genesis.
- Fence uncertain persistence. An uncertain Commit must not become an Abort
  merely because its reply was lost.
- On retained startup, permit historical admin inspection but refuse data
  admission, new migrations, capture, and control/copy/cleanup.
- Do not implement `DurableEngine` with no-op callbacks or treat newly loaded
  initial rows as recovered post-migration data.

The metadata remains useful even when data is unavailable: a completed nonce
retains its generation/outcome, and an interrupted migration remains pending
rather than disappearing or being guessed aborted.

## 2. Fixed design choices

- Use private online pre-copy while the source serves, then publish Frozen
  before a cut on the existing STO clock and drain retained oldest worker pins
  through effects, unlocks, engine cleanup and terminal RPC cleanup. Join/fence
  pre-copy by generation/round before the complete exact final scan, including
  destination-only deletes. No transaction spans the moving range or both owners.
- Use RocksDB's local atomic WriteBatch for journal entry plus head. No new
  single-key-only journal protocol or application transaction engine is needed.
- Keep fixed owners, one active migration, append-only history, and retained
  fences. No compaction, changing membership, or automatic rebalance.
- Keep the existing non-replicated migration restriction. Local RocksDB metadata
  does not enable migration alongside Raft/Paxos replay.
- Make persistence opt-in with a stable directory and explicit first
  initialization. Without a metadata directory, the existing live-only mode
  remains available.
- Use Rust for the adapter and runtime logic. The small unsafe boundary calls
  the installed RocksDB C API directly; no primary-engine rewrite is introduced.

## 3. Implementation sequence and result

### Step 1: owned production metadata backend

[`rocks_metadata.rs`](../../../src/cluster/rocks_metadata.rs) implements the
existing [`DurableKv`](../../../src/cluster/durable_kv.rs) byte-map contract:

- Own the database, native options, write batches, pinned reads, error buffers,
  and lifetime writer lock. The lock survives close/reopen settlement and rejects
  a competing opener. No raw handle or cloneable backend escapes.
- Distinguish `Found`, `Absent`, and `Io`, including present empty values.
- Submit ordered mixed put/delete batches with WAL enabled and synchronous
  completion. The journal entry and head are one local atomic batch.
- Return `Committed` only on successful synchronous completion. Proven
  non-submission returns `NotCommitted`; a post-submission error returns
  `Unknown` and fences reads/submissions.
- Settle a fenced backend through actual close/reopen, strict WAL recovery,
  synchronization, and recovered-state flush while retaining the writer lock.
  Never blindly resubmit an uncertain batch.
- Explicit initialization requires an absent role directory; retained open
  requires existing storage and never creates missing state. Synchronize the
  namespace and ancestor directory creation.

The adapter uses RocksDB's supported options parser for WAL manifest verification
on Docker's RocksDB 8.9.1. Unsupported configuration is an error, not a reason to
silently disable verification. CMake exports the native Rust library's RocksDB
link dependency to its consumers.

### Step 2: coordinator persistence without a second protocol

[`metadata_coordinator.rs`](../../../src/cluster/metadata_coordinator.rs) owns the
optional backend and reuses the checked
[`DurableCoordinator`](../../../src/cluster/durable_coordinator.rs).

- Bind the retained store to exact versioned deployment/catalog identity.
- Apply native operations privately and expose them only after journal success.
- Recover the coordinator from committed bytes, retaining nonce intent and
  routing/decision metadata. Remove the runtime's parallel volatile admin
  request map; the actor's retained plan is the nonce authority.
- Make a failed candidate inaccessible. Do not permit automatic abort after an
  uncertain journal append.
- On retained startup, permit only inspection and journaled administrative
  Reply operations. A metadata admission is not permission to serve rows or
  resume migration.

The live-only configuration continues to use the native Coordinator with the
same exact-intent retry semantics.

### Step 3: participant bookkeeping, not durable-engine impersonation

[`metadata.rs`](../../../src/cluster/metadata.rs) uses the checked Journal with a
separate versioned **live-bookkeeping** payload:

- Bootstrap records bind owner and exact catalog/deployment identity.
- The original lease-based integration retained Begin/acquire/Finish changes.
  That is historical bookkeeping, not the native epoch admission data path:
  worker pins and cached immutable read contexts are volatile, not journaled
  per-access lease updates and not recoverable transaction outcomes.
- Successful control results are retained before their replies. Transfer intent
  precedes physical work; its result is retained before acknowledgement.
- Control persistence remains serialized; native read cache hits do not acquire
  the participant/control mutex.
- Reopening validates the committed prefix but leaves the journal inactive and
  the participant inaccessible for data use. No recovered transaction is
  finished or released.

A live Ready/Done result is **not** a durable-row completion. The distinct
payload tag prevents this journal from being mistaken for the stronger
[`DurableParticipant`](../../../src/cluster/durable_participant.rs) format.
There is no production `DurableEngine` adapter and no claim of primary-row or
transaction-outcome reconstruction.

### Step 4: runtime admission and operator configuration

[`runtime.rs`](../../../src/cluster/runtime.rs) routes coordinator mutations and
participant bookkeeping through these boundaries. It binds metadata mode into
the peer hello identity and fences the node on persistence failure.

| State | Data and migration behavior | Admin behavior |
|---|---|---|
| Fresh live-only deployment | Loader/peer readiness and immutable epoch-cached admission; serialized controls. | Existing nonce semantics. |
| Explicitly initialized metadata deployment | Same live epoch algorithm, with synchronous control/transfer bookkeeping before successful acknowledgements. | Mutations and retained replies are journaled. |
| Retained metadata startup | Readiness and read admission return Retry; capture, controls, copy, cleanup, and continuation are rejected. | Poll and an identical Begin retry may inspect historical outcomes; new Begin and Abort are rejected. |
| Uncertain metadata persistence | Candidate is fenced; no uncertain decision is converted to Abort. | Failed private coordinator state is not exposed. |

A retained pending migration stays pending. These restrictions apply after
clean shutdown as well as SIGKILL. Workload loaders, local transaction absence,
and timeout do not discharge recovery obligations.

The native bridge is `src/mako/lib/sharding_admission.{h,cc}`, gated by
`sharding_admission_enabled`. Safely pinned callers use
`mako_sharding_read_begin(owner, epoch, out)` and the read route/point/range/live
APIs, then `mako_sharding_read_end` after actual cleanup. The TLS cache compares
owner and integer epoch on entry; complete local metadata certification occurs
at publication/refresh, not per operation. Captured views may continue admitting
their original context after Frozen; no commit-time epoch equality is required.
Fixed epoch/lifetime overhead and normal STO synchronization remain. The
conditional `DurableParticipant` lease/recovery library retains its stronger
contract separately; volatile native pins do not satisfy it.

Configuration:

- `MAKO_SHARD_METADATA_DIR`: stable root, with `owner-N/participant` and, for
  multi-owner deployments, `owner-0/coordinator`.
- `MAKO_SHARD_METADATA_INIT=1`: first initialization only; existing or partially
  initialized role directories are rejected.
- Omit INIT or use `0` for retained startup. Missing/corrupt stores or changed
  identities fail closed; no automatic wipe or force-resume is provided.

See the [operator instructions](../../mako-book.md#optional-durable-migration-metadata).

### Step 5: verification and documentation

Historical metadata-only integration evidence, before the native epoch cutover
(the fresh epoch-cutover evidence follows separately):

- Full native proof root: **1,398 verified, 0 errors**.
- Full independent root: **526 verified, 0 errors**.
- **6 native and 15 independent negative controls** passed after their full
  positive runs, with semantic failure rather than compile/resource failures.
- **40 Rust tests** passed, including 11 new real-RocksDB backend/consumer
  regressions for reopen, identity, exclusive writers, corruption, retained
  outcomes, incomplete prefixes, and recovered mutation fencing.
- The existing **2 file-backed reference SIGKILL cases** passed. They exercise
  the stronger conditional engine contract, not production MBTA durability.
- `native_sharding_smoke` and `mako_admin` built successfully.
- Both existing live smoke layouts passed. **4 production metadata SIGKILL/reopen
  cases** passed: committed and held-lease-pending migration in separate-process
  and two-owner single-process layouts. Retained outcomes survived; all lease
  entrypoints and readiness remained fenced. A surviving origin's actual old
  Start/Final/Commit messages reached the restarted destination and were rejected
  by its handler, including the destructive terminal-control path.

The [verification record](../../../tla/mako/README.md#checked-results) separates
these scopes. No proof escape, filtered positive root, compiler pin change, or
weakened durable-library contract was introduced.

The epoch cutover now has fresh Docker runtime evidence: both live layouts and
all four production metadata SIGKILL/reopen cases passed with immutable read
admission and real STO pins. The live scenarios cover retained retry/cleanup
guards, idle helpers, colocated owners, first old-view access after Frozen,
source writes/deletes after private pre-copy, exact final reconciliation, and
delayed-job rejection after Final/Abort/new generations. Nested guards and
reclamation behind newer readers pass the engine/RCU regression.
The current proof, Rust-test and smoke records are listed separately in the
[verification record](../../../tla/mako/README.md#checked-results). Epoch drain
is neither an empty-lease-book assertion nor crash recovery.

## 4. Completion boundary and limits

The metadata-only deliverable and its native epoch cutover are implemented and
exercised. Retained metadata remains inspection-only; replacing volatile lease
bookkeeping with process-local epoch pins does not add primary-data recovery.

The foreign RocksDB kernel and runtime integration are tested, not
Verus-verified. SIGKILL is process-death evidence, not a power-loss test or an
injected real write/fsync failure. Durability depends on the filesystem/device
honoring synchronization and all writers honoring the namespace lock.

Full production crash-safe migration remains unimplemented. It would require
an authoritative row/version image covering every ordinary and migration
mutation, restart-safe application identities, durable exact transaction
outcomes/effects, and global old-work settlement before recovered leases could
be released. Those changes were not authorized by the selected scope.
