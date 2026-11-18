# Rust cache over Rust STO: the engine cutover

Status: in progress, 2026-09-13. Starting revision is the merge commit
`7de836393` (`worktree-masstree-rocks` + `codex/sto-rust`). Phases 0 to 2 are
implemented; Phase 3's functional checks pass, performance acceptance remains
open, and Phase 4 has not started. This is Milestone 4 of
[the transaction-cache roadmap](rust-mako-transaction-cache.md) made concrete
now that a native Rust STO exists. Its implemented upper integration is recorded
in [Rust STO Appendix M](../architecture/rust-sto.md#appendix-m-mako-and-upper-layer-integration);
default-engine acceptance remains a separate gate.

### Integration acceptance, 2026-09-13

The Git merge is complete. The remaining work is the engine cutover, not
another branch merge. The current default remains the C ABI engine until
the checks below pass on the combined implementation.

The [functional qualification record](../testing/mako-cache-rust-sto-2026-09-13/README.md)
preserves the current candidate's source identities, mutation reports,
cross-engine recovery cases, and sanitizer inventories.

- Full Rust STO cache library suite passes without skips, including the two
  corrected GC fixtures. The out-of-order replay fixture now checks the
  largest timestamp wins, rather than the last log position. The empty
  database fixture distinguishes records reclaimed in one collection from
  the lifetime reclaimed count.
- Real core cleanup and hook failures now retire the affected worker and
  make subsequent begin/attach attempts report the poisoned runtime. Known
  committed outcomes remain committed even when their cleanup fails.
  `CacheStatus::engine_runtime_healthy` makes a poisoned Rust STO database
  unhealthy, including when status is read from a different healthy worker.
  The diagnostic query adds no runtime-health load to successful commits.
- The adapter's nine native-free ownership, holder, buffer-pool, and prefetch
  tests pass under pinned Miri without disabling leak checks. The Rust cache
  library suite passes all 206 tests; the C ABI reference passes 205.
  A test-only core observer now fires
  after the first actual write installation, so the partial-install crash
  point is exercised rather than skipped.
- Cross-engine recovery passes 12 sampled workloads and 16 exact-state
  cases in both directions, including checkpoint-only databases after full
  log GC. Four wrong-checksum tripwires are rejected. The engine's 19
  mutations, clock's 12 mutations, and cache's 15 mutations are killed.
  The final engine/cache runs verify that their original source and
  dependency trees did not change during the campaigns.
- ASan, TSan, and native-C++ UBSan pass the focused adapter/cache lane.
  [The sanitizer scope](../../scripts/ci/rust_sto_cache_sanitizers.md)
  records the two exact process-lifetime worker allocation exceptions,
  separate intentional-poison runs, ASan/TSan Loom runtime separation, and
  the required lost-transaction-log leak tripwire. These results do not
  claim instrumentation of the system RocksDB library. The latest focused
  lane covers 278 positives per profile. TSan instruments 270 production and
  native cases; all eight unbounded Loom models run separately because their
  generator runtime lacks sanitizer fiber bookkeeping.
- CI selects backend features explicitly. Cargo's additive `--all-features`
  cannot be used on the facade/cache packages because it enables both
  mutually exclusive engines. Both engines retain their own test targets.

Benchmark acceptance compares direct engine, engine plus log encoding,
foreground cache ACK, and sustained RocksDB application separately. Use
zoo-002 with boost disabled and identical CPU placement, workload, allocator,
compiler settings, and RocksDB libraries for both arms. A short ACK-only
sample is not evidence of sustainable persistence throughput.
The [paired performance investigation](../performance/mako-cache-rust-sto-2026-09-12.md)
records the retained optimizations and the remaining gap. The C ABI stays
the default while that acceptance work is open.

The [final production-source comparison](../performance/mako-cache-rust-sto-2026-09-12/final-production-2026-09-13/README.md)
completed 132 paired benchmark cells with boost disabled. All 60 foreground
cells passed count/value checks; the other 72 cells each passed a full drain
and fresh-process exact-state recovery. Source and native-input hashes stayed
unchanged. These compare the same Rust cache over either C++ STO through the
C ABI or Rust STO, not the distributed Mako server.

Rust STO's foreground one-put ACK throughput is 0.732 versus 1.159 million
commits/s at one worker, and 16.869 versus 20.142 million at 32 workers.
Those are deficits of 36.8% and 16.2%. The engine-only deficit is 41.9% to
50.9% across the five worker counts, so the remaining cost is not confined to
RocksDB writeback. In the separate CRC32C workload, four-key writes remain
37.1% to 42.4% slower and RMW workloads remain 31.6% to 38.4% slower. Its
allocation, timing, and CPU-placement protocol differs from the foreground
diagnostic; do not compare their absolute throughputs as one test.

Functional integration and the measurement campaign are complete. Performance
acceptance and a default switch are not. No acceptable regression budget has
been agreed. The next decision is whether to accept these measured costs for
an opt-in Rust engine or continue Rust core/adapter optimization before
cutover. The raw-STO diagnostic identifies combined adapter overhead, not a
proven benefit from any one ownership redesign. Keep the C++ implementation
and its differential tests until that decision and the compatibility release.

The first full sweep exposed a version-policy difference: non-opaque C++
MassTrans increments locked row versions, while the initial Rust adapter
allocated a shared ordered commit ID on every write. The new opt-in
[record-local version policy](../architecture/rust-sto.md#e11-opt-in-record-local-versions)
removes that requirement for the cache's non-opaque tables. Default core and
TPC-C bindings remain ordered; mixed transactions preserve both domains.
Core and native table tests, leak-checked Miri, the facade's mixed-domain
test, the required C++ differential, and the full cache suite pass on this
candidate. The final engine and cache mutation campaigns also pass with
unchanged source and dependency hashes. The tests catch loss of
the facade's local-version opt-in and incorrect reporting or decoding of
terminal version exhaustion. ASan, TSan, native-C++ UBSan, and fresh
cross-engine recovery also pass on this candidate. Performance acceptance
remains open. The report labels the earlier policy baselines, private
experiments, and final combined-source measurements separately.

### Concurrent read commits

Readers now take independent ordering cuts instead of holding one shared
reader mutex. Each reader takes the existing packed Acquire cut, then loads
the number of writer slots ever registered in this process. It scans only
those slots, using the same Acquire generation loads, exact-generation waits,
unknown-outcome rejection, and final engine validation as before.

The order of those two steps is required. A prefix writer registers before
its packed Release publication. Acquiring that publication before loading the
monotone registration count guarantees that the scan includes the writer.
Loading the count first can omit a writer even if that load uses Acquire.
Slots never reset or recycle, and writers need no additional shared atomic
operation. The bound counts historical registrations, not active threads;
after enough thread churn it can still reach the full 460-slot limit.

The [ordering proof and negative controls](../testing/mako-cache-rust-sto-2026-09-13/reader-bound-proof/PROOF.md)
separate weak-memory models from the actual helper's call-order regression.
The source-isolated mutation moves the count load before the cut and must
fail that regression. Independent readers must also reject an unresolved
prefix outcome. A post-cut conflicting writer remains subject to engine
validation. The single-producer lease path is unchanged.

## Decision

Make `mako-cache` drive `sto-core` directly, with no C ABI between them, by
introducing one engine-neutral Rust trait that both the existing
`mako-local` (C ABI to C++ STO) engine and a new `mako-engine-sto` engine
implement. The cache is moved onto that contract first, with zero behavior
change and the current engine as the only implementation. The Rust
STO engine is then built behind the same trait, proven against the C ABI
engine with the existing differential corpus, history oracle, crash matrix,
mutation campaign, and zoo-002 performance sweep, and only then becomes the
default. The C++ engine stays available for one compatibility release as
the oracle, exactly as roadmap Milestone 4 step 6 and Rust STO Appendix O.2 require.

The C++ Masstree index stays under the Rust STO through the `mt_*` C ABI.
Replacing the index is a separate step (roadmap Milestone 4 step 3) and is
out of scope here. So is the distributed path, opacity, durability policy,
and physical reclamation.

## What exists today (measured at `7de836393`)

Two stacks share one Cargo workspace and never call each other.

| Layer | Cache stack | Rust STO stack |
|---|---|---|
| Upper | `mako-cache` (22,235 LOC) | closed TPC-C bridge `rust_sto_tpcc_wrapper.cc` (2,141 LOC) |
| Boundary | `mako-local` (5,836 LOC) over `mako_local_abi.h` (37 fns) + `mako_local_rust_fast_abi.h` (28 fns + 4 test fns), implemented by `mako_local_abi.cc` (4,870 LOC) | `sto-tpcc-ffi` (38 `extern "C"` fns, 10,401 LOC) |
| Engine | C++ STO / MassTrans (`Transaction.cc`, `MassTrans.hh`) | `sto-core` (17,922 LOC) + `sto-masstree` (24,795 LOC) |
| Index | C++ Masstree via `mtx_*` (mrx cache only) and MassTrans directly | C++ Masstree via `mt_*` (`masstree` crate) |

Facts that shape the plan:

- `mako-cache` has no engine seam. It names `mako_local::{LocalDb, Table,
  Transaction, Scan, TrustedOnePutHolderPool, ...}` concretely and uses
  `mako_local::MAX_WORKERS` (460) in 21 places. Its only trait boundary is
  `mrx_core::Blobs` for the durable backend. The fake ABI in `mako-local` is a
  compile-time `cfg(test)` substitution for ownership tests, not a second
  engine.
- The cache reaches the engine through six commit-terminal shapes: fused SPSC
  one-put holder, preselected per-worker holder, preselected single-producer
  holder, preselected single-producer byte arena, native-ordered concurrent
  terminals, and general canonical-record commit; plus read-only fence and
  abort. All ordering work (HLC stamp, general gate, lane sequence, record
  serialization, holder sealing) happens inside those terminals in C++, under
  write locks, in `Transaction::try_commit`'s validation-gate protocol
  (`enter`/`leave`/`accept_ordered`/`after_leave`, restricted profile proved
  by `can_order_record_after_validation`).
- Lanes are a Rust concept: `sequence = ((worker_slot + 1) << 48) | local`.
  Native only allocates the dense sequence in CONCURRENT mode, which the cache
  no longer selects (`ForegroundMode::Concurrent` maps to `PerWorker`).
- The v5/v6 commit-record bytes are written by C++ for arena terminals and by
  Rust (`DeferredOnePutRecord::materialize`) for holder terminals. Rust
  already owns the decoder, checkpoint rows, lane metadata, and recovery.
- `sto-core` exposes `CommitHook { reserve_upper_metadata, pre_install,
  post_install }`, invoked per commit call, only for writing commits. It fires
  at exactly the points the C++ gate protocol needs: `reserve_upper_metadata`
  after `acquire_all` and before predicate upgrade and validation (the
  "enter the gate early" point of the general profile); `pre_install` after
  validation with the `OccCommitId` reserved and every write lock still held
  (the restricted-profile allocation point and the `after_leave`
  serialization point); `post_install` after every install and before any
  release. The two fallible callbacks are definite aborts on rejection or
  panic; `post_install` cannot fail. **The hook receives no data**: no write
  set, no commit id, no lock view.
- `sto-core` has no clock. `Runtime::reserve_commit_id` is one `fetch_add`
  producing an `OccCommitId`; there is no HLC, wall clock, or sequence. The
  C++ HLC (44/19 stamp, general gate bit, restricted allocation, dense
  sequence, floor advance, reseed) is about 200 lines of `Transaction.cc`.
- `sto-masstree` already gives the cache what it asked C++ for: bounded
  forward/reverse scans that observe the transaction's own staged mutations,
  owned or borrowed value delivery, `put_with_previous_presence` (the
  `created` bit), tombstones, and raw values without MassTrans's 20-byte
  metadata trailer.
- Failure models differ. C++ quarantines one worker after a cleanup failure
  and the cache counts `quarantined_worker_count`. `sto-core` poisons or
  marks indeterminate the whole `Runtime` on a hook or adapter panic; owner
  ids are recycled on `WorkerContext` drop, while native Masstree core ids
  (512) are never recycled.

## Target architecture

```text
mako-cache  ──uses──▶  mako-engine (traits + shared types)
                          ▲                    ▲
                          │                    │
              mako-local (C ABI engine)   mako-engine-sto (Rust engine)
                          │                    │
             mako_local_abi.cc / C++ STO   sto-core + sto-masstree + mako-clock
                          │                    │
                    C++ MassTrans/Masstree   C++ Masstree via mt_* (masstree crate)
```

Rules that carry over unchanged from the roadmap's stable decisions: one
active transaction per OS thread; conflicts return `Conflict`, never retry
invisibly; `MakoTimestamp`, `CacheSeq`, and OCC versions stay separate types;
one cache-order namespace per process; process-lifetime native resources are
honest in the API.

## Phase 0: the engine seam (refactor, no behavior change)

Status: implemented. Goal: one engine-neutral contract with `mako-local` as
the first implementation.

Decision taken while implementing: the engine is selected at compile time
through a facade crate rather than by a type parameter on `Cache`. Two facts
drove this. The hot commit path threads raw control pointers and outcome
words through the wrapper types, so a generic parameter would have had to
reach through `Cache`, `Writeback`, `WritebackSet`, `SingleProducerState`,
and the record path, some 11,000 lines, for no semantic gain. And the
cache-order namespace and Mako clock are process-global, so two engines can
never coexist in one process anyway. The layout is:

- `crates/mako-engine-api`: the contract. Every shared value type
  (`MakoTimestamp`, commit reports and dispositions, `Error`, `Features`,
  `CacheOrderMode`, limits, test-hook enums, the record framing constants,
  `CommitRecordTarget`, `FusedOnePutHolderAttempt<Outcome>`) plus the traits
  `LocalEngine`, `EngineDb`, `EngineTransaction`, `HolderPool`, `HolderView`,
  the three control-block traits, and the three outcome traits. Numeric
  contract values are literals; a native facade pins them against its ABI
  with compile-time asserts.
- `crates/mako-engine`: the facade `mako-cache` compiles against. Feature
  `c-abi` (default) re-exports `mako-local`'s types under fixed names.
  Exactly one backend feature may be enabled.
- `crates/mako-local`: re-exports the shared types from the contract crate
  at its historical paths and implements the traits by delegation
  (`MakoLocalEngine`).
- `crates/mako-cache`: refers to every engine name through `mako_engine`
  and carries no `mako_local` dependency.

The original plan text follows for the parts that still apply.

1. New crate `crates/mako-engine` holding the traits and the shared value
   types the cache already uses: `MakoTimestamp`, `CacheSeq`/lane packing,
   `Features`, `CommitDisposition`, `CommitReport`, `CommitRecordChecksum`,
   the commit-record framing constants, and the error enum. Move these out of
   `mako-local` (or re-export from it during the transition).
2. Traits, derived from what the cache consumes, not from the 69 C symbols:
   - `LocalEngine`: `open(options) -> Db`, `features()`, `engine_identity()`
     (id string + build fingerprint), `max_workers()`, `advance_timestamp_past`,
     `worker_health`, `quarantined_worker_count`, test hooks (commit-phase
     observer, cleanup-failure arming) behind a `test-hooks` feature.
   - `EngineDb`: `claim_cache_order_namespace(mode)`, `reseed(seed)`,
     `bind_table(name, id) -> Table`, `order_validation_prefix()`,
     `attach_current_thread()`, `begin_checked()`, `begin_trusted(table)`.
   - `EngineTxn`: `get`, `put -> created`, `insert`, `remove`, `scan`/`rscan`
     as a Rust iterator or visitor with owned or borrowed entries (the chunked
     resume protocol becomes an implementation detail of the C ABI engine),
     `abort`.
   - `CommitTerminals`: collapse the six shapes into three semantic
     operations the cache actually distinguishes: `commit_read_only()`,
     `commit_general(target: &mut RecordSink)` (allocate order under the
     general gate, serialize the canonical record, report `(seq, ts, bytes)`),
     and `commit_one_put(lane: PreselectedLane, sink: OnePutSink)` for the
     restricted profile (per-worker and single-producer variants, including the
     fused publish-on-commit outcome enum). The holder pool becomes an engine
     implementation detail behind `OnePutSink`; the C ABI engine keeps its
     native pool, the Rust engine uses plain owned buffers.
3. Make `mako-cache` generic. Monomorphize; no `dyn` on the commit path.
   Replace the 21 `MAX_WORKERS` uses with `E::max_workers()`.
4. Keep `mako-local`'s public surface, but implement the traits on it and
   move the cache to the trait names.

Gate: the entire existing verification passes unchanged on the C ABI engine:
202 `mako-cache` unit tests, 23 integration tests, 16 milestone-1 acceptance
tests, the crash matrix (16 hook-profile and 10 production points),
`mako-history` application tests, the 14-mutant campaign (all killed), the
strict fingerprint/symbol/C11/C++ gates, Miri on `mako-local`, and the
sanitizer lanes. Zoo-002 overhead within the existing advisory ceiling and
no measurable regression on the fused one-put path (compare against
`docs/benchmarks/mako-cache-*` on the same host). Reviewers should be able to
read this PR as a pure move.

Size: medium. Mostly mechanical, but `lib.rs` (4,165 LOC) and the record
paths in `writeback.rs` are touched throughout.

## Phase 1: `mako-clock`, the HLC and cache-order namespace in Rust

Status: implemented. `crates/mako-clock` is `forbid(unsafe_code)` and mirrors
`Transaction.cc` rule for rule: `Clock` holds the packed `(stamp << 1) | gate`
word and the dense sequence word; `try_allocate_timestamp`,
`try_allocate_restricted_timestamp` (returns `GeneralLocked`),
`try_allocate_sequence`, `try_allocate_order_pair` (timestamp first),
`enter_general`/`leave_general` plus an RAII `GeneralGate`,
`order_validation_prefix` (acquire `fetch_add(0)`), `reseed_sequence`,
`observe_stamp`, and `advance_past`. `global()` is the process-wide instance;
tests build private clocks. The physical source is `SystemTime` in
milliseconds with a `test-hooks` feature override (RDTSCP is deferred until a
benchmark asks for it). Gates in place: 21 unit tests ported from the C++
timestamp tests; `crates/mako-clock/mutations/run.py` (12 clock-rule mutants,
all killed, also wired as the `run_mako_clock_mutation_tests` CMake target);
and `crates/mako-local/tests/clock_differential.rs`, which drives the native
clock through the C ABI and the Rust clock through the same recovery floors
and commits and requires identical timestamps bit for bit, ending at the
final mintable stamp and exhaustion on both sides. The `mako-clock` unit
tests run inside `run_mako_local_rust_tests`.

Port `Transaction.cc`'s clock as a small safe crate: `allocate_hlc_stamp`
(44/19 packing, `max(physical, previous + 1)`), the general gate bit with
`enter`/`leave`, restricted allocation that returns `general_locked` instead
of waiting, the dense cache sequence (max 2^48 - 1) with "timestamp first,
then sequence" pairing, `order_validation_prefix` as an acquire RMW cut,
`advance_past` floor, and `reseed` that refuses while the gate is held and
never lowers the clock. Keep it a process-global namespace for parity with
the one-namespace-per-process rule; per-runtime clocks are a later change.

Gate: unit tests ported from the C++ timestamp tests; the existing timestamp
mutation campaign re-targeted at the Rust clock; a differential test that
drives both clocks through the same scripted physical-time sequence (the
`MAKO_LOCAL_TEST_HOOKS` physical-ms override exists on the C++ side) and
compares stamps bit for bit.

Size: small.

## Phase 2: `mako-engine-sto`

Status: implemented as `crates/mako-engine-sto`, selected by the `sto`
feature of `mako-engine` and the `engine-sto` feature of `mako-cache`
(`cargo test -p mako-cache --no-default-features --features
engine-sto-test-hooks,test-support`). What was built, and where it departs
from the sketch below:

- `LocalDb` owns one `sto_core::Runtime`, the process Masstree runtime, an
  append-only table registry, the FIFO ticket of the legacy record terminal,
  and the process-wide namespace claim. Workers are process-lifetime: the
  first use of a database on a thread attaches and leaks one `WorkerContext`
  and one Masstree `Worker`, so `MAX_WORKERS` bounds threads exactly as the
  C++ thread ids do. `Transaction` is `!Send`; one active transaction per
  thread per database, as in C++.
- The wrapper's operation log is the canonical write set: keys in first-touch
  order (reads and scanned rows included, so record order matches the C++
  `TransItem` walk), final effect per key, `existed_before` from the first
  observation. Item budget charges are the C++ ones (get 1, write 4 + one per
  8 key bytes, remove 1, scans per row).
- One `CommitHook` driver runs every record terminal: `reserve_upper_metadata`
  enters the general gate (HLC, or FIFO ticket then HLC for unclaimed and
  single-producer facades), `pre_install` mints the stamp, binds the sink,
  leaves the gate, serializes, and returns; a validation abort leaves the gate
  only when the driver drops after `sto-core` released every lock. The
  restricted profile is `can_order_record_after_validation` recomputed from
  the log (one update of an existing key, observed only through its own
  lock, no predicates): no gate, `(timestamp, sequence)` pair after
  validation, spinning on `GeneralLocked`. Every C++ terminal shape exists:
  general and one-put record targets, native-ordered record target, arena,
  holder (with FREE to BOUND to READY cell publication), preselected
  single-producer record and holder, per-worker holder, and the fused SPSC
  holder terminal with its `Untouched`/`Consumed` control codes.
- Records are encoded in Rust (`record.rs`, CRC-32C included) with the exact
  v5/v6 framing; a one-put v6 record is byte-identical to the C++ output.
- Outcome types carry the same fail-closed decode rules as `mako-local`'s,
  over `(disposition, cleanup)` reports instead of packed C status words.
  `sto-core`'s definite outcomes are preserved: a poisoned commit with a known
  outcome reports that outcome with `cleanup = Err(WorkerPoisoned)`.

Decisions taken here:

1. Quarantine model. `sto-core` poisons its whole runtime; this engine
   quarantines the calling worker (thread-local poison, global count) and
   maps every later `Poisoned` begin or attach on that runtime to
   `WorkerPoisoned`, so a poisoned runtime retires each worker as it next
   touches the database. Conservative, and the cache's cleanup gates pass on
   it.
2. `sto-masstree` change (for the Rust STO owners to review):
   `TableConfig::with_blocking_structural_admission(true)` makes a
   first-time key publication wait for structural admission
   (`RwLock::write`) instead of failing with `HiddenLockBusy`. The default is
   unchanged, so `structural_scan_seam_never_blocks_a_miss_publisher` still
   holds for other users. The engine opts in: two inserters of distinct keys
   used to abort each other, which the cache's contract forbids (the C++
   engine never conflicts there), and `concurrent_holder` hung on it half
   the time. The gate is held only across directory publication, never
   across a transaction lock, so waiting cannot deadlock.
3. `Features::TEST_COMMIT_MID_INSTALL_OBSERVER` (bit 8) says the test
   observer reports `FirstWriteInstalled`. Both engines now provide it in
   their hook-enabled builds. Rust STO's scoped test-only observer runs
   after each actual installation inside the core; the adapter emits the
   first-write phase once. The production core has no observer calls or
   additional loads. Engines without the feature still skip that point.
4. The physical clock stays `SystemTime`; RDTSCP is deferred to the perf
   sweep.
5. `sto-core`/`sto-masstree` change (for review): a transaction's own
   first-time key publication (any miss: get, put, insert, remove of a key
   that does not exist yet) used to invalidate its own earlier scan of the
   same table, because the scan's predicate is the table-wide directory
   generation and every publication advances it. The fourth-executor
   differential caught it on the first seeded script. `sto-masstree` now
   moves the transaction's own directory (and trusted scan) observation
   forward by one per own publication, through a new
   `sto_core::TransProxy::replace_read`, so validation still fails on every
   foreign publication but not on the transaction's own. Fixed-key batch
   publication (`fixed-u64`) does not account this way; the cache does not
   use it.

Gates in place: `crates/mako-engine-sto/tests/native.rs` covers every terminal
against the native Masstree; the complete `mako-cache` library and integration
suites built with `engine-sto` pass (206 library tests, including the two
corrected G4 fixtures and runtime-health propagation);
`mako-engine-sto/tests/differential.rs`,
the fourth executor of the shared corpus (`crates/mako-differential`),
compared against the C ABI executor with only the `features` lines masked.
The engine's test binaries need the CMake archives; run them with the
`MAKO_MTREE_NATIVE_*` environment the `rust_sto_integration` target sets.

Implement `LocalEngine` over `sto-core` + `sto-masstree` + `masstree` +
`mako-clock`.

Database and workers:
- One `sto_core::Runtime`, one `masstree::Runtime`, one `sto_masstree::Table`
  per bound table, `RegistryBudget` from cache options. Attach = masstree
  `Worker` + `WorkerContext` in thread-local storage; `max_workers()` = the
  cache's slot budget capped by the native core-id limit (512) and the sto
  owner limit.
- Namespace claim and reseed delegate to `mako-clock`.

Transactions:
- The wrapper records every operation it issues and its result (put created
  or overwrote, insert won or `AlreadyPresent`, remove existed) and derives
  the canonical write set from that log at commit: last write per key wins,
  insert-then-remove is elided, values are raw bytes. This keeps `sto-core`'s
  "hook receives no data" contract intact and mirrors what
  `visit_local_canonical_writes` computes in C++.
- Point ops map one to one onto `Table::{get, put_with_previous_presence,
  insert, remove_with_previous_presence}`; scans onto
  `visit_scan_bytes_with_scratch` with the cache's bounds and limit.
- Read-your-writes composition rules must match the merged C++ MassTrans
  semantics for local tables (read own insert/update/delete; delete-then-put
  as update; insert-then-delete cancels). Port the composition matrix from
  `tests/test_silo_nontxn_api.cc` into the differential corpus before
  claiming parity.

Commit protocol, one `CommitHook` implementation, chosen by the shape the
wrapper knows:
- General profile (any reads, scans, predicates, or more than one write):
  `reserve_upper_metadata` enters the general gate; `pre_install` allocates
  the stamp under the gate, takes the lane sequence (preselected by the cache
  or the dense counter), serializes the record into the preallocated sink,
  then leaves the gate; `post_install` is a no-op. Rejection (sink too small,
  timestamp exhausted) is a definite abort, matching `hook_rejected` and
  `timestamp_exhausted` today.
- Restricted profile (exactly one put, no reads): use `sto-core`'s direct
  commit lane; `reserve_upper_metadata` allocates no order metadata. On
  supported x86 CPUs it issues an optional write-prefetch hint for the HLC
  after acquiring write locks, before validation. The hint cannot allocate
  a timestamp, sequence, or gate and is a no-op on other targets and in Miri.
  `pre_install` performs
  the restricted allocation (spin on `general_locked`, never take the gate),
  seals the one-put value into the sink, and reports `(seq, ts, bytes)`.
  This is the `accept_ordered` path of C++ and the hot path of the cache.
- Read-only commits never invoke the hook; the read-only fence is
  `mako-clock::order_validation_prefix`.
- Map `AbortReason::Conflict` to `Conflict`, `HookRejected` to the rejection
  variants, `Capacity` to the exhaustion variants, `CommitFailure::Poisoned`
  and `Indeterminate` to the cache's poisoned and indeterminate dispositions.

Record bytes: encode v5/v6 in Rust for every terminal (the encoder already
exists for the holder path). The engine identity becomes the crate build
fingerprint; the cache's format record does not change.

Required `sto-core` changes, kept small and listed so the Rust STO owners can
review them separately:
1. Quarantine decision: retain `sto-core`'s runtime-wide fail-stop behavior.
   Uncertain cleanup requires retiring the database and restarting from its
   applied backend state. Do not pretend that replacing one worker restores
   a poisoned Rust runtime. Per-worker quarantine counters remain useful
   diagnostics; cache health also reports the runtime state.
2. Optional: pass `&CommitShape` (has_writes, single_write, has_predicates)
   and `OccCommitId` into hook callbacks. Not required if the wrapper tracks
   shape itself, but it removes a class of divergence bugs.
3. Optional: a hook-latency measurement point, which §17.2 of the STO
   contract says does not exist yet; the cache treats a post-install overrun
   as a health event.

Gate: the Phase 0 verification, now run per engine (test macro or cargo
feature selecting `E`), passes on the Rust engine; crash-matrix failpoints
are implemented as Rust failpoints in the engine instead of C hooks.

Size: large. Roughly the size of `sto-tpcc-ffi`'s non-TPC-C core (a few
thousand lines) plus tests.

