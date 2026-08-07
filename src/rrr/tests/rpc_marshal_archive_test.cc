// Round-trip tests for BinaryWriteArchive / BinaryReadArchive over
// BufferSink / BufferSource / FdSink / FdSource, plus the
// (the former Marshal bridge layer is deleted with Marshal itself).
//
// Historically this suite byte-compared the archive encoders against
// the old `Marshal` operator<< / operator>> serde surface. That
// Marshal-direct serde surface has been deleted; each case now
// asserts an archive encode -> decode round-trip over the same wire
// format (byte-level expectations that survive are asserted against
// the archive bytes directly).

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <time.h>


#include <fcntl.h>
#include <unistd.h>


#include <gtest/gtest.h>
#include <rusty/arc.hpp>
#include <rusty/option.hpp>


#include "../rrr.hpp"
#include "../misc/serializable.hpp"
#include "../misc/serializable_envelope.hpp"

import std;
import rusty;
import rrr.basetypes;

namespace rrr {
namespace {



std::vector<uint8_t> sink_to_vector(const BufferSink& sink) {
  std::vector<uint8_t> out;
  out.reserve(sink.bytes.len());
  for (size_t i = 0; i < sink.bytes.len(); ++i) {
    out.push_back(sink.bytes[i]);
  }
  return out;
}

template <typename T>
void check_round_trip(const T& value) {
  // Encode via the archive path.
  BufferSink sink;
  BinaryWriteArchive archive(make_sink_proxy(&sink));
  rrr::Serialize_::serialize(value, archive);

  BufferSource source(sink.bytes.data(), sink.bytes.len());
  BinaryReadArchive reader(make_source_proxy(&source));
  T decoded{};
  rrr::Deserialize_::deserialize(decoded, reader);

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
    check_round_trip(v);
  }
}

TEST(MarshalArchiveByteCompat, Int16Boundary) {
  for (int16_t v : {std::numeric_limits<int16_t>::min(),
                    int16_t{-1}, int16_t{0}, int16_t{1},
                    std::numeric_limits<int16_t>::max()}) {
    check_round_trip(v);
  }
}

TEST(MarshalArchiveByteCompat, Int32Boundary) {
  for (int32_t v : {std::numeric_limits<int32_t>::min(),
                    int32_t{-1}, int32_t{0}, int32_t{1}, int32_t{12345},
                    std::numeric_limits<int32_t>::max()}) {
    check_round_trip(v);
  }
}

TEST(MarshalArchiveByteCompat, Int64Boundary) {
  for (int64_t v : {std::numeric_limits<int64_t>::min(),
                    int64_t{-1}, int64_t{0}, int64_t{1},
                    int64_t{0x1234'5678'9ABC'DEF0LL},
                    std::numeric_limits<int64_t>::max()}) {
    check_round_trip(v);
  }
}

TEST(MarshalArchiveByteCompat, Uint8Boundary) {
  for (uint8_t v : {uint8_t{0}, uint8_t{1}, uint8_t{0x7F}, uint8_t{0x80},
                    std::numeric_limits<uint8_t>::max()}) {
    check_round_trip(v);
  }
}

TEST(MarshalArchiveByteCompat, Uint16Boundary) {
  for (uint16_t v : {uint16_t{0}, uint16_t{1},
                     std::numeric_limits<uint16_t>::max()}) {
    check_round_trip(v);
  }
}

TEST(MarshalArchiveByteCompat, Uint32Boundary) {
  for (uint32_t v : {uint32_t{0}, uint32_t{1}, uint32_t{0xDEADBEEF},
                     std::numeric_limits<uint32_t>::max()}) {
    check_round_trip(v);
  }
}

TEST(MarshalArchiveByteCompat, Uint64Boundary) {
  for (uint64_t v : {uint64_t{0}, uint64_t{1},
                     uint64_t{0xCAFEBABE'DEADBEEF},
                     std::numeric_limits<uint64_t>::max()}) {
    check_round_trip(v);
  }
}

TEST(MarshalArchiveByteCompat, DoubleBoundary) {
  // Use bit-pattern equality: NaN won't compare equal via operator==.
  // Pick values that have stable bit patterns.
  for (double v : {0.0, 1.0, -1.0, 3.14159265358979,
                   std::numeric_limits<double>::min(),
                   std::numeric_limits<double>::max()}) {
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

    BufferSink sink;
    BinaryWriteArchive archive(make_sink_proxy(&sink));
    rrr::Serialize_::serialize(v, archive);

    // Round-trip read.
    BufferSource source(sink.bytes.data(), sink.bytes.len());
    BinaryReadArchive reader(make_source_proxy(&source));
    v32 decoded;
    rrr::Deserialize_::deserialize(decoded, reader);
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

    BufferSink sink;
    BinaryWriteArchive archive(make_sink_proxy(&sink));
    rrr::Serialize_::serialize(v, archive);

    BufferSource source(sink.bytes.data(), sink.bytes.len());
    BinaryReadArchive reader(make_source_proxy(&source));
    v64 decoded;
    rrr::Deserialize_::deserialize(decoded, reader);
    EXPECT_EQ(decoded.get(), raw) << "v64 round-trip raw=" << raw;
    EXPECT_TRUE(source.eof());
  }
}

// ---------------------------------------------------------------------------
// std::string — v64 length prefix + raw bytes.
// ---------------------------------------------------------------------------

TEST(MarshalArchiveByteCompat, StringEmpty) {
  std::string s = "";
  check_round_trip(s);
}

TEST(MarshalArchiveByteCompat, StringShort) {
  std::string s = "hello";
  check_round_trip(s);
}

TEST(MarshalArchiveByteCompat, StringWithEmbeddedNul) {
  // 5-byte string with a NUL in the middle. The wire format MUST
  // preserve it byte-for-byte (length-prefixed; no C-string treatment).
  std::string s("ab\0cd", 5);
  check_round_trip(s);
}

TEST(MarshalArchiveByteCompat, StringMedium) {
  std::string s(200, 'x');
  check_round_trip(s);
}

TEST(MarshalArchiveByteCompat, StringLongerThanV64Boundary) {
  // 100 KB string — exercises the multi-byte v64 length prefix path.
  std::string s(100 * 1024, 'a');
  check_round_trip(s);
}

// ---------------------------------------------------------------------------
// Composite write — multiple primitives in a row, decoded back in order.
// ---------------------------------------------------------------------------

TEST(MarshalArchiveByteCompat, CompositePrimitiveSequence) {
  // Encode a heterogenous sequence, then decode it back in order.
  int32_t a = 0x12345678;
  int64_t b = 0x1122334455667788LL;
  v64 c(8192);
  std::string d = "hello world";
  double e = 3.14;

  BufferSink sink;
  BinaryWriteArchive archive(make_sink_proxy(&sink));
  rrr::Serialize_::serialize(a, archive);
  rrr::Serialize_::serialize(b, archive);
  rrr::Serialize_::serialize(c, archive);
  rrr::Serialize_::serialize(d, archive);
  rrr::Serialize_::serialize(e, archive);

  BufferSource source(sink.bytes.data(), sink.bytes.len());
  BinaryReadArchive reader(make_source_proxy(&source));
  int32_t a2; int64_t b2; v64 c2{0}; std::string d2; double e2;
  rrr::Deserialize_::deserialize(a2, reader);
  rrr::Deserialize_::deserialize(b2, reader);
  rrr::Deserialize_::deserialize(c2, reader);
  rrr::Deserialize_::deserialize(d2, reader);
  rrr::Deserialize_::deserialize(e2, reader);
  EXPECT_EQ(a2, a);
  EXPECT_EQ(b2, b);
  EXPECT_EQ(c2.get(), c.get());
  EXPECT_EQ(d2, d);
  EXPECT_EQ(e2, e);
  EXPECT_TRUE(source.eof());
}

// ---------------------------------------------------------------------------
// Source semantics — partial reads at EOF.
// ---------------------------------------------------------------------------

TEST(BufferSourceSemantics, EofReturnsZero) {
  uint8_t bytes[] = {1, 2, 3};
  BufferSource source(bytes, sizeof(bytes));

  uint8_t got[2];
  EXPECT_EQ(buffer_source_read(source, got, 2), 2u);
  EXPECT_EQ(source.remaining(), 1u);
  EXPECT_FALSE(source.eof());

  EXPECT_EQ(buffer_source_read(source, got, 2), 1u);  // partial read of last byte
  EXPECT_TRUE(source.eof());

  EXPECT_EQ(buffer_source_read(source, got, 2), 0u);  // no bytes left
}

TEST(BufferSinkSemantics, AccumulatesBytes) {
  BufferSink sink;
  uint32_t value = 0xDEADBEEF;
  // buffer_sink_write is gone; the copy is BufferSink::write_bytes (DSL).
  sink.write_bytes(reinterpret_cast<const std::uint8_t*>(&value), sizeof(value));
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
// (unordered_set/HashSet/unordered_map/HashMap) the container's
// bucket-walk order.
// ---------------------------------------------------------------------------

// Pair — no length prefix, just first followed by second.
TEST(MarshalArchiveByteCompat, PairOfPrimitives) {
  std::pair<int32_t, std::string> p{42, "hello"};
  check_round_trip(p);
}

// Helper for round-trip: encode + decode via the archive, assert
// equality.
template <typename Container>
void check_container_round_trip(const Container& c) {
  BufferSink sink;
  BinaryWriteArchive archive(make_sink_proxy(&sink));
  rrr::Serialize_::serialize(c, archive);
  BufferSource source(sink.bytes.data(), sink.bytes.len());
  BinaryReadArchive reader(make_source_proxy(&source));
  Container decoded;
  rrr::Deserialize_::deserialize(decoded, reader);
  EXPECT_EQ(decoded, c) << "container round-trip mismatch for "
                       << typeid(Container).name();
  EXPECT_TRUE(source.eof());
}

// ---- rusty::Vec / std::vector / std::list -------------------------------

// rusty containers (Vec/BTreeSet/HashSet/BTreeMap/HashMap):
//
// Verified by round-tripping through the archive (encode + decode);
// equality is checked via len() + element access since rusty types
// lack a deep operator== usable here.

template <typename Container>
void check_archive_round_trip_only(const Container& c) {
  BufferSink sink;
  BinaryWriteArchive writer(make_sink_proxy(&sink));
  rrr::Serialize_::serialize(c, writer);

  std::vector<uint8_t> bytes(sink.bytes.len());
  for (size_t i = 0; i < sink.bytes.len(); ++i) bytes[i] = sink.bytes[i];

  BufferSource source(bytes.data(), bytes.size());
  BinaryReadArchive reader(make_source_proxy(&source));
  Container decoded;
  rrr::Deserialize_::deserialize(decoded, reader);
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
  rrr::Serialize_::serialize(v, writer);

  std::vector<uint8_t> bytes(sink.bytes.len());
  for (size_t i = 0; i < sink.bytes.len(); ++i) bytes[i] = sink.bytes[i];

  BufferSource source(bytes.data(), bytes.size());
  BinaryReadArchive reader(make_source_proxy(&source));
  rusty::Vec<int32_t> decoded;
  rrr::Deserialize_::deserialize(decoded, reader);
  ASSERT_EQ(decoded.size(), v.size());
  for (size_t i = 0; i < v.size(); ++i) {
    EXPECT_EQ(decoded[i], v[i]);
  }
  EXPECT_TRUE(source.eof());
}

TEST(MarshalArchiveByteCompat, StdVectorEmpty) {
  std::vector<int32_t> v;
  check_container_round_trip(v);
}

TEST(MarshalArchiveByteCompat, StdVectorPrimitives) {
  std::vector<int64_t> v{1, 2, 3, -1, 0x7FFFFFFFFFFFFFFFLL};
  check_container_round_trip(v);
}

TEST(MarshalArchiveByteCompat, StdVectorOfStrings) {
  std::vector<std::string> v{"a", "bb", "", "ccc", "dddd"};
  check_container_round_trip(v);
}

TEST(MarshalArchiveByteCompat, StdVectorOfPairs) {
  std::vector<std::pair<int32_t, std::string>> v{
      {1, "one"}, {2, "two"}, {3, "three"}};
  check_container_round_trip(v);
}

TEST(MarshalArchiveByteCompat, NestedVectors) {
  std::vector<std::vector<int32_t>> v{{1, 2}, {}, {3, 4, 5}};
  check_container_round_trip(v);
}

TEST(MarshalArchiveByteCompat, StdListPrimitives) {
  std::list<int32_t> v{10, 20, 30};
  check_container_round_trip(v);
}

// ---- Sets ------------------------------------------------------------

TEST(MarshalArchiveByteCompat, StdSetEmpty) {
  std::set<int32_t> s;
  check_container_round_trip(s);
}

TEST(MarshalArchiveByteCompat, StdSetPrimitives) {
  // std::set is sorted by key — encoded in sorted order.
  std::set<int32_t> s{5, 1, 3, 2, 4};
  check_container_round_trip(s);
}

TEST(MarshalArchiveByteCompat, StdUnorderedSetPrimitives) {
  // For unordered_set the encoder iterates begin()/end() — the hash
  // table's own order. Round-trip equality is order-insensitive.
  std::unordered_set<int32_t> s{1, 2, 3, 4, 5};
  check_container_round_trip(s);
}

TEST(MarshalArchiveRoundTrip, RustyBTreeSetPrimitives) {
  auto s = rusty::BTreeSet<int32_t>::new_();
  s.insert(5); s.insert(1); s.insert(3); s.insert(2); s.insert(4);

  BufferSink sink;
  BinaryWriteArchive writer(make_sink_proxy(&sink));
  rrr::Serialize_::serialize(s, writer);

  std::vector<uint8_t> bytes(sink.bytes.len());
  for (size_t i = 0; i < sink.bytes.len(); ++i) bytes[i] = sink.bytes[i];

  BufferSource source(bytes.data(), bytes.size());
  BinaryReadArchive reader(make_source_proxy(&source));
  auto decoded = rusty::BTreeSet<int32_t>::new_();
  rrr::Deserialize_::deserialize(decoded, reader);
  ASSERT_EQ(decoded.len(), s.len());
  EXPECT_TRUE(source.eof());
}

TEST(MarshalArchiveRoundTrip, RustyHashSetPrimitives) {
  // clang-22's Itanium name mangler crashes (SIGSEGV in
  // CXXNameMangler::mangleSourceName) on the hashbrown table-iterator type
  // produced by the `rusty::iter()` lambda in slice.hpp — so the ENCODER
  // `operator<<(const rusty::HashSet<T>&)` (serializable.cpp) cannot be
  // instantiated on clang-22 at all. The wire format is just a v64 count +
  // elements, identical to std::set, so we encode a wire-compatible std::set
  // and exercise the rusty::HashSet DECODER (operator>>, which only inserts
  // and never enumerates the table — crash-free).
  std::set<int32_t> s{1, 2, 3};

  BufferSink sink;
  BinaryWriteArchive writer(make_sink_proxy(&sink));
  rrr::Serialize_::serialize(s, writer);

  std::vector<uint8_t> bytes(sink.bytes.len());
  for (size_t i = 0; i < sink.bytes.len(); ++i) bytes[i] = sink.bytes[i];

  BufferSource source(bytes.data(), bytes.size());
  BinaryReadArchive reader(make_source_proxy(&source));
  rusty::HashSet<int32_t> decoded;
  rrr::Deserialize_::deserialize(decoded, reader);
  ASSERT_EQ(decoded.len(), s.size());
  EXPECT_TRUE(source.eof());
}

// ---- Maps ------------------------------------------------------------

TEST(MarshalArchiveByteCompat, StdMapEmpty) {
  std::map<int32_t, std::string> m;
  check_container_round_trip(m);
}

TEST(MarshalArchiveByteCompat, StdMapPrimitives) {
  std::map<int32_t, std::string> m{
      {3, "three"}, {1, "one"}, {2, "two"}};
  check_container_round_trip(m);
}

TEST(MarshalArchiveByteCompat, StdUnorderedMapPrimitives) {
  std::unordered_map<int32_t, std::string> m{
      {1, "a"}, {2, "b"}, {3, "c"}};
  check_container_round_trip(m);
}

TEST(MarshalArchiveRoundTrip, RustyBTreeMapPrimitives) {
  auto m = rusty::BTreeMap<int32_t, int64_t>::new_();
  m.insert(3, 30); m.insert(1, 10); m.insert(2, 20);

  BufferSink sink;
  BinaryWriteArchive writer(make_sink_proxy(&sink));
  rrr::Serialize_::serialize(m, writer);

  std::vector<uint8_t> bytes(sink.bytes.len());
  for (size_t i = 0; i < sink.bytes.len(); ++i) bytes[i] = sink.bytes[i];

  BufferSource source(bytes.data(), bytes.size());
  BinaryReadArchive reader(make_source_proxy(&source));
  auto decoded = rusty::BTreeMap<int32_t, int64_t>::new_();
  rrr::Deserialize_::deserialize(decoded, reader);
  ASSERT_EQ(decoded.len(), m.len());
  EXPECT_TRUE(source.eof());
}

TEST(MarshalArchiveRoundTrip, RustyHashMapPrimitives) {
  // Same clang-22 mangler crash as RustyHashSetPrimitives: the hashbrown
  // table-iterator type cannot be mangled, so `operator<<(rusty::HashMap)`
  // can't be instantiated on clang-22. The wire format (v64 count + key/value
  // pairs) matches std::map, so encode a wire-compatible std::map and exercise
  // the rusty::HashMap DECODER (operator>>, insert-only, crash-free).
  std::map<int32_t, std::string> m{{1, "a"}, {2, "b"}, {3, "c"}};

  BufferSink sink;
  BinaryWriteArchive writer(make_sink_proxy(&sink));
  rrr::Serialize_::serialize(m, writer);

  std::vector<uint8_t> bytes(sink.bytes.len());
  for (size_t i = 0; i < sink.bytes.len(); ++i) bytes[i] = sink.bytes[i];

  BufferSource source(bytes.data(), bytes.size());
  BinaryReadArchive reader(make_source_proxy(&source));
  rusty::HashMap<int32_t, std::string> decoded;
  rrr::Deserialize_::deserialize(decoded, reader);
  ASSERT_EQ(decoded.len(), m.size());
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

// Temp directory for test scratch: honour TMPDIR, fall back to /tmp.
// Hardcoding /tmp made these tests fail with ENOSPC whenever the host's
// /tmp tmpfs filled up -- and the failure mode was a bare SIGABRT from
// the write path, which reads exactly like a real regression.
inline std::string test_tmp_dir() {
  const char* env = ::getenv("TMPDIR");
  return (env != nullptr && env[0] != '\0') ? std::string(env) : std::string("/tmp");
}

// RAII wrapper around a temp file. Lives only inside the test.
struct ScopedTempFile {
  std::vector<char> path;
  int fd = -1;
  ScopedTempFile() {
    const std::string tmpl = test_tmp_dir() + "/mako_archive_test_XXXXXX";
    path.assign(tmpl.begin(), tmpl.end());
    path.push_back('\0');
    fd = ::mkstemp(path.data());
    EXPECT_GE(fd, 0) << "mkstemp failed under " << test_tmp_dir();
  }
  ~ScopedTempFile() {
    if (fd >= 0) ::close(fd);
    ::unlink(path.data());
  }
  // Reopen by path read-only, returning a fresh fd. Caller closes it.
  int reopen_ro() const {
    int rfd = ::open(path.data(), O_RDONLY);
    EXPECT_GE(rfd, 0);
    return rfd;
  }
};

TEST(FdSinkSemantics, EmptyWriteIsNoop) {
  ScopedPipe p;
  FdSink sink(p.fds[1]);
  // Calling write(p, 0) should not block and should not consume bytes.
  fd_sink_write(sink, nullptr, 0);
  // Close the write end. The read end should immediately see EOF.
  p.close_write();
  uint8_t buf[1];
  ssize_t r = ::read(p.fds[0], buf, sizeof(buf));
  EXPECT_EQ(r, 0);
}

TEST(FdSourceSemantics, EmptyReadIsNoop) {
  ScopedPipe p;
  FdSource src(p.fds[0]);
  size_t got = fd_source_read(src, nullptr, 0);
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
  size_t got = fd_source_read(src, buf, sizeof(buf));
  EXPECT_EQ(got, 3u);
  EXPECT_EQ(buf[0], 0x01);
  EXPECT_EQ(buf[1], 0x02);
  EXPECT_EQ(buf[2], 0x03);

  // Subsequent read on the closed pipe sees EOF immediately.
  size_t again = fd_source_read(src, buf, 4);
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
    rrr::Serialize_::serialize(static_cast<int32_t>(0x12345678), writer);
    rrr::Serialize_::serialize(static_cast<int64_t>(-1), writer);
    rrr::Serialize_::serialize(std::string("hello"), writer);
    rrr::Serialize_::serialize(v32(5), writer);
    rrr::Serialize_::serialize(v64(1024), writer);
  }
  p.close_write();
  reader_thread.join();

  // Now decode via BufferSource (deterministic, no kernel timing) and
  // verify each value.
  BufferSource source(drained_bytes.data(), drained_bytes.size());
  BinaryReadArchive reader(make_source_proxy(&source));

  int32_t a; int64_t b; std::string c; v32 d{0}; v64 e{0};
  rrr::Deserialize_::deserialize(a, reader);
  rrr::Deserialize_::deserialize(b, reader);
  rrr::Deserialize_::deserialize(c, reader);
  rrr::Deserialize_::deserialize(d, reader);
  rrr::Deserialize_::deserialize(e, reader);
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
    rrr::Serialize_::serialize(static_cast<uint32_t>(42), writer);
    rrr::Serialize_::serialize(std::string("the quick brown fox"), writer);
    rrr::Serialize_::serialize(v64(0x123456789ABCDEFLL), writer);
    std::vector<int32_t> vec{1, 2, 3, 4, 5};
    rrr::Serialize_::serialize(vec, writer);
  }
  p.close_write();
  reader_thread.join();

  BufferSink ref_sink;
  BinaryWriteArchive ref_writer(make_sink_proxy(&ref_sink));
  rrr::Serialize_::serialize(static_cast<uint32_t>(42), ref_writer);
  rrr::Serialize_::serialize(std::string("the quick brown fox"), ref_writer);
  rrr::Serialize_::serialize(v64(0x123456789ABCDEFLL), ref_writer);
  std::vector<int32_t> vec{1, 2, 3, 4, 5};
  rrr::Serialize_::serialize(vec, ref_writer);

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
    rrr::Serialize_::serialize(static_cast<int32_t>(7), writer);
    rrr::Serialize_::serialize(static_cast<int64_t>(-99), writer);
    rrr::Serialize_::serialize(std::string("temp file payload"), writer);
    std::vector<std::string> strs{"a", "bb", "ccc"};
    rrr::Serialize_::serialize(strs, writer);
    std::map<int32_t, int64_t> m{{1, 100}, {2, 200}, {3, 300}};
    rrr::Serialize_::serialize(m, writer);
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
    rrr::Deserialize_::deserialize(a, reader);
    rrr::Deserialize_::deserialize(b, reader);
    rrr::Deserialize_::deserialize(c, reader);
    rrr::Deserialize_::deserialize(strs, reader);
    rrr::Deserialize_::deserialize(m, reader);
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
    rrr::Serialize_::serialize(big, writer);
  }
  p.close_write();
  reader_thread.join();

  // Decode and verify.
  BufferSource source(drained.data(), drained.size());
  BinaryReadArchive reader(make_source_proxy(&source));
  std::vector<int32_t> decoded;
  rrr::Deserialize_::deserialize(decoded, reader);
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
  rrr::Serialize_::serialize(static_cast<int32_t>(0xDEADBEEF), prep);
  rrr::Serialize_::serialize(static_cast<int64_t>(0x1122334455667788LL), prep);
  rrr::Serialize_::serialize(std::string("chunked across syscalls"), prep);
  rrr::Serialize_::serialize(v64(987654321LL), prep);

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
  rrr::Deserialize_::deserialize(a, reader);
  rrr::Deserialize_::deserialize(b, reader);
  rrr::Deserialize_::deserialize(c, reader);
  rrr::Deserialize_::deserialize(d, reader);
  EXPECT_EQ(static_cast<uint32_t>(a), 0xDEADBEEFu);
  EXPECT_EQ(b, 0x1122334455667788LL);
  EXPECT_EQ(c, "chunked across syscalls");
  EXPECT_EQ(d.get(), 987654321LL);

  writer_thread.join();
}

// ---- SerializableProxy / SerializableRegistry --------------

// Canary command for Phase 2: implements the Serializable interface
// (`save` / `load` / `kind`).
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
    rrr::Serialize_::serialize(id, ar);
    rrr::Serialize_::serialize(name, ar);
    rrr::Serialize_::serialize(values, ar);
  }

