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
disk-sync policy and recovery of an unsynced tail are later work. The timestamp
cutover deliberately promotes the local C ABI to revision 1 and rejects the old
32-bit timestamp and cache-record formats. Database open retains its sized
options seam, worker attachment remains implicit TLS, the conditional
all-output rule is retained, native table/epoch state is honestly
process-lifetime, and async callers use the fixed-worker adapter. Later
milestones move Mako's distributed orchestration into Rust, then replace the
local C++ engine only after its behavior is captured by an executable
compatibility suite.

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
   Async deployments submit complete synchronous closures to the bounded,
   long-lived `FixedWorkerPool`; they never move a native transaction through
   an executor.
4. **Conflicts do not cause invisible retries.** Commit returns `Conflict`;
   the caller decides whether and how to rerun application logic.
5. **Backend application is transaction-atomic and lane-prefix-batched.** Each
   worker publishes to its own single-producer lane. A physical RocksDB
   `WriteBatch` may hold a bounded contiguous prefix from one lane, but batches
   from different lanes need not follow serialization order. Every batch keeps
   each transaction's commit record and selected materialized mutations atomic.
   Mako timestamps, not physical batch order, decide which value wins.
6. **MassTrans OCC versions, physical log IDs, and Mako timestamps remain
   separate types and number spaces.** The single-machine cache uses the
   16-byte HLC `MakoTimestamp { physical_us, logical, origin }`. A `CacheSeq`
   identifies a position in one physical writeback lane. MassTrans row
   versions remain engine-private validation state. The existing distributed
   C++ path temporarily keeps its legacy 32-bit `tid_unique_`; it cannot be
   converted to or stored as the new type. Accidental comparison or conversion
   between these values should be impossible in Rust.
7. **Process-lifetime native resources are honest in the API.** STO has
   exactly 460 process-lifetime thread slots, and current MassTrans teardown
   lacks a verified global RCU quiescence protocol. The first ABI does not
   pretend those resources can be cheaply recycled.
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
local transaction participant          per-worker SPSC writeback lanes
       |                                    |
safe `mako-local` crate                 shared timestamp apply coordinator
       |                                    |
raw `mako-local-sys` declarations       black-box RocksDB batches
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

- **`MakoTimestamp`** is the 16-byte tuple `(physical_us, logical, origin)`.
  The single-machine cache allocates it after final Silo validation succeeds
  and while the complete write set remains locked. The local C ABI, Rust type,
  v5 and v6 cache records, replay comparisons, and recovery floor all carry
  that tuple without truncation. The first implementation uses a fixed origin
  of 1 because one process owns the local namespace. A distributed deployment
  requires leased nonzero origins.
- **`CacheSeq`** is a nonzero physical log ID. Concurrent mode stores the
  one-based worker lane tag in the upper 16 bits and a dense lane-local
  sequence in the low 48 bits. The lane tag is the cache's process-lifetime
  thread slot plus one; it need not equal STO's independent worker ID. An upper
  16-bit value of zero denotes the untagged dense stream used by current
  SingleProducer mode and accepted during recovery. A `CacheSeq` orders records
  only within its lane; it is not a global serialization order, an OCC version,
  or a progress count.
- **MassTrans row versions** remain the current nonopaque profile's per-record
  OCC counters. Carrying the HLC timestamp does not replace them. An opaque STO
  profile may separately use the 64-bit `commit_tid_` clock for row versions,
  but Phase 1E does not persist that value.
- **The legacy distributed commit ID** remains `tid_unique_ * 10 + term` in
  the current C++ Paxos log, value trailer, and watermark code. Phase 1 does
  not widen or reinterpret it. Milestone 2 removes this packing, uses the full
  HLC timestamp for transaction order, and stores the failure epoch in a
  separate field.

## Milestone 1: C++ Silo as the single-machine transaction cache

### 1A. Point-transaction C ABI vertical slice

**ABI revision policy.** The checked-in implementation reports
`MAKO_LOCAL_ABI_VERSION == 1`. This is an intentional clean break: revision 1
uses the 16-byte HLC timestamp, cache-record formats v5/v6, and no compatibility
reader for revision 0 timestamp records. Existing option-structure revision
tags remain zero because those layouts did not change. Exported revision-1
symbols and numeric statuses are permanent reservations. `DUPLICATE_WRITE` is
now a legacy/no-RYW result rather than part of the default profile, but its
assigned number remains reserved. Later semantic expansions use capability
bits or a later ABI revision rather than silently changing revision 1.

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

