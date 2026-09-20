# What is Rust and what is C++ in Mako's Raft, and where the seam is

A map of the running system at `e4963b7ae`, written so the "impossible"
claims in it can be attacked rather than believed.

**Provenance.** The first draft of this file was checked, claim by claim,
against the source by five independent verification passes. They found 31
factual errors and 9 overstatements in it, and one latent lock-order
inversion in the code it describes (fixed in `040d448c5`). Everything below
survived that pass or was rewritten by it. Where a number was measured
rather than inferred, it says so.

## How to read this

Three kinds of code, marked everywhere below:

- **[RUST]** — authored as Rust in a `#if RUSTYCPP_RUST` block. The
  transpiler renders it into the `/*RUSTYCPP:GEN-*/` region beside it, and
  THAT C++ is what compiles.
- **[C++]** — hand-written C++.
- **[EXT]** — outside `src/deptran/raft`: the rrr runtime, the wire types,
  the application state machine.

**No Rust machine code is in this binary.** Verify with the strict Rust v0
mangling — `nm build_rafttest/deptran_server | grep -cE ' _R[A-Za-z0-9_]{8,}$'`
returns 0. (A bare `grep -c '_R'` returns 154 and proves nothing: it matches
Itanium-mangled C++ symbols that merely contain those two characters.)

**This applies to rrr's "canonical Rust" modules too**, and the distinction
matters in section 3. `src/rrr/rust-modules.toml` lists seventeen `.rs`
files that rustc compiles — but only as a gate. Each is ALSO transpiled to a
`rrr.<module>.cppm` C++ module, and that is what enters the build
(`src/rrr/CMakeLists.txt:127`). "Canonical Rust" means Rust-authored, not
Rust-compiled-into-the-binary. The one place rustc output really links is
`third-party/mako-redis`; see claim 1b.

A `>>>` line marks a language boundary crossing.

## 1. The static picture: who owns what memory

The hierarchy first, because the first draft got it wrong and the error
propagated into two of the impossibility claims:

```
TxLogServer              [RUST] DSL pub trait -> C++ abstract class
  │                      3 pure virtuals (set_site_identity, set_commo,
  │                      reg_learner_action) + a virtual destructor. No state.
  │
  └── RaftServerBase     [RUST] DSL struct, #[cpp_inherit] impl TxLogServer
      │                  emitted as `struct RaftServerBase : public TxLogServer`
      │                  (server.h:5813). IT carries the vtable and implements
      │                  all three pure virtuals. 48 fields: all of Raft's state.
      │
      └── RaftServer     [C++] `class RaftServer : public RaftServerBase`
                         (server.h:7485). 361 lines, ZERO data members, and
                         ZERO virtual or override methods of its own.
```

`class RaftServer` is nearly a shell but not entirely one. What it still
holds:

```
├── OnRequestVote, OnAppendEntries                    [C++] genuine shims
│     >>> plain call to a DSL FREE FUNCTION (on_request_vote_body,
│         on_append_entries_body). Both sides are C++ after transpilation.
├── OnInstallSnapshot                                 [C++] NOT a plain shim
│     takes state_machine_apply_mtx_ then mtx_, calls the METHOD
│     RaftServerBase::OnInstallSnapshotLocked through raft_catch, and
│     fail-stops if it throws
├── Start                                             [C++] calls the METHOD
│     RaftServerBase::StartImpl. There is no `start_body`.
├── SetLocalAppend                                    [C++] ~25 lines of LOGIC
│     reads state_.raft_log_.last_index(), calls raft_log_.append(
│     RaftEntry::new_(current_term_, cmd)), verifies the result. server.h:7698
├── PrepareStateMachineSnapshotLocked                 [C++] returns unique_ptr
│     and calls an EMBEDDER callback -- behind raft_catch
├── LoadStateMachineSnapshotLocked                    [C++] embedder Commit(),
│     also behind raft_catch
├── InitializeSnapshotManager, GetSnapshotManager     [C++]
├── commo()                                           [C++] dynamic_cast
├── struct LabAccess                                  [C++] server.h:7640
│     11 static accessors the RaftLabTest harness uses to reach private
│     state: stop_, state_machine_apply_mtx_, state_.is_leader_,
│     state_.vote_for_, snapshot_manager_, CreateSnapshotLocked(), ...
├── RaftServer()                                      [C++] the two-step init
└── ~RaftServer() { Shutdown(); }                     [C++] one line
```

