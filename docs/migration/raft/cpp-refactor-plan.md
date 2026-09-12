# Refactoring `src/deptran/raft` C++ toward Rust

A work plan. Written 2026-09-12 against HEAD `c879dae8`, for a session that has
no memory of the conversation that produced it. Everything here was re-verified
against the tree on that date; where an older document disagrees, this one is
right and says why.

> **This plan has been executed through Tranche 4.** What was done, what it
> proved, and the four places this document turned out to be incomplete are
> recorded in [`cpp-refactor-progress.md`](cpp-refactor-progress.md). Tranches
> 5 and 6 remain. Read that file alongside this one.

## 0. Read this first

Three documents overlap. Their standing:

| document | status |
|---|---|
| `docs/migration/cpp-to-rust-pre-check-list.txt` | the aspect vocabulary (LIF/OWN/TYP/ERR/CON/LIB/TST). Stable, use it for naming. |
| `docs/migration/raft/cpp-to-rust-precheck-raft.txt` | the blocker inventory B1–B7. Substance is sound; **every `server.cc` line above 2802 is stale by +18** (commit `3774ba93` inserted 18 lines at :2803). Below 2802 and all of `server.h` are exact. |
| `docs/migration/raft/strategy-recommendation.txt` | superseded by this file for ordering. Its measurements are mostly right; its critical path is wrong. |
| `docs/migration/raft/cpp-refactor-progress.md` | what actually happened when this plan was executed. Authoritative for the current state of tranches 0-4. |

## 1. Goals, in priority order

1. **Keep correctness testable and performance behaviour intact.** Not proof of
   correctness — *persuasive* evidence: a test that would have caught the
   change if it broke something, and a harness number that did not move beyond
   the measured noise floor.
2. **Improve Rust expressiveness** — how much real Raft code can be expressed
   in the DSL at all. This is the goal that paces the work.
3. **Be memory-safe as much as possible**, but never at the cost of (2). Where
   safety and expressiveness conflict, defer the safety work; do not abandon it.

The decisive question for any proposed task is therefore **"what does this let
us express that we could not express before?"** — not "is this unsafe?"

## 2. Ground truth: what actually gates expressiveness

Established by probing the pinned emitter (`77c3ad5a`, `git_dirty=false`), not
by reading documentation. There are four gates. **The two that matter most are
not C++ refactoring at all**, which is the single most important finding here.

### G1 — the crate has no dependencies, so no foreign type can be named

`scripts/raft_dsl.sh:384-388` runs **bare `rustc`** on the extracted crate — no
Cargo context, no `--extern`, no type map — and `src/deptran/raft/Cargo.toml`
has no `[dependencies]`. The *emitter* handles foreign types perfectly:
`rusty::Mutex<rusty::Option<T>>`, `rusty::Box<T>` and `::janus::Command` all
emit correct C++ that compiles and runs. But the extracted Rust then dies at
E0433/E0573 before it reaches the emitter's output.

This is what stops every non-scalar type. Of `RaftServer`'s 59 data members,
~15 need a foreign name.

**It is a Mako-local script line, not a toolchain limit, and `rrr` already
solved it**: `src/rrr/Cargo.toml:14` (`rusty = { path = "rusty-rustc" }`),
`src/rrr/rusty-rustc/src/lib.rs:58-82` (opaque `#[repr(C)]` models of foreign
types), `src/rrr/rust-type-map.toml` (already maps `IntEvent` and `PollThread`).

### G2 — no ODR post-pass, so header carriers cannot hold a non-`const fn`

Real: a header-resident type with an inherent `impl` produces
`ld: multiple definition of ...` across two TUs. But **the fix already exists in
this repository and was run green on the raft case**:
`scripts/regen_storage_dsl.sh:72-100` is a 29-line pass that prefixes `inline`
inside GEN regions. It runs in production today on `src/mako/storage/mbta_wrapper.hh`
(23 inline-prefixed methods) and `src/cluster/config_manager.h` (~33).

Three independent cures, all verified working: put the block in a `.cc` carrier
(6 of 17 carriers already are); apply the post-pass; or use crate mode, where
definitions attach to a C++20 module.

`schema_version = 1` in `src/deptran/raft/rust-modules.toml` is a manifest
field, not a capability gate. The schema-2 pipeline already runs on raft
unchanged: crate mode on raft's manifest gives **18 files, 0 errors, `.cppm`
out**.

### G3 — orphan impl: the unit of conversion is a whole TYPE, never a method

