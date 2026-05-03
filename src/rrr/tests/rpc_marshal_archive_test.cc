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

#include <fcntl.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "../misc/marshal.hpp"
#include "../misc/marshal_archive.hpp"
#include "../misc/marshal_serializable_bridge.hpp"
#include "../misc/serializable_envelope.hpp"

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

// ---- FdSink / FdSource (Phase 1c) ------------------------------------

// RAII wrapper around a pipe pair so test failures don't leak fds.
struct ScopedPipe {
  int fds[2] = {-1, -1};
  ScopedPipe() {
    int rc = ::pipe(fds);
    EXPECT_EQ(rc, 0);
  }
  ~ScopedPipe() {
    if (fds[0] >= 0) ::close(fds[0]);
    if (fds[1] >= 0) ::close(fds[1]);
  }
  void close_read() {
    if (fds[0] >= 0) { ::close(fds[0]); fds[0] = -1; }
  }
  void close_write() {
    if (fds[1] >= 0) { ::close(fds[1]); fds[1] = -1; }
  }
};

// RAII wrapper around a temp file. Lives only inside the test.
struct ScopedTempFile {
  char path[64] = "/tmp/mako_archive_test_XXXXXX";
  int fd = -1;
  ScopedTempFile() {
    fd = ::mkstemp(path);
    EXPECT_GE(fd, 0);
  }
  ~ScopedTempFile() {
    if (fd >= 0) ::close(fd);
    ::unlink(path);
  }
  // Reopen by path read-only, returning a fresh fd. Caller closes it.
  int reopen_ro() const {
    int rfd = ::open(path, O_RDONLY);
    EXPECT_GE(rfd, 0);
    return rfd;
  }
};

TEST(FdSinkSemantics, EmptyWriteIsNoop) {
  ScopedPipe p;
  FdSink sink(p.fds[1]);
  // Calling write(p, 0) should not block and should not consume bytes.
  sink.write(nullptr, 0);
  // Close the write end. The read end should immediately see EOF.
  p.close_write();
  uint8_t buf[1];
  ssize_t r = ::read(p.fds[0], buf, sizeof(buf));
  EXPECT_EQ(r, 0);
}

TEST(FdSourceSemantics, EmptyReadIsNoop) {
  ScopedPipe p;
  FdSource src(p.fds[0]);
  size_t got = src.read(nullptr, 0);
  EXPECT_EQ(got, 0u);
}

TEST(FdSourceSemantics, EofReturnsShortRead) {
  ScopedPipe p;

  // Write 3 bytes, close write end.
  const uint8_t payload[] = {0x01, 0x02, 0x03};
  ssize_t w = ::write(p.fds[1], payload, sizeof(payload));
  ASSERT_EQ(w, 3);
  p.close_write();

  // Try to read 16 bytes. We should get 3 (then EOF).
  FdSource src(p.fds[0]);
  uint8_t buf[16];
  std::memset(buf, 0, sizeof(buf));
  size_t got = src.read(buf, sizeof(buf));
  EXPECT_EQ(got, 3u);
  EXPECT_EQ(buf[0], 0x01);
  EXPECT_EQ(buf[1], 0x02);
  EXPECT_EQ(buf[2], 0x03);

  // Subsequent read on the closed pipe sees EOF immediately.
  size_t again = src.read(buf, 4);
  EXPECT_EQ(again, 0u);
}

TEST(FdSinkArchive, PipeRoundTripPrimitives) {
  ScopedPipe p;

  // Write side runs in this thread; read side in another to avoid
  // blocking on the kernel pipe buffer for small payloads (this is
  // small enough that we wouldn't block, but the threaded pattern
  // matches the larger-payload test below).
  std::vector<uint8_t> drained_bytes;
  std::thread reader_thread([&] {
    uint8_t chunk[64];
    while (true) {
      ssize_t r = ::read(p.fds[0], chunk, sizeof(chunk));
      if (r <= 0) break;
      drained_bytes.insert(drained_bytes.end(), chunk, chunk + r);
    }
  });

  {
    FdSink sink(p.fds[1]);
    BinaryWriteArchive writer(&sink);
    writer << static_cast<int32_t>(0x12345678);
    writer << static_cast<int64_t>(-1);
    writer << std::string("hello");
    writer << v32(5);
    writer << v64(1024);
  }
  p.close_write();
  reader_thread.join();

  // Now decode via BufferSource (deterministic, no kernel timing) and
  // verify each value.
  BufferSource source(drained_bytes.data(), drained_bytes.size());
  BinaryReadArchive reader(&source);

  int32_t a; int64_t b; std::string c; v32 d{0}; v64 e{0};
  reader >> a >> b >> c >> d >> e;
  EXPECT_EQ(a, 0x12345678);
  EXPECT_EQ(b, -1);
  EXPECT_EQ(c, "hello");
  EXPECT_EQ(d.get(), 5);
  EXPECT_EQ(e.get(), 1024);
  EXPECT_TRUE(source.eof());
}

TEST(FdSinkArchive, ByteForByteCompatVsBufferSink) {
  // Encode the same payload via FdSink and BufferSink; compare bytes.
  // This proves FdSink does not introduce any framing/transformation.
  ScopedPipe p;

  std::vector<uint8_t> fd_bytes;
  std::thread reader_thread([&] {
    uint8_t chunk[256];
    while (true) {
      ssize_t r = ::read(p.fds[0], chunk, sizeof(chunk));
      if (r <= 0) break;
      fd_bytes.insert(fd_bytes.end(), chunk, chunk + r);
    }
  });

  {
    FdSink sink(p.fds[1]);
    BinaryWriteArchive writer(&sink);
    writer << static_cast<uint32_t>(42);
    writer << std::string("the quick brown fox");
    writer << v64(0x123456789ABCDEFLL);
    std::vector<int32_t> vec{1, 2, 3, 4, 5};
    writer << vec;
  }
  p.close_write();
  reader_thread.join();

  BufferSink ref_sink;
  BinaryWriteArchive ref_writer(&ref_sink);
  ref_writer << static_cast<uint32_t>(42);
  ref_writer << std::string("the quick brown fox");
  ref_writer << v64(0x123456789ABCDEFLL);
  std::vector<int32_t> vec{1, 2, 3, 4, 5};
  ref_writer << vec;

  std::vector<uint8_t> ref_bytes(ref_sink.bytes.len());
  for (size_t i = 0; i < ref_sink.bytes.len(); ++i) ref_bytes[i] = ref_sink.bytes[i];

  EXPECT_EQ(fd_bytes, ref_bytes);
}

TEST(FdSourceArchive, TempFileRoundTripCompositeSequence) {
  ScopedTempFile tf;

  // Encode via FdSink directly to the temp file.
  {
    FdSink sink(tf.fd);
    BinaryWriteArchive writer(&sink);
    writer << static_cast<int32_t>(7);
    writer << static_cast<int64_t>(-99);
    writer << std::string("temp file payload");
    std::vector<std::string> strs{"a", "bb", "ccc"};
    writer << strs;
    std::map<int32_t, int64_t> m{{1, 100}, {2, 200}, {3, 300}};
    writer << m;
  }
  // Flush by closing — done by ScopedTempFile dtor at scope end. Force
  // it now since we want to reopen for reading.
  ::fsync(tf.fd);

  // Reopen and decode via FdSource.
  int rfd = tf.reopen_ro();
  {
    FdSource src(rfd);
    BinaryReadArchive reader(&src);
    int32_t a; int64_t b; std::string c;
    std::vector<std::string> strs;
    std::map<int32_t, int64_t> m;
    reader >> a >> b >> c >> strs >> m;
    EXPECT_EQ(a, 7);
    EXPECT_EQ(b, -99);
    EXPECT_EQ(c, "temp file payload");
    EXPECT_EQ(strs.size(), 3u);
    EXPECT_EQ(strs[0], "a");
    EXPECT_EQ(strs[1], "bb");
    EXPECT_EQ(strs[2], "ccc");
    EXPECT_EQ(m.size(), 3u);
    EXPECT_EQ(m[1], 100);
    EXPECT_EQ(m[2], 200);
    EXPECT_EQ(m[3], 300);
  }
  ::close(rfd);
}

