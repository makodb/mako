// RaftCore -- the state the protocol decides over -- and the core calls
// made on it from the election and the inbound RPC handlers.
//
// [move, M1] From src/deptran/raft/src/server_h.rs (Phase 6); the
// command is the type parameter C (M11) and logging is records in the
// output (M7).

#[allow(unused_imports)]
use crate::*;
#[allow(unused_imports)]
use vstd::pervasive::runtime_assert;
use vstd::prelude::*;

verus! {

#[repr(C)]
pub struct RaftCore<C> {
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
    pub raft_log_: RaftLog<C>,
    pub peers_: PeerTable,
    // Election cluster.
    pub election_term_: i64,
    pub election_timeout_us_: u64,
    pub election_timer_generation_: u64,
    pub vote_for_: u16,
    // Leadership, and the campaign in progress.
    pub is_leader_: bool,
    pub req_voting_: bool,
    pub election_in_progress_: bool,
    pub current_leader_id_: u16,
    pub last_heartbeat_time_: u64,
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
    pub pending_leader_term_: Option<u64>,
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
    pub config_members_: Vec<u16>,
    // Ordinal peer table, rebuilt whenever the configuration changes.
    pub peer_sites_: Vec<u16>,
    // [fix, F5] Whether Setup found the verified configuration
    // (enter_gates): snapshots off and a whole log, failover on, and a
    // configuration that contains this server. Set once, never cleared.
    pub gated_: bool,
    // [move, M1] raft_on_append_entries' decode scratch, from
    // RaftServerBase: one term per encoded entry of the append being
    // handled, valid only inside that call. Kept across calls so a decode
    // never allocates.
    pub decoded_terms_: Vec<i64>,
    // [M12] The proof's state (plan Phase 8; Verus only, so a plain build has
    // none of it): the ghost log of the core's actions, and the spec fields
    // the core keeps no copy of -- the votes a campaign holds (V1), the
    // leader's match and next tables as the spec sees them (V2, V3).
    #[cfg(verus_keep_ghost)]
    pub g_log_: Ghost<Seq<glr::protocol::Raft::ghost_log::Entry>>,
    #[cfg(verus_keep_ghost)]
    pub g_votes_: Ghost<Set<int>>,
    #[cfg(verus_keep_ghost)]
    pub g_match_: Ghost<Map<u64, u64>>,
    #[cfg(verus_keep_ghost)]
    pub g_next_: Ghost<Map<u64, u64>>,
}

impl<C> RaftCore<C> {
    // What every core call keeps (ghost): the configuration's facts below,
    // and a peer table that is the configuration without this server.
    pub open spec fn inv(&self) -> bool {
        &&& self.inv_config()
        &&& self.inv_peers()
    }

    // The peer table and its site list, of one length, are the
    // configuration without this server.
    pub open spec fn inv_peers(&self) -> bool {
        &&& self.peers_.spec_len() == self.peer_sites_@.len()
        &&& self.peer_sites_@.len()
                + (if self.config_members_@.contains(self.site_id_) { 1int } else { 0int })
                == self.config_members_@.len()
    }

    // Everything else (ghost): the log's layout, the term below the index
    // ceiling, snapidx_ <= commit_index_ <= the last index with the log
    // starting at or below the snapshot boundary's successor, a sorted
    // configuration, the round state, and the gate's facts.
    pub open spec fn inv_config(&self) -> bool {
        &&& self.raft_log_.wf()
        &&& (self.current_term_ as int) < raft_index_limit()
        &&& self.snapidx_ <= self.commit_index_
        &&& (self.commit_index_ as int) <= self.raft_log_.spec_last_index()
        &&& self.raft_log_.spec_base() <= self.snapidx_ as int + 1
        &&& sites_sorted(self.config_members_@)
        &&& self.authority_rounds_.wf()
        &&& self.round_.wf()
        // an opened round admitted exactly the configuration
        &&& (self.round_.spec_nservers() == 0 || {
            &&& self.round_.spec_nservers() == self.config_members_@.len()
            &&& forall|x: u16| self.round_.spec_is_member(x) == self.config_members_@.contains(x)
        })
        // round ids are fresh within a leadership epoch
        &&& (self.heartbeat_round_ == u64::MAX
            || self.pending_leader_term_ != Some(self.current_term_)
            || self.authority_rounds_.spec_ids_below(self.heartbeat_round_))
        // a leader runs no campaign; an epoch's term never passes the
        // current term, and a campaign's is strictly above it
        &&& (self.is_leader_ ==> !self.election_in_progress_)
        // [M12] a campaign's term is a term (start_election's copy of one)
        &&& self.election_term_ >= 0
        &&& (self.pending_leader_term_ matches Some(t) ==> t <= self.current_term_)
        &&& (self.election_in_progress_ ==> (self.pending_leader_term_ matches Some(t) ==> t < self.current_term_))
        // the gate (F5): inside the verified configuration the log is whole
        // and the configuration contains this server
        &&& (self.gated_ ==> {
            &&& self.config_members_@.contains(self.site_id_)
            &&& self.snapidx_ == 0
            &&& self.raft_log_.spec_base() == 1
        })
    }
}

#[allow(clippy::new_without_default)]
impl<C: Clone> RaftCore<C> {
    pub fn new() -> (r: RaftCore<C>)
        ensures
            r.inv(),
            r.fresh(),  // [M12] (coupling::lemma_new_ginv: ginv)
    {
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
            pending_leader_term_: None,  // [move, M1]
            round_: HeartbeatRoundScope::new(),  // [move, M1]
            config_members_: Vec::new(),  // [move, M1]
            peer_sites_: Vec::new(),  // [move, M1]
            gated_: false,  // [fix, F5]
            decoded_terms_: Vec::new(),  // [move, M1]
            #[cfg(verus_keep_ghost)]
            g_log_: Ghost(Seq::empty()),  // [M12]
            #[cfg(verus_keep_ghost)]
            g_votes_: Ghost(Set::empty()),  // [M12]
            #[cfg(verus_keep_ghost)]
            g_match_: Ghost(Map::empty()),  // [M12]
            #[cfg(verus_keep_ghost)]
            g_next_: Ghost(Map::empty()),  // [M12]
        }
    }

    // [move, M1] set_site_identity's core half: the three ids, written once
    // by the worker before Setup, while the configuration is still empty.
    pub fn set_identity(&mut self, loc_id: u32, site_id: u16, partition_id: u32)
        requires
            old(self).inv(),
            // the host contract: the identity comes before the configuration
            old(self).config_members_@.len() == 0,
        ensures
            final(self).inv(),
            final(self).site_id_ == site_id,
            // [M12] nothing the spec sees moves (no membership yet, and no
            // action taken: the role a vote would bind is Follower)
            final(self).g_log_ == old(self).g_log_,
            old(self).ginv() && old(self).g_log_@.len() == 0 ==> final(self).ginv(),
    {
        proof {
            if old(self).g_log_@.len() == 0 {
                assert(old(self).g_log_@ =~= Seq::<glr::protocol::Raft::ghost_log::Entry>::empty());
            }
        }
        self.loc_id_ = loc_id;
        self.site_id_ = site_id;
        self.partition_id_ = partition_id;
    }

    // [move, M1] (whole item) LoadCurrentConfig's write: the partition's members, as the
    // config kernel lists them. [fix, F5] The peer table is built in the
    // same call (next index 1, as HeartbeatPrologue builds it), so the core
    // is consistent from Setup on rather than from the heartbeat loop's
    // first round; HeartbeatPrologue's rebuild still runs and finds the
    // same table.
    pub fn configure(&mut self, members: &[u16])
        requires
            old(self).inv(),
            !old(self).gated_,
            // the host contract: once, at Setup, before any round opened
            old(self).round_.spec_nservers() == 0,
            // the config kernel's contract: sorted and duplicate-free
            sites_sorted(members@),
        ensures
            final(self).inv(),
            final(self).config_members_@ == members@,
            final(self).site_id_ == old(self).site_id_,
            // [M12] the LoadConfig segment, on a core no action has touched
            // (the host contract: no member is the sentinel site)
            old(self).ginv() && old(self).g_log_@.len() == 0
                && !members@.contains(RAFT_SERVER_INVALID_SITE_ID) ==> final(self).ginv(),
    {
        let ghost on = self.ginv() && self.g_log_@.len() == 0
            && !members@.contains(RAFT_SERVER_INVALID_SITE_ID);
        self.config_members_.clear();
        let mut i: usize = 0;
        while i < members.len()
            invariant
                i <= members@.len(),
                self.config_members_@ == members@.subrange(0, i as int),
                *self == (RaftCore { config_members_: self.config_members_, ..*old(self) }),
            decreases members@.len() - i,
        {
            self.config_members_.push(members[i]);
            i += 1;
        }
        proof {
            assert(members@.subrange(0, members@.len() as int) == members@);
        }
        self.rebuild_peer_tables(1);
        proof {
            if on {
                let ghost pre = *old(self);
                self.g_log_@ = crate::coupling::load_config_log(self.n_view());
                crate::coupling::lemma_load_config(&pre, self);
            }
        }
    }

    // [fix, F5] (whole item) verified_config_ok's decision, made by the core (the plan's
    // new_gated): snapshots off, a whole log (no snapshot boundary, base
    // 1), failover on, and a configuration containing this server. When it
    // holds the core remembers it, and its invariant carries the facts.
    pub fn enter_gates(&mut self, snapshots_enabled: bool, failover: bool) -> (r: bool)
        requires old(self).inv(),
        ensures
            final(self).inv(),
            r ==> final(self).gated_,
            old(self).ginv() ==> final(self).ginv(),  // [M12] unseen by the spec
    {
        let whole_log: bool = self.snapidx_ == 0 && self.raft_log_.base() == 1;
        let contains_self: bool = self.is_config_member(self.site_id_);
        let ok: bool = !snapshots_enabled && whole_log && failover && contains_self;
        if ok {
            self.gated_ = true;
        }
        ok
    }

    // [move, M1] What a fresh HeartbeatDriver used to start with: each run of
    // the heartbeat loop begins with an empty round state, as before.
    pub fn reset_round_state(&mut self)
        requires old(self).inv(),
        ensures
            final(self).inv(),
            old(self).ginv() ==> final(self).ginv(),  // [M12] unseen by the spec
    {
        self.pending_rpcs_ = PendingTable::new();
        self.authority_rounds_ = AuthorityLedger::new();
        self.pending_leader_term_ = None;
        self.round_ = HeartbeatRoundScope::new();
    }

    // [move, M1] RaftServerBase::RebuildPeerTables. Rebuilds the ordinal
    // peer table from the configuration.
    pub fn rebuild_peer_tables(&mut self, next_index: u64)
        requires old(self).inv_config(),
        ensures
            final(self).inv(),
            final(self).raft_log_ == old(self).raft_log_,
            final(self).current_term_ == old(self).current_term_,
            final(self).commit_index_ == old(self).commit_index_,
            final(self).snapidx_ == old(self).snapidx_,
            final(self).election_in_progress_ == old(self).election_in_progress_,
            final(self).pending_leader_term_ == old(self).pending_leader_term_,
            final(self).config_members_ == old(self).config_members_,
            final(self).site_id_ == old(self).site_id_,
            final(self).round_ == old(self).round_,
            final(self).is_leader_ == old(self).is_leader_,
            final(self).heartbeat_round_ == old(self).heartbeat_round_,
            final(self).authority_rounds_ == old(self).authority_rounds_,
            final(self).pending_rpcs_ == old(self).pending_rpcs_,
            final(self).decoded_terms_ == old(self).decoded_terms_,
            final(self).g_log_ == old(self).g_log_,  // [M12]
            final(self).g_votes_ == old(self).g_votes_,  // [M12]
            final(self).g_match_ == old(self).g_match_,  // [M12]
            final(self).g_next_ == old(self).g_next_,  // [M12]
            final(self).vote_for_ == old(self).vote_for_,
            final(self).election_term_ == old(self).election_term_,
            final(self).snapterm_ == old(self).snapterm_,
            final(self).gated_ == old(self).gated_,
    {
        self.peer_sites_.clear();
        let mut self_is_a_member: bool = false;
        let mut i: usize = 0;
        while i < self.config_members_.len()
            invariant
                i <= self.config_members_@.len(),
                self.config_members_@ == old(self).config_members_@,
                sites_sorted(self.config_members_@),
                // the sites copied so far: config[0..i] without this site
                self.peer_sites_@.len() + (if self_is_a_member { 1int } else { 0int }) == i,
                self_is_a_member <==> exists|k: int| 0 <= k < i && self.config_members_@[k] == self.site_id_,
                self.site_id_ == old(self).site_id_,
            decreases self.config_members_@.len() - i,
        {
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
        runtime_assert(self.peers_.len() == expected);  // [move, M10]
    }

    // [move, M1] The site id at an ordinal of the peer table.
    pub fn peer_site_at(&self, ordinal: usize) -> u16
        requires ordinal < self.peer_sites_@.len(),
    {
        self.peer_sites_[ordinal]
    }

    // [move, M1] `current_config_.count(site) != 0`, over the cached vector.
    // A linear scan of three to five sorted u16s, which is what the std::set
    // lookup it replaces cost anyway.
    pub fn is_config_member(&self, site: u16) -> (r: bool)
        ensures r == self.config_members_@.contains(site),
    {
        let mut i: usize = 0;
        while i < self.config_members_.len()
            invariant
                i <= self.config_members_@.len(),
                forall|k: int| 0 <= k < i ==> self.config_members_@[k] != site,
            decreases self.config_members_@.len() - i,
        {
            if self.config_members_[i] == site {
                proof { assert(self.config_members_@[i as int] == site); }
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
    pub fn peer_ordinal(&self, site: u16) -> (r: usize)
        requires self.inv(),
        ensures r <= self.peers_.spec_len(),
    {
        let mut ord: usize = 0;
        while ord < self.peer_sites_.len()
            invariant
                ord <= self.peer_sites_@.len(),
                self.peer_sites_@.len() == self.peers_.spec_len(),
            decreases self.peer_sites_@.len() - ord,
        {
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
    pub fn append_local(&mut self, cmd: C, has_value: bool,
                        is_tpc_commit: bool, kind: i32,
                        payload_bytes: u64) -> u64
        requires
            old(self).inv(),
            // the host contract: no index reaches the ceiling
            old(self).raft_log_.spec_has_room(),
        ensures
            final(self).inv(),
            // [M12] a leader's proposal is LClientRequest (the host contract:
            // the shell proposes only while leading, under mtx_)
            old(self).ginv() && old(self).is_leader_ ==> final(self).ginv(),
    {
        let ghost pre = *self;
        let previous_index: u64 = self.raft_log_.last_index();
        let appended: u64 = self.raft_log_.append(RaftEntry::new(
            self.current_term_ as i64, cmd, has_value, is_tpc_commit, kind,
            payload_bytes));
        runtime_assert(appended == previous_index + 1);  // [move, M10]
        proof {
            if pre.ginv() && pre.is_leader_ {
                let e = self.raft_log_.view().last();
                assert(self.raft_log_.view() == pre.raft_log_.view().push(e));
                self.g_log_@ = crate::coupling::client_request_log(
                    pre.g_log_@, self.log_view(), crate::coupling::value_view(e.spec_cmd()));
                crate::coupling::lemma_client_request_ginv(&pre, self, e);
            }
        }
        previous_index
    }

    // [move, M1] RaftServerBase::LogTermChange's body (a log line), so a core
    // event can report its term changes itself.
    pub fn log_term_change(&self, reason: &'static str, old_term: u64,
                           new_term: u64, source: u16,
                           out: &mut CoreOutput) {  // [move, M7]
        if old_term == new_term {
            return;
        }
        if source != RAFT_SERVER_INVALID_SITE_ID {
            out.log(RAFT_LOG_INFO,
                "[RAFT-TERM] server {} term {} -> {} ({}, source_site={})",
                &[(self.site_id_).arg(),
                 (old_term).arg(),
                 (new_term).arg(),
                 (reason).arg(),
                 (source).arg()]);
        } else {
            out.log(RAFT_LOG_INFO,
                "[RAFT-TERM] server {} term {} -> {} ({})",
                &[(self.site_id_).arg(),
                 (old_term).arg(),
                 (new_term).arg(),
                 (reason).arg()]);
        }
    }

    // [move, M1] RaftServerBase::ElectionLastLogTermLocked: the term of the
    // last log entry, or the snapshot boundary term when the log has been
    // compacted past it. The caller holds mtx_.
    pub fn election_last_log_term(&self) -> (r: i64)
        requires self.inv(),
        ensures
            // [M12] under the gate: the last entry's term, or the boundary's
            // when the log is empty
            self.gated_ ==> r as int == crate::coupling::last_term_of(self),
    {
        proof { self.raft_log_.lemma_wf_bounds(); }
        let last_index: u64 = self.raft_log_.last_index();
        runtime_assert(last_index >= self.snapidx_);  // [move, M10]
        if raft_server_election_last_log_uses_snapshot(
            last_index, self.snapidx_)
        {
            return self.snapterm_;
        }
        // The C++ went through FindRaftInstance, which flattened the Option
        // to a raw pointer and then verified it non-null. Asking the log
        // directly is the same lookup with the check kept.
        let last_log = self.raft_log_.get(last_index);
        runtime_assert(last_log.is_some());  // [move, M10]
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
                   out: &mut CoreOutput)
        requires
            old(self).inv(),
            // the host contract: a term off the wire is below the ceiling
            (can_term as int) < raft_index_limit(),
        ensures
            final(self).inv(),
            // [M12] a higher term first (the term, no vote, a follower with
            // no campaign), then the vote; the answer is the term it ends
            // at, and the decision
            *final(reply_term) as int == final(self).current_term_ as int,
            (*final(vote_granted) != 0) == vote,
            raft_server_signed_term_is_newer(can_term, old(self).current_term_) ==> {
                &&& final(self).current_term_ as int == can_term as int
                &&& !final(self).is_leader_
                &&& !final(self).election_in_progress_
            },
            !raft_server_signed_term_is_newer(can_term, old(self).current_term_) ==> {
                &&& final(self).current_term_ == old(self).current_term_
                &&& final(self).election_in_progress_ == old(self).election_in_progress_
                &&& (vote ==> !final(self).is_leader_)
                &&& (!vote ==> final(self).is_leader_ == old(self).is_leader_)
            },
            final(self).vote_for_ == (if vote { can_id }
                else if raft_server_signed_term_is_newer(can_term, old(self).current_term_) { RAFT_SERVER_INVALID_SITE_ID }
                else { old(self).vote_for_ }),
            final(self).election_term_ == old(self).election_term_,
            final(self).raft_log_ == old(self).raft_log_,
            final(self).commit_index_ == old(self).commit_index_,
            final(self).config_members_ == old(self).config_members_,
            final(self).site_id_ == old(self).site_id_,
            final(self).snapterm_ == old(self).snapterm_,
            final(self).gated_ == old(self).gated_,
            final(self).g_log_ == old(self).g_log_,
            final(self).g_votes_ == old(self).g_votes_,
            final(self).g_match_ == old(self).g_match_,
            final(self).g_next_ == old(self).g_next_,
    {
        *vote_granted = vote as i8;
        *reply_term = self.current_term_ as i64;

        // Was #ifdef RAFT_LEADER_ELECTION_DEBUG. The preprocessor has no DSL
        // spelling, so the switch is a branch on a constant the compiler
        // folds -- the same treatment raft_batch_optimization_enabled gets.
        if election_debug {
            out.log(RAFT_LOG_INFO,
                "[RAFT_VOTE] server {} (loc {}) vote={} candidate={} can_term={} cur_term={} prev_vote_for={} is_leader={} lst_idx={} lst_term={}",
                &[(self.site_id_).arg(),
                 (self.loc_id_).arg(),
                 (vote).arg(),
                 (can_id).arg(),
                 (can_term).arg(),
                 (self.current_term_).arg(),
                 (self.vote_for_).arg(),
                 (self.is_leader_).arg(),
                 (lst_log_idx).arg(),
                 (lst_log_term).arg()]);
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
                               self.current_term_, can_id, out);  // [move, M7]
        }

        if vote {
            self.set_is_leader(false, stopped, failover, out);  // [move, M3]
            self.vote_for_ = can_id;
            if election_debug {
                out.log(RAFT_LOG_INFO,
                    "[RAFT_VOTE] server {} recorded vote_for={} at term={}",
                    &[(self.site_id_).arg(),
                     (self.vote_for_).arg(),
                     (self.current_term_).arg()]);
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
                          out: &mut CoreOutput) -> (r: CampaignStart)
        requires
            old(self).inv(),
            // the host contract: the next term is below the ceiling
            (old(self).current_term_ as int) + 1 < raft_index_limit(),
        ensures
            final(self).inv(),
            // [M12] a campaign that starts is LTimeout, broadcasting r's
            // RequestVote; one that does not is unseen by the spec
            old(self).ginv() && old(self).gated_ ==> {
                &&& final(self).ginv()
                &&& final(self).g_log_@ == (if r.started_ {
                        crate::coupling::timeout_log(old(self).g_log_@, old(self).my_rank(),
                            crate::coupling::campaign_msg(old(self).my_rank(), r))
                    } else {
                        old(self).g_log_@
                    })
            },
    {
        let ghost pre = *self;
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
            // [move, M10] `now` is read before mtx_, so a reset in between
            // can leave it below last_heartbeat_time_; release builds always
            // wrapped this, and the generation check below then refuses the
            // campaign. wrapping_sub says so: the same machine operation.
            let elapsed: u64 = now.wrapping_sub(self.last_heartbeat_time_);
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
                             RAFT_SERVER_INVALID_SITE_ID, out);  // [move, M7]
        campaign.lst_idx_ = self.raft_log_.last_index();
        campaign.lst_term_ = self.election_last_log_term();
        campaign.started_ = true;
        proof {
            if pre.ginv() && pre.gated_ {
                self.g_log_@ = crate::coupling::timeout_log(pre.g_log_@, pre.my_rank(),
                    crate::coupling::campaign_msg(pre.my_rank(), campaign));
                self.g_votes_@ = Set::<int>::empty().insert(pre.my_rank());
                crate::coupling::lemma_timeout_ginv(&pre, self, campaign);
            }
        }
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
                           out: &mut CoreOutput) -> bool
        requires
            old(self).inv(),
            voters@.len() == granted@.len(),
            voters@.len() == reply_terms@.len(),
            // the host contract: a reply's term is below the ceiling
            forall|k: int| 0 <= k < reply_terms@.len() ==> (#[trigger] reply_terms@[k] as int) < raft_index_limit(),
        ensures
            final(self).inv(),
            // [M12] the settlement's segments: each granted reply and a yes
            // quorum's BecomeLeader, a higher reply term's StepDown, a lost
            // campaign's StepAside; no sends, and every receive one of the
            // replies handed in (coupling.rs, "A campaign settled")
            old(self).ginv() && old(self).gated_
                && crate::coupling::settle_inputs_ok(old(self), voters@, granted@, reply_terms@,
                    term, n_total) ==> {
                &&& final(self).ginv()
                &&& old(self).g_log_@.is_prefix_of(final(self).g_log_@)
                &&& crate::coupling::settle_entries_ok(final(self).g_log_@, old(self).g_log_@.len() as int,
                        old(self).config_members_@, voters@, granted@, reply_terms@)
            },
    {
        let ghost pre = *self;
        let ghost on = pre.ginv() && pre.gated_
            && crate::coupling::settle_inputs_ok(&pre, voters@, granted@, reply_terms@, term, n_total);
        if stopped {
            self.election_in_progress_ = false;
            self.req_voting_ = false;
            proof {
                if on {
                    if pre.role_view() is Candidate {
                        self.g_log_@ = crate::coupling::step_aside_log(pre.g_log_@);
                        self.g_votes_@ = Set::<int>::empty();
                    }
                    crate::coupling::lemma_settle_aside(&pre, self, voters@, granted@, reply_terms@);
                }
            }
            return false;
        }
        // A higher term dominates every outcome, TIMEOUT and a concurrently
        // completed YES quorum included. FeedResponse publishes that maximum
        // before its wakeup, so it is snapshotted only now, after Raft state
        // has been reacquired.
        // [move, M5] The tally, counted here from the campaign's replies
        // rather than read out of the lane's quorum object.
        let mut votes: VoteSet = VoteSet::new();
        // [M12] the voters counted yes
        let ghost mut yes_set: Set<u16> = Set::<u16>::empty();
        let mut r: usize = 0;
        while r < voters.len()
            invariant
                r <= voters@.len(),
                voters@.len() == granted@.len(),
                voters@.len() == reply_terms@.len(),
                votes.wf(),
                votes.spec_highest_term() < raft_index_limit(),
                forall|k: int| 0 <= k < reply_terms@.len() ==> (#[trigger] reply_terms@[k] as int) < raft_index_limit(),
                // [M12] the yes count is the yes voters', each fed granted;
                // the highest term is a reply's (or none yet)
                votes.spec_yes_count() == yes_set.len(),
                forall|x: u16| #[trigger] yes_set.contains(x) ==> votes.spec_has(x),
                forall|x: u16| #[trigger] yes_set.contains(x)
                    ==> exists|k: int| 0 <= k < r && voters@[k] == x && granted@[k],
                votes.spec_highest_term() == 0
                    || exists|k: int| 0 <= k < r && reply_terms@[k] == votes.spec_highest_term(),
            decreases voters@.len() - r,
        {
            proof {
                if !votes.spec_has(voters@[r as int]) && granted@[r as int] {
                    yes_set = yes_set.insert(voters@[r as int]);
                }
            }
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
                               RAFT_SERVER_INVALID_SITE_ID, out);  // [move, M7]
            proof {
                if on {
                    // the reply that carried the highest term
                    let k = choose|k: int| 0 <= k < reply_terms@.len()
                        && reply_terms@[k] == observed_response_term;
                    self.g_log_@ = crate::coupling::settle_step_down_log(pre.g_log_@,
                        pre.config_members_@, voters@[k], granted@[k], reply_terms@[k]);
                    self.g_votes_@ = Set::<int>::empty();
                    crate::coupling::lemma_settle_step_down(&pre, self, voters@, granted@,
                        reply_terms@, k);
                }
            }
            return false;
        }

        // An accepted leader RPC can cancel this campaign while the broadcast
        // is yielding, and another campaign can begin before this result
        // arrives. Only the exact active term owns role changes and election
        // bookkeeping. A strictly higher response term was handled above,
        // because that evidence supersedes even a newer local campaign.
        if completion_action == ElectionCompletionAction::IGNORE_STALE as i32 {
            if election_debug {
                out.log(RAFT_LOG_INFO,
                    "[RAFT_ELECTION] server {} ignoring stale election result: result_term={} local_term={} election_term={} active={}",
                    &[(self.site_id_).arg(),
                     (term).arg(),
                     (self.current_term_).arg(),
                     (self.election_term_).arg(),
                     (self.election_in_progress_).arg()]);
            }
            proof {
                if on {
                    assert(pre.g_log_@.subrange(0, pre.g_log_@.len() as int) =~= pre.g_log_@);
                }
            }
            return false;
        }
        runtime_assert(completion_action
            == ElectionCompletionAction::APPLY_CURRENT as i32);  // [move, M10]
        if election_debug {
            out.log(RAFT_LOG_INFO,
                "[RAFT_ELECTION] server {} term {} vote outcome yes={} no={} highest_term_seen={} timeout={}",
                &[(self.site_id_).arg(),
                 (term).arg(),
                 (outcome.n_voted_yes_).arg(),
                 (outcome.n_voted_no_).arg(),
                 (outcome.term_).arg(),
                 (outcome.timeouted_).arg()]);
        }

        if outcome.yes_ {
            runtime_assert(self.current_term_ >= term);  // [move, M10]
            self.election_in_progress_ = false;
            self.req_voting_ = false;

            if stopped
                || self.current_term_ != term
            {
                self.req_voting_ = false;
                return false;
            }

            self.set_is_leader(true, stopped, failover, out);  // [move, M3]
            out.log(RAFT_LOG_DEBUG,
                "site {} became leader for term {}",
                &[(self.site_id_).arg(),
                 (term).arg()]);
            if election_debug {
                out.log(RAFT_LOG_INFO,
                    "[RAFT_ELECTION] server {} won election term {} (votes yes={} no={})",
                    &[(self.site_id_).arg(),
                     (term).arg(),
                     (outcome.n_voted_yes_).arg(),
                     (outcome.n_voted_no_).arg()]);
            }

            if looping && self.is_leader_ {  // IsLeaderLocked
                out.log(RAFT_LOG_DEBUG,
                    "vote accepted {} curterm {}",
                    &[(loc_id).arg(),
                     (self.current_term_).arg()]);
                self.req_voting_ = false;
                proof {
                    if on {
                        // APPLY_CURRENT: the campaign of the current term runs
                        assert(pre.election_term_ as u64 == term && term == pre.current_term_);
                        assert(pre.election_term_ as int == pre.current_term_ as int);
                        self.g_log_@ = crate::coupling::settle_won_log(pre.g_log_@,
                            pre.config_members_@, voters@, granted@, reply_terms@, pre.g_votes_@, false);
                        self.g_votes_@ = pre.g_votes_@.union(crate::coupling::granted_ranks(
                            pre.config_members_@, voters@, granted@, voters@.len() as int));
                        self.g_match_@ = Map::<u64, u64>::empty();
                        self.g_next_@ = Map::<u64, u64>::empty();
                        crate::coupling::lemma_settle_won(&pre, self, voters@, granted@, reply_terms@,
                            term, n_total, yes_set, false);
                    }
                }
                true
            } else {
                out.log(RAFT_LOG_DEBUG,
                    "vote rejected {} curterm {}, do rollback",
                    &[(loc_id).arg(),
                     (self.current_term_).arg()]);
                    self.set_is_leader(false, stopped, failover, out);  // [move, M3]
                proof {
                    if on {
                        // APPLY_CURRENT: the campaign of the current term runs
                        assert(pre.election_term_ as u64 == term && term == pre.current_term_);
                        assert(pre.election_term_ as int == pre.current_term_ as int);
                        self.g_log_@ = crate::coupling::settle_won_log(pre.g_log_@,
                            pre.config_members_@, voters@, granted@, reply_terms@, pre.g_votes_@, true);
                        self.g_votes_@ = Set::<int>::empty();
                        self.g_match_@ = Map::<u64, u64>::empty();
                        self.g_next_@ = Map::<u64, u64>::empty();
                        crate::coupling::lemma_settle_won(&pre, self, voters@, granted@, reply_terms@,
                            term, n_total, yes_set, true);
                    }
                }
                false
            }
        } else if outcome.no_ {
            out.log(RAFT_LOG_DEBUG, "site {} requestvote rejected", &[(self.site_id_).arg()]);
            self.set_is_leader(false, stopped, failover, out);  // [move, M3]
            if election_debug {
                out.log(RAFT_LOG_INFO,
                    "[RAFT_ELECTION] server {} lost election term {} (yes={} no={}) highest_term={}",
                    &[(self.site_id_).arg(),
                     (term).arg(),
                     (outcome.n_voted_yes_).arg(),
                     (outcome.n_voted_no_).arg(),
                     (outcome.term_).arg()]);
            }
            if self.election_in_progress_
                && self.election_term_ == term as i64
            {
                self.election_in_progress_ = false;
            }
            self.req_voting_ = false;
            proof {
                if on {
                    self.g_log_@ = crate::coupling::step_aside_log(pre.g_log_@);
                    self.g_votes_@ = Set::<int>::empty();
                    crate::coupling::lemma_settle_aside(&pre, self, voters@, granted@, reply_terms@);
                }
            }
            false
        } else {
            out.log(RAFT_LOG_DEBUG, "vote timeout {}", &[(loc_id).arg()]);
            if election_debug {
                out.log(RAFT_LOG_INFO,
                    "[RAFT_ELECTION] server {} election timed out term {} (yes={} no={})",
                    &[(self.site_id_).arg(),
                     (term).arg(),
                     (outcome.n_voted_yes_).arg(),
                     (outcome.n_voted_no_).arg()]);
            }
            if self.election_in_progress_
                && self.election_term_ == term as i64
            {
                self.election_in_progress_ = false;
            }
            self.req_voting_ = false;
            proof {
                if on {
                    self.g_log_@ = crate::coupling::step_aside_log(pre.g_log_@);
                    self.g_votes_@ = Set::<int>::empty();
                    crate::coupling::lemma_settle_aside(&pre, self, voters@, granted@, reply_terms@);
                }
            }
            false
        }
    }

    // [move, M3] PublishAppliedIndexLocked's decision: the applied index
    // never moves backward; a caller that tries is a bug, so it is reported
    // rather than obeyed. `published` is the shell's mirror. Returns whether
    // the index was recorded. The caller holds mtx_.
    pub fn on_applied(&mut self, index: u64, published: u64,
                      out: &mut CoreOutput) -> bool  // [move, M7]
        requires old(self).inv(),
        ensures
            final(self).inv(),
            old(self).ginv() ==> final(self).ginv(),  // [M12] unseen by the spec
    {
        if raft_server_log_index_above(published, index) {
            out.log(RAFT_LOG_WARN,
                "[RAFT-APPLY] Site {} refusing to move applied index backward from {} to {}",
                &[(self.site_id_).arg(),
                 (published).arg(),
                 (index).arg()]);
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
    pub fn reset_election_timer(&mut self, now: u64, timeout_us: u64) -> u64
        requires old(self).inv(),
        ensures
            final(self).inv(),
            old(self).ginv() ==> final(self).ginv(),  // [M12] unseen by the spec
    {
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
                         failover: bool, out: &mut CoreOutput)
        requires
            old(self).inv(),
            // becoming leader ends the campaign first, in a new epoch
            is_leader ==> !old(self).election_in_progress_,
            is_leader ==> old(self).pending_leader_term_ != Some(old(self).current_term_),
        ensures
            final(self).inv(),
            final(self).raft_log_ == old(self).raft_log_,
            final(self).current_term_ == old(self).current_term_,
            final(self).commit_index_ == old(self).commit_index_,
            final(self).snapidx_ == old(self).snapidx_,
            final(self).election_in_progress_ == old(self).election_in_progress_,
            final(self).pending_leader_term_ == old(self).pending_leader_term_,
            final(self).config_members_ == old(self).config_members_,
            final(self).site_id_ == old(self).site_id_,
            final(self).round_ == old(self).round_,
            final(self).pending_rpcs_ == old(self).pending_rpcs_,
            final(self).decoded_terms_ == old(self).decoded_terms_,
            !is_leader ==> final(self).authority_rounds_ == old(self).authority_rounds_,
            !is_leader ==> final(self).heartbeat_round_ == old(self).heartbeat_round_,
            // [M12] the role it leaves, and what it does not touch
            final(self).vote_for_ == old(self).vote_for_,
            final(self).election_term_ == old(self).election_term_,
            !is_leader ==> !final(self).is_leader_,
            is_leader && !stopped ==> final(self).is_leader_,
            is_leader && stopped ==> final(self).is_leader_ == old(self).is_leader_,
            !is_leader ==> final(self).peers_ == old(self).peers_,
            !is_leader ==> final(self).peer_sites_ == old(self).peer_sites_,
            final(self).snapterm_ == old(self).snapterm_,
            final(self).gated_ == old(self).gated_,
            final(self).g_log_ == old(self).g_log_,  // [M12]
            final(self).g_votes_ == old(self).g_votes_,  // [M12]
            final(self).g_match_ == old(self).g_match_,  // [M12]
            final(self).g_next_ == old(self).g_next_,  // [M12]
    {
        let prev_is_leader: bool = self.is_leader_;
        // raft_log_set_is_leader_entry's term, which it read first.
        let entry_term: u64 = self.current_term_;

        if is_leader && !prev_is_leader {
            // Leadership publication must not proceed once shutdown began.
            let publication_term: u64 = self.current_term_;
            if stopped || self.current_term_ != publication_term {
                out.log(RAFT_LOG_WARN,
                    "[RAFT_STATE] Site {} suppressing stale leadership publication for term {} (current={}, stopping={})",
                    &[(self.site_id_).arg(),
                     (publication_term).arg(),
                     (self.current_term_).arg(),
                     (stopped).arg()]);
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
            while ord < peers
                invariant
                    self.inv(),
                    peers == self.peers_.spec_len(),
                    self.raft_log_ == old(self).raft_log_,
                    self.current_term_ == old(self).current_term_,
                    self.commit_index_ == old(self).commit_index_,
                    self.snapidx_ == old(self).snapidx_,
                    self.election_in_progress_ == old(self).election_in_progress_,
                    self.pending_leader_term_ == old(self).pending_leader_term_,
                    self.config_members_ == old(self).config_members_,
                    self.site_id_ == old(self).site_id_,
                    self.round_ == old(self).round_,
                    self.pending_rpcs_ == old(self).pending_rpcs_,
                    self.decoded_terms_ == old(self).decoded_terms_,
                    self.is_leader_ == old(self).is_leader_,
                    self.heartbeat_round_ == 0,
                    self.vote_for_ == old(self).vote_for_,
                    self.election_term_ == old(self).election_term_,
                    self.snapterm_ == old(self).snapterm_,
                    self.gated_ == old(self).gated_,
                    self.g_log_ == old(self).g_log_,  // [M12]
                    self.g_votes_ == old(self).g_votes_,  // [M12]
                    self.g_match_ == old(self).g_match_,  // [M12]
                    self.g_next_ == old(self).g_next_,  // [M12]
                decreases peers - ord,
            {
                let site: u16 = self.peer_site_at(ord);
                out.log(RAFT_LOG_DEBUG,
                    "loc_id_={} match_index_[{}]={}, next_index_[{}]={}",
                    &[(self.loc_id_).arg(),
                     (site).arg(),
                     (self.peers_.match_index(ord)).arg(),
                     (site).arg(),
                     (self.peers_.next_index(ord)).arg()]);
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
            out.log(RAFT_LOG_INFO,
                "RaftServer::setIsLeader site_id_ {} become_new_leader {} become_new_follower {} isLeader {}",
                &[(self.site_id_).arg(),
                 (become_new_leader).arg(),
                 (become_new_follower).arg(),
                 (is_leader).arg()]);
        }

        if become_new_leader {
            out.log(RAFT_LOG_INFO,
                "[RAFT_STATE] setIsLeader transition LEADER: site {} term {} prev_is_leader={} become_new_leader={}",
                &[(self.site_id_).arg(),
                 (self.current_term_).arg(),
                 (prev_is_leader).arg(),
                 (become_new_leader).arg()]);
            out.push(CoreAction::append_noop());
        } else if become_new_follower {
            out.log(RAFT_LOG_INFO,
                "[RAFT_STATE] setIsLeader transition FOLLOWER: site {} term {} prev_is_leader={} become_new_follower={}",
                &[(self.site_id_).arg(),
                 (self.current_term_).arg(),
                 (prev_is_leader).arg(),
                 (become_new_follower).arg()]);

            // Resetting the timer here is what prevents an instant election
            // after a resume: last_heartbeat_time_ is stale from before the
            // pause, so counting from NOW gives the current leader time to
            // send a heartbeat first. Standard Raft: a server stepping down
            // resets its timer.
            out.push(CoreAction::reset_election(TimerResetReason::BECAME_FOLLOWER));
            out.log(RAFT_LOG_INFO,
                "[RAFT_VIEW] Server {} stepping down as leader for partition {}",
                &[(self.site_id_).arg(),
                 (self.partition_id_).arg()]);
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
                     out: &mut CoreOutput)
        requires old(self).inv(),
        ensures
            final(self).inv(),
            final(self).raft_log_ == old(self).raft_log_,
            final(self).current_term_ == old(self).current_term_,
            final(self).commit_index_ == old(self).commit_index_,
            final(self).snapidx_ == old(self).snapidx_,
            final(self).pending_leader_term_ == old(self).pending_leader_term_,
            final(self).config_members_ == old(self).config_members_,
            final(self).site_id_ == old(self).site_id_,
            final(self).round_ == old(self).round_,
            final(self).pending_rpcs_ == old(self).pending_rpcs_,
            final(self).decoded_terms_ == old(self).decoded_terms_,
            !final(self).election_in_progress_,
            // [M12] the role it leaves, and what it does not touch
            !final(self).is_leader_,
            final(self).vote_for_ == old(self).vote_for_,
            final(self).election_term_ == old(self).election_term_,
            final(self).peers_ == old(self).peers_,
            final(self).peer_sites_ == old(self).peer_sites_,
            final(self).snapterm_ == old(self).snapterm_,
            final(self).gated_ == old(self).gated_,
            final(self).g_log_ == old(self).g_log_,  // [M12]
            final(self).g_votes_ == old(self).g_votes_,  // [M12]
            final(self).g_match_ == old(self).g_match_,  // [M12]
            final(self).g_next_ == old(self).g_next_,  // [M12]
    {
        out.log(RAFT_LOG_INFO,
            "[SPEC-RAFT] Site {}: Stepping down as leader (term={})",
            &[(self.site_id_).arg(),
             (self.current_term_).arg()]);

        // Handles the leadership-change callback, the timer reset, and the
        // rest of the follower transition.
        self.set_is_leader(false, stopped, failover, out);

        // A late higher-term response can arrive after this server has
        // already entered a new candidacy.
        self.req_voting_ = false;
        self.election_in_progress_ = false;

        out.push(CoreAction::reset_election(TimerResetReason::STEP_DOWN));

        out.log(RAFT_LOG_INFO,
            "[SPEC-RAFT] Site {}: Step-down complete, now follower",
            &[(self.site_id_).arg()]);
    }
}

// [move, M5] The election timer's gather as a core call: the timer state read
// in one scope at `now` (the shell's clock read), and whether the timeout
// fired. The caller holds mtx_. A free function after ElectionTick rather
// than a RaftCore method, so the transpiled C++ declares the type first.
pub fn raft_election_tick<C>(core: &RaftCore<C>, now: u64) -> ElectionTick {
    let heartbeat_time: u64 = core.last_heartbeat_time_;
    // [move, M10] The same wrap start_election's elapsed time has: `now` is
    // read before mtx_, and release builds always wrapped it.
    let time_elapsed: u64 = now.wrapping_sub(heartbeat_time);
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
pub fn raft_on_request_vote<C: Clone>(
    core: &mut RaftCore<C>,
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
)
    requires
        old(core).inv(),
        // the host contract: a term off the wire is below the ceiling
        (can_term as int) < raft_index_limit(),
    ensures
        final(core).inv(),
        // [M12] the request's group (coupling::vote_group), for a request
        // from another member: a higher term taken up steps down first; the
        // answer is the group's one send, back to the candidate
        old(core).ginv() && old(core).gated_ && old(core).config_members_@.contains(can_id)
            && can_id != old(core).site_id_ ==> {
            &&& final(core).ginv()
            &&& final(core).g_log_@ == crate::coupling::vote_group(old(core).g_log_@,
                    crate::coupling::rank(old(core).config_members_@, can_id),
                    crate::coupling::request_vote_msg(old(core).config_members_@, can_id,
                        can_term, lst_log_idx, lst_log_term),
                    crate::coupling::vote_steps(stopped, candidate_is_current_voter, can_term,
                        lst_log_term, old(core).current_term_),
                    *final(vote_granted) != 0, *final(reply_term) as int, old(core).my_rank())
        },
{
    let ghost pre = *core;
    let ghost on = pre.ginv() && pre.gated_ && pre.config_members_@.contains(can_id)
        && can_id != pre.site_id_;
    let ghost src = crate::coupling::rank(pre.config_members_@, can_id);
    let ghost msg = crate::coupling::request_vote_msg(pre.config_members_@, can_id, can_term,
        lst_log_idx, lst_log_term);
    if stopped {
        *reply_term = core.current_term_ as i64;
        *vote_granted = 0;
        proof {
            if on {
                let stepped = crate::coupling::vote_steps(stopped, candidate_is_current_voter,
                    can_term, lst_log_term, pre.current_term_);
                core.g_log_@ = crate::coupling::vote_group(pre.g_log_@, src, msg, stepped,
                    *vote_granted != 0, *reply_term as int, pre.my_rank());
                if stepped {
                    core.g_votes_@ = Set::<int>::empty();
                }
                crate::coupling::lemma_vote_group(&pre, core, can_id, can_term, lst_log_idx,
                    lst_log_term, stepped, *vote_granted != 0, *reply_term);
            }
        }
        return;
    }

    if can_term < 0 || lst_log_term < 0 || !candidate_is_current_voter {
        *reply_term = core.current_term_ as i64;
        *vote_granted = 0;
        proof {
            if on {
                let stepped = crate::coupling::vote_steps(stopped, candidate_is_current_voter,
                    can_term, lst_log_term, pre.current_term_);
                core.g_log_@ = crate::coupling::vote_group(pre.g_log_@, src, msg, stepped,
                    *vote_granted != 0, *reply_term as int, pre.my_rank());
                if stepped {
                    core.g_votes_@ = Set::<int>::empty();
                }
                crate::coupling::lemma_vote_group(&pre, core, can_id, can_term, lst_log_idx,
                    lst_log_term, stepped, *vote_granted != 0, *reply_term);
            }
        }
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
        proof {
            if on {
                let stepped = crate::coupling::vote_steps(stopped, candidate_is_current_voter,
                    can_term, lst_log_term, pre.current_term_);
                core.g_log_@ = crate::coupling::vote_group(pre.g_log_@, src, msg, stepped,
                    *vote_granted != 0, *reply_term as int, pre.my_rank());
                if stepped {
                    core.g_votes_@ = Set::<int>::empty();
                }
                crate::coupling::lemma_vote_group(&pre, core, can_id, can_term, lst_log_idx,
                    lst_log_term, stepped, *vote_granted != 0, *reply_term);
            }
        }
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
        proof {
            if on {
                let stepped = crate::coupling::vote_steps(stopped, candidate_is_current_voter,
                    can_term, lst_log_term, pre.current_term_);
                core.g_log_@ = crate::coupling::vote_group(pre.g_log_@, src, msg, stepped,
                    *vote_granted != 0, *reply_term as int, pre.my_rank());
                if stepped {
                    core.g_votes_@ = Set::<int>::empty();
                }
                crate::coupling::lemma_vote_group(&pre, core, can_id, can_term, lst_log_idx,
                    lst_log_term, stepped, *vote_granted != 0, *reply_term);
            }
        }
        return;
    }

    // Every grant, including an idempotent retry, must still carry an
    // up-to-date candidate log. Defensive against damaged or legacy
    // persistent state, and the RequestVote rule in its direct form.
    // The last log index is not below the snapshot boundary.
    runtime_assert(core.raft_log_.last_index() >= core.snapidx_);  // [move, M10]
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
        proof {
            if on {
                let stepped = crate::coupling::vote_steps(stopped, candidate_is_current_voter,
                    can_term, lst_log_term, pre.current_term_);
                core.g_log_@ = crate::coupling::vote_group(pre.g_log_@, src, msg, stepped,
                    *vote_granted != 0, *reply_term as int, pre.my_rank());
                if stepped {
                    core.g_votes_@ = Set::<int>::empty();
                }
                crate::coupling::lemma_vote_group(&pre, core, can_id, can_term, lst_log_idx,
                    lst_log_term, stepped, *vote_granted != 0, *reply_term);
            }
        }
        return;
    }

    // Snapshot-aware offset invariant.
    runtime_assert(lstoff + core.snapidx_ == core.raft_log_.last_index());  // [move, M10]

    let grant = candidate_log_is_current;
    core.do_vote(lst_log_idx, lst_log_term, can_id, can_term,
                 reply_term, vote_granted, grant, stopped, failover,
                 election_debug, out);
    proof {
            if on {
                let stepped = crate::coupling::vote_steps(stopped, candidate_is_current_voter,
                    can_term, lst_log_term, pre.current_term_);
                core.g_log_@ = crate::coupling::vote_group(pre.g_log_@, src, msg, stepped,
                    *vote_granted != 0, *reply_term as int, pre.my_rank());
                if stepped {
                    core.g_votes_@ = Set::<int>::empty();
                }
                crate::coupling::lemma_vote_group(&pre, core, can_id, can_term, lst_log_idx,
                    lst_log_term, stepped, *vote_granted != 0, *reply_term);
            }
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

// [move, M11] The inbound AppendEntries payload, as the core sees it: a
// trusted, opaque view of the wire command (the shell's WireBatch). The core
// asks for it only after its gates pass, in the order the C++ did: the terms
// first (decode_terms, which also validates the count), then each appended
// entry (entry_at, one handle clone and the entry's cached facts, M6).
pub trait InboundBatch<C> {
    // Whether the RPC carries a payload at all (ghost). The shell builds the
    // batch from the flag it also passes the core as `has_cmd`.
    spec fn spec_has_payload(&self) -> bool;

    // Fills `terms` with one term per encoded entry -- its length IS the
    // decoded count -- and reports whether that count fits after
    // leader_prev_log_index and every term is a Raft term ([fix, F4]).
    //
    // The host contract (unverified: the shell implements this): no payload
    // decodes to no terms, and a batch the decoder accepts ends below the
    // index ceiling, which no leader's log reaches.
    fn decode_terms(&self, leader_prev_log_index: u64, terms: &mut Vec<i64>) -> (r: bool)
        ensures
            !self.spec_has_payload() ==> final(terms)@.len() == 0,
            r ==> leader_prev_log_index as int + final(terms)@.len() < raft_index_limit();
    // The entry at position k (0-based) of the payload decode_terms read.
    fn entry_at(&self, k: u64) -> RaftEntry<C>;  // [move, M11]
}

// [move, M5] A core call: the caller holds mtx_, and passes the payload as
// a WireBatch, the decode scratch, and the shell's failover_.
// unnecessary_unwrap: the per-slot lookup below uses is_some()/unwrap() with
// an explicit `&RaftEntry` binding rather than `if let`. `if let` is the
// better Rust and it transpiles, but the emitter renders the binding with a
// dot where the C++ needs an arrow, and an inferred binding COPIES the entry.
#[allow(clippy::too_many_arguments, clippy::unnecessary_unwrap)]
pub fn raft_on_append_entries<C: Clone, W: InboundBatch<C>>(
    core: &mut RaftCore<C>,
    wire: &W,  // [move, M5] [move, M11]
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
) -> AppendReport
    requires
        old(core).inv(),
        // the host contract: a term off the wire is below the ceiling, and
        // has_cmd is the flag the batch was built from
        (leader_current_term as int) < raft_index_limit(),
        has_cmd == wire.spec_has_payload(),
    ensures final(core).inv(),
{
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
                                                 &mut core.decoded_terms_);  // [move, M1]
    let decoded_count: u64 = core.decoded_terms_.len() as u64;  // [move, M1]

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
                                 prev_term, now_term, leader_site_id, out);  // [move, M7]
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
    while i < decoded_count
        invariant_except_break
            !have_first_write,
            !truncate_suffix,
            // every slot passed so far is held, so the first write comes at
            // most one past the old tail: the log never gets a hole
            leader_prev_log_index as int + i <= old_last_log_index,
        invariant
            core.inv(),
            decoded_count == core.decoded_terms_@.len(),
            leader_prev_log_index as int + decoded_count < raft_index_limit(),
            old_last_log_index == core.raft_log_.spec_last_index(),
            i <= decoded_count,
        ensures
            !have_first_write ==> leader_prev_log_index as int + decoded_count <= old_last_log_index,
            !have_first_write ==> !truncate_suffix,
            have_first_write ==> {
                &&& leader_prev_log_index < first_write_index
                &&& first_write_index as int <= leader_prev_log_index as int + decoded_count
                &&& first_write_index as int <= old_last_log_index as int + 1
                &&& truncate_suffix == (first_write_index <= old_last_log_index)
            },
        decreases decoded_count - i,
    {
        let index = leader_prev_log_index + i + 1;
        // ONE lookup per entry, as the original had. The lookup itself is
        // Rust -- the log is a Rust type -- and only "does this slot hold a
        // payload" crosses, because janus::Command is opaque here.
        let mut local_exists: bool = false;
        let mut local_term: i64 = 0;
        let slot = core.raft_log_.get(index);
        if slot.is_some() {
            let entry: &RaftEntry<C> = slot.unwrap();
            local_exists = entry.has_value();  // [move, M6]
            if local_exists {
                local_term = entry.term();
            }
        }
        // In bounds by construction: decoded_count IS decoded_terms_.len(),
        // the loop condition is i < decoded_count, and nothing in the body
        // touches the vector. The kernel this replaces needed a verify only
        // because i arrived across the language boundary.
        let incoming_term: i64 = core.decoded_terms_[i as usize];  // [move, M1]
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

    // a write starts at or above the log's base: below it, the slot would be
    // at or below the commit index, which the check above refused
    assert(have_first_write ==> core.raft_log_.spec_base() <= first_write_index);
    if have_first_write {
        // Two operations that cannot leave a hole: drop the divergent suffix,
        // then re-append in index order. truncate_from is a no-op when
        // first_write_index is already past the tail, the ordinary extend
        // case. The append is Rust; only the per-entry reads of the wire
        // payload are kernels.
        core.raft_log_.truncate_from(first_write_index);
        assert(core.raft_log_.spec_last_index() == first_write_index - 1);
        // [move, M1, M11] WireBatch::append_into's loop, in the core: each
        // entry from first_write_index on is materialized from the payload
        // (one handle clone) and appended, in index order.
        let mut k: u64 = 0;
        while k < decoded_count  // [move, M1, M11]
            invariant
                core.inv(),
                leader_prev_log_index as int + decoded_count < raft_index_limit(),
                leader_prev_log_index < first_write_index,
                first_write_index as int <= leader_prev_log_index as int + decoded_count,
                k <= decoded_count,
                // the tail: the write point less one, until the batch passes it
                core.raft_log_.spec_last_index() == (if leader_prev_log_index as int + k
                    < first_write_index as int - 1 { first_write_index as int - 1 }
                    else { leader_prev_log_index as int + k }),
            decreases decoded_count - k,
        {
            let index: u64 = raft_server_append_sent_end(leader_prev_log_index, k + 1);  // [move, M1, M11]
            if index >= first_write_index {
                let appended: u64 = core.raft_log_.append(wire.entry_at(k));  // [move, M1, M11]
                runtime_assert(appended == index);  // [move, M10]
            }
            k += 1;
        }
    }
    // The append left the log tail where the result rule predicts.
    runtime_assert(core.raft_log_.last_index()
        == raft_server_append_result_last_index(old_last_log_index, accepted_through,
                                                truncate_suffix));  // [move, M10]

    let follower_commit_candidate =
        raft_server_commit_index_clamp(leader_commit_index, accepted_through);
    if raft_server_log_index_above(follower_commit_candidate, core.commit_index_) {
        let old_commit = core.commit_index_;
        core.commit_index_ = follower_commit_candidate;
        // The commit index did not advance past the log tail.
        runtime_assert(core.raft_log_.last_index() >= core.commit_index_);  // [move, M10]
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

} // verus!
