# Decoupling Raft from srpc/rpc and rocksdb

**Status:** in-progress
**Owner:** Shuai / Claude
**Started:** 2026-04-22

## Goal

Make `janus::raft::RaftServer` usable without srpc/rpc (no sockets bound,
no fibers) and without rocksdb (no on-disk state). The lab-style
correctness tests in `src/deptran/raft/test.cc` should run as a single
in-process binary that wires N `RaftNode`s together with an in-memory
channel transport and an in-memory log/snapshot store.

## Rusty-safety constraints (MANDATORY)

These apply to **every** new file and every modification this plan
introduces. No exceptions.

1. **No inheritance / virtual functions.** Use `pro::facade_builder` +
   `pro::proxy<Facade>` for polymorphism, the same idiom used by
   `MarshallableFacade` / `MarshallableProxy` in `src/srpc/misc/marshal.hpp`.
   Define per-method tags with `PRO_DEF_MEM_DISPATCH` and chain them into
   a facade. Adapters provide the concrete implementations and wrap the
   owned state.
2. **No raw pointers for ownership.** Single owner → `rusty::Box<T>`.
   Shared → `rusty::Arc<T>`. Weak → the in-tree `rusty::sync::Weak<T>`
   wrapper. Raw pointers only across boundaries with a legacy
   non-rusty caller, marked `@unsafe { ... }`.
3. **No std smart pointers in new code.** `std::shared_ptr` /
   `std::unique_ptr` / `std::weak_ptr` are banned in new code. If an
   existing srpc module interface requires them at the boundary, convert
   at the edge.
4. **No std containers in new code.** `rusty::Vec`, `rusty::HashMap`,
   `rusty::HashSet`, `rusty::BTreeMap`. If a proxy facade demands a
   specific std type (e.g. `std::shared_ptr<Marshallable>` from
   srpc-module boundary), isolate the conversion in one spot and mark
   it `@unsafe`.
5. **No `std::optional`, `std::function`, `std::mutex`.**
   `rusty::Option<T>`, `rusty::Function<Sig>`, `rusty::Mutex<T>` (or
   `rusty::sync::Mutex<T>`). Callbacks from the transport use
   `rusty::Function<void(...)>`.
6. **No `std::thread` in new files.** `rusty::thread::spawn` +
   `rusty::thread::JoinHandle<void>` — the pattern already used in
   `src/srpc/misc/recorder.cpp`.
7. **Every new function annotated `@safe` or `@unsafe`.** Blocks that
   call unannotated code use `// @unsafe { ... }`.

## Surface area (audited)

| Concern                  | Today                                                                                   | Abstracted?        |
| ------------------------ | --------------------------------------------------------------------------------------- | ------------------ |
| Log persistence          | `RocksDBLogStorage`, `MemoryLogStorage` behind `janus::raft::LogStorage` (virtual)      | ✅, but needs proxy port |
| Snapshot persistence     | `FileSnapshotManager` behind `SnapshotManager` (virtual)                                | ✅, but needs proxy port + `MemorySnapshotManager` |
| Outbound RPC (8 methods) | `RaftCommo` + srpc `async_*` proxies (`RaftProxy`), `srpc::FutureAttr`, `PollThread`      | ❌                 |
| Inbound RPC (7 handlers) | `RaftService` registered via `srpc::Server::reg_service_proxy`, `srpc::DeferredReply`      | ❌                 |
| Timers / scheduling      | `srpc::Fiber`, `Reactor::create_sp_event<TimeoutEvent>`, `PollThread`                     | ❌ (deferred)      |

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

Move RPC payload structs out of srpc-generated types into plain C++
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

### Phase 2 — `SrpcTransport` adapter (≤1 day)

Move the body of every current `RaftCommo::Send*` / `Broadcast*` method
into a new `SrpcTransportAdapter` class with matching method signatures
(plain C++, no inheritance). Then:

```cpp
inline TransportProxy make_srpc_transport(rusty::Arc<SrpcTransportAdapter> a) {
  return pro::make_proxy<TransportFacade>(std::move(a));
}
```

Production wiring (`RaftWorker::SetupCommo`) constructs the adapter
and calls `make_srpc_transport`. `RaftCommo` either becomes the adapter
or is retired. The proxy facade forbids accidentally calling a
transport method with `srpc::Future` in its signature.

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
`DispatcherProxy` view of itself. `RaftService` (the srpc receiver)
becomes a thin shim: it unmarshals the srpc request, calls the
dispatcher, and supplies a reply-callback that marshals the reply
back through `srpc::DeferredReply`. No other caller of `RaftService`
needs srpc knowledge.

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

