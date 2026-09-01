# RaftWorker: The Mako-Raft Bridge

## 1. Purpose

`RaftWorker` is the bridge class that connects Mako's watermark-based
transaction pipeline to Raft's replicated log.  It occupies the same
architectural position as `PaxosWorker` in the original system: Mako hands
it serialised transaction logs, it pushes them into the consensus protocol,
and when the protocol commits an entry, it fires a callback that feeds the
committed data back to Mako for replay.

The key design constraint is that Mako does not know it is talking to Raft.
It calls the same API (`submit`, `add_log_to_nc`, `register_for_leader_par_id_return`)
regardless of which protocol is active.  The `raft_main_helper.cc` dispatcher
(see `architecture.md`) routes those calls to functions that operate on
`RaftWorker` instances stored in the global `raft_workers_g` vector.

**Source files**: `src/deptran/raft/raft_worker.h` (168 lines),
`src/deptran/raft/raft_worker.cc` (616 lines)

## 2. Class Layout

### 2.1 Member Variables

| Member | Type | Purpose |
|--------|------|---------|
| `callback_` | `std::function<void(const char*, int)>` | Simple log-apply callback (legacy) |
| `callback_par_id_` | `std::function<void(const char*&, int, int)>` | Callback with partition ID (legacy) |
| `leader_callback_par_id_return_` | `std::function<int(...)>` | Leader-role watermark callback |
| `follower_callback_par_id_return_` | `std::function<int(...)>` | Follower-role watermark callback |
| `submit_queue_` | `std::deque<PendingLog>` | Buffered logs awaiting submission |
| `submit_mutex_` / `submit_cv_` | mutex + condvar | Synchronisation for the submit queue |
| `submit_thread_` | `std::thread` | Background thread draining the queue |
| `submit_thread_stop_` | `std::atomic<bool>` | Shutdown signal for the submit thread |
| `batch_limit_` | `int` | Max logs dequeued per iteration (default 1) |
| `n_current` | `std::atomic<int>` | In-flight request count |
| `n_submit` | `std::atomic<int>` | Total submitted count |
| `n_tot` | `std::atomic<int>` | Total processed (committed) count |
| `site_info_` | `Config::SiteInfo*` | Partition, locale, port configuration |
| `rep_frame_` | `Frame*` | Raft frame (factory for protocol components) |
| `rep_sched_` | `TxLogServer*` | Points to the `RaftServer` instance |
| `rep_commo_` | `Communicator*` | Points to `RaftCommo` for peer RPCs |
| `svr_poll_thread_worker_` | `rusty::Option<rusty::Arc<PollThread>>` | Main RPC poll thread |
| `rpc_server_` | `srpc::Server*` | Raft RPC server (AppendEntries, Vote, etc.) |
| `thread_pool_g` | `base::ThreadPool*` | Thread pool for RPC handler dispatch |
| `svr_hb_poll_thread_worker_g` | `rusty::Option<rusty::Arc<PollThread>>` | Heartbeat RPC poll thread |
| `server_status_` | `rusty::Option<rusty::Arc<ServerStatus>>` | Status for heartbeat service |
| `hb_rpc_server_` | `srpc::Server*` | Heartbeat/control RPC server |
| `un_replay_logs_` | `std::queue<std::tuple<...>>` | Unreplayed logs (safety failures) |
| `is_leader` | `int` | Cached leadership flag (0 or 1) |
| `cur_epoch` | `int` | Current epoch from `ElectionState` |
| `election_state_lock` | `std::recursive_mutex` | Guards leadership state changes |

The watermark callback signature used by the production Mako path is:
```cpp
std::function<int(const char*& data, int len, int par_id, int epoch,
                  std::queue<std::tuple<int, int, int, int, const char*>>& unreplayed)>
```

### 2.2 Comparison with PaxosWorker

