# raft_main_helper.cc: The Raft Glue Layer

## 1. Purpose

`raft_main_helper.cc` (751 lines) implements the `raft_impl` namespace that
the `replication_helper` dispatcher routes to when `ReplicationType::RAFT`
is active.  It is the Raft counterpart to `paxos_main_helper.cc` and
provides all 30+ functions that Mako calls through the unified API.

Structurally, the file has three scoping levels:

1. **`janus::` namespace** (file-level globals): `raft_workers_g`,
   `leader_callback_`, `client_workers_storage`
2. **`raft_impl::` namespace** (public dispatcher targets): `setup()`,
   `add_log_to_nc()`, `register_for_leader_par_id_return()`, etc.
3. **Anonymous namespace** (internal helpers): `server_launch_worker()`,
   `find_worker()`, `enqueue_to_worker()`, `apply_callbacks_for_partition()`,
   `wait_for_local_leadership()`

**Source**: `src/deptran/raft_main_helper.cc`

## 2. Global State

### 2.1 Globals in `janus::` Namespace

```cpp
// raft_main_helper.cc:15-18
namespace janus {
vector<unique_ptr<ClientWorker>> client_workers_storage = {};
vector<shared_ptr<RaftWorker>> raft_workers_g = {};
std::function<void(int)> leader_callback_{};
}
```

| Variable | Type | Purpose |
|----------|------|---------|
| `client_workers_storage` | `vector<unique_ptr<ClientWorker>>` | Client-side workers (unused in Raft path, kept for linkage) |
| `raft_workers_g` | `vector<shared_ptr<RaftWorker>>` | All `RaftWorker` instances for this process |
| `leader_callback_` | `std::function<void(int)>` | External callback invoked on every leadership change |

`raft_workers_g` is the central data structure.  Every function in
`raft_impl::` looks up workers from this vector.  It is populated by
`setup()` and cleared by `shutdown_paxos()`.

### 2.2 State in `raft_impl::` Namespace

```cpp
// raft_main_helper.cc:29-35
std::map<int, WatermarkCallback> leader_replay_cb;
std::map<int, WatermarkCallback> follower_replay_cb;
shared_ptr<ElectionState> es = ElectionState::instance();
```

| Variable | Type | Purpose |
|----------|------|---------|
| `leader_replay_cb` | `map<par_id → callback>` | Cached leader watermark callbacks, keyed by partition |
| `follower_replay_cb` | `map<par_id → callback>` | Cached follower watermark callbacks, keyed by partition |
| `es` | `shared_ptr<ElectionState>` | Singleton shared with Paxos for epoch/leader state |

The dual callback maps are the key architectural difference from Paxos.
In Paxos, only `leader_replay_cb` exists because leadership is fixed.
In Raft, leadership can change at any time, so both maps are needed and
are re-applied to workers on every leadership change via
`apply_callbacks_for_partition()`.

## 3. Lifecycle Functions

### 3.1 `setup(argc, argv)` — Worker Creation

```
setup(argc, argv)                              [line 239]
  +-- check_current_path()                      // logging
  +-- setenv("MAKO_DISABLE_JETPACK", "1")       // disable Jetpack optimization
  +-- Config::CreateConfig(argc, argv)          // parse YAML configs
  +-- Verify replica_proto_ == MODE_RAFT        // safety check
  +-- for each server site (reverse order):
  |   +-- Config::SiteById(id) → site
  |   +-- RaftWorker::new()
  |   +-- worker->site_info_ = &site
  |   +-- worker->SetupBase()                   // creates RaftServer
  |   +-- raft_workers_g.push_back(worker)
  +-- reverse(raft_workers_g)                   // restore original order
  +-- es->machine_id = last worker's locale_id
  +-- return site names
```

**Reverse-then-reverse pattern**: Workers are created in reverse site order
then the vector is reversed.  This matches the Paxos helper's convention
where the last worker's locale determines the machine ID.

**Jetpack disabling**: `MAKO_DISABLE_JETPACK=1` is forced because Raft's
own batching pipeline replaces Jetpack's optimistic log aggregation.  If
the environment variable is already set, it is respected.

**Source**: `raft_main_helper.cc:239-285`

