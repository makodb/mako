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
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `/*RUSTYCPP:GEN-BEGIN ... END*/` block with the C++ definition.
#if RUSTYCPP_RUST
const kTcpConnectionOutboundHighWaterDefault: usize = 4 * 1024 * 1024; // 4 MiB
#endif
/*RUSTYCPP:GEN-BEGIN id=tcp_channel.1 version=1 rust_sha256=323dabc4f4884188a6f63b05c2efda063a7e612d34f04ccb15eb8aa2319f68ec*/
extern const size_t kTcpConnectionOutboundHighWaterDefault;

constexpr size_t kTcpConnectionOutboundHighWaterDefault = (static_cast<size_t>(4) * static_cast<size_t>(1024)) * static_cast<size_t>(1024);
/*RUSTYCPP:GEN-END id=tcp_channel.1*/

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
// Free-function implementations of the syscall-/lock-heavy methods. The
// DSL `struct TcpConnection` below keeps every method (so all call sites
// — adapters, tests, accept/connect — retain `conn.method()` syntax);
// the trivial ones have their bodies inline in the DSL, while the
// syscall/lock/callback ones delegate to these. Defined in the impl
// namespace further down; forward-declared here so the generated method
// bodies can call them. Each carries its own `// @unsafe` at the
// definition site (recv/send/shutdown syscalls, raw `uint8_t*` payload
// pointers, callback invocation).
ChannelError tcpconn_send_frame(const TcpConnection& self, const ChannelFrame& frame);
void         tcpconn_flush(const TcpConnection& self);
void         tcpconn_close(const TcpConnection& self);
void         tcpconn_set_on_frame(const TcpConnection& self, OnFrameCallback cb);
void         tcpconn_set_on_closed(const TcpConnection& self, OnClosedCallback cb);
void         tcpconn_set_on_error(const TcpConnection& self, OnErrorCallback cb);
int          tcpconn_poll_mode(const TcpConnection& self);
std::size_t  tcpconn_content_size(TcpConnection& self);
bool         tcpconn_handle_read(TcpConnection& self);
int          tcpconn_handle_write(TcpConnection& self);
void         tcpconn_handle_error(TcpConnection& self);

// Default-init helpers for the `#[cpp_ctor]` below: the DSL struct
// literal can't spell a default-constructed std::vector / FrameStreamReader
// / On*Callback inline, so the ctor field inits call these. (`OwnedFd`,
// `Cell`, `Option`, and `SpinMutex<std::vector<u8>>` it spells directly.)
inline std::vector<std::uint8_t> tcpconn_empty_buf()        { return {}; }
inline FrameStreamReader         tcpconn_default_inbound()  { return FrameStreamReader::new_(); }
inline OnFrameCallback           tcpconn_default_on_frame() { return OnFrameCallback{}; }
inline OnClosedCallback          tcpconn_default_on_closed(){ return OnClosedCallback{}; }
inline OnErrorCallback           tcpconn_default_on_error() { return OnErrorCallback{}; }

// One side of a connected stream socket. The fd is taken by ownership at
// construction and RAII-closed by the `OwnedFd` field; the connection is
// only ever held via `Arc<TcpConnection>` (constructed in-place by
// `Arc::make`), so non-movability is provided by Arc's stable storage —
// no explicit move-deletion needed.
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block is the
// source of truth; the transpiler regenerates the matching GEN block.
// All field types are already rusty (OwnedFd / SpinMutex / Cell /
// Option), so the struct is borrow-checked; the syscall/lock bodies live
// in the `tcpconn_*` free fns above, which the methods delegate to. The
// former hand-written dtor only latched `closed_` defensively — provably
// unobservable at destruction (the dtor runs at Arc refcount 0, so no
// thread can still hold an Arc to read it; `OwnedFd` RAII-closes the fd)
// — so it is dropped, leaving a clean `#[cpp_ctor]` struct (no Drop, no
// move-tracking machinery).
//
// @safe - the syscall methods delegate to the `tcpconn_*` free fns,
// which carry their own `// @unsafe` at their definition sites.
#if RUSTYCPP_RUST
struct TcpConnection {
    fd_: rusty::os::fd::OwnedFd,
    peer_address_: std::string,
    outbound_high_water_: usize,
    outbound_: SpinMutex<std::vector<u8>>,
    inbound_: FrameStreamReader,
    closed_: Cell<bool>,
    on_closed_fired_: Cell<bool>,
    pending_write_update_: Cell<bool>,
    poll_thread_: Option<Arc<PollThread>>,
    on_frame_: SpinMutex<OnFrameCallback>,
    on_closed_: SpinMutex<OnClosedCallback>,
    on_error_: SpinMutex<OnErrorCallback>,
}