| Aspect | PaxosWorker | RaftWorker |
|--------|-------------|------------|
| Source | `src/deptran/paxos_worker.h` | `src/deptran/raft/raft_worker.h` |
| Protocol server | `PaxosServer` (via `rep_sched_`) | `RaftServer` (via `rep_sched_`) |
| Callback model | Single `callback_par_id_return_` | Separate `leader_callback_par_id_return_` + `follower_callback_par_id_return_` |
| Submit path | `_Submit()` / `_BulkSubmit()` | `Submit()` / `EnqueueLog()` + `SubmitLoop()` |
| Background thread | Uses `SubmitPool` | Uses `std::thread` + `std::deque` |
| Leadership query | `IsLeader()` via `ElectionState` | `IsLeader()` via `RaftServer::IsLeader()` |
| Leader change | External, via `ElectionState` | Internal, via `RegisterLeaderChangeCallback()` |
| Port scheme | Main port (e.g., 17xxx) | Main port + heartbeat at port + 10000 |
| Poll threads | `rusty::Option<rusty::Arc<PollThread>>` | Same (migrated to RustyCpp) |

The most important difference is the **dual callback model**.  Paxos uses a
fixed leader, so the worker only ever has one role.  Raft leadership can change
at any time, so `RaftWorker` stores both callbacks and dynamically selects the
correct one in its `Next()` method based on the current leadership state.

## 3. Setup Chain

`RaftWorker` initialisation happens in four phases, called sequentially by
`raft_main_helper.cc::server_launch_worker()`:

### 3.1 `SetupBase()` — Create the Protocol Stack

```
SetupBase()
  +-- Config::GetConfig()
  +-- Frame::GetFrame(replica_proto_)     → RaftFrame
  +-- rep_frame_->CreateScheduler()       → RaftServer
  +-- Set loc_id_, site_id_, partition_id_ on RaftServer
  +-- RegisterLeaderChangeCallback(lambda)
  +-- tot_num = config->get_tot_req()
```

**Key detail**: The leader-change callback captures `this` and:
1. Updates `is_leader` under `election_state_lock`
2. Calls `NotifyRaftLeaderChange(par_id, leader)` which routes through
   `raft_handle_leader_change()` in `raft_main_helper.cc` to re-apply
   callbacks and invoke the external leader election callback

**Source**: `raft_worker.cc:35-62`

### 3.2 `SetupService()` — Start the RPC Server

```
SetupService()
  +-- PollThread::create()                → svr_poll_thread_worker_
  +-- new ThreadPool(1)                   → thread_pool_g
  +-- new srpc::Server(poll_worker)        → rpc_server_
  +-- rep_frame_->CreateRpcServices(...)  → RaftServiceImpl
  +-- rpc_server_->reg_service(svc)       // ownership transferred
  +-- rpc_server_->start(bind_addr)
```

The RPC server listens on the port specified in `site_info_` and handles
incoming `Vote`, `AppendEntries`, `EmptyAppendEntries`, and `TimeoutNow`
RPCs.  Services are owned by the `rpc_server_` (transferred via
`reg_service`).

**Source**: `raft_worker.cc:65-97`

### 3.3 `SetupCommo()` — Connect to Peers

```
SetupCommo()
  +-- rep_frame_->CreateCommo(poll_thread.clone())  → RaftCommo
  +-- rep_sched_->commo_ = rep_commo_
```

`CreateCommo()` inside `RaftFrame` calls `ConnectToPeers()` which establishes
outbound TCP connections to all other replicas in the same partition.  The
clone of the poll thread is used for outbound I/O.

**Source**: `raft_worker.cc:100-109`

### 3.4 `SetupHeartbeat()` — Control-Plane RPC

```
SetupHeartbeat()
  +-- if (!config->do_heart_beat()) return
  +-- PollThread::create()                         → svr_hb_poll_thread_worker_g
  +-- new ThreadPool(1)                            → hb_thread_pool_g
  +-- new srpc::Server(hb_poll_worker)              → hb_rpc_server_
  +-- Arc<ServerStatus>::make()                    → server_status_
  +-- make_box<ServerControlServiceImpl>(status)
  +-- hb_rpc_server_->start(port + 10000)
```

The heartbeat server runs on a separate port (`site_info_->port + CtrlPortDelta`
where `CtrlPortDelta = 10000`).  It provides a `ServerControlServiceImpl` that
external monitors use to check liveness and trigger graceful shutdown.

