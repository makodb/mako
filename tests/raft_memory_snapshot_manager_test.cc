#include <cstddef>
#include <cstring>
#include <type_traits>

#include <gtest/gtest.h>

#include "deptran/raft/memory_snapshot_manager.hpp"
#include "deptran/raft/snapshot_format.hpp"

using namespace janus::raft;

static_assert(sizeof(SnapshotCompression) == 1);
static_assert(alignof(SnapshotCompression) == 1);
static_assert(sizeof(SnapshotChecksumType) == 1);
static_assert(alignof(SnapshotChecksumType) == 1);
static_assert(sizeof(SnapshotHeader) == 52);
static_assert(offsetof(SnapshotHeader, compression) == 20);
static_assert(offsetof(SnapshotHeader, checksum_type) == 21);
static_assert(offsetof(SnapshotHeader, header_crc) == 46);
static_assert(offsetof(SnapshotHeader, padding) == 50);

namespace {

std::string WithRawSnapshotModes(std::string encoded,
                                 uint8_t compression,
                                 uint8_t checksum_type) {
  SnapshotHeader header{};
  std::memcpy(&header, encoded.data(), sizeof(header));
  header.compression = compression;
  header.checksum_type = checksum_type;
  header.header_crc = CRC32::Calculate(
      reinterpret_cast<const char*>(&header), 44);
  std::memcpy(encoded.data(), &header, sizeof(header));
  return encoded;
}

}  // namespace

TEST(MemorySnapshotManagerTest, TakeLoadRoundTrip) {
  MemorySnapshotManager mgr;
  EXPECT_FALSE(mgr.GetLatestSnapshot().is_some());

  const std::string payload = "hello snapshot";
  ASSERT_TRUE(mgr.TakeSnapshot(/*last_index=*/42, /*last_term=*/7,
                               payload.data(), payload.size()));

  auto meta = mgr.GetLatestSnapshot();
  ASSERT_TRUE(meta.is_some());
  EXPECT_EQ(meta.unwrap().last_included_index, 42u);

  SnapshotMetadata meta_out;
  std::string data_out;
  ASSERT_TRUE(mgr.LoadLatestSnapshot(&meta_out, &data_out));
  EXPECT_EQ(meta_out.last_included_term, 7u);
  EXPECT_EQ(data_out, payload);
  EXPECT_TRUE(mgr.HasSnapshotAtOrAfter(42));
  EXPECT_FALSE(mgr.HasSnapshotAtOrAfter(43));
}

TEST(MemorySnapshotManagerTest, WriterThenReader) {
  MemorySnapshotManager mgr;
  auto w = mgr.BeginSnapshot(100, 5);
  ASSERT_NE(w, nullptr);
  const char chunk1[] = "abc";
  const char chunk2[] = "def";
  ASSERT_TRUE(w->Write(chunk1, 3));
  ASSERT_TRUE(w->Write(chunk2, 3));
  ASSERT_TRUE(w->Finalize());

  auto r = mgr.BeginLoad(SnapshotMetadata{});
  ASSERT_NE(r, nullptr);
  char buf[16] = {};
  size_t got = 0;
  ASSERT_TRUE(r->Read(buf, sizeof(buf), &got));
  EXPECT_EQ(got, 6u);
  EXPECT_EQ(std::string(buf, got), "abcdef");
  EXPECT_TRUE(r->IsComplete());
}

TEST(MemorySnapshotManagerTest, PruneAndDelete) {
  MemorySnapshotManager mgr;
  mgr.TakeSnapshot(10, 1, "x", 1);
  EXPECT_EQ(mgr.PruneSnapshots(20), 1u);      // below threshold -> pruned
  EXPECT_FALSE(mgr.GetLatestSnapshot().is_some());
  mgr.TakeSnapshot(30, 2, "y", 1);
  EXPECT_EQ(mgr.DeleteAllSnapshots(), 1u);
  EXPECT_FALSE(mgr.GetLatestSnapshot().is_some());
}

TEST(SnapshotFormatTest, DefaultModesPreserveWireBytesAndChecksum) {
  const std::string payload("a\0\xffz", 4);
  std::string encoded;
  ASSERT_TRUE(SnapshotFormat::Serialize(
      42, 7, payload.data(), payload.size(), &encoded));
  ASSERT_EQ(encoded.size(), sizeof(SnapshotHeader) + payload.size() + 4);

  SnapshotHeader header{};
  std::memcpy(&header, encoded.data(), sizeof(header));
  EXPECT_EQ(header.compression,
            static_cast<uint8_t>(SnapshotCompression::NONE));
  EXPECT_EQ(header.checksum_type,
            static_cast<uint8_t>(SnapshotChecksumType::CRC32));
  EXPECT_EQ(header.last_index, 42u);
  EXPECT_EQ(header.last_term, 7u);

  uint32_t stored_crc = 0;
  std::memcpy(&stored_crc,
              encoded.data() + sizeof(SnapshotHeader) + payload.size(),
              sizeof(stored_crc));
  EXPECT_EQ(stored_crc, CRC32::Calculate(payload.data(), payload.size()));
}

TEST(SnapshotFormatTest, RawModeChecksPreserveLegacyAcceptance) {
  const std::string payload("raw\0bytes", 9);
  std::string encoded;
  ASSERT_TRUE(SnapshotFormat::Serialize(
      9, 3, payload.data(), payload.size(), &encoded,
      SnapshotCompression::NONE, SnapshotChecksumType::NONE));

  for (uint8_t raw_checksum : {
           static_cast<uint8_t>(SnapshotChecksumType::SHA256),
           static_cast<uint8_t>(0xfe)}) {
    std::string candidate = WithRawSnapshotModes(
        encoded, static_cast<uint8_t>(SnapshotCompression::NONE),
        raw_checksum);
    uint64_t last_index = 0;
    uint64_t last_term = 0;
    std::string decoded;
    EXPECT_TRUE(SnapshotFormat::Deserialize(
        candidate.data(), candidate.size(), &last_index, &last_term,
        &decoded));
    EXPECT_EQ(last_index, 9u);
    EXPECT_EQ(last_term, 3u);
    EXPECT_EQ(decoded, payload);
  }

  std::string unknown_compression = WithRawSnapshotModes(
      encoded, 0xfe, static_cast<uint8_t>(SnapshotChecksumType::NONE));
  uint64_t last_index = 0;
  uint64_t last_term = 0;
  std::string decoded;
  EXPECT_FALSE(SnapshotFormat::Deserialize(
      unknown_compression.data(), unknown_compression.size(), &last_index,
      &last_term, &decoded));
}
