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
#[repr(C)]
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
    fn raft_startup_wait(server: *mut RaftServerBase);
    fn raft_startup_notify_all(server: *mut RaftServerBase);
    fn raft_apply_thread_join(server: *mut RaftServerBase);
    fn raft_commo_set_network_enabled(server: *mut RaftServerBase, enabled: bool);
    fn raft_request_replication(server: *mut RaftServerBase);
    fn raft_set_local_append(server: *mut RaftServerBase,
                             cmd: *const rusty::RaftCommand,
                             term: *mut u64, index: *mut u64,
                             slot_id: u64, ballot: i64) -> RaftStartResult;
    fn raft_close_replication_wake_gate(server: *mut RaftServerBase);
    fn raft_spawn_election_timer(server: *mut RaftServerBase, wait_int_us: u64);
    fn raft_setup_internal_guarded(server: *mut RaftServerBase) -> bool;
    fn raft_shutdown_barrier_yield();
    fn raft_env_heartbeat_interval_us(out: *mut u64) -> i32;
    fn raft_env_log_retention_window(out: *mut u64) -> i32;
    fn raft_bind_replication_poll(server: *mut RaftServerBase) -> bool;
    fn raft_initialize_snapshot_manager(server: *mut RaftServerBase) -> bool;
    fn raft_load_current_config(server: *mut RaftServerBase) -> u64;
    fn raft_start_apply_thread(server: *mut RaftServerBase);
    fn raft_spawn_heartbeat_loop(server: *mut RaftServerBase);
    fn raft_spawn_election_timer_fiber(server: *mut RaftServerBase);
    fn raft_snapshot_serialize_and_save(server: *mut RaftServerBase,
                                        snap_index: u64,
                                        snap_term: i64) -> bool;
    fn raft_install_snapshot_payload(server: *mut RaftServerBase,
                                     last_included_index: u64,
                                     last_included_term: u64,
                                     data: *const rusty::RaftByteString) -> i32;
    fn raft_apply_invoke(server: *mut RaftServerBase, id: u64) -> bool;
    fn raft_monotonic_now_secs() -> u64;
    fn raft_thread_sleep_ms(millis: u64);
    fn raft_env_snapshots_enabled() -> bool;
    fn raft_prepare_snapshot_cb_is_set(server: *const RaftServerBase) -> bool;
    fn raft_env_snapshot_interval(out: *mut u64) -> i32;
    fn raft_snapshot_recovery_pick_manager(
        server: *mut RaftServerBase,
        out: *mut rusty::RaftSnapshotManagerPtr);
    fn raft_snapshot_manager_latest(
        manager: *const rusty::RaftSnapshotManagerPtr, index: *mut u64,
        term: *mut u64) -> bool;
    fn raft_snapshot_manager_load(
        manager: *const rusty::RaftSnapshotManagerPtr,
        data: *mut rusty::RaftByteString, index: *mut u64, term: *mut u64,
        size_bytes: *mut u64) -> bool;
    fn raft_load_state_machine_snapshot(
        server: *mut RaftServerBase, data: *const rusty::RaftByteString,
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
// apply_queue_epoch_, so an entry popped before the invalidation carries a
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
    pub peer_sites_: rusty::Vec<u16>,
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
    // The replica set for this partition, sorted and duplicate-free. It was
    // a std::set (current_config_) mirrored into this vector, because a
    // std::set is opaque to Rust; the set is gone and this is the only copy.
    // Sorted and duplicate-free is not cosmetic -- the round membership and
    // the authority ledger's set-equality check both rely on it -- so the
    // one writer (Setup) sorts and dedups before filling it.
    pub config_members_: rusty::Vec<u16>,
    pub apply_thread_: rusty::RaftStdThread,
    pub apply_thread_running_: rusty::sync::atomic::AtomicBool,
    pub state_machine_apply_mtx_: rusty::RaftStdMutex,
    pub apply_queue_mtx_: rusty::RaftStdMutex,
    pub apply_queue_epoch_: u64,
    pub apply_queue_: rusty::VecDeque<QueuedApplyEntry>,
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
    pub batch_buffer_: rusty::RaftBatchBuffer,
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
            peer_sites_: rusty::Vec::new(),
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
            config_members_: rusty::Vec::new(),
            apply_thread_: Default::default(),
            apply_thread_running_: rusty::sync::atomic::AtomicBool::new(false),
            state_machine_apply_mtx_: Default::default(),
            apply_queue_mtx_: Default::default(),
            apply_queue_epoch_: 0,
            apply_queue_: rusty::VecDeque::new(),
            pending_apply_command_: Default::default(),
            batch_buffer_: Default::default(),
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
        self.state_.peers_.reset(followers, next_index);
        // The C++ computed this as set.size() minus set.count(self); the
        // membership flag above is the same statement over a sorted vector.
        let expected: usize = if self_is_a_member {
            self.config_members_.len() - 1
        } else {
            self.config_members_.len()
        };
        unsafe {
            raft_verify(self.state_.peers_.len() == expected);
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
                let site: u16 = self.peer_site_at(ord);
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

    // @safe - the site id at an ordinal of the peer table.
    pub fn peer_site_at(&self, ordinal: usize) -> u16 {
        self.peer_sites_[ordinal]
    }

    // @unsafe - the owner-thread startup job. Every failure path closes the
    // server fail-closed rather than starting half a replica.
    pub fn SetupInternal(&mut self) -> bool {
        // RPC services may already be listening when this job begins. Keep
        // every handler fail-closed until snapshot loading has completed.
        self.rpc_ready_
            .store(false, rusty::sync::atomic::Ordering::Release);

        // Record startup time for the grace-period logic.
        self.startup_timestamp_ = unsafe { raft_time_now_us() };

        let mut hb_override: u64 = 0;
        let hb_status: i32 =
            unsafe {
                raft_env_heartbeat_interval_us(&mut hb_override as *mut u64)
            };
        if hb_status == 2 {
            self.FailClosed();
            return false;
        }
        if hb_status == 1 {
            self.heartbeat_interval_us_ = hb_override;
            rusty::raft_log_info_1(
                "[RAFT] Heartbeat interval set to {} us from env",
                self.heartbeat_interval_us_);
        }

        if !unsafe { raft_bind_replication_poll(self as *mut RaftServerBase) }
        {
            rusty::raft_log_error_1(
                "[RAFT-WAKE] Site {} has no PollThread owner during Setup",
                self.site_id_);
            self.FailClosed();
            return false;
        }

        let mut lrw_override: u64 = 0;
        let lrw_status: i32 =
            unsafe {
                raft_env_log_retention_window(&mut lrw_override as *mut u64)
            };
        if lrw_status == 2 {
            self.FailClosed();
            return false;
        }
        if lrw_status == 1 {
            self.log_retention_window_ =
                raft_server_retention_window_normalize(lrw_override);
            rusty::raft_log_info_1(
                "[RAFT] Log retention window set to {} from env",
                self.log_retention_window_);
        }

        if !unsafe {
            raft_initialize_snapshot_manager(self as *mut RaftServerBase)
        } {
            rusty::raft_log_error_1(
                "[RAFT-SNAPSHOT] Site {} cannot start after snapshot recovery failure",
                self.site_id_);
            self.FailClosed();
            return false;
        }

        let replicas: u64 =
            unsafe { raft_load_current_config(self as *mut RaftServerBase) };
        rusty::raft_log_info_3(
            "[RAFT-CONFIG] Initialized current_config_ for site {} partition {} with {} replicas",
            self.site_id_, self.partition_id_, replicas);

        unsafe {
            raft_start_apply_thread(self as *mut RaftServerBase);
        }
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
                raft_spawn_heartbeat_loop(self as *mut RaftServerBase);
            }
            if self.failover_ {
                self.election_loop_running_
                    .store(true, rusty::sync::atomic::Ordering::Release);
                unsafe {
                    raft_spawn_election_timer_fiber(
                        self as *mut RaftServerBase);
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
        let orphaned_compacted_suffix: bool = self.state_.snapidx_ == 0
            && !self.state_.raft_log_.is_empty()
            && self.state_.raft_log_.base() > 1;
        let uncovered_empty_progress: bool = self.state_.snapidx_ == 0
            && self.state_.raft_log_.is_empty()
            && self.state_.commit_index_ != 0;
        orphaned_compacted_suffix || uncovered_empty_progress
    }

    // @unsafe - CALLER HOLDS NOTHING; takes the apply gate and mtx_ in that
    // order. Wrapped by RaftServer::InitializeSnapshotManager's catch-all.
    //
    // Restores the exact state-machine bytes before publishing any recovered
    // snapshot boundary, so a failure anywhere leaves the live state machine,
    // the latest Raft snapshot and the reconstruction log untouched.
    pub fn InitializeSnapshotManagerLocked(&mut self) -> bool {
        if !unsafe { raft_env_snapshots_enabled() } {
            let _lock = RaftLockGuard::new(&mut self.mtx_);
            if self.HasUncoveredProgress() {
                let first: u64 = if self.state_.raft_log_.is_empty() {
                    0
                } else {
                    self.state_.raft_log_.base()
                };
                rusty::raft_log_error_3(
                    "[RAFT-SNAPSHOT] Site {} has recovered progress without its covering snapshot (first={} commit={}); snapshots are disabled",
                    self.site_id_, first, self.state_.commit_index_);
                self.FailStop();
                return false;
            }
            rusty::raft_log_info_1(
                "[RAFT-SNAPSHOT] Snapshots disabled for site {} (set MAKO_RAFT_SNAPSHOTS=1 to enable)",
                self.site_id_);
            return true;
        }

        let mut snapshot_interval: u64 = self.GetSnapshotThreshold();
        let mut interval_override: u64 = 0;
        let interval_status: i32 = unsafe {
            raft_env_snapshot_interval(&mut interval_override as *mut u64)
        };
        if interval_status == 2 {
            return false;
        }
        if interval_status == 1 {
            snapshot_interval = interval_override;
            self.SetSnapshotThreshold(snapshot_interval);
        }

        let _apply_lock =
            RaftStdLockGuard::new(&mut self.state_machine_apply_mtx_);
        let _lock = RaftLockGuard::new(&mut self.mtx_);

        let mut manager: rusty::RaftSnapshotManagerPtr = Default::default();
        unsafe {
            raft_snapshot_recovery_pick_manager(
                self as *mut RaftServerBase,
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
            if self.state_.snapidx_ != 0 || self.HasUncoveredProgress() {
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
        if recovered_snapshot_index < self.state_.snapidx_
            || (recovered_snapshot_index == self.state_.snapidx_
                && self.state_.snapidx_ != 0
                && recovered_snapshot_term != self.state_.snapterm_ as u64)
        {
            return self.FailSnapshotRecovery(
                "snapshot manager would move the live boundary backward or change its term");
        }
        let has_prepare_cb: bool =
            unsafe { raft_prepare_snapshot_cb_is_set(self as *const RaftServerBase) };
        if has_prepare_cb && self.GetAppliedIndex() > recovered_snapshot_index
        {
            return self.FailSnapshotRecovery(
                "refusing to rewind a live state machine to an older snapshot");
        }

        let previous_snapshot_index: u64 = self.state_.snapidx_;
        let previous_snapshot_term: u64 = self.state_.snapterm_ as u64;
        let previous_last_log_index: u64 = self.state_.raft_log_.last_index();
        let previous_min_active_slot: u64 = self.state_.raft_log_.base();

        // Reconstruct Figure 13's suffix decision from the old boundary when
        // it is still present. A live reinitialisation would use its exact
        // existing snapshot tuple as the same proof; that proof is
        // unreachable today because Setup is the only caller and snapidx_ is
        // still 0 there.
        let boundary = self.state_.raft_log_.get(recovered_snapshot_index);
        #[allow(clippy::unnecessary_unwrap)]
        let has_boundary: bool = boundary.is_some()
            && unsafe {
                raft_command_has_value(
                    boundary.unwrap().cmd() as *const rusty::RaftCommand)
            };
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
                self as *mut RaftServerBase,
                &snapshot_data as *const rusty::RaftByteString,
                recovered_snapshot_index, recovered_snapshot_term)
        } {
            return self.FailSnapshotRecovery(
                "state-machine snapshot validation/load failed");
        }

        self.state_.snapidx_ = recovered_snapshot_index;
        self.state_.snapterm_ = recovered_snapshot_term as i64;
        if retain_suffix {
            self.state_.raft_log_.compact_through(self.state_.snapidx_);
        } else {
            self.state_.raft_log_.reset(self.state_.snapidx_ + 1);
        }
        self.state_.commit_index_ = raft_server_snapshot_progress_clamp(
            self.state_.commit_index_, self.state_.snapidx_,
            self.state_.raft_log_.last_index());

        if self.state_.current_term_ < self.state_.snapterm_ as u64 {
            rusty::raft_log_warn_3(
                "[RAFT-SNAPSHOT] Site {} advancing recovered term {} -> {} to cover snapshot boundary",
                self.site_id_, self.state_.current_term_,
                self.state_.snapterm_);
            self.state_.current_term_ = self.state_.snapterm_ as u64;
            self.state_.vote_for_ = RAFT_SERVER_INVALID_SITE_ID;
        }

        unsafe {
            raft_verify(
                self.state_.commit_index_
                    <= self.state_.raft_log_.last_index());
        }

        self.snapshot_manager_ = manager;
        self.snapshot_manager_configured_
            .store(true, rusty::sync::atomic::Ordering::Release);
        self.snapshot_trigger_index_
            .store(self.state_.snapidx_,
                   rusty::sync::atomic::Ordering::Release);

        if self.state_.snapidx_ > self.GetAppliedIndex() {
            self.PublishAppliedIndexLocked(self.state_.snapidx_);
        }

        rusty::raft_log_info_8(
            "[RAFT-SNAPSHOT] Restored snapshot for site {}: index={} term={} size={} commit={} last={} min_active={} retain_suffix={}",
            self.site_id_, self.state_.snapidx_, self.state_.snapterm_,
            snapshot_size_bytes, self.state_.commit_index_,
            self.state_.raft_log_.last_index(),
            self.state_.raft_log_.base(), retain_suffix);
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
        let _lock = RaftLockGuard::new(&mut self.mtx_);
        if raft_server_observed_higher_term(follower_term,
                                            self.state_.current_term_) {
            rusty::raft_log_info_4(
                "[HEARTBEAT-SNAPSHOT] Site {}: Follower {} has higher term {} > {}, stepping down",
                self.site_id_, site_id, follower_term,
                self.state_.current_term_);
            let previous_term: u64 = self.state_.current_term_;
            self.state_.current_term_ = follower_term;
            self.state_.vote_for_ = RAFT_SERVER_INVALID_SITE_ID;
            self.LogTermChange("InstallSnapshot reply carried newer term",
                               previous_term, self.state_.current_term_,
                               site_id);
            // A follower's higher term does not identify the leader of that
            // term. Retire the previous leader hint before publishing
            // follower state.
            self.state_.current_leader_id_ =
                raft_server_leader_hint_after_transition(
                    false, false, self.site_id_, site_id);
            self.stepDown();
            self.state_.req_voting_ = false;
            self.state_.election_in_progress_ = false;
            return;
        }
        if self.state_.current_term_ != send_term {
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
        self.state_.peers_.accept_through(ord, snap_last_idx, has_successor,
                                          next_index);
        rusty::raft_log_info_4(
            "[HEARTBEAT-SNAPSHOT] Site {}: Updated follower {}: next_index={} match_index={}",
            self.site_id_, site_id, self.state_.peers_.next_index(ord),
            self.state_.peers_.match_index(ord));
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
    pub fn OnInstallSnapshotLocked(&mut self, term: u64, leader_id: u64,
                                   last_included_index: u64,
                                   last_included_term: u64,
                                   data: *const rusty::RaftByteString,
                                   term_out: &mut u64) {
        *term_out = 0;

        // Edge case 0: the server is shutting down.
        if self.stop_.load(rusty::sync::atomic::Ordering::Acquire) {
            rusty::raft_log_info_1(
                "[INSTALL-SNAPSHOT] Site {}: Ignoring InstallSnapshot - server shutting down",
                self.site_id_);
            return;
        }

        // Edge case 1: a stale term is rejected.
        if term < self.state_.current_term_ {
            rusty::raft_log_info_4(
                "[INSTALL-SNAPSHOT] Site {}: Rejecting InstallSnapshot from leader {} (leader_term={} < my_term={})",
                self.site_id_, leader_id, term, self.state_.current_term_);
            *term_out = self.state_.current_term_;
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
            raft_server_observed_higher_term(term, self.state_.current_term_);
        let sender_is_self: bool = leader_site == self.site_id_;
        let has_known_leader: bool =
            self.state_.current_leader_id_ != RAFT_SERVER_INVALID_SITE_ID;
        let known_leader_matches_sender: bool =
            self.state_.current_leader_id_ == leader_site;
        if !sender_is_current_voter
            || !raft_server_leader_rpc_sender_is_authoritative(
                leader_has_higher_term, self.state_.is_leader_,
                sender_is_self, has_known_leader,
                known_leader_matches_sender)
        {
            rusty::raft_log_warn_7(
                "[INSTALL-SNAPSHOT] Site {} rejected unauthoritative leader {} in term {} (local_term={} leader={} known_leader={} voter={})",
                self.site_id_, leader_id, term, self.state_.current_term_,
                self.state_.is_leader_, self.state_.current_leader_id_,
                sender_is_current_voter);
            return;
        }

        // Edge case 2: a higher or equal term is accepted as a legitimate
        // leader.
        let previous_term: u64 = self.state_.current_term_;
        if leader_has_higher_term {
            rusty::raft_log_info_4(
                "[INSTALL-SNAPSHOT] Site {}: Leader {} has higher term ({} > {}) - updating",
                self.site_id_, leader_id, term, self.state_.current_term_);
            self.state_.current_term_ = term;
            self.state_.vote_for_ = RAFT_SERVER_INVALID_SITE_ID;
        }

        // InstallSnapshot comes from a known leader. Publish its identity
        // before a possible leader-to-follower callback observes the role
        // transition.
        self.state_.current_leader_id_ =
            raft_server_leader_hint_after_transition(false, true,
                                                     self.site_id_,
                                                     leader_site);

        // Any accepted leader RPC, including one in our current term,
        // establishes follower state. Cancel the outstanding election as
        // well as leadership; RequestVote's delayed-success path
        // revalidates this ownership before it can promote again.
        if self.state_.is_leader_ {
            self.stepDown();
        } else {
            self.setIsLeader(false);
        }
        self.state_.req_voting_ = false;
        self.state_.election_in_progress_ = false;

        if leader_has_higher_term {
            self.LogTermChange("InstallSnapshot carried newer term",
                               previous_term, self.state_.current_term_,
                               leader_site);
        }

        // Legitimate leader contact.
        self.resetTimerLocked("received InstallSnapshot");
        // From here current_term_ denotes an accepted current-term leader
        // contact; individual install failures overwrite it with zero.
        *term_out = self.state_.current_term_;

        // A current-term leader may retry a snapshot after this follower has
        // already committed, applied or snapshotted through its boundary.
        // Acknowledge the contact, but roll no local state backward and do
        // not install the stale payload.
        let mut local_progress_index: u64 = self.state_.commit_index_;
        if self.state_.execute_index_ > local_progress_index {
            local_progress_index = self.state_.execute_index_;
        }
        let applied: u64 = self.GetAppliedIndex();
        if applied > local_progress_index {
            local_progress_index = applied;
        }
        if self.state_.snapidx_ > local_progress_index {
            local_progress_index = self.state_.snapidx_;
        }
        if last_included_index == self.state_.snapidx_
            && self.state_.snapidx_ != 0
            && last_included_term != self.state_.snapterm_ as u64
        {
            rusty::raft_log_error_5(
                "[INSTALL-SNAPSHOT] Site {}: rejecting snapshot boundary ({}, {}) that conflicts with local snapshot ({}, {})",
                self.site_id_, last_included_index, last_included_term,
                self.state_.snapidx_, self.state_.snapterm_);
            *term_out = 0;
            return;
        }
        if raft_server_snapshot_is_stale(last_included_index,
                                         local_progress_index) {
            rusty::raft_log_info_6(
                "[INSTALL-SNAPSHOT] Site {}: Snapshot index {} is already covered (commit={} execute={} applied={} snapidx={}); acknowledging no-op",
                self.site_id_, last_included_index,
                self.state_.commit_index_, self.state_.execute_index_,
                self.GetAppliedIndex(), self.state_.snapidx_);
            return;
        }
        if !raft_server_log_index_has_successor(last_included_index) {
            // The log's base requires S + 1. Reaching UINT64_MAX exhausts
            // the index space, so reject rather than wrap.
            rusty::raft_log_error_2(
                "[INSTALL-SNAPSHOT] Site {}: Cannot install terminal snapshot index {}; no successor index is representable",
                self.site_id_, last_included_index);
            *term_out = 0;
            return;
        }

        let configured: bool =
            unsafe { raft_snapshot_manager_is_set(&self.snapshot_manager_) };
        if !configured {
            rusty::raft_log_error_2(
                "[INSTALL-SNAPSHOT] Site {}: Cannot install snapshot at index {} without configured snapshot storage",
                self.site_id_, last_included_index);
            *term_out = 0;
            return;
        }

        // Complete every fallible observation the retention decision uses
        // BEFORE the application loader can replace external state. A
        // decoded command is required, so a synthesized empty RaftEntry can
        // never prove the snapshot boundary.
        let boundary = self.state_.raft_log_.get(last_included_index);
        #[allow(clippy::unnecessary_unwrap)]
        let has_boundary: bool = boundary.is_some()
            && unsafe {
                raft_command_has_value(
                    boundary.unwrap().cmd() as *const rusty::RaftCommand)
            };
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
            raft_install_snapshot_payload(self as *mut RaftServerBase,
                                          last_included_index,
                                          last_included_term, data)
        };
        if install == 2 {
            self.FailStop();
            *term_out = 0;
            return;
        }
        if install != 3 {
            *term_out = 0;
            return;
        }

        self.state_.snapidx_ = last_included_index;
        self.state_.snapterm_ = last_included_term as i64;
        self.snapshot_trigger_index_
            .store(self.state_.snapidx_,
                   rusty::sync::atomic::Ordering::Release);

        // Reconcile the in-memory log and the queued application work.
        if retain_suffix {
            self.state_.raft_log_.compact_through(last_included_index);
        } else {
            self.state_.raft_log_.reset(last_included_index + 1);
        }
        let mut purged_apply_entries: u64 = 0;
        {
            let _queue_lock =
                RaftStdLockGuard::new(&mut self.apply_queue_mtx_);
            if retain_suffix {
                // Rotate-filter: pop every entry once and push the survivors
                // back, which keeps their order without a second container.
                let examined: usize = self.apply_queue_.len();
                let mut seen: usize = 0;
                while seen < examined {
                    let entry = self.apply_queue_.pop_front().unwrap();
                    if raft_server_log_index_at_or_below(
                        entry.index_, last_included_index) {
                        purged_apply_entries += 1;
                    } else {
                        self.apply_queue_.push_back(entry);
                    }
                    seen += 1;
                }
            } else {
                // A conflicting snapshot invalidates the whole queue,
                // including an entry the apply thread has already popped --
                // it rechecks this epoch under the state-machine gate before
                // invoking the callback.
                self.apply_queue_epoch_ += 1;
                purged_apply_entries = self.apply_queue_.len() as u64;
                self.apply_queue_.clear();
            }
        }

        self.state_.commit_index_ = last_included_index;
        unsafe {
            raft_verify(
                self.state_.commit_index_
                    <= self.state_.raft_log_.last_index());
        }

        // Publish application only after the state machine has finished
        // loading. Acquire waiters must never observe the covered indices
        // early.
        self.PublishAppliedIndexLocked(last_included_index);

        rusty::raft_log_info_9(
            "[INSTALL-SNAPSHOT] Site {}: Installed snapshot from leader {} (snapidx={}, snapterm={}, state_.commit_index_={}, state_.execute_index_={}, state_.raft_log_.last_index()={}, retain_suffix={}, purged_apply={})",
            self.site_id_, leader_id, self.state_.snapidx_,
            self.state_.snapterm_, self.state_.commit_index_,
            self.state_.execute_index_, self.state_.raft_log_.last_index(),
            retain_suffix, purged_apply_entries);
    }

    // @unsafe - the background apply thread's loop. Runs on its own
    // std::thread, which RaftServer::StartApplyThread spawns.
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
                let _queue_lock =
                    RaftStdLockGuard::new(&mut self.apply_queue_mtx_);
                let size_before: u64 = self.apply_queue_.len() as u64;
                if !self.apply_queue_.is_empty() {
                    let mut entry = self.apply_queue_.pop_front().unwrap();
                    id = entry.index_;
                    entry_epoch = entry.epoch_;
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
                        self.state_.commit_index_
                    };
                    rusty::raft_log_info_5(
                        "[APPLY-THREAD] Site {}: IDLE state_.execute_index_={} state_.commit_index_={} queue_size={} applied_total={}",
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
                let current_epoch: u64 = {
                    let _queue_lock =
                        RaftStdLockGuard::new(&mut self.apply_queue_mtx_);
                    self.apply_queue_epoch_
                };
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
                        raft_apply_invoke(self as *mut RaftServerBase, id)
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
                    "[APPLY-THREAD] Site {}: applied {} entries, state_.execute_index_={} queue_remaining={}",
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

        let snap_index: u64 = self.state_.execute_index_;
        if snap_index == 0 {
            rusty::raft_log_debug_1(
                "[RAFT-SNAPSHOT] Site {}: state_.execute_index_ is 0, nothing to snapshot",
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
                                                   self.state_.snapidx_) {
            // The boundary entry is intentionally absent after compaction.
            // Its term is carried by snapshot metadata; do not recreate the
            // entry or rewind the log's base by appending it again.
            snap_term = self.state_.snapterm_;
        } else {
            let instance = self.state_.raft_log_.get(snap_index);
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
            raft_snapshot_serialize_and_save(self as *mut RaftServerBase,
                                             snap_index, snap_term)
        } {
            return false;
        }

        let old_snapidx: u64 = self.state_.snapidx_;
        self.state_.snapidx_ = snap_index;
        self.state_.snapterm_ = snap_term;
        self.snapshot_trigger_index_
            .store(self.state_.snapidx_,
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

    // @safe - `current_config_.count(site) != 0`, over the cached vector.
    // A linear scan of three to five sorted u16s, which is what the std::set
    // lookup it replaces cost anyway.
    pub fn IsConfigMember(&self, site: u16) -> bool {
        let mut i: usize = 0;
        while i < self.config_members_.len() {
            if self.config_members_[i] == site {
                return true;
            }
            i += 1;
        }
        false
    }

    // @safe - linear scan of a fixed, tiny table (replica counts are 3 or 5).
    // Returns state_.peers_.len() when the site is not a follower of this
    // leader, which is the "removed follower" case PHASE 2 guards against.
    // Deliberately an ordinal rather than a reference: an ordinal cannot
    // dangle across an RPC send or a re-entrant completion callback.
    pub fn PeerOrdinal(&self, site: u16) -> usize {
        let mut ord: usize = 0;
        while ord < self.peer_sites_.len() {
            if self.peer_sites_[ord] == site {
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
        self.CreateSnapshotLocked();
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
    // ONE PASS, TWO CRITICAL SECTIONS. The scan stops at the first gap and
    // lifts each usable command out of the log as it goes, because the log
    // must not be read while apply_queue_mtx_ is held -- the reason this used
    // to be split between a Rust scan and a C++ push kernel. Copying a
    // Command is a refcount bump on its inner Arc, not a payload copy.
    pub fn EnqueueCommittedEntries(&mut self, old_commit: u64,
                                   new_commit: u64) {
        let mut batch: rusty::VecDeque<QueuedApplyEntry> =
            rusty::VecDeque::new();
        let mut first_missing: u64 = 0;
        let mut id: u64 = old_commit + 1;
        while id <= new_commit {
            let found = self.state_.raft_log_.get(id);
            if found.is_none() {
                first_missing = id;
                break;
            }
            let entry: &RaftEntry = found.unwrap();
            let usable: bool = unsafe {
                raft_command_has_value(
                    entry.cmd() as *const rusty::RaftCommand)
            };
            if !usable {
                first_missing = id;
                break;
            }
            // epoch_ is stamped below, under the mutex that owns it.
            batch.push_back(QueuedApplyEntry {
                index_: id,
                command_: entry.cmd().clone(),
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
            let _queue_lock =
                RaftStdLockGuard::new(&mut self.apply_queue_mtx_);
            let epoch: u64 = self.apply_queue_epoch_;
            while !batch.is_empty() {
                let mut queued = batch.pop_front().unwrap();
                queued.epoch_ = epoch;
                self.apply_queue_.push_back(queued);
            }
            qsize = self.apply_queue_.len() as u64;
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
            if !unsafe { raft_election_wait(self.server_, delay) } {
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
            rusty::ReactorFiber::sleep(self.wait_int_us_);
            if unsafe { (*self.server_).ElectionLoopStopped() } {
                return false;
            }
        }
    }
}

// What is left of the C++ side: one wake-gate suspension.
#[allow(improper_ctypes)]
unsafe extern "C" {
    fn raft_election_wait(server: *mut RaftServerBase, timeout_us: u64) -> bool;
}

pub struct HeartbeatDriver {
    server_: *mut RaftServerBase,
    round_: *mut core::ffi::c_void,
}

impl HeartbeatDriver {
    pub fn new(server: *mut RaftServerBase,
               round: *mut core::ffi::c_void) -> HeartbeatDriver {
        HeartbeatDriver { server_: server, round_: round }
    }

    // decide -> emit -> collect -> decide, which is the shape the C++
    // already had as four comment-delimited phases. It is now the shape of
    // the Rust that sequences them.
    pub fn run(&self) {
        unsafe { (*self.server_).HeartbeatPrologue() };
        while unsafe { (*self.server_).HeartbeatLooping() } {
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
        unsafe { (*self.server_).HeartbeatEpilogue() };
    }
}

// What is left of the C++ half. The prologue, the looping check and the
// epilogue used to be here too; they are RaftServerBase methods now, so the
// driver calls them directly. The four that remain take the round-carried
// state, which is hand-written C++ this block cannot name.
#[allow(improper_ctypes)]
unsafe extern "C" {
    fn raft_heartbeat_wait(server: *mut RaftServerBase) -> bool;
    fn raft_heartbeat_phase0(server: *mut RaftServerBase,
                             round: *mut core::ffi::c_void) -> bool;
    fn raft_heartbeat_phase1(server: *mut RaftServerBase,
                             round: *mut core::ffi::c_void);
    fn raft_heartbeat_phase2(server: *mut RaftServerBase,
                             round: *mut core::ffi::c_void);
    fn raft_heartbeat_phase3(server: *mut RaftServerBase,
                             round: *mut core::ffi::c_void);
}
