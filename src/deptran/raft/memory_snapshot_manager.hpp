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
 * Note on rusty-safety: this file implements the existing virtual
 * SnapshotManager interface (which itself is virtual). Retiring that
 * interface is a cross-cutting server.cc refactor left for a follow-up
 * — see docs/dev/raft_decouple_plan.md Phase 5 notes.
 */

#include <cstring>
#include <mutex>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <rusty/num.hpp>
#include <rusty/slice.hpp>

#include "snapshot_manager.hpp"

namespace janus {
namespace raft {

#if RUSTYCPP_RUST
pub const fn memory_snapshot_advance_offset(offset: usize,
                                            size: usize) -> usize {
    offset.wrapping_add(size)
}

pub const fn memory_snapshot_reader_bytes_to_read(payload_size: usize,
                                                  offset: usize,
                                                  buffer_size: usize) -> usize {
    let remaining = payload_size.wrapping_sub(offset);
    if buffer_size < remaining {
        buffer_size
    } else {
        remaining
    }
}

pub const fn memory_snapshot_reader_is_complete(payload_size: usize,
                                                offset: usize) -> bool {
    offset >= payload_size
}
#endif
/*RUSTYCPP:GEN-BEGIN id=raft_memory_snapshot.stream_math version=1 rust_sha256=944cf8be20aaa4f98a2a7470ac72dcd139fb927988a33f8ddc6c564cabe5c36f*/
constexpr size_t memory_snapshot_advance_offset(size_t offset, size_t size);
constexpr size_t memory_snapshot_reader_bytes_to_read(size_t payload_size, size_t offset, size_t buffer_size);
constexpr bool memory_snapshot_reader_is_complete(size_t payload_size, size_t offset);
constexpr size_t memory_snapshot_advance_offset(size_t offset, size_t size) {
    return rusty::wrapping_add(offset, static_cast<std::remove_cvref_t<decltype(offset)>>(std::move(size)));
}
constexpr size_t memory_snapshot_reader_bytes_to_read(size_t payload_size, size_t offset, size_t buffer_size) {
    auto remaining = rusty::wrapping_sub(payload_size, static_cast<std::remove_cvref_t<decltype(payload_size)>>(std::move(offset)));
    if (rusty::detail::deref_if_pointer_like(buffer_size) < rusty::detail::deref_if_pointer_like(remaining)) {
        return std::move(buffer_size);
    } else {
        return std::move(remaining);
    }
}
constexpr bool memory_snapshot_reader_is_complete(size_t payload_size, size_t offset) {
    return rusty::detail::deref_if_pointer_like(offset) >= rusty::detail::deref_if_pointer_like(payload_size);
}
/*RUSTYCPP:GEN-END id=raft_memory_snapshot.stream_math*/

static_assert(memory_snapshot_advance_offset(7, 5) == 12);
static_assert(memory_snapshot_advance_offset(static_cast<size_t>(-1), 1) == 0);
static_assert(memory_snapshot_reader_bytes_to_read(10, 4, 3) == 3);
static_assert(memory_snapshot_reader_bytes_to_read(10, 4, 9) == 6);
static_assert(!memory_snapshot_reader_is_complete(10, 9));
static_assert(memory_snapshot_reader_is_complete(10, 10));

class MemorySnapshotWriter : public SnapshotWriter {
 public:
  // @safe
  MemorySnapshotWriter(std::string* dest_payload,
                       SnapshotMetadata* dest_meta,
                       bool* has_snapshot,
                       std::mutex* mtx,
                       slotid_t last_index,
                       ballot_t last_term)
      : dest_payload_(dest_payload),
        dest_meta_(dest_meta),
        has_snapshot_(has_snapshot),
        mtx_(mtx),
        last_index_(last_index),
        last_term_(last_term) {}

  // @unsafe - raw pointer append to internal buffer
  bool Write(const char* data, size_t size) override {
    buffer_.append(data, size);
    offset_ = memory_snapshot_advance_offset(offset_, size);
    return true;
  }

  // @unsafe - publishes buffer under mutex
  bool Finalize() override {
    std::lock_guard<std::mutex> lk(*mtx_);
    *dest_payload_ = std::move(buffer_);
    dest_meta_->last_included_index = last_index_;
    dest_meta_->last_included_term  = last_term_;
    dest_meta_->size_bytes          = dest_payload_->size();
    *has_snapshot_ = true;
    finalized_ = true;
    return true;
  }

  // @safe
  bool Abort() override { buffer_.clear(); return true; }

  // @safe
  size_t GetOffset() const override { return offset_; }

