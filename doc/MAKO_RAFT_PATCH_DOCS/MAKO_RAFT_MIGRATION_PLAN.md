# Mako Raft Migration Plan

**Document Version:** 1.0
**Created:** 2025-10-30
**Status:** Planning Phase
**Target:** Replace Paxos with Raft in Mako for log replication

---

## Table of Contents

1. [Executive Summary](#executive-summary)
2. [Background and Motivation](#background-and-motivation)
3. [Current Architecture Analysis](#current-architecture-analysis)
4. [Proposed Architecture](#proposed-architecture)
5. [Implementation Plan](#implementation-plan)
6. [Testing Strategy](#testing-strategy)
7. [Risk Assessment](#risk-assessment)
8. [Timeline and Milestones](#timeline-and-milestones)
9. [Success Criteria](#success-criteria)
10. [References](#references)

---

## Executive Summary

### Objective
Migrate Mako's replication layer from Multi-Paxos to Raft while preserving the watermark-based speculative execution system and maintaining performance characteristics.

### Scope
- Replace `src/deptran/paxos_main_helper.cc` implementation with Raft-based equivalent
- Create RaftWorker wrapper around existing RaftServer
- Integrate Raft log application with Mako's watermark callbacks
- Maintain backward compatibility with Paxos (conditional compilation)

### Key Benefits
- **Simpler protocol:** Raft is more understandable and debuggable than Paxos
- **Better tooling:** Leverage existing Raft implementation in codebase
- **Unified codebase:** Janus uses Paxos, Mako can use Raft for comparison
- **Research flexibility:** Easier to experiment with protocol variations

### Non-Goals
- **NOT changing Mako's core design:** Watermark system remains unchanged
- **NOT modifying transaction protocols:** Only replication layer changes
- **NOT removing Paxos:** Keep both implementations available

---

## Background and Motivation

### Why Raft?

1. **Existing Infrastructure:** The codebase already has a complete Raft implementation (`src/deptran/raft/`) with:
   - Leader election
   - Log replication
   - Safety guarantees
   - Test harness

2. **Simplicity:** Raft's understandability makes debugging and extending easier

3. **Research Value:** Comparing Paxos vs Raft in the same system provides research insights

### Current Paxos Usage in Mako

Mako uses Multi-Paxos for **replication log consensus**, not transaction consensus:

```
Leader:
  Transaction Commits → Log to Paxos (async) → Update Watermark

Follower:
  Receive Paxos Log → Safety Check (watermark) → Replay or Queue
```

Key insight: Paxos provides **ordered, durable log replication**. Raft can provide the same guarantee with simpler semantics.

---

## Current Architecture Analysis

### Paxos Integration Points

#### 1. Main Entry Point: `src/deptran/paxos_main_helper.cc`

**Key Functions:**
- `setup(argc, argv)` - Initialize Paxos workers
- `register_for_leader_par_id_return(callback, thread_id)` - Leader commit callback
- `register_for_follower_par_id_return(callback, thread_id)` - Follower replay callback
- `add_log_to_nc(log, len, par_id, batch_size)` - Submit log to Paxos
- `get_outstanding_logs(par_id)` - Query uncommitted logs
- `shutdown_paxos()` - Cleanup

#### 2. Data Structures

```cpp
// Global state
vector<shared_ptr<PaxosWorker>> pxs_workers_g;
map<int, function<int(const char*&, int, int, int, queue&)>> leader_replay_cb;
shared_ptr<ElectionState> es;
```

#### 3. PaxosWorker Responsibilities

```cpp
class PaxosWorker {
  PaxosServer* rep_sched_;           // Paxos protocol instance
  Config::SiteInfo* site_info_;      // Partition/site configuration
  bool is_leader;                    // Leadership status

  void Submit(log, len, par_id);     // Submit to Paxos
  void register_apply_callback(...); // Register callback
  bool IsLeader(par_id);             // Check leadership
  bool IsPartition(par_id);          // Check partition ownership
};
```

#### 4. Callback Flow

**Leader Callback:**
```cpp
register_for_leader_par_id_return([](const char*& log, int len,
                                     int par_id, int slot_id, queue& un_replay_logs_) {
    // 1. Extract commit info
    CommitInfo commit_info = get_latest_commit_info(log, len);

    // 2. Update local watermark
    sync_util::sync_logger::local_timestamp_[par_id].store(
        commit_info.timestamp, memory_order_release
    );

    // 3. Return encoded status
    return timestamp * 10 + status;
}, thread_id);
```

**Follower Callback:**
```cpp
register_for_follower_par_id_return([](const char*& log, int len,
                                       int par_id, int slot_id, queue& un_replay_logs_) {
    // 1. Extract timestamp
    CommitInfo commit_info = get_latest_commit_info(log, len);

    // 2. Update local watermark
    sync_util::sync_logger::local_timestamp_[par_id].store(
        commit_info.timestamp, memory_order_release
    );

    // 3. Safety check
    uint32_t w = sync_util::sync_logger::retrieveW();
    if (sync_util::sync_logger::safety_check(commit_info.timestamp, w)) {
        // Replay immediately
        treplay_in_same_thread_opt_mbta_v2(par_id, log, len, db, nthreads);
        return STATUS_REPLAY_DONE;
    } else {
        // Queue for later
        un_replay_logs_.push(make_tuple(timestamp, slot_id, par_id, len, log));
        return STATUS_SAFETY_FAIL;
    }
}, thread_id);
```

#### 5. Special Message Types

**NO-OPS:** Watermark synchronization markers
```cpp
// Format: "no-ops:X" where X is epoch number
if (isNoops(log, len) != -1) {
    // Synchronize watermarks across shards
    // Exchange via NFS sync
    // Replay queued transactions
}
```

**ADVANCER_MARKER:** Start watermark advancement thread
```cpp
if (len == ADVANCER_MARKER_NUM) {
    sync_util::sync_logger::start_advancer();
}
```

**ENDING:** Shutdown signal
```cpp
if (len == 0) {
    // Set watermark to max to unblock everything
    local_timestamp_[par_id].store(max_value, memory_order_release);
}
```

### Raft Infrastructure (Existing)

#### 1. Core Raft Implementation: `src/deptran/raft/`

**Files:**
- `server.h/cc` - RaftServer (state machine, elections, replication)
- `coordinator.h/cc` - Client request coordination
- `commo.h/cc` - RPC communication layer
- `frame.h/cc` - Framework integration
- `service.h/cc` - RPC service handlers

#### 2. RaftServer Key Methods

```cpp
class RaftServer : public TxLogServer {
  // Client submission
  void Start(shared_ptr<Marshallable> cmd, int* index, int* term);

  // Log application
  void applyLogs();  // Called when commitIndex advances

  // Election
  bool RequestVote();
  void setIsLeader(bool isLeader);

  // Replication
  void HeartbeatLoop();
  void SendAppendEntries(...);

  // State
  bool is_leader_;
  ballot_t currentTerm;
  slotid_t commitIndex;
  slotid_t executeIndex;
  map<slotid_t, shared_ptr<RaftData>> raft_logs_;

  // Callback
  function<int(int slot, shared_ptr<Marshallable> cmd)> app_next_;
};
```

#### 3. Raft Stub Implementation: `src/deptran/raft_main_helper.cc`

**Current Status:** All functions are `[RAFT_STUB]` - not implemented!

```cpp
void register_for_follower_par_id_return(...) {
  std::cerr << "[RAFT_STUB] register_for_follower_par_id_return() not yet implemented.\n";
}

void register_for_leader_par_id_return(...) {
  std::cerr << "[RAFT_STUB] register_for_leader_par_id_return() not yet implemented.\n";
}

void add_log_to_nc(...) {
  std::cerr << "[RAFT_STUB] add_log_to_nc() not yet implemented.\n";
}
```

**This is what we need to implement!**

---

## Proposed Architecture

### Paxos → Raft Mapping

| Paxos Concept | Raft Equivalent | Notes |
|---------------|-----------------|-------|
| `PaxosWorker` | `RaftWorker` (new) | Wrapper around RaftServer |
| `PaxosServer` | `RaftServer` (exists) | Core protocol implementation |
| `Submit(log)` | `Start(log)` | Add log to replicated log |
| `OnCommit(slot)` | `applyLogs()` when `commitIndex` advances | Apply committed logs |
| `max_committed_slot_` | `commitIndex` | Highest committed index |
| `max_executed_slot_` | `executeIndex` | Highest applied index |
| `app_next_(slot, cmd)` | Same callback interface | Apply log entry callback |
| Paxos epochs | Raft terms | Leadership epochs |
| Paxos ballot | Raft term | Ordering/versioning |

### High-Level Data Flow

```
┌─────────────────────────────────────────────────────────────────┐
│                    Mako + Raft Architecture                      │
└─────────────────────────────────────────────────────────────────┘

CLIENT TRANSACTION
      |
      v
┌─────────────────────┐
│ Mako Leader Shard   │
│                     │
│  1. Execute Txn     │────► Masstree (speculative)
│     (speculative)   │
│                     │
│  2. Serialize Log   │
│     add_log_to_nc() │
│         |           │
│         v           │
│  ┌───────────────┐  │
│  │ RaftWorker    │  │
│  │   Submit()    │  │
│  │       |       │  │
│  │       v       │  │
│  │ RaftServer    │  │
│  │   Start()     │  │
│  └───────┬───────┘  │
│          |          │
│    3. Replicate     │
│       via Raft      │
└──────────┬──────────┘
           |
           | AppendEntries RPCs
           |
           v
┌─────────────────────┐         ┌─────────────────────┐
│ Follower Shard 1    │         │ Follower Shard 2    │
│                     │         │                     │
│  RaftServer         │         │  RaftServer         │
│    OnAppendEntries()│         │    OnAppendEntries()│
│         |           │         │         |           │
│         v           │         │         v           │
│    commitIndex++    │         │    commitIndex++    │
│         |           │         │         |           │
│         v           │         │         v           │
│    applyLogs()      │         │    applyLogs()      │
│         |           │         │         |           │
│         v           │         │         v           │
│  app_next_(slot,cmd)│         │  app_next_(slot,cmd)│
│         |           │         │         |           │
│         v           │         │         v           │
│  Follower Callback  │         │  Follower Callback  │
│    - Extract ts     │         │    - Extract ts     │
│    - Safety check   │         │    - Safety check   │
│    - Replay or Queue│         │    - Replay or Queue│
│         |           │         │         |           │
│         v           │         │         v           │
│    Masstree         │         │    Masstree         │
│    (replayed)       │         │    (replayed)       │
└─────────────────────┘         └─────────────────────┘
```

### Component Interaction Diagram

```
┌──────────────────────────────────────────────────────────────────┐
│                         raft_main_helper.cc                       │
│                                                                   │
│  Global State:                                                    │
│    vector<shared_ptr<RaftWorker>> raft_workers_g                 │
│    map<int, function<...>> leader_replay_cb                      │
│    map<int, function<...>> follower_replay_cb                    │
│                                                                   │
│  API Functions:                                                   │
│    register_for_leader_par_id_return(callback, thread_id)        │
│    register_for_follower_par_id_return(callback, thread_id)      │
│    add_log_to_nc(log, len, par_id, batch_size)                   │
│    setup(argc, argv)                                              │
│    shutdown_paxos()                                               │
└────────────────────────┬─────────────────────────────────────────┘
                         |
                         | creates & manages
                         v
              ┌─────────────────────┐
              │    RaftWorker       │
              │  (one per partition)│
              │                     │
              │  - site_info_       │
              │  - raft_sched_      │───────────►┌─────────────────┐
              │  - apply_callback_  │            │   RaftServer    │
              │                     │            │                 │
              │  Submit(log)        │──calls────►│  Start(cmd)     │
              │  IsLeader()         │◄──queries──│  is_leader_     │
              │  register_callback()│            │  commitIndex    │
              └─────────────────────┘            │  executeIndex   │
                                                 │  applyLogs()    │
                                                 │  app_next_()    │
                                                 └────────┬────────┘
                                                          |
                                        invokes when commitIndex advances
                                                          |
                                                          v
                                             ┌────────────────────────┐
                                             │  Mako Watermark        │
                                             │  Callback              │
                                             │                        │
                                             │  Leader:               │
                                             │    update watermark    │
                                             │                        │
                                             │  Follower:             │
                                             │    safety_check()      │
                                             │    replay or queue     │
                                             └────────────────────────┘
```

---

## Implementation Plan

### Phase 1: Foundation (Days 1-2)

#### Step 1.1: Create RaftWorker Class

**Files to Create:**
- `src/deptran/raft/raft_worker.h`
- `src/deptran/raft/raft_worker.cc`

**Class Definition:**

```cpp
// raft_worker.h
#pragma once

#include "../server_worker.h"
#include "server.h"
#include <functional>
#include <memory>
#include <queue>
#include <tuple>

namespace janus {

class RaftWorker {
public:
  RaftWorker();
  ~RaftWorker();

  // Configuration
  Config::SiteInfo* site_info_ = nullptr;
  RaftServer* raft_sched_ = nullptr;

  // Leadership and partition management
  bool IsLeader(uint32_t par_id);
  bool IsPartition(uint32_t par_id);

  // Log submission
  void Submit(const char* log, int len, uint32_t par_id);
  void IncSubmit();
  void WaitForSubmit();

  // Callback registration
  void register_apply_callback_par_id_return(
    std::function<int(const char*&, int, int, int,
                      std::queue<std::tuple<int, int, int, int, const char*>>&)> cb
  );

  // Initialization
  void SetupBase();
  void SetupService();
  void SetupCommo();
  void SetupHeartbeat();

  // Stats
  size_t n_tot = 0;          // Total submitted
  size_t submit_num = 0;     // Current submitted
  size_t tot_num = 0;        // Target number

private:
  std::function<int(const char*&, int, int, int,
                    std::queue<std::tuple<int, int, int, int, const char*>>&)>
    apply_callback_;

  std::mutex submit_mutex_;
  std::condition_variable submit_cv_;

  // Queue for unreplayed logs (follower side)
  std::queue<std::tuple<int, int, int, int, const char*>> un_replay_logs_;
};

} // namespace janus
```

**Key Implementation Details:**

```cpp
// raft_worker.cc

#include "raft_worker.h"
#include "server.h"
#include "../config.h"

namespace janus {

RaftWorker::RaftWorker() {
  // Constructor
}

RaftWorker::~RaftWorker() {
  // Cleanup
}

bool RaftWorker::IsLeader(uint32_t par_id) {
  if (site_info_->partition_id_ != par_id) return false;
  return raft_sched_ && raft_sched_->is_leader_;
}

bool RaftWorker::IsPartition(uint32_t par_id) {
  return site_info_->partition_id_ == par_id;
}

void RaftWorker::Submit(const char* log, int len, uint32_t par_id) {
  if (!IsLeader(par_id)) {
    Log_info("Not leader for partition %d, ignoring submit", par_id);
    return;
  }

  // Create Marshallable command from log
  auto cmd = std::make_shared<TpcCommitCommand>();
  // TODO: Deserialize log into cmd

  int index, term;
  raft_sched_->Start(cmd, &index, &term);

  n_tot++;
}

void RaftWorker::register_apply_callback_par_id_return(
    std::function<int(const char*&, int, int, int,
                      std::queue<std::tuple<int, int, int, int, const char*>>&)> cb) {
  apply_callback_ = cb;

  // Register with RaftServer's app_next_
  raft_sched_->app_next_ = [this, cb](int slot, shared_ptr<Marshallable> cmd) -> int {
    // Serialize cmd to char* log
    // TODO: Implement serialization
    const char* log = nullptr;
    int len = 0;
    int par_id = this->site_info_->partition_id_;

    // Call Mako's callback
    int status = cb(log, len, par_id, slot, un_replay_logs_);

    return status;
  };
}

void RaftWorker::SetupBase() {
  // Create RaftServer instance
  // Similar to PaxosWorker::SetupBase()
  Frame* frame = Frame::GetFrame(Config::GetConfig()->tx_proto_);
  raft_sched_ = dynamic_cast<RaftServer*>(
    frame->CreateScheduler(site_info_->partition_id_)
  );
  // ... additional setup
}

} // namespace janus
```

**Checklist:**
- [ ] Implement RaftWorker constructor/destructor
- [ ] Implement IsLeader() and IsPartition()
- [ ] Implement Submit() that calls RaftServer::Start()
- [ ] Implement register_apply_callback_par_id_return()
- [ ] Wire app_next_ callback to Mako's watermark logic
- [ ] Implement SetupBase(), SetupService(), SetupCommo()
- [ ] Handle serialization/deserialization of logs

---

#### Step 1.2: Implement setup() in raft_main_helper.cc

**File:** `src/deptran/raft_main_helper.cc`

**Implementation:**

```cpp
#include "raft_main_helper.h"
#include "raft/raft_worker.h"
#include "config.h"
#include <boost/filesystem.hpp>

using namespace janus;

// Global state
vector<shared_ptr<RaftWorker>> raft_workers_g = {};
map<int, function<int(const char*&, int, int, int,
                      queue<tuple<int, int, int, int, const char*>>&)>> leader_replay_cb;
map<int, function<int(const char*&, int, int, int,
                      queue<tuple<int, int, int, int, const char*>>&)>> follower_replay_cb;

// Election state (if needed for failover)
struct ElectionState {
  int machine_id = 0;
  int epoch = 0;
  // ... additional state
};
shared_ptr<ElectionState> es = make_shared<ElectionState>();

void check_current_path() {
  auto path = boost::filesystem::current_path();
  Log_info("PWD : %s", path.string().c_str());
}

vector<string> setup(int argc, char* argv[]) {
  vector<string> retVector;
  check_current_path();
  Log_info("starting Raft process %ld", getpid());

  // Parse configuration
  int ret = Config::CreateConfig(argc, argv);
  if (ret != SUCCESS) {
    Log_fatal("Read config failed");
    return retVector;
  }

  // Get server info
  auto server_infos = Config::GetConfig()->GetMyServers();
  Log_info("server_infos, number of sites: %d, proc_name: %s",
           server_infos.size(),
           Config::GetConfig()->proc_name_.c_str());

  // Create RaftWorker for each partition
  for (int i = server_infos.size() - 1; i >= 0; i--) {
    retVector.push_back(Config::GetConfig()->SiteById(server_infos[i].id).name);

    RaftWorker* worker = new RaftWorker();
    raft_workers_g.push_back(shared_ptr<RaftWorker>(worker));
    raft_workers_g.back()->site_info_ =
      const_cast<Config::SiteInfo*>(&(Config::GetConfig()->SiteById(server_infos[i].id)));

    Log_info("partition id of each Raft group is %d, site-name: %s, site-id: %d",
             raft_workers_g.back()->site_info_->partition_id_,
             server_infos[i].name.c_str(),
             server_infos[i].id);

    // Setup frame and scheduler
    raft_workers_g.back()->SetupBase();
  }

  // Reverse order (same as Paxos)
  reverse(raft_workers_g.begin(), raft_workers_g.end());

  es->machine_id = raft_workers_g.back()->site_info_->locale_id;
  Log_info("running machine-id: %d", es->machine_id);

  return retVector;
}
```

**Checklist:**
- [ ] Parse configuration files
- [ ] Create RaftWorker instances for each partition
- [ ] Initialize site_info_ for each worker
- [ ] Call SetupBase() for each worker
- [ ] Return process names vector

---

### Phase 2: Core Replication API (Days 2-3)

#### Step 2.1: Implement add_log_to_nc()

**Purpose:** Submit transaction logs to Raft for replication

**Implementation:**

```cpp
void add_log_to_nc(const char* log, int len, uint32_t par_id, int batch_size) {
  Log_debug("add_log_to_nc, par_id:%d, len:%d", par_id, len);

  // Find worker for this partition
  if (par_id >= raft_workers_g.size()) {
    Log_error("Invalid partition id: %d", par_id);
    return;
  }

  auto& worker = raft_workers_g[par_id];

  // Check if this worker is leader for this partition
  if (!worker->IsLeader(par_id)) {
    Log_debug("Not leader for partition %d, ignoring submit", par_id);
    return;
  }

  // Submit to Raft
  worker->Submit(log, len, par_id);
}
```

**Checklist:**
- [ ] Validate partition ID
- [ ] Find RaftWorker for partition
- [ ] Check leadership status
- [ ] Call worker->Submit()
- [ ] Handle errors gracefully

---

#### Step 2.2: Implement Callback Registration

**Leader Callback:**

```cpp
void register_for_leader_par_id_return(
    function<int(const char*&, int, int, int,
                 queue<tuple<int, int, int, int, const char*>>&)> cb,
    uint32_t par_id) {

  Log_info("Registering leader callback for partition %d", par_id);

  // Store callback for later use (e.g., after election)
  leader_replay_cb[par_id] = cb;

  // Register with worker if this is the leader
  for (auto& worker : raft_workers_g) {
    if (worker->IsPartition(par_id) && worker->IsLeader(par_id)) {
      worker->register_apply_callback_par_id_return(cb);
      Log_info("Registered leader callback for partition %d", par_id);
      break;
    }
  }
}
```

**Follower Callback:**

```cpp
void register_for_follower_par_id_return(
    function<int(const char*&, int, int, int,
                 queue<tuple<int, int, int, int, const char*>>&)> cb,
    uint32_t par_id) {

  Log_info("Registering follower callback for partition %d", par_id);

  // Store callback
  follower_replay_cb[par_id] = cb;

  // Register with worker if this is a follower
  for (auto& worker : raft_workers_g) {
    if (worker->IsPartition(par_id) && !worker->IsLeader(par_id)) {
      worker->register_apply_callback_par_id_return(cb);
      Log_info("Registered follower callback for partition %d", par_id);
      break;
    }
  }
}
```

**Checklist:**
- [ ] Store callbacks in global maps
- [ ] Find correct RaftWorker for partition
- [ ] Check leader/follower status
- [ ] Call worker->register_apply_callback_par_id_return()
- [ ] Add logging for debugging

---

#### Step 2.3: Implement Helper Functions

**Get Outstanding Logs:**

```cpp
int get_outstanding_logs(uint32_t par_id) {
  for (auto& worker : raft_workers_g) {
    if (worker->IsPartition(par_id)) {
      auto rs = worker->raft_sched_;
      // Outstanding = submitted - committed
      return (int)worker->n_tot - (int)rs->commitIndex;
    }
  }
  return -1;
}
```

**Shutdown:**

```cpp
int shutdown_paxos() {
  Log_info("Shutting down Raft workers");

  // Stop all Raft workers
  for (auto& worker : raft_workers_g) {
    if (worker->raft_sched_) {
      worker->raft_sched_->stop_ = true;
    }
  }

  // Wait for cleanup
  // TODO: Join threads, cleanup resources

  raft_workers_g.clear();
  return 0;
}
```

**Epoch Management:**

```cpp
int get_epoch() {
  if (raft_workers_g.empty()) return 0;
  // In Raft, "epoch" maps to "term"
  return (int)raft_workers_g.back()->raft_sched_->currentTerm;
}

void set_epoch(int v) {
  if (v == -1) {
    es->epoch++;
  } else {
    es->epoch = v;
  }

  // Update all workers
  for (auto& worker : raft_workers_g) {
    // Raft manages terms internally, but we can track epochs separately
    // for compatibility with Mako's epoch-based logic
  }
}
```

**Get Hosts:**

```cpp
map<string, string> getHosts(string filename) {
  map<string, string> proc_host_map_;
  YAML::Node config = YAML::LoadFile(filename);

  if (config["host"]) {
    auto node = config["host"];
    for (auto it = node.begin(); it != node.end(); it++) {
      auto proc_name = it->first.as<string>();
      auto host_name = it->second.as<string>();
      proc_host_map_[proc_name] = host_name;
    }
  } else {
    cerr << "No host attribute in YAML: " << filename << endl;
    exit(1);
  }

  return proc_host_map_;
}
```

**Checklist:**
- [ ] Implement get_outstanding_logs()
- [ ] Implement shutdown_paxos() with proper cleanup
- [ ] Implement get_epoch() / set_epoch()
- [ ] Implement getHosts() for YAML parsing
- [ ] Implement microbench_paxos() (optional)
- [ ] Implement worker_info_stats() (optional)

---

### Phase 3: RaftServer Integration (Days 3-4)

#### Step 3.1: Modify RaftServer::applyLogs()

**File:** `src/deptran/raft/server.cc`

**Current Implementation:**

```cpp
void RaftServer::applyLogs() {
  in_applying_logs_ = true;

  for (slotid_t id = executeIndex + 1; id <= commitIndex; id++) {
    auto next_instance = GetRaftInstance(id);
    if (next_instance && next_instance->log_) {
      app_next_(id, next_instance->log_);
      executeIndex = id;
    } else {
      break;
    }
  }

  in_applying_logs_ = false;
}
```

**Enhanced Implementation:**

```cpp
void RaftServer::applyLogs() {
  in_applying_logs_ = true;

  Log_debug("applyLogs: executeIndex=%lu, commitIndex=%lu",
            executeIndex, commitIndex);

  for (slotid_t id = executeIndex + 1; id <= commitIndex; id++) {
    auto next_instance = GetRaftInstance(id);

    if (next_instance && next_instance->log_) {
      // Call application callback
      int status = app_next_(id, next_instance->log_);

      // Handle return status
      if (status < 0) {
        // Error occurred, log and continue
        Log_error("app_next_ returned error status %d for slot %lu", status, id);
      } else {
        // Decode status: timestamp * 10 + status_code
        int status_code = status % 10;
        uint32_t timestamp = status / 10;

        if (status_code == mako::PaxosStatus::STATUS_REPLAY_DONE) {
          // Successfully replayed
          Log_debug("Successfully applied log at slot %lu, timestamp %u",
                    id, timestamp);
        } else if (status_code == mako::PaxosStatus::STATUS_SAFETY_FAIL) {
          // Queued for later due to watermark
          Log_debug("Queued log at slot %lu, timestamp %u (watermark not ready)",
                    id, timestamp);
        }
      }

      // Advance executeIndex
      executeIndex = id;

      // Optionally: garbage collect old log entries
      removeCmd(id);

    } else {
      // Gap in log, should not happen in normal operation
      Log_error("Gap in log at slot %lu", id);
      break;
    }
  }

  in_applying_logs_ = false;

  Log_debug("applyLogs completed: executeIndex=%lu", executeIndex);
}
```

**Checklist:**
- [ ] Enhance applyLogs() with status code handling
- [ ] Add logging for debugging
- [ ] Handle watermark-based queueing status
- [ ] Implement log garbage collection (removeCmd)
- [ ] Handle gaps in log gracefully

---

#### Step 3.2: Enhance RaftServer Callback Setup

**File:** `src/deptran/raft/server.h`

**Add method to set callback:**

```cpp
class RaftServer : public TxLogServer {
public:
  // ... existing methods ...

  void SetApplicationCallback(
    function<int(slotid_t, shared_ptr<Marshallable>)> callback
  ) {
    app_next_ = callback;
  }

  // Existing callback
  function<int(slotid_t, shared_ptr<Marshallable>)> app_next_{};
};
```

**Checklist:**
- [ ] Add SetApplicationCallback() method
- [ ] Ensure app_next_ is properly initialized
- [ ] Document callback signature and return values

---

### Phase 4: Watermark Integration (Days 4-5)

#### Step 4.1: Leader Callback Watermark Logic

**Location:** Already implemented in `src/mako/mako.hh` (register_paxos_leader_callback)

**Key Points:**
- Extract timestamp from log
- Update `sync_util::sync_logger::local_timestamp_[par_id]`
- Return encoded status

**No changes needed** - the existing Mako callback should work with Raft!

```cpp
// Already in mako.hh lines 289-375
register_for_leader_par_id_return([&,thread_id](...) {
  CommitInfo commit_info = get_latest_commit_info((char*)log, len);
  uint32_t timestamp = commit_info.timestamp;

  // Update watermark
  sync_util::sync_logger::local_timestamp_[par_id].store(
    commit_info.timestamp, memory_order_release
  );

  return static_cast<int>(timestamp * 10 + status);
}, thread_id);
```

---

#### Step 4.2: Follower Callback Watermark Logic

**Location:** Already implemented in `src/mako/mako.hh` (register_paxos_follower_callback)

**Key Points:**
- Extract timestamp from log
- Update local_timestamp_[par_id]
- Safety check against watermark
- Replay immediately or queue
- Process queued logs when watermark advances

**No changes needed** - the existing Mako callback should work with Raft!

```cpp
// Already in mako.hh lines 128-285
register_for_follower_par_id_return([&,thread_id](...) {
  CommitInfo commit_info = get_latest_commit_info((char*)log, len);
  uint32_t timestamp = commit_info.timestamp;

  sync_util::sync_logger::local_timestamp_[par_id].store(
    commit_info.timestamp, memory_order_release
  );

  uint32_t w = sync_util::sync_logger::retrieveW();

  if (sync_util::sync_logger::safety_check(commit_info.timestamp, w)) {
    // Replay now
    treplay_in_same_thread_opt_mbta_v2(...);
    return STATUS_REPLAY_DONE;
  } else {
    // Queue for later
    un_replay_logs_.push(...);
    return STATUS_SAFETY_FAIL;
  }
}, thread_id);
```

---

#### Step 4.3: NO-OPS Handling

**Location:** Already implemented in `src/mako/mako.hh`

**Implementation:**
- Detect NO-OPS: `isNoops(log, len) != -1`
- Synchronize all threads
- Compute local watermark
- Exchange with other shards
- Replay queued transactions

**To verify:**
- [ ] Ensure NO-OPS are submitted via add_log_to_nc()
- [ ] Verify watermark synchronization works with Raft
- [ ] Test cross-shard watermark exchange

---

### Phase 5: Build System (Day 5)

#### Step 5.1: Update CMakeLists.txt

**File:** `CMakeLists.txt`

**Add option and conditional compilation:**

```cmake
# Mako Raft option
option(MAKO_USE_RAFT "Use Raft instead of Paxos for Mako replication" OFF)

if(MAKO_USE_RAFT)
  message(STATUS "Building Mako with Raft")
  add_definitions(-DMAKO_USE_RAFT)

  set(RAFT_HELPER_SOURCES
    src/deptran/raft/server.cc
    src/deptran/raft/coordinator.cc
    src/deptran/raft/commo.cc
    src/deptran/raft/frame.cc
    src/deptran/raft/service.cc
    src/deptran/raft/raft_worker.cc
    src/deptran/raft_main_helper.cc
  )
else()
  message(STATUS "Building Mako with Paxos")
  set(RAFT_HELPER_SOURCES
    src/deptran/paxos_main_helper.cc
  )
endif()

# Add to deptran library
add_library(deptran
  # ... existing sources ...
  ${RAFT_HELPER_SOURCES}
)
```

**Checklist:**
- [ ] Add MAKO_USE_RAFT option
- [ ] Conditionally include raft_worker.cc
- [ ] Conditionally include raft_main_helper.cc
- [ ] Keep paxos_main_helper.cc for default builds
- [ ] Test both build configurations

---

#### Step 5.2: Update Makefile

**File:** `Makefile`

**Add conditional compilation:**

```makefile
# Option to use Raft
MAKO_USE_RAFT ?= 0

ifeq ($(MAKO_USE_RAFT),1)
  CXXFLAGS += -DMAKO_USE_RAFT
  RAFT_SOURCES = \
    src/deptran/raft/server.cc \
    src/deptran/raft/coordinator.cc \
    src/deptran/raft/commo.cc \
    src/deptran/raft/frame.cc \
    src/deptran/raft/service.cc \
    src/deptran/raft/raft_worker.cc \
    src/deptran/raft_main_helper.cc
else
  RAFT_SOURCES = src/deptran/paxos_main_helper.cc
endif

# Add to sources
DEPTRAN_SOURCES += $(RAFT_SOURCES)
```

**Checklist:**
- [ ] Add MAKO_USE_RAFT flag
- [ ] Conditional source inclusion
- [ ] Test: `make MAKO_USE_RAFT=1`

---

### Phase 6: Configuration Files (Day 5)

#### Step 6.1: Create Raft Configuration

**File:** `config/mako_raft.yml`

```yaml
# Mako with Raft replication
bench: mako
mode: raft
protocol: none  # or rule for Jetpack

# Raft-specific parameters
raft:
  heartbeat_interval_ms: 5
  election_timeout_min_ms: 400
  election_timeout_max_ms: 700

# Existing Mako parameters
batch_size: 10000
duration: 30
```

**Checklist:**
- [ ] Create mako_raft.yml
- [ ] Set mode: raft
- [ ] Configure heartbeat and election timeouts
- [ ] Keep existing Mako parameters

---

#### Step 6.2: Create Topology Configuration

**File:** `config/mako_1c1s3r.yml` (1 client, 1 shard, 3 replicas)

```yaml
# Topology: 1 client, 1 shard, 3 Raft replicas

host:
  server0: 127.0.0.1
  server1: 127.0.0.1
  server2: 127.0.0.1
  client0: 127.0.0.1

site:
  # Shard 0, 3 replicas
  - host: server0
    port: 9000
    partition: 0
    replica: 0
    role: leader

  - host: server1
    port: 9001
    partition: 0
    replica: 1
    role: follower

  - host: server2
    port: 9002
    partition: 0
    replica: 2
    role: follower

client:
  - host: client0
    port: 10000
    num_threads: 1
```

**Checklist:**
- [ ] Create topology files for different scenarios
- [ ] 1c1s3r.yml (basic replication)
- [ ] 2c2s3r.yml (multi-shard)
- [ ] failover.yml (failure injection)

---

## Testing Strategy

### Phase 7: Unit Testing (Days 6-7)

#### Test 7.1: RaftWorker Unit Tests

**File:** `test/raft_worker_test.cc`

**Tests:**
- [ ] Test RaftWorker creation and initialization
- [ ] Test IsLeader() and IsPartition()
- [ ] Test Submit() calls RaftServer::Start()
- [ ] Test callback registration
- [ ] Test leadership change handling

**Example:**

```cpp
TEST(RaftWorkerTest, BasicSubmit) {
  RaftWorker worker;
  // Setup mock RaftServer
  // Call Submit()
  // Verify Start() was called
}

TEST(RaftWorkerTest, LeadershipCheck) {
  RaftWorker worker;
  worker.site_info_->partition_id_ = 0;
  worker.raft_sched_->is_leader_ = true;
  EXPECT_TRUE(worker.IsLeader(0));
  EXPECT_FALSE(worker.IsLeader(1));
}
```

---

#### Test 7.2: raft_main_helper Unit Tests

**File:** `test/raft_main_helper_test.cc`

**Tests:**
- [ ] Test setup() creates workers
- [ ] Test add_log_to_nc() finds correct worker
- [ ] Test callback registration
- [ ] Test get_outstanding_logs()
- [ ] Test shutdown_paxos() cleanup

---

#### Test 7.3: Callback Integration Tests

**File:** `test/mako_raft_callback_test.cc`

**Tests:**
- [ ] Test leader callback updates watermark
- [ ] Test follower callback performs safety check
- [ ] Test follower queues transactions when watermark too low
- [ ] Test follower replays queued transactions when watermark advances
- [ ] Test NO-OPS handling

---

### Phase 8: Integration Testing (Days 7-9)

#### Test 8.1: Single Shard, Single Replica

**Purpose:** Basic functionality without replication

**Command:**
```bash
cmake -B build -DMAKO_USE_RAFT=ON
cmake --build build -j32
./build/deptran_server -f config/mako_raft.yml -f config/1c1s1r.yml -d 30
```

**Verify:**
- [ ] Server starts without errors
- [ ] Transactions execute
- [ ] Watermarks update correctly
- [ ] No crashes or assertions

---

#### Test 8.2: Single Shard, 3 Replicas

**Purpose:** Basic replication and follower replay

**Command:**
```bash
./build/deptran_server -f config/mako_raft.yml -f config/1c1s3r.yml -d 30
```

**Verify:**
- [ ] Leader elected successfully
- [ ] Followers receive AppendEntries
- [ ] Followers replay transactions
- [ ] Watermarks synchronized across replicas
- [ ] All replicas have consistent state

---

#### Test 8.3: Multiple Shards (2 shards, 3 replicas each)

**Purpose:** Cross-shard watermark synchronization

**Command:**
```bash
./build/deptran_server -f config/mako_raft.yml -f config/2c2s3r.yml -d 30
```

**Verify:**
- [ ] Multiple Raft groups (one per shard) work independently
- [ ] Watermarks synchronized across shards
- [ ] NO-OPS trigger global watermark updates
- [ ] Cross-shard transactions work correctly

---

#### Test 8.4: Leader Failover

**Purpose:** Test leadership change and recovery

**Steps:**
1. Start cluster with 3 replicas
2. Kill leader process (SIGKILL)
3. Wait for election (< 1 second)
4. Verify new leader elected
5. Submit new transactions
6. Verify transactions committed

**Command:**
```bash
./build/deptran_server -f config/mako_raft.yml -f config/1c1s3r.yml -f config/failover.yml -d 30
```

**Verify:**
- [ ] New leader elected within election timeout
- [ ] Follower-to-leader callback switch works
- [ ] Transactions continue after failover
- [ ] No data loss

---

#### Test 8.5: Watermark Stress Test

**Purpose:** Test watermark system under high load

**Configuration:**
- High transaction rate (100K+ TPS)
- Multiple shards
- Frequent NO-OPS

**Verify:**
- [ ] No deadlocks in watermark computation
- [ ] Queued transactions eventually replay
- [ ] Memory usage stays bounded (no leak in queue)
- [ ] Performance comparable to Paxos

---

#### Test 8.6: NO-OPS Handling

**Purpose:** Test watermark synchronization via NO-OPS

**Steps:**
1. Start cluster
2. Submit transactions
3. Inject NO-OPS periodically
4. Verify watermark advances after each NO-OP
5. Verify queued transactions replay

**Verify:**
- [ ] NO-OPS detected correctly (isNoops())
- [ ] All threads synchronize on NO-OPS
- [ ] Watermarks exchanged across shards
- [ ] Historical watermarks updated

---

### Phase 9: Performance Testing (Days 9-10)

#### Test 9.1: Throughput Benchmark

**Workload:**
- Read-write mix (50/50, 80/20, 95/5)
- Different key distributions (uniform, zipf)
- 1-12 client threads

**Metrics:**
- Transactions per second (TPS)
- Compare Raft vs Paxos

**Command:**
```bash
# Raft
./build/deptran_server -f config/mako_raft.yml -f config/12c1s3r.yml -d 300

# Paxos (for comparison)
./build/deptran_server -f config/mako_paxos.yml -f config/12c1s3r.yml -d 300
```

**Verify:**
- [ ] Raft throughput within 90-110% of Paxos
- [ ] No significant performance regression

---

#### Test 9.2: Latency Benchmark

**Metrics:**
- p50, p95, p99, p999 latency
- Leader vs follower latency

**Verify:**
- [ ] Leader latency < 1ms (speculative execution)
- [ ] Follower latency < 10ms (watermark check + replay)
- [ ] Comparable to Paxos latencies

---

#### Test 9.3: Scalability Test

**Configuration:**
- 1, 2, 4, 8 shards
- 3 replicas per shard
- Fixed client load per shard

**Metrics:**
- Throughput vs number of shards
- Watermark synchronization overhead

**Verify:**
- [ ] Linear scalability with shards (ideally)
- [ ] Watermark exchange doesn't bottleneck

---

### Phase 10: Raft Lab Tests (Optional)

**File:** `src/deptran/raft/test.cc`

**Purpose:** Use existing Raft test suite

**Tests:**
- testInitialElection
- testReElection
- testBasicAgree
- testFailAgree
- testConcurrentStarts
- testUnreliableAgree

**Command:**
```bash
cmake -B build -DRAFT_TEST=ON -DMAKO_USE_RAFT=ON
cmake --build build -j32
./build/deptran_server -f config/raft_lab_test.yml
```

**Verify:**
- [ ] All lab tests pass with Mako callbacks
- [ ] No regressions in Raft core functionality

---

## Risk Assessment

### Risk Matrix

| Risk | Likelihood | Impact | Mitigation |
|------|------------|--------|------------|
| **Callback signature mismatch** | Medium | High | Careful review of Paxos vs Raft signatures; unit tests |
| **Watermark correctness bugs** | Medium | Critical | Extensive testing; formal verification if needed |
| **Performance regression** | Low | High | Early benchmarking; profiling |
| **Leadership race conditions** | Medium | Medium | Proper locking; use RustyCpp patterns |
| **Memory leaks in callbacks** | Low | Medium | Smart pointers; valgrind testing |
| **Serialization errors** | High | High | Thorough testing of log serialization/deserialization |
| **NO-OPS not working** | Medium | High | Dedicated NO-OPS tests |
| **Epoch/term confusion** | Low | Low | Clear documentation; careful mapping |

### Critical Path Items

1. **Serialization/Deserialization:** Getting log format right is critical
2. **Callback Integration:** app_next_ must correctly invoke Mako callbacks
3. **Watermark Safety:** Any bug here breaks correctness
4. **Leadership Changes:** Callback switching must be atomic

---

## Timeline and Milestones

### Week 1: Foundation and Core Implementation

| Day | Milestone | Deliverables |
|-----|-----------|--------------|
| **1** | Project setup + RaftWorker skeleton | raft_worker.h/cc created |
| **2** | Complete RaftWorker + setup() | raft_main_helper::setup() works |
| **3** | Implement callbacks + add_log_to_nc() | Full API implemented |
| **4** | RaftServer integration | applyLogs() enhanced |
| **5** | Build system + configuration | CMake/Makefile updated; config files |

**Checkpoint:** Code compiles with -DMAKO_USE_RAFT=ON

### Week 2: Testing and Refinement

| Day | Milestone | Deliverables |
|-----|-----------|--------------|
| **6** | Unit tests | Test suite passing |
| **7** | Basic integration tests | Single shard working |
| **8** | Multi-shard + replication tests | 3-replica setup working |
| **9** | Failover + performance tests | Failover successful; benchmarks |
| **10** | Documentation + cleanup | Updated docs; code review |

**Final Checkpoint:** All tests passing; performance acceptable

### Estimated Total Time: 10 days

**Buffer:** Add 2-3 days for unexpected issues

---

## Success Criteria

### Functional Requirements

- [ ] Mako runs with Raft replication (-DMAKO_USE_RAFT=ON)
- [ ] Leader executes transactions speculatively
- [ ] Followers replay transactions via Raft log
- [ ] Watermark system works correctly
  - [ ] Leader updates local_timestamp_
  - [ ] Follower performs safety_check()
  - [ ] Queued transactions replay when watermark advances
- [ ] NO-OPS trigger watermark synchronization
- [ ] Leader failover works (new leader elected, transactions continue)
- [ ] Multi-shard deployment works (cross-shard watermark exchange)

### Performance Requirements

- [ ] Throughput within 90-110% of Paxos baseline
- [ ] Latency p50 < 1ms on leader (speculative execution)
- [ ] Latency p99 < 10ms on follower (with watermark check)
- [ ] Scalability: linear with number of shards

### Code Quality Requirements

- [ ] All unit tests pass
- [ ] All integration tests pass
- [ ] No memory leaks (valgrind clean)
- [ ] No data races (ThreadSanitizer clean if possible)
- [ ] Code follows RustyCpp patterns where applicable
- [ ] Documentation updated

### Deployment Requirements

- [ ] Backward compatible (Paxos still works)
- [ ] Build system supports both modes
- [ ] Configuration files provided
- [ ] Clear migration guide

---

## References

### Documentation

1. **Mako Explained:** `/home/users/mmakadia/mako_temp/MAKO_EXPLAINED.md`
2. **Raft Explained:** `/home/users/mmakadia/mako_temp/src/deptran/raft/RAFT_EXPLAINED.md`
3. **Claude Code Guide:** `/home/users/mmakadia/mako_temp/CLAUDE.md`

### Code Locations

1. **Paxos Implementation:** `src/deptran/paxos_main_helper.cc`
2. **Raft Implementation:** `src/deptran/raft/`
3. **Mako Core:** `src/mako/`
4. **Watermark System:** `src/mako/benchmarks/sto/sync_util.hh`

### Key Files to Reference During Implementation

| Component | File |
|-----------|------|
| Paxos API | `src/deptran/paxos_main_helper.cc` (lines 347-427) |
| PaxosWorker | `src/deptran/paxos_worker.h` |
| Mako Leader Callback | `src/mako/mako.hh` (lines 289-375) |
| Mako Follower Callback | `src/mako/mako.hh` (lines 128-285) |
| RaftServer | `src/deptran/raft/server.h` |
| RaftServer::applyLogs() | `src/deptran/raft/server.cc` (line 281) |
| Watermark Logic | `src/mako/benchmarks/sto/sync_util.hh` |

---

## Appendix: Quick Reference

### Function Mapping

```
Paxos                                   Raft
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
setup(argc, argv)                    →  setup(argc, argv)
pxs_workers_g                        →  raft_workers_g
PaxosWorker                          →  RaftWorker
PaxosServer                          →  RaftServer
worker->Submit(log, len, par_id)     →  worker->Submit(log, len, par_id)
                                        └─→ raft_sched_->Start(cmd, &index, &term)
worker->register_apply_callback()    →  worker->register_apply_callback()
                                        └─→ raft_sched_->app_next_ = callback
PaxosServer::OnCommit()              →  RaftServer::applyLogs()
max_committed_slot_                  →  commitIndex
max_executed_slot_                   →  executeIndex
```

### Callback Signatures

```cpp
// Both Paxos and Raft use the same callback signature:
function<int(const char*& log,        // Serialized transaction
             int len,                 // Log length
             int par_id,              // Partition ID
             int slot_id,             // Slot/index in log
             queue<tuple<...>>& un_replay_logs_)>  // Queue for delayed replay
```

### Build Commands

```bash
# Build with Raft
cmake -B build -DMAKO_USE_RAFT=ON
cmake --build build -j32

# Or with Makefile
make clean
make MAKO_USE_RAFT=1 -j32

# Run with Raft
./build/deptran_server -f config/mako_raft.yml -f config/1c1s3r.yml -d 30
```

### Status Codes

```cpp
namespace mako {
  enum PaxosStatus {
    STATUS_INIT = 0,
    STATUS_NORMAL = 1,
    STATUS_REPLAY_DONE = 2,
    STATUS_SAFETY_FAIL = 3,
    STATUS_NOOPS = 4,
    STATUS_ENDING = 5,
  };
}

// Return encoding: timestamp * 10 + status
return static_cast<int>(timestamp * 10 + status);
```

---

**End of Migration Plan**

---

## Change Log

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.0 | 2025-10-30 | Claude | Initial comprehensive migration plan |

---

## Approval

- [ ] Technical Review
- [ ] Architecture Review
- [ ] Ready to Begin Implementation

---

**Next Steps:**
1. Review this plan with team
2. Set up development branch
3. Begin Phase 1 (Foundation)
4. Regular checkpoint reviews