`impl RaftServer { ... }` where `RaftServer` is hand-written C++
(`server.h:1170`) emits `#if 0  // patcher: orphan-impl block stubbed` and the
methods are compiled out.

**This is the structural wall.** To convert one `RaftServer` method you must
move `RaftServer` itself — `server.h` 1 997 lines plus `server.cc` 3 861 lines —
into the DSL wholesale. No rearrangement of that C++, no ownership decision
about `rep_sched_`, no removal of a default member initializer makes a single
`RaftServer` method convertible while this holds. It is a transpiler feature;
neither the ODR post-pass nor a type map touches it.

The only escape is `#[cpp_inherit]` (`src/mako/storage/mbta_wrapper.hh:538`),
which requires the *derived* type to be DSL-owned and only lets it inherit a
C++ base.

### G4 — implementation inheritance has no Rust spelling (precheck B7)

`class RaftServer : public TxLogServer` (`server.h:1170`). The base contributes
six public data members and **zero** pure virtuals (`scheduler.h:21-26`), and
`RaftServer` reads them as its own: 164 production uses of `site_id_`, 20 of
`partition_id_`, 10 of `loc_id_`, 6 of `app_next_`, 40 acquisitions of the
inherited `mtx_`. Same shape for `class RaftCommo : public Communicator`
(`commo.h:112`).

### Consequence for strategy

"Refactor the C++ first" is still the right posture, but **the first moves are
tooling, not C++**, and they are cheap because `rrr` and the storage headers
already solved them. C++ refactoring becomes the critical path only at G4.

## 3. Corrections to the existing documents

Apply these before trusting any number in them.

| claim | where | truth |
|---|---|---|
| `svr_` appears 2× in `test.cc` | precheck B1 | **0.** The two hits are `svr_id` — a substring match. `test.cc` (2 516 lines) has zero references to the ownership triangle, and `grep -c RaftWorker` on `test.cc` and `testconf.cc` both return 0. |
| B1 blast radius ~35 sites + 3 out-of-scope files | precheck B1 | **39 production sites + 6 out-of-scope files.** Both documents omit `src/deptran/paxos_worker.cc` entirely — 15 `rep_sched_` sites and its own `delete rep_sched_` at :240. |
| B1 "must be settled before almost anything else can be ported" | precheck B1 | **False.** It blocks exactly four functions. `server.cc` (3 861 lines) and `server.h` (1 997) contain zero references to `svr_` or `rep_sched_`. |
| lock chain reaches depth 4 | precheck B2 | **Depth 5.** The chain omits a rung: `stepDown` (`server.cc:3813`) calls `setIsLeader` (`server.cc:1676`, locks) *before* `resetTimer`. |
| "by FUNCTION it is 1" | strategy §1 | **3 functions** (`IsPreferredLeaderConfigured`, `equals_ignore_case`, `is_raft_group_mode_arg` — the latter two deleted as C++ in commit `b0760cd9`) **plus 11 pre-existing C++ types** (6 structs, 5 enums). |
| ~751 generated lines vs ~10 900 hand-written, 6–7% | strategy §1 | **795 generated; 14 392 hand-written** (7 673 production). 6.91% of the production slice, **5.03%** of all of `raft/`. |
| 142 of 146 are `const fn` | strategy §1 | **143 of 146.** |
| `Fiber::create_run` offers no handle to join on | precheck B4 | **False.** It returns `Rc<Fiber>` (`src/rrr/reactor/reactor.rs:889`) and `Fiber::finished()` exists at :916. A structured wait needs no rrr change. |
| the two "toolchain gaps" are independent of any C++ refactor | strategy §2 | True but misleading — both are already solved inside this repo. See G1/G2. |

## 4. Tranche 0 — the correctness net (priority 1, do this first)

**The tree as configured cannot run a single Raft correctness test.**
`build/CMakeCache.txt` has `MAKO_USE_RAFT=OFF` and `RAFT_TEST=OFF`, and
`RAFT_TEST` is set `ON` by no script, Makefile or YAML in the repository. So
neither `deptran_server` nor `simpleRaft` is built, and the 25-case
`RaftLabTest` suite — which is genuinely good at the `RaftServer` layer, 27
`Disconnect` calls' worth of leadership flap — is on no CI path.

What *is* built: ten header/POD gtests (41 cases, none including `server.h` or
`raft_worker.h`), `raft_lab_standalone` (which despite its header comment runs
`DummyDispatcher`, not `RaftServer`, and grants every vote unconditionally), and
`raft_bench`. **The performance harness is the only Raft driver in the
configured tree that runs real consensus.** That is exactly why the
`site_id_`/`loc_id` vote defect was found by a benchmark rather than a test.

