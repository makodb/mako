# The Raft C++-to-Rust conversion, commit by commit

211 commits, `65205328c`..`91277b4ac`, 2026-09-12 to 2026-09-29. The count
excludes merges and the srpc subtree squashes; 132 of the commits touch
`src/deptran/raft`. (By this rule, the range this header first covered,
`..087df006d`, holds 189 commits, not the 184 it originally stated.)

Every commit built, passed RaftLabTest, and passed `scripts/raft_dsl.sh
--check` before it was pushed. RaftLabTest had 25 cases until phase 15 added
two, so it is 27/27 from `a22d391cc` on.

From phase 8 on, the gate also runs the canonical-source census,
`rpcgen_rust.py --check`, clippy over both cargo features, and the crate's
own tests.

One caveat on "passed": RaftLabTest's TEST 9 counts idle RPCs against a
ceiling of 60, and fails spuriously when the machine is loaded. 46 of 48 lab
runs in phase 9 passed, and both failures followed a heavy build. The gate
now waits for load < 1 and settles 60 s.

Read the phases in order; within a phase the commits are chronological.

## 0. Before conversion was possible (09-11 .. 09-12)

The target was reduced first, and then measured, because neither had been
done and both changed what "converted" would mean.

| commit | what it did |
|---|---|
| `d288748f5` `3ba6edbc9` | cut the disk side and everything but Vote / AppendEntries / InstallSnapshot, leaving a memory-only Raft |
| `52c2d6c62` .. `1486c0ac1` | a standalone three-process performance harness, with a sweep driver and plots |
| `ed82389ca` | made that harness an ORACLE for log integrity -- gaps, duplicates, out-of-order -- not just a speedometer |
| `3ae871794` | recorded the pre-conversion baseline, and corrected two claims it falsified |
| `d408d1f49` | wired RaftLabTest into CI and made its verdict reach the exit code |
| `65205328c` | **unlocked the two transpiler gates that had confined conversions to scalar predicates** |

`65205328c` is where the conversion becomes possible at all. Before it, only
free `const fn`s over scalars could be expressed.

## 1. Making the shape expressible (09-12 .. 09-13)

Inheritance and mutexes had to be dealt with before any state could move.

| commit | what it did |
|---|---|
| `340bd3d37` | dissolved `TxLogServer`'s implementation inheritance -- six shared members pushed down into the two concrete servers, because implementation inheritance has no Rust spelling |
| `f060472e9` | `ReplicationWakeGate` becomes the first non-scalar DSL type |
| `5b5825659` | `TxLogServer` becomes a DSL `pub trait` |
| `4427129a9` | stopped holding a dereferenced `next_index_` cursor across InstallSnapshot -- a real bug, found by asking what Rust would say |
| `26e9107bd` `01d07f758` `43f436a17` | recorded why the mutex was right before it was wrong; corrected two claims about what the transpiler can do |

## 2. The recursive mutex (09-16)

A `std::recursive_mutex` cannot be spelled in Rust, and worse, it tracks
ownership by THREAD while this runtime schedules FIBERS that share one.

| commit | what it did |
|---|---|
| `659e166d5` | took `mtx_` below the availability check in the InstallSnapshot callback -- the one genuine near-miss |
| `09be9181d` | **removed the recursive mutex**: eight function splits, an instrumented depth counter (36,327 nested acquisitions -> 79 -> 0), and 23 sites in `test.cc` that were the real culprit |
| `fc023f0a6` | documented the re-entrancy contract the demotion created, at the registration point |
| `bda58df50` | made re-entry ABORT with a diagnostic instead of hanging (`RaftCheckedMutex`) |

## 3. Giving the data to Rust (09-13 .. 09-16)

The order is deliberate: data first, bodies after. Converting bodies over a
C++ data model adds bridge code that the later ownership change deletes.

