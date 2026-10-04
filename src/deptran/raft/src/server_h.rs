// The Raft server. rustc compiles this into libraft.a; nothing here is
// transpiled, so edit it directly.

use crate::server_pods_h::{RaftElectionTimeouts, RaftServerHandle};
// [move, M1] for the authority ledger, which moved here from server_cc.rs
use crate::quorum_hpp::raft_quorum_count_reached;
use crate::quorum_hpp::raft_quorum_majority_count;
// A delayed vote quorum result is interpreted before its YES/NO/TIMEOUT
// payload. Higher-term evidence is globally authoritative; every ordinary
// outcome belongs only to the exact campaign that is still active.

// ---------------------------------------------------------------------------
// The lab cluster registry.
//
// The RaftLab suite embeds all five replicas in ONE process, and the harness
// needs to reach each of them. The server publishes itself: set_site_identity
// is called exactly once per replica by the worker (server_worker.cc:43,
// raft_worker.cc:297) and is precisely where the server learns which replica
// it is, so no C++ has to hand the cluster over.
//
// So this costs ZERO new exports. The plan budgeted one -- C++ calling in to
// register each server -- but the registration point was already Rust.
//
// Entries hold the server as a usize rather than a pointer so the table is
// Send without a wrapper. That is sound here and nowhere else: the lab is one
// process, the worker owns every server and tears them down only after the
// suite has finished, and the harness runs on a fiber in that same process.
// Nothing outside a `raft_test` build can reach these items.
// Flat items rather than a `lab_registry` module: a nested module cannot be
// imported by the transpiled C++ lane (a C++ `using` cannot name a namespace).
#[cfg(feature = "raft_test")]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct LabEntry {
    pub loc_id: u32,
    server: usize,
}

#[cfg(feature = "raft_test")]
impl LabEntry {
    /// # Safety
    /// Valid only while the worker still owns the server -- i.e. for the
    /// life of the suite. See the note above.
    pub unsafe fn server(&self) -> *mut RaftServerBase {
        self.server as *mut RaftServerBase
    }
}

#[cfg(feature = "raft_test")]
static LAB_REGISTRY: std::sync::Mutex<Vec<LabEntry>> = std::sync::Mutex::new(Vec::new());

/// Publish one replica. Re-registering the same locale replaces the entry
/// rather than duplicating it: set_site_identity is idempotent and a
/// restarted replica must not appear twice.
#[cfg(feature = "raft_test")]
pub fn lab_register(loc_id: u32, server: *mut RaftServerBase) {
    let mut guard = LAB_REGISTRY.lock().unwrap();
    lab_upsert(&mut guard, LabEntry { loc_id, server: server as usize });
}

// On a plain `&mut Vec` rather than through the MutexGuard: the transpiler
// mis-lowers `guard.push(entry)` on a guard as a push of `Vec::from_iter(entry)`.
#[cfg(feature = "raft_test")]
fn lab_upsert(entries: &mut Vec<LabEntry>, entry: LabEntry) {
    match entries.iter_mut().find(|e| e.loc_id == entry.loc_id) {
        Some(slot) => *slot = entry,
        None => entries.push(entry),
    }
    entries.sort_by_key(|e| e.loc_id);
}

/// How many replicas have published themselves. The lab expects five.
#[cfg(feature = "raft_test")]
pub fn lab_count() -> usize {
    LAB_REGISTRY.lock().unwrap().len()
}

/// Every replica, ordered by locale id.
#[cfg(feature = "raft_test")]
pub fn lab_entries() -> Vec<LabEntry> {
    LAB_REGISTRY.lock().unwrap().clone()
}

#[cfg(feature = "raft_test")]
pub fn lab_get(loc_id: u32) -> Option<LabEntry> {
    LAB_REGISTRY.lock().unwrap().iter().copied().find(|e| e.loc_id == loc_id)
}



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
// rusty::Vec and so cannot be constant-evaluated; raftLabTest covers it.
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

#[repr(C)]
pub struct RaftEntry {
    term_: i64,
    cmd_: rusty::RaftCommand,
    // [move, M6] What the per-entry kernels report about cmd_, asked once
    // when the entry is made (raft_entry_from_command) instead of at every
    // use. The command is never modified once logged: a send stamps a copy
    // (server.cc raft_stamped_commit_into). The last three are read only
    // when has_value_ holds, as the kernels were only called then.
    has_value_: bool,
    is_tpc_commit_: bool,
    kind_: i32,
    payload_bytes_: u64,
}

impl RaftEntry {
    pub fn new(term: i64, cmd: rusty::RaftCommand, has_value: bool,
               is_tpc_commit: bool, kind: i32, payload_bytes: u64) -> RaftEntry {
        RaftEntry {
            term_: term,
            cmd_: cmd,
            has_value_: has_value,  // [move, M6]
            is_tpc_commit_: is_tpc_commit,  // [move, M6]
            kind_: kind,  // [move, M6]
            payload_bytes_: payload_bytes,  // [move, M6]
        }
    }

    pub fn term(&self) -> i64 {
        self.term_
    }

    // Handed back to C++, never followed from Rust.
    pub fn cmd(&self) -> &rusty::RaftCommand {
        &self.cmd_
    }

    // [move, M6] raft_command_has_value(cmd)
    pub fn has_value(&self) -> bool {
        self.has_value_
    }

    // [move, M6] raft_command_is_tpc_commit(cmd)
    pub fn is_tpc_commit(&self) -> bool {
        self.is_tpc_commit_
    }

    // [move, M6] raft_command_kind(cmd)
    pub fn kind(&self) -> i32 {
        self.kind_
    }

    // [move, M6] raft_command_payload_bytes(cmd)
    pub fn payload_bytes(&self) -> u64 {
        self.payload_bytes_
    }
}

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
// (src/srpc/rusty-rustc/src/lib.rs:907) -- so a DSL type owning one would be
// verified against semantics production does not have.
#[repr(C)]
pub struct PeerTable {
    progress_: rusty::Vec<FollowerProgress>,
}

#[allow(clippy::new_without_default)]
impl PeerTable {
    pub fn new() -> PeerTable {
        PeerTable { progress_: rusty::Vec::new() }
    }

    // One slot per follower, in ordinal order.
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

// ==========================================================================
// THE HEARTBEAT ROUND'S STATE ([move, M1], from server_cc.rs, where the
// heartbeat driver owned it as HeartbeatRoundState). RaftCore holds it now,
// beside the consensus state it is decided with: at most one AppendEntries
// in flight per follower (PendingTable), the read-index authority evidence
// (AuthorityLedger) and the membership and term one round latches
// (HeartbeatRoundScope). Bodies unchanged.
// ==========================================================================

pub struct PendingAppend {
    follower_: u16,
    sent_term_: u64,
    sent_round_: u64,
    // Inclusive end of the exact prefix proved by this RPC's wire payload. A
    // heartbeat proves only prevLogIndex; raw and batched payloads extend it
    // by their encoded entry count.
    sent_end_index_: u64,
    // [move, M5] Whether the RPC carried entries, which is all the reply's
    // log line asked of its command. The command and the response handle
    // stay with the shell (RaftServerBase::append_responses_), which sends
    // the RPC after the core has decided it.
    has_entries_: bool,
}

impl PendingAppend {
    pub fn new(follower: u16, sent_term: u64, sent_round: u64,
               sent_end_index: u64, has_entries: bool) -> PendingAppend {
        PendingAppend {
            follower_: follower,
            sent_term_: sent_term,
            sent_round_: sent_round,
            sent_end_index_: sent_end_index,
            has_entries_: has_entries,  // [move, M5]
        }
    }
}

pub struct PendingTable {
    slots_: rusty::Vec<rusty::Option<PendingAppend>>,
}

#[allow(clippy::new_without_default)]
impl PendingTable {
    pub fn new() -> PendingTable {
        PendingTable { slots_: rusty::Vec::new() }
    }

    // One slot per follower, all empty. Called wherever the peer table is
    // sized, so the two always agree on what an ordinal means.
    pub fn resize(&mut self, peers: usize) {
        self.slots_.clear();
        let mut i: usize = 0;
        while i < peers {
            self.slots_.push(rusty::None);
            i += 1;
        }
    }

    // Drops every in-flight context. Used on leadership loss and on a term
    // change, so a prior epoch's RPC can never occupy a slot.
    pub fn abandon(&mut self) {
        let peers = self.slots_.len();
        self.resize(peers);
    }

    pub fn len(&self) -> usize {
        self.slots_.len()
    }

    pub fn is_empty(&self) -> bool {
        self.slots_.is_empty()
    }

    pub fn occupied(&self, ordinal: usize) -> bool {
        self.slots_[ordinal].is_some()
    }

    pub fn place(&mut self, ordinal: usize, pending: PendingAppend) {
        self.slots_[ordinal] = rusty::Some(pending);
    }

    pub fn release(&mut self, ordinal: usize) {
        self.slots_[ordinal] = rusty::None;
    }

    pub fn follower(&self, ordinal: usize) -> u16 {
        if self.slots_[ordinal].is_none() {
            return 0;
        }
        self.slots_[ordinal].as_ref().unwrap().follower_
    }

    pub fn sent_term(&self, ordinal: usize) -> u64 {
        if self.slots_[ordinal].is_none() {
            return 0;
        }
        self.slots_[ordinal].as_ref().unwrap().sent_term_
    }

    pub fn sent_round(&self, ordinal: usize) -> u64 {
        if self.slots_[ordinal].is_none() {
            return 0;
        }
        self.slots_[ordinal].as_ref().unwrap().sent_round_
    }

    pub fn sent_end_index(&self, ordinal: usize) -> u64 {
        if self.slots_[ordinal].is_none() {
            return 0;
        }
        self.slots_[ordinal].as_ref().unwrap().sent_end_index_
    }

    // [move, M5] Whether the in-flight RPC carried entries.
    pub fn has_entries(&self, ordinal: usize) -> bool {
        if self.slots_[ordinal].is_none() {
            return false;
        }
        self.slots_[ordinal].as_ref().unwrap().has_entries_
    }
}

// [move, M9] The site sets of the authority ledger and the round scope: a
// sorted, duplicate-free Vec<u16> in place of rusty::BTreeSet<u16>, with the
// same membership and the same order. Only insert, remove, contains, len,
// is_empty and clear were ever used; nothing iterates. A set holds at most one
// entry per replica (3 or 5), so a linear scan does as well as a tree, and a
// Vec is a type the verifier specifies. Spelled with push, pop and indexing
// only, which both lanes' Vec support.
pub struct SiteSet {
    sites_: rusty::Vec<u16>,
}

#[allow(clippy::new_without_default)]
impl SiteSet {
    pub fn new() -> SiteSet {
        SiteSet { sites_: rusty::Vec::new() }
    }

    pub fn len(&self) -> usize {
        self.sites_.len()
    }

    pub fn is_empty(&self) -> bool {
        self.sites_.is_empty()
    }

    pub fn clear(&mut self) {
        self.sites_.clear();
    }

    pub fn contains(&self, site: &u16) -> bool {
        let mut i: usize = 0;
        while i < self.sites_.len() {
            if self.sites_[i] == *site {
                return true;
            }
            i += 1;
        }
        false
    }

    // BTreeSet::insert: false, and no change, if already present.
    // manual_swap: Vec::swap is not among the operations both lanes' Vec
    // support (push, pop and indexing; see above), so the swap is spelled out.
    #[allow(clippy::manual_swap)]
    pub fn insert(&mut self, site: u16) -> bool {
        if self.contains(&site) {
            return false;
        }
        self.sites_.push(site);
        let mut j: usize = self.sites_.len() - 1;
        while j > 0 && self.sites_[j - 1] > self.sites_[j] {
            let lower: u16 = self.sites_[j - 1];
            self.sites_[j - 1] = self.sites_[j];
            self.sites_[j] = lower;
            j -= 1;
        }
        true
    }

    // BTreeSet::remove: false if absent.
    pub fn remove(&mut self, site: &u16) -> bool {
        let mut i: usize = 0;
        while i < self.sites_.len() && self.sites_[i] != *site {
            i += 1;
        }
        if i == self.sites_.len() {
            return false;
        }
        while i + 1 < self.sites_.len() {
            self.sites_[i] = self.sites_[i + 1];
            i += 1;
        }
        self.sites_.pop();
        true
    }
}

pub struct HeartbeatAuthority {
    term_: u64,
    config_size_: usize,
    voters_: SiteSet,  // [move, M9]
    outstanding_: SiteSet,  // [move, M9]
}

#[allow(clippy::new_without_default)]
impl HeartbeatAuthority {
    // A generation begins with this site already counted as a voter: a leader
    // is evidence for its own authority.
    pub fn new(term: u64, config_size: usize, self_site: u16) -> HeartbeatAuthority {
        let mut voters: SiteSet = SiteSet::new();  // [move, M9]
        voters.insert(self_site);
        HeartbeatAuthority {
            term_: term,
            config_size_: config_size,
            voters_: voters,
            outstanding_: SiteSet::new(),  // [move, M9]
        }
    }

    pub fn term(&self) -> u64 {
        self.term_
    }

    pub fn config_size(&self) -> usize {
        self.config_size_
    }

    pub fn voter_count(&self) -> usize {
        self.voters_.len()
    }

    // One physical RPC exists per follower per generation, but these stay sets
    // so a future transport cannot double-count a voter.
    pub fn launch(&mut self, site: u16) {
        self.outstanding_.insert(site);
    }

    pub fn retire(&mut self, site: u16) {
        self.outstanding_.remove(&site);
    }

    pub fn record_vote(&mut self, site: u16) {
        self.voters_.insert(site);
    }

    // Every RPC launched in this generation has completed. No later event can
    // add evidence to it.
    pub fn all_completed(&self) -> bool {
        self.outstanding_.is_empty()
    }
}

pub struct AuthorityGeneration {
    round_id_: u64,
    config_: SiteSet,  // [move, M9]
    evidence_: HeartbeatAuthority,
}

impl AuthorityGeneration {
    pub fn round_id(&self) -> u64 {
        self.round_id_
    }

    pub fn term(&self) -> u64 {
        self.evidence_.term()
    }

    pub fn voter_count(&self) -> usize {
        self.evidence_.voter_count()
    }

    pub fn config_size(&self) -> usize {
        self.evidence_.config_size()
    }

    // Quorum is asked of the generation's OWN config size, not the current
    // one: a delayed reply is evidence against the membership that launched
    // it.
    pub fn has_quorum(&self) -> bool {
        let quorum = raft_quorum_majority_count(self.evidence_.config_size());
        raft_quorum_count_reached(self.evidence_.voter_count(), quorum)
    }

    pub fn all_completed(&self) -> bool {
        self.evidence_.all_completed()
    }

    // Set equality against the launching membership, spelled with len() and
    // contains() so it means the same thing under the Vec-backed rustc model
    // as under the real C++ btree. `sites` is the current config, sorted and
    // duplicate-free, which is what a std::set iteration yields.
    pub fn config_matches(&self, sites: &[u16]) -> bool {
        if self.config_.len() != sites.len() {
            return false;
        }
        let mut i: usize = 0;
        while i < sites.len() {
            if !self.config_.contains(&sites[i]) {
                return false;
            }
            i += 1;
        }
        true
    }
}

// The context one reply carries. Grouped into a value rather than passed as
// seven parameters, which clippy rejects and which reads worse at the call
// site: PHASE 2 is describing one event, not supplying seven unrelated
// arguments.
#[repr(C)]
pub struct AuthorityReply {
    sent_round_: u64,
    follower_: u16,
    sent_term_: u64,
    response_term_: u64,
    current_term_: u64,
    is_leader_: bool,
    response_available_: bool,
}

impl AuthorityReply {
    pub fn new(sent_round: u64, follower: u16, sent_term: u64,
               response_term: u64, current_term: u64, is_leader: bool,
               response_available: bool) -> AuthorityReply {
        AuthorityReply {
            sent_round_: sent_round,
            follower_: follower,
            sent_term_: sent_term,
            response_term_: response_term,
            current_term_: current_term,
            is_leader_: is_leader,
            response_available_: response_available,
        }
    }

    pub fn sent_round(&self) -> u64 { self.sent_round_ }
    pub fn follower(&self) -> u16 { self.follower_ }
    pub fn sent_term(&self) -> u64 { self.sent_term_ }
    pub fn response_term(&self) -> u64 { self.response_term_ }
    pub fn current_term(&self) -> u64 { self.current_term_ }
    pub fn is_leader(&self) -> bool { self.is_leader_ }
    pub fn response_available(&self) -> bool { self.response_available_ }
}

// The outcome of one settlement pass: at most one generation is published.
#[repr(C)]
pub struct AuthorityOutcome {
    confirmed_: bool,
    term_: u64,
    round_id_: u64,
    voter_count_: usize,
    config_size_: usize,
}

impl AuthorityOutcome {
    pub fn confirmed(&self) -> bool { self.confirmed_ }
    pub fn term(&self) -> u64 { self.term_ }
    pub fn round_id(&self) -> u64 { self.round_id_ }
    pub fn voter_count(&self) -> usize { self.voter_count_ }
    pub fn config_size(&self) -> usize { self.config_size_ }
}

pub struct AuthorityLedger {
    generations_: rusty::Vec<AuthorityGeneration>,
}

#[allow(clippy::new_without_default)]
impl AuthorityLedger {
    pub fn new() -> AuthorityLedger {
        AuthorityLedger { generations_: rusty::Vec::new() }
    }

    // Dropped wholesale on leadership loss or a term change, so a prior
    // epoch's evidence can never be counted against the new one.
    pub fn abandon(&mut self) {
        self.generations_.clear();
    }

    // Opens a generation over the membership that launched it. Returns false
    // if this round id is already present, which can only be the deliberately
    // fail-closed UINT64_MAX saturation generation; the caller asserts that.
    pub fn open(&mut self, round_id: u64, config: &[u16],
                evidence: HeartbeatAuthority) -> bool {
        if self.index_of(round_id) < self.generations_.len() {
            return false;
        }
        let mut snapshot: SiteSet = SiteSet::new();  // [move, M9]
        let mut i: usize = 0;
        while i < config.len() {
            snapshot.insert(config[i]);
            i += 1;
        }
        self.generations_.push(AuthorityGeneration {
            round_id_: round_id,
            config_: snapshot,
            evidence_: evidence,
        });
        true
    }

    // Returns generations_.len() when absent. An index, never a reference, so
    // nothing can dangle across an RPC send or a re-entrant callback.
    pub fn index_of(&self, round_id: u64) -> usize {
        let n = self.generations_.len();
        let mut i: usize = 0;
        while i < n {
            if self.generations_[i].round_id_ == round_id {
                return i;
            }
            i += 1;
        }
        n
    }

    pub fn len(&self) -> usize {
        self.generations_.len()
    }

    pub fn is_empty(&self) -> bool {
        self.generations_.is_empty()
    }

    pub fn launch(&mut self, round_id: u64, site: u16) -> bool {
        let index = self.index_of(round_id);
        if index >= self.generations_.len() {
            return false;
        }
        self.generations_[index].evidence_.launch(site);
        true
    }

    pub fn has_quorum(&self, round_id: u64) -> bool {
        let index = self.index_of(round_id);
        if index >= self.generations_.len() {
            return false;
        }
        self.generations_[index].has_quorum()
    }

    // One reply arrives. The RPC is retired unconditionally, and counted as a
    // vote only if it proves this exact generation: same term, a follower that
    // was in the launching membership, and the reply predicate agreeing.
    pub fn record_reply(&mut self, reply: &AuthorityReply) {
        let index = self.index_of(reply.sent_round());
        if index >= self.generations_.len() {
            return;
        }
        self.generations_[index].evidence_.retire(reply.follower());
        let matches_term =
            self.generations_[index].evidence_.term() == reply.sent_term();
        let was_member =
            self.generations_[index].config_.contains(&reply.follower());
        if matches_term && was_member &&
            raft_server_read_index_reply_confirms_authority(
                reply.response_available(), reply.is_leader(),
                reply.sent_term(), reply.response_term(),
                reply.current_term(), reply.sent_round(),
                self.generations_[index].round_id_) {
            self.generations_[index].evidence_.record_vote(reply.follower());
        }
    }

    // Publishes at most one generation and retires every generation that can
    // no longer contribute. Generations are held in ascending round order, and
    // the running confirmation is consulted as it advances, so the highest
    // round reaching quorum wins -- the same outcome the ascending std::map
    // scan produced.
    pub fn settle(&mut self, is_leader: bool, current_term: u64,
                  current_config: &[u16],
                  confirmed_term: u64, confirmed_round: u64)
                  -> AuthorityOutcome {
        let mut outcome = AuthorityOutcome {
            confirmed_: false,
            term_: 0,
            round_id_: 0,
            voter_count_: 0,
            config_size_: 0,
        };
        let mut running_term = confirmed_term;
        let mut running_round = confirmed_round;
        let mut i: usize = 0;
        while i < self.generations_.len() {
            let context_is_current = is_leader &&
                current_term == self.generations_[i].evidence_.term() &&
                self.generations_[i].config_matches(current_config);
            let already_published = running_term ==
                self.generations_[i].evidence_.term() &&
                self.generations_[i].round_id_ <= running_round;
            if !context_is_current || already_published {
                self.generations_.remove(i);
                continue;
            }
            if self.generations_[i].has_quorum() {
                running_term = self.generations_[i].evidence_.term();
                running_round = self.generations_[i].round_id_;
                outcome = AuthorityOutcome {
                    confirmed_: true,
                    term_: running_term,
                    round_id_: running_round,
                    voter_count_: self.generations_[i].evidence_.voter_count(),
                    config_size_: self.generations_[i].evidence_.config_size(),
                };
                self.generations_.remove(i);
                continue;
            }
            if self.generations_[i].all_completed() {
                // Every RPC launched in this generation completed without a
                // quorum. No later event can add evidence to it.
                self.generations_.remove(i);
                continue;
            }
            i += 1;
        }
        outcome
    }
}

pub struct HeartbeatRoundScope {
    term_: u64,
    round_id_: u64,
    config_: SiteSet,  // [move, M9]
    current_commit_index_: u64,
    authority_inserted_: bool,
}

#[allow(clippy::new_without_default)]
impl HeartbeatRoundScope {
    pub fn new() -> HeartbeatRoundScope {
        HeartbeatRoundScope {
            term_: 0,
            round_id_: 0,
            config_: SiteSet::new(),  // [move, M9]
            current_commit_index_: 0,
            authority_inserted_: false,
        }
    }

    // Opens a round. Term, generation and membership are latched together so
    // no later phase can observe a half-established scope, and the previous
    // round's membership is dropped rather than accumulated.
    pub fn begin(&mut self, term: u64, round_id: u64) {
        self.term_ = term;
        self.round_id_ = round_id;
        self.config_.clear();
        self.current_commit_index_ = 0;
        self.authority_inserted_ = false;
    }

    pub fn admit(&mut self, site: u16) {
        self.config_.insert(site);
    }

    pub fn term(&self) -> u64 {
        self.term_
    }

    pub fn round_id(&self) -> u64 {
        self.round_id_
    }

    // The replica count this round was launched against, membership snapshot
    // included, which is what every quorum decision divides by.
    pub fn nservers(&self) -> usize {
        self.config_.len()
    }

    pub fn is_member(&self, site: u16) -> bool {
        self.config_.contains(&site)
    }

    // The commit index the round puts on the wire. Published by PHASE 0 after
    // it recalculates, read by PHASE 1 when it builds each AppendEntries.
    pub fn publish_commit_index(&mut self, index: u64) {
        self.current_commit_index_ = index;
    }

    pub fn commit_index(&self) -> u64 {
        self.current_commit_index_
    }

    // Whether this round owns a fresh authority generation. False only in the
    // deliberately fail-closed UINT64_MAX saturation case, where PHASE 1 must
    // not record evidence against a reused generation.
    pub fn set_authority_inserted(&mut self, inserted: bool) {
        self.authority_inserted_ = inserted;
    }

    pub fn authority_inserted(&self) -> bool {
        self.authority_inserted_
    }
}

// ==========================================================================
// WHAT A CORE DECISION ASKS THE SHELL TO DO ([move, M3], Phase 2)
//
// A core function makes no FFI call, takes no lock, reads no clock and fires
// no callback. Where it used to, it pushes an action here, at the same
// point, and the shell carries the actions out in push order: under mtx_,
// before the guard is released (RaftServerBase::run_locked_actions), except
// the role change's log entry and leader-change callback, which run once
// it is released ([fix, F6], run_unlocked_actions).
// ==========================================================================
#[allow(non_camel_case_types)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Eq, PartialEq))]
#[repr(i32)]
pub enum CoreActionKind {
    // Hand the committed entries (from_, to_] to the apply queue
    // (EnqueueCommittedEntries).
    APPLY_RANGE = 0,
    // Restart the election timer (resetTimerLocked): the shell samples the
    // clock and a timeout, and RaftCore::reset_election_timer records them
    // (M4).
    RESET_ELECTION = 1,
    // The new leader's no-op: the shell builds the command and appends it,
    // then wakes replication (AppendLeaderNoop).
    APPEND_NOOP = 2,
    // A role was set (raft_log_set_is_leader_entry), and on a transition
    // the leader-change callback is due.
    ROLE_SET = 3,
}

// Which resetTimerLocked call an action stands for. The shell turns it back
// into the reason string the timer reset logs.
#[allow(non_camel_case_types)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Eq, PartialEq))]
#[repr(i32)]
pub enum TimerResetReason {
    BECAME_FOLLOWER = 0,
    STEP_DOWN = 1,
    GRANTED_VOTE = 2,
    APPEND_ENTRIES = 3,
    STARTING_CAMPAIGN = 4,  // [move, M5]
}

pub struct CoreAction {
    kind_: CoreActionKind,
    // APPLY_RANGE
    from_: u64,
    to_: u64,
    // RESET_ELECTION
    reason_: TimerResetReason,
    // ROLE_SET: the term and the role before and after the call, and whether
    // it was a transition, and which.
    term_: u64,
    prev_is_leader_: bool,
    is_leader_: bool,
    became_leader_: bool,
    became_follower_: bool,
}

impl CoreAction {
    fn of_kind(kind: CoreActionKind) -> CoreAction {
        CoreAction {
            kind_: kind,
            from_: 0,
            to_: 0,
            reason_: TimerResetReason::BECAME_FOLLOWER,
            term_: 0,
            prev_is_leader_: false,
            is_leader_: false,
            became_leader_: false,
            became_follower_: false,
        }
    }

    pub fn apply_range(from: u64, to: u64) -> CoreAction {
        let mut action = CoreAction::of_kind(CoreActionKind::APPLY_RANGE);
        action.from_ = from;
        action.to_ = to;
        action
    }

    pub fn reset_election(reason: TimerResetReason) -> CoreAction {
        let mut action = CoreAction::of_kind(CoreActionKind::RESET_ELECTION);
        action.reason_ = reason;
        action
    }

    pub fn append_noop() -> CoreAction {
        CoreAction::of_kind(CoreActionKind::APPEND_NOOP)
    }

    pub fn role_set(term: u64, prev_is_leader: bool, is_leader: bool,
                    became_leader: bool, became_follower: bool) -> CoreAction {
        let mut action = CoreAction::of_kind(CoreActionKind::ROLE_SET);
        action.term_ = term;
        action.prev_is_leader_ = prev_is_leader;
        action.is_leader_ = is_leader;
        action.became_leader_ = became_leader;
        action.became_follower_ = became_follower;
        action
    }

    pub fn kind(&self) -> CoreActionKind { self.kind_ }
    pub fn from(&self) -> u64 { self.from_ }
    pub fn to(&self) -> u64 { self.to_ }
    pub fn reason(&self) -> TimerResetReason { self.reason_ }
    pub fn term(&self) -> u64 { self.term_ }
    pub fn prev_is_leader(&self) -> bool { self.prev_is_leader_ }
    pub fn is_leader(&self) -> bool { self.is_leader_ }
    pub fn became_leader(&self) -> bool { self.became_leader_ }
    pub fn became_follower(&self) -> bool { self.became_follower_ }
}

// The actions of one critical section, in push order.
#[cfg_attr(not(any()), derive(Default))]
pub struct CoreOutput {
    actions_: rusty::Vec<CoreAction>,
}

impl CoreOutput {
    pub fn new() -> CoreOutput {
        CoreOutput { actions_: rusty::Vec::new() }
    }

    pub fn push(&mut self, action: CoreAction) {
        self.actions_.push(action);
    }

    pub fn len(&self) -> usize {
        self.actions_.len()
    }

    pub fn is_empty(&self) -> bool {
        self.actions_.is_empty()
    }

