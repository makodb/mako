# Rust Mako transaction-cache roadmap

## Goal and terminology

This plan interprets the dictated names as:

- **Masstree**: the current in-memory index used by the nontransactional
  Masstree/RocksDB write-back cache (`mrx`).
- **Silo**: Mako's existing single-machine C++ transaction engine: STO OCC
  plus `MassTrans` over Masstree.
- **Mako**: the distributed layer around Silo: routing, participant state,
  two-phase commit, logging, replication, watermarks, and recovery.

The intended progression is:

```text
Masstree cache              Silo transaction cache            Mako transaction cache
(single-key operations)  -> (single machine, atomic txns)  -> (distributed atomic txns)
                                  C++ engine                         Rust control plane
                                  C ABI                              C++ Silo initially
                                         \_______________________________/
                                                           |
                                             native Rust engine last
```

The first milestone deliberately does **not** port Silo. It keeps the proven
C++ STO/MassTrans engine, gives Rust a narrow C ABI and safe ownership layer,
then uses that engine as the single-machine transactional cache in front of a
durable system of record. The C ABI remains a draft until the Phase 1A-1D
boundary gate is complete. Later milestones move Mako's distributed
orchestration into Rust, then replace the local C++ engine only after its
behavior is captured by an executable compatibility suite.

## Decisions that should remain stable

1. **Do not turn `mrx-core` into an OCC engine.** Its versions and durability
   watermark describe per-key cache obligations, not Silo record versions,
   transaction validation, predicates, or distributed commit timestamps.
   Reuse its RocksDB adapter and lessons where helpful, but create a separate
   transaction-cache layer.
2. **The C seam is below policy and above C++ templates.** Rust sees opaque
   database, table, and transaction handles, binary slices, copied results,
   feature bits, and integer statuses. Its sole callback is the synchronous,
   scalar post-validation hook, which native code never retains. Rust never
   sees C++ vtables, `std::string`, exceptions, or Masstree/RCU pointers.
3. **One active transaction belongs to one OS thread.** STO's read/write set
   is ambient TLS. Transactions cannot nest, migrate, or cross `.await`.
   Initial deployments use a fixed set of long-lived synchronous workers.
4. **Conflicts do not cause invisible retries.** Commit returns `Conflict`;
   the caller decides whether and how to rerun application logic.
5. **Durability is transaction-granular.** The later cache must persist and
   recover one committed transaction atomically. Per-key dirty tickets or
   replaying each logged write as its own transaction are insufficient.
6. **MassTrans OCC versions, cache sequence numbers, and Mako timestamps
   remain separate types and number spaces.** `MakoTimestamp` wraps the
   checked, nonzero 32-bit `tid_unique_`; `CacheSeq` orders local durability
   obligations; and MassTrans row versions remain engine-private validation
   state. Accidental comparison or conversion between them should be
   impossible in Rust.
7. **Process-lifetime native resources are honest in the API.** STO has 460
   process-lifetime thread slots, and current MassTrans teardown lacks a
   verified global RCU quiescence protocol. The first ABI does not pretend
   those resources can be cheaply recycled.
8. **Distributed code depends on an engine-neutral Rust trait.** A
   `LocalTransactionEngine`/participant trait sits above `mako-local`; raw C
   handles never leak into routing, durability, or 2PC. That makes the final
   C++-to-Rust engine swap local rather than another distributed rewrite.

## Target architecture

```text
Rust application / database facade
                |
        mako-cache transaction API
                |
       +--------+---------------------------+
       |                                    |
local transaction participant          durability pipeline
       |                                    |
safe `mako-local` crate                 transaction WAL / RocksDB batches
       |                                    |
raw `mako-local-sys` declarations       durable commit watermark
       |                                    |
`mako_local_*` C ABI                    atomic recovery
       |
C++ STO + MassTrans + Masstree

Distributed stage:
Rust router/coordinator -> participant commands -> fixed worker on each node
                       -> 2PC + transaction log -> replication -> recovery
```

The local transaction API is the compatibility boundary. During the final
native-Rust port, the implementation behind that API changes while callers,
transaction scripts, and correctness oracles stay fixed.

### Ordering values and versions

The local cache protocol must not call every ordering value a "timestamp":

