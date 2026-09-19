# What is Rust and what is C++ in Mako's Raft, and where the seam is

A map of the running system as it stands at `37d308ed4`, written so the
"impossible" claims in it can be attacked rather than believed.

## How to read this

Three kinds of code, marked everywhere below:

- **[RUST]** -- authored as Rust in a `#if RUSTYCPP_RUST` block. The
  transpiler renders it into the `/*RUSTYCPP:GEN-*/` region next to it, and
  THAT C++ is what compiles. **No Rust machine code is in the binary**:
  `nm build_rafttest/deptran_server | grep -c '_R'` is 0. The Rust is
  type-checked by rustc as a crate (`cargo build` + `clippy -D warnings` in
  the gate) and then thrown away.
- **[C++]** -- hand-written C++.
- **[EXT]** -- outside `src/deptran/raft` entirely: the rrr runtime, the
  wire types, the application state machine. Treated as an external call
  even where it happens to be Rust-authored itself.

A `>>>` line marks a language boundary crossing and names the mechanism.

## 1. The static picture: who owns what memory

```
class RaftServer                                       [C++]  361 lines, 0 data members
│   Exists ONLY to be a vtable. It inherits TxLogServer, and the rrr
│   service layer calls four methods on it BY NAME.
│
├── OnRequestVote / OnAppendEntries / OnInstallSnapshot / Start     [C++] shims
│   │   >>> plain call into a DSL free function; both sides are C++ after
│   │       transpilation, so this crossing costs nothing at runtime.
│   └── on_request_vote_body / on_append_entries_body / ...         [RUST]
│
├── InitializeSnapshotManager / GetSnapshotManager                  [C++]
│       try/catch + a lock returning std::shared_ptr
├── commo()                                                         [C++]
│       dynamic_cast<RaftCommo*>(commo_)
├── RaftServer()                                                    [C++]
│       THE TWO-STEP INIT. Stores `this` into the async-RPC gate.
├── ~RaftServer() { Shutdown(); }                                   [C++] one line
│
└── RaftServerBase                                       [RUST]  48 fields
    │   The whole of Raft's state. Every field below is declared in Rust.
    │
    ├── state_: RaftConsensusState                       [RUST]  guarded by mtx_
    │   ├── raft_log_: RaftLog                           [RUST]
    │   │   ├── base_, head_, len_: u64                  [RUST] scalars
    │   │   └── blocks_: Vec<Vec<RaftEntry>>             [RUST] the container
    │   │       └── RaftEntry                            [RUST]
    │   │           ├── term_: i64                       [RUST] <-- LEAF, Rust owns it
    │   │           └── cmd_: RaftCommand                [OPAQUE] <-- LEAF, C++ owns it
    │   │                 24 bytes of janus::Command. Rust can hold it,
    │   │                 move it, clone it (a refcount bump on the inner
    │   │                 Arc) and default it. Rust CANNOT look inside.
    │   ├── peers_: PeerTable                            [RUST]
    │   │   └── progress_: Vec<FollowerProgress>         [RUST]
    │   │       └── next_, match_: u64                   [RUST] <-- LEAF
    │   └── 16 scalars: current_term_, commit_index_, execute_index_,
    │       snapidx_, snapterm_, vote_for_, is_leader_, ...           [RUST] <-- LEAVES
    │
    ├── apply_queue_: Mutex<ApplyQueue>                   [RUST] the lock OWNS the data
    │   └── ApplyQueue                                    [RUST]
    │       ├── entries_: VecDeque<QueuedApplyEntry>      [RUST]
    │       │   └── QueuedApplyEntry { index_: u64, command_: RaftCommand, epoch_: u64 }
    │       └── epoch_: u64                               [RUST] <-- LEAF
    │
    ├── replication_wake_gate_: Arc<ReplicationWakeGate>  [RUST]
    │   └── ReplicationWakeGate                           [RUST] PhantomPinned
    │       ├── owner_: Mutex<Option<Arc<PollThread>>>            [RUST holding EXT]
    │       ├── waiter_, election_waiter_: Mutex<Option<Arc<IntEvent>>>  [RUST holding EXT]
    │       └── 6 AtomicBools                             [RUST] <-- LEAVES
    │
    ├── decoded_terms_: Vec<i64>                          [RUST] <-- LEAF
    ├── peer_sites_, config_members_: Vec<u16>            [RUST] <-- LEAVES
    ├── batch_buffer_: Vec<Arc<RaftTpcCommitCommand>>     [RUST container, OPAQUE element]
    ├── startup_finished_: Mutex<bool> + startup_cv_: Condvar    [RUST]
    ├── 11 atomics (stop_, rpc_ready_, looping_, ...)     [RUST] <-- LEAVES
    ├── 17 plain scalars                                  [RUST] <-- LEAVES
    │
    └── THE OPAQUE FIELDS -- Rust names the type and holds the bytes, C++
        defines what they mean. Sizes measured and pinned from both sides
        (static_assert in server.h, size_of/align_of in rusty-rustc/lib.rs):
        ├── mtx_: RaftCheckedMutex                  48B/8   [C++ type, RUST field]
        ├── state_machine_apply_mtx_: std::mutex     40B/8
        ├── apply_thread_: std::thread                8B/8
        ├── async_callback_lifetime_: shared_ptr     16B/8
        ├── snapshot_manager_: shared_ptr            16B/8
        ├── create_sm_snapshot_cb_: std::function    48B/16
        ├── prepare_sm_snapshot_cb_: std::function   48B/16
        ├── leader_change_cb_: std::function         48B/16
        ├── pending_apply_command_: janus::Command   24B/8
        ├── app_next_: LearnerAction                  -- the state machine hook
        └── commo_: *mut Communicator                 -- a raw pointer to [EXT]
```

