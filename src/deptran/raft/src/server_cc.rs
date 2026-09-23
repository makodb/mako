// Canonical Rust source of the Raft server's second half (plan.md, F2.6): the
// C ABI exports, the RPC bodies and the heartbeat phases. rustc compiles this
// into libraft.a; nothing here is transpiled. It began as the extraction of
// server.cc's inline blocks and is edited here now.

#[allow(dead_code, non_snake_case)]
fn IsPreferredLeaderConfigured(preferred_leader_site_id: u16) -> bool {
    preferred_leader_site_id != u16::MAX
}

pub struct PendingAppend {
    follower_: u16,
    sent_term_: u64,
    sent_round_: u64,
    // Inclusive end of the exact prefix proved by this RPC's wire payload. A
    // heartbeat proves only prevLogIndex; raw and batched payloads extend it
    // by their encoded entry count.
    sent_end_index_: u64,
    response_: rusty::RaftResponsePtr,
    // Empty Command (has_value() == false) signals a heartbeat.
    cmd_: rusty::RaftCommand,
}

impl PendingAppend {
    pub fn new(follower: u16, sent_term: u64, sent_round: u64,
               sent_end_index: u64, response: rusty::RaftResponsePtr,
               cmd: rusty::RaftCommand) -> PendingAppend {
        PendingAppend {
            follower_: follower,
            sent_term_: sent_term,
            sent_round_: sent_round,
            sent_end_index_: sent_end_index,
            response_: response,
            cmd_: cmd,
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

    // Both of these hand a carried C++ value back to C++. The reference is
    // safe because the method is &self: the emitter binds the const unwrap
    // overload, which returns a reference into the live Option rather than a
    // moved-out temporary.
    // Callers check occupied() first; unwrap is the assertion of that.
    pub fn response(&self, ordinal: usize) -> &rusty::RaftResponsePtr {
        &self.slots_[ordinal].as_ref().unwrap().response_
    }

    // Callers check occupied() first; unwrap is the assertion of that.
    pub fn cmd(&self, ordinal: usize) -> &rusty::RaftCommand {
        &self.slots_[ordinal].as_ref().unwrap().cmd_
    }
}

pub struct HeartbeatAuthority {
    term_: u64,
    config_size_: usize,
    voters_: rusty::BTreeSet<u16>,
    outstanding_: rusty::BTreeSet<u16>,
}

#[allow(clippy::new_without_default)]
impl HeartbeatAuthority {
    // A generation begins with this site already counted as a voter: a leader
    // is evidence for its own authority.
    pub fn new(term: u64, config_size: usize, self_site: u16) -> HeartbeatAuthority {
        let mut voters = rusty::BTreeSet::new();
        voters.insert(self_site);
        HeartbeatAuthority {
            term_: term,
            config_size_: config_size,
            voters_: voters,
            outstanding_: rusty::BTreeSet::new(),
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

use crate::quorum_hpp::raft_quorum_majority_count;
use crate::quorum_hpp::raft_quorum_count_reached;
use crate::server_h::raft_server_read_index_reply_confirms_authority;
use crate::server_h::raft_server_read_index_round_can_advance;
use crate::server_h::raft_server_log_index_above;
use crate::server_h::raft_server_log_entry_is_current_term;
use crate::server_h::raft_server_observed_higher_term;
use crate::server_h::raft_server_append_acknowledged_through;
use crate::server_h::raft_server_log_index_has_successor;
use crate::server_h::raft_server_follower_next_index;
use crate::server_h::raft_server_leader_hint_after_transition;
use crate::server_h::raft_server_vote_term_is_stale;
use crate::server_h::raft_server_leader_rpc_sender_is_authoritative;
use crate::server_h::raft_server_append_term_is_acceptable;
use crate::server_h::raft_server_append_is_acceptable;
use crate::server_h::raft_server_append_sent_end;
use crate::server_h::raft_server_append_entry_count_fits;
use crate::server_h::raft_server_append_batch_count_is_valid;
use crate::server_h::raft_server_append_entry_conflicts;
use crate::server_h::raft_server_append_result_last_index;
use crate::server_h::raft_server_commit_index_clamp;
use crate::server_h::raft_server_candidate_log_is_at_least;
use crate::server_h::raft_server_vote_is_idempotent;
use crate::server_h::BackoffKind;
use crate::server_h::RAFT_SERVER_INVALID_SITE_ID;
use crate::server_h::RaftConsensusState;

pub struct AuthorityGeneration {
    round_id_: u64,
    config_: rusty::BTreeSet<u16>,
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
        let mut snapshot = rusty::BTreeSet::new();
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

use crate::server_h::RaftServerBase;
use crate::server_pods_h::AppendRespView;
use crate::scheduler_h::RaftSpecific;
use crate::server_h::RaftEntry;
use crate::server_h::RaftLockGuard;
// Every C++ kernel this carrier calls, in one place. improper_ctypes is
// allowed because each of these passes an opaque handle by pointer and
// nothing is laid out across the boundary -- the same case as the allow
// on server.h's bridge block.
#[allow(improper_ctypes)]
unsafe extern "C" {
    fn raft_monotonic_now_us() -> u64;
    fn raft_fiber_sleep_us(micros: u64);
    fn raft_append_response_read(response: *const rusty::RaftResponsePtr)
        -> AppendRespView;
    fn raft_command_has_value(cmd: *const rusty::RaftCommand) -> bool;
    fn raft_verify(condition: bool);
    fn raft_snapshot_manager_is_set(
        manager: *const rusty::RaftSnapshotManagerPtr) -> bool;
    fn raft_phase1_load_and_send_snapshot(
        commo: *mut rusty::Communicator,
        snapshot_manager: *const rusty::RaftSnapshotManagerPtr,
        lifetime: *const rusty::RaftAsyncCallbackLifetimePtr,
        self_site_id: u16, partition_id: u32, send_term: u64,
        site_id: u16, ord: usize) -> bool;
    fn raft_batch_optimization_enabled() -> bool;
    fn raft_append_entries_batch_max() -> u64;
    // The wire kind of a command, for the diagnostics that report why an
    // entry could not be batched. It takes the COMMAND, not (server, index):
    // the lookup is Rust, and only reading inside the opaque payload is not.
    fn raft_command_kind(cmd: *const rusty::RaftCommand) -> i32;
    // The leader's batch: a TpcCommitCommand is copied and stamped with its
    // log term in C++ (a Marshallable) by the facade's `raft_stamped_commit`,
    // pushed into batch_buffer_ in Rust, and the buffer's Arcs are moved into
    // one TpcBatchCommand at the end.
    fn raft_command_is_tpc_commit(cmd: *const rusty::RaftCommand) -> bool;
    fn raft_batch_finalize(entries: *mut rusty::RaftTpcCommitPtr,
                           count: usize, cmd_out: *mut rusty::RaftCommand);
    // The command copy INTO Rust's slot; see server.h for why never by value.
    fn raft_command_clone_into(src: *const rusty::RaftCommand,
                               dst: *mut rusty::RaftCommand);
    fn raft_phase1_send_append(commo: *mut rusty::Communicator,
                               self_site_id: u16, site_id: u16,
                               partition_id: u32, is_leader: bool, term: u64,
                               prev_log_index: u64, prev_log_term: u64,
                               commit_index: u64,
                               cmd: *const rusty::RaftCommand,
                               cmd_log_term: u64,
                               out: *mut rusty::RaftResponsePtr);
}

// ==========================================================================
// THE ROUND SCOPE, AND THE COMMIT RULE BOTH PHASE 0 AND PHASE 3 APPLY
// ==========================================================================

pub struct HeartbeatRoundScope {
    term_: u64,
    round_id_: u64,
    config_: rusty::BTreeSet<u16>,
    current_commit_index_: u64,
    authority_inserted_: bool,
}

#[allow(clippy::new_without_default)]
impl HeartbeatRoundScope {
    pub fn new() -> HeartbeatRoundScope {
        HeartbeatRoundScope {
            term_: 0,
            round_id_: 0,
            config_: rusty::BTreeSet::new(),
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

// The commit-index advance, which PHASE 0 and PHASE 3 perform identically:
// PHASE 0 before the round's RPCs go out, PHASE 3 after their replies have
// been processed. It was the same fifteen lines twice.
//
// Returns the range the caller must hand to EnqueueCommittedEntries. The
// caller does the enqueue because that is the apply queue, i.e. I/O; the
// decision is here.
#[repr(C)]
pub struct CommitAdvance {
    advanced_: bool,
    from_: u64,
    to_: u64,
}

impl CommitAdvance {
    pub fn advanced(&self) -> bool {
        self.advanced_
    }

    pub fn from_index(&self) -> u64 {
        self.from_
    }

    pub fn to_index(&self) -> u64 {
        self.to_
    }
}

// Same aliasing note as heartbeat_phase3_locked: peers and log are reached
// through `consensus` because both are its fields.
pub fn raft_commit_advance(
    consensus: &mut RaftConsensusState,
    nservers: usize,
) -> CommitAdvance {
    // nservers is the value latched in PHASE 0. Reusing it in PHASE 3 is
    // sound only because current_config_ has exactly one write, during
    // Setup, and progress_ is never erased, so the size is invariant across
    // the round. Assert it rather than trusting the phases to stay in step.
    if consensus.peers_.len() != nservers - 1 {
        panic!("peer table and round membership disagree");
    }
    let candidate_index = consensus.peers_
        .majority_match_index(nservers, consensus.raft_log_.last_index());
    if !raft_server_log_index_above(candidate_index, consensus.commit_index_) {
        return CommitAdvance { advanced_: false, from_: 0, to_: 0 };
    }
    // The candidate is <= last_index() and > commit_index_, so the entry
    // provably exists. This says so rather than leaving a null dereference
    // to express it.
    let candidate = consensus.raft_log_.get(candidate_index);
    if candidate.is_none() {
        panic!("committable index is absent from the log");
    }
    if !raft_server_log_entry_is_current_term(
        candidate.unwrap().term(),
        consensus.current_term_,
    ) {
        // Raft commits a prior-term entry only via one from the current term.
        return CommitAdvance { advanced_: false, from_: 0, to_: 0 };
    }
    let from = consensus.commit_index_;
    consensus.commit_index_ = candidate_index;
    CommitAdvance { advanced_: true, from_: from, to_: candidate_index }
}

// ==========================================================================
// PHASE 0 -- advance the read-index round, recompute the commit index
// ==========================================================================

#[repr(C)]
pub struct Phase0Outcome {
    restart_: bool,
    commit_advanced_: bool,
    commit_from_: u64,
    commit_to_: u64,
}

impl Phase0Outcome {
    // The round is over before it began -- this server is not the leader.
    // The caller returns, and the driver starts the next round.
    pub fn restart(&self) -> bool {
        self.restart_
    }

    pub fn commit_advanced(&self) -> bool {
        self.commit_advanced_
    }

    pub fn commit_from(&self) -> u64 {
        self.commit_from_
    }

    pub fn commit_to(&self) -> u64 {
        self.commit_to_
    }
}

// TODO(raft-server-struct): remove this allow once RaftServer is itself a DSL
// struct. The ten parameters are exactly the pieces of RaftServer's state that
// PHASE 0 touches; they are separate arguments only because the orphan-impl
// rule forbids `impl RaftServer`, so this cannot yet be a method taking
// &mut self. Grouping them into a carrier struct now would invent a type whose
// only purpose is to be dissolved by that change. Before removing the allow,
// verify the parameter list really has collapsed into self rather than being
// hidden behind a wrapper.
#[allow(clippy::too_many_arguments)]
// Same aliasing note as heartbeat_phase3_locked.
pub fn heartbeat_phase0_locked(
    consensus: &mut RaftConsensusState,
    round: &mut HeartbeatRoundScope,
    pending: &mut PendingTable,
    ledger: &mut AuthorityLedger,
    pending_leader_term: &mut rusty::Option<u64>,
    members: &[u16],
    site_id: u16,
    is_leader: bool,
) -> Phase0Outcome {
    if !is_leader {
        pending.abandon();
        ledger.abandon();
        *pending_leader_term = rusty::None;
        return Phase0Outcome {
            restart_: true,
            commit_advanced_: false,
            commit_from_: 0,
            commit_to_: 0,
        };
    }

    round.begin(consensus.current_term_, consensus.heartbeat_round_);

    // Sized here rather than in the prologue because the round state is the
    // loop's, not the server's. Idempotent: resize only runs when the two
    // tables disagree, so in-flight slots survive every later round.
    if pending.len() != consensus.peers_.len() {
        pending.resize(consensus.peers_.len());
    }

    // Leadership may be lost and regained between two observations by this
    // fiber. Never let a prior term's physical RPC occupy a slot or collide
    // with the new leader epoch's round counter reset.
    let epoch_changed = pending_leader_term.is_none()
        || *pending_leader_term.as_ref().unwrap() != round.term();
    if epoch_changed {
        pending.abandon();
        ledger.abandon();
        *pending_leader_term = rusty::Some(round.term());
    }

    if raft_server_read_index_round_can_advance(consensus.heartbeat_round_) {
        consensus.heartbeat_round_ += 1;
    }
    // Saturation is fail-closed for new reads: the round never wraps, so no
    // post-baseline proof can be forged from an old generation. The caller
    // reports it; see round_saturated below.

    let mut i = 0;
    while i < members.len() {
        round.admit(members[i]);
        i += 1;
    }
    if round.nservers() == 0 || !round.is_member(site_id) {
        panic!("heartbeat round admitted no quorum containing this site");
    }
    let advance = raft_commit_advance(consensus, round.nservers());
    round.publish_commit_index(consensus.commit_index_);

    Phase0Outcome {
        restart_: false,
        commit_advanced_: advance.advanced(),
        commit_from_: advance.from_index(),
        commit_to_: advance.to_index(),
    }
}

// Whether PHASE 0 declined to advance the read-index generation because the
// counter is saturated. Split out so the caller can log it at ERROR without
// the DSL body paying for an unconditional log_line every round.
pub fn heartbeat_round_saturated(round_counter: u64) -> bool {
    !raft_server_read_index_round_can_advance(round_counter)
}

// ==========================================================================
// PHASE 0 and PHASE 3, formerly RaftServer::HeartbeatPhase0/3.
//
// Their decisions were already DSL bodies (heartbeat_phase0_locked and
// heartbeat_phase3_locked). What was left in C++ was the mutex, the apply
// queue, the replication wake and the debug logging -- all of which a DSL
// body can now express, so the C++ halves are gone.
//
// The round members no longer come from a std::vector rebuilt out of
// current_config_ every round; they are server.config_members_, filled once
// during Setup. Same contents, sorted and duplicate-free, which is what both
// the round's membership and the ledger's set-equality check expect.
// ==========================================================================
pub fn heartbeat_phase0_body(server: &mut RaftServerBase,
                             pending_rpcs: &mut PendingTable,
                             authority_rounds: &mut AuthorityLedger,
                             pending_leader_term: &mut rusty::Option<u64>,
                             round: &mut HeartbeatRoundScope) -> bool {
    {
        let _lock = RaftLockGuard::new(&mut server.mtx_);
        let leader: bool = server.IsLeaderLocked();
        if leader && heartbeat_round_saturated(server.state_.heartbeat_round_) {
            rusty::raft_log_error_2(
                "[READ-INDEX] site={} heartbeat round saturated in term {}",
                server.site_id_, server.state_.current_term_);
        }
        if leader {
            let mut ord: usize = 0;
            while ord < server.state_.peers_.len() {
                rusty::raft_log_debug_2(
                    "[COMMIT-CALC] match_index_[{}] = {}",
                    server.peer_site_at(ord),
                    server.state_.peers_.match_index(ord));
                ord += 1;
            }
        }

        let site_id: u16 = server.site_id_;
        let members: rusty::Vec<u16> = server.config_members_.clone();
        let outcome: Phase0Outcome = heartbeat_phase0_locked(
            &mut server.state_, round, pending_rpcs, authority_rounds,
            pending_leader_term, &members, site_id, leader);

        if outcome.restart() {
            // Was `continue`; the Rust driver starts the next round when
            // this returns true.
            return true;
        }
        if outcome.commit_advanced() {
            // The apply queue is I/O, so the hand-off stays a call; the
            // DECISION to commit was made above.
            server.EnqueueCommittedEntries(outcome.commit_from(),
                                           outcome.commit_to());
        }
    }

    let members: rusty::Vec<u16> = server.config_members_.clone();
    let opened: bool = authority_rounds.open(
        round.round_id(), &members,
        HeartbeatAuthority::new(round.term(), round.nservers(),
                                server.site_id_));
    round.set_authority_inserted(opened);
    // heartbeat_round_ never wraps. The only possible duplicate is the
    // deliberately fail-closed UINT64_MAX saturation generation, which
    // open() declines rather than overwriting.
    if !round.authority_inserted() {
        unsafe {
            raft_verify(round.round_id() == u64::MAX);
        }
    }
    true
}

// ==========================================================================
// PHASE 1 -- choose each follower's payload and send it
// ==========================================================================

// Chooses what this AppendEntries carries: nothing (a heartbeat), one raw
// entry, or a TpcBatchCommand. Returns true when the follower must be
// skipped. CALLER MUST HOLD mtx_.
//
// The two arms were an #ifdef RAFT_BATCH_OPTIMIZATION / #ifndef pair. They
// are an if/else on a compile-time-constant function now, because the
// preprocessor has no DSL spelling; the compiler folds the dead arm exactly
// as the preprocessor removed it.
#[allow(clippy::too_many_arguments)]
pub fn heartbeat_phase1_select_payload(server: &mut RaftServerBase,
                                       ord: usize, site_id: u16,
                                       prev_log_index: u64,
                                       cmd: &mut rusty::RaftCommand,
                                       cmd_log_term: &mut u64,
                                       sent_end_index: &mut u64) -> bool {
    let mut skip_follower: bool = false;

    if !unsafe { raft_batch_optimization_enabled() } {
        rusty::raft_log_debug_5(
            "[BATCH_CHECK] site={} follower={} next_index={} state_.raft_log_.base()={} state_.raft_log_.last_index()={}",
            server.site_id_, site_id, server.state_.peers_.next_index(ord),
            server.state_.raft_log_.base(),
            server.state_.raft_log_.last_index());
        if server.state_.peers_.next_index(ord)
            <= server.state_.raft_log_.last_index()
        {
            if !raft_server_append_entry_count_fits(prev_log_index, 1) {
                rusty::raft_log_error_2(
                    "[HEARTBEAT-SEND] Log index exhausted after {}, skipping follower {}",
                    prev_log_index, site_id);
                skip_follower = true;
            } else {
                let next: u64 = server.state_.peers_.next_index(ord);
                let slot = server.state_.raft_log_.get(next);
                let usable: bool = slot.is_some()
                    && unsafe {
                        raft_command_has_value(
                            slot.unwrap().cmd() as *const rusty::RaftCommand)
                    };
                if !usable {
                    rusty::raft_log_error_2(
                        "[HEARTBEAT-SEND] Missing log entry {}, skipping follower {}",
                        next, site_id);
                    skip_follower = true;
                } else {
                    let entry: &RaftEntry = slot.unwrap();
                    *cmd_log_term = entry.term() as u64;
                    // Copying a Command is a refcount bump on its inner Arc:
                    // a kernel, never the carrier's clone.
                    unsafe {
                        raft_command_clone_into(
                            entry.cmd() as *const rusty::RaftCommand,
                            cmd as *mut rusty::RaftCommand);
                    }
                    *sent_end_index =
                        raft_server_append_sent_end(prev_log_index, 1);
                    // The kind tag identifies the payload better than the
                    // inner shared_ptr's raw address ever did.
                    let kind: i32 = unsafe {
                        raft_command_kind(
                            entry.cmd() as *const rusty::RaftCommand)
                    };
                    rusty::raft_log_debug_4(
                        "[APPEND_SEND] site={} sending entry {} to follower {} cmd_kind={}",
                        server.site_id_, next, site_id, kind);
                }
            }
        }
        return skip_follower;
    }

    // A fresh buffer per follower, as the C++ local was.
    server.batch_buffer_.clear();
    let max_batch_entries: u64 = unsafe { raft_append_entries_batch_max() };
    let batch_start_idx: u64 = server.state_.peers_.next_index(ord);
    rusty::raft_log_debug_5(
        "[BATCH_CHECK] site={} follower={} next_index={} state_.raft_log_.base()={} state_.raft_log_.last_index()={}",
        server.site_id_, site_id, server.state_.peers_.next_index(ord),
        server.state_.raft_log_.base(), server.state_.raft_log_.last_index());
    if !raft_server_append_entry_count_fits(prev_log_index, 1) {
        rusty::raft_log_error_2(
            "[HEARTBEAT-BATCH] Log index exhausted after {}, skipping follower {}",
            prev_log_index, site_id);
        skip_follower = true;
    }
    let first_encoded_index: u64 = if skip_follower {
        0
    } else {
        raft_server_append_sent_end(prev_log_index, 1)
    };
    if !skip_follower
        && (batch_start_idx != first_encoded_index
            || batch_start_idx < server.state_.raft_log_.base())
    {
        rusty::raft_log_error_4(
            "[HEARTBEAT-BATCH] Non-contiguous source for follower {}: prev={} start={} min_active={}; refusing to compress a hole",
            site_id, prev_log_index, batch_start_idx,
            server.state_.raft_log_.base());
        skip_follower = true;
    } else if !skip_follower {
        let mut idx: u64 = batch_start_idx;
        while idx <= server.state_.raft_log_.last_index()
            && (server.batch_buffer_.len() as u64) < max_batch_entries
        {
            let entry = server.state_.raft_log_.get(idx);
            let usable: bool = entry.is_some()
                && unsafe {
                    raft_command_has_value(
                        entry.unwrap().cmd() as *const rusty::RaftCommand)
                };
            if !usable {
                rusty::raft_log_error_2(
                    "[HEARTBEAT-BATCH] Missing log entry {} for follower {}; refusing to compress a hole",
                    idx, site_id);
                skip_follower = true;
                break;
            }
            let entry_term: i64 = entry.unwrap().term();
            let entry_cmd: *const rusty::RaftCommand =
                entry.unwrap().cmd() as *const rusty::RaftCommand;
            let is_commit: bool =
                unsafe { raft_command_is_tpc_commit(entry_cmd) };
            if is_commit {
                let stamped: rusty::RaftTpcCommitPtr = unsafe {
                    rusty::raft_stamped_commit(entry_cmd, entry_term)
                };
                server.batch_buffer_.push(stamped);
            } else {
                // Looked up again rather than held across the push above:
                // `entry` borrows the log, and handing the push a *mut to the
                // server ends that borrow. This branch is the rare one -- an
                // entry that is not a TpcCommitCommand -- so the second
                // lookup costs nothing on the batching path.
                let slot = server.state_.raft_log_.get(idx);
                let kind: i32 = unsafe {
                    raft_command_kind(
                        slot.unwrap().cmd() as *const rusty::RaftCommand)
                };
                let batched: u64 = server.batch_buffer_.len() as u64;
                if batched == 0 {
                    rusty::raft_log_info_3(
                        "[BATCH_SKIP] site={} idx={}: log entry is not TpcCommitCommand (kind={}), using raw log",
                        server.site_id_, idx, kind);
                    unsafe {
                        raft_command_clone_into(
                            slot.unwrap().cmd() as *const rusty::RaftCommand,
                            cmd as *mut rusty::RaftCommand);
                    }
                    *cmd_log_term = entry_term as u64;
                    *sent_end_index =
                        raft_server_append_sent_end(prev_log_index, 1);
                } else {
                    rusty::raft_log_info_3(
                        "[BATCH_STOP] site={} idx={}: ending batch before non-TpcCommitCommand kind={}",
                        server.site_id_, idx, kind);
                }
                break;
            }
            if !raft_server_log_index_has_successor(idx) {
                break;
            }
            idx += 1;
        }
    }

    let encoded_entry_count: u64 = server.batch_buffer_.len() as u64;
    if !skip_follower && encoded_entry_count > 0
        && !raft_server_append_batch_count_is_valid(prev_log_index,
                                                    encoded_entry_count)
    {
        rusty::raft_log_error_3(
            "[HEARTBEAT-BATCH] Invalid encoded count {} after previous index {}; skipping follower {}",
            encoded_entry_count, prev_log_index, site_id);
        skip_follower = true;
    }
    if !skip_follower && encoded_entry_count > 0 {
        unsafe {
            raft_batch_finalize(server.batch_buffer_.as_mut_ptr(),
                                server.batch_buffer_.len(),
                                cmd as *mut rusty::RaftCommand);
        }
        *sent_end_index = raft_server_append_sent_end(prev_log_index,
                                                      encoded_entry_count);
        let batch_end_idx: u64 = *sent_end_index;
        let truncated: bool =
            batch_end_idx < server.state_.raft_log_.last_index();
        rusty::raft_log_info_6(
            "[BATCH_SEND] site={} sending batch of {} entries to follower {} (from={} to={}{})",
            server.site_id_, encoded_entry_count, site_id, batch_start_idx,
            batch_end_idx, if truncated { ", truncated" } else { "" });
    }
    skip_follower
}

// ==========================================================================
// PHASE 1: send every AppendEntries RPC in parallel, non-blocking.
//
// Formerly RaftServer::HeartbeatPhase1. The ordinal is used for ITERATION
// ONLY; every read and write of a follower's next index goes through
// state_.peers_.next_index(ord).
//
// That is not style. The body reaches commo()->SendInstallSnapshot inside
// the lock scope, and that call's completion callback takes the SAME mutex
// and writes next_index -- so a callback that completes synchronously
// re-enters and mutates the table while a dereferenced cursor into it is
// live. Holding no dereferenced cursor across that call is also what makes
// the loop expressible in Rust at all: a `&mut` into a table cannot be held
// across a call that takes `&mut` to the same table.
// ==========================================================================
// The zero-initialisation of prev_log_index and sent_end_index is the C++
// original's, kept so the emitted C++ declares initialised locals rather
// than uninitialised ones. `is_none` then `unwrap` is likewise the shape the
// original had; rewriting it as a match would obscure the correspondence.
#[allow(unused_assignments, clippy::unnecessary_unwrap)]
pub fn heartbeat_phase1_body(server: &mut RaftServerBase,
                             pending_rpcs: &mut PendingTable,
                             authority_rounds: &mut AuthorityLedger,
                             round: &HeartbeatRoundScope) {
    let partition_id: u32 = server.partition_id_;
    let mut ord: usize = 0;
    while ord < server.state_.peers_.len() {
        let site_id: u16 = server.peer_site_at(ord);
        if site_id == server.site_id_ {
            ord += 1;
            continue;
        }
        if !server.IsLeader() {
            break;  // Stop sending if we lost leadership.
        }
        if pending_rpcs.occupied(ord) {
            ord += 1;
            continue;
        }

        let mut prev_log_index: u64 = 0;
        let mut prev_log_term: u64 = 0;
        // An empty Command (has_value() == false) signals a heartbeat.
        let mut cmd: rusty::RaftCommand = Default::default();
        let mut cmd_log_term: u64 = 0;
        let mut sent_end_index: u64 = 0;
        let mut skip_follower: bool = false;
        {
            let _lock = RaftLockGuard::new(&mut server.mtx_);
            if server.state_.peers_.next_index(ord) == 0 {
                rusty::raft_log_warn_2(
                    "[APPEND_ENTRIES] Repairing wrapped next_index for follower {} at leader last index {}",
                    site_id, server.state_.raft_log_.last_index());
                let last: u64 = server.state_.raft_log_.last_index();
                let repaired: u64 = if raft_server_log_index_has_successor(last) {
                    raft_server_follower_next_index(last)
                } else {
                    last
                };
                server.state_.peers_.set_next_index(ord, repaired);
            }
            prev_log_index = server.state_.peers_.next_index(ord) - 1;
            if prev_log_index > server.state_.raft_log_.last_index() {
                rusty::raft_log_info_2(
                    "[APPEND_ENTRIES] ERROR: prevLogIndex ({}) > state_.raft_log_.last_index() ({}), fixing next_index",
                    prev_log_index, server.state_.raft_log_.last_index());
                let last: u64 = server.state_.raft_log_.last_index();
                let repaired: u64 = if raft_server_log_index_has_successor(last) {
                    raft_server_follower_next_index(last)
                } else {
                    last
                };
                server.state_.peers_.set_next_index(ord, repaired);
                prev_log_index = server.state_.peers_.next_index(ord) - 1;
            }
            // Until a payload is selected this is a heartbeat, and proves
            // only the prefix named by prev_log_index.
            sent_end_index = raft_server_append_sent_end(prev_log_index, 0);

            let snapshot_configured: bool = unsafe {
                raft_snapshot_manager_is_set(&server.snapshot_manager_)
            };
            if prev_log_index > server.state_.raft_log_.last_index() {
                rusty::raft_log_info_3(
                    "[APPEND_ENTRIES] WARNING: Cannot send AppendEntries to follower {}: prevLogIndex ({}) > state_.raft_log_.last_index() ({}), skipping",
                    site_id, prev_log_index,
                    server.state_.raft_log_.last_index());
                server.state_.peers_.set_next_index(ord, 1);
                skip_follower = true;
            } else if server.state_.peers_.next_index(ord)
                < server.state_.raft_log_.base()
                && snapshot_configured
            {
                // The follower is behind the log's base, so send it a
                // snapshot instead of entries it can no longer be given.
                rusty::raft_log_info_4(
                    "[HEARTBEAT-SNAPSHOT] Site {}: Follower {} next_index={} < state_.raft_log_.base()={}, sending InstallSnapshot",
                    server.site_id_, site_id,
                    server.state_.peers_.next_index(ord),
                    server.state_.raft_log_.base());
                let sent: bool = unsafe {
                    raft_phase1_load_and_send_snapshot(
                        server.commo_,
                        &server.snapshot_manager_
                            as *const rusty::RaftSnapshotManagerPtr,
                        &server.async_callback_lifetime_
                            as *const rusty::RaftAsyncCallbackLifetimePtr,
                        server.site_id_, server.partition_id_,
                        server.state_.current_term_, site_id, ord)
                };
                if !sent {
                    rusty::raft_log_warn_2(
                        "[HEARTBEAT-SNAPSHOT] Site {}: Failed to load snapshot for follower {}, skipping",
                        server.site_id_, site_id);
                }
                // Both outcomes skip the normal AppendEntries for this
                // follower: sent, or failed to load.
                skip_follower = true;
            } else {
                unsafe {
                    raft_verify(
                        prev_log_index
                            <= server.state_.raft_log_.last_index());
                }
                if prev_log_index == 0 {
                    prev_log_term = 0;
                } else if prev_log_index == server.state_.snapidx_
                    && server.state_.snapidx_ > 0
                {
                    // Keep using snapshot boundary metadata after compaction.
                    prev_log_term = server.state_.snapterm_ as u64;
                } else {
                    // Was GetRaftInstance, which default-inserted and so
                    // could never return null -- the check below was dead,
                    // and a genuinely missing prevLogIndex silently
                    // fabricated an empty entry with term 0 and sent
                    // prevLogTerm = 0 rather than skipping the follower.
                    let instance = server.state_.raft_log_.get(prev_log_index);
                    if instance.is_none() {
                        rusty::raft_log_error_2(
                            "[HEARTBEAT-SEND] [CRITICAL] log entry {} is absent! Skipping follower {}",
                            prev_log_index, site_id);
                        skip_follower = true;
                    } else {
                        prev_log_term = instance.unwrap().term() as u64;
                    }
                }

                if !skip_follower {
                    skip_follower = heartbeat_phase1_select_payload(
                        server, ord, site_id, prev_log_index, &mut cmd,
                        &mut cmd_log_term, &mut sent_end_index);
                }
            }
        }
        if skip_follower {
            ord += 1;
            continue;
        }

        let is_leader: bool = server.IsLeader();
        let mut sent_response: rusty::RaftResponsePtr = Default::default();
        unsafe {
            raft_phase1_send_append(
                server.commo_, server.site_id_, site_id, partition_id,
                is_leader, round.term(), prev_log_index, prev_log_term,
                round.commit_index(),
                &cmd as *const rusty::RaftCommand, cmd_log_term,
                &mut sent_response as *mut rusty::RaftResponsePtr);
        }

        pending_rpcs.place(ord, PendingAppend::new(
            site_id, round.term(), round.round_id(), sent_end_index,
            sent_response, cmd));
        if round.authority_inserted() && round.is_member(site_id) {
            // Was a std::map iterator created in PHASE 0 and dereferenced
            // here, after the RPC sends. Nothing between the two points
            // mutates authority_rounds, so it was valid -- but a cursor held
            // across a phase boundary and across a synchronous completion
            // callback is the hazard class this file removed for next_index,
            // so look it up by key.
            unsafe {
                raft_verify(
                    authority_rounds.launch(round.round_id(), site_id));
            }
        }
        ord += 1;
    }
}

// ==========================================================================
// PHASE 2 -- poll the replies through one round deadline and apply them
// ==========================================================================

// PHASE 2's decision core: what one AppendEntries reply means.
//
// PHASE 2 is a polling loop over the in-flight slots. The loop itself, its
// round deadline and its Fiber::sleep stay in C++ -- suspension is the one
// thing genuinely shaped by the fiber runtime. What each reply MEANS is not,
// and that is this.
//
// The wire reply is read out by the caller and arrives here as three scalars.
// That is the same "convert at the edge" split the rest of the file uses: the
// rrr response object never crosses, only what it says.
//
// The caller keeps four things because none of them are decisions:
// LogTermChange and the backoff-rung logging (both pure logging, and both
// need their level short-circuit), PeerOrdinal (a scan over peer_sites_,
// which is C++), and stepDown -- which reaches setIsLeader and the election
// timer, i.e. the reactor. This returns STEP_DOWN and lets the caller do it.
#[repr(C)]
pub struct SentAppend {
    follower_: u16,
    term_: u64,
    round_: u64,
    end_index_: u64,
    // peers.len() when this follower is no longer a peer at all.
    ordinal_: usize,
}

impl SentAppend {
    pub fn new(follower: u16, term: u64, round: u64, end_index: u64, ordinal: usize) -> SentAppend {
        SentAppend { follower_: follower, term_: term, round_: round,
                     end_index_: end_index, ordinal_: ordinal }
    }

    pub fn round(&self) -> u64 {
        self.round_
    }
}

#[repr(C)]
pub struct AppendReply {
    available_: bool,
    status_: bool,
    term_: u64,
    last_log_index_: u64,
}

impl AppendReply {
    pub fn new(available: bool, status: bool, term: u64, last_log_index: u64) -> AppendReply {
        AppendReply { available_: available, status_: status, term_: term,
                      last_log_index_: last_log_index }
    }
}

#[allow(non_camel_case_types)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Eq, PartialEq))]
#[repr(i32)]
pub enum AppendReplyAction {
    // Nothing was learned: the RPC failed, or the reply belongs to a term or
    // a leadership epoch that is no longer current.
    IGNORED = 0,
    // The follower proved a newer term. Term, vote and leader hint have been
    // updated here; the caller must perform the step-down itself.
    STEP_DOWN = 1,
    // Rejected. The backoff ladder ran and reports which rung it took.
    BACKED_OFF = 2,
    // Accepted, and replication progress advanced.
    ACCEPTED = 3,
    // Success that does not cover the payload it was sent. AppendEntries
    // acceptance is atomic, so this proves nothing and must not be counted.
    CONTRADICTORY = 4,
    // The follower has no replication indices: it is not one this server
    // leads. Higher-term evidence was already handled above.
    UNKNOWN_FOLLOWER = 5,
}

#[repr(C)]
pub struct AppendReplyOutcome {
    action_: AppendReplyAction,
    rung_: BackoffKind,
    old_next_: u64,
    new_next_: u64,
    acknowledged_: u64,
    previous_term_: u64,
}

impl AppendReplyOutcome {
    pub fn action(&self) -> AppendReplyAction {
        self.action_
    }

    // BACKED_OFF only.
    pub fn rung(&self) -> BackoffKind {
        self.rung_
    }

    pub fn old_next(&self) -> u64 {
        self.old_next_
    }

    pub fn new_next(&self) -> u64 {
        self.new_next_
    }

    // ACCEPTED only.
    pub fn acknowledged(&self) -> u64 {
        self.acknowledged_
    }

    // STEP_DOWN only: the term this server held before the reply displaced it.
    pub fn previous_term(&self) -> u64 {
        self.previous_term_
    }
}

fn append_reply_nothing(action: AppendReplyAction) -> AppendReplyOutcome {
    AppendReplyOutcome {
        action_: action,
        rung_: BackoffKind::FLOOR,
        old_next_: 0,
        new_next_: 0,
        acknowledged_: 0,
        previous_term_: 0,
    }
}

// Takes the log's tail rather than the log: this decides what a reply
// proves, and the only thing it needs from the log is where the log ends.
// Narrowing the parameter is also what keeps the argument list inside
// clippy's limit without an allow.
// `peers` is reached through `consensus` rather than passed alongside it.
// The C++ call site passed `state_` and `state_.peers_` as two arguments --
// two mutable borrows of overlapping state, which only compiled because the
// caller was C++. A Rust caller cannot spell that, and PHASE 2 is a Rust
// caller now.
pub fn heartbeat_apply_append_reply(
    consensus: &mut RaftConsensusState,
    ledger: &mut AuthorityLedger,
    sent: &SentAppend,
    reply: &AppendReply,
    log_last_index: u64,
    is_leader: bool,
) -> AppendReplyOutcome {
    // Retire the RPC and, if it proves this exact generation, count the vote.
    // One physical RPC exists per follower per generation, but the evidence
    // stays a set so a future transport still cannot double-count a voter.
    let evidence = AuthorityReply::new(
        sent.round_,
        sent.follower_,
        sent.term_,
        reply.term_,
        consensus.current_term_,
        is_leader,
        reply.available_,
    );
    ledger.record_reply(&evidence);

    if !reply.available_ {
        return append_reply_nothing(AppendReplyAction::IGNORED);
    }

    // A higher term is authoritative regardless of the accompanying status
    // bit. The responding follower proves a newer term, not its leader.
    if raft_server_observed_higher_term(reply.term_, consensus.current_term_) {
        let previous_term = consensus.current_term_;
        consensus.current_term_ = reply.term_;
        consensus.vote_for_ = u16::MAX;
        // Neither leading nor knowing a leader, so the hint is cleared. The
        // responding follower proved a newer term, not that it is the leader
        // of that term. (With both flags false the shared predicate returns
        // the invalid id whatever ids it is handed, so it is spelled out
        // here rather than called with two arguments that do not matter.)
        consensus.current_leader_id_ = RAFT_SERVER_INVALID_SITE_ID;
        let mut out = append_reply_nothing(AppendReplyAction::STEP_DOWN);
        out.previous_term_ = previous_term;
        return out;
    }

    // A reply from a send term this server has left proves nothing about now.
    if consensus.current_term_ != sent.term_ {
        return append_reply_nothing(AppendReplyAction::IGNORED);
    }
    // A valid follower processes AppendEntries in the leader's term before
    // replying, so a lower response term cannot prove this send.
    if reply.term_ != sent.term_ {
        return append_reply_nothing(AppendReplyAction::IGNORED);
    }
    if !is_leader {
        return append_reply_nothing(AppendReplyAction::IGNORED);
    }
    if sent.ordinal_ == consensus.peers_.len() {
        return append_reply_nothing(AppendReplyAction::UNKNOWN_FOLLOWER);
    }

    if !reply.status_ {
        let old_next = consensus.peers_.next_index(sent.ordinal_);
        let rung = consensus.peers_
            .back_off_after_reject(sent.ordinal_, reply.last_log_index_);
        let new_next = consensus.peers_.next_index(sent.ordinal_);
        let mut out = append_reply_nothing(AppendReplyAction::BACKED_OFF);
        out.rung_ = rung;
        out.old_next_ = old_next;
        out.new_next_ = new_next;
        return out;
    }

    if reply.last_log_index_ < sent.end_index_ {
        return append_reply_nothing(AppendReplyAction::CONTRADICTORY);
    }

    // Successful responses are monotonic and prove no index beyond the exact
    // payload end. In particular a heartbeat cannot adopt an unknown
    // follower suffix.
    let acknowledged = raft_server_append_acknowledged_through(
        reply.last_log_index_, sent.end_index_, log_last_index);
    consensus.peers_.accept_through(
        sent.ordinal_,
        acknowledged,
        raft_server_log_index_has_successor(acknowledged),
        raft_server_follower_next_index(acknowledged),
    );
    let mut out = append_reply_nothing(AppendReplyAction::ACCEPTED);
    out.acknowledged_ = acknowledged;
    out
}

// ==========================================================================
// PHASE 2: poll responses through one SHORT round deadline and process them.
//
// Formerly RaftServer::HeartbeatPhase2. Never call wait_timeout on an
// individual response: that permanently marks its event TIMEOUT and loses a
// legitimate late persistence reply. Polling also gives every parallel RPC
// the same bounded round budget.
//
// The pieces of HeartbeatRoundState are passed separately rather than the
// struct itself, because that struct is hand-written C++ declared after this
// block; its three members are all DSL types declared in it.
// ==========================================================================
#[allow(clippy::too_many_arguments, clippy::manual_clamp)]
pub fn heartbeat_phase2_body(server: &mut RaftServerBase,
                             pending_rpcs: &mut PendingTable,
                             authority_rounds: &mut AuthorityLedger,
                             round: &HeartbeatRoundScope) {
    const RESPONSE_POLL_STEP_US: u64 = 1000;
    // max(1, min(100000, heartbeat_interval_us_)). Spelled out rather than
    // with clamp: this lowers to C++, where uint64_t has no such member.
    let response_round_timeout_us: u64 =
        if server.heartbeat_interval_us_ > 100000 {
            100000
        } else if server.heartbeat_interval_us_ < 1 {
            1
        } else {
            server.heartbeat_interval_us_
        };
    let response_deadline_us: u64 =
        unsafe { raft_monotonic_now_us() } + response_round_timeout_us;
    let mut stop_response_processing: bool = false;
    let mut retry_released_follower: bool = false;

    while !stop_response_processing {
        let mut waiting_for_current_round: bool = false;
        let mut pending_ord: usize = 0;
        while pending_ord < pending_rpcs.len() {
            if !server.IsLeader() {
                stop_response_processing = true;
                break;
            }
            if !pending_rpcs.occupied(pending_ord) {
                pending_ord += 1;
                continue;
            }

            // Bound once per slot per poll pass, not per use: every read
            // below is the same shape it was when this was a map value.
            let follower_id: u16 = pending_rpcs.follower(pending_ord);
            let sent_term: u64 = pending_rpcs.sent_term(pending_ord);
            let sent_round: u64 = pending_rpcs.sent_round(pending_ord);
            let sent_end_index: u64 = pending_rpcs.sent_end_index(pending_ord);
            let cmd_has_value: bool = unsafe {
                raft_command_has_value(
                    pending_rpcs.cmd(pending_ord) as *const rusty::RaftCommand)
            };
            let resp: AppendRespView = unsafe {
                raft_append_response_read(
                    pending_rpcs.response(pending_ord)
                        as *const rusty::RaftResponsePtr)
            };
            if !resp.completed_ {
                if sent_round == round.round_id() {
                    waiting_for_current_round = true;
                }
                pending_ord += 1;
                continue;
            }

            let mut stepped_down: bool = false;
            {
                let _lock = RaftLockGuard::new(&mut server.mtx_);
                // What the reply MEANS is heartbeat_apply_append_reply. It
                // reads the wire response as three scalars -- the rrr object
                // itself never crosses -- and returns what to do about it.
                let response_available: bool =
                    !(!resp.status_ && resp.term_ == 0
                      && resp.last_log_index_ == 0);
                let resp_ord: usize = server.PeerOrdinal(follower_id);
                let log_last_index: u64 = server.state_.raft_log_.last_index();
                let is_leader: bool = server.IsLeaderLocked();
                let outcome: AppendReplyOutcome = heartbeat_apply_append_reply(
                    &mut server.state_,
                    authority_rounds,
                    &SentAppend::new(follower_id, sent_term, sent_round,
                                     sent_end_index, resp_ord),
                    &AppendReply::new(response_available, resp.status_,
                                      resp.term_, resp.last_log_index_),
                    log_last_index,
                    is_leader);

                let action: AppendReplyAction = outcome.action();
                if action == AppendReplyAction::STEP_DOWN {
                    rusty::raft_log_info_4(
                        "[STEPDOWN] Site {}: AppendEntries response from follower {} carried higher term {} > {}",
                        server.site_id_, follower_id, resp.term_,
                        outcome.previous_term());
                    server.LogTermChange(
                        "AppendEntries response carried newer term",
                        outcome.previous_term(), server.state_.current_term_,
                        follower_id);
                    // stepDown reaches setIsLeader and the election timer, so
                    // it stays here; the decision to take it was made above.
                    server.stepDown();
                    server.state_.req_voting_ = false;
                    server.state_.election_in_progress_ = false;
                    stepped_down = true;
                } else if action == AppendReplyAction::BACKED_OFF {
                    // The five-rung ladder is
                    // FollowerProgress::back_off_after_reject; it reports
                    // which rung it took so the diagnostics stay as specific
                    // as they were when the branches were inline.
                    let rung: BackoffKind = outcome.rung();
                    if rung == BackoffKind::FAST {
                        rusty::raft_log_info_6(
                            "[LOG-RECONCILE] Site {}: Fast backoff for follower {}: next_index {} -> {} (gap: {}, follower reported last: {})",
                            server.site_id_, follower_id, outcome.old_next(),
                            outcome.new_next(),
                            outcome.old_next() - outcome.new_next(),
                            resp.last_log_index_);
                    } else if rung == BackoffKind::TERM_CONFLICT {
                        rusty::raft_log_info_4(
                            "[LOG-RECONCILE] Site {}: Term-conflict backoff for follower {}: next_index {} -> {}",
                            server.site_id_, follower_id, outcome.old_next(),
                            outcome.new_next());
                    } else if rung == BackoffKind::EXPONENTIAL {
                        rusty::raft_log_info_4(
                            "[LOG-RECONCILE] Site {}: Exponential backoff for follower {}: next_index {} -> {} (halved)",
                            server.site_id_, follower_id, outcome.old_next(),
                            outcome.new_next());
                    } else if rung == BackoffKind::LINEAR {
                        rusty::raft_log_debug_4(
                            "[LOG-RECONCILE] Site {}: Linear backoff for follower {}: next_index {} -> {}",
                            server.site_id_, follower_id, outcome.old_next(),
                            outcome.new_next());
                    }
                    // BackoffKind::FLOOR logs nothing, as before.
                } else if action == AppendReplyAction::ACCEPTED {
                    rusty::raft_log_debug_8(
                        "[APPEND_RPC] Leader {} accepted follower {} proof: kind={} reported={} sent_end={} acknowledged={} next={} match={}",
                        server.site_id_, follower_id,
                        if cmd_has_value { "entries" } else { "heartbeat" },
                        resp.last_log_index_, sent_end_index,
                        outcome.acknowledged(),
                        server.state_.peers_.next_index(resp_ord),
                        server.state_.peers_.match_index(resp_ord));
                } else if action == AppendReplyAction::CONTRADICTORY {
                    rusty::raft_log_warn_3(
                        "[APPEND_RPC] Ignoring contradictory success from follower {}: reported_end={} sent_end={}",
                        follower_id, resp.last_log_index_, sent_end_index);
                } else if action == AppendReplyAction::UNKNOWN_FOLLOWER {
                    rusty::raft_log_debug_1(
                        "[APPEND_RPC] Ignoring replication response from removed follower {}",
                        follower_id);
                }
                // AppendReplyAction::IGNORED does nothing, as before.
            }

            let completed_previous_round: bool =
                sent_round != round.round_id();
            pending_rpcs.release(pending_ord);
            retry_released_follower =
                retry_released_follower || completed_previous_round;
            if stepped_down {
                stop_response_processing = true;
                break;
            }
            pending_ord += 1;
        }

        let current_round_has_authority: bool =
            authority_rounds.has_quorum(round.round_id());
        if stop_response_processing || !waiting_for_current_round
            || current_round_has_authority
        {
            break;
        }
        let now_us: u64 = unsafe { raft_monotonic_now_us() };
        if now_us >= response_deadline_us {
            break;
        }
        let remaining_us: u64 = response_deadline_us - now_us;
        let step_us: u64 = if remaining_us < RESPONSE_POLL_STEP_US {
            remaining_us
        } else {
            RESPONSE_POLL_STEP_US
        };
        unsafe {
            raft_fiber_sleep_us(step_us);
        }
    }

    if stop_response_processing {
        pending_rpcs.abandon();
        authority_rounds.abandon();
    } else if retry_released_follower {
        // A completion from an older round opened a per-follower slot after
        // PHASE 1. Prompt another round instead of waiting a full interval.
        server.RequestReplication();
    }
}

// ==========================================================================
// PHASE 3 -- recompute the commit index and publish read-index authority
// ==========================================================================

// PHASE 3 of the heartbeat round: the whole locked section. Recomputes the
// commit index now that this round's replies have been processed, then
// publishes read-index authority -- deliberately in that order, because a
// delayed reply is evidence for the exact term, generation and membership
// snapshot that launched it and must never be relabelled as the current
// round.
//
// Both halves were already Rust in their parts: raft_commit_advance above
// and AuthorityLedger::settle. This is the body that joins them.
#[repr(C)]
pub struct Phase3Outcome {
    commit_: CommitAdvance,
    confirmed_: bool,
}

impl Phase3Outcome {
    pub fn commit(&self) -> &CommitAdvance {
        &self.commit_
    }

    // True when a read-index generation reached quorum this round, in which
    // case the confirmed term and round have already been stored.
    pub fn confirmed(&self) -> bool {
        self.confirmed_
    }
}

// peers and log are reached through `consensus`, for the same reason
// heartbeat_apply_append_reply's are: the C++ call site passed `state_`,
// `state_.peers_` and `state_.raft_log_` as three arguments, which is one
// mutable borrow overlapping two shared ones. A Rust caller cannot spell it.
pub fn heartbeat_phase3_locked(
    consensus: &mut RaftConsensusState,
    ledger: &mut AuthorityLedger,
    nservers: usize,
    members: &[u16],
    is_leader: bool,
) -> Phase3Outcome {
    let commit = raft_commit_advance(consensus, nservers);
    let outcome = ledger.settle(
        is_leader,
        consensus.current_term_,
        members,
        consensus.read_quorum_confirmed_term_,
        consensus.read_quorum_confirmed_round_,
    );
    let mut confirmed = false;
    if outcome.confirmed() {
        consensus.read_quorum_confirmed_term_ = outcome.term();
        consensus.read_quorum_confirmed_round_ = outcome.round_id();
        confirmed = true;
    }
    Phase3Outcome { commit_: commit, confirmed_: confirmed }
}

pub fn heartbeat_phase3_body(server: &mut RaftServerBase,
                             authority_rounds: &mut AuthorityLedger,
                             round: &HeartbeatRoundScope) {
    if !server.IsLeader() {
        return;
    }
    let mut commit_advanced_after_send: bool = false;
    {
        let _lock = RaftLockGuard::new(&mut server.mtx_);
        let members: rusty::Vec<u16> = server.config_members_.clone();
        let nservers: usize = round.nservers();
        let is_leader: bool = server.IsLeaderLocked();
        let outcome: Phase3Outcome = heartbeat_phase3_locked(
            &mut server.state_, authority_rounds, nservers, &members,
            is_leader);

        if outcome.commit().advanced() {
            rusty::raft_log_debug_2(
                "[PHASE3-COMMIT] Advancing state_.commit_index_ {} -> {}",
                outcome.commit().from_index(), outcome.commit().to_index());
            server.EnqueueCommittedEntries(outcome.commit().from_index(),
                                           outcome.commit().to_index());
            commit_advanced_after_send = true;
        }
        if outcome.confirmed() {
            rusty::raft_log_debug_3(
                "[READ-INDEX] site={} confirmed round={} term={}",
                server.site_id_, server.state_.read_quorum_confirmed_round_,
                server.state_.read_quorum_confirmed_term_);
        }
    }

    // The AppendEntries messages for this round carried the OLD commit
    // index. Latch exactly one prompt follow-up round so followers learn the
    // phase-3 commit without waiting out the periodic heartbeat.
    if commit_advanced_after_send {
        server.RequestReplication();
    }
}

// ==========================================================================
// THE ROUND DRIVER
//
// Sequences the four phases above. It is here rather than in server.h --
// where it used to be, beside RaftServerBase -- because a DSL block can only
// call what precedes it, and the phase bodies are in this file. Moving it
// deleted the four `raft_heartbeat_phase*` trampolines and the round-state
// cast they existed to perform.
// ==========================================================================

// Everything one round carries between its phases. This was a hand-written
// C++ struct whose four members were already DSL types -- it existed only
// because the DSL has no default member initialisers, which `fn new` solves.
// Being C++ was not free: the phases could not be called with it, so the
// driver passed it as `*mut c_void` and four kernels cast it back and split
// it into members.
pub struct HeartbeatRoundState {
    // At most one AppendEntries in flight per follower. A synchronous
    // follower may legitimately take longer than one heartbeat interval to
    // persist an entry; retaining its context lets a later round consume that
    // acknowledgement instead of queueing duplicate writes and discarding
    // every late success.
    pending_rpcs_: PendingTable,
    authority_rounds_: AuthorityLedger,
    pending_leader_term_: rusty::Option<u64>,
    // PHASE 0 establishes every field of this each round, so it needs no
    // reset; when PHASE 0 declines the round, phases 1-3 never read it.
    scope_: HeartbeatRoundScope,
}

#[allow(clippy::new_without_default)]
impl HeartbeatRoundState {
    pub fn new() -> HeartbeatRoundState {
        HeartbeatRoundState {
            pending_rpcs_: PendingTable::new(),
            authority_rounds_: AuthorityLedger::new(),
            pending_leader_term_: rusty::None,
            scope_: HeartbeatRoundScope::new(),
        }
    }
}

// The driver OWNS its round state rather than pointing at one the caller
// stack-allocated, which is what the `*mut c_void` handle used to be.
pub struct HeartbeatDriver {
    server_: *mut RaftServerBase,
    round_: HeartbeatRoundState,
}

#[allow(clippy::not_unsafe_ptr_arg_deref)]
impl HeartbeatDriver {
    pub fn new(server: *mut RaftServerBase) -> HeartbeatDriver {
        HeartbeatDriver { server_: server, round_: HeartbeatRoundState::new() }
    }

    // decide -> emit -> collect -> decide, which is the shape the C++ already
    // had as four comment-delimited phases. It is now the shape of the Rust
    // that sequences them, and the phases are called directly.
    pub fn run(&mut self) {
        let server: &mut RaftServerBase = unsafe { &mut *self.server_ };
        server.HeartbeatPrologue();
        while server.HeartbeatLooping() {
            // The wake gate returns false on shutdown rather than on timeout.
            if !server.HeartbeatWait() {
                break;
            }
            // PHASE 0 declines the round when leadership is not held. The C++
            // spelled that `continue`.
            if !heartbeat_phase0_body(server,
                                      &mut self.round_.pending_rpcs_,
                                      &mut self.round_.authority_rounds_,
                                      &mut self.round_.pending_leader_term_,
                                      &mut self.round_.scope_) {
                continue;
            }
            heartbeat_phase1_body(server, &mut self.round_.pending_rpcs_,
                                  &mut self.round_.authority_rounds_,
                                  &self.round_.scope_);
            heartbeat_phase2_body(server, &mut self.round_.pending_rpcs_,
                                  &mut self.round_.authority_rounds_,
                                  &self.round_.scope_);
            heartbeat_phase3_body(server, &mut self.round_.authority_rounds_,
                                  &self.round_.scope_);
        }
        server.HeartbeatEpilogue();
    }
}

// The whole loop, so the C++ side is one fiber spawn rather than a method.
#[allow(clippy::not_unsafe_ptr_arg_deref)]
pub fn heartbeat_loop_body(server: *mut RaftServerBase) {
    let mut driver = HeartbeatDriver::new(server);
    driver.run();
}

// ==========================================================================
// INBOUND RequestVote
// ==========================================================================

// OnRequestVote's whole body, as Rust. The caller holds mtx_ for the
// duration, exactly as the C++ did -- the lock stays in C++ because
// RaftCheckedMutex is a C++ type and because moving lock/unlock into a Rust
// body would lose RAII across this function's many early returns.
//
// Two things reach back into unconverted C++ through trampolines, which is
// the same mechanism HeartbeatDriver has used since it landed: doVote, which
// writes the reply and can step the term forward, and
// ElectionLastLogTermLocked, which consults the snapshot boundary. Neither
// is a blocker -- each becomes an ordinary call once its own body converts,
// and the trampoline is deleted then.
//
// The caller also passes `candidate_is_current_voter` rather than this
// reading current_config_: that member is a std::set that has not moved into
// the state struct, and computing the predicate on the C++ side keeps the
// rejection log line's level short-circuit where it belongs.
/// # Safety
///
/// `server` must be a live `RaftServer*`, and the caller must hold that
/// server's `mtx_` for the whole call. Both hold at the only call site,
/// `RaftServer::OnRequestVote`, which takes the lock and passes `this`.
///
/// The handle is not dereferenced here. It is forwarded to the two
/// trampolines below, which cast it back exactly once each.
#[allow(clippy::too_many_arguments)]
// TODO(raft-server-struct): the argument list collapses into &mut self when
// RaftServer is itself a DSL struct; these are its fields, passed separately
// only because the orphan-impl rule forbids `impl RaftServer` today.
pub unsafe fn raft_on_request_vote(
    server: &mut RaftServerBase,
    stopped: bool,
    candidate_is_current_voter: bool,
    lst_log_idx: u64,
    lst_log_term: i64,
    can_id: u16,
    can_term: i64,
    reply_term: &mut i64,
    vote_granted: &mut i8,
) {
    if stopped {
        *reply_term = server.state_.current_term_ as i64;
        *vote_granted = 0;
        return;
    }

    if can_term < 0 || lst_log_term < 0 || !candidate_is_current_voter {
        *reply_term = server.state_.current_term_ as i64;
        *vote_granted = 0;
        return;
    }

    let cur_term = server.state_.current_term_;
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
        server.doVote(lst_log_idx, lst_log_term, can_id, can_term,
                      reply_term, vote_granted, false);
        return;
    }

    // Already voted for someone ELSE this term. Raft allows re-granting to
    // the same candidate, which is why the identity is compared and not just
    // the presence of a vote.
    // u64 here too, for the same reason and so the two comparisons cannot
    // drift apart. Equality happens to be unaffected by the signedness, but
    // relying on that is how the bug above got written.
    if (can_term as u64) == cur_term
        && server.state_.vote_for_ != RAFT_SERVER_INVALID_SITE_ID
        && server.state_.vote_for_ != can_id
    {
        server.doVote(lst_log_idx, lst_log_term, can_id, can_term,
                      reply_term, vote_granted, false);
        return;
    }

    // Every grant, including an idempotent retry, must still carry an
    // up-to-date candidate log. Defensive against damaged or legacy
    // persistent state, and the RequestVote rule in its direct form.
    if server.state_.raft_log_.last_index() < server.state_.snapidx_ {
        panic!("last log index is below the snapshot boundary");
    }
    let lstoff = server.state_.raft_log_.last_index() - server.state_.snapidx_;
    let curlstterm = server.ElectionLastLogTermLocked();
    let curlstidx = server.state_.raft_log_.last_index();
    let candidate_log_is_current = raft_server_candidate_log_is_at_least(
        lst_log_term, curlstterm, lst_log_idx, curlstidx);

    if raft_server_vote_is_idempotent(can_term as u64, cur_term,
                                      server.state_.vote_for_, can_id)
        && candidate_log_is_current
    {
        server.doVote(lst_log_idx, lst_log_term, can_id, can_term,
                      reply_term, vote_granted, true);
        return;
    }

    // Snapshot-aware offset invariant.
    if lstoff + server.state_.snapidx_ != server.state_.raft_log_.last_index() {
        panic!("snapshot offset invariant violated");
    }

    let grant = candidate_log_is_current;
    server.doVote(lst_log_idx, lst_log_term, can_id, can_term,
                  reply_term, vote_granted, grant);
}

// ==========================================================================
// The two inbound RPC bodies, formerly RaftServer::OnRequestVote and
// RaftServer::OnAppendEntries. Both keep a one-line C++ entry point, because
// the rrr service layer calls them by name on RaftServer.
// ==========================================================================
#[allow(clippy::too_many_arguments)]
// Called by RaftServerBase::OnRequestVote (server_h.rs) -- Rust calling Rust
// since F2.7; the C++ forwarder that used to sit between them is gone.
pub fn on_request_vote_body(
    server: &mut RaftServerBase, lst_log_idx: u64,
    lst_log_term: i64, can_id: u16, can_term: i64,
    reply_term: &mut i64, vote_granted: &mut i8) {
    let _lock = RaftLockGuard::new(&mut server.mtx_);
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

    unsafe {
        raft_on_request_vote(server, stopped, candidate_is_current_voter,
                             lst_log_idx, lst_log_term, can_id, can_term,
                             reply_term, vote_granted);
    }
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
// type -- RaftConsensusState, PeerTable, RaftLog, HeartbeatRoundScope,
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
// Cross-carrier note: RaftConsensusState, PeerTable and RaftLog live in
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

/// # Safety
///
/// `server` must be a live `RaftServer*` and `cmd` a live `janus::Command*`
/// that outlives the call, and the caller must hold that server's `mtx_`
/// throughout. All three hold at the only call site,
/// `RaftServer::OnAppendEntries`. Neither handle is dereferenced here; both
/// are forwarded to trampolines that cast back exactly once.
// unnecessary_unwrap: the per-slot lookup below uses is_some()/unwrap() with
// an explicit `&RaftEntry` binding rather than `if let`. `if let` is the
// better Rust and it transpiles, but the emitter renders the binding with a
// dot where the C++ needs an arrow, and an inferred binding COPIES the entry.
#[allow(clippy::too_many_arguments, clippy::unnecessary_unwrap)]
// TODO(raft-server-struct): collapses into &mut self once RaftServer is a
// DSL struct; these are its fields and its RPC arguments.
pub unsafe fn raft_on_append_entries(
    server: &mut RaftServerBase,
    cmd: *const core::ffi::c_void,
    stopped: bool,
    sender_is_current_voter: bool,
    has_cmd: bool,
    leader_current_term: u64,
    leader_site_id: u16,
    leader_prev_log_index: u64,
    leader_prev_log_term: u64,
    leader_commit_index: u64,
    leader_next_log_term: u64,
    follower_append_ok: &mut u64,
    follower_current_term: &mut u64,
    follower_last_log_index: &mut u64,
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
        *follower_current_term = server.state_.current_term_;
        *follower_last_log_index = server.state_.raft_log_.last_index();
        return report;
    }

    let leader_has_higher_term =
        raft_server_observed_higher_term(leader_current_term, server.state_.current_term_);
    let leader_term_is_stale =
        raft_server_vote_term_is_stale(leader_current_term, server.state_.current_term_);
    let sender_is_self = leader_site_id == server.state_.site_id_;
    let has_known_leader = server.state_.current_leader_id_ != RAFT_SERVER_INVALID_SITE_ID;
    let known_leader_matches_sender = server.state_.current_leader_id_ == leader_site_id;
    if !sender_is_current_voter
        || leader_term_is_stale
        || !raft_server_leader_rpc_sender_is_authoritative(
            leader_has_higher_term,
            server.state_.is_leader_,
            sender_is_self,
            has_known_leader,
            known_leader_matches_sender,
        )
    {
        report.unauthoritative_ = true;
        *follower_append_ok = 0;
        *follower_current_term = server.state_.current_term_;
        *follower_last_log_index = server.state_.raft_log_.last_index();
        return report;
    }

    // Decode the wire payload HERE, not before the gates. The original did
    // the marshallable_cast and the count validation at exactly this point,
    // after a stopped server and an unauthoritative sender had already
    // returned. Hoisting it above them would make every rejected
    // AppendEntries pay a dynamic cast and N refcount bumps, on a path a
    // remote peer drives -- and backtracking rejects are common during log
    // repair.
    // AeDecodePayload fills server.decoded_terms_, one term per encoded
    // entry, so its length IS the decoded count.
    let append_payload_valid = server.AeDecodePayload(
        cmd, has_cmd, leader_prev_log_index, leader_next_log_term);
    let decoded_count: u64 = server.decoded_terms_.len() as u64;

    let term_ok =
        raft_server_append_term_is_acceptable(leader_current_term, server.state_.current_term_);
    let compacted_prefix_miss = leader_prev_log_index != 0
        && leader_prev_log_index < server.state_.raft_log_.base()
        && leader_prev_log_index != server.state_.snapidx_;
    let index_ok =
        leader_prev_log_index <= server.state_.raft_log_.last_index() && !compacted_prefix_miss;

    // THE LOG-MATCHING CHECK. A follower legitimately may not hold
    // leaderPrevLogIndex -- discovering that is the point, and what drives
    // the leader's backtracking. An absent entry falls through to term 0 and
    // the mismatch is reported rather than manufactured.
    let mut local_prev_term: u64 = 0;
    if leader_prev_log_index == 0 {
        local_prev_term = 0;
    } else if leader_prev_log_index == server.state_.snapidx_ {
        // The snapshot boundary is still valid when entries are compacted.
        local_prev_term = server.state_.snapterm_ as u64;
    } else if leader_prev_log_index <= server.state_.raft_log_.last_index()
        && !compacted_prefix_miss
        && server.state_.raft_log_.holds(leader_prev_log_index)
    {
        local_prev_term =
            server.state_.raft_log_.get(leader_prev_log_index).unwrap().term() as u64;
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
        if raft_server_observed_higher_term(leader_current_term, server.state_.current_term_) {
            let prev_term = server.state_.current_term_;
            server.state_.current_term_ = leader_current_term;
            server.state_.vote_for_ = RAFT_SERVER_INVALID_SITE_ID;
            // Publish the accepted leader before any leader-change callback
            // can observe the follower transition.
            server.state_.current_leader_id_ = raft_server_leader_hint_after_transition(
                false, true, server.state_.site_id_, leader_site_id);
            let now_term: u64 = server.state_.current_term_;
            server.LogTermChange("AppendEntries leader term is newer",
                                 prev_term, now_term, leader_site_id);
            if server.state_.is_leader_ {
                // The central transition, so no leadership state survives an
                // accepted competing leader epoch.
                server.stepDown();
            } else {
                server.setIsLeader(false);
            }
            server.state_.req_voting_ = false;
            server.state_.election_in_progress_ = false;
        }
        // Refresh the hint for current-term contact too; a higher-term sender
        // was already published above, before its role transition.
        server.state_.current_leader_id_ = raft_server_leader_hint_after_transition(
            false, true, server.state_.site_id_, leader_site_id);
        server.resetTimerLocked("AppendEntries from current-term leader");
    }

    if !(raft_server_append_is_acceptable(term_ok, index_ok, prev_term_ok)
         && append_payload_valid)
    {
        *follower_append_ok = 0;
        *follower_current_term = server.state_.current_term_;
        *follower_last_log_index = server.state_.raft_log_.last_index();
        return report;
    }

    // Any accepted leader RPC establishes follower state even in our current
    // term. Cancel an outstanding election before its delayed result can
    // promote this server after the accepted AppendEntries.
    if server.state_.is_leader_ {
        server.stepDown();
    } else {
        server.setIsLeader(false);
    }
    server.state_.req_voting_ = false;
    server.state_.election_in_progress_ = false;

    let old_last_log_index = server.state_.raft_log_.last_index();
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
        let slot = server.state_.raft_log_.get(index);
        if slot.is_some() {
            let entry: &RaftEntry = slot.unwrap();
            local_exists = unsafe {
                raft_command_has_value(entry.cmd() as *const rusty::RaftCommand)
            };
            if local_exists {
                local_term = entry.term();
            }
        }
        // In bounds by construction: decoded_count IS decoded_terms_.len(),
        // the loop condition is i < decoded_count, and nothing in the body
        // touches the vector. The kernel this replaces needed a verify only
        // because i arrived across the language boundary.
        let incoming_term: i64 = server.decoded_terms_[i as usize];
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
        && first_write_index <= (if server.state_.commit_index_ > server.state_.execute_index_ {
               server.state_.commit_index_
           } else {
               server.state_.execute_index_
           })
    {
        // A legitimate leader never conflicts with a committed entry. Do not
        // let malformed or internally inconsistent input rewrite applied
        // state; reject before memory changes.
        report.refused_committed_conflict_ = true;
        report.conflict_index_ = first_write_index;
        *follower_append_ok = 0;
        *follower_current_term = server.state_.current_term_;
        *follower_last_log_index = server.state_.raft_log_.last_index();
        return report;
    }

    if have_first_write {
        // Two operations that cannot leave a hole: drop the divergent suffix,
        // then re-append in index order. truncate_from is a no-op when
        // first_write_index is already past the tail, the ordinary extend
        // case. The append is Rust; only the per-entry reads of the wire
        // payload are kernels.
        server.state_.raft_log_.truncate_from(first_write_index);
        server.AeApplyIncoming(cmd, leader_prev_log_index, leader_next_log_term,
                               first_write_index);
    }
    if server.state_.raft_log_.last_index()
        != raft_server_append_result_last_index(old_last_log_index, accepted_through,
                                                truncate_suffix)
    {
        panic!("append left the log tail somewhere the result rule did not predict");
    }

    let follower_commit_candidate =
        raft_server_commit_index_clamp(leader_commit_index, accepted_through);
    if raft_server_log_index_above(follower_commit_candidate, server.state_.commit_index_) {
        let old_commit = server.state_.commit_index_;
        server.state_.commit_index_ = follower_commit_candidate;
        if server.state_.raft_log_.last_index() < server.state_.commit_index_ {
            panic!("commit index advanced past the log tail");
        }
        let new_commit: u64 = server.state_.commit_index_;
        server.EnqueueCommittedEntries(old_commit, new_commit);
    }

    *follower_append_ok = 1;
    *follower_current_term = server.state_.current_term_;
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
    let _lock = RaftLockGuard::new(&mut server.mtx_);

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
    let report: AppendReport = unsafe {
        raft_on_append_entries(
            server, cmd, stopped, sender_is_current_voter,
            cmd_has_value, leader_current_term, leader_site_id,
            leader_prev_log_index, leader_prev_log_term, leader_commit_index,
            leader_next_log_term, follower_append_ok, follower_current_term,
            follower_last_log_index)
    };

    if !stopped && !report.accepted() {
        if report.refused_committed_conflict() {
            // A legitimate leader can never conflict with a committed entry.
            rusty::raft_log_error_4(
                "[APPEND_REJECT] Site {} refusing conflict at committed index {} (commit_index={}, execute_index={})",
                server.site_id_, report.conflict_index(),
                server.state_.commit_index_, server.state_.execute_index_);
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
                server.state_.current_term_,
                server.state_.current_leader_id_, sender_is_current_voter);
        } else {
            rusty::raft_log_info_10(
                "[APPEND_REJECT] Site {} rejecting AppendEntries from leader {} - term_ok={} index_ok={} prev_term_ok={} (leaderTerm={} myTerm={} prevIdx={} myLastIdx={} local_prev_term={})",
                server.site_id_, leader_site_id, report.term_ok(),
                report.index_ok(), report.prev_term_ok(), leader_current_term,
                server.state_.current_term_, leader_prev_log_index,
                server.state_.raft_log_.last_index(),
                report.local_prev_term());
        }
    }
}

// ==========================================================================
// THE C ABI OVER RaftServerBase (plan.md, step F1)
//
// One extern "C" function per behaviour the hand-written C++ reaches -- the
// RaftServer shim's interface forwarders, the kernels' callbacks, the RaftLab
// harness -- each forwarding to the method. Generated by
// scripts/raft_gen_exports.py; the matching prototypes are server_exports.h.
// Emitted as extern "C" C++ today; at the cutover these are the crate's
// exported symbols and nothing else of the struct is visible to C++.
// ==========================================================================
use crate::scheduler_h::RaftStartResult;
use crate::scheduler_h::TxLogServer;
use crate::server_h::GateWakeJob;
use crate::server_h::ElectionTimerLoop;
// --- GENERATED EXPORTS BEGIN (scripts/raft_gen_exports.py; do not edit by hand) ---
// --- Lifetime. Rust allocates and frees (F2.6): the struct is a Box the shim
// holds as a raw pointer between these two calls.
/// # Safety
/// The returned pointer is owned by the caller until raft_server_delete.
#[no_mangle]
pub unsafe extern "C" fn raft_server_new() -> *mut RaftServerBase {
    let s: *mut RaftServerBase = rusty::Box::into_raw(rusty::Box::new(RaftServerBase::new()));
    (*s).ConstructRuntime();
    s
}

/// # Safety
/// `s` came from raft_server_new and is not used afterwards.
#[no_mangle]
pub unsafe extern "C" fn raft_server_delete(s: *mut RaftServerBase) {
    (*s).Shutdown();
    drop(rusty::Box::from_raw(s));
}

// --- The two fiber loops, entered from the spawn kernels.
/// # Safety
/// `s` is a live `RaftServerBase`; runs on the calling fiber until shutdown.
#[no_mangle]
pub unsafe extern "C" fn raft_server_heartbeat_loop(s: *mut RaftServerBase) {
    heartbeat_loop_body(s)
}

/// # Safety
/// `s` is a live `RaftServerBase`; runs on the calling fiber until shutdown.
#[no_mangle]
pub unsafe extern "C" fn raft_server_run_election_timer_loop(s: *mut RaftServerBase,
                                                              wait_int_us: u64) {
    let timer: ElectionTimerLoop = ElectionTimerLoop::new(s, wait_int_us);
    timer.run()
}

// --- The wake job, entered from the reactor's OneTimeJob (raft_queue_wake_job).
/// # Safety
/// `token` is the Box<GateWakeJob> RaftServerBase::queue_wake_job made raw,
/// handed back exactly once.
#[no_mangle]
pub unsafe extern "C" fn raft_wake_job_run(token: *mut core::ffi::c_void) {
    let job: rusty::Box<GateWakeJob> = rusty::Box::from_raw(token as *mut GateWakeJob);
    job.run();
}

// --- The replication interface: TxLogServer and RaftSpecific.

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_set_site_identity(s: *mut RaftServerBase,
                                                       loc_id: u32,
                                                       site_id: u16,
                                                       partition_id: u32) {
    (*s).set_site_identity(loc_id, site_id, partition_id)
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_set_commo(s: *mut RaftServerBase,
                                               commo: *mut rusty::Communicator) {
    (*s).set_commo(commo)
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_reg_learner_action(s: *mut RaftServerBase,
                                                        learner_action: *const rusty::LearnerAction) {
    (*s).reg_learner_action(&*learner_action)
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_ensure_setup(s: *mut RaftServerBase) {
    (*s).EnsureSetup()
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_wait_for_startup(s: *mut RaftServerBase) -> bool {
    (*s).WaitForStartup()
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_prepare_for_shutdown(s: *mut RaftServerBase) {
    (*s).PrepareForShutdown()
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_is_leader(s: *mut RaftServerBase) -> bool {
    (*s).IsLeader()
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_get_leader_hint(s: *mut RaftServerBase) -> u16 {
    (*s).GetLeaderHint()
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_set_preferred_leader(s: *mut RaftServerBase,
                                                          site_id: u16) {
    (*s).SetPreferredLeader(site_id)
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_register_leader_change_callback(s: *mut RaftServerBase,
                                                                     cb: *const rusty::RaftLeaderChangeCb) {
    (*s).RegisterLeaderChangeCallback(&*cb)
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_is_rpc_ready(s: *const RaftServerBase) -> bool {
    (*s).IsRpcReady()
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_is_disconnected(s: *const RaftServerBase) -> bool {
    (*s).IsDisconnected()
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_site_id(s: *const RaftServerBase) -> u16 {
    (*s).SiteId()
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_partition_id(s: *const RaftServerBase) -> u32 {
    (*s).PartitionId()
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_commit_index(s: *const RaftServerBase) -> u64 {
    (*s).CommitIndex()
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_start(s: *mut RaftServerBase,
                                           cmd: *const rusty::RaftCommand,
                                           index: *mut u64,
                                           term: *mut u64) -> RaftStartResult {
    (*s).Start(&*cmd, index, term)
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_on_request_vote(s: *mut RaftServerBase,
                                                     lst_log_idx: u64,
                                                     lst_log_term: i64,
                                                     can_id: u16,
                                                     can_term: i64,
                                                     reply_term: *mut i64,
                                                     vote_granted: *mut i8) {
    (*s).OnRequestVote(lst_log_idx, lst_log_term, can_id, can_term, reply_term, vote_granted)
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_on_append_entries(s: *mut RaftServerBase,
                                                       leader_current_term: u64,
                                                       leader_site_id: u16,
                                                       leader_prev_log_index: u64,
                                                       leader_prev_log_term: u64,
                                                       leader_commit_index: u64,
                                                       cmd: *const rusty::RaftCommand,
                                                       leader_next_log_term: u64,
                                                       follower_append_ok: *mut u64,
                                                       follower_current_term: *mut u64,
                                                       follower_last_log_index: *mut u64) {
    (*s).OnAppendEntries(leader_current_term, leader_site_id, leader_prev_log_index, leader_prev_log_term, leader_commit_index, &*cmd, leader_next_log_term, follower_append_ok, follower_current_term, follower_last_log_index)
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_on_install_snapshot(s: *mut RaftServerBase,
                                                         term: u64,
                                                         leader_id: u64,
                                                         last_included_index: u64,
                                                         last_included_term: u64,
                                                         data: *const rusty::RaftByteString,
                                                         term_out: *mut u64) {
    (*s).OnInstallSnapshot(term, leader_id, last_included_index, last_included_term, &*data, term_out)
}

// --- What the kernels in server.cc call back into.

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_apply_thread_loop(s: *mut RaftServerBase) {
    (*s).ApplyThreadLoop()
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_bind_replication_wake_owner(s: *mut RaftServerBase,
                                                                 owner: *const rusty::RaftPollThreadPtr) {
    let owner_copy: rusty::RaftPollThreadPtr = (*owner).clone();
    (*s).BindReplicationWakeOwner(owner_copy)
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_fail_stop(s: *mut RaftServerBase) {
    (*s).FailStop()
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_initialize_snapshot_manager_locked(s: *mut RaftServerBase) -> bool {
    (*s).InitializeSnapshotManagerLocked()
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_install_snapshot_reply_accepted(s: *mut RaftServerBase,
                                                                     site_id: u16,
                                                                     ord: usize,
                                                                     snap_last_idx: u64,
                                                                     send_term: u64,
                                                                     follower_term: u64) {
    (*s).InstallSnapshotReplyAccepted(site_id, ord, snap_last_idx, send_term, follower_term)
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_on_install_snapshot_locked(s: *mut RaftServerBase,
                                                                term: u64,
                                                                leader_id: u64,
                                                                last_included_index: u64,
                                                                last_included_term: u64,
                                                                data: *const rusty::RaftByteString,
                                                                term_out: *mut u64) {
    (*s).OnInstallSnapshotLocked(term, leader_id, last_included_index, last_included_term, data, term_out)
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_setup_internal(s: *mut RaftServerBase) -> bool {
    (*s).SetupInternal()
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_start_election_timer(s: *mut RaftServerBase) {
    (*s).StartElectionTimer()
}

// --- GENERATED EXPORTS END ---
