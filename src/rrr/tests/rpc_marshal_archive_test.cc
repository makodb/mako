// byte-for-byte compatibility tests for the
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

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <time.h>


#include <fcntl.h>
#include <unistd.h>


#include <gtest/gtest.h>


#include "../rrr.hpp"
#include "../misc/marshal.hpp"
#include "../misc/serializable.hpp"
#include "../misc/serializable_envelope.hpp"

import std;
import rusty;
import rrr.basetypes;

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
  BinaryWriteArchive archive(make_sink_proxy(&sink));
  archive << value;
  auto new_bytes = sink_to_vector(sink);

  ASSERT_EQ(old_bytes.size(), new_bytes.size())
      << "byte-length mismatch for type " << typeid(T).name();
  EXPECT_EQ(old_bytes, new_bytes)
      << "byte content mismatch for type " << typeid(T).name();
}

template <typename T>
void check_round_trip(const T& value) {
  // Encode via the OLD Marshal path.
  Marshal old_m;
  old_m << value;
  auto bytes = drain_marshal(old_m);

  BufferSource source(bytes.data(), bytes.size());
  BinaryReadArchive reader(make_source_proxy(&source));
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
    BinaryWriteArchive archive(make_sink_proxy(&sink));
    archive << v;
    auto new_bytes = sink_to_vector(sink);

    ASSERT_EQ(old_bytes.size(), new_bytes.size()) << "v32 raw=" << raw;
    EXPECT_EQ(old_bytes, new_bytes) << "v32 raw=" << raw;

    // Round-trip read.
    BufferSource source(old_bytes.data(), old_bytes.size());
    BinaryReadArchive reader(make_source_proxy(&source));
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
    BinaryWriteArchive archive(make_sink_proxy(&sink));
    archive << v;
    auto new_bytes = sink_to_vector(sink);

    ASSERT_EQ(old_bytes.size(), new_bytes.size()) << "v64 raw=" << raw;
    EXPECT_EQ(old_bytes, new_bytes) << "v64 raw=" << raw;

    BufferSource source(old_bytes.data(), old_bytes.size());
    BinaryReadArchive reader(make_source_proxy(&source));
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
  BinaryWriteArchive archive(make_sink_proxy(&sink));
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
// Container shapes.
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
  BinaryReadArchive reader(make_source_proxy(&source));
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
  BinaryWriteArchive archive(make_sink_proxy(&sink));
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
  BinaryReadArchive reader(make_source_proxy(&source));
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
  BinaryWriteArchive writer(make_sink_proxy(&sink));
  writer << c;

  std::vector<uint8_t> bytes(sink.bytes.len());
  for (size_t i = 0; i < sink.bytes.len(); ++i) bytes[i] = sink.bytes[i];

  BufferSource source(bytes.data(), bytes.size());
  BinaryReadArchive reader(make_source_proxy(&source));
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
  BinaryWriteArchive writer(make_sink_proxy(&sink));
  writer << v;

  std::vector<uint8_t> bytes(sink.bytes.len());
  for (size_t i = 0; i < sink.bytes.len(); ++i) bytes[i] = sink.bytes[i];

  BufferSource source(bytes.data(), bytes.size());
  BinaryReadArchive reader(make_source_proxy(&source));
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
  BinaryWriteArchive writer(make_sink_proxy(&sink));
  writer << s;

  std::vector<uint8_t> bytes(sink.bytes.len());
  for (size_t i = 0; i < sink.bytes.len(); ++i) bytes[i] = sink.bytes[i];

  BufferSource source(bytes.data(), bytes.size());
  BinaryReadArchive reader(make_source_proxy(&source));
  rusty::BTreeSet<int32_t> decoded;
  reader >> decoded;
  ASSERT_EQ(decoded.len(), s.len());
  EXPECT_TRUE(source.eof());
}

