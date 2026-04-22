# Decoupling Raft from rrr/rpc and rocksdb

**Status:** in-progress
**Owner:** Shuai / Claude
**Started:** 2026-04-22

## Goal

Make `janus::raft::RaftServer` usable without rrr/rpc (no sockets bound,
no fibers) and without rocksdb (no on-disk state). The lab-style
correctness tests in `src/deptran/raft/test.cc` should run as a single
in-process binary that wires N `RaftNode`s together with an in-memory
channel transport and an in-memory log/snapshot store.

## Rusty-safety constraints (MANDATORY)

These apply to **every** new file and every modification this plan
introduces. No exceptions.

1. **No inheritance / virtual functions.** Use `pro::facade_builder` +
   `pro::proxy<Facade>` for polymorphism, the same idiom used by
   `MarshallableFacade` / `MarshallableProxy` in `src/rrr/misc/marshal.hpp`.
   Define per-method tags with `PRO_DEF_MEM_DISPATCH` and chain them into
   a facade. Adapters provide the concrete implementations and wrap the
   owned state.
2. **No raw pointers for ownership.** Single owner → `rusty::Box<T>`.
   Shared → `rusty::Arc<T>`. Weak → the in-tree `rusty::sync::Weak<T>`
   wrapper. Raw pointers only across boundaries with a legacy
   non-rusty caller, marked `@unsafe { ... }`.
3. **No std smart pointers in new code.** `std::shared_ptr` /
   `std::unique_ptr` / `std::weak_ptr` are banned in new code. If an
   existing rrr module interface requires them at the boundary, convert
   at the edge.
4. **No std containers in new code.** `rusty::Vec`, `rusty::HashMap`,
   `rusty::HashSet`, `rusty::BTreeMap`. If a proxy facade demands a
   specific std type (e.g. `std::shared_ptr<Marshallable>` from
   rrr-module boundary), isolate the conversion in one spot and mark
   it `@unsafe`.
5. **No `std::optional`, `std::function`, `std::mutex`.**
   `rusty::Option<T>`, `rusty::Function<Sig>`, `rusty::Mutex<T>` (or
   `rusty::sync::Mutex<T>`). Callbacks from the transport use
   `rusty::Function<void(...)>`.
6. **No `std::thread` in new files.** `rusty::thread::spawn` +
   `rusty::thread::JoinHandle<void>` — the pattern already used in
   `src/rrr/misc/recorder.cpp`.
7. **Every new function annotated `@safe` or `@unsafe`.** Blocks that
   call unannotated code use `// @unsafe { ... }`.

## Surface area (audited)

| Concern                  | Today                                                                                   | Abstracted?        |
| ------------------------ | --------------------------------------------------------------------------------------- | ------------------ |
| Log persistence          | `RocksDBLogStorage`, `MemoryLogStorage` behind `janus::raft::LogStorage` (virtual)      | ✅, but needs proxy port |
| Snapshot persistence     | `FileSnapshotManager` behind `SnapshotManager` (virtual)                                | ✅, but needs proxy port + `MemorySnapshotManager` |
| Outbound RPC (8 methods) | `RaftCommo` + rrr `async_*` proxies (`RaftProxy`), `rrr::FutureAttr`, `PollThread`      | ❌                 |
| Inbound RPC (7 handlers) | `RaftService` registered via `rrr::Server::reg_service_proxy`, `rrr::DeferredReply`      | ❌                 |
| Timers / scheduling      | `rrr::Fiber`, `Reactor::create_sp_event<TimeoutEvent>`, `PollThread`                     | ❌ (deferred)      |

### Outbound RPC methods to abstract

From `src/deptran/raft/commo.h`:

1. `SendAppendEntries(site, term, ...)` and
   `SendAppendEntries2(site, ...)`
2. `BroadcastVote(par_id, term, candidate_id, last_log_idx, last_log_term, ...)`
3. `SendTimeoutNow(site_id, target_term)`
4. `SendVoteDurable(candidate_id, voter_id, term)`
5. `SendAppendEntriesDurable(leader_id, follower_id, term, last_log_idx)`
6. `SendNotifyRestart(self_id, par_id)`
7. `SendInstallSnapshot(site_id, term, ...)`
8. `UpdatePartitionView(par_id, view_data)` (gossip; may drop for tests)

