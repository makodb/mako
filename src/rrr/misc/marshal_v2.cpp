// Marshal V2 — prototype rewrite of rrr::Marshal on top of a single
// rusty::Vec<uint8_t> with a read-position counter.
//
// Lives parallel to the original Marshal (`rrr.marshal` module) so the
// two can be benchmarked side-by-side. If Cursor<Vec<u8>> hits the
// perf budget in docs/dev/marshal_perf_baseline.md, this will replace
// the chunk-linked-list implementation; otherwise it gets deleted and
// we fall back to physical-submodule quarantine.
//
// Public surface mirrors Marshal closely enough for the bench_marshal
// hot paths (write/read, operator<<>> for i8/i16/i32/i64/string, raw
// blob round-trips, bookmark patch). Containers (vector/map/set) are
// not implemented — out of scope for the prototype.

module;

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <rusty/vec.hpp>

export module rrr.marshal_v2;

import std;
import rrr.basetypes;
import rrr.debugging;

// @safe
export namespace rrr {

// @safe - Vec<u8>-backed byte queue with separate write/read cursors.
//   - writes append to buf_ (Vec::extend_from_slice).
//   - reads memcpy from buf_.data() + read_pos_ and advance read_pos_.
//   - when fully drained (read_pos_ == buf_.size()), both reset to
//     zero so steady-state write/read loops don't grow buf_ unboundedly.
class MarshalV2 {
public:
  // Pre-reserved capacity on first write so small payloads don't pay
  // the realloc-on-grow cost. Sized to match the existing Marshal's
  // default chunk size (4 KB) — keeps the per-Marshal memory footprint
  // comparable for like-for-like benchmarking.
  static constexpr std::size_t kInitialCapacity = 4096;

  // @safe - Default ctor: empty buffer, zero read cursor.
  MarshalV2() : buf_{}, read_pos_{0} {
    buf_.reserve(kInitialCapacity);
  }

  // @safe - Trivial dtor — Vec releases the heap allocation on drop.
  ~MarshalV2() = default;

  MarshalV2(const MarshalV2&) = delete;
  MarshalV2& operator=(const MarshalV2&) = delete;

  // Move-only. @safe — Vec is movable.
  MarshalV2(MarshalV2&& other) noexcept
      : buf_(std::move(other.buf_)), read_pos_(other.read_pos_) {
    other.read_pos_ = 0;
  }
  MarshalV2& operator=(MarshalV2&& other) noexcept {
    if (this != &other) {
      buf_ = std::move(other.buf_);
      read_pos_ = other.read_pos_;
      other.read_pos_ = 0;
    }
    return *this;
  }

  // @safe - Vec::is_empty + cheap arithmetic.
  bool empty() const { return read_pos_ >= buf_.size(); }

  // @safe - Vec::size + arithmetic.
  std::size_t content_size() const { return buf_.size() - read_pos_; }

  // @safe - Vec::extend_from_slice carries its own internal @unsafe
  // block around the memcpy + raw byte arithmetic.
  std::size_t write(const void* p, std::size_t n) {
    // @unsafe { caller-provided `const void*` cast to a byte span;
    //           Vec::extend_from_slice handles the memcpy. }
    {
      auto* bytes = static_cast<const std::uint8_t*>(p);
      buf_.extend_from_slice(std::span<const std::uint8_t>(bytes, n));
    }
    return n;
  }

  // @safe - bounded memcpy out of buf_, advance read_pos_, reset on
  // full drain. Inner @unsafe block wraps the libc memcpy + Vec::data
  // pointer arithmetic.
  std::size_t read(void* p, std::size_t n) {
    const std::size_t avail = buf_.size() - read_pos_;
    const std::size_t copy = std::min(n, avail);
    if (copy == 0) return 0;
    // @unsafe { libc memcpy; buf_.data() + read_pos_ pointer
    //           arithmetic; output `p` is caller-owned. }
    {
      std::memcpy(p, buf_.data() + read_pos_, copy);
    }
    read_pos_ += copy;
    if (read_pos_ == buf_.size()) {
      // Fully drained — recycle storage so steady-state write/read
      // loops don't grow buf_ unboundedly. Vec::clear keeps the
      // allocation (capacity) intact; only sets len back to 0.
      buf_.clear();
      read_pos_ = 0;
    }
    return copy;
  }

  // @safe - Like read() but doesn't advance the cursor.
  std::size_t peek(void* p, std::size_t n) const {
    const std::size_t avail = buf_.size() - read_pos_;
    const std::size_t copy = std::min(n, avail);
    if (copy == 0) return 0;
    // @unsafe { libc memcpy; buf_.data() + read_pos_ pointer
    //           arithmetic. }
    {
      std::memcpy(p, buf_.data() + read_pos_, copy);
    }
    return copy;
  }

  // @safe - Reset both ends — buf_.clear keeps the allocation.
  void reset() {
    buf_.clear();
    read_pos_ = 0;
  }

  // Bookmark is a (offset, size) handle into buf_. Patching writes
  // exactly `size` bytes at `offset`. The chunk implementation used a
  // `char**` to stash chunk-local pointers; here, `offset` into the
  // contiguous Vec is enough.
  // @safe - POD bookmark.
  struct bookmark {
    std::size_t offset = 0;
    std::size_t size = 0;
  };

  // @safe - Reserves `n` bytes at the current write tail; returns a
  // (offset, n) bookmark the caller patches with write_to_bookmark.
  // Internally appends `n` zero-bytes via push() in a loop; for the
  // bookmark sizes used in practice (4-8 bytes) this is fine. A
  // larger-bookmark variant could call resize_with if needed.
  bookmark set_bookmark(std::size_t n) {
    bookmark bm{buf_.size(), n};
    for (std::size_t i = 0; i < n; ++i) {
      buf_.push(std::uint8_t{0});
    }
    return bm;
  }