### 0a. Make the harness a correctness oracle (~20–30 lines, one file)

`src/deptran/raft/raft_bench.cc` already writes a per-partition sequence number
into every payload at :754 and **never reads it back**. The apply callback is at
:912-941 and already skips past that field to read the timestamp at :921.

- Add `last_seq` plus `out_of_order`, `gaps`, `duplicates` counters to
  `PartitionState` (:353-378).
- In the callback read `read_fixed_decimal(log + kMagicBytes, kSeqBytes)` and
  compare against `last_seq + 1`. That gives ordering, no-loss and no-duplicate
  per partition in one pass.
- Emit them beside `applied_total` in the JSON record (:600-624) and exit
  non-zero when any is non-zero.

Completeness already exists as `offered_total` vs `applied_total`, and
`foreign_applied` (:938) already catches cross-partition misrouting.

### 0b. Leadership flap (`examples/raft_bench.sh`)

Add `--kill-leader-at-sec S`, SIGKILLing the leading process mid-window. The
script already owns the process handles; `raft_bench.cc:892` already counts
leadership notifications and the offer loop already handles rejection at
:763-766.

The correct post-flap assertion is **no gap and no duplicate below the new
leader's committed watermark**. Re-ordering across a flap is legitimate and must
not be asserted away.

### 0c. Regression test for the vote fix

`./scripts/raft_perf/run_sweep.sh` already sweeps both group modes — that is
what surfaced the defect. A short `--partitions 6 --group-mode multi` run is the
regression test: in multi mode with 6 partitions, a leader must be elected for
all six.

### 0d. Decide what to do about `RAFT_TEST`

Either wire `RAFT_TEST=ON` into a CI path so `RaftLabTest` actually runs, or
record explicitly that it is dormant. Do not leave it ambiguous — a suite that
nobody builds is worse than no suite, because it reads as coverage.

## 5. Tranche 1 — unlock the type gate (G1). No C++ changes.

Roughly an afternoon, no production C++ touched, independently reversible.

1. Add `rusty = { path = "../../rrr/rusty-rustc" }` to
   `src/deptran/raft/Cargo.toml` (mirrors `src/rrr/Cargo.toml:14`).
2. Replace the bare `rustc` at `scripts/raft_dsl.sh:384-388` with a Cargo-based
   compile, or pass `--extern rusty=<rlib>`. **Without this, step 1 is inert** —
   `cargo clippy` at :397 resolves the dependency but bare `rustc` never reads
   the manifest.
3. Add opaque `#[repr(C)]` rustc models for `IntEvent` and `PollThread`,
   following `src/rrr/rusty-rustc/src/lib.rs:58-82`.
4. Re-run `bash scripts/raft_dsl.sh --check` (currently: 17 carriers, 26 blocks,
   0 failures, ~11 s).

**Gate:** `--check` stays green. No performance run needed — production compiles
the same generated C++.

## 6. Tranche 2 — unlock header carriers (G2). No C++ changes.

Port the 29-line `post_pass()` from `scripts/regen_storage_dsl.sh:72-100` into
`scripts/raft_dsl.sh`, applied on both sides of the `--check` comparison exactly
as `regen_storage_dsl.sh:101-118` does. Update the stale comment at
`scripts/raft_dsl.sh:11-14`, including its dead pointer to the non-existent
`docs/stage2_open_questions.md` (the live text is `docs/stage2_raft.txt:353`).

This moves raft from "free `const` functions over scalars" to "whole DSL-owned
value types with real methods" — which is precisely what
`docs/stage2_raft.txt:544` asks for: *"NOT MORE SCALAR PREDICATES. That category
is exhausted."*

**What it does not unlock**, so nobody plans on it: methods on existing C++
classes (G3), and packed layouts.

**Gate:** `--check` green; generated C++ for the existing 143 functions
byte-identical apart from `inline` prefixes.

## 7. Tranche 3 — the first real conversion: `ReplicationWakeGate`

`src/deptran/raft/server.cc:89-309`, ~220 lines, 14 methods, 9 fields.

It is the best-matched target in the tree and clears all four gates by
construction: **TU-local** (so G2 cannot arise), **a whole type** (so G3 does not
apply), **no base class** (so G4 does not apply), and after Tranche 1 its
foreign names resolve (it needs only `rrr::PollThread` and `IntEvent`).

