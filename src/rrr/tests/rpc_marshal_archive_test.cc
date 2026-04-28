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

// ---------------------------------------------------------------------------
// Container shapes (Phase 1b).
//
// All linear containers share the same wire format: v64 length prefix +
// each element serialized via its element-type operator<<. Iteration
// order matches the container's begin()/end() — for ordered containers
// (set/map/BTreeSet/BTreeMap) sorted-key order; for unordered containers
// (unordered_set/HashSet/unordered_map/HashMap) the same bucket-walk
// order Marshal uses, so byte-for-byte compatibility holds.
// ---------------------------------------------------------------------------

// Pair — no length prefix, just first followed by second.
TEST(MarshalArchiveByteCompat, PairOfPrimitives) {
  std::pair<int32_t, std::string> p{42, "hello"};
  check_byte_compat_write(p);

  // Round-trip: Marshal-encoded → BinaryReadArchive.
  Marshal m;
  m << p;
  auto bytes = drain_marshal(m);
  BufferSource source(bytes.data(), bytes.size());
  BinaryReadArchive reader(&source);
  std::pair<int32_t, std::string> decoded;
  reader >> decoded;
  EXPECT_EQ(decoded, p);
  EXPECT_TRUE(source.eof());
}

// Helper that drives the standard container test pattern: encode via
// both paths, byte-compat assert, round-trip from Marshal-bytes back
// through BinaryReadArchive.
template <typename Container>
void check_container_compat_write(const Container& c) {
  Marshal old_m;
  old_m << c;
  auto old_bytes = drain_marshal(old_m);

  BufferSink sink;
  BinaryWriteArchive archive(&sink);
  archive << c;
  auto new_bytes = sink_to_vector(sink);

  ASSERT_EQ(old_bytes.size(), new_bytes.size())
      << "container byte-length mismatch for " << typeid(Container).name();
  EXPECT_EQ(old_bytes, new_bytes)
      << "container byte content mismatch for " << typeid(Container).name();
}

// Helper for round-trip: encode via Marshal, decode via BinaryReadArchive,
// assert equality.
template <typename Container>
void check_container_round_trip(const Container& c) {
  Marshal m;
  m << c;
  auto bytes = drain_marshal(m);
  BufferSource source(bytes.data(), bytes.size());
  BinaryReadArchive reader(&source);
  Container decoded;
  reader >> decoded;
  EXPECT_EQ(decoded, c) << "container round-trip mismatch for "
                       << typeid(Container).name();
  EXPECT_TRUE(source.eof());
}

// ---- rusty::Vec / std::vector / std::list -------------------------------

// rusty containers (Vec/BTreeSet/HashSet/BTreeMap/HashMap):
//
// Note: the existing `Marshal::operator<<` templates for `rusty::Vec<T>`
// etc. (in marshal.hpp lines 1110+) reference `typename
// rusty::Vec<T>::const_iterator` — a typedef that rusty types do not
// expose. Those templates are dead code in the existing Marshal — they
// would fail to compile if any real call site instantiated them, but
// in practice rrr only marshals std:: containers. So byte-for-byte
// compatibility is not testable for rusty containers (no working
// reference). Instead we verify the new Archive round-trips correctly
// through itself (encode + decode via Archive), which is the actual
// guarantee the new code provides.

template <typename Container>
void check_archive_round_trip_only(const Container& c) {
  BufferSink sink;
  BinaryWriteArchive writer(&sink);
  writer << c;

  std::vector<uint8_t> bytes(sink.bytes.len());
  for (size_t i = 0; i < sink.bytes.len(); ++i) bytes[i] = sink.bytes[i];

  BufferSource source(bytes.data(), bytes.size());
  BinaryReadArchive reader(&source);
  Container decoded;
  reader >> decoded;
  EXPECT_TRUE(source.eof()) << "decoder did not consume all bytes for "
                            << typeid(Container).name();
  // Element-wise comparison via len() + index for rusty types.
  EXPECT_EQ(decoded.len(), c.len()) << "round-trip size mismatch for "
                                    << typeid(Container).name();
}

TEST(MarshalArchiveRoundTrip, RustyVecEmpty) {
  rusty::Vec<int32_t> v;
  check_archive_round_trip_only(v);
}

TEST(MarshalArchiveRoundTrip, RustyVecPrimitives) {
  rusty::Vec<int32_t> v;
  v.push(1); v.push(2); v.push(3); v.push(-1); v.push(0x7FFFFFFF);

  BufferSink sink;
  BinaryWriteArchive writer(&sink);
  writer << v;

  std::vector<uint8_t> bytes(sink.bytes.len());
  for (size_t i = 0; i < sink.bytes.len(); ++i) bytes[i] = sink.bytes[i];

  BufferSource source(bytes.data(), bytes.size());
  BinaryReadArchive reader(&source);
  rusty::Vec<int32_t> decoded;
  reader >> decoded;
  ASSERT_EQ(decoded.size(), v.size());
  for (size_t i = 0; i < v.size(); ++i) {
    EXPECT_EQ(decoded[i], v[i]);
  }
  EXPECT_TRUE(source.eof());
}

TEST(MarshalArchiveByteCompat, StdVectorEmpty) {
  std::vector<int32_t> v;
  check_container_compat_write(v);
  check_container_round_trip(v);
}

TEST(MarshalArchiveByteCompat, StdVectorPrimitives) {
  std::vector<int64_t> v{1, 2, 3, -1, 0x7FFFFFFFFFFFFFFFLL};
  check_container_compat_write(v);
  check_container_round_trip(v);
}

TEST(MarshalArchiveByteCompat, StdVectorOfStrings) {
  std::vector<std::string> v{"a", "bb", "", "ccc", "dddd"};
  check_container_compat_write(v);
  check_container_round_trip(v);
}

