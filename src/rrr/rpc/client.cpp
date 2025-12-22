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

// External safety annotations for STL and language features that cannot have in-place annotations
// Note: Marshal, Log, SpinLock, PollThread, Reactor, Coroutine, and rusty-cpp types
// now have in-place annotations in their respective headers.
// Note: std::atomic public API (load, store, etc.) is annotated in event.h
// @external: {
//   std::vector::push_back: [unsafe]
//   std::map::find: [unsafe]
//   std::map::end: [unsafe]
//   std::map::operator[]: [unsafe]
//   std::unordered_map::find: [unsafe]
//   std::unordered_map::end: [unsafe]
//   std::unordered_map::insert: [unsafe]
//   std::unordered_map::insert_or_assign: [unsafe]
//   std::unordered_map::erase: [unsafe]
//   std::unordered_map::clear: [unsafe]
//   operator!=: [unsafe]
//   operator==: [unsafe]
//   const_cast: [unsafe]
// }


using namespace std;

namespace rrr {

// ============================================================================
// Future implementation
// ============================================================================

// @safe - Uses rusty::Mutex and rusty::Condvar together
void Future::wait() const {
  auto guard = state_.lock().unwrap();
  // wait_while: waits WHILE condition is TRUE, stops when FALSE
  // We want to wait while NOT ready and NOT timed_out
  guard = ready_cond_.wait_while(std::move(guard), [](State& s) {
    return !s.ready && !s.timed_out;
  }).unwrap();
}

// @safe - Uses rusty::Mutex and rusty::Condvar together
void Future::timed_wait(double sec) const {
  auto guard = state_.lock().unwrap();
  auto duration = std::chrono::duration<double>(sec);
  // wait_timeout_while: waits WHILE condition is TRUE
  // Returns pair<Guard, bool> where bool = true if condition became false
  auto result = ready_cond_.wait_timeout_while(
    std::move(guard),
    duration,
    [](State& s) { return !s.ready && !s.timed_out; }
  ).unwrap();
  guard = std::move(result.first);
  bool condition_became_false = result.second;

  // If condition is still true (timed out while still waiting)
  if (!condition_became_false && !(*guard).ready) {
    (*guard).timed_out = true;
    error_code_.set(ETIMEDOUT);
  }
}

// @safe - Uses rusty::Mutex and rusty::Condvar together
void Future::notify_ready(rusty::Arc<Future> self) const {
  bool should_callback = false;
  {
    auto guard = state_.lock().unwrap();
    if (!(*guard).timed_out) {
      (*guard).ready = true;
    }
    should_callback = (*guard).ready;
  }  // Guard dropped here, releasing lock before notify

  ready_cond_.notify_all();

  // Execute callback outside lock to avoid deadlock
  if (should_callback && attr_.callback != nullptr) {
    auto x = attr_.callback;
    // @unsafe - Coroutine creation
    {
      Coroutine::CreateRun([x, self]() {
        x(self);
      }, __FILE__, __LINE__);
    }
  }
}

// ============================================================================
// ClientConnection implementation
// ============================================================================

// @safe - Initializes connection (only stores references)
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
// SAFETY: Protected by SpinMutex, proper refcount management
void ClientConnection::invalidate_pending_futures() {
  list<rusty::Arc<Future>> futures;
  // @unsafe - SpinMutex guard operations
  {
    auto guard = pending_fu_.lock().unwrap();
    for (auto& it: *guard) {
      futures.push_back(it.second);  // Copy Arc
    }
    guard->clear();  // Clear map (releases its Arc references)
  }  // Guard dropped here, releasing lock

  for (auto& fu: futures) {
    fu->error_code_.set(ENOTCONN);
    fu->notify_ready(fu);  // Pass Arc to self for callback safety
    // Arc auto-released when list destroyed
  }
}

// @unsafe - Closes socket and invalidates futures
// SAFETY: Should only be called from poll thread (via do_close_pollable or handle_error)
// Idempotent, proper cleanup sequence
void ClientConnection::close() {
  if (status_ == CONNECTED) {
    // @unsafe - system call
    {
      ::close(socket_);
    }
  }
  status_ = CLOSED;
  invalidate_pending_futures();
}

// @safe - Jetpack: handle_free for explicit future cleanup (calls @unsafe SpinMutex)
void ClientConnection::handle_free(i64 xid) {
  // @unsafe - SpinMutex guard operations
  {
    auto guard = pending_fu_.lock().unwrap();
    auto it = guard->find(xid);
    if (it != guard->end()) {
      guard->erase(it);
      // Arc auto-released when removed from map
    }
  }  // Guard dropped here, releasing lock
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
  // @unsafe - SpinLock and I/O operations
  {
    out_l_.get()->lock();
    out_.write_to_fd(socket_);
    if (out_.empty()) {
      // Return READ-only mode - PollThreadWorker will update epoll
      result = Pollable::READ;
    }
    out_l_.get()->unlock();
  }
  return result;
}

// @unsafe - Reads and processes RPC responses
// SAFETY: Contains raw pointer operations (GetReactor()->Loop())
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

// @unsafe - Reads data from socket (I/O system call)
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

// @unsafe - Processes packets from buffer
// SAFETY: Contains raw pointer operations (&packet_size, fu->reply_.get()->)
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

