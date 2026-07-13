#pragma once

/**
 * @file quorum.hpp
 * @brief `RaftQuorum<Reply>` — N-replies-with-timeout aggregator used by
 *        phase-8.1+ raft outbound paths in place of the legacy
 *        `RaftVoteQuorumEvent` / `SendAppendEntriesResults` machinery.
 *
 * Threading model:
 *  - `on_reply` is called from sub-fibers that completed their per-peer
 *    `transport_->send_*` call. Within rrr, those sub-fibers all run on
 *    the same `PollThread` as the orchestrator, so concurrency is
 *    cooperative — but the `rusty::Mutex` + atomic counter keep the
 *    structure correct even if a caller spawns threads.
 *  - `wait_until_quorum` yields the calling fiber on an `rrr::IntEvent`
 *    until `n_needed_` replies have arrived or the timeout elapses.
 *  - `collect` drains the replies under the mutex.
 *
 * Rusty-safety:
 *  - All public methods are `@safe` modulo the `rrr::IntEvent` boundary,
 *    which is annotated `@unsafe` (rrr is a header-as-module library and
 *    its `Reactor::create_sp_event<…>` returns `std::shared_ptr<Ev>` —
 *    `rusty::Arc<T>` cannot be substituted because the reactor itself
 *    must hold a strong reference, see docs/dev/raft_quorum.md for the
 *    full rationale).
 */

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include <rusty/mutex.hpp>
#include <rusty/sync/atomic.hpp>

#include "rrr/rrr.hpp"

#include "../constants.h"  // siteid_t

namespace janus {
namespace raft {

template <typename Reply>
class RaftQuorum {
 public:
  // @safe - construct an empty quorum aggregator.
  // n_total: how many peer sub-fibers will eventually call on_reply().
  // n_needed: how many replies are required to declare quorum.
  RaftQuorum(int n_total, int n_needed)
      : n_total_(n_total),
        n_needed_(n_needed),
        // @unsafe { rrr::Reactor::create_sp_event returns std::shared_ptr;
        //           we keep that shape because the reactor owns the event
        //           via its all_events_ list. }
        ready_(::rrr::Reactor::create_sp_event<::rrr::IntEvent>(n_needed)),
        replies_(std::vector<std::pair<siteid_t, Reply>>{}) {}

  // Non-copyable, non-movable: holds an event registered with the reactor.
  RaftQuorum(const RaftQuorum&) = delete;
  RaftQuorum& operator=(const RaftQuorum&) = delete;
  RaftQuorum(RaftQuorum&&) = delete;
  RaftQuorum& operator=(RaftQuorum&&) = delete;

  // @safe - record one peer's reply, possibly waking the orchestrator.
  void on_reply(siteid_t from, Reply reply) {
    {
      auto guard = replies_.lock().unwrap();
      // @unsafe { std::vector::emplace_back is not borrow-checked }
      guard->emplace_back(from, std::move(reply));
    }
    int n = n_received_.fetch_add(
                1, ::rusty::sync::atomic::Ordering::AcqRel) +
            1;
    if (n >= n_needed_) {
      // @unsafe { rrr::IntEvent::set bumps value_ and triggers Event::test;
      //           multiple sets past the threshold are idempotent because
      //           is_ready() / status_ stays terminal once fired. }
      ready_->set(n);
    }
  }

  // @safe - block the calling fiber up to timeout_us; returns whether the
  // quorum threshold was reached.
  bool wait_until_quorum(uint64_t timeout_us) {
    // @unsafe { rrr::IntEvent::wait yields the fiber via the reactor;
    //           rrr-boundary call }
    ready_->wait_timeout(timeout_us);
    // @unsafe { rrr::IntEvent::is_ready compares value_ >= target_ }
    return ready_->is_ready();
  }

  // @safe - drain the accumulated (siteid, reply) pairs.
  std::vector<std::pair<siteid_t, Reply>> collect() {
    auto guard = replies_.lock().unwrap();
    std::vector<std::pair<siteid_t, Reply>> out;
    // @unsafe { std::vector::swap is not borrow-checked }
    out.swap(*guard);
    return out;
  }

  // @safe - non-blocking diagnostic count.
  int received() const {
    return n_received_.load(::rusty::sync::atomic::Ordering::Acquire);
  }

  // @safe
  int n_total()  const { return n_total_; }
  // @safe
  int n_needed() const { return n_needed_; }

 private:
  const int n_total_;
  const int n_needed_;
  // See class-level @unsafe note about std::shared_ptr.
  std::shared_ptr<::rrr::IntEvent> ready_;
  rusty::sync::atomic::AtomicI32 n_received_{0};
  mutable rusty::Mutex<std::vector<std::pair<siteid_t, Reply>>> replies_;
};

}  // namespace raft
}  // namespace janus
