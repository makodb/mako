# Rust STO: Type-Aware Transactions for Safe, Extensible Data Structures

> **Status:** Implemented experimental non-opaque v1; pre-cutover
>
> **Implementation status:** `sto-core`, the raw and safe Masstree boundary,
> transactional Masstree point operations, copied scans, physical-directory
> generation validation, the homogeneous direct-commit lane, private direct
> directory tokens, bounded atomic values, trusted scan-generation validation,
> bounded registries, the terminal-read typestate, the optional fixed-`u64`
> Masstree specialization, pure Rust reference map/vector/queue adapters, and
> the upper commit-hook seam exist on this branch.
> The closed TPC-C bridge also implements fused full Payment, exact-home
> NewOrder, local Delivery, and the local StockLevel scan-and-join tail.
> A reproducible zoo-2 point-workload comparison is complete; the optional
> opacity profile, graceful native shutdown, production upper-layer
> facade/cutover, and production-wide performance acceptance remain deferred.
>
> **Audience:** STO core, transactional-datatype, Masstree ABI, and Mako integration developers
>
> **Baseline:** Mako `mako-dev` at `abfb6ea96739`; compatibility oracle
> `worktree-masstree-rocks` at `1daec550f`
>
> **Last updated:** 2026-09-04

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
Rust. The versioned public Masstree ABI exposes no C++ cursor, node pointer,
value pointer, callback, or transaction object. A hidden static-link lane lets
the safe Rust facade omit checks it has already discharged; it is not part of
the public ABI and exposes no additional safe application surface.

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
- a restricted terminal homogeneous read-batch typestate for adapters that can
  prove final certification and drop-only cleanup;
- an optional homogeneous direct-commit plan with explicit lock-injectivity and
  exact-token write-certification capabilities;
- private direct-directory tokens and opt-in bounded atomic values for the
  Masstree adapter;
- an opt-in trusted scan-generation profile for closed direct tables; and
- an optional, nonblocking pre-install hook with an upper-layer watchdog budget.

The `sto-masstree/fixed-u64` feature additionally provides an optional,
specialized all-present point-workload profile. It is not a replacement for the
general binary-value table and does not add transactional liveness changes or
range operations; its exact restrictions are normative in
[Section 14.7](#147-optional-fixed-u64-specialization).

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
index, counter value, map record, bucket generation, or table directory
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
                        public mt_* / hidden mako_mtree_*
                                      |
                       C++ Masstree structural kernel

current C++ TPC-C integration
             |
   rust_sto_tpcc_wrapper
             |
       sto-tpcc-ffi
             |
          sto-core
             |
       sto-masstree
             |
          masstree
             |
          mtree-sys
             |
   hidden/public native ABI
```

### 5.1 Crates

The intended workspace layout is:

```text
crates/
  sto-core/       Native transaction runtime and generic adapter protocol
  mtree-sys/      Raw generated or mechanically checked C declarations
  masstree/       Safe runtime, worker, tree, point, and scan wrapper
  sto-masstree/   Rust records and the transactional Masstree adapter
  sto-test-datatypes/
                  Pure Rust reference map, vector, and queue adapters
  sto-tpcc-ffi/   Closed C bridge used by the current C++ TPC-C integration
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
`sto-test-datatypes` supplies deliberately simple external-crate examples: a
fixed-bucket ordered map and whole-snapshot vector and queue. They are useful
for adapter-contract, composition, and conflict tests, but their cloning costs
and conservative conflict domains are not a production collection design.
`TransactionalResource`, `OpacityToken`, `TransactionLock`, and
`DirectBorrowedLockTarget` are nevertheless safe traits. A bad implementation
can violate isolation or progress, but `sto-core` memory safety MUST NOT depend
on its semantic claims. Any adapter that touches `UnsafeCell`, raw pointers, or
FFI owns the corresponding unsafe proof and must check the required lock or
guard itself.

The optional compact direct-commit path is a narrower unsafe extension seam.
`DirectTokenLock` is an unsafe public trait, and injective borrowed-token
capabilities use explicit unsafe constructors. Their proof binds each stable
token to exactly one retained physical lock and proves injectivity across every
eligible resource binding. Safe application code does not implement or invoke
that seam. The core still owns the plan driver, callback ordering, failure
containment, guard release, and item teardown.

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
pub struct TerminalReadOpen;
pub struct TerminalReadReady;
pub struct Transaction<'worker, State = Active> { /* private */ }
pub struct TerminalReadTransaction<
    'worker,
    State = TerminalReadOpen,
> { /* private */ }

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

    pub fn begin_terminal_read_batch(
        &mut self,
    ) -> Result<
        TerminalReadTransaction<'_, TerminalReadOpen>,
        BeginError,
    > {
        unimplemented!("signature-only design target")
    }

    pub fn begin_terminal_read_batch_with(
        &mut self,
        isolation: IsolationMode,
    ) -> Result<
        TerminalReadTransaction<'_, TerminalReadOpen>,
        BeginError,
    > {
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

impl<'worker> TerminalReadTransaction<'worker, TerminalReadOpen> {
    pub fn with_terminal_read_batch<A>(
        self,
        resource: &RegisteredResource<A>,
        keys: &[A::Key],
        operation: impl for<'entry> FnMut(
            usize,
            &mut TerminalReadEntry<'entry, A>,
        ) -> Result<(), AccessError>,
    ) -> Result<
        TerminalReadTransaction<'worker, TerminalReadReady>,
        AccessError,
    >
    where
        A: TransactionalResource,
    {
        unimplemented!("signature-only design target")
    }

    pub fn abort_with_access_error(
        self,
        error: AccessError,
    ) -> AccessError {
        unimplemented!("signature-only design target")
    }
}

impl TerminalReadTransaction<'_, TerminalReadReady> {
    pub fn commit(self) -> Result<CommitOutcome, CommitFailure> {
        unimplemented!("signature-only design target")
    }

    pub fn abort(self) -> AbortInfo {
        unimplemented!("signature-only design target")
    }
}

pub struct TerminalReadEntry<'entry, A: TransactionalResource> {
    /* private */
}

