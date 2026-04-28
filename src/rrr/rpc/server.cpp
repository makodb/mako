
// import std; replacement — see <std_compat.hpp> for rationale.
#include <std_compat.hpp>

// @c-compat-added
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <rusty/arc.hpp>
#include <rusty/box.hpp>
#include <rusty/cell.hpp>
#include <rusty/option.hpp>
#include <rusty/vec.hpp>
#include <rusty/rusty.hpp>  // For rusty::Mutex, rusty::Condvar

// 5g3: socket-path system headers (`<unistd.h>`, `<sys/socket.h>`,
// `<netdb.h>`, `<sys/select.h>`, `<sys/un.h>`, `<sys/types.h>`,
// `<netinet/tcp.h>`, `<string.h>`, `<pthread.h>`,
// `<rusty/function.hpp>`, `<rusty/unsafe_cell.hpp>`,
// `<proxy/proxy.h>`, `<proxy/proxy_macros.h>`) removed — no
// `socket(2)` / `connect(2)` / `bind(2)` / `accept(2)` /
// `setsockopt(2)` / `getaddrinfo(3)` / `::close(fd)` /
// `pthread_*` calls remain in the RPC server; the channel layer's
// `TcpListener` / `TcpConnection` own those syscalls. The proxy
// macros come in transitively via `channel.hpp` for the channel-
// layer types referenced from the inline `reply<F>` template.
// `<errno.h>` is kept for the errno-shaped error-code constants
// (`EINVAL`, `ENOENT`) that the RPC dispatch path still emits.
#include <errno.h>





#include "server.hpp"


#include "../rrr.hpp"

// Note: External safety annotations for STL now in std_annotation.hpp (via rusty-cpp).
// Marshal, Log, SpinLock, PollThread, Reactor, Fiber, and rusty-cpp types
// now have in-place annotations in their respective headers.
// Note: std::atomic public API (load, store, etc.) is annotated in event.h
//
// @external: {
//   const_cast: [unsafe]
//   std::function::operator=: [safe]
//   std::list::push_back: [safe]
//   std::list::front: [safe]
//   std::list::pop_front: [safe]
//   std::list::empty: [safe]
//   std::list::begin: [safe]
//   std::list::end: [safe]
//   std::list::iterator::operator++: [safe]
//   std::list::iterator::operator*: [safe]
//   std::list::iterator::operator!=: [safe]
//   std::unordered_map::find: [safe]
//   std::unordered_map::end: [safe]
//   std::unordered_map::iterator::operator!=: [safe]
//   std::unordered_map::iterator::operator->: [safe]
//   std::set::find: [safe]
//   std::set::end: [safe]
//   std::set::insert: [safe]
//   std::set::iterator::operator!=: [safe]
//   std::vector::operator[]: [safe]
//   rusty::sync::Weak::Weak: [safe]
//   rrr::WeakServerConnection: [safe]
//   rrr::Fiber::CreateRun: [safe]
//   rrr::Reactor::get_reactor: [safe]
//   rrr::Reactor::Loop: [safe]
// }

using namespace std;

