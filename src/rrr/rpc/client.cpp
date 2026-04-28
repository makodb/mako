
// import std; replacement — see <std_compat.hpp> for rationale.
#include <std_compat.hpp>

// @c-compat-added
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <rusty/rusty.hpp>
#include <rusty/result.hpp>
#include <rusty/cell.hpp>
#include <rusty/refcell.hpp>
#include <rusty/async.hpp>


#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <netdb.h>
#include <netinet/tcp.h>


#include "client.hpp"


#include "../rrr.hpp"

// Note: External safety annotations for STL now in std_annotation.hpp (via rusty-cpp).
// Marshal, Log, SpinLock, PollThread, Reactor, Fiber, and rusty-cpp types
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
// Workstream K, sub-leaf 4g2 — migration switch flipped to default-on.
// ============================================================================
//
// Channel mode is now the **default** for new connections.
// `Client::connect` automatically installs a default TCP-backed
// `ChannelFactoryProxy` if the caller hasn't already installed one
// via `set_channel_factory(...)`. The new connection then runs
// entirely in channel mode (factory connect -> bind_channel_direct ->
// inline on_frame callbacks; no recv-loop fiber per 4g1c).
//
// Opt-out: set `SRPC_DISABLE_CHANNEL=1` (or `true`/`yes`/`on`) in the
// environment to force the legacy fd path. This is an emergency
// rollback for callers that hit unexpected regressions; the legacy
// path is removed entirely in sub-leaf 4g3.
//
// Legacy compatibility: the old `SRPC_USE_CHANNEL=1` env var is
// honored as a no-op alias (channel mode is already default), but the
// kill-switch is now `SRPC_DISABLE_CHANNEL=1`. Tests that explicitly
// set `SRPC_USE_CHANNEL=0` to disable channel mode should migrate to
// `SRPC_DISABLE_CHANNEL=1`.
//
// The query is cached in a Cell on first use; subsequent calls are
// lock-free reads. The override (set via
// `srpc_set_use_channel_for_testing`) lets unit tests flip the switch
// without spawning a child process — it ALSO flips the cached value.
namespace {

enum class UseChannelChoice : int { Unset = -1, Off = 0, On = 1 };

// Cached resolution of the env var. Reads `SRPC_DISABLE_CHANNEL` once,
// then memoizes; later changes to the env var are intentionally
// ignored (the channel-mode decision is per-process, not per-call,
// to keep call sites consistent across the connection lifecycle).
rusty::Cell<UseChannelChoice> g_use_channel_choice{UseChannelChoice::Unset};

// @unsafe - Reads getenv (not borrow-checked); strncasecmp.
bool resolve_srpc_use_channel_from_env() {
    auto eq_ci = [](const char* a, const char* b) {
        return ::strcasecmp(a, b) == 0;
    };
    auto is_truthy = [&](const char* val) {
        if (val == nullptr || val[0] == '\0') return false;
        return eq_ci(val, "1") || eq_ci(val, "true") || eq_ci(val, "yes")
               || eq_ci(val, "on");
    };
    // 4g2: opt-out. If SRPC_DISABLE_CHANNEL is truthy, use legacy.
    // @unsafe { std::getenv }
    const char* disable = std::getenv("SRPC_DISABLE_CHANNEL");
    if (is_truthy(disable)) return false;
    // Channel mode is now the default. The old SRPC_USE_CHANNEL env
    // var is honored for backward compatibility but is functionally a
    // no-op (channel is already on); we do NOT honor `SRPC_USE_CHANNEL=0`
    // as a disable switch — use SRPC_DISABLE_CHANNEL=1 for that.
    return true;
}

}  // namespace

// @safe - Reads cached choice; lazy-initializes from env on first call.
bool srpc_use_channel() {
    UseChannelChoice cached = g_use_channel_choice.get();
    if (cached != UseChannelChoice::Unset) {
        return cached == UseChannelChoice::On;
    }
    bool env = resolve_srpc_use_channel_from_env();
    g_use_channel_choice.set(env ? UseChannelChoice::On
                                 : UseChannelChoice::Off);
    return env;
}

// @safe - Test-only override; replaces the cached value.
void srpc_set_use_channel_for_testing(bool on) {
    g_use_channel_choice.set(on ? UseChannelChoice::On
                                : UseChannelChoice::Off);
}

// @safe - Test-only reset; forces re-resolution from env on next call.
void srpc_reset_use_channel_for_testing() {
    g_use_channel_choice.set(UseChannelChoice::Unset);
}

// ============================================================================
// Future implementation
// ============================================================================

