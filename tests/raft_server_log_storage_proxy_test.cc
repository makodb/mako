#include <gtest/gtest.h>

#include <memory>

#include "deptran/raft/server.h"
#include "deptran/raft/memory_log_storage.hpp"

TEST(RaftServerLogStorageProxyTest, SetAndGetStorageRemainCompatible) {
  janus::RaftServer server(/*site_id=*/11, /*partition_id=*/2, /*loc_id=*/7);
  auto storage = std::make_shared<janus::raft::InMemoryLogStorage>();

  server.SetLogStorage(storage);
  EXPECT_NE(server.GetLogStorage(), nullptr);
  EXPECT_EQ(server.GetLogStorage().get(), storage.get());

  server.SetLogStorage(std::shared_ptr<janus::raft::LogStorage>{});
  EXPECT_EQ(server.GetLogStorage(), nullptr);
  EXPECT_TRUE(server.RecoverFromStorage());
}

TEST(RaftServerLogStorageProxyTest, RecoverFromStorageViaProxyBoundary) {
  janus::RaftServer server(/*site_id=*/21, /*partition_id=*/3, /*loc_id=*/8);
  auto storage = std::make_shared<janus::raft::InMemoryLogStorage>();

  storage->set_metadata("currentTerm", "7");
  storage->set_metadata("vote_for", "21");
  storage->set_metadata("commitIndex", "2");
  // Deliberately too large; RecoverFromStorage should clamp to lastLogIndex.
  storage->set_metadata("specCommitIndex", "50");
  storage->set_metadata("securedLogIndex", "49");

  janus::raft::LogEntry e1(1, 6);
  janus::raft::LogEntry e2(2, 7);
  storage->put(e1);
  storage->put(e2);

  server.SetLogStorage(storage);
  ASSERT_TRUE(server.RecoverFromStorage());

  EXPECT_EQ(server.currentTerm, 7u);
  EXPECT_EQ(server.commitIndex, 2u);
  EXPECT_EQ(server.GetLastLogIndex(), 2u);
  EXPECT_EQ(server.GetSpecCommitIndex(), 2u);
  EXPECT_EQ(server.GetSecuredLogIndex(), 2u);

  auto slot1 = server.GetRaftInstance(1);
  auto slot2 = server.GetRaftInstance(2);
  ASSERT_NE(slot1, nullptr);
  ASSERT_NE(slot2, nullptr);
  EXPECT_EQ(slot1->term, 6u);
  EXPECT_EQ(slot2->term, 7u);
}
