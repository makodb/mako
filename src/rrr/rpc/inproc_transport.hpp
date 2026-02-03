// @unsafe - In-process transport fallback uses socketpair and shared registries
#pragma once

#include <string>

#include <rusty/arc.hpp>

namespace rrr {

struct RpcServiceContext;
class PollThread;
class ServerListener;

// Register a server endpoint for in-process connections (keyed by port).
// Intended as a fallback when bind() fails with EPERM in restricted environments.
void inproc_register_server(const std::string& bind_addr,
                            const rusty::Arc<RpcServiceContext>& ctx,
                            const rusty::Arc<PollThread>& poll_thread,
                            const rusty::Arc<ServerListener>& listener);

void inproc_unregister_server(const std::string& bind_addr);

// Try to establish an in-process connection to a registered server.
// On success, returns 0 and sets *out_client_fd to a connected fd.
// On failure, returns an errno-style code (e.g., ENOTCONN).
int inproc_try_connect(const std::string& addr,
                       const rusty::Arc<PollThread>& client_poll_thread,
                       int* out_client_fd);

}  // namespace rrr