namespace rrr {


#ifdef RPC_STATISTICS

static const int g_stat_server_batching_size = 1000;
static int g_stat_server_batching[g_stat_server_batching_size];
static int g_stat_server_batching_idx;
static uint64_t g_stat_server_batching_report_time = 0;
static const uint64_t g_stat_server_batching_report_interval = 1000 * 1000 * 1000;

// @unsafe - Uses global mutable state (single-threaded context)
static void stat_server_batching(size_t batch) {
    g_stat_server_batching_idx = (g_stat_server_batching_idx + 1) % g_stat_server_batching_size;
    g_stat_server_batching[g_stat_server_batching_idx] = batch;
    uint64_t now = base::rdtsc();
    if (now - g_stat_server_batching_report_time > g_stat_server_batching_report_interval) {
        // do report
        int min = numeric_limits<int>::max();
        int max = 0;
        int sum_count = 0;
        int sum = 0;
        for (int i = 0; i < g_stat_server_batching_size; i++) {
            if (g_stat_server_batching[i] == 0) {
                continue;
            }
            if (g_stat_server_batching[i] > max) {
                max = g_stat_server_batching[i];
            }
            if (g_stat_server_batching[i] < min) {
                min = g_stat_server_batching[i];
            }
            sum += g_stat_server_batching[i];
            sum_count++;
            g_stat_server_batching[i] = 0;
        }
        double avg = double(sum) / sum_count;
        Log::info("* SERVER BATCHING: min=%d avg=%.1lf max=%d", min, avg, max);
        g_stat_server_batching_report_time = now;
    }
}

// rpc_id -> <count, cumulative>
static rusty::HashMap<i32, pair<Counter, Counter>> g_stat_rpc_counter;
static uint64_t g_stat_server_rpc_counting_report_time = 0;
static const uint64_t g_stat_server_rpc_counting_report_interval = 1000 * 1000 * 1000;

// @unsafe - Uses global mutable state (single-threaded context)
static void stat_server_rpc_counting(i32 rpc_id) {
    g_stat_rpc_counter[rpc_id].first.next();

    uint64_t now = base::rdtsc();
    if (now - g_stat_server_rpc_counting_report_time > g_stat_server_rpc_counting_report_interval) {
        // do report
        for (auto it: g_stat_rpc_counter) {
            i32 counted_rpc_id = it.first;
            i64 count = it.second.first.peek_next();
            it.second.first.reset();
            it.second.second.next(count);
            i64 cumulative = it.second.second.peek_next();
            Log::info("* RPC COUNT: id=%#08x count=%ld cumulative=%ld", counted_rpc_id, count, cumulative);
        }
        g_stat_server_rpc_counting_report_time = now;
    }
}

#endif // RPC_STATISTICS


// Static member definitions for missing RPC ID tracking
// SpinMutex wraps the unordered_set for thread-safe access
SpinMutex<rusty::HashSet<i32>> ServerConnection::rpc_id_missing_s{rusty::HashSet<i32>()};


// @safe - Initializes connection. 5g2: `socket_` field deleted;
// `socket` parameter is ignored (kept on the signature for source
// compatibility with existing call sites).
ServerConnection::ServerConnection(rusty::Arc<RpcServiceContext> ctx,
                                   int /*socket*/)
        : ctx_(std::move(ctx)), status_(CONNECTED) {
}

// @safe - Arc prevents premature destruction of RpcServiceContext
ServerConnection::~ServerConnection() {
    // Arc reference to RpcServiceContext is automatically released
}

// @unsafe - 5b/5c/5d: bind a channel proxy and flip the channel-mode latch.
//
// Mirrors `ClientConnection::bind_channel_direct`. After binding, the
// proxy serves as both the outbound dispatch sink (5b — `reply<F>(...)`
// calls `proxy->send_frame(...)`) and the inbound demux source (5c —
// installs `on_frame(...)` to call `decode_request_and_dispatch`).
// 5d also wires `on_closed` / `on_error` to transition the connection
// to CLOSED so the server's poll loop / accept-tracking notices the
// peer-side close (no orphan ServerConnection in the per-listener
// connection map).
//
// `weak_self_` MUST be initialized before calling this (the
// callbacks capture it; if the connection is destroyed, the upgrade
// fails and the callback short-circuits). The accept path in
// subsequent leaves (5e) wires `weak_self_` immediately after
// construction.
void ServerConnection::bind_channel(ChannelConnectionProxy proxy) {
    if (!proxy.has_value()) return;

    // Install callbacks BEFORE moving the proxy into the slot, so
    // the callbacks can capture a Weak<ServerConnection> without
    // holding the SpinMutex.
    WeakServerConnection weak_self;
    // @unsafe { Weak copy is currently treated as non-safe }
    { weak_self = weak_self_; }

    // @unsafe - lambda capture, channel proxy mutator
    proxy->set_on_frame([weak_self](const ChannelFrame& f) {
        auto sconn_opt = weak_self.upgrade();
        if (sconn_opt.is_none()) return;
        auto sconn = sconn_opt.unwrap();
        auto* mut_sconn = const_cast<ServerConnection*>(sconn.get());
        mut_sconn->decode_request_and_dispatch(f.payload, f.size);
    });
    // 5d: on_closed runs the existing close path so the connection
    // transitions to CLOSED. The channel-layer contract guarantees
    // on_closed fires exactly once; close() is itself idempotent
    // (status_ == CLOSED short-circuits).
    proxy->set_on_closed([weak_self](ChannelError /*reason*/) {
        auto sconn_opt = weak_self.upgrade();
        if (sconn_opt.is_none()) return;
        auto sconn = sconn_opt.unwrap();
        auto* mut_sconn = const_cast<ServerConnection*>(sconn.get());
        mut_sconn->close();
    });
    // 5d: on_error logs and force-closes. Per the channel-layer
    // contract, fatal errors are followed by on_closed, so the
    // close() here is also defensive — close() is idempotent.
    proxy->set_on_error([weak_self](ChannelError err,
                                    std::string_view message) {
        auto sconn_opt = weak_self.upgrade();
        if (sconn_opt.is_none()) return;
        auto sconn = sconn_opt.unwrap();
        // @unsafe - Log_warn formatting + std::string_view bridge
        Log_warn("rrr::ServerConnection: channel error %s: %.*s",
                 channel_error_to_string(err),
                 static_cast<int>(message.size()), message.data());
        auto* mut_sconn = const_cast<ServerConnection*>(sconn.get());
        mut_sconn->close();
    });

    // @unsafe { SpinMutex::lock + make_box + ChannelConnectionProxy move }
    {
        auto guard = channel_proxy_.lock().unwrap();
        *guard = rusty::Some(
            rusty::make_box<ChannelConnectionProxy>(std::move(proxy)));
    }
    channel_mode_.set(true);
}

// @unsafe - 5c: decode one channel-mode request frame and dispatch.
//
// Mirrors the per-packet body of `handle_read` minus the size-framed
// I/O loop: the channel layer has already stripped the 4-byte size
// prefix, so the body is `[xid:v64][rpc_id:i32][user-args]`.
void ServerConnection::decode_request_and_dispatch(
        const std::uint8_t* bytes, std::size_t size) {
    if (status_ == CLOSED) {
        return;
    }

    // Build a Request and copy the frame's bytes into its Marshal.
    // The channel-layer contract makes `bytes` valid only for the
    // duration of this callback, so we must copy before any code path
    // that may yield (e.g. `Fiber::create_run`).
    auto req_box = rusty::make_box<Request>();
    Request& req = *req_box;
    if (size > 0) {
        req.m.write(bytes, size);
    }

    // Header parse: xid + rpc_id. If the frame is malformed (less
    // than enough bytes for xid), drop it (no valid xid to reply
    // against). v64 is variable-length 1-8 bytes; an empty Marshal
    // means there's no xid.
    if (req.m.content_size() == 0) {
        Log_warn("rrr::ServerConnection: empty channel-mode request frame, "
                 "dropping");
        return;
    }
    v64 v_xid;
    req.m >> v_xid;
    req.xid = v_xid.get();
    req.attach_pending_guard(ctx_->pending_requests);

    if (req.m.content_size() < sizeof(i32)) {
        reply(req, EINVAL);
        return;
    }

    i32 rpc_id;
    req.m >> rpc_id;
    if (rpc_id == static_cast<i32>(kInternalHeartbeatRpcId)) {
        // @unsafe - std::atomic::load
        if (!ctx_->drop_heartbeat_replies->load(std::memory_order_acquire)) {
            reply(req, 0);
        }
        return;
    }

#ifdef RPC_STATISTICS
    stat_server_rpc_counting(rpc_id);
#endif // RPC_STATISTICS

    auto svc_index_opt = ctx_->rpc_to_service.get(rpc_id);
    if (svc_index_opt.is_none()) {
        bool surpress_warning = false;
        {
            auto guard = rpc_id_missing_s.lock().unwrap();
            if (!guard->contains(rpc_id)) {
                guard->insert(rpc_id);
            } else {
                surpress_warning = true;
            }
        }
        if (!surpress_warning) {
            Log_warn("rrr::ServerConnection: no handler for rpc_id = %d "
                     "(channel-mode dispatch)", rpc_id);
        }
        reply(req, ENOENT);
        return;
    }

    size_t svc_index = *svc_index_opt.unwrap();
    auto weak_this = weak_self_;
    if (ctx_->fast_rpc_ids.contains(rpc_id)) {
        // Fast inline dispatch — no fiber spawn.
        auto guard = ctx_->services[svc_index].borrow_mut();
        (*guard)->__dispatch__(rpc_id, std::move(req_box), weak_this);
    } else {
        // Slow path — spawn a fiber so the handler can yield (e.g.
        // for nested RPC calls). Capture an Arc<RpcServiceContext>
        // clone so the fiber stays valid even if the connection is
        // closed mid-flight.
        auto ctx = ctx_.clone();
        Fiber::create_run([ctx, svc_index, rpc_id,
                           req = std::move(req_box),
                           weak_this]() mutable {
            auto guard = ctx->services[svc_index].borrow_mut();
            (*guard)->__dispatch__(rpc_id, std::move(req), weak_this);
        }, __FILE__, __LINE__);
    }
}

// @unsafe - 5b: dispatch a reply-frame body through the bound proxy.
//
// Locks the SpinMutex briefly to extract the proxy pointer, then
// drops the guard so the actual `send_frame` happens without holding
// the lock (the proxy's `send_frame` is internally thread-safe per
// the channel-layer contract). Errors are observable via the
// proxy's installed `on_error` / `on_closed` callbacks; the return
// value is intentionally discarded — the RPC layer mirrors the
// legacy fd path's behavior of not surfacing send-side errors from
// `reply()`.
void ServerConnection::dispatch_response_frame_via_channel(
        const std::uint8_t* bytes, std::size_t size) const {
    ChannelConnectionProxy* proxy = nullptr;
    // @unsafe { SpinMutex::lock + raw pointer extraction }
    {
        auto guard = channel_proxy_.lock().unwrap();
        if (guard->is_none()) {
            Log_warn("rrr::ServerConnection::dispatch_response_frame_via_channel: "
                     "channel mode flipped on but proxy is unbound (race?). "
                     "Dropping reply.");
            return;
        }
        proxy = const_cast<ChannelConnectionProxy*>(
            guard->as_ref().unwrap().get());
    }
    ChannelFrame frame{bytes, size};
    // @unsafe - proxy method dispatch through pro::proxy
    (void)(*proxy)->send_frame(frame);
}

// @unsafe - Executes callback inline for API compatibility.
int ServerConnection::run_async(std::function<void()> f) {
  if (!f) {
    Log_warn("rrr::ServerConnection::run_async called with empty callback");
    return EINVAL;
  }
  f();
  return 0;
}

// @safe - 5g2: stubbed. The legacy `in_`/`out_` Marshal buffers are
// gone; channel mode buffers frames inside `TcpConnection`. Returns
// 0 for ABI compatibility with PollableProxy facade conformance.
size_t ServerConnection::content_size() {
    return 0;
}

// @unsafe - Explicit no-op for server connection API compatibility.
void ServerConnection::handle_free() {
    Log_warn("rrr::ServerConnection::handle_free() is a no-op on server connections");
}

// @safe - 5g2: stubbed. ServerConnection no longer implements the
// Pollable role — the channel layer's `TcpConnection` owns the fd
// and drives `handle_read`/`handle_write`/`handle_error` on its own
// pollable proxy. Inbound dispatch happens via the on_frame
// callback installed in `bind_channel(...)` (5c). This stub remains
// only for ABI compatibility (PollableProxy facade conformance);
// the body is unreachable from production paths.
bool ServerConnection::handle_read() {
    return false;
}

// @safe - 5g2: stubbed (Pollable facade ABI only). Channel mode's
// outbound writes go through `proxy->send_frame(...)` directly; no
// `out_` Marshal buffer to drain.
int ServerConnection::handle_write() {
    return PollMode::NO_CHANGE;
}

// @safe - Error handler. In channel mode, the bound channel proxy's
// `on_error` callback (wired in 5d) calls `close()` directly; this
// remains for legacy callers and as a defensive entry point.
void ServerConnection::handle_error() {
    this->close();
}

// @safe - Closes connection.
//
// 5g2: legacy `::close(socket_)` block deleted (the field is gone).
// Channel proxy close is the only fd-tearing-down path.
//
// The channel-layer proxy.close() is idempotent and safe under
// recursive entry: close() may be called from `on_closed` which 5d
// installs, and 5d's on_closed → close() → proxy.close() →
// (idempotent) on_closed re-fires without effect.
void ServerConnection::close() {
    if (status_ == CONNECTED) {
        status_ = CLOSED;
        Log_debug("server@%s close ServerConnection",
                  ctx_->addr.c_str());
        // Tear down the channel proxy. Idempotent per channel-
        // layer contract.
        // @unsafe { SpinMutex::lock + ChannelConnectionProxy method dispatch }
        {
            auto guard = channel_proxy_.lock().unwrap();
            if (guard->is_some()) {
                auto* proxy = const_cast<ChannelConnectionProxy*>(
                    guard->as_ref().unwrap().get());
                (*proxy)->close();
            }
        }
    }
}

// @safe - 5g2: stubbed. The channel layer's `TcpConnection` manages
// its own poll-mode state via `pending_write_update_` on the
// TcpConnection itself; this `ServerConnection` Pollable accessor
// is unreachable from production but kept for ABI compatibility.
int ServerConnection::poll_mode() const {
    return PollMode::READ;
}

// @unsafe - Executes callback inline for API compatibility.
int DeferredReply::run_async(std::function<void()> f) {
    if (!f) {
        Log_warn("rrr::DeferredReply::run_async called with empty callback");
        return EINVAL;
    }
    f();
    return 0;
}

// @safe - Constructs server with PollThread
// ctx_ starts as None; created in start() after all registrations
Server::Server(rusty::Option<rusty::Arc<PollThread>> poll_thread_worker /* =... */) {
    if (poll_thread_worker.is_none()) {  // Check if Option is None
        poll_thread_ = rusty::Some(PollThread::create());
    } else {
        poll_thread_ = std::move(poll_thread_worker);
    }

    // Generate unique instance ID for restart detection
    // Combines timestamp, random component, and process ID for uniqueness
    // @unsafe - std::random_device may use system entropy sources
    {
        auto now = std::chrono::steady_clock::now().time_since_epoch();
        uint64_t time_component = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());

        std::random_device rd;
        uint64_t random_component = static_cast<uint64_t>(rd()) << 32 |
                                    static_cast<uint64_t>(rd());

        uint64_t pid_component = static_cast<uint64_t>(getpid()) << 48;

        // Mix components with XOR for final ID
        instance_id_ = (time_component ^ random_component ^ pid_component)
            & static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
        if (instance_id_ == 0) {
            instance_id_ = 1;
        }

        Log_debug("Server: generated instance_id=%lu", instance_id_);
    }
}

