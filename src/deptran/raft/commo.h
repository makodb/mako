#pragma once

// The reply holders the Rust transport fills and the shell reads through its
// kernels: a campaign's vote quorum event and an append's response. (The C++
// communicator, RaftCommo, that used to live here was deleted with the C++
// lane.)

#include "../__dep__.h"
#include "../constants.h"
#include "../communicator.h"
#include "../replication_quorum.h"
#include <atomic>
#include <mutex>
#include <rusty/slice.hpp>
#include <utility>

// @external: {
//   Log_info: [safe, (...) -> void],
//   Log_debug: [safe, (...) -> void],
//   Log_warn: [safe, (...) -> void],
//   Log_error: [safe, (...) -> void],
//   verify: [safe, (bool) -> void],
//   Reactor::create_sp_event: [safe, () -> rusty::Arc<IntEvent>],
//   Config::GetConfig: [safe, () -> Config*],
//   MarshallDeputy: [safe, (...) -> janus::Command],
//   Future::safe_release: [safe, (Future*) -> void],
//   vote_yes: [safe, () -> void],
//   vote_no: [safe, () -> void]
// }

namespace janus {

// Pure decisions over copied scalar values (the quorum event uses the last).
#if RUSTYCPP_RUST
pub const fn commo_append_entries_empty_from_cmd(has_cmd: bool) -> bool {
    !has_cmd
}

pub const fn commo_future_failed(error_code: i32) -> bool {
    error_code != 0
}

pub const fn commo_quorum_should_advance_term(candidate_term: i64,
                                               highest_term: i64) -> bool {
    candidate_term > highest_term
}
#endif
/*RUSTYCPP:GEN-BEGIN id=raft_commo.scalar_decisions version=1 rust_sha256=57830c51585f92868c121248232256bb04c6114353bdb3e90c0bab230ad1f876*/
constexpr bool commo_append_entries_empty_from_cmd(bool has_cmd);
constexpr bool commo_future_failed(int32_t error_code);
constexpr bool commo_quorum_should_advance_term(int64_t candidate_term, int64_t highest_term);
constexpr bool commo_append_entries_empty_from_cmd(bool has_cmd) {
    return !has_cmd;
}
constexpr bool commo_future_failed(int32_t error_code) {
    return rusty::detail::deref_if_pointer_like(error_code) != static_cast<int32_t>(0);
}
constexpr bool commo_quorum_should_advance_term(int64_t candidate_term, int64_t highest_term) {
    return rusty::detail::deref_if_pointer_like(candidate_term) > rusty::detail::deref_if_pointer_like(highest_term);
}
/*RUSTYCPP:GEN-END id=raft_commo.scalar_decisions*/

static_assert(commo_quorum_should_advance_term(-1, -2));
static_assert(!commo_quorum_should_advance_term(-2, -1));

// @unsafe - inherits from non-@interface base QuorumEvent
// [move, M5] One RequestVote reply as delivered: who sent it, its vote and
// its term, kept for the core's own count (RaftCore::election_settle).
struct VoteReplyRecord {
  siteid_t voter;
  bool granted;
  ballot_t term;
};

class RaftVoteQuorumEvent: public QuorumEventBase {
 public:
  // @safe - n_total is kept for the core, which counts the campaign itself.
  RaftVoteQuorumEvent(int n_total, int quorum)
      : QuorumEventBase(n_total, quorum), n_total_(n_total) {}

  // @safe - [move, M5] one reply from `voter`: recorded for the core, then
  // counted here as before (this event still ends the lane's wait).
  void FeedResponse(bool y, ballot_t term, siteid_t voter) {
    {
      auto guard = replies_.lock().unwrap();
      // @unsafe { std::vector::push_back is not borrow-checked }
      guard->push_back(VoteReplyRecord{voter, y, term});
    }
    FeedResponse(y, term);
  }

  // @safe
  uint64_t ReplyCount() {
    auto guard = replies_.lock().unwrap();
    return static_cast<uint64_t>(guard->size());
  }

  // @safe - reply `i`, or false past the end.
  bool ReplyAt(uint64_t i, siteid_t* voter, bool* granted, int64_t* term) {
    auto guard = replies_.lock().unwrap();
    if (i >= guard->size()) {
      return false;
    }
    const VoteReplyRecord& record = (*guard)[i];
    *voter = record.voter;
    *granted = record.granted;
    *term = record.term;
    return true;
  }

  // @safe
  int NTotal() const { return n_total_; }
  // @safe
  bool HasAcceptedValue() {
    return false;
  }

  // @safe
  void FeedResponse(bool y, ballot_t term) {
    // Every syntactically valid reply term dominates its vote bit. Negative
    // ballot_t values are sentinels/malformed wire values, not Raft terms.
    if (term >= 0 &&
        commo_quorum_should_advance_term(
            term, q().highest_term_.get())) {
      q().highest_term_.set(term);
    }
    if (y) {
      // @unsafe
      { vote_yes(); }  // 1 unsafe line: calls @unsafe parent method
    } else {
      vote_no();
    }
  }

  // @safe
  int64_t Term() {
    return q().highest_term_.get();
  }

 private:
  const int n_total_;
  mutable rusty::Mutex<std::vector<VoteReplyRecord>> replies_{
      std::vector<VoteReplyRecord>{}};
};

// Response data for async AppendEntries RPC
// Uses shared_ptr semantics to ensure memory validity when callback fires.
// `event` is a nullable Arc handle: the struct is default-constructed (event =
// None) then the event is assigned via create_sp_event before the RPC is sent.
struct AppendEntriesResponse {
  rusty::Option<rusty::Arc<IntEvent>> event{rusty::None};
  // The callback publishes all scalar response fields before setting this
  // flag. HeartbeatLoop can therefore retain and poll a response across
  // rounds without timing out (and permanently poisoning) its IntEvent.
  std::atomic_bool completed{false};
  uint64_t status = 0;
  uint64_t term = 0;
  uint64_t last_log_index = 0;
};


} // namespace janus
