#pragma once

// Marshal Archive — serde / cereal-style serialization layer.
//
// Workstream N Phase 1: parallel to the existing `Marshal` /
// `Marshallable` system, decoupling format (how bytes are laid out)
// from target (where bytes go). Built atop `pro::proxy` for
// type-erased Sink/Source dispatch.
//
// Wire format byte layout matches the existing `Marshal` operator<< /
// operator>> output exactly — verified by `rpc_marshal_archive_test.cc`.
//
// Layered design (see docs/dev/marshal_archive_design.md):
//
//   Layer 1+2: SinkProxy / SourceProxy — type-erased byte sinks/sources.
//              Concrete impls in this file: BufferSink, BufferSource,
//              FdSink, FdSource.
//
//   Layer 3:   BinaryWriteArchive / BinaryReadArchive — knows the wire
//              format, holds a Sink/Source proxy, exposes operator<< /
//              operator>> for primitives.
//
//   Layer 4:   SerializableProxy — Phase 2.

// import std; replacement.
#include <std_compat.hpp>

// @c-compat-added
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <unistd.h>

#include <list>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <rusty/rusty.hpp>
#include <rusty/btreemap.hpp>
#include <rusty/btreeset.hpp>
#include <rusty/fn.hpp>
#include <rusty/hashmap.hpp>
#include <rusty/hashset.hpp>
#include <rusty/vec.hpp>

#ifdef RR
#pragma push_macro("RR")
#undef RR
#define RRR_MARSHAL_ARCHIVE_RESTORE_RR_MACRO 1
#endif
#include <proxy/proxy.h>
#include <proxy/proxy_macros.h>
#ifdef RRR_MARSHAL_ARCHIVE_RESTORE_RR_MACRO
#pragma pop_macro("RR")
#undef RRR_MARSHAL_ARCHIVE_RESTORE_RR_MACRO
#endif

#include "../base/all.hpp"

namespace rrr {

// Forward declaration so Phase 3a's `MarshalSink` / `MarshalSource`
// can hold a `Marshal*` without dragging the heavy `marshal.hpp`
// header into every translation unit that uses the new archive
// system. Method bodies for those classes live in
// `marshal_archive.cpp`.
class Marshal;

// ---------------------------------------------------------------------------
// Layer 1+2: Sink / Source pro::proxy facades.
// ---------------------------------------------------------------------------

PRO_DEF_MEM_DISPATCH(SinkMemWrite, write);
PRO_DEF_MEM_DISPATCH(SourceMemRead, read);

// Sink: anything that can accept (const void*, size_t) bytes.
//
// Convention is fire-and-forget. Concrete sinks may flush lazily;
// callers that need durability must call sink-specific flush methods
// before observing (FdSink will drain on destruction).
//
// `pro::skills::indirect_rtti` enables `proxy_cast<Adapter>(*proxy)`
// so callers can recover the concrete adapter type (used by the
// MarshallDeputy archive operators in marshal_serializable_bridge.hpp
// to detect a Marshal-backed sink/source and short-circuit through
// the existing legacy operator<<>>).
struct SinkFacade : pro::facade_builder
    ::add_convention<SinkMemWrite, void(const void*, size_t)>
    ::add_skill<pro::skills::indirect_rtti>
    ::build {};
using SinkProxy = pro::proxy<SinkFacade>;

// Source: returns the number of bytes actually read (may be < n at EOF).
//
// Convention: returns 0 at EOF; raises (or aborts) on transport error.
// Concrete sources control their own buffering / blocking semantics.
struct SourceFacade : pro::facade_builder
    ::add_convention<SourceMemRead, size_t(void*, size_t)>
    ::add_skill<pro::skills::indirect_rtti>
    ::build {};
using SourceProxy = pro::proxy<SourceFacade>;

// ---------------------------------------------------------------------------
// Concrete Sink / Source — Phase 1a: in-memory buffer only.
// ---------------------------------------------------------------------------

// In-memory byte sink. Bytes accumulate in `bytes` and can be observed
// after each write or at the end. Mostly used for tests + as the
// internal buffer for cases where Marshal-style accumulation is needed.
class BufferSink {
 public:
  rusty::Vec<uint8_t> bytes;

  // @safe - delegates to Vec::push, no raw pointer ops.
  void write(const void* p, size_t n) {
    // @unsafe { reading from the const void* p }
    {
      const auto* b = static_cast<const uint8_t*>(p);
      bytes.reserve(bytes.len() + n);
      for (size_t i = 0; i < n; ++i) {
        bytes.push(b[i]);
      }
    }
  }

  // Allow callers to reset between encodings without reallocating.
  void clear() noexcept { bytes.clear(); }
};

// In-memory byte source. Wraps a `const uint8_t*` view + length;
// caller owns the underlying storage.
//
// Returns the number of bytes actually copied. At EOF (pos_ == len_)
// returns 0; partial reads at the tail are allowed (not aborted).
class BufferSource {
  const uint8_t* data_;
  size_t         len_;
  size_t         pos_;
 public:
  BufferSource(const void* data, size_t len) noexcept
      : data_(static_cast<const uint8_t*>(data)), len_(len), pos_(0) {}

