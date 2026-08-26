#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <type_traits>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "deptran/raft/file_snapshot_manager.hpp"
#include "deptran/raft/memory_snapshot_manager.hpp"
#include "deptran/raft/snapshot_format.hpp"

using namespace janus::raft;

static_assert(sizeof(SnapshotCompression) == 1);
static_assert(alignof(SnapshotCompression) == 1);
static_assert(std::is_same_v<
              std::underlying_type_t<SnapshotCompression>, uint8_t>);
static_assert(std::is_trivially_copyable_v<SnapshotCompression>);
static_assert(static_cast<uint8_t>(SnapshotCompression::NONE) == 0);
static_assert(static_cast<uint8_t>(SnapshotCompression::SNAPPY) == 1);
static_assert(static_cast<uint8_t>(SnapshotCompression::ZSTD) == 2);
static_assert(SnapshotCompression{} == SnapshotCompression::NONE);
static_assert(sizeof(SnapshotChecksumType) == 1);
static_assert(alignof(SnapshotChecksumType) == 1);
static_assert(std::is_same_v<
              std::underlying_type_t<SnapshotChecksumType>, uint8_t>);
static_assert(std::is_trivially_copyable_v<SnapshotChecksumType>);
static_assert(static_cast<uint8_t>(SnapshotChecksumType::NONE) == 0);
static_assert(static_cast<uint8_t>(SnapshotChecksumType::CRC32) == 1);
static_assert(static_cast<uint8_t>(SnapshotChecksumType::SHA256) == 2);
static_assert(SnapshotChecksumType{} == SnapshotChecksumType::NONE);
static_assert(sizeof(SnapshotHeader) == 52);
static_assert(offsetof(SnapshotHeader, compression) == 20);
static_assert(offsetof(SnapshotHeader, checksum_type) == 21);
static_assert(offsetof(SnapshotHeader, header_crc) == 46);
static_assert(offsetof(SnapshotHeader, padding) == 50);

namespace {

constexpr std::array<uint32_t, 256> MakeCRC32ProbeTable() {
  std::array<uint32_t, 256> table{};
  for (uint32_t i = 0; i < table.size(); ++i) {
    uint32_t value = i;
    for (int bit = 0; bit < 8; ++bit) {
      value = (value >> 1) ^
              ((value & 1u) != 0 ? 0xEDB88320u : 0u);
    }
    table[i] = value;
  }
  return table;
}

inline constexpr auto kCRC32ProbeTable = MakeCRC32ProbeTable();

// Exact pre-migration update loop, parameterized by a table so the DSL-owned
// loop can be checked independently of CRC32's incumbent private table.
void IncumbentCRC32Update(uint32_t* crc,
                          const uint8_t* data,
                          size_t size,
                          const uint32_t* table) {
  for (size_t i = 0; i < size; ++i) {
    const uint8_t byte = data[i];
    *crc = table[(*crc ^ byte) & 0xFFu] ^ (*crc >> 8);
  }
}

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

class ScopedSnapshotDirectory {
 public:
  // @unsafe - Creates a uniquely named private directory with mkdtemp.
  ScopedSnapshotDirectory() {
    char path[] = "/tmp/mako_file_snapshot_test_XXXXXX";
    const char* created = ::mkdtemp(path);
    if (created != nullptr) {
      path_ = created;
    }
  }

  // @unsafe - Removes only flat files from the private directory created by
  // this object, then removes that directory.
  ~ScopedSnapshotDirectory() {
    if (path_.empty()) {
      return;
    }
    DIR* dir = ::opendir(path_.c_str());
    if (dir != nullptr) {
      while (struct dirent* entry = ::readdir(dir)) {
        const std::string name(entry->d_name);
        if (name != "." && name != "..") {
          const std::string file_path = path_ + "/" + name;
          ::unlink(file_path.c_str());
        }
      }
      ::closedir(dir);
    }
    ::rmdir(path_.c_str());
  }

  ScopedSnapshotDirectory(const ScopedSnapshotDirectory&) = delete;
  ScopedSnapshotDirectory& operator=(const ScopedSnapshotDirectory&) = delete;

  // @lifetime: (&'a) -> &'a
  const std::string& path() const { return path_; }

 private:
  std::string path_;
};

}  // namespace