### 3.2 `setup2(action, shardIndex)` — Service Launch + Preferred Leader

```
setup2(action, shardIndex)                     [line 297]
  +-- Get server_infos from config
  +-- For each RaftWorker:
  |   +-- GetRaftServer()
  |   +-- Find locale_id==0 site in same partition
  |   +-- SetPreferredLeader(preferred_site_id)
  +-- Configure ElectionState for Paxos compat:
  |   +-- es->set_state(0), set_epoch(0), set_leader(0)
  +-- server_launch_worker(server_infos)
  +-- return 0
```

**Preferred leader configuration**: For each partition, `setup2()` scans
all sites in that partition to find the one with `locale_id == 0`
(localhost).  That site becomes the preferred leader.  This bias causes
the localhost node to use shorter election timeouts (150-300ms vs 500ms-1s)
and triggers leadership transfer when the current leader is not the
preferred one.

**Multi-partition example** (from comments at line 309):
```
Partition 0: sites s101(localhost), s201(p1), s302(p2) → preferred = s101
Partition 1: sites s102(localhost), s202(p1), s303(p2) → preferred = s102
```

**ElectionState compatibility**: Even though Raft has its own leadership
mechanism, `ElectionState` is updated to keep Mako's `is_leader` checks
working.  Both preferred and non-preferred machines start with
`state=0, epoch=0, leader=0`.

**Source**: `raft_main_helper.cc:297-374`

### 3.3 `server_launch_worker()` — Internal Multi-Phase Boot

This anonymous-namespace function orchestrates the RaftWorker boot sequence
in three passes:

```
server_launch_worker(server_sites)             [line 116]
  Pass 1: SetupService()                       // start RPC servers
    +-- for each worker: worker->SetupService()

  Pass 2: SetupCommo() + EnsureSetup() + StartSubmitThread()
    +-- for each (site, worker):
    |   +-- worker->SetupCommo()               // connect to peers
    |   +-- Queue EnsureSetup() on poll thread  // start elections
    |   +-- worker->StartSubmitThread()         // start submit loop

  Pass 3: SetupHeartbeat()                     // control-plane RPC
    +-- for each worker: worker->SetupHeartbeat()
```

**EnsureSetup ordering**: `EnsureSetup()` (which starts the election timer
and heartbeat loop) is queued as a `OneTimeJob` on the worker's poll thread
rather than called inline.  This ensures correct thread affinity — the
election timer must run on the same event loop that handles incoming RPCs.

**Source**: `raft_main_helper.cc:116-178`

### 3.4 `shutdown_paxos()` — Two-Phase Teardown

```
shutdown_paxos()                               [line 415]
  +-- es->running = false
  +-- Phase 1: for each worker → WaitForShutdown()
  |   (drains submit queue, signals heartbeat server)
  +-- Phase 2: for each worker → ShutDown()
  |   (deletes RPC servers, destroys RaftServer)
  +-- raft_workers_g.clear()
  +-- RandomGenerator::destroy()
  +-- Config::DestroyConfig()
```

The two-phase approach ensures all in-flight RPCs complete before
destructors run.  `WaitForShutdown()` blocks until the heartbeat RPC
server has acknowledged shutdown, then `ShutDown()` destroys all
resources.

**Source**: `raft_main_helper.cc:415-436`

### 3.5 `pre_shutdown_step()` — Graceful Disconnect

```
pre_shutdown_step()                            [line 572]
  +-- for each worker:
  |   +-- hb_rpc_server_->do_shutdown()        // signal heartbeat
  |   +-- WaitForShutdown()                    // block until done
```

Called by Mako before `shutdown_paxos()` to gracefully disconnect
control-plane connections.  This allows external monitors to detect
the shutdown before TCP connections are torn down.

**Source**: `raft_main_helper.cc:572-583`

## 4. Internal Helper Functions

### 4.1 `find_worker(par_id)` — Partition Lookup

```cpp
// raft_main_helper.cc:181-188
RaftWorker* find_worker(uint32_t par_id) {
    for (auto& worker : raft_workers_g) {
        if (worker && worker->IsPartition(par_id))
            return worker.get();
    }
    return nullptr;
}
```