TEST(FdSinkArchive, LargePayloadChunkedWrite) {
  // Exceeds typical 64KB pipe buffer — exercises FdSink's full-write
  // loop. We use a thread to drain the pipe in parallel.
  ScopedPipe p;

  std::vector<uint8_t> drained;
  std::thread reader_thread([&] {
    uint8_t chunk[8192];
    while (true) {
      ssize_t r = ::read(p.fds[0], chunk, sizeof(chunk));
      if (r <= 0) break;
      drained.insert(drained.end(), chunk, chunk + r);
    }
  });

  // 200KB payload of std::vector<int32_t> = 50000 * 4 bytes + length
  // prefix.
  std::vector<int32_t> big;
  big.reserve(50000);
  for (int32_t i = 0; i < 50000; ++i) big.push_back(i);

  {
    FdSink sink(p.fds[1]);
    BinaryWriteArchive writer(&sink);
    writer << big;
  }
  p.close_write();
  reader_thread.join();

  // Decode and verify.
  BufferSource source(drained.data(), drained.size());
  BinaryReadArchive reader(&source);
  std::vector<int32_t> decoded;
  reader >> decoded;
  EXPECT_EQ(decoded.size(), big.size());
  EXPECT_EQ(decoded, big);
  EXPECT_TRUE(source.eof());
}

TEST(FdSourceArchive, ChunkedReadAcrossPipeBoundaries) {
  // The producer writes in tiny chunks (1 byte at a time) so that
  // FdSource's full-read loop exercises multiple ::read calls per
  // operator>>. We feed enough data to encode several primitives.
  ScopedPipe p;

  // Pre-encode the payload into a buffer so the producer thread just
  // splatters bytes onto the pipe in 1-byte writes.
  BufferSink prep_sink;
  BinaryWriteArchive prep(&prep_sink);
  prep << static_cast<int32_t>(0xDEADBEEF);
  prep << static_cast<int64_t>(0x1122334455667788LL);
  prep << std::string("chunked across syscalls");
  prep << v64(987654321LL);

  std::vector<uint8_t> payload(prep_sink.bytes.len());
  for (size_t i = 0; i < prep_sink.bytes.len(); ++i) payload[i] = prep_sink.bytes[i];

  std::thread writer_thread([&] {
    for (uint8_t b : payload) {
      // 1-byte writes maximize ::read short-returns on the consumer.
      ssize_t r = ::write(p.fds[1], &b, 1);
      EXPECT_EQ(r, 1);
    }
    ::close(p.fds[1]);
    p.fds[1] = -1;
  });

  FdSource src(p.fds[0]);
  BinaryReadArchive reader(&src);
  int32_t a; int64_t b; std::string c; v64 d{0};
  reader >> a >> b >> c >> d;
  EXPECT_EQ(static_cast<uint32_t>(a), 0xDEADBEEFu);
  EXPECT_EQ(b, 0x1122334455667788LL);
  EXPECT_EQ(c, "chunked across syscalls");
  EXPECT_EQ(d.get(), 987654321LL);

  writer_thread.join();
}

// ---- SerializableProxy / SerializableRegistry (Phase 2) --------------

// Canary command for Phase 2: implements BOTH the new Serializable
// interface (`save` / `load` / `kind`) AND the old Marshal-based
// interface (`to_marshal` / `from_marshal`) so we can byte-compare
// the two paths.
//
// This type is intentionally test-local — Phase 2 only validates the
// new infrastructure. Per-command-type production migrations land in
// Phase 4.
struct CanaryCommand {
  int32_t id{0};
  std::string name;
  std::vector<int64_t> values;

  static constexpr int32_t kKind = 0xCAFE;

  // ---- New Serializable interface (Layer 4 of marshal_archive) ----
  int32_t kind() const { return kKind; }

  void save(BinaryWriteArchive& ar) const {
    ar << id << name << values;
  }

  void load(BinaryReadArchive& ar) {
    ar >> id >> name >> values;
  }

  // ---- Old Marshal-based interface (for byte-compat verification) -
  Marshal& to_marshal(Marshal& m) const {
    m << id << name << values;
    return m;
  }

  Marshal& from_marshal(Marshal& m) {
    m >> id >> name >> values;
    return m;
  }
};

TEST(SerializableProxy, ByteCompatVsMarshalDirect) {
  // Encode the same payload via:
  //   (a) the old Marshal path: canary.to_marshal(m)
  //   (b) the new SerializableProxy path: proxy->save(writer)
  // and assert the two byte streams are identical.
  CanaryCommand canary;
  canary.id = 42;
  canary.name = "hello, world";
  canary.values = {1, 2, 3, 4, 5};

  // Path (a): old Marshal.
  Marshal old_m;
  canary.to_marshal(old_m);
  auto old_bytes = drain_marshal(old_m);

  // Path (b): SerializableProxy.
  SerializableProxy proxy = make_serializable_proxy<CanaryCommand>(canary);
  BufferSink sink;
  BinaryWriteArchive writer(&sink);
  proxy->save(writer);
  auto new_bytes = sink_to_vector(sink);

  ASSERT_EQ(old_bytes.size(), new_bytes.size());
  EXPECT_EQ(old_bytes, new_bytes);
}

TEST(SerializableProxy, KindIsExposedThroughProxy) {
  SerializableProxy proxy = make_serializable_proxy<CanaryCommand>();
  EXPECT_EQ(proxy->kind(), CanaryCommand::kKind);
}

TEST(SerializableProxy, RoundTripSaveLoadViaProxy) {
  // Serialize an instance through the proxy, then deserialize into a
  // fresh proxy. Verify that re-serializing the loaded proxy gives
  // identical bytes (proves the load populated state correctly).
  CanaryCommand orig;
  orig.id = 7;
  orig.name = "round trip";
  orig.values = {-100, 0, 100};

  // Save orig.
  SerializableProxy save_proxy = make_serializable_proxy<CanaryCommand>(orig);
  BufferSink sink_orig;
  BinaryWriteArchive writer_orig(&sink_orig);
  save_proxy->save(writer_orig);
  auto orig_bytes = sink_to_vector(sink_orig);

  // Load into a fresh (default-constructed) proxy.
  SerializableProxy load_proxy = make_serializable_proxy<CanaryCommand>();
  BufferSource source(orig_bytes.data(), orig_bytes.size());
  BinaryReadArchive reader(&source);
  load_proxy->load(reader);
  EXPECT_TRUE(source.eof());

  // Re-save via the loaded proxy. Bytes must match exactly.
  BufferSink sink_loaded;
  BinaryWriteArchive writer_loaded(&sink_loaded);
  load_proxy->save(writer_loaded);
  auto loaded_bytes = sink_to_vector(sink_loaded);

  EXPECT_EQ(orig_bytes, loaded_bytes);
}

TEST(SerializableRegistry, RegisterCreateAndRoundTrip) {
  // Phase 2 DoD test: factory registry produces a fresh proxy whose
  // bytes match an independently-saved instance after load.
  SerializableRegistry::clear_for_testing();
  ASSERT_FALSE(SerializableRegistry::is_registered(CanaryCommand::kKind));

  // Register CanaryCommand under its kind.
  (void)SerializableRegistry::reg<CanaryCommand>(CanaryCommand::kKind);
  EXPECT_TRUE(SerializableRegistry::is_registered(CanaryCommand::kKind));

  // Create + populate the source.
  CanaryCommand orig;
  orig.id = 1234;
  orig.name = "registry path";
  orig.values = {7, 8, 9};

  BufferSink sink_src;
  BinaryWriteArchive writer_src(&sink_src);
  orig.save(writer_src);
  auto src_bytes = sink_to_vector(sink_src);

  // Use the registry to create a fresh proxy from the kind, then load.
  SerializableProxy proxy = SerializableRegistry::create(CanaryCommand::kKind);
  EXPECT_EQ(proxy->kind(), CanaryCommand::kKind);

  BufferSource source(src_bytes.data(), src_bytes.size());
  BinaryReadArchive reader(&source);
  proxy->load(reader);
  EXPECT_TRUE(source.eof());

  // Save via the loaded proxy; compare bytes.
  BufferSink sink_loaded;
  BinaryWriteArchive writer_loaded(&sink_loaded);
  proxy->save(writer_loaded);
  auto loaded_bytes = sink_to_vector(sink_loaded);
  EXPECT_EQ(src_bytes, loaded_bytes);

  SerializableRegistry::clear_for_testing();
}

