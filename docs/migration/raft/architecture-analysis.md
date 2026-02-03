# Mako Architecture: How Logs Flow to Raft

**Date**: 2025-11-06
**Question**: Does Mako send logs to "its leader" or "anyone"? How does it send logs?

---

## TL;DR: **Mako is CO-LOCATED, Not Client-Server**

**Answer**: Mako **does NOT send logs to a remote leader**. Instead:
- Each Raft node runs **in the same process** as Mako worker threads
- Mako calls **local function** `add_log_to_nc()` (NOT RPC)
- The local Raft node either:
  - **Is leader** → submits log to Raft
  - **Is follower** → **drops log** (current behavior)

**This means the "redirect to leader" bug is REAL and affects production.**

---

## Architectural Analysis

### Configuration: 3-Node Cluster

**From** `config/1c1s3r1p_cluster_test.yml`:
```yaml
site:
  server:
    - ["localhost:38100", "p1:38101", "p2:38102"]

host:
  localhost: 127.0.0.1
  p1: 127.0.0.1
  p2: 127.0.0.1
```

**Deployment**:
- 3 separate **processes** (not threads)
- All on **same machine** (127.0.0.1)
- Different **ports** (38100, 38101, 38102)

### How Tests Run

**From** `examples/testSingleLogSubmission.sh`:
```bash
./build/testSingleLogSubmission p2 > single_log_p2.log 2>&1 &
P2_PID=$!

./build/testSingleLogSubmission p1 > single_log_p1.log 2>&1 &
P1_PID=$!

./build/testSingleLogSubmission localhost > single_log_localhost.log 2>&1 &
LOCALHOST_PID=$!
```

**Each process**:
- Runs its own `testSingleLogSubmission` binary
- Passed a **process name** argument (`p2`, `p1`, `localhost`)
- Runs **independently** (separate PIDs)
- Has its **own Raft instance**

---

## How Mako Sends Logs to Raft

### Step 1: Transaction Execution

**File**: `src/mako/benchmarks/sto/Transaction.cc:777`

```cpp
void Transaction::serialize_util(..., uint32_t timestamp) const {
    // ... serialize transaction to queueLog buffer ...

    // Submit to replication
    add_log_to_nc((char *)queueLog, pos, TThread::getPartitionID(), batch_size);
}
```

### Step 2: What is `TThread::getPartitionID()`?

**File**: `src/mako/benchmarks/sto/Interface.hh:143-145`

```cpp
class TThread {
    static __thread int pid;  // partition-id (thread-local)

public:
    static int getPartitionID() {
        return pid;
    }
};
```

**Key insight**: `pid` is **thread-local** (TLS variable).

**Question**: What is `pid` set to?

**Answer**: **Never explicitly set in Mako code!** It defaults to 0.

This means **ALL worker threads** call `add_log_to_nc(..., 0, ...)` (partition 0).

### Step 3: What is `add_log_to_nc()`?

**File**: `src/deptran/raft_main_helper.cc:448`

```cpp
void add_log_to_nc(const char* log, int len, uint32_t par_id, int batch_size) {
    auto* worker = find_worker(par_id);  // Find LOCAL RaftWorker
    if (!worker) {
        Log_warn("[RAFT-ADD-LOG] no worker found for par_id=%u", par_id);
        return;
    }

    if (!worker->IsLeader(par_id)) {
        Log_info("[RAFT-ADD-LOG] partition %u not led here, dropping", par_id);
        return;  // ← DROPS THE LOG!
    }

    enqueue_to_worker(worker, log, len, par_id, std::max(1, batch_size));
}
```

**This is a LOCAL FUNCTION CALL, not RPC!**

**Proof**:
```bash
$ nm build/testSingleLogSubmission | grep add_log_to_nc
U _Z13add_log_to_ncPKciji
```

The `U` means "undefined" (linked from another object file in the same binary).

### Step 4: What is `find_worker()`?

**File**: `src/deptran/raft_main_helper.cc:167`

```cpp
RaftWorker* find_worker(uint32_t par_id) {
    for (auto& worker : raft_workers_g) {  // ← LOCAL global variable
        if (worker && worker->IsPartition(par_id)) {
            return worker.get();
        }
    }
    return nullptr;
}
```

**Key**: `raft_workers_g` is a **process-global** vector defined in `raft_main_helper.cc:19`:

```cpp
namespace janus {
    vector<shared_ptr<RaftWorker>> raft_workers_g = {};  // ← Global in THIS process
}
```

---

## The Critical Realization

### Mako is **NOT** a Client-Server Architecture

**NOT like this** (what I initially assumed):
```
┌─────────────┐                    ┌─────────────┐
│  Mako       │  ── RPC ──>       │  Raft       │
│  Client     │    (send log)      │  Cluster    │
│  (separate) │                    │  (3 nodes)  │
└─────────────┘                    └─────────────┘
```

