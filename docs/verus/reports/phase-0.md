# Phase 0 report (in progress)

Plan: [../modification-plan.md](../modification-plan.md), Phase 0 and the
stopping point 0.7 point 1. Raw results: `$RESULTS = ~/raft-test-results/verus`
(not in git).

## 1. Environment (plan 0.3)

- `~/mako-verus-env.sh` written as in 0.3.
- `$VERUS_NEW` smoke test: `verus/commit_rule.rs` → 11 verified, 0 errors
  (`$RESULTS/env/verus_new_smoke.log`).
- `$VERUS_PIN` was broken as the plan said (`librustc_driver-832cf6cfb1386559.so`
  missing). Repaired with `rustup toolchain uninstall 1.97.1 && rustup
  toolchain install 1.97.1 --profile minimal` (network reachable). Smoke test
  then 11 verified, 0 errors; `verus --version` = 0.2026.08.02.b677dd5,
  toolchain 1.97.1 (`$RESULTS/env/verus_pin_smoke.log`).
- The worktree had no submodules checked out; `git submodule update --init`
  of `third-party/{rusty-cpp,googletest,yaml-cpp,mako-redis}` (pinned
  gitlinks) was needed before CMake would configure. Not in the plan.
- Phase-start merge (0.8): `origin/mako-dev` had no commits not already in
  `verus-raft`, so no merge commit was needed.

## 2. Spec (plan §4.2, §4.5)

- **v0**: `$GLR` branch `mako-spec` created at `d7e04ed7`; tag
  `raft-spec-v0`. Full crate under `$VERUS_PIN`: **2129 verified, 0 errors**,
  5 min 26 s, 6.7 GB peak (`$RESULTS/spec/v0-pin.log`). This is the v0
  reference count.
- **S1**: see §2.1 (in progress).

## 3. Spikes (plan Phase 0)

Spike crate: `PeerTable::majority_match_index` (`src/server_h.rs:881-903`)
inside `verus!`, body unchanged, with loop invariants, `decreases` and the
postcondition `r <= last_log_index`. `$VERUS_PIN`: 5 verified, 0 errors.

