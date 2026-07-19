module;

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <inttypes.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include <rusty/arc.hpp>
#include <rusty/box.hpp>
#include <rusty/fn.hpp>
#include <rusty/rusty.hpp>

export module rrr.marshal;

import std;
import rusty;
import rrr.basetypes;
import rrr.debugging;
import rrr.misc;
import rrr.serializable;
import rrr.threading;

// @safe - Marshal: append-only byte buffer with separate write/read
// cursors, backed by a single rusty::Vec<uint8_t>. Replaces the prior
// chunk-linked-list implementation; see docs/dev/marshal_perf_baseline.md
// for the perf comparison that motivated the swap (V2 wins 16-81%
// across every benchmark scenario).
//
// Public API is unchanged: write(p,n) / read(p,n) / peek<T>(out,n) /
// content_size / set_bookmark / write_bookmark / read_from_marshal /
// reset / MarshalSink + MarshalSource adapters. The chunk-specific
// helpers `read_chnk` and `read_reuse_chnk` are removed (no external
// callers) and `init_block_read` becomes a buffer pre-reserve.
//
// Unsafety footprint: every public method is `// @safe` with at most
// one inline `// @unsafe { ... }` block around the libc `memcpy`
// call. No raw `char*` arithmetic, no `chunk*` linked-list walks, no
// `char**` bookmark pointers.
export namespace rrr {


// @safe - Wrapper for std::min (pure function, no side effects)
template<typename T>
inline T safe_min(const T& a, const T& b) {
  // @unsafe
  { return std::min(a, b); }
}

// removed the entire `RPC_STATISTICS` block
// and `stat_marshal_in` declaration. After Phase 5b-7/5b-8 deleted
// the marshal-out side, the marshal-in side became dead too once
// `Marshal::read_from_fd` / `Marshal::chnk_read_from_fd` / `chunk::read_from_fd`
// had no production callers anywhere in the codebase. The receive path
// uses `FdSource` (`serializable.hpp`) instead.

// not thread safe, for better performance
class Marshal;


// @safe - Vec<uint8_t>-backed byte queue with separate write/read
// cursors. Append-only writes go to buf_; reads memcpy from buf_.data
// + read_pos_ and advance read_pos_. When read_pos_ catches up to
// buf_.size() (fully drained), both reset to zero so steady-state
// write/read loops don't grow buf_ unboundedly.
// Pre-reserved capacity on first construction so small payloads don't
// pay a realloc-on-first-write. 4 KB matches the legacy chunk-list's
// default chunk size, keeping per-Marshal memory footprint comparable
// for the bench comparison.
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block with the C++ constexpr. Lifted
// from `Marshal` class scope (private static constexpr) to namespace
// scope because DSL constants live at namespace level; the one
// existing use site references it unqualified, so namespace lookup
// still resolves to the new constant.
#if RUSTYCPP_RUST
const kInitialCapacity: usize = 4096;
#endif
/*RUSTYCPP:GEN-BEGIN id=marshal.initial_capacity version=1 rust_sha256=72b7f808695d048223203b2f3e49c2f8b175eace523aaf80b1b560ffaccb10a6*/
constexpr size_t kInitialCapacity = static_cast<size_t>(4096);
/*RUSTYCPP:GEN-END id=marshal.initial_capacity*/

// Hand-written kernels for the DSL methods below (raw byte-pointer
// memcpy/span surgery). Defined right after the GEN block.
std::size_t marshal_write(Marshal& self, const std::uint8_t* p, std::size_t n);
std::size_t marshal_read(Marshal& self, std::uint8_t* p, std::size_t n);
std::size_t marshal_peek_bytes(const Marshal& self, std::uint8_t* p, std::size_t n);
std::size_t marshal_read_from(Marshal& self, Marshal& src, std::size_t n);

// fn new()'s field init can't spell "a Vec with reserved capacity".
inline rusty::Vec<std::uint8_t> marshal_make_reserved_buf() {
  rusty::Vec<std::uint8_t> v;
  v.reserve(kInitialCapacity);
  return v;
}

// `Marshal` — the contiguous wire buffer (write tail + read cursor).
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `/*RUSTYCPP:GEN-BEGIN ... END*/` block.
//
// Behavioral diffs from the original C++ class:
//   * `read_pos_`/`write_cnt_` are `Cell`s: Cells default-CONSTRUCT to
//     zero, so plain `Marshal m;` locals and by-value fields (e.g. the
//     DSL `Request { m: Marshal }` in server.cpp) stay zero-initialized
//     without the old in-class initializers — zero call-site churn.
//     Cell is also the transpiler's move-only marker, replacing the
//     `: public NoCopy` base (copy deleted, move implicit, exactly as
//     before).
//   * Plain default construction no longer pre-reserves
//     kInitialCapacity (a DSL aggregate has no ctor body) — only
//     `Marshal::new_()` does, via marshal_make_reserved_buf(). Perf
//     hint only; first write grows the Vec.
//   * The raw byte overloads keep their names/signatures except
//     `const void*`/`void*` become `const uint8_t*`/`uint8_t*` (the
//     DSL has no void*); callers cast at the boundary.
//   * The typed overload `read<T>(T&, n=sizeof(T))` is renamed
//     `read_obj<T>(T&)` (a Rust impl cannot overload `read`, and DSL
//     fns have no default args); `peek<T>` likewise drops its `n`
//     parameter. Both delegate to marshal_* template free fns.
#if RUSTYCPP_RUST
struct Marshal {
    buf_: Vec<u8>,
    read_pos_: Cell<usize>,
    write_cnt_: Cell<i32>,
}

impl Marshal {
    fn new() -> Marshal {
        Marshal {
            buf_: marshal_make_reserved_buf(),
            read_pos_: Cell::new(0usize),
            write_cnt_: Cell::new(0i32),
        }
    }

