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
#include "log_storage.hpp"
#include "recovery_manager.hpp"
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
class ReplicatedDB;
class ReplicationWakeGate;
class InstallSnapshotCallbackGate;

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

/**
 * StepDownReason - Why the leader is stepping down
 *
 * Used by stepDown() to determine what action to take:
 * - UnsecuredFailure: Lost speculative quorum while unsecured leader.
 *   All current-term entries are suspect, clients should be notified.
 * - SecuredFailure: Lost quorum but was secured leader.
 *   Only unsecured entries (specCommitIndex, securedLogIndex] are suspect.
 * - HigherTerm: Saw higher term from another server.
 *   Entries may still be valid, no automatic rollback notification.
 */
#if RUSTYCPP_RUST
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Eq, PartialEq))]
#[repr(i32)]
pub enum StepDownReason {
    UnsecuredFailure = 0,
    SecuredFailure = 1,
    HigherTerm = 2,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=raft_server.step_down_reason version=1 rust_sha256=7dbfeb0d9b13f74566cf44845511c90676df477ef444f17563468b22c83508ad*/
enum class StepDownReason : int32_t;
constexpr StepDownReason StepDownReason_UnsecuredFailure();
constexpr StepDownReason StepDownReason_SecuredFailure();
constexpr StepDownReason StepDownReason_HigherTerm();

enum class StepDownReason : int32_t {
    UnsecuredFailure = 0,
    SecuredFailure = 1,
    HigherTerm = 2
};
inline constexpr StepDownReason StepDownReason_UnsecuredFailure() { return StepDownReason::UnsecuredFailure; }
inline constexpr StepDownReason StepDownReason_SecuredFailure() { return StepDownReason::SecuredFailure; }
inline constexpr StepDownReason StepDownReason_HigherTerm() { return StepDownReason::HigherTerm; }
/*RUSTYCPP:GEN-END id=raft_server.step_down_reason*/

static_assert(std::is_same_v<int, int32_t>);
static_assert(std::is_same_v<std::underlying_type_t<StepDownReason>, int>);
static_assert(std::is_trivially_copyable_v<StepDownReason>);
static_assert(sizeof(StepDownReason) == sizeof(int32_t));
static_assert(alignof(StepDownReason) == alignof(int32_t));
static_assert(static_cast<int32_t>(StepDownReason::UnsecuredFailure) == 0);
static_assert(static_cast<int32_t>(StepDownReason::SecuredFailure) == 1);
static_assert(static_cast<int32_t>(StepDownReason::HigherTerm) == 2);
static_assert(StepDownReason{} == StepDownReason::UnsecuredFailure);

/**
 * CommitStatus - Notification status for client callbacks
 *
 * Used by client callback infrastructure to notify clients of entry status:
 * - SPECULATIVE: Entry reached memory quorum, likely to commit
 * - DURABLE: Entry reached disk quorum with secured leader, guaranteed
 * - ROLLEDBACK: Entry will not commit (leader stepped down gracefully)
 */
#if RUSTYCPP_RUST
#[allow(non_camel_case_types)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Eq, PartialEq))]
#[repr(i32)]
pub enum CommitStatus {
    SPECULATIVE = 0,
    DURABLE = 1,
    ROLLEDBACK = 2,
}

// Submission admission cannot be represented by a bool once a failed local
// fsync may have left the command durable.  Keep this separate from
// CommitStatus: no callback has been registered at this boundary yet.
#[allow(non_camel_case_types)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Eq, PartialEq))]
#[repr(i32)]
pub enum RaftStartResult {
    REJECTED = 0,
    APPENDED = 1,
    INDETERMINATE = 2,
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
/*RUSTYCPP:GEN-BEGIN id=raft_server.commit_status version=1 rust_sha256=022633501d8d075bef2037569600f909780cbe81135560df733ab554dcf68265*/
enum class CommitStatus : int32_t;
constexpr CommitStatus CommitStatus_SPECULATIVE();
constexpr CommitStatus CommitStatus_DURABLE();
constexpr CommitStatus CommitStatus_ROLLEDBACK();
enum class RaftStartResult : int32_t;
constexpr RaftStartResult RaftStartResult_REJECTED();
constexpr RaftStartResult RaftStartResult_APPENDED();
constexpr RaftStartResult RaftStartResult_INDETERMINATE();
enum class ElectionCompletionAction : int32_t;
constexpr ElectionCompletionAction ElectionCompletionAction_IGNORE_STALE();
constexpr ElectionCompletionAction ElectionCompletionAction_APPLY_CURRENT();
constexpr ElectionCompletionAction ElectionCompletionAction_ADVANCE_HIGHER_TERM();

enum class CommitStatus : int32_t {
    SPECULATIVE = 0,
    DURABLE = 1,
    ROLLEDBACK = 2
};
inline constexpr CommitStatus CommitStatus_SPECULATIVE() { return CommitStatus::SPECULATIVE; }
inline constexpr CommitStatus CommitStatus_DURABLE() { return CommitStatus::DURABLE; }
inline constexpr CommitStatus CommitStatus_ROLLEDBACK() { return CommitStatus::ROLLEDBACK; }

enum class RaftStartResult : int32_t {
    REJECTED = 0,
    APPENDED = 1,
    INDETERMINATE = 2
};
inline constexpr RaftStartResult RaftStartResult_REJECTED() { return RaftStartResult::REJECTED; }
inline constexpr RaftStartResult RaftStartResult_APPENDED() { return RaftStartResult::APPENDED; }
inline constexpr RaftStartResult RaftStartResult_INDETERMINATE() { return RaftStartResult::INDETERMINATE; }

enum class ElectionCompletionAction : int32_t {
    IGNORE_STALE = 0,
    APPLY_CURRENT = 1,
    ADVANCE_HIGHER_TERM = 2
};
inline constexpr ElectionCompletionAction ElectionCompletionAction_IGNORE_STALE() { return ElectionCompletionAction::IGNORE_STALE; }
inline constexpr ElectionCompletionAction ElectionCompletionAction_APPLY_CURRENT() { return ElectionCompletionAction::APPLY_CURRENT; }
inline constexpr ElectionCompletionAction ElectionCompletionAction_ADVANCE_HIGHER_TERM() { return ElectionCompletionAction::ADVANCE_HIGHER_TERM; }
/*RUSTYCPP:GEN-END id=raft_server.commit_status*/

static_assert(std::is_same_v<std::underlying_type_t<CommitStatus>, int>);
static_assert(std::is_trivially_copyable_v<CommitStatus>);
static_assert(sizeof(CommitStatus) == sizeof(int32_t));
static_assert(alignof(CommitStatus) == alignof(int32_t));
static_assert(static_cast<int32_t>(CommitStatus::SPECULATIVE) == 0);
static_assert(static_cast<int32_t>(CommitStatus::DURABLE) == 1);
static_assert(static_cast<int32_t>(CommitStatus::ROLLEDBACK) == 2);
static_assert(CommitStatus{} == CommitStatus::SPECULATIVE);

static_assert(std::is_same_v<std::underlying_type_t<RaftStartResult>, int>);
static_assert(std::is_trivially_copyable_v<RaftStartResult>);
static_assert(sizeof(RaftStartResult) == sizeof(int32_t));
static_assert(alignof(RaftStartResult) == alignof(int32_t));
static_assert(static_cast<int32_t>(RaftStartResult::REJECTED) == 0);
static_assert(static_cast<int32_t>(RaftStartResult::APPENDED) == 1);
static_assert(static_cast<int32_t>(RaftStartResult::INDETERMINATE) == 2);
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
pub const fn raft_server_log_index_at_or_below(index: u64, boundary: u64) -> bool {
    index <= boundary
}

pub const fn raft_server_log_index_above(index: u64, boundary: u64) -> bool {
    index > boundary
}

pub const fn raft_server_site_is_preferred_leader(site_id: u16,
                                                   preferred_site_id: u16,
                                                   invalid_site_id: u16) -> bool {
    preferred_site_id != invalid_site_id && site_id == preferred_site_id
}

pub const fn raft_server_leadership_monitor_should_start(is_preferred: bool,
                                                          is_leader: bool,
                                                          looping: bool) -> bool {
    !is_preferred && is_leader && looping
}

pub const fn raft_server_preferred_replica_is_caught_up(preferred_match_index: u64,
                                                         commit_index: u64) -> bool {
    preferred_match_index >= commit_index
}

pub const fn raft_server_local_commit_has_caught_up(local_commit_index: u64,
                                                     leader_commit_index: u64) -> bool {
    local_commit_index >= leader_commit_index
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

pub const fn raft_server_leadership_stable_window_elapsed(elapsed: u64,
                                                           minimum: u64) -> bool {
    elapsed >= minimum
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

pub const fn raft_server_effective_election_timeout(
    randomized_timeout: u64,
    randomized_minimum: u64,
    heartbeat_interval: u64,
    storage_configured: bool,
) -> u64 {
    if storage_configured {
        let maximum_floor_input = u64::MAX / 20;
        let persistence_floor = if heartbeat_interval > maximum_floor_input {
            u64::MAX
        } else {
            heartbeat_interval * 20
        };
        let persistence_guard = if persistence_floor > randomized_minimum {
            persistence_floor - randomized_minimum
        } else {
            0
        };
        if randomized_timeout > u64::MAX - persistence_guard {
            u64::MAX
        } else {
            randomized_timeout + persistence_guard
        }
    } else {
        randomized_timeout
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
                                                           candidate_id: u16,
                                                           invalid_site_id: u16) -> bool {
    candidate_term == current_term &&
        voted_for != invalid_site_id &&
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
    storage_compaction_proves_suffix: bool,
    live_snapshot_proves_suffix: bool) -> bool {
    has_suffix &&
        ((has_boundary && boundary_matches) ||
         (!has_boundary &&
          (storage_compaction_proves_suffix || live_snapshot_proves_suffix)))
}

pub const fn raft_server_snapshot_recovery_has_unproven_gap(
    has_suffix: bool,
    has_boundary: bool,
    storage_compaction_proves_suffix: bool,
    live_snapshot_proves_suffix: bool) -> bool {
    has_suffix && !has_boundary &&
        !storage_compaction_proves_suffix && !live_snapshot_proves_suffix
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

pub const fn raft_server_read_index_local_state_allows(is_leader: bool,
                                                        disconnected: bool) -> bool {
    is_leader && !disconnected
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

pub const fn raft_server_read_index_quorum_is_fresh(request_term: u64,
                                                     baseline_round: u64,
                                                     confirmed_term: u64,
                                                     confirmed_round: u64) -> bool {
    confirmed_term == request_term && confirmed_round > baseline_round
}

pub const fn raft_server_read_index_has_current_term_commit(commit_index: u64,
                                                             commit_term: u64,
                                                             current_term: u64) -> bool {
    commit_index != 0 && commit_term == current_term
}

pub const fn raft_server_read_index_deadline_expired(timeout_us: u64,
                                                      elapsed_us: u64) -> bool {
    timeout_us != 0 && elapsed_us >= timeout_us
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

pub const fn raft_server_ack_is_memory(ack_type: u64) -> bool {
    ack_type == 0
}

// @safe - pure packed callback-gate admission decision.
pub const fn raft_server_callback_gate_is_open(state: u64,
                                                drain_bit: u64) -> bool {
    (state & drain_bit) == 0
}

// @safe - pure packed callback-gate borrower count extraction.
pub const fn raft_server_callback_gate_count(state: u64,
                                              count_mask: u64) -> u64 {
    state & count_mask
}

// @safe - pure persistence-mode classification.
pub const fn raft_server_persistence_can_report_durable(has_durable_storage: bool) -> bool {
    has_durable_storage
}

// @safe - pure persistence-mode classification.
pub const fn raft_server_sync_reply_is_durable(has_durable_storage: bool,
                                                 async_persistence: bool) -> bool {
    has_durable_storage && !async_persistence
}

// @safe - pure persistence-result classification.  A synchronous transport
// reply is durable only when this exact call crossed its write+sync boundary.
pub const fn raft_server_follower_append_ack_type(has_durable_storage: bool,
                                                   async_persistence: bool,
                                                   persistence_succeeded: bool) -> u64 {
    if persistence_succeeded &&
        raft_server_sync_reply_is_durable(has_durable_storage, async_persistence) {
        1
    } else {
        0
    }
}

// @safe - pure write-boundary result aggregation.
pub const fn raft_server_durable_write_succeeded(storage_ready: bool,
                                                  writes_succeeded: bool,
                                                  sync_succeeded: bool) -> bool {
    storage_ready && writes_succeeded && sync_succeeded
}

// @safe - pure async persistence admission decision.
pub const fn raft_server_async_persistence_should_queue(
    async_persistence: bool,
    storage_configured: bool,
    has_entries: bool,
) -> bool {
    async_persistence && storage_configured && has_entries
}

// @safe - pure FIFO ticket readiness decision.
pub const fn raft_server_persistence_ticket_is_ready(serving_ticket: u64,
                                                      worker_ticket: u64) -> bool {
    serving_ticket == worker_ticket
}

pub const fn raft_server_persisted_reply_context_is_current(
    stopping: bool,
    is_leader: bool,
    current_term: u64,
    accepted_term: u64,
    current_leader: u16,
    accepted_leader: u16,
) -> bool {
    !stopping &&
        !is_leader &&
        current_term == accepted_term &&
        current_leader == accepted_leader
}

// @safe - pure wire acknowledgement classification.
pub const fn raft_server_ack_is_durable(ack_type: u64) -> bool {
    ack_type == 1
}

// @safe - pure election/message-ordering decision.
pub const fn raft_server_can_buffer_early_durable_vote(
    has_durable_storage: bool,
    async_persistence: bool,
    is_leader: bool,
    election_in_progress: bool,
    vote_term: i64,
    election_term: i64,
) -> bool {
    has_durable_storage &&
        async_persistence &&
        !is_leader &&
        election_in_progress &&
        vote_term == election_term
}

pub const fn raft_server_should_become_secured(already_secured: bool,
                                                durable_vote_count: usize,
                                                quorum: usize) -> bool {
    !already_secured && durable_vote_count >= quorum
}

pub const fn raft_server_unsecured_leader_needs_quorum_check(already_secured: bool,
                                                              is_leader: bool) -> bool {
    !already_secured && is_leader
}

pub const fn raft_server_commit_status_is_durable(status: CommitStatus) -> bool {
    (status as i32) == (CommitStatus::DURABLE as i32)
}

pub const fn raft_server_start_was_rejected(result: RaftStartResult) -> bool {
    (result as i32) == (RaftStartResult::REJECTED as i32)
}

pub const fn raft_server_start_was_appended(result: RaftStartResult) -> bool {
    (result as i32) == (RaftStartResult::APPENDED as i32)
}

pub const fn raft_server_start_is_indeterminate(result: RaftStartResult) -> bool {
    (result as i32) == (RaftStartResult::INDETERMINATE as i32)
}

// A leadership or term change does not resolve an old entry. The exact slot
// becomes terminal only once it is inside the committed prefix.
pub const fn raft_server_submission_is_committed(commit_index: u64,
                                                  submitted_index: u64,
                                                  entry_matches: bool) -> bool {
    commit_index >= submitted_index && entry_matches
}

pub const fn raft_server_submission_is_superseded(commit_index: u64,
                                                   submitted_index: u64,
                                                   entry_known_conflict: bool,
                                                   committed_newer_prefix: bool) -> bool {
    entry_known_conflict &&
        (commit_index >= submitted_index || committed_newer_prefix)
}

// An accepted snapshot commits every slot through its boundary, but a local
// entry match proves inclusion only when the snapshot boundary proves the two
// prefixes identical. A divergent snapshot carries no per-entry identities
// below its boundary, so those otherwise-unresolved slots are indeterminate.
pub const fn raft_server_snapshot_resolves_submission(snapshot_index: u64,
                                                       submitted_index: u64) -> bool {
    submitted_index <= snapshot_index
}

pub const fn raft_server_snapshot_submission_is_committed(
    snapshot_index: u64,
    snapshot_term: u64,
    submitted_index: u64,
    submitted_term: u64,
    local_entry_matches: bool,
    local_commit_crossed: bool,
    snapshot_prefix_matches: bool,
) -> bool {
    raft_server_snapshot_resolves_submission(snapshot_index, submitted_index) &&
        ((local_commit_crossed && local_entry_matches) ||
         (snapshot_prefix_matches && local_entry_matches) ||
         (submitted_index == snapshot_index && submitted_term == snapshot_term))
}

pub const fn raft_server_snapshot_submission_is_superseded(
    snapshot_index: u64,
    snapshot_term: u64,
    submitted_index: u64,
    submitted_term: u64,
    local_entry_known_conflict: bool,
    local_commit_crossed: bool,
    snapshot_prefix_matches: bool,
) -> bool {
    raft_server_snapshot_resolves_submission(snapshot_index, submitted_index) &&
        ((local_commit_crossed && local_entry_known_conflict) ||
         (snapshot_prefix_matches && local_entry_known_conflict) ||
         (submitted_index == snapshot_index && submitted_term != snapshot_term))
}

pub const fn raft_server_snapshot_submission_is_indeterminate(
    snapshot_index: u64,
    submitted_index: u64,
    committed: bool,
    superseded: bool,
) -> bool {
    raft_server_snapshot_resolves_submission(snapshot_index, submitted_index) &&
        !committed && !superseded
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
                                                       known_leader_id: u16,
                                                       invalid_site_id: u16) -> u16 {
    if is_leader {
        self_id
    } else if has_known_leader {
        known_leader_id
    } else {
        invalid_site_id
    }
}

// Raft identifies replicas globally, but the client-routing View wire format
// identifies a replica by its locale within one partition. Keep the conversion
// decision in the Rust DSL; C++ supplies the validated remote lookup result.
pub const fn raft_server_view_leader_locale(leader_site: u16,
                                             self_site: u16,
                                             self_locale: i32,
                                             mapped_locale: i32,
                                             invalid_site_id: u16) -> i32 {
    if leader_site == invalid_site_id {
        -1
    } else if leader_site == self_site {
        self_locale
    } else {
        mapped_locale
    }
}

pub const fn raft_server_recovery_leader_site(leader_locale: i32,
                                               self_locale: i32,
                                               self_site: u16,
                                               mapped_site: u16,
                                               invalid_site_id: u16) -> u16 {
    if leader_locale < 0 {
        invalid_site_id
    } else if leader_locale == self_locale {
        self_site
    } else {
        mapped_site
    }
}

// Jetpack recovery may describe a leader, but only a Raft RPC may advance and
// durably publish currentTerm. Accept recovery routing for the current term.
pub const fn raft_server_recovery_view_matches_term(incoming_view_id: u32,
                                                     local_view_id: u32) -> bool {
    incoming_view_id == local_view_id
}

pub const fn raft_server_recovery_view_shape_is_valid(
    incoming_partition: u32,
    expected_partition: u32,
    incoming_replicas: i32,
    expected_replicas: i32,
    leader_count: u64,
    allow_empty: bool,
) -> bool {
    incoming_partition == expected_partition &&
        ((allow_empty && incoming_replicas == 0 && leader_count == 0) ||
         (incoming_replicas > 0 &&
          (allow_empty || incoming_replicas == expected_replicas) &&
          leader_count == 1))
}

pub const fn raft_server_recovery_view_matches_role(term_matches: bool,
                                                     local_is_leader: bool,
                                                     view_leader_is_self: bool,
                                                     has_known_leader: bool,
                                                     known_leader_matches_view: bool) -> bool {
    term_matches &&
        ((local_is_leader && view_leader_is_self) ||
         (!local_is_leader && !view_leader_is_self &&
          (!has_known_leader || known_leader_matches_view)))
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

pub const fn raft_server_term_advance_is_durable(
    has_configured_storage: bool,
    persistence_succeeded: bool,
) -> bool {
    !has_configured_storage || persistence_succeeded
}
#endif
/*RUSTYCPP:GEN-BEGIN id=raft_server.scalar_decisions version=1 rust_sha256=92e6d0e5bd8087f1893a51cca0279c2de5d7e8de14b256e8dc2d02de9a7716fb*/
constexpr bool raft_server_log_index_at_or_below(uint64_t index, uint64_t boundary);
constexpr bool raft_server_log_index_above(uint64_t index, uint64_t boundary);
constexpr bool raft_server_site_is_preferred_leader(uint16_t site_id, uint16_t preferred_site_id, uint16_t invalid_site_id);
constexpr bool raft_server_leadership_monitor_should_start(bool is_preferred, bool is_leader, bool looping);
constexpr bool raft_server_preferred_replica_is_caught_up(uint64_t preferred_match_index, uint64_t commit_index);
constexpr bool raft_server_local_commit_has_caught_up(uint64_t local_commit_index, uint64_t leader_commit_index);
constexpr bool raft_server_election_timeout_has_fired(bool is_leader, uint64_t elapsed, uint64_t timeout);
constexpr bool raft_server_timer_campaign_is_current(bool is_leader, uint64_t observed_generation, uint64_t current_generation, uint64_t elapsed, uint64_t timeout);
constexpr bool raft_server_campaign_can_start(bool is_leader, bool election_in_progress);
constexpr bool raft_server_leadership_stable_window_elapsed(uint64_t elapsed, uint64_t minimum);
constexpr bool raft_server_random_range_needs_swap(uint64_t minimum, uint64_t maximum);
constexpr bool raft_server_random_range_is_single_point(uint64_t minimum, uint64_t maximum);
constexpr uint64_t raft_server_random_range_cap(uint64_t range, uint64_t maximum);
constexpr uint64_t raft_server_effective_election_timeout(uint64_t randomized_timeout, uint64_t randomized_minimum, uint64_t heartbeat_interval, bool storage_configured);
constexpr bool raft_server_election_in_startup_grace_period(uint64_t now, uint64_t started_at, uint64_t grace_period);
constexpr bool raft_server_vote_term_is_stale(uint64_t candidate_term, uint64_t current_term);
constexpr bool raft_server_vote_is_already_granted_to_other(uint64_t candidate_term, uint64_t current_term, uint16_t voted_for, uint16_t candidate_id, uint16_t invalid_site_id);
constexpr bool raft_server_vote_is_idempotent(uint64_t candidate_term, uint64_t current_term, uint16_t voted_for, uint16_t candidate_id);
constexpr bool raft_server_candidate_log_is_at_least(int64_t candidate_term, int64_t current_term, uint64_t candidate_index, uint64_t current_index);
constexpr bool raft_server_election_last_log_uses_snapshot(uint64_t last_log_index, uint64_t snapshot_index);
constexpr bool raft_server_install_snapshot_reply_is_available(uint64_t follower_term);
constexpr bool raft_server_snapshot_is_stale(uint64_t last_included_index, uint64_t local_progress_index);
constexpr bool raft_server_snapshot_boundary_matches(bool has_entry, uint64_t local_term, uint64_t snapshot_term);
constexpr bool raft_server_snapshot_term_is_valid(uint64_t snapshot_term, uint64_t leader_term);
constexpr bool raft_server_snapshot_recovery_retains_suffix(bool has_suffix, bool has_boundary, bool boundary_matches, bool storage_compaction_proves_suffix, bool live_snapshot_proves_suffix);
constexpr bool raft_server_snapshot_recovery_has_unproven_gap(bool has_suffix, bool has_boundary, bool storage_compaction_proves_suffix, bool live_snapshot_proves_suffix);
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
constexpr bool raft_server_read_index_local_state_allows(bool is_leader, bool disconnected);
constexpr bool raft_server_read_index_round_can_advance(uint64_t round);
constexpr bool raft_server_read_index_reply_confirms_authority(bool response_available, bool is_leader, uint64_t sent_term, uint64_t response_term, uint64_t current_term, uint64_t sent_round, uint64_t active_round);
constexpr bool raft_server_read_index_quorum_is_fresh(uint64_t request_term, uint64_t baseline_round, uint64_t confirmed_term, uint64_t confirmed_round);
constexpr bool raft_server_read_index_has_current_term_commit(uint64_t commit_index, uint64_t commit_term, uint64_t current_term);
constexpr bool raft_server_read_index_deadline_expired(uint64_t timeout_us, uint64_t elapsed_us);
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
constexpr bool raft_server_ack_is_memory(uint64_t ack_type);
constexpr bool raft_server_callback_gate_is_open(uint64_t state, uint64_t drain_bit);
constexpr uint64_t raft_server_callback_gate_count(uint64_t state, uint64_t count_mask);
constexpr bool raft_server_persistence_can_report_durable(bool has_durable_storage);
constexpr bool raft_server_sync_reply_is_durable(bool has_durable_storage, bool async_persistence);
constexpr uint64_t raft_server_follower_append_ack_type(bool has_durable_storage, bool async_persistence, bool persistence_succeeded);
constexpr bool raft_server_durable_write_succeeded(bool storage_ready, bool writes_succeeded, bool sync_succeeded);
constexpr bool raft_server_async_persistence_should_queue(bool async_persistence, bool storage_configured, bool has_entries);
constexpr bool raft_server_persistence_ticket_is_ready(uint64_t serving_ticket, uint64_t worker_ticket);
constexpr bool raft_server_persisted_reply_context_is_current(bool stopping, bool is_leader, uint64_t current_term, uint64_t accepted_term, uint16_t current_leader, uint16_t accepted_leader);
constexpr bool raft_server_ack_is_durable(uint64_t ack_type);
constexpr bool raft_server_can_buffer_early_durable_vote(bool has_durable_storage, bool async_persistence, bool is_leader, bool election_in_progress, int64_t vote_term, int64_t election_term);
constexpr bool raft_server_should_become_secured(bool already_secured, size_t durable_vote_count, size_t quorum);
constexpr bool raft_server_unsecured_leader_needs_quorum_check(bool already_secured, bool is_leader);
constexpr bool raft_server_submission_is_committed(uint64_t commit_index, uint64_t submitted_index, bool entry_matches);
constexpr bool raft_server_submission_is_superseded(uint64_t commit_index, uint64_t submitted_index, bool entry_known_conflict, bool committed_newer_prefix);
constexpr bool raft_server_snapshot_resolves_submission(uint64_t snapshot_index, uint64_t submitted_index);
constexpr bool raft_server_snapshot_submission_is_committed(uint64_t snapshot_index, uint64_t snapshot_term, uint64_t submitted_index, uint64_t submitted_term, bool local_entry_matches, bool local_commit_crossed, bool snapshot_prefix_matches);
constexpr bool raft_server_snapshot_submission_is_superseded(uint64_t snapshot_index, uint64_t snapshot_term, uint64_t submitted_index, uint64_t submitted_term, bool local_entry_known_conflict, bool local_commit_crossed, bool snapshot_prefix_matches);
constexpr bool raft_server_snapshot_submission_is_indeterminate(uint64_t snapshot_index, uint64_t submitted_index, bool committed, bool superseded);
constexpr bool raft_server_command_is_internal_noop(int32_t command_kind, int32_t noop_kind);
constexpr uint64_t raft_server_retention_window_normalize(uint64_t window);
constexpr uint64_t raft_server_retention_cutoff(uint64_t execute_index, uint64_t retention_window);
constexpr bool raft_server_leadership_transition_to_leader(bool new_is_leader, bool previous_is_leader);
constexpr bool raft_server_leadership_transition_to_follower(bool new_is_leader, bool previous_is_leader);
constexpr bool raft_server_observed_higher_term(uint64_t observed_term, uint64_t current_term);
constexpr bool raft_server_signed_term_is_newer(int64_t observed_term, uint64_t current_term);
constexpr uint16_t raft_server_leader_hint_after_transition(bool is_leader, bool has_known_leader, uint16_t self_id, uint16_t known_leader_id, uint16_t invalid_site_id);
constexpr int32_t raft_server_view_leader_locale(uint16_t leader_site, uint16_t self_site, int32_t self_locale, int32_t mapped_locale, uint16_t invalid_site_id);
constexpr uint16_t raft_server_recovery_leader_site(int32_t leader_locale, int32_t self_locale, uint16_t self_site, uint16_t mapped_site, uint16_t invalid_site_id);
constexpr bool raft_server_recovery_view_matches_term(uint32_t incoming_view_id, uint32_t local_view_id);
constexpr bool raft_server_recovery_view_shape_is_valid(uint32_t incoming_partition, uint32_t expected_partition, int32_t incoming_replicas, int32_t expected_replicas, uint64_t leader_count, bool allow_empty);
constexpr bool raft_server_recovery_view_matches_role(bool term_matches, bool local_is_leader, bool view_leader_is_self, bool has_known_leader, bool known_leader_matches_view);
constexpr bool raft_server_leader_rpc_sender_is_authoritative(bool leader_has_higher_term, bool local_is_leader, bool sender_is_self, bool has_known_leader, bool known_leader_matches_sender);
constexpr bool raft_server_term_advance_is_durable(bool has_configured_storage, bool persistence_succeeded);
constexpr bool raft_server_log_index_at_or_below(uint64_t index, uint64_t boundary) {
    return rusty::detail::deref_if_pointer_like(index) <= rusty::detail::deref_if_pointer_like(boundary);
}
constexpr bool raft_server_log_index_above(uint64_t index, uint64_t boundary) {
    return rusty::detail::deref_if_pointer_like(index) > rusty::detail::deref_if_pointer_like(boundary);
}
constexpr bool raft_server_site_is_preferred_leader(uint16_t site_id, uint16_t preferred_site_id, uint16_t invalid_site_id) {
    return (rusty::detail::deref_if_pointer_like(preferred_site_id) != rusty::detail::deref_if_pointer_like(invalid_site_id)) && (rusty::detail::deref_if_pointer_like(site_id) == rusty::detail::deref_if_pointer_like(preferred_site_id));
}
constexpr bool raft_server_leadership_monitor_should_start(bool is_preferred, bool is_leader, bool looping) {
    return (!is_preferred && rusty::detail::deref_if_pointer_like(is_leader)) && rusty::detail::deref_if_pointer_like(looping);
}
constexpr bool raft_server_preferred_replica_is_caught_up(uint64_t preferred_match_index, uint64_t commit_index) {
    return rusty::detail::deref_if_pointer_like(preferred_match_index) >= rusty::detail::deref_if_pointer_like(commit_index);
}
constexpr bool raft_server_local_commit_has_caught_up(uint64_t local_commit_index, uint64_t leader_commit_index) {
    return rusty::detail::deref_if_pointer_like(local_commit_index) >= rusty::detail::deref_if_pointer_like(leader_commit_index);
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
constexpr bool raft_server_leadership_stable_window_elapsed(uint64_t elapsed, uint64_t minimum) {
    return rusty::detail::deref_if_pointer_like(elapsed) >= rusty::detail::deref_if_pointer_like(minimum);
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
constexpr uint64_t raft_server_effective_election_timeout(uint64_t randomized_timeout, uint64_t randomized_minimum, uint64_t heartbeat_interval, bool storage_configured) {
    if (storage_configured) {
        const auto maximum_floor_input = rusty::detail::deref_if_pointer_like(std::numeric_limits<uint64_t>::max()) / 20;
        const auto persistence_floor = (rusty::detail::deref_if_pointer_like(heartbeat_interval) > rusty::detail::deref_if_pointer_like(maximum_floor_input) ? std::numeric_limits<uint64_t>::max() : rusty::detail::deref_if_pointer_like(heartbeat_interval) * static_cast<uint64_t>(20));
        const auto persistence_guard = (rusty::detail::deref_if_pointer_like(persistence_floor) > rusty::detail::deref_if_pointer_like(randomized_minimum) ? rusty::detail::deref_if_pointer_like(persistence_floor) - rusty::detail::deref_if_pointer_like(randomized_minimum) : 0);
        if (rusty::detail::deref_if_pointer_like(randomized_timeout) > (rusty::detail::deref_if_pointer_like(std::numeric_limits<uint64_t>::max()) - rusty::detail::deref_if_pointer_like(persistence_guard))) {
            return std::numeric_limits<uint64_t>::max();
        } else {
            return rusty::detail::deref_if_pointer_like(randomized_timeout) + rusty::detail::deref_if_pointer_like(persistence_guard);
        }
    } else {
        return std::move(randomized_timeout);
    }
}
constexpr bool raft_server_election_in_startup_grace_period(uint64_t now, uint64_t started_at, uint64_t grace_period) {
    return rusty::wrapping_sub(now, static_cast<std::remove_cvref_t<decltype(now)>>(std::move(started_at))) < rusty::detail::deref_if_pointer_like(grace_period);
}
constexpr bool raft_server_vote_term_is_stale(uint64_t candidate_term, uint64_t current_term) {
    return rusty::detail::deref_if_pointer_like(candidate_term) < rusty::detail::deref_if_pointer_like(current_term);
}
constexpr bool raft_server_vote_is_already_granted_to_other(uint64_t candidate_term, uint64_t current_term, uint16_t voted_for, uint16_t candidate_id, uint16_t invalid_site_id) {
    return ((rusty::detail::deref_if_pointer_like(candidate_term) == rusty::detail::deref_if_pointer_like(current_term)) && (rusty::detail::deref_if_pointer_like(voted_for) != rusty::detail::deref_if_pointer_like(invalid_site_id))) && (rusty::detail::deref_if_pointer_like(voted_for) != rusty::detail::deref_if_pointer_like(candidate_id));
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
constexpr bool raft_server_snapshot_recovery_retains_suffix(bool has_suffix, bool has_boundary, bool boundary_matches, bool storage_compaction_proves_suffix, bool live_snapshot_proves_suffix) {
    return rusty::detail::deref_if_pointer_like(has_suffix) && ((((rusty::detail::deref_if_pointer_like(has_boundary) && rusty::detail::deref_if_pointer_like(boundary_matches))) || ((!has_boundary && ((rusty::detail::deref_if_pointer_like(storage_compaction_proves_suffix) || rusty::detail::deref_if_pointer_like(live_snapshot_proves_suffix)))))));
}
constexpr bool raft_server_snapshot_recovery_has_unproven_gap(bool has_suffix, bool has_boundary, bool storage_compaction_proves_suffix, bool live_snapshot_proves_suffix) {
    return ((rusty::detail::deref_if_pointer_like(has_suffix) && !has_boundary) && !storage_compaction_proves_suffix) && !live_snapshot_proves_suffix;
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
constexpr bool raft_server_read_index_local_state_allows(bool is_leader, bool disconnected) {
    return rusty::detail::deref_if_pointer_like(is_leader) && !disconnected;
}
constexpr bool raft_server_read_index_round_can_advance(uint64_t round) {
    return rusty::detail::deref_if_pointer_like(round) != rusty::detail::deref_if_pointer_like(std::numeric_limits<uint64_t>::max());
}
constexpr bool raft_server_read_index_reply_confirms_authority(bool response_available, bool is_leader, uint64_t sent_term, uint64_t response_term, uint64_t current_term, uint64_t sent_round, uint64_t active_round) {
    return (((rusty::detail::deref_if_pointer_like(response_available) && rusty::detail::deref_if_pointer_like(is_leader)) && (rusty::detail::deref_if_pointer_like(sent_term) == rusty::detail::deref_if_pointer_like(current_term))) && (rusty::detail::deref_if_pointer_like(response_term) == rusty::detail::deref_if_pointer_like(sent_term))) && (rusty::detail::deref_if_pointer_like(sent_round) == rusty::detail::deref_if_pointer_like(active_round));
}
constexpr bool raft_server_read_index_quorum_is_fresh(uint64_t request_term, uint64_t baseline_round, uint64_t confirmed_term, uint64_t confirmed_round) {
    return (rusty::detail::deref_if_pointer_like(confirmed_term) == rusty::detail::deref_if_pointer_like(request_term)) && (rusty::detail::deref_if_pointer_like(confirmed_round) > rusty::detail::deref_if_pointer_like(baseline_round));
}
constexpr bool raft_server_read_index_has_current_term_commit(uint64_t commit_index, uint64_t commit_term, uint64_t current_term) {
    return (rusty::detail::deref_if_pointer_like(commit_index) != static_cast<uint64_t>(0)) && (rusty::detail::deref_if_pointer_like(commit_term) == rusty::detail::deref_if_pointer_like(current_term));
}
constexpr bool raft_server_read_index_deadline_expired(uint64_t timeout_us, uint64_t elapsed_us) {
    return (rusty::detail::deref_if_pointer_like(timeout_us) != static_cast<uint64_t>(0)) && (rusty::detail::deref_if_pointer_like(elapsed_us) >= rusty::detail::deref_if_pointer_like(timeout_us));
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
constexpr bool raft_server_ack_is_memory(uint64_t ack_type) {
    return rusty::detail::deref_if_pointer_like(ack_type) == static_cast<uint64_t>(0);
}
constexpr bool raft_server_callback_gate_is_open(uint64_t state, uint64_t drain_bit) {
    return ((rusty::detail::deref_if_pointer_like(state) & rusty::detail::deref_if_pointer_like(drain_bit))) == static_cast<uint64_t>(0);
}
constexpr uint64_t raft_server_callback_gate_count(uint64_t state, uint64_t count_mask) {
    return rusty::detail::deref_if_pointer_like(state) & rusty::detail::deref_if_pointer_like(count_mask);
}
constexpr bool raft_server_persistence_can_report_durable(bool has_durable_storage) {
    return std::move(has_durable_storage);
}
constexpr bool raft_server_sync_reply_is_durable(bool has_durable_storage, bool async_persistence) {
    return rusty::detail::deref_if_pointer_like(has_durable_storage) && !async_persistence;
}
constexpr uint64_t raft_server_follower_append_ack_type(bool has_durable_storage, bool async_persistence, bool persistence_succeeded) {
    if (rusty::detail::deref_if_pointer_like(persistence_succeeded) && raft_server_sync_reply_is_durable(std::move(has_durable_storage), std::move(async_persistence))) {
        return static_cast<uint64_t>(1);
    } else {
        return static_cast<uint64_t>(0);
    }
}
constexpr bool raft_server_durable_write_succeeded(bool storage_ready, bool writes_succeeded, bool sync_succeeded) {
    return (rusty::detail::deref_if_pointer_like(storage_ready) && rusty::detail::deref_if_pointer_like(writes_succeeded)) && rusty::detail::deref_if_pointer_like(sync_succeeded);
}
constexpr bool raft_server_async_persistence_should_queue(bool async_persistence, bool storage_configured, bool has_entries) {
    return (rusty::detail::deref_if_pointer_like(async_persistence) && rusty::detail::deref_if_pointer_like(storage_configured)) && rusty::detail::deref_if_pointer_like(has_entries);
}
constexpr bool raft_server_persistence_ticket_is_ready(uint64_t serving_ticket, uint64_t worker_ticket) {
    return rusty::detail::deref_if_pointer_like(serving_ticket) == rusty::detail::deref_if_pointer_like(worker_ticket);
}
constexpr bool raft_server_persisted_reply_context_is_current(bool stopping, bool is_leader, uint64_t current_term, uint64_t accepted_term, uint16_t current_leader, uint16_t accepted_leader) {
    return ((!stopping && !is_leader) && (rusty::detail::deref_if_pointer_like(current_term) == rusty::detail::deref_if_pointer_like(accepted_term))) && (rusty::detail::deref_if_pointer_like(current_leader) == rusty::detail::deref_if_pointer_like(accepted_leader));
}
constexpr bool raft_server_ack_is_durable(uint64_t ack_type) {
    return rusty::detail::deref_if_pointer_like(ack_type) == static_cast<uint64_t>(1);
}
constexpr bool raft_server_can_buffer_early_durable_vote(bool has_durable_storage, bool async_persistence, bool is_leader, bool election_in_progress, int64_t vote_term, int64_t election_term) {
    return (((rusty::detail::deref_if_pointer_like(has_durable_storage) && rusty::detail::deref_if_pointer_like(async_persistence)) && !is_leader) && rusty::detail::deref_if_pointer_like(election_in_progress)) && (rusty::detail::deref_if_pointer_like(vote_term) == rusty::detail::deref_if_pointer_like(election_term));
}
constexpr bool raft_server_should_become_secured(bool already_secured, size_t durable_vote_count, size_t quorum) {
    return !already_secured && (rusty::detail::deref_if_pointer_like(durable_vote_count) >= rusty::detail::deref_if_pointer_like(quorum));
}
constexpr bool raft_server_unsecured_leader_needs_quorum_check(bool already_secured, bool is_leader) {
    return !already_secured && rusty::detail::deref_if_pointer_like(is_leader);
}
constexpr bool raft_server_commit_status_is_durable(CommitStatus status) {
    return ((static_cast<int32_t>(status))) == ((static_cast<int32_t>(CommitStatus_DURABLE())));
}
constexpr bool raft_server_start_was_rejected(RaftStartResult result) {
    return ((static_cast<int32_t>(result))) == ((static_cast<int32_t>(RaftStartResult_REJECTED())));
}
constexpr bool raft_server_start_was_appended(RaftStartResult result) {
    return ((static_cast<int32_t>(result))) == ((static_cast<int32_t>(RaftStartResult_APPENDED())));
}
constexpr bool raft_server_start_is_indeterminate(RaftStartResult result) {
    return ((static_cast<int32_t>(result))) == ((static_cast<int32_t>(RaftStartResult_INDETERMINATE())));
}
constexpr bool raft_server_submission_is_committed(uint64_t commit_index, uint64_t submitted_index, bool entry_matches) {
    return (rusty::detail::deref_if_pointer_like(commit_index) >= rusty::detail::deref_if_pointer_like(submitted_index)) && rusty::detail::deref_if_pointer_like(entry_matches);
}
constexpr bool raft_server_submission_is_superseded(uint64_t commit_index, uint64_t submitted_index, bool entry_known_conflict, bool committed_newer_prefix) {
    return rusty::detail::deref_if_pointer_like(entry_known_conflict) && (((rusty::detail::deref_if_pointer_like(commit_index) >= rusty::detail::deref_if_pointer_like(submitted_index)) || rusty::detail::deref_if_pointer_like(committed_newer_prefix)));
}
constexpr bool raft_server_snapshot_resolves_submission(uint64_t snapshot_index, uint64_t submitted_index) {
    return rusty::detail::deref_if_pointer_like(submitted_index) <= rusty::detail::deref_if_pointer_like(snapshot_index);
}
constexpr bool raft_server_snapshot_submission_is_committed(uint64_t snapshot_index, uint64_t snapshot_term, uint64_t submitted_index, uint64_t submitted_term, bool local_entry_matches, bool local_commit_crossed, bool snapshot_prefix_matches) {
    return raft_server_snapshot_resolves_submission(std::move(snapshot_index), std::move(submitted_index)) && (((((rusty::detail::deref_if_pointer_like(local_commit_crossed) && rusty::detail::deref_if_pointer_like(local_entry_matches))) || ((rusty::detail::deref_if_pointer_like(snapshot_prefix_matches) && rusty::detail::deref_if_pointer_like(local_entry_matches)))) || (((rusty::detail::deref_if_pointer_like(submitted_index) == rusty::detail::deref_if_pointer_like(snapshot_index)) && (rusty::detail::deref_if_pointer_like(submitted_term) == rusty::detail::deref_if_pointer_like(snapshot_term))))));
}
constexpr bool raft_server_snapshot_submission_is_superseded(uint64_t snapshot_index, uint64_t snapshot_term, uint64_t submitted_index, uint64_t submitted_term, bool local_entry_known_conflict, bool local_commit_crossed, bool snapshot_prefix_matches) {
    return raft_server_snapshot_resolves_submission(std::move(snapshot_index), std::move(submitted_index)) && (((((rusty::detail::deref_if_pointer_like(local_commit_crossed) && rusty::detail::deref_if_pointer_like(local_entry_known_conflict))) || ((rusty::detail::deref_if_pointer_like(snapshot_prefix_matches) && rusty::detail::deref_if_pointer_like(local_entry_known_conflict)))) || (((rusty::detail::deref_if_pointer_like(submitted_index) == rusty::detail::deref_if_pointer_like(snapshot_index)) && (rusty::detail::deref_if_pointer_like(submitted_term) != rusty::detail::deref_if_pointer_like(snapshot_term))))));
}
constexpr bool raft_server_snapshot_submission_is_indeterminate(uint64_t snapshot_index, uint64_t submitted_index, bool committed, bool superseded) {
    return (raft_server_snapshot_resolves_submission(std::move(snapshot_index), std::move(submitted_index)) && !committed) && !superseded;
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
constexpr uint16_t raft_server_leader_hint_after_transition(bool is_leader, bool has_known_leader, uint16_t self_id, uint16_t known_leader_id, uint16_t invalid_site_id) {
    if (is_leader) {
        return std::move(self_id);
    } else if (has_known_leader) {
        return std::move(known_leader_id);
    } else {
        return std::move(invalid_site_id);
    }
}
constexpr int32_t raft_server_view_leader_locale(uint16_t leader_site, uint16_t self_site, int32_t self_locale, int32_t mapped_locale, uint16_t invalid_site_id) {
    if (rusty::detail::deref_if_pointer_like(leader_site) == rusty::detail::deref_if_pointer_like(invalid_site_id)) {
        return -1;
    } else if (rusty::detail::deref_if_pointer_like(leader_site) == rusty::detail::deref_if_pointer_like(self_site)) {
        return std::move(self_locale);
    } else {
        return std::move(mapped_locale);
    }
}
constexpr uint16_t raft_server_recovery_leader_site(int32_t leader_locale, int32_t self_locale, uint16_t self_site, uint16_t mapped_site, uint16_t invalid_site_id) {
    if (rusty::detail::deref_if_pointer_like(leader_locale) < 0) {
        return std::move(invalid_site_id);
    } else if (rusty::detail::deref_if_pointer_like(leader_locale) == rusty::detail::deref_if_pointer_like(self_locale)) {
        return std::move(self_site);
    } else {
        return std::move(mapped_site);
    }
}
constexpr bool raft_server_recovery_view_matches_term(uint32_t incoming_view_id, uint32_t local_view_id) {
    return rusty::detail::deref_if_pointer_like(incoming_view_id) == rusty::detail::deref_if_pointer_like(local_view_id);
}
constexpr bool raft_server_recovery_view_shape_is_valid(uint32_t incoming_partition, uint32_t expected_partition, int32_t incoming_replicas, int32_t expected_replicas, uint64_t leader_count, bool allow_empty) {
    return (rusty::detail::deref_if_pointer_like(incoming_partition) == rusty::detail::deref_if_pointer_like(expected_partition)) && (((((rusty::detail::deref_if_pointer_like(allow_empty) && (rusty::detail::deref_if_pointer_like(incoming_replicas) == static_cast<int32_t>(0))) && (rusty::detail::deref_if_pointer_like(leader_count) == static_cast<uint64_t>(0)))) || ((((rusty::detail::deref_if_pointer_like(incoming_replicas) > 0) && ((rusty::detail::deref_if_pointer_like(allow_empty) || (rusty::detail::deref_if_pointer_like(incoming_replicas) == rusty::detail::deref_if_pointer_like(expected_replicas))))) && (rusty::detail::deref_if_pointer_like(leader_count) == static_cast<uint64_t>(1))))));
}
constexpr bool raft_server_recovery_view_matches_role(bool term_matches, bool local_is_leader, bool view_leader_is_self, bool has_known_leader, bool known_leader_matches_view) {
    return rusty::detail::deref_if_pointer_like(term_matches) && ((((rusty::detail::deref_if_pointer_like(local_is_leader) && rusty::detail::deref_if_pointer_like(view_leader_is_self))) || (((!local_is_leader && !view_leader_is_self) && ((!has_known_leader || rusty::detail::deref_if_pointer_like(known_leader_matches_view)))))));
}
constexpr bool raft_server_leader_rpc_sender_is_authoritative(bool leader_has_higher_term, bool local_is_leader, bool sender_is_self, bool has_known_leader, bool known_leader_matches_sender) {
    return (((rusty::detail::deref_if_pointer_like(sender_is_self) && rusty::detail::deref_if_pointer_like(local_is_leader)) && !leader_has_higher_term)) || ((!sender_is_self && ((rusty::detail::deref_if_pointer_like(leader_has_higher_term) || ((!local_is_leader && ((!has_known_leader || rusty::detail::deref_if_pointer_like(known_leader_matches_sender)))))))));
}
constexpr bool raft_server_term_advance_is_durable(bool has_configured_storage, bool persistence_succeeded) {
    return !has_configured_storage || rusty::detail::deref_if_pointer_like(persistence_succeeded);
}
/*RUSTYCPP:GEN-END id=raft_server.scalar_decisions*/

static_assert(raft_server_site_is_preferred_leader(
    7, 7, static_cast<uint16_t>(INVALID_SITEID)));
static_assert(!raft_server_site_is_preferred_leader(
    static_cast<uint16_t>(INVALID_SITEID),
    static_cast<uint16_t>(INVALID_SITEID),
    static_cast<uint16_t>(INVALID_SITEID)));
static_assert(raft_server_vote_is_already_granted_to_other(
    4, 4, 1, 2, static_cast<uint16_t>(INVALID_SITEID)));
static_assert(!raft_server_vote_is_already_granted_to_other(
    4, 4, static_cast<uint16_t>(INVALID_SITEID), 2,
    static_cast<uint16_t>(INVALID_SITEID)));
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
static_assert(raft_server_effective_election_timeout(
                  600000, 500000, 100000, true) == 2100000);
static_assert(raft_server_effective_election_timeout(
                  500000, 500000, 100000, true) == 2000000);
static_assert(raft_server_effective_election_timeout(
                  1000000, 500000, 100000, true) == 2500000);
static_assert(raft_server_effective_election_timeout(
                  200000, 150000, 5000, true) == 200000);
static_assert(raft_server_effective_election_timeout(
                  600000, 500000, 100000, false) == 600000);
static_assert(raft_server_effective_election_timeout(
                  7, 0, UINT64_MAX, true) == UINT64_MAX);
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
    true, true, true, false, false));
static_assert(!raft_server_snapshot_recovery_retains_suffix(
    true, true, false, true, true));
static_assert(raft_server_snapshot_recovery_retains_suffix(
    true, false, false, true, false));
static_assert(raft_server_snapshot_recovery_retains_suffix(
    true, false, false, false, true));
static_assert(!raft_server_snapshot_recovery_retains_suffix(
    false, false, false, true, true));
static_assert(raft_server_snapshot_recovery_has_unproven_gap(
    true, false, false, false));
static_assert(!raft_server_snapshot_recovery_has_unproven_gap(
    true, true, false, false));
static_assert(!raft_server_snapshot_recovery_has_unproven_gap(
    true, false, true, false));
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
static_assert(raft_server_read_index_local_state_allows(true, false));
static_assert(!raft_server_read_index_local_state_allows(true, true));
static_assert(!raft_server_read_index_local_state_allows(false, false));
static_assert(raft_server_read_index_round_can_advance(0));
static_assert(!raft_server_read_index_round_can_advance(UINT64_MAX));
static_assert(raft_server_read_index_reply_confirms_authority(
    true, true, 7, 7, 7, 11, 11));
static_assert(!raft_server_read_index_reply_confirms_authority(
    true, true, 7, 7, 7, 10, 11));
static_assert(!raft_server_read_index_reply_confirms_authority(
    true, true, 7, 8, 7, 11, 11));
static_assert(raft_server_read_index_quorum_is_fresh(7, 10, 7, 11));
static_assert(!raft_server_read_index_quorum_is_fresh(7, 11, 7, 11));
static_assert(!raft_server_read_index_quorum_is_fresh(7, 10, 8, 11));
static_assert(raft_server_read_index_has_current_term_commit(9, 7, 7));
static_assert(!raft_server_read_index_has_current_term_commit(0, 7, 7));
static_assert(!raft_server_read_index_has_current_term_commit(9, 6, 7));
static_assert(raft_server_read_index_deadline_expired(100, 100));
static_assert(!raft_server_read_index_deadline_expired(100, 99));
static_assert(!raft_server_read_index_deadline_expired(0, UINT64_MAX));
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
static_assert(raft_server_ack_is_memory(0));
static_assert(!raft_server_ack_is_memory(1));
static_assert(!raft_server_ack_is_memory(UINT64_MAX));
static_assert(raft_server_should_become_secured(false, 2, 2));
static_assert(!raft_server_should_become_secured(false, 1, 2));
static_assert(!raft_server_should_become_secured(true, 2, 2));
static_assert(raft_server_should_become_secured(false, 0, 0));
static_assert(raft_server_unsecured_leader_needs_quorum_check(false, true));
static_assert(!raft_server_unsecured_leader_needs_quorum_check(true, true));
static_assert(!raft_server_unsecured_leader_needs_quorum_check(false, false));
static_assert(raft_server_commit_status_is_durable(CommitStatus::DURABLE));
static_assert(!raft_server_commit_status_is_durable(CommitStatus::SPECULATIVE));
static_assert(raft_server_start_was_rejected(RaftStartResult::REJECTED));
static_assert(!raft_server_start_was_rejected(RaftStartResult::APPENDED));
static_assert(raft_server_start_was_appended(RaftStartResult::APPENDED));
static_assert(!raft_server_start_was_appended(
    RaftStartResult::INDETERMINATE));
static_assert(raft_server_start_is_indeterminate(
    RaftStartResult::INDETERMINATE));
static_assert(!raft_server_start_is_indeterminate(
    RaftStartResult::REJECTED));
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

// @unsafe - Stateful STL-set kernel. Scalar persistence decisions are owned by
// the Rust DSL helpers above.
inline std::set<siteid_t> raft_server_initial_durable_voters(
    bool has_durable_storage,
    bool async_persistence,
    bool local_vote_persisted,
    siteid_t self,
    const std::set<siteid_t>& speculative_voters,
    const std::set<siteid_t>& early_durable_voters) {
  if (!raft_server_persistence_can_report_durable(has_durable_storage)) {
    return {};
  }
  if (raft_server_sync_reply_is_durable(has_durable_storage,
                                        async_persistence)) {
    // Every successful synchronous Vote reply crossed the persistence
    // boundary before it entered speculative_voters. The candidate's own
    // in-memory vote is removed if its local write+sync failed.
    std::set<siteid_t> voters = speculative_voters;
    if (!local_vote_persisted) {
      voters.erase(self);
    }
    return voters;
  }

  // The candidate attempts its own vote synchronously before broadcasting.
  // Followers become durable only through VoteDurable notifications, some of
  // which can race ahead of the ordinary Vote quorum response.
  std::set<siteid_t> voters = early_durable_voters;
  if (local_vote_persisted) {
    voters.insert(self);
  }
  return voters;
}

// @unsafe - Stateful STL-map/set kernel. The quorum predicate itself is owned
// by the Rust DSL scalar block above.
inline uint64_t raft_server_highest_contiguous_secured_index(
    uint64_t secured_index,
    uint64_t speculative_index,
    uint64_t last_log_index,
    size_t quorum,
    const std::map<uint64_t, std::set<siteid_t>>& durable_acks) {
  const uint64_t upper = std::min(speculative_index, last_log_index);
  if (secured_index >= upper) {
    return secured_index;
  }

  uint64_t result = secured_index;
  for (uint64_t index = secured_index + 1; index <= upper; ++index) {
    const auto it = durable_acks.find(index);
    if (it == durable_acks.end() ||
        !raft_server_should_become_secured(
            /*already_secured=*/false, it->second.size(), quorum)) {
      break;
    }
    result = index;
    if (index == upper) {
      break;  // Avoid wrapping when upper == UINT64_MAX.
    }
  }
  return result;
}

static_assert(raft_server_follower_append_ack_type(false, false, true) == 0);
static_assert(raft_server_follower_append_ack_type(true, false, true) == 1);
static_assert(raft_server_follower_append_ack_type(true, false, false) == 0);
static_assert(raft_server_follower_append_ack_type(true, true, true) == 0);
static_assert(!raft_server_persistence_can_report_durable(false));
static_assert(raft_server_persistence_can_report_durable(true));
static_assert(raft_server_durable_write_succeeded(true, true, true));
static_assert(!raft_server_durable_write_succeeded(false, true, true));
static_assert(!raft_server_durable_write_succeeded(true, false, true));
static_assert(!raft_server_durable_write_succeeded(true, true, false));
static_assert(raft_server_async_persistence_should_queue(true, true, true));
static_assert(!raft_server_async_persistence_should_queue(false, true, true));
static_assert(!raft_server_async_persistence_should_queue(true, false, true));
static_assert(!raft_server_async_persistence_should_queue(true, true, false));
static_assert(raft_server_persistence_ticket_is_ready(7, 7));
static_assert(!raft_server_persistence_ticket_is_ready(6, 7));
static_assert(raft_server_persisted_reply_context_is_current(
    false, false, 9, 9, 3, 3));
static_assert(!raft_server_persisted_reply_context_is_current(
    true, false, 9, 9, 3, 3));
static_assert(!raft_server_persisted_reply_context_is_current(
    false, true, 9, 9, 3, 3));
static_assert(!raft_server_persisted_reply_context_is_current(
    false, false, 10, 9, 3, 3));
static_assert(!raft_server_persisted_reply_context_is_current(
    false, false, 9, 9, 4, 3));

// @unsafe - Executes an external LogStorage mutation and sync.  This is the
// single write+sync boundary used by durable Raft state and log writes; callers
// additionally gate its result with the server's sticky persistence health.
template <typename WriteOperation>
bool raft_server_write_and_sync(
    raft::LogStorage& storage, WriteOperation&& write_operation) {
  try {
    const bool storage_ready = storage.is_open();
    if (!storage_ready) {
      return false;
    }
    const bool writes_succeeded =
        std::forward<WriteOperation>(write_operation)(storage);
    const bool sync_succeeded = writes_succeeded && storage.sync();
    return raft_server_durable_write_succeeded(
        storage_ready, writes_succeeded, sync_succeeded);
  } catch (...) {
    // Storage implementations parse persistent metadata and invoke backend
    // APIs that may throw.  A durable ACK boundary must convert every such
    // failure into a failed write, never unwind through an RPC handler after
    // partially changing Raft or its state machine.
    return false;
  }
}

// A failed append may already have changed storage before its sync/exception
// boundary. A caller may report definitive rejection only after this
// compensating removal itself crosses a sync boundary and a final read proves
// the slot absent. This deliberately bypasses the sticky health gate: it is a
// last best-effort proof before the replica fail-stops.
inline bool raft_server_compensating_remove_and_sync(
    raft::LogStorage& storage, slotid_t slot_id) {
  try {
    if (!storage.is_open()) {
      return false;
    }
    const bool already_absent = storage.get(slot_id).is_none();
    const bool removal_succeeded =
        already_absent || storage.remove(slot_id);
    if (!removal_succeeded || !storage.sync()) {
      return false;
    }
    return storage.get(slot_id).is_none();
  } catch (...) {
    return false;
  }
}

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

// One locked observation of the two terminal conditions awaited by a local
// submitter. A term change alone is deliberately not terminal: an old-term
// entry can still be retained and committed by a later leader. Resolution is
// known only after the committed prefix crosses the submitted slot, at which
// point the slot either still has the submitted term or has been superseded.
struct RaftSubmissionProgress {
  bool committed = false;
  bool superseded = false;
  // A divergent installed snapshot covered the slot without carrying enough
  // per-entry identity to distinguish committed from superseded. This is a
  // terminal commit-outcome ambiguity; it must never be reported as success
  // or as a safe-to-retry rejection.
  bool indeterminate = false;
};

// One-shot terminal results whose identifying log slots were consumed by an
// installed snapshot. This container is deliberately not synchronized: its
// RaftServer owner accesses it only under mtx_. Record() rejects non-terminal
// and duplicate results, while Consume() removes the result it returns. The
// server transfers one active registration into this ledger per record, so
// its cardinality is bounded by tracked submissions not yet observed by their
// coordinators rather than accumulating a history of snapshot epochs.
class RaftResolvedSubmissionLedger {
 public:
  using Key = std::pair<slotid_t, ballot_t>;

  bool Record(const Key& key, const RaftSubmissionProgress& progress) {
    const unsigned terminal_outcomes =
        static_cast<unsigned>(progress.committed) +
        static_cast<unsigned>(progress.superseded) +
        static_cast<unsigned>(progress.indeterminate);
    if (terminal_outcomes != 1) {
      return false;
    }
    return resolved_.emplace(key, progress).second;
  }

  std::pair<bool, RaftSubmissionProgress> Consume(const Key& key) {
    const auto found = resolved_.find(key);
    if (found == resolved_.end()) {
      return {false, {}};
    }
    const RaftSubmissionProgress progress = found->second;
    resolved_.erase(found);
    return {true, progress};
  }

  size_t size() const { return resolved_.size(); }

 private:
  std::map<Key, RaftSubmissionProgress> resolved_;
};
#ifdef RAFT_TEST_CORO
#define HEARTBEAT_INTERVAL 100000
#else
#define HEARTBEAT_INTERVAL 5000
#endif


// @unsafe - inherits from non-@interface TxLogServer (individual methods are @safe)
class RaftServer : public TxLogServer {
  friend class RaftTestConfig;  // Allow test config to access private members for kill/restart
  friend class RaftLabTest;     // Allow test cases to access private members for verification
 private:
  struct AsyncCallbackLifetime {
    std::mutex mutex;
    RaftServer* server = nullptr;
  };

  // RPC futures can outlive the server during test kill/restart. Destruction
  // nulls this shared gate after waiting for any callback already using it.
  std::shared_ptr<AsyncCallbackLifetime> async_callback_lifetime_ =
      std::make_shared<AsyncCallbackLifetime>();

  // Coordinator submissions remain here until GetSubmissionProgress captures
  // a terminal result. CompactLog retains their exact slot identity meanwhile.
  std::set<std::pair<slotid_t, ballot_t>> active_submissions_;

  // InstallSnapshot must erase covered log identities. It first transfers
  // their terminal outcomes here so the owning coordinator cannot wait
  // forever after compaction; GetSubmissionProgress consumes each result.
  RaftResolvedSubmissionLedger resolved_submissions_;

  // Caller holds mtx_ and invokes this after the snapshot has been accepted,
  // but before any covered raft_logs_ entry is erased.
  void ResolveSnapshotCoveredSubmissionsLocked(
      slotid_t last_included_index,
      ballot_t last_included_term,
      bool snapshot_prefix_matches);

  // Caller holds mtx_. Raft's consensus state always uses global site IDs;
  // these helpers convert only at the partition-local View boundary.
  int LeaderSiteToLocaleLocked(siteid_t leader_site) const;
  siteid_t LeaderLocaleToSiteLocked(int leader_locale) const;

  // Shared atomic append path for ordinary and terminally-tracked callers.
  RaftStartResult StartImpl(const janus::Command& cmd,
                            uint64_t* index,
                            uint64_t* term,
                            bool track_resolution,
                            slotid_t slot_id,
                            ballot_t ballot);

  // ============================================================================
  // LOG PERSISTENCE
  // ============================================================================
  std::shared_ptr<janus::raft::LogStorage> log_storage_;  // Optional persistent storage
  bool async_persistence_ = false;  // Runtime: sync (default) vs async disk persistence
  // Sticky for this server lifetime: after any configured-storage write/sync
  // failure, no later operation may advertise a durable prefix that contains
  // the failed state. Recovery/restart installs a fresh healthy storage epoch.
  rusty::sync::atomic::AtomicBool persistence_healthy_{true};

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

  // State machine snapshot callbacks (set by ReplicatedDB or other state machines)
  // @unsafe - std::function holds non-borrow-checked closures
  // The requested boundary is part of the callback contract: an application
  // checkpoint that represents any other applied index must be rejected before
  // Raft durably publishes the snapshot or compacts its reconstruction log.
  std::function<std::string(uint64_t)> create_sm_snapshot_cb_;
  std::function<std::unique_ptr<PreparedStateMachineSnapshotInstall>(
      const std::string&, uint64_t)> prepare_sm_snapshot_cb_;
  uint64_t snapshot_callback_owner_token_ = 0;
  uint64_t next_snapshot_callback_owner_token_ = 1;

  // Optional replicated DB (created when MAKO_REPLICATED_DB=1 env var is set)
  std::shared_ptr<ReplicatedDB> replicated_db_;

  // @unsafe - Initializes the snapshot manager and restores the exact state
  // machine bytes before publishing any recovered snapshot boundary.
  bool InitializeSnapshotManager();

  // @unsafe - Caller holds state_machine_apply_mtx_ then mtx_. Fully validates
  // and stages a production state-machine image without publishing it, or
  // validates the RaftLab marker payload and returns a no-op transaction.
  std::unique_ptr<PreparedStateMachineSnapshotInstall>
  PrepareStateMachineSnapshotLocked(
      const std::string& data,
      uint64_t last_included_index,
      uint64_t last_included_term);

  // @unsafe - Startup helper for an already-durable Raft snapshot. Prepares and
  // immediately commits its state-machine image before publishing recovery.
  bool LoadStateMachineSnapshotLocked(
      const std::string& data,
      uint64_t last_included_index,
      uint64_t last_included_term);

  // @unsafe - External snapshot entry point. Acquires the state-machine apply
  // gate before mtx_ so the serialized bytes and executeIndex describe the
  // same applied prefix.
  void CreateSnapshot();

  // @unsafe - Requires state_machine_apply_mtx_ and mtx_ in that order.
  // Split out so the apply trigger and RaftLabTest's friend-only manager
  // rotation helper can preserve the global lock order without re-locking.
  bool CreateSnapshotLocked();

  // @unsafe - Cheap-trigger slow path. Acquires state_machine_apply_mtx_ then
  // mtx_, rechecks the canonical snapshot state, and snapshots only if due.
  void MaybeCreateSnapshot();

  // Metadata keys for LogStorage persistence
  static constexpr const char* META_TERM = "currentTerm";
  static constexpr const char* META_VOTE_FOR = "vote_for";
  static constexpr const char* META_COMMIT_INDEX = "commitIndex";
  static constexpr const char* META_SPEC_COMMIT_INDEX = "specCommitIndex";
  static constexpr const char* META_SECURED_LOG_INDEX = "securedLogIndex";

  // @safe - LogStorage-based persistence helper methods (external LogStorage API calls wrapped in @unsafe blocks)
  bool PersistTermAndVoteToLogStorage(uint64_t term, siteid_t voted_for);
  // @unsafe - Ordered wrapper for the current speculative metadata.
  bool PersistSpeculativeIndicesToLogStorage();
  // @unsafe - Unordered storage primitive; caller already owns a persistence
  // ticket or is running before concurrent admission opens.
  bool PersistSpeculativeIndicesSnapshotToLogStorage(
      uint64_t spec_commit_index, uint64_t secured_log_index);
  // @safe - Persists vote_for only to storage
  bool PersistVoteToLogStorage(siteid_t voted_for);
  // @safe - Persists commitIndex to storage
  bool PersistCommitIndexToLogStorage(uint64_t commit_index,
                                      uint64_t spec_commit_index,
                                      uint64_t secured_log_index);
  // @safe - Persists a single log entry
  bool PersistLogEntryToLogStorage(slotid_t slot_id, const RaftData& data);
  // @safe - Persists multiple log entries
  bool PersistLogEntriesToLogStorage(const std::vector<std::pair<slotid_t, std::shared_ptr<RaftData>>>& entries);
  // @unsafe - Replaces one follower log suffix and crosses one storage sync
  // boundary. The optional removal range is inclusive.
  bool PersistFollowerAppendToLogStorage(
      const std::vector<std::pair<slotid_t, std::shared_ptr<RaftData>>>& entries,
      const std::vector<std::pair<slotid_t, std::shared_ptr<RaftData>>>&
          matching_entries_to_verify,
      uint64_t committed_index,
      bool truncate_suffix,
      slotid_t truncate_first,
      slotid_t truncate_last);

  // Every accepted follower-log storage action (sync, async, or snapshot)
  // reserves its sequence while mtx_ still defines acceptance order.
  uint64_t ReserveLogPersistenceTicketLocked();
  void WaitForLogPersistenceTicket(uint64_t ticket);
  void CompleteLogPersistenceTicket(uint64_t ticket);
  void DrainLogPersistenceSequence();

  // Executes one already-reserved ticket and always advances the FIFO, even if
  // external storage throws. Callers reserve under mtx_ before publishing the
  // corresponding in-memory mutation.
  template <typename PersistenceOperation>
  bool ExecuteLogPersistenceTicket(
      uint64_t ticket,
      const char* operation,
      PersistenceOperation&& persistence_operation) {
    WaitForLogPersistenceTicket(ticket);
    bool succeeded = false;
    try {
      succeeded = std::forward<PersistenceOperation>(
          persistence_operation)();
    } catch (const std::exception& error) {
      Log_error("[RAFT-PERSISTENCE] Site {} operation '{}' threw: {}",
                site_id_, operation ? operation : "unknown", error.what());
      RecordPersistenceResult(false, operation);
    } catch (...) {
      Log_error("[RAFT-PERSISTENCE] Site {} operation '{}' threw an unknown exception",
                site_id_, operation ? operation : "unknown");
      RecordPersistenceResult(false, operation);
    }
    CompleteLogPersistenceTicket(ticket);
    return succeeded;
  }

  // Scope completion for the InstallSnapshot epoch, whose ticket deliberately
  // spans storage, in-memory reconciliation, and state-machine installation.
  class LogPersistenceTicketCompletion {
   public:
    LogPersistenceTicketCompletion(RaftServer* owner,
                                   uint64_t ticket,
                                   bool active)
        : owner_(owner), ticket_(ticket), active_(active) {}
    LogPersistenceTicketCompletion(const LogPersistenceTicketCompletion&) = delete;
    LogPersistenceTicketCompletion& operator=(
        const LogPersistenceTicketCompletion&) = delete;
    ~LogPersistenceTicketCompletion() noexcept {
      if (active_) {
        owner_->CompleteLogPersistenceTicket(ticket_);
      }
    }

   private:
    RaftServer* owner_;
    uint64_t ticket_;
    bool active_;
  };

  // Caller holds mtx_. Snapshot-aware, non-mutating boundary validation used
  // after persistence completes and before a success/durable proof is emitted.
  bool PersistedAppendContextIsCurrentLocked(
      uint64_t accepted_term,
      siteid_t accepted_leader,
      slotid_t boundary_index,
      ballot_t boundary_term) const;

  // @safe - Atomically marks the current persistence epoch unhealthy on the
  // first failed external storage operation.
  bool RecordPersistenceResult(bool succeeded, const char* operation);
  // @unsafe - Moves and joins every tracked native persistence worker.
  void DrainAsyncPersistenceThreads();

  // ============================================================================

  std::map<siteid_t, uint64_t> match_index_{};
  std::map<siteid_t, uint64_t> next_index_{};
  // ReadIndex authority proof, guarded by mtx_. A read captures
  // heartbeat_round_ and accepts only a quorum-confirmed later round in the
  // same term and membership configuration.
  uint64_t heartbeat_round_ = 0;
  uint64_t read_quorum_confirmed_term_ = 0;
  uint64_t read_quorum_confirmed_round_ = 0;

  // Caller holds mtx_. Uses only non-mutating log lookup and the persisted
  // snapshot boundary tuple; it must never recreate a compacted log entry.
  bool HasCommittedEntryInCurrentTermLocked() const;

  std::vector<std::thread> timer_threads_ = {};
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
  // Guarded by mtx_. OnAppendEntries releases mtx_ while synchronous storage
  // crosses its durability boundary. Treat that interval as one active leader
  // contact, matching a single-threaded Raft event loop: this follower must not
  // campaign in the middle of an already-accepted AppendEntries handler.
  uint64_t accepted_sync_append_persistence_ = 0;
  // @safe - logging calls wrapped in @unsafe blocks in implementation
  void LogTermChange(const char* reason, uint64_t old_term, uint64_t new_term, siteid_t source = INVALID_SITEID);
  rusty::sync::atomic::AtomicBool stop_{false};
  // Consensus RPC services are registered before their owner-thread Setup job
  // runs. Admission stays closed until durable log/snapshot recovery and state-
  // machine replay have completed, and closes again before shutdown drains.
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
  // Delayed preferred-leader elections are separate reactor fibers. Shutdown
  // waits for this count so none can retain `this` past server destruction.
  rusty::sync::atomic::AtomicU64 transfer_election_jobs_{0};
  bool heartbeat_ = true;
  bool heartbeat_setup_ = false;
  uint64_t heartbeat_interval_us_ = HEARTBEAT_INTERVAL;  // Runtime-configurable heartbeat interval (microseconds)
  uint64_t log_retention_window_ = 5000;  // Configurable log retention window (entries to keep after compaction)

  // Cross-thread submissions publish only to this level-triggered gate.  The
  // gate posts a gate-only job to the heartbeat PollThread; IntEvent itself is
  // created, signalled, waited, and cleared exclusively by that owner thread.
  rusty::Arc<ReplicationWakeGate> replication_wake_gate_;

  // Outbound InstallSnapshot futures can complete after HeartbeatLoop exits.
  // They capture only this independently owned gate, never a RaftServer raw
  // pointer. Shutdown closes its pointer admission and drains active borrowers.
  rusty::Arc<InstallSnapshotCallbackGate> install_snapshot_callback_gate_;

  // @unsafe - Reactor bridge; schedules a gate-only job on the bound owner.
  void RequestReplication();
  // @unsafe - Gives the owner scheduler one run opportunity after configured
  // local persistence, preventing hot client relocks from starving heartbeats.
  void YieldAfterSynchronousLocalAppend();
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
  // PREFERRED REPLICA SYSTEM - Leadership Transfer
  // ============================================================================
  // Implements leadership transfer protocol where one replica is designated as
  // the "preferred leader". The system works via:
  // 1. Standard Raft voting (no bias) - any replica can win initial election
  // 2. Non-preferred leader monitors for preferred replica
  // 3. When preferred is alive & caught up, non-preferred leader:
  //    - Ensures preferred has all committed logs
  //    - Steps down from leadership
  //    - Preferred replica starts election and becomes leader
  // 4. All operations maintain Raft safety guarantees (no data loss)

  siteid_t preferred_leader_site_id_ = INVALID_SITEID;     // Site ID of preferred leader
  uint64_t leader_last_commit_index_ = 0;                   // Leader's commit index (from heartbeats)
  bool transferring_leadership_ = false;                    // True when transfer in progress
  uint64_t leadership_transfer_start_time_ = 0;             // When transfer started (for timeout)
  rusty::sync::atomic::AtomicBool leadership_monitor_stop_{false};
  rusty::sync::atomic::AtomicBool leadership_monitor_joining_{false};
  rusty::Mutex<rusty::Option<rusty::thread::JoinHandle<
      rusty::thread::Unit>>> leadership_monitor_thread_{rusty::None};
  rusty::Mutex<bool> leadership_monitor_wait_mtx_{false};
  rusty::Condvar leadership_monitor_wait_cv_;
  uint64_t startup_timestamp_ = 0;                          // When server started (for grace period)

  // ============================================================================
  // SPECULATIVE REPLICATION STATE
  // ============================================================================
  // Enables separation of "speculative" (memory quorum) from "secured" (durable
  // quorum) for both leadership and log entries. See docs/dev/phase1_speculative_state_plan.md

  // Leader security status - true when durable vote quorum achieved
  // When securedLeader_ = true, a quorum has votedFor = me on disk,
  // so no other candidate can win election in this term.
  bool securedLeader_ = false;

  // Vote tracking for current term (as candidate/leader)
  std::set<siteid_t> specVoters_;     // servers that have memory-voted for us
  std::set<siteid_t> durableVoters_;  // servers that have durably-voted for us

  // VoteDurable can arrive before BroadcastVote returns and before the
  // candidate flips is_leader_.  Track that narrow election window explicitly
  // so a genuinely durable async vote is not lost due to message ordering.
  bool election_in_progress_ = false;
  ballot_t election_term_ = 0;
  std::set<siteid_t> earlyDurableVoters_;

  // Log commit tracking
  // Invariant: securedLogIndex_ <= specCommitIndex_ <= lastLogIndex
  uint64_t securedLogIndex_ = 0;      // highest index with durable ack quorum
  uint64_t specCommitIndex_ = 0;      // highest index with memory ack quorum

  // Acknowledgment tracking per log index
  // Key: log index, Value: set of nodes that have acked at that level
  std::map<uint64_t, std::set<siteid_t>> memoryAcks_;   // track memory acks per index
  std::map<uint64_t, std::set<siteid_t>> durableAcks_;  // track durable acks per index

  // @unsafe - shared_ptr presence is stable after Setup/SetLogStorage.
  bool HasConfiguredStorage() const {
    return log_storage_ != nullptr;
  }

  // @unsafe - LogStorage ownership and open-state inspection are external.
  bool HasDurableStorage() const {
    return log_storage_ &&
           persistence_healthy_.load(
               rusty::sync::atomic::Ordering::Acquire) &&
           log_storage_->is_open();
  }

  // Caller must hold mtx_.  Re-evaluates securedLogIndex_ from already-recorded
  // acknowledgements, so advancement is independent of whether durability,
  // speculative quorum, or secured leadership arrived first.
  void MaybeAdvanceSecuredLogIndex();

  // @unsafe - Thread completion flag wrapping std::atomic<bool> for use with rusty::Arc.
  // Arc only provides const access, so the atomic must be mutable to allow store().
  // Uses C++ mutable for interior mutability (analogous to UnsafeCell in Rust).
  struct AtomicFlag {
    mutable std::atomic<bool> value{false}; // @unsafe { mutable field for interior mutability }
    explicit AtomicFlag(bool v) : value(v) {}
    void set(bool v, std::memory_order order = std::memory_order_release) const {
      value.store(v, order);
    }
    bool get(std::memory_order order = std::memory_order_acquire) const {
      return value.load(order);
    }
  };

  // @safe - Tracked async persistence threads (joined in destructor to prevent UAF)
  // Each entry pairs a thread with a completion flag. The lambda sets the flag to true
  // when done, allowing us to prune finished threads at each new insertion to prevent
  // unbounded growth of thread handles.
  std::mutex async_threads_mtx_;
  std::vector<std::pair<std::thread, rusty::Arc<AtomicFlag>>> async_threads_;
  // Tickets are allocated while mtx_ still serializes accepted AppendEntries.
  // Native workers wait for their exact ticket, proving that index N cannot
  // write/sync or advertise a prefix before every earlier accepted batch has
  // completed (or poisoned persistence_healthy_).
  rusty::sync::atomic::AtomicU64 next_log_persistence_ticket_{0};
  rusty::Mutex<uint64_t> serving_log_persistence_ticket_{0};
  rusty::Condvar log_persistence_ticket_cv_;

  // Client notification callbacks. The registration token lets a timed-out
  // owner remove only its own callback, even if the index is reused later.
  struct PendingCommitCallback {
    uint64_t token;
    std::function<void(CommitStatus)> callback;
  };

  // Key: log index, Value: uniquely owned callback registration.
  // Callbacks are invoked with: SPECULATIVE (memory quorum), DURABLE (disk quorum),
  // or ROLLEDBACK (leader stepped down gracefully)
  std::map<uint64_t, PendingCommitCallback> pendingCallbacks_;
  uint64_t nextCommitCallbackToken_ = 1;
  uint64_t lastSpecNotifiedIndex_ = 0;    // last index notified with SPECULATIVE
  uint64_t lastDurableNotifiedIndex_ = 0; // last index notified with DURABLE

  // Caller must hold mtx_. Returns a non-zero ownership token.
  // @unsafe - May invoke the supplied callback while mtx_ is held.
  uint64_t RegisterCommitCallbackLocked(
      uint64_t index, std::function<void(CommitStatus)> callback);

  // ============================================================================
  // MEMBERSHIP CONFIGURATION TRACKING
  // ============================================================================
  // Tracks the active set of replicas in this partition. Initialized from the
  // static partition config in Setup(), then modified by AddServer/RemoveServer.
  // All quorum calculations should use current_config_.size() instead of the
  // static Config::GetConfig()->GetPartitionSize().
  std::set<siteid_t> current_config_;          // Active replica set (site IDs)
  bool config_change_pending_ = false;         // True when a config entry is in-flight
  uint64_t pending_config_index_ = 0;          // Log index of pending config entry
  View current_view_{};                        // Last locally published leader view

  // ============================================================================
  // LEARNER / NEW SERVER CATCH-UP TRACKING
  // ============================================================================
  // Servers being caught up before joining the quorum. Learners receive log
  // entries via HeartbeatLoop (they are added to next_index_/match_index_)
  // but do NOT count towards quorum for commit index calculation.
  // Once a learner's match_index_ is within catchup_threshold_ of the
  // leader's lastLogIndex, it is promoted to a full member in current_config_.
  std::set<siteid_t> learners_;               // Servers being caught up (not yet in quorum)
  uint64_t catchup_threshold_ = 100;          // Entries within lastLogIndex to consider "caught up"

  // @unsafe - Locks mtx_ before reading the dynamically configurable
  // preferred-leader identity. The mutex is recursive because consensus paths
  // commonly call this helper while already holding mtx_.
  bool AmIPreferredLeader() {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    return raft_server_site_is_preferred_leader(
        site_id_, preferred_leader_site_id_,
        static_cast<uint16_t>(INVALID_SITEID));
  }

  // @safe - Check if I have caught up to the current leader's commit level
  bool HaveCaughtUp() const {
    // We've caught up if our commitIndex >= leader's last known commitIndex
    // Note: leader_last_commit_index_ is updated from AppendEntries heartbeats
    return raft_server_local_commit_has_caught_up(
        commitIndex, leader_last_commit_index_);
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
  // SPECULATIVE VOTING: Respond immediately, then persist and send VoteDurable async
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
              false, false, site_id_, can_id,
              static_cast<siteid_t>(INVALID_SITEID));
          currentTerm = can_term ;
          // @unsafe
          {
            vote_for_ = INVALID_SITEID;  // Reset vote when advancing to new term
          }

          // A higher term is stable state even when this RequestVote is denied.
          // Persist it before replying or admitting any work in the new term.
          const bool has_configured_storage = HasConfiguredStorage();
          const bool persistence_succeeded =
              !has_configured_storage ||
              PersistState(
                  currentTerm, vote_for_, "doVote: observed higher term");

          if (was_leader) {
            stepDown(StepDownReason::HigherTerm);
          } else {
            setIsLeader(false);
          }
          req_voting_ = false;
          election_in_progress_ = false;
          earlyDurableVoters_.clear();

          // Publish the newly observed term, never the pre-transition value.
          *reply_term = currentTerm;
          if (!raft_server_term_advance_is_durable(
                  has_configured_storage, persistence_succeeded)) {
            *vote_granted = false;
            rpc_ready_.store(
                false, rusty::sync::atomic::Ordering::Release);
            stop_.store(true, rusty::sync::atomic::Ordering::Release);
            looping_.store(false, rusty::sync::atomic::Ordering::Release);
            apply_thread_running_.store(false);
            Log_error("[RAFT-PERSISTENCE] Site {} could not durably record "
                      "RequestVote term {}; failing stop and voting NO",
                      site_id_, currentTerm);
            return;
          }
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

          if (async_persistence_ && HasConfiguredStorage()) {
            // SPECULATIVE VOTING (async mode): return NOW (memory vote), then
            // start async persistence and send VoteDurable after fsync. The
            // outer fiber replies automatically once this function returns.
            n_vote_++ ;

            // Capture necessary state for the async persistence thread.
            ballot_t term_copy = currentTerm;
            siteid_t voter_copy = site_id_;
            siteid_t can_id_copy = can_id;
            parid_t par_id_copy = partition_id_;

            // Install a non-joinable placeholder before reserving the ordered
            // ticket. Allocation can then fail without leaving a FIFO hole, and
            // the real thread is moved into already-owned storage noexcept.
            {
              std::lock_guard<std::mutex> lk(async_threads_mtx_);
              // Prune completed threads to prevent unbounded accumulation
              async_threads_.erase(
                std::remove_if(async_threads_.begin(), async_threads_.end(),
                  [](auto& entry) {
                    if (entry.second->get()) {
                      if (entry.first.joinable()) entry.first.join();
                      return true;
                    }
                    return false;
                  }),
                async_threads_.end());
              auto done = rusty::Arc<AtomicFlag>::make(false);
              async_threads_.emplace_back(std::thread{}, done);
              const uint64_t vote_persistence_ticket =
                  ReserveLogPersistenceTicketLocked();
              try {
                async_threads_.back().first = std::thread(
                  [this, term_copy, voter_copy, can_id_copy, par_id_copy,
                   vote_persistence_ticket, done]() {
                    try {
                      const bool vote_persisted = ExecuteLogPersistenceTicket(
                          vote_persistence_ticket,
                          "ordered async vote persistence",
                          [this, term_copy, can_id_copy]() {
                            return PersistTermAndVoteToLogStorage(
                                term_copy, can_id_copy);
                          });

                      // Revalidate only after the ticket completes. A newer
                      // term/vote or shutdown suppresses the durable proof.
                      if (vote_persisted) {
                        std::lock_guard<std::recursive_mutex> lock(mtx_);
                        if (!stop_.load(
                                rusty::sync::atomic::Ordering::Acquire) &&
                            !is_leader_ && currentTerm == term_copy &&
                            vote_for_ == can_id_copy && HasDurableStorage()) {
                          auto c = commo();
                          if (c != nullptr) {
                            c->SendVoteDurable(
                                can_id_copy, par_id_copy, term_copy, voter_copy);
                          }
                        }
                      }
                    } catch (const std::exception& error) {
                      Log_error("[RAFT-PERSISTENCE] Site {} async vote worker "
                                "threw after launch: {}", site_id_, error.what());
                    } catch (...) {
                      Log_error("[RAFT-PERSISTENCE] Site {} async vote worker "
                                "threw after launch", site_id_);
                    }
                    done->set(true);
                  });
              } catch (const std::exception& error) {
                ExecuteLogPersistenceTicket(
                    vote_persistence_ticket,
                    "async vote thread launch failure",
                    [this]() {
                      return RecordPersistenceResult(
                          false, "async vote thread launch failure");
                    });
                async_threads_.pop_back();
                Log_error("[RAFT-PERSISTENCE] Site {} could not launch async "
                          "vote worker: {}", site_id_, error.what());
              } catch (...) {
                ExecuteLogPersistenceTicket(
                    vote_persistence_ticket,
                    "async vote thread launch failure",
                    [this]() {
                      return RecordPersistenceResult(
                          false, "async vote thread launch failure");
                    });
                async_threads_.pop_back();
                Log_error("[RAFT-PERSISTENCE] Site {} could not launch async "
                          "vote worker", site_id_);
              }
            }

            return;
          } else {
            // SYNC PERSISTENCE (traditional Raft) persists before returning.
            // With persistence disabled PersistState is a no-op and the caller
            // classifies the vote as memory-only.
            const bool persistence_required = HasConfiguredStorage();
            const bool vote_persisted = PersistState(
                currentTerm, can_id, "doVote: sync vote persist");
            if (persistence_required && !vote_persisted) {
              // Vote replies have no acknowledgement-strength field. Returning
              // YES here would let a synchronous candidate count this failed
              // write as durable, so reject while retaining the local memory
              // vote for idempotence/safety.
              // @unsafe
              {
                *vote_granted = false;
              }
              Log_error("[RAFT-PERSISTENCE] Site {} suppressing sync vote YES "
                        "for candidate {} term {} after persistence failure",
                        site_id_, can_id, currentTerm);
            }
            n_vote_++ ;
            return;
          }
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

  // @unsafe - Uses LogStorage for persistence. False means no configured
  // durable boundary was crossed (disabled, unhealthy, or I/O failure).
  bool PersistState(uint64_t term, siteid_t voted_for,
                    const char* reason = "unspecified");

  // @unsafe - Uses LogStorage for persistence.
  bool PersistLogEntry(slotid_t slot_id, const RaftData& entry,
                       const char* reason = "unspecified");

  // @unsafe - Uses LogStorage for persistence.
  bool PersistCommitIndex(uint64_t commit_index,
                          const char* reason = "unspecified");

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
  // Must run before HeartbeatLoop starts (Setup and test Restart both do so).
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

  View GetCurrentView() const { return current_view_; }

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

  // Append and register compaction protection in one Raft critical section.
  // GetSubmissionProgress releases the registration after observing a
  // definitive commit or supersession.
  RaftStartResult StartTracked(const janus::Command& cmd,
                               uint64_t* index,
                               uint64_t* term,
                               slotid_t slot_id = -1,
                               ballot_t ballot = 1);

  // Atomically appends and installs the callback under mtx_, then publishes
  // replication after releasing the lock. callback_token receives a unique,
  // non-zero registration owner on success and zero when no definitive append
  // was admitted. The tri-state result distinguishes safe rejection from a
  // possibly durable local append.
  // @unsafe - Callback ownership, mutex operations, and output pointers.
  RaftStartResult StartWithCallback(
      const janus::Command& cmd,
      uint64_t* index,
      uint64_t* term,
      std::function<void(CommitStatus)> callback,
      uint64_t* callback_token = nullptr,
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

  // @unsafe - Reads log/commit/term state under the Raft mutex.
  RaftSubmissionProgress GetSubmissionProgress(
      uint64_t index, uint64_t appended_term);

  // @unsafe - Returns a consistent role/term/leader-hint snapshot.
  rusty::Arc<ViewData> GetCurrentViewData();

  // Publishes a partition-local recovery view into Raft's global leader hint.
  // Returns false for stale, malformed, or unmappable views without changing
  // Raft role state.
  bool ValidateRecoveryView(const ViewData& incoming_view_data,
                            bool allow_empty);
  bool ObserveRecoveryView(const ViewData& incoming_view_data);
  bool RecoveryOperationIsCurrent(epoch_t operation_epoch,
                                  const View& accepted_view);

  // @safe - returns POD field
  uint64_t GetHeartbeatInterval() const { return heartbeat_interval_us_; }

  // @safe - sets POD field
  void SetHeartbeatInterval(uint64_t micros) { heartbeat_interval_us_ = micros; }

  // @unsafe - Implements ReadIndex protocol for linearizable reads.
  // Returns true if this server is confirmed leader and safe to serve reads.
  // Waits for executeIndex to catch up to commitIndex.
  bool ReadIndex(uint64_t timeout_us = 5000000);

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
    std::lock_guard<std::recursive_mutex> lock(mtx_);
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

    // CRITICAL: Persist log entry before replicating to followers
    // @unsafe
    {
      const bool local_entry_persisted = PersistLogEntry(
          lastLogIndex, *instance, "SetLocalAppend: leader log");
      if (HasConfiguredStorage() && !local_entry_persisted) {
        const slotid_t failed_index = lastLogIndex;
        const ballot_t failed_term = instance->term;
        const bool durable_absence_proven =
            log_storage_ != nullptr &&
            raft_server_compensating_remove_and_sync(
                *log_storage_, failed_index);
        const StepDownReason failure_reason = securedLeader_
            ? StepDownReason::SecuredFailure
            : StepDownReason::UnsecuredFailure;
        // Preserve the central transition: existing pending submissions must
        // receive terminal rollback notification and observers must see the
        // leader-change callback before RPC admission closes.
        stepDown(failure_reason);
        raft_logs_.erase(failed_index);
        durableAcks_.erase(failed_index);
        memoryAcks_.erase(failed_index);
        --lastLogIndex;
        // Nonzero outputs identify the potentially persisted slot. Zero means
        // the compensating durable removal proved that a normal retry cannot
        // resurrect this command.
        *index = durable_absence_proven ? 0 : failed_index;
        *term = durable_absence_proven ? 0 : failed_term;
        rpc_ready_.store(false, rusty::sync::atomic::Ordering::Release);
        stop_.store(true, rusty::sync::atomic::Ordering::Release);
        looping_.store(false, rusty::sync::atomic::Ordering::Release);
        apply_thread_running_.store(false);
        Log_error("[RAFT-PERSISTENCE] Site {} could not durably append "
                  "leader slot {}; compensating_absence={} and failing stop",
                  site_id_, failed_index, durable_absence_proven);
        return durable_absence_proven
            ? RaftStartResult::REJECTED
            : RaftStartResult::INDETERMINATE;
      }
      if (is_leader_ && local_entry_persisted &&
          raft_server_persistence_can_report_durable(HasDurableStorage())) {
        // The leader's local append crosses its persistence boundary before
        // replication begins, so it is one member of the durable quorum.
        durableAcks_[lastLogIndex].insert(site_id_);
      }
    }

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
  // LOG PERSISTENCE PUBLIC API
  // ============================================================================

  /**
   * Set the log storage backend for persistence.
   * Should be called before starting the server.
   * @param storage Shared pointer to LogStorage implementation
   */
  // @unsafe - moves shared_ptr into member field
  void SetLogStorage(std::shared_ptr<janus::raft::LogStorage> storage) {
    log_storage_ = std::move(storage);
    persistence_healthy_.store(
        true, rusty::sync::atomic::Ordering::Release);
  }

  /**
   * Get the current log storage backend.
   * @return Shared pointer to LogStorage, or nullptr if not set
   */
  // @unsafe - returns copy of shared_ptr
  std::shared_ptr<janus::raft::LogStorage> GetLogStorage() const {
    return log_storage_;
  }

  /**
   * Recover state from persistent storage.
   * Restores currentTerm, vote_for_, commitIndex, and log entries.
   * Should be called during initialization if storage is available.
   * @return true if recovery succeeded or storage is not set, false on error
   */
  // @safe - recovery from storage (external calls wrapped in @unsafe blocks)
  bool RecoverFromStorage();

  /**
   * Replay committed entries after recovery.
   * Called after app_next_ callback is registered to apply recovered entries.
   * Must be called AFTER RegLearnerAction() sets up the callback.
   */
  // @safe - replays committed entries (callbacks wrapped in @unsafe blocks)
  bool ReplayCommittedEntries();

  /**
   * Get count of uncommitted entries after recovery.
   * These entries will be resolved by the consensus protocol.
   * @return Number of uncommitted entries (lastLogIndex - commitIndex)
   */
  // @safe - Read-only accessor
  size_t GetUncommittedCount() const;

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
   * Get the ReplicatedDB instance, if one was created during Setup().
   * @return Shared pointer to ReplicatedDB, or nullptr if not enabled
   */
  // @unsafe - returns copy of shared_ptr
  std::shared_ptr<ReplicatedDB> GetReplicatedDB() const {
    return replicated_db_;
  }

  /**
   * Set state machine snapshot callbacks.
   * Called by ReplicatedDB (or other state machines) to hook into
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
   * Removes entries from storage that are covered by a snapshot.
   * @param up_to_index Remove entries with index <= this value
   * @return Number of entries removed
   */
  // @safe - log compaction (storage operations wrapped in @unsafe blocks)
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

  /**
   * VoteDurable RPC Handler - Speculative Voting Protocol
   *
   * Receives VoteDurable RPC from a follower after it has durably persisted
   * its vote to disk. This allows the leader to track durable votes separately
   * from memory votes, enabling speculative leader election.
   *
   * @param term - Term of the vote (must match current term)
   * @param voter_id - Site ID of the voter
   * @param acknowledged - [OUT] true if vote was recorded
   * @param cb - Callback to invoke when handling complete
   */
  // @unsafe - Modifies durableVoters_ and securedLeader_
  void OnVoteDurable(const ballot_t& term,
                     const siteid_t& voter_id,
                     bool_t* acknowledged);

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
                       uint64_t *followerLastLogIndex,
                       uint64_t *followerAckType,
                       bool trigger_election_now = false);

  /**
   * AppendEntriesDurable RPC Handler - Speculative Commit Protocol
   *
   * Receives AppendEntriesDurable RPC from a follower after it has durably
   * persisted log entries to disk. This allows the leader to track durable
   * acknowledgments separately from memory acks, enabling speculative commits.
   *
   * @param term - Term when entries were persisted (must match current term)
   * @param follower_id - Site ID of the follower
   * @param lastLogIndex - Highest log index that is now durable on follower
   * @param acknowledged - [OUT] true if ack was recorded
   * @param cb - Callback to invoke when handling complete
   */
  // @unsafe - Modifies durableAcks_ and securedLogIndex_
  void OnAppendEntriesDurable(const ballot_t& term,
                              const siteid_t& follower_id,
                              const uint64_t& lastLogIndex,
                              bool_t* acknowledged);

  /**
   * TimeoutNow RPC Handler - Leadership Transfer Protocol
   *
   * Receives TimeoutNow RPC from current leader instructing this replica
   * to start an election immediately (bypass random election timeout).
   *
   * Used for deterministic leadership transfer to preferred replica.
   *
   * @param leaderTerm - Current leader's term
   * @param leaderSiteId - Current leader's site ID
   * @param followerTerm - [OUT] This replica's current term
   * @param success - [OUT] true if election started, false otherwise
   * @param cb - Callback to invoke when handling complete
   */
  
  // @safe - external calls marked @external, output pointer writes in @unsafe blocks
  void OnTimeoutNow(const uint64_t leaderTerm,
                    const siteid_t leaderSiteId,
                    uint64_t *followerTerm,
                    bool_t *success);

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
  // MEMBERSHIP CHANGE PUBLIC API
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

  /**
   * Check if a server is a learner (being caught up, not yet in quorum).
   */
  // @unsafe - Read-only lookup on std::set
  bool IsLearner(siteid_t id) const { return learners_.count(id) > 0; }

  /**
   * Get the current set of learners.
   */
  // @unsafe - returns reference to internal state (no @lifetime annotation)
  const std::set<siteid_t>& GetLearners() const { return learners_; }

  /**
   * Promote a learner to full member in current_config_.
   * Removes from learners_, inserts into current_config_, clears pending flag.
   */
  // @unsafe - Modifies config state
  void PromoteLearner(siteid_t id);

  /**
   * Check if any learners are caught up and promote them to full members.
   * Called from HeartbeatLoop after commit index calculation.
   */
  // @unsafe - Calls PromoteLearner which modifies config state
  void CheckAndPromoteLearners();

  /**
   * AddServer RPC Handler - Membership Change Protocol
   *
   * Adds a new server to the cluster configuration. Only the leader can
   * process this request. Rejects if a config change is already pending.
   * The server is first added as a learner (receives log entries but does not
   * count for quorum). Once caught up, it is promoted to full member.
   *
   * @param term - Client's known term
   * @param new_server_id - Site ID of the server to add
   * @param addr - Address of the new server (host:port)
   * @param success - [OUT] true if config change was accepted
   * @param error_msg - [OUT] error description if rejected
   * @param leader_hint - [OUT] current leader's site ID (for redirect)
   * @param defer - Deferred reply
   */
  // @unsafe - Modifies config state
  void OnAddServer(const uint64_t term, const uint64_t new_server_id,
                   const std::string& addr,
                   bool_t* success, std::string* error_msg,
                   uint64_t* leader_hint);

  /**
   * RemoveServer RPC Handler - Membership Change Protocol
   *
   * Removes a server from the cluster configuration. Only the leader can
   * process this request. Rejects if a config change is already pending,
   * or if this would remove the last server.
   *
   * @param term - Client's known term
   * @param server_id - Site ID of the server to remove
   * @param success - [OUT] true if config change was accepted
   * @param error_msg - [OUT] error description if rejected
   * @param leader_hint - [OUT] current leader's site ID (for redirect)
   * @param defer - Deferred reply
   */
  // @unsafe - Modifies config state
  void OnRemoveServer(const uint64_t term, const uint64_t server_id,
                      bool_t* success, std::string* error_msg,
                      uint64_t* leader_hint);

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
  // PUBLIC API: Preferred Replica System - Leadership Transfer
  // ============================================================================

  /**
   * Set the preferred leader for this Raft group.
   *
   * @param site_id The site ID of the preferred leader (or INVALID_SITEID to disable)
   *
   * Behavior:
   * - All replicas should call this with the same site_id
   * - Standard Raft voting happens (any replica can win initial election)
   * - Non-preferred leaders monitor for preferred replica
   * - When preferred is alive and caught up, non-preferred leader transfers leadership
   *
   * Safety: This maintains all Raft safety guarantees via explicit transfer protocol.
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

    // If I'm a non-preferred leader, start monitoring for transfer opportunity
    if (raft_server_leadership_monitor_should_start(
            AmIPreferredLeader(), is_leader_,
            looping_.load(rusty::sync::atomic::Ordering::Acquire))) {
      Log_info("[LEADERSHIP-TRANSFER] Site {}: I'm non-preferred leader, starting transfer monitoring",
               site_id_);
      StartLeadershipTransferMonitoring();
    }
  }

  /**
   * Get the current preferred leader site ID
   * @return Preferred leader site ID, or INVALID_SITEID if none
   */
  // @unsafe - Locks mtx_ before reading the dynamically configurable identity.
  siteid_t GetPreferredLeader() {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    return preferred_leader_site_id_;
  }

  /**
   * Get the last known leader's site_id for client redirection.
   * @return Leader site_id, or INVALID_SITEID if unknown
   */
  // @unsafe - Locks mtx_ before reading role/leader identity.
  siteid_t GetLeaderHint();

  /**
   * Check if leadership transfer should be initiated
   * Called by non-preferred leaders to check if preferred replica is ready
   */
  // @safe - checks conditions for leadership transfer (mutex/map access via @unsafe blocks)
  bool ShouldTransferLeadership();

  // @safe - initiates leadership transfer (RPC/mutex via @unsafe blocks)
  void InitiateLeadershipTransfer();

  // @unsafe - Starts one persistent Rusty monitor thread.
  void StartLeadershipTransferMonitoring();

  // @unsafe - Final thread join barrier; must be called without holding mtx_.
  void StopLeadershipTransferMonitoring();

  // ============================================================================
  // PUBLIC API: Speculative Replication State
  // ============================================================================

  /**
   * Check if this leader has achieved secured status (durable vote quorum).
   * When securedLeader_ = true, a quorum has votedFor = me on disk,
   * so no other candidate can win election in this term.
   * @return true if leader has durable vote quorum
   */
  // @unsafe - Locks mtx_ before reading securedLeader_, then reads the external
  // LogStorage open state. The mutex is recursive for consensus-path callers.
  bool IsSecuredLeader() {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    return securedLeader_ && HasDurableStorage();
  }

  /**
   * Get the speculative commit index (highest index with memory ack quorum).
   * @return specCommitIndex value
   */
  // @safe - Read-only accessor
  uint64_t GetSpecCommitIndex() const {
    return specCommitIndex_;
  }

  /**
   * Get the secured log index (highest index with durable ack quorum).
   * Invariant: securedLogIndex <= specCommitIndex <= lastLogIndex
   * @return securedLogIndex value
   */
  // @safe - Read-only accessor
  uint64_t GetSecuredLogIndex() const {
    return securedLogIndex_;
  }

  /**
   * Get the set of servers that have memory-voted for us in current term.
   * @return copy of specVoters set
   */
  // @unsafe - Returns copy, read-only access
  std::set<siteid_t> GetSpecVoters() const {
    return specVoters_;
  }

  /**
   * Get the count of servers that have memory-voted for us in current term.
   * @return Number of servers in specVoters
   */
  // @unsafe - Read-only accessor on std::set
  size_t GetSpecVotersCount() const {
    return specVoters_.size();
  }

  /**
   * Get the set of servers that have durably-voted for us in current term.
   * @return copy of durableVoters set
   */
  // @unsafe - Returns copy, read-only access
  std::set<siteid_t> GetDurableVoters() const {
    return durableVoters_;
  }

  /**
   * Get the count of servers that have durably-voted for us in current term.
   * @return Number of servers in durableVoters
   */
  // @unsafe - Read-only accessor on std::set
  size_t GetDurableVotersCount() const {
    return durableVoters_.size();
  }

  /**
   * Get the last log index.
   * @return lastLogIndex value
   */
  // @safe - Read-only accessor
  uint64_t GetLastLogIndex() const {
    return lastLogIndex;
  }

  /**
   * Get the number of memory acks for a specific log index.
   * @param index Log index to query
   * @return Number of nodes that have memory-acked this index
   */
  // @unsafe - Read-only accessor on std::map
  size_t GetMemoryAckCount(uint64_t index) const {
    auto it = memoryAcks_.find(index);
    return it != memoryAcks_.end() ? it->second.size() : 0;
  }

  /**
   * Get the number of durable acks for a specific log index.
   * @param index Log index to query
   * @return Number of nodes that have durably-acked this index
   */
  // @unsafe - Read-only accessor on std::map
  size_t GetDurableAckCount(uint64_t index) const {
    auto it = durableAcks_.find(index);
    return it != durableAcks_.end() ? it->second.size() : 0;
  }

  /**
   * Reset speculative state when becoming leader or stepping down.
   * Called during leadership transitions.
   *
   * On becoming leader:
   * - specVoters = {self}  (voted for self)
   * - durableVoters = {self} only when local persistence is enabled
   * - securedLogIndex = commitIndex (from previous term)
   * - specCommitIndex = commitIndex
   *
   * On stepping down:
   * - All speculative state is cleared
   */
  // @unsafe - Modifies state
  void ResetSpeculativeState();

  /**
   * Verify speculative state invariants.
   * Debug helper - asserts if invariants are violated.
   * Invariants:
   * - securedLogIndex <= specCommitIndex <= lastLogIndex
   * - durableVoters ⊆ specVoters (conceptually, not strictly enforced after crashes)
   */
  // @safe - Read-only check
  void VerifySpeculativeInvariants() const;

  /**
   * Handle notification that a peer has restarted.
   *
   * Called when we receive notifyRestart from another server. For speculative
   * replication, this means the restarted server has lost:
   * 1. Its memory vote (if it voted but didn't fsync)
   * 2. Memory-acked log entries (not yet fsynced)
   *
   * This method invalidates any speculative state that depended on the
   * restarted server and may trigger step-down if we're an unsecured leader
   * who has lost speculative quorum.
   *
   * @param restarted_site_id - Site ID of the server that restarted
   */
  // @unsafe - Modifies speculative state
  void OnPeerRestart(siteid_t restarted_site_id);

  /**
   * Step down as leader with specified reason.
   *
   * This is the central function for leader step-down in speculative Raft.
   * It handles:
   * 1. Logging the step-down event with reason
   * 2. Resetting speculative state
   * 3. Transitioning to follower state
   * 4. Resetting election timer
   *
   * Future: Will also notify pending clients based on reason:
   * - UnsecuredFailure: Rollback all current-term entries
   * - SecuredFailure: Rollback only unsecured entries
   * - HigherTerm: No automatic rollback (entries may still be valid)
   *
   * @param reason - Why the leader is stepping down
   */
  // @unsafe - Modifies state, calls setIsLeader
  void stepDown(StepDownReason reason);

  // ===========================================================================
  // CLIENT NOTIFICATION CALLBACKS
  // ===========================================================================

  /**
   * Register a callback to be notified when an entry's commit status changes.
   *
   * The callback will be invoked with:
   * - SPECULATIVE: When entry reaches memory quorum (specCommitIndex advances)
   * - DURABLE: When entry reaches disk quorum with secured leader
   * - ROLLEDBACK: If leader steps down gracefully (best-effort)
   *
   * Note: Callback is invoked while holding mtx_, keep it lightweight.
   * If index is already at or past the requested state, callback is invoked
   * immediately.
   *
   * @param index - Log index to monitor
   * @param callback - Function to call on status change
   */
  // @unsafe - Modifies pendingCallbacks_. Returns a unique, non-zero token.
  uint64_t RegisterCommitCallback(
      uint64_t index, std::function<void(CommitStatus)> callback);

  /**
   * Remove a callback only if both its index and ownership token match.
   * This is safe to call after notification; it simply returns false when the
   * registration is already gone.
   */
  // @unsafe - Locks mtx_ and modifies pendingCallbacks_.
  bool UnregisterCommitCallback(uint64_t index, uint64_t callback_token);

  /**
   * Notify all registered callbacks for indices in range (from, to] with status.
   * Used internally by specCommitIndex/securedLogIndex advancement handlers.
   *
   * @param from - Exclusive lower bound
   * @param to - Inclusive upper bound
   * @param status - Commit status to notify
   */
  // @unsafe - Invokes callbacks, modifies pendingCallbacks_
  void NotifyCallbacks(uint64_t from, uint64_t to, CommitStatus status);

  /**
   * Notify rollback for pending callbacks based on step-down reason.
   * Called during step-down when leader is still alive.
   *
   * Behavior per reason:
   * - UnsecuredFailure: Rollback every pending entry in
   *   (securedLogIndex_, lastLogIndex]
   * - SecuredFailure: Rollback every pending entry in
   *   (securedLogIndex_, lastLogIndex]
   * - HigherTerm: No automatic rollback (entries may still be valid under new leader)
   *
   * Always clears pendingCallbacks_ and resets notification tracking regardless of reason.
   *
   * @param reason - Why the leader is stepping down
   */
  // @unsafe - Invokes callbacks, clears pendingCallbacks_
  void NotifyRollback(StepDownReason reason);
};
} // namespace janus