## Phase 3: prove equivalence

Status: functional qualification complete; performance acceptance remains
open. Done with Phase 2: item 1's fourth executor
(`crates/mako-differential` holds the corpus, parser, generators, and a
replay generic over `EngineDb`; `mako-local/tests/differential.rs` now uses
it for the three-way gate, and `mako-engine-sto/tests/differential.rs` runs
the Rust engine against the C ABI driver on the same corpus, injected-op
tripwire included; it found the own-publication scan conflict on its first
run). Item 3's acceptance suite and crash matrix run on the Rust engine as
part of the cache suites (`run_mako_engine_sto_tests`), including the genuine
mid-install point. The composition-matrix and binary scan-bounds corpus
extensions landed in `9cd856c18`. Cross-engine reopen, engine mutations,
Miri, all three mutation campaigns, and the qualified sanitizer lanes now
pass. One paired 960-second W32 GC run also passed on the immutable pre-format
candidate binaries, including drain, log reclamation, fresh-process recovery,
all 8,192 keys, and a post-reopen write on each engine. The
[saved pair](../performance/mako-cache-rust-sto-2026-09-12/sustained-record-local-960-results.json)
is not a repeated full-worker-count sweep. Performance acceptance remains
open and the C ABI stays the default.

1. Differential corpus. Add the Rust engine as a fourth executor to
   `crates/mako-local/tests/differential.rs` (direct C++, raw C ABI, safe
   Rust over C ABI, Rust engine) and byte-compare transcripts on
   `tests/data/mako_local_differential_v1.txt`, the seeded replay, and the
   injected-op mode. Extend the corpus with the MassTrans composition matrix
   and the scan bounds cases.