Who calls in: the rrr service layer names **three** of those methods
(`OnRequestVote` at service.cc:72, `OnAppendEntries` at :92 and :120,
`OnInstallSnapshot` at :141) plus `IsDisconnected` and `IsRpcReady`. `Start`
is called by the benchmark worker (raft_worker.cc:776) and the lab harness
(testconf.cc:307), not by the service layer.

### The state, all of it Rust-declared

```
RaftServerBase                                        [RUST] 48 fields
├── state_: RaftConsensusState                        [RUST] 25 fields, guarded by mtx_
│   ├── raft_log_: RaftLog                            [RUST]
│   │   ├── base_, head_, len_: u64                   [RUST]
│   │   └── blocks_: Vec<Vec<RaftEntry>>              [RUST]
│   │       └── RaftEntry                             [RUST]
│   │           ├── term_: i64                        [RUST]  <-- LEAF
│   │           └── cmd_: RaftCommand                 [OPAQUE] <-- LEAF
│   │                 24 bytes of janus::Command. Rust holds, moves, clones
│   │                 (a refcount bump on the inner Arc) and defaults it.
│   │                 Rust cannot look inside.
│   ├── peers_: PeerTable -> Vec<FollowerProgress{next_, match_: u64}>  <-- LEAVES
│   └── 23 scalars                                    [RUST]  <-- LEAVES
│         current_term_, commit_index_, execute_index_, snapidx_, snapterm_,
│         vote_for_, is_leader_, req_voting_, election_in_progress_,
│         current_leader_id_, heartbeat_round_, read_quorum_confirmed_{term,
│         round}_, site_id_, partition_id_, loc_id_, and 7 non-pub:
│         election_term_, election_timeout_us_, election_timer_generation_,
│         snapshot_threshold_, snapshot_callback_owner_token_,
│         next_snapshot_callback_owner_token_, last_heartbeat_time_
│
├── apply_queue_: Mutex<ApplyQueue>                   [RUST] the lock OWNS the data
│   └── ApplyQueue { entries_: VecDeque<QueuedApplyEntry>, epoch_: u64 }
│         QueuedApplyEntry { index_: u64, command_: RaftCommand, epoch_: u64 }
├── replication_wake_gate_: Arc<ReplicationWakeGate>  [RUST] PhantomPinned
│   ├── owner_: Mutex<Option<Arc<PollThread>>>        [RUST holding EXT]
│   ├── waiter_, election_waiter_: Mutex<Option<Arc<IntEvent>>>
│   └── 6 AtomicBools                                 [RUST]  <-- LEAVES
├── decoded_terms_: Vec<i64>, peer_sites_/config_members_: Vec<u16>  <-- LEAVES
├── batch_buffer_: Vec<Arc<RaftTpcCommitCommand>>     [RUST container, OPAQUE element]
├── startup_finished_: Mutex<bool>, startup_cv_: Condvar             [RUST]
├── 11 atomics, 17 plain scalars                      [RUST]  <-- LEAVES
│
└── THE OPAQUE FIELDS — Rust names the type and holds the bytes, C++ defines
    what they mean. Every size below is asserted from BOTH sides:
    static_assert in server.h:2251-2271, size_of/align_of in
    src/rrr/rusty-rustc/src/lib.rs:502-552.
    ├── mtx_: RaftCheckedMutex                  48B/8
    ├── state_machine_apply_mtx_: std::mutex     40B/8
    ├── apply_thread_: std::thread                8B/8
    ├── async_callback_lifetime_: shared_ptr     16B/8
    ├── snapshot_manager_: shared_ptr            16B/8
    ├── create_sm_snapshot_cb_: std::function    48B/16
    ├── prepare_sm_snapshot_cb_: std::function   48B/16
    ├── leader_change_cb_: std::function         48B/16
    ├── pending_apply_command_: janus::Command   24B/8
    ├── app_next_: LearnerAction                 the state machine hook
    └── commo_: *mut Communicator                a raw pointer to [EXT]
```

## 2. The seam: 50 kernels, 529 lines