Linear scan over `raft_workers_g`.  Called on every log submission and
callback registration.  The vector is typically small (1 worker per
partition per process, usually 1-8), so linear scan is faster than
a hash map due to cache locality.

### 4.2 `enqueue_to_worker()` — Submit Dispatch

```cpp
// raft_main_helper.cc:191-205
void enqueue_to_worker(RaftWorker* worker,
                       const char* log, int len,
                       uint32_t par_id, int batch_size) {
    worker->IncSubmit();
    if (worker->HasSubmitThread()) {
        worker->EnqueueLog(log, len, par_id, batch_size);
    } else {
        worker->Submit(log, len, par_id);
    }
}
```

Routes to the background submit thread if running, otherwise submits
inline.  `IncSubmit()` is always called first for bookkeeping.

### 4.3 `apply_callbacks_for_partition(par_id)` — Callback Re-Application

```cpp
// raft_main_helper.cc:209-226
void apply_callbacks_for_partition(uint32_t par_id) {
    auto* worker = find_worker(par_id);
    if (!worker || !worker->site_info_) return;

    auto leader_it = leader_replay_cb.find(par_id);
    if (leader_it != leader_replay_cb.end())
        worker->register_leader_callback_par_id_return(leader_it->second);

    auto follower_it = follower_replay_cb.find(par_id);
    if (follower_it != follower_replay_cb.end())
        worker->register_follower_callback_par_id_return(follower_it->second);
}
```

This function is called in two contexts:
1. **Initial registration**: When Mako registers callbacks via
   `register_for_leader_par_id_return()` / `register_for_follower_par_id_return()`
2. **Leadership change**: When `handle_leader_change_impl()` fires

Both callbacks are always re-applied because `RaftWorker::Next()` dynamically
selects which to call based on the current leadership state.

## 5. Leader Change Propagation

### 5.1 Notification Chain

```
RaftServer::setIsLeader()
    |
    | leader_change_callback_(is_leader)
    v
RaftWorker::RegisterLeaderChangeCallback lambda    [raft_worker.cc:51]
    |
    +-- Update is_leader under election_state_lock
    +-- NotifyRaftLeaderChange(par_id, is_leader)
         |
         v
janus::NotifyRaftLeaderChange()                    [line 744]
    |
    +-- if (is_using_raft())
    |
    v
janus::raft_handle_leader_change()                 [line 732]
    |
    +-- raft_impl::handle_leader_change_impl()     [line 233]
    |   +-- apply_callbacks_for_partition(par_id)   // re-wire callbacks
    |   +-- leader_wait_cv.notify_all()             // unblock waiters
    |
    +-- if (leader_callback_)
        leader_callback_(is_leader ? 1 : 0)        // notify Mako
```

**Key design points**:

- `NotifyRaftLeaderChange()` checks `is_using_raft()` to prevent accidental
  invocation in Paxos mode.
- `handle_leader_change_impl()` is in the `raft_impl` namespace (accessible
  to `janus::` via the forward declaration at line 233).
- `leader_wait_cv.notify_all()` unblocks any threads in
  `wait_for_local_leadership()`.
- The external callback receives `1` for "became leader" and `0` for
  "lost leadership".

### 5.2 `wait_for_local_leadership()` — Blocking Wait

```cpp
// raft_main_helper.cc:68-105
bool wait_for_local_leadership(RaftWorker* worker,
                               uint32_t par_id,
                               std::chrono::milliseconds timeout) {
    auto deadline = steady_clock::now() + timeout;
    unique_lock lock(leader_wait_mutex);
    while (true) {
        lock.unlock();
        if (worker->IsLeader(par_id)) return true;
        lock.lock();
        if (steady_clock::now() >= deadline) break;
        leader_wait_cv.wait_until(lock, deadline);
    }
    return worker->IsLeader(par_id);
}
```

**Timeout**: `kLeaderWaitTimeout = 5000ms` (5 seconds), defined at line 56.

**Usage pattern**: The lock-unlock-check-lock pattern avoids holding the
mutex during `IsLeader()` (which acquires the `RaftServer` mutex internally).
`leader_wait_cv` is notified whenever any leadership change occurs.

This function is currently defined but not called on the hot path.  It is
available for multi-shard startup scenarios where Mako needs to wait for
a partition to elect a leader before submitting logs.

