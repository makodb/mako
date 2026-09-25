# The RaftLab harness in Rust, and the shim that shrinks with it

The goal, stated once:

> The 25-case RaftLab correctness suite is Rust, running inside the raft
> crate. `class RaftServer` loses its entire `RAFT_TEST_CORO` half, and
> `server_exports.h` loses 41 of its 73 entries — without a single production
> code path changing.

Measured at `80433a59d`. Re-measure before trusting a number.

## Why this first

Of the 73 exports and 61 shim forwarders, **41 of each serve only
`test.cc` and `testconf.cc`**. Nothing else in the tree touches that surface:

```
test.cc                 194 uses   (2,529 lines)
testconf.cc              13 uses     (766 lines)
raft_bench.cc             0                        <- the perf harness
raft_lab_standalone.cc    0
raft_worker.cc            0                        <- production
test_cluster.hpp          0
```

So it is the largest single block of shim in the tree, it is test code, and
removing it cannot regress production or throughput. `raft_bench.cc` drives
the server through the ordinary production interface and is untouched by all
of this.

It is also restorative rather than novel. Those 41 getters did not exist
before the conversion: step C2 added them to replace direct field reads like
`server->state_.raft_log_.last_index()` and 24 `std::lock_guard(server->mtx_)`
in `test.cc`. Moving the harness to Rust lets the test read the struct
directly again, which is what it did originally and is simpler than what is
there now.

## The enabler, and why this was not possible before

`src/deptran/raft/src/server_h.rs` and `server_cc.rs` are **canonical Rust**
since F2.6 (`kind = "canonical"` in `rust-modules.toml`) — compiled by rustc,
never seen by the transpiler. So a real `#[cfg(feature = "raft_test")]` works
in them.

That was false while they were DSL blocks. `#[cfg]` is dropped **silently**
inside a DSL block — `scripts/raft_dsl.sh` rejects one for exactly that
reason, and a comment still sits at `server_h.rs:1742` saying so. It is why
`raft_lab_mode()` exists as a kernel at all: the lab predicate had to be read
on the C++ side and branched on in Rust.

Consequence: **the 41 lab methods can become feature-gated Rust methods with
no C ABI whatsoever.** Not forwarded through a narrower ABI — absent from the
non-lab build entirely.

## What is being ported

| file | lines | what it is |
|---|---|---|
| `test.cc` | 2,529 | `RaftLabTest` — the 25 cases |
| `testconf.cc` | 766 | `RaftTestConfig` — the cluster fixture |
| `test.h` | 108 | the case list |
| `testconf.h` | 201 | the fixture's interface |

The suite runs **in-process**: all five replicas in one process.
`RaftFrame::CreateRaftScheduler` registers each frame in `frames_[locale_id]`
under `RAFT_TEST_CORO`, `RaftLabTest::Run()` drives them, and the verdict
reaches the process exit code through `RaftFrame::RaftLabProcessExitCode()`.
`./ci/ci.sh raftLabTest` configures its own build directory with
`-DMAKO_USE_RAFT=ON -DRAFT_TEST=ON`.

The fixture's surface, from `testconf.h`, is what the Rust `LabCluster` must
provide: `OneLeader`, `NoLeader`, `OneTerm`, `TermMovedOn`, `NCommitted`,
`Start`, `Wait`, `DoAgreement`, `Disconnect`, `Reconnect`, `NDisconnected`,
`SetUnreliable`, `RpcCount`, `RpcTotal`, `ServerCommitted`, `Shutdown`,
plus the private `netctlLoop` / `slow` / `waitOneLeader`.

---

# The phases

Each is separately shippable and ends with the suite green. The order is
forced by one dependency: the Rust harness must be able to reach the five
servers before it can test anything.

## Phase 0 — a cargo feature for the lab

Add `[features] raft_test = []` to the raft crate; have CMake pass
`--features raft_test` when `RAFT_TEST=ON` (the cargo invocation is
`CMakeLists.txt:1134`). Then replace the `raft_lab_mode()` kernel with
`cfg!(feature = "raft_test")` at its two call sites.

Small, independent, and it proves the feature plumbing before anything
depends on it. It also deletes the first lab-related kernel.

**Done when:** `raft_lab_mode` is gone from `server.cc` and the kernel census;
RaftLabTest is 25/25 and the four production suites are unchanged.

## Phase 1 — a Rust registry of the five servers

The harness needs the cluster. Today that is
`RaftFrame::frames_[locale_id]->svr_`, walked from C++.

Add **one** export — `raft_server_lab_register(RaftServerBase*, u32 locale_id)`
— called from `RaftFrame::CreateRaftScheduler` inside the existing
`#ifdef RAFT_TEST_CORO` block, and a `#[cfg(feature = "raft_test")]` registry
on the Rust side.

This is the only export the port *adds*, and it is deleted again in Phase 4
once `frame.cc`'s lab block is the last C++ standing.

