# Sharding and migration on the Rust transaction engine: plan and handover

Branch `sharding-on-rust-engine`, worktree `/var/tmp/mako-shard-rust`. Sections 1–9 preserve the original handoff at `e8872f308`. Paths beginning with `notes/`, `tools/` or `prompts/` in those sections refer to `~/mako-archive/handoff-sharding-on-rust-engine-20261123/`.

## Resumed implementation

### Completed integration, 2026-11-23

The remaining live integration work is complete, with the scope and proof
assumptions below. The checklist refers to working integration, not a Git merge.

- [x] Make migration enumeration independent of unrelated directory inserts,
  without weakening ordinary transaction phantom validation. Check concurrent
  insertion, tombstone traversal, and complete migration under load.
- [x] Move storage-operation control flow into the verified core and connect its
  contracts to the migration store specifications. Enumerate the engine and FFI
  assumptions precisely. Cover epoch admission and completion ordering without
  counting existing sharding proofs as new integration proofs.
- [x] Provision destination warehouse indexes on demand and remove the
  18-global-warehouse allocation restriction. Check publication, reuse, failure,
  and shutdown with attached workers.
- [x] Exercise relocation of all eleven governed warehouse tables and repeated
  moves between independent owner processes while transactions run.
- [x] Run Miri and the relevant native sanitizer lanes. Fix findings in this
  integration and record any external blocker with its failing command.
- [x] Run the transaction-manager and Masstree component targets separately,
  the migration checks, and the proof targets against the final implementation.

The scope is live, nonreplicated migration. Replication and recovery of primary
data on restart remain separate extensions. No publication or merge is implied
by completing this checklist.

Implementation and evidence:

- Migration lookahead now retains structural admission through certification.
  Ordinary transaction scans retain optimistic phantom validation. The focused
  nine focused lookahead checks passed. The native migration fixture includes
  ordinary directory insertion while traversing 5,000 tombstones, with a
  deliberately constrained 1,024-item transaction budget.
- `shard-host-core` executes put/remove transaction control and the tombstone
  hop loop. Its gate passed 40 obligations and 21 semantic negative controls.
  E1-E7 explicitly name the native engine assumptions. The production sharding
  proof imports these same sources and checks `rust_storage_bridge.rs` against
  the real storage specifications. The full native gate passed: 1,495 production
  obligations (including the imported host core), 707 independent model
  obligations, 79 production and 69 model negative controls, and 69 runtime
  tests. The production positive proof was repeated after the final lint edit
  and again passed all 1,495 obligations. Of these, 40 come from the imported
  host core and five are new storage bridge lemmas; the remaining 1,450 are
  existing sharding obligations. Its separate file-backed recovery example
  also passed; that does not establish durable primary storage for this live
  integration. Native engine contracts E1-E7, cell/frame exclusion, pointer and
  registry lifetimes, actual epoch announcements, and TCP completion ordering
  remain trusted. Their runtime checks are not an end-to-end formal proof.
- The full pinned Miri script completed successfully. Its final Masstree
  library lane passed 224 tests with seven existing exclusions. Native ASan,
  UBSan and TSan integration checks, all 38 native ABI tests, and all six migration
  fixture outcomes passed. ASan migration runs enabled leak detection without
  added suppressions. The final separate component targets passed 406
  transaction-manager tests and 32 Masstree tests. The C++ sharding smoke passed
  both process arrangements, its metadata restart cases, mixed scans and epoch RCU.
- Lazy creation and selectors outside the legacy per-owner windows are
  implemented. The warehouse fixture provisions 231 additional indexes per
  owner with an attached worker, checks reuse and out-of-range refusal, and
  destroys the database after the nodes and workers stop. The native ABI and
  FFI checks cover creating a table within an existing transaction's RCU scope.
- `examples/rust_sharding_tpcc.py` runs all eleven tables through repeated
  relocations between independent processes. The first round runs at the load
  barrier and later rounds during TPC-C. All 33 moves committed and both owner
  processes exited 0 after the 240-second workload. Owner 0 committed 58,678
  transactions and owner 1 committed 2,946,695. Both accounting identities and
  NewOrder/Payment progress passed. These are sequential table moves, not an
  atomic multi-table migration. Logs and parsed results are in
  `build_docker/rust-warehouse-relocations-wt1yglhx/`.
