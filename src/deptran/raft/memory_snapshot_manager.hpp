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
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "snapshot_manager.hpp"

namespace janus {
namespace raft {

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
inline SnapshotMetadata memory_snapshot_metadata(uint64_t last_index, uint64_t last_term, size_t size) {
    return SnapshotMetadata{.last_included_index = std::move(last_index), .last_included_term = std::move(last_term), .timestamp_ms = 0, .size_bytes = std::move(size), .checksum = std::string()};
}
/*RUSTYCPP:GEN-END id=memory_snapshot_manager.metadata*/

class MemorySnapshotWriter : public SnapshotWriter {
 public:
  // @unsafe - borrows MemorySnapshotManager internals. The writer must not
  // outlive the manager that created it.
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
    offset_ += size;
    return true;
  }

  // @unsafe - publishes buffer under mutex
  bool Finalize() override {
    std::lock_guard<std::mutex> lk(*mtx_);
    *dest_payload_ = std::move(buffer_);
    *dest_meta_ = memory_snapshot_metadata(last_index_, last_term_,
                                           dest_payload_->size());
    *has_snapshot_ = true;
    finalized_ = true;
    return true;
  }

  // @safe
  bool Abort() override { buffer_.clear(); return true; }

  // @safe
  size_t GetOffset() const override { return offset_; }

 private:
  // @unsafe - borrowed from MemorySnapshotManager; not owned or deleted here.
  std::string*         dest_payload_{nullptr};
  // @unsafe - borrowed from MemorySnapshotManager; not owned or deleted here.
  SnapshotMetadata*    dest_meta_{nullptr};
  // @unsafe - borrowed from MemorySnapshotManager; not owned or deleted here.
  bool*                has_snapshot_{nullptr};
  // @unsafe - borrowed from MemorySnapshotManager; guards the borrowed fields.
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
    size_t remaining = payload_.size() - offset_;
    size_t n = buffer_size < remaining ? buffer_size : remaining;
    std::memcpy(buffer, payload_.data() + offset_, n);
    offset_ += n;
    *bytes_read = n;
    return true;
  }

  // @safe
  bool IsComplete() const override { return offset_ >= payload_.size(); }

  // @safe
  const SnapshotMetadata& GetMetadata() const override { return meta_; }

  // @safe
  size_t GetOffset() const override { return offset_; }

 private:
  std::string      payload_;
  SnapshotMetadata meta_{};
  size_t           offset_{0};
};

class MemorySnapshotManager : public SnapshotManager {
 public:
  // @safe
  MemorySnapshotManager() = default;

  // @unsafe - returned writer owns itself but borrows this manager's payload,
  // metadata, flag, and mutex. Do not let the writer outlive the manager.
  std::unique_ptr<SnapshotWriter> BeginSnapshot(
      slotid_t last_index, ballot_t last_term) override {
    return std::make_unique<MemorySnapshotWriter>(
        &payload_, &meta_, &has_snapshot_, &mtx_, last_index, last_term);
  }

  // @unsafe - memcpy into internal buffer under mutex
  bool TakeSnapshot(slotid_t last_index, ballot_t last_term,
                    const char* data, size_t size) override {
    std::lock_guard<std::mutex> lk(mtx_);
    payload_.assign(data, size);
    meta_ = memory_snapshot_metadata(last_index, last_term, size);
    has_snapshot_             = true;
    return true;
  }

  // @safe - returned reader owns a copy of the in-memory snapshot payload.
  std::unique_ptr<SnapshotReader> BeginLoad(
      const SnapshotMetadata& /*metadata*/) override {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!has_snapshot_) return nullptr;
    return std::make_unique<MemorySnapshotReader>(payload_, meta_);
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
