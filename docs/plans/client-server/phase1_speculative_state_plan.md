# Phase 1: Speculative State Extensions - Implementation Plan

## Overview

This document describes the implementation plan for adding speculative Raft state fields to RaftServer. These fields enable the separation of "speculative" (memory quorum) from "secured" (durable quorum) for both leadership and log entries.

## Design Rationale

### Why Separate Speculative and Durable State?

Traditional Raft waits for durable (disk-persisted) acknowledgments before responding to clients. This adds latency due to fsync operations. Speculative Raft allows:

1. **Faster client responses**: Respond with SPECULATIVE status when memory quorum is reached
2. **Same safety guarantees for DURABLE**: Only upgrade to DURABLE when disk quorum + secured leader
3. **Graceful degradation**: Speculative entries may be rolled back, but durable entries never are

### Key Invariants

1. `securedLogIndex <= specCommitIndex <= lastLogIndex`
   - Secured entries are always a subset of speculatively committed entries
   - Speculatively committed entries are always a subset of all log entries

2. `durableVoters ⊆ specVoters` (initially, before crashes)
   - Every node that has durably voted has also speculatively voted
   - After crashes, this may not hold due to memory loss

## Implementation Details

### New Fields in RaftServer

```cpp
// ============================================================================
// SPECULATIVE REPLICATION STATE (Phase 1.1)
// ============================================================================

// Leader security status
bool securedLeader_ = false;  // true when durable vote quorum achieved

// Vote tracking
std::set<siteid_t> specVoters_;    // servers that have memory-voted for us
std::set<siteid_t> durableVoters_; // servers that have durably-voted for us

// Log commit tracking
uint64_t securedLogIndex_ = 0;     // highest index with durable ack quorum
uint64_t specCommitIndex_ = 0;     // highest index with memory ack quorum

// Acknowledgment tracking per log index
std::map<uint64_t, std::set<siteid_t>> memoryAcks_;   // track memory acks per index
std::map<uint64_t, std::set<siteid_t>> durableAcks_;  // track durable acks per index
```

### Thread Safety

All new fields will be protected by the existing `mtx_` recursive mutex, consistent with other RaftServer state.

### Accessor Methods

Following RustyCpp safety annotations:

```cpp
// @safe - Read-only accessor
bool IsSecuredLeader() const {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  return securedLeader_;
}

// @safe - Read-only accessor
uint64_t GetSpecCommitIndex() const {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  return specCommitIndex_;
}

// @safe - Read-only accessor
uint64_t GetSecuredLogIndex() const {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  return securedLogIndex_;
}
```

### Reset on New Leadership

When a node becomes leader, speculative state must be reset:

```cpp
void ResetSpeculativeState() {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  securedLeader_ = false;
  specVoters_.clear();
  specVoters_.insert(site_id_);  // voted for self
  durableVoters_.clear();
  durableVoters_.insert(site_id_);  // self vote is always durable
  securedLogIndex_ = commitIndex;   // from previous term
  specCommitIndex_ = commitIndex;
  memoryAcks_.clear();
  durableAcks_.clear();
}
```

## File Changes

- `src/deptran/raft/server.h`: Add new private fields and public accessor methods
- `src/deptran/raft/server.cc`: Add initialization and reset logic (Phase 6 integration)

## Testing Strategy

These fields are foundational - they will be tested via the Phase 7 tests that exercise the complete speculative commit workflow. For Phase 1, we verify:

1. Fields compile correctly
2. Accessors return expected initial values
3. State is properly reset when leadership changes

## Dependencies

- RustyCpp: Using `std::set` and `std::map` (not yet migrated to RustyCpp equivalents)
- Existing mutex infrastructure

## Future Work (Later Phases)

- Phase 2: Vote RPC integration to populate specVoters/durableVoters
- Phase 3: AppendEntries integration to populate memoryAcks/durableAcks
- Phase 5: Step-down logic using these fields
- Phase 7: Comprehensive testing
