#pragma once

#include "__dep__.h"
#include "constants.h"
#include "../rcc_rpc.h"
#include "server.h"
#include <cstdint>
#include <mutex>
#include <map>
#include <vector>
#include <rusty/sync/atomic.hpp>

// @external: {
//   verify: [safe, (bool) -> void],
//   clock_gettime: [safe, (int, timespec*) -> int],
//   srand: [safe, (unsigned int) -> void]
// }

namespace janus {

class RaftServer;

// @unsafe - inherits from non-@interface RaftService
class RaftServiceImpl : public RaftService {
 public:
  // Static registry to find every service proxy for a site. Single-group
  // deployments expose the same RaftServer through the primary listener and
  // one stub listener per extra partition, so a site can have multiple
  // RaftServiceImpl instances that must all be closed before server teardown.
  static std::map<siteid_t, std::vector<RaftServiceImpl*>> service_registry_;
  static std::mutex registry_mutex_;

  // Rust-style atomic pointer - allows lock-free reads on RPC hot path.
  rusty::sync::atomic::AtomicPtr<RaftServer> svr_{nullptr};
  siteid_t site_id_;

  // Store the poll thread so Restart() can reuse the original, ensuring
  // inbound and outbound RPCs share a thread.
  rusty::Option<rusty::Arc<rrr::PollThread>> poll_thread_;

  // @unsafe - Registers the Raft server and RPC owner handle.
  RaftServiceImpl(RaftServer* sched,
                  rusty::Arc<rrr::PollThread> poll_thread);
  // @unsafe - Closes admission, drains borrowers, and unregisters this proxy.
  ~RaftServiceImpl();

  // Called by test framework during Kill/Restart to update server pointer
  // @unsafe - Registry lookup and raw server lifetime transition.
  static void UpdateServer(siteid_t site_id, RaftServer* new_svr);

  // Called by test framework during Restart to get the original poll thread
  // @unsafe - Locks the legacy global service registry.
  static rusty::Option<rusty::Arc<rrr::PollThread>> GetPollThread(siteid_t site_id);

  // Generated fiber-RPC overrides. The rrr codegen wraps each one in a
  // Fiber::create_run; we return a packed response struct and the
  // framework sends the reply on fiber completion. No DeferredReply.
  rusty::Result<RpcVoteResponse,                rrr::i32> Vote(const RpcVoteRequest& req) override;
  rusty::Result<RpcVoteDurableResponse,         rrr::i32> VoteDurable(const RpcVoteDurableRequest& req) override;
  rusty::Result<RpcAppendEntriesResponse,       rrr::i32> AppendEntries(const RpcAppendEntriesRequest& req) override;
  rusty::Result<RpcEmptyAppendEntriesResponse,  rrr::i32> EmptyAppendEntries(const RpcEmptyAppendEntriesRequest& req) override;
  rusty::Result<RpcAppendEntriesDurableResponse, rrr::i32> AppendEntriesDurable(const RpcAppendEntriesDurableRequest& req) override;
  rusty::Result<RpcTimeoutNowResponse,          rrr::i32> TimeoutNow(const RpcTimeoutNowRequest& req) override;
  rusty::Result<RpcNotifyRestartResponse,       rrr::i32> NotifyRestart(const RpcNotifyRestartRequest& req) override;
  rusty::Result<RpcInstallSnapshotResponse,     rrr::i32> InstallSnapshot(const RpcInstallSnapshotRequest& req) override;
  rusty::Result<RpcAddServerResponse,           rrr::i32> AddServer(const RpcAddServerRequest& req) override;
  rusty::Result<RpcRemoveServerResponse,        rrr::i32> RemoveServer(const RpcRemoveServerRequest& req) override;

 private:
  // A ServerLease is the only safe way for an RPC handler to retain the raw
  // RaftServer pointer. Admission is counted before the pointer load, and the
  // count remains held through the complete handler call. UpdateServer closes
  // admission before clearing svr_ and waits for admitted handlers to leave.
  // SAFETY: do not move the pointer out of this object's lifetime.
  class ServerLease {
   public:
    // @unsafe - Admits one raw-pointer borrower through the packed gate.
    explicit ServerLease(RaftServiceImpl& owner);
    // @unsafe - Releases that borrower before the pointer can be reclaimed.
    ~ServerLease();

    ServerLease(const ServerLease&) = delete;
    ServerLease& operator=(const ServerLease&) = delete;
    ServerLease(ServerLease&&) = delete;
    ServerLease& operator=(ServerLease&&) = delete;

    // @safe - Returns the pointer only while this lease remains alive.
    RaftServer* get() const { return server_; }

   private:
    // @unsafe - Atomically releases one admitted raw-pointer borrower.
    void Release();

    RaftServiceImpl* owner_{nullptr};
    RaftServer* server_{nullptr};
  };

  // @unsafe - Constructs a scoped raw-pointer lifetime lease.
  ServerLease AcquireServerLease();
  // @unsafe - Serializes replacement, closes admission, and drains leases.
  void ReplaceServerAndDrain(RaftServer* new_svr);

  // The top bit closes admission; the remaining bits count admitted handlers.
  // Keeping both in one atomic makes the increment-vs-close race linearizable.
  rusty::sync::atomic::AtomicU64 server_lease_state_{0};
  // UpdateServer releases the global registry lock before a fiber-aware drain.
  // This per-service gate serializes concurrent kill/restart transitions.
  rusty::sync::atomic::AtomicBool server_replacement_active_{false};
};

} // namespace janus
