#pragma once

#include "../__dep__.h"
#include "../constants.h"
#include "../communicator.h"

namespace janus {

class TxData;

// removed `class FpgaRaftForwardQuorumEvent`
// — only constructed by the now-deleted `FpgaRaftCommo::SendForward`.

class FpgaRaftPrepareQuorumEvent: public QuorumEventWrapper {
 public:
  using QuorumEventWrapper::QuorumEventWrapper;
//  ballot_t max_ballot_{0};
  bool HasAcceptedValue() {
    // TODO implement this
    return false;
  }
  void FeedResponse(bool y) {
    if (y) {
      vote_yes();
    } else {
      vote_no();
    }
  }
};

class FpgaRaftVoteQuorumEvent: public QuorumEventWrapper {
 public:
  using QuorumEventWrapper::QuorumEventWrapper;
  bool HasAcceptedValue() {
    return false;
  }
  void FeedResponse(bool y, ballot_t term) {
    if (y) {
      vote_yes();
    } else {
      vote_no();
      if(term > q().highest_term_)
      {
        q().highest_term_ = term ;
      }      
    }
  }
  
  int64_t Term() {
    return q().highest_term_;
  }
};

class FpgaRaftVote2FPGAQuorumEvent: public QuorumEventWrapper {
 public:
  using QuorumEventWrapper::QuorumEventWrapper;
  bool HasAcceptedValue() {
    return false;
  }
  void FeedResponse(bool y, ballot_t term) {
    if (y) {
      vote_yes();
    } else {
      vote_no();
      if(term > q().highest_term_)
      {
        q().highest_term_ = term ;
      }      
    }
  }
  
  int64_t Term() {
    return q().highest_term_;
  }
};

class FpgaRaftAcceptQuorumEvent: public QuorumEventWrapper {
 public:
  using QuorumEventWrapper::QuorumEventWrapper;
  void FeedResponse(bool y) {
    if (y) {
      vote_yes();
    } else {
      vote_no();
    }
    /*Log_debug("multi-paxos comm accept event, "
              "yes vote: %d, no vote: %d",
              n_voted_yes_, n_voted_no_);*/
  }
};

class FpgaRaftAppendQuorumEvent: public QuorumEventWrapper {
 public:
    uint64_t minIndex;
    using QuorumEventWrapper::QuorumEventWrapper;
    void FeedResponse(bool appendOK, uint64_t index, std::string ip_addr = "") {
        if (appendOK) {
            if ((q().n_voted_yes_ == 0) && (q().n_voted_no_ == 0))
                minIndex = index;
            else
                minIndex = std::min(minIndex, index);
            vote_yes();
        } else {
            vote_no();
        }
        /*Log_debug("fpga-raft comm accept event, "
                  "yes vote: %d, no vote: %d, min index: %d",
                  n_voted_yes_, n_voted_no_, minIndex);*/
    }
};



class FpgaRaftCommo : public Communicator {

friend class FpgaRaftProxy;
 public:
	std::unordered_map<siteid_t, uint64_t> matchedIndex {};
	int index;
	
  FpgaRaftCommo() = delete;
  FpgaRaftCommo(rusty::Option<rusty::Arc<PollThread>>);
  // removed `SendForward(par_id, self_id,
  // cmd)` declaration — only call site was the now-deleted
  // `CoordinatorFpgaRaft::Forward`.
	void BroadcastHeartbeat(parid_t par_id,
													uint64_t logIndex);
	void SendHeartbeat(parid_t par_id,
										 siteid_t site_id,
										 uint64_t logIndex);
	//ONLY FOR SIMULATION
  // take janus::Command;
  // shared_ptr<Marshallable> callers auto-convert.
  void SendAppendEntriesAgain(siteid_t site_id,
															parid_t par_id,
															slotid_t slot_id,
															ballot_t ballot,
															bool isLeader,
															uint64_t currentTerm,
															uint64_t prevLogIndex,
															uint64_t prevLogTerm,
															uint64_t commitIndex,
															const janus::Command& cmd);
  // removed dead `BroadcastPrepare` and
  // `BroadcastAccept` declarations (both shared_ptr-returning and
  // callback-style overloads).  Neither had any implementation in
  // `commo.cc` — they were declared but never defined, and no caller
  // anywhere invoked them (calls would have been link errors).  The
  // FpgaRaft path has its own `BroadcastAppendEntries` /
  // `BroadcastVote` / `BroadcastVote2FPGA` machinery instead.
  shared_ptr<FpgaRaftVoteQuorumEvent>
  BroadcastVote(parid_t par_id,
                        slotid_t lst_log_idx,
                        ballot_t lst_log_term,
                        parid_t self_id,
                        ballot_t cur_term );
  // removed deprecated callback-style
  // `void BroadcastVote(... callback)` — body had `verify(0);` and
  // no callers.
  shared_ptr<FpgaRaftVote2FPGAQuorumEvent>
  BroadcastVote2FPGA(parid_t par_id,
                        slotid_t lst_log_idx,
                        ballot_t lst_log_term,
                        parid_t self_id,
                        ballot_t cur_term );
  // removed deprecated callback-style
  // `void BroadcastVote2FPGA(... callback)` — body had `verify(0);`
  // and no callers.
  // take janus::Command;
  // shared_ptr<Marshallable> callers auto-convert.
  shared_ptr<FpgaRaftAppendQuorumEvent>
  BroadcastAppendEntries(parid_t par_id,
                         siteid_t leader_site_id,
                         slotid_t slot_id,
                         i64 dep_id,
                         ballot_t ballot,
                         bool isLeader,
                         uint64_t currentTerm,
                         uint64_t prevLogIndex,
                         uint64_t prevLogTerm,
                         uint64_t commitIndex,
                         const janus::Command& cmd);
  // removed deprecated callback-style
  // `void BroadcastAppendEntries(... callback)` — body had
  // `verify(0);` and no callers.
  // take janus::Command;
  // shared_ptr<Marshallable> callers auto-convert.
  void BroadcastDecide(const parid_t par_id,
                       const slotid_t slot_id,
											 const i64 dep_id,
                       const ballot_t ballot,
                       const janus::Command& cmd);
};

} // namespace janus