**Source**: `raft_worker.cc:112-134`

### 3.5 Post-Setup: `EnsureSetup()` and `StartSubmitThread()`

After the four phases, `server_launch_worker()` in `raft_main_helper.cc`
does two more things:

1. **`RaftServer::EnsureSetup()`** — starts the election timer and heartbeat
   loop.  This is queued as a `OneTimeJob` on the poll thread to ensure
   correct thread affinity.

2. **`StartSubmitThread()`** — launches the background thread that drains the
   `submit_queue_`.

## 4. Log Submission Path

The submission path is how Mako transaction logs flow from the application
through RaftWorker into the Raft consensus protocol.

### 4.1 Flow Diagram

```
Mako Worker Thread
    |
    | add_log_to_nc(data, len, par_id, batch_size)
    v
raft_impl::add_log_to_nc()                    [raft_main_helper.cc:534]
    |
    +-- find_worker(par_id)
    +-- IsLeader(par_id) check → drop if not leader
    +-- enqueue_to_worker(worker, data, len, par_id, batch_size)
         |
         +-- worker->IncSubmit()               [raft_worker.cc:382]
         +-- worker->EnqueueLog(...)           [raft_worker.cc:275]
              |
              +-- PendingLog{payload, par_id}
              +-- submit_queue_.push_back()
              +-- submit_cv_.notify_one()
                   |
                   v
SubmitLoop() [background thread]               [raft_worker.cc:588]
    |
    +-- wait on submit_cv_
    +-- dequeue up to batch_limit_ entries
    +-- for each entry: Submit(data, len, par_id)
         |
         v
Submit(data, len, par_id)                      [raft_worker.cc:338]
    |
    +-- IsLeader(par_id) check → drop if not leader
    +-- CreateRaftLogCommand(data, len, tx_id)
    |   +-- TpcCommitCommand{tx_id_}
    |   +-- VecPieceData{sp_vec_piece_data_}
    |   +-- SimpleCommand{input.values_[0] = STR(data)}
    |
    +-- raft_server->Start(cmd, &index, &term)
    +-- n_tot++
```

### 4.2 `PendingLog` Queue

```cpp
// raft_worker.h:41-44
struct PendingLog {
    std::string payload;   // Copy of the raw bytes
    uint32_t par_id;       // Target partition
};
```

The queue decouples Mako's write path from Raft's consensus latency.  Mako
threads push into the `submit_queue_` (protected by `submit_mutex_`) and
immediately return.  The background `SubmitLoop` thread drains entries and
calls `Submit()` which blocks until `RaftServer::Start()` returns.

The copy semantics (`payload.assign(log, len)` at `raft_worker.cc:282`) are
necessary because the Mako caller's buffer may be reused immediately after
`add_log_to_nc` returns.

### 4.3 `CreateRaftLogCommand()` — Wrapping Raw Bytes

Raft's internal log format uses `Marshallable` objects, specifically
`TpcCommitCommand` with `VecPieceData` for batch optimisation.  Mako
sends raw serialised bytes.  `CreateRaftLogCommand()` bridges this gap:

```cpp
// raft_worker.cc:298-335
TpcCommitCommand
  +-- tx_id_ = auto-incrementing atomic counter
  +-- cmd_ = VecPieceData
       +-- sp_vec_piece_data_ = vector<shared_ptr<SimpleCommand>>
            +-- SimpleCommand[0]
                 +-- input.values_[0] = Value(STR(raw_bytes))
                 +-- partition_id_ = 0
```

The raw bytes are stored as a `Value::STR` (not `i32`) to avoid
`get_i32()` crashes in `SetLocalAppend`.  This structure is the same
for both production Mako logs and test payloads.

**Source**: `raft_worker.cc:293-335`

### 4.4 `SubmitLoop()` — Background Draining

