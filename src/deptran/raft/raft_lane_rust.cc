// The Rust lane's worker plumbing: raft_lane.h over raft-rt's C ABI
// (transport_exports.h). Compiled only when MAKO_RAFT_LANE=rust.

#include "raft_lane.h"

#include "../__dep__.h"
#include "../config.h"
#include "server.h"
#include "transport_exports.h"

namespace janus {
namespace raft_lane {

namespace {

// A std::function carried across the C ABI and run once on the poll thread.
void RunBoxedJob(void* ctx) {
  auto* job = static_cast<std::function<void()>*>(ctx);
  (*job)();
  delete job;
}

}  // namespace

RaftTransport* Serve(RaftServer* server, const std::string& bind_addr) {
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
}

void Post(RaftTransport* t, std::function<void()> job) {
  raft_transport_post(t, &RunBoxedJob,
                      new std::function<void()>(std::move(job)));
}

void SetAdmissionReady(RaftTransport* t, bool ready) {
  raft_transport_set_admission_ready(t, ready);
}

bool Drain(RaftTransport* t, uint64_t timeout_ms) {
  return raft_transport_drain(t, timeout_ms);
}

void CloseServer(RaftTransport* t) { raft_transport_close_server(t); }

void Destroy(RaftTransport* t) { raft_transport_delete(t); }

uint64_t RpcCount(const RaftTransport* t) { return raft_transport_rpc_count(t); }

void* ServeStub(RaftTransport* t, RaftServer* server,
                const std::string& bind_addr) {
  return raft_transport_serve_stub(t, server->impl(), bind_addr.c_str());
}

void StubSetAdmissionReady(void* stub, bool ready) {
  raft_stub_server_set_admission_ready(static_cast<RaftStubServer*>(stub), ready);
}

bool StubDrain(void* stub, uint64_t timeout_ms) {
  return raft_stub_server_drain(static_cast<RaftStubServer*>(stub), timeout_ms);
}

void StubDestroy(void* stub) {
  raft_stub_server_delete(static_cast<RaftStubServer*>(stub));
}

int RunLab() { return raft_rt_run_lab(); }

}  // namespace raft_lane
}  // namespace janus
