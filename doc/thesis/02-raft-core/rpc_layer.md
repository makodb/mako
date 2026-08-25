# RPC Layer — Communication Infrastructure

## What This Document Covers

This document explains the complete RPC (Remote Procedure Call) infrastructure that enables Raft nodes to communicate. It covers the three-layer architecture (`RaftCommo` → `RaftProxy` → `RaftService` → `RaftServiceImpl`), the macro system that eliminates boilerplate, the `DeferredReply` pattern for asynchronous responses, the `RaftFrame` factory pattern, proxy connection setup, the wire format via `Marshal`, and the disconnection simulation system used for testing.

**Key source files**:
- `src/deptran/raft/commo.h` — `RaftCommo` and `RaftVoteQuorumEvent` (129 lines)
- `src/deptran/raft/commo.cc` — RPC sending implementations (287 lines)
- `src/deptran/raft/service.h` — `RaftServiceImpl` with `RpcHandler` macro usage (84 lines)
- `src/deptran/raft/service.cc` — Handler implementations (112 lines)
- `src/deptran/raft/macros.h` — `RpcHandler` and `Call_Async` macro definitions (77 lines)
- `src/deptran/raft/frame.h` — `RaftFrame` factory class (50 lines)
- `src/deptran/raft/frame.cc` — Factory method implementations (207 lines)
- `src/deptran/rcc_rpc.h` — Generated `RaftService` and `RaftProxy` classes
- `src/deptran/communicator.h` — Base `Communicator` class with proxy maps
- `src/srpc/rpc/server.hpp` — `DeferredReply` RAII class
- `src/srpc/misc/marshal.hpp` — `Marshal` and `MarshallDeputy` serialization

---

## 1. Architecture Overview

The Raft RPC system has four layers, from sender to receiver:

```
SENDER SIDE                           RECEIVER SIDE

RaftCommo                             RaftServiceImpl
  (application logic)                   (application logic)
        |                                     ^
        v                                     |
RaftProxy                             RaftService (base)
  (serializes args,                     (deserializes args,
   sends async RPC)                      dispatches to handler)
        |                                     ^
        v                                     |
srpc::Client                           srpc::Server
  (TCP connection,                      (TCP listener,
   marshals request)                     unmarshals request)
```

### Class Hierarchy

```
srpc::Service (abstract base)
    |
    v
RaftService (generated, rcc_rpc.h)
    |  - enum { VOTE, APPENDENTRIES, EMPTYAPPENDENTRIES, TIMEOUTNOW }
    |  - __reg_to__(): registers RPC IDs with server
    |  - __dispatch__(): routes incoming RPCs by ID
    |  - __*__wrapper__(): deserializes args, creates DeferredReply
    |  - pure virtual: Vote(), AppendEntries(), EmptyAppendEntries(), TimeoutNow()
    |
    v
RaftServiceImpl (service.h)
    |  - RpcHandler macro overrides pure virtuals
    |  - Handle*() methods delegate to RaftServer
    |
    v
RaftServer (actual Raft logic)
```

---

## 2. RPC Definitions

Four RPCs are defined for the Raft protocol, each with a unique 32-bit ID:

| RPC | ID | Direction | Purpose |
|-----|----|-----------|---------|
| `Vote` | `0x3587ec7b` | Candidate → Peer | Request vote during election |
| `AppendEntries` | `0x1fc0e195` | Leader → Follower | Replicate log entry |
| `EmptyAppendEntries` | `0x4e8db0c5` | Leader → Follower | Heartbeat (no data) |
| `TimeoutNow` | `0x33c8c834` | Leader → Preferred | Trigger immediate election |

### Wire Format

Each RPC's arguments are serialized sequentially using the `Marshal` (`srpc::Marshal`) binary format. The `<<` and `>>` operators handle serialization and deserialization for primitive types (`uint64_t`, `bool_t`, `ballot_t`, `siteid_t`) and compound types (`MarshallDeputy`).

**Vote RPC:**
```
Request:  [lst_log_idx:u64] [lst_log_term:ballot_t] [site_id:siteid_t] [cur_term:ballot_t]
Response: [reply_term:ballot_t] [vote_granted:bool_t]
```