**ACTUALLY like this**:
```
┌─────────────────────────────┐
│  Process: localhost:38100   │
│  ┌─────────┐  ┌──────────┐ │
│  │ Mako    │→ │ Raft     │ │
│  │ Workers │  │ (local)  │ │
│  └─────────┘  └──────────┘ │
└─────────────────────────────┘
         ↕ Raft RPCs
┌─────────────────────────────┐
│  Process: p1:38101          │
│  ┌─────────┐  ┌──────────┐ │
│  │ Mako    │→ │ Raft     │ │
│  │ Workers │  │ (local)  │ │
│  └─────────┘  └──────────┘ │
└─────────────────────────────┘
         ↕ Raft RPCs
┌─────────────────────────────┐
│  Process: p2:38102          │
│  ┌─────────┐  ┌──────────┐ │
│  │ Mako    │→ │ Raft     │ │
│  │ Workers │  │ (local)  │ │
│  └─────────┘  └──────────┘ │
└─────────────────────────────┘
```

**Each process contains**:
- Mako worker threads (execute transactions)
- RaftWorker instance (replication)
- Mako → Raft communication is **LOCAL** (function call)

---

## Implications for the Leader Drop Bug

### Scenario 1: Node is Leader

```
Process: localhost:38100 (LEADER)

1. Mako worker thread executes transaction
2. Calls add_log_to_nc(..., 0, ...)
3. find_worker(0) → returns local RaftWorker
4. RaftWorker->IsLeader(0) → TRUE
5. enqueue_to_worker() → log submitted to Raft
6. Raft replicates to p1 and p2
7. ✓ Transaction committed
```

**Result**: ✅ Works correctly

### Scenario 2: Node is Follower

```
Process: p1:38101 (FOLLOWER)

1. Mako worker thread executes transaction
2. Calls add_log_to_nc(..., 0, ...)
3. find_worker(0) → returns local RaftWorker
4. RaftWorker->IsLeader(0) → FALSE
5. LOG DROPPED!
6. ✗ Transaction lost
```

**Result**: ❌ **BUG - Transaction lost!**

---

## Why This Happens in Production

### The Production Deployment Model

In a real Mako deployment, you would have:

**Datacenter 1** (e.g., us-east):
```
┌─────────────────────────────┐
│  mako-node-1 (10.0.1.10)    │
│  ┌─────────┐  ┌──────────┐ │
│  │ Mako    │→ │ Raft     │ │  ← May or may not be leader
│  │ Workers │  │ (local)  │ │
│  └─────────┘  └──────────┘ │
└─────────────────────────────┘
```

**Datacenter 2** (e.g., us-west):
```
┌─────────────────────────────┐
│  mako-node-2 (10.0.2.20)    │
│  ┌─────────┐  ┌──────────┐ │
│  │ Mako    │→ │ Raft     │ │  ← May or may not be leader
│  │ Workers │  │ (local)  │ │
│  └─────────┘  └──────────┘ │
└─────────────────────────────┘
```

**Datacenter 3** (e.g., eu-central):
```
┌─────────────────────────────┐
│  mako-node-3 (10.0.3.30)    │
│  ┌─────────┐  ┌──────────┐ │
│  │ Mako    │→ │ Raft     │ │  ← May or may not be leader
│  │ Workers │  │ (local)  │ │
│  └─────────┘  └──────────┘ │
└─────────────────────────────┘
```

### The Problem

**If mako-node-2 is the Raft leader**:
- ✅ Transactions on mako-node-2 → submit successfully
- ❌ Transactions on mako-node-1 → **DROPPED** (not leader)
- ❌ Transactions on mako-node-3 → **DROPPED** (not leader)

**67% of transactions are silently lost!**

---

## Why Clients Don't Route to Leader

In a traditional Raft client-server system:
```
Client → discovers leader → routes request to leader → succeeds
```

But in Mako:
```
Mako Worker Thread → calls local add_log_to_nc() → no routing!
```

**There is NO client-side routing** because:
1. Mako workers are **co-located** with Raft
2. `add_log_to_nc()` is a **local function call**
3. There's no mechanism to "route to another node"

---

## Comparison with Paxos

### Paxos Approach (Multi-Paxos with Static Leader)

**setup2()** from `paxos_main_helper.cc:865`:
```cpp
int setup2(int action, int shardIndex) {
    if (action == 0 && es->machine_id == 0) {
        // I AM THE LEADER (statically assigned)
        es->set_state(1);
        for (int i = 0; i < pxs_workers_g.size(); i++) {
            pxs_workers_g[i]->is_leader = 1;
        }
    } else {
        // I AM A FOLLOWER
        es->set_state(0);
    }
}
```

**Deployment**:
- **Only machine_id=0 runs Mako workers**
- All other machines are pure replication nodes
- No worker threads on followers
- **Transactions only execute on leader!**