| commit | what it did |
|---|---|
| `313be117f` | `HeartbeatAuthority`, the first type carved out of HeartbeatLoop |
| `4a5969ada` `99765a06f` | the per-peer index maps merge into one DSL-owned `FollowerProgress`, then a dense `PeerTable` |
| `fc2f7c929` | the round scope, reachable only through methods |
| `d165f4238` | the read-index authority ledger and PHASE 3's scan |
| `cb0c4c207` | the in-flight AppendEntries table, carrying the wire types opaquely |
| `d295a4842` | stopped mutating committed log entries at send time -- a log that can be modified after commit cannot state its own invariants |
| `c95621a33` `2b359f186` `3c6c739a1` `cd8188d88` | the log entry, then the log container, become Rust-owned; read paths stop creating entries; two dead members go |
| `ae74b7648` | store the log in BLOCKS, which removed a reallocation stall the previous commit's measurement had exposed |
| `d05852665` `c94966626` `7bf6d1f2d` | the mutex-guarded consensus cluster becomes one Rust type, absorbing leadership, read-index and log clusters |
| `95d72c0a3` `aac58e63e` `125815145` | the log, the peer table, the atomics and this server's identity move into `RaftConsensusState` |

`bfb141422` in the middle of this is the one that matters methodologically:
it measured a conversion, found a REGRESSION, and the next commit fixed it.

## 4. The state struct, then the bodies (09-17 .. 09-18)

| commit | what it did |
|---|---|
| `39eed1542` | **`RaftServer`'s state becomes a DSL struct** (`RaftServerBase`) -- the pivot the rest depends on |
| `b295d252a` | the lock guard, and the first thirteen bodies |
| `7f016868f` | rrr logging reaches the DSL (one shim per level and arity), which unblocked 160 call sites and twenty-seven more bodies |
| `dc1ef5c97` `63f10fb44` | the kernel bridge; `setIsLeader`, the election timer, `RequestVoteImpl` |
| `1d8edc6e3` `95f9277c5` `84c0a5c06` `25b1b95a0` | heartbeat PHASE 0, PHASE 3, PHASE 2's reply decision, then PHASE 2 |
| `39890ade4` `1cc23826f` | fixed the commit-index defects in both phases, then gave both one shared selector |
| `c5ea6348d` `79fce51c6` `1e83ddebb` | startup, shutdown, submission, the apply queue; both inbound RPCs; PHASE 1's loop |
| `17c3e7af6` `ff7be1750` `0e9b3ead5` | SetupInternal, CreateSnapshotLocked, OnInstallSnapshot, the apply thread's loop, snapshot recovery |
| `0e27fe620` | the loops hold a TYPED server, and ten trampolines go away |
| `be3e24a4b` | PHASE 1's snapshot reply and batch selection |
| `f83537b7f` `54fed1ad5` `fd49793dd` `cfd311109` | four gates against transpiler constructs that are lowered WRONG IN SILENCE |

The four gate commits are worth separating out. Each one found a way the
transpiler can produce code that compiles and is wrong -- a dropped
statement, a missed TODO marker, an unauthenticated `#[cpp_inherit]` that
silently emits no base class -- and made the build fail instead.

## 5. Ownership, not ratio (09-18 .. 09-19)

The correction that shaped this phase: the DSL emits C++, so the Rust
*share* can reach its ceiling with zero Rust machine code in the binary.
Ownership is the variable. A boundary built of tiny accessors is the symptom
of data on the wrong side.

| commit | what it did | kernels |
|---|---|---|
| `1786e01ea` | `peer_sites_` becomes `rusty::Vec<u16>`, `current_config_` deleted; the round put back in protocol order | 93 -> 89 |
| `071989b10` | the two RPC entry points hold a typed server; eight forwarders go | 89 -> 81 |
| `bdd1dd9a8` | **the apply queue moves into Rust**; and the ODR post-pass stops mangling lambda bodies | 81 -> 76 |
| `7855ebd85` | `decoded_terms_` becomes `rusty::Vec<i64>`; the startup gate becomes `Mutex<bool>` + `Condvar` | 76 -> 72 |
| `41648c4ba` | the apply queue and its epoch become ONE type inside ONE lock | 72 -> 72 |
| `26ca518a3` | **the opaque carriers stop lying about their size** -- thirteen `[u8; 0]` models given real size and alignment, pinned from both sides | 72 -> 72 |
| `65e80b569` | **the wake gate moves into `RaftServerBase`; `RaftServer` holds NO data** | 72 -> 71 |
| `252db7d9c` | the four wait methods become Rust; one reactor factory call left | 71 -> 67 |
| `8fac9f5f5` | `HeartbeatRoundState` becomes Rust; the driver joins its phases; `HeartbeatLoop` goes | 67 -> 64 |
| `f8b83c4e2` | four env getters become one call returning a struct | 64 -> 61 |
| `34ce28b87` | `batch_buffer_` becomes a `rusty::Vec`, and stops being copied wholesale each round | 61 -> 59 |
| `39f07d140` | the lock guards declare their destructors non-throwing | 59 -> 59 |
| `68fe87ce1` | the per-slot log lookup becomes Rust; `FindRaftInstance` goes | 59 -> 58 |
| `244b9523c` | PHASE 1's payload selection stops asking C++ where the log is | 58 -> 57 |
| `a124990d7` | shutdown and the apply-thread start become Rust | 57 -> 56 |
| `37d308ed4` | `doVote` becomes Rust; four banners describing nothing are deleted | 56 -> 54 |

