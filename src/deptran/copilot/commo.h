#pragma once

#include "../__dep__.h"
#include "../constants.h"
#include "../communicator.h"
#include "server.h"

namespace janus {

class CopilotFastAcceptQuorumEvent : public QuorumEventWrapper {
  // TODO: use WaitAny to express fastpath vs. slowpath?
  vector<uint64_t> ret_deps_;
  int32_t n_fastac_ok_{0};
  int32_t n_fastac_reply_{0};
 public:
  // using QuorumEventWrapper::QuorumEventWrapper;
  CopilotFastAcceptQuorumEvent(int n_total, int quorum)
      : QuorumEventWrapper(n_total, quorum) {
    ret_deps_.reserve(n_total);
  }

  void FeedResponse(bool y, bool ok);
  void FeedRetDep(uint64_t dep);
  uint64_t GetFinalDep();

  bool FastYes();
  bool FastNo();
};

class CopilotAcceptQuorumEvent : public QuorumEventWrapper {
 public:
  using QuorumEventWrapper::QuorumEventWrapper;

  void FeedResponse(bool y) {
    if (y)
      vote_yes();
    else
      vote_no();
  }
};

class CopilotPrepareQuorumEvent : public QuorumEventWrapper {
  vector<vector<CopilotData> > ret_cmds_by_status_;

 public:
  // committed_seen_ + the readiness short-circuit now live on QuorumEvent
  // as QuorumPolicy::COMMITTED_SHORT (S3).
  CopilotPrepareQuorumEvent(int n_total, int quorum)
      : QuorumEventWrapper(n_total, quorum), ret_cmds_by_status_(n_status) {
    q().policy_.set(QuorumPolicy::COMMITTED_SHORT);
  }

  void FeedResponse(bool y) {
    if (y)
      vote_yes();
    else
      vote_no();
  }

  // takes janus::Command;
  // shared_ptr<Marshallable> callers auto-convert.
  void FeedRetCmd(ballot_t ballot,
                  uint64_t dep,
                  uint8_t is_pilot, slotid_t slot,
                  const janus::Command& cmd,
                  enum Status status);
  size_t GetCount(enum Status status);
  vector<CopilotData>& GetCmds(enum Status status);
  void Show();
};

/**
 * A "Quorum Event" which has no quorum
 * Used for those who don't need quorum reply
 */
class CopilotFakeQuorumEvent : public QuorumEventWrapper {
 public:
  // Readiness now lives on QuorumEvent as QuorumPolicy::ALWAYS_READY (S3).
  CopilotFakeQuorumEvent(int n_total)
    : QuorumEventWrapper(n_total, 0) {
    q().policy_.set(QuorumPolicy::ALWAYS_READY);
  }

  void FeedResponse() { vote_yes(); }
};

class CopilotCommo : public Communicator {
friend class CopilotProxy;
 public:
  static int fastQuorumSize(int total);
  static int quorumSize(int total);
  static int maxFailure(int total);

 public:
  CopilotCommo() = delete;
  CopilotCommo(rusty::Option<rusty::Arc<PollThread>> poll_thread_worker);

  shared_ptr<CopilotPrepareQuorumEvent>
  BroadcastPrepare(parid_t par_id,
                   uint8_t is_pilot,
                   slotid_t slot_id,
                   ballot_t ballot);
  
  shared_ptr<CopilotFastAcceptQuorumEvent>
  BroadcastFastAccept(parid_t par_id,
                      uint8_t is_pilot,
                      slotid_t slot_id,
                      ballot_t ballot,
                      uint64_t dep,
                      const janus::Command& cmd);

  shared_ptr<CopilotAcceptQuorumEvent>
  BroadcastAccept(parid_t par_id,
                  uint8_t is_pilot,
                  slotid_t slot_id,
                  ballot_t ballot,
                  uint64_t dep,
                  const janus::Command& cmd);
  
  shared_ptr<CopilotFakeQuorumEvent>
  BroadcastCommit(parid_t par_id,
                       uint8_t is_pilot,
                       slotid_t slot_id,
                       uint64_t dep,
                       const janus::Command& cmd);

};

}  // namespace janus