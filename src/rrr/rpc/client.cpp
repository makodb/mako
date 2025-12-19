#include <string>
#include <memory>
#include <chrono>
#include <mutex>

#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netdb.h>
#include <netinet/tcp.h>

#include "reactor/coroutine.h"
#include "reactor/reactor.h"
#include "client.hpp"
#include "utils.hpp"

// External safety annotations for atomic operations and STL functions
// @external: {
//   std::__atomic_base::load: [unsafe]
//   std::__atomic_base::store: [unsafe]
//   std::__atomic_base::fetch_add: [unsafe]
//   std::__atomic_base::fetch_sub: [unsafe]
//   std::vector::push_back: [unsafe]
//   rrr::Log::error: [unsafe]
//   Log_error: [unsafe]
//   Log_debug: [unsafe]
//   std::unordered_map<auto,auto>::clear: [safe]
//   std::unordered_map<auto,auto,auto,auto,auto>::clear: [safe]
//   rrr::Marshal::write_to_fd: [unsafe]
//   rrr::Marshal::empty: [safe]
//   const_cast: [unsafe]
// }


using namespace std;

namespace rrr {

// ============================================================================
// Future implementation
// ============================================================================

// @unsafe - Uses rusty::Condvar (low-level sync primitive)
void Future::wait() const {
  std::unique_lock<std::mutex> lock(*condvar_m_.get());
  ready_cond_.get()->wait(lock, [this]() {
    auto guard = state_.lock();
    return guard->ready || guard->timed_out;
  });
}

// @unsafe - Uses rusty::Condvar for timed waiting (low-level sync)
void Future::timed_wait(double sec) const {
  std::unique_lock<std::mutex> lock(*condvar_m_.get());

  auto duration = std::chrono::duration<double>(sec);
  bool success = ready_cond_.get()->wait_for(lock, duration, [this]() {
    auto guard = state_.lock();
    return guard->ready || guard->timed_out;
  });

  bool is_timed_out = false;
  {
    auto guard = state_.lock();
    if (!success && !guard->ready) {
      guard->timed_out = true;
      is_timed_out = true;
      error_code_.set(ETIMEDOUT);
    } else {
      is_timed_out = guard->timed_out;
    }
  }

  // Release lock before calling callback
  lock.unlock();

  // NOTE: timed_wait callback still needs Arc parameter update (TODO: requires Arc access)
  // For now, this is only called in test scenarios
  // if (is_timed_out && attr_.callback != nullptr) {
  //   attr_.callback(???);  // Need Arc<Future> to self
  // }
}

// @unsafe - Uses rusty::Condvar for notification (low-level sync)
void Future::notify_ready(rusty::Arc<Future> self) const {
  bool should_callback = false;
  {
    std::lock_guard<std::mutex> cv_lock(*condvar_m_.get());
    {
      auto guard = state_.lock();
      if (!guard->timed_out) {
        guard->ready = true;
      }
      should_callback = guard->ready;
    }
    // Notify while holding condvar_m_ to prevent race with wait()
    ready_cond_.get()->notify_all();
  }

  // Execute callback outside lock to avoid deadlock
  if (should_callback && attr_.callback != nullptr) {
    // SAFE: Callback receives Arc<Future> for lifetime safety
    auto x = attr_.callback;
    Coroutine::CreateRun([x, self]() {  // Capture Arc, not raw pointer
      x(self);  // Callback receives Arc<Future>
    }, __FILE__, __LINE__);
  }
}

// ============================================================================
// ClientConnection implementation
// ============================================================================

// @unsafe - Initializes connection
// SAFETY: Stores references safely
ClientConnection::ClientConnection(Client* client, rusty::Arc<PollThread> poll_thread_worker)
    : client_(client),
      poll_thread_worker_(poll_thread_worker),
      socket_(-1),
      status_(NEW) {
}

// @safe - Simple destructor
ClientConnection::~ClientConnection() {
  invalidate_pending_futures();
}

// @safe - Cancels all pending futures with error (has internal @unsafe blocks)
// SAFETY: Protected by spinlock, proper refcount management
void ClientConnection::invalidate_pending_futures() {
  list<rusty::Arc<Future>> futures;
  // @unsafe - SpinLock pointer dereference and map operations
  {
    pending_fu_l_.get()->lock();
    for (auto& it: pending_fu_) {
      futures.push_back(it.second);  // Copy Arc
    }
    pending_fu_.clear();  // Clear map (releases its Arc references)
    pending_fu_l_.get()->unlock();
  }

  for (auto& fu: futures) {
    fu->error_code_.set(ENOTCONN);
    fu->notify_ready(fu);  // Pass Arc to self for callback safety
    // Arc auto-released when list destroyed
  }
}

// @safe - Closes socket and invalidates futures (has internal @unsafe blocks)
// SAFETY: Idempotent, proper cleanup sequence
void ClientConnection::close() {
  if (status_ == CONNECTED) {
    // @unsafe - pointer dereference and system call
    {
      poll_thread_worker_->remove(*this);
      ::close(socket_);
    }
  }
  status_ = CLOSED;
  invalidate_pending_futures();
}

// Jetpack: handle_free for explicit future cleanup
void ClientConnection::handle_free(i64 xid) {
  pending_fu_l_.get()->lock();
  auto it = pending_fu_.find(xid);
  if (it != pending_fu_.end()) {
    pending_fu_.erase(it);
    // Arc auto-released when removed from map
  }
  pending_fu_l_.get()->unlock();
}

// @unsafe - Establishes TCP/IPC connection to server
// SAFETY: Proper socket creation, configuration, and error handling
int ClientConnection::connect(const char* addr) {
  verify(status_ != CONNECTED);
  string addr_str(addr);
  size_t idx = addr_str.find(":");
  if (idx == string::npos) {
    Log_error("rrr::ClientConnection: bad connect address: %s", addr);
    return EINVAL;
  }
  string host = addr_str.substr(0, idx);
  host_ = host;
  string port = addr_str.substr(idx + 1);

#ifdef USE_IPC
  struct sockaddr_un saun;
  saun.sun_family = AF_UNIX;
  string ipc_addr = "rsock" + port;
  strcpy(saun.sun_path, ipc_addr.data());
  int sock = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sock < 0) {
    perror("client: socket");
    exit(1);
  }
  socket_ = sock;
  auto len = sizeof(saun.sun_family) + strlen(saun.sun_path)+1;
  if (::connect(socket_, (struct sockaddr*)&saun, len) < 0) {
    perror("client: connect");
    exit(1);
  }
#else