**AppendEntries RPC:**
```
Request:  [slot:u64] [ballot:ballot_t] [leaderCurrentTerm:u64] [leaderSiteId:siteid_t]
          [leaderPrevLogIndex:u64] [leaderPrevLogTerm:u64] [leaderCommitIndex:u64]
          [cmd:MarshallDeputy] [leaderNextLogTerm:u64]
Response: [followerAppendOK:u64] [followerCurrentTerm:u64] [followerLastLogIndex:u64]
```

**EmptyAppendEntries RPC:**
```
Request:  [slot:u64] [ballot:ballot_t] [leaderCurrentTerm:u64] [leaderSiteId:siteid_t]
          [leaderPrevLogIndex:u64] [leaderPrevLogTerm:u64] [leaderCommitIndex:u64]
          [trigger_election_now:bool_t]
Response: [followerAppendOK:u64] [followerCurrentTerm:u64] [followerLastLogIndex:u64]
```

**TimeoutNow RPC:**
```
Request:  [leaderTerm:u64] [leaderSiteId:siteid_t]
Response: [followerTerm:u64] [success:bool_t]
```

### MarshallDeputy — Polymorphic Serialization

`MarshallDeputy` (`src/srpc/misc/marshal.hpp:88`) is a type-erasing wrapper that serializes polymorphic `Marshallable` objects. It stores:
- `kind_` — A 32-bit type tag identifying the concrete type (e.g., `TpcCommitCommand`, `TpcBatchCommand`)
- `sp_data_` — A `shared_ptr<Marshallable>` to the actual data

On the wire, `kind_` is written first, followed by the object's serialized form. On deserialization, a factory registry (populated via `reg_initializer()`) maps `kind_` back to a constructor, creating the correct concrete type.

---

## 3. Macros — Boilerplate Elimination

Two macros in `macros.h` handle the repetitive parts of RPC handler registration and async RPC sending.

### RpcHandler Macro (`macros.h:50-61`)

```cpp
#define RpcHandler(name, ...) \
  void name(_ARGPAIRS(__VA_ARGS__), srpc::DeferredReply defer) override { \
    verify(svr_ != nullptr); \
    if (svr_->IsDisconnected()) { \
      OnDisconnected##name(_PARAMS(__VA_ARGS__)); \
      defer.reply(); \
    }  else { \
      Handle##name(_PARAMS(__VA_ARGS__), std::move(defer)); \
    } \
  } \
  void Handle##name(_ARGPAIRS(__VA_ARGS__), srpc::DeferredReply defer); \
  void OnDisconnected##name(_ARGPAIRS(__VA_ARGS__))
```

This macro generates three things for each RPC:
1. **Override method** (`Vote()`, `AppendEntries()`, etc.) — the virtual method from `RaftService`. It checks if the server is disconnected (for testing) and routes accordingly.
2. **Handler declaration** (`HandleVote()`, `HandleAppendEntries()`, etc.) — the real handler, implemented in `service.cc`.
3. **Disconnection handler declaration** (`OnDisconnectedVote()`, etc.) — called when the server simulates disconnection, returning default values.

The helper macros `_PARAMS` and `_ARGPAIRS` extract parameter names and type-name pairs from variadic arguments, supporting up to 20 parameters.

**Example expansion** for `RpcHandler(Vote, 6, ...)`:
```cpp
// Generated override (calls HandleVote or OnDisconnectedVote)
void Vote(const uint64_t& lst_log_idx, const ballot_t& lst_log_term,
          const siteid_t& can_id, const ballot_t& can_term,
          ballot_t* reply_term, bool_t* vote_granted,
          srpc::DeferredReply defer) override {
    verify(svr_ != nullptr);
    if (svr_->IsDisconnected()) {
        OnDisconnectedVote(lst_log_idx, lst_log_term, can_id, can_term,
                          reply_term, vote_granted);
        defer.reply();  // Reply with default values
    } else {
        HandleVote(lst_log_idx, lst_log_term, can_id, can_term,
                  reply_term, vote_granted, std::move(defer));
    }
}

// Handler declaration (implemented in service.cc)
void HandleVote(const uint64_t& lst_log_idx, const ballot_t& lst_log_term,
                const siteid_t& can_id, const ballot_t& can_term,
                ballot_t* reply_term, bool_t* vote_granted,
                srpc::DeferredReply defer);

// Disconnection handler (returns defaults set in service.h initializer)
void OnDisconnectedVote(const uint64_t& lst_log_idx, const ballot_t& lst_log_term,
                        const siteid_t& can_id, const ballot_t& can_term,
                        ballot_t* reply_term, bool_t* vote_granted);
```

