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
//              Concrete impls in this file: BufferSink, BufferSource.
//              FdSink / FdSource arrive in Phase 1b.
//
//   Layer 3:   BinaryWriteArchive / BinaryReadArchive — knows the wire
//              format, holds a Sink/Source proxy, exposes operator<< /
//              operator>> for primitives.
//
//   Layer 4:   SerializableProxy — Phase 2.

// import std; replacement.
#include <std_compat.hpp>

// @c-compat-added
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <string>
#include <string_view>

#include <rusty/rusty.hpp>

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
struct SinkFacade : pro::facade_builder
    ::add_convention<SinkMemWrite, void(const void*, size_t)>
    ::build {};
using SinkProxy = pro::proxy<SinkFacade>;

// Source: returns the number of bytes actually read (may be < n at EOF).
//
// Convention: returns 0 at EOF; raises (or aborts) on transport error.
// Concrete sources control their own buffering / blocking semantics.
struct SourceFacade : pro::facade_builder
    ::add_convention<SourceMemRead, size_t(void*, size_t)>
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
};

class BinaryReadArchive {
  SourceProxy source_;
 public:
  explicit BinaryReadArchive(SourceProxy source) noexcept
      : source_(std::move(source)) {}

  // Convenience: build directly atop a concrete BufferSource.
  explicit BinaryReadArchive(BufferSource* source)
      : source_(make_source_proxy(source)) {}

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
};

}  // namespace rrr