It is already written in the exact shape that was proved to transpile and run:
`rusty::Mutex<rusty::Option<rusty::Arc<rrr::PollThread>>> owner_`, two
`rusty::Mutex<rusty::Option<rusty::Arc<IntEvent>>>` waiters (:300-302), six
`rusty::sync::atomic::AtomicBool`, and method bodies like
`auto guard = x.lock().unwrap(); *guard = rusty::None;` (:296-298).

This would be the first conversion in raft that is not a scalar predicate, and
the first that proves `impl` at all.

**Two guardrails.** Write the Tranche 0 coverage over the wake/submit path
*before* committing this — that layer is the untested one. And diff the emitted
C++ for the `set(std::move(n)); return std::move(n);` pattern: benign for
scalars, but this type moves `rusty::Arc`s.

**Gate:** Tranche 0 counters clean, plus a before/after `compare.py` run.

## 8. Tranche 4 — free deletions (pure win, no behaviour to defend)

Each is provably behaviour-preserving by exhaustive caller enumeration, so
neither a correctness argument nor a performance re-baseline is owed.

**4a. Delete the dead `InstallSnapshotCallbackGate` lease machinery (~90 lines).**
`InstallSnapshotCallbackLease` (`server.cc:395-419`) is constructed nowhere in
`src/`; `TryAcquire` (`server.cc:340`) has exactly one caller, that class's own
constructor initialiser. So `ActiveCallbacks()` is structurally always 0, which
makes the drain at `server.cc:1237` and the destructor assertion at
`server.cc:2673` both vacuous. Remove `server.cc:332-419`, `server.h:52`,
`server.h:1317` and the four lifecycle sites.

**Do not "fix" this by wiring real leases** — `~RaftServer` verifies rather than
drains, so live leases would convert a vacuous assertion into an abort.

**4b. Delete four inner `lock_guard`s and one dead method.** This removes 11 of
the 24 nested re-acquisitions and collapses the depth-5 chain, touching neither
`scheduler.h` nor Paxos:

| site | why it is dead weight |
|---|---|
| `server.h:1363` `AmIPreferredLeader` | exactly one caller repo-wide (`server.cc:1271`), already holds the lock |
| `server.cc:1260` `GetElectionTimeout` | exactly one caller (`server.h:1505`), already holds the lock |
| `server.h:1690` `SetLocalAppend` | two callers (`server.cc:1758`, :3140), both already hold it |
| `server.cc:1676` `setIsLeader` | seven nested callers; its only unlocked caller is `server.cc:1180` under `#ifdef RAFT_TEST_CORO` |
| `server.h:1961-1965` `GetPreferredLeader` | zero callers repo-wide — delete the method |

## 9. Tranche 5 — de-reentrancy proper (B2), and why it is not the wall

**The recursive mutex is not an expressiveness blocker.** Adversarial review
could not find one construct it prevents from being spelled. The 24 nested
re-acquisitions become inherent `&mut self` methods on a `RaftState` behind one
`Mutex<RaftState>`, locked at the four RPC entry points (`OnRequestVote`
`server.cc:2946`, `OnAppendEntries` :3183, `OnInstallSnapshot` :3498,
`StartImpl` :3111). `&mut RaftState` states "the lock is held" *as a type*,
where the recursive mutex states it in a comment (`server.cc:3814`) and
re-checks it at runtime.

After Tranche 4b, the remainder is the `X()`/`XLocked()` split for `setIsLeader`,
`resetTimer`, `IsLeader`, `SetLocalAppend` and `PublishAppliedIndex` — a pattern
already present in the C++ at `server.h:1221, 1229, 1242, 1328`.

Two genuine items survive and belong here:

- **The `next_index_` iterator** (`server.cc:2036`): a live mutable cursor into
  a `std::map`, written at :2063/:2070/:2082, held across
  `commo()->SendInstallSnapshot(...)` at :2095. This has no Rust spelling
  (OWN-03) and is a real aliasing hazard, not a surface one.
- **The external reach-ins** are cheaper than documented: 25 total, of which
  **24 are in `test.cc`** and exactly one is production (`server.cc:2108`).

**Sequencing note:** do not put this ahead of Tranches 1–2. It is refactoring
that *enables* the big type move, not a gate on expression.

## 10. Tranche 6 — dissolve implementation inheritance (G4 / B7)