impl TcpConnection {
    #[cpp_ctor] fn new(fd: i32, peer_address: std::string) -> TcpConnection {
        TcpConnection {
            fd_: rusty::os::fd::OwnedFd::from_raw_fd(fd),
            peer_address_: peer_address,
            outbound_high_water_: kTcpConnectionOutboundHighWaterDefault,
            outbound_: SpinMutex::<std::vector<u8>>::new(tcpconn_empty_buf()),
            inbound_: tcpconn_default_inbound(),
            closed_: Cell::new(false),
            on_closed_fired_: Cell::new(false),
            pending_write_update_: Cell::new(false),
            poll_thread_: None,
            on_frame_: SpinMutex::<OnFrameCallback>::new(tcpconn_default_on_frame()),
            on_closed_: SpinMutex::<OnClosedCallback>::new(tcpconn_default_on_closed()),
            on_error_: SpinMutex::<OnErrorCallback>::new(tcpconn_default_on_error()),
        }
    }

    fn set_outbound_high_water(&mut self, bytes: usize) {
        self.outbound_high_water_ = bytes;
    }

    fn send_frame(&self, frame: &ChannelFrame) -> ChannelError {
        tcpconn_send_frame(self, frame)
    }

    fn flush(&self) {
        tcpconn_flush(self)
    }

    fn close(&self) {
        tcpconn_close(self)
    }

    fn is_closed(&self) -> bool {
        self.closed_.get()
    }

    fn peer_address(&self) -> std::string {
        self.peer_address_
    }

    fn set_on_frame(&self, cb: OnFrameCallback) {
        tcpconn_set_on_frame(self, cb)
    }

    fn set_on_closed(&self, cb: OnClosedCallback) {
        tcpconn_set_on_closed(self, cb)
    }

    fn set_on_error(&self, cb: OnErrorCallback) {
        tcpconn_set_on_error(self, cb)
    }

    fn fd(&self) -> i32 {
        self.fd_.as_raw_fd()
    }

    fn poll_mode(&self) -> i32 {
        tcpconn_poll_mode(self)
    }

    fn content_size(&mut self) -> usize {
        tcpconn_content_size(self)
    }

    fn handle_read(&mut self) -> bool {
        tcpconn_handle_read(self)
    }

    fn handle_write(&mut self) -> i32 {
        tcpconn_handle_write(self)
    }

    fn handle_error(&mut self) {
        tcpconn_handle_error(self)
    }

    fn check_pending_write_update(&self) -> bool {
        if !self.pending_write_update_.get() {
            return false;
        }
        self.pending_write_update_.set(false);
        true
    }

    fn set_poll_thread(&mut self, pt: Arc<PollThread>) {
        self.poll_thread_ = Some(pt);
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=tcp_channel.conn version=1 rust_sha256=51c0ba58c4909220f12b1b1cff6a1ec515b246bf9a056f40864a0826ca704b52*/
struct TcpConnection;

struct TcpConnection {
    rusty::os::fd::OwnedFd fd_;
    std::string peer_address_;
    size_t outbound_high_water_;
    SpinMutex<std::vector<uint8_t>> outbound_;
    FrameStreamReader inbound_;
    rusty::Cell<bool> closed_;
    rusty::Cell<bool> on_closed_fired_;
    rusty::Cell<bool> pending_write_update_;
    rusty::Option<rusty::Arc<PollThread>> poll_thread_;
    SpinMutex<OnFrameCallback> on_frame_;
    SpinMutex<OnClosedCallback> on_closed_;
    SpinMutex<OnErrorCallback> on_error_;

    TcpConnection(int32_t fd, std::string peer_address);
    void set_outbound_high_water(size_t bytes);
    ChannelError send_frame(const ChannelFrame& frame) const;
    void flush() const;
    void close() const;
    bool is_closed() const;
    std::string peer_address() const;
    void set_on_frame(OnFrameCallback cb) const;
    void set_on_closed(OnClosedCallback cb) const;
    void set_on_error(OnErrorCallback cb) const;
    int32_t fd() const;
    int32_t poll_mode() const;
    size_t content_size();
    bool handle_read();
    int32_t handle_write();
    void handle_error();
    bool check_pending_write_update() const;
    void set_poll_thread(rusty::Arc<PollThread> pt);
};


TcpConnection::TcpConnection(int32_t fd, std::string peer_address)
    : fd_(rusty::os::fd::OwnedFd::from_raw_fd(std::move(fd)))
    , peer_address_(std::move(peer_address))
    , outbound_high_water_(kTcpConnectionOutboundHighWaterDefault)
    , outbound_(SpinMutex<std::vector<uint8_t>>::new_(tcpconn_empty_buf()))
    , inbound_(tcpconn_default_inbound())
    , closed_(rusty::Cell<bool>::new_(false))
    , on_closed_fired_(rusty::Cell<bool>::new_(false))
    , pending_write_update_(rusty::Cell<bool>::new_(false))
    , poll_thread_(rusty::Option<rusty::Arc<PollThread>>{rusty::None})
    , on_frame_(SpinMutex<OnFrameCallback>::new_(tcpconn_default_on_frame()))
    , on_closed_(SpinMutex<OnClosedCallback>::new_(tcpconn_default_on_closed()))
    , on_error_(SpinMutex<OnErrorCallback>::new_(tcpconn_default_on_error()))
{}

void TcpConnection::set_outbound_high_water(size_t bytes) {
    this->outbound_high_water_ = std::move(bytes);
}

ChannelError TcpConnection::send_frame(const ChannelFrame& frame) const {
    return tcpconn_send_frame((*this), frame);
}

void TcpConnection::flush() const {
    tcpconn_flush((*this));
}

void TcpConnection::close() const {
    tcpconn_close((*this));
}

bool TcpConnection::is_closed() const {
    return this->closed_.get();
}

std::string TcpConnection::peer_address() const {
    return this->peer_address_;
}

void TcpConnection::set_on_frame(OnFrameCallback cb) const {
    tcpconn_set_on_frame((*this), std::move(cb));
}

void TcpConnection::set_on_closed(OnClosedCallback cb) const {
    tcpconn_set_on_closed((*this), std::move(cb));
}

void TcpConnection::set_on_error(OnErrorCallback cb) const {
    tcpconn_set_on_error((*this), std::move(cb));
}

int32_t TcpConnection::fd() const {
    return this->fd_.as_raw_fd();
}

int32_t TcpConnection::poll_mode() const {
    return tcpconn_poll_mode((*this));
}

size_t TcpConnection::content_size() {
    return tcpconn_content_size((*this));
}

bool TcpConnection::handle_read() {
    return tcpconn_handle_read((*this));
}

int32_t TcpConnection::handle_write() {
    return tcpconn_handle_write((*this));
}

void TcpConnection::handle_error() {
    tcpconn_handle_error((*this));
}

bool TcpConnection::check_pending_write_update() const {
    if (!this->pending_write_update_.get()) {
        return false;
    }
    this->pending_write_update_.set(false);
    return true;
}

void TcpConnection::set_poll_thread(rusty::Arc<PollThread> pt) {
    this->poll_thread_ = rusty::Option<rusty::Arc<PollThread>>(std::move(pt));
}
/*RUSTYCPP:GEN-END id=tcp_channel.conn*/
// `TcpChannelShim` — the Arc-holding ChannelConnectionBase implementor
// over TcpConnection, replacing the hand-written const_cast adapter:
// TcpConnection's dispatched methods are now &self (interior-mutable),
// so the shim forwards straight through the Arc. Authored as inline
// Rust DSL with #[cpp_inherit] (sanctioned: ChannelConnectionBase is an
// impl-adjacent DSL interface trait) — the transpiler emits
// `struct TcpChannelShim : public ChannelConnectionBase` with implicit
// overrides and a fieldwise ctor that make_box constructs directly.
#if RUSTYCPP_RUST
struct TcpChannelShim {
    conn_: Arc<TcpConnection>,
}

#[cpp_inherit]
impl ChannelConnectionBase for TcpChannelShim {
    fn send_frame(&mut self, frame: &ChannelFrame) -> ChannelError {
        self.conn_.send_frame(frame)
    }
    fn flush(&mut self) {
        self.conn_.flush()
    }
    fn close(&mut self) {
        self.conn_.close()
    }
    fn is_closed(&self) -> bool {
        self.conn_.is_closed()
    }
    fn peer_address(&self) -> std::string {
        self.conn_.peer_address()
    }
    fn set_on_frame(&mut self, cb: OnFrameCallback) {
        self.conn_.set_on_frame(cb)
    }
    fn set_on_closed(&mut self, cb: OnClosedCallback) {
        self.conn_.set_on_closed(cb)
    }
    fn set_on_error(&mut self, cb: OnErrorCallback) {
        self.conn_.set_on_error(cb)
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=tcp_channel.channel_shim version=1 rust_sha256=fa2f6f6c3e0e2df428d352e50993ea73b9b19ae350486fe641baf8fd9557ba63*/
struct TcpChannelShim;

struct TcpChannelShim : public ChannelConnectionBase {
    rusty::Arc<TcpConnection> conn_;
    TcpChannelShim(rusty::Arc<TcpConnection> conn__init) : ChannelConnectionBase(), conn_(std::move(conn__init)) {}
    TcpChannelShim(TcpChannelShim&& other) noexcept : ChannelConnectionBase(), conn_(std::move(other.conn_)) {}


    ChannelError send_frame(const ChannelFrame& frame);
    void flush();
    void close();
    bool is_closed() const;
    std::string peer_address() const;
    void set_on_frame(OnFrameCallback cb);
    void set_on_closed(OnClosedCallback cb);
    void set_on_error(OnErrorCallback cb);
};


ChannelError TcpChannelShim::send_frame(const ChannelFrame& frame) {
    return this->conn_->send_frame(frame);
}

void TcpChannelShim::flush() {
    this->conn_->flush();
}

void TcpChannelShim::close() {
    this->conn_->close();
}

bool TcpChannelShim::is_closed() const {
    return this->conn_->is_closed();
}

std::string TcpChannelShim::peer_address() const {
    return this->conn_->peer_address();
}

void TcpChannelShim::set_on_frame(OnFrameCallback cb) {
    this->conn_->set_on_frame(std::move(cb));
}

void TcpChannelShim::set_on_closed(OnClosedCallback cb) {
    this->conn_->set_on_closed(std::move(cb));
}

void TcpChannelShim::set_on_error(OnErrorCallback cb) {
    this->conn_->set_on_error(std::move(cb));
}
/*RUSTYCPP:GEN-END id=tcp_channel.channel_shim*/

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
    // @safe - forwards to TcpConnection::is_closed (Cell::get is @safe).
    bool is_closed() const override                   { return conn_->is_closed(); }
    // @safe - forwards to TcpConnection::check_pending_write_update (Cell::get/set).
    bool check_pending_write_update() const override  { return conn_->check_pending_write_update(); }

 private:
    // @unsafe - const_cast through Arc::get<T*>().
    TcpConnection& mut_conn() { return const_cast<TcpConnection&>(*conn_.get()); }
    rusty::Arc<TcpConnection> conn_;
};

inline ChannelConnectionProxy make_tcp_connection_channel_proxy(
    rusty::Arc<TcpConnection> conn) {
    return rusty::make_box<TcpChannelShim>(std::move(conn));
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
// Free-fn implementations of the syscall-/lock-heavy TcpListener methods;
// the DSL methods delegate to these (same pattern as TcpConnection above).
// Each carries its own `// @unsafe` at the definition site.
struct TcpListener;  // defined by the GEN block below
ChannelError tcplistener_listen(TcpListener& self, std::string_view addr);
void         tcplistener_close(TcpListener& self);
void         tcplistener_set_on_accept(TcpListener& self, OnAcceptCallback cb);
void         tcplistener_set_on_error(TcpListener& self, OnErrorCallback cb);
bool         tcplistener_handle_read(TcpListener& self);
void         tcplistener_handle_error(TcpListener& self);

// Default-init helpers for the `#[cpp_ctor]` (the DSL can't spell a default
// rusty::net::TcpListener / std::string / On*Callback inline).
inline rusty::net::TcpListener tcplistener_default_net()    { return rusty::net::TcpListener(); }
inline std::string             tcplistener_empty_addr()     { return std::string(); }
inline OnAcceptCallback        tcplistener_default_on_accept() { return OnAcceptCallback{}; }
inline OnErrorCallback         tcplistener_default_on_error()  { return OnErrorCallback{}; }

// Server-side TCP listener — owns a `rusty::net::TcpListener` (RAII-closes
// the listen fd on drop) + an accept loop. All fields are already rusty
// (net::TcpListener / Cell / Option / SpinMutex), so the struct is
// borrow-checked; the bind / accept-loop / callback bodies live in the
// `tcplistener_*` free fns the methods delegate to. Held only via
// Arc<TcpListener> (stable storage), so no explicit move-deletion is needed.
// Structural twin of the migrated TcpConnection. The dead
// `listen_errno_to_channel_error` static (never called — `listen`/
// `handle_read` use `io_kind_to_channel_error`) is dropped.
//
// @safe - delegating methods forward to the `tcplistener_*` free fns,
// which carry their own `// @unsafe`.
#if RUSTYCPP_RUST
struct TcpListener {
    listener_: rusty::net::TcpListener,
    bound_address_: std::string,
    closed_: Cell<bool>,
    listened_: Cell<bool>,
    poll_thread_: Option<Arc<PollThread>>,
    self_weak_: Option<rusty::sync::Weak<TcpListener>>,
    on_accept_: SpinMutex<OnAcceptCallback>,
    on_error_: SpinMutex<OnErrorCallback>,
}

impl TcpListener {
    #[cpp_ctor] fn new() -> TcpListener {
        TcpListener {
            listener_: tcplistener_default_net(),
            bound_address_: tcplistener_empty_addr(),
            closed_: Cell::new(false),
            listened_: Cell::new(false),
            poll_thread_: None,
            self_weak_: None,
            on_accept_: SpinMutex::<OnAcceptCallback>::new(tcplistener_default_on_accept()),
            on_error_: SpinMutex::<OnErrorCallback>::new(tcplistener_default_on_error()),
        }
    }

    fn listen(&mut self, addr: std::string_view) -> ChannelError {
        tcplistener_listen(self, addr)
    }

    fn close(&mut self) {
        tcplistener_close(self)
    }

    fn is_closed(&self) -> bool {
        self.closed_.get()
    }

    fn local_address(&self) -> std::string {
        self.bound_address_
    }

    fn set_on_accept(&mut self, cb: OnAcceptCallback) {
        tcplistener_set_on_accept(self, cb)
    }

    fn set_on_error(&mut self, cb: OnErrorCallback) {
        tcplistener_set_on_error(self, cb)
    }

    fn fd(&self) -> i32 {
        self.listener_.as_owned_fd().as_raw_fd()
    }

    fn poll_mode(&self) -> i32 {
        PollMode::READ
    }

    fn content_size(&mut self) -> usize {
        0usize
    }

    fn handle_read(&mut self) -> bool {
        tcplistener_handle_read(self)
    }

    fn handle_write(&mut self) -> i32 {
        PollMode::NO_CHANGE
    }

    fn handle_error(&mut self) {
        tcplistener_handle_error(self)
    }

    fn check_pending_write_update(&self) -> bool {
        false
    }

    fn set_poll_thread(&mut self, pt: Arc<PollThread>) {
        self.poll_thread_ = Some(pt);
    }

    fn set_self_weak(&mut self, self_weak: rusty::sync::Weak<TcpListener>) {
        self.self_weak_ = Some(self_weak);
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=tcp_channel.listener version=1 rust_sha256=77a53eda6349c118aa41047bd3d3fd93bafa62245187b22bd26e0d752639fe50*/
struct TcpListener;

struct TcpListener {
    rusty::net::TcpListener listener_;
    std::string bound_address_;
    rusty::Cell<bool> closed_;
    rusty::Cell<bool> listened_;
    rusty::Option<rusty::Arc<PollThread>> poll_thread_;
    rusty::Option<rusty::sync::Weak<TcpListener>> self_weak_;
    SpinMutex<OnAcceptCallback> on_accept_;
    SpinMutex<OnErrorCallback> on_error_;

    TcpListener();
    ChannelError listen(std::string_view addr);
    void close();
    bool is_closed() const;
    std::string local_address() const;
    void set_on_accept(OnAcceptCallback cb);
    void set_on_error(OnErrorCallback cb);
    int32_t fd() const;
    int32_t poll_mode() const;
    size_t content_size();
    bool handle_read();
    int32_t handle_write();
    void handle_error();
    bool check_pending_write_update() const;
    void set_poll_thread(rusty::Arc<PollThread> pt);
    void set_self_weak(rusty::sync::Weak<TcpListener> self_weak);
};


TcpListener::TcpListener()
    : listener_(tcplistener_default_net())
    , bound_address_(tcplistener_empty_addr())
    , closed_(rusty::Cell<bool>::new_(false))
    , listened_(rusty::Cell<bool>::new_(false))
    , poll_thread_(rusty::Option<rusty::Arc<PollThread>>{rusty::None})
    , self_weak_(rusty::Option<rusty::sync::Weak<TcpListener>>{rusty::None})
    , on_accept_(SpinMutex<OnAcceptCallback>::new_(tcplistener_default_on_accept()))
    , on_error_(SpinMutex<OnErrorCallback>::new_(tcplistener_default_on_error()))
{}

ChannelError TcpListener::listen(std::string_view addr) {
    return tcplistener_listen((*this), std::move(addr));
}

void TcpListener::close() {
    tcplistener_close((*this));
}

bool TcpListener::is_closed() const {
    return this->closed_.get();
}

std::string TcpListener::local_address() const {
    return this->bound_address_;
}

void TcpListener::set_on_accept(OnAcceptCallback cb) {
    tcplistener_set_on_accept((*this), std::move(cb));
}

void TcpListener::set_on_error(OnErrorCallback cb) {
    tcplistener_set_on_error((*this), std::move(cb));
}

int32_t TcpListener::fd() const {
    return this->listener_.as_owned_fd().as_raw_fd();
}

int32_t TcpListener::poll_mode() const {
    return PollMode::READ;
}

size_t TcpListener::content_size() {
    return static_cast<size_t>(0);
}

bool TcpListener::handle_read() {
    return tcplistener_handle_read((*this));
}

int32_t TcpListener::handle_write() {
    return PollMode::NO_CHANGE;
}

void TcpListener::handle_error() {
    tcplistener_handle_error((*this));
}

bool TcpListener::check_pending_write_update() const {
    return false;
}

void TcpListener::set_poll_thread(rusty::Arc<PollThread> pt) {
    this->poll_thread_ = rusty::Option<rusty::Arc<PollThread>>(std::move(pt));
}

void TcpListener::set_self_weak(rusty::sync::Weak<TcpListener> self_weak) {
    this->self_weak_ = rusty::Option<rusty::sync::Weak<TcpListener>>(std::move(self_weak));
}
/*RUSTYCPP:GEN-END id=tcp_channel.listener*/

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
// TcpFactory is now an aggregate (public fields, no user ctors, no
// = delete) — same shape as the rrr DSL-style aggregates emitted by
// rusty-cpp. Non-copyability falls out implicitly because the
// `rusty::Arc<PollThread>` field is itself non-copyable. Callers
// build via `Arc<TcpFactory>::new_(TcpFactory::new_(arg))`.
// Authored as inline Rust DSL. `connect` and `make_listener` stay as
// free functions (defined further down in the file) because their
// bodies hold ~100 LOC of socket/connect/fcntl/setsockopt syscalls +
// sockaddr_in casts + PollThread::add_proxy calls — none of which
// translate to the DSL grammar today. The
// `connect_errno_to_channel_error` static helper also moves to a
// non-DSL free function (only called from `tcp_factory_connect`).
#if RUSTYCPP_RUST
struct TcpFactory {
    poll_thread_: Arc<PollThread>,
    connect_timeout_ms_: i32,
}

impl TcpFactory {
    fn new(poll_thread: Arc<PollThread>) -> TcpFactory {
        TcpFactory { poll_thread_: poll_thread, connect_timeout_ms_: 5000i32 }
    }

    fn backend_name(&self) -> std::string {
        std::string("tcp")
    }

    fn set_connect_timeout_ms(&mut self, timeout_ms: i32) {
        self.connect_timeout_ms_ = timeout_ms;
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=tcp_channel.factory version=1 rust_sha256=e747e0cf7b391eb08b698d50bdd72c0e05ac556f2d6e918889b6ae2892c15469*/
struct TcpFactory;

struct TcpFactory {
    rusty::Arc<PollThread> poll_thread_;
    int32_t connect_timeout_ms_;

    static TcpFactory new_(rusty::Arc<PollThread> poll_thread);
    std::string backend_name() const;
    void set_connect_timeout_ms(int32_t timeout_ms);
};


TcpFactory TcpFactory::new_(rusty::Arc<PollThread> poll_thread) {
    return TcpFactory{.poll_thread_ = std::move(poll_thread), .connect_timeout_ms_ = static_cast<int32_t>(5000)};
}

std::string TcpFactory::backend_name() const {
    return std::string("tcp");
}

void TcpFactory::set_connect_timeout_ms(int32_t timeout_ms) {
    this->connect_timeout_ms_ = std::move(timeout_ms);
}
/*RUSTYCPP:GEN-END id=tcp_channel.factory*/

// Free functions (non-DSL) — see definitions further down.
ConnectResult                       tcp_factory_connect(TcpFactory& self, std::string_view addr);
rusty::Option<ChannelListenerProxy> tcp_factory_make_listener(TcpFactory& self);

class TcpFactoryAdapter : public ChannelFactoryBase {
 public:
    explicit TcpFactoryAdapter(rusty::Arc<TcpFactory> factory)
        : factory_(std::move(factory)) {}

    // @unsafe - forwards through mut_factory() const_cast (socket+connect path).
    ConnectResult                       connect(std::string_view addr) override { return tcp_factory_connect(mut_factory(), addr); }
    // @unsafe - forwards through mut_factory() const_cast.
    rusty::Option<ChannelListenerProxy> make_listener() override                { return tcp_factory_make_listener(mut_factory()); }
    std::string                         backend_name() const override           { return factory_->backend_name(); }

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
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `/*RUSTYCPP:GEN-BEGIN ... END*/` block with the C++ definition.
#if RUSTYCPP_RUST
const kRecvScratchBytes: usize = 64 * 1024;
#endif
/*RUSTYCPP:GEN-BEGIN id=tcp_channel.2 version=1 rust_sha256=12624ec7f5bdb75ff8067366f78e1c00d9db805dce24c904a461f1f8ff8d4676*/
extern const size_t kRecvScratchBytes;

constexpr size_t kRecvScratchBytes = static_cast<size_t>(64) * static_cast<size_t>(1024);
/*RUSTYCPP:GEN-END id=tcp_channel.2*/

}  // namespace

// ---------------------------------------------------------------------------
// TcpConnection method implementations
// ---------------------------------------------------------------------------
//
// The DSL `struct TcpConnection` (above) declares the data + the
// delegating methods; these free fns hold the syscall-/lock-heavy bodies,
// each taking the connection as `self` and touching the migrated struct's
// public fields directly. The ctor (via `#[cpp_ctor]`), the dropped dtor
// (its `closed_.set(true)` was an unobservable no-op at refcount 0), and
// the trivial methods (set_outbound_high_water / is_closed / fd /
// check_pending_write_update / set_poll_thread) now live in the DSL block.

// Internal helpers, forward-declared so the public free fns below can
// call them before their definitions (mutual recursion across the set).
ChannelError tcpconn_drain_outbound_locked(const TcpConnection& self,
                                           std::vector<std::uint8_t>& buf);
void         tcpconn_deliver_on_closed_locked(const TcpConnection& self, ChannelError reason);
ChannelError tcpconn_errno_to_channel_error(int err);

// @unsafe - encodes into the outbound buffer (raw payload pointer) and
// posts `PollThread::update_mode` to wake the poll loop.
ChannelError tcpconn_send_frame(const TcpConnection& self, const ChannelFrame& frame) {
    if (self.closed_.get()) {
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

    auto guard = self.outbound_.lock().unwrap();
    auto& buf = *guard;

    // Reject when the queue is already past the high water — we never
    // append to a buffer that's already over budget so backpressure is
    // strictly bounded.
    if (buf.size() >= self.outbound_high_water_) {
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
    if (self.poll_thread_.is_some() && !PollThreadWorker::is_on_poll_thread()) {
        self.poll_thread_.as_ref().unwrap()->update_mode(
            self.fd_.as_raw_fd(), PollMode::READ | PollMode::WRITE);
    } else {
        self.pending_write_update_.set(true);
    }
    return ChannelError::None;
}

// @unsafe - drives tcpconn_drain_outbound_locked (which is @unsafe for
// raw `uint8_t*` arithmetic + send syscall).
void tcpconn_flush(const TcpConnection& self) {
    if (self.closed_.get()) return;

    auto guard = self.outbound_.lock().unwrap();
    if ((*guard).empty()) return;

    // Best-effort: try to drain immediately. Errors here are reported
    // via the callbacks set on this connection; the caller of `flush`
    // does not get a return code (matching the channel facade).
    ChannelError result = tcpconn_drain_outbound_locked(self, *guard);
    if (result != ChannelError::None && result != ChannelError::WouldBlock) {
        // Defer error/close delivery to the poll thread to keep the
        // callback contract (poll-thread-only) honest. We just mark the
        // closed flag; the next `handle_read` / `handle_write` will fire
        // the callbacks. If `flush` is being called from the poll
        // thread itself, fire immediately.
        // For sub-leaf 3a we don't have an independent poll-thread
        // hand-off; tests drive Pollable methods directly. Mark closed
        // and let the next poll cycle observe it.
        self.closed_.set(true);
    }
}

// @unsafe - ::shutdown libc syscall + OwnedFd RAII close + callback fire.
void tcpconn_close(const TcpConnection& self) {
    // Latch on first call. Idempotent for concurrent callers because
    // `Cell<bool>::set(true)` is a release-store; the first to set
    // wins, the rest observe `closed_.get() == true` here.
    if (self.closed_.get()) {
        return;
    }
    self.closed_.set(true);

    // Shutdown the write side to flush kernel buffers and signal the
    // peer; then drop the OwnedFd to RAII-close. `::shutdown` may fail
    // if the socket is already half-closed — we ignore that.
    if (self.fd_.is_valid()) {
        // @unsafe { ::shutdown is libc — initiates orderly TCP close. }
        { ::shutdown(self.fd_.as_raw_fd(), SHUT_RDWR); }
        // Irreducible plain-field assignment on the const facade
        // (fd teardown at close) — the documented localized-const_cast
        // pattern; every other mutation here is interior-mutable.
        const_cast<TcpConnection&>(self).fd_ = rusty::os::fd::OwnedFd{};
    }

    // Deliver `on_closed(ChannelError::None)` exactly once.
    tcpconn_deliver_on_closed_locked(self, ChannelError::None);
}

// @unsafe - last-writer-wins callback store under the spinlock.
void tcpconn_set_on_frame(const TcpConnection& self, OnFrameCallback cb) {
    auto guard = self.on_frame_.lock().unwrap();
    *guard = std::move(cb);
}

void tcpconn_set_on_closed(const TcpConnection& self, OnClosedCallback cb) {
    auto guard = self.on_closed_.lock().unwrap();
    *guard = std::move(cb);
}

void tcpconn_set_on_error(const TcpConnection& self, OnErrorCallback cb) {
    auto guard = self.on_error_.lock().unwrap();
    *guard = std::move(cb);
}

// ---------------------------------------------------------------------------
// Pollable methods
// ---------------------------------------------------------------------------

// @safe - peeks the outbound queue length under the spinlock.
int tcpconn_poll_mode(const TcpConnection& self) {
    int mode = PollMode::READ;
    auto guard = self.outbound_.lock().unwrap();
    if (!(*guard).empty()) {
        mode |= PollMode::WRITE;
    }
    return mode;
}

// @safe - outbound (locked) + inbound buffered byte counts.
std::size_t tcpconn_content_size(TcpConnection& self) {
    auto guard = self.outbound_.lock().unwrap();
    return (*guard).size() + self.inbound_.buffered_bytes();
}

// @unsafe - recv(2) syscall into a raw `char` scratch buffer +
// FrameStreamReader::append / next_frame / consume_frame are all
// @unsafe + raw `uint8_t*` payload pointers stored on the FrameView.
bool tcpconn_handle_read(TcpConnection& self) {
    if (self.closed_.get()) return false;

    std::uint8_t scratch[kRecvScratchBytes];
    bool any_progress = false;

    while (true) {
        ssize_t n;
        // @unsafe { ::recv libc syscall — reads raw bytes into the
        //           scratch buffer. }
        { n = ::recv(self.fd_.as_raw_fd(), scratch, sizeof(scratch), 0); }
        if (n > 0) {
            self.inbound_.append(scratch, static_cast<std::size_t>(n));
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
            self.closed_.set(true);
            // Irreducible plain-field assignment on the const facade
        // (fd teardown at close) — the documented localized-const_cast
        // pattern; every other mutation here is interior-mutable.
        const_cast<TcpConnection&>(self).fd_ = rusty::os::fd::OwnedFd{};  // RAII close
            tcpconn_deliver_on_closed_locked(self, ChannelError::None);
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
        const ChannelError ch = tcpconn_errno_to_channel_error(err);
        {
            auto guard = self.on_error_.lock().unwrap();
            if (*guard) {
                (*guard)(ch, std::strerror(err));
            }
        }
        self.closed_.set(true);
        // Irreducible plain-field assignment on the const facade
        // (fd teardown at close) — the documented localized-const_cast
        // pattern; every other mutation here is interior-mutable.
        const_cast<TcpConnection&>(self).fd_ = rusty::os::fd::OwnedFd{};  // RAII close
        tcpconn_deliver_on_closed_locked(self, ch);
        return false;
    }

    // Now drain any complete frames out of the inbound buffer.
    while (true) {
        FrameView v{};
        const FrameDecodeStatus s = self.inbound_.next_frame(v);
        if (s == FrameDecodeStatus::Complete) {
            ChannelFrame cf{v.payload, v.payload_size};
            {
                auto guard = self.on_frame_.lock().unwrap();
                if (*guard) {
                    (*guard)(cf);
                }
            }
            self.inbound_.consume_frame();
            continue;
        }
        if (s == FrameDecodeStatus::NeedMoreBytes) {
            break;
        }
        // Malformed.
        {
            auto guard = self.on_error_.lock().unwrap();
            if (*guard) {
                (*guard)(ChannelError::Internal,
                         "malformed frame on inbound stream");
            }
        }
        self.closed_.set(true);
        // Irreducible plain-field assignment on the const facade
        // (fd teardown at close) — the documented localized-const_cast
        // pattern; every other mutation here is interior-mutable.
        const_cast<TcpConnection&>(self).fd_ = rusty::os::fd::OwnedFd{};  // RAII close
        self.inbound_.reset();
        tcpconn_deliver_on_closed_locked(self, ChannelError::Internal);
        return false;
    }

    return any_progress;
}

// @unsafe - drives tcpconn_drain_outbound_locked (which is @unsafe for
// raw `uint8_t*` arithmetic + send syscall).
int tcpconn_handle_write(TcpConnection& self) {
    if (self.closed_.get()) return PollMode::NO_CHANGE;

    auto guard = self.outbound_.lock().unwrap();
    auto& buf = *guard;
    if (buf.empty()) {
        return PollMode::READ;
    }

    const ChannelError result = tcpconn_drain_outbound_locked(self, buf);

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
        auto err_guard = self.on_error_.lock().unwrap();
        if (*err_guard) {
            (*err_guard)(result, "outbound write failed");
        }
    }
    self.closed_.set(true);
    self.fd_ = rusty::os::fd::OwnedFd{};  // RAII close
    tcpconn_deliver_on_closed_locked(self, result);
    return PollMode::READ;  // Stop watching writes; closed.
}

// @unsafe - fires on_error callback + drives tcpconn_close (::shutdown).
void tcpconn_handle_error(TcpConnection& self) {
    if (self.closed_.get()) return;
    {
        auto guard = self.on_error_.lock().unwrap();
        if (*guard) {
            (*guard)(ChannelError::Internal, "epoll/poll signaled error");
        }
    }
    tcpconn_close(self);  // Idempotent; fires on_closed.
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// @unsafe - raw `uint8_t*` pointer arithmetic + send(2) syscall +
// pointer dereference on the outbound buffer.
ChannelError tcpconn_drain_outbound_locked(
    const TcpConnection& self, std::vector<std::uint8_t>& buf) {

    std::size_t offset = 0;
    while (offset < buf.size()) {
        const std::size_t remaining = buf.size() - offset;
        // @unsafe — system call
        ssize_t n = ::send(self.fd_.as_raw_fd(), buf.data() + offset, remaining, MSG_NOSIGNAL);
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
        return tcpconn_errno_to_channel_error(err);
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

// @unsafe - fires the on_closed callback (once) under the spinlock.
void tcpconn_deliver_on_closed_locked(const TcpConnection& self, ChannelError reason) {
    if (self.on_closed_fired_.get()) {
        return;
    }
    self.on_closed_fired_.set(true);
    auto guard = self.on_closed_.lock().unwrap();
    if (*guard) {
        (*guard)(reason);
    }
}

// @safe - pure errno -> ChannelError mapping.
ChannelError tcpconn_errno_to_channel_error(int err) {
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

// TcpListener method implementations (free fns the DSL methods delegate
// to). The ctor (#[cpp_ctor]) + the trivial methods (is_closed / poll_mode
// / content_size / handle_write / check_pending_write_update /
// set_poll_thread / set_self_weak) now live in the DSL block; the dead
// `listen_errno_to_channel_error` static is dropped.

// @safe - bind path delegates to rusty::net::TcpListener::bind +
// socket_addr_v4_from_str + set_nonblocking; pure flow control over Results.
ChannelError tcplistener_listen(TcpListener& self, std::string_view addr) {
    if (self.closed_.get()) {
        return ChannelError::AddressInUse;
    }
    if (self.listened_.get()) {
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
    self.listener_ = bind_result.unwrap();

    auto nonblock_result = self.listener_.set_nonblocking(true);
    if (nonblock_result.is_err()) {
        ChannelError ch = io_kind_to_channel_error(
            nonblock_result.unwrap_err().kind());
        self.listener_ = rusty::net::TcpListener{};  // RAII close
        return ch;
    }

    // Discover actual bound address (port may have been 0).
    auto local_result = self.listener_.local_addr();
    if (local_result.is_ok()) {
        self.bound_address_ = rusty::net::socket_addr_v4_to_string(
            local_result.unwrap());
    } else {
        self.bound_address_ = std::string(addr);
    }

    self.listened_.set(true);

    // Auto-register with the poll thread if the factory wired one in.
    // The factory pre-installs `poll_thread_` and a weak self-ref
    // before handing the listener proxy to the user; we upgrade the
    // weak ref and hand a `PollableProxy` clone to the poll thread.
    if (self.poll_thread_.is_some() && self.self_weak_.is_some()) {
        auto self_opt = self.self_weak_.as_ref().unwrap().upgrade();
        if (self_opt.is_some()) {
            self.poll_thread_.as_ref().unwrap()->add_proxy(
                make_tcp_listener_pollable_proxy(self_opt.unwrap()));
        }
    }
    return ChannelError::None;
}

// @safe - sets the closed latch and drops the owned listener (RAII close).
void tcplistener_close(TcpListener& self) {
    if (self.closed_.get()) return;
    self.closed_.set(true);

    self.listener_ = rusty::net::TcpListener{};  // RAII close
}

// @unsafe - last-writer-wins callback store under the spinlock.
void tcplistener_set_on_accept(TcpListener& self, OnAcceptCallback cb) {
    auto guard = self.on_accept_.lock().unwrap();
    *guard = std::move(cb);
}

// @unsafe - same shape as set_on_accept.
void tcplistener_set_on_error(TcpListener& self, OnErrorCallback cb) {
    auto guard = self.on_error_.lock().unwrap();
    *guard = std::move(cb);
}

// @safe - accept loop now delegates to `rusty::net::TcpListener::accept`
// (which encapsulates the ::accept syscall + peer-address marshalling).
// Per-accept setup (non-blocking flag, optional SO_NOSIGPIPE on macOS)
// runs through the new TcpStream wrapper; the only remaining inline
// `// @unsafe { }` here is the macOS-specific setsockopt(SO_NOSIGPIPE)
// — Linux uses MSG_NOSIGNAL on send() and doesn't need it.
// @unsafe - accept loop: rusty::net::TcpListener::accept + per-accept
// setsockopt(macOS) + TcpConnection construction + on_accept/on_error
// callback dispatch under the spinlock.
bool tcplistener_handle_read(TcpListener& self) {
    if (self.closed_.get()) return false;
    if (!self.listener_.is_bound()) return false;

    bool any_progress = false;
    while (true) {
        auto accept_result = self.listener_.accept();
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
                auto guard = self.on_error_.lock().unwrap();
                if (*guard) {
                    (*guard)(ch, err.to_string());
                }
            }
            // For EMFILE/ENFILE we don't want to close — the listener
            // is still functional once a fd is freed up. Use the
            // io::Error::Kind that maps to those (currently we have no
            // dedicated Kind, so we close on everything else).
            tcplistener_close(self);
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
            auto guard = self.on_error_.lock().unwrap();
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

        if (self.poll_thread_.is_some()) {
            // wire the poll thread into
            // the accepted connection BEFORE registering its pollable
            // proxy, so non-poll-thread `send_frame` callers on the
            // server side can also post `update_mode` actively.
            {
                auto& mut_conn = const_cast<TcpConnection&>(*conn.get());
                mut_conn.set_poll_thread(self.poll_thread_.as_ref().unwrap().clone());
            }
            self.poll_thread_.as_ref().unwrap()->add_proxy(
                make_tcp_connection_pollable_proxy(conn.clone()));
        }

        ChannelConnectionProxy proxy =
            make_tcp_connection_channel_proxy(std::move(conn));

        auto guard = self.on_accept_.lock().unwrap();
        if (*guard) {
            (*guard)(std::move(proxy));
        }
        // If no on_accept callback is installed, the proxy drops here
        // and the connection is destroyed.
    }
    return any_progress;
}

// @unsafe - drives the on_error callback then closes the listener.
void tcplistener_handle_error(TcpListener& self) {
    if (self.closed_.get()) return;
    {
        auto guard = self.on_error_.lock().unwrap();
        if (*guard) {
            (*guard)(ChannelError::Internal, "epoll/poll signaled error");
        }
    }
    tcplistener_close(self);
}

// ===========================================================================
// TcpFactory
// ===========================================================================

// @safe - file-static helper, mirrors the old
// `TcpFactory::connect_errno_to_channel_error` static method. Plain
// errno → ChannelError mapping; no state.
namespace {
ChannelError connect_errno_to_channel_error(int err) {
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
}  // namespace

// @unsafe - socket(2) / connect(2) / setsockopt(2) / fcntl(2) syscalls
// + reinterpret_cast<sockaddr*> on the sockaddr_in + PollThread::
// add_proxy is @unsafe + raw fd handling.
ConnectResult tcp_factory_connect(TcpFactory& self, std::string_view addr) {
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
        if (err == EINPROGRESS && self.connect_timeout_ms_ > 0) {
            // Wait up to `connect_timeout_ms_` for the connect to
            // complete. `select` returns with the fd writable on
            // success or when the kernel surfaces an error via
            // SO_ERROR.
            fd_set wset;
            FD_ZERO(&wset);
            FD_SET(fd, &wset);
            timeval tv;
            tv.tv_sec  =  self.connect_timeout_ms_ / 1000;
            tv.tv_usec = (self.connect_timeout_ms_ % 1000) * 1000;

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
        mut_conn.set_poll_thread(self.poll_thread_.clone());
    }
    self.poll_thread_->add_proxy(make_tcp_connection_pollable_proxy(conn.clone()));

    return ConnectResult{
        rusty::Some(make_tcp_connection_channel_proxy(std::move(conn))),
        ChannelError::None,
    };
}

rusty::Option<ChannelListenerProxy> tcp_factory_make_listener(TcpFactory& self) {
    auto listener = rusty::Arc<TcpListener>::make();
    // Wire the listener up with the poll thread + a weak self-ref so
    // it can self-register on a successful `listen(addr)` and so
    // accepted connections are auto-registered too.
    {
        auto& mut_l = const_cast<TcpListener&>(*listener.get());
        mut_l.set_poll_thread(self.poll_thread_.clone());
        mut_l.set_self_weak(rusty::sync::downgrade(listener));
    }
    return rusty::Some(make_tcp_listener_channel_proxy(std::move(listener)));
}

}  // namespace rrr
