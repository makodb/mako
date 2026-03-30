# Phase 2.4: State Machine Recovery

## Problem Statement

After recovering committed log entries and replaying them, the state machine needs to be rebuilt to the same state it was before the crash. This involves:
1. Applying committed commands to the database
2. Ensuring transaction state is consistent
3. Rebuilding any in-memory indexes

## Analysis

### Current Implementation Already Handles This

The replay mechanism implemented in Phase 2.2 already triggers state machine recovery:

1. `ReplayCommittedEntries()` calls `app_next_(id, log)` for each entry
2. `app_next_` is bound to `tx_sched_->Next` (via `RegLearnerAction`)
3. `SchedulerClassic::Next` dispatches to `CommitReplicated`
4. `CommitReplicated`:
   - Creates/gets transaction via `GetOrCreateTx`
   - Dispatches command to set up mdb_txn
   - Calls `DoCommit` which applies changes to database

```cpp
// SchedulerClassic::CommitReplicated
if (!sp_tx->is_leader_hint_) {
  // During recovery, all replicas are followers
  verify(sp_tx->cmd_);
  unique_ptr<TxnOutput> out = std::make_unique<TxnOutput>();
  DepId di = { "dep", 0 };
  SchedulerClassic::Dispatch(sp_tx->tid_, di, sp_tx->cmd_, *out);
  DoPrepare(sp_tx->tid_);
}
if (commit_or_abort == SUCCESS) {
  sp_tx->committed_ = true;
  DoCommit(*sp_tx);  // Applies changes to database
}
```

### What Phase 2.4 Adds

Since the core state machine recovery is already implemented, Phase 2.4 adds:
1. **Recovery mode flag**: Track when recovery is in progress
2. **Recovery complete logging**: Log when state machine recovery finishes
3. **Statistics**: Track number of transactions recovered

## Implementation

### TxLogServer Changes (~20 LOC)

**Add to scheduler.h:**
```cpp
// State machine recovery tracking
bool in_state_machine_recovery_{false};
size_t transactions_recovered_{0};

// @safe - Check if recovery is in progress
bool IsRecovering() const { return in_state_machine_recovery_; }

// @safe - Get count of recovered transactions
size_t GetRecoveredTransactionCount() const { return transactions_recovered_; }

// @unsafe - Set recovery mode
void SetRecoveryMode(bool recovering);
```

**Add to scheduler.cc:**
```cpp
void TxLogServer::SetRecoveryMode(bool recovering) {
  in_state_machine_recovery_ = recovering;
  if (!recovering) {
    Log_info("[STATE-RECOVERY] Recovery complete: %zu transactions applied",
             transactions_recovered_);
  }
}
```

### ServerWorker Changes (~10 LOC)

**Modify SetupBase to track recovery:**
```cpp
// Phase 2.4: Start state machine recovery tracking
if (tx_sched_) {
  tx_sched_->SetRecoveryMode(true);
}

// ... replay happens via ReplayCommittedEntries() ...

// Phase 2.4: End state machine recovery tracking
if (tx_sched_) {
  tx_sched_->SetRecoveryMode(false);
}
```

### SchedulerClassic Changes (~5 LOC)

**Modify CommitReplicated to track recovered transactions:**
```cpp
// In CommitReplicated, after DoCommit:
if (in_state_machine_recovery_) {
  transactions_recovered_++;
}
```

## Key Points

1. **Minimal Changes**: The core recovery is already implemented by Phase 2.2
2. **Visibility**: Added tracking and logging for operators
3. **No Functional Changes**: Just adds monitoring of existing recovery

## Files Modified

- `src/deptran/scheduler.h` - Add recovery tracking fields and methods
- `src/deptran/scheduler.cc` - Add SetRecoveryMode implementation
- `src/deptran/classic/scheduler.cc` - Track recovered transactions
- `src/deptran/server_worker.cc` - Set/clear recovery mode around replay

## Testing

1. Run `./ci/ci.sh shard1Replication` - Paxos test
2. Run `./ci/ci.sh shard1ReplicationRaft` - Raft test

Fresh start will show "Recovery complete: 0 transactions applied" since there's nothing to recover.

## LOC Estimate

~35 LOC total (within <500 LOC limit)

## Note

Phase 2.4 is intentionally minimal because:
1. The core state machine recovery happens via the existing `Next` callback mechanism
2. Log replay (Phase 2.2) already triggers `CommitReplicated` for each entry
3. The database is rebuilt by applying all committed transactions in order

No additional logic is needed for state machine recovery beyond what Phase 2.2 provides.