// @unsafe - Uses rusty::Mutex and rusty::Condvar together
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
  rusty::Vec<std::function<void()>> completion_callbacks;
  {
    auto guard = state_.lock().unwrap();
    if (!guard->timed_out) {
      guard->ready = true;
    }
    should_callback = guard->ready;
    completion_callbacks = std::move(guard->completion_callbacks);
  }  // Guard dropped here, releasing lock before notify

  ready_cond_.notify_all();

  for (auto& callback : completion_callbacks) {
    if (callback != nullptr) {
      callback();
    }
  }

  // Execute callback outside lock to avoid deadlock
  if (should_callback && attr_.callback != nullptr) {
    auto x = attr_.callback;
    x(self);
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
      heartbeat_manager_(HeartbeatConfig::disabled()),
      circuit_breaker_(CircuitBreakerConfig::disabled()),
      callback_manager_(rusty::Arc<CallbackManager>::make()),
      pending_queue_(buffering_config_.to_queue_config()) {
}

// @safe - Simple destructor
ClientConnection::~ClientConnection() {
  reconnect_abort_.store(true, std::memory_order_release);
  reconnecting_.store(false, std::memory_order_release);
  invalidate_pending_futures();
}

// @unsafe - Cancels all pending futures with error, protected by SpinMutex
void ClientConnection::invalidate_pending_futures() {
  list<rusty::Arc<Future>> futures;
  auto guard = pending_fu_.lock().unwrap();
  for (auto it: *guard) {
    futures.push_back(it.second);  // Copy Arc
  }
  guard->clear();  // Clear map (releases its Arc references)
  // Guard dropped here, releasing lock

  for (auto& fu: futures) {
    metrics_.record_request_dropped();
    fu->error_code_.set(ENOTCONN);
    fu->notify_ready(fu);  // Pass Arc to self for callback safety
    // Arc auto-released when list destroyed
  }
}

// @unsafe - Fails one pending future if it still exists in the pending map
void ClientConnection::fail_pending_future(i64 xid, int err) const {
  rusty::Option<rusty::Arc<Future>> fu_opt = rusty::None;
  {
    auto pending_guard = pending_fu_.lock().unwrap();
    auto fu_ptr = pending_guard->get(xid);
    if (fu_ptr.is_some()) {
      fu_opt = rusty::Some((*fu_ptr.unwrap()).clone());  // Copy Arc before remove
      pending_guard->remove(xid);
    }
  }  // Drop lock before notifying callback/future waiters

  if (fu_opt.is_some()) {
    auto fu = fu_opt.unwrap();
    metrics_.record_request_dropped();
    fu->error_code_.set(err);
    // @unsafe - Future::notify_ready uses interior mutability + callback execution.
    { fu->notify_ready(fu); }
  }
}

// @unsafe - Closes socket and invalidates futures (call from poll thread only)
void ClientConnection::close() {
  ConnectionState prev_state = state_machine_.state();
  const bool was_connected = state_machine_.is_connected();
  if (was_connected) {
    // Transition to DISCONNECTING state while preserving normal lifecycle semantics.
    state_machine_.transition_to(ConnectionState::DISCONNECTING);
  }

  // Always close the socket when it is valid, regardless of state.
  if (socket_ >= 0) {
    // @unsafe - system call
    {
      ::close(socket_);
    }
    socket_ = -1;
  }

  if (was_connected) {
    // Transition to DISCONNECTED state for clean shutdown.
    state_machine_.transition_to(ConnectionState::DISCONNECTED);
  } else if (!state_machine_.is_terminal()) {
    // If not connected and not already terminal, force to DISCONNECTED.
    state_machine_.force_state(ConnectionState::DISCONNECTED);
  }
  heartbeat_manager_.reset();
  invalidate_pending_futures();

  if (prev_state == ConnectionState::CONNECTED ||
      prev_state == ConnectionState::DISCONNECTING) {
    invoke_disconnected_callback();
  }
}

// @unsafe - Mark connection as closing without closing socket
// Used by Client::close() to update state before poll thread closes socket
void ClientConnection::mark_closing() {
  reconnect_abort_.store(true, std::memory_order_release);
  if (state_machine_.is_connected()) {
    // Mark as in-progress close, but do not enter terminal state yet.
    // The poll-thread close callback performs the actual fd close and final state transition.
    state_machine_.transition_to(ConnectionState::DISCONNECTING);
  }
  invalidate_pending_futures();
}

// @unsafe - Jetpack: handle_free for explicit future cleanup
void ClientConnection::handle_free(i64 xid) const {
  auto guard = pending_fu_.lock().unwrap();
  if (guard->remove(xid).is_some()) {
    metrics_.record_request_dropped();
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
    invoke_error_callback(EINVAL, "invalid state for connect");
    return EINVAL;
  }

  // Workstream K, sub-leaf 4e — factory-driven connect.
  //
  // If a `ChannelFactoryProxy` has been bound, route through it
  // instead of the legacy socket(2) + connect(2) path. The factory
  // returns a fully-wired `ChannelConnectionProxy` that we then
  // hand to `bind_channel(...)` so the connection enters channel
  // mode automatically. `reconnect_address_` is recorded the same
  // way the legacy path does it, so the close fan-out's reconnect
  // spawn can re-call the factory with the same target.
  if (is_factory_bound()) {
    int rc = connect_via_factory(addr);
    return rc;
  }

  string addr_str(addr);
  size_t idx = addr_str.find(":");
  if (idx == string::npos) {
    Log_error("rrr::ClientConnection: bad connect address: %s", addr);
    state_machine_.transition_to(ConnectionState::FAILED);
    invoke_error_callback(EINVAL, "invalid connect address");
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
    invoke_error_callback(EINVAL, "address resolution failed");
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
    invoke_error_callback(ENOTCONN, "connect failed");
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
  heartbeat_manager_.reset();

  // Register with poll thread using weak_self_
  // @unsafe { - Weak::upgrade and PollThread::add
  auto self = weak_self_.upgrade();
  if (self.is_some()) {
    auto poll_proxy = make_pollable_proxy_from_typed_arc(self.unwrap());
    poll_thread_worker_->add_proxy(std::move(poll_proxy));
  // }
  } else {
    Log_error("rrr::ClientConnection: weak_self_ upgrade failed - connection may not have been created properly");
    invoke_error_callback(EINVAL, "connection registration failed");
    return EINVAL;
  }

  // Store address for potential reconnection
  reconnect_address_ = addr;
  invoke_connected_callback();

  return 0;
}

// @unsafe - Attempts to reconnect to the last connected address
int ClientConnection::reconnect(std::function<void(bool)> on_complete) {
  auto complete_callback = [&](int result) -> int {
    if (on_complete) on_complete(result == 0);
    return result;
  };

  if (reconnect_abort_.load(std::memory_order_acquire)) {
    return complete_callback(ECANCELED);
  }

  auto wait_for_inflight_reconnect = [&]() -> int {
    while (reconnecting_.load(std::memory_order_acquire)) {
      if (reconnect_abort_.load(std::memory_order_acquire)) {
        return ECANCELED;
      }
      if (state_machine_.is_connected()) {
        return 0;
      }
      rusty::thread::sleep(std::chrono::milliseconds(5));
    }

    if (state_machine_.is_connected()) {
      return 0;
    }
    return INT_MIN;
  };

  if (reconnecting_.load(std::memory_order_acquire)) {
    int waited = wait_for_inflight_reconnect();
    if (waited != INT_MIN) {
      return complete_callback(waited);
    }
  }

  // Check if we have an address to reconnect to
  if (reconnect_address_.empty()) {
    Log_error("rrr::ClientConnection: no address to reconnect to");
    return complete_callback(EINVAL);
  }

  // Can only reconnect from FAILED or DISCONNECTED state
  if (!state_machine_.can_connect()) {
    Log_error("rrr::ClientConnection: cannot reconnect from state %s",
              connection_state_to_string(state_machine_.state()));
    return complete_callback(EINVAL);
  }

  while (true) {
    bool expected = false;
    if (reconnecting_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
      break;
    }

    int waited = wait_for_inflight_reconnect();
    if (waited != INT_MIN) {
      return complete_callback(waited);
    }
  }
  invoke_reconnecting_callback();

  auto complete_reconnect = [&](bool success, int result) -> int {
    reconnecting_.store(false, std::memory_order_release);
    invoke_reconnected_callback(success);

    if (success) {
      Log_info("rrr::ClientConnection: reconnected to %s", reconnect_address_.c_str());

      // Record reconnection in metrics
      metrics_.record_reconnect();

      // Replay any queued requests
      size_t replayed = replay_pending_requests();
      if (replayed > 0) {
        Log_info("rrr::ClientConnection: replayed %zu pending requests", replayed);
      }
      return complete_callback(0);
    } else {
      if (result == ECANCELED) {
        Log_debug("rrr::ClientConnection: reconnect cancelled for %s",
                  reconnect_address_.c_str());
      } else {
        Log_error("rrr::ClientConnection: reconnection failed to %s: %d",
                  reconnect_address_.c_str(), result);
      }
      return complete_callback(result);
    }
  };

  auto reconnect_once = [&]() -> int {
    if (reconnect_abort_.load(std::memory_order_acquire)) {
      return ECANCELED;
    }
    // Reset socket for each reconnect attempt.
    socket_ = -1;
    return connect(reconnect_address_.c_str());
  };

  if (reconnect_abort_.load(std::memory_order_acquire)) {
    return complete_reconnect(false, ECANCELED);
  }

  // Another reconnect attempt can complete between the pre-CAS state check and
  // this thread acquiring reconnect ownership.
  if (state_machine_.is_connected()) {
    return complete_reconnect(true, 0);
  }

  if (!state_machine_.can_connect()) {
    return complete_reconnect(false, EINVAL);
  }

  // First attempt happens immediately.
  int result = reconnect_once();
  if (result == 0) {
    return complete_reconnect(true, 0);
  }

  // Follow configured backoff/retry policy for subsequent attempts.
  ReconnectCalculator calc(reconnect_policy_);
  while (calc.should_retry()) {
    if (reconnect_abort_.load(std::memory_order_acquire)) {
      return complete_reconnect(false, ECANCELED);
    }

    uint32_t delay_ms = calc.next_delay_ms();
    if (delay_ms > 0) {
      rusty::thread::sleep(std::chrono::milliseconds(delay_ms));
    }

    if (reconnect_abort_.load(std::memory_order_acquire)) {
      return complete_reconnect(false, ECANCELED);
    }

    // Another path may have re-established connection while sleeping.
    if (state_machine_.is_connected()) {
      return complete_reconnect(true, 0);
    }

    if (!state_machine_.can_connect()) {
      return complete_reconnect(false, EINVAL);
    }

    Log_debug("rrr::ClientConnection: reconnect retry #%u to %s",
              calc.retry_count(), reconnect_address_.c_str());
    result = reconnect_once();
    if (result == 0) {
      return complete_reconnect(true, 0);
    }
  }

  return complete_reconnect(false, result);
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

// @unsafe - Configure heartbeat manager and timeout callback.
void ClientConnection::set_heartbeat_config(const HeartbeatConfig& config) const {
  heartbeat_manager_.set_config(config);
  WeakClientConnection weak_conn;
  // @unsafe - Weak copy construction is currently modeled as non-safe.
  { weak_conn = weak_self_; }
  heartbeat_manager_.set_on_timeout([weak_conn]() {
    auto conn_opt = weak_conn.upgrade();
    if (conn_opt.is_none()) {
      return;
    }
    auto conn = conn_opt.unwrap();
    if (!conn->connected()) {
      return;
    }
    Log_warn("rrr::ClientConnection: heartbeat timeout for %s", conn->host().c_str());
    auto* mut_conn = const_cast<ClientConnection*>(conn.get());
    if (mut_conn != nullptr) {
      mut_conn->handle_error();
    }
  });
}

// @unsafe - Returns heartbeat config snapshot.
HeartbeatConfig ClientConnection::heartbeat_config() const {
  return heartbeat_manager_.config();
}

// @unsafe - Configure circuit breaker and reset state.
void ClientConnection::set_circuit_breaker_config(const CircuitBreakerConfig& config) const {
  circuit_breaker_.set_config(config);
}

// @unsafe - Returns circuit breaker config snapshot.
CircuitBreakerConfig ClientConnection::circuit_breaker_config() const {
  return circuit_breaker_.config();
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
      metrics_.record_queue_drop();
      if (req.callback) req.callback(kRequestQueueExpiredError);
      continue;
    }

    // Check if still connected
    if (!state_machine_.is_connected()) {
      // Connection lost during replay, re-queue
      // Note: This could lead to infinite loop if connection keeps failing
      // The TTL will eventually expire stale requests
      i64 replay_xid = req.xid;
      req.callback = [this, replay_xid](int err) {
        if (err != 0) {
          fail_pending_future(replay_xid, err);
        }
      };
      if (!pending_queue_.enqueue(std::move(req))) {
        // Explicit fallback: even if queue policy rejects without invoking
        // callback, do not leave the pending future orphaned.
        metrics_.record_queue_drop();
        fail_pending_future(replay_xid, kRequestQueueRejectedError);
        Log_warn("rrr::ClientConnection: replay re-queue rejected by queue policy (xid=%lld)",
                 static_cast<long long>(replay_xid));
      }
      break;
    }

    // Copy payload to output buffer
    auto* payload = const_cast<Marshal*>(req.payload.get());
    if (payload->content_size() > 0) {
      size_t payload_size = payload->content_size();
      std::string payload_bytes;
      payload_bytes.resize(payload_size);
      verify(payload->read(payload_bytes.data(), payload_size) == payload_size);

      auto guard = out_.lock().unwrap();
      verify(guard->write(payload_bytes.data(), payload_size) == payload_size);
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
      poll_thread_worker_->update_mode(fd(), PollMode::READ | PollMode::WRITE);
    }
  }

  return replayed;
}

// @unsafe - Enqueue one internal heartbeat probe into the output
// buffer (legacy fd mode) or directly through the bound channel
// proxy (Workstream K, sub-leaf 4b channel mode).
void ClientConnection::enqueue_heartbeat_probe() const {
  if (is_channel_mode()) {
    // Channel mode: build the heartbeat frame body and dispatch
    // through the channel proxy. The channel layer adds the 4-byte
    // size prefix internally; the body bytes match what
    // `set_bookmark` / `write_bookmark` would have laid out for the
    // legacy fd path, so the wire format is identical.
    Marshal body;
    body << v64(xid_counter_.next());
    body << static_cast<i32>(kInternalHeartbeatRpcId);
    const std::size_t body_size = body.content_size();
    std::vector<std::uint8_t> body_bytes;
    if (body_size > 0) {
      body_bytes.resize(body_size);
      verify(body.read(body_bytes.data(), body_size) == body_size);
    }
    // Errors here are observable via the `on_error` callback when
    // sub-leaf 4d wires it; for now we ignore the return code, same
    // as the legacy fd path which never surfaces send-side errors
    // from the heartbeat probe.
    (void)dispatch_frame_via_channel(body_bytes.data(), body_size);
    return;
  }

  auto guard = out_.lock().unwrap();
  Marshal::bookmark bmark = guard->set_bookmark(sizeof(i32));
  *guard << v64(xid_counter_.next());
  *guard << static_cast<i32>(kInternalHeartbeatRpcId);
  i32 packet_size = guard->get_and_reset_write_cnt();
  guard->write_bookmark(bmark, packet_size);
}

// @unsafe - Reset channel-mode state for a factory-driven reconnect
// (Workstream K, sub-leaf 4e). Drops the closed FiberChannel,
// flips `channel_mode_` off, and forces the state machine to
// DISCONNECTED so `connect()`'s `verify(!is_connected())` passes.
// Caller: the spawn body inside `on_channel_closed_fan_out` when a
// factory is bound.
void ClientConnection::reset_channel_mode_for_reconnect() {
  // @unsafe { SpinMutex::lock + Option::take }
  {
    auto guard = fiber_channel_.lock().unwrap();
    *guard = rusty::None;
  }
  // 4g1c: also drop the direct-channel slot so reconnect can rebind
  // a fresh proxy with fresh callbacks.
  // @unsafe { SpinMutex::lock + Option::take }
  {
    auto guard = direct_channel_.lock().unwrap();
    *guard = rusty::None;
  }
  channel_mode_.set(false);
  state_machine_.force_state(ConnectionState::DISCONNECTED);
}

// @unsafe - Channel-factory connect path (Workstream K, sub-leaf 4e).
//
// Calls the bound `ChannelFactoryProxy::connect(addr)` to obtain a
// `ChannelConnectionProxy`, then hands it to `bind_channel(...)`.
// Mirrors the legacy fd-path's bookkeeping: records the address for
// reconnect, transitions the state machine to CONNECTED on success,
// invokes the connected callback, and reports errors through the
// usual `invoke_error_callback` path. Caller is `connect(addr)`,
// which already transitioned the state to CONNECTING and verified
// the factory binding.
int ClientConnection::connect_via_factory(const char* addr) {
  ChannelFactoryProxy factory;
  // Take a *clone* of the bound factory so we can call `connect` on
  // it without holding the RefCell guard across what may be a
  // blocking syscall (TCP handshake, address resolution). The
  // ChannelFactoryProxy's underlying type (e.g. TcpFactory wrapped in
  // an Arc<TcpFactory> adapter) is reference-counted, so copying the
  // proxy is cheap. We don't have a generic clone() on
  // pro::proxy<F>, so we copy through the Option's Arc-equivalent
  // semantics by re-binding via std::move from a fresh borrow.
  // @unsafe { SpinMutex::lock + ChannelFactoryProxy copy }
  {
    auto guard = factory_.lock().unwrap();
    if (guard->is_none()) {
      Log_error(
          "rrr::ClientConnection::connect_via_factory: factory unbound at "
          "the moment of connect (race against bind_factory)");
      state_machine_.transition_to(ConnectionState::FAILED);
      invoke_error_callback(ENOTCONN, "factory unbound");
      return ENOTCONN;
    }
    // pro::proxy is move-only; we can't clone. Use the proxy in
    // place via the Box wrapper. The SpinMutex guard is held across
    // the connect() syscall — the caller's perspective is that
    // connect is synchronous (channel-layer contract), and the
    // factory itself is read-only (bind_factory is essentially
    // one-shot per Client lifecycle), so holding the lock briefly
    // while we issue the syscall doesn't introduce contention with
    // the dispatch path (which locks `fiber_channel_`, not
    // `factory_`).
    auto* bound = const_cast<ChannelFactoryProxy*>(
        guard->as_ref().unwrap().get());
    ConnectResult result = (*bound)->connect(std::string_view(addr));
    if (result.error != ChannelError::None || !result.connection.has_value()) {
      const auto err_str = std::string("factory connect failed: ")
          + channel_error_to_string(result.error);
      Log_error("rrr::ClientConnection: %s (addr=%s)", err_str.c_str(), addr);
      state_machine_.transition_to(ConnectionState::FAILED);
      // Map the channel error onto an errno-shaped value the legacy
      // call sites expect.
      const int rc = (result.error == ChannelError::ConnectionRefused)
                       ? ECONNREFUSED
                     : (result.error == ChannelError::AddressInvalid)
                       ? EINVAL
                       : ENOTCONN;
      invoke_error_callback(rc, err_str);
      return rc;
    }
    // Sub-leaf 4g1c: bypass FiberChannel + recv-loop fiber entirely.
    // Install on_frame/on_closed callbacks directly on the channel
    // proxy. on_frame runs on the poll thread (where the channel
    // layer fires it) and calls decode_response_and_notify inline —
    // no IntEvent, no fiber yield, no waiting_events_ churn. This
    // works around the deeper reactor/fiber wedge documented in 4g1b.
    bind_channel_direct(std::move(result.connection));
  }

  // Record address for the close fan-out's reconnect spawn — it
  // re-runs the factory connect with the same target.
  // @unsafe { std::string assignment }
  { reconnect_address_ = addr; }

  // Mirror the fd path's terminal transition: the channel layer's
  // own state (proxy.is_closed()) becomes the source of truth, but
  // we still drive the legacy state machine through CONNECTED so
  // existing health-check / metric APIs (`connected()`,
  // `connection_state()`) keep working.
  if (!state_machine_.transition_to(ConnectionState::CONNECTED)) {
    state_machine_.force_state(ConnectionState::CONNECTED);
  }
  invoke_connected_callback();
  return 0;
}

// @unsafe - Spawns recv-loop fiber, constructs FiberChannel wrapper.
//
// Workstream K, sub-leaves 4a/4b/4c2:
//   - 4a flipped the `channel_mode_` latch.
//   - 4b routed outbound frames through the proxy.
//   - 4c2 wraps the proxy in a `FiberChannel` and spawns a recv-loop
//     fiber that drives response demux from `recv_frame()` calls.
//
// The fiber is spawned on the *current* thread's reactor. Per the
// channel-layer threading contract, the proxy's callbacks fire on the
// reactor that owns the underlying connection — typically the poll
// thread for production TCP, or the test thread for fake channels.
// Calling `bind_channel` from any other thread leaves the recv-loop
// fiber on the wrong reactor and would race the IntEvent signaling
// path. Cross-thread scheduling of the spawn is sub-leaf 4e's
// concern; for 4c2 we document the constraint and rely on the
// caller.
void ClientConnection::bind_channel(ChannelConnectionProxy channel) {
  if (!channel.has_value()) return;

  // Move the proxy into a heap-allocated `FiberChannel` so the
  // recv-loop fiber can hold a stable pointer to the wrapper across
  // its parking lifetime. `FiberChannel` is move-deleted (its
  // callbacks capture `this`), so we use `make_box` which constructs
  // in-place via perfect-forwarded `new` rather than moving.
  // @unsafe { make_box + SpinMutex mutation }
  {
    auto guard = fiber_channel_.lock().unwrap();
    *guard = rusty::Some(rusty::make_box<FiberChannel>(std::move(channel)));
  }
  channel_mode_.set(true);

  // Capture a Weak<> so the parked fiber doesn't extend the
  // connection's lifetime (which would create a cycle via
  // `fiber_channel_` ownership).
  WeakClientConnection weak_self;
  // @unsafe { Weak copy is currently treated as non-safe. }
  { weak_self = weak_self_; }

  // Spawn the recv-loop fiber on the *current* thread's reactor.
  // Per the channel-layer threading contract, the recv-loop fiber
  // must live on the same reactor that fires the proxy's
  // `on_frame` / `on_closed` callbacks (so the `IntEvent` it parks
  // on can be signaled cross-fiber within one thread —
  // cross-thread `IntEvent::set` is unsafe). Caller is responsible
  // for choosing the right thread:
  //   - Fake-channel unit tests call `bind_channel(...)` from the
  //     test thread, where they also drive `deliver()` /
  //     `deliver_closed()`. The recv-loop fiber lives on the test
  //     thread; everything stays single-threaded.
  //   - Production TCP / factory paths use
  //     `bind_channel_via_poll_thread(...)` (sub-leaf 4f) which
  //     submits a `OneTimeJob` to the poll thread, where the
  //     spawn — and therefore the resulting fiber — lands on the
  //     same reactor that fires `TcpConnection::handle_read`'s
  //     `on_frame` callback.
  Fiber::create_run([weak_self]() mutable {
    auto conn_opt = weak_self.upgrade();
    if (conn_opt.is_none()) return;
    auto conn = conn_opt.unwrap();
    auto* mut_conn = const_cast<ClientConnection*>(conn.get());
    mut_conn->run_recv_loop();
  }, __FILE__, __LINE__);
}

// @unsafe - Channel-mode bind that schedules the recv-loop fiber
// spawn onto the *poll thread*. Workstream K, sub-leaf 4f.
//
// Used by production code paths (factory-driven `connect` /
// reconnect) that run on the user thread but need the recv-loop
// fiber on the poll thread — same thread the channel proxy's
// callbacks fire on. Submits a `OneTimeJob` whose `Work()` runs
// `run_recv_loop()` from a fiber that the poll thread's
// `trigger_job` spawns on its own reactor.
void ClientConnection::bind_channel_via_poll_thread(
    ChannelConnectionProxy channel) {
  if (!channel.has_value()) return;

  // Move the proxy into the heap-allocated FiberChannel and flip
  // the latch on the calling thread — these are pure data
  // mutations and the recv-loop fiber doesn't observe them until
  // after we submit the OneTimeJob below.
  // @unsafe { make_box + SpinMutex mutation }
  {
    auto guard = fiber_channel_.lock().unwrap();
    *guard = rusty::Some(rusty::make_box<FiberChannel>(std::move(channel)));
  }
  channel_mode_.set(true);

  WeakClientConnection weak_self;
  // @unsafe { Weak copy }
  { weak_self = weak_self_; }

  // Schedule the recv-loop fiber spawn onto the poll thread. The
  // poll thread's `trigger_job` calls `Fiber::create_run` from
  // its own reactor, so the resulting fiber's IntEvent waits and
  // the `on_frame` callback's signal both land on the same
  // thread.
  // @unsafe { Arc::new_ + std::function + cross-thread queue }
  auto recv_job = rusty::Arc<OneTimeJob>::new_(OneTimeJob([weak_self]() {
    auto conn_opt = weak_self.upgrade();
    if (conn_opt.is_none()) return;
    auto conn = conn_opt.unwrap();
    auto* mut_conn = const_cast<ClientConnection*>(conn.get());
    mut_conn->run_recv_loop();
  }));
  // Upcast Arc<OneTimeJob> -> Arc<Job> for the PollThread queue.
  auto recv_job_base = rusty::Arc<Job>(recv_job);
  poll_thread_worker_->add(std::move(recv_job_base));
}

// @unsafe - Direct on_frame / on_closed callback binding (Workstream
// K, sub-leaf 4g1c).
//
// Bypasses FiberChannel + recv-loop fiber entirely. Installs the
// callbacks directly on the channel proxy:
//   - on_frame: builds a copy of the frame bytes (the channel-layer
//     contract makes the frame.payload pointer valid only during the
//     callback) and calls decode_response_and_notify on the same
//     thread the channel layer fires on. For TCP, that's the poll
//     thread — same thread that handles_read parses frames.
//   - on_closed: invokes on_channel_closed_fan_out on the same
//     thread.
//
// The proxy's other thread-safety properties carry over: send_frame
// is callable from any thread (we use it that way from
// dispatch_frame_via_channel in user threads).
//
// Stores the proxy in `direct_channel_`. The connection's
// `Arc<ClientConnection>` lifetime is captured weakly in the
// callbacks, so the connection can be torn down without leaving a
// dangling pointer in the proxy's installed callbacks. When
// `direct_channel_` is destroyed, the proxy's destructor drops the
// callbacks, so any in-flight callback dispatch from the channel
// layer must complete before drop is allowed (this matches the
// FiberChannel destructor's contract).
void ClientConnection::bind_channel_direct(ChannelConnectionProxy channel) {
  if (!channel.has_value()) return;

  // Capture a weak ref so the proxy's installed callbacks don't
  // extend the ClientConnection's lifetime (avoids a refcount cycle
  // through `direct_channel_` + the callbacks).
  WeakClientConnection weak_self;
  // @unsafe { Weak copy is currently treated as non-safe }
  { weak_self = weak_self_; }

  // Install callbacks BEFORE moving the proxy into the slot. After
  // the move, the proxy lives in `direct_channel_`; the lambdas
  // capture only the weak self-ref.
  // @unsafe { lambda capture, channel proxy mutator }
  channel->set_on_frame([weak_self](const ChannelFrame& f) {
    auto conn_opt = weak_self.upgrade();
    if (conn_opt.is_none()) return;
    auto conn = conn_opt.unwrap();
    auto* mut_conn = const_cast<ClientConnection*>(conn.get());
    mut_conn->decode_response_and_notify(f.payload, f.size);
  });
  channel->set_on_closed([weak_self](ChannelError /*reason*/) {
    auto conn_opt = weak_self.upgrade();
    if (conn_opt.is_none()) return;
    auto conn = conn_opt.unwrap();
    auto* mut_conn = const_cast<ClientConnection*>(conn.get());
    mut_conn->on_channel_closed_fan_out();
  });
  // on_error is not surfaced to the RPC layer in this binding mode
  // (the channel-layer contract follows fatal errors with on_closed,
  // so on_channel_closed_fan_out covers the recovery path).
  channel->set_on_error([](ChannelError, std::string_view) {});

  // Move the proxy into the slot and flip the channel-mode latch.
  // @unsafe { make_box + SpinMutex mutation }
  {
    auto guard = direct_channel_.lock().unwrap();
    *guard = rusty::Some(rusty::make_box<ChannelConnectionProxy>(std::move(channel)));
  }
  channel_mode_.set(true);
}

// @unsafe - Drives Marshal / Future / pending_fu_ from a fiber.
//
// Recv-loop body: blocks on `FiberChannel::recv_frame()` and forwards
// each frame's body to `decode_response_and_notify`. Returns when
// the channel closes (recv_frame returns None) or when the wrapper
// goes away.
//
// We resolve the FiberChannel raw pointer ONCE under a brief lock
// and then drop the SpinMutex guard — `recv_frame()` yields the
// fiber (parking on an `IntEvent`), and holding a lock across the
// yield would block other threads racing on `dispatch_frame_via_channel`
// (or, on the same reactor, prevent other fibers from running). The
// raw pointer stays valid because the spawning lambda keeps an
// `Arc<ClientConnection>` alive for the fiber's lifetime, and the
// connection owns the `Box<FiberChannel>`.
void ClientConnection::run_recv_loop() {
  FiberChannel* fc = nullptr;
  {
    auto guard = fiber_channel_.lock().unwrap();
    if (guard->is_none()) return;
    // @unsafe { Box::get returns raw pointer }
    fc = const_cast<FiberChannel*>(guard->as_ref().unwrap().get());
  }
  while (true) {
    rusty::Option<OwnedFrame> frame_opt = fc->recv_frame();
    if (frame_opt.is_none()) {
      // Channel closed. Run the close-side fan-out (sub-leaf 4d):
      // cancel pending futures with ENOTCONN, fire error /
      // disconnected callbacks, and trigger auto-reconnect if the
      // policy allows. The fiber then exits, dropping its
      // Arc<ClientConnection> capture so the connection can finish
      // teardown if no other strong refs remain.
      on_channel_closed_fan_out();
      return;
    }
    auto frame = std::move(frame_opt).unwrap();
    decode_response_and_notify(frame.bytes.data(), frame.bytes.size());
  }
}

// @unsafe - Marshal operators, Future::notify_ready, pending_fu_ map.
//
// Decode one response frame body and resolve the matching pending
// future. The body layout mirrors the legacy fd path's payload (i.e.,
// what arrives after the 4-byte size prefix in `client.cpp::handle_read`):
//
//     [v64 reply_xid][v32 error_code][v64 server_instance_id][user-marshaled reply]
//
// The channel layer consumes the size prefix (and with it the
// `kResponseHeaderExtFlag` bit), so we lose the runtime signal that
// distinguishes legacy responses (no instance ID) from extended
// responses (with instance ID). The current SRPC server always emits
// the extended form (`server.hpp::reply` sets
// `include_instance_id = true`), so channel mode unconditionally
// reads the instance ID. Sub-leaf 4f's migration switch / parity
// pass will revisit if a legacy-server interop path needs the bit
// surfaced through `ChannelFrame`.
void ClientConnection::decode_response_and_notify(const std::uint8_t* bytes,
                                                  std::size_t size) {
  // Wrap the bytes in a Marshal so we can reuse the existing
  // operator>> codecs for v64 / v32 / Marshal::read.
  Marshal body;
  if (size > 0) {
    body.write(bytes, size);
  }

  v64 v_reply_xid;
  v32 v_error_code;
  size_t parsed_header_size = 0;

  body >> v_reply_xid >> v_error_code;
  parsed_header_size += v_reply_xid.val_size() + v_error_code.val_size();

  // See the function-header note: in channel mode the extended-header
  // flag is consumed by the framing layer. We assume the server
  // always emits the extended form (matches `server.hpp` today).
  v64 v_server_instance_id;
  body >> v_server_instance_id;
  parsed_header_size += v_server_instance_id.val_size();
  check_server_instance(static_cast<uint64_t>(v_server_instance_id.get()));

  if (size < parsed_header_size) {
    invoke_error_callback(EPROTO, "response header larger than packet payload");
    return;
  }

  size_t response_payload_bytes = size - parsed_header_size;
  heartbeat_manager_.on_pong_received();

  rusty::Option<rusty::Arc<Future>> fu_opt = rusty::None;
  {
    auto guard = pending_fu_.lock().unwrap();
    auto fu_ptr = guard->get(v_reply_xid.get());
    if (fu_ptr.is_some()) {
      fu_opt = rusty::Some((*fu_ptr.unwrap()).clone());
      guard->remove(v_reply_xid.get());
    }
  }

  if (fu_opt.is_some()) {
    auto fu = fu_opt.unwrap();
    verify(fu->xid_ == v_reply_xid.get());
    fu->error_code_.set(v_error_code.get());
    fu->reply_.borrow_mut()->read_from_marshal(body, response_payload_bytes);

    if (v_error_code.get() == 0) {
      metrics_.record_request_completed();
    } else {
      metrics_.record_request_failed();
    }
    record_circuit_result(v_error_code.get());

    fu->notify_ready(fu);
  } else {
    // No matching future (timed out or replaced). Drain the payload
    // anyway to keep the Marshal balanced — same idiom as the legacy
    // fd path's branch in `handle_read`.
    Marshal reply;
    reply.read_from_marshal(body, response_payload_bytes);
  }
}

// @unsafe - Channel-mode close fan-out (Workstream K, sub-leaf 4d).
//
// Mirrors the legacy fd path's `handle_error` for channel-mode
// connections: when the recv-loop fiber sees `recv_frame()` return
// None (channel closed by peer or transport fault), this method
// runs the same reliability fan-out:
//
//   1. Force the connection state to FAILED (unless the user
//      already initiated the close — DISCONNECTING/DISCONNECTED).
//   2. Invoke the error callback with ECONNRESET (only on
//      non-user-initiated paths).
//   3. Reset the heartbeat manager so a future reconnect starts
//      from a clean baseline.
//   4. Invalidate every pending future (`ENOTCONN`).
//   5. Invoke the disconnected callback (only on
//      non-user-initiated paths, matching `close()`'s contract).
//   6. If `reconnect_policy_.auto_reconnect` is set and a
//      `reconnect_address_` was recorded, increment the
//      `channel_reconnect_attempts_` counter and spawn a thread
//      that will call `reconnect()`. Sub-leaf 4e replaces the
//      legacy `reconnect()` body with a factory-driven path; for
//      4d the spawn is observable through the counter without
//      requiring tests to actually drive the fd reconnect.
//
// Skips the socket-close half of `close()` (`::close(socket_)`,
// state transitions through DISCONNECTING) — channel mode never
// owned the fd, and the channel layer has already torn down its
// underlying transport.
void ClientConnection::on_channel_closed_fan_out() {
  ConnectionState prev_state = state_machine_.state();
  const bool user_initiated_closing =
      prev_state == ConnectionState::DISCONNECTING ||
      prev_state == ConnectionState::DISCONNECTED ||
      reconnect_abort_.load(std::memory_order_acquire);

  if (!user_initiated_closing) {
    invoke_error_callback(ECONNRESET, "channel closed");
    state_machine_.force_state(ConnectionState::FAILED);
  }

  heartbeat_manager_.reset();
  invalidate_pending_futures();

  if (!user_initiated_closing) {
    invoke_disconnected_callback();
  }

  // Trigger auto-reconnect if the policy allows. Channel-mode
  // reconnect is wired in sub-leaf 4e (factory-based); for 4d we
  // bump an observable counter the moment the fan-out reaches the
  // reconnect-policy branch, then conditionally spawn the legacy
  // fd reconnect path. The counter is the observability signal:
  // tests verify it incremented by setting
  // `reconnect_abort_=true` (so the spawn short-circuits without
  // actually calling `reconnect()`). Production callers that want
  // a real reconnect leave the abort flag false and rely on the
  // spawn.
  if (reconnect_policy_.auto_reconnect &&
      // @unsafe { std::string::empty }
      !reconnect_address_.empty()) {
    channel_reconnect_attempts_.fetch_add(1, std::memory_order_acq_rel);

    if (reconnect_abort_.load(std::memory_order_acquire)) {
      // Caller requested no reconnect (typically: connection
      // tearing down). Counter is still bumped for observability.
      return;
    }
    auto weak_conn = weak_self_;
    rusty::thread::spawn([weak_conn]() {
      auto conn_opt = weak_conn.upgrade();
      if (conn_opt.is_none()) {
        return;
      }
      auto conn = conn_opt.unwrap();
      if (!conn->reconnect_policy_.auto_reconnect ||
          conn->reconnect_abort_.load(std::memory_order_acquire)) {
        return;
      }
      auto state = conn->connection_state();
      if (state == ConnectionState::FAILED ||
          state == ConnectionState::DISCONNECTED) {
        // Workstream K, sub-leaf 4e — factory-driven reconnect.
        //
        // When a `ChannelFactoryProxy` is bound, the fan-out's
        // reconnect spawn re-runs the same factory connect path
        // that the original `connect(addr)` took (factory ->
        // connect -> bind_channel) instead of the legacy fd
        // `reconnect()` (which re-opens a raw socket). The
        // factory-aware path also re-arms the recv-loop fiber via
        // `bind_channel`, so a successful reconnect resumes
        // request demux without a manual setup step.
        auto* mut_conn = const_cast<ClientConnection*>(conn.get());
        if (mut_conn == nullptr) {
          return;
        }
        if (conn->is_factory_bound()) {
          Log_info(
              "rrr::ClientConnection: channel-mode auto-reconnect "
              "(factory) triggered after on_closed");
          // Reset the channel-mode latch + drop the stale
          // FiberChannel before calling connect again — connect's
          // verify(!is_connected()) requires the state machine to
          // be non-CONNECTED, and the new bind_channel needs the
          // option slot empty so it can install fresh callbacks.
          mut_conn->reset_channel_mode_for_reconnect();
          // `connect` reads `reconnect_address_` itself (set by
          // the original connect call), so we just call it.
          (void)mut_conn->connect(conn->reconnect_address_.c_str());
          return;
        }
        Log_info(
            "rrr::ClientConnection: channel-mode auto-reconnect (legacy) "
            "triggered after on_closed");
        // @unsafe - reconnect mutates socket/state and performs network I/O.
        mut_conn->reconnect();
      }
    }).detach();
  }
}

// @unsafe - Route allow_request through metrics (rejections + state transitions).
bool ClientConnection::allow_request_with_circuit_metrics() const {
  CircuitState before = circuit_breaker_.state();
  bool allowed = circuit_breaker_.allow_request();
  CircuitState after = circuit_breaker_.state();
  record_circuit_state_transition(before, after);
  if (!allowed) {
    metrics_.record_circuit_open_rejection();
  }
  return allowed;
}

// @safe - Checks whether an error should contribute to circuit tripping.
bool ClientConnection::should_trip_circuit_for_error(i32 err) {
  switch (err) {
    case 0:
      return false;
    case ENOTCONN:
    case ECONNREFUSED:
    case ECONNRESET:
    case ECONNABORTED:
    case ETIMEDOUT:
    case EHOSTUNREACH:
    case ENETUNREACH:
    case EPIPE:
      return true;
    default:
      return false;
  }
}

// @safe - Track circuit breaker state transitions in metrics.
void ClientConnection::record_circuit_state_transition(
    CircuitState before,
    CircuitState after) const {
  if (before == after) {
    return;
  }

  switch (after) {
    case CircuitState::OPEN:
      metrics_.record_circuit_open_transition();
      break;
    case CircuitState::HALF_OPEN:
      metrics_.record_circuit_half_open_transition();
      break;
    case CircuitState::CLOSED:
      metrics_.record_circuit_closed_transition();
      break;
    default:
      break;
  }
}

// @unsafe - Records success/failure in circuit breaker.
void ClientConnection::record_circuit_result(i32 err) const {
  CircuitState before = circuit_breaker_.state();
  if (err == 0) {
    circuit_breaker_.record_success();
  } else if (should_trip_circuit_for_error(err)) {
    circuit_breaker_.record_failure();
  }
  CircuitState after = circuit_breaker_.state();
  record_circuit_state_transition(before, after);
}

// @safe - Maps errno-style errors into structured RpcError categories.
RpcError ClientConnection::map_system_error(i32 err) {
  switch (err) {
    case 0:
      return RpcError::OK;
    case ENOTCONN:
      return RpcError::NOT_CONNECTED;
    case ECONNREFUSED:
      return RpcError::CONNECTION_REFUSED;
    case ECONNRESET:
      return RpcError::CONNECTION_RESET;
    case ENETUNREACH:
      return RpcError::NETWORK_UNREACHABLE;
    case EHOSTUNREACH:
      return RpcError::HOST_UNREACHABLE;
    case ECONNABORTED:
    case EPIPE:
      return RpcError::CONNECTION_CLOSED;
    case EBUSY:
      return RpcError::CIRCUIT_OPEN;
    case ETIMEDOUT:
      return RpcError::RESPONSE_TIMEOUT;
    case EAGAIN:
#if EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK:
#endif
      return RpcError::REQUEST_TIMEOUT;
    case EINVAL:
      return RpcError::INVALID_ARGUMENT;
    default:
      return RpcError::UNKNOWN_ERROR;
  }
}

// @unsafe - Invoke callback manager error hooks.
void ClientConnection::invoke_error_callback(i32 err, const std::string& message) const {
  if (!callback_manager_) {
    return;
  }
  callback_manager_->invoke_on_error(map_system_error(err), message);
}

// @unsafe - Invoke callback manager disconnected hooks.
void ClientConnection::invoke_disconnected_callback() const {
  if (!callback_manager_) {
    return;
  }
  callback_manager_->invoke_on_disconnected();
}

// @unsafe - Invoke callback manager reconnecting hooks.
void ClientConnection::invoke_reconnecting_callback() const {
  if (!callback_manager_) {
    return;
  }
  callback_manager_->invoke_on_reconnecting();
}

// @unsafe - Invoke callback manager reconnected hooks.
void ClientConnection::invoke_reconnected_callback(bool success) const {
  if (!callback_manager_) {
    return;
  }
  callback_manager_->invoke_on_reconnected(success);
}

// @unsafe - Invoke callback manager connected hooks.
void ClientConnection::invoke_connected_callback() const {
  if (!callback_manager_) {
    return;
  }
  callback_manager_->invoke_on_connected();
}

// @unsafe - Error handler - transitions to FAILED state
void ClientConnection::handle_error() {
  ConnectionState prev_state = state_machine_.state();
  const bool user_initiated_closing =
      prev_state == ConnectionState::DISCONNECTING ||
      prev_state == ConnectionState::DISCONNECTED ||
      reconnect_abort_.load(std::memory_order_acquire);

  if (!user_initiated_closing) {
    invoke_error_callback(ECONNRESET, "connection error");
    // Force transition to FAILED state (from any state)
    state_machine_.force_state(ConnectionState::FAILED);
  }
  // @unsafe - calls close() which does system calls
  { close(); }

  if (user_initiated_closing) {
    return;
  }
  invoke_disconnected_callback();

  // Trigger policy-driven reconnect automatically after transport failures.
  if (reconnect_policy_.auto_reconnect &&
      !reconnect_abort_.load(std::memory_order_acquire)) {
    // @unsafe - std::string::empty and Weak copy are currently treated as non-safe.
    {
      if (reconnect_address_.empty()) {
        return;
      }
      auto weak_conn = weak_self_;
      rusty::thread::spawn([weak_conn]() {
        auto conn_opt = weak_conn.upgrade();
        if (conn_opt.is_none()) {
          return;
        }

        auto conn = conn_opt.unwrap();
        if (!conn->reconnect_policy_.auto_reconnect ||
            conn->reconnect_abort_.load(std::memory_order_acquire)) {
          return;
        }

        auto state = conn->connection_state();
        if (state == ConnectionState::FAILED || state == ConnectionState::DISCONNECTED) {
          Log_info("rrr::ClientConnection: auto-reconnect triggered after connection failure");
          // @unsafe - reconnect mutates socket/state and performs network I/O.
          auto* mut_conn = const_cast<ClientConnection*>(conn.get());
          if (mut_conn != nullptr) {
            mut_conn->reconnect();
          }
        }
      }).detach();
    }
  }
}

// @unsafe - Poll-loop heartbeat tick and pending write-update handling.
bool ClientConnection::check_pending_write_update() const {
  if (state_machine_.is_connected() && !paused_.get()) {
    if (heartbeat_manager_.check_timeout()) {
      // Timeout callback already transitioned connection through error handling.
      return false;
    }
    if (heartbeat_manager_.should_send_heartbeat()) {
      enqueue_heartbeat_probe();
      heartbeat_manager_.on_heartbeat_sent();
      return true;
    }
  }

  if (pending_write_update_.get()) {
    pending_write_update_.set(false);
    return true;
  }
  return false;
}

// @unsafe - Writes buffered data to socket, protected by SpinMutex
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
  // Workstream K, sub-leaf 4c2: in channel mode, response demux is
  // owned by the recv-loop fiber driven from `run_recv_loop` over the
  // bound `FiberChannel`. The legacy `Pollable::handle_read` is
  // short-circuited as a defensive guard so a stale poll-loop
  // registration (from a connection that pre-dates `bind_channel`)
  // can't double-consume bytes from a socket the channel layer
  // already owns. Sub-leaf 4e removes the registration entirely.
  if (is_channel_mode()) {
    return false;
  }

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
    i32 encoded_packet_size = 0;
    int n_peek = in_.peek(encoded_packet_size);
    if (n_peek != sizeof(i32)) {
      break;
    }
    i32 packet_size = response_payload_size(encoded_packet_size);
    if (in_.content_size() >= static_cast<size_t>(packet_size) + sizeof(i32)) {

      verify(in_.read(&encoded_packet_size, sizeof(i32)) == sizeof(i32));

      const bool has_extended_header = response_has_extended_header(encoded_packet_size);
      packet_size = response_payload_size(encoded_packet_size);

      v64 v_reply_xid;
      v32 v_error_code;
      size_t parsed_header_size = 0;

      in_ >> v_reply_xid >> v_error_code;
      parsed_header_size += v_reply_xid.val_size() + v_error_code.val_size();

      if (has_extended_header) {
        v64 v_server_instance_id;
        in_ >> v_server_instance_id;
        parsed_header_size += v_server_instance_id.val_size();
        check_server_instance(static_cast<uint64_t>(v_server_instance_id.get()));
      }

      if (packet_size < static_cast<i32>(parsed_header_size)) {
        invoke_error_callback(EPROTO, "response header larger than packet payload");
        handle_error();
        return false;
      }

      size_t response_payload_size_bytes = static_cast<size_t>(packet_size) - parsed_header_size;
      heartbeat_manager_.on_pong_received();

      rusty::Option<rusty::Arc<Future>> fu_opt = rusty::None;
      {
        auto guard = pending_fu_.lock().unwrap();
        auto fu_ptr = guard->get(v_reply_xid.get());
        if (fu_ptr.is_some()) {
          fu_opt = rusty::Some((*fu_ptr.unwrap()).clone());  // Copy Arc (refcount still 2)
          guard->remove(v_reply_xid.get());  // Remove from map (refcount 2→1)
        }
      }  // Guard dropped here, releasing lock

      if (fu_opt.is_some()) {
        auto fu = fu_opt.unwrap();
        verify(fu->xid_ == v_reply_xid.get());

        fu->error_code_.set(v_error_code.get());
        fu->reply_.borrow_mut()->read_from_marshal(in_, response_payload_size_bytes);

        // Record request completion in metrics
        if (v_error_code.get() == 0) {
          metrics_.record_request_completed();
        } else {
          metrics_.record_request_failed();
        }
        record_circuit_result(v_error_code.get());

        fu->notify_ready(fu);  // Pass Arc to self for callback safety

        // Arc auto-released when scope exits (refcount 1→0 if user released theirs)
      } else {
        // the future might have timed out - consume data anyway
        Marshal reply;
        reply.read_from_marshal(in_, response_payload_size_bytes);
      }
    } else {
      // No complete packet available
      break;
    }
  }

  Reactor::get_reactor()->loop();

  return true;
}

