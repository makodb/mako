#pragma once

/**
 * File-Based Snapshot Manager Implementation
 *
 * This header provides:
 * - FileSnapshotWriter: Writes snapshots to temporary files, renames on finalize
 * - FileSnapshotReader: Reads snapshots from files with verification
 * - FileSnapshotManager: Manages snapshot files with retention policy
 *
 * File naming convention:
 *   snapshot_<index>_<term>.snap     - Complete snapshots
 *   snapshot_<index>_<term>.snap.tmp - In-progress writes
 *
 * RustyCpp Compliance: Uses @safe/@unsafe annotations
 */

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <regex>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <rusty/num.hpp>
#include <rusty/slice.hpp>

#include "snapshot_format.hpp"
#include "snapshot_manager.hpp"

namespace janus {
namespace raft {

using rrr::Log_error;
using rrr::Log_info;

#if RUSTYCPP_RUST
pub const fn file_snapshot_advance_offset(offset: usize,
                                          size: usize) -> usize {
    offset.wrapping_add(size)
}

pub const fn file_snapshot_reader_bytes_to_read(data_size: usize,
                                                offset: usize,
                                                buffer_size: usize) -> usize {
    if offset >= data_size {
        0
    } else {
        let remaining = data_size.wrapping_sub(offset);
        if buffer_size < remaining {
            buffer_size
        } else {
            remaining
        }
    }
}

pub const fn file_snapshot_io_chunk_size(remaining: usize,
                                         max_io_size: usize) -> usize {
    if remaining < max_io_size {
        remaining
    } else {
        max_io_size
    }
}

pub const fn file_snapshot_write_fits_limit(offset: usize,
                                            size: usize,
                                            limit: usize) -> bool {
    offset <= limit && size <= limit - offset
}

pub const fn file_snapshot_reader_is_complete(valid: bool,
                                              data_size: usize,
                                              offset: usize) -> bool {
    valid && offset >= data_size
}

pub const fn file_snapshot_should_prune(snapshot_index: u64,
                                        keep_after_index: u64) -> bool {
    snapshot_index < keep_after_index
}

pub const fn file_snapshot_has_latest(snapshot_count: usize) -> bool {
    snapshot_count > 0
}

pub const fn file_snapshot_retention_required(snapshot_count: usize,
                                              max_snapshots: usize) -> bool {
    snapshot_count > max_snapshots
}
#endif
/*RUSTYCPP:GEN-BEGIN id=raft_file_snapshot.scalar_decisions version=1 rust_sha256=b8fcd26197c849b6d05e9c13975b295b88f86cf3d2d74d4ea65a3030ddadd8dc*/
constexpr size_t file_snapshot_advance_offset(size_t offset, size_t size);
constexpr size_t file_snapshot_reader_bytes_to_read(size_t data_size, size_t offset, size_t buffer_size);
constexpr size_t file_snapshot_io_chunk_size(size_t remaining, size_t max_io_size);
constexpr bool file_snapshot_write_fits_limit(size_t offset, size_t size, size_t limit);
constexpr bool file_snapshot_reader_is_complete(bool valid, size_t data_size, size_t offset);
constexpr bool file_snapshot_should_prune(uint64_t snapshot_index, uint64_t keep_after_index);
constexpr bool file_snapshot_has_latest(size_t snapshot_count);
constexpr bool file_snapshot_retention_required(size_t snapshot_count, size_t max_snapshots);
constexpr size_t file_snapshot_advance_offset(size_t offset, size_t size) {
    return rusty::wrapping_add(offset, static_cast<std::remove_cvref_t<decltype(offset)>>(std::move(size)));
}
constexpr size_t file_snapshot_reader_bytes_to_read(size_t data_size, size_t offset, size_t buffer_size) {
    if (rusty::detail::deref_if_pointer_like(offset) >= rusty::detail::deref_if_pointer_like(data_size)) {
        return static_cast<size_t>(0);
    } else {
        auto remaining = rusty::wrapping_sub(data_size, static_cast<std::remove_cvref_t<decltype(data_size)>>(std::move(offset)));
        if (rusty::detail::deref_if_pointer_like(buffer_size) < rusty::detail::deref_if_pointer_like(remaining)) {
            return std::move(buffer_size);
        } else {
            return std::move(remaining);
        }
    }
}
constexpr size_t file_snapshot_io_chunk_size(size_t remaining, size_t max_io_size) {
    if (rusty::detail::deref_if_pointer_like(remaining) < rusty::detail::deref_if_pointer_like(max_io_size)) {
        return std::move(remaining);
    } else {
        return std::move(max_io_size);
    }
}
constexpr bool file_snapshot_write_fits_limit(size_t offset, size_t size, size_t limit) {
    return (rusty::detail::deref_if_pointer_like(offset) <= rusty::detail::deref_if_pointer_like(limit)) && (rusty::detail::deref_if_pointer_like(size) <= (rusty::detail::deref_if_pointer_like(limit) - rusty::detail::deref_if_pointer_like(offset)));
}
constexpr bool file_snapshot_reader_is_complete(bool valid, size_t data_size, size_t offset) {
    return rusty::detail::deref_if_pointer_like(valid) && (rusty::detail::deref_if_pointer_like(offset) >= rusty::detail::deref_if_pointer_like(data_size));
}
constexpr bool file_snapshot_should_prune(uint64_t snapshot_index, uint64_t keep_after_index) {
    return rusty::detail::deref_if_pointer_like(snapshot_index) < rusty::detail::deref_if_pointer_like(keep_after_index);
}
constexpr bool file_snapshot_has_latest(size_t snapshot_count) {
    return rusty::detail::deref_if_pointer_like(snapshot_count) > 0;
}
constexpr bool file_snapshot_retention_required(size_t snapshot_count, size_t max_snapshots) {
    return rusty::detail::deref_if_pointer_like(snapshot_count) > rusty::detail::deref_if_pointer_like(max_snapshots);
}
/*RUSTYCPP:GEN-END id=raft_file_snapshot.scalar_decisions*/

static_assert(file_snapshot_advance_offset(7, 5) == 12);
static_assert(file_snapshot_advance_offset(static_cast<size_t>(-1), 1) == 0);
static_assert(file_snapshot_reader_bytes_to_read(10, 4, 3) == 3);
static_assert(file_snapshot_reader_bytes_to_read(10, 4, 9) == 6);
static_assert(file_snapshot_reader_bytes_to_read(10, 10, 9) == 0);
static_assert(file_snapshot_reader_bytes_to_read(10, 11, 9) == 0);
static_assert(file_snapshot_io_chunk_size(3, 7) == 3);
static_assert(file_snapshot_io_chunk_size(9, 7) == 7);
static_assert(file_snapshot_write_fits_limit(3, 4, 7));
static_assert(!file_snapshot_write_fits_limit(3, 5, 7));
static_assert(!file_snapshot_write_fits_limit(8, 0, 7));
static_assert(!file_snapshot_reader_is_complete(false, 10, 10));
static_assert(file_snapshot_reader_is_complete(true, 10, 10));
static_assert(file_snapshot_should_prune(9, 10));
static_assert(!file_snapshot_should_prune(10, 10));
static_assert(!file_snapshot_has_latest(0));
static_assert(file_snapshot_has_latest(1));
static_assert(file_snapshot_retention_required(4, 3));
static_assert(!file_snapshot_retention_required(3, 3));

// The POSIX calls below are deliberately kept in small C++ kernels: raw file
// descriptors and errno are outside the Rust DSL's supported surface.

// @unsafe - Calls POSIX write and reads errno; completes all requested bytes.
inline bool file_snapshot_write_all(
    int fd, const char* data, size_t size) {
  if (data == nullptr && size != 0) {
    errno = EINVAL;
    return false;
  }

  size_t offset = 0;
  const size_t max_io_size =
      static_cast<size_t>(std::numeric_limits<ssize_t>::max());
  while (offset < size) {
    const size_t chunk_size = file_snapshot_io_chunk_size(
        size - offset, max_io_size);
    const ssize_t result = ::write(fd, data + offset, chunk_size);
    if (result < 0 && errno == EINTR) {
      continue;
    }
    if (result <= 0) {
      if (result == 0) {
        errno = EIO;
      }
      return false;
    }
    offset = file_snapshot_advance_offset(
        offset, static_cast<size_t>(result));
  }
  return true;
}

// @unsafe - Calls POSIX read and reads errno; rejects an early EOF.
inline bool file_snapshot_read_all(int fd, char* data, size_t size) {
  if (data == nullptr && size != 0) {
    errno = EINVAL;
    return false;
  }

  size_t offset = 0;
  const size_t max_io_size =
      static_cast<size_t>(std::numeric_limits<ssize_t>::max());
  while (offset < size) {
    const size_t chunk_size = file_snapshot_io_chunk_size(
        size - offset, max_io_size);
    const ssize_t result = ::read(fd, data + offset, chunk_size);
    if (result < 0 && errno == EINTR) {
      continue;
    }
    if (result <= 0) {
      if (result == 0) {
        errno = EIO;
      }
      return false;
    }
    offset = file_snapshot_advance_offset(
        offset, static_cast<size_t>(result));
  }
  return true;
}

// @unsafe - Calls POSIX fsync and retries only the unambiguous EINTR case.
inline bool file_snapshot_fsync(int fd) {
  int result;
  do {
    result = ::fsync(fd);
  } while (result < 0 && errno == EINTR);
  return result == 0;
}

// @unsafe - Calls POSIX close. Retrying close after EINTR can close a reused
// descriptor on Linux, so a failed close is reported without a retry.
inline bool file_snapshot_close(int fd) {
  return ::close(fd) == 0;
}

// @unsafe - Uses std::string path operations at the POSIX boundary.
inline std::string file_snapshot_parent_directory(const std::string& path) {
  size_t end = path.size();
  while (end > 1 && path[end - 1] == '/') {
    --end;
  }
  if (end == 0) {
    return ".";
  }
  const size_t slash = path.find_last_of('/', end - 1);
  if (slash == std::string::npos) {
    return ".";
  }
  if (slash == 0) {
    return "/";
  }
  return path.substr(0, slash);
}

// @unsafe - Opens, fsyncs, and closes a POSIX directory descriptor.
inline bool file_snapshot_sync_directory(const std::string& path) {
  int fd;
  do {
    fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  } while (fd < 0 && errno == EINTR);
  if (fd < 0) {
    return false;
  }

  const bool synced = file_snapshot_fsync(fd);
  const int sync_error = errno;
  const bool closed = file_snapshot_close(fd);
  if (!synced) {
    errno = sync_error;
    return false;
  }
  return closed;
}

/**
 * File-based snapshot writer.
 * Accumulates data in memory, writes to temp file, renames on finalize.
 */
class FileSnapshotWriter : public SnapshotWriter {
 public:
  // @unsafe - Creates file
  FileSnapshotWriter(const std::string& final_path,
                     const std::string& temp_path,
                     slotid_t last_index,
                     ballot_t last_term)
      : final_path_(final_path),
        temp_path_(temp_path),
        last_index_(last_index),
        last_term_(last_term) {
    Log_info("[SNAPSHOT-WRITER] Creating snapshot: index={} term={} path={}",
             last_index_, last_term_, final_path_.c_str());
  }

