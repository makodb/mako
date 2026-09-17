#pragma once

#include "../__dep__.h"
#include "../constants.h"
#include "../scheduler.h"
#include "../tpc_command.h"
#include "../view.h"
#include "commo.h"
#include <deque>
#include <exception>
#include <condition_variable>
#include <memory>
#include <rusty/move.hpp>   // rusty::clone in the generated FollowerProgress
#include <rusty/box.hpp>
#include <rusty/arc.hpp>
#include <rusty/condvar.hpp>
#include <rusty/num.hpp>
#include <rusty/array.hpp>   // rusty::len / rusty::is_empty in PeerTable
#include <rusty/ffi.hpp>   // rusty::ffi::c_void, the election loop opaque handle
// The rusty:: aliases for the rrr reactor types. server.cc has included this
// since the wake gate landed; the election timer block below is the first DSL
// in a HEADER to name one, so it must be visible here too. It MUST stay at
// global scope: included inside `namespace janus` it declares `janus::rusty`,
// which then shadows ::rusty for every lookup in the file. Its own ordering
// rule -- after the header that imports rrr.reactor -- is satisfied by
// commo.h above.
import rusty;   // rusty::Vec is a vec_port C++20 module, not a header
#include "rust_facade_types.h"
#include <rusty/option.hpp>
#include <rusty/slice.hpp>
#include <rusty/sync/atomic.hpp>
#include <rusty/thread.hpp>
#include <type_traits>
#include <utility>
#include "snapshot_manager.hpp"
#include <mutex>   // std::mutex mtx_, declared below

// @external: {
//   Log_info: [safe, (...) -> void],
//   Log_debug: [safe, (...) -> void],
//   Log_warn: [safe, (...) -> void],
//   Log_error: [safe, (...) -> void],
//   Log_fatal: [safe, (...) -> void],
//   verify: [safe, (bool) -> void],
//   Config::GetConfig: [safe, () -> Config*],
//   Reactor::create_sp_event: [safe, () -> rusty::Arc<IntEvent>],
//   Fiber::create_run: [safe, (...) -> void],
//   Fiber::sleep: [safe, (int) -> void],
//   RandomGenerator::rand_double: [safe, (double, double) -> double],
//   RandomGenerator::rand: [safe, (int, int) -> int],
//   Time::now: [safe, () -> uint64_t],
//   std::make_shared: [safe, (...) -> shared_ptr<T>],
//   dynamic_pointer_cast: [safe, (shared_ptr<T>) -> shared_ptr<U>],
//   strcmp: [safe, (const char*, const char*) -> int],
//   std::sort: [safe, (...) -> void],
//   std::max: [safe, (T, T) -> T],
//   std::min: [safe, (T, T) -> T],
//   std::stoull: [safe, (const string&) -> uint64_t],
//   std::stoll: [safe, (const string&) -> int64_t],
//   std::this_thread::sleep_for: [safe, (duration) -> void]
// }

namespace janus {
struct ReplicationWakeGate;

// PreparedStateMachineSnapshotInstall is an owned, abort-on-destruction
// transaction. Prepare callbacks must fully validate and durably stage an
// incoming state-machine image without changing the live state machine.
// Commit() may publish the staged image only after Raft has durably published
// the matching snapshot bytes.
// @unsafe - Abstract C++ ownership boundary for filesystem-backed state machines.
class PreparedStateMachineSnapshotInstall {
 public:
  virtual ~PreparedStateMachineSnapshotInstall() = default;