Hand-written `extern "C"` `raft_*` functions defined in `server.cc` outside
every Rust and GEN region. `extern "C"` is part of that definition and does
work: `server.cc` also holds two file-local `raft_*` helpers -- `raft_catch`
(13 lines) -- which is NOT counted here, because it crosses no language
boundary. Anything counting by name prefix alone gets 51. Grouped by WHY each exists, which is the axis on
which some are removable and some are not.

### (a) Wire format and marshalling — 11 kernels, 143 lines
```
raft_ae_decode_payload      raft_ae_apply_incoming     raft_batch_try_push
raft_batch_finalize         raft_append_response_read  raft_command_has_value
raft_command_kind           raft_append_entries_batch_max
raft_batch_optimization_enabled   raft_set_local_append   raft_append_leader_noop
```
`janus::Command` (`src/deptran/mako_commands.h:433`) is a
`rrr::SerializableEnvelope<MakoCommands>`: a kind tag plus an
`Arc<SerializableBase>`, whose virtuals are `save` / `load` / `kind` /
`payload_type_id`.

**The decode is NOT an RTTI downcast**, which the first draft claimed and
which would have made this group far harder to move. `marshallable_cast`
compares `payload_type_id()` against `std::type_index(typeid(T))` and then
`reinterpret_cast`s. A type-index equality test is much closer to something
the DSL could express. Note also that `SerializableEnvelope` itself is a
canonical Rust module (`src/rrr/misc/serializable_envelope.rs`) — only
`janus::Command`'s subclass in `mako_commands.h` is hand-written C++.

Shared with Paxos (`src/deptran/paxos/commo.h`), NOT with the benchmark —
`grep -rn "Command" src/bench/` returns nothing.

### (b) The reactor — 13 kernels, 59 lines
```
raft_spawn_heartbeat_loop   raft_spawn_election_timer   raft_spawn_election_timer_fiber
raft_spawn_apply_thread     raft_create_int_event       raft_queue_replication_wake
raft_queue_replication_shutdown_wake   raft_fiber_sleep_us
raft_thread_sleep_ms        raft_shutdown_barrier_yield
raft_new_replication_wake_gate   raft_apply_thread_join   raft_bind_replication_poll
```
Only **two** of these (`raft_fiber_sleep_us`, `raft_create_int_event`) are
pure naming gaps. See section 6 for what the other eleven actually need.

### (c) The snapshot manager — 8 kernels, 128 lines
```
raft_snapshot_manager_latest   raft_snapshot_manager_load   raft_snapshot_manager_has_latest
raft_snapshot_recovery_pick_manager    raft_snapshot_serialize_and_save
raft_install_snapshot_payload  raft_load_state_machine_snapshot
raft_initialize_snapshot_manager
```
A C++ class hierarchy, `std::string` payloads, and an abort-on-destruction
`std::unique_ptr` transaction. Two of these reach an embedder callback and
so sit behind `raft_catch`; see claim 2.

### (d) Application and configuration hooks — 13 kernels, 100 lines
```
raft_apply_invoke            raft_fire_leader_change     raft_leader_change_cb_is_set
raft_prepare_snapshot_cb_is_set   raft_env_lookup
raft_env_snapshots_enabled        raft_election_timeouts
raft_load_current_config     raft_monotonic_now_us      raft_monotonic_now_secs
raft_clear_async_callback_owner   raft_setup_internal_guarded
raft_log_set_is_leader_entry
```

### (e) The network path — 5 kernels, 99 lines
```
raft_commo_set_network_enabled   raft_phase1_send_append
raft_phase1_load_and_send_snapshot     raft_broadcast_vote_and_wait
raft_vote_quorum_snapshot
```

11 + 13 + 8 + 13 + 5 = 50. 143 + 59 + 128 + 100 + 99 = 529.

## 3. The external edge: where the bytes actually leave

```
[RUST] heartbeat_phase1_body                             server.cc:2675
  │
  >>> raft_phase1_send_append                            [C++] 9 lines
      └── RaftCommo::SendAppendEntries2                  [C++] commo.cc:26
          │   hand-written, 325 lines, zero DSL blocks
          ├── create_sp_int_event(1)  -> Arc<IntEvent>   [EXT]
          ├── PeerForSite(par_id, site_id)
          │      -> Communicator::Peer = shared_ptr<RpcPeer>   (NOT a proxy)
          ├── peer->WithClient([](rrr::Client* c){ RaftProxy proxy(c); ... })
          │      the proxy is STACK-CONSTRUCTED inside that callback
          └── proxy.async_EmptyAppendEntries(...)        [EXT] rcc_rpc.h
              │   1496 lines of C++ GENERATED BY bin/rpcgen, not by rusty-cpp
              │   (the non-empty-cmd branch uses async_AppendEntries)
              └── rrr::Client -> tcp_channel.rs:1114
                  └── send(fd, data, size, MSG_NOSIGNAL)  <-- LEAF: the real edge
                      src/rrr/rpc/srpc_connect.c:54  — hand-written PLAIN C
```

