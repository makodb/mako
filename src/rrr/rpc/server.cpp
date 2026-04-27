
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
#include <rusty/function.hpp>
#include <rusty/option.hpp>
#include <rusty/unsafe_cell.hpp>
#include <rusty/vec.hpp>
#include <rusty/rusty.hpp>  // For rusty::Mutex, rusty::Condvar
#include <pthread.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netdb.h>
#include <proxy/proxy.h>
#include <proxy/proxy_macros.h>


#include <sys/select.h>
#include <sys/un.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <netinet/tcp.h>





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


// @safe - Initializes connection
ServerConnection::ServerConnection(rusty::Arc<RpcServiceContext> ctx, int socket)
        : ctx_(std::move(ctx)), socket_(socket), status_(CONNECTED) {
}

// @safe - Arc prevents premature destruction of RpcServiceContext
ServerConnection::~ServerConnection() {
    // Arc reference to RpcServiceContext is automatically released
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

// @unsafe - Returns total buffered bytes owned by this connection.
size_t ServerConnection::content_size() {
    auto out_guard = out_.lock().unwrap();
    return in_.content_size() + out_guard->content_size();
}

// @unsafe - Explicit no-op for server connection API compatibility.
void ServerConnection::handle_free() {
    Log_warn("rrr::ServerConnection::handle_free() is a no-op on server connections");
}

// @unsafe - Reads requests from socket and dispatches to handlers
// Memory-safe: Uses Box for request ownership, virtual dispatch for handlers.
bool ServerConnection::handle_read() {
    if (status_ == CLOSED) {
        return false;
    }

    // CRITICAL FIX: With edge-triggered epoll (EPOLLET), we must:
    // 1. Drain all data from the socket
    // 2. Process ALL complete packets in the buffer
    // The old code only processed ONE packet per handle_read() call,
    // causing hangs when multiple requests arrive together.

    size_t bytes_read = in_.read_from_fd(socket_);
    if (bytes_read == 0 && in_.content_size() < sizeof(i32)) {
        return false;
    }

    std::list<rusty::Box<Request>> complete_requests;

    // Parse ALL complete packets from the buffer
    // Pattern: add to list first, then fill via reference to avoid move tracking issues
    for (;;) {
        i32 packet_size;
        int n_peek = in_.peek(packet_size);

        // Check exit condition first (inverted logic)
        if (!(n_peek == sizeof(i32) && in_.content_size() >= packet_size + sizeof(i32))) {
            break;
        }

        verify(in_.read(packet_size) == sizeof(i32));

        // Add to list first, then fill via reference (avoids move tracking issues)
        complete_requests.push_back(rusty::make_box<Request>());
        Request& req = *complete_requests.back();
        verify(req.m.read_from_marshal(in_, packet_size) == (size_t) packet_size);

        v64 v_xid;
        req.m >> v_xid;
        req.xid = v_xid.get();
    }

#ifdef RPC_STATISTICS
    stat_server_batching(complete_requests.size());
#endif // RPC_STATISTICS

    // Process each request
    while (!complete_requests.empty()) {
        // @unsafe - std::list::front() and pop_front()
        rusty::Box<Request> req = [&complete_requests]() {
            auto r = std::move(complete_requests.front());
            complete_requests.pop_front();
            return r;
        }();
        req->attach_pending_guard(ctx_->pending_requests);

        if (req->m.content_size() < sizeof(i32)) {
            reply(*req, EINVAL);
        } else {
            i32 rpc_id;
            req->m >> rpc_id;
            if (rpc_id == static_cast<i32>(kInternalHeartbeatRpcId)) {
                // Internal liveness probe from client heartbeat loop.
                if (!ctx_->drop_heartbeat_replies->load(std::memory_order_acquire)) {
                    reply(*req, 0);
                }
                continue;
            }

#ifdef RPC_STATISTICS
            stat_server_rpc_counting(rpc_id);
#endif // RPC_STATISTICS

            auto svc_index_opt = ctx_->rpc_to_service.get(rpc_id);
            if (svc_index_opt.is_none()) {
                // Handler not found - track missing RPC IDs
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
                    Log_warn("rrr::ServerConnection: no handler for rpc_id = %d", rpc_id);
                }
                reply(*req, ENOENT);
            } else {
                // Service found - dispatch via proxy facade using RefCell
                size_t svc_index = *svc_index_opt.unwrap();
                auto weak_this = weak_self_;
                if (ctx_->fast_rpc_ids.contains(rpc_id)) {
                    auto guard = ctx_->services[svc_index].borrow_mut();
                    (*guard)->__dispatch__(rpc_id, std::move(req), weak_this);
                } else {
                    auto ctx = ctx_.clone();  // Clone Arc for the fiber
                    Fiber::create_run([ctx, svc_index, rpc_id, req = std::move(req), weak_this]() mutable {
                        // Borrow inside fiber - guard released when lambda exits
                        // (*guard) dereferences RefMut to get Box<Service>&
                        // (*guard)-> calls Box::operator-> to get Service*
                        auto guard = ctx->services[svc_index].borrow_mut();
                        (*guard)->__dispatch__(rpc_id, std::move(req), weak_this);
                    }, __FILE__, __LINE__);
                }
            }
        }
    }

    Reactor::get_reactor()->loop();

    return false;
}

