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
    is_leader_: bool,
    req_voting_: bool,
    election_in_progress_: bool,
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
    execute_index_: u64,
    // Snapshot boundary.
    snapidx_: u64,
    snapterm_: i64,
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