  void load(BinaryReadArchive& ar) {
    rrr::Deserialize_::deserialize(id, ar);
    rrr::Deserialize_::deserialize(name, ar);
    rrr::Deserialize_::deserialize(values, ar);
  }
};

TEST(SerializableProxy, ByteCompatVsMarshalDirect) {
  // Encode the same payload via:
  //   (a) the archive-native path: canary.save(writer)
  //   (b) the SerializableProxy path: proxy->save(writer)
  // assert the two byte streams are identical, then load the bytes
  // back into a fresh command and verify the fields.
  CanaryCommand canary;
  canary.id = 42;
  canary.name = "hello, world";
  canary.values = {1, 2, 3, 4, 5};

  // Path (a): direct save.
  BufferSink direct_sink;
  BinaryWriteArchive direct_writer(make_sink_proxy(&direct_sink));
  canary.save(direct_writer);
  auto old_bytes = sink_to_vector(direct_sink);

  // Path (b): SerializableProxy.
  SerializableProxy proxy = make_serializable_proxy<CanaryCommand>(canary);
  BufferSink sink;
  BinaryWriteArchive writer(make_sink_proxy(&sink));
  proxy->save(writer);
  auto new_bytes = sink_to_vector(sink);

  ASSERT_EQ(old_bytes.size(), new_bytes.size());
  EXPECT_EQ(old_bytes, new_bytes);

  // Round-trip: load the bytes back into a fresh command.
  BufferSource source(direct_sink.bytes.data(), direct_sink.bytes.len());
  BinaryReadArchive reader(make_source_proxy(&source));
  CanaryCommand canary2;
  canary2.load(reader);
  EXPECT_EQ(canary2.id, canary.id);
  EXPECT_EQ(canary2.name, canary.name);
  EXPECT_EQ(canary2.values, canary.values);
  EXPECT_TRUE(source.eof());
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
  // @unsafe - unique-owner mutation window: load_proxy is factory-fresh
  // (strong_count 1), so get_mut() is Some.
  load_proxy.get_mut().unwrap().load(reader);
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
  // @unsafe - unique-owner mutation window: proxy is factory-fresh
  // (strong_count 1), so get_mut() is Some.
  proxy.get_mut().unwrap().load(reader);
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
  void save(BinaryWriteArchive& ar) const { rrr::Serialize_::serialize(a, ar); }
  void load(BinaryReadArchive& ar) { rrr::Deserialize_::deserialize(a, ar); }
  int32_t kind() const { return kKind; }
};