## 6. The plan's steps: interface, opacity, ABI (09-20 .. 09-21)

the Raft migration plan (removed; see git history) was written, and its steps A through F1
executed, each gated, built in both trees, run through RaftLabTest (25/25 every
time) and the four production Raft suites, and committed on its own.

| commit | step | what it did |
|---|---|---|
| `526595eb3` | -- | retired the migration write-ups; one plan from here to a rustc-compiled Raft |
| `c83a34bc0` | A | `RaftSpecific: TxLogServer` declared beside `TxLogServer`, implemented for `RaftServerBase`; found the adapter hazard (a trait impl without `#[cpp_inherit]` on a move-only struct does not compile) |
| `738a8b7f2` | B | zero `dynamic_cast`/`static_cast<RaftServer*>`; `RaftFrame::CreateRaftScheduler`, `RaftSpecific*` in the workers and the service |
| `eb5482070` `90b6f828a` `bcc298560` `2996cc572` | C | no hand-written C++ names a field of the struct: kernels take values and carriers, the log/batch/decode/config writers are Rust, the lab harness reads through getters, the shim constructor is one call; `scripts/raft_field_census.py` is the done-test |
| `5acdad280` | perf | 25 paired trials, before step A vs after C3: +0.60% median, p = 0.69 -- no cost |
| `204751587` | D1 | nothing non-trivial crosses the boundary by value; no Rust body clones a carrier |
| `e60267d97` | E | Rust stays on the fiber; `panic = "abort"`, guard page and one-thread-per-fiber written where they bind |
| `1d24bbf53` `782b34f48` `3cbcdcfe6` | F1 | the seam is a C ABI: 71 `extern "C"` functions defined in Rust, `server_exports.h`, a shim that holds a pointer and forwards; hand-written C++ knows the struct only as a pointer type |
| `3b0d810dd` | -- | F2 (the cutover proper) inventoried; a project of its own |
| `8b1cbfe97` | -- | F2 slice 1, facade -> runtime: the crate is a `staticlib` (71 exports defined, 94 kernels undefined); every opaque carrier has a kernel `Drop`; the wake gate's reactor handles are pinned carriers over `rusty::Arc`, built by facade factories and copied by `Clone` because a `rusty::Arc` has no empty state |
| `87ce66914` | -- | F2 slice 1c: the 44 `raft_log_*` facade functions are the Raft logger under rustc (level check, fmtlib-style substitution, one line to `rrr::log_line`); the transpiled build is unchanged |
| `424fe4c79` | -- | F2 slice 2: the wake gate is constructed, reserved and driven from Rust; the reactor job carries a `Box<GateWakeJob>` token back to the `raft_wake_job_run` export; the C++ wake-job pair and three kernels are deleted |
| `5e97e4884` | -- | F2 slice 3: the lab log fingerprint is two scalar exports (no `RaftLog&` crosses); the five by-value setters cross by pointer + clone kernel (D2 at the seam); 17 Rust predicates no Rust called are deleted and their 6 C++ callers compare in C++; the kernel-result PODs have a C++-visible block of their own, the RPC bodies are exports, and the 100 predicate static_asserts are Rust const asserts |
| `1de45affa` | -- | THE CUTOVER: server_h.rs / server_cc.rs are canonical Rust compiled by cargo into libraft.a and linked by CMake; the generated C++ of the server, the bridge, the alloc/free kernels and the facade's C++ halves are deleted; C++ holds `struct RaftServerBase;` as a name |
| docs | -- | perf verdict on the cutover: 25 paired trials, `19cfbb213` vs `1de45affa`, median +0.32%, p = 1.000 -- no detectable cost of the whole conversion |
| `9f3f350ae` | -- | Rust calls Rust: the two RPC forwarder kernels and their exports are deleted; OnRequestVote / OnAppendEntries call the bodies in server_cc.rs directly |