- Large accepted migration RPCs now retain effect ownership until their reply
  or connection failure. The generic one-second RPC deadline previously
  abandoned successful multi-second customer copies. A fixture delays the
  first destination write by 1.5 seconds to cover this failure. Conflicts during
  advisory precopy now advance to frozen, drained reconciliation.
- Ordinary scans still validate every traversed record, including tombstones.
  The benchmark's former 1,024-item limit exhausted during repeated warehouse
  moves and Delivery. `MAKO_STO_TPCC_MAX_TRANSACTION_ITEMS` now defaults to
  65,536 and accepts positive decimal values through `u32::MAX`. Allocation
  grows on demand. Zero, overflow and a suffix-bearing value were rejected
  with startup exit 2. This is a finite capacity adjustment, not tombstone
  reclamation or a relaxation of transaction validation.

The broad TSan script, `scripts/ci/run_rust_sto_sanitizer.sh thread`, stopped
in the separate `mrx` workspace tests with a race report in
`rocksdb::ThreadPoolImpl::Impl::SetBackgroundThreadsInternal` from the system
`librocksdb.so.8.9`. No suppression was added for it. This does not establish
whether that report is a RocksDB defect or incomplete instrumentation. The
transaction-manager, Masstree and migration targets passed when run directly.
TSan needs a container that permits `setarch x86_64 -R` on this host; no global
ASLR setting was changed. Existing UBSan and TSan suppression policies apply.

Logs for this run are under `/tmp/mako-integration-`: `miri.log`,
`bridge-proof.log`, `native-positive-final.log`, `lint-proof-final-02.log`,
`native-06.log`, `live-final.log`, `asan-native-final.log`,
`asan-fixture-final-02.log`, `ubsan-native-final-02.log`,
`ubsan-fixture-final.log`, `tsan-04.log`, `tsan-native-final.log`, and
`cpp-components-final.log`, and `cpp-compat-final.log`. In `native-06.log` and the UBSan native log, the
native checks passed before the fixture detected its changed default budget;
the final fixture logs cover the explicit 1,024-item setting.
The C++ compatibility build initially found an ASan Redis archive in the shared
Cargo output. Rebuilding that archive normally and relinking the already-built
objects resolved it; `cpp-compat-final.log` contains the passing smoke run.

| Milestone | What to check |
| --- | --- |
| Register the storage engine | Bootstrap chooses the Rust callbacks, keeps physical tables alive through shutdown, and fences each migration peer message by engine. C++ retains the original wire encoding. |
| Admit ordinary transactions | Point and range operations use the captured route. Coordinators and helpers keep their epoch until the last commit or abort effect. A second read and a repeated request on one helper must return the correct value. |
| Move a raw range under load | Hold a real writer across freeze, reject a late write, and compare both physical images after commit and abort. Use the former owner's original index to read, write and scan through the ordinary TCP participant protocol. |
| Move warehouse tables | Address a row by warehouse **and** physical key. Move two warehouses containing the same key, reconcile stale destination rows, and verify the neighboring warehouse. Exercise benchmark bootstrap and a live move during the workload. |

The Rust benchmark opens initially assigned warehouse tables before loading.
It creates other physical indexes and routing proxies on demand, including on
attached workers. Physical indexes and their proxies share a selector. Native
warehouse selectors occupy the remaining 15-bit protocol space above the
legacy owner windows, so the 18-global-warehouse restriction is removed. The
finite protocol selector and database registry budgets still apply. Replicated
Rust migration stays disabled.

The resumed implementation adds an exact-handle unregister operation to the native directory because an unsuccessful cold start leaves its pre-registered raw handle reachable. The receiver's completion decision now runs in the verified Rust admission module. The page reader obtains its row limit from the same Rust codec that writes the response.

### Local verification

Run these commands separately inside the build container.

| Scope | Command |
| --- | --- |
| Rust transaction manager | `cmake --build build_docker --target rust_sto_transaction_tests` |
| Masstree | `cmake --build build_docker --target rust_masstree_tests` |
| Storage callback decisions, including warehouse cursor bounds | `./scripts/verify_shard_host_core.sh` |
| Native sharding and receiver completion proofs | `./scripts/verify_native_sharding.sh` |
| Rust migration, including real transactions over TCP | `cmake --build build_docker --target rust_sharding_storage_fixture && python3 examples/rust_sharding_storage.py` |
| Repeated all-table relocation during TPC-C | `cmake --build build_docker --target sto_tpcc_bench mako_admin && python3 examples/rust_sharding_tpcc.py` |
| C++ compatibility | `./ci/ci.sh nativeShardingSmoke` |