This is where C++ refactoring genuinely becomes the critical path, and where the
`RaftServer` conversion becomes possible at all. `TxLogServer` contributes six
public data members and zero pure virtuals, so the fix is composition: make the
base an interface, move its data into the derived type or into a member struct,
and update the 164 `site_id_` / 20 `partition_id_` / 10 `loc_id_` / 6 `app_next_`
uses.

`scheduler.h` is shared with Paxos (57 `mtx_` uses in `raft/server.cc`, 24 in
`paxos/server.cc`), so this one **cannot be done raft-only**. It deserves its own
design round.

## 11. B1, and why it is not a tranche of its own

B1 is real — `RaftServer` is allocated into `RaftFrame::svr_` (`frame.cc:202`),
escapes raw through `CreateScheduler` (:208, :224) into `RaftWorker::rep_sched_`
(`raft_worker.cc:283`), and is deleted there (:564) while the `unique_ptr` still
holds it. The delete *is* live in production teardown via `shutdown_paxos`. The
double free is latent only because no `Frame` is ever deleted: adding one
`delete rep_frame_` anywhere would make it real. The leak is literally
load-bearing. The `Communicator` leaks identically, so
`Communicator::~Communicator` (`communicator.cc:59-72`) never closes a single
peer connection.

But it blocks **four functions**, and priority (3) says not to spend
expressiveness on it.

**When you do fix it, do not do the `rusty::Box` refactor the older documents
recommend. Copy Paxos.** `MultiPaxosFrame::CreateScheduler`
(`src/deptran/paxos/frame.cc:30-32`) is already `return new PaxosServer();` — no
frame-side owner, the worker is sole owner, and `delete rep_sched_` at
`paxos_worker.cc:240` is correct. Making Raft match means: delete
`RaftFrame::svr_` (`frame.h:46`), make `CreateScheduler` return
`new RaftServer()` (4 sites), route `testconf.cc`'s 25 sites through the
accessor that already exists at `testconf.cc:690-698`, and flip
`server_worker.cc:165-169` from skipping the delete to performing it.

That is ~6 production edits, changes no signature on the shared pure virtual
(`deptran/frame.h:26`), and therefore touches neither `paxos/frame.cc` nor
`paxos_worker.cc` — removing the entire "cannot be done raft-only" cost and the
~16 Paxos sites both documents missed. It also sidesteps the `Box` layout
question (`box.hpp:68-77` deallocating with the static base layout) until the
DSL can actually express an owning type.

## 12. How to prove each tranche held the goals

**Correctness.** Tranche 0's counters must be clean: zero `out_of_order`, zero
`gaps`, zero `duplicates`, `applied_total == offered_total`, zero
`foreign_applied`. Plus the flap case where applicable.

**Performance.** Against the committed baseline:

```bash
tar -xzf docs/performance/raft-baseline-412c225a/records.tar.gz -C /tmp
./scripts/raft_perf/run_sweep.sh --output raft_perf_output/after
python3 scripts/raft_perf/compare.py /tmp/records/rate raft_perf_output/after/rate
```

Exits 0 if nothing regressed beyond 5%, 1 if something did. Read it knowing
that **latency conclusions come from throttled points** and **throughput
conclusions come from the maximum over the series**, not the unthrottled point
alone — see `docs/performance/raft-harness.md`.

The reference point to watch is p50 at `p6/multi/286208B`, offered 190/s:
**7.503 ms ± 0.106**, noise floor 1.0%, so three trials resolve a 1.6% change.
Capacity there is **≥ 1 588 entries/s**. A full sweep is ~4h51m and needs the
machine to itself; for a single tranche, one configuration is usually enough.

**Expressiveness.** State the delta explicitly in each commit: what could not be
spelled before and can be now. `bash scripts/raft_dsl.sh --check` must stay
green, and the block count in `EXPECTED_BLOCKS` (`scripts/raft_dsl.sh:29-58`)
should go up.

## 13. What not to do

- **Do not convert more scalar predicates.** 25 of the 143 existing ones have no
  caller anywhere. `docs/stage2_raft.txt:544` says that category is exhausted;
  adding to it raises the line-coverage percentage while converting nothing.
- **Do not chase full memory safety ahead of expressiveness.** Priority (3).
- **Do not start with B1 or B2.** Both are refactoring, not gates.
- **Do not trust a `server.cc` line number above 2802 in the older documents**
  without adding 18.
- **Do not run two sweeps at once.** They do not collide on ports but they
  compete for the CPU and loopback bandwidth being measured, and nothing in the
  records reveals it.