  struct addrinfo hints, * result, * rp;
  memset(&hints, 0, sizeof(struct addrinfo));

  hints.ai_family = AF_INET; // ipv4
  hints.ai_socktype = SOCK_STREAM; // tcp

  int r = getaddrinfo(host.c_str(), port.c_str(), &hints, &result);
  if (r != 0) {
    Log_error("rrr::ClientConnection: getaddrinfo(): %s", gai_strerror(r));
    return EINVAL;
  }

  for (rp = result; rp != nullptr; rp = rp->ai_next) {
    int sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (sock == -1) {
      continue;
    }
    socket_ = sock;

    const int yes = 1;
    verify(setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == 0);
    verify(setsockopt(socket_, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes)) == 0);
    int buf_len = 1024 * 1024;
    setsockopt(socket_, SOL_SOCKET, SO_RCVBUF, &buf_len, sizeof(buf_len));
    setsockopt(socket_, SOL_SOCKET, SO_SNDBUF, &buf_len, sizeof(buf_len));

    if (::connect(socket_, rp->ai_addr, rp->ai_addrlen) == 0) {
      break;
    }
    ::close(socket_);
    socket_ = -1;
  }
  freeaddrinfo(result);

  if (rp == nullptr) {
    // failed to connect
    return ENOTCONN;
  }
#endif
  verify(set_nonblocking(socket_, true) == 0);
  Log_debug("rrr::ClientConnection: connected to %s", addr);

  status_ = CONNECTED;

  // Register with poll thread using weak_self_
  auto self = weak_self_.upgrade();
  if (self.is_some()) {
    poll_thread_worker_->add(self.unwrap());
  } else {
    Log_error("rrr::ClientConnection: weak_self_ upgrade failed - connection may not have been created properly");
    return EINVAL;
  }

  return 0;
}