// @safe - Destroys server and requests close for all connections
// Arc<RpcServiceContext> ensures services live until all connections are done
Server::~Server() noexcept {
    // 5g1: legacy `server_listener_` cleanup branch deleted (the
    // class is gone). Channel-mode listener cleanup follows.

    // 5e/5f: tear down the channel-mode listener (if bound). The
    // proxy's close() is idempotent; dropping the Box afterwards
    // releases the proxy's underlying TcpListener (or other backend),
    // which closes its listening fd via its destructor.
    //
    // Note: the listener may have just been registered with the
    // poll thread (the channel layer auto-registers via add_proxy
    // inside `listen()`). Calling close() directly from the user
    // thread races against the poll thread's pending CmdAddPollable
    // — by the time the poll thread reads `fd()`, it could already
    // be -1, tripping `Epoll::Add`'s fd>=0 verify. We schedule the
    // close on the poll thread via a OneTimeJob so commands are
    // processed in order (mirrors `Client::close`'s 4g3c3 fix). The
    // proxy holds an Arc<TcpListener> so the close() call inside
    // the job sees a live listener even though the original Box has
    // been moved into the lambda.
    if (channel_listener_.is_some()) {
        // Box is move-only and pro::proxy<F> is move-only, so wrap
        // in std::shared_ptr to make the lambda copyable (required
        // by std::function inside OneTimeJob).
        auto listener_sp = std::make_shared<rusty::Box<ChannelListenerProxy>>(
            std::move(channel_listener_).unwrap());
        auto close_job = rusty::Arc<OneTimeJob>::new_(OneTimeJob(
            [listener_sp]() mutable {
                auto* listener = listener_sp->get();
                (*listener)->close();
            }));
        auto close_job_base = rusty::Arc<Job>(close_job);
        poll_thread_.as_ref().unwrap()->add(std::move(close_job_base));
        channel_listener_ = rusty::None;
    }
    // 5f: actively close each accepted channel-mode ServerConnection
    // before dropping the Arcs. ServerConnection::close() drives the
    // bound channel proxy's close() which closes the underlying
    // TcpConnection's fd, so the peer (Client) sees EOF immediately
    // — without this active close, the TcpConnection's other Arc
    // (held by the poll thread's pollable proxy) would keep it alive
    // and the client would only notice on its next request attempt.
    // close() is idempotent (gated on status_ == CONNECTED), so
    // already-closed connections are no-ops.
    {
        auto guard = channel_sconns_.lock().unwrap();
        for (auto& sconn : *guard) {
            // @unsafe - const_cast through Arc::get for close()
            auto& mut_sconn = const_cast<ServerConnection&>(*sconn.get());
            mut_sconn.close();
        }
        // Drop the Arcs. The ChannelConnectionProxy stored inside
        // each ServerConnection drops along with the ServerConnection,
        // releasing the underlying TcpConnection's other refcount;
        // that fd is then closed by the poll-thread cleanup chain.
        guard->clear();
    }

    // No need to wait for connections - Arc<RpcServiceContext> ensures services
    // stay alive until the last ServerConnection drops its Arc reference.
    // Services are automatically cleaned up when last Arc is dropped.
    ctx_ = rusty::None;
}

