// raft_transport.h's helpers: the steps of starting Raft's transport that
// need Mako's C++ -- its config, its logger, std::function. Every other call
// goes straight to the C ABI.

#include "raft_transport.h"

#include "../__dep__.h"
#include "../config.h"
#include "server.h"

namespace janus {
namespace raft_transport {

namespace {

// A std::function carried across the C ABI and run once on the poll thread.
void RunBoxedJob(void* ctx) {
  auto* job = static_cast<std::function<void()>*>(ctx);
  (*job)();
  delete job;
}

}  // namespace

// [M0] trace kit kernels (server.cc).
extern "C" void raft_trace_through(int32_t stage, uint64_t through, uint64_t t_us);
extern "C" uint64_t raft_trace_now_us();
extern "C" bool raft_trace_enabled();

RaftTransport* Serve(RaftServer* server, const std::string& bind_addr) {
  if (raft_trace_enabled()) {  // [M0] trace kit: hooks only when tracing
    raft_rt_install_trace(&raft_trace_through, &raft_trace_now_us);
  }
  RaftTransport* t = raft_transport_new();
  const int32_t ret =
      raft_transport_serve(t, server->impl(), bind_addr.c_str());
  if (ret != 0) {
    Log_fatal("Raft server launch failed at {}", bind_addr.c_str());
  }
  return t;
}

void ConnectPeers(RaftTransport* t) {
  auto config = Config::GetConfig();
  verify(config != nullptr);
  for (const auto par_id : config->GetAllPartitionIds()) {
    for (auto& site : config->SitesByPartitionId(par_id)) {
      const std::string addr = site.GetHostAddr();
      // Bound to a local: `verify` must not be handed a side-effecting call.
      const bool connected =
          raft_transport_add_peer(t, par_id, site.id, addr.c_str());
      verify(connected);
    }
  }
  // Disk builds start with a peer down (B22): add_peer left it to a dial
  // thread, and a majority suffices.
  const bool majority = raft_transport_wait_majority(t);
  verify(majority);
}

void Post(RaftTransport* t, std::function<void()> job) {
  raft_transport_post(t, &RunBoxedJob,
                      new std::function<void()>(std::move(job)));
}

}  // namespace raft_transport
}  // namespace janus
