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

// Helper function to get current time in milliseconds
// @unsafe - Uses std::chrono which is not borrow-checked (but is memory-safe)
static uint64_t current_time_ms() {
    auto now = std::chrono::steady_clock::now();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count());
}

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

// @unsafe - Uses std::chrono which is not borrow-checked
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
    timeout_type_.set(TimeoutType::RESPONSE_TIMEOUT);
  }
}

// @unsafe - rusty-cpp false positive: should_callback IS initialized
void Future::notify_ready(rusty::Arc<Future> self) const {
  bool should_callback = false;  // Initialized here
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
    Coroutine::create_run([x, self]() {
      x(self);
    }, __FILE__, __LINE__);
  }
}

// ============================================================================
// ClientConnection implementation
// ============================================================================

// @safe - Initializes connection (only stores references)
// State machine defaults to NEW state
ClientConnection::ClientConnection(rusty::Arc<PollThread> poll_thread_worker)
    : poll_thread_worker_(poll_thread_worker),
      socket_(-1),
      state_machine_(),
      pending_queue_(buffering_config_.to_queue_config()) {
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
  if (state_machine_.is_connected()) {
    // Transition to DISCONNECTING state
    state_machine_.transition_to(ConnectionState::DISCONNECTING);
    // @unsafe - system call
    {
      ::close(socket_);
    }
    // Transition to DISCONNECTED state
    state_machine_.transition_to(ConnectionState::DISCONNECTED);
  } else if (!state_machine_.is_terminal()) {
    // If not connected and not already terminal, force to DISCONNECTED
    state_machine_.force_state(ConnectionState::DISCONNECTED);
  }
  invalidate_pending_futures();
}

// @safe - Mark connection as closing without closing socket
// Used by Client::close() to update state before poll thread closes socket
void ClientConnection::mark_closing() {
  if (state_machine_.is_connected()) {
    // Just transition state - don't close socket
    state_machine_.transition_to(ConnectionState::DISCONNECTING);
    state_machine_.transition_to(ConnectionState::DISCONNECTED);
  } else if (!state_machine_.is_terminal()) {
    state_machine_.force_state(ConnectionState::DISCONNECTED);
  }
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
  verify(!state_machine_.is_connected());

  // Transition to CONNECTING state
  if (!state_machine_.transition_to(ConnectionState::CONNECTING)) {
    Log_error("rrr::ClientConnection: cannot connect from state %s",
              connection_state_to_string(state_machine_.state()));
    return EINVAL;
  }
  string addr_str(addr);
  size_t idx = addr_str.find(":");
  if (idx == string::npos) {
    Log_error("rrr::ClientConnection: bad connect address: %s", addr);
    state_machine_.transition_to(ConnectionState::FAILED);
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
    state_machine_.transition_to(ConnectionState::FAILED);
    return ENOTCONN;
  }
#endif

  // @unsafe - set_nonblocking syscall
  {
    verify(set_nonblocking(socket_, true) == 0);
  }

  // Apply TCP keepalive options after socket is connected
  // @unsafe { setsockopt system calls }
  apply_keepalive_options();

  // Initialize last activity time and record connection in metrics
  auto now_ms = current_time_ms();
  update_last_activity(now_ms);
  metrics_.record_connect(now_ms);

  Log_debug("rrr::ClientConnection: connected to %s", addr);

  // Transition to CONNECTED state
  state_machine_.transition_to(ConnectionState::CONNECTED);

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

  // Store address for potential reconnection
  reconnect_address_ = addr;

  return 0;
}

// @unsafe - Attempts to reconnect to the last connected address
int ClientConnection::reconnect(std::function<void(bool)> on_complete) {
  // Check if we have an address to reconnect to
  if (reconnect_address_.empty()) {
    Log_error("rrr::ClientConnection: no address to reconnect to");
    if (on_complete) on_complete(false);
    return EINVAL;
  }

  // Can only reconnect from FAILED or DISCONNECTED state
  if (!state_machine_.can_connect()) {
    Log_error("rrr::ClientConnection: cannot reconnect from state %s",
              connection_state_to_string(state_machine_.state()));
    if (on_complete) on_complete(false);
    return EINVAL;
  }

  // Mark as reconnecting
  reconnecting_.set(true);

  // Reset socket for reconnection
  socket_ = -1;

  // Attempt to connect
  int result = connect(reconnect_address_.c_str());

  reconnecting_.set(false);

  if (result == 0) {
    Log_info("rrr::ClientConnection: reconnected to %s", reconnect_address_.c_str());

    // Record reconnection in metrics
    metrics_.record_reconnect();

    // Replay any queued requests
    size_t replayed = replay_pending_requests();
    if (replayed > 0) {
      Log_info("rrr::ClientConnection: replayed %zu pending requests", replayed);
    }

    if (on_complete) on_complete(true);
  } else {
    Log_error("rrr::ClientConnection: reconnection failed to %s: %d",
              reconnect_address_.c_str(), result);
    if (on_complete) on_complete(false);
  }

  return result;
}