  // @unsafe - raw pointer read.
  size_t read(void* p, size_t n) {
    // @unsafe { memcpy from data_ + pos_ }
    {
      size_t avail = len_ - pos_;
      size_t take  = (n < avail) ? n : avail;
      if (take > 0) {
        std::memcpy(p, data_ + pos_, take);
        pos_ += take;
      }
      return take;
    }
  }

  size_t pos()       const noexcept { return pos_; }
  size_t remaining() const noexcept { return len_ - pos_; }
  bool   eof()       const noexcept { return pos_ >= len_; }
};

// Adapter wrappers for the proxy facades.
//
// pro::proxy expects an "adapter" value that satisfies the convention
// methods — passing a raw `BufferSink*` directly does not match, so
// we wrap the pointer in a small forwarding adapter (mirroring the
// pattern used by `TcpConnectionChannelAdapter` etc. in tcp_channel.hpp).
//
// Lifetime: the proxy must not outlive `*sink` / `*source`.
class BufferSinkAdapter {
  BufferSink* sink_;
 public:
  explicit BufferSinkAdapter(BufferSink* s) noexcept : sink_(s) {}
  void write(const void* p, size_t n) { sink_->write(p, n); }
};

class BufferSourceAdapter {
  BufferSource* source_;
 public:
  explicit BufferSourceAdapter(BufferSource* s) noexcept : source_(s) {}
  size_t read(void* p, size_t n) { return source_->read(p, n); }
};

inline SinkProxy make_sink_proxy(BufferSink* sink) {
  return pro::make_proxy<SinkFacade, BufferSinkAdapter>(sink);
}
inline SourceProxy make_source_proxy(BufferSource* source) {
  return pro::make_proxy<SourceFacade, BufferSourceAdapter>(source);
}

// ---------------------------------------------------------------------------
// File descriptor Sink / Source (Phase 1c).
//
// FdSink::write — full-write loop. Calls ::write in a loop until n bytes
// have been written, retrying transparently on EINTR. On any other
// transport error (or unexpected ::write returning 0) it aborts via
// `verify`.
//
// FdSource::read — full-read loop. Calls ::read in a loop until n bytes
// have been read or EOF is reached. Returns the number of bytes
// actually read (may be < n at EOF). Retries on EINTR. Aborts on any
// other transport error.
//
// Lifetime: FdSink/FdSource hold a non-owning fd. Caller owns the fd
// and must keep it open for the lifetime of the Sink/Source. Same
// convention as BufferSink (caller owns the underlying storage).
//
// Threading: not thread-safe. Caller is responsible for serializing
// concurrent access (e.g. via SpinMutex around the Sink) if shared.
// ---------------------------------------------------------------------------

class FdSink {
  int fd_;
 public:
  explicit FdSink(int fd) noexcept : fd_(fd) {}

  int fd() const noexcept { return fd_; }

  // @safe - the only raw-pointer op is ::write itself, which is
  // annotated below.
  void write(const void* p, size_t n) {
    if (n == 0) return;
    const auto* b = static_cast<const uint8_t*>(p);
    size_t written = 0;
    while (written < n) {
      // @unsafe { ::write — raw libc syscall on a fd we don't own }
      ssize_t r = ::write(fd_, b + written, n - written);
      if (r < 0) {
        if (errno == EINTR) continue;
        verify(false);
      }
      verify(r > 0);
      written += static_cast<size_t>(r);
    }
  }
};

class FdSource {
  int fd_;
 public:
  explicit FdSource(int fd) noexcept : fd_(fd) {}

  int fd() const noexcept { return fd_; }

  // @safe - the only raw-pointer op is ::read itself, annotated below.
  size_t read(void* p, size_t n) {
    if (n == 0) return 0;
    auto* b = static_cast<uint8_t*>(p);
    size_t got = 0;
    while (got < n) {
      // @unsafe { ::read — raw libc syscall on a fd we don't own }
      ssize_t r = ::read(fd_, b + got, n - got);
      if (r < 0) {
        if (errno == EINTR) continue;
        verify(false);
      }
      if (r == 0) break;  // EOF — return short read.
      got += static_cast<size_t>(r);
    }
    return got;
  }
};

class FdSinkAdapter {
  FdSink* sink_;
 public:
  explicit FdSinkAdapter(FdSink* s) noexcept : sink_(s) {}
  void write(const void* p, size_t n) { sink_->write(p, n); }
};

class FdSourceAdapter {
  FdSource* source_;
 public:
  explicit FdSourceAdapter(FdSource* s) noexcept : source_(s) {}
  size_t read(void* p, size_t n) { return source_->read(p, n); }
};

inline SinkProxy make_sink_proxy(FdSink* sink) {
  return pro::make_proxy<SinkFacade, FdSinkAdapter>(sink);
}
inline SourceProxy make_source_proxy(FdSource* source) {
  return pro::make_proxy<SourceFacade, FdSourceAdapter>(source);
}

// ---------------------------------------------------------------------------
// Marshal ↔ Archive bridges (Phase 3a).
//
// MarshalSink wraps an `rrr::Marshal*` and forwards `write(p, n)` to
// `Marshal::write(p, n)`, so new `BinaryWriteArchive`-based code can
// emit bytes directly into an existing `Marshal` buffer without the
// caller having to allocate a separate `BufferSink` and copy.
//
// MarshalSource is the dual: wraps a `Marshal*` and forwards
// `read(p, n)` to `Marshal::read(p, n)`. Wire format is byte-for-byte
// identical (Phase 1 commitment), so a Marshal accumulated by old
// `Marshal::operator<<` calls can be drained by a `BinaryReadArchive`
// over `MarshalSource` and produce the same decoded values.
//
// Lifetime: non-owning. Caller owns the underlying `Marshal` and must
// keep it alive for the lifetime of the Sink/Source.
//
// Method bodies live in `marshal_archive.cpp` to avoid pulling
// `marshal.hpp` into every translation unit that uses this header.
// ---------------------------------------------------------------------------

class MarshalSink {
  Marshal* m_;
 public:
  explicit MarshalSink(Marshal* m) noexcept : m_(m) {}