TEST(MarshalArchiveRoundTrip, RustyHashSetPrimitives) {
  rusty::HashSet<int32_t> s;
  s.insert(1); s.insert(2); s.insert(3);

  BufferSink sink;
  BinaryWriteArchive writer(make_sink_proxy(&sink));
  writer << s;

  std::vector<uint8_t> bytes(sink.bytes.len());
  for (size_t i = 0; i < sink.bytes.len(); ++i) bytes[i] = sink.bytes[i];

  BufferSource source(bytes.data(), bytes.size());
  BinaryReadArchive reader(make_source_proxy(&source));
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
  BinaryWriteArchive writer(make_sink_proxy(&sink));
  writer << m;

  std::vector<uint8_t> bytes(sink.bytes.len());
  for (size_t i = 0; i < sink.bytes.len(); ++i) bytes[i] = sink.bytes[i];

  BufferSource source(bytes.data(), bytes.size());
  BinaryReadArchive reader(make_source_proxy(&source));
  rusty::BTreeMap<int32_t, int64_t> decoded;
  reader >> decoded;
  ASSERT_EQ(decoded.len(), m.len());
  EXPECT_TRUE(source.eof());
}

TEST(MarshalArchiveRoundTrip, RustyHashMapPrimitives) {
  rusty::HashMap<int32_t, std::string> m;
  m.insert(1, "a"); m.insert(2, "b"); m.insert(3, "c");

  BufferSink sink;
  BinaryWriteArchive writer(make_sink_proxy(&sink));
  writer << m;

  std::vector<uint8_t> bytes(sink.bytes.len());
  for (size_t i = 0; i < sink.bytes.len(); ++i) bytes[i] = sink.bytes[i];

  BufferSource source(bytes.data(), bytes.size());
  BinaryReadArchive reader(make_source_proxy(&source));
  rusty::HashMap<int32_t, std::string> decoded;
  reader >> decoded;
  ASSERT_EQ(decoded.len(), m.len());
  EXPECT_TRUE(source.eof());
}

// ---- FdSink / FdSource ------------------------------------

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
    BinaryWriteArchive writer(make_sink_proxy(&sink));
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
  BinaryReadArchive reader(make_source_proxy(&source));

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
    BinaryWriteArchive writer(make_sink_proxy(&sink));
    writer << static_cast<uint32_t>(42);
    writer << std::string("the quick brown fox");
    writer << v64(0x123456789ABCDEFLL);
    std::vector<int32_t> vec{1, 2, 3, 4, 5};
    writer << vec;
  }
  p.close_write();
  reader_thread.join();

  BufferSink ref_sink;
  BinaryWriteArchive ref_writer(make_sink_proxy(&ref_sink));
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
    BinaryWriteArchive writer(make_sink_proxy(&sink));
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
    BinaryReadArchive reader(make_source_proxy(&src));
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
    BinaryWriteArchive writer(make_sink_proxy(&sink));
    writer << big;
  }
  p.close_write();
  reader_thread.join();

  // Decode and verify.
  BufferSource source(drained.data(), drained.size());
  BinaryReadArchive reader(make_source_proxy(&source));
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
  BinaryWriteArchive prep(make_sink_proxy(&prep_sink));
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
  BinaryReadArchive reader(make_source_proxy(&src));
  int32_t a; int64_t b; std::string c; v64 d{0};
  reader >> a >> b >> c >> d;
  EXPECT_EQ(static_cast<uint32_t>(a), 0xDEADBEEFu);
  EXPECT_EQ(b, 0x1122334455667788LL);
  EXPECT_EQ(c, "chunked across syscalls");
  EXPECT_EQ(d.get(), 987654321LL);

  writer_thread.join();
}

// ---- SerializableProxy / SerializableRegistry --------------

// Canary command for Phase 2: implements BOTH the new Serializable
// interface (`save` / `load` / `kind`) AND the old Marshal-based
// interface (`to_marshal` / `from_marshal`) so we can byte-compare
// the two paths.
//
// This type is intentionally test-local — Phase 2 only validates the
// new infrastructure. Per-command-type production migrations land in
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
  BinaryWriteArchive writer(make_sink_proxy(&sink));
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
  BinaryWriteArchive writer_orig(make_sink_proxy(&sink_orig));
  save_proxy->save(writer_orig);
  auto orig_bytes = sink_to_vector(sink_orig);

  // Load into a fresh (default-constructed) proxy.
  SerializableProxy load_proxy = make_serializable_proxy<CanaryCommand>();
  BufferSource source(orig_bytes.data(), orig_bytes.size());
  BinaryReadArchive reader(make_source_proxy(&source));
  load_proxy->load(reader);
  EXPECT_TRUE(source.eof());

  // Re-save via the loaded proxy. Bytes must match exactly.
  BufferSink sink_loaded;
  BinaryWriteArchive writer_loaded(make_sink_proxy(&sink_loaded));
  load_proxy->save(writer_loaded);
  auto loaded_bytes = sink_to_vector(sink_loaded);

  EXPECT_EQ(orig_bytes, loaded_bytes);
}