The Phase 1B functional contract is complete. Direct native schedules and the
independent history oracle cover RW, WW, predicate/phantom, abort, progress,
and both advertised isolation profiles. The three-way scripts cover binary
point/scan bounds, maximum-sized keys, and chunk resumption. An exact 1 MiB
value is exercised through both abort and commit/readback paths. Repeated point
mutations on local single-version tables are covered through native, C ABI,
safe Rust, in-memory write-back, and RocksDB recovery tests, including operation
results, final bytes, canonical one-mutation-per-key log records, value growth,
and net no-op histories. Status 12 remains reserved for linked legacy/no-RYW
engines.

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

The Phase 1B semantics are frozen for the single-machine profile. Point
transactions, scans, and conventional read-your-writes are required by
default. Opacity remains an explicit profile rather than an implication of the
ABI revision: builds without it must pass strict serializability checks for
committed transactions, while a build advertising `OPACITY` must additionally
pass the aborted/in-flight observation checks. `CacheOptions::isolation`
declares the deployment requirement, defaults to `StrictSerializable`, and
rejects startup with `MissingOpacity` when `Opaque` is selected against a
non-opaque native engine.

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
- [x] Numeric table IDs are unique within a database. The revision-0 reference
      specifies empty names and serialized concurrent opens. Direct concurrent
      tests cover identical name/ID reuse, one name racing with two IDs, and
      two names racing for one numeric ID. Closing the in-memory facade is not
      persistence: a later `db_open` starts a new logical database even though
      old native table allocations remain process-lifetime.
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
- [x] Reserve a sized database/open options entry point before ABI v1.
      `mako_local_db_options_size()`, `MAKO_LOCAL_DB_OPTIONS_V0_SIZE`, and
      `mako_local_db_open_with_options()` define an append-only prefix. Revision
      0 currently accepts only zero flags; the original `mako_local_db_open()`
      remains the default-options spelling.
- [x] Make the C header the single source of truth for `mako-local-sys` through
      pinned build-time generation of every constant, type, callback, and
      function declaration. Strict C11 and C++ conformance translation units,
      a Rust all-export link probe, and an exact exported-symbol allowlist catch
      signature, constant, `noexcept`/calling-convention, feature-bit, and
      status-number drift even when artifacts report the same ABI revision.
- [x] Retain implicit TLS attachment for this boundary. Attachment,
      wrong-thread calls, nested begin, post-terminal calls, and
      database-close-while-busy are covered. A process-isolated probe creates
      and joins exactly 460 distinct workers, checks that attachment is
      idempotent on each one, then requires worker 461 to return
      `THREAD_LIMIT`. This proves the limit is a process-lifetime reservation,
      not merely a simultaneous-thread ceiling.
- [x] Specify revision 0's conditional output rule: after every required output
      pointer has been validated, initialize every scalar output before later
      validation. If a multi-output call receives a partially null output set,
      it writes none of that set. The freeze choice retains this rule; ABI v1
      must not switch to per-member initialization without a new contract.
- [x] Define cleanup failure conservatively beyond the live-handle
      path. If native abort or destroy cannot prove cleanup complete, retain
      every potentially referenced allocation, mark the attached worker
      poisoned, reject all later transactions on it, and expose the poison
      through status 19, a TLS health check, and a monotonic diagnostic counter.
      Failed begin cleanup installs the same independent TLS quarantine even
      though no facade is returned. Five test-only cleanup boundaries cover
      begin, terminal operations, commit, explicit abort, and active destroy;
      never silently reuse uncertain STO TLS state.
- [x] Specify process-lifetime table/epoch behavior. `db_close` reclaims only
      facade and borrowed table-handle storage; native MassTrans tables, STO
      worker slots, and epoch state remain allocated until process exit.
      Ordinary native teardown requires a separately tested global RCU
      quiescence protocol and is not implied by facade close.

### 1D. Complete the safe Rust layer

- [x] Add owned forward and reverse scan iterators over the chunk API, with
  feature-gated scan RYW, caller-owned chunk storage, and default-table
  exposure through `mako-cache`.
- [x] Add compile-fail tests proving a transaction cannot move threads,
      outlive its database, or be held across suspension in a `Send` async
      task.
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
- [x] Offer a bounded `FixedWorkerPool` for async applications; the native
      transaction remains `!Send + !Sync`. Each accepted closure runs to
      completion on one long-lived worker. Health is checked after every
      closure and, in unwind-enabled builds, every caught panic; cleanup
      uncertainty retires that worker, fails its queued commands, removes it
      from routing, and is visible through task
      errors and pool metrics. `LocalDb::open()` has already consumed one of
      the 460 process-lifetime slots, so a pool pre-rejects more than 459
      workers even in a fresh process; earlier attachments can reduce the
      actual available count further. Clean shutdown drains accepted work and
      joins every healthy worker.