    // Empty when fully drained.
    fn empty(&self) -> bool {
        self.read_pos_.get() >= self.buf_.len()
    }

    // Bytes between read cursor and write tail.
    fn content_size(&self) -> usize {
        self.buf_.len() - self.read_pos_.get()
    }

    // Append n bytes from caller-owned p. (Named write_bytes: the
    // transpiler reserves/suffixes a bare `write`.)
    fn write_bytes(&mut self, p: *const u8, n: usize) -> usize {
        marshal_write(self, p, n)
    }

    // Bounded copy out, advance cursor, recycle storage on full drain.
    fn read(&mut self, p: *mut u8, n: usize) -> usize {
        marshal_read(self, p, n)
    }

    // Type-safe read for trivially-copyable T (was `read<T>`).
    fn read_obj<T>(&mut self, out: &mut T) -> usize {
        marshal_read_obj(self, out)
    }

    // Like read_obj but does not advance the cursor.
    fn peek<T>(&self, out: &mut T) -> usize {
        marshal_peek(self, out)
    }

    // Raw bulk peek: copy up to n unread bytes without advancing.
    fn peek_bytes(&self, p: *mut u8, n: usize) -> usize {
        marshal_peek_bytes(self, p, n)
    }

    // Splice n bytes from another Marshal into this one.
    fn read_from_marshal(&mut self, src: &mut Marshal, n: usize) -> usize {
        marshal_read_from(self, src, n)
    }

