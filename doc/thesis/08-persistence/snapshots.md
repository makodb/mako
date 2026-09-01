# Snapshot Support

## 1. Overview

Snapshots capture the state machine at a specific log index, allowing
the system to discard log entries that have already been applied.  This
bounds log growth and enables fast catch-up for lagging followers.

The snapshot system has four components:
- `SnapshotMetadata`: describes a snapshot
- `SnapshotManager`: abstract interface for snapshot operations
- `FileSnapshotManager`: file-based implementation
- `SnapshotFormat`: binary serialization format with CRC32 checksums

## 2. SnapshotMetadata

**File**: `src/srpc/rpc/snapshot_manager.hpp` (294 lines, lines 39-57)

```cpp
struct SnapshotMetadata {
    slotid_t last_included_index;    // line 40: last log entry in snapshot
    ballot_t last_included_term;     // line 41: term of last included entry
    uint64_t timestamp_ms;           // line 42: creation time
    size_t size_bytes;               // line 43: snapshot data size
    std::string checksum;            // line 44: checksum string
};
```

Key methods:

| Method | Line | Purpose |
|--------|------|---------|
| `is_valid()` | 47 | Returns `last_included_index > 0` |
| `to_string()` | 52 | Format as `"Snapshot{index=..., term=..., size=...}"` |

## 3. SnapshotManager Interface

**File**: `src/srpc/rpc/snapshot_manager.hpp` (lines 148-258)

### 3.1 Snapshot Creation

| Method | Line | Signature |
|--------|------|-----------|
| `BeginSnapshot()` | 163 | `BeginSnapshot(slotid_t last_index, ballot_t last_term) -> unique_ptr<SnapshotWriter>` |
| `TakeSnapshot()` | 176 | `TakeSnapshot(slotid_t, ballot_t, const char* data, size_t size) -> bool` |

`BeginSnapshot()` returns a `SnapshotWriter` for streaming large
snapshots.  `TakeSnapshot()` is a convenience method that writes the
entire snapshot in one call.

### 3.2 Snapshot Loading

| Method | Line | Signature |
|--------|------|-----------|
| `BeginLoad()` | 189 | `BeginLoad(const SnapshotMetadata&) -> unique_ptr<SnapshotReader>` |
| `LoadLatestSnapshot()` | 199 | `LoadLatestSnapshot(SnapshotMetadata*, std::string*) -> bool` |

`BeginLoad()` returns a `SnapshotReader` for streaming reads.
`LoadLatestSnapshot()` loads the most recent snapshot completely into
memory.

### 3.3 Snapshot Queries

| Method | Line | Signature |
|--------|------|-----------|
| `GetLatestSnapshot()` | 211 | `GetLatestSnapshot() -> rusty::Option<SnapshotMetadata>` |
| `ListSnapshots()` | 218 | `ListSnapshots() -> std::vector<SnapshotMetadata>` |
| `HasSnapshotAtOrAfter()` | 226 | `HasSnapshotAtOrAfter(slotid_t min_index) -> bool` |

### 3.4 Snapshot Cleanup

| Method | Line | Signature |
|--------|------|-----------|
| `PruneSnapshots()` | 239 | `PruneSnapshots(slotid_t keep_after_index) -> bool` |
| `DeleteAllSnapshots()` | 247 | `DeleteAllSnapshots() -> bool` |

## 4. SnapshotReader and SnapshotWriter

**File**: `src/srpc/rpc/snapshot_manager.hpp`

### 4.1 SnapshotReader (lines 63-97)

```cpp
class SnapshotReader {
    virtual bool Read(char* buffer, size_t buffer_size, size_t* bytes_read);
    virtual bool IsComplete();
    virtual const SnapshotMetadata& GetMetadata();
    virtual size_t GetOffset();
};
```

Streaming interface for reading snapshots in chunks.  The caller
provides a buffer and reads until `IsComplete()` returns true.

### 4.2 SnapshotWriter (lines 103-137)

```cpp
class SnapshotWriter {
    virtual bool Write(const char* data, size_t size);
    virtual bool Finalize();
    virtual void Abort();
    virtual size_t GetOffset();
};
```

Streaming interface for writing snapshots.  After all data is written,
`Finalize()` persists the snapshot to disk.  `Abort()` cleans up
partial writes.

## 5. FileSnapshotManager

**File**: `src/srpc/rpc/file_snapshot_manager.hpp` (531 lines)

### 5.1 File Naming Convention

```
snapshot_{index}_{term}.snap        (complete snapshots)
snapshot_{index}_{term}.snap.tmp    (in-progress writes)
```

Example: `snapshot_00000000000000010000_00000000000000000005.snap`

### 5.2 Directory Structure

```
/tmp/{USER}_mako_snapshot_shard{partition_id}_replica{locale_id}/
    snapshot_00000000000000001000_00000000000000000001.snap
    snapshot_00000000000000005000_00000000000000000003.snap
    snapshot_00000000000000010000_00000000000000000005.snap
```