// @unsafe - Uses interior mutability (const method modifying mutable members)
void ClientConnection::set_buffering_config(const BufferingConfig& config) const {
  // @unsafe - struct assignment operator
  { buffering_config_ = config; }

  // Clear any pending requests since config changed
  // Note: We can't recreate the queue (mutex not movable), so just clear
  // @unsafe - const propagation through mutable member
  {
    if (!pending_queue_.empty()) {
      pending_queue_.clear_all(ECONNABORTED);
    }

    // Update the queue's internal config to match
    pending_queue_.update_config(config.to_queue_config());
  }
}

// @unsafe - Uses RequestQueue methods (not borrow-checked)
size_t ClientConnection::replay_pending_requests() {
  size_t replayed = 0;

  while (true) {
    auto req_opt = pending_queue_.dequeue();
    if (req_opt.is_none()) break;

    auto req = req_opt.unwrap();

    // Check if expired
    if (req.is_expired()) {
      if (req.callback) req.callback(-2);  // Expired error code
      continue;
    }

    // Check if still connected
    if (!state_machine_.is_connected()) {
      // Connection lost during replay, re-queue
      // Note: This could lead to infinite loop if connection keeps failing
      // The TTL will eventually expire stale requests
      pending_queue_.enqueue(std::move(req));
      break;
    }

    // Copy payload to output buffer
    if (req.payload && req.payload->content_size() > 0) {
      auto guard = out_.lock().unwrap();
      guard->read_from_marshal(*req.payload, req.payload->content_size());
      // Reset write_cnt_ so that subsequent set_bookmark() calls work
      // The replayed payload already contains the packet size
      guard->get_and_reset_write_cnt();
      replayed++;
    }
  }

  // Trigger write if we replayed any requests
  if (replayed > 0) {
    if (PollThreadWorker::is_on_poll_thread()) {
      pending_write_update_.set(true);
    } else {
      poll_thread_worker_->update_mode(*this, PollMode::READ | PollMode::WRITE);
    }
  }

  return replayed;
}

// @safe - Error handler - transitions to FAILED state
void ClientConnection::handle_error() {
  // Force transition to FAILED state (from any state)
  state_machine_.force_state(ConnectionState::FAILED);
  // @unsafe - calls close() which does system calls
  { close(); }
}

// @safe - Writes buffered data to socket, protected by SpinMutex
int ClientConnection::handle_write() {
  if (!state_machine_.is_connected()) {
    return PollMode::NO_CHANGE;
  }
  // Jetpack: respect pause state
  if (paused_.get()) return PollMode::NO_CHANGE;

  int result = PollMode::NO_CHANGE;
  auto guard = out_.lock().unwrap();
  size_t before_size = guard->content_size();
  guard->write_to_fd(socket_);
  size_t after_size = guard->content_size();

  // Update activity timestamp and metrics if we wrote any data
  if (after_size < before_size) {
    size_t bytes_written = before_size - after_size;
    update_last_activity(current_time_ms());
    metrics_.record_bytes_sent(bytes_written);
  }

  if (guard->empty()) {
    // Return READ-only mode - PollThreadWorker will update epoll
    result = PollMode::READ;
  }
  // Guard auto-unlocks here
  return result;
}

