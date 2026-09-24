# Raft Snapshot Design

## Overview

Raft log compaction via snapshotting prevents unbounded log growth by periodically
capturing the state machine state at a given log index/term, then discarding log
entries before that point. This document describes the snapshot data format,
metadata structures, and the integration with the RaftServer.

## Data Format

### Binary Wire Format (SnapshotFormat)

Snapshots are serialized using a binary format defined in `src/srpc/rpc/snapshot_format.hpp`:

```
Magic (4B) | Version (4B) | Header Size (4B) | Data Size (8B) |
Compression (1B) | Checksum Type (1B) | Last Index (8B) | Last Term (8B) |
Timestamp (8B) | Header CRC (4B) | Padding (2B) | Data... | Data CRC (4B)
```

Key properties:
- **Magic**: `0x504E4153` ("SNAP" in little-endian) for format identification
- **CRC32 checksums** on both header and data for corruption detection
- **Forward-compatible** header size field allows future extensions
- **Compression** field reserved for future Snappy/ZSTD support (currently NONE only)
- **52-byte fixed header** (8-byte aligned) for efficient parsing

### SnapshotMetadata

Defined in `src/srpc/rpc/snapshot_manager.hpp`:

```cpp
struct SnapshotMetadata {
  uint64_t last_included_index;  // Last log entry included in snapshot
  uint64_t last_included_term;   // Term of last included entry
  uint64_t timestamp_ms;         // When snapshot was taken
  size_t   size_bytes;           // Size of snapshot data
  string   checksum;             // Checksum for verification
};
```

The `last_included_index` and `last_included_term` are the critical fields for
Raft correctness: they identify the exact point in the log that the snapshot
covers, enabling log truncation and InstallSnapshot RPC.

## Architecture

### Storage Layer (srpc namespace)

Three layers of abstraction in `src/srpc/rpc/`:

1. **SnapshotManager** (interface) - Abstract API for snapshot CRUD operations
2. **FileSnapshotManager** (implementation) - File-based storage with retention policy
3. **SnapshotFormat** (utility) - Binary serialization/deserialization with CRC32

File naming convention: `snapshot_<index>_<term>.snap` with `.tmp` suffix during writes.
Atomic rename on finalize prevents partial snapshots from being visible.

### RaftServer Integration

The `snapshot_manager_` field in `RaftServer` (declared at `server.h:123`) is
initialized during `Setup()` when `MAKO_RAFT_SNAPSHOTS=1` is set. The initialization:

1. Reads configuration from environment variables
2. Creates a `FileSnapshotManager` with appropriate storage path
3. Loads existing snapshot metadata into `snapidx_`/`snapterm_` fields
4. These fields are used by `RequestVote` and `AppendEntries` for log consistency checks

Environment variables:
- `MAKO_RAFT_SNAPSHOTS=1` - Enable snapshot support
- `MAKO_RAFT_SNAPSHOT_PATH` - Custom base path for snapshot storage
- `MAKO_RAFT_SNAPSHOT_INTERVAL` - Entries between snapshots (default: 10000)

### Public API on RaftServer

```cpp
void SetSnapshotManager(shared_ptr<SnapshotManager> manager);
shared_ptr<SnapshotManager> GetSnapshotManager() const;
bool HasSnapshot() const;
uint64_t GetSnapshotIndex() const;
uint64_t GetSnapshotTerm() const;
size_t CompactLog(uint64_t up_to_index);
```

### CreateSnapshot (Phase 3.2)

`CreateSnapshot()` is a private method on `RaftServer` that performs periodic
log compaction via snapshotting. It is called automatically from `applyLogs()`
when the number of applied entries since the last snapshot exceeds the threshold.

**Trigger condition** (in `applyLogs()`):
```cpp
if (snapshot_manager_ && snapidx_ < executeIndex &&
    (executeIndex - snapidx_) > snapshot_threshold_)
```

**Implementation** (`server.cc`):
1. Reads `executeIndex` as the snapshot point
2. Looks up the term for that index via `GetRaftInstance()`
3. Serializes a minimal state marker (executeIndex + term, 16 bytes)
4. Calls `snapshot_manager_->TakeSnapshot(index, term, data, size)`
5. Updates `snapidx_` and `snapterm_`
6. Calls `CompactLog(executeIndex)` which removes entries from both
   `log_storage_` and `raft_logs_`, and advances `min_active_slot_`