// @safe - Simple error handler
void ClientConnection::handle_error() {
  close();
}

// @safe - Writes buffered data to socket (has internal @unsafe blocks)
// SAFETY: Protected by spinlock, handles partial writes
// Returns new poll mode, or MODE_NO_CHANGE if no update needed
int ClientConnection::handle_write() {
  if (status_ != CONNECTED) {
    return Pollable::MODE_NO_CHANGE;
  }
  // Jetpack: respect pause state
  if (paused_) return Pollable::MODE_NO_CHANGE;

  int result = Pollable::MODE_NO_CHANGE;
  // @unsafe - SpinLock pointer dereference
  { out_l_.get()->lock(); }
  // @unsafe - I/O operation
  { out_.write_to_fd(socket_); }
  if (out_.empty()) {
    // Return READ-only mode - PollThreadWorker will update epoll
    result = Pollable::READ;
  }
  // @unsafe - SpinLock pointer dereference
  { out_l_.get()->unlock(); }
  return result;
}

// @unsafe - Reads and processes RPC responses
// SAFETY: Protected by spinlock, validates packet structure
// Jetpack: Split into handle_read_one and handle_read_two for batching
bool ClientConnection::handle_read() {
  if (!handle_read_one()) {
    return false;
  }

  while (true) {
    bool done = handle_read_two();
    if (done) {
      break;
    }
    if (status_ != CONNECTED) {
      return false;
    }
  }

  // Ensure any ready futures wake their waiting coroutines (Jetpack + mako compatibility)
  Reactor::GetReactor()->Loop();

  return true;
}

// Jetpack: First phase - read data from socket
bool ClientConnection::handle_read_one() {
  if (status_ != CONNECTED) {
    return false;
  }

  int bytes_read = in_.read_from_fd(socket_);
  if (bytes_read == 0) {
    return false;
  }

  return true;
}

// Jetpack: Second phase - process packets from buffer
bool ClientConnection::handle_read_two() {
  if (status_ != CONNECTED) {
    return false;
  }

  bool done = false;
  int iters = 5;

  if (is_client_mode_) {
    iters = INT_MAX;
  }

  for (int i = 0; i < iters; i++) {
    i32 packet_size;

    int n_peek = in_.peek(&packet_size, sizeof(i32));

    if (n_peek == sizeof(i32)
        && in_.content_size() >= packet_size + sizeof(i32)) {

      verify(in_.read(&packet_size, sizeof(i32)) == sizeof(i32));

      v64 v_reply_xid;
      v32 v_error_code;

      in_ >> v_reply_xid >> v_error_code;

      pending_fu_l_.get()->lock();
      auto it = pending_fu_.find(v_reply_xid.get());
      if (it != pending_fu_.end()) {
        rusty::Arc<Future> fu = it->second;  // Copy Arc (refcount still 2)
        verify(fu->xid_ == v_reply_xid.get());

        pending_fu_.erase(it);  // Remove from map (refcount 2→1)
        pending_fu_l_.get()->unlock();

        fu->error_code_.set(v_error_code.get());
        fu->reply_.get()->read_from_marshal(in_,
                                            packet_size - v_reply_xid.val_size()
                                                - v_error_code.val_size());

        fu->notify_ready(fu);  // Pass Arc to self for callback safety

        // Arc auto-released when scope exits (refcount 1→0 if user released theirs)
      } else {
        // the future might timed out
        pending_fu_l_.get()->unlock();

        // Jetpack: consume data for timed-out futures
        Marshal reply;
        reply.read_from_marshal(in_,
                                packet_size - v_reply_xid.val_size()
                                - v_error_code.val_size());
      }
    } else {
      done = true;
      break;
    }
  }

  Reactor::GetReactor()->Loop();
  return done;
}

// @safe - Determines polling mode based on output buffer (has internal @unsafe blocks)
// SAFETY: Uses spinlock for thread-safety
int ClientConnection::poll_mode() const {
  int mode = Pollable::READ;
  // @unsafe - SpinLock pointer dereference
  { out_l_.get()->lock(); }
  if (!out_.empty()) {
    mode |= Pollable::WRITE;
  }
  // @unsafe - SpinLock pointer dereference
  { out_l_.get()->unlock(); }
  return mode;
}