// 5g1: legacy `ServerListener` implementation deleted. The
// channel layer's `TcpListener` handles bind/listen/accept; see
// `Server::start` (channel-mode listen path) and
// `src/rrr/rpc/tcp_channel.cpp`.

// @unsafe - Starts server listening (pointer dereference)
int Server::start(const char* bind_addr) {
  if (!bind_addr) {
    Log_error("rrr::Server::start: bind_addr is NULL!");
    return -1;
  }

  // Wrap each service in RefCell for interior mutability.
  rusty::Vec<rusty::RefCell<ServiceProxy>> wrapped_services;
  for (size_t i = 0; i < pending_services_.size(); ++i) {
    wrapped_services.push(rusty::RefCell<ServiceProxy>(std::move(pending_services_[i])));
  }

  // Create immutable RpcServiceContext from pending registration data
  std::string addr_str(bind_addr, strlen(bind_addr));
  ctx_ = rusty::Some(rusty::Arc<RpcServiceContext>::make(
      std::move(pending_rpc_to_service_),
      std::move(pending_fast_rpc_ids_),
      std::move(wrapped_services),
      addr_str,
      pending_requests_,
      drop_heartbeat_replies_,
      instance_id_));

  // Workstream K, server sub-leaf 5f — auto-install default TcpFactory.
  //
  // If the caller hasn't bound a factory via `set_channel_factory(...)`,
  // construct one wrapping a default `TcpFactory(poll_thread_)` so the
  // channel-mode path below is unconditional. Mirrors the client-side
  // post-4g4 pattern (`Client::connect` auto-installs a default TCP
  // factory). The legacy `ServerListener` socket path remains in
  // `server.cpp` for now but is unreachable from `Server::start` — 5g
  // deletes it.
  if (!is_channel_factory_bound()) {
    auto tcp_factory = rusty::Arc<TcpFactory>::make(
        poll_thread_.as_ref().unwrap().clone());
    set_channel_factory(make_tcp_factory_proxy(std::move(tcp_factory)));
  }

  // Workstream K, server sub-leaf 5e — channel-mode listen path.
  //
  // Routes `start(addr)` through `factory->make_listener() ->
  // listener.set_on_accept(...) -> listener->listen(addr)` instead
  // of the legacy `ServerListener` socket path. The on_accept
  // callback constructs a `ServerConnection` bound to the new
  // channel proxy (via 5b/5c/5d's `bind_channel(...)`) and parks
  // it in `channel_sconns_` so its lifetime is tied to the
  // `Server` (not the on_accept stack frame).
  if (is_channel_factory_bound()) {
    ChannelListenerProxy listener;
    // @unsafe { Box<ChannelFactoryProxy>::get + proxy method dispatch }
    {
      auto* factory = const_cast<ChannelFactoryProxy*>(
          channel_factory_.as_ref().unwrap().get());
      listener = (*factory)->make_listener();
    }
    if (!listener.has_value()) {
      Log_error("rrr::Server::start: factory->make_listener() returned a "
                "null proxy (factory backend=%s)",
                /*best-effort name*/ "unknown");
      ctx_ = rusty::None;
      return -1;
    }

    // Capture for the on_accept lambda. `this` outlives the listener
    // because Server owns `channel_listener_` (and `channel_sconns_`)
    // — destroying Server drops the listener which in turn waits for
    // any in-flight on_accept callback to complete (channel-layer
    // contract).
    Server* server_ptr = this;
    rusty::Arc<RpcServiceContext> ctx_arc = ctx_.as_ref().unwrap().clone();

    // @unsafe - lambda capture, channel proxy mutator
    listener->set_on_accept([server_ptr, ctx_arc](
                                ChannelConnectionProxy conn_proxy) {
      if (!conn_proxy.has_value()) return;
      auto sconn = rusty::Arc<ServerConnection>::make(
          ctx_arc.clone(), /*socket=*/-1);
      auto& mut_sconn = const_cast<ServerConnection&>(*sconn.get());
      // Wire `weak_self_` so bind_channel's installed callbacks can
      // upgrade to a strong ref.
      mut_sconn.install_self_weak_for_testing(rusty::sync::downgrade(sconn));
      mut_sconn.bind_channel(std::move(conn_proxy));
      // Park the Arc on the server so the on_frame / on_closed
      // callbacks (which only hold a Weak) keep observing a live
      // connection.
      // @unsafe { SpinMutex::lock + Vec::push }
      {
        auto guard = server_ptr->channel_sconns_.lock().unwrap();
        guard->push(std::move(sconn));
      }
    });
    listener->set_on_error([](ChannelError err, std::string_view msg) {
      // @unsafe - Log_warn formatting + std::string_view bridge
      Log_warn("rrr::Server: channel listener error %s: %.*s",
               channel_error_to_string(err),
               static_cast<int>(msg.size()), msg.data());
    });

    ChannelError listen_err = listener->listen(std::string_view(bind_addr));
    if (listen_err != ChannelError::None) {
      Log_error("rrr::Server::start: channel listener failed to bind %s: %s",
                bind_addr, channel_error_to_string(listen_err));
      ctx_ = rusty::None;
      return -1;
    }

    // Park the listener on the server so its lifetime matches Server's.
    // @unsafe { make_box + Option assignment }
    channel_listener_ = rusty::Some(
        rusty::make_box<ChannelListenerProxy>(std::move(listener)));
    return 0;
  }

  // 5g1: legacy `ServerListener` fallback deleted. The
  // `is_channel_factory_bound()` guard above is unconditionally true
  // post-5f (auto-installed default `TcpFactory`), so this fallthrough
  // is unreachable. We `verify(false)` defensively in case a future
  // refactor reintroduces a path that bypasses the auto-install.
  verify(false);
  return -1;
}