**Done when:** a Rust test can enumerate five distinct `RaftServerBase`s and
read `LabCurrentTerm` off each; no C++ behaviour changes.

## Phase 2 — the fixture (`testconf.cc`) in Rust

Port `RaftTestConfig` to a Rust `LabCluster`. Its methods call the server's
Rust methods directly, so every call it makes stops crossing the ABI.

Two pieces need care:
- **Network control.** `Disconnect`/`Reconnect`/`SetUnreliable` reach the
  communicator, which is still C++. Keep the existing
  `raft_commo_set_network_enabled` kernel; do not entangle this with the
  commo conversion.
- **`netctlLoop`** runs on a fiber and manipulates connectivity on a timer.
  It stays on the C++ fiber spawn (`raft_spawn_*`) for now.

**Done when:** `testconf.cc` and `testconf.h` are deleted, `./ci/ci.sh
raftLabTest` is 25/25, and the 13 lab-surface uses they held are gone.

## Phase 3 — the 25 cases (`test.cc`) in Rust

Port by family, running the full suite after each. The families and their
sizes are the natural commit boundaries:

```
elections    testInitialElection  testReElection
agreement    testBasicAgree  testFailAgree  testFailNoAgree
             testConcurrentStarts  testUnreliableAgree  testCount
partitions   testBackup  testRejoin  testFigure8  testLongPartitionRecovery
snapshots    testCreateSnapshotBasic  testCreateSnapshotAndCompaction
             testInstallSnapshotBasic  testInstallSnapshotRejectsStaleTerm
             testHeartbeatTriggersInstallSnapshot  testSnapshotFormatRoundTrip
             testSnapshotManagerSaveLoad  testSnapshotManagerWiring
             testSnapshotMetadataCreation
config       testHeartbeatIntervalConfigurable  testSnapshotThresholdConfigurable
             testLogRetentionWindowConfigurable
load         testHighFrequencyApply
```

**Run both suites in parallel until the last family lands.** The C++ suite is
the oracle for the Rust one; a case is ported when both agree on a clean run
*and* both agree on an injected failure. Deleting the C++ case before that is
removing the safety net while standing on it.

**Done when:** `test.cc` and `test.h` are deleted and `./ci/ci.sh raftLabTest`
reports 25/25 from the Rust suite.

## Phase 4 — delete the shim's lab half

With no C++ caller left, remove:

- the `#ifdef RAFT_TEST_CORO` block in `class RaftServer` — **41 forwarders**
- the 41 `// --- The RaftLab harness surface` entries in `server_exports.h`
- their rows in `scripts/raft_gen_exports.py`'s signature table
- the Phase 1 registry export, once `frame.cc`'s lab block is also Rust
- the `LabAccess`-era getters on `RaftServerBase`, which become ordinary
  `#[cfg(feature = "raft_test")]` methods with no `#[no_mangle]`

**Done when:**

```
server_exports.h entries      73 -> 32
class RaftServer forwarders   61 -> 20
class RaftServer              75 -> ~34 lines
```

and `python3 scripts/raft_regen_exports.py` reproduces the header, the shim
and the exports byte-for-byte from the signature table.

---

# What is left afterwards, honestly

32 exports and 20 forwarders, and none of them is the test harness:

| | count | needs |
|---|---|---|
| worker lifecycle | 15 | `raft_worker.cc` / `server_worker.cc` in Rust |
| inbound RPC (`service.cc`) | 5 | the service conversion |
| lifetime (`new`/`delete`) | 2 | irreducible while any C++ holds the server |
| kernel call-backs | 8 | 6 dissolve with the vendored srpc runtime; 4 are `try`/`catch` around embedder callbacks and stay |
| fiber loops + wake job | 3 | dissolve with srpc |

So this plan removes the largest block, and what remains is the *embedder*
boundary — Mako's worker — plus the small RPC surface.

# Risks

1. **The harness is the safety net for every other conversion in this tree.**
   Every commit in `conversion-log.md` was gated on RaftLabTest 25/25.
   Porting it means rebuilding the net while standing on it. The parallel-run
   rule in Phase 3 is the mitigation and is not optional.
2. **Timing.** Several cases turn on election timeouts and heartbeat
   intervals. A Rust fixture that sleeps differently can turn
   `testReElection` or `testFigure8` flaky without anything being wrong.
   Budget for re-running each ported family 10× before believing it.
3. **The feature flag must not leak.** `#[cfg(feature = "raft_test")]`
   methods must be unreferenced in the non-lab build, or the production
   `libraft.a` stops compiling. A `cargo build` without the feature is the
   check, and it belongs in the gate.
4. **Assertion vocabulary.** The C++ cases return `int` and use a
   `Log_info`/`verify` idiom, not gtest. The Rust port needs a small
   equivalent that reports the same 25-case verdict to the same exit code,
   or CI's oracle changes shape underneath it.

