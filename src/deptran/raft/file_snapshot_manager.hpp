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
#include <cstring>
#include <memory>
#include <mutex>
#include <regex>
#include <string>
#include <vector>

#include "snapshot_format.hpp"
#include "snapshot_manager.hpp"

namespace janus {
namespace raft {

inline std::string file_snapshot_path(const std::string& storage_path,
                                      slotid_t index,
                                      ballot_t term) {
  return storage_path + "/snapshot_" + std::to_string(index) + "_" +
         std::to_string(term) + ".snap";
}

inline std::string file_snapshot_temp_path(const std::string& storage_path,
                                           slotid_t index,
                                           ballot_t term) {
  return file_snapshot_path(storage_path, index, term) + ".tmp";
}

/**
 * File-based snapshot writer.
 * Accumulates data in memory, writes to temp file, renames on finalize.
 */
class FileSnapshotWriter : public SnapshotWriter {
 public:
  // @unsafe - Records final/temp paths. Finalize owns the temp-file write and
  // rename; Abort/destructor clean up an unfinished temp file.
  FileSnapshotWriter(const std::string& final_path,
                     const std::string& temp_path,
                     slotid_t last_index,
                     ballot_t last_term)
      : final_path_(final_path),
        temp_path_(temp_path),
        last_index_(last_index),
        last_term_(last_term) {
    Log_info("[SNAPSHOT-WRITER] Creating snapshot: index=%lu term=%lu path=%s",
             last_index_, last_term_, final_path_.c_str());
  }

  // @unsafe - Best-effort cleanup of an unfinished temp file.
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
    buffer_.append(data, size);
    offset_ += size;
    return true;
  }

  /**
   * Finalize the snapshot: serialize to format, write to temp, rename.
   */
  // @unsafe - File I/O operations. Owns the local fd until close(), then
  // atomically transfers temp_path_ into final_path_ with rename().
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

    // Write to temp file
    int fd = open(temp_path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
      Log_error("[SNAPSHOT-WRITER] Failed to open temp file: %s (%s)",
                temp_path_.c_str(), strerror(errno));
      return false;
    }

    ssize_t written = write(fd, serialized.data(), serialized.size());
    if (written != static_cast<ssize_t>(serialized.size())) {
      Log_error("[SNAPSHOT-WRITER] Failed to write snapshot: wrote %zd of %zu",
                written, serialized.size());
      close(fd);
      unlink(temp_path_.c_str());
      return false;
    }

    // Sync to disk
    if (fsync(fd) < 0) {
      Log_error("[SNAPSHOT-WRITER] Failed to fsync: %s", strerror(errno));
      close(fd);
      unlink(temp_path_.c_str());
      return false;
    }
    close(fd);

    // Atomic rename
    if (rename(temp_path_.c_str(), final_path_.c_str()) < 0) {
      Log_error("[SNAPSHOT-WRITER] Failed to rename %s -> %s: %s",
                temp_path_.c_str(), final_path_.c_str(), strerror(errno));
      unlink(temp_path_.c_str());
      return false;
    }

    finalized_ = true;
    Log_info("[SNAPSHOT-WRITER] Snapshot finalized: %s (%zu bytes data, %zu bytes total)",
             final_path_.c_str(), buffer_.size(), serialized.size());
    return true;
  }

  /**
   * Abort the snapshot, cleaning up any temporary files.
   */
  // @unsafe - Deletes the temp file if this writer has not finalized.
  bool Abort() override {
    if (finalized_ || aborted_) {
      return true;
    }
    aborted_ = true;
    unlink(temp_path_.c_str());
    Log_info("[SNAPSHOT-WRITER] Snapshot aborted");
    return true;
  }

  // @safe - Returns current offset
  size_t GetOffset() const override { return offset_; }

 private:
  // @unsafe - owned path strings; no file handle is kept between calls.
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
  // @unsafe - Opens the snapshot path with a local fd, reads it fully, closes
  // the fd, then owns the decoded payload in data_.
  explicit FileSnapshotReader(const std::string& path) : path_(path) {
    // Read entire file
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) {
      Log_error("[SNAPSHOT-READER] Failed to open: %s (%s)",
                path.c_str(), strerror(errno));
      valid_ = false;
      return;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
      Log_error("[SNAPSHOT-READER] Failed to stat: %s", path.c_str());
      close(fd);
      valid_ = false;
      return;
    }