// @unsafe - Unregisters RPC mapping from pending map (must be called before start())
void Server::unreg(i32 rpc_id) {
    pending_rpc_to_service_.remove(rpc_id);
    pending_fast_rpc_ids_.remove(rpc_id);
}

// @unsafe - Signals shutdown to waiting threads
void Server::do_shutdown() {
    Log_debug("Server::do_shutdown");
    {
        auto guard = shutdown_state_.lock().unwrap();
        guard->shutdown = true;
    }
    shutdown_cond_.notify_all();
}

// @unsafe - Blocks until shutdown is signaled
void Server::wait_for_shutdown() {
    Log_debug("Server::wait_for_shutdown");
    auto guard = shutdown_state_.lock().unwrap();
    guard = shutdown_cond_.wait_while(std::move(guard),
        [](ShutdownState& s) { return !s.shutdown; }).unwrap();
    Log_debug("Server::wait_for_shutdown - done");
}

// === Graceful Shutdown Implementation ===

// @unsafe - Thread-safe hook registration
void Server::add_shutdown_hook(ShutdownHook hook) {
    auto guard = shutdown_hooks_.lock().unwrap();
    guard->push(std::move(hook));
}

// @unsafe - Calls PollThread::request_close
void Server::stop_accepting() {
    if (shutdown_phase_.get() != ShutdownPhase::RUNNING) {
        Log_debug("Server::stop_accepting: already in phase %s",
                  shutdown_phase_to_string(shutdown_phase_.get()));
        return;
    }

    Log_info("Server::stop_accepting: transitioning to STOP_ACCEPTING");
    shutdown_phase_.set(ShutdownPhase::STOP_ACCEPTING);

    // 5g1: legacy `server_listener_` close branch deleted (the
    // class is gone). Channel-mode listener close follows.

    // 5e: close the channel-mode listener if bound. The proxy's
    // close() is idempotent and refuses further accepts; existing
    // accepted connections in `channel_sconns_` are unaffected (they
    // continue to serve in-flight requests until drained / shut down).
    if (channel_listener_.is_some()) {
        // @unsafe { Box::get + ChannelListenerProxy method dispatch }
        {
            auto* listener = const_cast<ChannelListenerProxy*>(
                channel_listener_.as_ref().unwrap().get());
            (*listener)->close();
        }
        Log_info("Server::stop_accepting: channel listener closed, "
                 "no longer accepting connections");
    }
    // Note: stop_accepting() is typically called well after the
    // listener has been registered with the poll thread (via the
    // channel-layer's auto-register in TcpListener::listen), so the
    // direct `proxy->close()` above doesn't race with a pending
    // `CmdAddPollable`. ~Server's teardown takes a more defensive
    // approach (scheduling the close via a OneTimeJob) because in
    // tests the listener may have just been registered.
}