2. History oracle. Both engine wrappers run the shared forced read-write,
   write-write, and phantom histories through `mako-history`'s
   strict-serializability checker. The native `sto-masstree` table separately
   runs its 128-seed history corpus; that corpus does not use the wrapper.
   Cache application histories on both engines additionally check visible,
   acknowledged, and backend-applied state in `application_history_tests.rs`.
3. Recovery and GC. The milestone-1 acceptance suite, crash matrix, and GC
   soak protocol (`crates/mako-cache-bench/gc-soak.md`) on the Rust engine,
   including reopen of a database written by the C ABI engine and vice versa
   (same record format, so this must work).
4. Mutation campaign. Run the 15-mutant cache campaign against the Rust engine;
   add mutants for the new code (drop the gate leave, allocate sequence
   before stamp, skip insert-then-remove elision, wrong `created` bit).
5. Memory safety. Miri on `mako-engine-sto` with `masstree` faked (the
   `sto-core` Miri lane already exists); ASan/TSan/UBSan through the existing
   Rust STO sanitizer lanes with the cache's boundary tests added to their
   inventories.
6. Performance. The zoo-002 sweep (1/4/8/16/32 workers, boost disabled) on
   both engines; the fused one-put path is the number that matters. Accept
   an explicit budget before cutover; expect the Rust engine to save the C ABI
   crossings and holder-pool bookkeeping but to pay for `Value` snapshots and
   hook work under locks. Measure a one-put spike at the end of Phase 2, not
   at the end of Phase 3.