## 2. The seam: 52 kernels, 559 lines

Every crossing from Rust into hand-written C++ goes through one of these.
Grouped by WHY it exists, which is the useful axis -- three of the five
groups are removable in principle and two are not.

### (a) Wire format and marshalling -- 11 kernels, 143 lines
```
raft_ae_decode_payload      raft_ae_apply_incoming     raft_batch_try_push
raft_batch_finalize         raft_append_response_read  raft_command_has_value
raft_command_kind           raft_append_entries_batch_max
raft_batch_optimization_enabled          raft_set_local_append
raft_append_leader_noop
```
`janus::Command` is a `SerializableEnvelope<MakoCommands>`: a kind tag plus
an `Arc<SerializableBase>` with a virtual `Marshal`/`Unmarshal` pair.
Decoding it means a `dynamic_cast` down a class hierarchy.
**NOT REMOVABLE without converting the wire types themselves**, which live
in `src/deptran/tpc_command.h` and are shared with Paxos and the benchmark.

### (b) The reactor -- 13 kernels, 59 lines
```
raft_spawn_heartbeat_loop   raft_spawn_election_timer   raft_spawn_election_timer_fiber
raft_spawn_apply_thread     raft_create_int_event       raft_queue_replication_wake
raft_queue_replication_shutdown_wake   raft_fiber_sleep_us
raft_thread_sleep_ms        raft_shutdown_barrier_yield
raft_new_replication_wake_gate         raft_apply_thread_join
raft_bind_replication_poll
```
Fiber creation, `std::thread` construction, `create_sp_int_event`, and
`PollThread::add`. **REMOVABLE IN PRINCIPLE** -- see section 6, this is the
softest of the five.

### (c) The snapshot manager -- 8 kernels, 134 lines
```
raft_snapshot_manager_latest   raft_snapshot_manager_load   raft_snapshot_manager_has_latest
raft_snapshot_recovery_pick_manager    raft_snapshot_serialize_and_save
raft_install_snapshot_payload  raft_load_state_machine_snapshot
raft_initialize_snapshot_manager
```
`janus::raft::SnapshotManager` is a C++ class hierarchy; the payloads are
`std::string`; installation is an abort-on-destruction transaction built on
`std::unique_ptr` and exceptions.
**NOT REMOVABLE while exceptions are the failure channel** -- the DSL has no
spelling for try/catch.

### (d) Application and configuration hooks -- 14 kernels, 122 lines
```
raft_apply_invoke            raft_fire_leader_change     raft_leader_change_cb_is_set
raft_prepare_snapshot_cb_is_set   raft_env_heartbeat_interval_us
raft_env_log_retention_window     raft_env_snapshot_interval
raft_env_snapshots_enabled        raft_election_timeouts
raft_load_current_config     raft_monotonic_now_us      raft_monotonic_now_secs
raft_clear_async_callback_owner   raft_setup_internal_guarded
```
Invoking a `std::function`, reading `getenv`, reading the clock.
**REMOVABLE IN PRINCIPLE**: a `std::function` is a fat pointer Rust could
hold behind a `Box`, and the env/clock reads are the legitimate "syscalls
that must be C" category -- and already consolidated (four getters became
one struct-returning call in `f8b83c4e2`).

### (e) The network path -- 5 kernels, 88 lines
```
raft_commo_set_network_enabled   raft_phase1_send_append
raft_phase1_load_and_send_snapshot     raft_broadcast_vote_and_wait
raft_vote_quorum_snapshot
```
These reach `commo()`, which is a `dynamic_cast` on a `TxLogServer` member.
**This is the group that touches the network.** See section 3.

