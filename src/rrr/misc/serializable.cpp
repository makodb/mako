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
/*RUSTYCPP:GEN-DISPATCH-BEGIN*/
namespace rusty { namespace detail {
RUSTY_METHOD_DISPATCH(unwrap)
} } // namespace rusty::detail (issue #31 deref_call dispatch)
/*RUSTYCPP:GEN-DISPATCH-END*/

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
class SinkBase;

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
class SourceBase;

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
// Hand-bridge: the SinkBase trait hands write_bytes a raw (ptr, len) pair,
// and the DSL cannot build a span from one. This is the whole kernel that
// remains — the copy itself is extend_from_slice below.
inline std::span<const std::uint8_t> sink_span(const std::uint8_t* p, size_t n) {
    return std::span<const std::uint8_t>(p, n);
}
#if RUSTYCPP_RUST
struct BufferSink {
    bytes: Vec<u8>,
}
impl SinkBase for BufferSink {
    fn write_bytes(&mut self, p: *const u8, n: usize) {
        if n == 0 {
            return;
        }
        // Appends, so extend_from_slice is exactly the old kernel's work:
        // grow if needed, then copy. The hand-rolled capacity doubling is
        // gone with it — Vec already amortises growth.
        self.bytes.extend_from_slice(sink_span(p, n));
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=serializable.buffer_sink version=1 rust_sha256=6239cfd1292829e54a30a003fc52a2bb8884870dee81f1304cec8a317f814a0f*/
struct BufferSink;

struct BufferSink {
    rusty::Vec<uint8_t> bytes;

    void write_bytes(const uint8_t* p, size_t n);
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
};


void BufferSink::write_bytes(const uint8_t* p, size_t n) {
    if (rusty::detail::deref_if_pointer_like(n) == static_cast<size_t>(0)) {
        return;
    }
    this->bytes.extend_from_slice(sink_span(p, std::move(n)));
}

template <>
class SinkBaseAdapter<BufferSink> final : public SinkBase {
    BufferSink value_;
public:
    SinkBaseAdapter(BufferSink v) : value_(std::move(v)) {}
    SinkBaseAdapter(SinkBaseAdapter&& other) : value_(std::move(other.value_)) {}
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
    SourceBaseAdapter(BufferSource v) : value_(std::move(v)) {}
    SourceBaseAdapter(SourceBaseAdapter&& other) : value_(std::move(other.value_)) {}
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
// concurrent access (e.g. via rusty::Mutex around the Sink) if shared.
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
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
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
    SinkBaseAdapter(FdSink v) : value_(std::move(v)) {}
    SinkBaseAdapter(SinkBaseAdapter&& other) : value_(std::move(other.value_)) {}
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
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
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
    SourceBaseAdapter(FdSource v) : value_(std::move(v)) {}
    SourceBaseAdapter(SourceBaseAdapter&& other) : value_(std::move(other.value_)) {}
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

// Varint scratch for the v32/v64 leaf impls: the DSL has no local
// arrays, so the stack buffer lives in a plain C++ POD whose C-array
// field decays to uint8_t* at the DSL call sites.
struct VarintBuf { uint8_t arr[9]; };
inline VarintBuf varint_buf_new() { return VarintBuf{}; }
// @unsafe - pointer offset into the scratch (the tail read lands after
// the already-consumed first byte).
inline uint8_t* varint_tail(VarintBuf* b) { return b->arr + 1; }

// ---- Serde-style Serialize trait (wire migration). --------------------
// Value-side serialization: each type implements how to write itself into
// a BinaryWriteArchive. Lowers to a UFCS free fn
// `Serialize_::serialize(const T&, BinaryWriteArchive&)` (static dispatch by
// overload, no orphan rule, impl-in-own-file). The `operator<<` overloads
// below forward here, so the byte kernel lives in exactly one place and
// byte-compat is automatic during the operator→trait coexistence.
#if RUSTYCPP_RUST
pub trait Serialize {
    fn serialize(&self, ar: &mut BinaryWriteArchive);
}
impl Serialize for v32 {
    fn serialize(&self, ar: &mut BinaryWriteArchive) {
        let mut b = varint_buf_new();
        let bsize = SparseInt::dump32(self.get(), b.arr);
        ar.write_bytes(b.arr, bsize);
    }
}
impl Serialize for v64 {
    fn serialize(&self, ar: &mut BinaryWriteArchive) {
        let mut b = varint_buf_new();
        let bsize = SparseInt::dump64(self.get(), b.arr);
        ar.write_bytes(b.arr, bsize);
    }
}
impl Serialize for i32 {
    fn serialize(&self, ar: &mut BinaryWriteArchive) {
        unsafe {
            let p: *const u8 = (self as *const i32) as *const u8;
            ar.write_bytes(p, std::mem::size_of::<i32>());
        }
    }
}
impl Serialize for i8 {
    fn serialize(&self, ar: &mut BinaryWriteArchive) {
        unsafe {
            let p: *const u8 = (self as *const i8) as *const u8;
            ar.write_bytes(p, std::mem::size_of::<i8>());
        }
    }
}
impl Serialize for i16 {
    fn serialize(&self, ar: &mut BinaryWriteArchive) {
        unsafe {
            let p: *const u8 = (self as *const i16) as *const u8;
            ar.write_bytes(p, std::mem::size_of::<i16>());
        }
    }
}
impl Serialize for i64 {
    fn serialize(&self, ar: &mut BinaryWriteArchive) {
        unsafe {
            let p: *const u8 = (self as *const i64) as *const u8;
            ar.write_bytes(p, std::mem::size_of::<i64>());
        }
    }
}
impl Serialize for u8 {
    fn serialize(&self, ar: &mut BinaryWriteArchive) {
        unsafe {
            let p: *const u8 = (self as *const u8) as *const u8;
            ar.write_bytes(p, std::mem::size_of::<u8>());
        }
    }
}
impl Serialize for u16 {
    fn serialize(&self, ar: &mut BinaryWriteArchive) {
        unsafe {
            let p: *const u8 = (self as *const u16) as *const u8;
            ar.write_bytes(p, std::mem::size_of::<u16>());
        }
    }
}
impl Serialize for u32 {
    fn serialize(&self, ar: &mut BinaryWriteArchive) {
        unsafe {
            let p: *const u8 = (self as *const u32) as *const u8;
            ar.write_bytes(p, std::mem::size_of::<u32>());
        }
    }
}
impl Serialize for u64 {
    fn serialize(&self, ar: &mut BinaryWriteArchive) {
        unsafe {
            let p: *const u8 = (self as *const u64) as *const u8;
            ar.write_bytes(p, std::mem::size_of::<u64>());
        }
    }
}
impl Serialize for f64 {
    fn serialize(&self, ar: &mut BinaryWriteArchive) {
        unsafe {
            let p: *const u8 = (self as *const f64) as *const u8;
            ar.write_bytes(p, std::mem::size_of::<f64>());
        }
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=serializable.serialize_trait version=1 rust_sha256=1c03eccea53c5ff40cee1e490f75758d22ba3957d5cb346765e81d3914dcbc13*/
class Serialize;

// Extension trait free-function forward declarations
namespace rusty_ext {
    void serialize(const v32& self_, BinaryWriteArchive& ar);

    void serialize(const v64& self_, BinaryWriteArchive& ar);

    void serialize(const int32_t& self_, BinaryWriteArchive& ar);

    void serialize(const int8_t& self_, BinaryWriteArchive& ar);

    void serialize(const int16_t& self_, BinaryWriteArchive& ar);

    void serialize(const int64_t& self_, BinaryWriteArchive& ar);

    void serialize(const uint8_t& self_, BinaryWriteArchive& ar);

    void serialize(const uint16_t& self_, BinaryWriteArchive& ar);

    void serialize(const uint32_t& self_, BinaryWriteArchive& ar);

    void serialize(const uint64_t& self_, BinaryWriteArchive& ar);

    void serialize(const double& self_, BinaryWriteArchive& ar);

}


namespace Serialize_ {
    void serialize(const v32& self_, BinaryWriteArchive& ar);
}
using namespace Serialize_;
namespace Serialize_ {
    void serialize(const v64& self_, BinaryWriteArchive& ar);
}
using namespace Serialize_;
namespace Serialize_ {
    void serialize(const int32_t& self_, BinaryWriteArchive& ar);
}
using namespace Serialize_;
namespace Serialize_ {
    void serialize(const int8_t& self_, BinaryWriteArchive& ar);
}
using namespace Serialize_;
namespace Serialize_ {
    void serialize(const int16_t& self_, BinaryWriteArchive& ar);
}
using namespace Serialize_;
namespace Serialize_ {
    void serialize(const int64_t& self_, BinaryWriteArchive& ar);
}
using namespace Serialize_;
namespace Serialize_ {
    void serialize(const uint8_t& self_, BinaryWriteArchive& ar);
}
using namespace Serialize_;
namespace Serialize_ {
    void serialize(const uint16_t& self_, BinaryWriteArchive& ar);
}
using namespace Serialize_;
namespace Serialize_ {
    void serialize(const uint32_t& self_, BinaryWriteArchive& ar);
}
using namespace Serialize_;
namespace Serialize_ {
    void serialize(const uint64_t& self_, BinaryWriteArchive& ar);
}
using namespace Serialize_;
namespace Serialize_ {
    void serialize(const double& self_, BinaryWriteArchive& ar);
}
using namespace Serialize_;
class Serialize {
public:
    virtual ~Serialize() noexcept(false) {}
    virtual void serialize(BinaryWriteArchive& ar) const = 0;
    Serialize(const Serialize&) = delete;
    Serialize& operator=(const Serialize&) = delete;
    Serialize(Serialize&&) = delete;
    Serialize& operator=(Serialize&&) = delete;
protected:
    Serialize() = default;
};

template <class U> class SerializeAdapter;
template <class U> class SerializeAdapterRef;
template <class U> class SerializeAdapterRefMut;

// trait impl for `v32` lowered via the Serialize_ free functions above

// trait impl for `v64` lowered via the Serialize_ free functions above

// trait impl for `i32` lowered via the Serialize_ free functions above

// trait impl for `i8` lowered via the Serialize_ free functions above

// trait impl for `i16` lowered via the Serialize_ free functions above

// trait impl for `i64` lowered via the Serialize_ free functions above

// trait impl for `u8` lowered via the Serialize_ free functions above

// trait impl for `u16` lowered via the Serialize_ free functions above

// trait impl for `u32` lowered via the Serialize_ free functions above

// trait impl for `u64` lowered via the Serialize_ free functions above

// trait impl for `f64` lowered via the Serialize_ free functions above

// Extension trait Serialize lowered to rusty_ext:: free functions
namespace rusty_ext {
    void serialize(const v32& self_, BinaryWriteArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        auto b = varint_buf_new();
        const auto bsize = SparseInt::dump32(self_.get(), std::move([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.arr); }) { return (__r.arr); } else if constexpr (requires { (__r.arr_field); }) { return (__r.arr_field); } else if constexpr (requires { ((*__r).arr); }) { return ((*__r).arr); } else { return ((*__r).arr_field); } }(b)));
        ar.write_bytes(std::move([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.arr); }) { return (__r.arr); } else if constexpr (requires { (__r.arr_field); }) { return (__r.arr_field); } else if constexpr (requires { ((*__r).arr); }) { return ((*__r).arr); } else { return ((*__r).arr_field); } }(b)), std::move(bsize));
    }

    void serialize(const v64& self_, BinaryWriteArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        auto b = varint_buf_new();
        const auto bsize = SparseInt::dump64(self_.get(), std::move([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.arr); }) { return (__r.arr); } else if constexpr (requires { (__r.arr_field); }) { return (__r.arr_field); } else if constexpr (requires { ((*__r).arr); }) { return ((*__r).arr); } else { return ((*__r).arr_field); } }(b)));
        ar.write_bytes(std::move([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.arr); }) { return (__r.arr); } else if constexpr (requires { (__r.arr_field); }) { return (__r.arr_field); } else if constexpr (requires { ((*__r).arr); }) { return ((*__r).arr); } else { return ((*__r).arr_field); } }(b)), std::move(bsize));
    }

    void serialize(const int32_t& self_, BinaryWriteArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        // @unsafe
        {
            const uint8_t* p = reinterpret_cast<const uint8_t*>((static_cast<const int32_t*>(rusty::detail::ptr_or_addr(self_))));
            ar.write_bytes(p, rusty::mem::size_of<int32_t>());
        }
    }

    void serialize(const int8_t& self_, BinaryWriteArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        // @unsafe
        {
            const uint8_t* p = reinterpret_cast<const uint8_t*>((static_cast<const int8_t*>(rusty::detail::ptr_or_addr(self_))));
            ar.write_bytes(p, rusty::mem::size_of<int8_t>());
        }
    }

    void serialize(const int16_t& self_, BinaryWriteArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        // @unsafe
        {
            const uint8_t* p = reinterpret_cast<const uint8_t*>((static_cast<const int16_t*>(rusty::detail::ptr_or_addr(self_))));
            ar.write_bytes(p, rusty::mem::size_of<int16_t>());
        }
    }

    void serialize(const int64_t& self_, BinaryWriteArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        // @unsafe
        {
            const uint8_t* p = reinterpret_cast<const uint8_t*>((static_cast<const int64_t*>(rusty::detail::ptr_or_addr(self_))));
            ar.write_bytes(p, rusty::mem::size_of<int64_t>());
        }
    }

    void serialize(const uint8_t& self_, BinaryWriteArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        // @unsafe
        {
            const uint8_t* p = reinterpret_cast<const uint8_t*>((static_cast<const uint8_t*>(rusty::detail::ptr_or_addr(self_))));
            ar.write_bytes(p, rusty::mem::size_of<uint8_t>());
        }
    }

    void serialize(const uint16_t& self_, BinaryWriteArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        // @unsafe
        {
            const uint8_t* p = reinterpret_cast<const uint8_t*>((static_cast<const uint16_t*>(rusty::detail::ptr_or_addr(self_))));
            ar.write_bytes(p, rusty::mem::size_of<uint16_t>());
        }
    }

    void serialize(const uint32_t& self_, BinaryWriteArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        // @unsafe
        {
            const uint8_t* p = reinterpret_cast<const uint8_t*>((static_cast<const uint32_t*>(rusty::detail::ptr_or_addr(self_))));
            ar.write_bytes(p, rusty::mem::size_of<uint32_t>());
        }
    }

    void serialize(const uint64_t& self_, BinaryWriteArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        // @unsafe
        {
            const uint8_t* p = reinterpret_cast<const uint8_t*>((static_cast<const uint64_t*>(rusty::detail::ptr_or_addr(self_))));
            ar.write_bytes(p, rusty::mem::size_of<uint64_t>());
        }
    }

    void serialize(const double& self_, BinaryWriteArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        // @unsafe
        {
            const uint8_t* p = reinterpret_cast<const uint8_t*>((static_cast<const double*>(rusty::detail::ptr_or_addr(self_))));
            ar.write_bytes(p, rusty::mem::size_of<double>());
        }
    }

}

template <>
class SerializeAdapter<v32> final : public Serialize {
    v32 value_;
public:
    SerializeAdapter(v32 v) : value_(std::move(v)) {}
    SerializeAdapter(SerializeAdapter&& other) : value_(std::move(other.value_)) {}
    void serialize(BinaryWriteArchive& ar) const override {
        rusty_ext::serialize(value_, ar);
    }
};

template <>
class SerializeAdapterRef<v32> final : public Serialize {
    const v32& value_;
public:
    explicit SerializeAdapterRef(const v32& u) : value_(u) {}
    void serialize(BinaryWriteArchive& ar) const override {
        rusty_ext::serialize(value_, ar);
    }
};

template <>
class SerializeAdapterRefMut<v32> final : public Serialize {
    v32& value_;
public:
    explicit SerializeAdapterRefMut(v32& u) : value_(u) {}
    void serialize(BinaryWriteArchive& ar) const override {
        rusty_ext::serialize(value_, ar);
    }
};

template <>
class SerializeAdapter<v64> final : public Serialize {
    v64 value_;
public:
    SerializeAdapter(v64 v) : value_(std::move(v)) {}
    SerializeAdapter(SerializeAdapter&& other) : value_(std::move(other.value_)) {}
    void serialize(BinaryWriteArchive& ar) const override {
        rusty_ext::serialize(value_, ar);
    }
};

template <>
class SerializeAdapterRef<v64> final : public Serialize {
    const v64& value_;
public:
    explicit SerializeAdapterRef(const v64& u) : value_(u) {}
    void serialize(BinaryWriteArchive& ar) const override {
        rusty_ext::serialize(value_, ar);
    }
};

