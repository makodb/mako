#include "service.h"
#include "server.h"

#include "rrr/rrr.hpp"
#include <rusty/slice.hpp>
#include <algorithm>
#include <chrono>
#include <thread>

// @external: {
//   Log_info:   [safe, (...) -> void]
//   Log_debug:  [safe, (...) -> void]
//   Log_warn:   [safe, (...) -> void]
//   Log_error:  [safe, (...) -> void]
//   verify:     [safe, (...) -> void]
//   clock_gettime: [safe, (...) -> int]
//   srand:      [safe, (...) -> void]
//   rrr::Fiber::sleep: [safe, (int) -> void]
//   std::this_thread::sleep_for: [safe, (...) -> void]
// }

namespace janus {

using rusty::Result;

// Pure RPC availability decisions over copied booleans. Each call site still
// computes `disconnected` behind a `has_server && ...` guard so the generated
// helper cannot broaden raw-pointer evaluation.
#if RUSTYCPP_RUST
pub const fn raft_service_server_unavailable(has_server: bool,
                                             disconnected: bool,
                                             rpc_ready: bool) -> bool {
    !has_server || disconnected || !rpc_ready
}

pub const fn raft_service_poll_thread_available(found: bool,
                                                has_poll_thread: bool) -> bool {
    found && has_poll_thread
}

pub const fn raft_service_lease_is_open(state: u64,
                                        drain_bit: u64) -> bool {
    (state & drain_bit) == 0
}

pub const fn raft_service_lease_count(state: u64,
                                      count_mask: u64) -> u64 {
    state & count_mask
}
#endif
/*RUSTYCPP:GEN-BEGIN id=raft_service.scalar_decisions version=1 rust_sha256=45356dadcaa04044306377676bfdae5154d3753cbf61d55c7a9625103ee6939d*/
constexpr bool raft_service_server_unavailable(bool has_server, bool disconnected, bool rpc_ready);
constexpr bool raft_service_poll_thread_available(bool found, bool has_poll_thread);
constexpr bool raft_service_lease_is_open(uint64_t state, uint64_t drain_bit);
constexpr uint64_t raft_service_lease_count(uint64_t state, uint64_t count_mask);
constexpr bool raft_service_server_unavailable(bool has_server, bool disconnected, bool rpc_ready) {
    return (!has_server || rusty::detail::deref_if_pointer_like(disconnected)) || !rpc_ready;
}
constexpr bool raft_service_poll_thread_available(bool found, bool has_poll_thread) {
    return rusty::detail::deref_if_pointer_like(found) && rusty::detail::deref_if_pointer_like(has_poll_thread);
}
constexpr bool raft_service_lease_is_open(uint64_t state, uint64_t drain_bit) {
    return ((rusty::detail::deref_if_pointer_like(state) & rusty::detail::deref_if_pointer_like(drain_bit))) == static_cast<uint64_t>(0);
}
constexpr uint64_t raft_service_lease_count(uint64_t state, uint64_t count_mask) {
    return rusty::detail::deref_if_pointer_like(state) & rusty::detail::deref_if_pointer_like(count_mask);
}
/*RUSTYCPP:GEN-END id=raft_service.scalar_decisions*/

static_assert(raft_service_server_unavailable(false, false, false));
static_assert(raft_service_server_unavailable(true, true, true));
static_assert(raft_service_server_unavailable(true, false, false));
static_assert(!raft_service_server_unavailable(true, false, true));
static_assert(raft_service_poll_thread_available(true, true));
static_assert(!raft_service_poll_thread_available(false, true));

namespace {

constexpr uint64_t kServerLeaseDrainBit = uint64_t{1} << 63;
constexpr uint64_t kServerLeaseCountMask = ~kServerLeaseDrainBit;
constexpr int kServerLeaseDrainPollUs = 100;

static_assert(raft_service_lease_is_open(0, kServerLeaseDrainBit));
static_assert(!raft_service_lease_is_open(kServerLeaseDrainBit,
                                          kServerLeaseDrainBit));
static_assert(raft_service_lease_count(kServerLeaseDrainBit | 7,
                                       kServerLeaseCountMask) == 7);

}  // namespace

// =====================================================================
// Fiber-RPC handlers.
//
// Each method here is invoked by the rrr-generated wrapper on a fresh
// Fiber (see src/rrr/pylib/simplerpcgen/lang_cpp.py). We do synchronous
// work — including any PersistState fsync from RaftServer — and return
// the response struct by value. The framework marshals and sends the
// reply when the fiber completes; no DeferredReply anywhere.
//
// Disconnected/killed server path: fill the response with the same
// defaults the old RpcHandler macro's OnDisconnected##name bodies used,
// and return Ok(resp). We deliberately do NOT return Err(...): the
// peer code treats nonzero error codes as "drop this reply", which
// would hide the disconnected-server signal that other code paths
// depend on (e.g., lost-RPC detection in SendAppendEntries).
// =====================================================================

Result<RaftService::RpcVoteResponse, rrr::i32>
RaftServiceImpl::Vote(const RpcVoteRequest& req) {
  RpcVoteResponse resp{};
  auto server_lease = AcquireServerLease();
  RaftServer* svr = server_lease.get();
  bool has_server = svr != nullptr;
  bool disconnected = has_server && svr->IsDisconnected();
  bool rpc_ready = has_server && svr->IsRpcReady();
  if (raft_service_server_unavailable(
          has_server, disconnected, rpc_ready)) {
    resp.max_ballot = req.cur_term;
    resp.vote_granted = false;
    return Result<RpcVoteResponse, rrr::i32>::Ok(resp);
  }
  svr->OnRequestVote(req.lst_log_idx, req.lst_log_term,
                     req.site_id, req.cur_term,
                     &resp.max_ballot, &resp.vote_granted);
  return Result<RpcVoteResponse, rrr::i32>::Ok(resp);
}

Result<RaftService::RpcVoteDurableResponse, rrr::i32>
RaftServiceImpl::VoteDurable(const RpcVoteDurableRequest& req) {
  RpcVoteDurableResponse resp{};
  auto server_lease = AcquireServerLease();
  RaftServer* svr = server_lease.get();
  bool has_server = svr != nullptr;
  bool disconnected = has_server && svr->IsDisconnected();
  bool rpc_ready = has_server && svr->IsRpcReady();
  if (raft_service_server_unavailable(
          has_server, disconnected, rpc_ready)) {
    resp.acknowledged = false;
    return Result<RpcVoteDurableResponse, rrr::i32>::Ok(resp);
  }
  svr->OnVoteDurable(req.term, req.voter_id, &resp.acknowledged);
  return Result<RpcVoteDurableResponse, rrr::i32>::Ok(resp);
}

Result<RaftService::RpcAppendEntriesResponse, rrr::i32>
RaftServiceImpl::AppendEntries(const RpcAppendEntriesRequest& req) {
  RpcAppendEntriesResponse resp{};
  auto server_lease = AcquireServerLease();
  RaftServer* svr = server_lease.get();
  bool has_server = svr != nullptr;
  bool disconnected = has_server && svr->IsDisconnected();
  bool rpc_ready = has_server && svr->IsRpcReady();
  if (raft_service_server_unavailable(
          has_server, disconnected, rpc_ready)) {
    resp.followerAppendOK = 0;
    resp.followerCurrentTerm = 0;
    resp.followerLastLogIndex = 0;
    resp.followerAckType = 0;  // Memory
    return Result<RpcAppendEntriesResponse, rrr::i32>::Ok(resp);
  }
  svr->OnAppendEntries(req.slot, req.ballot, req.leaderCurrentTerm,
                       req.leaderSiteId, req.leaderPrevLogIndex,
                       req.leaderPrevLogTerm, req.leaderCommitIndex,
                       req.cmd, req.leaderNextLogTerm,
                       &resp.followerAppendOK, &resp.followerCurrentTerm,
                       &resp.followerLastLogIndex,
                       &resp.followerAckType);
  // OnAppendEntries publishes the strength of this exact call. Disabled,
  // failed, heartbeat-only, and asynchronous paths remain memory ACKs; async
  // durability is reported later through AppendEntriesDurable.
  return Result<RpcAppendEntriesResponse, rrr::i32>::Ok(resp);
}

Result<RaftService::RpcEmptyAppendEntriesResponse, rrr::i32>
RaftServiceImpl::EmptyAppendEntries(const RpcEmptyAppendEntriesRequest& req) {
  Log_debug("RaftServiceImpl: EmptyAppendEntries answering leader {}", req.leaderSiteId);
  RpcEmptyAppendEntriesResponse resp{};
  auto server_lease = AcquireServerLease();
  RaftServer* svr = server_lease.get();
  bool has_server = svr != nullptr;
  bool disconnected = has_server && svr->IsDisconnected();
  bool rpc_ready = has_server && svr->IsRpcReady();
  if (raft_service_server_unavailable(
          has_server, disconnected, rpc_ready)) {
    resp.followerAppendOK = 0;
    resp.followerCurrentTerm = 0;
    resp.followerLastLogIndex = 0;
    resp.followerAckType = 0;
    return Result<RpcEmptyAppendEntriesResponse, rrr::i32>::Ok(resp);
  }
  // OnAppendEntries uses the same fields as the non-empty variant with
  // an empty cmd and leaderNextLogTerm == 0 (heartbeat path).
  // followerAppendOK/Term/LastLogIndex are shared layout with the non-empty
  // response, so we can pass pointers directly into our resp struct.
  svr->OnAppendEntries(req.slot, req.ballot, req.leaderCurrentTerm,
                       req.leaderSiteId, req.leaderPrevLogIndex,
                       req.leaderPrevLogTerm, req.leaderCommitIndex,
                       janus::Command{}, 0,
                       &resp.followerAppendOK, &resp.followerCurrentTerm,
                       &resp.followerLastLogIndex,
                       &resp.followerAckType,
                       req.trigger_election_now);
  return Result<RpcEmptyAppendEntriesResponse, rrr::i32>::Ok(resp);
}

Result<RaftService::RpcAppendEntriesDurableResponse, rrr::i32>
RaftServiceImpl::AppendEntriesDurable(const RpcAppendEntriesDurableRequest& req) {
  RpcAppendEntriesDurableResponse resp{};
  auto server_lease = AcquireServerLease();
  RaftServer* svr = server_lease.get();
  bool has_server = svr != nullptr;
  bool disconnected = has_server && svr->IsDisconnected();
  bool rpc_ready = has_server && svr->IsRpcReady();
  if (raft_service_server_unavailable(
          has_server, disconnected, rpc_ready)) {
    resp.acknowledged = false;
    return Result<RpcAppendEntriesDurableResponse, rrr::i32>::Ok(resp);
  }
  svr->OnAppendEntriesDurable(req.term, req.follower_id,
                              req.lastLogIndex, &resp.acknowledged);
  return Result<RpcAppendEntriesDurableResponse, rrr::i32>::Ok(resp);
}

Result<RaftService::RpcTimeoutNowResponse, rrr::i32>
RaftServiceImpl::TimeoutNow(const RpcTimeoutNowRequest& req) {
  RpcTimeoutNowResponse resp{};
  auto server_lease = AcquireServerLease();
  RaftServer* svr = server_lease.get();
  bool has_server = svr != nullptr;
  bool disconnected = has_server && svr->IsDisconnected();
  bool rpc_ready = has_server && svr->IsRpcReady();
  if (raft_service_server_unavailable(
          has_server, disconnected, rpc_ready)) {
    resp.followerTerm = 0;
    resp.success = false;
    return Result<RpcTimeoutNowResponse, rrr::i32>::Ok(resp);
  }
  svr->OnTimeoutNow(req.leaderTerm, req.leaderSiteId,
                    &resp.followerTerm, &resp.success);
  return Result<RpcTimeoutNowResponse, rrr::i32>::Ok(resp);
}

Result<RaftService::RpcNotifyRestartResponse, rrr::i32>
RaftServiceImpl::NotifyRestart(const RpcNotifyRestartRequest& req) {
  Log_info("[NOTIFY-RESTART] Received restart notification from site {}",
           req.restartedSiteId);
  RpcNotifyRestartResponse resp{};
  auto server_lease = AcquireServerLease();
  RaftServer* svr = server_lease.get();
  bool has_server = svr != nullptr;
  bool disconnected = has_server && svr->IsDisconnected();
  bool rpc_ready = has_server && svr->IsRpcReady();
  if (raft_service_server_unavailable(
          has_server, disconnected, rpc_ready)) {
    resp.acknowledged = false;
    return Result<RpcNotifyRestartResponse, rrr::i32>::Ok(resp);
  }
  auto commo = svr->commo();
  if (commo != nullptr) {
    bool success = commo->ReconnectToSite(req.restartedSiteId,
                                          svr->partition_id_);
    resp.acknowledged = success;
    Log_info("[NOTIFY-RESTART] Reconnected to site {}: {}",
             req.restartedSiteId, success ? "success" : "failed");
  } else {
    resp.acknowledged = false;
    Log_warn("[NOTIFY-RESTART] commo is null, cannot reconnect to site {}",
             req.restartedSiteId);
  }
  // Invalidate speculative state for the peer that just restarted.
  svr->OnPeerRestart(req.restartedSiteId);
  return Result<RpcNotifyRestartResponse, rrr::i32>::Ok(resp);
}

Result<RaftService::RpcInstallSnapshotResponse, rrr::i32>
RaftServiceImpl::InstallSnapshot(const RpcInstallSnapshotRequest& req) {
  RpcInstallSnapshotResponse resp{};
  auto server_lease = AcquireServerLease();
  RaftServer* svr = server_lease.get();
  bool has_server = svr != nullptr;
  bool disconnected = has_server && svr->IsDisconnected();
  bool rpc_ready = has_server && svr->IsRpcReady();
  if (raft_service_server_unavailable(
          has_server, disconnected, rpc_ready)) {
    resp.term_out = 0;
    return Result<RpcInstallSnapshotResponse, rrr::i32>::Ok(resp);
  }
  svr->OnInstallSnapshot(req.term, req.leader_id,
                         req.last_included_index, req.last_included_term,
                         req.data, &resp.term_out);
  return Result<RpcInstallSnapshotResponse, rrr::i32>::Ok(resp);
}

Result<RaftService::RpcAddServerResponse, rrr::i32>
RaftServiceImpl::AddServer(const RpcAddServerRequest& req) {
  RpcAddServerResponse resp{};
  auto server_lease = AcquireServerLease();
  RaftServer* svr = server_lease.get();
  bool has_server = svr != nullptr;
  bool disconnected = has_server && svr->IsDisconnected();
  bool rpc_ready = has_server && svr->IsRpcReady();
  if (raft_service_server_unavailable(
          has_server, disconnected, rpc_ready)) {
    resp.success = false;
    resp.error_msg = "server down";
    resp.leader_hint = 0;
    return Result<RpcAddServerResponse, rrr::i32>::Ok(resp);
  }
  svr->OnAddServer(req.term, req.new_server_id, req.new_server_addr,
                   &resp.success, &resp.error_msg, &resp.leader_hint);
  return Result<RpcAddServerResponse, rrr::i32>::Ok(resp);
}

Result<RaftService::RpcRemoveServerResponse, rrr::i32>
RaftServiceImpl::RemoveServer(const RpcRemoveServerRequest& req) {
  RpcRemoveServerResponse resp{};
  auto server_lease = AcquireServerLease();
  RaftServer* svr = server_lease.get();
  bool has_server = svr != nullptr;
  bool disconnected = has_server && svr->IsDisconnected();
  bool rpc_ready = has_server && svr->IsRpcReady();
  if (raft_service_server_unavailable(
          has_server, disconnected, rpc_ready)) {
    resp.success = false;
    resp.error_msg = "server down";
    resp.leader_hint = 0;
    return Result<RpcRemoveServerResponse, rrr::i32>::Ok(resp);
  }
  svr->OnRemoveServer(req.term, req.server_id,
                      &resp.success, &resp.error_msg, &resp.leader_hint);
  return Result<RpcRemoveServerResponse, rrr::i32>::Ok(resp);
}

// =====================================================================
// Registry + lifecycle plumbing (unchanged from prior commit)
// =====================================================================

std::map<siteid_t, std::vector<RaftServiceImpl*>>
    RaftServiceImpl::service_registry_;
std::mutex RaftServiceImpl::registry_mutex_;

// @unsafe - The packed admission/count word is the lifetime proof for the raw
// server pointer. A successful increment always happens before the pointer
// load. Once the drain bit is set, the CAS cannot admit another handler.
RaftServiceImpl::ServerLease::ServerLease(RaftServiceImpl& owner) {
  auto admitted = owner.server_lease_state_.fetch_update(
      rusty::sync::atomic::Ordering::AcqRel,
      rusty::sync::atomic::Ordering::Acquire,
      [](uint64_t state) -> rusty::Option<uint64_t> {
        if (!raft_service_lease_is_open(state, kServerLeaseDrainBit)) {
          return rusty::None;
        }
        verify(raft_service_lease_count(state, kServerLeaseCountMask) <
               kServerLeaseCountMask);
        return rusty::Some(state + 1);
      });
  if (admitted.is_err()) {
    return;
  }

  owner_ = &owner;
  server_ = owner.svr_.load(rusty::sync::atomic::Ordering::Acquire);
  if (server_ == nullptr) {
    Release();
  }
}

RaftServiceImpl::ServerLease::~ServerLease() {
  Release();
}

// @unsafe - Release ordering keeps all handler accesses before the count can
// reach zero. UpdateServer observes zero with an Acquire load before returning.
void RaftServiceImpl::ServerLease::Release() {
  if (owner_ == nullptr) {
    return;
  }
  RaftServiceImpl* owner = owner_;
  owner_ = nullptr;
  server_ = nullptr;
  const uint64_t previous = owner->server_lease_state_.fetch_sub(
      1, rusty::sync::atomic::Ordering::Release);
  verify(raft_service_lease_count(previous, kServerLeaseCountMask) > 0);
}

RaftServiceImpl::ServerLease RaftServiceImpl::AcquireServerLease() {
  return ServerLease(*this);
}

// @unsafe - Close admission before publishing nullptr. The packed state makes
// the close-vs-increment race linearizable, so a zero count proves no handler
// can still dereference the old pointer. The gate keeps the drain bounded to
// handlers already admitted when teardown began.
void RaftServiceImpl::ReplaceServerAndDrain(RaftServer* new_svr) {
  const bool running_in_fiber = Fiber::current_fiber().is_some();
  while (server_replacement_active_.swap(
      true, rusty::sync::atomic::Ordering::AcqRel)) {
    if (running_in_fiber) {
      Fiber::sleep(kServerLeaseDrainPollUs);
    } else {
      std::this_thread::sleep_for(
          std::chrono::microseconds(kServerLeaseDrainPollUs));
    }
  }

  server_lease_state_.fetch_or(kServerLeaseDrainBit,
                               rusty::sync::atomic::Ordering::AcqRel);
  svr_.store(nullptr, rusty::sync::atomic::Ordering::Release);

  while (raft_service_lease_count(
             server_lease_state_.load(
                 rusty::sync::atomic::Ordering::Acquire),
             kServerLeaseCountMask) != 0) {
    if (running_in_fiber) {
      Fiber::sleep(kServerLeaseDrainPollUs);
    } else {
      std::this_thread::sleep_for(
          std::chrono::microseconds(kServerLeaseDrainPollUs));
    }
  }

  if (new_svr != nullptr) {
    // Publish the replacement before reopening admission. A successful
    // Acquire/Release CAS on the state then sees the new pointer publication.
    svr_.store(new_svr, rusty::sync::atomic::Ordering::Release);
    server_lease_state_.store(
        0, rusty::sync::atomic::Ordering::Release);
  }
  server_replacement_active_.store(
      false, rusty::sync::atomic::Ordering::Release);
}

// @unsafe - Publishes the Raft server behind the lease admission gate.
RaftServiceImpl::RaftServiceImpl(
    RaftServer* sched, rusty::Arc<rrr::PollThread> poll_thread)
    : poll_thread_(rusty::Some(std::move(poll_thread))) {
  svr_.store(sched, rusty::sync::atomic::Ordering::Release);
  site_id_ = sched->site_id_;
  {
    std::lock_guard<std::mutex> lock(registry_mutex_);
    service_registry_[site_id_].push_back(this);
  }
  struct timespec curr_time;
  clock_gettime(CLOCK_MONOTONIC_RAW, &curr_time);
  srand(curr_time.tv_nsec);
}

// @unsafe - ServiceBoxShim owns this concrete object. The explicit drain is a
// final lifetime barrier for any shutdown path that did not already clear the
// registry entry through UpdateServer.
RaftServiceImpl::~RaftServiceImpl() {
  ReplaceServerAndDrain(nullptr);
  std::lock_guard<std::mutex> lock(registry_mutex_);
  auto it = service_registry_.find(site_id_);
  if (it == service_registry_.end()) {
    return;
  }
  auto& services = it->second;
  services.erase(std::remove(services.begin(), services.end(), this),
                 services.end());
  if (services.empty()) {
    service_registry_.erase(it);
  }
}

void RaftServiceImpl::UpdateServer(siteid_t site_id, RaftServer* new_svr) {
  std::vector<RaftServiceImpl*> services;
  {
    std::lock_guard<std::mutex> lock(registry_mutex_);
    auto it = service_registry_.find(site_id);
    if (it != service_registry_.end()) {
      services = it->second;
    }
  }
  if (services.empty()) {
    Log_warn("[RAFT-SERVICE] UpdateServer: site {} not found in registry", site_id);
    return;
  }

  // Service proxies own RaftServiceImpl for the entire RaftTestConfig lifetime;
  // only the pointed-to RaftServer is replaced by Kill/Restart. Do not hold the
  // global registry mutex across the fiber-aware lease drain.
  for (RaftServiceImpl* service : services) {
    service->ReplaceServerAndDrain(new_svr);
  }
  Log_info("[RAFT-SERVICE] UpdateServer: site {} proxies={} -> {}",
           site_id, services.size(), (void*)new_svr);
}

rusty::Option<rusty::Arc<rrr::PollThread>>
RaftServiceImpl::GetPollThread(siteid_t site_id) {
  std::lock_guard<std::mutex> lock(registry_mutex_);
  auto it = service_registry_.find(site_id);
  bool found = it != service_registry_.end();
  if (!found) {
    return rusty::None;
  }
  for (RaftServiceImpl* service : it->second) {
    bool has_poll_thread = service->poll_thread_.is_some();
    if (raft_service_poll_thread_available(found, has_poll_thread)) {
      // Registration order is stable: the primary listener is first, followed
      // by any single-group stub listeners. Restart therefore reuses the
      // primary listener's original PollThread.
      return rusty::Some(service->poll_thread_.as_ref().unwrap().clone());
    }
  }
  return rusty::None;
}

} // namespace janus
