#pragma once

#include "../__dep__.h"
#include "../constants.h"
#include "../communicator.h"

namespace janus {

class TxData;

class RaftVoteQuorumEvent: public QuorumEvent {
 public:
  using QuorumEvent::QuorumEvent;
  // @safe
  bool HasAcceptedValue() {
    return false;
  }

  // @safe
  void FeedResponse(bool y, ballot_t term) {
    if (y) {
      // @unsafe
      { VoteYes(); }  // 1 unsafe line: calls @unsafe parent method
    } else {
      // @unsafe
      { VoteNo(); }   // 1 unsafe line: calls @unsafe parent method
      if(term > highest_term_)
      {
        highest_term_ = term ;
      }
    }
  }

  // @safe
  int64_t Term() {
    return highest_term_;
  }
};

class SendAppendEntriesResults {
 public:
  std::recursive_mutex mtx;
  bool done = false;
  uint64_t ok = 0;
  uint64_t followerTerm = 0;
  uint64_t followerLastLogIndex = 0;
  bool empty = true;
};


class RaftCommo : public Communicator {

friend class RaftProxy;
 public:
#ifdef RAFT_TEST_CORO
  std::recursive_mutex rpc_mtx_ = {};
  uint64_t rpc_count_ = 0;
#endif
	
  RaftCommo() = delete;
  // @safe
  RaftCommo(rusty::Option<rusty::Arc<PollThread>> poll = rusty::None);

  // @safe
  shared_ptr<IntEvent>
  SendAppendEntries2(siteid_t site_id,
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
                    );

  // @safe
  shared_ptr<SendAppendEntriesResults>
  SendAppendEntries(siteid_t site_id,
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
                    bool trigger_election_now = false);
  // @safe
  shared_ptr<RaftVoteQuorumEvent>
  BroadcastVote(parid_t par_id,
                        slotid_t lst_log_idx,
                        ballot_t lst_log_term,
                        siteid_t self_id,
                        ballot_t cur_term );

  /**
   * SendTimeoutNow - Send TimeoutNow RPC to target replica
   *
   * Instructs target replica to start election immediately.
   * Used for leadership transfer protocol.
   *
   * @param site_id - Target replica (preferred leader)
   * @param par_id - Partition ID
   * @param leader_term - Current leader's term
   * @param leader_site_id - Current leader's site ID
   * @param callback - Called when RPC completes (success/failure)
   */

  // @safe
  void SendTimeoutNow(siteid_t site_id,
                      parid_t par_id,
                      uint64_t leader_term,
                      siteid_t leader_site_id,
                      std::function<void(bool success, uint64_t follower_term)> callback);
};

} // namespace janus

