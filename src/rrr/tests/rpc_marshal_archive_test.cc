// Workstream N Phase 1: byte-for-byte compatibility tests for the
// new BinaryWriteArchive / BinaryReadArchive against the existing
// `Marshal` operator<< / operator>> wire format.
//
// Strategy: encode each primitive (and std::string) via BOTH the old
// `Marshal` path AND the new `BinaryWriteArchive` + `BufferSink` path,
// then assert the two byte buffers are identical.
//
// Read side: encode via Marshal, then decode via BinaryReadArchive +
// BufferSource, and verify the value round-trips. (The reverse —
// encode via Archive, decode via Marshal — is implicitly verified by
// the byte-equality test plus Marshal's existing tests.)

#include <std_compat.hpp>

#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "../misc/marshal.hpp"
#include "../misc/marshal_archive.hpp"

namespace rrr {
namespace {

// Drain a Marshal into a contiguous byte vector for comparison.
std::vector<uint8_t> drain_marshal(Marshal& m) {
  std::vector<uint8_t> out;
  out.resize(m.content_size());
  if (!out.empty()) {
    auto got = m.read(out.data(), out.size());
    EXPECT_EQ(got, out.size());
  }
  return out;
}

std::vector<uint8_t> sink_to_vector(const BufferSink& sink) {
  std::vector<uint8_t> out;
  out.reserve(sink.bytes.len());
  for (size_t i = 0; i < sink.bytes.len(); ++i) {
    out.push_back(sink.bytes[i]);
  }
  return out;
}

template <typename T>
void check_byte_compat_write(const T& value) {
  Marshal old_m;
  old_m << value;
  auto old_bytes = drain_marshal(old_m);

  BufferSink sink;
  BinaryWriteArchive archive(&sink);
  archive << value;
  auto new_bytes = sink_to_vector(sink);

  ASSERT_EQ(old_bytes.size(), new_bytes.size())
      << "byte-length mismatch for type " << typeid(T).name();
  EXPECT_EQ(old_bytes, new_bytes)
      << "byte content mismatch for type " << typeid(T).name();
}

template <typename T>
void check_round_trip(const T& value) {
  // Encode via the OLD Marshal path. (Phase 1 commitment: the new
  // archive's read side must consume bytes the old Marshal produces.)
  Marshal old_m;
  old_m << value;
  auto bytes = drain_marshal(old_m);

  BufferSource source(bytes.data(), bytes.size());
  BinaryReadArchive reader(&source);
  T decoded{};
  reader >> decoded;

  EXPECT_EQ(decoded, value)
      << "round-trip mismatch for type " << typeid(T).name();
  EXPECT_TRUE(source.eof()) << "decoder did not consume all bytes";
}

// ---------------------------------------------------------------------------
// Fixed-width primitives.
// ---------------------------------------------------------------------------

TEST(MarshalArchiveByteCompat, Int8Boundary) {
  for (int8_t v : {std::numeric_limits<int8_t>::min(),
                   int8_t{-1}, int8_t{0}, int8_t{1},
                   std::numeric_limits<int8_t>::max()}) {
    check_byte_compat_write(v);
    check_round_trip(v);
  }
}

TEST(MarshalArchiveByteCompat, Int16Boundary) {
  for (int16_t v : {std::numeric_limits<int16_t>::min(),
                    int16_t{-1}, int16_t{0}, int16_t{1},
                    std::numeric_limits<int16_t>::max()}) {
    check_byte_compat_write(v);
    check_round_trip(v);
  }
}

TEST(MarshalArchiveByteCompat, Int32Boundary) {
  for (int32_t v : {std::numeric_limits<int32_t>::min(),
                    int32_t{-1}, int32_t{0}, int32_t{1}, int32_t{12345},
                    std::numeric_limits<int32_t>::max()}) {
    check_byte_compat_write(v);
    check_round_trip(v);
  }
}

TEST(MarshalArchiveByteCompat, Int64Boundary) {
  for (int64_t v : {std::numeric_limits<int64_t>::min(),
                    int64_t{-1}, int64_t{0}, int64_t{1},
                    int64_t{0x1234'5678'9ABC'DEF0LL},
                    std::numeric_limits<int64_t>::max()}) {
    check_byte_compat_write(v);
    check_round_trip(v);
  }
}

TEST(MarshalArchiveByteCompat, Uint8Boundary) {
  for (uint8_t v : {uint8_t{0}, uint8_t{1}, uint8_t{0x7F}, uint8_t{0x80},
                    std::numeric_limits<uint8_t>::max()}) {
    check_byte_compat_write(v);
    check_round_trip(v);
  }
}

TEST(MarshalArchiveByteCompat, Uint16Boundary) {
  for (uint16_t v : {uint16_t{0}, uint16_t{1},
                     std::numeric_limits<uint16_t>::max()}) {
    check_byte_compat_write(v);
    check_round_trip(v);
  }
}

TEST(MarshalArchiveByteCompat, Uint32Boundary) {
  for (uint32_t v : {uint32_t{0}, uint32_t{1}, uint32_t{0xDEADBEEF},
                     std::numeric_limits<uint32_t>::max()}) {
    check_byte_compat_write(v);
    check_round_trip(v);
  }
}

TEST(MarshalArchiveByteCompat, Uint64Boundary) {
  for (uint64_t v : {uint64_t{0}, uint64_t{1},
                     uint64_t{0xCAFEBABE'DEADBEEF},
                     std::numeric_limits<uint64_t>::max()}) {
    check_byte_compat_write(v);
    check_round_trip(v);
  }
}

TEST(MarshalArchiveByteCompat, DoubleBoundary) {
  // Use bit-pattern equality: NaN won't compare equal via operator==.
  // Pick values that have stable bit patterns.
  for (double v : {0.0, 1.0, -1.0, 3.14159265358979,
                   std::numeric_limits<double>::min(),
                   std::numeric_limits<double>::max()}) {
    check_byte_compat_write(v);
    check_round_trip(v);
  }
}

// ---------------------------------------------------------------------------
// Variable-length integers (SparseInt encoding).
//
// The boundaries chosen exercise each branch of the SparseInt encoding
// (1-byte / 2-byte / 5-byte for v32; 1/2/3/5/9 byte for v64).
// ---------------------------------------------------------------------------

TEST(MarshalArchiveByteCompat, V32Boundary) {
  for (i32 raw : {0, 1, 63, 64, 127, 128, 255, 256, 8191, 8192,
                  0x7FFFFFFF, -1, -64, -65, -8192, -8193,
                  std::numeric_limits<i32>::min()}) {
    v32 v(raw);

    // Byte compatibility on the write side.
    Marshal old_m;
    old_m << v;
    auto old_bytes = drain_marshal(old_m);

    BufferSink sink;
    BinaryWriteArchive archive(&sink);
    archive << v;
    auto new_bytes = sink_to_vector(sink);

    ASSERT_EQ(old_bytes.size(), new_bytes.size()) << "v32 raw=" << raw;
    EXPECT_EQ(old_bytes, new_bytes) << "v32 raw=" << raw;

    // Round-trip read.
    BufferSource source(old_bytes.data(), old_bytes.size());
    BinaryReadArchive reader(&source);
    v32 decoded;
    reader >> decoded;
    EXPECT_EQ(decoded.get(), raw) << "v32 round-trip raw=" << raw;
    EXPECT_TRUE(source.eof());
  }
}

TEST(MarshalArchiveByteCompat, V64Boundary) {
  for (i64 raw : {int64_t{0}, int64_t{1}, int64_t{63}, int64_t{64},
                  int64_t{8191}, int64_t{8192},
                  int64_t{0x7FFFFFFF}, int64_t{0x7FFFFFFF'FFFFFFFFLL},
                  int64_t{-1}, int64_t{-64}, int64_t{-65},
                  int64_t{-0x7FFFFFFF},
                  std::numeric_limits<int64_t>::min()}) {
    v64 v(raw);

    Marshal old_m;
    old_m << v;
    auto old_bytes = drain_marshal(old_m);

    BufferSink sink;
    BinaryWriteArchive archive(&sink);
    archive << v;
    auto new_bytes = sink_to_vector(sink);

    ASSERT_EQ(old_bytes.size(), new_bytes.size()) << "v64 raw=" << raw;
    EXPECT_EQ(old_bytes, new_bytes) << "v64 raw=" << raw;

    BufferSource source(old_bytes.data(), old_bytes.size());
    BinaryReadArchive reader(&source);
    v64 decoded;
    reader >> decoded;
    EXPECT_EQ(decoded.get(), raw) << "v64 round-trip raw=" << raw;
    EXPECT_TRUE(source.eof());
  }
}

// ---------------------------------------------------------------------------
// std::string — v64 length prefix + raw bytes.
// ---------------------------------------------------------------------------

TEST(MarshalArchiveByteCompat, StringEmpty) {
  std::string s = "";
  check_byte_compat_write(s);
  check_round_trip(s);
}

TEST(MarshalArchiveByteCompat, StringShort) {
  std::string s = "hello";
  check_byte_compat_write(s);
  check_round_trip(s);
}

TEST(MarshalArchiveByteCompat, StringWithEmbeddedNul) {
  // 5-byte string with a NUL in the middle. The wire format MUST
  // preserve it byte-for-byte (length-prefixed; no C-string treatment).
  std::string s("ab\0cd", 5);
  check_byte_compat_write(s);
  check_round_trip(s);
}

TEST(MarshalArchiveByteCompat, StringMedium) {
  std::string s(200, 'x');
  check_byte_compat_write(s);
  check_round_trip(s);
}

TEST(MarshalArchiveByteCompat, StringLongerThanV64Boundary) {
  // 100 KB string — exercises the multi-byte v64 length prefix path.
  std::string s(100 * 1024, 'a');
  check_byte_compat_write(s);
  check_round_trip(s);
}

// ---------------------------------------------------------------------------
// Composite write — multiple primitives in a row, byte-for-byte against
// Marshal's accumulated buffer.
// ---------------------------------------------------------------------------

TEST(MarshalArchiveByteCompat, CompositePrimitiveSequence) {
  // Encode a heterogenous sequence via both paths.
  auto encode_old = [&](Marshal& m) {
    int32_t a = 0x12345678;
    int64_t b = 0x1122334455667788LL;
    v64 c(8192);
    std::string d = "hello world";
    double e = 3.14;
    m << a << b << c << d << e;
  };
  auto encode_new = [&](BinaryWriteArchive& ar) {
    int32_t a = 0x12345678;
    int64_t b = 0x1122334455667788LL;
    v64 c(8192);
    std::string d = "hello world";
    double e = 3.14;
    ar << a << b << c << d << e;
  };

  Marshal old_m;
  encode_old(old_m);
  auto old_bytes = drain_marshal(old_m);

  BufferSink sink;
  BinaryWriteArchive archive(&sink);
  encode_new(archive);
  auto new_bytes = sink_to_vector(sink);

  ASSERT_EQ(old_bytes.size(), new_bytes.size());
  EXPECT_EQ(old_bytes, new_bytes);
}

// ---------------------------------------------------------------------------
// Source semantics — partial reads at EOF.
// ---------------------------------------------------------------------------

TEST(BufferSourceSemantics, EofReturnsZero) {
  uint8_t bytes[] = {1, 2, 3};
  BufferSource source(bytes, sizeof(bytes));

  uint8_t got[2];
  EXPECT_EQ(source.read(got, 2), 2u);
  EXPECT_EQ(source.remaining(), 1u);
  EXPECT_FALSE(source.eof());

  EXPECT_EQ(source.read(got, 2), 1u);  // partial read of last byte
  EXPECT_TRUE(source.eof());

  EXPECT_EQ(source.read(got, 2), 0u);  // no bytes left
}

TEST(BufferSinkSemantics, AccumulatesBytes) {
  BufferSink sink;
  uint32_t value = 0xDEADBEEF;
  sink.write(&value, sizeof(value));
  ASSERT_EQ(sink.bytes.len(), 4u);

  uint32_t reread;
  std::memcpy(&reread, &sink.bytes[0], 4);
  EXPECT_EQ(reread, value);
}

}  // namespace
}  // namespace rrr