## 3. The external edge: where the bytes actually leave

`commo()->Send*` is the boundary the question names, and it is further out
than the kernel list suggests.

```
[RUST] heartbeat_phase1_body  (server.cc, PHASE 1)
  │  decides: which follower, which entries, which term
  │
  >>> raft_phase1_send_append                                    [C++] 9 lines
      │
      └── RaftCommo::SendAppendEntries2(...)                     [EXT] commo.cc
          │   src/deptran/raft/commo.cc, hand-written C++, NOT a DSL carrier
          ├── create_sp_int_event(1)          -> Arc<IntEvent>   [EXT] rrr
          ├── PeerForSite(par_id, site_id)    -> a proxy or null
          ├── FutureAttr.callback = lambda capturing [response, site_id]
          └── proxy->async_AppendEntries(...)                    [EXT] rrr generated
              │
              └── rrr::Client                                    [EXT] src/rrr
                  ├── marshal into a wire buffer
                  └── write(2) on a TCP SOCKET fd  <-- LEAF: the real edge
```

And the receive side:

```
epoll_wait(2) on the PollThread's epoll fd  <-- LEAF: the real edge
  │   src/rrr/reactor/epoll_platform_linux.cc -- and note this file is an
  │   INLINE-RUST CARRIER. epoll_create is called from a Rust block.
  │
  └── rrr service dispatch                                       [EXT]
      └── RaftServiceImpl::AppendEntries (service.cc:92)          [C++]
          └── svr->OnAppendEntries(...)                           [C++] vtable
              >>> on_append_entries_body                          [RUST]
```

**The important thing this diagram shows**: the fds are not at the Rust/C++
boundary at all. `IntEvent` is a fiber counter with a wait-list, not an fd
(`src/rrr/reactor/reactor.rs:373`) -- it never touches the kernel. The only
real fds are the TCP sockets and the epoll instance, both inside rrr, and
**rrr's reactor is already canonical Rust** (`src/rrr/rust-modules.toml`
lists `reactor.rs`, `fiber.rs`, `future.rs`, `epoll_wrapper.rs`).

So the syscalls are already behind Rust-authored code. What sits between
Raft's Rust and rrr's Rust is `commo.cc` -- 325 lines of hand-written C++
that nobody has converted yet.

## 4. Concurrency: who touches which memory

**13 OS threads** in a five-site RaftLabTest process, counted from
`/proc/<pid>/task`. They are all named `deptran_server`, so the breakdown
below is from the code that spawns them, not from the thread names.

There is NO control-plane poll thread in either configuration measured here.
`RaftWorker::SetupHeartbeat` (`raft_worker.cc:484`) creates one --
`svr_hb_poll_thread_worker_g` -- but only when `Config::do_heart_beat()` is
true, which requires the `-b` flag, and neither `ci/ci.sh raftLabTest` nor
`examples/raft_bench.sh` passes it. When it does exist it serves
`ServerControlServiceImpl` on `site_port + 10000`: `server_ready`,
`server_shutdown`, `server_heart_beat` -- the benchmark driver's liveness
channel, nothing to do with Raft's AppendEntries heartbeat. The `_g` suffix
is vestigial; it is a per-worker member (`raft_worker.h:130`), not a global.

Worth noting as an inconsistency rather than a design: `ServerWorker` (the
Mako path) CLONES the server's poll thread for that service
(`server_worker.cc:13`), while `RaftWorker` and `PaxosWorker` each
`PollThread::create()` a second one.

```
MAIN THREAD
  └── Setup / Shutdown                    [RUST] via the C++ ctor/dtor

POLL THREAD (one per RaftWorker, plus one per client stub)
  │   Runs the epoll loop. Two Raft FIBERS are multiplexed onto it:
  ├── heartbeat fiber                     [RUST] heartbeat_loop_body
  │     └── HeartbeatDriver::run          [RUST] owns HeartbeatRoundState
  │           PHASE 0 -> 1 -> 2 -> 3      [RUST] all four
  │             takes mtx_, reads/writes state_, sends via (e)
  └── election fiber                      [RUST] ElectionTimerLoop::run
        takes mtx_, may campaign

  These two are FIBERS, not threads. They share one OS thread, which is why
  mtx_ cannot be recursive: std::recursive_mutex tracks ownership by
  thread::id and would hand the lock to a second fiber silently.

APPLY THREAD (one std::thread per server)
  └── ApplyThreadLoop                     [RUST]
        pops apply_queue_ (its own Mutex), then
        >>> raft_apply_invoke -> app_next_(id, cmd)   [EXT] the state machine

RPC HANDLER FIBERS (on whichever poll thread received)
  └── OnAppendEntries / OnRequestVote / OnInstallSnapshot
        take mtx_, write state_

LOCK ORDER, verified:
  callback_lifetime->mutex / startup_finished_
    -> state_machine_apply_mtx_
      -> mtx_
        -> apply_queue_
```

