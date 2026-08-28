# Rust STO: Type-Aware Transactions for Safe, Extensible Data Structures

> **Status:** Implemented experimental non-opaque v1; pre-cutover
>
> **Implementation status:** `sto-core`, the raw and safe Masstree boundary,
> transactional Masstree point operations, copied scans, membership validation,
> bounded registries, and the upper commit-hook seam exist on this branch.
> The optional opacity profile, graceful native shutdown, production upper-layer
> facade/cutover, and performance acceptance remain deferred milestones.
>
> **Audience:** STO core, transactional-datatype, Masstree ABI, and Mako integration developers
>
> **Baseline:** Mako `mako-dev` at `abfb6ea96739`; compatibility oracle
> `worktree-masstree-rocks` at `1daec550f`
>
> **Last updated:** 2026-08-27

This document defines the intended semantics and architecture of Mako's native
Rust implementation of STO. It is a living design contract: implementation
changes that contradict a normative requirement here must update this document
and its rationale in the same change.

The design begins with Herman et al.'s EuroSys 2016 paper,
[_Type-Aware Transactions for Faster Concurrent Code_][sto-paper], and then
makes the Rust, Masstree, and Mako adaptations explicit. It is not a claim that
the C++ implementation should be transliterated into Rust.

The words **MUST**, **MUST NOT**, **SHOULD**, **SHOULD NOT**, and **MAY** are
normative. Four labels distinguish the source of a decision:

- **[STO]** is inherited from the EuroSys STO model.
- **[COMPAT]** preserves externally useful behavior of Mako's current STO or
  the upper Rust transaction-cache branch.
- **[RUST]** is a deliberate redesign for Rust safety, explicit ownership, or
  the Masstree C ABI.
- **[DEFERRED]** is intentionally outside the first implementation.

Related repository guides describe the
[current C++ Masstree/MassTrans stack](../masstree-book.md) and the broader
[storage interface](../storage-interface.md). This document describes the
target native-Rust transaction layer.

## Contents

