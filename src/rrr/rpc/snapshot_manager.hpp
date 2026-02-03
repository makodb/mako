#pragma once

/**
 * Snapshot Manager Interface for Raft/Paxos Consensus Protocols
 *
 * This header defines:
 * - SnapshotMetadata: Metadata about a snapshot
 * - SnapshotReader: Abstract reader for streaming snapshot data
 * - SnapshotWriter: Abstract writer for streaming snapshot data
 * - SnapshotManager: Interface for snapshot operations
 *
 * RustyCpp Compliance: Uses rusty::Option for optional values
 */

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <rusty/option.hpp>

#include "base/misc.hpp"  // For Log_info, Log_error

namespace rrr {

// Type aliases matching existing codebase
// Use preprocessor guards to avoid conflict with macro definitions in constants.h
#ifndef slotid_t
using slotid_t = uint64_t;
#endif
#ifndef ballot_t
using ballot_t = uint64_t;
#endif

/**
 * Metadata about a snapshot.
 */
// @safe - POD struct
struct SnapshotMetadata {
  slotid_t last_included_index{0};  // Last log entry included in snapshot
  ballot_t last_included_term{0};   // Term of last included entry
  uint64_t timestamp_ms{0};         // When snapshot was taken
  size_t size_bytes{0};             // Size of snapshot data
  std::string checksum;             // Checksum for verification (e.g., SHA256)

  // @safe - Check if metadata is valid
  bool is_valid() const {
    return last_included_index > 0;
  }

  // @unsafe - String formatting
  std::string to_string() const {
    return "Snapshot{index=" + std::to_string(last_included_index) +
           ", term=" + std::to_string(last_included_term) +
           ", size=" + std::to_string(size_bytes) + "}";
  }
};

/**
 * Abstract reader for streaming snapshot data.
 * Used for transferring snapshots to followers.
 */
class SnapshotReader {
 public:
  virtual ~SnapshotReader() = default;

  /**
   * Read a chunk of snapshot data.
   * @param buffer Output buffer to write data to
   * @param buffer_size Size of the buffer
   * @param bytes_read Output: actual bytes read
   * @return true if read succeeded, false on error
   */
  // @unsafe - Writes to raw buffer
  virtual bool Read(char* buffer, size_t buffer_size, size_t* bytes_read) = 0;

  /**
   * Check if all data has been read.
   * @return true if no more data to read
   */
  // @safe
  virtual bool IsComplete() const = 0;

  /**
   * Get the metadata for this snapshot.
   * @return Reference to snapshot metadata
   */
  // @lifetime: (&'a) -> &'a
  virtual const SnapshotMetadata& GetMetadata() const = 0;

  /**
   * Get current read offset.
   * @return Bytes read so far
   */
  // @safe
  virtual size_t GetOffset() const = 0;
};

/**
 * Abstract writer for streaming snapshot data.
 * Used for receiving snapshots from leader.
 */
class SnapshotWriter {
 public:
  virtual ~SnapshotWriter() = default;

  /**
   * Write a chunk of snapshot data.
   * @param data Data to write
   * @param size Size of data
   * @return true if write succeeded, false on error
   */
  // @unsafe - Reads from raw pointer
  virtual bool Write(const char* data, size_t size) = 0;

  /**
   * Finalize the snapshot after all data is written.
   * Verifies checksum and makes snapshot available.
   * @return true if snapshot is valid and saved
   */
  // @unsafe - May have side effects
  virtual bool Finalize() = 0;

  /**
   * Abort the snapshot write, cleaning up partial data.
   * @return true if cleanup succeeded
   */
  // @unsafe - May have side effects
  virtual bool Abort() = 0;

  /**
   * Get current write offset.
   * @return Bytes written so far
   */
  // @safe
  virtual size_t GetOffset() const = 0;
};

/**
 * Abstract interface for snapshot management.
 *
 * Implementations should handle:
 * - Atomic snapshot creation
 * - Streaming for large snapshots
 * - Checksum verification
 * - Concurrent access safety
 */
class SnapshotManager {
 public:
  virtual ~SnapshotManager() = default;

  // ========================================================================
  // Snapshot Creation
  // ========================================================================

