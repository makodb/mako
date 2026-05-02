#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

namespace {

std::string ReadFile(const std::string& path) {
  std::ifstream input(path);
  EXPECT_TRUE(input.is_open()) << "failed to open " << path;
  std::stringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

}  // namespace

TEST(RaftVoteQuorumEventRemovedGuardTest, VoteQuorumEventAndBroadcastVoteStayRemoved) {
#ifndef MAKO_SOURCE_DIR
  GTEST_FAIL() << "MAKO_SOURCE_DIR is not defined";
#else
  const std::string root = MAKO_SOURCE_DIR;
  const std::string commo_h = ReadFile(root + "/src/deptran/raft/commo.h");
  const std::string commo_cc = ReadFile(root + "/src/deptran/raft/commo.cc");

  EXPECT_EQ(commo_h.find("class RaftVoteQuorumEvent"), std::string::npos);
  EXPECT_EQ(commo_h.find("BroadcastVote("), std::string::npos);

  EXPECT_EQ(commo_cc.find("RaftCommo::BroadcastVote("), std::string::npos);
  EXPECT_EQ(commo_cc.find("Reactor::create_sp_event<RaftVoteQuorumEvent>"),
            std::string::npos);
#endif
}
