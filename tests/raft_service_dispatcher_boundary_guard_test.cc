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

size_t CountOccurrences(const std::string& haystack, const std::string& needle) {
  size_t count = 0;
  size_t pos = 0;
  while ((pos = haystack.find(needle, pos)) != std::string::npos) {
    ++count;
    pos += needle.size();
  }
  return count;
}

}  // namespace

TEST(RaftServiceDispatcherBoundaryGuardTest, ServiceOwnsAndWiresDispatcherProxy) {
#ifndef MAKO_SOURCE_DIR
  GTEST_FAIL() << "MAKO_SOURCE_DIR is not defined";
#else
  const std::string root = MAKO_SOURCE_DIR;
  const std::string service_h = ReadFile(root + "/src/deptran/raft/service.h");
  const std::string service_cc = ReadFile(root + "/src/deptran/raft/service.cc");

  EXPECT_NE(service_h.find("rusty::Option<raft::DispatcherProxy> dispatcher_;"),
            std::string::npos);
  EXPECT_EQ(service_h.find("#include \"server.h\""), std::string::npos);
  EXPECT_NE(service_cc.find("#include \"raft_server_dispatcher.hpp\""),
            std::string::npos);
  EXPECT_EQ(CountOccurrences(service_cc, "make_raft_server_dispatcher("), 2u);
  EXPECT_EQ(CountOccurrences(service_cc, "dispatcher_ = rusty::Some("), 2u);
  EXPECT_EQ(CountOccurrences(service_cc, "dispatcher_ = rusty::None;"), 2u);
  EXPECT_EQ(CountOccurrences(service_cc, "dispatcher_.is_none()"), 10u);

  EXPECT_NE(service_cc.find("handle_vote("), std::string::npos);
  EXPECT_NE(service_cc.find("handle_vote_durable("), std::string::npos);
  EXPECT_NE(service_cc.find("handle_append_entries("), std::string::npos);
  EXPECT_NE(service_cc.find("handle_empty_append_entries("), std::string::npos);
  EXPECT_NE(service_cc.find("handle_append_entries_durable("), std::string::npos);
  EXPECT_NE(service_cc.find("handle_timeout_now("), std::string::npos);
  EXPECT_NE(service_cc.find("handle_notify_restart("), std::string::npos);
  EXPECT_NE(service_cc.find("handle_install_snapshot("), std::string::npos);
  EXPECT_NE(service_cc.find("handle_add_server("), std::string::npos);
  EXPECT_NE(service_cc.find("handle_remove_server("), std::string::npos);

  EXPECT_EQ(service_cc.find("svr->OnRequestVote("), std::string::npos);
  EXPECT_EQ(service_cc.find("svr->OnVoteDurable("), std::string::npos);
  EXPECT_EQ(service_cc.find("svr->OnAppendEntries("), std::string::npos);
  EXPECT_EQ(service_cc.find("svr->OnAppendEntriesDurable("), std::string::npos);
  EXPECT_EQ(service_cc.find("svr->OnTimeoutNow("), std::string::npos);
  EXPECT_EQ(service_cc.find("svr->OnPeerRestart("), std::string::npos);
  EXPECT_EQ(service_cc.find("svr->OnInstallSnapshot("), std::string::npos);
  EXPECT_EQ(service_cc.find("svr->OnAddServer("), std::string::npos);
  EXPECT_EQ(service_cc.find("svr->OnRemoveServer("), std::string::npos);
#endif
}
