# Sharding and range migration: end-to-end design

This is the review entry point for Mako's current native sharding design. It
separates the migration algorithm from application transactions, persistence,
and recovery. It describes the implementation we have, not a proposed deployment
of the durable library.

The [production durability integration plan](../plans/range-sharding/durable-production-integration.md)
maps the remaining backend, primary-data, transaction-settlement, and runtime
work. It distinguishes metadata-only persistence from complete crash recovery.

## 1. Read this first

**Sharding decides where a key belongs. Migration changes that ownership without
losing data or letting two owners independently serve the same range.**

The current algorithm stops new accesses to the selected source range, waits for
existing accesses to finish, copies the stable range, and then changes ownership.
It does **not** move the whole range inside a multi-key transaction.

Three distinctions matter:

| Concern | Current design |
|---|---|
| Moving application rows | Individual row copies/deletes, protected by range fencing and ordered protocol steps. No transaction spanning the entire move. |
| Persisting migration metadata | The durable library and optional production RocksDB adapter use a local atomic batch for a journal entry and its head. This is a multi-key write requirement, not a general transactional query/commit engine. |
| Coexisting with application transactions | Mako's existing transactions must finish their actual effects before their access leases are released. Migration does not implement that transaction protocol. |

There are three storage/deployment scopes:

- **Production live path:** native Rust actors connected to MBTA/STO and srpc.
  Fixed live owners, non-replicated migration, no row/TP crash recovery.
- **Production metadata-only persistence:** optional synchronous RocksDB
  journals retain coordinator decisions/nonces and participant lease/control
  bookkeeping. Reopening permits historical admin inspection but fences data
  access and migration continuation. Primary storage and TP are unchanged.
- **Conditional durable path:** executable Rust wrappers around the same actors,
  checked against durable-KV **and durable-engine/TP** contracts. A real file-backed
  reference exercises full recovery; the production durable-engine/TP adapter is
  still not implemented.

Persisting bookkeeping does not make the in-memory primary store recoverable.

### Reading order