- **(a) cargo, ghost code erased: works.** `vstd`, `verus_builtin` and
  `verus_builtin_macros` at exactly the pinned version
  `=0.0.0-2026-08-02-0125` are on crates.io; with them as ordinary
  dependencies, plain `cargo build --release` (stable rustc 1.97.1, the
  lane's `panic = "abort"` profile) builds the crate, ghost code erased, in
  19 s from scratch. No `verus --compile` step is needed.
- **(c) opt-level / LTO: identical code.** The lane builds with cargo's
  default release profile (opt-level 3, no LTO). At that profile the erased
  function compiles to the same machine code as a plain-Rust twin of it:
  102 instruction lines, byte-identical after label normalisation (compared
  through an identical `#[no_mangle]` wrapper, since rustc no longer emits
  small non-generic functions in their own rlib).
- **(b) rusty-cpp: not directly; yes through erased Rust.** Transpiling the
  `verus!` crate directly turns the whole block into `// TODO: verus!(...)`
  (silent loss; `scripts/raft_cpp_stage.py` already rejects that). Erased
  Rust from rustc's own expansion
  (`RUSTC_BOOTSTRAP=1 cargo rustc -- -Zunpretty=expanded`, minus four
  prelude/vstd lines) is clean plain Rust and transpiles with 0 TODOs.
  `assert!(c)` expands to `::core::panicking::panic(..)`, which rusty-cpp's
  panic shim lowers. Two caveats: `-Zunpretty` needs `RUSTC_BOOTSTRAP=1` on
  stable, and it expands every macro, not only `verus!`. Separately, Verus
  rejects `assert!` with format arguments in exec code, so M10's rewrites
  (`raft_verify`/`panic!` → `assert!`) must drop message arguments.
- **(d) spec import: both options work; (i) is the faster one.** Both were
  tried on scratch copies of `$GLR/src` (with S1), checking a Mako-side file
  that holds `majority_match_index` and a lemma whose `ensures` is S1's
  `LRejectAppendEntries` over the group's `LState`/`LConstants`.
  - (i) `--export`/`--import`: needs one visibility change in the group's
    crate root, `mod protocol;` → `pub mod protocol;` (`src/lib.rs`, not a
    statement-bearing file, so not a spec version bump). Then
    `verus --crate-type=lib --crate-name glr src/lib.rs --no-verify --export
    glr=glr.vir --compile -o libglr.rlib` (47 s, 1.7 GB, once per frozen
    version), and the Mako crate verifies with `--extern glr=libglr.rlib
    --import glr=glr.vir`: **1 verified, 0 errors in 1.9 s, 0.5 GB**.
  - (ii) port module (`src/ports/mako`, `#[path]` to the Mako file, as the
    group did for raft-rs): no upstream change at all;
    `--verify-only-module ports::mako`: **4 verified, 0 errors in 45 s,
    1.8 GB** (the whole crate is loaded each time).
  - Recommendation: (i) for the edit-verify loop of Phases 6 and 8 (seconds
    instead of a minute), with the export regenerated from the frozen tag by
    `verify_core.sh`; (ii) as the fallback if the visibility patch is
    unwelcome. Either way the certificate's trust in the spec rests on the
    full-crate run of `verify_spec.sh`.

## 4. Open question 3: does the leader-change callback re-enter Raft?

No, on today's paths. `setIsLeader` (caller holds `mtx_`) fires
`leader_change_cb_` synchronously (`src/server_h.rs:2379-2403`). The worker's
lambda (`raft_worker.cc:314-338`) takes `election_state_lock` (recursive),
then `callback_registry_mutex_` (released before notifying), then
`NotifyRaftLeaderChange` → `raft_handle_leader_change`
(`raft_main_helper.cc:1325-1338`): `apply_callbacks_for_partition` takes
`raft_global_callback_mutex` and re-registers callbacks on the worker
(`callback_registry_mutex_` again; the `register_*` functions only test
`rep_sched_ != nullptr`), notifies `leader_wait_cv`, then calls the
application's election callback with no lock (raft_bench: a printf; Mako's
`mako.hh:654`: `break` for both Raft cases). Nothing on that thread calls back
into `RaftServerBase`. `RaftWorker::Next` copies callbacks under its mutex and
invokes them outside it (`raft_worker.cc`, "Copy under the mutex and invoke
outside it"), so the apply thread holds no worker mutex while the
application runs.

The hazard F6 removes is real but latent: the whole chain runs with `mtx_`
held, so any future application callback that calls `IsLeader`,
`GetLeaderHint` or `add_log_to_nc` on the same thread would self-deadlock on
the non-recursive `mtx_`.

## 5. Bugs found

See [../bugs-found.md](../bugs-found.md): B1-B8. New ones not in the plan:
B1 (vote "no" quorum off by one: a 3-node candidate that lost can never be
decided early and waits its full 1 s), B7 (`get_outstanding_logs` mixes two
counters), B8 (`setIsLeader`'s stale-publication term check is a tautology).

## 6. Plan corrections found while executing

1. Phase 1's lab case "vote request to a server whose `rpc_ready_` is false
   ... gives up on a 'no' quorum without waiting for its deadline" only holds
   in the 5-node lab if all four peers refuse (B1).
2. §6 Tier 3 passes a single setting directory to `jetpack_compare.py`, which
   expects its parent (`ROOT/<setting>/round<k>`); `jetpack_gate.py` accepts
   either.
3. G7 as written would mostly measure the preferred-leader grace knob: the
   launcher's grace window (30 s) covers the kill at 5 s, during which the
   surviving non-preferred replicas use 5-10 s election timeouts. The G7
   command sets `MAKO_RAFT_PREFERRED_GRACE_US=6000000` and kills at 6 s, so
   the kill lands after both followers' grace ends and steady 0.5-1 s
   timeouts are measured.
4. `examples/raft_bench.sh --kill-leader-at-sec` wrote no record at all, and
   survivors never offered load, so "first commit after failover" had no
   observable; the harness adds both (M0).
5. The trace kit patch's raft-rt hooks named C++ symbols directly, which
   breaks raft-rt's standalone `cargo test` binaries (undefined
   `raft_trace_through`); they now go through installable hook slots
   (`rt/src/trace.rs`).