  Marshal* marshal() const noexcept { return m_; }

  // @unsafe - delegates to Marshal::write; verifies the underlying
  // chunk allocator accepted all n bytes.
  void write(const void* p, size_t n);
};

class MarshalSource {
  Marshal* m_;
 public:
  explicit MarshalSource(Marshal* m) noexcept : m_(m) {}

  Marshal* marshal() const noexcept { return m_; }

  // Returns the number of bytes actually read. May return < n at EOF
  // (consistent with BufferSource / FdSource).
  size_t read(void* p, size_t n);
};

class MarshalSinkAdapter {
  MarshalSink* sink_;
 public:
  explicit MarshalSinkAdapter(MarshalSink* s) noexcept : sink_(s) {}
  void write(const void* p, size_t n) { sink_->write(p, n); }

  // Symmetric with MarshalSourceAdapter::source(); exposed for the
  // MarshallDeputy archive operator<< (currently it doesn't need
  // this — save flows through the M→S adapter chain — but keeping
  // the API parallel for future use).
  MarshalSink* sink() const noexcept { return sink_; }
};

class MarshalSourceAdapter {
  MarshalSource* source_;
 public:
  explicit MarshalSourceAdapter(MarshalSource* s) noexcept : source_(s) {}
  size_t read(void* p, size_t n) { return source_->read(p, n); }

  // Used by the MarshallDeputy archive operator>> to recover the
  // underlying Marshal — the operator detours through legacy
  // operator>>(Marshal&, MarshallDeputy&) since the MarshallDeputy
  // wire format lacks a length prefix at the payload-bytes layer.
  MarshalSource* source() const noexcept { return source_; }
};

inline SinkProxy make_sink_proxy(MarshalSink* sink) {
  return pro::make_proxy<SinkFacade, MarshalSinkAdapter>(sink);
}
inline SourceProxy make_source_proxy(MarshalSource* source) {
  return pro::make_proxy<SourceFacade, MarshalSourceAdapter>(source);
}

// ---------------------------------------------------------------------------
// Layer 3: Binary archive — knows the wire format.
//
// Wire format (BYTE-FOR-BYTE COMPATIBLE with the existing `Marshal`
// operator<< / operator>>):
//
//   uint8_t / int8_t  : 1 byte raw
//   uint16_t / int16_t: 2 bytes raw, host byte order (little-endian on
//                       supported platforms — same as Marshal)
//   uint32_t / int32_t: 4 bytes raw, host byte order
//   uint64_t / int64_t: 8 bytes raw, host byte order
//   double            : 8 bytes raw, host byte order
//   v32 / v64         : SparseInt::dump variable-length encoding
//                       (1-5 bytes for v32, 1-9 bytes for v64)
//   std::string       : v64 length prefix followed by raw bytes
//
// The archive is non-templated in the sink — it holds a `SinkProxy`
// which is type-erased at runtime. The cost is one virtual call per
// primitive write (negligible vs. the actual I/O).
// ---------------------------------------------------------------------------

class BinaryWriteArchive {
  SinkProxy sink_;
 public:
  explicit BinaryWriteArchive(SinkProxy sink) noexcept : sink_(std::move(sink)) {}

  // Convenience: build directly atop a concrete BufferSink.
  explicit BinaryWriteArchive(BufferSink* sink)
      : sink_(make_sink_proxy(sink)) {}

  // Convenience: build directly atop a concrete FdSink.
  explicit BinaryWriteArchive(FdSink* sink)
      : sink_(make_sink_proxy(sink)) {}

  // Convenience: build directly atop a concrete MarshalSink (Phase 3a).
  explicit BinaryWriteArchive(MarshalSink* sink)
      : sink_(make_sink_proxy(sink)) {}

  // Expose the inner SinkProxy so callers can use proxy_cast to
  // recover the concrete adapter type (e.g.
  // `proxy_cast<MarshalSinkAdapter>(*archive.sink())` to detect a
  // Marshal-backed sink). Used by the MarshallDeputy archive
  // operators in marshal_serializable_bridge.hpp.
  SinkProxy& sink() noexcept { return sink_; }
  const SinkProxy& sink() const noexcept { return sink_; }

  // Emit raw bytes (used for unstructured payloads).
  void write_bytes(const void* p, size_t n) { sink_->write(p, n); }