  // @unsafe - May delete temp file
  ~FileSnapshotWriter() override {
    if (!finalized_ && !aborted_) {
      Abort();
    }
  }

  /**
   * Accumulate data for the snapshot.
   * Data is buffered until Finalize() is called.
   */
  // @unsafe - Reads from raw pointer
  bool Write(const char* data, size_t size) override {
    if (finalized_ || aborted_) {
      Log_error("[SNAPSHOT-WRITER] Write after finalize/abort");
      return false;
    }
    if (!file_snapshot_write_fits_limit(
            offset_, size, SnapshotFormat::MAX_PAYLOAD_SIZE)) {
      Log_error("[SNAPSHOT-WRITER] Snapshot payload exceeds {} byte limit",
                SnapshotFormat::MAX_PAYLOAD_SIZE);
      return false;
    }
    if (size == 0) {
      return true;
    }
    if (data == nullptr) {
      Log_error("[SNAPSHOT-WRITER] Null data with nonzero size");
      return false;
    }
    buffer_.append(data, size);
    offset_ = file_snapshot_advance_offset(offset_, size);
    return true;
  }

  /**
   * Finalize the snapshot: serialize to format, write to temp, rename.
   */
  // @unsafe - File I/O operations
  bool Finalize() override {
    if (finalized_ || aborted_) {
      Log_error("[SNAPSHOT-WRITER] Finalize after finalize/abort");
      return false;
    }

    // Serialize to binary format
    std::string serialized;
    if (!SnapshotFormat::Serialize(last_index_, last_term_,
                                   buffer_.data(), buffer_.size(),
                                   &serialized)) {
      Log_error("[SNAPSHOT-WRITER] Failed to serialize snapshot");
      return false;
    }

    // Write to temp file. Opening is retried because an interrupt before a
    // descriptor is returned has no side effects for the caller to reconcile.
    int fd;
    do {
      fd = ::open(temp_path_.c_str(),
                  O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
      Log_error("[SNAPSHOT-WRITER] Failed to open temp file: {} ({})",
                temp_path_.c_str(), strerror(errno));
      return false;
    }

    if (!file_snapshot_write_all(fd, serialized.data(), serialized.size())) {
      const int write_error = errno;
      if (!file_snapshot_close(fd)) {
        Log_error("[SNAPSHOT-WRITER] Failed to close temp file after write "
                  "failure: {} ({})",
                  temp_path_.c_str(), strerror(errno));
      }
      if (::unlink(temp_path_.c_str()) < 0 && errno != ENOENT) {
        Log_error("[SNAPSHOT-WRITER] Failed to remove incomplete temp file: "
                  "{} ({})",
                  temp_path_.c_str(), strerror(errno));
      }
      Log_error("[SNAPSHOT-WRITER] Failed to write snapshot: {}",
                strerror(write_error));
      return false;
    }

    // Sync to disk
    if (!file_snapshot_fsync(fd)) {
      const int sync_error = errno;
      if (!file_snapshot_close(fd)) {
        Log_error("[SNAPSHOT-WRITER] Failed to close temp file after fsync "
                  "failure: {} ({})",
                  temp_path_.c_str(), strerror(errno));
      }
      if (::unlink(temp_path_.c_str()) < 0 && errno != ENOENT) {
        Log_error("[SNAPSHOT-WRITER] Failed to remove unsynced temp file: "
                  "{} ({})",
                  temp_path_.c_str(), strerror(errno));
      }
      Log_error("[SNAPSHOT-WRITER] Failed to fsync: {}",
                strerror(sync_error));
      return false;
    }
    if (!file_snapshot_close(fd)) {
      const int close_error = errno;
      if (::unlink(temp_path_.c_str()) < 0 && errno != ENOENT) {
        Log_error("[SNAPSHOT-WRITER] Failed to remove temp file after close "
                  "failure: {} ({})",
                  temp_path_.c_str(), strerror(errno));
      }
      Log_error("[SNAPSHOT-WRITER] Failed to close temp file: {}",
                strerror(close_error));
      return false;
    }

    // Atomic rename
    if (::rename(temp_path_.c_str(), final_path_.c_str()) < 0) {
      Log_error("[SNAPSHOT-WRITER] Failed to rename {} -> {}: {}",
                temp_path_.c_str(), final_path_.c_str(), strerror(errno));
      if (::unlink(temp_path_.c_str()) < 0 && errno != ENOENT) {
        Log_error("[SNAPSHOT-WRITER] Failed to remove temp file after rename "
                  "failure: {} ({})",
                  temp_path_.c_str(), strerror(errno));
      }
      return false;
    }

    // rename() makes the snapshot visible but does not make the directory
    // entry crash-durable. Do not report success until the containing
    // directory is fsynced. FileSnapshotManager uses one directory for both
    // paths; syncing the source as well preserves the guarantee for callers
    // that construct a writer with two different directories.
    const std::string final_parent =
        file_snapshot_parent_directory(final_path_);
    const std::string temp_parent =
        file_snapshot_parent_directory(temp_path_);
    if (!file_snapshot_sync_directory(final_parent) ||
        (temp_parent != final_parent &&
         !file_snapshot_sync_directory(temp_parent))) {
      const int sync_error = errno;
      Log_error("[SNAPSHOT-WRITER] Failed to fsync snapshot directory after "
                "publishing {}: {}",
                final_path_.c_str(), strerror(sync_error));
      // Leave the fully written file in place. Removing it here could delete
      // an older durable snapshot that rename() replaced at the same path.
      // Returning false keeps Raft from compacting the log, so recovery is
      // safe whether the new directory entry survives a crash or not.
      return false;
    }

    finalized_ = true;
    Log_info("[SNAPSHOT-WRITER] Snapshot finalized: {} ({} bytes data, {} bytes total)",
             final_path_.c_str(), buffer_.size(), serialized.size());
    return true;
  }

  /**
   * Abort the snapshot, cleaning up any temporary files.
   */
  // @unsafe - File operations
  bool Abort() override {
    if (finalized_ || aborted_) {
      return true;
    }
    if (::unlink(temp_path_.c_str()) < 0 && errno != ENOENT) {
      Log_error("[SNAPSHOT-WRITER] Failed to remove aborted temp file: {} ({})",
                temp_path_.c_str(), strerror(errno));
      return false;
    }
    aborted_ = true;
    Log_info("[SNAPSHOT-WRITER] Snapshot aborted");
    return true;
  }

  // @safe - Returns current offset
  size_t GetOffset() const override { return offset_; }

 private:
  std::string final_path_;
  std::string temp_path_;
  slotid_t last_index_;
  ballot_t last_term_;
  size_t offset_{0};
  bool finalized_{false};
  bool aborted_{false};
  std::string buffer_;
};

/**
 * File-based snapshot reader.
 * Reads and verifies snapshot on construction, provides streaming read.
 */
class FileSnapshotReader : public SnapshotReader {
 public:
  // @unsafe - Opens and reads file
  explicit FileSnapshotReader(const std::string& path) : path_(path) {
    // Read entire file
    int fd;
    do {
      fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
      Log_error("[SNAPSHOT-READER] Failed to open: {} ({})",
                path.c_str(), strerror(errno));
      valid_ = false;
      return;
    }

    struct stat st;
    int stat_result;
    do {
      stat_result = ::fstat(fd, &st);
    } while (stat_result < 0 && errno == EINTR);
    if (stat_result < 0) {
      const int stat_error = errno;
      if (!file_snapshot_close(fd)) {
        Log_error("[SNAPSHOT-READER] Failed to close after stat failure: "
                  "{} ({})",
                  path.c_str(), strerror(errno));
      }
      Log_error("[SNAPSHOT-READER] Failed to stat: {} ({})",
                path.c_str(), strerror(stat_error));
      valid_ = false;
      return;
    }

    if (st.st_size < 0 ||
        static_cast<uintmax_t>(st.st_size) >
            static_cast<uintmax_t>(std::numeric_limits<size_t>::max())) {
      if (!file_snapshot_close(fd)) {
        Log_error("[SNAPSHOT-READER] Failed to close oversized snapshot: "
                  "{} ({})",
                  path.c_str(), strerror(errno));
      }
      Log_error("[SNAPSHOT-READER] Snapshot size is not representable: {}",
                path.c_str());
      valid_ = false;
      return;
    }

    const size_t file_size = static_cast<size_t>(st.st_size);
    if (!snapshot_serialized_size_within_limit(
            file_size, SnapshotFormat::MAX_SERIALIZED_SIZE)) {
      if (!file_snapshot_close(fd)) {
        Log_error("[SNAPSHOT-READER] Failed to close oversized snapshot: "
                  "{} ({})",
                  path.c_str(), strerror(errno));
      }
      Log_error("[SNAPSHOT-READER] Snapshot file exceeds {} byte limit: {} "
                "({} bytes)",
                SnapshotFormat::MAX_SERIALIZED_SIZE, path.c_str(), file_size);
      valid_ = false;
      return;
    }
    file_data_.resize(file_size);
    if (!file_snapshot_read_all(fd, file_data_.data(), file_size)) {
      const int read_error = errno;
      if (!file_snapshot_close(fd)) {
        Log_error("[SNAPSHOT-READER] Failed to close after read failure: "
                  "{} ({})",
                  path.c_str(), strerror(errno));
      }
      Log_error("[SNAPSHOT-READER] Failed to read complete snapshot: {} ({})",
                path.c_str(), strerror(read_error));
      valid_ = false;
      return;
    }
    if (!file_snapshot_close(fd)) {
      Log_error("[SNAPSHOT-READER] Failed to close: {} ({})",
                path.c_str(), strerror(errno));
      valid_ = false;
      return;
    }

    // Deserialize and verify
    uint64_t last_index, last_term;
    if (!SnapshotFormat::Deserialize(file_data_.data(), file_data_.size(),
                                     &last_index, &last_term, &data_)) {
      Log_error("[SNAPSHOT-READER] Failed to deserialize: {}", path.c_str());
      valid_ = false;
      return;
    }

    // Populate metadata
    metadata_.last_included_index = last_index;
    metadata_.last_included_term = last_term;
    metadata_.size_bytes = data_.size();

    // Get header for timestamp
    SnapshotHeader header;
    if (SnapshotFormat::GetHeader(file_data_.data(), file_data_.size(), &header)) {
      metadata_.timestamp_ms = header.timestamp_ms;
    }

    valid_ = true;
    Log_info("[SNAPSHOT-READER] Opened snapshot: index={} term={} size={}",
             last_index, last_term, data_.size());
  }

