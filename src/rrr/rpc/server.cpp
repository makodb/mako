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
#include "server.hpp"
#include "utils.hpp"

// External safety annotations for atomic operations
// @external: {
//   std::__atomic_base::load: [unsafe]
//   std::__atomic_base::store: [unsafe]
//   std::__atomic_base::fetch_add: [unsafe]
//   std::__atomic_base::fetch_sub: [unsafe]
// }


using namespace std;

// External safety annotations for std functions used in this module
// @external: {
//   std::unordered_map::find: [unsafe, (auto) -> auto]
//   std::unordered_map::erase: [unsafe, (auto) -> void]
//   std::function::operator=: [unsafe, (auto) -> std::function&]
// }

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
std::unordered_set<i32> ServerConnection::rpc_id_missing_s;
SpinLock ServerConnection::rpc_id_missing_l_s;


// @unsafe - Initializes connection and updates counter
// SAFETY: Counter operations are thread-safe
ServerConnection::ServerConnection(Server* server, int socket)
        : server_(server), socket_(socket), status_(CONNECTED) {
    // increase number of open connections
    server_->sconns_ctr_.next(1);
    block_read_in.init_block_read(100000000);
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

// @unsafe - Begins reply marshaling with locking
// SAFETY: Protected by output spinlock
void ServerConnection::begin_reply(const Request& req, i32 error_code /* =... */) {
    out_l_.lock();
    v32 v_error_code = error_code;
    v64 v_reply_xid = req.xid;

    bmark_ = rusty::Some(rusty::Box<Marshal::bookmark>(this->out_.set_bookmark(sizeof(i32)))); // will write reply size later

    *this << v_reply_xid;
    *this << v_error_code;
}

// @unsafe - Completes reply packet
// SAFETY: Protected by output spinlock, enables write polling
void ServerConnection::end_reply() {
    // set reply size in packet
    if (bmark_.is_some()) {
        i32 reply_size = out_.get_and_reset_write_cnt();
        out_.write_bookmark(&*bmark_.as_mut().unwrap(), &reply_size);
        bmark_ = rusty::None;  // Reset to None (automatically deletes old value)
    }

    // only update poll mode if connection is still active
    // (connection might have closed while handler was running)
    // NOTE: end_reply() is called from handler threads, NOT the poll thread.
    // Must use channel-based update_mode() - always goes through PollThread.
    if (status_ == CONNECTED) {
        server_->poll_thread_worker_.as_ref().unwrap()->update_mode(*this, Pollable::READ | Pollable::WRITE);
    }

    out_l_.unlock();
}

// @unsafe - Reads requests and dispatches to handlers
// SAFETY: Creates coroutines for concurrent handling
void ServerConnection::handle_read() {
    if (status_ == CLOSED) {
        return;
    }

    // CRITICAL FIX: With edge-triggered epoll (EPOLLET), we must:
    // 1. Drain all data from the socket
    // 2. Process ALL complete packets in the buffer
    // The old code only processed ONE packet per handle_read() call,
    // causing hangs when multiple requests arrive together.

    // First, read all available data from the socket into the buffer
    in_.read_from_fd(socket_);

    if (in_.content_size() == 0) {
        return;
    }

    list<rusty::Box<Request>> complete_requests;

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

    for (auto& req: complete_requests) {

        if (req->m.content_size() < sizeof(i32)) {
            // rpc id not provided
            begin_reply(*req, EINVAL);
            end_reply();
            // req automatically cleaned up by rusty::Box
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
            Coroutine::CreateRun([it, req = std::move(req), weak_this] () mutable {
                // Move rusty::Box to handler, transferring ownership
                it->second(std::move(req), weak_this);

                // Upgrade weak reference to access block_read_in
                auto sconn_opt = weak_this.upgrade();
                if (sconn_opt.is_some()) {
                    auto sconn = sconn_opt.unwrap();
                    const_cast<ServerConnection&>(*sconn).block_read_in.reset();
                }
            });
        } else {
            // Track missing RPC IDs and suppress duplicate warnings
            rpc_id_missing_l_s.lock();
            bool surpress_warning = false;
            if (rpc_id_missing_s.find(rpc_id) == rpc_id_missing_s.end()) {
                rpc_id_missing_s.insert(rpc_id);
            } else {
                surpress_warning = true;
            }
            rpc_id_missing_l_s.unlock();
            if (!surpress_warning) {
                Log_error("rrr::ServerConnection: no handler for rpc_id=0x%08x", rpc_id);
            }
            begin_reply(*req, ENOENT);
            end_reply();
            // req automatically cleaned up by rusty::Box
        }
    }
}

// @unsafe - Writes buffered data to socket
// SAFETY: Protected by output spinlock
// Returns new poll mode, or MODE_NO_CHANGE if no update needed
int ServerConnection::handle_write() {
    if (status_ == CLOSED) {
        return Pollable::MODE_NO_CHANGE;
    }

    int result = Pollable::MODE_NO_CHANGE;
    out_l_.lock();
    out_.write_to_fd(socket_);
    if (out_.empty()) {
        // Return READ-only mode - PollThreadWorker will update epoll
        result = Pollable::READ;
    }
    out_l_.unlock();
    return result;
}

// @safe - Simple error handler
void ServerConnection::handle_error() {
    this->close();
}

// @unsafe - Closes connection with proper cleanup
// SAFETY: Thread-safe with server connection lock, idempotent
void ServerConnection::close() {
    if (status_ == CONNECTED) {
        server_->sconns_l_.lock();

        // Find our Arc in server's connection set
        rusty::Option<rusty::Arc<ServerConnection>> self;
        for (auto it = server_->sconns_.begin(); it != server_->sconns_.end(); ++it) {
            if (it->get() == this) {
                self = rusty::Some(it->clone());
                server_->sconns_.erase(it);
                break;
            }
        }
        server_->sconns_l_.unlock();

        if (self.is_some()) {
            // Arc gives const access, need to cast to call remove()
            auto& conn = const_cast<ServerConnection&>(*self.unwrap());
            server_->poll_thread_worker_.as_ref().unwrap()->remove(conn);
            status_ = CLOSED;
            ::close(socket_);
            Log_debug("server@%s close ServerConnection at fd=%d", server_->addr_.c_str(), socket_);
        }
    }
}

// @safe - Returns poll mode based on output buffer
int ServerConnection::poll_mode() const {
    int mode = Pollable::READ;
    out_l_.lock();
    if (!out_.empty()) {
        mode |= Pollable::WRITE;
    }
    out_l_.unlock();
    return mode;
}

// @unsafe - Constructs server with PollThread
// SAFETY: Shared ownership via Arc<Mutex<>>, creates one if not provided
Server::Server(rusty::Option<rusty::Arc<PollThread>> poll_thread_worker /* =... */, ThreadPool* thrpool /* =? */)
        : server_sock_(-1), status_(NEW) {

    // get rid of eclipse warning
    memset(&loop_th_, 0, sizeof(loop_th_));

    if (poll_thread_worker.is_none()) {  // Check if Option is None
        poll_thread_worker_ = rusty::Some(PollThread::create());
    } else {
        poll_thread_worker_ = std::move(poll_thread_worker);
    }

//    if (thrpool == nullptr) {
//        threadpool_ = new ThreadPool;
//    } else {
//        threadpool_ = (ThreadPool *) thrpool->ref_copy();
//    }
}

// @unsafe - Destroys server and waits for connections
// SAFETY: Joins thread, closes all connections, waits for cleanup
Server::~Server() {
    if (status_ == RUNNING) {
        status_ = STOPPING;
        // wait till accepting thread done
        Pthread_join(loop_th_, nullptr);

        verify(server_sock_ == -1 && status_ == STOPPED);
    }

    sconns_l_.lock();
    vector<rusty::Arc<ServerConnection>> sconns;
    sconns.reserve(sconns_.size());
    for (auto& sconn : sconns_) {
        sconns.push_back(sconn.clone());  // Clone Arc from set
    }
    // NOTE: do NOT clear sconns_ here, because when running the following
    // arc_conn->close(), the ServerConnection object will check the sconns_ to
    // ensure it still resides in sconns_
    sconns_l_.unlock();

    for (auto& it: sconns) {
        auto& conn = const_cast<ServerConnection&>(*it);
        conn.close();
        poll_thread_worker_.as_ref().unwrap()->remove(conn);
    }

    if (sp_server_listener_.is_some()) {
        auto& listener = const_cast<ServerListener&>(*sp_server_listener_.as_ref().unwrap());
        listener.close();
        poll_thread_worker_.as_ref().unwrap()->remove(listener);
        sp_server_listener_ = rusty::None;  // Reset to None
    }

    // Now clear sconns_ and the local copy to release Arcs
    sconns_l_.lock();
    sconns_.clear();
    sconns_l_.unlock();
    sconns.clear();  // Release local Arcs

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

//    threadpool_->release();
    // owned_poll_thread_worker_ automatically released by shared_ptr

    //Log_debug("rrr::Server: destroyed");
}

struct start_server_loop_args_type {
    Server* server;
    struct addrinfo* gai_result;
    struct addrinfo* svr_addr;
};

// @unsafe - C-style thread entry point
// SAFETY: arg is always valid start_server_loop_args_type*
void* Server::start_server_loop(void* arg) {
    start_server_loop_args_type* start_server_loop_args = (start_server_loop_args_type*) arg;
    start_server_loop_args->server->server_loop(start_server_loop_args->svr_addr);
    freeaddrinfo(start_server_loop_args->gai_result);
    delete start_server_loop_args;
    if (arg) {
        pthread_exit(nullptr);
    }
    return nullptr;
}

// @unsafe - Main server accept loop
// SAFETY: Uses select for safe shutdown, proper socket handling
void Server::server_loop(struct addrinfo* svr_addr) {
    fd_set fds;
    while (status_ == RUNNING) {
        FD_ZERO(&fds);
        FD_SET(server_sock_, &fds);

        // use select to avoid waiting on accept when closing server
        timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 50 * 1000; // 0.05 sec
        int fdmax = server_sock_;

        int n_ready = select(fdmax + 1, &fds, nullptr, nullptr, &tv);
        if (n_ready == 0) {
            continue;
        }
        if (status_ != RUNNING) {
            break;
        }

#ifdef USE_IPC
      struct sockaddr_un fsaun;
        uint32_t from_len;
      int clnt_socket = ::accept(server_sock_, (struct sockaddr*)&fsaun, &from_len);
#else
      int clnt_socket = accept(server_sock_, svr_addr->ai_addr, &svr_addr->ai_addrlen);
#endif
        if (clnt_socket >= 0 && status_ == RUNNING) {
            Log_debug("server@%s got new client, fd=%d", this->addr_.c_str(), clnt_socket);
            verify(set_nonblocking(clnt_socket, true) == 0);
            int buf_len = 1024 * 1024; // 1M buffer
            setsockopt(clnt_socket, SOL_SOCKET, SO_RCVBUF, &buf_len, sizeof(buf_len));
            setsockopt(clnt_socket, SOL_SOCKET, SO_SNDBUF, &buf_len, sizeof(buf_len));
            sconns_l_.lock();
            auto sconn = rusty::Arc<ServerConnection>::make(this, clnt_socket);
            const_cast<ServerConnection&>(*sconn).weak_self_ = sconn;  // Initialize weak to self
            sconns_.insert(sconn.clone());  // Insert Arc into set
            sconns_l_.unlock();
            poll_thread_worker_.as_ref().unwrap()->add(sconn);
        }
    }

    close(server_sock_);
    server_sock_ = -1;
    status_ = STOPPED;
}

// @unsafe - Accepts new client connections
// @unsafe - Calls unsafe Log::debug for connection logging
// SAFETY: Thread-safe with server connection lock
void ServerListener::handle_read() {
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

      auto sconn = rusty::Arc<ServerConnection>::make(server_, clnt_socket);
      const_cast<ServerConnection&>(*sconn).weak_self_ = sconn;  // Initialize weak to self
      server_->sconns_l_.lock();
      server_->sconns_.insert(sconn.clone());  // Insert Arc into set
      server_->sconns_l_.unlock();
      server_->poll_thread_worker_.as_ref().unwrap()->add(sconn);
    } else {
      break;
    }
  }
}

// @safe - Closes server socket using safe external annotation
void ServerListener::close() {
  ::close(server_sock_);
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
// SAFETY: Creates listener with proper socket setup
int Server::start(const char* bind_addr) {
  if (!bind_addr) {
    Log_error("rrr::Server::start: bind_addr is NULL!");
    return -1;
  }
  string addr(bind_addr, strlen(bind_addr));
  sp_server_listener_ = rusty::Some(rusty::Arc<ServerListener>::make(this, addr));
  poll_thread_worker_.as_ref().unwrap()->add(sp_server_listener_.as_ref().unwrap().clone());
  return 0;
}

// @unsafe - Calls std::unordered_map::find and operator= (external unsafe)
// SAFETY: Thread-safe map operations for handler registration
int Server::reg(i32 rpc_id, const RequestHandler& func) {
    // disallow duplicate rpc_id
    if (handlers_.find(rpc_id) != handlers_.end()) {
        return EEXIST;
    }

    handlers_[rpc_id] = func;

    return 0;
}

// @unsafe - Calls std::unordered_map::erase (external unsafe)
// SAFETY: Thread-safe map operation for handler removal
void Server::unreg(i32 rpc_id) {
    handlers_.erase(rpc_id);
}

} // namespace rrr