template <>
class SerializeAdapterRefMut<v64> final : public Serialize {
    v64& value_;
public:
    explicit SerializeAdapterRefMut(v64& u) : value_(u) {}
    void serialize(BinaryWriteArchive& ar) const override {
        rusty_ext::serialize(value_, ar);
    }
};

template <>
class SerializeAdapter<int32_t> final : public Serialize {
    int32_t value_;
public:
    SerializeAdapter(int32_t v) : value_(std::move(v)) {}
    SerializeAdapter(SerializeAdapter&& other) : value_(std::move(other.value_)) {}
    void serialize(BinaryWriteArchive& ar) const override {
        rusty_ext::serialize(value_, ar);
    }
};

template <>
class SerializeAdapterRef<int32_t> final : public Serialize {
    const int32_t& value_;
public:
    explicit SerializeAdapterRef(const int32_t& u) : value_(u) {}
    void serialize(BinaryWriteArchive& ar) const override {
        rusty_ext::serialize(value_, ar);
    }
};

template <>
class SerializeAdapterRefMut<int32_t> final : public Serialize {
    int32_t& value_;
public:
    explicit SerializeAdapterRefMut(int32_t& u) : value_(u) {}
    void serialize(BinaryWriteArchive& ar) const override {
        rusty_ext::serialize(value_, ar);
    }
};

template <>
class SerializeAdapter<int8_t> final : public Serialize {
    int8_t value_;
public:
    SerializeAdapter(int8_t v) : value_(std::move(v)) {}
    SerializeAdapter(SerializeAdapter&& other) : value_(std::move(other.value_)) {}
    void serialize(BinaryWriteArchive& ar) const override {
        rusty_ext::serialize(value_, ar);
    }
};

template <>
class SerializeAdapterRef<int8_t> final : public Serialize {
    const int8_t& value_;
public:
    explicit SerializeAdapterRef(const int8_t& u) : value_(u) {}
    void serialize(BinaryWriteArchive& ar) const override {
        rusty_ext::serialize(value_, ar);
    }
};

template <>
class SerializeAdapterRefMut<int8_t> final : public Serialize {
    int8_t& value_;
public:
    explicit SerializeAdapterRefMut(int8_t& u) : value_(u) {}
    void serialize(BinaryWriteArchive& ar) const override {
        rusty_ext::serialize(value_, ar);
    }
};

template <>
class SerializeAdapter<int16_t> final : public Serialize {
    int16_t value_;
public:
    SerializeAdapter(int16_t v) : value_(std::move(v)) {}
    SerializeAdapter(SerializeAdapter&& other) : value_(std::move(other.value_)) {}
    void serialize(BinaryWriteArchive& ar) const override {
        rusty_ext::serialize(value_, ar);
    }
};

template <>
class SerializeAdapterRef<int16_t> final : public Serialize {
    const int16_t& value_;
public:
    explicit SerializeAdapterRef(const int16_t& u) : value_(u) {}
    void serialize(BinaryWriteArchive& ar) const override {
        rusty_ext::serialize(value_, ar);
    }
};

template <>
class SerializeAdapterRefMut<int16_t> final : public Serialize {
    int16_t& value_;
public:
    explicit SerializeAdapterRefMut(int16_t& u) : value_(u) {}
    void serialize(BinaryWriteArchive& ar) const override {
        rusty_ext::serialize(value_, ar);
    }
};

template <>
class SerializeAdapter<int64_t> final : public Serialize {
    int64_t value_;
public:
    SerializeAdapter(int64_t v) : value_(std::move(v)) {}
    SerializeAdapter(SerializeAdapter&& other) : value_(std::move(other.value_)) {}
    void serialize(BinaryWriteArchive& ar) const override {
        rusty_ext::serialize(value_, ar);
    }
};

template <>
class SerializeAdapterRef<int64_t> final : public Serialize {
    const int64_t& value_;
public:
    explicit SerializeAdapterRef(const int64_t& u) : value_(u) {}
    void serialize(BinaryWriteArchive& ar) const override {
        rusty_ext::serialize(value_, ar);
    }
};

template <>
class SerializeAdapterRefMut<int64_t> final : public Serialize {
    int64_t& value_;
public:
    explicit SerializeAdapterRefMut(int64_t& u) : value_(u) {}
    void serialize(BinaryWriteArchive& ar) const override {
        rusty_ext::serialize(value_, ar);
    }
};

template <>
class SerializeAdapter<uint8_t> final : public Serialize {
    uint8_t value_;
public:
    SerializeAdapter(uint8_t v) : value_(std::move(v)) {}
    SerializeAdapter(SerializeAdapter&& other) : value_(std::move(other.value_)) {}
    void serialize(BinaryWriteArchive& ar) const override {
        rusty_ext::serialize(value_, ar);
    }
};

template <>
class SerializeAdapterRef<uint8_t> final : public Serialize {
    const uint8_t& value_;
public:
    explicit SerializeAdapterRef(const uint8_t& u) : value_(u) {}
    void serialize(BinaryWriteArchive& ar) const override {
        rusty_ext::serialize(value_, ar);
    }
};

template <>
class SerializeAdapterRefMut<uint8_t> final : public Serialize {
    uint8_t& value_;
public:
    explicit SerializeAdapterRefMut(uint8_t& u) : value_(u) {}
    void serialize(BinaryWriteArchive& ar) const override {
        rusty_ext::serialize(value_, ar);
    }
};

template <>
class SerializeAdapter<uint16_t> final : public Serialize {
    uint16_t value_;
public:
    SerializeAdapter(uint16_t v) : value_(std::move(v)) {}
    SerializeAdapter(SerializeAdapter&& other) : value_(std::move(other.value_)) {}
    void serialize(BinaryWriteArchive& ar) const override {
        rusty_ext::serialize(value_, ar);
    }
};

template <>
class SerializeAdapterRef<uint16_t> final : public Serialize {
    const uint16_t& value_;
public:
    explicit SerializeAdapterRef(const uint16_t& u) : value_(u) {}
    void serialize(BinaryWriteArchive& ar) const override {
        rusty_ext::serialize(value_, ar);
    }
};

template <>
class SerializeAdapterRefMut<uint16_t> final : public Serialize {
    uint16_t& value_;
public:
    explicit SerializeAdapterRefMut(uint16_t& u) : value_(u) {}
    void serialize(BinaryWriteArchive& ar) const override {
        rusty_ext::serialize(value_, ar);
    }
};

template <>
class SerializeAdapter<uint32_t> final : public Serialize {
    uint32_t value_;
public:
    SerializeAdapter(uint32_t v) : value_(std::move(v)) {}
    SerializeAdapter(SerializeAdapter&& other) : value_(std::move(other.value_)) {}
    void serialize(BinaryWriteArchive& ar) const override {
        rusty_ext::serialize(value_, ar);
    }
};

template <>
class SerializeAdapterRef<uint32_t> final : public Serialize {
    const uint32_t& value_;
public:
    explicit SerializeAdapterRef(const uint32_t& u) : value_(u) {}
    void serialize(BinaryWriteArchive& ar) const override {
        rusty_ext::serialize(value_, ar);
    }
};

template <>
class SerializeAdapterRefMut<uint32_t> final : public Serialize {
    uint32_t& value_;
public:
    explicit SerializeAdapterRefMut(uint32_t& u) : value_(u) {}
    void serialize(BinaryWriteArchive& ar) const override {
        rusty_ext::serialize(value_, ar);
    }
};

template <>
class SerializeAdapter<uint64_t> final : public Serialize {
    uint64_t value_;
public:
    SerializeAdapter(uint64_t v) : value_(std::move(v)) {}
    SerializeAdapter(SerializeAdapter&& other) : value_(std::move(other.value_)) {}
    void serialize(BinaryWriteArchive& ar) const override {
        rusty_ext::serialize(value_, ar);
    }
};

template <>
class SerializeAdapterRef<uint64_t> final : public Serialize {
    const uint64_t& value_;
public:
    explicit SerializeAdapterRef(const uint64_t& u) : value_(u) {}
    void serialize(BinaryWriteArchive& ar) const override {
        rusty_ext::serialize(value_, ar);
    }
};

template <>
class SerializeAdapterRefMut<uint64_t> final : public Serialize {
    uint64_t& value_;
public:
    explicit SerializeAdapterRefMut(uint64_t& u) : value_(u) {}
    void serialize(BinaryWriteArchive& ar) const override {
        rusty_ext::serialize(value_, ar);
    }
};

template <>
class SerializeAdapter<double> final : public Serialize {
    double value_;
public:
    SerializeAdapter(double v) : value_(std::move(v)) {}
    SerializeAdapter(SerializeAdapter&& other) : value_(std::move(other.value_)) {}
    void serialize(BinaryWriteArchive& ar) const override {
        rusty_ext::serialize(value_, ar);
    }
};

template <>
class SerializeAdapterRef<double> final : public Serialize {
    const double& value_;
public:
    explicit SerializeAdapterRef(const double& u) : value_(u) {}
    void serialize(BinaryWriteArchive& ar) const override {
        rusty_ext::serialize(value_, ar);
    }
};

template <>
class SerializeAdapterRefMut<double> final : public Serialize {
    double& value_;
public:
    explicit SerializeAdapterRefMut(double& u) : value_(u) {}
    void serialize(BinaryWriteArchive& ar) const override {
        rusty_ext::serialize(value_, ar);
    }
};


// UFCS trait migration: free functions for `impl Serialize for ...`
namespace Serialize_ {
    void serialize(const v32& self_, BinaryWriteArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        auto b = varint_buf_new();
        const auto bsize = SparseInt::dump32(self_.get(), std::move([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.arr); }) { return (__r.arr); } else if constexpr (requires { (__r.arr_field); }) { return (__r.arr_field); } else if constexpr (requires { ((*__r).arr); }) { return ((*__r).arr); } else { return ((*__r).arr_field); } }(b)));
        ar.write_bytes(std::move([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.arr); }) { return (__r.arr); } else if constexpr (requires { (__r.arr_field); }) { return (__r.arr_field); } else if constexpr (requires { ((*__r).arr); }) { return ((*__r).arr); } else { return ((*__r).arr_field); } }(b)), std::move(bsize));
    }

}
// UFCS trait migration: free functions for `impl Serialize for ...`
namespace Serialize_ {
    void serialize(const v64& self_, BinaryWriteArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        auto b = varint_buf_new();
        const auto bsize = SparseInt::dump64(self_.get(), std::move([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.arr); }) { return (__r.arr); } else if constexpr (requires { (__r.arr_field); }) { return (__r.arr_field); } else if constexpr (requires { ((*__r).arr); }) { return ((*__r).arr); } else { return ((*__r).arr_field); } }(b)));
        ar.write_bytes(std::move([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.arr); }) { return (__r.arr); } else if constexpr (requires { (__r.arr_field); }) { return (__r.arr_field); } else if constexpr (requires { ((*__r).arr); }) { return ((*__r).arr); } else { return ((*__r).arr_field); } }(b)), std::move(bsize));
    }

}
// UFCS trait migration: free functions for `impl Serialize for ...`
namespace Serialize_ {
    void serialize(const int32_t& self_, BinaryWriteArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        // @unsafe
        {
            const uint8_t* p = reinterpret_cast<const uint8_t*>((static_cast<const int32_t*>(rusty::detail::ptr_or_addr(self_))));
            ar.write_bytes(p, rusty::mem::size_of<int32_t>());
        }
    }

}
// UFCS trait migration: free functions for `impl Serialize for ...`
namespace Serialize_ {
    void serialize(const int8_t& self_, BinaryWriteArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        // @unsafe
        {
            const uint8_t* p = reinterpret_cast<const uint8_t*>((static_cast<const int8_t*>(rusty::detail::ptr_or_addr(self_))));
            ar.write_bytes(p, rusty::mem::size_of<int8_t>());
        }
    }

}
// UFCS trait migration: free functions for `impl Serialize for ...`
namespace Serialize_ {
    void serialize(const int16_t& self_, BinaryWriteArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        // @unsafe
        {
            const uint8_t* p = reinterpret_cast<const uint8_t*>((static_cast<const int16_t*>(rusty::detail::ptr_or_addr(self_))));
            ar.write_bytes(p, rusty::mem::size_of<int16_t>());
        }
    }

}
// UFCS trait migration: free functions for `impl Serialize for ...`
namespace Serialize_ {
    void serialize(const int64_t& self_, BinaryWriteArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        // @unsafe
        {
            const uint8_t* p = reinterpret_cast<const uint8_t*>((static_cast<const int64_t*>(rusty::detail::ptr_or_addr(self_))));
            ar.write_bytes(p, rusty::mem::size_of<int64_t>());
        }
    }

}
// UFCS trait migration: free functions for `impl Serialize for ...`
namespace Serialize_ {
    void serialize(const uint8_t& self_, BinaryWriteArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        // @unsafe
        {
            const uint8_t* p = reinterpret_cast<const uint8_t*>((static_cast<const uint8_t*>(rusty::detail::ptr_or_addr(self_))));
            ar.write_bytes(p, rusty::mem::size_of<uint8_t>());
        }
    }

}
// UFCS trait migration: free functions for `impl Serialize for ...`
namespace Serialize_ {
    void serialize(const uint16_t& self_, BinaryWriteArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        // @unsafe
        {
            const uint8_t* p = reinterpret_cast<const uint8_t*>((static_cast<const uint16_t*>(rusty::detail::ptr_or_addr(self_))));
            ar.write_bytes(p, rusty::mem::size_of<uint16_t>());
        }
    }

}
// UFCS trait migration: free functions for `impl Serialize for ...`
namespace Serialize_ {
    void serialize(const uint32_t& self_, BinaryWriteArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        // @unsafe
        {
            const uint8_t* p = reinterpret_cast<const uint8_t*>((static_cast<const uint32_t*>(rusty::detail::ptr_or_addr(self_))));
            ar.write_bytes(p, rusty::mem::size_of<uint32_t>());
        }
    }

}
// UFCS trait migration: free functions for `impl Serialize for ...`
namespace Serialize_ {
    void serialize(const uint64_t& self_, BinaryWriteArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        // @unsafe
        {
            const uint8_t* p = reinterpret_cast<const uint8_t*>((static_cast<const uint64_t*>(rusty::detail::ptr_or_addr(self_))));
            ar.write_bytes(p, rusty::mem::size_of<uint64_t>());
        }
    }

}
// UFCS trait migration: free functions for `impl Serialize for ...`
namespace Serialize_ {
    void serialize(const double& self_, BinaryWriteArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        // @unsafe
        {
            const uint8_t* p = reinterpret_cast<const uint8_t*>((static_cast<const double*>(rusty::detail::ptr_or_addr(self_))));
            ar.write_bytes(p, rusty::mem::size_of<double>());
        }
    }

}
/*RUSTYCPP:GEN-END id=serializable.serialize_trait*/

// ---- Fixed-width primitives. ------------------------------------------
// Each scalar's byte representation is `reinterpret_cast<const
// uint8_t*>(&v)`; the trait's `const uint8_t*` parameter type
// makes the byte view explicit (the previous `const void*` form
// hid it behind implicit conversion).

// ---- Serde-trait leaf kernels: varints + strings. Hand-written byte
// kernels (like the containers below); encode goes through the DSL
// SparseInt::dump statics (u8 buffers).
namespace Serialize_ {
inline void serialize(std::string_view self_, BinaryWriteArchive& ar) {
  rrr::v64 v_len{static_cast<rrr::i64>(self_.size())};
  serialize(v_len, ar);
  if (self_.size() > 0) {
    ar.write_bytes(reinterpret_cast<const uint8_t*>(self_.data()), self_.size());
  }
}
inline void serialize(const std::string& self_, BinaryWriteArchive& ar) {
  serialize(std::string_view{self_}, ar);
}
// Generic bridge: any type is trait-serializable. A migrated type resolves to
// its specific (more-specialized) overload above; anything else falls through
// to its operator<< here. This is what lets `serialize(field, ar)` work for
// EVERY field type (containers, polymorphic messages, un-migrated user structs)
// during the operator->trait coexistence — the enabler for the generator flip.
// (At Phase 8, when operators are deleted, every type has a specific overload,
// so this bridge is dropped.)
// Phase 8 endgame: the generic bridge dispatches via ADL instead of the
// operator layer. The deleted decoy in adl_detail_ poisons unqualified
// lookup so the dispatcher can ONLY resolve through ADL (the type's own
// namespace) — the catch-all cannot self-select, and a type with neither a
// specific overload above nor an ADL serialize() fails with a hard
// "deleted function" diagnostic naming the type.
namespace adl_detail_ {
void serialize() = delete;  // lookup poison: stops ascent past this scope
template<typename T>
inline void dispatch_serialize(const T& v, BinaryWriteArchive& ar) {
  serialize(v, ar);  // ADL-only by construction
}
}  // namespace adl_detail_
template<typename T>
inline void serialize(const T& v, BinaryWriteArchive& ar) {
  adl_detail_::dispatch_serialize(v, ar);
}
}  // namespace Serialize_

