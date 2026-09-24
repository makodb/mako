#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "deptran/legacy_raft_log_payload.h"
#include "deptran/raft/log_storage.hpp"
#include "deptran/tpc_command.h"
#include "srpc/srpc.hpp"

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

std::string FromHex(const std::string& hex) {
  EXPECT_EQ(hex.size() % 2, 0u);
  auto nibble = [](char digit) -> uint8_t {
    if (digit >= '0' && digit <= '9') return digit - '0';
    if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
    ADD_FAILURE() << "invalid hex digit";
    return 0;
  };

  std::string bytes;
  bytes.reserve(hex.size() / 2);
  for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
    bytes.push_back(static_cast<char>((nibble(hex[i]) << 4) |
                                      nibble(hex[i + 1])));
  }
  return bytes;
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
  srpc::BufferSink sink;
  srpc::BinaryWriteArchive writer{srpc::make_sink_proxy_buffer(&sink)};
  entry.save(writer);
  return std::string(reinterpret_cast<const char*>(sink.bytes.data()),
                     sink.bytes.len());
}

TEST(LegacyRaftLogTest, FreezesCanonicalKind4RocksDbValue) {
  // Explicit registration both selects the MDB-free kind-4 reader and forces
  // its translation unit out of txlog_core's static archive.
  srpc::SerializableRegistry::reg<janus::TpcCommitCommand>(
      janus::TpcCommitCommand::static_kind());
  janus::EnsureLegacyRaftLogPayloadRegistered();

  const std::string encoded = FromHex(kGoldenHex);
  EXPECT_EQ(encoded.size(), 164u);
  EXPECT_EQ(ToHex(encoded), kGoldenHex);

  janus::raft::LogEntry decoded;
  srpc::BufferSource source(
      reinterpret_cast<const uint8_t*>(encoded.data()), encoded.size());
  srpc::BinaryReadArchive reader{srpc::make_source_proxy_buffer(&source)};
  decoded.load(reader);
  EXPECT_TRUE(source.eof());
  EXPECT_EQ(decoded.slot_id, 17u);
  EXPECT_EQ(decoded.term, 23);
  EXPECT_EQ(decoded.max_ballot_seen, 29);
  EXPECT_EQ(decoded.max_ballot_accepted, 31);
  EXPECT_TRUE(decoded.committed);
  EXPECT_FALSE(decoded.is_no_op);

  const auto commit =
      srpc::marshallable_cast<janus::TpcCommitCommand>(decoded.command);
  ASSERT_TRUE(commit.is_some());
  EXPECT_EQ(commit.unwrap()->tx_id_, 0x0102030405060708ULL);
  EXPECT_EQ(commit.unwrap()->term, 37);
  const auto legacy = srpc::marshallable_cast<janus::LegacyVecPieceData>(
      commit.unwrap()->cmd_);
  ASSERT_TRUE(legacy.is_some());
  ASSERT_EQ(legacy.unwrap()->pieces.size(), 1u);
  EXPECT_DOUBLE_EQ(legacy.unwrap()->time_sent_from_client, -1e9);
  EXPECT_EQ(legacy.unwrap()->is_recovery_command, 0);

  std::string payload;
  uint32_t partition_id = 0;
  ASSERT_TRUE(
      legacy.unwrap()->TryGetApplicationLog(&payload, &partition_id));
  EXPECT_EQ(partition_id, 0x89abcdefu);
  EXPECT_EQ(payload, LegacyPayload());

  EXPECT_EQ(Serialize(decoded), encoded);
}

}  // namespace
