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
#include <rusty/refcell.hpp>
#include <rusty/rusty.hpp>
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
// TcpFactory is rusty::Cell / rusty::Mutex / Arc / Option. Methods that
// drive socket / fd syscalls (socket / bind / listen / accept / send /
// recv / close / fcntl / getsockname) or that thread through
// const_cast `mut_conn` / `mut_listener` / `mut_factory` helpers
// carry per-method `// @unsafe`. The Phase 1 TcpListener half was
// already labeled; this iteration extends the same labeling to
// TcpConnection, its two adapters, TcpFactory, and the adapter set.
/*RUSTYCPP:GEN-DISPATCH-BEGIN*/
namespace rusty { namespace detail {
RUSTY_METHOD_DISPATCH(consume_frame)
RUSTY_METHOD_DISPATCH(reset)
RUSTY_METHOD_DISPATCH(unwrap)
} } // namespace rusty::detail (issue #31 deref_call dispatch)
/*RUSTYCPP:GEN-DISPATCH-END*/

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
void         tcpconn_close(const TcpConnection& conn);
void         tcpconn_reset_fd(const TcpConnection& conn);
bool         tcpconn_handle_read(const TcpConnection& self);
int          tcpconn_handle_write(const TcpConnection& self);
void         tcpconn_handle_error(const TcpConnection& self);

// Default-init helpers for the `#[cpp_ctor]` below: the DSL struct
// literal can't spell a default-constructed std::vector / FrameStreamReader
// / On*Callback inline, so the ctor field inits call these. (`OwnedFd`,
// `Cell`, `Option`, and `rusty::Mutex<std::vector<u8>>` it spells directly.)
// Default-value factories for TcpConnection's #[cpp_ctor] literal: a
// bare Default::default() mis-infers in THIS ctor's field positions
// (works in TcpListener's — parameterized-ctor inference gap), and
// #[cpp_ctor] bodies must stay pure literals, so typed lets can't help.
// tcpconn_default_inbound died (FrameStreamReader::new() lowers fine).
inline std::vector<std::uint8_t> tcpconn_empty_buf()        { return {}; }
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
// All field types are already rusty (OwnedFd / rusty::Mutex / Cell /
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
    outbound_: rusty::Mutex<std::vector<u8>>,
    inbound_: RefCell<FrameStreamReader>,
    closed_: Cell<bool>,
    on_closed_fired_: Cell<bool>,
    pending_write_update_: Cell<bool>,
    poll_thread_: Option<Arc<PollThread>>,
    on_frame_: rusty::Mutex<OnFrameCallback>,
    on_closed_: rusty::Mutex<OnClosedCallback>,
    on_error_: rusty::Mutex<OnErrorCallback>,
}

impl TcpConnection {
    #[cpp_ctor] fn new(fd: i32, peer_address: std::string) -> TcpConnection {
        TcpConnection {
            fd_: rusty::os::fd::OwnedFd::from_raw_fd(fd),
            peer_address_: peer_address,
            outbound_high_water_: kTcpConnectionOutboundHighWaterDefault,
            outbound_: rusty::Mutex::<std::vector<u8>>::new(tcpconn_empty_buf()),
            inbound_: RefCell::new(FrameStreamReader::new()),
            closed_: Cell::new(false),
            on_closed_fired_: Cell::new(false),
            pending_write_update_: Cell::new(false),
            poll_thread_: None,
            on_frame_: rusty::Mutex::<OnFrameCallback>::new(tcpconn_default_on_frame()),
            on_closed_: rusty::Mutex::<OnClosedCallback>::new(tcpconn_default_on_closed()),
            on_error_: rusty::Mutex::<OnErrorCallback>::new(tcpconn_default_on_error()),
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
        let mut guard = self.on_frame_.lock().unwrap();
        *guard = cb;
    }

    fn set_on_closed(&self, cb: OnClosedCallback) {
        let mut guard = self.on_closed_.lock().unwrap();
        *guard = cb;
    }

    fn set_on_error(&self, cb: OnErrorCallback) {
        let mut guard = self.on_error_.lock().unwrap();
        *guard = cb;
    }

    fn fd(&self) -> i32 {
        self.fd_.as_raw_fd()
    }

    // READ always; WRITE only while the outbound buffer is non-empty.
    fn poll_mode(&self) -> i32 {
        let mut mode: i32 = PollMode::READ;
        let guard = self.outbound_.lock().unwrap();
        if !(*guard).empty() {
            mode |= PollMode::WRITE;
        }
        mode
    }

    fn content_size(&self) -> usize {
        let guard = self.outbound_.lock().unwrap();
        (*guard).size() + self.inbound_.borrow().buffered_bytes()
    }

    fn handle_read(&self) -> bool {
        tcpconn_handle_read(self)
    }

    fn handle_write(&self) -> i32 {
        tcpconn_handle_write(self)
    }