 private:
  std::string*         dest_payload_{nullptr};
  SnapshotMetadata*    dest_meta_{nullptr};
  bool*                has_snapshot_{nullptr};
  std::mutex*          mtx_{nullptr};
  std::string          buffer_{};
  size_t               offset_{0};
  slotid_t             last_index_{0};
  ballot_t             last_term_{0};
  bool                 finalized_{false};
};

class MemorySnapshotReader : public SnapshotReader {
 public:
  // @safe
  MemorySnapshotReader(std::string payload, SnapshotMetadata meta)
      : payload_(std::move(payload)), meta_(std::move(meta)) {}

  // @unsafe - raw buffer copy from internal string
  bool Read(char* buffer, size_t buffer_size, size_t* bytes_read) override {
    size_t n = memory_snapshot_reader_bytes_to_read(
        payload_.size(), offset_, buffer_size);
    std::memcpy(buffer, payload_.data() + offset_, n);
    offset_ = memory_snapshot_advance_offset(offset_, n);
    *bytes_read = n;
    return true;
  }

  // @safe
  bool IsComplete() const override {
    return memory_snapshot_reader_is_complete(payload_.size(), offset_);
  }

  // @safe
  const SnapshotMetadata& GetMetadata() const override { return meta_; }

  // @safe
  size_t GetOffset() const override { return offset_; }

 private:
  std::string      payload_;
  SnapshotMetadata meta_;
  size_t           offset_{0};
};

class MemorySnapshotManager : public SnapshotManager {
 public:
  // @safe
  MemorySnapshotManager() = default;

  // @safe
  std::unique_ptr<SnapshotWriter> BeginSnapshot(
      slotid_t last_index, ballot_t last_term) override {
    return std::unique_ptr<SnapshotWriter>(
        new MemorySnapshotWriter(&payload_, &meta_, &has_snapshot_, &mtx_,
                                 last_index, last_term));
  }

  // @unsafe - memcpy into internal buffer under mutex
  bool TakeSnapshot(slotid_t last_index, ballot_t last_term,
                    const char* data, size_t size) override {
    std::lock_guard<std::mutex> lk(mtx_);
    payload_.assign(data, size);
    meta_.last_included_index = last_index;
    meta_.last_included_term  = last_term;
    meta_.size_bytes          = size;
    has_snapshot_             = true;
    return true;
  }

  // @safe
  std::unique_ptr<SnapshotReader> BeginLoad(
      const SnapshotMetadata& /*metadata*/) override {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!has_snapshot_) return nullptr;
    return std::unique_ptr<SnapshotReader>(
        new MemorySnapshotReader(payload_, meta_));
  }

  // @unsafe - writes to caller-owned pointers under mutex
  bool LoadLatestSnapshot(SnapshotMetadata* metadata_out,
                          std::string* data_out) override {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!has_snapshot_) return false;
    if (metadata_out) *metadata_out = meta_;
    if (data_out)     *data_out     = payload_;
    return true;
  }

  // @safe
  rusty::Option<SnapshotMetadata> GetLatestSnapshot() const override {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!has_snapshot_) return rusty::None;
    return rusty::Some(meta_);
  }

  // @safe
  std::vector<SnapshotMetadata> ListSnapshots() const override {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!has_snapshot_) return {};
    return {meta_};
  }

  // @safe
  bool HasSnapshotAtOrAfter(slotid_t min_index) const override {
    std::lock_guard<std::mutex> lk(mtx_);
    return has_snapshot_ && meta_.last_included_index >= min_index;
  }

  // @safe - single-snapshot manager never prunes below current
  size_t PruneSnapshots(slotid_t keep_after_index) override {
    std::lock_guard<std::mutex> lk(mtx_);
    if (has_snapshot_ && meta_.last_included_index < keep_after_index) {
      has_snapshot_ = false;
      payload_.clear();
      meta_ = SnapshotMetadata{};
      return 1;
    }
    return 0;
  }

  // @safe
  size_t DeleteAllSnapshots() override {
    std::lock_guard<std::mutex> lk(mtx_);
    size_t n = has_snapshot_ ? 1 : 0;
    has_snapshot_ = false;
    payload_.clear();
    meta_ = SnapshotMetadata{};
    return n;
  }

  // @lifetime: (&'a) -> &'a
  const std::string& GetStoragePath() const override {
    return storage_path_;
  }

 private:
  mutable std::mutex     mtx_;
  bool                   has_snapshot_{false};
  SnapshotMetadata       meta_{};
  std::string            payload_{};
  std::string            storage_path_{"<memory>"};
};

}  // namespace raft
}  // namespace janus
