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
then uses that engine as the single-machine transactional cache in front of
RocksDB. In the current slice RocksDB is an asynchronously updated black box;
disk-sync policy and recovery of an unsynced tail are later work. The C ABI
remains a draft until both the executable Phase 1A-1D boundary gate is green
and the remaining Phase 1C/1D ABI-design and freeze decisions are explicitly
accepted. Passing the executable gate does not itself promote revision 0 to
ABI v1. Later milestones move Mako's distributed orchestration into Rust, then
replace the local C++ engine only after its behavior is captured by an
executable compatibility suite.

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
5. **Backend application is transaction-granular.** One committed transaction
   becomes one atomic RocksDB `WriteBatch`. A later durability milestone must
   preserve the same unit when it defines sync and recovery guarantees.
6. **MassTrans OCC versions, cache sequence numbers, and Mako timestamps
   remain separate types and number spaces.** `MakoTimestamp` wraps the
   checked, nonzero 32-bit `tid_unique_`; `CacheSeq` orders local application
   obligations; and MassTrans row versions remain engine-private validation
   state. Accidental comparison or conversion between them should be
   impossible in Rust.
7. **Process-lifetime native resources are honest in the API.** STO has 460
   process-lifetime thread slots, and current MassTrans teardown lacks a
   verified global RCU quiescence protocol. The first ABI does not pretend
   those resources can be cheaply recycled.
8. **Distributed code depends on an engine-neutral Rust trait.** A
   `LocalTransactionEngine`/participant trait sits above `mako-local`; raw C
   handles never leak into routing, write-back, or 2PC. That makes the final
   C++-to-Rust engine swap local rather than another distributed rewrite.

## Target architecture