## 7. The lab harness becomes Rust (09-23)

The C++ lab suite had been the oracle for every commit above. Once the Rust
harness could reproduce its verdict, the oracle itself could go.

| commit | what it did |
|---|---|
| `b0f692e64` | the lab predicate becomes a cargo feature, not a C++ kernel |
| `4d74f8bcb` | the lab cluster registers itself, adding no ABI |
| `0250cabb4` | the fixture's invariant readers, in Rust and checked against the C++ |
| `5df4650ae` | Phase 3a -- eleven replication cases are Rust (54-60, 67-69, 72) |
| `b179c5f92` | Phase 3b -- **all 25 cases pass under the Rust harness**; `ci.sh` runs the binary once each way and requires 25 from both, so a half-ported harness cannot pass by skipping |
| `80f715dad` | Phase 4 -- `test.cc` (2,529 lines), `test.h`, `testconf.cc`, `testconf.h` deleted, and with them **41 of the 73 exports** that existed only so those files could reach the server |
| `9ad3870a0` `6f367f0bb` | followed the deletion through the comments and the borrow filter |

`80f715dad` is the largest single deletion in the conversion: 3,604 lines of
C++ and 41 ABI entry points, none of which had any production caller.

## 8. Adopting the srpc subtree (09-24 .. 09-25)

`src/rrr` -- the vendored RPC library the whole conversion sits on -- became
`src/srpc`, a git subtree of `stonysystems/srpc`, and was then pulled 141
commits forward. This was not Raft work, but nothing further could be done
without it: the Rust lane needs srpc's own Rust modules, and the two lanes
could not be linked into one binary while both defined the same C symbols.

| commit | what it did |
|---|---|
| `7ace54e1f` | merge mako-dev's restructuring: 501 files auto-merged, 28 conflicted, each resolution recorded |
| `cb8e4e6b3` | two executable defects the C++ lane never exercised |
| `bbbd51d89` | the Raft crate takes `srpc` as a dependency, to find out what linking both lanes costs |
| `9dd6ff492` | **the subtree pulled forward 141 commits** (2026-08-26 -> 09-23; 244 files, +25,902 / -4,501), for the plain-C epoll kernel that both lanes can name |
| `b21c36431` `bd42028ab` `4a8e233eb` | the kernel list, the canaries and the C boundary are taken from srpc's own manifest instead of repeated |
| `157bc8837` | one upstream refactor (`rusty::Cell` -> `srpc::SharedCell`) invalidated pinned values in **seven** layers of the gate, each only visible after the previous passed |
| `179182a10` | `rusty-rustc` and `rusty-cpp-markers` move out of the subtree -- they are Mako's, and living inside `src/srpc/` would fight every future pull |
| `95d085999` | `cpp_value_init` retired; the zero-init guarantee moves to the field census |
| `66d1a96c7` | deptran and mako follow srpc's API forward |
| `49defdf5e` `b0ee6d2e1` | the adoption recorded, with two refuted plan revisions kept |

The two refuted revisions are the useful part of `49defdf5e`: v1 assumed Raft
carries its AppendEntries payload as opaque bytes, which it does not.

## 9. The wire, and the server's last C++ field (09-25)

The plan from here is the two-lane RPC plan (removed; see git history). Stages 0 through 3 ran
in this phase.

| commit | stage | what it did |
|---|---|---|
| `561670f76` | 0a, 2a | the lanes can link -- `fiber_task_entry_thunk` was a defined `T` symbol in both archives; and the Rust wire structs are generated |
| `493881a8e` `c0706bc73` | 2a | service dispatch, the client proxy, and AppendEntries -- with no wire change |
| `b86f2dc81` | -- | the proxy copies `Copy` requests into its closure instead of cloning |
| `41bc18569` | -- | `rpc.rs` is **generated from `rcc_rpc.rpc`** instead of being a checked-in snapshot free to drift from the header the C++ lane uses |
| `458d75b5b` | 2b | the four wire ids get a file of their own, because rpcgen only reserves an id while the header still declares its service |
| `ad19d6cc7` | 0b, 0c | srpc's nine C kernels were built **twice, by different compilers**, and both copies reached the binary; 10 of 12 external symbols in two objects were defined in both archives, and `srpc_rand.c` branches on `__clang__` |
| `08d619f66` | 0d | the libraft rebuild watches srpc's Rust sources |
| `5d3591c95` | 3a | `Communicator`'s data becomes **one** Rust value type, `PeerRegistry`, written once for both lanes -- neither flattened into each subclass nor left in C++ with a second copy on the Rust side |
| `fa5a2b33a` `18012740c` | 3a | the Rust `RaftCommo` landing pad, then its deletion: `PeerRegistry` superseded it, and it had already dropped `partition_peers_` |
| `284860c8f` | 3a | **`commo_` leaves the server.** It was the one field of forty-eight that is not `Send`, and `trait Service: Send + Sync` is what the Rust srpc lane demands. The RPC gate moves into the server with it: three ABI crossings become one |
| `a353e30b4` | -- | the lab build is clippy-clean, so the gate can cover it |

