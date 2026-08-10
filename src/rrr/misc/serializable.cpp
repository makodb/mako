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
        //
        // The `sink_span` hand-bridge that used to sit above this block —
        // "the SinkBase trait hands write_bytes a raw (ptr, len) pair, and
        // the DSL cannot build a span from one" — was STALE.
        // `core::slice::from_raw_parts` lowers to `rusty::from_raw_parts(p,
        // n)`, whose body IS `std::span<const uint8_t>(p, n)`: the exact
        // expression the kernel spelled by hand. The same shape already
        // ships in GEN blocks inmemory_channel.14 and fiber_channel.3.
        //
        // @unsafe - builds a borrowed `&[u8]` over the caller's raw
        // pointer. Inherent boundary: the SinkBase contract pins those
        // bytes for the duration of the call.
        self.bytes.extend_from_slice(unsafe { core::slice::from_raw_parts(p, n) });
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=serializable.buffer_sink version=1 rust_sha256=3660992d9b625de489038e04d8d1276744e20ed726fb0a4d6d3f118510349c61*/
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
    this->bytes.extend_from_slice(rusty::from_raw_parts(p, n));
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
// `read(void*, size_t)` used to live OUTSIDE the DSL block as the free
// function `buffer_source_read`; that note ("the body's `void*` parameter
// and `memcpy` aren't expressible in inline-Rust") was stale. The trait
// already hands `read_bytes` a typed `*mut u8`, so there is no `void*`,
// and `core::ptr::copy_nonoverlapping` lowers to
// `rusty::ptr::copy_nonoverlapping` -- a real memcpy, no perf loss.
// The existing 2-arg paren-init form `BufferSource src(data, len)` keeps
// working via C++20 aggregate paren-init; callers can also use the DSL
// `fn new` factory directly (`BufferSource::new_(data, len)`).
struct BufferSource;
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
        let avail: usize = self.len_ - self.pos_;
        let mut take: usize = n;
        if avail < take {
            take = avail;
        }
        if take > 0usize {
            unsafe {
                core::ptr::copy_nonoverlapping(self.data_.add(self.pos_), p, take);
            }
            self.pos_ = self.pos_ + take;
        }
        take
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=serializable.buffer_source version=1 rust_sha256=6817e70aab631e2484d9b974101a347f9c17c72f34dc9e32ef18dee773b0a339*/
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
    size_t avail = rusty::detail::deref_if_pointer_like(this->len_) - rusty::detail::deref_if_pointer_like(this->pos_);
    size_t take = n;
    if (rusty::detail::deref_if_pointer_like(avail) < rusty::detail::deref_if_pointer_like(take)) {
        take = std::move(avail);
    }
    if (rusty::detail::deref_if_pointer_like(take) > static_cast<size_t>(0)) {
        // @unsafe
        {
            rusty::ptr::copy_nonoverlapping(rusty::ptr::add(this->data_, this->pos_), p, std::move(take));
        }
        this->pos_ = rusty::detail::deref_if_pointer_like(this->pos_) + rusty::detail::deref_if_pointer_like(take);
    }
    return std::move(take);
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

// (The former `buffer_source_read` kernel is now the DSL `read_bytes` above.)

// Adapter wrappers for the SinkBase / SourceBase virtual bases.
//
// Adapters wrap a non-owning raw pointer to the concrete sink/source
// and forward `write` / `read` through it.
//
// Lifetime: the proxy must not outlive `*sink` / `*source`.
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block.
//
// Two spellings here are LOAD-BEARING:
//
//   * The erasure is spelled EXPLICITLY as `rusty::make_box` with a
//     turbofish naming the *RefMut* adapter. A plain `Box::new(sink)`
//     over a borrow lowers to the OWNING `SinkBaseAdapter<BufferSink>`,
//     which would COPY the sink — every byte written through the proxy
//     would land in the copy and the caller's `BufferSink::bytes` would
//     stay empty. There is no borrowed-trait-object coercion in the
//     DSL, so the adapter must be named.
//
//   * The parameter is a RAW POINTER (`*mut BufferSink`), not `&mut`,
//     so the emitted signature stays exactly
//     `make_sink_proxy(BufferSink*)` — byte-identical to what the ~369
//     `make_*_proxy(&x)` call sites already spell (including the DSL
//     callers in client.cpp / server.cpp, which pass `&raw mut x`).
//
// The FdSink / FdSource overloads of these same two names live in a
// SECOND DSL block further down, beside FdSink/FdSource: Rust has no
// function overloading, so they cannot share this block — but each
// block emits a plain C++ free function, and those overload normally.
#if RUSTYCPP_RUST
fn make_sink_proxy(sink: *mut BufferSink) -> Box<SinkBase> {
    rusty::make_box::<SinkBaseAdapterRefMut<BufferSink>>(*sink)
}

fn make_source_proxy(source: *mut BufferSource) -> Box<SourceBase> {
    rusty::make_box::<SourceBaseAdapterRefMut<BufferSource>>(*source)
}
#endif
/*RUSTYCPP:GEN-BEGIN id=serializable.5 version=1 rust_sha256=5397c9e0938504bb3cb5d7c11106a3f80c35af60fd8f55ce6eed62b8f4c03fe7*/
rusty::Box<SinkBase> make_sink_proxy(BufferSink* sink) {
    return rusty::make_box<SinkBaseAdapterRefMut<BufferSink>>(*sink);
}

rusty::Box<SourceBase> make_source_proxy(BufferSource* source) {
    return rusty::make_box<SourceBaseAdapterRefMut<BufferSource>>(*source);
}
/*RUSTYCPP:GEN-END id=serializable.5*/

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
// The EINTR/short-I/O loops live in the plain-C srpc_io.c kernels. The
// DSL methods call those kernels directly; their concrete u8 pointers
// convert to the C ABI's void pointers without a C++ bridge.
// The DSL `fn new(fd: i32)` factory keeps the existing 1-arg paren-init
// form working via C++20 aggregate paren-init.
struct FdSink;
extern "C" void srpc_fd_write_all(int fd, const void* p, size_t n);
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
        srpc_fd_write_all(self.fd_, p, n);
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=serializable.fd_sink version=1 rust_sha256=eced1cd615ad7cc6998b698f2b352f0885da87c59429e5ced182b64019329aea*/
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
    srpc_fd_write_all(this->fd_, p, std::move(n));
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

struct FdSource;
extern "C" size_t srpc_fd_read_upto(int fd, void* p, size_t n);
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
        srpc_fd_read_upto(self.fd_, p, n)
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=serializable.fd_source version=1 rust_sha256=fd9f9a4dacdad557c7699900c24f31074538e95dce5f0fc13a0f779adde7e124*/
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
    return srpc_fd_read_upto(this->fd_, p, std::move(n));
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

// The FdSink / FdSource half of the make_*_proxy overload set — same
// DSL spelling as the BufferSink / BufferSource block above
// (raw-pointer parameter + explicit
// `rusty::make_box::<...BaseAdapterRefMut<T>>` erasure, both
// load-bearing for the same reasons documented there).
//
// This is a SEPARATE `#if RUSTYCPP_RUST` block purely because Rust has
// no function overloading and these two fns reuse the names above. The
// transpiler ids and hashes blocks independently, so two blocks in one
// file may define same-named fns; each emits an ordinary C++ free
// function and the four overload exactly as they did by hand.
#if RUSTYCPP_RUST
fn make_sink_proxy(sink: *mut FdSink) -> Box<SinkBase> {
    rusty::make_box::<SinkBaseAdapterRefMut<FdSink>>(*sink)
}

