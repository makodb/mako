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
 * RustyCpp migration notes:
 * - SnapshotMetadata, SnapshotReader, SnapshotWriter, SnapshotManager, and
 *   SnapshotConfig are DSL-owned declaration surfaces.
 * - Generated fallback below the Rust blocks is committed for ordinary C++
 *   builds; edit the Rust source and regenerate, never patch GEN regions.
 * - Concrete managers use small C++ bridges where locks, filesystem effects,
 *   raw buffers, and unique_ptr factory ownership are easiest to audit.
 */

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <rusty/option.hpp>

#include "../constants.h"
#include "rrr/rrr.hpp"

namespace janus {
namespace raft {

// slotid_t and ballot_t come from janus::constants via the enclosing namespace.
using c_char = char;

/**
 * Metadata about a snapshot.
 */
struct SnapshotMetadata;

// DSL helpers: keep SnapshotMetadata methods thin and value-only. String
// formatting stays in ordinary C++ so metadata remains a low-risk DSL surface.
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
 *
 * The trait intentionally includes the unique_ptr factory methods. Concrete
 * managers still bridge through hand-written C++ classes so object ownership,
 * mutexes, and side effects stay explicit at the implementation boundary.
 */
#if RUSTYCPP_RUST
pub trait SnapshotManager {
    // @unsafe - Creates writer with side effects.
    fn BeginSnapshot(&mut self, last_index: u64, last_term: i64)
        -> std::unique_ptr<SnapshotWriter>;
    // @unsafe - May have side effects and reads from raw pointer.
    fn TakeSnapshot(&mut self, last_index: u64, last_term: i64,
                    data: *const c_char, size: usize) -> bool;
    // @unsafe - Creates reader with side effects.
    fn BeginLoad(&mut self, metadata: &SnapshotMetadata)
        -> std::unique_ptr<SnapshotReader>;
    // @unsafe - Writes to caller-owned output pointers.
    fn LoadLatestSnapshot(&mut self, metadata_out: *mut SnapshotMetadata,
                          data_out: *mut std::string) -> bool;
    // @safe
    fn GetLatestSnapshot(&self) -> rusty::Option<SnapshotMetadata>;
    // @safe
    fn ListSnapshots(&self) -> std::vector<SnapshotMetadata>;
    // @safe
    fn HasSnapshotAtOrAfter(&self, min_index: u64) -> bool;
    // @unsafe - May delete persisted snapshots.
    fn PruneSnapshots(&mut self, keep_after_index: u64) -> usize;
    // @unsafe - May delete persisted snapshots.
    fn DeleteAllSnapshots(&mut self) -> usize;
    // @lifetime: (&'a) -> &'a
    fn GetStoragePath(&self) -> &std::string;
}
#endif
/*RUSTYCPP:GEN-BEGIN id=snapshot_manager.manager_interface version=1 rust_sha256=65c211277b0d3a27a9a28af07363a4880ab7d28f226d94b2343d6960e653324c*/
namespace {
class SnapshotManager {
public:
    virtual ~SnapshotManager() noexcept(false) {}
    virtual std::unique_ptr<SnapshotWriter> BeginSnapshot(uint64_t last_index, int64_t last_term) = 0;
    virtual bool TakeSnapshot(uint64_t last_index, int64_t last_term, const c_char* data, size_t size) = 0;
    virtual std::unique_ptr<SnapshotReader> BeginLoad(const SnapshotMetadata& metadata) = 0;
    virtual bool LoadLatestSnapshot(SnapshotMetadata* metadata_out, std::string* data_out) = 0;
    virtual rusty::Option<SnapshotMetadata> GetLatestSnapshot() const = 0;
    virtual std::vector<SnapshotMetadata> ListSnapshots() const = 0;
    virtual bool HasSnapshotAtOrAfter(uint64_t min_index) const = 0;
    virtual size_t PruneSnapshots(uint64_t keep_after_index) = 0;
    virtual size_t DeleteAllSnapshots() = 0;
    virtual const std::string& GetStoragePath() const = 0;
    SnapshotManager(const SnapshotManager&) = delete;
    SnapshotManager& operator=(const SnapshotManager&) = delete;
    SnapshotManager(SnapshotManager&&) = delete;
    SnapshotManager& operator=(SnapshotManager&&) = delete;
protected:
    SnapshotManager() = default;
};
}

template <class U> class SnapshotManagerAdapter;
template <class U> class SnapshotManagerAdapterRef;
template <class U> class SnapshotManagerAdapterRefMut;
/*RUSTYCPP:GEN-END id=snapshot_manager.manager_interface*/

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