### Call_Async Macro (`macros.h:72-76`)

```cpp
#define Call_Async(proxy, name, ...) { \
  auto f = proxy->async##_##name(__VA_ARGS__); \
  _RPC_COUNT(); \
  Future::safe_release(f); \
}
```

This macro:
1. Calls the proxy's async method (e.g., `proxy->async_Vote(...)`)
2. Optionally increments an RPC counter (when `RAFT_TEST_CORO` is defined)
3. Releases the future via `Future::safe_release()` — the callback (via `FutureAttr`) handles the result

---

## 4. RaftCommo — Sending RPCs

**Location**: `commo.h:51-125`, `commo.cc:1-287`

`RaftCommo` extends `Communicator` and provides three RPC sending methods plus a helper.

### Inheritance

```
Communicator (base, communicator.h)
    |  - rpc_par_proxies_: map<parid_t, vector<SiteProxyPair>>
    |  - ConnectToPeers(): establishes TCP connections
    |  - rpc_poll_: poll thread for async I/O
    |
    v
RaftCommo (raft/commo.h)
    |  - SendAppendEntries2(): point-to-point with IntEvent return
    |  - SendAppendEntries(): point-to-point with results struct return
    |  - BroadcastVote(): broadcast to all peers with quorum event
    |  - SendTimeoutNow(): point-to-point with callback
```

### Proxy Map

`rpc_par_proxies_` is populated by `Communicator::ConnectToPeers()` during startup. For each partition, it maps `parid_t` → `vector<(siteid_t, ClassicProxy*)>`. `RaftCommo` casts these `ClassicProxy*` pointers to `RaftProxy*` when sending Raft-specific RPCs:

```cpp
auto proxy = (RaftProxy*) p.second;
```

This cast is safe because the same `srpc::Client` connection supports multiple proxy types — they share the same underlying TCP connection and only differ in which RPC IDs they send.

### SendAppendEntries2() (`commo.cc:26-94`)

Used by `HeartbeatLoop` for the primary replication path. Returns an `IntEvent` that signals when the follower replies.

```
SendAppendEntries2(site_id, par_id, ..., cmd, ret_status, ret_term, ret_last_log_index):
  |
  +-- Create IntEvent (ret) for signaling completion
  +-- Find proxy for site_id in rpc_par_proxies_[par_id]
  +-- Set callback: on reply, extract (status, term, last_log_index), set ret(1)
  +-- IF cmd == nullptr:
  |     Call_Async(proxy, EmptyAppendEntries, ...)   // Heartbeat
  +-- ELSE:
  |     Wrap cmd in MarshallDeputy
  |     Call_Async(proxy, AppendEntries, ...)         // Data entry
  +-- RETURN IntEvent
```

Key design: the callback writes results directly into caller-provided pointers (`ret_status`, `ret_term`, `ret_last_log_index`) and then signals the event. The caller polls or waits on the event with a timeout.

### SendAppendEntries() (`commo.cc:96-175`)

Older variant used by some code paths. Returns a `SendAppendEntriesResults` struct instead of an `IntEvent`. Also supports the `trigger_election_now` flag for the preferred leader protocol.

The `SendAppendEntriesResults` struct tracks:
- `done` — Whether a valid reply was received
- `ok` — Whether the follower accepted the entry
- `followerTerm` — Follower's current term
- `followerLastLogIndex` — Follower's last log index
- `empty` — Whether this was a heartbeat

A special case: `(ok=false, followerTerm=0, followerLastLogIndex=0)` is reserved to simulate a lost RPC (no reply), so `done` stays `false`.

### BroadcastVote() (`commo.cc:177-210`)

Broadcasts `Vote` RPCs to all peers in the partition and returns a `RaftVoteQuorumEvent`:

```
BroadcastVote(par_id, lst_log_idx, lst_log_term, self_id, cur_term):
  |
  +-- n = partition size
  +-- Create RaftVoteQuorumEvent(n, n/2)
  |     Quorum threshold = n/2 (candidate already has self-vote)
  +-- FOR each peer (skip self):
  |     Set callback: extract (term, vote_granted), FeedResponse()
  |     Call_Async(proxy, Vote, ...)
  +-- RETURN quorum event
```

### SendTimeoutNow() (`commo.cc:228-285`)