- **`MakoTimestamp`** is the exact, nonzero, 32-bit base value stored in
  Mako's `tid_unique_`. Its checked domain ends at
  `(UINT32_MAX - 9) / 10`, preserving the existing one-decimal-digit term
  encoding. For a cache-backed write transaction it is allocated from Mako's
  checked local logical counter after all Silo write locks are held and before
  read-set validation, then carried verbatim in the durable commit record.
  Validation may abort after allocation, so gaps are expected. The
  single-machine path has no remote participant timestamps to merge. The
  current distributed path also takes maxima from remote participants and the
  read set, but permits ties and therefore must not yet treat this scalar as a
  globally unique history key.
- **`CacheSeq`** is the local, nonzero durability-queue sequence. It is
  allocated only when a validated transaction binds its prepared record in
  the preinstall hook. It orders publication, RocksDB application, replay, and
  the durable watermark; it is not an OCC version or a distributed timestamp.
- **MassTrans row versions** remain the current nonopaque profile's per-record
  OCC counters. Carrying `tid_unique_` does not replace them. An opaque STO
  profile may separately use the 64-bit `commit_tid_` clock for row versions,
  but Phase 1E does not persist that value.
- **Mako's term-stamped commit ID** is `tid_unique_ * 10 + term` in the
  existing Paxos log and multiversion trailer. Phase 1E is single-machine,
  term zero, and stores the base `MakoTimestamp`; the distributed record format
  must model the term explicitly rather than confusing the encoded commit ID
  with either `MakoTimestamp` or `CacheSeq`. The legacy representation reserves
  one decimal digit but does not currently reject larger epochs; explicit term
  validation or a wider tuple is a Milestone 2 requirement.

## Milestone 1: C++ Silo as the single-machine transaction cache

### 1A. Point-transaction C ABI vertical slice

**Draft revision policy.** The checked-in implementation reports
`MAKO_LOCAL_ABI_VERSION == 0`. Revision 0 may add symbols, statuses, option
fields, and capabilities while Phases 1A-1D are completed. Only the boundary
gate below
may promote the surface to ABI v1. After that promotion, exported symbols and
numeric statuses are permanent reservations. `DUPLICATE_WRITE` is now a
legacy/no-RYW result rather than part of the default profile, but its assigned
number remains reserved. Semantic expansions are advertised by capability
bits or a later ABI revision rather than silently changing v1.

The first slice proves the boundary with the smallest useful transaction:

- Attach a long-lived OS worker to STO.
- Open a local database facade and named tables.
- Begin one transaction.
- Get, put, insert-if-absent, and remove binary keys and values.
- Atomically commit across multiple keys and tables, or abort.
- Report OCC conflict separately from absence.
- Abort an active transaction when its handle is dropped; quarantine the
  worker if native cleanup cannot prove the abort complete.
- Copy every staged value into reference-stable C++ ownership until commit.
- Contain every C++ exception at every ABI entry point.

The safe Rust shape is:

```rust,ignore
let db = mako_local::LocalDb::open()?;
let accounts = db.open_table("accounts", 1)?;
let mut tx = db.transaction()?;       // !Send + !Sync
tx.put(&accounts, b"alice", b"10")?;
tx.put(&accounts, b"bob", b"20")?;
tx.commit()?;                         // consumes tx
```

Current implementation status:

- [x] Shared process-wide STO thread-ID allocator and one-time epoch startup.
- [x] Pure-C `mako_local_*` header, opaque handles, a draft revision number,
      feature bits, binary point operations, explicit statuses, and exception
      containment.
- [x] Stable per-write buffer ownership; raw values are encoded/decoded inside
      the facade rather than leaking Mako's hidden value trailer to Rust.
- [x] `mako-local-sys` raw declarations.
- [x] Safe `mako-local` ownership layer; `LocalDb`/`Table` are shareable and
      `Transaction` is structurally `!Send + !Sync` with abort-on-drop on the
      normal cleanup path. Cleanup-failure quarantine remains a contract gate.
- [x] Initial C++ and Rust tests for multi-key/multi-table commit, abort,
      missing versus empty, binary bytes, verb results, nested begin,
      wrong-thread use, finished handles, and deterministic conflict.