## 6. Log Submission Functions

### 6.1 `add_log_to_nc()` — Primary Hot Path

```cpp
// raft_main_helper.cc:534-553
void add_log_to_nc(const char* log, int len,
                   uint32_t par_id, int batch_size) {
    auto* worker = find_worker(par_id);
    if (!worker) return;

    if (!worker->IsLeader(par_id)) return;  // drop immediately

    enqueue_to_worker(worker, log, len, par_id, max(1, batch_size));
}
```

This is the function Mako worker threads call on every transaction commit.
It matches the Paxos behaviour of immediately dropping logs when not leader
(no waiting, no retry).

### 6.2 `submit()` / `add_log()` / `add_log_without_queue()`

```cpp
void submit(const char* log, int len, uint32_t par_id);   // line 510
void add_log(const char* log, int len, uint32_t par_id);   // line 524 → submit()
void add_log_without_queue(...);                            // line 529 → submit()
```

All three ultimately call `enqueue_to_worker()` with `batch_size=1`.
`add_log` and `add_log_without_queue` are aliases for Paxos API
compatibility.

### 6.3 `wait_for_submit(par_id)`

```cpp
// raft_main_helper.cc:557-564
void wait_for_submit(uint32_t par_id) {
    auto* worker = find_worker(par_id);
    if (!worker) return;
    worker->WaitForSubmit();
}
```

Blocks until the submit queue is empty and all submitted logs have been
processed.  Used at end-of-experiment to ensure all data is replicated.

## 7. Callback Registration Functions

### 7.1 Watermark Callbacks (Production Path)

```cpp
// Leader callback: line 498-504
void register_for_leader_par_id_return(callback cb, uint32_t par_id) {
    leader_replay_cb[par_id] = cb;           // cache in map
    apply_callbacks_for_partition(par_id);    // wire to worker
}

// Follower callback: line 464-470
void register_for_follower_par_id_return(callback cb, uint32_t par_id) {
    follower_replay_cb[par_id] = cb;         // cache in map
    apply_callbacks_for_partition(par_id);    // wire to worker
}
```

Both functions follow the same pattern: store the callback in the
partition-keyed map, then call `apply_callbacks_for_partition()` which
wires both leader and follower callbacks to the correct `RaftWorker`.

**Why cache**: When leadership changes, `handle_leader_change_impl()` must
re-apply callbacks.  Without the maps, the system would lose the callback
reference after a leadership transition.

### 7.2 Simple Callbacks

```cpp
void register_for_follower(cb, par_id);        // line 444
void register_for_follower_par_id(cb, par_id); // line 454
void register_for_leader(cb, par_id);          // line 473
void register_for_leader_par_id(cb, par_id);   // line 488
```

These iterate `raft_workers_g` and register directly on workers that match
the partition and current leadership role.  They are used for simple test
scenarios but not for Mako's production watermark system.

**Subtle difference from watermark callbacks**: These check `IsLeader()` at
registration time and only register on matching workers.  The watermark
callbacks register on the partition worker regardless of current role and
let `RaftWorker::Next()` choose dynamically.

### 7.3 `register_leader_election_callback()`

```cpp
// raft_main_helper.cc:483-485
void register_leader_election_callback(std::function<void(int)> cb) {
    janus::leader_callback_ = std::move(cb);
}
```

Mako calls this to receive notifications when any partition gains or loses
leadership.  The callback is a simple `void(int)` where `1` means
"became leader" and `0` means "lost leadership".

## 8. NO-OP Entry System

### 8.1 `send_no_ops_for_mark(epoch)`

```cpp
// raft_main_helper.cc:38-47
void send_no_ops_for_mark(int epoch) {
    std::string log = "no-ops:" + std::to_string(epoch);
    for (auto& worker : raft_workers_g) {
        if (!worker || !worker->site_info_) continue;
        add_log_to_nc(log.c_str(), log.size(),
                      worker->site_info_->partition_id_, 1);
    }
}
```

Sends a `"no-ops:<epoch>"` entry through the Raft log for every partition.
These entries serve two purposes:

1. **Watermark synchronization**: Followers process NO-OP entries and
   update their local watermarks, ensuring all replicas have a consistent
   view of the epoch boundary.