## Phase 4: cutover and removal

Precondition found during Phase 2: two `mako-cache` library tests failed on
both engines and on the pre-merge tree
(`gc_stops_at_first_unexpired_record_in_untagged_stream_despite_later_expired_records`,
`recovered_empty_database_advances_hlc_and_lane_ids_past_checkpoint_state`);
the existing CI never ran the cache library suite. Their fixture and
expectation errors are fixed, and `run_mako_engine_sto_tests` no longer
skips them. Both remain required coverage.

1. Default `E = mako-engine-sto` in `mako-cache-bench` and the acceptance
   docs; keep `mako-local` selectable for one compatibility release.
2. Retire, in this order, once the release has shipped: the 28-function fast
   ABI and holder pool, the six-terminal matrix in the cache, the C++ record
   serializer, then `mako_local_abi.cc` and the 37-function public ABI with
   its symbol, fingerprint, C11, and conformance gates. The differential
   corpus keeps the direct-C++ executor as long as C++ MassTrans exists for
   the distributed path.
3. Update the roadmap: Milestone 4 steps 1 to 3 are satisfied by the Rust STO
   branch; steps 4 to 6 by this plan.

## Risks and open decisions

- Quarantine model is runtime-wide for Rust STO and worker-local for C++ STO.
  Recovery after a Rust runtime failure uses the applied backend state.