- [x] One process-wide STO thread-ID space and one dedicated MassTrans
      Masstree RCU context, with its epoch advanced by the shared runtime.
- [x] Initial timestamp-based stale-artifact guard, exact CMake libc++
      discovery, and a required-native CMake/Cargo test mode. The content
      fingerprint and conformance gates in Phase 1C remain required because
      timestamps alone are not proof of build identity.
- [x] Repeated same-key point mutations on local single-version tables. A
      direct MassTrans matrix covers every three-operation combination of get,
      small/large put, small/large insert, and remove from present/absent state
      across commit and abort. The default ABI exposes that composition;
      legacy/no-RYW builds retain the reserved `DUPLICATE_WRITE` containment.
- [x] Conventional point and transactional-scan read-your-writes. Point reads
      copy the transaction's latest staged put/insert, hide a staged remove,
      and follow repeated mutation composition. Forward and reverse scans
      merge the same staged state while preserving range and resume semantics.
- [x] Explicit 1 KiB table/key and 1 MiB value limits, plus a key-weighted
      512-item transaction budget that returns terminal `TXN_TOO_LARGE` before
      STO can allocate beyond its embedded transaction set or hit its assert.
- [ ] Complete the contract gates below.

### 1B. Freeze local transaction semantics

Before calling the ABI stable, direct C++ tests must pin the actual engine
behavior for:

- read/write, write/write, and predicate/phantom conflicts;
- read-then-write, write-then-read, repeated writes, delete/reinsert, and
  insert/delete combinations;
- empty, embedded-NUL, long, and maximum-size keys and values;
- forward and reverse range bounds;
- record resize on commit and every abort cleanup path;
- read-only transactions and contention progress.

The repeated point-mutation portion is complete for local single-version
tables: native, C ABI, safe Rust, in-memory write-back, and RocksDB recovery
tests cover operation results, final bytes, canonical one-mutation-per-key log
records, commit, abort, value growth, and net no-op histories. Status 12 remains
reserved for linked legacy/no-RYW engines.

The current production defaults are `STO_RMW=ON` and `OPACITY=OFF`. CMake
normalizes the RMW option to a numeric preprocessor definition; passing the
literal token `ON` to `#if READ_MY_WRITES` previously left the guarded code
disabled. The ABI advertises point read-your-writes only when that definition
is active, and the Rust cache requires it by default. It advertises
`TRANSACTIONAL_SCANS` and `SCAN_READ_MY_WRITES` together only for that RYW
profile; a legacy/no-RYW engine remains point-only rather than exposing scans
whose overlays it cannot honor. Runtime forwarding is
deliberately limited to local single-version tables; native Mako remote proxies
and replicated multiversion participants retain their legacy behavior until
their staging and lock-transfer protocols are extended. The ABI guarantee
covers the exposed point and scan surfaces, including repeated same-key
mutations and scan overlays. Scan support is negotiated by its two scan feature
bits and is not inferred from the point capability bit.

The Phase 1A-1D boundary profile requires point transactions, scans, and
conventional read-your-writes. Opacity remains an explicit profile rather than
an implication of the ABI revision: builds without it must pass strict
serializability checks for committed transactions, while a build advertising
`OPACITY` must additionally pass the aborted/in-flight observation checks.
Phase 1E deployment configuration must declare whether opacity is required,
and startup must reject an engine whose feature bits do not satisfy that
declared profile.

### 1C. Complete the ABI surface

- [x] Add chunked forward and reverse scans. Results use entry offsets into a
  caller-owned byte arena; no callback into Rust and no internal pointer may
  cross the boundary. Forward bounds are `[start, end)`; reverse bounds must
  be defined and tested symmetrically. Resume keys must produce no gaps or
  duplicates.
- [x] Add `BUFFER_TOO_SMALL` for scans, including retry without gaps or
  duplicates. A no-RYW profile does not advertise the chunk API.
  `TXN_TOO_LARGE` is already a recoverable
  terminal error: draft point transactions use a conservative 512-item
  key-weighted budget and never reach STO's 32,768-item hard assertion.
- Numeric table IDs are unique within a database. Pin the remaining behavior
  for empty names and concurrent opens. Closing the
  in-memory facade is not persistence: a later `db_open` starts a new logical
  database even though old native table allocations remain process-lifetime.