Receive:

```
epoll_wait(2)   src/rrr/reactor/epoll_wrapper.rs:101     <-- LEAF: the real edge
  │   (epoll_create and epoll_ctl are elsewhere, in the inline-Rust carrier
  │    src/rrr/reactor/epoll_platform_linux.cc:192)
  └── rrr service dispatch                               [EXT]
      └── RaftService::AppendEntries      pure virtual, rcc_rpc.h:700
          └── RaftServiceImpl::AppendEntries   THE VTABLE HOP, service.cc:78
              └── svr->OnAppendEntries(...)  [C++] a DIRECT call — not virtual
                  >>> on_append_entries_body                     [RUST]
```

**Where the file descriptors are.** Not at the Rust/C++ boundary.
`IntEvent` is a fiber counter — `status_`, `value_`, `target_`, a
`Weak<Fiber>`, all `Cell`/`RefCell`, no mutex — and its `wait()` ends in
`fiber_swap_context`, hand-written assembly. It touches no fd, eventfd, pipe
or futex. (It holds no wait-list either; the queue is the Reactor's
`waiting_events_`, and only one fiber may wait on an event.) The only real
fds are the TCP sockets and the epoll instance.

**But do NOT conclude the syscalls are behind Rust.** That holds for epoll
only. The transport syscalls — `send`, `recv`, `connect`, `shutdown`,
`fcntl`, `socket` — live in `src/rrr/rpc/srpc_connect.c`, hand-written plain
C with zero DSL blocks, whose own header comment says the opposite of what
the first draft of this document concluded: *"this code will never be Rust
(it is the syscall surface the future crate's own extern-C kernels
mirror)"*.

## 4. Concurrency: who touches which memory

**13 OS threads** in a five-site RaftLabTest process, counted from
`/proc/<pid>/task` and reconciled against the spawn sites:

| count | source |
|---|---|
| 1 | main thread, blocked in `thread.join()` — `s_main.cc:105` |
| 1 | the one surviving site launcher — `s_main.cc:69` spawns 5; only site 0 stays, entering `Reactor::run_loop` (`server_worker.cc:141`) |
| 5 | one `PollThread` per `ServerWorker` — `server_worker.cc:64` |
| 5 | one apply thread per `RaftServer` — `server.cc:784` |
| 1 | `RaftTestConfig::netctlLoop` — `testconf.cc:133` |

Two things about the control-plane poll thread, both of which the first
draft got wrong in both directions:

- **`raft_bench` DOES have one.** The launcher script passes no `-b`, but
  the binary hand-builds its argv and injects `-b` itself
  (`raft_bench.cc:909`), so `Config::do_heart_beat()` is true and
  `RaftWorker::SetupHeartbeat` (`raft_worker.cc:484`) creates a second
  `PollThread`. It serves `ServerControlServiceImpl` on `site_port + 10000`
  — `server_ready`, `server_shutdown`, `server_heart_beat`, the benchmark
  driver's liveness channel, unrelated to AppendEntries.
- **The lab binary has none, and `-b` is not why.** `deptran_server`
  (RAFT_TEST=ON) is built from `s_main.cc` + `server_worker.cc` and drives
  `ServerWorker`, which has no `SetupHeartbeat` call at all.

`ServerWorker` is the RAFT_TEST harness; `RaftWorker` is the production
path. (`server_worker.h:15-16` says so.) **Both lifecycles are exercised.**
RaftLabTest covers `ServerWorker`; all four Raft replication suites --
`shard{1,2}ReplicationRaft` and `shard{1,2}ReplicationSimpleRaft` -- cover
`RaftWorker`, and the converted Raft is in those binaries because
`server.cc` is compiled into production `mako` regardless of
`MAKO_USE_RAFT` (`CMakeLists.txt:1049`). Until 2026-09-19 only the first
was ever run, and three of the errors the verification pass found were in
descriptions of the second. `svr_hb_poll_thread_worker_g` is a
per-worker member (`raft_worker.h:130`); the `_g` is vestigial.