// ---- Variable-length integer encoding (SparseInt). --------------------


// ---- Variable-length byte sequences. ----------------------------------

// std::string is a convenience overload — same wire format as
// string_view (length-prefixed bytes).

// ---- Composites. ------------------------------------------------------
// std::pair: write first then second, no length prefix (each side
// already knows the type and consumes its own bytes).
// Phase 8: container/pair serde overloads (operator bodies moved here;
// the operators are now one-line forwarders). Forward declarations first
// so nested containers resolve regardless of definition order; element
// calls are unqualified and fall back to the generic catch-all.
namespace Serialize_ {
template<class T1, class T2> inline void serialize(const std::pair<T1, T2>& v, BinaryWriteArchive& ar);
template<class T> inline void serialize(const rusty::Vec<T>& v, BinaryWriteArchive& ar);
template<class T> inline void serialize(const std::vector<T>& v, BinaryWriteArchive& ar);
template<class T> inline void serialize(const rusty::BTreeSet<T>& v, BinaryWriteArchive& ar);
template<class T> inline void serialize(const std::set<T>& v, BinaryWriteArchive& ar);
template<class T> inline void serialize(const rusty::HashSet<T>& v, BinaryWriteArchive& ar);
template<class T> inline void serialize(const std::unordered_set<T>& v, BinaryWriteArchive& ar);
template<class K, class V> inline void serialize(const rusty::BTreeMap<K, V>& v, BinaryWriteArchive& ar);
template<class K, class V> inline void serialize(const std::map<K, V>& v, BinaryWriteArchive& ar);
template<class K, class V> inline void serialize(const rusty::HashMap<K, V>& v, BinaryWriteArchive& ar);
template<class K, class V> inline void serialize(const std::unordered_map<K, V>& v, BinaryWriteArchive& ar);

template<class T1, class T2>
inline void serialize(const std::pair<T1, T2>& v, BinaryWriteArchive& ar) {
  serialize(v.first, ar);
  serialize(v.second, ar);
}

// Bodies moved to the WireSerialize DSL block below (impl<T>-for-
// container trait lowering, same as std::list); these 1-line
// forwarders keep the overload set in Serialize_ where qualified
// callers and the nested-container decls resolve. The namespace is
// opened empty here so the qualified dependent calls parse; the GEN
// below populates it, and the calls resolve at instantiation.
}  // namespace Serialize_ (paused for the fwd namespace decls)
// Forward declarations of the DSL-generated overloads: a qualified
// callee must resolve at template-definition context, so the names
// must exist here even though the GEN definitions come later.
namespace WireSerialize_ {
template<class T> void serialize(const std::list<T>& self_, BinaryWriteArchive& ar);
template<class T> void serialize(const rusty::Vec<T>& self_, BinaryWriteArchive& ar);
template<class T> void serialize(const std::vector<T>& self_, BinaryWriteArchive& ar);
template<class T> void serialize(const std::set<T>& self_, BinaryWriteArchive& ar);
template<class T> void serialize(const std::unordered_set<T>& self_, BinaryWriteArchive& ar);
template<class K, class V> void serialize(const std::map<K, V>& self_, BinaryWriteArchive& ar);
template<class K, class V> void serialize(const std::unordered_map<K, V>& self_, BinaryWriteArchive& ar);
template<class T> void serialize(const rusty::BTreeSet<T>& self_, BinaryWriteArchive& ar);
template<class K, class V> void serialize(const rusty::BTreeMap<K, V>& self_, BinaryWriteArchive& ar);
}
namespace Serialize_ {
template<class T>
inline void serialize(const rusty::Vec<T>& v, BinaryWriteArchive& ar) {
  WireSerialize_::serialize(v, ar);
}

template<class T>
inline void serialize(const std::vector<T>& v, BinaryWriteArchive& ar) {
  WireSerialize_::serialize(v, ar);
}

// First slice of the serde overload family converted to DSL (playbook
// §7.40): `impl Trait for X`, one impl per type, lowers to overloaded
// FREE functions in a nested namespace plus a `using namespace`.
//
// TWO placement constraints, both learned by compiling:
//  * the block must sit at `rrr` scope, NOT inside `Serialize_`. A trait
//    emits `namespace rusty_ext`, and this file already has `rrr::rusty_ext`
//    from the Deserialize trait; a second one at `rrr::Serialize_::rusty_ext`
//    is hoisted by `using namespace Serialize_` and every unqualified
//    `rusty_ext::` call becomes ambiguous.
//  * internal calls must be QUALIFIED (`Serialize_::serialize`). Unqualified
//    lookup inside the generated namespace finds the sibling it just emitted
//    and STOPS, hiding the rest of the overload set — the exact hazard this
//    file's `adl_detail_` machinery exists to defeat. Qualifying routes
//    through the catch-all, which still does ADL for user types.
}  // namespace Serialize_ (reopened after the DSL block below)

#if RUSTYCPP_RUST
pub trait WireSerialize {
    fn serialize(&self, ar: &mut BinaryWriteArchive);
}

impl<T> WireSerialize for std::list<T> {
    fn serialize(&self, ar: &mut BinaryWriteArchive) {
        let v_len: rrr::v64 = rrr::v64::new(self.size() as i64);
        Serialize_::serialize(v_len, ar);
        for e in self {
            Serialize_::serialize(e, ar);
        }
    }
}

// Index loops, not `for e in self`: rusty::iter over these vector
// shapes in this position mis-yields (the element call deduced T = the
// whole container and landed on the poisoned catch-all). Bodies live
// here; 1-line forwarders in Serialize_ keep the overload set where
// qualified callers look (the placement-arcana-free route).
impl<T> WireSerialize for Vec<T> {
    fn serialize(&self, ar: &mut BinaryWriteArchive) {
        let v_len: rrr::v64 = rrr::v64::new(self.len() as i64);
        Serialize_::serialize(v_len, ar);
        let mut i: usize = 0usize;
        while i < self.len() {
            Serialize_::serialize(self[i], ar);
            i += 1usize;
        }
    }
}

impl<T> WireSerialize for std::vector<T> {
    fn serialize(&self, ar: &mut BinaryWriteArchive) {
        let v_len: rrr::v64 = rrr::v64::new(self.size() as i64);
        Serialize_::serialize(v_len, ar);
        let mut i: usize = 0usize;
        while i < self.size() {
            Serialize_::serialize(self[i], ar);
            i += 1usize;
        }
    }
}

impl<T> WireSerialize for std::set<T> {
    fn serialize(&self, ar: &mut BinaryWriteArchive) {
        let v_len: rrr::v64 = rrr::v64::new(self.size() as i64);
        Serialize_::serialize(v_len, ar);
        for e in self {
            Serialize_::serialize(e, ar);
        }
    }
}

impl<T> WireSerialize for std::unordered_set<T> {
    fn serialize(&self, ar: &mut BinaryWriteArchive) {
        let v_len: rrr::v64 = rrr::v64::new(self.size() as i64);
        Serialize_::serialize(v_len, ar);
        for e in self {
            Serialize_::serialize(e, ar);
        }
    }
}

impl<K, V> WireSerialize for std::map<K, V> {
    fn serialize(&self, ar: &mut BinaryWriteArchive) {
        let v_len: rrr::v64 = rrr::v64::new(self.size() as i64);
        Serialize_::serialize(v_len, ar);
        for kv in self {
            Serialize_::serialize(kv.first, ar);
            Serialize_::serialize(kv.second, ar);
        }
    }
}

impl<K, V> WireSerialize for std::unordered_map<K, V> {
    fn serialize(&self, ar: &mut BinaryWriteArchive) {
        let v_len: rrr::v64 = rrr::v64::new(self.size() as i64);
        Serialize_::serialize(v_len, ar);
        for kv in self {
            Serialize_::serialize(kv.first, ar);
            Serialize_::serialize(kv.second, ar);
        }
    }
}

// rusty B-tree containers iterate Rust-style (no begin()/end()); the
// explicit iterator loop is the same shape their old C++ bodies used.
impl<T> WireSerialize for rusty::BTreeSet<T> {
    fn serialize(&self, ar: &mut BinaryWriteArchive) {
        let v_len: rrr::v64 = rrr::v64::new(self.len() as i64);
        Serialize_::serialize(v_len, ar);
        let mut it = self.iter();
        loop {
            let e = it.next();
            if e.is_none() {
                break;
            }
            Serialize_::serialize(e.unwrap(), ar);
        }
    }
}

impl<K, V> WireSerialize for rusty::BTreeMap<K, V> {
    fn serialize(&self, ar: &mut BinaryWriteArchive) {
        let v_len: rrr::v64 = rrr::v64::new(self.len() as i64);
        Serialize_::serialize(v_len, ar);
        let mut it = self.iter();
        loop {
            let e = it.next();
            if e.is_none() {
                break;
            }
            let kv = e.unwrap();
            Serialize_::serialize(kv.0, ar);
            Serialize_::serialize(kv.1, ar);
        }
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=serializable.wire_ser version=1 rust_sha256=d2f4076328f9ebc2e2a9a0bb4f8e1c062a07ac0c2a3b6905675bdda5fb0bca95*/
class WireSerialize;

// Extension trait free-function forward declarations
namespace rusty_ext {
    template<typename T>
    void serialize(const std::list<T>& self_, BinaryWriteArchive& ar);

    template<typename T>
    void serialize(const rusty::Vec<T>& self_, BinaryWriteArchive& ar);

    template<typename T>
    void serialize(const std::vector<T>& self_, BinaryWriteArchive& ar);

    template<typename T>
    void serialize(const std::set<T>& self_, BinaryWriteArchive& ar);

    template<typename T>
    void serialize(const std::unordered_set<T>& self_, BinaryWriteArchive& ar);

    template<typename K, typename V>
    void serialize(const std::map<K, V>& self_, BinaryWriteArchive& ar);

    template<typename K, typename V>
    void serialize(const std::unordered_map<K, V>& self_, BinaryWriteArchive& ar);

    template<typename T>
    void serialize(const rusty::BTreeSet<T>& self_, BinaryWriteArchive& ar);

    template<typename K, typename V>
    void serialize(const rusty::BTreeMap<K, V>& self_, BinaryWriteArchive& ar);

}


namespace WireSerialize_ {
    template<typename T>
    void serialize(const std::list<T>& self_, BinaryWriteArchive& ar);
}
using namespace WireSerialize_;
namespace WireSerialize_ {
    template<typename T>
    void serialize(const rusty::Vec<T>& self_, BinaryWriteArchive& ar);
}
using namespace WireSerialize_;
namespace WireSerialize_ {
    template<typename T>
    void serialize(const std::vector<T>& self_, BinaryWriteArchive& ar);
}
using namespace WireSerialize_;
namespace WireSerialize_ {
    template<typename T>
    void serialize(const std::set<T>& self_, BinaryWriteArchive& ar);
}
using namespace WireSerialize_;
namespace WireSerialize_ {
    template<typename T>
    void serialize(const std::unordered_set<T>& self_, BinaryWriteArchive& ar);
}
using namespace WireSerialize_;
namespace WireSerialize_ {
    template<typename K, typename V>
    void serialize(const std::map<K, V>& self_, BinaryWriteArchive& ar);
}
using namespace WireSerialize_;
namespace WireSerialize_ {
    template<typename K, typename V>
    void serialize(const std::unordered_map<K, V>& self_, BinaryWriteArchive& ar);
}
using namespace WireSerialize_;
namespace WireSerialize_ {
    template<typename T>
    void serialize(const rusty::BTreeSet<T>& self_, BinaryWriteArchive& ar);
}
using namespace WireSerialize_;
namespace WireSerialize_ {
    template<typename K, typename V>
    void serialize(const rusty::BTreeMap<K, V>& self_, BinaryWriteArchive& ar);
}
using namespace WireSerialize_;
class WireSerialize {
public:
    virtual ~WireSerialize() noexcept(false) {}
    virtual void serialize(BinaryWriteArchive& ar) const = 0;
    WireSerialize(const WireSerialize&) = delete;
    WireSerialize& operator=(const WireSerialize&) = delete;
    WireSerialize(WireSerialize&&) = delete;
    WireSerialize& operator=(WireSerialize&&) = delete;
protected:
    WireSerialize() = default;
};

template <class U> class WireSerializeAdapter;
template <class U> class WireSerializeAdapterRef;
template <class U> class WireSerializeAdapterRefMut;

// trait impl for `std::list` lowered via the WireSerialize_ free functions above

// trait impl for `Vec` lowered via the WireSerialize_ free functions above

// trait impl for `std::vector` lowered via the WireSerialize_ free functions above

// trait impl for `std::set` lowered via the WireSerialize_ free functions above

// trait impl for `std::unordered_set` lowered via the WireSerialize_ free functions above

// trait impl for `std::map` lowered via the WireSerialize_ free functions above

// trait impl for `std::unordered_map` lowered via the WireSerialize_ free functions above

// trait impl for `rusty::BTreeSet` lowered via the WireSerialize_ free functions above

// trait impl for `rusty::BTreeMap` lowered via the WireSerialize_ free functions above

// Extension trait WireSerialize lowered to rusty_ext:: free functions
namespace rusty_ext {
    template<typename T>
    void serialize(const std::list<T>& self_, BinaryWriteArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        rrr::v64 v_len = rrr::v64::new_(static_cast<int64_t>(self_.size()));
        Serialize_::serialize(v_len, ar);
        for (auto&& e : rusty::for_in(rusty::iter(self_))) {
            Serialize_::serialize(e, ar);
        }
    }

    template<typename T>
    void serialize(const rusty::Vec<T>& self_, BinaryWriteArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        rrr::v64 v_len = rrr::v64::new_(static_cast<int64_t>(rusty::len(self_)));
        Serialize_::serialize(v_len, ar);
        size_t i = static_cast<size_t>(0);
        while (rusty::detail::deref_if_pointer_like(i) < rusty::len(self_)) {
            Serialize_::serialize(self_[i], ar);
            i += static_cast<size_t>(1);
        }
    }

    template<typename T>
    void serialize(const std::vector<T>& self_, BinaryWriteArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        rrr::v64 v_len = rrr::v64::new_(static_cast<int64_t>(self_.size()));
        Serialize_::serialize(v_len, ar);
        size_t i = static_cast<size_t>(0);
        while (rusty::detail::deref_if_pointer_like(i) < self_.size()) {
            Serialize_::serialize(self_[i], ar);
            i += static_cast<size_t>(1);
        }
    }

    template<typename T>
    void serialize(const std::set<T>& self_, BinaryWriteArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        rrr::v64 v_len = rrr::v64::new_(static_cast<int64_t>(self_.size()));
        Serialize_::serialize(v_len, ar);
        for (auto&& e : rusty::for_in(rusty::iter(self_))) {
            Serialize_::serialize(e, ar);
        }
    }

    template<typename T>
    void serialize(const std::unordered_set<T>& self_, BinaryWriteArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        rrr::v64 v_len = rrr::v64::new_(static_cast<int64_t>(self_.size()));
        Serialize_::serialize(v_len, ar);
        for (auto&& e : rusty::for_in(rusty::iter(self_))) {
            Serialize_::serialize(e, ar);
        }
    }

    template<typename K, typename V>
    void serialize(const std::map<K, V>& self_, BinaryWriteArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        rrr::v64 v_len = rrr::v64::new_(static_cast<int64_t>(self_.size()));
        Serialize_::serialize(v_len, ar);
        for (auto&& kv : rusty::for_in(rusty::iter(self_))) {
            Serialize_::serialize([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.first); }) { return (__r.first); } else if constexpr (requires { (__r.first_field); }) { return (__r.first_field); } else if constexpr (requires { ((*__r).first); }) { return ((*__r).first); } else { return ((*__r).first_field); } }(kv), ar);
            Serialize_::serialize([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.second); }) { return (__r.second); } else if constexpr (requires { (__r.second_field); }) { return (__r.second_field); } else if constexpr (requires { ((*__r).second); }) { return ((*__r).second); } else { return ((*__r).second_field); } }(kv), ar);
        }
    }

    template<typename K, typename V>
    void serialize(const std::unordered_map<K, V>& self_, BinaryWriteArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        rrr::v64 v_len = rrr::v64::new_(static_cast<int64_t>(self_.size()));
        Serialize_::serialize(v_len, ar);
        for (auto&& kv : rusty::for_in(rusty::iter(self_))) {
            Serialize_::serialize([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.first); }) { return (__r.first); } else if constexpr (requires { (__r.first_field); }) { return (__r.first_field); } else if constexpr (requires { ((*__r).first); }) { return ((*__r).first); } else { return ((*__r).first_field); } }(kv), ar);
            Serialize_::serialize([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.second); }) { return (__r.second); } else if constexpr (requires { (__r.second_field); }) { return (__r.second_field); } else if constexpr (requires { ((*__r).second); }) { return ((*__r).second); } else { return ((*__r).second_field); } }(kv), ar);
        }
    }

    template<typename T>
    void serialize(const rusty::BTreeSet<T>& self_, BinaryWriteArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        rrr::v64 v_len = rrr::v64::new_(static_cast<int64_t>(rusty::len(self_)));
        Serialize_::serialize(v_len, ar);
        auto it = rusty::iter(self_);
        while (true) {
            auto e = it.next();
            if (e.is_none()) {
                break;
            }
            Serialize_::serialize(e.unwrap(), ar);
        }
    }

    template<typename K, typename V>
    void serialize(const rusty::BTreeMap<K, V>& self_, BinaryWriteArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        rrr::v64 v_len = rrr::v64::new_(static_cast<int64_t>(rusty::len(self_)));
        Serialize_::serialize(v_len, ar);
        auto it = rusty::iter(self_);
        while (true) {
            auto e = it.next();
            if (e.is_none()) {
                break;
            }
            const auto kv = e.unwrap();
            Serialize_::serialize(rusty::detail::deref_if_pointer(([](auto&& __t) -> decltype(auto) { if constexpr (requires { __t._0; }) return (std::forward<decltype(__t)>(__t)._0); else if constexpr (requires { std::get<0>(std::forward<decltype(__t)>(__t)); }) return std::get<0>(std::forward<decltype(__t)>(__t)); else if constexpr (requires { (*__t)._0; }) return ((*std::forward<decltype(__t)>(__t))._0); else return std::get<0>(*std::forward<decltype(__t)>(__t)); })(kv)), ar);
            Serialize_::serialize(rusty::detail::deref_if_pointer(([](auto&& __t) -> decltype(auto) { if constexpr (requires { __t._1; }) return (std::forward<decltype(__t)>(__t)._1); else if constexpr (requires { std::get<1>(std::forward<decltype(__t)>(__t)); }) return std::get<1>(std::forward<decltype(__t)>(__t)); else if constexpr (requires { (*__t)._1; }) return ((*std::forward<decltype(__t)>(__t))._1); else return std::get<1>(*std::forward<decltype(__t)>(__t)); })(kv)), ar);
        }
    }

}

