# Rust cache over Rust STO: the engine cutover

Status: in progress, 2026-09-12. Starting revision is the merge commit
`7de836393` (`worktree-masstree-rocks` + `codex/sto-rust`). Phases 0 and 1 are
implemented; Phases 2 to 4 are not. This is Milestone 4 of
[the transaction-cache roadmap](rust-mako-transaction-cache.md) made concrete
now that a native Rust STO exists; it also discharges the one deferred item
the [Rust STO design contract](../architecture/rust-sto.md) calls "upper
`mako-local`/cache cutover".

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
the oracle, exactly as roadmap Milestone 4 step 6 and Rust STO §19.2 require.

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
  commit lane; `reserve_upper_metadata` does nothing; `pre_install` performs
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
1. Worker-scoped quarantine. A cleanup or hook failure on one worker must
   quarantine that worker, not poison the runtime, or the cache's
   `quarantined_worker_count`, cleanup gates, and fixed-worker retry policy
   cannot be honored. Decision needed: add owner-scoped poison to
   `sto-core`, or redefine the cache's cleanup semantics for the Rust engine
   as process restart. This plan assumes the former.
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

1. Differential corpus. Add the Rust engine as a fourth executor to
   `crates/mako-local/tests/differential.rs` (direct C++, raw C ABI, safe
   Rust over C ABI, Rust engine) and byte-compare transcripts on
   `tests/data/mako_local_differential_v1.txt`, the seeded replay, and the
   injected-op mode. Extend the corpus with the MassTrans composition matrix
   and the scan bounds cases.
2. History oracle. Run `mako-history`'s application checker over concurrent
   cache histories on both engines (the harness exists in
   `application_history_tests.rs`); run `sto-masstree`'s own history oracle
   through the engine wrapper.
3. Recovery and GC. The milestone-1 acceptance suite, crash matrix, and GC
   soak protocol (`crates/mako-cache-bench/gc-soak.md`) on the Rust engine,
   including reopen of a database written by the C ABI engine and vice versa
   (same record format, so this must work).
4. Mutation campaign. Re-run the 14-mutant campaign against the Rust engine;
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

- Quarantine model (Phase 2, item 1). Must be decided before Phase 2 starts.
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
