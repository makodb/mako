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

// External safety annotations for STL and language features that cannot have in-place annotations
// Note: Marshal, Log, SpinLock, PollThread, Reactor, Coroutine, and rusty-cpp types
// now have in-place annotations in their respective headers.
// Note: std::atomic public API (load, store, etc.) is annotated in event.h
// @external: {
//   std::unordered_map::find: [unsafe]
//   std::unordered_map::end: [unsafe]
//   std::unordered_map::erase: [unsafe]
//   std::unordered_map::operator[]: [unsafe]
//   std::unordered_set::find: [unsafe]
//   std::unordered_set::end: [unsafe]
//   std::unordered_set::insert: [unsafe]
//   std::unordered_set::erase: [unsafe]
//   std::unordered_set::begin: [unsafe]
//   std::list::push_back: [unsafe]
//   std::__cxx11::list::push_back: [unsafe]
//   std::function::operator=: [unsafe]
//   std::function::operator(): [unsafe]
//   operator!=: [unsafe]
//   operator==: [unsafe]
//   const_cast: [unsafe]
// }

using namespace std;

namespace rrr {


#ifdef RPC_STATISTICS

static const int g_stat_server_batching_size = 1000;
static int g_stat_server_batching[g_stat_server_batching_size];
static int g_stat_server_batching_idx;
static uint64_t g_stat_server_batching_report_time = 0;
static const uint64_t g_stat_server_batching_report_interval = 1000 * 1000 * 1000;

// @unsafe - Uses global mutable state and calls Log::info
// SAFETY: Only called from single-threaded server context
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

// @unsafe - Uses global mutable state and calls Log::info
// SAFETY: Only called from single-threaded server context
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


// @safe - Initializes connection and updates counter
ServerConnection::ServerConnection(Server* server, int socket)
        : server_(server), socket_(socket), status_(CONNECTED) {
    // increase number of open connections
    server_->sconns_ctr_.next(1);
}

// @safe - Updates connection counter
ServerConnection::~ServerConnection() {
    // decrease number of open connections
    server_->sconns_ctr_.next(-1);
}

// get_shared() is now inherited from Pollable base class

// @safe - Delegates to thread pool
int ServerConnection::run_async(const std::function<void()>& f) {
  // run_async should not be used - process RPC synchronously
  // Call f() directly instead where this was being used
  verify(0); // This should never be called
  return 0;
}

// @safe - Begins reply marshaling with locking (has internal @unsafe block)
// SAFETY: Protected by output spinlock (SpinLock marked as external)
void ServerConnection::begin_reply(const Request& req, i32 error_code /* =... */) {
    // @unsafe
    { out_l_.get()->lock(); }
    v32 v_error_code = error_code;
    v64 v_reply_xid = req.xid;

    bmark_ = rusty::Some(rusty::Box<Marshal::bookmark>(this->out_.set_bookmark(sizeof(i32)))); // will write reply size later

    *this << v_reply_xid;
    *this << v_error_code;
}

// @safe - Completes reply packet
// SAFETY: Protected by output spinlock, uses weak ref to poll thread
void ServerConnection::end_reply() {
    // set reply size in packet
    if (bmark_.is_some()) {
        i32 reply_size = out_.get_and_reset_write_cnt();
        out_.write_bookmark(*bmark_.as_mut().unwrap(), reply_size);
        bmark_ = rusty::None;  // Reset to None (automatically deletes old value)
    }

    // Mark that we need write mode update - poll loop will handle it
    // This avoids the need for direct PollThreadWorker access from here
    if (status_ == CONNECTED) {
        pending_write_update_.set(true);
    }

    // @unsafe
    { out_l_.get()->unlock(); }
}

// @unsafe - Reads requests and dispatches to handlers
// SAFETY: Contains raw pointer operations (&packet_size, req->m)
bool ServerConnection::handle_read() {
    if (status_ == CLOSED) {
        return false;
    }

    // CRITICAL FIX: With edge-triggered epoll (EPOLLET), we must:
    // 1. Drain all data from the socket
    // 2. Process ALL complete packets in the buffer
    // The old code only processed ONE packet per handle_read() call,
    // causing hangs when multiple requests arrive together.

    // First, read all available data from the socket into the buffer
    size_t bytes_read = in_.read_from_fd(socket_);
    if (bytes_read == 0 && in_.content_size() < sizeof(i32)) {
        // Connection made no forward progress and there isn't enough buffered
        // data to decode a packet header yet. Mirror legacy behavior by
        // deferring until we either read more bytes or the peer closes.
        return false;
    }

    std::list<rusty::Box<Request>> complete_requests;

    // Process ALL complete packets in the buffer
    for (;;) {
        i32 packet_size;
        int n_peek = in_.peek(&packet_size, sizeof(i32));
        if (n_peek == sizeof(i32) && in_.content_size() >= packet_size + sizeof(i32)) {
            // consume the packet size
            verify(in_.read(&packet_size, sizeof(i32)) == sizeof(i32));

            auto req = rusty::Box<Request>(new Request());
            verify(req->m.read_from_marshal(in_, packet_size) == (size_t) packet_size);

            v64 v_xid;
            req->m >> v_xid;
            req->xid = v_xid.get();
            complete_requests.push_back(std::move(req));

        } else {
            // packet not complete or there's no more packet to process
            break;
        }
    }

#ifdef RPC_STATISTICS
    stat_server_batching(complete_requests.size());
#endif // RPC_STATISTICS

    for (auto& req : complete_requests) {
        if (req->m.content_size() < sizeof(i32)) {
            begin_reply(*req, EINVAL);
            end_reply();
            continue;
        }

        i32 rpc_id;
        req->m >> rpc_id;

#ifdef RPC_STATISTICS
        stat_server_rpc_counting(rpc_id);
#endif // RPC_STATISTICS

        auto it = server_->handlers_.find(rpc_id);
        if (it != server_->handlers_.end()) {
            // rusty::Function allows direct capture of move-only types like rusty::Box
            // Lambda captures rusty::Box<Request> by move, maintaining single ownership semantics
            auto weak_this = weak_self_;
            // Jetpack: pass file/line for debugging; mako-dev block_read_in not used in this branch
            Coroutine::CreateRun([handler = it->second, req = std::move(req), weak_this]() mutable {
                handler(std::move(req), weak_this);
            }, __FILE__, __LINE__);
        } else {
            // Track missing RPC IDs and suppress duplicate warnings
            // @unsafe - SpinMutex guard operations
            bool surpress_warning = false;
            {
                auto guard = rpc_id_missing_s.lock().unwrap();
                if (guard->find(rpc_id) == guard->end()) {
                    guard->insert(rpc_id);
                } else {
                    surpress_warning = true;
                }
            }  // Guard dropped here, releasing lock
            if (!surpress_warning) {
                Log_warn("rrr::ServerConnection: no handler for rpc_id = %d", rpc_id);
            }
            begin_reply(*req, ENOENT);
            end_reply();
        }
    }

    // Pump reactor after processing batch
    Reactor::GetReactor()->Loop();

    return false;
}

// @safe - Writes buffered data to socket
// SAFETY: Protected by output spinlock (SpinLock marked as external)
// Returns new poll mode, or MODE_NO_CHANGE if no update needed
int ServerConnection::handle_write() {
    if (status_ == CLOSED) {
        return Pollable::MODE_NO_CHANGE;
    }

    int result = Pollable::MODE_NO_CHANGE;
    // @unsafe
    { out_l_.get()->lock(); }
    out_.write_to_fd(socket_);
    if (out_.empty()) {
        // Return READ-only mode - PollThreadWorker will update epoll
        result = Pollable::READ;
    }
    // @unsafe
    { out_l_.get()->unlock(); }
    return result;
}

// @safe - Error handler (explicit this-> is now safe in rusty-cpp)
void ServerConnection::handle_error() {
    this->close();
}

// @unsafe - Closes connection with proper cleanup
// SAFETY: Should only be called from poll thread (via do_close_pollable or handle_error)
// Idempotent, proper cleanup sequence
void ServerConnection::close() {
    if (status_ == CONNECTED) {
        status_ = CLOSED;
        // @unsafe - system call
        { ::close(socket_); }
        // @unsafe - logging
        { Log_debug("server@%s close ServerConnection at fd=%d", server_->addr_.c_str(), socket_); }

        // Remove from sconns_ (if tracked)
        // @unsafe - SpinMutex guard operations
        {
            auto guard = server_->sconns_.lock().unwrap();
            for (auto it = guard->begin(); it != guard->end(); ++it) {
                if (it->get() == this) {
                    guard->erase(it);
                    break;
                }
            }
        }  // Guard dropped here, releasing lock
    }
}

// @safe - Returns poll mode based on output buffer (has internal @unsafe blocks)
// Uses UnsafeCell for interior mutability (const method can access mutable SpinLock)
int ServerConnection::poll_mode() const {
    int mode = Pollable::READ;
    // @unsafe
    { out_l_.get()->lock(); }
    if (!out_.empty()) {
        mode |= Pollable::WRITE;
    }
    // @unsafe
    { out_l_.get()->unlock(); }
    return mode;
}

// @safe - Constructs server with PollThread
Server::Server(rusty::Option<rusty::Arc<PollThread>> poll_thread_worker /* =... */) {
    if (poll_thread_worker.is_none()) {  // Check if Option is None
        poll_thread_worker_ = rusty::Some(PollThread::create());
    } else {
        poll_thread_worker_ = std::move(poll_thread_worker);
    }
}

// @safe - Destroys server and waits for connections (calls @unsafe functions)
Server::~Server() {
    // Request close for all connections via poll thread
    // @unsafe - SpinMutex guard operations
    {
        auto guard = sconns_.lock().unwrap();
        for (auto& sconn : *guard) {
            poll_thread_worker_.as_ref().unwrap()->request_close(sconn->fd());
        }
        guard->clear();  // Clear our references - poll thread owns them now
    }  // Guard dropped here, releasing lock

    // Request close for server listener via poll thread
    if (sp_server_listener_.is_some()) {
        poll_thread_worker_.as_ref().unwrap()->request_close(sp_server_listener_.as_ref().unwrap()->fd());
        sp_server_listener_ = rusty::None;  // Reset to None
    }

    // make sure all open connections are closed
    int alive_connection_count = -1;
    for (;;) {
        int new_alive_connection_count = sconns_ctr_.peek_next();
        if (new_alive_connection_count <= 0) {
            break;
        }
        if (alive_connection_count == -1 || new_alive_connection_count < alive_connection_count) {
            Log_debug("waiting for %d alive connections to shutdown", new_alive_connection_count);
        }
        alive_connection_count = new_alive_connection_count;
        // sleep 0.05 sec because this is the timeout for PollThread's epoll()
        usleep(50 * 1000);
    }
    verify(sconns_ctr_.peek_next() == 0);

    // Clean up owned services (after connections closed, so handlers don't access them)
    for (auto& cleanup : service_cleanups_) {
        cleanup();
    }
    service_cleanups_.clear();
}

// @unsafe - Accepts new client connections
// SAFETY: Contains raw pointer operations (p_svr_addr_->ai_addr)
bool ServerListener::handle_read() {
//  fd_set fds;
//  FD_ZERO(&fds);
//  FD_SET(server_sock_, &fds);

  while (true) {
#ifdef USE_IPC
    struct sockaddr_un fsaun;
      uint32_t from_len;
    int clnt_socket = ::accept(server_sock_, (struct sockaddr*)&fsaun, &from_len);
#else
    int clnt_socket = ::accept(server_sock_, p_svr_addr_->ai_addr, &p_svr_addr_->ai_addrlen);
#endif
    if (clnt_socket >= 0) {
      Log_debug("server@%s got new client, fd=%d", this->addr_.c_str(), clnt_socket);
      verify(set_nonblocking(clnt_socket, true) == 0);

      // @unsafe - SpinMutex guard operations
      auto sconn = rusty::Arc<ServerConnection>::make(server_, clnt_socket);
      const_cast<ServerConnection&>(*sconn).weak_self_ = sconn;  // Initialize weak to self
      {
          auto guard = server_->sconns_.lock().unwrap();
          guard->insert(sconn.clone());  // Insert Arc into set
      }
      server_->poll_thread_worker_.as_ref().unwrap()->add(sconn);
    } else {
      break;
    }
  }
  return false;
}

// @safe - Closes server socket using safe external annotation
void ServerListener::close() {
  if (server_sock_ >= 0) {
    ::close(server_sock_);
    server_sock_ = -1;
  }
}

// @safe - Creates listener socket and binds to address
// All socket operations are marked safe via external annotations
ServerListener::ServerListener(Server* server, string addr) {
  server_ = server;
  addr_ = addr;
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
  struct addrinfo hints, *result, *rp;
  memset(&hints, 0, sizeof(struct addrinfo));
  hints.ai_family = AF_INET; // ipv4
  hints.ai_socktype = SOCK_STREAM; // tcp
  hints.ai_flags = AI_PASSIVE; // server side

  int r = getaddrinfo((host == "0.0.0.0") ? nullptr : host.c_str(), port.c_str(), &hints, &result);
  if (r != 0) {
    Log_error("rrr::Server: getaddrinfo(): %s", gai_strerror(r));
  }

  for (rp = result; rp != nullptr; rp = rp->ai_next) {
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
    freeaddrinfo(result);

    // Print more helpful message and abort
    fprintf(stderr, "\n====== FATAL ERROR ======\n");
    fprintf(stderr, "Failed to bind to port %s - port may be in use\n", port.c_str());
    fprintf(stderr, "Check with: sudo lsof -i :%s\n", port.c_str());
    fprintf(stderr, "=========================\n\n");
    fflush(stderr);

    verify(0);  // Fatal error - cannot start server
  } else {
    p_gai_result_ = result;
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

// @unsafe - Starts server listening on specified address
// SAFETY: Contains raw pointer dereference (sp_server_listener_->)
int Server::start(const char* bind_addr) {
  if (!bind_addr) {
    Log_error("rrr::Server::start: bind_addr is NULL!");
    return -1;
  }
  addr_ = string(bind_addr, strlen(bind_addr));
  sp_server_listener_ = rusty::Some(rusty::Arc<ServerListener>::make(this, addr_));
  poll_thread_worker_.as_ref().unwrap()->add(sp_server_listener_.as_ref().unwrap().clone());
  return 0;
}

// @safe - Registers RPC handler in map (calls @unsafe unordered_map)
int Server::reg_handler(i32 rpc_id, const RequestHandler& func) {
    // disallow duplicate rpc_id
    if (handlers_.find(rpc_id) != handlers_.end()) {
        return EEXIST;
    }

    handlers_[rpc_id] = func;

    return 0;
}

// @safe - Unregisters RPC handler from map (calls @unsafe unordered_map)
void Server::unreg(i32 rpc_id) {
    handlers_.erase(rpc_id);
}

} // namespace rrr
