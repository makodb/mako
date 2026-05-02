// Compile + behavior test for SnapshotManagerFacade. Ensures a plain adapter
// with SnapshotManager-shaped methods can be wrapped with SnapshotManagerProxy
// and all facade methods route with the expected signatures.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <rusty/arc.hpp>
#include <rusty/sync/atomic.hpp>

#include "deptran/raft/snapshot_manager_facade.hpp"

using namespace janus::raft;

namespace {

using AtomicInt = rusty::sync::atomic::Atomic<int>;

struct Counts {
  AtomicInt n_begin_snapshot{0};
  AtomicInt n_take_snapshot{0};
  AtomicInt n_begin_load{0};
  AtomicInt n_load_latest_snapshot{0};
  AtomicInt n_get_latest_snapshot{0};
  AtomicInt n_list_snapshots{0};
  AtomicInt n_has_snapshot_at_or_after{0};
  AtomicInt n_prune_snapshots{0};
  AtomicInt n_delete_all_snapshots{0};
  AtomicInt n_get_storage_path{0};
};

struct SharedState {
  slotid_t begin_snapshot_index{0};
  ballot_t begin_snapshot_term{0};
  slotid_t take_snapshot_index{0};
  ballot_t take_snapshot_term{0};
  std::string take_snapshot_payload;
  SnapshotMetadata begin_load_metadata;
  bool has_latest{false};
  SnapshotMetadata latest_metadata;
  std::string latest_payload;
};

class BufferingSnapshotWriter : public SnapshotWriter {
 public:
  bool Write(const char* data, size_t size) override {
    if (aborted_ || finalized_) {
      return false;
    }
    if (data != nullptr && size > 0) {
      payload_.append(data, size);
    }
    return true;
  }

  bool Finalize() override {
    finalized_ = true;
    return !aborted_;
  }

  bool Abort() override {
    aborted_ = true;
    return true;
  }

  size_t GetOffset() const override {
    return payload_.size();
  }

 private:
  std::string payload_;
  bool finalized_{false};
  bool aborted_{false};
};

class BufferingSnapshotReader : public SnapshotReader {
 public:
  BufferingSnapshotReader(SnapshotMetadata metadata, std::string payload)
      : metadata_(std::move(metadata)), payload_(std::move(payload)) {}

  bool Read(char* buffer, size_t buffer_size, size_t* bytes_read) override {
    if (bytes_read == nullptr) {
      return false;
    }
    const size_t remaining = payload_.size() - offset_;
    const size_t n = std::min(buffer_size, remaining);
    if (n > 0 && buffer != nullptr) {
      std::memcpy(buffer, payload_.data() + offset_, n);
    }
    offset_ += n;
    *bytes_read = n;
    return true;
  }

  bool IsComplete() const override {
    return offset_ >= payload_.size();
  }

  const SnapshotMetadata& GetMetadata() const override {
    return metadata_;
  }

  size_t GetOffset() const override {
    return offset_;
  }

 private:
  SnapshotMetadata metadata_;
  std::string payload_;
  size_t offset_{0};
};

struct RecordingSnapshotManager {
  rusty::Arc<Counts> counts{rusty::Arc<Counts>::make()};
  std::shared_ptr<SharedState> state{std::make_shared<SharedState>()};
  std::string storage_path{"/tmp/mako_raft_snapshots"};

  std::unique_ptr<SnapshotWriter> BeginSnapshot(slotid_t last_index,
                                                ballot_t last_term) {
    counts->n_begin_snapshot.fetch_add(1);
    state->begin_snapshot_index = last_index;
    state->begin_snapshot_term = last_term;
    return std::unique_ptr<SnapshotWriter>(new BufferingSnapshotWriter());
  }

  bool TakeSnapshot(slotid_t last_index, ballot_t last_term,
                    const char* data, size_t size) {
    counts->n_take_snapshot.fetch_add(1);
    state->take_snapshot_index = last_index;
    state->take_snapshot_term = last_term;
    state->take_snapshot_payload.assign(data, size);
    state->latest_metadata.last_included_index = last_index;
    state->latest_metadata.last_included_term = last_term;
    state->latest_metadata.size_bytes = size;
    state->latest_payload = state->take_snapshot_payload;
    state->has_latest = true;
    return true;
  }

  std::unique_ptr<SnapshotReader> BeginLoad(const SnapshotMetadata& metadata) {
    counts->n_begin_load.fetch_add(1);
    state->begin_load_metadata = metadata;
    return std::unique_ptr<SnapshotReader>(
        new BufferingSnapshotReader(metadata, state->latest_payload));
  }

  bool LoadLatestSnapshot(SnapshotMetadata* metadata_out,
                          std::string* data_out) {
    counts->n_load_latest_snapshot.fetch_add(1);
    if (!state->has_latest || metadata_out == nullptr || data_out == nullptr) {
      return false;
    }
    *metadata_out = state->latest_metadata;
    *data_out = state->latest_payload;
    return true;
  }

  rusty::Option<SnapshotMetadata> GetLatestSnapshot() const {
    counts->n_get_latest_snapshot.fetch_add(1);
    if (!state->has_latest) {
      return rusty::None;
    }
    return rusty::Some(state->latest_metadata);
  }