// @unsafe - Starts new RPC request with marshaling
// SAFETY: Protected by spinlocks, proper refcounting
FutureResult ClientConnection::begin_request(i32 rpc_id, const FutureAttr& attr /* =... */) {
  out_l_.get()->lock();

  if (status_ != CONNECTED) {
    out_l_.get()->unlock();
    return FutureResult::Err(ENOTCONN);
  }

  auto fu = Future::create(xid_counter_.next(), attr);

  pending_fu_l_.get()->lock();
  pending_fu_.insert_or_assign(fu->xid_, fu);  // Store Arc in map (refcount now 2)
  pending_fu_l_.get()->unlock();

  // check if the connection gets closed in the meantime
  if (status_ != CONNECTED) {
    pending_fu_l_.get()->lock();
    auto it = pending_fu_.find(fu->xid_);
    if (it != pending_fu_.end()) {
      pending_fu_.erase(it);  // Arc auto-released when removed from map
    }
    pending_fu_l_.get()->unlock();
    out_l_.get()->unlock();

    return FutureResult::Err(ENOTCONN);
  }

  // Set bookmark for packet size (will fill later)
  Marshal::bookmark* bm = out_.set_bookmark(sizeof(i32));
  bmark_ = rusty::Some(rusty::Box<Marshal::bookmark>(bm));

  *this << v64(fu->xid_);
  *this << rpc_id;

  // Arc is in pending_fu_ (refcount=2), return copy to caller
  return FutureResult::Ok(fu);
}

// @unsafe - Finalizes request packet with size header
// SAFETY: Updates bookmark, enables write polling
void ClientConnection::end_request() {
  // set reply size in packet
  if (bmark_.is_some()) {
    i32 request_size = out_.get_and_reset_write_cnt();
    out_.write_bookmark(&*bmark_.as_mut().unwrap(), &request_size);
    bmark_ = rusty::None;  // Reset to None (automatically deletes old value)
  }

  // Jetpack: reset flags
  out_.found_dep = false;
  out_.valid_id = false;

  // always enable write events since the code above guaranteed there
  // will be some data to send
  // NOTE: end_request() may be called from user threads OR the poll thread.
  if (PollThreadWorker::is_on_poll_thread()) {
    // On poll thread - set flag, poll loop will handle update_mode
    pending_write_update_.set(true);
  } else {
    // On user thread - use channel to notify poll thread
    poll_thread_worker_->update_mode(*this, Pollable::READ | Pollable::WRITE);
  }

  out_l_.get()->unlock();
}

// ============================================================================
// Client implementation (facade that delegates to ClientConnection)
// ============================================================================

// @safe - Cleanup destructor (has internal @unsafe blocks)
// SAFETY: Connection cleanup handled by ClientConnection
Client::~Client() {
  if (connection_.get()->is_some()) {
    // @unsafe - const_cast for interior mutability
    { const_cast<ClientConnection&>(*connection_.get()->as_ref().unwrap()).close(); }
  }
}

// Jetpack: set validity flag on output marshal
void Client::set_valid(bool valid) const {
  if (connection_.get()->is_some()) {
    const_cast<ClientConnection&>(*connection_.get()->as_ref().unwrap()).out_.valid_id = valid;
  }
}

// @safe - Closes socket and cleans up (has internal @unsafe blocks)
// SAFETY: Idempotent, delegates to ClientConnection
void Client::close() const {
  if (connection_.get()->is_some()) {
    // @unsafe - const_cast for interior mutability
    { const_cast<ClientConnection&>(*connection_.get()->as_ref().unwrap()).close(); }
  }
}

// Jetpack: handle_free for explicit future cleanup
void Client::handle_free(i64 xid) const {
  if (connection_.get()->is_some()) {
    const_cast<ClientConnection&>(*connection_.get()->as_ref().unwrap()).handle_free(xid);
  }
}

// Jetpack: pause/resume for flow control
void Client::pause() const {
  if (connection_.get()->is_some()) {
    const_cast<ClientConnection&>(*connection_.get()->as_ref().unwrap()).pause();
  }
}

