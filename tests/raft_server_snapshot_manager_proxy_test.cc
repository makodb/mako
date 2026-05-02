#include <gtest/gtest.h>

#include <memory>

#include "deptran/raft/server.h"
#include "deptran/raft/memory_snapshot_manager.hpp"

TEST(RaftServerSnapshotManagerProxyTest, SetAndGetManagerRemainCompatible) {
  janus::RaftServer server(/*site_id=*/11, /*partition_id=*/2, /*loc_id=*/7);
  auto manager = std::make_shared<janus::raft::MemorySnapshotManager>();
  auto* raw_manager = manager.get();

  server.SetSnapshotManager(manager);
  EXPECT_NE(server.GetSnapshotManager(), nullptr);
  EXPECT_EQ(server.GetSnapshotManager().get(), raw_manager);

  manager.reset();
  EXPECT_NE(server.GetSnapshotManager(), nullptr);
  EXPECT_EQ(server.GetSnapshotManager().get(), raw_manager);

  server.SetSnapshotManager(std::shared_ptr<janus::raft::SnapshotManager>{});
  EXPECT_EQ(server.GetSnapshotManager(), nullptr);
  EXPECT_FALSE(server.HasSnapshot());
}

TEST(RaftServerSnapshotManagerProxyTest, HasSnapshotWorksViaProxyBoundary) {
  janus::RaftServer server(/*site_id=*/21, /*partition_id=*/3, /*loc_id=*/8);
  auto manager = std::make_shared<janus::raft::MemorySnapshotManager>();

  server.SetSnapshotManager(manager);
  EXPECT_FALSE(server.HasSnapshot());

  const char payload[] = "snapshot-data";
  ASSERT_TRUE(manager->TakeSnapshot(/*last_index=*/12,
                                    /*last_term=*/3,
                                    payload,
                                    sizeof(payload) - 1));
  EXPECT_TRUE(server.HasSnapshot());

  EXPECT_EQ(manager->DeleteAllSnapshots(), 1u);
  EXPECT_FALSE(server.HasSnapshot());
}