Verus checks the executed storage-operation control flow, status and cursor
decisions, their storage-specification bridge, and receiver decisions under the
listed engine contracts. The Rust FFI, C++ adapter, actual epoch announcements,
and TCP send/receive ordering are exercised by the integration fixture.

For a live TPC-C move, start the two-owner workload in the build container.

```sh
MAKO_CLUSTER_CONFIG=1 build_docker/sto_tpcc_bench --storage-engine rust \
    --shard-config src/mako/config/local-shards2-warehouses1.yml --local-shards 0,1 \
    --paxos-proc-name localhost --num-threads 1 --runtime 60 --slow-exit
```

After both workers print `TPCC_BENCH_MEASURE_START`, use a second shell in the same container. The bounds below select warehouse 1 in the `warehouse` table. Repeat the poll until it reports a committed outcome.

```sh
build_docker/mako_admin begin 127.0.0.1:51000 837291 1 warehouse 00000001 00000002 0 1
build_docker/mako_admin poll 127.0.0.1:51000 837291 1
```

On 2026-11-23 the component targets passed 406 transaction-manager tests and 32 Masstree tests. The migration driver reported six expected outcomes, including its intentionally failing C++-callback baseline. TPC-C passed with colocated owners and with one process per owner. Holding both workers at the load barrier until the move committed established that the former owner could complete NewOrder and Payment transactions using the new placement. The move also committed during an active TPC-C measurement. A constrained micro load exited with the documented resource-exhaustion status and released its preopened tables.

## 1. The goal and the owner's instructions

Goal: make dynamic sharding and migration (the Rust crate `mako-sharding` in `src/cluster`) work when the process
runs the Rust transaction engine (`crates/sto-core`, `crates/sto-masstree`, `crates/sto-tpcc-ffi`), which today it
does not.

The owner's instructions, verbatim, in order:

1. 2026-11-22: "another thing we should do concurrently is to merge the rust sharding/migration to use the rust
   version of transaction manager."
2. 2026-11-23: "let us do this moving forward for merging sharding and tx manager. Let us not use CI tests as gate,
   but replace all tests with local verus verification."
3. 2026-11-23: "for tests with no proof counterpart, can you try to add the proof counterpart."
4. 2026-11-23, after a discussion of splitting placement, migration and the transaction manager into layers:
   "actually, let us not do this, let us keep the placement, migration, and transaction manager in one layer for now."
5. 2026-11-23: "stop working but write down the plan so another agent can take over."

What follows from them:

- The gate for this work is a Verus verification that runs locally, in the build container, from the repository
  alone. No CI test step is added for it.
- Every property a test would have checked gets a proof counterpart where a real attempt can produce one. Where an
  attempt fails, record exactly what stops it and the smallest statement that would have to be trusted. A runtime
  test is removed only when its property is proven.
- Do not split the sharding crate or introduce layers. Do not raise the split again unless the owner does.

Standing rules:

- Nothing is pushed and no pull request is opened without the owner saying so.
- `src/cluster`, its 54 exported functions and its proof stay untouched. `tla/` stays untouched.
- The C++ engine stays the default and its sharding path must keep working. Mainline CI and the existing C++
  sharding smoke are not this work's gate, but nothing here may break them.
- No mixed-engine migration. Migration stays off on replicated nodes.
- Builds and tests run in the Docker/podman container, never on the host (project rule in `CLAUDE.md`).

## 2. Background

- The sharding crate is engine-neutral. It reaches storage, peers and the clock only through one table of C
  callbacks, `struct MakoShardHost` in `src/mako/native_sharding_host.h`: `thread_enter`, `thread_leave`, `open`,
  `scan_next`, `put`, `remove`, `peer_call`, `epoch_cut`, `epoch_drained`. The table is passed to
  `mako_sharding_start_node`.
- Until this work the only implementation of that table was C++ against the C++ engine
  (`src/mako/native_sharding_storage.cc`, `native_sharding_rpc.cc`, `cluster_bootstrap.cc`), and the only admission
  bridge was `src/mako/lib/sharding_admission.{h,cc}`, also against the C++ engine.
- The Rust engine is reached from C++ through `crates/sto-tpcc-ffi` and the adapter
  `src/mako/storage/rust_sto_tpcc_wrapper.{hh,cc}`. It has no admission calls and, before this work, no host
  callbacks. `docs/user-manual.md:285` says it "does not take part in native sharding".
