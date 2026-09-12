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
#include <rusty/box.hpp>
#include <rusty/arc.hpp>
#include <rusty/condvar.hpp>
#include <rusty/num.hpp>
#include <rusty/option.hpp>
#include <rusty/slice.hpp>
#include <rusty/sync/atomic.hpp>
#include <rusty/thread.hpp>
#include <type_traits>
#include <utility>
#include "snapshot_manager.hpp"

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
#define NUM_BATCH_TIMER_RESET  (100)
#define SEC_BATCH_TIMER_RESET  (1)

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

pub const fn raft_server_random_range_needs_swap(minimum: u64,
                                                  maximum: u64) -> bool {
    maximum < minimum
}

pub const fn raft_server_random_range_is_single_point(minimum: u64,
                                                       maximum: u64) -> bool {
    maximum == minimum
}

pub const fn raft_server_random_range_cap(range: u64, maximum: u64) -> u64 {
    if range > maximum {
        maximum
    } else {
        range
    }
}

pub const fn raft_server_election_in_startup_grace_period(now: u64,
                                                           started_at: u64,
                                                           grace_period: u64) -> bool {
    now.wrapping_sub(started_at) < grace_period
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

pub const fn raft_server_append_index_is_acceptable(previous_index: u64,
                                                     last_log_index: u64,
                                                     compacted_prefix_miss: bool) -> bool {
    previous_index <= last_log_index && !compacted_prefix_miss
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

pub const fn raft_server_snapshot_index_is_available(execute_index: u64) -> bool {
    execute_index != 0
}

pub const fn raft_server_snapshot_is_due(snapshot_index: u64,
                                          execute_index: u64,
                                          threshold: u64) -> bool {
    snapshot_index < execute_index &&
        (execute_index - snapshot_index) > threshold
}

pub const fn raft_server_compaction_index_clamp(candidate_index: u64,
                                                 commit_index: u64) -> u64 {
    if candidate_index > commit_index {
        commit_index
    } else {
        candidate_index
    }
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

pub const fn raft_server_leadership_transition_to_leader(new_is_leader: bool,
                                                          previous_is_leader: bool) -> bool {
    new_is_leader && !previous_is_leader
}

pub const fn raft_server_leadership_transition_to_follower(new_is_leader: bool,
                                                            previous_is_leader: bool) -> bool {
    !new_is_leader && previous_is_leader
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
/*RUSTYCPP:GEN-BEGIN id=raft_server.scalar_decisions version=1 rust_sha256=e9548046e1f48ce9cd332b67efe3765859ebf264e278b0c06088a2bd718a5185*/
constexpr uint16_t RAFT_SERVER_INVALID_SITE_ID = static_cast<uint16_t>(65535);
constexpr bool raft_server_log_index_at_or_below(uint64_t index, uint64_t boundary);
constexpr bool raft_server_log_index_above(uint64_t index, uint64_t boundary);
constexpr bool raft_server_site_is_preferred_leader(uint16_t site_id, uint16_t preferred_site_id);
constexpr bool raft_server_election_timeout_has_fired(bool is_leader, uint64_t elapsed, uint64_t timeout);
constexpr bool raft_server_timer_campaign_is_current(bool is_leader, uint64_t observed_generation, uint64_t current_generation, uint64_t elapsed, uint64_t timeout);
constexpr bool raft_server_campaign_can_start(bool is_leader, bool election_in_progress);
constexpr bool raft_server_random_range_needs_swap(uint64_t minimum, uint64_t maximum);
constexpr bool raft_server_random_range_is_single_point(uint64_t minimum, uint64_t maximum);
constexpr uint64_t raft_server_random_range_cap(uint64_t range, uint64_t maximum);
constexpr bool raft_server_election_in_startup_grace_period(uint64_t now, uint64_t started_at, uint64_t grace_period);
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
constexpr bool raft_server_append_index_is_acceptable(uint64_t previous_index, uint64_t last_log_index, bool compacted_prefix_miss);
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
constexpr bool raft_server_read_index_round_can_advance(uint64_t round);
constexpr bool raft_server_read_index_reply_confirms_authority(bool response_available, bool is_leader, uint64_t sent_term, uint64_t response_term, uint64_t current_term, uint64_t sent_round, uint64_t active_round);
constexpr bool raft_server_log_entry_is_current_term(int64_t entry_term, uint64_t current_term);
constexpr bool raft_server_snapshot_index_is_available(uint64_t execute_index);
constexpr bool raft_server_snapshot_is_due(uint64_t snapshot_index, uint64_t execute_index, uint64_t threshold);
constexpr uint64_t raft_server_compaction_index_clamp(uint64_t candidate_index, uint64_t commit_index);
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
constexpr bool raft_server_leadership_transition_to_leader(bool new_is_leader, bool previous_is_leader);
constexpr bool raft_server_leadership_transition_to_follower(bool new_is_leader, bool previous_is_leader);
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
constexpr bool raft_server_random_range_needs_swap(uint64_t minimum, uint64_t maximum) {
    return rusty::detail::deref_if_pointer_like(maximum) < rusty::detail::deref_if_pointer_like(minimum);
}
constexpr bool raft_server_random_range_is_single_point(uint64_t minimum, uint64_t maximum) {
    return rusty::detail::deref_if_pointer_like(maximum) == rusty::detail::deref_if_pointer_like(minimum);
}
constexpr uint64_t raft_server_random_range_cap(uint64_t range, uint64_t maximum) {
    if (rusty::detail::deref_if_pointer_like(range) > rusty::detail::deref_if_pointer_like(maximum)) {
        return std::move(maximum);
    } else {
        return std::move(range);
    }
}
constexpr bool raft_server_election_in_startup_grace_period(uint64_t now, uint64_t started_at, uint64_t grace_period) {
    return rusty::wrapping_sub(now, static_cast<std::remove_cvref_t<decltype(now)>>(std::move(started_at))) < rusty::detail::deref_if_pointer_like(grace_period);
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
constexpr bool raft_server_append_index_is_acceptable(uint64_t previous_index, uint64_t last_log_index, bool compacted_prefix_miss) {
    return (rusty::detail::deref_if_pointer_like(previous_index) <= rusty::detail::deref_if_pointer_like(last_log_index)) && !compacted_prefix_miss;
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
constexpr bool raft_server_read_index_round_can_advance(uint64_t round) {
    return rusty::detail::deref_if_pointer_like(round) != rusty::detail::deref_if_pointer_like(std::numeric_limits<uint64_t>::max());
}
constexpr bool raft_server_read_index_reply_confirms_authority(bool response_available, bool is_leader, uint64_t sent_term, uint64_t response_term, uint64_t current_term, uint64_t sent_round, uint64_t active_round) {
    return (((rusty::detail::deref_if_pointer_like(response_available) && rusty::detail::deref_if_pointer_like(is_leader)) && (rusty::detail::deref_if_pointer_like(sent_term) == rusty::detail::deref_if_pointer_like(current_term))) && (rusty::detail::deref_if_pointer_like(response_term) == rusty::detail::deref_if_pointer_like(sent_term))) && (rusty::detail::deref_if_pointer_like(sent_round) == rusty::detail::deref_if_pointer_like(active_round));
}
constexpr bool raft_server_log_entry_is_current_term(int64_t entry_term, uint64_t current_term) {
    return (static_cast<uint64_t>(entry_term)) == rusty::detail::deref_if_pointer_like(current_term);
}
constexpr bool raft_server_snapshot_index_is_available(uint64_t execute_index) {
    return rusty::detail::deref_if_pointer_like(execute_index) != static_cast<uint64_t>(0);
}
constexpr bool raft_server_snapshot_is_due(uint64_t snapshot_index, uint64_t execute_index, uint64_t threshold) {
    return (rusty::detail::deref_if_pointer_like(snapshot_index) < rusty::detail::deref_if_pointer_like(execute_index)) && (((rusty::detail::deref_if_pointer_like(execute_index) - rusty::detail::deref_if_pointer_like(snapshot_index))) > rusty::detail::deref_if_pointer_like(threshold));
}
constexpr uint64_t raft_server_compaction_index_clamp(uint64_t candidate_index, uint64_t commit_index) {
    if (rusty::detail::deref_if_pointer_like(candidate_index) > rusty::detail::deref_if_pointer_like(commit_index)) {
        return std::move(commit_index);
    } else {
        return std::move(candidate_index);
    }
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
constexpr bool raft_server_leadership_transition_to_leader(bool new_is_leader, bool previous_is_leader) {
    return rusty::detail::deref_if_pointer_like(new_is_leader) && !previous_is_leader;
}
constexpr bool raft_server_leadership_transition_to_follower(bool new_is_leader, bool previous_is_leader) {
    return !new_is_leader && rusty::detail::deref_if_pointer_like(previous_is_leader);
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
// Raft currently compares signed ballot_t values with uint64_t currentTerm.
// These casts make the existing C++ usual-arithmetic-conversion semantics
// explicit, including the historical negative-term edge case.
static_assert(!raft_server_vote_term_is_stale(static_cast<uint64_t>(-1), 0));
static_assert(raft_server_observed_higher_term(static_cast<uint64_t>(-1), 0));
static_assert(!raft_server_signed_term_is_newer(-1, 0));
static_assert(!raft_server_signed_term_is_newer(0, 0));
static_assert(raft_server_signed_term_is_newer(1, 0));
static_assert(raft_server_log_entry_is_current_term(
    -1, static_cast<uint64_t>(-1)));

// @safe - data struct with shared_ptr fields (shared_ptr marked @external)
//
// polymorphic command fields
// (`accepted_cmd_` / `committed_cmd_` / `log_`) migrated from
// `shared_ptr<Marshallable>` to `janus::Command`.  Internal storage
// inside Command remains `shared_ptr<Marshallable>` (boundary calls
// to APIs still taking `shared_ptr<Marshallable>` use
// `cmd.inner_marshallable()`).  Wire format unchanged.  See
// `docs/dev/l10-unblock-plan.md`.
struct RaftData {
  ballot_t max_ballot_seen_ = 0;
  ballot_t max_ballot_accepted_ = 0;
  Command accepted_cmd_{};
  Command committed_cmd_{};

  ballot_t term;
  Command log_{};

	//for retries
	ballot_t prevTerm;
	slotid_t slot_id;
	ballot_t ballot;
};

#ifdef RAFT_TEST_CORO
#define HEARTBEAT_INTERVAL 100000
#else
#define HEARTBEAT_INTERVAL 5000
#endif


// @unsafe - inherits from non-@interface TxLogServer (individual methods are @safe)
class RaftServer : public TxLogServer {
 public:
  // The five site fields and the mutex used to arrive by inheriting
  // TxLogServer's data members. They are declared here now; every body that
  // reads them -- 164 `site_id_`, 20 `partition_id_`, 10 `loc_id_`, 6
  // `app_next_` and 48 `mtx_` acquisitions -- is unchanged. See
  // src/deptran/scheduler.h for why, and cpp-refactor-plan.md gate G4.
  //
  // mtx_ being Raft's own is the point: it is what lets Tranche 5 replace this
  // recursive mutex with a single Mutex<RaftState> without touching Paxos.
  TXLOG_SERVER_SITE_FIELDS()
  std::recursive_mutex mtx_{};
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
  uint64_t snapshot_threshold_ = 10000;  // Entries between snapshots (configurable)
  // Apply-thread trigger mirrors. The state-machine hot path must not race on
  // snapshot_manager_, snapidx_, or snapshot_threshold_; it reads only these
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
  uint64_t snapshot_callback_owner_token_ = 0;
  uint64_t next_snapshot_callback_owner_token_ = 1;

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

  // @unsafe - External snapshot entry point. Acquires the state-machine apply
  // gate before mtx_ so the serialized bytes and executeIndex describe the
  // same applied prefix.
  void CreateSnapshot();

  // @unsafe - Requires state_machine_apply_mtx_ and mtx_ in that order.
  // Split out so the apply trigger and RaftLabTest's LabAccess-driven manager
  // rotation helper can preserve the global lock order without re-locking.
  bool CreateSnapshotLocked();

  // @unsafe - Cheap-trigger slow path. Acquires state_machine_apply_mtx_ then
  // mtx_, rechecks the canonical snapshot state, and snapshots only if due.
  void MaybeCreateSnapshot();

  // ============================================================================

  std::map<siteid_t, uint64_t> match_index_{};
  std::map<siteid_t, uint64_t> next_index_{};
  // Heartbeat quorum proof, guarded by mtx_. HeartbeatLoop stamps every round
  // with heartbeat_round_ and records the newest round that a quorum of the
  // membership configuration confirmed in the current term.
  uint64_t heartbeat_round_ = 0;
  uint64_t read_quorum_confirmed_term_ = 0;
  uint64_t read_quorum_confirmed_round_ = 0;
  // @unsafe - uses raw pointer parameter for thread signaling
  void timer_thread(bool *vote) ;
  rusty::Box<Timer> timer_;  // Owned timer, auto-cleaned on destruction
  // Election timing is one mutex-protected campaign. A reset samples exactly
  // one timeout and advances the generation; the timer must never redraw the
  // random timeout on each poll or start a campaign from an expired snapshot
  // after a concurrent heartbeat reset.
  uint64_t last_heartbeat_time_ = 0;
  uint64_t election_timeout_us_ = 0;
  uint64_t election_timer_generation_ = 0;
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
  siteid_t vote_for_ = INVALID_SITEID ;
  bool init_ = false ;
  bool is_leader_ = false ;
  siteid_t current_leader_id_ = INVALID_SITEID ;  // Last known leader (self if leader, sender of AppendEntries otherwise)
  slotid_t snapidx_ = 0 ;
  ballot_t snapterm_ = 0 ;
  int32_t wait_int_ = 100000 ;
  std::atomic_bool disconnected_{false};
  bool req_voting_ = false ;
  bool in_applying_logs_ = false ;
  std::atomic<bool> apply_pending_{false};  // Tracks if new work arrived while applying logs
#ifdef RAFT_TEST_CORO
  bool failover_{true} ;
#else
  bool failover_{true} ;
#endif
  atomic<int64_t> counter_{0};
  const char *filename = "/db/data.txt";

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

  enum { STOPPED, RUNNING } status_;
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

  // The campaign that owns req_voting_; a delayed vote result applies only to
  // this exact term.
  bool election_in_progress_ = false;
  ballot_t election_term_ = 0;

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

  // @safe - external calls marked @external, mutex/pointer ops in @unsafe blocks
	bool RequestVote() ;
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
        *reply_term = currentTerm ;
      }
#ifdef RAFT_LEADER_ELECTION_DEBUG
      siteid_t prev_vote_for = vote_for_;
      Log_info("[RAFT_VOTE] server {} (loc {}) vote={} candidate={} can_term={} cur_term={} prev_vote_for={} is_leader={} lst_idx={} lst_term={}",
               site_id_, loc_id_, vote, can_id, can_term, currentTerm, prev_vote_for, is_leader_, lst_log_idx, lst_log_term);
#endif

      if (raft_server_signed_term_is_newer(can_term, currentTerm))
      {
          const uint64_t prev_term = currentTerm;
          const bool was_leader = is_leader_;
          // A RequestVote proves only that a candidate exists, not that Raft
          // has elected it. Do not keep advertising the previous epoch's
          // leader while processing the higher-term request.
          current_leader_id_ = raft_server_leader_hint_after_transition(
              false, false, site_id_, can_id);
          currentTerm = can_term ;
          // @unsafe
          {
            vote_for_ = INVALID_SITEID;  // Reset vote when advancing to new term
          }

          // A higher term is stable state even when this RequestVote is denied.
          if (was_leader) {
            stepDown();
          } else {
            setIsLeader(false);
          }
          req_voting_ = false;
          election_in_progress_ = false;

          // Publish the newly observed term, never the pre-transition value.
          *reply_term = currentTerm;
          LogTermChange("vote request carried newer term", prev_term, currentTerm, can_id);
      }

      if(vote)
      {
          setIsLeader(false) ;
          vote_for_ = can_id ;

#ifdef RAFT_LEADER_ELECTION_DEBUG
          Log_info("[RAFT_VOTE] server {} recorded vote_for={} at term={}", site_id_, vote_for_, currentTerm);
#endif
          // Reset timeout
          resetTimer("granted vote");
      }

      n_vote_++ ;
  }

  // @safe - shared_ptr/callback operations wrapped in @unsafe blocks in implementation
  void applyLogs();

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
  // use this mirror instead of racing on the legacy executeIndex field.
  rusty::sync::atomic::AtomicU64 appliedIndexForWait_{0};

  // @unsafe - Caller owns state_machine_apply_mtx_; locks mtx_ before
  // publishing the legacy executeIndex field and its atomic mirror.
  void PublishAppliedIndex(uint64_t index);

  void StartApplyThread();
  void EnqueueCommittedEntries(slotid_t old_commit, slotid_t new_commit);

  // @unsafe - timer and atomic operations include atomics/mutexes
  void resetTimerBatch()
  {
    // Log_info("!!!!!!! if (!failover_)");
    if (!failover_) return ;
    auto cur_count = counter_++;
    if (cur_count > NUM_BATCH_TIMER_RESET ) {
      // @unsafe
      {
      if (timer_->elapsed() > SEC_BATCH_TIMER_RESET) {
        resetTimer("batch timer adjustment");
      }
      }
      counter_.store(0);
    }
  }
  // @unsafe - const char* parameter type requires unsafe context
  void resetTimer(const char* reason = "unspecified") {
    // @unsafe
    {
      std::lock_guard<std::recursive_mutex> lock(mtx_);
      const char* why = reason ? reason : "unspecified";
      auto prev_time = last_heartbeat_time_;
      last_heartbeat_time_ = Time::now(true);
      election_timeout_us_ = GetElectionTimeout();
      if (election_timer_generation_ ==
          std::numeric_limits<uint64_t>::max()) {
        election_timer_generation_ = 1;
      } else {
        ++election_timer_generation_;
      }
      // Log only important timer resets (elections, votes), not routine heartbeats
      if (strcmp(why, "granted vote") == 0 || strcmp(why, "start election timer") == 0) {
        Log_info("[TIMER_RESET] Site {}: reset timer ({}) - prev_hb_time={} new_hb_time={} delta={} timeout={} generation={}",
                 site_id_, why, prev_time, last_heartbeat_time_,
                 last_heartbeat_time_ - prev_time, election_timeout_us_,
                 election_timer_generation_);
      }
    }
    if (failover_) {
      timer_->start() ;
    }
  }

  // @safe - random number generation (external call wrapped in @unsafe block)
  double randDuration()
  {
    // election timeout between 0.4 and 0.7 seconds
    // @unsafe { RandomGenerator is external }
    return RandomGenerator::rand_double(0.4, 0.7) ;
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
    static bool& is_leader(RaftServer& s) { return s.is_leader_; }
    static siteid_t& vote_for(RaftServer& s) { return s.vote_for_; }
    static siteid_t& current_leader_id(RaftServer& s) { return s.current_leader_id_; }
    static bool& req_voting(RaftServer& s) { return s.req_voting_; }
    static bool& election_in_progress(RaftServer& s) { return s.election_in_progress_; }

    // --- snapshot boundary ---
    static slotid_t& snapidx(RaftServer& s) { return s.snapidx_; }
    static ballot_t& snapterm(RaftServer& s) { return s.snapterm_; }
    static std::shared_ptr<janus::raft::SnapshotManager>& snapshot_manager(RaftServer& s) { return s.snapshot_manager_; }

    // --- private methods the harness drives directly ---
    static bool CreateSnapshotLocked(RaftServer& s) { return s.CreateSnapshotLocked(); }
  };
#endif

  slotid_t min_active_slot_ = 1; // anything before (lt) this slot is freed
  slotid_t max_executed_slot_ = 0;
  slotid_t max_committed_slot_ = 0;
  map<slotid_t, shared_ptr<RaftData>> logs_{};
  int n_vote_ = 0;
  int n_prepare_ = 0;
  int n_accept_ = 0;
  int n_commit_ = 0;

  /* NOTE: I think I should move these to the RaftData class */
  /* TODO: talk to Shuai about it */
  uint64_t lastLogIndex = 0;
  uint64_t currentTerm = 0;
  uint64_t commitIndex = 0;
  uint64_t executeIndex = 0;
  map<slotid_t, shared_ptr<RaftData>> raft_logs_{};
//  vector<shared_ptr<RaftData>> raft_logs_{};

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
  bool IsLeader() {
    // Defensive check: if we're shutting down (looping_=false),
    // return false to prevent accessing member variables during destruction
    if (!looping_.load(rusty::sync::atomic::Ordering::Acquire)) {
      return false;
    }
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    return is_leader_ ;
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
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    // @unsafe
    {
      *is_leader = IsLeader();
      *term = currentTerm;
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
    // @unsafe
    {
      *index = lastLogIndex ;
    }
    lastLogIndex += 1;
    auto instance = GetRaftInstance(lastLogIndex);
    instance->log_ = cmd;
		instance->prevTerm = currentTerm;
    instance->term = currentTerm;
		instance->slot_id = slot_id;
		instance->ballot = ballot;

    // @unsafe
    {
      *term = currentTerm ;
    }
    return RaftStartResult::APPENDED;
  }

  // @unsafe - map access and shared_ptr mutation
  shared_ptr<RaftData> GetInstance(slotid_t id) {
    verify(id >= min_active_slot_ || lastLogIndex == 0);
    auto& sp_instance = logs_[id];
    if(!sp_instance)
      sp_instance = std::make_shared<RaftData>();
    return sp_instance;
  }

 /* shared_ptr<RaftData> GetRaftInstance(slotid_t id) {
    if ( id <= raft_logs_.size() )
    {
        return raft_logs_[id-1] ;
    }
    auto sp_instance = std::make_shared<RaftData>();
    raft_logs_.push_back(sp_instance) ;
    return sp_instance;
  }*/

  // @unsafe - map access and shared_ptr mutation
   shared_ptr<RaftData> GetRaftInstance(slotid_t id) {
    if (id < min_active_slot_ && id != 0) {
      Log_info("[RAFT_LOG] expanding min_active_slot_ from {} to {}", min_active_slot_, id);
      min_active_slot_ = id;
    }
    auto& sp_instance = raft_logs_[id];
    if(!sp_instance)
      sp_instance = std::make_shared<RaftData>();
    return sp_instance;
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
  size_t CompactLog(slotid_t up_to_index);

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
   * commitIndex/executeIndex.
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

  /**
   * Get the current quorum size based on current_config_.
   * @return Majority size: current_config_.size() / 2 + 1
   */
  // @safe - Read-only computation on member field
  size_t GetQuorumSize() const;

  /**
   * Get the current membership configuration.
   * @return Reference to the active replica set
   */
  // @safe - Read-only accessor
  // @lifetime: (&'a) -> &'a
  const std::set<siteid_t>& GetCurrentConfig() const;

  // Returns a membership copy under the Raft mutex for cross-component
  // recovery quorum construction.
  std::set<siteid_t> GetCurrentConfigSnapshot();

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

  // @safe - external calls marked @external
  void removeCmd(slotid_t slot);

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
    std::lock_guard<std::recursive_mutex> lock(mtx_);

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
   * Get the last log index.
   * @return lastLogIndex value
   */
  // @safe - Read-only accessor
  uint64_t GetLastLogIndex() const {
    return lastLogIndex;
  }

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