// TODO(interface_traits): skipped generic impl `WireSerializeAdapter<std::list<T>>`
// TODO(interface_traits): skipped generic impl `WireSerializeAdapter<rusty::Vec<T>>`
// TODO(interface_traits): skipped generic impl `WireSerializeAdapter<std::vector<T>>`
// TODO(interface_traits): skipped generic impl `WireSerializeAdapter<std::set<T>>`
// TODO(interface_traits): skipped generic impl `WireSerializeAdapter<std::unordered_set<T>>`
// TODO(interface_traits): skipped generic impl `WireSerializeAdapter<std::map<K, V>>`
// TODO(interface_traits): skipped generic impl `WireSerializeAdapter<std::unordered_map<K, V>>`
// TODO(interface_traits): skipped generic impl `WireSerializeAdapter<rusty::BTreeSet<T>>`
// TODO(interface_traits): skipped generic impl `WireSerializeAdapter<rusty::BTreeMap<K, V>>`

// UFCS trait migration: free functions for `impl WireSerialize for ...`
namespace WireSerialize_ {
    template<typename T>
    void serialize(const std::list<T>& self_, BinaryWriteArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        rrr::v64 v_len = rrr::v64::new_(static_cast<int64_t>(self_.size()));
        Serialize_::serialize(v_len, ar);
        for (auto&& e : rusty::for_in(rusty::iter(self_))) {
            Serialize_::serialize(e, ar);
        }
    }

}
// UFCS trait migration: free functions for `impl WireSerialize for ...`
namespace WireSerialize_ {
    template<typename T>
    void serialize(const rusty::Vec<T>& self_, BinaryWriteArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        rrr::v64 v_len = rrr::v64::new_(static_cast<int64_t>(rusty::len(self_)));
        Serialize_::serialize(v_len, ar);
        size_t i = static_cast<size_t>(0);
        while (rusty::detail::deref_if_pointer_like(i) < rusty::len(self_)) {
            Serialize_::serialize(self_[i], ar);
            i += static_cast<size_t>(1);
        }
    }

}
// UFCS trait migration: free functions for `impl WireSerialize for ...`
namespace WireSerialize_ {
    template<typename T>
    void serialize(const std::vector<T>& self_, BinaryWriteArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        rrr::v64 v_len = rrr::v64::new_(static_cast<int64_t>(self_.size()));
        Serialize_::serialize(v_len, ar);
        size_t i = static_cast<size_t>(0);
        while (rusty::detail::deref_if_pointer_like(i) < self_.size()) {
            Serialize_::serialize(self_[i], ar);
            i += static_cast<size_t>(1);
        }
    }

}
// UFCS trait migration: free functions for `impl WireSerialize for ...`
namespace WireSerialize_ {
    template<typename T>
    void serialize(const std::set<T>& self_, BinaryWriteArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        rrr::v64 v_len = rrr::v64::new_(static_cast<int64_t>(self_.size()));
        Serialize_::serialize(v_len, ar);
        for (auto&& e : rusty::for_in(rusty::iter(self_))) {
            Serialize_::serialize(e, ar);
        }
    }

}
// UFCS trait migration: free functions for `impl WireSerialize for ...`
namespace WireSerialize_ {
    template<typename T>
    void serialize(const std::unordered_set<T>& self_, BinaryWriteArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        rrr::v64 v_len = rrr::v64::new_(static_cast<int64_t>(self_.size()));
        Serialize_::serialize(v_len, ar);
        for (auto&& e : rusty::for_in(rusty::iter(self_))) {
            Serialize_::serialize(e, ar);
        }
    }

}
// UFCS trait migration: free functions for `impl WireSerialize for ...`
namespace WireSerialize_ {
    template<typename K, typename V>
    void serialize(const std::map<K, V>& self_, BinaryWriteArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        rrr::v64 v_len = rrr::v64::new_(static_cast<int64_t>(self_.size()));
        Serialize_::serialize(v_len, ar);
        for (auto&& kv : rusty::for_in(rusty::iter(self_))) {
            Serialize_::serialize([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.first); }) { return (__r.first); } else if constexpr (requires { (__r.first_field); }) { return (__r.first_field); } else if constexpr (requires { ((*__r).first); }) { return ((*__r).first); } else { return ((*__r).first_field); } }(kv), ar);
            Serialize_::serialize([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.second); }) { return (__r.second); } else if constexpr (requires { (__r.second_field); }) { return (__r.second_field); } else if constexpr (requires { ((*__r).second); }) { return ((*__r).second); } else { return ((*__r).second_field); } }(kv), ar);
        }
    }

}
// UFCS trait migration: free functions for `impl WireSerialize for ...`
namespace WireSerialize_ {
    template<typename K, typename V>
    void serialize(const std::unordered_map<K, V>& self_, BinaryWriteArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        rrr::v64 v_len = rrr::v64::new_(static_cast<int64_t>(self_.size()));
        Serialize_::serialize(v_len, ar);
        for (auto&& kv : rusty::for_in(rusty::iter(self_))) {
            Serialize_::serialize([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.first); }) { return (__r.first); } else if constexpr (requires { (__r.first_field); }) { return (__r.first_field); } else if constexpr (requires { ((*__r).first); }) { return ((*__r).first); } else { return ((*__r).first_field); } }(kv), ar);
            Serialize_::serialize([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.second); }) { return (__r.second); } else if constexpr (requires { (__r.second_field); }) { return (__r.second_field); } else if constexpr (requires { ((*__r).second); }) { return ((*__r).second); } else { return ((*__r).second_field); } }(kv), ar);
        }
    }

}
// UFCS trait migration: free functions for `impl WireSerialize for ...`
namespace WireSerialize_ {
    template<typename T>
    void serialize(const rusty::BTreeSet<T>& self_, BinaryWriteArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        rrr::v64 v_len = rrr::v64::new_(static_cast<int64_t>(rusty::len(self_)));
        Serialize_::serialize(v_len, ar);
        auto it = rusty::iter(self_);
        while (true) {
            auto e = it.next();
            if (e.is_none()) {
                break;
            }
            Serialize_::serialize(e.unwrap(), ar);
        }
    }

}
// UFCS trait migration: free functions for `impl WireSerialize for ...`
namespace WireSerialize_ {
    template<typename K, typename V>
    void serialize(const rusty::BTreeMap<K, V>& self_, BinaryWriteArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        rrr::v64 v_len = rrr::v64::new_(static_cast<int64_t>(rusty::len(self_)));
        Serialize_::serialize(v_len, ar);
        auto it = rusty::iter(self_);
        while (true) {
            auto e = it.next();
            if (e.is_none()) {
                break;
            }
            const auto kv = e.unwrap();
            Serialize_::serialize(rusty::detail::deref_if_pointer(([](auto&& __t) -> decltype(auto) { if constexpr (requires { __t._0; }) return (std::forward<decltype(__t)>(__t)._0); else if constexpr (requires { std::get<0>(std::forward<decltype(__t)>(__t)); }) return std::get<0>(std::forward<decltype(__t)>(__t)); else if constexpr (requires { (*__t)._0; }) return ((*std::forward<decltype(__t)>(__t))._0); else return std::get<0>(*std::forward<decltype(__t)>(__t)); })(kv)), ar);
            Serialize_::serialize(rusty::detail::deref_if_pointer(([](auto&& __t) -> decltype(auto) { if constexpr (requires { __t._1; }) return (std::forward<decltype(__t)>(__t)._1); else if constexpr (requires { std::get<1>(std::forward<decltype(__t)>(__t)); }) return std::get<1>(std::forward<decltype(__t)>(__t)); else if constexpr (requires { (*__t)._1; }) return ((*std::forward<decltype(__t)>(__t))._1); else return std::get<1>(*std::forward<decltype(__t)>(__t)); })(kv)), ar);
        }
    }

}
/*RUSTYCPP:GEN-END id=serializable.wire_ser*/

namespace Serialize_ {
// Bridge the generated overloads back into `Serialize_` via 1-line
// forwarders (the trait block must live at `rrr` scope — a second
// `rusty_ext` inside `Serialize_` is ambiguous against the Deserialize
// trait's — but callers reach this family through the QUALIFIED name
// `Serialize_::serialize`, and a miss lands on the poisoned catch-all).
// A forwarder per container replaces the earlier using-declaration,
// which conflicts with same-signature forwarders declared in-scope.
template<class T>
inline void serialize(const std::list<T>& v, BinaryWriteArchive& ar) {
  WireSerialize_::serialize(v, ar);
}

template<class T>
inline void serialize(const rusty::BTreeSet<T>& v, BinaryWriteArchive& ar) {
  WireSerialize_::serialize(v, ar);
}

template<class T>
inline void serialize(const std::set<T>& v, BinaryWriteArchive& ar) {
  WireSerialize_::serialize(v, ar);
}

template<class T>
inline void serialize(const rusty::HashSet<T>& v, BinaryWriteArchive& ar) {
  rrr::v64 v_len{static_cast<rrr::i64>(v.len())};
  serialize(v_len, ar);
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
    serialize(std::get<0>(std::move(__e).unwrap()), ar);
}

template<class T>
inline void serialize(const std::unordered_set<T>& v, BinaryWriteArchive& ar) {
  WireSerialize_::serialize(v, ar);
}

template<class K, class V>
inline void serialize(const rusty::BTreeMap<K, V>& v, BinaryWriteArchive& ar) {
  WireSerialize_::serialize(v, ar);
}

template<class K, class V>
inline void serialize(const std::map<K, V>& v, BinaryWriteArchive& ar) {
  WireSerialize_::serialize(v, ar);
}

template<class K, class V>
inline void serialize(const rusty::HashMap<K, V>& v, BinaryWriteArchive& ar) {
  rrr::v64 v_len{static_cast<rrr::i64>(v.len())};
  serialize(v_len, ar);
  // rusty HashMap has no const begin()/end(); iterate via the Rust iterator.
  // iter().next() yields Option<std::tuple<const K&, const V&>>.
  // WARNING: like rusty::HashSet above, hashbrown enumeration crashes
  // clang-22's name mangler — do not instantiate this overload on clang-22.
  auto __it = v.iter();
  for (auto __e = __it.next(); __e.is_some(); __e = __it.next()) {
    auto kv = std::move(__e).unwrap();
    serialize(std::get<0>(kv), ar);
    serialize(std::get<1>(kv), ar);
  }
}

template<class K, class V>
inline void serialize(const std::unordered_map<K, V>& v, BinaryWriteArchive& ar) {
  WireSerialize_::serialize(v, ar);
}

}  // namespace Serialize_