  // ---- Fixed-width primitives. ------------------------------------------
  BinaryWriteArchive& operator<<(int8_t v)   { sink_->write(&v, sizeof(v)); return *this; }
  BinaryWriteArchive& operator<<(int16_t v)  { sink_->write(&v, sizeof(v)); return *this; }
  BinaryWriteArchive& operator<<(int32_t v)  { sink_->write(&v, sizeof(v)); return *this; }
  BinaryWriteArchive& operator<<(int64_t v)  { sink_->write(&v, sizeof(v)); return *this; }
  BinaryWriteArchive& operator<<(uint8_t v)  { sink_->write(&v, sizeof(v)); return *this; }
  BinaryWriteArchive& operator<<(uint16_t v) { sink_->write(&v, sizeof(v)); return *this; }
  BinaryWriteArchive& operator<<(uint32_t v) { sink_->write(&v, sizeof(v)); return *this; }
  BinaryWriteArchive& operator<<(uint64_t v) { sink_->write(&v, sizeof(v)); return *this; }
  BinaryWriteArchive& operator<<(double v)   { sink_->write(&v, sizeof(v)); return *this; }

  // ---- Variable-length integer encoding (SparseInt). --------------------
  BinaryWriteArchive& operator<<(rrr::v32 v) {
    char buf[5];
    size_t bsize = rrr::SparseInt::dump(v.get(), buf);
    sink_->write(buf, bsize);
    return *this;
  }

  BinaryWriteArchive& operator<<(rrr::v64 v) {
    char buf[9];
    size_t bsize = rrr::SparseInt::dump(v.get(), buf);
    sink_->write(buf, bsize);
    return *this;
  }

  // ---- Variable-length byte sequences. ----------------------------------
  BinaryWriteArchive& operator<<(std::string_view s) {
    rrr::v64 v_len = static_cast<rrr::i64>(s.size());
    *this << v_len;
    if (s.size() > 0) {
      sink_->write(s.data(), s.size());
    }
    return *this;
  }

  // std::string is a convenience overload — same wire format as
  // string_view (length-prefixed bytes).
  BinaryWriteArchive& operator<<(const std::string& s) {
    return *this << std::string_view{s};
  }

  // ---- Composites. ------------------------------------------------------
  // std::pair: write first then second, no length prefix (each side
  // already knows the type and consumes its own bytes).
  template<class T1, class T2>
  BinaryWriteArchive& operator<<(const std::pair<T1, T2>& v) {
    *this << v.first;
    *this << v.second;
    return *this;
  }

  // ---- Linear containers (length prefix + sequential elements). --------
  // All linear containers share the wire format: v64 length prefix
  // followed by each element serialized via operator<<. Iteration
  // order matches the container's begin()/end(). For ordered containers
  // (set/map/BTreeSet/BTreeMap) this is sorted-key order. For unordered
  // containers (unordered_set/unordered_map/HashSet/HashMap) it's
  // bucket-walk order — same iteration order as the existing
  // `Marshal` operator<<, so byte-for-byte compatibility holds.
  template<class T>
  BinaryWriteArchive& operator<<(const rusty::Vec<T>& v) {
    rrr::v64 v_len = static_cast<rrr::i64>(v.size());
    *this << v_len;
    for (auto it = v.begin(); it != v.end(); ++it) *this << *it;
    return *this;
  }

  template<class T>
  BinaryWriteArchive& operator<<(const std::vector<T>& v) {
    rrr::v64 v_len = static_cast<rrr::i64>(v.size());
    *this << v_len;
    for (auto it = v.begin(); it != v.end(); ++it) *this << *it;
    return *this;
  }

  template<class T>
  BinaryWriteArchive& operator<<(const std::list<T>& v) {
    rrr::v64 v_len = static_cast<rrr::i64>(v.size());
    *this << v_len;
    for (auto it = v.begin(); it != v.end(); ++it) *this << *it;
    return *this;
  }

  template<class T>
  BinaryWriteArchive& operator<<(const rusty::BTreeSet<T>& v) {
    rrr::v64 v_len = static_cast<rrr::i64>(v.len());
    *this << v_len;
    for (auto it = v.begin(); it != v.end(); ++it) *this << *it;
    return *this;
  }

  template<class T>
  BinaryWriteArchive& operator<<(const std::set<T>& v) {
    rrr::v64 v_len = static_cast<rrr::i64>(v.size());
    *this << v_len;
    for (auto it = v.begin(); it != v.end(); ++it) *this << *it;
    return *this;
  }

  template<class T>
  BinaryWriteArchive& operator<<(const rusty::HashSet<T>& v) {
    rrr::v64 v_len = static_cast<rrr::i64>(v.len());
    *this << v_len;
    for (auto it = v.begin(); it != v.end(); ++it) *this << *it;
    return *this;
  }

  template<class T>
  BinaryWriteArchive& operator<<(const std::unordered_set<T>& v) {
    rrr::v64 v_len = static_cast<rrr::i64>(v.size());
    *this << v_len;
    for (auto it = v.begin(); it != v.end(); ++it) *this << *it;
    return *this;
  }

