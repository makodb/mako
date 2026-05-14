#pragma once

// import std; replacement — see <std_compat.hpp> for rationale.
#include <std_compat.hpp>

// @c-compat-added
#include <cstddef>
#include <cstdint>

#include <rusty/arc.hpp>
#include <rusty/cell.hpp>
#include <rusty/option.hpp>
#include <rusty/sync/weak.hpp>



#include "../base/threading.hpp"
#include "channel.hpp"
#include "frame_codec.hpp"
#include "pollable_proxy.h"
#include "../reactor/reactor.h"

/**
 * SRPC TCP Channel Backend.
 *
 * `TcpConnection` is one side of a connected TCP/Unix socket pair that
 * conforms to both the channel layer's `ChannelConnectionBase` and the
 * reactor's `Pollable` interface. Reads, writes, and callbacks are
 * driven from a single `PollThreadWorker`; the outbound queue is the
 * one piece of state guarded by a mutex because `send_frame()` may be
 * called from any thread (the channel contract pushes admission control
 * to the RPC layer above, but lets the channel's mutator be
 * thread-safe so the RPC layer doesn't need to round-trip through the
 * poll thread for every send).
 *
 * Lifetime / ownership:
 *
 *   - `TcpConnection` is heap-allocated and held as `rusty::Arc`.
 *   - Two proxies are produced from the same `Arc`:
 *       * `make_tcp_connection_channel_proxy`     — for the channel layer
 *         (RPC layer holds this).
 *       * `make_tcp_connection_pollable_proxy`    — for the poll thread
 *         (registered via `PollThread::add_proxy`).
 *     Both proxies own clones of the same `Arc`; the connection survives
 *     until both drop their references.
 *
 * Threading rules:
 *
 *   - All callbacks (`on_frame`, `on_closed`, `on_error`) are invoked on
 *     the poll thread. The RPC layer's reactor loops drive everything.
 *   - `send_frame` / `flush` / `close` / `is_closed` / `peer_address`
 *     and the `set_on_*` setters are safe to call from any thread.
 *
 * Close semantics:
 *
 *   - `close()` is idempotent. The first call shuts down the socket and
 *     queues `on_closed(ChannelError::None)`. Subsequent calls are
 *     no-ops.
 *   - Transport-level errors observed during `handle_read` /
 *     `handle_write` are reported via `on_error` and, if fatal, are
 *     followed by `on_closed`. `on_closed` fires *exactly once* per
 *     connection — guarded by an internal latch.
 *
 * Buffer limits:
 *
 *   - `send_frame` returns `ChannelError::WouldBlock` when the
 *     outbound queue exceeds an implementation-defined high-water mark
 *     (default 4 MiB). Callers (the RPC layer) apply admission control.
 */
namespace rrr {

class TcpConnection;

// Default high-water mark for the outbound byte queue. When the queue
// would grow past this size, `send_frame` returns
// `ChannelError::WouldBlock` instead of buffering further.
inline constexpr std::size_t kTcpConnectionOutboundHighWaterDefault =
    4u * 1024u * 1024u;  // 4 MiB

// ---------------------------------------------------------------------------
// TcpConnection
// ---------------------------------------------------------------------------

/**
 * One side of a connected stream socket.
 *
 * The fd is taken by ownership at construction; on destruction or
 * `close()` it is closed. The class is move-only via its `Arc` wrapper;
 * the class itself is non-copyable / non-movable so that pointers held
 * by adapters remain stable.
 */
class TcpConnection {
 public:
    /**
     * Construct from an already-connected file descriptor. The
     * `peer_address` is a human-readable label used only by
     * `peer_address()` and in log lines; the channel does not parse it.
     */
    TcpConnection(int fd, std::string peer_address);
    ~TcpConnection();

    TcpConnection(const TcpConnection&)            = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;
    TcpConnection(TcpConnection&&)                 = delete;
    TcpConnection& operator=(TcpConnection&&)      = delete;

    // Adjust the outbound queue's high-water mark (defaults to
    // `kTcpConnectionOutboundHighWaterDefault`). Must be called before
    // the connection is registered with a poll thread.
    void set_outbound_high_water(std::size_t bytes);

    // -----------------------------------------------------------------------
    // ChannelConnectionBase methods
    // -----------------------------------------------------------------------

    ChannelError send_frame(const ChannelFrame& frame);
    void         flush();
    void         close();
    bool         is_closed() const;
    std::string  peer_address() const;