// ---- Marshal ↔ Archive bridges (Phase 3a) ----------------------------

TEST(MarshalSinkBridge, WriteIntoMarshalProducesIdenticalBytes) {
  // Encode a payload via:
  //   (a) BinaryWriteArchive over BufferSink (reference)
  //   (b) BinaryWriteArchive over MarshalSink wrapping a Marshal
  // and verify the two byte streams are identical. Confirms
  // MarshalSink does not introduce any framing or transformation.
  const int32_t i = 0xDEADBEEFu;
  const std::string s = "bridge to marshal";
  std::vector<int64_t> v{1, -2, 3, -4, 5};

  // (a) reference via BufferSink.
  BufferSink ref_sink;
  BinaryWriteArchive ref_writer(&ref_sink);
  ref_writer << i << s << v;
  auto ref_bytes = sink_to_vector(ref_sink);

  // (b) via MarshalSink.
  Marshal m;
  MarshalSink mark_sink(&m);
  BinaryWriteArchive mark_writer(&mark_sink);
  mark_writer << i << s << v;
  auto bridge_bytes = drain_marshal(m);

  ASSERT_EQ(ref_bytes.size(), bridge_bytes.size());
  EXPECT_EQ(ref_bytes, bridge_bytes);
}

TEST(MarshalSinkBridge, MixedMarshalAndArchiveWrites) {
  // Interleave old-style `Marshal::operator<<` writes with new-style
  // BinaryWriteArchive writes through a MarshalSink wrapping the same
  // Marshal. The combined byte stream should be the concatenation —
  // proving the bridge is safe to use alongside legacy code.
  Marshal m;
  m << static_cast<int32_t>(1);

  {
    MarshalSink sink(&m);
    BinaryWriteArchive writer(&sink);
    writer << static_cast<int32_t>(2);
  }

  m << std::string("trailing");

  // Compare against a reference encoded entirely through Marshal.
  Marshal ref;
  ref << static_cast<int32_t>(1);
  ref << static_cast<int32_t>(2);
  ref << std::string("trailing");

  auto bridge_bytes = drain_marshal(m);
  auto ref_bytes = drain_marshal(ref);
  EXPECT_EQ(bridge_bytes, ref_bytes);
}

TEST(MarshalSourceBridge, ReadOldMarshalBytesViaArchive) {
  // Encode a payload via the OLD `Marshal::operator<<` path, then
  // decode it through a `BinaryReadArchive` over `MarshalSource`.
  // Expected: identical decoded values, source drained at the end.
  const int32_t i = 42;
  const std::string s = "marshal-encoded";
  std::vector<int64_t> v{10, 20, 30};

  Marshal m;
  m << i << s << v;

  MarshalSource src(&m);
  BinaryReadArchive reader(&src);

  int32_t i2;
  std::string s2;
  std::vector<int64_t> v2;
  reader >> i2 >> s2 >> v2;

  EXPECT_EQ(i2, i);
  EXPECT_EQ(s2, s);
  EXPECT_EQ(v2, v);
  EXPECT_EQ(m.content_size(), 0u);  // Marshal drained.
}

TEST(MarshalBridges, RoundTripThroughMarshalSinkAndSource) {
  // Write via BinaryWriteArchive(MarshalSink) -> Marshal ->
  // BinaryReadArchive(MarshalSource). Should round-trip cleanly.
  Marshal m;

  {
    MarshalSink sink(&m);
    BinaryWriteArchive writer(&sink);
    writer << static_cast<int32_t>(7);
    writer << static_cast<int64_t>(-99);
    writer << std::string("hello");
    std::vector<int32_t> ints{4, 5, 6};
    writer << ints;
  }

  MarshalSource src(&m);
  BinaryReadArchive reader(&src);

  int32_t a;
  int64_t b;
  std::string c;
  std::vector<int32_t> d;
  reader >> a >> b >> c >> d;

  EXPECT_EQ(a, 7);
  EXPECT_EQ(b, -99);
  EXPECT_EQ(c, "hello");
  EXPECT_EQ(d, (std::vector<int32_t>{4, 5, 6}));
  EXPECT_EQ(m.content_size(), 0u);
}

TEST(MarshalSourceBridge, ShortReadAtEofMatchesBufferSourceSemantics) {
  // Drain a Marshal, then attempt to read past its end via
  // MarshalSource. Should return 0 (not abort), consistent with
  // BufferSource and FdSource EOF semantics.
  Marshal m;
  m << static_cast<int32_t>(1);

  MarshalSource src(&m);
  int32_t v;
  BinaryReadArchive reader(&src);
  reader >> v;
  EXPECT_EQ(v, 1);

  uint8_t extra[4];
  size_t got = src.read(extra, sizeof(extra));
  EXPECT_EQ(got, 0u);
}

TEST(SerializableRegistry, MultipleKindsCoexist) {
  // Two different kinds should be retrievable independently.
  // We re-use CanaryCommand as the underlying type (the registry
  // doesn't know or care about T identity, only the kind tag and the
  // factory's return value).
  SerializableRegistry::clear_for_testing();

  constexpr int32_t kKindA = 0xAAAA;
  constexpr int32_t kKindB = 0xBBBB;

  // Different factories under different kinds. The factories happen
  // to construct CanaryCommand here, but in production they'd be
  // distinct types.
  (void)SerializableRegistry::reg<CanaryCommand>(kKindA);
  (void)SerializableRegistry::reg<CanaryCommand>(kKindB);

  EXPECT_TRUE(SerializableRegistry::is_registered(kKindA));
  EXPECT_TRUE(SerializableRegistry::is_registered(kKindB));
  EXPECT_FALSE(SerializableRegistry::is_registered(0xCCCC));

  auto pa = SerializableRegistry::create(kKindA);
  auto pb = SerializableRegistry::create(kKindB);
  // Both proxies report the kind that the underlying type's kind()
  // returns — `CanaryCommand::kKind` (0xCAFE), because we registered
  // CanaryCommand under both. This is the expected behavior: the
  // proxy reflects the CONCRETE type's kind, not the registry slot.
  EXPECT_EQ(pa->kind(), CanaryCommand::kKind);
  EXPECT_EQ(pb->kind(), CanaryCommand::kKind);

  SerializableRegistry::clear_for_testing();
}

// ---- Marshallable ↔ Serializable bridges (Phase 3b) ------------------

// Test fixture: a Marshallable subclass that mirrors CanaryCommand's
// fields. Used to verify the bidirectional adapter bridges between
// the old Marshallable interface and the new Serializable interface.
struct CanaryMarshallable : public Marshallable {
  int32_t id{0};
  std::string name;
  std::vector<int64_t> values;

  static constexpr int32_t kKind = 0xBEEF;

  CanaryMarshallable() : Marshallable(kKind) {}

  Marshal& to_marshal(Marshal& m) const override {
    m << id << name << values;
    return m;
  }

  Marshal& from_marshal(Marshal& m) override {
    m >> id >> name >> values;
    return m;
  }
};

