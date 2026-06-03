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

enum class ChannelError : int {
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
};

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

struct ConnectResult {
    rusty::Option<ChannelConnectionProxy> connection{rusty::None};
    ChannelError                          error = ChannelError::None;
};

// `ChannelFactoryBase` — abstract transport factory (TcpFactory,
// inmemory factory, ...). NOT migrated to a DSL trait this round:
// the `backend_name() -> const char*` return type doesn't lower
// cleanly through the inline-Rust → C++ pipeline (the DSL's
// `*const c_char` emits `const c_char*` which isn't a known C++
// type without a separate typedef). Tier-1.4 leaves this one in
// hand-written C++ until a `*const char` mapping or a `c_char`
// alias lands in the transpiler.
class ChannelFactoryBase {
 public:
  virtual ~ChannelFactoryBase() = default;
  virtual ConnectResult                       connect(std::string_view)    = 0;
  virtual rusty::Option<ChannelListenerProxy> make_listener()              = 0;
  virtual const char*                         backend_name()    const      = 0;
};

using ChannelFactoryProxy = rusty::Box<ChannelFactoryBase>;

}  // export namespace rrr