// @unsafe - Uses std::atomic::load
bool Server::drain(uint64_t timeout_ms) {
    auto current_phase = shutdown_phase_.get();
    if (current_phase != ShutdownPhase::RUNNING &&
        current_phase != ShutdownPhase::STOP_ACCEPTING) {
        Log_debug("Server::drain: already in phase %s",
                  shutdown_phase_to_string(current_phase));
        return pending_requests_->load(std::memory_order_relaxed) == 0;
    }

    Log_info("Server::drain: transitioning to DRAINING, pending=%d",
             pending_requests_->load(std::memory_order_relaxed));
    shutdown_phase_.set(ShutdownPhase::DRAINING);

    // Wait for pending requests with timeout
    // @unsafe - uses std::chrono
    {
        auto start = std::chrono::steady_clock::now();
        auto timeout = std::chrono::milliseconds(timeout_ms);

        while (pending_requests_->load(std::memory_order_relaxed) > 0) {
            auto elapsed = std::chrono::steady_clock::now() - start;
            if (elapsed >= timeout) {
                Log_warn("Server::drain: timeout after %lu ms, pending=%d",
                         timeout_ms, pending_requests_->load(std::memory_order_relaxed));
                return false;
            }

            // Brief sleep to avoid busy-waiting
            // @unsafe - usleep syscall
            usleep(1000);  // 1ms
        }
    }

    Log_info("Server::drain: completed, all requests drained");
    return true;
}