### Inbound RPC handlers to abstract

From `src/deptran/raft/service.h` (`RaftServiceImpl::Handle*`):

1. `HandleAppendEntries`
2. `HandleVote`
3. `HandleTimeoutNow`
4. `HandleVoteDurable`
5. `HandleAppendEntriesDurable`
6. `HandleNotifyRestart`
7. `HandleInstallSnapshot`

## Plan

### Phase 0 — typed request/response structs (≤1 day)

Move RPC payload structs out of rrr-generated types into plain C++
headers under `src/deptran/raft/messages.hpp`:

```cpp
namespace janus::raft {

struct AppendEntriesReq { /* term, leader_id, prev_log_idx, prev_log_term,
                             entries, leader_commit, ... */ };
struct AppendEntriesReply { /* term, success, ack_type, ... */ };

struct VoteReq { /* term, candidate_id, last_log_idx, last_log_term */ };
struct VoteReply { /* term, vote_granted */ };

struct InstallSnapshotReq { /* ... */ };
struct InstallSnapshotReply { /* ... */ };

// ... one pair per RPC type
}
```

Many of these structs already half-exist in `commo.h`
(`AppendEntriesResponse`, `SendAppendEntriesResults`). Finish the job.
No `std::shared_ptr`; entries carry `rusty::Arc<LogEntry>` or owned
`rusty::Vec<LogEntry>` as appropriate.

### Phase 1 — `TransportProxy` facade (≤1 day)

`src/deptran/raft/transport.hpp`:

```cpp
PRO_DEF_MEM_DISPATCH(TrSendAppendEntries, send_append_entries);
PRO_DEF_MEM_DISPATCH(TrBroadcastVote,     broadcast_vote);
PRO_DEF_MEM_DISPATCH(TrSendTimeoutNow,    send_timeout_now);
PRO_DEF_MEM_DISPATCH(TrSendVoteDurable,   send_vote_durable);
PRO_DEF_MEM_DISPATCH(TrSendAppendDurable, send_append_durable);
PRO_DEF_MEM_DISPATCH(TrSendNotifyRestart, send_notify_restart);
PRO_DEF_MEM_DISPATCH(TrSendInstallSnap,   send_install_snapshot);

struct TransportFacade : pro::facade_builder
    ::add_convention<TrSendAppendEntries,
        void(siteid_t, AppendEntriesReq,
             rusty::Function<void(siteid_t, AppendEntriesReply)>)>
    ::add_convention<TrBroadcastVote,
        void(parid_t, VoteReq,
             rusty::Function<void(VoteReply)>)>
    ::add_convention<TrSendTimeoutNow,
        void(siteid_t, TimeoutNowReq)>
    ::add_convention<TrSendVoteDurable,
        void(siteid_t, VoteDurableReq)>
    ::add_convention<TrSendAppendDurable,
        void(siteid_t, AppendDurableReq)>
    ::add_convention<TrSendNotifyRestart,
        void(siteid_t, parid_t)>
    ::add_convention<TrSendInstallSnap,
        void(siteid_t, InstallSnapshotReq,
             rusty::Function<void(InstallSnapshotReply)>)>
    ::build {};

using TransportProxy = pro::proxy<TransportFacade>;
```

`RaftServer` stores a `TransportProxy transport_;` member (the proxy
owns its adapter). No inheritance; polymorphism is via the proxy.

### Phase 2 — `RrrTransport` adapter (≤1 day)

Move the body of every current `RaftCommo::Send*` / `Broadcast*` method
into a new `RrrTransportAdapter` class with matching method signatures
(plain C++, no inheritance). Then:

```cpp
inline TransportProxy make_rrr_transport(rusty::Arc<RrrTransportAdapter> a) {
  return pro::make_proxy<TransportFacade>(std::move(a));
}
```

Production wiring (`RaftWorker::SetupCommo`) constructs the adapter
and calls `make_rrr_transport`. `RaftCommo` either becomes the adapter
or is retired. The proxy facade forbids accidentally calling a
transport method with `rrr::Future` in its signature.

### Phase 3 — `DispatcherProxy` for inbound (≤1 day)

Mirror structure:

```cpp
PRO_DEF_MEM_DISPATCH(DpHandleAppendEntries, handle_append_entries);
// ... one per RPC handler

struct DispatcherFacade : pro::facade_builder
    ::add_convention<DpHandleAppendEntries,
        void(AppendEntriesReq,
             rusty::Function<void(AppendEntriesReply)>)>
    // ...
    ::build {};

using DispatcherProxy = pro::proxy<DispatcherFacade>;
```

`RaftServer` exposes a `dispatcher()` method that returns a
`DispatcherProxy` view of itself. `RaftService` (the rrr receiver)
becomes a thin shim: it unmarshals the rrr request, calls the
dispatcher, and supplies a reply-callback that marshals the reply
back through `rrr::DeferredReply`. No other caller of `RaftService`
needs rrr knowledge.

### Phase 4 — `ChannelTransport` / `ChannelDispatcher` (≤1.5 days)

`src/deptran/raft/channel_transport.hpp`:

```cpp
namespace janus::raft {

// One variant per RPC kind + reply callback, send-trait-annotated.
struct Envelope {
  // ... kind, source, dest, payload variant,
  //     rusty::Function<void(ReplyVariant)> on_reply
};
REGISTER_RUSTY_SEND(Envelope);

class ChannelSwitchboard {
  // N × rusty::sync::mpsc::channel<Envelope>, one per site.
  // Public ops: send(dst, env), drain(self, handler).
  // Fault-injection hooks:
  //   drop_direction(siteid_t from, siteid_t to);
  //   partition(rusty::HashSet<siteid_t> group_a,
  //             rusty::HashSet<siteid_t> group_b);
  //   reset();
};

// Adapter exposed via TransportProxy.
class ChannelTransportAdapter {
 public:
  ChannelTransportAdapter(siteid_t self,
                          rusty::Arc<ChannelSwitchboard> sw);
  void send_append_entries(...);  // pushes an Envelope to sw_
  // ... etc
};
}
```

Per-node worker thread drains its receiver side and invokes the
sibling `DispatcherProxy`. Replies are delivered via the callback
stored in the Envelope.

### Phase 5 — `MemorySnapshotManager` (≤0.5 day)

Parallel to `MemoryLogStorage`. `rusty::HashMap<slotid_t,
rusty::Vec<u8>>`. Move both behind a proxy facade (`StorageFacade`) so
server code holds `LogStorageProxy` / `SnapshotManagerProxy` and the
virtual-inheritance versions can be retired.

### Phase 6 — `RaftNode` facade (≤0.5 day)

```cpp
class RaftNode {
 public:
  RaftNode(siteid_t id,
           TransportProxy transport,
           LogStorageProxy log_storage,
           SnapshotManagerProxy snap_manager,
           RaftConfig cfg);

  DispatcherProxy dispatcher();
  // Inspection helpers for tests:
  bool is_leader() const;
  slotid_t commit_index() const;
  ballot_t current_term() const;
  // ...
 private:
  RaftServer server_;  // owned, no inheritance exposed
};
```

Test cluster builder:

```cpp
class TestCluster {
 public:
  static TestCluster with_in_memory_transport(size_t n);
  RaftNode& node(siteid_t);
  ChannelSwitchboard& switchboard();
  void kill(siteid_t);
  void restart(siteid_t);
  void disconnect(siteid_t);
  void reconnect(siteid_t);
  void partition(rusty::Vec<siteid_t> group_a,
                 rusty::Vec<siteid_t> group_b);
};
```

### Phase 7 — port `RaftLabTest` to the in-memory harness (1–2 days)

`RaftTestConfig` gets a new constructor
`RaftTestConfig(TestCluster&)`. All `Kill`/`Restart`/`Disconnect` calls
route through `TestCluster` instead of through frames and sockets. A
new standalone binary `raft_lab_standalone` runs
`RaftLabTest::Run()` against the in-memory cluster — no
`deptran_server`, no config yaml, no rocksdb on disk.

### Phase 8 (deferred) — clock / timer abstraction

The `Reactor`/`Fiber` coupling is orthogonal; tackle it separately
once phases 0–7 prove the shape is right. Approach: abstract
`RaftClock` with a `ManualClock` implementation tests can advance.

## Invariants & non-goals

- **Non-goal:** changing wire format. `RrrTransport` keeps exact wire
  compatibility with the current code. Only the C++ API shape changes.
