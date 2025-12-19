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
      VoteYes();
    } else {
      VoteNo();
    }
  }


};

class MenciusSuggestQuorumEvent: public QuorumEvent {
 public:
  using QuorumEvent::QuorumEvent;
  void FeedResponse(bool y) {
    if (y) {
      VoteYes();
    } else {
      VoteNo();
    }
  }
};

class MenciusCommo : public Communicator {
 public:
  void *svr_workers_g{nullptr};
  
  MenciusCommo() = delete;
  MenciusCommo(rusty::Option<rusty::Arc<PollThread>> poll = rusty::None);

  shared_ptr<MenciusPrepareQuorumEvent>
  BroadcastPrepare(parid_t par_id,
                   slotid_t slot_id,
                   ballot_t ballot);
  void BroadcastPrepare(parid_t par_id,
                        slotid_t slot_id,
                        ballot_t ballot,
                        const function<void(rusty::Arc<Future>)> &callback);
  shared_ptr<MenciusSuggestQuorumEvent>
  BroadcastSuggest(parid_t par_id,
                  slotid_t slot_id,
                  ballot_t ballot,
                  shared_ptr<Marshallable> cmd);
  void BroadcastSuggest(parid_t par_id,
                       slotid_t slot_id,
                       ballot_t ballot,
                       shared_ptr<Marshallable> cmd,
                       const function<void(rusty::Arc<Future>)> &callback);
  void BroadcastDecide(const parid_t par_id,
                       const slotid_t slot_id,
                       const ballot_t ballot,
                       const shared_ptr<Marshallable> cmd);
};

} // namespace janus
