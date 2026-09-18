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
#include <thread>
#include <atomic>
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
#include "rust_log_shims.h"
#include <rusty/rusty.hpp>   // rusty::to_string_view, emitted for &str parameters
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
    // THIS SERVER'S IDENTITY, MIRRORED.
    //
    // site_id_, partition_id_ and loc_id_ also exist on RaftServer, where
    // TXLOG_SERVER_SITE_FIELDS() puts them (src/deptran/scheduler.h:136).
    // That macro is SHARED WITH PAXOS, so the fields cannot simply move; a
    // converted Rust body needs them and reaching back out to the C++ object
    // for a scalar would defeat the point.
    //
    // They are written exactly once, by RaftServer::set_site_identity, which
    // sets both copies together and then verifies they agree. All three are
    // immutable afterwards, so the two copies cannot drift -- but the
    // assertion is there because "cannot drift" is an argument, and this is
    // the kind of argument that stops being true when someone adds a setter.
    //
    // TODO(txlog-site-fields): remove the mirror by unpacking
    // TXLOG_SERVER_SITE_FIELDS() for both engines, so Raft and Paxos each own
    // their identity fields outright and Raft's can live only here. That is a
    // change to Paxos's contract, which is why it is not done in passing.
    // Before removing, check that PaxosServer still compiles against whatever
    // replaces the macro.
    pub site_id_: u16,
    pub partition_id_: u32,
    pub loc_id_: u32,
    // THE LOG AND THE PEERS LIVE HERE NOW, not beside the mutex.
    //
    // This is what makes a converted method body a one-line delegate instead
    // of a marshalling shim. PHASE 0, 2 and 3 each needed an outcome struct
    // and a switch on the C++ side purely because the state they decide over
    // was split across three members, so a Rust function could compute an
    // answer but not finish the job. With the state in one place a body can
    // be moved wholesale and the C++ that remains is `raft_foo(state_);`.
    pub raft_log_: RaftLog,
    pub peers_: PeerTable,
    // Election cluster.
    election_term_: i64,
    election_timeout_us_: u64,
    election_timer_generation_: u64,
    pub vote_for_: u16,
    // Snapshot configuration and callback ownership.
    snapshot_threshold_: u64,
    snapshot_callback_owner_token_: u64,
    next_snapshot_callback_owner_token_: u64,
    // Leadership, and the campaign in progress.
    pub is_leader_: bool,
    pub req_voting_: bool,
    pub election_in_progress_: bool,
    pub current_leader_id_: u16,
    last_heartbeat_time_: u64,
    // Read-index evidence: the round counter and the newest confirmed proof.
    pub heartbeat_round_: u64,
    pub read_quorum_confirmed_term_: u64,
    pub read_quorum_confirmed_round_: u64,
    // Log store: the term the server is in, and the three indices that bound
    // the log. NOTE the historical naming -- these four are the only members
    // in the class without a trailing underscore.
    pub current_term_: u64,
    pub commit_index_: u64,
    pub execute_index_: u64,
    // Snapshot boundary.
    pub snapidx_: u64,
    pub snapterm_: i64,
}