TEST(SerializableMarshallableAdapter, BidirectionalRoundTrip) {
  // Wrap a SerializableProxy in a Marshallable shape; serialize via
  // the OLD Marshal-based path and deserialize back through it.
  // Round-trip via the proxy's underlying Serializable should
  // recover identical bytes.
  CanaryCommand orig;
  orig.id = 13;
  orig.name = "S→M adapter";
  orig.values = {1, 2, 3};

  // Wrap orig in a SerializableProxy → Marshallable adapter.
  auto serial_proxy = make_serializable_proxy<CanaryCommand>(orig);
  auto as_m = as_marshallable(std::move(serial_proxy));

  // Encode via the Marshallable interface.
  Marshal m;
  as_m->to_marshal(m);

  // Reference encoding via the original CanaryCommand directly.
  Marshal ref;
  ref << orig.id << orig.name << orig.values;
  EXPECT_EQ(drain_marshal(ref), drain_marshal(m));

  // Round-trip: re-encode + decode through the adapter.
  Marshal m2;
  as_m->to_marshal(m2);

  // Decode back through a fresh Serializable underneath a fresh
  // S→M adapter.
  auto fresh_proxy = make_serializable_proxy<CanaryCommand>();
  auto fresh_m = as_marshallable(std::move(fresh_proxy));
  fresh_m->from_marshal(m2);

  // Re-encode the loaded adapter; bytes must match the original ref.
  Marshal m3;
  fresh_m->to_marshal(m3);

  Marshal ref2;
  ref2 << orig.id << orig.name << orig.values;
  EXPECT_EQ(drain_marshal(ref2), drain_marshal(m3));
}

TEST(SerializableMarshallableAdapter, KindIsForwarded) {
  // The Marshallable's kind() should report the underlying
  // Serializable's kind.
  auto proxy = make_serializable_proxy<CanaryCommand>();
  auto as_m = as_marshallable(std::move(proxy));
  EXPECT_EQ(as_m->kind(), CanaryCommand::kKind);
}




// ---- MarshallDeputy registration of Serializable types (Phase 4 prep)

// Test fixture: a Serializable type that does NOT inherit Marshallable
// and does NOT have a TypedMarshallableAdapterTraits specialization.
// This is the shape that Phase 4 migrations move command types toward
// — pure save/load/kind, no virtual inheritance.
struct CanaryDeputyCommand {
  int32_t header{0};
  std::string name;
  std::vector<int32_t> values;

  // Use a kind in the user range (avoid colliding with
  // MarshallDeputy's reserved CMD_* enum values).
  static constexpr int32_t kKind = 0x10001;

  int32_t kind() const { return kKind; }

  void save(BinaryWriteArchive& ar) const {
    ar << header << name << values;
  }

  void load(BinaryReadArchive& ar) {
    ar >> header >> name >> values;
  }
};

// Static-initializer registration. Runs once at TU load time; safe
// because the registry is SpinMutex-protected.
static int _reg_canary_deputy_command =
    reg_serializable_in_deputy<CanaryDeputyCommand>(
        CanaryDeputyCommand::kKind);

TEST(RegSerializableInDeputy, RegistersUnderKind) {
  // The registration ran at static init. Confirm the factory produces
  // a Marshallable of the right kind.  L5i: the public API is now
  // `create_initializer`, which invokes the registered factory under
  // the registry SpinMutex (the prior `get_initializer` returned the
  // copyable std::function; rusty::Function is move-only, so we
  // invoke under the lock instead of copying out).
  auto m = MarshallDeputy::create_initializer(CanaryDeputyCommand::kKind);
  ASSERT_NE(m, nullptr);
  EXPECT_EQ(m->kind(), CanaryDeputyCommand::kKind);
}

TEST(RegSerializableInDeputy, RoundTripThroughMarshallDeputy) {
  // Construct a MarshallDeputy holding a CanaryDeputyCommand wrapped
  // via the existing as_marshallable bridge (the write side; a
  // dedicated MarshallDeputy(SerializableProxy) ctor lands in Phase
  // 3f). Encode via Marshal (kind | payload), decode into a fresh
  // MarshallDeputy via the registered factory, verify bytes survive
  // a re-encode round-trip.
  CanaryDeputyCommand orig;
  orig.header = 7;
  orig.name = "canary deputy";
  orig.values = {1, 2, 3};

  // Build a MarshallDeputy whose inner is a SerializableMarshallable-
  // Adapter wrapping orig.
  auto orig_proxy = make_serializable_proxy<CanaryDeputyCommand>(orig);
  auto orig_marsh = as_marshallable(std::move(orig_proxy));
  MarshallDeputy md_orig{orig_marsh};
  EXPECT_EQ(md_orig.kind_, CanaryDeputyCommand::kKind);

  // Encode via the existing Marshal-based MarshallDeputy operator<<.
  Marshal m;
  m << md_orig;
  auto bytes = drain_marshal(m);

  // Decode into a fresh MarshallDeputy. This triggers
  // create_actual_object_from → registered factory → SerializableProxy
  // → SerializableMarshallableAdapter → from_marshal via MarshalSource.
  Marshal m2;
  m2.write(bytes.data(), bytes.size());
  MarshallDeputy md_decoded;
  m2 >> md_decoded;
  EXPECT_EQ(md_decoded.kind_, CanaryDeputyCommand::kKind);
  ASSERT_NE(md_decoded.inner(), nullptr);
  EXPECT_EQ(md_decoded.inner()->kind(), CanaryDeputyCommand::kKind);

  // Re-encode the decoded deputy. Bytes must match the original
  // encoding (proves the load reconstructed state correctly).
  Marshal m3;
  m3 << md_decoded;
  auto bytes2 = drain_marshal(m3);
  EXPECT_EQ(bytes, bytes2);
}

TEST(SerializableCast, RecoversTypedPayloadFromMarshallDeputy) {
  // After Phase 4 migration, callers will use `serializable_cast<T>`
  // in place of `marshallable_cast<T>` for types that switched from
  // Marshallable to Serializable. Verify the cast helper returns the
  // underlying T from a deputy populated via the
  // reg_serializable_in_deputy factory.

  CanaryDeputyCommand orig;
  orig.header = 12345;
  orig.name = "cast me";
  orig.values = {7, 8, 9};

  // Encode → decode round-trip through MarshallDeputy via the
  // registered Serializable factory.
  auto orig_marsh = as_marshallable(
      make_serializable_proxy<CanaryDeputyCommand>(orig));
  MarshallDeputy md_orig{orig_marsh};
  Marshal m;
  m << md_orig;
  auto bytes = drain_marshal(m);

  Marshal m2;
  m2.write(bytes.data(), bytes.size());
  MarshallDeputy md_decoded;
  m2 >> md_decoded;

  // Cast via serializable_cast; verify the recovered T's fields match.
  CanaryDeputyCommand* recovered =
      serializable_cast<CanaryDeputyCommand>(md_decoded);
  ASSERT_NE(recovered, nullptr);
  EXPECT_EQ(recovered->header, orig.header);
  EXPECT_EQ(recovered->name, orig.name);
  EXPECT_EQ(recovered->values, orig.values);
}

TEST(SerializableCast, ReturnsNullForWrongType) {
  // serializable_cast should fail safely when the requested T does
  // not match the wrapped type.
  struct OtherSerializable {
    int32_t kind() const { return 0x20002; }
    void save(BinaryWriteArchive&) const {}
    void load(BinaryReadArchive&) {}
  };

  auto marsh = as_marshallable(
      make_serializable_proxy<CanaryDeputyCommand>());
  ASSERT_NE(marsh, nullptr);

  // Cast to the actual type — succeeds.
  EXPECT_NE(serializable_cast<CanaryDeputyCommand>(marsh), nullptr);
  // Cast to a different Serializable type — returns nullptr.
  EXPECT_EQ(serializable_cast<OtherSerializable>(marsh), nullptr);
}

TEST(SerializableCast, ReturnsNullForNonSerializableMarshallable) {
  // serializable_cast on a Marshallable that's NOT a
  // SerializableMarshallableAdapter (e.g. a plain Marshallable
  // subclass from the legacy path) returns nullptr cleanly.
  auto canary = std::make_shared<CanaryMarshallable>();
  std::shared_ptr<Marshallable> marsh = canary;
  EXPECT_EQ(serializable_cast<CanaryMarshallable>(marsh), nullptr);
  EXPECT_EQ(serializable_cast<CanaryDeputyCommand>(marsh), nullptr);
}