  // @safe - Patch the reserved slot.
  void write_to_bookmark(const bookmark& bm, const void* p, std::size_t n) {
    verify(n == bm.size);
    verify(bm.offset + n <= buf_.size());
    // @unsafe { libc memcpy at buf_.data() + bm.offset }
    {
      std::memcpy(buf_.data() + bm.offset, p, n);
    }
  }

  // @safe - Splice `n` bytes from `src` into `*this`. Both sides
  // advance their cursors appropriately. Bytes moved through the
  // Vec::extend_from_slice path — its internal @unsafe block carries
  // the memcpy.
  std::size_t read_from_marshal(MarshalV2& src, std::size_t n) {
    const std::size_t avail = src.content_size();
    const std::size_t copy = std::min(n, avail);
    if (copy == 0) return 0;
    // @unsafe { construct a span over src.buf_'s unread bytes and
    //           hand it to extend_from_slice. }
    {
      auto* bytes = src.buf_.data() + src.read_pos_;
      buf_.extend_from_slice(std::span<const std::uint8_t>(bytes, copy));
    }
    src.read_pos_ += copy;
    if (src.read_pos_ == src.buf_.size()) {
      src.buf_.clear();
      src.read_pos_ = 0;
    }
    return copy;
  }

private:
  rusty::Vec<std::uint8_t> buf_;
  std::size_t read_pos_;
};

// ---------------------------------------------------------------------------
// operator<< / operator>> — minimum set needed by bench_marshal_v2.
// Containers (vector/map/set/pair/Box/etc.) are out of scope for the
// prototype.
// ---------------------------------------------------------------------------

// @safe
inline MarshalV2& operator<<(MarshalV2& m, const rrr::i8& v) {
  verify(m.write(&v, sizeof(v)) == sizeof(v));
  return m;
}

// @safe
inline MarshalV2& operator<<(MarshalV2& m, const rrr::i16& v) {
  verify(m.write(&v, sizeof(v)) == sizeof(v));
  return m;
}

// @safe
inline MarshalV2& operator<<(MarshalV2& m, const rrr::i32& v) {
  verify(m.write(&v, sizeof(v)) == sizeof(v));
  return m;
}

// @safe
inline MarshalV2& operator<<(MarshalV2& m, const rrr::i64& v) {
  verify(m.write(&v, sizeof(v)) == sizeof(v));
  return m;
}

// @safe
inline MarshalV2& operator<<(MarshalV2& m, const std::uint8_t& u) {
  verify(m.write(&u, sizeof(u)) == sizeof(u));
  return m;
}

// @safe
inline MarshalV2& operator<<(MarshalV2& m, const std::uint16_t& u) {
  verify(m.write(&u, sizeof(u)) == sizeof(u));
  return m;
}

// @safe
inline MarshalV2& operator<<(MarshalV2& m, const std::uint32_t& u) {
  verify(m.write(&u, sizeof(u)) == sizeof(u));
  return m;
}

// @safe
inline MarshalV2& operator<<(MarshalV2& m, const std::uint64_t& u) {
  verify(m.write(&u, sizeof(u)) == sizeof(u));
  return m;
}

// @safe - 4-byte length prefix + raw bytes. Matches the wire format
// the original Marshal uses for std::string (sans varint encoding).
// Bench-only: keeps the prototype simple. Production switch will need
// the varint encoding restored.
inline MarshalV2& operator<<(MarshalV2& m, const std::string& v) {
  std::uint32_t len = static_cast<std::uint32_t>(v.size());
  m.write(&len, sizeof(len));
  // @unsafe { v.data() handed to write(); write() is @safe. }
  {
    m.write(v.data(), len);
  }
  return m;
}

// @safe
inline MarshalV2& operator>>(MarshalV2& m, rrr::i8& v) {
  verify(m.read(&v, sizeof(v)) == sizeof(v));
  return m;
}

// @safe
inline MarshalV2& operator>>(MarshalV2& m, rrr::i16& v) {
  verify(m.read(&v, sizeof(v)) == sizeof(v));
  return m;
}

// @safe
inline MarshalV2& operator>>(MarshalV2& m, rrr::i32& v) {
  verify(m.read(&v, sizeof(v)) == sizeof(v));
  return m;
}

// @safe
inline MarshalV2& operator>>(MarshalV2& m, rrr::i64& v) {
  verify(m.read(&v, sizeof(v)) == sizeof(v));
  return m;
}

// @safe
inline MarshalV2& operator>>(MarshalV2& m, std::uint8_t& u) {
  verify(m.read(&u, sizeof(u)) == sizeof(u));
  return m;
}

// @safe
inline MarshalV2& operator>>(MarshalV2& m, std::uint16_t& u) {
  verify(m.read(&u, sizeof(u)) == sizeof(u));
  return m;
}

// @safe
inline MarshalV2& operator>>(MarshalV2& m, std::uint32_t& u) {
  verify(m.read(&u, sizeof(u)) == sizeof(u));
  return m;
}

// @safe
inline MarshalV2& operator>>(MarshalV2& m, std::uint64_t& u) {
  verify(m.read(&u, sizeof(u)) == sizeof(u));
  return m;
}

// @safe
inline MarshalV2& operator>>(MarshalV2& m, std::string& v) {
  std::uint32_t len = 0;
  verify(m.read(&len, sizeof(len)) == sizeof(len));
  v.resize(len);
  if (len > 0) {
    // @unsafe { v.data() handed to read(); read() is @safe. }
    {
      verify(m.read(&v[0], len) == len);
    }
  }
  return m;
}

}  // export namespace rrr