  template<class K, class V>
  BinaryWriteArchive& operator<<(const rusty::BTreeMap<K, V>& v) {
    rrr::v64 v_len = static_cast<rrr::i64>(v.len());
    *this << v_len;
    // L9: rusty::BTreeMap iter `operator*()` returns
    // `std::tuple<const K&, V&>` (post-2026-04 API).
    for (auto it = v.begin(); it != v.end(); ++it) {
      auto kv = *it;
      *this << std::get<0>(kv) << std::get<1>(kv);
    }
    return *this;
  }

  template<class K, class V>
  BinaryWriteArchive& operator<<(const std::map<K, V>& v) {
    rrr::v64 v_len = static_cast<rrr::i64>(v.size());
    *this << v_len;
    for (auto it = v.begin(); it != v.end(); ++it) {
      *this << it->first << it->second;
    }
    return *this;
  }

  template<class K, class V>
  BinaryWriteArchive& operator<<(const rusty::HashMap<K, V>& v) {
    rrr::v64 v_len = static_cast<rrr::i64>(v.len());
    *this << v_len;
    // L9: rusty::HashMap iter `operator*()` returns
    // `std::tuple<const K&, V&>` (post-2026-04 API).
    for (auto it = v.begin(); it != v.end(); ++it) {
      auto kv = *it;
      *this << std::get<0>(kv) << std::get<1>(kv);
    }
    return *this;
  }

  template<class K, class V>
  BinaryWriteArchive& operator<<(const std::unordered_map<K, V>& v) {
    rrr::v64 v_len = static_cast<rrr::i64>(v.size());
    *this << v_len;
    for (auto it = v.begin(); it != v.end(); ++it) {
      *this << it->first << it->second;
    }
    return *this;
  }
};

class BinaryReadArchive {
  SourceProxy source_;
 public:
  explicit BinaryReadArchive(SourceProxy source) noexcept
      : source_(std::move(source)) {}

  // Convenience: build directly atop a concrete BufferSource.
  explicit BinaryReadArchive(BufferSource* source)
      : source_(make_source_proxy(source)) {}

  // Convenience: build directly atop a concrete FdSource.
  explicit BinaryReadArchive(FdSource* source)
      : source_(make_source_proxy(source)) {}

  // Convenience: build directly atop a concrete MarshalSource (Phase 3a).
  explicit BinaryReadArchive(MarshalSource* source)
      : source_(make_source_proxy(source)) {}

  // Expose the inner SourceProxy so callers can use proxy_cast to
  // recover the concrete adapter type. Used by the MarshallDeputy
  // archive operators in marshal_serializable_bridge.hpp.
  SourceProxy& source() noexcept { return source_; }
  const SourceProxy& source() const noexcept { return source_; }

  // Read into raw bytes; verifies n bytes were actually read.
  // Returns false if the source ran out (caller can decide whether to
  // abort or surface the error).
  [[nodiscard]] bool read_exact(void* p, size_t n) {
    size_t got = source_->read(p, n);
    return got == n;
  }

  // ---- Fixed-width primitives. ------------------------------------------
  // Each operator>> verifies the read produced sizeof(T) bytes; on
  // truncation it aborts via `verify` (matches the existing
  // `Marshal::read` contract — short reads at the boundary are
  // programming errors at this layer, not recoverable conditions).
  BinaryReadArchive& operator>>(int8_t& v)   { verify(read_exact(&v, sizeof(v))); return *this; }
  BinaryReadArchive& operator>>(int16_t& v)  { verify(read_exact(&v, sizeof(v))); return *this; }
  BinaryReadArchive& operator>>(int32_t& v)  { verify(read_exact(&v, sizeof(v))); return *this; }
  BinaryReadArchive& operator>>(int64_t& v)  { verify(read_exact(&v, sizeof(v))); return *this; }
  BinaryReadArchive& operator>>(uint8_t& v)  { verify(read_exact(&v, sizeof(v))); return *this; }
  BinaryReadArchive& operator>>(uint16_t& v) { verify(read_exact(&v, sizeof(v))); return *this; }
  BinaryReadArchive& operator>>(uint32_t& v) { verify(read_exact(&v, sizeof(v))); return *this; }
  BinaryReadArchive& operator>>(uint64_t& v) { verify(read_exact(&v, sizeof(v))); return *this; }
  BinaryReadArchive& operator>>(double& v)   { verify(read_exact(&v, sizeof(v))); return *this; }

  // ---- Variable-length integer encoding (SparseInt). --------------------
  // SparseInt's first byte determines the total length; we peek it,
  // read the remaining bytes, then decode.
  BinaryReadArchive& operator>>(rrr::v32& v) {
    char buf[5];
    verify(read_exact(buf, 1));
    size_t total = rrr::SparseInt::buf_size(buf[0]);
    if (total > 1) {
      verify(read_exact(buf + 1, total - 1));
    }
    v.set(rrr::SparseInt::load_i32(buf));
    return *this;
  }

  BinaryReadArchive& operator>>(rrr::v64& v) {
    char buf[9];
    verify(read_exact(buf, 1));
    size_t total = rrr::SparseInt::buf_size(buf[0]);
    if (total > 1) {
      verify(read_exact(buf + 1, total - 1));
    }
    v.set(rrr::SparseInt::load_i64(buf));
    return *this;
  }