`284860c8f` is the pivot of this phase. `RaftServerBase` is `Send + Sync`
because the non-`Send` field was **deleted**, not because anything asserted it
-- proven by `tests/server_is_send.rs`.

## 10. Stage 3e: Raft's own RPC lane, in Rust (09-25 .. 09-26)

| commit | what it did |
|---|---|
| `6fd0da9cc` | the service: `impl srpc::server::Service for RaftRpcService`, four handlers calling `ServeVote` / `ServeAppendEntries` / `ServeInstallSnapshot`, registered and routed by the generated `rpc.rs` |
| `6e76caec9` | the transport and **all three send paths**: one poll thread, the peer clients, and the three operations Raft sends. One type, because inbound and outbound share `svr_poll_thread_worker_` -- moving the server without the clients would ADD a poll thread rather than swap one. `BroadcastVote` moved, which stage 3a's table had said it could not |
| `2a0deb21f` | `tests/transport_roundtrip.rs` over real loopback TCP, built from the same generated code production uses -- and it **immediately caught a split-brain bug**: `broadcast_vote` derived the quorum from the *reachable* peers, so a partitioned candidate would elect itself on its own vote |
| `d93451ecd` | the transport owns its server and gets a C ABI |
| `93f818a53` | resolved **by server identity**, not by a field: `RaftTransport` is `!Send` (its clients belong to a poll thread) while `RaftServerBase` must stay `Send + Sync`, so the assertion lives once, in the open, beside the registry |
| `551ee1a49` | 3e's cutover enumerated site by site, so it is executable |

Stage 3e is built, tested and verified but **not yet wired in**: nothing calls
`raft_transport_serve`. The cutover is the open item.

## 11. Making the documents true (09-25 .. 09-26)

| commit | what it did |
|---|---|
| `ea455e961` | a one-page C++/Rust correspondence table |
| `6de31e23a` | corrected the kernel justification -- most kernels are positional, not required |
| `a8691d647` | every open plan item gets a measured reason, not a dependency note |
| `ee9747171` `59b4708f1` `2087d841b` | the plan was rendering as **indented code blocks**: 412 continuation lines indented six spaces under list items, producing 257 code blocks and 90 literal `**` in a 900-line file. Reindented, tables rewritten, and "measured" removed from untimed claims |
| `edd49ff35` | a six-agent audit of both documents against the tree: **138 findings on 113 claims -- 71 stale, 37 wrong, 25 correct**. The correspondence file's numbers were wrong in four different ways, so it is now COMPUTED by `scripts/gen_correspondence.py` with the counting rule printed beside each |
| `b1ccc9104` `49341b986` `538df5f8c` | the audit applied: 15 corrections to the plan, then the last ten, and the generator stopped self-staling |
| `558374062` `087df006d` | the Rust-lane RPC call and response path -- then its correction, because `Start` does more than the first version said and does touch C++ (`raft_command_clone_into`) |

The audit is the honest entry here. Of 113 claims across two documents I had
written, 25 were correct. The generated correspondence file exists because a
number a human maintains by hand is a number that is wrong by the next commit.

## 12. Two lanes, and the two srpc regressions (09-26)

The plan's revision 5 made Raft one Rust source with two runtimes, the model
srpc itself uses. This phase built the Rust one and, measuring it, found and
fixed two srpc regressions that had been hurting BOTH lanes.

