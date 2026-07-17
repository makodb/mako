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
#include <utility>
#include <vector>

#include "snapshot_format.hpp"
#include "snapshot_manager.hpp"
#include "rusty/slice.hpp"

namespace janus {
namespace raft {

// @safe - deterministic path builders from copied values. Directory creation,
// file opening, rename/delete, and fs error handling stay in C++.
#if RUSTYCPP_RUST
pub fn file_snapshot_path(storage_path: &std::string,
                          index: u64,
                          term: u64) -> std::string {
    storage_path + std::string("/snapshot_") + std::to_string(index) +
        std::string("_") + std::to_string(term) + std::string(".snap")
}

pub fn file_snapshot_temp_path(storage_path: &std::string,
                               index: u64,
                               term: u64) -> std::string {
    storage_path + std::string("/snapshot_") + std::to_string(index) +
        std::string("_") + std::to_string(term) + std::string(".snap.tmp")
}
#endif
/*RUSTYCPP:GEN-BEGIN id=file_snapshot_manager.paths version=1 rust_sha256=3de54b7f5a6178dee5430ee423f133d5a2bdc7fcc82a29f5aab899834c8c75ae*/
std::string file_snapshot_path(const std::string& storage_path, uint64_t index, uint64_t term);
std::string file_snapshot_temp_path(const std::string& storage_path, uint64_t index, uint64_t term);

std::string file_snapshot_path(const std::string& storage_path, uint64_t index, uint64_t term) {
    return ((((rusty::detail::deref_if_pointer_like(storage_path) + std::string("/snapshot_")) + std::to_string(std::move(index))) + std::string("_")) + std::to_string(std::move(term))) + std::string(".snap");
}

std::string file_snapshot_temp_path(const std::string& storage_path, uint64_t index, uint64_t term) {
    return ((((rusty::detail::deref_if_pointer_like(storage_path) + std::string("/snapshot_")) + std::to_string(std::move(index))) + std::string("_")) + std::to_string(std::move(term))) + std::string(".snap.tmp");
}
/*RUSTYCPP:GEN-END id=file_snapshot_manager.paths*/

inline SnapshotMetadata file_snapshot_metadata_from_name_parts_cpp(
    const std::string& index_part,
    const std::string& term_part,
    size_t size_bytes) {
  SnapshotMetadata meta{};
  meta.last_included_index = std::stoull(index_part);
  meta.last_included_term = std::stoull(term_part);
  meta.size_bytes = size_bytes;
  return meta;
}

// @safe boundary split - reader offset/retention predicates are pure scalar
// logic. std::ifstream/ofstream state, directory scanning, regex parsing, and
// deletion remain hand-C++.
#if RUSTYCPP_RUST
pub fn file_snapshot_advance_offset(offset: usize, size: usize) -> usize {
    offset + size
}

pub fn file_snapshot_reader_bytes_to_read(data_size: usize,
                                          offset: usize,
                                          buffer_size: usize) -> usize {
    let remaining = data_size - offset;
    if buffer_size < remaining {
        buffer_size
    } else {
        remaining
    }
}

pub fn file_snapshot_reader_is_complete(valid: bool,
                                        data_size: usize,
                                        offset: usize) -> bool {
    valid && offset >= data_size
}

pub fn file_snapshot_should_prune(snapshot_index: u64,
                                  keep_after_index: u64) -> bool {
    snapshot_index < keep_after_index
}

pub fn file_snapshot_has_latest(snapshot_count: usize) -> bool {
    snapshot_count > 0
}

pub fn file_snapshot_retention_start(snapshot_count: usize,
                                     max_snapshots: usize) -> usize {
    if snapshot_count > max_snapshots {
        max_snapshots
    } else {
        snapshot_count
    }
}

pub fn file_snapshot_should_delete_for_retention(position: usize,
                                                 max_snapshots: usize,
                                                 snapshot_count: usize) -> bool {
    snapshot_count > max_snapshots && position >= max_snapshots
}

pub fn file_snapshot_metadata_from_name_parts(index_part: &std::string,
                                              term_part: &std::string,
                                              size_bytes: usize) -> SnapshotMetadata {
    file_snapshot_metadata_from_name_parts_cpp(index_part, term_part, size_bytes)
}
#endif
/*RUSTYCPP:GEN-BEGIN id=file_snapshot_manager.stream_helpers version=1 rust_sha256=2b8291cb63e1f96746a07c22b18c93e1cfb7425b6e4e9e797486be9c3d5f5e3f*/
size_t file_snapshot_advance_offset(size_t offset, size_t size);
size_t file_snapshot_reader_bytes_to_read(size_t data_size, size_t offset, size_t buffer_size);
bool file_snapshot_reader_is_complete(bool valid, size_t data_size, size_t offset);
bool file_snapshot_should_prune(uint64_t snapshot_index, uint64_t keep_after_index);
bool file_snapshot_has_latest(size_t snapshot_count);
size_t file_snapshot_retention_start(size_t snapshot_count, size_t max_snapshots);
bool file_snapshot_should_delete_for_retention(size_t position, size_t max_snapshots, size_t snapshot_count);

size_t file_snapshot_advance_offset(size_t offset, size_t size) {
    return rusty::detail::deref_if_pointer_like(offset) + rusty::detail::deref_if_pointer_like(size);
}

size_t file_snapshot_reader_bytes_to_read(size_t data_size, size_t offset, size_t buffer_size) {
    auto remaining = rusty::detail::deref_if_pointer_like(data_size) - rusty::detail::deref_if_pointer_like(offset);
    if (rusty::detail::deref_if_pointer_like(buffer_size) < rusty::detail::deref_if_pointer_like(remaining)) {
        return std::move(buffer_size);
    } else {
        return std::move(remaining);
    }
}

bool file_snapshot_reader_is_complete(bool valid, size_t data_size, size_t offset) {
    return rusty::detail::deref_if_pointer_like(valid) && (rusty::detail::deref_if_pointer_like(offset) >= rusty::detail::deref_if_pointer_like(data_size));
}

bool file_snapshot_should_prune(uint64_t snapshot_index, uint64_t keep_after_index) {
    return rusty::detail::deref_if_pointer_like(snapshot_index) < rusty::detail::deref_if_pointer_like(keep_after_index);
}

bool file_snapshot_has_latest(size_t snapshot_count) {
    return rusty::detail::deref_if_pointer_like(snapshot_count) > 0;
}

size_t file_snapshot_retention_start(size_t snapshot_count, size_t max_snapshots) {
    if (rusty::detail::deref_if_pointer_like(snapshot_count) > rusty::detail::deref_if_pointer_like(max_snapshots)) {
        return std::move(max_snapshots);
    } else {
        return std::move(snapshot_count);
    }
}

bool file_snapshot_should_delete_for_retention(size_t position, size_t max_snapshots, size_t snapshot_count) {
    return (rusty::detail::deref_if_pointer_like(snapshot_count) > rusty::detail::deref_if_pointer_like(max_snapshots)) && (rusty::detail::deref_if_pointer_like(position) >= rusty::detail::deref_if_pointer_like(max_snapshots));
}

SnapshotMetadata file_snapshot_metadata_from_name_parts(const std::string& index_part, const std::string& term_part, size_t size_bytes) {
    return file_snapshot_metadata_from_name_parts_cpp(index_part, term_part, std::move(size_bytes));
}
/*RUSTYCPP:GEN-END id=file_snapshot_manager.stream_helpers*/

// @unsafe - best-effort cleanup of an unfinished temp file.
inline bool file_snapshot_writer_cleanup_cpp(const std::string* temp_path,
                                             bool finalized,
                                             bool aborted) {
  if (!finalized && !aborted) {
    unlink(temp_path->c_str());
  }
  return true;
}

// @unsafe - raw pointer append into a caller-owned staging buffer.
inline bool file_snapshot_writer_write_cpp(std::string* buffer,
                                           size_t* offset,
                                           bool finalized,
                                           bool aborted,
                                           const char* data,
                                           size_t size) {
  if (finalized || aborted) {
    Log_error("[SNAPSHOT-WRITER] Write after finalize/abort");
    return false;
  }
  buffer->append(data, size);
  *offset = file_snapshot_advance_offset(*offset, size);
  return true;
}

// @unsafe - serializes, writes, fsyncs, and atomically renames the snapshot.
inline bool file_snapshot_writer_finalize_cpp(const std::string* final_path,
                                              const std::string* temp_path,
                                              slotid_t last_index,
                                              ballot_t last_term,
                                              std::string* buffer,
                                              bool* finalized,
                                              bool aborted) {
  if (*finalized || aborted) {
    Log_error("[SNAPSHOT-WRITER] Finalize after finalize/abort");
    return false;
  }

  std::string serialized;
  if (!SnapshotFormat::Serialize(last_index, last_term,
                                 buffer->data(), buffer->size(),
                                 &serialized)) {
    Log_error("[SNAPSHOT-WRITER] Failed to serialize snapshot");
    return false;
  }

  int fd = open(temp_path->c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    Log_error("[SNAPSHOT-WRITER] Failed to open temp file: %s (%s)",
              temp_path->c_str(), strerror(errno));
    return false;
  }

  ssize_t written = write(fd, serialized.data(), serialized.size());
  if (written != static_cast<ssize_t>(serialized.size())) {
    Log_error("[SNAPSHOT-WRITER] Failed to write snapshot: wrote %zd of %zu",
              written, serialized.size());
    close(fd);
    unlink(temp_path->c_str());
    return false;
  }

  if (fsync(fd) < 0) {
    Log_error("[SNAPSHOT-WRITER] Failed to fsync: %s", strerror(errno));
    close(fd);
    unlink(temp_path->c_str());
    return false;
  }
  close(fd);

  if (rename(temp_path->c_str(), final_path->c_str()) < 0) {
    Log_error("[SNAPSHOT-WRITER] Failed to rename %s -> %s: %s",
              temp_path->c_str(), final_path->c_str(), strerror(errno));
    unlink(temp_path->c_str());
    return false;
  }

  *finalized = true;
  Log_info("[SNAPSHOT-WRITER] Snapshot finalized: %s (%zu bytes data, %zu bytes total)",
           final_path->c_str(), buffer->size(), serialized.size());
  return true;
}

// @unsafe - deletes the temp file if this writer has not finalized.
inline bool file_snapshot_writer_abort_cpp(const std::string* temp_path,
                                           bool finalized,
                                           bool* aborted) {
  if (finalized || *aborted) {
    return true;
  }
  *aborted = true;
  unlink(temp_path->c_str());
  Log_info("[SNAPSHOT-WRITER] Snapshot aborted");
  return true;
}

// @unsafe - opens, stats, reads, deserializes, and verifies a snapshot file.
inline bool file_snapshot_reader_open_cpp(const std::string* path,
                                          std::string* file_data,
                                          std::string* data,
                                          SnapshotMetadata* metadata,
                                          bool* valid) {
  int fd = open(path->c_str(), O_RDONLY);
  if (fd < 0) {
    Log_error("[SNAPSHOT-READER] Failed to open: %s (%s)",
              path->c_str(), strerror(errno));
    *valid = false;
    return false;
  }

  struct stat st;
  if (fstat(fd, &st) < 0) {
    Log_error("[SNAPSHOT-READER] Failed to stat: %s", path->c_str());
    close(fd);
    *valid = false;
    return false;
  }

  file_data->resize(st.st_size);
  ssize_t bytes_read = read(fd, file_data->data(), st.st_size);
  close(fd);

  if (bytes_read != st.st_size) {
    Log_error("[SNAPSHOT-READER] Failed to read: got %zd of %ld",
              bytes_read, st.st_size);
    *valid = false;
    return false;
  }

  uint64_t last_index, last_term;
  if (!SnapshotFormat::Deserialize(file_data->data(), file_data->size(),
                                   &last_index, &last_term, data)) {
    Log_error("[SNAPSHOT-READER] Failed to deserialize: %s", path->c_str());
    *valid = false;
    return false;
  }

  metadata->last_included_index = last_index;
  metadata->last_included_term = last_term;
  metadata->size_bytes = data->size();

  SnapshotHeader header = snapshot_header_defaults();
  if (SnapshotFormat::GetHeader(file_data->data(), file_data->size(), &header)) {
    metadata->timestamp_ms = header.timestamp_ms;
  }

  *valid = true;
  Log_info("[SNAPSHOT-READER] Opened snapshot: index=%lu term=%lu size=%zu",
           last_index, last_term, data->size());
  return true;
}

// @unsafe - copies snapshot payload bytes into caller-owned raw output buffer.
inline bool file_snapshot_reader_read_cpp(const std::string* data,
                                          size_t* read_offset,
                                          bool valid,
                                          char* buffer,
                                          size_t buffer_size,
                                          size_t* bytes_read) {
  if (!valid) {
    *bytes_read = 0;
    return false;
  }

  size_t to_read = file_snapshot_reader_bytes_to_read(data->size(),
                                                      *read_offset,
                                                      buffer_size);
  if (to_read > 0) {
    std::memcpy(buffer, data->data() + *read_offset, to_read);
    *read_offset = file_snapshot_advance_offset(*read_offset, to_read);
  }
  *bytes_read = to_read;
  return true;
}

// @safe - checks reader-local stream position.
inline bool file_snapshot_reader_is_complete_cpp(const std::string* data,
                                                 bool valid,
                                                 size_t read_offset) {
  return file_snapshot_reader_is_complete(valid, data->size(), read_offset);
}

// @lifetime: (&'a) -> &'a
inline const SnapshotMetadata& file_snapshot_reader_metadata_cpp(
    const SnapshotMetadata* metadata) {
  return *metadata;
}

/**
 * File-based snapshot writer.
 * Accumulates data in memory, writes to temp file, renames on finalize.
 */
// FileSnapshotWriterCore is the DSL-owned writer state and method surface.
// FileSnapshotWriter remains a tiny C++ hand-bridge for the virtual
// SnapshotWriter interface and destructor-driven cleanup.
#if RUSTYCPP_RUST
pub struct FileSnapshotWriterCore {
    final_path_: std::string,
    temp_path_: std::string,
    last_index_: u64,
    last_term_: u64,
    offset_: usize,
    finalized_: bool,
    aborted_: bool,
    buffer_: std::string,
}

impl FileSnapshotWriterCore {
    // @unsafe - Records final/temp paths. Finalize owns file I/O; cleanup is
    // driven by the C++ bridge destructor.
    #[cpp_ctor]
    fn new(final_path: std::string,
           temp_path: std::string,
           last_index: u64,
           last_term: u64) -> FileSnapshotWriterCore {
        FileSnapshotWriterCore {
            final_path_: final_path,
            temp_path_: temp_path,
            last_index_: last_index,
            last_term_: last_term,
            offset_: 0usize,
            finalized_: false,
            aborted_: false,
            buffer_: std::string(),
        }
    }