2. **Epoch advancement**: When Mako transitions between epochs (e.g.,
   during shard reconfiguration), NO-OP entries mark the boundary in
   the replicated log.

The `batch_size=1` ensures each NO-OP is submitted individually rather
than batched, guaranteeing ordering with respect to regular log entries.

### 8.2 `send_no_ops_to_all_workers(epoch)`

```cpp
// raft_main_helper.cc:50-52
void send_no_ops_to_all_workers(int epoch) {
    send_no_ops_for_mark(epoch);
}
```

Compatibility alias used by Paxos-era call sites.

## 9. Epoch and Election State Functions

### 9.1 `ElectionState` Singleton

`ElectionState` (defined in `paxos_worker.h:696`) is a Paxos-originated
singleton that Mako uses for leader/epoch queries.  The Raft helper reuses
it for compatibility:

| Field | Type | Raft Usage |
|-------|------|------------|
| `machine_id` | `int` | Set to last worker's `locale_id` in `setup()` |
| `cur_epoch` | `int` | Epoch counter managed by `set_epoch()` |
| `cur_state` | `int` | 0=follower, 1=leader (set in `setup2()`) |
| `running` | `bool` | Set to `false` in `shutdown_paxos()` |
| `leader_id` | `int` | Set to 0 in `setup2()` |

### 9.2 `get_epoch()` / `set_epoch()`

```cpp
// raft_main_helper.cc:586-605
int get_epoch() { return es ? es->get_epoch() : 0; }

void set_epoch(int epoch) {
    if (epoch == -1) es->set_epoch();       // auto-increment
    else             es->set_epoch(epoch);
    for (auto& worker : raft_workers_g)
        worker->cur_epoch = es->get_epoch();  // propagate
}
```

`set_epoch(-1)` triggers `ElectionState::set_epoch()` with no arguments,
which auto-increments the epoch.  After updating `ElectionState`, the
new epoch is propagated to all workers' `cur_epoch` field.

### 9.3 `upgrade_p1_to_leader()`

```cpp
// raft_main_helper.cc:608-613
void upgrade_p1_to_leader() {
    Log_info("upgrade_p1_to_leader invoked for Raft helper.");
    if (::janus::leader_callback_)
        ::janus::leader_callback_(0);
}
```

Compatibility function for Paxos-era callers.  In Raft, leadership is
decided by elections, not manual promotion.  This function simply invokes
the leader callback (with `0`, not `1`) to trigger any Paxos-era startup
logic.

### 9.4 `get_outstanding_logs(par_id)`

```cpp
// raft_main_helper.cc:400-412
int get_outstanding_logs(uint32_t par_id) {
    auto* worker = find_worker(par_id);
    auto* raft_server = worker->GetRaftServer();
    return worker->n_tot.load() - raft_server->commitIndex;
}
```

Returns the number of log entries that have been submitted but not yet
committed by Raft consensus.  The formula `n_tot - commitIndex`
approximates the in-flight log count.  Used by Mako for backpressure.

## 10. Preferred Leader API

### 10.1 `set_preferred_leader(site_id)`

```cpp
// raft_main_helper.cc:650-679
void set_preferred_leader(int site_id) {
    siteid_t preferred = static_cast<siteid_t>(site_id);
    for (auto& worker : raft_workers_g) {
        auto raft_server = worker->GetRaftServer();
        raft_server->SetPreferredLeader(preferred);
    }
}
```

Runtime API for changing the preferred leader across all partitions.
Iterates all workers and calls `RaftServer::SetPreferredLeader()` on
each.  This triggers the monitoring thread on the current leader which
will transfer leadership to the new preferred node once it catches up.

This function is Raft-only and is dispatched from
`replication_helper.cc::set_preferred_leader()` only when
`is_using_raft()` is true.

## 11. Stub Functions

The following functions exist solely for link-time compatibility with
`paxos_impl`.  They log warnings and return default values:

| Function | Lines | Behavior |
|----------|-------|----------|
| `microbench_paxos()` | 439-441 | Log warning, return |
| `microbench_paxos_queue()` | 567-569 | Log warning, return |
| `worker_info_stats()` | 616-626 | Dumps partition counters (not a stub) |
| `getHosts()` | 377-397 | Fully implemented YAML parser |