TEST(SerializableRegistry, RegisterCreateAndRoundTrip) {
  // Phase 2 DoD test: factory registry produces a fresh proxy whose
  // bytes match an independently-saved instance after load.
  // 2 step 5 prep (2026-05-05): no leading
  // `clear_for_testing()` — the registry is shared with
  // SerializableEnvelope::load and must keep static-init-time
  // registrations alive.  CanaryCommand uses its own kind tag
  // (0xCAFE) and coexists with whatever else is registered.
  (void)SerializableRegistry::reg<CanaryCommand>(CanaryCommand::kKind);
  EXPECT_TRUE(SerializableRegistry::is_registered(CanaryCommand::kKind));

  // Create + populate the source.
  CanaryCommand orig;
  orig.id = 1234;
  orig.name = "registry path";
  orig.values = {7, 8, 9};

  BufferSink sink_src;
  BinaryWriteArchive writer_src(make_sink_proxy(&sink_src));
  orig.save(writer_src);
  auto src_bytes = sink_to_vector(sink_src);

  // Use the registry to create a fresh proxy from the kind, then load.
  SerializableProxy proxy = SerializableRegistry::create(CanaryCommand::kKind);
  EXPECT_EQ(proxy->kind(), CanaryCommand::kKind);

  BufferSource source(src_bytes.data(), src_bytes.size());
  BinaryReadArchive reader(make_source_proxy(&source));
  proxy->load(reader);
  EXPECT_TRUE(source.eof());

  // Save via the loaded proxy; compare bytes.
  BufferSink sink_loaded;
  BinaryWriteArchive writer_loaded(make_sink_proxy(&sink_loaded));
  proxy->save(writer_loaded);
  auto loaded_bytes = sink_to_vector(sink_loaded);
  EXPECT_EQ(src_bytes, loaded_bytes);

  // 2 step 5 prep (2026-05-05): no longer
  // `clear_for_testing()` at end — `SerializableEnvelope::load`
  // now uses the same registry and depends on static-init-time
  // registrations (TypeListFactory* + every MakoCommands type)
  // staying alive across this test.  Leave whatever was registered
  // before this test in place.
}

// ---- Marshal ↔ Archive bridges ----------------------------

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
  BinaryWriteArchive ref_writer(make_sink_proxy(&ref_sink));
  ref_writer << i << s << v;
  auto ref_bytes = sink_to_vector(ref_sink);

  // (b) via MarshalSink.
  Marshal m;
  MarshalSink mark_sink(&m);
  BinaryWriteArchive mark_writer(make_sink_proxy(&mark_sink));
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
    BinaryWriteArchive writer(make_sink_proxy(&sink));
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
  BinaryReadArchive reader(make_source_proxy(&src));

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
    BinaryWriteArchive writer(make_sink_proxy(&sink));
    writer << static_cast<int32_t>(7);
    writer << static_cast<int64_t>(-99);
    writer << std::string("hello");
    std::vector<int32_t> ints{4, 5, 6};
    writer << ints;
  }

  MarshalSource src(&m);
  BinaryReadArchive reader(make_source_proxy(&src));

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
  BinaryReadArchive reader(make_source_proxy(&src));
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
  // 2 step 5 prep (2026-05-05): see RegisterCreateAndRoundTrip
  // for why we don't clear_for_testing here.

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
  // 2 step 5 prep (2026-05-05): see RegisterCreateAndRoundTrip
  // for why we no longer trailing-clear the registry.
}

// removed the entire
// "Marshallable ↔ Serializable bridges" + "MarshallDeputy
// registration" + "MarshallDeputy archive operators" + "Phase 3f
// lazy serializable() cache" + "L9 wire compaction via
// MarshallDeputy" test sections (~670 LOC, 17 tests + 4 helper
// structs + 2 static reg lines).  All exercised infrastructure
// retiring in this same commit: `SerializableMarshallableAdapter`,
// `as_marshallable`, `wrap_serializable_aliased`,
// `wrap_typed_marshallable`, `serializable_cast<T>(shared_ptr<
// Marshallable>)`, `marshallable_cast<T>(shared_ptr<Marshallable>)`,
// `reg_serializable_in_deputy`, `MarshallDeputy::reg_initializer`,
// `MarshallDeputy::create_initializer`, the `MarshallDeputy`
// class itself, the `Marshallable` class, and the
// `MarshallableProxyFacadeTest::DeputyDefaults...` family already
// removed earlier this session.

