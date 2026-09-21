# Raft in Rust: the plan

The goal, stated once so every step below can be judged against it:

> `RaftServerBase` is a Rust struct that owns Raft's memory. `TxLogServer`
> (what any replication engine offers the worker) and `RaftSpecific:
> TxLogServer` (what only Raft offers) are Rust traits implemented for it.
> Workers drive replication through `dyn TxLogServer`. The struct and the
> trait impls are compiled by rustc and linked -- not transpiled to C++.
>
> Out of scope, by decision: memory whose layout C++ describes (the
> `Marshallable` wire hierarchy, rpcgen output), the transport syscalls, and
> the reactor's fibers and poll threads. C++ keeps those.

Everything in this file was measured in the tree at `19cfbb213`. Re-measure
before trusting a number; the commands are given.

## Where the tree is

| | value | how measured |
|---|---|---|
| Rust machine code linked into the Raft binaries | **none** | `nm build_rafttest/deptran_server \| grep -cE ' _R[A-Za-z0-9_]{8,}$'` = 0 |
| Rust source that already compiles under rustc | 7,157 lines | `wc -l src/deptran/raft/src/*.rs`; the gate builds it |
| Rust share of `server.{h,cc}` authored lines | 71.6% | `conversion-log.md` |
| data members on `class RaftServer` (the C++ shim) | 0 | it is 361 lines of forwarding, one ctor/dtor, four RPC entry points |
| hand-written `raft_*` kernels in `server.cc` | **50**, 529 lines | column-0 definitions outside GEN/RUST regions |
| ... of which reach into `RaftServerBase`'s fields | **24** | body contains `self->` / `server->` (0 after step C; `scripts/raft_field_census.py`) |
| ... of which downcast `RaftServerBase*` to `RaftServer*` | **12** | `static_cast<RaftServer*>(self)` |
| ... of which pass values only | 21 | neither |
| worker-side `dynamic_cast<RaftServer*>` | **6** | `raft_main_helper.cc`, `server_worker.cc`, `frame.cc`, `raft_worker.{h,cc}` |
| opaque carriers (Rust names a C++ type by size only) | 11 | `rusty_opaque_cpp_carrier!` in `src/rrr/rusty-rustc/src/lib.rs`, pinned by `static_assert` in `server.h` |

The one in-tree proof that real Rust can link into this program is
`third-party/mako-redis`: `crate-type = ["staticlib"]`, four `extern "C"`
functions, 3,595 mangled Rust symbols in `build/makoCon`.

## Where the tree is after the cutover (`1de45affa`)

| | value | how measured |
|---|---|---|
| Rust machine code linked into the Raft binaries | **214 v0 symbols of the `raft` crate**, 74 `extern "C"` exports | `nm build/dbtest \| grep -c '_RN[a-zA-Z0-9_]*raft'`; `nm --defined-only build/dbtest \| grep -c '^raft_server_'` (on the third column) |
| C++-mangled `RaftServerBase` methods in the binary | **0** (87 Rust ones) | `nm --defined-only build/dbtest \| grep -c '_ZN5janus14RaftServerBase'` |
| canonical Rust source of the server | 7,842 lines | `wc -l src/deptran/raft/src/server_h.rs src/deptran/raft/src/server_cc.rs src/deptran/raft/src/server_pods_h.rs` |
| `server.h` / `server.cc` | 673 / 1,474 lines (from ~8,600 / ~6,700) | `wc -l`; server.cc is the kernels, server.h the aliases, layout pins, one POD block and the shim |
| hand-written `raft_*` kernels the crate calls | **97**, every one defined by a C++ object | `nm -u <build>/raft-cargo/release/libraft.a \| grep '^raft_'` against `nm --defined-only` of the objects |
| inline DSL blocks left in the two carriers | 1 (`raft_server.kernel_result_pods`) | `grep -c '#if RUSTYCPP_RUST' src/deptran/raft/server.h src/deptran/raft/server.cc` |

## The fact the plan rests on

`rusty::Vec` is 48 bytes in C++ and `std::vec::Vec` is 24 in Rust, and
`rusty::Mutex`, `VecDeque`, `Condvar` and `Arc` differ too. All five sit in
`RaftServerBase`. That looks like the blocker to compiling the struct with
rustc: C++ and Rust would disagree on where every field is.

It only matters because **24 kernels look inside the struct**. If no C++
reads or writes a field of `RaftServerBase`, its layout is private, Rust can
use the real `std` types, and the mismatch is not a fact about anything.
The blocker is the width of the boundary, and the boundary is ours.

So the plan is not "translate more bodies". The bodies are mostly translated.
The plan is to narrow the boundary until the struct is opaque, then cut over.

## Constraints that shape the steps

Transpiler rules, verified against the pinned `77c3ad5a` source. They are
why the steps have the shape they do; fight them and the failure is silent.

- **One base per struct.** `#[cpp_inherit] impl Trait for X` records the base
  in a `HashMap<String,String>` with `.insert()`; a second impl overwrites the
  first with no diagnostic. A struct gets exactly one C++ base this way -- the
  LAST `#[cpp_inherit]` impl in the block. `server.h` pins the outcome with
  `static_assert(std::is_base_of_v<RaftSpecific, RaftServerBase>)`.
- **A trait impl WITHOUT `#[cpp_inherit]` lowers through `TraitAdapter<Self>`.**
  It emits three adapter specializations, one of which holds `Self` by value
  and moves it; for a move-only struct with mutexes that is a hard compile
  error in every including TU (`call to implicitly-deleted copy constructor`).
  So every trait impl on `RaftServerBase` carries `#[cpp_inherit]`, even the
  one that does not own the base clause -- the attribute suppresses the
  adapters per impl (`collect_passes.rs:7780`) independently of the slot.
- **`override` is emitted only for a trait declared in the same block.** The
  two traits live in `scheduler.h`, so none of `RaftServerBase`'s sixteen
  interface methods carries the keyword; they override implicitly. A signature
  drift therefore surfaces not at the method but as `RaftServer` turning
  abstract at its `new` -- still a compile error, just a less pointed one.
- **Supertraits emit inheritance only within one carrier.** `pub trait B: A`
  becomes `class B : public A` only if `A` is declared in the same file's
  DSL blocks; otherwise the base is dropped silently.
- **`#[cpp_inherit]` needs `use rusty::cpp_inherit;` in the same block.**
  Without it the struct is emitted with no base and no error.
