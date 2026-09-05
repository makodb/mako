# Phase 1.1: Log Persistence Interface

## Overview

Create an abstract `LogStorage` interface that abstracts log storage operations for both Raft and Paxos consensus protocols. The interface will support both in-memory (for testing) and RocksDB (for production) backends.

## Current State Analysis

### Existing Patterns

**Raft (src/deptran/raft/server.h):**
```cpp
struct RaftData {
  ballot_t max_ballot_seen_ = 0;
  ballot_t max_ballot_accepted_ = 0;
  shared_ptr<Marshallable> accepted_cmd_{nullptr};
  shared_ptr<Marshallable> committed_cmd_{nullptr};
  ballot_t term;
  shared_ptr<Marshallable> log_{nullptr};
  ballot_t prevTerm;
  slotid_t slot_id;
  ballot_t ballot;
};
map<slotid_t, shared_ptr<RaftData>> logs_{};
```

**Paxos (src/deptran/paxos/server.h):**
```cpp
struct PaxosData {
  ballot_t max_ballot_seen_ = 0;
  ballot_t max_ballot_accepted_ = 0;
  bool is_no_op = false;
  shared_ptr<Marshallable> accepted_cmd_{nullptr};
  shared_ptr<Marshallable> committed_cmd_{nullptr};
};
map<slotid_t, shared_ptr<PaxosData>> logs_{};
```

**Key Observations:**
1. Both use `slotid_t` (uint64_t) as primary key
2. Both store ballot/term metadata with each entry
3. Both store `shared_ptr<Marshallable>` for commands
4. Both track indices: min_active_slot_, max_executed_slot_, max_committed_slot_
5. Access via `GetInstance(slotid_t)` pattern

## Design

### LogEntry Structure

A unified log entry structure that works for both Raft and Paxos:

```cpp
struct LogEntry {
    slotid_t slot_id;           // Primary key
    ballot_t term;              // Raft term or Paxos ballot
    ballot_t max_ballot_seen;   // For Paxos prepare phase
    ballot_t max_ballot_accepted; // For Paxos accept phase
    shared_ptr<Marshallable> command;  // The replicated command
    bool committed;             // Whether entry is committed
    bool is_no_op;              // No-op entry flag

    // Serialization
    Marshal& to_marshal(Marshal& m) const;
    Marshal& from_marshal(Marshal& m);
};
```

### LogStorage Interface

```cpp
class LogStorage {
public:
    virtual ~LogStorage() = default;

    // Single entry operations
    virtual rusty::Option<LogEntry> get(slotid_t slot_id) const = 0;
    virtual bool put(const LogEntry& entry) = 0;
    virtual bool remove(slotid_t slot_id) = 0;

    // Batch operations
    virtual std::vector<LogEntry> get_range(slotid_t start, slotid_t end) const = 0;
    virtual bool put_batch(const std::vector<LogEntry>& entries) = 0;
    virtual bool remove_range(slotid_t start, slotid_t end) = 0;

    // Index queries
    virtual slotid_t get_first_index() const = 0;
    virtual slotid_t get_last_index() const = 0;
    virtual rusty::Option<ballot_t> get_term(slotid_t slot_id) const = 0;

    // Metadata operations
    virtual bool set_metadata(const std::string& key, const std::string& value) = 0;
    virtual rusty::Option<std::string> get_metadata(const std::string& key) const = 0;

    // Lifecycle
    virtual bool sync() = 0;  // Force flush to disk
    virtual bool close() = 0;
    virtual bool is_open() const = 0;
};
```

### InMemoryLogStorage Implementation

For testing and simple use cases:

```cpp
class InMemoryLogStorage : public LogStorage {
private:
    rusty::Mutex<std::map<slotid_t, LogEntry>> logs_;
    rusty::Mutex<std::map<std::string, std::string>> metadata_;
    rusty::Cell<bool> is_open_{true};

public:
    // All methods implemented using the internal maps
    // Thread-safe via rusty::Mutex
};
```

## File Structure

```
src/srpc/rpc/
├── log_storage.hpp      # LogEntry struct and LogStorage interface (~150 LOC)
└── memory_log_storage.hpp  # InMemoryLogStorage implementation (~150 LOC)
```

## Implementation Tasks

### Task 1: LogEntry Structure (~50 LOC)
- Define `LogEntry` struct with all fields
- Implement `to_marshal()` and `from_marshal()` for serialization
- Add comparison operators for testing

### Task 2: LogStorage Interface (~50 LOC)
- Define abstract `LogStorage` class
- Document each method's semantics
- Include error handling patterns

### Task 3: InMemoryLogStorage (~150 LOC)
- Implement all LogStorage methods
- Use rusty::Mutex for thread safety
- Add statistics tracking (optional)

### Task 4: Unit Tests (~150 LOC)
- Test single entry operations (get/put/remove)
- Test batch operations
- Test index queries
- Test metadata operations
- Test thread safety

## RustyCpp Compliance

All code must:
- Use `rusty::Option<T>` instead of `std::optional`
- Use `rusty::Mutex<T>` for thread-safe containers
- Use `rusty::Cell<T>` for simple atomic values
- Include `@safe` or `@unsafe` annotations on all methods
- Pass borrow checking

## Estimated LOC

| Component | LOC |
|-----------|-----|
| LogEntry struct | 50 |
| LogStorage interface | 50 |
| InMemoryLogStorage | 150 |
| Unit tests | 150 |
| **Total** | **~400** |

## Success Criteria

1. LogEntry can serialize/deserialize any Marshallable command
2. InMemoryLogStorage passes all unit tests
3. Thread-safe concurrent access works correctly
4. All code passes rusty-cpp borrow checking
5. Interface is suitable for RocksDB backend (Phase 1.2)