    void set_on_frame(OnFrameCallback cb);
    void set_on_closed(OnClosedCallback cb);
    void set_on_error(OnErrorCallback cb);

    // -----------------------------------------------------------------------
    // Pollable interface
    // -----------------------------------------------------------------------
    //
    // These methods are invoked from the poll thread. They do not need
    // to be safe to call from arbitrary threads.

    int    fd() const;
    int    poll_mode() const;
    std::size_t content_size();
    bool   handle_read();
    int    handle_write();
    void   handle_error();
    bool   check_pending_write_update() const;

    // Wire a `PollThread` reference into the connection so non-poll-thread
    // callers of `send_frame(...)` can post `update_mode(fd, READ|WRITE)`
    // commands directly to wake `epoll_wait` instead of waiting for the
    // poll thread to poll the `pending_write_update_` flag.
    //
    // must be called *before* the connection
    // starts receiving outbound traffic. `TcpFactory::connect()` and
    // `TcpListener::handle_read()` (accept loop) wire this in immediately
    // after the connection is constructed and before its proxy is handed
    // to user code. When unset (e.g. the socketpair-driven unit tests),
    // `send_frame` falls back to the legacy flag-poll behavior — fine
    // because those tests exercise the connection from a single thread.
    void set_poll_thread(rusty::Arc<PollThread> pt);

 private:
    // Post-close cleanup and on_closed delivery. Idempotent — only the
    // first call actually fires the callback. Always invoked on the
    // poll thread or under the close latch held by `close()`.
    void deliver_on_closed_locked(ChannelError reason);

    // Try to drain bytes from the outbound queue into the socket. Used
    // both inline from `send_frame` (best-effort) and from
    // `handle_write`. Returns:
    //   - `ChannelError::None`        on success or partial success
    //   - `ChannelError::WouldBlock`  if EAGAIN/EWOULDBLOCK
    //   - other ChannelError values   on hard transport faults
    ChannelError drain_outbound_locked(
        std::vector<std::uint8_t>& buf);

    // Translate a POSIX errno from a socket call into a `ChannelError`.
    static ChannelError errno_to_channel_error(int err);

    // -----------------------------------------------------------------------
    // State
    // -----------------------------------------------------------------------

    int fd_;
    std::string peer_address_;

    std::size_t outbound_high_water_ = kTcpConnectionOutboundHighWaterDefault;

    // Outbound queue of fully-encoded frame bytes (header + payload).
    // Guarded because senders may live on threads other than the poll
    // thread.
    SpinMutex<std::vector<std::uint8_t>> outbound_{std::vector<std::uint8_t>{}};

    // Inbound stream reader. Touched only from the poll thread, no lock
    // needed.
    FrameStreamReader inbound_;

    // Latches / flags. `closed_` flips to true on first `close()` or
    // first fatal error. `on_closed_fired_` flips on first `on_closed`
    // delivery. Both are touched from the poll thread; `closed_` is
    // also read by `send_frame` from arbitrary threads, hence
    // `rusty::Cell<bool>` (atomic enough for the bool case).
    rusty::Cell<bool> closed_{false};
    rusty::Cell<bool> on_closed_fired_{false};

    // Set when something on the poll thread wants the worker to call
    // `update_mode` after the current callback returns (mirrors the
    // existing `ClientConnection::pending_write_update_`).
    rusty::Cell<bool> pending_write_update_{false};

    // Optional `PollThread` reference for non-poll-thread `send_frame`
    // callers to post `update_mode(fd, READ|WRITE)` directly. See
    // `set_poll_thread()`. None until wired up by the factory / accept
    // loop. Touched read-only from `send_frame` (any thread) and
    // `set_poll_thread` (which the factory only calls before any other
    // thread can observe the connection), so a plain `Option` without
    // a lock is fine.
    rusty::Option<rusty::Arc<PollThread>> poll_thread_{rusty::None};