- Publish a normative operation/status state table. For every function and
  status it specifies initialized outputs and ownership, whether the
  transaction remains active, finishes normally, or poisons its worker, and
  whether destroy is still required. In particular, `DUPLICATE_WRITE` and
  `BUFFER_TOO_SMALL` are nonterminal, while an operation-level `CONFLICT`,
  `TXN_TOO_LARGE`, `OUT_OF_MEMORY`, or `INTERNAL` is terminal. The Rust status
  mapping is derived from or checked against this table; it must not rely on an
  undocumented hard-coded list.
- Add a stable engine/build identifier and reserve sized option structs before
  ABI v1 is declared frozen, so later limits and durability modes can be
  negotiated without changing existing function signatures. CMake must also
  embed a content/configuration fingerprint covering the canonical header,
  ABI implementation and transitive STO inputs, relevant compile definitions
  such as RYW and opacity, compiler/standard-library identity, and generated
  configuration. Cargo verifies that fingerprint and treats the existing
  modification-time check as an advisory diagnostic only.
- Make the C header the single source of truth for `mako-local-sys`: either
  generate the Rust declarations or regenerate them in CI and require a clean
  diff. Add C11 and C++ conformance translation units, a Rust link probe, and
  an exported-symbol allowlist check. These gates must catch signature,
  constant, calling-convention, feature-bit, and status-number drift even when
  two artifacts happen to report the same revision number.
- Decide whether ABI v1 remains implicit TLS attachment or gains an opaque
  worker context. Either way, test attachment, wrong-thread calls, nested
  begin, post-terminal calls, database-close-while-busy, and the 460-thread
  limit in isolated subprocesses.
- Keep all outputs initialized on failure.
- Define cleanup failure conservatively. If native abort or destroy cannot
  prove cleanup complete, retain every potentially referenced allocation,
  mark the attached worker poisoned, reject all later transactions on it, and
  expose the poison through a typed status, health check, and diagnostic
  counter. Never silently reuse uncertain STO TLS state.
- Specify process-lifetime table/epoch behavior. Add ordinary teardown only
  after a tested RCU quiescence protocol exists.

### 1D. Complete the safe Rust layer

- [x] Add owned forward and reverse scan iterators over the chunk API, with
  feature-gated scan RYW, caller-owned chunk storage, and default-table
  exposure through `mako-cache`.
- Add compile-fail tests proving a transaction cannot move threads, outlive
  its database, or be held in a spawned async task.
- Unit-test the generated/verified status mapping and abort-on-drop against a
  fake ABI so Miri can exercise the ownership logic without C++. The fake must
  cover every active, terminal, and poisoned transition in the normative state
  table. An unknown future status returned during an active transaction is
  terminal-uncertain: the wrapper quarantines the worker rather than assuming
  the transaction can continue.
- Offer a fixed-worker adapter for async applications; do not mark the native
  transaction `Send` as a convenience. Because Rust `Drop` cannot return an
  error, the adapter owns worker health: an abort/destroy failure quarantines
  that worker, fails its pending command, and is reported on subsequent use
  and through metrics rather than being ignored.
- Document conflict retry patterns and put an explicit retry budget above the
  transaction API.

### Phase 1A-1D boundary gate

The native boundary is ready for cache integration when a Rust program can run
local multi-key and multi-table transactions through C++ Silo and all of these
gates are green:

1. Direct-C++, C-ABI, and safe-Rust implementations replay the same
   deterministic transaction scripts and agree on every result and final
   table state.
2. A full-history checker records operation invocation/response intervals,
   transaction boundaries, returned values, and real-time precedence, then
   searches for a legal serial execution against an independent model. It
   does not assume commit order is the serialization order. The required
   boundary profile checks strict serializability of committed transactions;
   when `OPACITY` is advertised, every relevant history prefix also includes
   aborted and in-flight observations in the opacity check. Forced RW, WW,
   and phantom schedules have deterministic expected outcomes in each
   advertised feature profile.
3. C++ integration passes ASan and UBSan. TSan findings are either fixed or
   captured as reviewed engine suppressions. Rust-only wrapper logic passes
   Miri.
4. Fixed worker pools at 1, 4, and 16 threads pass conflict, abort, progress,
   and soak tests without leaked locks or ephemeral-thread churn.