- **Non-goal:** removing rrr from production. rrr remains the production
  transport; we just hide it behind a proxy so the test path can
  substitute.
- **Invariant:** production throughput must not regress. All phases
  are refactors or additions; the production path keeps calling the
  same adapter code (now reached through a proxy pointer hop).

## File map

```
src/deptran/raft/
├── messages.hpp          (new, phase 0) — plain C++ RPC payload structs
├── transport.hpp         (new, phase 1) — TransportFacade + TransportProxy
├── rrr_transport.hpp/.cc (new, phase 2) — current commo body, renamed
├── dispatcher.hpp        (new, phase 3) — DispatcherFacade + DispatcherProxy
├── channel_transport.hpp/.cc (new, phase 4)
├── snapshot_manager_proxy.hpp (new, phase 5)
├── memory_snapshot_manager.hpp (new, phase 5)
├── raft_node.hpp/.cc     (new, phase 6)
├── test_cluster.hpp/.cc  (new, phase 6)
├── commo.cc/.h           (phase 2: delete or shrink to RrrTransport thunks)
├── service.cc/.h         (phase 3: shrink to rrr→dispatcher shim)
├── server.cc/.h          (phases 1, 3: replace commo_/RaftProxy with TransportProxy)
├── testconf.cc/.h        (phase 7: accept TestCluster)
└── test.cc/.h            (phase 7: standalone-binary entrypoint)
```

## Completion criteria

1. `make -j` / `cmake --build` is green (both the decoupled lab test
   binary and the production `deptran_server`).
2. `src/deptran/raft/server.{h,cc}` contains zero references to
   `rrr::Future`, `rrr::DeferredReply`, `FutureAttr`, `RaftProxy`.
3. `raft_lab_standalone` runs `RaftLabTest::Run()` end-to-end without
   binding any socket (verify with `ss -lntp | grep raft_lab_standalone`
   → no output).
4. `shard1ReplicationRaft` (production path) still hits ≥80k ops/sec
   on this host (same as before the refactor).
5. No `std::shared_ptr` / `std::unique_ptr` / `std::function` /
   `std::thread` / `std::vector` / `std::mutex` / `virtual` appears in
   any new file introduced by this plan. (Exceptions: proxy adapters
   that have to bridge to existing rrr/rocksdb types — each conversion
   site annotated `@unsafe` with justification.)

## Tracking

Phases completed will be checked off as commits land.

- [x] Phase 0 — `messages.hpp`                             (f4356b6c1)
- [x] Phase 1 — `TransportProxy`                           (ef79abcd1)
- [x] Phase 2 — `RrrTransport` adapter (fire-and-forget)   (668d2ba84)
- [x] Phase 2.5 — callback-shaped quorum RPCs on RaftCommo +
      full RrrTransportAdapter                            (6cf3dfd21)
- [x] Phase 3 — `DispatcherProxy` facade                   (4c6a1b102)
- [x] Phase 4 — `ChannelTransport` + `Switchboard`         (2b39593bf)
- [x] Phase 5 — `MemorySnapshotManager`                    (b97aff1e6)
- [x] Phase 6 — `RaftNode` + `TestCluster` (skeleton)      (b288a2bca)
- [x] Phase 7 — `raft_lab_standalone` (skeleton)

### Remaining follow-ups (deferred)

- [ ] Phase 3.5 — wire `RaftServiceImpl` to call through
      `DispatcherProxy` (swap `rrr::DeferredReply` for the callback
      shape). Currently the proxy is compile-verified but unused by
      production handlers.
- [ ] Phase 5.5 — retire the virtual `LogStorage` /
      `SnapshotManager` interfaces in favor of `LogStorageProxy` /
      `SnapshotManagerProxy` facades. Requires threading the proxy
      types through `server.cc` where both are currently referenced
      by virtual pointer.
- [ ] Phase 6.5 — replace `DummyDispatcher` inside `RaftNode` with a
      real `RaftServer`-backed dispatcher. Requires first decoupling
      `RaftServer` from `rrr::PollThread` / `rrr::Fiber` (the outbound
      transport abstraction is done; the fiber/timer abstraction is
      Phase 8).
- [ ] Phase 7.5 — port `RaftLabTest::Run()` to drive `TestCluster`
      once Phase 6.5 is in. Today `raft_lab_standalone` only exercises
      the transport + fault plumbing.