```cpp
// raft_worker.cc:588-613
void RaftWorker::SubmitLoop() {
    unique_lock lock(submit_mutex_);
    while (true) {
        submit_cv_.wait(lock, [&] {
            return submit_thread_stop_ || !submit_queue_.empty();
        });
        if (submit_thread_stop_ && submit_queue_.empty()) break;

        // Dequeue up to batch_limit_ entries
        vector<PendingLog> batch;
        while (!submit_queue_.empty() && batch.size() < batch_limit_)
            batch.push_back(move(submit_queue_.front()));

        lock.unlock();
        for (auto& entry : batch)
            Submit(entry.payload.data(), entry.payload.size(), entry.par_id);
        lock.lock();
    }
}
```

The lock is released during `Submit()` calls so Mako threads can continue
enqueuing.  The `batch_limit_` is set by the `batch_size` parameter of
`EnqueueLog()`, which comes from `add_log_to_nc`'s fourth argument.

### 4.5 `StopSubmitThread()` — Graceful Drain

When shutting down, `StopSubmitThread()` (`raft_worker.cc:249-272`):
1. Sets `submit_thread_stop_ = true` under lock
2. Notifies the condvar
3. Joins the thread
4. Drains any remaining entries by calling `Submit()` inline

This ensures no committed logs are lost during shutdown.

## 5. Committed Entry Callback Path

When Raft commits a log entry, it flows back to Mako through the `Next()`
callback.

### 5.1 How `Next()` Gets Registered

Every callback registration method (`register_apply_callback`,
`register_leader_callback_par_id_return`, etc.) calls:

```cpp
rep_sched_->RegLearnerAction(
    std::bind(&RaftWorker::Next, this, _1, _2));
```

`RegLearnerAction()` is defined in `TxLogServer` (the base class of
`RaftServer`) at `scheduler.h:501`:

```cpp
void RegLearnerAction(function<int(int, shared_ptr<Marshallable>)> learner_action) {
    app_next_ = learner_action;
}
```

### 5.2 How `RaftServer` Invokes `app_next_`

`RaftServer::applyLogs()` is called after `commitIndex` advances (when a
majority confirms an entry).  For each newly committed slot:

```cpp
// server.cc:601-609
for (slotid_t id = executeIndex + 1; id <= commitIndex; id++) {
    auto next_instance = GetRaftInstance(id);
    if (next_instance && next_instance->log_) {
        app_next_(id, next_instance->log_);   // → RaftWorker::Next()
        executeIndex = id;
    }
}
```

### 5.3 `Next()` — The Core Callback

```
RaftServer::applyLogs()
    |
    | app_next_(slot_id, cmd)
    v
RaftWorker::Next(slot_id, cmd)               [raft_worker.cc:497]
    |
    +-- dynamic_cast<TpcCommitCommand>(cmd)
    +-- Extract VecPieceData → SimpleCommand → Value::STR → raw bytes
    +-- Determine role: am_leader = IsLeader(par_id)
    +-- Select callback:
    |     leader? → leader_callback_par_id_return_
    |     follower? → follower_callback_par_id_return_
    |
    +-- active_callback(log, len, par_id, slot_id, un_replay_logs_)
    |   → returns encoded_value
    |
    +-- status = encoded_value % 10
    +-- timestamp = encoded_value / 10
    |
    +-- if status == STATUS_SAFETY_FAIL && len > 0:
    |     malloc + memcpy → un_replay_logs_.push(...)
    |
    +-- return status
```

**Key details**:

1. **Payload extraction**: `Next()` reverses the wrapping done by
   `CreateRaftLogCommand()`.  It follows the chain
   `TpcCommitCommand → VecPieceData → SimpleCommand → Value::STR → raw bytes`.

2. **Dynamic role selection**: Unlike `PaxosWorker` which only has one
   callback, `RaftWorker` checks `IsLeader(par_id)` on **every** commit
   to choose the correct callback.  This handles leadership changes that
   occur between log submission and commit.

3. **Encoded return value**: Mako's callback returns `timestamp * 10 + status`.
   `Next()` decodes this to separate the two values.

4. **Safety failure handling**: When the callback returns
   `PaxosStatus::STATUS_SAFETY_FAIL`, the log entry is copied (via `malloc +
   memcpy`) into `un_replay_logs_` for later retry.  The queue is passed to
   the next callback invocation so Mako can attempt to replay them.

