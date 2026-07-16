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

// @safe - pure threshold predicate. The atomic increment, reply storage, and
// rrr::IntEvent wakeup stay in RaftQuorum::on_reply(); this helper only names
// the scalar decision that a quorum has enough replies.
#if RUSTYCPP_RUST
pub fn raft_quorum_reached(received: i32, needed: i32) -> bool {
    received >= needed
}
#endif
/*RUSTYCPP:GEN-BEGIN id=raft_quorum_reached version=1 rust_sha256=4ff716e366ead197cfd9ce2e06b793adbdf3f2b584d698677a0c0fc5ecc895cd*/
inline bool raft_quorum_reached(int32_t received, int32_t needed);

inline bool raft_quorum_reached(int32_t received, int32_t needed) {
    return received >= needed;
}
/*RUSTYCPP:GEN-END id=raft_quorum_reached*/

// @safe - pure quorum arithmetic over copied counts. Server membership,
// ack/vote sets, and leader state stay in RaftServer; these helpers only
// centralize majority and count-threshold decisions.
#if RUSTYCPP_RUST
pub fn raft_quorum_majority_count(total: usize) -> usize {
    (total / 2) + 1
}

pub fn raft_quorum_count_reached(count: usize, quorum: usize) -> bool {
    count >= quorum
}

pub fn raft_quorum_count_below(count: usize, quorum: usize) -> bool {
    count < quorum
}
#endif
/*RUSTYCPP:GEN-BEGIN id=raft_quorum_count_helpers version=1 rust_sha256=105d43fa5ff3bdbbcdf702c82053586a047fd87033fe67606c1f2a1642b626ef*/
inline size_t raft_quorum_majority_count(size_t total);
inline bool raft_quorum_count_reached(size_t count, size_t quorum);
inline bool raft_quorum_count_below(size_t count, size_t quorum);

inline size_t raft_quorum_majority_count(size_t total) {
    return (total / 2) + 1;
}

inline bool raft_quorum_count_reached(size_t count, size_t quorum) {
    return count >= quorum;
}

inline bool raft_quorum_count_below(size_t count, size_t quorum) {
    return count < quorum;
}
/*RUSTYCPP:GEN-END id=raft_quorum_count_helpers*/

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
    if (raft_quorum_reached(n, n_needed_)) {
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
    ready_->wait(timeout_us);
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
  rusty::sync::atomic::Atomic<int> n_received_{0};
  mutable rusty::Mutex<std::vector<std::pair<siteid_t, Reply>>> replies_;
};

}  // namespace raft
}  // namespace janus