### 5.3 Configuration

**File**: `src/srpc/rpc/snapshot_manager.hpp` (lines 264-292)

```cpp
struct SnapshotConfig {
    std::string storage_path;           // line 265
    size_t snapshot_interval = 10000;   // line 266: entries between snapshots
    size_t max_snapshots = 3;           // line 267: retention limit
    bool verify_on_load = true;         // line 268: verify CRC on load
    size_t chunk_size = 65536;          // line 269: 64 KB streaming chunks
};
```

| Parameter | Default | Purpose |
|-----------|---------|---------|
| `snapshot_interval` | 10,000 | Take a snapshot every 10,000 log entries |
| `max_snapshots` | 3 | Retain at most 3 snapshots |
| `verify_on_load` | `true` | Verify CRC32 checksums when loading |
| `chunk_size` | 64 KB | Streaming read/write buffer size |

### 5.4 Taking a Snapshot

`TakeSnapshot()` (line 299-309):

```
1. Create FileSnapshotWriter with index and term
2. Write data to writer
3. Finalize writer (serialize, fsync, atomic rename)
4. Apply retention policy (delete old snapshots)
```

### 5.5 FileSnapshotWriter

**File**: `src/srpc/rpc/file_snapshot_manager.hpp` (lines 39-161)

The writer accumulates data in a buffer, then performs an atomic write:

```
Finalize() sequence:
  1. SnapshotFormat::Serialize(index, term, buffer, size, &output)
     → produces header + data + CRC32
  2. Open temp file: snapshot_{index}_{term}.snap.tmp
     → O_WRONLY | O_CREAT | O_TRUNC
  3. Write serialized data to temp file
  4. fsync() temp file (ensure durability)
  5. Atomic rename() temp → final: snapshot_{index}_{term}.snap
```

The atomic rename ensures that readers never see a partially-written
snapshot file.  If a crash occurs during write, only the `.tmp` file
is left, which is ignored on recovery.

The destructor (line 55-59) calls `Abort()` if the writer was not
finalized, cleaning up any `.tmp` file via `unlink()`.

### 5.6 FileSnapshotReader

**File**: `src/srpc/rpc/file_snapshot_manager.hpp` (lines 167-267)

The reader loads the entire file at construction:

```
Constructor sequence:
  1. Open file with O_RDONLY
  2. Read entire file into memory
  3. SnapshotFormat::Deserialize(file_data, &index, &term, &data)
     → verifies header magic, version, CRC32
  4. Extract metadata (index, term, size, timestamp from header)
  5. Set valid_ flag
```

The `Read()` method (line 230-244) provides streaming access from the
in-memory buffer, returning chunks of `chunk_size` bytes.

### 5.7 Retention Policy

`ApplyRetentionPolicy()` (line 514-528):

```
1. List all snapshots (sorted by index, descending)
2. If count > max_snapshots:
     Delete oldest snapshots until count == max_snapshots
```

With `max_snapshots = 3`, this keeps the 3 most recent snapshots.

### 5.8 Log Compaction

When a snapshot covers entries up to index N, all log entries with
index <= N can be safely removed.  The Paxos server exposes
`CompactLog(slotid_t up_to_index)` (paxos/server.h, line 115) which
calls `log_storage_->remove_range(first_index, up_to_index)`.

## 6. Snapshot Binary Format

**File**: `src/srpc/rpc/snapshot_format.hpp` (373 lines)

### 6.1 Layout

```
Offset  Size   Field
------  ----   -----
0       4      Magic number: 0x504E4153 ("SNAP")
4       4      Version: 1
8       4      Header size: 52 bytes
12      8      Data size (uncompressed)
20      1      Compression type (0=NONE, 1=SNAPPY, 2=ZSTD)
21      1      Checksum type (0=NONE, 1=CRC32, 2=SHA256)
22      8      Last included index
30      8      Last included term
38      8      Timestamp (milliseconds since epoch)
46      4      Header CRC32 (computed over bytes 0-43)
50      2      Padding (8-byte alignment)
------- Header end (52 bytes) -------
52      N      Snapshot data
52+N    4      Data CRC32 (if checksum type = CRC32)
```

Total size: 52 + N + 4 bytes (with CRC32).

### 6.2 SnapshotHeader

**File**: `src/srpc/rpc/snapshot_format.hpp` (lines 56-76)

```cpp
#pragma pack(push, 1)
struct SnapshotHeader {
    uint32_t magic;            // 0x504E4153
    uint32_t version;          // 1
    uint32_t header_size;      // 52
    uint64_t data_size;        // uncompressed data size
    uint8_t compression;       // SnapshotCompression enum
    uint8_t checksum_type;     // SnapshotChecksumType enum
    uint64_t last_index;       // last included log index
    uint64_t last_term;        // term of last included entry
    uint64_t timestamp_ms;     // creation time
    uint32_t header_crc;       // CRC32 of bytes 0-43
    uint8_t padding[2];        // alignment padding
};
#pragma pack(pop)
```

