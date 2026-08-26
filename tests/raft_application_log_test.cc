#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "deptran/raft/application_log.h"
#include "deptran/replication_log_entry.h"
#include "deptran/tpc_command.h"
#include "deptran/view_data.h"
#include "srpc/srpc.hpp"

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
  srpc::BufferSink sink;
  srpc::BinaryWriteArchive writer{srpc::make_sink_proxy_buffer(&sink)};
  srpc::Serialize_::serialize(outgoing, writer);

  janus::Command incoming;
  srpc::BufferSource source(sink.bytes.data(), sink.bytes.len());
  srpc::BinaryReadArchive reader{srpc::make_source_proxy_buffer(&source)};
  srpc::Deserialize_::deserialize(incoming, reader);
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

  const auto commit = srpc::marshallable_cast<janus::TpcCommitCommand>(incoming);
  ASSERT_TRUE(commit.is_some());
  EXPECT_EQ(commit.unwrap()->tx_id_, 42u);
  EXPECT_EQ(commit.unwrap()->term, 11);
  const auto raw = srpc::marshallable_cast<janus::LogEntry>(
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
  const auto decoded = srpc::marshallable_cast<janus::TpcBatchCommand>(incoming);
  ASSERT_TRUE(decoded.is_some());
  ASSERT_EQ(decoded.unwrap()->cmds_.size(), 2u);

  for (size_t i = 0; i < decoded.unwrap()->cmds_.size(); ++i) {
    const auto raw = srpc::marshallable_cast<janus::LogEntry>(
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

TEST(RaftApplicationLogTest, ViewDataKindAndWireFormatRemainStable) {
  static_assert(janus::ViewData::static_kind() == 16);
  janus::EnsureViewDataRegistered();

  janus::ViewData view_data;
  view_data.view_.n_ = 3;
  view_data.view_.view_id_ = 9;
  view_data.view_.timestamp_ = 12345;
  view_data.view_.leaders_ = {1, 2, 1};
  view_data.partition_id_ = 7;

  const janus::Command outgoing{
      rusty::Arc<janus::ViewData>::make(view_data)};
  srpc::BufferSink sink;
  srpc::BinaryWriteArchive writer{srpc::make_sink_proxy_buffer(&sink)};
  srpc::Serialize_::serialize(outgoing, writer);

  // Kind 16's v32 tag is followed by the historical payload layout: n, view
  // id, timestamp, leader count, leaders, then partition id. The archive uses
  // fixed-width native little-endian integers on the supported x86_64 ABI.
  const std::vector<uint8_t> expected{
      0x10,
      0x03, 0x00, 0x00, 0x00,
      0x09, 0x00, 0x00, 0x00,
      0x39, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x03, 0x00, 0x00, 0x00,
      0x01, 0x00, 0x00, 0x00,
      0x02, 0x00, 0x00, 0x00,
      0x01, 0x00, 0x00, 0x00,
      0x07, 0x00, 0x00, 0x00};
  ASSERT_EQ(sink.bytes.len(), expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ(sink.bytes[i], expected[i]) << "byte offset " << i;
  }

  janus::Command incoming;
  srpc::BufferSource source(sink.bytes.data(), sink.bytes.len());
  srpc::BinaryReadArchive reader{srpc::make_source_proxy_buffer(&source)};
  srpc::Deserialize_::deserialize(incoming, reader);
  EXPECT_TRUE(source.eof());

  const auto decoded = srpc::marshallable_cast<janus::ViewData>(incoming);
  ASSERT_TRUE(decoded.is_some());
  EXPECT_EQ(decoded.unwrap()->view_.n_, 3);
  EXPECT_EQ(decoded.unwrap()->view_.view_id_, 9u);
  EXPECT_EQ(decoded.unwrap()->view_.timestamp_, 12345u);
  EXPECT_EQ(decoded.unwrap()->view_.leaders_,
            (std::vector<int>{1, 2, 1}));
  EXPECT_EQ(decoded.unwrap()->partition_id_, 7u);
}

}  // namespace
