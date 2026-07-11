module;

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include <rusty/arc.hpp>
#include <rusty/box.hpp>
#include <rusty/rusty.hpp>
#include <rusty/fn.hpp>
#include <rusty/function.hpp>

export module rrr.serializable;

import std;
import rusty;
import rrr.basetypes;
import rrr.debugging;
import rrr.threading;

// @safe - Sink/Source/Archive layers + Serializable proxy machinery.
// Most classes are interfaces or pure dispatch through `SinkProxy` /
// `SourceProxy`. Genuinely-unsafe shells (raw fd + libc syscalls,
// raw `const uint8_t*` source storage, std::shared_ptr<T> holder, void*
// memcpy in archive primitives) carry per-class `// @unsafe` overrides
// or inline `// @unsafe { }` blocks below.
export namespace rrr {

// `MarshalSink` / `MarshalSource` (formerly here) now live in
// marshal.hpp alongside the `Marshal` class they wrap — moving them
// breaks the impl-side cycle (serializable.cpp no longer needs
// marshal.hpp's full Marshal class def to implement member functions
// declared in serializable.hpp). Forward-declared here for the
// BinaryWriteArchive/BinaryReadArchive convenience constructors below;
// constructor bodies are defined inline in marshal.hpp where the full
// class def is in scope.

// ---------------------------------------------------------------------------
// Layer 1+2: Sink / Source virtual base classes.
// ---------------------------------------------------------------------------

// Sink: anything that can accept (const uint8_t*, size_t) bytes.
//
// Convention is fire-and-forget. Concrete sinks may flush lazily;
// callers that need durability must call sink-specific flush methods
// before observing (FdSink will drain on destruction).
//
// The trait surface uses `const uint8_t*` (matching the actual byte-
// buffer semantics) instead of the historical `const void*`. Callers
// at the BinaryWriteArchive / BinaryReadArchive layer reinterpret_cast
// from typed pointers before dispatch.
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block as a virtual `class SinkBase`
// (same pattern as the Service / PollableBase / Job pub-trait
// migrations).
//
// The method is named `write_bytes` not `write` because rusty-cpp's
// `escape_cpp_keyword` list treats `write` as reserved (libc syscall
// collision) and would append an underscore in the emit; using a
// non-reserved name keeps the C++ symbol the same as the DSL
// signature and avoids the `sink_->write_(...)` naming dissonance.
#if RUSTYCPP_RUST
pub trait SinkBase {
    fn write_bytes(&mut self, p: *const u8, n: usize);
}
#endif
/*RUSTYCPP:GEN-BEGIN id=serializable.sink_base version=1 rust_sha256=c6dde192737a6cb41eeb1ea174af464c9775406a8821e4aaab6934e2d107bbab*/
class SinkBase {
public:
    virtual ~SinkBase() noexcept(false) {}
    virtual void write_bytes(const uint8_t* p, size_t n) = 0;
    SinkBase(const SinkBase&) = delete;
    SinkBase& operator=(const SinkBase&) = delete;
    SinkBase(SinkBase&&) = delete;
    SinkBase& operator=(SinkBase&&) = delete;
protected:
    SinkBase() = default;
};

template <class U> class SinkBaseAdapter;
template <class U> class SinkBaseAdapterRef;
template <class U> class SinkBaseAdapterRefMut;
/*RUSTYCPP:GEN-END id=serializable.sink_base*/
using SinkProxy = rusty::Box<SinkBase>;

// Source: returns the number of bytes actually read (may be < n at EOF).
//
// Convention: returns 0 at EOF; raises (or aborts) on transport error.
// Concrete sources control their own buffering / blocking semantics.
//
// Authored as inline Rust DSL — same pattern as SinkBase above.
// (`read` isn't in the rusty-cpp keyword-escape list so the DSL name
// emits verbatim, but we use `read_bytes` for symmetry with the sink
// side.)
#if RUSTYCPP_RUST
pub trait SourceBase {
    fn read_bytes(&mut self, p: *mut u8, n: usize) -> usize;
}
#endif
/*RUSTYCPP:GEN-BEGIN id=serializable.source_base version=1 rust_sha256=8ca92ac4f6d36965baaf91b4f6c0f852f253d7cd57cdb4177b27183f31a9a383*/
class SourceBase {
public:
    virtual ~SourceBase() noexcept(false) {}
    virtual size_t read_bytes(uint8_t* p, size_t n) = 0;
    SourceBase(const SourceBase&) = delete;
    SourceBase& operator=(const SourceBase&) = delete;
    SourceBase(SourceBase&&) = delete;
    SourceBase& operator=(SourceBase&&) = delete;
protected:
    SourceBase() = default;
};

template <class U> class SourceBaseAdapter;
template <class U> class SourceBaseAdapterRef;
template <class U> class SourceBaseAdapterRefMut;
/*RUSTYCPP:GEN-END id=serializable.source_base*/
using SourceProxy = rusty::Box<SourceBase>;

// ---------------------------------------------------------------------------
// Concrete Sink / Source — Phase 1a: in-memory buffer only.
// ---------------------------------------------------------------------------

// In-memory byte sink. Bytes accumulate in `bytes` and can be observed
// after each write or at the end. Mostly used for tests + as the
// internal buffer for cases where Marshal-style accumulation is needed.
//
// Note: `rusty::Vec::reserve(n)` sets capacity to exactly `n` (not
// geometric grow), so we double the capacity ourselves on every
// realloc to keep amortized append cost O(1).  Without this, a
// frame built from N small writes triggers N reallocations.
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block.
//
// `write(const void*, size_t)` lives OUTSIDE the DSL block as a free
// function (`buffer_sink_write`) — the body's `std::memcpy` over the
// raw `const void*` parameter isn't expressible in inline-Rust today
// (the DSL grammar doesn't accept `void*`). That was the previous
// "trivial-blocked (void* in param)" classification. Callers that
// need to reset just touch the field directly via `sink.bytes.clear()`
// (the legacy `clear()` method had zero callers).
struct BufferSink;
inline void buffer_sink_write(BufferSink& self, const void* p, size_t n);
#if RUSTYCPP_RUST
struct BufferSink {
    bytes: Vec<u8>,
}
impl SinkBase for BufferSink {
    fn write_bytes(&mut self, p: *const u8, n: usize) {
        buffer_sink_write(self, p, n);
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=serializable.buffer_sink version=1 rust_sha256=227637d9171f91f5238e1c5a35a9f22536242cba0d954607c090dd07dbe7baeb*/
struct BufferSink;

struct BufferSink {
    rusty::Vec<uint8_t> bytes;

    void write_bytes(const uint8_t* p, size_t n);
};


void BufferSink::write_bytes(const uint8_t* p, size_t n) {
    buffer_sink_write((*this), p, std::move(n));
}

template <>
class SinkBaseAdapter<BufferSink> final : public SinkBase {
    BufferSink value_;
public:
    explicit SinkBaseAdapter(BufferSink v) : value_(std::move(v)) {}
    void write_bytes(const uint8_t* p, size_t n) override {
        value_.write_bytes(p, n);
    }
};

template <>
class SinkBaseAdapterRef<BufferSink> final : public SinkBase {
    const BufferSink& value_;
public:
    explicit SinkBaseAdapterRef(const BufferSink& u) : value_(u) {}
    void write_bytes(const uint8_t* p, size_t n) override {
        std::abort();  // unreachable through &dyn T
    }
};

template <>
class SinkBaseAdapterRefMut<BufferSink> final : public SinkBase {
    BufferSink& value_;
public:
    explicit SinkBaseAdapterRefMut(BufferSink& u) : value_(u) {}
    void write_bytes(const uint8_t* p, size_t n) override {
        value_.write_bytes(p, n);
    }
};
/*RUSTYCPP:GEN-END id=serializable.buffer_sink*/

// @unsafe { memcpy + set_len bypass per-element init for trivial T=uint8_t }
// Free function — kept outside the DSL block because the body's
// `std::memcpy` over a `const void*` parameter isn't expressible in
// inline-Rust today.
inline void buffer_sink_write(BufferSink& self, const void* p, size_t n) {
    if (n == 0) return;
    const size_t old_len = self.bytes.len();
    const size_t needed = old_len + n;
    if (needed > self.bytes.capacity()) {
        size_t new_cap = self.bytes.capacity() == 0 ? 64 : self.bytes.capacity() * 2;
        while (new_cap < needed) new_cap *= 2;
        self.bytes.reserve(new_cap);
    }
    std::memcpy(self.bytes.data() + old_len, p, n);
    self.bytes.set_len(needed);
}

// In-memory byte source. Wraps a `const uint8_t*` view + length;
// caller owns the underlying storage.
//
// Returns the number of bytes actually copied. At EOF (pos_ == len_)
// returns 0; partial reads at the tail are allowed (not aborted).
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block.
//
// `read(void*, size_t)` lives OUTSIDE the DSL block as a free function
// `buffer_source_read` — the body's `void*` parameter and `memcpy`
// aren't expressible in inline-Rust today. The existing 2-arg
// paren-init form `BufferSource src(data, len)` keeps working via
// C++20 aggregate paren-init; callers can also use the DSL `fn new`
// factory directly (`BufferSource::new_(data, len)`).
struct BufferSource;
inline size_t buffer_source_read(BufferSource& self, void* p, size_t n);
#if RUSTYCPP_RUST
struct BufferSource {
    data_: *const u8,
    len_: usize,
    pos_: usize,
}

impl BufferSource {
    fn new(data: *const u8, len: usize) -> BufferSource {
        BufferSource {
            data_: data as *const u8,
            len_: len,
            pos_: 0usize,
        }
    }

    fn pos(&self) -> usize { self.pos_ }
    fn remaining(&self) -> usize { self.len_ - self.pos_ }
    fn eof(&self) -> bool { self.pos_ >= self.len_ }
}

impl SourceBase for BufferSource {
    fn read_bytes(&mut self, p: *mut u8, n: usize) -> usize {
        buffer_source_read(self, p, n)
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=serializable.buffer_source version=1 rust_sha256=8f444f57cba3be172b7b75830a7f3568b8a1158b3f63353db1fb2bf3b141cd79*/
struct BufferSource;

struct BufferSource {
    const uint8_t* data_;
    size_t len_;
    size_t pos_;

    static BufferSource new_(const uint8_t* data, size_t len);
    size_t pos() const;
    size_t remaining() const;
    bool eof() const;
    size_t read_bytes(uint8_t* p, size_t n);
};


BufferSource BufferSource::new_(const uint8_t* data, size_t len) {
    return BufferSource{.data_ = reinterpret_cast<const uint8_t*>(data), .len_ = std::move(len), .pos_ = static_cast<size_t>(0)};
}

size_t BufferSource::pos() const {
    return this->pos_;
}

size_t BufferSource::remaining() const {
    return rusty::detail::deref_if_pointer_like(this->len_) - rusty::detail::deref_if_pointer_like(this->pos_);
}

bool BufferSource::eof() const {
    return rusty::detail::deref_if_pointer_like(this->pos_) >= rusty::detail::deref_if_pointer_like(this->len_);
}

size_t BufferSource::read_bytes(uint8_t* p, size_t n) {
    return buffer_source_read((*this), p, std::move(n));
}

template <>
class SourceBaseAdapter<BufferSource> final : public SourceBase {
    BufferSource value_;
public:
    explicit SourceBaseAdapter(BufferSource v) : value_(std::move(v)) {}
    size_t read_bytes(uint8_t* p, size_t n) override {
        return value_.read_bytes(p, n);
    }
};

template <>
class SourceBaseAdapterRef<BufferSource> final : public SourceBase {
    const BufferSource& value_;
public:
    explicit SourceBaseAdapterRef(const BufferSource& u) : value_(u) {}
    size_t read_bytes(uint8_t* p, size_t n) override {
        std::abort();  // unreachable through &dyn T
    }
};

template <>
class SourceBaseAdapterRefMut<BufferSource> final : public SourceBase {
    BufferSource& value_;
public:
    explicit SourceBaseAdapterRefMut(BufferSource& u) : value_(u) {}
    size_t read_bytes(uint8_t* p, size_t n) override {
        return value_.read_bytes(p, n);
    }
};
/*RUSTYCPP:GEN-END id=serializable.buffer_source*/

// @unsafe - raw pointer read; memcpy from data_ + pos_.
inline size_t buffer_source_read(BufferSource& self, void* p, size_t n) {
    size_t avail = self.len_ - self.pos_;
    size_t take  = (n < avail) ? n : avail;
    if (take > 0) {
        std::memcpy(p, self.data_ + self.pos_, take);
        self.pos_ += take;
    }
    return take;
}

// Adapter wrappers for the SinkBase / SourceBase virtual bases.
//
// Adapters wrap a non-owning raw pointer to the concrete sink/source
// and forward `write` / `read` through it.
//
// Lifetime: the proxy must not outlive `*sink` / `*source`.
inline SinkProxy make_sink_proxy(BufferSink* sink) {
  return rusty::make_box<SinkBaseAdapterRefMut<BufferSink>>(*sink);
}
inline SourceProxy make_source_proxy(BufferSource* source) {
  return rusty::make_box<SourceBaseAdapterRefMut<BufferSource>>(*source);
}

// ---------------------------------------------------------------------------
// File descriptor Sink / Source.
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

// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block.
//
// `write(const void*, size_t)` / `read(void*, size_t)` live OUTSIDE
// the DSL block as `fd_sink_write` / `fd_source_read` — the body's
// `::write`/`::read` libc syscalls take `const void*`/`void*` and
// the loop's `static_cast<const uint8_t*>` aren't expressible in
// inline-Rust today. The DSL `fn new(fd: i32)` factory keeps the
// existing 1-arg paren-init form working via C++20 aggregate
// paren-init.
struct FdSink;
inline void fd_sink_write(FdSink& self, const void* p, size_t n);
#if RUSTYCPP_RUST
struct FdSink {
    fd_: i32,
}

impl FdSink {
    fn new(fd: i32) -> FdSink {
        FdSink { fd_: fd }
    }

    fn fd(&self) -> i32 { self.fd_ }
}

impl SinkBase for FdSink {
    fn write_bytes(&mut self, p: *const u8, n: usize) {
        fd_sink_write(self, p, n);
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=serializable.fd_sink version=1 rust_sha256=9e2ba62c1d8eaa1ce731c1fb3782ff92affefcbd880c0e97221e86f52f88f303*/
struct FdSink;

struct FdSink {
    int32_t fd_;

    static FdSink new_(int32_t fd);
    int32_t fd() const;
    void write_bytes(const uint8_t* p, size_t n);
};


FdSink FdSink::new_(int32_t fd) {
    return FdSink{.fd_ = std::move(fd)};
}

int32_t FdSink::fd() const {
    return this->fd_;
}

void FdSink::write_bytes(const uint8_t* p, size_t n) {
    fd_sink_write((*this), p, std::move(n));
}

template <>
class SinkBaseAdapter<FdSink> final : public SinkBase {
    FdSink value_;
public:
    explicit SinkBaseAdapter(FdSink v) : value_(std::move(v)) {}
    void write_bytes(const uint8_t* p, size_t n) override {
        value_.write_bytes(p, n);
    }
};

template <>
class SinkBaseAdapterRef<FdSink> final : public SinkBase {
    const FdSink& value_;
public:
    explicit SinkBaseAdapterRef(const FdSink& u) : value_(u) {}
    void write_bytes(const uint8_t* p, size_t n) override {
        std::abort();  // unreachable through &dyn T
    }
};

template <>
class SinkBaseAdapterRefMut<FdSink> final : public SinkBase {
    FdSink& value_;
public:
    explicit SinkBaseAdapterRefMut(FdSink& u) : value_(u) {}
    void write_bytes(const uint8_t* p, size_t n) override {
        value_.write_bytes(p, n);
    }
};
/*RUSTYCPP:GEN-END id=serializable.fd_sink*/

// @safe - The only raw-pointer op is ::write itself, annotated below.
inline void fd_sink_write(FdSink& self, const void* p, size_t n) {
    if (n == 0) return;
    const auto* b = static_cast<const uint8_t*>(p);
    size_t written = 0;
    while (written < n) {
        // @unsafe { ::write — raw libc syscall on a fd we don't own }
        ssize_t r = ::write(self.fd_, b + written, n - written);
        if (r < 0) {
            if (errno == EINTR) continue;
            verify(false);
        }
        verify(r > 0);
        written += static_cast<size_t>(r);
    }
}

struct FdSource;
inline size_t fd_source_read(FdSource& self, void* p, size_t n);
#if RUSTYCPP_RUST
struct FdSource {
    fd_: i32,
}

impl FdSource {
    fn new(fd: i32) -> FdSource {
        FdSource { fd_: fd }
    }

    fn fd(&self) -> i32 { self.fd_ }
}

impl SourceBase for FdSource {
    fn read_bytes(&mut self, p: *mut u8, n: usize) -> usize {
        fd_source_read(self, p, n)
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=serializable.fd_source version=1 rust_sha256=cc6fa226d5fe2f963b8915313a0153781c32b65b563261cdfb5924ab7540bfd3*/
struct FdSource;

struct FdSource {
    int32_t fd_;

    static FdSource new_(int32_t fd);
    int32_t fd() const;
    size_t read_bytes(uint8_t* p, size_t n);
};


FdSource FdSource::new_(int32_t fd) {
    return FdSource{.fd_ = std::move(fd)};
}

int32_t FdSource::fd() const {
    return this->fd_;
}

size_t FdSource::read_bytes(uint8_t* p, size_t n) {
    return fd_source_read((*this), p, std::move(n));
}

template <>
class SourceBaseAdapter<FdSource> final : public SourceBase {
    FdSource value_;
public:
    explicit SourceBaseAdapter(FdSource v) : value_(std::move(v)) {}
    size_t read_bytes(uint8_t* p, size_t n) override {
        return value_.read_bytes(p, n);
    }
};

template <>
class SourceBaseAdapterRef<FdSource> final : public SourceBase {
    const FdSource& value_;
public:
    explicit SourceBaseAdapterRef(const FdSource& u) : value_(u) {}
    size_t read_bytes(uint8_t* p, size_t n) override {
        std::abort();  // unreachable through &dyn T
    }
};

template <>
class SourceBaseAdapterRefMut<FdSource> final : public SourceBase {
    FdSource& value_;
public:
    explicit SourceBaseAdapterRefMut(FdSource& u) : value_(u) {}
    size_t read_bytes(uint8_t* p, size_t n) override {
        return value_.read_bytes(p, n);
    }
};
/*RUSTYCPP:GEN-END id=serializable.fd_source*/

// @safe - The only raw-pointer op is ::read itself, annotated below.
inline size_t fd_source_read(FdSource& self, void* p, size_t n) {
    if (n == 0) return 0;
    auto* b = static_cast<uint8_t*>(p);
    size_t got = 0;
    while (got < n) {
        // @unsafe { ::read — raw libc syscall on a fd we don't own }
        ssize_t r = ::read(self.fd_, b + got, n - got);
        if (r < 0) {
            if (errno == EINTR) continue;
            verify(false);
        }
        if (r == 0) break;  // EOF — return short read.
        got += static_cast<size_t>(r);
    }
    return got;
}

inline SinkProxy make_sink_proxy(FdSink* sink) {
  return rusty::make_box<SinkBaseAdapterRefMut<FdSink>>(*sink);
}
inline SourceProxy make_source_proxy(FdSource* source) {
  return rusty::make_box<SourceBaseAdapterRefMut<FdSource>>(*source);
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

struct BinaryWriteArchive;
struct BinaryReadArchive;

// Byte kernels for the DSL archives below (Box-proxy arrow calls).
void bwa_write_bytes(BinaryWriteArchive& self, const std::uint8_t* p, std::size_t n);
bool bra_read_exact(BinaryReadArchive& self, std::uint8_t* p, std::size_t n);

// `BinaryWriteArchive` — the wire-format encoder over a type-erased
// SinkProxy. Authored as inline Rust DSL: the `#if RUSTYCPP_RUST`
// block below is the source of truth; the transpiler regenerates the
// matching `/*RUSTYCPP:GEN-BEGIN ... END*/` block.
//
// Behavioral diffs from the original C++ class:
//   * The struct is a single-field aggregate with NO constructors: the
//     ubiquitous `BinaryWriteArchive ar(make_sink_proxy(&x));` sites
//     (incl. 150+ in generated rcc_rpc.h) keep compiling via C++20
//     parenthesized aggregate init — exact single-field match, so the
//     multi-field misfill hazard does not apply. The BufferSink*/
//     FdSink* convenience ctors are gone; their four call sites now
//     pass make_sink_proxy(...) explicitly. `explicit`/noexcept drop.
//   * write_bytes takes *const u8 (no void* in the DSL); the few
//     raw-buffer callers cast at the boundary.
#if RUSTYCPP_RUST
struct BinaryWriteArchive {
    sink_: SinkProxy,
}

impl BinaryWriteArchive {
    // Emit raw bytes (used for unstructured payloads).
    fn write_bytes(&mut self, p: *const u8, n: usize) {
        bwa_write_bytes(self, p, n)
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=serializable.write_archive version=1 rust_sha256=7dded1c21502d9a4f7a9fe3a23e4aaddbd833fc70037dfe7b45d91c2a8784c02*/
struct BinaryWriteArchive;

struct BinaryWriteArchive {
    SinkProxy sink_;

    void write_bytes(const uint8_t* p, size_t n);
};


void BinaryWriteArchive::write_bytes(const uint8_t* p, size_t n) {
    bwa_write_bytes((*this), p, std::move(n));
}
/*RUSTYCPP:GEN-END id=serializable.write_archive*/

// ---- Fixed-width primitives. ------------------------------------------
// Each scalar's byte representation is `reinterpret_cast<const
// uint8_t*>(&v)`; the trait's `const uint8_t*` parameter type
// makes the byte view explicit (the previous `const void*` form
// hid it behind implicit conversion).
inline BinaryWriteArchive& operator<<(BinaryWriteArchive& ar, int8_t v)   { ar.write_bytes(reinterpret_cast<const uint8_t*>(&v), sizeof(v)); return ar; }
inline BinaryWriteArchive& operator<<(BinaryWriteArchive& ar, int16_t v)  { ar.write_bytes(reinterpret_cast<const uint8_t*>(&v), sizeof(v)); return ar; }
inline BinaryWriteArchive& operator<<(BinaryWriteArchive& ar, int32_t v)  { ar.write_bytes(reinterpret_cast<const uint8_t*>(&v), sizeof(v)); return ar; }
inline BinaryWriteArchive& operator<<(BinaryWriteArchive& ar, int64_t v)  { ar.write_bytes(reinterpret_cast<const uint8_t*>(&v), sizeof(v)); return ar; }
inline BinaryWriteArchive& operator<<(BinaryWriteArchive& ar, uint8_t v)  { ar.write_bytes(&v, sizeof(v)); return ar; }
inline BinaryWriteArchive& operator<<(BinaryWriteArchive& ar, uint16_t v) { ar.write_bytes(reinterpret_cast<const uint8_t*>(&v), sizeof(v)); return ar; }
inline BinaryWriteArchive& operator<<(BinaryWriteArchive& ar, uint32_t v) { ar.write_bytes(reinterpret_cast<const uint8_t*>(&v), sizeof(v)); return ar; }
inline BinaryWriteArchive& operator<<(BinaryWriteArchive& ar, uint64_t v) { ar.write_bytes(reinterpret_cast<const uint8_t*>(&v), sizeof(v)); return ar; }
inline BinaryWriteArchive& operator<<(BinaryWriteArchive& ar, double v)   { ar.write_bytes(reinterpret_cast<const uint8_t*>(&v), sizeof(v)); return ar; }

// ---- Variable-length integer encoding (SparseInt). --------------------
inline BinaryWriteArchive& operator<<(BinaryWriteArchive& ar, rrr::v32 v) {
  char buf[5];
  size_t bsize = rrr::sparseint_dump(v.get(), buf);
  ar.write_bytes(reinterpret_cast<const uint8_t*>(buf), bsize);
  return ar;
}

inline BinaryWriteArchive& operator<<(BinaryWriteArchive& ar, rrr::v64 v) {
  char buf[9];
  size_t bsize = rrr::sparseint_dump(v.get(), buf);
  ar.write_bytes(reinterpret_cast<const uint8_t*>(buf), bsize);
  return ar;
}

// ---- Variable-length byte sequences. ----------------------------------
inline BinaryWriteArchive& operator<<(BinaryWriteArchive& ar, std::string_view s) {
  rrr::v64 v_len{static_cast<rrr::i64>(s.size())};
  ar << v_len;
  if (s.size() > 0) {
    ar.write_bytes(reinterpret_cast<const uint8_t*>(s.data()), s.size());
  }
  return ar;
}

// std::string is a convenience overload — same wire format as
// string_view (length-prefixed bytes).
inline BinaryWriteArchive& operator<<(BinaryWriteArchive& ar, const std::string& s) {
  return ar << std::string_view{s};
}

// ---- Composites. ------------------------------------------------------
// std::pair: write first then second, no length prefix (each side
// already knows the type and consumes its own bytes).
template<class T1, class T2>
inline BinaryWriteArchive& operator<<(BinaryWriteArchive& ar, const std::pair<T1, T2>& v) {
  ar << v.first;
  ar << v.second;
  return ar;
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
inline BinaryWriteArchive& operator<<(BinaryWriteArchive& ar, const rusty::Vec<T>& v) {
  rrr::v64 v_len{static_cast<rrr::i64>(v.size())};
  ar << v_len;
  for (auto it = v.begin(); it != v.end(); ++it) ar << *it;
  return ar;
}

template<class T>
inline BinaryWriteArchive& operator<<(BinaryWriteArchive& ar, const std::vector<T>& v) {
  rrr::v64 v_len{static_cast<rrr::i64>(v.size())};
  ar << v_len;
  for (auto it = v.begin(); it != v.end(); ++it) ar << *it;
  return ar;
}

template<class T>
inline BinaryWriteArchive& operator<<(BinaryWriteArchive& ar, const std::list<T>& v) {
  rrr::v64 v_len{static_cast<rrr::i64>(v.size())};
  ar << v_len;
  for (auto it = v.begin(); it != v.end(); ++it) ar << *it;
  return ar;
}

template<class T>
inline BinaryWriteArchive& operator<<(BinaryWriteArchive& ar, const rusty::BTreeSet<T>& v) {
  rrr::v64 v_len{static_cast<rrr::i64>(v.len())};
  ar << v_len;
  // rusty BTreeSet has no begin()/end(); iterate the Rust-style iterator.
  auto __it = v.iter();
  for (auto __e = __it.next(); __e.is_some(); __e = __it.next())
    ar << std::move(__e).unwrap();
  return ar;
}

template<class T>
inline BinaryWriteArchive& operator<<(BinaryWriteArchive& ar, const std::set<T>& v) {
  rrr::v64 v_len{static_cast<rrr::i64>(v.size())};
  ar << v_len;
  for (auto it = v.begin(); it != v.end(); ++it) ar << *it;
  return ar;
}

template<class T>
inline BinaryWriteArchive& operator<<(BinaryWriteArchive& ar, const rusty::HashSet<T>& v) {
  rrr::v64 v_len{static_cast<rrr::i64>(v.len())};
  ar << v_len;
  // HashSet wraps a HashMap<T, monostate>; walk the underlying map's const
  // Rust iterator (HashSet's own begin()/end() is non-const). next() yields
  // Option<tuple<const T&, ...>>.
  // WARNING: ANY hashbrown enumeration (iter()/begin()/drain()) routes
  // through the `rusty::iter(table)` lambda in slice.hpp, whose return-type
  // name crashes clang-22's Itanium mangler (SIGSEGV in mangleSourceName).
  // So this overload MUST NOT be instantiated on clang-22 — there is no
  // crash-free way to enumerate a hashbrown table there. No production code
  // serializes a rusty::HashSet today; if that changes, the encoder needs a
  // mangler-safe enumeration path (or a fixed toolchain). See the
  // RustyHashSetPrimitives test for the decoder-only workaround.
  auto __it = v.map.iter();
  for (auto __e = __it.next(); __e.is_some(); __e = __it.next())
    ar << std::get<0>(std::move(__e).unwrap());
  return ar;
}

template<class T>
inline BinaryWriteArchive& operator<<(BinaryWriteArchive& ar, const std::unordered_set<T>& v) {
  rrr::v64 v_len{static_cast<rrr::i64>(v.size())};
  ar << v_len;
  for (auto it = v.begin(); it != v.end(); ++it) ar << *it;
  return ar;
}

template<class K, class V>
inline BinaryWriteArchive& operator<<(BinaryWriteArchive& ar, const rusty::BTreeMap<K, V>& v) {
  rrr::v64 v_len{static_cast<rrr::i64>(v.len())};
  ar << v_len;
  // rusty BTreeMap has no begin()/end(); iterate the Rust-style iterator.
  // iter().next() yields Option<std::tuple<const K&, V&>>.
  auto __it = v.iter();
  for (auto __e = __it.next(); __e.is_some(); __e = __it.next()) {
    auto kv = std::move(__e).unwrap();
    ar << std::get<0>(kv) << std::get<1>(kv);
  }
  return ar;
}

template<class K, class V>
inline BinaryWriteArchive& operator<<(BinaryWriteArchive& ar, const std::map<K, V>& v) {
  rrr::v64 v_len{static_cast<rrr::i64>(v.size())};
  ar << v_len;
  for (auto it = v.begin(); it != v.end(); ++it) {
    ar << it->first << it->second;
  }
  return ar;
}

template<class K, class V>
inline BinaryWriteArchive& operator<<(BinaryWriteArchive& ar, const rusty::HashMap<K, V>& v) {
  rrr::v64 v_len{static_cast<rrr::i64>(v.len())};
  ar << v_len;
  // rusty HashMap has no const begin()/end(); iterate via the Rust iterator.
  // iter().next() yields Option<std::tuple<const K&, const V&>>.
  // WARNING: like rusty::HashSet above, hashbrown enumeration crashes
  // clang-22's name mangler — do not instantiate this overload on clang-22.
  auto __it = v.iter();
  for (auto __e = __it.next(); __e.is_some(); __e = __it.next()) {
    auto kv = std::move(__e).unwrap();
    ar << std::get<0>(kv) << std::get<1>(kv);
  }
  return ar;
}

template<class K, class V>
inline BinaryWriteArchive& operator<<(BinaryWriteArchive& ar, const std::unordered_map<K, V>& v) {
  rrr::v64 v_len{static_cast<rrr::i64>(v.size())};
  ar << v_len;
  for (auto it = v.begin(); it != v.end(); ++it) {
    ar << it->first << it->second;
  }
  return ar;
}


// `BinaryReadArchive` — the wire-format decoder over a type-erased
// SourceProxy. Same aggregate/ctor story as BinaryWriteArchive above;
// read_exact keeps its verify-at-caller bool contract ([[nodiscard]]
// drops — the DSL has no attribute syntax; every caller already
// verify()s the result).
#if RUSTYCPP_RUST
struct BinaryReadArchive {
    source_: SourceProxy,
}

impl BinaryReadArchive {
    // Read into raw bytes; false if the source ran out.
    fn read_exact(&mut self, p: *mut u8, n: usize) -> bool {
        bra_read_exact(self, p, n)
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=serializable.read_archive version=1 rust_sha256=374c7bfbe6fb7868add3bb9adfab547b3eef450e27c96acaf02928770b62b47f*/
struct BinaryReadArchive;

struct BinaryReadArchive {
    SourceProxy source_;

    bool read_exact(uint8_t* p, size_t n);
};


bool BinaryReadArchive::read_exact(uint8_t* p, size_t n) {
    return bra_read_exact((*this), p, std::move(n));
}
/*RUSTYCPP:GEN-END id=serializable.read_archive*/

// ---- Archive byte kernels (Box-proxy arrow boundary) -----------------

// @unsafe - virtual write through the type-erased sink proxy.
inline void bwa_write_bytes(BinaryWriteArchive& self, const std::uint8_t* p,
                            std::size_t n) {
  self.sink_->write_bytes(p, n);
}

// @unsafe - virtual read through the type-erased source proxy.
inline bool bra_read_exact(BinaryReadArchive& self, std::uint8_t* p,
                           std::size_t n) {
  std::size_t got = self.source_->read_bytes(p, n);
  return got == n;
}

// ---- Fixed-width primitives. ------------------------------------------
// Each operator>> verifies the read produced sizeof(T) bytes; on
// truncation it aborts via `verify` (matches the existing
// `Marshal::read` contract — short reads at the boundary are
// programming errors at this layer, not recoverable conditions).
inline BinaryReadArchive& operator>>(BinaryReadArchive& ar, int8_t& v)   { verify(ar.read_exact(reinterpret_cast<uint8_t*>(&v), sizeof(v))); return ar; }
inline BinaryReadArchive& operator>>(BinaryReadArchive& ar, int16_t& v)  { verify(ar.read_exact(reinterpret_cast<uint8_t*>(&v), sizeof(v))); return ar; }
inline BinaryReadArchive& operator>>(BinaryReadArchive& ar, int32_t& v)  { verify(ar.read_exact(reinterpret_cast<uint8_t*>(&v), sizeof(v))); return ar; }
inline BinaryReadArchive& operator>>(BinaryReadArchive& ar, int64_t& v)  { verify(ar.read_exact(reinterpret_cast<uint8_t*>(&v), sizeof(v))); return ar; }
inline BinaryReadArchive& operator>>(BinaryReadArchive& ar, uint8_t& v)  { verify(ar.read_exact(reinterpret_cast<uint8_t*>(&v), sizeof(v))); return ar; }
inline BinaryReadArchive& operator>>(BinaryReadArchive& ar, uint16_t& v) { verify(ar.read_exact(reinterpret_cast<uint8_t*>(&v), sizeof(v))); return ar; }
inline BinaryReadArchive& operator>>(BinaryReadArchive& ar, uint32_t& v) { verify(ar.read_exact(reinterpret_cast<uint8_t*>(&v), sizeof(v))); return ar; }
inline BinaryReadArchive& operator>>(BinaryReadArchive& ar, uint64_t& v) { verify(ar.read_exact(reinterpret_cast<uint8_t*>(&v), sizeof(v))); return ar; }
inline BinaryReadArchive& operator>>(BinaryReadArchive& ar, double& v)   { verify(ar.read_exact(reinterpret_cast<uint8_t*>(&v), sizeof(v))); return ar; }

// ---- Variable-length integer encoding (SparseInt). --------------------
// SparseInt's first byte determines the total length; we peek it,
// read the remaining bytes, then decode.
inline BinaryReadArchive& operator>>(BinaryReadArchive& ar, rrr::v32& v) {
  char buf[5];
  verify(ar.read_exact(reinterpret_cast<uint8_t*>(buf), 1));
  size_t total = rrr::SparseInt::buf_size(buf[0]);
  if (total > 1) {
    verify(ar.read_exact(reinterpret_cast<uint8_t*>(buf + 1), total - 1));
  }
  v.set(rrr::sparseint_load_i32(buf));
  return ar;
}

inline BinaryReadArchive& operator>>(BinaryReadArchive& ar, rrr::v64& v) {
  char buf[9];
  verify(ar.read_exact(reinterpret_cast<uint8_t*>(buf), 1));
  size_t total = rrr::SparseInt::buf_size(buf[0]);
  if (total > 1) {
    verify(ar.read_exact(reinterpret_cast<uint8_t*>(buf + 1), total - 1));
  }
  v.set(rrr::sparseint_load_i64(buf));
  return ar;
}

// ---- Variable-length byte sequences. ----------------------------------
inline BinaryReadArchive& operator>>(BinaryReadArchive& ar, std::string& s) {
  rrr::v64 v_len{0};
  ar >> v_len;
  auto len = static_cast<size_t>(v_len.get());
  s.resize(len);
  if (len > 0) {
    // @unsafe { writing into string's internal buffer via &s[0] }
    verify(ar.read_exact(reinterpret_cast<uint8_t*>(&s[0]), len));
  }
  return ar;
}

// ---- Composites. ------------------------------------------------------
template<class T1, class T2>
inline BinaryReadArchive& operator>>(BinaryReadArchive& ar, std::pair<T1, T2>& v) {
  ar >> v.first;
  ar >> v.second;
  return ar;
}

// ---- Linear containers. -----------------------------------------------
// Wire format: v64 length prefix + N elements deserialized in order.
// Containers are cleared first; matches the existing Marshal operator>>
// semantics.
template<class T>
inline BinaryReadArchive& operator>>(BinaryReadArchive& ar, rusty::Vec<T>& v) {
  rrr::v64 v_len{0};
  ar >> v_len;
  v.clear();
  auto n = static_cast<size_t>(v_len.get());
  v.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    T elem{};
    ar >> elem;
    v.push(std::move(elem));
  }
  return ar;
}

template<class T>
inline BinaryReadArchive& operator>>(BinaryReadArchive& ar, std::vector<T>& v) {
  rrr::v64 v_len{0};
  ar >> v_len;
  v.clear();
  auto n = static_cast<size_t>(v_len.get());
  v.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    T elem{};
    ar >> elem;
    v.push_back(std::move(elem));
  }
  return ar;
}

template<class T>
inline BinaryReadArchive& operator>>(BinaryReadArchive& ar, std::list<T>& v) {
  rrr::v64 v_len{0};
  ar >> v_len;
  v.clear();
  auto n = static_cast<size_t>(v_len.get());
  for (size_t i = 0; i < n; ++i) {
    T elem{};
    ar >> elem;
    v.push_back(std::move(elem));
  }
  return ar;
}

template<class T>
inline BinaryReadArchive& operator>>(BinaryReadArchive& ar, rusty::BTreeSet<T>& v) {
  rrr::v64 v_len{0};
  ar >> v_len;
  v.clear();
  auto n = static_cast<size_t>(v_len.get());
  for (size_t i = 0; i < n; ++i) {
    T elem{};
    ar >> elem;
    v.insert(std::move(elem));
  }
  return ar;
}

template<class T>
inline BinaryReadArchive& operator>>(BinaryReadArchive& ar, std::set<T>& v) {
  rrr::v64 v_len{0};
  ar >> v_len;
  v.clear();
  auto n = static_cast<size_t>(v_len.get());
  for (size_t i = 0; i < n; ++i) {
    T elem{};
    ar >> elem;
    v.insert(std::move(elem));
  }
  return ar;
}

template<class T>
inline BinaryReadArchive& operator>>(BinaryReadArchive& ar, rusty::HashSet<T>& v) {
  rrr::v64 v_len{0};
  ar >> v_len;
  v.clear();
  auto n = static_cast<size_t>(v_len.get());
  for (size_t i = 0; i < n; ++i) {
    T elem{};
    ar >> elem;
    v.insert(std::move(elem));
  }
  return ar;
}

template<class T>
inline BinaryReadArchive& operator>>(BinaryReadArchive& ar, std::unordered_set<T>& v) {
  rrr::v64 v_len{0};
  ar >> v_len;
  v.clear();
  auto n = static_cast<size_t>(v_len.get());
  for (size_t i = 0; i < n; ++i) {
    T elem{};
    ar >> elem;
    v.insert(std::move(elem));
  }
  return ar;
}

template<class K, class V>
inline BinaryReadArchive& operator>>(BinaryReadArchive& ar, rusty::BTreeMap<K, V>& v) {
  rrr::v64 v_len{0};
  ar >> v_len;
  v.clear();
  auto n = static_cast<size_t>(v_len.get());
  for (size_t i = 0; i < n; ++i) {
    K key{};
    V value{};
    ar >> key >> value;
    v.insert(std::move(key), std::move(value));
  }
  return ar;
}

template<class K, class V>
inline BinaryReadArchive& operator>>(BinaryReadArchive& ar, std::map<K, V>& v) {
  rrr::v64 v_len{0};
  ar >> v_len;
  v.clear();
  auto n = static_cast<size_t>(v_len.get());
  for (size_t i = 0; i < n; ++i) {
    K key{};
    V value{};
    ar >> key >> value;
    v.emplace(std::move(key), std::move(value));
  }
  return ar;
}

template<class K, class V>
inline BinaryReadArchive& operator>>(BinaryReadArchive& ar, rusty::HashMap<K, V>& v) {
  rrr::v64 v_len{0};
  ar >> v_len;
  v.clear();
  auto n = static_cast<size_t>(v_len.get());
  for (size_t i = 0; i < n; ++i) {
    K key{};
    V value{};
    ar >> key >> value;
    v.insert(std::move(key), std::move(value));
  }
  return ar;
}

template<class K, class V>
inline BinaryReadArchive& operator>>(BinaryReadArchive& ar, std::unordered_map<K, V>& v) {
  rrr::v64 v_len{0};
  ar >> v_len;
  v.clear();
  auto n = static_cast<size_t>(v_len.get());
  for (size_t i = 0; i < n; ++i) {
    K key{};
    V value{};
    ar >> key >> value;
    v.emplace(std::move(key), std::move(value));
  }
  return ar;
}


// ---------------------------------------------------------------------------
// Layer 4: Serializable proxy + factory registry.
//
// A type T satisfies the Serializable concept if it provides:
//   - void save(BinaryWriteArchive&) const  -- emit bytes
//   - void load(BinaryReadArchive&)         -- consume bytes
//   - int32_t kind() const                  -- factory tag
//
// SerializableBase type-erases over any such T (via virtual dispatch).
// SerializableRegistry maps a kind tag to a factory that constructs a
// fresh SerializableProxy (default-constructing the underlying T).
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

// Abstract base class for serializable payloads. Concrete derived
// types implement `save`, `load`, and `kind`. The proxy is a
// `std::shared_ptr<SerializableBase>` so SerializableEnvelope copies
// share the underlying payload via refcount (matching the original
// `support_copy<nontrivial>` semantics where copies via
// SerializableSharedPtrHolder shared the inner shared_ptr).
//
// Downcasting: `dynamic_cast<T*>(proxy.get())` recovers the concrete
// derived type.
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `/*RUSTYCPP:GEN-BEGIN ... END*/` block with the C++ abstract class.
// Tier-2.1 of the rrr trait sweep — same `pub trait` → namespace-
// scope-class pattern as PollableBase / Job / Channel*Base.
//
// `SerializableSharedPtrHolder<T>` inherits the emitted class as
// before; the DSL form adds `= delete` for copy/move on the base,
// but no caller copies/moves a `SerializableBase` value — it's only
// reachable through `std::shared_ptr<SerializableBase>`, which only
// needs the base to be polymorphic-deletable.
#if RUSTYCPP_RUST
pub trait SerializableBase {
    fn save(&self, ar: &mut BinaryWriteArchive);
    fn load(&mut self, ar: &mut BinaryReadArchive);
    fn kind(&self) -> i32;
}
#endif
/*RUSTYCPP:GEN-BEGIN id=serializable.1 version=1 rust_sha256=09faf724fbedcb9cf6f47203e2f6865062f27426a1e4b6a2f2c53a8bfe3afa60*/
class SerializableBase {
public:
    virtual ~SerializableBase() noexcept(false) {}
    virtual void save(BinaryWriteArchive& ar) const = 0;
    virtual void load(BinaryReadArchive& ar) = 0;
    virtual int32_t kind() const = 0;
    SerializableBase(const SerializableBase&) = delete;
    SerializableBase& operator=(const SerializableBase&) = delete;
    SerializableBase(SerializableBase&&) = delete;
    SerializableBase& operator=(SerializableBase&&) = delete;
protected:
    SerializableBase() = default;
};

template <class U> class SerializableBaseAdapter;
template <class U> class SerializableBaseAdapterRef;
template <class U> class SerializableBaseAdapterRefMut;
/*RUSTYCPP:GEN-END id=serializable.1*/

using SerializableProxy = std::shared_ptr<SerializableBase>;

namespace details {

// Wrapper used to put a `shared_ptr<T>` inside a SerializableProxy.
// Authored as generic inline Rust DSL with #[cpp_inherit]
// (SerializableBase is a DSL interface trait). Behavioral diffs:
//   * The unused default ctor (make_shared<T>() eager-construct) is
//     dropped — every construction site adopts an existing
//     shared_ptr<T> via the synthesized fieldwise ctor.
//   * std::shared_ptr is not in the transpiler's auto-deref set, so
//     the trait methods delegate to holder_* template free fns (found
//     by ADL at instantiation) that do the -> dispatch.
#if RUSTYCPP_RUST
struct SerializableSharedPtrHolder<T> {
    ptr: std::shared_ptr<T>,
}

#[cpp_inherit]
impl<T> SerializableBase for SerializableSharedPtrHolder<T> {
    fn save(&self, ar: &mut BinaryWriteArchive) {
        holder_save(self, ar)
    }
    fn load(&mut self, ar: &mut BinaryReadArchive) {
        holder_load(self, ar)
    }
    fn kind(&self) -> i32 {
        holder_kind(self)
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=serializable.shared_ptr_holder version=1 rust_sha256=be05fc063a8b3a66c7cda47018d08a37c2b929303cb86219404a2979b6cce2b8*/
template<typename T>
struct SerializableSharedPtrHolder;

template<typename T>
struct SerializableSharedPtrHolder : public SerializableBase {
    std::shared_ptr<T> ptr;
    SerializableSharedPtrHolder(std::shared_ptr<T> ptr_init) : SerializableBase(), ptr(std::move(ptr_init)) {}
    SerializableSharedPtrHolder(SerializableSharedPtrHolder&& other) noexcept : SerializableBase(), ptr(std::move(other.ptr)) {}


    void save(BinaryWriteArchive& ar) const {
        holder_save((*this), ar);
    }
    void load(BinaryReadArchive& ar) {
        holder_load((*this), ar);
    }
    int32_t kind() const {
        return holder_kind((*this));
    }
};
/*RUSTYCPP:GEN-END id=serializable.shared_ptr_holder*/

// @unsafe - shared_ptr arrow dispatch onto the held T.
template<typename T>
inline void holder_save(const SerializableSharedPtrHolder<T>& self,
                        BinaryWriteArchive& ar) {
  self.ptr->save(ar);
}
template<typename T>
inline void holder_load(SerializableSharedPtrHolder<T>& self,
                        BinaryReadArchive& ar) {
  self.ptr->load(ar);
}
template<typename T>
inline int32_t holder_kind(const SerializableSharedPtrHolder<T>& self) {
  return self.ptr->kind();
}

}  // namespace details

// CRTP base providing `kind()` (instance) + `static_kind()` (static)
// derived from a TypeList position.  Production payload types
// inherit `rrr::Serializable<MyType, MakoCommands>` to pick up these
// methods + satisfy the SerializableFacade convention shape.
//
// 2 step 5 (2026-05-05): moved here from marshal_serializable_bridge.hpp
// when that header retired with the rest of the bridge.
struct DefaultPayloadList {
  template<typename T>
  static constexpr int32_t index_of() noexcept { return 0; }
};

template<typename Derived, typename PayloadList = DefaultPayloadList>
struct Serializable {
  int32_t kind() const noexcept {
    return PayloadList::template index_of<Derived>();
  }
  static int32_t static_kind() noexcept {
    return PayloadList::template index_of<Derived>();
  }
};

// The structural contract for a Serializable-migrated T is:
//   - void save(BinaryWriteArchive&) const
//   - void load(BinaryReadArchive&)
//   - int32_t kind() const
// Overload disambiguation between the Marshallable subclass path
// and the Serializable bridge path uses
// `!std::is_base_of_v<Marshallable, T>`; shape mismatches surface
// as instantiation-time template errors rather than a separate
// constraint predicate.

// Construct a SerializableProxy that owns a T constructed from the
// forwarded arguments (default-constructed if no args). T just needs
// to satisfy the structural shape:
//   - void save(BinaryWriteArchive&) const
//   - void load(BinaryReadArchive&)
//   - int32_t kind() const
// The factory wraps T in a SerializableSharedPtrHolder<T> so callers
// can downcast back to T* via
//   dynamic_cast<details::SerializableSharedPtrHolder<T>*>(proxy.get())
// (or via the envelope's unpack<T>() / unpack_shared<T>()).
template<class T, class... Args>
inline SerializableProxy make_serializable_proxy(Args&&... args) {
  auto sp = std::make_shared<T>(std::forward<Args>(args)...);
  return std::make_shared<details::SerializableSharedPtrHolder<T>>(std::move(sp));
}

// Factory registry: maps int32_t kind tags to factories that produce
// fresh SerializableProxy instances. Authored as inline Rust DSL
// (statics on an empty struct; the generic reg<T> delegates to a
// template free fn — T is non-deducible, so it is declared before the
// GEN block). The factory-map singleton + SpinMutex live in the impl
// kernels below unchanged.
using SerializableRegistryFactory = rusty::Function<SerializableProxy()>;

struct SerializableRegistry;
SerializableProxy serializable_registry_create_impl(int32_t kind);
bool serializable_registry_is_registered_impl(int32_t kind);
void serializable_registry_clear_impl();
void serializable_registry_register_factory(int32_t kind,
                                            SerializableRegistryFactory factory);
template<class T> int serializable_registry_reg(int32_t kind);

#if RUSTYCPP_RUST
struct SerializableRegistry {}

impl SerializableRegistry {
    // Register T under `kind` (returns 0 for static-initializer use).
    fn reg<T>(kind: i32) -> i32 {
        serializable_registry_reg::<T>(kind)
    }

    // Create a fresh proxy for the given kind; aborts if unregistered.
    fn create(kind: i32) -> SerializableProxy {
        serializable_registry_create_impl(kind)
    }

    fn is_registered(kind: i32) -> bool {
        serializable_registry_is_registered_impl(kind)
    }

    // Test helper; not thread-safe.
    fn clear_for_testing() {
        serializable_registry_clear_impl()
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=serializable.registry version=1 rust_sha256=0825f31f594471e8211386988da76a6ed2b7efbea76faff6a2ecf0fafc2bf203*/
struct SerializableRegistry;

struct SerializableRegistry {

    template<typename T>
    static int32_t reg(int32_t kind);
    static SerializableProxy create(int32_t kind);
    static bool is_registered(int32_t kind);
    static void clear_for_testing();
};


template<typename T>
int32_t SerializableRegistry::reg(int32_t kind) {
    return serializable_registry_reg<T>(std::move(kind));
}

SerializableProxy SerializableRegistry::create(int32_t kind) {
    return serializable_registry_create_impl(std::move(kind));
}

bool SerializableRegistry::is_registered(int32_t kind) {
    return serializable_registry_is_registered_impl(std::move(kind));
}

void SerializableRegistry::clear_for_testing() {
    serializable_registry_clear_impl();
}
/*RUSTYCPP:GEN-END id=serializable.registry*/

// The no-arg reg<T>() (kind = T::static_kind()) can't live in the DSL
// (a Rust impl can't overload `reg`); it keeps its call-site spelling
// as a template free fn on the class via this shim.
template<class T>
inline int serializable_registry_reg(int32_t kind) {
  serializable_registry_register_factory(kind, []() -> SerializableProxy {
    // Holder-shaped proxy so SerializableEnvelope::load gives
    // unpack_shared<T> a refcount-shared shared_ptr<T>.
    auto sp = std::make_shared<T>();
    return std::make_shared<details::SerializableSharedPtrHolder<T>>(
        std::move(sp));
  });
  return 0;
}

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
// `create_at(pos)` adds compile-time-driven factory
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


}  // export namespace rrr

// ============================================================================
// SerializableRegistry implementation (formerly in serializable.cpp).
// ============================================================================
// @safe - Implementation namespace. The anon-namespace `registry()`
// helper has its own per-function `// @unsafe` (returns a reference
// to a process-wide singleton; rusty-cpp can't express `'static`
// lifetimes). All other functions are lock+map dispatch through the
// SpinMutex<SerializableRegistryMap> guard.
namespace rrr {

namespace {

// `SerializableRegistryMap` — TU-local POD wrapping the single
// `HashMap<i32, Factory>` the SpinMutex guards. Mirrors the shape of
// `AnyMessageRegistryMap` over in any_message.cpp.
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block.
#if RUSTYCPP_RUST
struct SerializableRegistryMap {
    map: rusty::HashMap<i32, SerializableRegistryFactory>,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=serializable.registry_map version=1 rust_sha256=1ec228167958ea8a4dcd5060a23c9ef2cc06ec0d1bcb4dcb9467b3b638869cf4*/
struct SerializableRegistryMap;

struct SerializableRegistryMap {
    rusty::HashMap<int32_t, SerializableRegistryFactory> map;
};
/*RUSTYCPP:GEN-END id=serializable.registry_map*/

// @unsafe - Returns a reference into a process-wide static singleton; the
// caller treats the returned reference as `'static`-lifetime, which rusty-cpp
// doesn't express. Marked @unsafe rather than @safe so the analyzer doesn't
// demand a `@lifetime: () -> &'a where 'a: 'static` annotation it can't yet
// model.
SpinMutex<SerializableRegistryMap>& registry() {
  static SpinMutex<SerializableRegistryMap> r;
  return r;
}

}  // namespace

void serializable_registry_register_factory(int32_t kind, SerializableRegistryFactory factory) {
  auto guard = registry().lock().unwrap();
  (*guard).map.insert(kind, std::move(factory));
}

SerializableProxy serializable_registry_create_impl(int32_t kind) {
  auto guard = registry().lock().unwrap();
  auto entry = (*guard).map.get(kind);
  verify(entry.is_some());
  return entry.unwrap()();
}

bool serializable_registry_is_registered_impl(int32_t kind) {
  auto guard = registry().lock().unwrap();
  return (*guard).map.get(kind).is_some();
}

void serializable_registry_clear_impl() {
  auto guard = registry().lock().unwrap();
  (*guard).map.clear();
}

}  // namespace rrr
