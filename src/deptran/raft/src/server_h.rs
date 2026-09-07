#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Eq, PartialEq))]
#[repr(i32)]
pub enum StepDownReason {
    UnsecuredFailure = 0,
    SecuredFailure = 1,
    HigherTerm = 2,
}

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

// TODO(stage2): allowed pending codegen check, NOT a decision. clippy wants
// `saturating_sub` here. The hand-written guard below is what production
// compiles today and the emitter is proven on that shape; `saturating_sub`
// may lower to a `rusty::` call a bare-rustc carrier cannot name. Verify the
// emitter output for it, apply the change on its own with the predicate
// reviewed, then remove this allow.
#[allow(clippy::implicit_saturating_sub)]
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

// Raft identifies replicas globally, but the client-routing View wire format
// identifies a replica by its locale within one partition. Keep the conversion
// decision in the Rust DSL; C++ supplies the validated remote lookup result.
pub const fn raft_server_view_leader_locale(leader_site: u16,
                                             self_site: u16,
                                             self_locale: i32,
                                             mapped_locale: i32) -> i32 {
    if leader_site == RAFT_SERVER_INVALID_SITE_ID {
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
                                               mapped_site: u16) -> u16 {
    if leader_locale < 0 {
        RAFT_SERVER_INVALID_SITE_ID
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