impl<A: TransactionalResource> TerminalReadEntry<'_, A> {
    pub fn key(&self) -> &A::Key {
        unimplemented!("signature-only design target")
    }

    pub fn record_read(
        &mut self,
        observation: A::Observation,
    ) -> Result<(), AccessError> {
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

The terminal-read handle is deliberately a different public type, not a mode
bit on `Transaction<Active>`. `TerminalReadOpen` exposes exactly one consuming
`with_terminal_read_batch` transition. The callback receives only the retained
key and a one-shot `record_read`; it cannot create local state, predicates,
intents, prepared state, locks, or another transaction item. Success returns
`TerminalReadReady`, which exposes only consuming `commit` and `abort`.
Neither typestate has `with_item`, `with_item_session`, unique-write batching,
or a conversion into the general transaction. Each callback invocation MUST
record exactly one ordinary observation. Duplicate keys are permitted because
the batch cannot write or perform later item lookup; they remain independent,
conservative observations.

An outer access error or unwind definitely aborts the retained prefix and at
most one pending key. `abort_with_access_error` exists for an adapter that must
perform fallible directory preparation before handing keys to core; it applies
the normal poisoning policy and returns the original error. Like ordinary STO,
effects performed by the visitor itself are not rolled back if final
certification later conflicts. The capability proof that authorizes this
restricted representation is defined in Section 8.3, and its commit path is
defined in Section 10.2.

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
exist. `ResourceClass` distinguishes adapter resource domains such as records
and a scan-only directory generation. `ResourceKey` is an adapter-owned, stable,
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
according to their documented abstract semantics. The conceptual snapshot need
not be retained in the item: the general Masstree adapter stores a compact
first-observation token and reloads committed bytes against that same OCC
generation on every later unmodified access.

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
unique resource-group append lane feed the same private typed item storage. The core
performs `TypeId`-checked key and item downcasts; no public `Any`,
caller-supplied vtable, or unchecked extraction exists. Applications normally
call adapter methods such as `get` and `put`, not this adapter-author surface
directly.

### 7.4 Physical lock planning modes

**[RUST]** The default correctness-first implementation deduplicates physical lock
requests by full `LockIdentity`, sorts the unique requests by that identity,
acquires them in ascending order, and releases them exactly once in reverse
order. Logical items sharing a coarse lock retain independent observations and
install state, but share one core-owned lock token. Sorting `ItemIdentity` alone
is insufficient because different logical orderings can alias the same pair of
physical locks and form a cycle.

For the general Masstree adapter, only staged record items emit physical lock
requests. Its scan-only directory-generation item is a prepared-free read and
emits no lock, intent, or install callback. Canonical ordering therefore applies
only to the transaction's record locks.

The core also exposes `PreflightContext::require_unique_lock` for a proven
transaction-wide uniqueness profile. If any callback selects this lane, every
physical lock request emitted by every adapter in that transaction MUST use the
same lane, and no two requests may have equal `LockIdentity` values. Core
rejects a canonical/unique mixture and verifies the uniqueness claim with a
cheap filter followed by exact equality on possible collisions. Successful
requests are acquired in request order and released in reverse request order;
there is no deduplication or sorting pass. This remains deadlock-free only
because every `try_acquire` is bounded and nonblocking and a failure releases
the acquired prefix.

`TableConfig::with_unique_lock_requests(true)` makes record writes from that
table select this lane, but a table-local flag cannot prove a transaction-wide
property. The embedding integration must ensure that every other lock-emitting
adapter reachable by the transaction uses the unique lane and that its physical
identities cannot alias. The table's directory-generation resource is read-only,
emits no request, and does not constrain the mode. Enabling one table and writing
through a default-configured table or unrelated canonical adapter in the same
transaction is an integration error that fails during preflight.

The direct-commit capability in Section 8.3 is a separate homogeneous plan,
not another `PreflightContext` request mode. It is selected only when the live
typed batch contains at least one write, contains no predicate, has not created
ordinary `Prepared` state, and every exact resource binding returns the same
static capability. Safe direct constructors retain core's exact duplicate-lock
check. An unsafe injective constructor may omit that identity vector only when
the adapter proves that distinct full item identities map to distinct physical
locks or distinct `(target, token)` pairs across every eligible binding. Direct
acquisition remains bounded and nonblocking, and every acquired prefix is
released exactly once in reverse order on failure.

An adapter that cannot expose a canonical identity for an internal physical
lock MUST use a strictly nonblocking or finitely bounded acquisition attempt.
It MUST never wait indefinitely while holding another lock, and it MUST release
every successful acquisition exactly once on failure. This fallback may cause
false aborts and is not permission to hide a blocking mutex from the lock plan.

The canonical default deliberately differs from the paper, which used item
insertion order plus bounded spinning. The paper reports a nearly 10% combined
TPC-C improvement from expected-`O(1)` item lookup and avoiding write-set
sorting, without isolating sorting's contribution. The unique lane recovers
request-order planning only for an integration that proves its stronger
contract. The homogeneous direct lane can additionally omit the general
identity and prepared-state representation under its explicit capability
proof. Neither alternative weakens the general adapter default.

## 8. Rust adapter interface and protocol

The v1 signatures in this section are the normative adapter-author contract.
The ownership, type-state, phase, fallibility, safe base traits, and explicit
unsafe direct-token seam are design requirements. The signature blocks use
`unimplemented!()` only to keep
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
| specialized homogeneous commit callbacks | `A::direct_commit_capability()` returning a core-defined `DirectCommitCapability<A>` | An optional typed capability selects a sealed direct plan; safe constructors keep exact duplicate-lock checking, while unsafe injective constructors make the omitted check and any write-at-acquisition certification an explicit proof obligation. |
| `Sto::commit_id()` | `InstallContext::occ_commit_id` and committed `LockDisposition` | Core OCC identity is phase-scoped and remains distinct from upper timestamps. |
| `new_item`, `fresh_item`, `read_item`, `check_item` | `with_item`, resolved access, `with_unique_item_batch`, and `ObservationState` transitions | The fast lane requires a core-checked exact uniqueness proof; the first group starts an empty transaction and later groups use distinct resource bindings of the same adapter type. An earlier exact indexed prefix is allowed. It exposes no unchecked item access. |
| specialized terminal read loop | `TerminalReadTransaction<Open/Ready>`, `TerminalReadEntry`, and `TerminalReadBatchCapability` | A distinct consuming typestate proves there can be no later operation or mutation; the batch stores only typed keys and observations. |
| clear/user flags | private observation/preparation states and `A::Local` | Adapters own typed local data, not core flag bits. |
| `TObject::print` | ordinary adapter diagnostics outside the commit protocol | No formatting callback runs while committing. |
| Mako `get_table_id` / `get_is_remote` extensions | upper integration layer | Distribution and table-routing policy are not STO callbacks. |
| static `Sto` / current transaction TLS | `WorkerContext` and `Transaction<'worker, Active>` | Worker affinity and transaction lifetime are explicit. |

This split preserves the paper's central virtual-callback seam while making the
equivalent of `TItem` a core-controlled generic type. A datatype exposes normal
methods such as `get`, `put`, or `increment`; those methods use the entry API
below and are not themselves required trait methods.

The implemented public relationships are:

```text
ResourceKey                 OpacityToken
     |                           |
     +------ associated types ---+
                    |
          TransactionalResource
             |              |
             |              +-- optional PreflightFreeReadCapability
             |              +-- optional TerminalReadBatchCapability
             |              +-- optional DirectCommitCapability
             |
             +-- optional DirectBorrowedLockTarget<L>

TransactionLock
     |
     +-- DirectTokenLock                    unsafe compact-token extension

RegisteredResource<A>                      typed registered binding
Transaction<'worker, Active>               core-owned item coordinator
```

`TransactionalResource` is the adapter extension trait. `TransactionLock` is
the independent physical-lock contract used by preflight and commit. The
capability types are core-owned structs, not adapter-defined commit drivers.
They authorize narrower sealed paths while leaving the ordinary trait methods
available as fallback. `DirectBorrowedLockTarget` only supplies a stable
adapter-owned lock target. `DirectTokenLock` is the sole unsafe trait in this
hierarchy because its compact token must name the exact stable lock that its
guard protects.

### 8.2 Object registration and typed item access

One transactional object can expose several resource classes. For example, one
Masstree table registers a record resource and a scan-only directory-generation
resource under the same `ObjectId`. Each `(ObjectId, ResourceClass)` pair binds exactly one
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
pub struct UniqueItemKeyIndex { /* reusable private buckets */ }
pub struct UniqueItemKeys<'keys, K: ResourceKey> { /* private */ }

impl UniqueItemKeyIndex {
    pub const fn new() -> Self {
        unimplemented!("signature-only design target")
    }

    pub fn try_reserve_for_len(
        &mut self,
        needed: usize,
    ) -> Result<(), std::collections::TryReserveError> {
        unimplemented!("signature-only design target")
    }
}

impl<'keys, K: ResourceKey> UniqueItemKeys<'keys, K> {
    pub fn try_new(keys: &'keys [K]) -> Option<Self> {
        unimplemented!("signature-only design target")
    }

    pub fn try_new_indexed(
        keys: &'keys [K],
        order: &mut Vec<usize>,
    ) -> Result<Option<Self>, std::collections::TryReserveError> {
        unimplemented!("signature-only design target")
    }

    pub fn try_new_hashed(
        keys: &'keys [K],
        index: &mut UniqueItemKeyIndex,
    ) -> Result<Option<Self>, std::collections::TryReserveError> {
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
but the erased address is never dereferenced or converted back into an `Arc`.
A successful scalar access retains a strong binding handle in the live item.
Within a scoped resource session the caller's borrowed handle keeps a newly
validated address alive; if that session or a direct unique batch succeeds
without retaining any item, core restores the previous item-backed cache entry
before the borrow ends. Any error or unwind dooms the attempt before another
access can consult a possibly unretained entry. The one-word cache is reset at
every transaction begin. Distinct live classes, adapter types, and runtimes
have distinct binding allocations and take the full validation path.

`with_unique_item_batch` is a safe optimization for a batch whose stable item
identities are already available. `UniqueItemKeys::try_new` performs an
allocation-free pairwise `Eq` scan. `try_new_indexed` sorts a reusable index
permutation and checks adjacent keys with exact equality.
`try_new_hashed` uses a reusable generation-tagged open-addressed
`UniqueItemKeyIndex`; a hash selects candidate buckets, but full `Eq` confirms
every possible duplicate. Hash collisions therefore neither alias distinct
keys nor admit a repeated key. All three constructors return no proof when an
exact duplicate exists, and all allocation growth is fallible. The first group
requires an otherwise empty transaction.
Another group may append without hashing when the transaction still uses the
same typed storage and the group has a distinct exact `RegisteredResource`
binding. Earlier scalar accesses may already have populated an exact item-index
prefix. A binding may occur in only one direct group; therefore exact
uniqueness inside each group implies uniqueness of every full `(ObjectId,
ResourceClass, key)` identity even when two resources use equal keys. Core
checks the proposed total live-item count against the configured limit and
reserves the enlarged typed vectors before initializing any item in the new
group. It then appends pooled typed items in group and key order without
hashing, probing, or extending the item index.

The operation receives the ordinary full `Entry`, so observations, predicates,
and intents retain their normal transition rules. If a later ordinary or
resolved access follows the groups, core notices that the index length trails
the live item count, reserves the complete open-addressed table, hashes only
the missing suffix, and installs those entries before the later lookup. At all
times current-generation index entries cover exactly the live slots
`[0, item_index.len)`, while `[item_index.len, item_count)` is an unindexed
typed suffix. Full erased-identity equality still resolves hash collisions and
distinguishes equal keys in different resource bindings, so a later access to
a batched identity reuses its observation or staged intent. An unmodified
Masstree record reloads its committed value and checks the original generation;
a staged record reads its intent directly. A commit or abort traverses the
complete typed batch independently of index coverage. When all live bindings
select the same exact direct-commit capability, those groups also remain
eligible for one concrete direct plan; distinct physical lock identity is
still enforced by that capability.

A repeated binding, a different adapter type, or prior materialization into
ordinary heterogeneous storage makes another direct group ineligible. Capacity
failure, callback error, initialization failure, or unwind dooms the transaction. Items
successfully activated before a later group's failure join the earlier prefix
in reverse abort cleanup; the failing item never becomes active. Successful
finish retains the same worker-local typed slots and their exact adapter
bindings as the ordinary path, so later attempts may reactivate the same group
layout or rebind each pooled slot under the ordinary contained-destruction
boundary.

`sto-masstree` uses pairwise comparison for batches of at most 32 keys. Larger
general batches use the sorted indexed proof when their cold duplicate path
needs a permutation. Larger fixed-record batches use the hashed proof and run
the sorted exact key-to-record alias check only when a duplicate resolved
identity is found. This policy changes proof cost, not equality semantics.

The same append operation is available inside `ResolvedItemSession` as
`try_with_unique_item_batch`. It returns `Ok(false)` without invoking the
operation or changing transaction state when the session's resource binding
already appeared, the active typed adapter differs, or typed storage has
already been materialized. `can_start_unique_item_batch` performs the same cheap structural check
before the caller constructs an exact uniqueness proof. An adapter can
therefore perform lookup and prefetch work once, try the append lane, and take
ordinary per-item session lookup on `false` without opening another failure
boundary. That scalar fallback builds or uses the exact index and safely
handles a repeated identity. A real append error is latched as the session's
first error, and later session operations report a doomed transaction.

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

pub type TerminalReadBatchValidate<A> = for<'context> fn(
    &A,
    &<A as TransactionalResource>::Key,
    &<A as TransactionalResource>::Observation,
    &PreflightFreeValidationContext<'context>,
) -> Result<(), CheckError>;

pub struct TerminalReadBatchCapability<
    A: TransactionalResource,
> { /* private */ }

impl<A: TransactionalResource> TerminalReadBatchCapability<A> {
    pub const fn new_drop_only(
        validate: TerminalReadBatchValidate<A>,
    ) -> Self {
        unimplemented!("signature-only design target")
    }
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

pub struct UniqueLockCommitCapability<
    A: TransactionalResource,
    L: TransactionLock,
> { /* private callbacks */ }

pub struct BorrowedUniqueLockCommitCapability<
    A: TransactionalResource,
    L: TransactionLock,
> { /* private callbacks */ }

pub struct BorrowedInjectiveLockCommitCapability<
    A: TransactionalResource,
    L: DirectTokenLock,
> { /* private callbacks */ }

pub struct DirectCommitCapability<A: TransactionalResource> { /* sealed */ }

pub trait DirectBorrowedLockTarget<L: TransactionLock>:
    TransactionalResource
{
    fn direct_borrowed_lock_target(&self) -> &L;
}

pub unsafe trait DirectTokenLock: TransactionLock {
    type Token: Copy + 'static;

    fn try_acquire_token(
        &self,
        runtime_id: RuntimeId,
        token: Self::Token,
        cx: &AcquireContext<'_>,
    ) -> Result<Self::Guard, AcquireError>;
}

impl<A, L> BorrowedInjectiveLockCommitCapability<A, L>
where
    A: TransactionalResource,
    L: DirectTokenLock,
{
    pub const unsafe fn with_write_acquisition_certification(mut self) -> Self {
        unimplemented!("signature-only design target")
    }
}

impl<A: TransactionalResource> DirectCommitCapability<A> {
    pub const fn unique_lock<L: TransactionLock>(
        implementation: &'static UniqueLockCommitCapability<A, L>,
    ) -> Self
    where
        A: TransactionalResource<Predicate = NoPredicate>,
    {
        unimplemented!("signature-only design target")
    }

    pub const fn borrowed_unique_lock<L: TransactionLock>(
        implementation: &'static BorrowedUniqueLockCommitCapability<A, L>,
    ) -> Self
    where
        A: DirectBorrowedLockTarget<L>
            + TransactionalResource<Predicate = NoPredicate>,
    {
        unimplemented!("signature-only design target")
    }

    pub const unsafe fn borrowed_injective_lock<L: TransactionLock>(
        implementation: &'static BorrowedUniqueLockCommitCapability<A, L>,
    ) -> Self
    where
        A: DirectBorrowedLockTarget<L>
            + TransactionalResource<Predicate = NoPredicate>,
    {
        unimplemented!("signature-only design target")
    }

    pub const unsafe fn borrowed_injective_token_lock<L: DirectTokenLock>(
        implementation: &'static BorrowedInjectiveLockCommitCapability<A, L>,
    ) -> Self
    where
        A: DirectBorrowedLockTarget<L>
            + TransactionalResource<Predicate = NoPredicate>,
    {
        unimplemented!("signature-only design target")
    }

    pub const fn with_drop_only_committed_finish(self) -> Self {
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

    fn terminal_read_batch_capability(
        &self,
    ) -> Option<&'static TerminalReadBatchCapability<Self>> {
        None
    }

    fn direct_commit_capability(
        &self,
    ) -> Option<&'static DirectCommitCapability<Self>> {
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

`direct_commit_capability` is the optional replacement for the ordinary
`preflight`/`Prepared`/general-lock-plan sequence. The returned capability MUST
be one static value for the complete binding lifetime. It supplies typed
prepare, validate, and install callbacks to a sealed core driver. The core
selects it only for the homogeneous eligibility conditions in Section 7.4;
otherwise the same adapter takes the ordinary trait methods above. The direct
plan uses its own callbacks rather than making the ordinary callbacks
conditionally valid, and it does not expose a pluggable commit driver.

The unsafe `with_write_acquisition_certification` builder is narrower still.
For every writing item, prepare MUST bind the exact execution observation and
exact physical lock into the compact token. `try_acquire_token` MUST atomically
reject a stale observation or return a guard that holds that exact lock and
excludes every change through install and release. Install MUST independently
check that same target, owner, held guard, and commit generation. The skipped
write-validation callback MUST have no side effect and MUST be guaranteed to
return success for that guard. Core still runs the direct validation callback
for every read-only item, including scan- or directory-generation resources;
it skips only writes already certified by exact-token acquisition. Token
compactness never removes certification.

The `ObservationOrder` returned by `revalidate_predicate` is the bound through
which the unchanged predicate was just certified. The core advances its
checked-through opacity bound but does not replace the predicate or perform the
commit-time state transition. `Unordered` forces the conservative full-
revalidation path.

`TerminalReadBatchCapability` is stronger than
`PreflightFreeReadCapability::new_drop_only`. It proves that a homogeneous
batch operation needs only `A::Key` and `A::Observation`: core MUST NOT
construct `A::Local`, `A::Intent`, or `A::Prepared`, and MUST NOT invoke
`new_local`, `preflight`, `install`, or `finish` for the terminal batch. Its
validation callback performs the same final certification as `validate_read`,
using the restricted `PreflightFreeValidationContext` and no lock guard.
Dropping each key and observation MUST be the complete cleanup on both commit
and abort; cleanup cannot depend on shared mutation, outcome, transaction-local
state, or a phase context. An operation using the capability is therefore a
read only and cannot create an untracked cleanup obligation.

As with the ordinary prepared-free capability, an adapter MUST return the same
`'static` terminal capability for the complete registered lifetime. Merely
having a cheap `validate_read` implementation is not enough to advertise it.
The capability opts into the representation and cleanup contract; the
`TerminalReadOpen -> TerminalReadReady` consuming transition proves that no
later general operation can invalidate that contract.

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
    PreflightFreeRead,
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

`PreparationState::PreflightFreeRead` is a core-selected marker for an ordinary
`ItemBox<A>` that used `PreflightFreeReadCapability`; it is not adapter-owned
prepared state. Terminal batches do not allocate `ItemBox<A>` at all and
therefore do not add another state to this enum. Their homogeneous pooled
storage contains parallel typed key and observation vectors plus one retained
registered-resource binding and capability.

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

    pub fn require_unique_lock<L: TransactionLock>(
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

`require_lock` and `require_unique_lock` allocate one private erased lock frame
during preflight. That frame contains an inline `Option<L::Guard>`, so
`try_acquire` can place its result into already allocated storage. Lock
acquisition MUST be bounded and nonblocking and MUST NOT allocate. `Err` leaves
the lock unowned. A successful guard remains in core custody until release;
`release` makes it inert and does not fail or panic.

The canonical lane deduplicates and sorts by full `LockIdentity`. Duplicate
requests must also have the same lock target `TypeId` and canonical `Arc`
instance before they share a frame; a mismatch is `AdapterFault`, not an
unchecked cast. The unique lane instead rejects duplicate identities and
preserves request order under the transaction-wide restriction in Section 7.4.
`LockUse<L>` is unforgeable and tied to one plan nonce. Every phase-context
access checks that nonce, slot, identity, and `TypeId` before downcasting. A mismatch during
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
- `PreflightFreeValidationContext` exposes final-certification metadata but no
  lock-plan or guard resolution, and is used by both prepared-free ordinary
  reads and terminal read batches;
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
vtable. The base adapter protocol is safe. The optional compact direct-token
extension deliberately adds the unsafe public `DirectTokenLock` trait and
unsafe injective capability constructors. Their safety contract covers stable
target/token identity, token validation before dereference, and one-to-one
physical-lock mapping; it does not alter the safety of the base traits.

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
| terminal batch operation | execution | outer `AccessError`, missing `record_read`, or unwind | Consume/doom the open handle and definitely abort the retained prefix; poison on adapter fault. |
| `TransactionalResource::preflight` | preflight | `PrepareError::{Conflict, Capacity, Fault}` | Definite abort for conflict/capacity; poison for adapter fault. |
| `TransactionLock::try_acquire` | locking | `AcquireError::{Conflict, Fault}` | Reverse-release acquired guards on conflict; poison on fault. |
| `revalidate_read` / `revalidate_predicate` | execution-time opacity | `CheckError::{Conflict, Fault}` | Abort before exposing an inconsistent result; poison on fault. |
| `TerminalReadBatchCapability::validate` | terminal final certification | `CheckError::{Conflict, Fault}` | Definite read-only abort for conflict; poison with a definite aborted outcome on fault. |
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
every inserted item unless the item entered an explicitly authorized
core-owned drop-only cleanup path. Terminal batches use drop-only cleanup on
both outcomes and never call `finish`. A direct capability may opt into
drop-only cleanup only after committed validation, installation, and release;
aborted direct items still call `finish` exactly once. If `finish` itself
violates the contract, the
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
acquire an untracked blocking lock. `install` MUST NOT run arbitrary or foreign
destructors, block, allocate fallibly, or perform cleanup whose latency or
effects are outside the adapter's audited publication protocol. Displaced
user-owned buffers, foreign allocations, and general cleanup obligations move
into preallocated state; `finish(FinishDisposition::Committed)` releases or
retires them after all transaction locks are published and released. A
representation may perform a bounded internal reference-count decrement and
possible deallocation during install only when the referent contains no user
destructor or foreign resource and the adapter's install proof explicitly
accounts for that operation. The Masstree committed-state `ArcSwap` replacement
uses this narrow exception for its internal byte-only `SharedValue`.

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

Rust-owned record values MUST use one of these race-free forms:

- payload and descriptor words are atomic, and bytes are exposed only after an
  OCC version-before/version-after sandwich succeeds;
- an immutable allocation is acquired through a sound atomic `Arc` or hazard
  mechanism and retained until the reader finishes, with the same OCC
  sandwich when the reference is part of versioned record state;
- bytes are copied while holding a real synchronization guard; or
- bytes are copied inside C++ while C++ owns the synchronization and the ABI
  returns only owned bytes.

Masstree v1 uses the first two forms. Values through 38 bytes occupy inline
atomic words. An opted-in bounded table stores values through 160 bytes in the
same inline prefix plus an adjacent stable atomic cell. Longer values, and
standard-table values longer than 38 bytes, use an owned immutable allocation
published through the record's embedded `ArcSwapOption`. Liveness and the value
representation are encoded by one packed atomic descriptor. Readers accept
that descriptor and payload only after the surrounding OCC version sandwich
succeeds, so no torn liveness/value combination is exposed.

### 9.4 Baseline ordering table

The first implementation favors an auditable ordering baseline. Weaker
orderings require a written happens-before argument plus Loom or equivalent
tests.

| Event | Baseline ordering | Required effect |
| --- | --- | --- |
| Load an unlocked version before a committed value | `Acquire` | Observe state published before the version. |
| Load packed descriptor and atomic payload words | `Acquire` | Copy only the representation and length named by the descriptor. |
| Acquire a shared fallback lease | `ArcSwapOption::load_full` | Retain one fully initialized immutable allocation. |
| Reload version after the value load | `Acquire` | Detect intervening lock/install. |
| Acquire version lock with CAS | `AcqRel` success, `Acquire` failure | Own mutation and observe prior state. |
| Publish a prepared value representation under own lock | `Release` | Order initialized atomic words or the shared allocation before commit-unlock. |
| Advance generation while retaining own lock | `Release` | Keep the resource unavailable until publication completes. |
| Commit-unlock with the new generation | `Release` | Atomically make the installed state available to acquiring readers. |
| Abort-unlock to original version | `Release` | Release ownership without publishing a write. |
| Load runtime opacity bound | `Acquire` | Order clock observations. |
| Advance runtime commit clock | `AcqRel` | Produce a unique ordered commit value. |

Rust and C++ atomics are not assumed to have interoperable object layout.
Masstree stores only a nonzero scalar directory token. In the general lane that
token is a logical `RecordId`; in a private direct table it is the exposed
address of a stable Rust registry entry. C++ stores, copies, and compares that
scalar but never dereferences or prefetches it as a pointer. Every record atomic
is owned and accessed by Rust.

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

The normative ordinary local protocol is listed below. The homogeneous direct
protocol later in this section replaces its general preflight, plan, and
callback representation while retaining the same metadata, certification,
irreversibility, publication, and failure boundaries.

1. **Preflight.** Deduplicate and finalize items; allocate all prepared write
   representations, adapter scratch, and lock vectors; derive, deduplicate, and
   sort the canonical physical lock plan by calling
   `TransactionalResource::preflight` for each
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
   correctness MUST NOT depend on their relative position. The Masstree
   directory-generation observation has no intent and therefore has no install
   callback.
10. **Unlock and finish.** Call `TransactionLock::release` with
    `LockDisposition::Committed` in reverse acquisition order, publishing new
    generations. The canonical Masstree plan releases its record locks in
    reverse canonical order; a proven unique-request plan releases them in
    reverse request order. All installs have already completed, and any
    still-locked or newly advanced record prevents a reader from certifying a
    partial result. Then finish items in reverse order. Ordinary
    items use `TransactionalResource::finish` with
    `FinishDisposition::Committed`;
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

#### Homogeneous direct commit

`DirectCommitCapability<A>` is the public opt-in handle for one sealed,
core-owned alternate plan. Its typed construction hierarchy is:

```text
DirectCommitCapability<A>
  -> UniqueLockCommitCapability<A, L>
       owns one LockRequest<L> per write; core checks exact identities
  -> BorrowedUniqueLockCommitCapability<A, L>
       reborrows one stable adapter-owned target; core checks identities
  -> BorrowedInjectiveLockCommitCapability<A, L>
       retains one compact token per write; unsafe injectivity proof;
       optional exact-observation certification during token acquisition
       L: DirectTokenLock (unsafe trait)
```

The first two forms use safe constructors and retain an exact duplicate-lock
proof. The borrowed injective forms use unsafe constructors because core omits
that transaction-wide identity set. `DirectBorrowedLockTarget<L>` promises a
stable target borrow. `DirectTokenLock` additionally proves that every token
accepted by its safe acquisition method names exactly one stable physical lock,
that all malleable token shape is checked before dereference, and that the
returned guard owns that same lock. The unsafe capability proof extends this
to injectivity across distinct full `(binding, key)` item identities. A compact
token still participates in certification: under the write-at-acquisition
option it also carries the exact observed state that acquisition must prove
current before returning the held guard.

The plan is eligible only for a nonempty homogeneous typed batch with at least
one intent, no predicate, no prior ordinary preparation, and the same exact
static capability from every live resource binding. A failed eligibility check
silently selects the ordinary trait protocol. Direct preparation emits no lock
for a read and exactly one lock or token for a write. Core enforces the lock
limit, acquires in item/request order with bounded nonblocking attempts, and
reverse-releases the acquired prefix on failure. It then reserves the normal
commit metadata and defines the certification cut immediately before its final
validation pass. The default form validates every item through the capability
callback. A compact capability constructed with
`with_write_acquisition_certification` instead validates every read-only item
and skips only a writing item whose exact token acquisition already compared
its execution observation and returned the retained guard. Because that guard
excludes changes from acquisition through the certification cut and install,
the earlier check still proves the write's observed state at the cut. The plan
then crosses the same irreversible boundary, installs each write through its
exact held guard, and reverse-publishes/releases all guards. Callback panics
and release failures retain the ordinary definite-outcome or indeterminate
quarantine rules.

The direct plan does not construct `A::Prepared` and does not call the ordinary
`preflight`, `validate_read`, or `install` methods for that attempt. It does
retain `A::Local`, observations, intents, keys, and exact registered bindings.
By default it calls `A::finish` with `prepared == None` on every definite
outcome. `with_drop_only_committed_finish` may replace only the committed
finish callback with reverse core-owned field teardown after all publication
and release complete. Aborts still call `finish`; indeterminate attempts remain
quarantined without invented cleanup.

#### Terminal homogeneous read batches

The terminal path is a stronger, transaction-wide specialization and does not
enter the general `ItemBox` commit pipeline. Before recording the batch, an
adapter may perform fallible directory lookup and construct stable logical
keys while retaining the open handle. `with_terminal_read_batch` then validates
the resource/runtime binding, reserves homogeneous key and observation vectors,
retains one binding and one `TerminalReadBatchCapability`, and invokes the
restricted operation once per key. A callback must record exactly one
observation. It cannot stage a write, add another resource, or construct any
associated local, intent, predicate, or prepared value.

Commit defines its certification cut immediately before the capability's first
validation callback and validates every retained `(key, observation)` in order
with a lock-free `PreflightFreeValidationContext`. A conflict definitely aborts;
an adapter fault or callback panic poisons the runtime with a definite aborted
outcome. If every validation succeeds, that final certification is the definite
read-only commit boundary: there is no preflight, lock plan, core commit ID,
hook, irreversible install, publication, or adapter `finish` callback. Core
then drops observations and keys in reverse order and recycles the worker-local
homogeneous allocation. A destructor panic after certification poisons with a
definite committed outcome; before certification it poisons with a definite
aborted outcome. The terminal protocol therefore preserves the same
certification argument as an ordinary read-only transaction while removing
state that its consuming typestate proves unreachable.

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
   every ordinary or direct item; a terminal read handle instead performs its
   authorized reverse drop-only teardown;
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

A range scan must detect both a newly published directory entry and a liveness
change to a physical record already in its bounds. The v1 Masstree adapter uses
two covering observations:

- every nontrivial scan reads one coarse physical-directory generation;
- every admitted tombstone-interning attempt advances that generation while
  holding exclusive structural admission and before native publication can
  become visible;
- the scan holds shared structural admission while sampling the generation and
  traversing every physical record through its logical limit; and
- it observes each traversed record, including tombstones, and overlays its own
  staged mutations.

The directory generation is an unordered, prepared-free read. It has no
transactional intent or lock. Existing tombstone resurrection, live removal,
and value replacement do not change physical structure and are covered by the
corresponding record observations. A new physical key anywhere in the table
invalidates every earlier scan, even if the key is outside that scan's bounds or
the inserting transaction later aborts. These safe false conflicts are the v1
price for avoiding table-wide liveness intents and locks; this remains a
conservative baseline, not a claim of MassTrans-equivalent precision.

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

No dereferenceable C++ Masstree node, value, cursor, or raw RCU guard crosses
the v1 ABI. `mt_read_scope` is an ABI POD containing a generation-tagged,
implementation-owned owner cookie; that cookie may encode a native TLS address
but MUST be treated as opaque and MUST NOT be interpreted or dereferenced. The
actual RCU objects remain in native TLS.

Each ordinary scalar, one-shot strided, or scan ABI call is independently safe
and enters a native RCU region. Two explicit retention modes are available and
owned through safe Rust RAII. A tree-bound point-read scope retains both
structural-reader admission and RCU protection, rejects unrelated operations
through that worker, and makes same-tree structural writers wait. A worker-wide
RCU scope retains only the tree-independent RCU region: ordinary operations
keep their per-call structural admission, so one transaction may safely access
several trees and perform both reads and get-or-insert. This mirrors the C++ STO
transaction lifetime without turning the RCU capability into a snapshot or a
structural lock. After ordinary-operation validation proves that the active
scope belongs to the same runtime and worker, the native call reuses that RCU
region and omits its otherwise redundant local RCU guard. Calls without a
validated worker scope retain their established self-contained RCU lifetime.
Structural admission remains local to every operation in both modes.

Both capabilities use generation-tagged, implementation-owned owner cookies
and are mutually exclusive on one worker. Their shared POD representation does
not make their tokens interchangeable: distinct native TLS owner identities
reject cross-family use. Callers MUST keep either scope synchronous and SHOULD
keep it short; they MUST NOT deliberately hold it across I/O, blocking waits,
`.await`, or reentrant native work. A safe fixed-batch helper may grow
caller-owned result storage while a scope is active, so v1 does not claim
allocation-free or hard duration-bounded scopes. Any higher-layer visitor or
callback executed while a scope is active inherits the same caller contract.

Repeated fixed-width calls SHOULD use the strided batch or explicit point-read
scope. Transaction-shaped multi-tree access SHOULD use the worker RCU scope.
Implicit unowned pins, raw RCU-implementation guards, and cross-worker scope
transfer remain forbidden.

### 13.4 Reclamation policy

Published Masstree directory entries and published or publication-unknown Rust
records are append-only in v1. Their consumed registry slots and logical IDs
are never reused. A candidate
proved never to have entered the directory may release directory-reachable
record and key-byte quota, but its internal numeric slot ID remains consumed.
The private direct-directory mode may publish the stable slot address rather
than that numeric ID; the same no-move and no-reuse rule applies. An
implementation may drop separately allocated candidate backing or retain its
in-place arena slot; in the latter case the consumed-ID limit is also the hard
bound on failed-candidate slot memory. Logical deletion installs a tombstone;
it does not free the record or remove the directory key. These rules apply
during live runtime operation; a successful whole-runtime shutdown may free the
entire ownership unit after quiescence.

Physical reclamation requires all of:

- conditional directory removal tied to the expected scalar directory token;
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
binary key  ->  nonzero 64-bit directory token
                standard table: monotonic RegistryId
                private direct table: stable RegistryEntry address
```

C++ owns tree nodes, traversal, splits, allocation, and native RCU. Rust owns
the record registry, record locks/versions, atomic inline and stable-cell
storage, shared-value publication, tombstones, transaction items, the
physical-directory generation, and values.

One `TableInner` owns the native directory handle, record registry, and
directory-generation resource as one lifetime unit. The runtime retains that
unit until no operation can return an ID from its directory. The registry and
directory-generation resource cannot be dropped independently of the native
tree's safe reachability.

Masstree MUST use a dedicated integral `uint64_t` value type with value prefetch
disabled. C++ MUST NOT reinterpret, dereference, or pointer-prefetch either
token mode. Address reconstruction belongs only to the audited Rust direct-table
module after the private ownership invariant has been established.

### 14.2 Record registry

The raw Rust wrapper continues to call the scalar type `RecordId`, and zero
means directory absence in both modes. Its semantic payload depends on table
construction. `Table::new` stores a monotonically allocated nonzero
`RegistryId`. `Table::new_direct` creates and retains a fresh private directory
and stores the stable address of the corresponding registry entry as an opaque
`u64` token. C++ never uses the address as a pointer. Safe code cannot publish
through another handle, so every direct token was minted from this exact
registry and remains valid while `TableShared` is retained.

The registry still consumes an internal numeric slot ID monotonically in both
modes. IDs and slots are never reused in v1. The allocatable domain is bounded
by the minimum of the configured consumed-ID limit, the registry-addressable
slot domain, and `u64::MAX`; scalar ABI support for every nonzero `u64` does not
imply the registry can allocate every value. Exhaustion is a terminal capacity
error before native publication. Wrapping to zero or aliasing any consumed slot
is forbidden. Direct mode additionally rejects zero, misaligned, or
non-pointer-domain address encodings before publication.

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

Directory-token construction and registry resolution remain private to
`sto-masstree`. Safe callers cannot resolve guessed IDs or addresses or access
proven-unpublished/publication-unknown slots outside quarantine diagnostics.

An implementation may use segmented append-only storage or another design that
keeps registry lookup stable while the registry grows. The default
`RegistryLayout::LazySegmented` publishes fixed-size `RegistrySegment`s through
segment-level `OnceLock`s. A published segment already contains eagerly
initialized slots of the table's standard or extended entry type; an atomic
`UNALLOCATED -> RESERVED -> READY` transition claims and publishes a slot.
There is no per-slot `OnceLock`. Each entry owns its `Record` in place, and both
`Ready` and `Published` resolution borrow the same address. This removes a second
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

enum ValueRepr {
    Inline { len: u8, bytes: [u8; 38] },
    Shared(Arc<SharedValue>),
    Staged(Box<[u8]>),
    BorrowedStaged(&'static [u8]), // private, unsafe transaction-lifetime proof
}

enum SharedValue {
    Medium { len: u8, bytes: [u8; 128] },
    Heap(Vec<u8>),
}

struct CommittedRecordState {
    inline_head: [AtomicU64; 4],
    tail_and_descriptor: AtomicU64,
    shared: ArcSwapOption<SharedValue>,
}

struct StableAtomicValueCell {
    suffix: [AtomicU64; 16],
}

#[repr(C, align(64))]
struct RegistryEntry { /* Record at offset zero; total size 64 */ }

#[repr(C, align(64))]
struct StableRegistryEntry {
    base: RegistryEntry,         // offset 0
    cell: StableAtomicValueCell, // offset 64; total size 192
}
```

Values through 38 bytes are copied inline in both transaction-local and
committed state. This bound covers the measured TPC-C new-order, stock,
order-line, and secondary-index payloads without allocation or reference-count
traffic. The committed copy uses four full `AtomicU64` head words. Bytes 32--37
occupy the low 48 bits of a fifth `AtomicU64`; its high 16 bits hold the state
descriptor. Code accesses the tail and descriptor only through the complete
64-bit atomic, never through a mixed-size alias.

The standard table policy stores longer committed values through the embedded
`ArcSwapOption`. `SharedValue::Medium` keeps values through 128 bytes in the
same `Arc` allocation; larger values use `SharedValue::Heap`. Arbitrary lengths
and arbitrary binary bytes remain supported. `ValueRepr::Staged` is private
owned transaction storage used only when bounded atomic publication is enabled;
it never aliases a committed atomic cell. `BorrowedStaged` is restricted to an
unsafe private write lane for 39--160 byte replacements. The caller keeps the
slice fixed, readable, and immutable until commit or abort; install copies it
into the stable atomic cell, and safe cloning first converts it to owned
storage.

`TableConfig::with_bounded_atomic_values(true)` selects the extended entry.
Values from 39 through 160 bytes are staged in `Box<[u8]>` and committed through
the existing 38-byte atomic prefix plus the adjacent 16-word atomic suffix.
The private borrowed lane may replace the `Box` with `BorrowedStaged` for the
same size range. Values above 160 bytes use an owned `Arc<SharedValue>` while
staged and the embedded `ArcSwapOption` when committed. When the option is
disabled, the standard 64-byte entry and the same shared fallback begin at 39
bytes. Reads of an extended value reconstruct bytes into private scratch,
close the same OCC version sandwich, and expose the bytes only after
validation. The option is a table construction policy and never changes an
entry's layout after publication.

On the supported 64-bit layout, `Value`, `RecordState`, and `TableIntent` are 40
bytes; `Arc<SharedValue>` and `ArcSwapOption<SharedValue>` are eight bytes; the
four head words, packed tail/descriptor word, and embedded ArcSwap keep
`CommittedRecordState` at 48 bytes. The enclosing `Record` remains 56 bytes,
and the registry publication byte plus seven explicit padding bytes produces a
64-byte-aligned, 64-byte `RegistryEntry` stride. `StableAtomicValueCell` is 128
bytes, so `StableRegistryEntry` is 192 bytes with 64-byte alignment; its base is
at offset zero and its cell is at offset 64. The eager registry `max_bytes`
accounts for the selected concrete stride, and lazy segments allocate a
homogeneous standard or extended arena. There is no side directory for bounded
or shared values.

Per-record physical lock targets are stored separately in `Arc`-owned
16-record segments. A detached guard names the exact inline `AtomicVersion`,
while its core lock frame retains the owning slot arena through release. Point
reads pay neither an `Arc` clone for the record nor a separately allocated lock
object.

The transaction-local record metadata is deliberately smaller than a value:

```rust
struct RecordObservation {
    version: OccVersion,
    original_live: bool,
    old_was_shared: bool,
}
```

The transaction-local value is only an `AdapterRole` tag distinguishing record
and directory items. Record state is resolved through the stable registry when
an operation needs it, and writer preflight independently resolves the owning
lock-target segment before cloning its Arc into the lock plan. No registry or
lock-target pointer is retained in an item local.

On the supported 64-bit layout, `AdapterRole` and `Option<AdapterRole>` are one
byte, `RecordObservation` and the containing `TableObservation` are 16 bytes,
and the erased `TablePrepared` lock token is 24 bytes. An operation copies an
inline or stable-cell value into its own `Value` or output buffer. A shared
fallback read retains its acquired `Arc` lease until the operation finishes.

Shared fallback values use the eight-byte ArcSwap embedded in their exact
record. Reads and writes therefore need neither an ID-to-value side lookup nor
a hash or table-wide map lock. Replacing a shared value swaps the new Arc into
that slot; removing, shrinking, or moving it to bounded atomic storage clears
the slot while any Arc already acquired by a reader remains alive
independently.

The exact binary key is owned once by the private append-only directory. A
successful directory lookup returns a scalar capability into this table's
private registry, either a logical registry ID or a direct stable address;
safe callers cannot forge tokens, insert through another handle, or pair one
registry with another directory. Repeating every key in `Record`
and comparing it on each hit would therefore duplicate the trusted directory
binding rather than strengthen the safe contract. Likewise, the per-record
lock identity is reconstructed without allocation from table identity plus
the scalar token only for a staged record; it is not stored in the read-hot
slot.

#### Direct cached-record layout

The direct plan's held guard caches one `CachedRecord`, represented by one
eight-byte `NonNull<RegistryEntry>` on the supported 64-bit target. Bit zero is
the extended-entry tag. Both concrete entries are 64-byte aligned,
`RegistryEntry::record` and `StableRegistryEntry::base` are at offset zero, and
the extended cell is at offset 64, so the tag consumes no address information.
Tag insertion and removal use strict-provenance `NonNull::map_addr`; code masks
the tag before any dereference.

Debug builds re-resolve the original directory token and require the same entry
address and standard/extended kind before using the cache. Release builds rely
on the closed direct-directory proof: one immutable layout kind per table,
only this registry mints directory values, boxed arenas are append-only and
stable, slots never move or are reused, and the transaction item and lock target
retain `TableShared` through the last guard access. Pinned layout tests require
`CachedRecord` size/alignment 8/8, `RegistryEntry` size/alignment 64/64,
`StableRegistryEntry` size/alignment 192/64, base offset zero, and cell offset
64.

The inline snapshot is race-free without a read-side mutex. A reader observes
the OCC version, Acquire-loads the packed tail/descriptor word first, decodes
its descriptor and tail, loads only the head words named by its length, then
validates the same OCC version. A writer already holds that version exclusively,
Release-publishes the updating descriptor through the whole packed word,
writes the needed head words, Release-publishes one final packed
tail/descriptor word, and later Release-unlocks the OCC version. The final
Release and initial Acquire order the relaxed head stores. A reader that
overlaps publication may assemble words from different generations, but its
version sandwich must reject. A reader that observes the new unlocked
generation also observes the preceding descriptor and word stores. Bytes
beyond the descriptor's length are never exposed.

The bounded atomic snapshot extends the same proof. A reader samples the
descriptor, loads the inline prefix and only the suffix words named by its
length, and validates the same OCC version before copying private scratch to
the caller. A writer owns that version, publishes the updating descriptor,
stores the prefix and suffix atoms, publishes the final length descriptor, and
then releases the version. A torn reconstruction therefore cannot pass the
version sandwich.

The large snapshot has a separate, narrow reference-safety proof. After seeing
the shared descriptor, a reader uses that record's embedded
`ArcSwapOption::load_full` to acquire an owned `Arc<SharedValue>`, then validates
the same OCC version. A writer already holds that record's OCC version
exclusively, publishes the updating descriptor, atomically swaps or clears the
Arc, and publishes the final descriptor before Release-unlocking the OCC
version. ArcSwap keeps a concurrently acquired old Arc alive even after the
slot drops its reference. Any reader overlapping physical replacement rejects
its version sandwich. A delayed reader that sampled the old shared descriptor
may find a cleared slot, but the version check is classified first and makes
that race a retryable conflict. A missing payload under the same stable
unlocked generation, or an interrupted publication, fails closed and poisons
the table. Dropping an incomplete publication guard Release-publishes the
poisoned descriptor through the complete packed word.

The embedded ArcSwap is atomic reference publication, not a second
transactional lock. It introduces no hidden wait-for edge during OCC install.
Publishing a replacement swaps or clears one internal `Arc<SharedValue>` and
may drop the displaced reference while the record lock is held. This is the
bounded internal exception from Section 8.7: `SharedValue` owns bytes only and
has no user callback or foreign resource, so the operation cannot run an
application destructor or fallible cleanup. General displaced resources still
belong in post-unlock `finish` state.
Any replacement snapshot mechanism still requires an equivalent reclamation
and ordering proof.

### 14.4 Point lookup and miss interning

Point access proceeds as follows:

1. Look up the key through the copied/scalar C ABI.
2. On a directory miss, atomically reserve quota and allocate a fully initialized
   tombstone candidate.
3. Acquire the table's nonblocking exclusive structural gate. If admission
   fails, prove the reserved candidate unpublished and return a retryable
   conflict without advancing the directory generation. Once admitted,
   advance that generation before calling native atomic `get_or_insert`.
4. When core creates the record's first transaction item, retain only its
   record-role tag. A standard table resolves the winning logical ID through
   the table-private registry and resolves its lock-target segment during
   writer preflight. A direct table validates and reconstructs its private
   address token; the direct lock guard may then retain the tagged
   `CachedRecord` described above. Read-only final certification and the
   restricted terminal batch use the corresponding checked table-private path.
   The exact key-to-token association is the trusted result of the exclusively
   owned safe directory boundary; no duplicate Rust key comparison is needed.
   Release retained quota only for a candidate whose nonpublication was
   positively proved by the ABI outcome.
5. On the first access, load an operation-local committed value through the
   tier-specific atomic or `ArcSwap` path, close a stable
   version-before/version-after check, and record its version,
   `original_live`, and `old_was_shared` classifications.
6. On a later unmodified access, resolve the checked directory token, reload another
   operation-local committed value, and validate it against that first version. A
   generation change or held lock is a retryable conflict and dooms the active
   transaction.
7. If the item already has a staged record intent, return that state directly
   without reloading committed storage. Drop every non-returned value copy or
   shared lease when its visitor or copy operation ends.

Metadata-only presence reads and presence-only mutations specialize steps
5--7. `contains_resolving` resolves the key, interns a stable tombstone on a
miss, and returns both transaction-local liveness and a `ResolvedRecord`.
`contains_resolved` validates an existing token and skips the native directory.
Both methods use staged liveness for read-your-writes. For an unstaged item they
install an ordinary record observation and retain the first logical presence
answer, so commit still rejects a concurrent record-generation change.

On their first unstaged access, these reads,
`put_with_previous_presence`, `remove_with_previous_presence`, their resolved
forms, and the expected-absent insert observe the record version, Acquire-load
only the committed packed descriptor word, validate the same version, and
retain the resulting `original_live` and `old_was_shared` bits. They do not load
inline head payload words, enter the embedded large-value `ArcSwap`, or decode
application bytes. A later unstaged presence-only access returns
`original_live`; a staged access returns the intent's liveness. Repeated
presence tests therefore retain one transaction-local logical snapshot without
another payload or descriptor access. A stable updating, poisoned, or invalid
descriptor fails closed; the same descriptor sampled inside a changed or held
version sandwich is a retryable conflict.

Value-returning point operations, visitors, and scans retain the full reload
path because they expose bytes. The fixed mutation callback also retains that
path even when its result-capture flag is false: its public callback contract
still receives `Option<&Value>` and may inspect the prior payload.

Every structurally admitted interning attempt advances the directory generation,
including a losing `Existing` outcome and an error after admission. This is a
deliberate conservative witness: native publication can become visible to point
lookups as soon as `get_or_insert` runs, so the generation must change first.
An aborted insert may therefore leave an interned tombstone and always leaves an
advanced generation, but it has no committed logical effect. A later activation
or removal of an existing tombstone changes only that record's version.

#### Resolved record tokens and bridge cache policy

`sto-masstree::ResolvedRecord` is the reusable result of resolving one binary
key through Masstree. Its fields are private. It carries the minting runtime,
table object, and private directory token, so a resolved operation rejects a
token from another runtime or table before interpreting its scalar payload.
The type contains no borrow, version observation, lock guard, or public address
operation. It remains valid across transactions while its table remains live
because registry slots and their directory tokens are never reused in v1.

Point lookups and scan rows can return a `ResolvedRecord` with the value. Later
`visit_get_resolved*`, `copy_get_resolved`, `put_resolved*`, and
`remove_resolved*` operations skip another Masstree traversal. They still enter
the same `TableAdapter` item, load or reuse the same transaction observation,
and run the normal final certification and commit protocol. A resolved token is
therefore an index-lookup capability, not permission to bypass STO.

The closed `sto-tpcc-ffi` bridge keeps a bounded worker-local mapping from an
exact table and key to `ResolvedRecord`. Its table policy controls lookup cost,
not correctness:

| Policy | Point behavior | Scan behavior |
| --- | --- | --- |
| `Full` | Probe and update a 4,096-entry direct-mapped cache, with a most-recent hit shortcut. | Cache callback-visible rows only when the logical scan limit is at most 16. |
| `LastOnly` | Probe and replace one exact most-recent point resolution. | Do not populate the cache. |
| `ReadThenWrite` | A get always traverses Masstree, then retains its resolution for the next matching put or remove. | Do not populate the cache. |
| `None` | Always resolve through Masstree and retain no point resolution. | Do not populate the cache. |

Entries store at most 32 key bytes and confirm both the full key and the
never-reused table identity. A hash collision can only evict or miss. It cannot
alias records. `sto_tpcc_table_create` selects `Full` for compatibility, while
the C++ TPC-C wrapper uses `sto_tpcc_table_create_with_cache_policy` and assigns
policies by table role:

| Current TPC-C tables | Policy | Reason |
| --- | --- | --- |
| `customer`, `warehouse`, `district`, `new_order`, `oorder` | `Full` | Reuse hot point resolutions, Delivery's scan-to-remove handoff, and its order get-to-put handoff. |
| `stock` | `ReadThenWrite` | Preserve the immediate get-to-put handoff without keeping a large cross-transaction cache. |
| `item`, `customer_name_idx`, `oorder_c_id_idx`, `history`, `stock_data`, `order_line` | `None` | Their current access pattern is point-read-only, insert-only, or scan/batch based, so retained point entries do not remove a later traversal. |

`LastOnly` remains available to other bridge users but the current TPC-C table
classifier does not select it. Fused Delivery also retains tokens directly from
its value-only scans for the same transaction. That path does not depend on the
cross-transaction cache and is why `order_line` can use `None` while Delivery
still updates each scanned row without a second tree lookup.

### 14.5 Writes and liveness changes

`put`, `insert`, and `remove` change only transaction-local staged state during
execution. The intent owns the complete replacement; preflight resolves the
record's lock-target segment, clones its Arc into the lock plan, and requests
its identity through the table's selected canonical or unique-request lane.
The owning Arc in the lock plan covers detached-guard release.

At commit, every record with a staged intent is locked and certified. The
ordinary plan performs final validation while holding the lock. The direct
plan may instead certify the exact observed version while acquiring its token
lock and retain that guard through install. Prepared value representations may
install in any callback order while all record locks remain held; publication
advances each affected record generation during release. There is no
table-wide liveness intent, lock request, installation, or publication.

Staging is the write boundary, matching C++ MassTrans. A same-value `put`, or a
write sequence whose final bytes equal its initially observed bytes, remains a
real writer and advances the record generation. An operation that never stages
an intent, such as removing a tombstone or retaining a value with
`PointMutation::Keep`, remains read-only. The observation's `original_live` bit
preserves the first transaction-local presence answer for repeated presence-only
operations; it no longer drives a table-wide intent. Its `old_was_shared` bit
tells install whether the embedded slot must contain an old Arc and must be
cleared when publishing an inline value or tombstone. Shared-to-shared
publication likewise verifies that the replaced slot contained an Arc;
nonshared-to-shared publication verifies that it did not. An invariant failure
unwinds through the publication guard, publishes the poisoned descriptor, and
poisons the table.

Liveness-changing transactions on distinct already-interned records therefore
do not conflict merely because they share a table. A scanner that could expose
a partially installed multi-record transaction cannot certify it: every
traversed physical record has its own observation, so at least one affected
record is still locked or has advanced from the observed generation.

`Table::new_direct` exposes the Masstree adapter's direct capability for record
and generation resources. Direct preparation retains one compact private
directory token containing the record identity and exact observed OCC version
for each write. Token acquisition compares that version while acquiring the
exact record lock and returns a held guard only on equality. Final validation
still rechecks every read-only record and directory- or scan-generation item;
it skips only writes already certified by acquisition. Install uses the held
guard, and release publishes in reverse acquisition order. The unsafe
injectivity proof rests on core item deduplication, a distinct retained
`TableShared` target for each table binding, and one stable address token per
registry entry. Compact tokens omit the general `LockIdentity` vector and the
repeated write-validation callback, not certification or core failure
handling.

The closed `sto-tpcc-ffi` integration constructs every TPC-C table with
`Table::new_direct` and also enables
`TableConfig::with_unique_lock_requests(true)` as the ordinary fallback. Its
transaction handles expose only these `sto-masstree` resources: no caller can
add a default-configured or unrelated adapter to the same transaction. Core
item deduplication emits at most one record request per table/token; generation
observations emit none, and each table owns a distinct lock target and
namespace. Those properties establish both the direct injectivity proof and
the fallback no-mixed-mode/unique-`LockIdentity` contract. General-purpose
table construction keeps the unique flag disabled because it cannot prove the
surrounding adapter graph.

TPC-C enables bounded atomic values only for `customer`, `district`, and
`warehouse`. It enables trusted scan value generation only for
`customer_name_idx`, `oorder_c_id_idx`, `new_order`, and `order_line`; point-only
tables avoid the shared generation RMW. The public `sto_tpcc_table_config`
places `trusted_scan_value_generation` at byte offset 64 and
`bounded_atomic_values` at byte offset 68, with size 72 on the supported 64-bit
ABI. The bounded flag consumed former trailing padding, so every caller must be
rebuilt and initialize the complete structure.

### 14.6 Scans

The ordinary scan path:

1. obtains the table's shared structural guard and observes the physical
   directory generation while that guard is held;
2. obtains copied key/scalar-token chunks from Masstree;
3. privately resolves every returned token through the exclusively owned
   directory-to-registry capability;
4. soundly reloads every returned record, including tombstones, against that
   item's first observed generation, or reads its staged intent directly;
5. adds a compact record read observation for every newly encountered record;
6. filters tombstones, whose later tombstone → live transition is covered by
   that record's observation;
7. applies lower/upper and inclusive/exclusive bounds exactly; and
8. merges transaction-local writes and deletes into key order.

`Table::visit_scan_with_scratch` is the allocation-retaining streaming form of
this protocol. It invokes the visitor only after the row's ordinary record
observation has been installed, borrows the packed directory key and the
operation-local or staged value for that invocation, and stops immediately
when the visitor requests it. Rows after that stop are not resolved or added to
the read set. `Table::visit_scan` supplies temporary scratch for convenience;
the existing owning `Table::scan` is a collector over the same internal engine,
so bounds, tombstone handling, RYW overlay, logical limits, and validation do
not fork into separate implementations.

Streaming delivery is not transactional rollback for callback side effects.
If a later chunk, record reload, or configured capacity limit fails, earlier
visitor calls have already occurred and the method returns the later error.
Callers must count or otherwise account for delivered rows as they run and
then follow the transaction's ordinary abort/retry policy. The TPC-C C bridge
does exactly this: `out_visited` includes every callback invocation even when a
later scan error determines the final status.

The C bridge serializes each possible tree insertion against point lookups and
scans for that tree; read-only calls may proceed together. A single copied C
chunk therefore has stable native structure, while a multi-chunk range remains
structurally weakly consistent because mutations may occur between calls.
Transactional consistency comes from the adapter's whole-scan structural gate,
record observations, and directory-generation validation. In opaque mode, the
adapter additionally performs execution-time validation before exposing a chunk
whose observations might be inconsistent.

For each call, “weakly consistent” still requires a gap-free key-ordered prefix
up to the reported stop/resume boundary: a concurrent append-only insert or
split cannot corrupt, omit, or duplicate an entry continuously present in that
traversed prefix. Exclusive resumption then partitions the remaining range. The
Masstree bridge MUST prove that property or serialize the conflicting native
operations. V1 takes the conservative serialization route twice: the ABI uses
per-tree shared/exclusive structural access for each call, and the
transactional table holds a nonblocking structural read guard across its whole
multi-chunk scan while tombstone interning holds the write guard across every
native publication outcome. The interner advances the directory generation
after write admission but before native publication. Thus a scan cannot overlap
that structural change, and a completed scan whose guard has been released
cannot later validate through it. A later protocol may replace the coarse gate
only after it proves the same prefix, resumption, and pre-publication witness
properties. Generation validation alone cannot hide a structural traversal gap.

Bounded and reverse scans apply the same proof in their traversal direction.
They observe all in-bound physical records, including tombstones, up to the
logical result limit. An existing tombstone beyond that limit or outside the
bounds cannot affect the result; a resurrection before the limit changes an
observed record version. A new physical directory entry remains deliberately
table-coarse and invalidates the scan regardless of bounds.

The paper's `TMasstree` used transaction items for both stored values and leaf
nodes and modified Masstree to expose the prior versions of leaves split by an
insertion. That let the adapter repair an earlier range witness after the same
transaction's eager insert. Rust v1 deliberately replaces that precision with
the coarse physical-directory generation, per-record observations, and staged
scan overlay. Consequently, a transaction that scans and then interns its own
first miss conservatively invalidates itself.

#### Trusted direct-table scan generation

`TableConfig::with_trusted_scan_value_generation(true)` enables a second,
coarser scan strategy for `Table::new_direct`. The hidden unsafe
`visit_scan_bytes_trusted_with_scratch` entry first falls back to the ordinary
row-item path unless the option and direct-token mode are both active. It also
falls back when the transaction already contains record items for this table,
because only the ordinary path can merge prior staged liveness and values into
the result.

On the trusted path, the transaction records one scan-resource generation
observation for the table. It holds one shared structural guard from that
observation through every copied native chunk. Each returned record is still
resolved through the private directory capability and read through its own OCC
version sandwich before any bytes reach the visitor, but it does not create a
per-row STO item. Before releasing structural admission, the operation checks
the same scan generation again. Core performs another final generation check
at the transaction certification cut. Thus the execution check prevents torn
or lock-covered bytes from escaping, while the retained table observation
certifies the complete result at commit.

The scan generation advances for every structurally admitted directory
interning attempt and every committed record publication. An update to an
existing record after the scan may join the same transaction: it is installed
after certification and serializes after the scan result. The first miss after
the scan advances the generation during execution and therefore
self-conflicts. Any other transaction's publication anywhere in the table also
invalidates the scan, including a value change outside its bounds. This
table-wide false-conflict cost and one atomic RMW per committed record
publication are why the option is limited to scan-bearing TPC-C tables and
disabled for point-only tables.

The method is unsafe only at its callback and private-tree boundary. The caller
must not retain borrowed pointers, reenter the transaction or worker, or unwind
across FFI. Structural admission, native result memory validation, OCC checks,
capacity limits, and final STO certification remain enforced.

### 14.7 Optional fixed-`u64` specialization

The Cargo feature `sto-masstree/fixed-u64` exposes `FixedU64Table` as a
separate, deliberately restricted table. It exists to remove general
binary-value representation cost from preloaded fixed-copy point workloads; it
does not change `Table`, weaken the general adapter contract, or claim support
for arbitrary Masstree workloads.

#### Ownership and lifecycle

`FixedU64Table::new` MUST create a fresh Masstree through the supplied native
runtime and worker and keep that directory privately owned. It MUST NOT accept
an externally clonable `Tree`, expose the new tree, or share a directory whose
`RecordId` bindings can be published outside this table. Cloning
`FixedU64Table` clones only its registered table handle and retains the same
private ownership unit. This exclusive directory-to-arena relationship is the
capability that permits a transaction-time `RecordId` to resolve without
rechecking publication metadata.

Construction requires a bounded `RegistryLayout::EagerContiguous` arena.
`insert_initial(worker, key, value)` is nontransactional loader-only work. It
publishes a fully initialized record before its immutable key-to-ID binding;
repeating a key is accepted only when the already published value is equal.
No transactional worker may use the table until `finish_initial_load()` takes
the structural publication gate and Release-publishes a permanent seal. After
the seal, loader calls fail and neither directory contents nor cold slot
publication state may ever change. There is no unseal transition.

#### Supported operation set

The specialized table supports only all-present, fixed-width point batches:

- terminal read batches through `visit_fixed_terminal`; and
- exactly unique update/keep batches through `modify_fixed` on an otherwise
  empty general transaction.

It provides no transactional insert, delete, resurrection, variable-width
value, scan, range predicate, or directory-generation item. A point miss invokes no
visitor/mutator callback and does not create an STO item. The terminal API may
return `RetryOrdinary` as a neutral dispatch result, and the mutation API may
return `None`, but this table intentionally has no ordinary miss/insertion
fallback; the caller must treat the miss as a workload violation or consult a
different general table. Distinct input keys resolving to one `RecordId` fail
closed. Duplicate reads are permitted, while a duplicate mutation batch takes
the no-callback result because direct unique item append cannot prove one item
per input position.

#### Record shape and OCC protocol

The hot arena layout is pinned by tests:

```rust
#[repr(C)]
struct FixedRecord {
    version: AtomicVersion,
    value: AtomicU64,
}
```

`FixedRecord` is exactly 16 bytes with eight-byte alignment: the adjacent OCC
word and atomic `u64` match the relevant physical shape of C++
`versioned_value_struct<u64>`. Publication state remains in a separate cold
one-byte `AtomicU8` sidecar per arena slot. Loader resolution checks that
sidecar; after permanent sealing, transaction-time resolution retains a bounds
check but does not load the immutable cold state. Lock-target segment metadata
is likewise outside the hot stride.

A read observes an unlocked `AtomicVersion`, loads the `AtomicU64`, and validates
the same version. It retries an unlocked generation change and reports a
conflict if a writer holds the version lock. A changed value uses the ordinary
STO lock plan and the same version word as its physical lock: preflight emits a
canonical record `LockIdentity`, final validation compares the observation with
the guard's pre-lock version, install stores the staged replacement while that
guard is held, and committed release publishes the new OCC generation. The
atomic value load/store may be relaxed because the version's acquire/release
protocol supplies publication ordering and the payload itself remains atomic.
`Intent = ()` marks the presence of a write while the replacement `u64` lives
in typed transaction-local state; a same-value `Put` is a no-op in this sealed
fixed-width specialization. The general Masstree adapter instead treats every
staged intent as a write, as specified in Section 14.5.

Thus fixed-`u64` uses the same core OCC certification, canonical locking,
validation, failure dispositions, and exact-once publication protocol as the
general adapter. Its performance advantage comes only from stronger lifecycle
and datatype restrictions, a smaller record, and the terminal/batch interfaces;
it is not a separate transaction algorithm.

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
- one-way directory sealing after all structural writers have stopped;
- scalar point lookup, a scoped repeated-read form, and fixed-width strided
  batch lookup (`mt_get_strided`);
- atomic get-or-insert with explicit publication disposition, `inserted`, and
  `winner` outputs; and
- copied forward/reverse scan chunks.

There is no update, value mutation, cursor, callback, borrowed output, physical
delete, or raw RCU pin in the public `mt_*` v1 ABI. The callback-free Payment
prefix, the commit-owning Payment, NewOrder, and Delivery calls, and the
commit-owning StockLevel tail in Section 17.1 are wrapper-private
`sto-tpcc-ffi` integrations. They do not extend this public Masstree surface.

Unless explicitly qualified below, this section's ABI requirements apply to
the versioned public `mt_*` surface. The native library also provides hidden
`mako_mtree_*_trusted` functions for its statically linked safe Rust facade.
Those symbols are outside ABI-version and feature negotiation and outside the
44-symbol public export fingerprint; shared-library consumers cannot link them.

The safe facade owns the retained native handles and validates runtime/worker
pairing, key lengths, enum values, candidate shape, slice relationships, and
output capacities before calling a hidden function. `Worker` is `!Send +
!Sync`, so safe Rust statically preserves its attaching-thread affinity; a
dynamic thread-ID assertion remains in debug builds only. Caller-provided raw
storage must still satisfy the documented readable/writable lifetime and
non-aliasing preconditions. The hidden native body may omit only the checks
covered by those safe types and preconditions. It still enforces
runtime poison and active-scope rules, structural reader/writer admission,
native RCU lifetime, C++ exception containment, and exact insertion publication
classification. The trusted scan decoder still validates counts, offsets, and
lengths before constructing Rust borrows, even where a private-tree caller
accepts native key ordering and range membership. Calling a hidden symbol from
application or foreign code, or violating any precondition, is outside the ABI
contract and may be undefined behavior.

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

The safe Masstree wrapper offers both an owning packed result and a borrowed
result backed by `PackedScanScratch`. Scratch storage retains initialized entry
descriptors and key-arena bytes across calls. A call may grow it, but a later
call at the same or smaller capacities performs no buffer allocation or
clearing. Validation is identical for both result forms, and the borrowed view
prevents scratch reuse until all entry and continuation-key borrows end.

### 15.4 Threading and RCU

Every `mt_thread` is bound to its creating OS thread and runtime. Safe Rust
represents it as `!Send + !Sync`. Workers are fixed and long-lived because the
current native core-ID and threadinfo registrations are capped and not truly
recycled. The ABI MUST report exhaustion rather than reaching an assertion or
`abort()`.

Every public `mt_*` tree operation dynamically validates the worker's current
OS thread, the worker's runtime, and the tree's runtime before traversal. The
safe Rust facade uses `!Send + !Sync` ownership for the first property on its
hidden fast lane, retains a debug-only thread-ID assertion, and dynamically
checks runtime pairing.

Structural readers publish into cacheline-private native-core slots. A writer
sets its per-tree exclusion flag and drains only slots named by the append-only
thread registry, rather than scanning the configured maximum core capacity.
Thread-handle publication participates in the same sequentially consistent
order as reader publication/recheck and the writer flag/snapshot: a newly
attaching reader is therefore either present in the writer snapshot or observes
the flag and retracts before native access.

Ordinary scalar, one-shot strided, and scan calls manage native RCU internally.
The explicit point-read scope is tree-bound and admits only point reads. The
explicit worker RCU scope is tree-independent and permits ordinary point,
insert, and scan calls while retaining their per-operation structural guards.
Both are RAII-owned, worker-affine, mutually exclusive, and follow the
synchronous caller contract in Section 13.3. A bridge-internal,
allocation-free, nonblocking collector may copy scan results into caller
buffers. Each scoped capability carries an opaque owner cookie, but no
dereferenceable node, value, cursor, or raw guard escapes in any mode.

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
2. every commit-phase lock retained across callbacks is covered by the
   equality-correct canonical plan, the checked unique-request plan, or an
   exact direct capability whose safe duplicate check or unsafe injectivity
   proof holds; every acquisition is bounded, every acquired prefix has one
   reverse release path, and every logical write remains exclusively protected
   through installation;
3. final validation, or an explicitly certified exact-token acquisition, sees
   either the observed version, that version locked by self, or failure;
4. installation cannot fail or panic;
5. published value representations and versions obey the memory ordering in
   Section 9;
6. item identities are stable and equality-correct;
7. nontransactional operations participate in the same protocol; and
8. an ordinary general-table scan holds shared structural admission from its
   directory-generation sample through its final native chunk, and every
   admitted interning attempt holds exclusive admission and advances that
   generation before native publication can become visible; and
9. a trusted direct-table scan retains one scan-generation item, reads each row
   through a valid OCC sandwich under the same structural admission, and every
   admitted interning attempt or committed record publication advances that
   scan generation.

### 16.2 Writing transactions

All write resources are locked before core clock reservation and final
validation. Define the certification cut immediately before the first final
read check. Since covering versions do not wrap or revert, any dependency that
changed before the cut remains mismatched or locked when checked. Success of the
whole sequential pass therefore proves retrospectively that every read and
predicate described shared state at the cut, except for resources locked by the
same transaction at their observed pre-lock version.

The homogeneous direct plan changes representation, not this argument. Its
capability emits exactly one held lock for every write and none for a read, and
its default final callback validates every item against the exact optional
guard. Under the unsafe write-at-acquisition option, the write token binds the
execution observation to the exact physical lock; acquisition proves that
observation current before returning the guard. The retained guard excludes a
change through the later certification cut and install, so the final pass may
skip that certified write while it still validates every read-only record and
scan- or directory-generation observation. Safe capability forms reject
duplicate physical identities; unsafe injective forms assume the reviewed
one-to-one token proof. All direct locks remain held through every install and
are released only after the complete write set has been published. The same
certification cut and partial-publication exclusion therefore apply without a
general `Prepared` vector or `LockIdentity` plan.

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

An ordinary nontrivial scan samples the physical-directory generation while
holding the table's shared structural guard, keeps that guard through every copied chunk,
and finally validates the sample as an unordered prepared-free read. Every
structurally admitted interning attempt holds the exclusive guard and advances
the generation before it calls native `get_or_insert`. Therefore an attempt is
either excluded from the traversal or invalidates the scan; native publication
cannot appear in the interval between the scan's sample and that advance.
Advancing for losing, failed, or subsequently aborted attempts adds only safe
false conflicts.

The scan also observes every physical record traversed through its logical
limit, including tombstones. Existing-record value changes, removal, and
resurrection therefore encounter a held or advanced record generation at final
validation. The same argument applies to forward/reverse direction and exact
inclusive/exclusive bounds: an unobserved existing record lies outside the
logical prefix that can affect the result. Scan overlay supplies read-your-
writes. Together with Assumption 8, these witnesses prevent a committed scan
from omitting, duplicating, or accepting a phantom. They remain conservative
because an unrelated physical interning anywhere in the table invalidates the
scan, and a scan followed by its own first miss self-conflicts.

A trusted direct-table scan substitutes one table-wide value-generation
observation for the per-row STO items. Its shared structural guard and eager
generation advance retain the same no-traversal-gap proof for new directory
entries. Each row's OCC sandwich prevents a torn or lock-covered value from
being exposed during execution. Every committed value, liveness, or directory
publication advances the scan generation, so the operation's final generation
check and core's certification-time recheck reject any change that could alter
the returned range. A transaction with prior record state takes the ordinary
overlay path; a later existing-record write is installed after certification,
while a later first miss changes the generation and self-conflicts. Assumption
9 therefore provides phantom and existing-value coverage without retaining a
record item per row. It is more conservative than the ordinary path because
any committed record publication in the table invalidates it.

### 16.6 What this argument does not prove

It does not prove fairness, bounded retries, crash durability, distributed
atomicity, MVCC visibility, or correct behavior if a C++ or nontransactional
path bypasses the Rust record/directory-generation protocol.

## 17. Mako and upper-layer integration

### 17.1 Reference backend

The upper branch at `1daec550f` remains the behavioral oracle:

```text
safe Rust mako-local -> mako_local_* C ABI -> C++ STO/MassTrans/Masstree
```

The architectural native-backend target bypasses that STO C ABI and calls
`sto-core` directly. Both target backends implement a backend-neutral upper
transaction interface during rollout.

The current C++ TPC-C comparison uses a narrower transitional bridge:

```text
C++ abstract_db / rust_sto_tpcc_wrapper
                  |
             sto-tpcc-ffi
                  |
       sto-core -> sto-masstree -> masstree -> hidden/public native ABI
```

`sto-tpcc-ffi` owns Rust runtime, worker, table, transaction, status, and panic
containment for this closed integration. It is not a stable generic ABI for
`sto-core` and does not expose the adapter trait hierarchy to arbitrary C
callers. Its checked public calls require live handles and readable/writable,
properly aligned, non-aliasing ranges for the full call. Within those caller
safety preconditions they validate nullness, lengths and address overflow,
owner-thread affinity, active transaction state, table ownership, enum values,
callback presence, and output capacities.

The paired C++ wrapper may use wrapper-private trusted transaction-lifecycle,
scalar point-operation, and scan calls that are absent from the installed
header. Its object graph and transaction state establish the live-handle and
affinity invariants; request construction supplies the documented range,
enum, callback, and unique-output preconditions. Capability and table
ownership are construction invariants with debug assertions, not release-mode
dynamic validation, and raw pointer liveness/readability remains an unsafe C++
caller obligation. Direct application or foreign use violates the contract
and may be undefined behavior. The private functions retain the Rust panic
boundary and preserve semantic outcomes plus conflict, capacity, and fatal
status classification; they remove only checks covered by those invariants.

#### Private fixed-layout TPC-C capabilities

[`tpcc_fixed_batch.h`](../../src/mako/benchmarks/tpcc_fixed_batch.h) defines
four optional C++ workload interfaces:

```text
TxnTpccPaymentCapability
  payment_full_enabled
  tx_payment_prefix
  tx_payment_full

TxnTpccNewOrderCapability
  tx_new_order_full

TxnTpccDeliveryCapability
  tx_delivery_full

TxnTpccStockLevelCapability
  tx_stock_level_full
```

`abstract_db` has default accessors that return null. The Rust TPC-C wrapper
inherits these interfaces and returns itself only for enabled capabilities.
The benchmark therefore keeps its existing scalar transaction as the semantic
fallback. These classes are workload dispatch, not the STO adapter hierarchy.
They neither replace `TransactionalResource` nor expose Rust transaction items
to C++.

The corresponding Rust entry points are hidden static-link symbols and are
absent from the installed `sto_tpcc_ffi.h`. They use fixed-layout request and
result records with matching Rust and C++ offset assertions. The current
eligibility rules are deliberately narrow:

| Capability | Required benchmark mode |
| --- | --- |
| Payment prefix and full Payment | Fixed Masstree keys, control mode 0, every row local, and no remote transaction path. |
| Full NewOrder | The Payment locality rules, the nontransactional fast order-ID generator, 5 through 15 lines, and every supplier mapped to the exact home warehouse and stock table. |
| Full Delivery | Fixed Masstree keys, control mode 0, and local `new_order`, `oorder`, `order_line`, and `customer` tables. |
| StockLevel tail | Fixed Masstree keys, control mode 0, and local `order_line` and `stock` tables. The scalar prefix must already have read the district row and selected the current next-order ID. |

`MAKO_STO_TPCC_DISABLE_PAYMENT_PREFIX`,
`MAKO_STO_TPCC_DISABLE_PAYMENT_FULL`,
`MAKO_STO_TPCC_DISABLE_NEW_ORDER_FULL`,
`MAKO_STO_TPCC_DISABLE_DELIVERY_FULL`, and
`MAKO_STO_TPCC_DISABLE_STOCK_LEVEL_FULL` allow controlled fallback
comparisons. Disabling full Payment retains the fused prefix when the Payment
capability itself remains enabled. Failure-injection modes, remote rows,
transactional district order-ID allocation, and ineligible layouts stay on the
scalar C++ path. Disabling the StockLevel capability keeps the complete scalar
scan and stock-probe tail after the same district prefix.

The fused operations preserve these benchmark transactions:

| Call | Work performed inside Rust |
| --- | --- |
| `payment_prefix` | Update warehouse YTD, update district YTD, optionally scan at most 32 customer-name rows and select the lower median, then update the selected customer. The transaction remains active after success. |
| `payment_full` | Perform the prefix, insert the exact encoded history row with duplicate as a no-op, update the separate customer-data row for bad credit, and commit. |
| `new_order_full` | Batch-read item prices, batch-update exact-home stock rows, read the customer, warehouse, and district rows, insert the `new_order`, `oorder`, and customer-order-index headers, insert 5 through 15 order lines, and commit. The caller supplies the already-consumed fast order ID and timestamp. |
| `delivery_full` | For districts 1 through 10, select the first new-order row at or after the worker cursor, read and update its order, scan and update at most 15 order lines, remove the new-order row, add the line total to the customer balance row, then commit the complete multi-district attempt. |
| `stock_level_full` | Continue the active transaction after C++ has read the district row and selected its current next-order ID. Scan the preceding 20 orders, up to 300 order-line rows, deduplicate their item IDs, read each distinct local stock row, count quantities strictly below the threshold, and commit. |

These functions build ordinary `sto-masstree` observations and intents. They
do not implement a separate transaction algorithm. Because the bridge creates
private direct tables and the live items use one adapter type and one static
capability, eligible writes take the sealed homogeneous direct-commit plan in
`sto-core`. Exact-token acquisition certifies each write. Read-only record and
scan-generation items still run final validation before installation.

Every commit-owning call owns the active attempt through commit or guarded abort
and resolves it before returning. It writes its result only after a successful
commit. `RETRY` maps to the benchmark's abort exception. Each lane preserves
its documented missing-row and malformed-row status, while invalid private-ABI
inputs, adapter faults, or contained panics map to a fatal error. The C++
wrapper marks the transaction inactive after every commit-owning call returns,
so no caller can commit or reuse a partially executed attempt.

Delivery's ten `last_no_o_ids` cursors are intentionally outside STO. A cursor
advances to one past the selected order before later reads or writes for that
district. That change survives a later conflict or fatal abort, matching the
scalar worker. An empty district leaves its cursor unchanged. Full NewOrder
likewise consumes its nontransactional fast order ID and timestamp before the
Rust call; an abort may leave an order-ID gap but publishes no database row.

The StockLevel split is intentional. C++ retains the district get and chooses
the current next-order ID from either the fast holder or the decoded district
row. Rust receives that ID, scans the half-open order range
`[max(current - 20, 0), current)`, decodes the leading item ID from at most 300
order-line values, deduplicates in fixed-capacity storage, and reads the native
`i16` quantity from each distinct stock row. Its commit validates the earlier
district observation together with the Rust tail. A failure leaves the caller's
result unchanged and resolves the active attempt.

Full NewOrder needs the customer, warehouse, and district rows only as presence
and OCC witnesses when the fast order-ID generator is active. It uses
`contains_resolving` on a cache miss and `contains_resolved` on a cache hit, so
it does not load or decode those three payloads. This preserves release-build
database behavior for well-formed TPC-C data while retaining missing-row and
commit-conflict detection. It deliberately does not reproduce the scalar
decoder or `CHECK_INVARIANTS` corruption checks for those unused values.
Eligibility therefore assumes valid benchmark encodings; malformed-row
diagnostics are outside parity for this optimized lane.

On the supported 64-bit target, the raw wrapper-private ABI request and result
sizes are:

| Call | Request bytes | Result bytes | Additional caller storage |
| --- | ---: | ---: | --- |
| Payment prefix | 120 | 32 | Three distinct 164-byte replacement buffers. |
| Full Payment | 112 | 16 | None retained across the return. |
| Full NewOrder | 112 | 8 | Readable item-ID and quantity arrays with `line_count` elements. |
| Full Delivery | 56 | 16 | One writable, ten-element district cursor array. |
| StockLevel tail | 40 | 24 | None retained across the return. |

The C++ capability records in `tpcc_fixed_batch.h` are dispatch types, not the
raw ABI records in this table. In particular,
`tpcc_fixed_batch::stock_level_full_request` is 48 bytes and includes `txn` as
its first field. The wrapper validates that handle, then translates the request
to the 40-byte raw record because the Rust thread handle already owns the active
transaction.

Every request, result, handle, input array, and mutable output range obeys the
private boundary's complete-call liveness, alignment, ownership, and
non-aliasing rules. All table handles belong to the active wrapper database and
the same Rust runtime. Fixed encoded keys are 4 bytes for warehouse, 8 for
district, 12 for customer, 16 for order line, and 40 for customer-name bounds
where applicable.

The Payment prefix has the one longer lifetime contract. Its three output
buffers remain at fixed addresses, readable, and immutable until the caller
later commits or aborts the still-active transaction. Rows through 160 bytes
use private borrowed intents and copy into stable atomic cells during install.
Valid 161 through 164 byte rows use owned shared staging. The Rust boundary
writes the three lengths and selected customer ID only after the complete
prefix succeeds. Any reported failure or contained panic aborts the attempt and
clears its borrowed intents.

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
- compile-fail checks that neither terminal typestate exposes `with_item`, plus
  runtime tests for the consuming open-to-ready transition and structurally
  worker-affine terminal handle;
- terminal-batch tests proving validation-only commit, reverse drop-only
  cleanup, capability refusal, explicit abort, preparation/operation error and
  unwind containment, definite validation conflict/fault classification,
  post-certification destructor poisoning with a definite committed outcome,
  and safe pooled-resource rebind/drop behavior;
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

The optional fixed-`u64` profile additionally requires:

- pinned 16-byte/eight-byte-aligned hot-record layout and cold-sidecar
  accounting;
- permanent loader sealing, checked bounds, private native-tree construction,
  publication outcome handling, and fail-closed alias detection;
- version-sandwiched atomic snapshots, including retry after an unlocked
  generation change and conflict on a held writer lock;
- stale-writer validation, unchanged-value no-op behavior, duplicate and miss
  no-callback outcomes, terminal reads, and exact OCC update publication; and
- a feature-enabled native loader/update/read round trip through the real C ABI.

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

The reusable, implementation-independent checker lives in
[`mako-history`](../../crates/mako-history). It validates complete call
intervals, operation/result type agreement, exact binary values returned by
point mutations, ordered scan rows, and the independently observed final
state. It then searches serial orders of committed transactions while enforcing
response-before-begin real-time edges. A bounded search is reported as
inconclusive when its node budget is exhausted, never as success. Checker
fixtures accept legal read-your-writes histories and reject write-skew,
phantom, stale-read, malformed-interval, and corrupted-prior-value histories.
Replay diagnostics encode all keys and values in hexadecimal.

The original pure-Rust reference suite in
[`strict_serializability.rs`](../../crates/sto-test-datatypes/tests/strict_serializability.rs)
still runs 128 seeded workloads with three workers and two transactions per
worker. Deterministic litmus tests in
[`isolation_litmus.rs`](../../crates/sto-test-datatypes/tests/isolation_litmus.rs)
also cover write skew, dirty-read prevention with explicit abort, and a
fractured read across two adapter types.

The transactional Masstree suite in
[`history_tests.rs`](../../crates/sto-masstree/src/history_tests.rs) applies the
independent checker to both registry-ID and direct-token memory tables. Each
mode runs the same 128 by three by two seeded schedule over `get`, `put`,
conditional insert, remove, resurrection, and bounded forward and reverse
scans. The key and value corpus includes empty, NUL-containing, and high-byte
data. Exact full scans establish each final state. Committed-only coverage,
commit-count, and overlapping-interval thresholds prevent an all-abort or
effectively sequential run from passing. A deterministic overlapping history
also requires a committed reader to observe a committed writer's value and
requires the oracle witness to order the writer before the reader. A separate
transaction commits and aborts a Masstree update together with a `TxnVec`
update to pin cross-adapter atomicity.

The opt-in
[`history_oracle.rs`](../../crates/sto-masstree/tests/history_oracle.rs)
integration test runs an analogous 128-seed schedule through the public
`sto-masstree` API and the real Masstree C ABI in both table modes. Three
persistent worker threads are reused across all seeds in each mode because
native worker registrations have process lifetime. Every seed must contain an
overlapping committed writer-to-reader dependency, and the oracle witness must
order that writer before the reader. Every operation category must also occur
in the committed projection. The
`rust_masstree_native_integration` CMake target runs this test in its own
process after the lower Masstree and transactional-adapter suites.

Aborted observations are excluded from these serial-order searches because the
implemented Serializable profile is nonopaque. Running one identical corpus
through direct C++ MassTrans, raw `mako_local_*`, and the upper Rust wrapper is
still required to complete the five-backend differential ladder above.

### 18.5 Performance ladder

Measure each boundary separately:

```text
raw C++ Masstree -> C ABI -> safe Rust Masstree
direct C++ MassTrans -> native Rust STO/Masstree
```

Track throughput, p50/p99 latency, abort rate, allocations, item bytes,
lock-hold time, scan chunk cost, and false conflicts from the coarse physical-
directory generation. Performance changes never weaken a stated correctness
gate.

The second line of the ladder now has a reproducible development comparison on
`zoo-002`; the first line remains to be characterized separately. The workload
uses 100,000 prepopulated eight-byte keys and values, ten unique point
operations per transaction, 0%, 5%, or 50% writes, and 1–64 physical cores.
Each cell has three adjacent C++/Rust pairs, with a one-second warmup and
five-second measurement. Within each shuffled repetition, the first engine
alternates by schedule position; that does not guarantee a balanced first
engine for every individual cell. `Rust/C++` below is the median same-seed
paired throughput ratio:

| Threads | Ten reads | 5% writes | 50% writes |
| ---: | ---: | ---: | ---: |
| 1 | 79.85% | 78.02% | 78.58% |
| 2 | 80.29% | 78.18% | 78.15% |
| 4 | 80.25% | 78.19% | 77.91% |
| 8 | 83.42% | 78.33% | 74.86% |
| 16 | 84.14% | 85.88% | 80.32% |
| 32 | 87.12% | 104.72% | 86.54% |
| 64 | 110.55% | 140.99% | 106.26% |

The unweighted geometric mean across the 21 cells is 86.14%, with a range of
74.86–140.99%. Rust remains about 20–25% behind at low thread counts, approaches
or crosses C++ at 32 cores depending on the write mix, and is faster in each
64-core cell. This meets the development goal of roughly comparable
committed-operation throughput for this bounded point workload; it is not
uniform parity or a production-wide acceptance result. The raw samples, exact
builds, independent audit, and five-pair confirmation of the one flagged cell
are archived in
[`sto-rust-zoo2-optimized-2026-08-28`](../performance/sto-rust-zoo2-optimized-2026-08-28/README.md).

This is an end-to-end comparison of two integrations, not an isolated STO
measurement or a language benchmark. C++ stores a direct pointer to a typed
versioned-`u64` record in each Masstree leaf. Rust stores a `RecordId`, resolves
a general binary-value Rust registry, and runs the typed Rust item protocol;
the benchmark explicitly selects the bounded eager-contiguous registry rather
than the public lazy-segmented default. Conversely, Rust uses a fixed/fused
batch lookup, while each C++ write performs scalar `transGet` followed by a
`transPut` that traverses Masstree again. C++ has read-your-writes disabled, but
the unique transaction keys make that immaterial to abstract results.

The run counts committed logical operations and includes retry cost, so abort
rate is part of the result. It uses `taskset`, not per-worker pinning, on one
shared non-isolated host with three repetitions. Both effective timed paths use
native `-O2`; Rust uses fat LTO and one codegen unit while the C++ repository
target does not use LTO. Latency percentiles, scans, misses and interning,
non-eight-byte values, allocations, lock-hold time, false-conflict cost, and
application traces remain unmeasured.

### 18.6 Implementation conformance record

The experimental v1 implementation corresponding to this contract is split as
follows:

| Surface | Repository implementation | V1 state |
| --- | --- | --- |
| Typed STO protocol, private erasure, lock planning, direct exact-token write certification, and failure dispositions | [`crates/sto-core`](../../crates/sto-core) | Implemented for serializable non-opaque transactions. |
| Restricted terminal-read typestate, homogeneous pooled storage, and adapter capability | [`terminal_read.rs`](../../crates/sto-core/src/terminal_read.rs), [`adapter.rs`](../../crates/sto-core/src/adapter.rs), and [`transaction.rs`](../../crates/sto-core/src/transaction.rs) | Implemented with final certification, drop-only cleanup, compile-fail API separation, and contained failure dispositions. |
| Raw stable declarations | [`crates/mtree-sys`](../../crates/mtree-sys) | Implemented for ABI version 1. |
| Native C boundary | [`mtree_abi.h`](../../src/mako/storage/mtree_abi.h) and [`mtree_abi.cc`](../../src/mako/storage/mtree_abi.cc) | Implemented for scalar/scoped/strided point operations, worker-wide RCU retention, and copied bounded scans. |
| Safe runtime, worker, tree, point, and scan facade | [`crates/masstree`](../../crates/masstree) | Implemented; native cursors, pointers, and RCU guards remain private. |
| Transactional records, tiered atomic/shared values, tombstones, quotas, physical-directory generation, and scan overlay | [`crates/sto-masstree`](../../crates/sto-masstree) | Implemented with exact-token write acquisition, final read/generation validation, scan-only directory validation, per-record coverage of existing liveness changes, and seeded history checks in both registry-ID and direct-token modes. |
| Closed C++ TPC-C bridge, resolved-token cache policies, and fused workload capabilities | [`crates/sto-tpcc-ffi`](../../crates/sto-tpcc-ffi), [`rust_sto_tpcc_wrapper.cc`](../../src/mako/storage/rust_sto_tpcc_wrapper.cc), and [`tpcc_fixed_batch.h`](../../src/mako/benchmarks/tpcc_fixed_batch.h) | Implemented with checked public scalar operations plus wrapper-private fixed-layout Payment prefix, full Payment, exact-home NewOrder, local Delivery, and the local StockLevel tail. Commit-owning calls resolve their active attempt; ineligible modes retain the scalar path. |
| Optional all-present fixed-`u64` point specialization | [`fixed_u64.rs`](../../crates/sto-masstree/src/fixed_u64.rs) | Implemented behind `fixed-u64`: private fresh directory, permanent loader seal, 16-byte atomic record, terminal reads, and exact-unique point updates; liveness changes, scans, and miss fallback are intentionally unsupported. |
| Upper metadata reservation and pre-install coordination | [`hook.rs`](../../crates/sto-core/src/hook.rs) | Implemented as an optional caller-owned `CommitHook`. |
| Pure-Rust reference adapters and bounded isolation checks | [`crates/sto-test-datatypes`](../../crates/sto-test-datatypes) | Implemented for map, vector, and queue composition, deterministic isolation litmus tests, and model-checked strict-serializability histories. |
| Independent binary-safe transaction-history oracle | [`crates/mako-history`](../../crates/mako-history) | Implemented with exact interval and result validation, bounded strict-serializability and opacity search, final-state checking, negative fixtures, and hexadecimal replay diagnostics. |
| Opacity, graceful native shutdown, and upper backend cutover | Sections 12, 15.5, 17, and 19.2 | Deferred; callers receive explicit unsupported/capability outcomes rather than silent downgrade. |
| Bounded point-workload performance characterization | [`sto-rust-zoo2-optimized-2026-08-28`](../performance/sto-rust-zoo2-optimized-2026-08-28/README.md) | Complete on `zoo-002`; production-wide budget acceptance remains deferred. |

The branch-level validation record for this implementation includes the full
workspace suite in debug and release modes on Rust 1.95, strict Clippy and
rustdoc builds, C11 header compilation, the exact 44-symbol native allowlist,
required feature mask `0x3f7f`, export-manifest FNV-1a fingerprint
`0x8275e6faa88a4fe0`, the raw ABI suite, native safe-wrapper and
transactional-adapter integration. It also includes independent-oracle
self-tests and seeded in-memory and real-C-ABI Masstree histories in both
registry-ID and direct-token modes. The current feature-enabled suite also pins
the terminal API/typestate failure protocol and fixed-`u64` record layout,
loader seal, snapshot validation, stale-writer behavior, miss/duplicate
outcomes, public ownership surface, real-native load/update/read path, resolved
cache policy behavior, metadata-only resolved presence and final-validation
behavior, and native full Payment, NewOrder, Delivery, and StockLevel semantic
fixtures. The StockLevel fixture covers empty and clamped ranges, strict
threshold comparison, duplicate item IDs, the 300-row bound, missing-stock
retry, result immutability on failure, and post-failure handle reuse.
Earlier branch baselines included ASan, UBSan, and unsuppressed TSan stress, but
those sanitizer suites were not rerun against the exact scoped/strided ABI
performance commit and therefore are not a current cutover claim. The
production cutover record still needs explicit native fault injection for
allocation failure, ordinary C++ exceptions, and publication-unknown insertion,
plus the upper-backend differential histories, accepted production-wide
throughput, latency, and false-conflict budgets, the exact sanitizer reruns, and
the deferred capabilities in the table above.

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
5. **Scans and predicates.** Add copied bounded scans, pre-publication physical-
   directory generation validation, per-record liveness coverage, and the
   staged-write overlay required by v1.
6. **Upper integration and nonopaque cutover.** Implement the backend-neutral
   facade and differential corpus while retaining C++ as the reference backend;
   shadow or feature-flag production before changing the default.
7. **Optional opacity profile.** Add execution-time revalidation as a separate
   negotiated capability milestone after the nonopaque cutover gates pass.
8. **Optimize.** Profile and optimize item representation, ordering, batching,
   and predicate precision without weakening the selected profile's gates;
   continue against application and production-wide budgets.

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

The zoo-2 development comparison is evidence toward, but does not by itself
satisfy, the performance and false-conflict gate.

## 20. Decision record and change policy

### 20.1 Accepted v1 decisions

| ID | Source | Decision | Rationale |
| --- | --- | --- | --- |
| D1 | RUST | STO executes as native Rust; only Masstree remains behind the architectural C boundary. The current closed TPC-C integration also crosses `sto-tpcc-ffi`. | The target avoids a stable generic STO ABI while permitting the transitional C++ benchmark/backend wrapper. |
| D2 | STO/RUST | Items represent logical resources with stable identity. The general path uses owned IDs; a private direct table may use a stable registry-entry address token under an explicit unsafe capability. | Preserves type-aware conflict tracking while making the one reviewed address-identity exception visible. |
| D3 | RUST | Generic adapters use deferred writes in v1. | Makes abort and memory safety tractable. |
| D4 | RUST | Masstree stores immutable nonzero scalar tokens: a monotonic registry ID in the standard lane or an opaque stable entry address in a private direct table. | C++ only stores and copies the scalar; pointer reconstruction and dereference remain in the audited Rust owner. |
| D5 | RUST | Point misses intern tombstones; logical deletes retain them. | Gives stable absence witnesses without native leaf tokens. |
| D6 | RUST | Ordinary scans observe physical-directory generation plus every traversed record; opted-in trusted direct scans observe one table-wide scan generation and use per-row execution OCC sandwiches. | The ordinary path preserves precise existing-record coverage and RYW overlay; the trusted path trades table-wide false conflicts and a write RMW for compact scan state. |
| D7 | RUST | Published/publication-unknown records and all consumed IDs are not reclaimed or reused in v1; proven-unpublished candidates may be dropped. | Avoids premature cross-language reclamation without leaking proved losers. |
| D8 | RUST | Explicit thread-affine worker contexts replace core TLS. | Makes ownership and finite worker resources visible. |
| D9 | RUST | The default deduplicates and sorts canonical `LockIdentity` requests. A checked unique-request mode and an explicitly capable homogeneous direct plan may retain request order. | The general path prevents deadlock under aliasing; alternatives require bounded acquisition plus exact uniqueness or reviewed injectivity. |
| D10 | STO/COMPAT | Serializable nonopaque OCC lands first; opacity is explicit. | Matches controlled Silo usage without claiming silent opacity. |
| D11 | RUST | Installation and mandatory cleanup are infallible; finish is exact-once except for explicitly authorized core-owned drop-only paths. | Prevents reporting partial publication as abort while documenting terminal and committed-direct cleanup. |
| D12 | RUST | Native RCU remains behind ABI-owned one-shot regions, a tree-bound point-read scope, or a tree-independent worker RCU scope that safe Rust owns through RAII. | Generation-tagged owner cookies are opaque and scope-family checked, no dereferenceable native object escapes, structural admission remains operation-local for transaction-wide retention, and all retained state stays worker-affine under a synchronous caller contract. |
| D13 | COMPAT | The pre-install hook runs once after lock+validation and before install. | Preserves the upper branch's commit seam. |
| D14 | RUST | Distributed prepared state is outside core v1. | Network waiting while locks are held needs a separate liveness design. |
| D15 | RUST | Native version encoding is private; the C++ layout is a parity oracle. | Rust records do not exchange atomic objects with C++. |
| D16 | RUST/COMPAT | Native teardown requires negotiated `GRACEFUL_SHUTDOWN`; otherwise native allocations are process-lifetime. | Makes RCU/thread-affine destruction an explicit capability rather than a `Drop` guess. |
| D17 | RUST | External adapters implement safe typed base traits and private checked erasure. Compact direct tokens use the separate unsafe `DirectTokenLock` and unsafe injective constructors. | Preserves the paper's extension seam while isolating the stable-token memory proof. Compactness may omit the general identity vector, but acquisition still certifies an exact observation when the capability selects that option. |
| D18 | RUST | A terminal homogeneous read batch uses a distinct `Open -> Ready` consuming transaction typestate and an explicit stronger adapter capability. | The type surface proves that general item state, later operations, locks, installation, and outcome-dependent cleanup are unreachable, allowing a minimal key/observation representation without weakening certification. |
| D19 | RUST | Fixed-`u64` is an optional permanently sealed, all-present point profile over a fresh privately owned Masstree, not a mode of the general table. | The 16-byte hot record and omitted publication checks are justified only by exclusive directory ownership and immutable post-load liveness. |
| D20 | RUST | A homogeneous direct capability selects a sealed core plan with typed prepare, validate, install, release, and cleanup phases. Its unsafe exact-token option may certify writes at acquisition and skip only those writes in the final pass. | Removes general identity/prepared representation and a repeated write callback only when one stable capability proves the observation-to-lock binding and retains the guard; reads and scan generations still receive final validation. |
| D21 | RUST | Committed values through 38 bytes use inline atomics. Bounded-value tables use 192-byte entries and an adjacent stable atomic cell through 160 bytes. Values above the applicable atomic tier use owned staging and the embedded `ArcSwapOption`; standard 64-byte entries take that fallback above 38 bytes. | Makes the memory-versus-reference-publication tradeoff an immutable table policy while retaining arbitrary binary values. |
| D22 | RUST | Hidden `mako_mtree_*` and wrapper-private TPC-C lifecycle, scalar, scan, and fused workload lanes may omit checks already proved by their owning facade. | Keeps the optimized boundary closed while retaining structural admission, RCU, panic/exception containment, status semantics, and explicit unsafe lifetime contracts. |
| D23 | RUST/COMPAT | TPC-C uses private direct tables, unique-lock fallback, bounded values on customer/district/warehouse, and trusted scan generation only on its four range-scan tables. | Applies shared atomic costs only where measured workload shape can use them and records the closed integration proof. |
| D24 | RUST/COMPAT | The local fixed-layout Payment prefix is one callback-free private call using three caller-owned 164-byte buffers. Rows through 160 bytes use borrowed bounded staging; 161--164 byte rows use owned shared staging. | Preserves scalar operation order while making the same-database, local-table, nonaliasing, and transaction-lifetime obligations explicit at the closed boundary. |
| D25 | RUST | `ResolvedRecord` is a table-bound lookup capability, and the closed TPC-C bridge may retain it under a bounded per-table cache policy. | A cache hit skips Masstree traversal only. Exact table/key matching, transaction item reuse, OCC observation, and final certification remain mandatory. |
| D26 | RUST/COMPAT | Full local Payment, exact-home NewOrder, local Delivery, and the local StockLevel tail use hidden fixed-layout calls that own the active attempt through commit or abort and publish result metadata only after a successful commit. | Removes repeated C ABI crossings without adding a second transaction protocol or exposing workload-specific calls as a stable application ABI. StockLevel retains its district read and current-ID selection in C++ before handing off the scan-and-join tail. |
| D27 | COMPAT | Full NewOrder consumes its external fast order ID and timestamp before the Rust call. Full Delivery advances each worker cursor immediately after selecting a row, even if a later operation aborts. | Matches the benchmark's existing nontransactional state and retry semantics while keeping database writes atomic. |
| D28 | RUST/COMPAT | `contains_resolving` and `contains_resolved` provide metadata-only transactional presence with ordinary final OCC validation. Full NewOrder uses them for customer, warehouse, and district witnesses. | Avoids loading and decoding payloads that the valid-data release path does not consume. Missing rows, staged liveness, resolved-token ownership, and commit conflicts remain checked; corruption diagnostics for unused payload bytes are not a parity promise. |

### 20.2 Deferred decisions and review triggers

| Topic | V1 position | Revisit when |
| --- | --- | --- |
| Ordinary heterogeneous erased item lane | Typed unique batches and pooling are implemented; mixed external adapter items retain private erasure. | Profiling attributes material cost to the remaining heterogeneous allocation or dispatch. |
| Unsorted write locking | Sorted total order | Benchmarks show sorting material and an unsorted proof/test suite exists. |
| Precise range witnesses | Ordinary scans use table-wide physical-directory generation plus traversed-record observations; trusted direct scans use one table-wide value generation. | False conflicts from either strategy exceed an accepted workload budget. |
| Implicit, raw, or cross-worker RCU pin | Fixed-width strided calls, a tree-bound point scope, and a tree-independent worker RCU scope are RAII-owned in safe Rust; no raw guard or cross-worker pin is exposed. | A use case requires a broader lifetime model with enforceable ownership and progress rules. |
| Commutative multi-owner locks | Exclusive locks plus semantic intents | Counter/queue workloads justify the additional protocol. |
| Physical record GC | No reclamation | Growth is material and a grace-period design is proven. |
| Distributed `PreparedTransaction` | Not exposed | `sto-mako` specifies IDs, terms, idempotence, recovery, and liveness. |
| Native version bit allocation | Opaque `AtomicVersion` contract; legacy layout permitted but not exposed | Before the core implementation is performance-frozen. |
| Fixed-`u64` liveness changes, scans, or general miss fallback | Unsupported; use the general binary-value `Table` | A workload requires them and supplies a new conflict, publication, and representation proof without weakening the specialized sealed profile. |

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