The `#pragma pack(push, 1)` ensures no compiler-inserted padding,
giving an exact 52-byte binary layout.

### 6.3 CRC32 Implementation

**File**: `src/srpc/rpc/snapshot_format.hpp` (lines 85-162)

A table-driven CRC32 using the IEEE 802.3 polynomial (reversed:
`0xEDB88320`).  The 256-entry lookup table is defined inline
(lines 118-161).

```cpp
class CRC32 {
    uint32_t crc_ = 0xFFFFFFFF;

    void Update(const char* data, size_t size);  // line 91
    uint32_t Finalize();                          // line 99: return crc_ ^ 0xFFFFFFFF
    static uint32_t Calculate(const char*, size_t);  // line 107: one-shot
};
```

Two CRC32 values protect the snapshot:
1. **Header CRC** (bytes 0-43): Protects the header metadata.
2. **Data CRC** (after data section): Protects the snapshot payload.

### 6.4 Serialization

`SnapshotFormat::Serialize()` (line 186-252):

```
1. Build SnapshotHeader with metadata
2. Compute header CRC32 over bytes 0-43
3. Compute data CRC32 if checksum type is CRC32
4. Allocate output: 52 (header) + data_size + 4 (data CRC)
5. Copy header, data, and CRC into output
```

### 6.5 Deserialization

`SnapshotFormat::Deserialize()` (line 264-341):

```
1. Check input size >= 52 (header)
2. Copy header from input
3. Verify magic == 0x504E4153
4. Verify version == 1
5. Verify header CRC32 (bytes 0-43)
6. Check compression == NONE (others reserved)
7. Verify total size matches header + data + CRC
8. Verify data CRC32
9. Extract last_index, last_term, data
```

### 6.6 Compression

Currently only `NONE` is supported (line 20-21 in format).  The enum
reserves values for `SNAPPY` and `ZSTD` for future use.

## 7. Snapshot Types

```cpp
enum class SnapshotCompression : uint8_t {
    NONE = 0,       // No compression (current)
    SNAPPY = 1,     // Reserved
    ZSTD = 2        // Reserved
};

enum class SnapshotChecksumType : uint8_t {
    NONE = 0,       // No checksum
    CRC32 = 1,      // 4-byte CRC32 (current)
    SHA256 = 2      // Reserved
};
```

## 8. Server Integration

### 8.1 Raft Server

**File**: `src/deptran/raft/server.h`

```cpp
std::shared_ptr<srpc::SnapshotManager> snapshot_manager_;  // line 57
```

The Raft server holds a reference to the snapshot manager but the
current implementation focuses on log persistence rather than
snapshotting.  The infrastructure is in place for future use.

### 8.2 Paxos Server

**File**: `src/deptran/paxos/server.h`

```cpp
std::shared_ptr<srpc::SnapshotManager> snapshot_manager_;  // line 81

void SetSnapshotManager(std::shared_ptr<srpc::SnapshotManager>);  // line 84
std::shared_ptr<srpc::SnapshotManager> GetSnapshotManager() const;  // line 88
void CompactLog(slotid_t up_to_index);  // line 115
```

`CompactLog()` removes log entries up to the given index after a
snapshot has been taken, bounding log growth.

## 9. Snapshot Lifecycle

```
1. Application reaches snapshot_interval (10,000 entries)
2. TakeSnapshot(last_applied_index, last_applied_term, state_data, size)
3. FileSnapshotWriter accumulates data in buffer
4. Finalize():
   a. Serialize with header + CRC32
   b. Write to .tmp file
   c. fsync()
   d. Atomic rename to .snap
5. ApplyRetentionPolicy():
   a. List snapshots sorted by index (descending)
   b. Delete oldest beyond max_snapshots (3)
6. CompactLog(last_applied_index):
   a. log_storage_->remove_range(first, last_applied_index)
   b. Free disk space from old log entries
```

## 10. Crash Safety Properties

### 10.1 Atomic Writes

Snapshots use the write-to-temp-then-rename pattern:

```
write → .snap.tmp    (crash here = only .tmp left, ignored)
fsync  .snap.tmp     (crash here = only .tmp left, ignored)
rename .snap.tmp → .snap  (atomic on POSIX filesystems)
```

After a crash, only complete `.snap` files are visible.  Partial
`.tmp` files are ignored by `ListSnapshots()`.

### 10.2 CRC32 Verification

Both header and data CRCs are verified on load.  If either checksum
fails, the snapshot is rejected and the system falls back to log
replay for recovery.

### 10.3 Retention Guarantee

The retention policy (`max_snapshots = 3`) ensures that even if the
most recent snapshot is corrupted, two older snapshots are available
as fallbacks.