1. [Abstract](#1-abstract)
2. [Lineage and central idea](#2-lineage-and-central-idea)
3. [Goals, non-goals, and first-release scope](#3-goals-non-goals-and-first-release-scope)
4. [Semantic contract](#4-semantic-contract)
5. [System architecture and trust boundaries](#5-system-architecture-and-trust-boundaries)
6. [Rust programming model](#6-rust-programming-model)
7. [Transaction items](#7-transaction-items)
8. [Rust adapter interface and protocol](#8-rust-adapter-interface-and-protocol)
9. [Versions and the Rust memory model](#9-versions-and-the-rust-memory-model)
10. [Commit, abort, and failure state machine](#10-commit-abort-and-failure-state-machine)
11. [Predicates, commutativity, and absence](#11-predicates-commutativity-and-absence)
12. [Opacity profiles](#12-opacity-profiles)
13. [Lifetimes and reclamation](#13-lifetimes-and-reclamation)
14. [Masstree transactional adapter](#14-masstree-transactional-adapter)
15. [Masstree C ABI](#15-masstree-c-abi)
16. [Correctness argument](#16-correctness-argument)
17. [Mako and upper-layer integration](#17-mako-and-upper-layer-integration)
18. [Verification and evaluation](#18-verification-and-evaluation)
19. [Implementation sequence and rollout](#19-implementation-sequence-and-rollout)
20. [Decision record and change policy](#20-decision-record-and-change-policy)
21. [References](#references)

## 1. Abstract

Conventional software transactional memory tracks reads and writes to memory
words. STO instead tracks operations on logical parts of transactional
datatypes. A counter, array slot, map record, missing-key witness, or range
predicate can each define the conflict unit most appropriate for its semantics.
The transaction core coordinates those units; datatype adapters perform the
actual locking, validation, installation, and cleanup.

Rust STO preserves that separation. `sto-core` is a native Rust optimistic
transaction coordinator. Rust adapters make arbitrary concurrent datatypes
transactional by implementing a typed protocol over stable logical items.
Masstree remains a C++ structural index behind a narrow C ABI, but record
versions, locks, values, tombstones, predicates, and transaction state live in
Rust. The C ABI exposes no C++ cursor, node pointer, value pointer, callback, or
transaction object.

The first release provides strictly serializable, non-opaque transactions,
read-your-writes, heterogeneous composition, point operations, logical deletes,
and conservative transactional range scans. Opacity is a separately negotiated
profile. Distributed coordination, MVCC, durability, and physical reclamation
remain above or after the core.

## 2. Lineage and central idea

### 2.1 The EuroSys STO model

**[STO]** The paper's defining observation is that transactional bookkeeping
should operate at the level of datatype semantics rather than untyped memory.
A transaction maintains a set of logical items. Each item is identified by a
transactional object and a datatype-defined key and can hold:

- an observation, normally a version;
- a staged write;
- an optimistic predicate; and
- datatype-private commit state.

The core knows how to coordinate a transaction, but it does not know how a tree,
counter, or queue represents state. Datatype callbacks know how to lock a
logical segment, validate an observation, install a write, and clean up.

This division is both the source of STO's generality and its performance model.
Semantic items can be far fewer than the memory words traversed by an operation,
and datatype specifications can recognize commutative operations that should
not conflict.

### 2.2 What the paper requires from a datatype

The paper identifies three broad responsibilities:

1. Split shared state into logical, versioned segments.
2. Record abstract reads, writes, and predicates in the transaction's item set.
3. Implement commit callbacks for locking, validation, installation, and
   cleanup.

“Transactions for any data structure” therefore does not mean automatic
instrumentation. It means any correctly concurrent datatype can participate if
its adapter supplies a sound conflict model and obeys the protocol in
[Section 8](#8-rust-adapter-interface-and-protocol).

### 2.3 The local C++ lineage

The original abstraction is still visible in:

- [`TransactionTid` and `TObject`](../../src/mako/sto/Interface.hh);
- [`TransItem`](../../src/mako/sto/TransItem.hh);
- [`Transaction`](../../src/mako/sto/Transaction.hh) and its
  [commit implementation](../../src/mako/sto/Transaction.cc); and
- the [`MassTrans` adapter](../../src/mako/sto/MassTrans.hh).

The true extension seam remains `TObject::{lock, check_predicate, check,
install, unlock, cleanup}`. However, Mako's current `Transaction.cc` also owns
RPC batching, logging, remote-table handling, replication timestamps, MVCC
policy, and MassTrans-specific payload casts. **[RUST]** Those concerns MUST NOT
be translated into `sto-core`; they belong to upper coordination and storage
adapters.

### 2.4 Lessons from the paper's evaluation

The paper reports that semantic tracking sets, constant-time item lookup, and
application-specific conflict reduction were material to performance. It also
shows that the generic core could match or exceed a purpose-built Silo path on
the authors' 2016 hardware. Those results motivate this design, but their
absolute numbers are not current performance targets.

We retain four durable lessons:

- item lookup must be expected `O(1)`;
- common items must eventually have a compact representation;
- false-conflict rate is a first-class performance metric; and
- opacity, item ordering, and type erasure must be benchmarked rather than
  optimized from intuition.

Correctness-first boxed or erased state is acceptable for the first vertical
slice as long as the public adapter protocol does not preclude later arenas or
small-item storage.

## 3. Goals, non-goals, and first-release scope

### 3.1 Goals

Rust STO MUST:

1. Execute the transaction coordinator as native Rust.
2. Compose operations across heterogeneous transactional objects in one atomic
   transaction.
3. Preserve the paper's type-aware item and callback model without reproducing
   its unchecked C++ storage tricks.
4. Make transaction, worker, resource, and cleanup lifetimes explicit.
5. Support correct adapters outside the `sto-core` crate.
6. Support Masstree through a narrow, hardened C ABI.
7. Provide read-your-writes and deterministic same-item mutation composition.
8. Provide strict serializability for every committed transaction.
9. Permit an opacity profile without silently weakening it.
10. Be differentially testable against the current C++ STO/MassTrans engine.

### 3.2 Non-goals

The first release is not:

- a transparent, word-based STM or compiler transformation;
- an automatic wrapper for an arbitrary non-thread-safe datatype;
- a port of Mako RPC, Paxos, RocksDB, logging, or watermark logic;
- an MVCC implementation;
- a distributed atomic-commit protocol;
- a stable C ABI for the Rust STO core;
- a physical Masstree deletion or record-GC system;
- an async or cross-thread transaction API;
- nested transactions;
- concurrent mixed use of the C++ and Rust engines over the same physical
  table; or
- a port of every historical C++ STO datatype.

The original STO library included `TGeneric`, an untyped word-tracking fallback.
**[RUST]** Omitting that fallback from v1 is a scope decision; it is not a claim
that the EuroSys system supported only purpose-built datatypes.

### 3.3 First-release feature boundary

The initial production profile includes:

- non-opaque OCC with final read and predicate validation;
- `TxnCell` and at least one independently implemented generic adapter;
- heterogeneous, multi-item and multi-table transactions;
- binary-key Masstree `get`, `put`, `insert`, and `remove`;
- logical tombstones and insert/delete/resurrection composition;
- bounded forward and reverse scans;
- scan read-your-writes and conservative phantom detection; and
- an optional, nonblocking pre-install hook with an upper-layer watchdog budget.

Opacity can land after the non-opaque protocol is proven. An API request for an
unimplemented isolation profile MUST fail explicitly; it MUST NOT silently
downgrade.

## 4. Semantic contract

### 4.1 Definitions

A **runtime** is one independent STO clock, object registry, and worker-ID
domain.

A **transactional object** is a concurrent datatype instance registered in one
runtime.

A **logical resource** is the smallest adapter-defined state segment whose
observation or mutation participates in conflicts. Examples include an array
index, counter value, map record, bucket generation, or table membership
generation.

A **transaction item** is the unique transaction-local entry for
`(ObjectId, ResourceClass, ResourceKey)`.

An **observation** is an owned token sufficient to validate a prior abstract
result. A **covering observation set** changes whenever shared state relevant to
that result changes.

A **write intent** is an owned, transaction-local description of the logical
state to install if the transaction commits.

### 4.2 Required guarantees

**[STO] Atomicity.** A committed transaction has all of its abstract effects; an
aborted transaction has none. Physical, logically invisible scaffolding such as
an interned tombstone may remain after abort.

**[STO/RUST terminology] Linearizable transactional effects / strict
serializability.** The paper describes committed transactions as having
linearizable effects. This document calls the corresponding history property
strict serializability: registered transactional-object operations MUST be
explainable by one serial history that respects real-time order. Ordinary Rust
or external effects outside the STO protocol are not covered. If transaction A
returns committed before transaction B begins, B cannot be serialized before A.

**[STO] Isolation before commit.** A transaction's staged logical mutations MUST
not be visible to another transaction before the irreversible install phase.
An audited eager operation may change physical structure only if other
transactions still observe the pre-transaction abstract state or abort.

**[STO] Read-your-writes.** Before returning an abstract result, every operation
MUST compose it with prior transaction-local intents for the same logical
resource. A tree operation may still need shared structural access before that
composition. Scans additionally overlay all staged in-range insertions, updates,
and removals. Sequences such as insert → remove → insert, put → get, and scan →
own insert reflect the transaction-local logical state.

**[STO] Composition.** Items from different adapters and objects can commit in
one transaction. An adapter MUST NOT assume that it is the transaction's only
resource.

**[STO] Optional opacity.** In the opaque profile, even a transaction that later
aborts MUST NOT expose a transactionally inconsistent state to its body. The
non-opaque profile retains commit atomicity and strict serializability but does
not provide this execution-time snapshot guarantee.

### 4.3 Progress and retry

STO does not promise lock freedom, wait freedom, starvation freedom, or fairness.
A conflict may cause a false abort, including a failed bounded lock attempt.
Retry policy, delay, backoff, cancellation, and retry limits belong to the
caller or upper layer, not to `sto-core`.

**[RUST]** Unlike the paper's `TRANSACTION ... RETRY(expr)` wrapper, `sto-core`
returns a typed abort outcome and leaves retry policy to its caller.

Transactions are one-shot values. A commit attempt consumes the active
transaction and returns a typed outcome. Reusing a finished transaction is an
API error.

The core does not roll back ordinary Rust or external side effects performed by
the transaction body. Code intended for retry SHOULD restrict such effects to
transactional objects or immutable/thread-local computation. A pre-install hook
may be used only when its upper contract supplies preallocated staging whose
rejection or panic path has no externally visible effect; it is not a general
rollback mechanism.

### 4.4 Error classes

The public API MUST distinguish at least:

- `Conflict`: a normal, retryable validation or lock failure;
- `Capacity`: a transaction, worker, key, ID, or buffer limit;
- `InvalidUse`: wrong runtime, worker, thread, state, or argument;
- `Unsupported`: a requested capability is unavailable;
- `Poisoned`: the runtime can no longer accept work, but this transaction's
  abstract committed/aborted outcome is known and returned with the error;
- `Indeterminate`: publication may have started and the abstract outcome cannot
  be classified safely; and
- `Internal`: a failure occurred before the irreversible boundary and cleanup
  completed.

After the irreversible boundary, no failure may be reported as an ordinary
abort. A failure with a proven abstract result is `Poisoned` with its
`DefiniteOutcome`; a failure whose publication cannot be classified is
`Indeterminate`. Both quarantine the affected worker/runtime according to the
integration contract, and neither is a retryable conflict.

## 5. System architecture and trust boundaries

```text
application / existing upper Rust transaction API
                         |
                 backend-neutral facade
                    /             \
      C++ reference backend       native Rust backend
        mako_local_* ABI                |
                                  sto-core
                                      |
                                sto-masstree
                                      |
                              safe masstree crate
                                      |
                                  mtree-sys
                                      |
                             Masstree-only C ABI
                                      |
                       C++ Masstree structural kernel
```

### 5.1 Crates

The intended workspace layout is:

```text
crates/
  sto-core/       Native transaction runtime and generic adapter protocol
  mtree-sys/      Raw generated or mechanically checked C declarations
  masstree/       Safe runtime, worker, tree, point, and scan wrapper
  sto-masstree/   Rust records and the transactional Masstree adapter
```

An upper `sto-mako` or equivalent integration crate MAY own Mako timestamps,
write descriptions, distributed participant state, and durability policy. None
of those types belong in `sto-core`.

### 5.2 Trust boundaries

Safe application code may use `sto-core`, `masstree`, and `sto-masstree`
without writing `unsafe`.

`mtree-sys` is entirely unsafe and contains no policy. The safe `masstree`
crate validates lifetimes, thread affinity, sizes, statuses, and output buffers.

Adapter implementations are part of the **transactional-correctness trusted
base**, even when written entirely in safe Rust. Rust memory safety cannot prove
that an adapter chose a covering version or a sound conflict unit.
`TransactionalResource`, `OpacityToken`, and `TransactionLock` are nevertheless
safe traits. A bad implementation can violate isolation or progress, but
`sto-core` memory safety MUST NOT depend on its semantic claims. Any adapter
that touches `UnsafeCell`, raw pointers, or FFI owns the corresponding unsafe
proof and must check the required lock or guard itself.

The private erased item and lock vtables perform checked type transitions only.
If a future optimization cannot satisfy that boundary, it requires a separately
reviewed unsafe primitive; it does not retroactively make the public adapter
protocol unsafe.

## 6. Rust programming model

### 6.1 Runtime and worker contexts

`Runtime` owns:

- a unique `RuntimeId`;
- the optional opacity/commit clock;
- the object registry;
- the finite owner-ID registry;
- runtime health and statistics; and
- registered object/resource type bindings.

`WorkerContext` represents one attached, long-lived OS worker. It has a stable
`OwnerId`, belongs to exactly one runtime, and is `!Send + !Sync`. Creating more
workers than the version encoding or native Masstree runtime supports returns a
capacity error; it never aborts the process.

The core API uses explicit context borrowing:

```rust
let mut txn = worker.begin()?;
let old = table.get(&mut txn, b"account")?;
table.put(&mut txn, b"account", new_value)?;
match txn.commit()? {
    CommitOutcome::Committed(info) => { /* visible */ }
    CommitOutcome::Aborted(reason) => { /* caller may retry */ }
}
```

Ambient TLS MAY exist inside the C++ bridge or a legacy compatibility shim, but
it is not the Rust core's ownership model.

### 6.2 Public transaction surface

**[RUST]** The following is the normative v1 ownership and outcome shape and is
implemented by `sto-core`. Future refactoring MUST NOT replace the explicit
worker borrow, consuming completion methods, or the definite-versus-
indeterminate outcome split with ambient state or booleans.

```rust
use std::sync::Arc;

pub struct Runtime { /* private */ }
pub struct WorkerContext { /* private; structurally !Send + !Sync */ }
pub struct Active;
pub struct Transaction<'worker, State = Active> { /* private */ }

impl Runtime {
    pub fn new(config: RuntimeConfig) -> Result<Arc<Self>, RuntimeError> {
        unimplemented!("signature-only design target")
    }

    pub fn attach(
        self: &Arc<Self>,
    ) -> Result<WorkerContext, AttachError> {
        unimplemented!("signature-only design target")
    }
}

impl WorkerContext {
    pub fn begin(
        &mut self,
    ) -> Result<Transaction<'_, Active>, BeginError> {
        unimplemented!("signature-only design target")
    }

    pub fn begin_with(
        &mut self,
        isolation: IsolationMode,
    ) -> Result<Transaction<'_, Active>, BeginError> {
        unimplemented!("signature-only design target")
    }
}

impl<'worker> Transaction<'worker, Active> {
    pub fn commit(
        self,
    ) -> Result<CommitOutcome, CommitFailure> {
        unimplemented!("signature-only design target")
    }

    pub fn abort(self) -> AbortInfo {
        unimplemented!("signature-only design target")
    }
}

pub enum CommitOutcome {
    Committed(CommitInfo),
    Aborted(AbortReason),
}

pub enum CommitFailure {
    Poisoned {
        outcome: DefiniteOutcome,
        info: PoisonInfo,
    },
    Indeterminate(IndeterminateInfo),
}

pub enum DefiniteOutcome {
    Committed(CommitInfo),
    Aborted(AbortReason),
}
```

`begin` uses the runtime's configured default isolation profile; `begin_with`
fails with `Unsupported` rather than downgrading an unavailable profile.
Ordinary conflict, capacity exhaustion discovered during commit, hook rejection,
and any other definite pre-irrevocable abort return
`Ok(CommitOutcome::Aborted(...))`. `CommitFailure::Poisoned` quarantines the
runtime but carries a definite committed or aborted outcome; a caller MUST use
that outcome and MUST NOT retry a definitely committed transaction.
`CommitFailure::Indeterminate` is reserved for publication that cannot be
classified after the irreversible boundary. `CommitInfo` contains core OCC
metadata only; it never smuggles a `MakoTimestamp`, durability sequence, or
upper-layer commit decision into `sto-core`.

Both completion methods consume the active transaction. `abort` is infallible
under the adapter contract and returns only after exact-once cleanup. Dropping
an active transaction takes the same abort path but cannot return its
`AbortInfo`; any impossible cleanup violation is recorded in runtime health.

### 6.3 Transaction ownership

`Transaction<'worker, Active>` mutably borrows its worker. It is `!Send` and
`!Sync`, cannot outlive the worker, and cannot be used concurrently. A mutable
borrow is the primary guard against reentrant use.

Beginning a nested transaction on an occupied worker is an `InvalidUse` error
in v1. Adapters and pre-install hooks cannot begin one indirectly.

Dropping an active transaction aborts it. This is the Rust equivalent of the
paper's hidden transaction guard. Drop cleanup MUST be bounded and MUST NOT
panic.

The internal commit states are represented by private typestates or guards so
that every acquired lock has exactly one release path. The first release does
not expose a long-lived public `PreparedTransaction`; see
[Section 17.3](#173-distributed-prepare-is-not-a-core-v1-feature).

### 6.4 No async suspension

A transaction MUST NOT cross `.await`, migrate threads, execute blocking I/O,
or yield a worker while it holds STO locks or native resource guards. The core
does not implement `Send` for active transaction states. An upper system that
needs distributed waiting must define a separate protocol and liveness model.

## 7. Transaction items

### 7.1 Stable identity

Each item has:

```text
ItemIdentity = (ObjectId, ResourceClass, ResourceKey)
```

`ObjectId` is unique and never reused while reachable objects or transactions
exist. `ResourceClass` distinguishes adapter resource domains such as table
membership and records. `ResourceKey` is an adapter-owned, stable,
equality-comparable and totally ordered value. Together they MUST describe
logical identity, not a temporary address, cursor, vector slot that can move,
or hash value that can collide.

In the Rust interface, `ResourceKey` is the associated `Key` of the relevant
`TransactionalResource` implementation. Erased key equality or ordering is
attempted only after `ObjectId`, `ResourceClass`, and the stored key `TypeId`
match. A mismatch is an invalid registration or poisoned core state, never an
unchecked downcast.

The core may hash the identity for lookup, but correctness MUST use full
equality. Hash collisions are ordinary collisions, not aliases.

Logical identity and physical lock identity are distinct. During preflight,
every staged write contributes one or more canonical requests of the form:

```text
LockIdentity = (LockNamespaceId, LockClass, LockKey)
```

`LockNamespaceId` is unique within a runtime for one physical lock namespace.
The same non-reentrant physical lock MUST always map to the same full
`LockIdentity`, even when it conservatively protects several logical resources
or objects; distinct physical locks MUST NOT compare equal. Equality and total
order are correctness properties, not hash properties.

### 7.2 Deduplication and mutation composition

The transaction owns an item vector plus an expected-`O(1)` index from identity
to vector position. Looking up the same identity always returns the same item.
Repeated operations merge into that item rather than creating duplicate locks
or independent writes.

Every adapter defines a deterministic transition table for repeated mutations.
For a map-like record, the transaction-local state is conceptually:

```text
Observed: Unknown | Absent(version) | Present(version, snapshot)
Staged:   Unchanged | Absent | Present(new_snapshot)
```

Reads return `Staged` when present. Insert, put, and remove update `Staged`
according to their documented abstract semantics.

### 7.3 Item contents

An item may own:

- read observations;
- a predicate token and its covering observations;
- a staged write intent;
- adapter scratch state prepared before locking;
- an acquired lock token; and
- cleanup metadata.

The original `TItem` made a read version and predicate value mutually exclusive
before predicate upgrade. Rust SHOULD encode that lifecycle explicitly, for
example as `Unobserved | Read | Predicate | UpgradedPredicate`, rather than
permit unchecked combinations of observation fields.

External adapters implement the typed protocol in Section 8.
`Transaction::with_item`, resolved access, item sessions, and the exactly
unique batch-append lane feed the same private typed item storage. The core
performs `TypeId`-checked key and item downcasts; no public `Any`,
caller-supplied vtable, or unchecked extraction exists. Applications normally
call adapter methods such as `get` and `put`, not this adapter-author surface
directly.

### 7.4 Total physical lock order

**[RUST]** The correctness-first implementation deduplicates physical lock
requests by full `LockIdentity`, sorts the unique requests by that identity,
acquires them in ascending order, and releases them exactly once in reverse
order. Logical items sharing a coarse lock retain independent observations and
install state, but share one core-owned lock token. Sorting `ItemIdentity` alone
is insufficient because different logical orderings can alias the same pair of
physical locks and form a cycle.

For the Masstree adapter, a table's membership lock sorts before its record
locks. It is acquired first and released last, after all affected record states
and versions are published.

An adapter that cannot expose a canonical identity for an internal physical
lock MUST use a strictly nonblocking or finitely bounded acquisition attempt.
It MUST never wait indefinitely while holding another lock, and it MUST release
every successful acquisition exactly once on failure. This fallback may cause
false aborts and is not permission to hide a blocking mutex from the lock plan.

This deliberately differs from the paper, which used item insertion order plus
bounded spinning. The paper reports a nearly 10% combined TPC-C improvement
from expected-`O(1)` item lookup and avoiding write-set sorting, without
isolating sorting's contribution. The sorted policy may be replaced only after
deterministic schedule tests and benchmarks show that an unsorted,
abort-on-contention policy preserves safety and improves relevant workloads.

## 8. Rust adapter interface and protocol

The v1 signatures in this section are the normative adapter-author contract.
The ownership, type-state, phase, fallibility, and safe-trait properties are
design requirements. The signature blocks use `unimplemented!()` only to keep
the document focused on public shape; the corresponding real methods and an
out-of-crate adapter contract fixture compile in `crates/sto-core`.

### 8.1 Mapping the C++ class abstraction to Rust

Figure 1 of the paper and the current implementation use inheritance because a
heterogeneous transaction stores `TObject*` and invokes virtual commit
callbacks. Rust uses a typed trait at the extension boundary and a sealed
object-safe shim inside the core. It does **not** expose a public inheritance
hierarchy or ask adapters to implement type erasure.

| C++ STO abstraction | Rust STO counterpart | Deliberate difference |
| --- | --- | --- |
| `TObject` subclass | `TransactionalResource` implementation held by `RegisteredResource<A>` | Associated types replace untyped per-item payloads. |
| `TItem` / `TransItem` | private `ItemBox<A>` with an explicit observation and preparation state machine | The core, not an adapter, owns flags and legal state transitions. |
| `Sto::item(obj, key)` / `TransProxy` | `Transaction::with_item(resource, key, operation)` and scoped `Entry<A>` | Explicit transaction borrowing replaces TLS; an entry cannot escape the operation. |
| `TransProxy::{observe, add_read, set_predicate, add_write}` | `Entry::{record_read, record_predicate, stage}` plus typed accessors | The core checks legal state transitions instead of mutating public flags. |
| `rdata`, predicate, and `wdata` slots | `A::Observation`, `A::Predicate`, and `A::Intent` | No raw pointer union or caller-selected cast. |
| `TransProxy::stash` / datatype-private item data | `A::Local` through scoped `Entry` and phase views | Owned typed state replaces an unmarked `void*` payload. |
| `TObject::lock` | `A::preflight` emits `LockRequest<L>`; the core later calls `L::try_acquire` | Logical items and deduplicated physical locks are separate. |
| `check_predicate(item, false)` | `A::revalidate_predicate` | Execution-time opacity check does not change item state. |
| `check_predicate(item, true)` | `A::upgrade_predicate` | On success the core changes `Predicate` to `UpgradedPredicate`. |
| `TObject::check` | `A::validate_read` | Validation receives only phase-appropriate capabilities. |
| `TObject::install` | `A::install` | Called only after an internal irreversible permit exists; no `Result`. |
| `TObject::unlock` | `TransactionLock::release` | One core-owned guard selects abort, commit, or indeterminate publication. |
| `TObject::cleanup` | `A::finish` | Runs after all physical locks are released on definite outcomes. |
| `Sto::commit_id()` | `InstallContext::occ_commit_id` and committed `LockDisposition` | Core OCC identity is phase-scoped and remains distinct from upper timestamps. |
| `new_item`, `fresh_item`, `read_item`, `check_item` | `with_item`, resolved access, `with_unique_item_batch`, and `ObservationState` transitions | The fast lane requires a core-checked exact uniqueness proof and an empty transaction; it exposes no unchecked item access. |
| clear/user flags | private observation/preparation states and `A::Local` | Adapters own typed local data, not core flag bits. |
| `TObject::print` | ordinary adapter diagnostics outside the commit protocol | No formatting callback runs while committing. |
| Mako `get_table_id` / `get_is_remote` extensions | upper integration layer | Distribution and table-routing policy are not STO callbacks. |
| static `Sto` / current transaction TLS | `WorkerContext` and `Transaction<'worker, Active>` | Worker affinity and transaction lifetime are explicit. |

This split preserves the paper's central virtual-callback seam while making the
equivalent of `TItem` a core-controlled generic type. A datatype exposes normal
methods such as `get`, `put`, or `increment`; those methods use the entry API
below and are not themselves required trait methods.

### 8.2 Object registration and typed item access

One transactional object can expose several resource classes. For example, one
Masstree table registers a record resource and a membership resource under the
same `ObjectId`. Each `(ObjectId, ResourceClass)` pair binds exactly one
adapter type and key type for the lifetime of the registration.
`RegisteredResource<A>` is a cloneable `Send + Sync`, one-`Arc` handle to a
private immutable typed binding. That allocation contains the adapter, object
lease, runtime/object identity, resource class, and cached registration type
proof. Dropping an `ObjectRegistration` cannot invalidate a live resource or
transaction item, and cloning a handle preserves exact binding identity with
one reference-count operation.

```rust
use std::{fmt, hash::Hash, sync::Arc};

pub trait ResourceKey:
    Clone + Eq + Ord + Hash + fmt::Debug + Send + Sync + 'static
{
}

impl<T> ResourceKey for T where
    T: Clone + Eq + Ord + Hash + fmt::Debug + Send + Sync + 'static
{
}

pub struct ObjectRegistration { /* private RuntimeId and ObjectId lease */ }
pub struct RegisteredResource<A: TransactionalResource> { /* private */ }
pub struct Entry<'entry, A: TransactionalResource> { /* private */ }
pub struct ResolvedItemSession<'session, A: TransactionalResource> { /* private */ }
pub struct UniqueItemKeys<'keys, K: ResourceKey> { /* private */ }

impl<'keys, K: ResourceKey> UniqueItemKeys<'keys, K> {
    pub fn try_new(keys: &'keys [K]) -> Option<Self> {
        unimplemented!("signature-only design target")
    }
}

impl<A: TransactionalResource> Clone for RegisteredResource<A> {
    fn clone(&self) -> Self {
        unimplemented!("signature-only design target")
    }
}

impl<A: TransactionalResource> RegisteredResource<A> {
    pub fn adapter(&self) -> &A {
        unimplemented!("signature-only design target")
    }
}

impl Runtime {
    pub fn register_object(
        self: &Arc<Self>,
    ) -> Result<ObjectRegistration, RegistrationError> {
        unimplemented!("signature-only design target")
    }
}

impl ObjectRegistration {
    pub fn register_resource<A: TransactionalResource>(
        &self,
        class: ResourceClass,
        adapter: A,
    ) -> Result<RegisteredResource<A>, RegistrationError> {
        unimplemented!("signature-only design target")
    }
}

impl<'worker> Transaction<'worker, Active> {
    pub fn with_item<A, R>(
        &mut self,
        resource: &RegisteredResource<A>,
        key: A::Key,
        operation: impl for<'entry> FnOnce(
            &mut Entry<'entry, A>,
        ) -> Result<R, AccessError>,
    ) -> Result<R, AccessError>
    where
        A: TransactionalResource,
    {
        unimplemented!("signature-only design target")
    }

    pub fn with_unique_item_batch<A>(
        &mut self,
        resource: &RegisteredResource<A>,
        keys: UniqueItemKeys<'_, A::Key>,
        operation: impl for<'entry> FnMut(
            usize,
            &mut Entry<'entry, A>,
        ) -> Result<(), AccessError>,
    ) -> Result<(), AccessError>
    where
        A: TransactionalResource,
    {
        unimplemented!("signature-only design target")
    }

    pub fn with_item_session<A, R>(
        &mut self,
        resource: &RegisteredResource<A>,
        operation: impl for<'session> FnOnce(
            &mut ResolvedItemSession<'session, A>,
        ) -> Result<R, AccessError>,
    ) -> Result<R, AccessError>
    where
        A: TransactionalResource,
    {
        unimplemented!("signature-only design target")
    }
}

impl<A: TransactionalResource> ResolvedItemSession<'_, A> {
    pub fn try_with_unique_item_batch(
        &mut self,
        keys: UniqueItemKeys<'_, A::Key>,
        operation: impl for<'entry> FnMut(
            usize,
            &mut Entry<'entry, A>,
        ) -> Result<(), AccessError>,
    ) -> Result<bool, AccessError> {
        unimplemented!("signature-only design target")
    }
}
```

For every `ResourceKey`, `Eq`, `Ord`, and `Hash` MUST agree, and the value's
logical meaning MUST NOT mutate while stored in a transaction. Interior
mutation that changes comparison or hashing can create duplicate logical items
even in otherwise safe Rust and is an adapter-contract violation.

An external datatype normally wraps the registered handle and exposes its own
abstract operations:

```rust
pub struct TxnMap {
    records: RegisteredResource<RecordAdapter>,
}

impl TxnMap {
    pub fn get(
        &self,
        txn: &mut Transaction<'_, Active>,
        key: RecordKey,
    ) -> Result<Option<Value>, AccessError> {
        let adapter = self.records.adapter();
        txn.with_item(&self.records, key, |entry| adapter.get(entry))
    }
}
```

`RecordAdapter::get` is ordinary external-crate code: it soundly snapshots its
shared datatype, records the typed observation through `Entry`, overlays any
staged intent, and only then returns the abstract result.

`with_item` MUST:

1. reject a resource registered in another runtime;
2. form the full identity `(ObjectId, ResourceClass, key)` and use hashing only
   to find candidates, never to replace full equality;
3. return the existing typed item when that identity is already present;
4. on a miss, call `A::new_local`, create one `ItemBox<A>`, and insert it through
   the one private typed-to-erased path;
5. check the registered adapter and key `TypeId`s before every erased comparison
   or downcast; and
6. arm a transaction-doomed guard at `with_item` entry, before runtime checks,
   `new_local`, insertion, or `operation`, and clear it only after the entire
   call returns `Ok`.

Consequently, an outer `AccessError` or an unwind leaves the transaction
definitely non-committable even if application code catches or ignores it.
Abstract datatype outcomes such as `NotFound` or `DuplicateKey` belong inside
`R`, not in the outer `AccessError`; for example, an adapter may return
`Result<Result<Value, NotFound>, AccessError>`. Such an inner outcome is still a
completed abstract operation and must leave the item in its documented state;
a partially completed adapter operation returns the outer error instead.

The frame may cache the most recently successful binding validation by the
nonzero allocation-address identity of its compact `RegisteredResource`
binding. An address-equal hit can skip repeated runtime and `TypeId` checks,
but the erased address is never dereferenced or converted back into an `Arc`. Successful item
access retains a strong binding handle in the live item; any failure before
that retention dooms the transaction, so the token cannot be consulted again.
The cache is reset at every transaction begin. Distinct classes, adapter types,
and runtimes always have distinct live binding allocations and take the full
validation path.

`with_unique_item_batch` is a safe optimization for a small batch whose stable
item identities are already available. `UniqueItemKeys::try_new` compares each
key with full `Eq` equality and returns no proof if any key repeats; hashes are
not part of the proof. The operation requires an otherwise empty transaction
and one registered resource binding, so exact key uniqueness implies uniqueness
of every full `(ObjectId, ResourceClass, key)` identity. Core checks the whole
batch against the configured item limit and reserves its item-vector capacity
before initialization. It then appends pooled typed boxes in input order
without hashing, probing, or populating the item index.

The operation receives the ordinary full `Entry`, so observations, predicates,
and intents retain their normal transition rules. If a later ordinary or
resolved access follows the batch, core notices that the index length trails
the live item count, reserves the complete open-addressed table, hashes the
unique prefix once, and installs every entry before the later lookup. Full
erased-identity equality still resolves hash collisions, and a later access to
a batched identity therefore reuses its snapshot or staged intent. A commit or
abort needs no item index and leaves an all-batch transaction unindexed. Any
misuse, capacity failure, callback error, or unwind dooms the transaction; only
fully initialized prefix items participate in abort cleanup. Successful finish
retains the same worker-local typed boxes and adapter bindings as the ordinary
path.

The same append operation is available inside `ResolvedItemSession` as
`try_with_unique_item_batch`. It returns `Ok(false)` without invoking the
operation or changing transaction state when an earlier item makes the frame
nonempty. An adapter can therefore perform lookup and prefetch work once, try
the append lane, and take ordinary per-item session lookup on `false` without
opening another failure boundary. A real append error is latched as the
session's first error, and later session operations report a doomed
transaction.

### 8.3 The transactional-resource trait

The associated types are the safe Rust replacement for `TItem`'s untyped
read, predicate, write, and scratch slots. Observations and predicates report
whether their version is ordered with the runtime opacity clock.

```rust
pub trait OpacityToken: 'static {
    fn observation_order(&self) -> ObservationOrder;
}

pub enum ObservationOrder {
    Ordered(OccVersion),
    Unordered,
}

pub type PreflightFreeReadValidate<A> = for<'context> fn(
    &A,
    &<A as TransactionalResource>::Key,
    &<A as TransactionalResource>::Observation,
    &PreflightFreeValidationContext<'context>,
) -> Result<(), CheckError>;

pub type PreflightFreeReadFinish<A> = for<'item, 'context> fn(
    &A,
    &<A as TransactionalResource>::Key,
    FinishItem<'item, A>,
    &mut FinishContext<'context>,
);

pub struct PreflightFreeReadCapability<A: TransactionalResource> { /* private */ }

impl<A: TransactionalResource> PreflightFreeReadCapability<A> {
    pub const fn new(
        validate: PreflightFreeReadValidate<A>,
        finish_committed: PreflightFreeReadFinish<A>,
    ) -> Self {
        unimplemented!("signature-only design target")
    }

    pub const fn new_drop_only(
        validate: PreflightFreeReadValidate<A>,
    ) -> Self {
        unimplemented!("signature-only design target")
    }
}

pub trait TransactionalResource: Send + Sync + Sized + 'static {
    type Key: ResourceKey;
    type Local: 'static;
    type Observation: OpacityToken;
    type Predicate: OpacityToken;
    type Intent: 'static;
    type Prepared: 'static;

    fn new_local(
        &self,
        key: &Self::Key,
    ) -> Result<Self::Local, ItemInitError>;

    fn preflight_free_read_capability(
        &self,
    ) -> Option<&'static PreflightFreeReadCapability<Self>> {
        None
    }

    fn preflight(
        &self,
        key: &Self::Key,
        item: PreflightItem<'_, Self>,
        cx: &mut PreflightContext<'_>,
    ) -> Result<Self::Prepared, PrepareError>;

    fn revalidate_read(
        &self,
        key: &Self::Key,
        observation: &Self::Observation,
        cx: &ExecutionCheckContext<'_>,
    ) -> Result<(), CheckError>;

    fn revalidate_predicate(
        &self,
        key: &Self::Key,
        predicate: &Self::Predicate,
        cx: &ExecutionCheckContext<'_>,
    ) -> Result<ObservationOrder, CheckError>;

    fn upgrade_predicate(
        &self,
        key: &Self::Key,
        predicate: &Self::Predicate,
        prepared: &Self::Prepared,
        cx: &PredicateContext<'_>,
    ) -> Result<Self::Observation, CheckError>;

    fn validate_read(
        &self,
        key: &Self::Key,
        observation: &Self::Observation,
        prepared: &Self::Prepared,
        cx: &ValidationContext<'_>,
    ) -> Result<(), CheckError>;

    fn install(
        &self,
        key: &Self::Key,
        item: InstallItem<'_, Self>,
        prepared: &mut Self::Prepared,
        cx: &mut InstallContext<'_>,
    );

    fn finish(
        &self,
        key: &Self::Key,
        item: FinishItem<'_, Self>,
        prepared: Option<&mut Self::Prepared>,
        disposition: FinishDisposition,
        cx: &mut FinishContext<'_>,
    );
}

pub struct PreflightItem<'a, A: TransactionalResource> { /* private */ }
pub struct InstallItem<'a, A: TransactionalResource> { /* private */ }
pub struct FinishItem<'a, A: TransactionalResource> { /* private */ }

impl<A: TransactionalResource> PreflightItem<'_, A> {
    pub fn local(&self) -> &A::Local {
        unimplemented!("signature-only design target")
    }

    pub fn local_mut(&mut self) -> &mut A::Local {
        unimplemented!("signature-only design target")
    }

    pub fn observation(&self) -> ObservationRef<'_, A> {
        unimplemented!("signature-only design target")
    }

    pub fn intent(&self) -> Option<&A::Intent> {
        unimplemented!("signature-only design target")
    }

    pub fn intent_mut(&mut self) -> Option<&mut A::Intent> {
        unimplemented!("signature-only design target")
    }
}

impl<A: TransactionalResource> InstallItem<'_, A> {
    pub fn local_mut(&mut self) -> &mut A::Local {
        unimplemented!("signature-only design target")
    }

    pub fn observation(&self) -> ObservationRef<'_, A> {
        unimplemented!("signature-only design target")
    }

    pub fn intent(&self) -> &A::Intent {
        unimplemented!("signature-only design target")
    }
}

impl<A: TransactionalResource> FinishItem<'_, A> {
    pub fn local_mut(&mut self) -> &mut A::Local {
        unimplemented!("signature-only design target")
    }

    pub fn observation(&self) -> ObservationRef<'_, A> {
        unimplemented!("signature-only design target")
    }

    pub fn remaining_intent(&self) -> Option<&A::Intent> {
        unimplemented!("signature-only design target")
    }

    pub fn take_remaining_intent(&mut self) -> Option<A::Intent> {
        unimplemented!("signature-only design target")
    }
}
```

There are intentionally no default validation implementations: returning
`Ok(())` accidentally would silently remove conflict coverage. The core calls
callbacks only for the corresponding item state; a resource must still
implement each method explicitly. `Local = ()` is valid when no datatype-
private operation state is needed, and `Prepared = ()` is valid when no commit
scratch or physical lock is needed. `new_local` constructs owned
transaction-local state only; it MUST NOT perform an unrecorded shared read.

The `ObservationOrder` returned by `revalidate_predicate` is the bound through
which the unchanged predicate was just certified. The core advances its
checked-through opacity bound but does not replace the predicate or perform the
commit-time state transition. `Unordered` forces the conservative full-
revalidation path.

`PreflightItem`, `InstallItem`, and `FinishItem` have private fields and expose
only phase-appropriate typed accessors. `PreflightItem` can inspect observations,
predicates, and intents. `InstallItem` can borrow but cannot move the core-owned
intent, keeping it reachable if installation unwinds. Only `FinishItem`, after
all locks are gone and the outcome is definite, can take the remaining intent
or drain cleanup stored in `Local` and `Prepared`. None exposes the enclosing
`Transaction` or permits insertion of another item.

After a successful `finish`, the core drops any untaken intent, then prepared
state, observation/predicate state, local state, and key, all outside
transaction locks and inside panic containment. A reusable worker-local item
box may retain its immutable typed resource binding, avoiding shared reference-
count writes when the next transaction uses the same binding. Retention is
bounded by that worker's peak item count and therefore by
`max_items_per_transaction`. The binding is disposed under its own panic
boundary when the slot is rebound or the worker is dropped. Each cleanup step
first leaves an empty core slot and then drops the removed value. On a cleanup
panic the core does not retry a partially run callback or continue destructing
uncertain adapter state; it quarantines the remaining frame or scratch and
poisons with the already-known outcome.

### 8.4 Core-owned item state and operation-time entry

The following private state is the semantic replacement for C++ `TItem` flags.
The exact storage layout may change, but its legal states may not be weakened.

```rust
enum ObservationState<O, P> {
    Unobserved,
    Read(O),
    Predicate(P),
    UpgradedPredicate(O),
}

enum PreparationState<P> {
    Unprepared,
    Prepared(P),
    Installed(P),
}

struct ItemBox<A: TransactionalResource> {
    intent: Option<A::Intent>,
    preparation: PreparationState<A::Prepared>,
    observation: ObservationState<A::Observation, A::Predicate>,
    retained_predicate: Option<A::Predicate>,
    local: Option<A::Local>,
    key: Option<A::Key>,
    resource: Option<RegisteredResource<A>>,
}

pub enum ObservationRef<'a, A: TransactionalResource> {
    Unobserved,
    Read(&'a A::Observation),
    Predicate(&'a A::Predicate),
    UpgradedPredicate(&'a A::Observation),
}

impl<A: TransactionalResource> Entry<'_, A> {
    pub fn local(&self) -> &A::Local {
        unimplemented!("signature-only design target")
    }

    pub fn local_mut(&mut self) -> &mut A::Local {
        unimplemented!("signature-only design target")
    }

    pub fn observation(&self) -> ObservationRef<'_, A> {
        unimplemented!("signature-only design target")
    }

    pub fn intent(&self) -> Option<&A::Intent> {
        unimplemented!("signature-only design target")
    }

    pub fn intent_mut(&mut self) -> Option<&mut A::Intent> {
        unimplemented!("signature-only design target")
    }

    pub fn record_read(
        &mut self,
        observation: A::Observation,
    ) -> Result<(), AccessError> {
        unimplemented!("signature-only design target")
    }

    pub fn record_predicate(
        &mut self,
        predicate: A::Predicate,
    ) -> Result<(), AccessError> {
        unimplemented!("signature-only design target")
    }

    pub fn stage(&mut self, intent: A::Intent) -> Result<(), AccessError> {
        unimplemented!("signature-only design target")
    }
}

```

The upgraded form moves the old predicate into the separate retained field so
its destructor cannot run while the commit lock plan is held and so observation
and predicate destructors have distinct unwind boundaries. Likewise, a released
`TransactionLock::Guard` is made inert by `release` but retained in the plan
until every physical lock has been released. Post-unlock teardown extracts and
drops each guard before its target, one frame and one unwind boundary at a time;
on the first destructor panic it retains the rest. These retained values and
the optional item fields make teardown order enforceable even when an associated
type has a nontrivial destructor.

In the implemented serializable profile, `Entry` is a scoped typed borrow of an
already `TypeId`-checked `ItemBox<A>`; it is not a pointer into erased storage,
and its higher-ranked operation closure prevents it from escaping.
An opaque profile may replace that internal representation with a transaction
reference plus slot index so `record_read` and `record_predicate` can trigger
whole-transaction execution-time revalidation before exposing a result. That
change must preserve this public API. The adapter is responsible for same-item
operation composition and read-your-writes, using `local`, `observation`,
`intent`, and their mutable counterparts before replacing state with `stage`.
The core performs successful predicate and installation transitions itself, so
an adapter cannot install twice or claim that a predicate was upgraded when its
callback failed.

`record_read` permits `Unobserved -> Read`; `record_predicate` permits
`Unobserved -> Predicate`; successful predicate upgrade is the only
`Predicate -> UpgradedPredicate` transition. A repeated operation inspects and
reuses the existing observation rather than overwriting it with a later token.
The optional intent is orthogonal to those states, and `stage` replaces only an
already-composed intent. An illegal transition returns `AccessError`, dooms the
transaction, and is never repaired by discarding the earlier observation.

### 8.5 Canonical physical-lock trait

Logical callbacks plan locks, but the core owns every acquired guard. V1
requires a `'static` guard rather than a borrowed `MutexGuard<'a, T>`; this
avoids self-referential transaction storage. The guard may own its target, or
it may be a detached token: the lock frame retains the canonical `Arc<L>` at a
stable address until `release` and guard destruction have both completed. A
callback or destructor unwind quarantines that frame, retaining the target
rather than invalidating such a token. A guard may be `!Send` and `!Sync`
because the transaction is worker-affine.

```rust
pub trait TransactionLock: Send + Sync + 'static {
    type Guard: 'static;

    fn try_acquire(
        &self,
        identity: &LockIdentity,
        cx: &AcquireContext<'_>,
    ) -> Result<Self::Guard, AcquireError>;

    fn release(
        &self,
        guard: &mut Self::Guard,
        disposition: LockDisposition,
        cx: &ReleaseContext<'_>,
    );
}

pub enum LockDisposition {
    Aborted,
    Committed { occ_commit_id: Option<OccCommitId> },
    Indeterminate { occ_commit_id: Option<OccCommitId> },
}

pub struct LockRequest<L: TransactionLock> { /* private identity and Arc<L> */ }
pub struct LockUse<L: TransactionLock> { /* private plan nonce and slot */ }

impl<L: TransactionLock> LockRequest<L> {
    pub fn new(identity: LockIdentity, target: Arc<L>) -> Self {
        unimplemented!("signature-only design target")
    }
}

impl PreflightContext<'_> {
    pub fn require_lock<L: TransactionLock>(
        &mut self,
        request: LockRequest<L>,
    ) -> Result<LockUse<L>, PrepareError> {
        unimplemented!("signature-only design target")
    }
}

impl PredicateContext<'_> {
    pub fn guard<L: TransactionLock>(
        &self,
        use_: &LockUse<L>,
    ) -> Result<&L::Guard, AdapterFault> {
        unimplemented!("signature-only design target")
    }
}

impl ValidationContext<'_> {
    pub fn guard<L: TransactionLock>(
        &self,
        use_: &LockUse<L>,
    ) -> Result<&L::Guard, AdapterFault> {
        unimplemented!("signature-only design target")
    }
}

impl InstallContext<'_> {
    pub fn guard_mut<L: TransactionLock>(
        &mut self,
        use_: &LockUse<L>,
    ) -> Result<&mut L::Guard, AdapterFault> {
        unimplemented!("signature-only design target")
    }

    pub fn occ_commit_id(&self) -> Option<OccCommitId> {
        unimplemented!("signature-only design target")
    }
}
```

`require_lock` allocates one private erased lock frame during preflight. That
frame contains an inline `Option<L::Guard>`, so `try_acquire` can place its
result into already allocated storage. Lock acquisition MUST be bounded and
nonblocking and MUST NOT allocate. `Err` leaves the lock unowned. A successful
guard remains in core custody until release; `release` makes it inert and does
not fail or panic.

The core deduplicates and sorts by full `LockIdentity`. Duplicate requests must
also have the same lock target `TypeId` and canonical `Arc` instance before they
share a frame; a mismatch is `AdapterFault`, not an unchecked cast. `LockUse<L>`
is unforgeable and tied to one plan nonce. Every phase-context access checks
that nonce, slot, identity, and `TypeId` before downcasting. A mismatch during
predicate upgrade or validation is a pre-irrevocable `AdapterFault`; presenting
a stale token for the first time during `install` is an adapter-contract
violation that becomes indeterminate and quarantines the runtime, never an
unchecked cast or undefined behavior. A correct `Prepared` value retains only
the `LockUse` values returned by its current `PreflightContext`.

A private release guard initially has fallback disposition `Aborted`. At the
`Irrevocable` transition it changes the fallback to `Indeterminate`, which must
conservatively advance or preserve publication and quarantine the runtime; it
must never restore the pre-lock generation after partial installation. The
normal path explicitly releases in reverse order with `Committed`. If a release
callback panics, the core retains or deliberately leaks the frame for diagnosis
rather than losing the only remaining guard.

### 8.6 Phase capabilities and private erasure

All phase contexts have private constructors and fields, carry a non-cloneable
lifetime, and are `!Send + !Sync`. Their capabilities are deliberately narrow:

- `PreflightContext` builds the physical lock plan and reserves bounded scratch;
- `AcquireContext` exposes the opaque current `LockOwner` only;
- `ExecutionCheckContext` exposes the opacity bound needed to revalidate prior
  reads or predicates;
- `PredicateContext` provides immutable access to already-held guards, before a
  core commit ID necessarily exists;
- `ValidationContext` provides immutable guards and core OCC metadata;
- `InstallContext` exists only with the private irreversible permit and provides
  mutable held guards;
- `ReleaseContext` supplies only the metadata needed to publish or abort a held
  lock; and
- `FinishContext` permits retirement of displaced resources after locks are
  gone.

These context types are public and nameable so an external crate can implement
the traits, but their fields and constructors remain opaque. The core supplies
the non-lossy conversions needed for `?`, including `ItemInitError` into the
outer `AccessError` and `AdapterFault` into `PrepareError`, `AcquireError`, and
`CheckError`; none of those conversions turns a fault into `Conflict`.

No context can be retained, construct or borrow a `Transaction`, perform a new
item lookup, acquire an unplanned blocking lock, or invoke arbitrary application
code. No commit callback receives `&mut Transaction`, preventing callback
reentry and item-set mutation while the core iterates it.

The registered adapter `A` and its handle are `Send + Sync`. `A::Local`,
`A::Observation`, `A::Predicate`, `A::Intent`, `A::Prepared`, and `L::Guard`
are owned and `'static` but intentionally need not be `Send` or `Sync`; they
remain inside the worker-affine transaction.

`TransactionalResource` is intentionally not object-safe. Although another
public trait may be technically object-safe after fixing its associated types,
the API exposes no public adapter or lock trait objects. Internally,
`ItemBox<A>` blanket-implements a sealed `ErasedItem` vtable, and a corresponding
sealed lock frame stores `L::Guard`. The transaction may therefore hold
heterogeneous items and locks, while `Any::downcast_ref`/`downcast_mut` checks
every transition. The erased traits and their constructors are private to
`sto-core`; external crates never implement them, name `Any`, or supply a
vtable. There is no `unsafe` public trait in this protocol.

Conceptually, the private item vtable has this shape:

```rust
trait ErasedItem: sealed::Sealed {
    fn identity(&self) -> &ItemIdentity;
    fn concrete_type_id(&self) -> TypeId;
    fn as_any_mut(&mut self) -> &mut dyn Any;
    fn preflight(
        &mut self,
        cx: &mut PreflightContext<'_>,
    ) -> Result<(), PrepareError>;
    fn revalidate_for_opacity(
        &mut self,
        cx: &ExecutionCheckContext<'_>,
    ) -> Result<(), CheckError>;
    fn upgrade_predicate(
        &mut self,
        cx: &PredicateContext<'_>,
    ) -> Result<(), CheckError>;
    fn validate(
        &self,
        cx: &ValidationContext<'_>,
    ) -> Result<(), CheckError>;
    fn install(&mut self, cx: &mut InstallContext<'_>);
    fn finish(
        &mut self,
        disposition: FinishDisposition,
        cx: &mut FinishContext<'_>,
    );
}
```

The blanket implementation dispatches to the registered `A`, passes typed
phase views over `ItemBox<A>`, and owns the state transitions described above.
This trait is shown to make heterogeneous dispatch reviewable; it is not part
of the public adapter API.

### 8.7 Callback phases and fallibility

The callback-specific failures are deliberately smaller than the public
transaction error taxonomy:

```rust
pub enum PrepareError {
    Conflict(Conflict),
    Capacity(CapacityError),
    Fault(AdapterFault),
}

pub enum ItemInitError {
    Capacity(CapacityError),
    Fault(AdapterFault),
}

pub enum AcquireError {
    Conflict(Conflict),
    Fault(AdapterFault),
}

pub enum CheckError {
    Conflict(Conflict),
    Fault(AdapterFault),
}

pub enum FinishDisposition {
    Committed,
    Aborted,
}
```

| Invocation | Phase | Result contract | Core response |
| --- | --- | --- | --- |
| `TransactionalResource::new_local` | first typed lookup | `ItemInitError::{Capacity, Fault}` | Doom the transaction; poison on adapter fault. |
| adapter operation inside `with_item` | execution | outer `AccessError` or unwind | Mark the transaction doomed; no later commit. |
| `TransactionalResource::preflight` | preflight | `PrepareError::{Conflict, Capacity, Fault}` | Definite abort for conflict/capacity; poison for adapter fault. |
| `TransactionLock::try_acquire` | locking | `AcquireError::{Conflict, Fault}` | Reverse-release acquired guards on conflict; poison on fault. |
| `revalidate_read` / `revalidate_predicate` | execution-time opacity | `CheckError::{Conflict, Fault}` | Abort before exposing an inconsistent result; poison on fault. |
| `upgrade_predicate` | predicate upgrade | `CheckError::{Conflict, Fault}` | Definite abort or poison; state changes only on success. |
| `validate_read` | final validation | `CheckError::{Conflict, Fault}` | Definite abort or poison before irreversibility. |
| optional upper hook | after validation | reject or contained panic | Definite abort under the stronger no-visible-effect hook contract. |
| `install` | irreversible install | no `Result`; must not panic | Panic yields `Indeterminate` and quarantine. |
| `TransactionLock::release` | reverse publication/unlock | no `Result`; must not panic | Retain/quarantine; report indeterminate unless publication is proved. |
| `finish` | post-unlock cleanup | no `Result`; must not panic | Poison with the already-known `DefiniteOutcome`; never invite retry. |

`ItemInitError`, `PrepareError`, `AcquireError`, and `CheckError` distinguish
ordinary capacity or conflict outcomes from `AdapterFault`; a fault is never
converted into a retryable conflict.
Installation, release, and finish have no error return because there is no safe
abstract rollback after `Irrevocable` and no remaining lock during finish that
can be reacquired out of order.

For every definite committed or aborted path whose lock callbacks all return,
including explicit abort and Drop, the core calls `finish` exactly once for
every inserted item. If `finish` itself violates the contract, the
already-known outcome is returned as `CommitFailure::Poisoned { outcome, .. }`;
the callback is not retried, and unfinished cleanup state is retained for
diagnosis. An unwind from acquire or abort-release leaves that callback frame's
guard state uncertain. Although no installation began and the abstract outcome
is definitely aborted, the core MUST retain the lock plan and item vector and
MUST NOT invoke the post-unlock `finish` callback. If publication becomes
indeterminate after irreversibility, the core likewise does not invent a
`FinishDisposition::Committed` or `Aborted`: it uses indeterminate lock release,
quarantines the runtime, and retains the item vector as Section 10.5 requires.

The execution-time `with_item` closure may run ordinary synchronous adapter
code and access its registered datatype, but it cannot `.await`, migrate the
worker, perform blocking I/O, or reenter the mutably borrowed transaction. No
preflight-or-later callback may perform network or disk I/O, await, invoke
arbitrary application code, recursively enter STO on the same worker, or
acquire an untracked blocking lock. `install` MUST NOT drop displaced `Arc`s,
buffers, or foreign allocations while transaction locks are held. It moves
them into preallocated cleanup state;
`finish(FinishDisposition::Committed)` releases or retires them after all
transaction locks are published and released.

### 8.8 Normative adapter obligations

The following adapt the six correctness rules in Section 4.1 of the EuroSys
paper:

1. **Coverage.** If transaction A changes state that can affect an abstract
   result observed by B, B records at least one token that A changes before B
   can commit.
2. **Exclusive mutation.** Two transactions cannot simultaneously own mutation
   authority for the same logical resource. Distinct identities MUST NOT denote
   the same logical state accidentally. Distinct logical resources MAY
   conservatively share a physical lock, but every alias MUST produce the same
   `LockIdentity` so the core acquires that lock at most once per transaction
   while retaining each resource's independent validation state.
3. **Predicate soundness.** Predicate upgrade fails if relevant state no longer
   satisfies the recorded semantic condition, and its resulting token set
   covers later relevant changes.
4. **Read soundness.** Read validation fails if the resource changed or is
   locked by another transaction. A resource locked by the validating
   transaction may match its own observed pre-lock version.
5. **Abstract invisibility.** Mutations are not visible before `install`, except
   audited eager scaffolding that remains logically absent or poisoned.
6. **Deadlock avoidance.** Every exposed physical lock participates in the
   canonical total order. A hidden internal lock uses only a strictly
   nonblocking or finitely bounded acquisition attempt with a proven exact-once
   release path; it never waits indefinitely while another lock is held.

Rust STO adds:

7. **Lifetime.** Every token and associated value is owned for the transaction
   and remains valid through validation and cleanup without an unguarded
   foreign pointer.
8. **Infallible installation.** All allocation and fallible computation needed
   by `install`, `TransactionLock::release`, and `finish` completes during
   execution or `preflight`.
9. **Panic freedom.** Resource, `TransactionLock`, validation, install, release,
   and finish callbacks do not panic. A pre-irrevocable callback panic performs
   guarded abort and poisons the resource/runtime; a post-irrevocable panic is
   indeterminate and quarantines it.
10. **Memory-model soundness.** Reads and writes use actual synchronization;
    later version validation does not excuse a Rust data race.
11. **Nontransactional participation.** Any concurrent nontransactional
    operation updates the same locks and versions required to keep transactions
    correct.
12. **Cross-object neutrality.** The adapter does not assume callback position,
    sole ownership of the transaction, or a particular other adapter.
13. **Owned execution state.** Keys, observations, predicates, intents,
    prepared state, and guards contain no borrow that can outlive its source or
    be invalidated before exact-once cleanup.
14. **Deterministic composition.** Repeated operations on one identity merge in
    its one item and define read-your-writes for every supported sequence.

### 8.9 Deferred and eager updates

The default adapter strategy is deferred update: execution records an intent,
and commit publishes it.

The paper permits direct/eager updates when uncommitted elements are poisoned
and abort cleanup reverses physical state. **[RUST]** Generic eager adapters are
deferred until an explicit capability and cleanup proof exist. The Masstree
adapter's insertion of a logically absent tombstone directory entry is the only
v1 eager structural exception; its abstract state remains absent.

**[STO]** The original callback model also permits datatype-specific mixtures
of optimistic and pessimistic concurrency control, direct and deferred updates,
and abort-time undo. **[RUST/DEFERRED]** V1 intentionally standardizes generic
adapters on deferred OCC. Operation-time pessimistic adapters and general eager
updates are later capabilities, not limitations of STO itself.

## 9. Versions and the Rust memory model

### 9.1 Version roles

The implementation distinguishes:

- `OccVersion`: a record or predicate validation value;
- `OccCommitId`: an ordered core commit value used by opacity-capable records;
- `OwnerId`: the worker encoded in a held version lock;
- `MakoTimestamp`: an upper-layer replication/visibility timestamp; and
- `DurabilitySeq` or `CacheSeq`: persistence/cache sequence values.

These are different Rust newtypes. No implicit integer conversion connects
them.

### 9.2 Legacy parity and native requirements

[`TransactionTid`](../../src/mako/sto/Interface.hh) defines the current C++
layout, which remains a compatibility oracle:

| Field | Mask/value | Meaning |
| --- | ---: | --- |
| owner ID | `0x01ff` | Nine-bit legacy lock owner; values 0–511 are representable. |
| lock | `0x0200` | Resource is being installed or validated by a writer. |
| nonopaque | `0x0400` | Observation is unordered and forces full opacity revalidation. |
| compatibility-reserved | `0x0800` | Legacy adapter user/invalid bit; reserved from the generic native layout. |
| version increment | `0x4000` | Leaves low helper bits clear. |

Native Rust records do not share version words with C++, so this exact bit
allocation is not normative for `AtomicVersion`. The native representation is
opaque outside `sto-core` and MUST provide:

- an ordered generation or an explicit unordered marker;
- exclusive ownership distinguishable from an unlocked value;
- recognition of a lock held by the validating worker;
- checked owner and generation capacity; and
- no wraparound or ABA reuse while an observation can exist.

The first implementation MAY reuse the legacy encoding after accepting its
worker capacity, but generic `sto-core` APIs MUST NOT expose those masks or make
the Masstree worker cap a universal semantic limit. Adapter metadata such as
live/tombstone state uses explicit typed fields rather than shared bit positions.

`OwnerId` allocation is checked against the selected representation. Version
addition MUST NOT wrap in production. Before a value could alias an observable
historical version, the runtime enters an exhausted state and refuses new
transactions until a proved quiescent rebase or restart.

### 9.3 Atomicity does not make payload access safe

Rust defines a conflicting unsynchronized non-atomic access as a data race and
undefined behavior. A seqlock-style “copy mutable bytes, then check the version”
is therefore invalid when the copy can race with a writer, even if the retry
would reject it.

Rust-owned record values MUST be either:

- immutable snapshots published through a sound atomic `Arc`/hazard mechanism;
- copied while holding a real synchronization guard; or
- copied inside C++ while C++ owns the synchronization and the ABI returns only
  owned bytes.

The Masstree v1 design uses immutable Rust-owned record-state snapshots. The
`live` flag and value are in the same snapshot so a reader cannot observe a
torn membership/value pair.

### 9.4 Baseline ordering table

The first implementation favors an auditable ordering baseline. Weaker
orderings require a written happens-before argument plus Loom or equivalent
tests.

| Event | Baseline ordering | Required effect |
| --- | --- | --- |
| Load an unlocked version before a snapshot | `Acquire` | Observe state published before the version. |
| Load immutable snapshot handle | `Acquire` | Retain a fully initialized snapshot. |
| Reload version after snapshot | `Acquire` | Detect intervening lock/install. |
| Acquire version lock with CAS | `AcqRel` success, `Acquire` failure | Own mutation and observe prior state. |
| Publish prepared snapshot under own lock | `Release` | Order initialized payload before commit-unlock. |
| Advance generation while retaining own lock | `Release` | Keep the resource unavailable until publication completes. |
| Commit-unlock with the new generation | `Release` | Atomically make the installed state available to acquiring readers. |
| Abort-unlock to original version | `Release` | Release ownership without publishing a write. |
| Load runtime opacity bound | `Acquire` | Order clock observations. |
| Advance runtime commit clock | `AcqRel` | Produce a unique ordered commit value. |

Rust and C++ atomics are not assumed to have interoperable object layout.
Masstree stores only a scalar `RecordId`; every record atomic is owned and
accessed by Rust.

## 10. Commit, abort, and failure state machine

### 10.1 States

```text
Active
  -> Preflight
  -> Locking
  -> Locked
  -> OptionalUpperMetadata
  -> PredicateUpgrade
  -> CoreCommitMetadata
  -> Validating
  -> Validated
  -> OptionalPreinstallHook
       | reject or contained panic -> Aborting -> Aborted -> Finished
       | accept
       v
  -> Irrevocable
  -> Installing
  -> PublishingAndUnlocking
  -> Committed
  -> Finished

Any other failure before Irrevocable -> Aborting -> Aborted -> Finished
Any uncontained failure at/after Irrevocable -> Poisoned/Indeterminate
```

`Validated` is an internal state, not a durable distributed-prepare promise.

### 10.2 Commit phases

The normative local protocol is:

1. **Preflight.** Deduplicate and finalize items; allocate all write snapshots,
   adapter scratch, and lock vectors; derive, deduplicate, and sort the canonical
   physical lock plan by calling `TransactionalResource::preflight` for each
   item except an eligible prepared-free ordinary read. `Conflict` or
   `Capacity` returns `Ok(CommitOutcome::Aborted(..))` with no locks held.
   `Fault` performs the same definite-abort cleanup but returns
   `CommitFailure::Poisoned` with `DefiniteOutcome::Aborted`.
2. **Acquire planned locks.** For each unique `LockIdentity` in total order,
   call its `TransactionLock::try_acquire` and place the `'static` guard in the
   preallocated erased lock frame. On `Conflict`, call
   `TransactionLock::release` with
   `LockDisposition::Aborted` for already acquired guards exactly once in
   reverse order and return a normal abort. `Fault` uses that same release path
   and returns poisoned with a definite aborted outcome.
3. **Reserve optional upper metadata.** After the full write set is locked, an
   upper compatibility coordinator may reserve a `MakoTimestamp` or equivalent
   preallocated ticket. Reservation failure aborts. Gaps from later validation
   failure are harmless. This value is not an `OccVersion` and does not mean the
   transaction committed.
4. **Upgrade predicates.** Call `TransactionalResource::upgrade_predicate` and
   capture its covering observation while write locks are held. The core changes
   the item to `UpgradedPredicate` only on success. `Conflict` is a normal
   abort; `Fault` abort-cleans and returns poisoned with a definite aborted
   outcome.
5. **Choose core commit metadata.** Before the final validating pass, reserve
   any `OccCommitId` required by the isolation profile. Gaps from validation or
   later hook rejection are allowed. Reserving here makes the global clock order
   compatible with the subsequent validation proof and ensures commit-ID
   exhaustion cannot abort after hook acceptance. Upper `MakoTimestamp`
   allocation remains separate.
6. **Validate reads.** Define the **certification cut** immediately before the
   first check in the final validating pass. Call
   `TransactionalResource::validate_read` for each ordinary prepared item and
   upgraded predicate, and the capability's restricted validation callback for
   each prepared-free read. A value may match the observed version or that
   version locked by this transaction. Because covering versions cannot wrap
   or return to an old observable value, success of the whole pass
   retrospectively certifies that every observation held at this cut. A
   `CheckError::Conflict` is a normal abort; `CheckError::Fault` abort-cleans
   and returns poisoned with a definite aborted outcome.
7. **Run the optional pre-install hook.** For a non-read-only compatibility
   transaction, invoke the hook exactly once after all local writes are locked
   and reads validate, but before the first install. It receives the previously
   reserved upper metadata. Rejection or a contained panic is a definite abort.
   The hook contract, not `sto-core`, MUST guarantee that rejection or panic
   leaves no externally visible effect. The hook MUST use only its documented
   preallocated bookkeeping; it MUST NOT perform blocking I/O, await, or reenter
   STO, and SHOULD complete within the configured hook-watchdog budget. Disjoint
   workers may run hooks concurrently.
8. **Cross the irreversible boundary.** From this point, the transaction cannot
   report conflict or abort.
9. **Install.** Call `TransactionalResource::install` for every item with an
   intent while all resources retain their own-lock state. The method is
   infallible and nonpanicking. Install callbacks may run in any item order;
   correctness MUST NOT depend on their relative position. In particular, a
   membership resource may prepare its next locked generation before or after
   record snapshots, because none is visible while its lock remains held.
10. **Unlock and finish.** Call `TransactionLock::release` with
    `LockDisposition::Committed` in reverse acquisition order, publishing new
    generations. This releases Masstree record resources before their table
    membership resource. Then finish items in reverse order. Ordinary items use
    `TransactionalResource::finish` with `FinishDisposition::Committed`;
    prepared-free reads use their committed-finish callback, or core-owned
    teardown alone when their capability was constructed with `new_drop_only`.
    Return committed only after required cleanup and worker-state restoration
    complete.

The paper's basic protocol uses lock → predicate → validate → version →
install → cleanup. **[RUST/OPAQUE]** This design reserves an opacity-ordering
ID immediately before the final validating pass, matching the standard clock-
then-validate proof even if a worker is preempted. Upper metadata reservation
and the pre-install hook are Mako compatibility seams; they are not part of the
original paper.

#### Prepared-free ordinary reads

An adapter may explicitly expose a stable
`PreflightFreeReadCapability<Self>` for an ordinary `Read` item that has no
intent or predicate. This capability skips only that item's adapter `preflight`
and `Prepared` allocation. Other, heterogeneous items still preflight normally,
the core constructs and acquires the same globally ordered physical lock plan,
and the prepared-free read is certified in the same final validation pass and
at the same certification cut described above. Its capability callback must
therefore implement final certification, not merely execution-time opacity or
an advisory recheck.

`PreflightFreeReadCapability::new` supplies an explicit committed-finish
callback without a `Prepared` value. `new_drop_only` is a stronger adapter
promise: after successful certification, dropping the core-owned item fields is
the complete cleanup, so no adapter committed-finish callback runs. On every
definite abort both forms instead use the ordinary adapter `finish` callback
with `prepared == None` and `FinishDisposition::Aborted`. Capability discovery,
validation, optional committed finish, ordinary abort finish, and teardown all
remain inside the existing per-item panic/fault boundaries. Returning a
capability is a stable adapter contract for the lifetime of an item; callbacks
must be nonpanicking and must preserve the same cleanup obligations as the
ordinary path. Items with an intent, a predicate, or no observation always take
the ordinary preflight path.

For a mixed transaction this is a per-item optimization and retains the normal
heterogeneous lock and validation protocol. If every item is an eligible
prepared-free ordinary read, the core additionally skips creation of the empty
lock plan and runs the restricted final-certification pass directly. Neither
form substitutes execution-time `revalidate_read` for final certification.

### 10.3 Serialization and irreversible points

**[STO/COMPAT]** The paper says a transaction will commit after successful
phase-2 validation. A validation pass is sequential, so its checks do not share
one literal instant. The common certified instant is the certification cut just
before the pass begins: if any covering state changed before that cut, its later
check sees a different non-ABA version or a conflicting lock. Success therefore
assigns this cut retrospectively as the serialization point.

The compatibility hook adds one later rejection point. The certification cut is
assigned only after the hook accepts and installation completes; a rejected
transaction has no committed serialization point. Hook delay cannot move the
chosen point past a change to an already validated read dependency. Reserving
`OccCommitId` immediately before the cut prevents the hook interval itself from
reversing version order and ensures that a lower-ID conflicting writer is either
installed before the cut, remains locked and causes validation to fail, or has
aborted without publication.

The later transition to `Irrevocable`, after hook acceptance, is the failure-
disposition boundary rather than the serialization point. Physical
installations may occur sequentially, but no other transaction can commit a
partial observation through the retained locks. The transaction does not return
until installation and required cleanup finish.

For a read-only transaction, v1 defines the same cut just before its final
validation pass and linearizes there if the whole pass succeeds. A paper-style
opaque read-only fast path may be added only with a proof that execution-time
validation already supplies the same guarantee.

### 10.4 Abort and Drop

Abort performs, exactly once:

1. reverse `TransactionLock::release(LockDisposition::Aborted)` for every
   acquired lock;
2. reverse `TransactionalResource::finish(FinishDisposition::Aborted)` for
   every inserted item;
3. resource-scope exit; and
4. worker transaction-state reset.

Those steps describe callbacks that honor their infallible contract. If an
acquire or abort-release callback unwinds, its physical guard is uncertain;
the runtime is poisoned, other definitely held guards are abort-released when
possible, and item state is quarantined without calling post-unlock `finish`.
The abstract outcome remains definitely aborted because installation never
started.

Dropping `Transaction<Active>` invokes this path. `commit(self)` first transfers
the item vector and release guards out of the public active-transaction Drop
guard into a private commit guard. Before `Irrevocable`, that guard's fallback
is `Aborted`; at `Irrevocable`, it changes the fallback to `Indeterminate`
before the first install. The ownership transfer disarms the public Drop path,
so it can never abort-release an irrevocable transaction. A partially locked,
pre-irrevocable commit guard also invokes the abort path. Double unlock, double
finish, and reuse after finish are bugs caught by state ownership rather than
boolean flags.

If abort cleanup violates its infallible contract, the abstract outcome remains
definitely aborted, but the runtime is poisoned. A fallible commit call reports
`CommitFailure::Poisoned { outcome: DefiniteOutcome::Aborted(..), .. }`;
explicit `abort` or Drop records the same health failure because those paths
cannot return a `CommitFailure`.

### 10.5 Failure after installation starts

Installation and mandatory cleanup are designed to be infallible. An adapter
panic, foreign abort, or invariant violation after `Irrevocable` cannot be
translated into `Conflict` or `Aborted`. The implementation MUST preserve all
core-retained reachable state for diagnosis, quarantine the affected
worker/runtime, and
return `Indeterminate` when publication cannot be classified. If publication
is provably complete and only later cleanup fails, it returns
`CommitFailure::Poisoned` carrying `DefiniteOutcome::Committed`; callers MUST
NOT retry it.
Every still-held physical guard is released only with
`LockDisposition::Indeterminate`, which conservatively advances its generation
or preserves publication; it never restores a pre-lock version after partial
installation. An unreleasable guard and its frame remain quarantined rather
than being implicitly dropped.

If an accepted hook staged upper metadata and installation later becomes
poisoned or indeterminate, recovery of that staged metadata belongs to the upper
layer and MUST use the same indeterminate disposition. `sto-core` does not
attempt an external rollback.

No Rust panic or C++ exception may cross the C ABI.

## 11. Predicates, commutativity, and absence

### 11.1 Logical segmentation

The datatype specification determines concurrency. An adapter SHOULD allocate
separate logical resources for operations that commute and share resources for
operations that do not. Return values matter: an insert that returns a global
size observes more state, and therefore conflicts more broadly, than an insert
that returns only per-key success.

Adapters MUST document their abstract operations, return values, conflict
units, and covering versions before optimization work begins.

### 11.2 Commutative writes

An operation that does not expose prior state need not record a read. For
example, a counter can accumulate a delta and serialize only during install;
concurrent increments then need not abort simply because they both update the
counter.

The first core supports adapters that stage such semantic write intents. A
specialized multi-owner `CommutativeVersion` optimization is deferred until the
basic exclusive-lock protocol is complete.

### 11.3 Optimistic predicates

A predicate is an owned adapter token representing a necessary commit condition,
not an arbitrary application closure. During execution, the adapter evaluates
the predicate against a sound snapshot. During commit, `upgrade_predicate`:

1. atomically observes the relevant state and its covering versions;
2. rejects if the semantic condition no longer holds; and
3. records covering versions for ordinary validation.

Version validation after predicate evaluation is mandatory because a value can
change and then change back while still invalidating a multi-item execution.

Adapters SHOULD consider separate semantic generations—for example, record
data versus tree structure—before falling back to a coarse shared generation.
The paper found extra semantic versions somewhat cheaper than predicates in
many low-conflict cases, while predicates remained more general and could be
more valuable under contention.

### 11.4 Missing keys

An unsuccessful lookup is an observation, not the absence of one. The adapter
MUST record a witness changed by any insertion that could change the result.
The paper uses bucket, neighbor, or Masstree leaf versions. The Rust Masstree
adapter instead interns a stable tombstone record for a point miss and observes
that record's version.

### 11.5 Range predicates

A range scan must detect membership changes inside its logical bounds. The v1
Masstree adapter uses one coarse, versioned and exclusively lockable membership
resource per table:

- every scan reads it;
- every transaction whose committed final liveness for any record differs from
  its observed committed liveness writes it exactly once;
- validation accepts the resource if it is unchanged or locked by the same
  transaction; and
- scan output overlays all staged mutations from the scanning transaction.

This serializes all membership-changing transactions in a table and causes
false conflicts between unrelated ranges. It is a correctness baseline, not a
claim of MassTrans-equivalent precision.

## 12. Opacity profiles

### 12.1 Serializable profile

`IsolationMode::Serializable` performs commit-time validation and guarantees
strict serializability for committed transactions. During execution it may
observe a combination of individually sound snapshots that never existed
together.

Callers may select this mode only when transaction code remains safe under such
logical inconsistency: it cannot use inconsistent values to cause undefined
behavior, unbounded execution, unrecoverable external effects, or an uncontained
foreign failure. Safe Rust prevents many memory errors but not panics, infinite
loops, semantic corruption of nontransactional state, or unsafe/FFI misuse.

Mako's controlled database transaction path begins with this profile for Silo
compatibility.

### 12.2 Opaque profile

`IsolationMode::Opaque` adapts the paper's TL2-style protocol:

1. read the runtime's ordered version bound at transaction start;
2. compare every ordered observation with the bound;
3. on a newer, unordered, or otherwise unsafe observation, reload the bound and
   revalidate all prior reads and predicates; and
4. abort before returning an inconsistent result if revalidation fails.

**[STO]** Committed ordered versions in an opaque runtime MUST advance
monotonically with the runtime commit clock. Every ordered version installed by
a transaction derives from its `OccCommitId`, or from a mechanism with an
equivalent ordering proof. An adapter cannot publish an independently ordered
version while advertising it as comparable with the opacity bound.

**[RUST/OPAQUE]** A writing transaction locks its complete write set, upgrades
predicates, reserves `OccCommitId`, and then begins the final validation pass.
Thus a conflicting lower-ID writer has either installed before the certification
cut, still exposes a lock/mismatch that aborts validation, or has aborted without
publication; a later conflicting writer receives a higher ID. Clock
reservations from transactions that later abort are harmless gaps. This ordering
is the commit-side basis for comparing execution observations with the opacity
bound.

Execution-time predicate checks use `revalidate_predicate`; they re-evaluate the
condition and covered state without converting the item. Commit-time checks use
`upgrade_predicate` to produce ordinary covering observations. An adapter using
versions not ordered by the runtime clock marks them unordered and forces full
revalidation or a conservative abort.

The paper used a GV4 global clock for simplicity but explicitly allowed other,
more scalable clocks. One global counter is not part of the abstract STO
contract.

Opaque read-only transactions still retain their read and predicate
observations for execution-time revalidation. The paper permits them to skip a
final commit pass only when those checks already establish the required bound;
Rust v1 does not take that fast path initially.

Opacity is a runtime capability bit. Until implemented and tested end to end,
constructing an opaque runtime returns `Unsupported`.

### 12.3 Panics in transaction bodies

With unwind enabled, a panic in ordinary Rust transaction-body code triggers
the active transaction's Drop abort before unwinding continues. A panic MUST
NOT cross into C++. With `panic=abort`, no cleanup guarantee is possible; such a
build profile is unsuitable for claims of recoverable transaction-body panic.

Adapter commit callbacks are required to be panic-free regardless of profile.

## 13. Lifetimes and reclamation

### 13.1 General rule

Every object reachable from an observation, predicate, write intent, or lock
token MUST stay alive until the transaction finishes. A reference that was valid
during execution but freed before validation invalidates the entire protocol.

The C++ paper implementation makes a transaction one RCU read-side critical
section. Rust STO does not impose one universal reclamation system; each adapter
must register and own the required resource scope explicitly.

### 13.2 Rust-owned state

The core prefers owned snapshots, `Arc` handles, stable append-only registries,
and guards whose lifetime is tied to the transaction. Raw pointers are not
logical identities. If an adapter uses epoch or hazard reclamation, the guard
must be represented in transaction state and released on every outcome.

### 13.3 Foreign RCU

No C++ Masstree node, cursor, key pointer, or RCU token crosses the v1 ABI. Each
ordinary ABI call opens and closes its own native RCU region. Rust MUST NOT keep
that region open across application code, a transaction, I/O, blocking, or
`.await`.

If repeated calls need amortization, prefer one C ABI batch operation. A public
long-lived RCU pin is deferred.

### 13.4 Reclamation policy

Published Masstree directory entries and published or publication-unknown Rust
records are append-only in v1. Their `RecordId`s are never reused. A candidate
proved never to have entered the directory may release directory-reachable
record and key-byte quota, but its numeric ID remains consumed. An
implementation may drop separately allocated candidate backing or retain its
in-place arena slot; in the latter case the consumed-ID limit is also the hard
bound on failed-candidate slot memory. Logical deletion installs a tombstone;
it does not free the record or remove the directory key. These rules apply
during live runtime operation; a successful whole-runtime shutdown may free the
entire ownership unit after quiescence.

Physical reclamation requires all of:

- conditional directory removal tied to the expected `RecordId`;
- proof that no transaction can resolve or retain the record;
- an explicit native grace-period barrier;
- a real worker unregister/shutdown model; and
- clear allocator ownership.

Until those exist, memory growth is explicit technical debt that benchmarks and
operational limits MUST measure.

## 14. Masstree transactional adapter

### 14.1 Ownership split

Masstree is an append-only ordered directory:

```text
binary key  ->  nonzero RecordId
```

C++ owns tree nodes, traversal, splits, allocation, and native RCU. Rust owns
the record registry, record locks/versions, immutable state snapshots,
tombstones, transaction items, membership predicate, and values.

One `TableInner` owns the native directory handle, record registry, and
membership resource as one lifetime unit. The runtime retains that unit until
no operation can return an ID from its directory. The registry and membership
resource cannot be dropped independently of the native tree's safe reachability.

Masstree MUST use a dedicated integral `uint64_t` value type with value prefetch
disabled. The existing `concurrent_btree` pointer-value configuration MUST NOT
reinterpret small `RecordId` integers as pointers or prefetch them as addresses.

### 14.2 Record registry

`RecordId(0)` means absence at the raw directory layer and is never allocated.
IDs are monotonically allocated from a checked nonzero `u64` domain and never
reused in v1. The allocatable domain is bounded by the minimum of the configured
consumed-ID limit, the registry-addressable slot domain, and `u64::MAX`; scalar
ABI support for every nonzero `u64` does not imply the registry can allocate
every value.
Exhaustion is a terminal capacity error before native publication. Wrapping to
zero or aliasing any consumed slot is forbidden.

A registry slot follows:

```text
Reserved -> Ready -> Published | ProvenUnpublished | PublicationUnknown
```

The record is fully initialized before `Ready` is Release-published. Resolution
uses Acquire semantics and accepts a fully ready slot, which is necessary
because another worker may observe a winning directory entry before the
inserter returns to mark it `Published`. Once the native insertion call begins,
a `Ready` candidate is conservatively treated as possibly published.

Only `OK, inserted=false, winner!=candidate`, or another ABI outcome explicitly
guaranteed to mean that the candidate never entered the directory, permits
`ProvenUnpublished`, retained-quota release, and dropping any separately owned
backing. A success that chose the candidate becomes `Published`. Any ambiguous
native error becomes `PublicationUnknown`:
the ready record and its quota remain retained permanently, its ID is not
reused, and the table is poisoned or quarantined before further access.

`RecordId` construction and registry resolution remain private to
`sto-masstree`. Safe callers cannot resolve guessed IDs or access
proven-unpublished/publication-unknown slots outside quarantine diagnostics.

An implementation may use segmented append-only storage or another design that
keeps registry lookup stable while the registry grows. The default
`RegistryLayout::LazySegmented` publishes fixed-size `RegistrySegment`s through
segment-level `OnceLock`s. A published segment already contains eagerly
initialized, stable `RegistryEntry` slots; an atomic `UNALLOCATED -> RESERVED ->
READY` transition claims and publishes a slot. There is no per-slot `OnceLock`.
Each entry owns its `Record` in place, and both `Ready` and `Published`
resolution borrow the same address. This removes a second
candidate/published-pointer layer while preserving the race in which the native
directory exposes a winner before its inserter records `Published`.

`RegistryLayout::EagerContiguous { max_bytes }` is an explicit alternative for
a bounded table that expects to consume most of its configured ID space. Table
construction allocates and initializes the entire stable arena and all record
lock targets, making resolution a direct base-plus-index operation. Construction
fails with `Capacity` if the accounted slot and lock storage exceeds
`max_bytes`; it never silently falls back to segmented storage. The lazy layout
remains the public default. Copying the current prototype's unchecked wrapping
`fetch_add` allocator is forbidden.

Because a point miss can intern a new key, each runtime/table MUST enforce
configured retained-record and retained-key-byte quotas. Quota is reserved
atomically before candidate creation so concurrent misses cannot overcommit it.
Exhaustion returns `Capacity` before native publication. A proven-unpublished
loser may release retained-record and key-byte quota, although its numeric ID
and registry slot remain consumed. In the stable in-place arena it also retains
the initialized tombstone/version/lock slot until whole-table destruction;
`max_consumed_record_ids`, not retained-record quota, bounds that physical arena
memory. A publication-unknown candidate retains both quotas. A deployment may
configure very large limits, but unbounded miss-driven allocation is not an
acceptable implicit policy.

### 14.3 Record representation

Conceptually:

```rust
struct Record {
    version: AtomicVersion,
    state: CommittedRecordState,
}

enum RecordState {
    Tombstone,
    Live(Value),
}

struct CommittedRecordState {
    inline: AtomicU64,
    shared: ArcSwapOption<Vec<u8>>,
    tag: AtomicU8,
}
```

The implemented registry entry has a measured 48-byte stride. Values of up to
eight bytes are copied through the inline atomic payload; larger immutable
values use the protected shared pointer. Per-record physical lock targets are
stored separately in `Arc`-owned 16-record segments. A detached guard names the
exact inline `AtomicVersion`, while its core lock frame retains the owning slot
arena through release. Thus point reads pay neither an `Arc` clone for the
record nor a separately allocated lock object.

The exact binary key is owned once by the private append-only directory. A
successful directory lookup returns a `RecordId` capability into this table's
private registry; safe callers cannot forge IDs, insert through another handle,
or pair one registry with another directory. Repeating every key in `Record`
and comparing it on each hit would therefore duplicate the trusted directory
binding rather than strengthen the safe contract. Likewise, the per-record
lock identity is reconstructed without allocation from table identity plus
`RecordId` only for a changed record during preflight; it is not stored in the
read-hot slot.

`AtomicImmutableSnapshot` is descriptive, not a commitment to a specific crate.
Its implementation must have a sound reference-count and memory-order proof.
A lock-backed immutable snapshot is acceptable only when that synchronization
guard is itself the tracked record lock, is acquired by
`TransactionLock::try_acquire` in the global lock order, and remains in the
core-owned lock frame through installation.
`install` MUST NOT acquire a second mutex. Otherwise the adapter must use a
sound atomic immutable-snapshot publication primitive.

### 14.4 Point lookup and miss interning

Point access proceeds as follows:

1. Look up the key through the copied/scalar C ABI.
2. On a directory miss, atomically reserve quota and allocate a fully initialized
   tombstone candidate.
3. If the native strong-scan guarantee is unavailable, lock the table's
   structural version; then call atomic `get_or_insert` and complete the
   advance/unlock disposition from Section 14.6.
4. Resolve the winning `RecordId` through the table-private stable registry.
   The exact key-to-ID association is the trusted result of the exclusively
   owned safe directory boundary; no duplicate Rust key comparison is needed.
   Release retained quota only for a candidate whose nonpublication was
   positively proved by the ABI outcome.
5. Read a sound immutable state snapshot with a stable version-before/version-
   after check.
6. Record the version and logical present/absent result in the transaction item.

Interning a tombstone changes physical tree structure but not logical table
membership, so it does not advance the membership version. A later activation
of that tombstone changes both record state and table membership transactionally.

### 14.5 Writes and membership changes

`put`, `insert`, and `remove` change only transaction-local staged state during
execution. Preflight allocates the complete replacement snapshot.

At commit:

- every changed record is locked and validated;
- a transaction whose committed final liveness for any record differs from its
  observed committed liveness also writes the table's membership item once;
- the membership `LockIdentity` sorts before the table's record locks, so it is
  acquired first;
- record snapshots and locked generations may install in any callback order
  while every affected resource, including membership, remains locked;
- record resources publish and unlock before membership; and
- membership's new unlocked generation publishes last, advancing once for the
  transaction's net table change.

The membership object is not a relaxed statistic. It is a real transaction item
with exclusive lock and read validation. This prevents a scan from validating
through a partially installed multi-record membership transaction.

All such net membership-changing transactions on one table therefore conflict
with one another in v1, even when their keys and scan ranges are disjoint.

### 14.6 Scans

A scan:

1. observes the table membership item;
2. obtains copied key/`RecordId` chunks from Masstree;
3. privately resolves every returned ID through the exclusively owned
   directory-to-registry capability;
4. soundly snapshots every returned record, including tombstones, treating a
   locked record as a conflict;
5. adds a record read observation for each returned live value;
6. filters tombstones, whose later tombstone → live transition is covered by
   the membership item;
7. applies lower/upper and inclusive/exclusive bounds exactly; and
8. merges transaction-local writes and deletes into key order.

The C bridge serializes each possible tree insertion against point lookups and
scans for that tree; read-only calls may proceed together. A single copied C
chunk therefore has stable native structure, while a multi-chunk range remains
structurally weakly consistent because mutations may occur between calls.
Transactional consistency comes from the adapter's whole-scan structural gate,
record observations, and membership validation. In opaque mode, the adapter
additionally performs execution-time validation before exposing a chunk whose
observations might be inconsistent.

For each call, “weakly consistent” still requires a gap-free key-ordered prefix
up to the reported stop/resume boundary: a concurrent append-only insert or
split cannot corrupt, omit, or duplicate an entry continuously present in that
traversed prefix. Exclusive resumption then partitions the remaining range. The
Masstree bridge MUST prove that property or serialize the conflicting native
operations. V1 takes the conservative serialization route twice: the ABI uses
per-tree shared/exclusive structural access for each call, and the
transactional table holds a nonblocking structural read guard across its whole
multi-chunk scan while tombstone interning holds the write guard across every
native publication outcome. This prevents a physical mutation between chunks
without turning physical tombstone creation into an abstract transactional
write. A later versioned protocol may replace the coarse gate only after it
proves the same prefix and resumption properties. Logical membership validation
alone cannot hide a structural traversal gap.

The paper's `TMasstree` used transaction items for both stored values and leaf
nodes and modified Masstree to expose the prior versions of leaves split by an
insertion. That let the adapter repair an earlier range witness after the same
transaction's eager insert. Rust v1 deliberately replaces that precision with
the coarse membership resource and staged scan overlay.

## 15. Masstree C ABI

### 15.1 Boundary principles

The existing `mtx_*` prototype on `worktree-masstree-rocks` is evidence and a
starting point, not an ABI to merge unchanged. The hardened ABI is pure C11,
with `extern "C" noexcept` declarations in C++ and `extern "C"` declarations in
Rust.

It exposes opaque:

```c
typedef struct mt_runtime mt_runtime;
typedef struct mt_thread mt_thread;
typedef struct mt_tree mt_tree;
typedef uint64_t mt_record_id; /* zero reserved */
```

The safe ownership shape is:

```text
Runtime: shared native-runtime ownership
Worker:  !Send + !Sync, bound to one OS thread and Runtime
Tree:    shareable facade; every operation also requires &Worker
```

V1 MAY support only one process-wide native Masstree runtime if the inherited
RCU/core-ID implementation cannot prove multiple independent instances. The
runtime handle remains explicit, and a second incompatible acquisition is
rejected rather than silently sharing global state.

The initial operation families are:

- ABI, feature, layout, build-fingerprint, maximum-key, and limit queries;
- runtime acquisition and health;
- same-thread worker attachment and quiescence;
- tree creation and capability-gated runtime shutdown, both with a matching
  worker;
- scalar point lookup, a scoped repeated-read form, and fixed-width strided
  batch lookup (`mt_get_strided`);
- atomic get-or-insert with explicit publication disposition, `inserted`, and
  `winner` outputs; and
- copied forward/reverse scan chunks.

There is no update, value mutation, cursor, callback, borrowed output, physical
delete, or raw RCU pin in v1.

### 15.2 Keys and bounds

Keys are binary. Empty keys are valid. A null pointer is valid only with zero
length. Every entry point validates lengths before constructing a C++ `Str` or
cursor. Inputs above the runtime-advertised maximum, the configured 1024-byte
Masstree limit, or the signed internal length domain are rejected.

Scan bounds explicitly distinguish:

- absent/unbounded;
- present empty key;
- inclusive; and
- exclusive.

Resumption uses an exclusive bound containing the last copied key. It never
constructs a successor by appending a byte.

### 15.3 Scan outputs

Scans fill caller-provided entry and key-arena buffers. The result explicitly
states:

- entries written;
- arena bytes used;
- whether the stop reason was end-of-range, entry capacity, or arena capacity;
- bytes required for the next key when no progress fits; and
- the authoritative resume key/semantics.

After the complete set of required output pointers has been validated, every
output is initialized before any other input validation. If a required output
pointer is null, the call returns `INVALID` without dereferencing it. Resuming
produces no gaps or duplicates under a quiescent tree; concurrent logical
consistency is provided by STO validation.

### 15.4 Threading and RCU

Every `mt_thread` is bound to its creating OS thread and runtime. Safe Rust
represents it as `!Send + !Sync`. Workers are fixed and long-lived because the
current native core-ID and threadinfo registrations are capped and not truly
recycled. The ABI MUST report exhaustion rather than reaching an assertion or
`abort()`.

Every tree operation validates the worker's current OS thread, the worker's
runtime, and the tree's runtime before traversal.

Ordinary calls enter and leave native RCU internally. No caller-supplied,
foreign, or arbitrary callback executes while RCU is held. A bridge-internal,
allocation-free, nonblocking collector may copy scan results into caller
buffers. No native pointer escapes.

### 15.5 Tree and runtime shutdown

Native Masstree destruction is thread-affine and deferred through RCU. Therefore
`Tree::drop` MUST NOT call native teardown from an arbitrary thread.

Native teardown is a negotiated `GRACEFUL_SHUTDOWN` feature. Without that
feature, native runtime and tree allocations live for the process lifetime;
facade `Drop` removes safe reachability but never calls native destruction, and
an explicit shutdown request returns `UNSUPPORTED`.

With `GRACEFUL_SHUTDOWN`, shutdown requires a matching attached shutdown worker.
It atomically marks the runtime closing and rejects new work, then requires no
live transactions, tree facades, guards, operations, or other workers. If any
remain, it returns `BUSY` without partial destruction. Otherwise it destroys
runtime-owned trees on that worker and drains the required native grace periods.
Worker-facade destruction never implies that its native core ID can be recycled.
Dropping a runtime that has not completed explicit shutdown never guesses at
thread-affine destruction; it retains the native allocations and reports the
leak through diagnostics.

### 15.6 Errors and unwinding

Statuses distinguish invalid input, key too large, buffer too small, not
attached, wrong thread, wrong runtime, thread limit, out of memory, busy, active
guards, ABI mismatch, contained C++ exception, and internal failure.

The get-or-insert result additionally distinguishes candidate inserted,
different winner with candidate proven unpublished, failure proven before
publication, and publication unknown. If the bridge cannot prove that an error
preceded publication, it MUST return publication unknown; Rust then retains the
candidate and quarantines the table. A generic error status is never evidence
that a candidate is safe to free.

C++ catches `std::bad_alloc` and all ordinary exceptions before returning.
Assertions, `abort`, unchecked allocation, and memory corruption are not
exceptions; every path reachable from malformed ABI input must be audited to
avoid them. Rust panics and C++ exceptions never cross this boundary.

### 15.7 Build identity

The C++ shim is built by CMake with exactly the same generated Masstree
configuration, target definitions, compiler, standard library, and native
archives as the tree implementation. Cargo MUST NOT independently compile the
template-heavy Masstree headers with a divergent `CONFIG_H`.

Runtime negotiation verifies ABI version, feature bits, endianness, pointer
width, maximum key length, every public POD size/alignment, exact exported
symbols, and a native build fingerprint.

## 16. Correctness argument

### 16.1 Assumptions

The strict-serializability argument assumes:

1. every adapter satisfies the obligations in Section 8;
2. every commit-phase lock retained across callbacks either has one equality-
   correct `LockIdentity` acquired in the unique planned order or satisfies the
   finite hidden-lock fallback; every logical write resource remains exclusively
   protected through installation;
3. validation sees either the observed version, that version locked by self, or
   failure;
4. installation cannot fail or panic;
5. published snapshots and versions obey the memory ordering in Section 9;
6. item identities are stable and equality-correct;
7. nontransactional operations participate in the same protocol; and
8. native scans either preserve continuously present entries without omission
   or duplication across concurrent splits, or validate a structural version
   locked before and advanced after every possible directory mutation.

### 16.2 Writing transactions

All write resources are locked before core clock reservation and final
validation. Define the certification cut immediately before the first final
read check. Since covering versions do not wrap or revert, any dependency that
changed before the cut remains mismatched or locked when checked. Success of the
whole sequential pass therefore proves retrospectively that every read and
predicate described shared state at the cut, except for resources locked by the
same transaction at their observed pre-lock version.

Serialize an ultimately committed transaction at that certification cut. Its
ordered core ID was reserved immediately before the cut. Because a lower-ID
conflicting writer locked first, it must have installed, remain locked/detected,
or have aborted without publication; a writer ordered later receives a higher
ID. The transaction later crosses the failure-disposition boundary and installs
all writes while retaining every lock. A concurrent transaction that could
observe a partial installation sees a lock or changed version and cannot commit
that observation. Hook acceptance determines whether the certified transaction
commits; it does not move the serialization point.

### 16.3 Read-only transactions

A read-only transaction defines its certification cut immediately before its
final pass. If the whole pass succeeds, monotonic covering versions prove every
observation was unchanged and unlocked at that cut, so the transaction can be
serialized there. The opacity profile strengthens this by maintaining a
consistent prefix during execution.

### 16.4 Aborts and eager scaffolding

Deferred writes have no shared abstract effect before install. Abort releases
locks and discards intents. The Masstree adapter may have interned tombstone
records, but those are logically absent before and after abort. Therefore an
aborted transaction has no abstract effect.

### 16.5 Phantom safety in v1

A scan observes the table membership version. Every transaction whose committed
final liveness differs from its observed committed liveness writes and advances
that version while holding its exclusive lock. A scan concurrent with such a
net insertion, deletion, or resurrection either validates before the change,
sees the lock/change and aborts, or validates after it. Per-record versions
cover value updates. This is conservative but sufficient when every logical
table mutation goes through the Rust adapter.

Tombstone interning changes only physical structure and therefore does not
advance logical membership. Scan completeness additionally relies on Assumption
8. In v1, per-call ABI synchronization and the table's whole-scan structural
read guard exclude a concurrent split; an interning writer holds the matching
write guard across the native outcome classification. Thus a physical-only
insert cannot silently hide or duplicate a stable live record in a committed
scan.

### 16.6 What this argument does not prove

It does not prove fairness, bounded retries, crash durability, distributed
atomicity, MVCC visibility, or correct behavior if a C++ or nontransactional
path bypasses the Rust record/membership protocol.

## 17. Mako and upper-layer integration

### 17.1 Reference backend

The upper branch at `1daec550f` remains the behavioral oracle:

```text
safe Rust mako-local -> mako_local_* C ABI -> C++ STO/MassTrans/Masstree
```

The native Rust backend bypasses that STO C ABI and calls `sto-core` directly.
Both backends implement a backend-neutral upper transaction interface during
rollout.

### 17.2 Commit hook compatibility

**[COMPAT]** For a non-read-only transaction, the existing upper contract places
Mako timestamp reservation after the full write set is locked and before local
read validation. It places the hook after validation but before the first
install. Rust STO preserves that sequence and the hook's exactly-once behavior.

The implemented seam keeps typed upper metadata in the hook object; `sto-core`
never erases or interprets it:

```rust
pub enum CommitHookError {
    Rejected,
    Capacity(CapacityError),
}

pub trait CommitHook {
    fn reserve_upper_metadata(&mut self) -> Result<(), CommitHookError>;
    fn pre_install(&mut self) -> Result<(), CommitHookError>;
}

impl<'worker> Transaction<'worker, Active> {
    pub fn commit_with_hook<H: CommitHook>(
        self,
        hook: &mut H,
    ) -> Result<CommitOutcome, CommitFailure>;
}
```

A later conflict may leave a harmless timestamp gap. Hook rejection or a
contained panic is a definite abort only because the upper hook contract
guarantees that its preallocated staging has no externally visible rejection
path. Hooks for disjoint workers may run concurrently. Blocking I/O, await, and
reentrant STO use are forbidden; a configured watchdog observes hook latency.
The watchdog is diagnostic and does not forcibly interrupt a hook. Exceeding
its budget is an abort only if the upper contract can still reject with no
externally visible effect; otherwise it is reported to integration health
policy without changing the local commit protocol.

The hook receives typed upper metadata. It MUST NOT cause `sto-core` to interpret
Mako timestamps as OCC versions or inspect erased adapter payloads. If a
post-hook install is poisoned or indeterminate, upper-layer recovery owns the
staged metadata and consumes that disposition; the core cannot roll it back.

### 17.3 Distributed prepare is not a core v1 feature

A long-lived prepared transaction would retain locks while a network protocol
waits, conflicting with the core's bounded, nonblocking commit discipline.
Therefore the internal `Validated` state is not exposed as a distributed
`PreparedTransaction` in v1.

Failure-tolerant prepare/commit/abort, stale-term rejection, idempotence, timeout
handling, and participant recovery require a separate `sto-mako` protocol
design. If later exposed, its liveness and lock-retention tradeoff must be
specified rather than implied by typestate.

### 17.4 Policy separation

The following remain distinct upper-layer concerns:

- `MakoTimestamp` allocation and visibility;
- replication terms and participant IDs;
- WAL and RocksDB write descriptions;
- durability/cache sequence numbers;
- MVCC versions and watermarks; and
- RPC batching and retry.

The core can emit typed, adapter-produced write descriptors to an upper layer.
It MUST NOT hard-cast erased item state to MassTrans key/value types.

## 18. Verification and evaluation

### 18.1 Pure Rust core

Required tests include:

- an out-of-crate adapter contract fixture that implements
  `TransactionalResource` for two resource classes and uses a custom
  `TransactionLock`;
- compile checks that the normative public signatures remain externally
  implementable while the erased item and lock traits remain inaccessible;
- heterogeneous dispatch through two `ItemBox<A>` types in one transaction,
  including reuse of the same typed item on repeated lookup;
- rejection of wrong-runtime resources, registration `TypeId` mismatches, and
  stale or mismatched `LockUse` tokens without an unchecked downcast;
- a caught operation error or unwind dooms the transaction and cannot commit
  partially changed item state;
- `ItemInitError` and failure at every other fallible `with_item` step leave the
  transaction doomed;
- compile-fail checks that active transactions, entries, phase contexts, and
  worker-affine guards do not accidentally become `Send` or `Sync`;
- version classification, checked overflow, and own-lock validation;
- item identity, hash collision handling, deduplication, and total order;
- aliased physical-lock canonicalization, cross-transaction order, and
  exact-once release, including two logical resource classes deliberately
  sharing one `LockIdentity`;
- adversarial alias schedules such as `a,c -> L` and `b,d -> M` across opposite
  logical item sets;
- bounded hidden-lock failure releases every planned and hidden acquisition
  exactly once;
- every same-item read/write mutation sequence;
- lock-N failure releases locks `0..N-1` exactly once in reverse;
- phase-by-phase panic injection proves commit-guard ownership transfer and
  that no Drop path uses `LockDisposition::Aborted` after `Irrevocable`;
- every pre-irrevocable `AdapterFault` performs abort cleanup and returns
  `Poisoned` with a definite aborted outcome;
- install or release panic returns `Indeterminate`, while finish panic after
  complete publication returns `Poisoned` with a definite committed outcome;
- callback traces for `preflight` failure at item N distinguish items with and
  without `Prepared` state and finish each inserted item exactly once;
- a failing allocator is enabled after preflight through install, release, and
  finish to prove those phases allocate nothing;
- predicate-upgrade and validation failure install nothing;
- commit/abort/Drop cleanup exactly once;
- cross-object atomicity using two independently implemented adapters;
- panic before install versus invariant failure after install begins;
- Loom schedules for version, lock, publication, and transaction models;
- Miri for unsafe type erasure or registry code; and
- property tests for operation composition and reference histories.

### 18.2 Legacy semantic oracle

The stronger existing programs, rather than only current CMake smoke tests,
should seed the compatibility suite:

- [`unit-tgeneric.cc`](../../src/mako/sto/unit-tgeneric.cc): generic reads,
  writes, opacity, non-opacity, and read-your-writes;
- [`unit-tcounter.cc`](../../src/mako/sto/unit-tcounter.cc): commutative updates
  and optimistic predicates;
- [`unit-tarray.cc`](../../src/mako/sto/unit-tarray.cc) and
  [`unit-tvector.cc`](../../src/mako/sto/unit-tvector.cc): indexed conflicts,
  iterators, and predicate composition;
- [`rbtree.cc`](../../src/mako/sto/rbtree.cc): insert/delete/reinsert and abort
  cleanup;
- [`single.cc`](../../src/mako/sto/single.cc): MassTrans point/range schedules;
  and
- [`concurrent.cc`](../../src/mako/sto/concurrent.cc): randomized concurrent
  workloads.

The native crate MUST add Rust version-word parity tests against the committed
C++ `TransactionTid` behavior. Those tests are a bit-level migration oracle,
not proof of transaction correctness.

### 18.3 Masstree ABI

Before safe integration, test:

- C11 and C++ header conformance;
- ABI layout, symbol allowlist, feature, and fingerprint agreement;
- empty, binary, embedded-NUL, 1024-byte, and rejected 1025-byte keys;
- scalar ABI round-trips for all nonzero 64-bit `RecordId` bit patterns without
  requiring the registry to allocate or resolve them;
- exactly one winner under concurrent get-or-insert;
- success, proven-unpublished, and publication-unknown insertion outcomes;
- atomic retained-record/key-byte quota reservation under concurrent misses;
- Release publication and Acquire registry resolution;
- forward/reverse inclusive/exclusive bounds;
- concurrent split scans never omit or duplicate continuously present entries,
  or lock-before-mutation/advance-after and validate the negotiated structural
  version, including scans in both mutation windows;
- zero/tiny buffers, long-key arena exhaustion, and exact resumability;
- wrong thread/runtime and exhausted worker space;
- injected `bad_alloc` and ordinary C++ exceptions;
- shutdown with active handles/guards when `GRACEFUL_SHUTDOWN` is negotiated,
  plus process-lifetime behavior when it is not; and
- ASan, UBSan, and TSan stress.

Compile-fail tests prove worker and active transaction types are `!Send` and
`!Sync`.

### 18.4 Differential histories

Run the same operation corpus through:

1. a sequential `BTreeMap` reference model;
2. direct C++ MassTrans;
3. raw `mako_local_*`;
4. the existing safe upper Rust wrapper; and
5. native Rust STO over the Masstree C ABI.

Sequential traces compare exact observations and final states. Controlled
concurrent schedules cover read/write, write/write, point-miss, phantom,
insert/delete/resurrection, hook rejection, and abort cleanup. An independent
history checker verifies strict serializability; an additional checker is used
when opacity is enabled.

### 18.5 Performance ladder

Measure each boundary separately:

```text
raw C++ Masstree -> C ABI -> safe Rust Masstree
direct C++ MassTrans -> native Rust STO/Masstree
```

Track throughput, p50/p99 latency, abort rate, allocations, item bytes,
lock-hold time, scan chunk cost, and false conflicts from the coarse membership
item. Performance changes never weaken a stated correctness gate.

### 18.6 Implementation conformance record

The experimental v1 implementation corresponding to this contract is split as
follows:

| Surface | Repository implementation | V1 state |
| --- | --- | --- |
| Typed STO protocol, private erasure, lock planning, failure dispositions | [`crates/sto-core`](../../crates/sto-core) | Implemented for serializable non-opaque transactions. |
| Raw stable declarations | [`crates/mtree-sys`](../../crates/mtree-sys) | Implemented for ABI version 1. |
| Native C boundary | [`mtree_abi.h`](../../src/mako/storage/mtree_abi.h) and [`mtree_abi.cc`](../../src/mako/storage/mtree_abi.cc) | Implemented for scalar/scoped/strided point operations and copied bounded scans. |
| Safe runtime, worker, tree, point, and scan facade | [`crates/masstree`](../../crates/masstree) | Implemented; native cursors, pointers, and RCU guards remain private. |
| Transactional records, tombstones, quotas, membership predicate, and scan overlay | [`crates/sto-masstree`](../../crates/sto-masstree) | Implemented for the conservative table-membership profile. |
| Upper metadata reservation and pre-install coordination | [`hook.rs`](../../crates/sto-core/src/hook.rs) | Implemented as an optional caller-owned `CommitHook`. |
| Opacity, graceful native shutdown, upper backend cutover, and performance acceptance | Sections 12, 15.5, 17, and 19.2 | Deferred; callers receive explicit unsupported/capability outcomes rather than silent downgrade. |

The branch-level validation record for this implementation includes the full
workspace suite in debug and release modes on Rust 1.95, strict Clippy and
rustdoc builds, C11 header compilation, the exact 41-symbol native allowlist,
required feature mask `0x0f7f`, export-manifest FNV-1a fingerprint
`0xdb5bed9b8f1490e3`, the raw ABI suite, native safe-wrapper and
transactional-adapter integration, and ASan, UBSan, and unsuppressed TSan
stress. The production cutover record
still needs explicit native fault injection for allocation failure, ordinary
C++ exceptions, and publication-unknown insertion, plus the upper-backend
differential histories, performance budgets, and the deferred capabilities in
the table above.

This table records implementation presence, not authorization to make the Rust
backend the production default. That decision still requires the cutover gates
in Section 19.2.

## 19. Implementation sequence and rollout

### 19.1 Non-normative implementation sequence

1. **Contract and workspace.** Keep the Rust version-word parity tests, retire
   generated-C++ execution as the production direction, establish the crate
   workspace, and land this guideline plus executable contract tests.
2. **Masstree point ABI.** Transplant and harden the existing `mtx_*` prototype
   with integral/no-prefetch values, explicit handles, checked keys, winner
   semantics, build identity, and the safe wrapper.
3. **Native `sto-core`.** Implement runtime/worker contexts, items, versions,
   preflight, lock/validate/install/abort, and in-memory adapters.
4. **Point vertical slice.** Add the stable registry, tombstones, and
   transactional Masstree point operations.
5. **Scans and predicates.** Add copied bounded scans, membership-item phantom
   protection, and staged-write overlay required by v1.
6. **Upper integration and nonopaque cutover.** Implement the backend-neutral
   facade and differential corpus while retaining C++ as the reference backend;
   shadow or feature-flag production before changing the default.
7. **Optional opacity profile.** Add execution-time revalidation as a separate
   negotiated capability milestone after the nonopaque cutover gates pass.
8. **Optimize.** Profile item representation, ordering, batching, and predicate
   precision without weakening the selected profile's gates.

### 19.2 Cutover gates

Native Rust becomes the default only when:

- every semantic and ABI correctness gate passes;
- sanitizer and fixed-worker stress are clean;
- differential histories agree or have documented intentional differences;
- failure dispositions and worker quarantine are tested;
- supported isolation capabilities are accurately negotiated;
- memory growth and worker limits have operational bounds; and
- performance and false-conflict budgets are explicitly accepted.

The C++ reference backend remains available for at least one compatibility
release after cutover.

## 20. Decision record and change policy

### 20.1 Accepted v1 decisions

| ID | Source | Decision | Rationale |
| --- | --- | --- | --- |
| D1 | RUST | STO executes as native Rust; only Masstree is behind C. | This is the requested lower boundary and avoids a second STO ABI. |
| D2 | STO/RUST | Items represent logical resources with stable owned identity. | Preserves type-aware conflict tracking without address identity. |
| D3 | RUST | Generic adapters use deferred writes in v1. | Makes abort and memory safety tractable. |
| D4 | RUST | Masstree stores immutable nonzero `RecordId`s. | No Rust or C++ record pointer crosses the ABI. |
| D5 | RUST | Point misses intern tombstones; logical deletes retain them. | Gives stable absence witnesses without native leaf tokens. |
| D6 | RUST | One lockable membership item per table protects scan phantoms. | Correct, conservative baseline with per-call RCU. |
| D7 | RUST | Published/publication-unknown records and all consumed IDs are not reclaimed or reused in v1; proven-unpublished candidates may be dropped. | Avoids premature cross-language reclamation without leaking proved losers. |
| D8 | RUST | Explicit thread-affine worker contexts replace core TLS. | Makes ownership and finite worker resources visible. |
| D9 | RUST | V1 deduplicates and sorts canonical physical `LockIdentity` requests. | Prevents deadlock even when logical resources share coarse locks. |
| D10 | STO/COMPAT | Serializable nonopaque OCC lands first; opacity is explicit. | Matches controlled Silo usage without claiming silent opacity. |
| D11 | RUST | Installation and mandatory cleanup are infallible. | Prevents reporting a partially installed transaction as aborted. |
| D12 | RUST | Native RCU is scoped inside ABI calls. | No foreign pointer or process-wide epoch stall leaks into Rust code. |
| D13 | COMPAT | The pre-install hook runs once after lock+validation and before install. | Preserves the upper branch's commit seam. |
| D14 | RUST | Distributed prepared state is outside core v1. | Network waiting while locks are held needs a separate liveness design. |
| D15 | RUST | Native version encoding is private; the C++ layout is a parity oracle. | Rust records do not exchange atomic objects with C++. |
| D16 | RUST/COMPAT | Native teardown requires negotiated `GRACEFUL_SHUTDOWN`; otherwise native allocations are process-lifetime. | Makes RCU/thread-affine destruction an explicit capability rather than a `Drop` guess. |
| D17 | RUST | External adapters implement safe typed `TransactionalResource`, `OpacityToken`, and `TransactionLock` traits; heterogeneous item/guard erasure is sealed, private, and checked. | Preserves the paper's virtual extension seam without exposing untagged storage or making memory safety depend on adapter semantics. |

### 20.2 Deferred decisions and review triggers

| Topic | V1 position | Revisit when |
| --- | --- | --- |
| Compact/arena items | Correctness-first erased storage | Profiling attributes material cost to allocation or dispatch. |
| Unsorted write locking | Sorted total order | Benchmarks show sorting material and an unsorted proof/test suite exists. |
| Precise range witnesses | Table membership item | False conflicts exceed an accepted workload budget. |
| Long RCU pin or batching | Per-call RCU | ABI overhead is material and a nonblocking scope can be enforced. |
| Commutative multi-owner locks | Exclusive locks plus semantic intents | Counter/queue workloads justify the additional protocol. |
| Physical record GC | No reclamation | Growth is material and a grace-period design is proven. |
| Distributed `PreparedTransaction` | Not exposed | `sto-mako` specifies IDs, terms, idempotence, recovery, and liveness. |
| Native version bit allocation | Opaque `AtomicVersion` contract; legacy layout permitted but not exposed | Before the core implementation is performance-frozen. |

### 20.3 Changing this guideline

A change to a normative invariant requires:

1. a rationale identifying whether it is STO inheritance, compatibility,
   redesign, or deferred scope;
2. deterministic tests for the affected state transition;
3. an updated correctness argument;
4. differential results when behavior changes; and
5. benchmark evidence when the change trades precision or safety surface for
   performance.

Source line numbers are intentionally not normative. The symbols and semantic
tests referenced above are the migration map and will move as the Rust crates
replace C++ code.

## References

1. Nathaniel Herman, Jeevana Priya Inala, Yihe Huang, Lillian L. Tsai, Eddie
   Kohler, Barbara Liskov, and Liuba Shrira. “Type-Aware Transactions for Faster
   Concurrent Code.” EuroSys 2016, Article 31, 16 pages.
   [DOI][sto-doi], [author-hosted PDF][sto-paper], and
   [maintained STO repository][sto-source].
2. Stephen Tu, Wenting Zheng, Eddie Kohler, Barbara Liskov, and Samuel Madden.
   “Speedy Transactions in Multicore In-Memory Databases.” SOSP 2013.
   [Author-hosted PDF][silo-paper].
3. Yandong Mao, Eddie Kohler, and Robert Morris. “Cache Craftiness for Fast
   Multicore Key-Value Storage.” EuroSys 2012. [Author-hosted PDF][masstree-paper].
4. Rust standard library. [`std::sync::atomic` memory model][rust-atomics].
5. The Rustonomicon. [`Send` and `Sync`][rust-send-sync] and [FFI][rust-ffi].

[sto-paper]: https://read.seas.harvard.edu/~kohler/pubs/herman16type-aware.pdf
[sto-doi]: https://doi.org/10.1145/2901318.2901348
[sto-source]: https://github.com/readablesystems/sto
[silo-paper]: https://pdos.csail.mit.edu/papers/silo:sosp13.pdf
[masstree-paper]: https://pdos.csail.mit.edu/papers/masstree:eurosys12.pdf
[rust-atomics]: https://doc.rust-lang.org/std/sync/atomic/
[rust-send-sync]: https://doc.rust-lang.org/nomicon/send-and-sync.html
[rust-ffi]: https://doc.rust-lang.org/nomicon/ffi.html
