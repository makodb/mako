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
// }


using namespace std;

namespace rrr {

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
    auto guard = state_.lock();
    if (!guard->timed_out) {
      guard->ready = true;
    }
    should_callback = guard->ready;
  }
  // Notify after releasing state lock
  ready_cond_.get()->notify_all();

  // Execute callback outside lock to avoid deadlock
  if (should_callback && attr_.callback != nullptr) {
    // SAFE: Callback receives Arc<Future> for lifetime safety
    auto x = attr_.callback;
    Coroutine::CreateRun([x, self]() {  // Capture Arc, not raw pointer
      x(self);  // Callback receives Arc<Future>
    });
  }
}

// @unsafe - Cancels all pending futures with error
// SAFETY: Protected by spinlock, proper refcount management
void Client::invalidate_pending_futures() const {
  list<rusty::Arc<Future>> futures;
  pending_fu_l_.get()->lock();
  for (auto& it: *pending_fu_.borrow()) {
    futures.push_back(it.second);  // Copy Arc
  }
  pending_fu_.borrow_mut()->clear();  // Clear map (releases its Arc references)
  pending_fu_l_.get()->unlock();

  for (auto& fu: futures) {
    fu->error_code_.set(ENOTCONN);
    fu->notify_ready(fu);  // Pass Arc to self for callback safety
    // Arc auto-released when list destroyed
  }
}

// @unsafe - Closes socket and invalidates futures
// SAFETY: Idempotent, proper cleanup sequence
void Client::close() const {
  if (status_.get() == CONNECTED) {
    // const_cast needed: Arc gives const access but remove() needs non-const reference
    poll_thread_worker_->remove(const_cast<Client&>(*this));
    ::close(sock_.get());
  }
  status_.set(CLOSED);
  invalidate_pending_futures();
}

// @unsafe - Establishes TCP/IPC connection to server
// SAFETY: Proper socket creation, configuration, and error handling
int Client::connect(const char* addr) const {
  verify(status_.get() != CONNECTED);
  string addr_str(addr);
  size_t idx = addr_str.find(":");
  if (idx == string::npos) {
    Log_error("rrr::Client: bad connect address: %s", addr);
    return EINVAL;
  }
  string host = addr_str.substr(0, idx);
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
  sock_.set(sock);
  auto len = sizeof(saun.sun_family) + strlen(saun.sun_path)+1;
  if (::connect(sock_.get(), (struct sockaddr*)&saun, len) < 0) {
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
    Log_error("rrr::Client: getaddrinfo(): %s", gai_strerror(r));
    return EINVAL;
  }

  for (rp = result; rp != nullptr; rp = rp->ai_next) {
    int sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (sock == -1) {
      continue;
    }
    sock_.set(sock);

    const int yes = 1;
    verify(setsockopt(sock_.get(), SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == 0);
    verify(setsockopt(sock_.get(), IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes)) == 0);
    int buf_len = 1024 * 1024;
    setsockopt(sock_.get(), SOL_SOCKET, SO_RCVBUF, &buf_len, sizeof(buf_len));
    setsockopt(sock_.get(), SOL_SOCKET, SO_SNDBUF, &buf_len, sizeof(buf_len));

    if (::connect(sock_.get(), rp->ai_addr, rp->ai_addrlen) == 0) {
      break;
    }
    ::close(sock_.get());
    sock_.set(-1);
  }
  freeaddrinfo(result);

  if (rp == nullptr) {
    // failed to connect
    Log_error("rrr::Client: connect(%s): %s", addr, strerror(errno));
    return ENOTCONN;
  }
#endif
  verify(set_nonblocking(sock_.get(), true) == 0);
  Log_debug("rrr::Client: connected to %s", addr);

  status_.set(CONNECTED);

  // Use weak_self_ instead of shared_from_this()
  auto self = weak_self_.borrow()->upgrade();
  if (self.is_some()) {
    poll_thread_worker_->add(self.unwrap());
  } else {
    Log_error("rrr::Client: weak_self_ upgrade failed - client may not have been created with factory method");
    return EINVAL;
  }

  return 0;
}

// @safe - Simple error handler
void Client::handle_error() {
  close();
}

// @unsafe - Writes buffered data to socket
// SAFETY: Protected by spinlock, handles partial writes
// Returns new poll mode, or MODE_NO_CHANGE if no update needed
int Client::handle_write() {
  if (status_.get() != CONNECTED) {
    return Pollable::MODE_NO_CHANGE;
  }

  int result = Pollable::MODE_NO_CHANGE;
  out_l_.get()->lock();
  out_.borrow_mut()->write_to_fd(sock_.get());
  if (out_.borrow()->empty()) {
    // Return READ-only mode - PollThreadWorker will update epoll
    result = Pollable::READ;
  }
  out_l_.get()->unlock();
  return result;
}