TEST(SnapshotFormatTest, CRC32KnownBytesAndIncrementalUpdates) {
  CRC32 empty;
  empty.Update(nullptr, 0);
  EXPECT_EQ(empty.Finalize(), 0u);
  EXPECT_EQ(CRC32::Calculate(nullptr, 0), 0u);

  constexpr char canonical[] = "123456789";
  EXPECT_EQ(CRC32::Calculate(canonical, sizeof(canonical) - 1),
            0xCBF43926u);

  constexpr std::array<uint8_t, 5> high_bytes{
      0x00, 0x7f, 0x80, 0xfe, 0xff};
  // Pin the incumbent table rather than silently changing persisted snapshot
  // checksums. Correcting its legacy non-IEEE entries needs a format/version
  // migration separate from this source-ownership change.
  EXPECT_EQ(CRC32::Calculate(
                reinterpret_cast<const char*>(high_bytes.data()),
                high_bytes.size()),
            0x009CE935u);

  CRC32 split;
  split.Update(canonical, 4);
  split.Update(canonical + 4, sizeof(canonical) - 1 - 4);
  EXPECT_EQ(split.Finalize(), 0xCBF43926u);
}

TEST(SnapshotFormatTest, CRC32UpdateLoopMatchesIncumbentAcrossRandomChunking) {
  std::mt19937_64 random(0x524146545f435243ULL);
  for (size_t trial = 0; trial < 1000; ++trial) {
    const size_t size = static_cast<size_t>(random() % 4097);
    std::vector<uint8_t> bytes(size);
    for (uint8_t& byte : bytes) {
      byte = static_cast<uint8_t>(random());
    }

    uint32_t actual = snapshot_crc32_initial();
    uint32_t expected = snapshot_crc32_initial();
    size_t offset = 0;
    while (offset < size) {
      const size_t chunk = std::min(
          size - offset, static_cast<size_t>((random() % 97) + 1));
      snapshot_crc32_update_buffer(
          &actual, bytes.data() + offset, chunk, kCRC32ProbeTable.data());
      IncumbentCRC32Update(
          &expected, bytes.data() + offset, chunk, kCRC32ProbeTable.data());
      offset += chunk;
    }
    EXPECT_EQ(actual, expected) << "trial=" << trial;
  }
}

TEST(SnapshotFormatTest, CRC32PreservesSelfAliasedByteObservation) {
  for (size_t offset = 0; offset < sizeof(uint32_t); ++offset) {
    for (size_t size = 0; size <= sizeof(uint32_t) - offset; ++size) {
      uint32_t actual = snapshot_crc32_initial();
      uint32_t expected = snapshot_crc32_initial();
      snapshot_crc32_update_buffer(
          &actual, reinterpret_cast<const uint8_t*>(&actual) + offset, size,
          kCRC32ProbeTable.data());
      IncumbentCRC32Update(
          &expected, reinterpret_cast<const uint8_t*>(&expected) + offset,
          size, kCRC32ProbeTable.data());
      EXPECT_EQ(actual, expected)
          << "offset=" << offset << " size=" << size;
    }
  }
}

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

// @unsafe - Exercises durable file publication in a private temp directory.
TEST(FileSnapshotManagerTest, PublishesRoundTripWithoutTemporaryFile) {
  ScopedSnapshotDirectory directory;
  ASSERT_FALSE(directory.path().empty());

  SnapshotConfig config;
  config.storage_path = directory.path();
  config.max_snapshots = 3;
  FileSnapshotManager mgr(config);

  std::string payload(256 * 1024, '\0');
  for (size_t i = 0; i < payload.size(); ++i) {
    payload[i] = static_cast<char>(i % 251);
  }

  auto writer = mgr.BeginSnapshot(/*last_index=*/42, /*last_term=*/7);
  ASSERT_NE(writer, nullptr);
  ASSERT_TRUE(writer->Write(payload.data(), 17));
  ASSERT_TRUE(writer->Write(payload.data() + 17, payload.size() - 17));
  ASSERT_TRUE(writer->Finalize());

  const std::string final_path =
      directory.path() + "/snapshot_42_7.snap";
  const std::string temp_path = final_path + ".tmp";
  EXPECT_EQ(::access(final_path.c_str(), F_OK), 0);
  EXPECT_NE(::access(temp_path.c_str(), F_OK), 0);

  SnapshotMetadata metadata;
  std::string loaded;
  ASSERT_TRUE(mgr.LoadLatestSnapshot(&metadata, &loaded));
  EXPECT_EQ(metadata.last_included_index, 42u);
  EXPECT_EQ(metadata.last_included_term, 7u);
  EXPECT_EQ(loaded, payload);
}