// @unsafe - Writes buffered data to socket, protected by SpinMutex
int ServerConnection::handle_write() {
    if (status_ == CLOSED) {
        return PollMode::NO_CHANGE;
    }

    int result = PollMode::NO_CHANGE;
    auto guard = out_.lock().unwrap();
    guard->write_to_fd(socket_);
    if (guard->empty()) {
        // Return READ-only mode - PollThreadWorker will update epoll
        result = PollMode::READ;
    }
    // Guard auto-unlocks here
    return result;
}

// @safe - Error handler
void ServerConnection::handle_error() {
    this->close();
}

// @safe - Closes connection
// SAFETY: Internal @unsafe block for system calls and pointer operations
void ServerConnection::close() {
    if (status_ == CONNECTED) {
        status_ = CLOSED;
        // @unsafe - system call
        {
            ::close(socket_);
            Log_debug("server@%s close ServerConnection at fd=%d", ctx_->addr.c_str(), socket_);
        }
        // Note: We don't remove fd from Server's sconn_fds_ list.
        // At shutdown, Server will request_close on all fds (closed ones are no-ops).
    }
}

// @unsafe - Returns poll mode based on output buffer, protected by SpinMutex
int ServerConnection::poll_mode() const {
    int mode = PollMode::READ;
    auto guard = out_.lock().unwrap();
    if (!guard->empty()) {
        mode |= PollMode::WRITE;
    }
    // Guard auto-unlocks here
    return mode;
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
    // Request close for server listener and all its connections via poll thread
    if (server_listener_.is_some()) {
        auto& listener = server_listener_.as_ref().unwrap();

        // Request close for all connections accepted by this listener
        {
            auto guard = listener->sconn_fds_.lock().unwrap();
            for (int fd : *guard) {
                poll_thread_.as_ref().unwrap()->request_close(fd);
            }
            // Note: Some fds may already be closed, request_close on closed fds is a no-op.
        }

        // Request close for the listener itself
        poll_thread_.as_ref().unwrap()->request_close(listener->fd());
        server_listener_ = rusty::None;  // Reset to None
    }

    // No need to wait for connections - Arc<RpcServiceContext> ensures services
    // stay alive until the last ServerConnection drops its Arc reference.
    // Services are automatically cleaned up when last Arc is dropped.
    ctx_ = rusty::None;
}

// @safe - Accepts new client connections
// SAFETY: All unsafe operations wrapped in @unsafe blocks
size_t ServerListener::content_size() {
  return 0;
}

int ServerListener::handle_write() {
  static std::atomic<bool> warned{false};
  bool expected = false;
  if (warned.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
    Log_warn("rrr::ServerListener::handle_write() is unsupported for READ-only listener");
  }
  return PollMode::NO_CHANGE;
}

void ServerListener::handle_error() {
  static std::atomic<bool> warned{false};
  bool expected = false;
  if (warned.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
    Log_warn("rrr::ServerListener::handle_error() closing listener after poll error");
  }
  close();
}