1. [Algorithm](#3-the-migration-algorithm): what moves and when ownership changes.
2. [Application transactions](#4-integration-with-application-transactions): what the access leases protect.
3. [Storage primitives](#5-storage-primitives-required-by-the-durable-path): exactly where atomicity is required.
4. [Crash recovery](#6-crash-recovery): what survives and how execution resumes.
5. [Implementation status](#7-implemented-versus-conditional-behavior): what can be used today.
6. [Review decisions](#8-decisions-to-review): current tradeoffs, not promised features.

## 2. Vocabulary and responsibilities

A migration selects a half-open range `[lo, hi)` in **one logical table**, moving
it from source owner A to destination owner B. An absent upper bound means no
upper limit. The physical row identity includes table, routing coordinate, and
row key. For ordinary tables the coordinate is the raw key; warehouse tables
can group multiple row keys under a warehouse coordinate.

| Term | Meaning |
|---|---|
| Directory / routing snapshot | Immutable mapping from table and coordinate to an ownership grant. |
| Ownership grant `(owner, ownership_version)` | Which owner may serve this placement incarnation. Returning a range to A does not resurrect A's old grant. |
| Migration coordinator | Retains the move's plan, issues controls, records participant certificates, and decides Commit or Abort. The live service uses owner 0 and allows one active move. |
| Participant | An owner applying controls and guarding its local rows. It checks its own state; it does not assume it can see the coordinator's latest phase. |
| Migration generation | Identifies a move and fences controls/copy work from older moves. |
| Administrative nonce | Identifies the caller's move request. Identical retries retain the same intent and generation; changed intent is rejected. |
| Access lease | Registration that an operation or transaction still depends on a particular ownership grant. Not a timeout-based lease. |
| Receiver admission incarnation | Durable-path restart identity. Old queued work cannot acquire the new identity simply because its receiver restarted. |
| Certificate | A participant's protocol acknowledgement, such as Drained or Ready. It is not a cryptographic proof; authenticated provenance remains an RPC contract. |

A transaction ID, migration generation, ownership version, and receiver admission
are different identities. They must not be substituted for one another.

`ownership_version` belongs to a particular range's ownership incarnation.
`Snapshot.version` identifies a complete routing snapshot: another range can move
and advance the snapshot version while this range keeps its ownership version.
Neither is Mako's global transaction/speculation epoch. A captured request carries
`(snapshot_version, owner, ownership_version)` from one immutable snapshot.
`Grant` and its persistent record encoding remain unchanged; the transient
`RoutedGrant` / `MakoShardRoute` adds the snapshot version. Data/scan/gateway
envelopes carrying this tuple use the new request format, not a mixed-version
compatibility path.

## 3. The migration algorithm

Consider moving table T's range `[m, n)` from A to B. Initially A serves it, and B
is not allowed to serve it. B can still have leftover physical rows; those are
not authoritative merely because they exist.

```text
Prepare B
   -> Freeze A and drain existing accesses
   -> Exact final copy A -> B; B becomes Ready
   -> Retire A, keeping A's data
   -> Commit the directory change to B
   -> Activate B and delete A's old copy
   -> Finish after both participant acknowledgements
```

This is a sequence of local steps and messages, not one atomic cross-shard
operation. There may be a period with no owner accepting new accesses. The
safety property is **at most one serving owner**, not uninterrupted availability.

### 3.1 Successful move

| Step | Action and required evidence | What clients can rely on |
|---|---|---|
| Admit the move | Validate that A owns the selected interval. Retain the nonce, fresh generation, old routing snapshot, and intended replacement. | An identical retry is the same move, not another allocation. |
| Prepare B (`Start`, coordinator phase `Copy`) | Put B's selected range in private staging for this generation. | A still owns the range. B's staged bytes are not client-visible data. |
| Freeze A (`Freeze`, `Freezing`) | Reject new access leases on the range. Let previously admitted accesses finish. Report Drained only when the exact intersecting lease registry is empty. | No unaccounted old access can still modify the final source image. |
| Final copy (`Final`) | Use ordered scans to copy A's values and remove destination-only rows. Require explicit EOF and complete coverage. B then records Ready. | B equals the stable source over the selected range; matching checksums alone are not sufficient. B still does not serve it. |
| Retire A (`Retire`, `Retiring`) | After Ready, stop source serving and receive Retired. Keep A's data through the decision. | The old source cannot continue serving after the routing cutover. Abort is still possible before Commit. |
| Decide Commit (`Committed`) | Select the already-validated replacement directory and retain an irreversible committed outcome. | Ownership has been decided for B. In the durable path, this decision must be durable before the new route is exposed. |
| Deliver Commit and finish | B enters Serving. A becomes nonserving and deletes its old range. Collect DestinationDone and SourceDone before clearing the active move. | A recorded decision and a completely finished migration are different milestones. |

**There is currently no background bulk copy.** The `Copy` phase name and copy
round fencing do not imply an online pre-copy implementation. A is frozen for
the complete final mirror, not just a bounded final delta. Freeze time therefore
depends on existing access completion and range size.

The final mirror merges ordered `(coordinate, row key)` scans. It preserves rows
outside the selected range, copies actual values, and deletes extras at B. An
empty value is not an absent key. An error, conflict, or short nonempty page is
not evidence that the scan has reached the end.

The protocol fence is range-scoped. It is not a requirement to stop unrelated
ranges, although an adapter's locking or execution model can impose additional
serialization.

### 3.2 Abort and retries

Before Commit, the coordinator may decide Abort. A retains its authoritative
data and returns to Serving; B remains nonserving and cleans its staging range.
Both participants still have to acknowledge their required terminal work.
After Commit, the protocol must finish the committed decision, not roll it back.

Messages can be delayed, duplicated, or reordered. Each participant checks the
plan, generation, role, copy round, and terminal fence. A late packet cannot
become current by looking up a newer route or changing its generation. Retained
nonce outcomes let the caller discover what happened after a lost response.

An important race is allowed: B can finish its previously authorized final copy
after the coordinator has aborted but before B receives Abort. B's Ready state
is still private. That late Ready cannot reverse the retained Abort; B later
cleans the private copy.

The native transition sources are [the coordinator](../../src/cluster/migration.rs)
and [participant role transitions](../../src/cluster/participant_metadata.rs).

## 4. Integration with application transactions

### 4.1 Does migration require multi-key transactions?

**Not to perform the move.** Migration does not invoke a transaction that reads
or writes all migrating keys atomically, nor a transaction that atomically
updates both source and destination.

However, Mako already supports application transactions touching multiple keys
and owners. Migration must preserve their correctness. The transaction engine
still owns read validation, commit/abort decisions, atomic application effects,
and terminal peer cleanup. The migration layer owns admission, placement
fencing, and waiting until those accesses no longer depend on the old placement.

For a store exposing only single-key application operations, the analogous
obligation would be to register and drain those in-flight operations. That does
not by itself satisfy the current durable library's metadata and engine APIs;
we have not supplied a separate minimal single-key-only adapter.

### 4.2 Why accesses need leases

Before selecting a physical owner, an access captures its snapshot version and
`(owner, ownership_version)` grant together and registers a point or range lease.
Readers, writers, absent-key reads, and scans all count. Retries retain the entire
captured tuple rather than combining an old grant with a newer snapshot version.

The lease remains held through the real engine effects and required participant
cleanup. A commit decision or lost RPC reply is not necessarily completion.

For example, transaction X has a write to A pending when the range freezes:

```text
Unsafe: release X's lease -> copy A -> publish B -> X's late write reaches A
Safe:   fence new access -> finish/fence X's effects -> release lease -> copy A
```

In the unsafe sequence the late write can be omitted from B. Multi-key
transactions make the completion boundary more involved, but even one delayed
single-key write creates this problem.

#### Two-level admission: snapshot fast path, ownership fallback

The ordinary receiver can skip its table hash lookup and range-boundary search
when a request's snapshot version equals its **locally certified snapshot
version**. This is not equality with the latest routing-cache version alone.
Certification checks, at snapshot/authority update time, that every range the
snapshot assigns to this owner has local `Serving` authority with the same
ownership version. The request and certificate refer to the same immutable
publication from the fixed coordinator.

Under the participant mutex, admission follows this structure:

1. Enforce recovery, transaction identity, and retained-lease rules.
2. For a genuinely captured request and matching local certificate, use the
   version fast path.
3. Otherwise perform the existing exact local range/ownership-version check.
4. Register the lease before returning permission to perform physical work.

The version comparison removes only the metadata lookup. It does not remove
lease registration, transaction settlement, or drain obligations. A mismatch is
not itself a rejection: an older snapshot can still name an unchanged range.

Every local authority mutation invalidates the derived certificate before
changing metadata. This includes `Freeze`, which occurs **before** a new
snapshot is published. Without that invalidation, a request and receiver could
both say snapshot 10 even though the requested range is already frozen.
Certification is retried on snapshot installation and completed local controls,
never by scanning the entire routing map for each data request. Unrelated ranges
remain available through the ownership fallback while certification is absent.

Snapshot provenance is part of the request contract, not an unchecked assertion
by a client. Native point capture binds the actual table and coordinate to both
versions; forwarding and retries preserve that binding. A full-scan capture
binds its **whole segment**, including empty-key gaps, to one retained snapshot.
A point grant at the lower endpoint is not evidence for an entire scan interval.
Generic local intervals and untrusted gateway routing claims therefore use the
exact range/ownership check unless full capture provenance is established.
`NO_SNAPSHOT` explicitly requests that slow path; it never means “use the latest
snapshot.” Gateway wire retries still retain their original tuple.

The certificate is a disposable admission optimization, not a durable authority
record. Recovery starts without one. Production metadata-only recovery continues
to refuse data access even if cached versions happen to match. Verus checks the
capture, certification, invalidation, and admission/refinement obligations;
the native C++ ABI and genuine, unchanged peer transport remain named foreign
contracts rather than verified or cryptographically authenticated bodies.

### 4.3 Additional TP requirements for crash recovery

The durable path retains unfinished identities and leases. Its TP adapter must:

- Keep transaction IDs stable across participants and restarts; no worker-ID
  reset may make an old transaction appear new.
- Settle the exact recorded effects and terminal outcome, and exclude later
  effects from surviving peers or queued requests before releasing a lease.
- Fence even a journaled identity that never reached the engine. Local absence
  is not proof of completion; the contract permits a durable empty-abort fence
  only after old work is actually excluded.
- Authorize transaction enrollment through the metadata operation, including a
  late accepted operation whose reply was lost. A transaction settled elsewhere
  must not join a new owner afterward.

`DurableParticipant::finish` and `release` call
`DurableEngine::settle_transaction` before releasing the native lease. A local
`Resolve` record is not a substitute for that engine outcome. These are
**requirements on TP integration**, not a distributed transaction-recovery
implementation provided by the migration library.

## 5. Storage primitives required by the durable path

This section describes the separate durable library, not the current MBTA
production adapter.

| Primitive | Required guarantee | Does it require a multi-key application transaction? |
|---|---|---|
| Ordered range scan | Canonical row identities and values, strict cursor progression, explicit EOF, and no conversion of errors into successful partial coverage. | No. The source is stabilized by the protocol. |
| Row mutation | One atomic durable logical row/value-version effect; writes outside the selected range are preserved. | No whole-range transaction. Value/version coherence is the engine's responsibility. |
| Durability / settlement barrier | Establish which accepted I/O happened and exclude later application of old work. | No transaction query API, but stronger than treating a timeout as failure. |
| Journal append | Atomically persist the operation record and updated head in one authority namespace. | **Requires an atomic two-key metadata batch today**, not general transaction isolation or cross-shard commit. |
| Writer ownership and fencing | One serialized live writer per namespace; stale writers cannot apply after recovery. | Atomic batch writes alone do not provide this. Rust `&mut` is not distributed locking or leader election. |
| Transaction settlement | Preserve whatever application transaction semantics the embedding engine provides. The current interface carries exact write sets and outcomes. | For Mako's multi-key transactions, those existing TP guarantees remain required. |

The engine facade is exclusively borrowed during a transfer call. A successful
admission check must expose a coherent working/durable image with no unresolved
raw mutation. Wrapping a primary store that can mutate independently behind that
facade does not satisfy the contract. Physical table/coordinate/key bindings must
also survive reopening.

### 5.1 What the journal stores

Each coordinator or participant authority has a separate namespace. A head
stores the receiver admission incarnation and committed entry count. Numbered
entries contain versioned encodings of native inputs, starting with Bootstrap at
entry zero. They include migration decisions and receipts, plan bindings, lease
operations, transfer reservations, and completion records as appropriate to the
authority.

Replay rebuilds the existing native actor by executing those inputs. It does not
load a second executable protocol or accept the old actor's memory as recovery
input. The committed coordinator input determines both its decision and routing
snapshot; these are not independently published mutable records.

The metadata-only production coordinator uses this checked replay. Its
participant journal instead retains versioned **live bookkeeping**, including
lease changes and transfer intent/result records. That payload has a separate
format tag: an observed live Ready/Done is not a durable-row completion. Reopening
validates the participant prefix but does not reactivate its actor or release
leases. It is not a substitute implementation of `DurableParticipant`.

### 5.2 The exact atomic-batch dependency

Using symbolic key names, suppose one journal contains:

```text
head = { incarnation: 3, count: 7 }
entry[0] ... entry[6] = committed operations
```

Appending operation X currently means one atomic durable batch:

```text
put entry[7] = X
put head = { incarnation: 3, count: 8 }
```

Recovery reads the head and replays exactly entries `0..count-1`. Atomicity makes
this append retain either the old prefix or the new prefix, never a head that
requires an uncommitted entry. The batch belongs to one authority; it does not
atomically update A's journal, B's journal, and the coordinator's journal.

This is a **current implementation choice**, not a claim that sharding inherently
needs multi-key transactions. An entry-first, single-key head-publication design
is a possible alternative: make the entry durable before publishing the head,
and explicitly handle unpublished tails, uncertain writes, and old-writer
fencing. That alternative is not implemented or proved here. Merely replacing
the batch with two arbitrary puts would not preserve the current contract.

### 5.3 Ordering across data and metadata

No atomic transaction spans the row store and all migration journals. Instead,
the durable wrappers enforce these orderings:

```text
durable transfer reservation -> row copy -> durable complete copy -> Ready record
Ready + Retired evidence -> durable Commit decision -> expose new routing
Commit/Abort cleanup obligation -> durable deletion -> Done record
```

A participant can become logically nonserving before its physical rows are
removed. That intermediate state is not Done. Likewise, partially copied rows
at B are not Ready and cannot be served.

The KV API distinguishes `Committed`, `NotCommitted`, and `Unknown`. Unknown is
not success or failure. Failed/unknown persistence fences the candidate wrapper;
recovery resolves the accepted work before execution continues. A coordinator
applies an operation privately, appends it, and only exposes the resulting actor
through its admitted API after success.

The concrete key encoding and full interface obligations are specified in
[Storage Interface: recovery scope](../storage-interface.md#recovery-and-progress-scope).
The implementation is [durable_kv.rs](../../src/cluster/durable_kv.rs) and
[durable_engine.rs](../../src/cluster/durable_engine.rs).

## 6. Crash recovery

The production metadata-only mode reopens its stores, validates the retained
identity/prefix, and reconstructs the coordinator for historical admin polling.
An identical Begin retry can return its retained outcome. It does **not** publish
recovered serving routes: readiness, lease begin/acquire/range/finish, capture,
control/copy/cleanup, new Begin, and Abort remain fenced. Pending work stays
pending; lost in-memory leases are not interpreted as completed transactions.
This applies after clean shutdown as well as process death.

The full recovery steps below apply only when the backend actually satisfies
both durable metadata and durable engine/TP contracts. Restoring journal bytes
beside lost in-memory primary rows is not valid data recovery.

### 6.1 Reconstruct, fence, then resume

1. **Stop using the old wrapper.** Do not reuse its cached actor, cursor, or
   admission after an uncertain persistence result or restart.
2. **Settle old I/O.** Participant recovery first restores authoritative rows and
   settles/excludes old raw copy/delete submissions. Journal recovery resolves
   accepted metadata batches before reading the definitive head.
3. **Read and replay the committed prefix.** Validate records, construct the
   actor from Bootstrap, and replay its native operations. A missing/malformed
   required record is an error, not permission to initialize fresh state.
4. **Persist a fresh admission incarnation.** Install it in the participant's
   recovered engine before returning an admission to callers. Settle a recovered
   transfer reservation before permitting replacement work. Never relabel an
   old queued RPC with the new admission.
5. **Keep unfinished access leases.** Enumerate old identities from committed
   Begin records with `next_recovery_transaction`. Settle them through the engine
   before `finish` releases their leases. Activation alone does not prove that
   the source range is drained.
6. **Resume the retained phase.** Retransmit missing controls, repeat authorized
   copy/cleanup when needed, and record actual certificates. Do not allocate a
   new move just because its previous reply was lost.

Recovered serving still depends on the participant's role and normal access/TP
authorization. Recovery does not turn Stage, Ready, Frozen, or Retired into
Serving merely by opening the database.

### 6.2 Work selected from the coordinator's phase

| Recovered phase | Next work |
|---|---|
| `Copy` | Ensure destination staging, then request freeze. No background bulk copy is implied. |
| `Freezing` | Continue source freeze/drain; advance only with Drained. |
| `Final` | Continue/retry final copy; advance only with Ready. |
| `Retiring` | Obtain/replay Retired; then request Commit. |
| `Committed` | Deliver the retained Commit and finish outstanding participant work. Never change it to Abort. |
| `Aborted` | Deliver the retained Abort, restore source service, and complete destination cleanup. |
| No active move | No migration work. Retained nonce outcomes remain queryable. |

`DurableCoordinator::resume` selects work; a driver performs it and journals the
result. It is not a background recovery service installed in production.

### 6.3 Representative crash boundaries

| Crash or uncertainty | Required recovery behavior |
|---|---|
| Some destination rows copied, no committed Ready | B stays private. Settle raw I/O and redo the authorized exact mirror; never infer completion from the rows that happen to exist. |
| Copy durable, completion append uncertain | Resolve the journal prefix. Retained completion can replay Ready; otherwise completion must be re-established through the native path. |
| Commit append reply lost | Resolve whether Commit entered the committed prefix before exposing routing or deciding what to retry. If committed, retain that decision. |
| Source deletion partly applied, no Done | Keep the committed route to B. Settle old deletion work and resume generation-scoped cleanup. |
| Begin/lease reply lost before engine registration | Discover the identity from the journal; require a real old-work fence before recording an empty abort and releasing its lease. |
| Old cleanup arrives after ownership returns to A | Reject it using the retained generation/terminal/admission fences; owner number equality is insufficient. |

Permanent failures, indefinitely delayed messages, or transactions that cannot
settle can block completion. There is no unconditional or bounded-time progress
guarantee.

## 7. Implemented versus conditional behavior

| Area | Status |
|---|---|
| Native coordinator, participant, leases, exact ordered transfer, ownership versions, nonce retention | Implemented in Rust and connected to the live MBTA/STO/srpc path. |
| Live production migration | Fixed live owners, one active move, non-replicated mode. Same-process and separate-process paths are exercised. |
| Production metadata persistence | Implemented, opt-in via `MAKO_SHARD_METADATA_DIR`. [`rocks_metadata.rs`](../../src/cluster/rocks_metadata.rs) supplies synchronous atomic journal batches; [`metadata_coordinator.rs`](../../src/cluster/metadata_coordinator.rs) and [`metadata.rs`](../../src/cluster/metadata.rs) integrate coordinator replay and live participant bookkeeping. Reopened owners remain inspection-only. |
| Production crash-safe migration | **Not implemented.** Primary MBTA rows are in memory; raw migration uses `try_commit_no_paxos`. Existing RocksDB logs do not satisfy the new authoritative-row/recovery contracts. |
| Durable journal, actor replay, admissions, phase-resume selection, completion ordering | Verified against explicit backend/TP contracts. Production uses the journal/coordinator metadata pieces, not durable participant/engine recovery or automatic phase resumption. |
| File-backed reference backend | Exercises actual persistence, reopen, unknown outcomes, and SIGKILL. Its TP fixture is single-owner, not distributed TP recovery. |
| Native background pre-copy, automatic rebalance, dynamic membership, journal/tombstone compaction | Not implemented by this native durable path. Broader independent model results do not add these deployment features. |
| Formal guarantees | Native correspondence and independent protocol proofs have separate scopes. They are not a proof of the foreign C++ storage/RPC bodies or a combined production TP/Raft/migration deployment. |

The checked invariants include at most one serving owner, stable held grants,
exact final copy, retained decisions, stale-work rejection, and data-before-completion
ordering under the named interfaces. The [verification record](../../tla/mako/README.md#checked-results)
lists complete proof runs, negative controls, Rust regressions, reference-backend
SIGKILL cases, and production live/metadata-restart smoke evidence. Their scopes
remain separate: retaining metadata and refusing service is not MBTA durability.

## 8. Decisions to review

These are the current choices exposed for review, not requests to silently widen
or change the implementation:

1. **Storage floor:** accept an atomic local metadata batch, or require a backend
   with only atomic durable single-key writes? The latter needs a different
   journal publication protocol and corresponding recovery proofs.
2. **Availability during movement:** accept freezing the range for the full
   final mirror? A background pre-copy/delta design would be a separate change.
3. **Approved deployment boundary:** metadata persistence only; primary storage
   and TP remain unchanged. Automatic crash-safe migration would require a
   separately approved authoritative-row and transaction-recovery integration.
4. **Growth and lifecycle:** accept append-only history and retained fences for
   this path? Bounded retention and changing membership require additional native
   protocols, not just deleting old records or adding owners to configuration.

## 9. Code and deeper references

| Subject | Source |
|---|---|
| Names and identities | [types.rs](../../src/cluster/types.rs) |
| Coordinator and publication | [migration.rs](../../src/cluster/migration.rs), [durable_coordinator.rs](../../src/cluster/durable_coordinator.rs) |
| Local roles, access leases, replay | [participant.rs](../../src/cluster/participant.rs), [leases.rs](../../src/cluster/leases.rs), [durable_participant.rs](../../src/cluster/durable_participant.rs) |
| Actual copy and cleanup loops | [storage.rs](../../src/cluster/storage.rs), [transfer.rs](../../src/cluster/transfer.rs) |
| Durable primitives and record format | [durable_kv.rs](../../src/cluster/durable_kv.rs), [durable_engine.rs](../../src/cluster/durable_engine.rs) |
| Live integration and operational limits | [Mako book](../mako-book.md#data-migration-protocol-live-native-sharding) |
| Detailed storage and TP obligations | [Storage interface](../storage-interface.md#recovery-and-progress-scope) |
| Formal specifications and proof boundaries | [Sharding specification](../../tla/mako/README.md#dynamic-range-sharding) |