  // ---- Variable-length byte sequences. ----------------------------------
  BinaryReadArchive& operator>>(std::string& s) {
    rrr::v64 v_len{0};
    *this >> v_len;
    auto len = static_cast<size_t>(v_len.get());
    s.resize(len);
    if (len > 0) {
      // @unsafe { writing into string's internal buffer via &s[0] }
      verify(read_exact(&s[0], len));
    }
    return *this;
  }

  // ---- Composites. ------------------------------------------------------
  template<class T1, class T2>
  BinaryReadArchive& operator>>(std::pair<T1, T2>& v) {
    *this >> v.first;
    *this >> v.second;
    return *this;
  }

  // ---- Linear containers. -----------------------------------------------
  // Wire format: v64 length prefix + N elements deserialized in order.
  // Containers are cleared first; matches the existing Marshal operator>>
  // semantics.
  template<class T>
  BinaryReadArchive& operator>>(rusty::Vec<T>& v) {
    rrr::v64 v_len{0};
    *this >> v_len;
    v.clear();
    auto n = static_cast<size_t>(v_len.get());
    v.reserve(n);
    for (size_t i = 0; i < n; ++i) {
      T elem{};
      *this >> elem;
      v.push(std::move(elem));
    }
    return *this;
  }

  template<class T>
  BinaryReadArchive& operator>>(std::vector<T>& v) {
    rrr::v64 v_len{0};
    *this >> v_len;
    v.clear();
    auto n = static_cast<size_t>(v_len.get());
    v.reserve(n);
    for (size_t i = 0; i < n; ++i) {
      T elem{};
      *this >> elem;
      v.push_back(std::move(elem));
    }
    return *this;
  }

  template<class T>
  BinaryReadArchive& operator>>(std::list<T>& v) {
    rrr::v64 v_len{0};
    *this >> v_len;
    v.clear();
    auto n = static_cast<size_t>(v_len.get());
    for (size_t i = 0; i < n; ++i) {
      T elem{};
      *this >> elem;
      v.push_back(std::move(elem));
    }
    return *this;
  }

  template<class T>
  BinaryReadArchive& operator>>(rusty::BTreeSet<T>& v) {
    rrr::v64 v_len{0};
    *this >> v_len;
    v.clear();
    auto n = static_cast<size_t>(v_len.get());
    for (size_t i = 0; i < n; ++i) {
      T elem{};
      *this >> elem;
      v.insert(std::move(elem));
    }
    return *this;
  }

  template<class T>
  BinaryReadArchive& operator>>(std::set<T>& v) {
    rrr::v64 v_len{0};
    *this >> v_len;
    v.clear();
    auto n = static_cast<size_t>(v_len.get());
    for (size_t i = 0; i < n; ++i) {
      T elem{};
      *this >> elem;
      v.insert(std::move(elem));
    }
    return *this;
  }

  template<class T>
  BinaryReadArchive& operator>>(rusty::HashSet<T>& v) {
    rrr::v64 v_len{0};
    *this >> v_len;
    v.clear();
    auto n = static_cast<size_t>(v_len.get());
    for (size_t i = 0; i < n; ++i) {
      T elem{};
      *this >> elem;
      v.insert(std::move(elem));
    }
    return *this;
  }

  template<class T>
  BinaryReadArchive& operator>>(std::unordered_set<T>& v) {
    rrr::v64 v_len{0};
    *this >> v_len;
    v.clear();
    auto n = static_cast<size_t>(v_len.get());
    for (size_t i = 0; i < n; ++i) {
      T elem{};
      *this >> elem;
      v.insert(std::move(elem));
    }
    return *this;
  }

  template<class K, class V>
  BinaryReadArchive& operator>>(rusty::BTreeMap<K, V>& v) {
    rrr::v64 v_len{0};
    *this >> v_len;
    v.clear();
    auto n = static_cast<size_t>(v_len.get());
    for (size_t i = 0; i < n; ++i) {
      K key{};
      V value{};
      *this >> key >> value;
      v.insert(std::move(key), std::move(value));
    }
    return *this;
  }

  template<class K, class V>
  BinaryReadArchive& operator>>(std::map<K, V>& v) {
    rrr::v64 v_len{0};
    *this >> v_len;
    v.clear();
    auto n = static_cast<size_t>(v_len.get());
    for (size_t i = 0; i < n; ++i) {
      K key{};
      V value{};
      *this >> key >> value;
      v.emplace(std::move(key), std::move(value));
    }
    return *this;
  }

  template<class K, class V>
  BinaryReadArchive& operator>>(rusty::HashMap<K, V>& v) {
    rrr::v64 v_len{0};
    *this >> v_len;
    v.clear();
    auto n = static_cast<size_t>(v_len.get());
    for (size_t i = 0; i < n; ++i) {
      K key{};
      V value{};
      *this >> key >> value;
      v.insert(std::move(key), std::move(value));
    }
    return *this;
  }