// ---- Linear containers (length prefix + sequential elements). --------
// All linear containers share the wire format: v64 length prefix
// followed by each element serialized via operator<<. Iteration
// order matches the container's begin()/end(). For ordered containers
// (set/map/BTreeSet/BTreeMap) this is sorted-key order. For unordered
// containers (unordered_set/unordered_map/HashSet/HashMap) it's
// bucket-walk order — same iteration order as the existing
// `Marshal` operator<<, so byte-for-byte compatibility holds.












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
    // Read exactly n bytes or abort — the operator>> truncation contract
    // (short reads at this layer are programming errors, not recoverable).
    // The DSL leaf Deserialize impls call this so the verify() lives once.
    fn read_or_abort(&mut self, p: *mut u8, n: usize) {
        verify(self.read_exact(p, n));
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=serializable.read_archive version=1 rust_sha256=59c8686129a09ffcb6f6fb74eb086ff01f9ea8bb79f11e1078df41d7bc82ea3b*/
struct BinaryReadArchive;

struct BinaryReadArchive {
    SourceProxy source_;

    bool read_exact(uint8_t* p, size_t n);
    void read_or_abort(uint8_t* p, size_t n);
};


bool BinaryReadArchive::read_exact(uint8_t* p, size_t n) {
    return bra_read_exact((*this), p, std::move(n));
}

void BinaryReadArchive::read_or_abort(uint8_t* p, size_t n) {
    verify(this->read_exact(p, std::move(n)));
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

// ---- Serde-style Deserialize trait (wire migration). ------------------
// Value-side deserialization: each type reads itself from a
// BinaryReadArchive, mutating in place (`&mut self`, NOT `-> Self`, so
// container reads keep the default-construct-then-read-into shape). Lowers
// to a UFCS free fn `Deserialize_::deserialize(T&, BinaryReadArchive&)`.
// The `operator>>` overloads below forward here.
// Fwd-decls of the late hand-written overloads: the generated
// container bodies below resolve their qualified element calls against
// declarations visible at this point in the module (reachability =
// declaration order). The string overload covers pair<string, ...>
// elements; the CATCH-ALL template covers user-type elements
// (mdb::Value, janus::* in generated rcc_rpc.h maps) — at overload
// resolution the exact scalar/container matches still win (non-template
// beats template; partial ordering beats plain T&), so this reproduces
// exactly what the old bodies' unqualified lookup found.
namespace Deserialize_ {
inline void deserialize(std::string& self_, BinaryReadArchive& ar);
template<typename T>
inline void deserialize(T& v, BinaryReadArchive& ar);
}

#if RUSTYCPP_RUST
pub trait Deserialize {
    fn deserialize(&mut self, ar: &mut BinaryReadArchive);
}
impl Deserialize for v32 {
    fn deserialize(&mut self, ar: &mut BinaryReadArchive) {
        let mut b = varint_buf_new();
        verify(ar.read_exact(b.arr, 1));
        let total = SparseInt::buf_size(b.arr[0]);
        if total > 1 {
            verify(ar.read_exact(varint_tail(&mut b), total - 1));
        }
        self.set(SparseInt::load32(b.arr));
    }
}
impl Deserialize for v64 {
    fn deserialize(&mut self, ar: &mut BinaryReadArchive) {
        let mut b = varint_buf_new();
        verify(ar.read_exact(b.arr, 1));
        let total = SparseInt::buf_size(b.arr[0]);
        if total > 1 {
            verify(ar.read_exact(varint_tail(&mut b), total - 1));
        }
        self.set(SparseInt::load64(b.arr));
    }
}
impl Deserialize for i32 {
    fn deserialize(&mut self, ar: &mut BinaryReadArchive) {
        unsafe {
            let p: *mut u8 = (self as *mut i32) as *mut u8;
            ar.read_or_abort(p, std::mem::size_of::<i32>());
        }
    }
}
impl Deserialize for i8 {
    fn deserialize(&mut self, ar: &mut BinaryReadArchive) {
        unsafe {
            let p: *mut u8 = (self as *mut i8) as *mut u8;
            ar.read_or_abort(p, std::mem::size_of::<i8>());
        }
    }
}
impl Deserialize for i16 {
    fn deserialize(&mut self, ar: &mut BinaryReadArchive) {
        unsafe {
            let p: *mut u8 = (self as *mut i16) as *mut u8;
            ar.read_or_abort(p, std::mem::size_of::<i16>());
        }
    }
}
impl Deserialize for i64 {
    fn deserialize(&mut self, ar: &mut BinaryReadArchive) {
        unsafe {
            let p: *mut u8 = (self as *mut i64) as *mut u8;
            ar.read_or_abort(p, std::mem::size_of::<i64>());
        }
    }
}
impl Deserialize for u8 {
    fn deserialize(&mut self, ar: &mut BinaryReadArchive) {
        unsafe {
            let p: *mut u8 = (self as *mut u8) as *mut u8;
            ar.read_or_abort(p, std::mem::size_of::<u8>());
        }
    }
}
impl Deserialize for u16 {
    fn deserialize(&mut self, ar: &mut BinaryReadArchive) {
        unsafe {
            let p: *mut u8 = (self as *mut u16) as *mut u8;
            ar.read_or_abort(p, std::mem::size_of::<u16>());
        }
    }
}
impl Deserialize for u32 {
    fn deserialize(&mut self, ar: &mut BinaryReadArchive) {
        unsafe {
            let p: *mut u8 = (self as *mut u32) as *mut u8;
            ar.read_or_abort(p, std::mem::size_of::<u32>());
        }
    }
}
impl Deserialize for u64 {
    fn deserialize(&mut self, ar: &mut BinaryReadArchive) {
        unsafe {
            let p: *mut u8 = (self as *mut u64) as *mut u8;
            ar.read_or_abort(p, std::mem::size_of::<u64>());
        }
    }
}
impl Deserialize for f64 {
    fn deserialize(&mut self, ar: &mut BinaryReadArchive) {
        unsafe {
            let p: *mut u8 = (self as *mut f64) as *mut u8;
            ar.read_or_abort(p, std::mem::size_of::<f64>());
        }
    }
}

// ---- Container impls (generic; emitted straight into Deserialize_,
// where the callers and the nested-container fwd-decls already look —
// unlike the serialize side, no forwarders are needed here). Wire
// format: v64 length prefix + N elements in order; containers cleared
// first, matching the Marshal operator>> semantics the old hand
// bodies preserved. The hashbrown decoders ARE safe to convert: they
// only insert (the clang-22 mangler crash is in ENUMERATION, which
// only the serialize side does).

impl<T1, T2> Deserialize for std::pair<T1, T2> {
    fn deserialize(&mut self, ar: &mut BinaryReadArchive) {
        Deserialize_::deserialize(&mut self.first, ar);
        Deserialize_::deserialize(&mut self.second, ar);
    }
}

impl<T> Deserialize for Vec<T> {
    fn deserialize(&mut self, ar: &mut BinaryReadArchive) {
        let mut v_len = v64::new(0i64);
        Deserialize_::deserialize(&mut v_len, ar);
        self.clear();
        let n: usize = v_len.get() as usize;
        self.reserve(n);
        let mut i: usize = 0usize;
        while i < n {
            let mut elem: T = Default::default();
            Deserialize_::deserialize(&mut elem, ar);
            self.push(elem);
            i += 1usize;
        }
    }
}

impl<T> Deserialize for std::vector<T> {
    fn deserialize(&mut self, ar: &mut BinaryReadArchive) {
        let mut v_len = v64::new(0i64);
        Deserialize_::deserialize(&mut v_len, ar);
        self.clear();
        let n: usize = v_len.get() as usize;
        self.reserve(n);
        let mut i: usize = 0usize;
        while i < n {
            let mut elem: T = Default::default();
            Deserialize_::deserialize(&mut elem, ar);
            self.push_back(elem);
            i += 1usize;
        }
    }
}

impl<T> Deserialize for std::list<T> {
    fn deserialize(&mut self, ar: &mut BinaryReadArchive) {
        let mut v_len = v64::new(0i64);
        Deserialize_::deserialize(&mut v_len, ar);
        self.clear();
        let n: usize = v_len.get() as usize;
        let mut i: usize = 0usize;
        while i < n {
            let mut elem: T = Default::default();
            Deserialize_::deserialize(&mut elem, ar);
            self.push_back(elem);
            i += 1usize;
        }
    }
}

impl<T> Deserialize for rusty::BTreeSet<T> {
    fn deserialize(&mut self, ar: &mut BinaryReadArchive) {
        let mut v_len = v64::new(0i64);
        Deserialize_::deserialize(&mut v_len, ar);
        self.clear();
        let n: usize = v_len.get() as usize;
        let mut i: usize = 0usize;
        while i < n {
            let mut elem: T = Default::default();
            Deserialize_::deserialize(&mut elem, ar);
            self.insert(elem);
            i += 1usize;
        }
    }
}

impl<T> Deserialize for std::set<T> {
    fn deserialize(&mut self, ar: &mut BinaryReadArchive) {
        let mut v_len = v64::new(0i64);
        Deserialize_::deserialize(&mut v_len, ar);
        self.clear();
        let n: usize = v_len.get() as usize;
        let mut i: usize = 0usize;
        while i < n {
            let mut elem: T = Default::default();
            Deserialize_::deserialize(&mut elem, ar);
            self.insert(elem);
            i += 1usize;
        }
    }
}

impl<T> Deserialize for rusty::HashSet<T> {
    fn deserialize(&mut self, ar: &mut BinaryReadArchive) {
        let mut v_len = v64::new(0i64);
        Deserialize_::deserialize(&mut v_len, ar);
        self.clear();
        let n: usize = v_len.get() as usize;
        let mut i: usize = 0usize;
        while i < n {
            let mut elem: T = Default::default();
            Deserialize_::deserialize(&mut elem, ar);
            self.insert(elem);
            i += 1usize;
        }
    }
}

impl<T> Deserialize for std::unordered_set<T> {
    fn deserialize(&mut self, ar: &mut BinaryReadArchive) {
        let mut v_len = v64::new(0i64);
        Deserialize_::deserialize(&mut v_len, ar);
        self.clear();
        let n: usize = v_len.get() as usize;
        let mut i: usize = 0usize;
        while i < n {
            let mut elem: T = Default::default();
            Deserialize_::deserialize(&mut elem, ar);
            self.insert(elem);
            i += 1usize;
        }
    }
}

impl<K, V> Deserialize for rusty::BTreeMap<K, V> {
    fn deserialize(&mut self, ar: &mut BinaryReadArchive) {
        let mut v_len = v64::new(0i64);
        Deserialize_::deserialize(&mut v_len, ar);
        self.clear();
        let n: usize = v_len.get() as usize;
        let mut i: usize = 0usize;
        while i < n {
            let mut key: K = Default::default();
            let mut value: V = Default::default();
            Deserialize_::deserialize(&mut key, ar);
            Deserialize_::deserialize(&mut value, ar);
            self.insert(key, value);
            i += 1usize;
        }
    }
}

impl<K, V> Deserialize for std::map<K, V> {
    fn deserialize(&mut self, ar: &mut BinaryReadArchive) {
        let mut v_len = v64::new(0i64);
        Deserialize_::deserialize(&mut v_len, ar);
        self.clear();
        let n: usize = v_len.get() as usize;
        let mut i: usize = 0usize;
        while i < n {
            let mut key: K = Default::default();
            let mut value: V = Default::default();
            Deserialize_::deserialize(&mut key, ar);
            Deserialize_::deserialize(&mut value, ar);
            self.emplace(key, value);
            i += 1usize;
        }
    }
}

impl<K, V> Deserialize for rusty::HashMap<K, V> {
    fn deserialize(&mut self, ar: &mut BinaryReadArchive) {
        let mut v_len = v64::new(0i64);
        Deserialize_::deserialize(&mut v_len, ar);
        self.clear();
        let n: usize = v_len.get() as usize;
        let mut i: usize = 0usize;
        while i < n {
            let mut key: K = Default::default();
            let mut value: V = Default::default();
            Deserialize_::deserialize(&mut key, ar);
            Deserialize_::deserialize(&mut value, ar);
            self.insert(key, value);
            i += 1usize;
        }
    }
}

impl<K, V> Deserialize for std::unordered_map<K, V> {
    fn deserialize(&mut self, ar: &mut BinaryReadArchive) {
        let mut v_len = v64::new(0i64);
        Deserialize_::deserialize(&mut v_len, ar);
        self.clear();
        let n: usize = v_len.get() as usize;
        let mut i: usize = 0usize;
        while i < n {
            let mut key: K = Default::default();
            let mut value: V = Default::default();
            Deserialize_::deserialize(&mut key, ar);
            Deserialize_::deserialize(&mut value, ar);
            self.emplace(key, value);
            i += 1usize;
        }
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=serializable.deserialize_trait version=1 rust_sha256=9bbaa27e5e3e9d5025dae8faf15ce213041bc18abc8bc42ee7c44d27aa790bf7*/
class Deserialize;

// Extension trait free-function forward declarations
namespace rusty_ext {
    void deserialize(v32& self_, BinaryReadArchive& ar);

    void deserialize(v64& self_, BinaryReadArchive& ar);

    void deserialize(int32_t& self_, BinaryReadArchive& ar);

    void deserialize(int8_t& self_, BinaryReadArchive& ar);

    void deserialize(int16_t& self_, BinaryReadArchive& ar);

    void deserialize(int64_t& self_, BinaryReadArchive& ar);

    void deserialize(uint8_t& self_, BinaryReadArchive& ar);

    void deserialize(uint16_t& self_, BinaryReadArchive& ar);

    void deserialize(uint32_t& self_, BinaryReadArchive& ar);

    void deserialize(uint64_t& self_, BinaryReadArchive& ar);

    void deserialize(double& self_, BinaryReadArchive& ar);

    template<typename T1, typename T2>
    void deserialize(std::pair<T1, T2>& self_, BinaryReadArchive& ar);

    template<typename T>
    void deserialize(rusty::Vec<T>& self_, BinaryReadArchive& ar);

    template<typename T>
    void deserialize(std::vector<T>& self_, BinaryReadArchive& ar);

    template<typename T>
    void deserialize(std::list<T>& self_, BinaryReadArchive& ar);

    template<typename T>
    void deserialize(rusty::BTreeSet<T>& self_, BinaryReadArchive& ar);

    template<typename T>
    void deserialize(std::set<T>& self_, BinaryReadArchive& ar);

    template<typename T>
    void deserialize(rusty::HashSet<T>& self_, BinaryReadArchive& ar);

    template<typename T>
    void deserialize(std::unordered_set<T>& self_, BinaryReadArchive& ar);

    template<typename K, typename V>
    void deserialize(rusty::BTreeMap<K, V>& self_, BinaryReadArchive& ar);

    template<typename K, typename V>
    void deserialize(std::map<K, V>& self_, BinaryReadArchive& ar);

    template<typename K, typename V>
    void deserialize(rusty::HashMap<K, V>& self_, BinaryReadArchive& ar);

    template<typename K, typename V>
    void deserialize(std::unordered_map<K, V>& self_, BinaryReadArchive& ar);

}


namespace Deserialize_ {
    void deserialize(v32& self_, BinaryReadArchive& ar);
}
using namespace Deserialize_;
namespace Deserialize_ {
    void deserialize(v64& self_, BinaryReadArchive& ar);
}
using namespace Deserialize_;
namespace Deserialize_ {
    void deserialize(int32_t& self_, BinaryReadArchive& ar);
}
using namespace Deserialize_;
namespace Deserialize_ {
    void deserialize(int8_t& self_, BinaryReadArchive& ar);
}
using namespace Deserialize_;
namespace Deserialize_ {
    void deserialize(int16_t& self_, BinaryReadArchive& ar);
}
using namespace Deserialize_;
namespace Deserialize_ {
    void deserialize(int64_t& self_, BinaryReadArchive& ar);
}
using namespace Deserialize_;
namespace Deserialize_ {
    void deserialize(uint8_t& self_, BinaryReadArchive& ar);
}
using namespace Deserialize_;
namespace Deserialize_ {
    void deserialize(uint16_t& self_, BinaryReadArchive& ar);
}
using namespace Deserialize_;
namespace Deserialize_ {
    void deserialize(uint32_t& self_, BinaryReadArchive& ar);
}
using namespace Deserialize_;
namespace Deserialize_ {
    void deserialize(uint64_t& self_, BinaryReadArchive& ar);
}
using namespace Deserialize_;
namespace Deserialize_ {
    void deserialize(double& self_, BinaryReadArchive& ar);
}
using namespace Deserialize_;
namespace Deserialize_ {
    template<typename T1, typename T2>
    void deserialize(std::pair<T1, T2>& self_, BinaryReadArchive& ar);
}
using namespace Deserialize_;
namespace Deserialize_ {
    template<typename T>
    void deserialize(rusty::Vec<T>& self_, BinaryReadArchive& ar);
}
using namespace Deserialize_;
namespace Deserialize_ {
    template<typename T>
    void deserialize(std::vector<T>& self_, BinaryReadArchive& ar);
}
using namespace Deserialize_;
namespace Deserialize_ {
    template<typename T>
    void deserialize(std::list<T>& self_, BinaryReadArchive& ar);
}
using namespace Deserialize_;
namespace Deserialize_ {
    template<typename T>
    void deserialize(rusty::BTreeSet<T>& self_, BinaryReadArchive& ar);
}
using namespace Deserialize_;
namespace Deserialize_ {
    template<typename T>
    void deserialize(std::set<T>& self_, BinaryReadArchive& ar);
}
using namespace Deserialize_;
namespace Deserialize_ {
    template<typename T>
    void deserialize(rusty::HashSet<T>& self_, BinaryReadArchive& ar);
}
using namespace Deserialize_;
namespace Deserialize_ {
    template<typename T>
    void deserialize(std::unordered_set<T>& self_, BinaryReadArchive& ar);
}
using namespace Deserialize_;
namespace Deserialize_ {
    template<typename K, typename V>
    void deserialize(rusty::BTreeMap<K, V>& self_, BinaryReadArchive& ar);
}
using namespace Deserialize_;
namespace Deserialize_ {
    template<typename K, typename V>
    void deserialize(std::map<K, V>& self_, BinaryReadArchive& ar);
}
using namespace Deserialize_;
namespace Deserialize_ {
    template<typename K, typename V>
    void deserialize(rusty::HashMap<K, V>& self_, BinaryReadArchive& ar);
}
using namespace Deserialize_;
namespace Deserialize_ {
    template<typename K, typename V>
    void deserialize(std::unordered_map<K, V>& self_, BinaryReadArchive& ar);
}
using namespace Deserialize_;
class Deserialize {
public:
    virtual ~Deserialize() noexcept(false) {}
    virtual void deserialize(BinaryReadArchive& ar) = 0;
    Deserialize(const Deserialize&) = delete;
    Deserialize& operator=(const Deserialize&) = delete;
    Deserialize(Deserialize&&) = delete;
    Deserialize& operator=(Deserialize&&) = delete;
protected:
    Deserialize() = default;
};

template <class U> class DeserializeAdapter;
template <class U> class DeserializeAdapterRef;
template <class U> class DeserializeAdapterRefMut;

// trait impl for `v32` lowered via the Deserialize_ free functions above

// trait impl for `v64` lowered via the Deserialize_ free functions above

// trait impl for `i32` lowered via the Deserialize_ free functions above

// trait impl for `i8` lowered via the Deserialize_ free functions above

// trait impl for `i16` lowered via the Deserialize_ free functions above

// trait impl for `i64` lowered via the Deserialize_ free functions above

// trait impl for `u8` lowered via the Deserialize_ free functions above

// trait impl for `u16` lowered via the Deserialize_ free functions above

// trait impl for `u32` lowered via the Deserialize_ free functions above

// trait impl for `u64` lowered via the Deserialize_ free functions above

// trait impl for `f64` lowered via the Deserialize_ free functions above

// trait impl for `std::pair` lowered via the Deserialize_ free functions above

// trait impl for `Vec` lowered via the Deserialize_ free functions above

// trait impl for `std::vector` lowered via the Deserialize_ free functions above

// trait impl for `std::list` lowered via the Deserialize_ free functions above

// trait impl for `rusty::BTreeSet` lowered via the Deserialize_ free functions above

// trait impl for `std::set` lowered via the Deserialize_ free functions above

// trait impl for `rusty::HashSet` lowered via the Deserialize_ free functions above

// trait impl for `std::unordered_set` lowered via the Deserialize_ free functions above

// trait impl for `rusty::BTreeMap` lowered via the Deserialize_ free functions above

// trait impl for `std::map` lowered via the Deserialize_ free functions above

// trait impl for `rusty::HashMap` lowered via the Deserialize_ free functions above

// trait impl for `std::unordered_map` lowered via the Deserialize_ free functions above

// Extension trait Deserialize lowered to rusty_ext:: free functions
namespace rusty_ext {
    void deserialize(v32& self_, BinaryReadArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        auto b = varint_buf_new();
        verify(ar.read_exact(std::move([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.arr); }) { return (__r.arr); } else if constexpr (requires { (__r.arr_field); }) { return (__r.arr_field); } else if constexpr (requires { ((*__r).arr); }) { return ((*__r).arr); } else { return ((*__r).arr_field); } }(b)), 1));
        const auto total = SparseInt::buf_size([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.arr); }) { return (__r.arr); } else if constexpr (requires { (__r.arr_field); }) { return (__r.arr_field); } else if constexpr (requires { ((*__r).arr); }) { return ((*__r).arr); } else { return ((*__r).arr_field); } }(b)[static_cast<size_t>(0)]);
        if (rusty::detail::deref_if_pointer_like(total) > 1) {
            verify(ar.read_exact(varint_tail(&b), rusty::detail::deref_if_pointer_like(total) - 1));
        }
        self_.set(SparseInt::load32(std::move([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.arr); }) { return (__r.arr); } else if constexpr (requires { (__r.arr_field); }) { return (__r.arr_field); } else if constexpr (requires { ((*__r).arr); }) { return ((*__r).arr); } else { return ((*__r).arr_field); } }(b))));
    }

    void deserialize(v64& self_, BinaryReadArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        auto b = varint_buf_new();
        verify(ar.read_exact(std::move([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.arr); }) { return (__r.arr); } else if constexpr (requires { (__r.arr_field); }) { return (__r.arr_field); } else if constexpr (requires { ((*__r).arr); }) { return ((*__r).arr); } else { return ((*__r).arr_field); } }(b)), 1));
        const auto total = SparseInt::buf_size([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.arr); }) { return (__r.arr); } else if constexpr (requires { (__r.arr_field); }) { return (__r.arr_field); } else if constexpr (requires { ((*__r).arr); }) { return ((*__r).arr); } else { return ((*__r).arr_field); } }(b)[static_cast<size_t>(0)]);
        if (rusty::detail::deref_if_pointer_like(total) > 1) {
            verify(ar.read_exact(varint_tail(&b), rusty::detail::deref_if_pointer_like(total) - 1));
        }
        self_.set(SparseInt::load64(std::move([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.arr); }) { return (__r.arr); } else if constexpr (requires { (__r.arr_field); }) { return (__r.arr_field); } else if constexpr (requires { ((*__r).arr); }) { return ((*__r).arr); } else { return ((*__r).arr_field); } }(b))));
    }

    void deserialize(int32_t& self_, BinaryReadArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        // @unsafe
        {
            uint8_t* const p = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>((static_cast<int32_t*>(rusty::detail::ptr_or_addr(self_)))));
            ar.read_or_abort(p, rusty::mem::size_of<int32_t>());
        }
    }

    void deserialize(int8_t& self_, BinaryReadArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        // @unsafe
        {
            uint8_t* const p = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>((static_cast<int8_t*>(rusty::detail::ptr_or_addr(self_)))));
            ar.read_or_abort(p, rusty::mem::size_of<int8_t>());
        }
    }

    void deserialize(int16_t& self_, BinaryReadArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        // @unsafe
        {
            uint8_t* const p = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>((static_cast<int16_t*>(rusty::detail::ptr_or_addr(self_)))));
            ar.read_or_abort(p, rusty::mem::size_of<int16_t>());
        }
    }

    void deserialize(int64_t& self_, BinaryReadArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        // @unsafe
        {
            uint8_t* const p = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>((static_cast<int64_t*>(rusty::detail::ptr_or_addr(self_)))));
            ar.read_or_abort(p, rusty::mem::size_of<int64_t>());
        }
    }

    void deserialize(uint8_t& self_, BinaryReadArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        // @unsafe
        {
            uint8_t* const p = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>((static_cast<uint8_t*>(rusty::detail::ptr_or_addr(self_)))));
            ar.read_or_abort(p, rusty::mem::size_of<uint8_t>());
        }
    }

    void deserialize(uint16_t& self_, BinaryReadArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        // @unsafe
        {
            uint8_t* const p = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>((static_cast<uint16_t*>(rusty::detail::ptr_or_addr(self_)))));
            ar.read_or_abort(p, rusty::mem::size_of<uint16_t>());
        }
    }

    void deserialize(uint32_t& self_, BinaryReadArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        // @unsafe
        {
            uint8_t* const p = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>((static_cast<uint32_t*>(rusty::detail::ptr_or_addr(self_)))));
            ar.read_or_abort(p, rusty::mem::size_of<uint32_t>());
        }
    }

    void deserialize(uint64_t& self_, BinaryReadArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        // @unsafe
        {
            uint8_t* const p = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>((static_cast<uint64_t*>(rusty::detail::ptr_or_addr(self_)))));
            ar.read_or_abort(p, rusty::mem::size_of<uint64_t>());
        }
    }

    void deserialize(double& self_, BinaryReadArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        // @unsafe
        {
            uint8_t* const p = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>((static_cast<double*>(rusty::detail::ptr_or_addr(self_)))));
            ar.read_or_abort(p, rusty::mem::size_of<double>());
        }
    }

    template<typename T1, typename T2>
    void deserialize(std::pair<T1, T2>& self_, BinaryReadArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        Deserialize_::deserialize(self_.first, ar);
        Deserialize_::deserialize(self_.second, ar);
    }

    template<typename T>
    void deserialize(rusty::Vec<T>& self_, BinaryReadArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        auto v_len = v64::new_(static_cast<int64_t>(0));
        Deserialize_::deserialize(v_len, ar);
        self_.clear();
        const size_t n = static_cast<size_t>(v_len.get());
        self_.reserve(std::move(n));
        size_t i = static_cast<size_t>(0);
        while (rusty::detail::deref_if_pointer_like(i) < rusty::detail::deref_if_pointer_like(n)) {
            T elem = rusty::default_like<T>();
            Deserialize_::deserialize(elem, ar);
            self_.push(std::move(elem));
            i += static_cast<size_t>(1);
        }
    }

    template<typename T>
    void deserialize(std::vector<T>& self_, BinaryReadArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        auto v_len = v64::new_(static_cast<int64_t>(0));
        Deserialize_::deserialize(v_len, ar);
        self_.clear();
        const size_t n = static_cast<size_t>(v_len.get());
        self_.reserve(std::move(n));
        size_t i = static_cast<size_t>(0);
        while (rusty::detail::deref_if_pointer_like(i) < rusty::detail::deref_if_pointer_like(n)) {
            T elem = rusty::default_like<T>();
            Deserialize_::deserialize(elem, ar);
            self_.push_back(std::move(elem));
            i += static_cast<size_t>(1);
        }
    }

    template<typename T>
    void deserialize(std::list<T>& self_, BinaryReadArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        auto v_len = v64::new_(static_cast<int64_t>(0));
        Deserialize_::deserialize(v_len, ar);
        self_.clear();
        const size_t n = static_cast<size_t>(v_len.get());
        size_t i = static_cast<size_t>(0);
        while (rusty::detail::deref_if_pointer_like(i) < rusty::detail::deref_if_pointer_like(n)) {
            T elem = rusty::default_like<T>();
            Deserialize_::deserialize(elem, ar);
            self_.push_back(std::move(elem));
            i += static_cast<size_t>(1);
        }
    }

    template<typename T>
    void deserialize(rusty::BTreeSet<T>& self_, BinaryReadArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        auto v_len = v64::new_(static_cast<int64_t>(0));
        Deserialize_::deserialize(v_len, ar);
        self_.clear();
        const size_t n = static_cast<size_t>(v_len.get());
        size_t i = static_cast<size_t>(0);
        while (rusty::detail::deref_if_pointer_like(i) < rusty::detail::deref_if_pointer_like(n)) {
            T elem = rusty::default_like<T>();
            Deserialize_::deserialize(elem, ar);
            self_.insert(std::move(elem));
            i += static_cast<size_t>(1);
        }
    }

    template<typename T>
    void deserialize(std::set<T>& self_, BinaryReadArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        auto v_len = v64::new_(static_cast<int64_t>(0));
        Deserialize_::deserialize(v_len, ar);
        self_.clear();
        const size_t n = static_cast<size_t>(v_len.get());
        size_t i = static_cast<size_t>(0);
        while (rusty::detail::deref_if_pointer_like(i) < rusty::detail::deref_if_pointer_like(n)) {
            T elem = rusty::default_like<T>();
            Deserialize_::deserialize(elem, ar);
            self_.insert(std::move(elem));
            i += static_cast<size_t>(1);
        }
    }

    template<typename T>
    void deserialize(rusty::HashSet<T>& self_, BinaryReadArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        auto v_len = v64::new_(static_cast<int64_t>(0));
        Deserialize_::deserialize(v_len, ar);
        self_.clear();
        const size_t n = static_cast<size_t>(v_len.get());
        size_t i = static_cast<size_t>(0);
        while (rusty::detail::deref_if_pointer_like(i) < rusty::detail::deref_if_pointer_like(n)) {
            T elem = rusty::default_like<T>();
            Deserialize_::deserialize(elem, ar);
            self_.insert(std::move(elem));
            i += static_cast<size_t>(1);
        }
    }

    template<typename T>
    void deserialize(std::unordered_set<T>& self_, BinaryReadArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        auto v_len = v64::new_(static_cast<int64_t>(0));
        Deserialize_::deserialize(v_len, ar);
        self_.clear();
        const size_t n = static_cast<size_t>(v_len.get());
        size_t i = static_cast<size_t>(0);
        while (rusty::detail::deref_if_pointer_like(i) < rusty::detail::deref_if_pointer_like(n)) {
            T elem = rusty::default_like<T>();
            Deserialize_::deserialize(elem, ar);
            self_.insert(std::move(elem));
            i += static_cast<size_t>(1);
        }
    }

    template<typename K, typename V>
    void deserialize(rusty::BTreeMap<K, V>& self_, BinaryReadArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        auto v_len = v64::new_(static_cast<int64_t>(0));
        Deserialize_::deserialize(v_len, ar);
        self_.clear();
        const size_t n = static_cast<size_t>(v_len.get());
        size_t i = static_cast<size_t>(0);
        while (rusty::detail::deref_if_pointer_like(i) < rusty::detail::deref_if_pointer_like(n)) {
            K key = rusty::default_like<K>();
            V value = rusty::default_like<V>();
            Deserialize_::deserialize(key, ar);
            Deserialize_::deserialize(value, ar);
            self_.insert(std::move(key), std::move(value));
            i += static_cast<size_t>(1);
        }
    }

    template<typename K, typename V>
    void deserialize(std::map<K, V>& self_, BinaryReadArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        auto v_len = v64::new_(static_cast<int64_t>(0));
        Deserialize_::deserialize(v_len, ar);
        self_.clear();
        const size_t n = static_cast<size_t>(v_len.get());
        size_t i = static_cast<size_t>(0);
        while (rusty::detail::deref_if_pointer_like(i) < rusty::detail::deref_if_pointer_like(n)) {
            K key = rusty::default_like<K>();
            V value = rusty::default_like<V>();
            Deserialize_::deserialize(key, ar);
            Deserialize_::deserialize(value, ar);
            self_.emplace(std::move(key), std::move(value));
            i += static_cast<size_t>(1);
        }
    }

    template<typename K, typename V>
    void deserialize(rusty::HashMap<K, V>& self_, BinaryReadArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        auto v_len = v64::new_(static_cast<int64_t>(0));
        Deserialize_::deserialize(v_len, ar);
        self_.clear();
        const size_t n = static_cast<size_t>(v_len.get());
        size_t i = static_cast<size_t>(0);
        while (rusty::detail::deref_if_pointer_like(i) < rusty::detail::deref_if_pointer_like(n)) {
            K key = rusty::default_like<K>();
            V value = rusty::default_like<V>();
            Deserialize_::deserialize(key, ar);
            Deserialize_::deserialize(value, ar);
            self_.insert(std::move(key), std::move(value));
            i += static_cast<size_t>(1);
        }
    }

    template<typename K, typename V>
    void deserialize(std::unordered_map<K, V>& self_, BinaryReadArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        auto v_len = v64::new_(static_cast<int64_t>(0));
        Deserialize_::deserialize(v_len, ar);
        self_.clear();
        const size_t n = static_cast<size_t>(v_len.get());
        size_t i = static_cast<size_t>(0);
        while (rusty::detail::deref_if_pointer_like(i) < rusty::detail::deref_if_pointer_like(n)) {
            K key = rusty::default_like<K>();
            V value = rusty::default_like<V>();
            Deserialize_::deserialize(key, ar);
            Deserialize_::deserialize(value, ar);
            self_.emplace(std::move(key), std::move(value));
            i += static_cast<size_t>(1);
        }
    }

}