**Why Paxos doesn't have this bug**:
- Followers **don't have Mako workers** → can't execute transactions
- Only leader executes transactions
- Only leader calls `add_log_to_nc()`
- **Architecture guarantees no drops!**

### Raft Approach (Dynamic Leader Election)

**setup2()** from `raft_main_helper.cc:257`:
```cpp
int setup2(int action, int shardIndex) {
    // ALL nodes start as followers
    es->set_state(0);
    es->set_epoch(0);
    es->set_leader(0);
    // No pre-assignment of leadership!
}
```

**Deployment**:
- **ALL nodes run Mako workers**
- Leadership is **dynamic** (can change)
- **Transactions execute on all nodes**
- Non-leaders **drop logs** → **BUG!**

---

## The Fix: Three Options

### Option 1: Paxos-Style (Static Leader)

**Only run Mako workers on designated leader**:
```cpp
int setup2(int action, int shardIndex) {
    if (action == 0 && es->machine_id == 0) {
        // I am designated leader - start Mako workers
        StartMakoWorkers();
    } else {
        // I am follower - no Mako workers!
        // Just run Raft replication
    }
}
```

**Pros**:
- Simple
- Matches Paxos behavior
- No drops possible

**Cons**:
- ❌ Defeats purpose of Raft (no automatic failover)
- ❌ If leader fails, system is down until manual intervention
- ❌ Doesn't use Raft's strength

### Option 2: Local Wait-for-Leadership

**Wait briefly for leadership before dropping**:
```cpp
void add_log_to_nc(...) {
    if (!worker->IsLeader(par_id)) {
        // Wait 1 second to become leader
        if (wait_for_local_leadership(worker, par_id, 1000ms)) {
            // Became leader, submit!
        } else {
            // Still not leader, drop (but log ERROR)
            Log_error("DROPPING LOG: not leader!");
            return;
        }
    }
    enqueue_to_worker(...);
}
```

**Pros**:
- Simple to implement
- Reduces drop window

**Cons**:
- ❌ Still drops after timeout
- ❌ Adds latency to every submission
- ❌ Doesn't actually fix the bug

### Option 3: Return Error + Leader Info (CORRECT FIX)

**Make Mako aware of replication failures**:

**In raft_main_helper.cc**:
```cpp
AddLogResult add_log_to_nc(...) {
    if (!worker->IsLeader(par_id)) {
        auto raft_server = worker->GetRaftServer();
        siteid_t current_leader = raft_server->GetCurrentLeader();
        return {AddLogResult::NOT_LEADER, current_leader};
    }
    enqueue_to_worker(...);
    return {AddLogResult::SUCCESS, INVALID_SITEID};
}
```

**In Transaction.cc**:
```cpp
auto result = add_log_to_nc(...);
if (!result.IsSuccess()) {
    // Abort transaction
    // Mako layer can retry entire transaction
    throw TransactionAbort("replication failed");
}
```

**Pros**:
- ✅ Correct - no silent drops
- ✅ Allows Mako to handle failures
- ✅ Matches industry standard (etcd, Consul)

**Cons**:
- Requires changes to Mako transaction layer
- More complex implementation

---

## Production Impact

### Current Behavior in Production

**3-node cluster**:
- Node 1 is leader → handles 100% of transactions
- Node 2 is follower → **drops 100% of transactions** (if workers run)
- Node 3 is follower → **drops 100% of transactions** (if workers run)

**If workers run on all nodes**:
- Expected throughput: 3x (all nodes process transactions)
- **Actual throughput: 1x** (only leader succeeds)
- **66% of work is silently lost!**

### Why This Wasn't Caught in Testing

**Tests like testNoOps/testSingleLogSubmission**:
- All 3 nodes try to submit
- Non-leaders drop silently
- Only leader's submissions succeed
- **Test passes because leader's submissions succeed!**

We assumed "all nodes successfully submitted" but actually:
- localhost submitted 3 logs → **success** (was leader)
- p1 submitted 3 logs → **dropped** (was follower)
- p2 submitted 3 logs → **dropped** (was follower)

**The chaos we observed was the bug manifesting!**

---

## Conclusion

### Question: How Does Mako Send Logs?

**Answer**: Mako sends logs via **LOCAL function call** to **co-located Raft instance**.

### Question: Does it send to "its leader" or "anyone"?

**Answer**: It sends to **whoever is in the same process** (co-located Raft node).

### Is This a Bug?

**YES, it's a critical bug** because:
1. Mako workers run on **all nodes** (not just leader)
2. Each node calls **local** `add_log_to_nc()`
3. Non-leaders **drop** logs silently
4. **Transactions are lost** without error

### The Fix

**We MUST implement Option 3** (return error + leader info):
1. Track current leader in RaftServer
2. Return AddLogResult from add_log_to_nc()
3. Make Mako abort transactions on replication failure
4. Optionally: Mako can retry entire transaction

**This is NOT optional** - it's a correctness requirement for production.
