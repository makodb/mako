# Phase 1.4: Paxos Integration

## Overview

Integrate LogStorage into PaxosServer to provide durable persistence for Paxos consensus state, following the same pattern as RaftServer.

## Current State Analysis

### PaxosServer Location
- `src/deptran/paxos/server.h` - Class definition
- `src/deptran/paxos/server.cc` - Implementation

### PaxosData Structure
```cpp
struct PaxosData {
  ballot_t max_ballot_seen_ = 0;      // Highest ballot seen in Prepare
  ballot_t max_ballot_accepted_ = 0;  // Highest ballot accepted in Accept
  bool is_no_op = false;
  shared_ptr<Marshallable> accepted_cmd_{nullptr};
  shared_ptr<Marshallable> committed_cmd_{nullptr};
};
```

### State That Must Persist

| Variable | Type | Description |
|----------|------|-------------|
| `logs_` entries | PaxosData | Per-slot consensus state |
| `cur_epoch` | ballot_t | Current ballot/epoch |
| `max_committed_slot_` | slotid_t | Highest committed slot |

### Key Methods That Modify State

1. **OnPrepare** - Updates `max_ballot_seen_`
2. **OnAccept** - Updates ballots and `accepted_cmd_`
3. **OnCommit** - Updates `committed_cmd_` and `max_committed_slot_`
4. **OnBulkAccept** - Bulk accept for multiple slots
5. **OnSyncCommit** - Bulk commit for multiple slots

## Design

### Metadata Keys
```cpp
static constexpr const char* META_EPOCH = "cur_epoch";
static constexpr const char* META_MAX_COMMITTED = "max_committed_slot";
```

### New Methods

```cpp
// Private persistence helpers
void PersistEpoch();
void PersistMaxCommitted();
void PersistLogEntry(slotid_t slot_id, const PaxosData& data);
void PersistLogEntries(const std::vector<std::pair<slotid_t, std::shared_ptr<PaxosData>>>& entries);

// Public API
void SetLogStorage(std::shared_ptr<srpc::LogStorage> storage);
std::shared_ptr<srpc::LogStorage> GetLogStorage() const;
bool RecoverFromStorage();
```

### Integration Points

#### OnPrepare
```cpp
if (instance->max_ballot_seen_ < ballot) {
    instance->max_ballot_seen_ = ballot;
    PersistLogEntry(slot_id, *instance);  // NEW
}
```

#### OnAccept
```cpp
if (instance->max_ballot_seen_ <= ballot) {
    instance->max_ballot_seen_ = ballot;
    instance->max_ballot_accepted_ = ballot;
    instance->accepted_cmd_ = cmd;
    PersistLogEntry(slot_id, *instance);  // NEW
}
```

#### OnCommit
```cpp
instance->committed_cmd_ = cmd;
if (slot_id > max_committed_slot_) {
    max_committed_slot_ = slot_id;
    PersistMaxCommitted();  // NEW
}
PersistLogEntry(slot_id, *instance);  // NEW
```

#### OnBulkAccept
```cpp
// Collect entries to persist
std::vector<std::pair<slotid_t, std::shared_ptr<PaxosData>>> entries;
for (...) {
    entries.emplace_back(slot_id, instance);
}
PersistLogEntries(entries);  // NEW
```

#### OnSyncCommit
```cpp
// Similar bulk persistence pattern
PersistLogEntries(entries);
PersistMaxCommitted();
```

## Estimated LOC

| Component | LOC |
|-----------|-----|
| Header changes | 40 |
| Persistence helpers | 70 |
| Integration changes | 50 |
| **Total** | **~160** |

## RustyCpp Compliance

- Use `std::shared_ptr<LogStorage>` for storage member
- Mark all persistence methods as `@unsafe`
- Use existing mutex for thread safety

## Success Criteria

1. Paxos state persists across restarts
2. No data loss for committed entries
3. All existing tests pass
4. Compatible with RaftServer pattern