  ~FileSnapshotReader() override = default;

  /**
   * Read a chunk of snapshot data.
   */
  // @unsafe - Writes to raw buffer
  bool Read(char* buffer, size_t buffer_size, size_t* bytes_read) override {
    if (!valid_) {
      *bytes_read = 0;
      return false;
    }

    size_t to_read = file_snapshot_reader_bytes_to_read(
        data_.size(), read_offset_, buffer_size);
    if (to_read > 0) {
      std::memcpy(buffer, data_.data() + read_offset_, to_read);
      read_offset_ = file_snapshot_advance_offset(read_offset_, to_read);
    }
    *bytes_read = to_read;
    return true;
  }

  // @unsafe - Check if all data read
  bool IsComplete() const override {
    return file_snapshot_reader_is_complete(
        valid_, data_.size(), read_offset_);
  }

  // @lifetime: (&'a) -> &'a
  const SnapshotMetadata& GetMetadata() const override { return metadata_; }

  // @safe - Returns current offset
  size_t GetOffset() const override { return read_offset_; }

  // @safe - Check if reader is valid
  bool IsValid() const { return valid_; }

 private:
  std::string path_;
  std::string file_data_;
  std::string data_;
  SnapshotMetadata metadata_;
  size_t read_offset_{0};
  bool valid_{false};
};

/**
 * File-based snapshot manager implementation.
 * Stores snapshots in a directory with automatic retention policy.
 */
class FileSnapshotManager : public SnapshotManager {
 public:
  // @unsafe - Validates or durably creates the storage directory.
  explicit FileSnapshotManager(const SnapshotConfig& config)
      : config_(config),
        storage_ready_(config.max_snapshots != 0 &&
                       EnsureDirectory(config.storage_path)) {
    if (!storage_ready_) {
      if (config_.max_snapshots == 0) {
        Log_error("[SNAPSHOT-MGR] max_snapshots must be at least one");
      } else {
        Log_error("[SNAPSHOT-MGR] Storage is not ready: path={} ({})",
                  config_.storage_path.c_str(), strerror(errno));
      }
    } else {
      Log_info("[SNAPSHOT-MGR] Initialized: path={} max_snapshots={}",
               config_.storage_path.c_str(), config_.max_snapshots);
    }
  }