  // @unsafe - Atomically publishes the already-validated staged image.
  virtual bool Commit() = 0;
};

#define INVALID_SITEID  ((siteid_t)-1)

static_assert(std::is_same_v<int, int32_t>);

#if RUSTYCPP_RUST
// Submission admission result for the RaftWorker interface.  Memory-only Raft
// either rejects a command (not leader) or appends it; there is no durable
// append whose outcome could be unknown.
#[allow(non_camel_case_types)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Eq, PartialEq))]
#[repr(i32)]
pub enum RaftStartResult {
    REJECTED = 0,
    APPENDED = 1,
}

// A delayed vote quorum result is interpreted before its YES/NO/TIMEOUT
// payload. Higher-term evidence is globally authoritative; every ordinary
// outcome belongs only to the exact campaign that is still active.
#[allow(non_camel_case_types)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Eq, PartialEq))]
#[repr(i32)]
pub enum ElectionCompletionAction {
    IGNORE_STALE = 0,
    APPLY_CURRENT = 1,
    ADVANCE_HIGHER_TERM = 2,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=raft_server.commit_status version=1 rust_sha256=058ee4e0993ec838fa9c35337fd84eb768c98161794eae3932fb9cc2f07b98ba*/
enum class RaftStartResult : int32_t;
constexpr RaftStartResult RaftStartResult_REJECTED();
constexpr RaftStartResult RaftStartResult_APPENDED();
enum class ElectionCompletionAction : int32_t;
constexpr ElectionCompletionAction ElectionCompletionAction_IGNORE_STALE();
constexpr ElectionCompletionAction ElectionCompletionAction_APPLY_CURRENT();
constexpr ElectionCompletionAction ElectionCompletionAction_ADVANCE_HIGHER_TERM();

enum class RaftStartResult : int32_t {
    REJECTED = 0,
    APPENDED = 1
};
inline constexpr RaftStartResult RaftStartResult_REJECTED() { return RaftStartResult::REJECTED; }
inline constexpr RaftStartResult RaftStartResult_APPENDED() { return RaftStartResult::APPENDED; }

enum class ElectionCompletionAction : int32_t {
    IGNORE_STALE = 0,
    APPLY_CURRENT = 1,
    ADVANCE_HIGHER_TERM = 2
};
inline constexpr ElectionCompletionAction ElectionCompletionAction_IGNORE_STALE() { return ElectionCompletionAction::IGNORE_STALE; }
inline constexpr ElectionCompletionAction ElectionCompletionAction_APPLY_CURRENT() { return ElectionCompletionAction::APPLY_CURRENT; }
inline constexpr ElectionCompletionAction ElectionCompletionAction_ADVANCE_HIGHER_TERM() { return ElectionCompletionAction::ADVANCE_HIGHER_TERM; }
/*RUSTYCPP:GEN-END id=raft_server.commit_status*/

static_assert(std::is_same_v<std::underlying_type_t<RaftStartResult>, int>);
static_assert(std::is_trivially_copyable_v<RaftStartResult>);
static_assert(sizeof(RaftStartResult) == sizeof(int32_t));
static_assert(alignof(RaftStartResult) == alignof(int32_t));
static_assert(static_cast<int32_t>(RaftStartResult::REJECTED) == 0);
static_assert(static_cast<int32_t>(RaftStartResult::APPENDED) == 1);
static_assert(RaftStartResult{} == RaftStartResult::REJECTED);

static_assert(
    std::is_same_v<std::underlying_type_t<ElectionCompletionAction>, int>);
static_assert(std::is_trivially_copyable_v<ElectionCompletionAction>);
static_assert(sizeof(ElectionCompletionAction) == sizeof(int32_t));
static_assert(alignof(ElectionCompletionAction) == alignof(int32_t));
static_assert(static_cast<int32_t>(
                  ElectionCompletionAction::IGNORE_STALE) == 0);
static_assert(static_cast<int32_t>(
                  ElectionCompletionAction::APPLY_CURRENT) == 1);
static_assert(static_cast<int32_t>(
                  ElectionCompletionAction::ADVANCE_HIGHER_TERM) == 2);
static_assert(ElectionCompletionAction{} ==
              ElectionCompletionAction::IGNORE_STALE);

// Pure scalar Raft decisions. Stateful sequencing, locks, persistence,
// callbacks, logging, and pointer access remain at their existing C++ call
// sites. `const fn` makes the generated C++ constexpr/implicitly inline.
#if RUSTYCPP_RUST
// INVALID_SITEID is `((siteid_t)-1)` i.e. uint16_t(-1); server.cc already
// static_asserts that it equals numeric_limits<uint16_t>::max(). Owning it
// here lets the predicates below stop taking it as a parameter.
pub const RAFT_SERVER_INVALID_SITE_ID: u16 = 65535u16;

pub const fn raft_server_log_index_at_or_below(index: u64, boundary: u64) -> bool {
    index <= boundary
}

pub const fn raft_server_log_index_above(index: u64, boundary: u64) -> bool {
    index > boundary
}

pub const fn raft_server_site_is_preferred_leader(site_id: u16,
                                                   preferred_site_id: u16) -> bool {
    preferred_site_id != RAFT_SERVER_INVALID_SITE_ID && site_id == preferred_site_id
}

pub const fn raft_server_election_timeout_has_fired(is_leader: bool,
                                                     elapsed: u64,
                                                     timeout: u64) -> bool {
    !is_leader && elapsed > timeout
}

pub const fn raft_server_timer_campaign_is_current(is_leader: bool,
                                                    observed_generation: u64,
                                                    current_generation: u64,
                                                    elapsed: u64,
                                                    timeout: u64) -> bool {
    observed_generation == current_generation &&
        raft_server_election_timeout_has_fired(is_leader, elapsed, timeout)
}

pub const fn raft_server_campaign_can_start(is_leader: bool,
                                             election_in_progress: bool) -> bool {
    !is_leader && !election_in_progress
}

pub const fn raft_server_vote_term_is_stale(candidate_term: u64,
                                             current_term: u64) -> bool {
    candidate_term < current_term
}

pub const fn raft_server_vote_is_already_granted_to_other(candidate_term: u64,
                                                           current_term: u64,
                                                           voted_for: u16,
                                                           candidate_id: u16) -> bool {
    candidate_term == current_term &&
        voted_for != RAFT_SERVER_INVALID_SITE_ID &&
        voted_for != candidate_id
}

pub const fn raft_server_vote_is_idempotent(candidate_term: u64,
                                             current_term: u64,
                                             voted_for: u16,
                                             candidate_id: u16) -> bool {
    candidate_term == current_term && voted_for == candidate_id
}

pub const fn raft_server_candidate_log_is_at_least(candidate_term: i64,
                                                    current_term: i64,
                                                    candidate_index: u64,
                                                    current_index: u64) -> bool {
    candidate_term > current_term ||
        (candidate_term == current_term && candidate_index >= current_index)
}

pub const fn raft_server_election_last_log_uses_snapshot(last_log_index: u64,
                                                          snapshot_index: u64) -> bool {
    last_log_index == snapshot_index
}

pub const fn raft_server_install_snapshot_reply_is_available(follower_term: u64) -> bool {
    follower_term != 0
}

pub const fn raft_server_snapshot_is_stale(last_included_index: u64,
                                           local_progress_index: u64) -> bool {
    last_included_index <= local_progress_index
}

pub const fn raft_server_snapshot_boundary_matches(has_entry: bool,
                                                    local_term: u64,
                                                    snapshot_term: u64) -> bool {
    has_entry && local_term == snapshot_term
}

pub const fn raft_server_snapshot_term_is_valid(snapshot_term: u64,
                                                 leader_term: u64) -> bool {
    snapshot_term <= leader_term
}

pub const fn raft_server_snapshot_recovery_retains_suffix(
    has_suffix: bool,
    has_boundary: bool,
    boundary_matches: bool,
    live_snapshot_proves_suffix: bool) -> bool {
    has_suffix &&
        ((has_boundary && boundary_matches) ||
         (!has_boundary && live_snapshot_proves_suffix))
}

pub const fn raft_server_snapshot_recovery_has_unproven_gap(
    has_suffix: bool,
    has_boundary: bool,
    live_snapshot_proves_suffix: bool) -> bool {
    has_suffix && !has_boundary && !live_snapshot_proves_suffix
}

pub const fn raft_server_snapshot_term_uses_boundary(snapshot_index: u64,
                                                      existing_snapshot_index: u64) -> bool {
    snapshot_index == existing_snapshot_index
}

pub const fn raft_server_snapshot_marker_matches(payload_size: usize,
                                                   marker_size: usize,
                                                   payload_index: u64,
                                                   payload_term: u64,
                                                   expected_index: u64,
                                                   expected_term: u64) -> bool {
    payload_size == marker_size &&
        payload_index == expected_index &&
        payload_term == expected_term
}

pub const fn raft_server_election_result_is_current(election_in_progress: bool,
                                                     election_term: u64,
                                                     result_term: u64,
                                                     current_term: u64) -> bool {
    election_in_progress &&
        election_term == result_term &&
        result_term == current_term
}

pub const fn raft_server_election_completion_action(
    election_in_progress: bool,
    election_term: u64,
    campaign_term: u64,
    current_term: u64,
    observed_response_term: i64,
) -> i32 {
    if raft_server_signed_term_is_newer(observed_response_term, current_term) {
        ElectionCompletionAction::ADVANCE_HIGHER_TERM as i32
    } else if raft_server_election_result_is_current(
        election_in_progress, election_term, campaign_term, current_term,
    ) {
        ElectionCompletionAction::APPLY_CURRENT as i32
    } else {
        ElectionCompletionAction::IGNORE_STALE as i32
    }
}

pub const fn raft_server_apply_epoch_is_current(entry_epoch: u64,
                                                 current_epoch: u64) -> bool {
    entry_epoch == current_epoch
}

pub const fn raft_server_log_index_has_successor(index: u64) -> bool {
    index != u64::MAX
}

pub const fn raft_server_append_term_is_acceptable(leader_term: u64,
                                                    follower_term: u64) -> bool {
    leader_term >= follower_term
}

pub const fn raft_server_append_prefix_is_compacted_miss(previous_index: u64,
                                                          minimum_active_slot: u64,
                                                          snapshot_index: u64) -> bool {
    previous_index != 0 &&
        previous_index < minimum_active_slot &&
        previous_index != snapshot_index
}

pub const fn raft_server_append_previous_term_is_acceptable(previous_index: u64,
                                                             local_previous_term: u64,
                                                             leader_previous_term: u64) -> bool {
    previous_index == 0 || local_previous_term == leader_previous_term
}

pub const fn raft_server_append_is_acceptable(term_ok: bool,
                                               index_ok: bool,
                                               previous_term_ok: bool) -> bool {
    term_ok && index_ok && previous_term_ok
}

pub const fn raft_server_append_command_is_batch(command_kind: i32,
                                                  batch_kind: i32) -> bool {
    command_kind == batch_kind
}

pub const fn raft_server_append_entry_count_fits(previous_index: u64,
                                                  entry_count: u64) -> bool {
    entry_count <= u64::MAX - previous_index
}

pub const fn raft_server_append_batch_count_is_valid(previous_index: u64,
                                                      entry_count: u64) -> bool {
    entry_count != 0 &&
        raft_server_append_entry_count_fits(previous_index, entry_count)
}

pub const fn raft_server_append_entry_conflicts(local_entry_exists: bool,
                                                 local_term: u64,
                                                 incoming_term: u64) -> bool {
    !local_entry_exists || local_term != incoming_term
}

pub const fn raft_server_append_result_last_index(old_last_index: u64,
                                                   accepted_through: u64,
                                                   found_conflict: bool) -> u64 {
    if found_conflict || accepted_through > old_last_index {
        accepted_through
    } else {
        old_last_index
    }
}

pub const fn raft_server_append_sent_end(previous_index: u64,
                                         entry_count: u64) -> u64 {
    previous_index + entry_count
}

pub const fn raft_server_append_acknowledged_through(reported_index: u64,
                                                     sent_end_index: u64,
                                                     leader_last_index: u64) -> u64 {
    let reported_through_send = if reported_index < sent_end_index {
        reported_index
    } else {
        sent_end_index
    };
    if reported_through_send < leader_last_index {
        reported_through_send
    } else {
        leader_last_index
    }
}

pub const fn raft_server_commit_index_clamp(candidate_index: u64,
                                             last_log_index: u64) -> u64 {
    if candidate_index > last_log_index {
        last_log_index
    } else {
        candidate_index
    }
}

// The commit index a leader may advance to, given its followers' match
// indices already sorted ascending. Both heartbeat phases computed this
// inline and identically; this is the one spelling.
//
// `sorted_match_indices` excludes the leader, whose own match index is always
// the largest, so the majority position among all `nservers` replicas is at
// (nservers - 1) / 2 of the nservers - 1 follower values. A single-replica
// partition has no followers at all: the leader alone is the majority, so the
// candidate is everything it has appended. Indexing without that guard reads
// element [0] of an empty slice.
//
// The caller still applies the current-term rule, which needs a log lookup
// this function cannot do.
pub const fn raft_server_commit_index_candidate(selected_match: u64,
                                                 nservers: usize,
                                                 last_log_index: u64) -> u64 {
    let mut candidate = last_log_index;
    if nservers > 1 {
        candidate = selected_match;
    }
    if candidate > last_log_index {
        last_log_index
    } else {
        candidate
    }
}

pub const fn raft_server_read_index_round_can_advance(round: u64) -> bool {
    round != u64::MAX
}

pub const fn raft_server_read_index_reply_confirms_authority(
    response_available: bool,
    is_leader: bool,
    sent_term: u64,
    response_term: u64,
    current_term: u64,
    sent_round: u64,
    active_round: u64,
) -> bool {
    response_available &&
        is_leader &&
        sent_term == current_term &&
        response_term == sent_term &&
        sent_round == active_round
}

pub const fn raft_server_log_entry_is_current_term(entry_term: i64,
                                                    current_term: u64) -> bool {
    entry_term as u64 == current_term
}

pub const fn raft_server_snapshot_is_due(snapshot_index: u64,
                                          execute_index: u64,
                                          threshold: u64) -> bool {
    snapshot_index < execute_index &&
        (execute_index - snapshot_index) > threshold
}

pub const fn raft_server_compaction_safe_index(candidate_index: u64,
                                                commit_index: u64,
                                                snapshot_index: u64) -> u64 {
    let committed = if candidate_index < commit_index {
        candidate_index
    } else {
        commit_index
    };
    if committed < snapshot_index {
        committed
    } else {
        snapshot_index
    }
}

pub const fn raft_server_snapshot_progress_clamp(candidate_index: u64,
                                                  commit_index: u64,
                                                  upper_bound: u64) -> u64 {
    let committed_floor = if candidate_index < commit_index {
        commit_index
    } else {
        candidate_index
    };
    if committed_floor > upper_bound {
        upper_bound
    } else {
        committed_floor
    }
}

pub const fn raft_server_follower_next_index(last_log_index: u64) -> u64 {
    last_log_index.wrapping_add(1)
}

pub const fn raft_server_append_reject_can_fast_backoff(last_log_index: u64,
                                                         next_index: u64) -> bool {
    last_log_index > 0 &&
        raft_server_follower_next_index(last_log_index) < next_index
}

pub const fn raft_server_append_reject_has_term_conflict(last_log_index: u64,
                                                          next_index: u64) -> bool {
    last_log_index > 0 &&
        raft_server_follower_next_index(last_log_index) == next_index &&
        next_index > 1
}

pub const fn raft_server_append_reject_can_halve(next_index: u64) -> bool {
    next_index > 10
}

pub const fn raft_server_append_reject_can_decrement(next_index: u64) -> bool {
    next_index > 1
}

pub const fn raft_server_append_reject_halved(next_index: u64) -> u64 {
    next_index / 2
}

pub const fn raft_server_append_reject_decremented(next_index: u64) -> u64 {
    next_index.wrapping_sub(1)
}

pub const fn raft_server_append_reject_floor() -> u64 {
    1
}

pub const fn raft_server_start_was_rejected(result: RaftStartResult) -> bool {
    (result as i32) == (RaftStartResult::REJECTED as i32)
}

pub const fn raft_server_start_was_appended(result: RaftStartResult) -> bool {
    (result as i32) == (RaftStartResult::APPENDED as i32)
}


pub const fn raft_server_command_is_internal_noop(command_kind: i32,
                                                   noop_kind: i32) -> bool {
    command_kind == noop_kind
}

pub const fn raft_server_retention_window_normalize(window: u64) -> u64 {
    if window > 0 {
        window
    } else {
        1
    }
}

// TODO(stage2): allowed pending codegen check, NOT a decision. clippy wants
// `saturating_sub` here. The hand-written guard below is what production
// compiles today and the emitter is proven on that shape; `saturating_sub`
// may lower to a `rusty::` call a bare-rustc carrier cannot name. Verify the
// emitter output for it, apply the change on its own with the predicate
// reviewed, then remove this allow.
#[allow(clippy::implicit_saturating_sub)]
pub const fn raft_server_retention_cutoff(execute_index: u64,
                                           retention_window: u64) -> u64 {
    if execute_index > retention_window {
        execute_index - retention_window
    } else {
        0
    }
}

pub const fn raft_server_observed_higher_term(observed_term: u64,
                                               current_term: u64) -> bool {
    observed_term > current_term
}

pub const fn raft_server_signed_term_is_newer(observed_term: i64,
                                               current_term: u64) -> bool {
    observed_term >= 0 && observed_term as u64 > current_term
}

pub const fn raft_server_leader_hint_after_transition(is_leader: bool,
                                                       has_known_leader: bool,
                                                       self_id: u16,
                                                       known_leader_id: u16) -> u16 {
    if is_leader {
        self_id
    } else if has_known_leader {
        known_leader_id
    } else {
        RAFT_SERVER_INVALID_SITE_ID
    }
}

pub const fn raft_server_leader_rpc_sender_is_authoritative(
    leader_has_higher_term: bool,
    local_is_leader: bool,
    sender_is_self: bool,
    has_known_leader: bool,
    known_leader_matches_sender: bool,
) -> bool {
    (sender_is_self && local_is_leader && !leader_has_higher_term) ||
        (!sender_is_self &&
         (leader_has_higher_term ||
          (!local_is_leader &&
           (!has_known_leader || known_leader_matches_sender))))
}
#endif
/*RUSTYCPP:GEN-BEGIN id=raft_server.scalar_decisions version=1 rust_sha256=b0882f35b3a23aff7b3f7fff82ee98b0db0f4c7612c3bca4629dc519e46acee7*/
constexpr uint16_t RAFT_SERVER_INVALID_SITE_ID = static_cast<uint16_t>(65535);
constexpr bool raft_server_log_index_at_or_below(uint64_t index, uint64_t boundary);
constexpr bool raft_server_log_index_above(uint64_t index, uint64_t boundary);
constexpr bool raft_server_site_is_preferred_leader(uint16_t site_id, uint16_t preferred_site_id);
constexpr bool raft_server_election_timeout_has_fired(bool is_leader, uint64_t elapsed, uint64_t timeout);
constexpr bool raft_server_timer_campaign_is_current(bool is_leader, uint64_t observed_generation, uint64_t current_generation, uint64_t elapsed, uint64_t timeout);
constexpr bool raft_server_campaign_can_start(bool is_leader, bool election_in_progress);
constexpr bool raft_server_vote_term_is_stale(uint64_t candidate_term, uint64_t current_term);
constexpr bool raft_server_vote_is_already_granted_to_other(uint64_t candidate_term, uint64_t current_term, uint16_t voted_for, uint16_t candidate_id);
constexpr bool raft_server_vote_is_idempotent(uint64_t candidate_term, uint64_t current_term, uint16_t voted_for, uint16_t candidate_id);
constexpr bool raft_server_candidate_log_is_at_least(int64_t candidate_term, int64_t current_term, uint64_t candidate_index, uint64_t current_index);
constexpr bool raft_server_election_last_log_uses_snapshot(uint64_t last_log_index, uint64_t snapshot_index);
constexpr bool raft_server_install_snapshot_reply_is_available(uint64_t follower_term);
constexpr bool raft_server_snapshot_is_stale(uint64_t last_included_index, uint64_t local_progress_index);
constexpr bool raft_server_snapshot_boundary_matches(bool has_entry, uint64_t local_term, uint64_t snapshot_term);
constexpr bool raft_server_snapshot_term_is_valid(uint64_t snapshot_term, uint64_t leader_term);
constexpr bool raft_server_snapshot_recovery_retains_suffix(bool has_suffix, bool has_boundary, bool boundary_matches, bool live_snapshot_proves_suffix);
constexpr bool raft_server_snapshot_recovery_has_unproven_gap(bool has_suffix, bool has_boundary, bool live_snapshot_proves_suffix);
constexpr bool raft_server_snapshot_term_uses_boundary(uint64_t snapshot_index, uint64_t existing_snapshot_index);
constexpr bool raft_server_snapshot_marker_matches(size_t payload_size, size_t marker_size, uint64_t payload_index, uint64_t payload_term, uint64_t expected_index, uint64_t expected_term);
constexpr bool raft_server_election_result_is_current(bool election_in_progress, uint64_t election_term, uint64_t result_term, uint64_t current_term);
constexpr int32_t raft_server_election_completion_action(bool election_in_progress, uint64_t election_term, uint64_t campaign_term, uint64_t current_term, int64_t observed_response_term);
constexpr bool raft_server_apply_epoch_is_current(uint64_t entry_epoch, uint64_t current_epoch);
constexpr bool raft_server_log_index_has_successor(uint64_t index);
constexpr bool raft_server_append_term_is_acceptable(uint64_t leader_term, uint64_t follower_term);
constexpr bool raft_server_append_prefix_is_compacted_miss(uint64_t previous_index, uint64_t minimum_active_slot, uint64_t snapshot_index);
constexpr bool raft_server_append_previous_term_is_acceptable(uint64_t previous_index, uint64_t local_previous_term, uint64_t leader_previous_term);
constexpr bool raft_server_append_is_acceptable(bool term_ok, bool index_ok, bool previous_term_ok);
constexpr bool raft_server_append_command_is_batch(int32_t command_kind, int32_t batch_kind);
constexpr bool raft_server_append_entry_count_fits(uint64_t previous_index, uint64_t entry_count);
constexpr bool raft_server_append_batch_count_is_valid(uint64_t previous_index, uint64_t entry_count);
constexpr bool raft_server_append_entry_conflicts(bool local_entry_exists, uint64_t local_term, uint64_t incoming_term);
constexpr uint64_t raft_server_append_result_last_index(uint64_t old_last_index, uint64_t accepted_through, bool found_conflict);
constexpr uint64_t raft_server_append_sent_end(uint64_t previous_index, uint64_t entry_count);
constexpr uint64_t raft_server_append_acknowledged_through(uint64_t reported_index, uint64_t sent_end_index, uint64_t leader_last_index);
constexpr uint64_t raft_server_commit_index_clamp(uint64_t candidate_index, uint64_t last_log_index);
constexpr uint64_t raft_server_commit_index_candidate(uint64_t selected_match, size_t nservers, uint64_t last_log_index);
constexpr bool raft_server_read_index_round_can_advance(uint64_t round);
constexpr bool raft_server_read_index_reply_confirms_authority(bool response_available, bool is_leader, uint64_t sent_term, uint64_t response_term, uint64_t current_term, uint64_t sent_round, uint64_t active_round);
constexpr bool raft_server_log_entry_is_current_term(int64_t entry_term, uint64_t current_term);
constexpr bool raft_server_snapshot_is_due(uint64_t snapshot_index, uint64_t execute_index, uint64_t threshold);
constexpr uint64_t raft_server_compaction_safe_index(uint64_t candidate_index, uint64_t commit_index, uint64_t snapshot_index);
constexpr uint64_t raft_server_snapshot_progress_clamp(uint64_t candidate_index, uint64_t commit_index, uint64_t upper_bound);
constexpr uint64_t raft_server_follower_next_index(uint64_t last_log_index);
constexpr bool raft_server_append_reject_can_fast_backoff(uint64_t last_log_index, uint64_t next_index);
constexpr bool raft_server_append_reject_has_term_conflict(uint64_t last_log_index, uint64_t next_index);
constexpr bool raft_server_append_reject_can_halve(uint64_t next_index);
constexpr bool raft_server_append_reject_can_decrement(uint64_t next_index);
constexpr uint64_t raft_server_append_reject_halved(uint64_t next_index);
constexpr uint64_t raft_server_append_reject_decremented(uint64_t next_index);
constexpr uint64_t raft_server_append_reject_floor();
constexpr bool raft_server_command_is_internal_noop(int32_t command_kind, int32_t noop_kind);
constexpr uint64_t raft_server_retention_window_normalize(uint64_t window);
constexpr uint64_t raft_server_retention_cutoff(uint64_t execute_index, uint64_t retention_window);
constexpr bool raft_server_observed_higher_term(uint64_t observed_term, uint64_t current_term);
constexpr bool raft_server_signed_term_is_newer(int64_t observed_term, uint64_t current_term);
constexpr uint16_t raft_server_leader_hint_after_transition(bool is_leader, bool has_known_leader, uint16_t self_id, uint16_t known_leader_id);
constexpr bool raft_server_leader_rpc_sender_is_authoritative(bool leader_has_higher_term, bool local_is_leader, bool sender_is_self, bool has_known_leader, bool known_leader_matches_sender);
constexpr bool raft_server_log_index_at_or_below(uint64_t index, uint64_t boundary) {
    return rusty::detail::deref_if_pointer_like(index) <= rusty::detail::deref_if_pointer_like(boundary);
}
constexpr bool raft_server_log_index_above(uint64_t index, uint64_t boundary) {
    return rusty::detail::deref_if_pointer_like(index) > rusty::detail::deref_if_pointer_like(boundary);
}
constexpr bool raft_server_site_is_preferred_leader(uint16_t site_id, uint16_t preferred_site_id) {
    return (rusty::detail::deref_if_pointer_like(preferred_site_id) != rusty::detail::deref_if_pointer_like(RAFT_SERVER_INVALID_SITE_ID)) && (rusty::detail::deref_if_pointer_like(site_id) == rusty::detail::deref_if_pointer_like(preferred_site_id));
}
constexpr bool raft_server_election_timeout_has_fired(bool is_leader, uint64_t elapsed, uint64_t timeout) {
    return !is_leader && (rusty::detail::deref_if_pointer_like(elapsed) > rusty::detail::deref_if_pointer_like(timeout));
}
constexpr bool raft_server_timer_campaign_is_current(bool is_leader, uint64_t observed_generation, uint64_t current_generation, uint64_t elapsed, uint64_t timeout) {
    return (rusty::detail::deref_if_pointer_like(observed_generation) == rusty::detail::deref_if_pointer_like(current_generation)) && raft_server_election_timeout_has_fired(std::move(is_leader), std::move(elapsed), std::move(timeout));
}
constexpr bool raft_server_campaign_can_start(bool is_leader, bool election_in_progress) {
    return !is_leader && !election_in_progress;
}
constexpr bool raft_server_vote_term_is_stale(uint64_t candidate_term, uint64_t current_term) {
    return rusty::detail::deref_if_pointer_like(candidate_term) < rusty::detail::deref_if_pointer_like(current_term);
}
constexpr bool raft_server_vote_is_already_granted_to_other(uint64_t candidate_term, uint64_t current_term, uint16_t voted_for, uint16_t candidate_id) {
    return ((rusty::detail::deref_if_pointer_like(candidate_term) == rusty::detail::deref_if_pointer_like(current_term)) && (rusty::detail::deref_if_pointer_like(voted_for) != rusty::detail::deref_if_pointer_like(RAFT_SERVER_INVALID_SITE_ID))) && (rusty::detail::deref_if_pointer_like(voted_for) != rusty::detail::deref_if_pointer_like(candidate_id));
}
constexpr bool raft_server_vote_is_idempotent(uint64_t candidate_term, uint64_t current_term, uint16_t voted_for, uint16_t candidate_id) {
    return (rusty::detail::deref_if_pointer_like(candidate_term) == rusty::detail::deref_if_pointer_like(current_term)) && (rusty::detail::deref_if_pointer_like(voted_for) == rusty::detail::deref_if_pointer_like(candidate_id));
}
constexpr bool raft_server_candidate_log_is_at_least(int64_t candidate_term, int64_t current_term, uint64_t candidate_index, uint64_t current_index) {
    return (rusty::detail::deref_if_pointer_like(candidate_term) > rusty::detail::deref_if_pointer_like(current_term)) || (((rusty::detail::deref_if_pointer_like(candidate_term) == rusty::detail::deref_if_pointer_like(current_term)) && (rusty::detail::deref_if_pointer_like(candidate_index) >= rusty::detail::deref_if_pointer_like(current_index))));
}
constexpr bool raft_server_election_last_log_uses_snapshot(uint64_t last_log_index, uint64_t snapshot_index) {
    return rusty::detail::deref_if_pointer_like(last_log_index) == rusty::detail::deref_if_pointer_like(snapshot_index);
}
constexpr bool raft_server_install_snapshot_reply_is_available(uint64_t follower_term) {
    return rusty::detail::deref_if_pointer_like(follower_term) != static_cast<uint64_t>(0);
}
constexpr bool raft_server_snapshot_is_stale(uint64_t last_included_index, uint64_t local_progress_index) {
    return rusty::detail::deref_if_pointer_like(last_included_index) <= rusty::detail::deref_if_pointer_like(local_progress_index);
}
constexpr bool raft_server_snapshot_boundary_matches(bool has_entry, uint64_t local_term, uint64_t snapshot_term) {
    return rusty::detail::deref_if_pointer_like(has_entry) && (rusty::detail::deref_if_pointer_like(local_term) == rusty::detail::deref_if_pointer_like(snapshot_term));
}
constexpr bool raft_server_snapshot_term_is_valid(uint64_t snapshot_term, uint64_t leader_term) {
    return rusty::detail::deref_if_pointer_like(snapshot_term) <= rusty::detail::deref_if_pointer_like(leader_term);
}
constexpr bool raft_server_snapshot_recovery_retains_suffix(bool has_suffix, bool has_boundary, bool boundary_matches, bool live_snapshot_proves_suffix) {
    return rusty::detail::deref_if_pointer_like(has_suffix) && ((((rusty::detail::deref_if_pointer_like(has_boundary) && rusty::detail::deref_if_pointer_like(boundary_matches))) || ((!has_boundary && rusty::detail::deref_if_pointer_like(live_snapshot_proves_suffix)))));
}
constexpr bool raft_server_snapshot_recovery_has_unproven_gap(bool has_suffix, bool has_boundary, bool live_snapshot_proves_suffix) {
    return (rusty::detail::deref_if_pointer_like(has_suffix) && !has_boundary) && !live_snapshot_proves_suffix;
}
constexpr bool raft_server_snapshot_term_uses_boundary(uint64_t snapshot_index, uint64_t existing_snapshot_index) {
    return rusty::detail::deref_if_pointer_like(snapshot_index) == rusty::detail::deref_if_pointer_like(existing_snapshot_index);
}
constexpr bool raft_server_snapshot_marker_matches(size_t payload_size, size_t marker_size, uint64_t payload_index, uint64_t payload_term, uint64_t expected_index, uint64_t expected_term) {
    return ((rusty::detail::deref_if_pointer_like(payload_size) == rusty::detail::deref_if_pointer_like(marker_size)) && (rusty::detail::deref_if_pointer_like(payload_index) == rusty::detail::deref_if_pointer_like(expected_index))) && (rusty::detail::deref_if_pointer_like(payload_term) == rusty::detail::deref_if_pointer_like(expected_term));
}
constexpr bool raft_server_election_result_is_current(bool election_in_progress, uint64_t election_term, uint64_t result_term, uint64_t current_term) {
    return (rusty::detail::deref_if_pointer_like(election_in_progress) && (rusty::detail::deref_if_pointer_like(election_term) == rusty::detail::deref_if_pointer_like(result_term))) && (rusty::detail::deref_if_pointer_like(result_term) == rusty::detail::deref_if_pointer_like(current_term));
}
constexpr int32_t raft_server_election_completion_action(bool election_in_progress, uint64_t election_term, uint64_t campaign_term, uint64_t current_term, int64_t observed_response_term) {
    if (raft_server_signed_term_is_newer(std::move(observed_response_term), std::move(current_term))) {
        return static_cast<int32_t>(ElectionCompletionAction_ADVANCE_HIGHER_TERM());
    } else if (raft_server_election_result_is_current(std::move(election_in_progress), std::move(election_term), std::move(campaign_term), std::move(current_term))) {
        return static_cast<int32_t>(ElectionCompletionAction_APPLY_CURRENT());
    } else {
        return static_cast<int32_t>(ElectionCompletionAction_IGNORE_STALE());
    }
}
constexpr bool raft_server_apply_epoch_is_current(uint64_t entry_epoch, uint64_t current_epoch) {
    return rusty::detail::deref_if_pointer_like(entry_epoch) == rusty::detail::deref_if_pointer_like(current_epoch);
}
constexpr bool raft_server_log_index_has_successor(uint64_t index) {
    return rusty::detail::deref_if_pointer_like(index) != rusty::detail::deref_if_pointer_like(std::numeric_limits<uint64_t>::max());
}
constexpr bool raft_server_append_term_is_acceptable(uint64_t leader_term, uint64_t follower_term) {
    return rusty::detail::deref_if_pointer_like(leader_term) >= rusty::detail::deref_if_pointer_like(follower_term);
}
constexpr bool raft_server_append_prefix_is_compacted_miss(uint64_t previous_index, uint64_t minimum_active_slot, uint64_t snapshot_index) {
    return ((rusty::detail::deref_if_pointer_like(previous_index) != static_cast<uint64_t>(0)) && (rusty::detail::deref_if_pointer_like(previous_index) < rusty::detail::deref_if_pointer_like(minimum_active_slot))) && (rusty::detail::deref_if_pointer_like(previous_index) != rusty::detail::deref_if_pointer_like(snapshot_index));
}
constexpr bool raft_server_append_previous_term_is_acceptable(uint64_t previous_index, uint64_t local_previous_term, uint64_t leader_previous_term) {
    return (rusty::detail::deref_if_pointer_like(previous_index) == static_cast<uint64_t>(0)) || (rusty::detail::deref_if_pointer_like(local_previous_term) == rusty::detail::deref_if_pointer_like(leader_previous_term));
}
constexpr bool raft_server_append_is_acceptable(bool term_ok, bool index_ok, bool previous_term_ok) {
    return (rusty::detail::deref_if_pointer_like(term_ok) && rusty::detail::deref_if_pointer_like(index_ok)) && rusty::detail::deref_if_pointer_like(previous_term_ok);
}
constexpr bool raft_server_append_command_is_batch(int32_t command_kind, int32_t batch_kind) {
    return rusty::detail::deref_if_pointer_like(command_kind) == rusty::detail::deref_if_pointer_like(batch_kind);
}
constexpr bool raft_server_append_entry_count_fits(uint64_t previous_index, uint64_t entry_count) {
    return rusty::detail::deref_if_pointer_like(entry_count) <= (rusty::detail::deref_if_pointer_like(std::numeric_limits<uint64_t>::max()) - rusty::detail::deref_if_pointer_like(previous_index));
}
constexpr bool raft_server_append_batch_count_is_valid(uint64_t previous_index, uint64_t entry_count) {
    return (rusty::detail::deref_if_pointer_like(entry_count) != static_cast<uint64_t>(0)) && raft_server_append_entry_count_fits(std::move(previous_index), std::move(entry_count));
}
constexpr bool raft_server_append_entry_conflicts(bool local_entry_exists, uint64_t local_term, uint64_t incoming_term) {
    return !local_entry_exists || (rusty::detail::deref_if_pointer_like(local_term) != rusty::detail::deref_if_pointer_like(incoming_term));
}
constexpr uint64_t raft_server_append_result_last_index(uint64_t old_last_index, uint64_t accepted_through, bool found_conflict) {
    if (rusty::detail::deref_if_pointer_like(found_conflict) || (rusty::detail::deref_if_pointer_like(accepted_through) > rusty::detail::deref_if_pointer_like(old_last_index))) {
        return std::move(accepted_through);
    } else {
        return std::move(old_last_index);
    }
}
constexpr uint64_t raft_server_append_sent_end(uint64_t previous_index, uint64_t entry_count) {
    return rusty::detail::deref_if_pointer_like(previous_index) + rusty::detail::deref_if_pointer_like(entry_count);
}
constexpr uint64_t raft_server_append_acknowledged_through(uint64_t reported_index, uint64_t sent_end_index, uint64_t leader_last_index) {
    auto reported_through_send = (rusty::detail::deref_if_pointer_like(reported_index) < rusty::detail::deref_if_pointer_like(sent_end_index) ? reported_index : sent_end_index);
    if (rusty::detail::deref_if_pointer_like(reported_through_send) < rusty::detail::deref_if_pointer_like(leader_last_index)) {
        return std::move(reported_through_send);
    } else {
        return std::move(leader_last_index);
    }
}
constexpr uint64_t raft_server_commit_index_clamp(uint64_t candidate_index, uint64_t last_log_index) {
    if (rusty::detail::deref_if_pointer_like(candidate_index) > rusty::detail::deref_if_pointer_like(last_log_index)) {
        return std::move(last_log_index);
    } else {
        return std::move(candidate_index);
    }
}
constexpr uint64_t raft_server_commit_index_candidate(uint64_t selected_match, size_t nservers, uint64_t last_log_index) {
    auto candidate = std::move(last_log_index);
    if (rusty::detail::deref_if_pointer_like(nservers) > 1) {
        candidate = std::move(selected_match);
    }
    if (rusty::detail::deref_if_pointer_like(candidate) > rusty::detail::deref_if_pointer_like(last_log_index)) {
        return std::move(last_log_index);
    } else {
        return std::move(candidate);
    }
}
constexpr bool raft_server_read_index_round_can_advance(uint64_t round) {
    return rusty::detail::deref_if_pointer_like(round) != rusty::detail::deref_if_pointer_like(std::numeric_limits<uint64_t>::max());
}
constexpr bool raft_server_read_index_reply_confirms_authority(bool response_available, bool is_leader, uint64_t sent_term, uint64_t response_term, uint64_t current_term, uint64_t sent_round, uint64_t active_round) {
    return (((rusty::detail::deref_if_pointer_like(response_available) && rusty::detail::deref_if_pointer_like(is_leader)) && (rusty::detail::deref_if_pointer_like(sent_term) == rusty::detail::deref_if_pointer_like(current_term))) && (rusty::detail::deref_if_pointer_like(response_term) == rusty::detail::deref_if_pointer_like(sent_term))) && (rusty::detail::deref_if_pointer_like(sent_round) == rusty::detail::deref_if_pointer_like(active_round));
}
constexpr bool raft_server_log_entry_is_current_term(int64_t entry_term, uint64_t current_term) {
    return (static_cast<uint64_t>(entry_term)) == rusty::detail::deref_if_pointer_like(current_term);
}
constexpr bool raft_server_snapshot_is_due(uint64_t snapshot_index, uint64_t execute_index, uint64_t threshold) {
    return (rusty::detail::deref_if_pointer_like(snapshot_index) < rusty::detail::deref_if_pointer_like(execute_index)) && (((rusty::detail::deref_if_pointer_like(execute_index) - rusty::detail::deref_if_pointer_like(snapshot_index))) > rusty::detail::deref_if_pointer_like(threshold));
}
constexpr uint64_t raft_server_compaction_safe_index(uint64_t candidate_index, uint64_t commit_index, uint64_t snapshot_index) {
    auto committed = (rusty::detail::deref_if_pointer_like(candidate_index) < rusty::detail::deref_if_pointer_like(commit_index) ? candidate_index : commit_index);
    if (rusty::detail::deref_if_pointer_like(committed) < rusty::detail::deref_if_pointer_like(snapshot_index)) {
        return std::move(committed);
    } else {
        return std::move(snapshot_index);
    }
}
constexpr uint64_t raft_server_snapshot_progress_clamp(uint64_t candidate_index, uint64_t commit_index, uint64_t upper_bound) {
    auto committed_floor = (rusty::detail::deref_if_pointer_like(candidate_index) < rusty::detail::deref_if_pointer_like(commit_index) ? commit_index : candidate_index);
    if (rusty::detail::deref_if_pointer_like(committed_floor) > rusty::detail::deref_if_pointer_like(upper_bound)) {
        return std::move(upper_bound);
    } else {
        return std::move(committed_floor);
    }
}
constexpr uint64_t raft_server_follower_next_index(uint64_t last_log_index) {
    return rusty::wrapping_add(last_log_index, static_cast<std::remove_cvref_t<decltype(last_log_index)>>(1));
}
constexpr bool raft_server_append_reject_can_fast_backoff(uint64_t last_log_index, uint64_t next_index) {
    return (rusty::detail::deref_if_pointer_like(last_log_index) > 0) && (raft_server_follower_next_index(std::move(last_log_index)) < rusty::detail::deref_if_pointer_like(next_index));
}
constexpr bool raft_server_append_reject_has_term_conflict(uint64_t last_log_index, uint64_t next_index) {
    return ((rusty::detail::deref_if_pointer_like(last_log_index) > 0) && (raft_server_follower_next_index(std::move(last_log_index)) == rusty::detail::deref_if_pointer_like(next_index))) && (rusty::detail::deref_if_pointer_like(next_index) > 1);
}
constexpr bool raft_server_append_reject_can_halve(uint64_t next_index) {
    return rusty::detail::deref_if_pointer_like(next_index) > 10;
}
constexpr bool raft_server_append_reject_can_decrement(uint64_t next_index) {
    return rusty::detail::deref_if_pointer_like(next_index) > 1;
}
constexpr uint64_t raft_server_append_reject_halved(uint64_t next_index) {
    return rusty::detail::deref_if_pointer_like(next_index) / static_cast<uint64_t>(2);
}
constexpr uint64_t raft_server_append_reject_decremented(uint64_t next_index) {
    return rusty::wrapping_sub(next_index, static_cast<std::remove_cvref_t<decltype(next_index)>>(1));
}
constexpr uint64_t raft_server_append_reject_floor() {
    return static_cast<uint64_t>(1);
}
constexpr bool raft_server_start_was_rejected(RaftStartResult result) {
    return ((static_cast<int32_t>(result))) == ((static_cast<int32_t>(RaftStartResult_REJECTED())));
}
constexpr bool raft_server_start_was_appended(RaftStartResult result) {
    return ((static_cast<int32_t>(result))) == ((static_cast<int32_t>(RaftStartResult_APPENDED())));
}
constexpr bool raft_server_command_is_internal_noop(int32_t command_kind, int32_t noop_kind) {
    return rusty::detail::deref_if_pointer_like(command_kind) == rusty::detail::deref_if_pointer_like(noop_kind);
}
constexpr uint64_t raft_server_retention_window_normalize(uint64_t window) {
    if (rusty::detail::deref_if_pointer_like(window) > 0) {
        return std::move(window);
    } else {
        return static_cast<uint64_t>(1);
    }
}
constexpr uint64_t raft_server_retention_cutoff(uint64_t execute_index, uint64_t retention_window) {
    if (rusty::detail::deref_if_pointer_like(execute_index) > rusty::detail::deref_if_pointer_like(retention_window)) {
        return rusty::detail::deref_if_pointer_like(execute_index) - rusty::detail::deref_if_pointer_like(retention_window);
    } else {
        return static_cast<uint64_t>(0);
    }
}
constexpr bool raft_server_observed_higher_term(uint64_t observed_term, uint64_t current_term) {
    return rusty::detail::deref_if_pointer_like(observed_term) > rusty::detail::deref_if_pointer_like(current_term);
}
constexpr bool raft_server_signed_term_is_newer(int64_t observed_term, uint64_t current_term) {
    return (rusty::detail::deref_if_pointer_like(observed_term) >= 0) && ((static_cast<uint64_t>(observed_term)) > rusty::detail::deref_if_pointer_like(current_term));
}
constexpr uint16_t raft_server_leader_hint_after_transition(bool is_leader, bool has_known_leader, uint16_t self_id, uint16_t known_leader_id) {
    if (is_leader) {
        return std::move(self_id);
    } else if (has_known_leader) {
        return std::move(known_leader_id);
    } else {
        return RAFT_SERVER_INVALID_SITE_ID;
    }
}
constexpr bool raft_server_leader_rpc_sender_is_authoritative(bool leader_has_higher_term, bool local_is_leader, bool sender_is_self, bool has_known_leader, bool known_leader_matches_sender) {
    return (((rusty::detail::deref_if_pointer_like(sender_is_self) && rusty::detail::deref_if_pointer_like(local_is_leader)) && !leader_has_higher_term)) || ((!sender_is_self && ((rusty::detail::deref_if_pointer_like(leader_has_higher_term) || ((!local_is_leader && ((!has_known_leader || rusty::detail::deref_if_pointer_like(known_leader_matches_sender)))))))));
}
/*RUSTYCPP:GEN-END id=raft_server.scalar_decisions*/

// The sentinel is now owned by the DSL block as
// RAFT_SERVER_INVALID_SITE_ID; pin that it still equals the C++ macro.
static_assert(RAFT_SERVER_INVALID_SITE_ID ==
              static_cast<uint16_t>(INVALID_SITEID));
static_assert(raft_server_site_is_preferred_leader(7, 7));
// A site that is itself the sentinel is not the preferred leader, because
// the sentinel means "no preferred leader configured".
static_assert(!raft_server_site_is_preferred_leader(
    static_cast<uint16_t>(INVALID_SITEID),
    static_cast<uint16_t>(INVALID_SITEID)));
static_assert(raft_server_vote_is_already_granted_to_other(4, 4, 1, 2));
static_assert(!raft_server_vote_is_already_granted_to_other(
    4, 4, static_cast<uint16_t>(INVALID_SITEID), 2));
static_assert(raft_server_vote_is_idempotent(4, 4, 2, 2));
static_assert(raft_server_candidate_log_is_at_least(3, 2, 1, 9));
static_assert(raft_server_candidate_log_is_at_least(3, 3, 9, 9));
static_assert(!raft_server_candidate_log_is_at_least(3, 3, 8, 9));
static_assert(raft_server_election_last_log_uses_snapshot(0, 0));
static_assert(raft_server_election_last_log_uses_snapshot(460, 460));
static_assert(!raft_server_election_last_log_uses_snapshot(461, 460));
static_assert(raft_server_timer_campaign_is_current(
    false, 9, 9, 501, 500));
static_assert(!raft_server_timer_campaign_is_current(
    false, 8, 9, 501, 500));
static_assert(!raft_server_timer_campaign_is_current(
    false, 9, 9, 500, 500));
static_assert(!raft_server_timer_campaign_is_current(
    true, 9, 9, 501, 500));
static_assert(raft_server_campaign_can_start(false, false));
static_assert(!raft_server_campaign_can_start(true, false));
static_assert(!raft_server_campaign_can_start(false, true));
static_assert(!raft_server_install_snapshot_reply_is_available(0));
static_assert(raft_server_install_snapshot_reply_is_available(1));
static_assert(raft_server_install_snapshot_reply_is_available(UINT64_MAX));
static_assert(raft_server_snapshot_is_stale(9, 9));
static_assert(raft_server_snapshot_is_stale(8, 9));
static_assert(!raft_server_snapshot_is_stale(10, 9));
static_assert(raft_server_snapshot_boundary_matches(true, 7, 7));
static_assert(!raft_server_snapshot_boundary_matches(false, 7, 7));
static_assert(!raft_server_snapshot_boundary_matches(true, 6, 7));
static_assert(raft_server_snapshot_term_is_valid(7, 7));
static_assert(raft_server_snapshot_term_is_valid(6, 7));
static_assert(!raft_server_snapshot_term_is_valid(8, 7));
static_assert(raft_server_snapshot_recovery_retains_suffix(
    true, true, true, false));
static_assert(!raft_server_snapshot_recovery_retains_suffix(
    true, true, false, true));
static_assert(raft_server_snapshot_recovery_retains_suffix(
    true, false, false, true));
static_assert(!raft_server_snapshot_recovery_retains_suffix(
    true, false, false, false));
static_assert(!raft_server_snapshot_recovery_retains_suffix(
    false, false, false, true));
static_assert(raft_server_snapshot_recovery_has_unproven_gap(
    true, false, false));
static_assert(!raft_server_snapshot_recovery_has_unproven_gap(
    true, true, false));
static_assert(!raft_server_snapshot_recovery_has_unproven_gap(
    true, false, true));
static_assert(raft_server_snapshot_term_uses_boundary(11, 11));
static_assert(!raft_server_snapshot_term_uses_boundary(12, 11));
static_assert(raft_server_snapshot_marker_matches(16, 16, 11, 7, 11, 7));
static_assert(!raft_server_snapshot_marker_matches(15, 16, 11, 7, 11, 7));
static_assert(!raft_server_snapshot_marker_matches(16, 16, 10, 7, 11, 7));
static_assert(!raft_server_snapshot_marker_matches(16, 16, 11, 6, 11, 7));
static_assert(raft_server_election_result_is_current(true, 4, 4, 4));
static_assert(!raft_server_election_result_is_current(false, 4, 4, 4));
static_assert(!raft_server_election_result_is_current(true, 3, 4, 4));
static_assert(!raft_server_election_result_is_current(true, 4, 4, 5));
static_assert(raft_server_election_completion_action(
                  true, 4, 4, 4, 4) ==
              static_cast<int32_t>(
                  ElectionCompletionAction::APPLY_CURRENT));
static_assert(raft_server_election_completion_action(
                  true, 5, 4, 5, 5) ==
              static_cast<int32_t>(
                  ElectionCompletionAction::IGNORE_STALE));
static_assert(raft_server_election_completion_action(
                  true, 5, 4, 5, 6) ==
              static_cast<int32_t>(
                  ElectionCompletionAction::ADVANCE_HIGHER_TERM));
static_assert(raft_server_apply_epoch_is_current(8, 8));
static_assert(!raft_server_apply_epoch_is_current(7, 8));
static_assert(raft_server_log_index_has_successor(0));
static_assert(raft_server_log_index_has_successor(UINT64_MAX - 1));
static_assert(!raft_server_log_index_has_successor(UINT64_MAX));
static_assert(raft_server_append_prefix_is_compacted_miss(4, 5, 3));
static_assert(!raft_server_append_prefix_is_compacted_miss(3, 5, 3));
static_assert(raft_server_append_previous_term_is_acceptable(0, 7, 8));
static_assert(raft_server_append_is_acceptable(true, true, true));
static_assert(!raft_server_append_is_acceptable(false, true, true));
static_assert(!raft_server_append_is_acceptable(true, false, true));
static_assert(!raft_server_append_is_acceptable(true, true, false));
static_assert(raft_server_append_command_is_batch(4, 4));
static_assert(!raft_server_append_command_is_batch(19, 4));
static_assert(raft_server_append_entry_count_fits(UINT64_MAX, 0));
static_assert(!raft_server_append_entry_count_fits(UINT64_MAX, 1));
static_assert(raft_server_append_entry_count_fits(UINT64_MAX - 3, 3));
static_assert(!raft_server_append_entry_count_fits(UINT64_MAX - 3, 4));
static_assert(!raft_server_append_batch_count_is_valid(7, 0));
static_assert(raft_server_append_batch_count_is_valid(UINT64_MAX - 3, 3));
static_assert(!raft_server_append_batch_count_is_valid(UINT64_MAX - 3, 4));
static_assert(raft_server_append_entry_conflicts(false, 0, 7));
static_assert(raft_server_append_entry_conflicts(true, 6, 7));
static_assert(!raft_server_append_entry_conflicts(true, 7, 7));
static_assert(raft_server_append_result_last_index(10, 8, false) == 10);
static_assert(raft_server_append_result_last_index(10, 8, true) == 8);
static_assert(raft_server_append_result_last_index(8, 10, false) == 10);
static_assert(raft_server_append_sent_end(7, 0) == 7);
static_assert(raft_server_append_sent_end(7, 1) == 8);
static_assert(raft_server_append_sent_end(7, 4) == 11);
static_assert(raft_server_append_acknowledged_through(20, 10, 15) == 10);
static_assert(raft_server_append_acknowledged_through(8, 10, 15) == 8);
static_assert(raft_server_append_acknowledged_through(20, 15, 9) == 9);
static_assert(raft_server_commit_index_clamp(9, 7) == 7);
// The clamp policy is now scalar, so it tests as plain static_asserts like its
// 67 siblings. The majority SELECTION moved onto PeerTable, which owns a
// rusty::Vec and so cannot be constant-evaluated; raftLabTest covers it.
static_assert(raft_server_commit_index_candidate(7, 3, 100) == 7);
static_assert(raft_server_commit_index_candidate(9, 5, 100) == 9);
static_assert(raft_server_commit_index_candidate(7, 3, 5) == 5);
static_assert(raft_server_commit_index_candidate(0, 1, 42) == 42);
static_assert(raft_server_compaction_safe_index(12, 10, 8) == 8);
static_assert(raft_server_compaction_safe_index(7, 10, 8) == 7);
static_assert(raft_server_compaction_safe_index(9, 8, 10) == 8);
static_assert(raft_server_read_index_round_can_advance(0));
static_assert(!raft_server_read_index_round_can_advance(UINT64_MAX));
static_assert(raft_server_read_index_reply_confirms_authority(
    true, true, 7, 7, 7, 11, 11));
static_assert(!raft_server_read_index_reply_confirms_authority(
    true, true, 7, 7, 7, 10, 11));
static_assert(!raft_server_read_index_reply_confirms_authority(
    true, true, 7, 8, 7, 11, 11));
static_assert(raft_server_snapshot_progress_clamp(3, 5, 9) == 5);
static_assert(raft_server_snapshot_progress_clamp(7, 5, 9) == 7);
static_assert(raft_server_snapshot_progress_clamp(12, 5, 9) == 9);
static_assert(raft_server_snapshot_is_due(4, 10, 5));
static_assert(!raft_server_snapshot_is_due(10, 4, 5));
static_assert(raft_server_follower_next_index(7) == 8);
static_assert(raft_server_follower_next_index(UINT64_MAX) == 0);
// Pin every branch of the incumbent rejection-backoff decision tree. The
// helper arguments are (follower_last_log_index, current_next_index).
static_assert(raft_server_append_reject_can_fast_backoff(4, 20));
static_assert(raft_server_follower_next_index(4) == 5);
static_assert(!raft_server_append_reject_can_fast_backoff(4, 5));
static_assert(raft_server_append_reject_has_term_conflict(4, 5));
static_assert(raft_server_append_reject_decremented(5) == 4);
static_assert(!raft_server_append_reject_has_term_conflict(0, 1));
static_assert(raft_server_append_reject_can_halve(20));
static_assert(raft_server_append_reject_halved(20) == 10);
static_assert(!raft_server_append_reject_can_halve(10));
static_assert(raft_server_append_reject_can_decrement(10));
static_assert(raft_server_append_reject_decremented(10) == 9);
static_assert(!raft_server_append_reject_can_decrement(1));
static_assert(raft_server_append_reject_floor() == 1);
static_assert(raft_server_append_reject_can_fast_backoff(UINT64_MAX, 5));
static_assert(raft_server_follower_next_index(UINT64_MAX) == 0);
static_assert(!raft_server_append_reject_can_fast_backoff(UINT64_MAX, 0));
static_assert(!raft_server_append_reject_has_term_conflict(UINT64_MAX, 0));
static_assert(raft_server_start_was_rejected(RaftStartResult::REJECTED));
static_assert(!raft_server_start_was_rejected(RaftStartResult::APPENDED));
static_assert(raft_server_start_was_appended(RaftStartResult::APPENDED));
static_assert(!raft_server_start_was_appended(RaftStartResult::REJECTED));
static_assert(raft_server_retention_window_normalize(0) == 1);
static_assert(raft_server_retention_window_normalize(1) == 1);
static_assert(raft_server_retention_window_normalize(UINT64_MAX) == UINT64_MAX);
static_assert(raft_server_retention_cutoff(5, 5) == 0);
static_assert(raft_server_retention_cutoff(4, 5) == 0);
static_assert(raft_server_retention_cutoff(6, 5) == 1);
// Raft currently compares signed ballot_t values with uint64_t state_.current_term_.
// These casts make the existing C++ usual-arithmetic-conversion semantics
// explicit, including the historical negative-term edge case.
static_assert(!raft_server_vote_term_is_stale(static_cast<uint64_t>(-1), 0));
static_assert(raft_server_observed_higher_term(static_cast<uint64_t>(-1), 0));
static_assert(!raft_server_signed_term_is_newer(-1, 0));
static_assert(!raft_server_signed_term_is_newer(0, 0));
static_assert(raft_server_signed_term_is_newer(1, 0));
static_assert(raft_server_log_entry_is_current_term(
    -1, static_cast<uint64_t>(-1)));

// One log entry, owned by Rust.
//
// Was nine fields; seven were dead. max_ballot_seen_, max_ballot_accepted_,
// accepted_cmd_ and committed_cmd_ had ZERO uses in raft -- a tree-wide grep
// appears to show them live, but every hit is PaxosData (paxos/server.h:18),
// a separate struct that happens to share the field names. prevTerm, slot_id
// and ballot were written once each in SetLocalAppend and read nowhere.
//
// What is left is one scalar and one opaque carrier, each read at 13 sites in
// server.cc. The payload crosses as rusty::RaftCommand -- held, moved, handed
// to a kernel, never dereferenced from Rust -- which is the same opaque carry
// PendingTable already uses for this exact type.
//
// The payload's internal storage is still a shared_ptr<Marshallable>; calls
// to APIs that take one go through cmd().inner_marshallable(). Wire format
// unchanged -- see docs/dev/l10-unblock-plan.md.
//
// Deliberately NO method returning &mut: an entry is written when it is
// constructed and never afterwards. That is what makes "modify an existing
// entry" unspellable rather than merely discouraged, and it is why the
// send-time term stamp had to become a copy first (d295a4842).
#if RUSTYCPP_RUST
#[repr(C)]
pub struct RaftEntry {
    term_: i64,
    cmd_: rusty::RaftCommand,
}

impl RaftEntry {
    pub fn new(term: i64, cmd: rusty::RaftCommand) -> RaftEntry {
        RaftEntry { term_: term, cmd_: cmd }
    }

