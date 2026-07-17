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
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <rusty/option.hpp>

#include "rrr/rrr.hpp"

namespace janus {
namespace raft {

// Type aliases matching existing codebase
// Use preprocessor guards to avoid conflict with macro definitions in constants.h
#ifndef slotid_t
using slotid_t = uint64_t;
#endif
#ifndef ballot_t
using ballot_t = uint64_t;
#endif
using c_char = char;

/**
 * Metadata about a snapshot.
 */
struct SnapshotMetadata;

// DSL-prep helpers: keep SnapshotMetadata methods thin so a later inline-Rust
// impl can delegate string formatting to ordinary C++.
inline bool snapshot_metadata_is_valid(const SnapshotMetadata& metadata);
inline std::string snapshot_metadata_to_string(const SnapshotMetadata& metadata);

#if RUSTYCPP_RUST
pub struct SnapshotMetadata {
    last_included_index: u64,
    last_included_term: u64,
    timestamp_ms: u64,
    size_bytes: usize,
    checksum: std::string,
}

impl SnapshotMetadata {
    fn is_valid(&self) -> bool {
        snapshot_metadata_is_valid(self)
    }

    fn to_string(&self) -> std::string {
        snapshot_metadata_to_string(self)
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=snapshot_manager.metadata version=1 rust_sha256=53199a4922e628a778ab0ad03e5fe655b6e953a7c274a896d8a1d0298b6baf4a*/
struct SnapshotMetadata;

struct SnapshotMetadata {
    uint64_t last_included_index;
    uint64_t last_included_term;
    uint64_t timestamp_ms;
    size_t size_bytes;
    std::string checksum;

    bool is_valid() const;
    std::string to_string() const;
};


inline bool SnapshotMetadata::is_valid() const {
    return snapshot_metadata_is_valid((*this));
}

inline std::string SnapshotMetadata::to_string() const {
    return snapshot_metadata_to_string((*this));
}
/*RUSTYCPP:GEN-END id=snapshot_manager.metadata*/

inline bool snapshot_metadata_is_valid(const SnapshotMetadata& metadata) {
  return metadata.last_included_index > 0;
}

inline std::string snapshot_metadata_to_string(const SnapshotMetadata& metadata) {
  return "Snapshot{index=" + std::to_string(metadata.last_included_index) +
         ", term=" + std::to_string(metadata.last_included_term) +
         ", size=" + std::to_string(metadata.size_bytes) + "}";
}

/**
 * Abstract reader for streaming snapshot data.
 * Used for transferring snapshots to followers.
 */
#if RUSTYCPP_RUST
pub trait SnapshotReader {
    // @unsafe - Writes to raw buffer and out-pointer.
    fn Read(&mut self, buffer: *mut c_char, buffer_size: usize,
            bytes_read: *mut usize) -> bool;
    // @safe
    fn IsComplete(&self) -> bool;
    // @lifetime: (&'a) -> &'a
    fn GetMetadata(&self) -> &SnapshotMetadata;
    // @safe
    fn GetOffset(&self) -> usize;
}

pub trait SnapshotWriter {
    // @unsafe - Reads from raw pointer.
    fn Write(&mut self, data: *const c_char, size: usize) -> bool;
    // @unsafe - May have side effects.
    fn Finalize(&mut self) -> bool;
    // @unsafe - May have side effects.
    fn Abort(&mut self) -> bool;
    // @safe
    fn GetOffset(&self) -> usize;
}
#endif
/*RUSTYCPP:GEN-BEGIN id=snapshot_manager.stream_interfaces version=1 rust_sha256=bbb013ac4c2e826ccbb4780c6fc4daaa772f0e4779f40b2d05317c99f86056f3*/
namespace {
class SnapshotWriter {
public:
    virtual ~SnapshotWriter() noexcept(false) {}
    virtual bool Write(const c_char* data, size_t size) = 0;
    virtual bool Finalize() = 0;
    virtual bool Abort() = 0;
    virtual size_t GetOffset() const = 0;
    SnapshotWriter(const SnapshotWriter&) = delete;
    SnapshotWriter& operator=(const SnapshotWriter&) = delete;
    SnapshotWriter(SnapshotWriter&&) = delete;
    SnapshotWriter& operator=(SnapshotWriter&&) = delete;
protected:
    SnapshotWriter() = default;
};
}

template <class U> class SnapshotWriterAdapter;
template <class U> class SnapshotWriterAdapterRef;
template <class U> class SnapshotWriterAdapterRefMut;

namespace {
class SnapshotReader {
public:
    virtual ~SnapshotReader() noexcept(false) {}
    virtual bool Read(c_char* buffer, size_t buffer_size, size_t* bytes_read) = 0;
    virtual bool IsComplete() const = 0;
    virtual const SnapshotMetadata& GetMetadata() const = 0;
    virtual size_t GetOffset() const = 0;
    SnapshotReader(const SnapshotReader&) = delete;
    SnapshotReader& operator=(const SnapshotReader&) = delete;
    SnapshotReader(SnapshotReader&&) = delete;
    SnapshotReader& operator=(SnapshotReader&&) = delete;
protected:
    SnapshotReader() = default;
};
}

template <class U> class SnapshotReaderAdapter;
template <class U> class SnapshotReaderAdapterRef;
template <class U> class SnapshotReaderAdapterRefMut;
/*RUSTYCPP:GEN-END id=snapshot_manager.stream_interfaces*/

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
struct SnapshotConfig;

inline SnapshotConfig snapshot_config_defaults();
inline SnapshotConfig snapshot_config_for_replica(uint32_t partition_id,
                                                  uint32_t locale_id);

#if RUSTYCPP_RUST
pub struct SnapshotConfig {
    storage_path: std::string,
    snapshot_interval: usize,
    max_snapshots: usize,
    verify_on_load: bool,
    chunk_size: usize,
}

impl SnapshotConfig {
    fn defaults() -> SnapshotConfig {
        snapshot_config_defaults()
    }

    fn for_replica(partition_id: u32, locale_id: u32) -> SnapshotConfig {
        snapshot_config_for_replica(partition_id, locale_id)
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=snapshot_manager.config version=1 rust_sha256=8de117da9884879ddb452353addd353f0bc00ad7292d530aff6b93217755fcda*/
struct SnapshotConfig;

struct SnapshotConfig {
    std::string storage_path;
    size_t snapshot_interval;
    size_t max_snapshots;
    bool verify_on_load;
    size_t chunk_size;

    static SnapshotConfig defaults();
    static SnapshotConfig for_replica(uint32_t partition_id, uint32_t locale_id);
};


inline SnapshotConfig SnapshotConfig::defaults() {
    return snapshot_config_defaults();
}

inline SnapshotConfig SnapshotConfig::for_replica(uint32_t partition_id, uint32_t locale_id) {
    return snapshot_config_for_replica(std::move(partition_id), std::move(locale_id));
}
/*RUSTYCPP:GEN-END id=snapshot_manager.config*/

inline SnapshotConfig snapshot_config_defaults() {
  SnapshotConfig config{};
  config.storage_path = "";
  config.snapshot_interval = 10000;
  config.max_snapshots = 3;
  config.verify_on_load = true;
  config.chunk_size = 64 * 1024;
  return config;
}

inline SnapshotConfig snapshot_config_for_replica(uint32_t partition_id,
                                                  uint32_t locale_id) {
  SnapshotConfig config = SnapshotConfig::defaults();
  // Use username prefix to avoid conflicts between users.
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

}  // namespace raft
}  // namespace janus
