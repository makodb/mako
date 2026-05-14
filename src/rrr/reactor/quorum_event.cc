
// import std; replacement — see <std_compat.hpp> for rationale.
#include <stdint.h>


// @c-compat-added

#include <rusty/rusty.hpp>







#include "quorum_event.h"


#include "../rrr.hpp"

import std;

namespace janus {

using rrr::Fiber;
using rrr::Time;

QuorumEvent::QuorumEvent(int n_total, int quorum)
    : Event(), n_total_(n_total), quorum_(quorum) {
  finalize_event_ = std::make_shared<IntEvent>(n_total_);
  finalize_event_->__debug_creator = 1;
  begin_timestamp_ = Time::now(true);
}

void QuorumEvent::finalize(
    uint64_t timeout,
    rusty::Function<bool(rusty::Vec<std::pair<uint16_t, rrr::i64> > &)> finalize_func) {


  // rusty::Function is move-only, so capture the callback by move
  // into the background fiber's lambda.  The lambda must also be
  // `mutable` so the captured (non-const) Function can be invoked.
  Fiber::create_run([timeout, finalize_func = std::move(finalize_func), this]() mutable {
    bool ret = false;

    auto final_ev = finalize_event_;  // have to make a copy of finalized event (for reason, see comment A)
    rusty::Vec<std::pair<uint16_t, rrr::i64> > dangling_rpc;
    for (auto it : xids_)
      dangling_rpc.push(it);  // fetch out dangling rpc info before it's freed (see comment A)

    final_ev->wait(timeout);
    /* A: by the time this fires, the quorum event could have been freed. Thus,
     avoid accesing the quorum event object or its members after this line */

    // didn't receive all RPC replies
    if (final_ev->status_.get() == Event::TIMEOUT) {
      // Log_info("finalized timeout");
      ret = finalize_func(dangling_rpc);
    }
    (void)ret;
  }, __FILE__, __LINE__);
}

void QuorumEvent::add_xid(uint16_t site, rrr::i64 xid) {
  xids_[site] = xid;
}

void QuorumEvent::remove_xid(uint16_t site) {
  xids_.remove(site);
}

void QuorumEvent::vote_yes() {
  n_voted_yes_++;
  test();
  vec_timestamp_.push(Time::now(true) - begin_timestamp_);

  if (finalize_event_->status_.get() != Event::TIMEOUT)
    finalize_event_->set(n_voted_yes_ + n_voted_no_);
}

void QuorumEvent::vote_no() {
  n_voted_no_++;
  test();

  if (finalize_event_->status_.get() != Event::TIMEOUT)
    finalize_event_->set(n_voted_yes_ + n_voted_no_);
}

void QuorumEvent::log_event() {
  for (auto t : vec_timestamp_)
    std::cout << " " << t;
  std::cout << std::endl;
}

} // namespace janus