template <>
class DeserializeAdapter<v32> final : public Deserialize {
    v32 value_;
public:
    DeserializeAdapter(v32 v) : value_(std::move(v)) {}
    DeserializeAdapter(DeserializeAdapter&& other) : value_(std::move(other.value_)) {}
    void deserialize(BinaryReadArchive& ar) override {
        rusty_ext::deserialize(value_, ar);
    }
};

template <>
class DeserializeAdapterRef<v32> final : public Deserialize {
    const v32& value_;
public:
    explicit DeserializeAdapterRef(const v32& u) : value_(u) {}
    void deserialize(BinaryReadArchive& ar) override {
        std::abort();  // unreachable through &dyn T
    }
};

template <>
class DeserializeAdapterRefMut<v32> final : public Deserialize {
    v32& value_;
public:
    explicit DeserializeAdapterRefMut(v32& u) : value_(u) {}
    void deserialize(BinaryReadArchive& ar) override {
        rusty_ext::deserialize(value_, ar);
    }
};

template <>
class DeserializeAdapter<v64> final : public Deserialize {
    v64 value_;
public:
    DeserializeAdapter(v64 v) : value_(std::move(v)) {}
    DeserializeAdapter(DeserializeAdapter&& other) : value_(std::move(other.value_)) {}
    void deserialize(BinaryReadArchive& ar) override {
        rusty_ext::deserialize(value_, ar);
    }
};

template <>
class DeserializeAdapterRef<v64> final : public Deserialize {
    const v64& value_;
public:
    explicit DeserializeAdapterRef(const v64& u) : value_(u) {}
    void deserialize(BinaryReadArchive& ar) override {
        std::abort();  // unreachable through &dyn T
    }
};

template <>
class DeserializeAdapterRefMut<v64> final : public Deserialize {
    v64& value_;
public:
    explicit DeserializeAdapterRefMut(v64& u) : value_(u) {}
    void deserialize(BinaryReadArchive& ar) override {
        rusty_ext::deserialize(value_, ar);
    }
};

template <>
class DeserializeAdapter<int32_t> final : public Deserialize {
    int32_t value_;
public:
    DeserializeAdapter(int32_t v) : value_(std::move(v)) {}
    DeserializeAdapter(DeserializeAdapter&& other) : value_(std::move(other.value_)) {}
    void deserialize(BinaryReadArchive& ar) override {
        rusty_ext::deserialize(value_, ar);
    }
};

template <>
class DeserializeAdapterRef<int32_t> final : public Deserialize {
    const int32_t& value_;
public:
    explicit DeserializeAdapterRef(const int32_t& u) : value_(u) {}
    void deserialize(BinaryReadArchive& ar) override {
        std::abort();  // unreachable through &dyn T
    }
};

template <>
class DeserializeAdapterRefMut<int32_t> final : public Deserialize {
    int32_t& value_;
public:
    explicit DeserializeAdapterRefMut(int32_t& u) : value_(u) {}
    void deserialize(BinaryReadArchive& ar) override {
        rusty_ext::deserialize(value_, ar);
    }
};