```text
Rust application / database facade
                |
        mako-cache transaction API
                |
       +--------+---------------------------+
       |                                    |
local transaction participant          async application pipeline
       |                                    |
safe `mako-local` crate                 transaction records / RocksDB batches
       |                                    |
raw `mako-local-sys` declarations       in-memory applied watermark
       |                                    |
`mako_local_*` C ABI                    later sync/recovery policy
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
  read-set validation, then carried verbatim in the backend commit record.
  Validation may abort after allocation, so gaps are expected. The
  single-machine path has no remote participant timestamps to merge. The
  current distributed path also takes maxima from remote participants and the
  read set, but permits ties and therefore must not yet treat this scalar as a
  globally unique history key.
- **`CacheSeq`** is the local, nonzero application-queue sequence. It is
  allocated only when a validated transaction binds its prepared record in
  the preinstall hook. It orders publication, RocksDB application, replay, and
  the in-memory applied watermark; it is not an OCC version or a distributed
  timestamp.
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
fields, and capabilities while Phases 1A-1D are completed. A green executable
boundary gate below is necessary but not sufficient for ABI-v1 promotion: the
remaining options, worker-context, output-rule, and process-lifetime design
decisions in Phases 1C/1D must also be accepted in an explicit freeze review.
After that promotion, exported symbols and numeric statuses are permanent
reservations. `DUPLICATE_WRITE` is now a legacy/no-RYW result rather than part
of the default profile, but its assigned number remains reserved. Semantic
expansions are advertised by capability bits or a later ABI revision rather
than silently changing v1.

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
      normal cleanup path. Typed cleanup failure permanently quarantines the
      worker and remains observable through `WorkerHealth` and the process
      counter even though Rust `Drop` cannot return it.
- [x] Initial C++ and Rust tests for multi-key/multi-table commit, abort,
      missing versus empty, binary bytes, verb results, nested begin,
      wrong-thread use, finished handles, and deterministic conflict.
- [x] One process-wide STO thread-ID space and one dedicated MassTrans
      Masstree RCU context, with its epoch advanced by the shared runtime.
- [x] Replaced the initial timestamp-only stale-artifact guard with a
      source/configuration-derived native fingerprint, exact CMake libc++
      discovery, a digest-named archive anchor, and a required-native
      CMake/Cargo test mode. Modification times remain advisory only.
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
- [x] Publish the revision-0 operation/status and ownership contract at
      [Mako local C ABI revision 0](../reference/mako-local-abi-v0.md), including
      the active/finished/quarantined/destroyed state model and the conservative
      one-shot destroy rule for `WORKER_POISONED` and terminal uncertainty.
- [x] Record a green from-scratch run of the executable contract gates below.
      Candidate `5a3dd3eaf` passed every row on 2026-08-25; the implementation,
      exact commands, and retained evidence are documented at
      [Mako local boundary gates](../mako-local-boundary-gates.md).

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
- Numeric table IDs are unique within a database. The revision-0 reference now
  specifies empty names and serialized concurrent opens; add the remaining
  direct concurrency coverage. Closing the
  in-memory facade is not persistence: a later `db_open` starts a new logical
  database even though old native table allocations remain process-lifetime.
- [x] Publish a normative revision-0 operation/status state table. The
      [reference contract](../reference/mako-local-abi-v0.md) covers every
      export and status, output initialization and ownership, transaction
      disposition, worker health, and destroy requirements. It records the
      current conditional all-output-pointer rule and requires both scan
      feature bits before a raw scan call.
- [x] Generate Rust status identity from the header's canonical manifest and
      exhaustively classify every generated status for ordinary operations and
      commit disposition. Required-native open also checks every linked status
      message, so a stale C++ catalog is rejected rather than silently mapped.
- [x] Add fake-ABI coverage for every active, finished, and quarantined
      transition and for malformed successful outputs.
- [x] Add a stable engine/build identifier. CMake embeds a
      content/configuration fingerprint covering the canonical header, the
      configured source and dependency closure of every linked native archive,
      relevant compile definitions such as RYW and opacity,
      compiler/standard-library identity, and generated configuration. Cargo
      independently verifies that fingerprint and treats modification times
      as advisory only; a digest-named link anchor prevents a manifest from
      blessing a different archive.
- Reserve a sized database/open options entry point before ABI v1 is declared
  frozen, so later limits and durability modes can be negotiated without
  changing existing function signatures. The existing scan options struct is
  already sized, but `mako_local_db_open()` has no general options seam yet.
- [x] Make the C header the single source of truth for `mako-local-sys` through
      pinned build-time generation of every constant, type, callback, and
      function declaration. Strict C11 and C++ conformance translation units,
      a Rust all-export link probe, and an exact exported-symbol allowlist catch
      signature, constant, `noexcept`/calling-convention, feature-bit, and
      status-number drift even when artifacts report the same ABI revision.
- Decide whether ABI v1 remains implicit TLS attachment or gains an opaque
  worker context. Either way, test attachment, wrong-thread calls, nested
  begin, post-terminal calls, database-close-while-busy, and the 460-thread
  limit in isolated subprocesses.
- [x] Specify revision 0's conditional output rule: after every required output
      pointer has been validated, initialize every scalar output before later
      validation. If a multi-output call receives a partially null output set,
      it writes none of that set. Decide before ABI v1 whether to retain this
      rule or initialize each non-null member of an invalid set independently.
- [x] Define cleanup failure conservatively beyond the live-handle
      path. If native abort or destroy cannot prove cleanup complete, retain
      every potentially referenced allocation, mark the attached worker
      poisoned, reject all later transactions on it, and expose the poison
      through status 19, a TLS health check, and a monotonic diagnostic counter.
      Failed begin cleanup installs the same independent TLS quarantine even
      though no facade is returned. Five test-only cleanup boundaries cover
      begin, terminal operations, commit, explicit abort, and active destroy;
      never silently reuse uncertain STO TLS state.
- Specify process-lifetime table/epoch behavior. Add ordinary teardown only
  after a tested RCU quiescence protocol exists.

### 1D. Complete the safe Rust layer

- [x] Add owned forward and reverse scan iterators over the chunk API, with
  feature-gated scan RYW, caller-owned chunk storage, and default-table
  exposure through `mako-cache`.
- Add compile-fail tests proving a transaction cannot move threads, outlive
  its database, or be held in a spawned async task.
- [x] Unit-test the generated/verified status mapping and abort-on-drop against a
  fake ABI so Miri can exercise the ownership logic without C++. The fake must
  cover every active, terminal, and poisoned transition in the normative state
  table. An unknown future status returned during an active transaction is
  terminal-uncertain: the wrapper ends local use of the transaction, performs
  one destroy probe, and gates every later safe table-open or transaction-begin
  admission through mandatory re-attach. Re-attach checks the same
  authoritative native TLS quarantine flag as the health query, so it permits
  reuse only while the worker remains healthy. Every revision-0 status
  extension must preserve the invariant that cleanup which cannot be proved
  complete sets that quarantine before return.
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
   RMW, transaction sizes 1/4/16/64, and low/high conflict rates. Every
   low-contention configuration is recorded, per-workload median/maximum
   wrapper tax has a same-host advisory budget, and every high-contention
   write/RMW configuration proves it actually generated conflicts.
6. Generated-binding or clean-regeneration checks, C11/C++ header probes, the
   Rust link probe, exported-symbol allowlist, ABI revision, and embedded build
   fingerprint all agree in a from-scratch required-native build. CI fails if
   Cargo is unavailable or native tests are skipped.
7. The five native cleanup failpoints at begin, terminal operation, commit,
   explicit abort, and active destroy demonstrate deterministic quarantine,
   one counter increment, and permanent worker rejection. No test may pass by
   ignoring a Drop error, retrying cleanup, reusing uncertain TLS state, or
   loading a stale native artifact.

The implementation, reproducible commands, exact concurrency/benchmark
methodology, retained artifacts, and execution status are maintained in
[Mako local boundary gates](../mako-local-boundary-gates.md). Treat the
executable gate as complete only when that validation record is entirely
green. Even then, ABI revision 0 remains a draft until the explicit Phase
1C/1D design/freeze work above is accepted.

This intermediate boundary gate excludes RocksDB durability and eviction. It
is not completion of Milestone 1; distributed routing, 2PC, replication, and a
native Rust OCC implementation remain outside Milestone 1 entirely.

Before Phase 1E exposes a public database contract, name five states
separately: **visible** in Silo, **acknowledged** to the caller, **applied** to
RocksDB, **durable** under a future disk-sync rule, and **final** after the
configured replication rule. The current milestone implements only the first
three. No API may use the single word “committed” when those states differ.

### 1E. Correct unbounded asynchronous write-back cache

Phases 1A-1D establish an in-memory engine binding. This phase adds ordered,
asynchronous application to a black-box RocksDB backend.

Create a new `mako-cache` layer rather than adding transaction semantics to
`mrx-core`:

- Silo/MassTrans is the authoritative live state while the process runs.
- RocksDB is an asynchronously updated materialization. This phase does not
  define recovery of an unflushed tail.
- Start unbounded: every live value remains in Silo. Eviction is a later
  subphase so it cannot obscure transaction/durability correctness.
- Assign a cache commit sequence distinct from Mako's logical timestamp and
  MassTrans's per-record OCC versions.
- Before entering native commit, acquire bounded write-back capacity and
  fully prepare the owned commit-record body, RocksDB keys, and publication
  slot. The permit is still detached: it has no `CacheSeq` and is not visible
  to the ordered writer. This closes the allocation/backpressure publish gap
  without forcing transactions that later conflict to consume log positions.
- Store each transaction as one checksummed, versioned commit record and
  apply its RocksDB mutations with one atomic `WriteBatch`.
- Define `wait_applied()` as: every transaction acknowledged before the call
  has reached a successful atomic RocksDB batch. The compatibility spelling
  `flush()` means the same thing and must not add a separate RocksDB flush, WAL
  sync, or `fsync` beyond the configured ordinary batch writes.
- Keep the applied watermark only in memory. Recovery from complete backend
  records may reconstruct it on open, but recovery of an unflushed log tail is
  outside this phase.

The selected first-slice protocol is below. It supersedes the earlier design
that put a global commit gate around native commit and assigned a sequence plus
cancellation marker before Silo validation. The existing vertical-slice work
already exercises record validation, ordered writeback, atomic RocksDB batches,
retry/fail-stop behavior, native multi-key transactions, and reopen recovery.
Those component tests do not by themselves establish that this revised
preinstall-hook protocol, Phase 1E, or Milestone 1 has passed. Transactional
scan read-your-writes and its C ABI, safe Rust, and cache integration slice are
complete. The fresh-process SIGKILL gate now covers ten outer/Rocks-wrapper
write-path boundaries in the production-default profile and all sixteen named
boundaries in a dedicated native-hook profile. Eight recovery/replay boundaries
are each interrupted on two consecutive fresh-process restarts. The native
boundary now also has a process-isolated direct-C++/C-ABI/safe-Rust
differential gate and an independent strict-serializability/opacity oracle.
Item 4's remaining Phase 1A-1D sanitizer/Miri, fixed-worker concurrency, and
relative-overhead gates passed on candidate `5a3dd3eaf` on 2026-08-25. The
authoritative evidence is the validation record in
[Mako local boundary gates](../mako-local-boundary-gates.md). Phase 1F's
application-aware correctness gate passed on implementation commit
`5546062af` on 2026-08-25; its separate
[Item 5 validation record](../mako-local-boundary-gates.md#item-5-phase-1f-validation-record)
retains the evidence.
Interruption inside RocksDB's WAL is deliberately not a milestone gate:
RocksDB remains a black box.

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
   The bounded queue is volatile, so an acknowledged but unapplied tail may be
   lost on process crash. This phase also makes no promise for an applied but
   unsynced RocksDB tail.
5. **Pin any ambiguous post-bind outcome.** Once bind succeeds, no failure may
   be treated as a normal conflict or cancellation. If native install/cleanup
   has an unknown outcome, or publication cannot prove the bound record Ready,
   that `CacheSeq` pins the queue and applied watermark. The exact finalized
   write set remains attached to every ambiguous slot, and a known-committed
   suffix is likewise retained and pinned if an earlier ambiguity wins the
   publication race. New binds fail and no later sequence can be acknowledged
   or applied until a higher-level recovery protocol resolves the ambiguity.
6. **Apply exactly one atomic RocksDB batch.** The background writer consumes
   Ready slots strictly in `CacheSeq` order. One `WriteBatch` contains the
   retained commit record and every put/delete in that transaction; a single
   RocksDB write atomically applies the whole batch before advancing the
   in-memory `AppliedWatermark`. That watermark is the pair consisting of the
   dense `CacheSeq` frontier and the exact `MakoTimestamp` on that frontier;
   the sequence proves contiguity, while the timestamp identifies the Mako
   transaction. A backend failure retains the same Ready record and leaves the
   watermark unchanged, without letting a later sequence pass it. Ordinary
   conflicts never appear in this queue.
7. **Validate complete backend history on open.** Reopen validates any records
   RocksDB presents by version, checksum, `CacheSeq`, and checked
   `MakoTimestamp`, then replays them in cache-sequence order. It reconstructs
   the in-memory applied position from the last `CacheSeq` record, while
   separately flooring Mako's process-wide clock past the maximum recovered
   timestamp. This validation does not promise recovery of a RocksDB tail that
   had not been synced before a machine failure. The first slice exposes one
   default logical table and uses a tagged RocksDB key format separating user
   data, commit records, and future internal namespaces; compatibility or
   migration from `mrx`'s raw-key layout remains a separate task.

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
- Phase 1 admits one recovered cache namespace per process. Native
  tables and the timestamp authority are process-wide, so independently
  opening a second pre-existing backend after work begins cannot retroactively
  preserve history. Supporting multiple caches requires a supervisor that
  scans and floors every backend before admitting any transaction.

The in-memory applied watermark has one meaning in every RocksDB write mode:
the complete ordered batch is confirmed present in RocksDB. During live
application that confirmation is a successful `rocksdb_write` return; during
open it is validated backend history. It never means “synced.” The current
production default is `Wal`: ordinary writes use `sync=false`, and the cache
adds no separate `FlushWAL`, `SyncWAL`, or memtable-flush call.

- `Sync`: an explicitly configured atomic batch asks RocksDB to synchronize
  its WAL. This lower-level option is useful for separate durability tests but
  is not required by this milestone.
- `Wal`: the WAL is enabled but not synchronously flushed. A completed batch
  has been accepted by RocksDB, while a machine or power failure may lose the
  OS-cached tail.
- `None`: the WAL is disabled. Batch application remains atomic while the
  process is live; this mode is only valid when RocksDB is disposable or for
  explicit test and benchmark configurations.

RocksDB 9.10 exposes a latest sequence number through the C API, but that is
an accepted-write position rather than a passive last-synced position. It also
exposes active flush operations, which this phase intentionally does not call.
If a future version provides a sound passive sync notification, record it as a
separate observed-durable watermark rather than changing `AppliedWatermark`.

Do not reuse Mako's current recovery behavior unchanged: it applies logged
key/value pairs as separate one-operation STO transactions and therefore does
not establish atomic recovery of a multi-key commit.

### 1F. Recovery and crash gates

The write path has test-only native points at the exact Silo seams after the
complete write set is locked, after Mako timestamp allocation, after local
validation, after preinstall acceptance, after the first of multiple installs,
and after all installs. Rocks wrapper points bracket batch construction and the
public `rocksdb_write` call. The milestone uses those component boundaries to
verify publication and applied-watermark ordering; it does not inspect
RocksDB's WAL implementation.

The repository also retains a stronger synchronous-Rocks SIGKILL/recovery
matrix from earlier work. It remains useful ancillary coverage for complete
records, clock flooring, and replay, but it is not a requirement for this
asynchronous milestone. Recovery of an unflushed log tail, forced sync, torn
WAL simulation, and interruption inside RocksDB are deferred to the later
durability milestone. No private RocksDB C++ shim is required here.

The Phase 1F correctness gate is complete for the current asynchronous
contract:

- Pre-preparation plus every reachable cache abort/commit-cleanup path has a
  fresh-worker quarantine assertion. The raw ABI independently covers all five
  native cleanup seams, including destroy.
- The strict isolated suite mutation-tests stale writeback, early detached
  capacity discharge, hook-time allocation, conflict cancellation slots,
  missing/premature Ready publication, unpinned unknown outcomes, partial
  replay, reordered commits, duplicate replay, wrong Mako timestamps, and a
  recovered native clock not advanced past the recovered maximum; all twelve
  mutants are killed only by their designated exact tests.
- Synthetic and real cache histories run through the transaction oracle first,
  then add cache order, backend batches/retries, visible/applied frontiers,
  wait barriers, pinned suffixes, and one-global-clock validation.
- Deliberate decoded-batch divergence turns the same full-history checker path
  red; partial materialization is rejected earlier by transcript decoding.

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
- Atomic multi-key application through one black-box RocksDB `WriteBatch`.
- Reopen advances Mako's native logical counter past every recovered record
  before admitting work, including near-exhaustion and corrupt-timestamp tests.
- An honest in-memory `AppliedWatermark` and `wait_applied()` barrier under
  concurrent writers, write failures, and sustained overload; neither claims
  disk sync.
- Concurrent disjoint commits demonstrate that only hook-time queue metadata
  is serialized; no database-wide native commit gate remains.
- Clean shutdown drains all accepted transactions to RocksDB; forced shutdown
  may discard only the unapplied in-memory tail.
- Throughput, abort rate, p50, p99, recovery time, and log amplification are
  measured against both the current `mrx` cache and raw RocksDB.

## Milestone 2: distributed Mako with C++ Silo participants

Port the distributed control plane while retaining the local C++ engine:

1. Define a participant ABI for begin/read, batch-lock, validate, install,
   abort, and commit-record production. A participant transaction stays on
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
and cache exposure are complete for the RYW profile. The hook-enabled
fresh-process suite now exercises sixteen write-path and eight repeated
recovery boundaries. The in-memory applied watermark is now explicit and
advances only after a successful ordered backend call. Item 3's native
history oracle is complete. Item 4's sanitizer/Miri, fixed-worker concurrency,
and overhead gate was accepted on candidate `5a3dd3eaf`; the linked
[validation record](../mako-local-boundary-gates.md#validation-record) retains
the evidence. Item 5's Phase 1F cleanup, mutation, and application-history gate
was accepted on implementation commit `5546062af`; the linked
[Item 5 validation record](../mako-local-boundary-gates.md#item-5-phase-1f-validation-record)
retains the evidence. Inside-RocksDB instrumentation is intentionally outside
this milestone.

1. The revision-0 operation/status contract and numeric reservations 0 through
   19 are now published and mechanically checked across the C header, C++
   diagnostics, raw Rust declarations, and exhaustive safe-Rust lifecycle
   policy. Typed poison, independent TLS worker health, begin cleanup,
   one-shot cleanup, a monotonic quarantine counter, five native cleanup
   failpoints, and fake-ABI/Miri ownership coverage complete this item.
   Retain draft revision `0` through the full executable boundary gate and the
   subsequent explicit Phase 1C/1D design/freeze review. Revisit the conservative
   transaction budget only with safe pre-reservation.
2. The C header now generates the raw Rust declarations. Strict C11/C++
   conformance probes, a Rust all-export link probe, an exact native symbol
   allowlist, and the source/configuration-derived fingerprint plus digest link
   anchor complete this item in both production and hook-enabled profiles.
3. The three-way deterministic differential harness and independent
   full-history real-time/opacity oracle complete this item. The gate replays
   one binary-safe corpus through direct MassTrans C++, the raw C ABI, and the
   safe Rust API in isolated processes; it compares every observation and
   final table state. Seeded and crafted coverage includes point operations,
   aborts, RYW, bounded/unbounded scans in both directions, scan chunking, and
   maximum-sized binary keys. The independent model does not use Silo or Mako
   timestamps and proves that commit response order is not assumed to be the
   serialization order. Native forced RW, WW, and phantom schedules run in
   both advertised non-opaque and opaque profiles. Comparison mismatches retain
   their exact corpus and all three transcripts; earlier process failures
   retain the corpus plus every available output/error for deterministic
   replay. An injected child-process divergence exercises the same comparison
   path.
4. The executable Item 4 gate now supplies pinned fake-ABI Miri; strict
   ASan/UBSan integration and TSan with reviewed suppressions; exact
   1/4/16-worker conflict, abort, progress, and soak coverage; and an
   opt-in optimized direct-C++ -> raw-ABI -> safe-Rust overhead matrix. The
   benchmark validates every configured key, requires at least one-percent
   aggregate conflict rate
   in each high-contention write/RMW configuration, records every
   low-contention ratio plus its per-workload median/maximum, and applies the
   initial `6.0x` ceiling only as a same-host advisory sanity check. Every row
   in the [validation record](../mako-local-boundary-gates.md#validation-record)
   passed on candidate `5a3dd3eaf` on 2026-08-25. That completes the numbered
   executable Phase 1A-1D gate, not the revision-0 design/freeze work or
   Milestone 1.
5. Phase 1F is complete on implementation commit `5546062af`. Four
   fresh-worker cache cleanup/quarantine scenarios complement all five raw ABI
   seams; the 12-mutant isolated suite has no survivor or harness error; and the
   application-aware oracle accepts real sequential and response-reordered
   concurrent cache histories while rejecting injected divergence. The
   dedicated hook-enabled profile is mandatory for native seam tests; the
   production-default native commit hot path contains no observer branches.
   See the
   [Item 5 validation record](../mako-local-boundary-gates.md#item-5-phase-1f-validation-record).
6. Treat disk-sync observation, unflushed-tail recovery, and log reclamation as
   a separate durability milestone. They do not block beginning the
   distributed Rust port once the local transaction and ordered-application
   contract passes its gate.
