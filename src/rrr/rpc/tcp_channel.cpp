// rrr.tcp_channel — TCP/Unix-socket channel backend (formerly
// tcp_channel.hpp + tcp_channel.cpp).
//
// `TcpConnection` is one side of a connected TCP/Unix socket pair that
// conforms to both the channel layer's `ChannelConnectionBase` and the
// reactor's `Pollable` interface. Reads, writes, and callbacks are
// driven from a single `PollThreadWorker`; the outbound queue is the
// one piece of state guarded by a mutex because `send_frame()` may be
// called from any thread.
//
// Lifetime / ownership: `TcpConnection` is heap-allocated and held as
// `rusty::Arc`. Two proxies (channel-facade and pollable) are produced
// from the same Arc and share the connection.
module;

#include <cstddef>
#include <cstdint>

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#include <rusty/arc.hpp>
#include <rusty/box.hpp>
#include <rusty/cell.hpp>
#include <rusty/option.hpp>
#include <rusty/net.hpp>
#include <rusty/net/tcp.hpp>
#include <rusty/os/fd.hpp>
#include <rusty/sync/weak.hpp>

export module rrr.tcp_channel;

import std;
import rrr.channel;
import rrr.epoll_wrapper;
import rrr.frame_codec;
import rrr.pollable_proxy;
import rrr.reactor;
import rrr.threading;

// ===========================================================================
// Class declarations (from former tcp_channel.hpp)
// ===========================================================================
// @safe - TCP channel backend. State on TcpConnection / TcpListener /
// TcpFactory is rusty::Cell / SpinMutex / Arc / Option. Methods that
// drive socket / fd syscalls (socket / bind / listen / accept / send /
// recv / close / fcntl / getsockname) or that thread through
// const_cast `mut_conn` / `mut_listener` / `mut_factory` helpers
// carry per-method `// @unsafe`. The Phase 1 TcpListener half was
// already labeled; this iteration extends the same labeling to
// TcpConnection, its two adapters, TcpFactory, and the adapter set.
export namespace rrr {

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
// @safe - see file header. Socket-fd syscall methods (send_frame,
// flush, close, handle_read, handle_write, handle_error,
// drain_outbound_locked, dtor, deliver_on_closed_locked) carry their
// own per-method `// @unsafe`.
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

    // Owned file descriptor — RAII-closes on drop. Replaces the
    // previous raw `int fd_` (kept the name) so call sites that
    // mutate the fd life cycle do so by move-assigning a fresh
    // `OwnedFd{}` (which closes the prior fd in its destructor) and
    // call sites that need the integer pass `fd_.as_raw_fd()`.
    rusty::os::fd::OwnedFd fd_;
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

    // @unsafe - forwards through mut_conn() const_cast.
    ChannelError send_frame(const ChannelFrame& f) override { return mut_conn().send_frame(f); }
    // @unsafe - forwards through mut_conn() const_cast.
    void         flush() override                            { mut_conn().flush(); }
    // @unsafe - forwards through mut_conn() const_cast.
    void         close() override                            { mut_conn().close(); }
    // @unsafe - forwards into TcpConnection::is_closed (touches closed_ Cell).
    bool         is_closed() const override                  { return conn_->is_closed(); }
    // @unsafe - forwards into TcpConnection::peer_address (touches peer_address_ string).
    std::string  peer_address() const override               { return conn_->peer_address(); }

    // @unsafe - forwards through mut_conn() const_cast.
    void set_on_frame (OnFrameCallback  cb) override { mut_conn().set_on_frame (std::move(cb)); }
    // @unsafe - forwards through mut_conn() const_cast.
    void set_on_closed(OnClosedCallback cb) override { mut_conn().set_on_closed(std::move(cb)); }
    // @unsafe - forwards through mut_conn() const_cast.
    void set_on_error (OnErrorCallback  cb) override { mut_conn().set_on_error (std::move(cb)); }

 private:
    // `rusty::Arc<T>` exposes only `const T&` through its `operator->`
    // and `operator*`. The virtual base dispatches non-const methods
    // (e.g. `send_frame`, `close`, `set_on_*`) on the underlying
    // connection, so we cast through here. Mirrors the
    // `PollableTypedArcAdapter::mut_poll` idiom in `pollable_proxy.h`.
    // @unsafe - const_cast through Arc::get<T*>().
    TcpConnection& mut_conn() { return const_cast<TcpConnection&>(*conn_.get()); }
    rusty::Arc<TcpConnection> conn_;
};

class TcpConnectionPollableAdapter : public PollableBase {
 public:
    explicit TcpConnectionPollableAdapter(rusty::Arc<TcpConnection> conn)
        : conn_(std::move(conn)) {}