// @unsafe - Exercises retention unlink and directory-barrier paths.
TEST(FileSnapshotManagerTest, RetentionAndExplicitDeletionKeepExpectedFiles) {
  ScopedSnapshotDirectory directory;
  ASSERT_FALSE(directory.path().empty());

  SnapshotConfig config;
  config.storage_path = directory.path();
  config.max_snapshots = 2;
  FileSnapshotManager mgr(config);

  ASSERT_TRUE(mgr.TakeSnapshot(10, 1, "ten", 3));
  ASSERT_TRUE(mgr.TakeSnapshot(20, 2, "twenty", 6));
  ASSERT_TRUE(mgr.TakeSnapshot(30, 3, "thirty", 6));

  auto snapshots = mgr.ListSnapshots();
  ASSERT_EQ(snapshots.size(), 2u);
  EXPECT_EQ(snapshots[0].last_included_index, 30u);
  EXPECT_EQ(snapshots[1].last_included_index, 20u);
  EXPECT_NE(::access(
                (directory.path() + "/snapshot_10_1.snap").c_str(), F_OK),
            0);

  EXPECT_EQ(mgr.PruneSnapshots(30), 1u);
  snapshots = mgr.ListSnapshots();
  ASSERT_EQ(snapshots.size(), 1u);
  EXPECT_EQ(snapshots[0].last_included_index, 30u);

  EXPECT_EQ(mgr.DeleteAllSnapshots(), 1u);
  EXPECT_TRUE(mgr.ListSnapshots().empty());
}