template <>
class DeserializeAdapter<int8_t> final : public Deserialize {
    int8_t value_;
public:
    DeserializeAdapter(int8_t v) : value_(std::move(v)) {}
    DeserializeAdapter(DeserializeAdapter&& other) : value_(std::move(other.value_)) {}
    void deserialize(BinaryReadArchive& ar) override {
        rusty_ext::deserialize(value_, ar);
    }
};

template <>
class DeserializeAdapterRef<int8_t> final : public Deserialize {
    const int8_t& value_;
public:
    explicit DeserializeAdapterRef(const int8_t& u) : value_(u) {}
    void deserialize(BinaryReadArchive& ar) override {
        std::abort();  // unreachable through &dyn T
    }
};

template <>
class DeserializeAdapterRefMut<int8_t> final : public Deserialize {
    int8_t& value_;
public:
    explicit DeserializeAdapterRefMut(int8_t& u) : value_(u) {}
    void deserialize(BinaryReadArchive& ar) override {
        rusty_ext::deserialize(value_, ar);
    }
};

template <>
class DeserializeAdapter<int16_t> final : public Deserialize {
    int16_t value_;
public:
    DeserializeAdapter(int16_t v) : value_(std::move(v)) {}
    DeserializeAdapter(DeserializeAdapter&& other) : value_(std::move(other.value_)) {}
    void deserialize(BinaryReadArchive& ar) override {
        rusty_ext::deserialize(value_, ar);
    }
};

template <>
class DeserializeAdapterRef<int16_t> final : public Deserialize {
    const int16_t& value_;
public:
    explicit DeserializeAdapterRef(const int16_t& u) : value_(u) {}
    void deserialize(BinaryReadArchive& ar) override {
        std::abort();  // unreachable through &dyn T
    }
};

template <>
class DeserializeAdapterRefMut<int16_t> final : public Deserialize {
    int16_t& value_;
public:
    explicit DeserializeAdapterRefMut(int16_t& u) : value_(u) {}
    void deserialize(BinaryReadArchive& ar) override {
        rusty_ext::deserialize(value_, ar);
    }
};

template <>
class DeserializeAdapter<int64_t> final : public Deserialize {
    int64_t value_;
public:
    DeserializeAdapter(int64_t v) : value_(std::move(v)) {}
    DeserializeAdapter(DeserializeAdapter&& other) : value_(std::move(other.value_)) {}
    void deserialize(BinaryReadArchive& ar) override {
        rusty_ext::deserialize(value_, ar);
    }
};

template <>
class DeserializeAdapterRef<int64_t> final : public Deserialize {
    const int64_t& value_;
public:
    explicit DeserializeAdapterRef(const int64_t& u) : value_(u) {}
    void deserialize(BinaryReadArchive& ar) override {
        std::abort();  // unreachable through &dyn T
    }
};

template <>
class DeserializeAdapterRefMut<int64_t> final : public Deserialize {
    int64_t& value_;
public:
    explicit DeserializeAdapterRefMut(int64_t& u) : value_(u) {}
    void deserialize(BinaryReadArchive& ar) override {
        rusty_ext::deserialize(value_, ar);
    }
};

template <>
class DeserializeAdapter<uint8_t> final : public Deserialize {
    uint8_t value_;
public:
    DeserializeAdapter(uint8_t v) : value_(std::move(v)) {}
    DeserializeAdapter(DeserializeAdapter&& other) : value_(std::move(other.value_)) {}
    void deserialize(BinaryReadArchive& ar) override {
        rusty_ext::deserialize(value_, ar);
    }
};

template <>
class DeserializeAdapterRef<uint8_t> final : public Deserialize {
    const uint8_t& value_;
public:
    explicit DeserializeAdapterRef(const uint8_t& u) : value_(u) {}
    void deserialize(BinaryReadArchive& ar) override {
        std::abort();  // unreachable through &dyn T
    }
};

template <>
class DeserializeAdapterRefMut<uint8_t> final : public Deserialize {
    uint8_t& value_;
public:
    explicit DeserializeAdapterRefMut(uint8_t& u) : value_(u) {}
    void deserialize(BinaryReadArchive& ar) override {
        rusty_ext::deserialize(value_, ar);
    }
};

template <>
class DeserializeAdapter<uint16_t> final : public Deserialize {
    uint16_t value_;
public:
    DeserializeAdapter(uint16_t v) : value_(std::move(v)) {}
    DeserializeAdapter(DeserializeAdapter&& other) : value_(std::move(other.value_)) {}
    void deserialize(BinaryReadArchive& ar) override {
        rusty_ext::deserialize(value_, ar);
    }
};

template <>
class DeserializeAdapterRef<uint16_t> final : public Deserialize {
    const uint16_t& value_;
public:
    explicit DeserializeAdapterRef(const uint16_t& u) : value_(u) {}
    void deserialize(BinaryReadArchive& ar) override {
        std::abort();  // unreachable through &dyn T
    }
};

template <>
class DeserializeAdapterRefMut<uint16_t> final : public Deserialize {
    uint16_t& value_;
public:
    explicit DeserializeAdapterRefMut(uint16_t& u) : value_(u) {}
    void deserialize(BinaryReadArchive& ar) override {
        rusty_ext::deserialize(value_, ar);
    }
};

template <>
class DeserializeAdapter<uint32_t> final : public Deserialize {
    uint32_t value_;
public:
    DeserializeAdapter(uint32_t v) : value_(std::move(v)) {}
    DeserializeAdapter(DeserializeAdapter&& other) : value_(std::move(other.value_)) {}
    void deserialize(BinaryReadArchive& ar) override {
        rusty_ext::deserialize(value_, ar);
    }
};

template <>
class DeserializeAdapterRef<uint32_t> final : public Deserialize {
    const uint32_t& value_;
public:
    explicit DeserializeAdapterRef(const uint32_t& u) : value_(u) {}
    void deserialize(BinaryReadArchive& ar) override {
        std::abort();  // unreachable through &dyn T
    }
};

template <>
class DeserializeAdapterRefMut<uint32_t> final : public Deserialize {
    uint32_t& value_;
public:
    explicit DeserializeAdapterRefMut(uint32_t& u) : value_(u) {}
    void deserialize(BinaryReadArchive& ar) override {
        rusty_ext::deserialize(value_, ar);
    }
};

template <>
class DeserializeAdapter<uint64_t> final : public Deserialize {
    uint64_t value_;
public:
    DeserializeAdapter(uint64_t v) : value_(std::move(v)) {}
    DeserializeAdapter(DeserializeAdapter&& other) : value_(std::move(other.value_)) {}
    void deserialize(BinaryReadArchive& ar) override {
        rusty_ext::deserialize(value_, ar);
    }
};

template <>
class DeserializeAdapterRef<uint64_t> final : public Deserialize {
    const uint64_t& value_;
public:
    explicit DeserializeAdapterRef(const uint64_t& u) : value_(u) {}
    void deserialize(BinaryReadArchive& ar) override {
        std::abort();  // unreachable through &dyn T
    }
};

template <>
class DeserializeAdapterRefMut<uint64_t> final : public Deserialize {
    uint64_t& value_;
public:
    explicit DeserializeAdapterRefMut(uint64_t& u) : value_(u) {}
    void deserialize(BinaryReadArchive& ar) override {
        rusty_ext::deserialize(value_, ar);
    }
};

template <>
class DeserializeAdapter<double> final : public Deserialize {
    double value_;
public:
    DeserializeAdapter(double v) : value_(std::move(v)) {}
    DeserializeAdapter(DeserializeAdapter&& other) : value_(std::move(other.value_)) {}
    void deserialize(BinaryReadArchive& ar) override {
        rusty_ext::deserialize(value_, ar);
    }
};

template <>
class DeserializeAdapterRef<double> final : public Deserialize {
    const double& value_;
public:
    explicit DeserializeAdapterRef(const double& u) : value_(u) {}
    void deserialize(BinaryReadArchive& ar) override {
        std::abort();  // unreachable through &dyn T
    }
};

template <>
class DeserializeAdapterRefMut<double> final : public Deserialize {
    double& value_;
public:
    explicit DeserializeAdapterRefMut(double& u) : value_(u) {}
    void deserialize(BinaryReadArchive& ar) override {
        rusty_ext::deserialize(value_, ar);
    }
};

// TODO(interface_traits): skipped generic impl `DeserializeAdapter<std::pair<T1, T2>>`
// TODO(interface_traits): skipped generic impl `DeserializeAdapter<rusty::Vec<T>>`
// TODO(interface_traits): skipped generic impl `DeserializeAdapter<std::vector<T>>`
// TODO(interface_traits): skipped generic impl `DeserializeAdapter<std::list<T>>`
// TODO(interface_traits): skipped generic impl `DeserializeAdapter<rusty::BTreeSet<T>>`
// TODO(interface_traits): skipped generic impl `DeserializeAdapter<std::set<T>>`
// TODO(interface_traits): skipped generic impl `DeserializeAdapter<rusty::HashSet<T>>`
// TODO(interface_traits): skipped generic impl `DeserializeAdapter<std::unordered_set<T>>`
// TODO(interface_traits): skipped generic impl `DeserializeAdapter<rusty::BTreeMap<K, V>>`
// TODO(interface_traits): skipped generic impl `DeserializeAdapter<std::map<K, V>>`
// TODO(interface_traits): skipped generic impl `DeserializeAdapter<rusty::HashMap<K, V>>`
// TODO(interface_traits): skipped generic impl `DeserializeAdapter<std::unordered_map<K, V>>`

