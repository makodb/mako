// The pure decision helpers (raft_server_*) and their compile-time checks.
//
// [move, M1] Moved verbatim from src/deptran/raft/src/server_h.rs (Phase 6),
// paths aside: the rust lane's rusty::Vec/Option are std's own.

#[allow(unused_imports)]
use crate::*;

#[allow(non_camel_case_types)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Eq, PartialEq))]
#[repr(i32)]
pub enum ElectionCompletionAction {
    IGNORE_STALE = 0,
    APPLY_CURRENT = 1,
    ADVANCE_HIGHER_TERM = 2,
}

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

pub const fn raft_server_append_is_acceptable(term_ok: bool,
                                               index_ok: bool,
                                               previous_term_ok: bool) -> bool {
    term_ok && index_ok && previous_term_ok
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
// The predicates' tests, one `const` assert each, in the order the C++
// static_asserts had them. The emitter lowers each to a static_assert, so
// the transpiled build checks them exactly as before; rustc checks them at
// every gate. Numeric literals take the predicate's parameter types.
const _: () = assert!(raft_server_site_is_preferred_leader(7, 7));
// A site that is itself the sentinel is not the preferred leader, because
// the sentinel means "no preferred leader configured".
const _: () = assert!(!raft_server_site_is_preferred_leader(RAFT_SERVER_INVALID_SITE_ID, RAFT_SERVER_INVALID_SITE_ID));
const _: () = assert!(raft_server_vote_is_idempotent(4, 4, 2, 2));
const _: () = assert!(raft_server_candidate_log_is_at_least(3, 2, 1, 9));
const _: () = assert!(raft_server_candidate_log_is_at_least(3, 3, 9, 9));
const _: () = assert!(!raft_server_candidate_log_is_at_least(3, 3, 8, 9));
const _: () = assert!(raft_server_election_last_log_uses_snapshot(0, 0));
const _: () = assert!(raft_server_election_last_log_uses_snapshot(460, 460));
const _: () = assert!(!raft_server_election_last_log_uses_snapshot(461, 460));
const _: () = assert!(raft_server_timer_campaign_is_current(false, 9, 9, 501, 500));
const _: () = assert!(!raft_server_timer_campaign_is_current(false, 8, 9, 501, 500));
const _: () = assert!(!raft_server_timer_campaign_is_current(false, 9, 9, 500, 500));
const _: () = assert!(!raft_server_timer_campaign_is_current(true, 9, 9, 501, 500));
const _: () = assert!(raft_server_campaign_can_start(false, false));
const _: () = assert!(!raft_server_campaign_can_start(true, false));
const _: () = assert!(!raft_server_campaign_can_start(false, true));
const _: () = assert!(raft_server_snapshot_is_stale(9, 9));
const _: () = assert!(raft_server_snapshot_is_stale(8, 9));
const _: () = assert!(!raft_server_snapshot_is_stale(10, 9));
const _: () = assert!(raft_server_snapshot_boundary_matches(true, 7, 7));
const _: () = assert!(!raft_server_snapshot_boundary_matches(false, 7, 7));
const _: () = assert!(!raft_server_snapshot_boundary_matches(true, 6, 7));
const _: () = assert!(raft_server_snapshot_term_is_valid(7, 7));
const _: () = assert!(raft_server_snapshot_term_is_valid(6, 7));
const _: () = assert!(!raft_server_snapshot_term_is_valid(8, 7));
const _: () = assert!(raft_server_snapshot_recovery_retains_suffix(true, true, true, false));
const _: () = assert!(!raft_server_snapshot_recovery_retains_suffix(true, true, false, true));
const _: () = assert!(raft_server_snapshot_recovery_retains_suffix(true, false, false, true));
const _: () = assert!(!raft_server_snapshot_recovery_retains_suffix(true, false, false, false));
const _: () = assert!(!raft_server_snapshot_recovery_retains_suffix(false, false, false, true));
const _: () = assert!(raft_server_snapshot_recovery_has_unproven_gap(true, false, false));
const _: () = assert!(!raft_server_snapshot_recovery_has_unproven_gap(true, true, false));
const _: () = assert!(!raft_server_snapshot_recovery_has_unproven_gap(true, false, true));
const _: () = assert!(raft_server_snapshot_term_uses_boundary(11, 11));
const _: () = assert!(!raft_server_snapshot_term_uses_boundary(12, 11));
const _: () = assert!(raft_server_election_result_is_current(true, 4, 4, 4));
const _: () = assert!(!raft_server_election_result_is_current(false, 4, 4, 4));
const _: () = assert!(!raft_server_election_result_is_current(true, 3, 4, 4));
const _: () = assert!(!raft_server_election_result_is_current(true, 4, 4, 5));
const _: () = assert!(raft_server_election_completion_action(true, 4, 4, 4, 4) == ElectionCompletionAction::APPLY_CURRENT as i32);
const _: () = assert!(raft_server_election_completion_action(true, 5, 4, 5, 5) == ElectionCompletionAction::IGNORE_STALE as i32);
const _: () = assert!(raft_server_election_completion_action(true, 5, 4, 5, 6) == ElectionCompletionAction::ADVANCE_HIGHER_TERM as i32);
const _: () = assert!(raft_server_apply_epoch_is_current(8, 8));
const _: () = assert!(!raft_server_apply_epoch_is_current(7, 8));
const _: () = assert!(raft_server_log_index_has_successor(0));
const _: () = assert!(raft_server_log_index_has_successor(u64::MAX - 1));
const _: () = assert!(!raft_server_log_index_has_successor(u64::MAX));
const _: () = assert!(raft_server_append_is_acceptable(true, true, true));
const _: () = assert!(!raft_server_append_is_acceptable(false, true, true));
const _: () = assert!(!raft_server_append_is_acceptable(true, false, true));
const _: () = assert!(!raft_server_append_is_acceptable(true, true, false));
const _: () = assert!(raft_server_append_entry_count_fits(u64::MAX, 0));
const _: () = assert!(!raft_server_append_entry_count_fits(u64::MAX, 1));
const _: () = assert!(raft_server_append_entry_count_fits(u64::MAX - 3, 3));
const _: () = assert!(!raft_server_append_entry_count_fits(u64::MAX - 3, 4));
const _: () = assert!(!raft_server_append_batch_count_is_valid(7, 0));
const _: () = assert!(raft_server_append_batch_count_is_valid(u64::MAX - 3, 3));
const _: () = assert!(!raft_server_append_batch_count_is_valid(u64::MAX - 3, 4));
const _: () = assert!(raft_server_append_entry_conflicts(false, 0, 7));
const _: () = assert!(raft_server_append_entry_conflicts(true, 6, 7));
const _: () = assert!(!raft_server_append_entry_conflicts(true, 7, 7));
const _: () = assert!(raft_server_append_result_last_index(10, 8, false) == 10);
const _: () = assert!(raft_server_append_result_last_index(10, 8, true) == 8);
const _: () = assert!(raft_server_append_result_last_index(8, 10, false) == 10);
const _: () = assert!(raft_server_append_sent_end(7, 0) == 7);
const _: () = assert!(raft_server_append_sent_end(7, 1) == 8);
const _: () = assert!(raft_server_append_sent_end(7, 4) == 11);
const _: () = assert!(raft_server_append_acknowledged_through(20, 10, 15) == 10);
const _: () = assert!(raft_server_append_acknowledged_through(8, 10, 15) == 8);
const _: () = assert!(raft_server_append_acknowledged_through(20, 15, 9) == 9);
const _: () = assert!(raft_server_commit_index_clamp(9, 7) == 7);
// The clamp policy is now scalar, so it tests as plain static_asserts like its
// 67 siblings. The majority SELECTION moved onto PeerTable, which owns a
// Vec and so cannot be constant-evaluated; raftLabTest covers it.
const _: () = assert!(raft_server_commit_index_candidate(7, 3, 100) == 7);
const _: () = assert!(raft_server_commit_index_candidate(9, 5, 100) == 9);
const _: () = assert!(raft_server_commit_index_candidate(7, 3, 5) == 5);
const _: () = assert!(raft_server_commit_index_candidate(0, 1, 42) == 42);
const _: () = assert!(raft_server_compaction_safe_index(12, 10, 8) == 8);
const _: () = assert!(raft_server_compaction_safe_index(7, 10, 8) == 7);
const _: () = assert!(raft_server_compaction_safe_index(9, 8, 10) == 8);
const _: () = assert!(raft_server_read_index_round_can_advance(0));
const _: () = assert!(!raft_server_read_index_round_can_advance(u64::MAX));
const _: () = assert!(raft_server_read_index_reply_confirms_authority(true, true, 7, 7, 7, 11, 11));
const _: () = assert!(!raft_server_read_index_reply_confirms_authority(true, true, 7, 7, 7, 10, 11));
const _: () = assert!(!raft_server_read_index_reply_confirms_authority(true, true, 7, 8, 7, 11, 11));
const _: () = assert!(raft_server_snapshot_progress_clamp(3, 5, 9) == 5);
const _: () = assert!(raft_server_snapshot_progress_clamp(7, 5, 9) == 7);
const _: () = assert!(raft_server_snapshot_progress_clamp(12, 5, 9) == 9);
const _: () = assert!(raft_server_snapshot_is_due(4, 10, 5));
const _: () = assert!(!raft_server_snapshot_is_due(10, 4, 5));
const _: () = assert!(raft_server_follower_next_index(7) == 8);
const _: () = assert!(raft_server_follower_next_index(u64::MAX) == 0);
const _: () = assert!(raft_server_follower_next_index(4) == 5);
const _: () = assert!(raft_server_follower_next_index(u64::MAX) == 0);
const _: () = assert!(raft_server_retention_window_normalize(0) == 1);
const _: () = assert!(raft_server_retention_window_normalize(1) == 1);
const _: () = assert!(raft_server_retention_window_normalize(u64::MAX) == u64::MAX);
// Raft currently compares signed ballot_t values with uint64_t core.current_term_.
// These casts make the existing C++ usual-arithmetic-conversion semantics
// explicit, including the historical negative-term edge case.
const _: () = assert!(!raft_server_vote_term_is_stale(u64::MAX, 0));
const _: () = assert!(raft_server_observed_higher_term(u64::MAX, 0));
const _: () = assert!(!raft_server_signed_term_is_newer(-1, 0));
const _: () = assert!(!raft_server_signed_term_is_newer(0, 0));
const _: () = assert!(raft_server_signed_term_is_newer(1, 0));
const _: () = assert!(raft_server_log_entry_is_current_term(-1, u64::MAX));

// [move, M1] The two quorum helpers the authority ledger uses, copied from
// src/deptran/raft/src/quorum_hpp.rs (whose inline-DSL block in quorum.hpp
// stays the source of the C++ copies). The core depends on no shell crate.
pub const fn raft_quorum_majority_count(total: usize) -> usize {
    (total / 2) + 1
}

pub const fn raft_quorum_count_reached(count: usize, quorum: usize) -> bool {
    count >= quorum
}