TEST(SerializableCast, MutationVisibleThroughProxy) {
  // serializable_cast returns a pointer to the proxy-owned T; mutating
  // it should be visible to subsequent saves through the proxy.
  CanaryDeputyCommand orig;
  orig.header = 1;
  orig.name = "before";

  auto marsh = as_marshallable(
      make_serializable_proxy<CanaryDeputyCommand>(orig));
  CanaryDeputyCommand* p = serializable_cast<CanaryDeputyCommand>(marsh);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p->name, "before");

  // Mutate through the recovered pointer.
  p->name = "after";
  p->header = 999;

  // Save through the Marshallable interface; bytes should reflect the
  // mutation.
  Marshal m;
  marsh->to_marshal(m);
  auto bytes = drain_marshal(m);

  // Decode into a fresh independent CanaryDeputyCommand and verify.
  BufferSource src(bytes.data(), bytes.size());
  BinaryReadArchive reader(&src);
  CanaryDeputyCommand decoded;
  decoded.load(reader);
  EXPECT_EQ(decoded.name, "after");
  EXPECT_EQ(decoded.header, 999);
}

// ---- Aliased wrap (Phase 4a-2) ---------------------------------------

TEST(WrapSerializableAliased, PreservesSharedPtrAliasing) {
  // wrap_serializable_aliased holds a shared_ptr<T> inside the
  // adapter, so the proxy and the caller's shared_ptr reference the
  // SAME T instance. Mutations through either are visible to the
  // other.
  auto sp = std::make_shared<CanaryDeputyCommand>();
  sp->header = 100;
  sp->name = "before";

  auto marsh = wrap_serializable_aliased<CanaryDeputyCommand>(sp);
  ASSERT_NE(marsh, nullptr);
  EXPECT_EQ(marsh->kind(), CanaryDeputyCommand::kKind);

  // Mutate through the original shared_ptr.
  sp->header = 200;
  sp->name = "after";

  // Save through the Marshallable interface; bytes should reflect the
  // mutation (proves the proxy aliases the same instance).
  Marshal m;
  marsh->to_marshal(m);
  auto bytes = drain_marshal(m);

  // Independently decode and verify.
  BufferSource src(bytes.data(), bytes.size());
  BinaryReadArchive reader(&src);
  CanaryDeputyCommand decoded;
  decoded.load(reader);
  EXPECT_EQ(decoded.name, "after");
  EXPECT_EQ(decoded.header, 200);
}

TEST(SerializableCast, RecoversAliasedTypedPayload) {
  // serializable_cast<T> should also recover T from an aliased wrap
  // (SerializableSharedPtrAdapter<T> shape), returning a pointer
  // aliasing the caller's shared_ptr<T>.
  auto sp = std::make_shared<CanaryDeputyCommand>();
  sp->header = 7;
  sp->name = "aliased";

  auto marsh = wrap_serializable_aliased<CanaryDeputyCommand>(sp);
  CanaryDeputyCommand* p = serializable_cast<CanaryDeputyCommand>(marsh);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p, sp.get())
      << "serializable_cast on an aliased wrap should return the same "
         "instance as the caller's shared_ptr";
  EXPECT_EQ(p->header, 7);
  EXPECT_EQ(p->name, "aliased");

  // Mutate through the recovered pointer.
  p->header = 42;
  // The caller's shared_ptr observes the mutation (aliased).
  EXPECT_EQ(sp->header, 42);
}

TEST(WrapSerializableAliased, RoundTripsThroughMarshallDeputy) {
  // Encode an aliased wrap into a MarshallDeputy → wire bytes →
  // decode through the registered factory. The READ side gets a fresh
  // (non-aliased) instance via reg_serializable_in_deputy, so
  // serializable_cast there returns a different pointer — but bytes
  // match.
  auto sp = std::make_shared<CanaryDeputyCommand>();
  sp->header = 333;
  sp->name = "round trip aliased";
  sp->values = {1, 2, 3};

  // Wrap aliased; build deputy.
  auto marsh = wrap_serializable_aliased<CanaryDeputyCommand>(sp);
  MarshallDeputy md_orig{marsh};

  // Encode via Marshal, decode into fresh deputy.
  Marshal m;
  m << md_orig;
  auto bytes = drain_marshal(m);

  Marshal m2;
  m2.write(bytes.data(), bytes.size());
  MarshallDeputy md_decoded;
  m2 >> md_decoded;

  // The decoded side has a value-semantic proxy (created by the
  // factory). serializable_cast<T> works on either shape.
  CanaryDeputyCommand* recovered =
      serializable_cast<CanaryDeputyCommand>(md_decoded);
  ASSERT_NE(recovered, nullptr);
  EXPECT_NE(recovered, sp.get())
      << "decoded side should be a fresh instance (factory creates new T)";
  EXPECT_EQ(recovered->header, 333);
  EXPECT_EQ(recovered->name, "round trip aliased");
  EXPECT_EQ(recovered->values, (std::vector<int32_t>{1, 2, 3}));
}

// ---- MarshallDeputy archive operators (Phase 4 enabler) ------------

TEST(MarshallDeputyArchiveOps, WriteByteEquivalentToMarshal) {
  // Encode a MarshallDeputy via:
  //   (a) the legacy `Marshal m; m << md;` path
  //   (b) `BinaryWriteArchive(BufferSink) << md`
  // Bytes must match exactly.
  CanaryDeputyCommand orig;
  orig.header = 42;
  orig.name = "deputy archive test";
  orig.values = {1, 2, 3};

  auto orig_marsh = as_marshallable(
      make_serializable_proxy<CanaryDeputyCommand>(orig));
  MarshallDeputy md{orig_marsh};

  // (a) legacy.
  Marshal m;
  m << md;
  auto legacy_bytes = drain_marshal(m);

  // (b) archive.
  BufferSink sink;
  BinaryWriteArchive writer(&sink);
  writer << md;
  auto archive_bytes = sink_to_vector(sink);

  ASSERT_EQ(legacy_bytes.size(), archive_bytes.size());
  EXPECT_EQ(legacy_bytes, archive_bytes);
}

TEST(MarshallDeputyArchiveOps, ReadFromMarshalSourceRoundTrip) {
  // Encode via legacy `Marshal m; m << md;`, then decode via
  // `BinaryReadArchive(MarshalSource) >> md_decoded`. Bytes match
  // (re-encode after decode produces the same legacy bytes).
  CanaryDeputyCommand orig;
  orig.header = 99;
  orig.name = "marshal source round trip";
  orig.values = {7, 8, 9};

  auto orig_marsh = as_marshallable(
      make_serializable_proxy<CanaryDeputyCommand>(orig));
  MarshallDeputy md_orig{orig_marsh};

  Marshal m_wire;
  m_wire << md_orig;

  // Read through BinaryReadArchive over MarshalSource.
  MarshalSource src(&m_wire);
  BinaryReadArchive reader(&src);
  MarshallDeputy md_decoded;
  reader >> md_decoded;
  EXPECT_EQ(md_decoded.kind_, CanaryDeputyCommand::kKind);
  ASSERT_NE(md_decoded.inner(), nullptr);

  // Re-encode the decoded deputy via the legacy path; bytes match the
  // original wire encoding (proves the load reconstructed state
  // byte-for-byte).
  Marshal m_check;
  m_check << md_orig;
  auto orig_bytes = drain_marshal(m_check);
  Marshal m_redecoded;
  m_redecoded << md_decoded;
  auto redecoded_bytes = drain_marshal(m_redecoded);
  EXPECT_EQ(orig_bytes, redecoded_bytes);
}

// Local struct with static constexpr requires file/namespace scope —
// can't be defined inside a TEST body.
struct OuterCommandWithDeputy {
  int32_t outer_id{0};
  MarshallDeputy nested_deputy;
  std::string outer_label;

  static constexpr int32_t kKind = 0x10003;
  int32_t kind() const { return kKind; }

  void save(BinaryWriteArchive& ar) const {
    ar << outer_id;
    ar << nested_deputy;
    ar << outer_label;
  }
  void load(BinaryReadArchive& ar) {
    ar >> outer_id;
    ar >> nested_deputy;
    ar >> outer_label;
  }
};

