#include <string>
#include <sstream>
#include <memory>
#include <cerrno>

#include <sys/select.h>
#include <sys/un.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <netinet/tcp.h>

#include "reactor/coroutine.h"
#include "reactor/reactor.h"
#include "server.hpp"
#include "utils.hpp"

// Note: External safety annotations for STL now in std_annotation.hpp (via rusty-cpp).
// Marshal, Log, SpinLock, PollThread, Reactor, Coroutine, and rusty-cpp types
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
//   rrr::Coroutine::CreateRun: [safe]
//   rrr::Reactor::GetReactor: [safe]
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
static unordered_map<i32, pair<Counter, Counter>> g_stat_rpc_counter;
static uint64_t g_stat_server_rpc_counting_report_time = 0;
static const uint64_t g_stat_server_rpc_counting_report_interval = 1000 * 1000 * 1000;

// @unsafe - Uses global mutable state (single-threaded context)
static void stat_server_rpc_counting(i32 rpc_id) {
    g_stat_rpc_counter[rpc_id].first.next();

    uint64_t now = base::rdtsc();
    if (now - g_stat_server_rpc_counting_report_time > g_stat_server_rpc_counting_report_interval) {
        // do report
        for (auto& it: g_stat_rpc_counter) {
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
SpinMutex<std::unordered_set<i32>> ServerConnection::rpc_id_missing_s{std::unordered_set<i32>()};


// @safe - Initializes connection
ServerConnection::ServerConnection(rusty::Arc<RpcServiceContext> ctx, int socket)
        : ctx_(std::move(ctx)), socket_(socket), status_(CONNECTED) {
}

// @safe - Arc prevents premature destruction of RpcServiceContext
ServerConnection::~ServerConnection() {
    // Arc reference to RpcServiceContext is automatically released
}

// get_shared() is now inherited from Pollable base class

// @safe - Delegates to thread pool
int ServerConnection::run_async(const std::function<void()>& f) {
  // run_async should not be used - process RPC synchronously
  // Call f() directly instead where this was being used
  verify(0); // This should never be called
  return 0;
}

// @safe - Reads requests from socket and dispatches to handlers
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

        if (req->m.content_size() < sizeof(i32)) {
            reply(*req, EINVAL);
        } else {
            i32 rpc_id;
            req->m >> rpc_id;

#ifdef RPC_STATISTICS
            stat_server_rpc_counting(rpc_id);
#endif // RPC_STATISTICS

            auto it = ctx_->rpc_to_service.find(rpc_id);
            if (it == ctx_->rpc_to_service.end()) {
                // Handler not found - track missing RPC IDs
                bool surpress_warning = false;
                {
                    auto guard = rpc_id_missing_s.lock().unwrap();
                    if (guard->find(rpc_id) == guard->end()) {
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
                // Service found - dispatch via virtual method using RefCell
                size_t svc_index = it->second;
                auto weak_this = weak_self_;
                auto ctx = ctx_.clone();  // Clone Arc for the coroutine
                Coroutine::CreateRun([ctx, svc_index, rpc_id, req = std::move(req), weak_this]() mutable {
                    // Borrow inside coroutine - guard released when lambda exits
                    // (*guard) dereferences RefMut to get Box<Service>&
                    // (*guard)-> calls Box::operator-> to get Service*
                    auto guard = ctx->services[svc_index].borrow_mut();
                    (*guard)->__dispatch__(rpc_id, std::move(req), weak_this);
                }, __FILE__, __LINE__);
            }
        }
    }

    Reactor::GetReactor()->Loop();

    return false;
}

// @safe - Writes buffered data to socket, protected by SpinMutex
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

// @safe - Returns poll mode based on output buffer, protected by SpinMutex
int ServerConnection::poll_mode() const {
    int mode = PollMode::READ;
    auto guard = out_.lock().unwrap();
    if (!guard->empty()) {
        mode |= PollMode::WRITE;
    }
    // Guard auto-unlocks here
    return mode;
}

// @safe - Constructs server with PollThread
// ctx_ starts as None; created in start() after all registrations
Server::Server(rusty::Option<rusty::Arc<PollThread>> poll_thread_worker /* =... */) {
    if (poll_thread_worker.is_none()) {  // Check if Option is None
        poll_thread_ = rusty::Some(PollThread::create());
    } else {
        poll_thread_ = std::move(poll_thread_worker);
    }
}

// @safe - Destroys server and requests close for all connections
// Arc<RpcServiceContext> ensures services live until all connections are done
Server::~Server() {
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
bool ServerListener::handle_read() {
//  fd_set fds;
//  FD_ZERO(&fds);
//  FD_SET(server_sock_, &fds);

  while (true) {
    int clnt_socket;
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
      { verify(set_nonblocking(clnt_socket, true) == 0); }

      auto sconn = rusty::Arc<ServerConnection>::make(ctx_.clone(), clnt_socket);
      // @unsafe - const_cast to initialize weak_self_ (safe: we just created this object)
      { const_cast<ServerConnection&>(*sconn).weak_self_ = sconn; }
      {
          // Track fd for shutdown cleanup (Server reads this list in destructor)
          auto guard = sconn_fds_.lock().unwrap();
          guard->push(clnt_socket);
      }
      // @unsafe - add_pollable_from_current_thread
      { PollThreadWorker::add_pollable_from_current_thread(sconn); }
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
  }
  string host = addr.substr(0, idx);
  string port = addr.substr(idx + 1);

#ifdef USE_IPC
  struct sockaddr_un saun;
  if ((server_sock_ = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
    perror("server: socket");
    exit(1);
  }
  saun.sun_family = AF_UNIX;
  string ipc_addr = "rsock" + port;
  strcpy(saun.sun_path, ipc_addr.data());
  auto len = sizeof(saun.sun_family) + strlen(saun.sun_path)+1;
  ::unlink(ipc_addr.data());
  if (::bind(server_sock_, (struct sockaddr*)&saun, len) != 0) {
    perror("server: socket bind");
    exit(1);
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
    verify(0);  // Fatal error
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
    Log_error("rrr::Server: FATAL - failed to bind to %s:%s after trying all addresses", host.c_str(), port.c_str());
    Log_error("rrr::Server: This is likely because the port is already in use by another process");
    Log_error("rrr::Server: Please check: sudo lsof -i :%s or sudo ss -tulpn | grep %s", port.c_str(), port.c_str());
    // gai_result_ RAII wrapper handles freeaddrinfo automatically

    // Print more helpful message and abort
    fprintf(stderr, "\n====== FATAL ERROR ======\n");
    fprintf(stderr, "Failed to bind to port %s - port may be in use\n", port.c_str());
    fprintf(stderr, "Check with: sudo lsof -i :%s\n", port.c_str());
    fprintf(stderr, "=========================\n\n");
    fflush(stderr);

    verify(0);  // Fatal error - cannot start server
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
  }
  verify(listen_ret == 0);

  int nonblock_ret = set_nonblocking(server_sock_, true);
  if (nonblock_ret != 0) {
    Log_error("rrr::Server: set_nonblocking() failed on %s: errno=%d (%s)", addr.c_str(), errno, strerror(errno));
  }
  verify(nonblock_ret == 0);

  Log_debug("rrr::Server: started on %s", addr.c_str());
}

// @unsafe - Starts server listening (pointer dereference: server_listener_->)
int Server::start(const char* bind_addr) {
  if (!bind_addr) {
    Log_error("rrr::Server::start: bind_addr is NULL!");
    return -1;
  }

  // Wrap each service in RefCell for interior mutability
  rusty::Vec<rusty::RefCell<rusty::Box<Service>>> wrapped_services;
  for (size_t i = 0; i < pending_services_.size(); ++i) {
    wrapped_services.push(rusty::RefCell<rusty::Box<Service>>(std::move(pending_services_[i])));
  }

  // Create immutable RpcServiceContext from pending registration data
  std::string addr_str(bind_addr, strlen(bind_addr));
  ctx_ = rusty::Some(rusty::Arc<RpcServiceContext>::make(
      std::move(pending_rpc_to_service_),
      std::move(wrapped_services),
      addr_str));

  server_listener_ = rusty::Some(rusty::Arc<ServerListener>::make(
      ctx_.as_ref().unwrap().clone(), addr_str));
  poll_thread_.as_ref().unwrap()->add(server_listener_.as_ref().unwrap().clone());
  return 0;
}

// @safe - Unregisters RPC mapping from pending map (must be called before start())
void Server::unreg(i32 rpc_id) {
    pending_rpc_to_service_.erase(rpc_id);
}

// @safe - Signals shutdown to waiting threads
void Server::do_shutdown() {
    Log_debug("Server::do_shutdown");
    {
        auto guard = shutdown_state_.lock().unwrap();
        guard->shutdown = true;
    }
    shutdown_cond_.notify_all();
}

// @safe - Blocks until shutdown is signaled
void Server::wait_for_shutdown() {
    Log_debug("Server::wait_for_shutdown");
    auto guard = shutdown_state_.lock().unwrap();
    guard = shutdown_cond_.wait_while(std::move(guard),
        [](ShutdownState& s) { return !s.shutdown; }).unwrap();
    Log_debug("Server::wait_for_shutdown - done");
}

} // namespace rrr