// @unsafe - Creates a deliberately truncated on-disk snapshot.
TEST(FileSnapshotManagerTest, RejectsTruncatedPublishedSnapshot) {
  ScopedSnapshotDirectory directory;
  ASSERT_FALSE(directory.path().empty());

  std::string serialized;
  const std::string payload = "complete payload";
  ASSERT_TRUE(SnapshotFormat::Serialize(
      55, 8, payload.data(), payload.size(), &serialized));
  ASSERT_GT(serialized.size(), 1u);

  const std::string path =
      directory.path() + "/snapshot_55_8.snap";
  const int fd = ::open(
      path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  ASSERT_GE(fd, 0);
  ASSERT_TRUE(file_snapshot_write_all(
      fd, serialized.data(), serialized.size() - 1));
  ASSERT_TRUE(file_snapshot_fsync(fd));
  ASSERT_TRUE(file_snapshot_close(fd));
  ASSERT_TRUE(file_snapshot_sync_directory(directory.path()));

  FileSnapshotReader reader(path);
  EXPECT_FALSE(reader.IsValid());

  SnapshotConfig config;
  config.storage_path = directory.path();
  FileSnapshotManager mgr(config);
  SnapshotMetadata metadata;
  std::string loaded;
  EXPECT_FALSE(mgr.LoadLatestSnapshot(&metadata, &loaded));
}

// @unsafe - Creates a sparse oversized file without allocating its payload.
TEST(FileSnapshotManagerTest, RejectsOversizedFileBeforeReadAllocation) {
  ScopedSnapshotDirectory directory;
  ASSERT_FALSE(directory.path().empty());

  const std::string path =
      directory.path() + "/snapshot_56_8.snap";
  const int fd = ::open(
      path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  ASSERT_GE(fd, 0);
  ASSERT_EQ(::ftruncate(
                fd, static_cast<off_t>(
                        SnapshotFormat::MAX_SERIALIZED_SIZE + 1)),
            0);
  ASSERT_TRUE(file_snapshot_fsync(fd));
  ASSERT_TRUE(file_snapshot_close(fd));
  ASSERT_TRUE(file_snapshot_sync_directory(directory.path()));

  FileSnapshotReader reader(path);
  EXPECT_FALSE(reader.IsValid());

  SnapshotConfig config;
  config.storage_path = directory.path();
  FileSnapshotManager mgr(config);
  SnapshotMetadata metadata;
  std::string loaded;
  EXPECT_FALSE(mgr.LoadLatestSnapshot(&metadata, &loaded));

  auto writer = mgr.BeginSnapshot(57, 8);
  ASSERT_NE(writer, nullptr);
  EXPECT_FALSE(writer->Write(
      "x", SnapshotFormat::MAX_PAYLOAD_SIZE + 1));
  EXPECT_EQ(writer->GetOffset(), 0u);
  EXPECT_TRUE(writer->Abort());
}

// @unsafe - Creates a regular file where a snapshot directory is required.
TEST(FileSnapshotManagerTest, RejectsInvalidStorageDirectory) {
  ScopedSnapshotDirectory directory;
  ASSERT_FALSE(directory.path().empty());

  const std::string storage_path = directory.path() + "/not_a_directory";
  const int fd = ::open(
      storage_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  ASSERT_GE(fd, 0);
  ASSERT_TRUE(file_snapshot_close(fd));

  SnapshotConfig config;
  config.storage_path = storage_path;
  FileSnapshotManager mgr(config);
  EXPECT_EQ(mgr.BeginSnapshot(1, 1), nullptr);
  EXPECT_FALSE(mgr.TakeSnapshot(1, 1, "x", 1));
  EXPECT_EQ(mgr.BeginLoad(SnapshotMetadata{}), nullptr);

  SnapshotMetadata metadata;
  std::string loaded;
  EXPECT_FALSE(mgr.LoadLatestSnapshot(&metadata, &loaded));
  EXPECT_TRUE(mgr.GetLatestSnapshot().is_none());
  EXPECT_TRUE(mgr.ListSnapshots().empty());
  EXPECT_FALSE(mgr.HasSnapshotAtOrAfter(1));
  EXPECT_EQ(mgr.PruneSnapshots(2), 0u);
  EXPECT_EQ(mgr.DeleteAllSnapshots(), 0u);
}

// @unsafe - Exercises fail-closed configuration and filename parsing.
TEST(FileSnapshotManagerTest, RejectsZeroRetentionAndIgnoresOverflowFilename) {
  ScopedSnapshotDirectory directory;
  ASSERT_FALSE(directory.path().empty());

  SnapshotConfig invalid_config;
  invalid_config.storage_path = directory.path();
  invalid_config.max_snapshots = 0;
  FileSnapshotManager invalid_manager(invalid_config);
  EXPECT_FALSE(invalid_manager.IsStorageReady());
  EXPECT_EQ(invalid_manager.BeginSnapshot(1, 1), nullptr);

  const std::string overflow_name =
      directory.path() + "/snapshot_" + std::string(40, '9') +
      "_1.snap";
  const int fd = ::open(
      overflow_name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  ASSERT_GE(fd, 0);
  ASSERT_TRUE(file_snapshot_close(fd));

  SnapshotConfig valid_config;
  valid_config.storage_path = directory.path();
  FileSnapshotManager valid_manager(valid_config);
  EXPECT_TRUE(valid_manager.ListSnapshots().empty());
  EXPECT_TRUE(valid_manager.GetLatestSnapshot().is_none());
}

// @unsafe - Lets FileSnapshotManager create and validate a missing child
// directory, then removes only that test-owned directory.
TEST(FileSnapshotManagerTest, CreatesMissingStorageDirectory) {
  ScopedSnapshotDirectory directory;
  ASSERT_FALSE(directory.path().empty());

  EXPECT_EQ(file_snapshot_parent_directory("snapshot.snap"), ".");
  EXPECT_EQ(file_snapshot_parent_directory("/snapshot.snap"), "/");
  EXPECT_EQ(file_snapshot_parent_directory("/tmp/snapshots/"), "/tmp");

  const std::string storage_path = directory.path() + "/snapshots";
  SnapshotConfig config;
  config.storage_path = storage_path;
  FileSnapshotManager mgr(config);

  ASSERT_TRUE(mgr.TakeSnapshot(9, 2, "durable", 7));
  SnapshotMetadata metadata;
  std::string loaded;
  ASSERT_TRUE(mgr.LoadLatestSnapshot(&metadata, &loaded));
  EXPECT_EQ(metadata.last_included_index, 9u);
  EXPECT_EQ(loaded, "durable");
  EXPECT_EQ(mgr.DeleteAllSnapshots(), 1u);
  EXPECT_EQ(::rmdir(storage_path.c_str()), 0);
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
  EXPECT_EQ(static_cast<uint64_t>(header.last_index), 42u);
  EXPECT_EQ(static_cast<uint64_t>(header.last_term), 7u);

  uint32_t stored_crc = 0;
  std::memcpy(&stored_crc,
              encoded.data() + sizeof(SnapshotHeader) + payload.size(),
              sizeof(stored_crc));
  EXPECT_EQ(stored_crc, CRC32::Calculate(payload.data(), payload.size()));
}

TEST(SnapshotFormatTest, RawModeChecksRejectUnsupportedValues) {
  const std::string payload("raw\0bytes", 9);
  std::string encoded;
  ASSERT_TRUE(SnapshotFormat::Serialize(
      9, 3, payload.data(), payload.size(), &encoded,
      SnapshotCompression::NONE, SnapshotChecksumType::NONE));

  uint64_t last_index = 0;
  uint64_t last_term = 0;
  std::string decoded;
  EXPECT_TRUE(SnapshotFormat::Deserialize(
      encoded.data(), encoded.size(), &last_index, &last_term, &decoded));
  EXPECT_EQ(last_index, 9u);
  EXPECT_EQ(last_term, 3u);
  EXPECT_EQ(decoded, payload);

  for (uint8_t raw_checksum : {
           static_cast<uint8_t>(SnapshotChecksumType::SHA256),
           static_cast<uint8_t>(0xfe)}) {
    std::string candidate = WithRawSnapshotModes(
        encoded, static_cast<uint8_t>(SnapshotCompression::NONE),
        raw_checksum);
    EXPECT_FALSE(SnapshotFormat::Deserialize(
        candidate.data(), candidate.size(), &last_index, &last_term,
        &decoded));
    SnapshotHeader header{};
    EXPECT_FALSE(SnapshotFormat::GetHeader(
        candidate.data(), candidate.size(), &header));
  }

  std::string unknown_compression = WithRawSnapshotModes(
      encoded, 0xfe, static_cast<uint8_t>(SnapshotChecksumType::NONE));
  EXPECT_FALSE(SnapshotFormat::Deserialize(
      unknown_compression.data(), unknown_compression.size(), &last_index,
      &last_term, &decoded));
}

TEST(SnapshotFormatTest, RejectsOverflowSizeMismatchAndTrailingBytes) {
  const std::string payload = "bounded snapshot";
  std::string encoded;
  ASSERT_TRUE(SnapshotFormat::Serialize(
      71, 9, payload.data(), payload.size(), &encoded));

  uint64_t last_index = 0;
  uint64_t last_term = 0;
  std::string decoded;

  std::string trailing = encoded;
  trailing.push_back('\0');
  EXPECT_FALSE(SnapshotFormat::Deserialize(
      trailing.data(), trailing.size(), &last_index, &last_term, &decoded));
  SnapshotHeader inspected{};
  inspected.last_index = 999;
  EXPECT_FALSE(SnapshotFormat::GetHeader(
      trailing.data(), trailing.size(), &inspected));
  EXPECT_EQ(inspected.last_index, 999u);

  SnapshotHeader header{};
  std::memcpy(&header, encoded.data(), sizeof(header));
  header.header_size = static_cast<uint32_t>(sizeof(header) + 1);
  header.header_crc = CRC32::Calculate(
      reinterpret_cast<const char*>(&header), 44);
  std::string bad_header_size = encoded;
  std::memcpy(bad_header_size.data(), &header, sizeof(header));
  EXPECT_FALSE(SnapshotFormat::Deserialize(
      bad_header_size.data(), bad_header_size.size(), &last_index, &last_term,
      &decoded));

  std::memcpy(&header, encoded.data(), sizeof(header));
  header.data_size =
      static_cast<uint64_t>(SnapshotFormat::MAX_PAYLOAD_SIZE) + 1;
  header.header_crc = CRC32::Calculate(
      reinterpret_cast<const char*>(&header), 44);
  std::string oversized_header = encoded;
  std::memcpy(oversized_header.data(), &header, sizeof(header));
  EXPECT_FALSE(SnapshotFormat::Deserialize(
      oversized_header.data(), oversized_header.size(), &last_index,
      &last_term, &decoded));

  header.data_size = UINT64_MAX;
  header.header_crc = CRC32::Calculate(
      reinterpret_cast<const char*>(&header), 44);
  std::string overflowing_header = encoded;
  std::memcpy(overflowing_header.data(), &header, sizeof(header));
  EXPECT_FALSE(SnapshotFormat::Deserialize(
      overflowing_header.data(), overflowing_header.size(), &last_index,
      &last_term, &decoded));

  EXPECT_FALSE(SnapshotFormat::Deserialize(
      encoded.data(), SnapshotFormat::MAX_SERIALIZED_SIZE + 1,
      &last_index, &last_term, &decoded));
}

TEST(SnapshotFormatTest, SerializeRejectsInvalidPointersModesAndOversize) {
  std::string output = "unchanged";
  EXPECT_FALSE(SnapshotFormat::Serialize(
      1, 1, nullptr, 1, &output));
  EXPECT_EQ(output, "unchanged");

  EXPECT_FALSE(SnapshotFormat::Serialize(
      1, 1, "x", SnapshotFormat::MAX_PAYLOAD_SIZE + 1, &output));
  EXPECT_EQ(output, "unchanged");

  EXPECT_FALSE(SnapshotFormat::Serialize(
      1, 1, "x", 1, &output, SnapshotCompression::NONE,
      SnapshotChecksumType::SHA256));
  EXPECT_EQ(output, "unchanged");

  EXPECT_FALSE(SnapshotFormat::Serialize(
      1, 1, "x", 1, &output, SnapshotCompression::NONE,
      static_cast<SnapshotChecksumType>(0xfe)));
  EXPECT_EQ(output, "unchanged");
}
