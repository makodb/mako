#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "deptran/raft/application_log.h"
#include "deptran/replication_log_entry.h"
#include "deptran/tpc_command.h"
#include "rrr/rrr.hpp"

namespace {

rusty::Arc<janus::TpcCommitCommand> MakeCommit(
    const std::string& payload, uint32_t partition_id, uint64_t tx_id) {
  janus::LogEntry raw_log;
  EXPECT_TRUE(janus::raft::EncodeApplicationLog(
      payload.data(), static_cast<int>(payload.size()), partition_id,
      &raw_log.log_entry));
  raw_log.length = static_cast<int>(raw_log.log_entry.size());

  auto commit = rusty::Arc<janus::TpcCommitCommand>::make();
  auto& mut = commit.get_mut().unwrap();
  mut.tx_id_ = tx_id;
  mut.term = 11;
  mut.cmd_ = rusty::Arc<janus::LogEntry>::make(std::move(raw_log));
  return commit;
}

janus::Command RoundTrip(const janus::Command& outgoing) {
  rrr::BufferSink sink;
  rrr::BinaryWriteArchive writer{rrr::make_sink_proxy_buffer(&sink)};
  rrr::Serialize_::serialize(outgoing, writer);

  janus::Command incoming;
  rrr::BufferSource source(sink.bytes.data(), sink.bytes.len());
  rrr::BinaryReadArchive reader{rrr::make_source_proxy_buffer(&source)};
  rrr::Deserialize_::deserialize(incoming, reader);
  EXPECT_TRUE(source.eof());
  return incoming;
}

TEST(RaftApplicationLogTest, RoundTripsBinaryPayloadAndPartition) {
  const std::string input{"A\0B\xff" "C", 5};
  std::string encoded;
  ASSERT_TRUE(janus::raft::EncodeApplicationLog(
      input.data(), static_cast<int>(input.size()), 0xfedcba98u, &encoded));

  const char* payload = nullptr;
  int length = -1;
  uint32_t partition_id = 0;
  ASSERT_TRUE(janus::raft::DecodeApplicationLog(
      encoded, &payload, &length, &partition_id));
  EXPECT_EQ(partition_id, 0xfedcba98u);
  ASSERT_EQ(length, static_cast<int>(input.size()));
  EXPECT_EQ(std::string(payload, length), input);
}

TEST(RaftApplicationLogTest, RoundTripsEmptyPayload) {
  std::string encoded;
  ASSERT_TRUE(janus::raft::EncodeApplicationLog(nullptr, 0, 7, &encoded));

  const char* payload = nullptr;
  int length = -1;
  uint32_t partition_id = 0;
  ASSERT_TRUE(janus::raft::DecodeApplicationLog(
      encoded, &payload, &length, &partition_id));
  EXPECT_EQ(length, 0);
  EXPECT_EQ(partition_id, 7u);
  EXPECT_NE(payload, nullptr);
}

TEST(RaftApplicationLogTest, RejectsInvalidEncodeArguments) {
  std::string encoded = "unchanged";
  EXPECT_FALSE(janus::raft::EncodeApplicationLog(nullptr, 1, 0, &encoded));
  EXPECT_EQ(encoded, "unchanged");
  EXPECT_FALSE(janus::raft::EncodeApplicationLog("x", -1, 0, &encoded));
  EXPECT_FALSE(janus::raft::EncodeApplicationLog(
      "x", std::numeric_limits<int>::max(), 0, &encoded));
  EXPECT_FALSE(janus::raft::EncodeApplicationLog("x", 1, 0, nullptr));
}

TEST(RaftApplicationLogTest, RejectsTruncationAndHeaderCorruption) {
  std::string encoded;
  ASSERT_TRUE(janus::raft::EncodeApplicationLog("payload", 7, 3, &encoded));

  const char* payload = nullptr;
  int length = -1;
  uint32_t partition_id = 0;
  for (size_t size = 0; size < encoded.size(); ++size) {
    const std::string truncated = encoded.substr(0, size);
    EXPECT_FALSE(janus::raft::DecodeApplicationLog(
        truncated, &payload, &length, &partition_id));
  }

  for (size_t offset : {size_t{0}, size_t{8}, size_t{9}, size_t{10},
                        size_t{11}, size_t{16}}) {
    std::string corrupt = encoded;
    corrupt[offset] ^= 1;
    EXPECT_FALSE(janus::raft::DecodeApplicationLog(
        corrupt, &payload, &length, &partition_id));
  }
}

TEST(RaftApplicationLogTest, RoundTripsCommitThroughCommandRegistry) {
  const std::string input{"registry\0payload", 16};
  janus::Command outgoing{MakeCommit(input, 19, 42)};
  const janus::Command incoming = RoundTrip(outgoing);

  const auto commit = rrr::marshallable_cast<janus::TpcCommitCommand>(incoming);
  ASSERT_TRUE(commit.is_some());
  EXPECT_EQ(commit.unwrap()->tx_id_, 42u);
  EXPECT_EQ(commit.unwrap()->term, 11);
  const auto raw = rrr::marshallable_cast<janus::LogEntry>(
      commit.unwrap()->cmd_);
  ASSERT_TRUE(raw.is_some());

  const char* payload = nullptr;
  int length = -1;
  uint32_t partition_id = 0;
  ASSERT_TRUE(janus::raft::DecodeApplicationLog(
      raw.unwrap()->log_entry, &payload, &length, &partition_id));
  EXPECT_EQ(partition_id, 19u);
  EXPECT_EQ(std::string(payload, length), input);
}

TEST(RaftApplicationLogTest, RoundTripsNativePayloadsInCommitBatch) {
  std::vector<rusty::Arc<janus::TpcCommitCommand>> commits;
  commits.push_back(MakeCommit("first", 2, 100));
  commits.push_back(MakeCommit("second", 3, 101));
  janus::TpcBatchCommand batch;
  batch.AddCmds(commits);

  const janus::Command incoming = RoundTrip(
      janus::Command{rusty::Arc<janus::TpcBatchCommand>::make(
          std::move(batch))});
  const auto decoded = rrr::marshallable_cast<janus::TpcBatchCommand>(incoming);
  ASSERT_TRUE(decoded.is_some());
  ASSERT_EQ(decoded.unwrap()->cmds_.size(), 2u);

  for (size_t i = 0; i < decoded.unwrap()->cmds_.size(); ++i) {
    const auto raw = rrr::marshallable_cast<janus::LogEntry>(
        decoded.unwrap()->cmds_[i]->cmd_);
    ASSERT_TRUE(raw.is_some());
    const char* payload = nullptr;
    int length = -1;
    uint32_t partition_id = 0;
    ASSERT_TRUE(janus::raft::DecodeApplicationLog(
        raw.unwrap()->log_entry, &payload, &length, &partition_id));
    EXPECT_EQ(partition_id, static_cast<uint32_t>(i + 2));
    EXPECT_EQ(std::string(payload, length), i == 0 ? "first" : "second");
  }
}

}  // namespace
