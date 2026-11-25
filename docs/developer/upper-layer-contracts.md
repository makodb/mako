# Upper-layer contracts

The upper protocol composes distributed OCC + 2PC, sharding and migration over
explicit storage, transport and journal contracts. The
[OCC + 2PC plan](../plans/occ-2pc.md) records its current qualification. Earlier
upper-gate timing and dependency qualification is in the
[upper-layer plan](../plans/upper-layer-verification.md).

## Verified decisions and tested composition

`mako-protocol-core::two_phase` verifies executable coordinator and participant
state transitions. The coordinator counts named participants, requires every
write-lock acknowledgement before any Yes vote, requires every Yes before
Commit, and requires confirmation of a saved decision before recording it.
A terminal decision cannot change. Completion requires an acknowledgement from
every member. Preparing and Prepared retain protection and cannot abort locally.
The specifications also require successful progress when their preconditions
hold, so a reject-all implementation does not satisfy them.

`mako-protocol::commit` is the production coordinator driver. It composes those
transitions with adapter effects. Its complete function, serialization, journal
maps, exact-payload checks and runtime adapters are ordinary Rust. They are
covered by deterministic tests and native regressions, not claimed as
whole-function Verus proofs.

The admission core verifies stream, lease, attempt, term, sequence and receipt
rules. The ordinary `Lane` wrapper refuses execution changes once the physical
participant is frozen. Transaction control messages have a separate stable ID;
a late terminal receipt must not release another attempt on the same stream.

Sharding/migration proofs establish routing, admission, drain, transfer,
reconciliation and publication properties under their own storage contracts.
Production and isolated drivers call the same actors.

## OCC storage contract

Each participant collects a fixed read/write set before its lock acknowledgement.
It exposes committed data and private read-your-writes. Present reads, absent
reads, tombstones and scan predicates all contribute to validation. A failed
application operation cannot be ignored to publish a partial transaction.

Lock succeeds only after acquiring conflict protection for every frozen write.
No committed value is installed by Lock or Prepare. Locks remain held until the
saved terminal outcome has physically completed. Final validation checks every
observation against current versions and conflicting write protection, including
inserts/deletes that could invalidate absence or range predicates. Every live
writer, including gateway operations, must participate in this protection.

The coordinator starts final validation only after every member acknowledged
its write locks. This supplies a candidate serial position between execution
and validation under the storage contract. It does not require a shard-wide
reservation. The small history explorer checks two- and three-shard point,
absence and predicate cycles against an independent serial-order oracle. That
finite exploration is not a proof of the physical storage implementation.

Native preparation exports immutable owned redo after validation. Recovery must
reconstruct equivalent write protection from that saved intent before admitting
new conflicting work. Recovered redo must equal the saved intent. A restoration
failure fences service; it cannot revoke a Yes vote. An installation failure
after saved Commit also fences service and requires recovery. Unexpected
prepared-worker teardown fences admission before releasing native locks and
retains its migration pin until shard retirement.

The Masstree and direct RocksDB STO adapters supply these physical facts. The
transaction manager tests use synthetic datatypes; each engine runs the shared
storage conformance scenarios and its own component suite. The outer protocol
gate links neither native implementation.

## Journal and authority contract

`journal::Journal` provides atomic transitions, authoritative lookup and an
explicit durability policy. Apply errors may follow persistence. An
acknowledgement means the requested exact record is saved under that policy.
Lookup must observe the current authority's prefix, not a stale follower's
absence of a decision.

- Begin binds the immutable coordinator descriptor.
- Prepare saves final row intents and any gateway receipt before Yes. It does
  not publish committed rows.
- Decide saves one immutable Commit or Abort at the coordinator. A trusted
  coordinator submits Commit only after all Yes votes. Its compare-and-set must
  serialize against recovery's attempt to choose Abort for a pending record.
- Complete records participant outcome and committed row effects together in
  persistent storage. Duplicate completion cannot repeat or change effects.

The descriptor fixes membership, leases, timestamp, recovery endpoints and
policy. Identity reuse with another descriptor or intent is rejected. Terminal
records are retained; reclamation requires a separate stale-request fence.

`CommitLog` requires `Journal`, so `MemoryLog`, `LocalLog` and `ReplicatedLog`
all supply the same journal contract directly. No missing implementation can
silently select an in-memory journal. `MemoryLog` is for tests, with an explicit
volatile opt-in for native fixtures. `LocalLog` and `ReplicatedLog` are typed
specializations of `mako_runtime::log::Log<C>` and share one command codec and
state machine with either `LocalHost` or Raft authority/read barriers. Both ordering drivers persist the
same RocksDB log format through `raft_store::journal`; a local append saves its
commit prefix atomically, while Raft controls that prefix through consensus.
Both adapters use a common RocksDB data image. Direct application persists every
committed effect immediately. Masstree keeps a full RAM image and batches the
same committed effects into RocksDB. The log covers any materialization gap.
Prepare remains private intent. A batch atomically writes rows, logical versions,
tombstones, protocol metadata, receipts, replay identities and its final durable
position. Logical transaction time is distinct from RocksDB STO's conflict
generation. Physical installation never reruns validation after Commit.