fn make_source_proxy(source: *mut FdSource) -> Box<SourceBase> {
    rusty::make_box::<SourceBaseAdapterRefMut<FdSource>>(*source)
}
#endif
/*RUSTYCPP:GEN-BEGIN id=serializable.8 version=1 rust_sha256=41b4f1fc68626277166cf5e6af3c0eb1f41bf63b7655f7ac5521f5b3a9c7cbce*/
rusty::Box<SinkBase> make_sink_proxy(FdSink* sink) {
    return rusty::make_box<SinkBaseAdapterRefMut<FdSink>>(*sink);
}

rusty::Box<SourceBase> make_source_proxy(FdSource* source) {
    return rusty::make_box<SourceBaseAdapterRefMut<FdSource>>(*source);
}
/*RUSTYCPP:GEN-END id=serializable.8*/

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
    // @unsafe - virtual write through the type-erased sink proxy.
    // The explicit `(*self.sink_)` deref is LOAD-BEARING: SinkProxy is a
    // hand-written C++ alias, so the transpiler cannot see the Box
    // behind it and a bare `self.sink_.write_bytes(..)` lowers to a `.`
    // member access on the handle (which does not compile). The deref
    // lowers through rusty::detail::deref_if_pointer_like, i.e. exactly
    // the `(*sink_).write_bytes(..)` the old kernel spelled `sink_->`.
    fn write_bytes(&mut self, p: *const u8, n: usize) {
        (*self.sink_).write_bytes(p, n)
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=serializable.write_archive version=1 rust_sha256=e49ee402ee8c5c3907a942a481a9883606b167645f256c5a8999bbddf42fcb6e*/
struct BinaryWriteArchive;

struct BinaryWriteArchive {
    SinkProxy sink_;

    void write_bytes(const uint8_t* p, size_t n);
};


void BinaryWriteArchive::write_bytes(const uint8_t* p, size_t n) {
    ((rusty::detail::deref_if_pointer_like(this->sink_))).write_bytes(p, std::move(n));
}
/*RUSTYCPP:GEN-END id=serializable.write_archive*/

// The v32/v64 leaf impls' 9-byte varint scratch used to live in a
// hand-written `struct VarintBuf { uint8_t arr[9]; }` POD, because "the
// DSL has no local arrays, so the buffer lives in a plain C++ POD whose
// C-array field decays to uint8_t* at the DSL call sites". That is now
// STALE: `let mut b: [u8; 9] = [0u8; 9];` lowers to a zero-filled
// `std::array<uint8_t, 9>`, and `b.as_mut_ptr()` / `b.as_ptr()` lower to
// `rusty::as_mut_ptr(b)` / `rusty::as_ptr(b)` — the same `uint8_t*` /
// `const uint8_t*` the C array decayed to. `varint_tail` becomes
// `unsafe { b.as_mut_ptr().add(1) }` -> `rusty::ptr::add(...)`.
// VarintBuf, varint_buf_new and varint_tail are all gone; the scratch is
// a DSL local at each of the four leaf sites below.
//
// NB the tracker's own suggested DSL (`pub struct VarintBuf { arr: [u8; 9] }`)
// is a TRAP and was rejected: as a DSL struct field the array lowers to a
// `std::array` that does NOT decay, breaking all four `b.arr` call sites
// while adding nothing. Deleting the struct is strictly better.

// ---- Serde-style Serialize trait (wire migration). --------------------
// Value-side serialization: each type implements how to write itself into
// a BinaryWriteArchive. Lowers to a UFCS free fn
// `Serialize_::serialize(const T&, BinaryWriteArchive&)` (static dispatch by
// overload, no orphan rule, impl-in-own-file). The `operator<<` overloads
// below forward here, so the byte kernel lives in exactly one place and
// byte-compat is automatic during the operator→trait coexistence.
// All generic Serialize impl declarations must precede every generated body.
// The trait block below supplies the concrete container overload declarations;
// this one declaration supplies the ADL fallback for arbitrary user types.
namespace Serialize_ {
template<class T>
void serialize(const T& v, BinaryWriteArchive& ar);
}
#if RUSTYCPP_RUST
pub trait Serialize {
    fn serialize(&self, ar: &mut BinaryWriteArchive);
}
impl Serialize for v32 {
    fn serialize(&self, ar: &mut BinaryWriteArchive) {
        let mut b: [u8; 9] = [0u8; 9];
        let bsize = SparseInt::dump32(self.get(), b.as_mut_ptr());
        ar.write_bytes(b.as_ptr(), bsize);
    }
}
impl Serialize for v64 {
    fn serialize(&self, ar: &mut BinaryWriteArchive) {
        let mut b: [u8; 9] = [0u8; 9];
        let bsize = SparseInt::dump64(self.get(), b.as_mut_ptr());
        ar.write_bytes(b.as_ptr(), bsize);
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

// ---- Variable-length byte sequences: v64 length prefix + raw bytes.
// BOTH leaves carry the body (rather than std::string forwarding to a
// std::string_view temporary, as the old hand pair did): a
// `std::string_view{self}` conversion has no DSL spelling. The wire
// bytes are identical either way.
//
// `self.data() as *const u8` lowers to
// rusty::detail::ptr_cast<const uint8_t*>, replacing the hand
// reinterpret_cast. The length write MUST be QUALIFIED — unqualified
// lookup inside the generated namespace finds the sibling it just
// emitted and stops (the same hazard the container impls below avoid).
//
// Only behavioural delta vs. the deleted hand overloads: string_view
// goes from by-value to `const std::string_view&`; rvalues bind to that
// reference with identical semantics.
impl Serialize for std::string_view {
    fn serialize(&self, ar: &mut BinaryWriteArchive) {
        let v_len: rrr::v64 = rrr::v64::new(self.size() as i64);
        Serialize_::serialize(v_len, ar);
        if self.size() > 0usize {
            let p: *const u8 = self.data() as *const u8;
            ar.write_bytes(p, self.size());
        }
    }
}
impl Serialize for std::string {
    fn serialize(&self, ar: &mut BinaryWriteArchive) {
        let v_len: rrr::v64 = rrr::v64::new(self.size() as i64);
        Serialize_::serialize(v_len, ar);
        if self.size() > 0usize {
            let p: *const u8 = self.data() as *const u8;
            ar.write_bytes(p, self.size());
        }
    }
}
impl<T> Serialize for std::list<T> {
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
// whole container and landed on the poisoned catch-all).
impl<T> Serialize for Vec<T> {
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

impl<T> Serialize for std::vector<T> {
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

impl<T> Serialize for std::set<T> {
    fn serialize(&self, ar: &mut BinaryWriteArchive) {
        let v_len: rrr::v64 = rrr::v64::new(self.size() as i64);
        Serialize_::serialize(v_len, ar);
        for e in self {
            Serialize_::serialize(e, ar);
        }
    }
}

impl<T> Serialize for std::unordered_set<T> {
    fn serialize(&self, ar: &mut BinaryWriteArchive) {
        let v_len: rrr::v64 = rrr::v64::new(self.size() as i64);
        Serialize_::serialize(v_len, ar);
        for e in self {
            Serialize_::serialize(e, ar);
        }
    }
}

impl<K, V> Serialize for std::map<K, V> {
    fn serialize(&self, ar: &mut BinaryWriteArchive) {
        let v_len: rrr::v64 = rrr::v64::new(self.size() as i64);
        Serialize_::serialize(v_len, ar);
        for kv in self {
            Serialize_::serialize(kv.first, ar);
            Serialize_::serialize(kv.second, ar);
        }
    }
}

impl<K, V> Serialize for std::unordered_map<K, V> {
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
impl<T> Serialize for rusty::BTreeSet<T> {
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

impl<K, V> Serialize for rusty::BTreeMap<K, V> {
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

// The two hashbrown write bodies, same explicit-iterator shape as the
// B-tree pair above. HashSet has no const begin()/end() of its own, so
// it walks the underlying HashMap field: `self.map.iter()` lowers to
// `rusty::iter(self_.map)`, whose next() yields
// Option<tuple<const T&, const monostate&>> — hence the `kv.0`.
//
// WARNING (unchanged by this conversion): ANY hashbrown enumeration
// (iter()/begin()/drain()) routes through the `rusty::iter(table)`
// dispatcher in slice.hpp, whose return-type name crashes clang-22's
// Itanium mangler (SIGSEGV in mangleSourceName). These two templates
// MUST therefore stay UNINSTANTIATED — no production code serializes a
// rusty::HashSet/HashMap today, and the DECODER side (insert-only) is
// crash-free and is what the RustyHashSetPrimitives /
// RustyHashMapPrimitives tests exercise. If that ever changes, the
// encoder needs a mangler-safe enumeration path (or a fixed toolchain).
impl<T> Serialize for rusty::HashSet<T> {
    fn serialize(&self, ar: &mut BinaryWriteArchive) {
        let v_len: rrr::v64 = rrr::v64::new(self.len() as i64);
        Serialize_::serialize(v_len, ar);
        let mut it = self.map.iter();
        loop {
            let e = it.next();
            if e.is_none() {
                break;
            }
            let kv = e.unwrap();
            Serialize_::serialize(kv.0, ar);
        }
    }
}

impl<K, V> Serialize for rusty::HashMap<K, V> {
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

// std::pair: write first then second, no length prefix (each side
// already knows the type and consumes its own bytes). It stays last in
// the trait block; codegen emits every Serialize_ overload declaration
// before any definition, so both element calls see the complete set.
impl<T1, T2> Serialize for std::pair<T1, T2> {
    fn serialize(&self, ar: &mut BinaryWriteArchive) {
        Serialize_::serialize(self.first, ar);
        Serialize_::serialize(self.second, ar);
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=serializable.serialize_trait version=1 rust_sha256=5d3ef56b43300c71459919ff087e1c50aa5c0260c4d85e694559672482aeff6f*/
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

    void serialize(const std::string_view& self_, BinaryWriteArchive& ar);

    void serialize(const std::string& self_, BinaryWriteArchive& ar);

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

    template<typename T>
    void serialize(const rusty::HashSet<T>& self_, BinaryWriteArchive& ar);

    template<typename K, typename V>
    void serialize(const rusty::HashMap<K, V>& self_, BinaryWriteArchive& ar);

    template<typename T1, typename T2>
    void serialize(const std::pair<T1, T2>& self_, BinaryWriteArchive& ar);

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
namespace Serialize_ {
    void serialize(const std::string_view& self_, BinaryWriteArchive& ar);
}
using namespace Serialize_;
namespace Serialize_ {
    void serialize(const std::string& self_, BinaryWriteArchive& ar);
}
using namespace Serialize_;
namespace Serialize_ {
    template<typename T>
    void serialize(const std::list<T>& self_, BinaryWriteArchive& ar);
}
using namespace Serialize_;
namespace Serialize_ {
    template<typename T>
    void serialize(const rusty::Vec<T>& self_, BinaryWriteArchive& ar);
}
using namespace Serialize_;
namespace Serialize_ {
    template<typename T>
    void serialize(const std::vector<T>& self_, BinaryWriteArchive& ar);
}
using namespace Serialize_;
namespace Serialize_ {
    template<typename T>
    void serialize(const std::set<T>& self_, BinaryWriteArchive& ar);
}
using namespace Serialize_;
namespace Serialize_ {
    template<typename T>
    void serialize(const std::unordered_set<T>& self_, BinaryWriteArchive& ar);
}
using namespace Serialize_;
namespace Serialize_ {
    template<typename K, typename V>
    void serialize(const std::map<K, V>& self_, BinaryWriteArchive& ar);
}
using namespace Serialize_;
namespace Serialize_ {
    template<typename K, typename V>
    void serialize(const std::unordered_map<K, V>& self_, BinaryWriteArchive& ar);
}
using namespace Serialize_;
namespace Serialize_ {
    template<typename T>
    void serialize(const rusty::BTreeSet<T>& self_, BinaryWriteArchive& ar);
}
using namespace Serialize_;
namespace Serialize_ {
    template<typename K, typename V>
    void serialize(const rusty::BTreeMap<K, V>& self_, BinaryWriteArchive& ar);
}
using namespace Serialize_;
namespace Serialize_ {
    template<typename T>
    void serialize(const rusty::HashSet<T>& self_, BinaryWriteArchive& ar);
}
using namespace Serialize_;
namespace Serialize_ {
    template<typename K, typename V>
    void serialize(const rusty::HashMap<K, V>& self_, BinaryWriteArchive& ar);
}
using namespace Serialize_;
namespace Serialize_ {
    template<typename T1, typename T2>
    void serialize(const std::pair<T1, T2>& self_, BinaryWriteArchive& ar);
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

// trait impl for `std::string_view` lowered via the Serialize_ free functions above

// trait impl for `std::string` lowered via the Serialize_ free functions above

// trait impl for `std::list` lowered via the Serialize_ free functions above

// trait impl for `Vec` lowered via the Serialize_ free functions above

// trait impl for `std::vector` lowered via the Serialize_ free functions above

// trait impl for `std::set` lowered via the Serialize_ free functions above

// trait impl for `std::unordered_set` lowered via the Serialize_ free functions above

// trait impl for `std::map` lowered via the Serialize_ free functions above

// trait impl for `std::unordered_map` lowered via the Serialize_ free functions above

// trait impl for `rusty::BTreeSet` lowered via the Serialize_ free functions above

// trait impl for `rusty::BTreeMap` lowered via the Serialize_ free functions above

// trait impl for `rusty::HashSet` lowered via the Serialize_ free functions above

// trait impl for `rusty::HashMap` lowered via the Serialize_ free functions above

// trait impl for `std::pair` lowered via the Serialize_ free functions above

// Extension trait Serialize lowered to rusty_ext:: free functions
namespace rusty_ext {
    void serialize(const v32& self_, BinaryWriteArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        std::array<uint8_t, 9> b = [](auto _seed) { std::array<uint8_t, 9> _repeat{}; _repeat.fill(static_cast<uint8_t>(_seed)); return _repeat; }(static_cast<uint8_t>(0));
        const auto bsize = SparseInt::dump32(self_.get(), rusty::as_mut_ptr(b));
        ar.write_bytes(rusty::as_ptr(b), std::move(bsize));
    }

    void serialize(const v64& self_, BinaryWriteArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        std::array<uint8_t, 9> b = [](auto _seed) { std::array<uint8_t, 9> _repeat{}; _repeat.fill(static_cast<uint8_t>(_seed)); return _repeat; }(static_cast<uint8_t>(0));
        const auto bsize = SparseInt::dump64(self_.get(), rusty::as_mut_ptr(b));
        ar.write_bytes(rusty::as_ptr(b), std::move(bsize));
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

    void serialize(const std::string_view& self_, BinaryWriteArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        rrr::v64 v_len = rrr::v64::new_(static_cast<int64_t>(self_.size()));
        Serialize_::serialize(v_len, ar);
        if (self_.size() > static_cast<size_t>(0)) {
            const uint8_t* p = rusty::detail::ptr_cast<const uint8_t*>(self_.data());
            ar.write_bytes(p, self_.size());
        }
    }

    void serialize(const std::string& self_, BinaryWriteArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        rrr::v64 v_len = rrr::v64::new_(static_cast<int64_t>(self_.size()));
        Serialize_::serialize(v_len, ar);
        if (self_.size() > static_cast<size_t>(0)) {
            const uint8_t* p = rusty::detail::ptr_cast<const uint8_t*>(self_.data());
            ar.write_bytes(p, self_.size());
        }
    }

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

    template<typename T>
    void serialize(const rusty::HashSet<T>& self_, BinaryWriteArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        rrr::v64 v_len = rrr::v64::new_(static_cast<int64_t>(rusty::len(self_)));
        Serialize_::serialize(v_len, ar);
        auto it = rusty::iter(self_.map);
        while (true) {
            auto e = it.next();
            if (e.is_none()) {
                break;
            }
            const auto kv = e.unwrap();
            Serialize_::serialize(rusty::detail::deref_if_pointer(([](auto&& __t) -> decltype(auto) { if constexpr (requires { __t._0; }) return (std::forward<decltype(__t)>(__t)._0); else if constexpr (requires { std::get<0>(std::forward<decltype(__t)>(__t)); }) return std::get<0>(std::forward<decltype(__t)>(__t)); else if constexpr (requires { (*__t)._0; }) return ((*std::forward<decltype(__t)>(__t))._0); else return std::get<0>(*std::forward<decltype(__t)>(__t)); })(kv)), ar);
        }
    }

    template<typename K, typename V>
    void serialize(const rusty::HashMap<K, V>& self_, BinaryWriteArchive& ar) {
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

    template<typename T1, typename T2>
    void serialize(const std::pair<T1, T2>& self_, BinaryWriteArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        Serialize_::serialize(self_.first, ar);
        Serialize_::serialize(self_.second, ar);
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

template <>
class SerializeAdapter<std::string_view> final : public Serialize {
    std::string_view value_;
public:
    SerializeAdapter(std::string_view v) : value_(std::move(v)) {}
    SerializeAdapter(SerializeAdapter&& other) : value_(std::move(other.value_)) {}
    void serialize(BinaryWriteArchive& ar) const override {
        rusty_ext::serialize(value_, ar);
    }
};

template <>
class SerializeAdapterRef<std::string_view> final : public Serialize {
    const std::string_view& value_;
public:
    explicit SerializeAdapterRef(const std::string_view& u) : value_(u) {}
    void serialize(BinaryWriteArchive& ar) const override {
        rusty_ext::serialize(value_, ar);
    }
};

template <>
class SerializeAdapterRefMut<std::string_view> final : public Serialize {
    std::string_view& value_;
public:
    explicit SerializeAdapterRefMut(std::string_view& u) : value_(u) {}
    void serialize(BinaryWriteArchive& ar) const override {
        rusty_ext::serialize(value_, ar);
    }
};

template <>
class SerializeAdapter<std::string> final : public Serialize {
    std::string value_;
public:
    SerializeAdapter(std::string v) : value_(std::move(v)) {}
    SerializeAdapter(SerializeAdapter&& other) : value_(std::move(other.value_)) {}
    void serialize(BinaryWriteArchive& ar) const override {
        rusty_ext::serialize(value_, ar);
    }
};

template <>
class SerializeAdapterRef<std::string> final : public Serialize {
    const std::string& value_;
public:
    explicit SerializeAdapterRef(const std::string& u) : value_(u) {}
    void serialize(BinaryWriteArchive& ar) const override {
        rusty_ext::serialize(value_, ar);
    }
};

template <>
class SerializeAdapterRefMut<std::string> final : public Serialize {
    std::string& value_;
public:
    explicit SerializeAdapterRefMut(std::string& u) : value_(u) {}
    void serialize(BinaryWriteArchive& ar) const override {
        rusty_ext::serialize(value_, ar);
    }
};

// TODO(interface_traits): skipped generic impl `SerializeAdapter<std::list<T>>`
// TODO(interface_traits): skipped generic impl `SerializeAdapter<rusty::Vec<T>>`
// TODO(interface_traits): skipped generic impl `SerializeAdapter<std::vector<T>>`
// TODO(interface_traits): skipped generic impl `SerializeAdapter<std::set<T>>`
// TODO(interface_traits): skipped generic impl `SerializeAdapter<std::unordered_set<T>>`
// TODO(interface_traits): skipped generic impl `SerializeAdapter<std::map<K, V>>`
// TODO(interface_traits): skipped generic impl `SerializeAdapter<std::unordered_map<K, V>>`
// TODO(interface_traits): skipped generic impl `SerializeAdapter<rusty::BTreeSet<T>>`
// TODO(interface_traits): skipped generic impl `SerializeAdapter<rusty::BTreeMap<K, V>>`
// TODO(interface_traits): skipped generic impl `SerializeAdapter<rusty::HashSet<T>>`
// TODO(interface_traits): skipped generic impl `SerializeAdapter<rusty::HashMap<K, V>>`
// TODO(interface_traits): skipped generic impl `SerializeAdapter<std::pair<T1, T2>>`

// UFCS trait migration: free functions for `impl Serialize for ...`
namespace Serialize_ {
    void serialize(const v32& self_, BinaryWriteArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        std::array<uint8_t, 9> b = [](auto _seed) { std::array<uint8_t, 9> _repeat{}; _repeat.fill(static_cast<uint8_t>(_seed)); return _repeat; }(static_cast<uint8_t>(0));
        const auto bsize = SparseInt::dump32(self_.get(), rusty::as_mut_ptr(b));
        ar.write_bytes(rusty::as_ptr(b), std::move(bsize));
    }

}
// UFCS trait migration: free functions for `impl Serialize for ...`
namespace Serialize_ {
    void serialize(const v64& self_, BinaryWriteArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        std::array<uint8_t, 9> b = [](auto _seed) { std::array<uint8_t, 9> _repeat{}; _repeat.fill(static_cast<uint8_t>(_seed)); return _repeat; }(static_cast<uint8_t>(0));
        const auto bsize = SparseInt::dump64(self_.get(), rusty::as_mut_ptr(b));
        ar.write_bytes(rusty::as_ptr(b), std::move(bsize));
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
// UFCS trait migration: free functions for `impl Serialize for ...`
namespace Serialize_ {
    void serialize(const std::string_view& self_, BinaryWriteArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        rrr::v64 v_len = rrr::v64::new_(static_cast<int64_t>(self_.size()));
        Serialize_::serialize(v_len, ar);
        if (self_.size() > static_cast<size_t>(0)) {
            const uint8_t* p = rusty::detail::ptr_cast<const uint8_t*>(self_.data());
            ar.write_bytes(p, self_.size());
        }
    }

}
// UFCS trait migration: free functions for `impl Serialize for ...`
namespace Serialize_ {
    void serialize(const std::string& self_, BinaryWriteArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        rrr::v64 v_len = rrr::v64::new_(static_cast<int64_t>(self_.size()));
        Serialize_::serialize(v_len, ar);
        if (self_.size() > static_cast<size_t>(0)) {
            const uint8_t* p = rusty::detail::ptr_cast<const uint8_t*>(self_.data());
            ar.write_bytes(p, self_.size());
        }
    }

}
// UFCS trait migration: free functions for `impl Serialize for ...`
namespace Serialize_ {
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
// UFCS trait migration: free functions for `impl Serialize for ...`
namespace Serialize_ {
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
// UFCS trait migration: free functions for `impl Serialize for ...`
namespace Serialize_ {
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
// UFCS trait migration: free functions for `impl Serialize for ...`
namespace Serialize_ {
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
// UFCS trait migration: free functions for `impl Serialize for ...`
namespace Serialize_ {
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
// UFCS trait migration: free functions for `impl Serialize for ...`
namespace Serialize_ {
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
// UFCS trait migration: free functions for `impl Serialize for ...`
namespace Serialize_ {
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
// UFCS trait migration: free functions for `impl Serialize for ...`
namespace Serialize_ {
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
// UFCS trait migration: free functions for `impl Serialize for ...`
namespace Serialize_ {
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
// UFCS trait migration: free functions for `impl Serialize for ...`
namespace Serialize_ {
    template<typename T>
    void serialize(const rusty::HashSet<T>& self_, BinaryWriteArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        rrr::v64 v_len = rrr::v64::new_(static_cast<int64_t>(rusty::len(self_)));
        Serialize_::serialize(v_len, ar);
        auto it = rusty::iter(self_.map);
        while (true) {
            auto e = it.next();
            if (e.is_none()) {
                break;
            }
            const auto kv = e.unwrap();
            Serialize_::serialize(rusty::detail::deref_if_pointer(([](auto&& __t) -> decltype(auto) { if constexpr (requires { __t._0; }) return (std::forward<decltype(__t)>(__t)._0); else if constexpr (requires { std::get<0>(std::forward<decltype(__t)>(__t)); }) return std::get<0>(std::forward<decltype(__t)>(__t)); else if constexpr (requires { (*__t)._0; }) return ((*std::forward<decltype(__t)>(__t))._0); else return std::get<0>(*std::forward<decltype(__t)>(__t)); })(kv)), ar);
        }
    }

}
// UFCS trait migration: free functions for `impl Serialize for ...`
namespace Serialize_ {
    template<typename K, typename V>
    void serialize(const rusty::HashMap<K, V>& self_, BinaryWriteArchive& ar) {
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
// UFCS trait migration: free functions for `impl Serialize for ...`
namespace Serialize_ {
    template<typename T1, typename T2>
    void serialize(const std::pair<T1, T2>& self_, BinaryWriteArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        Serialize_::serialize(self_.first, ar);
        Serialize_::serialize(self_.second, ar);
    }

}
/*RUSTYCPP:GEN-END id=serializable.serialize_trait*/

// ---- Fixed-width primitives. ------------------------------------------
// Each scalar's byte representation is `reinterpret_cast<const
// uint8_t*>(&v)`; the trait's `const uint8_t*` parameter type
// makes the byte view explicit (the previous `const void*` form
// hid it behind implicit conversion).

// The std::string / std::string_view leaves moved into the Serialize
// trait DSL block above (`impl Serialize for std::string{,_view}`),
// which lowers straight into this namespace and declares both
// overloads at the top of its GEN. Serialize_ reopens here only to
// carry the generic ADL bridge below.
namespace Serialize_ {
// Generic bridge: any type is trait-serializable. A migrated type resolves to
// its specific (more-specialized) overload above; anything else falls through
// to its operator<< here. This is what lets `serialize(field, ar)` work for
// EVERY field type (containers, polymorphic messages, un-migrated user structs)
// during the operator->trait coexistence — the enabler for the generator flip.
// (At Phase 8, when operators are deleted, every type has a specific overload,
// so this bridge is dropped.)
// Phase 8 endgame: the generic bridge dispatches via ADL instead of the
// operator layer. The zero-argument declaration in adl_detail_ poisons
// unqualified lookup so the dispatcher can ONLY resolve through ADL (the
// type's own namespace) — the catch-all cannot self-select, and a type with
// neither a specific overload above nor an ADL serialize() fails to compile, with
// the diagnostic naming the type through two instantiation notes.
//
// NB the declaration is never SELECTED (arity 0 vs 2), so the failure is an
// ordinary "no matching function for call to 'serialize'", NOT a
// "deleted function" diagnostic. Compile-verified: deleted and ordinary
// declarations give byte-identical diagnostics for the live 2-argument route.
// There are no 0-argument callers; a hypothetical one would now fail at link.
namespace adl_detail_ {
// Lookup poison: stops ascent past this scope.
void serialize();
// The two generic templates around the declaration are DSL now. The lookup
// guarantee is unchanged, probe-verified in both directions
// (scratchpad/recover3/probe_adl.cpp,
// probe_adl_neg.cpp, probe_adl_order.cpp): the emitted dispatcher's call
// stays UNQUALIFIED, so it can only resolve through ADL, the catch-all
// cannot self-select, and a type with neither a specific overload above
// nor an ADL serialize() still fails with a hard "no matching function"
// diagnostic naming the type. The GEN drops `inline`, which is redundant on
// a function template; this file already ships non-inline DSL-generated
// definitions in this very namespace (the string/string_view leaves above).
#if RUSTYCPP_RUST
fn dispatch_serialize<T>(v: &T, ar: &mut BinaryWriteArchive) {
    serialize(v, ar);
}
#endif
/*RUSTYCPP:GEN-BEGIN id=serializable.adl_ser_dispatch version=1 rust_sha256=38c0ddc40c850c769b7a2728ffe56ac6d2d96b3191cdf3c501196c27e9fe5c11*/
template<typename T>
void dispatch_serialize(const T& v, BinaryWriteArchive& ar) {
    serialize(v, ar);
}
/*RUSTYCPP:GEN-END id=serializable.adl_ser_dispatch*/
}  // namespace adl_detail_
#if RUSTYCPP_RUST
fn serialize<T>(v: &T, ar: &mut BinaryWriteArchive) {
    adl_detail_::dispatch_serialize(v, ar);
}
#endif
/*RUSTYCPP:GEN-BEGIN id=serializable.adl_ser version=1 rust_sha256=2c782e0b72a43d20b47c9df017419cea19c8506444278ca490bc1cdcd56bca2b*/
template<typename T>
void serialize(const T& v, BinaryWriteArchive& ar) {
    adl_detail_::dispatch_serialize(v, ar);
}
/*RUSTYCPP:GEN-END id=serializable.adl_ser*/
}  // namespace Serialize_

// ---- Variable-length integer encoding (SparseInt). --------------------


// ---- Variable-length byte sequences. ----------------------------------

// std::string is a convenience overload — same wire format as
// string_view (length-prefixed bytes).

// ---- Composites and containers. ----------------------------------------
// Their implementations live in the single Serialize trait DSL block above.
// That block emits the complete Serialize_ overload declaration set before
// any body, so nested containers resolve without a hand-written forwarding
// namespace or a second Rust trait.

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
    // @unsafe - virtual read through the type-erased source proxy.
    // The explicit `(*self.source_)` deref is load-bearing for exactly
    // the same reason as BinaryWriteArchive::write_bytes above:
    // SourceProxy is a hand-written C++ alias, so the transpiler cannot
    // see the Box behind it and would emit a `.` on the handle.
    fn read_exact(&mut self, p: *mut u8, n: usize) -> bool {
        let got: usize = (*self.source_).read_bytes(p, n);
        got == n
    }
    // Read exactly n bytes or abort — the operator>> truncation contract
    // (short reads at this layer are programming errors, not recoverable).
    // The DSL leaf Deserialize impls call this so the verify() lives once.
    fn read_or_abort(&mut self, p: *mut u8, n: usize) {
        verify(self.read_exact(p, n));
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=serializable.read_archive version=1 rust_sha256=00a1b72ca3ebcf830123d455ada1edca2614d3f827a2030f3db9222aef419e1c*/
struct BinaryReadArchive;

struct BinaryReadArchive {
    SourceProxy source_;

    bool read_exact(uint8_t* p, size_t n);
    void read_or_abort(uint8_t* p, size_t n);
};


bool BinaryReadArchive::read_exact(uint8_t* p, size_t n) {
    const size_t got = ((rusty::detail::deref_if_pointer_like(this->source_))).read_bytes(p, std::move(n));
    return rusty::detail::deref_if_pointer_like(got) == rusty::detail::deref_if_pointer_like(n);
}

void BinaryReadArchive::read_or_abort(uint8_t* p, size_t n) {
    verify(this->read_exact(p, std::move(n)));
}
/*RUSTYCPP:GEN-END id=serializable.read_archive*/



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
// Non-inline: the string leaf is now DSL-generated below and the GEN
// emits a non-inline definition (an inline-first declaration would
// silently make the whole function inline).
void deserialize(std::string& self_, BinaryReadArchive& ar);
template<typename T>
inline void deserialize(T& v, BinaryReadArchive& ar);
}

#if RUSTYCPP_RUST
pub trait Deserialize {
    fn deserialize(&mut self, ar: &mut BinaryReadArchive);
}
impl Deserialize for v32 {
    fn deserialize(&mut self, ar: &mut BinaryReadArchive) {
        let mut b: [u8; 9] = [0u8; 9];
        verify(ar.read_exact(b.as_mut_ptr(), 1));
        let total = SparseInt::buf_size(b[0]);
        if total > 1 {
            // @unsafe - the tail read lands after the already-consumed
            // first byte (the retired `varint_tail` kernel's whole job).
            verify(ar.read_exact(unsafe { b.as_mut_ptr().add(1) }, total - 1));
        }
        self.set(SparseInt::load32(b.as_ptr()));
    }
}
impl Deserialize for v64 {
    fn deserialize(&mut self, ar: &mut BinaryReadArchive) {
        let mut b: [u8; 9] = [0u8; 9];
        verify(ar.read_exact(b.as_mut_ptr(), 1));
        let total = SparseInt::buf_size(b[0]);
        if total > 1 {
            // @unsafe - the tail read lands after the already-consumed
            // first byte (the retired `varint_tail` kernel's whole job).
            verify(ar.read_exact(unsafe { b.as_mut_ptr().add(1) }, total - 1));
        }
        self.set(SparseInt::load64(b.as_ptr()));
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

// Read-side mirror of the string serialize leaf: v64 length prefix,
// resize, then read the bytes straight into the string's buffer.
// @unsafe { writing into std::string's internal buffer }
// `self.data() as *mut u8` picks the C++17 non-const data() overload
// (the receiver is `std::string&`) and lowers to
// rusty::detail::ptr_cast<uint8_t*>, replacing the old
// `reinterpret_cast<uint8_t*>(&self_[0])`. verify() keeps the
// abort-on-truncation contract the hand kernel had.
impl Deserialize for std::string {
    fn deserialize(&mut self, ar: &mut BinaryReadArchive) {
        let mut v_len = v64::new(0i64);
        Deserialize_::deserialize(&mut v_len, ar);
        let len: usize = v_len.get() as usize;
        self.resize(len);
        if len > 0usize {
            let p: *mut u8 = self.data() as *mut u8;
            verify(ar.read_exact(p, len));
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
/*RUSTYCPP:GEN-BEGIN id=serializable.deserialize_trait version=1 rust_sha256=d18c8bd66098a37a626daecdcd431849941815b4c84dfc640d806a65cc847b2e*/
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

    void deserialize(std::string& self_, BinaryReadArchive& ar);

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
    void deserialize(std::string& self_, BinaryReadArchive& ar);
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

// trait impl for `std::string` lowered via the Deserialize_ free functions above

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
        std::array<uint8_t, 9> b = [](auto _seed) { std::array<uint8_t, 9> _repeat{}; _repeat.fill(static_cast<uint8_t>(_seed)); return _repeat; }(static_cast<uint8_t>(0));
        verify(ar.read_exact(rusty::as_mut_ptr(b), 1));
        const auto total = SparseInt::buf_size(b.at(static_cast<size_t>(0)));
        if (rusty::detail::deref_if_pointer_like(total) > 1) {
            verify(ar.read_exact(rusty::ptr::add(rusty::as_mut_ptr(b), 1), rusty::detail::deref_if_pointer_like(total) - 1));
        }
        self_.set(SparseInt::load32(rusty::as_ptr(b)));
    }

    void deserialize(v64& self_, BinaryReadArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        std::array<uint8_t, 9> b = [](auto _seed) { std::array<uint8_t, 9> _repeat{}; _repeat.fill(static_cast<uint8_t>(_seed)); return _repeat; }(static_cast<uint8_t>(0));
        verify(ar.read_exact(rusty::as_mut_ptr(b), 1));
        const auto total = SparseInt::buf_size(b.at(static_cast<size_t>(0)));
        if (rusty::detail::deref_if_pointer_like(total) > 1) {
            verify(ar.read_exact(rusty::ptr::add(rusty::as_mut_ptr(b), 1), rusty::detail::deref_if_pointer_like(total) - 1));
        }
        self_.set(SparseInt::load64(rusty::as_ptr(b)));
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

    void deserialize(std::string& self_, BinaryReadArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        auto v_len = v64::new_(static_cast<int64_t>(0));
        Deserialize_::deserialize(v_len, ar);
        const size_t len = static_cast<size_t>(v_len.get());
        self_.resize(std::move(len));
        if (rusty::detail::deref_if_pointer_like(len) > static_cast<size_t>(0)) {
            uint8_t* const p = rusty::detail::ptr_cast<uint8_t*>(self_.data());
            verify(ar.read_exact(p, std::move(len)));
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

template <>
class DeserializeAdapter<std::string> final : public Deserialize {
    std::string value_;
public:
    DeserializeAdapter(std::string v) : value_(std::move(v)) {}
    DeserializeAdapter(DeserializeAdapter&& other) : value_(std::move(other.value_)) {}
    void deserialize(BinaryReadArchive& ar) override {
        rusty_ext::deserialize(value_, ar);
    }
};

template <>
class DeserializeAdapterRef<std::string> final : public Deserialize {
    const std::string& value_;
public:
    explicit DeserializeAdapterRef(const std::string& u) : value_(u) {}
    void deserialize(BinaryReadArchive& ar) override {
        std::abort();  // unreachable through &dyn T
    }
};

template <>
class DeserializeAdapterRefMut<std::string> final : public Deserialize {
    std::string& value_;
public:
    explicit DeserializeAdapterRefMut(std::string& u) : value_(u) {}
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
        std::array<uint8_t, 9> b = [](auto _seed) { std::array<uint8_t, 9> _repeat{}; _repeat.fill(static_cast<uint8_t>(_seed)); return _repeat; }(static_cast<uint8_t>(0));
        verify(ar.read_exact(rusty::as_mut_ptr(b), 1));
        const auto total = SparseInt::buf_size(b.at(static_cast<size_t>(0)));
        if (rusty::detail::deref_if_pointer_like(total) > 1) {
            verify(ar.read_exact(rusty::ptr::add(rusty::as_mut_ptr(b), 1), rusty::detail::deref_if_pointer_like(total) - 1));
        }
        self_.set(SparseInt::load32(rusty::as_ptr(b)));
    }

}
// UFCS trait migration: free functions for `impl Deserialize for ...`
namespace Deserialize_ {
    void deserialize(v64& self_, BinaryReadArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        std::array<uint8_t, 9> b = [](auto _seed) { std::array<uint8_t, 9> _repeat{}; _repeat.fill(static_cast<uint8_t>(_seed)); return _repeat; }(static_cast<uint8_t>(0));
        verify(ar.read_exact(rusty::as_mut_ptr(b), 1));
        const auto total = SparseInt::buf_size(b.at(static_cast<size_t>(0)));
        if (rusty::detail::deref_if_pointer_like(total) > 1) {
            verify(ar.read_exact(rusty::ptr::add(rusty::as_mut_ptr(b), 1), rusty::detail::deref_if_pointer_like(total) - 1));
        }
        self_.set(SparseInt::load64(rusty::as_ptr(b)));
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
    void deserialize(std::string& self_, BinaryReadArchive& ar) {
        using Self = std::remove_reference_t<decltype(self_)>;
        auto v_len = v64::new_(static_cast<int64_t>(0));
        Deserialize_::deserialize(v_len, ar);
        const size_t len = static_cast<size_t>(v_len.get());
        self_.resize(std::move(len));
        if (rusty::detail::deref_if_pointer_like(len) > static_cast<size_t>(0)) {
            uint8_t* const p = rusty::detail::ptr_cast<uint8_t*>(self_.data());
            verify(ar.read_exact(p, std::move(len)));
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

// The std::string leaf moved into the Deserialize trait DSL block
// above (`impl Deserialize for std::string`), which lowers straight
// into this namespace. Deserialize_ reopens here only to carry the
// generic ADL bridge below.
namespace Deserialize_ {
// Generic bridge (read side): mirror of the serialize catch-all.
// Phase 8 endgame: ADL dispatch via a poison declaration (see the serialize
// catch-all for the full rationale).
namespace adl_detail_ {
// Lookup poison: stops ascent past this scope.
void deserialize();
// The two generic templates around the declaration are DSL now. The lookup
// guarantee is unchanged, probe-verified in both directions
// (scratchpad/recover3/probe_adl.cpp,
// probe_adl_neg.cpp, probe_adl_order.cpp): the emitted dispatcher's call
// stays UNQUALIFIED, so it can only resolve through ADL, the catch-all
// cannot self-select, and a type with neither a specific overload nor an
// ADL deserialize() still fails with a hard "no matching function"
// diagnostic naming the type. The GEN drops `inline`, which is redundant on
// a function template (and the Deserialize_ forward-declaration wall above
// already declares this catch-all inline, so its linkage is unchanged).
#if RUSTYCPP_RUST
fn dispatch_deserialize<T>(v: &mut T, ar: &mut BinaryReadArchive) {
    deserialize(v, ar);
}
#endif
/*RUSTYCPP:GEN-BEGIN id=serializable.adl_deser_dispatch version=1 rust_sha256=6f05cd04c398dfd75470edb49ad10dedc6c6975c5273a025df0e62ee6b40a1cb*/
template<typename T>
void dispatch_deserialize(T& v, BinaryReadArchive& ar) {
    deserialize(v, ar);
}
/*RUSTYCPP:GEN-END id=serializable.adl_deser_dispatch*/
}  // namespace adl_detail_
#if RUSTYCPP_RUST
fn deserialize<T>(v: &mut T, ar: &mut BinaryReadArchive) {
    adl_detail_::dispatch_deserialize(v, ar);
}
#endif
/*RUSTYCPP:GEN-BEGIN id=serializable.adl_deser version=1 rust_sha256=49f52b1c8ebde469ce577e1d97b3064ac6b64789022aa79ef7133b1707d82737*/
template<typename T>
void deserialize(T& v, BinaryReadArchive& ar) {
    adl_detail_::dispatch_deserialize(v, ar);
}
/*RUSTYCPP:GEN-END id=serializable.adl_deser*/
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
//     contract. The kind prefix lives at the next-higher framing layer
//     (`SerializableEnvelope` for closed sets, `AnyMessage` for open sets).
//
// This replaced `Marshallable` + `MarshallableProxy` +
// `MarshallDeputy::reg_initializer`; production command envelopes now
// register and construct payloads through SerializableRegistry.
// ---------------------------------------------------------------------------

// Abstract base class for serializable payloads. Concrete derived
// types implement `save`, `load`, `kind`, and exact payload type identity. The proxy is a
// `std::shared_ptr<SerializableBase>` so SerializableEnvelope copies
// share the underlying payload via refcount (matching the original
// `support_copy<nontrivial>` semantics where copies via
// SerializableSharedPtrHolder shared the inner shared_ptr).
//
// Typed recovery compares the holder's `std::any::TypeId` first, then uses the
// shared `serializable_holder_of<T>` checked cast below.
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
    fn payload_type_id(&self) -> std::any::TypeId;
}
#endif
/*RUSTYCPP:GEN-BEGIN id=serializable.1 version=1 rust_sha256=6916f97bcf38559da004cc48217408c430d325dacd17f5961d6519a43efb80c0*/
class SerializableBase;

class SerializableBase {
public:
    virtual ~SerializableBase() noexcept(false) {}
    virtual void save(BinaryWriteArchive& ar) const = 0;
    virtual void load(BinaryReadArchive& ar) = 0;
    virtual int32_t kind() const = 0;
    virtual std::type_index payload_type_id() const = 0;
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
//     dispatch directly through the field (`self.ptr.save(ar)` lowers
//     to `this->ptr->save(ar)`), while the Arc HANDLE method get_mut()
//     correctly stays a `.` call — so `load` needs no kernel either.
//     (The old "get_mut() has no DSL spelling" note was wrong at pin
//     da6e9bf4: `self.ptr.get_mut().unwrap().load(ar)` lowers verbatim.)
#if RUSTYCPP_RUST
struct SerializableSharedPtrHolder<T> {
    ptr: Arc<T>,
}

#[cpp_inherit]
impl<T: 'static> SerializableBase for SerializableSharedPtrHolder<T> {
    fn save(&self, ar: &mut BinaryWriteArchive) {
        self.ptr.save(ar)
    }
    // @unsafe - unique-owner mutation window: load always runs on a
    // factory-fresh proxy (registry create -> strong_count 1), so
    // get_mut() is Some; a shared proxy here would be a bug and panics
    // loudly instead of silently mutating shared state.
    fn load(&mut self, ar: &mut BinaryReadArchive) {
        self.ptr.get_mut().unwrap().load(ar)
    }
    fn kind(&self) -> i32 {
        self.ptr.kind()
    }
    fn payload_type_id(&self) -> std::any::TypeId {
        std::any::TypeId::of::<T>()
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=serializable.shared_ptr_holder version=1 rust_sha256=42c08469c2727c171554e2f7d93d01ef8e4e4d1abb575b9ab45ca8476de655c5*/
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
        this->ptr.get_mut().unwrap().load(ar);
    }
    int32_t kind() const {
        return this->ptr->kind();
    }
    std::type_index payload_type_id() const {
        return std::type_index(typeid(T));
    }
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = rusty::is_send<T>::value && rusty::is_sync<T>::value;
    static constexpr bool is_sync = rusty::is_send<T>::value && rusty::is_sync<T>::value;
};
/*RUSTYCPP:GEN-END id=serializable.shared_ptr_holder*/



}  // namespace details

// @unsafe - checked recovery from the erased base. SerializableSharedPtrHolder
// is the only SerializableBase implementation, and payload_type_id equality
// proves its template argument before the raw cast. Null and mismatches remain
// null, matching the former dynamic_cast contract.
#if RUSTYCPP_RUST
fn serializable_holder_of<T: 'static>(base: *const SerializableBase)
    -> *const details::SerializableSharedPtrHolder<T> {
    if base.is_null() {
        return core::ptr::null();
    }
    if unsafe { (*base).payload_type_id() } != std::any::TypeId::of::<T>() {
        return core::ptr::null();
    }
    base as *const details::SerializableSharedPtrHolder<T>
}
#endif
/*RUSTYCPP:GEN-BEGIN id=serializable.19 version=1 rust_sha256=f6581625353e6989d6f0ce911ef1522510095956e1006877b486083acb9c7d3c*/
template<typename T>
std::add_pointer_t<std::add_const_t<details::SerializableSharedPtrHolder<T>>> serializable_holder_of(const SerializableBase* base) {
    if ((base == nullptr)) {
        return rusty::ptr::null();
    }
    if (((*base)).payload_type_id() != std::type_index(typeid(T))) {
        return rusty::ptr::null();
    }
    return reinterpret_cast<std::add_pointer_t<std::add_const_t<details::SerializableSharedPtrHolder<T>>>>(base);
}
/*RUSTYCPP:GEN-END id=serializable.19*/

// Const-generic base providing `kind()` (instance) + `static_kind()`
// (static) from an explicit wire discriminant. Production payloads bind
// KIND through their closed-set PayloadMember registration, e.g.
// `Serializable<PayloadMember<MakoCommands, MyType>::KIND>`.
//
// KIND 0 remains the wire-level "unknown / unset" sentinel. The const
// accessor asserts before returning the discriminant. `cpp_no_auto_traits`
// is load-bearing: this empty C++ base must not inject inherited Send/Sync
// markers into payload classes.
#if RUSTYCPP_RUST
#[cfg_attr(any(), cpp_no_auto_traits)]
pub struct Serializable<const KIND: i32> {}

impl<const KIND: i32> Serializable<KIND> {
    #[cfg_attr(any(), cpp_noexcept)]
    pub const fn kind(&self) -> i32 {
        Self::static_kind()
    }

    #[cfg_attr(any(), cpp_noexcept)]
    pub const fn static_kind() -> i32 {
        assert!(KIND != 0i32,
            "Serializable kind 0 is reserved for unknown / unset");
        KIND
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=serializable.22 version=1 rust_sha256=760f928907e3a9832cddb249b7197eb0cbaaa3fd2403da10ad8768f25880fb29*/
template<int32_t KIND>
struct Serializable;

template<int32_t KIND>
struct Serializable {

    constexpr int32_t kind() const noexcept(true) {
        return Serializable<KIND>::static_kind();
    }
    static constexpr int32_t static_kind() noexcept(true) {
        if (!(rusty::detail::deref_if_pointer_like(KIND) != static_cast<int32_t>(0))) { throw std::logic_error("Serializable kind 0 is reserved for unknown / unset"); }
        return KIND;
    }
};
/*RUSTYCPP:GEN-END id=serializable.22*/

// The structural contract for a Serializable-migrated T is:
//   - void save(BinaryWriteArchive&) const
//   - void load(BinaryReadArchive&)
//   - int32_t kind() const
// Overload disambiguation between the Marshallable subclass path
// and the Serializable bridge path uses
// `!std::is_base_of_v<Marshallable, T>`; shape mismatches surface
// as instantiation-time template errors rather than a separate
// constraint predicate.

// Construct a SerializableProxy that owns a T — default-constructed, or
// copy-constructed from `value`. T just needs to satisfy the structural
// shape:
//   - void save(BinaryWriteArchive&) const
//   - void load(BinaryReadArchive&)
//   - int32_t kind() const
// The factory wraps T in a SerializableSharedPtrHolder<T> so callers can
// recover T through `serializable_holder_of<T>(proxy.get())`
// (or via the envelope's unpack<T>() / unpack_shared<T>()).
//
// This was one `template<class T, class... Args>` perfect-forwarding
// factory, and the tracker called parameter-packs a PERMANENT blocker.
// It is not: no call site passes more than one argument (4 total, all in
// rpc_marshal_archive_test.cc), so
// the pack splits into two arity-disjoint generic fns and every call site
// keeps its exact current spelling.
//
// Rust has no function overloading, so the two live in SEPARATE
// `#if RUSTYCPP_RUST` blocks — the same trick the make_sink_proxy /
// make_source_proxy pairs above use. Each emits an ordinary C++ function
// template; being arity-disjoint they overload unambiguously.
//
// The 1-arg form takes `&T` (not `T`) DELIBERATELY: that lowers to
// `const T&`, so `rusty::Arc<T>::make(value)` copy-constructs exactly as
// `std::forward<Args>(args)...` did at both existing (lvalue) call sites.
// An rvalue argument would now copy where the pack would have moved; if
// such a site ever appears, add a third block taking `value: T` rather
// than widening these.
#if RUSTYCPP_RUST
fn make_serializable_proxy<T: 'static>() -> SerializableProxy {
    let sp: Arc<T> = rusty::Arc::<T>::make();
    rusty::Arc::<details::SerializableSharedPtrHolder<T>>::make(sp)
}
#endif
/*RUSTYCPP:GEN-BEGIN id=serializable.20 version=1 rust_sha256=a42f8ff395cbba6ada3d0c95bf209c4a3289d9bd510da1c2a78d3d43a6de843c*/
template<typename T>
SerializableProxy make_serializable_proxy() {
    rusty::Arc<T> sp = rusty::Arc<T>::make();
    return rusty::Arc<details::SerializableSharedPtrHolder<T>>::make(std::move(sp));
}
/*RUSTYCPP:GEN-END id=serializable.20*/

#if RUSTYCPP_RUST
fn make_serializable_proxy<T: 'static>(value: &T) -> SerializableProxy {
    let sp: Arc<T> = rusty::Arc::<T>::make(value);
    rusty::Arc::<details::SerializableSharedPtrHolder<T>>::make(sp)
}
#endif
/*RUSTYCPP:GEN-BEGIN id=serializable.21 version=1 rust_sha256=74f125038edd4452782f26df7e376c3e0d48fccac0b2903277877470a38d9fb4*/
template<typename T>
SerializableProxy make_serializable_proxy(const T& value) {
    rusty::Arc<T> sp = rusty::Arc<T>::make(value);
    return rusty::Arc<details::SerializableSharedPtrHolder<T>>::make(std::move(sp));
}
/*RUSTYCPP:GEN-END id=serializable.21*/

// Factory registry: maps int32_t kind tags to factories that produce
// fresh SerializableProxy instances. Authored as inline Rust DSL
// (statics on an empty struct; the generic reg<T> now carries its own
// body — a DSL generic emits a real template<typename T>). The
// factory-map singleton + rusty::Mutex live in the impl kernels below
// unchanged.
using SerializableRegistryFactory = rusty::Function<SerializableProxy()>;

struct SerializableRegistry;
SerializableProxy serializable_registry_create_impl(int32_t kind);
bool serializable_registry_is_registered_impl(int32_t kind);
void serializable_registry_clear_impl();
void serializable_registry_register_factory(int32_t kind,
                                            SerializableRegistryFactory factory);

#if RUSTYCPP_RUST
struct SerializableRegistry {}

impl SerializableRegistry {
    // Register T under `kind` (returns 0 for static-initializer use).
    // The factory closure captures NOTHING — it only names T — so the
    // `[&]` lambda the DSL emits cannot dangle even though the
    // rusty::Function it becomes is stored in a process-wide map that
    // outlives this call. DO NOT introduce a captured local here
    // without re-checking that; a by-reference capture would dangle.
    // The proxy is holder-shaped so SerializableEnvelope::load gives
    // unpack_shared<T> a refcount-shared Arc<T>.
    fn reg<T: 'static>(kind: i32) -> i32 {
        serializable_registry_register_factory(kind, || -> SerializableProxy {
            let sp: Arc<T> = rusty::Arc::<T>::make();
            rusty::Arc::<details::SerializableSharedPtrHolder<T>>::make(sp)
        });
        0i32
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
/*RUSTYCPP:GEN-BEGIN id=serializable.registry version=1 rust_sha256=f9e9c5051552af6c5a71c0b67ca56357ff6503a5907aa62ed3f16c08509f4eda*/
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
    serializable_registry_register_factory(std::move(kind), [&]() -> SerializableProxy {
rusty::Arc<T> sp = rusty::Arc<T>::make();
return rusty::Arc<details::SerializableSharedPtrHolder<T>>::make(std::move(sp));
});
    return static_cast<int32_t>(0);
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
//
// Authored as inline Rust DSL: the Meyers-singleton shape IS expressible —
// `static NAME: T = init;` lowers to a block-scope C++ static, and the
// `&mut NAME` TAIL expression lowers to a plain `return NAME;` (spelling
// `return NAME;` in the DSL instead emits `return std::move(NAME)`, which
// would gut the process-lifetime object on the first call). rusty::Mutex
// has no default ctor (unlike the retired SpinMutex), so it is seeded with
// an empty registry map explicitly.
#if RUSTYCPP_RUST
fn registry() -> &mut rusty::Mutex<SerializableRegistryMap> {
    static R: rusty::Mutex<SerializableRegistryMap> = rusty::Mutex::new(SerializableRegistryMap {});
    &mut R
}
#endif
/*RUSTYCPP:GEN-BEGIN id=serializable.18 version=1 rust_sha256=49f8745b3588459aaeb3f9b7a87779db671a3cda43b00e14c67ac03a06a153f1*/
rusty::Mutex<SerializableRegistryMap>& registry() {
    static rusty::Mutex<SerializableRegistryMap> R = rusty::Mutex<SerializableRegistryMap>::new_(SerializableRegistryMap{});
    return R;
}
/*RUSTYCPP:GEN-END id=serializable.18*/

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
