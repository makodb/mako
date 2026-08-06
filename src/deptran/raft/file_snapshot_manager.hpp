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
 * RustyCpp migration notes:
 * - Path parsing, scalar predicates, reader/writer cores, and manager state
 *   are DSL-owned.
 * - File descriptors, directory scans, unlink/rename/fsync, and raw buffer
 *   reads stay in C++ helpers.
 * - Public classes remain C++ virtual bridges so filesystem side effects and
 *   unique_ptr ownership are visible at the boundary.
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
// testconf.h has a legacy two-argument Init macro. Preserve it around the
// RustyCpp umbrella, whose array helpers use Init as an ordinary identifier.
#pragma push_macro("Init")
#undef Init
#include "rusty/rusty.hpp"
#pragma pop_macro("Init")

// Snapshot payloads are owned byte vectors.  Paths remain strings because
// they cross the POSIX filesystem boundary.
import rusty;

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
inline std::string file_snapshot_path(const std::string& storage_path, uint64_t index, uint64_t term);
inline std::string file_snapshot_temp_path(const std::string& storage_path, uint64_t index, uint64_t term);

inline std::string file_snapshot_path(const std::string& storage_path, uint64_t index, uint64_t term) {
    return ((((rusty::detail::deref_if_pointer_like(storage_path) + std::string("/snapshot_")) + std::to_string(std::move(index))) + std::string("_")) + std::to_string(std::move(term))) + std::string(".snap");
}