5. Direct C++, C ABI, and safe Rust benchmarks cover read-only, write-only,
   RMW, transaction sizes 1/4/16/64, and low/high conflict rates. Wrapper tax
   has a recorded same-host budget before it becomes a release gate.
6. Generated-binding or clean-regeneration checks, C11/C++ header probes, the
   Rust link probe, exported-symbol allowlist, ABI revision, and embedded build
   fingerprint all agree in a from-scratch required-native build. CI fails if
   Cargo is unavailable or native tests are skipped.
7. Failpoints at every abort, commit-cleanup, and destroy boundary demonstrate
   either complete cleanup and worker reuse or deterministic quarantine. No
   test may pass by ignoring a Drop error, reusing uncertain TLS state, or
   loading a stale native artifact.

This intermediate boundary gate excludes RocksDB durability and eviction. It
is not completion of Milestone 1; distributed routing, 2PC, replication, and a
native Rust OCC implementation remain outside Milestone 1 entirely.

Before Phase 1E exposes a public database contract, name four states
separately: **visible** in Silo, **acknowledged** to the caller, **durable** in
the local system of record, and **final** after the configured replication
rule. No API may use the single word “committed” when those states differ.

### 1E. Correct unbounded write-back cache

Phases 1A-1D establish an in-memory engine binding. This phase makes it a
cache with a durable system of record.

Create a new `mako-cache` layer rather than adding transaction semantics to
`mrx-core`:

- Silo/MassTrans is the authoritative live state while the process runs.
- RocksDB is the recoverable system of record.
- Start unbounded: every live value remains in Silo. Eviction is a later
  subphase so it cannot obscure transaction/durability correctness.
- Assign a cache commit sequence distinct from Mako's logical timestamp and
  MassTrans's per-record OCC versions.
- Before entering native commit, acquire bounded durability capacity and
  fully prepare the owned commit-record body, RocksDB keys, and publication
  slot. The permit is still detached: it has no `CacheSeq` and is not visible
  to the ordered writer. This closes the allocation/backpressure publish gap
  without forcing transactions that later conflict to consume log positions.
- Persist each transaction as one checksummed, versioned commit record and
  apply its RocksDB mutations with one atomic `WriteBatch`.
- Define `flush()` as: every transaction acknowledged before the call has a
  durable commit record and atomically applied RocksDB batch. Keep the current
  write-back choice explicit: an unflushed crash may lose an acknowledged
  tail.
- Recover only complete records, replay each record as one transaction, and
  make replay idempotent by commit ID.

The selected first-slice protocol is below. It supersedes the earlier design
that put a global commit gate around native commit and assigned a sequence plus
cancellation marker before Silo validation. The existing vertical-slice work
already exercises record validation, ordered writeback, atomic RocksDB batches,
retry/fail-stop behavior, native multi-key transactions, and reopen recovery.
Those component tests do not by themselves establish that this revised
preinstall-hook protocol, Phase 1E, or Milestone 1 has passed. Transactional
scan read-your-writes and its C ABI, safe Rust, and cache integration slice are
complete. An initial fresh-process SIGKILL smoke gate now covers six outer
protocol boundaries, but the remaining ABI boundary gates and Phase 1F's
exhaustive process-crash injection remain open.

1. **Prepare a detached permit before native commit.** Acquire one unit of
   bounded queue capacity and own every mutation byte, encoded-record buffer,
   tagged RocksDB key, and publication cell needed by the transaction. Record
   preparation performs all size checks and fallible allocation while Silo
   holds no commit locks. Fixed-width `CacheSeq`/`MakoTimestamp` fields and the
   final checksum remain to be filled. The detached permit occupies capacity
   but no ordered queue position.
2. **Allocate Mako's timestamp under Silo's locks, then validate.** Native
   commit performs its existing phase-1 predicate checks while collecting and
   locking the write set. Once the complete write set is locked, it allocates a
   checked, nonzero base `tid_unique_` from Mako's local logical counter, then
   performs the remaining read-set and participant validation. The clock stores
   the next value to return and uses one value beyond the valid base range as
   its exhausted sentinel. Silo's lock order
   supplies the serialization constraints; allocating while the locks are held
   assigns Mako's transaction-history timestamp consistently with them. A lock
   or validation conflict drops the detached permit. It consumes no `CacheSeq`,
   creates no queue slot, and therefore needs no cancellation marker; only the
   already allocated Mako timestamp may contain a harmless gap.