    file_data_.resize(st.st_size);
    ssize_t bytes_read = read(fd, file_data_.data(), st.st_size);
    close(fd);

    if (bytes_read != st.st_size) {
      Log_error("[SNAPSHOT-READER] Failed to read: got %zd of %ld",
                bytes_read, st.st_size);
      valid_ = false;
      return;
    }

    // Deserialize and verify
    uint64_t last_index, last_term;
    if (!SnapshotFormat::Deserialize(file_data_.data(), file_data_.size(),
                                     &last_index, &last_term, &data_)) {
      Log_error("[SNAPSHOT-READER] Failed to deserialize: %s", path.c_str());
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
    Log_info("[SNAPSHOT-READER] Opened snapshot: index=%lu term=%lu size=%zu",
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

    size_t remaining = data_.size() - read_offset_;
    size_t to_read = std::min(buffer_size, remaining);
    if (to_read > 0) {
      std::memcpy(buffer, data_.data() + read_offset_, to_read);
      read_offset_ += to_read;
    }
    *bytes_read = to_read;
    return true;
  }

  // @unsafe - Check if all data read
  bool IsComplete() const override {
    return valid_ && read_offset_ >= data_.size();
  }

  // @lifetime: (&'a) -> &'a
  const SnapshotMetadata& GetMetadata() const override { return metadata_; }

  // @safe - Returns current offset
  size_t GetOffset() const override { return read_offset_; }

  // @safe - Check if reader is valid
  bool IsValid() const { return valid_; }

 private:
  // @unsafe - path is retained for diagnostics; file_data_/data_ are owned
  // in-memory copies, not borrowed file-backed buffers.
  std::string path_;
  std::string file_data_;
  std::string data_;
  SnapshotMetadata metadata_{};
  size_t read_offset_{0};
  bool valid_{false};
};

/**
 * File-based snapshot manager implementation.
 * Stores snapshots in a directory with automatic retention policy.
 */
class FileSnapshotManager : public SnapshotManager {
 public:
  // @unsafe - May create the snapshot directory; config_ owns the path string.
  explicit FileSnapshotManager(const SnapshotConfig& config) : config_(config) {
    EnsureDirectory();
    Log_info("[SNAPSHOT-MGR] Initialized: path=%s max_snapshots=%zu",
             config_.storage_path.c_str(), config_.max_snapshots);
  }

  ~FileSnapshotManager() override = default;

  // ========================================================================
  // Snapshot Creation
  // ========================================================================

  // @unsafe - Returns an owned writer. The writer owns path strings and later
  // owns any temp-file cleanup for this snapshot.
  std::unique_ptr<SnapshotWriter> BeginSnapshot(
      slotid_t last_index, ballot_t last_term) override {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string final_path = file_snapshot_path(config_.storage_path,
                                                last_index, last_term);
    std::string temp_path = file_snapshot_temp_path(config_.storage_path,
                                                    last_index, last_term);
    return std::make_unique<FileSnapshotWriter>(final_path, temp_path,
                                                 last_index, last_term);
  }

  // @unsafe - Creates and finalizes snapshot
  bool TakeSnapshot(slotid_t last_index, ballot_t last_term,
                    const char* data, size_t size) override {
    auto writer = BeginSnapshot(last_index, last_term);
    if (!writer) return false;
    if (!writer->Write(data, size)) return false;
    if (!writer->Finalize()) return false;

    // Apply retention policy
    ApplyRetentionPolicy();
    return true;
  }

  // ========================================================================
  // Snapshot Loading
  // ========================================================================

  // @unsafe - Returns an owned reader with an in-memory copy of the snapshot.
  std::unique_ptr<SnapshotReader> BeginLoad(
      const SnapshotMetadata& metadata) override {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string path = file_snapshot_path(config_.storage_path,
                                          metadata.last_included_index,
                                          metadata.last_included_term);
    auto reader = std::make_unique<FileSnapshotReader>(path);
    if (!reader->IsValid()) {
      return nullptr;
    }
    return reader;
  }