    pub fn at(&self, i: usize) -> &CoreAction {
        &self.actions_[i]
    }
}

// [move, M5] One campaign's outcome as the core counted it: the fields
// raft_vote_quorum_snapshot used to read out of the lane's quorum object,
// field for field.
pub struct VoteOutcome {
    pub term_: i64,
    pub yes_: bool,
    pub no_: bool,
    pub n_voted_yes_: i32,
    pub n_voted_no_: i32,
    pub timeouted_: bool,
}

// [move, M5] One campaign's votes, counted by the core with the rule both
// lanes' tallies use (RaftVoteQuorumEvent, raft-rt's TallyState): yes once
// the peer yes votes reach n/2, no once the peer no votes exceed n - n/2
// (the off-by-one bugs-found B1 records, kept for lane parity). Each voter
// counts once ([fix, F1], now on every lane), and a reply term that is
// non-negative and higher than any seen is kept, as FeedResponse did.
pub struct VoteSet {
    voters_: SiteSet,
    yes_: u64,
    no_: u64,
    highest_term_: i64,
}

#[allow(clippy::new_without_default)]
impl VoteSet {
    pub fn new() -> VoteSet {
        VoteSet { voters_: SiteSet::new(), yes_: 0, no_: 0, highest_term_: 0 }
    }

    pub fn feed(&mut self, voter: u16, granted: bool, term: i64) {
        if !self.voters_.insert(voter) {
            return;
        }
        if term >= 0 && term > self.highest_term_ {
            self.highest_term_ = term;
        }
        if granted {
            self.yes_ += 1;
        } else {
            self.no_ += 1;
        }
    }

    // `n_total` is the configured partition size, self included, as the
    // lane counted it.
    pub fn outcome(&self, n_total: u64, timed_out: bool) -> VoteOutcome {
        let quorum: u64 = n_total / 2;
        VoteOutcome {
            term_: self.highest_term_,
            yes_: self.yes_ >= quorum,
            no_: self.no_ > n_total - quorum,
            n_voted_yes_: self.yes_ as i32,
            n_voted_no_: self.no_ as i32,
            timeouted_: timed_out,
        }
    }
}

// [move, M5] What start_election decided: whether the campaign starts, and
// the values the broadcast and its log line need.
pub struct CampaignStart {
    pub started_: bool,
    pub term_: u64,
    pub prev_term_: u64,
    pub prev_vote_for_: u16,
    pub lst_idx_: u64,
    pub lst_term_: i64,
}

impl CampaignStart {
    pub fn not_started() -> CampaignStart {
        CampaignStart {
            started_: false,
            term_: 0,
            prev_term_: 0,
            prev_vote_for_: RAFT_SERVER_INVALID_SITE_ID,
            lst_idx_: 0,
            lst_term_: 0,
        }
    }
}

#[repr(C)]
pub struct RaftCore {
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
    // be moved wholesale and the C++ that remains is `raft_foo(core);`.
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
    // [move, M1] The heartbeat round, formerly the driver's own
    // HeartbeatRoundState (server_cc.rs). At most one AppendEntries in flight
    // per follower; a synchronous follower may take longer than one heartbeat
    // interval, and its slot lets a later round consume the acknowledgement.
    pub pending_rpcs_: PendingTable,
    pub authority_rounds_: AuthorityLedger,
    pub pending_leader_term_: rusty::Option<u64>,
    // PHASE 0 establishes every field of this each round, so it needs no
    // reset; when PHASE 0 declines the round, phases 1-3 never read it.
    pub round_: HeartbeatRoundScope,
    // [move, M1] The configuration and the ordinal peer table, from
    // RaftServerBase, so a role change can rebuild the peers in the core.
    //
    // a std::set (current_config_) mirrored into this vector, because a
    // std::set is opaque to Rust; the set is gone and this is the only copy.
    // Sorted and duplicate-free is not cosmetic -- the round membership and
    // the authority ledger's set-equality check both rely on it -- so the
    // one writer (Setup) sorts and dedups before filling it.
    pub config_members_: rusty::Vec<u16>,
    // Ordinal peer table, rebuilt whenever the configuration changes.
    pub peer_sites_: rusty::Vec<u16>,
}

#[allow(clippy::new_without_default)]
impl RaftCore {
    pub fn new() -> RaftCore {
        RaftCore {
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
            pending_rpcs_: PendingTable::new(),  // [move, M1]
            authority_rounds_: AuthorityLedger::new(),  // [move, M1]
            pending_leader_term_: rusty::None,  // [move, M1]
            round_: HeartbeatRoundScope::new(),  // [move, M1]
            config_members_: rusty::Vec::new(),  // [move, M1]
            peer_sites_: rusty::Vec::new(),  // [move, M1]
        }
    }

    // [move, M1] What a fresh HeartbeatDriver used to start with: each run of
    // the heartbeat loop begins with an empty round state, as before.
    pub fn reset_round_state(&mut self) {
        self.pending_rpcs_ = PendingTable::new();
        self.authority_rounds_ = AuthorityLedger::new();
        self.pending_leader_term_ = rusty::None;
        self.round_ = HeartbeatRoundScope::new();
    }

    // [move, M1] RaftServerBase::RebuildPeerTables. Rebuilds the ordinal
    // peer table from the configuration.
    pub fn rebuild_peer_tables(&mut self, next_index: u64) {
        self.peer_sites_.clear();
        let mut self_is_a_member: bool = false;
        let mut i: usize = 0;
        while i < self.config_members_.len() {
            let peer_id: u16 = self.config_members_[i];
            if peer_id == self.site_id_ {
                self_is_a_member = true;
            } else {
                self.peer_sites_.push(peer_id);
            }
            i += 1;
        }
        let followers: usize = self.peer_sites_.len();
        self.peers_.reset(followers, next_index);
        // The C++ computed this as set.size() minus set.count(self); the
        // membership flag above is the same statement over a sorted vector.
        let expected: usize = if self_is_a_member {
            self.config_members_.len() - 1
        } else {
            self.config_members_.len()
        };
        assert!(self.peers_.len() == expected);  // [move, M10]
    }

    // [move, M1] The site id at an ordinal of the peer table.
    pub fn peer_site_at(&self, ordinal: usize) -> u16 {
        self.peer_sites_[ordinal]
    }

    // [move, M1] `current_config_.count(site) != 0`, over the cached vector.
    // A linear scan of three to five sorted u16s, which is what the std::set
    // lookup it replaces cost anyway.
    pub fn is_config_member(&self, site: u16) -> bool {
        let mut i: usize = 0;
        while i < self.config_members_.len() {
            if self.config_members_[i] == site {
                return true;
            }
            i += 1;
        }
        false
    }

    // [move, M1] Linear scan of a fixed, tiny table (replica counts are 3
    // or 5). Returns peers_.len() when the site is not a follower of this
    // leader, which is the "removed follower" case PHASE 2 guards against.
    // Deliberately an ordinal rather than a reference: an ordinal cannot
    // dangle across an RPC send or a re-entrant completion callback.
    pub fn peer_ordinal(&self, site: u16) -> usize {
        let mut ord: usize = 0;
        while ord < self.peer_sites_.len() {
            if self.peer_sites_[ord] == site {
                return ord;
            }
            ord += 1;
        }
        self.peers_.len()
    }

    // [move, M3] RaftServerBase::AppendLocal's append: one entry at the
    // current term, with its command's metadata as the shell asked it
    // (raft_command_meta). Reports the PRE-append tail: the new entry lands
    // at prev + 1. The caller holds mtx_.
    pub fn append_local(&mut self, cmd: rusty::RaftCommand, has_value: bool,
                        is_tpc_commit: bool, kind: i32,
                        payload_bytes: u64) -> u64 {
        let previous_index: u64 = self.raft_log_.last_index();
        let appended: u64 = self.raft_log_.append(RaftEntry::new(
            self.current_term_ as i64, cmd, has_value, is_tpc_commit, kind,
            payload_bytes));
        assert!(appended == previous_index + 1);  // [move, M10]
        previous_index
    }

    // [move, M1] RaftServerBase::LogTermChange's body (a log line), so a core
    // event can report its term changes itself.
    pub fn log_term_change(&self, reason: &str, old_term: u64, new_term: u64,
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

    // [move, M1] RaftServerBase::ElectionLastLogTermLocked: the term of the
    // last log entry, or the snapshot boundary term when the log has been
    // compacted past it. The caller holds mtx_.
    pub fn election_last_log_term(&self) -> i64 {
        let last_index: u64 = self.raft_log_.last_index();
        assert!(last_index >= self.snapidx_);  // [move, M10]
        if raft_server_election_last_log_uses_snapshot(
            last_index, self.snapidx_)
        {
            return self.snapterm_;
        }
        // The C++ went through FindRaftInstance, which flattened the Option
        // to a raw pointer and then verified it non-null. Asking the log
        // directly is the same lookup with the check kept.
        let last_log = self.raft_log_.get(last_index);
        assert!(last_log.is_some());  // [move, M10]
        last_log.unwrap().term()
    }

    // [move, M5] RaftServerBase::doVote, a core call: the caller
    // (raft_on_request_vote) holds mtx_. `election_debug` is the shell's
    // raft_election_debug_enabled(), `stopped` and `failover` its stop_ and
    // failover_.
    //
    // Records one RequestVote decision. The reply is written through two
    // out-params because that is what the srpc service layer's handler owns:
    // ballot_t* and bool_t*, which are int64_t and int8_t.
    #[allow(clippy::too_many_arguments)]
    pub fn do_vote(&mut self, lst_log_idx: u64, lst_log_term: i64,
                   can_id: u16, can_term: i64, reply_term: &mut i64,
                   vote_granted: &mut i8, vote: bool, stopped: bool,
                   failover: bool, election_debug: bool,
                   out: &mut CoreOutput) {
        *vote_granted = vote as i8;
        *reply_term = self.current_term_ as i64;

        // Was #ifdef RAFT_LEADER_ELECTION_DEBUG. The preprocessor has no DSL
        // spelling, so the switch is a branch on a constant the compiler
        // folds -- the same treatment raft_batch_optimization_enabled gets.
        if election_debug {
            rusty::raft_log_info_10(
                "[RAFT_VOTE] server {} (loc {}) vote={} candidate={} can_term={} cur_term={} prev_vote_for={} is_leader={} lst_idx={} lst_term={}",
                self.site_id_, self.loc_id_, vote, can_id, can_term,
                self.current_term_, self.vote_for_,
                self.is_leader_, lst_log_idx, lst_log_term);
        }

        if raft_server_signed_term_is_newer(can_term,
                                            self.current_term_) {
            let prev_term: u64 = self.current_term_;
            let was_leader: bool = self.is_leader_;
            // A RequestVote proves only that a candidate exists, not that
            // Raft has elected it. Do not keep advertising the previous
            // epoch's leader while processing the higher-term request.
            self.current_leader_id_ =
                raft_server_leader_hint_after_transition(false, false,
                                                         self.site_id_,
                                                         can_id);
            self.current_term_ = can_term as u64;
            // Reset the vote when advancing to a new term.
            self.vote_for_ = RAFT_SERVER_INVALID_SITE_ID;

            // A higher term is stable state even when this RequestVote is
            // denied.
            if was_leader {
                self.step_down(stopped, failover, out);  // [move, M3]
            } else {
                self.set_is_leader(false, stopped, failover, out);  // [move, M3]
            }
            self.req_voting_ = false;
            self.election_in_progress_ = false;

            // Publish the newly observed term, never the pre-transition one.
            *reply_term = self.current_term_ as i64;
            self.log_term_change("vote request carried newer term", prev_term,
                               self.current_term_, can_id);
        }

        if vote {
            self.set_is_leader(false, stopped, failover, out);  // [move, M3]
            self.vote_for_ = can_id;
            if election_debug {
                rusty::raft_log_info_3(
                    "[RAFT_VOTE] server {} recorded vote_for={} at term={}",
                    self.site_id_, self.vote_for_,
                    self.current_term_);
            }
            // doVote runs only from OnRequestVote, which holds mtx_.
            out.push(CoreAction::reset_election(TimerResetReason::GRANTED_VOTE));  // [move, M3]
        }
    }

    // [move, M5] RequestVoteImpl's first critical section as a core call: the
    // campaign's admission and its start. The caller holds mtx_ and passes
    // stop_ and the clock (`now`, read where the timer check read it).
    // `started_` is false when the campaign does not start.
    pub fn start_election(&mut self, timer_guarded: bool,
                          expected_generation: u64, now: u64, stopped: bool,
                          out: &mut CoreOutput) -> CampaignStart {
        let mut campaign: CampaignStart = CampaignStart::not_started();
        if stopped {
            self.req_voting_ = false;
            return campaign;
        }
        // This is the sole campaign admission point. Entrants can overlap
        // while one of them is yielding, so a caller must NOT reserve
        // req_voting_ before entering this critical section.
        if !raft_server_campaign_can_start(self.is_leader_,
                                           self.election_in_progress_)
        {
            return campaign;
        }
        if timer_guarded {
            let elapsed: u64 = now - self.last_heartbeat_time_;
            if !raft_server_timer_campaign_is_current(
                self.is_leader_, expected_generation,
                self.election_timer_generation_, elapsed,
                self.election_timeout_us_)
            {
                return campaign;
            }
        }

        // A campaign owns a fresh, latched timeout. If it loses without
        // hearing from a leader, the next campaign waits out this whole
        // interval instead of reusing the already-expired deadline.
        out.push(CoreAction::reset_election(TimerResetReason::STARTING_CAMPAIGN));
        campaign.prev_term_ = self.current_term_;
        campaign.prev_vote_for_ = self.vote_for_;
        let prev_local_term: u64 = self.current_term_;
        self.current_term_ += 1;
        // Vote for ourselves.
        self.vote_for_ = self.site_id_;
        // A candidate has no elected-leader evidence in its new term; in
        // particular it must not redirect clients to the leader of the term
        // it just left.
        self.current_leader_id_ = raft_server_leader_hint_after_transition(
            false, false, self.site_id_, self.current_leader_id_);

        // Publish ownership of req_voting_ and the election term before
        // broadcasting, so no second caller can campaign concurrently.
        self.election_in_progress_ = true;
        // election_term_ is ballot_t (int64_t) and current_term_ is
        // uint64_t; the C++ assigned across that implicitly.
        self.election_term_ = self.current_term_ as i64;
        self.req_voting_ = true;
        campaign.term_ = self.current_term_;

        let now_term: u64 = self.current_term_;
        self.log_term_change("starting election", prev_local_term, now_term,
                             RAFT_SERVER_INVALID_SITE_ID);
        campaign.lst_idx_ = self.raft_log_.last_index();
        campaign.lst_term_ = self.election_last_log_term();
        campaign.started_ = true;
        campaign
    }

    // [move, M5] RequestVoteImpl's second critical section as a core call:
    // the campaign settled from the replies its wait gathered, counted here
    // (VoteSet). The caller holds mtx_ and passes what the shell holds:
    // stop_, looping_, failover_, raft_election_debug_enabled(), and the
    // lane's quorum size and timeout.
    #[allow(clippy::too_many_arguments)]
    pub fn election_settle(&mut self, term: u64, loc_id: u32, voters: &[u16],
                           granted: &[bool], reply_terms: &[i64],
                           n_total: u64, timed_out: bool, stopped: bool,
                           looping: bool, failover: bool,
                           election_debug: bool,
                           out: &mut CoreOutput) -> bool {
        if stopped {
            self.election_in_progress_ = false;
            self.req_voting_ = false;
            return false;
        }
        // A higher term dominates every outcome, TIMEOUT and a concurrently
        // completed YES quorum included. FeedResponse publishes that maximum
        // before its wakeup, so it is snapshotted only now, after Raft state
        // has been reacquired.
        // [move, M5] The tally, counted here from the campaign's replies
        // rather than read out of the lane's quorum object.
        let mut votes: VoteSet = VoteSet::new();
        let mut r: usize = 0;
        while r < voters.len() {
            votes.feed(voters[r], granted[r], reply_terms[r]);
            r += 1;
        }
        let outcome: VoteOutcome = votes.outcome(n_total, timed_out);
        let observed_response_term: i64 = outcome.term_;
        let completion_action: i32 = raft_server_election_completion_action(
            self.election_in_progress_,
            self.election_term_ as u64, term, self.current_term_,
            observed_response_term);

        if completion_action == ElectionCompletionAction::ADVANCE_HIGHER_TERM as i32 {
            let previous_term: u64 = self.current_term_;
            self.current_term_ = observed_response_term as u64;
            self.vote_for_ = RAFT_SERVER_INVALID_SITE_ID;
            self.current_leader_id_ =
                raft_server_leader_hint_after_transition(
                    false, false, self.site_id_, self.current_leader_id_);

            if self.is_leader_ {
                self.step_down(stopped, failover, out);  // [move, M3]
            } else {
                self.set_is_leader(false, stopped, failover, out);  // [move, M3]
            }
            self.election_in_progress_ = false;
            self.req_voting_ = false;

            self.log_term_change("observed higher term from RequestVote replies",
                               previous_term, self.current_term_,
                               RAFT_SERVER_INVALID_SITE_ID);
            return false;
        }

        // An accepted leader RPC can cancel this campaign while the broadcast
        // is yielding, and another campaign can begin before this result
        // arrives. Only the exact active term owns role changes and election
        // bookkeeping. A strictly higher response term was handled above,
        // because that evidence supersedes even a newer local campaign.
        if completion_action == ElectionCompletionAction::IGNORE_STALE as i32 {
            if election_debug {
                rusty::raft_log_info_5(
                    "[RAFT_ELECTION] server {} ignoring stale election result: result_term={} local_term={} election_term={} active={}",
                    self.site_id_, term, self.current_term_,
                    self.election_term_,
                    self.election_in_progress_);
            }
            return false;
        }
        assert!(completion_action
            == ElectionCompletionAction::APPLY_CURRENT as i32);  // [move, M10]
        if election_debug {
            rusty::raft_log_info_6(
                "[RAFT_ELECTION] server {} term {} vote outcome yes={} no={} highest_term_seen={} timeout={}",
                self.site_id_, term, outcome.n_voted_yes_, outcome.n_voted_no_,
                outcome.term_, outcome.timeouted_);
        }

        if outcome.yes_ {
            assert!(self.current_term_ >= term);  // [move, M10]
            self.election_in_progress_ = false;
            self.req_voting_ = false;

            if stopped
                || self.current_term_ != term
            {
                self.req_voting_ = false;
                return false;
            }

            self.set_is_leader(true, stopped, failover, out);  // [move, M3]
            rusty::raft_log_debug_2("site {} became leader for term {}",
                                    self.site_id_, term);
            if election_debug {
                rusty::raft_log_info_4(
                    "[RAFT_ELECTION] server {} won election term {} (votes yes={} no={})",
                    self.site_id_, term, outcome.n_voted_yes_,
                    outcome.n_voted_no_);
            }

            if looping && self.is_leader_ {  // IsLeaderLocked
                rusty::raft_log_debug_2("vote accepted {} curterm {}",
                                        loc_id, self.current_term_);
                self.req_voting_ = false;
                true
            } else {
                rusty::raft_log_debug_2("vote rejected {} curterm {}, do rollback",
                                        loc_id, self.current_term_);
                    self.set_is_leader(false, stopped, failover, out);  // [move, M3]
                false
            }
        } else if outcome.no_ {
            rusty::raft_log_debug_1("site {} requestvote rejected", self.site_id_);
            self.set_is_leader(false, stopped, failover, out);  // [move, M3]
            if election_debug {
                rusty::raft_log_info_5(
                    "[RAFT_ELECTION] server {} lost election term {} (yes={} no={}) highest_term={}",
                    self.site_id_, term, outcome.n_voted_yes_,
                    outcome.n_voted_no_, outcome.term_);
            }
            if self.election_in_progress_
                && self.election_term_ == term as i64
            {
                self.election_in_progress_ = false;
            }
            self.req_voting_ = false;
            false
        } else {
            rusty::raft_log_debug_1("vote timeout {}", loc_id);
            if election_debug {
                rusty::raft_log_info_4(
                    "[RAFT_ELECTION] server {} election timed out term {} (yes={} no={})",
                    self.site_id_, term, outcome.n_voted_yes_,
                    outcome.n_voted_no_);
            }
            if self.election_in_progress_
                && self.election_term_ == term as i64
            {
                self.election_in_progress_ = false;
            }
            self.req_voting_ = false;
            false
        }
    }

    // [move, M3] PublishAppliedIndexLocked's decision: the applied index
    // never moves backward; a caller that tries is a bug, so it is reported
    // rather than obeyed. `published` is the shell's mirror. Returns whether
    // the index was recorded. The caller holds mtx_.
    pub fn on_applied(&mut self, index: u64, published: u64) -> bool {
        if raft_server_log_index_above(published, index) {
            rusty::raft_log_warn_3(
                "[RAFT-APPLY] Site {} refusing to move applied index backward from {} to {}",
                self.site_id_, published, index);
            return false;
        }
        self.execute_index_ = index;
        true
    }

    // [move, M4] resetTimerLocked's state change. The clock read and the
    // timeout sample are parameters: the shell samples both where
    // resetTimerLocked did. Returns the previous heartbeat time, for the
    // shell's log line. Advances the generation, so a concurrent heartbeat
    // reset cannot leave a campaign running off an expired snapshot.
    pub fn reset_election_timer(&mut self, now: u64, timeout_us: u64) -> u64 {
        let prev_time: u64 = self.last_heartbeat_time_;
        self.last_heartbeat_time_ = now;
        self.election_timeout_us_ = timeout_us;
        if self.election_timer_generation_ == u64::MAX {
            self.election_timer_generation_ = 1;
        } else {
            self.election_timer_generation_ += 1;
        }
        prev_time
    }

    // [move, M3] RaftServerBase::setIsLeader's decisions: the one place this
    // server's role changes. Its effects are actions, pushed where they used
    // to run: the new leader's no-op (APPEND_NOOP), the new follower's timer
    // reset (RESET_ELECTION), and the role's log entry and leader-change
    // callback (ROLE_SET, after mtx_ is released, [fix, F6]). `stopped` is
    // the shell's stop_ and `failover` its failover_; the caller holds mtx_.
    pub fn set_is_leader(&mut self, is_leader: bool, stopped: bool,
                         failover: bool, out: &mut CoreOutput) {
        let prev_is_leader: bool = self.is_leader_;
        // raft_log_set_is_leader_entry's term, which it read first.
        let entry_term: u64 = self.current_term_;

        if is_leader && !prev_is_leader {
            // Leadership publication must not proceed once shutdown began.
            let publication_term: u64 = self.current_term_;
            if stopped || self.current_term_ != publication_term {
                rusty::raft_log_warn_4(
                    "[RAFT_STATE] Site {} suppressing stale leadership publication for term {} (current={}, stopping={})",
                    self.site_id_, publication_term, self.current_term_,
                    stopped);
                out.push(CoreAction::role_set(entry_term, prev_is_leader,
                                              is_leader, false, false));
                return;
            }
        }

        if is_leader {
            // A heartbeat proof belongs to exactly one leadership term. Reset
            // the local generation BEFORE publishing this server as leader,
            // so delayed or historical acknowledgements cannot prove a quorum
            // in the new term.
            self.heartbeat_round_ = 0;
            self.read_quorum_confirmed_term_ = 0;
            self.read_quorum_confirmed_round_ = 0;
        }

        if is_leader && failover {
            let next_index: u64 = self.raft_log_.last_index() + 1;
            self.rebuild_peer_tables(next_index);
            let peers: usize = self.peers_.len();
            let mut ord: usize = 0;
            while ord < peers {
                let site: u16 = self.peer_site_at(ord);
                rusty::raft_log_debug_5(
                    "loc_id_={} match_index_[{}]={}, next_index_[{}]={}",
                    self.loc_id_, site, self.peers_.match_index(ord),
                    site, self.peers_.next_index(ord));
                ord += 1;
            }
        }

        // These two MUST be computed before is_leader_ is assigned, or they
        // both become false.
        let become_new_leader: bool = is_leader && !self.is_leader_;
        let become_new_follower: bool = !is_leader && self.is_leader_;

        self.is_leader_ = is_leader;

        // Becoming leader establishes self as the known leader. Becoming a
        // follower deliberately PRESERVES a hint learned from AppendEntries
        // or InstallSnapshot; transitions with no known leader clear it at
        // their own call sites.
        self.current_leader_id_ = raft_server_leader_hint_after_transition(
            is_leader,
            !is_leader && self.current_leader_id_ != RAFT_SERVER_INVALID_SITE_ID,
            self.site_id_,
            self.current_leader_id_);

        // Only on an actual transition, not on a no-op call.
        if become_new_leader || become_new_follower {
            rusty::raft_log_info_4(
                "RaftServer::setIsLeader site_id_ {} become_new_leader {} become_new_follower {} isLeader {}",
                self.site_id_, become_new_leader, become_new_follower, is_leader);
        }

        if become_new_leader {
            rusty::raft_log_info_4(
                "[RAFT_STATE] setIsLeader transition LEADER: site {} term {} prev_is_leader={} become_new_leader={}",
                self.site_id_, self.current_term_, prev_is_leader,
                become_new_leader);
            out.push(CoreAction::append_noop());
        } else if become_new_follower {
            rusty::raft_log_info_4(
                "[RAFT_STATE] setIsLeader transition FOLLOWER: site {} term {} prev_is_leader={} become_new_follower={}",
                self.site_id_, self.current_term_, prev_is_leader,
                become_new_follower);

            // Resetting the timer here is what prevents an instant election
            // after a resume: last_heartbeat_time_ is stale from before the
            // pause, so counting from NOW gives the current leader time to
            // send a heartbeat first. Standard Raft: a server stepping down
            // resets its timer.
            out.push(CoreAction::reset_election(TimerResetReason::BECAME_FOLLOWER));
            rusty::raft_log_info_2(
                "[RAFT_VIEW] Server {} stepping down as leader for partition {}",
                self.site_id_, self.partition_id_);
        }

        // The leader-change callback, so RaftWorker can retarget clients to
        // the new leader after an election.
        out.push(CoreAction::role_set(entry_term, prev_is_leader, is_leader,
                                      become_new_leader, become_new_follower));
    }

    // [move, M3] RaftServerBase::stepDown. CALLER MUST HOLD mtx_. Demotion
    // is terminal for the election in progress as well as for the leadership
    // epoch that is ending.
    pub fn step_down(&mut self, stopped: bool, failover: bool,
                     out: &mut CoreOutput) {
        rusty::raft_log_info_2(
            "[SPEC-RAFT] Site {}: Stepping down as leader (term={})",
            self.site_id_, self.current_term_);

        // Handles the leadership-change callback, the timer reset, and the
        // rest of the follower transition.
        self.set_is_leader(false, stopped, failover, out);

        // A late higher-term response can arrive after this server has
        // already entered a new candidacy.
        self.req_voting_ = false;
        self.election_in_progress_ = false;

        out.push(CoreAction::reset_election(TimerResetReason::STEP_DOWN));

        rusty::raft_log_info_1(
            "[SPEC-RAFT] Site {}: Step-down complete, now follower",
            self.site_id_);
    }
}

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
    // NOT UNWINDABLE, and saying so is a performance fix rather than a
    // documentation one. Rust Drop is unwindable in general, so the emitter
    // writes `~RaftLockGuard() noexcept(false)` by default -- and a
    // potentially-throwing destructor forces the compiler to keep unwind
    // state alive in EVERY scope that holds a guard. The Raft hot path holds
    // one in about thirty places, including the heartbeat round and the
    // AppendEntries handler, and `std::lock_guard`, which this replaced, has
    // a noexcept destructor.
    //
    // The body is one call to RaftCheckedMutex::unlock, which is a relaxed
    // atomic store and std::mutex::unlock. Neither can throw.
    #[cfg_attr(any(), cpp_noexcept)]
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
    // The same contract, for the same reason. std::mutex::unlock cannot
    // throw.
    #[cfg_attr(any(), cpp_noexcept)]
    fn drop(&mut self) {
        unsafe {
            raft_std_mutex_unlock(self.mutex_);
        }
    }
}

// The reactor event's two verbs, as kernels. `set` and `wait_timeout` are
// methods of the srpc::IntEvent behind the Arc, and the emitter renders a
// method call on an opaque carrier with a dot where the C++ needs an arrow,
// so they are named here as functions of the handle. Creation is the facade's
// `rusty::raft_new_int_event()`; a copy is the carrier's Clone.
unsafe extern "C" {
    fn raft_int_event_set(event: *const rusty::RaftIntEventPtr, value: i32);
    fn raft_int_event_wait_timeout(event: *const rusty::RaftIntEventPtr,
                                   timeout_us: u64);
}

pub struct ReplicationWakeGate {
    // The gate is pinned: it holds mutexes and atomics, so C++ deletes its
    // move constructor, and the DSL has to say so for two reasons. It makes
    // `rusty::sync::Arc::new(ReplicationWakeGate::new())` lower to
    // `rusty::Arc<ReplicationWakeGate>::make_with(...)`, the in-place seam
    // that places the payload directly in the allocation -- which is exactly
    // what RaftServer's hand-written constructor used to call. And it states
    // the invariant the waiters depend on: an armed waiter holds the gate's
    // address, so the gate must not move while anyone is waiting on it.
    _pin: core::marker::PhantomPinned,
    owner_: rusty::Mutex<rusty::Option<rusty::RaftPollThreadPtr>>,
    waiter_: rusty::Mutex<rusty::Option<rusty::RaftIntEventPtr>>,
    election_waiter_: rusty::Mutex<rusty::Option<rusty::RaftIntEventPtr>>,
    pending_: rusty::sync::atomic::AtomicBool,
    waiter_armed_: rusty::sync::atomic::AtomicBool,
    election_waiter_armed_: rusty::sync::atomic::AtomicBool,
    wake_job_queued_: rusty::sync::atomic::AtomicBool,
    shutdown_job_queued_: rusty::sync::atomic::AtomicBool,
    accepting_: rusty::sync::atomic::AtomicBool,
}

// A DECISION, not a deferral, so no TODO: clippy asks for `impl Default`
// alongside `fn new`, but a trait impl here would emit a second C++
// construction path into the generated struct that no C++ caller uses, and
// the owning `Arc::make_with` call in RaftServer's constructor names `new_()`
// explicitly. The Rust-only ergonomic is not worth the extra emitted surface.
#[allow(clippy::new_without_default)]
impl ReplicationWakeGate {
    pub fn new() -> ReplicationWakeGate {
        ReplicationWakeGate {
            _pin: core::marker::PhantomPinned,
            owner_: rusty::Mutex::new(rusty::None),
            waiter_: rusty::Mutex::new(rusty::None),
            election_waiter_: rusty::Mutex::new(rusty::None),
            pending_: rusty::sync::atomic::AtomicBool::new(false),
            waiter_armed_: rusty::sync::atomic::AtomicBool::new(false),
            election_waiter_armed_: rusty::sync::atomic::AtomicBool::new(false),
            wake_job_queued_: rusty::sync::atomic::AtomicBool::new(false),
            shutdown_job_queued_: rusty::sync::atomic::AtomicBool::new(false),
            accepting_: rusty::sync::atomic::AtomicBool::new(true),
        }
    }