- Process-global clock versus per-runtime clock. Global keeps parity and the
  one-namespace rule; it also means both engines cannot run in one process,
  which the differential harness must respect by using separate processes.
- Write-set derivation in the wrapper. Any divergence between the wrapper's
  op log and what `sto-masstree` actually installs is a silent record bug;
  the differential corpus and the `created`/`existed` return values are the
  defense. Prefer the optional `sto-core` shape passing if it proves cheap.
- No reclamation on either side. Tombstones and consumed registry slots grow
  for the process lifetime in `sto-masstree`, as MassTrans rows do today.
  Bounded values and eviction (roadmap 1G) remain deferred.
- Each Rust engine worker/database pair retains its native worker and STO
  context for the process lifetime. Wrapper canonical touch-log buffers retain
  at most 64 entries and 64 KiB per pair. The core also retains reusable
  transaction storage sized by admitted work and configuration, without that
  byte ceiling. Creating arbitrarily many database instances in one process
  is not globally bounded. Use one database with
  long-lived worker threads and restart the process for a new runtime epoch.
  The inline atomic value optimization grows stable entries from 64 to 192
  bytes and applies to values through 160 bytes; larger values still use
  the shared representation.
- Performance under locks. The Rust engine serializes the record in
  `pre_install` with locks held, as C++ does in `after_leave`; keep sinks
  preallocated and never allocate in a hook.
- Two engines in one binary during rollout link C++ Masstree once through
  two ABIs (`mtx_*` for mrx, `mt_*` for the Rust STO) plus MassTrans; the
  merge already proved the symbol sets are disjoint.

## Order of work

Phase 0, then Phase 1, then a one-put-only spike of Phase 2 with its
performance measurement, then the rest of Phase 2, then Phase 3, then Phase
4. Phases 0 and 1 are independent and can proceed in parallel.