    pub fn term(&self) -> i64 {
        self.term_
    }

    // Handed back to C++, never followed from Rust.
    pub fn cmd(&self) -> &rusty::RaftCommand {
        &self.cmd_
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=raft_server.log_entry version=1 rust_sha256=98590c5f9b9334a9d3e73f09aff14969c2e5630711aef7b90e94e297cdc87453*/
struct RaftEntry;

struct RaftEntry {
    int64_t term_;
    rusty::RaftCommand cmd_;

    static RaftEntry new_(int64_t term, rusty::RaftCommand cmd);
    int64_t term() const;
    const rusty::RaftCommand& cmd() const;
};


inline RaftEntry RaftEntry::new_(int64_t term, rusty::RaftCommand cmd) {
    return RaftEntry{.term_ = std::move(term), .cmd_ = std::move(cmd)};
}

inline int64_t RaftEntry::term() const {
    return this->term_;
}

inline const rusty::RaftCommand& RaftEntry::cmd() const {
    return this->cmd_;
}
/*RUSTYCPP:GEN-END id=raft_server.log_entry*/

// The Raft log itself, owned by Rust.
//
// Fixed-size blocks plus the index of the first live entry, not a map.
//
// WHY DENSE. The log is contiguous by construction, and always has been. Only
// two paths insert. SetLocalAppend writes at last_log_index_ + 1. The
// AppendEntries overwrite loop writes a run whose first index is the first
// entry that is missing or term-conflicting -- which is either one past the
// local tail, or an index the same statement has just truncated back to.
// Every erase is a whole prefix or a whole suffix.
//
// That is an argument, not a measurement, so the conversion asserts it rather
// than assuming it: RaftLog::append returns the index it wrote and both call
// sites verify() it is the index they intended, so a gap aborts instead of
// appearing.
//
// WHY IT MATTERS. min_active_slot_ and last_log_index_ WERE a second and a
// third copy of the log's extents, advanced by hand at six write sites and
// never once checked against the container. They are base() and
// base() + len() - 1 now, and cannot disagree with it, because they are no
// longer stored. A transitional assertion carried both representations
// through raftLabTest's 25 cases and shard1ReplicationSimpleRaft and found
// they never once disagreed, which is what let the fields go.
//
// WHY BLOCKS AND NOT ONE VECTOR. One growing vector was measured at 2.0
// points of saturation throughput against the map it replaced, and it blew
// the tail out -- p99 +20%, p999 +25%, max +52%. The mechanism is the
// doubling reallocation, which copies the whole log while holding mtx_, so
// the whole pipeline stalls for as long as the memcpy takes. Pre-reserving
// the vector recovered those 2 points and pushed p99 and max BELOW the map
// baseline, which is what identified it. Blocks get the same result without
// having to guess a capacity: a full block is never touched again.
//
// rusty::Vec specifically: it re-exports std::vec::Vec on the rustc side and
// is the real vec_port on the C++ side, so both are faithful. See PeerTable's
// note above for why rusty::BTreeMap is not an option.
#if RUSTYCPP_RUST
#[repr(C)]
pub struct RaftLog {
    // Logical index of the first live entry.
    base_: u64,
    // How many entries at the front of blocks_[0] are dead (compacted away).
    head_: u64,
    // Live entry count.
    len_: u64,
    // Fixed-size blocks. Every block is exactly BLOCK long except the last.
    // Physical position of logical index i is head_ + (i - base_).
    blocks_: rusty::Vec<rusty::Vec<RaftEntry>>,
}

#[allow(clippy::new_without_default)]
impl RaftLog {
    pub fn new() -> RaftLog {
        RaftLog { base_: 1, head_: 0, len_: 0, blocks_: rusty::Vec::new() }
    }

    // Entries per block. 4096 * sizeof(RaftEntry) = 128KB, so a block is a
    // handful of huge pages' worth and the outer vector stays tiny: a
    // 400k-entry log is 98 pointers.
    pub fn block_len() -> u64 {
        4096
    }

    pub fn base(&self) -> u64 {
        self.base_
    }

    pub fn len(&self) -> usize {
        self.len_ as usize
    }

    pub fn is_empty(&self) -> bool {
        self.len_ == 0
    }

    pub fn last_index(&self) -> u64 {
        self.base_ + self.len_ - 1
    }

    pub fn holds(&self, index: u64) -> bool {
        index >= self.base_ && index - self.base_ < self.len_
    }

    pub fn get(&self, index: u64) -> rusty::Option<&RaftEntry> {
        if !self.holds(index) {
            return rusty::None;
        }
        let phys = self.head_ + (index - self.base_);
        let block = (phys / 4096) as usize;
        let slot = (phys % 4096) as usize;
        rusty::Some(&self.blocks_[block][slot])
    }

    // Appends at last_index() + 1 and returns it. Never moves an existing
    // entry: a full block is left alone and a new one is pushed, so the
    // reallocation stall that a single growing vector pays under the Raft
    // mutex does not exist here.
    pub fn append(&mut self, entry: RaftEntry) -> u64 {
        // "is there room in the last block", said directly rather than as
        // (head_ + len_) % BLOCK == 0, which clippy reads as a hand-rolled
        // is_multiple_of and which emits as a method call on a uint64_t.
        let need_block = self.blocks_.is_empty()
            || self.blocks_[self.blocks_.len() - 1].len() == 4096;
        if need_block {
            let fresh: rusty::Vec<RaftEntry> = rusty::Vec::with_capacity(4096);
            self.blocks_.push(fresh);
        }
        let last = self.blocks_.len() - 1;
        self.blocks_[last].push(entry);
        self.len_ += 1;
        self.base_ + self.len_ - 1
    }

    // Discard [index, end). A no-op past the tail, which is the ordinary
    // extend case.
    pub fn truncate_from(&mut self, index: u64) {
        if index <= self.base_ {
            self.blocks_.clear();
            self.head_ = 0;
            self.len_ = 0;
            return;
        }
        let keep = index - self.base_;
        if keep >= self.len_ {
            return;
        }
        let new_phys = self.head_ + keep;
        if new_phys == 0 {
            self.blocks_.clear();
        } else {
            let nblocks = new_phys.div_ceil(4096) as usize;
            self.blocks_.truncate(nblocks);
            let tail = (new_phys - 4096 * ((nblocks as u64) - 1)) as usize;
            self.blocks_[nblocks - 1].truncate(tail);
        }
        self.len_ = keep;
    }

    // Discard [base, index] -- snapshot compaction. Returns how many went.
    // Whole leading blocks are released; a partial block is retained and its
    // dead prefix is recorded in head_, so the index arithmetic stays exact
    // and no surviving entry is ever copied.
    pub fn compact_through(&mut self, index: u64) -> usize {
        if index < self.base_ {
            return 0;
        }
        let mut drop_count = index - self.base_ + 1;
        if drop_count > self.len_ {
            drop_count = self.len_;
        }
        self.head_ += drop_count;
        self.len_ -= drop_count;
        // index + 1, not base_ + drop_count. They agree whenever index is
        // inside the log, and when it is past the tail this is what the flat
        // vector did: the log empties and the index space restarts above the
        // compaction point rather than at the old tail.
        self.base_ = index + 1;
        while self.head_ >= 4096 && !self.blocks_.is_empty() {
            self.blocks_.remove(0);
            self.head_ -= 4096;
        }
        if self.len_ == 0 {
            self.blocks_.clear();
            self.head_ = 0;
        }
        drop_count as usize
    }

    // Drop everything and restart the index space at `base`. The follower
    // path after an InstallSnapshot that supersedes the whole local log.
    pub fn reset(&mut self, base: u64) {
        self.blocks_.clear();
        self.head_ = 0;
        self.len_ = 0;
        self.base_ = base;
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=raft_server.log_container version=1 rust_sha256=53ff563b18fcebf43b80f6887bca9ed71093bfdb7e7e2f76286714687dfd2bec*/
struct RaftLog;

struct RaftLog {
    uint64_t base_;
    uint64_t head_;
    uint64_t len_;
    rusty::Vec<rusty::Vec<RaftEntry>> blocks_;

    static RaftLog new_();
    static uint64_t block_len();
    uint64_t base() const;
    size_t len() const;
    bool is_empty() const;
    uint64_t last_index() const;
    bool holds(uint64_t index) const;
    rusty::Option<const RaftEntry&> get(uint64_t index) const;
    uint64_t append(RaftEntry entry);
    void truncate_from(uint64_t index);
    size_t compact_through(uint64_t index);
    void reset(uint64_t base);
};


inline RaftLog RaftLog::new_() {
    return RaftLog{.base_ = static_cast<uint64_t>(1), .head_ = static_cast<uint64_t>(0), .len_ = static_cast<uint64_t>(0), .blocks_ = rusty::Vec<rusty::Vec<RaftEntry>>::new_()};
}

inline uint64_t RaftLog::block_len() {
    return static_cast<uint64_t>(4096);
}

inline uint64_t RaftLog::base() const {
    return this->base_;
}

inline size_t RaftLog::len() const {
    return static_cast<size_t>(this->len_);
}

inline bool RaftLog::is_empty() const {
    return rusty::detail::deref_if_pointer_like(this->len_) == static_cast<uint64_t>(0);
}

inline uint64_t RaftLog::last_index() const {
    return (rusty::detail::deref_if_pointer_like(this->base_) + rusty::detail::deref_if_pointer_like(this->len_)) - static_cast<uint64_t>(1);
}

inline bool RaftLog::holds(uint64_t index) const {
    return (rusty::detail::deref_if_pointer_like(index) >= rusty::detail::deref_if_pointer_like(this->base_)) && ((rusty::detail::deref_if_pointer_like(index) - rusty::detail::deref_if_pointer_like(this->base_)) < rusty::detail::deref_if_pointer_like(this->len_));
}

inline rusty::Option<const RaftEntry&> RaftLog::get(uint64_t index) const {
    if (!this->holds(std::move(index))) {
        return rusty::None;
    }
    const auto phys = rusty::detail::deref_if_pointer_like(this->head_) + ((rusty::detail::deref_if_pointer_like(index) - rusty::detail::deref_if_pointer_like(this->base_)));
    const auto block = static_cast<size_t>((rusty::detail::deref_if_pointer_like(phys) / 4096));
    const auto slot = static_cast<size_t>((rusty::detail::deref_if_pointer_like(phys) % 4096));
    return rusty::Option<const RaftEntry&>(this->blocks_[block][slot]);
}

inline uint64_t RaftLog::append(RaftEntry entry) {
    const auto need_block = rusty::is_empty(this->blocks_) || (rusty::len(this->blocks_[rusty::len(this->blocks_) - 1]) == 4096);
    if (need_block) {
        rusty::Vec<RaftEntry> fresh = rusty::Vec<RaftEntry>::with_capacity(4096);
        this->blocks_.push(std::move(fresh));
    }
    const auto last = rusty::len(this->blocks_) - 1;
    this->blocks_[last].push(std::move(entry));
    this->len_ += 1;
    return (rusty::detail::deref_if_pointer_like(this->base_) + rusty::detail::deref_if_pointer_like(this->len_)) - static_cast<uint64_t>(1);
}

inline void RaftLog::truncate_from(uint64_t index) {
    if (rusty::detail::deref_if_pointer_like(index) <= rusty::detail::deref_if_pointer_like(this->base_)) {
        this->blocks_.clear();
        this->head_ = static_cast<uint64_t>(0);
        this->len_ = static_cast<uint64_t>(0);
        return;
    }
    auto keep = rusty::detail::deref_if_pointer_like(index) - rusty::detail::deref_if_pointer_like(this->base_);
    if (rusty::detail::deref_if_pointer_like(keep) >= rusty::detail::deref_if_pointer_like(this->len_)) {
        return;
    }
    const auto new_phys = rusty::detail::deref_if_pointer_like(this->head_) + rusty::detail::deref_if_pointer_like(keep);
    if (rusty::detail::deref_if_pointer_like(new_phys) == 0) {
        this->blocks_.clear();
    } else {
        const auto nblocks = static_cast<size_t>(rusty::div_ceil(new_phys, 4096));
        this->blocks_.truncate(std::move(nblocks));
        const auto tail = static_cast<size_t>((rusty::detail::deref_if_pointer_like(new_phys) - (4096 * ((((static_cast<uint64_t>(nblocks))) - 1)))));
        this->blocks_[rusty::detail::deref_if_pointer_like(nblocks) - 1].truncate(std::move(tail));
    }
    this->len_ = std::move(keep);
}

inline size_t RaftLog::compact_through(uint64_t index) {
    if (rusty::detail::deref_if_pointer_like(index) < rusty::detail::deref_if_pointer_like(this->base_)) {
        return static_cast<size_t>(0);
    }
    auto drop_count = (rusty::detail::deref_if_pointer_like(index) - rusty::detail::deref_if_pointer_like(this->base_)) + static_cast<uint64_t>(1);
    if (rusty::detail::deref_if_pointer_like(drop_count) > rusty::detail::deref_if_pointer_like(this->len_)) {
        drop_count = this->len_;
    }
    this->head_ += drop_count;
    this->len_ -= drop_count;
    this->base_ = rusty::detail::deref_if_pointer_like(index) + static_cast<uint64_t>(1);
    while ((rusty::detail::deref_if_pointer_like(this->head_) >= 4096) && rusty::detail::rust_not(rusty::is_empty(this->blocks_))) {
        this->blocks_.remove(0);
        this->head_ -= 4096;
    }
    if (rusty::detail::deref_if_pointer_like(this->len_) == static_cast<uint64_t>(0)) {
        this->blocks_.clear();
        this->head_ = static_cast<uint64_t>(0);
    }
    return static_cast<size_t>(drop_count);
}

inline void RaftLog::reset(uint64_t base) {
    this->blocks_.clear();
    this->head_ = static_cast<uint64_t>(0);
    this->len_ = static_cast<uint64_t>(0);
    this->base_ = std::move(base);
}
/*RUSTYCPP:GEN-END id=raft_server.log_container*/

#ifdef RAFT_TEST_CORO
#define HEARTBEAT_INTERVAL 100000
#else
#define HEARTBEAT_INTERVAL 5000
#endif


// @unsafe - inherits from non-@interface TxLogServer (individual methods are @safe)
// Per-follower replication progress.
//
// next_index_ and match_index_ were two std::maps keyed identically, always
// initialised together and asserted to have equal size. They are now ONE map of
// a DSL-owned value type, which removes the "find both, check both" dance at
// the reply site and makes the index arithmetic a method rather than five
// inline branches.
//
// The map itself stays C++: rusty::BTreeMap's rustc facade has no new(), no
// remove() and no mutable get, so converting the container would cost more
// facade work than this step is worth. The VALUE is what carries the logic.
#if RUSTYCPP_RUST
// SCREAMING_CASE variants match the surrounding C++ enum convention and the
// existing DSL enums in snapshot_format.hpp, which carries this same allow.
#[allow(non_camel_case_types)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Eq, PartialEq))]
#[repr(i32)]
pub enum BackoffKind {
    FAST = 0,
    TERM_CONFLICT = 1,
    EXPONENTIAL = 2,
    LINEAR = 3,
    FLOOR = 4,
}

#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Default, Eq, PartialEq))]
#[repr(C)]
pub struct FollowerProgress {
    pub next_: u64,
    pub match_: u64,
}

#[allow(clippy::new_without_default)]
impl FollowerProgress {
    pub fn new(next: u64, matched: u64) -> FollowerProgress {
        FollowerProgress { next_: next, match_: matched }
    }

    pub fn next_index(&self) -> u64 {
        self.next_
    }

    pub fn match_index(&self) -> u64 {
        self.match_
    }

    pub fn set_next_index(&mut self, value: u64) {
        self.next_ = value;
    }

    // The five-way backoff ladder taken when a follower rejects AppendEntries.
    // Returns which rung was used so the caller can log it; the arithmetic
    // itself is identical to the inline version it replaces.
    pub fn back_off_after_reject(&mut self, follower_last_log_index: u64) -> BackoffKind {
        if follower_last_log_index > 0
            && (follower_last_log_index + 1) < self.next_
        {
            self.next_ = follower_last_log_index + 1;
            return BackoffKind::FAST;
        }
        if follower_last_log_index > 0
            && (follower_last_log_index + 1) == self.next_
            && self.next_ > 1
        {
            self.next_ -= 1;
            return BackoffKind::TERM_CONFLICT;
        }
        if self.next_ > 10 {
            self.next_ /= 2;
            return BackoffKind::EXPONENTIAL;
        }
        if self.next_ > 1 {
            self.next_ -= 1;
            return BackoffKind::LINEAR;
        }
        self.next_ = 1;
        BackoffKind::FLOOR
    }

    // A successful AppendEntries proves the exact payload end and no more.
    // Both indices are monotonic: a late reply can never move them backwards.
    pub fn accept_through(&mut self, acknowledged_through: u64, has_successor: bool,
                          follower_next: u64) {
        if acknowledged_through > self.match_ {
            self.match_ = acknowledged_through;
        }
        if has_successor && follower_next > self.next_ {
            self.next_ = follower_next;
        }
    }
}

// The whole peer-progress cluster, owned by one type instead of scattered
// across a std::map keyed by site id.
//
// WHY A DENSE VECTOR. The replica set is fixed for the process lifetime:
// current_config_ has exactly one write, at server.cc:1671 inside Setup, and
// progress_'s key set was established once from it. Every follower therefore
// has a stable ordinal, and the map was paying a comparison and a cursor for
// what is an array index. The original plan proposed this shape and then
// abandoned it, recording that "the dense rewrite needs a stable
// site-to-ordinal mapping that does not exist" -- which that single-write
// measurement shows is not so.
//
// It also removes the map cursor as a category. Every access is by ordinal,
// computed fresh at each use, so there is no iterator to hold across an RPC
// send or a synchronous completion callback -- the hazard commit 4427129a9
// fixed by hand for next_index_, now unspellable.
//
// rusty::Vec specifically, not rusty::BTreeMap: Vec's rustc model is a
// re-export of std::vec::Vec and its C++ side is the real vec_port, so both
// sides are faithful. BTreeMap's rustc model is not -- its insert is a plain
// push with no key replacement and its get returns the first match
// (src/rrr/rusty-rustc/src/lib.rs:907) -- so a DSL type owning one would be
// verified against semantics production does not have.
pub struct PeerTable {
    progress_: rusty::Vec<FollowerProgress>,
}

#[allow(clippy::new_without_default)]
impl PeerTable {
    pub fn new() -> PeerTable {
        PeerTable { progress_: rusty::Vec::new() }
    }

    // One slot per follower, in ordinal order. Mirrors the two places the map
    // used to be filled.
    pub fn reset(&mut self, peers: usize, next_index: u64) {
        self.progress_.clear();
        let mut i: usize = 0;
        while i < peers {
            self.progress_.push(FollowerProgress::new(next_index, 0));
            i += 1;
        }
    }

    pub fn len(&self) -> usize {
        self.progress_.len()
    }

    // Required by clippy alongside len(). A leader always has followers in
    // this table unless the partition is single-replica, which is exactly the
    // case the commit-index selector special-cases.
    pub fn is_empty(&self) -> bool {
        self.progress_.is_empty()
    }

    pub fn next_index(&self, ordinal: usize) -> u64 {
        self.progress_[ordinal].next_index()
    }

    pub fn set_next_index(&mut self, ordinal: usize, value: u64) {
        self.progress_[ordinal].set_next_index(value);
    }

    pub fn match_index(&self, ordinal: usize) -> u64 {
        self.progress_[ordinal].match_index()
    }

    // The committable index this table's evidence supports.
    //
    // Both heartbeat phases used to build a std::vector of match indices,
    // std::sort it, and index (nservers - 1) / 2. The table owns those values,
    // so it can answer directly -- and it does so by RANK SELECTION rather
    // than sorting, because a DSL body has no working spelling for .sort():
    // the emitter lowers every receiver shape to rusty::sort, which is defined
    // only in the non-exported global module fragment of the transpiled ports
    // and is declared by no header. Selection is O(n^2) where sorting is
    // O(n log n), which is free at the replica counts this system runs (3 or
    // 5) and is on the per-round path, not the per-entry path.
    //
    // Ties are broken by ordinal so the result matches a stable sort exactly.
    pub fn majority_match_index(&self, nservers: usize, last_log_index: u64) -> u64 {
        let target = (nservers - 1) / 2;
        let n = self.progress_.len();
        let mut selected: u64 = 0;
        let mut i: usize = 0;
        while i < n {
            let value = self.progress_[i].match_index();
            let mut rank: usize = 0;
            let mut j: usize = 0;
            while j < n {
                let other = self.progress_[j].match_index();
                if other < value || (other == value && j < i) {
                    rank += 1;
                }
                j += 1;
            }
            if rank == target {
                selected = value;
            }
            i += 1;
        }
        raft_server_commit_index_candidate(selected, nservers, last_log_index)
    }

    pub fn back_off_after_reject(&mut self, ordinal: usize,
                                 follower_last_log_index: u64) -> BackoffKind {
        self.progress_[ordinal].back_off_after_reject(follower_last_log_index)
    }

    pub fn accept_through(&mut self, ordinal: usize, acknowledged_through: u64,
                          has_successor: bool, follower_next: u64) {
        self.progress_[ordinal].accept_through(acknowledged_through,
                                               has_successor, follower_next);
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=raft_server.follower_progress version=1 rust_sha256=3e9645ed30a8ca98ee0047674c33da994be4d39c9cb7c33c98d3fb74461c1f8f*/
enum class BackoffKind : int32_t;
constexpr BackoffKind BackoffKind_FAST();
constexpr BackoffKind BackoffKind_TERM_CONFLICT();
constexpr BackoffKind BackoffKind_EXPONENTIAL();
constexpr BackoffKind BackoffKind_LINEAR();
constexpr BackoffKind BackoffKind_FLOOR();
struct FollowerProgress;
struct PeerTable;

enum class BackoffKind : int32_t {
    FAST = 0,
    TERM_CONFLICT = 1,
    EXPONENTIAL = 2,
    LINEAR = 3,
    FLOOR = 4
};
inline constexpr BackoffKind BackoffKind_FAST() { return BackoffKind::FAST; }
inline constexpr BackoffKind BackoffKind_TERM_CONFLICT() { return BackoffKind::TERM_CONFLICT; }
inline constexpr BackoffKind BackoffKind_EXPONENTIAL() { return BackoffKind::EXPONENTIAL; }
inline constexpr BackoffKind BackoffKind_LINEAR() { return BackoffKind::LINEAR; }
inline constexpr BackoffKind BackoffKind_FLOOR() { return BackoffKind::FLOOR; }

struct FollowerProgress {
    uint64_t next_;
    uint64_t match_;

    static FollowerProgress new_(uint64_t next, uint64_t matched);
    uint64_t next_index() const;
    uint64_t match_index() const;
    void set_next_index(uint64_t value);
    BackoffKind back_off_after_reject(uint64_t follower_last_log_index);
    void accept_through(uint64_t acknowledged_through, bool has_successor, uint64_t follower_next);
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
};

struct PeerTable {
    rusty::Vec<FollowerProgress> progress_;

    static PeerTable new_();
    void reset(size_t peers, uint64_t next_index);
    size_t len() const;
    bool is_empty() const;
    uint64_t next_index(size_t ordinal) const;
    void set_next_index(size_t ordinal, uint64_t value);
    uint64_t match_index(size_t ordinal) const;
    uint64_t majority_match_index(size_t nservers, uint64_t last_log_index) const;
    BackoffKind back_off_after_reject(size_t ordinal, uint64_t follower_last_log_index);
    void accept_through(size_t ordinal, uint64_t acknowledged_through, bool has_successor, uint64_t follower_next);
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
};


inline FollowerProgress FollowerProgress::new_(uint64_t next, uint64_t matched) {
    return FollowerProgress{.next_ = std::move(next), .match_ = std::move(matched)};
}

inline uint64_t FollowerProgress::next_index() const {
    return this->next_;
}

inline uint64_t FollowerProgress::match_index() const {
    return this->match_;
}

inline void FollowerProgress::set_next_index(uint64_t value) {
    this->next_ = std::move(value);
}

inline BackoffKind FollowerProgress::back_off_after_reject(uint64_t follower_last_log_index) {
    if ((rusty::detail::deref_if_pointer_like(follower_last_log_index) > 0) && (((rusty::detail::deref_if_pointer_like(follower_last_log_index) + 1)) < rusty::detail::deref_if_pointer_like(this->next_))) {
        this->next_ = rusty::detail::deref_if_pointer_like(follower_last_log_index) + static_cast<uint64_t>(1);
        return rusty::clone(BackoffKind_FAST());
    }
    if (((rusty::detail::deref_if_pointer_like(follower_last_log_index) > 0) && (((rusty::detail::deref_if_pointer_like(follower_last_log_index) + static_cast<uint64_t>(1))) == rusty::detail::deref_if_pointer_like(this->next_))) && (rusty::detail::deref_if_pointer_like(this->next_) > 1)) {
        this->next_ -= 1;
        return rusty::clone(BackoffKind_TERM_CONFLICT());
    }
    if (rusty::detail::deref_if_pointer_like(this->next_) > 10) {
        this->next_ /= 2;
        return rusty::clone(BackoffKind_EXPONENTIAL());
    }
    if (rusty::detail::deref_if_pointer_like(this->next_) > 1) {
        this->next_ -= 1;
        return rusty::clone(BackoffKind_LINEAR());
    }
    this->next_ = static_cast<uint64_t>(1);
    return rusty::clone(rusty::clone(BackoffKind_FLOOR()));
}

inline void FollowerProgress::accept_through(uint64_t acknowledged_through, bool has_successor, uint64_t follower_next) {
    if (rusty::detail::deref_if_pointer_like(acknowledged_through) > rusty::detail::deref_if_pointer_like(this->match_)) {
        this->match_ = std::move(acknowledged_through);
    }
    if (rusty::detail::deref_if_pointer_like(has_successor) && (rusty::detail::deref_if_pointer_like(follower_next) > rusty::detail::deref_if_pointer_like(this->next_))) {
        this->next_ = std::move(follower_next);
    }
}

inline PeerTable PeerTable::new_() {
    return PeerTable{.progress_ = rusty::Vec<FollowerProgress>::new_()};
}

inline void PeerTable::reset(size_t peers, uint64_t next_index) {
    this->progress_.clear();
    size_t i = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < rusty::detail::deref_if_pointer_like(peers)) {
        this->progress_.push(FollowerProgress::new_(std::move(next_index), static_cast<uint64_t>(0)));
        i += 1;
    }
}

inline size_t PeerTable::len() const {
    return rusty::len(this->progress_);
}

inline bool PeerTable::is_empty() const {
    return rusty::is_empty(this->progress_);
}

inline uint64_t PeerTable::next_index(size_t ordinal) const {
    return this->progress_[ordinal].next_index();
}

inline void PeerTable::set_next_index(size_t ordinal, uint64_t value) {
    this->progress_[ordinal].set_next_index(std::move(value));
}

inline uint64_t PeerTable::match_index(size_t ordinal) const {
    return this->progress_[ordinal].match_index();
}

inline uint64_t PeerTable::majority_match_index(size_t nservers, uint64_t last_log_index) const {
    const auto target = ((rusty::detail::deref_if_pointer_like(nservers) - 1)) / 2;
    const auto n = rusty::len(this->progress_);
    uint64_t selected = static_cast<uint64_t>(0);
    size_t i = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < rusty::detail::deref_if_pointer_like(n)) {
        auto value = this->progress_[i].match_index();
        size_t rank = static_cast<size_t>(0);
        size_t j = static_cast<size_t>(0);
        while (rusty::detail::deref_if_pointer_like(j) < rusty::detail::deref_if_pointer_like(n)) {
            const auto other = this->progress_[j].match_index();
            if ((rusty::detail::deref_if_pointer_like(other) < rusty::detail::deref_if_pointer_like(value)) || (((rusty::detail::deref_if_pointer_like(other) == rusty::detail::deref_if_pointer_like(value)) && (rusty::detail::deref_if_pointer_like(j) < rusty::detail::deref_if_pointer_like(i))))) {
                rank += 1;
            }
            j += 1;
        }
        if (rusty::detail::deref_if_pointer_like(rank) == rusty::detail::deref_if_pointer_like(target)) {
            selected = std::move(value);
        }
        i += 1;
    }
    return raft_server_commit_index_candidate(std::move(selected), std::move(nservers), std::move(last_log_index));
}

inline BackoffKind PeerTable::back_off_after_reject(size_t ordinal, uint64_t follower_last_log_index) {
    return this->progress_[ordinal].back_off_after_reject(std::move(follower_last_log_index));
}

inline void PeerTable::accept_through(size_t ordinal, uint64_t acknowledged_through, bool has_successor, uint64_t follower_next) {
    this->progress_[ordinal].accept_through(std::move(acknowledged_through), std::move(has_successor), std::move(follower_next));
}
/*RUSTYCPP:GEN-END id=raft_server.follower_progress*/

// The consensus state that mtx_ guards, owned by Rust.
//
// mtx_ guards its sibling members by CONVENTION: nothing in the C++ says which
// fields it covers, which is why a census found 24 members touched on both
// sides of it. Rust's Mutex<T> guards by OWNERSHIP -- it contains the data and
// lock() is the only way to reach it -- so the conversion needs the guarded
// fields gathered into one type first. This is that type, starting with the
// members measured to be touched ONLY under the lock.
//
// Still a plain member behind the C++ mtx_ for now. Making it
// rusty::Mutex<RaftConsensusState> is the next step and is now unblocked,
// because mtx_ is no longer recursive -- rusty::Mutex cannot be, since its
// lock() hands out a reference to the guarded data.
//
// Fields stay public: the C++ that has not been converted yet reaches them as
// state_.field, exactly as it reached them as bare members. Methods move onto
// this type as the bodies that use them convert.
#if RUSTYCPP_RUST
#[repr(C)]
pub struct RaftConsensusState {
    // Election cluster.
    election_term_: i64,
    election_timeout_us_: u64,
    election_timer_generation_: u64,
    vote_for_: u16,
    // Snapshot configuration and callback ownership.
    snapshot_threshold_: u64,
    snapshot_callback_owner_token_: u64,
    next_snapshot_callback_owner_token_: u64,
    // Leadership, and the campaign in progress.
    is_leader_: bool,
    req_voting_: bool,
    election_in_progress_: bool,
    current_leader_id_: u16,
    last_heartbeat_time_: u64,
    // Read-index evidence: the round counter and the newest confirmed proof.
    heartbeat_round_: u64,
    read_quorum_confirmed_term_: u64,
    read_quorum_confirmed_round_: u64,
    // Log store: the term the server is in, and the three indices that bound
    // the log. NOTE the historical naming -- these four are the only members
    // in the class without a trailing underscore.
    current_term_: u64,
    commit_index_: u64,
    execute_index_: u64,
    // Snapshot boundary.
    snapidx_: u64,
    snapterm_: i64,
}

#[allow(clippy::new_without_default)]
impl RaftConsensusState {
    pub fn new() -> RaftConsensusState {
        RaftConsensusState {
            election_term_: 0,
            election_timeout_us_: 0,
            election_timer_generation_: 0,
            // INVALID_SITEID is (siteid_t)-1 and siteid_t is uint16_t.
            vote_for_: u16::MAX,
            // Anything before this slot has been freed by compaction.
            snapshot_threshold_: 10000,
            snapshot_callback_owner_token_: 0,
            next_snapshot_callback_owner_token_: 1,
            is_leader_: false,
            req_voting_: false,
            election_in_progress_: false,
            current_leader_id_: u16::MAX,
            last_heartbeat_time_: 0,
            heartbeat_round_: 0,
            read_quorum_confirmed_term_: 0,
            read_quorum_confirmed_round_: 0,
            current_term_: 0,
            commit_index_: 0,
            execute_index_: 0,
            snapidx_: 0,
            snapterm_: 0,
        }
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=raft_server.consensus_state version=1 rust_sha256=42e53307ce4bdd5c68ba98c998e2a1315321f50735f6a39a9df793f5eb81d438*/
struct RaftConsensusState;

struct RaftConsensusState {
    int64_t election_term_;
    uint64_t election_timeout_us_;
    uint64_t election_timer_generation_;
    uint16_t vote_for_;
    uint64_t snapshot_threshold_;
    uint64_t snapshot_callback_owner_token_;
    uint64_t next_snapshot_callback_owner_token_;
    bool is_leader_;
    bool req_voting_;
    bool election_in_progress_;
    uint16_t current_leader_id_;
    uint64_t last_heartbeat_time_;
    uint64_t heartbeat_round_;
    uint64_t read_quorum_confirmed_term_;
    uint64_t read_quorum_confirmed_round_;
    uint64_t current_term_;
    uint64_t commit_index_;
    uint64_t execute_index_;
    uint64_t snapidx_;
    int64_t snapterm_;

    static RaftConsensusState new_();
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
};


inline RaftConsensusState RaftConsensusState::new_() {
    return RaftConsensusState{.election_term_ = static_cast<int64_t>(0), .election_timeout_us_ = static_cast<uint64_t>(0), .election_timer_generation_ = static_cast<uint64_t>(0), .vote_for_ = std::numeric_limits<uint16_t>::max(), .snapshot_threshold_ = static_cast<uint64_t>(10000), .snapshot_callback_owner_token_ = static_cast<uint64_t>(0), .next_snapshot_callback_owner_token_ = static_cast<uint64_t>(1), .is_leader_ = false, .req_voting_ = false, .election_in_progress_ = false, .current_leader_id_ = std::numeric_limits<uint16_t>::max(), .last_heartbeat_time_ = static_cast<uint64_t>(0), .heartbeat_round_ = static_cast<uint64_t>(0), .read_quorum_confirmed_term_ = static_cast<uint64_t>(0), .read_quorum_confirmed_round_ = static_cast<uint64_t>(0), .current_term_ = static_cast<uint64_t>(0), .commit_index_ = static_cast<uint64_t>(0), .execute_index_ = static_cast<uint64_t>(0), .snapidx_ = static_cast<uint64_t>(0), .snapterm_ = static_cast<int64_t>(0)};
}
/*RUSTYCPP:GEN-END id=raft_server.consensus_state*/

// The election timer loop, owned by Rust.
//
// This is the first loop in Raft whose control flow -- not merely its
// arithmetic -- lives in the DSL. The Rust body runs the whole `while` that
// used to sit inside StartElectionTimer's fiber lambda; C++ keeps only the
// eight kernels below, each of which is an operation that genuinely cannot
// cross: a lock, a private member read, an rrr logging macro, or a call into
// another RaftServer method.
//
// ON THE OPAQUE HANDLE. ElectionTimerLoop carries the server as
// `*mut core::ffi::c_void`, not as a pointer to a modelled RaftServer. That is
// deliberate and it is the whole safety argument. A Rust type that modelled
// RaftServer's fields would typecheck against a hand-written model while the
// apply thread and the submit edge mutate the same members concurrently
// (see docs/migration/raft/heartbeat-first-conversion-plan.md section 3.5),
// producing a green borrow check over an untrue premise. `c_void` makes that
// structurally impossible: Rust cannot dereference it, so every read of server
// state is forced through a kernel that takes the lock the way the old inline
// code did. The handle is carried, moved and handed back -- never followed.
#if RUSTYCPP_RUST
// One locked gather's worth of election state. Plain copies, so the loop can
// branch on them after the lock is released, exactly as the C++ did.
// repr(C) is mandatory, not decorative: raft_election_gather returns this
// across an extern "C" boundary, so Rust's layout must be the C++ struct's.
#[repr(C)]
pub struct ElectionTick {
    time_elapsed_: u64,
    election_timeout_: u64,
    heartbeat_time_: u64,
    generation_: u64,
    term_: u64,
    vote_for_: u16,
    fired_: bool,
}

#[allow(clippy::too_many_arguments)]
impl ElectionTick {
    pub fn new(time_elapsed: u64, election_timeout: u64, heartbeat_time: u64,
               generation: u64, term: u64, vote_for: u16, fired: bool) -> ElectionTick {
        ElectionTick {
            time_elapsed_: time_elapsed,
            election_timeout_: election_timeout,
            heartbeat_time_: heartbeat_time,
            generation_: generation,
            term_: term,
            vote_for_: vote_for,
            fired_: fired,
        }
    }

    pub fn fired(&self) -> bool { self.fired_ }
    pub fn generation(&self) -> u64 { self.generation_ }
    pub fn time_elapsed(&self) -> u64 { self.time_elapsed_ }
    pub fn election_timeout(&self) -> u64 { self.election_timeout_ }
    pub fn heartbeat_time(&self) -> u64 { self.heartbeat_time_ }
    pub fn term(&self) -> u64 { self.term_ }
    pub fn vote_for(&self) -> u16 { self.vote_for_ }
}

pub struct ElectionTimerLoop {
    server_: *mut core::ffi::c_void,
    wait_int_us_: u64,
}

impl ElectionTimerLoop {
    pub fn new(server: *mut core::ffi::c_void, wait_int_us: u64) -> ElectionTimerLoop {
        ElectionTimerLoop { server_: server, wait_int_us_: wait_int_us }
    }

    // The body of the fiber. Structurally identical to the C++ it replaces:
    // wait a randomised sub-interval, gather under the lock, and if the
    // timeout fired, campaign and then wait out the vote before looping.
    pub fn run(&self) {
        unsafe { raft_election_log_start(self.server_) };
        while !unsafe { raft_election_stopped(self.server_) } {
            let delay = unsafe { raft_election_random_delay(self.server_) };
            // Unlike a plain sleep this is interrupted by shutdown, so a
            // false return means "stop", not "timed out".
            if !unsafe { raft_election_wait(self.server_, delay) } {
                break;
            }
            let tick = unsafe { raft_election_gather(self.server_) };
            if tick.fired() {
                unsafe { raft_election_log_fired(self.server_, &tick) };
                // Re-check before campaigning: RequestVote reaches through a
                // vtable that a concurrent destructor may already have
                // collapsed.
                if unsafe { raft_election_stopped(self.server_) } {
                    break;
                }
                unsafe { raft_election_request_vote(self.server_, tick.generation()) };
                if !self.await_vote_settled() {
                    break;
                }
            }
        }
        unsafe { raft_election_set_running(self.server_, false) };
    }

    // Returns true when voting finished normally, false when shutdown cut it
    // short. The C++ spelled both as `break` out of the inner loop and let the
    // outer `while (!stop_)` sort them out; naming the two outcomes is the one
    // place this reads differently from the original, and the observable
    // behaviour is the same.
    fn await_vote_settled(&self) -> bool {
        loop {
            if !unsafe { raft_election_is_voting(self.server_) } {
                return true;
            }
            rusty::ReactorFiber::sleep(self.wait_int_us_);
            if unsafe { raft_election_stopped(self.server_) } {
                return false;
            }
        }
    }
}

// The C++ side. Each takes the opaque handle and casts it back exactly once.
unsafe extern "C" {
    fn raft_election_stopped(server: *mut core::ffi::c_void) -> bool;
    fn raft_election_is_voting(server: *mut core::ffi::c_void) -> bool;
    fn raft_election_random_delay(server: *mut core::ffi::c_void) -> u64;
    fn raft_election_wait(server: *mut core::ffi::c_void, timeout_us: u64) -> bool;
    fn raft_election_gather(server: *mut core::ffi::c_void) -> ElectionTick;
    fn raft_election_log_start(server: *mut core::ffi::c_void);
    fn raft_election_log_fired(server: *mut core::ffi::c_void, tick: &ElectionTick);
    fn raft_election_request_vote(server: *mut core::ffi::c_void, generation: u64);
    fn raft_election_set_running(server: *mut core::ffi::c_void, running: bool);
}
#endif
/*RUSTYCPP:GEN-BEGIN id=raft_server.election_timer version=1 rust_sha256=22ace867d33370be4652baeb5ab6ce64294dfdb0fcb9053015137c8f5a210c3e*/
struct ElectionTick;
struct ElectionTimerLoop;

struct ElectionTick {
    uint64_t time_elapsed_;
    uint64_t election_timeout_;
    uint64_t heartbeat_time_;
    uint64_t generation_;
    uint64_t term_;
    uint16_t vote_for_;
    bool fired_;

    static ElectionTick new_(uint64_t time_elapsed, uint64_t election_timeout, uint64_t heartbeat_time, uint64_t generation, uint64_t term, uint16_t vote_for, bool fired);
    bool fired() const;
    uint64_t generation() const;
    uint64_t time_elapsed() const;
    uint64_t election_timeout() const;
    uint64_t heartbeat_time() const;
    uint64_t term() const;
    uint16_t vote_for() const;
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
};

struct ElectionTimerLoop {
    rusty::ffi::c_void* server_;
    uint64_t wait_int_us_;

    static ElectionTimerLoop new_(rusty::ffi::c_void* server, uint64_t wait_int_us);
    void run() const;
    bool await_vote_settled() const;
};

extern "C" {
    bool raft_election_stopped(rusty::ffi::c_void* server);
    bool raft_election_is_voting(rusty::ffi::c_void* server);
    uint64_t raft_election_random_delay(rusty::ffi::c_void* server);
    bool raft_election_wait(rusty::ffi::c_void* server, uint64_t timeout_us);
    ElectionTick raft_election_gather(rusty::ffi::c_void* server);
    void raft_election_log_start(rusty::ffi::c_void* server);
    void raft_election_log_fired(rusty::ffi::c_void* server, const ElectionTick& tick);
    void raft_election_request_vote(rusty::ffi::c_void* server, uint64_t generation);
    void raft_election_set_running(rusty::ffi::c_void* server, bool running);
}


inline ElectionTick ElectionTick::new_(uint64_t time_elapsed, uint64_t election_timeout, uint64_t heartbeat_time, uint64_t generation, uint64_t term, uint16_t vote_for, bool fired) {
    return ElectionTick{.time_elapsed_ = std::move(time_elapsed), .election_timeout_ = std::move(election_timeout), .heartbeat_time_ = std::move(heartbeat_time), .generation_ = std::move(generation), .term_ = std::move(term), .vote_for_ = std::move(vote_for), .fired_ = std::move(fired)};
}

inline bool ElectionTick::fired() const {
    return this->fired_;
}

inline uint64_t ElectionTick::generation() const {
    return this->generation_;
}

inline uint64_t ElectionTick::time_elapsed() const {
    return this->time_elapsed_;
}

inline uint64_t ElectionTick::election_timeout() const {
    return this->election_timeout_;
}

inline uint64_t ElectionTick::heartbeat_time() const {
    return this->heartbeat_time_;
}

inline uint64_t ElectionTick::term() const {
    return this->term_;
}

inline uint16_t ElectionTick::vote_for() const {
    return this->vote_for_;
}

inline ElectionTimerLoop ElectionTimerLoop::new_(rusty::ffi::c_void* server, uint64_t wait_int_us) {
    return ElectionTimerLoop{.server_ = server, .wait_int_us_ = std::move(wait_int_us)};
}

inline void ElectionTimerLoop::run() const {
    // @unsafe
    {
        raft_election_log_start(this->server_);
    }
    while (!raft_election_stopped(this->server_)) {
        auto delay = raft_election_random_delay(this->server_);
        if (!raft_election_wait(this->server_, std::move(delay))) {
            break;
        }
        const auto tick = raft_election_gather(this->server_);
        if (tick.fired()) {
            // @unsafe
            {
                raft_election_log_fired(this->server_, tick);
            }
            if (raft_election_stopped(this->server_)) {
                break;
            }
            // @unsafe
            {
                raft_election_request_vote(this->server_, tick.generation());
            }
            if (!this->await_vote_settled()) {
                break;
            }
        }
    }
    // @unsafe
    {
        raft_election_set_running(this->server_, false);
    }
}

inline bool ElectionTimerLoop::await_vote_settled() const {
    while (true) {
        if (!raft_election_is_voting(this->server_)) {
            return true;
        }
        rusty::ReactorFiber::sleep(this->wait_int_us_);
        if (raft_election_stopped(this->server_)) {
            return false;
        }
    }
}
/*RUSTYCPP:GEN-END id=raft_server.election_timer*/

// The heartbeat loop, owned by Rust.
//
// Same shape as ElectionTimerLoop above and for the same reason: the outer
// while, the lifecycle and the continuation decision are Rust; everything
// that touches shared RaftServer state is a C++ kernel behind an opaque
// handle. Two handles here rather than one, because the round-carried state
// (the pending-RPC table, the authority generations, the leader-term latch)
// outlives a round but not the loop, and holds unique_ptr and wire types that
// have no DSL spelling. Rust carries it and hands it back; it never looks in.
#if RUSTYCPP_RUST
pub struct HeartbeatDriver {
    server_: *mut core::ffi::c_void,
    round_: *mut core::ffi::c_void,
}

impl HeartbeatDriver {
    pub fn new(server: *mut core::ffi::c_void,
               round: *mut core::ffi::c_void) -> HeartbeatDriver {
        HeartbeatDriver { server_: server, round_: round }
    }

    // decide -> emit -> collect -> decide, which is the shape the C++
    // already had as four comment-delimited phases. It is now the shape of
    // the Rust that sequences them.
    pub fn run(&self) {
        unsafe { raft_heartbeat_prologue(self.server_) };
        while unsafe { raft_heartbeat_looping(self.server_) } {
            // The wake gate returns false on shutdown rather than on timeout.
            if !unsafe { raft_heartbeat_wait(self.server_) } {
                break;
            }
            // PHASE 0 declines the round when leadership is not held. The C++
            // spelled that `continue`.
            if !unsafe { raft_heartbeat_phase0(self.server_, self.round_) } {
                continue;
            }
            unsafe { raft_heartbeat_phase1(self.server_, self.round_) };
            unsafe { raft_heartbeat_phase2(self.server_, self.round_) };
            unsafe { raft_heartbeat_phase3(self.server_, self.round_) };
        }
        unsafe { raft_heartbeat_epilogue(self.server_) };
    }
}

unsafe extern "C" {
    fn raft_heartbeat_prologue(server: *mut core::ffi::c_void);
    fn raft_heartbeat_looping(server: *mut core::ffi::c_void) -> bool;
    fn raft_heartbeat_wait(server: *mut core::ffi::c_void) -> bool;
    fn raft_heartbeat_phase0(server: *mut core::ffi::c_void,
                             round: *mut core::ffi::c_void) -> bool;
    fn raft_heartbeat_phase1(server: *mut core::ffi::c_void,
                             round: *mut core::ffi::c_void);
    fn raft_heartbeat_phase2(server: *mut core::ffi::c_void,
                             round: *mut core::ffi::c_void);
    fn raft_heartbeat_phase3(server: *mut core::ffi::c_void,
                             round: *mut core::ffi::c_void);
    fn raft_heartbeat_epilogue(server: *mut core::ffi::c_void);
}
#endif
/*RUSTYCPP:GEN-BEGIN id=raft_server.heartbeat_driver version=1 rust_sha256=d45c8272767dbc00023a8d7680890f9c5d788b549fe36e71c5fb7cae1d0d95df*/
struct HeartbeatDriver;

struct HeartbeatDriver {
    rusty::ffi::c_void* server_;
    rusty::ffi::c_void* round_;

    static HeartbeatDriver new_(rusty::ffi::c_void* server, rusty::ffi::c_void* round);
    void run() const;
};

extern "C" {
    void raft_heartbeat_prologue(rusty::ffi::c_void* server);
    bool raft_heartbeat_looping(rusty::ffi::c_void* server);
    bool raft_heartbeat_wait(rusty::ffi::c_void* server);
    bool raft_heartbeat_phase0(rusty::ffi::c_void* server, rusty::ffi::c_void* round);
    void raft_heartbeat_phase1(rusty::ffi::c_void* server, rusty::ffi::c_void* round);
    void raft_heartbeat_phase2(rusty::ffi::c_void* server, rusty::ffi::c_void* round);
    void raft_heartbeat_phase3(rusty::ffi::c_void* server, rusty::ffi::c_void* round);
    void raft_heartbeat_epilogue(rusty::ffi::c_void* server);
}


inline HeartbeatDriver HeartbeatDriver::new_(rusty::ffi::c_void* server, rusty::ffi::c_void* round) {
    return HeartbeatDriver{.server_ = server, .round_ = round};
}

inline void HeartbeatDriver::run() const {
    // @unsafe
    {
        raft_heartbeat_prologue(this->server_);
    }
    while (raft_heartbeat_looping(this->server_)) {
        if (!raft_heartbeat_wait(this->server_)) {
            break;
        }
        if (!raft_heartbeat_phase0(this->server_, this->round_)) {
            continue;
        }
        // @unsafe
        {
            raft_heartbeat_phase1(this->server_, this->round_);
        }
        // @unsafe
        {
            raft_heartbeat_phase2(this->server_, this->round_);
        }
        // @unsafe
        {
            raft_heartbeat_phase3(this->server_, this->round_);
        }
    }
    // @unsafe
    {
        raft_heartbeat_epilogue(this->server_);
    }
}
/*RUSTYCPP:GEN-END id=raft_server.heartbeat_driver*/

class RaftServer : public TxLogServer {
 public:
  // ==========================================================================
  // ELECTION TIMER KERNELS
  //
  // The C++ half of the DSL-owned ElectionTimerLoop declared above. The Rust
  // loop holds this object only as an opaque void*, so every one of its reads
  // and writes of RaftServer state lands here, where the lock discipline is
  // the same as the inline code these replace. Each is the smallest operation
  // that genuinely cannot cross the boundary: a recursive_mutex acquisition, a
  // private member read, an rrr logging macro, or a call to another method.
  // ==========================================================================

  // @safe - relaxed atomic read, no lock needed (the C++ read it the same way)
  bool ElectionLoopStopped() const;
  // @unsafe - takes mtx_ to read state_.req_voting_
  bool ElectionLoopVoting();
  // @unsafe - RandomGenerator is external
  uint64_t ElectionLoopRandomDelay() const;
  // @unsafe - suspends this fiber on the wake gate's election waiter
  bool ElectionLoopWait(uint64_t timeout_us);
  // @unsafe - takes mtx_ and reads the election cluster
  ElectionTick ElectionLoopGather();
  // @unsafe - rrr logging macro
  void ElectionLoopLogStart() const;
  // @unsafe - rrr logging macro
  void ElectionLoopLogFired(const ElectionTick& tick) const;
  // @unsafe - dispatches through the vtable; caller re-checks stop_ first
  void ElectionLoopRequestVote(uint64_t generation);
  // @safe - release store on an atomic
  void ElectionLoopSetRunning(bool running);

  // ==========================================================================
  // HEARTBEAT LOOP KERNELS
  //
  // The C++ half of the DSL-owned HeartbeatDriver declared above. The round
  // body is still one kernel; splitting it into the four phases is the next
  // tranche.
  // ==========================================================================

  // @unsafe - timer allocation, progress_ initialisation, atomic stores
  void HeartbeatPrologue();
  // @safe - acquire load
  bool HeartbeatLooping() const;
  // @safe - linear scan of a fixed, tiny table (replica counts are 3 or 5).
  // Returns peers_.len() when the site is not a follower of this leader,
  // which is the "removed follower" case PHASE 2 guards against. Deliberately
  // returns an ordinal rather than a reference: an ordinal cannot dangle
  // across an RPC send or a re-entrant completion callback.
  size_t PeerOrdinal(siteid_t site) const {
    for (size_t ord = 0; ord < peer_sites_.size(); ord++) {
      if (peer_sites_[ord] == site) {
        return ord;
      }
    }
    return peers_.len();
  }

  // @unsafe - rebuilds the ordinal peer tables; CALLER MUST HOLD mtx_
  void RebuildPeerTables(uint64_t next_index);

  // @unsafe - suspends on the wake gate; false means shutdown
  bool HeartbeatWait();
  // @unsafe - advances the read-index round and recomputes the commit index;
  // false means leadership is not held, so phases 1-3 are skipped
  bool HeartbeatPhase0(struct HeartbeatRoundState& state,
                       struct HeartbeatRoundScope& round);
  // @unsafe - builds and sends AppendEntries / InstallSnapshot per follower
  void HeartbeatPhase1(struct HeartbeatRoundState& state,
                       struct HeartbeatRoundScope& round);
  // @unsafe - polls replies through one round deadline and processes them
  void HeartbeatPhase2(struct HeartbeatRoundState& state,
                       struct HeartbeatRoundScope& round);
  // @unsafe - recomputes the commit index and publishes read-index authority
  void HeartbeatPhase3(struct HeartbeatRoundState& state,
                       struct HeartbeatRoundScope& round);
  // @safe - two release stores
  void HeartbeatEpilogue();

  // The five site fields and the mutex used to arrive by inheriting
  // TxLogServer's data members. They are declared here now; every body that
  // reads them -- 164 `site_id_`, 20 `partition_id_`, 10 `loc_id_`, 6
  // `app_next_` and 48 `mtx_` acquisitions -- is unchanged. See
  // src/deptran/scheduler.h for why, and cpp-refactor-plan.md gate G4.
  //
  // mtx_ being Raft's own is the point: it is what lets Tranche 5 replace this
  // recursive mutex with a single Mutex<RaftState> without touching Paxos.
  TXLOG_SERVER_SITE_FIELDS()
  std::mutex mtx_{};

  // The consensus cluster mtx_ guards, now one Rust-owned value instead of
  // eight bare members. Reached as state_.field by C++ that has not converted.
  RaftConsensusState state_{RaftConsensusState::new_()};
  TXLOG_SERVER_SITE_METHODS()

 private:
  struct AsyncCallbackLifetime {
    std::mutex mutex;
    RaftServer* server = nullptr;
  };

  // RPC futures can outlive the server during shutdown. Destruction nulls
  // this shared gate after waiting for any callback already using it.
  std::shared_ptr<AsyncCallbackLifetime> async_callback_lifetime_ =
      std::make_shared<AsyncCallbackLifetime>();

  // Atomic append path shared by the public Start() entry point.
  RaftStartResult StartImpl(const janus::Command& cmd,
                            uint64_t* index,
                            uint64_t* term,
                            slotid_t slot_id,
                            ballot_t ballot);

  // ============================================================================
  // SNAPSHOT SUPPORT
  // ============================================================================
  std::shared_ptr<janus::raft::SnapshotManager> snapshot_manager_;  // Optional snapshot manager
  // Apply-thread trigger mirrors. The state-machine hot path must not race on
  // snapshot_manager_, state_.snapidx_, or state_.snapshot_threshold_; it reads only these
  // atomics and lets MaybeCreateSnapshot() revalidate under the full lock
  // order before doing any work.
  rusty::sync::atomic::AtomicBool snapshot_manager_configured_{false};
  rusty::sync::atomic::AtomicU64 snapshot_trigger_index_{0};
  rusty::sync::atomic::AtomicU64 snapshot_trigger_threshold_{10000};

  // State machine snapshot callbacks (set by the application state machine)
  // @unsafe - std::function holds non-borrow-checked closures
  // The requested boundary is part of the callback contract: an application
  // checkpoint that represents any other applied index must be rejected before
  // Raft durably publishes the snapshot or compacts its reconstruction log.
  std::function<std::string(uint64_t)> create_sm_snapshot_cb_;
  std::function<std::unique_ptr<PreparedStateMachineSnapshotInstall>(
      const std::string&, uint64_t)> prepare_sm_snapshot_cb_;

  // @unsafe - Initializes the in-memory snapshot manager and restores the exact
  // state machine bytes before publishing any recovered snapshot boundary.
  bool InitializeSnapshotManager();

  // @unsafe - Caller holds state_machine_apply_mtx_ then mtx_. Fully validates
  // and stages a production state-machine image without publishing it, or
  // validates the RaftLab marker payload and returns a no-op transaction.
  std::unique_ptr<PreparedStateMachineSnapshotInstall>
  PrepareStateMachineSnapshotLocked(
      const std::string& data,
      uint64_t last_included_index,
      uint64_t last_included_term);

  // @unsafe - Startup helper for a snapshot already held by the manager.
  // Prepares and immediately commits its state-machine image before publishing
  // recovery.
  bool LoadStateMachineSnapshotLocked(
      const std::string& data,
      uint64_t last_included_index,
      uint64_t last_included_term);


  // @unsafe - Requires state_machine_apply_mtx_ and mtx_ in that order.
  // Split out so the apply trigger and RaftLabTest's LabAccess-driven manager
  // rotation helper can preserve the global lock order without re-locking.
  bool CreateSnapshotLocked();

  // @unsafe - Cheap-trigger slow path. Acquires state_machine_apply_mtx_ then
  // mtx_, rechecks the canonical snapshot state, and snapshots only if due.
  void MaybeCreateSnapshot();

  // ============================================================================

  // One map of FollowerProgress, replacing the two identically-keyed maps
  // match_index_ and next_index_. They were always initialised together and
  // asserted to have equal size; merging removes the "find both, check both"
  // dance at the reply site. The value type is DSL-owned; see server.cc.
  // Peer progress, indexed by ordinal. peer_sites_ is the ordinal -> site id
  // map, fixed at the same moment peers_ is sized; both come from
  // current_config_, which is written once during Setup.
  PeerTable peers_{PeerTable::new_()};
  std::vector<siteid_t> peer_sites_{};
  // Heartbeat quorum proof, guarded by mtx_. HeartbeatLoop stamps every round
  // with state_.heartbeat_round_ and records the newest round that a quorum of the
  // membership configuration confirmed in the current term.
  // Election timing is one mutex-protected campaign. A reset samples exactly
  // one timeout and advances the generation; the timer must never redraw the
  // random timeout on each poll or start a campaign from an expired snapshot
  // after a concurrent heartbeat reset.
  // @safe - logging calls wrapped in @unsafe blocks in implementation
  void LogTermChange(const char* reason, uint64_t old_term, uint64_t new_term, siteid_t source = INVALID_SITEID);
  rusty::sync::atomic::AtomicBool stop_{false};
  // Consensus RPC services are registered before their owner-thread Setup job
  // runs. Admission stays closed until snapshot recovery has completed, and
  // closes again before shutdown drains.
  rusty::sync::atomic::AtomicBool rpc_ready_{false};
  // Worker launch/readiness waits for the owner-thread Setup job to finish.
  // This is deliberately separate from rpc_ready_: a failed setup must wake
  // the waiter too, while admission remains permanently closed.
  mutable std::mutex startup_mtx_;
  std::condition_variable startup_cv_;
  bool startup_finished_ = false;
  bool startup_succeeded_ = false;
  int32_t wait_int_ = 100000 ;
  std::atomic_bool disconnected_{false};
  bool in_applying_logs_ = false ;
  std::atomic<bool> apply_pending_{false};  // Tracks if new work arrived while applying logs
  // UNWIRED, and unconditional -- the two #ifdef RAFT_TEST_CORO arms declared
  // it identically, so the conditional said nothing.
  //
  // This is RaftServer's own member, not Config's: Config parses a real
  // failover flag from YAML (`method: none` -> false, config.cc:544) and
  // exposes get_failover(), but nothing here reads it and nothing assigns this
  // one, so it is a compile-time true and its guards are no-op branches. Left
  // in place deliberately -- deleting it removes the hook for running Raft
  // without elections, and wiring it to Config would CHANGE BEHAVIOUR: a
  // `method: none` deployment would stop starting the election timer, where
  // today it starts regardless. That is a product decision, not a cleanup.
  bool failover_{true} ;

  rusty::sync::atomic::AtomicBool looping_{false};
  rusty::sync::atomic::AtomicBool heartbeat_loop_running_{false};
  rusty::sync::atomic::AtomicBool election_loop_running_{false};
  bool heartbeat_ = true;
  bool heartbeat_setup_ = false;
  uint64_t heartbeat_interval_us_ = HEARTBEAT_INTERVAL;  // Runtime-configurable heartbeat interval (microseconds)
  uint64_t log_retention_window_ = 5000;  // Configurable log retention window (entries to keep after compaction)

  // Cross-thread submissions publish only to this level-triggered gate.  The
  // gate posts a gate-only job to the heartbeat PollThread; IntEvent itself is
  // created, signalled, waited, and cleared exclusively by that owner thread.
  rusty::Arc<ReplicationWakeGate> replication_wake_gate_;

  // @unsafe - Reactor bridge; schedules a gate-only job on the bound owner.
  void RequestReplication();
  // @unsafe - Owner-thread-only wait on the gate's IntEvent.
  bool WaitForReplicationOrHeartbeat(uint64_t timeout_us);
  // @unsafe - Owner-thread-only election delay that shutdown can interrupt.
  bool WaitForElectionTimeoutOrShutdown(uint64_t timeout_us);
  // @unsafe - Stops new wake jobs and releases the gate's PollThread handle.
  void CloseReplicationWakeGate();
  // @unsafe - Caller holds mtx_; performs a non-mutating absolute-slot lookup.
  ballot_t ElectionLastLogTermLocked() const;

	std::function<void(bool)> leader_change_cb_{};

  // ============================================================================
  // PREFERRED REPLICA SYSTEM - Election timeout bias
  // ============================================================================
  // One replica may be designated as the "preferred leader". Voting itself
  // carries no bias: any replica can win any election. The preference only
  // shapes GetElectionTimeout(), so the preferred replica campaigns sooner
  // than its peers and normally wins the startup election.

  siteid_t preferred_leader_site_id_ = INVALID_SITEID;     // Site ID of preferred leader
  uint64_t startup_timestamp_ = 0;                          // When server started (for grace period)

  // The campaign that owns state_.req_voting_; a delayed vote result applies only to
  // this exact term.

  // ============================================================================
  // MEMBERSHIP CONFIGURATION
  // ============================================================================
  // The set of replicas in this partition, initialized from the static
  // partition config in Setup(). Memory-only Raft has no membership change, so
  // this set is fixed for the server lifetime. All quorum calculations use
  // current_config_.size() instead of the static
  // Config::GetConfig()->GetPartitionSize().
  std::set<siteid_t> current_config_;          // Active replica set (site IDs)

  // Reads the dynamically configurable preferred-leader identity.
  //
  // Must be called with mtx_ held. The one caller repo-wide is
  // GetElectionTimeout(), which is itself called only from resetTimer(), which
  // takes mtx_ -- so the inner re-acquisition this used to take was a no-op on
  // the recursive mutex, and removing it states the precondition as a type-
  // adjacent comment rather than re-checking it at runtime. See
  // docs/migration/raft/cpp-refactor-plan.md tranche 4b.
  // @unsafe - Must be called with mtx_ held (caller's responsibility).
  bool AmIPreferredLeader() {
    return raft_server_site_is_preferred_leader(
        site_id_, preferred_leader_site_id_);
  }

  // ============================================================================

  // Timer-only entry retains the reset generation observed at expiry. The
  // first RequestVote state lock revalidates it immediately before term++.
  bool RequestVoteFromElectionTimer(uint64_t expected_generation);
  bool RequestVoteImpl(bool timer_guarded,
                       uint64_t expected_generation);

  // @safe - server setup (threading via @unsafe blocks)
	void Setup();
  bool SetupInternal();
  // @safe - external calls marked @external, core replication loop
	void HeartbeatLoop() ;

  // @unsafe - raw pointer output parameters (reply_term, vote_granted)
  // Memory-only voting: record the vote and reply immediately.
  void doVote(const slotid_t& lst_log_idx,
              const ballot_t& lst_log_term,
              const siteid_t& can_id,
              const ballot_t& can_term,
              ballot_t *reply_term,
              bool_t *vote_granted,
              bool_t vote) {
      // @unsafe
      {
        *vote_granted = vote ;
        *reply_term = state_.current_term_ ;
      }
#ifdef RAFT_LEADER_ELECTION_DEBUG
      siteid_t prev_vote_for = state_.vote_for_;
      Log_info("[RAFT_VOTE] server {} (loc {}) vote={} candidate={} can_term={} cur_term={} prev_vote_for={} is_leader={} lst_idx={} lst_term={}",
               site_id_, loc_id_, vote, can_id, can_term, state_.current_term_, prev_vote_for, state_.is_leader_, lst_log_idx, lst_log_term);
#endif

      if (raft_server_signed_term_is_newer(can_term, state_.current_term_))
      {
          const uint64_t prev_term = state_.current_term_;
          const bool was_leader = state_.is_leader_;
          // A RequestVote proves only that a candidate exists, not that Raft
          // has elected it. Do not keep advertising the previous epoch's
          // leader while processing the higher-term request.
          state_.current_leader_id_ = raft_server_leader_hint_after_transition(
              false, false, site_id_, can_id);
          state_.current_term_ = can_term ;
          // @unsafe
          {
            state_.vote_for_ = INVALID_SITEID;  // Reset vote when advancing to new term
          }

          // A higher term is stable state even when this RequestVote is denied.
          if (was_leader) {
            stepDown();
          } else {
            setIsLeader(false);
          }
          state_.req_voting_ = false;
          state_.election_in_progress_ = false;

          // Publish the newly observed term, never the pre-transition value.
          *reply_term = state_.current_term_;
          LogTermChange("vote request carried newer term", prev_term, state_.current_term_, can_id);
      }

      if(vote)
      {
          setIsLeader(false) ;
          state_.vote_for_ = can_id ;

#ifdef RAFT_LEADER_ELECTION_DEBUG
          Log_info("[RAFT_VOTE] server {} recorded vote_for={} at term={}", site_id_, state_.vote_for_, state_.current_term_);
#endif
          // Reset timeout
          // doVote runs only from OnRequestVote, which holds mtx_.
          resetTimerLocked("granted vote");
      }

  }


  std::thread apply_thread_;
  std::atomic<bool> apply_thread_running_{false};
  // Serializes state-machine application/replay with snapshot installation.
  // Lock order, when more than one is needed:
  // state_machine_apply_mtx_ -> mtx_ -> apply_queue_mtx_.
  // Keep this separate from apply_queue_mtx_: callbacks may be slow, while
  // AppendEntries must retain its short queue-enqueue critical section.
  std::mutex state_machine_apply_mtx_;
  std::mutex apply_queue_mtx_;
  struct QueuedApplyEntry {
    slotid_t index = 0;
    Command command{};
    uint64_t epoch = 0;
  };
  // Guarded by apply_queue_mtx_. A conflicting snapshot increments the epoch
  // so an entry popped before queue invalidation cannot apply afterward.
  uint64_t apply_queue_epoch_ = 0;
  std::deque<QueuedApplyEntry> apply_queue_;

  // Release-published after app_next_ returns. New synchronous client waits
  // use this mirror instead of racing on the legacy state_.execute_index_ field.
  rusty::sync::atomic::AtomicU64 appliedIndexForWait_{0};

  // @unsafe - Caller owns state_machine_apply_mtx_; locks mtx_ before
  // publishing the legacy state_.execute_index_ field and its atomic mirror.
  void PublishAppliedIndex(uint64_t index);
  // @unsafe - CALLER MUST HOLD mtx_
  void PublishAppliedIndexLocked(uint64_t index);

  void StartApplyThread();
  void EnqueueCommittedEntries(slotid_t old_commit, slotid_t new_commit);

  // @unsafe - const char* parameter type requires unsafe context
  // Acquiring entry point. See resetTimerLocked for the body.
  void resetTimer(const char* reason = "unspecified") {
    // @unsafe
    {
      std::lock_guard<std::mutex> lock(mtx_);
      resetTimerLocked(reason);
    }
  }

  // CALLER MUST HOLD mtx_.
  void resetTimerLocked(const char* reason = "unspecified") {
    // @unsafe
    {
      const char* why = reason ? reason : "unspecified";
      auto prev_time = state_.last_heartbeat_time_;
      state_.last_heartbeat_time_ = Time::now(true);
      state_.election_timeout_us_ = GetElectionTimeout();
      if (state_.election_timer_generation_ ==
          std::numeric_limits<uint64_t>::max()) {
        state_.election_timer_generation_ = 1;
      } else {
        ++state_.election_timer_generation_;
      }
      // Log only important timer resets (elections, votes), not routine heartbeats
      if (strcmp(why, "granted vote") == 0 || strcmp(why, "start election timer") == 0) {
        Log_info("[TIMER_RESET] Site {}: reset timer ({}) - prev_hb_time={} new_hb_time={} delta={} timeout={} generation={}",
                 site_id_, why, prev_time, state_.last_heartbeat_time_,
                 state_.last_heartbeat_time_ - prev_time, state_.election_timeout_us_,
                 state_.election_timer_generation_);
      }
    }
  }


  /**
   * Get dynamic election timeout based on preferred replica role and grace period
   *
   * Returns:
   * - Preferred replica: 150-300ms (short timeout to win elections quickly)
   * - Non-preferred during grace period (0-5s after startup): 1-2s (long timeout to allow preferred to win)
   * - Non-preferred after grace period: 500ms-1s (medium timeout to enable failover)
   *
   * This implements startup election bias for preferred replica system.
   */
  // @safe - election timeout calculation (external calls wrapped in @unsafe blocks)
  uint64_t GetElectionTimeout();
 public:
  // @unsafe - Returns the scheduler's non-owning typed communicator.
  RaftCommo* commo() {
    auto* communicator = dynamic_cast<RaftCommo*>(commo_);
    verify(communicator != nullptr);
    return communicator;
  }

#ifdef RAFT_TEST_CORO
  // Test-only inspection surface for the RaftLab harness (testconf.cc,
  // test.cc). It replaces the former friendship grants to RaftTestConfig
  // and RaftLabTest: a nested class may name the enclosing class's private
  // members, so no friendship is required. Each
  // accessor hands back a reference to one private field (read and write
  // through the same function) or forwards one private method. Production
  // code must not use it; the Rust port expresses this as a #[cfg(test)]
  // module.
  struct LabAccess {
    // --- shutdown / apply-gate state the harness synchronizes with ---
    static rusty::sync::atomic::AtomicBool& stop(RaftServer& s) { return s.stop_; }
    static std::mutex& state_machine_apply_mtx(RaftServer& s) { return s.state_machine_apply_mtx_; }

    // --- role / election state inspected by tests ---
    static bool& is_leader(RaftServer& s) { return s.state_.is_leader_; }
    static siteid_t& vote_for(RaftServer& s) { return s.state_.vote_for_; }
    static siteid_t& current_leader_id(RaftServer& s) { return s.state_.current_leader_id_; }
    static bool& req_voting(RaftServer& s) { return s.state_.req_voting_; }
    static bool& election_in_progress(RaftServer& s) { return s.state_.election_in_progress_; }

    // --- snapshot boundary ---
    static slotid_t& snapidx(RaftServer& s) { return s.state_.snapidx_; }
    static ballot_t& snapterm(RaftServer& s) { return s.state_.snapterm_; }
    static std::shared_ptr<janus::raft::SnapshotManager>& snapshot_manager(RaftServer& s) { return s.snapshot_manager_; }

    // --- private methods the harness drives directly ---
    static bool CreateSnapshotLocked(RaftServer& s) { return s.CreateSnapshotLocked(); }
  };
#endif

  int n_prepare_ = 0;
  int n_accept_ = 0;
  int n_commit_ = 0;

  RaftLog raft_log_{RaftLog::new_()};

  // @unsafe - Binds the cross-thread wake gate to HeartbeatLoop's PollThread.
  // Must run before HeartbeatLoop starts (Setup does so).
  void BindReplicationWakeOwner(rusty::Arc<rrr::PollThread> owner);

  // @unsafe - Must be called from a reactor fiber before destroying a live
  // server; signals both runtime loops and waits for their completion flags.
  void PrepareForShutdown();

  // @safe - Acquire-load paired with the final startup Release publication.
  bool IsRpcReady() const {
    return rpc_ready_.load(rusty::sync::atomic::Ordering::Acquire);
  }

  // @safe - Waits for the owner-thread startup job and reports its result.
  bool WaitForStartup();

  // Acquire-load pairs with PublishAppliedIndex after app_next_ completes.
  // @safe - Rusty atomic read.
  uint64_t GetAppliedIndex() const {
    return appliedIndexForWait_.load(
        rusty::sync::atomic::Ordering::Acquire);
  }

  // @safe - election timer setup (threading via @unsafe blocks in implementation)
  void StartElectionTimer() ;
  // @safe - calls Setup
  void EnsureSetup();

  // @unsafe - Locks mtx_ before reading the role published by setIsLeader().
  // CALLER MUST HOLD mtx_. The looping_ check is an atomic, so it needs no
  // lock and stays here: it is the guard against reading members during
  // destruction.
  bool IsLeaderLocked() const {
    if (!looping_.load(rusty::sync::atomic::Ordering::Acquire)) {
      return false;
    }
    return state_.is_leader_ ;
  }

  // Acquiring entry point, for callers that do not already hold mtx_.
  bool IsLeader() {
    if (!looping_.load(rusty::sync::atomic::Ordering::Acquire)) {
      return false;
    }
    std::lock_guard<std::mutex> lock(mtx_);
    return state_.is_leader_ ;
  }
  
  // @safe - leadership state transition (callbacks and logging wrapped in @unsafe blocks)
  void setIsLeader(bool isLeader);

  // @safe - stores callback for later invocation
  void RegisterLeaderChangeCallback(std::function<void(bool)> cb);

  // @safe - external calls marked @external, output pointer writes in @unsafe blocks
  // take janus::Command;
  // shared_ptr<Marshallable> callers auto-convert via Command's
  // implicit ctor.
  RaftStartResult Start(const janus::Command& cmd,
                        uint64_t* index,
                        uint64_t* term,
                        slotid_t slot_id = -1,
                        ballot_t ballot = 1);

  // @unsafe - output pointer writes and mutex operations
  void GetState(bool *is_leader, uint64_t *term) {
    std::lock_guard<std::mutex> lock(mtx_);
    // @unsafe
    {
      *is_leader = IsLeaderLocked();
      *term = state_.current_term_;
    }
  }

  // @safe - returns POD field
  uint64_t GetHeartbeatInterval() const { return heartbeat_interval_us_; }

  // @safe - sets POD field
  void SetHeartbeatInterval(uint64_t micros) { heartbeat_interval_us_ = micros; }

  // @safe - returns POD field
  uint64_t GetLogRetentionWindow() const { return log_retention_window_; }

  // @safe - sets POD field (minimum 1 to avoid division by zero)
  void SetLogRetentionWindow(uint64_t window) {
    log_retention_window_ = raft_server_retention_window_normalize(window);
  }

  // @unsafe - external calls plus output pointer writes and shared_ptr ops
  // take janus::Command;
  // shared_ptr<Marshallable> callers auto-convert via Command's
  // implicit ctor.
  RaftStartResult SetLocalAppend(const janus::Command& cmd,
                                 uint64_t* term,
                                 uint64_t* index,
                                 slotid_t slot_id = -1,
                                 ballot_t ballot = 1) {
    // Must be called with mtx_ held. Both callers -- setIsLeader() and
    // StartImpl() -- take it before reaching here; the re-acquisition this
    // replaces was a no-op on the recursive mutex. Tranche 4b.
    // The pre-append tail, which is what this out-parameter has always
    // reported -- the new entry lands at *index + 1.
    *index = raft_log_.last_index();
    // slot_id and ballot are accepted for signature compatibility with the
    // Paxos-shaped callers; RaftEntry has no field for either, because the
    // three fields that used to receive them here were read nowhere.
    (void)slot_id;
    (void)ballot;
    const uint64_t appended = raft_log_.append(
        RaftEntry::new_(state_.current_term_, cmd));
    verify(appended == *index + 1);

    // @unsafe
    {
      *term = state_.current_term_ ;
    }
    return RaftStartResult::APPENDED;
  }


  // Unwraps RaftLog::get's borrow into a pointer for the C++ callers.
  //
  // Sound because every caller reads a field out of the result before the
  // next statement that could touch the log -- audited one by one, and the
  // reason RaftLog::get can return a borrow instead of a refcounted handle.
  // The Rust side never sees this pointer.
  // @unsafe - borrow flattened to a pointer; caller must hold mtx_
  const RaftEntry* FindRaftInstance(slotid_t id) const {
    const auto found = raft_log_.get(id);
    if (found.is_none()) {
      return nullptr;
    }
    return &found.unwrap();
  }

  RaftServer();
  // @unsafe - thread join and timer cleanup require manual resource management
  ~RaftServer() ;

  // ============================================================================
  // SNAPSHOT SUPPORT PUBLIC API
  // ============================================================================

  /**
   * Set the snapshot manager for this server.
   * Should be called before starting the server.
   * @param manager Shared pointer to SnapshotManager implementation
   */
  // @unsafe - Locks mtx_, moves shared_ptr into member field, and publishes
  // the apply-thread trigger hint.
  void SetSnapshotManager(
      std::shared_ptr<janus::raft::SnapshotManager> manager);

  /**
   * Get the current snapshot manager.
   * @return Shared pointer to SnapshotManager, or nullptr if not set
   */
  // @unsafe - Locks mtx_ and returns a copy of the shared_ptr.
  std::shared_ptr<janus::raft::SnapshotManager> GetSnapshotManager();

  /**
   * Set state machine snapshot callbacks.
   * Called by the application state machine to hook into
   * CreateSnapshot() and OnInstallSnapshot().
   * @param create_cb Returns serialized state machine snapshot data
   * @param prepare_cb Validates and stages serialized state-machine bytes. The
   * returned transaction must leave the live image unchanged until Commit().
   */
  // Returns a unique owner token. Replacing callbacks invalidates the previous
  // owner's token, so its eventual destructor cannot clear the new owner.
  // @unsafe - Locks mtx_ and stores std::function closures.
  uint64_t SetStateMachineSnapshotCallbacks(
      std::function<std::string(uint64_t)> create_cb,
      std::function<std::unique_ptr<PreparedStateMachineSnapshotInstall>(
          const std::string&, uint64_t)> prepare_cb);

  // Clears callbacks only when callback_owner_token still owns them.
  // @unsafe - Locks mtx_ and destroys std::function closures.
  bool ClearStateMachineSnapshotCallbacks(uint64_t callback_owner_token);

  /**
   * Check if a snapshot is available.
   * @return true if a snapshot exists in the snapshot manager
   */
  // @unsafe - Copies the manager under mtx_ before querying it.
  bool HasSnapshot();

  /**
   * Get the last log index included in the most recent snapshot.
   * @return Last included index, or 0 if no snapshot exists
   */
  // @unsafe - Reads snapshot metadata under mtx_.
  uint64_t GetSnapshotIndex();

  /**
   * Get the term of the last log entry included in the most recent snapshot.
   * @return Last included term, or 0 if no snapshot exists
   */
  // @unsafe - Reads snapshot metadata under mtx_.
  uint64_t GetSnapshotTerm();

  /**
   * Compact log entries up to the given index.
   * Removes in-memory entries that are covered by a snapshot.
   * @param up_to_index Remove entries with index <= this value
   * @return Number of entries removed
   */
  // @unsafe - In-memory log compaction under mtx_.
  // @unsafe - CALLER MUST HOLD mtx_ (the *Locked accessors exist so test code
  // and internal callers that already hold the lock do not re-acquire it,
  // which a non-recursive mutex cannot tolerate)
  uint64_t GetSnapshotIndexLocked() const;
  uint64_t GetSnapshotTermLocked() const;
  void SetSnapshotThresholdLocked(uint64_t threshold);
  void SetSnapshotManagerLocked(
      std::shared_ptr<janus::raft::SnapshotManager> manager);
  size_t CompactLog(slotid_t up_to_index);
  // @unsafe - CALLER MUST HOLD mtx_
  size_t CompactLogLocked(slotid_t up_to_index);

  /**
   * Set the snapshot threshold (number of entries between snapshots).
   * @param threshold Number of log entries applied before taking a snapshot
   */
  // @unsafe - Locks mtx_ and publishes the apply-thread trigger hint.
  void SetSnapshotThreshold(uint64_t threshold);

  /**
   * Get the current snapshot threshold.
   * @return Current threshold value
   */
  // @safe - Reads the atomic trigger mirror.
  uint64_t GetSnapshotThreshold() const {
    return snapshot_trigger_threshold_.load(
        rusty::sync::atomic::Ordering::Acquire);
  }

  // ============================================================================

  // @safe - calls doVote which is @safe, output pointer writes in @unsafe blocks
  void OnRequestVote(const slotid_t& lst_log_idx,
                     const ballot_t& lst_log_term,
                     const siteid_t& can_id,
                     const ballot_t& can_term,
                     ballot_t *reply_term,
                     bool_t *vote_granted) ;

  // @safe - external calls marked @external, output pointer writes in @unsafe blocks
  // take janus::Command;
  // shared_ptr<Marshallable> callers auto-convert via Command's
  // implicit ctor.
  void OnAppendEntries(const slotid_t slot_id,
                       const ballot_t ballot,
                       const uint64_t leaderCurrentTerm,
                       const siteid_t leaderSiteId,
                       const uint64_t leaderPrevLogIndex,
                       const uint64_t leaderPrevLogTerm,
                       const uint64_t leaderCommitIndex,
                       const janus::Command& cmd,
                       const uint64_t leaderNextLogTerm, // disabled in batched version (term recorded in the TpcCommitCommand)
                       uint64_t *followerAppendOK,
                       uint64_t *followerCurrentTerm,
                       uint64_t *followerLastLogIndex);

  /**
   * InstallSnapshot RPC Handler - Snapshot Transfer Protocol
   *
   * Receives a full snapshot from the leader when this follower is too far
   * behind to catch up via AppendEntries. Replaces the follower's state machine
   * state, updates snapshot metadata, discards old log entries, and advances
   * state_.commit_index_/state_.execute_index_.
   *
   * @param term - Leader's current term
   * @param leader_id - Leader's site ID
   * @param last_included_index - Last log index included in the snapshot
   * @param last_included_term - Term of the last included log entry
   * @param data - Serialized snapshot data
   * @param term_out - [OUT] Follower's current term (for leader to update itself)
   * @param cb - Callback to invoke when handling complete
   */
  // @unsafe - Modifies log state, snapshot metadata, calls snapshot_manager_
  void OnInstallSnapshot(const uint64_t term,
                         const uint64_t leader_id,
                         const uint64_t last_included_index,
                         const uint64_t last_included_term,
                         const std::string& data,
                         uint64_t* term_out);

  // ============================================================================
  // MEMBERSHIP CONFIGURATION PUBLIC API
  // ============================================================================




  // Gates inbound and outbound test traffic without moving transport state.
  void Disconnect(const bool disconnect = true);

  // @safe - calls Disconnect (wrapped in @unsafe block) and resetTimer
  void Reconnect() {
    // @unsafe
    {
      Disconnect(false);
    }
    // @unsafe
    { resetTimer("reconnect"); }
  }

  // @safe
  bool IsDisconnected();


  // ============================================================================
  // PUBLIC API: Preferred Replica System - Election timeout bias
  // ============================================================================

  /**
   * Set the preferred leader for this Raft group.
   *
   * @param site_id The site ID of the preferred leader (or INVALID_SITEID to disable)
   *
   * Behavior:
   * - All replicas should call this with the same site_id
   * - Standard Raft voting happens (any replica can win any election)
   * - The preference only shortens the preferred replica's election timeout,
   *   so it normally campaigns first and wins
   *
   * Safety: Voting is unbiased, so all Raft safety guarantees are preserved.
   */
  // @unsafe - Log_info plus mutex operations
  void SetPreferredLeader(siteid_t site_id) {
    std::lock_guard<std::mutex> lock(mtx_);

    siteid_t old_preferred = preferred_leader_site_id_;
    preferred_leader_site_id_ = site_id;

    if (old_preferred != site_id) {
      Log_info("[LEADERSHIP-TRANSFER] Site {}: Preferred leader set to {}",
               site_id_, site_id);
    }
  }

  /**
   * Get the last known leader's site_id for client redirection.
   * @return Leader site_id, or INVALID_SITEID if unknown
   */
  // @unsafe - Locks mtx_ before reading role/leader identity.
  siteid_t GetLeaderHint();


  /**
   * Step down as leader.
   *
   * This is the central function for leader step-down. Observing a higher term
   * is the only cause in memory-only Raft; the leader itself never fails. It
   * handles:
   * 1. Logging the step-down event
   * 2. Transitioning to follower state
   * 3. Cancelling any in-flight campaign
   * 4. Resetting the election timer
   */
  // @unsafe - Modifies state, calls setIsLeader
  void stepDown();
};
} // namespace janus