    pub fn bind_owner(&self, owner: rusty::RaftPollThreadPtr) {
        let mut guard = self.owner_.lock().unwrap();
        *guard = rusty::Some(owner);
        self.accepting_.store(true, rusty::sync::atomic::Ordering::Release);
    }

    pub fn publish(&self) -> bool {
        self.pending_.store(true, rusty::sync::atomic::Ordering::Release);
        self.accepting_.load(rusty::sync::atomic::Ordering::Acquire)
    }

    pub fn close(&self) {
        self.accepting_.store(false, rusty::sync::atomic::Ordering::Release);
        self.pending_.store(true, rusty::sync::atomic::Ordering::Release);
    }

    pub fn clear_owner(&self) {
        let mut guard = self.owner_.lock().unwrap();
        *guard = rusty::None;
    }

    pub fn accepting(&self) -> bool {
        self.accepting_.load(rusty::sync::atomic::Ordering::Acquire)
    }

    pub fn reserve_wake_owner(
        &self,
    ) -> rusty::Option<rusty::RaftPollThreadPtr> {
        if !self.waiter_armed_.load(rusty::sync::atomic::Ordering::Acquire) {
            return rusty::None;
        }
        let guard = self.owner_.lock().unwrap();
        if self.wake_job_queued_.swap(true, rusty::sync::atomic::Ordering::AcqRel) {
            return rusty::None;
        }
        if (*guard).is_none() {
            self.wake_job_queued_.store(false, rusty::sync::atomic::Ordering::Release);
            return rusty::None;
        }
        // The owner's handle, copied: the Arc's copy constructor under the
        // transpiler, the clone kernel under rustc (the carrier's Clone).
        (*guard).clone()
    }

    pub fn reserve_shutdown_wake_owner(
        &self,
    ) -> rusty::Option<rusty::RaftPollThreadPtr> {
        if !self.waiter_armed_.load(rusty::sync::atomic::Ordering::Acquire)
            && !self
                .election_waiter_armed_
                .load(rusty::sync::atomic::Ordering::Acquire)
        {
            return rusty::None;
        }
        let guard = self.owner_.lock().unwrap();
        if self
            .shutdown_job_queued_
            .swap(true, rusty::sync::atomic::Ordering::AcqRel)
        {
            return rusty::None;
        }
        if (*guard).is_none() {
            self.shutdown_job_queued_
                .store(false, rusty::sync::atomic::Ordering::Release);
            return rusty::None;
        }
        // The owner's handle, copied: the Arc's copy constructor under the
        // transpiler, the clone kernel under rustc (the carrier's Clone).
        (*guard).clone()
    }

    // TODO(raft-dsl): drop this allow once the emitter lowers an `if let`
    // binding of an Option<Arc<T>> THROUGH the Arc. Clippy's suggested
    // `if let rusty::Some(event) = &waiter { event.set(1) }` is the better
    // Rust, and it transpiles, but the binding is emitted as `event.set(1)`
    // on a `rusty::Arc<srpc::IntEvent>` -- a dot, not an arrow -- which does
    // not compile. `as_ref().unwrap()` is emitted as `->set(1)`, which is
    // what the hand-written C++ this replaces already did, but ONLY when the
    // local carries an explicit type; an inferred `let` emits `const auto`
    // and the dot comes back. Hence the annotations below, which are
    // load-bearing rather than documentation. Verify by switching the two
    // bodies back to `if let` and rebuilding src/deptran/raft.
    #[allow(clippy::unnecessary_unwrap)]
    pub fn wake_on_owner(&self) {
        if !self.waiter_armed_.load(rusty::sync::atomic::Ordering::Acquire) {
            self.wake_job_queued_.store(false, rusty::sync::atomic::Ordering::Release);
            return;
        }
        // The guard is a temporary of this statement, so waiter_ is unlocked
        // again before the set() below: set() may make the heartbeat fiber
        // runnable, and that fiber takes waiter_ in DisarmWaiter.
        let waiter: rusty::Option<rusty::RaftIntEventPtr> =
            self.clone_waiter(&self.waiter_);
        if waiter.is_some() {
            unsafe {
                raft_int_event_set(
                    waiter.as_ref().unwrap() as *const rusty::RaftIntEventPtr, 1);
            }
        }
    }

    // See the TODO on wake_on_owner for why this is not `if let`.
    #[allow(clippy::unnecessary_unwrap)]
    pub fn wake_shutdown_on_owner(&self) {
        // Both guards are statement temporaries; neither lock is held across
        // the set() calls below, for the reason given in wake_on_owner.
        let heartbeat_waiter: rusty::Option<rusty::RaftIntEventPtr> =
            self.clone_waiter(&self.waiter_);
        let election_waiter: rusty::Option<rusty::RaftIntEventPtr> =
            self.clone_waiter(&self.election_waiter_);
        if heartbeat_waiter.is_some() {
            unsafe {
                raft_int_event_set(
                    heartbeat_waiter.as_ref().unwrap()
                        as *const rusty::RaftIntEventPtr, 1);
            }
        }
        if election_waiter.is_some() {
            unsafe {
                raft_int_event_set(
                    election_waiter.as_ref().unwrap()
                        as *const rusty::RaftIntEventPtr, 1);
            }
        }
    }

    pub fn begin_wait_for_work(&self) -> rusty::Option<bool> {
        if !self.accepting_.load(rusty::sync::atomic::Ordering::Acquire) {
            return rusty::Some(false);
        }
        if self.pending_.swap(false, rusty::sync::atomic::Ordering::AcqRel) {
            return rusty::Some(self.accepting_.load(rusty::sync::atomic::Ordering::Acquire));
        }
        rusty::None
    }

    pub fn finish_wait_for_work(
        &self,
        waiter: rusty::RaftIntEventPtr,
        timeout_us: u64,
    ) -> bool {
        unsafe {
            raft_int_event_set(&waiter as *const rusty::RaftIntEventPtr, 0);
        }
        {
            let mut guard = self.waiter_.lock().unwrap();
            *guard = rusty::Some(waiter.clone());
        }
        self.waiter_armed_.store(true, rusty::sync::atomic::Ordering::Release);
        if self.pending_.swap(false, rusty::sync::atomic::Ordering::AcqRel) {
            self.disarm_waiter();
            return self.accepting_.load(rusty::sync::atomic::Ordering::Acquire);
        }
        unsafe {
            raft_int_event_wait_timeout(
                &waiter as *const rusty::RaftIntEventPtr, timeout_us);
        }
        self.pending_.swap(false, rusty::sync::atomic::Ordering::AcqRel);
        self.disarm_waiter();
        self.accepting_.load(rusty::sync::atomic::Ordering::Acquire)
    }

    pub fn wait_for_election_timeout(
        &self,
        waiter: rusty::RaftIntEventPtr,
        timeout_us: u64,
    ) -> bool {
        unsafe {
            raft_int_event_set(&waiter as *const rusty::RaftIntEventPtr, 0);
        }
        {
            let mut guard = self.election_waiter_.lock().unwrap();
            *guard = rusty::Some(waiter.clone());
        }
        self.election_waiter_armed_
            .store(true, rusty::sync::atomic::Ordering::Release);
        if !self.accepting_.load(rusty::sync::atomic::Ordering::Acquire) {
            self.disarm_election_waiter();
            return false;
        }
        unsafe {
            raft_int_event_wait_timeout(
                &waiter as *const rusty::RaftIntEventPtr, timeout_us);
        }
        self.disarm_election_waiter();
        self.accepting_.load(rusty::sync::atomic::Ordering::Acquire)
    }

    // The armed waiter, copied out from under the gate's lock so the wake
    // itself happens with the lock released, as it always did. The copy is
    // the handle's Clone: the Arc's copy constructor under the transpiler,
    // the clone kernel under rustc (rusty-rustc/src/lib.rs).
    fn clone_waiter(
        &self,
        slot: &rusty::Mutex<rusty::Option<rusty::RaftIntEventPtr>>,
    ) -> rusty::Option<rusty::RaftIntEventPtr> {
        let guard = slot.lock().unwrap();
        (*guard).clone()
    }

    pub fn disarm_waiter(&self) {
        self.waiter_armed_.store(false, rusty::sync::atomic::Ordering::Release);
        {
            let mut guard = self.waiter_.lock().unwrap();
            *guard = rusty::None;
        }
        self.wake_job_queued_.store(false, rusty::sync::atomic::Ordering::Release);
    }

    pub fn disarm_election_waiter(&self) {
        self.election_waiter_armed_
            .store(false, rusty::sync::atomic::Ordering::Release);
        let mut guard = self.election_waiter_.lock().unwrap();
        *guard = rusty::None;
    }
}

// One queued wake, as the reactor carries it: the gate's handle and which
// wake to run. Boxed and made raw by RaftServerBase::queue_wake_job, taken
// back and dropped by the raft_wake_job_run export (server.cc).
pub struct GateWakeJob {
    pub gate: rusty::sync::Arc<ReplicationWakeGate>,
    pub shutdown: bool,
}

impl GateWakeJob {
    // The wake itself, here rather than in the export: the emitter knows
    // `gate` is an Arc only inside the block that declares it, and renders a
    // method call on it with an arrow; from another module it would emit a
    // dot, which does not compile.
    pub fn run(&self) {
        if self.shutdown {
            self.gate.wake_shutdown_on_owner();
        } else {
            self.gate.wake_on_owner();
        }
    }
}

use rusty::cpp_inherit;
use crate::scheduler_h::TxLogServer;
use crate::scheduler_h::RaftSpecific;
use crate::scheduler_h::RaftStartResult;

// The srpc `verify` macro, reachable from a DSL body. extern "C" is the one
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
    fn raft_trace_at(stage: i32, idx: u64, t_us: u64);  // [M0] trace kit
    // Suspends the calling fiber (rt/src/seam.rs).
    fn raft_fiber_sleep_us(micros: u64);
    fn raft_time_now_us() -> u64;
    fn raft_snapshot_manager_is_set(manager: *const rusty::RaftSnapshotManagerPtr) -> bool;
    fn raft_snapshot_manager_ptr_clone_into(src: *const rusty::RaftSnapshotManagerPtr,
                                            dst: *mut rusty::RaftSnapshotManagerPtr);
    fn raft_random_range_us(low: u64, high: u64) -> u64;
    // The kernel bridge (server.cc). Each takes the base and, where it has
    // to reach a method that has not converted, casts down to RaftServer.
    fn raft_leader_change_cb_is_set(cb: *const rusty::RaftLeaderChangeCb) -> bool;
    fn raft_fire_leader_change(cb: *const rusty::RaftLeaderChangeCb, is_leader: bool);
    // The env-tunable election-timeout knobs, read in one shot. They were
    // four separate calls; a boundary crossing should be a verb that does a
    // unit of work rather than a getter, and GetElectionTimeout wants the
    // whole set to make one decision.
    fn raft_election_timeouts() -> RaftElectionTimeouts;
    fn raft_log_set_is_leader_entry(site_id: u16, loc_id: u32, term: u64,
                                    prev_is_leader: bool, new_is_leader: bool);
    // RAFT_TEST_CORO, as a predicate: conditional compilation has no spelling
    // in this dialect. The lab suite counts log entries, so the leader no-op
    // is skipped in lab mode, and the lab's initial role is set explicitly.
    // The leader no-op command: a janus::Command the DSL cannot construct.
    fn raft_noop_command_into(dst: *mut rusty::RaftCommand);
    // What RaftServer's constructor used to do after the generated one, for
    // ConstructRuntime: the shared lifetime gate whose `server` back-pointer
    // is this object (a std::make_shared), the heartbeat interval (a macro
    // whose value depends on RAFT_TEST), and the legacy payload registration.
    fn raft_new_callback_lifetime(server: *mut RaftServerHandle,
                                  out: *mut rusty::RaftAsyncCallbackLifetimePtr);
    fn raft_heartbeat_interval_default() -> u64;
    fn raft_ensure_legacy_payload_registered();
    // A copy of a janus::Command: a shared_ptr refcount the opaque carrier
    // cannot touch, made in C++ INTO a slot Rust owns. Never by value: a
    // non-trivial C++ object returned by value is an ABI mismatch under rustc,
    // and never `.clone()` on the carrier, which would be a bitwise copy.
    fn raft_command_clone_into(src: *const rusty::RaftCommand,
                               dst: *mut rusty::RaftCommand);
    // The four std::function carriers, copied INTO their final slot: a libc++
    // std::function is not bitwise-relocatable, so it is never held by value
    // anywhere but there (see reg_learner_action).
    fn raft_learner_action_clone_into(src: *const rusty::LearnerAction,
                                      dst: *mut rusty::LearnerAction);
    fn raft_leader_change_cb_clone_into(src: *const rusty::RaftLeaderChangeCb,
                                        dst: *mut rusty::RaftLeaderChangeCb);
    fn raft_create_snapshot_cb_clone_into(src: *const rusty::RaftCreateSnapshotCb,
                                          dst: *mut rusty::RaftCreateSnapshotCb);
    fn raft_prepare_snapshot_cb_clone_into(src: *const rusty::RaftPrepareSnapshotCb,
                                           dst: *mut rusty::RaftPrepareSnapshotCb);
    // The AppendEntries payload, read for AeDecodePayload / AeApplyIncoming.
    // A janus::Command laundered as c_void by the service forwarder; a batch
    // is opened once (raft_wire_batch: the one marshallable_cast, as before)
    // and then indexed, so the per-entry cost is unchanged.
    fn raft_wire_is_batch(cmd: *const core::ffi::c_void) -> bool;
    fn raft_wire_batch(cmd: *const core::ffi::c_void) -> *const core::ffi::c_void;
    fn raft_batch_len(batch: *const core::ffi::c_void) -> u64;
    fn raft_batch_term_at(batch: *const core::ffi::c_void, i: u64) -> i64;
    fn raft_batch_command_into(batch: *const core::ffi::c_void, i: u64,
                               dst: *mut rusty::RaftCommand);
    fn raft_wire_command_clone_into(cmd: *const core::ffi::c_void,
                                    dst: *mut rusty::RaftCommand);
    // The partition's membership from the static (yaml) config: sorted,
    // de-duplicated site ids, one read per index. Startup only.
    fn raft_config_replica_count(partition_id: u32) -> u64;
    fn raft_config_replica_site(partition_id: u32, i: u64) -> u16;
    fn raft_election_debug_enabled() -> bool;
    // The campaign broadcast, and the reply quorum read back under mtx_.
    fn raft_broadcast_vote_and_wait(server: *mut RaftServerHandle, par_id: u32,
                                    last_log_index: u64, last_log_term: i64,
                                    self_site_id: u16, term: i64,
                                    out: *mut rusty::RaftVoteQuorumPtr);
    // [move, M5] A finished campaign's replies, as the lane's wait gathered
    // them, for the core's own count; with the quorum size and timeout the
    // lane used.
    fn raft_vote_quorum_size(quorum: *const rusty::RaftVoteQuorumPtr) -> u64;
    fn raft_vote_quorum_timed_out(quorum: *const rusty::RaftVoteQuorumPtr) -> bool;
    fn raft_vote_quorum_reply_count(quorum: *const rusty::RaftVoteQuorumPtr) -> u64;
    fn raft_vote_quorum_reply_at(quorum: *const rusty::RaftVoteQuorumPtr, i: u64,
                                 voter: *mut u16, granted: *mut bool,
                                 term: *mut i64) -> bool;
    fn raft_command_has_value(cmd: *const rusty::RaftCommand) -> bool;
    // [move, M6] has_value, is_tpc_commit, kind and payload_bytes in one call
    fn raft_command_meta(cmd: *const rusty::RaftCommand, has_value: *mut bool,
                         is_tpc_commit: *mut bool, kind: *mut i32,
                         payload_bytes: *mut u64);
    fn raft_apply_thread_join(thread: *mut rusty::RaftStdThread);
    fn raft_commo_set_network_enabled(server: *mut RaftServerHandle, enabled: bool);
    // The communicator binding: a dynamic_cast, which Rust cannot spell, plus
    // the insert into the server -> RaftCommo table the kernels resolve
    // through. See set_commo below for why the pointer is not a field.
    fn raft_bind_commo(server: *mut RaftServerHandle,
                       commo: *mut rusty::Communicator);
    // The reactor's PollThread::add, for the wake job: `owner` is the gate's
    // owner thread, `token` a Box<GateWakeJob> made raw (queue_wake_job),
    // which the queued OneTimeJob hands back to raft_wake_job_run once.
    fn raft_queue_wake_job(owner: *const rusty::RaftPollThreadPtr,
                           token: *mut core::ffi::c_void);
    // The InstallSnapshot exception boundary: the one catch that turns an
    // embedder throw into FailStop. It wraps OnInstallSnapshotLocked, whose
    // two locks are taken by OnInstallSnapshot below; `false` is the throw.
    // RequestVote and AppendEntries need no such kernel -- their bodies are
    // Rust and are called directly.
    fn raft_install_snapshot_guarded(server: *mut RaftServerHandle, site_id: u16,
                                     term: u64, leader_id: u64,
                                     last_included_index: u64,
                                     last_included_term: u64,
                                     data: *const rusty::RaftByteString,
                                     term_out: *mut u64) -> bool;
    fn raft_spawn_election_timer(server: *mut RaftServerHandle, wait_int_us: u64);
    fn raft_setup_internal_guarded(server: *mut RaftServerHandle, site_id: u16) -> bool;
    fn raft_shutdown_barrier_yield();
    // getenv, and nothing else. Returns null when the variable is unset or
    // empty. The PARSE is Rust -- see RaftServerBase::raft_env_u64 -- which is what removes
    // the three try/catch blocks this used to need: std::stoull throws, and
    // a hand-written digit loop cannot.
    fn raft_env_lookup(which: i32) -> *const core::ffi::c_char;
    fn raft_bind_replication_poll(server: *mut RaftServerHandle) -> bool;
    fn raft_initialize_snapshot_manager(server: *mut RaftServerHandle, site_id: u16) -> bool;
    // Only the std::thread construction. The flag and the loop are Rust.
    fn raft_spawn_apply_thread(server: *mut RaftServerHandle, thread: *mut rusty::RaftStdThread);
    // Nulls the server back-pointer the async-RPC gate holds, under the
    // gate's own mutex. Both the mutex and the shared_ptr live inside an
    // opaque C++ type, so this stays a kernel.
    fn raft_clear_async_callback_owner(lifetime: *const rusty::RaftAsyncCallbackLifetimePtr);
    fn raft_spawn_heartbeat_loop(server: *mut RaftServerHandle);
    fn raft_spawn_election_timer_fiber(server: *mut RaftServerHandle);
    fn raft_snapshot_serialize_and_save(
        create_cb: *const rusty::RaftCreateSnapshotCb,
        snapshot_manager: *const rusty::RaftSnapshotManagerPtr, site_id: u16,
        snap_index: u64, snap_term: i64) -> bool;
    fn raft_install_snapshot_payload(
        prepare_cb: *const rusty::RaftPrepareSnapshotCb,
        snapshot_manager: *const rusty::RaftSnapshotManagerPtr, site_id: u16,
        last_included_index: u64, last_included_term: u64,
        data: *const rusty::RaftByteString) -> i32;
    fn raft_apply_invoke(app_next: *const rusty::LearnerAction,
                         pending: *const rusty::RaftCommand, site_id: u16,
                         id: u64) -> bool;
    fn raft_monotonic_now_secs() -> u64;
    fn raft_thread_sleep_ms(millis: u64);
    fn raft_env_snapshots_enabled() -> bool;
    fn raft_prepare_snapshot_cb_is_set(cb: *const rusty::RaftPrepareSnapshotCb) -> bool;
    fn raft_snapshot_recovery_pick_manager(
        current: *const rusty::RaftSnapshotManagerPtr,
        out: *mut rusty::RaftSnapshotManagerPtr);
    fn raft_snapshot_manager_latest(
        manager: *const rusty::RaftSnapshotManagerPtr, index: *mut u64,
        term: *mut u64) -> bool;
    fn raft_snapshot_manager_load(
        manager: *const rusty::RaftSnapshotManagerPtr,
        data: *mut rusty::RaftByteString, index: *mut u64, term: *mut u64,
        size_bytes: *mut u64) -> bool;
    fn raft_load_state_machine_snapshot(
        prepare_cb: *const rusty::RaftPrepareSnapshotCb, site_id: u16,
        data: *const rusty::RaftByteString,
        last_included_index: u64, last_included_term: u64) -> bool;
}

// One campaign's reply quorum, read in a single shot.
//
// RequestVoteImpl must snapshot these AFTER reacquiring mtx_ -- FeedResponse
// publishes the highest observed term before its wakeup -- so the broadcast
// kernel hands back the event and this is filled by a second kernel called
// under the lock. Splitting it that way is not a detail: reading the term
// inside the broadcast kernel would sample it before the lock and lose the
// ordering the comment at that call site exists to protect.
// One entry waiting for the apply thread. A conflicting snapshot increments
// ApplyQueue::epoch_, so an entry popped before the invalidation carries a
// stale epoch and is skipped instead of applied after it.
//
// This was a C++ struct in a std::deque that five kernels reached into. It is
// a DSL struct in a rusty::VecDeque now, which is the difference between Rust
// describing the queue and Rust asking C++ about it: the push, the pop, the
// purge and the two size reads are all ordinary Rust below, and nothing
// crosses the language boundary to touch the queue any more.
//
// command_ stays opaque. Rust moves it from the log into the queue and out
// again into pending_apply_command_, and never looks inside -- the payload is
// a Marshallable hierarchy that only C++ can decode.
#[repr(C)]
pub struct QueuedApplyEntry {
    pub index_: u64,
    pub command_: rusty::RaftCommand,
    pub epoch_: u64,
}

// The queue and the epoch that invalidates it, as one type, because they are
// one invariant: an entry is applicable only while its epoch still matches,
// and a snapshot that replaces the log bumps the epoch in the same critical
// section that empties the queue. Holding them as two fields beside a mutex
// stated that relationship in a comment; holding them inside one
// rusty::Mutex<ApplyQueue> states it in the type, and there is no longer a
// spelling for reading either one unlocked.
#[repr(C)]
pub struct ApplyQueue {
    pub entries_: rusty::VecDeque<QueuedApplyEntry>,
    pub epoch_: u64,
}

#[allow(clippy::new_without_default)]
impl ApplyQueue {
    pub fn new() -> ApplyQueue {
        ApplyQueue { entries_: rusty::VecDeque::new(), epoch_: 0 }
    }
}

// [move, M5] The heartbeat's response handles, by peer ordinal: the shell's
// half of the in-flight slots, beside RaftCore::pending_rpcs_'s protocol
// half. Each AppendEntries reply lands in its handle; the collection loop
// polls the handles and hands each completed reply to the core. Touched only
// by the heartbeat fiber.
pub struct AppendResponseSlot {
    response_: rusty::RaftResponsePtr,
    // The round the RPC went out in, so the loop knows whether it is still
    // waiting for this round without asking the core.
    sent_round_: u64,
}

pub struct AppendResponses {
    slots_: rusty::Vec<rusty::Option<AppendResponseSlot>>,
}

#[allow(clippy::new_without_default)]
impl AppendResponses {
    pub fn new() -> AppendResponses {
        AppendResponses { slots_: rusty::Vec::new() }
    }

    // `peers` empty slots: what the core's table holds after it resizes or
    // abandons its own.
    pub fn reset(&mut self, peers: usize) {
        self.slots_.clear();
        let mut i: usize = 0;
        while i < peers {
            self.slots_.push(rusty::None);
            i += 1;
        }
    }

    pub fn len(&self) -> usize {
        self.slots_.len()
    }

    pub fn is_empty(&self) -> bool {
        self.slots_.is_empty()
    }

    pub fn occupied(&self, ordinal: usize) -> bool {
        self.slots_[ordinal].is_some()
    }

    pub fn place(&mut self, ordinal: usize, response: rusty::RaftResponsePtr,
                 sent_round: u64) {
        self.slots_[ordinal] = rusty::Some(AppendResponseSlot {
            response_: response,
            sent_round_: sent_round,
        });
    }

    pub fn release(&mut self, ordinal: usize) {
        self.slots_[ordinal] = rusty::None;
    }

    pub fn sent_round(&self, ordinal: usize) -> u64 {
        if self.slots_[ordinal].is_none() {
            return 0;
        }
        self.slots_[ordinal].as_ref().unwrap().sent_round_
    }

    // The reference is safe because the method is &self: the emitter binds
    // the const unwrap overload, which returns a reference into the live
    // Option rather than a moved-out temporary. Callers check occupied()
    // first; unwrap is the assertion of that.
    pub fn response(&self, ordinal: usize) -> &rusty::RaftResponsePtr {
        &self.slots_[ordinal].as_ref().unwrap().response_
    }
}

// [fix, F6] Leader-change notices waiting to be fired, in transition order:
// a transition queues its notice under mtx_ (run_locked_actions), and
// whichever thread then finds the queue idle fires them all with no lock
// held (fire_leader_notices). true = became leader.
pub struct LeaderNotices {
    pub pending_: rusty::VecDeque<bool>,
    pub draining_: bool,
}

#[allow(clippy::new_without_default)]
impl LeaderNotices {
    pub fn new() -> LeaderNotices {
        LeaderNotices { pending_: rusty::VecDeque::new(), draining_: false }
    }
}

// Why an environment value was rejected. An error type rather than `()`
// because the distinction is worth logging: a typo and a number too large
// for u64 are different operator mistakes, and std::stoull reported the
// second by throwing std::out_of_range and the first by silently truncating.
#[allow(non_camel_case_types)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Eq, PartialEq))]
#[repr(i32)]
pub enum RaftEnvError {
    NOT_A_WHOLE_NUMBER = 0,
    OVERFLOWS_U64 = 1,
}

// Which environment variable raft_env_lookup should read. An i32 rather
// than a string, so nothing has to carry a Rust &str into C++.
pub const RAFT_ENV_HEARTBEAT_INTERVAL_US: i32 = 0;
pub const RAFT_ENV_LOG_RETENTION_WINDOW: i32 = 1;
pub const RAFT_ENV_SNAPSHOT_INTERVAL: i32 = 2;
// [fix, F5] 1: fail closed outside the verified configuration (SetupInternal).
pub const RAFT_ENV_VERIFIED_GATES: i32 = 3;


