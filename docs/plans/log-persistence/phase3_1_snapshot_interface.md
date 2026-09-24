# Phase 3.1: Snapshot Interface

## Problem Statement

As the consensus log grows, recovery time increases linearly. Snapshots solve this by:
1. Periodically capturing the full state machine state
2. Truncating log entries that are covered by the snapshot
3. Enabling faster follower catch-up via snapshot transfer

## Design

### Snapshot Metadata

```cpp
struct SnapshotMetadata {
  slotid_t last_included_index;  // Last log entry included in snapshot
  ballot_t last_included_term;   // Term of last included entry
  uint64_t timestamp_ms;         // When snapshot was taken
  size_t size_bytes;             // Size of snapshot data
  std::string checksum;          // SHA256 or similar for verification
};
```

### SnapshotManager Interface

```cpp
class SnapshotManager {
public:
  // Create a snapshot at the given index
  virtual bool TakeSnapshot(slotid_t last_index, ballot_t last_term) = 0;

  // Load the latest snapshot
  virtual bool LoadSnapshot(SnapshotMetadata* metadata_out) = 0;

  // List available snapshots
  virtual std::vector<SnapshotMetadata> ListSnapshots() const = 0;

  // Get the latest snapshot metadata (without loading data)
  virtual rusty::Option<SnapshotMetadata> GetLatestSnapshot() const = 0;

  // Delete snapshots older than the given index
  virtual bool PruneSnapshots(slotid_t keep_after_index) = 0;

  // Get snapshot data reader for streaming
  virtual std::unique_ptr<SnapshotReader> GetReader(const SnapshotMetadata& meta) = 0;

  // Get snapshot data writer for streaming
  virtual std::unique_ptr<SnapshotWriter> GetWriter(slotid_t index, ballot_t term) = 0;
};

// Abstract reader for streaming snapshot data
class SnapshotReader {
public:
  virtual ~SnapshotReader() = default;
  virtual bool Read(char* buffer, size_t* bytes_read) = 0;
  virtual bool IsComplete() const = 0;
  virtual const SnapshotMetadata& GetMetadata() const = 0;
};

// Abstract writer for streaming snapshot data
class SnapshotWriter {
public:
  virtual ~SnapshotWriter() = default;
  virtual bool Write(const char* data, size_t size) = 0;
  virtual bool Finalize() = 0;
  virtual bool Abort() = 0;
};
```

### Integration Points

1. **RaftServer/PaxosServer**:
   - Call `TakeSnapshot()` periodically or when log size exceeds threshold
   - Call `LoadSnapshot()` during recovery before log replay

2. **RecoveryManager**:
   - Check for snapshots during `detect_mode()`
   - Load snapshot before replaying remaining log entries

3. **LogStorage**:
   - After snapshot, call `remove_range()` to compact old entries

## Implementation

### File: `src/srpc/rpc/snapshot_manager.hpp`

Create abstract interface and metadata structures.

### File: `src/deptran/raft/server.h` / `src/deptran/paxos/server.h`

Add snapshot manager member and integration hooks.

## Key Considerations

1. **Atomicity**: Snapshot must be atomic - either fully written or not at all
2. **Streaming**: Support large snapshots that don't fit in memory
3. **Verification**: Checksum to detect corruption
4. **Concurrency**: Don't block normal operations during snapshot

## Files Created/Modified

- `src/srpc/rpc/snapshot_manager.hpp` - Interface definitions (~100 LOC)
- Minor additions to Raft/Paxos server headers for snapshot manager member

## LOC Estimate

~100 LOC for interface definitions

## Next Steps

- Phase 3.2: Implement binary snapshot format
- Phase 3.3: Implement file-based snapshot storage
- Phase 3.4: Integrate log compaction