bool ServerListener::handle_read() {
//  fd_set fds;
//  FD_ZERO(&fds);
//  FD_SET(server_sock_, &fds);

  while (true) {
    int clnt_socket = -1;  // Initialize to invalid fd
    // @unsafe - syscall with raw pointers
    {
#ifdef USE_IPC
      struct sockaddr_un fsaun;
      uint32_t from_len;
      clnt_socket = ::accept(server_sock_, (struct sockaddr*)&fsaun, &from_len);
#else
      clnt_socket = ::accept(server_sock_, p_svr_addr_->ai_addr, &p_svr_addr_->ai_addrlen);
#endif
    }
    if (clnt_socket >= 0) {
      Log_debug("server@%s got new client, fd=%d", this->addr_.c_str(), clnt_socket);
      // @unsafe - set_nonblocking
      {
        if (set_nonblocking(clnt_socket, true) != 0) {
          Log_error("server@%s failed to set nonblocking on accepted fd=%d: errno=%d (%s)",
                    this->addr_.c_str(), clnt_socket, errno, strerror(errno));
          ::close(clnt_socket);
          continue;
        }
      }

      auto sconn = rusty::Arc<ServerConnection>::make(ctx_.clone(), clnt_socket);
      // @unsafe - const_cast to initialize weak_self_ (safe: we just created this object)
      { const_cast<ServerConnection&>(*sconn).weak_self_ = sconn; }
      {
          // Track fd for shutdown cleanup (Server reads this list in destructor)
          auto guard = sconn_fds_.lock().unwrap();
          guard->push(clnt_socket);
      }
      auto poll_proxy = make_pollable_proxy_from_typed_arc(sconn);
      PollThreadWorker::add_pollable_from_current_thread(std::move(poll_proxy));
    } else {
      break;
    }
  }
  return false;
}

// @safe - Closes server socket
// SAFETY: Internal @unsafe block for ::close() system call
void ServerListener::close() {
  if (server_sock_ >= 0) {
    // @unsafe - system call
    { ::close(server_sock_); }
    server_sock_ = -1;
  }
}

// @safe - Creates listener socket and binds to address
// All socket operations are marked safe via external annotations
ServerListener::ServerListener(rusty::Arc<RpcServiceContext> ctx, string addr)
    : ctx_(std::move(ctx)), addr_(addr) {
  size_t idx = addr.find(":");
  if (idx == string::npos) {
    Log_error("rrr::Server: bad bind address: %s", addr.c_str());
    server_sock_ = -1;
    return;
  }
  string host = addr.substr(0, idx);
  string port = addr.substr(idx + 1);

#ifdef USE_IPC
  struct sockaddr_un saun;
  if ((server_sock_ = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
    Log_error("rrr::Server: ipc socket() failed for %s: errno=%d (%s)",
              addr.c_str(), errno, strerror(errno));
    server_sock_ = -1;
    return;
  }
  saun.sun_family = AF_UNIX;
  string ipc_addr = "rsock" + port;
  strcpy(saun.sun_path, ipc_addr.data());
  auto len = sizeof(saun.sun_family) + strlen(saun.sun_path)+1;
  ::unlink(ipc_addr.data());
  if (::bind(server_sock_, (struct sockaddr*)&saun, len) != 0) {
    Log_error("rrr::Server: ipc bind() failed for %s: errno=%d (%s)",
              addr.c_str(), errno, strerror(errno));
    ::close(server_sock_);
    server_sock_ = -1;
    return;
  }

#else
  struct addrinfo hints;
  memset(&hints, 0, sizeof(struct addrinfo));
  hints.ai_family = AF_INET; // ipv4
  hints.ai_socktype = SOCK_STREAM; // tcp
  hints.ai_flags = AI_PASSIVE; // server side

  // Use AddrInfo RAII wrapper
  auto addr_result = AddrInfo::resolve(
      (host == "0.0.0.0") ? nullptr : host.c_str(),
      port.c_str(),
      &hints);
  if (addr_result.is_err()) {
    Log_error("rrr::Server: getaddrinfo(): %s", gai_strerror(addr_result.unwrap_err()));
    server_sock_ = -1;
    return;
  }
  gai_result_ = addr_result.unwrap();

  struct addrinfo* rp = nullptr;
  for (rp = gai_result_.get(); rp != nullptr; rp = rp->ai_next) {
    server_sock_ = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (server_sock_ == -1) {
      continue;
    }

    const int yes = 1;
    // Set SO_REUSEADDR to allow binding to ports in TIME_WAIT state
    if (setsockopt(server_sock_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) != 0) {
      Log_error("SO_REUSEADDR failed for %s: errno=%d (%s)", addr.c_str(), errno, strerror(errno));
      ::close(server_sock_);
      server_sock_ = -1;
      continue;  // Try next address
    }

#ifdef SO_REUSEPORT
    // Set SO_REUSEPORT to allow multiple binds (helps with rapid restart)
    if (setsockopt(server_sock_, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes)) != 0) {
      Log_debug("SO_REUSEPORT failed for %s: errno=%d (%s) - continuing anyway", addr.c_str(), errno, strerror(errno));
      // Not fatal - continue without SO_REUSEPORT
    }
#endif

    if (setsockopt(server_sock_, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes)) != 0) {
      Log_error("TCP_NODELAY failed for %s: errno=%d (%s)", addr.c_str(), errno, strerror(errno));
      ::close(server_sock_);
      server_sock_ = -1;
      continue;  // Try next address
    }

    if (::bind(server_sock_, rp->ai_addr, rp->ai_addrlen) == 0) {
      break;  // Successfully bound
    } else {
      Log_error("port bind error for %s:%s, errno: %d (%s)", host.c_str(), port.c_str(), errno, strerror(errno));
      ::close(server_sock_);
      server_sock_ = -1;
      // Continue to next address in the list
    }
  }

  if (rp == nullptr) {
    // Failed to bind to any address
    Log_error("rrr::Server: failed to bind to %s:%s after trying all addresses", host.c_str(), port.c_str());
    Log_error("rrr::Server: This is likely because the port is already in use by another process");
    Log_error("rrr::Server: Please check: sudo lsof -i :%s or sudo ss -tulpn | grep %s", port.c_str(), port.c_str());
    // gai_result_ RAII wrapper handles freeaddrinfo automatically
    // Mark socket as invalid so start() can detect failure
    server_sock_ = -1;
    return;  // Caller should check server_sock_ for failure
  } else {
    // gai_result_ already stores the AddrInfo, just save pointer into the list
    p_svr_addr_ = rp;
  }