// appliedIndexForWait_ keeps its C++ spelling: it is read by name from
// test.cc and from the C++ bodies that have not converted yet, and renaming
// it is a mechanical change that belongs in its own commit.
#[allow(non_snake_case)]
#[repr(C)]
pub struct RaftServerBase {
    // Raft's own copy of what TXLOG_SERVER_SITE_FIELDS() gives Paxos; see the
    // mirror note on RaftCore for why both copies exist.
    pub loc_id_: u32,
    pub site_id_: u16,
    pub app_next_: rusty::LearnerAction,
    pub partition_id_: u32,
    pub mtx_: rusty::RaftCheckedMutex,
    // The consensus cluster mtx_ guards, as one Rust-owned value.
    pub core: RaftCore,
    // Scratch for raft_ae_decode_payload; valid only inside one
    // OnAppendEntries call, which is entirely under mtx_.
    pub decoded_terms_: rusty::Vec<i64>,
    // RPC futures can outlive the server during shutdown. Destruction nulls
    // this shared gate after waiting for any callback already using it.
    pub async_callback_lifetime_: rusty::RaftAsyncCallbackLifetimePtr,
    pub snapshot_manager_: rusty::RaftSnapshotManagerPtr,
    pub snapshot_manager_configured_: rusty::sync::atomic::AtomicBool,
    pub snapshot_trigger_index_: rusty::sync::atomic::AtomicU64,
    pub snapshot_trigger_threshold_: rusty::sync::atomic::AtomicU64,
    pub create_sm_snapshot_cb_: rusty::RaftCreateSnapshotCb,
    pub prepare_sm_snapshot_cb_: rusty::RaftPrepareSnapshotCb,
    pub stop_: rusty::sync::atomic::AtomicBool,
    pub rpc_ready_: rusty::sync::atomic::AtomicBool,
    // The heartbeat/election wake gate, shared with whatever owner-thread job
    // is queued against it -- hence Arc rather than a plain member.
    pub replication_wake_gate_: rusty::sync::Arc<ReplicationWakeGate>,
    // THE LOCK OWNS WHAT IT GUARDS. The flag lives inside the mutex rather
    // than beside it, so it is unreachable without the lock and no separate
    // wait/notify kernel pair is needed to keep the two in step.
    //
    // startup_succeeded_ stays outside deliberately: it is written before the
    // flag and read after the wait, so the mutex's own release/acquire
    // publishes it, and putting it inside would mean re-taking the lock to
    // read a value the waiter has already been handed exclusive sight of.
    pub startup_finished_: rusty::Mutex<bool>,
    pub startup_cv_: rusty::Condvar,
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
    // The preferred leader, written without mtx_ by SetPreferredLeader;
    // only the election timeout's choice reads it.
    pub preferred_leader_site_id_: rusty::sync::atomic::AtomicU64,
    pub startup_timestamp_: u64,
    // The replica set for this partition, sorted and duplicate-free. It was
    pub apply_thread_: rusty::RaftStdThread,
    pub apply_thread_running_: rusty::sync::atomic::AtomicBool,
    pub state_machine_apply_mtx_: rusty::RaftStdMutex,
    pub apply_queue_: rusty::Mutex<ApplyQueue>,
    // [fix, F6] Fired after mtx_ is released, in transition order.
    pub leader_notices_: rusty::Mutex<LeaderNotices>,
    // [move, M5] The heartbeat's response handles (the core holds the
    // protocol half of each in-flight slot). Heartbeat fiber only.
    pub append_responses_: AppendResponses,
    // [move, M3] OnInstallSnapshotLocked's actions, which reach it through a
    // C++ kernel (raft_install_snapshot_guarded) rather than a parameter;
    // OnInstallSnapshot runs their unlocked half. Touched only under mtx_
    // and by that one caller.
    pub install_out_: CoreOutput,
    // The command the apply thread popped and is about to hand to the
    // learner. A staging field rather than a local because Command is opaque
    // to Rust: the pop kernel moves it here and the invoke kernel reads it,
    // both under the apply thread's own serialisation.
    pub pending_apply_command_: rusty::RaftCommand,
    // PHASE 1's batch under assembly. A field rather than a local because
    // its element type is a wire command Rust cannot construct; the loop
    // that fills it is Rust, the pushes and the final wrap are kernels. Only
    // the heartbeat fiber touches it.
    //
    // It is cleared as the FIRST statement of the batch arm, unconditionally
    // and per follower, which is what makes it equivalent to the local
    // vector it replaces: no reader can see another follower's entries. The
    // one difference is that between two rounds it holds refcounts the local
    // would have dropped at end of scope -- bounded by max_batch_entries and
    // released on the next round, which runs every heartbeat interval
    // whether or not there is work.
    // PHASE 1's batch under assembly. Rust drives the loop that fills it,
    // clears it and reads its length; only the marshalling of each element
    // stays C++.
    pub batch_buffer_: rusty::Vec<rusty::RaftTpcCommitPtr>,
    pub appliedIndexForWait_: rusty::sync::atomic::AtomicU64,
    // [fix, F8] What other threads read without mtx_: published from the
    // core at the end of every critical section that can change them
    // (publish_mirrors). IsLeader, GetLeaderHint and CommitIndex read
    // these; the term is published with them.
    pub commit_index_mirror_: rusty::sync::atomic::AtomicU64,
    pub is_leader_mirror_: rusty::sync::atomic::AtomicBool,
    pub leader_hint_mirror_: rusty::sync::atomic::AtomicU64,
    pub term_mirror_: rusty::sync::atomic::AtomicU64,
    // [fix, F5] MAKO_RAFT_VERIFIED_GATES=1, read at setup: outside the
    // verified configuration the server fails closed, and inside it log
    // compaction and snapshots do nothing.
    pub verified_gates_: bool,
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
    // The address a kernel receives: kernels are declared over the opaque
    // RaftServerHandle (server_pods_h.rs) so their C declarations name only
    // global types, and this is the one cast from the real type.
    pub fn handle(&mut self) -> *mut RaftServerHandle {
        self as *mut RaftServerBase as *mut RaftServerHandle
    }

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
            partition_id_: 0,
            mtx_: Default::default(),
            core: RaftCore::new(),
            decoded_terms_: rusty::Vec::new(),
            // Null here; RaftServer's constructor allocates it, because it
            // also has to store `this` into the gate.
            async_callback_lifetime_: Default::default(),
            snapshot_manager_: Default::default(),
            snapshot_manager_configured_: rusty::sync::atomic::AtomicBool::new(false),
            snapshot_trigger_index_: rusty::sync::atomic::AtomicU64::new(0),
            snapshot_trigger_threshold_: rusty::sync::atomic::AtomicU64::new(10000),
            create_sm_snapshot_cb_: Default::default(),
            prepare_sm_snapshot_cb_: Default::default(),
            stop_: rusty::sync::atomic::AtomicBool::new(false),
            rpc_ready_: rusty::sync::atomic::AtomicBool::new(false),
            // new_cyclic, not new: the emitter lowers `Arc::new(T::new())` to
            // `Arc<T>::make(T::new_())`, a move into the allocation that a
            // PhantomPinned payload cannot make, and lowers new_cyclic to the
            // runtime's deferred-init path, which constructs the prvalue in
            // place (arc.hpp, new_cyclic). Checked on a scratch carrier, not
            // assumed. The Weak is unused: the gate keeps no handle to itself.
            replication_wake_gate_: rusty::sync::Arc::new_cyclic(
                |_weak| ReplicationWakeGate::new()),
            startup_finished_: rusty::Mutex::new(false),
            startup_cv_: rusty::Condvar::new(),
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
            preferred_leader_site_id_: rusty::sync::atomic::AtomicU64::new(
                RAFT_SERVER_INVALID_SITE_ID as u64),
            startup_timestamp_: 0,
            apply_thread_: Default::default(),
            apply_thread_running_: rusty::sync::atomic::AtomicBool::new(false),
            state_machine_apply_mtx_: Default::default(),
            apply_queue_: rusty::Mutex::new(ApplyQueue::new()),
            leader_notices_: rusty::Mutex::new(LeaderNotices::new()),  // [fix, F6]
            append_responses_: AppendResponses::new(),  // [move, M5]
            install_out_: CoreOutput::new(),  // [move, M3]
            pending_apply_command_: Default::default(),
            batch_buffer_: rusty::Vec::new(),
            appliedIndexForWait_: rusty::sync::atomic::AtomicU64::new(0),
            commit_index_mirror_: rusty::sync::atomic::AtomicU64::new(0),  // [fix, F8]
            is_leader_mirror_: rusty::sync::atomic::AtomicBool::new(false),  // [fix, F8]
            leader_hint_mirror_: rusty::sync::atomic::AtomicU64::new(
                RAFT_SERVER_INVALID_SITE_ID as u64),  // [fix, F8]
            term_mirror_: rusty::sync::atomic::AtomicU64::new(0),  // [fix, F8]
            verified_gates_: false,  // [fix, F5]
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
        self.core.snapidx_
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
        self.core.snapterm_ as u64
    }

    // @unsafe - returns the snapshot boundary term under mtx_.
    pub fn GetSnapshotTerm(&mut self) -> u64 {
        let _lock = RaftLockGuard::new(&mut self.mtx_);
        self.GetSnapshotTermLocked()
    }

    // CALLER MUST HOLD mtx_.
    pub fn SetSnapshotThresholdLocked(&mut self, threshold: u64) {
        self.core.snapshot_threshold_ = threshold;
        self.snapshot_trigger_threshold_
            .store(threshold, rusty::sync::atomic::Ordering::Release);
    }

    // @unsafe - takes mtx_.
    pub fn SetSnapshotThreshold(&mut self, threshold: u64) {
        let _lock = RaftLockGuard::new(&mut self.mtx_);
        self.SetSnapshotThresholdLocked(threshold);
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

    // @unsafe - the campaign the election timer starts. Timer-only entry:
    // it carries the reset generation observed at expiry, and the first
    // RequestVote state lock revalidates it immediately before term++.
    pub fn RequestVoteFromElectionTimer(&mut self, expected_generation: u64)
        -> bool
    {
        self.RequestVoteImpl(true, expected_generation)
    }

    // @safe - acquire load; the C++ read it the same way
    pub fn ElectionLoopStopped(&self) -> bool {
        self.stop_.load(rusty::sync::atomic::Ordering::Acquire)
    }

    // @unsafe - takes mtx_ to read core.req_voting_
    pub fn ElectionLoopVoting(&mut self) -> bool {
        let _lock = RaftLockGuard::new(&mut self.mtx_);
        self.core.req_voting_
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
        // [move, M4] the clock read, then the core's gather ([move, M5])
        let time_now: u64 = unsafe { raft_time_now_us() };
        raft_election_tick(&self.core, time_now)
    }

    // CALLER MUST HOLD mtx_. [move, M1] RaftCore::election_last_log_term.
    pub fn ElectionLastLogTermLocked(&self) -> i64 {
        self.core.election_last_log_term()
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
        if self.core.snapshot_callback_owner_token_ != callback_owner_token {
            return false;
        }
        self.create_sm_snapshot_cb_ = Default::default();
        self.prepare_sm_snapshot_cb_ = Default::default();
        self.core.snapshot_callback_owner_token_ = 0;
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

    // ------------------------------------------------------------------
    // Role and progress accessors.
    // ------------------------------------------------------------------

    // @safe - the preferred leader's site id (RAFT_SERVER_INVALID_SITE_ID
    // when none is configured).
    pub fn preferred_leader(&self) -> u16 {
        self.preferred_leader_site_id_
            .load(rusty::sync::atomic::Ordering::Acquire) as u16
    }

    // @unsafe - CALLER MUST HOLD mtx_.
    pub fn AmIPreferredLeader(&self) -> bool {
        raft_server_site_is_preferred_leader(
            self.site_id_, self.preferred_leader())
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
        self.core.is_leader_
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
            *term = self.core.current_term_;
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

    // ------------------------------------------------------------------
    // Applied-index publication, compaction, and the term-change log.
    // ------------------------------------------------------------------

    // CALLER MUST HOLD mtx_. The applied index never moves backward; a
    // caller that tries is a bug, so it is reported rather than obeyed.
    pub fn PublishAppliedIndexLocked(&mut self, index: u64) {
        let published: u64 = self.GetAppliedIndex();
        // [move, M3] the decision is the core's; the mirror is the shell's
        if self.core.on_applied(index, published) {
            self.appliedIndexForWait_
                .store(index, rusty::sync::atomic::Ordering::Release);
        }
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
            up_to_index, self.core.commit_index_, self.core.snapidx_);
        if safe_index != requested_index {
            rusty::raft_log_warn_5(
                "[RAFT-COMPACT] Site {}: Clamped compaction {} -> {} (core.commit_index_={}, snapidx={})",
                self.site_id_, requested_index, safe_index,
                self.core.commit_index_, self.core.snapidx_);
        }

        if !raft_server_log_index_has_successor(safe_index) {
            rusty::raft_log_error_2(
                "[RAFT-COMPACT] Site {}: Refusing terminal compaction index {}; the exclusive storage bound and min_active_slot would wrap",
                self.site_id_, safe_index);
            return 0;
        }

        let removed_memory: usize =
            self.core.raft_log_.compact_through(safe_index);

        rusty::raft_log_info_3(
            "[RAFT-COMPACT] Site {}: Compacted in-memory entries through {} (memory={})",
            self.site_id_, safe_index, removed_memory);
        removed_memory
    }

    // @unsafe - acquiring entry point, for callers that do not hold mtx_.
    pub fn CompactLog(&mut self, up_to_index: u64) -> usize {
        // [fix, F5] Inside the verified configuration the log stays whole.
        if self.verified_gates_ {
            return 0;
        }
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
        self.core.log_term_change(reason, old_term, new_term, source);  // [move, M1]
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

    // @unsafe - srpc logging.
    pub fn ElectionLoopLogStart(&self) {
        rusty::raft_log_debug_0("start timer for election");
    }

    // @unsafe - srpc logging.
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

    // @unsafe - CALLER MUST HOLD mtx_. [move, M1] The body is
    // RaftCore::rebuild_peer_tables.
    pub fn RebuildPeerTables(&mut self, next_index: u64) {
        self.core.rebuild_peer_tables(next_index);
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
        let knobs: RaftElectionTimeouts = unsafe { raft_election_timeouts() };
        let current_time: u64 = unsafe { raft_time_now_us() };
        let in_grace_period: bool =
            (current_time - self.startup_timestamp_) < knobs.grace_period_us_;
        // IsPreferredLeaderConfigured's whole body (server.cc). It is a DSL
        // function of the server.cc carrier, so this block cannot name it;
        // the predicate is one comparison and is spelled out rather than
        // bridged.
        let preferred_leader_configured: bool =
            self.preferred_leader() != RAFT_SERVER_INVALID_SITE_ID;

        if !preferred_leader_configured {
            // Traditional Raft when no preferred leader is configured.
            knobs.non_preferred_steady_us_
        } else if self.AmIPreferredLeader() {
            knobs.preferred_us_
        } else if in_grace_period {
            // The startup grace timeout is env-tunable for test stability.
            knobs.non_preferred_grace_us_
        } else {
            knobs.non_preferred_steady_us_
        }
    }

    // CALLER MUST HOLD mtx_. Samples exactly one timeout and advances the
    // generation, so a concurrent heartbeat reset cannot leave a campaign
    // running off an expired snapshot.
    pub fn resetTimerLocked(&mut self, reason: &str) {
        // [move, M4] The clock read and the timeout sample, in the order the
        // body made them, then the core records them.
        let now: u64 = unsafe { raft_time_now_us() };
        let timeout: u64 = self.GetElectionTimeout();
        let prev_time: u64 = self.core.reset_election_timer(now, timeout);
        // Log only the resets that matter (elections, votes), never the
        // routine heartbeat ones.
        if reason == "granted vote" || reason == "start election timer" {
            rusty::raft_log_info_7(
                "[TIMER_RESET] Site {}: reset timer ({}) - prev_hb_time={} new_hb_time={} delta={} timeout={} generation={}",
                self.site_id_, reason, prev_time,
                self.core.last_heartbeat_time_,
                self.core.last_heartbeat_time_ - prev_time,
                self.core.election_timeout_us_,
                self.core.election_timer_generation_);
        }
    }

    // @unsafe - acquiring entry point, for callers that do not hold mtx_.
    pub fn resetTimer(&mut self, reason: &str) {
        let _lock = RaftLockGuard::new(&mut self.mtx_);
        self.resetTimerLocked(reason);
    }

    // [move, M3] setIsLeader's decisions are RaftCore::set_is_leader; what
    // they ask for is carried out here.
    //
    // The actions of one critical section that run under mtx_, in push
    // order. CALLER MUST HOLD mtx_. A ROLE_SET that is a transition queues
    // its leader-change notice here, under mtx_, so notices are queued in
    // transition order ([fix, F6]); run_unlocked_actions fires them.
    pub fn run_locked_actions(&mut self, out: &CoreOutput) {
        let mut i: usize = 0;
        while i < out.len() {
            let action: &CoreAction = out.at(i);
            let kind: CoreActionKind = action.kind();
            if kind == CoreActionKind::APPLY_RANGE {
                self.EnqueueCommittedEntries(action.from(), action.to());
            } else if kind == CoreActionKind::RESET_ELECTION {
                let reason: TimerResetReason = action.reason();
                if reason == TimerResetReason::BECAME_FOLLOWER {
                    self.resetTimerLocked("became follower");
                    rusty::raft_log_info_2(
                        "[RAFT_TIMER] Site {} reset election timer when becoming follower (last_hb now={})",
                        self.site_id_, self.core.last_heartbeat_time_);
                } else if reason == TimerResetReason::STEP_DOWN {
                    self.resetTimerLocked("stepDown");
                } else if reason == TimerResetReason::GRANTED_VOTE {
                    self.resetTimerLocked("granted vote");
                } else if reason == TimerResetReason::STARTING_CAMPAIGN {
                    self.resetTimerLocked("starting election campaign");
                } else {
                    self.resetTimerLocked("AppendEntries from current-term leader");
                }
            } else if kind == CoreActionKind::APPEND_NOOP {
                self.AppendLeaderNoop();
            } else if kind == CoreActionKind::ROLE_SET
                && (action.became_leader() || action.became_follower())
            {
                // The slot is asked under the queue's lock, which
                // RegisterLeaderChangeCallback takes to write it.
                let mut notices = self.leader_notices_.lock().unwrap();
                if unsafe {
                    raft_leader_change_cb_is_set(
                        &self.leader_change_cb_ as *const rusty::RaftLeaderChangeCb)
                } {
                    notices.pending_.push_back(action.became_leader());
                }
            }
            i += 1;
        }
        self.publish_mirrors();  // [fix, F8]
    }

    // [fix, F6] What a critical section leaves for after mtx_ is released:
    // each role setting's log entry, in push order, then the queued
    // leader-change notices. CALLER MUST NOT HOLD mtx_.
    pub fn run_unlocked_actions(&mut self, out: &CoreOutput) {
        let mut transitioned: bool = false;
        let mut i: usize = 0;
        while i < out.len() {
            let action: &CoreAction = out.at(i);
            if action.kind() == CoreActionKind::ROLE_SET {
                unsafe {
                    raft_log_set_is_leader_entry(self.site_id_, self.loc_id_,
                                                 action.term(),
                                                 action.prev_is_leader(),
                                                 action.is_leader());
                }
                transitioned = transitioned || action.became_leader()
                    || action.became_follower();
            }
            i += 1;
        }
        // Only a section that queued a notice drains: a notice is fired by
        // the thread that queued it or by the drainer already running, so a
        // section with no transition (most AppendEntries) skips the lock.
        if transitioned {
            self.fire_leader_notices();
        }
    }

    // [fix, F6] Fires the queued leader-change notices in transition order,
    // each once, with no lock held while a callback runs: whichever thread
    // finds the queue idle drains it, taking the queue's lock only to pop.
    // A callback may take mtx_; one that causes another transition only
    // queues its notice, and this loop fires it next.
    fn fire_leader_notices(&mut self) {
        {
            let mut notices = self.leader_notices_.lock().unwrap();
            if notices.draining_ || notices.pending_.is_empty() {
                return;
            }
            notices.draining_ = true;
        }
        let mut more: bool = true;
        while more {
            let mut have: bool = false;
            let mut became_leader: bool = false;
            // A copy of the callback, made under the queue's lock (which
            // registration takes) and called with no lock held.
            let mut cb: rusty::RaftLeaderChangeCb = Default::default();
            {
                let mut notices = self.leader_notices_.lock().unwrap();
                if notices.pending_.is_empty() {
                    notices.draining_ = false;
                } else {
                    became_leader = notices.pending_.pop_front().unwrap();
                    unsafe {
                        raft_leader_change_cb_clone_into(
                            &self.leader_change_cb_ as *const rusty::RaftLeaderChangeCb,
                            &mut cb as *mut rusty::RaftLeaderChangeCb);
                    }
                    have = true;
                }
            }
            if !have {
                more = false;
            } else {
                if became_leader {
                    rusty::raft_log_info_1(
                        "[LEADER_CALLBACK] Site {}: Firing leader_change_cb_(true) - became leader",
                        self.site_id_);
                } else {
                    rusty::raft_log_info_1(
                        "[LEADER_CALLBACK] Site {}: Firing leader_change_cb_(false) - became follower",
                        self.site_id_);
                }
                unsafe {
                    raft_fire_leader_change(
                        &cb as *const rusty::RaftLeaderChangeCb, became_leader)
                };
            }
        }
    }

    // [fix, F8] CALLER MUST HOLD mtx_. Publishes the fields other threads
    // read without the lock. Called at the end of run_locked_actions, which
    // every critical section that runs a core decision ends with, and after
    // the shell's own writes of these fields (snapshot install and
    // recovery).
    pub fn publish_mirrors(&self) {
        self.commit_index_mirror_
            .store(self.core.commit_index_, rusty::sync::atomic::Ordering::Release);
        self.is_leader_mirror_
            .store(self.core.is_leader_, rusty::sync::atomic::Ordering::Release);
        let hint: u16 = if self.core.is_leader_ {
            self.site_id_
        } else {
            self.core.current_leader_id_
        };
        self.leader_hint_mirror_
            .store(hint as u64, rusty::sync::atomic::Ordering::Release);
        self.term_mirror_
            .store(self.core.current_term_, rusty::sync::atomic::Ordering::Release);
    }

    // @safe - stop_ as setIsLeader read it, for the core's `stopped`.
    pub fn stopped_now(&self) -> bool {
        self.stop_.load(rusty::sync::atomic::Ordering::Acquire)
    }

    // ------------------------------------------------------------------
    // Loop prologue, startup, shutdown, submission and the apply queue.
    // ------------------------------------------------------------------

    // @safe - the site id at an ordinal of the peer table.
    pub fn peer_site_at(&self, ordinal: usize) -> u16 {
        self.core.peer_site_at(ordinal)  // [move, M1]
    }

    // A whole non-negative decimal that fits in u64, parsed in Rust.
    //
    //   Ok(None)     the variable is unset or empty
    //   Ok(Some(v))  it parsed
    //   Err(())      it is present and is NOT a whole u64
    //
    // This is the error handling those three readers used to express with
    // try/catch around std::stoull. Nothing here can throw: the digits are
    // walked one at a time off the raw pointer, exactly as
    // src/srpc/base/logging.rs:114 walks a C string, and the overflow test is a
    // comparison rather than an exception.
    //
    // It is also STRICTER than std::stoull, which stopped at the first non-digit
    // and returned what it had -- so "5000x" silently configured 5000. Every
    // byte must be a digit. A leading '-' is rejected rather than wrapped, which
    // std::stoull did NOT do: it accepts a sign and wraps into a huge u64.
    // manual_range_contains: `(48..=57).contains(&digit)` is the better Rust and
    // this lowers to C++, where a u8 has no `contains`. not_unsafe_ptr_arg_deref:
    // the pointer is getenv's, which outlives every caller.
    #[allow(clippy::not_unsafe_ptr_arg_deref, clippy::manual_range_contains)]
    // &self is unused and deliberate: an associated fn emits as a free
    // function at column 0, which the ODR post-pass in scripts/raft_dsl.sh
    // does not prefix with `inline` -- it only matches `Owner::method(`.
    // In a header that is a multiple-definition link error. A method emits
    // as RaftServerBase::raft_env_u64 and is inlined correctly.
    #[allow(clippy::unused_self)]
    pub fn raft_env_u64(&self, which: i32) -> Result<rusty::Option<u64>, RaftEnvError> {
        let raw: *const core::ffi::c_char = unsafe { raft_env_lookup(which) };
        if raw.is_null() {
            return Result::<rusty::Option<u64>, RaftEnvError>::Ok(rusty::None);
        }
        let mut value: u64 = 0;
        let mut index: usize = 0;
        while unsafe { *raw.add(index) } != 0 {
            let digit: u8 = unsafe { *raw.add(index) } as u8;
            if digit < 48 || digit > 57 {
                return Result::<rusty::Option<u64>, RaftEnvError>::Err(
                    RaftEnvError::NOT_A_WHOLE_NUMBER);
            }
            // Overflow checked BEFORE it happens: u64::MAX / 10, then the last
            // digit. `checked_mul`/`checked_add` would read better and lower to
            // an Option this has to unwrap anyway.
            if value > u64::MAX / 10 {
                return Result::<rusty::Option<u64>, RaftEnvError>::Err(
                    RaftEnvError::OVERFLOWS_U64);
            }
            value *= 10;
            let addend: u64 = (digit - 48) as u64;
            if value > u64::MAX - addend {
                return Result::<rusty::Option<u64>, RaftEnvError>::Err(
                    RaftEnvError::OVERFLOWS_U64);
            }
            value += addend;
            index += 1;
        }
        Result::<rusty::Option<u64>, RaftEnvError>::Ok(rusty::Some(value))
    }

    // is_some()/unwrap() rather than `if let`: the emitter renders an
    // `if let` binding with a dot where the C++ needs an arrow. Same
    // constraint as ReplicationWakeGate::wake_on_owner.
    #[allow(clippy::unnecessary_unwrap)]
    // @unsafe - the owner-thread startup job. Every failure path closes the
    // server fail-closed rather than starting half a replica.
    pub fn SetupInternal(&mut self) -> bool {
        // RPC services may already be listening when this job begins. Keep
        // every handler fail-closed until snapshot loading has completed.
        self.rpc_ready_
            .store(false, rusty::sync::atomic::Ordering::Release);

        // Record startup time for the grace-period logic.
        self.startup_timestamp_ = unsafe { raft_time_now_us() };

        let hb_env = self.raft_env_u64(RAFT_ENV_HEARTBEAT_INTERVAL_US);
        if hb_env.is_err() {
            rusty::raft_log_error_0(
                "[RAFT] MAKO_RAFT_HEARTBEAT_INTERVAL_US is not a whole u64");
            self.FailClosed();
            return false;
        }
        let hb_override = hb_env.unwrap();
        if hb_override.is_some() {
            self.heartbeat_interval_us_ = hb_override.unwrap();
            rusty::raft_log_info_1(
                "[RAFT] Heartbeat interval set to {} us from env",
                self.heartbeat_interval_us_);
        }

        if !unsafe {
            raft_bind_replication_poll(self.handle())
        }
        {
            rusty::raft_log_error_1(
                "[RAFT-WAKE] Site {} has no PollThread owner during Setup",
                self.site_id_);
            self.FailClosed();
            return false;
        }

        let lrw_env = self.raft_env_u64(RAFT_ENV_LOG_RETENTION_WINDOW);
        if lrw_env.is_err() {
            rusty::raft_log_error_0(
                "[RAFT] MAKO_RAFT_LOG_RETENTION_WINDOW is not a whole u64");
            self.FailClosed();
            return false;
        }
        let lrw_override = lrw_env.unwrap();
        if lrw_override.is_some() {
            self.log_retention_window_ =
                raft_server_retention_window_normalize(lrw_override.unwrap());
            rusty::raft_log_info_1(
                "[RAFT] Log retention window set to {} from env",
                self.log_retention_window_);
        }

        if !unsafe {
            raft_initialize_snapshot_manager(self.handle(),
                                             self.site_id_)
        } {
            rusty::raft_log_error_1(
                "[RAFT-SNAPSHOT] Site {} cannot start after snapshot recovery failure",
                self.site_id_);
            self.FailClosed();
            return false;
        }
        {
            // [fix, F8] recovery may have restored the term and commit index
            let _lock = RaftLockGuard::new(&mut self.mtx_);
            self.publish_mirrors();
        }

        let replicas: u64 = self.LoadCurrentConfig();
        rusty::raft_log_info_3(
            "[RAFT-CONFIG] Initialized current_config_ for site {} partition {} with {} replicas",
            self.site_id_, self.partition_id_, replicas);

        // [fix, F5] The configuration the verification covers
        // (docs/verus/modification-plan.md §4.1). Outside it a normal run
        // logs once and carries on; MAKO_RAFT_VERIFIED_GATES=1 fails closed.
        // The knob is parsed whatever the configuration, as the others are,
        // so a malformed value never goes unnoticed.
        let gates_env = self.raft_env_u64(RAFT_ENV_VERIFIED_GATES);
        if gates_env.is_err() {
            rusty::raft_log_error_0(
                "[RAFT] MAKO_RAFT_VERIFIED_GATES is not a whole u64");
            self.FailClosed();
            return false;
        }
        let gates = gates_env.unwrap();
        self.verified_gates_ = gates.is_some() && gates.unwrap() == 1;
        if !self.verified_config_ok() {
            if gates.is_some() && gates.unwrap() == 1 {
                rusty::raft_log_error_1(
                    "[RAFT-VERIFY] Site {} is outside the verified configuration; failing closed (MAKO_RAFT_VERIFIED_GATES=1)",
                    self.site_id_);
                self.FailClosed();
                return false;
            }
            rusty::raft_log_warn_1(
                "[RAFT-VERIFY] Site {} is outside the verified configuration (snapshots, failover or membership)",
                self.site_id_);
        }

        self.StartApplyThread();
        self.rpc_ready_
            .store(true, rusty::sync::atomic::Ordering::Release);

        // Unconditional. This was written twice, once under
        // #ifdef RAFT_TEST_CORO and once under #ifndef, with
        // CHARACTER-IDENTICAL bodies -- so it always ran, and editing one arm
        // without the other was a standing trap.
        if self.heartbeat_ {
            rusty::raft_log_debug_1("starting heartbeat loop at site {}",
                                    self.site_id_);
            self.heartbeat_loop_running_
                .store(true, rusty::sync::atomic::Ordering::Release);
            unsafe {
                raft_spawn_heartbeat_loop(self.handle());
            }
            if self.failover_ {
                self.election_loop_running_
                    .store(true, rusty::sync::atomic::Ordering::Release);
                unsafe {
                    raft_spawn_election_timer_fiber(
                        self.handle());
                }
            }
        }
        true
    }

    // @safe - the fail-stop a snapshot recovery failure performs, with the
    // reason. Was a lambda inside InitializeSnapshotManager.
    pub fn FailSnapshotRecovery(&mut self, reason: &str) -> bool {
        rusty::raft_log_error_2(
            "[RAFT-SNAPSHOT] Site {} recovery failed: {}", self.site_id_,
            reason);
        self.FailStop();
        false
    }

    // @safe - the two shapes of recovered progress that no snapshot covers.
    // Both mean the live log was compacted by a snapshot this manager does
    // not have.
    pub fn HasUncoveredProgress(&self) -> bool {
        let orphaned_compacted_suffix: bool = self.core.snapidx_ == 0
            && !self.core.raft_log_.is_empty()
            && self.core.raft_log_.base() > 1;
        let uncovered_empty_progress: bool = self.core.snapidx_ == 0
            && self.core.raft_log_.is_empty()
            && self.core.commit_index_ != 0;
        orphaned_compacted_suffix || uncovered_empty_progress
    }

    // @unsafe - CALLER HOLDS NOTHING; takes the apply gate and mtx_ in that
    // order. Wrapped by RaftServer::InitializeSnapshotManager's catch-all.
    //
    // Restores the exact state-machine bytes before publishing any recovered
    // snapshot boundary, so a failure anywhere leaves the live state machine,
    // the latest Raft snapshot and the reconstruction log untouched.
    // is_some()/unwrap() rather than `if let`: the emitter renders an
    // `if let` binding with a dot where the C++ needs an arrow. Same
    // constraint as ReplicationWakeGate::wake_on_owner.
    #[allow(clippy::unnecessary_unwrap)]
    pub fn InitializeSnapshotManagerLocked(&mut self) -> bool {
        if !unsafe { raft_env_snapshots_enabled() } {
            let _lock = RaftLockGuard::new(&mut self.mtx_);
            if self.HasUncoveredProgress() {
                let first: u64 = if self.core.raft_log_.is_empty() {
                    0
                } else {
                    self.core.raft_log_.base()
                };
                rusty::raft_log_error_3(
                    "[RAFT-SNAPSHOT] Site {} has recovered progress without its covering snapshot (first={} commit={}); snapshots are disabled",
                    self.site_id_, first, self.core.commit_index_);
                self.FailStop();
                return false;
            }
            rusty::raft_log_info_1(
                "[RAFT-SNAPSHOT] Snapshots disabled for site {} (set MAKO_RAFT_SNAPSHOTS=1 to enable)",
                self.site_id_);
            return true;
        }

        let mut snapshot_interval: u64 = self.GetSnapshotThreshold();
        let interval_env = self.raft_env_u64(RAFT_ENV_SNAPSHOT_INTERVAL);
        if interval_env.is_err() {
            rusty::raft_log_error_0(
                "[RAFT-SNAPSHOT] MAKO_RAFT_SNAPSHOT_INTERVAL is not a whole u64");
            return false;
        }
        let interval_override = interval_env.unwrap();
        if interval_override.is_some() {
            snapshot_interval = interval_override.unwrap();
            self.SetSnapshotThreshold(snapshot_interval);
        }

        let _apply_lock =
            RaftStdLockGuard::new(&mut self.state_machine_apply_mtx_);
        let _lock = RaftLockGuard::new(&mut self.mtx_);

        let mut manager: rusty::RaftSnapshotManagerPtr = Default::default();
        unsafe {
            raft_snapshot_recovery_pick_manager(
                &self.snapshot_manager_ as *const rusty::RaftSnapshotManagerPtr,
                &mut manager as *mut rusty::RaftSnapshotManagerPtr);
        }

        let mut discovered_index: u64 = 0;
        let mut discovered_term: u64 = 0;
        let has_latest: bool = unsafe {
            raft_snapshot_manager_latest(
                &manager as *const rusty::RaftSnapshotManagerPtr,
                &mut discovered_index as *mut u64,
                &mut discovered_term as *mut u64)
        };
        if !has_latest {
            if self.core.snapidx_ != 0 || self.HasUncoveredProgress() {
                return self.FailSnapshotRecovery(
                    "empty snapshot manager cannot cover the compacted live log");
            }
            self.snapshot_manager_ = manager;
            self.snapshot_manager_configured_
                .store(true, rusty::sync::atomic::Ordering::Release);
            rusty::raft_log_info_3(
                "[RAFT-SNAPSHOT] Initialized empty in-memory manager for site {} partition {}: interval={}",
                self.site_id_, self.partition_id_, snapshot_interval);
            return true;
        }

        let mut snapshot_data: rusty::RaftByteString = Default::default();
        let mut recovered_snapshot_index: u64 = 0;
        let mut recovered_snapshot_term: u64 = 0;
        let mut snapshot_size_bytes: u64 = 0;
        let loaded: bool = unsafe {
            raft_snapshot_manager_load(
                &manager as *const rusty::RaftSnapshotManagerPtr,
                &mut snapshot_data as *mut rusty::RaftByteString,
                &mut recovered_snapshot_index as *mut u64,
                &mut recovered_snapshot_term as *mut u64,
                &mut snapshot_size_bytes as *mut u64)
        };
        if !loaded {
            return self.FailSnapshotRecovery(
                "latest snapshot bytes failed to load");
        }
        if recovered_snapshot_index != discovered_index
            || recovered_snapshot_term != discovered_term
        {
            return self.FailSnapshotRecovery(
                "snapshot manager metadata does not match its loaded snapshot");
        }
        if recovered_snapshot_index == 0
            || !raft_server_log_index_has_successor(recovered_snapshot_index)
        {
            return self.FailSnapshotRecovery(
                "snapshot boundary is outside the recoverable log range");
        }
        if recovered_snapshot_index < self.core.snapidx_
            || (recovered_snapshot_index == self.core.snapidx_
                && self.core.snapidx_ != 0
                && recovered_snapshot_term != self.core.snapterm_ as u64)
        {
            return self.FailSnapshotRecovery(
                "snapshot manager would move the live boundary backward or change its term");
        }
        let has_prepare_cb: bool =
            unsafe {
                raft_prepare_snapshot_cb_is_set(
                    &self.prepare_sm_snapshot_cb_
                        as *const rusty::RaftPrepareSnapshotCb)
            };
        if has_prepare_cb && self.GetAppliedIndex() > recovered_snapshot_index
        {
            return self.FailSnapshotRecovery(
                "refusing to rewind a live state machine to an older snapshot");
        }

        let previous_snapshot_index: u64 = self.core.snapidx_;
        let previous_snapshot_term: u64 = self.core.snapterm_ as u64;
        let previous_last_log_index: u64 = self.core.raft_log_.last_index();
        let previous_min_active_slot: u64 = self.core.raft_log_.base();

        // Reconstruct Figure 13's suffix decision from the old boundary when
        // it is still present. A live reinitialisation would use its exact
        // existing snapshot tuple as the same proof; that proof is
        // unreachable today because Setup is the only caller and snapidx_ is
        // still 0 there.
        let boundary = self.core.raft_log_.get(recovered_snapshot_index);
        #[allow(clippy::unnecessary_unwrap)]
        let has_boundary: bool = boundary.is_some()
            && boundary.unwrap().has_value();  // [move, M6]
        let local_boundary_term: u64 = if has_boundary {
            boundary.unwrap().term() as u64
        } else {
            0
        };
        let boundary_matches: bool = raft_server_snapshot_boundary_matches(
            has_boundary, local_boundary_term, recovered_snapshot_term);
        let has_recovered_suffix: bool = raft_server_log_index_above(
            previous_last_log_index, recovered_snapshot_index);
        let live_snapshot_proves_suffix: bool =
            previous_snapshot_index == recovered_snapshot_index
                && previous_snapshot_term == recovered_snapshot_term
                && previous_min_active_slot == recovered_snapshot_index + 1;
        let retain_suffix: bool =
            raft_server_snapshot_recovery_retains_suffix(
                has_recovered_suffix, has_boundary, boundary_matches,
                live_snapshot_proves_suffix);

        if raft_server_snapshot_recovery_has_unproven_gap(
            has_recovered_suffix, has_boundary, live_snapshot_proves_suffix)
        {
            return self.FailSnapshotRecovery(
                "recovered suffix has no snapshot boundary or live-snapshot proof");
        }
        if has_recovered_suffix && has_boundary && !boundary_matches {
            rusty::raft_log_warn_5(
                "[RAFT-SNAPSHOT] Site {} discarding recovered suffix after snapshot boundary term mismatch: local=({}, {}) snapshot=({}, {})",
                self.site_id_, recovered_snapshot_index, local_boundary_term,
                recovered_snapshot_index, recovered_snapshot_term);
        }

        if !unsafe {
            raft_load_state_machine_snapshot(
                &self.prepare_sm_snapshot_cb_
                    as *const rusty::RaftPrepareSnapshotCb,
                self.site_id_,
                &snapshot_data as *const rusty::RaftByteString,
                recovered_snapshot_index, recovered_snapshot_term)
        } {
            return self.FailSnapshotRecovery(
                "state-machine snapshot validation/load failed");
        }

        self.core.snapidx_ = recovered_snapshot_index;
        self.core.snapterm_ = recovered_snapshot_term as i64;
        if retain_suffix {
            self.core.raft_log_.compact_through(self.core.snapidx_);
        } else {
            self.core.raft_log_.reset(self.core.snapidx_ + 1);
        }
        self.core.commit_index_ = raft_server_snapshot_progress_clamp(
            self.core.commit_index_, self.core.snapidx_,
            self.core.raft_log_.last_index());

        if self.core.current_term_ < self.core.snapterm_ as u64 {
            rusty::raft_log_warn_3(
                "[RAFT-SNAPSHOT] Site {} advancing recovered term {} -> {} to cover snapshot boundary",
                self.site_id_, self.core.current_term_,
                self.core.snapterm_);
            self.core.current_term_ = self.core.snapterm_ as u64;
            self.core.vote_for_ = RAFT_SERVER_INVALID_SITE_ID;
        }

        assert!(
            self.core.commit_index_
                <= self.core.raft_log_.last_index());  // [move, M10]

        self.snapshot_manager_ = manager;
        self.snapshot_manager_configured_
            .store(true, rusty::sync::atomic::Ordering::Release);
        self.snapshot_trigger_index_
            .store(self.core.snapidx_,
                   rusty::sync::atomic::Ordering::Release);

        if self.core.snapidx_ > self.GetAppliedIndex() {
            self.PublishAppliedIndexLocked(self.core.snapidx_);
        }

        rusty::raft_log_info_8(
            "[RAFT-SNAPSHOT] Restored snapshot for site {}: index={} term={} size={} commit={} last={} min_active={} retain_suffix={}",
            self.site_id_, self.core.snapidx_, self.core.snapterm_,
            snapshot_size_bytes, self.core.commit_index_,
            self.core.raft_log_.last_index(),
            self.core.raft_log_.base(), retain_suffix);
        rusty::raft_log_info_3(
            "[RAFT-SNAPSHOT] Initialized for site {} partition {}: interval={}",
            self.site_id_, self.partition_id_, snapshot_interval);
        true
    }

    // @unsafe - an InstallSnapshot reply that carried a usable term. The
    // caller (the RPC completion lambda in server.cc) has already taken the
    // async-callback lifetime lock and confirmed the reply is available;
    // it does NOT hold mtx_, which this takes, and that ordering is what
    // keeps the inline-completion path from self-deadlocking.
    pub fn InstallSnapshotReplyAccepted(&mut self, site_id: u16, ord: usize,
                                        snap_last_idx: u64, send_term: u64,
                                        follower_term: u64) {
        // [move, M3] The decision under mtx_, its locked actions before the
        // guard drops, the leader-change callback after ([fix, F6]).
        let mut out: CoreOutput = CoreOutput::new();
        {
            let _lock = RaftLockGuard::new(&mut self.mtx_);
            self.InstallSnapshotReplyAcceptedLocked(site_id, ord, snap_last_idx,
                                                    send_term, follower_term,
                                                    &mut out);
            self.run_locked_actions(&out);
        }
        self.run_unlocked_actions(&out);
    }

    // [move, M3] InstallSnapshotReplyAccepted's body. CALLER MUST HOLD mtx_.
    #[allow(clippy::too_many_arguments)]
    pub fn InstallSnapshotReplyAcceptedLocked(&mut self, site_id: u16,
                                              ord: usize, snap_last_idx: u64,
                                              send_term: u64,
                                              follower_term: u64,
                                              out: &mut CoreOutput) {
        if raft_server_observed_higher_term(follower_term,
                                            self.core.current_term_) {
            rusty::raft_log_info_4(
                "[HEARTBEAT-SNAPSHOT] Site {}: Follower {} has higher term {} > {}, stepping down",
                self.site_id_, site_id, follower_term,
                self.core.current_term_);
            let previous_term: u64 = self.core.current_term_;
            self.core.current_term_ = follower_term;
            self.core.vote_for_ = RAFT_SERVER_INVALID_SITE_ID;
            self.LogTermChange("InstallSnapshot reply carried newer term",
                               previous_term, self.core.current_term_,
                               site_id);
            // A follower's higher term does not identify the leader of that
            // term. Retire the previous leader hint before publishing
            // follower state.
            self.core.current_leader_id_ =
                raft_server_leader_hint_after_transition(
                    false, false, self.site_id_, site_id);
            let stopped: bool = self.stopped_now();
            self.core.step_down(stopped, self.failover_, out);  // [move, M3]
            self.core.req_voting_ = false;
            self.core.election_in_progress_ = false;
            return;
        }
        if self.core.current_term_ != send_term {
            rusty::raft_log_info_1(
                "[HEARTBEAT-SNAPSHOT] Site {}: Term changed since snapshot send, ignoring response",
                self.site_id_);
            return;
        }
        let has_successor: bool =
            raft_server_log_index_has_successor(snap_last_idx);
        let next_index: u64 = if has_successor {
            raft_server_follower_next_index(snap_last_idx)
        } else {
            snap_last_idx
        };
        self.core.peers_.accept_through(ord, snap_last_idx, has_successor,
                                          next_index);
        rusty::raft_log_info_4(
            "[HEARTBEAT-SNAPSHOT] Site {}: Updated follower {}: next_index={} match_index={}",
            self.site_id_, site_id, self.core.peers_.next_index(ord),
            self.core.peers_.match_index(ord));
    }

    // @safe - the fail-stop every unrecoverable snapshot path performs.
    pub fn FailStop(&mut self) {
        self.rpc_ready_
            .store(false, rusty::sync::atomic::Ordering::Release);
        self.stop_
            .store(true, rusty::sync::atomic::Ordering::Release);
        self.looping_
            .store(false, rusty::sync::atomic::Ordering::Release);
        self.apply_thread_running_
            .store(false, rusty::sync::atomic::Ordering::SeqCst);
    }

    // @unsafe - installs a leader's snapshot. CALLER HOLDS
    // state_machine_apply_mtx_ then mtx_, and wraps this in the catch-all
    // that RaftServer::OnInstallSnapshot still owns: snapshot replacement
    // must not overlap entry application or recovery replay, and the global
    // order is apply gate -> Raft state -> queue.
    //
    // *term_out is the reply. Zero means "unavailable": the leader's
    // callback leaves match_index/next_index untouched on zero, so every
    // individual install failure writes it back to zero after the accepted
    // leader contact has already set it.
    #[allow(clippy::too_many_arguments, clippy::not_unsafe_ptr_arg_deref)]
    // term_out is a raw pointer end to end: the RPC service's out-parameter
    // arrives through the C ABI and the kernel as a pointer, and an `&mut`
    // here would only be rebuilt from it at the boundary.
    #[allow(clippy::not_unsafe_ptr_arg_deref)]
    pub fn OnInstallSnapshotLocked(&mut self, term: u64, leader_id: u64,
                                   last_included_index: u64,
                                   last_included_term: u64,
                                   data: *const rusty::RaftByteString,
                                   term_out: *mut u64) {
        unsafe {
            *term_out = 0;
        }

        // Edge case 0: the server is shutting down.
        if self.stop_.load(rusty::sync::atomic::Ordering::Acquire) {
            rusty::raft_log_info_1(
                "[INSTALL-SNAPSHOT] Site {}: Ignoring InstallSnapshot - server shutting down",
                self.site_id_);
            return;
        }

        // Edge case 1: a stale term is rejected.
        if term < self.core.current_term_ {
            rusty::raft_log_info_4(
                "[INSTALL-SNAPSHOT] Site {}: Rejecting InstallSnapshot from leader {} (leader_term={} < my_term={})",
                self.site_id_, leader_id, term, self.core.current_term_);
            unsafe {
                *term_out = self.core.current_term_;
            }
            return;
        }

        // A leader cannot have committed an entry from a term that has not
        // happened yet. That is a malformed boundary, not usable leader
        // evidence: reject it with the unavailable sentinel BEFORE
        // authenticating the sender, stepping down, resetting the timer, or
        // touching payload state.
        if !raft_server_snapshot_term_is_valid(last_included_term, term) {
            rusty::raft_log_error_4(
                "[INSTALL-SNAPSHOT] Site {}: Rejecting impossible snapshot boundary term {} from leader {} in term {}",
                self.site_id_, last_included_term, leader_id, term);
            return;
        }

        if leader_id > RAFT_SERVER_INVALID_SITE_ID as u64 {
            rusty::raft_log_warn_3(
                "[INSTALL-SNAPSHOT] Site {} rejected unrepresentable leader identity {} in term {}",
                self.site_id_, leader_id, term);
            return;
        }
        let leader_site: u16 = leader_id as u16;
        let sender_is_current_voter: bool =
            leader_site != RAFT_SERVER_INVALID_SITE_ID
                && leader_site != self.site_id_
                && self.IsConfigMember(leader_site);
        let leader_has_higher_term: bool =
            raft_server_observed_higher_term(term, self.core.current_term_);
        let sender_is_self: bool = leader_site == self.site_id_;
        let has_known_leader: bool =
            self.core.current_leader_id_ != RAFT_SERVER_INVALID_SITE_ID;
        let known_leader_matches_sender: bool =
            self.core.current_leader_id_ == leader_site;
        if !sender_is_current_voter
            || !raft_server_leader_rpc_sender_is_authoritative(
                leader_has_higher_term, self.core.is_leader_,
                sender_is_self, has_known_leader,
                known_leader_matches_sender)
        {
            rusty::raft_log_warn_7(
                "[INSTALL-SNAPSHOT] Site {} rejected unauthoritative leader {} in term {} (local_term={} leader={} known_leader={} voter={})",
                self.site_id_, leader_id, term, self.core.current_term_,
                self.core.is_leader_, self.core.current_leader_id_,
                sender_is_current_voter);
            return;
        }

        // Edge case 2: a higher or equal term is accepted as a legitimate
        // leader.
        let previous_term: u64 = self.core.current_term_;
        if leader_has_higher_term {
            rusty::raft_log_info_4(
                "[INSTALL-SNAPSHOT] Site {}: Leader {} has higher term ({} > {}) - updating",
                self.site_id_, leader_id, term, self.core.current_term_);
            self.core.current_term_ = term;
            self.core.vote_for_ = RAFT_SERVER_INVALID_SITE_ID;
        }

        // InstallSnapshot comes from a known leader. Publish its identity
        // before a possible leader-to-follower callback observes the role
        // transition.
        self.core.current_leader_id_ =
            raft_server_leader_hint_after_transition(false, true,
                                                     self.site_id_,
                                                     leader_site);

        // Any accepted leader RPC, including one in our current term,
        // establishes follower state. Cancel the outstanding election as
        // well as leadership; RequestVote's delayed-success path
        // revalidates this ownership before it can promote again.
        // [move, M3] The locked actions run here, where stepDown and
        // setIsLeader ran them, so the timer resets keep their place ahead of
        // the install below; the leader-change callback waits in install_out_
        // until OnInstallSnapshot has released mtx_ ([fix, F6]).
        let mut out: CoreOutput = CoreOutput::new();
        let stopped: bool = self.stopped_now();
        if self.core.is_leader_ {
            self.core.step_down(stopped, self.failover_, &mut out);
        } else {
            self.core.set_is_leader(false, stopped, self.failover_, &mut out);
        }
        self.run_locked_actions(&out);
        self.install_out_ = out;
        self.core.req_voting_ = false;
        self.core.election_in_progress_ = false;

        if leader_has_higher_term {
            self.LogTermChange("InstallSnapshot carried newer term",
                               previous_term, self.core.current_term_,
                               leader_site);
        }

        // Legitimate leader contact.
        self.resetTimerLocked("received InstallSnapshot");
        // From here current_term_ denotes an accepted current-term leader
        // contact; individual install failures overwrite it with zero.
        unsafe {
            *term_out = self.core.current_term_;
        }

        // A current-term leader may retry a snapshot after this follower has
        // already committed, applied or snapshotted through its boundary.
        // Acknowledge the contact, but roll no local state backward and do
        // not install the stale payload.
        let mut local_progress_index: u64 = self.core.commit_index_;
        if self.core.execute_index_ > local_progress_index {
            local_progress_index = self.core.execute_index_;
        }
        let applied: u64 = self.GetAppliedIndex();
        if applied > local_progress_index {
            local_progress_index = applied;
        }
        if self.core.snapidx_ > local_progress_index {
            local_progress_index = self.core.snapidx_;
        }
        if last_included_index == self.core.snapidx_
            && self.core.snapidx_ != 0
            && last_included_term != self.core.snapterm_ as u64
        {
            rusty::raft_log_error_5(
                "[INSTALL-SNAPSHOT] Site {}: rejecting snapshot boundary ({}, {}) that conflicts with local snapshot ({}, {})",
                self.site_id_, last_included_index, last_included_term,
                self.core.snapidx_, self.core.snapterm_);
            unsafe {
                *term_out = 0;
            }
            return;
        }
        if raft_server_snapshot_is_stale(last_included_index,
                                         local_progress_index) {
            rusty::raft_log_info_6(
                "[INSTALL-SNAPSHOT] Site {}: Snapshot index {} is already covered (commit={} execute={} applied={} snapidx={}); acknowledging no-op",
                self.site_id_, last_included_index,
                self.core.commit_index_, self.core.execute_index_,
                self.GetAppliedIndex(), self.core.snapidx_);
            return;
        }
        if !raft_server_log_index_has_successor(last_included_index) {
            // The log's base requires S + 1. Reaching UINT64_MAX exhausts
            // the index space, so reject rather than wrap.
            rusty::raft_log_error_2(
                "[INSTALL-SNAPSHOT] Site {}: Cannot install terminal snapshot index {}; no successor index is representable",
                self.site_id_, last_included_index);
            unsafe {
                *term_out = 0;
            }
            return;
        }

        let configured: bool =
            unsafe { raft_snapshot_manager_is_set(&self.snapshot_manager_) };
        if !configured {
            rusty::raft_log_error_2(
                "[INSTALL-SNAPSHOT] Site {}: Cannot install snapshot at index {} without configured snapshot storage",
                self.site_id_, last_included_index);
            unsafe {
                *term_out = 0;
            }
            return;
        }

        // Complete every fallible observation the retention decision uses
        // BEFORE the application loader can replace external state. A
        // decoded command is required, so a synthesized empty RaftEntry can
        // never prove the snapshot boundary.
        let boundary = self.core.raft_log_.get(last_included_index);
        #[allow(clippy::unnecessary_unwrap)]
        let has_boundary: bool = boundary.is_some()
            && boundary.unwrap().has_value();  // [move, M6]
        // The entry's term is ballot_t (int64_t) and the predicate takes
        // u64; the C++ converted implicitly at the call.
        let local_boundary_term: u64 = if has_boundary {
            boundary.unwrap().term() as u64
        } else {
            0
        };
        let retain_suffix: bool = raft_server_snapshot_boundary_matches(
            has_boundary, local_boundary_term, last_included_term);

        let install: i32 = unsafe {
            raft_install_snapshot_payload(
                &self.prepare_sm_snapshot_cb_
                    as *const rusty::RaftPrepareSnapshotCb,
                &self.snapshot_manager_ as *const rusty::RaftSnapshotManagerPtr,
                self.site_id_, last_included_index, last_included_term, data)
        };
        if install == 2 {
            self.FailStop();
            unsafe {
                *term_out = 0;
            }
            return;
        }
        if install != 3 {
            unsafe {
                *term_out = 0;
            }
            return;
        }

        self.core.snapidx_ = last_included_index;
        self.core.snapterm_ = last_included_term as i64;
        self.snapshot_trigger_index_
            .store(self.core.snapidx_,
                   rusty::sync::atomic::Ordering::Release);

        // Reconcile the in-memory log and the queued application work.
        if retain_suffix {
            self.core.raft_log_.compact_through(last_included_index);
        } else {
            self.core.raft_log_.reset(last_included_index + 1);
        }
        let mut purged_apply_entries: u64 = 0;
        {
            let mut queue = self.apply_queue_.lock().unwrap();
            if retain_suffix {
                // Rotate-filter: pop every entry once and push the survivors
                // back, which keeps their order without a second container.
                let examined: usize = queue.entries_.len();
                let mut seen: usize = 0;
                while seen < examined {
                    let entry = queue.entries_.pop_front().unwrap();
                    if raft_server_log_index_at_or_below(
                        entry.index_, last_included_index) {
                        purged_apply_entries += 1;
                    } else {
                        queue.entries_.push_back(entry);
                    }
                    seen += 1;
                }
            } else {
                // A conflicting snapshot invalidates the whole queue,
                // including an entry the apply thread has already popped --
                // it rechecks this epoch under the state-machine gate before
                // invoking the callback.
                queue.epoch_ += 1;
                purged_apply_entries = queue.entries_.len() as u64;
                queue.entries_.clear();
            }
        }

        self.core.commit_index_ = last_included_index;
        assert!(
            self.core.commit_index_
                <= self.core.raft_log_.last_index());  // [move, M10]

        // Publish application only after the state machine has finished
        // loading. Acquire waiters must never observe the covered indices
        // early.
        self.PublishAppliedIndexLocked(last_included_index);

        rusty::raft_log_info_9(
            "[INSTALL-SNAPSHOT] Site {}: Installed snapshot from leader {} (snapidx={}, snapterm={}, core.commit_index_={}, core.execute_index_={}, core.raft_log_.last_index()={}, retain_suffix={}, purged_apply={})",
            self.site_id_, leader_id, self.core.snapidx_,
            self.core.snapterm_, self.core.commit_index_,
            self.core.execute_index_, self.core.raft_log_.last_index(),
            retain_suffix, purged_apply_entries);
    }


    // @unsafe - starts the background apply thread. The flag is Rust; only
    // the std::thread construction is a kernel, and the thread stays
    // JOINABLE so Shutdown can await it. Detaching causes use-after-free:
    // the thread holds the server and keeps pulling from the apply queue
    // after the server is destroyed, which shows up as an empty
    // std::function invocation.
    pub fn StartApplyThread(&mut self) {
        self.apply_thread_running_
            .store(true, rusty::sync::atomic::Ordering::SeqCst);
        unsafe {
            let thread: *mut rusty::RaftStdThread =
                &mut self.apply_thread_ as *mut rusty::RaftStdThread;
            raft_spawn_apply_thread(self.handle(), thread);
        }
    }

    // @unsafe - everything RaftServer's destructor used to do. Idempotent for
    // a server that was never started and for one whose caller already ran
    // PrepareForShutdown; a live server must be prepared on a reactor fiber
    // before this runs.
    pub fn Shutdown(&mut self) {
        self.stop_.store(true, rusty::sync::atomic::Ordering::Release);
        self.looping_
            .store(false, rusty::sync::atomic::Ordering::Release);
        self.CloseReplicationWakeGate();
        assert!(!self
            .heartbeat_loop_running_
            .load(rusty::sync::atomic::Ordering::Acquire));  // [move, M10]
        assert!(!self
            .election_loop_running_
            .load(rusty::sync::atomic::Ordering::Acquire));  // [move, M10]
        unsafe {
            raft_clear_async_callback_owner(
                &self.async_callback_lifetime_
                    as *const rusty::RaftAsyncCallbackLifetimePtr);
        }

        // Stop and join the apply thread if it was started. The thread holds
        // the server and walks the apply queue and app_next_, so it must
        // finish before any member state is destroyed.
        self.apply_thread_running_
            .store(false, rusty::sync::atomic::Ordering::SeqCst);
        unsafe {
            raft_apply_thread_join(
                &mut self.apply_thread_ as *mut rusty::RaftStdThread);
        }

        rusty::raft_log_info_5(
            "site par {}, loc {}: prepare {}, accept {}, commit {}",
            self.partition_id_, self.loc_id_, self.n_prepare_,
            self.n_accept_, self.n_commit_);
    }

    // @unsafe - the background apply thread's loop. Runs on the std::thread
    // StartApplyThread spawns.
    // `%`, an explicit max and an explicit clamp rather than their idiomatic
    // Rust forms: this lowers to C++, where uint64_t has no such members.
    #[allow(clippy::manual_is_multiple_of, clippy::implicit_saturating_sub,
            clippy::manual_clamp)]
    pub fn ApplyThreadLoop(&mut self) {
        rusty::raft_log_info_1(
            "[APPLY-THREAD] Site {}: Started background apply thread",
            self.site_id_);
        let mut apply_count: u64 = 0;
        let mut last_log_time: u64 = unsafe { raft_monotonic_now_secs() };
        while !self.stop_.load(rusty::sync::atomic::Ordering::Acquire)
            && self
                .apply_thread_running_
                .load(rusty::sync::atomic::Ordering::SeqCst)
        {
            let mut id: u64 = 0;
            let mut entry_epoch: u64 = 0;
            let mut got_entry: bool = false;
            // The size is sampled BEFORE the pop, as the kernel did, so the
            // "queue_remaining" figure below still counts the entry that is
            // about to be applied.
            let queue_size: u64 = {
                let mut queue = self.apply_queue_.lock().unwrap();
                let size_before: u64 = queue.entries_.len() as u64;
                if !queue.entries_.is_empty() {
                    let mut entry = queue.entries_.pop_front().unwrap();
                    id = entry.index_;
                    entry_epoch = entry.epoch_;
                    unsafe { raft_trace_at(10, id, 0) };  // [M0] trace kit
                    // mem::take rather than the plainer partial move
                    // `= entry.command_`: the emitter renders a binding it
                    // sees no whole-value move out of as `const auto`, and a
                    // const source turns the assignment into a Command COPY
                    // -- an Arc refcount pair per applied entry that the
                    // std::move in the kernel this replaces did not pay.
                    self.pending_apply_command_ =
                        core::mem::take(&mut entry.command_);
                    got_entry = true;
                }
                size_before
            };

            if !got_entry {
                // Periodic heartbeat while the queue is empty.
                let now_secs: u64 = unsafe { raft_monotonic_now_secs() };
                if now_secs - last_log_time >= 5 {
                    let commit_index_snapshot: u64 = {
                        let _lock = RaftLockGuard::new(&mut self.mtx_);
                        self.core.commit_index_
                    };
                    rusty::raft_log_info_5(
                        "[APPLY-THREAD] Site {}: IDLE core.execute_index_={} core.commit_index_={} queue_size={} applied_total={}",
                        self.site_id_, self.GetAppliedIndex(),
                        commit_index_snapshot, queue_size, apply_count);
                    last_log_time = now_secs;
                }
                unsafe {
                    raft_thread_sleep_ms(1);
                }
                continue;
            }

            let mut applied_entry: bool = false;
            {
                // An InstallSnapshot can acquire this gate after the entry
                // is popped but before its callback starts. Re-check the
                // published applied index inside the gate, so an entry the
                // snapshot covers is skipped once the snapshot state is in.
                let _apply_lock =
                    RaftStdLockGuard::new(&mut self.state_machine_apply_mtx_);
                let current_epoch: u64 =
                    self.apply_queue_.lock().unwrap().epoch_;
                let applied_index: u64 = self.GetAppliedIndex();
                if !raft_server_apply_epoch_is_current(entry_epoch,
                                                       current_epoch) {
                    rusty::raft_log_debug_4(
                        "[APPLY-THREAD] Site {}: Skipping invalidated entry {} (entry_epoch={} current_epoch={})",
                        self.site_id_, id, entry_epoch, current_epoch);
                } else if raft_server_log_index_at_or_below(id,
                                                            applied_index) {
                    rusty::raft_log_debug_3(
                        "[APPLY-THREAD] Site {}: Skipping snapshot-covered entry {} (applied={})",
                        self.site_id_, id, applied_index);
                } else {
                    // Entries near the historical stall point are logged at
                    // INFO for debugging.
                    if (470..=500).contains(&id) {
                        rusty::raft_log_info_3(
                            "[APPLY-THREAD] Site {}: ABOUT TO APPLY entry {} (queue_remaining={})",
                            self.site_id_, id, queue_size);
                    }
                    if !unsafe {
                        raft_apply_invoke(
                            &self.app_next_ as *const rusty::LearnerAction,
                            &self.pending_apply_command_
                                as *const rusty::RaftCommand,
                            self.site_id_, id)
                    } {
                        self.rpc_ready_
                            .store(false,
                                   rusty::sync::atomic::Ordering::Release);
                        self.stop_
                            .store(true,
                                   rusty::sync::atomic::Ordering::Release);
                        self.looping_
                            .store(false,
                                   rusty::sync::atomic::Ordering::Release);
                        continue;
                    }
                    if (470..=500).contains(&id) {
                        rusty::raft_log_info_2(
                            "[APPLY-THREAD] Site {}: DONE APPLYING entry {}",
                            self.site_id_, id);
                    }
                    self.PublishAppliedIndex(id);
                    applied_entry = true;
                }
            }
            if !applied_entry {
                continue;
            }
            apply_count += 1;

            if apply_count % 100 == 0 {
                rusty::raft_log_info_4(
                    "[APPLY-THREAD] Site {}: applied {} entries, core.execute_index_={} queue_remaining={}",
                    self.site_id_, apply_count, self.GetAppliedIndex(),
                    queue_size);
            }

            // Snapshot trigger for the queued apply path. The hot precheck
            // reads only atomic mirrors; the slow path revalidates canonical
            // state under the apply-gate -> Raft-mutex order.
            if self
                .snapshot_manager_configured_
                .load(rusty::sync::atomic::Ordering::Acquire)
            {
                let trigger_snapshot_index: u64 = self
                    .snapshot_trigger_index_
                    .load(rusty::sync::atomic::Ordering::Acquire);
                let trigger_threshold: u64 = self
                    .snapshot_trigger_threshold_
                    .load(rusty::sync::atomic::Ordering::Acquire);
                if raft_server_snapshot_is_due(trigger_snapshot_index,
                                               self.GetAppliedIndex(),
                                               trigger_threshold) {
                    self.MaybeCreateSnapshot();
                }
            }

            // Periodic cleanup goes through the snapshot-aware compactor,
            // which retains any prefix a snapshot does not yet cover.
            if id % 5000 == 0 {
                let applied_now: u64 = self.GetAppliedIndex();
                let cutoff: u64 = if applied_now > 10000 {
                    applied_now - 10000
                } else {
                    0
                };
                self.CompactLog(cutoff);
            }
        }
        rusty::raft_log_info_1(
            "[APPLY-THREAD] Site {}: Background apply thread exiting",
            self.site_id_);
    }

    // @safe - the three stores every SetupInternal failure path performed.
    // Named rather than repeated so a new failure path cannot forget one.
    pub fn FailClosed(&mut self) {
        self.stop_
            .store(true, rusty::sync::atomic::Ordering::Release);
        self.looping_
            .store(false, rusty::sync::atomic::Ordering::Release);
    }

    // `is_some` then `unwrap` mirrors the C++ null check it replaces;
    // rewriting it as a match would obscure the correspondence.
    #[allow(clippy::unnecessary_unwrap)]
    // @unsafe - CALLER MUST HOLD mtx_. Takes a state-machine checkpoint at
    // the applied index, records the new snapshot boundary, and compacts the
    // log behind it.
    pub fn CreateSnapshotLocked(&mut self) -> bool {
        let configured: bool =
            unsafe { raft_snapshot_manager_is_set(&self.snapshot_manager_) };
        if !configured {
            rusty::raft_log_debug_1(
                "[RAFT-SNAPSHOT] Site {}: No snapshot manager, skipping CreateSnapshot",
                self.site_id_);
            return false;
        }

        let snap_index: u64 = self.core.execute_index_;
        if snap_index == 0 {
            rusty::raft_log_debug_1(
                "[RAFT-SNAPSHOT] Site {}: core.execute_index_ is 0, nothing to snapshot",
                self.site_id_);
            return false;
        }
        if !raft_server_log_index_has_successor(snap_index) {
            rusty::raft_log_error_2(
                "[RAFT-SNAPSHOT] Site {}: Cannot snapshot terminal log index {}; no successor index is representable",
                self.site_id_, snap_index);
            return false;
        }

        let snap_term: i64;
        if raft_server_snapshot_term_uses_boundary(snap_index,
                                                   self.core.snapidx_) {
            // The boundary entry is intentionally absent after compaction.
            // Its term is carried by snapshot metadata; do not recreate the
            // entry or rewind the log's base by appending it again.
            snap_term = self.core.snapterm_;
        } else {
            let instance = self.core.raft_log_.get(snap_index);
            if instance.is_some() {
                snap_term = instance.unwrap().term();
            } else {
                // A missing historical term cannot be inferred from
                // current_term_: doing so would forge the snapshot boundary
                // tuple and could make a follower retain a conflicting
                // suffix. Preserve the existing state and wait for a
                // trustworthy boundary.
                rusty::raft_log_error_2(
                    "[RAFT-SNAPSHOT] Site {}: Cannot determine term at applied index {}; aborting snapshot creation",
                    self.site_id_, snap_index);
                return false;
            }
        }

        if !unsafe {
            raft_snapshot_serialize_and_save(
                &self.create_sm_snapshot_cb_ as *const rusty::RaftCreateSnapshotCb,
                &self.snapshot_manager_ as *const rusty::RaftSnapshotManagerPtr,
                self.site_id_, snap_index, snap_term)
        } {
            return false;
        }

        let old_snapidx: u64 = self.core.snapidx_;
        self.core.snapidx_ = snap_index;
        self.core.snapterm_ = snap_term;
        self.snapshot_trigger_index_
            .store(self.core.snapidx_,
                   rusty::sync::atomic::Ordering::Release);
        rusty::raft_log_info_4(
            "[RAFT-SNAPSHOT] Site {}: Snapshot saved at index={} term={} (prev snapidx={})",
            self.site_id_, snap_index, snap_term, old_snapidx);

        let compacted: usize = self.CompactLogLocked(snap_index);
        rusty::raft_log_info_3(
            "[RAFT-SNAPSHOT] Site {}: Compacted {} entries up to index={}",
            self.site_id_, compacted, snap_index);
        true
    }

    // @safe - [move, M1] RaftCore::is_config_member.
    pub fn IsConfigMember(&self, site: u16) -> bool {
        self.core.is_config_member(site)
    }

    // @safe - [move, M1] RaftCore::peer_ordinal.
    pub fn PeerOrdinal(&self, site: u16) -> usize {
        self.core.peer_ordinal(site)
    }

    // @unsafe - publishes the cross-thread replication wake.
    //
    // Every decision is Rust: publish() reports whether the gate is still
    // accepting, reserve_wake_owner() hands out the owner thread if a waiter
    // is armed and no wake is already queued, and the job the reactor runs is
    // a Rust-owned token (queue_wake_job). Only PollThread::add is a kernel.
    pub fn RequestReplication(&mut self) {
        if !self.replication_wake_gate_.publish() {
            return;
        }
        let owner: rusty::Option<rusty::RaftPollThreadPtr> =
            self.replication_wake_gate_.reserve_wake_owner();
        if let rusty::Some(owner) = owner {
            self.queue_wake_job(owner, false);
        }
    }

    // The reactor job as Rust owns it: a Box<GateWakeJob> -- the gate's Arc
    // and which wake to run -- made raw for the kernel, which queues a
    // OneTimeJob on the owner thread whose body is the raft_wake_job_run
    // export: it takes the Box back, runs the wake, and drops it. The token,
    // not the server, is what the job holds, as the C++ closure used to hold
    // only the gate.
    fn queue_wake_job(&self, owner: rusty::RaftPollThreadPtr, is_shutdown: bool) {
        let token: rusty::Box<GateWakeJob> = rusty::Box::new(GateWakeJob {
            gate: self.replication_wake_gate_.clone(),
            shutdown: is_shutdown,
        });
        unsafe {
            raft_queue_wake_job(
                &owner as *const rusty::RaftPollThreadPtr,
                rusty::Box::into_raw(token) as *mut core::ffi::c_void);
        }
    }

    // @unsafe - Called only by HeartbeatLoop, on its bound PollThread.
    //
    // begin_wait_for_work() is the fast path: Some(answer) when the gate
    // could decide without arming a waiter, None when the caller must arm
    // one. Only the slow path allocates an event, so a round that finds work
    // already pending allocates nothing -- the same split the C++ had, for
    // the same reason.
    // is_some()/unwrap() rather than `if let`, for the reason recorded on
    // ReplicationWakeGate::wake_on_owner: the emitter renders an `if let`
    // binding with a dot where the C++ needs an arrow. The annotation is
    // load-bearing, not documentation.
    #[allow(clippy::unnecessary_unwrap)]
    pub fn WaitForReplicationOrHeartbeat(&mut self, timeout_us: u64) -> bool {
        let decided: rusty::Option<bool> =
            self.replication_wake_gate_.begin_wait_for_work();
        if decided.is_some() {
            return decided.unwrap();
        }
        let waiter: rusty::RaftIntEventPtr = rusty::raft_new_int_event();
        self.replication_wake_gate_.finish_wait_for_work(waiter, timeout_us)
    }

    // @unsafe - Called only by the election fiber, on the bound PollThread.
    //
    // The accepting() check stays AHEAD of the factory call: a closed gate
    // must not allocate an event it will never wait on.
    pub fn WaitForElectionTimeoutOrShutdown(&mut self,
                                            timeout_us: u64) -> bool {
        if !self.replication_wake_gate_.accepting() {
            return false;
        }
        let waiter: rusty::RaftIntEventPtr = rusty::raft_new_int_event();
        self.replication_wake_gate_
            .wait_for_election_timeout(waiter, timeout_us)
    }

    // @unsafe - one heartbeat tick's wait, which is the above bound to the
    // configured interval.
    pub fn HeartbeatWait(&mut self) -> bool {
        self.WaitForReplicationOrHeartbeat(self.heartbeat_interval_us_)
    }

    // @unsafe - Bind the gate to the communicator's PollThread before
    // HeartbeatLoop can publish an owner-thread-only IntEvent against it.
    pub fn BindReplicationWakeOwner(
        &mut self, owner: rusty::RaftPollThreadPtr) {
        self.replication_wake_gate_.bind_owner(owner);
    }

    // @unsafe - Close ordering is intentional: make new submissions inert,
    // queue one owner-thread wake for an armed waiter, then drop the owner's
    // gate handle.
    pub fn CloseReplicationWakeGate(&mut self) {
        self.replication_wake_gate_.close();
        let owner: rusty::Option<rusty::RaftPollThreadPtr> =
            self.replication_wake_gate_.reserve_shutdown_wake_owner();
        if let rusty::Some(owner) = owner {
            self.queue_wake_job(owner, true);
        }
        self.replication_wake_gate_.clear_owner();
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
        // [fix, F5] Inside the verified configuration there are no snapshots.
        if self.verified_gates_ {
            return;
        }
        let _apply_lock =
            RaftStdLockGuard::new(&mut self.state_machine_apply_mtx_);
        let _lock = RaftLockGuard::new(&mut self.mtx_);
        let configured: bool =
            unsafe { raft_snapshot_manager_is_set(&self.snapshot_manager_) };
        if !configured
            || !raft_server_snapshot_is_due(self.core.snapidx_,
                                            self.core.execute_index_,
                                            self.core.snapshot_threshold_)
        {
            return;
        }
        self.CreateSnapshotLocked();
    }

    // @unsafe - copies the manager under mtx_ before querying it, so the
    // query itself never runs with Raft state locked.
    //
    // The copy is real (plan N4): before, only the is_set test ran under the
    // lock and the query read snapshot_manager_ after releasing it, so a
    // concurrent SetSnapshotManagerLocked could replace the carrier mid-read.
    pub fn HasSnapshot(&mut self) -> bool {
        let mut manager: rusty::RaftSnapshotManagerPtr = Default::default();
        let configured: bool = {
            let _lock = RaftLockGuard::new(&mut self.mtx_);
            let set: bool =
                unsafe { raft_snapshot_manager_is_set(&self.snapshot_manager_) };
            if set {
                unsafe {
                    raft_snapshot_manager_ptr_clone_into(&self.snapshot_manager_,
                                                         &raw mut manager);
                }
            }
            set
        };
        if !configured {
            return false;
        }
        let mut index: u64 = 0;
        let mut term: u64 = 0;
        unsafe {
            raft_snapshot_manager_latest(
                &manager as *const rusty::RaftSnapshotManagerPtr,
                &raw mut index, &raw mut term)
        }
    }

    // @unsafe - gates inbound and outbound test traffic under mtx_.
    pub fn Disconnect(&mut self, disconnect: bool) {
        // Taken before the guard, not inside it: RaftLockGuard borrows
        // self.mtx_ mutably, so `self as *mut RaftServerBase` cannot be
        // written in its scope. The kernel resolves the communicator from
        // this identity -- see commo_of in server.cc.
        let this = self as *mut RaftServerBase;
        let _lock = RaftLockGuard::new(&mut self.mtx_);
        assert!(
            self.disconnected_.load(rusty::sync::atomic::Ordering::Acquire)
                != disconnect);  // [move, M10]
        unsafe {
            // A seam kernel: each lane's runtime owns its own network flag
            // (the C++ lane's RaftCommo, the Rust lane's RaftTransport).
            raft_commo_set_network_enabled(this as *mut RaftServerHandle, !disconnect);
        }
        self.disconnected_
            .store(disconnect, rusty::sync::atomic::Ordering::Release);
    }

    // @safe - calls Disconnect and resets the timer.
    pub fn Reconnect(&mut self) {
        self.Disconnect(false);
        self.resetTimer("reconnect");
    }

    // @unsafe - runs SetupInternal under a catch-all and publishes the
    // result to whoever is blocked in WaitForStartup.
    pub fn Setup(&mut self) {
        let succeeded: bool =
            unsafe { raft_setup_internal_guarded(self.handle(),
                                                 self.site_id_) };
        if !succeeded {
            self.rpc_ready_
                .store(false, rusty::sync::atomic::Ordering::Release);
            self.stop_
                .store(true, rusty::sync::atomic::Ordering::Release);
            self.looping_
                .store(false, rusty::sync::atomic::Ordering::Release);
        }
        // `ready` is read unconditionally, as it was when this was one
        // critical section: `succeeded && self.IsRpcReady()` would
        // short-circuit and skip the call.
        let ready: bool = self.IsRpcReady();
        // Published BEFORE the flag, so the unlock below releases it to
        // whichever thread the wait hands the flag to.
        self.startup_succeeded_ = succeeded && ready;
        {
            let mut finished = self.startup_finished_.lock().unwrap();
            *finished = true;
        }
        self.startup_cv_.notify_all();
    }

    // @safe - election timer setup; the fiber spawn is a kernel.
    pub fn StartElectionTimer(&mut self) {
        self.ElectionLoopSetRunning(true);
        self.resetTimer("start election timer");
        let wait_int: u64 = self.wait_int_ as u64;
        unsafe {
            raft_spawn_election_timer(self.handle(), wait_int);
        }
    }

    // @unsafe - hands newly committed entries to the background apply thread.
    #[allow(clippy::manual_is_multiple_of)]
    // ONE PASS, TWO CRITICAL SECTIONS. The scan stops at the first gap and
    // lifts each usable command out of the log as it goes, because the log
    // must not be read while the apply queue is locked -- the reason this used
    // to be split between a Rust scan and a C++ push kernel. Copying a
    // Command is a refcount bump on its inner Arc, not a payload copy.
    pub fn EnqueueCommittedEntries(&mut self, old_commit: u64,
                                   new_commit: u64) {
        let mut batch: rusty::VecDeque<QueuedApplyEntry> =
            rusty::VecDeque::new();
        let mut first_missing: u64 = 0;
        let mut id: u64 = old_commit + 1;
        while id <= new_commit {
            let found = self.core.raft_log_.get(id);
            if found.is_none() {
                first_missing = id;
                break;
            }
            let entry: &RaftEntry = found.unwrap();
            let usable: bool = entry.has_value();  // [move, M6]
            if !usable {
                first_missing = id;
                break;
            }
            // The command copy is a kernel: a refcount the carrier cannot
            // touch. epoch_ is stamped below, under the mutex that owns it.
            let mut command: rusty::RaftCommand = Default::default();
            unsafe {
                raft_command_clone_into(
                    entry.cmd() as *const rusty::RaftCommand,
                    &mut command as *mut rusty::RaftCommand);
            }
            batch.push_back(QueuedApplyEntry {
                index_: id,
                command_: command,
                epoch_: 0,
            });
            id += 1;
        }

        let enqueued: u64 = batch.len() as u64;
        // One in fifty, so a steady stream of commits does not drown the log.
        // `%` rather than is_multiple_of: this lowers to C++, where uint64_t
        // has no such member.
        let ticket: u64 = self.enqueue_log_counter_;
        self.enqueue_log_counter_ += 1;
        let want_size: bool = ticket % 50 == 0;

        // ONE acquisition covering the push and the size read, and none at
        // all when there is neither to do. The kernels took it twice on a
        // logging tick and once otherwise.
        let mut qsize: u64 = 0;
        if enqueued > 0 || want_size {
            let mut queue = self.apply_queue_.lock().unwrap();
            let epoch: u64 = queue.epoch_;
            while !batch.is_empty() {
                let mut queued = batch.pop_front().unwrap();
                queued.epoch_ = epoch;
                unsafe { raft_trace_at(9, queued.index_, 0) };  // [M0] trace kit
                queue.entries_.push_back(queued);
            }
            qsize = queue.entries_.len() as u64;
        }

        if first_missing > 0 {
            rusty::raft_log_info_5(
                "[ENQUEUE] Site {}: gap at slot {} (range {}..{}, enqueued {})",
                self.site_id_, first_missing, old_commit + 1, new_commit,
                enqueued);
        }
        if want_size {
            rusty::raft_log_info_5(
                "[ENQUEUE] Site {}: enqueued {} entries ({}..{}) queue_total={}",
                self.site_id_, enqueued, old_commit + 1, new_commit, qsize);
        }
    }

    // @unsafe - the campaign. Two critical sections with one fiber
    // suspension between them: the broadcast must not hold mtx_, and every
    // decision after it must be re-derived from state read under the
    // reacquired lock. [move, M5] Each section is a core call
    // (RaftCore::start_election, RaftCore::election_settle); the broadcast
    // and its wait are the lane's.
    //
    // Returns true only when this server both won the election and still
    // held leadership when the result was applied.
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

        // [move, M5] The campaign's start, a core call under mtx_; its timer
        // reset is an action that runs before the guard drops.
        let mut out1: CoreOutput = CoreOutput::new();
        let campaign: CampaignStart = {
            let _lock = RaftLockGuard::new(&mut self.mtx_);
            let stopped: bool = self.stopped_now();
            // [move, M4] the clock read, as the timer check made it
            let now: u64 = unsafe { raft_time_now_us() };
            let decided: CampaignStart = self.core.start_election(
                timer_guarded, expected_generation, now, stopped, &mut out1);
            self.run_locked_actions(&out1);
            decided
        };
        self.run_unlocked_actions(&out1);
        if !campaign.started_ {
            return false;
        }
        let prev_term: u64 = campaign.prev_term_;
        let prev_vote_for: u16 = campaign.prev_vote_for_;
        let term: u64 = campaign.term_;
        let lst_idx: u64 = campaign.lst_idx_;
        let lst_term: i64 = campaign.lst_term_;

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
        let mut quorum: rusty::RaftVoteQuorumPtr = Default::default();
        unsafe {
            raft_broadcast_vote_and_wait(
                self.handle(), par_id, lst_idx, lst_term,
                self.site_id_, term as i64,
                &mut quorum as *mut rusty::RaftVoteQuorumPtr);
        }

        // [move, M5] The settlement, a core call under mtx_: the replies the
        // lane's wait gathered are handed to the core, which counts them
        // itself. Its locked actions run before the guard drops; the
        // leader-change callback after it ([fix, F6]).
        let mut out: CoreOutput = CoreOutput::new();
        let won: bool = {
            let _lock1 = RaftLockGuard::new(&mut self.mtx_);
            let q: *const rusty::RaftVoteQuorumPtr =
                &quorum as *const rusty::RaftVoteQuorumPtr;
            let n_total: u64 = unsafe { raft_vote_quorum_size(q) };
            let timed_out: bool = unsafe { raft_vote_quorum_timed_out(q) };
            let replies: u64 = unsafe { raft_vote_quorum_reply_count(q) };
            let mut voters: rusty::Vec<u16> = rusty::Vec::new();
            let mut granted: rusty::Vec<bool> = rusty::Vec::new();
            let mut reply_terms: rusty::Vec<i64> = rusty::Vec::new();
            let mut r: u64 = 0;
            while r < replies {
                let mut voter: u16 = 0;
                let mut vote: bool = false;
                let mut reply_term: i64 = 0;
                if unsafe {
                    raft_vote_quorum_reply_at(q, r, &mut voter as *mut u16,
                                              &mut vote as *mut bool,
                                              &mut reply_term as *mut i64)
                } {
                    voters.push(voter);
                    granted.push(vote);
                    reply_terms.push(reply_term);
                }
                r += 1;
            }
            let stopped: bool = self.stopped_now();
            let looping: bool =
                self.looping_.load(rusty::sync::atomic::Ordering::Acquire);
            let election_debug: bool = unsafe { raft_election_debug_enabled() };
            let decided: bool = self.core.election_settle(
                term, loc_id, &voters, &granted, &reply_terms, n_total,
                timed_out, stopped, looping, self.failover_, election_debug,
                &mut out);
            self.run_locked_actions(&out);
            decided
        };
        self.run_unlocked_actions(&out);
        won
    }


}