- [x] Provide explicit conflict retry above the transaction API.
      `RetryPolicy` bounds whole-closure reruns, only `Conflict` is retried,
      attempt/conflict counts are returned, and external side effects remain
      the caller's idempotence responsibility.

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
[Mako local boundary gates](../mako-local-boundary-gates.md). The executable
Phase 1A-1D evidence there predates the timestamp cutover and remains historical.
The current source reports ABI revision 1. Its functional verification has
passed. The combined timestamp mutation campaign also passed with all 12
mutants killed, zero survivors, and zero harness errors. The revision-1
performance sweep and canonical all-in-one hook CI gate remain pending. The
linked records keep these current results separate from pre-HLC evidence.

This intermediate boundary gate excludes RocksDB durability and eviction. By
itself it is not completion of Milestone 1; distributed routing, 2PC,
replication, and a native Rust OCC implementation remain outside Milestone 1
entirely.

Before Phase 1E exposes a public database contract, name five states
separately: **visible** in Silo, **acknowledged** to the caller, **applied** to
RocksDB, **durable** under a future disk-sync rule, and **final** after the
configured replication rule. The current milestone implements only the first
three. No API may use the single word “committed” when those states differ.

### 1E. Correct unbounded asynchronous write-back cache

Phases 1A-1D establish an in-memory engine binding. This phase adds
asynchronous application to a black-box RocksDB backend. It preserves logical
last-writer-wins and recovery semantics with Mako timestamps instead of forcing
every worker through one physical publication order.

Create a new `mako-cache` layer rather than adding transaction semantics to
`mrx-core`:

- Silo/MassTrans is the authoritative live state while the process runs.
- RocksDB is an asynchronously updated materialization. This phase does not
  define recovery of an unflushed tail.
- Start unbounded: every live value remains in Silo. Eviction is a later
  subphase so it cannot obscure transaction/durability correctness.
- Give each foreground worker a lazily initialized SPSC writeback lane. Each
  lane has its own bounded capacity and dense local sequence. After validation,
  the native engine serializes its canonical write set directly into the
  caller-provided record buffer and publishes it to that worker's lane.
- Store each transaction as one versioned commit record. CRC remains an
  optional record-format choice. The background runtime polls lanes in round-
  robin order and may apply records in a different physical order than their
  Mako timestamps.
- Route every backend batch through one shared apply coordinator. It always
  retains the commit log, but it sends a put or delete to the materialized data
  key only when that record has the greatest Mako timestamp seen for the key.
  This makes a late older record harmless without exposing RocksDB internals.
- Define `wait_applied()` as: every transaction acknowledged before the call
  has reached a successful atomic RocksDB batch. The compatibility spelling
  `flush()` means the same thing and must not add a separate RocksDB flush, WAL
  sync, or `fsync` beyond the configured ordinary batch writes.
- Keep progress only in memory. The acknowledged and applied sequence APIs
  report aggregate record counts across initialized lanes. The applied
  watermark also reports the greatest applied Mako timestamp. Neither value
  claims a contiguous global serialization prefix. Recovery from complete
  backend records may reconstruct these values on open, but recovery of an
  unflushed log tail is outside this phase.