// @unsafe - Determines polling mode based on output buffer, protected by SpinMutex
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

// @unsafe - Cleanup destructor, uses request_close() for thread-safe close
Client::~Client() {
  close();  // Delegate to close() which uses request_close()
}

// @unsafe - Sets connection validity using RefCell
void Client::set_valid(bool valid) const {
  auto guard = connection_.borrow_mut();
  if (guard->is_some()) {
    auto out_guard = guard->as_mut().unwrap()->out_.lock().unwrap();
    out_guard->valid_id = valid;
  }
}

// @unsafe - Closes socket via request_close() for thread-safe cleanup
// Note: Does NOT clear the connection object so reconnect() can work.
// The connection object retains the address for reconnection.
// IMPORTANT: Does NOT call conn.close() directly! The socket close must happen
// in the poll thread to avoid race conditions with pending CmdAddPollable commands.
void Client::close() const {
  auto guard = connection_.borrow_mut();
  if (guard->is_some()) {
    auto& conn = const_cast<ClientConnection&>(*guard->as_ref().unwrap());
    const bool was_connected = conn.connected();
    conn.mark_closing();
    if (was_connected) {
      // Request poll thread to close the connection
      // The poll thread will call ClientConnection::close() via do_close_pollable()
      // @unsafe - PollThread::request_close
      { poll_thread_worker_->request_close(conn.fd()); }
    }
    // Don't clear connection to None - we need it for reconnect()
  }
}