  ~FileSnapshotManager() override = default;

  // @safe - Immutable result of constructor-time directory validation and
  // durability barriers.
  bool IsStorageReady() const { return storage_ready_; }

  // ========================================================================
  // Snapshot Creation
  // ========================================================================

  // @unsafe - Creates writer
  std::unique_ptr<SnapshotWriter> BeginSnapshot(
      slotid_t last_index, ballot_t last_term) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!storage_ready_) {
      Log_error("[SNAPSHOT-MGR] Cannot begin snapshot: storage is not ready");
      return nullptr;
    }
    std::string final_path = GetSnapshotPath(last_index, last_term);
    std::string temp_path = GetTempPath(last_index, last_term);
    return std::make_unique<FileSnapshotWriter>(final_path, temp_path,
                                                 last_index, last_term);
  }

  // @unsafe - Creates and finalizes snapshot
  bool TakeSnapshot(slotid_t last_index, ballot_t last_term,
                    const char* data, size_t size) override {
    if (!storage_ready_) {
      Log_error("[SNAPSHOT-MGR] Cannot take snapshot: storage is not ready");
      return false;
    }
    auto writer = BeginSnapshot(last_index, last_term);
    if (!writer) return false;
    if (!writer->Write(data, size)) return false;
    if (!writer->Finalize()) return false;

    // Apply retention policy under the manager lock. Writer finalization is
    // intentionally outside this lock because it can perform slow disk I/O.
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ApplyRetentionPolicy();
    }
    return true;
  }