// [move, M11] An inbound AppendEntries payload, opaque to the core. Its two
// methods are the shell's wire kernels -- the entries' count and terms, and
// each entry made into a log entry -- and the core calls them only after its
// gates have passed, where the handler always did ("decode only after the
// gates"). Built by the shell around the RPC's command for one call.
pub struct WireBatch {
    cmd_: *const core::ffi::c_void,
    has_cmd_: bool,
    next_log_term_: u64,
}

#[allow(clippy::not_unsafe_ptr_arg_deref)]
impl WireBatch {
    pub fn new(cmd: *const core::ffi::c_void, has_cmd: bool,
               next_log_term: u64) -> WireBatch {
        WireBatch { cmd_: cmd, has_cmd_: has_cmd, next_log_term_: next_log_term }
    }

    // [move, M1] RaftServerBase::AeDecodePayload. Decodes the payload into
    // `terms` -- one term per encoded entry, so its length IS the decoded
    // count -- and reports whether that count fits after
    // leader_prev_log_index.
    pub fn decode_terms(&self, leader_prev_log_index: u64,
                        terms: &mut rusty::Vec<i64>) -> bool {
        terms.clear();
        if !self.has_cmd_ {
            return raft_server_append_entry_count_fits(leader_prev_log_index,
                                                       0);
        }
        if unsafe { raft_wire_is_batch(self.cmd_) } {
            let batch: *const core::ffi::c_void = unsafe { raft_wire_batch(self.cmd_) };
            if batch.is_null() {
                return false;
            }
            let count: u64 = unsafe { raft_batch_len(batch) };
            // [fix, F4] Every entry term must be a Raft term, i.e. at least 1
            // (the spec's B16). A payload carrying any other is refused like
            // any undecodable one.
            let mut terms_valid: bool = true;
            let mut i: u64 = 0;
            while i < count {
                let term: i64 = unsafe { raft_batch_term_at(batch, i) };
                if term < 1 {
                    terms_valid = false;  // [fix, F4]
                }
                terms.push(term);
                i += 1;
            }
            return terms_valid  // [fix, F4]
                && raft_server_append_batch_count_is_valid(
                    leader_prev_log_index, count);
        }
        terms.push(self.next_log_term_ as i64);
        // [fix, F4] the single entry's term, checked as decoded
        (self.next_log_term_ as i64) >= 1
            && raft_server_append_entry_count_fits(leader_prev_log_index, 1)
    }