  std::vector<SnapshotMetadata> ListSnapshots() const {
    counts->n_list_snapshots.fetch_add(1);
    if (!state->has_latest) {
      return {};
    }
    SnapshotMetadata older = state->latest_metadata;
    if (older.last_included_index > 0) {
      older.last_included_index -= 1;
    }
    return {state->latest_metadata, older};
  }

  bool HasSnapshotAtOrAfter(slotid_t min_index) const {
    counts->n_has_snapshot_at_or_after.fetch_add(1);
    return state->has_latest &&
           state->latest_metadata.last_included_index >= min_index;
  }

  size_t PruneSnapshots(slotid_t keep_after_index) {
    counts->n_prune_snapshots.fetch_add(1);
    return state->has_latest &&
                   state->latest_metadata.last_included_index < keep_after_index
               ? 1
               : 0;
  }

  size_t DeleteAllSnapshots() {
    counts->n_delete_all_snapshots.fetch_add(1);
    const size_t removed = state->has_latest ? 1 : 0;
    state->has_latest = false;
    state->latest_payload.clear();
    state->latest_metadata = SnapshotMetadata{};
    return removed;
  }

  const std::string& GetStoragePath() const {
    counts->n_get_storage_path.fetch_add(1);
    return storage_path;
  }
};

}  // namespace

TEST(RaftSnapshotManagerFacadeTest, AdapterConformsToFacade) {
  auto adapter = rusty::Arc<RecordingSnapshotManager>::make();
  SnapshotManagerProxy proxy =
      pro::make_proxy<SnapshotManagerFacade, RecordingSnapshotManager>(*adapter);

  auto writer = proxy->BeginSnapshot(42, 7);
  ASSERT_NE(writer, nullptr);
  EXPECT_TRUE(writer->Write("abc", 3));
  EXPECT_EQ(writer->GetOffset(), 3u);
  EXPECT_TRUE(writer->Finalize());
  EXPECT_EQ(adapter->state->begin_snapshot_index, 42u);
  EXPECT_EQ(adapter->state->begin_snapshot_term, 7u);

  EXPECT_TRUE(proxy->TakeSnapshot(43, 8, "hello", 5));
  EXPECT_EQ(adapter->state->take_snapshot_index, 43u);
  EXPECT_EQ(adapter->state->take_snapshot_term, 8u);
  EXPECT_EQ(adapter->state->take_snapshot_payload, "hello");

  SnapshotMetadata requested{};
  requested.last_included_index = 43;
  requested.last_included_term = 8;
  auto reader = proxy->BeginLoad(requested);
  ASSERT_NE(reader, nullptr);
  char buf[8] = {};
  size_t n1 = 0;
  size_t n2 = 0;
  EXPECT_TRUE(reader->Read(buf, 2, &n1));
  EXPECT_TRUE(reader->Read(buf + 2, sizeof(buf) - 2, &n2));
  EXPECT_EQ(std::string(buf, n1 + n2), "hello");
  EXPECT_TRUE(reader->IsComplete());
  EXPECT_EQ(reader->GetMetadata().last_included_index, 43u);
  EXPECT_EQ(reader->GetOffset(), 5u);
  EXPECT_EQ(adapter->state->begin_load_metadata.last_included_index, 43u);

  SnapshotMetadata loaded{};
  std::string payload;
  EXPECT_TRUE(proxy->LoadLatestSnapshot(&loaded, &payload));
  EXPECT_EQ(loaded.last_included_index, 43u);
  EXPECT_EQ(loaded.last_included_term, 8u);
  EXPECT_EQ(payload, "hello");

  auto latest = proxy->GetLatestSnapshot();
  ASSERT_TRUE(latest.is_some());
  EXPECT_EQ(latest.unwrap().last_included_index, 43u);

  auto list = proxy->ListSnapshots();
  ASSERT_EQ(list.size(), 2u);
  EXPECT_EQ(list[0].last_included_index, 43u);
  EXPECT_EQ(list[1].last_included_index, 42u);

  EXPECT_TRUE(proxy->HasSnapshotAtOrAfter(40));
  EXPECT_FALSE(proxy->HasSnapshotAtOrAfter(50));
  EXPECT_EQ(proxy->PruneSnapshots(44), 1u);

  EXPECT_EQ(proxy->DeleteAllSnapshots(), 1u);
  auto none = proxy->GetLatestSnapshot();
  EXPECT_TRUE(none.is_none());

  EXPECT_EQ(proxy->GetStoragePath(), "/tmp/mako_raft_snapshots");

  EXPECT_EQ(adapter->counts->n_begin_snapshot.load(), 1);
  EXPECT_EQ(adapter->counts->n_take_snapshot.load(), 1);
  EXPECT_EQ(adapter->counts->n_begin_load.load(), 1);
  EXPECT_EQ(adapter->counts->n_load_latest_snapshot.load(), 1);
  EXPECT_EQ(adapter->counts->n_get_latest_snapshot.load(), 2);
  EXPECT_EQ(adapter->counts->n_list_snapshots.load(), 1);
  EXPECT_EQ(adapter->counts->n_has_snapshot_at_or_after.load(), 2);
  EXPECT_EQ(adapter->counts->n_prune_snapshots.load(), 1);
  EXPECT_EQ(adapter->counts->n_delete_all_snapshots.load(), 1);
  EXPECT_EQ(adapter->counts->n_get_storage_path.load(), 1);
}
