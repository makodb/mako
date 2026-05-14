#pragma once

// import std; replacement — see <std_compat.hpp> for rationale.
#include <std_compat.hpp>

// @c-compat-added
#include <cstddef>
#include <cstdint>

#include <rusty/arc.hpp>
#include <rusty/box.hpp>
#include <rusty/function.hpp>

#include "../base/callback_wrapper.hpp"


/**
 * SRPC Channel Layer Core Interfaces.
 *
 * This header defines the *contract* between SRPC's RPC layer and the
 * underlying byte-channel that moves serialized frames across the wire.
 * It is intentionally minimal and transport-agnostic: a TCP backend, an
 * in-memory backend (for deterministic tests), and possibly a TLS or
 * RDMA backend in the future, can all conform to the same facade.
 *
 * ============================================================================
 * Layering and ownership boundaries
 * ============================================================================
 *
 * Channel layer owns:
 *   - Socket / file descriptor lifecycle (open, close, idempotent shutdown).
 *   - Epoll / poll-thread integration.
 *   - Partial-read assembly and partial-write coalescing at byte level.
 *   - Wire-frame boundary detection (one frame in, one frame out).
 *   - Reporting transport-level errors and close events to the RPC layer.
 *
 * RPC layer owns:
 *   - Request/response correlation (xid, rpc_id, futures).
 *   - Reliability: timeout, retry, reconnect policy, circuit breaker,
 *     heartbeat, request buffering, completion tracking, metrics.
 *   - Service registration, dispatch, and serialization of typed requests
 *     and responses.
 *
 * The channel layer never decides to reconnect on its own. It reports
 * `on_closed` and `on_error`; the RPC layer interprets those events
 * against its `ReconnectPolicy` and decides what to do. This avoids the
 * "split-brain reconnection" failure mode where two layers race to
 * re-establish the same connection.
 *
 * ============================================================================
 * Wire format
 * ============================================================================
 *
 * The channel preserves SRPC's existing wire format byte-for-byte. A
 * frame is just an opaque byte buffer to the channel; the codec for
 * `<size><header><payload>` lives one layer up (`frame_codec.*` in this
 * workstream). The channel never inspects frame contents.
 *
 * ============================================================================
 * Threading and ordering
 * ============================================================================
 *
 * Each `ChannelConnection` is associated with a single poll thread (the
 * one it was constructed on / registered to). All callbacks
 * (`on_frame`, `on_closed`, `on_error`) are invoked **from that poll
 * thread only**. Implementations must not invoke callbacks from
 * arbitrary threads.
 *
 * Per-connection ordering guarantees:
 *   - `on_frame` is invoked once per fully-assembled inbound frame, in
 *     the order the bytes arrived on the wire.
 *   - `on_closed` is invoked at most once per connection. After it
 *     fires, no further `on_frame` or `on_error` callback will fire.
 *   - `on_error` may fire zero or more times before `on_closed`. If the
 *     error is fatal, the implementation must follow it with
 *     `on_closed`.
 *
 * ============================================================================
 * Backpressure
 * ============================================================================
 *
 * `send_frame` is non-blocking and returns immediately. Implementations
 * are expected to enqueue and drain in the poll thread. When the
 * outbound queue grows beyond an implementation-defined high-water
 * mark, `send_frame` returns `ChannelError::WouldBlock` instead of
 * silently dropping or unbounded buffering. The RPC layer is then
 * responsible for applying its own queueing/admission control policy.
 *
 * ============================================================================
 * Lifetime and ownership
 * ============================================================================
 *
 * Connections are held as `rusty::Box<ChannelConnectionBase>`
 * (move-only). The owner of the proxy is the owner of the connection;
 * destroying the proxy must close and unregister the underlying
 * resource. Callback objects supplied via `set_on_*` are kept alive by
 * the connection until the connection is destroyed, replaced, or
 * `on_closed` has fired.
 *
 * Implementations must accept `set_on_*` calls both before and after
 * registration with a poll thread. Calls after `on_closed` are silently
 * ignored.
 */