    // @unsafe - forwards into TcpConnection::fd (returns the raw fd int).
    int  fd() const override                          { return conn_->fd(); }
    // @unsafe - forwards into TcpConnection::poll_mode.
    int  poll_mode() const override                   { return conn_->poll_mode(); }
    // @unsafe - forwards through mut_conn() const_cast.
    std::size_t content_size() override               { return mut_conn().content_size(); }
    // @unsafe - forwards through mut_conn() const_cast (recv syscall path).
    bool handle_read() override                       { return mut_conn().handle_read(); }
    // @unsafe - forwards through mut_conn() const_cast (send syscall path).
    int  handle_write() override                      { return mut_conn().handle_write(); }
    // @unsafe - forwards through mut_conn() const_cast.
    void handle_error() override                      { mut_conn().handle_error(); }
    // @unsafe - forwards through mut_conn() const_cast.
    void close() override                             { mut_conn().close(); }
    // @unsafe - forwards into TcpConnection::is_closed.
    bool is_closed() const override                   { return conn_->is_closed(); }
    // @unsafe - forwards into TcpConnection::check_pending_write_update.
    bool check_pending_write_update() const override  { return conn_->check_pending_write_update(); }

 private:
    // @unsafe - const_cast through Arc::get<T*>().
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
// @safe - State is rusty::Cell / Option / SpinMutex / Arc /
// rusty::os::fd::OwnedFd (RAII-closes listen_fd_ on drop). Methods
// that genuinely touch the raw fd via syscalls (listen, fd,
// handle_read) carry their own `// @unsafe` overrides at the
// out-of-class definitions; `close()` is now @safe — it just
// move-assigns an empty OwnedFd, whose destructor handles the
// ::close().
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

    // Owned rusty::net::TcpListener — wraps the socket/bind/listen
    // syscalls and RAII-closes the listen fd on drop. Move-only;
    // default-constructed (`!listener_.is_bound()`) means we haven't
    // called listen() yet.
    rusty::net::TcpListener listener_;
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

    // @unsafe - forwards through mut_listener() const_cast.
    ChannelError listen(std::string_view a) override { return mut_listener().listen(a); }
    // @unsafe - forwards through mut_listener() const_cast.
    void         close() override                    { mut_listener().close(); }
    bool         is_closed() const override          { return listener_->is_closed(); }
    std::string  local_address() const override      { return listener_->local_address(); }

    // @unsafe - forwards through mut_listener() const_cast.
    void set_on_accept(OnAcceptCallback cb) override { mut_listener().set_on_accept(std::move(cb)); }
    // @unsafe - forwards through mut_listener() const_cast.
    void set_on_error (OnErrorCallback  cb) override { mut_listener().set_on_error (std::move(cb)); }

 private:
    // @unsafe - const_cast through Arc::get<T*>().
    TcpListener& mut_listener() { return const_cast<TcpListener&>(*listener_.get()); }
    rusty::Arc<TcpListener> listener_;
};

class TcpListenerPollableAdapter : public PollableBase {
 public:
    explicit TcpListenerPollableAdapter(rusty::Arc<TcpListener> listener)
        : listener_(std::move(listener)) {}

    // @unsafe - forwards into TcpListener::fd (raw listen_fd_).
    int  fd() const override                          { return listener_->fd(); }
    int  poll_mode() const override                   { return listener_->poll_mode(); }
    // @unsafe - forwards through mut_listener() const_cast.
    std::size_t content_size() override               { return mut_listener().content_size(); }
    // @unsafe - forwards through mut_listener() const_cast (accept syscall path).
    bool handle_read() override                       { return mut_listener().handle_read(); }
    // @unsafe - forwards through mut_listener() const_cast.
    int  handle_write() override                      { return mut_listener().handle_write(); }
    // @unsafe - forwards through mut_listener() const_cast.
    void handle_error() override                      { mut_listener().handle_error(); }
    // @unsafe - forwards through mut_listener() const_cast.
    void close() override                             { mut_listener().close(); }
    bool is_closed() const override                   { return listener_->is_closed(); }
    bool check_pending_write_update() const override  { return listener_->check_pending_write_update(); }

 private:
    // @unsafe - const_cast through Arc::get<T*>().
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
    ConnectResult                       connect(std::string_view addr);
    rusty::Option<ChannelListenerProxy> make_listener();
    const char*                         backend_name() const { return "tcp"; }

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

    // @unsafe - forwards through mut_factory() const_cast (socket+connect path).
    ConnectResult                       connect(std::string_view addr) override { return mut_factory().connect(addr); }
    // @unsafe - forwards through mut_factory() const_cast.
    rusty::Option<ChannelListenerProxy> make_listener() override                { return mut_factory().make_listener(); }
    const char*                         backend_name() const override           { return factory_->backend_name(); }

 private:
    // @unsafe - const_cast through Arc::get<T*>().
    TcpFactory& mut_factory() { return const_cast<TcpFactory&>(*factory_.get()); }
    rusty::Arc<TcpFactory> factory_;
};

inline ChannelFactoryProxy make_tcp_factory_proxy(rusty::Arc<TcpFactory> factory) {
    return rusty::make_box<TcpFactoryAdapter>(std::move(factory));
}

}  // export namespace rrr