- **Every `rusty::` name a DSL block uses must resolve under rustc.** The
  transpiler maps `rusty::ReactorIntEvent`, `rusty::Communicator` and the
  like to C++ types; rustc knows nothing of them. `src/rrr/rusty-rustc`
  (the facade crate) and `src/deptran/raft/rust_facade_types.h` declare the
  two halves of each such name, and both must be kept in step -- the
  extracted crate dies at E0433 before the emitter is consulted otherwise.
- **Orphan impls are stubbed.** An `impl` is honoured only when its
  `pub struct` is in the same block. The unit of conversion is a type.
- **No implementation inheritance.** A DSL struct cannot absorb a C++ base's
  data members; that is why `TxLogServer` became a pure interface.
- **`server.h` / `server.cc` are canonical; `src/*.rs` are extracted.**
  Edit the DSL block in the carrier, then `scripts/raft_dsl.sh --rewrite
  <carrier>`; the gate (`--check`) fails on drift either way.
- **`rusty::panic::catch_unwind` is `try { } catch (...)`.** In transpiled
  C++ it catches every C++ throw; under rustc it catches only panics. Code
  that relies on the first meaning breaks at cutover.

## The steps

Each step exists to make the next one possible. Each has a mechanical
"done" test, because the failure modes above are silent.

### A. Declare the trait pair in one carrier

**Goal:** a typed Rust interface that Raft-specific behaviour can be called
through.

`pub trait TxLogServer` already lives in `src/deptran/scheduler.h`. Add
`pub trait RaftSpecific: TxLogServer` to the **same** carrier (the
same-carrier rule), and in `server.h` replace the shim's inheritance with
`#[cpp_inherit] impl RaftSpecific for RaftServerBase`. `RaftSpecific` holds
what the six `dynamic_cast` sites and the twelve `static_cast` kernels
reach for today: `commo()`, `SetLocalAppend`, `InitializeSnapshotManager`,
`GetSnapshotManager`, `LoadStateMachineSnapshotLocked`,
`PrepareStateMachineSnapshotLocked`, and the RPC entry points.

**Makes possible:** step B. Without a typed interface there is nothing to
route the downcasts through.

**Done when:** the GEN region of `scheduler.h` contains
`class RaftSpecific : public TxLogServer`, `RaftServerBase`'s GEN region
names `RaftSpecific` as its base, `grep -c 'Adapter[A-Za-z]*<RaftServerBase>'
src/deptran/raft/server.h` is 0, and the two `static_assert(is_base_of_v<..>)`
after the struct compile.

**Cost:** one carrier edit, one regeneration. The risk is entirely the
silent-failure list above -- and the adapter hazard in that list was found
by this step failing the build, not by reading.

### B. Delete the downcasts

**Goal:** nothing recovers the concrete C++ type from a base pointer.

Two populations. The **6** worker-side `dynamic_cast<RaftServer*>` sites
become calls through `RaftSpecific` (or through `TxLogServer`, where that is
all the caller needs). The **12** kernel-side `static_cast<RaftServer*>(self)`
sites -- `raft_broadcast_vote_and_wait`, `raft_commo_set_network_enabled`,
`raft_set_local_append`, `raft_load_state_machine_snapshot`,
`raft_install_snapshot_payload`, `raft_bind_replication_poll`,
`raft_initialize_snapshot_manager`, `raft_spawn_election_timer_fiber`,
`raft_append_leader_noop`, `raft_phase1_load_and_send_snapshot`,
`raft_phase1_send_append`, `raft_ae_apply_incoming` -- reach `commo()` and
the snapshot manager. They get a host handle installed at construction
instead: a small `#[repr(C)]` table of function pointers (send, snapshot
manager access, embedder callbacks) that the C++ shim fills in.

**Makes possible:** `class RaftServer` becomes a pure vtable adapter with no
identity anything depends on, so it can be frozen or deleted at step F. Also
the workers now talk to replication through an interface, which is what
makes a second engine behind `TxLogServer` possible.

**Done when:**
`grep -rn 'dynamic_cast<RaftServer\|static_cast<RaftServer\*>' src/deptran/ | grep -v '/raft/src/' | grep -vc LabAccess`
is 0. (`RaftServer::LabAccess` is the RAFT_TEST harness's window into state
and is handled in step C.)

### C. Make the struct opaque

**Goal:** no C++ outside `RaftServerBase`'s own generated region names a
field of it.

The **24** kernels that read or write `self->...` each become one of:
a call that receives the values it needs and returns a result (most of the
marshalling and network group); a method on the struct that does the field
access in Rust and hands the kernel only what crosses (the snapshot and
apply groups); or, for the reactor group, a kernel that receives an
`IntEvent`/fiber handle and never sees the struct at all.

`RaftServer::LabAccess` -- ~50 uses in `test.cc`/`testconf.cc` -- becomes
read-only `Lab*` getters on the struct (done in C2; the DSL has no cfg, so
they are emitted unconditionally, as inline reads).

`mtx_` is still a mutex *next to* the state, not `Mutex<RaftConsensusState>`
owning it. Sized after C3: 34 lock sites, ~415 `state_.` reads and ~71
writes across the two Rust modules; every `*Locked` method would take the
guard's `&mut RaftConsensusState` instead of `&mut self`, and the struct
would split into state-under-lock and everything else. The payoff --
borrow-checked critical sections -- only materialises under real rustc, and
the C++ `RaftCheckedMutex` abort-on-re-entry diagnostic would have to be
re-provided. **Recommendation: do it as part of F, not before.** Not
required for C's done-test, which is about layout, not lock ownership.

**Makes possible:** step F. This is the hinge. After C, `rusty::Vec`'s
layout is irrelevant, because nothing outside agrees on anything.