3. **Bind after validation and before install.** After every validation has
   succeeded, but before phase 3 can make any write visible, native code calls
   a narrow preinstall hook with `MakoTimestamp`. Under only the queue's short
   metadata lock, the hook checks fail-stop health, assigns the next
   `CacheSeq`, fills the two fixed-width fields, and moves the preallocated
   cell into a Prepared queue slot. This bind is allocation-free and performs
   no RocksDB IO. Rejection is a definite native abort because install has not
   begun. A Rust panic is converted to rejection in unwind-enabled builds;
   the workspace release profile uses `panic = "abort"`, so production hook
   code must remain non-panicking and a violated invariant fail-stops the
   process before unwinding can cross C.
4. **Install, finalize, and acknowledge.** Silo installs the write set and
   returns success. The transaction-wide Mako timestamp is retained as history
   metadata even though the current nonopaque MassTrans profile advances its
   per-record versions separately. Outside the native lock critical section,
   Rust computes/fills the record checksum and atomically changes the bound
   slot from Prepared to Ready. Only then is the transaction acknowledged.
   The bounded queue is volatile, so an acknowledged but unflushed tail may
   still be lost on process crash under the selected write-back contract.
5. **Pin any ambiguous post-bind outcome.** Once bind succeeds, no failure may
   be treated as a normal conflict or cancellation. If native install/cleanup
   has an unknown outcome, or publication cannot prove the bound record Ready,
   that `CacheSeq` pins the queue and durability watermark. The exact finalized
   write set remains attached to every ambiguous slot, and a known-committed
   suffix is likewise retained and pinned if an earlier ambiguity wins the
   publication race. New binds fail and no later sequence can be acknowledged
   or applied until a higher-level recovery protocol resolves the ambiguity.
6. **Apply exactly one atomic RocksDB batch.** The background writer consumes
   Ready slots strictly in `CacheSeq` order. One `WriteBatch` contains the
   retained commit record and every put/delete in that transaction; a single
   RocksDB write atomically applies the whole batch before advancing the
   durable watermark. A backend failure retains the same Ready record and
   retries without letting a later sequence pass it. Ordinary conflicts never
   appear in this queue.
7. **Recover both state and Mako's clock.** Durable commit records are
   retained. Reopen validates record version, checksum, `CacheSeq`, and the
   checked `MakoTimestamp` range, then replays records in cache-sequence order
   and idempotently materializes their mutations. Before accepting any new user
   transaction, recovery advances Mako's process-wide local logical counter so
   its next `tid_unique_` is strictly greater than the maximum recovered Mako
   timestamp. Clock overflow or inability to establish that floor makes open
   fail; recovery must never manufacture a Mako timestamp from `CacheSeq`.
   The first slice exposes one default logical table and uses a tagged RocksDB
   key format separating user data, commit records, and future internal
   namespaces; compatibility or migration from `mrx`'s raw-key layout remains
   a separate task.

The timestamp switch bumps the draft commit-record value format from v2 to v3:
v2 carried a 64-bit Silo TID, while v3 carries the exact 32-bit base
`MakoTimestamp`. Recovery rejects v2 rather than guessing or truncating a
timestamp. This is allowed while both the C ABI and durable format remain
pre-v1; a production format must ship an explicit migration policy.

There is deliberately no per-database/global commit gate in this protocol.
Disjoint native transactions may lock, validate, bind, and install
concurrently. For this single-machine protocol, `MakoTimestamp` is the
transaction-history timestamp and remains separate from the current nonopaque
row version. The much shorter queue metadata lock exists only to allocate
`CacheSeq` and append an already prepared slot.

Concurrent disjoint transactions may bind in a different `CacheSeq` order than
their Mako timestamps. That is safe in this slice because their mutations
commute; neither number space is derived from the other.

- This slice is unbounded and local: it has no value eviction, distributed
  routing, 2PC, replication, or distributed-finality semantics.
