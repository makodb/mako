// Compile + behavior test for LogStorageFacade. Ensures a plain adapter with
// LogStorage-shaped methods can be wrapped with LogStorageProxy and all facade
// methods route with the expected signatures.

#include <gtest/gtest.h>

#include <rusty/arc.hpp>
#include <rusty/sync/atomic.hpp>

#include "deptran/raft/log_storage_facade.hpp"

using namespace janus::raft;

namespace {

using AtomicInt = rusty::sync::atomic::Atomic<int>;

struct Counts {
  AtomicInt n_get{0};
  AtomicInt n_put{0};
  AtomicInt n_remove{0};
  AtomicInt n_get_range{0};
  AtomicInt n_put_batch{0};
  AtomicInt n_remove_range{0};
  AtomicInt n_first_index{0};
  AtomicInt n_last_index{0};
  AtomicInt n_get_term{0};
  AtomicInt n_size{0};
  AtomicInt n_empty{0};
  AtomicInt n_set_metadata{0};
  AtomicInt n_get_metadata{0};
  AtomicInt n_sync{0};
  AtomicInt n_close{0};
  AtomicInt n_is_open{0};
  AtomicInt n_clear{0};
};

struct RecordingStorage {
  rusty::Arc<Counts> counts{rusty::Arc<Counts>::make()};
  bool open{true};

  rusty::Option<LogEntry> get(slotid_t slot) const {
    counts->n_get.fetch_add(1);
    if (slot == 7) {
      return rusty::Some(LogEntry{slot, 11});
    }
    return rusty::None;
  }

  bool put(const LogEntry& entry) {
    counts->n_put.fetch_add(1);
    return entry.slot_id > 0;
  }

  bool remove(slotid_t slot) {
    counts->n_remove.fetch_add(1);
    return slot > 0;
  }

  std::vector<LogEntry> get_range(slotid_t start, slotid_t end) const {
    counts->n_get_range.fetch_add(1);
    if (start >= end) {
      return {};
    }
    return {LogEntry{start, 1}, LogEntry{end - 1, 2}};
  }

  bool put_batch(const std::vector<LogEntry>& entries) {
    counts->n_put_batch.fetch_add(1);
    return !entries.empty();
  }

  bool remove_range(slotid_t start, slotid_t end) {
    counts->n_remove_range.fetch_add(1);
    return start < end;
  }

  slotid_t get_first_index() const {
    counts->n_first_index.fetch_add(1);
    return 3;
  }

  slotid_t get_last_index() const {
    counts->n_last_index.fetch_add(1);
    return 19;
  }

  rusty::Option<ballot_t> get_term(slotid_t slot) const {
    counts->n_get_term.fetch_add(1);
    if (slot == 9) {
      return rusty::Some(static_cast<ballot_t>(101));
    }
    return rusty::None;
  }

  size_t size() const {
    counts->n_size.fetch_add(1);
    return 5;
  }

  bool empty() const {
    counts->n_empty.fetch_add(1);
    return false;
  }

  bool set_metadata(const std::string& key, const std::string& value) {
    counts->n_set_metadata.fetch_add(1);
    return key == "k" && value == "v";
  }

  rusty::Option<std::string> get_metadata(const std::string& key) const {
    counts->n_get_metadata.fetch_add(1);
    if (key == "k") {
      return rusty::Some(std::string("v"));
    }
    return rusty::None;
  }

  bool sync() {
    counts->n_sync.fetch_add(1);
    return true;
  }

  bool close() {
    counts->n_close.fetch_add(1);
    open = false;
    return true;
  }

  bool is_open() const {
    counts->n_is_open.fetch_add(1);
    return open;
  }

  bool clear() {
    counts->n_clear.fetch_add(1);
    return true;
  }
};

}  // namespace

TEST(RaftLogStorageFacadeTest, AdapterConformsToFacade) {
  auto adapter = rusty::Arc<RecordingStorage>::make();
  LogStorageProxy proxy =
      pro::make_proxy<LogStorageFacade, RecordingStorage>(*adapter);

  auto got = proxy->get(7);
  EXPECT_TRUE(got.is_some());
  EXPECT_EQ(got.unwrap().slot_id, 7u);

  EXPECT_TRUE(proxy->put(LogEntry{1, 2}));
  EXPECT_TRUE(proxy->remove(1));

  auto range = proxy->get_range(4, 8);
  ASSERT_EQ(range.size(), 2u);
  EXPECT_EQ(range[0].slot_id, 4u);
  EXPECT_EQ(range[1].slot_id, 7u);

  EXPECT_TRUE(proxy->put_batch({LogEntry{2, 3}, LogEntry{3, 3}}));
  EXPECT_TRUE(proxy->remove_range(2, 4));

  EXPECT_EQ(proxy->get_first_index(), 3u);
  EXPECT_EQ(proxy->get_last_index(), 19u);

  auto term = proxy->get_term(9);
  EXPECT_TRUE(term.is_some());
  EXPECT_EQ(term.unwrap(), 101u);

  EXPECT_EQ(proxy->size(), 5u);
  EXPECT_FALSE(proxy->empty());
  EXPECT_TRUE(proxy->set_metadata("k", "v"));

  auto meta = proxy->get_metadata("k");
  EXPECT_TRUE(meta.is_some());
  EXPECT_EQ(meta.unwrap(), "v");

  EXPECT_TRUE(proxy->sync());
  EXPECT_TRUE(proxy->is_open());
  EXPECT_TRUE(proxy->close());
  EXPECT_FALSE(proxy->is_open());
  EXPECT_TRUE(proxy->clear());

  EXPECT_EQ(adapter->counts->n_get.load(), 1);
  EXPECT_EQ(adapter->counts->n_put.load(), 1);
  EXPECT_EQ(adapter->counts->n_remove.load(), 1);
  EXPECT_EQ(adapter->counts->n_get_range.load(), 1);
  EXPECT_EQ(adapter->counts->n_put_batch.load(), 1);
  EXPECT_EQ(adapter->counts->n_remove_range.load(), 1);
  EXPECT_EQ(adapter->counts->n_first_index.load(), 1);
  EXPECT_EQ(adapter->counts->n_last_index.load(), 1);
  EXPECT_EQ(adapter->counts->n_get_term.load(), 1);
  EXPECT_EQ(adapter->counts->n_size.load(), 1);
  EXPECT_EQ(adapter->counts->n_empty.load(), 1);
  EXPECT_EQ(adapter->counts->n_set_metadata.load(), 1);
  EXPECT_EQ(adapter->counts->n_get_metadata.load(), 1);
  EXPECT_EQ(adapter->counts->n_sync.load(), 1);
  EXPECT_EQ(adapter->counts->n_close.load(), 1);
  EXPECT_EQ(adapter->counts->n_is_open.load(), 2);
  EXPECT_EQ(adapter->counts->n_clear.load(), 1);
}