// @unsafe - Jetpack: handle_free for explicit future cleanup
void Client::handle_free(i64 xid) const {
  auto guard = connection_.borrow();
  if (guard->is_some()) {
    guard->as_ref().unwrap()->handle_free(xid);
  }
}

// @unsafe - Pauses the connection
void Client::pause() const {
  auto guard = connection_.borrow();
  if (guard->is_some()) {
    guard->as_ref().unwrap()->pause();
  }
}

// @unsafe - Resumes the connection
void Client::resume() const {
  auto guard = connection_.borrow();
  if (guard->is_some()) {
    guard->as_ref().unwrap()->resume();
  }
}

// @unsafe - Establishes TCP/IPC connection to server
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
  mut_conn.set_callback_manager(callback_manager_);
  mut_conn.is_client_mode_ = client;
  is_client_mode_.set(client);

  // Apply pending keepalive config before connecting
  mut_conn.set_keepalive(pending_keepalive_config_.get());
  // Apply pending heartbeat config before connecting
  mut_conn.set_heartbeat_config(pending_heartbeat_config_.get());
  // Apply pending circuit-breaker config before connecting
  mut_conn.set_circuit_breaker_config(pending_circuit_breaker_config_.get());
  // Apply pending reconnect policy before connecting
  mut_conn.set_reconnect_policy(pending_reconnect_policy_.get());

  // Workstream K, sub-leaf 4f — migration switch.
  //
  // If `SRPC_USE_CHANNEL=1` is set in the environment AND no factory
  // has been installed via `set_channel_factory(...)`, install a
  // default TCP factory now. This routes the subsequent
  // `connect(addr)` through `factory->connect(addr)` ->
  // `bind_channel(...)`, exercising the channel-mode path
  // end-to-end without forcing every existing call site to opt in.
  // The flag default-off keeps the legacy fd path active for
  // unsuspecting consumers; sub-leaf 4g flips the default and
  // removes the legacy path.
  if (srpc_use_channel() && !has_pending_channel_factory()) {
    auto tcp_factory =
        rusty::Arc<TcpFactory>::make(poll_thread_worker_);
    set_channel_factory(make_tcp_factory_proxy(std::move(tcp_factory)));
  }

  // Workstream K, sub-leaf 4e — push the pending channel factory
  // into the new ClientConnection. Once bound, the connection's
  // `connect(addr)` and reconnect spawn route through the factory
  // (`factory->connect(addr)` -> `bind_channel(...)`) instead of
  // the legacy fd path. Take the proxy by std::move because
  // pro::proxy is move-only; the factory is a one-shot push per
  // Client lifecycle (re-bind via `set_channel_factory` to install
  // a different one — affects subsequent Client::connect calls).
  // @unsafe { SpinMutex::lock + Box deref + ChannelFactoryProxy move }
  {
    auto guard = pending_factory_.lock().unwrap();
    if (guard->is_some()) {
      // Move the proxy out of the boxed Option. The Box stays
      // alive on the stack until the end of this scope; we move
      // the inner proxy into the new ClientConnection's bind_factory
      // (which re-boxes it on the connection side).
      auto box = std::move(*guard).unwrap();
      ChannelFactoryProxy moved = std::move(*box);
      mut_conn.bind_factory(std::move(moved));
      *guard = rusty::None;  // single-use; tests can re-bind
    }
  }

  // Call connect through mutable reference
  int result = 0;
  // @unsafe - Low-level TCP/IPC connection (or factory-driven)
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
  conn.reconnect_abort_.store(false, std::memory_order_release);

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