The selected first-slice protocol is below. It supersedes both the early global
commit gate and the later global dense publication queue. Record validation,
per-worker publication, timestamp-filtered atomic RocksDB batches,
retry/fail-stop behavior, native multi-key transactions, and reopen recovery
passed revision-1 functional verification. That run checked bounded sustained
overload, clean drain and reopen, forced process stop, and near-exhaustion
recovery. Transactional scan read-your-writes and its C ABI, safe Rust, and
cache integration slice remain implemented. The fresh-process SIGKILL suite
covers ten outer or RocksDB-wrapper write-path boundaries in the
production-default profile and all sixteen named boundaries in a dedicated
native-hook profile. It also interrupts eight recovery and replay boundaries
on two consecutive fresh-process restarts. The native boundary includes a
process-isolated direct-C++, C ABI, and safe-Rust differential gate plus an
independent strict-serializability and opacity oracle.
Item 4's Phase 1A-1D sanitizer/Miri, fixed-worker concurrency, and
relative-overhead gates passed on historical candidate `5a3dd3eaf` on
2026-08-25. The
authoritative evidence is the validation record in
[Mako local boundary gates](../mako-local-boundary-gates.md). Phase 1F's
application-aware correctness gate passed on historical implementation commit
`5546062af` on 2026-08-25; its separate
[Item 5 validation record](../mako-local-boundary-gates.md#item-5-phase-1f-validation-record)
retains the evidence.
Interruption inside RocksDB's WAL is deliberately not a milestone gate:
RocksDB remains a black box.

1. **Prepare against the worker's lane before native commit.** First seal and
   preflight STO's canonical final write-set extent. Then acquire one unit of
   capacity from the current worker's lane and its publication cell, followed
   by exactly one encoded-record buffer sized from that preflight.
   No Rust mutation journal or tagged RocksDB keys are constructed on the
   foreground path; the background decoder materializes those keys later.
   Every size check and fallible allocation finishes while Silo holds no commit
   locks. The detached permit occupies lane capacity but is not visible to the
   consumer. Some fast paths also preselect a reusable lane generation and
   `CacheSeq` candidate. That candidate is not consumed until native code
   accepts it into the commit order.
2. **Validate under Silo's locks, then allocate the HLC timestamp.** Native
   commit performs its phase-1 predicate checks while collecting and locking
   the complete write set. General transactions enter the validation gate,
   perform final point and predicate validation, and only then allocate one
   HLC timestamp. Restricted transactions may validate before taking the gate
   only when their locked update proves the same ordering rule. A lock or
   validation conflict drops the detached permit before timestamp allocation.
3. **Bind after validation and before install.** Before phase 3 can make a
   write visible, native code calls a narrow preinstall hook. The public hook
   receives the full `MakoTimestamp`. Same-build fast hooks carry the private
   63-bit hot stamp and expand it losslessly with process origin 1. The hook
   checks the cache-wide fail-stop state and binds the next dense sequence in
   this worker's lane to the timestamp. The general gate remains held through
   the ordered bind. Native fills the fixed-width record fields during direct
   serialization. This bind performs no allocation or RocksDB IO. Rejection
   before irrevocable sequence binding is a definite native abort and may leave
   a harmless HLC gap. Once a nonzero sequence is accepted or advanced, any
   ownership, install, or publication uncertainty pins that exact obligation
   and latches fail-stop state. A Rust panic is converted to rejection in
   unwind-enabled builds. The workspace release profile uses `panic = "abort"`,
   so production hook code must remain non-panicking and a violated invariant
   fail-stops the process before unwinding can cross C.
4. **Serialize, install, publish, and acknowledge independently.** Native walks
   the canonical STO write set directly, fills the caller-owned record and the
   optional CRC, then Silo installs the write set. Rust attaches the complete
   bytes in constant time and publishes the record to its SPSC lane. A worker
   may acknowledge its transaction as soon as that lane publication succeeds.
   It does not wait for a lower-timestamp transaction on another worker to
   publish. The Mako timestamp records the logical serialization order even
   when physical publication order differs.
   The bounded queue is volatile, so an acknowledged but unapplied tail may be
   lost on process crash. This phase also makes no promise for an applied but
   unsynced RocksDB tail.
5. **Fail-stop unresolved irrevocable obligations.** Once the final preinstall
   ownership handoff succeeds, native install or publication uncertainty
   cannot become an ordinary conflict. The affected lane retains its exact
   finalized record when those bytes exist, and the shared unhealthy state
   rejects new work in every lane. This preserves the obligation without a
   cross-worker acknowledgement gate. A higher-level recovery protocol must
   resolve any unknown outcome.
6. **Poll lanes and apply through one coordinator.** One background runtime
   visits initialized lanes in round-robin order. It takes a bounded contiguous
   Ready prefix from a lane, then enters the shared apply coordinator. The
   coordinator serializes black-box backend calls across lanes. Every selected
   transaction contributes its commit-log operation. For each data key, the
   coordinator emits only the mutation with a timestamp newer than the latest
   timestamp recorded for that key. One RocksDB `WriteBatch` atomically stores
   those log records and winning materialized mutations before the lane
   advances. A failure retains the lane prefix for retry and leaves its applied
   position unchanged. Physical application order may differ from timestamp
   order, but an older late batch cannot overwrite a newer value or resurrect a
   newer delete.
7. **Validate complete backend history on open.** Reopen validates any records
   RocksDB presents by version, optional checksum, physical `CacheSeq`, and
   checked `MakoTimestamp`. It requires a dense local sequence and increasing
   timestamps within each tagged lane, accepts the upper-zero untagged dense
   stream, and rejects duplicate Mako timestamps across the cache. Recovery
   reconstructs the latest timestamp for every data key from the permanent commit logs,
   including delete records, and checks the raw materialized state against
   those winners. It then sorts whole transactions by Mako timestamp for native
   replay and sets Mako's process-wide clock floor so the next allocation is
   greater than the recovered maximum. The progress sequence is the recovered
   record count, not the last physical ID.
   This validation does not promise recovery of a RocksDB tail that had not
   been synced before a machine failure. The first slice exposes one default
   logical table and uses a tagged RocksDB key format separating user data,
   commit records, and future internal namespaces. Compatibility or migration
   from `mrx`'s raw-key layout remains a separate task.

The HLC switch replaces the draft v3 and v4 cache records with v5 and v6. Both
new formats carry the exact 16-byte `MakoTimestamp`; v5 includes a CRC and v6
is the explicitly unchecked variant. Recovery rejects older versions rather
than guessing, synthesizing an origin, or truncating a timestamp. Backward
compatibility is not part of this cutover, so old cache state must be rebuilt.

The protocol has no global publication ticket. Disjoint transactions may
validate, bind a lane-local sequence, install, publish, and return concurrently.
Silo's locks determine the serialization constraints, and the native Mako
timestamp records that logical order. A worker's physical log IDs increase
densely within its own lane. No ordering relationship exists between physical
IDs in different lanes. `MakoTimestamp` remains separate from the current
nonopaque row version.

- This slice is unbounded and local: it has no value eviction, distributed
  routing, 2PC, replication, or distributed-finality semantics.
- The timestamp filter is currently an in-memory index over raw RocksDB values.
  Correct reopen therefore depends on retaining every commit log, including
  deletes, so recovery can rebuild the index. Milestone 1 never prunes those
  logs.
- One cache exclusively owns the backend and its tagged keyspace. External
  writers, a second cache writer, or distributed writers would bypass the
  shared apply coordinator and invalidate last-writer-wins materialization.
- Phase 1 admits exactly one recovered cache namespace per process. This is a
  deployment precondition, not a mutex-enforced runtime feature. Native tables
  and the timestamp authority are process-wide, so independently opening a
  second pre-existing backend after work begins cannot retroactively preserve
  history. Supporting multiple caches requires a supervisor that identifies
  every namespace, scans every backend, and floors the shared timestamp clock
  before admitting any transaction to any of them.
- Before log pruning or distributed backend writers, store the winning
  timestamp with each materialized value and tombstone. A RocksDB merge
  operator or an equivalent conditional-update envelope must compare that
  timestamp atomically in persistent state. The current raw-value layout and
  process-local coordinator are not sufficient for either extension.

The in-memory applied watermark has one meaning in every RocksDB write mode:
its sequence is the aggregate number of commit records confirmed present in
RocksDB, and its timestamp is the greatest applied Mako timestamp. During live
application, confirmation is a successful `rocksdb_write` return. During open,
it is validated backend history. Neither field claims that all smaller Mako
timestamps have been applied, and neither means "synced." The current
production default is `Wal`: ordinary writes use `sync=false`, and the cache
adds no separate `FlushWAL`, `SyncWAL`, or memtable-flush call. The analogous
acknowledgement API also reports an aggregate count. `wait_applied()` snapshots
each initialized lane's acknowledged position and waits for every snapshot,
rather than waiting for a global prefix.

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

The historical Phase 1F correctness gate is complete for its named pre-HLC
candidate. Current revision-1 functional verification has passed. The
combined timestamp mutation campaign also killed all 12 mutants, with zero
survivors and zero harness errors. The performance sweep and canonical
all-in-one hook CI gate remain pending. The
[acceptance record](../mako-cache-milestone1-acceptance.md) keeps the old
evidence separate from current status. Required revision-1 coverage includes:

- Pre-preparation plus every reachable cache abort or commit-cleanup path must
  have a fresh-worker quarantine assertion. The raw ABI must independently
  cover all five native cleanup seams, including destroy.
- The strict isolated suite must mutation-test corrupted native-record put
  replay, early detached capacity discharge, hook-time allocation, conflict
  cancellation slots, missing or premature Ready publication, unpinned unknown
  outcomes, partial replay, reordered commits, exact-batch retry, wrong Mako
  timestamps, and a missing recovery clock floor. The per-worker-lane revision
  adds lane-local density, duplicate-timestamp, stale-materialization, and
  shared fail-stop cases.
- Synthetic and real cache histories must run through the transaction oracle
  first, then add physical lane order, timestamp order, backend batches and
  retries, aggregate progress, wait barriers, pinned lane suffixes, and one
  global clock.
- Deliberate decoded-batch divergence must turn the same full-history checker
  path red; transcript decoding must reject partial materialization first.

### 1G. Bounded values and eviction (deferred until after Milestone 1)

Phase 1G is explicitly not a Milestone 1 release blocker. Milestone 1 keeps
every live value in Silo, so the complete live dataset must fit in RAM. It also
does not reclaim the commit-record history accumulated in RocksDB. This is
separate from writeback backpressure: detached permits plus prepared/ready
in-memory records are bounded by `WritebackConfig::capacity`, and producers
block before native commit when that capacity is exhausted. Concurrent mode
applies that configured capacity to each initialized worker lane, so total
queue capacity grows with the number of active lanes.

The post-Milestone-1 eviction design may retain a complete key index in Silo
while bounding resident value bytes:

- An index miss must remain authoritative absence.
- Evicted markers carry the cache commit sequence to prevent ABA fills.
- Dirty or uncovered values cannot be evicted.
- A read-through fill participates in the transaction's validation and may
  not perform unbounded blocking IO while holding native locks.
- Tombstone reclamation waits for a proven conditional-remove/RCU design.

If transactional read-through makes OCC windows unacceptable, keep the first
production Silo cache unbounded and treat bounded values and log reclamation as
separate designs.

### Milestone 1 final acceptance gate

The checklist below separates completed historical validation waves from the
current revision-1 validation. Historical candidate
`6574cf47c` passed the original functional/contract gate and complete
comparative zoo-2 matrix on 2026-08-26. The later
native-record/bounded-batching rewrite passed its delta correctness gates and
old-versus-rewrite zoo-2 scaling run on 2026-08-29. The retained
[Milestone 1 acceptance record](../mako-cache-milestone1-acceptance.md) reports
both evidence sets and their concurrent-write scaling limitations. The
per-worker-lane implementation supersedes that rewrite's global publication
queue. Its retained full-suite and W1/W4 evidence also predates the HLC
cutover. Revision-1 functional verification has passed. Its performance sweep
and canonical all-in-one hook CI gate remain pending. The combined timestamp
mutation campaign has passed with all 12 mutants killed, zero survivors, and
zero harness errors.

- [x] **Historical foundation:** every Phase 1A-1D boundary gate is green,
      including the resolved
      Phase 1C/1D freeze choices.
- [x] **Per-worker lane revision:** transaction-atomic multi-key application,
      with a
      bounded contiguous lane prefix per black-box RocksDB `WriteBatch` and one
      shared timestamp-filtering apply coordinator.
- [x] **Per-worker lane implementation:** reopen sets Mako's HLC floor so the
      next timestamp is greater than every recovered record before admitting
      work, including near-exhaustion and corrupt-timestamp cases.
- [x] **Per-worker lane revision:** an honest in-memory `AppliedWatermark` and
      `wait_applied()` barrier under concurrent writers, write failures, and
      sustained overload. Progress is an aggregate count plus the greatest
      applied timestamp, not a global prefix, and neither claims disk sync.
- [x] **Per-worker lane revision:** concurrent disjoint commits publish and
      acknowledge independently through per-worker SPSC lanes. The runtime
      polls lanes in round-robin order, and stale cross-lane application cannot
      overwrite a newer timestamp.
- [x] **Per-worker lane revision:** physical log IDs encode the one-based
      worker lane in the upper 16 bits and a dense lane-local sequence in the
      low 48 bits. Reopen also accepts upper-zero untagged records, validates
      each lane, rejects duplicate timestamps, and replays whole transactions
      in Mako timestamp order.
- [x] **Per-worker lane revision:** clean cache/process shutdown drains all
      accepted transactions to RocksDB. A forced cache/process stop may discard
      the acknowledged but unapplied in-memory tail. A machine or power failure
      may additionally lose an applied RocksDB WAL tail that was accepted with
      `sync=false`; `AppliedWatermark` never claims otherwise.
- [x] **Historical foundation:** on zoo-2, measure throughput, abort rate,
      retry-inclusive p50/p99,
      acknowledgement-to-application drain, recovery time, and log/backend
      amplification against both the current `mrx` cache and raw RocksDB.
      Record the candidate commit, build fingerprint, exact command, hardware,
      CPU affinity, methodology, machine-readable artifact, and acceptance
      result in the linked Milestone 1 acceptance record.
- [x] **Previous global-queue rewrite:** on zoo-2, run the frozen-source
      1/2/4/8/16/32-worker read/write comparison against the pre-rewrite
      implementation, retain all 84 raw samples, and independently verify their
      accounting, recovery, and report-integrity invariants.
- [x] **Historical pre-HLC per-worker validation:** the full production and
      hook-enabled native/cache suites passed, and the frozen zoo-2 W1/W4
      comparison confirmed near-constant scaling efficiency. The linked
      acceptance record retains exact commands, identities, logs, and samples.
- [x] **Revision-1 functional validation:** the native suite passed 123 of 123
      tests. `mako-cache` passed 159 unit tests, 23 integration tests, and three
      doctests. The native-backed `mako-local` library, integration, and
      documentation suites passed, as did the fake-ABI suites. `mako-history`
      passed 25 application tests and 12 base transaction-oracle tests. The
      release Cargo check and strict fingerprint, symbol, C11, and C++ gates
      also passed.
- [x] **Revision-1 mutation acceptance:** the combined timestamp campaign
      killed all 12 mutants, with zero survivors and zero harness errors. The
      first full run killed 11 and exposed a weak oracle in
      `recovery_advances_mako_timestamp_past_the_recovered_maximum`. After that
      test was strengthened with a future but representable HLC, the focused
      rerun killed `missing-recovery-clock-floor`. The source-integrity check
      matched before and after the campaign.
- [ ] **Revision-1 performance and hook acceptance:** run the performance sweep
      and canonical all-in-one hook CI gate. Both remain pending.

## Milestone 2: distributed Mako with C++ Silo participants

Port the distributed control plane while retaining the local C++ engine:

### 2A. Cut the distributed path over to HLC

Treat this as one protocol and format change, with no mixed 32-bit and HLC
mode:

1. Replace the timestamp-allocation RPC with a validation reply carrying the
   participant's greatest full HLC bound. Validate every read or write
   participant before choosing a commit timestamp.
2. After all validations succeed, let the coordinator allocate exactly one
   timestamp greater than every participant and local bound. Propagate that
   exact timestamp to every participant, and merge it into each participant's
   local HLC before exposing the commit.
3. Move distributed value metadata, replicated transaction logs, replay
   records, failure-history entries, and visibility watermarks to the 16-byte
   timestamp. Store the failure epoch, transaction ID, Raft term, and Raft log
   index in separate fields.
4. Remove `timestamp * 10 + term` from values and logs. Make replication apply
   callbacks return status separately instead of returning
   `timestamp * 10 + status`.
5. Add shuffled-reply, clock-skew, restart-floor, duplicate-message, and
   multi-shard differential tests before enabling the new distributed path.

Phase 1 does none of these distributed changes. Its fixed origin of 1 and
single-process clock are not a distributed origin-allocation scheme.

### 2B. Port coordination to Rust

1. Define a participant ABI for begin/read, batch-lock, validate, install,
   abort, and commit-record production. A participant transaction stays on
   the same affinity-pinned worker for every phase.
2. Port key routing and coordinator state to Rust. Keep Mako's point-key hash
   routing compatible first. Do not promise globally ordered range scans over
   hash shards; either merge explicit per-shard scans or adopt range sharding.
3. Implement the HLC commit order from 2A: lock remote writes, lock local
   writes, validate local predicates and every participant, collect full
   bounds, allocate one strict successor, propagate it exactly, install,
   log or replicate, and release. Encode the state machine so invalid phase
   transitions are typed errors.
4. Give every RPC an idempotence key, deadline, cancellation rule, and
   duplicate-response behavior. Unknown commit outcome is distinct from an
   OCC conflict.
5. Differential-test a Rust coordinator against the existing C++ coordinator
   using deterministic schedules before switching any default.

Milestone 2 must restore every timestamp authority during replay or promotion
and cover remote read-only participants during validation. These are
pre-existing distributed gaps. Phase 1's local clock does not claim to repair
them.

The gate requires single-node and multi-node agreement, participant crash at
every 2PC phase, coordinator crash/restart, duplicate/reordered messages,
network partition, retry exhaustion, and proof that no prepared participant
is silently abandoned.

## Milestone 3: distributed durability, replication, and recovery

- Version and checksum the Rust transaction log format. Make the HLC cutover a
  clean break that rejects the old C++ timestamp layout; operators discard and
  rebuild old state or convert it offline before starting the new version.
- Port replication adapters without coupling consensus log indexes to Silo
  versions or cache commit sequences.
- Specify exactly when a client receives success: local install, durable local
  log, or replicated quorum. Expose weaker modes only as explicit options.
- Port watermark handling and checkpointing, then test follower catch-up,
  snapshot install, leader changes, truncated or corrupt tails, rejection of
  legacy records, and coordinated clean-cutover restart.
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

This section separates current implementation status from historical
acceptance. Transactional scan chunks, scan read-your-writes, the explicit
applied watermark, per-worker lanes, and HLC timestamp arbitration are
implemented. Revision-1 functional verification has passed with 123 of 123
native tests, 159 `mako-cache` unit tests, 23 integration tests, three doctests,
25 `mako-history` application tests, and 12 base transaction-oracle tests. The
native-backed and fake-ABI `mako-local` suites, release Cargo check, and strict
fingerprint, symbol, C11, and C++ gates also passed. The performance sweep and
canonical all-in-one hook CI gate remain pending. The combined timestamp
mutation campaign killed all 12 mutants, with zero survivors and zero harness
errors. Its first full run exposed one weak recovery-floor oracle after killing
11. The strengthened future-HLC oracle killed the remaining
`missing-recovery-clock-floor` mutant on a focused rerun. Source integrity
matched before and after the campaign. Phase 1G eviction remains deferred.

Item 4's sanitizer, Miri, fixed-worker concurrency, and overhead gate was
accepted on historical candidate `5a3dd3eaf`; the linked
[validation record](../mako-local-boundary-gates.md#validation-record)
retains the evidence. Item 5's cleanup, mutation, and application-history gate
was accepted on historical implementation commit `5546062af`; the linked
[Item 5 validation record](../mako-local-boundary-gates.md#item-5-phase-1f-validation-record)
retains that evidence. The comparative zoo-2 gate belongs to historical
candidate `6574cf47c`; its
[acceptance record](../mako-cache-milestone1-acceptance.md) preserves the raw
artifact and observed concurrent-write scaling cost. None of those records is
revision-1 acceptance. Inside-RocksDB instrumentation remains outside this
milestone.

1. The current operation and status contract reports ABI revision 1. It adds
   the 16-byte HLC timestamp and v5/v6 records as a clean break while retaining
   the published numeric status reservations. The Phase 1C/1D design choices
   still use implicit TLS, conditional all-output initialization, and
   process-lifetime native resources. The revision-0 validation rows below are
   historical. Current strict fingerprint, symbol, C11, and C++ conformance
   gates passed alongside the revision-1 functional suites.
2. The C header now generates the raw Rust declarations. Strict C11/C++
   conformance probes, a Rust all-export link probe, an exact native symbol
   allowlist, and the source/configuration-derived fingerprint plus digest link
   anchor complete this item in both production and hook-enabled profiles. The
   same checks cover the sized database options seam and exact 460-worker
   constant/probe.
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
   executable Phase 1A-1D gate; the subsequently resolved revision-0
   design/freeze choices are documented above. It does not by itself complete
   Milestone 1.
5. Historical Phase 1F is complete on implementation commit `5546062af`. Four
   fresh-worker cache cleanup/quarantine scenarios complement all five raw ABI
   seams; the 12-mutant isolated suite has no survivor or harness error; and the
   application-aware oracle accepts real sequential and response-reordered
   concurrent cache histories while rejecting injected divergence. The
   per-worker revision removes cross-worker prefix waiting and instead checks
   independent acknowledgement, lane-local density, and timestamp-filtered
   stale application. The
   dedicated hook-enabled profile is mandatory for native seam tests; the
   production-default native commit hot path contains no observer branches.
   See the
   [Item 5 validation record](../mako-local-boundary-gates.md#item-5-phase-1f-validation-record).
6. Treat disk-sync observation, unflushed-tail recovery, and log reclamation as
   a separate durability milestone. They do not block beginning the
   distributed Rust port once the local transaction and timestamp-arbitrated
   contract passes its gate.
7. The final cache-level comparative benchmark completed on zoo-2 on
   2026-08-26. Its machine-readable evidence and independently checked medians
   are retained in the linked acceptance record. That historical revision was
   accepted within its single-machine, asynchronous scope. The later pre-HLC
   per-worker-lane evidence is also historical. The revision-1 performance
   sweep and canonical all-in-one hook CI gate remain pending. Functional and
   mutation verification have passed, but revision 1 has no final acceptance
   result until those two remaining gates complete.