Sends a single `TimeoutNow` RPC to a target replica for leadership transfer:

```
SendTimeoutNow(site_id, par_id, leader_term, leader_site_id, callback):
  |
  +-- Find proxy for site_id
  +-- Set callback: on reply, extract (follower_term, success), call user callback
  +-- Call_Async(proxy, TimeoutNow, leader_term, leader_site_id, ...)
  +-- IF site not found: callback(false, 0)
```

### WAN_WAIT

The `WAN_WAIT` macro (`communicator.h:29-35`) is a compile-time switch:
- When `SIMULATE_WAN` is defined: calls `_wan_wait()` to inject artificial network delay
- Otherwise: expands to `;` (no-op)

This allows testing WAN-like latency in local deployments.

---

## 5. RaftServiceImpl — Receiving RPCs

**Location**: `service.h:21-83`, `service.cc:1-112`

`RaftServiceImpl` receives incoming RPCs and delegates to `RaftServer`.

### Constructor (`service.cc:15-20`)

```cpp
RaftServiceImpl::RaftServiceImpl(TxLogServer *sched)
    : svr_((RaftServer*)sched) {
    struct timespec curr_time;
    clock_gettime(CLOCK_MONOTONIC_RAW, &curr_time);
    srand(curr_time.tv_nsec);
}
```

Seeds the random number generator with nanosecond-precision time for election timeout randomization.

### Handler Registration (via `RpcHandler` macro)

Each RPC is declared in `service.h` using the `RpcHandler` macro, which:
1. Sets output parameters to safe defaults (e.g., `*vote_granted = false`)
2. Generates the override, handler declaration, and disconnection handler

```cpp
RpcHandler(Vote, 6,
           const uint64_t&, lst_log_idx,
           const ballot_t&, lst_log_term,
           const siteid_t&, can_id,
           const ballot_t&, can_term,
           ballot_t*, reply_term,
           bool_t*, vote_granted) {
    *reply_term = can_term;     // Default: echo candidate's term
    *vote_granted = false;      // Default: reject
}
```

The body in braces after the macro is the `OnDisconnected` handler — it sets the default output values when the server is simulating disconnection.

### Handler Implementations (`service.cc`)

Each `Handle*` method delegates to the corresponding `RaftServer` method:

| Handler | Runs In | Delegates To |
|---------|---------|-------------|
| `HandleVote` | Caller's fiber | `svr_->OnRequestVote()` |
| `HandleAppendEntries` | New fiber | `svr_->OnAppendEntries()` |
| `HandleEmptyAppendEntries` | New fiber | `svr_->OnAppendEntries()` with `cmd=nullptr` |
| `HandleTimeoutNow` | Caller's fiber | `svr_->OnTimeoutNow()` |

**Why new fibers for AppendEntries?** `HandleAppendEntries` and `HandleEmptyAppendEntries` create new fibers (`Fiber::create_run`) because `OnAppendEntries` may block (e.g., waiting for a mutex or applying logs). Running in a separate fiber prevents blocking the RPC dispatch thread.

`HandleVote` and `HandleTimeoutNow` run in the caller's fiber because they are fast operations that don't block.

### DeferredReply Pattern

Each handler receives a `DeferredReply` object by move. This RAII wrapper:
1. Holds the original `Request` and a `WeakServerConnection`
2. On `defer.reply()`: serializes output parameters via `marshal_reply_` callback, sends the response
3. On destruction: calls `cleanup_` to delete the heap-allocated parameter storage

The handler captures `defer` in a lambda and calls `defer.reply()` after setting output parameters:

```cpp
void HandleVote(..., srpc::DeferredReply defer) {
    svr_->OnRequestVote(lst_log_idx, lst_log_term, can_id, can_term,
                        reply_term, vote_granted,
                        [defer = std::move(defer)]() mutable { defer.reply(); });
}
```

This pattern decouples the reply from the RPC dispatch — the server can reply asynchronously after completing its work.

---

## 6. RaftService — Generated Base Class

**Location**: `rcc_rpc.h:1491-1671`

`RaftService` is the base class for `RaftServiceImpl`. It handles the low-level RPC plumbing:

### RPC Registration (`__reg_to__`)

Registers each RPC ID with the `srpc::Server`:

```cpp
int __reg_to__(srpc::Server& svr, size_t svc_index) override {
    svr.reg_rpc(VOTE, svc_index);
    svr.reg_rpc(APPENDENTRIES, svc_index);
    svr.reg_rpc(EMPTYAPPENDENTRIES, svc_index);
    svr.reg_rpc(TIMEOUTNOW, svc_index);
}
```

The server uses `svc_index` to find the correct service when an RPC arrives.

### RPC Dispatch (`__dispatch__`)

Routes incoming RPCs by ID:

```cpp
void __dispatch__(srpc::i32 rpc_id, rusty::Box<srpc::Request> req,
                  srpc::WeakServerConnection weak_sconn) override {
    switch (rpc_id) {
        case VOTE:              __Vote__wrapper__(std::move(req), weak_sconn); break;
        case APPENDENTRIES:     __AppendEntries__wrapper__(std::move(req), weak_sconn); break;
        case EMPTYAPPENDENTRIES:__EmptyAppendEntries__wrapper__(std::move(req), weak_sconn); break;
        case TIMEOUTNOW:        __TimeoutNow__wrapper__(std::move(req), weak_sconn); break;
    }
}
```

### Wrapper Methods

Each `__*__wrapper__` method performs deserialization:

1. **Allocate** heap storage for each input and output parameter
2. **Deserialize** inputs from the request's `Marshal` buffer (`req->m >> *in_N`)
3. **Create** `marshal_reply_` lambda to serialize outputs on reply
4. **Create** `cleanup_` lambda to delete all allocated storage
5. **Construct** `DeferredReply` with these lambdas
6. **Call** the virtual handler method

Example for `Vote`:
```cpp
void __Vote__wrapper__(rusty::Box<srpc::Request> req, WeakServerConnection weak_sconn) {
    uint64_t* in_0 = new uint64_t;     req->m >> *in_0;  // lst_log_idx
    ballot_t* in_1 = new ballot_t;     req->m >> *in_1;  // lst_log_term
    siteid_t* in_2 = new siteid_t;     req->m >> *in_2;  // can_id
    ballot_t* in_3 = new ballot_t;     req->m >> *in_3;  // can_term
    ballot_t* out_0 = new ballot_t;                       // reply_term
    bool_t*   out_1 = new bool_t;                         // vote_granted

    auto __marshal_reply__ = [=](Marshal& m) { m << *out_0; m << *out_1; };
    auto __cleanup__ = [=] { delete in_0; ...; delete out_1; };

    DeferredReply __defer__(std::move(req), weak_sconn, __marshal_reply__, __cleanup__);
    this->Vote(*in_0, *in_1, *in_2, *in_3, out_0, out_1, std::move(__defer__));
}
```

---

## 7. RaftProxy — Sending Side

**Location**: `rcc_rpc.h:1673-1770`

`RaftProxy` wraps an `srpc::Client` connection and provides typed async methods for each RPC:

```cpp
class RaftProxy {
    srpc::Client* __cl__;

    FutureResult async_Vote(const uint64_t& lst_log_idx, ..., const FutureAttr& fuattr) {
        return __cl__->request(RaftService::VOTE, fuattr, [&](Marshal& m) {
            m << lst_log_idx;
            m << lst_log_term;
            m << site_id;
            m << cur_term;
        });
    }
    // ... async_AppendEntries, async_EmptyAppendEntries, async_TimeoutNow
};
```

The `request()` method:
1. Serializes the RPC ID and arguments into a `Marshal` buffer
2. Sends the request over TCP
3. Associates the `FutureAttr::callback` with the request for async response handling
4. Returns a `FutureResult` (which `Call_Async` releases via `safe_release()`)

---

## 8. RaftFrame — Factory Pattern

**Location**: `frame.h:16-49`, `frame.cc:1-207`

`RaftFrame` is the factory that creates all Raft components. It extends the base `Frame` class and is registered via the `REG_FRAME` macro.

### Registration

```cpp
REG_FRAME(MODE_RAFT, vector<string>({"raft"}), RaftFrame);
```

This registers `RaftFrame` for mode `MODE_RAFT` with the name `"raft"`, matching YAML config `mode: raft`.

### Factory Methods

| Method | Creates | Lifetime |
|--------|---------|----------|
| `CreateScheduler()` | `RaftServer` | Owned by frame (`unique_ptr`) |
| `CreateCommo()` | `RaftCommo` | Owned by frame (`unique_ptr`) |
| `CreateCoordinator()` | `CoordinatorRaft` | Caller-owned (raw `new`) |
| `CreateExecutor()` | `RaftExecutor` | Caller-owned (raw `new`) |
| `CreateRpcServices()` | `RaftServiceImpl` | Returned as `Box<Service>` |