**Source**: `raft_worker.cc:497-585`

## 6. Leadership Queries

### 6.1 `IsLeader(par_id)`

```cpp
// raft_worker.cc:212-229
bool RaftWorker::IsLeader(uint32_t par_id) {
    // Check partition match
    if (rep_frame_->site_info_->partition_id_ != par_id)
        return false;
    // Delegate to RaftServer
    auto raft_server = GetRaftServer();
    if (raft_server)
        return raft_server->IsLeader();
    return false;
}
```

This is called on every `Submit()` and every `Next()`.  It queries the
`RaftServer` directly rather than caching, ensuring that leadership changes
from elections or transfers are immediately reflected.

### 6.2 `IsPartition(par_id)`

```cpp
// raft_worker.cc:232-236
bool RaftWorker::IsPartition(uint32_t par_id) {
    return rep_frame_->site_info_->partition_id_ == par_id;
}
```

Used by `raft_main_helper.cc::find_worker()` to locate the correct worker
for a given partition.

### 6.3 Leader Change Notification

When `RaftServer` detects a leadership change (via `setIsLeader()`), it
invokes the callback registered in `SetupBase()`:

```cpp
// raft_worker.cc:51-58
raft_server->RegisterLeaderChangeCallback([this](bool leader) {
    {
        std::lock_guard<std::recursive_mutex> guard(election_state_lock);
        is_leader = leader ? 1 : 0;
    }
    uint32_t par_id = site_info_ ? site_info_->partition_id_ : 0;
    NotifyRaftLeaderChange(par_id, leader);
});
```

`NotifyRaftLeaderChange()` is a free function in `janus::` namespace that
delegates to `raft_handle_leader_change()`, which re-applies the correct
leader/follower callbacks and invokes Mako's external election callback.

## 7. Callback Registration Methods

`RaftWorker` provides five callback registration methods, forming a
hierarchy from simple to full-featured:

### 7.1 Simple Callbacks (Legacy)

```cpp
void register_apply_callback(function<void(const char*, int)> cb);
void register_apply_callback_par_id(function<void(const char*&, int, int)> cb);
```

These store the callback and call `RegLearnerAction()` to wire up `Next()`.
They are the simplest form: the callback receives just the log bytes and
optionally the partition ID.  Used for basic test scenarios.

### 7.2 Watermark Callbacks (Production)

```cpp
void register_leader_callback_par_id_return(function<int(...)> cb);
void register_follower_callback_par_id_return(function<int(...)> cb);
```

These are the callbacks Mako uses in production.  They receive the full
signature including `epoch` (slot ID) and the `un_replay_logs_` queue, and
return an encoded `timestamp * 10 + status` value.

Both methods also call `RegLearnerAction()` to install `Next()` as the
`app_next_` handler.  Multiple registrations are idempotent because
`RegLearnerAction` simply overwrites the previous binding.

### 7.3 Deprecated Legacy Bridge

```cpp
void register_apply_callback_par_id_return(function<int(...)> cb);
```

This method exists for backward compatibility.  It logs a deprecation
warning and delegates to `register_follower_callback_par_id_return()`.

## 8. Shutdown Sequence

Shutdown is a two-phase process:

### Phase 1: `WaitForShutdown()`

```
WaitForShutdown()                            [raft_worker.cc:202]
  +-- StopSubmitThread()                     // drain queue, join thread
  +-- hb_rpc_server_->do_shutdown()          // signal heartbeat server
  +-- hb_rpc_server_->wait_for_shutdown()    // block until done
```

### Phase 2: `ShutDown()`

```
ShutDown()                                   [raft_worker.cc:137]
  +-- Signal poll threads to stop            // allows Reactor::Loop() to exit
  +-- delete rpc_server_                     // tears down Raft RPC server
  +-- delete hb_rpc_server_                  // tears down heartbeat server
  +-- server_status_ = rusty::None           // release Arc
  +-- hb_thread_pool_g->release()
  +-- thread_pool_g->release()
  +-- StopSubmitThread()                     // idempotent
  +-- delete rep_sched_                      // destroys RaftServer
  +-- Shutdown poll threads                  // AFTER servers are destroyed
```

