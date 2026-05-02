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

TEST(RaftRpcParProxiesBoundaryGuardTest, ServerUsesCommoHelperInsteadOfDirectLookup) {
#ifndef MAKO_SOURCE_DIR
  GTEST_FAIL() << "MAKO_SOURCE_DIR is not defined";
#else
  const std::string root = MAKO_SOURCE_DIR;
  const std::string server_cc = ReadFile(root + "/src/deptran/raft/server.cc");
  const std::string commo_h = ReadFile(root + "/src/deptran/raft/commo.h");
  const std::string commo_cc = ReadFile(root + "/src/deptran/raft/commo.cc");

  EXPECT_NE(commo_h.find("GetPartitionProxySiteIds("), std::string::npos);
  EXPECT_NE(commo_cc.find("RaftCommo::GetPartitionProxySiteIds("), std::string::npos);
  EXPECT_EQ(CountOccurrences(server_cc, "GetPartitionProxySiteIds("), 2u);

  EXPECT_EQ(server_cc.find("commo()->rpc_par_proxies_["), std::string::npos);
  EXPECT_EQ(server_cc.find("proxies = c->rpc_par_proxies_[partition_id_]"), std::string::npos);
  EXPECT_EQ(server_cc.find("proxies = commo()->rpc_par_proxies_[partition_id]"), std::string::npos);
#endif
}