- Phase 1 admits one recovered durable cache namespace per process. Native
  tables and the timestamp authority are process-wide, so independently
  opening a second pre-existing backend after work begins cannot retroactively
  preserve history. Supporting multiple caches requires a supervisor that
  scans and floors every backend before admitting any transaction.

The RocksDB durability mode qualifies what the durable watermark means; the
API and tests must not collapse these modes into one promise:

- `Sync`: the atomic batch uses the RocksDB WAL and synchronously flushes it;
  the watermark represents stable-storage completion subject to the storage
  stack's fsync guarantees.
- `Wal`: the WAL is enabled but not synchronously flushed. A completed batch
  is recoverable after a process crash, while a machine or power failure may
  lose the OS-cached tail.
- `None`: the WAL is disabled. Batch application remains atomic while the
  process is live, but the watermark is not a crash-durability guarantee; this
  mode is only valid when RocksDB is itself disposable or for explicit test
  and benchmark configurations.

Do not reuse Mako's current recovery behavior unchanged: it applies logged
key/value pairs as separate one-operation STO transactions and therefore does
not establish atomic recovery of a multi-key commit.

### 1F. Recovery and crash gates

The initial smoke gate is complete for six named boundaries: after detached
preparation, after preinstall bind, after native commit but before Ready,
after Ready but before backend application, after the backend write but before
durable-watermark advancement, and after durable-watermark advancement. Each
case uses a fresh writer process, a real `SIGKILL`, and a fresh verifier process
against synchronous RocksDB; the verifier requires a three-key transaction to
recover entirely old or entirely new.

This is not the exhaustive Phase 1F gate. The following work remains:

- Crash at every boundary: before/after detached preparation, after Silo lock
  and timestamp allocation, before/after validation, before/after preinstall
  bind, during install, before/after Ready publication, during the exact
  RocksDB batch, and before/after watermark advancement.
- Add recovery/replay interruption points, including record validation, clock
  flooring, and idempotent multi-key replay.
- Assert only two recovered states for every transaction: all writes or none.
- Mutation-test stale writeback, early detached-capacity discharge,
  hook-time allocation, conflict cancellation slots, missing/premature Ready
  publication, unpinned unknown outcomes, partial replay, reordered commits,
  duplicate replay, omitted Mako timestamps, and a recovered native clock not
  advanced past the durable maximum.
- Run differential histories through the same full-history, real-time-aware
  oracle used at the native boundary, extended with durability observations.
- Deliberately inject one divergence and require the differential harness to
  turn red, avoiding the stale-artifact false-green previously found in the
  Rust cache build.

### 1G. Bounded values and eviction

After 1E/1F are stable, add value eviction while retaining a complete key
index in Silo:

- An index miss must remain authoritative absence.
- Evicted markers carry the cache commit sequence to prevent ABA fills.
- Dirty or uncovered values cannot be evicted.
- A read-through fill participates in the transaction's validation and may
  not perform unbounded blocking IO while holding native locks.
- Tombstone reclamation waits for a proven conditional-remove/RCU design.

If transactional read-through makes OCC windows unacceptable, keep the first
production Silo cache unbounded and treat bounded values as a separate design,
not a release blocker.

### Milestone 1 final acceptance gate

- Every Phase 1A-1D boundary gate is green.
- Atomic multi-key recovery under exhaustive crash injection.
- Reopen advances Mako's native logical counter past every recovered record
  before admitting work, including near-exhaustion and corrupt-timestamp tests.
- An honest commit watermark and `flush()` barrier under concurrent writers,
  write failures, and sustained overload.
- Concurrent disjoint commits demonstrate that only hook-time queue metadata
  is serialized; no database-wide native commit gate remains.
- Clean shutdown drains all accepted transactions; forced shutdown loses no
  covered transaction.
- Throughput, abort rate, p50, p99, recovery time, and log amplification are
  measured against both the current `mrx` cache and raw RocksDB.

## Milestone 2: distributed Mako with C++ Silo participants

Port the distributed control plane while retaining the local C++ engine:

1. Define a participant ABI for begin/read, batch-lock, validate, install,
   abort, and durable-record production. A participant transaction stays on
   the same affinity-pinned worker for every phase.
2. Port key routing and coordinator state to Rust. Keep Mako's point-key hash
   routing compatible first. Do not promise globally ordered range scans over
   hash shards; either merge explicit per-shard scans or adopt range sharding.