**Critical ordering**: Poll threads must be shut down **after** the RPC
servers are destroyed.  The `Server::~Server()` destructor enqueues cleanup
commands to the poll thread; if the poll thread is already stopped, those
commands are lost and reference counts (`sconns_ctr_`) never reach zero,
causing resource leaks.

The destructor (`~RaftWorker()`) provides a separate safety net:

```cpp
// raft_worker.cc:22-32
RaftWorker::~RaftWorker() {
    StopSubmitThread();
    if (svr_poll_thread_worker_.is_some())
        svr_poll_thread_worker_.as_ref().unwrap()->shutdown();
    if (svr_hb_poll_thread_worker_g.is_some())
        svr_hb_poll_thread_worker_g.as_ref().unwrap()->shutdown();
}
```

## 9. Helper Methods

### 9.1 `GetRaftServer()`

```cpp
RaftServer* GetRaftServer() {
    return dynamic_cast<RaftServer*>(rep_sched_);
}
```

Casts the base `TxLogServer*` to `RaftServer*`.  Used throughout the class
and by `raft_main_helper.cc` to access Raft-specific APIs like
`SetPreferredLeader()`, `IsLeader()`, and `EnsureSetup()`.

### 9.2 `GetPollThreadWorker()`

```cpp
rusty::Option<rusty::Arc<PollThread>> GetPollThreadWorker() {
    return svr_poll_thread_worker_.clone();
}
```

Returns a clone of the poll thread `Arc`, used by `server_launch_worker()`
to queue the `EnsureSetup()` job on the correct event loop.

### 9.3 `IncSubmit()` / `WaitForSubmit()`

`IncSubmit()` bumps `n_submit`.  `WaitForSubmit()` polls until
`n_submit >= tot_num` (all expected logs submitted) and the submit queue
is empty.  Used during experiments to synchronise with the log pipeline.

## 10. Global State

```cpp
// raft_worker.h:165
extern vector<shared_ptr<RaftWorker>> raft_workers_g;
```

All `RaftWorker` instances for the current process are stored in this vector,
created by `raft_impl::setup()` and destroyed by `raft_impl::shutdown_paxos()`.
The vector is indexed by creation order (one worker per server site).
`find_worker(par_id)` in `raft_main_helper.cc` linearly scans this vector
by calling `IsPartition()`.

## 11. Complete Data Flow Diagram

```
Mako Worker                                        Raft Consensus
  Thread                                            Protocol
    |                                                  |
    | add_log_to_nc(data, len, par_id, batch)         |
    v                                                  |
[raft_impl::add_log_to_nc]                            |
    |                                                  |
    +-- find_worker(par_id)                            |
    +-- IsLeader? → drop if no                        |
    +-- IncSubmit()                                    |
    +-- EnqueueLog(data, len, par_id, batch)          |
         |                                             |
         v                                             |
    [PendingLog queue]                                 |
         |                                             |
         v                                             |
    [SubmitLoop thread]                                |
         |                                             |
         +-- dequeue batch                             |
         +-- Submit(data, len, par_id)                 |
              |                                        |
              +-- CreateRaftLogCommand(data, len, tx)   |
              +-- RaftServer::Start(cmd)  ------------>|
                                                       |
                                          [leader appends to log]
                                          [sends AppendEntries RPCs]
                                          [majority acknowledges]
                                          [commitIndex advances]
                                                       |
                                          [applyLogs()]|
                                                       |
              app_next_(slot_id, cmd) <----------------|
              |                                        |
              v                                        |
    [RaftWorker::Next(slot_id, cmd)]                   |
         |                                             |
         +-- extract raw bytes from TpcCommitCommand    |
         +-- IsLeader? → choose leader/follower cb     |
         +-- active_callback(log, len, par_id, slot)   |
              |                                        |
              v                                        |
         [Mako watermark callback]                     |
              |                                        |
              +-- update local watermark               |
              +-- return timestamp * 10 + status       |
```