    // @unsafe - best-effort cleanup of an unfinished temp file.
    fn Cleanup(&self) -> bool {
        file_snapshot_writer_cleanup_cpp(&self.temp_path_,
                                         self.finalized_,
                                         self.aborted_)
    }

    // @unsafe - Reads from raw pointer into the staging buffer.
    fn Write(&mut self, data: *const c_char, size: usize) -> bool {
        unsafe {
            file_snapshot_writer_write_cpp(&mut self.buffer_,
                                           &mut self.offset_,
                                           self.finalized_,
                                           self.aborted_,
                                           data,
                                           size)
        }
    }

    // @unsafe - serializes, writes, fsyncs, and renames the snapshot.
    fn Finalize(&mut self) -> bool {
        file_snapshot_writer_finalize_cpp(&self.final_path_,
                                          &self.temp_path_,
                                          self.last_index_,
                                          self.last_term_,
                                          &mut self.buffer_,
                                          &mut self.finalized_,
                                          self.aborted_)
    }

    // @unsafe - deletes the temp file if this writer has not finalized.
    fn Abort(&mut self) -> bool {
        file_snapshot_writer_abort_cpp(&self.temp_path_,
                                       self.finalized_,
                                       &mut self.aborted_)
    }

    // @safe - Returns current offset.
    fn GetOffset(&self) -> usize {
        self.offset_
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=file_snapshot_manager.writer_core version=1 rust_sha256=1eb1cf455fc0b378947811175d55dc01352ac7436678cf05cdddf62252278c01*/
struct FileSnapshotWriterCore;

struct FileSnapshotWriterCore {
    std::string final_path_;
    std::string temp_path_;
    uint64_t last_index_;
    uint64_t last_term_;
    size_t offset_;
    bool finalized_;
    bool aborted_;
    std::string buffer_;

    FileSnapshotWriterCore(std::string final_path, std::string temp_path, uint64_t last_index, uint64_t last_term);
    bool Cleanup() const;
    bool Write(const c_char* data, size_t size);
    bool Finalize();
    bool Abort();
    size_t GetOffset() const;
};


FileSnapshotWriterCore::FileSnapshotWriterCore(std::string final_path, std::string temp_path, uint64_t last_index, uint64_t last_term)
    : final_path_(final_path)
    , temp_path_(temp_path)
    , last_index_(last_index)
    , last_term_(last_term)
    , offset_(static_cast<size_t>(0))
    , finalized_(false)
    , aborted_(false)
    , buffer_(std::string())
{}

bool FileSnapshotWriterCore::Cleanup() const {
    return file_snapshot_writer_cleanup_cpp(&this->temp_path_, this->finalized_, this->aborted_);
}

bool FileSnapshotWriterCore::Write(const c_char* data, size_t size) {
    // @unsafe
    {
        return file_snapshot_writer_write_cpp(&this->buffer_, &this->offset_, this->finalized_, this->aborted_, data, std::move(size));
    }
}

bool FileSnapshotWriterCore::Finalize() {
    return file_snapshot_writer_finalize_cpp(&this->final_path_, &this->temp_path_, this->last_index_, this->last_term_, &this->buffer_, &this->finalized_, this->aborted_);
}

bool FileSnapshotWriterCore::Abort() {
    return file_snapshot_writer_abort_cpp(&this->temp_path_, this->finalized_, &this->aborted_);
}

size_t FileSnapshotWriterCore::GetOffset() const {
    return this->offset_;
}
/*RUSTYCPP:GEN-END id=file_snapshot_manager.writer_core*/

class FileSnapshotWriter : public SnapshotWriter {
 public:
  // @unsafe - Records final/temp paths. Finalize owns the temp-file write and
  // rename; Abort/destructor clean up an unfinished temp file.
  FileSnapshotWriter(const std::string& final_path,
                     const std::string& temp_path,
                     slotid_t last_index,
                     ballot_t last_term)
      : core_(final_path, temp_path, last_index, last_term) {
    Log_info("[SNAPSHOT-WRITER] Creating snapshot: index=%lu term=%lu path=%s",
             last_index, last_term, final_path.c_str());
  }

  // @unsafe - Best-effort cleanup of an unfinished temp file.
  ~FileSnapshotWriter() override {
    core_.Cleanup();
  }

  // @unsafe - Reads from raw pointer
  bool Write(const char* data, size_t size) override {
    return core_.Write(data, size);
  }

  // @unsafe - File I/O operations.
  bool Finalize() override {
    return core_.Finalize();
  }

  // @unsafe - Deletes the temp file if this writer has not finalized.
  bool Abort() override {
    return core_.Abort();
  }

  // @safe - Returns current offset
  size_t GetOffset() const override { return core_.GetOffset(); }

 private:
  FileSnapshotWriterCore core_;
};

/**
 * File-based snapshot reader.
 * Reads and verifies snapshot on construction, provides streaming read.
 */
// FileSnapshotReaderCore is the DSL-owned reader state and method surface.
// FileSnapshotReader remains the C++ virtual bridge for SnapshotReader.
#if RUSTYCPP_RUST
pub struct FileSnapshotReaderCore {
    path_: std::string,
    file_data_: std::string,
    data_: std::string,
    metadata_: SnapshotMetadata,
    read_offset_: usize,
    valid_: bool,
}

impl FileSnapshotReaderCore {
    // @unsafe - Stores path, then Open() owns file I/O through a C++ helper.
    #[cpp_ctor]
    fn new(path: std::string) -> FileSnapshotReaderCore {
        FileSnapshotReaderCore {
            path_: path,
            file_data_: std::string(),
            data_: std::string(),
            metadata_: SnapshotMetadata {},
            read_offset_: 0usize,
            valid_: false,
        }
    }

    // @unsafe - opens, reads, deserializes, and verifies the file.
    fn Open(&mut self) -> bool {
        unsafe {
            file_snapshot_reader_open_cpp(&self.path_,
                                          &mut self.file_data_,
                                          &mut self.data_,
                                          &mut self.metadata_,
                                          &mut self.valid_)
        }
    }

    // @unsafe - Writes to raw buffer.
    fn Read(&mut self, buffer: *mut c_char, buffer_size: usize,
            bytes_read: *mut usize) -> bool {
        unsafe {
            file_snapshot_reader_read_cpp(&self.data_,
                                          &mut self.read_offset_,
                                          self.valid_,
                                          buffer,
                                          buffer_size,
                                          bytes_read)
        }
    }

    // @safe
    fn IsComplete(&self) -> bool {
        file_snapshot_reader_is_complete_cpp(&self.data_,
                                             self.valid_,
                                             self.read_offset_)
    }

    // @lifetime: (&'a) -> &'a
    fn GetMetadata(&self) -> &SnapshotMetadata {
        unsafe { file_snapshot_reader_metadata_cpp(&self.metadata_) }
    }

    // @safe - Returns current offset.
    fn GetOffset(&self) -> usize {
        self.read_offset_
    }

    // @safe - Check if reader is valid.
    fn IsValid(&self) -> bool {
        self.valid_
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=file_snapshot_manager.reader_core version=1 rust_sha256=309ea541afaae6d7bfc437e89938e041575f6792aa4bf446e0bd315306f5bea0*/
struct FileSnapshotReaderCore;

struct FileSnapshotReaderCore {
    std::string path_;
    std::string file_data_;
    std::string data_;
    SnapshotMetadata metadata_;
    size_t read_offset_;
    bool valid_;

    FileSnapshotReaderCore(std::string path);
    bool Open();
    bool Read(c_char* buffer, size_t buffer_size, size_t* bytes_read);
    bool IsComplete() const;
    const SnapshotMetadata& GetMetadata() const;
    size_t GetOffset() const;
    bool IsValid() const;
};


FileSnapshotReaderCore::FileSnapshotReaderCore(std::string path)
    : path_(path)
    , file_data_(std::string())
    , data_(std::string())
    , metadata_(SnapshotMetadata{})
    , read_offset_(static_cast<size_t>(0))
    , valid_(false)
{}

bool FileSnapshotReaderCore::Open() {
    // @unsafe
    {
        return file_snapshot_reader_open_cpp(&this->path_, &this->file_data_, &this->data_, &this->metadata_, &this->valid_);
    }
}

bool FileSnapshotReaderCore::Read(c_char* buffer, size_t buffer_size, size_t* bytes_read) {
    // @unsafe
    {
        return file_snapshot_reader_read_cpp(&this->data_, &this->read_offset_, this->valid_, buffer, std::move(buffer_size), bytes_read);
    }
}

bool FileSnapshotReaderCore::IsComplete() const {
    return file_snapshot_reader_is_complete_cpp(&this->data_, this->valid_, this->read_offset_);
}

const SnapshotMetadata& FileSnapshotReaderCore::GetMetadata() const {
    // @unsafe
    {
        return file_snapshot_reader_metadata_cpp(&this->metadata_);
    }
}

size_t FileSnapshotReaderCore::GetOffset() const {
    return this->read_offset_;
}

bool FileSnapshotReaderCore::IsValid() const {
    return this->valid_;
}
/*RUSTYCPP:GEN-END id=file_snapshot_manager.reader_core*/

class FileSnapshotReader : public SnapshotReader {
 public:
  // @unsafe - Opens the snapshot path with a local fd, reads it fully, closes
  // the fd, then owns the decoded payload in core_.
  explicit FileSnapshotReader(const std::string& path) : core_(path) {
    core_.Open();
  }

  ~FileSnapshotReader() override = default;

  // @unsafe - Writes to raw buffer
  bool Read(char* buffer, size_t buffer_size, size_t* bytes_read) override {
    return core_.Read(buffer, buffer_size, bytes_read);
  }

  // @safe
  bool IsComplete() const override {
    return core_.IsComplete();
  }

  // @lifetime: (&'a) -> &'a
  const SnapshotMetadata& GetMetadata() const override {
    return core_.GetMetadata();
  }

  // @safe - Returns current offset
  size_t GetOffset() const override { return core_.GetOffset(); }

  // @safe - Check if reader is valid
  bool IsValid() const { return core_.IsValid(); }

 private:
  FileSnapshotReaderCore core_;
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
      if (file_snapshot_should_prune(snap.last_included_index, keep_after_index)) {
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
        std::string index_part = match[1].str();
        std::string term_part = match[2].str();
        SnapshotMetadata meta = file_snapshot_metadata_from_name_parts(
            index_part, term_part, 0);

        // Get file size
        std::string path = file_snapshot_path(config_.storage_path,
                                              meta.last_included_index,
                                              meta.last_included_term);
        struct stat st;
        if (stat(path.c_str(), &st) == 0) {
          meta = file_snapshot_metadata_from_name_parts(index_part,
                                                        term_part,
                                                        st.st_size);
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

  // @unsafe - Deletes old complete snapshot files (must hold mutex).
  void ApplyRetentionPolicy() {
    auto snapshots = ListSnapshotsUnlocked();
    size_t retention_start = file_snapshot_retention_start(snapshots.size(),
                                                           config_.max_snapshots);
    if (!file_snapshot_has_latest(snapshots.size()) ||
        retention_start == snapshots.size()) {
      return;
    }

    // Delete oldest snapshots beyond retention limit
    for (size_t i = retention_start; i < snapshots.size(); i++) {
      if (!file_snapshot_should_delete_for_retention(i, config_.max_snapshots,
                                                     snapshots.size())) {
        continue;
      }
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
