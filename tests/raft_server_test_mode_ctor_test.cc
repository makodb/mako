#include <gtest/gtest.h>

#include "deptran/raft/server.h"

TEST(RaftServerTestModeCtorTest, InitializesIdentityWithoutFrame) {
  constexpr siteid_t kSiteId = 11;
  constexpr parid_t kPartitionId = 2;
  constexpr locid_t kLocId = 7;

  janus::RaftServer server(kSiteId, kPartitionId, kLocId);

  EXPECT_EQ(server.frame_, nullptr);
  EXPECT_EQ(server.site_id_, kSiteId);
  EXPECT_EQ(server.partition_id_, kPartitionId);
  EXPECT_EQ(server.loc_id_, kLocId);
  EXPECT_FALSE(server.IsDisconnected());
  EXPECT_FALSE(server.IsLeader());
  EXPECT_EQ(server.GetLeaderHint(), INVALID_SITEID);
}
