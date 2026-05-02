#pragma once

#include <memory>
#include <string>
#include <vector>

#include "log_storage.hpp"
#include "log_storage_facade.hpp"
#include "snapshot_manager.hpp"
#include "snapshot_manager_facade.hpp"

namespace janus {
namespace raft {

struct SharedLogStorageAdapter {
  std::shared_ptr<LogStorage> impl;

  rusty::Option<LogEntry> get(slotid_t slot_id) const {
    return impl->get(slot_id);
  }
  bool put(const LogEntry& entry) {
    return impl->put(entry);
  }
  bool remove(slotid_t slot_id) {
    return impl->remove(slot_id);
  }
  std::vector<LogEntry> get_range(slotid_t start, slotid_t end) const {
    return impl->get_range(start, end);
  }
  bool put_batch(const std::vector<LogEntry>& entries) {
    return impl->put_batch(entries);
  }
  bool remove_range(slotid_t start, slotid_t end) {
    return impl->remove_range(start, end);
  }
  slotid_t get_first_index() const {
    return impl->get_first_index();
  }
  slotid_t get_last_index() const {
    return impl->get_last_index();
  }
  rusty::Option<ballot_t> get_term(slotid_t slot_id) const {
    return impl->get_term(slot_id);
  }
  size_t size() const {
    return impl->size();
  }
  bool empty() const {
    return impl->empty();
  }
  bool set_metadata(const std::string& key, const std::string& value) {
    return impl->set_metadata(key, value);
  }
  rusty::Option<std::string> get_metadata(const std::string& key) const {
    return impl->get_metadata(key);
  }
  bool sync() {
    return impl->sync();
  }
  bool close() {
    return impl->close();
  }
  bool is_open() const {
    return impl->is_open();
  }
  bool clear() {
    return impl->clear();
  }
};

struct SharedSnapshotManagerAdapter {
  std::shared_ptr<SnapshotManager> impl;

  std::unique_ptr<SnapshotWriter> BeginSnapshot(slotid_t last_index,
                                                ballot_t last_term) {
    return impl->BeginSnapshot(last_index, last_term);
  }
  bool TakeSnapshot(slotid_t last_index, ballot_t last_term,
                    const char* data, size_t size) {
    return impl->TakeSnapshot(last_index, last_term, data, size);
  }
  std::unique_ptr<SnapshotReader> BeginLoad(const SnapshotMetadata& metadata) {
    return impl->BeginLoad(metadata);
  }
  bool LoadLatestSnapshot(SnapshotMetadata* metadata_out,
                          std::string* data_out) {
    return impl->LoadLatestSnapshot(metadata_out, data_out);
  }
  rusty::Option<SnapshotMetadata> GetLatestSnapshot() const {
    return impl->GetLatestSnapshot();
  }
  std::vector<SnapshotMetadata> ListSnapshots() const {
    return impl->ListSnapshots();
  }
  bool HasSnapshotAtOrAfter(slotid_t min_index) const {
    return impl->HasSnapshotAtOrAfter(min_index);
  }
  size_t PruneSnapshots(slotid_t keep_after_index) {
    return impl->PruneSnapshots(keep_after_index);
  }
  size_t DeleteAllSnapshots() {
    return impl->DeleteAllSnapshots();
  }
  const std::string& GetStoragePath() const {
    return impl->GetStoragePath();
  }
};

inline LogStorageProxy make_log_storage_proxy(
    const std::shared_ptr<LogStorage>& storage) {
  if (!storage) {
    return LogStorageProxy{};
  }
  return pro::make_proxy<LogStorageFacade, SharedLogStorageAdapter>(
      SharedLogStorageAdapter{storage});
}

inline SnapshotManagerProxy make_snapshot_manager_proxy(
    const std::shared_ptr<SnapshotManager>& manager) {
  if (!manager) {
    return SnapshotManagerProxy{};
  }
  return pro::make_proxy<SnapshotManagerFacade, SharedSnapshotManagerAdapter>(
      SharedSnapshotManagerAdapter{manager});
}

inline std::shared_ptr<LogStorage> make_non_owning_log_storage(
    LogStorage* storage) {
  if (storage == nullptr) {
    return {};
  }
  return std::shared_ptr<LogStorage>(storage, [](LogStorage*) {});
}

inline std::shared_ptr<SnapshotManager> make_non_owning_snapshot_manager(
    SnapshotManager* manager) {
  if (manager == nullptr) {
    return {};
  }
  return std::shared_ptr<SnapshotManager>(manager, [](SnapshotManager*) {});
}

}  // namespace raft
}  // namespace janus
