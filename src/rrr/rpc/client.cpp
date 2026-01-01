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

// Note: External safety annotations for STL now in std_annotation.hpp (via rusty-cpp).
// Marshal, Log, SpinLock, PollThread, Reactor, Coroutine, and rusty-cpp types
// now have in-place annotations in their respective headers.
// Note: std::atomic public API (load, store, etc.) is annotated in event.h
//
// @external: {
//   const_cast: [unsafe]
//   std::__cxx11::basic_string::basic_string: [safe]
//   std::map::find: [safe]
//   std::map::erase: [safe]
//   std::map::end: [safe]
//   std::unordered_map::find: [safe]
//   std::unordered_map::erase: [safe]
//   std::unordered_map::end: [safe]
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
  if (!condition_became_false && !guard->ready) {
    guard->timed_out = true;
    error_code_.set(ETIMEDOUT);
  }
}

// @safe - Uses rusty::Mutex and rusty::Condvar together
void Future::notify_ready(rusty::Arc<Future> self) const {
  bool should_callback = false;
  {
    auto guard = state_.lock().unwrap();
    if (!guard->timed_out) {
      guard->ready = true;
    }
    should_callback = guard->ready;
  }  // Guard dropped here, releasing lock before notify

  ready_cond_.notify_all();

  // Execute callback outside lock to avoid deadlock
  if (should_callback && attr_.callback != nullptr) {
    auto x = attr_.callback;
    // Coroutine::CreateRun is now @safe
    Coroutine::CreateRun([x, self]() {
      x(self);
    }, __FILE__, __LINE__);
  }
}

// ============================================================================
// ClientConnection implementation
// ============================================================================

// @safe - Initializes connection (only stores references)
ClientConnection::ClientConnection(rusty::Arc<PollThread> poll_thread_worker)
    : poll_thread_worker_(poll_thread_worker),
      socket_(-1),
      status_(NEW) {
}

// @safe - Simple destructor
ClientConnection::~ClientConnection() {
  invalidate_pending_futures();
}

// @safe - Cancels all pending futures with error, protected by SpinMutex
void ClientConnection::invalidate_pending_futures() {
  list<rusty::Arc<Future>> futures;
  auto guard = pending_fu_.lock().unwrap();
  for (auto& it: *guard) {
    futures.push_back(it.second);  // Copy Arc
  }
  guard->clear();  // Clear map (releases its Arc references)
  // Guard dropped here, releasing lock

  for (auto& fu: futures) {
    fu->error_code_.set(ENOTCONN);
    fu->notify_ready(fu);  // Pass Arc to self for callback safety
    // Arc auto-released when list destroyed
  }
}

// @unsafe - Closes socket and invalidates futures (call from poll thread only)
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

// @safe - Jetpack: handle_free for explicit future cleanup
void ClientConnection::handle_free(i64 xid) const {
  auto guard = pending_fu_.lock().unwrap();
  auto it = guard->find(xid);
  if (it != guard->end()) {
    guard->erase(it);
    // Arc auto-released when removed from map
  }
  // Guard dropped here, releasing lock
}

// @unsafe - Establishes TCP/IPC connection to server
// Contains syscalls, raw pointers, and other unsafe operations
int ClientConnection::connect(const char* addr) {
  verify(status_ != CONNECTED);
  string addr_str(addr);
  size_t idx = addr_str.find(":");
  if (idx == string::npos) {
    Log_error("rrr::ClientConnection: bad connect address: %s", addr);
    return EINVAL;
  }
  // @unsafe { - string operations
  string host = addr_str.substr(0, idx);
  host_ = host;
  string port = addr_str.substr(idx + 1);
  // }

#ifdef USE_IPC
  // @unsafe { - IPC socket creation and connect syscalls
  struct sockaddr_un saun;
  saun.sun_family = AF_UNIX;
  string ipc_addr = "rsock" + port;
  strcpy(saun.sun_path, ipc_addr.data());
  int sock = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sock < 0) {
    Log_error("rrr::ClientConnection: socket() failed: %s", strerror(errno));
    return errno;
  }
  socket_ = sock;
  auto len = sizeof(saun.sun_family) + strlen(saun.sun_path)+1;
  if (::connect(socket_, (struct sockaddr*)&saun, len) < 0) {
    int err = errno;
    Log_error("rrr::ClientConnection: connect() failed: %s", strerror(err));
    ::close(socket_);
    socket_ = -1;
    return err;
  }
  // }
#else

  struct addrinfo hints;
  // @unsafe { - memset
  memset(&hints, 0, sizeof(struct addrinfo));
  // }

  hints.ai_family = AF_INET; // ipv4
  hints.ai_socktype = SOCK_STREAM; // tcp

  // Use AddrInfo RAII wrapper - automatically frees on scope exit
  auto addr_result = AddrInfo::resolve(host.c_str(), port.c_str(), &hints);
  if (addr_result.is_err()) {
    Log_error("rrr::ClientConnection: getaddrinfo(): %s", gai_strerror(addr_result.unwrap_err()));
    return EINVAL;
  }
  auto addr_info = addr_result.unwrap();

  // @unsafe { - TCP socket creation, options, and connect syscalls
  struct addrinfo* rp = nullptr;
  for (rp = addr_info.get(); rp != nullptr; rp = rp->ai_next) {
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
  // AddrInfo automatically freed when addr_info goes out of scope
  // }

  if (rp == nullptr) {
    // failed to connect
    return ENOTCONN;
  }
#endif

  // @unsafe - set_nonblocking syscall
  {
    verify(set_nonblocking(socket_, true) == 0);
  }

  Log_debug("rrr::ClientConnection: connected to %s", addr);

  status_ = CONNECTED;

  // Register with poll thread using weak_self_
  // @unsafe { - Weak::upgrade and PollThread::add
  auto self = weak_self_.upgrade();
  if (self.is_some()) {
    poll_thread_worker_->add(self.unwrap());
  // }
  } else {
    Log_error("rrr::ClientConnection: weak_self_ upgrade failed - connection may not have been created properly");
    return EINVAL;
  }

  return 0;
}