| commit | what it did |
|---|---|
| `65754b66f` | the two-lane plan and its validation (143 claims, 45 upheld findings) |
| `70fc2c0da` | mako-dev merged: 22 conflicts, from two separate squashes of one srpc range |
| `edc5db890` | **the reactor evicts timed-out events again** -- the latency regression. p50 back at the pre-regression binary (2.70-2.75 ms vs 3.25-3.30 ms), flat with run length |
| `4149bf438` | R3's data: 62 of 63 rate points against the C++ baseline within noise or better |
| `217cc26d4` | **`MAKO_RAFT_LANE=rust`**: the core crate names no srpc type; `raft-rt` holds the seam over the Rust srpc runtime, the transport, the service and the generated wire code; RaftLabTest 25/25 on both lanes. Three wire defects found on the way, each of which would have broken the Rust lane on real traffic |
| `b6141e723` | **large payloads**: AppendEntries batches bounded by bytes (a 73 MB batch was past srpc's 64 MiB frame limit and re-sent forever), srpc's inbound compaction made linear, and its per-byte frame copy made a memcpy. 286 KB saturation 10/s -> 413/s (hybrid) and 312-319/s (rust), against the baseline's ~280 |

The finding worth keeping is how the second regression surfaced. The Rust
lane's first production suite run failed a throughput check, which looked like
a Rust-lane slowdown -- and `raft_bench` said the Rust lane was FASTER. The
difference was payload size. Following it down found a bug that had been
capping every large-payload run on this branch since the subtree pull: Mako's
own replication suite had been replaying 155-368 batches where it now replays
~8,000, on both lanes. The hybrid lane had been failing that check 2 times in
5 all along.


## 13. The Rust lane becomes the default (09-26)

| commit | what it did |
|---|---|
| `9a361eccd` | **no payload copies on the Rust lane.** The leader encodes the `Command` straight into the request frame (`serialize_with`, through a C++ `EmitSink` that forwards to the Rust archive). The follower decodes from a slice of the frame (`AppendEntriesRequestRef`). 286 KB saturation: hybrid 380-413/s, rust 682-696/s, C++ baseline ~280. T4's paired trial (rust vs hybrid, 25 ABBA pairs): 4 KB saturation throughput +40.85%, p50 -29.0%. T3's mixed-lane clusters: 12 of 12. |
| `2cd7f5e74` | **`MAKO_RAFT_LANE` defaults to rust** (T5). The full sweep against the C++ baseline `412c225a` ran 624 of 624 runs (`docs/performance/raft-rust-9a361eccd`). The kernel classification is now computed and checked by the build (S1). The Rust seam aborts on an off-thread `IntEvent` (S2). The lab's assertion macros became functions, because a transpiled macro becomes a `// TODO` comment and the transpiled lab had reported 25/25 while checking nothing (189 dropped checks). |

## 14. The transpiled C++ lane (09-27)

| commit | what it did |
|---|---|
| `c23e397e4` | **`MAKO_RAFT_LANE=cpp`**: the same Rust core, transpiled at build time into 22 C++20 modules (`cmake/raft_cpp_lane.cmake`, `scripts/raft_cpp_stage.py`) and linked over the C++ seam and C++ srpc, with no rustc in the lane. Two silent transpiler miscompiles were fixed and guarded in the stage step: an untyped `Vec::new()` lowered to `Vec<bool>`, and `[..].into_iter().max()` returned a reference into a dead iterator. `raft_lane_check` enforces exactly-once kernels on every lane, and identical rustc/transpiled cores on hybrid. |

The Raft core is now one Rust source with three runtimes: rust (rustc + Rust
srpc), hybrid (rustc + C++ srpc), and cpp (transpiled + C++ srpc).

## 15. The snapshot store on the Rust lane (09-27 .. 09-29)

Plan phase N (the two-lane RPC plan (removed; see git history), revision 7). The Rust lane's
snapshot store is converted to Rust. hybrid and cpp keep the C++
`MemorySnapshotManager` by decision.

| commit | step | what it did |
|---|---|---|
| `21a5d7a99` | -- | Phase N planned, N0-N13, with correctness and performance gates |
| `b7ac74a0b` | N0 | a snapshot-capable `raft_bench`, landed before any conversion: `--snapshot-bytes`, a stalled-follower mode, follower side-records and InstallSnapshot counters. The frozen "pre" arm. |
| `a6ba19ee9` | N3-N5 | `rt/src/snapshot.rs`: `SnapshotStore`, one `Arc<Snapshot>` slot with in-place buffer reuse. The store accessors move to SEAM (`snapshot_seam_cpp.cc` on the C++ lanes). On the Rust lane a `SnapshotManager` call in `server.cc` no longer compiles. InstallSnapshot: the leader makes 1 copy instead of 3 (a borrowed writer generated by rpcgen), the follower 2 instead of 3 (a buffer hand-off). The 64 MiB frame cap is enforced. |
| `39950ea3e` | N6 | resend suppression: at most one InstallSnapshot per (term, follower, index) while a reply is outstanding. The key is cleared by the reply, a drop, a failed send, a deadline, or a term change. |
| `a22d391cc` | N7-N8 | startup-recovery lab cases 73-74 on all three lanes. `ci.sh` derives the lab count (27) from the source. raft-rt's `cargo test` gates `raft_lane_check`. |
| `0225bb198` `fffab9338` | N9-N13 | docs; `nm` shows no C++ snapshot classes in Rust-lane binaries; lab x10 per lane (30/30), 8 Raft suites, mixed-lane catch-up in all three mixes; `scripts/raft_perf/rotation_trial.sh` |
| `dd3c0ca9c` | N12 | snapshot-enabled performance, 5 arms rotated, 17 points, 1,650 runs (`docs/performance/raft-rust-snapshot-fffab9338/SUMMARY.md`) |
| `000c41840` `8a5d45db8` | N12 | the pre arm's crash explained; the raw records moved to `~/raft-test-results` |

What N12 showed:
- Store vs pre is within +-0.7% on p50 and +-3% on p99 wherever it
  resolves (64 KiB-16 MiB, and 286 KB unthrottled). RSS is unchanged.
- Catch-up at 16 MiB: 806 -> 280 ms. The leader's p99 during it:
  397 -> 13 ms. Install RPCs per catch-up: 11 -> 2.
- The gates it missed are recorded with their evidence. The main one: at
  60 MiB every build degrades, because `CreateSnapshotLocked` compacts the
  log straight through the new snapshot index, so a lagging follower can
  never catch up. That is core behaviour on all lanes.

## 16. Documents, data and the local merge (09-29)

| commit | what it did |
|---|---|
| `b83630193` | `rust-lane-rpc-path.md`: the Raft project structure in four parts (Rust logic, C kernel, C++ shim, message types), and the Paxos/Raft inheritance under `TxLogServer` |
| `5216f26b3` | local mako-dev's reactor fix R4 (`b73936735`) merged as history only. Its content was already on this branch; a duplicated `test_srpc_timeout_race` block was dropped, and the tree is unchanged. mako-dev is now an ancestor, so merging into it is a fast-forward. |
| `91277b4ac` | the per-entry latency breakdown of the Rust lane (`docs/performance/raft-latency-breakdown/`). Below saturation, three ~1 ms waits make up most of an entry's time; unthrottled, the time is queueing behind stop-and-wait rounds. |

## What the numbers did

| | before | after |
|---|---|---|
| `extern "C"` kernels in `server.cc` | 93 | **54** (939 lines) |
| opaque carriers (Rust names a C++ type) | 13 | **12**, all with pinned size and alignment |
| data members on `class RaftServer` | 51 | **0** |
| out-of-line `RaftServer` methods | 67 (2,527 lines) | **6** |
| Rust share of `server.{h,cc}` authored lines | 33% | **71.6%** |

Counts as of 09-19, on the counting rule in use then.

Do not extend that table forward by hand. The current values -- and the exact
counting rule behind each, which is the part that kept going wrong -- are
GENERATED into `cpp-rust-correspondence.md` by `scripts/gen_correspondence.py`,
and `--check` fails the build when they drift. As of 09-26 it reports:

| | C++ | Rust |
|---|---|---|
| `raft/server.h` | 606 lines | `server_h.rs` 5,020 |
| `raft/server.cc` | 1,882 | `server_cc.rs` 2,774 |
| `raft/service.cc` | 123 | `service.rs` 150 (both exist; the C++ is what srpc dispatches to) |
| `raft/commo.cc` | 325 | `transport.rs` 647 (both exist; the C++ is what runs, bar one rerouted site) |

with 31 exports in `server_exports.h`, 6 more in `transport_exports.h` (included
by no `.cc` yet), and 91 distinct `raft_*` kernels. `class RaftServer` is an
eight-line shim holding a pointer: zero data members, zero out-of-line methods.

## What it cost

**Throughput: nothing.** The full sweep on 09-25/26 -- 624 paired runs, three
trials per point, `412c225a` (C++ Raft) against `538df5f8c` (Rust Raft) -- put
129 of 146 throughput points within noise and none outside the regression
threshold. At every offered rate up to saturation the two arms deliver the same
entries per second.

This supersedes the earlier reading of a ~1.5% throughput regression (22 paired
saturation trials, median -1.71%, sign test p = 0.017, never localised: three
runs disagreed by an order of magnitude and the `noexcept(false)` destructor
hypothesis was refuted at p = 0.688). With 624 runs instead of 22 the effect is
not there.

**Latency: 330 regressions, 0 improvements -- and it is not this conversion.**
The same sweep found p50 up ~45% and p99 up ~110% at typical points, widening
with load. Three binaries measured interleaved on an idle host (4 KB entries,
240/s, three trials each) locate it:

| build | what it is | applied/s | p50 us | p99 us |
|---|---|---|---|---|
| Rust Raft + **old** srpc | Sep 21 tree | 240.0 | 2,711 | 3,659 |
| **new** srpc, before stage 0-3 (`49defdf5e`) | phase 8 only | 239.9 | 3,218 | 6,939 |
| new srpc + stage 0-3 (`538df5f8c`) | phases 9-10 | 239.9 | 3,231 | 6,858 |

The last two agree to +0.4%: phases 9 and 10 cost nothing measurable. The whole
gap sits in phase 8, the subtree pull.

The cause is in `src/srpc/reactor/reactor.rs`. `Reactor::run_loop`'s retain
predicate lost a clause in the merge -- `status != DONE && status != TIMEOUT`
became `status != DONE` -- and with it the comment explaining that retaining a
timed-out event "leaks one Arc per timed wait and makes every subsequent
reactor pass rescan all past timeouts". Three observations match: latency grows
with run length (head p50 3,059 us at 4 s -> 3,231 at 8 s -> 4,818 at 20 s,
while the old runtime is flat at 2,711 -> 2,717), the poll loop body is 2.3x
slower (349 vs 154 us p50 under strace) while the socket path is not, and
throughput is untouched. Raft performs one timed wait per heartbeat round per
replica, so the leaked set grows at the round rate.

Written up with the evidence in `docs/performance/raft-latency-regression.md`.
The fix is two lines, but it is a subtree file shared with Paxos and Mako and
it regenerates srpc's C++ lane, so it is not yet claimed. (It was later made in
`edc5db890`, phase 12: p50 back to 2.70-2.75 ms.)

