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

Scalars, `#[repr(C)]` structs, opaque pointers. `janus::Command` becomes an
opaque handle `{ i32 kind; void* payload }` with four C++ functions
(`clone`, `drop`, `kind`, `has_value`); Raft never inspects the payload
beyond those two facts. `rusty::Function` callbacks become
`extern "C" fn(*mut c_void, ...)` plus a context pointer.

**Makes possible:** the *traits* surviving the cutover, not just the struct.
A trait object is a vtable; an FFI table is a vtable. Same shape, so D is
retyping, not restructuring.

**Done when:** no `rusty::`-namespaced type appears in the signature of any
kernel or of any method in `TxLogServer`/`RaftSpecific`.

### E. Decide who owns each suspension point

**Goal:** an explicit, recorded decision for every place a Rust-authored
body yields the fiber, so the cutover has no surprise.

Today two Rust-authored bodies suspend: `ReplicationWakeGate::
finish_wait_for_work` and `wait_for_election_timeout` (both call
`waiter.wait_timeout`), and `HeartbeatDriver::run` /
`heartbeat_loop_body` loop around the first. A Rust frame can live across
the reactor's hand-written x86-64 context switch -- it is an ordinary stack
swap on one OS thread -- but three things are then true and must be
accepted in writing: fiber stacks have no Rust guard page, so overflow is a
plain SIGSEGV rather than a stack-overflow abort; no unwind may cross the
switch; and the Rust frame's stack budget is the fiber's, not the thread's.

Two options, and the goal statement leans to the first:

1. **C++ owns every loop and every wait.** `HeartbeatDriver::run` becomes a
   C++ loop that calls `HeartbeatPrologue`, `heartbeat_phase0..3_body`,
   `HeartbeatEpilogue` as non-suspending Rust calls; the wake gate's wait
   moves to the C++ side of the call. Same for the election timer. Rust
   never yields.
2. **Rust runs on the fiber, with the three constraints recorded** and a
   stack-size assertion in the spawn kernel.

Independent of A-D; can run alongside them.

**Done when:** either `grep -n 'wait_timeout\|\.wait(' src/deptran/raft/src/*.rs`
is empty (option 1), or the constraints are written into the spawn kernels
and enforced by an assertion (option 2).

### F. Cut over

**Goal:** the goal statement, literally.

`src/deptran/raft/Cargo.toml` gets `crate-type = ["staticlib"]` and links
the way `third-party/mako-redis` does. Inside the crate `rusty::Vec` becomes
`Vec`, `rusty::Mutex` becomes `std::sync::Mutex`, and so on -- the source
otherwise does not change. The GEN regions for everything that moved are
deleted from `server.h`/`server.cc`; what remains of them is the shim (three
`TxLogServer` forwarders) and the kernels, now `extern "C"` on both sides.

This is a cutover, not an increment. Keep both builds alive behind a CMake
option for one release so the two can be run side by side.

**Done when:** `nm` on the Raft binaries shows the Rust v0 symbols; every
GEN region that used to hold a `RaftServerBase` method is gone; and the
verification below passes on the rustc build.

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