**The benchmark configuration has two more kinds of thread, and the table
above does not cover it.** `raft_bench` goes through `raft_main_helper.cc`,
not `s_main.cc`, and that path adds:

- the control-plane `PollThread` described above (`raft_bench` injects `-b`
  itself);
- one `PollThread` per STUB SERVER (`raft_main_helper.cc:375`).

A stub server is not a client and holds no client state -- the name is
misleading. In single-group mode the process runs exactly ONE `RaftServer`,
but remote replicas' `Communicator`s expect to connect to every partition
port, so the process binds the OTHER sites' addresses too and registers the
same `RaftServiceImpl` against the same server (`raft_main_helper.cc:354-357,
379-383`). A request arriving on a foreign port reaches the one real Raft
server. The count is `all_site_infos_g.size() - 1` -- one per other site,
NOT one per server -- and zero unless `raft_group_mode_g == kSingleGroup`
with more than one site.

Each stub gets its own `PollThread` because `rrr::Server` is constructed
with one, and that thread owns the epoll set its listening socket and
accepted connections register on. Sharing is possible and done elsewhere:
`ServerWorker` runs two `rrr::Server`s on one poll thread
(`server_worker.cc:13`). So the per-stub thread is a choice.

```
MAIN THREAD
  └── ~RaftServer -> Shutdown                [RUST] via the C++ destructor
      Setup does NOT run here: both worker paths wrap EnsureSetup() in a
      OneTimeJob and add() it to the POLL thread (server_worker.cc:121-130).

POLL THREAD (one per server)
  │   Runs the epoll loop. Setup, and two Raft FIBERS, are multiplexed on it:
  ├── heartbeat fiber              [RUST] heartbeat_loop_body
  │     └── HeartbeatDriver::run   [RUST] owns HeartbeatRoundState
  │           PHASE 0 -> 1 -> 2 -> 3, all four Rust
  ├── election fiber               [RUST] ElectionTimerLoop::run
  └── RPC handler fibers           OnAppendEntries / OnRequestVote / OnInstallSnapshot

  The first two are FIBERS sharing one OS thread — which is why mtx_ cannot
  be recursive: std::recursive_mutex tracks ownership by thread::id and
  would hand the lock to a second fiber silently.

APPLY THREAD (one std::thread per server)
  └── ApplyThreadLoop              [RUST]
        pops apply_queue_ (its own Mutex), then
        >>> raft_apply_invoke -> app_next_(id, cmd)      [EXT]
```

**Lock order** — `callback_lifetime->mutex` → `state_machine_apply_mtx_` →
`mtx_` → `apply_queue_`. This was documented as "verified" and was not: the
InstallSnapshot completion callback took `callback_lifetime->mutex` while
PHASE 1 held `mtx_`, on the inline path where `PeerForSite` returns null and
`commo.cc:167-170` invokes the callback on the caller's stack. Fixed in
`040d448c5` by moving the availability check above both locks. It was latent
rather than live — both contexts run on one poll thread — but a total order
an existing call site inverts is not a total order.

## 5. The three claims of impossibility, and exactly what backs each

### Claim 1: `RaftServerBase` cannot be compiled by rustc and linked

**1a. It is a vtable-bearing C++ base class.** `struct RaftServerBase :
public TxLogServer` (server.h:5813) implements three pure virtuals, and
`class RaftServer : public RaftServerBase` derives from it. Base-class
layout and vtable injection are C++ ABI features **rustc-emitted machine
code** cannot participate in.

The transpiler already emits `RaftServerBase` *as C++ source* that clang
compiles into a real base class — that path works today, and mistaking it
for a counterexample is the obvious wrong objection. It is not one:
`#[cpp_inherit]` is an identity macro under rustc
(`rusty-cpp-markers/src/lib.rs:7-9` returns its input unchanged), so rustc
sees a bare `impl Trait for Type` with no vtable slot and no base subobject;
the base clause is literal text the emitter writes
(`emit_items.rs:2221-2223`). Nothing in rusty-cpp makes a Rust type
inheritable *by* C++ — no `cpp_base`, `cpp_derive`, `cpp_abstract` or
`inheritable` attribute exists.