Correctness held throughout: every commit passed RaftLabTest 25/25, and
every performance run reported zero gaps, zero duplicates and zero
out-of-order applications on both arms.

## What it found

Bugs that existed before the conversion and were found BY it, because Rust
forces a question C++ does not ask:

- `4427129a9` a `next_index_` cursor held across an InstallSnapshot suspension
- `39890ade4` commit-index defects in both heartbeat phases
- `d295a4842` committed log entries mutated at send time
- `3774ba937` the locale id sent where the global site id was meant
- `be3e24a4b` three DSL functions taking one `&mut` overlapping two `&` of the same object -- which compiled only because the caller was C++
- `bfb141422` a performance regression that only a measured conversion would have caught
- `2a0deb21f` a split-brain in my own `broadcast_vote` -- quorum derived from
  the reachable peers, so a partitioned candidate would elect itself. Caught by
  the round-trip test written in the same commit, not by review
- `5d3591c95` the Rust `RaftCommo` had silently dropped `partition_peers_`, so
  its `peers_except()` had no partition dimension at all
- `ad19d6cc7` srpc's nine C kernels compiled twice by two different compilers,
  both copies in one binary, over a source that branches on `__clang__`

- `2cd7f5e74` a transpiled lab that reported 25/25 while checking nothing:
  its assertion macros were lowered to `// TODO` comments (189 dropped checks)
- `c23e397e4` two silent transpiler miscompiles: an untyped `Vec::new()` as
  `Vec<bool>`, and `[..].into_iter().max()` returning a reference into a dead
  iterator
- `000c41840` a use-after-free in the N0 Rust lane's InstallSnapshot send. The
  reply callback `move |t| deliver(owned.0 ...)` captured only the `usize`
  field under Rust 2021's disjoint capture, so the context's guard dropped
  when the send returned. It segfaulted the leader in 13 of 370 N12 runs;
  N5's `move |t| ctx.deliver(t)` moves the whole value

And one found by auditing the prose rather than the code: of 113 claims across
the two migration documents, 25 were correct (`edd49ff35`).