Checkpoint creation flushes through the exact application cut before taking the
native RocksDB snapshot. Saved Complete effects are included even if affine
installation is still pending. The host publishes durable snapshot authority
before trimming. Startup validates the data/metadata cut, rebuilds Masstree when
enabled, and replays the missing committed suffix. Both adapters read the same
checkpoint and can be selected on restart without conversion.

The recovery core verifies contiguous materialization beginning at D + 1,
materialization ending no later than A, checkpoint equality D = A, and rejection
of a durable position beyond log commitment C. It also verifies prepare-count
coverage, publication before trimming and replay from restored durable progress.
The runtime and storage adapters establish the inputs to these policies. Verus
does not prove native WriteBatch atomicity, serialization, checksums, filesystem
persistence, timer scheduling or reconstruction of physical locks. Separate
component and crash tests check those obligations. The upper gate has semantic
negative controls for skipped records, unapplied effects, lagging checkpoints
and recovery beyond commitment.

Synchronous storage is required to advertise durability. MemoryLog has no
restart guarantee and requires explicit development/test configuration. A
transaction cannot mix durability policies.

The participant service restores unresolved prepares before new foreground
admission after startup or authority change. Only the coordinator may resolve
Pending to Abort. A participant timeout or unreachable coordinator retains
prepared protection. Saved Commit requires eventual completion, never rollback.
The native replica adapter may fail closed if another leader's prefix arrives
while old native transactions remain active; restart reconstructs that replica.

Gateway receipts bind the exact request and response to prepared intent and
completion. Reads and no-change mutations need receipts too. A pending prepared
gateway operation must not execute again under a fresh transaction ID.

Raft consensus, RocksDB atomicity and their recovery are component obligations.
The upper tests supply journals that can acknowledge, refuse, persist before a
lost reply, or crash and restore owned bytes. They do not simulate consensus.

## Message delivery

Transport exchanges owned, bounded messages with configured peers. Delivery may
fail, duplicate, reorder or lose a response. Replies must match the pending
owner and transaction identity. Execution uses fresh stream leases and exact
sequence receipts; prepare and completion bind the frozen descriptor.

The deployment assumes trusted peers and non-Byzantine components. Identity
checks are not cryptographic authentication. SRPC supplies actual delivery and
pending-call cancellation. A deadline bounds a wait, not the remote effect's
lifetime. Safety does not assume eventual delivery. Completion additionally
requires an available authority, functioning storage and eventual delivery.

A client can retry a definite abort as a new attempt. It cannot rerun the body
after saved Commit or Unknown. It resolves the existing transaction ID or
repeats the exact gateway request instead.

## Migration storage and time

Migration uses `mako_sharding::transfer::RawStore` and `storage::Source`.
Ordered scans return the exact next row, and explicit EOF certifies the remaining
range's absence. Successful raw mutations change only the requested cell;
definite errors preserve the selected window. Admission and completed drain
supply exclusion for that window, not for unrelated ranges.

The storage host maps ambiguous raw installation or cleanup to fail-stop because
its callback contract has no Unknown variant. Transaction participants do have
Unknown and retain their native protection and migration pin. Live migration
with Raft or local persistence remains unsupported and is rejected.

A timestamp proposal must be positive, encodable and greater than its read
dependencies. Allocation cannot wrap. The distributed row/redo timestamp is
separate from Raft position, routing generation, transaction ID and cache HLC.

## Evidence and remaining trust

| Decision or contract | Production mapping | Evidence |
| --- | --- | --- |
| OCC + 2PC state | `mako-protocol-core/src/two_phase.rs` | Full core verification and semantic negative controls |
| Coordinator effect order | `mako-protocol/src/commit.rs`, runtime `client.rs::NativeEffects` | Production driver under lock, prepare, decision and completion faults |
| Immutable journal transitions | `mako-protocol/src/journal.rs` | Changed intent/descriptor rejection, duplicate decisions, restart bytes and completion row oracles |
| Lease and execution admission | Core and ordinary `admission.rs`, generic `participant::Lane` | Core proof plus changed-payload, retired-lease and delayed-request scenarios |
| Serializability composition | Frozen reads/writes and row/predicate storage contract | Exhaustive small histories with independent serial-order oracle; focused native write-skew tests |
| Prepared physical lifetime | Runtime `prepared.rs`, `receiver.rs`, owned native API | Native listener teardown, late message, migration pin, restart and saved-decision regressions |
| Authoritative journal and replay | Runtime `log.rs`, `log/materialization.rs` | Separate RocksDB and Raft checks; native checkpoint, restart, prepared failover and receipt recovery |
| Delivery and discovery | `remote.rs`, gateway SDK, runtime `rpc.rs` | Deterministic delivery faults and separate real SRPC/runtime tests |
| Sharding/migration effects | Existing protocol actors, storage host and native migration adapter | Production Verus proofs, semantic controls, ordered byte/absence oracles and native migration tests |

The old local callback coordinator remains in the native component API and its
component tests. The distributed runtime and gateway use the new commit driver;
they do not call the old post-install logging sequence. Compatibility component
proofs remain in the core gate but do not replace verification of the new path.

Source audits check the production call mappings and reject proof escapes.
Dependency audits check both the selected Cargo graph and emitted artifacts.
C/C++ compilers are disabled in the fast gate. These checks support
correspondence; they do not prove the trusted physical adapters, journal
implementation, RPC transport or Rust runtime as a whole.