- **Non-goal:** changing wire format. `SrpcTransport` keeps exact wire
  compatibility with the current code. Only the C++ API shape changes.
- **Non-goal:** removing srpc from production. srpc remains the production
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
├── srpc_transport.hpp/.cc (new, phase 2) — current commo body, renamed
├── dispatcher.hpp        (new, phase 3) — DispatcherFacade + DispatcherProxy
├── channel_transport.hpp/.cc (new, phase 4)
├── snapshot_manager_proxy.hpp (new, phase 5)
├── memory_snapshot_manager.hpp (new, phase 5)
├── raft_node.hpp/.cc     (new, phase 6)
├── test_cluster.hpp/.cc  (new, phase 6)
├── commo.cc/.h           (phase 2: delete or shrink to SrpcTransport thunks)
├── service.cc/.h         (phase 3: shrink to srpc→dispatcher shim)
├── server.cc/.h          (phases 1, 3: replace commo_/RaftProxy with TransportProxy)
├── testconf.cc/.h        (phase 7: accept TestCluster)
└── test.cc/.h            (phase 7: standalone-binary entrypoint)
```

## Completion criteria

1. `make -j` / `cmake --build` is green (both the decoupled lab test
   binary and the production `deptran_server`).
2. `src/deptran/raft/server.{h,cc}` contains zero references to
   `srpc::Future`, `srpc::DeferredReply`, `FutureAttr`, `RaftProxy`.
3. `raft_lab_standalone` runs `RaftLabTest::Run()` end-to-end without
   binding any socket (verify with `ss -lntp | grep raft_lab_standalone`
   → no output).
4. `shard1ReplicationRaft` (production path) still hits ≥80k ops/sec
   on this host (same as before the refactor).
5. No `std::shared_ptr` / `std::unique_ptr` / `std::function` /
   `std::thread` / `std::vector` / `std::mutex` / `virtual` appears in
   any new file introduced by this plan. (Exceptions: proxy adapters
   that have to bridge to existing srpc/rocksdb types — each conversion
   site annotated `@unsafe` with justification.)

## Tracking

Phases completed will be checked off as commits land.

- [x] Phase 0 — `messages.hpp`                             (f4356b6c1)
- [x] Phase 1 — `TransportProxy`                           (ef79abcd1)
- [x] Phase 2 — `SrpcTransport` adapter (fire-and-forget)   (668d2ba84)
- [x] Phase 2.5 — callback-shaped quorum RPCs on RaftCommo +
      full SrpcTransportAdapter                            (6cf3dfd21)
- [x] Phase 3 — `DispatcherProxy` facade                   (4c6a1b102)
- [x] Phase 4 — `ChannelTransport` + `Switchboard`         (2b39593bf)
- [x] Phase 5 — `MemorySnapshotManager`                    (b97aff1e6)
- [x] Phase 6 — `RaftNode` + `TestCluster` (skeleton)      (b288a2bca)
- [x] Phase 7 — `raft_lab_standalone` (skeleton)

### Related work already landed

- `raft: replace DeferredReply with Fiber-wrapped auto-reply`        (36766c1f0)
- `raft: flip 10 RPCs from defer to fiber; remove RpcHandler macro`  (1ea323cfb)
   — every raft RPC is now declared `fiber` in `src/deptran/rcc_rpc.rpc`;
   the generated wrapper launches `Fiber::create_run` and marshals the
   returned response struct. `RaftServiceImpl` overrides take a request
   struct and return `Result<Resp, srpc::i32>` directly.
- `raft: snapshot trigger in StartApplyThread (fixes TEST 55)`       (31dc57a37)
   — unrelated snapshot bug fix.

## Plan for the remaining work (phases 8.x)

The phase 0–7 work shipped the callback-shaped facades in
`transport.hpp` / `dispatcher.hpp` and the channel transport that feeds
them. That was the right shape before the inbound DeferredReply → fiber
migration; it is no longer. With `fiber` RPCs in place, both inbound
and outbound should speak the same synchronous-return shape, using
`srpc::Future::wait` (srpc side) or `IntEvent::wait` (channel side) to
yield the caller's fiber until a reply arrives. That uniformity
eliminates the `RaftQuorum` primitive the earlier plan needed, removes
every `rusty::Function<void(...)>` reply callback from raft code, and
makes every subsequent phase smaller.

### Phase 8.0 — collapse facades to fiber-synchronous

One commit, ~400 LOC across existing phase headers + tests. Nothing in
`RaftServer` yet — this is a pure refactor of the facades from phases
1 / 3 / 4.

- `transport.hpp` — every quorum/reply-expecting `send_*` returns its
  reply type (or `rusty::Result<Reply, srpc::i32>` to match the srpc
  codegen convention). Fire-and-forget methods
  (`send_vote_durable`, `send_append_entries_durable`,
  `send_notify_restart`) stay `void`. Delete every `OnXReply` typedef.
- `dispatcher.hpp` — every `handle_*` returns its reply type. Delete
  every `OnXReplyDispatch` typedef.
- `srpc_transport.hpp` — each reply-expecting method becomes a thin
  wrapper that blocks on the srpc Future. The RaftCommo `*Cb` variants
  added in phase 2.5 either get synchronous counterparts or get
  rewritten to use `Future::wait` and return the reply. No
  `std::shared_ptr` holder bridging.
- `channel_transport.hpp` — `Envelope` now carries
  `{from, to, rusty::Function<Reply(DispatcherProxy&)> run_and_return,
    Reply* out_slot, rusty::Arc<IntEvent> ready}`. Sender pushes, then
  `ready->Wait()` (yields the fiber); worker pops, calls
  `*out_slot = run_and_return(disp);`, `ready->set(1)`. Sender's fiber
  resumes with the reply.
- Tests: `test_raft_transport_facade`, `test_raft_dispatcher_facade`,
  `test_raft_channel_transport` — replace "call then wait for
  callback" assertions with "call, check returned value".
- `raft_node.hpp::DummyDispatcher` — each method returns a Reply
  struct; delete the callback-calling code.
- Gate: all existing phase gtests pass + `deptran_server -f
  config/raft_lab_test.yml` still passes (no production path touched).

### Phase 8.1 — route outbound through `TransportProxy`

1–2 commits, ~800 LOC changed in `server.cc`.

- Add `TransportProxy transport_` on `RaftServer`; initialize in
  `Setup()` as `make_srpc_transport(commo_, site_id_, partition_id_)`.
  Production path unchanged underneath.
- Replace every `commo()->SendX(...)` with `transport_->send_x(...)`.
  Because the facade is now fiber-synchronous, the leader's election
  and replication fibers stop using `QuorumEvent::Wait()`: a vote
  broadcast becomes N sub-fibers (`Fiber::create_run` × peers), each
  blocking on `send_vote`, with a shared atomic counter signalling
  quorum; append-entries replication is per-peer linear code that
  blocks on the reply.
- Delete `SendAppendEntriesResults` and `RaftVoteQuorumEvent`.
- Gates: phase gtests + `raft_lab_test.yml` + throughput runs
  (`shard1ReplicationRaft` ≥ 80k ops/sec).

### Phase 8.2 — `RaftServerDispatcher`

1 commit, ~300 LOC.

- New `RaftServerDispatcher` adapter holding `RaftServer*` and
  satisfying `DispatcherFacade`. Each `handle_X` allocates a local
  `Reply`, calls the existing `RaftServer::OnX(...)` with
  output-pointer args, returns the reply.
- Factory `make_raft_server_dispatcher(RaftServer*) -> DispatcherProxy`.
- Unit test analogous to `test_raft_dispatcher_facade`.

### Phase 8.3 — `RaftServiceImpl` forwards to `DispatcherProxy`

1 commit, ~150 LOC changed.

- `RaftServiceImpl` builds a `DispatcherProxy` via
  `make_raft_server_dispatcher(svr)` in its constructor.
- Each `RaftServiceImpl::Vote(req)` etc. becomes:
  `return Ok(dispatcher_->handle_vote(req));`.
- Gate: `raft_lab_test.yml` still passes.

### Phase 8.4 — storage proxies (optional)

1 commit, ~400 LOC.

- Define `LogStorageFacade` / `SnapshotManagerFacade` matching the
  existing virtual methods.
- Thread them through `RaftServer` — the field types move from
  `std::shared_ptr<LogStorage>` + `std::shared_ptr<SnapshotManager>`
  to the proxies.
- Deferrable if time is short: the existing virtual interfaces are
  already used by `MemoryLogStorage` / `MemorySnapshotManager`.

### Phase 8.5 — `TestCluster` runs real `RaftServer`s

1–2 commits, ~500 LOC.

- `RaftNode` constructs a real `RaftServer` given a `TransportProxy`
  pointing at `ChannelTransportAdapter`, a `MemoryLogStorage`, and a
  `MemorySnapshotManager`. The server is wired to the switchboard via
  `make_raft_server_dispatcher(server)`.
- Bring up the server's timers / fibers
  (`StartElectionTimer`, `HeartbeatLoop`, `StartApplyThread`) exactly
  as `deptran_server` does today. `srpc::Reactor` is still loaded; no
  sockets are involved.
- Verify with small gtest cases: election converges, `DoAgreement`
  commits across all nodes, `disconnect(follower)` silences AEs.

### Phase 8.6 — port `RaftTestConfig` to `TestCluster`

1–2 commits, ~600 LOC.

- `Kill(i)` → destroy `nodes_[i]`'s server; switchboard drops its
  traffic.
- `Restart(i)` → rebuild server in place, re-register its dispatcher.
- `Disconnect(i)` → `sw_.drop_direction(i, *)` + `sw_.drop_direction(*, i)`.
- `Reconnect(i)` → per-pair undrop (small switchboard API addition).
- `Partition(a, b)` → `sw_.partition({a, b})`.
- `DoAgreement(cmd, n, wait)` → call leader's log-append path, poll
  `commit_index()` across nodes.
- `OneLeader()` → scan nodes for `is_leader()`.
- Keep the existing srpc-based path under a build flag so
  `deptran_server -f raft_lab_test.yml` continues to work.

### Phase 8.7 — `raft_lab_standalone` runs the full `RaftLabTest::Run()`

1 commit, ~100 LOC.

- Replace the current 4-case skeleton with construction of a 5-node
  `TestCluster` + `RaftTestConfig(cluster)` + `RaftLabTest::Run()`.
- Completion criterion (matches the plan's original goal): the full
  lab suite passes and `ss -lntp | grep raft_lab_standalone` prints
  nothing.

### Phase 8.8 (deferred) — `RaftClock` abstraction

Only needed if deterministic scheduling becomes a goal. A `ManualClock`
lets tests advance time explicitly. The real-time Fiber path works
without it for a first pass.

### Ordering and risk

| Step | Depends on | Gate |
|---|---|---|
| 8.0 | — | phase gtests + `raft_lab_test.yml` |
| 8.1 | 8.0 | phase gtests + `raft_lab_test.yml` + `shard1ReplicationRaft` ≥ 80k |
| 8.2 | 8.1 | unit test |
| 8.3 | 8.2 | `raft_lab_test.yml` |
| 8.4 | — (orthogonal) | unit + `raft_lab_test.yml` |
| 8.5 | 8.1, 8.2 | TestCluster gtest |
| 8.6 | 8.5 | subset of `RaftLabTest` |
| 8.7 | 8.6 | full `RaftLabTest` + `ss` verification |

Highest risk: Phase 8.1. Every reply now yields the calling fiber via
`Future::wait` / `IntEvent::wait`; we need to audit every site that
holds `mtx_` across a send-and-wait for deadlocks (fiber yields while
holding a recursive mutex are safe on the same thread but risky if
replies dispatch on a different thread). Phase 8.0 should establish
the fiber-yield pattern in the transport facade tests so the shape is
validated before it ships into production server code.

## Rusty-safety in the remaining work

Every new file this plan introduces must follow the rusty-safety
constraints at the top of this document (no inheritance, no std smart
pointers / containers / mutex / thread / optional / function, every
function annotated `@safe` or `@unsafe`). Beyond new code:

- Prefer `rusty::Vec` / `rusty::HashMap` / `rusty::HashSet` /
  `rusty::BTreeMap` over `std::vector` / `std::unordered_*` / `std::map`.
- Prefer `rusty::Arc` / `rusty::Box` / `rusty::Rc` over their std
  counterparts.
- Prefer `rusty::Mutex` / `rusty::RefCell` / `rusty::Cell` over
  `std::mutex` + raw mutable fields.
- Prefer `rusty::Function<Sig>` over `std::function<Sig>`.
- Prefer `rusty::thread::spawn` over `std::thread`.
- Prefer `rusty::Option<T>` over `std::optional<T>`.

When touching existing code as part of a phase 8.x step, migrate any
std constructs that show up in the blast radius of your change to their
rusty equivalents if it's safe to do so. Log each migration in the
commit message. Do not attempt to migrate code outside the step's
scope — that stretches review surface and makes bisection harder.

## Tracking (phase 8)

- [x] Phase 8.0 — collapse facades to fiber-synchronous  (cf5db3fef)
- [ ] Phase 8.1 — route outbound RPCs through `TransportProxy`
- [ ] Phase 8.2 — `RaftServerDispatcher`
- [ ] Phase 8.3 — `RaftServiceImpl` → `DispatcherProxy`
- [ ] Phase 8.4 — storage proxies (optional)
- [ ] Phase 8.5 — `TestCluster` with real `RaftServer`s
- [ ] Phase 8.6 — port `RaftTestConfig` to `TestCluster`
- [ ] Phase 8.7 — `raft_lab_standalone` runs full `RaftLabTest`
- [ ] Phase 8.8 — `RaftClock` abstraction (deferred)