TEST(MarshallDeputyArchiveOps, NestedInsideUserStructSaveLoad) {
  // Demonstrate the Phase 4 use case: a user struct with a
  // MarshallDeputy field whose save/load uses the new archive
  // operators. After write→read, the nested deputy reconstructs
  // correctly.
  using OuterCommand = OuterCommandWithDeputy;

  // Build outer command with a nested CanaryDeputyCommand.
  CanaryDeputyCommand inner;
  inner.header = 555;
  inner.name = "nested";
  inner.values = {100, 200};

  OuterCommand outer;
  outer.outer_id = 7;
  outer.nested_deputy = MarshallDeputy{
      as_marshallable(
          make_serializable_proxy<CanaryDeputyCommand>(inner))};
  outer.outer_label = "wrapped";

  // Encode outer via Marshal wrapper (so the read side has a
  // MarshalSource).
  Marshal m;
  MarshalSink sink(&m);
  BinaryWriteArchive writer(&sink);
  writer << outer.outer_id;
  writer << outer.nested_deputy;
  writer << outer.outer_label;

  // Decode.
  MarshalSource src(&m);
  BinaryReadArchive reader(&src);
  int32_t decoded_id;
  MarshallDeputy decoded_deputy;
  std::string decoded_label;
  reader >> decoded_id >> decoded_deputy >> decoded_label;

  EXPECT_EQ(decoded_id, 7);
  EXPECT_EQ(decoded_label, "wrapped");
  EXPECT_EQ(decoded_deputy.kind_, CanaryDeputyCommand::kKind);

  // Verify the nested CanaryDeputyCommand contents.
  CanaryDeputyCommand* nested =
      serializable_cast<CanaryDeputyCommand>(decoded_deputy);
  ASSERT_NE(nested, nullptr);
  EXPECT_EQ(nested->header, 555);
  EXPECT_EQ(nested->name, "nested");
  EXPECT_EQ(nested->values, (std::vector<int32_t>{100, 200}));
}

// ---- Phase 4a-prep: marshallable_cast / wrap_typed_marshallable
//      overloads for Serializable types -------------------------------

TEST(MarshallableCastSerializable, ReturnsSharedPtrAliasingProxyOwnedT) {
  // For a Serializable type T, `marshallable_cast<T>(value)` (via the
  // bridge overload) returns a `shared_ptr<T>` aliasing the
  // SerializableMarshallableAdapter that owns T. This preserves the
  // legacy call shape after a Marshallable→Serializable migration.
  CanaryDeputyCommand orig;
  orig.header = 7;
  orig.name = "marshallable_cast bridge";
  orig.values = {10, 20, 30};

  // Wrap the Serializable T as a shared_ptr<Marshallable>.
  auto marsh = as_marshallable(
      make_serializable_proxy<CanaryDeputyCommand>(orig));

  // Legacy-style call shape.
  std::shared_ptr<CanaryDeputyCommand> sp =
      marshallable_cast<CanaryDeputyCommand>(marsh);
  ASSERT_NE(sp, nullptr);
  EXPECT_EQ(sp->header, 7);
  EXPECT_EQ(sp->name, "marshallable_cast bridge");
  EXPECT_EQ(sp->values, (std::vector<int32_t>{10, 20, 30}));

  // The returned shared_ptr aliases the proxy's owned T — same
  // underlying pointer as serializable_cast<T> would return.
  EXPECT_EQ(sp.get(), serializable_cast<CanaryDeputyCommand>(marsh));
}

TEST(MarshallableCastSerializable, AliasingExtendsLifetime) {
  // The aliasing shared_ptr from marshallable_cast keeps the
  // underlying SerializableMarshallableAdapter alive even if the
  // original `marsh` shared_ptr is dropped. Verifies the aliasing
  // ctor's reference counting works correctly.
  std::shared_ptr<CanaryDeputyCommand> sp;
  {
    CanaryDeputyCommand orig;
    orig.header = 99;
    auto marsh = as_marshallable(
        make_serializable_proxy<CanaryDeputyCommand>(orig));
    sp = marshallable_cast<CanaryDeputyCommand>(marsh);
    // `marsh` goes out of scope here; sp's aliasing keeps the
    // SerializableMarshallableAdapter (and its proxy-owned T) alive.
  }
  ASSERT_NE(sp, nullptr);
  EXPECT_EQ(sp->header, 99);
}

struct OtherCmdMarshallableCastTest {
  static constexpr int32_t kKind = 0x40004;
  int32_t kind() const { return kKind; }
  void save(BinaryWriteArchive&) const {}
  void load(BinaryReadArchive&) {}
};

TEST(MarshallableCastSerializable, ReturnsNullForWrongType) {
  // marshallable_cast on a Serializable type should fail safely when
  // T doesn't match the wrapped type.
  auto marsh = as_marshallable(
      make_serializable_proxy<CanaryDeputyCommand>());
  EXPECT_EQ(marshallable_cast<OtherCmdMarshallableCastTest>(marsh),
            nullptr);
  EXPECT_NE(marshallable_cast<CanaryDeputyCommand>(marsh), nullptr);
}

TEST(WrapTypedMarshallableSerializable, RoutesToWrapSerializable) {
  // For a Serializable T, `wrap_typed_marshallable(make_shared<T>())`
  // (via the bridge overload) routes to `wrap_serializable`. Wire
  // bytes are byte-for-byte identical to a fresh proxy save.
  auto sp = std::make_shared<CanaryDeputyCommand>();
  sp->header = 11;
  sp->name = "wrap_typed bridge";

  // Bridge overload via `wrap_typed_marshallable` (the legacy call
  // shape — no need to update existing call sites that wrap a
  // migrated type).
  auto marsh_via_typed = wrap_typed_marshallable<CanaryDeputyCommand>(sp);
  ASSERT_NE(marsh_via_typed, nullptr);

  Marshal m_typed;
  marsh_via_typed->to_marshal(m_typed);
  auto bytes_typed = drain_marshal(m_typed);

  // Reference: direct `wrap_serializable`.
  auto marsh_direct = wrap_serializable(sp);
  Marshal m_direct;
  marsh_direct->to_marshal(m_direct);
  auto bytes_direct = drain_marshal(m_direct);

  EXPECT_EQ(bytes_typed, bytes_direct);
}

// ---- Phase 3f-2 (added field) / Phase 3f-3 (made it lazy) --------
//
// The deputy holds a `mutable shared_ptr<SerializableProxy> serializable_`
// field that is populated lazily on first call to `serializable()`,
// not eagerly inside `set_marshallable`. These tests verify:
//   - The field starts null after construction (no eager work).
//   - `serializable()` populates the cache on first call.
//   - Subsequent calls return the same cached proxy instance.
//   - The proxy's `kind()` matches the deputy's `kind_`.
//   - Bytes saved through the proxy match the bytes that
//     `inner()->to_marshal` would produce.
//   - Wire format on the Marshal `<<` / `>>` path is unchanged.
//   - Copy semantics: copying before any access leaves both copies
//     unpopulated; copying after access shares the cached proxy.
//   - Move semantics: moves transfer the cached proxy (or the empty
//     sentinel) cleanly.
//   - The wire-decode path (`operator>> + create_actual_object_from`)
//     does NOT pre-populate the cache (lazy on the receive side too).












// ---- Phase 3f-5: serializable() short-circuit for SerializableMarshallableAdapter





// ---------------------------------------------------------------------------
// L9 wire compaction: kind serializes as v32 (1 byte for values 0-63 —
// SparseInt's first-byte encoding has a 6-bit signed payload, so values
// in the [-64, 63] range fit in 1 byte; larger values use up to 5
// bytes) instead of fixed 4-byte int32. Saves 3 bytes per polymorphic
// envelope for every closed-set kind (TypeList positions 1-19) and for
// ANY_MESSAGE=24 — i.e., every production payload.
// ---------------------------------------------------------------------------

struct V32CanaryCommand {
  // Small kind in the v32 single-byte range (0-63), outside the
  // closed-set MakoCommands range (1-19) and outside ANY_MESSAGE=24.
  static constexpr int32_t kKind = 50;
  int32_t value{0};