**Configuration**:
- `snapshot_threshold_` member (default: 10000) controls how many entries
  are applied before triggering a snapshot
- Set via `MAKO_RAFT_SNAPSHOT_INTERVAL` env var (read in `InitializeSnapshotManager()`)
- Programmatic setter: `SetSnapshotThreshold(uint64_t)`

**Thread safety**: `CreateSnapshot()` is called from `applyLogs()` which uses
the `in_applying_logs_` reentrance guard. `CompactLog()` acquires `mtx_` internally.

**Tests** (Tests 55-57 in `test.cc`):
- Test 55: Basic snapshot creation after exceeding threshold
- Test 56: Snapshot + compaction, then verify continued operation
- Test 57: Threshold configurability via getter/setter

### InstallSnapshot RPC (Phase 3.3)

`OnInstallSnapshot()` handles the follower side of snapshot transfer. When a leader
determines a follower is too far behind (its `next_index_` points to compacted log),
the leader sends the full snapshot in a single RPC.

**Follower-side handler** (`OnInstallSnapshot()` in `server.cc`):
1. Validates term: rejects if `term < currentTerm` (stale leader)
2. Updates term and steps down if leader has higher term
3. Resets election timer (legitimate leader contact)
4. Saves snapshot data via `snapshot_manager_->TakeSnapshot()`
5. Updates `snapidx_` and `snapterm_`
6. Discards all in-memory log entries up to `last_included_index`
7. Compacts persistent log storage if available
8. Advances `commitIndex`, `executeIndex`, and `lastLogIndex` to at least `last_included_index`

**Leader-side sender** (`SendInstallSnapshot()` in `commo.cc`):
- Sends `InstallSnapshot` RPC with term, leader_id, last_included_index,
  last_included_term, and serialized snapshot data
- Callback receives follower's current term for leader term update

**RPC definition** (in `rcc_rpc.rpc`):
```
defer InstallSnapshot(uint64_t term, uint64_t leader_id,
                      uint64_t last_included_index, uint64_t last_included_term,
                      string data | uint64_t term_out);
```

**Tests** (Tests 58-59 in `test.cc`):
- Test 58: Basic InstallSnapshot on a follower, verifying snapidx_, snapterm_,
  commitIndex, executeIndex update and snapshot persistence
- Test 59: InstallSnapshot with stale term is rejected, follower state unchanged
- Test 60: HeartbeatLoop detects lagging follower (next_index < min_active_slot_),
  sends InstallSnapshot, and verifies next_index_/match_index_ are updated

## Subsequent Tasks

The following features are planned:

1. ~~**HeartbeatLoop integration** (Phase 3.4)~~: **DONE**. In HeartbeatLoop's PHASE 1
   per-follower loop, checks `next_index < min_active_slot_ && snapshot_manager_` before
   attempting GetRaftInstance. Loads snapshot via LoadLatestSnapshot, sends InstallSnapshot
   RPC, callback updates next_index_/match_index_ or steps down on higher term.
   Test 60 (testHeartbeatTriggersInstallSnapshot) covers this integration.

2. ~~**Recovery** (Phase 3.5)~~: **DONE**. In `InitializeSnapshotManager()`, after loading
   snapshot metadata (`snapidx_`/`snapterm_`), advances `executeIndex`, `commitIndex`,
   `lastLogIndex`, and `min_active_slot_` to at least the snapshot index. Uses `>` checks
   to only advance values (never go backwards), since `RecoverFromStorage()` runs first.
   Tests 65 (testSnapshotRecoveryOnStartup) and 66 (testSnapshotRecoveryFieldAdvancement)
   cover this.

## RustyCpp Compliance

All new code follows RustyCpp safety requirements:
- `@safe`/`@unsafe` annotations on every function
- `rusty::Option<T>` for optional return values (e.g., `GetLatestSnapshot()`)
- No `std::unique_ptr` or `std::shared_ptr` in new Raft-level code (existing `shared_ptr`
  usage in the srpc layer is grandfathered but documented)
