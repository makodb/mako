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

TEST(RaftTimeoutNowCallsiteGuardTest, RaftServerHasNoLegacyDirectTimeoutNowSend) {
#ifndef MAKO_SOURCE_DIR
  GTEST_FAIL() << "MAKO_SOURCE_DIR is not defined";
#else
  const std::string root = MAKO_SOURCE_DIR;
  const std::string server_h = ReadFile(root + "/src/deptran/raft/server.h");
  const std::string server_cc = ReadFile(root + "/src/deptran/raft/server.cc");

  EXPECT_EQ(server_h.find("SendTimeoutNow("), std::string::npos);
  EXPECT_EQ(server_cc.find("SendTimeoutNow("), std::string::npos);
  EXPECT_EQ(server_h.find("commo()->SendTimeoutNow"), std::string::npos);
  EXPECT_EQ(server_cc.find("commo()->SendTimeoutNow"), std::string::npos);
#endif
}