// @unsafe - Check if a client is considered healthy
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
  for (auto it : cache_) {
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
  auto clients_opt = cache_.get(addr);
  if (clients_opt.is_some()) {
    for (const auto& client : *clients_opt.unwrap()) {
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
  auto clients_opt = cache_.get(addr);
  if (clients_opt.is_some()) {
    auto* clients = clients_opt.unwrap();
    auto cfg = config_.get();

    // Remove unhealthy clients, but keep at least min_connections.
    rusty::Vec<rusty::Arc<Client>> kept;
    kept.reserve(clients->len());
    for (const auto& client : *clients) {
      if (clients->len() - removed <= static_cast<size_t>(cfg.min_connections)) {
        kept.push(client.clone());
        continue;
      }
      if (!is_client_healthy(client)) {
        client->close();
        removed++;
      } else {
        kept.push(client.clone());
      }
    }
    *clients = std::move(kept);

    // Remove empty entries from cache
    if (clients->is_empty()) {
      cache_.remove(addr);
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

  auto clients_opt = cache_.get(addr);
  if (clients_opt.is_some()) {
    auto* clients = clients_opt.unwrap();

    rusty::Vec<rusty::Arc<Client>> kept;
    kept.reserve(clients->len());
    for (const auto& client : *clients) {
      if (clients->len() - closed <= static_cast<size_t>(cfg.min_connections)) {
        kept.push(client.clone());
        continue;
      }
      if (client->is_idle(cfg.idle_timeout_ms, current_time_ms)) {
        client->close();
        closed++;
      } else {
        kept.push(client.clone());
      }
    }
    *clients = std::move(kept);

    if (clients->is_empty()) {
      cache_.remove(addr);
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

  rusty::Vec<std::string> keys = cache_.keys();
  rusty::Vec<std::string> empty_keys;
  for (const auto& addr : keys) {
    auto clients_opt = cache_.get(addr);
    if (clients_opt.is_none()) {
      continue;
    }
    auto* clients = clients_opt.unwrap();
    size_t removed = 0;
    rusty::Vec<rusty::Arc<Client>> kept;
    kept.reserve(clients->len());
    for (const auto& client : *clients) {
      if (clients->len() - removed <= static_cast<size_t>(cfg.min_connections)) {
        kept.push(client.clone());
        continue;
      }
      if (!is_client_healthy(client)) {
        client->close();
        removed++;
      } else {
        kept.push(client.clone());
      }
    }
    *clients = std::move(kept);
    total_removed += removed;
    if (clients->is_empty()) {
      empty_keys.push(addr);
    }
  }
  for (const auto& addr : empty_keys) {
    cache_.remove(addr);
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

  rusty::Vec<std::string> keys = cache_.keys();
  rusty::Vec<std::string> empty_keys;
  for (const auto& addr : keys) {
    auto clients_opt = cache_.get(addr);
    if (clients_opt.is_none()) {
      continue;
    }
    auto* clients = clients_opt.unwrap();
    size_t closed = 0;
    rusty::Vec<rusty::Arc<Client>> kept;
    kept.reserve(clients->len());
    for (const auto& client : *clients) {
      if (clients->len() - closed <= static_cast<size_t>(cfg.min_connections)) {
        kept.push(client.clone());
        continue;
      }
      if (client->is_idle(cfg.idle_timeout_ms, current_time_ms)) {
        client->close();
        closed++;
      } else {
        kept.push(client.clone());
      }
    }
    *clients = std::move(kept);
    total_closed += closed;
    if (clients->is_empty()) {
      empty_keys.push(addr);
    }
  }
  for (const auto& addr : empty_keys) {
    cache_.remove(addr);
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
  size_t count = cache_.len();
  l_.unlock();
  return count;
}

// @unsafe - Reconnects all clients for a specific address
ClientPool::BulkReconnectResult ClientPool::reconnect_all(
    const std::string& addr, const BulkReconnectConfig& config) {

  BulkReconnectResult result{0, 0, 0, 0};

  // Collect clients to reconnect
  rusty::Vec<rusty::Arc<Client>> clients_to_reconnect;
  {
    l_.lock();
    auto clients_opt = cache_.get(addr);
    if (clients_opt.is_some()) {
      for (const auto& client : *clients_opt.unwrap()) {
        auto state = client->connection_state();
        if (config.skip_connected && state == ConnectionState::CONNECTED) {
          result.skipped++;
        } else {
          clients_to_reconnect.push(client);
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
    rusty::Vec<std::atomic<int>> batch_results(batch_end - i);
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
  rusty::Vec<std::string> addresses;
  {
    l_.lock();
    for (const auto& kv : cache_) {
      addresses.push(kv.first);
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
  auto lb_state_opt = lb_state_.get(addr);
  if (lb_state_opt.is_none()) {
    lb_state_.insert(addr, LoadBalancerState{});
    lb_state_opt = lb_state_.get(addr);
  }
  auto* lb_state = lb_state_opt.unwrap();

  auto clients_opt = cache_.get(addr);
  if (clients_opt.is_some()) {
    auto* clients = clients_opt.unwrap();
    int client_count = static_cast<int>(clients->size());

    // Use load balancer to select starting index
    size_t start_idx = LoadBalancer::select(
        cfg.load_balancing,
        *clients,
        *lb_state,
        rand_()
    );

    for (int i = 0; i < client_count; i++) {
      int idx = (start_idx + i) % client_count;
      auto& client = (*clients)[idx];

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
      for (auto& client : *clients) {
        client->close();
      }
      clients->clear();

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
        clients->push(client);
      }

      if (ok && !clients->is_empty()) {
        sp_cl = rusty::Some((*clients)[rand_() % clients->size()].clone());
      } else {
        // Remove from cache if we can't connect
        cache_.remove(addr);
      }
    }
  } else {
    // No cached connections - create new ones
    rusty::Vec<rusty::Arc<Client>> parallel_clients;
    bool ok = true;
    for (int i = 0; i < num_connections; i++) {
      auto client = Client::create(this->poll_thread_worker_.as_ref().unwrap().clone());
      client->set_client_mode(true);  // Jetpack: mark as client
      if (client->connect(addr.c_str()) != 0) {
        ok = false;
        break;
      }
      parallel_clients.push(client);
    }
    if (ok) {
      sp_cl = rusty::Some(parallel_clients[rand_() % parallel_clients.size()].clone());
      cache_.insert(addr, std::move(parallel_clients));
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