// @unsafe - Calls stop_accepting() and drain() which are unsafe
void Server::graceful_shutdown(uint64_t drain_timeout_ms) {
    Log_info("Server::graceful_shutdown: starting graceful shutdown");

    // Phase 1: Stop accepting new connections
    stop_accepting();  // @unsafe

    // Phase 2: Drain existing requests
    bool drained = drain(drain_timeout_ms);  // @unsafe
    if (!drained) {
        Log_warn("Server::graceful_shutdown: drain timed out, proceeding with shutdown");
    }

    // Phase 3: Execute shutdown hooks
    Log_info("Server::graceful_shutdown: transitioning to CLOSING, executing hooks");
    shutdown_phase_.set(ShutdownPhase::CLOSING);

    {
        auto guard = shutdown_hooks_.lock().unwrap();
        for (auto& hook : *guard) {
            // @unsafe - callback execution
            {
                try {
                    hook();
                } catch (const std::exception& e) {
                    Log_error("Server::graceful_shutdown: hook threw exception: %s", e.what());
                } catch (...) {
                    Log_error("Server::graceful_shutdown: hook threw unknown exception");
                }
            }
        }
    }

    // Phase 4: Close all connections (destructor handles this)
    // Signal shutdown to any waiting threads
    do_shutdown();

    Log_info("Server::graceful_shutdown: transitioning to STOPPED");
    shutdown_phase_.set(ShutdownPhase::STOPPED);
}

// @safe - 5g1: re-implemented atop the channel-layer's
// `ChannelListenerProxy::local_address()` (which `TcpListener` fills
// in via `getsockname` after a successful `bind(2)`). The returned
// string is `host:port`; we parse out the port suffix.
int Server::get_bound_port() const {
    if (channel_listener_.is_none()) {
        return -1;
    }
    std::string local;
    // @unsafe { Box<ChannelListenerProxy>::get + proxy method dispatch }
    {
        auto* listener = const_cast<ChannelListenerProxy*>(
            channel_listener_.as_ref().unwrap().get());
        local = (*listener)->local_address();
    }
    auto colon = local.rfind(':');
    if (colon == std::string::npos) {
        Log_error("Server::get_bound_port: malformed local_address %s",
                  local.c_str());
        return -1;
    }
    try {
        int port = std::stoi(local.substr(colon + 1));
        return port;
    } catch (const std::exception&) {
        Log_error("Server::get_bound_port: failed to parse port from %s",
                  local.c_str());
        return -1;
    }
}

} // namespace rrr