// @unsafe - Reads and processes RPC responses
// SAFETY: Protected by spinlock, validates packet structure
void Client::handle_read() {
  if (status_.get() != CONNECTED) {
    return;
  }

  int bytes_read = in_.borrow_mut()->read_from_fd(sock_.get());

  // Optimization: If no new data was read AND buffer is empty, return early.
  // CRITICAL BUG FIX: With edge-triggered epoll (EPOLLET), we MUST check if
  // there's buffered data to process. The old code would return early even when
  // content_size() > 0, causing futures to hang because buffered packets never
  // got processed and we'd lose the edge trigger.
  if (bytes_read == 0 && in_.borrow()->content_size() == 0) {
    return;
  }

  // Process all complete packets in the buffer
  for (;;) {
    //Log_info("stuck in client handle_read loop");
    i32 packet_size;
    int n_peek = in_.borrow_mut()->peek(&packet_size, sizeof(i32));
    if (n_peek == sizeof(i32)
        && in_.borrow()->content_size() >= packet_size + sizeof(i32)) {
      // consume the packet size
      verify(in_.borrow_mut()->read(&packet_size, sizeof(i32)) == sizeof(i32));

      v64 v_reply_xid;
      v32 v_error_code;

      *in_.borrow_mut() >> v_reply_xid >> v_error_code;

      pending_fu_l_.get()->lock();
      auto it = pending_fu_.borrow_mut()->find(v_reply_xid.get());
      if (it != pending_fu_.borrow_mut()->end()) {
        rusty::Arc<Future> fu = it->second;  // Copy Arc (refcount still 2)
        verify(fu->xid_ == v_reply_xid.get());
        pending_fu_.borrow_mut()->erase(it);  // Remove from map (refcount 2→1)
        pending_fu_l_.get()->unlock();

        fu->error_code_.set(v_error_code.get());
        fu->reply_.get()->read_from_marshal(*in_.borrow_mut(),
                                            packet_size - v_reply_xid.val_size()
                                                - v_error_code.val_size());

        fu->notify_ready(fu);  // Pass Arc to self for callback safety

        // Arc auto-released when scope exits (refcount 1→0 if user released theirs)
      } else {
        // the future might timed out
        pending_fu_l_.get()->unlock();
      }

    } else {
      // packet incomplete or no more packets to process
      break;
    }
  }
}

// @unsafe - Determines polling mode based on output buffer
// SAFETY: Uses RefCell borrow operations
int Client::poll_mode() const {
  int mode = Pollable::READ;
  out_l_.get()->lock();
  if (!out_.borrow()->empty()) {
    mode |= Pollable::WRITE;
  }
  out_l_.get()->unlock();
  return mode;
}

// @unsafe - Starts new RPC request with marshaling
// SAFETY: Protected by spinlocks, proper refcounting
FutureResult Client::begin_request(i32 rpc_id, const FutureAttr& attr /* =... */) const {
  out_l_.get()->lock();

  if (status_.get() != CONNECTED) {
    return FutureResult::Err(ENOTCONN);
  }

  auto fu = Future::create(xid_counter_.borrow_mut()->next(), attr);
  pending_fu_l_.get()->lock();
  pending_fu_.borrow_mut()->insert_or_assign(fu->xid_, fu);  // Store Arc in map (refcount now 2)
  pending_fu_l_.get()->unlock();
  //Log_info("Starting a new request with rpc_id %ld,xid_:%llu", rpc_id,fu->xid_);
  // check if the client gets closed in the meantime
  if (status_.get() != CONNECTED) {
    pending_fu_l_.get()->lock();
    auto it = pending_fu_.borrow_mut()->find(fu->xid_);
    if (it != pending_fu_.borrow_mut()->end()) {
      pending_fu_.borrow_mut()->erase(it);  // Arc auto-released when removed from map
    }
    pending_fu_l_.get()->unlock();

    return FutureResult::Err(ENOTCONN);
  }

  // Separate the borrows to avoid overlapping mutable borrows
  Marshal::bookmark* bm = out_.borrow_mut()->set_bookmark(sizeof(i32)); // will fill packet size later
  *bmark_.borrow_mut() = rusty::Some(rusty::Box<Marshal::bookmark>(bm));

  *this << v64(fu->xid_);
  *this << rpc_id;

  // Arc is in pending_fu_ (refcount=2), return copy to caller
  return FutureResult::Ok(fu);
}

// @unsafe - Finalizes request packet with size header
// SAFETY: Updates bookmark, enables write polling
void Client::end_request() const {
  // set reply size in packet
  if (bmark_.borrow()->is_some()) {
    i32 request_size = out_.borrow_mut()->get_and_reset_write_cnt();
    out_.borrow_mut()->write_bookmark(&*bmark_.borrow_mut()->as_mut().unwrap(), &request_size);
    *bmark_.borrow_mut() = rusty::None;  // Reset to None (automatically deletes old value)
  }

  // always enable write events since the code above gauranteed there
  // will be some data to send
  // NOTE: end_request() is called from user threads, NOT the poll thread.
  // Must use channel-based update_mode() - the direct worker() path is only safe
  // for poll handlers (handle_read/handle_write) running on the poll thread.
  poll_thread_worker_->update_mode(const_cast<Client&>(*this), Pollable::READ | Pollable::WRITE);

  out_l_.get()->unlock();
}

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