namespace rrr {

// ---------------------------------------------------------------------------
// Error reporting
// ---------------------------------------------------------------------------

/**
 * Error categories surfaced by the channel layer. These map cleanly
 * onto the existing `RpcErrorCategory::CONNECTION` family but are kept
 * separate so the RPC layer can decide policy without owning transport
 * details.
 */
enum class ChannelError : int {
    None = 0,             // No error (sentinel)
    WouldBlock = 1,       // Outbound queue full; caller should retry later
    ConnectionRefused = 2,// Peer refused / unreachable
    ConnectionReset = 3,  // Peer closed unexpectedly
    Timeout = 4,          // Operation timed out at transport level
    AddressInUse = 5,     // listen() failed: address already bound
    AddressInvalid = 6,   // Malformed address / DNS failure
    PermissionDenied = 7, // Insufficient privilege to bind / connect
    TooManyOpenFiles = 8, // ulimit/EMFILE
    Internal = 9,         // Catch-all for unexpected transport faults
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

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

/**
 * A single channel frame. Opaque payload bytes plus a small header
 * carrying just enough metadata to identify the frame on the wire. The
 * channel layer never interprets `payload`; the codec layer does.
 *
 * On send, the caller relinquishes ownership of `payload` to the
 * channel for the duration of the call. On receive, the channel
 * delivers a `ChannelFrame` whose `payload` is owned by the channel
 * and remains valid only for the duration of the `on_frame` callback;
 * consumers that need to retain the bytes must copy or move them.
 */
struct ChannelFrame {
    // Opaque frame body. The high bit of `size` carries the response
    // header extension flag (see internal_protocol.hpp); the channel
    // layer treats it as opaque, but reserves the right to look at the
    // first 4 bytes for framing. Implementations must not reorder or
    // coalesce frames beyond what the codec emits.
    const std::uint8_t* payload = nullptr;
    std::size_t size = 0;
};

// ---------------------------------------------------------------------------
// Callback contracts
// ---------------------------------------------------------------------------

/**
 * `on_frame(frame)`         — invoked when a complete frame is decoded.
 * `on_closed(reason)`       — invoked exactly once when the connection
 *                             is closed for any reason.
 * `on_error(err, message)`  — invoked when a transport-level error is
 *                             observed. May be followed by `on_closed`
 *                             if the error is fatal.
 *
 * All callbacks run on the connection's poll thread.
 *
 * Storage is `Arc<Function<...const>>` via `detail::CallbackWrapper`
 * (defined in `base/callback_wrapper.hpp`).  This lets the (transport-
 * specific) implementation clone the Arc under its lock and invoke
 * without holding it — the in-memory transport relies on this for the
 * receiver→sender re-entrant `send_frame` path.  Arc clone is a cheap
 * atomic refcount bump; the contained Function is move-only and shared
 * via the Arc.  The wrapper preserves the std::function-style API
 * (default-constructible, copyable, implicit lambda construction,
 * `operator bool`/`operator()`), so call sites do not change.
 */
using OnFrameCallback  = detail::CallbackWrapper<void(const ChannelFrame&) const>;
using OnClosedCallback = detail::CallbackWrapper<void(ChannelError reason) const>;
using OnErrorCallback  = detail::CallbackWrapper<void(ChannelError err,
                                                       std::string_view message) const>;

// ---------------------------------------------------------------------------
// ChannelConnection virtual base
// ---------------------------------------------------------------------------

/**
 * Connection-oriented byte channel between two peers.
 *
 * Send semantics:
 *   - `send_frame` is non-blocking; it returns ChannelError::None on
 *     success, or ChannelError::WouldBlock when the outbound queue is
 *     full. Other errors indicate a hard transport failure; the
 *     connection should be considered closed once `on_closed` fires.
 *   - `flush()` is best-effort: it asks the implementation to drain
 *     the outbound queue if possible. It does not block.
 *
 * Close semantics:
 *   - `close()` is idempotent. The first call schedules teardown and
 *     queues `on_closed(ChannelError::None)`. Subsequent calls are
 *     no-ops.
 *   - After `is_closed()` returns true, `send_frame` must return a
 *     non-None error and must not deliver further frames to the peer.
 */
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

using ChannelConnectionProxy = rusty::Box<ChannelConnectionBase>;

// ---------------------------------------------------------------------------
// ChannelListener facade
// ---------------------------------------------------------------------------

/**
 * Accept callback. Invoked once per inbound connection.
 *
 * The listener hands off ownership of the new `ChannelConnectionProxy`
 * to the callback. The callback typically attaches RPC server
 * dispatch handlers and registers the connection with the poll
 * thread.  Same `Arc<Function<...const>>`-backed wrapper as the
 * conn-level callbacks above.
 */
using OnAcceptCallback = detail::CallbackWrapper<void(ChannelConnectionProxy) const>;

/**
 * Server-side accept loop.
 *
 *   - `listen(addr)` binds and starts accepting. Returns
 *     `ChannelError::None` on success.
 *   - `close()` is idempotent. It refuses further accepts and tears
 *     down the listening socket. Existing accepted connections are
 *     unaffected.
 */
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

using ChannelListenerProxy = rusty::Box<ChannelListenerBase>;

// ---------------------------------------------------------------------------
// ChannelFactory facade
// ---------------------------------------------------------------------------

/**
 * Connection result returned by `connect`. On success, `connection`
 * is non-null; on failure, `error` describes why and `connection` is
 * a default-constructed (null) proxy.
 *
 * `connect` is synchronous from the caller's perspective: it either
 * produces a usable connection or an error. Implementations are free
 * to perform the underlying TCP handshake asynchronously, but they
 * must not invoke `on_frame`/`on_error` until at least one
 * `set_on_*` callback has been installed by the caller.
 */
struct ConnectResult {
    ChannelConnectionProxy connection;
    ChannelError           error = ChannelError::None;
};

/**
 * Constructs ChannelConnections and ChannelListeners. Each backend
 * (TCP, in-memory, future ones) provides its own factory.
 */
class ChannelFactoryBase {
 public:
  virtual ~ChannelFactoryBase() = default;
  virtual ConnectResult         connect(std::string_view)    = 0;
  virtual ChannelListenerProxy  make_listener()              = 0;
  virtual const char*           backend_name()    const      = 0;
};

using ChannelFactoryProxy = rusty::Box<ChannelFactoryBase>;

}  // namespace rrr