- Approach taken: a second implementation of the callback table, and of admission, for the Rust engine.

## 3. What the contract study established

A study (three readers and a skeptic) answered the questions that size the job. Nothing was run; every statement is
a code trace. Full material: `notes/study_raw.json`, `notes/epoch-cut-and-drain/`, `notes/admission-coverage/`,
`notes/storage-callbacks-and-smoke/`, `notes/challenge/EVIDENCE.txt`.

- **Size:** glue plus small engine additions, several weeks. It does not need a change to the Rust engine's
  commit path.
- **Epoch cut and drain: glue.** The pin registry (`Transaction::epoch_enter`, `epoch_cut`, `epoch_drained`,
  `src/mako/sto/Transaction.cc:574-640`) lives in the C++ engine's source but does not depend on C++ transactions.
  Rust-engine threads already own a slot in it (`rust_sto_tpcc_wrapper.cc:1346`). A Rust-engine process must start
  the 100 ms tick with `Transaction::start_epoch_advancer()` only (not `ensure_epoch_runtime`).
- **Admission: can be added entirely in the C++ adapter.** Every Rust row access passes through
  `rust_sto_tpcc_wrapper.cc`, which holds the table id and the key. Insertion points are listed in
  `notes/admission-coverage/`. Protocol mode must be forced whenever sharding is on, or the fused and fixed-batch
  TPC-C lanes skip admission.
- **Storage callbacks:** scans, puts and removes map onto existing engine functions, with two gaps: a one-row
  lookahead cannot cross about a thousand deleted keys (confirmed later by a test), and a table cannot be created at
  run time on a thread that holds an engine handle. Proxies for remote tables cannot be expressed for a Rust index.
- **Defects found in today's tree** (they matter only when the Rust engine runs with sharding on):
  - Nothing refuses `--storage-engine rust` with `MAKO_CLUSTER_CONFIG=1`. By the trace, the first access to a
    warehouse owned by another shard stops the process.
  - `src/mako/lib/server.cc:293` decides "this piece is finished" from the C++ transaction object, which is null
    under Rust. A second read in one transaction would get the first read's reply.
- **Order hazard:** fixing `server.cc:293` without participant admission makes things worse, because cross-shard
  commits would start working with no admission check, which loses writes during a migration. The receiver fix,
  proxies and participant admission must land as one change.
- **Limits the C++ path does not have:** keys or values over 65,535 bytes cannot pass the Rust write path; every
  migrated row consumes about two values of a process-lifetime 32-bit clock; nothing in the peer handshake identifies
  the engine, so mixed-engine migration is not refused by anything.
- **What the existing sharding proof assumes** (`notes/obligations.md`): it proves 12 top-level properties and
  relies on 18 host requirements, H1 to H18. Only H3, H4 and H5 (scan, put, remove) are stated formally; the rest are
  prose. None is proven for the C++ host. H9 to H14 are the admission order: announce the epoch before reading a
  view, recheck the clock, the pin is the oldest one and survives retries, hold the pin until every effect is over,
  every access admitted against the captured view, storage callbacks never call ordinary admission.

## 4. Where the code is

- **Branch:** `sharding-on-rust-engine`, local only, never pushed: mainline `c4c84df1e` plus nine commits, one merge,
  and a last commit that adds this document as `docs/plans/sharding-on-rust-engine-handover.md`. The last code commit
  is `9c38f7289`. The code is 24 files, 6,625 lines added.
- **Checkout:** `/var/tmp/mako-shard-rust` (a git worktree of `~/mako`), clean. Its build tree `build_docker` was
  built at `9c38f7289`, the last code commit.
- **Copy:** `sharding-on-rust-engine.bundle` in this folder
  (`git fetch <bundle> 'refs/heads/sharding-on-rust-engine:refs/restored/sharding-on-rust-engine'`; needs `c4c84df1e`).

Commits, oldest first:

| Commit | What it adds |
|---|---|
| `ce1d8e882` | Test program `examples/rust_sharding_storage_fixture.cc`, driver `examples/rust_sharding_storage.py`, CMake target `rust_sharding_storage_fixture` (not in the default build, no CTest). Scenario `red-baseline`: C++ callbacks on a Rust database |
| `7d31d2ef2` | `crates/sto-masstree/src/distributed/lookahead.rs`: a bounded forward lookahead that can resume past deleted keys; 8 unit tests |
| `a414b30c2` | `crates/sto-tpcc-ffi/tests/shard_host.rs`: a test recording that the existing limit-1 scan fails past the 1,024 item cap |
| `cd69bb2ba` | `crates/sto-tpcc-ffi/src/shard_host.rs`: registry, registration exports, `scan_next` in hops of at most 256 entries |
| `1015ebb29` | `put` and `remove` on the participant protocol path |
| `0ce4aa37a` | `src/mako/storage/rust_sto_sharding_host.{hh,cc}`: thread entry, open of pre-registered handles, the callback table; two accessors in `rust_sto_tpcc_wrapper.hh`; scenario `migrate` |
| `d7d5009bf` | Scenarios `pinned`, `deleted-keys` (5,000 deleted keys), `aborted` |
| `ffca03c8a` | Counts the crate's `open` calls; removes a dead branch in `thread_enter` |
| `172380e65` | Merge of mainline `c4c84df1e` |
| `9c38f7289` | Spike: new verified crate `crates/shard-host-core` (`status.rs`, `order.rs`, `scan.rs`), gate `scripts/verify_shard_host_core.sh`, driver `scripts/shard_host_core_proof.py`, list `scripts/shard_host_core_trusted.txt`; `shard_host.rs` now calls the verified functions |

What was observed running, all in the container, by the engineers who wrote it (not rerun by the lead):

- `red-baseline` fails as predicted: the source's first scan answers IO and the migration stays pending.
- `migrate`, `pinned`, `deleted-keys` and `aborted` pass (`python3 examples/rust_sharding_storage.py`).
- The Rust tests pass through the CMake runner (`rust_masstree_native_integration`). 24 defects were injected one at
  a time (5 lookahead, 9 scan path, 10 put and remove) and each was caught by a test. The scripts are in
  `notes/work/`, the list is in `notes/impl_reports_raw.json`.
- The gate: `./scripts/verify_shard_host_core.sh` reports 33 verified, 0 errors, rejects 15 deliberately wrong
  variants, and takes about 18 seconds. The lead reran it at the last code commit through
  `tools/run_in_container.sh`; it passed (`notes/gate_run_via_handover_runner.log`).
- The existing sharding proof (`./ci/ci.sh nativeShardingProof`) was NOT rerun on this branch. Nothing under
  `src/cluster` or `tla` was touched. (A line "2044 verified, 0 errors" in the spike's logs is `cargo verus` checking
  the `vstd` dependency, not the sharding proof.)

What was not done:

- The code commits were never reviewed by anyone but their authors, apart from what the verification plan's
  reviewer read. Two findings are already known: after a panic, `contain_verb` closes an attempt the verb did not
  open (plan step 7 repairs it); and the gate as committed can be fooled: a `#[cfg(not(verus_keep_ghost))]` twin of
  a verified function is what plain cargo compiles and runs, while Verus proves the original and the audit passes
  (plan step 1 closes it).
- The planned defect scenarios in the test program, the `dbtest` refusal guard and a `ci/ci.sh` target were never
  written. The planned CI step is withdrawn (instruction 2).
- Miri and the sanitizer lanes have not run on any of the new Rust code. The container has no `rustup`.
- The mainline sanitizer workflow is red for unrelated reasons (a paused follow-up, pull request #103, closed), so
  new unsafe code has no green sanitizer signal. Tell the owner at pull-request time.

## 5. The plan from here

### 5a. First: read the verification plan and its review

`notes/VERUS_PLAN.md` is the verification plan for slice 1. A skeptic reviewed it and edited it in place: the summary
of the review is at the top of the plan (section 0), its findings are in section 10, and every change it made is
marked "[revised]" and listed in section 12. The review was stopped before it returned its final report, so check
that section 12 accounts for the marks before relying on it.

What the review found matters for every later step:

- Six ways a wrong implementation, or a gate that proves code that does not run, would have passed under the plan as
  first written. Examples: a `scan_next` that answers IO for every call met the first scan contract; one trusted
  clause was the theorem itself; a trusted clause could be strengthened without the gate noticing. Each now has a
  named clause or gate rule in the plan.
- Three trusted clauses were false or wider than the engine code, and two more ways the engine's scan can break the
  walk clause were found (plan section 2).
- No theorem ties the hop size to the engine's item cap of 1,024; that stays unverified.
- The review's own probes were three separate files with `u64` keys, never combined. They show each clause is
  needed, not that the set is consistent.

The reviewer's and the slice-2 designer's raw transcripts are in `notes/transcripts/`. The prompts are in
`prompts/sharding-rust-verus-gate-*.js`.

### 5b. Slice 1: prove the storage half (eleven steps from the plan)

Shape: the whole control flow of `scan_next`, `put` and `remove` moves into `crates/shard-host-core` as generic
functions over one trait, `Engine`. The trait's method contracts are the trusted list (clauses E0 to E9, in one
file, mirrored in `scripts/shard_host_core_trusted.txt`). The shell in `crates/sto-tpcc-ffi/src/shard_host.rs`
implements the trait with one engine call per method. A probe of this shape verified (26 obligations, 1.4 s).