    // User callbacks, last-writer-wins. Setters and reads are
    // serialized by the spinlock so that `send_frame` and the poll
    // thread don't race over them. (`OnXCallback` is the
    // Arc<Function const>-backed wrapper from channel.hpp; storage
    // ranks copy-cheaply for the channel callsites that need it.)
    SpinMutex<OnFrameCallback>  on_frame_{OnFrameCallback{}};
    SpinMutex<OnClosedCallback> on_closed_{OnClosedCallback{}};
    SpinMutex<OnErrorCallback>  on_error_{OnErrorCallback{}};
};

// ---------------------------------------------------------------------------
// Adapters
// ---------------------------------------------------------------------------
//
// The same `TcpConnection` is exposed via two proxies:
//
//   - The channel-facade proxy (for `ChannelConnectionBase`).
//   - The pollable proxy (for the reactor's poll thread).
//
// Each adapter holds an `Arc<TcpConnection>` and forwards method calls.
// Both adapters together ensure the connection lives until *both*
// proxies are released.
class TcpConnectionChannelAdapter : public ChannelConnectionBase {
 public:
    explicit TcpConnectionChannelAdapter(rusty::Arc<TcpConnection> conn)
        : conn_(std::move(conn)) {}

    ChannelError send_frame(const ChannelFrame& f) override { return mut_conn().send_frame(f); }
    void         flush() override                            { mut_conn().flush(); }
    void         close() override                            { mut_conn().close(); }
    bool         is_closed() const override                  { return conn_->is_closed(); }
    std::string  peer_address() const override               { return conn_->peer_address(); }

    void set_on_frame (OnFrameCallback  cb) override { mut_conn().set_on_frame (std::move(cb)); }
    void set_on_closed(OnClosedCallback cb) override { mut_conn().set_on_closed(std::move(cb)); }
    void set_on_error (OnErrorCallback  cb) override { mut_conn().set_on_error (std::move(cb)); }

 private:
    // `rusty::Arc<T>` exposes only `const T&` through its `operator->`
    // and `operator*`. The virtual base dispatches non-const methods
    // (e.g. `send_frame`, `close`, `set_on_*`) on the underlying
    // connection, so we cast through here. Mirrors the
    // `PollableTypedArcAdapter::mut_poll` idiom in `pollable_proxy.h`.
    TcpConnection& mut_conn() { return const_cast<TcpConnection&>(*conn_.get()); }
    rusty::Arc<TcpConnection> conn_;
};

class TcpConnectionPollableAdapter : public PollableBase {
 public:
    explicit TcpConnectionPollableAdapter(rusty::Arc<TcpConnection> conn)
        : conn_(std::move(conn)) {}

    int  fd() const override                          { return conn_->fd(); }
    int  poll_mode() const override                   { return conn_->poll_mode(); }
    std::size_t content_size() override               { return mut_conn().content_size(); }
    bool handle_read() override                       { return mut_conn().handle_read(); }
    int  handle_write() override                      { return mut_conn().handle_write(); }
    void handle_error() override                      { mut_conn().handle_error(); }
    void close() override                             { mut_conn().close(); }
    bool is_closed() const override                   { return conn_->is_closed(); }
    bool check_pending_write_update() const override  { return conn_->check_pending_write_update(); }

 private:
    TcpConnection& mut_conn() { return const_cast<TcpConnection&>(*conn_.get()); }
    rusty::Arc<TcpConnection> conn_;
};

inline ChannelConnectionProxy make_tcp_connection_channel_proxy(
    rusty::Arc<TcpConnection> conn) {
    return rusty::make_box<TcpConnectionChannelAdapter>(std::move(conn));
}

inline PollableProxy make_tcp_connection_pollable_proxy(
    rusty::Arc<TcpConnection> conn) {
    return rusty::make_box<TcpConnectionPollableAdapter>(std::move(conn));
}

// ---------------------------------------------------------------------------
// TcpListener
// ---------------------------------------------------------------------------

/**
 * Server-side TCP listener.
 *
 * Owns a single listening socket. `listen(addr)` creates the socket
 * (`socket(2)` + `setsockopt(SO_REUSEADDR)` + `bind(2)` + `listen(2)`)
 * and discovers the bound port via `getsockname(2)` so that callers can
 * pass `"127.0.0.1:0"` and recover the actual port through
 * `local_address()`.
 *
 * `handle_read()` runs an accept loop. Each successful `accept(2)`
 * yields a connected fd; the listener wraps it in a `TcpConnection`,
 * builds a `ChannelConnectionProxy` from it, and hands it to the
 * `on_accept` callback. The peer address is decoded from the
 * `accept(2)` `sockaddr` and stored on the connection so the RPC
 * layer can use it for logging without an extra syscall.
 *
 * Idempotent close: `close()` flips a latch, closes the listening fd,
 * and stops emitting `on_accept`. Connections already delivered
 * through `on_accept` are unaffected — the listener does not retain
 * references to them.
 *
 * Threading: the public mutators (`listen`, `close`, `set_on_*`) are
 * safe to call from any thread; the `Pollable` methods
 * (`handle_read`, `handle_error`, `poll_mode`, ...) run on the poll
 * thread.
 */
class TcpListener {
 public:
    TcpListener();
    ~TcpListener();