    fn handle_error(&self) {
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
/*RUSTYCPP:GEN-BEGIN id=tcp_channel.conn version=1 rust_sha256=fef3dd0297308772d5cff2343b6d361cd6f17ee3497bea50c11b1a4520410cac*/
struct TcpConnection;

struct TcpConnection {
    rusty::os::fd::OwnedFd fd_;
    std::string peer_address_;
    size_t outbound_high_water_;
    rusty::Mutex<std::vector<uint8_t>> outbound_;
    rusty::RefCell<FrameStreamReader> inbound_;
    rusty::Cell<bool> closed_;
    rusty::Cell<bool> on_closed_fired_;
    rusty::Cell<bool> pending_write_update_;
    rusty::Option<rusty::Arc<PollThread>> poll_thread_;
    rusty::Mutex<OnFrameCallback> on_frame_;
    rusty::Mutex<OnClosedCallback> on_closed_;
    rusty::Mutex<OnErrorCallback> on_error_;

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
    size_t content_size() const;
    bool handle_read() const;
    int32_t handle_write() const;
    void handle_error() const;
    bool check_pending_write_update() const;
    void set_poll_thread(rusty::Arc<PollThread> pt);
};


TcpConnection::TcpConnection(int32_t fd, std::string peer_address)
    : fd_(rusty::os::fd::OwnedFd::from_raw_fd(std::move(fd)))
    , peer_address_(std::move(peer_address))
    , outbound_high_water_(kTcpConnectionOutboundHighWaterDefault)
    , outbound_(rusty::Mutex<std::vector<uint8_t>>::new_(tcpconn_empty_buf()))
    , inbound_(rusty::RefCell<FrameStreamReader>::new_(FrameStreamReader::new_()))
    , closed_(rusty::Cell<bool>::new_(false))
    , on_closed_fired_(rusty::Cell<bool>::new_(false))
    , pending_write_update_(rusty::Cell<bool>::new_(false))
    , poll_thread_(rusty::Option<rusty::Arc<PollThread>>{rusty::None})
    , on_frame_(rusty::Mutex<OnFrameCallback>::new_(tcpconn_default_on_frame()))
    , on_closed_(rusty::Mutex<OnClosedCallback>::new_(tcpconn_default_on_closed()))
    , on_error_(rusty::Mutex<OnErrorCallback>::new_(tcpconn_default_on_error()))
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
    auto guard = this->on_frame_.lock().unwrap();
    *guard = std::move(cb);
}

void TcpConnection::set_on_closed(OnClosedCallback cb) const {
    auto guard = this->on_closed_.lock().unwrap();
    *guard = std::move(cb);
}

void TcpConnection::set_on_error(OnErrorCallback cb) const {
    auto guard = this->on_error_.lock().unwrap();
    *guard = std::move(cb);
}

int32_t TcpConnection::fd() const {
    return this->fd_.as_raw_fd();
}

int32_t TcpConnection::poll_mode() const {
    int32_t mode = PollMode::READ;
    auto guard = this->outbound_.lock().unwrap();
    if (rusty::detail::rust_not(((*guard)).empty())) {
        mode |= PollMode::WRITE;
    }
    return std::move(mode);
}

size_t TcpConnection::content_size() const {
    auto guard = this->outbound_.lock().unwrap();
    return ((*guard)).size() + this->inbound_.borrow()->buffered_bytes();
}

bool TcpConnection::handle_read() const {
    return tcpconn_handle_read((*this));
}

int32_t TcpConnection::handle_write() const {
    return tcpconn_handle_write((*this));
}

void TcpConnection::handle_error() const {
    tcpconn_handle_error((*this));
}

bool TcpConnection::check_pending_write_update() const {
    if (rusty::detail::rust_not(this->pending_write_update_.get())) {
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
// `TcpPollableShim` — the Arc-holding PollableBase implementor over
// TcpConnection (same recipe as TcpChannelShim above: methods are
// &self now, so the shim forwards through the Arc with no const_cast).
#if RUSTYCPP_RUST
struct TcpPollableShim {
    conn_: Arc<TcpConnection>,
}

#[cpp_inherit]
impl PollableBase for TcpPollableShim {
    fn fd(&self) -> i32 {
        self.conn_.fd()
    }
    fn poll_mode(&self) -> i32 {
        self.conn_.poll_mode()
    }
    fn content_size(&mut self) -> usize {
        self.conn_.content_size()
    }
    fn handle_read(&mut self) -> bool {
        self.conn_.handle_read()
    }
    fn handle_write(&mut self) -> i32 {
        self.conn_.handle_write()
    }
    fn handle_error(&mut self) {
        self.conn_.handle_error()
    }
    fn close(&mut self) {
        self.conn_.close()
    }
    fn check_pending_write_update(&self) -> bool {
        self.conn_.check_pending_write_update()
    }
    fn is_closed(&self) -> bool {
        self.conn_.is_closed()
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=tcp_channel.pollable_shim version=1 rust_sha256=5a7a00daf84a4269edceb814bb284b5c11aec69daca8184daf427001bf1ce17e*/
struct TcpPollableShim;

struct TcpPollableShim : public PollableBase {
    rusty::Arc<TcpConnection> conn_;
    TcpPollableShim(rusty::Arc<TcpConnection> conn__init) : PollableBase(), conn_(std::move(conn__init)) {}
    TcpPollableShim(TcpPollableShim&& other) noexcept : PollableBase(), conn_(std::move(other.conn_)) {}


    int32_t fd() const;
    int32_t poll_mode() const;
    size_t content_size();
    bool handle_read();
    int32_t handle_write();
    void handle_error();
    void close();
    bool check_pending_write_update() const;
    bool is_closed() const;
};


int32_t TcpPollableShim::fd() const {
    return this->conn_->fd();
}

int32_t TcpPollableShim::poll_mode() const {
    return this->conn_->poll_mode();
}

size_t TcpPollableShim::content_size() {
    return this->conn_->content_size();
}

bool TcpPollableShim::handle_read() {
    return this->conn_->handle_read();
}

int32_t TcpPollableShim::handle_write() {
    return this->conn_->handle_write();
}

void TcpPollableShim::handle_error() {
    this->conn_->handle_error();
}

void TcpPollableShim::close() {
    this->conn_->close();
}

bool TcpPollableShim::check_pending_write_update() const {
    return this->conn_->check_pending_write_update();
}

bool TcpPollableShim::is_closed() const {
    return this->conn_->is_closed();
}
/*RUSTYCPP:GEN-END id=tcp_channel.pollable_shim*/

#if RUSTYCPP_RUST
fn make_tcp_connection_channel_proxy(conn: rusty::Arc<TcpConnection>) -> ChannelConnectionProxy {
    Box::new(TcpChannelShim { conn_: conn })
}
#endif
/*RUSTYCPP:GEN-BEGIN id=tcp_channel.5 version=1 rust_sha256=cec688b22d6120186bd07ebddbf762669cd64d100cb3bfb5c340b80cc89a4583*/
ChannelConnectionProxy make_tcp_connection_channel_proxy(rusty::Arc<TcpConnection> conn) {
    return rusty::Box<TcpChannelShim>::new_(TcpChannelShim(std::move(conn)));
}
/*RUSTYCPP:GEN-END id=tcp_channel.5*/

#if RUSTYCPP_RUST
fn make_tcp_connection_pollable_proxy(conn: rusty::Arc<TcpConnection>) -> PollableProxy {
    Box::new(TcpPollableShim { conn_: conn })
}
#endif
/*RUSTYCPP:GEN-BEGIN id=tcp_channel.6 version=1 rust_sha256=c80b3065b2404ec90e95c70039b7cac19a044d6d437ab13548c7a9d4f19654e6*/
PollableProxy make_tcp_connection_pollable_proxy(rusty::Arc<TcpConnection> conn) {
    return rusty::Box<TcpPollableShim>::new_(TcpPollableShim(std::move(conn)));
}
/*RUSTYCPP:GEN-END id=tcp_channel.6*/

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
// @safe - State is rusty::Cell / Option / rusty::Mutex / Arc /
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
bool         tcplistener_handle_read(const TcpListener& self);
void         tcplistener_handle_error(const TcpListener& self);

// Forward decls for the DSL listen() body below — both definitions
// live later in the file (io_kind_to_channel_error with the connection
// kernels — module-internal, so its declaration must leave the export
// block to keep one linkage; the proxy maker after the shims).
struct TcpListener;
inline PollableProxy make_tcp_listener_pollable_proxy(rusty::Arc<TcpListener> listener);
}  // export namespace rrr (paused for a module-linkage definition)
namespace rrr {
// @safe - Map a rusty::io::Error::Kind to a ChannelError. Used at the
// boundary where `rusty::net::*` operations return Result<T,io::Error>
// and we need to surface the failure as a ChannelError on the
// listener / connection API. Defined HERE (module linkage, before the
// TcpListener DSL block whose listen() calls it); previously lived in
// the impl-side anonymous namespace, but an exported inline method
// cannot name an internal-linkage helper.
// KERNEL by probe (arc cycle 9): foreign NESTED enum paths
// (rusty::io::Error::Kind::X) in DSL comparison position get the
// variant-call lowering (X() — invalid); only same-file DSL enums
// compare cleanly. This mapper therefore stays hand-written C++.
// @unsafe kernel by verdict: a match/comparison over rusty::io::Error::Kind
// is not DSL-expressible today -- ANY qualified C++ enum-class variant path
// (Kind::ConnectionRefused) emits a nullary variant CALL (Kind::ConnectionRefused()),
// the DSL-enum factory convention misapplied to a plain C++ enum. Same trap
// family as the docs 7.51-era notes; needs a transpiler-side "is this a DSL
// enum" check before appending parens.
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
}  // namespace rrr
export namespace rrr {

// Server-side TCP listener — owns a `rusty::net::TcpListener` (RAII-closes
// the listen fd on drop) + an accept loop. All fields are already rusty
// (net::TcpListener / Cell / Option / rusty::Mutex), so the struct is
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
    // Interior-mutable: `listen()` runs on the shared Arc facade (the
    // shim's &mut is on the Box, not the TcpListener), so the bind-time
    // writes go through RefCell instead of the old localized const_cast.
    listener_: RefCell<rusty::net::TcpListener>,
    bound_address_: RefCell<std::string>,
    closed_: Cell<bool>,
    listened_: Cell<bool>,
    poll_thread_: Option<Arc<PollThread>>,
    self_weak_: Option<rusty::sync::Weak<TcpListener>>,
    on_accept_: rusty::Mutex<OnAcceptCallback>,
    on_error_: rusty::Mutex<OnErrorCallback>,
}

impl TcpListener {
    #[cpp_ctor] fn new() -> TcpListener {
        TcpListener {
            listener_: RefCell::<rusty::net::TcpListener>::new(Default::default()),
            bound_address_: RefCell::<std::string>::new(Default::default()),
            closed_: Cell::new(false),
            listened_: Cell::new(false),
            poll_thread_: None,
            self_weak_: None,
            on_accept_: rusty::Mutex::<OnAcceptCallback>::new(Default::default()),
            on_error_: rusty::Mutex::<OnErrorCallback>::new(tcpconn_default_on_error()),
        }
    }

    // Bind path: rusty::net::TcpListener::bind + socket_addr_v4_from_str
    // + set_nonblocking — pure flow control over Results, all field
    // writes through the RefCells (listen runs once; the RefCell
    // replaces the old setup-time const_cast pattern).
    fn listen(&self, addr: std::string_view) -> ChannelError {
        if self.closed_.get() {
            return ChannelError_AddressInUse();
        }
        if self.listened_.get() {
            return ChannelError_AddressInUse();
        }
        let parse_result = rusty::net::socket_addr_v4_from_str(addr);
        if parse_result.is_err() {
            return ChannelError_AddressInvalid();
        }
        let bind_result = rusty::net::TcpListener::bind(parse_result.unwrap());
        if bind_result.is_err() {
            return io_kind_to_channel_error(bind_result.unwrap_err().kind());
        }
        {
            let mut g = self.listener_.borrow_mut();
            *g = bind_result.unwrap();
        }
        let nonblock_result = {
            let g = self.listener_.borrow();
            (*g).set_nonblocking(true)
        };
        if nonblock_result.is_err() {
            let ch = io_kind_to_channel_error(nonblock_result.unwrap_err().kind());
            let mut g = self.listener_.borrow_mut();
            let _closed = core::mem::take(&mut *g); // RAII close
            return ch;
        }
        // Discover actual bound address (port may have been 0).
        let local_result = {
            let g = self.listener_.borrow();
            (*g).local_addr()
        };
        {
            let mut b = self.bound_address_.borrow_mut();
            if local_result.is_ok() {
                *b = rusty::net::socket_addr_v4_to_string(local_result.unwrap());
            } else {
                *b = format!("{}", addr);
            }
        }
        self.listened_.set(true);
        // Auto-register with the poll thread if the factory wired one
        // in. The factory pre-installs `poll_thread_` and a weak
        // self-ref before handing the listener proxy to the user; we
        // upgrade the weak ref and hand a PollableProxy clone to the
        // poll thread.
        if self.poll_thread_.is_some() && self.self_weak_.is_some() {
            let self_opt = self.self_weak_.as_ref().unwrap().upgrade();
            if self_opt.is_some() {
                let pt: &Arc<PollThread> = self.poll_thread_.as_ref().unwrap();
                pt.add_proxy(make_tcp_listener_pollable_proxy(self_opt.unwrap()));
            }
        }
        ChannelError_None()
    }

    // Sets the closed latch and replaces the owned listener with a
    // default one — the old value drops here, RAII-closing the fd.
    fn close(&self) {
        if self.closed_.get() {
            return;
        }
        self.closed_.set(true);
        let mut g = self.listener_.borrow_mut();
        let _closed = core::mem::take(&mut *g);
    }

    fn is_closed(&self) -> bool {
        self.closed_.get()
    }

    fn local_address(&self) -> std::string {
        let g = self.bound_address_.borrow();
        (*g).clone()
    }

    fn set_on_accept(&self, cb: OnAcceptCallback) {
        let mut guard = self.on_accept_.lock().unwrap();
        *guard = cb;
    }

    fn set_on_error(&self, cb: OnErrorCallback) {
        let mut guard = self.on_error_.lock().unwrap();
        *guard = cb;
    }

    fn fd(&self) -> i32 {
        let g = self.listener_.borrow();
        (*g).as_owned_fd().as_raw_fd()
    }

    fn poll_mode(&self) -> i32 {
        PollMode::READ
    }

    fn content_size(&self) -> usize {
        0usize
    }

    fn handle_read(&self) -> bool {
        tcplistener_handle_read(self)
    }

    fn handle_write(&self) -> i32 {
        PollMode::NO_CHANGE
    }

    fn handle_error(&self) {
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
/*RUSTYCPP:GEN-BEGIN id=tcp_channel.listener version=1 rust_sha256=5ec5e9087bcc3f2c0cae06b9ba4b850d0f6da00b6f5441588b490788fece3426*/
struct TcpListener;

struct TcpListener {
    rusty::RefCell<rusty::net::TcpListener> listener_;
    rusty::RefCell<std::string> bound_address_;
    rusty::Cell<bool> closed_;
    rusty::Cell<bool> listened_;
    rusty::Option<rusty::Arc<PollThread>> poll_thread_;
    rusty::Option<rusty::sync::Weak<TcpListener>> self_weak_;
    rusty::Mutex<OnAcceptCallback> on_accept_;
    rusty::Mutex<OnErrorCallback> on_error_;

    TcpListener();
    ChannelError listen(std::string_view addr) const;
    void close() const;
    bool is_closed() const;
    std::string local_address() const;
    void set_on_accept(OnAcceptCallback cb) const;
    void set_on_error(OnErrorCallback cb) const;
    int32_t fd() const;
    int32_t poll_mode() const;
    size_t content_size() const;
    bool handle_read() const;
    int32_t handle_write() const;
    void handle_error() const;
    bool check_pending_write_update() const;
    void set_poll_thread(rusty::Arc<PollThread> pt);
    void set_self_weak(rusty::sync::Weak<TcpListener> self_weak);
};


TcpListener::TcpListener()
    : listener_(rusty::RefCell<rusty::net::TcpListener>::new_(rusty::default_like<rusty::net::TcpListener>()))
    , bound_address_(rusty::RefCell<std::string>::new_(rusty::default_like<std::string>()))
    , closed_(rusty::Cell<bool>::new_(false))
    , listened_(rusty::Cell<bool>::new_(false))
    , poll_thread_(rusty::Option<rusty::Arc<PollThread>>{rusty::None})
    , self_weak_(rusty::Option<rusty::sync::Weak<TcpListener>>{rusty::None})
    , on_accept_(rusty::Mutex<OnAcceptCallback>::new_(rusty::default_like<OnAcceptCallback>()))
    , on_error_(rusty::Mutex<OnErrorCallback>::new_(tcpconn_default_on_error()))
{}

ChannelError TcpListener::listen(std::string_view addr) const {
    if (this->closed_.get()) {
        return ChannelError_AddressInUse();
    }
    if (this->listened_.get()) {
        return ChannelError_AddressInUse();
    }
    auto parse_result = rusty::net::socket_addr_v4_from_str(std::move(addr));
    if (parse_result.is_err()) {
        return ChannelError_AddressInvalid();
    }
    auto bind_result = rusty::net::TcpListener::bind(parse_result.unwrap());
    if (bind_result.is_err()) {
        return io_kind_to_channel_error(bind_result.unwrap_err().kind());
    }
    {
        auto g = this->listener_.borrow_mut();
        *g = bind_result.unwrap();
    }
    auto nonblock_result = [&]() { const auto g = this->listener_.borrow();
return ((*g)).set_nonblocking(true); }();
    if (nonblock_result.is_err()) {
        auto ch = io_kind_to_channel_error(nonblock_result.unwrap_err().kind());
        auto g = this->listener_.borrow_mut();
        const auto _closed = rusty::mem::take(*g);
        return std::move(ch);
    }
    auto local_result = [&]() { const auto g = this->listener_.borrow();
return ((*g)).local_addr(); }();
    {
        auto b = this->bound_address_.borrow_mut();
        if (local_result.is_ok()) {
            *b = rusty::net::socket_addr_v4_to_string(local_result.unwrap());
        } else {
            *b = std::format("{}" , addr);
        }
    }
    this->listened_.set(true);
    if (this->poll_thread_.is_some() && this->self_weak_.is_some()) {
        auto self_opt = this->self_weak_.as_ref().unwrap().upgrade();
        if (self_opt.is_some()) {
            const rusty::Arc<PollThread>& pt = this->poll_thread_.as_ref().unwrap();
            pt->add_proxy(make_tcp_listener_pollable_proxy(self_opt.unwrap()));
        }
    }
    return ChannelError_None();
}

void TcpListener::close() const {
    if (this->closed_.get()) {
        return;
    }
    this->closed_.set(true);
    auto g = this->listener_.borrow_mut();
    const auto _closed = rusty::mem::take(*g);
}

bool TcpListener::is_closed() const {
    return this->closed_.get();
}

std::string TcpListener::local_address() const {
    const auto g = this->bound_address_.borrow();
    return rusty::clone(((*g)));
}

void TcpListener::set_on_accept(OnAcceptCallback cb) const {
    auto guard = this->on_accept_.lock().unwrap();
    *guard = std::move(cb);
}

void TcpListener::set_on_error(OnErrorCallback cb) const {
    auto guard = this->on_error_.lock().unwrap();
    *guard = std::move(cb);
}

int32_t TcpListener::fd() const {
    const auto g = this->listener_.borrow();
    return ((*g)).as_owned_fd().as_raw_fd();
}

int32_t TcpListener::poll_mode() const {
    return PollMode::READ;
}

size_t TcpListener::content_size() const {
    return static_cast<size_t>(0);
}

bool TcpListener::handle_read() const {
    return tcplistener_handle_read((*this));
}

int32_t TcpListener::handle_write() const {
    return PollMode::NO_CHANGE;
}

void TcpListener::handle_error() const {
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
// `TcpListenerChannelShim` / `TcpListenerPollableShim` — the
// Arc-holding trait implementors over TcpListener (the TcpChannelShim
// recipe: methods are &self now; forwarding through the Arc, no
// const_cast idiom).
#if RUSTYCPP_RUST
struct TcpListenerChannelShim {
    listener_: Arc<TcpListener>,
}

#[cpp_inherit]
impl ChannelListenerBase for TcpListenerChannelShim {
    fn listen(&mut self, a: std::string_view) -> ChannelError {
        self.listener_.listen(a)
    }
    fn close(&mut self) {
        self.listener_.close()
    }
    fn is_closed(&self) -> bool {
        self.listener_.is_closed()
    }
    fn local_address(&self) -> std::string {
        self.listener_.local_address()
    }
    fn set_on_accept(&mut self, cb: OnAcceptCallback) {
        self.listener_.set_on_accept(cb)
    }
    fn set_on_error(&mut self, cb: OnErrorCallback) {
        self.listener_.set_on_error(cb)
    }
}

struct TcpListenerPollableShim {
    listener_: Arc<TcpListener>,
}

#[cpp_inherit]
impl PollableBase for TcpListenerPollableShim {
    fn fd(&self) -> i32 {
        self.listener_.fd()
    }
    fn poll_mode(&self) -> i32 {
        self.listener_.poll_mode()
    }
    fn content_size(&mut self) -> usize {
        self.listener_.content_size()
    }
    fn handle_read(&mut self) -> bool {
        self.listener_.handle_read()
    }
    fn handle_write(&mut self) -> i32 {
        self.listener_.handle_write()
    }
    fn handle_error(&mut self) {
        self.listener_.handle_error()
    }
    fn close(&mut self) {
        self.listener_.close()
    }
    fn check_pending_write_update(&self) -> bool {
        self.listener_.check_pending_write_update()
    }
    fn is_closed(&self) -> bool {
        self.listener_.is_closed()
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=tcp_channel.listener_shims version=1 rust_sha256=45ae70513ed938eb2afcc7e7a282bc90a26a47f005e5563dba286d723f323c9c*/
struct TcpListenerChannelShim;
struct TcpListenerPollableShim;

struct TcpListenerChannelShim : public ChannelListenerBase {
    rusty::Arc<TcpListener> listener_;
    TcpListenerChannelShim(rusty::Arc<TcpListener> listener__init) : ChannelListenerBase(), listener_(std::move(listener__init)) {}
    TcpListenerChannelShim(TcpListenerChannelShim&& other) noexcept : ChannelListenerBase(), listener_(std::move(other.listener_)) {}


    ChannelError listen(std::string_view a);
    void close();
    bool is_closed() const;
    std::string local_address() const;
    void set_on_accept(OnAcceptCallback cb);
    void set_on_error(OnErrorCallback cb);
};

struct TcpListenerPollableShim : public PollableBase {
    rusty::Arc<TcpListener> listener_;
    TcpListenerPollableShim(rusty::Arc<TcpListener> listener__init) : PollableBase(), listener_(std::move(listener__init)) {}
    TcpListenerPollableShim(TcpListenerPollableShim&& other) noexcept : PollableBase(), listener_(std::move(other.listener_)) {}


    int32_t fd() const;
    int32_t poll_mode() const;
    size_t content_size();
    bool handle_read();
    int32_t handle_write();
    void handle_error();
    void close();
    bool check_pending_write_update() const;
    bool is_closed() const;
};


ChannelError TcpListenerChannelShim::listen(std::string_view a) {
    return this->listener_->listen(std::move(rusty::to_string_view(a)));
}

void TcpListenerChannelShim::close() {
    this->listener_->close();
}

bool TcpListenerChannelShim::is_closed() const {
    return this->listener_->is_closed();
}

std::string TcpListenerChannelShim::local_address() const {
    return this->listener_->local_address();
}

void TcpListenerChannelShim::set_on_accept(OnAcceptCallback cb) {
    this->listener_->set_on_accept(std::move(cb));
}

void TcpListenerChannelShim::set_on_error(OnErrorCallback cb) {
    this->listener_->set_on_error(std::move(cb));
}

int32_t TcpListenerPollableShim::fd() const {
    return this->listener_->fd();
}

int32_t TcpListenerPollableShim::poll_mode() const {
    return this->listener_->poll_mode();
}

size_t TcpListenerPollableShim::content_size() {
    return this->listener_->content_size();
}

bool TcpListenerPollableShim::handle_read() {
    return this->listener_->handle_read();
}

int32_t TcpListenerPollableShim::handle_write() {
    return this->listener_->handle_write();
}

void TcpListenerPollableShim::handle_error() {
    this->listener_->handle_error();
}

void TcpListenerPollableShim::close() {
    this->listener_->close();
}

bool TcpListenerPollableShim::check_pending_write_update() const {
    return this->listener_->check_pending_write_update();
}

bool TcpListenerPollableShim::is_closed() const {
    return this->listener_->is_closed();
}
/*RUSTYCPP:GEN-END id=tcp_channel.listener_shims*/

inline ChannelListenerProxy make_tcp_listener_channel_proxy(
    rusty::Arc<TcpListener> listener) {
    return rusty::make_box<TcpListenerChannelShim>(std::move(listener));
}

#if RUSTYCPP_RUST
fn make_tcp_listener_pollable_proxy(listener: rusty::Arc<TcpListener>) -> PollableProxy {
    Box::new(TcpListenerPollableShim { listener_: listener })
}
#endif
/*RUSTYCPP:GEN-BEGIN id=tcp_channel.9 version=1 rust_sha256=e3f0b9dfcfeece4fcfe5b99a84af0520abb93fe9676abc78fe640e12c43b51ec*/
PollableProxy make_tcp_listener_pollable_proxy(rusty::Arc<TcpListener> listener) {
    return rusty::Box<TcpListenerPollableShim>::new_(TcpListenerPollableShim(std::move(listener)));
}
/*RUSTYCPP:GEN-END id=tcp_channel.9*/

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
struct TcpFactory;
ConnectResult                       tcp_factory_connect(const TcpFactory& self, std::string_view addr);
rusty::Option<ChannelListenerProxy> tcp_factory_make_listener(const TcpFactory& self);

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

    // Socket + connect path (kernel does the syscalls).
    fn connect(&self, addr: std::string_view) -> ConnectResult {
        tcp_factory_connect(self, addr)
    }

    fn make_listener(&self) -> Option<ChannelListenerProxy> {
        tcp_factory_make_listener(self)
    }

    fn set_connect_timeout_ms(&mut self, timeout_ms: i32) {
        self.connect_timeout_ms_ = timeout_ms;
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=tcp_channel.factory version=1 rust_sha256=d43919efde1a49f808f725b869069277bcf518d0c536d1c7d016d0d36dc97288*/
struct TcpFactory;

struct TcpFactory {
    rusty::Arc<PollThread> poll_thread_;
    int32_t connect_timeout_ms_;

    static TcpFactory new_(rusty::Arc<PollThread> poll_thread);
    std::string backend_name() const;
    ConnectResult connect(std::string_view addr) const;
    rusty::Option<ChannelListenerProxy> make_listener() const;
    void set_connect_timeout_ms(int32_t timeout_ms);
};


TcpFactory TcpFactory::new_(rusty::Arc<PollThread> poll_thread) {
    return TcpFactory{.poll_thread_ = std::move(poll_thread), .connect_timeout_ms_ = static_cast<int32_t>(5000)};
}

std::string TcpFactory::backend_name() const {
    return std::string("tcp");
}

ConnectResult TcpFactory::connect(std::string_view addr) const {
    return tcp_factory_connect((*this), std::move(addr));
}

rusty::Option<ChannelListenerProxy> TcpFactory::make_listener() const {
    return tcp_factory_make_listener((*this));
}

void TcpFactory::set_connect_timeout_ms(int32_t timeout_ms) {
    this->connect_timeout_ms_ = std::move(timeout_ms);
}
/*RUSTYCPP:GEN-END id=tcp_channel.factory*/

// Free functions (non-DSL) — see definitions further down.
// `TcpFactoryShim` — Arc-holding ChannelFactoryBase implementor
// (same recipe as the four shims above; no const_cast idiom).
#if RUSTYCPP_RUST
struct TcpFactoryShim {
    factory_: Arc<TcpFactory>,
}

#[cpp_inherit]
impl ChannelFactoryBase for TcpFactoryShim {
    fn connect(&mut self, addr: std::string_view) -> ConnectResult {
        self.factory_.connect(addr)
    }
    fn make_listener(&mut self) -> Option<ChannelListenerProxy> {
        self.factory_.make_listener()
    }
    fn backend_name(&self) -> std::string {
        self.factory_.backend_name()
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=tcp_channel.factory_shim version=1 rust_sha256=fc1345d561aeeac70720fcc9c259cee8fcd14a33d56fc1eb66b83887e2373474*/
struct TcpFactoryShim;

struct TcpFactoryShim : public ChannelFactoryBase {
    rusty::Arc<TcpFactory> factory_;
    TcpFactoryShim(rusty::Arc<TcpFactory> factory__init) : ChannelFactoryBase(), factory_(std::move(factory__init)) {}
    TcpFactoryShim(TcpFactoryShim&& other) noexcept : ChannelFactoryBase(), factory_(std::move(other.factory_)) {}


    ConnectResult connect(std::string_view addr);
    rusty::Option<ChannelListenerProxy> make_listener();
    std::string backend_name() const;
};


ConnectResult TcpFactoryShim::connect(std::string_view addr) {
    return this->factory_->connect(std::move(rusty::to_string_view(addr)));
}

rusty::Option<ChannelListenerProxy> TcpFactoryShim::make_listener() {
    return this->factory_->make_listener();
}

std::string TcpFactoryShim::backend_name() const {
    return this->factory_->backend_name();
}
/*RUSTYCPP:GEN-END id=tcp_channel.factory_shim*/

#if RUSTYCPP_RUST
fn make_tcp_factory_proxy(factory: rusty::Arc<TcpFactory>) -> ChannelFactoryProxy {
    Box::new(TcpFactoryShim { factory_: factory })
}
#endif
/*RUSTYCPP:GEN-BEGIN id=tcp_channel.14 version=1 rust_sha256=5ee8f1277d7c5520dfeffe29a9ef7950630cdfae0a8d244fd74264ec1e8cb716*/
ChannelFactoryProxy make_tcp_factory_proxy(rusty::Arc<TcpFactory> factory) {
    return rusty::Box<TcpFactoryShim>::new_(TcpFactoryShim(std::move(factory)));
}
/*RUSTYCPP:GEN-END id=tcp_channel.14*/

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
// Outbound byte buffer alias so the DSL can spell the parameter type.
using TcpOutBuf = std::vector<std::uint8_t>;
ChannelError tcpconn_drain_outbound_locked(const TcpConnection& conn, TcpOutBuf& buf);
// Stack scratch for the recv drain (the DSL has no local arrays; the
// C-array field decays at the kernel call site; guaranteed RVO makes
// the factory return copy-free).
struct RecvScratch { std::uint8_t arr[kRecvScratchBytes]; };
RecvScratch* tcpconn_scratch();
int64_t      tcpconn_recv_bytes(const TcpConnection& conn, RecvScratch* s);
FrameDecodeStatus tcpconn_next_frame(const TcpConnection& conn, FrameView* v);
void         tcpconn_append_inbound(const TcpConnection& conn, std::size_t n);
void         tcpconn_consume_inbound(const TcpConnection& conn);
void         tcpconn_reset_inbound(const TcpConnection& conn);
int64_t      tcpconn_send_bytes(const TcpConnection& conn, TcpOutBuf& buf, size_t offset);
void         tcpconn_trim_sent(TcpOutBuf& buf, size_t offset);
void         tcpconn_drop_after_error(TcpOutBuf& buf, size_t offset);
void         tcpconn_deliver_on_closed_locked(const TcpConnection& conn, ChannelError reason);
ChannelError tcpconn_errno_to_channel_error(int err);

// Authored as inline Rust DSL; the raw-payload encode stays in the
// `frame_codec_encode_into` kernel (pointer arithmetic + memcpy), the
// DSL owns the control flow around it.
//
// The extended-header flag is always clear here: `frame.size` in the
// channel's `ChannelFrame` is the *raw payload byte count* (no flag),
// and the codec sets the flag when the RPC layer constructs a
// response — see the pre-DSL comment history for the wire-compat
// rationale.
//
// The wake path mirrors the legacy fd path's idiom in
// `ClientConnection::replay_pending_requests`: on the poll thread just
// set the deferred flag (poll_loop picks it up via
// `check_pending_write_update`); off the poll thread post
// `update_mode(fd, READ|WRITE)` directly — posting writes the mpsc
// channel's eventfd and wakes `epoll_wait` immediately. Without this,
// multi-threaded senders contend on the non-atomic
// `pending_write_update_` Cell and lose wake-ups (the
// MultiThreadedStressTest 100-thread wedge, TODO-srpc 4g1b). The
// `poll_thread_` slot may be None for socketpair-driven unit tests
// (single-threaded — flag-poll is fine).
#if RUSTYCPP_RUST
fn tcpconn_send_frame(conn: &TcpConnection, frame: &ChannelFrame) -> ChannelError {
    if conn.closed_.get() {
        return ChannelError_ConnectionReset();
    }
    if frame.size > 0usize && frame.payload.is_null() {
        return ChannelError_Internal();
    }
    if frame.size > (kMaxFramePayloadSize as usize) {
        return ChannelError_Internal();
    }
    let extended_header_flag = false;

    let mut guard = conn.outbound_.lock().unwrap();

    // Reject when the queue is already past the high water — we never
    // append to a buffer that's already over budget so backpressure is
    // strictly bounded.
    if (*guard).len() >= conn.outbound_high_water_ {
        return ChannelError_WouldBlock();
    }

    if !frame_codec_encode_into(&mut *guard, frame.payload,
                                frame.size as i32, extended_header_flag) {
        return ChannelError_Internal();
    }

    if conn.poll_thread_.is_some() && !pollworker_is_on_poll_thread() {
        let pt: &Arc<PollThread> = conn.poll_thread_.as_ref().unwrap();
        pt.update_mode(conn.fd_.as_raw_fd(), PollMode::READ | PollMode::WRITE);
    } else {
        conn.pending_write_update_.set(true);
    }
    ChannelError_None()
}
#endif
/*RUSTYCPP:GEN-BEGIN id=tcp_channel.12 version=1 rust_sha256=69e85f7d58c9ced9cd4d0a723ed1f253cad724e3794ebe8e7dae982f1a9f4f31*/
ChannelError tcpconn_send_frame(const TcpConnection& conn, const ChannelFrame& frame) {
    if (conn.closed_.get()) {
        return ChannelError_ConnectionReset();
    }
    if ((rusty::detail::deref_if_pointer_like(frame.size) > static_cast<size_t>(0)) && (frame.payload == nullptr)) {
        return ChannelError_Internal();
    }
    if (rusty::detail::deref_if_pointer_like(frame.size) > ((static_cast<size_t>(kMaxFramePayloadSize)))) {
        return ChannelError_Internal();
    }
    const auto extended_header_flag = false;
    auto&& guard = rusty::deref_call(conn.outbound_.lock(), rusty::detail::__mdisp_unwrap{});
    if (rusty::len((rusty::detail::deref_if_pointer_like(guard))) >= rusty::detail::deref_if_pointer_like(conn.outbound_high_water_)) {
        return ChannelError_WouldBlock();
    }
    if (rusty::detail::rust_not(frame_codec_encode_into(rusty::detail::deref_if_pointer_like(guard), frame.payload, static_cast<int32_t>(frame.size), std::move(extended_header_flag)))) {
        return ChannelError_Internal();
    }
    if (conn.poll_thread_.is_some() && rusty::detail::rust_not(pollworker_is_on_poll_thread())) {
        const rusty::Arc<PollThread>& pt = conn.poll_thread_.as_ref().unwrap();
        pt->update_mode(conn.fd_.as_raw_fd(), rusty::clone(PollMode::READ) | rusty::clone(PollMode::WRITE));
    } else {
        conn.pending_write_update_.set(true);
    }
    return ChannelError_None();
}
/*RUSTYCPP:GEN-END id=tcp_channel.12*/

// @unsafe - drives tcpconn_drain_outbound_locked (which is @unsafe for
// raw `uint8_t*` arithmetic + send syscall).
#if RUSTYCPP_RUST
fn tcpconn_flush(conn: &TcpConnection) {
    if conn.closed_.get() {
        return;
    }
    let mut guard = conn.outbound_.lock().unwrap();
    if (*guard).empty() {
        return;
    }
    // Best-effort immediate drain; errors are reported via the
    // connection callbacks on the next poll cycle (see the pre-DSL
    // comment history for the poll-thread hand-off rationale).
    let result = tcpconn_drain_outbound_locked(conn, &mut *guard);
    if result != ChannelError::None && result != ChannelError::WouldBlock {
        conn.closed_.set(true);
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=tcp_channel.flush version=1 rust_sha256=ad19717ea74e4f070efb18e5e74333e65729f3ce9aa8692a3b1a821bd779901e*/
void tcpconn_flush(const TcpConnection& conn) {
    if (conn.closed_.get()) {
        return;
    }
    auto&& guard = rusty::deref_call(conn.outbound_.lock(), rusty::detail::__mdisp_unwrap{});
    if (((rusty::detail::deref_if_pointer_like(guard))).empty()) {
        return;
    }
    const auto result = tcpconn_drain_outbound_locked(conn, rusty::detail::deref_if_pointer_like(guard));
    if ((rusty::detail::deref_if_pointer_like(result) != rusty::detail::deref_if_pointer_like(ChannelError::None)) && (rusty::detail::deref_if_pointer_like(result) != rusty::detail::deref_if_pointer_like(ChannelError::WouldBlock))) {
        conn.closed_.set(true);
    }
}
/*RUSTYCPP:GEN-END id=tcp_channel.flush*/

// @unsafe - ::shutdown libc syscall + OwnedFd RAII close + callback fire.
// Latch + orderly shutdown + RAII fd drop, authored in the DSL; the
// libc shutdown(2) is an expression-shaped unsafe{} call (SHUT_RDWR
// lowers as an identifier) and the const-facade fd reset stays a
// 1-line const_cast kernel.
#if RUSTYCPP_RUST
fn tcpconn_close(conn: &TcpConnection) {
    if conn.closed_.get() {
        return;
    }
    conn.closed_.set(true);
    if conn.fd_.is_valid() {
        unsafe { shutdown(conn.fd_.as_raw_fd(), SHUT_RDWR); }
        tcpconn_reset_fd(conn);
    }
    tcpconn_deliver_on_closed_locked(conn, ChannelError_None());
}
#endif
/*RUSTYCPP:GEN-BEGIN id=tcp_channel.conn_close version=1 rust_sha256=6dfb9284005dada621038e02ddbb3d7b8b17798ff8e0553556a7487183faeb9c*/
void tcpconn_close(const TcpConnection& conn) {
    if (conn.closed_.get()) {
        return;
    }
    conn.closed_.set(true);
    if (conn.fd_.is_valid()) {
        // @unsafe
        {
            shutdown(conn.fd_.as_raw_fd(), SHUT_RDWR);
        }
        tcpconn_reset_fd(conn);
    }
    tcpconn_deliver_on_closed_locked(conn, ChannelError_None());
}
/*RUSTYCPP:GEN-END id=tcp_channel.conn_close*/

// @unsafe - the documented localized-const_cast fd teardown (plain
// field assignment on the const facade; RAII-closes via OwnedFd).
void tcpconn_reset_fd(const TcpConnection& conn) {
    const_cast<TcpConnection&>(conn).fd_ = rusty::os::fd::OwnedFd{};
}

// @unsafe - last-writer-wins callback store under the spinlock.



// ---------------------------------------------------------------------------
// Pollable methods
// ---------------------------------------------------------------------------

// @safe - peeks the outbound queue length under the spinlock.


// @unsafe - recv(2) syscall into a raw `char` scratch buffer +
// FrameStreamReader::append / next_frame / consume_frame are all
// @unsafe + raw `uint8_t*` payload pointers stored on the FrameView.
// Read-readiness: drain recv(2) into the frame reader (edge-trigger
// safe), then dispatch complete frames — all DSL; the syscall, the
// scratch buffer, and the small POD builders live in kernels.
#if RUSTYCPP_RUST
fn tcpconn_handle_read(conn: &TcpConnection) -> bool {
    if conn.closed_.get() {
        return false;
    }
    let mut any_progress = false;
    let mut draining = true;
    while draining {
        let n = tcpconn_recv_bytes(conn, tcpconn_scratch());
        if n > 0 {
            tcpconn_append_inbound(conn, n as usize);
            any_progress = true;
            if (n as usize) < kRecvScratchBytes {
                draining = false;
            }
        } else if n == 0 {
            // Peer closed cleanly: no on_error, just the close latch.
            conn.closed_.set(true);
            tcpconn_reset_fd(conn);
            tcpconn_deliver_on_closed_locked(conn, ChannelError_None());
            return false;
        } else {
            let err: i32 = errno;
            if err == EAGAIN || err == EWOULDBLOCK {
                draining = false;
            } else if err == EINTR {
                // retry — loop continues
            } else {
                let ch = tcpconn_errno_to_channel_error(err);
                {
                    let mut guard = conn.on_error_.lock().unwrap();
                    if *guard {
                        (*guard)(ch, strerror(err));
                    }
                }
                conn.closed_.set(true);
                tcpconn_reset_fd(conn);
                tcpconn_deliver_on_closed_locked(conn, ch);
                return false;
            }
        }
    }

    let mut decoding = true;
    while decoding {
        let mut v: FrameView = Default::default();
        let s = tcpconn_next_frame(conn, &mut v);
        if s == FrameDecodeStatus::Complete {
            let cf = ChannelFrame { payload: v.payload, size: v.payload_size };
            {
                let mut guard = conn.on_frame_.lock().unwrap();
                if *guard {
                    (*guard)(cf);
                }
            }
            tcpconn_consume_inbound(conn);
        } else if s == FrameDecodeStatus::NeedMoreBytes {
            decoding = false;
        } else {
            // Malformed inbound stream.
            {
                let mut guard = conn.on_error_.lock().unwrap();
                if *guard {
                    (*guard)(ChannelError_Internal(), "malformed frame on inbound stream");
                }
            }
            conn.closed_.set(true);
            tcpconn_reset_fd(conn);
            tcpconn_reset_inbound(conn);
            tcpconn_deliver_on_closed_locked(conn, ChannelError_Internal());
            return false;
        }
    }
    any_progress
}
#endif
/*RUSTYCPP:GEN-BEGIN id=tcp_channel.handle_read version=1 rust_sha256=0a9f9cd82cdfa7eec33669e9c9df0b123743e056812b7884a668f99d4b56dad0*/
bool tcpconn_handle_read(const TcpConnection& conn) {
    if (conn.closed_.get()) {
        return false;
    }
    auto any_progress = false;
    auto draining = true;
    while (draining) {
        const auto n = tcpconn_recv_bytes(conn, tcpconn_scratch());
        if (rusty::detail::deref_if_pointer_like(n) > 0) {
            tcpconn_append_inbound(conn, static_cast<size_t>(n));
            any_progress = true;
            if (((static_cast<size_t>(n))) < rusty::detail::deref_if_pointer_like(kRecvScratchBytes)) {
                draining = false;
            }
        } else if (rusty::detail::deref_if_pointer_like(n) == 0) {
            conn.closed_.set(true);
            tcpconn_reset_fd(conn);
            tcpconn_deliver_on_closed_locked(conn, ChannelError_None());
            return false;
        } else {
            const int32_t err = errno;
            if ((rusty::detail::deref_if_pointer_like(err) == rusty::detail::deref_if_pointer_like(EAGAIN)) || (rusty::detail::deref_if_pointer_like(err) == rusty::detail::deref_if_pointer_like(EWOULDBLOCK))) {
                draining = false;
            } else if (rusty::detail::deref_if_pointer_like(err) == rusty::detail::deref_if_pointer_like(EINTR)) {
            } else {
                const auto ch = tcpconn_errno_to_channel_error(std::move(err));
                {
                    auto&& guard = rusty::deref_call(conn.on_error_.lock(), rusty::detail::__mdisp_unwrap{});
                    if (rusty::detail::deref_if_pointer_like(guard)) {
                        (rusty::detail::deref_if_pointer_like(guard))(std::move(ch), strerror(std::move(err)));
                    }
                }
                conn.closed_.set(true);
                tcpconn_reset_fd(conn);
                tcpconn_deliver_on_closed_locked(conn, std::move(ch));
                return false;
            }
        }
    }
    auto decoding = true;
    while (decoding) {
        FrameView v = rusty::default_like<FrameView>();
        const auto s = tcpconn_next_frame(conn, &v);
        if (rusty::detail::deref_if_pointer_like(s) == rusty::detail::deref_if_pointer_like(FrameDecodeStatus::Complete)) {
            const auto cf = ChannelFrame{.payload = std::move(v.payload), .size = std::move(v.payload_size)};
            {
                auto&& guard = rusty::deref_call(conn.on_frame_.lock(), rusty::detail::__mdisp_unwrap{});
                if (rusty::detail::deref_if_pointer_like(guard)) {
                    (rusty::detail::deref_if_pointer_like(guard))(std::move(cf));
                }
            }
            tcpconn_consume_inbound(conn);
        } else if (rusty::detail::deref_if_pointer_like(s) == rusty::detail::deref_if_pointer_like(FrameDecodeStatus::NeedMoreBytes)) {
            decoding = false;
        } else {
            {
                auto&& guard = rusty::deref_call(conn.on_error_.lock(), rusty::detail::__mdisp_unwrap{});
                if (rusty::detail::deref_if_pointer_like(guard)) {
                    (rusty::detail::deref_if_pointer_like(guard))(ChannelError_Internal(), "malformed frame on inbound stream");
                }
            }
            conn.closed_.set(true);
            tcpconn_reset_fd(conn);
            tcpconn_reset_inbound(conn);
            tcpconn_deliver_on_closed_locked(conn, ChannelError_Internal());
            return false;
        }
    }
    return std::move(any_progress);
}
/*RUSTYCPP:GEN-END id=tcp_channel.handle_read*/

// @unsafe - per-poll-thread recv scratch (single-threaded per
// connection by the poll contract; thread_local keeps 64 KiB off the
// hot stack and out of the DSL's grammar).
RecvScratch* tcpconn_scratch() {
    static thread_local RecvScratch s;
    return &s;
}

// @unsafe - recv(2) into the scratch.
int64_t tcpconn_recv_bytes(const TcpConnection& conn, RecvScratch* s) {
    return ::recv(conn.fd_.as_raw_fd(), s->arr, sizeof(s->arr), 0);
}

// @unsafe - RefCell arrow into the frame reader (append the scratch
// prefix that recv filled).
void tcpconn_append_inbound(const TcpConnection& conn, std::size_t n) {
    conn.inbound_.borrow_mut()->append(tcpconn_scratch()->arr, n);
}

// @unsafe - RefCell arrows into the frame reader.
FrameDecodeStatus tcpconn_next_frame(const TcpConnection& conn, FrameView* v) {
    return conn.inbound_.borrow()->next_frame(*v);
}
#if RUSTYCPP_RUST
fn tcpconn_consume_inbound(conn: &TcpConnection) {
    let mut guard = conn.inbound_.borrow_mut();
    guard.consume_frame();
}

fn tcpconn_reset_inbound(conn: &TcpConnection) {
    let mut guard = conn.inbound_.borrow_mut();
    guard.reset();
}
#endif
/*RUSTYCPP:GEN-BEGIN id=tcp_channel.13 version=1 rust_sha256=0317def90e6a944dc338fde6c6ce54481f1c9df9c90b0d730bef6b8262963d17*/
void tcpconn_consume_inbound(const TcpConnection& conn) {
    auto&& guard = conn.inbound_.borrow_mut();
    rusty::deref_call(guard, rusty::detail::__mdisp_consume_frame{});
}

void tcpconn_reset_inbound(const TcpConnection& conn) {
    auto&& guard = conn.inbound_.borrow_mut();
    rusty::deref_call(guard, rusty::detail::__mdisp_reset{});
}
/*RUSTYCPP:GEN-END id=tcp_channel.13*/

// Write-readiness: drain what the kernel will take, keep or drop the
// WRITE interest, and run the hard-error teardown — all DSL; the send
// syscall and the erase surgery live in the kernels below.
#if RUSTYCPP_RUST
fn tcpconn_handle_write(conn: &TcpConnection) -> i32 {
    if conn.closed_.get() {
        return PollMode::NO_CHANGE;
    }
    let mut guard = conn.outbound_.lock().unwrap();
    if (*guard).empty() {
        return PollMode::READ;
    }
    let result = tcpconn_drain_outbound_locked(conn, &mut *guard);
    if result == ChannelError::None {
        if (*guard).empty() {
            return PollMode::READ;
        }
        return PollMode::NO_CHANGE;
    }
    if result == ChannelError::WouldBlock {
        return PollMode::NO_CHANGE;
    }
    {
        let mut eg = conn.on_error_.lock().unwrap();
        if *eg {
            (*eg)(result, "outbound write failed");
        }
    }
    conn.closed_.set(true);
    tcpconn_reset_fd(conn);
    tcpconn_deliver_on_closed_locked(conn, result);
    PollMode::READ
}
#endif
/*RUSTYCPP:GEN-BEGIN id=tcp_channel.handle_write version=1 rust_sha256=d62ba418ca1472463bd8865b4c52d0479af7c5bb6213342601067267ca6672a4*/
int32_t tcpconn_handle_write(const TcpConnection& conn) {
    if (conn.closed_.get()) {
        return PollMode::NO_CHANGE;
    }
    auto&& guard = rusty::deref_call(conn.outbound_.lock(), rusty::detail::__mdisp_unwrap{});
    if (((rusty::detail::deref_if_pointer_like(guard))).empty()) {
        return PollMode::READ;
    }
    const auto result = tcpconn_drain_outbound_locked(conn, rusty::detail::deref_if_pointer_like(guard));
    if (rusty::detail::deref_if_pointer_like(result) == rusty::detail::deref_if_pointer_like(ChannelError::None)) {
        if (((rusty::detail::deref_if_pointer_like(guard))).empty()) {
            return PollMode::READ;
        }
        return PollMode::NO_CHANGE;
    }
    if (rusty::detail::deref_if_pointer_like(result) == rusty::detail::deref_if_pointer_like(ChannelError::WouldBlock)) {
        return PollMode::NO_CHANGE;
    }
    {
        auto&& eg = rusty::deref_call(conn.on_error_.lock(), rusty::detail::__mdisp_unwrap{});
        if (rusty::detail::deref_if_pointer_like(eg)) {
            (rusty::detail::deref_if_pointer_like(eg))(std::move(result), "outbound write failed");
        }
    }
    conn.closed_.set(true);
    tcpconn_reset_fd(conn);
    tcpconn_deliver_on_closed_locked(conn, std::move(result));
    return PollMode::READ;
}
/*RUSTYCPP:GEN-END id=tcp_channel.handle_write*/

// @unsafe - fires on_error callback + drives tcpconn_close (::shutdown).
#if RUSTYCPP_RUST
fn tcpconn_handle_error(conn: &TcpConnection) {
    if conn.closed_.get() {
        return;
    }
    {
        let mut guard = conn.on_error_.lock().unwrap();
        if *guard {
            (*guard)(ChannelError_Internal(), "epoll/poll signaled error");
        }
    }
    tcpconn_close(conn);
}
#endif
/*RUSTYCPP:GEN-BEGIN id=tcp_channel.handle_error version=1 rust_sha256=5df98346f620c114fb5dbdaca8d90b11ce46b3e4b9772d4c81efc9f5017faa3b*/
void tcpconn_handle_error(const TcpConnection& conn) {
    if (conn.closed_.get()) {
        return;
    }
    {
        auto&& guard = rusty::deref_call(conn.on_error_.lock(), rusty::detail::__mdisp_unwrap{});
        if (rusty::detail::deref_if_pointer_like(guard)) {
            (rusty::detail::deref_if_pointer_like(guard))(ChannelError_Internal(), "epoll/poll signaled error");
        }
    }
    tcpconn_close(conn);
}
/*RUSTYCPP:GEN-END id=tcp_channel.handle_error*/

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// The outbound drain loop — partial-write accounting, EINTR retry
// (flag-restructured: the DSL has no continue), EAGAIN backpressure,
// hard-error cleanup — as DSL. Kernels: one send(2) with the raw
// pointer arithmetic, two erase-surgery helpers.
#if RUSTYCPP_RUST
fn tcpconn_drain_outbound_locked(conn: &TcpConnection, buf: &mut TcpOutBuf) -> ChannelError {
    let mut offset: usize = 0;
    let mut blocked = false;
    while !blocked && offset < buf.size() {
        let n = tcpconn_send_bytes(conn, buf, offset);
        if n > 0 {
            offset += n as usize;
        } else if n == 0 {
            // send returning 0 with bytes remaining = transport reset.
            return ChannelError_ConnectionReset();
        } else {
            let err: i32 = errno;
            if err == EAGAIN || err == EWOULDBLOCK {
                blocked = true;
            } else if err == EINTR {
                // retry — loop continues
            } else {
                // Hard error: drop what we couldn't send (dead anyway).
                tcpconn_drop_after_error(buf, offset);
                return tcpconn_errno_to_channel_error(err);
            }
        }
    }
    tcpconn_trim_sent(buf, offset);
    if offset == 0 && !buf.empty() {
        return ChannelError_WouldBlock();
    }
    ChannelError_None()
}
#endif
/*RUSTYCPP:GEN-BEGIN id=tcp_channel.drain version=1 rust_sha256=cae449183bab01ee3657973d3d74b941d0c81318c72ddd69ae75f1b2100ce612*/
ChannelError tcpconn_drain_outbound_locked(const TcpConnection& conn, TcpOutBuf& buf) {
    size_t offset = static_cast<size_t>(0);
    auto blocked = false;
    while (rusty::detail::rust_not(blocked) && (rusty::detail::deref_if_pointer_like(offset) < buf.size())) {
        const auto n = tcpconn_send_bytes(conn, buf, std::move(offset));
        if (rusty::detail::deref_if_pointer_like(n) > 0) {
            offset += static_cast<size_t>(n);
        } else if (rusty::detail::deref_if_pointer_like(n) == 0) {
            return ChannelError_ConnectionReset();
        } else {
            const int32_t err = errno;
            if ((rusty::detail::deref_if_pointer_like(err) == rusty::detail::deref_if_pointer_like(EAGAIN)) || (rusty::detail::deref_if_pointer_like(err) == rusty::detail::deref_if_pointer_like(EWOULDBLOCK))) {
                blocked = true;
            } else if (rusty::detail::deref_if_pointer_like(err) == rusty::detail::deref_if_pointer_like(EINTR)) {
            } else {
                tcpconn_drop_after_error(buf, std::move(offset));
                return tcpconn_errno_to_channel_error(std::move(err));
            }
        }
    }
    tcpconn_trim_sent(buf, std::move(offset));
    if ((rusty::detail::deref_if_pointer_like(offset) == static_cast<size_t>(0)) && rusty::detail::rust_not(buf.empty())) {
        return ChannelError_WouldBlock();
    }
    return ChannelError_None();
}
/*RUSTYCPP:GEN-END id=tcp_channel.drain*/

// @unsafe - send(2) with raw pointer arithmetic into the buffer.
int64_t tcpconn_send_bytes(const TcpConnection& conn, TcpOutBuf& buf, size_t offset) {
    return ::send(conn.fd_.as_raw_fd(), buf.data() + offset,
                  buf.size() - offset, MSG_NOSIGNAL);
}

// @unsafe - iterator surgery: drop the sent prefix.
void tcpconn_trim_sent(TcpOutBuf& buf, size_t offset) {
    if (offset == 0) return;
    if (offset == buf.size()) {
        buf.clear();
    } else {
        buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(offset));
    }
}

// @unsafe - hard-error cleanup: drop the sent prefix, or everything
// when nothing was sent (the connection is dead).
void tcpconn_drop_after_error(TcpOutBuf& buf, size_t offset) {
    if (offset > 0) {
        buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(offset));
    } else {
        buf.clear();
    }
}

// @unsafe - fires the on_closed callback (once) under the spinlock.
#if RUSTYCPP_RUST
fn tcpconn_deliver_on_closed_locked(conn: &TcpConnection, reason: ChannelError) {
    if conn.on_closed_fired_.get() {
        return;
    }
    conn.on_closed_fired_.set(true);
    let mut guard = conn.on_closed_.lock().unwrap();
    if *guard {
        (*guard)(reason);
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=tcp_channel.deliver_closed version=1 rust_sha256=2699de9cf591b5f113430eedd6805fb1b46f7d89bfa480ad8e10eaab84216aee*/
void tcpconn_deliver_on_closed_locked(const TcpConnection& conn, ChannelError reason) {
    if (conn.on_closed_fired_.get()) {
        return;
    }
    conn.on_closed_fired_.set(true);
    auto&& guard = rusty::deref_call(conn.on_closed_.lock(), rusty::detail::__mdisp_unwrap{});
    if (rusty::detail::deref_if_pointer_like(guard)) {
        (rusty::detail::deref_if_pointer_like(guard))(std::move(reason));
    }
}
/*RUSTYCPP:GEN-END id=tcp_channel.deliver_closed*/

// @safe - pure errno -> ChannelError mapping.
// Authored as inline Rust DSL (if-chain per the clientconn_map_system_error
// precedent — errno macros compare fine; cross-module enum variants use the
// generated factories).
#if RUSTYCPP_RUST
fn tcpconn_errno_to_channel_error(err: i32) -> ChannelError {
    if err == ECONNREFUSED { return ChannelError_ConnectionRefused(); }
    if err == ECONNRESET || err == EPIPE || err == ENOTCONN { return ChannelError_ConnectionReset(); }
    if err == ETIMEDOUT { return ChannelError_Timeout(); }
    if err == EADDRINUSE { return ChannelError_AddressInUse(); }
    if err == EADDRNOTAVAIL { return ChannelError_AddressInvalid(); }
    if err == EACCES || err == EPERM { return ChannelError_PermissionDenied(); }
    if err == EMFILE || err == ENFILE { return ChannelError_TooManyOpenFiles(); }
    ChannelError_Internal()
}
#endif
/*RUSTYCPP:GEN-BEGIN id=tcp_channel.10 version=1 rust_sha256=14460363b2f881fc0edbc724c1f4a60394bfa3fca2c987f95bc437933c5616b7*/
ChannelError tcpconn_errno_to_channel_error(int32_t err) {
    if (rusty::detail::deref_if_pointer_like(err) == rusty::detail::deref_if_pointer_like(ECONNREFUSED)) {
        return ChannelError_ConnectionRefused();
    }
    if (((rusty::detail::deref_if_pointer_like(err) == rusty::detail::deref_if_pointer_like(ECONNRESET)) || (rusty::detail::deref_if_pointer_like(err) == rusty::detail::deref_if_pointer_like(EPIPE))) || (rusty::detail::deref_if_pointer_like(err) == rusty::detail::deref_if_pointer_like(ENOTCONN))) {
        return ChannelError_ConnectionReset();
    }
    if (rusty::detail::deref_if_pointer_like(err) == rusty::detail::deref_if_pointer_like(ETIMEDOUT)) {
        return ChannelError_Timeout();
    }
    if (rusty::detail::deref_if_pointer_like(err) == rusty::detail::deref_if_pointer_like(EADDRINUSE)) {
        return ChannelError_AddressInUse();
    }
    if (rusty::detail::deref_if_pointer_like(err) == rusty::detail::deref_if_pointer_like(EADDRNOTAVAIL)) {
        return ChannelError_AddressInvalid();
    }
    if ((rusty::detail::deref_if_pointer_like(err) == rusty::detail::deref_if_pointer_like(EACCES)) || (rusty::detail::deref_if_pointer_like(err) == rusty::detail::deref_if_pointer_like(EPERM))) {
        return ChannelError_PermissionDenied();
    }
    if ((rusty::detail::deref_if_pointer_like(err) == rusty::detail::deref_if_pointer_like(EMFILE)) || (rusty::detail::deref_if_pointer_like(err) == rusty::detail::deref_if_pointer_like(ENFILE))) {
        return ChannelError_TooManyOpenFiles();
    }
    return ChannelError_Internal();
}
/*RUSTYCPP:GEN-END id=tcp_channel.10*/

// ===========================================================================
// TcpListener
// ===========================================================================

namespace {

// Set the FD non-blocking. Returns 0 on success, errno on failure.
// Authored in the DSL via expression-shaped unsafe{} libc calls (the
// threading.cpp pthread pattern): fcntl is variadic C but the call
// lowers textually; F_GETFL/O_NONBLOCK/errno are macros that survive
// the lowering as identifiers.
#if RUSTYCPP_RUST
fn set_nonblocking_fd(fd: i32) -> i32 {
    let flags = unsafe { fcntl(fd, F_GETFL, 0) };
    if flags < 0 {
        return errno;
    }
    let rc = unsafe { fcntl(fd, F_SETFL, flags | O_NONBLOCK) };
    if rc < 0 {
        return errno;
    }
    0
}
#endif
/*RUSTYCPP:GEN-BEGIN id=tcp_channel.set_nonblocking version=1 rust_sha256=d1f83d8b9266d28be729f9b54a4b1548e5c26dfdb102d561918a172f999fbbf7*/
int32_t set_nonblocking_fd(int32_t fd);

int32_t set_nonblocking_fd(int32_t fd) {
    const auto flags = fcntl(std::move(fd), F_GETFL, 0);
    if (rusty::detail::deref_if_pointer_like(flags) < 0) {
        return errno;
    }
    const auto rc = fcntl(std::move(fd), F_SETFL, rusty::detail::deref_if_pointer_like(flags) | rusty::detail::deref_if_pointer_like(O_NONBLOCK));
    if (rusty::detail::deref_if_pointer_like(rc) < 0) {
        return errno;
    }
    return static_cast<int32_t>(0);
}
/*RUSTYCPP:GEN-END id=tcp_channel.set_nonblocking*/

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

// Note: `tcplistener_listen` and `tcplistener_close` moved into the
// DSL impl block (listen / close methods) — the RefCell-wrapped fields
// replaced their setup-time const_cast writes.

// @unsafe - last-writer-wins callback store under the spinlock.

// @unsafe - same shape as set_on_accept.

// @safe - accept loop now delegates to `rusty::net::TcpListener::accept`
// (which encapsulates the ::accept syscall + peer-address marshalling).
// Per-accept setup (non-blocking flag, optional SO_NOSIGPIPE on macOS)
// runs through the new TcpStream wrapper; the only remaining inline
// `// @unsafe { }` here is the macOS-specific setsockopt(SO_NOSIGPIPE)
// — Linux uses MSG_NOSIGNAL on send() and doesn't need it.
// @unsafe - accept loop: rusty::net::TcpListener::accept + per-accept
// setsockopt(macOS) + TcpConnection construction + on_accept/on_error
// callback dispatch under the spinlock.
// One accept iteration's mechanics (foreign Result/pair/TcpStream
// interop, the APPLE SO_NOSIGPIPE split, the RAII unwrap into
// TcpConnection, poll-thread wiring, proxy construction) — a single
// classify-and-wrap kernel. Returns 1 accepted (out->proxy filled),
// 0 retriable/no-work, 2 nonblock-config failure (out->ch filled),
// -1 hard error (out->ch + out->msg filled).
struct AcceptStep {
    ChannelError ch = ChannelError::None;
    std::string msg;
    rusty::Option<ChannelConnectionProxy> proxy;
};
AcceptStep tcplistener_accept_step_new();
int32_t tcplistener_accept_step(const TcpListener& self, AcceptStep* out);
ChannelConnectionProxy tcplistener_take_proxy(AcceptStep* s);
bool tcplistener_is_bound(const TcpListener& self);

// The accept LOOP policy — progress accounting, retriable break,
// nonblock-failure skip, hard-error close, on_accept dispatch — as
// DSL over the step kernel.
#if RUSTYCPP_RUST
fn tcplistener_handle_read(lst: &TcpListener) -> bool {
    if lst.closed_.get() {
        return false;
    }
    if !tcplistener_is_bound(lst) {
        return false;
    }
    let mut any_progress = false;
    let mut accepting = true;
    while accepting {
        let mut step = tcplistener_accept_step_new();
        let rc = tcplistener_accept_step(lst, &mut step);
        if rc == 1 {
            any_progress = true;
            let mut guard = lst.on_accept_.lock().unwrap();
            if *guard {
                (*guard)(tcplistener_take_proxy(&mut step));
            }
        } else if rc == 0 {
            accepting = false;
        } else if rc == 2 {
            let mut guard = lst.on_error_.lock().unwrap();
            if *guard {
                (*guard)(step.ch, "accept: failed to set non-blocking");
            }
        } else {
            {
                let mut guard = lst.on_error_.lock().unwrap();
                if *guard {
                    (*guard)(step.ch, step.msg);
                }
            }
            lst.close();
            return any_progress;
        }
    }
    any_progress
}
#endif
/*RUSTYCPP:GEN-BEGIN id=tcp_channel.listener_read version=1 rust_sha256=080592ac95bc87701ff3d161936f2b63f6914e3fc7abfe2ded3a1edacda11472*/
bool tcplistener_handle_read(const TcpListener& lst) {
    if (lst.closed_.get()) {
        return false;
    }
    if (rusty::detail::rust_not(tcplistener_is_bound(lst))) {
        return false;
    }
    auto any_progress = false;
    auto accepting = true;
    while (accepting) {
        auto step = tcplistener_accept_step_new();
        const auto rc = tcplistener_accept_step(lst, &step);
        if (rusty::detail::deref_if_pointer_like(rc) == 1) {
            any_progress = true;
            auto&& guard = rusty::deref_call(lst.on_accept_.lock(), rusty::detail::__mdisp_unwrap{});
            if (rusty::detail::deref_if_pointer_like(guard)) {
                (rusty::detail::deref_if_pointer_like(guard))(tcplistener_take_proxy(&step));
            }
        } else if (rusty::detail::deref_if_pointer_like(rc) == 0) {
            accepting = false;
        } else if (rusty::detail::deref_if_pointer_like(rc) == 2) {
            auto&& guard = rusty::deref_call(lst.on_error_.lock(), rusty::detail::__mdisp_unwrap{});
            if (rusty::detail::deref_if_pointer_like(guard)) {
                (rusty::detail::deref_if_pointer_like(guard))(std::move([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.ch); }) { return (__r.ch); } else if constexpr (requires { (__r.ch_field); }) { return (__r.ch_field); } else if constexpr (requires { ((*__r).ch); }) { return ((*__r).ch); } else { return ((*__r).ch_field); } }(step)), "accept: failed to set non-blocking");
            }
        } else {
            {
                auto&& guard = rusty::deref_call(lst.on_error_.lock(), rusty::detail::__mdisp_unwrap{});
                if (rusty::detail::deref_if_pointer_like(guard)) {
                    (rusty::detail::deref_if_pointer_like(guard))(std::move([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.ch); }) { return (__r.ch); } else if constexpr (requires { (__r.ch_field); }) { return (__r.ch_field); } else if constexpr (requires { ((*__r).ch); }) { return ((*__r).ch); } else { return ((*__r).ch_field); } }(step)), std::move([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.msg); }) { return (__r.msg); } else if constexpr (requires { (__r.msg_field); }) { return (__r.msg_field); } else if constexpr (requires { ((*__r).msg); }) { return ((*__r).msg); } else { return ((*__r).msg_field); } }(step)));
                }
            }
            lst.close();
            return std::move(any_progress);
        }
    }
    return std::move(any_progress);
}
/*RUSTYCPP:GEN-END id=tcp_channel.listener_read*/

// @unsafe - value-init factory + move-out helper for the DSL.
AcceptStep tcplistener_accept_step_new() { return AcceptStep{}; }
ChannelConnectionProxy tcplistener_take_proxy(AcceptStep* s) { return s->proxy.take().unwrap(); }

// @unsafe - const method on the foreign rusty::net::TcpListener field.
#if RUSTYCPP_RUST
fn tcplistener_is_bound(lst: &TcpListener) -> bool {
    let g = lst.listener_.borrow();
    (*g).is_bound()
}
#endif
/*RUSTYCPP:GEN-BEGIN id=tcp_channel.22 version=1 rust_sha256=3780c6f38b82c0bdeacce69f7635eda06c2929f76c84927575a40ed86b6fe038*/
bool tcplistener_is_bound(const TcpListener& lst) {
    auto&& g = rusty::borrow(lst.listener_);
    return ((rusty::detail::deref_if_pointer_like(g))).is_bound();
}
/*RUSTYCPP:GEN-END id=tcp_channel.22*/

// @unsafe - accept(2) via rusty::net + the full accepted-socket wrap.
int32_t tcplistener_accept_step(const TcpListener& self, AcceptStep* out) {
    auto listener_guard = self.listener_.borrow();
    auto accept_result = (*listener_guard).accept();
    if (accept_result.is_err()) {
        auto err = accept_result.unwrap_err();
        auto kind = err.kind();
        // Retriable / "no work" — the DSL loop breaks without spinning.
        if (kind == rusty::io::Error::Kind::WouldBlock ||
            kind == rusty::io::Error::Kind::Interrupted ||
            kind == rusty::io::Error::Kind::ConnectionAborted) {
            return 0;
        }
        out->ch = io_kind_to_channel_error(kind);
        out->msg = err.to_string();
        return -1;
    }

    auto accepted = accept_result.unwrap();
    rusty::net::TcpStream stream = std::move(accepted.first);
    rusty::net::SocketAddrV4 peer_addr = accepted.second;

#ifdef __APPLE__
    // Prevent SIGPIPE termination on write() to closed sockets (Linux
    // uses MSG_NOSIGNAL on send(); macOS lacks that flag).
    {
        const int yes = 1;
        (void)::setsockopt(stream.as_owned_fd().as_raw_fd(),
                           SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
    }
#endif

    auto nonblock_result = stream.set_nonblocking(true);
    if (nonblock_result.is_err()) {
        out->ch = io_kind_to_channel_error(nonblock_result.unwrap_err().kind());
        // stream drops here, closing the accepted fd.
        return 2;
    }

    std::string peer_addr_str = rusty::net::socket_addr_v4_to_string(peer_addr);

    // Hand the accepted fd to TcpConnection (Phase D will pass the
    // TcpStream directly).
    int conn_fd = stream.into_owned_fd().into_raw_fd();
    auto conn = rusty::Arc<TcpConnection>::make(conn_fd, std::move(peer_addr_str));

    if (self.poll_thread_.is_some()) {
        {
            auto& mut_conn = const_cast<TcpConnection&>(*conn.get());
            mut_conn.set_poll_thread(self.poll_thread_.as_ref().unwrap().clone());
        }
        self.poll_thread_.as_ref().unwrap()->add_proxy(
            make_tcp_connection_pollable_proxy(conn.clone()));
    }

    out->proxy = rusty::Some(make_tcp_connection_channel_proxy(std::move(conn)));
    return 1;
}

// @unsafe - drives the on_error callback then closes the listener.
// @unsafe - fires on_error callback + drives tcplistener_close (::shutdown).
// Authored as inline Rust DSL — mirrors the tcpconn_handle_error twin.
#if RUSTYCPP_RUST
fn tcplistener_handle_error(listener: &TcpListener) {
    if listener.closed_.get() {
        return;
    }
    {
        let mut guard = listener.on_error_.lock().unwrap();
        if *guard {
            (*guard)(ChannelError_Internal(), "epoll/poll signaled error");
        }
    }
    listener.close();
}
#endif
/*RUSTYCPP:GEN-BEGIN id=tcp_channel.20 version=1 rust_sha256=56a1514c092e75e73ea033dfa0cc5fae8d59a6f9cbe64128ea5253e1cf6efe36*/
void tcplistener_handle_error(const TcpListener& listener) {
    if (listener.closed_.get()) {
        return;
    }
    {
        auto&& guard = rusty::deref_call(listener.on_error_.lock(), rusty::detail::__mdisp_unwrap{});
        if (rusty::detail::deref_if_pointer_like(guard)) {
            (rusty::detail::deref_if_pointer_like(guard))(ChannelError_Internal(), "epoll/poll signaled error");
        }
    }
    listener.close();
}
/*RUSTYCPP:GEN-END id=tcp_channel.20*/

// ===========================================================================
// TcpFactory
// ===========================================================================

// @safe - file-static helper, mirrors the old
// `TcpFactory::connect_errno_to_channel_error` static method. Plain
// errno → ChannelError mapping; no state.
namespace {
// Authored as inline Rust DSL (see tcpconn_errno_to_channel_error).
#if RUSTYCPP_RUST
fn connect_errno_to_channel_error(err: i32) -> ChannelError {
    if err == ECONNREFUSED { return ChannelError_ConnectionRefused(); }
    if err == ECONNRESET || err == EPIPE { return ChannelError_ConnectionReset(); }
    if err == ETIMEDOUT { return ChannelError_Timeout(); }
    if err == EHOSTUNREACH || err == ENETUNREACH || err == EADDRNOTAVAIL { return ChannelError_AddressInvalid(); }
    if err == EACCES || err == EPERM { return ChannelError_PermissionDenied(); }
    if err == EMFILE || err == ENFILE { return ChannelError_TooManyOpenFiles(); }
    ChannelError_Internal()
}
#endif
/*RUSTYCPP:GEN-BEGIN id=tcp_channel.11 version=1 rust_sha256=e5b31bbc4e56598bc47f47580ebea04da74387f91c0ec281f8b4070be4021a16*/
ChannelError connect_errno_to_channel_error(int32_t err) {
    if (rusty::detail::deref_if_pointer_like(err) == rusty::detail::deref_if_pointer_like(ECONNREFUSED)) {
        return ChannelError_ConnectionRefused();
    }
    if ((rusty::detail::deref_if_pointer_like(err) == rusty::detail::deref_if_pointer_like(ECONNRESET)) || (rusty::detail::deref_if_pointer_like(err) == rusty::detail::deref_if_pointer_like(EPIPE))) {
        return ChannelError_ConnectionReset();
    }
    if (rusty::detail::deref_if_pointer_like(err) == rusty::detail::deref_if_pointer_like(ETIMEDOUT)) {
        return ChannelError_Timeout();
    }
    if (((rusty::detail::deref_if_pointer_like(err) == rusty::detail::deref_if_pointer_like(EHOSTUNREACH)) || (rusty::detail::deref_if_pointer_like(err) == rusty::detail::deref_if_pointer_like(ENETUNREACH))) || (rusty::detail::deref_if_pointer_like(err) == rusty::detail::deref_if_pointer_like(EADDRNOTAVAIL))) {
        return ChannelError_AddressInvalid();
    }
    if ((rusty::detail::deref_if_pointer_like(err) == rusty::detail::deref_if_pointer_like(EACCES)) || (rusty::detail::deref_if_pointer_like(err) == rusty::detail::deref_if_pointer_like(EPERM))) {
        return ChannelError_PermissionDenied();
    }
    if ((rusty::detail::deref_if_pointer_like(err) == rusty::detail::deref_if_pointer_like(EMFILE)) || (rusty::detail::deref_if_pointer_like(err) == rusty::detail::deref_if_pointer_like(ENFILE))) {
        return ChannelError_TooManyOpenFiles();
    }
    return ChannelError_Internal();
}
/*RUSTYCPP:GEN-END id=tcp_channel.11*/
}  // namespace

// @unsafe - socket(2) / connect(2) / setsockopt(2) / fcntl(2) syscalls
// + reinterpret_cast<sockaddr*> on the sockaddr_in + PollThread::
// add_proxy is @unsafe + raw fd handling.
// KERNEL by verdict (TCP sweep): unlike the accept loop, connect has
// no loop/callback POLICY separable from its syscall sequence — the
// socket/nonblock/EINPROGRESS/select/SO_ERROR ladder with
// close-on-every-error IS the body. A DSL shell over one fat step
// kernel would be cosmetic fragmentation (anti-fragmentation rule).
// The socket/connect/select ladder lives in srpc_connect.c now (plain
// C, Goal-0 C demotion — this surface will never be Rust). This shim
// converts the C++ boundary types: SocketAddrV4 in, ChannelError out.
// C return contract: >=0 fd; -1 errno in *out_errno; -2 timeout;
// -3 self-connect guard (reported as ConnectionRefused so callers
// retry like a not-yet-up server).
extern "C" int32_t srpc_tcp_connect_socket(uint32_t addr_be, uint16_t port_be,
                                           int32_t timeout_ms, int32_t* out_errno);

int32_t tcp_factory_connect_socket(rusty::net::SocketAddrV4 peer,
                                   int32_t connect_timeout_ms,
                                   ChannelError* err_out) {
    sockaddr_in sa = rusty::net::sockaddr_in_from_socket_addr_v4(peer);
    int32_t err_no = 0;
    int32_t fd = srpc_tcp_connect_socket(sa.sin_addr.s_addr, sa.sin_port,
                                         connect_timeout_ms, &err_no);
    if (fd >= 0) {
        return fd;
    }
    if (fd == -2) {
        *err_out = ChannelError::Timeout;
    } else if (fd == -3) {
        *err_out = ChannelError::ConnectionRefused;
    } else {
        *err_out = connect_errno_to_channel_error(err_no);
    }
    return -1;
}

// Parse head + connection build/wiring as DSL over the syscall kernel.
// The old tail's `const_cast<TcpConnection&>(*conn.get())` becomes the
// get_mut mint window (the Arc is freshly minted and uniquely owned).
// The poll thread is wired into the connection BEFORE the channel
// proxy is handed back, so any user-thread `send_frame` can post
// `update_mode` actively (no lost-wake-up race against
// `pending_write_update_`); the channel proxy keeps one Arc, the
// pollable proxy another, so the connection survives until both
// layers release.
#if RUSTYCPP_RUST
fn tcp_factory_connect(fac: &TcpFactory, addr: std::string_view) -> ConnectResult {
    let parse_result = rusty::net::socket_addr_v4_from_str(addr);
    if parse_result.is_err() {
        return ConnectResult {
            connection: None,
            error: ChannelError_AddressInvalid(),
        };
    }
    let mut err: ChannelError = ChannelError_None();
    let fd = tcp_factory_connect_socket(parse_result.unwrap(),
                                        fac.connect_timeout_ms_,
                                        &raw mut err);
    if fd < 0i32 {
        return ConnectResult { connection: None, error: err };
    }

    let mut conn: Arc<TcpConnection> =
        Arc::<TcpConnection>::make(fd, format!("{}", addr));
    {
        let opt = conn.get_mut();
        let mut_conn: &mut TcpConnection = opt.unwrap();
        mut_conn.set_poll_thread(fac.poll_thread_.clone());
    }
    let pt: &Arc<PollThread> = &fac.poll_thread_;
    pt.add_proxy(make_tcp_connection_pollable_proxy(conn.clone()));

    ConnectResult {
        connection: Some(make_tcp_connection_channel_proxy(conn)),
        error: ChannelError_None(),
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=tcp_channel.25 version=1 rust_sha256=3891ea53fc0276750c765c9a18afb52425b59a5c5a31ab7790a7902ce2028bfe*/
ConnectResult tcp_factory_connect(const TcpFactory& fac, std::string_view addr) {
    auto parse_result = rusty::net::socket_addr_v4_from_str(std::move(addr));
    if (parse_result.is_err()) {
        return ConnectResult{.connection = rusty::None, .error = ChannelError_AddressInvalid()};
    }
    ChannelError err = ChannelError_None();
    auto fd = tcp_factory_connect_socket(parse_result.unwrap(), fac.connect_timeout_ms_, &err);
    if (rusty::detail::deref_if_pointer_like(fd) < static_cast<int32_t>(0)) {
        return ConnectResult{.connection = rusty::None, .error = std::move(err)};
    }
    rusty::Arc<TcpConnection> conn = rusty::Arc<TcpConnection>::make(std::move(fd), std::format("{}" , addr));
    {
        auto opt = conn.get_mut();
        TcpConnection& mut_conn = opt.unwrap();
        mut_conn.set_poll_thread(rusty::clone(fac.poll_thread_));
    }
    const rusty::Arc<PollThread>& pt = fac.poll_thread_;
    pt->add_proxy(make_tcp_connection_pollable_proxy(rusty::clone(conn)));
    return ConnectResult{.connection = rusty::Some(make_tcp_connection_channel_proxy(std::move(conn))), .error = ChannelError_None()};
}
/*RUSTYCPP:GEN-END id=tcp_channel.25*/

// Wire the listener up with the poll thread + a weak self-ref so it can
// self-register on a successful `listen(addr)` and so accepted
// connections are auto-registered too. get_mut, not const_cast: the Arc
// was just made and is still uniquely owned (same mint-window idiom as
// the server accept path).
#if RUSTYCPP_RUST
fn tcp_factory_make_listener(self_: &TcpFactory) -> Option<ChannelListenerProxy> {
    let mut listener: Arc<TcpListener> = Arc::<TcpListener>::make();
    {
        let opt = listener.get_mut();
        let mut_l: &mut TcpListener = opt.unwrap();
        mut_l.set_poll_thread(self_.poll_thread_.clone());
        mut_l.set_self_weak(rusty::sync::downgrade(listener));
    }
    Some(make_tcp_listener_channel_proxy(listener))
}
#endif
/*RUSTYCPP:GEN-BEGIN id=tcp_channel.30 version=1 rust_sha256=534aab19a50835b3e07abb6e136d04b133172a8362fe0ce919ab3634184c462c*/
rusty::Option<ChannelListenerProxy> tcp_factory_make_listener(const TcpFactory& self_) {
    rusty::Arc<TcpListener> listener = rusty::Arc<TcpListener>::make();
    {
        auto opt = listener.get_mut();
        TcpListener& mut_l = opt.unwrap();
        mut_l.set_poll_thread(rusty::clone(self_.poll_thread_));
        mut_l.set_self_weak(rusty::sync::downgrade(std::move(listener)));
    }
    return rusty::Option<ChannelListenerProxy>(make_tcp_listener_channel_proxy(std::move(listener)));
}
/*RUSTYCPP:GEN-END id=tcp_channel.30*/

}  // namespace rrr