    // [move, M1] RaftServerBase::AeApplyIncoming. Appends the payload's
    // entries from first_write_index on, in index order, after the caller
    // truncated the divergent suffix.
    pub fn append_into(&self, log: &mut RaftLog, leader_prev_log_index: u64,
                       first_write_index: u64) {
        if unsafe { raft_wire_is_batch(self.cmd_) } {
            let batch: *const core::ffi::c_void = unsafe { raft_wire_batch(self.cmd_) };
            assert!(!batch.is_null());  // [move, M10]
            let count: u64 = unsafe { raft_batch_len(batch) };
            let mut i: u64 = 0;
            while i < count {
                let index: u64 =
                    raft_server_append_sent_end(leader_prev_log_index, i + 1);
                if index >= first_write_index {
                    let term: i64 = unsafe { raft_batch_term_at(batch, i) };
                    let mut entry_cmd: rusty::RaftCommand = Default::default();
                    unsafe {
                        raft_batch_command_into(
                            batch, i, &mut entry_cmd as *mut rusty::RaftCommand);
                    }
                    let appended: u64 = log
                        .append(raft_entry_from_command(term, entry_cmd));  // [move, M6]
                    assert!(appended == index);  // [move, M10]
                }
                i += 1;
            }
            return;
        }
        let index: u64 = raft_server_append_sent_end(leader_prev_log_index, 1);
        if index >= first_write_index {
            let mut copy: rusty::RaftCommand = Default::default();
            unsafe {
                raft_wire_command_clone_into(
                    self.cmd_, &mut copy as *mut rusty::RaftCommand);
            }
            let appended: u64 = log.append(
                raft_entry_from_command(self.next_log_term_ as i64, copy));  // [move, M6]
            assert!(appended == index);  // [move, M10]
        }
    }
}