3. Reproduce the current commit order: lock remote writes, lock local writes,
   choose/merge a distributed timestamp, validate local predicates, validate
   participants, install, log/replicate, and release. Encode the state machine
   so invalid phase transitions are typed errors. Before calling that timestamp
   a history order, make every read dependency advance strictly rather than
   merge by equality, define a shard/coordinator tie-break for independent
   transactions, propagate remote timestamp-allocation failures, and floor
   every participant's next-to-return clock at the chosen value plus one.
4. Give every RPC an idempotence key, deadline, cancellation rule, and
   duplicate-response behavior. Unknown commit outcome is distinct from an
   OCC conflict.
5. Differential-test a Rust coordinator against the existing C++ coordinator
   using deterministic schedules before switching any default.

Milestone 2 must also replace or validate the legacy one-digit term packing,
restore every timestamp authority during replay/promotion, and cover remote
read-only participants during validation. Those are pre-existing distributed
gaps; Phase 1E's local clock does not claim to repair them.

The gate requires single-node and multi-node agreement, participant crash at
every 2PC phase, coordinator crash/restart, duplicate/reordered messages,
network partition, retry exhaustion, and proof that no prepared participant
is silently abandoned.

## Milestone 3: distributed durability, replication, and recovery

- Version and checksum the Rust transaction log format; retain a reader for
  the old C++ format during migration.
- Port replication adapters without coupling consensus log indexes to Silo
  versions or cache commit sequences.
- Specify exactly when a client receives success: local install, durable local
  log, or replicated quorum. Expose weaker modes only as explicit options.
- Port watermark handling and checkpointing, then test follower catch-up,
  snapshot install, leader changes, truncated/corrupt tails, and mixed-version
  rolling upgrades.
- Recovery replays a distributed transaction atomically and idempotently; a
  commit cannot reappear as independent per-key transactions.

Only after failure/recovery tests and macro benchmarks pass should the Rust
distributed path become the default.

## Milestone 4: native Rust Silo, then remove the C++ seam

Implement a native Rust local engine behind the same `mako-local` behavior,
not directly inside callers:

1. Port record/version representation and the transaction read/write set.
2. Port locking, validation, predicates, install, abort cleanup, and commit
   ordering.
3. Port Masstree access or replace it behind a separately tested ordered-index
   trait; do not combine index replacement with OCC replacement in one step.
4. Replay the three-way script corpus against direct C++, C ABI, and native
   Rust. Shadow production-like workloads and compare results and aborts.
5. Meet memory-safety, serializability, and performance gates before switching
   the local participant implementation.
6. Keep the C ABI oracle for at least one compatibility release, then remove
   C++ only after distributed recovery and rolling-upgrade tests use the Rust
   engine by default.

This order ports Mako to Rust without betting the first usable transaction
cache on a simultaneous rewrite of OCC, Masstree, RPC, replication, and
recovery.

## Immediate execution order

Transactional scan chunks, scan read-your-writes, and their C ABI, safe Rust,
and cache exposure are complete for the RYW profile. The six-boundary
fresh-process SIGKILL matrix is the initial crash smoke gate, not Phase 1F's
exhaustive gate.

1. Finish the remaining ABI-freeze work: publish the normative operation/status
   state table, reserve every existing status number, and retain draft revision
   `0` until the full boundary gate passes. Revisit the conservative transaction
   budget only with safe pre-reservation.
2. Generate or mechanically verify the Rust declarations from the C header,
   then add the symbol, build-fingerprint, and cleanup-quarantine failpoint
   gates.
3. Build the three-way deterministic differential harness and the independent
   full-history real-time/opacity oracle.
4. Run sanitizer, concurrency, and wrapper-overhead gates.
5. Extend the completed six-boundary fresh-process SIGKILL smoke gate into the
   exhaustive Phase 1F campaign. Add the native lock/timestamp/validation/install
   seams, interruption inside the RocksDB batch, and recovery/replay failures;
   then close the remaining Phase 1E publish-gap and recovery evidence without
   eviction.
6. Do not begin distributed porting until atomic local crash recovery is
   demonstrated by the exhaustive fault-injection gate.
