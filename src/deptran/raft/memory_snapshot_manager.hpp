#pragma once

/**
 * In-memory SnapshotManager implementation. Parallels MemoryLogStorage
 * (memory_log_storage.hpp) and is intended for tests that want to
 * exercise the snapshot path without touching disk.
 *
 * Deliberately kept minimal: a single latest snapshot is retained.
 * BeginSnapshot / BeginLoad return trivial writers/readers that
 * accumulate and serve the payload in-memory. This covers everything
 * the lab-style correctness tests exercise; production still uses
 * FileSnapshotManager.
 *
 * RustyCpp migration notes:
 * - Writer, reader, and manager state live in DSL-owned Core structs.
 * - The public classes remain C++ virtual bridges for SnapshotWriter,
 *   SnapshotReader, and SnapshotManager.
 * - Mutex locking stays in the manager bridge; Core methods expect the bridge
 *   to provide synchronization and lifetime discipline.
 */

#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <rusty/move.hpp>
#include <rusty/rusty.hpp>

// `rusty::Vec` is exported by RustyCpp's named umbrella module.  The
// handwritten headers above provide its supporting declarations (including
// the generated scalar helpers); the import provides the Vec alias itself.
import rusty;

#include "snapshot_manager.hpp"

namespace janus {
namespace raft {

// @safe - builds metadata from copied scalar fields. Snapshot payload storage,
// reader/writer lifetime, and manager mutation remain in the C++ classes.
#if RUSTYCPP_RUST
pub fn memory_snapshot_metadata(last_index: u64,
                                last_term: u64,
                                size: usize) -> SnapshotMetadata {
    SnapshotMetadata {
        last_included_index: last_index,
        last_included_term: last_term,
        timestamp_ms: 0,
        size_bytes: size,
        checksum: std::string(),
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=memory_snapshot_manager.metadata version=1 rust_sha256=3678b8c2aca40b44613126d24c67e673f791cd4fe51ca6b49a74a69ae31d1b57*/
SnapshotMetadata memory_snapshot_metadata(uint64_t last_index, uint64_t last_term, size_t size) {
    return SnapshotMetadata{.last_included_index = std::move(last_index), .last_included_term = std::move(last_term), .timestamp_ms = 0, .size_bytes = std::move(size), .checksum = std::string()};
}
/*RUSTYCPP:GEN-END id=memory_snapshot_manager.metadata*/

// @safe - stream-position arithmetic over copied sizes. Buffer copies and
// bounds checks against caller-provided memory stay in the reader methods.
#if RUSTYCPP_RUST
pub fn memory_snapshot_advance_offset(offset: usize, size: usize) -> usize {
    offset + size
}

pub fn memory_snapshot_reader_bytes_to_read(payload_size: usize,
                                            offset: usize,
                                            buffer_size: usize) -> usize {
    let remaining = payload_size - offset;
    if buffer_size < remaining {
        buffer_size
    } else {
        remaining
    }
}

pub fn memory_snapshot_reader_is_complete(payload_size: usize,
                                          offset: usize) -> bool {
    offset >= payload_size
}
#endif
/*RUSTYCPP:GEN-BEGIN id=memory_snapshot_manager.stream_helpers version=1 rust_sha256=d98f5868cc5d5a7f49cde4ce1883e770ee25ece0c944988a8a8e95fc364028f1*/
size_t memory_snapshot_advance_offset(size_t offset, size_t size);
size_t memory_snapshot_reader_bytes_to_read(size_t payload_size, size_t offset, size_t buffer_size);
bool memory_snapshot_reader_is_complete(size_t payload_size, size_t offset);

size_t memory_snapshot_advance_offset(size_t offset, size_t size) {
    return rusty::detail::deref_if_pointer_like(offset) + rusty::detail::deref_if_pointer_like(size);
}

size_t memory_snapshot_reader_bytes_to_read(size_t payload_size, size_t offset, size_t buffer_size) {
    auto remaining = rusty::detail::deref_if_pointer_like(payload_size) - rusty::detail::deref_if_pointer_like(offset);
    if (rusty::detail::deref_if_pointer_like(buffer_size) < rusty::detail::deref_if_pointer_like(remaining)) {
        return std::move(buffer_size);
    } else {
        return std::move(remaining);
    }
}

bool memory_snapshot_reader_is_complete(size_t payload_size, size_t offset) {
    return rusty::detail::deref_if_pointer_like(offset) >= rusty::detail::deref_if_pointer_like(payload_size);
}
/*RUSTYCPP:GEN-END id=memory_snapshot_manager.stream_helpers*/

// The SnapshotWriter virtual API is the sole raw-byte boundary. Snapshot
// payload ownership itself is always a Rusty Vec<u8>, never a std::string.
// @unsafe - copies caller-owned bytes into an owned Rusty byte vector.
inline bool memory_snapshot_copy_from_raw(rusty::Vec<uint8_t>* buffer,
                                             size_t* offset,
                                             const char* data,
                                             size_t size) {
  for (size_t i = 0; i < size; ++i) {
    buffer->push(static_cast<uint8_t>(data[i]));
  }
  *offset = memory_snapshot_advance_offset(*offset, size);
  return true;
}

// @unsafe - publishes a completed in-memory snapshot under the manager mutex.
inline bool memory_snapshot_writer_finalize_cpp(rusty::Vec<uint8_t>* buffer,
                                                rusty::Vec<uint8_t>* dest_payload,
                                                SnapshotMetadata* dest_meta,
                                                bool* has_snapshot,
                                                std::mutex* mtx,
                                                slotid_t last_index,
                                                ballot_t last_term,
                                                bool* finalized) {
  std::lock_guard<std::mutex> lk(*mtx);
  *dest_payload = rusty::move(*buffer);
  *dest_meta = memory_snapshot_metadata(last_index, last_term,
                                        dest_payload->size());
  *has_snapshot = true;
  *finalized = true;
  return true;
}

// @safe - resets writer-local staging state.
inline bool memory_snapshot_writer_abort_cpp(rusty::Vec<uint8_t>* buffer,
                                             size_t* offset,
                                             bool* finalized) {
  buffer->clear();
  *offset = 0;
  *finalized = false;
  return true;
}

// @unsafe - copies snapshot payload bytes into caller-owned raw output buffer.
inline bool memory_snapshot_copy_to_raw(const rusty::Vec<uint8_t>* payload,
                                            size_t* offset,
                                            char* buffer,
                                            size_t buffer_size,
                                            size_t* bytes_read) {
  size_t n = memory_snapshot_reader_bytes_to_read(payload->size(),
                                                  *offset,
                                                  buffer_size);
  std::memcpy(buffer, payload->data() + *offset, n);
  *offset = memory_snapshot_advance_offset(*offset, n);
  *bytes_read = n;
  return true;
}

// @safe - checks reader-local stream position against owned payload size.
inline bool memory_snapshot_reader_is_complete_cpp(const rusty::Vec<uint8_t>* payload,
                                                   size_t offset) {
  return memory_snapshot_reader_is_complete(payload->size(), offset);
}

// @lifetime: (&'a) -> &'a
inline const SnapshotMetadata& memory_snapshot_reader_metadata_cpp(
    const SnapshotMetadata* metadata) {
  return *metadata;
}

// MemorySnapshotWriterCore is the DSL-owned state and behavior for the memory
// writer. MemorySnapshotWriter remains a tiny C++ hand-bridge because the
// transpiler cannot currently attach #[cpp_inherit] to a trait defined in the
// included snapshot_manager.hpp file.
#if RUSTYCPP_RUST
pub struct MemorySnapshotWriterCore {
    dest_payload_: *mut rusty::Vec<u8>,
    dest_meta_: *mut SnapshotMetadata,
    has_snapshot_: *mut bool,
    mtx_: *mut std::mutex,
    buffer_: rusty::Vec<u8>,
    offset_: usize,
    last_index_: u64,
    last_term_: u64,
    finalized_: bool,
}

impl MemorySnapshotWriterCore {
    // @unsafe - borrows MemorySnapshotManager internals. The writer must not
    // outlive the manager that created it.
    fn new(dest_payload: *mut rusty::Vec<u8>,
           dest_meta: *mut SnapshotMetadata,
           has_snapshot: *mut bool,
           mtx: *mut std::mutex,
           last_index: u64,
           last_term: u64) -> MemorySnapshotWriterCore {
        MemorySnapshotWriterCore {
            dest_payload_: dest_payload,
            dest_meta_: dest_meta,
            has_snapshot_: has_snapshot,
            mtx_: mtx,
            buffer_: rusty::Vec::<u8>::new_(),
            offset_: 0usize,
            last_index_: last_index,
            last_term_: last_term,
            finalized_: false,
        }
    }

    // @unsafe - raw pointer append to internal buffer.
    fn Write(&mut self, data: *const c_char, size: usize) -> bool {
        unsafe {
            memory_snapshot_copy_from_raw(&mut self.buffer_,
                                             &mut self.offset_,
                                             data,
                                             size)
        }
    }

    // @unsafe - publishes buffer under mutex.
    fn Finalize(&mut self) -> bool {
        unsafe {
            memory_snapshot_writer_finalize_cpp(&mut self.buffer_,
                                                self.dest_payload_,
                                                self.dest_meta_,
                                                self.has_snapshot_,
                                                self.mtx_,
                                                self.last_index_,
                                                self.last_term_,
                                                &mut self.finalized_)
        }
    }

    // @safe
    fn Abort(&mut self) -> bool {
        unsafe {
            memory_snapshot_writer_abort_cpp(&mut self.buffer_,
                                             &mut self.offset_,
                                             &mut self.finalized_)
        }
    }

    // @safe
    fn GetOffset(&self) -> usize {
        self.offset_
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=memory_snapshot_manager.writer_core version=1 rust_sha256=d12bdf978ae0472f0437c4d825e81b681f119eb98b9953c3c3ff61582e44ccdd*/
struct MemorySnapshotWriterCore;

struct MemorySnapshotWriterCore {
    rusty::Vec<uint8_t>* dest_payload_;
    SnapshotMetadata* dest_meta_;
    bool* has_snapshot_;
    std::mutex* mtx_;
    rusty::Vec<uint8_t> buffer_;
    size_t offset_;
    uint64_t last_index_;
    uint64_t last_term_;
    bool finalized_;

    static MemorySnapshotWriterCore new_(rusty::Vec<uint8_t>* dest_payload, SnapshotMetadata* dest_meta, bool* has_snapshot, std::mutex* mtx, uint64_t last_index, uint64_t last_term);
    bool Write(const c_char* data, size_t size);
    bool Finalize();
    bool Abort();
    size_t GetOffset() const;
};


MemorySnapshotWriterCore MemorySnapshotWriterCore::new_(rusty::Vec<uint8_t>* dest_payload, SnapshotMetadata* dest_meta, bool* has_snapshot, std::mutex* mtx, uint64_t last_index, uint64_t last_term) {
    return MemorySnapshotWriterCore{.dest_payload_ = dest_payload, .dest_meta_ = dest_meta, .has_snapshot_ = has_snapshot, .mtx_ = mtx, .buffer_ = rusty::Vec<uint8_t>::new_(), .offset_ = static_cast<size_t>(0), .last_index_ = std::move(last_index), .last_term_ = std::move(last_term), .finalized_ = false};
}

bool MemorySnapshotWriterCore::Write(const c_char* data, size_t size) {
    // @unsafe
    {
        return memory_snapshot_copy_from_raw(&this->buffer_, &this->offset_, data, std::move(size));
    }
}

bool MemorySnapshotWriterCore::Finalize() {
    // @unsafe
    {
        return memory_snapshot_writer_finalize_cpp(&this->buffer_, this->dest_payload_, this->dest_meta_, this->has_snapshot_, this->mtx_, this->last_index_, this->last_term_, &this->finalized_);
    }
}

bool MemorySnapshotWriterCore::Abort() {
    // @unsafe
    {
        return memory_snapshot_writer_abort_cpp(&this->buffer_, &this->offset_, &this->finalized_);
    }
}

size_t MemorySnapshotWriterCore::GetOffset() const {
    return this->offset_;
}
/*RUSTYCPP:GEN-END id=memory_snapshot_manager.writer_core*/

class MemorySnapshotWriter : public SnapshotWriter {
 public:
  // @unsafe - borrows MemorySnapshotManager internals via core_.
  MemorySnapshotWriter(rusty::Vec<uint8_t>* dest_payload,
                       SnapshotMetadata* dest_meta,
                       bool* has_snapshot,
                       std::mutex* mtx,
                       slotid_t last_index,
                       ballot_t last_term)
      : core_(MemorySnapshotWriterCore::new_(
          dest_payload, dest_meta, has_snapshot, mtx, last_index, last_term)) {}

  // @unsafe - raw pointer append to internal buffer
  bool Write(const char* data, size_t size) override {
    return core_.Write(data, size);
  }

  // @unsafe - publishes buffer under mutex
  bool Finalize() override {
    return core_.Finalize();
  }

  // @safe
  bool Abort() override {
    return core_.Abort();
  }

  // @safe
  size_t GetOffset() const override { return core_.GetOffset(); }

 private:
  MemorySnapshotWriterCore core_;
};

// MemorySnapshotReaderCore is the DSL-owned state and behavior for the memory
// reader. MemorySnapshotReader remains a C++ hand-bridge for the virtual
// SnapshotReader interface.
#if RUSTYCPP_RUST
pub struct MemorySnapshotReaderCore {
    payload_: rusty::Vec<u8>,
    meta_: SnapshotMetadata,
    offset_: usize,
}

impl MemorySnapshotReaderCore {
    // @safe
    fn new(payload: rusty::Vec<u8>, meta: SnapshotMetadata) -> MemorySnapshotReaderCore {
        MemorySnapshotReaderCore {
            payload_: payload,
            meta_: meta,
            offset_: 0usize,
        }
    }

    // @unsafe - copies owned bytes across the SnapshotReader raw boundary.
    fn Read(&mut self, buffer: *mut c_char, buffer_size: usize,
            bytes_read: *mut usize) -> bool {
        unsafe {
            memory_snapshot_copy_to_raw(&self.payload_,
                                            &mut self.offset_,
                                            buffer,
                                            buffer_size,
                                            bytes_read)
        }
    }

    // @safe
    fn IsComplete(&self) -> bool {
        memory_snapshot_reader_is_complete_cpp(&self.payload_, self.offset_)
    }

    // @lifetime: (&'a) -> &'a
    fn GetMetadata(&self) -> &SnapshotMetadata {
        unsafe { memory_snapshot_reader_metadata_cpp(&self.meta_) }
    }

    // @safe
    fn GetOffset(&self) -> usize {
        self.offset_
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=memory_snapshot_manager.reader_core version=1 rust_sha256=0c0291d7f09cfd39482cb4336b01334d891e89f9f7220975bd13210e4ff1eb44*/
struct MemorySnapshotReaderCore;

struct MemorySnapshotReaderCore {
    rusty::Vec<uint8_t> payload_;
    SnapshotMetadata meta_;
    size_t offset_;

    static MemorySnapshotReaderCore new_(rusty::Vec<uint8_t> payload, SnapshotMetadata meta);
    bool Read(c_char* buffer, size_t buffer_size, size_t* bytes_read);
    bool IsComplete() const;
    const SnapshotMetadata& GetMetadata() const;
    size_t GetOffset() const;
};


MemorySnapshotReaderCore MemorySnapshotReaderCore::new_(rusty::Vec<uint8_t> payload, SnapshotMetadata meta) {
    return MemorySnapshotReaderCore{.payload_ = std::move(payload), .meta_ = std::move(meta), .offset_ = static_cast<size_t>(0)};
}

bool MemorySnapshotReaderCore::Read(c_char* buffer, size_t buffer_size, size_t* bytes_read) {
    // @unsafe
    {
        return memory_snapshot_copy_to_raw(&this->payload_, &this->offset_, buffer, std::move(buffer_size), bytes_read);
    }
}

bool MemorySnapshotReaderCore::IsComplete() const {
    return memory_snapshot_reader_is_complete_cpp(&this->payload_, this->offset_);
}

const SnapshotMetadata& MemorySnapshotReaderCore::GetMetadata() const {
    // @unsafe
    {
        return memory_snapshot_reader_metadata_cpp(&this->meta_);
    }
}

size_t MemorySnapshotReaderCore::GetOffset() const {
    return this->offset_;
}
/*RUSTYCPP:GEN-END id=memory_snapshot_manager.reader_core*/

class MemorySnapshotReader : public SnapshotReader {
 public:
  // @safe
  MemorySnapshotReader(rusty::Vec<uint8_t> payload, SnapshotMetadata meta)
      : core_(MemorySnapshotReaderCore::new_(rusty::move(payload), rusty::move(meta))) {}

  // @unsafe - SnapshotReader's virtual interface exposes caller-owned bytes.
  bool Read(char* buffer, size_t buffer_size, size_t* bytes_read) override {
    return core_.Read(buffer, buffer_size, bytes_read);
  }

  // @safe
  bool IsComplete() const override {
    return core_.IsComplete();
  }

  // @safe
  const SnapshotMetadata& GetMetadata() const override {
    return core_.GetMetadata();
  }

  // @safe
  size_t GetOffset() const override { return core_.GetOffset(); }

 private:
  MemorySnapshotReaderCore core_;
};

// @unsafe - returns a writer borrowing manager-owned state.
inline std::unique_ptr<SnapshotWriter> memory_snapshot_manager_begin_snapshot_cpp(
    rusty::Vec<uint8_t>* payload, SnapshotMetadata* meta, bool* has_snapshot,
    std::mutex* mtx, slotid_t last_index, ballot_t last_term) {
  return std::make_unique<MemorySnapshotWriter>(
      payload, meta, has_snapshot, mtx, last_index, last_term);
}

// @unsafe - copies caller-owned bytes into manager state.
inline bool memory_snapshot_manager_take_snapshot_cpp(
    rusty::Vec<uint8_t>* payload, SnapshotMetadata* meta, bool* has_snapshot,
    slotid_t last_index, ballot_t last_term, const char* data, size_t size) {
  payload->clear();
  for (size_t i = 0; i < size; ++i) {
    payload->push(static_cast<uint8_t>(data[i]));
  }
  *meta = memory_snapshot_metadata(last_index, last_term, size);
  *has_snapshot = true;
  return true;
}

// @safe - returns a reader owning copies of the in-memory snapshot state.
inline std::unique_ptr<SnapshotReader> memory_snapshot_manager_begin_load_cpp(
    const rusty::Vec<uint8_t>* payload, const SnapshotMetadata* meta,
    bool has_snapshot) {
  if (!has_snapshot) return nullptr;
  return std::make_unique<MemorySnapshotReader>(payload->clone(), *meta);
}

// @unsafe - writes latest snapshot into caller-owned output pointers.
inline bool memory_snapshot_manager_load_latest_cpp(
    const rusty::Vec<uint8_t>* payload, const SnapshotMetadata* meta, bool has_snapshot,
    SnapshotMetadata* metadata_out, std::string* data_out) {
  if (!has_snapshot) return false;
  if (metadata_out) *metadata_out = *meta;
  if (data_out) data_out->assign(reinterpret_cast<const char*>(payload->data()),
                                 payload->size());
  return true;
}

// @safe
inline rusty::Option<SnapshotMetadata> memory_snapshot_manager_latest_cpp(
    const SnapshotMetadata* meta, bool has_snapshot) {
  if (!has_snapshot) return rusty::None;
  return rusty::Some(*meta);
}

// @safe
inline std::vector<SnapshotMetadata> memory_snapshot_manager_list_cpp(
    const SnapshotMetadata* meta, bool has_snapshot) {
  if (!has_snapshot) return {};
  return {*meta};
}

// @safe
inline bool memory_snapshot_manager_has_at_or_after_cpp(
    const SnapshotMetadata* meta, bool has_snapshot, slotid_t min_index) {
  return has_snapshot && meta->last_included_index >= min_index;
}

// @safe - single-snapshot manager prunes the current snapshot if it is old.
inline size_t memory_snapshot_manager_prune_cpp(
    rusty::Vec<uint8_t>* payload, SnapshotMetadata* meta, bool* has_snapshot,
    slotid_t keep_after_index) {
  if (*has_snapshot && meta->last_included_index < keep_after_index) {
    *has_snapshot = false;
    payload->clear();
    *meta = SnapshotMetadata{};
    return 1;
  }
  return 0;
}

// @safe
inline size_t memory_snapshot_manager_delete_all_cpp(
    rusty::Vec<uint8_t>* payload, SnapshotMetadata* meta, bool* has_snapshot) {
  size_t n = *has_snapshot ? 1 : 0;
  *has_snapshot = false;
  payload->clear();
  *meta = SnapshotMetadata{};
  return n;
}

// @lifetime: (&'a) -> &'a
inline const std::string& memory_snapshot_manager_storage_path_cpp(
    const std::string* storage_path) {
  return *storage_path;
}

// MemorySnapshotManagerCore owns the single in-memory snapshot payload and
// manager query/update behavior. The C++ bridge below keeps mutex locking and
// SnapshotManager virtual dispatch outside the DSL-owned state.
#if RUSTYCPP_RUST
pub struct MemorySnapshotManagerCore {
    has_snapshot_: bool,
    meta_: SnapshotMetadata,
    payload_: rusty::Vec<u8>,
    storage_path_: std::string,
}

impl MemorySnapshotManagerCore {
    // @safe
    fn new() -> MemorySnapshotManagerCore {
        MemorySnapshotManagerCore {
            has_snapshot_: false,
            meta_: SnapshotMetadata {},
            payload_: rusty::Vec::<u8>::new_(),
            storage_path_: std::string("<memory>"),
        }
    }

    // @unsafe - returns a writer borrowing manager-owned state.
    fn BeginSnapshot(&mut self, mtx: *mut std::mutex, last_index: u64,
                     last_term: i64) -> std::unique_ptr<SnapshotWriter> {
        unsafe {
            memory_snapshot_manager_begin_snapshot_cpp(&mut self.payload_,
                                                       &mut self.meta_,
                                                       &mut self.has_snapshot_,
                                                       mtx,
                                                       last_index,
                                                       last_term)
        }
    }

    // @unsafe - reads from raw pointer.
    fn TakeSnapshot(&mut self, last_index: u64, last_term: i64,
                    data: *const c_char, size: usize) -> bool {
        unsafe {
            memory_snapshot_manager_take_snapshot_cpp(&mut self.payload_,
                                                     &mut self.meta_,
                                                     &mut self.has_snapshot_,
                                                     last_index,
                                                     last_term,
                                                     data,
                                                     size)
        }
    }

    // @safe - reader owns copied state.
    fn BeginLoad(&self, _metadata: &SnapshotMetadata)
        -> std::unique_ptr<SnapshotReader> {
        memory_snapshot_manager_begin_load_cpp(&self.payload_,
                                               &self.meta_,
                                               self.has_snapshot_)
    }

    // @unsafe - writes to caller-owned output pointers.
    fn LoadLatestSnapshot(&self, metadata_out: *mut SnapshotMetadata,
                          data_out: *mut std::string) -> bool {
        unsafe {
            memory_snapshot_manager_load_latest_cpp(&self.payload_,
                                                   &self.meta_,
                                                   self.has_snapshot_,
                                                   metadata_out,
                                                   data_out)
        }
    }

    // @safe
    fn GetLatestSnapshot(&self) -> rusty::Option<SnapshotMetadata> {
        memory_snapshot_manager_latest_cpp(&self.meta_, self.has_snapshot_)
    }

    // @safe
    fn ListSnapshots(&self) -> std::vector<SnapshotMetadata> {
        memory_snapshot_manager_list_cpp(&self.meta_, self.has_snapshot_)
    }

    // @safe
    fn HasSnapshotAtOrAfter(&self, min_index: u64) -> bool {
        memory_snapshot_manager_has_at_or_after_cpp(&self.meta_,
                                                   self.has_snapshot_,
                                                   min_index)
    }

    // @safe
    fn PruneSnapshots(&mut self, keep_after_index: u64) -> usize {
        memory_snapshot_manager_prune_cpp(&mut self.payload_,
                                          &mut self.meta_,
                                          &mut self.has_snapshot_,
                                          keep_after_index)
    }

    // @safe
    fn DeleteAllSnapshots(&mut self) -> usize {
        memory_snapshot_manager_delete_all_cpp(&mut self.payload_,
                                               &mut self.meta_,
                                               &mut self.has_snapshot_)
    }

    // @lifetime: (&'a) -> &'a
    fn GetStoragePath(&self) -> &std::string {
        unsafe { memory_snapshot_manager_storage_path_cpp(&self.storage_path_) }
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=memory_snapshot_manager.manager_core version=1 rust_sha256=818cd6a71bcf1b6ac7e4cfa0c228f8c5698f456100ba224d3e8557e89b88af28*/
struct MemorySnapshotManagerCore;

struct MemorySnapshotManagerCore {
    bool has_snapshot_;
    SnapshotMetadata meta_;
    rusty::Vec<uint8_t> payload_;
    std::string storage_path_;

    static MemorySnapshotManagerCore new_();
    std::unique_ptr<SnapshotWriter> BeginSnapshot(std::mutex* mtx, uint64_t last_index, int64_t last_term);
    bool TakeSnapshot(uint64_t last_index, int64_t last_term, const c_char* data, size_t size);
    std::unique_ptr<SnapshotReader> BeginLoad(const SnapshotMetadata& _metadata) const;
    bool LoadLatestSnapshot(SnapshotMetadata* metadata_out, std::string* data_out) const;
    rusty::Option<SnapshotMetadata> GetLatestSnapshot() const;
    std::vector<SnapshotMetadata> ListSnapshots() const;
    bool HasSnapshotAtOrAfter(uint64_t min_index) const;
    size_t PruneSnapshots(uint64_t keep_after_index);
    size_t DeleteAllSnapshots();
    const std::string& GetStoragePath() const;
};


MemorySnapshotManagerCore MemorySnapshotManagerCore::new_() {
    return MemorySnapshotManagerCore{.has_snapshot_ = false, .meta_ = SnapshotMetadata{}, .payload_ = rusty::Vec<uint8_t>::new_(), .storage_path_ = std::string("<memory>")};
}

std::unique_ptr<SnapshotWriter> MemorySnapshotManagerCore::BeginSnapshot(std::mutex* mtx, uint64_t last_index, int64_t last_term) {
    // @unsafe
    {
        return memory_snapshot_manager_begin_snapshot_cpp(&this->payload_, &this->meta_, &this->has_snapshot_, mtx, std::move(last_index), std::move(last_term));
    }
}

bool MemorySnapshotManagerCore::TakeSnapshot(uint64_t last_index, int64_t last_term, const c_char* data, size_t size) {
    // @unsafe
    {
        return memory_snapshot_manager_take_snapshot_cpp(&this->payload_, &this->meta_, &this->has_snapshot_, std::move(last_index), std::move(last_term), data, std::move(size));
    }
}

std::unique_ptr<SnapshotReader> MemorySnapshotManagerCore::BeginLoad(const SnapshotMetadata& _metadata) const {
    return memory_snapshot_manager_begin_load_cpp(&this->payload_, &this->meta_, this->has_snapshot_);
}

bool MemorySnapshotManagerCore::LoadLatestSnapshot(SnapshotMetadata* metadata_out, std::string* data_out) const {
    // @unsafe
    {
        return memory_snapshot_manager_load_latest_cpp(&this->payload_, &this->meta_, this->has_snapshot_, metadata_out, data_out);
    }
}

rusty::Option<SnapshotMetadata> MemorySnapshotManagerCore::GetLatestSnapshot() const {
    return memory_snapshot_manager_latest_cpp(&this->meta_, this->has_snapshot_);
}

std::vector<SnapshotMetadata> MemorySnapshotManagerCore::ListSnapshots() const {
    return memory_snapshot_manager_list_cpp(&this->meta_, this->has_snapshot_);
}

bool MemorySnapshotManagerCore::HasSnapshotAtOrAfter(uint64_t min_index) const {
    return memory_snapshot_manager_has_at_or_after_cpp(&this->meta_, this->has_snapshot_, std::move(min_index));
}

size_t MemorySnapshotManagerCore::PruneSnapshots(uint64_t keep_after_index) {
    return memory_snapshot_manager_prune_cpp(&this->payload_, &this->meta_, &this->has_snapshot_, std::move(keep_after_index));
}

size_t MemorySnapshotManagerCore::DeleteAllSnapshots() {
    return memory_snapshot_manager_delete_all_cpp(&this->payload_, &this->meta_, &this->has_snapshot_);
}

const std::string& MemorySnapshotManagerCore::GetStoragePath() const {
    // @unsafe
    {
        return memory_snapshot_manager_storage_path_cpp(&this->storage_path_);
    }
}
/*RUSTYCPP:GEN-END id=memory_snapshot_manager.manager_core*/

class MemorySnapshotManager : public SnapshotManager {
 public:
  // @safe
  MemorySnapshotManager()
      : core_(MemorySnapshotManagerCore::new_()) {}

  // @unsafe - returned writer owns itself but borrows this manager's payload,
  // metadata, flag, and mutex. Do not let the writer outlive the manager.
  std::unique_ptr<SnapshotWriter> BeginSnapshot(
      uint64_t last_index, int64_t last_term) override {
    return core_.BeginSnapshot(&mtx_, last_index, last_term);
  }

  // @unsafe - memcpy into internal buffer under mutex
  bool TakeSnapshot(uint64_t last_index, int64_t last_term,
                    const char* data, size_t size) override {
    std::lock_guard<std::mutex> lk(mtx_);
    return core_.TakeSnapshot(last_index, last_term, data, size);
  }

  // @safe - returned reader owns a copy of the in-memory snapshot payload.
  std::unique_ptr<SnapshotReader> BeginLoad(
      const SnapshotMetadata& metadata) override {
    std::lock_guard<std::mutex> lk(mtx_);
    return core_.BeginLoad(metadata);
  }

  // @unsafe - writes to caller-owned pointers under mutex
  bool LoadLatestSnapshot(SnapshotMetadata* metadata_out,
                          std::string* data_out) override {
    std::lock_guard<std::mutex> lk(mtx_);
    return core_.LoadLatestSnapshot(metadata_out, data_out);
  }

  // @safe
  rusty::Option<SnapshotMetadata> GetLatestSnapshot() const override {
    std::lock_guard<std::mutex> lk(mtx_);
    return core_.GetLatestSnapshot();
  }

  // @safe
  std::vector<SnapshotMetadata> ListSnapshots() const override {
    std::lock_guard<std::mutex> lk(mtx_);
    return core_.ListSnapshots();
  }

  // @safe
  bool HasSnapshotAtOrAfter(slotid_t min_index) const override {
    std::lock_guard<std::mutex> lk(mtx_);
    return core_.HasSnapshotAtOrAfter(min_index);
  }

  // @safe - single-snapshot manager never prunes below current
  size_t PruneSnapshots(slotid_t keep_after_index) override {
    std::lock_guard<std::mutex> lk(mtx_);
    return core_.PruneSnapshots(keep_after_index);
  }

  // @safe
  size_t DeleteAllSnapshots() override {
    std::lock_guard<std::mutex> lk(mtx_);
    return core_.DeleteAllSnapshots();
  }

  // @lifetime: (&'a) -> &'a
  const std::string& GetStoragePath() const override {
    return core_.GetStoragePath();
  }

 private:
  mutable std::mutex mtx_;
  MemorySnapshotManagerCore core_;
};

}  // namespace raft
}  // namespace janus
