module;

#include <cstdint>
#include <rusty/arc.hpp>
#include <rusty/box.hpp>
#include <rusty/function.hpp>
#include <rusty/option.hpp>

export module rrr.channel;

import std;
import rrr.callback_wrapper;

// @safe - virtual interfaces only (ChannelConnectionBase /
// ChannelListenerBase / ChannelFactoryBase have no method bodies),
// a constexpr error-name lookup, and the POD `ChannelFrame` /
// `ConnectResult` structs. The raw `const uint8_t* payload` field
// on `ChannelFrame` is a transport-level non-owning view (the
// SinkProxy contract pins the bytes for the call duration); no
// method here dereferences it.
export namespace rrr {

// `ChannelError` — transport-error code surfaced by `ChannelConnectionBase`
// / `ChannelListenerBase` / `ChannelFactoryBase`. Authored as inline
// Rust DSL: the `#if RUSTYCPP_RUST` block below is the source of truth;
// the transpiler regenerates the matching `RUSTYCPP:GEN-BEGIN ... END`
// block.
#if RUSTYCPP_RUST
#[repr(i32)]
enum ChannelError {
    None = 0,
    WouldBlock = 1,
    ConnectionRefused = 2,
    ConnectionReset = 3,
    Timeout = 4,
    AddressInUse = 5,
    AddressInvalid = 6,
    PermissionDenied = 7,
    TooManyOpenFiles = 8,
    Internal = 9,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=channel.channel_error version=1 rust_sha256=a17eae07b8ee684b8064e36c651b23a17d2afd98e4f9d2cfa194f222b6212597*/
enum class ChannelError;
constexpr ChannelError ChannelError_None();
constexpr ChannelError ChannelError_WouldBlock();
constexpr ChannelError ChannelError_ConnectionRefused();
constexpr ChannelError ChannelError_ConnectionReset();
constexpr ChannelError ChannelError_Timeout();
constexpr ChannelError ChannelError_AddressInUse();
constexpr ChannelError ChannelError_AddressInvalid();
constexpr ChannelError ChannelError_PermissionDenied();
constexpr ChannelError ChannelError_TooManyOpenFiles();
constexpr ChannelError ChannelError_Internal();

enum class ChannelError {
    None = 0,
    WouldBlock = 1,
    ConnectionRefused = 2,
    ConnectionReset = 3,
    Timeout = 4,
    AddressInUse = 5,
    AddressInvalid = 6,
    PermissionDenied = 7,
    TooManyOpenFiles = 8,
    Internal = 9
};
inline constexpr ChannelError ChannelError_None() { return ChannelError::None; }
inline constexpr ChannelError ChannelError_WouldBlock() { return ChannelError::WouldBlock; }
inline constexpr ChannelError ChannelError_ConnectionRefused() { return ChannelError::ConnectionRefused; }
inline constexpr ChannelError ChannelError_ConnectionReset() { return ChannelError::ConnectionReset; }
inline constexpr ChannelError ChannelError_Timeout() { return ChannelError::Timeout; }
inline constexpr ChannelError ChannelError_AddressInUse() { return ChannelError::AddressInUse; }
inline constexpr ChannelError ChannelError_AddressInvalid() { return ChannelError::AddressInvalid; }
inline constexpr ChannelError ChannelError_PermissionDenied() { return ChannelError::PermissionDenied; }
inline constexpr ChannelError ChannelError_TooManyOpenFiles() { return ChannelError::TooManyOpenFiles; }
inline constexpr ChannelError ChannelError_Internal() { return ChannelError::Internal; }
/*RUSTYCPP:GEN-END id=channel.channel_error*/

inline constexpr const char* channel_error_to_string(ChannelError e) {
    switch (e) {
        case ChannelError::None:               return "None";
        case ChannelError::WouldBlock:         return "WouldBlock";
        case ChannelError::ConnectionRefused:  return "ConnectionRefused";
        case ChannelError::ConnectionReset:    return "ConnectionReset";
        case ChannelError::Timeout:            return "Timeout";
        case ChannelError::AddressInUse:       return "AddressInUse";
        case ChannelError::AddressInvalid:     return "AddressInvalid";
        case ChannelError::PermissionDenied:   return "PermissionDenied";
        case ChannelError::TooManyOpenFiles:   return "TooManyOpenFiles";
        case ChannelError::Internal:           return "Internal";
    }
    return "Unknown";
}

// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `/*RUSTYCPP:GEN-BEGIN ... END*/` block with the C++ struct. The
// generated struct is still an aggregate, so every call site's
// positional `ChannelFrame{payload, size}` brace init and every
// `ChannelFrame f{}` value-init continues to work; the `= nullptr` /
// `= 0` field defaults are dropped but every consumer either supplies
// both fields explicitly or relies on `{}` zero-init.
#if RUSTYCPP_RUST
struct ChannelFrame {
    payload: *const u8,
    size: usize,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=channel.1 version=1 rust_sha256=00e89d6c5a4b7e937f13ff4dd75a4258a187f6661dd053f2816bf00a2e95e1fb*/
struct ChannelFrame;

struct ChannelFrame {
    const uint8_t* payload;
    size_t size;
};
/*RUSTYCPP:GEN-END id=channel.1*/

using OnFrameCallback  = detail::CallbackWrapper<void(const ChannelFrame&) const>;
using OnClosedCallback = detail::CallbackWrapper<void(ChannelError reason) const>;
using OnErrorCallback  = detail::CallbackWrapper<void(ChannelError err,
                                                       std::string_view message) const>;

// `ChannelConnectionBase` — abstract transport-connection interface
// (TcpConnection, inmemory connection, ...). Authored as inline Rust
// DSL `pub trait`; the transpiler emits the same shape of C++
// abstract class at namespace scope. Tier-1.4 of the rrr trait sweep.
#if RUSTYCPP_RUST
pub trait ChannelConnectionBase {
    fn send_frame(&mut self, frame: &ChannelFrame) -> ChannelError;
    fn flush(&mut self);
    fn close(&mut self);
    fn is_closed(&self) -> bool;
    fn peer_address(&self) -> std::string;
    fn set_on_frame(&mut self, cb: OnFrameCallback);
    fn set_on_closed(&mut self, cb: OnClosedCallback);
    fn set_on_error(&mut self, cb: OnErrorCallback);
}
#endif
/*RUSTYCPP:GEN-BEGIN id=channel.2 version=1 rust_sha256=894b849f6f600fe181394b093c2fc7f0b7960d8219710fcac56ff8398d61fdd2*/
class ChannelConnectionBase {
public:
    virtual ~ChannelConnectionBase() noexcept(false) {}
    virtual ChannelError send_frame(const ChannelFrame& frame) = 0;
    virtual void flush() = 0;
    virtual void close() = 0;
    virtual bool is_closed() const = 0;
    virtual std::string peer_address() const = 0;
    virtual void set_on_frame(OnFrameCallback cb) = 0;
    virtual void set_on_closed(OnClosedCallback cb) = 0;
    virtual void set_on_error(OnErrorCallback cb) = 0;
    ChannelConnectionBase(const ChannelConnectionBase&) = delete;
    ChannelConnectionBase& operator=(const ChannelConnectionBase&) = delete;
    ChannelConnectionBase(ChannelConnectionBase&&) = delete;
    ChannelConnectionBase& operator=(ChannelConnectionBase&&) = delete;
protected:
    ChannelConnectionBase() = default;
};

template <class U> class ChannelConnectionBaseAdapter;
template <class U> class ChannelConnectionBaseAdapterRef;
template <class U> class ChannelConnectionBaseAdapterRefMut;
/*RUSTYCPP:GEN-END id=channel.2*/

// Owned, non-nullable handle to a channel connection. Use
// `rusty::Option<ChannelConnectionProxy>` at call sites that need a
// nullable / sentinel form (e.g. `ConnectResult.connection`).
using ChannelConnectionProxy = rusty::Box<ChannelConnectionBase>;

using OnAcceptCallback = detail::CallbackWrapper<void(ChannelConnectionProxy) const>;

// `ChannelListenerBase` — abstract accept-loop interface (TcpListener,
// inmemory listener, ...). Tier-1.4 trait migration.
#if RUSTYCPP_RUST
pub trait ChannelListenerBase {
    fn listen(&mut self, addr: std::string_view) -> ChannelError;
    fn close(&mut self);
    fn is_closed(&self) -> bool;
    fn local_address(&self) -> std::string;
    fn set_on_accept(&mut self, cb: OnAcceptCallback);
    fn set_on_error(&mut self, cb: OnErrorCallback);
}
#endif
/*RUSTYCPP:GEN-BEGIN id=channel.3 version=1 rust_sha256=3d86b3cd57d06df5430ae80d838e0bee7b9318ea9e8b467b5d10832684aa91fa*/
class ChannelListenerBase {
public:
    virtual ~ChannelListenerBase() noexcept(false) {}
    virtual ChannelError listen(std::string_view addr) = 0;
    virtual void close() = 0;
    virtual bool is_closed() const = 0;
    virtual std::string local_address() const = 0;
    virtual void set_on_accept(OnAcceptCallback cb) = 0;
    virtual void set_on_error(OnErrorCallback cb) = 0;
    ChannelListenerBase(const ChannelListenerBase&) = delete;
    ChannelListenerBase& operator=(const ChannelListenerBase&) = delete;
    ChannelListenerBase(ChannelListenerBase&&) = delete;
    ChannelListenerBase& operator=(ChannelListenerBase&&) = delete;
protected:
    ChannelListenerBase() = default;
};

template <class U> class ChannelListenerBaseAdapter;
template <class U> class ChannelListenerBaseAdapterRef;
template <class U> class ChannelListenerBaseAdapterRefMut;
/*RUSTYCPP:GEN-END id=channel.3*/

using ChannelListenerProxy = rusty::Box<ChannelListenerBase>;

// `ConnectResult` — value-result returned by `ChannelFactoryBase::connect`.
// All call sites use positional brace init (`ConnectResult{connection,
// error}`), so dropping the field-level defaults (`{rusty::None}` /
// `= ChannelError::None`) is safe: no `ConnectResult{}` zero-init callers
// remain in the tree.
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `/*RUSTYCPP:GEN-BEGIN ... END*/` block with the C++ struct.
#if RUSTYCPP_RUST
struct ConnectResult {
    connection: rusty::Option<ChannelConnectionProxy>,
    error: ChannelError,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=channel.connect_result version=1 rust_sha256=7bbe3787f9a7b6904d3b5194e148a9baab9ef1d9f6971846ad946afb022873cf*/
struct ConnectResult;

struct ConnectResult {
    rusty::Option<ChannelConnectionProxy> connection;
    ChannelError error;
};
/*RUSTYCPP:GEN-END id=channel.connect_result*/

// `ChannelFactoryBase` — abstract transport factory (TcpFactory,
// inmemory factory, ...). Tier-1.4 trait migration. `backend_name`
// returns `std::string` (Rust `String`) instead of the original
// `const char*` so the DSL has a clean type to lower — string
// literals on the impl side auto-convert via the implicit
// `std::string(const char*)` constructor, so impl bodies like
// `return "tcp";` keep compiling unchanged. The behavioural diff
// is a small heap allocation per call, which is fine for a
// "report-my-backend-name" accessor that runs at startup / in
// diagnostics, not on the hot path.
#if RUSTYCPP_RUST
pub trait ChannelFactoryBase {
    fn connect(&mut self, addr: std::string_view) -> ConnectResult;
    fn make_listener(&mut self) -> Option<ChannelListenerProxy>;
    fn backend_name(&self) -> std::string;
}
#endif
/*RUSTYCPP:GEN-BEGIN id=channel.4 version=1 rust_sha256=46c44a281cc328cf55cb46fcbdfc3a64b6e07227c560428d445abeb2f5f9bd8b*/
class ChannelFactoryBase {
public:
    virtual ~ChannelFactoryBase() noexcept(false) {}
    virtual ConnectResult connect(std::string_view addr) = 0;
    virtual rusty::Option<ChannelListenerProxy> make_listener() = 0;
    virtual std::string backend_name() const = 0;
    ChannelFactoryBase(const ChannelFactoryBase&) = delete;
    ChannelFactoryBase& operator=(const ChannelFactoryBase&) = delete;
    ChannelFactoryBase(ChannelFactoryBase&&) = delete;
    ChannelFactoryBase& operator=(ChannelFactoryBase&&) = delete;
protected:
    ChannelFactoryBase() = default;
};

template <class U> class ChannelFactoryBaseAdapter;
template <class U> class ChannelFactoryBaseAdapterRef;
template <class U> class ChannelFactoryBaseAdapterRefMut;
/*RUSTYCPP:GEN-END id=channel.4*/

using ChannelFactoryProxy = rusty::Box<ChannelFactoryBase>;

}  // export namespace rrr
