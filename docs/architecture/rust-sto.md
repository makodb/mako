# Rust STO (STO-rs): Type-Aware Transactions for Concurrent Data Structures

> **Updated:** 2026-11-23. Rust is now the default and sole transaction backend.
> The C++ transaction implementation and its FFI crates have been removed.
> Native Rust STO implements synchronous, strictly serializable **non-opaque**
> local transactions and a worker-affine retained participant. The native
> `sto-tpcc-ffi` coordinator integrates with existing SRPC, replication, and replay;
> the separate Rust cache uses `mako-engine-sto`. Native sharding and migration
> use the Rust storage host and retained admission/engine pins.
>
> **Still deferred:** core opacity, native Masstree root/worker reclamation,
> async or migrating transactions, and general-purpose MVCC. Distributed history
> selection belongs to `sto-masstree`; transport, replication and durability
> policy remain upper responsibilities. `sto-core` is not a durable engine.
>
> **Historical measurement:** the September 23 singleton engine comparison was
> within ±3% of C++ at 29/32 worker counts, with a 1.89% median Rust deficit.
> Those binaries predate the completed source migration. The migrated source
> has retained correctness/safety evidence but **no performance acceptance**;
> the Q11 campaign is closed and further qualification was explicitly stopped.

“STO-rs” here means the Rust STO stack, principally `sto-core`, `sto-masstree`,
and its integrations; there is no separate crate named `sto-rs`.

The main paper (§§1–6) follows Herman et al., [*Type-Aware Transactions for Faster
Concurrent Code*, EuroSys 2016][sto-paper]: **Introduction → Related work →
Design → Datatypes → Evaluation → Conclusions**. Design follows the paper's
own sequence: overview, platform, basic commit protocol, example, predicates,
opacity, and implementation. Evaluation keeps the paper's questions in order
but distinguishes our evidence from experiments performed only in the paper.
The detailed Rust contracts follow in appendices **in this same document**.
There is no separate paper/whitepaper copy to keep in sync: the narrative,
implementation reference, and evaluation evidence share this source.

The labels **[STO]**, **[RUST]**, **[COMPAT]**, and **[DEFERRED]** identify paper
inheritance, Rust redesign, existing API compatibility, and unimplemented
capabilities. `MUST`/`MUST NOT` express required invariants; they do not claim
that an explicitly deferred profile has been implemented. API blocks in the
appendices are signature excerpts with bodies omitted, not standalone programs. The complete
`TxnCell` example in §3.4 is executable.

The [C++-familiar Rust STO migration](../plans/rust-sto-cpp-shape.md) has
implemented its canonical vocabulary and source organization. The canonical
object/item modules are `object.rs` and `trans_item.rs`; there is one native
Rust implementation, not a compatibility wrapper or a second engine.
The retained Q11 record covers matched builds, 570 functional cases,
correctness/instrumentation gates, the public example, and bounded layout/code
reviews. These are recorded dirty-local results, not clean-commit or
production-default qualification.

The migration's full acceptance criteria are **not met**. Q11 closed after
baseline allocation quality-budget exhaustion, before any Q11 throughput
comparison. The user chose to retain the migration with performance unqualified;
allocation, retained-memory, abort, sustained-soak/recovery, and final C++-gap
acceptance remain unclaimed. Earlier measurements do not qualify the migrated
source. Neither shared-core transpilation nor C++ rusty-safety preparation is
part of this implemented native Rust stack.

## Contents