  /**
   * Begin taking a snapshot at the given index.
   * @param last_index Last log entry to include in snapshot
   * @param last_term Term of the last included entry
   * @return Writer for streaming snapshot data, or nullptr on error
   */
  // @unsafe - Creates writer with side effects
  virtual std::unique_ptr<SnapshotWriter> BeginSnapshot(
      slotid_t last_index, ballot_t last_term) = 0;

  /**
   * Take a complete snapshot synchronously.
   * Convenience method that handles writer internally.
   * @param last_index Last log entry to include
   * @param last_term Term of last included entry
   * @param data Complete snapshot data
   * @param size Size of data
   * @return true if snapshot was saved successfully
   */
  // @unsafe - May have side effects
  virtual bool TakeSnapshot(slotid_t last_index, ballot_t last_term,
                           const char* data, size_t size) = 0;

  // ========================================================================
  // Snapshot Loading
  // ========================================================================

  /**
   * Begin loading a snapshot.
   * @param metadata Metadata of snapshot to load
   * @return Reader for streaming snapshot data, or nullptr on error
   */
  // @unsafe - Creates reader with side effects
  virtual std::unique_ptr<SnapshotReader> BeginLoad(
      const SnapshotMetadata& metadata) = 0;

  /**
   * Load the latest snapshot completely.
   * @param metadata_out Output: metadata of loaded snapshot
   * @param data_out Output: snapshot data
   * @return true if snapshot was loaded successfully
   */
  // @unsafe - Allocates and writes to output parameters
  virtual bool LoadLatestSnapshot(SnapshotMetadata* metadata_out,
                                  std::string* data_out) = 0;

  // ========================================================================
  // Snapshot Queries
  // ========================================================================

  /**
   * Get metadata of the latest snapshot.
   * @return Metadata if snapshot exists, None otherwise
   */
  // @safe
  virtual rusty::Option<SnapshotMetadata> GetLatestSnapshot() const = 0;

  /**
   * List all available snapshots.
   * @return Vector of snapshot metadata, sorted by index descending
   */
  // @safe
  virtual std::vector<SnapshotMetadata> ListSnapshots() const = 0;

  /**
   * Check if a snapshot exists at or after the given index.
   * @param min_index Minimum index to check
   * @return true if such a snapshot exists
   */
  // @safe
  virtual bool HasSnapshotAtOrAfter(slotid_t min_index) const = 0;

  // ========================================================================
  // Snapshot Cleanup
  // ========================================================================

  /**
   * Delete snapshots older than the given index.
   * Keeps the snapshot covering the given index.
   * @param keep_after_index Keep snapshots with last_included_index >= this
   * @return Number of snapshots deleted
   */
  // @unsafe - Deletes files
  virtual size_t PruneSnapshots(slotid_t keep_after_index) = 0;

  /**
   * Delete all snapshots.
   * Used for testing or forced fresh start.
   * @return Number of snapshots deleted
   */
  // @unsafe - Deletes files
  virtual size_t DeleteAllSnapshots() = 0;

  // ========================================================================
  // Configuration
  // ========================================================================

  /**
   * Get the storage path for snapshots.
   */
  // @lifetime: (&'a) -> &'a
  virtual const std::string& GetStoragePath() const = 0;
};

/**
 * Configuration for snapshot behavior.
 */
// @safe - POD struct
struct SnapshotConfig {
  std::string storage_path;           // Path for snapshot files
  size_t snapshot_interval{10000};    // Take snapshot every N log entries
  size_t max_snapshots{3};            // Maximum snapshots to keep
  bool verify_on_load{true};          // Verify checksum when loading
  size_t chunk_size{64 * 1024};       // Chunk size for streaming (64KB)

  // @unsafe - Returns struct by value
  static SnapshotConfig defaults() {
    return SnapshotConfig{};
  }

  // @unsafe - Uses getenv and string operations
  static SnapshotConfig for_replica(uint32_t partition_id, uint32_t locale_id) {
    SnapshotConfig config;
    // Use username prefix to avoid conflicts between users
    std::string username;
    auto user = std::getenv("USER");  // @unsafe
    if (user) {
      username = user;
    } else {
      username = "unknown";
    }
    config.storage_path = "/tmp/" + username + "_mako_snapshot_shard" +
                         std::to_string(partition_id) + "_replica" +
                         std::to_string(locale_id);
    return config;
  }
};

}  // namespace rrr