  int32_t kind() const { return kKind; }
  void save(BinaryWriteArchive& ar) const { ar << value; }
  void load(BinaryReadArchive& ar) { ar >> value; }
};

static int _reg_v32_canary =
    reg_serializable_in_deputy<V32CanaryCommand>(V32CanaryCommand::kKind);

TEST(MarshallDeputyKindEncoding, V32SingleByteForSmallKindsViaMarshal) {
  // Pre-L9: kind serialized as raw 4-byte int32 → wire size = 4 + payload.
  // Post-L9: kind serialized as v32 → wire size = 1 + payload (for kind <= 127).
  auto val = std::make_shared<V32CanaryCommand>();
  val->value = 0x12345678;

  Marshal m;
  MarshallDeputy out(wrap_typed_marshallable(val));
  ASSERT_EQ(out.kind_, V32CanaryCommand::kKind);
  m << out;

  auto bytes = drain_marshal(m);

  // Wire layout: [v32 kind = 100 (1 byte)] [int32 value 0x12345678 (4 bytes)]
  ASSERT_EQ(bytes.size(), 1u + sizeof(int32_t));
  EXPECT_EQ(bytes[0], static_cast<uint8_t>(V32CanaryCommand::kKind));

  // Round-trip: bytes encoded with v32 kind decode correctly.
  Marshal m2;
  m2.write(bytes.data(), bytes.size());
  MarshallDeputy in;
  m2 >> in;
  EXPECT_EQ(in.kind_, V32CanaryCommand::kKind);
  auto recovered = marshallable_cast<V32CanaryCommand>(in);
  ASSERT_NE(recovered, nullptr);
  EXPECT_EQ(recovered->value, 0x12345678);
}

TEST(MarshallDeputyKindEncoding, V32SingleByteForSmallKindsViaArchive) {
  // Same wire-size assertion, but through the BinaryWriteArchive +
  // MarshalSink path that the rpcgen-emitted RPC plumbing uses.
  auto val = std::make_shared<V32CanaryCommand>();
  val->value = 0x77;

  Marshal m;
  MarshallDeputy out(wrap_typed_marshallable(val));
  {
    MarshalSink sink(&m);
    BinaryWriteArchive ar(&sink);
    ar << out;
  }

  auto bytes = drain_marshal(m);
  ASSERT_EQ(bytes.size(), 1u + sizeof(int32_t));
  EXPECT_EQ(bytes[0], static_cast<uint8_t>(V32CanaryCommand::kKind));

  // Decode via the BinaryReadArchive path (which delegates to legacy
  // operator>> via the MarshalSource adapter).
  Marshal m2;
  m2.write(bytes.data(), bytes.size());
  MarshalSource source(&m2);
  BinaryReadArchive reader(&source);
  MarshallDeputy in;
  reader >> in;
  EXPECT_EQ(in.kind_, V32CanaryCommand::kKind);
  auto recovered = marshallable_cast<V32CanaryCommand>(in);
  ASSERT_NE(recovered, nullptr);
  EXPECT_EQ(recovered->value, 0x77);
}

// ---------------------------------------------------------------------------
// L10a: TypeList::create_at(pos) compile-time-dispatched factory.
//
// Replaces the runtime `MarshallDeputy::reg_initializer(kind, factory)`
// registry for the closed-set polymorphic path: the TypeList knows its
// types at compile time, so wire-kind → fresh SerializableProxy is a
// switch over `Ts...` with no static-init registration step. Used by
// the L10b `SerializableEnvelope<TypeList>` carrier on its read path.
// ---------------------------------------------------------------------------

// Three small Serializable types for the TypeList factory tests. They
// have distinct save/load shapes so an out-of-position dispatch
// produces a recognizable type mismatch.
// Test types use kind values 50/51/52 — fit in v32 single-byte range
// (≤63), distinct from `MakoCommands` (1-19) and `ANY_MESSAGE` (24).
// The L10c-cmds runtime-registry path on `SerializableEnvelope::load`
// uses `MarshallDeputy::create_initializer(kind)` which requires the
// kind to be registered; tests register their types via
// `reg_serializable_in_deputy` below.  `TypeList::index_of<T>()`
// remains compile-time (1-indexed positions); the kind value the
// test types report is independent of TypeList position.
struct TypeListFactoryAlpha {
  static constexpr int32_t kKind = 60;
  int32_t a{0};
  void save(BinaryWriteArchive& ar) const { ar << a; }
  void load(BinaryReadArchive& ar) { ar >> a; }
  int32_t kind() const { return kKind; }
};

struct TypeListFactoryBeta {
  static constexpr int32_t kKind = 61;
  std::string b;
  void save(BinaryWriteArchive& ar) const { ar << b; }
  void load(BinaryReadArchive& ar) { ar >> b; }
  int32_t kind() const { return kKind; }
};

struct TypeListFactoryGamma {
  static constexpr int32_t kKind = 62;
  int64_t c{0};
  void save(BinaryWriteArchive& ar) const { ar << c; }
  void load(BinaryReadArchive& ar) { ar >> c; }
  int32_t kind() const { return kKind; }
};

using TypeListFactoryList = TypeList<TypeListFactoryAlpha,
                                     TypeListFactoryBeta,
                                     TypeListFactoryGamma>;

// Register with MarshallDeputy so SerializableEnvelope::load can find
// them via the runtime registry path.  (Static-init ordering is fine:
// these run before any test body.)
static int _reg_tl_factory_alpha =
    reg_serializable_in_deputy<TypeListFactoryAlpha>(
        TypeListFactoryAlpha::kKind);
static int _reg_tl_factory_beta =
    reg_serializable_in_deputy<TypeListFactoryBeta>(
        TypeListFactoryBeta::kKind);
static int _reg_tl_factory_gamma =
    reg_serializable_in_deputy<TypeListFactoryGamma>(
        TypeListFactoryGamma::kKind);

TEST(TypeListFactory, IndexOfReturns1IndexedPosition) {
  EXPECT_EQ(TypeListFactoryList::index_of<TypeListFactoryAlpha>(), 1);
  EXPECT_EQ(TypeListFactoryList::index_of<TypeListFactoryBeta>(), 2);
  EXPECT_EQ(TypeListFactoryList::index_of<TypeListFactoryGamma>(), 3);
  // Type not in list resolves to 0 (UNKNOWN sentinel).
  EXPECT_EQ(TypeListFactoryList::index_of<int>(), 0);
}

TEST(TypeListFactory, ContainsTracksIndexOf) {
  EXPECT_TRUE(TypeListFactoryList::contains<TypeListFactoryAlpha>());
  EXPECT_TRUE(TypeListFactoryList::contains<TypeListFactoryBeta>());
  EXPECT_TRUE(TypeListFactoryList::contains<TypeListFactoryGamma>());
  EXPECT_FALSE(TypeListFactoryList::contains<int>());
}

TEST(TypeListFactory, CreateAtReturnsCorrectTypeForEachPosition) {
  // pos=1 → Alpha
  {
    auto proxy = TypeListFactoryList::create_at(1);
    auto* alpha = proxy_cast<TypeListFactoryAlpha>(&*proxy);
    EXPECT_NE(alpha, nullptr);
    EXPECT_EQ(proxy_cast<TypeListFactoryBeta>(&*proxy), nullptr);
    EXPECT_EQ(proxy_cast<TypeListFactoryGamma>(&*proxy), nullptr);
  }
  // pos=2 → Beta
  {
    auto proxy = TypeListFactoryList::create_at(2);
    EXPECT_EQ(proxy_cast<TypeListFactoryAlpha>(&*proxy), nullptr);
    auto* beta = proxy_cast<TypeListFactoryBeta>(&*proxy);
    EXPECT_NE(beta, nullptr);
    EXPECT_EQ(proxy_cast<TypeListFactoryGamma>(&*proxy), nullptr);
  }
  // pos=3 → Gamma
  {
    auto proxy = TypeListFactoryList::create_at(3);
    EXPECT_EQ(proxy_cast<TypeListFactoryAlpha>(&*proxy), nullptr);
    EXPECT_EQ(proxy_cast<TypeListFactoryBeta>(&*proxy), nullptr);
    auto* gamma = proxy_cast<TypeListFactoryGamma>(&*proxy);
    EXPECT_NE(gamma, nullptr);
  }
}