// @safe - Simple error handler
void ClientConnection::handle_error() {
  // @unsafe - calls close() which does system calls
  { close(); }
}

// @safe - Writes buffered data to socket, protected by SpinMutex
int ClientConnection::handle_write() {
  if (status_ != CONNECTED) {
    return PollMode::NO_CHANGE;
  }
  // Jetpack: respect pause state
  if (paused_.get()) return PollMode::NO_CHANGE;

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

// @safe - Reads and processes RPC responses
bool ClientConnection::handle_read() {
  if (status_ != CONNECTED) {
    return false;
  }

  int bytes_read = in_.read_from_fd(socket_);
  if (bytes_read == 0) {
    return false;
  }

  // Process all available packets
  while (status_ == CONNECTED) {
    i32 packet_size;
    int n_peek = in_.peek(packet_size);

    if (n_peek == sizeof(i32)
        && in_.content_size() >= packet_size + sizeof(i32)) {

      verify(in_.read(&packet_size, sizeof(i32)) == sizeof(i32));

      v64 v_reply_xid;
      v32 v_error_code;

      in_ >> v_reply_xid >> v_error_code;

      rusty::Option<rusty::Arc<Future>> fu_opt = rusty::None;
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
        fu->reply_.borrow_mut()->read_from_marshal(in_,
                                            packet_size - v_reply_xid.val_size()
                                                - v_error_code.val_size());

        fu->notify_ready(fu);  // Pass Arc to self for callback safety

        // Arc auto-released when scope exits (refcount 1→0 if user released theirs)
      } else {
        // the future might have timed out - consume data anyway
        Marshal reply;
        reply.read_from_marshal(in_,
                                packet_size - v_reply_xid.val_size()
                                - v_error_code.val_size());
      }
    } else {
      // No complete packet available
      break;
    }
  }

  Reactor::GetReactor()->Loop();

  return true;
}

// @safe - Determines polling mode based on output buffer, protected by SpinMutex
int ClientConnection::poll_mode() const {
  int mode = PollMode::READ;
  auto guard = out_.lock().unwrap();
  if (!guard->empty()) {
    mode |= PollMode::WRITE;
  }
  // Guard auto-unlocks here
  return mode;
}

// ============================================================================
// Client implementation (facade that delegates to ClientConnection)
// ============================================================================

// @safe - Cleanup destructor, uses request_close() for thread-safe close
Client::~Client() {
  close();  // Delegate to close() which uses request_close()
}

// @safe - Sets connection validity using RefCell
void Client::set_valid(bool valid) const {
  auto guard = connection_.borrow_mut();
  if (guard->is_some()) {
    auto out_guard = guard->as_mut().unwrap()->out_.lock().unwrap();
    out_guard->valid_id = valid;
  }
}

// @safe - Closes socket via request_close() for thread-safe cleanup
void Client::close() const {
  auto guard = connection_.borrow_mut();
  if (guard->is_some()) {
    auto& conn = *guard->as_ref().unwrap();
    if (conn.connected()) {
      // Request poll thread to close the connection
      // @unsafe - PollThread::request_close
      { poll_thread_worker_->request_close(conn.fd()); }
    }
    // Clear connection to prevent further use
    *guard = rusty::None;
  }
}

// @safe - Jetpack: handle_free for explicit future cleanup
void Client::handle_free(i64 xid) const {
  auto guard = connection_.borrow();
  if (guard->is_some()) {
    guard->as_ref().unwrap()->handle_free(xid);
  }
}

// @safe - Pauses the connection
void Client::pause() const {
  auto guard = connection_.borrow();
  if (guard->is_some()) {
    guard->as_ref().unwrap()->pause();
  }
}

// @safe - Resumes the connection
void Client::resume() const {
  auto guard = connection_.borrow();
  if (guard->is_some()) {
    guard->as_ref().unwrap()->resume();
  }
}

// @safe - Establishes TCP/IPC connection to server
// Uses Arc::get_mut() for exclusive mutable access during initialization
int Client::connect(const char* addr, bool client) const {
  // Create the ClientConnection
  auto conn = rusty::Arc<ClientConnection>::make(poll_thread_worker_);

  // Use get_mut() since we're the sole owner (strong_count == 1)
  // This is Rust's idiomatic pattern for init-before-sharing
  auto opt = conn.get_mut();
  verify(opt.is_some());  // Must succeed for freshly-created Arc
  ClientConnection& mut_conn = opt.unwrap();

  // Initialize fields through mutable reference (no const_cast needed)
  // @unsafe - Weak pointer assignment
  {
    mut_conn.weak_self_ = conn;
  }
  mut_conn.is_client_mode_ = client;
  is_client_mode_.set(client);

  // Call connect through mutable reference
  int result = 0;
  // @unsafe - Low-level TCP/IPC connection
  {
    result = mut_conn.connect(addr);
  }

  if (result == 0) {
    // Connection successful, store it
    auto guard = connection_.borrow_mut();
    *guard = rusty::Some(std::move(conn));
  }

  return result;
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