  // @unsafe - Reads the latest snapshot into caller-owned output pointers.
  bool LoadLatestSnapshot(SnapshotMetadata* metadata_out,
                          std::string* data_out) override {
    std::lock_guard<std::mutex> lock(mutex_);
    auto latest = GetLatestSnapshotUnlocked();
    if (latest.is_none()) {
      return false;
    }

    auto meta = latest.unwrap();
    std::string path = file_snapshot_path(config_.storage_path,
                                          meta.last_included_index,
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
    return GetLatestSnapshotUnlocked();
  }

  // @unsafe (with mutex)
  std::vector<SnapshotMetadata> ListSnapshots() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return ListSnapshotsUnlocked();
  }

  // @unsafe (with mutex)
  bool HasSnapshotAtOrAfter(slotid_t min_index) const override {
    std::lock_guard<std::mutex> lock(mutex_);
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

  // @unsafe - Deletes snapshot files below keep_after_index.
  size_t PruneSnapshots(slotid_t keep_after_index) override {
    std::lock_guard<std::mutex> lock(mutex_);
    auto snapshots = ListSnapshotsUnlocked();
    size_t deleted = 0;

    for (const auto& snap : snapshots) {
      if (snap.last_included_index < keep_after_index) {
        std::string path = file_snapshot_path(config_.storage_path,
                                              snap.last_included_index,
                                              snap.last_included_term);
        if (unlink(path.c_str()) == 0) {
          Log_info("[SNAPSHOT-MGR] Pruned snapshot: %s", path.c_str());
          deleted++;
        }
      }
    }
    return deleted;
  }

  // @unsafe - Deletes all complete snapshot files known to the manager.
  size_t DeleteAllSnapshots() override {
    std::lock_guard<std::mutex> lock(mutex_);
    auto snapshots = ListSnapshotsUnlocked();
    size_t deleted = 0;

    for (const auto& snap : snapshots) {
      std::string path = file_snapshot_path(config_.storage_path,
                                            snap.last_included_index,
                                            snap.last_included_term);
      if (unlink(path.c_str()) == 0) {
        deleted++;
      }
    }
    Log_info("[SNAPSHOT-MGR] Deleted all %zu snapshots", deleted);
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
  // @unsafe - owned configuration. GetStoragePath returns a borrowed reference
  // to config_.storage_path; callers must not retain it past manager lifetime.
  SnapshotConfig config_;
  mutable std::mutex mutex_;

  // @unsafe - Creates storage_path if missing.
  bool EnsureDirectory() const {
    struct stat st;
    if (stat(config_.storage_path.c_str(), &st) == 0) {
      return S_ISDIR(st.st_mode);
    }
    return mkdir(config_.storage_path.c_str(), 0755) == 0;
  }

  // @unsafe - Directory operations (must hold mutex). DIR* is closed before
  // return; only complete .snap files are returned, not .tmp files.
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
        SnapshotMetadata meta{};
        meta.last_included_index = std::stoull(match[1].str());
        meta.last_included_term = std::stoull(match[2].str());

        // Get file size
        std::string path = file_snapshot_path(config_.storage_path,
                                              meta.last_included_index,
                                              meta.last_included_term);
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
    if (snapshots.empty()) {
      return rusty::None;
    }
    return rusty::Some(snapshots[0]);
  }

  // @unsafe - Deletes old complete snapshot files (must hold mutex).
  void ApplyRetentionPolicy() {
    auto snapshots = ListSnapshotsUnlocked();
    if (snapshots.size() <= config_.max_snapshots) {
      return;
    }

    // Delete oldest snapshots beyond retention limit
    for (size_t i = config_.max_snapshots; i < snapshots.size(); i++) {
      std::string path = file_snapshot_path(config_.storage_path,
                                            snapshots[i].last_included_index,
                                            snapshots[i].last_included_term);
      if (unlink(path.c_str()) == 0) {
        Log_info("[SNAPSHOT-MGR] Retention policy: deleted %s", path.c_str());
      }
    }
  }
};

}  // namespace raft
}  // namespace janus
