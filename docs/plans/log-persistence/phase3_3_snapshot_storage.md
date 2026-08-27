# Phase 3.3: Snapshot Storage

## Problem Statement

We need a concrete implementation of SnapshotManager that:
1. Stores snapshots to the filesystem using the binary format from Phase 3.2
2. Supports listing and retrieving snapshots
3. Implements retention policy to limit disk usage
4. Provides streaming read/write for large snapshots

## Design

### File Naming Convention

Snapshots are stored in a directory with a naming convention that allows sorting by index:
```
/tmp/<user>_mako_snapshot_shard<partition>_replica<locale>/
  snapshot_<index>_<term>.snap
  snapshot_<index>_<term>.snap.tmp    # In-progress writes
```

### FileSnapshotManager

Implements the SnapshotManager interface using filesystem storage.

```cpp
class FileSnapshotManager : public SnapshotManager {
public:
    FileSnapshotManager(const SnapshotConfig& config);

    // Snapshot Creation
    std::unique_ptr<SnapshotWriter> BeginSnapshot(slotid_t last_index, ballot_t last_term) override;
    bool TakeSnapshot(slotid_t last_index, ballot_t last_term,
                     const char* data, size_t size) override;

    // Snapshot Loading
    std::unique_ptr<SnapshotReader> BeginLoad(const SnapshotMetadata& metadata) override;
    bool LoadLatestSnapshot(SnapshotMetadata* metadata_out, std::string* data_out) override;

    // Snapshot Queries
    rusty::Option<SnapshotMetadata> GetLatestSnapshot() const override;
    std::vector<SnapshotMetadata> ListSnapshots() const override;
    bool HasSnapshotAtOrAfter(slotid_t min_index) const override;

    // Snapshot Cleanup
    size_t PruneSnapshots(slotid_t keep_after_index) override;
    size_t DeleteAllSnapshots() override;

    // Configuration
    const std::string& GetStoragePath() const override;

private:
    SnapshotConfig config_;
    mutable std::mutex mutex_;  // Protect concurrent access

    std::string GetSnapshotPath(slotid_t index, ballot_t term) const;
    std::string GetTempPath(slotid_t index, ballot_t term) const;
    bool EnsureDirectory() const;
    std::vector<std::string> ListSnapshotFiles() const;
    bool ParseSnapshotFilename(const std::string& filename,
                               slotid_t* index, ballot_t* term) const;
};
```

### FileSnapshotWriter

Writes snapshot data to a temporary file, then atomically renames on Finalize.

```cpp
class FileSnapshotWriter : public SnapshotWriter {
public:
    FileSnapshotWriter(const std::string& final_path,
                       const std::string& temp_path,
                       slotid_t last_index, ballot_t last_term);
    ~FileSnapshotWriter();

    bool Write(const char* data, size_t size) override;
    bool Finalize() override;
    bool Abort() override;
    size_t GetOffset() const override;

private:
    std::string final_path_;
    std::string temp_path_;
    slotid_t last_index_;
    ballot_t last_term_;
    int fd_{-1};
    size_t offset_{0};
    bool finalized_{false};
    std::string buffer_;  // Accumulate data for format serialization
};
```

### FileSnapshotReader

Reads snapshot data from file, verifying format on construction.

```cpp
class FileSnapshotReader : public SnapshotReader {
public:
    FileSnapshotReader(const std::string& path);
    ~FileSnapshotReader();

    bool Read(char* buffer, size_t buffer_size, size_t* bytes_read) override;
    bool IsComplete() const override;
    const SnapshotMetadata& GetMetadata() const override;
    size_t GetOffset() const override;

private:
    std::string path_;
    SnapshotMetadata metadata_;
    int fd_{-1};
    size_t data_offset_{0};   // Offset in file where data starts
    size_t data_size_{0};     // Size of data
    size_t read_offset_{0};   // How much we've read
};
```

## Implementation

### File: `src/srpc/rpc/file_snapshot_manager.hpp`

Contains FileSnapshotManager, FileSnapshotWriter, FileSnapshotReader implementations.

## Key Considerations

1. **Atomicity**: Write to temp file, rename on success (atomic on POSIX)
2. **Concurrency**: Mutex protects metadata operations
3. **Error Handling**: Clean up temp files on failure
4. **Retention**: Delete old snapshots when max_snapshots exceeded

## Files Created

- `src/srpc/rpc/file_snapshot_manager.hpp` (~350 LOC)

## LOC Estimate

~350 LOC for file-based storage implementation

## Next Steps

- Phase 3.4: Log compaction after snapshot