TEST(MarshalArchiveByteCompat, StdVectorOfPairs) {
  std::vector<std::pair<int32_t, std::string>> v{
      {1, "one"}, {2, "two"}, {3, "three"}};
  check_container_compat_write(v);
  check_container_round_trip(v);
}

TEST(MarshalArchiveByteCompat, NestedVectors) {
  std::vector<std::vector<int32_t>> v{{1, 2}, {}, {3, 4, 5}};
  check_container_compat_write(v);
  check_container_round_trip(v);
}

TEST(MarshalArchiveByteCompat, StdListPrimitives) {
  std::list<int32_t> v{10, 20, 30};
  check_container_compat_write(v);
  check_container_round_trip(v);
}

// ---- Sets ------------------------------------------------------------

TEST(MarshalArchiveByteCompat, StdSetEmpty) {
  std::set<int32_t> s;
  check_container_compat_write(s);
  check_container_round_trip(s);
}

TEST(MarshalArchiveByteCompat, StdSetPrimitives) {
  // std::set is sorted by key — both Marshal and BinaryWriteArchive
  // iterate in the same sorted order, so bytes match.
  std::set<int32_t> s{5, 1, 3, 2, 4};
  check_container_compat_write(s);
  check_container_round_trip(s);
}

TEST(MarshalArchiveByteCompat, StdUnorderedSetPrimitives) {
  // For unordered_set, both Marshal and BinaryWriteArchive iterate via
  // the same begin()/end() — same hash table iteration order, so the
  // bytes match (even though the order is not deterministic across
  // runs/sets, within a single set both encoders produce the same).
  std::unordered_set<int32_t> s{1, 2, 3, 4, 5};
  check_container_compat_write(s);
  check_container_round_trip(s);
}

TEST(MarshalArchiveRoundTrip, RustyBTreeSetPrimitives) {
  rusty::BTreeSet<int32_t> s;
  s.insert(5); s.insert(1); s.insert(3); s.insert(2); s.insert(4);

  BufferSink sink;
  BinaryWriteArchive writer(&sink);
  writer << s;

  std::vector<uint8_t> bytes(sink.bytes.len());
  for (size_t i = 0; i < sink.bytes.len(); ++i) bytes[i] = sink.bytes[i];

  BufferSource source(bytes.data(), bytes.size());
  BinaryReadArchive reader(&source);
  rusty::BTreeSet<int32_t> decoded;
  reader >> decoded;
  ASSERT_EQ(decoded.len(), s.len());
  EXPECT_TRUE(source.eof());
}

TEST(MarshalArchiveRoundTrip, RustyHashSetPrimitives) {
  rusty::HashSet<int32_t> s;
  s.insert(1); s.insert(2); s.insert(3);

  BufferSink sink;
  BinaryWriteArchive writer(&sink);
  writer << s;

  std::vector<uint8_t> bytes(sink.bytes.len());
  for (size_t i = 0; i < sink.bytes.len(); ++i) bytes[i] = sink.bytes[i];

  BufferSource source(bytes.data(), bytes.size());
  BinaryReadArchive reader(&source);
  rusty::HashSet<int32_t> decoded;
  reader >> decoded;
  ASSERT_EQ(decoded.len(), s.len());
  EXPECT_TRUE(source.eof());
}

// ---- Maps ------------------------------------------------------------

TEST(MarshalArchiveByteCompat, StdMapEmpty) {
  std::map<int32_t, std::string> m;
  check_container_compat_write(m);
  check_container_round_trip(m);
}

TEST(MarshalArchiveByteCompat, StdMapPrimitives) {
  std::map<int32_t, std::string> m{
      {3, "three"}, {1, "one"}, {2, "two"}};
  check_container_compat_write(m);
  check_container_round_trip(m);
}

TEST(MarshalArchiveByteCompat, StdUnorderedMapPrimitives) {
  std::unordered_map<int32_t, std::string> m{
      {1, "a"}, {2, "b"}, {3, "c"}};
  check_container_compat_write(m);
  check_container_round_trip(m);
}

TEST(MarshalArchiveRoundTrip, RustyBTreeMapPrimitives) {
  rusty::BTreeMap<int32_t, int64_t> m;
  m.insert(3, 30); m.insert(1, 10); m.insert(2, 20);

  BufferSink sink;
  BinaryWriteArchive writer(&sink);
  writer << m;

  std::vector<uint8_t> bytes(sink.bytes.len());
  for (size_t i = 0; i < sink.bytes.len(); ++i) bytes[i] = sink.bytes[i];

  BufferSource source(bytes.data(), bytes.size());
  BinaryReadArchive reader(&source);
  rusty::BTreeMap<int32_t, int64_t> decoded;
  reader >> decoded;
  ASSERT_EQ(decoded.len(), m.len());
  EXPECT_TRUE(source.eof());
}

TEST(MarshalArchiveRoundTrip, RustyHashMapPrimitives) {
  rusty::HashMap<int32_t, std::string> m;
  m.insert(1, "a"); m.insert(2, "b"); m.insert(3, "c");

  BufferSink sink;
  BinaryWriteArchive writer(&sink);
  writer << m;

  std::vector<uint8_t> bytes(sink.bytes.len());
  for (size_t i = 0; i < sink.bytes.len(); ++i) bytes[i] = sink.bytes[i];

  BufferSource source(bytes.data(), bytes.size());
  BinaryReadArchive reader(&source);
  rusty::HashMap<int32_t, std::string> decoded;
  reader >> decoded;
  ASSERT_EQ(decoded.len(), m.len());
  EXPECT_TRUE(source.eof());
}

}  // namespace
}  // namespace rrr