And the repo demonstrates the claim rather than contradicting it: rustc
compiles this exact struct today — `scripts/raft_dsl.sh:680` runs `cargo
build --lib` over the generated crate, where `pub struct RaftServerBase`
sits at `src/deptran/raft/src/server_h.rs:1451` — and its output reaches no
linker. CMake links exactly one cargo artifact, `librust_redis.a`.

*Strength: the soundest claim in this section.* **Cost of removing the
premise: the twelve `static_cast<RaftServer*>(self)` sites, and nothing
else.** `RaftServer` supplies no vtable — it has zero `virtual` and zero
`override` in its whole 361-line body; `RaftServerBase` implements
`TxLogServer` directly, from the DSL.

**1b. The layout does not match.** 22 of the 52 kernels read or write
`RaftServerBase` fields directly (`self->state_.raft_log_`,
`self->site_id_`); two more reach `RaftServer` methods through the same
pointer. A rustc-compiled struct would need byte-identical layout. The
opaque carriers match as of `26ca518a3`. The containers do not, and
`server.h:2244-2249` lists all five as unpinned: `rusty::Vec` is **48 bytes
in C++ against std::vec::Vec's 24 in Rust** — measured, by compiling a probe
against the project's own `vec_port.vec` PCM, not inferred — and
`rusty::Mutex`, `rusty::VecDeque`, `rusty::Condvar` and `rusty::Arc` have
their own differences. All five are present in `RaftServerBase`.

**The bold alternative 1b does not need.** Make the boundary NARROW instead
of making the layout match. `third-party/mako-redis` is the in-tree proof:
`crate-type = ["staticlib"]`, a statically linked Rust runtime of 3,595
mangled symbols in `build/makoCon` — almost all of it `core`/`std`/`alloc`
and dependency crates, with roughly six authored top-level functions —
reached through a boundary of **four** `extern "C"` functions (`rust_init`
out; `cpp_worker_thread_init`, `cpp_execute_transaction`,
`cpp_free_transaction_response` in).

Be precise about what that boundary does and does not agree on. **Four
`#[repr(C)]` structs DO cross**, by pointer — `TxnOperation`, `TxnRequest`,
`TxnOpResult`, `TxnResponse`, each declared twice and hand-mirrored, and
reordering a field on either side silently corrupts the other. What is
avoided is agreement on any *transpiled C++ container*: no `Box::into_raw`,
no opaque handle, no callback, no C string. That is the property Raft would
need, and its 22 field-dereferencing kernels are what stand in the way.

### Claim 2: exceptions — the claim was wrong as stated

The original wording — "exceptions have no DSL spelling, and three places
need it" — was wrong twice. There were **ten**, not three; and the thing
without a DSL spelling is not the catch.

`std::panic::catch_unwind` IS mapped (`types.rs:719`), and the C++ it lowers
to is literally `try { … } catch (...) { Err(current_exception()) }`
(`panic.hpp:35`). So nine of the ten catches were expressible in Rust. What
has no DSL spelling is **invoking a C++ callable**: `app_next_` is a
`std::function` (`scheduler.h:61`) and `Commit()` is a pure virtual
(`server.h:82`). Rust can hold both — it does, as opaque carriers — and can
call neither.

Sorting by WHAT THROWS is what decides each site:

| what throws | sites | treatment |
|---|---|---|
| `std::stoull` on an env var | 3 | **now Rust.** `RaftServerBase::raft_env_u64` returns `Result<Option<u64>, RaftEnvError>`; the digits are walked off the raw pointer, so nothing can throw |
| an embedder callback | 4 | **irreducible**, but because of the CALL, not the catch |
| a Rust body, beneath which `bad_alloc` and the snapshot manager still can | 3 | **kept as a backstop**, unified behind `raft_catch` |

`server.cc` holds 1 `try` and 2 `catch` clauses, all inside `raft_catch`.

**Why the remaining seven stay in C++, which is a judgement and not a
limit.** `catch_unwind` would express them, at two costs. Under rustc it
catches *Rust panics* and returns `Result<R, Box<dyn Any + Send>>`; under
the emitted C++ it catches *any C++ throw* and returns `Result<R,
std::exception_ptr>`. Those are different functions wearing one name, and
the gate verifies the first while production runs the second. And it is
sound only while the Rust is transpiled: once real Rust links, a C++
exception crossing a Rust frame is UB and `catch_unwind` genuinely will not
catch it. A C++ frame at the boundary is correct in both regimes, and is
what `cxx` generates rather than avoids.

