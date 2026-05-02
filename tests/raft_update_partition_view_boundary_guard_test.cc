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

TEST(RaftUpdatePartitionViewBoundaryGuardTest, DirectBoundaryIsExplicitAndUnique) {
#ifndef MAKO_SOURCE_DIR
  GTEST_FAIL() << "MAKO_SOURCE_DIR is not defined";
#else
  const std::string root = MAKO_SOURCE_DIR;
  const std::string server_cc = ReadFile(root + "/src/deptran/raft/server.cc");

  EXPECT_NE(server_cc.find("PHASE8_BOUNDARY_UPDATE_PARTITION_VIEW"), std::string::npos);
  EXPECT_EQ(CountOccurrences(server_cc, "commo()->UpdatePartitionView("), 1u);
#endif
}