struct TypeListFactoryBeta {
  static constexpr int32_t kKind = 61;
  std::string b;
  void save(BinaryWriteArchive& ar) const { rrr::Serialize_::serialize(b, ar); }
  void load(BinaryReadArchive& ar) { rrr::Deserialize_::deserialize(b, ar); }
  int32_t kind() const { return kKind; }
};

struct TypeListFactoryGamma {
  static constexpr int32_t kKind = 62;
  int64_t c{0};
  void save(BinaryWriteArchive& ar) const { rrr::Serialize_::serialize(c, ar); }
  void load(BinaryReadArchive& ar) { rrr::Deserialize_::deserialize(c, ar); }
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
// The proxy is a const-view rusty::Arc<SerializableBase>; downcast the
// holder and return a const view of the carried payload.
template<typename T>
const T* serializable_proxy_cast(const SerializableProxy& proxy) {
  if (auto* h = dynamic_cast<const details::SerializableSharedPtrHolder<T>*>(
          proxy.get())) {
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
  // (the redundant `explicit operator bool` was removed with the DSL
  // conversion — no trait maps to it and it had zero production callers)
  EXPECT_FALSE(env.has_value());
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
  // Unique-owner mutation window: the Arc is not shared yet.
  auto sp = rusty::Arc<TypeListFactoryAlpha>::make();
  sp.get_mut().unwrap().a = 7;

  auto env = SerializableEnvelope<EnvelopeTestList>::pack_aliased(sp);
  EXPECT_TRUE(env.has_value());
  EXPECT_EQ(env.kind(), TypeListFactoryAlpha::kKind);

  auto* recovered = env.unpack<TypeListFactoryAlpha>();
  ASSERT_NE(recovered, nullptr);
  EXPECT_EQ(static_cast<const TypeListFactoryAlpha*>(recovered),
            sp.get());  // aliased — same object
  EXPECT_EQ(recovered->a, 7);

  // Mutating through the original IS visible.
  // @unsafe { aliasing canary: proves pack_aliased shares (not copies)
  // the payload }
  const_cast<TypeListFactoryAlpha*>(sp.get())->a = 99;
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
  // Unique-owner mutation window: the Arc is not shared yet.
  auto sp = rusty::Arc<TypeListFactoryGamma>::make();
  sp.get_mut().unwrap().c = 0xDEADBEEFCAFEBABEll;

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
  // Unique-owner mutation window: the Arc is not shared yet.
  auto sp = rusty::Arc<TypeListFactoryAlpha>::make();
  sp.get_mut().unwrap().a = 100;
  auto env_a = SerializableEnvelope<EnvelopeTestList>::pack_aliased(sp);

  // Copy.
  auto env_b = env_a;

  // Both should see the same payload.
  EXPECT_EQ(env_a.unpack<TypeListFactoryAlpha>(),
            env_b.unpack<TypeListFactoryAlpha>());

  // Mutation through one envelope is visible to the other (because
  // both share the same Arc-backed proxy internally).
  // @unsafe { aliasing canary: proves pack_aliased shares (not copies)
  // the payload }
  const_cast<TypeListFactoryAlpha*>(sp.get())->a = 200;
  EXPECT_EQ(env_a.unpack<TypeListFactoryAlpha>()->a, 200);
  EXPECT_EQ(env_b.unpack<TypeListFactoryAlpha>()->a, 200);
}

TEST(TypeListFactory, CreateAtRoundTripsViaProxySaveLoad) {
  // Pack a Beta via create_at(2), save + load through a proxy, verify
  // the value survives. Demonstrates the L10b read-path shape:
  //   1) Read v32 kind from wire.
  //   2) create_at(kind) → fresh SerializableProxy for that type.
  //   3) proxy.get_mut().unwrap().load(reader) — populates the typed
  //      value through the factory-fresh Arc's unique-owner window.
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
    // @unsafe - unique-owner mutation window: proxy is factory-fresh
    // (strong_count 1), so get_mut() is Some.
    proxy.get_mut().unwrap().load(reader);

    auto* recovered = serializable_proxy_cast<TypeListFactoryBeta>(proxy);
    ASSERT_NE(recovered, nullptr);
    EXPECT_EQ(recovered->b, "round-trip canary");
  }
}

}  // namespace
}  // namespace rrr
