#pragma once

#include "../__dep__.h"
#include "../constants.h"
#include "../communicator.h"
#include <chrono>
#include <ctime>

namespace janus {

class TxData;

class MenciusPrepareQuorumEvent: public QuorumEvent {
 public:
  using QuorumEvent::QuorumEvent;
//  ballot_t max_ballot_{0};
  bool HasSuggestedValue() {
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

class MenciusSuggestQuorumEvent: public QuorumEvent {
 public:
  using QuorumEvent::QuorumEvent;
  void FeedResponse(bool y) {
    if (y) {
      vote_yes();
    } else {
      vote_no();
    }
  }
};

class MenciusCommo : public Communicator {
 public:
  void *svr_workers_g{nullptr};
  
  MenciusCommo() = delete;
  MenciusCommo(rusty::Option<rusty::Arc<PollThread>> poll = rusty::None);

  // Workstream N Phase 4e-30: removed `BroadcastPrepare(parid, slot,
  // ballot)` declaration — only call site was the now-deleted
  // `CoordinatorMencius::Prepare()`; body was a `verify(0)` shell.
  // Workstream N Phase 4e-11: removed deprecated callback-style
  // `void BroadcastPrepare(parid_t, slotid_t, ballot_t, callback)`.
  // Body started with `verify(0); // deprecated function`, and the
  // only call site was a commented-out line in
  // `coordinator.cc:80`.
  // Workstream N L10f-prep6u: take janus::Command;
  // shared_ptr<Marshallable> callers auto-convert.
  shared_ptr<MenciusSuggestQuorumEvent>
  BroadcastSuggest(parid_t par_id,
                  slotid_t slot_id,
                  ballot_t ballot,
                  const janus::Command& cmd);
  // Workstream N Phase 4e-11: removed deprecated callback-style
  // `void BroadcastSuggest(parid_t, slotid_t, ballot_t, cmd, callback)`.
  // Same shape as above — body had `verify(0);` and no live callers.
  void BroadcastDecide(const parid_t par_id,
                       const slotid_t slot_id,
                       const ballot_t ballot,
                       const janus::Command& cmd);
};

} // namespace janus