  template<class K, class V>
  BinaryReadArchive& operator>>(std::unordered_map<K, V>& v) {
    rrr::v64 v_len{0};
    *this >> v_len;
    v.clear();
    auto n = static_cast<size_t>(v_len.get());
    for (size_t i = 0; i < n; ++i) {
      K key{};
      V value{};
      *this >> key >> value;
      v.emplace(std::move(key), std::move(value));
    }
    return *this;
  }
};

// ---------------------------------------------------------------------------
// Layer 4: Serializable proxy + factory registry (Phase 2).
//
// A type T satisfies the Serializable concept if it provides:
//   - void save(BinaryWriteArchive&) const  -- emit bytes
//   - void load(BinaryReadArchive&)         -- consume bytes
//   - int32_t kind() const                  -- factory tag
//
// SerializableFacade type-erases over any such T. SerializableRegistry
// maps a kind tag to a factory that constructs a fresh
// SerializableProxy (default-constructing the underlying T).
//
// Wire format:
//   - SerializableProxy::save emits ONLY the payload bytes (no kind
//     prefix). This matches the existing `Marshallable::to_marshal`
//     contract. The kind prefix is at the next-higher framing layer
//     (currently `MarshallDeputy`; in Phase 3 it will be rewritten in
//     terms of SerializableProxy).
//
// This is the new replacement for `Marshallable` + `MarshallableProxy`
// + `MarshallDeputy::reg_initializer`. It runs in parallel with the
// old system; per-command-type migrations happen in Phase 4.
// ---------------------------------------------------------------------------

PRO_DEF_MEM_DISPATCH(SerializableMemSave, save);
PRO_DEF_MEM_DISPATCH(SerializableMemLoad, load);
PRO_DEF_MEM_DISPATCH(SerializableMemKind, kind);

struct SerializableFacade : pro::facade_builder
    ::add_convention<SerializableMemSave, void(BinaryWriteArchive&) const>
    ::add_convention<SerializableMemLoad, void(BinaryReadArchive&)>
    ::add_convention<SerializableMemKind, int32_t() const>
    // Phase 4 prep: enable `pro::proxy_cast<T*>(&proxy)` for type-safe
    // downcast back to the underlying T. Used by `serializable_cast<T>`
    // in `marshal_serializable_bridge.hpp` to extract a typed payload
    // from a SerializableMarshallableAdapter wrapped in MarshallDeputy.
    ::add_skill<pro::skills::indirect_rtti>
    ::build {};

using SerializableProxy = pro::proxy<SerializableFacade>;

// L6-pivot (2026-05-01): the `SerializableConcept<T>` C++20 concept
// was retired in this commit.  It described the "T has save/load/kind"
// shape, but it duplicated what `SerializableFacade` already enforces
// at proxy construction time (and what `pro::proxy_cast<T*>` enforces
// at downcast time, returning nullptr if T is wrong).  The concept's
// only practical role was disambiguating overloads in
// `marshallable_cast<T>` / `wrap_typed_marshallable<T>` /
// `MarshallDeputy(shared_ptr<T>)` between the Marshallable subclass
// path and the bridge path; that disambiguation now uses just
// `!std::is_base_of_v<Marshallable, T>` and trusts the proxy library
// to reject wrong-shaped T at instantiation / runtime.
//
// In exchange for slightly worse compile errors when a non-
// Serializable T sneaks into a bridge overload, we drop the concept
// boilerplate (one less abstraction in the marshal layer).  The
// underlying contract — `save(BinaryWriteArchive&) const`,
// `load(BinaryReadArchive&)`, `kind() const -> int32_t` — is still
// what every Serializable-migrated type implements; it's just no
// longer expressed as a separate template-constraint predicate.

// Construct a SerializableProxy that owns a T constructed from the
// forwarded arguments (default-constructed if no args). T must
// satisfy:
//   - void save(BinaryWriteArchive&) const
//   - void load(BinaryReadArchive&)
//   - int32_t kind() const
template<class T, class... Args>
inline SerializableProxy make_serializable_proxy(Args&&... args) {
  return pro::make_proxy<SerializableFacade, T>(std::forward<Args>(args)...);
}

// Factory registry: maps int32_t kind tags to factories that produce
// fresh SerializableProxy instances.
//
// Usage:
//   static int reg_canary = SerializableRegistry::reg<CanaryCommand>(0xCAFE);
//
//   SerializableProxy proxy = SerializableRegistry::create(0xCAFE);
//   proxy->load(reader);  // populate from wire
//
// Implementation lives in marshal_archive.cpp behind a SpinMutex —
// registration runs at static init time and lookups during RPC
// dispatch are concurrent across reactor threads.
class SerializableRegistry {
 public:
  // rusty::Function is move-only; the registry stores each factory by move
  // and invokes it under the registry's SpinMutex inside `create()` (no
  // copy-out-of-lock — see marshal_archive.cpp).
  using Factory = rusty::Function<SerializableProxy()>;

  // Register T under `kind`. Returns 0 so it can sit at namespace
  // scope as a static-initializer return value:
  //   static int _reg = SerializableRegistry::reg<CanaryCommand>(0xCAFE);
  template<class T>
  static int reg(int32_t kind) {
    register_factory(kind, []() {
      return make_serializable_proxy<T>();
    });
    return 0;
  }

  // Create a fresh proxy for the given kind. Aborts via verify() if
  // the kind is not registered.
  static SerializableProxy create(int32_t kind);