// UFCS trait migration: free functions for `impl Deserialize for ...`
namespace Deserialize_ {
    void deserialize(v32& self_, BinaryReadArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        auto b = varint_buf_new();
        verify(ar.read_exact(std::move([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.arr); }) { return (__r.arr); } else if constexpr (requires { (__r.arr_field); }) { return (__r.arr_field); } else if constexpr (requires { ((*__r).arr); }) { return ((*__r).arr); } else { return ((*__r).arr_field); } }(b)), 1));
        const auto total = SparseInt::buf_size([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.arr); }) { return (__r.arr); } else if constexpr (requires { (__r.arr_field); }) { return (__r.arr_field); } else if constexpr (requires { ((*__r).arr); }) { return ((*__r).arr); } else { return ((*__r).arr_field); } }(b)[static_cast<size_t>(0)]);
        if (rusty::detail::deref_if_pointer_like(total) > 1) {
            verify(ar.read_exact(varint_tail(&b), rusty::detail::deref_if_pointer_like(total) - 1));
        }
        self_.set(SparseInt::load32(std::move([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.arr); }) { return (__r.arr); } else if constexpr (requires { (__r.arr_field); }) { return (__r.arr_field); } else if constexpr (requires { ((*__r).arr); }) { return ((*__r).arr); } else { return ((*__r).arr_field); } }(b))));
    }

}
// UFCS trait migration: free functions for `impl Deserialize for ...`
namespace Deserialize_ {
    void deserialize(v64& self_, BinaryReadArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        auto b = varint_buf_new();
        verify(ar.read_exact(std::move([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.arr); }) { return (__r.arr); } else if constexpr (requires { (__r.arr_field); }) { return (__r.arr_field); } else if constexpr (requires { ((*__r).arr); }) { return ((*__r).arr); } else { return ((*__r).arr_field); } }(b)), 1));
        const auto total = SparseInt::buf_size([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.arr); }) { return (__r.arr); } else if constexpr (requires { (__r.arr_field); }) { return (__r.arr_field); } else if constexpr (requires { ((*__r).arr); }) { return ((*__r).arr); } else { return ((*__r).arr_field); } }(b)[static_cast<size_t>(0)]);
        if (rusty::detail::deref_if_pointer_like(total) > 1) {
            verify(ar.read_exact(varint_tail(&b), rusty::detail::deref_if_pointer_like(total) - 1));
        }
        self_.set(SparseInt::load64(std::move([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.arr); }) { return (__r.arr); } else if constexpr (requires { (__r.arr_field); }) { return (__r.arr_field); } else if constexpr (requires { ((*__r).arr); }) { return ((*__r).arr); } else { return ((*__r).arr_field); } }(b))));
    }

}
// UFCS trait migration: free functions for `impl Deserialize for ...`
namespace Deserialize_ {
    void deserialize(int32_t& self_, BinaryReadArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        // @unsafe
        {
            uint8_t* const p = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>((static_cast<int32_t*>(rusty::detail::ptr_or_addr(self_)))));
            ar.read_or_abort(p, rusty::mem::size_of<int32_t>());
        }
    }

}
// UFCS trait migration: free functions for `impl Deserialize for ...`
namespace Deserialize_ {
    void deserialize(int8_t& self_, BinaryReadArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        // @unsafe
        {
            uint8_t* const p = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>((static_cast<int8_t*>(rusty::detail::ptr_or_addr(self_)))));
            ar.read_or_abort(p, rusty::mem::size_of<int8_t>());
        }
    }

}
// UFCS trait migration: free functions for `impl Deserialize for ...`
namespace Deserialize_ {
    void deserialize(int16_t& self_, BinaryReadArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        // @unsafe
        {
            uint8_t* const p = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>((static_cast<int16_t*>(rusty::detail::ptr_or_addr(self_)))));
            ar.read_or_abort(p, rusty::mem::size_of<int16_t>());
        }
    }

}
// UFCS trait migration: free functions for `impl Deserialize for ...`
namespace Deserialize_ {
    void deserialize(int64_t& self_, BinaryReadArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        // @unsafe
        {
            uint8_t* const p = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>((static_cast<int64_t*>(rusty::detail::ptr_or_addr(self_)))));
            ar.read_or_abort(p, rusty::mem::size_of<int64_t>());
        }
    }

}
// UFCS trait migration: free functions for `impl Deserialize for ...`
namespace Deserialize_ {
    void deserialize(uint8_t& self_, BinaryReadArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        // @unsafe
        {
            uint8_t* const p = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>((static_cast<uint8_t*>(rusty::detail::ptr_or_addr(self_)))));
            ar.read_or_abort(p, rusty::mem::size_of<uint8_t>());
        }
    }

}
// UFCS trait migration: free functions for `impl Deserialize for ...`
namespace Deserialize_ {
    void deserialize(uint16_t& self_, BinaryReadArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        // @unsafe
        {
            uint8_t* const p = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>((static_cast<uint16_t*>(rusty::detail::ptr_or_addr(self_)))));
            ar.read_or_abort(p, rusty::mem::size_of<uint16_t>());
        }
    }

}
// UFCS trait migration: free functions for `impl Deserialize for ...`
namespace Deserialize_ {
    void deserialize(uint32_t& self_, BinaryReadArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        // @unsafe
        {
            uint8_t* const p = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>((static_cast<uint32_t*>(rusty::detail::ptr_or_addr(self_)))));
            ar.read_or_abort(p, rusty::mem::size_of<uint32_t>());
        }
    }

}
// UFCS trait migration: free functions for `impl Deserialize for ...`
namespace Deserialize_ {
    void deserialize(uint64_t& self_, BinaryReadArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        // @unsafe
        {
            uint8_t* const p = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>((static_cast<uint64_t*>(rusty::detail::ptr_or_addr(self_)))));
            ar.read_or_abort(p, rusty::mem::size_of<uint64_t>());
        }
    }

}
// UFCS trait migration: free functions for `impl Deserialize for ...`
namespace Deserialize_ {
    void deserialize(double& self_, BinaryReadArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        // @unsafe
        {
            uint8_t* const p = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>((static_cast<double*>(rusty::detail::ptr_or_addr(self_)))));
            ar.read_or_abort(p, rusty::mem::size_of<double>());
        }
    }

}
// UFCS trait migration: free functions for `impl Deserialize for ...`
namespace Deserialize_ {
    template<typename T1, typename T2>
    void deserialize(std::pair<T1, T2>& self_, BinaryReadArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        Deserialize_::deserialize(self_.first, ar);
        Deserialize_::deserialize(self_.second, ar);
    }

}
// UFCS trait migration: free functions for `impl Deserialize for ...`
namespace Deserialize_ {
    template<typename T>
    void deserialize(rusty::Vec<T>& self_, BinaryReadArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        auto v_len = v64::new_(static_cast<int64_t>(0));
        Deserialize_::deserialize(v_len, ar);
        self_.clear();
        const size_t n = static_cast<size_t>(v_len.get());
        self_.reserve(std::move(n));
        size_t i = static_cast<size_t>(0);
        while (rusty::detail::deref_if_pointer_like(i) < rusty::detail::deref_if_pointer_like(n)) {
            T elem = rusty::default_like<T>();
            Deserialize_::deserialize(elem, ar);
            self_.push(std::move(elem));
            i += static_cast<size_t>(1);
        }
    }

}
// UFCS trait migration: free functions for `impl Deserialize for ...`
namespace Deserialize_ {
    template<typename T>
    void deserialize(std::vector<T>& self_, BinaryReadArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        auto v_len = v64::new_(static_cast<int64_t>(0));
        Deserialize_::deserialize(v_len, ar);
        self_.clear();
        const size_t n = static_cast<size_t>(v_len.get());
        self_.reserve(std::move(n));
        size_t i = static_cast<size_t>(0);
        while (rusty::detail::deref_if_pointer_like(i) < rusty::detail::deref_if_pointer_like(n)) {
            T elem = rusty::default_like<T>();
            Deserialize_::deserialize(elem, ar);
            self_.push_back(std::move(elem));
            i += static_cast<size_t>(1);
        }
    }

}
// UFCS trait migration: free functions for `impl Deserialize for ...`
namespace Deserialize_ {
    template<typename T>
    void deserialize(std::list<T>& self_, BinaryReadArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        auto v_len = v64::new_(static_cast<int64_t>(0));
        Deserialize_::deserialize(v_len, ar);
        self_.clear();
        const size_t n = static_cast<size_t>(v_len.get());
        size_t i = static_cast<size_t>(0);
        while (rusty::detail::deref_if_pointer_like(i) < rusty::detail::deref_if_pointer_like(n)) {
            T elem = rusty::default_like<T>();
            Deserialize_::deserialize(elem, ar);
            self_.push_back(std::move(elem));
            i += static_cast<size_t>(1);
        }
    }

}
// UFCS trait migration: free functions for `impl Deserialize for ...`
namespace Deserialize_ {
    template<typename T>
    void deserialize(rusty::BTreeSet<T>& self_, BinaryReadArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        auto v_len = v64::new_(static_cast<int64_t>(0));
        Deserialize_::deserialize(v_len, ar);
        self_.clear();
        const size_t n = static_cast<size_t>(v_len.get());
        size_t i = static_cast<size_t>(0);
        while (rusty::detail::deref_if_pointer_like(i) < rusty::detail::deref_if_pointer_like(n)) {
            T elem = rusty::default_like<T>();
            Deserialize_::deserialize(elem, ar);
            self_.insert(std::move(elem));
            i += static_cast<size_t>(1);
        }
    }

}
// UFCS trait migration: free functions for `impl Deserialize for ...`
namespace Deserialize_ {
    template<typename T>
    void deserialize(std::set<T>& self_, BinaryReadArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        auto v_len = v64::new_(static_cast<int64_t>(0));
        Deserialize_::deserialize(v_len, ar);
        self_.clear();
        const size_t n = static_cast<size_t>(v_len.get());
        size_t i = static_cast<size_t>(0);
        while (rusty::detail::deref_if_pointer_like(i) < rusty::detail::deref_if_pointer_like(n)) {
            T elem = rusty::default_like<T>();
            Deserialize_::deserialize(elem, ar);
            self_.insert(std::move(elem));
            i += static_cast<size_t>(1);
        }
    }

}
// UFCS trait migration: free functions for `impl Deserialize for ...`
namespace Deserialize_ {
    template<typename T>
    void deserialize(rusty::HashSet<T>& self_, BinaryReadArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        auto v_len = v64::new_(static_cast<int64_t>(0));
        Deserialize_::deserialize(v_len, ar);
        self_.clear();
        const size_t n = static_cast<size_t>(v_len.get());
        size_t i = static_cast<size_t>(0);
        while (rusty::detail::deref_if_pointer_like(i) < rusty::detail::deref_if_pointer_like(n)) {
            T elem = rusty::default_like<T>();
            Deserialize_::deserialize(elem, ar);
            self_.insert(std::move(elem));
            i += static_cast<size_t>(1);
        }
    }

}
// UFCS trait migration: free functions for `impl Deserialize for ...`
namespace Deserialize_ {
    template<typename T>
    void deserialize(std::unordered_set<T>& self_, BinaryReadArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        auto v_len = v64::new_(static_cast<int64_t>(0));
        Deserialize_::deserialize(v_len, ar);
        self_.clear();
        const size_t n = static_cast<size_t>(v_len.get());
        size_t i = static_cast<size_t>(0);
        while (rusty::detail::deref_if_pointer_like(i) < rusty::detail::deref_if_pointer_like(n)) {
            T elem = rusty::default_like<T>();
            Deserialize_::deserialize(elem, ar);
            self_.insert(std::move(elem));
            i += static_cast<size_t>(1);
        }
    }

}
// UFCS trait migration: free functions for `impl Deserialize for ...`
namespace Deserialize_ {
    template<typename K, typename V>
    void deserialize(rusty::BTreeMap<K, V>& self_, BinaryReadArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        auto v_len = v64::new_(static_cast<int64_t>(0));
        Deserialize_::deserialize(v_len, ar);
        self_.clear();
        const size_t n = static_cast<size_t>(v_len.get());
        size_t i = static_cast<size_t>(0);
        while (rusty::detail::deref_if_pointer_like(i) < rusty::detail::deref_if_pointer_like(n)) {
            K key = rusty::default_like<K>();
            V value = rusty::default_like<V>();
            Deserialize_::deserialize(key, ar);
            Deserialize_::deserialize(value, ar);
            self_.insert(std::move(key), std::move(value));
            i += static_cast<size_t>(1);
        }
    }

}
// UFCS trait migration: free functions for `impl Deserialize for ...`
namespace Deserialize_ {
    template<typename K, typename V>
    void deserialize(std::map<K, V>& self_, BinaryReadArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        auto v_len = v64::new_(static_cast<int64_t>(0));
        Deserialize_::deserialize(v_len, ar);
        self_.clear();
        const size_t n = static_cast<size_t>(v_len.get());
        size_t i = static_cast<size_t>(0);
        while (rusty::detail::deref_if_pointer_like(i) < rusty::detail::deref_if_pointer_like(n)) {
            K key = rusty::default_like<K>();
            V value = rusty::default_like<V>();
            Deserialize_::deserialize(key, ar);
            Deserialize_::deserialize(value, ar);
            self_.emplace(std::move(key), std::move(value));
            i += static_cast<size_t>(1);
        }
    }

}
// UFCS trait migration: free functions for `impl Deserialize for ...`
namespace Deserialize_ {
    template<typename K, typename V>
    void deserialize(rusty::HashMap<K, V>& self_, BinaryReadArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        auto v_len = v64::new_(static_cast<int64_t>(0));
        Deserialize_::deserialize(v_len, ar);
        self_.clear();
        const size_t n = static_cast<size_t>(v_len.get());
        size_t i = static_cast<size_t>(0);
        while (rusty::detail::deref_if_pointer_like(i) < rusty::detail::deref_if_pointer_like(n)) {
            K key = rusty::default_like<K>();
            V value = rusty::default_like<V>();
            Deserialize_::deserialize(key, ar);
            Deserialize_::deserialize(value, ar);
            self_.insert(std::move(key), std::move(value));
            i += static_cast<size_t>(1);
        }
    }

}
// UFCS trait migration: free functions for `impl Deserialize for ...`
namespace Deserialize_ {
    template<typename K, typename V>
    void deserialize(std::unordered_map<K, V>& self_, BinaryReadArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        auto v_len = v64::new_(static_cast<int64_t>(0));
        Deserialize_::deserialize(v_len, ar);
        self_.clear();
        const size_t n = static_cast<size_t>(v_len.get());
        size_t i = static_cast<size_t>(0);
        while (rusty::detail::deref_if_pointer_like(i) < rusty::detail::deref_if_pointer_like(n)) {
            K key = rusty::default_like<K>();
            V value = rusty::default_like<V>();
            Deserialize_::deserialize(key, ar);
            Deserialize_::deserialize(value, ar);
            self_.emplace(std::move(key), std::move(value));
            i += static_cast<size_t>(1);
        }
    }

}
/*RUSTYCPP:GEN-END id=serializable.deserialize_trait*/

// ---- Fixed-width primitives. ------------------------------------------
// Each operator>> verifies the read produced sizeof(T) bytes; on
// truncation it aborts via `verify` (matches the existing
// `Marshal::read` contract — short reads at the boundary are
// programming errors at this layer, not recoverable conditions).

// ---- Serde-trait leaf kernels (read side): varints + strings. ----------
namespace Deserialize_ {
inline void deserialize(std::string& self_, BinaryReadArchive& ar) {
  rrr::v64 v_len{0};
  deserialize(v_len, ar);
  auto len = static_cast<size_t>(v_len.get());
  self_.resize(len);
  if (len > 0) {
    // @unsafe { writing into string's internal buffer via &self_[0] }
    verify(ar.read_exact(reinterpret_cast<uint8_t*>(&self_[0]), len));
  }
}
// Generic bridge (read side): mirror of the serialize catch-all.
// Phase 8 endgame: ADL dispatch via poisoned decoy (see the serialize
// catch-all for the full rationale).
namespace adl_detail_ {
void deserialize() = delete;
template<typename T>
inline void dispatch_deserialize(T& v, BinaryReadArchive& ar) {
  deserialize(v, ar);  // ADL-only by construction
}
}  // namespace adl_detail_
template<typename T>
inline void deserialize(T& v, BinaryReadArchive& ar) {
  adl_detail_::dispatch_deserialize(v, ar);
}
}  // namespace Deserialize_

// ---- Variable-length integer encoding (SparseInt). --------------------
// SparseInt's first byte determines the total length; we peek it,
// read the remaining bytes, then decode.


// ---- Variable-length byte sequences. ----------------------------------

// ---- Composites. ------------------------------------------------------
// Phase 8: container/pair serde overloads (operator bodies moved here;
// the operators are now one-line forwarders). Forward declarations first
// so nested containers resolve regardless of definition order; element
// calls are unqualified and fall back to the generic catch-all.
namespace Deserialize_ {
// (pair + container fwd-decls deleted — the trait GEN above declares
// every overload before any use, and its definitions are non-inline.)

// (pair + container deserialize definitions moved into the
// Deserialize trait block above — impl-for-container lowering lands
// directly in this namespace; the forward declarations above remain
// for nested-container resolution.)


}  // namespace Deserialize_


// ---- Linear containers. -----------------------------------------------
// Wire format: v64 length prefix + N elements deserialized in order.
// Containers are cleared first; matches the existing Marshal operator>>
// semantics.












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
class SerializableBase;

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

using SerializableProxy = rusty::Arc<SerializableBase>;

namespace details {

// Wrapper used to put an `Arc<T>` inside a SerializableProxy.
// Authored as generic inline Rust DSL with #[cpp_inherit]
// (SerializableBase is a DSL interface trait). Behavioral diffs:
//   * The unused default ctor (eager-construct) is dropped — every
//     construction site adopts an existing Arc<T> via the synthesized
//     fieldwise ctor.
//   * rusty::Arc IS in the transpiler's auto-deref set, so save/kind
//     dispatch directly through the field; only load keeps a kernel
//     (Arc grants const-only access — T::load needs the unique-owner
//     get_mut() escape, which has no DSL spelling).
#if RUSTYCPP_RUST
struct SerializableSharedPtrHolder<T> {
    ptr: Arc<T>,
}

#[cpp_inherit]
impl<T> SerializableBase for SerializableSharedPtrHolder<T> {
    fn save(&self, ar: &mut BinaryWriteArchive) {
        self.ptr.save(ar)
    }
    fn load(&mut self, ar: &mut BinaryReadArchive) {
        holder_load(self, ar)
    }
    fn kind(&self) -> i32 {
        self.ptr.kind()
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=serializable.shared_ptr_holder version=1 rust_sha256=352273da3183f75aa659c5868747306f95efc1770fdfa66b4c32f8843fac426b*/
template<typename T>
struct SerializableSharedPtrHolder;

template<typename T>
struct SerializableSharedPtrHolder : public SerializableBase {
    rusty::Arc<T> ptr;
    SerializableSharedPtrHolder(rusty::Arc<T> ptr_init) : SerializableBase(), ptr(std::move(ptr_init)) {}
    SerializableSharedPtrHolder(SerializableSharedPtrHolder&& other) noexcept : SerializableBase(), ptr(std::move(other.ptr)) {}


    void save(BinaryWriteArchive& ar) const {
        this->ptr->save(ar);
    }
    void load(BinaryReadArchive& ar) {
        holder_load((*this), ar);
    }
    int32_t kind() const {
        return this->ptr->kind();
    }
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = rusty::is_send<T>::value && rusty::is_sync<T>::value;
    static constexpr bool is_sync = rusty::is_send<T>::value && rusty::is_sync<T>::value;
};
/*RUSTYCPP:GEN-END id=serializable.shared_ptr_holder*/

// @unsafe - unique-owner mutation window: load always runs on a
// factory-fresh proxy (registry create -> strong_count 1), so
// get_mut() is Some; a shared proxy here would be a bug and panics
// loudly instead of silently mutating shared state.
template<typename T>
inline void holder_load(SerializableSharedPtrHolder<T>& self,
                        BinaryReadArchive& ar) {
  self.ptr.get_mut().unwrap().load(ar);
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
  auto sp = rusty::Arc<T>::make(std::forward<Args>(args)...);
  return rusty::Arc<details::SerializableSharedPtrHolder<T>>::make(std::move(sp));
}

// Factory registry: maps int32_t kind tags to factories that produce
// fresh SerializableProxy instances. Authored as inline Rust DSL
// (statics on an empty struct; the generic reg<T> delegates to a
// template free fn — T is non-deducible, so it is declared before the
// GEN block). The factory-map singleton + rusty::Mutex live in the impl
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
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
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
    // unpack_shared<T> a refcount-shared Arc<T>.
    auto sp = rusty::Arc<T>::make();
    return rusty::Arc<details::SerializableSharedPtrHolder<T>>::make(
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
// rusty::Mutex<SerializableRegistryMap> guard.
namespace rrr {

namespace {

// `SerializableRegistryMap` — TU-local POD wrapping the single
// `HashMap<i32, Factory>` the rusty::Mutex guards. Mirrors the shape of
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
rusty::Mutex<SerializableRegistryMap>& registry() {
  // rusty::Mutex has no default ctor (unlike the retired SpinMutex), so seed
  // it with an empty registry map explicitly.
  static rusty::Mutex<SerializableRegistryMap> r{SerializableRegistryMap{}};
  return r;
}

}  // namespace

// Registry impls, authored as inline Rust DSL over the anon-namespace
// registry() singleton (same shape as any_message's registry queries).
#if RUSTYCPP_RUST
fn serializable_registry_register_factory(kind: i32, factory: SerializableRegistryFactory) {
    let mut guard = registry().lock().unwrap();
    (*guard).map.insert(kind, factory);
}

fn serializable_registry_create_impl(kind: i32) -> SerializableProxy {
    let mut guard = registry().lock().unwrap();
    let entry = (*guard).map.get(kind);
    verify(entry.is_some());
    entry.unwrap()()
}

fn serializable_registry_is_registered_impl(kind: i32) -> bool {
    let guard = registry().lock().unwrap();
    (*guard).map.get(kind).is_some()
}

fn serializable_registry_clear_impl() {
    let mut guard = registry().lock().unwrap();
    (*guard).map.clear();
}
#endif
/*RUSTYCPP:GEN-BEGIN id=serializable.registry_impls version=1 rust_sha256=bfb02e6edc144116e0b940d4d17814b3bdc58228c17da55929ce3e441e26ecbf*/
bool serializable_registry_is_registered_impl(int32_t kind);
void serializable_registry_clear_impl();

void serializable_registry_register_factory(int32_t kind, SerializableRegistryFactory factory) {
    auto&& guard = rusty::deref_call(registry().lock(), rusty::detail::__mdisp_unwrap{});
    (rusty::detail::deref_if_pointer_like(guard)).map.insert(std::move(kind), std::move(factory));
}

SerializableProxy serializable_registry_create_impl(int32_t kind) {
    auto&& guard = rusty::deref_call(registry().lock(), rusty::detail::__mdisp_unwrap{});
    auto entry = (rusty::detail::deref_if_pointer_like(guard)).map.get(std::move(kind));
    verify(entry.is_some());
    return entry.unwrap()();
}

bool serializable_registry_is_registered_impl(int32_t kind) {
    const auto&& guard = rusty::deref_call(registry().lock(), rusty::detail::__mdisp_unwrap{});
    return (rusty::detail::deref_if_pointer_like(guard)).map.get(std::move(kind)).is_some();
}

void serializable_registry_clear_impl() {
    auto&& guard = rusty::deref_call(registry().lock(), rusty::detail::__mdisp_unwrap{});
    (rusty::detail::deref_if_pointer_like(guard)).map.clear();
}
/*RUSTYCPP:GEN-END id=serializable.registry_impls*/

}  // namespace rrr