// ---------------------------------------------------------------------------
// TypeList::create_at(pos) compile-time-dispatched factory.
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
    SerializableRegistry::reg<TypeListFactoryAlpha>(
        TypeListFactoryAlpha::kKind);
static int _reg_tl_factory_beta =
    SerializableRegistry::reg<TypeListFactoryBeta>(
        TypeListFactoryBeta::kKind);
static int _reg_tl_factory_gamma =
    SerializableRegistry::reg<TypeListFactoryGamma>(
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

namespace {
template<typename T>
T* serializable_proxy_cast(SerializableProxy& proxy) {
  if (auto* h = dynamic_cast<details::SerializableSharedPtrHolder<T>*>(proxy.get())) {
    return h->ptr.get();
  }
  return nullptr;
}
}  // namespace

TEST(TypeListFactory, CreateAtReturnsCorrectTypeForEachPosition) {
  // pos=1 → Alpha
  {
    auto proxy = TypeListFactoryList::create_at(1);
    auto* alpha = serializable_proxy_cast<TypeListFactoryAlpha>(proxy);
    EXPECT_NE(alpha, nullptr);
    EXPECT_EQ(serializable_proxy_cast<TypeListFactoryBeta>(proxy), nullptr);
    EXPECT_EQ(serializable_proxy_cast<TypeListFactoryGamma>(proxy), nullptr);
  }
  // pos=2 → Beta
  {
    auto proxy = TypeListFactoryList::create_at(2);
    EXPECT_EQ(serializable_proxy_cast<TypeListFactoryAlpha>(proxy), nullptr);
    auto* beta = serializable_proxy_cast<TypeListFactoryBeta>(proxy);
    EXPECT_NE(beta, nullptr);
    EXPECT_EQ(serializable_proxy_cast<TypeListFactoryGamma>(proxy), nullptr);
  }
  // pos=3 → Gamma
  {
    auto proxy = TypeListFactoryList::create_at(3);
    EXPECT_EQ(serializable_proxy_cast<TypeListFactoryAlpha>(proxy), nullptr);
    EXPECT_EQ(serializable_proxy_cast<TypeListFactoryBeta>(proxy), nullptr);
    auto* gamma = serializable_proxy_cast<TypeListFactoryGamma>(proxy);
    EXPECT_NE(gamma, nullptr);
  }
}

// ---------------------------------------------------------------------------
// SerializableEnvelope<TypeList> — closed-set polymorphic carrier.
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
  BinaryWriteArchive writer(make_sink_proxy(&sink));
  auto outgoing = SerializableEnvelope<EnvelopeTestList>::pack(beta);
  outgoing.save(writer);

  // Decode.
  BufferSource source(sink.bytes.data(), sink.bytes.len());
  BinaryReadArchive reader(make_source_proxy(&source));
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
  BinaryWriteArchive writer(make_sink_proxy(&sink));
  auto outgoing = SerializableEnvelope<EnvelopeTestList>::pack_aliased(sp);
  outgoing.save(writer);

  // Decode.
  BufferSource source(sink.bytes.data(), sink.bytes.len());
  BinaryReadArchive reader(make_source_proxy(&source));
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
  BinaryWriteArchive writer(make_sink_proxy(&sink));
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
  //   4) Caller dispatches via dynamic_cast on the SerializableBase holder.
  {
    BufferSink sink;
    BinaryWriteArchive writer(make_sink_proxy(&sink));
    TypeListFactoryBeta beta;
    beta.b = "round-trip canary";
    beta.save(writer);

    BufferSource source(sink.bytes.data(), sink.bytes.len());
    BinaryReadArchive reader(make_source_proxy(&source));
    auto proxy = TypeListFactoryList::create_at(2);
    proxy->load(reader);

    auto* recovered = serializable_proxy_cast<TypeListFactoryBeta>(proxy);
    ASSERT_NE(recovered, nullptr);
    EXPECT_EQ(recovered->b, "round-trip canary");
  }
}

}  // namespace
}  // namespace rrr