# Independence

This plan touches no RPC, no commo, no service, and no reactor. It can run
concurrently with the srpc work on `srpc-rust-runtime`, and neither blocks
the other.

---

# Outcome

Done, on branch `raft-lab-rust` (worktree `/home/users/zyang2/mako-raft-lab`).
Four commits, one per phase.

```
b0f692e64  Phase 0  a cargo feature for the lab            95 -> 94 kernels
4d74f8bcb  Phase 1  a Rust registry of the five servers    0 new exports
0250cabb4  Phase 2  the fixture's query half               48 AGREE, 0 DISAGREE
5df4650ae  Phase 3a the 11 replication cases               TEST 1..11 Passed
b179c5f92  Phase 3b the other 14                           25/25, both harnesses
bc667dbdb  Phase 4  delete the C++ harness                 73 -> 31 exports
```

## What it measured out at

| | before | after | plan said |
|---|---|---|---|
| `server_exports.h` declarations | 73 | **32** | 32 |
| `class RaftServer`, calls out | 62 | **21** | — |
| `class RaftServer` forwarders | 60 | **19** | 20 |
| `class RaftServer` | 75 lines | **30** | ~34 |
| C++ harness | 3,604 lines | **0** | 0 |

The plan's target, hit exactly. It is met rather than missed by one because
Phase 1 needed no export of its own: `set_site_identity` is already Rust and
is the one point a replica learns which replica it is, so it publishes itself
to the registry, and the export the plan budgeted for C++ to do it never
existed. (Counts are `grep -cE '^[A-Za-z_].*\braft_[a-z_]+\('` on
`server_exports.h` and `raft_server_[a-z_]+\(` inside the shim class, taken
identically at `80433a59d` and at the Phase 4 commit; "forwarders" excludes
`raft_server_new`/`_delete`.)

## Three corrections to the plan, found by doing it

**Phase 1's export was unnecessary** — above.

**Three cases should not be ported, and the plan did not distinguish them.**
Of the 25, only **11 name a `RaftServer`**; between them they make all 176
calls into the shim. Eleven more are pure replication tests that reach the
cluster through the fixture. The last three — `testSnapshotMetadataCreation`,
`testSnapshotFormatRoundTrip`, `testSnapshotManagerSaveLoad` — build a
`SnapshotMetadata`, call `SnapshotFormat`'s statics and drive a
`MemorySnapshotManager` on the stack. Porting them would have added about ten
kernels to delete **zero** exports. They are unit tests of C++ classes; they
stay C++, in `lab_unit_tests.cc`, called through one kernel.

**Running both suites in one process does not work**, so "run both in
parallel until the last family lands" became *one process each way*, selected
by `MAKO_RAFT_LAB_RUST` while both existed. A second suite in the same process
starts on a cluster whose indices, terms, snapshot managers and retention
windows the first has already moved, so half the cases would measure something
other than what they say.

## The kernels it cost

Ten, all `#ifdef RAFT_TEST_CORO`, against 41 exports removed.

| kernel | why it cannot be Rust |
|---|---|
| `raft_lab_make_commit_command` | `PayloadMember<MakoCommands, T>::KIND` |
| `raft_lab_commit_tx_id` | as above, read direction |
| `raft_lab_make_learner_action` | the apply callback is a `std::function` |
| `raft_lab_frame_rpc_count` | `RaftCommo` is C++, and `frames_` is private |
| `raft_lab_new_snapshot_manager` + `_delete_all` / `_probe` / `_copy_latest` | `shared_ptr<SnapshotManager>` is a C++ interface |
| `raft_lab_byte_string_from` | `std::string` payloads |
| `raft_lab_make_reject_prepare_cbs` + `_reject_prepare_called` | test 58's probe |
| `raft_lab_make_probe_cbs` + `_probe_flags` / `_probe_release` | test 60's probe |
| `raft_lab_cpp_unit_tests` | the three C++ unit tests |

The first three are NC5 of `commo-service-rpc-to-rust-plan.md`: Rust can hold
a `janus::Command` but cannot be a member of `MakoCommands`. The last two
pairs exist because the prepare callback returns a
`std::unique_ptr<PreparedStateMachineSnapshotInstall>` — a C++ interface with
a vtable, which Rust cannot implement. So the *probes* are C++ and the test
*logic* that installs them and reads their verdict is Rust.

## Two bugs found on the way

- `scripts/raft_gen_exports.py` had three **absolute** paths into
  `/home/users/zyang2/mako`. Regenerating from a worktree read the main
  checkout's `server.h` while rewriting the worktree's. Now `__file__`-relative.
- A fresh worktree needs `LD_LIBRARY_PATH` as well as `LIBRARY_PATH`:
  `ld` resolves `libsnappy.so.1` as a transitive `DT_NEEDED` of
  `librocksdb.so`, which `LIBRARY_PATH` does not cover.