1. Close gaps in the gate's audit; add two controls; correct the trusted list.
2. Tie the status and disposition numbers between the core, `protocol.rs` and `src/cluster/native_sharding.h`.
3. `engine.rs` and `verbs.rs`: `put` and `remove` in the core, with a model; the shell calls them.
4. `walk.rs`: the lookahead's walk decision; `Table::visit_versioned` replaces `lookahead_versioned`.
5. The hop loop and `scan_next` in the core.
6. The registry rules and thread admission in the core.
7. Panic containment repaired (the finding in section 4).
8. `bridge.rs`: tie the core's key order and its scan, put and remove theorems to the sharding crate's own specs
   (H3, H4, H5). Not probed. Fallback: restate the definitions and let the audit compare the text.
9. Add the `dbtest` refusal guard (`src/mako/benchmarks/dbtest.cc`, after line 551, inside the existing
   `#if defined(MAKO_RUST_STO_TPCC)` branch): exit 2 with a message when the engine is Rust and
   `MAKO_CLUSTER_CONFIG=1`. This is not a test and is needed regardless.
10. Retire each test once its property is proven, keeping the exact test lists in
    `cmake/RunRustMasstreeNativeIntegration.cmake` in step so mainline's integration step does not go red.
11. Final gate run from a clean cache; `./ci/ci.sh nativeShardingProof` must pass unchanged.

Each step's "done when" is in `notes/verus_gate_raw.json` (`plan.steps`). Under this plan 18 of the 24 injected
defects are rejected by Verus. The other six sit where no theorem reaches: the bounds passed at the engine call
(three), panic containment, the choice of commit path ("plain-put"), and the log callback.

What Verus refused in the spike, which decides the cut between core and shell: dereferencing raw pointers, function
pointer types, `thread_local!` with `Cell`, a `static RwLock`, `catch_unwind`, `std::process::abort`, closures
passed to std helpers, and a loop without a bound.

### 5c. Proof counterparts still missing (instruction 3)

The plan leaves 13 of 52 tests without a counterpart. For each, make a real attempt before recording a gap:

| Test or defect | Why the plan has no proof | Where to try |
|---|---|---|
| Two lookahead tests: a newly interned key, or a walked tombstone becoming live, fails certification | Facts about the engine's read validation (clause E5b) | Prove them in `sto-core`/`sto-masstree`, or state E5b as the single trusted clause and make it as small as possible |
| The item-cap record (limit-1 scan fails past 1,024 items) | Records a limit of existing code | Keep the numbers as a comment; a proof is not meaningful here |
| Scenario `pinned` (a migration waits while a pin is held) | Cut and drain are the C++ host's and the sharding runtime's | Slice 2: verify `drained(slots, cut)` and link it to the epoch model files in `tla/mako/src` |
| Scenario `red-baseline`; the driver's self-check | They test the starting state and the test driver | Go with the driver; the refusal guard covers the starting state |
| Defects: bounds at the engine call (three) | The bound conversion is inside the trusted `visit` | Move the conversion into the core so only the raw engine call is trusted |
| Defect: panic leaves an attempt open | `catch_unwind` cannot go under Verus | Model "an attempt is closed by whoever opened it" as a state machine in the core; the catch itself stays trusted |
| Defects: plain-put, log-refused | Which export the shell calls | The gate's text audit; or make the wrong call unrepresentable by type |
| 35 perturbations of the test program and the C++ file | C++ | Move thread entry and `open` decisions into Rust; what remains in C++ is listed as unverified |
| The end-to-end composition (a real migration through the real crate) | Each side is proven against the contract between them, not the joint | Step 8's bridge to the sharding crate's specs is the attempt |

### 5d. Slice 2: ordinary transactions under sharding (not designed yet)

A design was started and stopped before it wrote anything; its prompt is `prompts/sharding-rust-slice2-design-*.js`
and can be rerun as is. Scope:

1. Admission on every Rust row path, coordinator side and participant side.
2. The lifecycle bracket around a Rust attempt: the pin is taken before any view is read and released only after
   every effect, unlock and cleanup. It must not be taken for an idle helper thread, or no drain ever completes.
3. Proxies for tables owned by another shard, and run-time table open as far as needed.
4. The receiver fix at `src/mako/lib/server.cc:293`, landed together with participant admission (the order hazard).
5. The clock tick in a Rust-engine process.
6. Protocol mode forced whenever sharding is on.
7. The `dbtest` guard narrowed to whatever is still unsafe.
8. A rule that refuses a peer running the other engine.

Required shape under the owner's gate: the decisions are verified Rust (in `crates/shard-host-core` or a sibling
that follows the same pattern); the C++ gathers plain values, calls one export and switches on the answer. Host
requirements H9 to H14 become theorems about the running code. Ideas from the plan: one dispatch at the entry of the
receiver for a Rust-engine database, so the default path gains one branch; a verified `completion_decide` that takes
the engine's own answer instead of the C++ transaction object; an admission token that only the verified admission
function can make, so "every access is admitted first" is a type.

For every edit to code the C++ engine path runs, show that C++ behaviour is unchanged, and say how: a proof where it
can be stated, otherwise the exact trusted statement.

Risk carried from slice 1: each scan hop is invalidated when any thread first touches a never-seen key in the same
table, and the sharding crate treats a retry in the final copy as fatal for the migration. With ordinary workers
running, a migration behind many deleted keys may abort repeatedly. Slice 2 must measure this and may need a
different validation rule.

### 5e. Slice 3

TPC-C keeps working after a warehouse has moved: remote full scans for a Rust index (the page server in
`src/mako/lib/server.cc:98` accepts only the C++ index type). Weeks.

### 5f. Before any pull request