## 5. The three claims of impossibility, and exactly what backs each

These are the ones worth attacking. I have stated each as narrowly as the
evidence supports.

### Claim 1: `RaftServerBase` cannot be compiled by rustc and linked

Two independent reasons, and they have different strengths.

**1a. It is a C++ base class.** `class RaftServer : public RaftServerBase,
public TxLogServer`. Base-class layout, vtable injection, and the
`static_cast<RaftServer*>(base)` that 12 of the 52 kernels still perform are
C++ ABI features rustc cannot emit. *Strength: hard, but the premise is removable* --
`RaftServer` holds no data now, so composition plus an explicit back-pointer
would work. Cost: the downcast sites, and `RaftServer` must still supply the
`TxLogServer` vtable.

**1b. The layout does not match.** 24 of the 52 kernels dereference fields
of `RaftServerBase` directly (`self->state_.raft_log_`, `self->site_id_`). For
a rustc-compiled struct that requires byte-identical layout. The opaque
carriers now match (commit `26ca518a3`). What does not:
`rusty::Vec<T>` is **48 bytes in C++ against std::vec::Vec's 24 in Rust**,
and `rusty::Mutex`, `rusty::VecDeque`, `rusty::Condvar` and `rusty::Arc`
have their own differences. *Strength: hard today, but it is a rusty-cpp
problem, not a Raft one.*

**The bold alternative 1b does not need**: make the boundary NARROW instead
of making the layout match. `third-party/mako-redis` is the in-tree proof --
3,595 Rust symbols in `build/makoCon`, and the entire surface is five
`extern "C"` functions passing `#[repr(C)]` messages and pointer+length
pairs. Nobody agrees on a struct layout because no struct crosses. Raft
would have to stop reaching into fields and start passing values -- which is
the same direction as "fewer, wider boundary calls", already underway.

### Claim 2: exceptions have no DSL spelling

`try`/`catch` cannot be written in the DSL, and three places need it: the
apply callback, snapshot installation, and snapshot recovery. Each turns a
throwing application into a fail-stop rather than a half-updated replica.
*Strength: hard for the DSL as it stands.* A Rust `Result` at the boundary
with a C++ shim doing the catching would work and is not large -- the shims
already exist, they just also contain the logic.

### Claim 3: the constructor's two-step initialization

```cpp
async_callback_lifetime_ = std::make_shared<AsyncCallbackLifetime>();
async_callback_lifetime_->server = this;     // <-- needs `this`
```
The async-RPC gate stores a back-pointer to the server that owns it, so it
cannot be built inside a `fn new`: the object does not exist yet.
*Strength: weak.* Rust expresses this every day as construct-then-bind. It
needs a two-step API, not a new language feature. The same is true of
`heartbeat_interval_us_ = HEARTBEAT_INTERVAL` (a macro whose value depends
on `RAFT_TEST`) and `EnsureLegacyRaftLogPayloadRegistered()` (a global
registry call).

## 6. Where I would push if pushing harder

Ordered by (value / resistance), most promising first.

1. **`commo.cc`.** 325 lines of hand-written C++ sitting between Raft's Rust
   and rrr's Rust, converted by nobody. Everything below it is already
   Rust-authored. This is the largest single piece of C++ on the hot path
   and it is not blocked by any of the three claims.
2. **The reactor kernels (group b).** `Fiber::create_run` and
   `PollThread::add` are Rust-authored upstream. Thirteen kernels exist because
   Raft's DSL cannot NAME them, not because they are C++ -- the same
   `--type-map`-shaped gap that `rust_facade_types.h` works around for
   `IntEvent` and `PollThread`. Extending that facade is mechanical.
3. **Composition instead of inheritance** (claim 1a). `RaftServer` holds no
   data; this is now a rename-and-back-pointer exercise across `server.cc`,
   `test.cc`, `testconf.cc` and the worker -- large but not deep.
4. **`rusty::Vec`'s 48 bytes** (claim 1b). A rusty-cpp change, upstream of
   this repository, and the single thing standing between a matched layout
   and a rustc-compiled `RaftServerBase`.
5. **The wire types** (group a). Converting `janus::Command` and its
   `Marshallable` hierarchy would remove the largest remaining kernel group
   -- but it is shared with Paxos and the benchmark, so its blast radius is
   the whole of `src/deptran`.