**Done when:** `python3 scripts/raft_field_census.py` exits 0. It strips
GEN/RUST regions, comments and string literals from every C++ file under
`src/deptran`, takes the field list from the `pub struct RaftServerBase` and
`pub struct RaftConsensusState` blocks, and reports every site that names one
through a Raft-server receiver (`self`, `server`, `rep_sched_`, `frame->svr_`,
implicit `this` inside the server's own carriers, ...). **Done in C1a/C1b/C2/
C3; see Progress.**

**You can stop here.** After C the design in the goal statement exists in
the DSL: struct owns memory, traits define the interface, no downcasts, one
lock owning the state. D-F only change *what compiles it*. If a permanent
hand-maintained FFI boundary is not worth genuine rustc borrow checking,
A-C is the better version of what the tree already has, and D-F are not
started.

### D. Narrow the surviving signatures to FFI-legal types

**Goal:** every function that crosses the boundary in either direction has
a signature `extern "C"` can carry.

What "can carry" means, decided after inventorying the boundary at C3 (75
kernel declarations, 19 trait methods, 8 server methods C++ calls plus the
wake gate's):

- **Scalars, `#[repr(C)]` value structs, pointers to opaque C++ objects**
  pass as they are. `RaftElectionTimeouts`, `RaftVoteOutcome`,
  `AppendRespView` are already `#[repr(C)]`.
- **Non-trivial C++ objects never cross by value.** A `janus::Command`, a
  `shared_ptr`, a `rusty::Arc` returned by value is an ABI mismatch under
  rustc: C++ returns a non-trivial type through a hidden pointer, Rust
  would expect a `[u8; N]` in registers. Every such return becomes an
  **out-parameter the C++ side fills in place**: `raft_command_clone_into
  (src, dst)`, `raft_new_callback_lifetime(self, out)`, and so on. 13 of
  them at C3: four `Command`s, three `shared_ptr`s, three `Arc`s (one of
  them into a Rust `Vec`), two `std::function` trait parameters.
- **The carriers stay inline and are declared relocatable.** `RaftCommand`
  (24 bytes) is a field of every log entry; boxing it would cost a heap
  allocation per entry on the hottest path. Rust moves it by `memcpy`,
  which is sound for `std::shared_ptr` and `rusty::Arc` on this platform
  (no interior pointers) and is the assumption F must state once, next to
  the layout pins. Construction, copy and destruction are kernels; under
  rustc the carrier gets a `Drop` that calls the destroy kernel.
- **Rust never `.clone()`s a carrier.** Ten sites did at C3 (three
  `RaftCommand`, seven `Arc<IntEvent>` waiters). Under the transpiler that
  is the C++ copy constructor; under rustc it is a bitwise copy, i.e. a
  refcount bug. Each becomes a kernel clone.
- **Rust-native objects handed to C++** (`Arc<ReplicationWakeGate>` into a
  poll-thread job, the `IntEvent` waiters) are the fiber-wait plumbing and
  are settled by E, not D.
- **`std::function` trait parameters** (`RegisterLeaderChangeCallback`,
  `reg_learner_action`) become `extern "C" fn(*mut c_void, ...)` plus a
  context pointer, or a boxed handle the worker allocates; this touches the
  workers and is D2. **References in trait signatures** (`&RaftCommand`,
  `&RaftByteString`) are `const T*` in C; the change is mechanical and
  belongs to the cutover.

**Makes possible:** the *traits* surviving the cutover, not just the struct.
A trait object is a vtable; an FFI table is a vtable. Same shape, so D is
retyping, not restructuring.

**Done when:** no kernel returns or takes a non-trivial C++ object by value,
no Rust body calls `.clone()` on an opaque carrier, and every `rusty::`
type in a crossing signature is a scalar, a `#[repr(C)]` struct, a
pointer, or an inline carrier with a pinned layout -- checked by a census
over the extern blocks, like step C's.

### E. Decide who owns each suspension point -- DECIDED

**Goal:** an explicit, recorded decision for every place a Rust-authored
body yields the fiber, so the cutover has no surprise.

Surveyed after D1: five yield points reachable from Rust-authored bodies --
`ReplicationWakeGate::finish_wait_for_work` and `::wait_for_election_timeout`
(`IntEvent::wait_timeout`), heartbeat phase 2's response-collection poll
(`raft_fiber_sleep_us` in a loop), `PrepareForShutdown`'s barrier yield, and
`ApplyThreadLoop`'s 1 ms sleep, which is an OS thread, not a fiber -- inside
two fiber loops, `HeartbeatDriver::run` and `ElectionTimerLoop::run`.

**Decision: option 2. Rust stays on the fiber; C++ keeps the scheduling.**
Option 1 (C++ owns every wait) would have turned phase 2's poll into a
resumable state machine for no gain but timing risk. The three facts that
make a Rust frame on a fiber stack sound are written at the spawn kernels in
`server.cc` (`raft_spawn_election_timer`) and enforced where they can be:

1. A fiber is a stack switch on one OS thread; each site has one PollThread,
   so a suspended frame resumes where it left. `thread_local!` is stable.
2. No unwind may cross the assembly switch: `raft_catch` catches on the C++
   side, and the raft crate builds with **`panic = "abort"`** (`Cargo.toml`)
   -- the one place rustc enforces it.
3. The stack budget is rrr's `kDefaultStackBytes` (1 MiB) with a
   **`PROT_NONE` guard page** below it (`srpc_fiber.c:46`) -- an overflow
   faults at once rather than corrupting. (The first draft of this plan
   assumed no guard page; the code has one.)

What this leaves for the cutover: `raft_create_int_event` still returns an
`Arc<IntEvent>` by value and the gate clones its waiters seven times in Rust;
under rustc those become the same out-parameter/kernel-clone shape D1 gave
everything else, and the `Arc<ReplicationWakeGate>` the wake job captures
becomes an `Arc::into_raw` handle with a Rust-exported wake entry point.

**Done when:** the constraints are stated at the spawn kernels, the crate
profile aborts on panic, and the plan records the decision -- all three
in the E commit.

### F. Cut over -- done: F1 (`3cbcdcfe6`), F2.1-F2.3, and the cutover F2.6 (`1de45affa`)

**F1 (done: F1a `1d24bbf53`, F1b `782b34f48`, F1c `3cbcdcfe6`): the seam is a C ABI
while everything is still transpiled C++.** `server_exports.h` declares 71
`extern "C"` functions -- lifetime, the two fiber loops, the 19 interface
methods, the 8 methods kernels call back, the 40 lab-harness methods --
and all 71 are defined in Rust in server.cc's DSL block. `class RaftServer`
holds a `RaftServerBase*` and forwards. Hand-written C++ knows the struct
only as a pointer type: it derives from nothing of it, names no field, calls
no method, and reaches it through the header alone. The generator
(`scripts/raft_gen_exports.py`) produces the exports, the header and the
shim from one signature table, so they cannot drift apart silently.

**F2 is what defines those 71 symbols: the crate, built as a `staticlib`,
with every Rust-owned type's C++ definition deleted from the headers.** It is
a project of its own. The inventory, from every `rusty::` path the raft
crate uses (`grep` over `src/deptran/raft/src/*.rs` against the facade):

| what the crate uses | today (verification facade) | F2 must make it |
|---|---|---|
| `Option`, `Vec`, `VecDeque`, atomics, `Mutex`, `Condvar`, `BTreeSet/Map`, `sync::Arc` of Rust types | `std` re-exports / wrappers | unchanged -- they are already real |
| 15 opaque carriers and models: `RaftCommand` (64 uses), the seven pointer carriers, three `std::function`s + `LearnerAction`, `RaftCheckedMutex`, `RaftStdMutex`, `RaftStdThread` | pinned byte arrays, `Default`, no `Drop` | `impl Drop` calling a destroy kernel each (~13 kernels), no `Clone` (D1 already routed clones through kernels), construction only from kernels; the thread carrier must be joined before drop **Done: F2.1.** |
| `sync::Arc<ReactorIntEvent>`, `sync::Arc<ReactorPollThread>` (14 uses, all in the wake gate) | `std::sync::Arc` over an opaque type | opaque carriers with kernel clone/drop -- the C++ side owns a `rusty::Arc`, not a `std::sync::Arc` (E's leftover) **Done: F2.1.** |
| 33 `raft_log_<level>_<arity>` functions (~200 call sites) | facade stubs | one `raft_log_line(level, ptr, len)` kernel and Rust `format!` -- the format strings are `{}`-style already; a macro makes the rewrite mechanical **Done: F2.1c** (44 functions, `RaftLogArg` + `raft_log_format` in the facade, two kernels). |
| `ReplicationWakeGate` methods called from C++ (`QueueReplicationWake`: `reserve_wake_owner`, `wake_on_owner`, ...) | emitted C++ class | exports over an opaque gate pointer; the wake job's `Arc` clone becomes `Arc::into_raw` / `from_raw` **Done: F2.2** (a `Box<GateWakeJob>` token and one export, `raft_wake_job_run`). |
| Rust `pub const fn` helpers the kernels call (`raft_server_append_command_is_batch`, `raft_server_command_is_internal_noop`, `raft_server_append_sent_end`, ...) | emitted inline C++ | exports, or the kernels receive the booleans from Rust |
| `RaftLogFingerprint(const RaftLog&)` in test.cc | reads the emitted log class | one `raft_log_fingerprint(*const RaftLog)` export in Rust **Done: F2.3** (two scalar lab exports). |
| the two `std::function` trait parameters (D2) | by value | `extern "C" fn + ctx`, touching both workers and Paxos **Done: F2.3** for the seam: by pointer + clone kernel; the interface types themselves are unchanged. |
| `mtx_` next to the state | `RaftCheckedMutex` carrier + guard kernels | optionally `Mutex<RaftConsensusState>` (sized under C; ~415 reads, ~71 writes) |

Plus the build: a cargo `staticlib` target for `src/deptran/raft` with the
runtime facade, linked by CMake the way `third-party/mako-redis` is; the
kernels already have C linkage on both sides. And the deletion: every GEN
region of a Rust-owned type in `server.h`/`server.cc` goes, `struct
RaftServerBase;` becomes a forward declaration, and the layout pins and
`static_assert(is_base_of...)` lines go with them.

**Done when:** `nm` on the Raft binaries shows the Rust v0 symbols for the
71 exports; no GEN region defines a Rust-owned type; and the verification
below passes on the rustc build.

**Done (`1de45affa`).** `nm build/dbtest`: 214 v0-mangled `raft` crate
symbols, 74 `raft_server_*` exports, 0 C++-mangled `RaftServerBase` methods
(87 Rust ones), 14 symbols of the pointer-holding shim. No GEN region of a
Rust-owned type exists: `server.h` has one inline block (the kernel-result
PODs) and `server.cc` none; `src/deptran/raft/src/server_h.rs` and
`server_cc.rs` are canonical Rust compiled by cargo into `libraft.a`.
RaftLabTest 25/25 and the four production suites pass on the rustc-compiled
server. The paired throughput trial against `19cfbb213` is recorded in the
Performance section below.

**F2.6, the cutover, as the probe measured it.** A scratch copy with slices 2-5
applied, every generated region of server.h/server.cc deleted and six TUs
syntax-checked leaves the compiler wanting exactly: the three kernel-result
PODs (now their own C++-visible block), the two RPC bodies (now exports), the
`is_base_of` pins and the `ElectionCompletionAction` layout asserts (both about
the emitted C++, deleted with it), and the alloc/free kernels (Rust's
`Box::into_raw` / `Box::from_raw` in `raft_server_new` / `raft_server_delete`).
Of the 49 DSL types, 25 are Rust-only; the rest are the shared interface
(`RaftStartResult` is `#[repr(i32)]`), C++-only wire and enum types Rust never
names outside their own modules, or `#[repr(C)]` PODs -- no type is laid out
by both worlds. The 73 `rusty::` paths the crate uses all resolve to real
facade code (none is a stub). So the cutover is: (1) the Rust-only blocks of
server.h/server.cc become canonical `.rs` sources -- the extracted mirrors are
already that text -- and leave the carriers, whose remaining C++ is the
aliases and layout pins, the kernels, the shim and the one POD block;
(2) CMake builds `src/deptran/raft` as a `staticlib` with its own
`--target-dir` and links it wherever the Raft kernels link; (3) the facade's
C++ halves (`rust_log_shims.h`, `raft_construct_by`, the factory mirrors) go,
the alias header stays for the kernels; (4) the gate's block inventory and the
crate manifest learn the canonical modules. Done when RaftLabTest and the four
suites pass on the rustc-compiled Raft and `nm` shows the Rust symbols.

## What stays C++

Named, so no step is judged against them.

- `src/rrr/base/srpc_connect.c` and the socket syscalls beneath the reactor.
- `rcc_rpc.h` (rpcgen output) and the `Marshallable` / `SerializableEnvelope`
  hierarchy that `janus::Command` is built from -- shared with Paxos, and the
  layout C++ describes.
- The fiber scheduler, `fiber_context_x86_64.S`, the poll threads, the apply
  thread's `std::thread` spawn.
- The `TxLogServer` vtable shim: three forwarders.
- Invoking the embedder's `std::function` callbacks (`app_next_`, the
  snapshot callbacks) and the one `catch` around each such call. The call
  is C++; only the call can catch what it throws.
- The snapshot manager's rocksdb calls.

## Verification, at every step

Correctness and performance are constraints, not goals; each step ends with
all of these green, not some.

- `bash scripts/raft_dsl.sh --check` -- transpiler pin, drift, clippy under
  `-D warnings`, the rustc build of the crate.
- `./ci/ci.sh raftLabTest` -- the 25-case cluster suite (its own build dir,
  `-DMAKO_USE_RAFT=ON -DRAFT_TEST=ON`).
- The four production Raft suites: `shard1ReplicationRaft`,
  `shard2ReplicationRaft`, `shard1ReplicationSimpleRaft`,
  `shard2ReplicationSimpleRaft`. Last three runs: 153,019 / 150,382 /
  153,381 ops/s.
- Paired throughput trials against the step's parent commit --
  `scripts/raft_paired_trial.sh <treeA> <treeB> <pairs> <out.csv>` alternates
  the single-shard Raft suite between two built trees (ABBA order) and
  `scripts/raft_paired_trial.py` gives the median delta and the exact sign
  test. The tree already carries a ~1.5% cost (22 paired trials, median
  -1.71%, p = 0.017) that no single commit owns; a step that widens it
  measurably is not done. Single runs of the suite spread 13% on identical
  code (measured after C3), so a single number is not evidence either way.
  Step F in particular needs 50+ pairs on a quiet host: the estimate for
  the FFI crossings it adds is ~0.1% (about 450k `Command` clone/drop
  crossings per second at 153k ops/s with two followers, ~2 ns each) and an
  estimate is not evidence.

## Progress

| step | commit | measured after |
|---|---|---|
| A | `c83a34bc0` | `RaftServerBase : public RaftSpecific : public TxLogServer`; 16 interface methods on the struct, 13 of them moved out of inherent impls or off the shim; shim `class RaftServer` 361 -> 255 lines; `Start`/`OnRequestVote`/`OnAppendEntries`/`OnInstallSnapshot` deleted from the shim and from `server.cc` (-80 hand-written lines, +61 for the three `raft_rpc_*` kernels); kernels 50 -> 53 (569 lines), of which 25 reach into the struct, 12 downcast; worker `dynamic_cast`s still 6 (step B). RaftLabTest and the production suite results are in the commit message. |
| B | `738a8b7f2` | `grep -rn 'dynamic_cast<RaftServer\|static_cast<RaftServer\*>' src/deptran` (excluding `LabAccess`) is **0** -- was 6 + 12. `RaftFrame::CreateRaftScheduler()` gives both workers a typed pointer; `RaftWorker` holds `RaftSpecific* raft_sched_`, `RaftServiceImpl` holds `RaftSpecific*`, the main helper reads `SiteId()/PartitionId()/CommitIndex()` off the trait instead of fields. `class RaftServer` 255 -> 41 lines: ctor, dtor, `LabAccess`, and a test-only `GetSnapshotManager`; `server.cc` defines nothing of it but ctor and dtor. The shim's four out-of-line bodies became kernel bodies (`raft_set_local_append`, `raft_load_state_machine_snapshot`, `raft_initialize_snapshot_manager`, and the file-local `prepare_state_machine_snapshot_locked`), `commo()` became the file-local `commo_of(self)`; kernels 53 (592 lines), 28 reaching into the struct -- the moved bodies read fields, which is exactly what step C removes. One `dynamic_cast` remains in the Raft worker, `Frame* -> RaftFrame*`, at the generic frame registry; it is a frame cast, not a server cast, and it now `verify`s instead of silently skipping. Test results in the commit message. |
| C1a | `eb5482070` | Kernels that name a field of the struct: **26 -> 7**, and the seven left are exactly the container group (`raft_log_`, `decoded_terms_`, `config_members_`, `batch_buffer_`) that C1b moves into Rust. The other nineteen now take what they need: scalars by value (`site_id`, `loc_id`, `term`, `partition_id`), opaque carriers by pointer (`*const RaftLeaderChangeCb`, `*const RaftSnapshotManagerPtr`, `*mut RaftStdThread`, `*const LearnerAction`, `*const Arc<ReplicationWakeGate>`, ...), the communicator as the `Communicator*` field value (`commo_of(commo)`). `OnInstallSnapshot` takes its two locks in Rust (`RaftStdLockGuard` then `RaftLockGuard`, the C++ order) and only the catch stays C++ (`raft_install_snapshot_guarded`). Seventeen kernels still receive `RaftServerBase*`, for METHOD calls only (`SetupInternal`, `StartElectionTimer`, `ApplyThreadLoop`, `OnInstallSnapshotLocked`, ...); a method is the interface, not the layout. Kernels 53 (600 lines). `AsyncCallbackLifetime::server` is `RaftServerBase*`. Test results in the commit message. |
| C1b | `90b6f828a` | **Kernels that name a field of the struct: 7 -> 0.** The log's writers are Rust: `AppendLocal` (was `SetLocalAppend`/`raft_set_local_append`), `AppendLeaderNoop`, `AeApplyIncoming`; so are `AeDecodePayload` (fills `decoded_terms_`), `LoadCurrentConfig` (fills `config_members_`) and the leader's batch loop (pushes `batch_buffer_`). What the old kernels could not do is the whole of what the new ones do: copy a `janus::Command` (`raft_command_clone`, `raft_wire_command_clone`, `raft_batch_command_at`), look inside a `TpcBatchCommand` (`raft_wire_is_batch`, `raft_wire_batch` -- the one `marshallable_cast`, made once per payload as before -- `raft_batch_len`, `raft_batch_term_at`), stamp and batch a `TpcCommitCommand` (`raft_command_is_tpc_commit`, `raft_stamped_commit`, `raft_batch_finalize` over `(ptr, len)` of the Rust Vec), read the yaml config (`raft_config_replica_count/site`), and the `RAFT_TEST_CORO` predicate for the no-op (`raft_leader_noop_enabled`, `raft_noop_command`). Kernels 53 -> 60 but 600 -> 545 lines: 50 value-only, 10 take `RaftServerBase*` for method calls only. Rust owns every container it declares. Test results in the commit message. |
| C2 | `bcc298560` | The RaftLab suite holds the layout of nothing: `RaftServer::LabAccess` (11 accessors, 45 uses) and `test.cc`'s 70-odd direct reads (`server->state_.raft_log_.last_index()`, `->state_.commit_index_`, 24 `std::lock_guard(server->mtx_)`, ...) all became `Lab*` getters on `RaftServerBase` -- `LabMutex()`, `LabApplyMutex()`, `LabCommitIndex()`, `LabLastLogIndex()`, `LabLog()` for the fingerprint, and so on -- read-only, inline, emitted unconditionally because the DSL has no cfg. The RAFT_TEST `ServerWorker` uses `set_site_identity`/`set_commo`/`SiteId()` instead of poking three fields and `commo_`. `class RaftServer` is ctor + dtor. Receiver-aware census over every C++ file under `src/deptran` (comments and string literals stripped): **0 sites name a field of `RaftServerBase` or `RaftConsensusState`** through a server pointer; the two remaining hits are `RaftFrame::commo_` and `SiteInfo::partition_id_`, other objects' same-named fields. The shim constructor still sets two fields by implicit `this` (C3). Test results in the commit message. |
| C3 | `2996cc572` | The shim constructor is `RaftServer::RaftServer() { ConstructRuntime(); }`: the two members the generated constructor could not initialise (`async_callback_lifetime_`, a `std::make_shared` whose payload points back at the object; `heartbeat_interval_us_`, a RAFT_TEST-dependent macro) come from `raft_new_callback_lifetime` and `raft_heartbeat_interval_default`, the legacy payload registration from `raft_ensure_legacy_payload_registered`, and the lab's initial role from `raft_lab_mode()` -- the RAFT_TEST_CORO predicate the no-op already used (renamed from `raft_leader_noop_enabled`). **Step C is done: `python3 scripts/raft_field_census.py` exits 0** -- no hand-written C++ under `src/deptran` names a field of `RaftServerBase` or `RaftConsensusState` through the server (comments, string literals, GEN and RUST regions stripped; `this` counts only inside the server's own carriers; other objects' same-named fields are listed, not counted). `class RaftServer` is ctor + dtor; its layout is private to the struct's generated region. Test results in the commit message. |
| D1 | `204751587` | **No kernel returns a non-trivial C++ object by value, and no Rust body clones a `Command` carrier.** The four `Command` returns became in-place `_into` kernels (`raft_command_clone_into`, `raft_noop_command_into`, `raft_batch_command_into`, `raft_wire_command_clone_into`); the three `shared_ptr` returns (`raft_new_callback_lifetime`, `raft_broadcast_vote_and_wait`, `raft_phase1_send_append`) and the batch `Arc` (`raft_stamped_commit_into`) take an out-parameter Rust default-constructs; the three `entry.cmd().clone()` sites call the clone kernel. The batch buffer's element is a new pinned 8-byte carrier `RaftTpcCommitPtr` (`rusty::Arc<TpcCommitCommand>` is one control-block pointer), so `raft_batch_finalize` iterates `(ptr, len)` over carriers of known size. Left for E, deliberately: `raft_create_int_event -> Arc<IntEvent>` and the seven waiter clones inside `ReplicationWakeGate`, which are the fiber-wait plumbing. Left for D2: the two `std::function` trait parameters. Behaviour-neutral by construction (same C++ runs, moved from a return to an out-slot); tests in the commit message. |
| E | `raft: step E` | **Decided: Rust stays on the fiber, C++ keeps the scheduling.** Five yield points surveyed (two wake-gate waits, phase 2's response poll, the shutdown barrier yield, and the apply thread's OS sleep); option 1 would have made phase 2 a resumable state machine for timing risk and no gain. The three soundness facts are written at `raft_spawn_election_timer` in `server.cc` and enforced where they can be: one OS thread per fiber (thread-locals stable); no unwind across the switch -- **`panic = "abort"` in the raft crate's profile**; a 1 MiB fiber stack with a `PROT_NONE` guard page (`srpc_fiber.c:46`) -- the plan's earlier "no guard page" assumption was wrong. Comments and a profile: no behaviour change, no rebuild; the gate re-checked the crate under the new profile. |
| F1a | `1d24bbf53` | **The C ABI over the struct exists, in Rust.** 67 `pub unsafe extern "C" fn raft_server_*` exports (19 interface, 8 kernel-called, 40 lab) in server.cc's DSL block, each forwarding to the method; the transpiler emits them as `extern "C"` C++ definitions today and they are the crate's symbols at F2. `src/deptran/raft/server_exports.h` is the generated C++ prototype header -- the one thing hand-written C++ is meant to know of the struct's behaviour -- included from server.h. Both sides come from `scripts/raft_gen_exports.py` off the method signatures, so they cannot drift apart silently: a mismatch is a compile error. Rules learned: exports go in the .cc carrier (a column-0 `extern "C"` definition in a header is multiply defined; the ODR pass only inlines members); shared handles (`Arc<PollThread>`) cross by pointer and are cloned inside -- rustc's `improper_ctypes_definitions` rejected the by-value form, correctly; `# Safety` docs satisfy clippy; the ABI header opens `namespace janus` itself, so its include is scoped outside the namespace (a nested `janus::janus` broke every later `janus::Command`); the emitter lowers `&mut local` in a method call through a raw pointer to `&local`, so the one `&mut u64` out-parameter (`OnInstallSnapshotLocked`) is a raw pointer end to end; and `Box::new(RaftServerBase::new())` cannot lower because the emitted struct is not movable, so allocation is a kernel pair (`raft_server_alloc`/`free`) until the struct is Rust's. Plus the two lifecycle exports `raft_server_new` (alloc + `ConstructRuntime`) and `raft_server_delete` (`Shutdown` + free) the shim will call. Pure addition: no caller uses them yet (F1b, F1c). Test results in the commit message. |
| F1b | `782b34f48` | **`class RaftServer` no longer derives from the struct.** It is `RaftSpecific` with one member, `RaftServerBase* impl_`, obtained from `raft_server_new()` and released by `raft_server_delete()`; its 19 interface overrides and 40 `RAFT_TEST_CORO` lab forwarders are one call each into the C ABI (`scripts/raft_gen_exports.py --shim`, generated from the GEN's own declarations so `const` and reference types are exact). The workers, the RPC service and the lab harness hold the shim and did not change; `frame.cc` still `new`s it. The struct's C++ definition is still emitted and still visible, but no hand-written C++ derives from it, defines a member of it, or touches `impl_->` anything -- only the eleven kernel call sites F1c retargets still call its methods directly. Test results in the commit message. |
| F1c | `3cbcdcfe6` | **Hand-written C++ calls no method of the struct.** The eleven kernel sites (`SetupInternal`, `BindReplicationWakeOwner`, `InitializeSnapshotManagerLocked`, `FailStop`, `ApplyThreadLoop`, `StartElectionTimer`, `OnInstallSnapshotLocked`, `InstallSnapshotReplyAccepted` in the snapshot callback, and the two fiber-loop entries) go through the ABI; two exports were added for the loops, `raft_server_heartbeat_loop` and `raft_server_run_election_timer_loop`, so the spawn kernels no longer name `heartbeat_loop_body` or construct an `ElectionTimerLoop`. **F1 is done: the seam is a C ABI while everything is still transpiled C++.** Hand-written C++ knows `RaftServerBase` only as a pointer type: it derives from nothing of it (F1b), names no field of it (C), calls no method of it (F1c), and reaches it solely through `server_exports.h` -- 71 `extern "C"` functions defined in Rust. What F2 changes is *what defines them*: the crate, linked as a `staticlib`, with the struct's C++ definition deleted from the header. Test results in the commit message. |
| perf | `1085fe9a9` | 25 paired trials, before step A vs after F1c: median -0.61%, mean -0.58%, 12/25 favour F1, p = 1.000. No detectable cost through F1. |
| F2.1 | `8b1cbfe97` | **The crate builds as the static library the cutover links, and every carrier has ownership.** `crate-type = ["rlib", "staticlib"]`; `nm target/release/libraft.a` shows all 71 `raft_server_*` exports defined and 94 `raft_*` kernels undefined -- the link shape F2 needs, checked before any linking. The 16 opaque carriers have kernel-backed `Drop` (`raft_destroy_<name>`, `std::destroy_at` in place); `RaftCommand` lost its bitwise `Clone`; `LearnerAction` -- modelled zero-sized while the struct stores a 48-byte `std::function` by value -- is a pinned carrier; and the wake gate's two `std::sync::Arc<Reactor*>` models became pinned carriers over the C++ `rusty::Arc` (`RaftIntEventPtr`, `RaftPollThreadPtr`, joining `RaftTpcCommitPtr`). Two conventions fell out of the C++ runtime. A `rusty::Arc` has no empty state (`Arc() = delete`), so the three Arc carriers have no `Default`: they are built by facade factories (`rusty::raft_new_int_event`, `rusty::raft_stamped_commit`) and copied by `Clone`, each a kernel into `MaybeUninit` storage under rustc and the same kernel through `raft_construct_by` / the copy constructor under the transpiler -- the DSL spells neither, and the batch path lost a default-constructed `TpcCommitCommand` allocation per entry that `default_like` used to make and `construct_into` destroy. Kernels that fill a slot holding an empty state still construct in place (`construct_into`: destroy, then placement-new); kernels that fill unconstructed storage placement-new only. `set`/`wait_timeout` on the event are kernels, because the emitter renders a method on an opaque carrier with a dot where the C++ Arc needs an arrow. The clone/destroy kernels are unused by the transpiled build and compile against the real types so a drift is caught now. Test results in the commit message. |
| F2.1c | `87ce66914` | **The Raft crate's 44 logging functions are a logger, not stubs.** Under rustc `rusty::raft_log_<level>_<n>` asks the C++ logger whether the level is on (`raft_log_enabled`, so a disabled debug line costs one call and no formatting, as `Log_debug`'s guard arranges), substitutes the arguments into the fmtlib-style string -- `{}`, `{:x}` and doubled braces are the whole specifier set the 123 call sites use; anything else is printed as `{bad spec}` rather than dropped -- and hands rrr's logger one line (`raft_log_line`, into `rrr::log_line` with line 0 and a null file, as `rrr_log.h`'s templates pass). Argument types implement a small `RaftLogArg` trait (Display, plus hex for the integers), so every call site's types were checked by rustc when the crate compiled. Under the transpiler nothing changed: the same names resolve to the variadic templates in `rust_log_shims.h`, which keep `std::format_string`'s compile-time placeholder check. The formatter has unit tests in the facade (`cargo test` in src/rrr/rusty-rustc). Test results in the commit message. |
| F2.2 | `424fe4c79` | **The wake gate is Rust's: construction, reservation, and the job the reactor runs.** `ReplicationWakeGate` is built in the DSL with `Arc::new_cyclic(\|_weak\| ReplicationWakeGate::new())` -- the one `Arc` constructor the emitter lowers to the runtime's in-place path, so a `PhantomPinned` payload compiles (checked on a scratch carrier; `Arc::new` lowers to a move that does not) -- and the `raft_new_replication_wake_gate` kernel is gone. `RequestReplication` and `CloseReplicationWakeGate` reserve the owner thread themselves and hand the reactor a Rust-owned token, `Box<GateWakeJob>` (the gate's `Arc` and which wake to run) made raw; the one kernel left, `raft_queue_wake_job`, does `PollThread::add` of a `OneTimeJob` whose body is the export `raft_wake_job_run`, which takes the `Box` back, runs `GateWakeJob::run`, and drops it. `QueueReplicationWake`, `QueueReplicationShutdownWake` and the two `raft_queue_replication_*` kernels are deleted: no hand-written C++ calls a method of the gate any more. An emitter fact recorded on the token type: a field's `Arc`-ness is known only inside the block that declares it, so the wake runs in `GateWakeJob::run` rather than in the export. Test results in the commit message. |
| F2.3 | `5e97e4884` | **Nothing crosses the seam as a C++ object: the lab suite reads scalars, and no export takes a non-trivial type by value.** `LabLog()` -- a `RaftLog&` handed to test.cc -- is replaced by `LabLogFingerprintLen`/`LabLogFingerprintAt`, two `u64` reads that `RaftLogFingerprint(RaftServer&)` loops over, so the harness holds no pointer into the log and `RaftLog` no longer crosses (`use crate::server_h::RaftLog` and its bridge alias are gone from server.cc). D2: the five setters that passed a `std::function`/`shared_ptr` by value across the C ABI (`reg_learner_action`, `RegisterLeaderChangeCallback`, `SetSnapshotManager[Locked]`, `SetStateMachineSnapshotCallbacks`) receive the carrier by pointer and copy it inside through a clone kernel into a default-constructed slot, exactly as `RaftCommand` does; the shim passes `&param`, the generator's `BY_VALUE_CARRIERS` table drives all three outputs, and the interface in scheduler.h and Paxos are untouched. `grep` over `server_exports.h` for a by-value `rusty::` parameter is **0** -- was 5. And the 17 `pub const fn raft_server_*` predicates that Rust never called are deleted: 13 had no caller at all, and the 6 hand-written C++ call sites (a wire kind, the lab snapshot marker, the commo inline-path sentinel, a `Start` result in raft_worker.cc and testconf.cc) now compare in C++, which is where those facts live; under the cutover their emitted inline C++ would have vanished with the GEN. Three cutover preconditions land with it. The three `#[repr(C)]` values kernels return by value (`RaftElectionTimeouts`, `RaftVoteOutcome`, `AppendRespView`) have their own block, `raft_server.kernel_result_pods`, which stays C++-visible at the cutover while the struct's block is deleted from C++. The two RPC bodies the service kernels called directly (`on_request_vote_body`, `on_append_entries_body`) are exports (`raft_server_on_*_body`, `&mut` parameters as C++ references), declared in `server_exports.h`. And the 100 C++ `static_assert`s over the Rust predicates are Rust `const _: () = assert!(...)` items next to the predicates: the emitter lowers each back to a `static_assert`, so the transpiled build checks them exactly as before and rustc checks them at every gate; the hand-written C++ block is gone. A probe that deleted every generated region of server.h/server.cc on a scratch copy and compiled six TUs is what produced this list -- after it, what the compiler still wants from the generated C++ is the base-class pins and the alloc/free kernels, which are the cutover's own. Test results in the commit message. |
| F2.6 | `1de45affa` | **The Raft server is compiled by rustc.** `src/deptran/raft/src/server_h.rs` and `server_cc.rs` are canonical Rust (no longer extracted; `kind = "canonical"` in `rust-modules.toml`), built by cargo as `libraft.a` into the build tree and linked by CMake (`raft_rust`, in a rescan group with `txlog_core` because each needs the other). server.h keeps one inline block, the kernel-result PODs (extracted to `server_pods_h.rs`); server.cc keeps none -- 11 + 5 generated regions, the bridge namespace, the alloc/free kernels, the base-class and enum-layout pins, `rust_log_shims.h` and the facade's other C++ halves are deleted, `struct RaftServerBase;` is a forward declaration, `raft_server_new`/`raft_server_delete` are `Box::into_raw`/`Box::from_raw`, nine `extern "C" inline` kernels became out-of-line definitions (a header inline is emitted only where C++ uses it, and only Rust uses these), and three RPC kernels say `extern "C"` on their definitions. The generator derives the shim's lab signatures from the Rust signatures (byte-identical to what the C++ struct used to yield). `nm libraft.a`: 74 exports defined, 97 kernels undefined, every one defined by a C++ object; no C++ object defines a `raft_server_*` symbol. `dbtest` and `deptran_server` already link a second Rust static library (`librust_redis.a`); a trivial program linked against both archives, referencing one export from each, shows no duplicate-symbol clash on this toolchain, so `libraft.a` links alongside it rather than through an umbrella crate. **The first rustc-compiled build found the one false assumption of slice 1:** a libc++ `std::function` is not bitwise-relocatable (its small callable lives inside the object and `__f_` points at it), so the first `reg_learner_action` -- a by-value carrier moved twice on the way to its slot -- jumped through a dead stack frame (SIGSEGV in `raft_server_reg_learner_action`, confirmed under gdb). The fix is structural: the four callback carriers are never held by value anywhere but their final slot; the two interface setters take them by reference (`const T&` in C++, which is also what Paxos's macro now takes) and the two lab setters likewise, and the clone kernels construct in place. Every other carrier relocates bitwise. Test results in the commit message. |
| perf | `paired-trial-19cfbb213-vs-1de45affa.csv` | 25 paired trials, before step A (`19cfbb213`) vs the rustc-compiled server (`1de45affa`): median +0.32%, mean +1.61%, 13/25 favour Rust, exact sign test p = 1.000. **No detectable throughput cost of the whole conversion, cutover included.** |
| F2.7 | `raft: F2.7` | **Rust calls Rust.** `OnRequestVote` and `OnAppendEntries` (server_h.rs) call `on_request_vote_body` / `on_append_entries_body` (server_cc.rs) directly; the two `raft_rpc_*` forwarder kernels -- which existed only because one carrier's Rust could not name the other's through C++ -- and the two exports they needed are deleted (2 kernels, 2 prototypes, 2 `#[no_mangle]`s). The AppendEntries payload still crosses as the opaque handle plus `raft_command_has_value`, read in Rust now. `scripts/raft_regen_exports.py` is the one command that regenerates the exports, the header and the shim. Test results in the commit message. |

## Performance verdict on the cutover (`19cfbb213` vs `1de45affa`)

Same method as below: `scripts/raft_paired_trial.sh`, 25 ABBA pairs of
`shard1ReplicationRaft`, one run at a time, on this host, the baseline tree's
`build/dbtest` against this tree's -- which is now the rustc-compiled Raft
server linked as `libraft.a`. Raw data:
`docs/migration/raft/paired-trial-19cfbb213-vs-1de45affa.csv`; verdict by
`python3 scripts/raft_paired_trial.py <csv>`.

| | value |
|---|---|
| completed pairs | 25 (0 dropped) |
| median delta (Rust - baseline) / baseline | **+0.32%** |
| mean delta | +1.61% |
| pairs favouring Rust | 13 / 25 |
| exact two-sided sign test | p = 1.000 |
| per-pair deltas (%) | -2.8 -5.7 -1.0 +0.3 +10.0 -0.7 -2.7 +12.1 +7.0 -2.4 -2.1 +6.6 -4.7 +7.0 +9.8 +4.5 +2.7 +6.0 -0.7 +4.9 -4.7 -4.8 +3.2 +8.2 -9.8 |

No detectable throughput cost of the conversion, the cutover included: the
Rust-compiled server is statistically indistinguishable from the C++ it
replaced, with the same ~13% run-to-run spread the earlier trials showed.

## Performance verdict on A..C3

`scripts/raft_paired_trial.sh` with tree A = `19cfbb213` (before step A,
built in a worktree with the production configuration) and tree B =
`2996cc572` (after C3), 25 ABBA pairs of `shard1ReplicationRaft`, quiet host,
no failed runs. Raw data: `paired-trial-19cfbb213-vs-2996cc572.csv`.

| | A (before) | B (after C3) |
|---|---|---|
| median ops/s | 157,474 | 157,724 |
| mean ops/s | 155,029 | 156,485 |
| run-to-run spread (stdev/mean) | 5.0% | 4.2% |

Per-pair delta (B-A)/A: **median +0.60%, mean +1.23%, 14/25 pairs favour B,
exact two-sided sign test p = 0.69.** No detectable cost, at a resolution of
roughly +-3% on the median. The ~1.5% the tree already carried from the
earlier conversion phases has not been widened by A..C3, which is the only
claim this trial can make; localising that earlier cost remains the open
performance task it was.

**And again for F1** (tree B = `3cbcdcfe6`, after F1c; same method, same
baseline, 25 pairs, no failed runs; raw data
`paired-trial-19cfbb213-vs-3cbcdcfe6.csv`): median delta **-0.61%**, mean
-0.58%, 12/25 pairs favour B, **p = 1.000**. Medians 158,734 vs 155,158
ops/s. The extra cross-TU call on every interface method -- virtual, then
`extern "C"`, then the method -- is not visible at this resolution. Both
trials together: from before step A to the end of F1, no detectable
throughput cost.

## Risks, ranked

1. **The silent failures in the constraints list.** Every one compiles.
   The "done" tests exist because of this.
2. **Step E, option 1, perturbing timing.** Moving the wait out of the Rust
   loop changes nothing about *when* the wait happens only if the C++ loop
   is a faithful transliteration. RaftLabTest's election-timing cases are
   the check.
3. **Step F's one-shot nature.** Mitigated by the side-by-side build.
4. **Performance.** See above. The cost of the current approach is known and
   small; the cost of F is estimated and unmeasured.