- Merge the branch with mainline again.
- Give the owner the list of what no proof covers (the plan's `unverified_list` in `notes/verus_gate_raw.json`): the
  engine behind the trusted clauses, the C++ host file, the FFI layer, panic containment, the end-to-end
  composition, liveness.
- Say that the sanitizer workflow is red on mainline.
- Check Miri and the sanitizer lanes before any merge. `sto-tpcc-ffi` now depends on `vstd` (through
  `shard-host-core`), and those lanes build it on a nightly toolchain (`nightly-2026-08-12`). Whether `vstd` and its
  proc macros build there, with sanitizer instrumentation, has not been tried: the container has no `rustup`.
  Mainline's Miri job is green today; this branch must not turn it red.

## 6. Decisions waiting for the owner

1. The scan's hop loop needs a bound for Verus to accept it. Proposed: `u32::MAX` hops, answering "exhausted" if
   reached. A real bound would make a cleanup behind more deleted keys than that repeat for ever.
2. For tests that end up with no proof after a real attempt: delete them, or keep them in the tree outside the gate.
3. If the test program is retired: keep `rust_sto_sharding_host.cc` compiled through a compile-only target, or
   remove it until slice 2.
4. Whether keys or values over 65,535 bytes may simply abort a migration on the Rust engine.
5. For slice 2: what counts as evidence that the C++ default path is unchanged, given that Verus cannot cover the
   shared C++ receiver.

## 7. How to build, test and verify

Everything runs in the container.

- **The runner to use:** `tools/run_in_container.sh <checkout> <timeout_seconds> '<command run from the checkout>'`.
  It needs only this folder, rootless podman and the image `mako-build:merge`. It was proven on 2026-11-23 by
  running the Verus gate through it. Example:
  `tools/run_in_container.sh /var/tmp/mako-shard-rust 2400 './scripts/verify_shard_host_core.sh'`.
- **What it sets up:** a private podman configuration (`tools/xdg`, store at
  `/var/tmp/mako-baseline-containers/storage`); the checkout mounted at its own host path; the main repository's
  `.git` mounted read-only, which a git worktree needs because the build runs git; the CI job's `MAKO_*` settings
  and reserved-port setting; cargo output under `<checkout>/target-docker` and `<checkout>/.cargo-docker`.
- **The image** `mako-build:merge` was built from the tree's own `Dockerfile.ubuntu24` and carries Rust 1.97.1. It
  lives in that store under `/var/tmp`. If it is gone, rebuild it from the Dockerfile (about 5 minutes, needs
  network) and tag it `mako-build:merge`.
- **Plain `podman` and `./docker_build.sh` were not used for this work.** On this host plain podman fails without
  the private configuration, and `docker_build.sh` does not add the mounts a worktree needs.
- **Older helper files** (`tools/incontainer.sh`, `run_row.sh`, `lib.sh`, `run.sh`, `env.sh`) are kept for
  reference. They depend on the previous session's scratch directory and are not needed.

Commands, from the checkout root inside the container:

| What | Command |
|---|---|
| The Verus gate (about 18 s) | `./scripts/verify_shard_host_core.sh` |
| The existing sharding proof (about 13 min) | `./ci/ci.sh nativeShardingProof` |
| Rust transaction manager and in-memory test datatypes | `cmake --build build_docker --target rust_sto_transaction_tests` |
| Masstree directory, native ABI and Rust wrapper | `cmake --build build_docker --target rust_masstree_tests` |
| Integration of the transaction manager, Masstree adapter and native host | `cmake --build build_docker --target rust_masstree_native_integration -j 16` |
| The test program's scenarios | `cmake --build build_docker --target rust_sharding_storage_fixture -j 16`, then `python3 examples/rust_sharding_storage.py` |
| The C++ sharding smoke (must stay green) | `./ci/ci.sh nativeShardingSmoke` |
| Rust engine binaries | `./ci/ci.sh rustStoBoundaryBinaries` |
| Format check | `cd crates && cargo fmt -- --check` |

A full configure and build takes about 30 minutes; any edit to `CMakeLists.txt` rebuilds `libmako.a`. Run one
container build at a time per checkout, and never edit a checkout while a run is using it.

## 8. Index of this folder

- `notes/SLICE1_DESIGN.md`: the reviewed design of the storage slice (contract of each callback in section A).
- `notes/SLICE1_DECISIONS.md`: the owner's instructions and the lead's decisions on the design's open points.
  Decision 3 there (a CI step) is withdrawn.
- `notes/VERUS_PLAN.md`: the verification plan, partly revised by an interrupted review.
- `notes/obligations.md`: what the sharding proof proves and assumes; the 18 host requirements.
- `notes/study_raw.json` and the four folders beside it: the contract study.
- `notes/impl_reports_raw.json`: the engineers' reports, with every injected defect and the output seen.
- `notes/verus_gate_raw.json`: the spike's report and the plan's structured summary.
- `notes/work/`: the mutation scripts and scratch probes, including the probe of the `Engine`-trait shape
  (`notes/work/planprobe/`) and the reviewer's probes (`notes/work/skeptic/`).
- `notes/out/`: scripts the engineers ran inside the container.
- `notes/transcripts/`: raw transcripts of the two agents that were interrupted.
- `notes/gate_run_via_handover_runner.log`: the gate run that proved the runner.
- `prompts/`: the five workflow scripts used, with their full prompts.
- `tools/run_in_container.sh` and `tools/xdg/`: the container runner. The other files in `tools/` are the older
  helpers.
- `boundary-map/`: a separate study of component boundaries in the whole tree (`BOUNDARY_MAP.md`); see section 9.
- `sharding-on-rust-engine.bundle`: the branch.

## 9. Other threads from the same session

- **Mainline** is `c4c84df1e`; `mako-dev` is the only branch on origin and no pull request is open. The last full CI
  run on it was green.
- **Dead C++ code:** 102 files were deleted and merged as pull request #105. Not yet deleted, because each needs
  more than a file removal: about 15 files that are compiled but never called, the unbuilt upstream files in
  `src/masstree`, Paxos (deprecated, untested, still built), and 3,345 tracked experiment logs under `src/mako`.
- **Component boundaries:** `boundary-map/BOUNDARY_MAP.md` maps what each component owns and depends on, with a list
  of decisions for the owner. The owner has since decided to keep placement, migration and the transaction manager
  in one layer for now.
- **Retiring the C++ transaction manager:** studied, not started. The Rust engine lacks sharding (this work),
  sharded-table open and the RocksDB-style facade, and any throughput number in distributed mode. The repository's
  own documents say C++ remains the default, so a recorded decision is needed first. Details in section 4 of the
  boundary map.
- **Sanitizer follow-up (pull request #103):** closed and paused. The branch and notes are in
  `~/mako-archive/mako-sanitizer-followup-paused-20261122.bundle` and the two
  `sanitizer-followup-paused-*-20261122.tar.zst` files; `~/mako-archive/README.txt` is the index.