  // ========================================================================
  // Snapshot Loading
  // ========================================================================

  // @unsafe - Creates reader
  std::unique_ptr<SnapshotReader> BeginLoad(
      const SnapshotMetadata& metadata) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!storage_ready_) {
      Log_error("[SNAPSHOT-MGR] Cannot load snapshot: storage is not ready");
      return nullptr;
    }
    std::string path = GetSnapshotPath(metadata.last_included_index,
                                        metadata.last_included_term);
    auto reader = std::make_unique<FileSnapshotReader>(path);
    if (!reader->IsValid()) {
      return nullptr;
    }
    return reader;
  }

  // @unsafe - Reads file
  bool LoadLatestSnapshot(SnapshotMetadata* metadata_out,
                          std::string* data_out) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!storage_ready_) {
      Log_error("[SNAPSHOT-MGR] Cannot load latest snapshot: storage is not "
                "ready");
      return false;
    }
    auto latest = GetLatestSnapshotUnlocked();
    if (latest.is_none()) {
      return false;
    }

    auto meta = latest.unwrap();
    std::string path = GetSnapshotPath(meta.last_included_index,
                                        meta.last_included_term);
    FileSnapshotReader reader(path);
    if (!reader.IsValid()) {
      return false;
    }

    *metadata_out = reader.GetMetadata();

    // Read all data
    data_out->resize(metadata_out->size_bytes);
    size_t total_read = 0;
    while (!reader.IsComplete()) {
      size_t bytes_read;
      if (!reader.Read(data_out->data() + total_read,
                       data_out->size() - total_read, &bytes_read)) {
        return false;
      }
      total_read += bytes_read;
    }

    return true;
  }

  // ========================================================================
  // Snapshot Queries
  // ========================================================================

  // @unsafe (with mutex)
  rusty::Option<SnapshotMetadata> GetLatestSnapshot() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!storage_ready_) {
      return rusty::None;
    }
    return GetLatestSnapshotUnlocked();
  }

  // @unsafe (with mutex)
  std::vector<SnapshotMetadata> ListSnapshots() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!storage_ready_) {
      return {};
    }
    return ListSnapshotsUnlocked();
  }

  // @unsafe (with mutex)
  bool HasSnapshotAtOrAfter(slotid_t min_index) const override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!storage_ready_) {
      return false;
    }
    auto snapshots = ListSnapshotsUnlocked();
    for (const auto& snap : snapshots) {
      if (snap.last_included_index >= min_index) {
        return true;
      }
    }
    return false;
  }

  // ========================================================================
  // Snapshot Cleanup
  // ========================================================================

  // @unsafe - Deletes files
  size_t PruneSnapshots(slotid_t keep_after_index) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!storage_ready_) {
      return 0;
    }
    auto snapshots = ListSnapshotsUnlocked();
    size_t deleted = 0;

    for (const auto& snap : snapshots) {
      if (file_snapshot_should_prune(
              snap.last_included_index, keep_after_index)) {
        std::string path = GetSnapshotPath(snap.last_included_index,
                                            snap.last_included_term);
        if (unlink(path.c_str()) == 0) {
          Log_info("[SNAPSHOT-MGR] Pruned snapshot: {}", path.c_str());
          deleted++;
        }
      }
    }
    SyncStorageDirectoryAfterDeletion(deleted, "pruning snapshots");
    return deleted;
  }

  // @unsafe - Deletes files
  size_t DeleteAllSnapshots() override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!storage_ready_) {
      return 0;
    }
    auto snapshots = ListSnapshotsUnlocked();
    size_t deleted = 0;

    for (const auto& snap : snapshots) {
      std::string path = GetSnapshotPath(snap.last_included_index,
                                          snap.last_included_term);
      if (unlink(path.c_str()) == 0) {
        deleted++;
      }
    }
    SyncStorageDirectoryAfterDeletion(deleted, "deleting all snapshots");
    Log_info("[SNAPSHOT-MGR] Deleted all {} snapshots", deleted);
    return deleted;
  }

  // ========================================================================
  // Configuration
  // ========================================================================

  // @lifetime: (&'a) -> &'a
  const std::string& GetStoragePath() const override {
    return config_.storage_path;
  }

 private:
  SnapshotConfig config_;
  const bool storage_ready_;
  mutable std::mutex mutex_;

  // @unsafe - Validates the directory and fsyncs both it and its parent. If
  // mkdir creates the directory, these barriers make the directory entry
  // durable before any snapshot can be published within it.
  static bool EnsureDirectory(const std::string& path) {
    struct stat st;
    int stat_result;
    do {
      stat_result = ::stat(path.c_str(), &st);
    } while (stat_result < 0 && errno == EINTR);

    if (stat_result == 0) {
      if (!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return false;
      }
    } else {
      if (errno != ENOENT) {
        return false;
      }

      int mkdir_result;
      do {
        mkdir_result = ::mkdir(path.c_str(), 0755);
      } while (mkdir_result < 0 && errno == EINTR);
      if (mkdir_result < 0) {
        if (errno != EEXIST) {
          return false;
        }
        do {
          stat_result = ::stat(path.c_str(), &st);
        } while (stat_result < 0 && errno == EINTR);
        if (stat_result < 0 || !S_ISDIR(st.st_mode)) {
          if (stat_result == 0) {
            errno = ENOTDIR;
          }
          return false;
        }
      }
    }

    if (!file_snapshot_sync_directory(path)) {
      return false;
    }
    const std::string parent = file_snapshot_parent_directory(path);
    if (parent == path) {
      return true;
    }
    return file_snapshot_sync_directory(parent);
  }

  // @unsafe - Generates path
  std::string GetSnapshotPath(slotid_t index, ballot_t term) const {
    return config_.storage_path + "/snapshot_" + std::to_string(index) +
           "_" + std::to_string(term) + ".snap";
  }

  // @unsafe - Generates temp path
  std::string GetTempPath(slotid_t index, ballot_t term) const {
    return GetSnapshotPath(index, term) + ".tmp";
  }

  // @unsafe - Fsyncs the storage directory after one or more successful
  // unlinks. The public deletion APIs return an unlink count, so a barrier
  // failure is reported through logging without falsifying that count.
  void SyncStorageDirectoryAfterDeletion(
      size_t deleted, const char* operation) const {
    if (deleted == 0) {
      return;
    }
    if (!file_snapshot_sync_directory(config_.storage_path)) {
      Log_error("[SNAPSHOT-MGR] Failed to fsync directory after {}: {} ({})",
                operation, config_.storage_path.c_str(), strerror(errno));
    }
  }

  // @unsafe - Directory operations (must hold mutex)
  std::vector<SnapshotMetadata> ListSnapshotsUnlocked() const {
    std::vector<SnapshotMetadata> result;

    DIR* dir = opendir(config_.storage_path.c_str());
    if (!dir) {
      return result;
    }

    std::regex pattern(R"(snapshot_(\d+)_(\d+)\.snap)");
    struct dirent* entry;

    while ((entry = readdir(dir)) != nullptr) {
      std::string name(entry->d_name);
      std::smatch match;
      if (std::regex_match(name, match, pattern)) {
        SnapshotMetadata meta;
        try {
          size_t index_end = 0;
          size_t term_end = 0;
          const std::string index_text = match[1].str();
          const std::string term_text = match[2].str();
          meta.last_included_index = std::stoull(index_text, &index_end);
          meta.last_included_term = std::stoull(term_text, &term_end);
          if (index_end != index_text.size() || term_end != term_text.size()) {
            continue;
          }
        } catch (...) {
          // A filename outside uint64_t is not a valid snapshot. Ignore it
          // without allowing directory contents to unwind startup recovery.
          continue;
        }

        // Get file size
        std::string path = config_.storage_path + "/" + name;
        struct stat st;
        if (stat(path.c_str(), &st) == 0) {
          meta.size_bytes = st.st_size;
        }

        result.push_back(meta);
      }
    }
    closedir(dir);

    // Sort by index descending (newest first)
    std::sort(result.begin(), result.end(),
              [](const SnapshotMetadata& a, const SnapshotMetadata& b) {
                return a.last_included_index > b.last_included_index;
              });

    return result;
  }

  // @unsafe (must hold mutex)
  rusty::Option<SnapshotMetadata> GetLatestSnapshotUnlocked() const {
    auto snapshots = ListSnapshotsUnlocked();
    if (!file_snapshot_has_latest(snapshots.size())) {
      return rusty::None;
    }
    return rusty::Some(snapshots[0]);
  }

  // @unsafe - Deletes files (must hold mutex)
  void ApplyRetentionPolicy() {
    auto snapshots = ListSnapshotsUnlocked();
    if (!file_snapshot_retention_required(
            snapshots.size(), config_.max_snapshots)) {
      return;
    }

    // Delete oldest snapshots beyond retention limit
    size_t deleted = 0;
    for (size_t i = config_.max_snapshots; i < snapshots.size(); i++) {
      std::string path = GetSnapshotPath(snapshots[i].last_included_index,
                                          snapshots[i].last_included_term);
      if (::unlink(path.c_str()) == 0) {
        Log_info("[SNAPSHOT-MGR] Retention policy: deleted {}", path.c_str());
        deleted++;
      }
    }
    SyncStorageDirectoryAfterDeletion(deleted, "applying retention policy");
  }
};

}  // namespace raft
}  // namespace janus