#endif

  // about backlog: http://www.linuxjournal.com/files/linuxjournal.com/linuxjournal/articles/023/2333/2333s2.html
  const int backlog = SOMAXCONN;
  int listen_ret = listen(server_sock_, backlog);
  if (listen_ret != 0) {
    Log_error("rrr::Server: listen() failed on %s: errno=%d (%s)", addr.c_str(), errno, strerror(errno));
    ::close(server_sock_);
    server_sock_ = -1;
    return;
  }

  int nonblock_ret = set_nonblocking(server_sock_, true);
  if (nonblock_ret != 0) {
    Log_error("rrr::Server: set_nonblocking() failed on %s: errno=%d (%s)", addr.c_str(), errno, strerror(errno));
    ::close(server_sock_);
    server_sock_ = -1;
    return;
  }

  Log_debug("rrr::Server: started on %s", addr.c_str());
}

// @unsafe - Starts server listening (pointer dereference: server_listener_->)
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

  server_listener_ = rusty::Some(rusty::Arc<ServerListener>::make(
      ctx_.as_ref().unwrap().clone(), addr_str));

  // Check if listener was created successfully (binding may have failed)
  if (server_listener_.as_ref().unwrap()->server_sock_ < 0) {
    Log_error("rrr::Server::start: failed to bind to %s", bind_addr);
    server_listener_ = rusty::None;
    ctx_ = rusty::None;
    return -1;
  }

  auto listener_proxy = make_pollable_proxy_from_typed_arc(server_listener_.as_ref().unwrap().clone());
  poll_thread_.as_ref().unwrap()->add_proxy(std::move(listener_proxy));
  return 0;
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

    // Close the server listener to stop accepting new connections
    if (server_listener_.is_some()) {
        auto& listener = server_listener_.as_ref().unwrap();
        poll_thread_.as_ref().unwrap()->request_close(listener->fd());
        Log_info("Server::stop_accepting: listener closed, no longer accepting connections");
    }
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

// @unsafe - Calls getsockname
int Server::get_bound_port() const {
    if (server_listener_.is_none()) {
        return -1;
    }

    int sock = server_listener_.as_ref().unwrap()->server_sock_;
    if (sock < 0) {
        return -1;
    }

    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    memset(&addr, 0, sizeof(addr));

    if (getsockname(sock, (struct sockaddr*)&addr, &addrlen) != 0) {
        Log_error("Server::get_bound_port: getsockname failed, errno=%d (%s)",
                  errno, strerror(errno));
        return -1;
    }

    return ntohs(addr.sin_port);
}

} // namespace rrr