void Client::resume() const {
  if (connection_.get()->is_some()) {
    const_cast<ClientConnection&>(*connection_.get()->as_ref().unwrap()).resume();
  }
}

// @unsafe - Establishes TCP/IPC connection to server
// SAFETY: Creates ClientConnection and connects
int Client::connect(const char* addr, bool client) const {
  // Create the ClientConnection
  auto conn = rusty::Arc<ClientConnection>::make(const_cast<Client*>(this), poll_thread_worker_);

  // Initialize weak self-reference for poll thread registration
  // const_cast is safe - we need to mutate the newly created connection
  const_cast<WeakClientConnection&>(conn->weak_self_) = conn;

  // Set client mode
  const_cast<bool&>(conn->is_client_mode_) = client;
  is_client_mode_.set(client);

  // Attempt to connect
  int result = const_cast<ClientConnection&>(*conn).connect(addr);

  if (result == 0) {
    // Connection successful, store it
    *connection_.get() = rusty::Some(std::move(conn));
  }

  return result;
}

// @unsafe - Begins RPC request with marshaling
// SAFETY: Delegates to ClientConnection
FutureResult Client::begin_request(i32 rpc_id, const FutureAttr& attr) const {
  if (connection_.get()->is_none()) {
    return FutureResult::Err(ENOTCONN);
  }

  rpc_id_.set(rpc_id);
  return const_cast<ClientConnection&>(*connection_.get()->as_ref().unwrap()).begin_request(rpc_id, attr);
}

// @unsafe - Completes request packet
// SAFETY: Delegates to ClientConnection
void Client::end_request() const {
  if (connection_.get()->is_some()) {
    const_cast<ClientConnection&>(*connection_.get()->as_ref().unwrap()).end_request();
  }
}

// ============================================================================
// ClientPool implementation
// ============================================================================

// @unsafe - Constructs pool with PollThread ownership
// SAFETY: Shared ownership of PollThread
ClientPool::ClientPool(rusty::Option<rusty::Arc<PollThread>> poll_thread_worker /* =? */,
                       int parallel_connections /* =? */)
    : parallel_connections_(parallel_connections) {

  verify(parallel_connections_ > 0);
  if (poll_thread_worker.is_none()) {
    poll_thread_worker_ = rusty::Some(PollThread::create());
  } else {
    poll_thread_worker_ = std::move(poll_thread_worker);
  }
}

// @unsafe - Destroys pool and all cached connections
// SAFETY: Closes all clients and releases PollThread
ClientPool::~ClientPool() {
  for (auto& it : cache_) {
    for (auto& client : it.second) {
      client->close();
    }
  }

  // Shutdown PollThread if we own it
  if (poll_thread_worker_.is_some()) {
    poll_thread_worker_.as_ref().unwrap()->shutdown();
  }
}

// @unsafe - Gets cached or creates new client connections
// SAFETY: Protected by spinlock, handles connection failures gracefully
rusty::Option<rusty::Arc<Client>> ClientPool::get_client(const string& addr) {
  rusty::Option<rusty::Arc<Client>> sp_cl = rusty::None;
  l_.lock();
  auto it = cache_.find(addr);
  if (it != cache_.end()) {
    sp_cl = rusty::Some(it->second[rand_() % parallel_connections_].clone());
  } else {
    std::vector<rusty::Arc<Client>> parallel_clients;
    bool ok = true;
    for (int i = 0; i < parallel_connections_; i++) {
      auto client = Client::create(this->poll_thread_worker_.as_ref().unwrap().clone());
      client->set_client_mode(true);  // Jetpack: mark as client
      if (client->connect(addr.c_str()) != 0) {
        ok = false;
        break;
      }
      parallel_clients.push_back(client);
    }
    if (ok) {
      sp_cl = rusty::Some(parallel_clients[rand_() % parallel_connections_].clone());
      cache_[addr] = std::move(parallel_clients);
    }
    // If not ok, parallel_clients automatically cleaned up by Arc
  }
  l_.unlock();
  return sp_cl;
}

} // namespace rrr