// ===========================================================================
// Implementation (from former tcp_channel.cpp)
// ===========================================================================
// @safe - impl namespace. Out-of-class definitions inherit their
// per-method `// @safe` / `// @unsafe` from the matching declarations
// in the export namespace; the syscall-heavy bodies (TcpConnection::
// dtor / send_frame / drain_outbound_locked / handle_read /
// handle_write / handle_error / close, TcpListener::listen / close /
// handle_read / handle_error / dtor, TcpFactory::connect /
// make_listener) all carry per-method `// @unsafe` at their
// definition sites.
namespace rrr {

namespace {

// One-shot recv buffer used by `handle_read` to copy bytes off the
// socket before handing them to `FrameStreamReader`. The reader uses
// its own contiguous buffer; this is just a syscall-side scratchpad.
constexpr std::size_t kRecvScratchBytes = 64 * 1024;

}  // namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

TcpConnection::TcpConnection(int fd, std::string peer_address)
    : fd_(rusty::os::fd::OwnedFd::from_raw_fd(fd)),
      peer_address_(std::move(peer_address)) {}

TcpConnection::~TcpConnection() {
    // OwnedFd's destructor handles the ::close() — no manual cleanup
    // needed. We still latch closed_ so any concurrent observer sees
    // the closed state.
    closed_.set(true);
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

void TcpConnection::set_outbound_high_water(std::size_t bytes) {
    outbound_high_water_ = bytes;
}

// ---------------------------------------------------------------------------
// Channel-facade methods
// ---------------------------------------------------------------------------

ChannelError TcpConnection::send_frame(const ChannelFrame& frame) {
    if (closed_.get()) {
        return ChannelError::ConnectionReset;
    }
    if (frame.size > 0 && frame.payload == nullptr) {
        return ChannelError::Internal;
    }
    if (frame.size > static_cast<std::size_t>(kMaxFramePayloadSize)) {
        return ChannelError::Internal;
    }

    // Determine whether the caller asked for the response extended-
    // header flag by sniffing the frame size's high bit. The channel
    // layer treats `payload` as opaque; the encoded byte stream that
    // crosses the wire carries the flag in the size header. To stay
    // wire-compatible with the existing inline send paths
    // (`ServerConnection::reply` writes the extended-flag bit into the
    // size prefix, NOT into `payload`), we expose the extended flag
    // through a separate convention: `frame.size` in the channel's
    // `ChannelFrame` is the *raw payload byte count* (no flag), and
    // the codec is responsible for setting the flag when the RPC
    // layer constructs a response. For sub-leaf 3a we always emit
    // request-style frames (flag clear). The server-side flag handling
    // lands when the RPC layer is refactored onto channel.
    constexpr bool extended_header_flag = false;

    auto guard = outbound_.lock().unwrap();
    auto& buf = *guard;

    // Reject when the queue is already past the high water — we never
    // append to a buffer that's already over budget so backpressure is
    // strictly bounded.
    if (buf.size() >= outbound_high_water_) {
        return ChannelError::WouldBlock;
    }

    if (!frame_codec_encode_into(buf,
                                 frame.payload,
                                 static_cast<std::int32_t>(frame.size),
                                 extended_header_flag)) {
        return ChannelError::Internal;
    }

    // Wake the poll thread so the new outbound bytes actually leave
    // the buffer. Two paths, mirroring the legacy fd path's idiom in
    // `ClientConnection::replay_pending_requests`:
    //
    //   * On the poll thread: just set the deferred flag — the
    //     poll_loop will pick it up at the bottom of the current
    //     iteration via `check_pending_write_update`.
    //
    //   * Off the poll thread (the common case — RPC user threads
    //     calling `send_frame` from `dispatch_frame_via_channel`):
    //     post `update_mode(fd, READ|WRITE)` directly. Posting writes
    //     to the mpsc channel's eventfd, which wakes the poll thread's
    //     `epoll_wait` immediately. Without this, multi-threaded
    //     senders contend on the non-atomic `pending_write_update_`
    //     `Cell<bool>` and lose wake-ups, producing the
    //     `MultiThreadedStressTest` 100-thread wedge documented in
    //     `docs/TODO-srpc.md` sub-leaf 4g1b.
    //
    // The `poll_thread_` slot may be `None` for unit tests that drive
    // `TcpConnection` directly via `socketpair(2)` without a poll
    // thread (those tests are single-threaded — flag-poll is fine).
    if (poll_thread_.is_some() && !PollThreadWorker::is_on_poll_thread()) {
        poll_thread_.as_ref().unwrap()->update_mode(
            fd_.as_raw_fd(), PollMode::READ | PollMode::WRITE);
    } else {
        pending_write_update_.set(true);
    }
    return ChannelError::None;
}

void TcpConnection::set_poll_thread(rusty::Arc<PollThread> pt) {
    poll_thread_ = rusty::Some(std::move(pt));
}

// @unsafe - drives drain_outbound_locked (which is @unsafe for raw
// `uint8_t*` arithmetic + send syscall).
void TcpConnection::flush() {
    if (closed_.get()) return;

    auto guard = outbound_.lock().unwrap();
    if (guard->empty()) return;

    // Best-effort: try to drain immediately. Errors here are reported
    // via the callbacks set on this connection; the caller of `flush`
    // does not get a return code (matching the channel facade).
    ChannelError result = drain_outbound_locked(*guard);
    if (result != ChannelError::None && result != ChannelError::WouldBlock) {
        // Defer error/close delivery to the poll thread to keep the
        // callback contract (poll-thread-only) honest. We just mark the
        // closed flag; the next `handle_read` / `handle_write` will fire
        // the callbacks. If `flush` is being called from the poll
        // thread itself, fire immediately.
        // For sub-leaf 3a we don't have an independent poll-thread
        // hand-off; tests drive Pollable methods directly. Mark closed
        // and let the next poll cycle observe it.
        closed_.set(true);
    }
}

void TcpConnection::close() {
    // Latch on first call. Idempotent for concurrent callers because
    // `Cell<bool>::set(true)` is a release-store; the first to set
    // wins, the rest observe `closed_.get() == true` here.
    if (closed_.get()) {
        return;
    }
    closed_.set(true);

    // Shutdown the write side to flush kernel buffers and signal the
    // peer; then drop the OwnedFd to RAII-close. `::shutdown` may fail
    // if the socket is already half-closed — we ignore that.
    if (fd_.is_valid()) {
        // @unsafe { ::shutdown is libc — initiates orderly TCP close. }
        { ::shutdown(fd_.as_raw_fd(), SHUT_RDWR); }
        fd_ = rusty::os::fd::OwnedFd{};
    }

    // Deliver `on_closed(ChannelError::None)` exactly once.
    deliver_on_closed_locked(ChannelError::None);
}

bool TcpConnection::is_closed() const {
    return closed_.get();
}

std::string TcpConnection::peer_address() const {
    return peer_address_;
}

void TcpConnection::set_on_frame(OnFrameCallback cb) {
    auto guard = on_frame_.lock().unwrap();
    *guard = std::move(cb);
}

void TcpConnection::set_on_closed(OnClosedCallback cb) {
    auto guard = on_closed_.lock().unwrap();
    *guard = std::move(cb);
}

void TcpConnection::set_on_error(OnErrorCallback cb) {
    auto guard = on_error_.lock().unwrap();
    *guard = std::move(cb);
}

// ---------------------------------------------------------------------------
// Pollable methods
// ---------------------------------------------------------------------------

int TcpConnection::fd() const {
    return fd_.as_raw_fd();
}

int TcpConnection::poll_mode() const {
    int mode = PollMode::READ;
    auto guard = outbound_.lock().unwrap();
    if (!guard->empty()) {
        mode |= PollMode::WRITE;
    }
    return mode;
}

std::size_t TcpConnection::content_size() {
    auto guard = outbound_.lock().unwrap();
    return guard->size() + inbound_.buffered_bytes();
}

// @unsafe - recv(2) syscall into a raw `char` scratch buffer +
// FrameStreamReader::append / next_frame / consume_frame are all
// @unsafe + raw `uint8_t*` payload pointers stored on the FrameView.
bool TcpConnection::handle_read() {
    if (closed_.get()) return false;

    std::uint8_t scratch[kRecvScratchBytes];
    bool any_progress = false;

    while (true) {
        ssize_t n;
        // @unsafe { ::recv libc syscall — reads raw bytes into the
        //           scratch buffer. }
        { n = ::recv(fd_.as_raw_fd(), scratch, sizeof(scratch), 0); }
        if (n > 0) {
            inbound_.append(scratch, static_cast<std::size_t>(n));
            any_progress = true;
            // Drain the syscall in a loop so edge-triggered epoll users
            // don't lose readiness; cap at one iteration when the
            // syscall returns less than the scratch (level-triggered
            // would be fine either way).
            if (static_cast<std::size_t>(n) < sizeof(scratch)) {
                break;
            }
            continue;
        }
        if (n == 0) {
            // Peer closed cleanly. Signal the listener; do not fire
            // on_error — this isn't a fault, it's a graceful close.
            closed_.set(true);
            fd_ = rusty::os::fd::OwnedFd{};  // RAII close
            deliver_on_closed_locked(ChannelError::None);
            return false;
        }
        // n < 0
        const int err = errno;
        if (err == EAGAIN || err == EWOULDBLOCK) {
            break;
        }
        if (err == EINTR) {
            continue;
        }
        // Hard transport error.
        const ChannelError ch = errno_to_channel_error(err);
        {
            auto guard = on_error_.lock().unwrap();
            if (*guard) {
                (*guard)(ch, std::strerror(err));
            }
        }
        closed_.set(true);
        fd_ = rusty::os::fd::OwnedFd{};  // RAII close
        deliver_on_closed_locked(ch);
        return false;
    }

    // Now drain any complete frames out of the inbound buffer.
    while (true) {
        FrameView v{};
        const FrameDecodeStatus s = inbound_.next_frame(v);
        if (s == FrameDecodeStatus::Complete) {
            ChannelFrame cf{v.payload, v.payload_size};
            {
                auto guard = on_frame_.lock().unwrap();
                if (*guard) {
                    (*guard)(cf);
                }
            }
            inbound_.consume_frame();
            continue;
        }
        if (s == FrameDecodeStatus::NeedMoreBytes) {
            break;
        }
        // Malformed.
        {
            auto guard = on_error_.lock().unwrap();
            if (*guard) {
                (*guard)(ChannelError::Internal,
                         "malformed frame on inbound stream");
            }
        }
        closed_.set(true);
        fd_ = rusty::os::fd::OwnedFd{};  // RAII close
        inbound_.reset();
        deliver_on_closed_locked(ChannelError::Internal);
        return false;
    }

    return any_progress;
}

// @unsafe - drives drain_outbound_locked (which is @unsafe for raw
// `uint8_t*` arithmetic + send syscall).
int TcpConnection::handle_write() {
    if (closed_.get()) return PollMode::NO_CHANGE;

    auto guard = outbound_.lock().unwrap();
    auto& buf = *guard;
    if (buf.empty()) {
        return PollMode::READ;
    }

    const ChannelError result = drain_outbound_locked(buf);

    if (result == ChannelError::None) {
        if (buf.empty()) {
            return PollMode::READ;
        }
        return PollMode::NO_CHANGE;
    }
    if (result == ChannelError::WouldBlock) {
        return PollMode::NO_CHANGE;
    }
    // Hard transport error.
    {
        auto err_guard = on_error_.lock().unwrap();
        if (*err_guard) {
            (*err_guard)(result, "outbound write failed");
        }
    }
    closed_.set(true);
    fd_ = rusty::os::fd::OwnedFd{};  // RAII close
    deliver_on_closed_locked(result);
    return PollMode::READ;  // Stop watching writes; closed.
}

void TcpConnection::handle_error() {
    if (closed_.get()) return;
    {
        auto guard = on_error_.lock().unwrap();
        if (*guard) {
            (*guard)(ChannelError::Internal, "epoll/poll signaled error");
        }
    }
    close();  // Idempotent; fires on_closed.
}

bool TcpConnection::check_pending_write_update() const {
    if (!pending_write_update_.get()) return false;
    pending_write_update_.set(false);
    return true;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// @unsafe - raw `uint8_t*` pointer arithmetic + send(2) syscall +
// pointer dereference on the outbound buffer.
ChannelError TcpConnection::drain_outbound_locked(
    std::vector<std::uint8_t>& buf) {

    std::size_t offset = 0;
    while (offset < buf.size()) {
        const std::size_t remaining = buf.size() - offset;
        // @unsafe — system call
        ssize_t n = ::send(fd_.as_raw_fd(), buf.data() + offset, remaining, MSG_NOSIGNAL);
        if (n > 0) {
            offset += static_cast<std::size_t>(n);
            if (static_cast<std::size_t>(n) < remaining) {
                // Partial write — keep trying.
                continue;
            }
            continue;
        }
        if (n == 0) {
            // send returning 0 with non-zero `remaining` is treated as
            // a transport reset.
            return ChannelError::ConnectionReset;
        }
        const int err = errno;
        if (err == EAGAIN || err == EWOULDBLOCK) {
            break;  // Caller will retry on next handle_write.
        }
        if (err == EINTR) {
            continue;
        }
        // Hard error. Drop the bytes we couldn't send (the connection
        // is dead anyway).
        if (offset > 0) {
            buf.erase(buf.begin(),
                      buf.begin() + static_cast<std::ptrdiff_t>(offset));
        } else {
            buf.clear();
        }
        return errno_to_channel_error(err);
    }

    if (offset > 0) {
        if (offset == buf.size()) {
            buf.clear();
        } else {
            buf.erase(buf.begin(),
                      buf.begin() + static_cast<std::ptrdiff_t>(offset));
        }
    }
    if (offset == 0 && !buf.empty()) {
        return ChannelError::WouldBlock;
    }
    return ChannelError::None;
}

void TcpConnection::deliver_on_closed_locked(ChannelError reason) {
    if (on_closed_fired_.get()) {
        return;
    }
    on_closed_fired_.set(true);
    auto guard = on_closed_.lock().unwrap();
    if (*guard) {
        (*guard)(reason);
    }
}

ChannelError TcpConnection::errno_to_channel_error(int err) {
    switch (err) {
        case ECONNREFUSED:                 return ChannelError::ConnectionRefused;
        case ECONNRESET:
        case EPIPE:
        case ENOTCONN:                     return ChannelError::ConnectionReset;
        case ETIMEDOUT:                    return ChannelError::Timeout;
        case EADDRINUSE:                   return ChannelError::AddressInUse;
        case EADDRNOTAVAIL:                return ChannelError::AddressInvalid;
        case EACCES:
        case EPERM:                        return ChannelError::PermissionDenied;
        case EMFILE:
        case ENFILE:                       return ChannelError::TooManyOpenFiles;
        default:                           return ChannelError::Internal;
    }
}

// ===========================================================================
// TcpListener
// ===========================================================================

namespace {

// @safe - Map a rusty::io::Error::Kind to a ChannelError. Used at the
// boundary where `rusty::net::*` operations return Result<T,io::Error>
// and we need to surface the failure as a ChannelError on the
// listener / connection API.
ChannelError io_kind_to_channel_error(rusty::io::Error::Kind kind) {
    switch (kind) {
        case rusty::io::Error::Kind::ConnectionRefused: return ChannelError::ConnectionRefused;
        case rusty::io::Error::Kind::ConnectionReset:
        case rusty::io::Error::Kind::ConnectionAborted:
        case rusty::io::Error::Kind::NotConnected:
        case rusty::io::Error::Kind::BrokenPipe:        return ChannelError::ConnectionReset;
        case rusty::io::Error::Kind::TimedOut:          return ChannelError::Timeout;
        case rusty::io::Error::Kind::AddrInUse:         return ChannelError::AddressInUse;
        case rusty::io::Error::Kind::AddrNotAvailable:  return ChannelError::AddressInvalid;
        case rusty::io::Error::Kind::InvalidInput:      return ChannelError::AddressInvalid;
        case rusty::io::Error::Kind::PermissionDenied:  return ChannelError::PermissionDenied;
        case rusty::io::Error::Kind::WouldBlock:        return ChannelError::WouldBlock;
        default:                                        return ChannelError::Internal;
    }
}

// Set the FD non-blocking. Returns 0 on success, errno on failure.
int set_nonblocking_fd(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return errno;
    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return errno;
    return 0;
}

// Note: `sockaddr_to_string` lived here before the Phase C migration —
// every caller now uses `rusty::net::socket_addr_v4_to_string` directly.

// Note: address parsing now lives in `rusty::net::socket_addr_v4_from_str`
// (in `rusty/net/tcp.hpp`). The legacy `parse_inet4_addr(addr, out)`
// helper was removed in the Phase C migration — call sites now go
// through the rusty::net helpers directly.

}  // namespace

TcpListener::TcpListener() = default;

TcpListener::~TcpListener() = default;  // rusty::net::TcpListener RAII-closes

// @safe - listen path now delegates to `rusty::net::TcpListener::bind`
// (which encapsulates socket / setsockopt(SO_REUSEADDR) / bind / listen),
// `socket_addr_v4_from_str` for address parsing, and `set_nonblocking`
// for the F_GETFL/F_SETFL fcntl pair. All libc calls live behind the
// wrapper boundary; this body is pure flow control over Result types.
ChannelError TcpListener::listen(std::string_view addr) {
    if (closed_.get()) {
        return ChannelError::AddressInUse;
    }
    if (listened_.get()) {
        return ChannelError::AddressInUse;
    }

    auto parse_result = rusty::net::socket_addr_v4_from_str(addr);
    if (parse_result.is_err()) {
        return ChannelError::AddressInvalid;
    }
    auto bind_result = rusty::net::TcpListener::bind(parse_result.unwrap());
    if (bind_result.is_err()) {
        return io_kind_to_channel_error(bind_result.unwrap_err().kind());
    }
    listener_ = bind_result.unwrap();

    auto nonblock_result = listener_.set_nonblocking(true);
    if (nonblock_result.is_err()) {
        ChannelError ch = io_kind_to_channel_error(
            nonblock_result.unwrap_err().kind());
        listener_ = rusty::net::TcpListener{};  // RAII close
        return ch;
    }

    // Discover actual bound address (port may have been 0).
    auto local_result = listener_.local_addr();
    if (local_result.is_ok()) {
        bound_address_ = rusty::net::socket_addr_v4_to_string(
            local_result.unwrap());
    } else {
        bound_address_ = std::string(addr);
    }

    listened_.set(true);

    // Auto-register with the poll thread if the factory wired one in.
    // The factory pre-installs `poll_thread_` and a weak self-ref
    // before handing the listener proxy to the user; we upgrade the
    // weak ref and hand a `PollableProxy` clone to the poll thread.
    if (poll_thread_.is_some() && self_weak_.is_some()) {
        auto self_opt = self_weak_.as_ref().unwrap().upgrade();
        if (self_opt.is_some()) {
            poll_thread_.as_ref().unwrap()->add_proxy(
                make_tcp_listener_pollable_proxy(self_opt.unwrap()));
        }
    }
    return ChannelError::None;
}

void TcpListener::set_poll_thread(rusty::Arc<PollThread> pt) {
    poll_thread_ = rusty::Some(std::move(pt));
}

void TcpListener::set_self_weak(rusty::sync::Weak<TcpListener> self_weak) {
    self_weak_ = rusty::Some(std::move(self_weak));
}

void TcpListener::close() {
    if (closed_.get()) return;
    closed_.set(true);

    listener_ = rusty::net::TcpListener{};  // RAII close
}

bool TcpListener::is_closed() const {
    return closed_.get();
}

// @unsafe - std::string copy constructor isn't borrow-checked.
std::string TcpListener::local_address() const {
    return bound_address_;
}

// @unsafe - SpinMutex::lock + CallbackWrapper move-assign (not @safe).
void TcpListener::set_on_accept(OnAcceptCallback cb) {
    auto guard = on_accept_.lock().unwrap();
    *guard = std::move(cb);
}

// @unsafe - SpinMutex::lock + CallbackWrapper move-assign (not @safe).
void TcpListener::set_on_error(OnErrorCallback cb) {
    auto guard = on_error_.lock().unwrap();
    *guard = std::move(cb);
}

int TcpListener::fd() const {
    return listener_.as_owned_fd().as_raw_fd();
}

int TcpListener::poll_mode() const {
    return PollMode::READ;
}

std::size_t TcpListener::content_size() {
    return 0;
}

// @safe - accept loop now delegates to `rusty::net::TcpListener::accept`
// (which encapsulates the ::accept syscall + peer-address marshalling).
// Per-accept setup (non-blocking flag, optional SO_NOSIGPIPE on macOS)
// runs through the new TcpStream wrapper; the only remaining inline
// `// @unsafe { }` here is the macOS-specific setsockopt(SO_NOSIGPIPE)
// — Linux uses MSG_NOSIGNAL on send() and doesn't need it.
bool TcpListener::handle_read() {
    if (closed_.get()) return false;
    if (!listener_.is_bound()) return false;

    bool any_progress = false;
    while (true) {
        auto accept_result = listener_.accept();
        if (accept_result.is_err()) {
            auto err = accept_result.unwrap_err();
            auto kind = err.kind();
            // Retriable / "no work" — break out so the caller doesn't spin.
            if (kind == rusty::io::Error::Kind::WouldBlock ||
                kind == rusty::io::Error::Kind::Interrupted ||
                kind == rusty::io::Error::Kind::ConnectionAborted) {
                break;
            }
            // Non-recoverable failure.
            const ChannelError ch = io_kind_to_channel_error(kind);
            {
                auto guard = on_error_.lock().unwrap();
                if (*guard) {
                    (*guard)(ch, err.to_string());
                }
            }
            // For EMFILE/ENFILE we don't want to close — the listener
            // is still functional once a fd is freed up. Use the
            // io::Error::Kind that maps to those (currently we have no
            // dedicated Kind, so we close on everything else).
            close();
            return any_progress;
        }

        auto accepted = accept_result.unwrap();
        rusty::net::TcpStream stream = std::move(accepted.first);
        rusty::net::SocketAddrV4 peer_addr = accepted.second;

        any_progress = true;

#ifdef __APPLE__
        // Prevent SIGPIPE termination on write() to closed sockets.
        // Linux uses MSG_NOSIGNAL on send(); macOS lacks that flag.
        // Apply directly to the underlying fd before we hand it to
        // TcpConnection.
        {
            const int yes = 1;
            // @unsafe { setsockopt(SO_NOSIGPIPE) on macOS only. }
            (void)::setsockopt(stream.as_owned_fd().as_raw_fd(),
                               SOL_SOCKET, SO_NOSIGPIPE,
                               &yes, sizeof(yes));
        }
#endif

        // Non-blocking accepted socket — matches the rest of the
        // channel layer's expectations.
        auto nonblock_result = stream.set_nonblocking(true);
        if (nonblock_result.is_err()) {
            auto guard = on_error_.lock().unwrap();
            if (*guard) {
                (*guard)(io_kind_to_channel_error(
                             nonblock_result.unwrap_err().kind()),
                         "accept: failed to set non-blocking");
            }
            // stream drops here, closing the accepted fd.
            continue;
        }

        std::string peer_addr_str =
            rusty::net::socket_addr_v4_to_string(peer_addr);

        // Hand the accepted fd to TcpConnection. We unwrap the
        // TcpStream back into a raw int because TcpConnection still
        // takes an int fd — Phase D will swap TcpConnection's
        // OwnedFd field for a TcpStream and we'll pass the
        // TcpStream directly.
        int conn_fd;
        // @unsafe { into_owned_fd() releases ownership of the
        //           underlying fd; into_raw_fd() relinquishes the
        //           OwnedFd's RAII close. We rebuild RAII inside the
        //           TcpConnection ctor below. }
        { conn_fd = stream.into_owned_fd().into_raw_fd(); }
        auto conn = rusty::Arc<TcpConnection>::make(
            conn_fd, std::move(peer_addr_str));

        if (poll_thread_.is_some()) {
            // wire the poll thread into
            // the accepted connection BEFORE registering its pollable
            // proxy, so non-poll-thread `send_frame` callers on the
            // server side can also post `update_mode` actively.
            {
                auto& mut_conn = const_cast<TcpConnection&>(*conn.get());
                mut_conn.set_poll_thread(poll_thread_.as_ref().unwrap().clone());
            }
            poll_thread_.as_ref().unwrap()->add_proxy(
                make_tcp_connection_pollable_proxy(conn.clone()));
        }

        ChannelConnectionProxy proxy =
            make_tcp_connection_channel_proxy(std::move(conn));

        auto guard = on_accept_.lock().unwrap();
        if (*guard) {
            (*guard)(std::move(proxy));
        }
        // If no on_accept callback is installed, the proxy drops here
        // and the connection is destroyed.
    }
    return any_progress;
}

// @unsafe - Pollable interface; never fires for a listener.
int TcpListener::handle_write() {
    return PollMode::NO_CHANGE;
}

// @unsafe - Drives on_error callback after listen_fd_ failure.
void TcpListener::handle_error() {
    if (closed_.get()) return;
    {
        auto guard = on_error_.lock().unwrap();
        if (*guard) {
            (*guard)(ChannelError::Internal, "epoll/poll signaled error");
        }
    }
    close();
}

// @unsafe - Pollable interface; never fires for a listener.
bool TcpListener::check_pending_write_update() const {
    return false;
}

ChannelError TcpListener::listen_errno_to_channel_error(int err) {
    switch (err) {
        case EADDRINUSE:    return ChannelError::AddressInUse;
        case EADDRNOTAVAIL: return ChannelError::AddressInvalid;
        case EACCES:
        case EPERM:         return ChannelError::PermissionDenied;
        case EMFILE:
        case ENFILE:        return ChannelError::TooManyOpenFiles;
        case ECONNRESET:    return ChannelError::ConnectionReset;
        default:            return ChannelError::Internal;
    }
}

// ===========================================================================
// TcpFactory
// ===========================================================================

TcpFactory::TcpFactory(rusty::Arc<PollThread> poll_thread)
    : poll_thread_(std::move(poll_thread)) {}

// @unsafe - socket(2) / connect(2) / setsockopt(2) / fcntl(2) syscalls
// + reinterpret_cast<sockaddr*> on the sockaddr_in + PollThread::
// add_proxy is @unsafe + raw fd handling.
ConnectResult TcpFactory::connect(std::string_view addr) {
    auto parse_result = rusty::net::socket_addr_v4_from_str(addr);
    if (parse_result.is_err()) {
        return ConnectResult{rusty::None, ChannelError::AddressInvalid};
    }
    sockaddr_in sa =
        rusty::net::sockaddr_in_from_socket_addr_v4(parse_result.unwrap());

    // @unsafe — system call
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return ConnectResult{rusty::None,
                             connect_errno_to_channel_error(errno)};
    }

#ifdef __APPLE__
    // Prevent SIGPIPE termination on write() to closed sockets.
    // Linux uses MSG_NOSIGNAL on send(); macOS lacks that flag.
    {
        const int yes = 1;
        // @unsafe — system call
        (void)::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
    }
#endif

    // Make the socket non-blocking BEFORE the connect so we can apply
    // a timeout via `select(2)` if the kernel doesn't fail-fast.
    if (set_nonblocking_fd(fd) != 0) {
        const int err = errno;
        ::close(fd);
        return ConnectResult{rusty::None,
                             connect_errno_to_channel_error(err)};
    }

    // @unsafe — system call
    int rc = ::connect(fd, reinterpret_cast<const sockaddr*>(&sa), sizeof(sa));
    if (rc < 0) {
        const int err = errno;
        if (err == EINPROGRESS && connect_timeout_ms_ > 0) {
            // Wait up to `connect_timeout_ms_` for the connect to
            // complete. `select` returns with the fd writable on
            // success or when the kernel surfaces an error via
            // SO_ERROR.
            fd_set wset;
            FD_ZERO(&wset);
            FD_SET(fd, &wset);
            timeval tv;
            tv.tv_sec  =  connect_timeout_ms_ / 1000;
            tv.tv_usec = (connect_timeout_ms_ % 1000) * 1000;

            // @unsafe — system call
            int sel = ::select(fd + 1, nullptr, &wset, nullptr, &tv);
            if (sel == 0) {
                ::close(fd);
                return ConnectResult{rusty::None,
                                     ChannelError::Timeout};
            }
            if (sel < 0) {
                const int sel_err = errno;
                ::close(fd);
                return ConnectResult{rusty::None,
                                     connect_errno_to_channel_error(sel_err)};
            }
            // Check SO_ERROR for the actual connect outcome.
            int so_err = 0;
            socklen_t so_err_len = sizeof(so_err);
            // @unsafe — system call
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &so_err_len) < 0
                || so_err != 0) {
                const int eff_err = (so_err != 0) ? so_err : errno;
                ::close(fd);
                return ConnectResult{rusty::None,
                                     connect_errno_to_channel_error(eff_err)};
            }
        } else if (err != EISCONN) {
            ::close(fd);
            return ConnectResult{rusty::None,
                                 connect_errno_to_channel_error(err)};
        }
    }