    TcpListener(const TcpListener&)            = delete;
    TcpListener& operator=(const TcpListener&) = delete;
    TcpListener(TcpListener&&)                 = delete;
    TcpListener& operator=(TcpListener&&)      = delete;

    // -----------------------------------------------------------------------
    // ChannelListenerBase methods
    // -----------------------------------------------------------------------

    /**
     * Bind and start listening on `addr` (format: `"host:port"`,
     * e.g., `"127.0.0.1:0"`, `"0.0.0.0:8080"`). Returns
     * `ChannelError::None` on success; on failure the listener is
     * left in a closed state and `local_address()` returns `""`.
     *
     * If `listen` is called more than once on the same listener, the
     * second call returns `ChannelError::AddressInUse` — listeners
     * are single-use. The factory / RPC layer above can construct a
     * fresh listener if it needs to rebind.
     */
    ChannelError listen(std::string_view addr);
    void         close();
    bool         is_closed() const;
    std::string  local_address() const;

    void set_on_accept(OnAcceptCallback cb);
    void set_on_error (OnErrorCallback  cb);

    // -----------------------------------------------------------------------
    // Pollable methods
    // -----------------------------------------------------------------------

    int    fd() const;
    int    poll_mode() const;
    std::size_t content_size();
    bool   handle_read();
    int    handle_write();
    void   handle_error();
    bool   check_pending_write_update() const;

    // -----------------------------------------------------------------------
    // Auto-registration with a PollThread (used by `TcpFactory`).
    // -----------------------------------------------------------------------

    /**
     * Configure auto-registration with a poll thread. Must be called
     * BEFORE `listen()` for it to take effect. When set, a successful
     * `listen(addr)` registers this listener with the poll thread,
     * and each `handle_read`-accepted `TcpConnection` is also
     * registered automatically.
     *
     * `self_weak` must be a weak reference to the same `Arc` that
     * owns this listener; the factory wires it up after Arc creation
     * because `Arc` does not expose `enable_shared_from_this`-style
     * access.
     */
    void set_poll_thread(rusty::Arc<PollThread> pt);
    void set_self_weak(rusty::sync::Weak<TcpListener> self_weak);

 private:
    // Translate a POSIX errno from a listening syscall into a
    // ChannelError.
    static ChannelError listen_errno_to_channel_error(int err);

    // -----------------------------------------------------------------------
    // State
    // -----------------------------------------------------------------------

    int         listen_fd_ = -1;
    std::string bound_address_;

    rusty::Cell<bool> closed_{false};
    rusty::Cell<bool> listened_{false};

    rusty::Option<rusty::Arc<PollThread>>      poll_thread_{rusty::None};
    rusty::Option<rusty::sync::Weak<TcpListener>> self_weak_{rusty::None};

    SpinMutex<OnAcceptCallback> on_accept_{OnAcceptCallback{}};
    SpinMutex<OnErrorCallback>  on_error_ {OnErrorCallback{}};
};

class TcpListenerChannelAdapter : public ChannelListenerBase {
 public:
    explicit TcpListenerChannelAdapter(rusty::Arc<TcpListener> listener)
        : listener_(std::move(listener)) {}

    ChannelError listen(std::string_view a) override { return mut_listener().listen(a); }
    void         close() override                    { mut_listener().close(); }
    bool         is_closed() const override          { return listener_->is_closed(); }
    std::string  local_address() const override      { return listener_->local_address(); }

    void set_on_accept(OnAcceptCallback cb) override { mut_listener().set_on_accept(std::move(cb)); }
    void set_on_error (OnErrorCallback  cb) override { mut_listener().set_on_error (std::move(cb)); }

 private:
    TcpListener& mut_listener() { return const_cast<TcpListener&>(*listener_.get()); }
    rusty::Arc<TcpListener> listener_;
};

class TcpListenerPollableAdapter : public PollableBase {
 public:
    explicit TcpListenerPollableAdapter(rusty::Arc<TcpListener> listener)
        : listener_(std::move(listener)) {}

