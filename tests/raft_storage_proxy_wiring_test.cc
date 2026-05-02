#include <gtest/gtest.h>

#include <memory>

#include "deptran/raft/memory_log_storage.hpp"
#include "deptran/raft/memory_snapshot_manager.hpp"
#include "deptran/raft/storage_proxy_wiring.hpp"

using namespace janus::raft;

TEST(RaftStorageProxyWiringTest, NonOwningSharedAliasesPreservePointers) {
  auto log = std::make_shared<InMemoryLogStorage>();
  auto snap = std::make_shared<MemorySnapshotManager>();

  auto log_alias = make_non_owning_log_storage(log.get());
  auto snap_alias = make_non_owning_snapshot_manager(snap.get());

  ASSERT_NE(log_alias, nullptr);
  ASSERT_NE(snap_alias, nullptr);
  EXPECT_EQ(log_alias.get(), log.get());
  EXPECT_EQ(snap_alias.get(), snap.get());
}

TEST(RaftStorageProxyWiringTest, LogStorageProxyFactoryRoutesOperations) {
  auto storage = std::make_shared<InMemoryLogStorage>();
  auto proxy = make_log_storage_proxy(storage);

  ASSERT_TRUE(proxy.has_value());
  EXPECT_TRUE(proxy->empty());
  EXPECT_TRUE(proxy->put(LogEntry{3, 9}));
  EXPECT_FALSE(proxy->empty());
  EXPECT_EQ(proxy->get_last_index(), 3u);

  auto loaded = proxy->get(3);
  ASSERT_TRUE(loaded.is_some());
  EXPECT_EQ(loaded.unwrap().term, 9u);

  auto none_proxy = make_log_storage_proxy(std::shared_ptr<LogStorage>{});
  EXPECT_FALSE(none_proxy.has_value());
}

TEST(RaftStorageProxyWiringTest, SnapshotProxyFactoryRoutesOperations) {
  auto manager = std::make_shared<MemorySnapshotManager>();
  auto proxy = make_snapshot_manager_proxy(manager);

  ASSERT_TRUE(proxy.has_value());
  EXPECT_FALSE(proxy->HasSnapshotAtOrAfter(1));
  ASSERT_TRUE(proxy->TakeSnapshot(10, 2, "abc", 3));
  EXPECT_TRUE(proxy->HasSnapshotAtOrAfter(10));

  SnapshotMetadata meta;
  std::string payload;
  ASSERT_TRUE(proxy->LoadLatestSnapshot(&meta, &payload));
  EXPECT_EQ(meta.last_included_index, 10u);
  EXPECT_EQ(meta.last_included_term, 2u);
  EXPECT_EQ(payload, "abc");

  auto none_proxy =
      make_snapshot_manager_proxy(std::shared_ptr<SnapshotManager>{});
  EXPECT_FALSE(none_proxy.has_value());
}