Two things worth knowing before anyone tries the Rust route anyway.
`rusty::ffi::CStr` is mapped by the transpiler but **not implemented** in
the C++ runtime, so a DSL body cannot receive a C string — which is why
`raft_env_lookup` returns `*const c_char` and Rust walks it, the same shape
`src/rrr/base/logging.rs:114` already uses. And `rusty::String::parse()`
returns `Ok` for `"-5"` and then **throws** `std::out_of_range` converting
to `u64` (`string.hpp:1036`), where real Rust returns `Err` — so the
obvious `raw.parse::<u64>()` would have reintroduced the exception it was
meant to remove.

*Strength: not a blocker.* What remains is a property of calling
application code, plus a deliberate choice about which regime to be correct
in.

### Claim 3: the constructor's two-step initialization

```cpp
async_callback_lifetime_ = std::make_shared<AsyncCallbackLifetime>();
heartbeat_interval_us_ = HEARTBEAT_INTERVAL;

async_callback_lifetime_->server = this;     // <-- needs `this`
```
The gate stores a back-pointer to its owner, so it cannot be built inside a
`fn new`. Four more statements in that constructor are of the same
character: `HEARTBEAT_INTERVAL` (a macro whose value depends on
`RAFT_TEST_CORO`, which the `RAFT_TEST` CMake option defines),
`EnsureLegacyRaftLogPayloadRegistered()`, an `#ifdef RAFT_TEST_CORO` region,
and a release-ordered `stop_.store`.

*Strength: weak.* Rust expresses construct-then-bind every day. This needs a
two-step API, not a language feature.

## 6. Where I would push if pushing harder

Ordered by (value / resistance). **These items overlap** — see item 5.

1. **`commo.cc`.** The largest hot-path file that is *entirely*
   hand-written: 325 lines, not one DSL block. (`server.cc` holds more
   hand-written C++ in total — 1506 lines, 385 of them in the unbroken
   region after the last GEN-END — but it is a mixed file with conversion
   underway; `commo.cc` has had none.) Blocked by none of the three claims.
   **But converting it does not reach Rust on the far side**: beneath it sit
   `communicator.{h,cc}` (292 lines, hand-written, zero DSL) and
   `rcc_rpc.h` (1496 lines of C++ generated by `bin/rpcgen`, not by
   rusty-cpp). The real stack is hand-written C++ → Python-generated C++ →
   rrr's Rust.
2. **Composition instead of inheritance** (claim 1a). Now known to cost only
   the twelve downcast sites — `RaftServer` supplies no vtable. The blast
   radius is twenty files, concentrated in `server.h` (264 references) and
   `server.cc` (155), then `test.cc` (48), `raft_worker.cc` (14),
   `raft_main_helper.cc` (13), plus regenerating the two extracted `.rs`
   crates. Large but not deep.
3. **`rusty::Vec`'s 48 bytes** — necessary but NOT sufficient, and the first
   draft called it "the single thing", contradicting its own claim 1b eight
   lines earlier. `rusty::Mutex`, `rusty::VecDeque`, `rusty::Condvar` and
   `rusty::Arc` are all in `RaftServerBase` and all unpinned. A rusty-cpp
   change, upstream of this repository.
4. **The wire types** (group a). Weaker resistance than first thought: the
   decode is a `type_index` comparison, not an RTTI downcast, and
   `SerializableEnvelope` is already a canonical Rust module. What is left
   is `janus::Command`'s subclass in `mako_commands.h` — shared with Paxos,
   but NOT with the benchmark. (`Marshallable` no longer exists;
   `__dep__.h:103-105` records its retirement.)
5. **The reactor kernels (group b) are NOT an independent lever.** Only two
   of the thirteen are pure naming gaps that a facade extension would fix,
   and even that means adding rustc-side models, not just C++ aliases — the
   `ReactorFiber` model has only `id` and `yields`, no `create_run`. The
   rest need a C++ lambda closure (5), `std::thread` construction (1),
   `std::this_thread`/`std::chrono` (2), methods on a deliberately opaque
   carrier (1), or the `RaftServer` downcast from claim 1a (2) — which makes
   those two the same work item as item 2.