#[allow(clippy::new_without_default)]
impl RaftConsensusState {
    pub fn new() -> RaftConsensusState {
        RaftConsensusState {
            // Overwritten by set_site_identity before anything reads them.
            site_id_: u16::MAX,
            partition_id_: 0,
            loc_id_: u32::MAX,
            raft_log_: RaftLog::new(),
            peers_: PeerTable::new(),
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
/*RUSTYCPP:GEN-BEGIN id=raft_server.consensus_state version=1 rust_sha256=cd68c26e42b48a077fca92b6c6c297a9b56a1a900752cb44d21f7763788a083e*/
struct RaftConsensusState;

struct RaftConsensusState {
    uint16_t site_id_;
    uint32_t partition_id_;
    uint32_t loc_id_;
    RaftLog raft_log_;
    PeerTable peers_;
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
};


inline RaftConsensusState RaftConsensusState::new_() {
    return RaftConsensusState{.site_id_ = std::numeric_limits<uint16_t>::max(), .partition_id_ = static_cast<uint32_t>(0), .loc_id_ = std::numeric_limits<uint32_t>::max(), .raft_log_ = RaftLog::new_(), .peers_ = PeerTable::new_(), .election_term_ = static_cast<int64_t>(0), .election_timeout_us_ = static_cast<uint64_t>(0), .election_timer_generation_ = static_cast<uint64_t>(0), .vote_for_ = std::numeric_limits<uint16_t>::max(), .snapshot_threshold_ = static_cast<uint64_t>(10000), .snapshot_callback_owner_token_ = static_cast<uint64_t>(0), .next_snapshot_callback_owner_token_ = static_cast<uint64_t>(1), .is_leader_ = false, .req_voting_ = false, .election_in_progress_ = false, .current_leader_id_ = std::numeric_limits<uint16_t>::max(), .last_heartbeat_time_ = static_cast<uint64_t>(0), .heartbeat_round_ = static_cast<uint64_t>(0), .read_quorum_confirmed_term_ = static_cast<uint64_t>(0), .read_quorum_confirmed_round_ = static_cast<uint64_t>(0), .current_term_ = static_cast<uint64_t>(0), .commit_index_ = static_cast<uint64_t>(0), .execute_index_ = static_cast<uint64_t>(0), .snapidx_ = static_cast<uint64_t>(0), .snapterm_ = static_cast<int64_t>(0)};
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

// A std::mutex that remembers which thread holds it, so re-entering it
// aborts with a message instead of hanging.
//
// WHY THIS IS WORTH AN ATOMIC PER ACQUISITION. mtx_ was a recursive_mutex
// until the Tranche 5 demotion. Re-entry used to be legal; now it is a
// self-deadlock -- the thread waits for a lock only it can release. Every
// path INSIDE RaftServer was checked then, and a static walk still finds no
// function holding mtx_ that reaches another taking it.
//
// The gap is the two application-provided callbacks. CreateSnapshotLocked
// and PrepareStateMachineSnapshotLocked invoke embedder code with mtx_
// held, so a callback that calls back into this server deadlocks. No test
// can catch it: the only in-tree callbacks are pure. Without this, the
// symptom is a silent hang with no stack and no log line. With it, the
// symptom names the bug.
//
// Satisfies BasicLockable and Lockable, so std::lock_guard and
// std::unique_lock work on it unchanged.
class RaftCheckedMutex {
 public:
  void lock() {
    const std::thread::id self = std::this_thread::get_id();
    if (owner_.load(std::memory_order_relaxed) == self) {
      ReportReentry();
    }
    inner_.lock();
    owner_.store(self, std::memory_order_relaxed);
  }

  bool try_lock() {
    const std::thread::id self = std::this_thread::get_id();
    if (owner_.load(std::memory_order_relaxed) == self) {
      ReportReentry();
    }
    if (!inner_.try_lock()) {
      return false;
    }
    owner_.store(self, std::memory_order_relaxed);
    return true;
  }

  void unlock() {
    owner_.store(std::thread::id{}, std::memory_order_relaxed);
    inner_.unlock();
  }

 private:
  // @unsafe { writes to stderr and aborts }
  [[noreturn]] static void ReportReentry();

  std::mutex inner_{};
  std::atomic<std::thread::id> owner_{};
};

// If thread::id is not lock-free, std::atomic<> falls back to an internal
// lock and this check would cost far more than intended. Fail the build
// rather than quietly pay for it.
static_assert(std::atomic<std::thread::id>::is_always_lock_free,
              "RaftCheckedMutex assumes a lock-free atomic<thread::id>");

// Lifted out of RaftServer ahead of the struct conversion: a DSL struct
// cannot declare a nested type, and these two are plain data with no reason
// to be nested. AsyncCallbackLifetime keeps a back-pointer, so RaftServer is
// forward-declared above it.
class RaftServer;

// RPC futures can outlive the server during shutdown. Destruction nulls this
// shared gate after waiting for any callback already using it.
struct AsyncCallbackLifetime {
  std::mutex mutex;
  RaftServer* server = nullptr;
};

// One entry waiting for the apply thread. A conflicting snapshot increments
// the epoch so an entry popped before queue invalidation cannot apply after.
struct QueuedApplyEntry {
  slotid_t index = 0;
  Command command{};
  uint64_t epoch = 0;
};


// ---------------------------------------------------------------------------
// C++ spellings for RaftServerBase's field types.
//
// Inline mode has no --type-map, so a foreign type reaches C++ under the exact
// path the Rust spells -- the mechanism src/deptran/raft/rust_facade_types.h
// documents at length. These pairs belong in that header and cannot go there:
// every one aliases a type server.h itself declares, and rust_facade_types.h
// is included before those declarations exist.
//
// The Rust half is src/rrr/rusty-rustc/src/lib.rs, where each name is a
// zero-sized opaque struct. Rust can hold, move and default-construct one;
// it cannot look inside. Each alias below is EXACTLY the type the
// hand-written member used, so this changes where the members are declared
// and nothing else about them.
// ---------------------------------------------------------------------------
}  // namespace janus

namespace rusty {
using RaftCheckedMutex = ::janus::RaftCheckedMutex;
using RaftDecodedTerms = ::std::vector<int64_t>;
using RaftAsyncCallbackLifetimePtr =
    ::std::shared_ptr<::janus::AsyncCallbackLifetime>;
using RaftSnapshotManagerPtr =
    ::std::shared_ptr<::janus::raft::SnapshotManager>;
using RaftCreateSnapshotCb = ::std::function<::std::string(uint64_t)>;
using RaftPrepareSnapshotCb = ::std::function<
    ::std::unique_ptr<::janus::PreparedStateMachineSnapshotInstall>(
        const ::std::string&, uint64_t)>;
using RaftPeerSites = ::std::vector<siteid_t>;
using RaftStdMutex = ::std::mutex;
using RaftStdCondVar = ::std::condition_variable;
using RaftLeaderChangeCb = ::std::function<void(bool)>;
using RaftSiteIdSet = ::std::set<siteid_t>;
using RaftStdThread = ::std::thread;
using RaftApplyQueue = ::std::deque<::janus::QueuedApplyEntry>;
using RaftVoteQuorumPtr = ::std::shared_ptr<::janus::RaftVoteQuorumEvent>;
}  // namespace rusty

// The DSL block below names the interface as `crate::scheduler_h::TxLogServer`
// -- the Rust module path the raft crate extracts src/deptran/scheduler.h to
// -- so the emitted C++ is `::scheduler_h::TxLogServer`. This is the same shim
// server.cc already opens for its own cross-carrier references.
namespace scheduler_h {
using ::janus::TxLogServer;
}  // namespace scheduler_h

namespace janus {

// @unsafe - wraps the rrr `verify` macro so a DSL body can assert. Declared
// extern "C" because that is the one function-declaration form a DSL block
// can spell and rustc can resolve without a Rust definition behind it.
extern "C" inline void raft_verify(bool condition) { verify(condition); }

// @unsafe - the two halves of std::lock_guard<RaftCheckedMutex>, so a DSL
// body can hold mtx_ across a scope. mtx_ is opaque to Rust; these are the
// only operations on it a converted body performs.
extern "C" inline void raft_mutex_lock(RaftCheckedMutex* mutex) {
  mutex->lock();
}
extern "C" inline void raft_mutex_unlock(RaftCheckedMutex* mutex) {
  mutex->unlock();
}

// @unsafe - the same two halves for a plain std::mutex. RaftServer has four
// (startup_mtx_, state_machine_apply_mtx_, apply_queue_mtx_) that a DSL body
// has to hold across a scope.
extern "C" inline void raft_std_mutex_lock(std::mutex* mutex) {
  mutex->lock();
}
extern "C" inline void raft_std_mutex_unlock(std::mutex* mutex) {
  mutex->unlock();
}

// @unsafe - Time::now is an rrr clock read; the argument is the
// microsecond-resolution flag every Raft call site already passes.
extern "C" inline uint64_t raft_time_now_us() { return Time::now(true); }

// @unsafe - shared_ptr null test. The pointer itself is opaque to Rust, so
// "is a snapshot manager configured" has to be asked here.
extern "C" inline bool raft_snapshot_manager_is_set(
    const rusty::RaftSnapshotManagerPtr* manager) {
  return *manager != nullptr;
}

// @unsafe - RandomGenerator is external.
extern "C" inline uint64_t raft_random_range_us(uint64_t low, uint64_t high) {
  return RandomGenerator::rand(low, high);
}

// @safe - RAFT_LEADER_ELECTION_DEBUG, as a value a DSL body can branch on.
//
// #[cfg] is dropped silently inside a DSL block, so an #ifdef-guarded log
// cannot be written there directly. Returning the flag instead keeps the log
// AT its call site, where its arguments are, and the compiler folds the
// branch away exactly as the preprocessor did -- the arguments are all
// scalars already in registers, so there is nothing else the #ifdef was
// saving.
extern "C" inline bool raft_election_debug_enabled() {
#ifdef RAFT_LEADER_ELECTION_DEBUG
  return true;
#else
  return false;
#endif
}

// ============================================================================
// RaftLockGuard -- std::lock_guard<RaftCheckedMutex>, for converted bodies.
//
// mtx_ is an opaque carrier on the Rust side, so a DSL body cannot call
// lock() on it. This guard is the one piece of machinery every converted
// RaftServer method needs: it acquires in `new`, releases in Drop, and so
// reproduces the exact scope the C++ std::lock_guard had -- including early
// returns. That is what lets a body be translated statement by statement
// rather than restructured, which is the difference between a translation
// that can be reviewed against the original and one that cannot.
//
// A separate block from RaftServerBase on purpose: `impl Drop` makes the
// emitter add a move constructor, which RaftServerBase (std::mutex,
// std::condition_variable) could not compile.
// ============================================================================
#if RUSTYCPP_RUST
unsafe extern "C" {
    fn raft_mutex_lock(mutex: *mut rusty::RaftCheckedMutex);
    fn raft_mutex_unlock(mutex: *mut rusty::RaftCheckedMutex);
    fn raft_std_mutex_lock(mutex: *mut rusty::RaftStdMutex);
    fn raft_std_mutex_unlock(mutex: *mut rusty::RaftStdMutex);
}

pub struct RaftLockGuard {
    mutex_: *mut rusty::RaftCheckedMutex,
}

impl RaftLockGuard {
    // A raw pointer rather than `&mut RaftCheckedMutex`, because the emitter
    // renders the argument `&mut self.mtx_` as `&this->mtx_` either way, and
    // a reference parameter would then not bind. The pointer is not a
    // widening of the contract: every call site in this file passes
    // `&mut self.mtx_`, a field of the live object whose method is running,
    // and the borrow ends with the constructing statement -- which is
    // precisely what lets the body go on using `self` while the lock is held.
    #[allow(clippy::not_unsafe_ptr_arg_deref)]
    pub fn new(mutex: *mut rusty::RaftCheckedMutex) -> RaftLockGuard {
        unsafe {
            raft_mutex_lock(mutex);
        }
        RaftLockGuard { mutex_: mutex }
    }
}

impl Drop for RaftLockGuard {
    fn drop(&mut self) {
        unsafe {
            raft_mutex_unlock(self.mutex_);
        }
    }
}

// The same, for the three plain std::mutex members. Separate because they
// are a different C++ type, not because the discipline differs.
pub struct RaftStdLockGuard {
    mutex_: *mut rusty::RaftStdMutex,
}

impl RaftStdLockGuard {
    #[allow(clippy::not_unsafe_ptr_arg_deref)]
    pub fn new(mutex: *mut rusty::RaftStdMutex) -> RaftStdLockGuard {
        unsafe {
            raft_std_mutex_lock(mutex);
        }
        RaftStdLockGuard { mutex_: mutex }
    }
}

impl Drop for RaftStdLockGuard {
    fn drop(&mut self) {
        unsafe {
            raft_std_mutex_unlock(self.mutex_);
        }
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=raft_server.lock_guard version=1 rust_sha256=68e3a5b5408cb910c432764fa92a1972f4429593acf26a16cddf4a4b209ed106*/
struct RaftLockGuard;
struct RaftStdLockGuard;

extern "C" {
    void raft_mutex_lock(rusty::RaftCheckedMutex* mutex);
    void raft_mutex_unlock(rusty::RaftCheckedMutex* mutex);
    void raft_std_mutex_lock(rusty::RaftStdMutex* mutex);
    void raft_std_mutex_unlock(rusty::RaftStdMutex* mutex);
}

struct RaftLockGuard {
    rusty::RaftCheckedMutex* mutex_;
    mutable bool _rusty_forgotten = false;
    RaftLockGuard(rusty::RaftCheckedMutex* mutex__init) : mutex_(std::move(mutex__init)) {}
    RaftLockGuard(const RaftLockGuard&) = delete;
    RaftLockGuard(RaftLockGuard&& other) noexcept : mutex_(std::move(other.mutex_)) {
        this->_rusty_forgotten = other._rusty_forgotten;
        other._rusty_forgotten = true;
    }
    RaftLockGuard& operator=(const RaftLockGuard&) = delete;
    RaftLockGuard& operator=(RaftLockGuard&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        this->~RaftLockGuard();
        new (this) RaftLockGuard(std::move(other));
        return *this;
    }
    void rusty_mark_forgotten() const noexcept { _rusty_forgotten = true; rusty::detail::mark_forgotten_if_supported(this->mutex_); }


    static RaftLockGuard new_(rusty::RaftCheckedMutex* mutex);
    ~RaftLockGuard() noexcept(false);
};

struct RaftStdLockGuard {
    rusty::RaftStdMutex* mutex_;
    mutable bool _rusty_forgotten = false;
    RaftStdLockGuard(rusty::RaftStdMutex* mutex__init) : mutex_(std::move(mutex__init)) {}
    RaftStdLockGuard(const RaftStdLockGuard&) = delete;
    RaftStdLockGuard(RaftStdLockGuard&& other) noexcept : mutex_(std::move(other.mutex_)) {
        this->_rusty_forgotten = other._rusty_forgotten;
        other._rusty_forgotten = true;
    }
    RaftStdLockGuard& operator=(const RaftStdLockGuard&) = delete;
    RaftStdLockGuard& operator=(RaftStdLockGuard&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        this->~RaftStdLockGuard();
        new (this) RaftStdLockGuard(std::move(other));
        return *this;
    }
    void rusty_mark_forgotten() const noexcept { _rusty_forgotten = true; rusty::detail::mark_forgotten_if_supported(this->mutex_); }


    static RaftStdLockGuard new_(rusty::RaftStdMutex* mutex);
    ~RaftStdLockGuard() noexcept(false);
};


inline RaftLockGuard RaftLockGuard::new_(rusty::RaftCheckedMutex* mutex) {
    // @unsafe
    {
        raft_mutex_lock(mutex);
    }
    return RaftLockGuard(mutex);
}

inline RaftLockGuard::~RaftLockGuard() noexcept(false) {
    if (_rusty_forgotten) { return; }
    // @unsafe
    {
        raft_mutex_unlock(this->mutex_);
    }
}

inline RaftStdLockGuard RaftStdLockGuard::new_(rusty::RaftStdMutex* mutex) {
    // @unsafe
    {
        raft_std_mutex_lock(mutex);
    }
    return RaftStdLockGuard(mutex);
}

inline RaftStdLockGuard::~RaftStdLockGuard() noexcept(false) {
    if (_rusty_forgotten) { return; }
    // @unsafe
    {
        raft_std_mutex_unlock(this->mutex_);
    }
}
/*RUSTYCPP:GEN-END id=raft_server.lock_guard*/

// ============================================================================
// RaftServerBase -- RaftServer's STATE, as a DSL struct.
//
// Step E of the migration. Every one of RaftServer's data members except the
// replication wake gate lives here, in Rust, and `class RaftServer` below
// derives from this instead of from TxLogServer directly.
//
// Why a base class rather than making RaftServer itself the DSL struct: the
// transpiler honours an `impl` only when its `pub struct` sits in the SAME
// `#if RUSTYCPP_RUST` block, so making RaftServer the DSL struct would
// require all 67 of its out-of-line method bodies (2,527 lines) to become
// Rust in a single landing. With the state in a base, a method converts by
// MOVING from `RaftServer::Foo` (deleted C++) to `impl RaftServerBase`
// (added Rust) one at a time -- call sites keep resolving through
// inheritance, so nothing is left behind as a shim. That is the difference
// between substituting C++ and accreting Rust beside it.
//
// The field types are named through the aliases above so the emitted C++ is
// spelled exactly as the hand-written members were; this landing changes
// where the members are declared and nothing else about them.
//
// NOT here: replication_wake_gate_. `ReplicationWakeGate` is only a forward
// declaration in this header (its DSL block is in server.cc), so a
// constructor emitted here cannot build the Arc. It stays a RaftServer
// member, initialised by RaftServer's own constructor, until the gate's
// definition moves into a header.
// ============================================================================
#if RUSTYCPP_RUST
use rusty::cpp_inherit;
use crate::scheduler_h::TxLogServer;

// The rrr `verify` macro, reachable from a DSL body. extern "C" is the one
// function-declaration form a block can spell that rustc resolves without a
// Rust definition behind it; server.h defines it just above.
// improper_ctypes fires on every `*mut RaftServerBase` below, because the
// struct transitively holds rusty::Vec and Rust cannot promise its layout.
// Nothing is laid out across this boundary: the pointer is an opaque handle,
// both sides are the same translation unit compiled by the same compiler, and
// only the C++ side ever dereferences it. This is the case the allow exists
// for -- the same one the existing raft_* trampolines sidestep by erasing to
// void*, which hides the type rather than describing it.
#[allow(improper_ctypes)]
unsafe extern "C" {
    fn raft_verify(condition: bool);
    fn raft_time_now_us() -> u64;
    fn raft_snapshot_manager_is_set(manager: *const rusty::RaftSnapshotManagerPtr) -> bool;
    fn raft_random_range_us(low: u64, high: u64) -> u64;
    // The kernel bridge (server.cc). Each takes the base and, where it has
    // to reach a method that has not converted, casts down to RaftServer.
    fn raft_rebuild_peer_tables(server: *mut RaftServerBase, next_index: u64);
    fn raft_peer_site_at(server: *const RaftServerBase, ordinal: usize) -> u16;
    fn raft_leader_change_cb_is_set(server: *const RaftServerBase) -> bool;
    fn raft_fire_leader_change(server: *mut RaftServerBase, is_leader: bool);
    // The four env-tunable election-timeout knobs, which are ordinary C++
    // free functions in server.cc.
    fn raft_preferred_leader_grace_period_us() -> u64;
    fn raft_preferred_election_timeout_us() -> u64;
    fn raft_non_preferred_grace_election_timeout_us() -> u64;
    fn raft_non_preferred_steady_election_timeout_us() -> u64;
    fn raft_log_set_is_leader_entry(server: *const RaftServerBase,
                                    prev_is_leader: bool, new_is_leader: bool);
    fn raft_append_leader_noop(server: *mut RaftServerBase);
    fn raft_election_debug_enabled() -> bool;
    // The campaign broadcast, and the reply quorum read back under mtx_.
    fn raft_broadcast_vote_and_wait(server: *mut RaftServerBase, par_id: u32,
                                    last_log_index: u64, last_log_term: i64,
                                    self_site_id: u16, term: i64)
        -> rusty::RaftVoteQuorumPtr;
    fn raft_vote_quorum_snapshot(quorum: *const rusty::RaftVoteQuorumPtr)
        -> RaftVoteOutcome;
    fn raft_snapshot_manager_has_latest(
        manager: *const rusty::RaftSnapshotManagerPtr) -> bool;
    fn raft_command_has_value(cmd: *const rusty::RaftCommand) -> bool;
    fn raft_apply_queue_push_range(server: *mut RaftServerBase, first: u64,
                                   last: u64) -> u64;
    fn raft_apply_queue_size(server: *mut RaftServerBase) -> u64;
    fn raft_startup_wait(server: *mut RaftServerBase);
    fn raft_startup_notify_all(server: *mut RaftServerBase);
    fn raft_apply_thread_join(server: *mut RaftServerBase);
    fn raft_commo_set_network_enabled(server: *mut RaftServerBase, enabled: bool);
    fn raft_create_snapshot_locked(server: *mut RaftServerBase) -> bool;
    fn raft_request_replication(server: *mut RaftServerBase);
    fn raft_set_local_append(server: *mut RaftServerBase,
                             cmd: *const rusty::RaftCommand,
                             term: *mut u64, index: *mut u64,
                             slot_id: u64, ballot: i64) -> RaftStartResult;
    fn raft_close_replication_wake_gate(server: *mut RaftServerBase);
    fn raft_spawn_election_timer(server: *mut RaftServerBase, wait_int_us: u64);
    fn raft_setup_internal_guarded(server: *mut RaftServerBase) -> bool;
    fn raft_shutdown_barrier_yield();
    fn raft_peer_sites_len(server: *const RaftServerBase) -> usize;
}

// One campaign's reply quorum, read in a single shot.
//
// RequestVoteImpl must snapshot these AFTER reacquiring mtx_ -- FeedResponse
// publishes the highest observed term before its wakeup -- so the broadcast
// kernel hands back the event and this is filled by a second kernel called
// under the lock. Splitting it that way is not a detail: reading the term
// inside the broadcast kernel would sample it before the lock and lose the
// ordering the comment at that call site exists to protect.
#[repr(C)]
pub struct RaftVoteOutcome {
    pub term_: i64,
    pub yes_: bool,
    pub no_: bool,
    pub n_voted_yes_: i32,
    pub n_voted_no_: i32,
    pub timeouted_: bool,
}

// appliedIndexForWait_ keeps its C++ spelling: it is read by name from
// test.cc and from the C++ bodies that have not converted yet, and renaming
// it is a mechanical change that belongs in its own commit.
#[allow(non_snake_case)]
#[repr(C)]
pub struct RaftServerBase {
    // Raft's own copy of what TXLOG_SERVER_SITE_FIELDS() gives Paxos; see the
    // mirror note on RaftConsensusState for why both copies exist.
    pub loc_id_: u32,
    pub site_id_: u16,
    pub app_next_: rusty::LearnerAction,
    pub commo_: *mut rusty::Communicator,
    pub partition_id_: u32,
    pub mtx_: rusty::RaftCheckedMutex,
    // The consensus cluster mtx_ guards, as one Rust-owned value.
    pub state_: RaftConsensusState,
    // Scratch for raft_ae_decode_payload; valid only inside one
    // OnAppendEntries call, which is entirely under mtx_.
    pub decoded_terms_: rusty::RaftDecodedTerms,
    // RPC futures can outlive the server during shutdown. Destruction nulls
    // this shared gate after waiting for any callback already using it.
    pub async_callback_lifetime_: rusty::RaftAsyncCallbackLifetimePtr,
    pub snapshot_manager_: rusty::RaftSnapshotManagerPtr,
    pub snapshot_manager_configured_: rusty::sync::atomic::AtomicBool,
    pub snapshot_trigger_index_: rusty::sync::atomic::AtomicU64,
    pub snapshot_trigger_threshold_: rusty::sync::atomic::AtomicU64,
    pub create_sm_snapshot_cb_: rusty::RaftCreateSnapshotCb,
    pub prepare_sm_snapshot_cb_: rusty::RaftPrepareSnapshotCb,
    // Ordinal peer table, rebuilt whenever the configuration changes.
    pub peer_sites_: rusty::RaftPeerSites,
    pub stop_: rusty::sync::atomic::AtomicBool,
    pub rpc_ready_: rusty::sync::atomic::AtomicBool,
    pub startup_mtx_: rusty::RaftStdMutex,
    pub startup_cv_: rusty::RaftStdCondVar,
    pub startup_finished_: bool,
    pub startup_succeeded_: bool,
    pub wait_int_: i32,
    pub disconnected_: rusty::sync::atomic::AtomicBool,
    pub in_applying_logs_: bool,
    pub failover_: bool,
    pub looping_: rusty::sync::atomic::AtomicBool,
    pub heartbeat_loop_running_: rusty::sync::atomic::AtomicBool,
    pub election_loop_running_: rusty::sync::atomic::AtomicBool,
    pub heartbeat_: bool,
    pub heartbeat_setup_: bool,
    pub heartbeat_interval_us_: u64,
    pub log_retention_window_: u64,
    pub leader_change_cb_: rusty::RaftLeaderChangeCb,
    pub preferred_leader_site_id_: u16,
    pub startup_timestamp_: u64,
    pub current_config_: rusty::RaftSiteIdSet,
    pub apply_thread_: rusty::RaftStdThread,
    pub apply_thread_running_: rusty::sync::atomic::AtomicBool,
    pub state_machine_apply_mtx_: rusty::RaftStdMutex,
    pub apply_queue_mtx_: rusty::RaftStdMutex,
    pub apply_queue_epoch_: u64,
    pub apply_queue_: rusty::RaftApplyQueue,
    pub appliedIndexForWait_: rusty::sync::atomic::AtomicU64,
    // Was a function-static in EnqueueCommittedEntries. A DSL body has no
    // static local, and a per-server counter is the more honest shape: the
    // C++ one was shared across every RaftServer in a single-process test.
    pub enqueue_log_counter_: u64,
    pub n_prepare_: i32,
    pub n_accept_: i32,
    pub n_commit_: i32,
}

// Method names keep their C++ spelling: every one of them is called by name
// from bodies that have not converted yet, and from test.cc. Renaming them to
// snake_case is a mechanical change for after the conversion, not during it.
#[allow(non_snake_case)]
impl RaftServerBase {
    // Every default here is the one the hand-written member declaration
    // carried; the DSL has no field-initialiser syntax, so they move into the
    // constructor's member-initialiser list instead.
    // No Default impl: `new` here lowers to a real C++ constructor, and a
    // Rust-side Default would suggest RaftServerBase is default-constructible
    // as a value, which it is not -- it holds a mutex and a thread.
    #[allow(clippy::new_without_default)]
    #[cfg_attr(any(), cpp_ctor)]
    pub fn new() -> RaftServerBase {
        RaftServerBase {
            // locid_t is uint32_t, so `static_cast<locid_t>(-1)` is this.
            loc_id_: 4294967295,
            site_id_: RAFT_SERVER_INVALID_SITE_ID,
            app_next_: Default::default(),
            commo_: core::ptr::null_mut(),
            partition_id_: 0,
            mtx_: Default::default(),
            state_: RaftConsensusState::new(),
            decoded_terms_: Default::default(),
            // Null here; RaftServer's constructor allocates it, because it
            // also has to store `this` into the gate.
            async_callback_lifetime_: Default::default(),
            snapshot_manager_: Default::default(),
            snapshot_manager_configured_: rusty::sync::atomic::AtomicBool::new(false),
            snapshot_trigger_index_: rusty::sync::atomic::AtomicU64::new(0),
            snapshot_trigger_threshold_: rusty::sync::atomic::AtomicU64::new(10000),
            create_sm_snapshot_cb_: Default::default(),
            prepare_sm_snapshot_cb_: Default::default(),
            peer_sites_: Default::default(),
            stop_: rusty::sync::atomic::AtomicBool::new(false),
            rpc_ready_: rusty::sync::atomic::AtomicBool::new(false),
            startup_mtx_: Default::default(),
            startup_cv_: Default::default(),
            startup_finished_: false,
            startup_succeeded_: false,
            wait_int_: 100000,
            disconnected_: rusty::sync::atomic::AtomicBool::new(false),
            in_applying_logs_: false,
            failover_: true,
            looping_: rusty::sync::atomic::AtomicBool::new(false),
            heartbeat_loop_running_: rusty::sync::atomic::AtomicBool::new(false),
            election_loop_running_: rusty::sync::atomic::AtomicBool::new(false),
            heartbeat_: true,
            heartbeat_setup_: false,
            // HEARTBEAT_INTERVAL is a macro whose value depends on
            // RAFT_TEST; RaftServer's constructor applies it, because a DSL
            // block drops #[cfg] silently and must not decide this.
            heartbeat_interval_us_: 0,
            log_retention_window_: 5000,
            leader_change_cb_: Default::default(),
            preferred_leader_site_id_: RAFT_SERVER_INVALID_SITE_ID,
            startup_timestamp_: 0,
            current_config_: Default::default(),
            apply_thread_: Default::default(),
            apply_thread_running_: rusty::sync::atomic::AtomicBool::new(false),
            state_machine_apply_mtx_: Default::default(),
            apply_queue_mtx_: Default::default(),
            apply_queue_epoch_: 0,
            apply_queue_: Default::default(),
            appliedIndexForWait_: rusty::sync::atomic::AtomicU64::new(0),
            enqueue_log_counter_: 0,
            n_prepare_: 0,
            n_accept_: 0,
            n_commit_: 0,
        }
    }

    // ======================================================================
    // Bodies moved here from server.cc. Each is the C++ statement sequence,
    // unchanged; mtx_ is held through RaftLockGuard, whose scope is the same
    // as the std::lock_guard it replaces.
    // ======================================================================

    // CALLER MUST HOLD mtx_.
    pub fn GetSnapshotIndexLocked(&self) -> u64 {
        self.state_.snapidx_
    }

    // @unsafe - returns the last snapshotted log index under mtx_.
    pub fn GetSnapshotIndex(&mut self) -> u64 {
        let _lock = RaftLockGuard::new(&mut self.mtx_);
        self.GetSnapshotIndexLocked()
    }

    // CALLER MUST HOLD mtx_.
    pub fn GetSnapshotTermLocked(&self) -> u64 {
        // snapterm_ is ballot_t (int64_t); the C++ signature returned
        // uint64_t and relied on the implicit conversion.
        self.state_.snapterm_ as u64
    }

    // @unsafe - returns the snapshot boundary term under mtx_.
    pub fn GetSnapshotTerm(&mut self) -> u64 {
        let _lock = RaftLockGuard::new(&mut self.mtx_);
        self.GetSnapshotTermLocked()
    }

    // CALLER MUST HOLD mtx_.
    pub fn SetSnapshotThresholdLocked(&mut self, threshold: u64) {
        self.state_.snapshot_threshold_ = threshold;
        self.snapshot_trigger_threshold_
            .store(threshold, rusty::sync::atomic::Ordering::Release);
    }

    // @unsafe - takes mtx_.
    pub fn SetSnapshotThreshold(&mut self, threshold: u64) {
        let _lock = RaftLockGuard::new(&mut self.mtx_);
        self.SetSnapshotThresholdLocked(threshold);
    }

    // @unsafe - synchronizes with Disconnect() through the Raft state mutex.
    pub fn IsDisconnected(&self) -> bool {
        self.disconnected_.load(rusty::sync::atomic::Ordering::Acquire)
    }

    // @unsafe - synchronizes with role/leader publication through mtx_.
    pub fn GetLeaderHint(&mut self) -> u16 {
        let _lock = RaftLockGuard::new(&mut self.mtx_);
        if self.state_.is_leader_ {
            return self.site_id_;
        }
        self.state_.current_leader_id_
    }

    // @safe - acquire load
    pub fn HeartbeatLooping(&self) -> bool {
        self.looping_.load(rusty::sync::atomic::Ordering::Acquire)
    }

    // @safe - two release stores
    pub fn HeartbeatEpilogue(&mut self) {
        self.looping_
            .store(false, rusty::sync::atomic::Ordering::Release);
        self.heartbeat_loop_running_
            .store(false, rusty::sync::atomic::Ordering::Release);
    }

    // @safe - acquire load; the C++ read it the same way
    pub fn ElectionLoopStopped(&self) -> bool {
        self.stop_.load(rusty::sync::atomic::Ordering::Acquire)
    }

    // @unsafe - takes mtx_ to read state_.req_voting_
    pub fn ElectionLoopVoting(&mut self) -> bool {
        let _lock = RaftLockGuard::new(&mut self.mtx_);
        self.state_.req_voting_
    }

    // @safe - release store on an atomic
    pub fn ElectionLoopSetRunning(&mut self, running: bool) {
        self.election_loop_running_
            .store(running, rusty::sync::atomic::Ordering::Release);
    }

    // @unsafe - takes mtx_ and reads the whole election cluster in one scope,
    // so the Rust loop can branch on copies after the lock is released.
    pub fn ElectionLoopGather(&mut self) -> ElectionTick {
        let _lock = RaftLockGuard::new(&mut self.mtx_);
        let time_now: u64 = unsafe { raft_time_now_us() };
        let heartbeat_time: u64 = self.state_.last_heartbeat_time_;
        let time_elapsed: u64 = time_now - heartbeat_time;
        let election_timeout: u64 = self.state_.election_timeout_us_;
        ElectionTick::new(
            time_elapsed,
            election_timeout,
            heartbeat_time,
            self.state_.election_timer_generation_,
            self.state_.current_term_,
            self.state_.vote_for_,
            raft_server_election_timeout_has_fired(
                self.state_.is_leader_, time_elapsed, election_timeout),
        )
    }

    // CALLER MUST HOLD mtx_. The term of the last log entry, or the snapshot
    // boundary term when the log has been compacted past it.
    pub fn ElectionLastLogTermLocked(&self) -> i64 {
        let last_index: u64 = self.state_.raft_log_.last_index();
        unsafe {
            raft_verify(last_index >= self.state_.snapidx_);
        }
        if raft_server_election_last_log_uses_snapshot(
            last_index, self.state_.snapidx_)
        {
            return self.state_.snapterm_;
        }
        // The C++ went through FindRaftInstance, which flattened the Option
        // to a raw pointer and then verified it non-null. Asking the log
        // directly is the same lookup with the check kept.
        let last_log = self.state_.raft_log_.get(last_index);
        unsafe {
            raft_verify(last_log.is_some());
        }
        last_log.unwrap().term()
    }

    // @unsafe - takes mtx_; the returned token is how a state machine proves
    // ownership when it later clears the callbacks.
    pub fn SetStateMachineSnapshotCallbacks(
        &mut self,
        create_cb: rusty::RaftCreateSnapshotCb,
        prepare_cb: rusty::RaftPrepareSnapshotCb,
    ) -> u64 {
        let _lock = RaftLockGuard::new(&mut self.mtx_);
        if self.state_.next_snapshot_callback_owner_token_ == 0 {
            self.state_.next_snapshot_callback_owner_token_ = 1;
        }
        let owner_token: u64 = self.state_.next_snapshot_callback_owner_token_;
        self.state_.next_snapshot_callback_owner_token_ += 1;
        self.create_sm_snapshot_cb_ = create_cb;
        self.prepare_sm_snapshot_cb_ = prepare_cb;
        self.state_.snapshot_callback_owner_token_ = owner_token;
        owner_token
    }

    // @unsafe - takes mtx_; a non-owning token is refused.
    pub fn ClearStateMachineSnapshotCallbacks(
        &mut self,
        callback_owner_token: u64,
    ) -> bool {
        if callback_owner_token == 0 {
            return false;
        }
        let _lock = RaftLockGuard::new(&mut self.mtx_);
        if self.state_.snapshot_callback_owner_token_ != callback_owner_token {
            return false;
        }
        self.create_sm_snapshot_cb_ = Default::default();
        self.prepare_sm_snapshot_cb_ = Default::default();
        self.state_.snapshot_callback_owner_token_ = 0;
        true
    }

    // CALLER MUST HOLD mtx_.
    pub fn SetSnapshotManagerLocked(
        &mut self,
        manager: rusty::RaftSnapshotManagerPtr,
    ) {
        self.snapshot_manager_ = manager;
        let configured: bool =
            unsafe { raft_snapshot_manager_is_set(&self.snapshot_manager_) };
        self.snapshot_manager_configured_
            .store(configured, rusty::sync::atomic::Ordering::Release);
    }

    // @unsafe - takes mtx_.
    pub fn SetSnapshotManager(&mut self, manager: rusty::RaftSnapshotManagerPtr) {
        let _lock = RaftLockGuard::new(&mut self.mtx_);
        self.SetSnapshotManagerLocked(manager);
    }

    // @safe - a plain move into the notification slot; no lock, exactly as
    // the C++ had it.
    pub fn RegisterLeaderChangeCallback(&mut self, cb: rusty::RaftLeaderChangeCb) {
        self.leader_change_cb_ = cb;
    }

    // ------------------------------------------------------------------
    // Role and progress accessors, formerly inline in class RaftServer.
    // ------------------------------------------------------------------

    // @unsafe - CALLER MUST HOLD mtx_.
    pub fn AmIPreferredLeader(&self) -> bool {
        raft_server_site_is_preferred_leader(
            self.site_id_, self.preferred_leader_site_id_)
    }

    // @safe - acquire load pairing with the final startup publication.
    pub fn IsRpcReady(&self) -> bool {
        self.rpc_ready_.load(rusty::sync::atomic::Ordering::Acquire)
    }

    // @safe - acquire load pairing with PublishAppliedIndex.
    pub fn GetAppliedIndex(&self) -> u64 {
        self.appliedIndexForWait_
            .load(rusty::sync::atomic::Ordering::Acquire)
    }

    // The looping_ check is not an optimisation: it is the guard against
    // reading members during destruction.
    pub fn IsLeaderLocked(&self) -> bool {
        if !self.looping_.load(rusty::sync::atomic::Ordering::Acquire) {
            return false;
        }
        self.state_.is_leader_
    }

    // Acquiring entry point, for callers that do not already hold mtx_.
    pub fn IsLeader(&mut self) -> bool {
        if !self.looping_.load(rusty::sync::atomic::Ordering::Acquire) {
            return false;
        }
        let _lock = RaftLockGuard::new(&mut self.mtx_);
        self.state_.is_leader_
    }

    // @unsafe - writes through caller-provided out-pointers under mtx_. The
    // signature is the C++ one; every call site passes the address of a
    // local.
    #[allow(clippy::not_unsafe_ptr_arg_deref)]
    pub fn GetState(&mut self, is_leader: *mut bool, term: *mut u64) {
        let _lock = RaftLockGuard::new(&mut self.mtx_);
        let leading: bool = self.IsLeaderLocked();
        unsafe {
            *is_leader = leading;
            *term = self.state_.current_term_;
        }
    }

    // @safe - POD field
    pub fn GetHeartbeatInterval(&self) -> u64 {
        self.heartbeat_interval_us_
    }

    // @safe - POD field
    pub fn SetHeartbeatInterval(&mut self, micros: u64) {
        self.heartbeat_interval_us_ = micros;
    }

    // @safe - POD field
    pub fn GetLogRetentionWindow(&self) -> u64 {
        self.log_retention_window_
    }

    // @safe - POD field, floored at 1 so the retention arithmetic cannot
    // divide by zero.
    pub fn SetLogRetentionWindow(&mut self, window: u64) {
        self.log_retention_window_ =
            raft_server_retention_window_normalize(window);
    }

    // @safe - reads the atomic trigger mirror.
    pub fn GetSnapshotThreshold(&self) -> u64 {
        self.snapshot_trigger_threshold_
            .load(rusty::sync::atomic::Ordering::Acquire)
    }

    // @unsafe - takes mtx_ and logs.
    pub fn SetPreferredLeader(&mut self, site_id: u16) {
        let _lock = RaftLockGuard::new(&mut self.mtx_);
        let old_preferred: u16 = self.preferred_leader_site_id_;
        self.preferred_leader_site_id_ = site_id;
        if old_preferred != site_id {
            rusty::raft_log_info_2(
                "[LEADERSHIP-TRANSFER] Site {}: Preferred leader set to {}",
                self.site_id_, site_id);
        }
    }

    // ------------------------------------------------------------------
    // Applied-index publication, compaction, and the term-change log.
    // ------------------------------------------------------------------

    // CALLER MUST HOLD mtx_. The applied index never moves backward; a
    // caller that tries is a bug, so it is reported rather than obeyed.
    pub fn PublishAppliedIndexLocked(&mut self, index: u64) {
        let published: u64 = self.GetAppliedIndex();
        if raft_server_log_index_above(published, index) {
            rusty::raft_log_warn_3(
                "[RAFT-APPLY] Site {} refusing to move applied index backward from {} to {}",
                self.site_id_, published, index);
            return;
        }
        self.state_.execute_index_ = index;
        self.appliedIndexForWait_
            .store(index, rusty::sync::atomic::Ordering::Release);
    }

    // @unsafe - takes mtx_.
    pub fn PublishAppliedIndex(&mut self, index: u64) {
        let _lock = RaftLockGuard::new(&mut self.mtx_);
        self.PublishAppliedIndexLocked(index);
    }

    // CALLER MUST HOLD mtx_. Compaction is safe only through the prefix
    // covered by BOTH the committed state and the snapshot boundary.
    pub fn CompactLogLocked(&mut self, up_to_index: u64) -> usize {
        let requested_index: u64 = up_to_index;
        let safe_index: u64 = raft_server_compaction_safe_index(
            up_to_index, self.state_.commit_index_, self.state_.snapidx_);
        if safe_index != requested_index {
            rusty::raft_log_warn_5(
                "[RAFT-COMPACT] Site {}: Clamped compaction {} -> {} (state_.commit_index_={}, snapidx={})",
                self.site_id_, requested_index, safe_index,
                self.state_.commit_index_, self.state_.snapidx_);
        }

        if !raft_server_log_index_has_successor(safe_index) {
            rusty::raft_log_error_2(
                "[RAFT-COMPACT] Site {}: Refusing terminal compaction index {}; the exclusive storage bound and min_active_slot would wrap",
                self.site_id_, safe_index);
            return 0;
        }

        let removed_memory: usize =
            self.state_.raft_log_.compact_through(safe_index);

        rusty::raft_log_info_3(
            "[RAFT-COMPACT] Site {}: Compacted in-memory entries through {} (memory={})",
            self.site_id_, safe_index, removed_memory);
        removed_memory
    }

    // @unsafe - acquiring entry point, for callers that do not hold mtx_.
    pub fn CompactLog(&mut self, up_to_index: u64) -> usize {
        let _lock = RaftLockGuard::new(&mut self.mtx_);
        self.CompactLogLocked(up_to_index)
    }

    // @unsafe - logs a term transition; silent when the term is unchanged.
    //
    // `reason` was `const char*` with a `reason ? reason : "unspecified"`
    // fallback. All seven call sites pass a string literal, so the fallback
    // was dead; the parameter is now &str, which lowers to std::string_view
    // and cannot be null. Every C++ caller still compiles unchanged.
    pub fn LogTermChange(&self, reason: &str, old_term: u64, new_term: u64,
                         source: u16) {
        if old_term == new_term {
            return;
        }
        if source != RAFT_SERVER_INVALID_SITE_ID {
            rusty::raft_log_info_5(
                "[RAFT-TERM] server {} term {} -> {} ({}, source_site={})",
                self.site_id_, old_term, new_term, reason, source);
        } else {
            rusty::raft_log_info_4(
                "[RAFT-TERM] server {} term {} -> {} ({})",
                self.site_id_, old_term, new_term, reason);
        }
    }

    // ------------------------------------------------------------------
    // Election-loop kernels.
    // ------------------------------------------------------------------

    // @unsafe - RandomGenerator is external.
    pub fn ElectionLoopRandomDelay(&self) -> u64 {
        unsafe {
            raft_random_range_us(
                self.heartbeat_interval_us_ * 2,
                self.heartbeat_interval_us_ * 4)
        }
    }

    // @unsafe - rrr logging.
    pub fn ElectionLoopLogStart(&self) {
        rusty::raft_log_debug_0("start timer for election");
    }

    // @unsafe - rrr logging.
    pub fn ElectionLoopLogFired(&self, tick: &ElectionTick) {
        rusty::raft_log_info_3(
            "[ELECTION_TIMER] Site {}: TIMEOUT FIRED - starting election (elapsed={} > timeout={})",
            self.site_id_, tick.time_elapsed(), tick.election_timeout());
        rusty::raft_log_info_6(
            "[ELECTION_START] Site {}: TRIGGERING REQUESTVOTE - time_elapsed={} > timeout={} last_hb={} current_term={} vote_for={}",
            self.site_id_, tick.time_elapsed(), tick.election_timeout(),
            tick.heartbeat_time(), tick.term(), tick.vote_for());
    }

    // ------------------------------------------------------------------
    // The role transition, the election timer, and stepping down.
    // ------------------------------------------------------------------

    // @unsafe - CALLER MUST HOLD mtx_. Rebuilds the ordinal peer table from
    // the configuration. The body is a kernel because current_config_ is a
    // std::set and peer_sites_ a std::vector: Rust holds them but cannot
    // iterate them.
    pub fn RebuildPeerTables(&mut self, next_index: u64) {
        unsafe {
            raft_rebuild_peer_tables(self as *mut RaftServerBase, next_index);
        }
    }

    // CALLER MUST HOLD mtx_ -- the one caller repo-wide is resetTimerLocked,
    // whose own callers take it. The configured identity is stable for the
    // whole decision because of that lock, not because anything is sampled
    // atomically here.
    //
    // Memory-only Raft configures no log storage, so the randomized timeout
    // IS the effective election timeout: there is no persistence floor to
    // add.
    pub fn GetElectionTimeout(&self) -> u64 {
        let current_time: u64 = unsafe { raft_time_now_us() };
        let grace_period_us: u64 =
            unsafe { raft_preferred_leader_grace_period_us() };
        let in_grace_period: bool =
            (current_time - self.startup_timestamp_) < grace_period_us;
        // IsPreferredLeaderConfigured's whole body (server.cc). It is a DSL
        // function of the server.cc carrier, so this block cannot name it;
        // the predicate is one comparison and is spelled out rather than
        // bridged.
        let preferred_leader_configured: bool =
            self.preferred_leader_site_id_ != RAFT_SERVER_INVALID_SITE_ID;

        if !preferred_leader_configured {
            // Traditional Raft when no preferred leader is configured.
            unsafe { raft_non_preferred_steady_election_timeout_us() }
        } else if self.AmIPreferredLeader() {
            unsafe { raft_preferred_election_timeout_us() }
        } else if in_grace_period {
            // The startup grace timeout is env-tunable for test stability.
            unsafe { raft_non_preferred_grace_election_timeout_us() }
        } else {
            unsafe { raft_non_preferred_steady_election_timeout_us() }
        }
    }

    // CALLER MUST HOLD mtx_. Samples exactly one timeout and advances the
    // generation, so a concurrent heartbeat reset cannot leave a campaign
    // running off an expired snapshot.
    pub fn resetTimerLocked(&mut self, reason: &str) {
        let prev_time: u64 = self.state_.last_heartbeat_time_;
        self.state_.last_heartbeat_time_ = unsafe { raft_time_now_us() };
        self.state_.election_timeout_us_ = self.GetElectionTimeout();
        if self.state_.election_timer_generation_ == u64::MAX {
            self.state_.election_timer_generation_ = 1;
        } else {
            self.state_.election_timer_generation_ += 1;
        }
        // Log only the resets that matter (elections, votes), never the
        // routine heartbeat ones.
        if reason == "granted vote" || reason == "start election timer" {
            rusty::raft_log_info_7(
                "[TIMER_RESET] Site {}: reset timer ({}) - prev_hb_time={} new_hb_time={} delta={} timeout={} generation={}",
                self.site_id_, reason, prev_time,
                self.state_.last_heartbeat_time_,
                self.state_.last_heartbeat_time_ - prev_time,
                self.state_.election_timeout_us_,
                self.state_.election_timer_generation_);
        }
    }

    // @unsafe - acquiring entry point, for callers that do not hold mtx_.
    pub fn resetTimer(&mut self, reason: &str) {
        let _lock = RaftLockGuard::new(&mut self.mtx_);
        self.resetTimerLocked(reason);
    }

    // @unsafe - CALLER MUST HOLD mtx_. The one place this server's role
    // changes.
    pub fn setIsLeader(&mut self, is_leader: bool) {
        let prev_is_leader: bool = self.state_.is_leader_;
        unsafe {
            raft_log_set_is_leader_entry(self as *const RaftServerBase, prev_is_leader, is_leader);
        }

        if is_leader && !prev_is_leader {
            // Leadership publication must not proceed once shutdown began.
            let publication_term: u64 = self.state_.current_term_;
            if self.stop_.load(rusty::sync::atomic::Ordering::Acquire)
                || self.state_.current_term_ != publication_term
            {
                rusty::raft_log_warn_4(
                    "[RAFT_STATE] Site {} suppressing stale leadership publication for term {} (current={}, stopping={})",
                    self.site_id_, publication_term, self.state_.current_term_,
                    self.stop_.load(rusty::sync::atomic::Ordering::Acquire));
                return;
            }
        }

        if is_leader {
            // A heartbeat proof belongs to exactly one leadership term. Reset
            // the local generation BEFORE publishing this server as leader,
            // so delayed or historical acknowledgements cannot prove a quorum
            // in the new term.
            self.state_.heartbeat_round_ = 0;
            self.state_.read_quorum_confirmed_term_ = 0;
            self.state_.read_quorum_confirmed_round_ = 0;
        }

        if is_leader && self.failover_ {
            let next_index: u64 = self.state_.raft_log_.last_index() + 1;
            self.RebuildPeerTables(next_index);
            let peers: usize = self.state_.peers_.len();
            let mut ord: usize = 0;
            while ord < peers {
                let site: u16 = unsafe { raft_peer_site_at(self as *const RaftServerBase, ord) };
                rusty::raft_log_debug_5(
                    "loc_id_={} match_index_[{}]={}, next_index_[{}]={}",
                    self.loc_id_, site, self.state_.peers_.match_index(ord),
                    site, self.state_.peers_.next_index(ord));
                ord += 1;
            }
        }

        // These two MUST be computed before state_.is_leader_ is assigned,
        // or they both become false.
        let become_new_leader: bool = is_leader && !self.state_.is_leader_;
        let become_new_follower: bool = !is_leader && self.state_.is_leader_;

        self.state_.is_leader_ = is_leader;

        // Becoming leader establishes self as the known leader. Becoming a
        // follower deliberately PRESERVES a hint learned from AppendEntries
        // or InstallSnapshot; transitions with no known leader clear it at
        // their own call sites.
        self.state_.current_leader_id_ = raft_server_leader_hint_after_transition(
            is_leader,
            !is_leader && self.state_.current_leader_id_ != RAFT_SERVER_INVALID_SITE_ID,
            self.site_id_,
            self.state_.current_leader_id_);

        // Only on an actual transition, not on a no-op call.
        if become_new_leader || become_new_follower {
            rusty::raft_log_info_4(
                "RaftServer::setIsLeader site_id_ {} become_new_leader {} become_new_follower {} isLeader {}",
                self.site_id_, become_new_leader, become_new_follower, is_leader);
        }

        if become_new_leader {
            rusty::raft_log_info_4(
                "[RAFT_STATE] setIsLeader transition LEADER: site {} term {} prev_is_leader={} become_new_leader={}",
                self.site_id_, self.state_.current_term_, prev_is_leader,
                become_new_leader);
            // Compiled out under RAFT_TEST_CORO; see the kernel.
            unsafe {
                raft_append_leader_noop(self as *mut RaftServerBase);
            }
        } else if become_new_follower {
            rusty::raft_log_info_4(
                "[RAFT_STATE] setIsLeader transition FOLLOWER: site {} term {} prev_is_leader={} become_new_follower={}",
                self.site_id_, self.state_.current_term_, prev_is_leader,
                become_new_follower);

            // Resetting the timer here is what prevents an instant election
            // after a resume: last_heartbeat_time_ is stale from before the
            // pause, so counting from NOW gives the current leader time to
            // send a heartbeat first. Standard Raft: a server stepping down
            // resets its timer. setIsLeader is caller-holds.
            self.resetTimerLocked("became follower");
            rusty::raft_log_info_2(
                "[RAFT_TIMER] Site {} reset election timer when becoming follower (last_hb now={})",
                self.site_id_, self.state_.last_heartbeat_time_);
            rusty::raft_log_info_2(
                "[RAFT_VIEW] Server {} stepping down as leader for partition {}",
                self.site_id_, self.partition_id_);
        }

        // Fire the leadership-change callback so RaftWorker can retarget
        // clients to the new leader after an election.
        if unsafe { raft_leader_change_cb_is_set(self as *const RaftServerBase) } {
            if become_new_leader {
                rusty::raft_log_info_1(
                    "[LEADER_CALLBACK] Site {}: Firing leader_change_cb_(true) - became leader",
                    self.site_id_);
                unsafe { raft_fire_leader_change(self as *mut RaftServerBase, true) };
            } else if become_new_follower {
                rusty::raft_log_info_1(
                    "[LEADER_CALLBACK] Site {}: Firing leader_change_cb_(false) - became follower",
                    self.site_id_);
                unsafe { raft_fire_leader_change(self as *mut RaftServerBase, false) };
            }
        }
    }

    // ------------------------------------------------------------------
    // Loop prologue, startup, shutdown, submission and the apply queue.
    // ------------------------------------------------------------------

    // @safe - linear scan of a fixed, tiny table (replica counts are 3 or 5).
    // Returns state_.peers_.len() when the site is not a follower of this
    // leader, which is the "removed follower" case PHASE 2 guards against.
    // Deliberately an ordinal rather than a reference: an ordinal cannot
    // dangle across an RPC send or a re-entrant completion callback.
    pub fn PeerOrdinal(&self, site: u16) -> usize {
        let count: usize =
            unsafe { raft_peer_sites_len(self as *const RaftServerBase) };
        let mut ord: usize = 0;
        while ord < count {
            if unsafe { raft_peer_site_at(self as *const RaftServerBase, ord) }
                == site
            {
                return ord;
            }
            ord += 1;
        }
        self.state_.peers_.len()
    }

    // @unsafe - publishes the cross-thread replication wake. The gate itself
    // is still a RaftServer member (ReplicationWakeGate is only forward
    // declared in this header), so the body is a kernel.
    pub fn RequestReplication(&mut self) {
        unsafe {
            raft_request_replication(self as *mut RaftServerBase);
        }
    }

    // @unsafe - timer allocation and the first peer-table build.
    pub fn HeartbeatPrologue(&mut self) {
        self.heartbeat_loop_running_
            .store(true, rusty::sync::atomic::Ordering::Release);
        {
            // Taken explicitly. setIsLeader does this rebuild holding mtx_
            // and this did not, which was safe only because both run as
            // fibers on one poll thread with no suspension between them --
            // an accident, not a design.
            let _lock = RaftLockGuard::new(&mut self.mtx_);
            self.RebuildPeerTables(1);
        }
        rusty::raft_log_debug_1("heartbeat loop init from site: {}",
                                self.site_id_);
        self.looping_
            .store(true, rusty::sync::atomic::Ordering::Release);
    }

    // @unsafe - takes the state-machine apply gate, then mtx_. That order is
    // the one every apply-side path uses.
    pub fn MaybeCreateSnapshot(&mut self) {
        let _apply_lock =
            RaftStdLockGuard::new(&mut self.state_machine_apply_mtx_);
        let _lock = RaftLockGuard::new(&mut self.mtx_);
        let configured: bool =
            unsafe { raft_snapshot_manager_is_set(&self.snapshot_manager_) };
        if !configured
            || !raft_server_snapshot_is_due(self.state_.snapidx_,
                                            self.state_.execute_index_,
                                            self.state_.snapshot_threshold_)
        {
            return;
        }
        unsafe {
            raft_create_snapshot_locked(self as *mut RaftServerBase);
        }
    }

    // @unsafe - copies the manager under mtx_ before querying it, so the
    // query itself never runs with Raft state locked.
    pub fn HasSnapshot(&mut self) -> bool {
        let configured: bool = {
            let _lock = RaftLockGuard::new(&mut self.mtx_);
            unsafe { raft_snapshot_manager_is_set(&self.snapshot_manager_) }
        };
        if !configured {
            return false;
        }
        unsafe { raft_snapshot_manager_has_latest(&self.snapshot_manager_) }
    }

    // @unsafe - gates inbound and outbound test traffic under mtx_.
    pub fn Disconnect(&mut self, disconnect: bool) {
        let _lock = RaftLockGuard::new(&mut self.mtx_);
        unsafe {
            raft_verify(
                self.disconnected_.load(rusty::sync::atomic::Ordering::Acquire)
                    != disconnect);
            raft_commo_set_network_enabled(self as *mut RaftServerBase,
                                           !disconnect);
        }
        self.disconnected_
            .store(disconnect, rusty::sync::atomic::Ordering::Release);
    }

    // @safe - calls Disconnect and resets the timer.
    pub fn Reconnect(&mut self) {
        self.Disconnect(false);
        self.resetTimer("reconnect");
    }

    // @unsafe - idempotent one-shot setup.
    pub fn EnsureSetup(&mut self) {
        if self.heartbeat_setup_ {
            return;
        }
        self.heartbeat_setup_ = true;
        self.Setup();
    }

    // @unsafe - runs SetupInternal under a catch-all and publishes the
    // result to whoever is blocked in WaitForStartup.
    pub fn Setup(&mut self) {
        let succeeded: bool =
            unsafe { raft_setup_internal_guarded(self as *mut RaftServerBase) };
        if !succeeded {
            self.rpc_ready_
                .store(false, rusty::sync::atomic::Ordering::Release);
            self.stop_
                .store(true, rusty::sync::atomic::Ordering::Release);
            self.looping_
                .store(false, rusty::sync::atomic::Ordering::Release);
        }
        {
            let ready: bool = self.IsRpcReady();
            let _lock = RaftStdLockGuard::new(&mut self.startup_mtx_);
            self.startup_succeeded_ = succeeded && ready;
            self.startup_finished_ = true;
        }
        unsafe {
            raft_startup_notify_all(self as *mut RaftServerBase);
        }
    }

    // @safe - waits for the owner-thread startup job and reports its result.
    pub fn WaitForStartup(&mut self) -> bool {
        unsafe {
            raft_startup_wait(self as *mut RaftServerBase);
        }
        self.startup_succeeded_
    }

    // @safe - election timer setup; the fiber spawn is a kernel.
    pub fn StartElectionTimer(&mut self) {
        self.ElectionLoopSetRunning(true);
        self.resetTimer("start election timer");
        let wait_int: u64 = self.wait_int_ as u64;
        unsafe {
            raft_spawn_election_timer(self as *mut RaftServerBase, wait_int);
        }
    }

    // @unsafe - must be called from a reactor fiber before destroying a live
    // server; signals both runtime loops and waits for their completion
    // flags, then stops and joins the apply thread while the server is still
    // fully alive (applying an entry can trigger snapshot compaction).
    pub fn PrepareForShutdown(&mut self) {
        {
            // Linearize admission closure with every RPC and local mutation
            // under mtx_.
            let _admission_lock = RaftLockGuard::new(&mut self.mtx_);
            self.rpc_ready_
                .store(false, rusty::sync::atomic::Ordering::Release);
            self.stop_
                .store(true, rusty::sync::atomic::Ordering::Release);
            self.looping_
                .store(false, rusty::sync::atomic::Ordering::Release);
        }
        unsafe {
            raft_close_replication_wake_gate(self as *mut RaftServerBase);
        }

        while self
            .heartbeat_loop_running_
            .load(rusty::sync::atomic::Ordering::Acquire)
            || self
                .election_loop_running_
                .load(rusty::sync::atomic::Ordering::Acquire)
        {
            unsafe {
                raft_shutdown_barrier_yield();
            }
        }

        self.apply_thread_running_
            .store(false, rusty::sync::atomic::Ordering::SeqCst);
        unsafe {
            raft_apply_thread_join(self as *mut RaftServerBase);
        }
    }

    // @unsafe - CALLER MUST NOT HOLD mtx_. Appends one command locally and
    // then publishes the replication wake, in that order: the wake path never
    // nests the gate's owner mutex below Raft state.
    #[allow(clippy::not_unsafe_ptr_arg_deref)]
    pub fn StartImpl(&mut self, cmd: *const rusty::RaftCommand,
                     index: *mut u64, term: *mut u64, slot_id: u64,
                     ballot: i64) -> RaftStartResult {
        {
            let _lock = RaftLockGuard::new(&mut self.mtx_);
            if !self.IsLeaderLocked() {
                unsafe {
                    *index = 0;
                    *term = 0;
                }
                return RaftStartResult::REJECTED;
            }
            let append_result: RaftStartResult = unsafe {
                raft_set_local_append(self as *mut RaftServerBase, cmd, term,
                                      index, slot_id, ballot)
            };
            unsafe {
                raft_verify(raft_server_start_was_appended(append_result));
                // SetLocalAppend reports the OLD last index; Start reports
                // the index of the entry it just appended.
                raft_verify(
                    self.state_.raft_log_.last_index() == *index + 1);
                *index = self.state_.raft_log_.last_index();
                rusty::raft_log_debug_3("Start(): ldr={} index={} term={}",
                                        self.loc_id_, *index, *term);
            }
        }
        unsafe {
            raft_request_replication(self as *mut RaftServerBase);
        }
        RaftStartResult::APPENDED
    }

    // @unsafe - hands newly committed entries to the background apply thread.
    #[allow(clippy::manual_is_multiple_of)]
    // The scan stops at the first gap; the copy out of the log happens before
    // apply_queue_mtx_ is taken, which is why the enqueue itself is a kernel.
    pub fn EnqueueCommittedEntries(&mut self, old_commit: u64,
                                   new_commit: u64) {
        let mut first_missing: u64 = 0;
        let mut last_present: u64 = old_commit;
        let mut id: u64 = old_commit + 1;
        while id <= new_commit {
            let found = self.state_.raft_log_.get(id);
            let usable: bool = found.is_some()
                && unsafe {
                    raft_command_has_value(
                        found.unwrap().cmd() as *const rusty::RaftCommand)
                };
            if !usable {
                first_missing = id;
                break;
            }
            last_present = id;
            id += 1;
        }

        let mut enqueued: u64 = 0;
        if last_present > old_commit {
            enqueued = unsafe {
                raft_apply_queue_push_range(self as *mut RaftServerBase,
                                            old_commit + 1, last_present)
            };
        }

        if first_missing > 0 {
            rusty::raft_log_info_5(
                "[ENQUEUE] Site {}: gap at slot {} (range {}..{}, enqueued {})",
                self.site_id_, first_missing, old_commit + 1, new_commit,
                enqueued);
        }
        // One in fifty, so a steady stream of commits does not drown the log.
        let ticket: u64 = self.enqueue_log_counter_;
        self.enqueue_log_counter_ += 1;
        // `%` rather than is_multiple_of: this lowers to C++, where uint64_t
        // has no such member.
        if ticket % 50 == 0 {
            let qsize: u64 =
                unsafe { raft_apply_queue_size(self as *mut RaftServerBase) };
            rusty::raft_log_info_5(
                "[ENQUEUE] Site {}: enqueued {} entries ({}..{}) queue_total={}",
                self.site_id_, enqueued, old_commit + 1, new_commit, qsize);
        }
    }

    // @unsafe - CALLER MUST HOLD mtx_. Demotion is terminal for the election
    // in progress as well as for the leadership epoch that is ending.
    // @unsafe - the campaign. Two critical sections with one fiber
    // suspension between them: the broadcast must not hold mtx_, and every
    // decision after it must be re-derived from state read under the
    // reacquired lock.
    //
    // Returns true only when this server both won the election and still
    // held leadership when the result was applied.
    // The zero-initialisation of lst_idx/lst_term/prev_term is the C++
    // original's and is kept deliberately: dropping it to satisfy the lint
    // would emit uninitialised C++ locals, which is a worse trade than an
    // assignment rustc can see is redundant.
    #[allow(unused_assignments)]
    pub fn RequestVoteImpl(&mut self, timer_guarded: bool,
                           expected_generation: u64) -> bool {
        // The election timer fiber can fire after ~RaftServer has run, which
        // would reach TxLogServer::RequestVote and its verify(0). stop_ is
        // the guard against that teardown race.
        if self.stop_.load(rusty::sync::atomic::Ordering::Acquire) {
            rusty::raft_log_debug_1(
                "[RAFT-SHUTDOWN] RequestVote called during shutdown (site={}), ignoring to prevent crash",
                self.site_id_);
            return false;
        }

        let par_id: u32 = self.partition_id_;
        let loc_id: u32 = self.loc_id_;

        let mut lst_idx: u64 = 0;
        let mut lst_term: i64 = 0;
        let mut prev_term: u64 = 0;
        let mut term: u64 = 0;
        let mut prev_vote_for: u16 = RAFT_SERVER_INVALID_SITE_ID;

        {
            let _lock = RaftLockGuard::new(&mut self.mtx_);
            if self.stop_.load(rusty::sync::atomic::Ordering::Acquire) {
                self.state_.req_voting_ = false;
                return false;
            }
            // This is the sole campaign admission point. Entrants can overlap
            // while one of them is yielding, so a caller must NOT reserve
            // req_voting_ before entering this critical section.
            if !raft_server_campaign_can_start(
                self.state_.is_leader_, self.state_.election_in_progress_)
            {
                return false;
            }
            if timer_guarded {
                let now: u64 = unsafe { raft_time_now_us() };
                let elapsed: u64 = now - self.state_.last_heartbeat_time_;
                if !raft_server_timer_campaign_is_current(
                    self.state_.is_leader_, expected_generation,
                    self.state_.election_timer_generation_, elapsed,
                    self.state_.election_timeout_us_)
                {
                    return false;
                }
            }

            // A campaign owns a fresh, latched timeout. If it loses without
            // hearing from a leader, the next campaign waits out this whole
            // interval instead of reusing the already-expired deadline.
            self.resetTimerLocked("starting election campaign");
            prev_term = self.state_.current_term_;
            prev_vote_for = self.state_.vote_for_;
            let prev_local_term: u64 = self.state_.current_term_;
            self.state_.current_term_ += 1;
            // Vote for ourselves.
            self.state_.vote_for_ = self.site_id_;
            // A candidate has no elected-leader evidence in its new term; in
            // particular it must not redirect clients to the leader of the
            // term it just left.
            self.state_.current_leader_id_ =
                raft_server_leader_hint_after_transition(
                    false, false, self.site_id_, self.state_.current_leader_id_);

            // Publish ownership of req_voting_ and the election term before
            // broadcasting, so no second caller can campaign concurrently.
            self.state_.election_in_progress_ = true;
            // election_term_ is ballot_t (int64_t) and current_term_ is
            // uint64_t; the C++ assigned across that implicitly.
            self.state_.election_term_ = self.state_.current_term_ as i64;
            self.state_.req_voting_ = true;
            term = self.state_.current_term_;

            self.LogTermChange("starting election", prev_local_term,
                               self.state_.current_term_,
                               RAFT_SERVER_INVALID_SITE_ID);
            lst_idx = self.state_.raft_log_.last_index();
            lst_term = self.ElectionLastLogTermLocked();
        }

        if unsafe { raft_election_debug_enabled() } {
            rusty::raft_log_info_7(
                "[RAFT_ELECTION] server {} (loc {}) starting election term {}->{} lastLogIdx={} lastLogTerm={} prev_vote_for={}",
                self.site_id_, loc_id, prev_term, term, lst_idx, lst_term,
                prev_vote_for);
        }

        // The candidate id on the wire is a GLOBAL site id, not the
        // per-partition locale id. Everything downstream treats it that way:
        // RaftCommo skips itself by comparing peer->site_id(), the receiver
        // admits a candidate only if current_config_ contains it and
        // current_config_ is filled from Config::SitesByPartitionId()'s
        // site.id, and this candidate just recorded vote_for_ = site_id_,
        // which the grant path compares against can_id.
        //
        // Passing loc_id_ here was correct only for partition 0:
        // Config::LoadSiteYML increments site_id globally across replica-group
        // rows while resetting locale_id to 0 at the top of each row, so with
        // three replicas per group site_id == 3 * partition + locale and the
        // two id spaces coincide only at partition 0. Above it the candidate
        // advertised 0, 1 or 2 while current_config_ held {3p, 3p+1, 3p+2}:
        // every vote was rejected as a non-voter, the self-skip never matched
        // so the candidate RequestVoted its own listener, and the term counter
        // ran away. It compiled silently because locid_t is uint32_t and
        // siteid_t is uint16_t, so the call narrowed.
        let quorum: rusty::RaftVoteQuorumPtr = unsafe {
            raft_broadcast_vote_and_wait(
                self as *mut RaftServerBase, par_id, lst_idx, lst_term,
                self.site_id_, term as i64)
        };

        let _lock1 = RaftLockGuard::new(&mut self.mtx_);
        if self.stop_.load(rusty::sync::atomic::Ordering::Acquire) {
            self.state_.election_in_progress_ = false;
            self.state_.req_voting_ = false;
            return false;
        }
        // A higher term dominates every outcome, TIMEOUT and a concurrently
        // completed YES quorum included. FeedResponse publishes that maximum
        // before its wakeup, so it is snapshotted only now, after Raft state
        // has been reacquired.
        let outcome: RaftVoteOutcome =
            unsafe {
                raft_vote_quorum_snapshot(
                    &quorum as *const rusty::RaftVoteQuorumPtr)
            };
        let observed_response_term: i64 = outcome.term_;
        let completion_action: i32 = raft_server_election_completion_action(
            self.state_.election_in_progress_,
            self.state_.election_term_ as u64, term, self.state_.current_term_,
            observed_response_term);

        if completion_action == ElectionCompletionAction::ADVANCE_HIGHER_TERM as i32 {
            let previous_term: u64 = self.state_.current_term_;
            self.state_.current_term_ = observed_response_term as u64;
            self.state_.vote_for_ = RAFT_SERVER_INVALID_SITE_ID;
            self.state_.current_leader_id_ =
                raft_server_leader_hint_after_transition(
                    false, false, self.site_id_, self.state_.current_leader_id_);

            if self.state_.is_leader_ {
                self.stepDown();
            } else {
                self.setIsLeader(false);
            }
            self.state_.election_in_progress_ = false;
            self.state_.req_voting_ = false;

            self.LogTermChange("observed higher term from RequestVote replies",
                               previous_term, self.state_.current_term_,
                               RAFT_SERVER_INVALID_SITE_ID);
            return false;
        }

        // An accepted leader RPC can cancel this campaign while the broadcast
        // is yielding, and another campaign can begin before this result
        // arrives. Only the exact active term owns role changes and election
        // bookkeeping. A strictly higher response term was handled above,
        // because that evidence supersedes even a newer local campaign.
        if completion_action == ElectionCompletionAction::IGNORE_STALE as i32 {
            if unsafe { raft_election_debug_enabled() } {
                rusty::raft_log_info_5(
                    "[RAFT_ELECTION] server {} ignoring stale election result: result_term={} local_term={} election_term={} active={}",
                    self.site_id_, term, self.state_.current_term_,
                    self.state_.election_term_,
                    self.state_.election_in_progress_);
            }
            return false;
        }
        unsafe {
            raft_verify(completion_action
                == ElectionCompletionAction::APPLY_CURRENT as i32);
        }
        if unsafe { raft_election_debug_enabled() } {
            rusty::raft_log_info_6(
                "[RAFT_ELECTION] server {} term {} vote outcome yes={} no={} highest_term_seen={} timeout={}",
                self.site_id_, term, outcome.n_voted_yes_, outcome.n_voted_no_,
                outcome.term_, outcome.timeouted_);
        }

        if outcome.yes_ {
            unsafe {
                raft_verify(self.state_.current_term_ >= term);
            }
            self.state_.election_in_progress_ = false;
            self.state_.req_voting_ = false;

            if self.stop_.load(rusty::sync::atomic::Ordering::Acquire)
                || self.state_.current_term_ != term
            {
                self.state_.req_voting_ = false;
                return false;
            }

            self.setIsLeader(true);
            rusty::raft_log_debug_2("site {} became leader for term {}",
                                    self.site_id_, term);
            if unsafe { raft_election_debug_enabled() } {
                rusty::raft_log_info_4(
                    "[RAFT_ELECTION] server {} won election term {} (votes yes={} no={})",
                    self.site_id_, term, outcome.n_voted_yes_,
                    outcome.n_voted_no_);
            }

            if self.IsLeaderLocked() {
                rusty::raft_log_debug_2("vote accepted {} curterm {}",
                                        loc_id, self.state_.current_term_);
                self.state_.req_voting_ = false;
                true
            } else {
                rusty::raft_log_debug_2("vote rejected {} curterm {}, do rollback",
                                        loc_id, self.state_.current_term_);
                self.setIsLeader(false);
                false
            }
        } else if outcome.no_ {
            rusty::raft_log_debug_1("site {} requestvote rejected", self.site_id_);
            self.setIsLeader(false);
            if unsafe { raft_election_debug_enabled() } {
                rusty::raft_log_info_5(
                    "[RAFT_ELECTION] server {} lost election term {} (yes={} no={}) highest_term={}",
                    self.site_id_, term, outcome.n_voted_yes_,
                    outcome.n_voted_no_, outcome.term_);
            }
            if self.state_.election_in_progress_
                && self.state_.election_term_ == term as i64
            {
                self.state_.election_in_progress_ = false;
            }
            self.state_.req_voting_ = false;
            false
        } else {
            rusty::raft_log_debug_1("vote timeout {}", loc_id);
            if unsafe { raft_election_debug_enabled() } {
                rusty::raft_log_info_4(
                    "[RAFT_ELECTION] server {} election timed out term {} (yes={} no={})",
                    self.site_id_, term, outcome.n_voted_yes_,
                    outcome.n_voted_no_);
            }
            if self.state_.election_in_progress_
                && self.state_.election_term_ == term as i64
            {
                self.state_.election_in_progress_ = false;
            }
            self.state_.req_voting_ = false;
            false
        }
    }

    pub fn stepDown(&mut self) {
        rusty::raft_log_info_2(
            "[SPEC-RAFT] Site {}: Stepping down as leader (term={})",
            self.site_id_, self.state_.current_term_);

        // Handles the leadership-change callback, the timer reset, and the
        // rest of the follower transition.
        self.setIsLeader(false);

        // A late higher-term response can arrive after this server has
        // already entered a new candidacy.
        self.state_.req_voting_ = false;
        self.state_.election_in_progress_ = false;

        self.resetTimerLocked("stepDown");

        rusty::raft_log_info_1(
            "[SPEC-RAFT] Site {}: Step-down complete, now follower",
            self.site_id_);
    }
}

// The three methods a worker reaches through a TxLogServer base pointer.
// Raft's set_site_identity mirrors the ids into state_ as well, where
// converted Rust bodies can see them, and asserts the copies agree.
#[cpp_inherit]
impl TxLogServer for RaftServerBase {
    fn set_site_identity(&mut self, loc_id: u32, site_id: u16, partition_id: u32) {
        self.loc_id_ = loc_id;
        self.site_id_ = site_id;
        self.partition_id_ = partition_id;
        self.state_.loc_id_ = loc_id;
        self.state_.site_id_ = site_id;
        self.state_.partition_id_ = partition_id;
        unsafe {
            raft_verify(self.state_.site_id_ == self.site_id_
                && self.state_.partition_id_ == self.partition_id_
                && self.state_.loc_id_ == self.loc_id_);
        }
    }

    fn set_commo(&mut self, commo: *mut rusty::Communicator) {
        self.commo_ = commo;
    }

    fn reg_learner_action(&mut self, learner_action: rusty::LearnerAction) {
        self.app_next_ = learner_action;
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=raft_server.server_state version=1 rust_sha256=fca7362aee83eaafc29411c775ce936e8a733ab4f468de3a50430cd1c58951df*/
struct RaftVoteOutcome;
struct RaftServerBase;

// Rust-only compiler marker import: rusty::cpp_inherit

using ::scheduler_h::TxLogServer;

extern "C" {
    void raft_verify(bool condition);
    uint64_t raft_time_now_us();
    bool raft_snapshot_manager_is_set(const rusty::RaftSnapshotManagerPtr* manager);
    uint64_t raft_random_range_us(uint64_t low, uint64_t high);
    void raft_rebuild_peer_tables(RaftServerBase* server, uint64_t next_index);
    uint16_t raft_peer_site_at(const RaftServerBase* server, size_t ordinal);
    bool raft_leader_change_cb_is_set(const RaftServerBase* server);
    void raft_fire_leader_change(RaftServerBase* server, bool is_leader);
    uint64_t raft_preferred_leader_grace_period_us();
    uint64_t raft_preferred_election_timeout_us();
    uint64_t raft_non_preferred_grace_election_timeout_us();
    uint64_t raft_non_preferred_steady_election_timeout_us();
    void raft_log_set_is_leader_entry(const RaftServerBase* server, bool prev_is_leader, bool new_is_leader);
    void raft_append_leader_noop(RaftServerBase* server);
    bool raft_election_debug_enabled();
    rusty::RaftVoteQuorumPtr raft_broadcast_vote_and_wait(RaftServerBase* server, uint32_t par_id, uint64_t last_log_index, int64_t last_log_term, uint16_t self_site_id, int64_t term);
    RaftVoteOutcome raft_vote_quorum_snapshot(const rusty::RaftVoteQuorumPtr* quorum);
    bool raft_snapshot_manager_has_latest(const rusty::RaftSnapshotManagerPtr* manager);
    bool raft_command_has_value(const rusty::RaftCommand* cmd);
    uint64_t raft_apply_queue_push_range(RaftServerBase* server, uint64_t first, uint64_t last);
    uint64_t raft_apply_queue_size(RaftServerBase* server);
    void raft_startup_wait(RaftServerBase* server);
    void raft_startup_notify_all(RaftServerBase* server);
    void raft_apply_thread_join(RaftServerBase* server);
    void raft_commo_set_network_enabled(RaftServerBase* server, bool enabled);
    bool raft_create_snapshot_locked(RaftServerBase* server);
    void raft_request_replication(RaftServerBase* server);
    RaftStartResult raft_set_local_append(RaftServerBase* server, const rusty::RaftCommand* cmd, uint64_t* term, uint64_t* index, uint64_t slot_id, int64_t ballot);
    void raft_close_replication_wake_gate(RaftServerBase* server);
    void raft_spawn_election_timer(RaftServerBase* server, uint64_t wait_int_us);
    bool raft_setup_internal_guarded(RaftServerBase* server);
    void raft_shutdown_barrier_yield();
    size_t raft_peer_sites_len(const RaftServerBase* server);
}

struct RaftVoteOutcome {
    int64_t term_;
    bool yes_;
    bool no_;
    int32_t n_voted_yes_;
    int32_t n_voted_no_;
    bool timeouted_;
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
};

struct RaftServerBase : public TxLogServer {
    uint32_t loc_id_;
    uint16_t site_id_;
    rusty::LearnerAction app_next_;
    rusty::Communicator* commo_;
    uint32_t partition_id_;
    rusty::RaftCheckedMutex mtx_;
    RaftConsensusState state_;
    rusty::RaftDecodedTerms decoded_terms_;
    rusty::RaftAsyncCallbackLifetimePtr async_callback_lifetime_;
    rusty::RaftSnapshotManagerPtr snapshot_manager_;
    rusty::sync::atomic::AtomicBool snapshot_manager_configured_;
    rusty::sync::atomic::AtomicU64 snapshot_trigger_index_;
    rusty::sync::atomic::AtomicU64 snapshot_trigger_threshold_;
    rusty::RaftCreateSnapshotCb create_sm_snapshot_cb_;
    rusty::RaftPrepareSnapshotCb prepare_sm_snapshot_cb_;
    rusty::RaftPeerSites peer_sites_;
    rusty::sync::atomic::AtomicBool stop_;
    rusty::sync::atomic::AtomicBool rpc_ready_;
    rusty::RaftStdMutex startup_mtx_;
    rusty::RaftStdCondVar startup_cv_;
    bool startup_finished_;
    bool startup_succeeded_;
    int32_t wait_int_;
    rusty::sync::atomic::AtomicBool disconnected_;
    bool in_applying_logs_;
    bool failover_;
    rusty::sync::atomic::AtomicBool looping_;
    rusty::sync::atomic::AtomicBool heartbeat_loop_running_;
    rusty::sync::atomic::AtomicBool election_loop_running_;
    bool heartbeat_;
    bool heartbeat_setup_;
    uint64_t heartbeat_interval_us_;
    uint64_t log_retention_window_;
    rusty::RaftLeaderChangeCb leader_change_cb_;
    uint16_t preferred_leader_site_id_;
    uint64_t startup_timestamp_;
    rusty::RaftSiteIdSet current_config_;
    rusty::RaftStdThread apply_thread_;
    rusty::sync::atomic::AtomicBool apply_thread_running_;
    rusty::RaftStdMutex state_machine_apply_mtx_;
    rusty::RaftStdMutex apply_queue_mtx_;
    uint64_t apply_queue_epoch_;
    rusty::RaftApplyQueue apply_queue_;
    rusty::sync::atomic::AtomicU64 appliedIndexForWait_;
    uint64_t enqueue_log_counter_;
    int32_t n_prepare_;
    int32_t n_accept_;
    int32_t n_commit_;

    RaftServerBase();
    uint64_t GetSnapshotIndexLocked() const;
    uint64_t GetSnapshotIndex();
    uint64_t GetSnapshotTermLocked() const;
    uint64_t GetSnapshotTerm();
    void SetSnapshotThresholdLocked(uint64_t threshold);
    void SetSnapshotThreshold(uint64_t threshold);
    bool IsDisconnected() const;
    uint16_t GetLeaderHint();
    bool HeartbeatLooping() const;
    void HeartbeatEpilogue();
    bool ElectionLoopStopped() const;
    bool ElectionLoopVoting();
    void ElectionLoopSetRunning(bool running);
    ElectionTick ElectionLoopGather();
    int64_t ElectionLastLogTermLocked() const;
    uint64_t SetStateMachineSnapshotCallbacks(rusty::RaftCreateSnapshotCb create_cb, rusty::RaftPrepareSnapshotCb prepare_cb);
    bool ClearStateMachineSnapshotCallbacks(uint64_t callback_owner_token);
    void SetSnapshotManagerLocked(rusty::RaftSnapshotManagerPtr manager);
    void SetSnapshotManager(rusty::RaftSnapshotManagerPtr manager);
    void RegisterLeaderChangeCallback(rusty::RaftLeaderChangeCb cb);
    bool AmIPreferredLeader() const;
    bool IsRpcReady() const;
    uint64_t GetAppliedIndex() const;
    bool IsLeaderLocked() const;
    bool IsLeader();
    void GetState(bool* is_leader, uint64_t* term);
    uint64_t GetHeartbeatInterval() const;
    void SetHeartbeatInterval(uint64_t micros);
    uint64_t GetLogRetentionWindow() const;
    void SetLogRetentionWindow(uint64_t window);
    uint64_t GetSnapshotThreshold() const;
    void SetPreferredLeader(uint16_t site_id);
    void PublishAppliedIndexLocked(uint64_t index);
    void PublishAppliedIndex(uint64_t index);
    size_t CompactLogLocked(uint64_t up_to_index);
    size_t CompactLog(uint64_t up_to_index);
    void LogTermChange(std::string_view reason, uint64_t old_term, uint64_t new_term, uint16_t source) const;
    uint64_t ElectionLoopRandomDelay() const;
    void ElectionLoopLogStart() const;
    void ElectionLoopLogFired(const ElectionTick& tick) const;
    void RebuildPeerTables(uint64_t next_index);
    uint64_t GetElectionTimeout() const;
    void resetTimerLocked(std::string_view reason);
    void resetTimer(std::string_view reason);
    void setIsLeader(bool is_leader);
    size_t PeerOrdinal(uint16_t site) const;
    void RequestReplication();
    void HeartbeatPrologue();
    void MaybeCreateSnapshot();
    bool HasSnapshot();
    void Disconnect(bool disconnect);
    void Reconnect();
    void EnsureSetup();
    void Setup();
    bool WaitForStartup();
    void StartElectionTimer();
    void PrepareForShutdown();
    RaftStartResult StartImpl(const rusty::RaftCommand* cmd, uint64_t* index, uint64_t* term, uint64_t slot_id, int64_t ballot);
    void EnqueueCommittedEntries(uint64_t old_commit, uint64_t new_commit);
    bool RequestVoteImpl(bool timer_guarded, uint64_t expected_generation);
    void stepDown();
    void set_site_identity(uint32_t loc_id, uint16_t site_id, uint32_t partition_id);
    void set_commo(rusty::Communicator* commo);
    void reg_learner_action(rusty::LearnerAction learner_action);
};


inline RaftServerBase::RaftServerBase()
    : TxLogServer()
    , loc_id_(static_cast<uint32_t>(4294967295))
    , site_id_(RAFT_SERVER_INVALID_SITE_ID)
    , app_next_(rusty::default_like<rusty::LearnerAction>())
    , commo_(rusty::ptr::null_mut())
    , partition_id_(static_cast<uint32_t>(0))
    , mtx_(rusty::default_like<rusty::RaftCheckedMutex>())
    , state_(RaftConsensusState::new_())
    , decoded_terms_(rusty::default_like<rusty::RaftDecodedTerms>())
    , async_callback_lifetime_(rusty::default_like<rusty::RaftAsyncCallbackLifetimePtr>())
    , snapshot_manager_(rusty::default_like<rusty::RaftSnapshotManagerPtr>())
    , snapshot_manager_configured_(rusty::sync::atomic::AtomicBool::new_(false))
    , snapshot_trigger_index_(rusty::sync::atomic::AtomicU64::new_(0))
    , snapshot_trigger_threshold_(rusty::sync::atomic::AtomicU64::new_(10000))
    , create_sm_snapshot_cb_(rusty::default_like<rusty::RaftCreateSnapshotCb>())
    , prepare_sm_snapshot_cb_(rusty::default_like<rusty::RaftPrepareSnapshotCb>())
    , peer_sites_(rusty::default_like<rusty::RaftPeerSites>())
    , stop_(rusty::sync::atomic::AtomicBool::new_(false))
    , rpc_ready_(rusty::sync::atomic::AtomicBool::new_(false))
    , startup_mtx_(rusty::default_like<rusty::RaftStdMutex>())
    , startup_cv_(rusty::default_like<rusty::RaftStdCondVar>())
    , startup_finished_(false)
    , startup_succeeded_(false)
    , wait_int_(static_cast<int32_t>(100000))
    , disconnected_(rusty::sync::atomic::AtomicBool::new_(false))
    , in_applying_logs_(false)
    , failover_(true)
    , looping_(rusty::sync::atomic::AtomicBool::new_(false))
    , heartbeat_loop_running_(rusty::sync::atomic::AtomicBool::new_(false))
    , election_loop_running_(rusty::sync::atomic::AtomicBool::new_(false))
    , heartbeat_(true)
    , heartbeat_setup_(false)
    , heartbeat_interval_us_(static_cast<uint64_t>(0))
    , log_retention_window_(static_cast<uint64_t>(5000))
    , leader_change_cb_(rusty::default_like<rusty::RaftLeaderChangeCb>())
    , preferred_leader_site_id_(RAFT_SERVER_INVALID_SITE_ID)
    , startup_timestamp_(static_cast<uint64_t>(0))
    , current_config_(rusty::default_like<rusty::RaftSiteIdSet>())
    , apply_thread_(rusty::default_like<rusty::RaftStdThread>())
    , apply_thread_running_(rusty::sync::atomic::AtomicBool::new_(false))
    , state_machine_apply_mtx_(rusty::default_like<rusty::RaftStdMutex>())
    , apply_queue_mtx_(rusty::default_like<rusty::RaftStdMutex>())
    , apply_queue_epoch_(static_cast<uint64_t>(0))
    , apply_queue_(rusty::default_like<rusty::RaftApplyQueue>())
    , appliedIndexForWait_(rusty::sync::atomic::AtomicU64::new_(0))
    , enqueue_log_counter_(static_cast<uint64_t>(0))
    , n_prepare_(static_cast<int32_t>(0))
    , n_accept_(static_cast<int32_t>(0))
    , n_commit_(static_cast<int32_t>(0))
{}

inline uint64_t RaftServerBase::GetSnapshotIndexLocked() const {
    return this->state_.snapidx_;
}

inline uint64_t RaftServerBase::GetSnapshotIndex() {
    const auto _lock = RaftLockGuard::new_(&this->mtx_);
    return this->GetSnapshotIndexLocked();
}

inline uint64_t RaftServerBase::GetSnapshotTermLocked() const {
    return static_cast<uint64_t>(this->state_.snapterm_);
}

inline uint64_t RaftServerBase::GetSnapshotTerm() {
    const auto _lock = RaftLockGuard::new_(&this->mtx_);
    return this->GetSnapshotTermLocked();
}

inline void RaftServerBase::SetSnapshotThresholdLocked(uint64_t threshold) {
    this->state_.snapshot_threshold_ = std::move(threshold);
    this->snapshot_trigger_threshold_.store(std::move(threshold), rusty::sync::atomic::Ordering::Release);
}

inline void RaftServerBase::SetSnapshotThreshold(uint64_t threshold) {
    const auto _lock = RaftLockGuard::new_(&this->mtx_);
    this->SetSnapshotThresholdLocked(std::move(threshold));
}

inline bool RaftServerBase::IsDisconnected() const {
    return this->disconnected_.load(rusty::sync::atomic::Ordering::Acquire);
}

inline uint16_t RaftServerBase::GetLeaderHint() {
    const auto _lock = RaftLockGuard::new_(&this->mtx_);
    if (this->state_.is_leader_) {
        return this->site_id_;
    }
    return this->state_.current_leader_id_;
}

inline bool RaftServerBase::HeartbeatLooping() const {
    return this->looping_.load(rusty::sync::atomic::Ordering::Acquire);
}

inline void RaftServerBase::HeartbeatEpilogue() {
    this->looping_.store(false, rusty::sync::atomic::Ordering::Release);
    this->heartbeat_loop_running_.store(false, rusty::sync::atomic::Ordering::Release);
}

inline bool RaftServerBase::ElectionLoopStopped() const {
    return this->stop_.load(rusty::sync::atomic::Ordering::Acquire);
}

inline bool RaftServerBase::ElectionLoopVoting() {
    const auto _lock = RaftLockGuard::new_(&this->mtx_);
    return this->state_.req_voting_;
}

inline void RaftServerBase::ElectionLoopSetRunning(bool running) {
    this->election_loop_running_.store(std::move(running), rusty::sync::atomic::Ordering::Release);
}

inline ElectionTick RaftServerBase::ElectionLoopGather() {
    const auto _lock = RaftLockGuard::new_(&this->mtx_);
    const uint64_t time_now = raft_time_now_us();
    uint64_t heartbeat_time = this->state_.last_heartbeat_time_;
    uint64_t time_elapsed = rusty::detail::deref_if_pointer_like(time_now) - rusty::detail::deref_if_pointer_like(heartbeat_time);
    uint64_t election_timeout = this->state_.election_timeout_us_;
    return ElectionTick::new_(std::move(time_elapsed), std::move(election_timeout), std::move(heartbeat_time), this->state_.election_timer_generation_, this->state_.current_term_, this->state_.vote_for_, raft_server_election_timeout_has_fired(this->state_.is_leader_, std::move(time_elapsed), std::move(election_timeout)));
}

inline int64_t RaftServerBase::ElectionLastLogTermLocked() const {
    const uint64_t last_index = this->state_.raft_log_.last_index();
    // @unsafe
    {
        raft_verify(rusty::detail::deref_if_pointer_like(last_index) >= rusty::detail::deref_if_pointer_like(this->state_.snapidx_));
    }
    if (raft_server_election_last_log_uses_snapshot(std::move(last_index), this->state_.snapidx_)) {
        return this->state_.snapterm_;
    }
    auto last_log = this->state_.raft_log_.get(std::move(last_index));
    // @unsafe
    {
        raft_verify(last_log.is_some());
    }
    return last_log.unwrap().term();
}

inline uint64_t RaftServerBase::SetStateMachineSnapshotCallbacks(rusty::RaftCreateSnapshotCb create_cb, rusty::RaftPrepareSnapshotCb prepare_cb) {
    const auto _lock = RaftLockGuard::new_(&this->mtx_);
    if (rusty::detail::deref_if_pointer_like(this->state_.next_snapshot_callback_owner_token_) == 0) {
        this->state_.next_snapshot_callback_owner_token_ = 1;
    }
    uint64_t owner_token = this->state_.next_snapshot_callback_owner_token_;
    rusty::detail::deref_if_pointer_like(this->state_.next_snapshot_callback_owner_token_) += 1;
    this->create_sm_snapshot_cb_ = std::move(create_cb);
    this->prepare_sm_snapshot_cb_ = std::move(prepare_cb);
    this->state_.snapshot_callback_owner_token_ = std::move(owner_token);
    return std::move(owner_token);
}

inline bool RaftServerBase::ClearStateMachineSnapshotCallbacks(uint64_t callback_owner_token) {
    if (rusty::detail::deref_if_pointer_like(callback_owner_token) == static_cast<uint64_t>(0)) {
        return false;
    }
    const auto _lock = RaftLockGuard::new_(&this->mtx_);
    if (rusty::detail::deref_if_pointer_like(this->state_.snapshot_callback_owner_token_) != rusty::detail::deref_if_pointer_like(callback_owner_token)) {
        return false;
    }
    this->create_sm_snapshot_cb_ = rusty::default_like<rusty::RaftCreateSnapshotCb>();
    this->prepare_sm_snapshot_cb_ = rusty::default_like<rusty::RaftPrepareSnapshotCb>();
    this->state_.snapshot_callback_owner_token_ = 0;
    return true;
}

inline void RaftServerBase::SetSnapshotManagerLocked(rusty::RaftSnapshotManagerPtr manager) {
    this->snapshot_manager_ = std::move(manager);
    const bool configured = raft_snapshot_manager_is_set(&this->snapshot_manager_);
    this->snapshot_manager_configured_.store(std::move(configured), rusty::sync::atomic::Ordering::Release);
}

inline void RaftServerBase::SetSnapshotManager(rusty::RaftSnapshotManagerPtr manager) {
    const auto _lock = RaftLockGuard::new_(&this->mtx_);
    this->SetSnapshotManagerLocked(std::move(manager));
}

inline void RaftServerBase::RegisterLeaderChangeCallback(rusty::RaftLeaderChangeCb cb) {
    this->leader_change_cb_ = std::move(cb);
}

inline bool RaftServerBase::AmIPreferredLeader() const {
    return raft_server_site_is_preferred_leader(this->site_id_, this->preferred_leader_site_id_);
}

inline bool RaftServerBase::IsRpcReady() const {
    return this->rpc_ready_.load(rusty::sync::atomic::Ordering::Acquire);
}

inline uint64_t RaftServerBase::GetAppliedIndex() const {
    return this->appliedIndexForWait_.load(rusty::sync::atomic::Ordering::Acquire);
}

inline bool RaftServerBase::IsLeaderLocked() const {
    if (rusty::detail::rust_not(this->looping_.load(rusty::sync::atomic::Ordering::Acquire))) {
        return false;
    }
    return this->state_.is_leader_;
}

inline bool RaftServerBase::IsLeader() {
    if (rusty::detail::rust_not(this->looping_.load(rusty::sync::atomic::Ordering::Acquire))) {
        return false;
    }
    const auto _lock = RaftLockGuard::new_(&this->mtx_);
    return this->state_.is_leader_;
}

inline void RaftServerBase::GetState(bool* is_leader, uint64_t* term) {
    const auto _lock = RaftLockGuard::new_(&this->mtx_);
    bool leading = this->IsLeaderLocked();
    // @unsafe
    {
        *is_leader = std::move(leading);
        *term = this->state_.current_term_;
    }
}

inline uint64_t RaftServerBase::GetHeartbeatInterval() const {
    return this->heartbeat_interval_us_;
}

inline void RaftServerBase::SetHeartbeatInterval(uint64_t micros) {
    this->heartbeat_interval_us_ = std::move(micros);
}

inline uint64_t RaftServerBase::GetLogRetentionWindow() const {
    return this->log_retention_window_;
}

inline void RaftServerBase::SetLogRetentionWindow(uint64_t window) {
    this->log_retention_window_ = raft_server_retention_window_normalize(std::move(window));
}

inline uint64_t RaftServerBase::GetSnapshotThreshold() const {
    return this->snapshot_trigger_threshold_.load(rusty::sync::atomic::Ordering::Acquire);
}

inline void RaftServerBase::SetPreferredLeader(uint16_t site_id) {
    const auto _lock = RaftLockGuard::new_(&this->mtx_);
    const uint16_t old_preferred = this->preferred_leader_site_id_;
    this->preferred_leader_site_id_ = std::move(site_id);
    if (rusty::detail::deref_if_pointer_like(old_preferred) != rusty::detail::deref_if_pointer_like(site_id)) {
        rusty::raft_log_info_2("[LEADERSHIP-TRANSFER] Site {}: Preferred leader set to {}", this->site_id_, std::move(site_id));
    }
}

inline void RaftServerBase::PublishAppliedIndexLocked(uint64_t index) {
    const uint64_t published = this->GetAppliedIndex();
    if (raft_server_log_index_above(std::move(published), std::move(index))) {
        rusty::raft_log_warn_3("[RAFT-APPLY] Site {} refusing to move applied index backward from {} to {}", this->site_id_, std::move(published), std::move(index));
        return;
    }
    this->state_.execute_index_ = std::move(index);
    this->appliedIndexForWait_.store(std::move(index), rusty::sync::atomic::Ordering::Release);
}

inline void RaftServerBase::PublishAppliedIndex(uint64_t index) {
    const auto _lock = RaftLockGuard::new_(&this->mtx_);
    this->PublishAppliedIndexLocked(std::move(index));
}

inline size_t RaftServerBase::CompactLogLocked(uint64_t up_to_index) {
    const uint64_t requested_index = up_to_index;
    const uint64_t safe_index = raft_server_compaction_safe_index(std::move(up_to_index), this->state_.commit_index_, this->state_.snapidx_);
    if (rusty::detail::deref_if_pointer_like(safe_index) != rusty::detail::deref_if_pointer_like(requested_index)) {
        rusty::raft_log_warn_5("[RAFT-COMPACT] Site {}: Clamped compaction {} -> {} (state_.commit_index_={}, snapidx={})", this->site_id_, std::move(requested_index), std::move(safe_index), this->state_.commit_index_, this->state_.snapidx_);
    }
    if (rusty::detail::rust_not(raft_server_log_index_has_successor(std::move(safe_index)))) {
        rusty::raft_log_error_2("[RAFT-COMPACT] Site {}: Refusing terminal compaction index {}; the exclusive storage bound and min_active_slot would wrap", this->site_id_, std::move(safe_index));
        return static_cast<size_t>(0);
    }
    size_t removed_memory = this->state_.raft_log_.compact_through(std::move(safe_index));
    rusty::raft_log_info_3("[RAFT-COMPACT] Site {}: Compacted in-memory entries through {} (memory={})", this->site_id_, std::move(safe_index), std::move(removed_memory));
    return std::move(removed_memory);
}

inline size_t RaftServerBase::CompactLog(uint64_t up_to_index) {
    const auto _lock = RaftLockGuard::new_(&this->mtx_);
    return this->CompactLogLocked(std::move(up_to_index));
}

inline void RaftServerBase::LogTermChange(std::string_view reason, uint64_t old_term, uint64_t new_term, uint16_t source) const {
    if (rusty::detail::deref_if_pointer_like(old_term) == rusty::detail::deref_if_pointer_like(new_term)) {
        return;
    }
    if (rusty::detail::deref_if_pointer_like(source) != rusty::detail::deref_if_pointer_like(RAFT_SERVER_INVALID_SITE_ID)) {
        rusty::raft_log_info_5("[RAFT-TERM] server {} term {} -> {} ({}, source_site={})", this->site_id_, std::move(old_term), std::move(new_term), reason, std::move(source));
    } else {
        rusty::raft_log_info_4("[RAFT-TERM] server {} term {} -> {} ({})", this->site_id_, std::move(old_term), std::move(new_term), reason);
    }
}

inline uint64_t RaftServerBase::ElectionLoopRandomDelay() const {
    // @unsafe
    {
        return raft_random_range_us(rusty::detail::deref_if_pointer_like(this->heartbeat_interval_us_) * static_cast<uint64_t>(2), rusty::detail::deref_if_pointer_like(this->heartbeat_interval_us_) * static_cast<uint64_t>(4));
    }
}

inline void RaftServerBase::ElectionLoopLogStart() const {
    rusty::raft_log_debug_0("start timer for election");
}

inline void RaftServerBase::ElectionLoopLogFired(const ElectionTick& tick) const {
    rusty::raft_log_info_3("[ELECTION_TIMER] Site {}: TIMEOUT FIRED - starting election (elapsed={} > timeout={})", this->site_id_, tick.time_elapsed(), tick.election_timeout());
    rusty::raft_log_info_6("[ELECTION_START] Site {}: TRIGGERING REQUESTVOTE - time_elapsed={} > timeout={} last_hb={} current_term={} vote_for={}", this->site_id_, tick.time_elapsed(), tick.election_timeout(), tick.heartbeat_time(), tick.term(), tick.vote_for());
}

inline void RaftServerBase::RebuildPeerTables(uint64_t next_index) {
    // @unsafe
    {
        raft_rebuild_peer_tables(static_cast<RaftServerBase*>(rusty::detail::ptr_or_addr((*this))), std::move(next_index));
    }
}

inline uint64_t RaftServerBase::GetElectionTimeout() const {
    const uint64_t current_time = raft_time_now_us();
    const uint64_t grace_period_us = raft_preferred_leader_grace_period_us();
    const bool in_grace_period = ((rusty::detail::deref_if_pointer_like(current_time) - rusty::detail::deref_if_pointer_like(this->startup_timestamp_))) < rusty::detail::deref_if_pointer_like(grace_period_us);
    const bool preferred_leader_configured = rusty::detail::deref_if_pointer_like(this->preferred_leader_site_id_) != rusty::detail::deref_if_pointer_like(RAFT_SERVER_INVALID_SITE_ID);
    if (!preferred_leader_configured) {
        // @unsafe
        {
            return raft_non_preferred_steady_election_timeout_us();
        }
    } else if (this->AmIPreferredLeader()) {
        // @unsafe
        {
            return raft_preferred_election_timeout_us();
        }
    } else if (in_grace_period) {
        // @unsafe
        {
            return raft_non_preferred_grace_election_timeout_us();
        }
    } else {
        // @unsafe
        {
            return raft_non_preferred_steady_election_timeout_us();
        }
    }
}

inline void RaftServerBase::resetTimerLocked(std::string_view reason) {
    const uint64_t prev_time = this->state_.last_heartbeat_time_;
    this->state_.last_heartbeat_time_ = raft_time_now_us();
    this->state_.election_timeout_us_ = this->GetElectionTimeout();
    if (rusty::detail::deref_if_pointer_like(this->state_.election_timer_generation_) == rusty::detail::deref_if_pointer_like(std::numeric_limits<uint64_t>::max())) {
        this->state_.election_timer_generation_ = 1;
    } else {
        rusty::detail::deref_if_pointer_like(this->state_.election_timer_generation_) += 1;
    }
    if ((rusty::detail::deref_if_pointer_like(rusty::to_string_view(reason)) == std::string_view("granted vote")) || (rusty::detail::deref_if_pointer_like(rusty::to_string_view(reason)) == std::string_view("start election timer"))) {
        rusty::raft_log_info_7("[TIMER_RESET] Site {}: reset timer ({}) - prev_hb_time={} new_hb_time={} delta={} timeout={} generation={}", this->site_id_, reason, std::move(prev_time), this->state_.last_heartbeat_time_, rusty::detail::deref_if_pointer_like(this->state_.last_heartbeat_time_) - rusty::detail::deref_if_pointer_like(prev_time), this->state_.election_timeout_us_, this->state_.election_timer_generation_);
    }
}

inline void RaftServerBase::resetTimer(std::string_view reason) {
    const auto _lock = RaftLockGuard::new_(&this->mtx_);
    this->resetTimerLocked(rusty::to_string_view(reason));
}

inline void RaftServerBase::setIsLeader(bool is_leader) {
    bool prev_is_leader = this->state_.is_leader_;
    // @unsafe
    {
        raft_log_set_is_leader_entry(static_cast<const RaftServerBase*>(rusty::detail::ptr_or_addr((*this))), std::move(prev_is_leader), std::move(is_leader));
    }
    if (rusty::detail::deref_if_pointer_like(is_leader) && !prev_is_leader) {
        const uint64_t publication_term = this->state_.current_term_;
        if (this->stop_.load(rusty::sync::atomic::Ordering::Acquire) || (rusty::detail::deref_if_pointer_like(this->state_.current_term_) != rusty::detail::deref_if_pointer_like(publication_term))) {
            rusty::raft_log_warn_4("[RAFT_STATE] Site {} suppressing stale leadership publication for term {} (current={}, stopping={})", this->site_id_, std::move(publication_term), this->state_.current_term_, this->stop_.load(rusty::sync::atomic::Ordering::Acquire));
            return;
        }
    }
    if (is_leader) {
        this->state_.heartbeat_round_ = 0;
        this->state_.read_quorum_confirmed_term_ = 0;
        this->state_.read_quorum_confirmed_round_ = 0;
    }
    if (rusty::detail::deref_if_pointer_like(is_leader) && rusty::detail::deref_if_pointer_like(this->failover_)) {
        uint64_t next_index = this->state_.raft_log_.last_index() + static_cast<uint64_t>(1);
        this->RebuildPeerTables(std::move(next_index));
        const size_t peers = rusty::len(this->state_.peers_);
        size_t ord = static_cast<size_t>(0);
        while (rusty::detail::deref_if_pointer_like(ord) < rusty::detail::deref_if_pointer_like(peers)) {
            const uint16_t site = raft_peer_site_at(static_cast<const RaftServerBase*>(rusty::detail::ptr_or_addr((*this))), std::move(ord));
            rusty::raft_log_debug_5("loc_id_={} match_index_[{}]={}, next_index_[{}]={}", this->loc_id_, std::move(site), this->state_.peers_.match_index(std::move(ord)), std::move(site), this->state_.peers_.next_index(std::move(ord)));
            ord += 1;
        }
    }
    const bool become_new_leader = rusty::detail::deref_if_pointer_like(is_leader) && rusty::detail::rust_not(this->state_.is_leader_);
    const bool become_new_follower = !is_leader && rusty::detail::deref_if_pointer_like(this->state_.is_leader_);
    this->state_.is_leader_ = std::move(is_leader);
    this->state_.current_leader_id_ = raft_server_leader_hint_after_transition(std::move(is_leader), !is_leader && (rusty::detail::deref_if_pointer_like(this->state_.current_leader_id_) != rusty::detail::deref_if_pointer_like(RAFT_SERVER_INVALID_SITE_ID)), this->site_id_, this->state_.current_leader_id_);
    if (rusty::detail::deref_if_pointer_like(become_new_leader) || rusty::detail::deref_if_pointer_like(become_new_follower)) {
        rusty::raft_log_info_4("RaftServer::setIsLeader site_id_ {} become_new_leader {} become_new_follower {} isLeader {}", this->site_id_, std::move(become_new_leader), std::move(become_new_follower), std::move(is_leader));
    }
    if (become_new_leader) {
        rusty::raft_log_info_4("[RAFT_STATE] setIsLeader transition LEADER: site {} term {} prev_is_leader={} become_new_leader={}", this->site_id_, this->state_.current_term_, std::move(prev_is_leader), std::move(become_new_leader));
        // @unsafe
        {
            raft_append_leader_noop(static_cast<RaftServerBase*>(rusty::detail::ptr_or_addr((*this))));
        }
    } else if (become_new_follower) {
        rusty::raft_log_info_4("[RAFT_STATE] setIsLeader transition FOLLOWER: site {} term {} prev_is_leader={} become_new_follower={}", this->site_id_, this->state_.current_term_, std::move(prev_is_leader), std::move(become_new_follower));
        this->resetTimerLocked(std::string_view("became follower"));
        rusty::raft_log_info_2("[RAFT_TIMER] Site {} reset election timer when becoming follower (last_hb now={})", this->site_id_, this->state_.last_heartbeat_time_);
        rusty::raft_log_info_2("[RAFT_VIEW] Server {} stepping down as leader for partition {}", this->site_id_, this->partition_id_);
    }
    if (raft_leader_change_cb_is_set(static_cast<const RaftServerBase*>(rusty::detail::ptr_or_addr((*this))))) {
        if (become_new_leader) {
            rusty::raft_log_info_1("[LEADER_CALLBACK] Site {}: Firing leader_change_cb_(true) - became leader", this->site_id_);
            // @unsafe
            {
                raft_fire_leader_change(static_cast<RaftServerBase*>(rusty::detail::ptr_or_addr((*this))), true);
            }
        } else if (become_new_follower) {
            rusty::raft_log_info_1("[LEADER_CALLBACK] Site {}: Firing leader_change_cb_(false) - became follower", this->site_id_);
            // @unsafe
            {
                raft_fire_leader_change(static_cast<RaftServerBase*>(rusty::detail::ptr_or_addr((*this))), false);
            }
        }
    }
}

inline size_t RaftServerBase::PeerOrdinal(uint16_t site) const {
    const size_t count = raft_peer_sites_len(static_cast<const RaftServerBase*>(rusty::detail::ptr_or_addr((*this))));
    size_t ord = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(ord) < rusty::detail::deref_if_pointer_like(count)) {
        if (raft_peer_site_at(static_cast<const RaftServerBase*>(rusty::detail::ptr_or_addr((*this))), std::move(ord)) == rusty::detail::deref_if_pointer_like(site)) {
            return std::move(ord);
        }
        ord += 1;
    }
    return rusty::len(this->state_.peers_);
}

inline void RaftServerBase::RequestReplication() {
    // @unsafe
    {
        raft_request_replication(static_cast<RaftServerBase*>(rusty::detail::ptr_or_addr((*this))));
    }
}

inline void RaftServerBase::HeartbeatPrologue() {
    this->heartbeat_loop_running_.store(true, rusty::sync::atomic::Ordering::Release);
    {
        const auto _lock = RaftLockGuard::new_(&this->mtx_);
        this->RebuildPeerTables(static_cast<uint64_t>(1));
    }
    rusty::raft_log_debug_1("heartbeat loop init from site: {}", this->site_id_);
    this->looping_.store(true, rusty::sync::atomic::Ordering::Release);
}

inline void RaftServerBase::MaybeCreateSnapshot() {
    const auto _apply_lock = RaftStdLockGuard::new_(&this->state_machine_apply_mtx_);
    const auto _lock = RaftLockGuard::new_(&this->mtx_);
    const bool configured = raft_snapshot_manager_is_set(&this->snapshot_manager_);
    if (!configured || rusty::detail::rust_not(raft_server_snapshot_is_due(this->state_.snapidx_, this->state_.execute_index_, this->state_.snapshot_threshold_))) {
        return;
    }
    // @unsafe
    {
        raft_create_snapshot_locked(static_cast<RaftServerBase*>(rusty::detail::ptr_or_addr((*this))));
    }
}

inline bool RaftServerBase::HasSnapshot() {
    const bool configured = [&]() -> bool { const auto _lock = RaftLockGuard::new_(&this->mtx_);
// @unsafe
{
    return raft_snapshot_manager_is_set(&this->snapshot_manager_);
} }();
    if (!configured) {
        return false;
    }
    // @unsafe
    {
        return raft_snapshot_manager_has_latest(&this->snapshot_manager_);
    }
}

inline void RaftServerBase::Disconnect(bool disconnect) {
    const auto _lock = RaftLockGuard::new_(&this->mtx_);
    // @unsafe
    {
        raft_verify(this->disconnected_.load(rusty::sync::atomic::Ordering::Acquire) != rusty::detail::deref_if_pointer_like(disconnect));
        raft_commo_set_network_enabled(static_cast<RaftServerBase*>(rusty::detail::ptr_or_addr((*this))), !disconnect);
    }
    this->disconnected_.store(std::move(disconnect), rusty::sync::atomic::Ordering::Release);
}

inline void RaftServerBase::Reconnect() {
    this->Disconnect(false);
    this->resetTimer(std::string_view("reconnect"));
}

inline void RaftServerBase::EnsureSetup() {
    if (this->heartbeat_setup_) {
        return;
    }
    this->heartbeat_setup_ = true;
    this->Setup();
}

inline void RaftServerBase::Setup() {
    const bool succeeded = raft_setup_internal_guarded(static_cast<RaftServerBase*>(rusty::detail::ptr_or_addr((*this))));
    if (!succeeded) {
        this->rpc_ready_.store(false, rusty::sync::atomic::Ordering::Release);
        this->stop_.store(true, rusty::sync::atomic::Ordering::Release);
        this->looping_.store(false, rusty::sync::atomic::Ordering::Release);
    }
    {
        const bool ready = this->IsRpcReady();
        const auto _lock = RaftStdLockGuard::new_(&this->startup_mtx_);
        this->startup_succeeded_ = rusty::detail::deref_if_pointer_like(succeeded) && rusty::detail::deref_if_pointer_like(ready);
        this->startup_finished_ = true;
    }
    // @unsafe
    {
        raft_startup_notify_all(static_cast<RaftServerBase*>(rusty::detail::ptr_or_addr((*this))));
    }
}

inline bool RaftServerBase::WaitForStartup() {
    // @unsafe
    {
        raft_startup_wait(static_cast<RaftServerBase*>(rusty::detail::ptr_or_addr((*this))));
    }
    return this->startup_succeeded_;
}

inline void RaftServerBase::StartElectionTimer() {
    this->ElectionLoopSetRunning(true);
    this->resetTimer(std::string_view("start election timer"));
    uint64_t wait_int = static_cast<uint64_t>(this->wait_int_);
    // @unsafe
    {
        raft_spawn_election_timer(static_cast<RaftServerBase*>(rusty::detail::ptr_or_addr((*this))), std::move(wait_int));
    }
}

inline void RaftServerBase::PrepareForShutdown() {
    {
        const auto _admission_lock = RaftLockGuard::new_(&this->mtx_);
        this->rpc_ready_.store(false, rusty::sync::atomic::Ordering::Release);
        this->stop_.store(true, rusty::sync::atomic::Ordering::Release);
        this->looping_.store(false, rusty::sync::atomic::Ordering::Release);
    }
    // @unsafe
    {
        raft_close_replication_wake_gate(static_cast<RaftServerBase*>(rusty::detail::ptr_or_addr((*this))));
    }
    while (this->heartbeat_loop_running_.load(rusty::sync::atomic::Ordering::Acquire) || this->election_loop_running_.load(rusty::sync::atomic::Ordering::Acquire)) {
        // @unsafe
        {
            raft_shutdown_barrier_yield();
        }
    }
    this->apply_thread_running_.store(false, rusty::sync::atomic::Ordering::SeqCst);
    // @unsafe
    {
        raft_apply_thread_join(static_cast<RaftServerBase*>(rusty::detail::ptr_or_addr((*this))));
    }
}

inline RaftStartResult RaftServerBase::StartImpl(const rusty::RaftCommand* cmd, uint64_t* index, uint64_t* term, uint64_t slot_id, int64_t ballot) {
    {
        const auto _lock = RaftLockGuard::new_(&this->mtx_);
        if (!this->IsLeaderLocked()) {
            // @unsafe
            {
                *index = static_cast<uint64_t>(0);
                *term = static_cast<uint64_t>(0);
            }
            return rusty::clone(RaftStartResult_REJECTED());
        }
        const RaftStartResult append_result = raft_set_local_append(static_cast<RaftServerBase*>(rusty::detail::ptr_or_addr((*this))), cmd, term, index, std::move(slot_id), std::move(ballot));
        // @unsafe
        {
            raft_verify(raft_server_start_was_appended(std::move(append_result)));
            raft_verify(this->state_.raft_log_.last_index() == (*index + 1));
            *index = this->state_.raft_log_.last_index();
            rusty::raft_log_debug_3("Start(): ldr={} index={} term={}", this->loc_id_, *index, *term);
        }
    }
    // @unsafe
    {
        raft_request_replication(static_cast<RaftServerBase*>(rusty::detail::ptr_or_addr((*this))));
    }
    return rusty::clone(rusty::clone(RaftStartResult_APPENDED()));
}

inline void RaftServerBase::EnqueueCommittedEntries(uint64_t old_commit, uint64_t new_commit) {
    uint64_t first_missing = static_cast<uint64_t>(0);
    uint64_t last_present = old_commit;
    uint64_t id = rusty::detail::deref_if_pointer_like(old_commit) + static_cast<uint64_t>(1);
    while (rusty::detail::deref_if_pointer_like(id) <= rusty::detail::deref_if_pointer_like(new_commit)) {
        auto found = this->state_.raft_log_.get(std::move(id));
        const bool usable = found.is_some() && raft_command_has_value(rusty::detail::ptr_cast<const rusty::RaftCommand*>(found.unwrap().cmd()));
        if (!usable) {
            first_missing = std::move(id);
            break;
        }
        last_present = std::move(id);
        id += 1;
    }
    uint64_t enqueued = static_cast<uint64_t>(0);
    if (rusty::detail::deref_if_pointer_like(last_present) > rusty::detail::deref_if_pointer_like(old_commit)) {
        enqueued = raft_apply_queue_push_range(static_cast<RaftServerBase*>(rusty::detail::ptr_or_addr((*this))), rusty::detail::deref_if_pointer_like(old_commit) + static_cast<uint64_t>(1), std::move(last_present));
    }
    if (rusty::detail::deref_if_pointer_like(first_missing) > 0) {
        rusty::raft_log_info_5("[ENQUEUE] Site {}: gap at slot {} (range {}..{}, enqueued {})", this->site_id_, std::move(first_missing), rusty::detail::deref_if_pointer_like(old_commit) + 1, std::move(new_commit), std::move(enqueued));
    }
    const uint64_t ticket = this->enqueue_log_counter_;
    this->enqueue_log_counter_ += 1;
    if ((rusty::detail::deref_if_pointer_like(ticket) % static_cast<int32_t>(50)) == static_cast<uint64_t>(0)) {
        const uint64_t qsize = raft_apply_queue_size(static_cast<RaftServerBase*>(rusty::detail::ptr_or_addr((*this))));
        rusty::raft_log_info_5("[ENQUEUE] Site {}: enqueued {} entries ({}..{}) queue_total={}", this->site_id_, std::move(enqueued), rusty::detail::deref_if_pointer_like(old_commit) + 1, std::move(new_commit), std::move(qsize));
    }
}

inline bool RaftServerBase::RequestVoteImpl(bool timer_guarded, uint64_t expected_generation) {
    if (this->stop_.load(rusty::sync::atomic::Ordering::Acquire)) {
        rusty::raft_log_debug_1("[RAFT-SHUTDOWN] RequestVote called during shutdown (site={}), ignoring to prevent crash", this->site_id_);
        return false;
    }
    uint32_t par_id = this->partition_id_;
    const uint32_t loc_id = this->loc_id_;
    uint64_t lst_idx = static_cast<uint64_t>(0);
    int64_t lst_term = static_cast<int64_t>(0);
    uint64_t prev_term = static_cast<uint64_t>(0);
    uint64_t term = static_cast<uint64_t>(0);
    uint16_t prev_vote_for = RAFT_SERVER_INVALID_SITE_ID;
    {
        const auto _lock = RaftLockGuard::new_(&this->mtx_);
        if (this->stop_.load(rusty::sync::atomic::Ordering::Acquire)) {
            this->state_.req_voting_ = false;
            return false;
        }
        if (rusty::detail::rust_not(raft_server_campaign_can_start(this->state_.is_leader_, this->state_.election_in_progress_))) {
            return false;
        }
        if (timer_guarded) {
            const uint64_t now = raft_time_now_us();
            const uint64_t elapsed = rusty::detail::deref_if_pointer_like(now) - rusty::detail::deref_if_pointer_like(this->state_.last_heartbeat_time_);
            if (rusty::detail::rust_not(raft_server_timer_campaign_is_current(this->state_.is_leader_, std::move(expected_generation), this->state_.election_timer_generation_, std::move(elapsed), this->state_.election_timeout_us_))) {
                return false;
            }
        }
        this->resetTimerLocked(std::string_view("starting election campaign"));
        prev_term = this->state_.current_term_;
        prev_vote_for = this->state_.vote_for_;
        uint64_t prev_local_term = this->state_.current_term_;
        rusty::detail::deref_if_pointer_like(this->state_.current_term_) += 1;
        this->state_.vote_for_ = this->site_id_;
        this->state_.current_leader_id_ = raft_server_leader_hint_after_transition(false, false, this->site_id_, this->state_.current_leader_id_);
        this->state_.election_in_progress_ = true;
        this->state_.election_term_ = static_cast<int64_t>(this->state_.current_term_);
        this->state_.req_voting_ = true;
        term = this->state_.current_term_;
        this->LogTermChange(std::string_view("starting election"), std::move(prev_local_term), this->state_.current_term_, RAFT_SERVER_INVALID_SITE_ID);
        lst_idx = this->state_.raft_log_.last_index();
        lst_term = this->ElectionLastLogTermLocked();
    }
    if (raft_election_debug_enabled()) {
        rusty::raft_log_info_7("[RAFT_ELECTION] server {} (loc {}) starting election term {}->{} lastLogIdx={} lastLogTerm={} prev_vote_for={}", this->site_id_, std::move(loc_id), std::move(prev_term), std::move(term), std::move(lst_idx), std::move(lst_term), std::move(prev_vote_for));
    }
    const rusty::RaftVoteQuorumPtr quorum = raft_broadcast_vote_and_wait(static_cast<RaftServerBase*>(rusty::detail::ptr_or_addr((*this))), std::move(par_id), std::move(lst_idx), std::move(lst_term), this->site_id_, static_cast<int64_t>(term));
    const auto _lock1 = RaftLockGuard::new_(&this->mtx_);
    if (this->stop_.load(rusty::sync::atomic::Ordering::Acquire)) {
        this->state_.election_in_progress_ = false;
        this->state_.req_voting_ = false;
        return false;
    }
    const RaftVoteOutcome outcome = raft_vote_quorum_snapshot(static_cast<const rusty::RaftVoteQuorumPtr*>(&quorum));
    const int64_t observed_response_term = outcome.term_;
    const int32_t completion_action = raft_server_election_completion_action(this->state_.election_in_progress_, static_cast<uint64_t>(this->state_.election_term_), std::move(term), this->state_.current_term_, std::move(observed_response_term));
    if (rusty::detail::deref_if_pointer_like(completion_action) == (static_cast<int32_t>(ElectionCompletionAction_ADVANCE_HIGHER_TERM()))) {
        uint64_t previous_term = this->state_.current_term_;
        this->state_.current_term_ = static_cast<uint64_t>(observed_response_term);
        this->state_.vote_for_ = RAFT_SERVER_INVALID_SITE_ID;
        this->state_.current_leader_id_ = raft_server_leader_hint_after_transition(false, false, this->site_id_, this->state_.current_leader_id_);
        if (this->state_.is_leader_) {
            this->stepDown();
        } else {
            this->setIsLeader(false);
        }
        this->state_.election_in_progress_ = false;
        this->state_.req_voting_ = false;
        this->LogTermChange(std::string_view("observed higher term from RequestVote replies"), std::move(previous_term), this->state_.current_term_, RAFT_SERVER_INVALID_SITE_ID);
        return false;
    }
    if (rusty::detail::deref_if_pointer_like(completion_action) == (static_cast<int32_t>(ElectionCompletionAction_IGNORE_STALE()))) {
        if (raft_election_debug_enabled()) {
            rusty::raft_log_info_5("[RAFT_ELECTION] server {} ignoring stale election result: result_term={} local_term={} election_term={} active={}", this->site_id_, std::move(term), this->state_.current_term_, this->state_.election_term_, this->state_.election_in_progress_);
        }
        return false;
    }
    // @unsafe
    {
        raft_verify(rusty::detail::deref_if_pointer_like(completion_action) == (static_cast<int32_t>(ElectionCompletionAction_APPLY_CURRENT())));
    }
    if (raft_election_debug_enabled()) {
        rusty::raft_log_info_6("[RAFT_ELECTION] server {} term {} vote outcome yes={} no={} highest_term_seen={} timeout={}", this->site_id_, std::move(term), std::move(outcome.n_voted_yes_), std::move(outcome.n_voted_no_), std::move(outcome.term_), std::move(outcome.timeouted_));
    }
    if (outcome.yes_) {
        // @unsafe
        {
            raft_verify(rusty::detail::deref_if_pointer_like(this->state_.current_term_) >= rusty::detail::deref_if_pointer_like(term));
        }
        this->state_.election_in_progress_ = false;
        this->state_.req_voting_ = false;
        if (this->stop_.load(rusty::sync::atomic::Ordering::Acquire) || (rusty::detail::deref_if_pointer_like(this->state_.current_term_) != rusty::detail::deref_if_pointer_like(term))) {
            this->state_.req_voting_ = false;
            return false;
        }
        this->setIsLeader(true);
        rusty::raft_log_debug_2("site {} became leader for term {}", this->site_id_, std::move(term));
        if (raft_election_debug_enabled()) {
            rusty::raft_log_info_4("[RAFT_ELECTION] server {} won election term {} (votes yes={} no={})", this->site_id_, std::move(term), std::move(outcome.n_voted_yes_), std::move(outcome.n_voted_no_));
        }
        if (this->IsLeaderLocked()) {
            rusty::raft_log_debug_2("vote accepted {} curterm {}", std::move(loc_id), this->state_.current_term_);
            this->state_.req_voting_ = false;
            return true;
        } else {
            rusty::raft_log_debug_2("vote rejected {} curterm {}, do rollback", std::move(loc_id), this->state_.current_term_);
            this->setIsLeader(false);
            return false;
        }
    } else if (outcome.no_) {
        rusty::raft_log_debug_1("site {} requestvote rejected", this->site_id_);
        this->setIsLeader(false);
        if (raft_election_debug_enabled()) {
            rusty::raft_log_info_5("[RAFT_ELECTION] server {} lost election term {} (yes={} no={}) highest_term={}", this->site_id_, std::move(term), std::move(outcome.n_voted_yes_), std::move(outcome.n_voted_no_), std::move(outcome.term_));
        }
        if (rusty::detail::deref_if_pointer_like(this->state_.election_in_progress_) && (rusty::detail::deref_if_pointer_like(this->state_.election_term_) == (static_cast<int64_t>(term)))) {
            this->state_.election_in_progress_ = false;
        }
        this->state_.req_voting_ = false;
        return false;
    } else {
        rusty::raft_log_debug_1("vote timeout {}", std::move(loc_id));
        if (raft_election_debug_enabled()) {
            rusty::raft_log_info_4("[RAFT_ELECTION] server {} election timed out term {} (yes={} no={})", this->site_id_, std::move(term), std::move(outcome.n_voted_yes_), std::move(outcome.n_voted_no_));
        }
        if (rusty::detail::deref_if_pointer_like(this->state_.election_in_progress_) && (rusty::detail::deref_if_pointer_like(this->state_.election_term_) == (static_cast<int64_t>(term)))) {
            this->state_.election_in_progress_ = false;
        }
        this->state_.req_voting_ = false;
        return false;
    }
}

inline void RaftServerBase::stepDown() {
    rusty::raft_log_info_2("[SPEC-RAFT] Site {}: Stepping down as leader (term={})", this->site_id_, this->state_.current_term_);
    this->setIsLeader(false);
    this->state_.req_voting_ = false;
    this->state_.election_in_progress_ = false;
    this->resetTimerLocked(std::string_view("stepDown"));
    rusty::raft_log_info_1("[SPEC-RAFT] Site {}: Step-down complete, now follower", this->site_id_);
}

inline void RaftServerBase::set_site_identity(uint32_t loc_id, uint16_t site_id, uint32_t partition_id) {
    this->loc_id_ = std::move(loc_id);
    this->site_id_ = std::move(site_id);
    this->partition_id_ = std::move(partition_id);
    this->state_.loc_id_ = std::move(loc_id);
    this->state_.site_id_ = std::move(site_id);
    this->state_.partition_id_ = std::move(partition_id);
    // @unsafe
    {
        raft_verify(((rusty::detail::deref_if_pointer_like(this->state_.site_id_) == rusty::detail::deref_if_pointer_like(this->site_id_)) && (rusty::detail::deref_if_pointer_like(this->state_.partition_id_) == rusty::detail::deref_if_pointer_like(this->partition_id_))) && (rusty::detail::deref_if_pointer_like(this->state_.loc_id_) == rusty::detail::deref_if_pointer_like(this->loc_id_)));
    }
}

inline void RaftServerBase::set_commo(rusty::Communicator* commo) {
    this->commo_ = std::move(commo);
}

inline void RaftServerBase::reg_learner_action(rusty::LearnerAction learner_action) {
    this->app_next_ = std::move(learner_action);
}
/*RUSTYCPP:GEN-END id=raft_server.server_state*/
class RaftServer : public RaftServerBase {
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

  // @unsafe - suspends this fiber on the wake gate's election waiter
  bool ElectionLoopWait(uint64_t timeout_us);
  // @unsafe - dispatches through the vtable; caller re-checks stop_ first
  void ElectionLoopRequestVote(uint64_t generation);

  // ==========================================================================
  // HEARTBEAT LOOP KERNELS
  //
  // The C++ half of the DSL-owned HeartbeatDriver declared above. The round
  // body is still one kernel; splitting it into the four phases is the next
  // tranche.
  // ==========================================================================

  // @safe - linear scan of a fixed, tiny table (replica counts are 3 or 5).
  // Returns state_.peers_.len() when the site is not a follower of this leader,
  // which is the "removed follower" case PHASE 2 guards against. Deliberately
  // returns an ordinal rather than a reference: an ordinal cannot dangle
  // across an RPC send or a re-entrant completion callback.
  size_t PeerOrdinal(siteid_t site) const {
    for (size_t ord = 0; ord < peer_sites_.size(); ord++) {
      if (peer_sites_[ord] == site) {
        return ord;
      }
    }
    return state_.peers_.len();
  }


  // @unsafe - suspends on the wake gate; false means shutdown
  bool HeartbeatWait();
  // @unsafe - advances the read-index round and recomputes the commit index;
  // false means leadership is not held, so phases 1-3 are skipped
  bool HeartbeatPhase0(struct HeartbeatRoundState& state,
                       struct HeartbeatRoundScope& round);
  // @unsafe - builds and sends AppendEntries / InstallSnapshot per follower
  void HeartbeatPhase1(struct HeartbeatRoundState& state,
                       struct HeartbeatRoundScope& round);
  // @unsafe - recomputes the commit index and publishes read-index authority
  void HeartbeatPhase3(struct HeartbeatRoundState& state,
                       struct HeartbeatRoundScope& round);


  // set_site_identity / set_commo / reg_learner_action are RaftServerBase's
  // now -- the DSL struct implements TxLogServer directly.

 private:



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
 public:  // for the kernel bridge (server.cc); private again once converted
  bool CreateSnapshotLocked();
 private:


  // ============================================================================

  // Heartbeat quorum proof, guarded by mtx_. HeartbeatLoop stamps every round
  // with state_.heartbeat_round_ and records the newest round that a quorum of the
  // membership configuration confirmed in the current term.
  // Election timing is one mutex-protected campaign. A reset samples exactly
  // one timeout and advances the generation; the timer must never redraw the
  // random timeout on each poll or start a campaign from an expired snapshot
  // after a concurrent heartbeat reset.

  // Cross-thread submissions publish only to this level-triggered gate.  The
  // gate posts a gate-only job to the heartbeat PollThread; IntEvent itself is
  // created, signalled, waited, and cleared exclusively by that owner thread.
 public:  // for the kernel bridge (server.cc); private again once the gate
          // itself can live in RaftServerBase
  rusty::Arc<ReplicationWakeGate> replication_wake_gate_;
 private:
  // @unsafe - Owner-thread-only wait on the gate's IntEvent.
  bool WaitForReplicationOrHeartbeat(uint64_t timeout_us);
  // @unsafe - Owner-thread-only election delay that shutdown can interrupt.
  bool WaitForElectionTimeoutOrShutdown(uint64_t timeout_us);
  // @unsafe - Stops new wake jobs and releases the gate's PollThread handle.
 public:  // for the kernel bridge (server.cc); private again once converted
  void CloseReplicationWakeGate();
 private:


  // ============================================================================
  // PREFERRED REPLICA SYSTEM - Election timeout bias
  // ============================================================================
  // One replica may be designated as the "preferred leader". Voting itself
  // carries no bias: any replica can win any election. The preference only
  // shapes GetElectionTimeout(), so the preferred replica campaigns sooner
  // than its peers and normally wins the startup election.


  // The campaign that owns state_.req_voting_; a delayed vote result applies only to
  // this exact term.


  // Reads the dynamically configurable preferred-leader identity.
  //
  // Must be called with mtx_ held. The one caller repo-wide is
  // GetElectionTimeout(), which is itself called only from resetTimer(), which
  // takes mtx_ -- so the inner re-acquisition this used to take was a no-op on
  // the recursive mutex, and removing it states the precondition as a type-
  // adjacent comment rather than re-checking it at runtime. See

  // ============================================================================

  // Timer-only entry retains the reset generation observed at expiry. The
  // first RequestVote state lock revalidates it immediately before term++.
  bool RequestVoteFromElectionTimer(uint64_t expected_generation);

 public:  // for the kernel bridge (server.cc); private again once converted
  bool SetupInternal();
 private:
  // @safe - external calls marked @external, core replication loop
	void HeartbeatLoop() ;

  // @unsafe - raw pointer output parameters (reply_term, vote_granted)
  // Memory-only voting: record the vote and reply immediately.
  // PUBLIC for the raft_do_vote trampoline, which raft_on_request_vote calls
  // back through. An extern "C" function is not a member and cannot reach a
  // private one; the existing heartbeat trampolines work only because
  // HeartbeatPhase0..3 are public. Both go back to private when their own
  // bodies convert and the trampolines are deleted.
 public:
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

 private:





  void StartApplyThread();
 public:  // for the raft_ae_* trampolines; back to private when converted
 private:

  // @unsafe - const char* parameter type requires unsafe context
  // Acquiring entry point. See resetTimerLocked for the body.
  // CALLER MUST HOLD mtx_.
 public:  // for the raft_ae_* trampolines; back to private when converted
 private:


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



  // @unsafe - Binds the cross-thread wake gate to HeartbeatLoop's PollThread.
  // Must run before HeartbeatLoop starts (Setup does so).
  void BindReplicationWakeOwner(rusty::Arc<rrr::PollThread> owner);


  // @safe - Acquire-load paired with the final startup Release publication.


  // Acquire-load pairs with PublishAppliedIndex after app_next_ completes.
  // @safe - Rusty atomic read.


  // @unsafe - Locks mtx_ before reading the role published by setIsLeader().
  // CALLER MUST HOLD mtx_. The looping_ check is an atomic, so it needs no
  // lock and stays here: it is the guard against reading members during
  // destruction.

  

  // @safe - external calls marked @external, output pointer writes in @unsafe blocks
  // take janus::Command;
  // shared_ptr<Marshallable> callers auto-convert via Command's
  // implicit ctor.
  RaftStartResult Start(const janus::Command& cmd,
                        uint64_t* index,
                        uint64_t* term,
                        slotid_t slot_id = -1,
                        ballot_t ballot = 1);

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
    *index = state_.raft_log_.last_index();
    // slot_id and ballot are accepted for signature compatibility with the
    // Paxos-shaped callers; RaftEntry has no field for either, because the
    // three fields that used to receive them here were read nowhere.
    (void)slot_id;
    (void)ballot;
    const uint64_t appended = state_.raft_log_.append(
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
    const auto found = state_.raft_log_.get(id);
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
  /**
   * Get the current snapshot manager.
   * @return Shared pointer to SnapshotManager, or nullptr if not set
   */
  // @unsafe - Locks mtx_ and returns a copy of the shared_ptr.
  std::shared_ptr<janus::raft::SnapshotManager> GetSnapshotManager();






  /**
   * Get the current snapshot threshold.
   * @return Current threshold value
   */
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



};
} // namespace janus
