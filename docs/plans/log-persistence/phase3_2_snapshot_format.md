# Phase 3.2: Snapshot Format

## Problem Statement

We need a binary format for snapshots that:
1. Stores last included index and term for log truncation
2. Contains state machine data in a self-describing format
3. Supports checksum verification for data integrity
4. Optionally supports compression for space efficiency

## Design

### Binary Format Structure

```
+-------------------+
| Magic Number (4B) |  "SNAP" = 0x534E4150
+-------------------+
| Version (4B)      |  Format version (1)
+-------------------+
| Header Size (4B)  |  Size of header section
+-------------------+
| Data Size (8B)    |  Size of uncompressed data
+-------------------+
| Compressed (1B)   |  0 = none, 1 = snappy, 2 = zstd
+-------------------+
| Checksum Type (1B)|  0 = none, 1 = CRC32, 2 = SHA256
+-------------------+
| Last Index (8B)   |  Last included log index
+-------------------+
| Last Term (8B)    |  Term of last included entry
+-------------------+
| Timestamp (8B)    |  When snapshot was taken (ms since epoch)
+-------------------+
| Header Checksum   |  CRC32 of header (4B)
+-------------------+
| Data...           |  Variable length state data
+-------------------+
| Data Checksum     |  32 bytes (SHA256) or 4 bytes (CRC32)
+-------------------+
```

### Implementation Classes

```cpp
// Compression types
enum class SnapshotCompression : uint8_t {
    NONE = 0,
    SNAPPY = 1,
    ZSTD = 2
};

// Checksum types
enum class SnapshotChecksumType : uint8_t {
    NONE = 0,
    CRC32 = 1,
    SHA256 = 2
};

// Binary header structure (fixed size = 50 bytes)
struct SnapshotHeader {
    uint32_t magic;           // "SNAP"
    uint32_t version;         // Format version
    uint32_t header_size;     // Size of header
    uint64_t data_size;       // Uncompressed data size
    uint8_t compression;      // Compression type
    uint8_t checksum_type;    // Checksum type
    uint64_t last_index;      // Last included index
    uint64_t last_term;       // Last included term
    uint64_t timestamp_ms;    // Snapshot timestamp
    uint32_t header_crc;      // CRC32 of header (excluding this field)
};

// Snapshot format utilities
class SnapshotFormat {
public:
    static constexpr uint32_t MAGIC = 0x534E4150;  // "SNAP"
    static constexpr uint32_t VERSION = 1;

    // Serialize snapshot to buffer
    static bool Serialize(const SnapshotMetadata& meta,
                         const char* data, size_t size,
                         std::string* output,
                         SnapshotCompression compression = SnapshotCompression::NONE,
                         SnapshotChecksumType checksum = SnapshotChecksumType::CRC32);

    // Deserialize snapshot from buffer
    static bool Deserialize(const char* input, size_t size,
                           SnapshotMetadata* meta,
                           std::string* data);

    // Calculate checksum
    static std::string CalculateChecksum(const char* data, size_t size,
                                        SnapshotChecksumType type);

    // Verify checksum
    static bool VerifyChecksum(const char* data, size_t size,
                              const std::string& expected,
                              SnapshotChecksumType type);
};
```

### CRC32 Implementation

Use a table-driven CRC32 implementation (IEEE 802.3 polynomial):

```cpp
class CRC32 {
public:
    // Calculate CRC32 checksum
    static uint32_t Calculate(const char* data, size_t size);

    // Incremental calculation
    CRC32();
    void Update(const char* data, size_t size);
    uint32_t Finalize() const;

private:
    static const uint32_t TABLE[256];
    uint32_t crc_;
};
```

### Compression Strategy

For initial implementation, skip compression (set to NONE). This can be added later:
- Snappy: Fast compression/decompression, moderate ratio
- ZSTD: Better ratio, still good speed

## Implementation

### File: `src/srpc/rpc/snapshot_format.hpp`

Contains:
- SnapshotCompression enum
- SnapshotChecksumType enum
- SnapshotHeader struct
- SnapshotFormat class with Serialize/Deserialize
- CRC32 class for checksums

## Key Considerations

1. **Endianness**: Use little-endian for all integers (native on x86)
2. **Alignment**: Header is naturally aligned (50 bytes, but padded as needed)
3. **Versioning**: Version field allows format evolution
4. **Magic Number**: Quick validation that file is a snapshot

## Files Created

- `src/srpc/rpc/snapshot_format.hpp` (~200 LOC)

## LOC Estimate

~200 LOC for format utilities

## Next Steps

- Phase 3.3: Implement file-based snapshot storage using this format