    int  fd() const override                          { return listener_->fd(); }
    int  poll_mode() const override                   { return listener_->poll_mode(); }
    std::size_t content_size() override               { return mut_listener().content_size(); }
    bool handle_read() override                       { return mut_listener().handle_read(); }
    int  handle_write() override                      { return mut_listener().handle_write(); }
    void handle_error() override                      { mut_listener().handle_error(); }
    void close() override                             { mut_listener().close(); }
    bool is_closed() const override                   { return listener_->is_closed(); }
    bool check_pending_write_update() const override  { return listener_->check_pending_write_update(); }

 private:
    TcpListener& mut_listener() { return const_cast<TcpListener&>(*listener_.get()); }
    rusty::Arc<TcpListener> listener_;
};

inline ChannelListenerProxy make_tcp_listener_channel_proxy(
    rusty::Arc<TcpListener> listener) {
    return rusty::make_box<TcpListenerChannelAdapter>(std::move(listener));
}

inline PollableProxy make_tcp_listener_pollable_proxy(
    rusty::Arc<TcpListener> listener) {
    return rusty::make_box<TcpListenerPollableAdapter>(std::move(listener));
}

// ---------------------------------------------------------------------------
// TcpFactory
// ---------------------------------------------------------------------------

/**
 * Backend-aware factory for the TCP channel. Holds a reference to a
 * `PollThread`; uses it to auto-register newly created connections
 * (from `connect`) and listeners (from `make_listener`, on a
 * successful `listen`).
 *
 * Conforms to `ChannelFactoryBase`:
 *
 *   - `connect(addr)` synchronously does `socket(2) + connect(2)`,
 *     wraps the fd in a `TcpConnection`, registers it with the poll
 *     thread, and returns a `ChannelConnectionProxy`. On failure the
 *     `ConnectResult::error` field carries the translated
 *     `ChannelError` and `connection` is null.
 *   - `make_listener()` constructs a `TcpListener` pre-wired with
 *     the factory's `PollThread`. The listener auto-registers itself
 *     with the poll thread on a successful `listen()` call, and
 *     auto-registers each connection accepted via `handle_read`.
 *   - `backend_name()` returns `"tcp"`.
 *
 * Thread safety: `connect` is safe to call from any thread; the
 * registration is sent to the poll thread asynchronously via the
 * MPSC command channel, matching `PollThread::add_proxy` semantics.
 */
class TcpFactory {
 public:
    explicit TcpFactory(rusty::Arc<PollThread> poll_thread);
    ~TcpFactory() = default;

    TcpFactory(const TcpFactory&)            = delete;
    TcpFactory& operator=(const TcpFactory&) = delete;
    TcpFactory(TcpFactory&&)                 = delete;
    TcpFactory& operator=(TcpFactory&&)      = delete;

    // ChannelFactoryBase methods.
    ConnectResult         connect(std::string_view addr);
    ChannelListenerProxy  make_listener();
    const char*           backend_name() const { return "tcp"; }

    // Optional override for the connect-side IPv4 timeout (default
    // 5s). Only used when the kernel doesn't fail-fast on
    // unreachable destinations. Set to 0 for blocking behavior.
    void set_connect_timeout_ms(int timeout_ms) { connect_timeout_ms_ = timeout_ms; }

 private:
    static ChannelError connect_errno_to_channel_error(int err);

    rusty::Arc<PollThread> poll_thread_;
    int                    connect_timeout_ms_ = 5000;
};

class TcpFactoryAdapter : public ChannelFactoryBase {
 public:
    explicit TcpFactoryAdapter(rusty::Arc<TcpFactory> factory)
        : factory_(std::move(factory)) {}

    ConnectResult        connect(std::string_view addr) override { return mut_factory().connect(addr); }
    ChannelListenerProxy make_listener() override                { return mut_factory().make_listener(); }
    const char*          backend_name() const override           { return factory_->backend_name(); }

 private:
    TcpFactory& mut_factory() { return const_cast<TcpFactory&>(*factory_.get()); }
    rusty::Arc<TcpFactory> factory_;
};

inline ChannelFactoryProxy make_tcp_factory_proxy(rusty::Arc<TcpFactory> factory) {
    return rusty::make_box<TcpFactoryAdapter>(std::move(factory));
}

}  // namespace rrr