### Ownership Model

```
RaftFrame (owns)
    |
    +-- commo_: unique_ptr<RaftCommo>     (single communicator)
    +-- svr_: unique_ptr<RaftServer>      (single server)
    +-- slot_hint_: Arc<Cell<slotid_t>>   (shared counter)
    |
    |   (borrows to)
    +-- CoordinatorRaft instances
    |     coo->commo_ = commo_.get()      (raw pointer borrow)
    |     coo->svr_ = svr_.get()          (raw pointer borrow)
    |     coo->slot_hint_ = slot_hint_    (Arc copy)
    |
    +-- RaftServiceImpl
          svc->svr_ = svr_.get()          (raw pointer borrow)
```

The frame owns the long-lived components. Short-lived components (coordinators, service impl) borrow pointers. This is safe because the frame outlives all borrowers.

### CreateScheduler() (`frame.cc:80-100`)

Creates the `RaftServer` exactly once. Calling it twice triggers `verify(0)` (abort). Under `RAFT_TEST_CORO`, also registers this frame in a static map for test infrastructure.

### CreateCommo() (`frame.cc:103-190`)

Creates the `RaftCommo` on first call, returns the cached instance on subsequent calls. Under `RAFT_TEST_CORO`, waits for all 5 replicas to create communicators, then launches the test coroutine.

### CreateRpcServices() (`frame.cc:193-204`)

Returns a vector containing a single `RaftServiceImpl` wrapped in `rusty::Box<Service>`:

```cpp
vector<rusty::Box<Service>>
RaftFrame::CreateRpcServices(uint32_t site_id, TxLogServer *rep_sched, ...) {
    auto result = vector<rusty::Box<Service>>();
    switch (config->replica_proto_) {
        case MODE_RAFT:
            result.push_back(rusty::make_box<RaftServiceImpl>(rep_sched));
        default: break;
    }
    return result;
}
```

---

## 9. RaftVoteQuorumEvent — Quorum Tracking

**Location**: `commo.h:11-38`

`RaftVoteQuorumEvent` extends `QuorumEvent` to track vote responses:

```cpp
class RaftVoteQuorumEvent : public QuorumEvent {
    void FeedResponse(bool y, ballot_t term) {
        if (y) { vote_yes(); }
        else {
            vote_no();
            if (term > highest_term_) highest_term_ = term;
        }
    }

    int64_t Term() { return highest_term_; }
};
```

The `highest_term_` field tracks the maximum term seen in any NO vote, allowing the candidate to advance its term after losing an election.

The quorum event integrates with Mako's reactor system:
- `vote_yes()` / `vote_no()` call `test()` which evaluates `is_ready()`
- When ready (quorum reached), the waiting coroutine is woken

---

## 10. Disconnection Simulation

The RPC layer supports simulating network partitions for testing via the `IsDisconnected()` mechanism:

### How It Works

1. `RaftServer::Disconnect(true)` sets `disconnected_ = true`
2. When an RPC arrives, the `RpcHandler` macro checks `svr_->IsDisconnected()`
3. If disconnected: runs `OnDisconnected*()` handler (returns default/reject values) and immediately replies
4. If connected: runs `Handle*()` handler (delegates to `RaftServer`)

### Default Disconnection Responses

Set in the `RpcHandler` initializer blocks in `service.h`:

| RPC | Disconnected Response |
|-----|----------------------|
| Vote | `reply_term = can_term`, `vote_granted = false` |
| AppendEntries | `followerAppendOK = false`, `followerCurrentTerm = 0`, `followerLastLogIndex = 0` |
| EmptyAppendEntries | Same as AppendEntries |
| TimeoutNow | `followerTerm = 0`, `success = false` |

The `(ok=false, term=0, lastLogIndex=0)` triple for AppendEntries is specially recognized by `SendAppendEntries()` as a "lost RPC" — the `done` flag stays `false`, simulating a packet that never arrived.

### Reconnection

`RaftServer::Reconnect()` sets `disconnected_ = false` and resets the election timer, simulating a node rejoining the network.

---

## 11. SendAppendEntriesResults — Response Tracking

**Location**: `commo.h:40-48`