// @unsafe - Calls Future::notify_ready (uses interior mutability)
bool ClientConnection::handle_read() {
  if (!state_machine_.is_connected()) {
    return false;
  }

  int bytes_read = in_.read_from_fd(socket_);
  if (bytes_read == 0) {
    return false;
  }

  // Update activity timestamp and metrics on successful read
  update_last_activity(current_time_ms());
  if (bytes_read > 0) {
    metrics_.record_bytes_received(static_cast<uint64_t>(bytes_read));
  }

  // Process all available packets
  while (state_machine_.is_connected()) {
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

        // Record request completion in metrics
        if (v_error_code.get() == 0) {
          metrics_.record_request_completed();
        } else {
          metrics_.record_request_failed();
        }

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

  Reactor::get_reactor()->loop();

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
// Note: Does NOT clear the connection object so reconnect() can work.
// The connection object retains the address for reconnection.
// IMPORTANT: Does NOT call conn.close() directly! The socket close must happen
// in the poll thread to avoid race conditions with pending CmdAddPollable commands.
void Client::close() const {
  auto guard = connection_.borrow_mut();
  if (guard->is_some()) {
    auto& conn = const_cast<ClientConnection&>(*guard->as_ref().unwrap());
    if (conn.connected()) {
      // Request poll thread to close the connection
      // The poll thread will call ClientConnection::close() via do_close_pollable()
      // @unsafe - PollThread::request_close
      { poll_thread_worker_->request_close(conn.fd()); }
      // Just update state machine - don't close socket here
      // The actual socket close happens in the poll thread
      conn.mark_closing();
    }
    // Don't clear connection to None - we need it for reconnect()
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

  // Apply pending keepalive config before connecting
  mut_conn.set_keepalive(pending_keepalive_config_.get());

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

// @unsafe - Attempts to reconnect to the last connected address
int Client::reconnect(std::function<void(bool)> on_complete) const {
  auto guard = connection_.borrow();
  if (guard->is_none()) {
    Log_error("rrr::Client: no connection to reconnect");
    if (on_complete) on_complete(false);
    return ENOTCONN;
  }

  // Need to get mutable access to the connection for reconnect
  auto& conn = const_cast<ClientConnection&>(*guard->as_ref().unwrap());

  // @unsafe - reconnect does socket operations
  {
    return conn.reconnect(on_complete);
  }
}


// ============================================================================
// ClientPool implementation
// ============================================================================

// @safe - Constructs pool with PollThread ownership
ClientPool::ClientPool(rusty::Option<rusty::Arc<PollThread>> poll_thread_worker /* =? */,
                       const PoolConfig& config /* =? */)
    : config_(config) {

  verify(config.min_connections > 0);
  verify(config.max_connections >= config.min_connections);
  if (poll_thread_worker.is_none()) {
    poll_thread_worker_ = rusty::Some(PollThread::create());
  } else {
    poll_thread_worker_ = std::move(poll_thread_worker);
  }
}

// @safe - Set pool configuration
void ClientPool::set_pool_config(const PoolConfig& config) {
  config_.set(config);
}

// @safe - Get current pool configuration
PoolConfig ClientPool::pool_config() const {
  return config_.get();
}

// @safe - Check if a client is considered healthy
bool ClientPool::is_client_healthy(const rusty::Arc<Client>& client) const {
  auto cfg = config_.get();

  // If health checking is disabled, all clients are considered healthy
  if (!cfg.health_check_enabled) {
    return true;
  }

  // Must be connected to be healthy
  if (!client->connected()) {
    return false;
  }

  // Check metrics-based health
  const auto& metrics = client->metrics();
  auto requests_sent = metrics.requests_sent();

  // Not enough data to judge health
  if (requests_sent < cfg.min_requests_for_health) {
    return true;  // Assume healthy until proven otherwise
  }

  // Check success rate
  auto success_rate = metrics.success_rate_percent();
  return success_rate >= cfg.unhealthy_threshold_percent;
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

// @unsafe - Uses SpinLock (lock/unlock not borrow-checked)
size_t ClientPool::get_healthy_client_count(const std::string& addr) {
  l_.lock();
  size_t count = 0;
  auto it = cache_.find(addr);
  if (it != cache_.end()) {
    for (const auto& client : it->second) {
      if (is_client_healthy(client)) {
        count++;
      }
    }
  }
  l_.unlock();
  return count;
}

// @unsafe - Uses SpinLock (lock/unlock not borrow-checked)
size_t ClientPool::remove_unhealthy_clients(const std::string& addr) {
  l_.lock();
  size_t removed = 0;
  auto it = cache_.find(addr);
  if (it != cache_.end()) {
    auto& clients = it->second;
    auto cfg = config_.get();

    // Remove unhealthy clients, but keep at least min_connections
    auto new_end = std::remove_if(clients.begin(), clients.end(),
      [this, &removed, &clients, &cfg](const rusty::Arc<Client>& client) {
        // Keep if we're at minimum connections
        if (clients.size() - removed <= static_cast<size_t>(cfg.min_connections)) {
          return false;
        }
        if (!is_client_healthy(client)) {
          client->close();
          removed++;
          return true;
        }
        return false;
      });
    clients.erase(new_end, clients.end());

    // Remove empty entries from cache
    if (clients.empty()) {
      cache_.erase(it);
    }
  }
  l_.unlock();
  return removed;
}

// @unsafe - Uses SpinLock (lock/unlock not borrow-checked)
size_t ClientPool::close_idle_clients(const std::string& addr, uint64_t current_time_ms) {
  l_.lock();
  size_t closed = 0;
  auto cfg = config_.get();

  // If idle timeout is 0, no timeout
  if (cfg.idle_timeout_ms == 0) {
    l_.unlock();
    return 0;
  }

  auto it = cache_.find(addr);
  if (it != cache_.end()) {
    auto& clients = it->second;

    auto new_end = std::remove_if(clients.begin(), clients.end(),
      [this, &closed, &clients, &cfg, current_time_ms](const rusty::Arc<Client>& client) {
        // Keep if we're at minimum connections
        if (clients.size() - closed <= static_cast<size_t>(cfg.min_connections)) {
          return false;
        }
        // Check if idle
        if (client->is_idle(cfg.idle_timeout_ms, current_time_ms)) {
          client->close();
          closed++;
          return true;
        }
        return false;
      });
    clients.erase(new_end, clients.end());

    if (clients.empty()) {
      cache_.erase(it);
    }
  }
  l_.unlock();
  return closed;
}

// @unsafe - Uses SpinLock (lock/unlock not borrow-checked)
size_t ClientPool::remove_all_unhealthy() {
  l_.lock();
  size_t total_removed = 0;
  auto cfg = config_.get();

  for (auto it = cache_.begin(); it != cache_.end(); ) {
    auto& clients = it->second;
    size_t removed = 0;

    auto new_end = std::remove_if(clients.begin(), clients.end(),
      [this, &removed, &clients, &cfg](const rusty::Arc<Client>& client) {
        if (clients.size() - removed <= static_cast<size_t>(cfg.min_connections)) {
          return false;
        }
        if (!is_client_healthy(client)) {
          client->close();
          removed++;
          return true;
        }
        return false;
      });
    clients.erase(new_end, clients.end());
    total_removed += removed;

    if (clients.empty()) {
      it = cache_.erase(it);
    } else {
      ++it;
    }
  }
  l_.unlock();
  return total_removed;
}

// @unsafe - Uses SpinLock (lock/unlock not borrow-checked)
size_t ClientPool::close_all_idle(uint64_t current_time_ms) {
  l_.lock();
  size_t total_closed = 0;
  auto cfg = config_.get();

  if (cfg.idle_timeout_ms == 0) {
    l_.unlock();
    return 0;
  }

  for (auto it = cache_.begin(); it != cache_.end(); ) {
    auto& clients = it->second;
    size_t closed = 0;

    auto new_end = std::remove_if(clients.begin(), clients.end(),
      [this, &closed, &clients, &cfg, current_time_ms](const rusty::Arc<Client>& client) {
        if (clients.size() - closed <= static_cast<size_t>(cfg.min_connections)) {
          return false;
        }
        if (client->is_idle(cfg.idle_timeout_ms, current_time_ms)) {
          client->close();
          closed++;
          return true;
        }
        return false;
      });
    clients.erase(new_end, clients.end());
    total_closed += closed;

    if (clients.empty()) {
      it = cache_.erase(it);
    } else {
      ++it;
    }
  }
  l_.unlock();
  return total_closed;
}

// @unsafe - Uses SpinLock (lock/unlock not borrow-checked)
size_t ClientPool::total_client_count() {
  l_.lock();
  size_t count = 0;
  for (const auto& it : cache_) {
    count += it.second.size();
  }
  l_.unlock();
  return count;
}

// @unsafe - Uses SpinLock (lock/unlock not borrow-checked)
size_t ClientPool::address_count() {
  l_.lock();
  size_t count = cache_.size();
  l_.unlock();
  return count;
}

// @unsafe - Reconnects all clients for a specific address
ClientPool::BulkReconnectResult ClientPool::reconnect_all(
    const std::string& addr, const BulkReconnectConfig& config) {

  BulkReconnectResult result{0, 0, 0, 0};

  // Collect clients to reconnect
  std::vector<rusty::Arc<Client>> clients_to_reconnect;
  {
    l_.lock();
    auto it = cache_.find(addr);
    if (it != cache_.end()) {
      for (const auto& client : it->second) {
        auto state = client->connection_state();
        if (config.skip_connected && state == ConnectionState::CONNECTED) {
          result.skipped++;
        } else {
          clients_to_reconnect.push_back(client);
        }
      }
    }
    l_.unlock();
  }

  result.total = clients_to_reconnect.size() + result.skipped;

  // Reconnect in batches with rate limiting
  size_t i = 0;
  while (i < clients_to_reconnect.size()) {
    // Process a batch
    size_t batch_end = std::min(i + config.max_concurrent,
                                clients_to_reconnect.size());

    // Track reconnection results for this batch
    std::vector<std::atomic<int>> batch_results(batch_end - i);
    for (auto& r : batch_results) r.store(-1);

    // Start async reconnections
    for (size_t j = i; j < batch_end; j++) {
      size_t idx = j - i;
      clients_to_reconnect[j]->reconnect([&batch_results, idx](bool success) {
        batch_results[idx].store(success ? 0 : 1);
      });
    }

    // Wait for batch to complete (simple polling)
    bool all_done = false;
    while (!all_done) {
      all_done = true;
      for (const auto& r : batch_results) {
        if (r.load() == -1) {
          all_done = false;
          break;
        }
      }
      if (!all_done) {
        // @unsafe { nanosleep }
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = 1000000;  // 1ms
        nanosleep(&ts, nullptr);
      }
    }

    // Count results
    for (const auto& r : batch_results) {
      if (r.load() == 0) {
        result.succeeded++;
      } else {
        result.failed++;
      }
    }

    i = batch_end;

    // Delay between batches
    if (config.delay_between_ms > 0 && i < clients_to_reconnect.size()) {
      // @unsafe { nanosleep }
      struct timespec ts;
      ts.tv_sec = config.delay_between_ms / 1000;
      ts.tv_nsec = (config.delay_between_ms % 1000) * 1000000;
      nanosleep(&ts, nullptr);
    }
  }

  return result;
}

// @unsafe - Reconnects all clients across all addresses
ClientPool::BulkReconnectResult ClientPool::reconnect_all(const BulkReconnectConfig& config) {
  BulkReconnectResult total_result{0, 0, 0, 0};

  // Get list of addresses
  std::vector<std::string> addresses;
  {
    l_.lock();
    for (const auto& kv : cache_) {
      addresses.push_back(kv.first);
    }
    l_.unlock();
  }

  // Reconnect each address
  for (const auto& addr : addresses) {
    auto result = reconnect_all(addr, config);
    total_result.total += result.total;
    total_result.succeeded += result.succeeded;
    total_result.failed += result.failed;
    total_result.skipped += result.skipped;
  }

  return total_result;
}

// @unsafe - Gets cached or creates new client connections
// Now includes health checking, automatic reconnection, and load balancing
rusty::Option<rusty::Arc<Client>> ClientPool::get_client(const string& addr) {
  rusty::Option<rusty::Arc<Client>> sp_cl = rusty::None;
  auto cfg = config_.get();
  int num_connections = cfg.min_connections;

  l_.lock();

  // Get or create load balancer state for this address
  auto& lb_state = lb_state_[addr];

  auto it = cache_.find(addr);
  if (it != cache_.end()) {
    auto& clients = it->second;
    int client_count = static_cast<int>(clients.size());

    // Use load balancer to select starting index
    size_t start_idx = LoadBalancer::select(
        cfg.load_balancing,
        clients,
        lb_state,
        rand_()
    );

    for (int i = 0; i < client_count; i++) {
      int idx = (start_idx + i) % client_count;
      auto& client = clients[idx];

      // Check if client is connected and healthy
      if (client->connected() && is_client_healthy(client)) {
        sp_cl = rusty::Some(client.clone());
        break;
      }

      // Try to reconnect failed/disconnected clients
      auto state = client->connection_state();
      if (state == ConnectionState::FAILED || state == ConnectionState::DISCONNECTED) {
        Log_info("ClientPool: client to %s in state %s, attempting reconnect",
                 addr.c_str(), connection_state_to_string(state));
        if (client->try_reconnect_if_needed()) {
          Log_info("ClientPool: reconnected to %s successfully", addr.c_str());
          sp_cl = rusty::Some(client.clone());
          break;
        } else {
          Log_warn("ClientPool: reconnect to %s failed", addr.c_str());
        }
      }
    }

    // If no healthy client found after trying reconnects, recreate all connections
    if (sp_cl.is_none()) {
      Log_info("ClientPool: all clients to %s failed, recreating connections", addr.c_str());
      // Close old connections
      for (auto& client : clients) {
        client->close();
      }
      clients.clear();

      // Create new connections (use min_connections)
      bool ok = true;
      for (int i = 0; i < num_connections; i++) {
        auto client = Client::create(this->poll_thread_worker_.as_ref().unwrap().clone());
        client->set_client_mode(true);
        if (client->connect(addr.c_str()) != 0) {
          Log_warn("ClientPool: failed to create new connection to %s", addr.c_str());
          ok = false;
          break;
        }
        clients.push_back(client);
      }

      if (ok && !clients.empty()) {
        sp_cl = rusty::Some(clients[rand_() % clients.size()].clone());
      } else {
        // Remove from cache if we can't connect
        cache_.erase(it);
      }
    }
  } else {
    // No cached connections - create new ones
    std::vector<rusty::Arc<Client>> parallel_clients;
    bool ok = true;
    for (int i = 0; i < num_connections; i++) {
      auto client = Client::create(this->poll_thread_worker_.as_ref().unwrap().clone());
      client->set_client_mode(true);  // Jetpack: mark as client
      if (client->connect(addr.c_str()) != 0) {
        ok = false;
        break;
      }
      parallel_clients.push_back(client);
    }
    if (ok) {
      sp_cl = rusty::Some(parallel_clients[rand_() % parallel_clients.size()].clone());
      cache_[addr] = std::move(parallel_clients);
    }
    // If not ok, parallel_clients automatically cleaned up by Arc
  }
  l_.unlock();
  return sp_cl;
}

// @unsafe - Apply TCP keepalive options to socket
void ClientConnection::apply_keepalive_options() {
  if (socket_ < 0) {
    return;
  }

  auto config = keepalive_config_.get();
  if (!config.enabled) {
    // Disable keepalive
    int no = 0;
    // @unsafe { setsockopt system call }
    setsockopt(socket_, SOL_SOCKET, SO_KEEPALIVE, &no, sizeof(no));
    return;
  }

  // Enable keepalive
  int yes = 1;
  // @unsafe { setsockopt system calls }
  if (setsockopt(socket_, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes)) != 0) {
    Log_warn("Failed to enable SO_KEEPALIVE: %s", strerror(errno));
    return;
  }

#ifdef __linux__
  // Linux-specific TCP keepalive parameters
  if (setsockopt(socket_, IPPROTO_TCP, TCP_KEEPIDLE,
                 &config.idle_sec, sizeof(int)) != 0) {
    Log_warn("Failed to set TCP_KEEPIDLE: %s", strerror(errno));
  }
  if (setsockopt(socket_, IPPROTO_TCP, TCP_KEEPINTVL,
                 &config.interval_sec, sizeof(int)) != 0) {
    Log_warn("Failed to set TCP_KEEPINTVL: %s", strerror(errno));
  }
  if (setsockopt(socket_, IPPROTO_TCP, TCP_KEEPCNT,
                 &config.count, sizeof(int)) != 0) {
    Log_warn("Failed to set TCP_KEEPCNT: %s", strerror(errno));
  }
#elif defined(__APPLE__)
  // macOS uses TCP_KEEPALIVE for idle time (equivalent to TCP_KEEPIDLE)
  if (setsockopt(socket_, IPPROTO_TCP, TCP_KEEPALIVE,
                 &config.idle_sec, sizeof(int)) != 0) {
    Log_warn("Failed to set TCP_KEEPALIVE: %s", strerror(errno));
  }
  // macOS doesn't have TCP_KEEPINTVL or TCP_KEEPCNT via setsockopt
#endif

  Log_debug("TCP keepalive configured: idle=%ds, interval=%ds, count=%d",
            config.idle_sec, config.interval_sec,
            config.count);
}

// @unsafe - Validate connection is alive using getsockopt
bool ClientConnection::validate_connection() const {
  // Check 1: State machine says CONNECTED
  if (!state_machine_.is_connected()) {
    return false;
  }

  // Check 2: Socket is valid
  if (socket_ < 0) {
    return false;
  }

  // Check 3: No pending socket error (getsockopt SO_ERROR)
  int error = 0;
  socklen_t len = sizeof(error);
  // @unsafe { getsockopt system call }
  if (getsockopt(socket_, SOL_SOCKET, SO_ERROR, &error, &len) != 0) {
    // getsockopt itself failed
    Log_warn("getsockopt(SO_ERROR) failed: %s", strerror(errno));
    return false;
  }

  if (error != 0) {
    Log_warn("Socket has pending error: %s", strerror(error));
    return false;
  }

  return true;
}

} // namespace rrr