    // Build the TcpConnection and register its pollable proxy with
    // the poll thread before returning. The channel proxy keeps one
    // Arc; the pollable proxy keeps another, so the connection
    // survives until both layers release.
    auto conn = rusty::Arc<TcpConnection>::make(fd, std::string(addr));
    // wire the poll thread reference into
    // the connection BEFORE the channel proxy is handed back, so any
    // user-thread `send_frame` call can post `update_mode` actively
    // (without the lost-wake-up race against `pending_write_update_`).
    {
        auto& mut_conn = const_cast<TcpConnection&>(*conn.get());
        mut_conn.set_poll_thread(poll_thread_.clone());
    }
    poll_thread_->add_proxy(make_tcp_connection_pollable_proxy(conn.clone()));

    return ConnectResult{
        rusty::Some(make_tcp_connection_channel_proxy(std::move(conn))),
        ChannelError::None,
    };
}

rusty::Option<ChannelListenerProxy> TcpFactory::make_listener() {
    auto listener = rusty::Arc<TcpListener>::make();
    // Wire the listener up with the poll thread + a weak self-ref so
    // it can self-register on a successful `listen(addr)` and so
    // accepted connections are auto-registered too.
    {
        auto& mut_l = const_cast<TcpListener&>(*listener.get());
        mut_l.set_poll_thread(poll_thread_.clone());
        mut_l.set_self_weak(rusty::sync::downgrade(listener));
    }
    return rusty::Some(make_tcp_listener_channel_proxy(std::move(listener)));
}

ChannelError TcpFactory::connect_errno_to_channel_error(int err) {
    switch (err) {
        case ECONNREFUSED:                 return ChannelError::ConnectionRefused;
        case ECONNRESET:
        case EPIPE:                        return ChannelError::ConnectionReset;
        case ETIMEDOUT:                    return ChannelError::Timeout;
        case EHOSTUNREACH:
        case ENETUNREACH:
        case EADDRNOTAVAIL:                return ChannelError::AddressInvalid;
        case EACCES:
        case EPERM:                        return ChannelError::PermissionDenied;
        case EMFILE:
        case ENFILE:                       return ChannelError::TooManyOpenFiles;
        default:                           return ChannelError::Internal;
    }
}

}  // namespace rrr