inline std::string file_snapshot_temp_path(const std::string& storage_path, uint64_t index, uint64_t term) {
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
inline size_t file_snapshot_advance_offset(size_t offset, size_t size);
inline size_t file_snapshot_reader_bytes_to_read(size_t data_size, size_t offset, size_t buffer_size);
inline bool file_snapshot_reader_is_complete(bool valid, size_t data_size, size_t offset);
inline bool file_snapshot_should_prune(uint64_t snapshot_index, uint64_t keep_after_index);
inline bool file_snapshot_has_latest(size_t snapshot_count);
inline size_t file_snapshot_retention_start(size_t snapshot_count, size_t max_snapshots);
inline bool file_snapshot_should_delete_for_retention(size_t position, size_t max_snapshots, size_t snapshot_count);

inline size_t file_snapshot_advance_offset(size_t offset, size_t size) {
    return rusty::detail::deref_if_pointer_like(offset) + rusty::detail::deref_if_pointer_like(size);
}

inline size_t file_snapshot_reader_bytes_to_read(size_t data_size, size_t offset, size_t buffer_size) {
    auto remaining = rusty::detail::deref_if_pointer_like(data_size) - rusty::detail::deref_if_pointer_like(offset);
    if (rusty::detail::deref_if_pointer_like(buffer_size) < rusty::detail::deref_if_pointer_like(remaining)) {
        return std::move(buffer_size);
    } else {
        return std::move(remaining);
    }
}

inline bool file_snapshot_reader_is_complete(bool valid, size_t data_size, size_t offset) {
    return rusty::detail::deref_if_pointer_like(valid) && (rusty::detail::deref_if_pointer_like(offset) >= rusty::detail::deref_if_pointer_like(data_size));
}

inline bool file_snapshot_should_prune(uint64_t snapshot_index, uint64_t keep_after_index) {
    return rusty::detail::deref_if_pointer_like(snapshot_index) < rusty::detail::deref_if_pointer_like(keep_after_index);
}

inline bool file_snapshot_has_latest(size_t snapshot_count) {
    return rusty::detail::deref_if_pointer_like(snapshot_count) > 0;
}

inline size_t file_snapshot_retention_start(size_t snapshot_count, size_t max_snapshots) {
    if (rusty::detail::deref_if_pointer_like(snapshot_count) > rusty::detail::deref_if_pointer_like(max_snapshots)) {
        return std::move(max_snapshots);
    } else {
        return std::move(snapshot_count);
    }
}

inline bool file_snapshot_should_delete_for_retention(size_t position, size_t max_snapshots, size_t snapshot_count) {
    return (rusty::detail::deref_if_pointer_like(snapshot_count) > rusty::detail::deref_if_pointer_like(max_snapshots)) && (rusty::detail::deref_if_pointer_like(position) >= rusty::detail::deref_if_pointer_like(max_snapshots));
}

inline SnapshotMetadata file_snapshot_metadata_from_name_parts(const std::string& index_part, const std::string& term_part, size_t size_bytes) {
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
inline bool file_snapshot_writer_write_cpp(rusty::Vec<uint8_t>* buffer,
                                           size_t* offset,
                                           bool finalized,
                                           bool aborted,
                                           const char* data,
                                           size_t size) {
  if (finalized || aborted) {
    Log_error("[SNAPSHOT-WRITER] Write after finalize/abort");
    return false;
  }
  for (size_t i = 0; i < size; ++i) buffer->push(static_cast<uint8_t>(data[i]));
  *offset = file_snapshot_advance_offset(*offset, size);
  return true;
}

// @unsafe - serializes, writes, fsyncs, and atomically renames the snapshot.
inline bool file_snapshot_writer_finalize_cpp(const std::string* final_path,
                                              const std::string* temp_path,
                                              slotid_t last_index,
                                              ballot_t last_term,
                                              rusty::Vec<uint8_t>* buffer,
                                              bool* finalized,
                                              bool aborted) {
  if (*finalized || aborted) {
    Log_error("[SNAPSHOT-WRITER] Finalize after finalize/abort");
    return false;
  }

  std::string serialized;
  if (!SnapshotFormat::Serialize(last_index, last_term,
                                 reinterpret_cast<const char*>(buffer->data()), buffer->size(),
                                 &serialized)) {
    Log_error("[SNAPSHOT-WRITER] Failed to serialize snapshot");
    return false;
  }

  int fd = open(temp_path->c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    Log_error("[SNAPSHOT-WRITER] Failed to open temp file: {} ({})",
              temp_path->c_str(), strerror(errno));
    return false;
  }

  ssize_t written = write(fd, serialized.data(), serialized.size());
  if (written != static_cast<ssize_t>(serialized.size())) {
    Log_error("[SNAPSHOT-WRITER] Failed to write snapshot: wrote {} of {}",
              written, serialized.size());
    close(fd);
    unlink(temp_path->c_str());
    return false;
  }

  if (fsync(fd) < 0) {
    Log_error("[SNAPSHOT-WRITER] Failed to fsync: {}", strerror(errno));
    close(fd);
    unlink(temp_path->c_str());
    return false;
  }
  close(fd);

  if (rename(temp_path->c_str(), final_path->c_str()) < 0) {
    Log_error("[SNAPSHOT-WRITER] Failed to rename {} -> {}: {}",
              temp_path->c_str(), final_path->c_str(), strerror(errno));
    unlink(temp_path->c_str());
    return false;
  }

  *finalized = true;
  Log_info("[SNAPSHOT-WRITER] Snapshot finalized: {} ({} bytes data, {} bytes total)",
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
                                          rusty::Vec<uint8_t>* file_data,
                                          rusty::Vec<uint8_t>* data,
                                          SnapshotMetadata* metadata,
                                          bool* valid) {
  int fd = open(path->c_str(), O_RDONLY);
  if (fd < 0) {
    Log_error("[SNAPSHOT-READER] Failed to open: {} ({})",
              path->c_str(), strerror(errno));
    *valid = false;
    return false;
  }

  struct stat st;
  if (fstat(fd, &st) < 0) {
    Log_error("[SNAPSHOT-READER] Failed to stat: {}", path->c_str());
    close(fd);
    *valid = false;
    return false;
  }

  std::string serialized(st.st_size, '\0');
  ssize_t bytes_read = read(fd, serialized.data(), st.st_size);
  close(fd);

  if (bytes_read != st.st_size) {
    Log_error("[SNAPSHOT-READER] Failed to read: got {} of {}",
              bytes_read, st.st_size);
    *valid = false;
    return false;
  }

  uint64_t last_index, last_term;
  std::string decoded;
  if (!SnapshotFormat::Deserialize(serialized.data(), serialized.size(),
                                   &last_index, &last_term, &decoded)) {
    Log_error("[SNAPSHOT-READER] Failed to deserialize: {}", path->c_str());
    *valid = false;
    return false;
  }

  metadata->last_included_index = last_index;
  metadata->last_included_term = last_term;
  file_data->clear();
  data->clear();
  for (unsigned char byte : serialized) file_data->push(byte);
  for (unsigned char byte : decoded) data->push(byte);
  metadata->size_bytes = data->size();

  SnapshotHeader header = snapshot_header_defaults();
  if (SnapshotFormat::GetHeader(serialized.data(), serialized.size(), &header)) {
    metadata->timestamp_ms = header.timestamp_ms;
  }

  *valid = true;
  Log_info("[SNAPSHOT-READER] Opened snapshot: index={} term={} size={}",
           last_index, last_term, data->size());
  return true;
}

// @unsafe - copies snapshot payload bytes into caller-owned raw output buffer.
inline bool file_snapshot_reader_read_cpp(const rusty::Vec<uint8_t>* data,
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
inline bool file_snapshot_reader_is_complete_cpp(const rusty::Vec<uint8_t>* data,
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
    buffer_: rusty::Vec<u8>,
}

impl FileSnapshotWriterCore {
    // @unsafe - Records final/temp paths. Finalize owns file I/O; cleanup is
    // driven by the C++ bridge destructor.
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
            buffer_: rusty::Vec::<u8>::new_(),
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
/*RUSTYCPP:GEN-BEGIN id=file_snapshot_manager.writer_core version=1 rust_sha256=04479d502c334424eb77105bae672e5af989efc53b512b7083d21335ab0aeb0a*/
struct FileSnapshotWriterCore;

struct FileSnapshotWriterCore {
    std::string final_path_;
    std::string temp_path_;
    uint64_t last_index_;
    uint64_t last_term_;
    size_t offset_;
    bool finalized_;
    bool aborted_;
    rusty::Vec<uint8_t> buffer_;

    static FileSnapshotWriterCore new_(std::string final_path, std::string temp_path, uint64_t last_index, uint64_t last_term);
    bool Cleanup() const;
    bool Write(const c_char* data, size_t size);
    bool Finalize();
    bool Abort();
    size_t GetOffset() const;
};


inline FileSnapshotWriterCore FileSnapshotWriterCore::new_(std::string final_path, std::string temp_path, uint64_t last_index, uint64_t last_term) {
    return FileSnapshotWriterCore{.final_path_ = std::move(final_path), .temp_path_ = std::move(temp_path), .last_index_ = std::move(last_index), .last_term_ = std::move(last_term), .offset_ = static_cast<size_t>(0), .finalized_ = false, .aborted_ = false, .buffer_ = rusty::Vec<uint8_t>::new_()};
}

inline bool FileSnapshotWriterCore::Cleanup() const {
    return file_snapshot_writer_cleanup_cpp(&this->temp_path_, this->finalized_, this->aborted_);
}

inline bool FileSnapshotWriterCore::Write(const c_char* data, size_t size) {
    // @unsafe
    {
        return file_snapshot_writer_write_cpp(&this->buffer_, &this->offset_, this->finalized_, this->aborted_, data, std::move(size));
    }
}

inline bool FileSnapshotWriterCore::Finalize() {
    return file_snapshot_writer_finalize_cpp(&this->final_path_, &this->temp_path_, this->last_index_, this->last_term_, &this->buffer_, &this->finalized_, this->aborted_);
}

inline bool FileSnapshotWriterCore::Abort() {
    return file_snapshot_writer_abort_cpp(&this->temp_path_, this->finalized_, &this->aborted_);
}

inline size_t FileSnapshotWriterCore::GetOffset() const {
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
      : core_(FileSnapshotWriterCore::new_(final_path, temp_path, last_index,
                                           last_term)) {
    Log_info("[SNAPSHOT-WRITER] Creating snapshot: index={} term={} path={}",
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
    file_data_: rusty::Vec<u8>,
    data_: rusty::Vec<u8>,
    metadata_: SnapshotMetadata,
    read_offset_: usize,
    valid_: bool,
}

impl FileSnapshotReaderCore {
    // @unsafe - Stores path, then Open() owns file I/O through a C++ helper.
    fn new(path: std::string) -> FileSnapshotReaderCore {
        FileSnapshotReaderCore {
            path_: path,
            file_data_: rusty::Vec::<u8>::new_(),
            data_: rusty::Vec::<u8>::new_(),
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
/*RUSTYCPP:GEN-BEGIN id=file_snapshot_manager.reader_core version=1 rust_sha256=1743a8867e8150be150a3851fad9e09cdcffa9b7483dda122321af21ff3b8084*/
struct FileSnapshotReaderCore;

struct FileSnapshotReaderCore {
    std::string path_;
    rusty::Vec<uint8_t> file_data_;
    rusty::Vec<uint8_t> data_;
    SnapshotMetadata metadata_;
    size_t read_offset_;
    bool valid_;

    static FileSnapshotReaderCore new_(std::string path);
    bool Open();
    bool Read(c_char* buffer, size_t buffer_size, size_t* bytes_read);
    bool IsComplete() const;
    const SnapshotMetadata& GetMetadata() const;
    size_t GetOffset() const;
    bool IsValid() const;
};


inline FileSnapshotReaderCore FileSnapshotReaderCore::new_(std::string path) {
    return FileSnapshotReaderCore{.path_ = std::move(path), .file_data_ = rusty::Vec<uint8_t>::new_(), .data_ = rusty::Vec<uint8_t>::new_(), .metadata_ = SnapshotMetadata{}, .read_offset_ = static_cast<size_t>(0), .valid_ = false};
}

inline bool FileSnapshotReaderCore::Open() {
    // @unsafe
    {
        return file_snapshot_reader_open_cpp(&this->path_, &this->file_data_, &this->data_, &this->metadata_, &this->valid_);
    }
}

inline bool FileSnapshotReaderCore::Read(c_char* buffer, size_t buffer_size, size_t* bytes_read) {
    // @unsafe
    {
        return file_snapshot_reader_read_cpp(&this->data_, &this->read_offset_, this->valid_, buffer, std::move(buffer_size), bytes_read);
    }
}

inline bool FileSnapshotReaderCore::IsComplete() const {
    return file_snapshot_reader_is_complete_cpp(&this->data_, this->valid_, this->read_offset_);
}

inline const SnapshotMetadata& FileSnapshotReaderCore::GetMetadata() const {
    // @unsafe
    {
        return file_snapshot_reader_metadata_cpp(&this->metadata_);
    }
}

inline size_t FileSnapshotReaderCore::GetOffset() const {
    return this->read_offset_;
}

inline bool FileSnapshotReaderCore::IsValid() const {
    return this->valid_;
}
/*RUSTYCPP:GEN-END id=file_snapshot_manager.reader_core*/

class FileSnapshotReader : public SnapshotReader {
 public:
  // @unsafe - Opens the snapshot path with a local fd, reads it fully, closes
  // the fd, then owns the decoded payload in core_.
  explicit FileSnapshotReader(const std::string& path)
      : core_(FileSnapshotReaderCore::new_(path)) {
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

// @unsafe - creates storage_path if missing.
inline bool file_snapshot_manager_ensure_directory_cpp(
    const SnapshotConfig* config) {
  struct stat st;
  if (stat(config->storage_path.c_str(), &st) == 0) {
    return S_ISDIR(st.st_mode);
  }
  return mkdir(config->storage_path.c_str(), 0755) == 0;
}

// @unsafe - Returns an owned writer for the configured snapshot path.
inline std::unique_ptr<SnapshotWriter> file_snapshot_manager_begin_snapshot_cpp(
    const SnapshotConfig* config, slotid_t last_index, ballot_t last_term) {
  std::string final_path = file_snapshot_path(config->storage_path,
                                              last_index, last_term);
  std::string temp_path = file_snapshot_temp_path(config->storage_path,
                                                  last_index, last_term);
  return std::make_unique<FileSnapshotWriter>(final_path, temp_path,
                                               last_index, last_term);
}

// @unsafe - Directory operations. DIR* is closed before return; only complete
// .snap files are returned, not .tmp files.
inline std::vector<SnapshotMetadata> file_snapshot_manager_list_cpp(
    const SnapshotConfig* config) {
  std::vector<SnapshotMetadata> result;

  DIR* dir = opendir(config->storage_path.c_str());
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

      std::string path = file_snapshot_path(config->storage_path,
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

  std::sort(result.begin(), result.end(),
            [](const SnapshotMetadata& a, const SnapshotMetadata& b) {
              return a.last_included_index > b.last_included_index;
            });

  return result;
}

// @unsafe - directory scan helper.
inline rusty::Option<SnapshotMetadata> file_snapshot_manager_latest_cpp(
    const SnapshotConfig* config) {
  auto snapshots = file_snapshot_manager_list_cpp(config);
  if (!file_snapshot_has_latest(snapshots.size())) {
    return rusty::None;
  }
  return rusty::Some(snapshots[0]);
}

// @unsafe - creates and verifies an owned file reader.
inline std::unique_ptr<SnapshotReader> file_snapshot_manager_begin_load_cpp(
    const SnapshotConfig* config, const SnapshotMetadata& metadata) {
  std::string path = file_snapshot_path(config->storage_path,
                                        metadata.last_included_index,
                                        metadata.last_included_term);
  auto reader = std::make_unique<FileSnapshotReader>(path);
  if (!reader->IsValid()) {
    return nullptr;
  }
  return reader;
}

// @unsafe - Reads the latest snapshot into caller-owned output pointers.
inline bool file_snapshot_manager_load_latest_cpp(
    const SnapshotConfig* config, SnapshotMetadata* metadata_out,
    std::string* data_out) {
  auto latest = file_snapshot_manager_latest_cpp(config);
  if (latest.is_none()) {
    return false;
  }

  auto meta = latest.unwrap();
  std::string path = file_snapshot_path(config->storage_path,
                                        meta.last_included_index,
                                        meta.last_included_term);
  FileSnapshotReader reader(path);
  if (!reader.IsValid()) {
    return false;
  }

  *metadata_out = reader.GetMetadata();

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

// @unsafe - directory scan helper.
inline bool file_snapshot_manager_has_at_or_after_cpp(
    const SnapshotConfig* config, slotid_t min_index) {
  auto snapshots = file_snapshot_manager_list_cpp(config);
  for (const auto& snap : snapshots) {
    if (snap.last_included_index >= min_index) {
      return true;
    }
  }
  return false;
}

// @unsafe - Deletes snapshot files below keep_after_index.
inline size_t file_snapshot_manager_prune_cpp(const SnapshotConfig* config,
                                              slotid_t keep_after_index) {
  auto snapshots = file_snapshot_manager_list_cpp(config);
  size_t deleted = 0;

  for (const auto& snap : snapshots) {
    if (file_snapshot_should_prune(snap.last_included_index, keep_after_index)) {
      std::string path = file_snapshot_path(config->storage_path,
                                            snap.last_included_index,
                                            snap.last_included_term);
      if (unlink(path.c_str()) == 0) {
        Log_info("[SNAPSHOT-MGR] Pruned snapshot: {}", path.c_str());
        deleted++;
      }
    }
  }
  return deleted;
}

// @unsafe - Deletes all complete snapshot files known to the manager.
inline size_t file_snapshot_manager_delete_all_cpp(
    const SnapshotConfig* config) {
  auto snapshots = file_snapshot_manager_list_cpp(config);
  size_t deleted = 0;

  for (const auto& snap : snapshots) {
    std::string path = file_snapshot_path(config->storage_path,
                                          snap.last_included_index,
                                          snap.last_included_term);
    if (unlink(path.c_str()) == 0) {
      deleted++;
    }
  }
  Log_info("[SNAPSHOT-MGR] Deleted all {} snapshots", deleted);
  return deleted;
}

// @unsafe - Deletes old complete snapshot files.
inline void file_snapshot_manager_apply_retention_cpp(
    const SnapshotConfig* config) {
  auto snapshots = file_snapshot_manager_list_cpp(config);
  size_t retention_start = file_snapshot_retention_start(snapshots.size(),
                                                         config->max_snapshots);
  if (!file_snapshot_has_latest(snapshots.size()) ||
      retention_start == snapshots.size()) {
    return;
  }

  for (size_t i = retention_start; i < snapshots.size(); i++) {
    if (!file_snapshot_should_delete_for_retention(i, config->max_snapshots,
                                                   snapshots.size())) {
      continue;
    }
    std::string path = file_snapshot_path(config->storage_path,
                                          snapshots[i].last_included_index,
                                          snapshots[i].last_included_term);
    if (unlink(path.c_str()) == 0) {
      Log_info("[SNAPSHOT-MGR] Retention policy: deleted {}", path.c_str());
    }
  }
}

// @unsafe - creates, writes, finalizes snapshot, then applies retention.
inline bool file_snapshot_manager_take_snapshot_cpp(
    const SnapshotConfig* config, slotid_t last_index, ballot_t last_term,
    const char* data, size_t size) {
  auto writer = file_snapshot_manager_begin_snapshot_cpp(config, last_index,
                                                         last_term);
  if (!writer) return false;
  if (!writer->Write(data, size)) return false;
  if (!writer->Finalize()) return false;

  file_snapshot_manager_apply_retention_cpp(config);
  return true;
}

// @lifetime: (&'a) -> &'a
inline const std::string& file_snapshot_manager_storage_path_cpp(
    const SnapshotConfig* config) {
  return config->storage_path;
}

// FileSnapshotManagerCore owns SnapshotConfig and delegates every filesystem
// operation to C++ helpers. The FileSnapshotManager bridge below supplies
// mutex locking and SnapshotManager virtual dispatch.
#if RUSTYCPP_RUST
pub struct FileSnapshotManagerCore {
    config_: SnapshotConfig,
}

impl FileSnapshotManagerCore {
    // @safe
    fn new(config: SnapshotConfig) -> FileSnapshotManagerCore {
        FileSnapshotManagerCore {
            config_: config,
        }
    }

    // @unsafe - May create the snapshot directory.
    fn EnsureDirectory(&self) -> bool {
        unsafe { file_snapshot_manager_ensure_directory_cpp(&self.config_) }
    }

    // @unsafe - Creates writer with side effects.
    fn BeginSnapshot(&self, last_index: u64, last_term: i64)
        -> std::unique_ptr<SnapshotWriter> {
        unsafe {
            file_snapshot_manager_begin_snapshot_cpp(&self.config_,
                                                     last_index,
                                                     last_term)
        }
    }

    // @unsafe - Creates, writes, finalizes, and applies retention.
    fn TakeSnapshot(&self, last_index: u64, last_term: i64,
                    data: *const c_char, size: usize) -> bool {
        unsafe {
            file_snapshot_manager_take_snapshot_cpp(&self.config_,
                                                   last_index,
                                                   last_term,
                                                   data,
                                                   size)
        }
    }

    // @unsafe - Creates reader with side effects.
    fn BeginLoad(&self, metadata: &SnapshotMetadata)
        -> std::unique_ptr<SnapshotReader> {
        unsafe {
            file_snapshot_manager_begin_load_cpp(&self.config_, metadata)
        }
    }

    // @unsafe - Writes to caller-owned output pointers.
    fn LoadLatestSnapshot(&self, metadata_out: *mut SnapshotMetadata,
                          data_out: *mut std::string) -> bool {
        unsafe {
            file_snapshot_manager_load_latest_cpp(&self.config_,
                                                 metadata_out,
                                                 data_out)
        }
    }

    // @unsafe - Directory scan.
    fn GetLatestSnapshot(&self) -> rusty::Option<SnapshotMetadata> {
        unsafe { file_snapshot_manager_latest_cpp(&self.config_) }
    }

    // @unsafe - Directory scan.
    fn ListSnapshots(&self) -> std::vector<SnapshotMetadata> {
        unsafe { file_snapshot_manager_list_cpp(&self.config_) }
    }

    // @unsafe - Directory scan.
    fn HasSnapshotAtOrAfter(&self, min_index: u64) -> bool {
        unsafe { file_snapshot_manager_has_at_or_after_cpp(&self.config_, min_index) }
    }

    // @unsafe - Deletes files.
    fn PruneSnapshots(&self, keep_after_index: u64) -> usize {
        unsafe { file_snapshot_manager_prune_cpp(&self.config_, keep_after_index) }
    }

    // @unsafe - Deletes files.
    fn DeleteAllSnapshots(&self) -> usize {
        unsafe { file_snapshot_manager_delete_all_cpp(&self.config_) }
    }

    // @lifetime: (&'a) -> &'a
    fn GetStoragePath(&self) -> &std::string {
        unsafe { file_snapshot_manager_storage_path_cpp(&self.config_) }
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=file_snapshot_manager.manager_core version=1 rust_sha256=9ff9b3ac65deb432abe7938d8e6a1139a067b7c6d71f2ebd55d5db5012e085fa*/
struct FileSnapshotManagerCore;

struct FileSnapshotManagerCore {
    SnapshotConfig config_;

    static FileSnapshotManagerCore new_(SnapshotConfig config);
    bool EnsureDirectory() const;
    std::unique_ptr<SnapshotWriter> BeginSnapshot(uint64_t last_index, int64_t last_term) const;
    bool TakeSnapshot(uint64_t last_index, int64_t last_term, const c_char* data, size_t size) const;
    std::unique_ptr<SnapshotReader> BeginLoad(const SnapshotMetadata& metadata) const;
    bool LoadLatestSnapshot(SnapshotMetadata* metadata_out, std::string* data_out) const;
    rusty::Option<SnapshotMetadata> GetLatestSnapshot() const;
    std::vector<SnapshotMetadata> ListSnapshots() const;
    bool HasSnapshotAtOrAfter(uint64_t min_index) const;
    size_t PruneSnapshots(uint64_t keep_after_index) const;
    size_t DeleteAllSnapshots() const;
    const std::string& GetStoragePath() const;
};


inline FileSnapshotManagerCore FileSnapshotManagerCore::new_(SnapshotConfig config) {
    return FileSnapshotManagerCore{.config_ = std::move(config)};
}

inline bool FileSnapshotManagerCore::EnsureDirectory() const {
    // @unsafe
    {
        return file_snapshot_manager_ensure_directory_cpp(&this->config_);
    }
}

inline std::unique_ptr<SnapshotWriter> FileSnapshotManagerCore::BeginSnapshot(uint64_t last_index, int64_t last_term) const {
    // @unsafe
    {
        return file_snapshot_manager_begin_snapshot_cpp(&this->config_, std::move(last_index), std::move(last_term));
    }
}

inline bool FileSnapshotManagerCore::TakeSnapshot(uint64_t last_index, int64_t last_term, const c_char* data, size_t size) const {
    // @unsafe
    {
        return file_snapshot_manager_take_snapshot_cpp(&this->config_, std::move(last_index), std::move(last_term), data, std::move(size));
    }
}

inline std::unique_ptr<SnapshotReader> FileSnapshotManagerCore::BeginLoad(const SnapshotMetadata& metadata) const {
    // @unsafe
    {
        return file_snapshot_manager_begin_load_cpp(&this->config_, metadata);
    }
}

inline bool FileSnapshotManagerCore::LoadLatestSnapshot(SnapshotMetadata* metadata_out, std::string* data_out) const {
    // @unsafe
    {
        return file_snapshot_manager_load_latest_cpp(&this->config_, metadata_out, data_out);
    }
}

inline rusty::Option<SnapshotMetadata> FileSnapshotManagerCore::GetLatestSnapshot() const {
    // @unsafe
    {
        return file_snapshot_manager_latest_cpp(&this->config_);
    }
}

inline std::vector<SnapshotMetadata> FileSnapshotManagerCore::ListSnapshots() const {
    // @unsafe
    {
        return file_snapshot_manager_list_cpp(&this->config_);
    }
}

inline bool FileSnapshotManagerCore::HasSnapshotAtOrAfter(uint64_t min_index) const {
    // @unsafe
    {
        return file_snapshot_manager_has_at_or_after_cpp(&this->config_, std::move(min_index));
    }
}

inline size_t FileSnapshotManagerCore::PruneSnapshots(uint64_t keep_after_index) const {
    // @unsafe
    {
        return file_snapshot_manager_prune_cpp(&this->config_, std::move(keep_after_index));
    }
}

inline size_t FileSnapshotManagerCore::DeleteAllSnapshots() const {
    // @unsafe
    {
        return file_snapshot_manager_delete_all_cpp(&this->config_);
    }
}

inline const std::string& FileSnapshotManagerCore::GetStoragePath() const {
    // @unsafe
    {
        return file_snapshot_manager_storage_path_cpp(&this->config_);
    }
}
/*RUSTYCPP:GEN-END id=file_snapshot_manager.manager_core*/

/**
 * File-based snapshot manager implementation.
 * Stores snapshots in a directory with automatic retention policy.
 */
class FileSnapshotManager : public SnapshotManager {
 public:
  // @unsafe - May create the snapshot directory; config_ owns the path string.
  explicit FileSnapshotManager(const SnapshotConfig& config)
      : core_(FileSnapshotManagerCore::new_(config)) {
    core_.EnsureDirectory();
    Log_info("[SNAPSHOT-MGR] Initialized: path={} max_snapshots={}",
             core_.GetStoragePath().c_str(), config.max_snapshots);
  }

  ~FileSnapshotManager() override = default;

  // ========================================================================
  // Snapshot Creation
  // ========================================================================

  // @unsafe - Returns an owned writer. The writer owns path strings and later
  // owns any temp-file cleanup for this snapshot.
  std::unique_ptr<SnapshotWriter> BeginSnapshot(
      uint64_t last_index, int64_t last_term) override {
    std::lock_guard<std::mutex> lock(mutex_);
    return core_.BeginSnapshot(last_index, last_term);
  }

  // @unsafe - Creates and finalizes snapshot
  bool TakeSnapshot(uint64_t last_index, int64_t last_term,
                    const char* data, size_t size) override {
    return core_.TakeSnapshot(last_index, last_term, data, size);
  }

  // ========================================================================
  // Snapshot Loading
  // ========================================================================

  // @unsafe - Returns an owned reader with an in-memory copy of the snapshot.
  std::unique_ptr<SnapshotReader> BeginLoad(
      const SnapshotMetadata& metadata) override {
    std::lock_guard<std::mutex> lock(mutex_);
    return core_.BeginLoad(metadata);
  }

  // @unsafe - Reads the latest snapshot into caller-owned output pointers.
  bool LoadLatestSnapshot(SnapshotMetadata* metadata_out,
                          std::string* data_out) override {
    std::lock_guard<std::mutex> lock(mutex_);
    return core_.LoadLatestSnapshot(metadata_out, data_out);
  }

  // ========================================================================
  // Snapshot Queries
  // ========================================================================

  // @unsafe (with mutex)
  rusty::Option<SnapshotMetadata> GetLatestSnapshot() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return core_.GetLatestSnapshot();
  }

  // @unsafe (with mutex)
  std::vector<SnapshotMetadata> ListSnapshots() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return core_.ListSnapshots();
  }

  // @unsafe (with mutex)
  bool HasSnapshotAtOrAfter(slotid_t min_index) const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return core_.HasSnapshotAtOrAfter(min_index);
  }

  // ========================================================================
  // Snapshot Cleanup
  // ========================================================================

  // @unsafe - Deletes snapshot files below keep_after_index.
  size_t PruneSnapshots(slotid_t keep_after_index) override {
    std::lock_guard<std::mutex> lock(mutex_);
    return core_.PruneSnapshots(keep_after_index);
  }

  // @unsafe - Deletes all complete snapshot files known to the manager.
  size_t DeleteAllSnapshots() override {
    std::lock_guard<std::mutex> lock(mutex_);
    return core_.DeleteAllSnapshots();
  }

  // ========================================================================
  // Configuration
  // ========================================================================

  // @lifetime: (&'a) -> &'a
  const std::string& GetStoragePath() const override {
    return core_.GetStoragePath();
  }

 private:
  mutable std::mutex mutex_;
  FileSnapshotManagerCore core_;
};

}  // namespace raft
}  // namespace janus
