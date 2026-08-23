#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "deptran/procedure.h"
#include "deptran/raft/log_storage.hpp"
#include "deptran/tpc_command.h"
#include "rrr/rrr.hpp"

namespace {

// Captured from the pre-cleanup VecPieceData/SimpleCommand/mdb::Value writer
// on the supported x86_64 ABI. RocksDB stores these LogEntry::save bytes
// verbatim.
constexpr char kGoldenHex[] =
    "110000000000000017000000000000001d000000000000001f00000000000000"
    "010001030807060504030201ffffffff2500000000000000040100000000000000"
    "000000000000000000000000000000000000000000000000ffffffffffffffff00"
    "010000000000000000000000000000000003000000096f6c64006d616b6fffffff"
    "ffff0000000000efcdab890000000000000000000000000000000065cdcdc10000";

const std::string& LegacyPayload() {
  static const std::string payload{"old\0mako\xff", 9};
  return payload;
}

std::string ToHex(const std::string& bytes) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string hex;
  hex.reserve(bytes.size() * 2);
  for (unsigned char byte : bytes) {
    hex.push_back(digits[byte >> 4]);
    hex.push_back(digits[byte & 0x0f]);
  }
  return hex;
}

std::string Serialize(const janus::raft::LogEntry& entry) {
  rrr::BufferSink sink;
  rrr::BinaryWriteArchive writer{rrr::make_sink_proxy_buffer(&sink)};
  entry.save(writer);
  return std::string(reinterpret_cast<const char*>(sink.bytes.data()),
                     sink.bytes.len());
}

janus::raft::LogEntry MakeLegacyEntry() {
  auto piece = std::make_shared<janus::SimpleCommand>();
  piece->input.keys_.insert(0);
  (*piece->input.values_)[0] = mdb::Value(LegacyPayload());
  piece->partition_id_ = 0x89abcdefu;

  auto pieces = rusty::Arc<janus::VecPieceData>::make();
  pieces.get_mut().unwrap().sp_vec_piece_data_ =
      std::make_shared<std::vector<std::shared_ptr<janus::SimpleCommand>>>();
  pieces->sp_vec_piece_data_->push_back(std::move(piece));

  auto commit = rusty::Arc<janus::TpcCommitCommand>::make();
  auto& mut = commit.get_mut().unwrap();
  mut.tx_id_ = 0x0102030405060708ULL;
  mut.term = 37;
  mut.cmd_ = std::move(pieces);

  janus::raft::LogEntry entry;
  entry.slot_id = 17;
  entry.term = 23;
  entry.max_ballot_seen = 29;
  entry.max_ballot_accepted = 31;
  entry.command = std::move(commit);
  entry.committed = true;
  entry.is_no_op = false;
  return entry;
}

TEST(LegacyRaftLogTest, FreezesCanonicalKind4RocksDbValue) {
  const std::string encoded = Serialize(MakeLegacyEntry());
  EXPECT_EQ(encoded.size(), 164u);
  EXPECT_EQ(ToHex(encoded), kGoldenHex);

  janus::raft::LogEntry decoded;
  rrr::BufferSource source(
      reinterpret_cast<const uint8_t*>(encoded.data()), encoded.size());
  rrr::BinaryReadArchive reader{rrr::make_source_proxy_buffer(&source)};
  decoded.load(reader);
  EXPECT_TRUE(source.eof());
  EXPECT_EQ(decoded.slot_id, 17u);
  EXPECT_EQ(decoded.term, 23);
  EXPECT_EQ(decoded.max_ballot_seen, 29);
  EXPECT_EQ(decoded.max_ballot_accepted, 31);
  EXPECT_TRUE(decoded.committed);
  EXPECT_FALSE(decoded.is_no_op);

  const auto commit =
      rrr::marshallable_cast<janus::TpcCommitCommand>(decoded.command);
  ASSERT_TRUE(commit.is_some());
  EXPECT_EQ(commit.unwrap()->tx_id_, 0x0102030405060708ULL);
  EXPECT_EQ(commit.unwrap()->term, 37);
  const auto pieces =
      rrr::marshallable_cast<janus::VecPieceData>(commit.unwrap()->cmd_);
  ASSERT_TRUE(pieces.is_some());
  ASSERT_TRUE(pieces.unwrap()->sp_vec_piece_data_);
  ASSERT_EQ(pieces.unwrap()->sp_vec_piece_data_->size(), 1u);
  const auto& piece = pieces.unwrap()->sp_vec_piece_data_->front();
  ASSERT_TRUE(piece);
  EXPECT_EQ(piece->partition_id_, 0x89abcdefu);
  ASSERT_TRUE(piece->input.values_);
  ASSERT_EQ(piece->input.values_->size(), 1u);
  EXPECT_EQ(piece->input.values_->at(0).get_str(), LegacyPayload());

  EXPECT_EQ(Serialize(decoded), encoded);
}

}  // namespace