- [Abstract](#abstract)
- [1. Introduction](#1-introduction)
- [2. Related work](#2-related-work)
- [3. Design](#3-design)
  - [3.1 Overview](#31-overview)
  - [3.2 STO platform](#32-sto-platform)
  - [3.3 Basic commit protocol](#33-basic-commit-protocol)
  - [3.4 Example](#34-example)
  - [3.5 False conflicts and optimistic predicates](#35-false-conflicts-and-optimistic-predicates)
  - [3.6 Opacity](#36-opacity)
  - [3.7 Implementation and discussion](#37-implementation-and-discussion)
- [4. Datatypes](#4-datatypes)
- [5. Evaluation](#5-evaluation)
- [6. Conclusions](#6-conclusions)
- [Detailed implementation contracts](#detailed-implementation-contracts)
- [References](#references)

## Abstract

Conventional software transactional memory records accesses to memory words.
A datatype-aware system instead records the logical observations and updates
whose consistency matters to an operation. A map lookup should usually depend
on the requested record or an absence witness, not every pointer traversed on
the way to that record. STO coordinates these logical dependencies while
letting each datatype provide its own locking, validation, installation, and
cleanup operations.

Rust STO preserves this separation. `sto-core` owns transaction state,
heterogeneous item composition, certification, and failure handling. Datatype
adapters own the conflict model and payload representation. `sto-masstree`
keeps transactional records, versions, liveness, and values in Rust, while
using the native C++ Masstree only as a structural directory behind a narrow
C ABI. The upper cache drives this Rust engine through `mako-engine-api`.
The former C++ transaction backend has been removed.

The implementation supports point operations, read-your-writes, logical
deletes, conservative range validation, and optimized homogeneous commit and
terminal-read paths. It deliberately does not claim the paper's full datatype
library or opacity implementation. Its performance results show that a
concrete, carefully owned common path can substantially reduce the overhead
of a general transaction protocol without replacing that protocol or removing
its certification and failure boundaries.
In the retained September singleton engine experiment, the then-current Rust
build was within ±3% of C++ at 29 of 32 worker counts. This is historical,
workload-bounded evidence, not performance qualification of the migrated source
or production-default acceptance.

## Distributed storage adapters

The distributed runtime's client, participant service and scan path now use
`mako-storage-api`. Its `Database` creates thread-affine workers; participant
guards may release their Rust borrow while retaining the underlying STO frame
between protocol messages. Lock, validation, immutable logical intent and
installation are separate operations. This boundary contains no native storage,
RPC or Raft dependency.

`mako-storage-masstree` implements the interface with the existing Rust STO
Masstree adapter. The conversion from its local redo framing to owned logical
rows is inside that adapter. `Store::with_engine` accepts an explicit engine;
the existing default constructor selects Masstree. The migration host still
uses a checked Masstree bridge during the ongoing extraction.

Shared conformance scenarios exercise an in-memory model and Masstree
independently. They check private writes, ordered scans, absence and predicate
validation, write skew after the global lock barrier, retained prepares, and
tombstone replay. They are tests of the storage contract, not a proof of either
backend. The upper gate runs only the model; native tests remain separate.

The [pluggable engine plan](../plans/pluggable-distributed-engines.md) tracks
the remaining direct RocksDB adapter, independent Masstree checkpoint/log
recovery, durable migration and build qualification. The interface extraction
alone does not remove the current RocksDB row mirror or make these modes ready.

## 1. Introduction

**[STO]** The central cost of a word-based STM is not merely copying data. It
must track and later validate implementation details that the application
never observed. Rotating a search tree can invalidate a memory-word read set
even when the requested key and value have not changed. A datatype can choose
a smaller and more meaningful conflict unit.

Three responsibilities follow:

1. Define which parts of state an operation observes or changes.
2. Record those observations, predicates, and intended changes in the current
   transaction, including reads of the transaction's own staged writes.
3. Implement the callbacks that certify and publish those operations together
   with operations on other datatypes.

This is an extension mechanism, not automatic transactionalization of arbitrary
Rust code. A collection must first support memory-safe concurrent access. Its
adapter must then make a sound argument that its versions cover every relevant
change. Rust's type system helps with ownership and aliasing, but cannot prove
that a chosen conflict unit captures the semantics of a map or range query.

The current stack has three distinct integration boundaries:

| Surface | Current status | What it does not imply |
| --- | --- | --- |
| `sto-core` and reference adapters | Implemented native Rust transaction protocol | A complete port of the paper's datatype library |
| `sto-masstree` over `masstree`/`mtree-sys` | Implemented transactional point and range operations | A Rust rewrite of the native index or native-state reclamation |
| `mako-engine-sto` through `mako-engine` into `mako-cache` | Default and sole transaction backend | Performance acceptance of every cache workload |
| `sto-tpcc-ffi` and `rust_sto_tpcc_wrapper` | Native local/distributed coordinator, retained participants, replication/replay integration | Production qualification, arbitrary remote range scans, or a generated C++ STO backend |
| Generic C++ Rocks-compatible `mako::DB` | Implemented over the Rust transaction adapter | Restartable native runtime or durable foreground writes |

The local OCC contract is strict serializability: committed transactions admit
one legal serial history respecting real-time precedence. Active or aborted
transactions are not promised a coherent global snapshot. Mako's distributed
integration additionally uses speculative, term-aware history visibility and
replication authority; it is not a claim of core opacity or crash durability.
Conflict, resource exhaustion, definite committed cleanup failure, and uncertain
publication must not be collapsed into one retryable Boolean.

The scope and exact outcome contract are retained in
[the scope/semantics reference](#a2-semantic-contract). The owner-authorized default switch is recorded in the
[retirement handover](../plans/sharding-on-rust-engine-handover.md);
[rollout gates](#o2-cutover-gates) retain the operational qualification limits.

## 2. Related work

The paper positions STO among several different ways of reducing transaction
cost. These distinctions also explain the Rust design; they are not new
comparative performance claims for this repository.

| Approach | Tracking and synchronization | Relation to Rust STO |
| --- | --- | --- |
| Word-based STM, including TL2 | Track memory locations and validate their versions | STO tracks semantic items instead. The Rust stack has no general `TGeneric` word-STM fallback. |
| Object-copying STM / MVCC | Preserve shadow objects or multiple versions | Owned snapshots are used where Rust memory safety requires them, but that is not a general MVCC implementation. |
| Transactional boosting | Acquire abstract locks around concurrent operations and undo eager changes | The paper can express this strategy. Rust's general adapter contract currently standardizes deferred OCC, not a ready-made boosting library. |
| Predication / semantic versioning | Track logical conditions rather than exact values | Rust exposes predicate upgrade and final validation; datatype-specific proofs still determine the conflict graph. |
| Silo-style database OCC | Specialize records and commit for one database | The retired C++ MassTrans lineage supplies historical compatibility/performance evidence; the Rust core separates generic certification from database policy. |
| Hardware TM | Execute suitably bounded operations speculatively in hardware | Orthogonal to the interface; no HTM capability is claimed here. |

The historical C++ seam comprised `TObject`, `TransItem`, and
`Transaction::try_commit` under `src/mako/sto/` at revision `0d26e2e0b`.
That implementation also carried logging, RPC, replication timestamps,
and MassTrans-specific policy. Those concerns are not requirements of the STO
abstraction and do not belong in `sto-core`. The
[Masstree book](../masstree-book.md) describes that native lineage; the
[cache integration record](../plans/mako-cache-over-rust-sto.md) describes the
separate upper-engine transition.

## 3. Design

### 3.1 Overview

Application code obtains a worker from a `Runtime`, begins a transaction, and
invokes ordinary datatype methods with a mutable transaction reference. The
transaction borrows its worker and is `!Send`/`!Sync`: it cannot migrate to
another thread or outlive the worker. Commit and abort consume the active
transaction. Dropping an active transaction aborts it under the documented
unwind contract.

Unlike the paper's `TRANSACTION ... RETRY(...)` syntax, the core does not hide a
retry loop or look up the transaction in thread-local state. The caller receives
an explicit outcome and decides whether another attempt is appropriate. Upper
facades may pool thread-affine worker state; that is distinct from the core's
explicit ownership API. The retained participant permits synchronous network
lifecycle calls between phases on its owning worker, not async suspension or
thread migration; datatype callbacks themselves remain bounded and nonblocking.

Read-your-writes is part of the datatype contract. A read first consults its
logical item's pending intent. Repeated writes compose in that same item;
insert/delete/reinsert must not accidentally create independent states for
one logical key. Outside effects such as I/O, application counters, or random
ID allocation are not rolled back by abort.

### 3.2 STO platform

The platform divides responsibility as follows:

```text
application / mako-cache
          |
      mako-engine -- common contract in mako-engine-api
          |
      mako-engine-sto
          |             \
       sto-core       sto-masstree
                               |
                            masstree
                               |
                            mtree-sys
                               |
                  mt_* / hidden mako_mtree_*
                               |
                     C++ Masstree directory

C++ TPC-C/dbtest -> rust_sto_tpcc_wrapper -> sto-tpcc-ffi coordinator
                           |                       |
                 existing SRPC/replication    sto-core Participant
                           |                       |
                 backend-routed replay       sto-masstree history
                                                   |
                                         mako-clock::distributed
```

The Rust engine implements the upper interface. The native directory stores
immutable nonzero scalar tokens; the Rust adapter interprets them as registry
IDs or, for a private direct table, certified opaque record-address tokens.
The public ABI does not expose a native cursor or dereferenceable node pointer.

A transaction's tracking set identifies an item by its registered resource
binding and typed logical key, within one runtime. The same identity yields the
same item. Different adapter types and bindings can coexist in one transaction.
An item carries an observation, an optional predicate, an intent, adapter-local
state, and (on the ordinary path) prepared commit state. A predicate becomes a
covering observation during commit rather than being treated as an arbitrary
late Boolean check.

| Paper concept | Rust counterpart | Responsibility |
| --- | --- | --- |
| `TObject` callbacks | Rust `TObject` with private checked erasure | Define the semantic conflict unit and datatype operations |
| `TItem(owner, key)` | Typed resource binding, key, and core-owned item state | Deduplicate accesses and compose observations/intents |
| Read/version slot | Associated `Observation` | Preserve a sound snapshot and covering version |
| Write slot | Associated `Intent` | Own or safely retain the proposed modification |
| Predicate slot | Associated `Predicate` and upgrade callback | Convert a semantic condition into final-certification coverage |
| Datatype scratch | Associated `Local` / `Prepared` | Hold operation state and preallocate commit resources |
| Version/lock helper | `AtomicVersion`, `TransactionLock`, owned guards | Exclude conflicting installs and publish a new generation |

The source map follows those responsibilities:

| Responsibility | Authoritative Rust source |
| --- | --- |
| `TObject`, associated payload types, phase item views, and read capabilities | [`object.rs`](../../crates/sto-core/src/object.rs) |
| `TransProxy`, `TransItem`, `TransItemData`, typed batches, and private checked erasure | [`trans_item.rs`](../../crates/sto-core/src/trans_item.rs) |
| Explicit item lookup, transaction coordination, and outcome handling | [`transaction.rs`](../../crates/sto-core/src/transaction.rs) |

These are source modules, not a requirement to import their private internals.
External adapters use the public `sto_core` types; the core retains ownership of
item states, guards, phase transitions, and failure containment.

Safe adapter traits are trusted for **transactional correctness**, not an
unchecked memory-safety license. An adapter that lies about a conflict may
violate isolation, but ordinary private erasure still performs checked type
transitions. The compact `DirectTokenLock` seam is separately unsafe because
it requires a stable address, retained ownership, token validation, and an
injectivity proof. See the [adapter reference](#appendix-d-rust-adapter-interface-and-protocol)
for the full signatures and obligations.

### 3.3 Basic commit protocol

The paper's basic protocol locks writes, validates observations, installs
updates, and cleans up; predicates add an upgrade step before final validation.
Rust retains that structure but makes resource preparation, failure outcomes,
and upper-layer publication explicit:

1. **Datatype locking (`TObject::lock`):** prepare the current intent and call
   `LockContext::try_lock` or `try_unique_lock`. Acquisition happens synchronously
   in item/access order, not through a later sorted write set. The core retains
   guards and deduplicates canonical aliases. Failure reverse-releases the
   acquired prefix.
2. **Reserve optional upper metadata:** after writes are locked, the hook may
   reserve a ticket or enter the cache certification gate. Reservation is not
   commitment.
3. **Check predicates (`TObject::check_predicate`):** upgrade successful
   conditions to covering observations.
4. **Choose core metadata:** reserve an ordered `OccCommitId` only when a
   writing binding requires one, before final read checking. Record-local
   writers instead publish checked local successors.
5. **Check reads (`TObject::check`):** certify ordinary and upgraded reads.
   A matching payload alone is not a covering-version proof.
6. **Accept:** the optional pre-install hook may reject before irreversibility,
   under its no-visible-effect rejection contract.
7. **Install (`TObject::install`):** cross the irreversible boundary before any
   mutation. Local `TxnCell`/`TxnCounter` install may publish/unlock their
   exclusive lock through `InstallContext::try_publish_unlock`. Aliased locks
   defer until all protected items are installed; Masstree retains its locks.
   Post-install hooks run after all installs, before the final release pass,
   not necessarily before datatype-local publication.
8. **Final release and cleanup:** release remaining guards in reverse acquisition
   order without releasing an early-published guard twice; retain guard/target
   ownership through teardown. Only after successful release of all locks does
   `TObject::cleanup` receive a definite outcome.

For the ordinary path, the core's ordered ID is reserved **before** final
validation, not copied mechanically from the paper's clock placement. A local
record generation is not an upper Mako timestamp and is not an opacity bound.
Mixed-version transactions retain the ordered requirement if any writing
binding needs it. All version arithmetic is checked; no generation can wrap
back to an earlier observable value.

The irreversible boundary determines error handling:

| Failure point | Permitted interpretation |
| --- | --- |
| Conflict or ordinary capacity limit before installation | Definite abort; staged writes have no abstract effect |
| Broken adapter invariant before installation | Poisoned runtime with definite aborted outcome |
| Installation, post-install publication, or release fails after irreversibility | Indeterminate; do not report a normal abort or invent rollback |
| Cleanup fails after complete publication/release | Poisoned runtime with a definite committed outcome |

Panic containment applies only to unwinding builds and to the documented
boundaries. `panic=abort` terminates the process. When ownership is uncertain,
quarantining a frame or payload is safer than invoking speculative cleanup
through potentially invalid state.

Three optimizations keep the same logical obligations:

- A prepared-free read omits unused preparation, **not** its final check.
- A homogeneous direct plan has explicit eligibility and lock-injectivity
  requirements. Exact observed-version acquisition can certify that write
  while its guard remains held; ordinary reads still need final validation.
- A terminal homogeneous read batch has a consuming `Open -> Ready` typestate.
  It can only record the restricted reads, certify them, and tear down. It is
  not the paper's opacity-based “all reads are already certified” shortcut.

[The commit/failure reference](#appendix-f-commit-abort-and-failure-state-machine)
retains the full phase rules, capability hierarchy, and cleanup distinctions.

The authoritative shared driver is
[`ordinary_protocol.rs`](../../crates/sto-core/src/ordinary_protocol.rs).
[`Participant`](../../crates/sto-core/src/participant.rs) reuses that frame and
driver: incremental locking retains guards across calls, validation seals
execution, INSTALL adopts an external decision, and finish releases locks.
Unlike local cell/counter commit, retained INSTALL suppresses early unlock.
`CleanupItem`/`CleanupContext` still confer no lock capability. Thus Rust follows
C++ datatype-controlled publication without copying its unchecked lifetimes.

### 3.4 Example

**Naming convention.** Keep `TxnCell<T>` and `TxnCounter` for the Rust
datatypes, while retaining STO's protocol names `Transaction`, `TObject`,
`TransItem`, and `TransProxy`. `TxnCell` describes a shared transactional value
slot, not an exclusive owning pointer like Rust's `Box<T>`. Its `get`/`set`
interface is cell-like, but unlike `std::cell::Cell`, reads clone `T`, writes
are staged until commit, and cloned handles share the same synchronized
object. `TBox` and `TCounter` identify the C++ counterparts in comparisons;
they are not alternative Rust API names. Conceptual correspondence does not
imply identical ownership, publication, or datatype capabilities.

The paper introduces its protocol with `TBox`, one transactional value and one
version. The current Rust analogue is
[`sto_core::TxnCell<T>`](../../crates/sto-core/src/txn_cell.rs). The following
complete program depends on the in-tree `sto-core` crate:

```rust
use sto_core::{CommitOutcome, Runtime, RuntimeConfig, TxnCell};

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let runtime = Runtime::new(RuntimeConfig::default())?;
    let from = TxnCell::new(&runtime, 100_i64)?;
    let to = TxnCell::new(&runtime, 50_i64)?;
    let mut worker = runtime.attach()?;

    let mut txn = worker.begin()?;
    let debit = from.get(&mut txn)?;
    let credit = to.get(&mut txn)?;
    from.set(&mut txn, debit - 10)?;
    to.set(&mut txn, credit + 10)?;
    assert_eq!(from.get(&mut txn)?, 90); // Reads the staged write.
    if let CommitOutcome::Aborted(reason) = txn.commit()? {
        return Err(format!("transfer aborted: {reason:?}").into());
    }

    let mut check = worker.begin()?;
    let balances = (from.get(&mut check)?, to.get(&mut check)?);
    if let CommitOutcome::Aborted(reason) = check.commit()? {
        return Err(format!("verification aborted: {reason:?}").into());
    }
    assert_eq!(balances, (90, 60));
    println!("committed balances: from={}, to={}", balances.0, balances.1);
    Ok(())
}
```

`get` retains a versioned immutable snapshot on first shared access. `set`
stages an owned replacement; it does not modify committed state. The later
`get` in the same attempt reads that replacement. Commit locks both cells,
validates the observations, installs both replacements, and releases both
versions. A concurrent writer can make the attempt abort, but cannot make a
successfully committed transfer update only one cell.

This is deliberately a fixed-value example, not a financial API. Real account
code must specify overflow and insufficient-funds behavior. It must also handle
`CommitOutcome::Aborted` separately from exceptional `CommitFailure`: `?` alone
does not turn a normal abort into a commit. A poisoned-but-committed or
indeterminate attempt must never be blindly retried. Effects outside the cells
are not rolled back.

The example demonstrates a completed, nonconcurrent transfer and
read-your-writes. It does not establish concurrent opacity or the performance
of the Masstree adapter.

#### TxnCell and TBox: one replacement value

Read the Rust [`txn_cell.rs`](../../crates/sto-core/src/txn_cell.rs) beside
[`TBox.hh`](../../src/mako/sto/TBox.hh). They share one logical value/version
conflict unit, not a payload representation or an identical callback schedule.
The table follows one attempt that reads a cell, replaces it twice, and reads
its own last replacement before committing.

| Step | C++ `TBox<T, W>` | Rust `TxnCell<T>` |
| --- | --- | --- |
| Item lookup | `read` and `write` call `Sto::item(this, 0)` (`TBox.hh:18–34`), using the current transaction. | `get` and `set` call `txn.with_item(&self.resource, (), ...)`. A registered binding plus unit key identifies the one cell item; cloned handles keep that identity. The transaction is explicitly borrowed. |
| First shared read | With no pending write, `read` delegates snapshot/version handling to `W::read(item, vers_)`. | `CellAdapter::get` observes the version, loads an owned immutable `Arc<T>` through `ArcSwap`, and validates the version around that load. It calls `add_read` and retains `first_snapshot`. `get` returns a cloned `T`, not a borrowed shared payload. |
| Stage replacement | `write` uses `add_write` to retain the replacement in the item's write slot. | `set` creates an `Arc<T>` intent and calls `TransProxy::add_write`. A second `set` replaces the pending intent in the same item; neither call publishes shared state. A blind `set` need not record a read. |
| Read your writes | `read` checks `has_write` first and returns the staged write value. | `get` checks `intent` first, then the retained first snapshot. After two sets it returns the last replacement; an earlier shared-read observation still participates in commit checking. |
| Lock | `TBox::lock` calls `txn.try_lock(item, vers_)` (`TBox.hh:71–73`). | `CellAdapter::lock` synchronously calls `LockContext::try_lock` only for an intent. `CellPrepared` retains a typed use of the core-owned guard. |
| Predicate and read checking | `TBox` has no custom predicate callback. `check` invokes `item.check_version(vers_)` (`TBox.hh:74–76`). | `Predicate = NoPredicate`. `check` validates an ordinary read against the current version, or its observed pre-lock version under this transaction's checked own guard. A blind replacement has no prior value dependency to check. |
| Install | `install` moves the staged value into `v_`, then calls `txn.set_version_unlock(vers_, item)` (`TBox.hh:77–80`). | After irreversibility, `install` checks the guard and advancing OCC ID, swaps the replacement `Arc`, retains the displaced value, and calls `try_publish_unlock`. |
| Unlock/publication | Successful install already publishes/unlocks; `unlock` separately calls `vers_.unlock()` (`TBox.hh:81–83`). | An exclusive local lock publishes during install. Aliases and retained participants defer publication to final release. Core retains ownership and never releases twice; pre-install abort publishes no replacement. |
| Cleanup | `TBox` inherits the no-op `TObject::cleanup` (`Interface.hh:876–878`); transaction-owned item storage is a separate responsibility. | Post-unlock `cleanup` releases the retained first snapshot, displaced value, and remaining intent. This keeps destruction of a displaced user value out of installation under locks. Core-owned field teardown and failure containment still apply. |

The relevant Rust bodies are `TxnCell::{get,set}`, `CellAdapter::get`, and its
`TObject::{lock,check,install,cleanup}` implementation. The `Arc` snapshot
keeps a valid value alive even if this transaction will abort; it does **not**
make a multi-cell execution snapshot opaque. These are non-opaque transactions,
and no `TBox` wrapper choice or paper opacity guarantee is implied by the
analogy. If failure makes lock ownership or publication uncertain, the core
uses the quarantine rules in Appendix F rather than calling ordinary cleanup
as though release had succeeded.

#### TxnCounter and TCounter: one composed delta

Read [`txn_counter.rs`](../../crates/sto-core/src/txn_counter.rs) beside the
former C++ `src/mako/sto/TCounter.hh`. That file was removed from the tree; read
it with `git show fd3ed37e1:src/mako/sto/TCounter.hh`. The `TCounter.hh` line
citations below refer to that commit. Rust deliberately implements a smaller
counter: `get` and `increment`, with wrapping `i64` deltas. It does not
implement C++ counter assignment or interval predicates.

| Step | C++ `TCounter<T, W>` | Rust `TxnCounter` |
| --- | --- | --- |
| Item lookup | Conversion, assignment, comparisons, and arithmetic use `Sto::item(this, 0)`. | `get` and `increment` use the same registered binding/unit-key `with_item` pattern as the cell, with a separate counter adapter and one item per counter. |
| Stage a delta | `operator+=`/`-=` combine the write slot and mark `delta_bit` unless `assigned_bit` is set (`TCounter.hh:73–85`). | `increment(delta)` computes `old_intent.unwrap_or(0).wrapping_add(delta)` and calls `add_write`. It performs no shared read. Even `increment(0)` records an intent and takes the writing path; a zero or cancelling delta is not optimized into a read-only attempt. |
| Read your writes | Conversion obtains the base snapshot unless assigned, records the observed value in the predicate state, and adds any staged write (`TCounter.hh:23–32`). An assigned item can return the assigned value without a shared base. | `get` obtains a stable version/atomic-value pair once, records `add_read`, retains `first_base`, and returns `base.wrapping_add(delta)`. A delta staged before the first get is included. Later gets reuse the base and overlay the current composed delta. |
| Assignment and predicates | Assignment records a replacement and `assigned_bit` (`TCounter.hh:35–45`). Comparisons record integer constraints adjusted for the pending delta; `check_predicate` verifies a fresh `wait_snapshot` (`TCounter.hh:54–70,104–109,142–173`). | There is no assignment operation and `Predicate = NoPredicate`. Testing `counter.get(txn)? > 0` records an exact value/version dependency, not a weaker interval predicate. The generic `TObject` predicate protocol exists, but this counter does not opt into it. |
| Lock | `lock` calls `txn.try_lock(item, vers_)` (`TCounter.hh:101–103`). | An intent makes `lock` synchronously acquire the canonical exclusive `VersionLock`. Core owns the guard. Blind increments avoid an old-value dependency, not exclusive-lock contention. |
| Check | Predicate checking and `item.check_version(vers_)` cover the state recorded by C++ operations (`TCounter.hh:104–112`). | A get records a read checked against the version, including checked own-lock validation for a writer. A blind increment has no old-value observation to invalidate merely because another increment already committed. There is no predicate to upgrade. |
| Install | `install` adds the current value for a delta, or uses the assigned replacement, writes it, and calls `set_version_unlock` (`TCounter.hh:113–119`). | Under the core-held guard and irreversible permit, `install` applies the composed delta with `AtomicI64::fetch_add(..., Relaxed)`. It checks the guard/commit generation and, for an observed counter, consistency with the validated base. Version-lock publication supplies ordering; the atomic addition is not a substitute for transaction certification. |
| Unlock and cleanup | Successful install publishes/unlocks; `unlock` releases `vers_` (`TCounter.hh:120–122`). Cleanup is the inherited no-op. | Install calls `try_publish_unlock`; aliases and participants defer it. Post-unlock cleanup clears the retained base, installation marker, and delta. Abort before install discards work; uncertainty after install cannot imply rollback. |

For example, starting from 10, `increment(3)`, `get`, `increment(-2)`, `get`
returns 13 then 11 in one uncontended Rust attempt and stages a net delta of 1.
Those gets introduce a dependency on the original base. Without the gets,
another successfully committed increment before this attempt acquires its lock
does not by itself invalidate the blind delta: installation adds to the value
protected by the acquired guard. Lock contention can still abort either attempt.

Delta composition, read-your-writes arithmetic, and installed atomic addition
all wrap in `i64`; version generations, in contrast, must not wrap. The templated
C++ arithmetic is not a promise of identical signed-overflow semantics. Neither
the operation analogy nor atomic payload safety supplies opacity, predicate
counter support, or performance equivalence.

### 3.5 False conflicts and optimistic predicates

A datatype can reduce conflicts in two independent ways: choose a narrower
state segment, or record a weaker condition on that state.

For example, the implemented `TxnCounter::increment(delta)` can stage a delta
without observing the prior value. Two blind increments still serialize through
an exclusive commit lock, but a changed value alone need not invalidate either
intent. If the transaction also calls `get`, it has acquired a value dependency
and must validate it. The current counter uses wrapping `i64` arithmetic; it
is neither a checked financial counter nor a multi-owner commutative lock.

The paper's predicate example is stronger. If an integer counter first fails a
positive-value test, is increased by ten, and then passes that test, its initial
value need only be in `[-9, 0]`. Tracking that interval can permit unrelated
changes inside it. It is not enough to test the interval once at the end:
observations on another object could otherwise belong to a different state.
Commit must upgrade the predicate to covering version observations and then
include those observations in the common final certification pass.

Rust implements that protocol through `Predicate`, `check_predicate`, and
`check`, but the current reference datatypes use `NoPredicate`; the
interval example is an explanation of the extension point, not a shipped
predicate-counter implementation. Current direct-commit constructors and
terminal read batches also exclude predicate items.

Masstree absence and scans illustrate a different representation of semantic
conditions. A point miss observes a stable tombstone. An ordinary range scan
records a structural-generation observation plus the versions of all physical
records that can affect its logical prefix. These are ordinary read items,
not custom `Predicate` values. The resulting coverage is conservative: a
foreign physical insertion anywhere in the table can invalidate a range even
when it is outside the requested bounds. Details and the exact own-publication
repair rule are in [the scan contract](#j6-scans).

### 3.6 Opacity

**[STO]** The paper's opacity protocol protects transactions that will eventually
abort as well as those that commit. It uses an execution-time bound and
revalidation when new observations cannot be certified against that bound;
unordered observations require the conservative path. This can prevent an
application from acting on a combination of values that never existed together.

**[DEFERRED]** Rust STO does **not** implement that execution profile.
`Runtime::new` rejects an opaque default, beginning an opaque transaction is
unsupported, and the Rust cache rejects a requested opaque isolation level.
The `OpacityToken`, `ObservationOrder`, and execution-check seams preserve the
intended API distinction but are not evidence that whole-transaction
revalidation occurs on each read. Successful non-opaque transactions still
undergo final certification.

Memory safety is a separate obligation. Even a transaction destined to abort
must not race a non-atomic payload, follow a freed pointer, or expose an invalid
Rust value. Atomic payload words, immutable snapshots, owned leases, and native
RCU supply those guarantees; none by itself supplies a coherent multi-object
snapshot. A later version mismatch cannot retroactively legalize a data race.

[The opacity reference](#appendix-h-opacity) is explicitly a deferred protocol, not a
supported configuration or a read-only fast-path claim.

### 3.7 Implementation and discussion

The paper implements a compact, type-erased tracking set and aggressively
avoids per-operation allocation. Rust preserves heterogeneous composition but
uses typed associated state and checked private erasure instead of unchecked
payload extraction.

A worker separately pools a boxed ordinary transaction frame and terminal-read
scratch. Homogeneous ordinary items can remain in `TypedTransItemBatch<A>` with
intent and prepared sidecars; mixed adapters materialize the general
representation. Unique groups append without repeated hashing. A later scalar
access can reuse the exact last-access binding/key or append a fresh-binding
singleton without indexing; an uncached lookup within a live binding indexes
the missing suffix. Equality, not a hash alone, proves identity. Idle pools may
retain bindings, so the last public table handle need not be its last owner.

Scalar acquisition keeps `with_ordinary_item_inner_validated` out of line so
each typed dispatcher does not also embed the heterogeneous lookup and pooling
body. This adds a call boundary on the general path; it does not change exact
identity checks, payload ownership, or callback order. The helper is not marked
`cold`: mixed transactions may use it routinely. This code-size choice must be
qualified on both homogeneous and mixed workloads, not just singleton timing.

An eligible singleton crosses the batch dispatch and concrete plan entry once.
The compact-token body then uses static plan dispatch and borrows one exact
item and lock frame. The core still owns the boxed frame, phase state, acquired
guard, irreversible flag, and cold failure handlers. This is a specialization
of the common protocol, not a separately maintained commit algorithm. It does
not justify a general “zero allocations” claim: growth, fallback representation,
and datatype-owned snapshots may allocate.

Native `AtomicVersion` currently packs a nonzero 48-bit generation above a
16-bit `OwnerId + 1` tag; tag zero means unlocked. The default core worker limit
is 512, while the representation supports 65,535 owners. Native Masstree and
upper integrations impose their own smaller admission limits. Ordered and
record-local policies are captured per registered binding. An all-local writing
transaction has no ordered commit ID but still executes every required write
phase and hook. Fixed-width and trusted-scan combinations reject policies they
cannot support rather than silently changing semantics.

The upper cache's timestamp is not this OCC generation. Its general hook enters
a certification gate after acquiring write locks, then allocates HLC/order and
binds the log record **after successful local validation**, before installation.
Queue/holder publication is completed after the core terminal returns; the
upper engine's `post_install` hook is not the cache queue publication point.
The ordinary local TPC-C bridge uses post-install table-size publication;
retained participants publish size deltas after INSTALL and before finish.

Scratch ownership is similarly explicit. A worker lends a boxed `TouchLog` to
an upper transaction. An eligible first put can retain immutable log-owned
39–160-byte data instead of making another staged copy; later operations lose
that singleton witness. Overwrites retire the borrowed allocation rather than
mutating it. Proven cleanup permits reuse; uncertain cleanup pins the exact
allocation. General recycling applies an entry/byte cap, while the clean
singleton reuse path retains its buffers without that cap scan. The fused
success path explicitly recycles ownership before publishing ACK because its
caller intentionally forgets the consumed facade.

These choices move cost out of common paths only where a stronger contract is
available. Canonical fallback, exact binding checks, failure classification,
and lifetime retention remain essential parts of the implementation.

## 4. Datatypes

### 4.1 Methodology

The paper derives a transactional datatype from its abstract specification,
not from a list of memory accesses. Apply the same review sequence here:

1. **Specify results and composition.** Does insertion return a previous value,
   a Boolean, or nothing? Can two increments commute? What should a scan see
   after this transaction inserts and deletes keys?
2. **Choose logical conflict units.** A cell, array index, bucket, record, or
   range witness must cover every state change that could alter the result.
   Coarser coverage is safe but can create false conflicts.
3. **Make concurrent payload access sound.** Select immutable snapshots,
   atomics, or an owned reclamation guard before writing a version check.
4. **Separate execution from installation.** Record observations and compose
   deferred intents. Eager directory scaffolding must remain logically absent
   until the transaction installs it.
5. **Define certification and ownership.** Each write installs under its lock;
   publication respects alias and participant retention; every observation
   checks at the common cut and every token survives release/cleanup.
6. **Check all outcomes and outside mutators.** Conflict, capacity, unwind,
   partial publication, and nontransactional changes must obey the same model.
   An optimization's eligibility failure must not partially perform an operation
   before an ordinary fallback repeats it.

The full [adapter obligations](#d8-normative-adapter-obligations) adapt the
paper's six correctness rules. The [correctness argument](#appendix-l-correctness-argument)
then composes sound adapters; it is not a proof that arbitrary trait
implementations are correct.

### 4.2 List of types

The original paper includes `TBox`, `TCounter`, `TArray`, `TVector`, `TList`,
`TListSet`, `TQueue`, `TPriorityQueue`, `THashtable`, `TRBTree`, `TMasstree`, and
`TGeneric`. The current Rust inventory is smaller and its conflict granularity
must be stated independently:

| Rust datatype | Logical conflict unit and storage | Important limit |
| --- | --- | --- |
| `sto_core::TxnCell<T>` | One immutable snapshot and version per cell | Whole-cell conflicts; the TBox analogue |
| `sto_core::TxnArray<T>` | Independent snapshot/version/item per fixed index | Length is immutable; out-of-bounds is an abstract operation outcome |
| `sto_core::TxnCounter` | One atomic integer, composed deltas, exclusive commit ownership | Blind increments can omit reads; no predicate interval or multi-owner lock |
| `sto_test_datatypes::TxnHashMap<K,V>` | Fixed buckets, each an immutable map snapshot/version | Colliding unequal keys conflict; aggregates observe multiple buckets |
| `sto_test_datatypes::TxnVec<T>` | One whole-vector snapshot | Disjoint indices still conflict; not the paper's segmented TVector |
| `sto_test_datatypes::TxnQueue<T>` | One whole-queue snapshot | Head/tail operations are not independently synchronized |
| `sto_masstree::Table` | Per-record OCC plus structural/range witnesses | Append-only directory; logical tombstones; non-opaque |
| `sto_masstree::FixedU64Table` | Sealed, all-present 16-byte version/value records | Optional feature; no transactional membership changes or scans |

The first six are correctness-oriented reference adapters. They use ordinary
ordered versions and canonical locks, and do not advertise the optimized
Masstree direct/terminal capabilities. No performance equivalence with the
paper's optimized collections is implied.

#### Masstree points, absence, and ranges

A general table maps binary keys to immutable native scalar tokens. Rust owns
the corresponding value, liveness bit, and OCC state. A first point miss interns
a fully initialized tombstone before recording its version. A later activation
must therefore invalidate the absence observation. Aborting may leave that
physical key and its consumed registry slot, but not a committed logical value.
`remove` installs a tombstone rather than deleting a native key.

Ordinary scans retain structural admission across all copied native chunks,
observe directory generation, and observe every traversed record that can affect
the logical result, including tombstones. Staged values and liveness provide
read-your-writes. The limit counts live logical rows; stopping a visitor stops
expansion of that prefix. Earlier visitor effects are not undone by a later
error or abort.

A successful scalar own-publication updates an earlier directory/trusted-scan
observation by **exactly its own increment**. It does not reload the current
generation and erase evidence of another transaction's publication. Thus a scan
followed by an own scalar first miss need not self-conflict. General fixed-batch
miss paths do not currently use that repair and can conservatively self-conflict.
A hidden trusted direct-table scan can replace row items with one table-wide
value generation, but each exposed row still passes a sound OCC snapshot check.
Writing commits advance that generation once per transaction/table before their
first record-state publication; structural publications advance it independently.
This ordered-generation feature is not enabled by the record-local FFI.
Distributed versioned reads/scans additionally select coherent payload/metadata
pairs and retain tombstone dependencies under the policy in Appendix M.3.

The default structural admission is nonblocking. An explicit table option permits
blocking admission for execution-time first publication, not blocking while
holding commit locks. General tables can also permanently seal their directory:
existing records and tombstones remain usable, while genuinely new physical
keys are rejected. Sealed scans bypass structural reader admission but retain
RCU and STO certification. This is distinct from `FixedU64Table`'s loader seal,
which does not itself call the native tree-seal operation.

#### Payloads, allocation, and native lifetime

Bootstrap/local records use atomic words through 38 bytes and an embedded
ArcSwap-backed immutable allocation for larger values. The bounded-value option
adds an adjacent suffix and raises the atomic limit to 160 bytes. Distributed
metadata promotes the record into immutable history in that existing ArcSwap
slot. Readers reconstruct/select sound owned state and validate its covering
version; there is no racy `memcpy` followed by a hoped-for abort.

Lazy standard registries allocate contiguous 64-byte entries in segments.
Bounded-value lazy registries instead allocate 192-byte stable entries on the
claiming thread, behind per-slot ownership cells. This is allocation locality,
not thread-local ownership: the arena and detached lock targets retain those
cross-thread records. At full occupancy the 16-byte ownership cells add about
8.3% relative to the 192-byte row storage; partially occupied segments allocate
only claimed extended rows. Eager contiguous allocation remains a separate
explicit policy.

The registry budget accounts for its structural allocations, not total heap,
RSS, arbitrary payloads, or native Masstree memory. IDs never recycle, including
losing candidate IDs. Proven-unpublished candidates can release retained
record/key quotas without freeing their initialized slot. General
`Table::new_direct_with_budget` admits the registry before native tree creation;
`Table::new_direct` inherits that order with an unrestricted structural budget.
Supplied-tree and fixed-`u64` constructors do not provide this ordering guarantee.

Native traversal uses a C11 ABI and thread-affine RCU scopes. Native tree/root
allocations and worker registrations remain process-lifetime; closing a facade
is not reclamation. A fixed worker pool and operational memory limits are
therefore part of deployment, not optional tuning. The
[Masstree adapter reference](#appendix-j-masstree-transactional-adapter) and
[native ABI reference](#appendix-k-masstree-c-abi) retain the detailed safety boundary.

## 5. Evaluation

This section follows the paper's evaluation questions, but does not pretend that
its STAMP, boosting, or Silo experiments have been reproduced in Rust. Separate
three forms of evidence: inspected implementation, executed correctness gates,
and measured performance. A checked-in test is not a current passing result;
a passing smoke is not a throughput claim.

### 5.1 Microbenchmarks

The retained pre-migration result is the [September 23 singleton attribution
record](../performance/mako-cache-rust-sto-singleton-attribution-2026-09-17.md#10-production-fixes-and-scaling-verification-2026-09-23).
It compares Rust and C++ for **existing-key, one-write engine transactions**,
256 disjoint keys per worker, 128-byte values, and checksum disabled. It does
not include insertion, log encoding, cache ACK, or sustained RocksDB writeback.

Both binaries use the matched campaign configuration: Rust 1.95,
`opt-level=3`, one codegen unit, `panic=abort`, explicitly enabled ThinLTO,
`target-cpu=znver1`, frame pointers, and debug information. **ThinLTO is a
campaign override**, not the workspace release default. The retained report
records native archive identity, image, binary hashes, CPU placement, and raw
runs. Workers use physical CPUs 32–63; helpers and writeback have separate
placements. Each worker performs 1,048,576 warmup and 4,194,304 measured
transactions, and all 256 keys per worker are checked afterward.

| Workers | C++ Mtxn/s | Rust Mtxn/s | Rust deficit |
| ---: | ---: | ---: | ---: |
| 1 | 1.882 | 1.831 | 2.71% |
| 8 | 14.471 | 14.272 | 1.38% |
| 16 | 28.933 | 28.224 | 2.45% |
| 32 | 57.395 | 55.964 | 2.49% |

Across 1–32 workers, 29 counts are within ±3%, and 30 are no more than 3%
behind C++. The median deficit is 1.89%. Rust is 3.18% behind at 12 workers and
3.11% behind at 13, and 3.17% ahead at 22. The last case fails a symmetric ±3%
band but is not a slowdown.

Noise handling is material to interpreting that result. The campaign retains
564 executions, of which 255 pass a symmetric, throughput-independent rule:
maximum SMT-sibling utilization and worker off-CPU time are each at most 1%.
There are at least three clean observations per backend/count; ordering is
balanced forward/reverse and repetitions target coverage rather than a desired
winner. The initial raw sweep has 23/32 counts within ±3%; all repeated raw
runs yield 28/32. These are median point estimates, not confidence bounds or a
statistical equivalence test. Allocation profiling was disabled, so zero-valued
allocation fields do not establish zero allocations.

The result supports this bounded singleton engine path. It does not discharge
all performance gates, and the retained multi-item, logged, and cache-ACK
smokes do not claim comparable parity.

### 5.2 Typed transactions: STAMP

The paper's STAMP evaluation asks whether exposing datatype semantics improves
whole applications rather than isolated primitives. There is no claimed Rust
STAMP port or reproduced STAMP score in this worktree.

The current repository instead has reference datatype composition, a shared
engine operation corpus, forced read/write, write/write and phantom histories,
and an independent bounded history oracle. These test different questions:
operation and terminal-result compatibility, conflict handling, and whether an
observed concurrent history admits the advertised isolation. They do not measure
STAMP application speed or prove every possible history correct.

[`mako-differential`](../../crates/mako-differential) supports direct C++, raw
C ABI, the safe Rust C ABI facade, and Rust STO operation transcripts. Its
forced-history checker remains a distinct artifact; this is not a universal
four- or five-engine concurrent lockstep harness. Required-driver settings in
the gate prevent a missing reference executable from masquerading as success.

### 5.3 Boosting

The paper compares optimistic semantic objects with boosting's abstract locks
and undo. The current Rust implementation has no separately evaluated boosting
backend. It therefore provides no Rust-versus-boosting speedup result.

What is implemented is the relevant extension boundary: semantic intents and
covering observations are adapter-owned, while lock retention and the commit
outcome are core-owned. `TxnCounter` demonstrates blind delta composition with
exclusive installation. General eager-update/undo adapters and multi-owner
commutative locks remain outside the standardized adapter set. Their absence
must not be disguised by relabeling a direct OCC fast path as boosting.

### 5.4 Application-defined operations

The paper demonstrates that an application can expose a stronger operation than
a sequence of generic reads and writes. Rust uses the same principle in typed
batches and closed workload capabilities: the TPC-C bridge can operate on known
Payment, NewOrder, Delivery, and StockLevel shapes, while the upper cache has
restricted record/holder terminals for eligible one-put operations.

These are not public escape hatches around validation. Eligibility includes
exact table/schema, key, value, transaction-state, and ownership requirements.
A declined batch must perform no callbacks before scalar fallback; a real error
dooms or consumes the attempt according to its API. Committing terminals own
the complete attempt, including cold failure disposition. The cache's unchecked
v6 fused lane additionally requires `RecordChecksum::None`; the default CRC32C
profile does not silently use it.

The executable cell example establishes the ordinary API shape. Existing
semantic and failure fixtures exercise the specialized surfaces. Neither is a
new end-to-end TPC-C throughput result.

### 5.5 Datatype design

The retained measurements informed several implementation choices without
establishing their universal superiority or qualifying the later source migration:

- Static singleton prepare/body/teardown and a concrete operation provider reduce
  repeated dispatch while preserving the common phase and quarantine machinery.
- Typed batch and `TouchLog` reuse retain stable ownership rather than copying
  state or relying on destructor execution after a fused success.
- Atomic word packing and bounded atomic payloads reduce representation cost
  without introducing non-atomic races.
- Lazy extended-record allocation on the claiming worker trades extra ownership
  cells for locality and partial-segment allocation. The recorded L2/lock-event
  correlation is not proof of a particular hardware-prefetch mechanism.

Padding/skew, broad inlining, speculative prefetching, and an extra unconditional
pre-CAS load were not retained by the campaign. The source contract therefore
tracks the measured choices rather than accumulating every experimental idea.

False-conflict precision remains a separate axis. A bucketed reference map,
whole-vector snapshot, per-record Masstree scan, and trusted table-generation
scan have different conflict graphs. Throughput on disjoint singleton updates
cannot establish that a coarse scan witness is acceptable for a contended range
workload. Such acceptance requires workload-specific abort, latency, throughput,
and retained-memory evidence.

### 5.6 Silo

The paper's Silo comparison used its own C++ prototype and hardware, with
logging/persistence and opacity disabled for the TPC-C comparison. Those are
historical results, not Rust STO measurements. The local C++ MassTrans engine
is a valuable compatibility and cost oracle, but calling it an oracle does not
make every current comparison a reproduction of the paper's experiment.

This repository has a closed Rust TPC-C bridge and older
[August point-workload measurements](../performance/sto-rust-zoo2-optimized-2026-08-28/README.md).
They must remain separate from the September singleton cache-engine experiment:
workload shape, value representation, integration layer, and build profile
matter. No new TPC-C/Silo speedup or all-workload parity is asserted here.

For the cache, distinguish engine cost, engine-plus-record encoding, foreground
ACK cost, and sustained background RocksDB application. The latter includes
queue backpressure, checksums, batches, GC, and recovery-compatible state.
Foreground commit ACK is volatile even when background RocksDB uses WAL or
synchronous writes; an ACK benchmark is not durable transaction throughput.

### 5.7 Correctness evidence and release limits

| Evidence record | What was exercised | What it does not establish |
| --- | --- | --- |
| [September 13 upper-integration qualification](../testing/mako-cache-rust-sto-2026-09-13/README.md) | Separately selected engines, cache semantics, differential/mutation gates, and cross-engine reopen including checkpoint-only recovery | Acceptance of a later source revision or a Rust-default switch |
| [September 23 retained correctness record](../performance/mako-cache-rust-sto-singleton-attribution-2026-09-17.md) | 22 Docker test executables: 565 passed, one ignored; 69 focused Miri cases, one under the intentional-quarantine leak exception | Native C++ verification by Miri, universal leak freedom, or strict-provenance proof |
| Same campaign's 36 broader smokes | Four-item engine, singleton logged, and singleton cache ACK at 1/8/32 workers for both engines; 41,984 key checks per mode | General parity; cache-ACK runs ended without drain/reopen and are not recovery tests |
| Stage 2 §3.4 documentation-example smoke | The actual complete transfer block was extracted, compiled and executed in pinned Docker; it printed `committed balances: from=90, to=60`. Receipt: `diagnostics/1790795048683895069-R1-public-example/result.json` in the C++-familiar migration archive. | Full candidate correctness, sanitizer, or performance qualification; this is a nonconcurrent `TxnCell` example |
| [October 16 Q11 migration closure](../plans/rust-sto-cpp-shape.md) | Retained matched `R0-30`/`S1-19`/`R1-19` builds, 570 functional cases, 14 correctness gates, five instrumentation gates, the public example, and bounded S1/R1 layout/code reviews | Dirty-local evidence, not clean-commit qualification. Baseline allocation quality-budget exhaustion stopped the campaign before any Q11 throughput comparison; performance and the remaining acceptance gates are explicitly unclaimed. |
| October 24 protocol-alignment core | Core/clock/reference-datatype tests, including retained participants, publication/failure cases and trybuild: PASS (`commands/1792879329755017906-core-clock-datatypes`). | No throughput or production qualification |
| October 24 native/local integration | Full native integration, including the five FFI protocol regressions: PASS (`commands/1792883845466116657-native-rust-integration`). Fixed-read/scan wrappers and both slow-exit smokes: 4/4 PASS (`commands/1792883845601147549-local-wrapper-smokes`). Strict four-crate rustdoc: PASS (`commands/1792883845910105562-strict-rustdoc`). | Not sanitizer or distributed crash/restart proof |
| October 24 native compatibility | Metadata (14), runtime (37), and participant/admission/learner-prefix cases (18): PASS (`commands/1792883818377900684-cpp-metadata`, `commands/1792883818410632049-cpp-runtime`, `commands/1792883818974300719-cpp-participant-rpc`). Note of 2026-11-22: the cases of that executable that constructed Paxos classes, the learner-prefix one among them, were removed when Paxos was deprecated. | Not mixed-backend cluster qualification |
| October 24 cache consumers | Native storage local-version cases (13), Rust engine cases (51), and cache cases (232): PASS (`commands/1792884007865128347-native-cache-consumers`). The existing LSan negative control remains intentionally ignored in this unsanitized run. | No new sanitizer or performance result |
| October 24 poll-worker regression | Both real-loopback ready-pressure/self-wake cases fail before the scheduler fix (`commands/1792883123596713686-poll-io-fairness-before`) and pass afterward (`commands/1792883817589218486-poll-io-fairness`). Existing manual event-chain/nested-fiber compatibility also passes (`commands/1792883822741523431-manual-reactor-compatibility`). | A functional liveness boundary, not latency/throughput qualification |
| October 24 actual topology matrix | All 11 CTests pass: separately selected C++/Rust backends, two-shard NewOrder/Payment, unreplicated execution, Paxos/Raft, and colocated replication; Rust also covers unreplicated colocation. Required replica replay receipts and clean terminal drain are checked (`commands/1792883868957276087-actual-cross-language-protocols`). Note of 2026-11-22: the four Paxos CTests of this matrix were removed when Paxos was deprecated; seven remain. | Replay receipts prove nonempty backend apply-or-version-skip, not payload-digest equality, post-loading-only replay, live failover, or crash/restart recovery |
| October 24 uncertain INSTALL | A private overlay drops a reply only after actual successful participant INSTALL. Both engines fail-stop; Rust reports UNKNOWN rather than retry/success (`commands/1792884364140759364-dropped-install-rust`, `commands/1792884375708817173-dropped-install-cpp`). | Not a general network-fault campaign; no fault hook is shipped in production sources |
| October 24 shutdown freshness | Both engines complete two uneven-duration runs using the same working-directory synchronization namespace; Rust also rejects a missing peer despite old acknowledgements (`commands/1792883869198015395-stale-drain-rust`, `commands/1792883962836986348-stale-drain-cpp`). | The missing-peer negative case was exercised for Rust, not separately for C++ |

These receipts are under
`/home/users/shuai/mako-sto-protocol-alignment-20261024/` on `zoo-002`; all
execution used isolated Docker. The final functional runs exercise the bounded
admission, finite poll-turn, runtime/allocator and shutdown repairs. Earlier
failed traces remain failed evidence: silent pre-voter `WouldBlock` loss,
poll-worker I/O starvation, and RocksDB flushing after background-pool exit
were diagnosed and repaired, not reclassified as successful runs.

The source and gate inventory is in
[the verification reference](#appendix-n-verification-and-evaluation). Campaign counts
belong to their recorded candidates and build profiles, not automatically to
every future worktree state. Exact-candidate sanitizers, differential outcomes,
operational limits, and accepted application budgets remain release gates.

The Q11 receipts and explicit stop decision are retained under
`/home/users/shuai/mako-sto-cpp-shape-20260924/validation/`, including
`guard-v2-q11-functional-validation-03.json`,
`guard-v2-q11-correctness-validation-03.json`,
`guard-v2-q11-instrumentation-validation-02.json`, and
`guard-v2-q11-user-stop-01.json`. These are historical results for their named
inputs, not checks rerun by this documentation update. No further performance
campaign or shared-service change is authorized by the paper.

## 6. Conclusions

Rust STO implements the paper's main architectural idea: datatypes describe
semantic dependencies, and a common transaction engine certifies and publishes
them together. Typed state, explicit worker borrowing, checked erasure,
phase-restricted capabilities, and precise failure outcomes make the Rust
ownership and trust boundaries reviewable.

The implementation includes reference datatypes, transactional Masstree,
a selectable cache engine with logging/recovery, and native Mako distributed
coordination with retained participants, term-aware history and backend-routed
replay. These do not provide core opacity, general-purpose snapshot isolation,
native directory reclamation, async migration, or durable foreground cache ACK.
Existing SRPC and replication infrastructure remains shared with C++; native
Rust protocol ownership does not require generating a C++ STO backend.

The historical singleton result is evidence that common-path representation
and ownership costs can be reduced without removing certification. It does
not qualify the later source migration, change defaults, or establish general
parity. The migrated implementation and its retained correctness/safety
evidence remain available with performance explicitly unqualified. Future
implementation changes must preserve the contracts below and meet the
acceptance criteria of the particular integration being changed.

## Detailed implementation contracts

These appendices retain the detailed API, safety, integration, and rollout
contracts. Signature excerpts omit bodies, including inherent-method bodies;
they are reference notation rather than compilable replacement implementations.

- [Appendix A. Scope and semantic contract](#appendix-a-scope-and-semantic-contract)
- [Appendix B. Architecture and programming model](#appendix-b-architecture-and-programming-model)
- [Appendix C. Transaction items](#appendix-c-transaction-items)
- [Appendix D. Rust adapter interface and protocol](#appendix-d-rust-adapter-interface-and-protocol)
- [Appendix E. Versions and the Rust memory model](#appendix-e-versions-and-the-rust-memory-model)
- [Appendix F. Commit, abort, and failure state machine](#appendix-f-commit-abort-and-failure-state-machine)
- [Appendix G. Predicates, commutativity, and absence](#appendix-g-predicates-commutativity-and-absence)
- [Appendix H. Opacity](#appendix-h-opacity)
- [Appendix I. Lifetimes and reclamation](#appendix-i-lifetimes-and-reclamation)
- [Appendix J. Masstree transactional adapter](#appendix-j-masstree-transactional-adapter)
- [Appendix K. Masstree C ABI](#appendix-k-masstree-c-abi)
- [Appendix L. Correctness argument](#appendix-l-correctness-argument)
- [Appendix M. Mako and upper-layer integration](#appendix-m-mako-and-upper-layer-integration)
- [Appendix N. Verification and evaluation](#appendix-n-verification-and-evaluation)
- [Appendix O. Implementation status and rollout](#appendix-o-implementation-status-and-rollout)
- [Appendix P. Decision record and change policy](#appendix-p-decision-record-and-change-policy)

## Appendix A. Scope and semantic contract

### A.1 Goals, non-goals, and supported scope

#### A.1.1 Goals

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
9. Reject unavailable isolation profiles rather than silently weakening them.
10. Be differentially testable against an independent ordered-map model.
    The retained C++ comparison results are historical evidence.

#### A.1.2 Non-goals

The transaction core is not:

- a transparent, word-based STM or compiler transformation;
- an automatic wrapper for an arbitrary non-thread-safe datatype;
- a port of Mako RPC, Paxos, RocksDB, logging, or watermark logic;
- a general-purpose MVCC engine (table-specific distributed histories live in
  `sto-masstree`);
- a network coordinator or replication authority (it supplies the retained
  participant and typed decision seam used by the upper native coordinator);
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

#### A.1.3 Implemented v1 feature boundary

The implemented v1 core and adapter capability set includes:

- non-opaque OCC with final read and predicate validation;
- core `TxnCell`, `TxnArray`, and `TxnCounter`, plus external map/vector/queue
  reference adapters;
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
- opt-in record-local versions, structural admission policy, and permanent
  general-table directory sealing;
- an opt-in trusted scan-generation profile for closed direct tables; and
- an optional bounded commit hook with metadata reservation, fallible
  pre-install acceptance, and infallible post-install publication phases.

The `sto-masstree/fixed-u64` feature additionally provides an optional,
specialized all-present point-workload profile. It is not a replacement for the
general binary-value table and does not add transactional liveness changes or
range operations; its exact restrictions are normative in
[Appendix J.7](#j7-optional-fixed-u64-specialization).

The optional upper cache engine and its logging/recovery integration remain
outside core; Rust is the sole backend. Native Mako integration uses
the implemented retained participant, distributed authority, histories, SRPC,
and backend-routed replay described in Appendix M. Core crash durability and
native reclamation are not implied by either integration.

Opacity remains deferred. An API request for an
unimplemented isolation profile MUST fail explicitly; it MUST NOT silently
downgrade.

### A.2 Semantic contract

#### A.2.1 Definitions

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

#### A.2.2 Required guarantees

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

**[STO/DEFERRED] Optional opacity.** The paper's opaque profile protects even
transactions that later abort from a transactionally inconsistent state. Rust
STO currently rejects this profile. The implemented non-opaque profile retains
commit atomicity and strict serializability but does not provide this
execution-time snapshot guarantee.

#### A.2.3 Progress and retry

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
rollback mechanism. A post-install hook runs after the irreversible boundary.
It may publish only infallible, preallocated metadata whose visibility must
precede lock release. A panic there makes the outcome indeterminate and
quarantines the runtime.

#### A.2.4 Error classes

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

## Appendix B. Architecture and programming model

### B.1 System architecture and trust boundaries

The dependency diagram in §3.2 describes the implemented stack. Upper cache
policy and the closed C++ TPC-C bridge are separate clients of the same Rust
transaction and Masstree layers.

#### B.1.1 Crates

| Crate | Implemented responsibility |
| --- | --- |
| `sto-core` | Runtime, workers, typed items, synchronous datatype locking, certification, retained participants, failure handling, and cell/array/counter references |
| `sto-test-datatypes` | External correctness-oriented map/vector/queue adapters |
| `sto-masstree` | Rust transactional records, values, registries, tombstones, scans, and optional fixed-u64 table |
| `masstree` | Safe thread-affine native runtime/worker/tree and copied point/scan facade |
| `mtree-sys` | Raw `no_std` C11 ABI declarations; no independent C++ build |
| `mako-engine-api` | Common engine operations, outcomes, limits, features, and record contracts |
| `mako-engine-sto` | Native Rust implementation of the common local engine contract |
| `mako-engine` | Public reexports of the sole Rust transaction engine |
| `mako-clock` | Separate distributed decimal base/term authority and local cache HLC/order coordination |
| `mako-cache` | Foreground API, log lanes, writeback, checkpoint/recovery, and health |
| `mako-differential` / `mako-history` | Shared operation corpus and independent bounded isolation oracle |
| `sto-tpcc-ffi` | Native Mako coordinator/participant bridge, final-write ownership and legacy log serialization |

Core accepts typed `DistributedTimeTerm` installation metadata from `mako-clock`;
it does not allocate network decisions or implement RPC, replication, RocksDB,
or cache policy. The real distributed coordinator is in `sto-tpcc-ffi`, not an
imagined separate `sto-mako` crate.

#### B.1.2 Trust boundaries

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
`TObject`, `OpacityToken`, `TransactionLock`, and
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

### B.2 Rust programming model

#### B.2.1 Runtime and worker contexts

`Runtime` owns:

- a unique `RuntimeId`;
- the ordered commit clock;
- a weak object-registration registry;
- the finite owner-ID registry; and
- runtime configuration and health.

`ObjectRegistration` and typed resource bindings retain object leases; leases
own the resource-class registrations. A typed binding also retains its adapter,
checked type identity, and captured version policy. The runtime's weak registry
does not itself keep every registered object alive.

`WorkerContext` is a same-thread attachment to one core runtime and is
`!Send + !Sync`. It owns one checked `OwnerId` and independently pools a boxed
`TransactionFrame` and boxed `TerminalReadScratch`. Begin transfers the
appropriate allocation to the active handle; clean completion returns it.
A second simultaneous attachment on the same thread/runtime is rejected.
Dropping the worker disposes its retained storage and releases the core owner
slot while preserving lock-plan nonce generation.

This core lifecycle is distinct from native Masstree registration. Native
workers are fixed and long-lived: recreating a facade on the same OS thread
reuses its registration, but replacing the OS thread consumes another native
core ID. Appendix K.5 specifies that process-lifetime boundary. The core's
finite owner domain and each integration's native limit are checked separately.

For a `TxnCell<i64>`, the core API uses explicit context borrowing:

```rust
let mut txn = worker.begin()?;
let old = cell.get(&mut txn)?;
cell.set(&mut txn, old + 1)?;
match txn.commit()? {
    CommitOutcome::Committed(info) => { /* visible */ }
    CommitOutcome::Aborted(reason) => { /* caller may retry */ }
}
```

Ambient TLS MAY exist inside the C++ bridge or a legacy compatibility shim, but
it is not the Rust core's ownership model.

#### B.2.2 Public transaction surface

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
    pub fn new(config: RuntimeConfig) -> Result<Arc<Self>, RuntimeError>;

    pub fn attach(
        self: &Arc<Self>,
    ) -> Result<WorkerContext, AttachError>;
}

impl WorkerContext {
    pub fn begin(
        &mut self,
    ) -> Result<Transaction<'_, Active>, BeginError>;

    pub fn begin_with(
        &mut self,
        isolation: IsolationMode,
    ) -> Result<Transaction<'_, Active>, BeginError>;

    pub fn begin_terminal_read_batch(
        &mut self,
    ) -> Result<
        TerminalReadTransaction<'_, TerminalReadOpen>,
        BeginError,
    >;

    pub fn begin_terminal_read_batch_with(
        &mut self,
        isolation: IsolationMode,
    ) -> Result<
        TerminalReadTransaction<'_, TerminalReadOpen>,
        BeginError,
    >;
}

impl<'worker> Transaction<'worker, Active> {
    pub fn commit(
        self,
    ) -> Result<CommitOutcome, CommitFailure>;

    pub fn abort(self) -> AbortInfo;
}

impl<'worker> TerminalReadTransaction<'worker, TerminalReadOpen> {
    pub fn with_terminal_read_batch<A>(
        self,
        resource: &RegisteredResource<A>,
        keys: &[A::Key],
        operation: impl for<'entry> FnMut(
            usize,
            &mut TerminalReadProxy<'entry, A>,
        ) -> Result<(), AccessError>,
    ) -> Result<
        TerminalReadTransaction<'worker, TerminalReadReady>,
        AccessError,
    >
    where
        A: TObject,;

    pub fn abort_with_access_error(
        self,
        error: AccessError,
    ) -> AccessError;
}

impl TerminalReadTransaction<'_, TerminalReadReady> {
    pub fn commit(self) -> Result<CommitOutcome, CommitFailure>;

    pub fn abort(self) -> AbortInfo;
}

pub struct TerminalReadProxy<'entry, A: TObject> {
    /* private */
}

impl<A: TObject> TerminalReadProxy<'_, A> {
    pub fn key(&self) -> &A::Key;

    pub fn add_read(
        &mut self,
        observation: A::Observation,
    ) -> Result<(), AccessError>;
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
Ordinary conflict, nonterminal capacity failure, and hook rejection return
`Ok(CommitOutcome::Aborted(...))` when teardown succeeds without a poisoning
fault. Adapter faults and contained commit-callback panics before irreversibility
return `CommitFailure::Poisoned` with a definite aborted outcome. Uncertain
publication returns `Indeterminate`; failure after proven publication returns
`Poisoned` with a definite committed outcome. A caller MUST use that outcome
and MUST NOT retry a definitely committed or indeterminate attempt.
`CommitInfo` contains core OCC metadata only; it never smuggles a
`MakoTimestamp`, durability sequence, or upper-layer commit decision into
`sto-core`.

Both completion methods consume the active transaction. `abort` is infallible
under the adapter contract and returns only after exact-once cleanup. Dropping
an active transaction takes the same abort path but cannot return its
`AbortInfo`; any impossible cleanup violation is recorded in runtime health.

The terminal-read handle is deliberately a different public type, not a mode
bit on `Transaction<Active>`. `TerminalReadOpen` exposes exactly one consuming
`with_terminal_read_batch` transition. The callback receives only the retained
key and a one-shot `add_read`; it cannot create local state, predicates,
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
restricted representation is defined in Appendix D.3, and its commit path is
defined in Appendix F.2.

#### B.2.3 Transaction ownership

`Transaction<'worker, Active>` mutably borrows its worker. It is `!Send` and
`!Sync`, cannot outlive the worker, and cannot be used concurrently. A mutable
borrow is the primary guard against reentrant use.

Beginning a nested transaction on an occupied worker is an `InvalidUse` error
in v1. Adapters and commit hooks cannot begin one indirectly.

Dropping an active transaction aborts it. This is the Rust equivalent of the
paper's hidden transaction guard. Drop cleanup MUST be bounded and MUST NOT
panic.

Every acquired lock has one release path. `Transaction::into_participant`
transfers the same owned frame into `Participant<'worker>`, retaining the worker
borrow rather than copying its items into another protocol. See
[Appendix M.3](#m3-native-distributed-participants-and-coordination).

#### B.2.4 No async suspension

Neither transaction nor participant may cross `.await`, migrate threads, or
yield ownership of the worker while holding locks/native guards. Retained
participants permit synchronous network lifecycle calls between phase methods
on the same persistent helper lane. This deliberately retains locks while the
upper protocol waits and has a liveness cost; it is not an async-safe API.
Datatype and commit-hook callbacks still prohibit blocking I/O and reentry.

## Appendix C. Transaction items

The implementation is [`trans_item.rs`](../../crates/sto-core/src/trans_item.rs):
`TransProxy` is the scoped public operation view; `TransItem`,
`TransItemData`, and `TypedTransItemBatch` are private storage. The public
phase views and associated-type contract are in
[`object.rs`](../../crates/sto-core/src/object.rs). The following identity,
composition, and lock-plan rules apply to both ordinary and specialized storage.

### C.1 Stable identity

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
`TObject` implementation. Erased key equality or ordering is
attempted only after `ObjectId`, `ResourceClass`, and the stored key `TypeId`
match. A mismatch is an invalid registration or poisoned core state, never an
unchecked downcast.

The core may hash the identity for lookup, but correctness MUST use full
equality. Hash collisions are ordinary collisions, not aliases.

Logical identity and physical lock identity are distinct. During `TObject::lock`,
every staged write synchronously contributes one or more requests of the form:

```text
LockIdentity = (LockNamespaceId, LockClass, LockKey)
```

`LockNamespaceId` is unique within a runtime for one physical lock namespace.
The same non-reentrant physical lock MUST always map to the same full
`LockIdentity`, even when it conservatively protects several logical resources
or objects; distinct physical locks MUST NOT compare equal. Equality and total
order are correctness properties, not hash properties.

### C.2 Deduplication and mutation composition

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

### C.3 Item contents

An item may own:

- read observations;
- a predicate token and its covering observations;
- a staged write intent;
- adapter scratch state prepared before locking;
- an acquired lock token; and
- cleanup metadata.

The original `TItem` made a read version and predicate value mutually exclusive
before predicate upgrade. Rust implements that lifecycle explicitly as
`Unobserved | Read | Predicate | UpgradedPredicate`, rather than permitting
unchecked combinations of observation fields.

External adapters implement the typed protocol in Appendix D.
`Transaction::with_item`, resolved access, item sessions, and the exactly
unique resource-group append lane feed the same private typed item storage. The core
performs `TypeId`-checked key and item downcasts; no public `Any`,
caller-supplied vtable, or unchecked extraction exists. Applications normally
call adapter methods such as `get` and `put`, not this adapter-author surface
directly.

### C.4 Physical lock acquisition modes

[`LockContext`](../../crates/sto-core/src/lock.rs) synchronously acquires each
request in item/access order. `try_lock` deduplicates full `LockIdentity` and
checks that duplicates name the same concrete type and canonical target.
Logical items sharing a physical lock keep independent intents/observations,
but share one core-owned guard. No sorted acquisition pass follows the callback.
Every acquisition is bounded/nonblocking; failure releases the acquired prefix
in reverse order. This avoids indefinite deadlock, not conflicts or starvation.

`try_unique_lock` is a transaction-wide local optimization: every emitting
adapter must use the unique lane and physical identities must be distinct.
Core rejects mixed lanes and duplicate requests. `with_unique_lock_requests`
on one table cannot prove the whole adapter graph. Read-only generation
resources emit no lock. Retained participants revisit items across batches:
their unique requests use canonical deduplication so an already-held target
is not acquired again, and preparation is refreshed from the latest intent.

The homogeneous direct capability is a separate representation of these
obligations. Safe constructors check duplicates; unsafe injective constructors
require distinct full item identities to map to distinct physical targets/tokens.
Eligibility still requires writes, no predicates, no ordinary prepared state,
and one identical capability across exact bindings. Direct failure also
reverse-releases every acquired guard.

`InstallContext::try_publish_unlock` permits local datatype publication only
for an exclusive physical lock. Shared aliases defer until final release so an
unfinished sibling mutation cannot become visible. Retained participants always
defer. Core owns guards and targets even after early publication until teardown;
cleanup has no independent unlock authority.

The paper's access-order/bounded-acquisition approach and Rust's default now
agree on order, while Rust additionally checks identities, ownership, exhaustion,
and failure dispositions. Historical sorting-performance attribution is not
evidence about this source.


## Appendix D. Rust adapter interface and protocol

The v1 signatures in this section are the normative adapter-author contract.
The ownership, type-state, phase, fallibility, safe base traits, and explicit
unsafe direct-token seam are design requirements. Signature blocks omit method bodies to focus on public shape. The implemented
methods and an out-of-crate adapter contract fixture live in `crates/sto-core`;
these excerpts are not replacement implementations.

The canonical definitions are in
[`object.rs`](../../crates/sto-core/src/object.rs), with operation-time proxies
and private item dispatch in
[`trans_item.rs`](../../crates/sto-core/src/trans_item.rs).
The concrete walkthroughs in [§3.4](#34-example) introduce the lifecycle;
they do not restrict the external direct/batch/token capabilities below.

### D.1 Mapping the C++ class abstraction to Rust

Figure 1 of the paper and the current C++ implementation use inheritance because
a heterogeneous transaction stores `TObject*` and invokes virtual commit
callbacks. Rust uses a typed trait at the extension boundary and a sealed
object-safe shim inside the core. It does **not** expose a public inheritance
hierarchy or ask adapters to implement type erasure.

| C++ STO abstraction | Rust STO counterpart | Deliberate difference |
| --- | --- | --- |
| `TObject` subclass | Rust `TObject` implementation held by `RegisteredResource<A>` | Associated types replace untyped per-item payloads. |
| `TItem` / `TransItem` | private `TransItem<A>` with an explicit observation and preparation state machine | The core, not an adapter, owns flags and legal state transitions. |
| `Sto::item(obj, key)` / `TransProxy` | `Transaction::with_item(resource, key, operation)` and scoped `TransProxy<A>` | Explicit transaction borrowing replaces TLS; a proxy cannot escape the operation. |
| `TransProxy::{observe, add_read, set_predicate, add_write}` | Rust `TransProxy::{add_read, set_predicate, add_write}` plus typed accessors | The core checks legal state transitions instead of mutating public flags. |
| `rdata`, predicate, and `wdata` slots | `A::Observation`, `A::Predicate`, and `A::Intent` | No raw pointer union or caller-selected cast. |
| `TransProxy::stash` / datatype-private item data | `A::Local` through scoped `TransProxy` and phase views | Owned typed state replaces an unmarked `void*` payload. |
| `TObject::lock` | `A::lock` calls `LockContext::try_lock`/`try_unique_lock` synchronously | Datatypes choose acquisition; the core retains and deduplicates physical guards. |
| `check_predicate(item, false)` | `A::revalidate_predicate` | Execution-time opacity check does not change item state. |
| `check_predicate(item, true)` | `A::check_predicate` | On success the core changes `Predicate` to `UpgradedPredicate`. |
| `TObject::check` | `A::check` | Validation receives only phase-appropriate capabilities. |
| `TObject::install` | `A::install` | Called only after an internal irreversible permit exists; no `Result`. |
| `TObject::unlock` | `TransactionLock::release` | One core-owned guard selects abort, commit, or indeterminate publication. |
| `TObject::cleanup` | `A::cleanup` | Runs after all physical locks are released on definite outcomes. |
| specialized homogeneous commit callbacks | `A::direct_commit_capability()` returning a core-defined `DirectCommitCapability<A>` | An optional typed capability selects a sealed direct plan; safe constructors keep exact duplicate-lock checking, while unsafe injective constructors make the omitted check and any write-at-acquisition certification an explicit proof obligation. |
| `Sto::commit_id()` | `InstallContext::occ_commit_id` and committed `LockDisposition` | Core OCC identity is phase-scoped and remains distinct from upper timestamps. |
| `new_item`, `fresh_item`, `read_item`, `check_item` | `with_item`, resolved access, complete/streaming unique batches, and `ObservationState` transitions | The fast lane requires a core-checked exact uniqueness proof. A group can start an empty transaction or append to a homogeneous typed prefix, including scalar items, when its exact binding has not appeared. It exposes no unchecked item access. |
| specialized terminal read loop | `TerminalReadTransaction<Open/Ready>`, `TerminalReadProxy`, and `TerminalReadBatchCapability` | A distinct consuming typestate proves there can be no later operation or mutation; the batch stores only typed keys and observations. |
| clear/user flags | private observation/preparation states and `A::Local` | Adapters own typed local data, not core flag bits. |
| `TObject::print` | ordinary adapter diagnostics outside the commit protocol | No formatting callback runs while committing. |
| Mako `get_table_id` / `get_is_remote` extensions | upper integration layer | Distribution and table-routing policy are not STO callbacks. |
| static `Sto` / current transaction TLS | `WorkerContext` and `Transaction<'worker, Active>` | Worker affinity and transaction lifetime are explicit. |

This split preserves the paper's central virtual-callback seam while making the
equivalent of `TItem` a core-controlled generic type. A datatype exposes normal
methods such as `get`, `put`, or `increment`; those methods use the proxy API
below and are not themselves required trait methods.

The implemented public relationships are:

```text
ResourceKey                 OpacityToken
     |                           |
     +------ associated types ---+
                    |
                 TObject
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

`TObject` is the adapter extension trait. `TransactionLock` is
the independent physical-lock contract used by datatype locking and commit. The
capability types are core-owned structs, not adapter-defined commit drivers.
They authorize narrower sealed paths while leaving the ordinary trait methods
available as fallback. `DirectBorrowedLockTarget` only supplies a stable
adapter-owned lock target. `DirectTokenLock` is the sole unsafe trait in this
hierarchy because its compact token must name the exact stable lock that its
guard protects.

### D.2 Object registration and typed item access

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
pub struct RegisteredResource<A: TObject> { /* private */ }
pub struct TransProxy<'entry, A: TObject> { /* private */ }
pub struct ResolvedItemSession<'session, A: TObject> { /* private */ }
pub struct UniqueItemKeyIndex { /* reusable private buckets */ }
pub struct UniqueItemKeys<'keys, K: ResourceKey> { /* private */ }

impl UniqueItemKeyIndex {
    pub const fn new() -> Self;

    pub fn try_reserve_for_len(
        &mut self,
        needed: usize,
    ) -> Result<(), std::collections::TryReserveError>;
}

impl<'keys, K: ResourceKey> UniqueItemKeys<'keys, K> {
    pub fn try_new(keys: &'keys [K]) -> Option<Self>;

    pub fn try_new_indexed(
        keys: &'keys [K],
        order: &mut Vec<usize>,
    ) -> Result<Option<Self>, std::collections::TryReserveError>;

    pub fn try_new_hashed(
        keys: &'keys [K],
        index: &mut UniqueItemKeyIndex,
    ) -> Result<Option<Self>, std::collections::TryReserveError>;
}

impl<A: TObject> Clone for RegisteredResource<A> {
    fn clone(&self) -> Self;
}

impl<A: TObject> RegisteredResource<A> {
    pub fn adapter(&self) -> &A;
}

impl Runtime {
    pub fn register_object(
        self: &Arc<Self>,
    ) -> Result<ObjectRegistration, RegistrationError>;
}

impl ObjectRegistration {
    pub fn register_resource<A: TObject>(
        &self,
        class: ResourceClass,
        adapter: A,
    ) -> Result<RegisteredResource<A>, RegistrationError>;
}

impl<'worker> Transaction<'worker, Active> {
    pub fn with_item<A, R>(
        &mut self,
        resource: &RegisteredResource<A>,
        key: A::Key,
        operation: impl for<'entry> FnOnce(
            &mut TransProxy<'entry, A>,
        ) -> Result<R, AccessError>,
    ) -> Result<R, AccessError>
    where
        A: TObject,;

    pub fn with_unique_item_batch<A>(
        &mut self,
        resource: &RegisteredResource<A>,
        keys: UniqueItemKeys<'_, A::Key>,
        operation: impl for<'entry> FnMut(
            usize,
            &mut TransProxy<'entry, A>,
        ) -> Result<(), AccessError>,
    ) -> Result<(), AccessError>
    where
        A: TObject,;

    pub fn with_item_session<A, R>(
        &mut self,
        resource: &RegisteredResource<A>,
        operation: impl for<'session> FnOnce(
            &mut ResolvedItemSession<'session, A>,
        ) -> Result<R, AccessError>,
    ) -> Result<R, AccessError>
    where
        A: TObject,;
}

pub enum ItemBatchControl {
    Continue,
    Stop,
}

pub enum ItemBatchOutcome {
    Ineligible,
    Complete { appended: usize },
    Stopped { appended: usize },
}

impl<A: TObject> ResolvedItemSession<'_, A> {
    pub fn can_start_unique_item_batch(&self) -> bool;

    pub fn try_with_unique_item_batch(
        &mut self,
        keys: UniqueItemKeys<'_, A::Key>,
        operation: impl for<'entry> FnMut(
            usize,
            &mut TransProxy<'entry, A>,
        ) -> Result<(), AccessError>,
    ) -> Result<bool, AccessError>;

    pub fn try_with_unique_item_batch_while(
        &mut self,
        keys: UniqueItemKeys<'_, A::Key>,
        operation: impl for<'entry> FnMut(
            usize,
            &mut TransProxy<'entry, A>,
        ) -> Result<ItemBatchControl, AccessError>,
    ) -> Result<ItemBatchOutcome, AccessError>;
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
shared datatype, records the typed observation through `TransProxy`, overlays any
staged intent, and only then returns the abstract result.

`with_item` MUST:

1. reject a resource registered in another runtime;
2. form the full identity `(ObjectId, ResourceClass, key)` and use hashing only
   to find candidates, never to replace full equality;
3. return the existing typed item when that identity is already present;
4. on a miss, call `A::new_local` and activate one core-owned typed item:
   `TransItemData<A>` plus sidecars in a homogeneous `TypedTransItemBatch<A>`,
   or `TransItem<A>` behind checked private erasure in heterogeneous storage;
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
exact duplicate exists, and all allocation growth is fallible. A group can
start an empty transaction or append to an existing homogeneous typed prefix,
including a scalar-only prefix, when its exact `RegisteredResource` binding
has not appeared. Earlier scalar accesses may already have populated an exact
item-index prefix. A binding may occur in only one direct group; therefore exact
uniqueness inside each group implies uniqueness of every full `(ObjectId,
ResourceClass, key)` identity even when two resources use equal keys. Core
checks the proposed total live-item count against the configured limit and
reserves the enlarged typed vectors before initializing any item in the new
group. It then appends pooled typed items in group and key order without
hashing, probing, or extending the item index.

The operation receives the ordinary full `TransProxy`, so observations,
predicates, and intents retain their normal transition rules. A later scalar or
resolved access first tries exact last-access binding/key reuse; a singleton
with a previously absent binding can also append without indexing. An uncached
lookup that may match a live binding indexes the missing suffix before probing.
At all times current-generation index entries cover exactly the live slots
`[0, item_index.len)`, while `[item_index.len, item_count)` is an unindexed
typed suffix. A different adapter type materializes the typed items into the
ordinary heterogeneous representation. Full erased-identity equality still
resolves hash collisions and
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
failure, callback error, initialization failure, or unwind dooms the transaction.
An initializer failure leaves that item inactive. An operation error or unwind
occurs after activation, so the current item joins every earlier active item in
reverse abort cleanup. Successful cleanup retains the worker-local typed slots
and their exact adapter bindings, allowing later attempts to reactivate the
same layout or rebind pooled slots under contained destruction.

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
boundary. That scalar fallback uses the exact reuse/index rules above and
safely handles repeated identities. A real append error is latched as the
session's first error, and later session operations report a doomed transaction.

`try_with_unique_item_batch_while` is the streaming form. `Ineligible` invokes
no operation and permits scalar fallback. `Stop` retains the current item and
leaves all following keys untouched; `appended` counts the retained prefix.
Unlike the complete-batch form, streaming capacity/storage admission happens
immediately before each next item. A capacity error preserves the already-active
prefix for abort cleanup and dooms the attempt rather than undoing its callbacks.

### D.3 The transactional-resource trait

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
    &<A as TObject>::Key,
    &<A as TObject>::Observation,
    &PreflightFreeValidationContext<'context>,
) -> Result<(), CheckError>;

pub struct TerminalReadBatchCapability<
    A: TObject,
> { /* private */ }

impl<A: TObject> TerminalReadBatchCapability<A> {
    pub const fn new_drop_only(
        validate: TerminalReadBatchValidate<A>,
    ) -> Self;
}

pub type PreflightFreeReadValidate<A> = for<'context> fn(
    &A,
    &<A as TObject>::Key,
    &<A as TObject>::Observation,
    &PreflightFreeValidationContext<'context>,
) -> Result<(), CheckError>;

pub type PreflightFreeReadCleanup<A> = for<'item, 'context> fn(
    &A,
    &<A as TObject>::Key,
    CleanupItem<'item, A>,
    &mut CleanupContext<'context>,
);

pub struct PreflightFreeReadCapability<A: TObject> { /* private */ }

impl<A: TObject> PreflightFreeReadCapability<A> {
    pub const fn new(
        validate: PreflightFreeReadValidate<A>,
        finish_committed: PreflightFreeReadCleanup<A>,
    ) -> Self;

    pub const fn new_drop_only(
        validate: PreflightFreeReadValidate<A>,
    ) -> Self;
}

pub struct UniqueLockCommitCapability<
    A: TObject,
    L: TransactionLock,
> { /* private callbacks */ }

pub struct BorrowedUniqueLockCommitCapability<
    A: TObject,
    L: TransactionLock,
> { /* private callbacks */ }

pub trait BorrowedTokenCommitOperations<A: TObject, L: DirectTokenLock>:
    Sync + 'static
{
    fn prepare(
        &self,
        adapter: &A,
        key: &A::Key,
        item: PreflightItem<'_, A>,
    ) -> Result<Option<BorrowedLockToken<L::Token>>, PrepareError>;

    fn validate(
        &self,
        adapter: &A,
        key: &A::Key,
        item: DirectValidationItem<'_, A>,
        lock: Option<DirectLockRef<'_, L>>,
        cx: &DirectValidationContext,
    ) -> Result<(), CheckError>;

    fn install(
        &self,
        adapter: &A,
        key: &A::Key,
        item: InstallItem<'_, A>,
        lock: DirectLockMut<'_, L>,
        cx: &mut DirectInstallContext,
    );
}

pub struct BorrowedInjectiveLockCommitCapability<
    A: TObject,
    L: DirectTokenLock,
    O: BorrowedTokenCommitOperations<A, L>,
> { /* private operation provider and certification flags */ }

pub struct DirectCommitCapability<A: TObject> { /* sealed */ }

pub trait DirectBorrowedLockTarget<L: TransactionLock>:
    TObject
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

impl<A, L, O> BorrowedInjectiveLockCommitCapability<A, L, O>
where
    A: TObject,
    L: DirectTokenLock,
    O: BorrowedTokenCommitOperations<A, L>,
{
    pub const fn new(operations: O) -> Self;

    pub const unsafe fn with_write_acquisition_certification(mut self) -> Self;
}

impl<A: TObject> DirectCommitCapability<A> {
    pub const fn unique_lock<L: TransactionLock>(
        implementation: &'static UniqueLockCommitCapability<A, L>,
    ) -> Self
    where
        A: TObject<Predicate = NoPredicate>,;

    pub const fn borrowed_unique_lock<L: TransactionLock>(
        implementation: &'static BorrowedUniqueLockCommitCapability<A, L>,
    ) -> Self
    where
        A: DirectBorrowedLockTarget<L>
            + TObject<Predicate = NoPredicate>,;

    pub const unsafe fn borrowed_injective_lock<L: TransactionLock>(
        implementation: &'static BorrowedUniqueLockCommitCapability<A, L>,
    ) -> Self
    where
        A: DirectBorrowedLockTarget<L>
            + TObject<Predicate = NoPredicate>,;

    pub const unsafe fn borrowed_injective_token_lock<
        L: DirectTokenLock,
        O: BorrowedTokenCommitOperations<A, L>,
    >(
        implementation: &'static BorrowedInjectiveLockCommitCapability<A, L, O>,
    ) -> Self
    where
        A: DirectBorrowedLockTarget<L>
            + TObject<Predicate = NoPredicate>,;

    pub const fn with_drop_only_committed_cleanup(self) -> Self;
}

pub enum CommitVersionRequirement {
    Ordered,
    AdapterLocal,
}

pub trait TObject: Send + Sync + Sized + 'static {
    type Key: ResourceKey;
    type Local: 'static;
    type Observation: OpacityToken;
    type Predicate: OpacityToken;
    type Intent: 'static;
    type Prepared: 'static;

    fn commit_version_requirement(&self) -> CommitVersionRequirement {
        CommitVersionRequirement::Ordered
    }

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

    fn lock(
        &self,
        key: &Self::Key,
        item: PreflightItem<'_, Self>,
        cx: &mut LockContext<'_>,
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

    fn check_predicate(
        &self,
        key: &Self::Key,
        predicate: &Self::Predicate,
        prepared: &Self::Prepared,
        cx: &PredicateContext<'_>,
    ) -> Result<Self::Observation, CheckError>;

    fn check(
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

    fn cleanup(
        &self,
        key: &Self::Key,
        item: CleanupItem<'_, Self>,
        prepared: Option<&mut Self::Prepared>,
        disposition: CleanupDisposition,
        cx: &mut CleanupContext<'_>,
    );
}

pub struct PreflightItem<'a, A: TObject> { /* private */ }
pub struct InstallItem<'a, A: TObject> { /* private */ }
pub struct CleanupItem<'a, A: TObject> { /* private */ }

impl<A: TObject> PreflightItem<'_, A> {
    pub fn local(&self) -> &A::Local;

    pub fn local_mut(&mut self) -> &mut A::Local;

    pub fn observation(&self) -> ObservationRef<'_, A>;

    pub fn intent(&self) -> Option<&A::Intent>;

    pub fn intent_mut(&mut self) -> Option<&mut A::Intent>;
}

impl<A: TObject> InstallItem<'_, A> {
    pub fn local_mut(&mut self) -> &mut A::Local;

    pub fn observation(&self) -> ObservationRef<'_, A>;

    pub fn intent(&self) -> &A::Intent;
}

impl<A: TObject> CleanupItem<'_, A> {
    pub fn local_mut(&mut self) -> &mut A::Local;

    pub fn observation(&self) -> ObservationRef<'_, A>;

    pub fn remaining_intent(&self) -> Option<&A::Intent>;

    pub fn take_remaining_intent(&mut self) -> Option<A::Intent>;
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
`lock`/`Prepared`/general-lock-plan sequence. The returned capability MUST
be one static value for the complete binding lifetime. It supplies typed
prepare, validate, and install callbacks to a sealed core driver. The core
selects it only for the homogeneous eligibility conditions in Appendix C.4;
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

**[DEFERRED]** `revalidate_read`, `revalidate_predicate`, and
`ExecutionCheckContext` reserve the execution-time opacity seam. The implemented
serializable profile does not invoke whole-transaction revalidation on each
read, and opaque begins return `Unsupported`. In a future opaque implementation,
a predicate revalidation result would certify a checked-through bound without
performing the commit-time upgrade; unordered tokens would require conservative
revalidation. This is not current execution behavior.

`TerminalReadBatchCapability` is stronger than
`PreflightFreeReadCapability::new_drop_only`. It proves that a homogeneous
batch operation needs only `A::Key` and `A::Observation`: core MUST NOT
construct `A::Local`, `A::Intent`, or `A::Prepared`, and MUST NOT invoke
`new_local`, `lock`, `install`, or `cleanup` for the terminal batch. Its
validation callback performs the same final certification as `check`,
using the restricted `PreflightFreeValidationContext` and no lock guard.
Dropping each key and observation MUST be the complete cleanup on both commit
and abort; cleanup cannot depend on shared mutation, outcome, transaction-local
state, or a phase context. An operation using the capability is therefore a
read only and cannot create an untracked cleanup obligation.

As with the ordinary prepared-free capability, an adapter MUST return the same
`'static` terminal capability for the complete registered lifetime. Merely
having a cheap `check` implementation is not enough to advertise it.
The capability opts into the representation and cleanup contract; the
`TerminalReadOpen -> TerminalReadReady` consuming transition proves that no
later general operation can invalidate that contract.

`PreflightItem`, `InstallItem`, and `CleanupItem` have private fields and expose
only phase-appropriate typed accessors. `PreflightItem` can inspect observations,
predicates, and intents. `InstallItem` can borrow but cannot move the core-owned
intent, keeping it reachable if installation unwinds. Only `CleanupItem`, after
all locks are gone and the outcome is definite, can take the remaining intent
or drain cleanup stored in `Local` and `Prepared`. None exposes the enclosing
`Transaction` or permits insertion of another item.

After a successful `cleanup`, the core drops any untaken intent, then prepared
state, observation/predicate state, local state, and key, all outside
transaction locks and inside panic containment. A reusable worker-local item
box may retain its immutable typed resource binding, avoiding shared reference-
count writes when the next transaction uses the same binding. Retention is
bounded by that worker's peak ordinary item count and therefore by
`max_items_per_transaction`; independent terminal scratch can retain one
additional batch-wide binding. Bindings are disposed under panic containment
when slots are rebound or the worker is dropped. Each teardown step empties its
core slot before dropping the removed value. The enclosing reverse pass stops
on the first cleanup/destructor panic; it does not retry the callback or continue
destructing uncertain state. The remaining frame or scratch is quarantined and
the runtime is poisoned with the already-known outcome.

### D.4 Core-owned item state and operation-time entry

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

struct TransItem<A: TObject> {
    intent: Option<A::Intent>,
    preparation: PreparationState<A::Prepared>,
    observation: ObservationState<A::Observation, A::Predicate>,
    retained_predicate: Option<A::Predicate>,
    local: Option<A::Local>,
    key: Option<A::Key>,
    resource: Option<RegisteredResource<A>>,
}

pub enum ObservationRef<'a, A: TObject> {
    Unobserved,
    Read(&'a A::Observation),
    Predicate(&'a A::Predicate),
    UpgradedPredicate(&'a A::Observation),
}

impl<A: TObject> TransProxy<'_, A> {
    pub fn local(&self) -> &A::Local;

    pub fn local_mut(&mut self) -> &mut A::Local;

    pub fn observation(&self) -> ObservationRef<'_, A>;

    pub fn intent(&self) -> Option<&A::Intent>;

    pub fn intent_mut(&mut self) -> Option<&mut A::Intent>;

    pub fn add_read(
        &mut self,
        observation: A::Observation,
    ) -> Result<(), AccessError>;

    pub fn replace_read(
        &mut self,
        observation: A::Observation,
    ) -> Result<(), AccessError>;

    pub fn set_predicate(
        &mut self,
        predicate: A::Predicate,
    ) -> Result<(), AccessError>;

    pub fn add_write(&mut self, intent: A::Intent) -> Result<(), AccessError>;
}

```

The upgraded form moves the old predicate into a separate retained field so its
destructor cannot run while the commit lock plan is held. Observation and
predicate storage can then be emptied separately before destruction. Likewise,
a released `TransactionLock::Guard` is made inert by `release` but retained
until every physical lock has been released. Post-unlock teardown traverses
frames in reverse, taking and dropping each guard before its target. One
enclosing unwind boundary contains that reverse pass; the first destructor
panic stops traversal and quarantines the remaining state. The guarantee is
ordered, stop-on-first-failure teardown, not a separate catch frame per field.

`PreparationState::PreflightFreeRead` is a core-selected marker for an ordinary
`TransItem<A>` that used `PreflightFreeReadCapability`; it is not adapter-owned
prepared state. Terminal batches do not allocate `TransItem<A>` at all and
therefore do not add another state to this enum. Their homogeneous pooled
storage contains parallel typed key and observation vectors plus one retained
registered-resource binding and capability.

For retained participants, preparation is not a once-per-lifetime frozen intent.
Each incremental lock pass derives replacement `Prepared` state from the current
intent while reusing the existing core-held target. Operations remain permitted
only in `Collecting`; certification seals the frame. These transitions use the
same item representation and do not introduce a parallel participant record map.

In the implemented serializable profile, `TransProxy` borrows typed operation-time
fields from either an ordinary `TransItem<A>` or dense `TransItemData<A>` together
with its separate intent sidecar. It does not expose erased storage, and its
higher-ranked operation closure prevents it from escaping.
An opaque profile may replace that internal representation with a transaction
reference plus slot index so `add_read` and `set_predicate` can trigger
whole-transaction execution-time revalidation before exposing a result. That
change must preserve this public API. The adapter is responsible for same-item
operation composition and read-your-writes, using `local`, `observation`,
`intent`, and their mutable counterparts before replacing state with `add_write`.
The core performs successful predicate and installation transitions itself, so
an adapter cannot install twice or claim that a predicate was upgraded when its
callback failed.

`add_read` permits `Unobserved -> Read`; `set_predicate` permits
`Unobserved -> Predicate`; successful predicate upgrade is the only
`Predicate -> UpgradedPredicate` transition. Ordinary repeated reads retain
their original dependency. `replace_read` permits `Read -> Read` only when the
adapter proves that the successor accounts for this transaction's own change
without losing any foreign-change conflict; it cannot refresh away a stale read.
The optional intent is orthogonal, and `add_write` replaces an already-composed
intent. Illegal proxy transitions return
`AccessError::InvalidUse(IllegalItemState)` and MUST be propagated through the
enclosing operation. An outer access error or unwind dooms the transaction;
swallowing a proxy-method error inside an operation that returns `Ok` does not
itself latch doom.

### D.5 Canonical physical-lock trait

Logical callbacks acquire locks synchronously, but the core owns every guard.
The contract requires a `'static` guard rather than a borrowed `MutexGuard<'a, T>`; this
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
    pub fn new(identity: LockIdentity, target: Arc<L>) -> Self;
}

impl LockContext<'_> {
    pub fn try_lock<L: TransactionLock>(
        &mut self,
        request: LockRequest<L>,
    ) -> Result<LockUse<L>, PrepareError>;

    pub fn try_unique_lock<L: TransactionLock>(
        &mut self,
        request: LockRequest<L>,
    ) -> Result<LockUse<L>, PrepareError>;

    pub fn guard<L: TransactionLock>(
        &self,
        use_: &LockUse<L>,
    ) -> Result<&L::Guard, AdapterFault>;
}

impl PredicateContext<'_> {
    pub fn guard<L: TransactionLock>(
        &self,
        use_: &LockUse<L>,
    ) -> Result<&L::Guard, AdapterFault>;
}

impl ValidationContext<'_> {
    pub fn guard<L: TransactionLock>(
        &self,
        use_: &LockUse<L>,
    ) -> Result<&L::Guard, AdapterFault>;
}

impl InstallContext<'_> {
    pub fn guard_mut<L: TransactionLock>(
        &mut self,
        use_: &LockUse<L>,
    ) -> Result<&mut L::Guard, AdapterFault>;

    pub fn occ_commit_id(&self) -> Option<OccCommitId>;
    pub fn distributed_time_term(&self)
        -> Option<mako_clock::distributed::DistributedTimeTerm>;
    pub fn try_publish_unlock<L: TransactionLock>(
        &mut self,
        use_: &LockUse<L>,
    ) -> Result<bool, AdapterFault>;
}
```

The lock callback assigns each distinct request a private erased frame, reusing
compatible worker-pooled storage where possible; canonical duplicates share
the existing frame. Its inline `Option<L::Guard>` retains the synchronous
acquisition result. Physical lock
acquisition MUST be bounded and nonblocking and MUST NOT allocate. `Err` leaves
the lock unowned. A successful guard remains in core custody until release;
`release` makes it inert and does not fail or panic.

The canonical lane deduplicates full `LockIdentity` in acquisition order.
Duplicate requests must have the same lock target `TypeId` and canonical `Arc`;
a mismatch is `AdapterFault`, not an unchecked cast. The local unique lane
rejects duplicates under Appendix C.4's transaction-wide restriction.
`LockUse<L>` is unforgeable and tied to one plan nonce. Guard resolution checks
runtime, nonce, slot bounds, concrete frame type, and held state. Full identity
and canonical-target equality are established during planning, not recomputed
on every guard access. A mismatch during predicate upgrade or validation is a
pre-irrevocable `AdapterFault`; presenting
a stale token for the first time during `install` is an adapter-contract
violation that becomes indeterminate and quarantines the runtime, never an
unchecked cast or undefined behavior. A correct `Prepared` value retains only
the `LockUse` values returned by its current `LockContext`.

The private commit driver's boundary selects fallback disposition: before
irreversibility it abort-releases acquired guards; after `Irrevocable` it uses
`Indeterminate` while publication is uncertain. That release must conservatively
advance or preserve publication, never restore a pre-lock generation after
partial installation. The final pass releases remaining guards in reverse
acquisition order with `Committed`, skipping guards published during install.
If release panics, the uncertain guard and its frame remain
quarantined rather than losing the only remaining ownership witness.

### D.6 Phase capabilities and private erasure

Phase contexts have private constructors and fields and are `!Send + !Sync`.
Ordinary contexts and phase views carry scoped lifetimes; direct metadata
contexts are supplied by scoped borrow, with guard access carried separately by
`DirectLockRef`/`DirectLockMut`. Their capabilities are deliberately narrow:

- `LockContext` synchronously acquires/reuses typed guards; it is not a general
  scratch allocator;
- `AcquireContext` exposes the current `OwnerId`;
- `ExecutionCheckContext` reserves the deferred opacity revalidation seam;
- `PreflightFreeValidationContext` exposes final-certification metadata but no
  lock-plan or guard resolution, and is used by both prepared-free ordinary
  reads and terminal read batches;
- `PredicateContext` provides immutable access to already-held guards, before a
  core commit ID necessarily exists;
- `ValidationContext` provides immutable guards and core OCC metadata;
- `InstallContext` exists only with the private irreversible permit, provides
  mutable held guards and decision metadata, and mediates safe early publication;
- `ReleaseContext` supplies only the metadata needed to publish or abort a held
  lock; and
- `CleanupContext` marks post-unlock cleanup but exposes no public operation or
  lock capability; adapters retire their retained resources in `cleanup`.

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

`TObject` is intentionally not object-safe. Although another
public trait may be technically object-safe after fixing its associated types,
the API exposes no public adapter or lock trait objects. Internally,
`TransItem<A>` blanket-implements a sealed `ErasedItem` vtable, and a corresponding
sealed lock frame stores `L::Guard`. The transaction may therefore hold
heterogeneous items and locks, while `Any::downcast_ref`/`downcast_mut` checks
every transition. The erased traits and their constructors are private to
`sto-core`; external crates never implement them, name `Any`, or supply a
vtable. The base adapter protocol is safe. The optional compact direct-token
extension deliberately adds the unsafe public `DirectTokenLock` trait and
unsafe injective capability constructors. Their safety contract covers stable
target/token identity, token validation before dereference, and one-to-one
physical-lock mapping; it does not alter the safety of the base traits.

Private erasure in [`trans_item.rs`](../../crates/sto-core/src/trans_item.rs)
dispatches to the registered adapter with typed phase views and owns the item
state transitions. Its internal preparation helpers may still be named
`preflight`; that is not a public `TObject` callback. `PreflightItem`,
`PreflightFreeReadCapability`, and `PreflightFreeValidationContext` remain
valid public names. `FailurePhase::Preflight` and `FailurePhase::Finish` are
diagnostic phase names, not obsolete exported callback aliases.

### D.7 Callback phases and fallibility

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
    Capacity(CapacityError),
    Fault(AdapterFault),
}

pub enum CheckError {
    Conflict(Conflict),
    Fault(AdapterFault),
}

pub enum CleanupDisposition {
    Committed,
    Aborted,
}
```

| Invocation | Phase | Result contract | Core response |
| --- | --- | --- | --- |
| `TObject::new_local` | first typed lookup | `ItemInitError::{Capacity, Fault}` | Doom the transaction; poison on adapter fault. |
| adapter operation inside `with_item` | execution | outer `AccessError` or unwind | Mark the transaction doomed; no later commit. |
| terminal batch operation | execution | outer `AccessError`, missing `add_read`, or unwind | Consume/doom the open handle and definitely abort the retained prefix; poison on adapter fault. |
| `TObject::lock` | preparation and synchronous acquisition | `PrepareError::{Conflict, Capacity, Fault}` | Reverse-release acquired guards; definite abort for conflict/capacity, poison for adapter fault. |
| `TransactionLock::try_acquire` | locking | `AcquireError::{Conflict, Capacity, Fault}` | Reverse-release the acquired prefix; terminal version capacity exhausts runtime health before remaining abort teardown; poison on fault. |
| `revalidate_read` / `revalidate_predicate` | deferred execution-time opacity | `CheckError::{Conflict, Fault}` | Reserved contract; no supported opaque execution profile invokes this path today. |
| `TerminalReadBatchCapability::validate` | terminal final certification | `CheckError::{Conflict, Fault}` | Definite read-only abort for conflict; poison with a definite aborted outcome on fault. |
| `check_predicate` | predicate upgrade | `CheckError::{Conflict, Fault}` | Definite abort or poison; state changes only on success. |
| `check` | final validation | `CheckError::{Conflict, Fault}` | Definite abort or poison before irreversibility. |
| `CommitHook::reserve_upper_metadata` | after all write locks, before predicate/final checking | reject or contained panic | Rejection definitely aborts; a contained panic poisons with a definite aborted outcome. |
| `CommitHook::pre_install` | after final validation, before irreversibility | reject or contained panic | Same definite-abort boundary under the no-visible-effect contract. |
| `CommitHook::post_install` | after all installs, before final release | no `Result`; must not panic | Panic makes publication indeterminate; local datatype publication may already have happened. |
| `install` | irreversible install | no `Result`; must not panic | Panic yields `Indeterminate` and quarantine. |
| `TransactionLock::release` | reverse publication/unlock | no `Result`; must not panic | Retain/quarantine; report indeterminate unless publication is proved. |
| `cleanup` | post-unlock cleanup | no `Result`; must not panic | Poison with the already-known `DefiniteOutcome`; never invite retry. |

`ItemInitError`, `PrepareError`, `AcquireError`, and `CheckError` distinguish
ordinary capacity or conflict outcomes from `AdapterFault`; a fault is never
converted into a retryable conflict.
Installation, release, and cleanup have no error return because there is no safe
abstract rollback after `Irrevocable` and no remaining lock during cleanup that
can be reacquired out of order.

For every definite committed or aborted path whose lock callbacks all return,
including explicit abort and Drop, the core calls `cleanup` exactly once for
every inserted item unless the item entered an explicitly authorized
core-owned drop-only cleanup path. Terminal batches use drop-only cleanup on
both outcomes and never call `cleanup`. A direct capability may opt into
drop-only cleanup only after committed validation, installation, and release;
aborted direct items still call `cleanup` exactly once. If `cleanup` itself
violates the contract, the
already-known outcome is returned as `CommitFailure::Poisoned { outcome, .. }`;
the callback is not retried, and unfinished cleanup state is retained for
diagnosis. An unwind from acquire or abort-release leaves that callback frame's
guard state uncertain. Although no installation began and the abstract outcome
is definitely aborted, the core MUST retain the lock plan and item vector and
MUST NOT invoke the post-unlock `cleanup` callback. If publication becomes
indeterminate after irreversibility, the core likewise does not invent a
`CleanupDisposition::Committed` or `Aborted`: it uses indeterminate lock release,
quarantines the runtime, and retains the item vector as Appendix F.5 requires.

The execution-time `with_item` closure may run ordinary synchronous adapter
code and access its registered datatype, but it cannot `.await`, migrate the
worker, perform blocking I/O, or reenter the mutably borrowed transaction. No
lock-or-later callback may perform network or disk I/O, await, invoke
arbitrary application code, recursively enter STO on the same worker, or
acquire an untracked blocking lock. `install` MUST NOT run arbitrary or foreign
destructors, block, allocate fallibly, or perform cleanup whose latency or
effects are outside the adapter's audited publication protocol. Displaced
user-owned buffers, foreign allocations, and general cleanup obligations move
into preallocated state; `cleanup(CleanupDisposition::Committed)` releases or
retires them after all transaction locks are published and released. A
representation may perform a bounded internal reference-count decrement and
possible deallocation during install only when the referent contains no user
destructor or foreign resource and the adapter's install proof explicitly
accounts for that operation. The Masstree committed-state `ArcSwap` replacement
uses this narrow exception for its internal byte-only `SharedValue`.

### D.8 Normative adapter obligations

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
   canonical plan or an explicitly authorized checked-unique/direct plan under
   Appendix C.4. All acquisitions remain bounded and nonblocking, with exact-once
   reverse prefix release. Hidden internal locks need the same bounded
   acquisition and exact-once release proof; they cannot wait indefinitely
   while another lock is held.

Rust STO adds:

7. **Lifetime.** Every token and associated value is owned for the transaction
   and remains valid through validation and cleanup without an unguarded
   foreign pointer.
8. **Infallible installation.** Fallible acceptance and explicit resource
   reservation complete before install. No installation failure may become a
   retryable abort. Ordinary local adapters preallocate their publication
   representation; distributed history currently constructs immutable nodes
   during publication, so it does not inherit the local no-allocation claim.
9. **Panic freedom.** Resource, lock, validation, install, release, and cleanup
   callbacks must not panic. A contained pre-irrevocable panic guardedly aborts
   and poisons the runtime. Uncertain publication is indeterminate and
   quarantined; failure after proven publication poisons with a definite
   committed outcome instead.
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

### D.9 Deferred and eager updates

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

## Appendix E. Versions and the Rust memory model

### E.1 Version roles

The implementation distinguishes:

- `OccVersion`: a record or predicate validation value;
- `OccCommitId`: an ordered core commit value required by ordered writing bindings;
- `OwnerId`: the worker encoded in a held version lock;
- `DistributedTimeTerm`: checked Mako distributed BASE/decimal TERM, carried
  separately into install and history;
- cache HLC timestamps and upper persistence sequence values, including
  per-lane ordering.

These are distinct domains. The core uses explicit OCC identity types; upper
integrations must not reinterpret an OCC generation as an HLC or cache sequence.

### E.1.1 Opt-in record-local versions

`TObject::commit_version_requirement()` declares the version
policy of one registered binding. The default is
`CommitVersionRequirement::Ordered` for core reference datatypes.
`AdapterLocal` opts a binding into checked record-local generations; native
TPC-C FFI and `mako-engine-sto` tables explicitly choose this policy.
The requirement is captured at registration and cannot change during that
binding's lifetime.
All mutators sharing one physical version must use the same policy. Declaring
`AdapterLocal` does not change the default `AtomicVersion` implementation of
`TransactionLock`, whose committed release still requires an ordered ID. A
local adapter must deliberately use the local acquisition/release primitives
and report unordered observations.

Core folds the requirements of the transaction's **writing** bindings. It
reserves a global `OccCommitId` if any writer requires `Ordered`; an ordered
read-only binding does not require a ticket. The fold is per exact binding,
not per adapter type or the first binding in a typed batch. A transaction can
therefore write both an ordered and a local-version table implemented by the
same adapter type.

An all-local writing transaction has `CommitInfo::occ_commit_id() == None`.
This does **not** make it read-only. Lock acquisition, final validation, write
installation, all writing hooks, and release still depend on the actual
write set. `None` means no runtime-ordered commit ID was required.

The Masstree adapter opts in with
`TableConfig::with_record_local_versions(true)`. Each committed write changes
its locked record from generation `g` to `g + 1`. Generations may coincide
across different records and are not comparable with the runtime clock.
Every observation from such a table reports `ObservationOrder::Unordered`;
ordinary record and predicate validation still enforce strict serializability.
Opaque runtimes remain unsupported.

Mixed transactions keep each table in its own version domain. A local record
always advances by one, even when an ordered writer makes core supply
`Some(OccCommitId)`. Both successful and indeterminate local release ignore
that ordered ID. Otherwise a local record with generation 1,000 could fail
or regress when a mixed transaction receives global ID 2. Ordered tables
continue to require an ID strictly newer than their acquired generation.

Local acquisition checks the exact unlocked word and available generation
space **before** its CAS succeeds. The observed-acquisition form checks a
stale observation or busy lock first, so those remain conflicts rather than
false exhaustion. This check is also required for direct writes whose lock
acquisition certifies their write observation and skips another validation
callback. Generation exhaustion is detected before `pre_install` or any
write installation, not during an irreversible commit. Aborting restores the
acquired generation; committing publishes the checked successor with Release
ordering. Neither path permits wraparound or ABA reuse.

On `AcquireError::Capacity(VersionExhausted)`, acquisition first reverse-releases
the acquired prefix. The commit driver then marks the runtime `Exhausted` before
remaining abort teardown/cleanup and before returning the failure.
Transactions already overlapping that failure may complete. A subsequent
begin, attach, or commit start fails, including commit on a transaction created
earlier. A failure during abort cleanup retains the existing poison or
indeterminate handling. This contract does not promise that the health flag
changes before the first abort unlock. The cache facade reports terminal
`VersionExhausted`, not `TransactionTooLarge`, and runtime health makes the
database unready.

Two table-policy combinations are rejected: record-local versions with `trusted_scan_value_generation`, whose current
publication deduplication needs an ordered commit ID; and record-local
versions on `FixedU64Table`, whose implementation remains ordered. Ordinary
forward/reverse scans, RYW, tombstones, and the generic mixed-adapter fallback
remain supported on opted-in ordinary tables.

The `mako-engine-sto` cache facade and native TPC-C FFI opt tables into this
mode to match the default non-opaque C++ MassTrans path (`STO_SORT_WRITESET=0`,
`CONSISTENCY_CHECK=0`, opacity off). That path increments locked row versions
instead of allocating global `_TID` for every write. This does not alter cache
HLC, its validation gate, lane sequence allocation,
commit-record bytes, recovery floor, replay ordering, or log GC. Persisted
records still carry `MakoTimestamp`, never a record-local OCC generation.

### E.2 Legacy parity and native requirements

[`TransactionTid`](../../src/mako/sto/Interface.hh) defines the current C++
layout, which remains a compatibility oracle:

| Field | Mask/value | Meaning |
| --- | ---: | --- |
| owner ID | `0x01ff` | Nine-bit legacy lock owner; values 0–511 are representable. |
| lock | `0x0200` | Resource is being installed or validated by a writer. |
| nonopaque | `0x0400` | Observation is unordered and forces full opacity revalidation. |
| compatibility-reserved | `0x0800` | Legacy adapter user/invalid bit; reserved from the generic native layout. |
| version increment | `0x4000` | Leaves low helper bits clear. |

Native Rust records do not share version words with C++. The implemented
`AtomicVersion` packs a nonzero 48-bit generation into the high bits and a
16-bit `OwnerId + 1` tag into the low bits. Tag zero means unlocked;
`OccVersion::MAX_VALUE` is `2^48 - 1`, and the initial generation is 1.
`OwnerId::MAX_VALUE` is 65,534, permitting 65,535 representable owners. The
core's default worker limit is 512; native Masstree and upper engine admission
limits are separate. Default item and lock limits are each 4,096.

There is no encoded unordered bit. `OpacityToken`/`ObservationOrder` carries
that semantic distinction. Generic APIs keep the packed word opaque, recognize
self-owned locks, and check owner/generation capacity. Adapter liveness remains
separate typed metadata.

Version arithmetic never wraps or reuses a still-observable generation.
Terminal exhaustion prevents new work; restart is the available operational
response. A proved quiescent rebase is a future protocol, not a currently
implemented recovery API.

### E.3 Atomicity does not make payload access safe

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

### E.4 Baseline ordering table

The first implementation favors an auditable ordering baseline. Weaker
orderings require a written happens-before argument plus Loom or equivalent
tests.

| Event | Baseline ordering | Required effect |
| --- | --- | --- |
| Load an unlocked version before a committed value | `Acquire` | Observe state published before the version. |
| Load packed descriptor and atomic payload words | `Acquire` | Copy only the representation and length named by the descriptor. |
| Acquire a shared fallback lease | `ArcSwapOption::load_full` or `load` | Retain an owned `Arc` for `Value` snapshots or an operation-scoped ArcSwap guard for byte visitors/copies. |
| Reload version after the value load | `Acquire` | Detect intervening lock/install. |
| Acquire version lock with CAS | `AcqRel` success, `Acquire` failure | Own mutation and observe prior state. |
| Publish a prepared value representation under own lock | `Release` | Order initialized atomic words or the shared allocation before commit-unlock. |
| Advance generation while retaining own lock | `Release` | Keep the resource unavailable until publication completes. |
| Commit-unlock with the new generation | `Release` | Atomically make the installed state available to acquiring readers. |
| Abort-unlock to original version | `Release` | Release ownership without publishing a write. |
| Proposed opacity-bound load (deferred) | `Acquire` | Order clock observations if the opaque protocol is implemented. |
| Advance runtime commit clock | `AcqRel` | Produce a unique ordered commit value. |

Rust and C++ atomics are not assumed to have interoperable object layout.
Masstree stores only a nonzero scalar directory token. In the general lane that
token is a logical `RecordId`; in a private direct table it is the exposed
address of a stable Rust registry entry. C++ stores, copies, and compares that
scalar but never dereferences or prefetches it as a pointer. Every record atomic
is owned and accessed by Rust.

## Appendix F. Commit, abort, and failure state machine

### F.1 States

```text
Active
  -> DatatypeLockingAndPreparation
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
  -> OptionalPostInstallPublication
       | contained panic -> Indeterminate
       v
  -> PublishingAndUnlocking
  -> Committed
  -> Finished

Any other failure before Irrevocable -> Aborting -> Aborted -> Finished
Any uncontained failure at/after Irrevocable -> Poisoned/Indeterminate
```

The retained participant exposes `Validated`, but this is an in-memory sealed
frame, not a durable prepared-record or participant-recovery promise.

### F.2 Commit phases

The normative ordinary local phases are those in §3.3, implemented once in
[`ordinary_protocol.rs`](../../crates/sto-core/src/ordinary_protocol.rs).
`TObject::lock` prepares and acquires in access order. A failure during a later
callback may therefore occur with earlier guards held: core abort-releases
them in reverse order before definite-outcome cleanup. Ordinary conflict or
capacity aborts; terminal version exhaustion also exhausts runtime health.
Adapter faults poison, and uncertain acquisition/release ownership quarantines
instead of pretending cleanup is safe.

After locking, `reserve_upper_metadata` precedes predicate upgrade. Ordered
OCC metadata, when needed, precedes final read checking. All-local writers omit
the ticket, not hooks or certification. The local cache's gate/HLC sequencing
is specified in Appendix M.2 and remains separate from distributed allocation.
Final checks certify ordinary reads and upgraded predicates against monotone
covering versions; a prepared-free read substitutes its restricted check.
The pre-install hook is the last fallible acceptance point.

Core marks `Irrevocable` before invoking any install. Each installed item must
have its protecting lock held, but an earlier independent local cell/counter
may already have published/unlocked. `try_publish_unlock` refuses early release
for aliases and for retained participants. Masstree and direct plans retain
their locks through all installs. Thus a hook requiring coupled publication
before any conflicting writer must use a retain-through-install datatype
profile, as the table/cache integrations do; a generic hook cannot assume all
datatypes retain locks.

Post-install publication is infallible by contract and cannot reject. Failure
after irreversibility is indeterminate, never a retryable abort. The final
release pass publishes/unlocks only still-held guards in reverse acquisition
order; early-released guards remain core-owned and are not released twice.
Then ordinary items receive `cleanup`, or an expressly authorized capability
uses committed drop-only teardown. Cleanup failure after proven publication
reports poisoned with a definite committed outcome.

Retained participants call the same lock/certify/install/finish routines.
`lock_writes` may repeat while collecting; `validate` performs the final lock
pass and seals operations. `install(decision)` adopts typed distributed metadata
without allocating a timestamp and forces lock retention until `finish`.
An installed participant's abort/Drop resolves committed cleanup, not rollback.


#### Prepared-free ordinary reads

An adapter may explicitly expose a stable
`PreflightFreeReadCapability<Self>` for an ordinary `Read` item that has no
intent or predicate. This capability skips only that item's adapter `lock`
and `Prepared` allocation. Other heterogeneous items still acquire normally
in access order, and the prepared-free read is certified in the same final
validation pass and
at the same certification cut described above. Its capability callback must
therefore implement final certification, not merely execution-time opacity or
an advisory recheck.

`PreflightFreeReadCapability::new` supplies an explicit committed-cleanup
callback without a `Prepared` value. `new_drop_only` is a stronger adapter
promise: after successful certification, dropping the core-owned item fields is
the complete cleanup, so no adapter committed-cleanup callback runs. On every
definite abort both forms instead use the ordinary adapter `cleanup` callback
with `prepared == None` and `CleanupDisposition::Aborted`. Capability discovery,
validation, optional committed cleanup, ordinary abort cleanup, and teardown all
remain inside the existing per-item panic/fault boundaries. Returning a
capability is a stable adapter contract for the lifetime of an item; callbacks
must be nonpanicking and must preserve the same cleanup obligations as the
ordinary path. Items with an intent, a predicate, or no observation always take
the ordinary datatype-lock path.

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
  -> BorrowedInjectiveLockCommitCapability<A, L, O>
       retains one compact token per write; unsafe injectivity proof;
       optional exact-observation certification during token acquisition
       L: DirectTokenLock (unsafe trait)
       O: BorrowedTokenCommitOperations<A, L> (concrete operation provider)
```

The compact-token form keeps preparation, validation, and installation in a
concrete `BorrowedTokenCommitOperations` implementation after the erased plan
entry. This permits specialization without moving guard ownership, failure
recovery, or certification out of core.

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
`lock`, `check`, or `install` methods for that attempt. It does
retain `A::Local`, observations, intents, keys, and exact registered bindings.
By default it calls `A::cleanup` with `prepared == None` on every definite
outcome. `with_drop_only_committed_cleanup` may replace only the committed
cleanup callback with reverse core-owned field teardown after all publication
and release complete. Aborts still call `cleanup`; indeterminate attempts remain
quarantined without invented cleanup.

An eligible singleton crosses the `TypedTransItemBatch<A>` dispatch once and the
concrete plan entry once, then executes shared phases with static plan dispatch.
Compact-token singletons additionally borrow one `DirectSingletonItem` and its
exact lock frame through `singleton_direct::SingletonBody`, avoiding the larger
batch's item walk. Larger batches retain the general shared driver. The route
changes representation, not eligibility, certification, guard ownership, or
publication semantics.

`DirectCommitContext::run_body` contains acquire through release in one unwind
boundary, updates phase state before each step, and retains separate hook
containment. The commit driver owns the boxed frame, acquisition/irreversibility
state, and cold cleanup handlers. Caught panic payloads return to that driver
and are destroyed after cleanup, including the case where a payload destructor
also panics. This guarantees neither panic containment in abort builds nor
allocation-free execution.

#### Terminal homogeneous read batches

The terminal path is a stronger, transaction-wide specialization and does not
enter the general `TransItem` commit pipeline. Before recording the batch, an
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
hook, irreversible install, publication, or adapter `cleanup` callback. Core
then drops observations and keys in reverse order and recycles the worker-local
homogeneous allocation. A destructor panic after certification poisons with a
definite committed outcome; before certification it poisons with a definite
aborted outcome. The terminal protocol therefore preserves the same
certification argument as an ordinary read-only transaction while removing
state that its consuming typestate proves unreachable.

### F.3 Serialization and irreversible points

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
installations may occur sequentially. A concurrent attempt seeing part of a
multi-item update cannot certify incompatible observations: remaining locks or
advanced non-ABA versions reject them. Exclusive local cell/counter publication
need not wait for independent installs, but aliases defer. The transaction
returns only after required publication and cleanup finish.

For a read-only transaction, v1 defines the same cut just before its final
validation pass and linearizes there if the whole pass succeeds. A paper-style
opaque read-only fast path may be added only with a proof that execution-time
validation already supplies the same guarantee.

### F.4 Abort and Drop

Abort performs, exactly once:

1. reverse `TransactionLock::release(LockDisposition::Aborted)` for every
   acquired lock;
2. reverse `TObject::cleanup(CleanupDisposition::Aborted)` for
   every ordinary or direct item; a terminal read handle instead performs its
   authorized reverse drop-only teardown;
3. resource-scope exit; and
4. worker transaction-state reset.

Those steps describe callbacks that honor their infallible contract. If an
acquire or abort-release callback unwinds, its physical guard is uncertain;
the runtime is poisoned, other definitely held guards are abort-released when
possible, and item state is quarantined without calling post-unlock `cleanup`.
The abstract outcome remains definitely aborted because installation never
started.

Dropping `Transaction<Active>` invokes this path. `commit(self)` first transfers
the boxed transaction frame and exclusive worker borrow to `CommitDriver`,
disarming public Drop before commit locks exist. The driver subsequently owns
the ordinary lock plan or the typed batch's direct plan. Its private
`CommitBoundary::{Reversible, Irrevocable, Published}` and progress state select
definite abort, indeterminate handling, or definite committed poisoning.
Ownership transfer prevents public Drop from abort-releasing an irrevocable
transaction; the driver's state tracks acquisition, release, and cleanup so
partially completed work is not repeated.

If abort cleanup violates its infallible contract, the abstract outcome remains
definitely aborted, but the runtime is poisoned. A fallible commit call reports
`CommitFailure::Poisoned { outcome: DefiniteOutcome::Aborted(..), .. }`.
Ordinary `Transaction::abort` and Drop only record that health failure;
`Participant::abort`/`finish` can return `CommitFailure`. Installed participant
termination is committed cleanup, not this reversible abort path.

### F.5 Failure after installation starts

Installation and mandatory cleanup are designed to be infallible. With
unwinding enabled, a caught adapter panic or detected invariant violation after
`Irrevocable` cannot be translated into `Conflict` or `Aborted`. The core
quarantines uncertain item/guard state and the affected runtime, returning
`Indeterminate` when publication cannot be classified. If publication is proven
complete and only later teardown fails, it returns `CommitFailure::Poisoned`
carrying `DefiniteOutcome::Committed`; callers MUST NOT retry it.

While publication is uncertain, still-held guards are released only with
`LockDisposition::Indeterminate`, which conservatively advances their generation
or preserves publication; it never restores a pre-lock version after partial
installation. Unreleasable guards remain quarantined. Guards proven released
may undergo contained inert teardown, while uncertain item state remains
retained; quarantine does not require leaking every successfully released target.
A process abort, including a panic under `panic=abort`, cannot return a Rust
failure outcome. The workspace release default uses `panic=abort`; unwind-based
containment applies only to builds that enable unwinding.

If an accepted hook staged upper metadata and installation later becomes
poisoned or indeterminate, recovery of that staged metadata belongs to the upper
layer and MUST use the same indeterminate disposition. A completed post-install
callback has crossed the same irreversible boundary as datatype installation.
`sto-core` does not attempt an external rollback.

No Rust panic or C++ exception may cross the C ABI.

## Appendix G. Predicates, commutativity, and absence

### G.1 Logical segmentation

The datatype specification determines concurrency. An adapter SHOULD allocate
separate logical resources for operations that commute and share resources for
operations that do not. Return values matter: an insert that returns a global
size observes more state, and therefore conflicts more broadly, than an insert
that returns only per-key success.

Adapters MUST document their abstract operations, return values, conflict
units, and covering versions before optimization work begins.

### G.2 Commutative writes

An operation that does not expose prior state need not record a read. For
example, a counter can accumulate a delta and serialize only during install;
concurrent increments then need not abort simply because they both update the
counter.

The implemented core supports semantic write intents; `TxnCounter` stages
wrapping deltas under ordinary exclusive commit ownership. A multi-owner
`CommutativeVersion` is not implemented. It requires a separately justified
protocol and workload evidence, not merely completion of the basic OCC path.

### G.3 Optimistic predicates

A predicate is an owned adapter token representing a necessary commit condition,
not an arbitrary application closure. During execution, the adapter evaluates
the predicate against a sound snapshot. During commit, `check_predicate`:

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

### G.4 Missing keys

An unsuccessful lookup is an observation, not the absence of one. The adapter
MUST record a witness changed by any insertion that could change the result.
The paper uses bucket, neighbor, or Masstree leaf versions. The Rust Masstree
adapter instead interns a stable tombstone record for a point miss and observes
that record's version.

### G.5 Range predicates

A range scan must detect both a newly published directory entry and a liveness
change to a physical record already in its bounds. Ordinary nontrivial scans
record one coarse physical-directory generation and an observation for every
traversed record, including tombstones, while overlaying staged state. They
retain shared structural admission across generation sampling and traversal
unless the directory has been permanently sealed. Every admitted
tombstone-interning attempt advances the directory generation under exclusive
structural admission before native publication can become visible.

The ordinary directory observation is unordered and prepared-free, with no
transactional intent or lock. Existing tombstone resurrection, live removal,
and value replacement are covered by record observations rather than a
physical-structure change. A foreign physical publication anywhere in the table
invalidates the earlier directory witness, even outside the requested bounds
or when the inserting transaction later aborts.

The optional trusted direct-table scan instead retains one table-wide
scan-generation observation and sound per-row OCC snapshots, as specified in
[J.6](#j6-scans). Successful **scalar** interning repairs an earlier directory
or trusted-scan observation by exactly its own increment through
`TransProxy::replace_read`. It never resamples away foreign increments.
General fixed-batch miss paths do not perform that repair and can
conservatively self-conflict after an earlier scan. These are deliberately
different scan/access profiles, not a universal no-self-conflict guarantee or
a claim of MassTrans-equivalent precision.

## Appendix H. Opacity

**[DEFERRED]** This appendix describes a retained design requirement, not an
implemented execution profile. `Runtime::new` rejects an opaque default;
`WorkerContext` rejects opaque ordinary and terminal begins. The upper cache
likewise rejects `Isolation::Opaque` with `MissingOpacity`.

### H.1 Implemented non-opaque profile

Serializable transactions record sound individual observations and perform
final certification. They do not maintain a whole-transaction snapshot during
execution. `TransProxy::add_read` and `set_predicate` update local item state;
they do not secretly revalidate the transaction's earlier observations.

An aborting transaction may therefore observe values from incompatible logical
states. It must still execute memory-safely: snapshot reconstruction, object
lifetime, and Rust value validity cannot depend on later successful validation.
No opacity-based read-only commit elision is implemented. Prepared-free reads
and terminal batches still perform final certification.

### H.2 Proposed opaque execution protocol

The paper combines an execution bound with validation of earlier reads when a
new observation cannot be certified against that bound. An implementation of
`IsolationMode::Opaque` would need all of the following before becoming a
supported capability:

1. Initialize a transaction bound from a checked runtime clock and retain enough
   coverage state to explain every result already returned to the body.
2. Obtain each new result through a sound payload snapshot, not a racy read.
3. Accept an ordered observation against the existing bound only when its
   version and lock state prove it belongs to that coherent prefix.
4. Otherwise revalidate all relevant earlier read and predicate observations
   against a suitable new bound, or abort before exposing the new result.
5. Treat unordered observations conservatively; never compare record-local
   generations as though they were values in one global clock.
6. Preserve read-your-writes, predicate semantics, and whole-prefix coverage
   across heterogeneous adapters, scans, exceptions, and all read specializations.
7. Prove any read-only commit shortcut separately from the ordinary final-pass
   protocol and test it against an opacity-aware history oracle.

The reserved `OpacityToken`, `ObservationOrder`, `ExecutionCheckContext`, and
revalidation callbacks identify this future seam. They do not mean a checked-
through bound is currently advanced by execution. Neither public enum presence
nor an adapter implementing these callbacks is sufficient to advertise opacity.

### H.3 Panics in transaction bodies

With unwind enabled, a panic in ordinary Rust transaction-body code triggers
the active transaction's Drop abort before unwinding continues. A panic MUST
NOT cross into C++. With `panic=abort`, no cleanup guarantee is possible; such a
build profile is unsuitable for claims of recoverable transaction-body panic.

Adapter commit callbacks are required to be panic-free regardless of profile.

### H.4 Version and integration admission

Adapter-local versions explicitly report `ObservationOrder::Unordered` and are
currently supported only in the serializable profile. Enabling opacity cannot
silently reinterpret them as ordered IDs, add an unreviewed clock load, or drop
unsupported adapters from the transaction. A future capability must specify
which adapters, direct plans, scans, hooks, and upper interfaces it supports.

The core, adapter, and upper feature advertisement must agree. Until that
protocol and its gates exist, every opaque request fails explicitly and the
non-opaque guarantees remain unchanged.

## Appendix I. Lifetimes and reclamation

### I.1 General rule

Every object reachable from an observation, predicate, write intent, or lock
token MUST stay alive until the transaction finishes. A reference that was valid
during execution but freed before validation invalidates the entire protocol.

The C++ paper implementation makes a transaction one RCU read-side critical
section. Rust STO does not impose one universal reclamation system; each adapter
must register and own the required resource scope explicitly.

### I.2 Rust-owned state

The core prefers owned snapshots, `Arc` handles, stable append-only registries,
and guards whose lifetime is tied to the transaction. Raw pointers are not
logical identities. If an adapter uses epoch or hazard reclamation, the guard
must be represented in transaction state and released on every outcome.

The worker-owned frame and typed pools can outlive an attempt. Idle general
slots may retain a resource binding per peak slot, and the independent terminal
pool may retain one additional binding. Rebinding or dropping the worker
releases those leases; destructor failure quarantines the affected pool.
Transaction completion therefore does not promise immediate object destruction.

### I.3 Foreign RCU

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
Structural admission remains operation-local for worker-wide retention; a
tree-bound point scope retains it across its reads. Permanently sealed trees
bypass reader admission while retaining native RCU protection.

Both capabilities use generation-tagged, implementation-owned owner cookies
and are mutually exclusive on one worker. Their shared POD representation does
not make their tokens interchangeable: distinct native TLS owner identities
reject cross-family use. Callers MUST keep either scope synchronous and SHOULD
keep it short. Ordinary scope callers must not hold it across I/O or blocking
waits. The retained Mako participant explicitly extends its worker scope across
synchronous protocol waiting on the owning lane, with the associated retention
cost; it still forbids `.await`, thread migration and reentrant native work.
Scopes may span caller-owned buffer growth and are not allocation-free or
hard duration-bounded. Datatype/visitor callbacks retain their no-blocking rule.

Repeated fixed-width calls SHOULD use the strided batch or explicit point-read
scope. Transaction-shaped multi-tree access SHOULD use the worker RCU scope.
Implicit unowned pins, raw RCU-implementation guards, and cross-worker scope
transfer remain forbidden.

### I.4 Reclamation policy

Published Masstree directory entries and published or publication-unknown Rust
records are append-only in v1. Their consumed registry slots and logical IDs
are never reused. A candidate
proved never to have entered the directory may release directory-reachable
record and key-byte quota, but its internal numeric slot ID remains consumed.
The private direct-directory mode may publish the stable slot address rather
than that numeric ID; the same no-move and no-reuse rule applies. An
initialized candidate retains its slot/box and structural allocation charge
in the current registry, even when nonpublication releases retained quotas. Consumed-ID limits and the structural allocation budget
both constrain retained slot storage. Logical deletion installs a tombstone;
it does not free the record or remove the directory key. These rules apply
during live runtime operation. Rust registry allocations can be freed when
their last legitimate owner/lease drops, but native tree/root allocations and
worker registrations remain process-lifetime. Successful native whole-runtime
shutdown is not an implemented reclamation path.

Distributed payload-history pruning is separate: an authoritative monotonic
floor can drop immutable predecessor tails after snapshots release ownership.
It neither deletes native keys nor reuses registry IDs or worker slots
(Appendix M.3).

Physical reclamation requires all of:

- conditional directory removal tied to the expected scalar directory token;
- proof that no transaction can resolve or retain the record;
- an explicit native grace-period barrier;
- a real worker unregister/shutdown model; and
- clear allocator ownership.

Until those exist, memory growth is explicit technical debt that benchmarks and
operational limits MUST measure.

## Appendix J. Masstree transactional adapter

### J.1 Ownership split

Masstree is an append-only ordered directory:

```text
binary key  ->  nonzero 64-bit directory token
                standard table: monotonic numeric RecordId
                private direct table: stable RegistryEntry address
```

C++ owns tree nodes, traversal, splits, allocation, and native RCU. Rust owns
the record registry, record locks/versions, atomic inline and stable-cell
storage, shared-value publication, tombstones, transaction items, the
physical-directory generation, and values.

`Table` owns three registered resources for record, directory, and scan roles.
Their adapters retain `Arc<TableShared>`, which owns the native directory
facade, record registry, generations, and admission state. Active items,
reusable resource bindings, and lock-target owners keep the relevant Rust state
alive through their last access.

`ResolvedRecord` is a copyable runtime/object/token identifier, not an owning
lease. It is used through a still-reachable table; wrong-runtime/table access
is rejected before interpreting an address token. The registry and its native
directory binding cannot be independently substituted while the table is live.

Masstree MUST use a dedicated integral `uint64_t` value type with value prefetch
disabled. C++ MUST NOT reinterpret, dereference, or pointer-prefetch either
token mode. Address reconstruction belongs only to the audited Rust direct-table
module after the private ownership invariant has been established.

### J.2 Record registry

The raw ABI's `mtree_sys::RecordId` is `u64`, with zero denoting directory
absence; the safe `masstree::RecordId` wrapper contains a `NonZeroU64`.
`Table::new` stores a monotonically allocated numeric ID in
`RecordTokenMode::RegistryId` mode. `RegistryId` is a mode variant, not another
public identifier type. `Table::new_direct` creates and retains a fresh private
directory and stores a stable registry-entry address in the same scalar wrapper.
C++ never uses it as a pointer. Safe code cannot publish through another handle,
so every direct token was minted from this exact registry and remains valid
while `TableShared` is retained.

The registry still consumes an internal numeric slot ID monotonically in both
modes. IDs and slots are never reused in v1. The allocatable domain is bounded
by the minimum of the configured consumed-ID limit, the registry-addressable
slot domain, and `u64::MAX`; scalar ABI support for every nonzero `u64` does not
imply the registry can allocate every value. The current addressable numeric
domain is capped at `isize::MAX`. Exhaustion ends the current attempt with a
capacity error before native publication of the rejected candidate. It does
not require process termination. Wrapping to zero or aliasing any consumed slot
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
`ProvenUnpublished` and retained-quota release. Current initialized slots/boxes
and their structural charges remain retained; quota release is not allocation
rollback. A success that chose the candidate becomes `Published`. Any ambiguous
native error becomes `PublicationUnknown`:
the ready record and its quota remain retained permanently, its ID is not
reused, and the table is poisoned or quarantined before further access.

Directory-token construction and registry resolution remain private to
`sto-masstree`. Safe callers cannot resolve guessed IDs or addresses or access
proven-unpublished/publication-unknown slots outside quarantine diagnostics.

The implemented append-only registry keeps resolved storage stable while it
grows. The default
`RegistryLayout::LazySegmented` has a small fixed root of sparse directory
buckets. Bucket `b` covers `2^b` segment positions starting at `2^b - 1`; both
the bucket's segment-pointer array and each 1,024-entry `RegistrySegment` are
allocated only when needed. Construction does not allocate one directory cell
for every configured segment. Thus a large numeric quota does not reserve a
proportional outer directory at startup. A per-table growth mutex serializes
first allocation; existing-record resolution requires no growth mutex.

Buckets and segments publish through `OnceLock`s and remain at stable
addresses. Standard segments eagerly initialize their 64-byte entries.
Bounded-value segments instead retain a directory of per-slot `OnceLock`s;
the claiming thread allocates and first-touches each 192-byte extended entry
in its own box. This preserves allocator locality rather than interleaving
different workers' mutable records in one allocation. Ownership remains
cross-thread: the arena retains every box, and lock targets retain the arena.
Initialization shares the growth mutex, including batch preflight, so racing
claims cannot temporarily double-charge the same slot.

The consumed-ID CAS grants exclusive claim ownership, followed by
`UNALLOCATED -> RESERVED -> READY` publication. Batch preflight initializes its
entire range before that CAS. Both `Ready` and `Published` resolution borrow
the same record address, preserving the race in which the native directory
exposes a winner before its inserter records `Published`. Direct tokens still
address the record itself; they do not add a per-transaction directory lookup.

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
retained-record quota alone therefore cannot bound physical arena memory.
A publication-unknown candidate retains both quotas. A deployment may
configure very large numeric limits, but MUST select an explicit allocation
budget for growth driven by misses or new keys.

`RegistryBudget` is shared across tables by cloning its accounting handle.
`Table::new_with_budget` selects it for the registry-ID lane;
`Table::new_direct_with_budget` selects it for a private direct-token table.
The existing constructors retain their finite numeric quota defaults and use
an unrestricted structural budget. Callers that enable large numeric quotas
should use one of the explicit-budget constructors. General
`new_direct_with_budget` admits registry configuration/budget before creating
its private native tree; `new_direct` delegates to it with a `u64::MAX`
structural budget and inherits the ordering, not a finite operational byte cap.
`new_with_budget` receives an already-created tree. `FixedU64Table::new` creates
its tree before eager registry-capacity admission and does not share this
budget. The pre-native rejection guarantee must not be applied to all constructors.
Charges are atomically reserved before allocation and released only when the
allocation's last owner drops. Each lazy segment reserves its directory/arena,
pointer array, and lock targets together, then divides that reservation among
the constructed owners. Bounded-value record boxes are separately admitted on
claim and transfer their charges into the arena owner without releasing any
budget. A failed allocation unwinds its temporary charge; an initialized
directory can remain allocated for a later retry. Directory buckets have
separate, infrequent reservations.

The budget includes sparse directory arrays, concrete record storage,
per-slot ownership cells for worker-local extended entries, arena ownership
allocations, record-lock targets, and their pointer arrays. Detached lock
targets retain the arena and all its accumulated charges, so dropping the table
handle cannot report still-reachable storage as freed. Numeric-ID consumption
and retained-record/key quotas keep their existing failure semantics.

This is a structural registry budget, not a Rust heap or process memory limit.
It excludes separately allocated variable-size key/value payloads, fixed table
and budget control objects, dense resolved caches, transaction/worker scratch,
allocator overhead, native Masstree allocations, and process RSS. Inline
payload fields are included in the charged record stride. Deployments MUST
measure these other allocations separately. Numeric retained-record,
consumed-ID, and retained-key-byte quotas remain independent limits, and their
counters do not represent interchangeable memory measurements.

### J.3 Record representation

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
    Distributed(DistributedVersion),
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

Before distributed metadata is present, values through 38 bytes are copied
inline in transaction-local and committed state. This covers the historical
TPC-C new-order, stock, order-line, and secondary-index payload sizes without
payload allocation/refcount traffic. The committed copy uses four full atomic
head words and 48 tail bits; the high 16 bits hold the descriptor. All accesses
use the complete 64-bit atomic, never a mixed-size alias. Decision-bearing
history uses `SharedValue::Distributed` even for small rows, so the bootstrap
allocation claim is not a distributed steady-state guarantee.

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

The distributed history variant uses that same embedded shared slot for payload,
metadata and predecessor ownership, including tombstones. Ordinary and direct
install both preserve it. Retention-floor pruning and fail-closed metadata
requirements are in Appendix M.3; the fixed-`u64` specialization does not supply
this distributed-history profile.

On the supported 64-bit layout, `Value`, `RecordState`, and `TableIntent` are 40
bytes; `Arc<SharedValue>` and `ArcSwapOption<SharedValue>` are eight bytes; the
four head words, packed tail/descriptor word, and embedded ArcSwap keep
`CommittedRecordState` at 48 bytes. The enclosing `Record` remains 56 bytes,
and the registry publication byte plus seven explicit padding bytes produces a
64-byte-aligned, 64-byte `RegistryEntry` stride. `StableAtomicValueCell` is 128
bytes, so `StableRegistryEntry` is 192 bytes with 64-byte alignment; its base is
at offset zero and its cell is at offset 64. The eager registry `max_bytes`
accounts for the selected concrete stride. Standard lazy segments remain
contiguous; bounded-value lazy segments own separately allocated extended
entries through 16-byte ownership cells on this ABI. Partly occupied segments
allocate only the claimed extended records. The suffix remains adjacent to its
own record; there is no separate bounded-payload directory.

Registry-ID writers use separately stored `Arc<RecordLockSegment>` targets,
each covering 16 records. A direct table instead uses `TableShared`: ordinary
fallback `lock` retains that Arc in the plan, while the homogeneous direct
plan uses the binding-retained target and a compact exact-record token. In every
lane the held guard names the exact inline `AtomicVersion`, and its target or
binding keeps record storage alive through release. Point reads need neither
an owning record-Arc clone nor a separately allocated per-read lock object.

The transaction-local record metadata is deliberately smaller than a value:

```rust
struct RecordObservation {
    version: OccVersion,
    original_live: bool,
    old_was_shared: bool,
    record_local: bool,
}
```

Transaction-local state is only an `AdapterRole` tag distinguishing record,
directory, and trusted-scan items. Operations resolve record state through the
stable registry. Ordinary writer `lock` retains the mode-specific lock
target described above; direct preparation retains its compact token separately.
No registry or lock-target pointer is retained in an item local.

On the supported 64-bit layout, `AdapterRole` and `Option<AdapterRole>` are one
byte, `RecordObservation` and the containing `TableObservation` are 16 bytes,
and the erased `TablePrepared` lock token is 24 bytes. An operation copies an
inline or stable-cell value into its own `Value` or output buffer. Shared
snapshots retain an owned `Arc`; byte visitors and copy APIs retain an
operation-scoped ArcSwap guard instead.

Shared fallback values use the eight-byte ArcSwap embedded in their exact
record. Reads and writes therefore need neither an ID-to-value side lookup nor
a hash or table-wide map lock. Replacing a shared value swaps the new Arc into
that slot; removing, shrinking, or moving it to bounded atomic storage clears
the slot. A reader's already-acquired Arc or ArcSwap guard keeps the old
allocation alive independently.

The exact binary key is owned once by the append-only directory. A successful
lookup returns a table-bound registry ID or private direct address token.
`new_direct` creates an unexposed directory, so callers cannot publish through
another handle. `Table::new` instead accepts a clonable `Tree` and documents
fresh, exclusive semantic ownership; its type signature does not prevent a
caller retaining a clone. That caller must not inject foreign bindings or pair
the tree with another registry. Under the corresponding ownership contract,
repeating every key in `Record` would duplicate the directory binding rather
than improve the conflict proof. Likewise, the per-record
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
only this registry mints directory values, arena-owned allocations are append-only
and stable, slots never move or are reused, and the transaction item and lock
target retain `TableShared` through the last guard access. The `repr(C)` layout
keeps the base at offset zero and the cell at offset 64; separately allocated
extended records need not be adjacent. Growth and detached-guard tests exercise
record identity, value preservation, and retained allocation accounting.

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

The large snapshot has a separate reference-safety proof. After observing the
shared descriptor, owned `Value` snapshots use the record's embedded
`ArcSwapOption::load_full` to acquire an `Arc<SharedValue>`. Byte visitors and
copy APIs use `load` and retain its operation-scoped guard. Both close the OCC
version sandwich before exposing bytes; visitor borrows cannot escape the
invocation. A writer holds the record version exclusively, publishes the
updating descriptor, swaps or clears the Arc, and publishes the final descriptor
before Release-unlocking the version. Either lease keeps the old allocation
alive across replacement. A reader overlapping replacement rejects its version
sandwich.
A delayed reader that sampled the old shared descriptor
may find a cleared slot, but the version check is classified first and makes
that race a retryable conflict. A missing payload under the same stable
unlocked generation, or an interrupted publication, fails closed and poisons
the table. Dropping an incomplete publication guard Release-publishes the
poisoned descriptor through the complete packed word.

The embedded ArcSwap is atomic reference publication, not a second
transactional lock. It introduces no hidden wait-for edge during OCC install.
Publishing a replacement swaps or clears one internal `Arc<SharedValue>` and
may drop the displaced reference while the record lock is held. This is the
bounded internal exception from Appendix D.7: `SharedValue` owns bytes only and
has no user callback or foreign resource, so the operation cannot run an
application destructor or fallible cleanup. General displaced resources still
belong in post-unlock `cleanup` state.
Any replacement snapshot mechanism still requires an equivalent reclamation
and ordering proof.

### J.4 Point lookup and miss interning

Point access proceeds as follows:

1. Look up the key through the copied/scalar C ABI.
2. On a directory miss, atomically reserve quota and allocate a fully initialized
   tombstone candidate.
3. Acquire exclusive structural admission. The default is nonblocking and
   returns `Conflict::HiddenLockBusy`, proving the reserved candidate unpublished
   without advancing generation. `with_blocking_structural_admission(true)`
   explicitly permits waiting at this execution-time boundary, not while holding
   STO commit locks. Once admitted, advance generation before native atomic
   `get_or_insert`. A permanently sealed directory rejects a genuinely missing
   physical key before reserving its ID.
4. When core creates the record's first transaction item, retain only its
   record-role tag. A standard table resolves the winning logical ID through
   the table-private registry and resolves its lock-target segment during
   writer locking. A direct table validates and reconstructs its private
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
   `original_live`, `old_was_shared`, and record-local-policy classifications.
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
| `DenseItem` | Fused NewOrder item batches share a table-wide, direct-index cache keyed by item ID. Scalar operations behave as `None`. | Do not populate the cache. |
| `DenseStock` | Fused NewOrder and StockLevel stock batches share a table-wide, direct-index cache keyed by item ID while the table is bound to one positive warehouse ID. Scalar operations behave as `ReadThenWrite`; a warehouse mismatch permanently disables dense lookup for that table. | Do not populate the cache. |

Entries store at most 32 key bytes and confirm both the full key and the
never-reused table identity. A hash collision can only evict or miss. It cannot
alias records. `sto_tpcc_table_create` selects `Full` for compatibility, while
the C++ TPC-C wrapper uses `sto_tpcc_table_create_with_cache_policy` and assigns
policies by table role:

| Current TPC-C tables | Policy | Reason |
| --- | --- | --- |
| `customer`, `warehouse`, `district`, `new_order`, `oorder` | `Full` | Reuse hot point resolutions, Delivery's scan-to-remove handoff, and its order get-to-put handoff. |
| `item` | `DenseItem` | Share item resolutions across workers in the fused NewOrder item batch; ordinary scalar item reads retain no resolution. |
| `stock` | `DenseStock` | Share stock resolutions across workers in fused NewOrder and StockLevel batches while retaining the scalar get-to-put handoff. |
| `customer_name_idx`, `oorder_c_id_idx`, `history`, `stock_data`, `order_line` | `None` | Their current access pattern is point-read-only, insert-only, or scan/batch based, so retained point entries do not remove a later traversal. |

`LastOnly` remains available to other bridge users but the current TPC-C table
classifier does not select it. Each dense policy eagerly allocates exactly
100,000 eight-byte atomic identity slots (800,000 bytes, plus the retained table
handle and allocation metadata) when its table is created. Slots hold no value
or OCC state, and every hit still performs normal transactional observation and
validation. Fused Delivery also retains tokens directly from its value-only
scans for the same transaction. That path does not depend on the
cross-transaction cache and is why `order_line` can use `None` while Delivery
still updates each scanned row without a second tree lookup.

### J.5 Writes and liveness changes

`put`, `insert`, and `remove` change transaction-local staged state during
execution. Ordinary `TObject::lock` synchronously acquires the record lock through
the table's canonical or unique-request lane, retaining `RecordLockSegment` in
registry-ID mode or `TableShared` in direct mode. That owner covers detached-guard
release. The homogeneous direct plan separately prepares its compact token.
An intent retains the replacement either as owned data or through the explicit
borrowed-buffer lifetime contract of a closed integration.

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
`Table::new_direct_with_budget`, sharing the database's `RegistryBudget`, and
enables `TableConfig::with_unique_lock_requests(true)` as the ordinary fallback. Its
transaction handles expose only these `sto-masstree` resources: no caller can
add a default-configured or unrelated adapter to the same transaction. Core
item deduplication emits at most one record request per table/token; generation
observations emit none, and each table owns a distinct lock target and
namespace. Those properties establish both the direct injectivity proof and
the fallback no-mixed-mode/unique-`LockIdentity` contract. General-purpose
table construction keeps the unique flag disabled because it cannot prove the
surrounding adapter graph.

TPC-C enables bounded atomic values for `customer`, `district`, and `warehouse`.
Every FFI table uses record-local OCC and distributed-history support; bootstrap
rows still use ordinary payload tiers until decision metadata is installed.
The permanently unsupported FFI `trusted_scan_value_generation` field and its
old ABI offset assertions have been removed. Callers rebuild the lockstep
header/library and initialize the current complete structure. Bounded atomic
values remain supported. General Rust `TableConfig` still offers the ordered
trusted-generation feature to other callers; it is not selectable through FFI.

### J.6 Scans

The ordinary nontrivial scan path:

1. obtains shared structural admission unless permanently sealed, then observes
   the physical-directory generation;
2. obtains copied key/scalar-token chunks from Masstree;
3. privately resolves every returned token through the exclusively owned
   directory-to-registry capability;
4. soundly reloads every returned record, including tombstones, against that
   item's first observed generation, or reads its staged intent directly;
5. adds a compact record read observation for every newly encountered record;
6. filters tombstones, whose later tombstone → live transition is covered by
   that record's observation;
7. applies lower/upper and inclusive/exclusive bounds exactly; and
8. resolves staged values/liveness through the same record items in directory
   order; staged keys have already been physically interned, so no separate
   ordered pending-key map is needed.

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
record observations, and directory-generation validation. The supported profile
is non-opaque; execution-time whole-transaction opacity validation is not an
available scan mode.

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

`Table::seal_directory_structure` waits for structural users, seals the native
directory, then Release-publishes the Rust seal. It is irreversible. Existing
physical records, including tombstones, remain readable and mutable; only new
physical keys are prohibited. Once sealed, scans skip the Rust structural read
guard and native reads skip reader-slot admission, while retaining RCU and STO
validation. The general seal is separate from the fixed-u64 loader seal.

Bounded and reverse scans apply the same proof in their traversal direction.
They observe all in-bound physical records, including tombstones, up to the
logical result limit. An existing tombstone beyond that limit or outside the
bounds cannot affect the result; a resurrection before the limit changes an
observed record version. A new physical directory entry remains deliberately
table-coarse and invalidates the scan regardless of bounds.

The paper's `TMasstree` used transaction items for both stored values and leaf
nodes and modified Masstree to expose the prior versions of leaves split by an
insertion. That let the adapter repair an earlier range witness after the same
transaction's eager insert. Rust replaces that leaf precision with coarse
physical-directory generation, per-record observations, and staged overlay.
After successful scalar interning, `account_own_publication` advances any
existing directory and trusted-scan observations by exactly its own increment.
It never reloads the current generation, so foreign increments remain visible
as conflicts. A scan followed by an own scalar first miss can therefore commit.
General fixed-batch miss paths bypass this helper and can still conservatively
self-conflict. A native fixed-insert batch advances structural generations once
per admitted batch, not once per candidate.

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

Structural publications advance the scan generation independently. A writing
commit advances it once per transaction/table, before its first record-state
publication. `ScanPublicationOwner` deduplicates using the runtime owner and
never-repeated ordered commit ID, so later records in that same table/commit do
not repeat the shared RMW. Owner-ID reuse is safe because commit IDs do not
repeat. Record-local versions are therefore incompatible with this option.

An existing-record write after the scan can join the same transaction: install
occurs after certification. An own scalar first miss uses the exact increment
repair described above; general fixed-batch misses do not. Foreign publication
anywhere in the table still invalidates the scan, even outside its bounds.
This coarse conflict domain and one RMW per writing transaction/table are why
the option is used selectively rather than enabled for every point table.

The method is unsafe only at its callback and private-tree boundary. The caller
must not retain borrowed pointers, reenter the transaction or worker, or unwind
across FFI. Structural admission, native result memory validation, OCC checks,
capacity limits, and final STO certification remain enforced. The hidden
`visit_bounded_forward_values_trusted_with_scratch` specialization additionally
requires a lower-inclusive/upper-exclusive forward range of at most 300 rows
and uses keyless native token output. It retains the same fallback and
certification rules and adds no public `mt_*` ABI capability.

This general ordered-table optimization is not the native TPC-C FFI policy.
FFI always uses record-local versions and per-record/predicate scan coverage;
the private trusted visitor falls back accordingly. Historical measurements of
the earlier TPC-C generation configuration do not describe the current source.

### J.7 Optional fixed-`u64` specialization

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
publication state may ever change. There is no unseal transition. This is the
Rust `initial_load_finished` seal; it does not call native
`mt_tree_seal_structure`. Construction rejects record-local versions and does
not participate in the general shared `RegistryBudget`.

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
STO lock plan and the same version word as its physical lock: `lock` acquires a
canonical record identity synchronously, final validation compares the observation with
the guard's pre-lock version, install stores the staged replacement while that
guard is held, and committed release publishes the new OCC generation. The
atomic value load/store may be relaxed because the version's acquire/release
protocol supplies publication ordering and the payload itself remains atomic.
`Intent = ()` marks the presence of a write while the replacement `u64` lives
in typed transaction-local state; a same-value `Put` is a no-op in this sealed
fixed-width specialization. The general Masstree adapter instead treats every
staged intent as a write, as specified in Appendix J.5.

Thus fixed-`u64` uses the same core OCC certification, canonical locking,
validation, failure dispositions, and exact-once publication protocol as the
general adapter. Its performance advantage comes only from stronger lifecycle
and datatype restrictions, a smaller record, and the terminal/batch interfaces;
it is not a separate transaction algorithm.

## Appendix K. Masstree C ABI

### K.1 Boundary principles

The older `mtx_*` prototype is historical lineage, not the current Masstree
boundary. The implemented `mt_*` ABI is pure C11, with `extern "C" noexcept`
declarations in C++ and `extern "C"` declarations in `mtree-sys`. Cargo consumes
those declarations rather than compiling a second native template instantiation.

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
Tree:    shareable facade; every data-path operation also requires &Worker
```

V1 currently uses one process-wide native Masstree runtime because the inherited
RCU/core-ID implementation does not support independent runtime instances. The
runtime handle remains explicit, and a second incompatible acquisition is
rejected rather than silently sharing global state.

The initial operation families are:

- ABI, feature, layout, build-fingerprint, maximum-key, and limit queries;
- runtime acquisition and health;
- same-thread worker attachment and quiescence;
- tree creation and an explicit runtime-shutdown request with a matching worker
  (graceful shutdown is currently unsupported);
- one-way directory sealing after all structural writers have stopped;
- scalar point lookup, a scoped repeated-read form, and fixed-width strided
  batch lookup (`mt_get_strided`);
- atomic get-or-insert with explicit publication disposition, `inserted`, and
  `winner` outputs; and
- copied forward/reverse scan chunks.

There is no update, value mutation, cursor, callback, borrowed output, physical
delete, or raw RCU pin in the public `mt_*` v1 ABI. The callback-free Payment
prefix, the commit-owning Payment, NewOrder, and Delivery calls, and the
commit-owning StockLevel tail in Appendix M.1 are wrapper-private
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

### K.2 Keys and bounds

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

### K.3 Scan outputs

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

### K.4 Threading and RCU

Every `mt_thread` is bound to its creating OS thread and runtime. Safe Rust
represents it as `!Send + !Sync`. Workers are fixed and long-lived because the
current native core-ID and threadinfo registrations are capped and not truly
recycled. Checked attachment reports native worker-ID exhaustion as
`THREAD_LIMIT`, rather than reaching the legacy registration assertion. This
is not a guarantee that all native allocator failures are recoverable.

Every public `mt_*` tree data-path operation dynamically validates the worker's
current OS thread, the worker's runtime, and the tree's runtime before
traversal. Seal and facade-release operations instead enforce their documented
lifecycle state and ownership requirements. The safe Rust facade uses
`!Send + !Sync` ownership for worker affinity on its hidden fast lane, retains a
debug-only thread-ID assertion, and dynamically checks runtime pairing.

Structural readers first register their native core in a persistent per-tree
reader bitmask, then publish the current tree in a cacheline-private per-core
slot. A writer serializes on a native mutex, sets its exclusion flag, snapshots
that tree's mask, and drains only the registered slots. The sequentially
consistent registration/publication/flag handshake covers racing first reads.
The separate append-only handle registry validates foreign pointers; it is not
the current writer-drain index. Native admission can spin or wait and is not a
bounded/nonblocking traversal guarantee. Permanently sealed trees bypass
reader-slot admission but retain native RCU.

Ordinary scalar, one-shot strided, and scan calls manage native RCU internally.
The explicit point-read scope is tree-bound and admits only point reads. The
explicit worker RCU scope is tree-independent and permits ordinary point,
insert, and scan calls while retaining their per-operation structural guards.
Both are RAII-owned, worker-affine, mutually exclusive, and follow the
synchronous caller contract in Appendix I.3. A bridge-internal,
allocation-free, nonblocking collector may copy scan results into caller
buffers. Each scoped capability carries an opaque owner cookie, but no
dereferenceable node, value, cursor, or raw guard escapes in any mode.

### K.5 Tree and runtime shutdown

The current native feature set excludes `MT_FEATURE_GRACEFUL_SHUTDOWN`.
`mt_runtime_shutdown` validates its runtime/worker and returns
`MT_ERR_UNSUPPORTED`; invalid handles or active scopes can fail earlier. There
is no implemented closing/drain/destroy state machine to invoke through this
request. `mt_tree_release` closes the facade but never destroys the native tree.

Deployments MUST create a bounded, fixed set of long-lived OS worker threads.
Dropping and recreating a facade on the same OS thread reuses its registration;
replacing OS threads consumes additional native core IDs. A reported worker
limit is an operational restart signal, not permission to reuse an ID. Native
runtime, tree, and registration reclamation currently requires process exit.

A future graceful-shutdown capability would need explicit quiescence, retained-
handle exclusion, matching thread/allocator ownership, and native grace-period
proofs. It must not be inferred from `Drop`, an orderly benchmark join, or
`mt_thread_quiesce`: that function enters/exits a scoped RCU region, not a
public full reclamation barrier.

### K.6 Errors and unwinding

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
avoid them. In particular, `SiloRuntime::AllocateUnmanagedWithLock` still asserts
on allocator-region exhaustion or mmap failure. Operational native-memory
limits remain necessary even though checked worker attachment is fallible.
Rust panics and C++ exceptions must never unwind across this boundary;
`panic=abort` builds terminate rather than converting a panic into a status.

### K.7 Build identity

The C++ shim is built by CMake with exactly the same generated Masstree
configuration, target definitions, compiler, standard library, and native
archives as the tree implementation. Cargo MUST NOT independently compile the
template-heavy Masstree headers with a divergent `CONFIG_H`.

Runtime negotiation checks ABI version, required features, endianness, pointer
width, public POD layouts, key/worker/scalar limits, and the native-reported
export-manifest fingerprint. It requires a nonzero native build fingerprint and
rejects identity changes while a shared runtime remains reachable. It does
**not** compare first acquisition with a predeclared expected build fingerprint
or enumerate the actual symbol table. Exact export auditing is a separate
build/gate check; the reported manifest hash is not runtime build attestation.

## Appendix L. Correctness argument

### L.1 Assumptions

This local OCC strict-serializability argument assumes the following. It is not
a distributed speculative-history or crash-recovery proof; those additional
protocol responsibilities and limits are specified in Appendix M.3.

1. every adapter satisfies the obligations in Appendix D;
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
   Appendix E;
6. item identities are stable and equality-correct;
7. nontransactional operations participate in the same protocol; and
8. an ordinary general-table scan retains shared structural admission across
   traversal, or uses a permanently sealed directory; admitted interning advances
   structural generation before native publication; scalar own-publication
   repair adds only its exact increment, preserving every foreign increment; and
9. a trusted direct-table scan retains one scan-generation item and sound row
   OCC snapshots under the same structural rule; structural publications advance
   independently, and each writing transaction/table advances the generation
   before its first record-state publication.

### L.2 Writing transactions

All write resources are locked before final validation and any required
ordered clock reservation. Only writing bindings requiring ordered versions
force that reservation; all-local writers rely on checked monotone record
successors. Define the certification cut immediately before the first final
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
covering versions and held write locks, not a universally allocated global ID,
establish the ordering. Ordered bindings receive the reserved ID; local bindings
publish their own checked successor even in mixed transactions. Local generations
are not comparable across records. Disjoint transactions may reserve ordered
IDs and complete in a different order because there is no global publication
barrier. After the failure-disposition boundary, each write installs under its
own held guard; independent local cell/counter guards can publish early, while
aliases, tables and retained participants defer. A concurrent attempt that
could commit an incompatible partial observation encounters a remaining lock
or an advanced covering version. Hook acceptance determines whether the
certified transaction commits; it does not move the serialization point.

### L.3 Read-only transactions

A read-only transaction defines its certification cut immediately before its
final pass. Under the adapter invariants, covering generations and retained
write ownership certify the observations' abstract dependencies at that cut.
This argument does not require every resource to have been literally unlocked:
a transient writer lock later released by abort can leave the dependency
unchanged. A conflicting committed change must advance its covering generation.
The paper's opacity protocol would additionally maintain a consistent execution
prefix; that profile remains unimplemented in Rust.

### L.4 Aborts and eager scaffolding

Deferred writes have no shared abstract effect before install. Abort releases
locks and discards intents. The Masstree adapter may have interned tombstone
records, but those are logically absent before and after abort. Therefore an
aborted transaction has no abstract effect.

### L.5 Phantom safety in v1

An ordinary nontrivial scan samples the physical-directory generation while
holding shared structural admission, keeps that guard through every copied
chunk, and finally validates the sample as an unordered prepared-free read.
A permanently sealed directory can omit the guard because new physical keys
are impossible; row mutation still requires normal validation. Every
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
because unrelated physical interning anywhere in the table invalidates the
scan. Scalar own-publication repair adds exactly the transaction's increment
to an existing witness instead of resampling shared state, so it cannot erase
a foreign change. General fixed-batch misses omit that repair and can still
self-conflict.

A trusted direct-table scan substitutes one table-wide value-generation
observation for the per-row STO items. Its shared structural guard and eager
generation advance retain the same no-traversal-gap proof for new directory
entries. Each row's OCC sandwich prevents a torn or lock-covered value from
being exposed during execution. A writing transaction advances generation
before its first state publication in that table, while holding every write
lock; later publications in the same commit need no additional generation RMW.
Structural publications advance independently. The operation's final check and
core's certification-time recheck therefore cover relevant changes without
retaining a record item per row.

Prior record state forces ordinary overlay. A later existing-record write is
installed after certification; an own scalar first miss performs exact-increment
repair, while a general fixed-batch miss can still self-conflict. Under
Assumption 9 these witnesses cover both phantoms and existing-value changes.
The trusted strategy is more conservative than ordinary per-row observations:
a foreign writing transaction anywhere in the table can invalidate it.

### L.6 What this argument does not prove

It does not prove fairness, bounded retries, crash durability, distributed
atomicity, MVCC visibility, or correct behavior if a C++ or nontransactional
path bypasses the Rust record/directory-generation protocol.

## Appendix M. Mako and upper-layer integration

### M.1 Reference backend

The upper interface uses the Rust engine:

```text
mako-cache -> mako-engine -> mako-engine-sto -> sto-core / sto-masstree
```

`mako-engine-api` owns the `LocalEngine`, `EngineDb`, `EngineTransaction`,
holder/control, feature, error, and commit-report contracts. `mako-engine`
reexports the sole implementation. `sto` and `engine-sto` remain the default
feature spellings for existing Rust callers; the C ABI backend features and
`mako-local` crates have been removed. Test-hook features remain nonproduction.

Native `dbtest`, the TPC-C runner, examples, replay, and the RocksDB-compatible
facade all default to Rust and reject `cpp`. Native sharding and migration use
registered Rust storage handles. Transaction algorithms run independent
in-memory datatype tests, while Masstree and native integration have separate
targets. The binary-key and composition corpus now compares Rust with an
independent ordered-map model; cache recovery uses fresh Rust processes.

The native application bridge is:


```text
C++ abstract_db / rust_sto_tpcc_wrapper
                  |
             sto-tpcc-ffi
                  |
       sto-core -> sto-masstree -> masstree -> hidden/public native ABI
```

`sto-tpcc-ffi` owns Rust runtime, worker, table, transaction, status, and panic
containment for this closed integration. Its CMake release static-library build
explicitly uses `panic=unwind`; the ordinary Rust workspace release profile is
`panic=abort`, so the bridge's containment must not be generalized to cache
release panics. It is not a stable generic ABI for
`sto-core` and does not expose the adapter trait hierarchy to arbitrary C
callers. It is a lockstep static boundary: its Rust library, private C++
wrapper, and header MUST be rebuilt and deployed from one source revision. It
does not negotiate version or feature compatibility independently of the
versioned `mt_*` Masstree ABI below it. Its checked public calls require live
handles and readable/writable, properly aligned, non-aliasing ranges for the
full call. Within those caller safety preconditions they validate nullness,
alignment, lengths, the Rust `isize` slice bound, address overflow,
owner-thread affinity, active transaction state, table ownership, enum values,
callback presence, and output capacities.

The bridge admits local, multi-shard and replicated execution. Nonreplicated
native Rust sharding also creates destination warehouse indexes on demand,
including on an attached worker. Attachment admission is process-wide and
counts foreground, loader, persistent helper, and replay lanes, bounded by both
`mt_max_threads()` and `MAX_THREADS`. It is not the upper cache's worker limit.
See the topology and lifetime contract below and Appendix M.3.

The TPC-C wrapper selects sparse registry growth for every table. Its previous
name-based 4-million and 16-million retained-record ceilings and 6-million and
20-million consumed-ID ceilings are removed. All three numeric table quotas
default to `u64::MAX`, subject to the registry's `isize::MAX` addressable ID
limit and shared structural allocation budget. The startup environment is:

| Variable | Default | Accepted value |
| --- | --- | --- |
| `MAKO_STO_TPCC_REGISTRY_MEMORY` | `8G` | Positive decimal byte count, optionally followed by uppercase `K`, `M`, or `G`, using powers of 1,024. |
| `MAKO_STO_TPCC_MAX_RETAINED_RECORDS` | `u64::MAX` | Positive decimal integer, applied to every table. |
| `MAKO_STO_TPCC_MAX_CONSUMED_RECORD_IDS` | `u64::MAX` | Positive decimal integer, applied to every table. |
| `MAKO_STO_TPCC_MAX_RETAINED_KEY_BYTES` | `u64::MAX` | Positive decimal integer byte count, applied to every table. |
| `MAKO_STO_TPCC_MAX_TRANSACTION_ITEMS` | `65536` | Positive decimal integer up to `u32::MAX`, bounding observations in one transaction. |

Unset variables select defaults. Empty values, zero, leading zeros, signs,
whitespace, invalid suffixes, and arithmetic overflow are rejected before
database initialization with exit status 2. Registry bytes must also fit
`size_t`. The C ABI's trailing `sto_tpcc_db_config.max_registry_bytes` selects
the shared budget; its zero value selects 8 GiB for zero-initialized C callers.
That C defaulting convention does not make an explicit environment value of
zero valid. The lockstep Rust library, wrapper, and C header MUST all be
rebuilt after this structure change.

Transaction observations include tombstones traversed by range scans, even
when the scan returns few live rows. Repeated migrations and Delivery can
therefore exceed the former 1,024-item benchmark limit. The configurable item
budget preserves those observations for validation; it does not reclaim
tombstones or bound the number of rows returned. Storage grows on demand.
Transactions that exceed this finite budget still report resource exhaustion.

`STO_TPCC_RESOURCE_EXHAUSTED`, numeric status 6, distinguishes ordinary capacity
exhaustion from retryable conflicts and fatal integrity failures. If an access
or commit exhausts capacity, the FFI ends the active attempt, discards staged
writes and logical-row-count deltas, releases its locks, and closes its native
RCU scope before returning. Another commit without a new begin is invalid;
idempotent abort and a fresh begin remain permitted. Ordinary exhaustion MUST
NOT quarantine a healthy runtime. If cleanup fails, or publication was
ambiguous or the runtime becomes poisoned, fatal/quarantine status takes
precedence over the capacity result. Successfully interned tombstones and
consumed IDs from the aborted attempt can remain allocated under the v1
identity rules; transactional rollback does not promise allocation rollback.

Not every `Capacity` is ordinary. A spent checked OCC commit-ID domain is
terminal: `Runtime::reserve_commit_id` transitions the runtime to
`RuntimeHealth::Exhausted`, and every later transaction is refused until a
proved quiescent rebase or restart. The runtime therefore MUST classify
capacity exhaustion as terminal or ordinary before translating it, and a
terminal exhaustion MUST be reported as a fatal status rather than as status 6.
Reporting it as status 6 would invite the closed TPC-C boundary to retry a
commit that can never succeed. `CapacityError::is_terminal` is the single
predicate for that classification, and the closed boundary MUST consult it in
every capacity-to-status mapping.

The C++ wrapper translates status 6 into `storage_resource_exhausted`. Loader
and worker boundaries catch it, stop the workload, join started threads, and
complete database-thread cleanup. Failure during worker setup still completes
the startup barrier protocol. A stopped process emits
`TPCC_RESOURCE_EXHAUSTED phase=startup|load|run`, exits with status 3, and MUST
NOT emit a successful `TPCC_BENCH_RESULT`. It does not retry an unchanged full
budget indefinitely.

The failure reporter emits one record of at most 512 bytes with a single
write to standard error, so concurrent shutdown logging cannot split the
phase marker on a pipe. The bound includes leading and trailing newlines.
Embedded control characters become spaces; truncated details end with `...`.
Failure to write the complete diagnostic is fatal. This does not change the
successful result format or the quiescent usage records.

`sto_tpcc_db_usage` reports allocated structural registry bytes and their
shared maximum. `sto_tpcc_table_usage` reports retained records, retained key
bytes, consumed IDs, allocated structural registry bytes, and each configured
numeric maximum. These are independent observations, not one atomic snapshot
across counters during concurrent mutation. The benchmark samples them while
workers are quiescent after loading and after joining, including failure paths,
and emits `STO_TPCC_CAPACITY` records with the phase and table name. End-of-run
reporting occurs after the measured interval. Native allocator configuration
is reported separately as `STO_TPCC_NATIVE_ALLOCATOR`; it excludes Rust heap
allocations. A larger registry budget never increases native allocator memory.

The thread handle uses a private owner with a raw pointer to its stable
`WorkerContext` allocation and an inline lifetime-erased active transaction
slot. The transaction is a small exclusive handle. Each attached worker
allocates one boxed general transaction frame and transfers ownership of that
box to the active transaction, then to the commit driver. Clean completion
returns the box to the idle worker. Its item pools, index and lock storage
stay at the same address across attempts. Begin resets attempt-local state
in place; it does not allocate another frame or move the pooled storage.
On 64-bit targets the active handle is 16 bytes and the frame is 200 bytes.
Uncertain cleanup quarantines the complete frame at the existing failure
boundaries. Worker destruction still contains retained-adapter destructor
panics before disposing of an idle frame. The separate terminal-read scratch
and its protocol are unchanged.

An outer handle borrow cannot recursively retag the transaction's exclusive
worker reference. Exclusive ownership of the frame box avoids keeping a raw
frame reference into the mutably borrowed worker.
The owner caches its `RuntimeId` before transaction begin and exposes the cached
identity, but no worker reference, allocation pointer, or transaction reference.
Transactional operations run inside a forced-inline higher-ranked closure that
gives each borrow a fresh lifetime; safe parent-module code cannot retain that
borrow or exchange transactions between owners. Commit, abort, fatal cleanup,
and handle destruction use a second higher-ranked closure that consumes the
transaction before reclaiming the worker. A native-free Miri test moves an
active owner, reads its cached identity, reuses its transaction, and exercises
active-owner destruction. A native integration test also rejects a
foreign-runtime table, checks automatic abort, and reuses the worker.

Colocated wrappers share one native Rust database/runtime, so a local facade
never hands a worker a foreign-runtime resource. Replicated foreign facades
route through the owning RPC participant rather than directly mutating another
shard's table. Canonical shard/table IDs remain stable for routing and replay.
`dbtest` joins every runner before closing cross-shard facades and stops shared
helpers only after no shard can issue further calls.

The closed profile constructs its complete table set during startup, before
long-lived transaction workers attach or any concurrent table use begins.
Creating tables after worker startup is outside this profile. The generic
Rocks-compatible `mako::DB` facade is a separate C++-only surface and cannot be
used to select Rust STO.

Attachment budgets are checked before database creation and multiplication
overflow. The base budget covers `shards * workers`; a second topology-aware
bound covers each local shard's persistent helpers, foreground/replay workers,
and the maximum concurrent loader group, plus fixed maintenance reserve.
[`rust_sto_tpcc_wrapper::init`](../../src/mako/storage/rust_sto_tpcc_wrapper.cc)
is authoritative. A flat “1–111 workers” rule is no longer sufficient when
helper and replay lanes coexist. Native allocations/registrations remain
process-lifetime; detach is not permission for unbounded thread churn.

The paired C++ wrapper may use wrapper-private trusted transaction-lifecycle,
scalar point-operation, and scan calls that are absent from the installed
header. Its object graph and transaction state establish the live-handle and
affinity invariants; request construction supplies the documented range,
enum, callback, and unique-output preconditions. Capability and table
ownership are construction invariants with debug assertions, not release-mode
dynamic validation, and raw pointer liveness/readability remains an unsafe C++
caller obligation. The private functions retain cheap raw-range shape checks,
the Rust panic boundary, and semantic outcome plus conflict, capacity, and fatal
status classification. They omit dynamic handle and ownership checks already
proved by the wrapper. Direct application or foreign use violates the contract
and may be undefined behavior.

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
They neither replace `TObject` nor expose Rust transaction items
to C++.

The corresponding Rust entry points are hidden static-link symbols and are
absent from the installed `sto_tpcc_ffi.h`. They use fixed-layout request and
result records with matching Rust and C++ offset assertions. Protocol-mode
(multi-shard or replicated) attempts do not select these local commit-owning
capabilities; they retain the native scalar distributed path. Local eligibility
is deliberately narrow:

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

### M.2 Commit hook compatibility

**[COMPAT]** The core invokes `reserve_upper_metadata` after acquiring the full
write set, `pre_install` after successful final validation, and `post_install`
after every write is installed but before the final release pass. Local
datatype publication may already have occurred; table/cache profiles retain
locks through this hook. Callback names do not determine timestamp allocation.

Logging record/holder terminals use this seam; ordinary upper-engine
`commit_report` calls the core without an upper metadata hook. General logging
paths enter the HLC/certification gate in `reserve_upper_metadata`. An eligible
existing-row singleton in the unchecked callback-record path instead defers gate
entry until `pre_install`, after local validation. Other restricted singleton
terminals can omit the general gate. Eligibility excludes insert/delete, scan
predicates, and independent read-only observations.

HLC/order allocation or caller-lane sequencing, exact record binding, and
serialization remain pre-install work after local validation. Holder terminals
can retain an owned deferred one-put payload instead. An acquired gate may be
released after binding while write locks stay held; on validation abort it
survives until the consuming core call returns after lock cleanup.

Upper-engine `post_install` is an observer notification, not cache queue
publication. Holder READY/SPSC ACK or ordinary cache publication follows the
core terminal. The ordinary local TPC-C path uses post-install table-size
deltas. The retained distributed path publishes those deltas after successful
INSTALL and before participant finish, while its locks remain held.

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
    fn post_install(&mut self) {}
}

impl<'worker> Transaction<'worker, Active> {
    pub fn commit_with_hook<H: CommitHook>(
        self,
        hook: &mut H,
    ) -> Result<CommitOutcome, CommitFailure>;
}
```

A hook that reserves a ticket early may leave a harmless gap. The current cache
does not mint HLC before local validation. Pre-install rejection or a
contained pre-install panic is a definite abort only because the upper hook
contract guarantees that its preallocated staging has no externally visible
rejection path. `post_install` cannot reject. In retain-through-install table
profiles it precedes lock release and coupled metadata cannot lag a conflicting
writer. Generic cell/counter hooks must not assume this retention.
A post-install panic is indeterminate and quarantines the runtime.

Hooks for disjoint workers may run concurrently. Blocking I/O, await, and
reentrant STO use are forbidden in every callback. The core does not currently
measure or enforce hook latency. A production integration must monitor that
latency outside the commit driver and treat a post-install overrun as a health
event without attempting to interrupt or roll back the irrevocable commit.

The hook receives typed upper metadata. It MUST NOT cause `sto-core` to interpret
Mako timestamps as OCC versions or inspect erased adapter payloads. If an
accepted commit is poisoned or indeterminate during installation, publication,
or release, upper-layer recovery owns the staged metadata and consumes that
disposition; the core cannot roll it back.

### M.3 Native distributed participants and coordination

The implementation is native Rust, not a generated C++ STO backend.
[`participant.rs`](../../crates/sto-core/src/participant.rs) owns the same
transaction frame as local commit; [`protocol.rs`](../../crates/sto-tpcc-ffi/src/protocol.rs)
owns the Mako coordinator, final writes, decision and terminal report.
The C++ wrapper bridges existing `ShardClient`/SRPC and replication interfaces;
it does not reconstruct Rust items as C++ `Transaction` items.

#### Retained ownership and phases

The public core surface is:

```rust
impl<'worker> Transaction<'worker, Active> {
    pub fn into_participant(self) -> Participant<'worker>;
}

impl Participant<'_> {
    pub fn with_transaction<R>(
        &mut self,
        operation: impl for<'op> FnOnce(
            &mut Transaction<'op, Active>,
        ) -> Result<R, AccessError>,
    ) -> Result<R, AccessError>;
    pub fn lock_writes(&mut self) -> Result<(), ParticipantError>;
    pub fn validate(&mut self) -> Result<(), ParticipantError>;
    pub fn install(
        &mut self,
        decision: mako_clock::distributed::DistributedTimeTerm,
    ) -> Result<(), ParticipantError>;
    pub fn finish(self, committed: bool) -> Result<CommitOutcome, CommitFailure>;
    pub fn abort(self) -> Result<CommitOutcome, CommitFailure>;
}
```

`ParticipantError` distinguishes `Aborted(AbortReason)`,
`Failure(CommitFailure)`, and `InvalidUse(InvalidUse)`. The phases are
`Collecting -> Validated -> Installed -> Finished`. While collecting, operations
and incremental `lock_writes` share typed items; repeated writes refresh prepared
intent state without relocking a retained physical target. `validate` acquires
remaining writes and seals execution. `install` enters irreversibility before
the first mutation and suppresses datatype early unlock. `finish` releases;
after INSTALL, `abort`, `finish(false)`, or Drop performs committed cleanup,
never rollback. A validated read-only participant may finish committed without
an INSTALL mutation.

The participant is `!Send`/`!Sync`, borrows its worker, and retains every needed
allocation and native scope. The FFI's private boxed worker owner brands
operation borrows and keeps worker lifetime beyond participant/guard teardown.
Synchronous network calls between phases use the same persistent OS-thread
helper lane. This is lock retention across network waiting, not async support
or a durable prepared-record recovery system. An uncertain destructor
quarantines instead of releasing possibly live ownership or reporting idle.

#### Three distinct clock domains

[`mako-clock::distributed`](../../crates/mako-clock/src/distributed.rs) defines
`DistributedTimestamp`, `DistributedTerm`, `DistributedTimeTerm`, and the
process-wide `DistributedClock`. Bases are checked nonzero `u32` values through
`(u32::MAX - 9) / 10`; terms are decimal digits 0–9. Legacy rows/logs encode
`base * 10 + term`; zero means bootstrap/no decision. INSTALL carries **base**,
not that encoding, an OCC generation, or the cache's 16-byte HLC timestamp.

Each selected read contributes its own payload's base, including historical
reads, remote reads and selected scan tombstones. Allocation proposes above
selected dependencies; the coordinator takes the maximum participant/local
proposal, raising its own proposal above any remaining selected-read bound.
It need not choose strictly above every proposal. `observe` floors future
allocation before publication/replay effects. Adopting the last valid decision
still succeeds when subsequent allocation is exhausted. This authority is
distinct from private 48/16 OCC generations and from cache HLC/dense ordering;
no numeric comparison across those domains is valid.

#### Coordinator, admission and terminal outcomes

The native coordinator flushes final remote mutations, locks local writes,
collects timestamp proposals, certifies local and all touched remote participants,
then installs the selected decision, submits native final-write logs and finishes.
The touched set includes read-only participants for validation and terminal
cleanup; a write-only set would leak their retained frames.

[`server.cc`](../../src/mako/lib/server.cc) admits incarnation, attempt, sequence
and term **before** reset, storage, clocks or terminal effects. Exact duplicate
requests use the cached bounded reply; stale sequences and late terminal packets
cannot mutate the next frame. A new incarnation in the same term is rejected.
[`client.cc`](../../src/mako/lib/client.cc) preserves lane identity and counters
across same-OS-thread detach/reattach. The client checks exact echoed identity
and all expected replies; absence is not success.

Before the decision boundary, conflict can definitely abort after terminal
cleanup. Once INSTALL may have occurred, missing/failed acknowledgement is
UNKNOWN (native status 7, wire status 5), not workload retry. Proven committed
cleanup failure is native status 8. The wrapper fail-stops these outcomes
instead of throwing into legacy catch-all retry loops. Log submission
uncertainty also remains UNKNOWN. Repeating or dropping a handle must not
erase its uncertainty.

Normal shutdown must not manufacture an uncertain transaction.
[`bench.cc`](../../src/mako/benchmarks/bench.cc) stops worker admission, joins
local workers with RPC still active, then waits for every routed shard
coordinator to finish before closing clients or participant servers. The
bounded NFS drain handshake acknowledges each runner's fresh nonce; reused
working directories and old completion files cannot satisfy a new run. Atomic
record replacement prevents partial acknowledgements. Missing peers, I/O
failures, or a drain deadline fail closed rather than permit early teardown.
Peer roles follow the existing SRPC failover routing. Colocated runners obey
the same all-coordinator boundary before the process-wide client stop.
The records use the same shared working-directory/host/port namespace as the
existing `NFSSync` load barrier, not the host-local `/tmp` readiness directory.

Replicated shutdown then waits on every hosted partition's delivered END and
current-incarnation acknowledgements from its configured replica membership,
including the Paxos learner. Follower END drains deferred replay before marking
the stream terminal; duplicate END delivery cannot increment completion twice.
The replication services stay alive through this fence. Accept quorum or a
fixed propagation sleep is not proof that a peer applied the terminal prefix.
Persistence is explicitly shut down while RocksDB's background pools are still
alive, rather than first flushing from a late static destructor.

#### History selection, mutation and retention

[`sto-masstree::distributed`](../../crates/sto-masstree/src/distributed.rs)
stores immutable history in the existing `SharedValue`/ArcSwap record slot,
not a second record map. Bootstrap rows retain inline/atomic fast paths until
metadata appears. Versioned reads return a selected payload and its own
`Option<DistributedTimeTerm>` under current OCC certification, never substitute
an old history generation as the OCC witness.

`ReadVisibility` carries current term, per-term stable BASE bounds and an
optional failed-shard bound. Current-term versions are visible; known bounds
filter other terms, while unknown per-term authority preserves the legacy
`FAIL_NEW_VERSION` policy. The selected dependency must satisfy the failed-shard
bound, where zero is meaningful. If history exists but no version is selectable,
the operation conflicts rather than exposing the unstable head. Versioned scans
include traversed selected tombstones in their dependency even though no row is
returned for them.

Conditional INSERT/REMOVE use selected historical liveness. Physical logical-row
counts use actual/staged-head liveness; these can differ. Unconditional tombstone
staging handles a historical REMOVE without accidentally skipping a live physical
head. Once a record has distributed history, a write lacking accepted decision
metadata fails closed instead of silently discarding history.

`Table::advance_history_retention_floor` takes an authoritative stable shard BASE,
not a read bound or per-term visibility watermark. It is monotonic. Later installs
retain the new head and previous prefix at/above the floor, then cut the first
below-floor node **and its tail**, including bootstrap. They do not retain one
extra “first stable below floor” node. Existing owned snapshots stay alive;
discarded versions cannot be reconstructed if authority retreats. History
publication/pruning currently allocates immutable nodes and may clone the retained
prefix; it is not the allocation-free bootstrap path. Payload-history pruning
does not reclaim native directory entries, record IDs or worker registrations,
and does not implement general-purpose MVCC or core opacity.

#### Logs, replay and topology

The final-write ledger copies keys/payloads and composes repeated operations,
so delayed serialization cannot use mutated caller buffers or a stale INSERT
after PUT. Rust serializes its own final writes with existing legacy framing,
canonical table IDs and source decision; the C++ transaction has no Rust items
to log. Existing speculative replication acknowledgement semantics remain.
Native logging currently submits one valid transaction frame at a time; C++
coordinator/helper logging batches transactions. The functional contract matches,
but batching and transport pressure differ. This alignment does not establish
performance parity.

[`ThreadPool.cc`](../../src/mako/sto/ThreadPool.cc) routes replay through the
selected backend's attachment, `observe_replay_timestamp` and
`replay_apply_record` capability. The native replay path preserves source
base/term, does not relog, and observes record/footer clock floors before
watermark/promotion effects, including version-skipped records. Apply replaces
only when source TERM is higher **or** source BASE is higher, exactly the legacy
rule, not lexicographic `(term, base)` ordering. Only definite predecision
conflicts are retryable; fatal/unknown outcomes cannot loop indefinitely.
An apply receipt proves successful backend apply-or-version-skip, not a payload
digest match or independent post-loading replay proof.

Colocated native wrappers share the process-global `SiloRuntime` required by the
native Masstree ABI. Shard contexts retain `GlobalDefaultOwner().clone()` and
initialize that runtime's pinned arenas before any loader runs; binding an
unrelated `Create()` runtime or skipping the single-shard allocator bootstrap
is not valid native startup. The C++ backend retains its existing runtime choice.
Full colocated replication requires every peer to receive the same shard-ordered
full YAML configuration list. Replication partition IDs use
`shard * W + local_partition`; the standalone legacy topology keeps its local
mapping. Full-topology Raft uses per-partition groups. Canonical table IDs and
owner routing are shared by foreground and replay.

The actual remote scalar scan is the existing value-only lower-median wire
contract used by TPC-C. It is not an arbitrary distributed range scan API.
The multi-process, replicated/replay and colocated consumer evidence is recorded
in §5.7. It qualifies those exercised functional scenarios, not mixed-backend
clusters, live failover, distributed crash/restart recovery or performance.


### M.4 Policy separation

The following remain distinct upper-layer concerns:

- distributed base/term allocation and visibility;
- participant identities and transport admission;
- WAL and RocksDB write descriptions;
- cache HLC/dense durability sequence numbers;
- stable-history selection and authoritative retention watermarks; and
- RPC batching and predecision retry policy.

The upper native ledger owns write descriptions; it MUST NOT hard-cast erased
core item state to MassTrans key/value types. Core only carries typed accepted
decision metadata into the datatype install context.

### M.5 Shared facade and capability admission

The common ordinary surface includes `LocalDb::open`, `open_table`,
`transaction`, and transaction `get`, `put`, `insert`, `remove`, `scan`, `rscan`,
`commit`, `commit_report`, and `abort`. A missing value is distinct from a
present empty value. Upper `put` returns newly-created liveness; `insert` is
conditional; `remove` returns prior liveness. These return types differ from
some lower `sto_masstree::Table` methods and must not be interchanged. Upper
scans return owned binary rows in `[lower, upper)`, with `None` unbounded above.

The Rust backend advertises point transactions, read-my-writes, transactional
scans, and scan read-my-writes, but not opacity. Cache startup rejects opaque
requests rather than downgrading them. Public limits are 1,024-byte table names,
1,024-byte keys, 1,048,576-byte values, 460 workers, and a transaction budget of
512 **weighted credits**. Key-dependent write charges mean that 512 is not a
universal count of ordinary writes.

`CommitReport` separates `Committed | Aborted(Error) | Unknown(Error)` from its
cleanup result. A visible commit with failed cleanup is not an abort. Unsafe
record/holder controls and consuming fused terminals remain cache-private
optimization seams, not ordinary application APIs.

The cache currently admits one application table and one recovered cache-order
namespace per process. Concurrent foreground work uses distinct bounded SPSC
lanes. `ForegroundMode::SingleProducer` requires an explicit `!Send + !Sync`
lease and disables the ordinary `Cache::transaction` entry in that mode.
Neither mode supports suspension while a transaction or native scope is active.

### M.6 Log records, durability, and recovery

The implemented path is authoritative in-memory state, bounded worker log lane,
then background bounded atomic RocksDB `WriteBatch` application. Cross-lane
physical order can differ from serialization order; HLC timestamps prevent an
older applied record from overwriting a newer row. HLC, OCC generation, physical
application counts, and lane sequences are separate domains.

Default CRC32C uses v5 `MAKOCMT\0` records; checksum `None` uses self-describing
v6 `MAKONOC\0` without a record corruption checksum. Both encode the same
16-byte HLC and are understood by both selected engines. `Durability::Wal`
enables the RocksDB WAL with `sync=false`. `Durability::Sync` requests sync for
each background batch, but foreground ACK still precedes that application.
The cache rejects `Durability::None` with `WalRequired`. `wait_applied`, flush,
and close do not add a separate WAL flush or fsync. Concurrent applied status
is an applied count / greatest applied timestamp, not a contiguous global
serialization prefix.

Failure handling preserves ordering rather than inventing rollback:

- `UnknownCommitOutcome` permanently pins its lane slot and fail-stops the cache.
- `BoundRecordUnwritten` is a definite abort but still pins its consumed ordering
  slot.
- `CommittedButCleanupFailed` still publishes the visible commit to writeback.

Recovery validates namespace, formats, lane coverage, checkpoint envelopes,
retained-log continuity, timestamp identities, winners, and payload consistency
before exposing the cache. It advances the selected engine's timestamp floor
past recovered state, then hydrates only current live rows through bounded local
transactions while the cache is private. It does not replay every historical
payload or recover foreground ACKs that never reached persistence. Tombstone
and lane metadata survive log GC. Five-minute retention and ten-second GC
interval defaults are policies, not hard bounds on resident data or history.

The shared record/checkpoint format is unchanged by backend retirement. The driver in
[`mako_cache_reopen.py`](../../scripts/mako_cache_reopen.py)
checks same-build or cross-revision Rust reopen, checksum modes, concurrent/single-producer
profiles, retained-log and checkpoint-only recovery, deletes/scans, further
writes, and negative wrong-checksum cases. A successful process exit alone is
not the equivalence criterion. Recorded results belong to their exact candidate;
this implementation inventory does not certify a later default switch.

### M.7 Touch-log ownership and fused completion

Each process-lifetime `WorkerSlot` owns reusable `TouchLog` state. Beginning a
transaction moves an `Option<Box<TouchLogState>>` owning handle, not the vectors
inside it; empty transactions need not allocate that state. An eligible first
trusted put anchors immutable 39–160-byte payload before entering the core and
may borrow it for bounded atomic staging. Later operations invalidate the
one-put witness and use owned staging. Replacing that first value retains the
borrowed allocation in a retirement slot until cleanup proves it releasable.

Unknown cleanup quarantines the exact borrowed allocation independently of a
caller-owned holder; an uncertain holder receives an independent clone when
needed. Normal reuse resets lease and retirement state. General recycling
retains at most 64 entries and budgets 64 KiB of backing allocations, dropping
oversized buffers. The earlier clean-single-entry path retains its buffers
without that cap scan, so there is no unconditional 64 KiB cap on all pooled
per-worker scratch.

Fused success explicitly returns touched ownership to the worker **before**
Release-storing ACK. Its cache caller intentionally forgets the consumed facade,
so relying on `Drop` would lose reuse and violate the ownership protocol. Cold
results preserve active, consumed, and committed-unpublished distinctions.
The unchecked v6 fused lane requires `RecordChecksum::None` and an eligible
size witness; default CRC32C follows the checked path. Per-worker SPSC lanes in
concurrent mode can use the optimization too; it is not limited to the global
single-producer profile.

Production native/core attachments remain process-lifetime in this integration.
A test-only healthy-fixture teardown helper is not a supported recycling API.

## Appendix N. Verification and evaluation

### N.1 Pure Rust core

Required tests include:

- an out-of-crate adapter contract fixture that implements
  `TObject` for two resource classes and uses a custom
  `TransactionLock`;
- compile checks that the normative public signatures remain externally
  implementable while the erased item and lock traits remain inaccessible;
- heterogeneous dispatch through two `TransItem<A>` types in one transaction,
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
- install or release panic returns `Indeterminate`, while cleanup panic after
  complete publication returns `Poisoned` with a definite committed outcome;
- callback traces for `lock` failure at item N distinguish prepared/unprepared
  items and acquired prefixes, with exact-once cleanup where ownership is known;
- allocator-failure checks for the ordinary local preallocated publication
  profile; distributed history allocation is a separate profile, not covered
  by that no-allocation assertion;
- predicate-upgrade and validation failure install nothing;
- commit/abort/Drop cleanup exactly once;
- cross-object atomicity using two independently implemented adapters;
- panic before install versus invariant failure after install begins;
- Loom schedules for version, lock, publication, and transaction models;
- Miri for unsafe type erasure or registry code; and
- property tests for operation composition and reference histories.

### N.2 Legacy semantic oracle

The stronger historical programs, rather than only current CMake smoke tests,
should seed the compatibility suite. They were removed from the tree; read one
with `git show fd3ed37e1:src/mako/sto/<name>`:

- `unit-tgeneric.cc`: generic reads, writes, opacity, non-opacity, and
  read-your-writes;
- `unit-tcounter.cc`: commutative updates and optimistic predicates;
- `unit-tarray.cc` and `unit-tvector.cc`: indexed conflicts, iterators, and
  predicate composition;
- `rbtree.cc`: insert/delete/reinsert and abort cleanup;
- `single.cc`: MassTrans point/range schedules; and
- `concurrent.cc`: randomized concurrent workloads.

The native crate MUST add Rust version-word parity tests against the committed
C++ `TransactionTid` behavior. Those tests are a bit-level migration oracle,
not proof of transaction correctness.

### N.3 Masstree ABI

Before safe integration, test:

- C11 and C++ header conformance;
- ABI layout, symbol allowlist, feature, and fingerprint agreement;
- empty, binary, embedded-NUL, 1024-byte, and rejected 1025-byte keys;
- scalar ABI round-trips for all nonzero 64-bit `RecordId` bit patterns without
  requiring the registry to allocate or resolve them;
- exactly one winner under concurrent get-or-insert;
- success, proven-unpublished, and publication-unknown insertion outcomes;
- atomic retained-record/key-byte quota reservation under concurrent misses;
- concurrent sparse-directory and segment growth without moving existing
  records, budget reservation failure before publication, and accounting until
  the last detached arena owner drops;
- tiny-quota scalar, batch, and fused-transaction exhaustion with full abort,
  healthy worker reuse, and fatal precedence if cleanup or publication is
  uncertain;
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

### N.4 Differential histories

Run the same operation corpus through the sequential `BTreeMap` reference
model and native Rust STO over the Masstree C ABI. The C++ executors have been
removed; the retained corpus still covers binary bounds and operation composition.

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
implemented Serializable profile is nonopaque. The shared sequential corpus
compares Rust against the ordered-map model; the forced concurrent history
oracle is a separate check of serializability.

### N.5 Performance ladder

The August results below are historical workload-specific evidence. The
September singleton results and their limits are summarized in
[Evaluation](#5-evaluation). Both predate the completed source migration;
neither profile's acceptance claims qualify that migration.

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
shared non-isolated host with three repetitions. Both effective timed paths used
native `-O2`; the measured Rust binary, the Cargo-built `sto_masstree_compare`,
was built with fat LTO and one codegen unit, while the C++ repository target did
not use LTO. That profile belongs to this archived run, not to the CMake
`sto_tpcc_bench` target: its `sto-tpcc-ffi` adapter archive (the
`sto_tpcc_rust_ffi` command in `CMakeLists.txt`) is built with `lto=false` and
one codegen unit, because it shares the executable with the native sharding and
Raft Rust staticlibs. Latency percentiles, scans, misses and interning,
non-eight-byte values, allocations, lock-hold time, false-conflict cost, and
application traces remain unmeasured.

### N.6 Implementation conformance record

The experimental v1 implementation corresponding to this contract is split as
follows:

| Surface | Repository implementation | V1 state |
| --- | --- | --- |
| Typed STO protocol, private erasure, lock planning, direct exact-token write certification, and failure dispositions | [`object.rs`](../../crates/sto-core/src/object.rs), [`trans_item.rs`](../../crates/sto-core/src/trans_item.rs), and [`transaction.rs`](../../crates/sto-core/src/transaction.rs) | Implemented for serializable non-opaque transactions. |
| Restricted terminal-read typestate, homogeneous pooled storage, and adapter capability | [`terminal_read.rs`](../../crates/sto-core/src/terminal_read.rs), [`object.rs`](../../crates/sto-core/src/object.rs), and [`transaction.rs`](../../crates/sto-core/src/transaction.rs) | Implemented with final certification, drop-only cleanup, compile-fail API separation, and contained failure dispositions. |
| Raw stable declarations | [`crates/mtree-sys`](../../crates/mtree-sys) | Implemented for ABI version 1. |
| Native C boundary | [`mtree_abi.h`](../../src/mako/storage/mtree_abi.h) and [`mtree_abi.cc`](../../src/mako/storage/mtree_abi.cc) | Implemented for scalar/scoped/strided point operations, worker-wide RCU retention, and copied bounded scans. |
| Safe runtime, worker, tree, point, and scan facade | [`crates/masstree`](../../crates/masstree) | Implemented; native cursors, pointers, and RCU guards remain private. |
| Transactional records, tiered atomic/shared values, tombstones, quotas, physical-directory generation, and scan overlay | [`crates/sto-masstree`](../../crates/sto-masstree) | Implemented with exact-token write acquisition, final read/generation validation, scan-only directory validation, per-record coverage of existing liveness changes, and seeded history checks in both registry-ID and direct-token modes. |
| Native C++ TPC-C bridge and Mako protocol | [`crates/sto-tpcc-ffi`](../../crates/sto-tpcc-ffi), [`rust_sto_tpcc_wrapper.cc`](../../src/mako/storage/rust_sto_tpcc_wrapper.cc), and [`tpcc_fixed_batch.h`](../../src/mako/benchmarks/tpcc_fixed_batch.h) | Native local/distributed coordinator, retained participant, final-write ledger, replication/replay and colocated routing implemented. Bounded local workload capabilities remain eligibility-gated; protocol modes use scalar operations. Source presence is not end-to-end or performance qualification. |
| Optional all-present fixed-`u64` point specialization | [`fixed_u64.rs`](../../crates/sto-masstree/src/fixed_u64.rs) | Implemented behind `fixed-u64`: private fresh directory, permanent loader seal, 16-byte atomic record, terminal reads, and exact-unique point updates; liveness changes, scans, and miss fallback are intentionally unsupported. |
| Upper metadata reservation, pre-install acceptance, and post-install publication | [`hook.rs`](../../crates/sto-core/src/hook.rs) | Implemented optional `CommitHook`; post-install runs before final release, but may follow datatype-local unlock. A panic is indeterminate. |
| Pure-Rust reference adapters and bounded isolation checks | [`crates/sto-test-datatypes`](../../crates/sto-test-datatypes) | Implemented for map, vector, and queue composition, deterministic isolation litmus tests, and model-checked strict-serializability histories. |
| Independent binary-safe transaction-history oracle | [`crates/mako-history`](../../crates/mako-history) | Implemented with exact interval and result validation, bounded strict-serializability and opacity search, final-state checking, negative fixtures, and hexadecimal replay diagnostics. |
| Orderly Rust benchmark teardown | [`bench.cc`](../../src/mako/benchmarks/bench.cc), [`dbtest.cc`](../../src/mako/benchmarks/dbtest.cc), and [`rust_sto_tpcc_wrapper.cc`](../../src/mako/storage/rust_sto_tpcc_wrapper.cc) | Workers and shared helpers stop before cross-shard facades close. Colocated wrappers share the database/runtime; invalid local topologies fail admission. Close/join do not reclaim native Masstree allocations or registrations. |
| C++ Rocks-compatible facade lifecycle | [`db.hh`](../../src/rocks_interface/db.hh), [`local_table.hh`](../../src/rocks_interface/local_table.hh), and [`rocksdbInterfaceTest.cc`](../../examples/rocksdbInterfaceTest.cc) | Implemented over Rust. It enforces paired fixed-worker attachment, active-attempt tokens, no nested transaction, graceful table-capacity failure, and external quiescence before close. `GetDB()` is a non-owning borrow, tokens may reuse an address, native state is process-lived, and replicated schemas must be static and deterministically created at startup. |
| Shared upper contract and compile-selected engine | [`mako-engine-api`](../../crates/mako-engine-api), [`mako-engine`](../../crates/mako-engine), [`mako-engine-sto`](../../crates/mako-engine-sto), the sole engine | Implemented; Rust is the default and C ABI backend selection is removed. |
| Rust cache logging, writeback, checkpoints, and recovery | [`mako-cache`](../../crates/mako-cache) and [`recovery.rs`](../../crates/mako-cache/src/recovery.rs) | Implemented over Rust; foreground ACK remains volatile and one recovered namespace is admitted per process. |
| Model transcripts, forced histories, and fresh-process reopen | [`mako-differential`](../../crates/mako-differential) and [`mako_cache_reopen.py`](../../scripts/mako_cache_reopen.py) | Implemented gates; exact-candidate results and production/default acceptance are separate. |
| Core opacity, native reclamation/worker-ID recycling, general-purpose MVCC, and durable foreground cache ACK | Appendices H, K.5, and M | Unsupported/deferred. Implemented table history and distributed replication do not remove these limitations. |
| Rust-default/production acceptance | Appendix O.2 | Rust is the default and sole backend; retirement validation is recorded in the sharding integration handover. |
| Bounded point-workload performance characterization | [August point matrix](../performance/sto-rust-zoo2-optimized-2026-08-28/README.md) and [September singleton record](../performance/mako-cache-rust-sto-singleton-attribution-2026-09-17.md) | Historical profiles remain distinct; September engine-only singleton evidence is not production-wide budget acceptance. |

The checked-in gate inventory includes Rust formatting, Clippy, rustdoc,
core/adapter tests, C11 header compilation, ABI negotiation and export checks,
safe-wrapper/native integration, independent-oracle histories, closed TPC-C
fixtures, and focused Miri/ASan/UBSan/TSan lanes. Native ABI checks cover the
Masstree record-directory boundary and `sto-tpcc-ffi` exports. The retired
`mako_local_*` ABI and its export inventory are removed.
Historical gates used Rust 1.95, still the `crates/` workspace `rust-version`.
The repository toolchain pin is now 1.97.1; retirement validation uses that
container toolchain and the separately pinned sanitizer/Miri nightly.
Current CMake targets separate `rust_sto_transaction_tests`,
`rust_masstree_tests`, `rust_sto_cache_integration`, and `rust_cache_reopen`.
The cache mutation runner also selects Rust only. Backend features no longer
select mutually exclusive C++ and Rust implementations.
Tests and runtime scenarios must run inside Docker, not through host `ci.sh`.

An inventory is not a release-candidate result. Candidate identity, source-state
digest, build profile, CI jobs, artifacts, and per-gate outcomes must accompany
an acceptance claim. The [September 13 integration qualification](../testing/mako-cache-rust-sto-2026-09-13/README.md)
and [September 23 retained evidence](../performance/mako-cache-rust-sto-singleton-attribution-2026-09-17.md)
record distinct scopes and candidates.

Leak qualification must remain exact. Failure-injection cases that deliberately
quarantine uncertain guards/frames, and native processes whose roots are
process-lifetime, cannot be described as globally leak-clean. Keep leak checking
enabled for ordinary Rust cases; disable it only for explicitly reviewed exact
quarantine/native selectors while retaining address-error checks. Miri does
not execute the native C++ tree; its exposed-provenance warnings are not a
strict-provenance proof. Native UBSan coverage likewise must not be extended
to uninstrumented third-party binaries. The selected allowlists and build flags
belong with each artifact, not with an unqualified “sanitizers pass” statement.

The default one- and two-shard lifecycle cases now run ordinary `dbtest`
without a backend flag and execute Rust transactions. The C++ MassTrans path
associated with historical Finding 13 in
[`masstree-sanitizer-findings.md`](../masstree-sanitizer-findings.md) is removed.
Existing native Masstree sanitizer qualifications remain applicable.

A production cutover record must include the required native fault-injection
and exception coverage, upper differential/recovery outcomes on the exact
candidate, operational limits, and accepted throughput, latency, and
false-conflict budgets. Existing upper gates need candidate results; they are
no longer an unimplemented facade/corpus milestone. Deferred capabilities must
remain excluded or be separately implemented and qualified.

The owner authorized Rust as the sole default backend on 2026-11-23.
The retirement handover records its correctness validation; this does not
supply the unclaimed performance and operational acceptance in Appendix O.2.

## Appendix O. Implementation status and rollout

### O.1 Implemented stages and remaining work

| Stage | Current state | Remaining qualification |
| --- | --- | --- |
| Native core and external adapter contract | Implemented, including typed pools, general/direct commit, terminal reads, local versions, and failure quarantine | Exact-candidate correctness and safety gates |
| Hardened Masstree ABI and safe wrapper | Implemented point/scoped/batched access, copied scans, sealing, and explicit lifecycle limits | Native fault/exception, sanitizer, and fixed-worker stress coverage |
| Transactional Masstree | Implemented records, absence witnesses, scan overlay, registry budgets, payload tiers, and optional fixed-u64 profile | Workload-specific memory and false-conflict budgets |
| Upper engine and cache | Sole Rust engine, shared records, recovery, model replay and reopen gates implemented | Production performance/operational acceptance |
| Native TPC-C/Mako bridge | Local and retained distributed integration, clocks, histories, replication/replay and bounded local specializations implemented | Exact-candidate distributed consumer checks and operational qualification; performance remains user-stopped |
| Core opacity, general-purpose MVCC, native reclamation | Deferred | Separate designs, implementation, capability admission, and evidence |
| C++-familiar source migration | Canonical vocabulary and source organization implemented, using `object.rs` and `trans_item.rs`; fresh Q11 functional/correctness/instrumentation evidence retained | Q11 closed with performance explicitly unqualified by user decision; allocation, retained-memory, abort, sustained-soak/recovery and final C++-gap acceptance remain unclaimed |

The owner-authorized retirement makes Rust the sole default backend. The
September singleton campaign remains an older bounded comparison; Q11 did not
qualify the migrated implementation's performance. Further performance
qualification was explicitly stopped. The upper facade and integration gates
are implemented.

### O.2 Cutover gates

A Rust-default decision must name its surface: Rust `mako-cache`, native
`dbtest`, the closed TPC-C benchmark, and the separate C++ Rocks-compatible
facade are not interchangeable. For the surface under consideration:

- every required semantic and ABI correctness gate must pass;
- sanitizer and fixed-worker stress must pass on the exact candidate under
  reviewed, exact quarantine/leak qualifications;
- differential transcripts, applicable histories, and recovery formats must
  agree or have documented intentional differences;
- failure dispositions, cleanup, and fail-stop/quarantine must be exercised;
- advertised isolation and operation capabilities must match implementation;
- retained memory, native allocator growth, and worker limits need operational
  bounds; and
- application throughput, latency, and false-conflict budgets need explicit
  acceptance, not inference from one microbenchmark.

The 2026-11-23 owner instruction supersedes the earlier requirement to retain
a C++ compatibility backend: it has been deleted, and Rust is the sole default.
The zoo-2 comparisons remain evidence for particular historical workloads;
backend retirement does not establish performance acceptance.

## Appendix P. Decision record and change policy

### P.1 Accepted v1 decisions

| ID | Source | Decision | Rationale |
| --- | --- | --- | --- |
| D1 | RUST | STO executes as native Rust; only Masstree remains behind the architectural C boundary. The current closed TPC-C integration also crosses `sto-tpcc-ffi`. | The target avoids a stable generic STO ABI while permitting the transitional C++ benchmark/backend wrapper. |
| D2 | STO/RUST | Items represent logical resources with stable identity. The general path uses owned IDs; a private direct table may use a stable registry-entry address token under an explicit unsafe capability. | Preserves type-aware conflict tracking while making the one reviewed address-identity exception visible. |
| D3 | RUST | Generic adapters use deferred writes in v1. | Makes abort and memory safety tractable. |
| D4 | RUST | Masstree stores immutable nonzero scalar tokens: a monotonic registry ID in the standard lane or an opaque stable entry address in a private direct table. | C++ only stores and copies the scalar; pointer reconstruction and dereference remain in the audited Rust owner. |
| D5 | RUST | Point misses intern tombstones; logical deletes retain them. | Gives stable absence witnesses without native leaf tokens. |
| D6 | RUST | Ordinary scans observe physical-directory generation plus every traversed record; opted-in trusted direct scans observe one table-wide scan generation and use per-row execution OCC sandwiches. | The ordinary path preserves precise existing-record coverage and RYW overlay; the trusted path trades table-wide false conflicts and a write RMW for compact scan state. |
| D7 | RUST | Published/publication-unknown records and consumed IDs are not reused during live operation. Proven-unpublished candidates release retained quotas but currently retain initialized slots/boxes and structural charges. | Separates logical quota rollback from physical storage retention and prevents premature cross-language reclamation. |
| D8 | RUST | Explicit thread-affine worker contexts replace core TLS. | Makes ownership and finite worker resources visible. |
| D9 | RUST | Canonical requests deduplicate physical identities and acquire synchronously in access order. Checked unique and homogeneous direct lanes retain their explicit proofs. | Bounded nonblocking acquisition and reverse-prefix release avoid indefinite deadlock without sorting or losing alias ownership. |
| D10 | STO/COMPAT | Serializable nonopaque OCC is implemented; opaque configuration is explicitly unsupported. | Preserves the distinction between safe snapshots, committed serializability, and execution-time opacity. |
| D11 | RUST | Installation and mandatory cleanup are infallible; cleanup is exact-once except for explicitly authorized core-owned drop-only paths. | Prevents reporting partial publication as abort while documenting terminal and committed-direct cleanup. |
| D12 | RUST | Native RCU remains behind ABI-owned one-shot regions, a tree-bound point-read scope, or a tree-independent worker RCU scope that safe Rust owns through RAII. | Generation-tagged owner cookies are opaque and scope-family checked, no dereferenceable native object escapes, structural admission remains operation-local for transaction-wide retention, and all retained state stays worker-affine under a synchronous caller contract. |
| D13 | COMPAT | Hook acceptance follows locking/validation; post-install runs after installs before final release. Datatype-local publication may precede it. | Table/cache profiles retain locks for coupled metadata; generic hooks must respect datatype publication policy. |
| D14 | RUST | Worker-affine `Participant` retains the same core frame across synchronous distributed phases. | Supports real network lifecycle without async migration or fictitious rollback after INSTALL. |
| D15 | RUST | Native version encoding is private; the C++ layout is a parity oracle. | Rust records do not exchange atomic objects with C++. |
| D16 | RUST/COMPAT | `GRACEFUL_SHUTDOWN` is not advertised; native allocations and registrations are process-lifetime, and explicit shutdown returns unsupported. | A facade close or orderly join cannot substitute for a native reclamation protocol. |
| D17 | RUST | External adapters implement safe typed base traits and private checked erasure. Compact direct tokens use the separate unsafe `DirectTokenLock` and unsafe injective constructors. | Preserves the paper's extension seam while isolating the stable-token memory proof. Compactness may omit the general identity vector, but acquisition still certifies an exact observation when the capability selects that option. |
| D18 | RUST | A terminal homogeneous read batch uses a distinct `Open -> Ready` consuming transaction typestate and an explicit stronger adapter capability. | The type surface proves that general item state, later operations, locks, installation, and outcome-dependent cleanup are unreachable, allowing a minimal key/observation representation without weakening certification. |
| D19 | RUST | Fixed-`u64` is an optional permanently sealed, all-present point profile over a fresh privately owned Masstree, not a mode of the general table. | The 16-byte hot record and omitted publication checks are justified only by exclusive directory ownership and immutable post-load liveness. |
| D20 | RUST | A homogeneous direct capability selects a sealed core plan with typed prepare, validate, install, release, and cleanup phases. Its unsafe exact-token option may certify writes at acquisition and skip only those writes in the final pass. | Removes general identity/prepared representation and a repeated write callback only when one stable capability proves the observation-to-lock binding and retains the guard; reads and scan generations still receive final validation. |
| D21 | RUST | Committed values through 38 bytes use inline atomics. Bounded-value tables use 192-byte entries and an adjacent stable atomic cell through 160 bytes. Values above the applicable atomic tier use owned staging and the embedded `ArcSwapOption`; standard 64-byte entries take that fallback above 38 bytes. | Makes the memory-versus-reference-publication tradeoff an immutable table policy while retaining arbitrary binary values. |
| D22 | RUST | Hidden `mako_mtree_*` and wrapper-private TPC-C lifecycle, scalar, scan, and fused workload lanes may omit checks already proved by their owning facade. | Keeps the optimized boundary closed while retaining structural admission, RCU, panic/exception containment, status semantics, and explicit unsafe lifetime contracts. |
| D23 | RUST/COMPAT | FFI tables use direct records, record-local OCC, distributed histories and unique-lock fallback; bounded values remain enabled for selected tables. The FFI trusted-generation field is removed. | Aligns native MassTrans version policy while retaining per-record scan certification and bootstrap fast paths. |
| D24 | RUST/COMPAT | The local fixed-layout Payment prefix is one callback-free private call using three caller-owned 164-byte buffers. Rows through 160 bytes use borrowed bounded staging; 161--164 byte rows use owned shared staging. | Preserves scalar operation order while making the same-database, local-table, nonaliasing, and transaction-lifetime obligations explicit at the closed boundary. |
| D25 | RUST | `ResolvedRecord` is a table-bound lookup capability, and the closed TPC-C bridge may retain it under a bounded per-table cache policy. | A cache hit skips Masstree traversal only. Exact table/key matching, transaction item reuse, OCC observation, and final certification remain mandatory. |
| D26 | RUST/COMPAT | Full local Payment, exact-home NewOrder, local Delivery, and the local StockLevel tail use hidden fixed-layout calls that own the active attempt through commit or abort and publish result metadata only after a successful commit. | Removes repeated C ABI crossings without adding a second transaction protocol or exposing workload-specific calls as a stable application ABI. StockLevel retains its district read and current-ID selection in C++ before handing off the scan-and-join tail. |
| D27 | COMPAT | Full NewOrder consumes its external fast order ID and timestamp before the Rust call. Full Delivery advances each worker cursor immediately after selecting a row, even if a later operation aborts. | Matches the benchmark's existing nontransactional state and retry semantics while keeping database writes atomic. |
| D28 | RUST/COMPAT | `contains_resolving` and `contains_resolved` provide metadata-only transactional presence with ordinary final OCC validation. Full NewOrder uses them for customer, warehouse, and district witnesses. | Avoids loading and decoding payloads that the valid-data release path does not consume. Missing rows, staged liveness, resolved-token ownership, and commit conflicts remain checked; corruption diagnostics for unused payload bytes are not a parity promise. |
| D29 | RUST | Lazy record registries grow through sparse, stable directory buckets and share an explicit structural allocation budget. Numeric record/ID/key quotas remain separately configurable; published slots are never reused. | Removes workload-duration ceilings without moving cached records or treating a very large ID quota as a proportional startup allocation. |
| D30 | RUST/COMPAT | Ordinary capacity exhaustion ends the active attempt and returns status 6 through the closed TPC-C ABI; benchmark boundaries join workers, report usage, suppress successful results, and exit 3. Fatal cleanup or uncertain publication takes precedence. | A full configured budget becomes an application-visible resource outcome while preserving transaction atomicity and quarantine guarantees. |
| D31 | RUST | General `Table::new_direct_with_budget` admits registry configuration/budget before native tree creation; `new_direct` inherits the order with an unrestricted structural budget. Supplied-tree and fixed-u64 constructors do not provide this ordering guarantee. | Makes the no-native-allocation rejection guarantee exact rather than extending it to constructors with different ownership/admission order. |
| D32 | RUST/COMPAT | Terminal capacity exhaustion, including spent ordered or record-local version domains, reports a fatal status instead of status 6, and still ends the attempt cleanly. | The runtime permanently refuses new transactions after `RuntimeHealth::Exhausted`, so a retryable resource status would misdescribe the outcome. |
| D33 | RUST | Version requirement is captured per exact binding and folded only over writers; local-only writes omit the ordered ticket but retain all write phases. | Avoids unnecessary shared-clock traffic without comparing local generations or weakening exhaustion checks. |
| D34 | RUST/COMPAT | The upper Rust cache uses `mako-engine-sto` by default and retains its log/recovery contracts. | The Rocks-compatible facade also uses Rust; application and cache bridges retain their separate APIs. |
| D35 | RUST | Scalar own-publication repairs only its exact directory/scan increment; trusted writing commits advance scan generation once per transaction/table. | Preserves foreign-change detection while avoiding unnecessary self-conflicts and repeated shared RMWs. |
| D36 | RUST | Bounded-value lazy records are first-touched on the claiming worker but owned by a cross-thread stable arena. | Improves locality without turning allocation placement into a TLS lifetime assumption. |

### P.2 Deferred decisions and review triggers

| Topic | V1 position | Revisit when |
| --- | --- | --- |
| Ordinary heterogeneous erased item lane | Typed unique batches and pooling are implemented; mixed external adapter items retain private erasure. | Profiling attributes material cost to the remaining heterogeneous allocation or dispatch. |
| Lock acquisition | Synchronous access order with canonical deduplication, checked unique or capable direct paths | A new mechanism supplies equivalent ownership, bounded acquisition, reverse release and alias-publication proofs. |
| Precise range witnesses | Ordinary scans use table-wide physical-directory generation plus traversed-record observations; trusted direct scans use one table-wide value generation. | False conflicts from either strategy exceed an accepted workload budget. |
| Implicit, raw, or cross-worker RCU pin | Fixed-width strided calls, a tree-bound point scope, and a tree-independent worker RCU scope are RAII-owned in safe Rust; no raw guard or cross-worker pin is exposed. | A use case requires a broader lifetime model with enforceable ownership and progress rules. |
| Commutative multi-owner locks | Exclusive locks plus semantic intents | Counter/queue workloads justify the additional protocol. |
| Physical record GC | No reclamation | Growth is material and a grace-period design is proven. |
| Native allocator-region exhaustion | The Silo region allocator asserts when a worker's configured region is exhausted, so a process that outgrows `MAKO_TPCC_ALLOCATOR_MEMORY` aborts instead of reporting a capacity outcome. Deployments MUST size the region to the dataset and volume, and MUST treat an abort as an operational failure; `sto_tpcc_db_usage` reports structural budget use, and `evt_silo_runtime_region_usage` reports native region use. | The region allocator gains a fallible allocation path and the RCU and Masstree callers can propagate it, so exhaustion becomes a reported capacity error at startup or run time. |
| Async/migrating distributed participants | Not supported; synchronous retained `Participant` is implemented | A separate lifetime, durable recovery and liveness design can preserve ownership and truthful terminal outcomes. |
| Native version bit allocation | Implemented 48-bit generation / 16-bit owner-tag encoding behind opaque APIs; not the legacy C++ layout | A demonstrated capacity or performance need supplies a migration and no-ABA proof. |
| Fixed-`u64` liveness changes, scans, or general miss fallback | Unsupported; use the general binary-value `Table` | A workload requires them and supplies a new conflict, publication, and representation proof without weakening the specialized sealed profile. |

### P.3 Changing this guideline

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
