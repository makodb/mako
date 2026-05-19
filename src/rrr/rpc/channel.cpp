module;

#include <cstdint>
#include <rusty/arc.hpp>
#include <rusty/box.hpp>
#include <rusty/function.hpp>

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

struct ChannelFrame {
    const std::uint8_t* payload = nullptr;
    std::size_t size = 0;
};

using OnFrameCallback  = detail::CallbackWrapper<void(const ChannelFrame&) const>;
using OnClosedCallback = detail::CallbackWrapper<void(ChannelError reason) const>;
using OnErrorCallback  = detail::CallbackWrapper<void(ChannelError err,
                                                       std::string_view message) const>;

class ChannelConnectionBase {
 public:
  virtual ~ChannelConnectionBase() = default;
  virtual ChannelError send_frame(const ChannelFrame&)         = 0;
  virtual void         flush()                                  = 0;
  virtual void         close()                                  = 0;
  virtual bool         is_closed()       const                  = 0;
  virtual std::string  peer_address()    const                  = 0;
  virtual void         set_on_frame(OnFrameCallback)            = 0;
  virtual void         set_on_closed(OnClosedCallback)          = 0;
  virtual void         set_on_error(OnErrorCallback)            = 0;
};

using ChannelConnectionProxy = std::unique_ptr<ChannelConnectionBase>;

using OnAcceptCallback = detail::CallbackWrapper<void(ChannelConnectionProxy) const>;

class ChannelListenerBase {
 public:
  virtual ~ChannelListenerBase() = default;
  virtual ChannelError listen(std::string_view)         = 0;
  virtual void         close()                          = 0;
  virtual bool         is_closed()       const          = 0;
  virtual std::string  local_address()   const          = 0;
  virtual void         set_on_accept(OnAcceptCallback)  = 0;
  virtual void         set_on_error(OnErrorCallback)    = 0;
};

using ChannelListenerProxy = std::unique_ptr<ChannelListenerBase>;

struct ConnectResult {
    ChannelConnectionProxy connection;
    ChannelError           error = ChannelError::None;
};

class ChannelFactoryBase {
 public:
  virtual ~ChannelFactoryBase() = default;
  virtual ConnectResult         connect(std::string_view)    = 0;
  virtual ChannelListenerProxy  make_listener()              = 0;
  virtual const char*           backend_name()    const      = 0;
};

using ChannelFactoryProxy = std::unique_ptr<ChannelFactoryBase>;

}  // export namespace rrr