// [move, M6] An entry for the log, with the facts the core reads about its
// command asked once, here. Shell code: it calls a kernel.
pub fn raft_entry_from_command(term: i64, cmd: rusty::RaftCommand) -> RaftEntry {
    let mut has_value: bool = false;
    let mut is_tpc_commit: bool = false;
    let mut kind: i32 = 0;
    let mut payload_bytes: u64 = 0;
    unsafe {
        raft_command_meta(&cmd as *const rusty::RaftCommand,
                          &mut has_value as *mut bool,
                          &mut is_tpc_commit as *mut bool,
                          &mut kind as *mut i32,
                          &mut payload_bytes as *mut u64);
    }
    RaftEntry::new(term, cmd, has_value, is_tpc_commit, kind, payload_bytes)
}

// The log's writers and the membership loader, in Rust. Until step C1b these
// were kernels reaching into core.raft_log_, decoded_terms_,
// config_members_ and batch_buffer_ from C++. What those kernels could not
// do -- copy a janus::Command, look inside a TpcBatchCommand, read the yaml
// config -- is now the whole of what the raft_command_* / raft_wire_* /
// raft_batch_* / raft_config_* kernels do; the containers are touched here.
//
// not_unsafe_ptr_arg_deref: the c_void payload pointers are the service's
// wire payload, borrowed for the call and only ever handed to the kernels.
#[allow(non_snake_case)]
#[allow(clippy::not_unsafe_ptr_arg_deref)]
impl RaftServerBase {
    // Appends one entry at the current term -- mtx_ held by the caller -- and
    // reports the PRE-append tail: the new entry lands at prev + 1.
    pub fn AppendLocal(&mut self, cmd: rusty::RaftCommand) -> u64 {
        // [move, M3] The shell asks the command's metadata (a kernel); the
        // core appends it at its current term.
        let mut has_value: bool = false;
        let mut is_tpc_commit: bool = false;
        let mut kind: i32 = 0;
        let mut payload_bytes: u64 = 0;
        unsafe {
            raft_command_meta(&cmd as *const rusty::RaftCommand,
                              &mut has_value as *mut bool,
                              &mut is_tpc_commit as *mut bool,
                              &mut kind as *mut i32,
                              &mut payload_bytes as *mut u64);
        }
        self.core.append_local(cmd, has_value, is_tpc_commit, kind,
                               payload_bytes)
    }

    // The new leader's no-op entry, so the term commits something without
    // waiting for a client. Skipped in lab mode, where the suite counts
    // entries.
    pub fn AppendLeaderNoop(&mut self) {
        if cfg!(feature = "raft_test") {
            return;
        }
        let mut noop: rusty::RaftCommand = Default::default();
        unsafe {
            raft_noop_command_into(&mut noop as *mut rusty::RaftCommand);
        }
        let previous_index: u64 = self.AppendLocal(noop);
        assert!(
            self.core.raft_log_.last_index() == previous_index + 1);  // [move, M10]
        rusty::raft_log_info_3(
            "[RAFT-NOOP] Site {} appended leader no-op at index {} term {}",
            self.site_id_, self.core.raft_log_.last_index(),
            self.core.current_term_);
        self.RequestReplication();
    }

    // [fix, F5] The gates of the verified configuration: snapshots off, so
    // the log is whole (no snapshot boundary, base 1); failover on; a static
    // configuration that contains this server. CALLER MUST NOT HOLD mtx_.
    pub fn verified_config_ok(&mut self) -> bool {
        let snapshots_off: bool = !unsafe { raft_env_snapshots_enabled() };
        let _lock = RaftLockGuard::new(&mut self.mtx_);
        let whole_log: bool =
            self.core.snapidx_ == 0 && self.core.raft_log_.base() == 1;
        let mut contains_self: bool = false;
        let mut i: usize = 0;
        while i < self.core.config_members_.len() {
            if self.core.config_members_[i] == self.site_id_ {
                contains_self = true;
            }
            i += 1;
        }
        snapshots_off && whole_log && self.failover_ && contains_self
    }

    // config_members_ from the static config: the partition's sorted,
    // de-duplicated site ids. Returns the replica count.
    pub fn LoadCurrentConfig(&mut self) -> u64 {
        let replicas: u64 =
            unsafe { raft_config_replica_count(self.partition_id_) };
        self.core.config_members_.clear();
        let mut i: u64 = 0;
        while i < replicas {
            let site: u16 =
                unsafe { raft_config_replica_site(self.partition_id_, i) };
            self.core.config_members_.push(site);
            i += 1;
        }
        replicas
    }


}

// What RaftServer's constructor does after the generated one. The generated
// constructor initialises every field the DSL can spell; these two it could
// not -- a std::make_shared whose payload points back at this object, and a
// macro -- come from kernels now, and the rest is what the C++ constructor
// did in order. RaftServer::RaftServer() is one call to this.
#[allow(non_snake_case)]
impl RaftServerBase {
    pub fn ConstructRuntime(&mut self) {
        let lifetime_slot: *mut rusty::RaftAsyncCallbackLifetimePtr =
            &mut self.async_callback_lifetime_
                as *mut rusty::RaftAsyncCallbackLifetimePtr;
        unsafe {
            raft_new_callback_lifetime(self.handle(),
                                       lifetime_slot);
        }
        self.heartbeat_interval_us_ =
            unsafe { raft_heartbeat_interval_default() };
        unsafe {
            raft_ensure_legacy_payload_registered();
        }
        if cfg!(feature = "raft_test") {
            // [move, M3] No lock: nothing else can see the server yet.
            let mut out: CoreOutput = CoreOutput::new();
            let stopped: bool = self.stopped_now();
            self.core.set_is_leader(false, stopped, self.failover_, &mut out);
            self.run_locked_actions(&out);
            self.run_unlocked_actions(&out);
        }
        self.stop_.store(false, rusty::sync::atomic::Ordering::Release);
    }
}

// RaftLab inspection: read-only views of the state the 25-case suite asserts
// on, plus the two mutexes it takes and the one method it drives. Every one
// is a getter, so test.cc and testconf.cc hold the layout of nothing --
// this replaces the C++ shim's LabAccess, which named the fields directly.
// Emitted unconditionally because the DSL has no cfg; they are inline reads.
// CALLER MUST HOLD LabMutex() for the core reads, as the tests always did.
#[allow(non_snake_case)]
impl RaftServerBase {
    // Raw pointers, which is what the lock guards take: the transpiled C++
    // lane does not apply Rust's &mut -> *mut coercion to a returned
    // reference.
    pub fn LabMutex(&mut self) -> *mut rusty::RaftCheckedMutex {
        &raw mut self.mtx_
    }
    pub fn LabApplyMutex(&mut self) -> *mut rusty::RaftStdMutex {
        &raw mut self.state_machine_apply_mtx_
    }
    pub fn LabStopped(&self) -> bool {
        self.stop_.load(rusty::sync::atomic::Ordering::Acquire)
    }
    pub fn LabCurrentTerm(&self) -> u64 {
        self.core.current_term_
    }
    pub fn LabCommitIndex(&self) -> u64 {
        self.core.commit_index_
    }
    pub fn LabExecuteIndex(&self) -> u64 {
        self.core.execute_index_
    }
    pub fn LabLastLogIndex(&self) -> u64 {
        self.core.raft_log_.last_index()
    }
    pub fn LabLogBase(&self) -> u64 {
        self.core.raft_log_.base()
    }
    pub fn LabIsLeader(&self) -> bool {
        self.core.is_leader_
    }
    pub fn LabVoteFor(&self) -> u16 {
        self.core.vote_for_
    }
    pub fn LabCurrentLeaderId(&self) -> u16 {
        self.core.current_leader_id_
    }
    pub fn LabReqVoting(&self) -> bool {
        self.core.req_voting_
    }
    pub fn LabElectionInProgress(&self) -> bool {
        self.core.election_in_progress_
    }
    pub fn LabSnapIdx(&self) -> u64 {
        self.core.snapidx_
    }
    pub fn LabSnapTerm(&self) -> i64 {
        self.core.snapterm_
    }
    pub fn LabSnapshotManager(&self) -> &rusty::RaftSnapshotManagerPtr {
        &self.snapshot_manager_
    }
    // The lab suite's log fingerprint (test.cc RaftLogFingerprint), one
    // element per call so the harness never holds a pointer into the log:
    // element 0 is the base, 1 the length, 2+i the term of entry base+i, or
    // 0 where there is none. is_some()/unwrap() rather than `if let`, for the
    // reason recorded on ReplicationWakeGate::wake_on_owner.
    pub fn LabLogFingerprintLen(&self) -> u64 {
        2 + self.core.raft_log_.len() as u64
    }
    #[allow(clippy::unnecessary_unwrap)]
    pub fn LabLogFingerprintAt(&self, i: u64) -> u64 {
        if i == 0 {
            return self.core.raft_log_.base();
        }
        if i == 1 {
            return self.core.raft_log_.len() as u64;
        }
        let entry: rusty::Option<&RaftEntry> =
            self.core.raft_log_.get(self.core.raft_log_.base() + (i - 2));
        if entry.is_some() {
            return entry.unwrap().term() as u64;
        }
        0
    }
}

// The three methods a worker reaches through a TxLogServer base pointer.
// Raft's set_site_identity mirrors the ids into core as well, where
// converted Rust bodies can see them, and asserts the copies agree.
//
// `#[cpp_inherit]` on BOTH impls, deliberately. The transpiler grants a struct
// one C++ base -- the last `#[cpp_inherit]` impl in the block wins, which is
// RaftSpecific below, whose supertrait is this -- but the attribute does a
// second job per impl: without it the impl lowers through the generic
// TraitAdapter<Self> path, which emits a by-value `TxLogServerAdapter<
// RaftServerBase>` that cannot compile (the struct is move-only and holds
// mutexes). The static_assert after the struct pins the base clause, so if
// the "last wins" order ever changes, the build fails instead of the vtable.
#[cpp_inherit]
impl TxLogServer for RaftServerBase {
    // The communicator is NOT stored. `commo_: *mut rusty::Communicator` was
    // the one field of RaftServerBase's forty-eight that is not `Send`, and it
    // is `Send` the Rust srpc lane demands of anything it dispatches to
    // (`trait Service: Send + Sync`, src/srpc/rpc/server.rs). Asserting
    // `unsafe impl Send` over it would have been false: janus::Communicator
    // then held `peers_` and `partition_peers_` as unguarded std::maps.
    // (Stage 3d has since replaced all five of Communicator's members with
    // one Rust-authored PeerRegistry, so that hazard is gone -- but it was
    // real then, which is why the field went.) The pointer stays on the C++
    // side, in a table
    // keyed by this server, and the kernels ask for it by identity.
    //
    // not_unsafe_ptr_arg_deref: the kernel's dynamic_cast does read through
    // `commo`, and the method is not `unsafe fn` because it cannot be -- it
    // implements TxLogServer, whose signature is shared with the Paxos server
    // (src/deptran/scheduler.h). The contract is the one that method always
    // had: the worker passes a live Communicator and outlives the server.
    #[allow(clippy::not_unsafe_ptr_arg_deref)]
    fn set_commo(&mut self, commo: *mut rusty::Communicator) {
        unsafe { raft_bind_commo(self.handle(), commo) }
    }

    fn set_site_identity(&mut self, loc_id: u32, site_id: u16, partition_id: u32) {
        // Publish this replica to the lab registry. See lab_registry's note:
        // this is the one point the server learns which replica it is, and it
        // is already Rust, so the harness needs no export to find the cluster.
        #[cfg(feature = "raft_test")]
        lab_register(loc_id, self as *mut RaftServerBase);
        #[cfg(feature = "raft_test")]
        rusty::raft_log_info_3(
            "[LAB-REGISTRY] replica loc_id={} site={} published ({} of 5)",
            loc_id, site_id, lab_count() as i64);
        self.loc_id_ = loc_id;
        self.site_id_ = site_id;
        self.partition_id_ = partition_id;
        self.core.loc_id_ = loc_id;
        self.core.site_id_ = site_id;
        self.core.partition_id_ = partition_id;
        assert!(self.core.site_id_ == self.site_id_
            && self.core.partition_id_ == self.partition_id_
            && self.core.loc_id_ == self.loc_id_);  // [move, M10]
    }

    // The callback is copied INTO its slot by the kernel, never moved: a
    // libc++ std::function whose callable fits its small buffer points at that
    // buffer, so a bitwise move (a Rust move) leaves it pointing at the old
    // storage and the next call or destruction runs off a dead stack frame --
    // the SIGSEGV the first rustc-compiled build hit right here.
    fn reg_learner_action(&mut self, learner_action: &rusty::LearnerAction) {
        unsafe {
            raft_learner_action_clone_into(
                learner_action as *const rusty::LearnerAction,
                &mut self.app_next_ as *mut rusty::LearnerAction);
        }
    }
}

// The Raft-specific interface: what the workers and the RPC service reach
// beyond TxLogServer (declared next to it in scheduler.h). `#[cpp_inherit]`
// is spent here -- the transpiler grants a struct one C++ base -- and
// RaftSpecific: TxLogServer carries the other, so the emitted C++ is
// `struct RaftServerBase : public RaftSpecific` with TxLogServer above it.
// not_unsafe_ptr_arg_deref: the raw pointers are the srpc service's C++
// out-parameters. Start writes through its two under `unsafe`; the three
// RPC methods hand theirs to the raft_rpc_* kernels untouched.
#[allow(clippy::not_unsafe_ptr_arg_deref)]
#[cpp_inherit]
#[allow(non_snake_case)]
#[allow(clippy::too_many_arguments)]
impl RaftSpecific for RaftServerBase {
    // @unsafe - idempotent one-shot setup.
    fn EnsureSetup(&mut self) {
        if self.heartbeat_setup_ {
            return;
        }
        self.heartbeat_setup_ = true;
        self.Setup();
    }

    // @safe - waits for the owner-thread startup job and reports its result.
    fn WaitForStartup(&mut self) -> bool {
        {
            let finished = self.startup_finished_.lock().unwrap();
            // wait_while re-checks under the lock on every wake, so a
            // spurious one is not a false start -- the same guarantee the
            // predicate form of std::condition_variable::wait gave.
            let _finished = self
                .startup_cv_
                .wait_while(finished, |done: &mut bool| !*done)
                .unwrap();
        }
        self.startup_succeeded_
    }

    // @unsafe - must be called from a reactor fiber before destroying a live
    // server; signals both runtime loops and waits for their completion
    // flags, then stops and joins the apply thread while the server is still
    // fully alive (applying an entry can trigger snapshot compaction).
    fn PrepareForShutdown(&mut self) {
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
        self.CloseReplicationWakeGate();

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
            raft_apply_thread_join(
                &mut self.apply_thread_ as *mut rusty::RaftStdThread);
        }
    }

    // For callers that do not hold mtx_. [fix, F8] It no longer takes it:
    // the role is the mirror the last critical section published.
    fn IsLeader(&mut self) -> bool {
        if !self.looping_.load(rusty::sync::atomic::Ordering::Acquire) {
            return false;
        }
        self.is_leader_mirror_.load(rusty::sync::atomic::Ordering::Acquire)
    }

    // [fix, F8] Without mtx_: this server's id while it leads, otherwise the
    // leader it last heard from, as the last critical section published it.
    fn GetLeaderHint(&mut self) -> u16 {
        self.leader_hint_mirror_.load(rusty::sync::atomic::Ordering::Acquire) as u16
    }

    // @safe - writes a shell atomic and logs. [move, M1] Not mtx_ any more
    // (plan Phase 4): only the election timeout's choice reads it, and
    // nothing in the core does.
    fn SetPreferredLeader(&mut self, site_id: u16) {
        let old_preferred: u16 = self.preferred_leader();
        self.preferred_leader_site_id_
            .store(site_id as u64, rusty::sync::atomic::Ordering::Release);
        if old_preferred != site_id {
            rusty::raft_log_info_2(
                "[LEADERSHIP-TRANSFER] Site {}: Preferred leader set to {}",
                self.site_id_, site_id);
        }
    }

    // @safe - a plain move into the notification slot.
    // In place, for the reason given on reg_learner_action.
    // [fix, F6] Under leader_notices_'s lock: with the callback fired after
    // mtx_ is released, the slot is read under that lock instead
    // (run_locked_actions, fire_leader_notices), so writing it there keeps
    // registration from racing a firing.
    fn RegisterLeaderChangeCallback(&mut self, cb: &rusty::RaftLeaderChangeCb) {
        let _notices = self.leader_notices_.lock().unwrap();
        unsafe {
            raft_leader_change_cb_clone_into(
                cb as *const rusty::RaftLeaderChangeCb,
                &mut self.leader_change_cb_ as *mut rusty::RaftLeaderChangeCb);
        }
    }

    // @safe - acquire load pairing with the final startup publication.
    fn IsRpcReady(&self) -> bool {
        self.rpc_ready_.load(rusty::sync::atomic::Ordering::Acquire)
    }

    // @unsafe - synchronizes with Disconnect() through the Raft state mutex.
    fn SiteId(&self) -> u16 {
        self.site_id_
    }

    fn PartitionId(&self) -> u32 {
        self.partition_id_
    }

    // See the trait: this is the unlocked read get_outstanding_logs makes.
    // [fix, F8] It reads the mirror, not the core's field, which another
    // thread may be writing under mtx_ (bugs-found B2).
    fn CommitIndex(&self) -> u64 {
        self.commit_index_mirror_.load(rusty::sync::atomic::Ordering::Acquire)
    }

    // @unsafe - CALLER MUST NOT HOLD mtx_. Appends one command locally and
    // then publishes the replication wake, in that order: the wake path never
    // nests the gate's owner mutex below Raft state.
    #[allow(clippy::not_unsafe_ptr_arg_deref)]
    fn Start(&mut self, cmd: &rusty::RaftCommand, index: *mut u64,
             term: *mut u64) -> RaftStartResult {
        {
            let _lock = RaftLockGuard::new(&mut self.mtx_);
            if !self.IsLeaderLocked() {
                unsafe {
                    *index = 0;
                    *term = 0;
                }
                return RaftStartResult::REJECTED;
            }
            let mut copy: rusty::RaftCommand = Default::default();
            unsafe {
                raft_command_clone_into(cmd as *const rusty::RaftCommand,
                                        &mut copy as *mut rusty::RaftCommand);
            }
            let previous_index: u64 = self.AppendLocal(copy);
            // AppendLocal reports the OLD last index; Start reports the
            // index of the entry it just appended.
            assert!(
                self.core.raft_log_.last_index() == previous_index + 1);  // [move, M10]
            unsafe {
                *index = self.core.raft_log_.last_index();
                *term = self.core.current_term_;
                rusty::raft_log_debug_3("Start(): ldr={} index={} term={}",
                                        self.loc_id_, *index, *term);
            }
        }
        self.RequestReplication();
        RaftStartResult::APPENDED
    }

    // Inbound RPC, gate included. THE GATE IS THE POINT: service.cc used to
    // ask IsDisconnected and IsRpcReady across the ABI before every handler,
    // so each request cost three virtual-plus-FFI round trips. The replies
    // written on the unavailable path are byte-for-byte the ones service.cc
    // wrote; only where they are written changed.
    //
    // The order of the two reads matches the predicate it replaces --
    // `!has_server || disconnected || !rpc_ready` -- minus the null test,
    // which stays in C++ because a null server has no method to call.
    fn ServeVote(&mut self, lst_log_idx: u64, lst_log_term: i64,
                 can_id: u16, can_term: i64, reply_term: *mut i64,
                 vote_granted: *mut i8) {
        if self.IsDisconnected() || !self.IsRpcReady() {
            unsafe {
                *reply_term = can_term;
                *vote_granted = 0;
            }
            return;
        }
        self.OnRequestVote(lst_log_idx, lst_log_term, can_id, can_term,
                           reply_term, vote_granted);
    }

    fn ServeAppendEntries(&mut self, leader_current_term: u64,
                          leader_site_id: u16, leader_prev_log_index: u64,
                          leader_prev_log_term: u64, leader_commit_index: u64,
                          cmd: &rusty::RaftCommand, leader_next_log_term: u64,
                          follower_append_ok: *mut u64,
                          follower_current_term: *mut u64,
                          follower_last_log_index: *mut u64) {
        if self.IsDisconnected() || !self.IsRpcReady() {
            unsafe {
                *follower_append_ok = 0;
                *follower_current_term = 0;
                *follower_last_log_index = 0;
            }
            return;
        }
        self.OnAppendEntries(leader_current_term, leader_site_id,
                             leader_prev_log_index, leader_prev_log_term,
                             leader_commit_index, cmd, leader_next_log_term,
                             follower_append_ok, follower_current_term,
                             follower_last_log_index);
    }

    fn ServeInstallSnapshot(&mut self, term: u64, leader_id: u64,
                            last_included_index: u64, last_included_term: u64,
                            data: &rusty::RaftByteString,
                            term_out: *mut u64) {
        if self.IsDisconnected() || !self.IsRpcReady() {
            unsafe {
                *term_out = 0;
            }
            return;
        }
        self.OnInstallSnapshot(term, leader_id, last_included_index,
                               last_included_term, data, term_out);
    }

    // @unsafe - takes mtx_; the returned token is how a state machine proves
    // ownership when it later clears the callbacks. A RaftSpecific method so
    // an embedder (raft_bench, through the replication helper) can register
    // them; before, only the lab reached it, as an inherent method.
    fn SetStateMachineSnapshotCallbacks(
        &mut self,
        create_cb: &rusty::RaftCreateSnapshotCb,
        prepare_cb: &rusty::RaftPrepareSnapshotCb,
    ) -> u64 {
        let _lock = RaftLockGuard::new(&mut self.mtx_);
        if self.core.next_snapshot_callback_owner_token_ == 0 {
            self.core.next_snapshot_callback_owner_token_ = 1;
        }
        let owner_token: u64 = self.core.next_snapshot_callback_owner_token_;
        self.core.next_snapshot_callback_owner_token_ += 1;
        // In place, for the reason given on reg_learner_action.
        unsafe {
            raft_create_snapshot_cb_clone_into(
                create_cb as *const rusty::RaftCreateSnapshotCb,
                &mut self.create_sm_snapshot_cb_ as *mut rusty::RaftCreateSnapshotCb);
            raft_prepare_snapshot_cb_clone_into(
                prepare_cb as *const rusty::RaftPrepareSnapshotCb,
                &mut self.prepare_sm_snapshot_cb_ as *mut rusty::RaftPrepareSnapshotCb);
        }
        self.core.snapshot_callback_owner_token_ = owner_token;
        owner_token
    }
}

// The handlers themselves. Inherent, not trait: no C++ caller is left -- the
// service goes through Serve* above -- and the lab cases call
// OnInstallSnapshot directly on the struct.
#[allow(non_snake_case)]
#[allow(clippy::too_many_arguments)]
impl RaftServerBase {
    // The bodies are Rust -- on_request_vote_body and on_append_entries_body
    // in server_cc.rs, OnInstallSnapshotLocked above -- reached through the
    // raft_rpc_* kernels for the reasons given at their declaration.
    pub fn IsDisconnected(&self) -> bool {
        self.disconnected_.load(rusty::sync::atomic::Ordering::Acquire)
    }

    // not_unsafe_ptr_arg_deref: the out-parameters are fields of the reply
    // struct the caller owns for the whole call -- RpcVoteResponse and its
    // siblings in service.cc, or a stack slot in the lab cases. These were
    // exempt from the lint as trait-impl methods; the contract did not change
    // with the impl block.
    #[allow(clippy::not_unsafe_ptr_arg_deref)]
    pub fn OnRequestVote(&mut self, lst_log_idx: u64, lst_log_term: i64,
                     can_id: u16, can_term: i64, reply_term: *mut i64,
                     vote_granted: *mut i8) {
        // The body is the free function on_request_vote_body, below.
        on_request_vote_body(
            self, lst_log_idx, lst_log_term, can_id, can_term,
            unsafe { &mut *reply_term }, unsafe { &mut *vote_granted });
    }

    #[allow(clippy::not_unsafe_ptr_arg_deref)]
    pub fn OnAppendEntries(&mut self, leader_current_term: u64,
                       leader_site_id: u16, leader_prev_log_index: u64,
                       leader_prev_log_term: u64, leader_commit_index: u64,
                       cmd: &rusty::RaftCommand, leader_next_log_term: u64,
                       follower_append_ok: *mut u64,
                       follower_current_term: *mut u64,
                       follower_last_log_index: *mut u64) {
        // The payload crosses as the handle the body takes plus has_value() --
        // the only two things Raft asks of a janus::Command -- read here
        // through the one kernel that can look inside it.
        let cmd_has_value: bool =
            unsafe { raft_command_has_value(cmd as *const rusty::RaftCommand) };
        on_append_entries_body(
            self, leader_current_term, leader_site_id, leader_prev_log_index,
            leader_prev_log_term, leader_commit_index,
            cmd as *const rusty::RaftCommand as *const core::ffi::c_void,
            cmd_has_value, leader_next_log_term,
            unsafe { &mut *follower_append_ok },
            unsafe { &mut *follower_current_term },
            unsafe { &mut *follower_last_log_index });
    }

    #[allow(clippy::not_unsafe_ptr_arg_deref)]
    pub fn OnInstallSnapshot(&mut self, term: u64, leader_id: u64,
                         last_included_index: u64, last_included_term: u64,
                         data: &rusty::RaftByteString, term_out: *mut u64) {
        // Lock order: the state-machine apply gate, then mtx_ -- the order
        // the C++ handler took. Only the catch around the locked body is
        // still C++ (raft_install_snapshot_guarded); false is the throw.
        {
            let _apply_lock =
                RaftStdLockGuard::new(&mut self.state_machine_apply_mtx_);
            let _lock = RaftLockGuard::new(&mut self.mtx_);
            self.install_out_ = CoreOutput::new();  // [move, M3]
            let installed: bool = unsafe {
                raft_install_snapshot_guarded(
                    self.handle(), self.site_id_, term, leader_id,
                    last_included_index, last_included_term,
                    data as *const rusty::RaftByteString, term_out)
            };
            if !installed {
                self.FailStop();
                unsafe {
                    *term_out = 0;
                }
            }
            // [fix, F8] the install wrote the term, role and commit index
            self.publish_mirrors();
        }
        // [fix, F6] The role change's log entry and callback, both locks
        // released.
        let out: CoreOutput = core::mem::take(&mut self.install_out_);
        self.run_unlocked_actions(&out);
    }
}

// [move, M5] The election timer's gather as a core call: the timer state read
// in one scope at `now` (the shell's clock read), and whether the timeout
// fired. The caller holds mtx_. A free function after ElectionTick rather
// than a RaftCore method, so the transpiled C++ declares the type first.
pub fn raft_election_tick(core: &RaftCore, now: u64) -> ElectionTick {
    let heartbeat_time: u64 = core.last_heartbeat_time_;
    let time_elapsed: u64 = now - heartbeat_time;
    let election_timeout: u64 = core.election_timeout_us_;
    ElectionTick::new(
        time_elapsed,
        election_timeout,
        heartbeat_time,
        core.election_timer_generation_,
        core.current_term_,
        core.vote_for_,
        raft_server_election_timeout_has_fired(
            core.is_leader_, time_elapsed, election_timeout),
    )
}