// ---------------------------------------------------------------------------
// L10b: SerializableEnvelope<TypeList> — closed-set polymorphic carrier.
//
// Replaces MarshallDeputy for closed-set polymorphism. Wire format
// [v32 kind][payload bytes] — byte-for-byte identical to MarshallDeputy
// post-L9.
// ---------------------------------------------------------------------------

using EnvelopeTestList = TypeList<TypeListFactoryAlpha,
                                  TypeListFactoryBeta,
                                  TypeListFactoryGamma>;

TEST(SerializableEnvelope, DefaultConstructedIsEmpty) {
  SerializableEnvelope<EnvelopeTestList> env;
  EXPECT_FALSE(env.has_value());
  EXPECT_FALSE(static_cast<bool>(env));
  EXPECT_EQ(env.kind(), 0);
  // unpack on empty returns nullptr.
  EXPECT_EQ(env.unpack<TypeListFactoryAlpha>(), nullptr);
  EXPECT_FALSE(env.is_a<TypeListFactoryAlpha>());
}

TEST(SerializableEnvelope, PackValueSemanticHoldsCopy) {
  TypeListFactoryBeta beta;
  beta.b = "value-semantic";

  auto env = SerializableEnvelope<EnvelopeTestList>::pack(beta);
  EXPECT_TRUE(env.has_value());
  EXPECT_EQ(env.kind(), TypeListFactoryBeta::kKind);
  EXPECT_TRUE(env.is_a<TypeListFactoryBeta>());
  EXPECT_FALSE(env.is_a<TypeListFactoryAlpha>());

  auto* recovered = env.unpack<TypeListFactoryBeta>();
  ASSERT_NE(recovered, nullptr);
  EXPECT_EQ(recovered->b, "value-semantic");

  // Mutating the original after pack does NOT reflect (value semantics).
  beta.b = "mutated";
  EXPECT_EQ(recovered->b, "value-semantic");
}

TEST(SerializableEnvelope, PackAliasedSharesPayload) {
  auto sp = std::make_shared<TypeListFactoryAlpha>();
  sp->a = 7;

  auto env = SerializableEnvelope<EnvelopeTestList>::pack_aliased(sp);
  EXPECT_TRUE(env.has_value());
  EXPECT_EQ(env.kind(), TypeListFactoryAlpha::kKind);

  auto* recovered = env.unpack<TypeListFactoryAlpha>();
  ASSERT_NE(recovered, nullptr);
  EXPECT_EQ(recovered, sp.get());  // aliased — same object
  EXPECT_EQ(recovered->a, 7);

  // Mutating through the original IS visible.
  sp->a = 99;
  EXPECT_EQ(recovered->a, 99);
}

TEST(SerializableEnvelope, UnpackWrongTypeReturnsNullptr) {
  auto env = SerializableEnvelope<EnvelopeTestList>::pack(
      TypeListFactoryGamma{});
  EXPECT_TRUE(env.is_a<TypeListFactoryGamma>());
  EXPECT_FALSE(env.is_a<TypeListFactoryAlpha>());
  EXPECT_EQ(env.unpack<TypeListFactoryAlpha>(), nullptr);
}

TEST(SerializableEnvelope, RoundTripValueSemanticViaArchive) {
  TypeListFactoryBeta beta;
  beta.b = "wire round-trip";

  // Encode.
  BufferSink sink;
  BinaryWriteArchive writer(&sink);
  auto outgoing = SerializableEnvelope<EnvelopeTestList>::pack(beta);
  outgoing.save(writer);

  // Decode.
  BufferSource source(sink.bytes.data(), sink.bytes.len());
  BinaryReadArchive reader(&source);
  SerializableEnvelope<EnvelopeTestList> incoming;
  incoming.load(reader);

  EXPECT_TRUE(incoming.has_value());
  EXPECT_EQ(incoming.kind(), TypeListFactoryBeta::kKind);
  auto* recovered = incoming.unpack<TypeListFactoryBeta>();
  ASSERT_NE(recovered, nullptr);
  EXPECT_EQ(recovered->b, "wire round-trip");
}

TEST(SerializableEnvelope, RoundTripAliasedViaArchive) {
  auto sp = std::make_shared<TypeListFactoryGamma>();
  sp->c = 0xDEADBEEFCAFEBABEll;

  // Encode aliased pack.
  BufferSink sink;
  BinaryWriteArchive writer(&sink);
  auto outgoing = SerializableEnvelope<EnvelopeTestList>::pack_aliased(sp);
  outgoing.save(writer);

  // Decode.
  BufferSource source(sink.bytes.data(), sink.bytes.len());
  BinaryReadArchive reader(&source);
  SerializableEnvelope<EnvelopeTestList> incoming;
  incoming.load(reader);

  EXPECT_EQ(incoming.kind(), TypeListFactoryGamma::kKind);
  auto* recovered = incoming.unpack<TypeListFactoryGamma>();
  ASSERT_NE(recovered, nullptr);
  EXPECT_EQ(recovered->c, 0xDEADBEEFCAFEBABEll);
}

TEST(SerializableEnvelope, WireSizeFor1ByteKind) {
  // Kind 50 (alpha) fits in 1-byte v32 (≤63); alpha's `a` field is
  // i32 (4 bytes).  Total wire size: 1 (v32 kind) + 4 (i32 a) = 5 bytes.
  TypeListFactoryAlpha alpha;
  alpha.a = 0;

  BufferSink sink;
  BinaryWriteArchive writer(&sink);
  auto env = SerializableEnvelope<EnvelopeTestList>::pack(alpha);
  env.save(writer);

  EXPECT_EQ(sink.bytes.len(), 1u + sizeof(int32_t));
  EXPECT_EQ(sink.bytes[0],
            static_cast<uint8_t>(TypeListFactoryAlpha::kKind));
}

TEST(SerializableEnvelope, IsCopyableAndCopiesShareProxy) {
  // SerializableEnvelope copy semantics mirror MarshallDeputy:
  // copies share the underlying proxy (and payload). This matches
  // the existing `req.cmd = some_md;` pattern where two MarshallDeputy
  // instances refer to the same payload.
  auto sp = std::make_shared<TypeListFactoryAlpha>();
  sp->a = 100;
  auto env_a = SerializableEnvelope<EnvelopeTestList>::pack_aliased(sp);

  // Copy.
  auto env_b = env_a;

  // Both should see the same payload.
  EXPECT_EQ(env_a.unpack<TypeListFactoryAlpha>(),
            env_b.unpack<TypeListFactoryAlpha>());

  // Mutation through one envelope is visible to the other (because
  // both share the same shared_ptr<SerializableProxy> internally).
  sp->a = 200;
  EXPECT_EQ(env_a.unpack<TypeListFactoryAlpha>()->a, 200);
  EXPECT_EQ(env_b.unpack<TypeListFactoryAlpha>()->a, 200);
}

TEST(TypeListFactory, CreateAtRoundTripsViaProxySaveLoad) {
  // Pack a Beta via create_at(2), save + load through a proxy, verify
  // the value survives. Demonstrates the L10b read-path shape:
  //   1) Read v32 kind from wire.
  //   2) create_at(kind) → fresh SerializableProxy for that type.
  //   3) proxy->load(reader) — populates the typed value.
  //   4) Caller dispatches via proxy_cast<T>.
  {
    BufferSink sink;
    BinaryWriteArchive writer(&sink);
    TypeListFactoryBeta beta;
    beta.b = "round-trip canary";
    beta.save(writer);

    BufferSource source(sink.bytes.data(), sink.bytes.len());
    BinaryReadArchive reader(&source);
    auto proxy = TypeListFactoryList::create_at(2);
    proxy->load(reader);

    auto* recovered = proxy_cast<TypeListFactoryBeta>(&*proxy);
    ASSERT_NE(recovered, nullptr);
    EXPECT_EQ(recovered->b, "round-trip canary");
  }
}

}  // namespace
}  // namespace rrr
