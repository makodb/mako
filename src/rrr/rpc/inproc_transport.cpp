#include "inproc_transport.hpp"

#include <errno.h>
#include <string.h>
#include <unistd.h>

#include <mutex>
#include <unordered_map>

#include <sys/socket.h>

#include "rpc/server.hpp"
#include "rpc/utils.hpp"

namespace rrr {

namespace {

struct InprocEndpoint {
  rusty::sync::Weak<RpcServiceContext> ctx;
  rusty::sync::Weak<PollThread> server_poll_thread;
  rusty::sync::Weak<ServerListener> listener;
};

std::mutex g_inproc_mu;
std::unordered_map<std::string, InprocEndpoint> g_inproc_by_port;

std::string port_key_from_addr(const std::string& addr) {
  auto idx = addr.rfind(':');
  if (idx == std::string::npos || idx + 1 >= addr.size()) {
    return std::string();
  }
  return addr.substr(idx + 1);
}

}  // namespace

void inproc_register_server(const std::string& bind_addr,
                            const rusty::Arc<RpcServiceContext>& ctx,
                            const rusty::Arc<PollThread>& poll_thread,
                            const rusty::Arc<ServerListener>& listener) {
  const auto key = port_key_from_addr(bind_addr);
  if (key.empty()) {
    return;
  }
  std::lock_guard<std::mutex> lk(g_inproc_mu);
  g_inproc_by_port[key] = InprocEndpoint{
      rusty::downgrade(ctx),
      rusty::downgrade(poll_thread),
      rusty::downgrade(listener),
  };
}

void inproc_unregister_server(const std::string& bind_addr) {
  const auto key = port_key_from_addr(bind_addr);
  if (key.empty()) {
    return;
  }
  std::lock_guard<std::mutex> lk(g_inproc_mu);
  g_inproc_by_port.erase(key);
}

int inproc_try_connect(const std::string& addr,
                       const rusty::Arc<PollThread>& client_poll_thread,
                       int* out_client_fd) {
  if (!out_client_fd) {
    return EINVAL;
  }
  *out_client_fd = -1;

  const auto key = port_key_from_addr(addr);
  if (key.empty()) {
    return ENOTCONN;
  }

  InprocEndpoint ep;
  {
    std::lock_guard<std::mutex> lk(g_inproc_mu);
    auto it = g_inproc_by_port.find(key);
    if (it == g_inproc_by_port.end()) {
      return ENOTCONN;
    }
    ep = it->second;
  }

  auto ctx_opt = ep.ctx.upgrade();
  auto poll_opt = ep.server_poll_thread.upgrade();
  auto listener_opt = ep.listener.upgrade();
  if (ctx_opt.is_none() || poll_opt.is_none() || listener_opt.is_none()) {
    // Server was dropped; remove stale entry.
    std::lock_guard<std::mutex> lk(g_inproc_mu);
    g_inproc_by_port.erase(key);
    return ENOTCONN;
  }

  auto ctx = ctx_opt.unwrap();
  auto server_poll = poll_opt.unwrap();
  auto listener = listener_opt.unwrap();

  int fds[2] = {-1, -1};
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
    return errno ? errno : ENOTCONN;
  }

  const int server_fd = fds[0];
  const int client_fd = fds[1];

#ifdef __APPLE__
  // Prevent SIGPIPE termination on write() to closed sockets.
  const int yes = 1;
  (void)setsockopt(server_fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
  (void)setsockopt(client_fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
#endif

  // Make both ends non-blocking so they behave like the normal TCP sockets.
  if (set_nonblocking(server_fd, true) != 0 || set_nonblocking(client_fd, true) != 0) {
    ::close(server_fd);
    ::close(client_fd);
    return errno ? errno : EINVAL;
  }

  // Create a server-side connection and register it with the server poll thread.
  auto sconn = rusty::Arc<ServerConnection>::make(ctx.clone(), server_fd);
  ServerConnection::init_weak_self(sconn);
  {
    auto guard = listener->sconn_fds_.lock().unwrap();
    guard->push(server_fd);
  }
  server_poll->add(sconn);

  // Client side will be owned and closed by ClientConnection.
  *out_client_fd = client_fd;
  (void)client_poll_thread;  // reserved for future use
  return 0;
}

}  // namespace rrr