// The election-timer fiber, owned by Rust.
//
// It sits after RaftServerBase rather than beside ElectionTick because it
// now holds a TYPED pointer to the server and calls its methods directly.
// Seven of the nine C++ kernels it used to go through are gone: the
// gather, the two logs, the two state reads, the random delay and the
// running flag are all RaftServerBase methods. The two that remain suspend
// on the wake gate and dispatch RequestVote, both still C++.
pub struct ElectionTimerLoop {
    server_: *mut RaftServerBase,
    wait_int_us_: u64,
}

impl ElectionTimerLoop {
    pub fn new(server: *mut RaftServerBase, wait_int_us: u64) -> ElectionTimerLoop {
        ElectionTimerLoop { server_: server, wait_int_us_: wait_int_us }
    }

    // The body of the fiber. Structurally identical to the C++ it replaces:
    // wait a randomised sub-interval, gather under the lock, and if the
    // timeout fired, campaign and then wait out the vote before looping.
    pub fn run(&self) {
        unsafe { (*self.server_).ElectionLoopLogStart() };
        while !unsafe { (*self.server_).ElectionLoopStopped() } {
            let delay = unsafe { (*self.server_).ElectionLoopRandomDelay() };
            // Unlike a plain sleep this is interrupted by shutdown, so a
            // false return means "stop", not "timed out".
            if !unsafe {
                (*self.server_).WaitForElectionTimeoutOrShutdown(delay)
            } {
                break;
            }
            let tick = unsafe { (*self.server_).ElectionLoopGather() };
            if tick.fired() {
                unsafe { (*self.server_).ElectionLoopLogFired(&tick) };
                // Re-check before campaigning: RequestVote reaches through a
                // vtable that a concurrent destructor may already have
                // collapsed.
                if unsafe { (*self.server_).ElectionLoopStopped() } {
                    break;
                }
                unsafe {
                    (*self.server_)
                        .RequestVoteFromElectionTimer(tick.generation())
                };
                if !self.await_vote_settled() {
                    break;
                }
            }
        }
        unsafe { (*self.server_).ElectionLoopSetRunning(false) };
    }

    // Returns true when voting finished normally, false when shutdown cut it
    // short. The C++ spelled both as `break` out of the inner loop and let the
    // outer `while (!stop_)` sort them out; naming the two outcomes is the one
    // place this reads differently from the original, and the observable
    // behaviour is the same.
    fn await_vote_settled(&self) -> bool {
        loop {
            if !unsafe { (*self.server_).ElectionLoopVoting() } {
                return true;
            }
            // The seam's fiber sleep, as in heartbeat_loop_body. Not
            // rusty::ReactorFiber::sleep: under rustc that is the facade's
            // recording model, which neither sleeps nor yields.
            unsafe { raft_fiber_sleep_us(self.wait_int_us_) };
            if unsafe { (*self.server_).ElectionLoopStopped() } {
                return false;
            }
        }
    }
}

// What is left of the C++ side: one wake-gate suspension.
#[allow(improper_ctypes)]
unsafe extern "C" {
}

// The inbound RPC bodies live here rather than in server_cc.rs, next to the
// OnRequestVote / OnAppendEntries methods that call them: server_cc imports
// this module, and a C++20 module graph may not be cyclic, so the transpiled
// C++ lane needs the callee on this side of the edge.
// ==========================================================================
// INBOUND RequestVote
// ==========================================================================

// OnRequestVote's whole body, as Rust. The caller holds mtx_ for the
// duration, exactly as the C++ did -- the lock stays in C++ because
// RaftCheckedMutex is a C++ type and because moving lock/unlock into a Rust
// body would lose RAII across this function's many early returns.
//
// [move, M5] A core call: it takes the core, and what it needs from the
// shell (stop_, failover_, raft_election_debug_enabled()) as parameters.
//
// The caller also passes `candidate_is_current_voter` rather than this
// reading current_config_: that member is a std::set that has not moved into
// the state struct, and computing the predicate on the C++ side keeps the
// rejection log line's level short-circuit where it belongs.
#[allow(clippy::too_many_arguments)]
pub fn raft_on_request_vote(
    core: &mut RaftCore,
    stopped: bool,
    candidate_is_current_voter: bool,
    lst_log_idx: u64,
    lst_log_term: i64,
    can_id: u16,
    can_term: i64,
    reply_term: &mut i64,
    vote_granted: &mut i8,
    failover: bool,  // [move, M5]
    election_debug: bool,  // [move, M5]
    out: &mut CoreOutput,  // [move, M3]
) {
    if stopped {
        *reply_term = core.current_term_ as i64;
        *vote_granted = 0;
        return;
    }

    if can_term < 0 || lst_log_term < 0 || !candidate_is_current_voter {
        *reply_term = core.current_term_ as i64;
        *vote_granted = 0;
        return;
    }

    let cur_term = core.current_term_;
    // UNSIGNED, deliberately. The C++ this replaces was
    // `if (can_term < cur_term)` with can_term an int64_t and cur_term a
    // uint64_t, and C++'s usual arithmetic conversions make that an UNSIGNED
    // comparison. Writing it as `can_term < cur_term as i64` instead -- the
    // obvious-looking translation -- is a different function: once
    // current_term_ passes INT64_MAX the cast goes negative, a non-negative
    // can_term is never below it, this rejection is skipped, and the
    // fall-through can GRANT a vote to a candidate whose term is far below
    // ours. current_term_ is a u64 taken straight off the wire with no
    // clamp, so that state is reachable from a peer.
    //
    // can_term >= 0 is already guaranteed above, so the cast to u64 is the
    // faithful spelling.
    if (can_term as u64) < cur_term {
        core.do_vote(lst_log_idx, lst_log_term, can_id, can_term,
                     reply_term, vote_granted, false, stopped, failover,
                     election_debug, out);
        return;
    }

    // Already voted for someone ELSE this term. Raft allows re-granting to
    // the same candidate, which is why the identity is compared and not just
    // the presence of a vote.
    // u64 here too, for the same reason and so the two comparisons cannot
    // drift apart. Equality happens to be unaffected by the signedness, but
    // relying on that is how the bug above got written.
    if (can_term as u64) == cur_term
        && core.vote_for_ != RAFT_SERVER_INVALID_SITE_ID
        && core.vote_for_ != can_id
    {
        core.do_vote(lst_log_idx, lst_log_term, can_id, can_term,
                     reply_term, vote_granted, false, stopped, failover,
                     election_debug, out);
        return;
    }

    // Every grant, including an idempotent retry, must still carry an
    // up-to-date candidate log. Defensive against damaged or legacy
    // persistent state, and the RequestVote rule in its direct form.
    // The last log index is not below the snapshot boundary.
    assert!(core.raft_log_.last_index() >= core.snapidx_);  // [move, M10]
    let lstoff = core.raft_log_.last_index() - core.snapidx_;
    let curlstterm = core.election_last_log_term();
    let curlstidx = core.raft_log_.last_index();
    let candidate_log_is_current = raft_server_candidate_log_is_at_least(
        lst_log_term, curlstterm, lst_log_idx, curlstidx);

    if raft_server_vote_is_idempotent(can_term as u64, cur_term,
                                      core.vote_for_, can_id)
        && candidate_log_is_current
    {
        core.do_vote(lst_log_idx, lst_log_term, can_id, can_term,
                     reply_term, vote_granted, true, stopped, failover,
                     election_debug, out);
        return;
    }

    // Snapshot-aware offset invariant.
    assert!(lstoff + core.snapidx_ == core.raft_log_.last_index());  // [move, M10]

    let grant = candidate_log_is_current;
    core.do_vote(lst_log_idx, lst_log_term, can_id, can_term,
                 reply_term, vote_granted, grant, stopped, failover,
                 election_debug, out);
}

// ==========================================================================
// The two inbound RPC bodies. Both keep a one-line C++ entry point, because
// the srpc service layer calls them by name on RaftServer.
// ==========================================================================
#[allow(clippy::too_many_arguments)]
// Called directly by RaftServerBase::OnRequestVote (server_h.rs).
pub fn on_request_vote_body(
    server: &mut RaftServerBase, lst_log_idx: u64,
    lst_log_term: i64, can_id: u16, can_term: i64,
    reply_term: &mut i64, vote_granted: &mut i8) {
    // [move, M3] The decision under mtx_ and its locked actions before the
    // guard drops; the leader-change callback after it ([fix, F6]).
    let mut out: CoreOutput = CoreOutput::new();
    {
        let _lock = RaftLockGuard::new(&mut server.mtx_);
        on_request_vote_locked(server, lst_log_idx, lst_log_term, can_id,
                               can_term, reply_term, vote_granted, &mut out);
        server.run_locked_actions(&out);
    }
    server.run_unlocked_actions(&out);
}

// [move, M3] on_request_vote_body's critical section. CALLER MUST HOLD mtx_.
#[allow(clippy::too_many_arguments)]
fn on_request_vote_locked(
    server: &mut RaftServerBase, lst_log_idx: u64,
    lst_log_term: i64, can_id: u16, can_term: i64,
    reply_term: &mut i64, vote_granted: &mut i8, out: &mut CoreOutput) {
    rusty::raft_log_debug_1("raft receives vote from candidate: {:x}", can_id);

    let stopped: bool =
        server.stop_.load(rusty::sync::atomic::Ordering::Acquire);
    if stopped {
        rusty::raft_log_debug_2(
            "[RAFT-SHUTDOWN] Site {} rejecting RequestVote from {}",
            server.site_id_, can_id);
    }

    let candidate_is_current_voter: bool =
        can_id != RAFT_SERVER_INVALID_SITE_ID && can_id != server.site_id_
            && server.IsConfigMember(can_id);
    if !stopped
        && (can_term < 0 || lst_log_term < 0 || !candidate_is_current_voter)
    {
        rusty::raft_log_warn_5(
            "[RAFT_VOTE] Site {} rejected malformed/non-voter candidate {} term {} last_log_term {} (voter={})",
            server.site_id_, can_id, can_term, lst_log_term,
            candidate_is_current_voter);
    }

    let election_debug: bool = unsafe { raft_election_debug_enabled() };
    raft_on_request_vote(&mut server.core, stopped, candidate_is_current_voter,
                         lst_log_idx, lst_log_term, can_id, can_term,
                         reply_term, vote_granted, server.failover_,
                         election_debug, out);  // [move, M5]
}

// ==========================================================================
// INBOUND AppendEntries
// ==========================================================================

// PHASE 0 of the heartbeat round, which is the whole locked section of
// RaftServer::HeartbeatPhase0.
//
// Everything it decides is now Rust: whether the round runs at all, whether
// the leader epoch changed under the fiber, whether the read-index round may
// advance, which members the round admits, and whether the majority-matched
// index may be committed. Every piece of state it touches was already a DSL
// type -- RaftCore, PeerTable, RaftLog, HeartbeatRoundScope,
// PendingTable, AuthorityLedger -- which is what made the phase convertible
// at all; this is the first body to be assembled out of them rather than
// alongside them.
//
// Three things stay in C++ on purpose, and they are the only three:
//   - taking mtx_, because a std::mutex has no Rust spelling here;
//   - EnqueueCommittedEntries, which is the apply queue, i.e. I/O. It is
//     driven by the range this returns, so the DECISION to commit is Rust
//     and only the hand-off is not;
//   - the Log_debug calls, which must keep their level short-circuit. A DSL
//     body logs through log_line, which evaluates unconditionally, and this
//     runs once per follower per round.
//
// Cross-carrier note: RaftCore, PeerTable and RaftLog live in
// server.h. Referencing a TYPE across carriers works exactly as referencing
// a free function does -- `use crate::server_h::X` on the Rust side, and the
// emitter writes the name unqualified, which resolves because both blocks
// sit in namespace janus. No shim namespace is needed for types.
// OnAppendEntries' body, as Rust. The caller holds mtx_ throughout.
//
// The wire payload never crosses. C++ decodes it once -- it is the only side
// that can, since janus::Command is opaque here -- and hands over three
// scalars plus the incoming entries' TERMS. Rust makes every protocol
// decision from those, and when it decides to append it calls back through
// raft_ae_apply_incoming, which builds the entries C++-side. That ordering
// keeps the original's laziness: entries are constructed only for a payload
// that is actually being written, not for one about to be rejected, which
// matters because backtracking rejects are common during log repair.
//
// AppendReport is diagnostics only. The caller acts on nothing in it; it
// exists so the two rejection log lines can keep their level short-circuit
// and still name which check failed.
#[repr(C)]
pub struct AppendReport {
    accepted_: bool,
    term_ok_: bool,
    index_ok_: bool,
    prev_term_ok_: bool,
    refused_committed_conflict_: bool,
    unauthoritative_: bool,
    conflict_index_: u64,
    local_prev_term_: u64,
}

impl AppendReport {
    pub fn accepted(&self) -> bool {
        self.accepted_
    }

    pub fn term_ok(&self) -> bool {
        self.term_ok_
    }

    pub fn index_ok(&self) -> bool {
        self.index_ok_
    }

    pub fn prev_term_ok(&self) -> bool {
        self.prev_term_ok_
    }

    // The append was refused because it would rewrite an entry at or below
    // commit_index_/execute_index_. A legitimate leader never does this.
    pub fn refused_committed_conflict(&self) -> bool {
        self.refused_committed_conflict_
    }

    // Rejected at the authoritative-sender gate, before term_ok, index_ok or
    // prev_term_ok were ever evaluated. The caller needs this to pick the
    // right log line: reporting those three as "failed" when they were never
    // computed is a lie, and it hides a distinct failure mode behind the
    // generic one.
    pub fn unauthoritative(&self) -> bool {
        self.unauthoritative_
    }

    pub fn conflict_index(&self) -> u64 {
        self.conflict_index_
    }

    pub fn local_prev_term(&self) -> u64 {
        self.local_prev_term_
    }
}

// [move, M5] A core call: the caller holds mtx_, and passes the payload as
// a WireBatch, the decode scratch, and the shell's failover_.
// unnecessary_unwrap: the per-slot lookup below uses is_some()/unwrap() with
// an explicit `&RaftEntry` binding rather than `if let`. `if let` is the
// better Rust and it transpiles, but the emitter renders the binding with a
// dot where the C++ needs an arrow, and an inferred binding COPIES the entry.
#[allow(clippy::too_many_arguments, clippy::unnecessary_unwrap)]
pub fn raft_on_append_entries(
    core: &mut RaftCore,
    wire: &WireBatch,  // [move, M5]
    decoded_terms: &mut rusty::Vec<i64>,  // [move, M5]
    stopped: bool,
    sender_is_current_voter: bool,
    has_cmd: bool,
    leader_current_term: u64,
    leader_site_id: u16,
    leader_prev_log_index: u64,
    leader_prev_log_term: u64,
    leader_commit_index: u64,
    follower_append_ok: &mut u64,
    follower_current_term: &mut u64,
    follower_last_log_index: &mut u64,
    failover: bool,  // [move, M5]
    out: &mut CoreOutput,  // [move, M3]
) -> AppendReport {
    let mut report = AppendReport {
        accepted_: false,
        term_ok_: false,
        index_ok_: false,
        prev_term_ok_: false,
        refused_committed_conflict_: false,
        unauthoritative_: false,
        conflict_index_: 0,
        local_prev_term_: 0,
    };

    if stopped {
        *follower_append_ok = 0;
        *follower_current_term = core.current_term_;
        *follower_last_log_index = core.raft_log_.last_index();
        return report;
    }

    let leader_has_higher_term =
        raft_server_observed_higher_term(leader_current_term, core.current_term_);
    let leader_term_is_stale =
        raft_server_vote_term_is_stale(leader_current_term, core.current_term_);
    let sender_is_self = leader_site_id == core.site_id_;
    let has_known_leader = core.current_leader_id_ != RAFT_SERVER_INVALID_SITE_ID;
    let known_leader_matches_sender = core.current_leader_id_ == leader_site_id;
    if !sender_is_current_voter
        || leader_term_is_stale
        || !raft_server_leader_rpc_sender_is_authoritative(
            leader_has_higher_term,
            core.is_leader_,
            sender_is_self,
            has_known_leader,
            known_leader_matches_sender,
        )
    {
        report.unauthoritative_ = true;
        *follower_append_ok = 0;
        *follower_current_term = core.current_term_;
        *follower_last_log_index = core.raft_log_.last_index();
        return report;
    }

    // Decode the wire payload HERE, not before the gates. The original did
    // the marshallable_cast and the count validation at exactly this point,
    // after a stopped server and an unauthoritative sender had already
    // returned. Hoisting it above them would make every rejected
    // AppendEntries pay a dynamic cast and N refcount bumps, on a path a
    // remote peer drives -- and backtracking rejects are common during log
    // repair.
    // decode_terms fills decoded_terms, one term per encoded entry, so its
    // length IS the decoded count.
    let append_payload_valid = wire.decode_terms(leader_prev_log_index,
                                                 decoded_terms);
    let decoded_count: u64 = decoded_terms.len() as u64;

    let term_ok =
        raft_server_append_term_is_acceptable(leader_current_term, core.current_term_);
    let compacted_prefix_miss = leader_prev_log_index != 0
        && leader_prev_log_index < core.raft_log_.base()
        && leader_prev_log_index != core.snapidx_;
    let index_ok =
        leader_prev_log_index <= core.raft_log_.last_index() && !compacted_prefix_miss;

    // THE LOG-MATCHING CHECK. A follower legitimately may not hold
    // leaderPrevLogIndex -- discovering that is the point, and what drives
    // the leader's backtracking. An absent entry falls through to term 0 and
    // the mismatch is reported rather than manufactured.
    let mut local_prev_term: u64 = 0;
    if leader_prev_log_index == 0 {
        local_prev_term = 0;
    } else if leader_prev_log_index == core.snapidx_ {
        // The snapshot boundary is still valid when entries are compacted.
        local_prev_term = core.snapterm_ as u64;
    } else if leader_prev_log_index <= core.raft_log_.last_index()
        && !compacted_prefix_miss
        && core.raft_log_.holds(leader_prev_log_index)
    {
        local_prev_term =
            core.raft_log_.get(leader_prev_log_index).unwrap().term() as u64;
    }
    let prev_term_ok = leader_prev_log_index == 0 || local_prev_term == leader_prev_log_term;

    report.term_ok_ = term_ok;
    report.index_ok_ = index_ok;
    report.prev_term_ok_ = prev_term_ok;
    report.local_prev_term_ = local_prev_term;

    // Reset the timer for any current-term leader, even when the log
    // conflicts, so a follower being repaired by backtracking does not keep
    // starting elections.
    if term_ok {
        if raft_server_observed_higher_term(leader_current_term, core.current_term_) {
            let prev_term = core.current_term_;
            core.current_term_ = leader_current_term;
            core.vote_for_ = RAFT_SERVER_INVALID_SITE_ID;
            // Publish the accepted leader before any leader-change callback
            // can observe the follower transition.
            core.current_leader_id_ = raft_server_leader_hint_after_transition(
                false, true, core.site_id_, leader_site_id);
            let now_term: u64 = core.current_term_;
            core.log_term_change("AppendEntries leader term is newer",
                                 prev_term, now_term, leader_site_id);
            // `stopped` is the caller's read of stop_, under this same lock.
            if core.is_leader_ {
                // The central transition, so no leadership state survives an
                // accepted competing leader epoch.
                core.step_down(stopped, failover, out);  // [move, M3]
            } else {
                core.set_is_leader(false, stopped, failover, out);  // [move, M3]
            }
            core.req_voting_ = false;
            core.election_in_progress_ = false;
        }
        // Refresh the hint for current-term contact too; a higher-term sender
        // was already published above, before its role transition.
        core.current_leader_id_ = raft_server_leader_hint_after_transition(
            false, true, core.site_id_, leader_site_id);
        out.push(CoreAction::reset_election(TimerResetReason::APPEND_ENTRIES));  // [move, M3]
    }

    if !(raft_server_append_is_acceptable(term_ok, index_ok, prev_term_ok)
         && append_payload_valid)
    {
        *follower_append_ok = 0;
        *follower_current_term = core.current_term_;
        *follower_last_log_index = core.raft_log_.last_index();
        return report;
    }

    // Any accepted leader RPC establishes follower state even in our current
    // term. Cancel an outstanding election before its delayed result can
    // promote this server after the accepted AppendEntries.
    if core.is_leader_ {
        core.step_down(stopped, failover, out);  // [move, M3]
    } else {
        core.set_is_leader(false, stopped, failover, out);  // [move, M3]
    }
    core.req_voting_ = false;
    core.election_in_progress_ = false;

    let old_last_log_index = core.raft_log_.last_index();
    let count = if has_cmd { decoded_count } else { 0 };
    let accepted_through = raft_server_append_sent_end(leader_prev_log_index, count);

    // Raft's conflict rule is deliberately narrower than "replace through the
    // RPC end". Concurrent RPCs can complete out of order: if an older
    // payload is already identical through its end, the follower must keep
    // any newer suffix it has since accepted. Only the first missing or
    // term-conflicting slot starts an overwrite.
    let mut have_first_write = false;
    let mut truncate_suffix = false;
    let mut first_write_index: u64 = 0;
    let mut i: u64 = 0;
    while i < decoded_count {
        let index = leader_prev_log_index + i + 1;
        // ONE lookup per entry, as the original had. The lookup itself is
        // Rust -- the log is a Rust type -- and only "does this slot hold a
        // payload" crosses, because janus::Command is opaque here.
        let mut local_exists: bool = false;
        let mut local_term: i64 = 0;
        let slot = core.raft_log_.get(index);
        if slot.is_some() {
            let entry: &RaftEntry = slot.unwrap();
            local_exists = entry.has_value();  // [move, M6]
            if local_exists {
                local_term = entry.term();
            }
        }
        // In bounds by construction: decoded_count IS decoded_terms_.len(),
        // the loop condition is i < decoded_count, and nothing in the body
        // touches the vector. The kernel this replaces needed a verify only
        // because i arrived across the language boundary.
        let incoming_term: i64 = decoded_terms[i as usize];
        if raft_server_append_entry_conflicts(local_exists, local_term as u64,
                                              incoming_term as u64) {
            have_first_write = true;
            first_write_index = index;
            truncate_suffix = index <= old_last_log_index;
            break;
        }
        i += 1;
    }

    if truncate_suffix
        && first_write_index <= (if core.commit_index_ > core.execute_index_ {
               core.commit_index_
           } else {
               core.execute_index_
           })
    {
        // A legitimate leader never conflicts with a committed entry. Do not
        // let malformed or internally inconsistent input rewrite applied
        // state; reject before memory changes.
        report.refused_committed_conflict_ = true;
        report.conflict_index_ = first_write_index;
        *follower_append_ok = 0;
        *follower_current_term = core.current_term_;
        *follower_last_log_index = core.raft_log_.last_index();
        return report;
    }

    if have_first_write {
        // Two operations that cannot leave a hole: drop the divergent suffix,
        // then re-append in index order. truncate_from is a no-op when
        // first_write_index is already past the tail, the ordinary extend
        // case. The append is Rust; only the per-entry reads of the wire
        // payload are kernels.
        core.raft_log_.truncate_from(first_write_index);
        wire.append_into(&mut core.raft_log_, leader_prev_log_index,
                         first_write_index);
    }
    // The append left the log tail where the result rule predicts.
    assert!(core.raft_log_.last_index()
        == raft_server_append_result_last_index(old_last_log_index, accepted_through,
                                                truncate_suffix));  // [move, M10]

    let follower_commit_candidate =
        raft_server_commit_index_clamp(leader_commit_index, accepted_through);
    if raft_server_log_index_above(follower_commit_candidate, core.commit_index_) {
        let old_commit = core.commit_index_;
        core.commit_index_ = follower_commit_candidate;
        // The commit index did not advance past the log tail.
        assert!(core.raft_log_.last_index() >= core.commit_index_);  // [move, M10]
        let new_commit: u64 = core.commit_index_;
        out.push(CoreAction::apply_range(old_commit, new_commit));  // [move, M3]
    }

    *follower_append_ok = 1;
    *follower_current_term = core.current_term_;
    // The inclusive end PROVED by this call, not the follower's possibly
    // longer and divergent local suffix. Rejections above report the local
    // tail instead, as a backoff hint.
    *follower_last_log_index = accepted_through;
    report.accepted_ = true;
    report
}

// `cmd` is an opaque handle to the caller's janus::Command; it is passed
// straight through to raft_on_append_entries, never dereferenced here.
#[allow(clippy::too_many_arguments, clippy::not_unsafe_ptr_arg_deref)]
// Called by RaftServerBase::OnAppendEntries (server_h.rs), likewise.
pub fn on_append_entries_body(
    server: &mut RaftServerBase,
                              leader_current_term: u64, leader_site_id: u16,
                              leader_prev_log_index: u64,
                              leader_prev_log_term: u64,
                              leader_commit_index: u64,
                              cmd: *const core::ffi::c_void,
                              cmd_has_value: bool,
                              leader_next_log_term: u64,
                              follower_append_ok: &mut u64,
                              follower_current_term: &mut u64,
                              follower_last_log_index: &mut u64) {
    // [move, M3] The decision under mtx_ and its locked actions before the
    // guard drops; the leader-change callback after it ([fix, F6]).
    let mut out: CoreOutput = CoreOutput::new();
    {
        let _lock = RaftLockGuard::new(&mut server.mtx_);
        on_append_entries_locked(server, leader_current_term, leader_site_id,
                                 leader_prev_log_index, leader_prev_log_term,
                                 leader_commit_index, cmd, cmd_has_value,
                                 leader_next_log_term, follower_append_ok,
                                 follower_current_term,
                                 follower_last_log_index, &mut out);
    }
    server.run_unlocked_actions(&out);
}

// [move, M3] on_append_entries_body's critical section. CALLER MUST HOLD
// mtx_. The locked actions run right after the decision, before the
// rejection log lines, where the effects used to sit.
#[allow(clippy::too_many_arguments, clippy::not_unsafe_ptr_arg_deref)]
fn on_append_entries_locked(
    server: &mut RaftServerBase,
    leader_current_term: u64, leader_site_id: u16,
    leader_prev_log_index: u64, leader_prev_log_term: u64,
    leader_commit_index: u64, cmd: *const core::ffi::c_void,
    cmd_has_value: bool, leader_next_log_term: u64,
    follower_append_ok: &mut u64, follower_current_term: &mut u64,
    follower_last_log_index: &mut u64, out: &mut CoreOutput) {
    let stopped: bool =
        server.stop_.load(rusty::sync::atomic::Ordering::Acquire);
    let sender_is_current_voter: bool =
        leader_site_id != RAFT_SERVER_INVALID_SITE_ID
            && leader_site_id != server.site_id_
            && server.IsConfigMember(leader_site_id);

    // NO DECODE HERE. raft_on_append_entries calls raft_ae_decode_payload
    // itself, after the stopped check and the authoritative gate -- which is
    // where the original did this work. Decoding up front would make every
    // rejected AppendEntries pay a dynamic cast and N refcount bumps on a
    // path a remote peer drives.
    let wire: WireBatch = WireBatch::new(cmd, cmd_has_value,
                                         leader_next_log_term);  // [move, M5]
    let report: AppendReport = raft_on_append_entries(
        &mut server.core, &wire, &mut server.decoded_terms_, stopped,
        sender_is_current_voter, cmd_has_value, leader_current_term,
        leader_site_id, leader_prev_log_index, leader_prev_log_term,
        leader_commit_index, follower_append_ok, follower_current_term,
        follower_last_log_index, server.failover_, out);
    server.run_locked_actions(out);  // [move, M3]

    if !stopped && !report.accepted() {
        if report.refused_committed_conflict() {
            // A legitimate leader can never conflict with a committed entry.
            rusty::raft_log_error_4(
                "[APPEND_REJECT] Site {} refusing conflict at committed index {} (commit_index={}, execute_index={})",
                server.site_id_, report.conflict_index(),
                server.core.commit_index_, server.core.execute_index_);
        } else if report.unauthoritative() {
            // Dispatch on WHICH gate rejected, not on
            // sender_is_current_voter. A stale-term or non-authoritative
            // sender that IS a current voter would otherwise land in the
            // branch below and be reported with term_ok/index_ok/
            // prev_term_ok all "failed" -- three checks never evaluated on
            // that path.
            rusty::raft_log_warn_6(
                "[APPEND_REJECT] Site {} rejecting unauthoritative AppendEntries sender {} term {} (local_term={} leader={} voter={})",
                server.site_id_, leader_site_id, leader_current_term,
                server.core.current_term_,
                server.core.current_leader_id_, sender_is_current_voter);
        } else {
            rusty::raft_log_info_10(
                "[APPEND_REJECT] Site {} rejecting AppendEntries from leader {} - term_ok={} index_ok={} prev_term_ok={} (leaderTerm={} myTerm={} prevIdx={} myLastIdx={} local_prev_term={})",
                server.site_id_, leader_site_id, report.term_ok(),
                report.index_ok(), report.prev_term_ok(), leader_current_term,
                server.core.current_term_, leader_prev_log_index,
                server.core.raft_log_.last_index(),
                report.local_prev_term());
        }
    }
}

