pub struct ReplicationWakeGate {
    owner_: rusty::Mutex<rusty::Option<rusty::sync::Arc<rusty::ReactorPollThread>>>,
    waiter_: rusty::Mutex<rusty::Option<rusty::sync::Arc<rusty::ReactorIntEvent>>>,
    election_waiter_: rusty::Mutex<rusty::Option<rusty::sync::Arc<rusty::ReactorIntEvent>>>,
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

    pub fn bind_owner(&self, owner: rusty::sync::Arc<rusty::ReactorPollThread>) {
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
    ) -> rusty::Option<rusty::sync::Arc<rusty::ReactorPollThread>> {
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
        (*guard).clone()
    }

    pub fn reserve_shutdown_wake_owner(
        &self,
    ) -> rusty::Option<rusty::sync::Arc<rusty::ReactorPollThread>> {
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
        (*guard).clone()
    }

    // TODO(raft-dsl): drop this allow once the emitter lowers an `if let`
    // binding of an Option<Arc<T>> THROUGH the Arc. Clippy's suggested
    // `if let rusty::Some(event) = &waiter { event.set(1) }` is the better
    // Rust, and it transpiles, but the binding is emitted as `event.set(1)`
    // on a `rusty::Arc<rrr::IntEvent>` -- a dot, not an arrow -- which does
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
        let waiter: rusty::Option<rusty::sync::Arc<rusty::ReactorIntEvent>> =
            (*self.waiter_.lock().unwrap()).clone();
        if waiter.is_some() {
            waiter.as_ref().unwrap().set(1);
        }
    }

    // See the TODO on wake_on_owner for why this is not `if let`.
    #[allow(clippy::unnecessary_unwrap)]
    pub fn wake_shutdown_on_owner(&self) {
        // Both guards are statement temporaries; neither lock is held across
        // the set() calls below, for the reason given in wake_on_owner.
        let heartbeat_waiter: rusty::Option<rusty::sync::Arc<rusty::ReactorIntEvent>> =
            (*self.waiter_.lock().unwrap()).clone();
        let election_waiter: rusty::Option<rusty::sync::Arc<rusty::ReactorIntEvent>> =
            (*self.election_waiter_.lock().unwrap()).clone();
        if heartbeat_waiter.is_some() {
            heartbeat_waiter.as_ref().unwrap().set(1);
        }
        if election_waiter.is_some() {
            election_waiter.as_ref().unwrap().set(1);
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
        waiter: rusty::sync::Arc<rusty::ReactorIntEvent>,
        timeout_us: u64,
    ) -> bool {
        waiter.set(0);
        {
            let mut guard = self.waiter_.lock().unwrap();
            *guard = rusty::Some(waiter.clone());
        }
        self.waiter_armed_.store(true, rusty::sync::atomic::Ordering::Release);
        if self.pending_.swap(false, rusty::sync::atomic::Ordering::AcqRel) {
            self.disarm_waiter();
            return self.accepting_.load(rusty::sync::atomic::Ordering::Acquire);
        }
        waiter.wait_timeout(timeout_us);
        self.pending_.swap(false, rusty::sync::atomic::Ordering::AcqRel);
        self.disarm_waiter();
        self.accepting_.load(rusty::sync::atomic::Ordering::Acquire)
    }

    pub fn wait_for_election_timeout(
        &self,
        waiter: rusty::sync::Arc<rusty::ReactorIntEvent>,
        timeout_us: u64,
    ) -> bool {
        waiter.set(0);
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
        waiter.wait_timeout(timeout_us);
        self.disarm_election_waiter();
        self.accepting_.load(rusty::sync::atomic::Ordering::Acquire)
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
use crate::server_h::RaftConsensusState;
use crate::server_h::PeerTable;
use crate::server_h::RaftLog;

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

pub fn raft_commit_advance(
    consensus: &mut RaftConsensusState,
    peers: &PeerTable,
    log: &RaftLog,
    nservers: usize,
) -> CommitAdvance {
    // nservers is the value latched in PHASE 0. Reusing it in PHASE 3 is
    // sound only because current_config_ has exactly one write, during
    // Setup, and progress_ is never erased, so the size is invariant across
    // the round. Assert it rather than trusting the phases to stay in step.
    if peers.len() != nservers - 1 {
        panic!("peer table and round membership disagree");
    }
    let candidate_index = peers.majority_match_index(nservers, log.last_index());
    if !raft_server_log_index_above(candidate_index, consensus.commit_index_) {
        return CommitAdvance { advanced_: false, from_: 0, to_: 0 };
    }
    // The candidate is <= last_index() and > commit_index_, so the entry
    // provably exists. This says so rather than leaving a null dereference
    // to express it.
    let candidate = log.get(candidate_index);
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

pub fn heartbeat_phase3_locked(
    consensus: &mut RaftConsensusState,
    peers: &PeerTable,
    log: &RaftLog,
    ledger: &mut AuthorityLedger,
    nservers: usize,
    members: &[u16],
    is_leader: bool,
) -> Phase3Outcome {
    let commit = raft_commit_advance(consensus, peers, log, nservers);
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
pub fn heartbeat_phase0_locked(
    consensus: &mut RaftConsensusState,
    peers: &PeerTable,
    log: &RaftLog,
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
    if pending.len() != peers.len() {
        pending.resize(peers.len());
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
    let advance = raft_commit_advance(consensus, peers, log, round.nservers());
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