## 12. Function Reference Table

| Function | Lines | Scope | Category |
|----------|-------|-------|----------|
| `setup()` | 239-285 | `raft_impl` | Lifecycle |
| `setup2()` | 297-374 | `raft_impl` | Lifecycle |
| `shutdown_paxos()` | 415-436 | `raft_impl` | Lifecycle |
| `pre_shutdown_step()` | 572-583 | `raft_impl` | Lifecycle |
| `server_launch_worker()` | 116-178 | anonymous | Lifecycle (internal) |
| `add_log_to_nc()` | 534-553 | `raft_impl` | Log submission |
| `submit()` | 510-521 | `raft_impl` | Log submission |
| `add_log()` | 524-526 | `raft_impl` | Log submission |
| `add_log_without_queue()` | 529-531 | `raft_impl` | Log submission |
| `wait_for_submit()` | 557-564 | `raft_impl` | Log submission |
| `register_for_follower()` | 444-451 | `raft_impl` | Callback |
| `register_for_follower_par_id()` | 454-461 | `raft_impl` | Callback |
| `register_for_follower_par_id_return()` | 464-470 | `raft_impl` | Callback |
| `register_for_leader()` | 473-480 | `raft_impl` | Callback |
| `register_for_leader_par_id()` | 488-495 | `raft_impl` | Callback |
| `register_for_leader_par_id_return()` | 498-504 | `raft_impl` | Callback |
| `register_leader_election_callback()` | 483-485 | `raft_impl` | Callback |
| `send_no_ops_for_mark()` | 38-47 | `raft_impl` | NO-OP |
| `send_no_ops_to_all_workers()` | 50-52 | `raft_impl` | NO-OP |
| `get_epoch()` | 586-588 | `raft_impl` | Epoch |
| `set_epoch()` | 591-605 | `raft_impl` | Epoch |
| `upgrade_p1_to_leader()` | 608-613 | `raft_impl` | Epoch |
| `get_outstanding_logs()` | 400-412 | `raft_impl` | Query |
| `set_preferred_leader()` | 650-679 | `raft_impl` | Preferred leader |
| `getHosts()` | 377-397 | `raft_impl` | Config |
| `worker_info_stats()` | 616-626 | `raft_impl` | Debug |
| `find_worker()` | 181-188 | anonymous | Internal |
| `enqueue_to_worker()` | 191-205 | anonymous | Internal |
| `apply_callbacks_for_partition()` | 209-226 | anonymous | Internal |
| `handle_leader_change_impl()` | 233-236 | `raft_impl` | Internal |
| `wait_for_local_leadership()` | 68-105 | anonymous | Internal |
| `check_current_path()` | 108-113 | anonymous | Internal |
| `raft_handle_leader_change()` | 732-740 | `janus` | Leader change |
| `NotifyRaftLeaderChange()` | 744-748 | `janus` | Leader change |

## 13. Comparison with `paxos_main_helper.cc`

| Aspect | `paxos_main_helper.cc` | `raft_main_helper.cc` |
|--------|------------------------|----------------------|
| Worker type | `PaxosWorker` | `RaftWorker` |
| Worker vector | `pxs_workers_g` | `raft_workers_g` |
| Callback maps | `leader_replay_cb` only | Both `leader_replay_cb` and `follower_replay_cb` |
| Leader selection | Fixed (`action` parameter in `setup2()`) | Raft election with preferred bias |
| NO-OP format | Paxos-native NO-OP slots | `"no-ops:<epoch>"` string entries |
| `setup2()` | Creates submit pool, starts bulk coordinators | Configures preferred leader, launches workers |
| Jetpack | Respects environment | Forces `MAKO_DISABLE_JETPACK=1` |
| Submit mechanism | `_BulkSubmit()` via coordinator | `EnqueueLog()` → `SubmitLoop()` → `Start()` |
| Shutdown | Similar 2-phase pattern | Similar 2-phase pattern |
| Leader change | Via `ElectionState` singleton | Via `RegisterLeaderChangeCallback` + `NotifyRaftLeaderChange` |
| Lines of code | ~900 | 751 |