```cpp
class SendAppendEntriesResults {
    std::recursive_mutex mtx;
    bool done = false;
    uint64_t ok = 0;
    uint64_t followerTerm = 0;
    uint64_t followerLastLogIndex = 0;
    bool empty = true;
};
```

This struct is used by `SendAppendEntries()` (the older variant) to collect results from a single follower. The `done` flag distinguishes between:
- `done = true`: A valid response was received
- `done = false`: Either no response yet or a simulated lost RPC

---

## 12. RAFT_TEST_CORO — Test Infrastructure

When compiled with `RAFT_TEST_CORO`, additional infrastructure is enabled:

1. **RPC counting**: `Call_Async` increments `rpc_count_` (protected by `rpc_mtx_`) for test assertions
2. **Frame tracking**: Static `frames_` map tracks all `RaftFrame` instances by locale ID
3. **Test coroutine**: `CreateCommo()` on site 0 creates a test fiber that waits for all 5 communicators, then runs `RaftLabTest::Run()`
4. **Reactor shutdown**: After tests complete, `Reactor::get_reactor()->looping_ = false` terminates the event loop

---

## 13. Complete RPC Flow — AppendEntries Example

```
Leader RaftServer (HeartbeatLoop)
    |
    | RaftCommo::SendAppendEntries2(site_id, ..., cmd, &status, &term, &last_idx)
    v
RaftCommo
    |
    | Create IntEvent for result signaling
    | Find proxy in rpc_par_proxies_[par_id]
    | Set callback to extract reply fields
    | Call_Async(proxy, AppendEntries, slot, ballot, term, siteId, prevIdx, prevTerm, commitIdx, md, logTerm, fuattr)
    v
RaftProxy::async_AppendEntries(...)
    |
    | client->request(APPENDENTRIES, fuattr, [&](Marshal& m) { m << slot << ballot << ... })
    v
    ~~~~ TCP ~~~~
    v
srpc::Server receives request
    |
    | Looks up svc_index for APPENDENTRIES
    | Calls RaftService::__dispatch__(APPENDENTRIES, req, weak_sconn)
    v
RaftService::__AppendEntries__wrapper__()
    |
    | Deserializes: req->m >> slot >> ballot >> term >> siteId >> prevIdx >> prevTerm >> commitIdx >> cmd >> logTerm
    | Allocates output: followerAppendOK, followerCurrentTerm, followerLastLogIndex
    | Creates DeferredReply with marshal_reply_ and cleanup_ lambdas
    v
RaftServiceImpl::AppendEntries()  (generated by RpcHandler macro)
    |
    | IsDisconnected()? → OnDisconnectedAppendEntries() → reply with defaults
    | Otherwise → HandleAppendEntries()
    v
HandleAppendEntries() [in new Fiber]
    |
    | svr_->OnAppendEntries(slot, ballot, term, siteId, prevIdx, prevTerm,
    |                       commitIdx, cmd->sp_data_, logTerm,
    |                       &followerAppendOK, &followerCurrentTerm, &followerLastLogIndex,
    |                       [defer]() { defer.reply(); })
    v
RaftServer::OnAppendEntries() — processes entry
    |
    | Sets *followerAppendOK, *followerCurrentTerm, *followerLastLogIndex
    | Calls callback → defer.reply()
    v
DeferredReply::reply()
    |
    | marshal_reply_(m): m << *followerAppendOK << *followerCurrentTerm << *followerLastLogIndex
    | Sends response over TCP
    | cleanup_(): deletes all heap-allocated parameters
    v
    ~~~~ TCP ~~~~
    v
RaftProxy callback fires (on leader)
    |
    | fu->get_reply() >> *ret_status >> *ret_term >> *ret_last_log_index
    | ret->set(1)  — signals IntEvent
    v
Leader HeartbeatLoop wakes, reads results from ret_status/ret_term/ret_last_log_index
```

---

## Related Documents

- [Protocol Overview](protocol_overview.md) — High-level Raft architecture
- [Server Implementation](server_implementation.md) — `RaftServer` internals
- [Leader Election](leader_election.md) — How `BroadcastVote()` drives elections
- [Log Replication](log_replication.md) — How `SendAppendEntries2()` drives replication
- [CoordinatorRaft](coordinator.md) — Transaction submission layer
- [System Architecture](../01-mako-overview/system_architecture.md) — Where the RPC layer fits in Mako
