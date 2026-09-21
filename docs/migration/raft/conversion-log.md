# The Raft C++-to-Rust conversion, commit by commit

88 commits, `65205328c`..`37d308ed4`, 2026-09-12 to 2026-09-19. Every one of
them built, passed RaftLabTest 25/25, and passed `scripts/raft_dsl.sh
--check` before it was pushed.

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

`docs/migration/raft/plan.md` was written, and its steps A through F1
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

## What the numbers did

| | before | after |
|---|---|---|
| `extern "C"` kernels in `server.cc` | 93 | **54** (939 lines) |
| opaque carriers (Rust names a C++ type) | 13 | **12**, all with pinned size and alignment |
| data members on `class RaftServer` | 51 | **0** |
| out-of-line `RaftServer` methods | 67 (2,527 lines) | **6** |
| Rust share of `server.{h,cc}` authored lines | 33% | **71.6%** |

Counts as of 09-19. `plan.md` carries the current measured values and the
method behind each.

## What it cost

A ~1.5% throughput regression against the pre-conversion baseline, measured
over 22 paired saturation trials: median -1.71%, 5/22 favour the converted
tree, exact sign test p = 0.017.

It is not localised. Three independent runs disagree on its size by an order
of magnitude (-0.22%, -0.69%, -2.32%), a bisect put the midpoint of the
conversion window at -1.20% -- so the cost accumulates across tranches
rather than arriving in one -- and one specific hypothesis (the lock guards'
`noexcept(false)` destructors) was tested and refuted at p = 0.688.

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
