
#include "commo.h"
#include "../rcc/graph.h"
#include "../rcc/graph_marshaler.h"
#include "../command.h"
#include "../procedure.h"
#include "../command_marshaler.h"
#include "../rcc_rpc.h"
#include "macros.h"
#include <utility>

// @external: {
//   std::vector::vector: [unsafe, () -> owned]
//   std::shared_ptr::shared_ptr: [unsafe, () -> owned]
//   std::function::operator bool: [unsafe, () -> bool]
// }

namespace janus {

// @safe
RaftCommo::RaftCommo(rusty::Option<rusty::Arc<PollThread>> poll) : Communicator(poll) {
//  verify(poll != nullptr);
}

// @safe - Calls undeclared Reactor::CreateSpEvent() variadic template functions
shared_ptr<IntEvent>
RaftCommo::SendAppendEntries2(siteid_t site_id,
                             parid_t par_id,
                             slotid_t slot_id,
                             ballot_t ballot,
                             bool isLeader,
                             siteid_t leader_site_id,
                             uint64_t currentTerm,
                             uint64_t prevLogIndex,
                             uint64_t prevLogTerm,
                             uint64_t commitIndex,
                             shared_ptr<Marshallable> cmd,
                             uint64_t cmdLogTerm,
                             uint64_t* ret_status,
                             uint64_t* ret_term,
                             uint64_t* ret_last_log_index
                             ) {
  // verify(par_id == 0);                          
  auto ret = Reactor::CreateSpEvent<IntEvent>();
  auto proxies = rpc_par_proxies_[par_id];
  vector<rusty::Arc<Future>> fus;
	WAN_WAIT;
  for (auto& p : proxies) {
    if (p.first != site_id)
        continue;
		auto follower_id = p.first;
    auto proxy = (RaftProxy*) p.second;
    FutureAttr fuattr;
    fuattr.callback = [ret,ret_status,ret_term,ret_last_log_index](rusty::Arc<Future> fu) {
      if (fu->get_error_code() != 0) {
        Log_info("Get a error message in reply");
        return;
      }
      fu->get_reply() >> *ret_status >> *ret_term >> *ret_last_log_index;
      ret->Set(1);
    };

    if (cmd == nullptr) {
      // send a heartbeat AppendEntries
      Log_debug("Heartbeat AppendEntries to site %d prevLogIndex=%ld", site_id, prevLogIndex);
      Call_Async(proxy, EmptyAppendEntries, slot_id,
                                            ballot,
                                            currentTerm,
                                            leader_site_id,
                                            prevLogIndex,
                                            prevLogTerm,
                                            commitIndex,
                                            false,  // trigger_election_now (always false for SendAppendEntries2)
                                            fuattr);
    } else {
      // send a regular AppendEntries
      MarshallDeputy md(cmd);
      verify(md.sp_data_ != nullptr);

      Log_debug("AppendEntries to site %d for log index %d", site_id, prevLogIndex + 1);
      Call_Async(proxy, AppendEntries, slot_id,
                                       ballot,
                                       currentTerm,
                                       leader_site_id,
                                       prevLogIndex,
                                       prevLogTerm,
                                       commitIndex,
                                       md,
                                       cmdLogTerm,
                                       fuattr);
    }
  }
  return ret;
}

// @safe
shared_ptr<SendAppendEntriesResults>
RaftCommo::SendAppendEntries(siteid_t site_id,
                             parid_t par_id,
                             slotid_t slot_id,
                             ballot_t ballot,
                             bool isLeader,
                             siteid_t leader_site_id,
                             uint64_t currentTerm,
                             uint64_t prevLogIndex,
                             uint64_t prevLogTerm,
                             uint64_t commitIndex,
                             shared_ptr<Marshallable> cmd,
                             uint64_t cmdLogTerm,
                             bool trigger_election_now) {
  // verify(par_id == 0);
  // Use direct shared_ptr construction instead of make_shared (template function)
  // to keep this function @safe. The 'new' operator is allowed in @safe code.
  auto res = shared_ptr<SendAppendEntriesResults>(new SendAppendEntriesResults());
  auto proxies = rpc_par_proxies_[par_id];
  vector<rusty::Arc<Future>> fus;
	WAN_WAIT;
  for (auto& p : proxies) {
    if (p.first != site_id)
        continue;
		auto follower_id = p.first;
    auto proxy = (RaftProxy*) p.second;
    FutureAttr fuattr;
    fuattr.callback = [res, cmd](rusty::Arc<Future> fu) {
      if (fu->get_error_code() != 0) {
        Log_info("Get a error message in reply");
        return;
      }
      // std::lock_guard<std::recursive_mutex> lk(res->mtx);
      fu->get_reply() >> res->ok;
      fu->get_reply() >> res->followerTerm;
      fu->get_reply() >> res->followerLastLogIndex;
      res->empty = (cmd == nullptr);
      // false, 0, 0 is the return value reserved to simulate a lost RPC.
      // only set res->done if it's not a lost RPC
      if (res->ok == false && res->followerTerm == 0 && res->followerLastLogIndex == 0) {
        res->done = false;
      } else {
        res->done = true;
      }
    };

    if (cmd == nullptr) {
      // send a heartbeat AppendEntries
      Log_debug("Heartbeat AppendEntries to site %d prevLogIndex=%ld trigger_election=%d",
                site_id, prevLogIndex, trigger_election_now);
      Call_Async(proxy, EmptyAppendEntries, slot_id,
                                            ballot,
                                            currentTerm,
                                            leader_site_id,
                                            prevLogIndex,
                                            prevLogTerm,
                                            commitIndex,
                                            trigger_election_now,
                                            fuattr);
    } else {
      // send a regular AppendEntries
      MarshallDeputy md(cmd);
      verify(md.sp_data_ != nullptr);

      Log_debug("AppendEntries to site %d for log index %d", site_id, prevLogIndex + 1);
      Call_Async(proxy, AppendEntries, slot_id,
                                       ballot,
                                       currentTerm,
                                       leader_site_id,
                                       prevLogIndex,
                                       prevLogTerm,
                                       commitIndex,
                                       md,
                                       cmdLogTerm,
                                       fuattr);
    }
  }
  return res;
}

// @safe - Calls undeclared Reactor::CreateSpEvent()
shared_ptr<RaftVoteQuorumEvent>
RaftCommo::BroadcastVote(parid_t par_id,
                         slotid_t lst_log_idx,
                         ballot_t lst_log_term,
                         siteid_t self_id,
                         ballot_t cur_term ) {
  int n = Config::GetConfig()->GetPartitionSize(par_id);
  auto e = Reactor::CreateSpEvent<RaftVoteQuorumEvent>(n, n/2);
  auto proxies = rpc_par_proxies_[par_id];
  WAN_WAIT;
  for (auto& p : proxies) {
    auto site_id = p.first;
    if (site_id == self_id) {
      continue;
    }
    auto proxy = (RaftProxy*) p.second;
    FutureAttr fuattr;
    fuattr.callback = [e](rusty::Arc<Future> fu) {
      if (fu->get_error_code() != 0) {
        Log_info("Get a error message in reply");
        return;
      }
      ballot_t term = 0;
      bool_t vote = false ;
      fu->get_reply() >> term;
      fu->get_reply() >> vote ;
      e->FeedResponse(vote, term);
      // TODO add max accepted value.
    };
    Call_Async(proxy, Vote, lst_log_idx, lst_log_term, self_id, cur_term, fuattr);
  }
  return std::move(e);
}

// ============================================================================
// TimeoutNow RPC - Leadership Transfer Protocol
// ============================================================================

/**
 * SendTimeoutNow - Send TimeoutNow RPC to target replica
 *
 * Instructs target replica to start election immediately.
 * Part of leadership transfer protocol.
 *
 * Edge Cases Handled:
 * - RPC fails (network error) → callback(false, 0)
 * - RPC succeeds but target rejects → callback(false, follower_term)
 * - RPC succeeds and target starts election → callback(true, follower_term)
 */
// @safe
void RaftCommo::SendTimeoutNow(siteid_t site_id,
                               parid_t par_id,
                               uint64_t leader_term,
                               siteid_t leader_site_id,
                               std::function<void(bool success, uint64_t follower_term)> callback) {
  auto proxies = rpc_par_proxies_[par_id];

  // Find the target proxy
  for (auto& p : proxies) {
    if (p.first != site_id) {
      continue;
    }

    auto proxy = (RaftProxy*) p.second;
    FutureAttr fuattr;

    fuattr.callback = [callback](rusty::Arc<Future> fu) {
      if (fu->get_error_code() != 0) {
        // RPC failed (network error, timeout, etc.)
        Log_warn("[TIMEOUT-NOW-RPC] Failed to send TimeoutNow - network error (code=%d)",
                 fu->get_error_code());
        if (callback) {
          callback(false, 0);
        }
        return;
      }

      // RPC succeeded - extract response
      uint64_t follower_term = 0;
      bool_t success = false;

      fu->get_reply() >> follower_term;
      fu->get_reply() >> success;

      Log_info("[TIMEOUT-NOW-RPC] TimeoutNow RPC completed: success=%d, follower_term=%lu",
               (int)success, follower_term);

      if (callback) {
        callback(success, follower_term);
      }
    };

    Log_info("[TIMEOUT-NOW-RPC] Sending TimeoutNow to site %d (term=%lu)",
             site_id, leader_term);

    // Send TimeoutNow RPC
    Call_Async(proxy, TimeoutNow, leader_term, leader_site_id, fuattr);

    return;  // Found and sent to target
  }

  // Target not found in proxy list
  Log_warn("[TIMEOUT-NOW-RPC] Failed to send TimeoutNow - site %d not found in proxies",
           site_id);
  if (callback) {
    callback(false, 0);
  }
}

} // namespace janus