  // Test helper: check if a kind is registered.
  static bool is_registered(int32_t kind);

  // Test helper: clear the registry. Not thread-safe; use only
  // between tests in single-threaded fixtures.
  static void clear_for_testing();

 private:
  static void register_factory(int32_t kind, Factory factory);
};

// ---------------------------------------------------------------------------
// Layer 5: declaration-order kind tags via a central TypeList.
//
// Mirrors Rust's `enum Foo { A(...), B(...), ... }` + bincode pattern,
// where each variant's wire discriminant is derived from declaration
// order in the enum.  In C++, the "central enum" is a `TypeList<...>`
// at namespace scope:
//
//   namespace janus {
//   class EmptyGraph;
//   class RccGraph;
//   class TpcCommitCommand;
//   // ...all forward-declared
//
//   using AllPayloads = rrr::TypeList<
//       EmptyGraph,
//       RccGraph,
//       TpcCommitCommand,
//       // ...
//   >;
//   }
//
// `TypeList<Ts...>::index_of<T>()` returns T's position in the list as
// a constexpr int32_t.  Each Serializable type's kind is its position.
//
// Properties:
//   * Cross-compiler / cross-machine deterministic — index is purely
//     a property of declaration order in source.
//   * Zero collision risk — distinct types get distinct indices.
//   * Rename-stable — renaming `TpcCommitCommand` doesn't change its
//     position in the list, so its kind is unchanged.
//   * Backward-compat by appending — adding a new type at the END of
//     the list assigns it the next available kind; existing types keep
//     theirs.  Reordering or removing is a wire-break.
//
// This replaces the prior approaches:
//   * Manual `static constexpr int32_t kMarshallKind = MarshallDeputy::CMD_X`
//     (per-type constant + central int enum) — the per-type constant
//     and the central enum collapse into the single TypeList.
//   * FNV-1a hash of `typeid(T).name()` (the previous POC) — replaced
//     because hashing is implementation-dependent (mangled name format)
//     and rename-fragile.

// `TypeList<Ts...>` — variadic compile-time type list with index lookup.
// Follows the standard `std::tuple`-style variadic pattern but exposes
// only the operations we need.
//
// Indices start at 1 (not 0) — position 0 is reserved for
// `MarshallDeputy::UNKNOWN`, which `MarshallDeputy::set_marshallable`
// rejects as a sentinel "kind unset" value.  So the first element of
// the list has `index_of<...>() == 1`.  Types not in the list resolve
// to 0 (UNKNOWN), which surfaces as a `verify(0)`-grade error at
// registration / set_marshallable time.
//
// Workstream N L10a: `create_at(pos)` adds compile-time-driven factory
// dispatch — given a 1-indexed wire kind, returns a fresh
// `SerializableProxy` for the corresponding type. This is the
// closed-set counterpart of `MarshallDeputy::create_initializer(kind)`,
// but with zero runtime registry state: the dispatch is a switch over
// declaration-order `Ts...`, decided at compile time. Used by the
// L10b `SerializableEnvelope<TypeList>` carrier on its read path.
template<typename... Ts>
struct TypeList {
  static constexpr std::size_t size = sizeof...(Ts);

  // Returns T's 1-indexed position in `Ts...`, or 0 if T is not in
  // the list.  Constexpr so `kind()` etc. inline to a literal.
  template<typename T>
  static constexpr int32_t index_of() noexcept {
    return index_of_impl<T, 1, Ts...>();
  }

  // Returns true iff `T` appears in `Ts...`.
  template<typename T>
  static constexpr bool contains() noexcept {
    return index_of<T>() != 0;
  }

  // Construct a fresh SerializableProxy for the type at 1-indexed
  // position `pos`. Aborts via `verify` if `pos` is 0 (UNKNOWN
  // sentinel) or out of range — invalid wire kind on the read side
  // is a hard deserialization error, consistent with how
  // `MarshallDeputy::create_initializer` handles unknown kinds.
  static SerializableProxy create_at(int32_t pos) {
    verify(pos >= 1 && pos <= static_cast<int32_t>(sizeof...(Ts)));
    return create_at_impl<Ts...>(pos - 1);
  }

 private:
  template<typename T, int32_t I>
  static constexpr int32_t index_of_impl() noexcept {
    return 0;  // T not in list — surfaces as UNKNOWN sentinel
  }

  template<typename T, int32_t I, typename Head, typename... Rest>
  static constexpr int32_t index_of_impl() noexcept {
    if constexpr (std::is_same_v<T, Head>) {
      return I;
    } else {
      return index_of_impl<T, I + 1, Rest...>();
    }
  }

  // Recursive variadic walk to locate the type at index `idx` (0-based
  // within the implementation; the public API takes 1-indexed `pos`).
  // The compiler instantiates one branch per type in the list; the
  // resulting code is equivalent to a flat switch.
  template<typename Head, typename... Rest>
  static SerializableProxy create_at_impl(int32_t idx) {
    if (idx == 0) {
      return make_serializable_proxy<Head>();
    }
    if constexpr (sizeof...(Rest) > 0) {
      return create_at_impl<Rest...>(idx - 1);
    }
    // Unreachable — public `create_at` already verified the bound.
    verify(false);
  }
};

}  // namespace rrr