      rusty::Option<rusty::Arc<Future>> fu_opt = rusty::None;
      // @unsafe - SpinMutex guard operations
      {
        auto guard = pending_fu_.lock().unwrap();
        auto it = guard->find(v_reply_xid.get());
        if (it != guard->end()) {
          fu_opt = rusty::Some(it->second);  // Copy Arc (refcount still 2)
          guard->erase(it);  // Remove from map (refcount 2→1)
        }
      }  // Guard dropped here, releasing lock

      if (fu_opt.is_some()) {
        auto fu = fu_opt.unwrap();
        verify(fu->xid_ == v_reply_xid.get());

        fu->error_code_.set(v_error_code.get());
        fu->reply_.get()->read_from_marshal(in_,
                                            packet_size - v_reply_xid.val_size()
                                                - v_error_code.val_size());

        fu->notify_ready(fu);  // Pass Arc to self for callback safety

        // Arc auto-released when scope exits (refcount 1→0 if user released theirs)
      } else {
        // the future might timed out
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
  // @unsafe - SpinLock operations via UnsafeCell
  {
    out_l_.get()->lock();
    if (!out_.empty()) {
      mode |= Pollable::WRITE;
    }
    out_l_.get()->unlock();
  }
  return mode;
}

// @unsafe - Starts new RPC request with marshaling
// SAFETY: Contains raw pointer operations (out_l_.get()->lock())
FutureResult ClientConnection::begin_request(i32 rpc_id, const FutureAttr& attr /* =... */) {
  out_l_.get()->lock();

  if (status_ != CONNECTED) {
    out_l_.get()->unlock();
    return FutureResult::Err(ENOTCONN);
  }

  auto fu = Future::create(xid_counter_.next(), attr);

  // @unsafe - SpinMutex guard operations
  {
    auto guard = pending_fu_.lock().unwrap();
    guard->insert_or_assign(fu->xid_, fu);  // Store Arc in map (refcount now 2)
  }  // Guard dropped here

  // check if the connection gets closed in the meantime
  if (status_ != CONNECTED) {
    // @unsafe - SpinMutex guard operations
    {
      auto guard = pending_fu_.lock().unwrap();
      auto it = guard->find(fu->xid_);
      if (it != guard->end()) {
        guard->erase(it);  // Arc auto-released when removed from map
      }
    }  // Guard dropped here
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
// SAFETY: Contains raw pointer operations (&*bmark_)
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
// SAFETY: Connection cleanup handled via request_close() to poll thread
Client::~Client() {
  close();  // Delegate to close() which uses request_close()
}

// @unsafe - Uses const_cast for interior mutability
// SAFETY: Contains raw pointer dereference (connection_.get()->)
void Client::set_valid(bool valid) const {
  if (connection_.get()->is_some()) {
    const_cast<ClientConnection&>(*connection_.get()->as_ref().unwrap()).out_.valid_id = valid;
  }
}

// @safe - Closes socket and cleans up (has internal @unsafe blocks)
// SAFETY: Uses request_close() for thread-safe close via poll thread
void Client::close() const {
  if (connection_.get()->is_some()) {
    // @unsafe - pointer dereference through Arc
    {
      auto& conn = *connection_.get()->as_ref().unwrap();
      if (conn.connected()) {
        // Request poll thread to close the connection
        poll_thread_worker_->request_close(conn.fd());
      }
    }
    // Clear connection to prevent further use
    // @unsafe - const_cast for interior mutability
    { *connection_.get() = rusty::None; }
  }
}

// @unsafe - Uses const_cast for interior mutability
// SAFETY: Contains raw pointer cast
void Client::handle_free(i64 xid) const {
  if (connection_.get()->is_some()) {
    const_cast<ClientConnection&>(*connection_.get()->as_ref().unwrap()).handle_free(xid);
  }
}

// @unsafe - Uses const_cast for interior mutability
// SAFETY: Contains raw pointer cast
void Client::pause() const {
  if (connection_.get()->is_some()) {
    const_cast<ClientConnection&>(*connection_.get()->as_ref().unwrap()).pause();
  }
}

// @unsafe - Uses const_cast for interior mutability
// SAFETY: Contains raw pointer cast
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
// SAFETY: Contains raw pointer cast in return
FutureResult Client::begin_request(i32 rpc_id, const FutureAttr& attr) const {
  if (connection_.get()->is_none()) {
    return FutureResult::Err(ENOTCONN);
  }

  rpc_id_.set(rpc_id);
  return const_cast<ClientConnection&>(*connection_.get()->as_ref().unwrap()).begin_request(rpc_id, attr);
}

// @unsafe - Completes request packet
// SAFETY: Contains raw pointer cast
void Client::end_request() const {
  if (connection_.get()->is_some()) {
    const_cast<ClientConnection&>(*connection_.get()->as_ref().unwrap()).end_request();
  }
}

// ============================================================================
// ClientPool implementation
// ============================================================================

// @safe - Constructs pool with PollThread ownership
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

// @safe - Destroys pool and all cached connections
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
// SAFETY: Contains raw pointer dereference
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