    // Empty the buffer, reset cursor and write count.
    fn reset(&mut self) {
        self.buf_.clear();
        self.read_pos_.set(0);
        self.write_cnt_.set(0);
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=marshal.marshal version=1 rust_sha256=12876777f123eaff5008fe1923017338fb295de7e8199c3316c1ee9b1635da6c*/
struct Marshal;

struct Marshal {
    rusty::Vec<uint8_t> buf_;
    rusty::Cell<size_t> read_pos_;
    rusty::Cell<int32_t> write_cnt_;

    static Marshal new_();
    bool empty() const;
    size_t content_size() const;
    size_t write_bytes(const uint8_t* p, size_t n);
    size_t read(uint8_t* p, size_t n);
    template<typename T>
    size_t read_obj(T& out);
    template<typename T>
    size_t peek(T& out) const;
    size_t peek_bytes(uint8_t* p, size_t n) const;
    size_t read_from_marshal(Marshal& src, size_t n);
    void reset();
};


Marshal Marshal::new_() {
    return Marshal{.buf_ = marshal_make_reserved_buf(), .read_pos_ = rusty::Cell<size_t>::new_(static_cast<size_t>(0)), .write_cnt_ = rusty::Cell<int32_t>::new_(static_cast<int32_t>(0))};
}

bool Marshal::empty() const {
    return this->read_pos_.get() >= rusty::len(this->buf_);
}

size_t Marshal::content_size() const {
    return rusty::len(this->buf_) - this->read_pos_.get();
}

size_t Marshal::write_bytes(const uint8_t* p, size_t n) {
    return marshal_write((*this), p, std::move(n));
}

size_t Marshal::read(uint8_t* p, size_t n) {
    return marshal_read((*this), p, std::move(n));
}

template<typename T>
size_t Marshal::read_obj(T& out) {
    return marshal_read_obj((*this), out);
}

template<typename T>
size_t Marshal::peek(T& out) const {
    return marshal_peek((*this), out);
}

size_t Marshal::peek_bytes(uint8_t* p, size_t n) const {
    return marshal_peek_bytes((*this), p, std::move(n));
}

size_t Marshal::read_from_marshal(Marshal& src, size_t n) {
    return marshal_read_from((*this), src, std::move(n));
}

void Marshal::reset() {
    this->buf_.clear();
    this->read_pos_.set(static_cast<size_t>(0));
    this->write_cnt_.set(static_cast<int32_t>(0));
}
/*RUSTYCPP:GEN-END id=marshal.marshal*/

// ---- Marshal kernels (raw byte surgery; @unsafe boundaries) ----------

// @unsafe - caller-provided byte pointer into Vec::extend_from_slice.
inline std::size_t marshal_write(Marshal& self, const std::uint8_t* p,
                                 std::size_t n) {
  self.buf_.extend_from_slice(std::span<const std::uint8_t>(p, n));
  self.write_cnt_.set(self.write_cnt_.get() + static_cast<rrr::i32>(n));
  return n;
}

// @unsafe - libc memcpy from buf_.data()+read_pos_ to caller p.
inline std::size_t marshal_read(Marshal& self, std::uint8_t* p,
                                std::size_t n) {
  const std::size_t avail = self.buf_.size() - self.read_pos_.get();
  const std::size_t copy = std::min(n, avail);
  if (copy == 0) return 0;
  std::memcpy(p, self.buf_.data() + self.read_pos_.get(), copy);
  self.read_pos_.set(self.read_pos_.get() + copy);
  if (self.read_pos_.get() == self.buf_.size()) {
    // Fully drained — recycle storage so steady-state write/read loops
    // don't grow buf_ unboundedly (clear keeps capacity).
    self.buf_.clear();
    self.read_pos_.set(0);
  }
  return copy;
}

// @unsafe - reinterpret_cast for the type-safe wrapper.
template <typename T>
inline std::size_t marshal_read_obj(Marshal& self, T& out) {
  static_assert(std::is_trivially_copyable_v<T>,
                "read_obj requires trivially copyable type");
  return marshal_read(self, reinterpret_cast<std::uint8_t*>(&out), sizeof(T));
}

// @unsafe - libc memcpy from buf_.data()+read_pos_ to caller p.
inline std::size_t marshal_peek_bytes(const Marshal& self, std::uint8_t* p,
                                      std::size_t n) {
  const std::size_t avail = self.buf_.size() - self.read_pos_.get();
  const std::size_t copy = std::min(n, avail);
  if (copy == 0) return 0;
  std::memcpy(p, self.buf_.data() + self.read_pos_.get(), copy);
  return copy;
}

// @unsafe - libc memcpy from buf_.data()+read_pos_; T* address-of.
template <typename T>
inline std::size_t marshal_peek(const Marshal& self, T& out) {
  static_assert(std::is_trivially_copyable_v<T>,
                "peek requires trivially copyable type");
  const std::size_t avail = self.buf_.size() - self.read_pos_.get();
  const std::size_t copy = std::min(sizeof(T), avail);
  if (copy == 0) return 0;
  std::memcpy(reinterpret_cast<void*>(&out),
              self.buf_.data() + self.read_pos_.get(), copy);
  return copy;
}

// @unsafe - span over src's unread range into Vec::extend_from_slice.
inline std::size_t marshal_read_from(Marshal& self, Marshal& src,
                                     std::size_t n) {
  verify(src.content_size() >= n);
  if (n == 0) return 0;
  const auto* bytes = src.buf_.data() + src.read_pos_.get();
  self.buf_.extend_from_slice(std::span<const std::uint8_t>(bytes, n));
  self.write_cnt_.set(self.write_cnt_.get() + static_cast<rrr::i32>(n));
  src.read_pos_.set(src.read_pos_.get() + n);
  if (src.read_pos_.get() == src.buf_.size()) {
    src.buf_.clear();
    src.read_pos_.set(0);
  }
  return n;
}

// ---------------------------------------------------------------------------
// Marshal ↔ Archive bridges (Phase 1 of marshal-serde-split).
//
// MarshalSink / MarshalSource are thin DSL wrapper structs holding a
// borrowed `Marshal*`; `impl SinkBase for MarshalSink` /
// `impl SourceBase for MarshalSource` generate the trait adapters via
// the transpiler (same `impl Trait for Type` pattern as FdSink/FdSource
// in serializable.cpp). The `Marshal::write` / `Marshal::read` calls take
// a `const void*` / `void*` and live OUTSIDE the DSL block as the
// `marshal_sink_write` / `marshal_source_read` free functions (raw void*
// args aren't expressible in inline-Rust today). Caller owns the Marshal
// and must keep it alive for the lifetime of the proxy.
//
// Two proxy factories per side:
//   - make_sink_proxy(Marshal*)     — production/rpcgen path; wraps the
//     borrowed Marshal* in an owned MarshalSink (by-value adapter).
//   - make_sink_proxy(MarshalSink*) — borrows a caller-owned MarshalSink
//     (RefMut adapter), mirroring make_sink_proxy(FdSink*).
// ---------------------------------------------------------------------------

struct MarshalSink;
inline void marshal_sink_write(MarshalSink& self, const void* p, size_t n);
#if RUSTYCPP_RUST
struct MarshalSink {
    m_: *mut Marshal,
}
impl SinkBase for MarshalSink {
    fn write_bytes(&mut self, p: *const u8, n: usize) {
        marshal_sink_write(self, p, n);
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=marshal.marshal_sink version=1 rust_sha256=af1c10fc7f250e944ecc0aa43e779f3d9e70730b3e8cf85a564474d8d175cb6d*/
struct MarshalSink;

struct MarshalSink {
    Marshal* m_;

    void write_bytes(const uint8_t* p, size_t n);
};


void MarshalSink::write_bytes(const uint8_t* p, size_t n) {
    marshal_sink_write((*this), p, std::move(n));
}

template <>
class SinkBaseAdapter<MarshalSink> final : public SinkBase {
    MarshalSink value_;
public:
    explicit SinkBaseAdapter(MarshalSink v) : value_(std::move(v)) {}
    void write_bytes(const uint8_t* p, size_t n) override {
        value_.write_bytes(p, n);
    }
};

template <>
class SinkBaseAdapterRef<MarshalSink> final : public SinkBase {
    const MarshalSink& value_;
public:
    explicit SinkBaseAdapterRef(const MarshalSink& u) : value_(u) {}
    void write_bytes(const uint8_t* p, size_t n) override {
        std::abort();  // unreachable through &dyn T
    }
};

template <>
class SinkBaseAdapterRefMut<MarshalSink> final : public SinkBase {
    MarshalSink& value_;
public:
    explicit SinkBaseAdapterRefMut(MarshalSink& u) : value_(u) {}
    void write_bytes(const uint8_t* p, size_t n) override {
        value_.write_bytes(p, n);
    }
};
/*RUSTYCPP:GEN-END id=marshal.marshal_sink*/

// @safe - The only raw-pointer op is Marshal::write, annotated below.
inline void marshal_sink_write(MarshalSink& self, const void* p, size_t n) {
  // @unsafe { Marshal::write through borrowed pointer + verify }
  size_t actual = self.m_->write_bytes(static_cast<const std::uint8_t*>(p), n);
  verify(actual == n);
}

struct MarshalSource;
inline size_t marshal_source_read(MarshalSource& self, void* p, size_t n);
#if RUSTYCPP_RUST
struct MarshalSource {
    m_: *mut Marshal,
}
impl SourceBase for MarshalSource {
    fn read_bytes(&mut self, p: *mut u8, n: usize) -> usize {
        marshal_source_read(self, p, n)
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=marshal.marshal_source version=1 rust_sha256=6e4c1562ee01df9828ae43890e9b8c582715bb4e733d0ff949773eb211f5213a*/
struct MarshalSource;

struct MarshalSource {
    Marshal* m_;

    size_t read_bytes(uint8_t* p, size_t n);
};


size_t MarshalSource::read_bytes(uint8_t* p, size_t n) {
    return marshal_source_read((*this), p, std::move(n));
}

template <>
class SourceBaseAdapter<MarshalSource> final : public SourceBase {
    MarshalSource value_;
public:
    explicit SourceBaseAdapter(MarshalSource v) : value_(std::move(v)) {}
    size_t read_bytes(uint8_t* p, size_t n) override {
        return value_.read_bytes(p, n);
    }
};

template <>
class SourceBaseAdapterRef<MarshalSource> final : public SourceBase {
    const MarshalSource& value_;
public:
    explicit SourceBaseAdapterRef(const MarshalSource& u) : value_(u) {}
    size_t read_bytes(uint8_t* p, size_t n) override {
        std::abort();  // unreachable through &dyn T
    }
};

template <>
class SourceBaseAdapterRefMut<MarshalSource> final : public SourceBase {
    MarshalSource& value_;
public:
    explicit SourceBaseAdapterRefMut(MarshalSource& u) : value_(u) {}
    size_t read_bytes(uint8_t* p, size_t n) override {
        return value_.read_bytes(p, n);
    }
};
/*RUSTYCPP:GEN-END id=marshal.marshal_source*/

// @safe - The only raw-pointer op is Marshal::read, annotated below.
inline size_t marshal_source_read(MarshalSource& self, void* p, size_t n) {
  // @unsafe { Marshal::read through borrowed pointer }
  return self.m_->read(static_cast<std::uint8_t*>(p), n);
}

inline SinkProxy make_sink_proxy(Marshal* m) {
  return rusty::make_box<SinkBaseAdapter<MarshalSink>>(MarshalSink{.m_ = m});
}
inline SourceProxy make_source_proxy(Marshal* m) {
  return rusty::make_box<SourceBaseAdapter<MarshalSource>>(MarshalSource{.m_ = m});
}

inline SinkProxy make_sink_proxy(MarshalSink* sink) {
  return rusty::make_box<SinkBaseAdapterRefMut<MarshalSink>>(*sink);
}
inline SourceProxy make_source_proxy(MarshalSource* source) {
  return rusty::make_box<SourceBaseAdapterRefMut<MarshalSource>>(*source);
}

// Marshal-deprecation slice C: the entire Marshal-direct Serialize_/
// Deserialize_ overload surface (leaves, containers, catch-alls, ADL
// decoys) is DELETED. The archives are the only serde surface; Marshal
// survives below only as a raw byte buffer for the remaining bridge
// uses (MarshalSink/MarshalSource), which die with it.

// NOTE: the variadic `deserialize_from(RefMut<ReplyBuffer>&&, ...)` reply-read
// helper lives in rpc/client.cpp alongside the RefMut<ReplyBuffer> `operator>>`
// bridge it reuses — NOT here. Reply structs carry Archive operators (not
// Marshal ones), so the helper must read through a BinaryReadArchive; keeping
// it next to the bridge also avoids baking a RefMut<ReplyBuffer> specialization
// into this (already huge) marshal BMI, which tripped a clang-22 ASTReader
// crash when heavy consumers (communicator.cc) imported it.


}  // export namespace rrr

// ============================================================================
// Implementation
// ============================================================================
// Marshal is fully header-emitted now — all methods are inline in the class
// definition above. The chunk-list out-of-class definitions (~Marshal,
// content_size_slow, write, read, read_chnk, read_reuse_chnk,
// read_from_marshal, set_bookmark) are gone; their Vec<uint8_t>-backed
// replacements are inline above. No translation-unit-local state remains.
//
// @safe - impl namespace placeholder. Retained as a no-op so module
// consumers' expectations about `namespace rrr` being closed in this
// TU are preserved.
namespace rrr {
} // namespace rrr
