// [move, M5] (whole file) The core's one entry point; [fix, F9] its checked
// form for messages from the network.
//
// Every decision the shell asks of the core is
// an Event, handed to RaftCore::step under mtx_, and answered with a Reply
// plus the actions and log records in the CoreOutput (plan §3.1-3.2, Phase
// 6). step only dispatches: each arm is the core call the shell used to make
// directly, with the same arguments, in the same critical section.
//
// One entry point is what makes the core's history a sequence of events:
// the shell's recorder writes each event and what it produced, and a replay
// feeds the same events to another build of the core (plan A.4).
//
// Queries that change nothing (the election timer's gather, a membership or
// ordinal lookup, the last log term, a term-change log line) stay plain
// reads: they are not part of that history.

#[allow(unused_imports)]
use crate::*;
use vstd::prelude::*;

verus! {

pub enum Event<'a, C, W> {
    // ---- Setup, in this order ----
    // set_site_identity's core half; the worker sets it before Setup.
    SetIdentity { loc_id: u32, site_id: u16, partition_id: u32 },
    // LoadCurrentConfig: the partition's members, sorted and duplicate-free.
    Configure { members: &'a [u16] },
    // verified_config_ok: the gate (F5).
    EnterGates { snapshots_enabled: bool, failover: bool },
    // RebuildPeerTables (HeartbeatPrologue).
    RebuildPeers { next_index: u64 },

    // ---- A proposal: Start's append, or the new leader's no-op ----
    Propose { cmd: C, has_value: bool, is_tpc_commit: bool, kind: i32, payload_bytes: u64 },

    // ---- The election ----
    StartElection { timer_guarded: bool, expected_generation: u64, now: u64, stopped: bool },
    SettleElection {
        term: u64,
        loc_id: u32,
        voters: &'a [u16],
        granted: &'a [bool],
        reply_terms: &'a [i64],
        n_total: u64,
        timed_out: bool,
        stopped: bool,
        looping: bool,
        failover: bool,
        election_debug: bool,
    },
    ResetElectionTimer { now: u64, timeout_us: u64 },

    // ---- Inbound RPCs ----
    RecvRequestVote {
        stopped: bool,
        candidate_is_current_voter: bool,
        lst_log_idx: u64,
        lst_log_term: i64,
        can_id: u16,
        can_term: i64,
        failover: bool,
        election_debug: bool,
    },
    RecvAppendEntries {
        wire: &'a W,
        stopped: bool,
        sender_is_current_voter: bool,
        has_cmd: bool,
        leader_current_term: u64,
        leader_site_id: u16,
        leader_prev_log_index: u64,
        leader_prev_log_term: u64,
        leader_commit_index: u64,
        failover: bool,
    },

    // ---- The heartbeat round ----
    TickHeartbeat {
        is_leader: bool,
        snapshot_configured: bool,
        batching: bool,
        max_batch_entries: u64,
        max_batch_bytes: u64,
    },
    RecvAppendReply {
        ord: usize,
        status: bool,
        term: u64,
        last_log_index: u64,
        is_leader: bool,
        stopped: bool,
        failover: bool,
    },
    AbandonRound,
    RoundEnd { is_leader: bool },
    // The heartbeat loop's start and end: a fresh round state.
    ResetRoundState,

    // ---- The apply thread ----
    Applied { index: u64, published: u64 },

    // ---- Role changes the shell makes itself: the lab's construction, and
    // the InstallSnapshot paths (outside the verified configuration) ----
    SetFollower { stopped: bool, failover: bool },
    StepDown { stopped: bool, failover: bool },
}

pub enum Reply<C> {
    Done,
    Gates(bool),
    // The pre-append tail: the proposal landed at this + 1.
    Proposed(u64),
    Campaign(CampaignStart),
    Settled(bool),
    // The previous heartbeat time, for the shell's log line.
    TimerReset(u64),
    // RequestVote's reply: ballot_t and bool_t, as srpc carries them.
    Vote { term: i64, granted: i8 },
    // AppendEntries' reply, and the diagnostics the shell's log lines read.
    Append { report: AppendReport, ok: u64, term: u64, last_log_index: u64 },
    Tick(HeartbeatTick<C>),
    AppendReply(ReplyResult),
    RoundEnd(bool),
    Applied(bool),
}

impl<C> Reply<C> {
    // The shell's unwrapping of each reply step gives for its event (step's
    // ensures says which).
    pub fn into_done(self)
        requires self is Done,
    {
    }

    pub fn into_gates(self) -> bool
        requires self is Gates,
    {
        match self {
            Reply::Gates(ok) => ok,
            _ => vstd::pervasive::unreached(),
        }
    }

    pub fn into_proposed(self) -> u64
        requires self is Proposed,
    {
        match self {
            Reply::Proposed(prev) => prev,
            _ => vstd::pervasive::unreached(),
        }
    }

    pub fn into_campaign(self) -> CampaignStart
        requires self is Campaign,
    {
        match self {
            Reply::Campaign(c) => c,
            _ => vstd::pervasive::unreached(),
        }
    }

    pub fn into_settled(self) -> bool
        requires self is Settled,
    {
        match self {
            Reply::Settled(won) => won,
            _ => vstd::pervasive::unreached(),
        }
    }

    pub fn into_timer_reset(self) -> u64
        requires self is TimerReset,
    {
        match self {
            Reply::TimerReset(prev) => prev,
            _ => vstd::pervasive::unreached(),
        }
    }

    pub fn into_vote(self) -> (i64, i8)
        requires self is Vote,
    {
        match self {
            Reply::Vote { term, granted } => (term, granted),
            _ => vstd::pervasive::unreached(),
        }
    }

    pub fn into_append(self) -> (AppendReport, u64, u64, u64)
        requires self is Append,
    {
        match self {
            Reply::Append { report, ok, term, last_log_index } => (report, ok, term, last_log_index),
            _ => vstd::pervasive::unreached(),
        }
    }

    pub fn into_tick(self) -> HeartbeatTick<C>
        requires self is Tick,
    {
        match self {
            Reply::Tick(t) => t,
            _ => vstd::pervasive::unreached(),
        }
    }

    pub fn into_append_reply(self) -> ReplyResult
        requires self is AppendReply,
    {
        match self {
            Reply::AppendReply(r) => r,
            _ => vstd::pervasive::unreached(),
        }
    }

    pub fn into_round_end(self) -> bool
        requires self is RoundEnd,
    {
        match self {
            Reply::RoundEnd(advanced) => advanced,
            _ => vstd::pervasive::unreached(),
        }
    }

    pub fn into_applied(self) -> bool
        requires self is Applied,
    {
        match self {
            Reply::Applied(recorded) => recorded,
            _ => vstd::pervasive::unreached(),
        }
    }
}

impl<C: Clone> RaftCore<C> {
    // What each event needs of the state and of the shell (ghost): the
    // handlers' preconditions, the host contract's part of them included.
    pub open spec fn admits<W: InboundBatch<C>>(&self, ev: &Event<'_, C, W>) -> bool {
        match *ev {
            Event::SetIdentity { .. } => self.config_members_@.len() == 0,
            Event::Configure { members } => {
                &&& !self.gated_
                &&& self.round_.spec_nservers() == 0
                &&& sites_sorted(members@)
            },
            Event::Propose { .. } => self.raft_log_.spec_has_room(),
            Event::StartElection { .. } => (self.current_term_ as int) + 1 < raft_index_limit(),
            Event::SettleElection { voters, granted, reply_terms, .. } => {
                &&& voters@.len() == granted@.len()
                &&& voters@.len() == reply_terms@.len()
                &&& forall|k: int| 0 <= k < reply_terms@.len()
                        ==> (#[trigger] reply_terms@[k] as int) < raft_index_limit()
            },
            Event::RecvRequestVote { can_term, .. } => (can_term as int) < raft_index_limit(),
            Event::RecvAppendEntries { wire, has_cmd, leader_current_term, .. } => {
                &&& (leader_current_term as int) < raft_index_limit()
                &&& has_cmd == wire.spec_has_payload()
            },
            Event::TickHeartbeat { is_leader, .. } => {
                &&& (is_leader ==> self.is_leader_)
                &&& self.config_members_@.contains(self.site_id_)
            },
            Event::RecvAppendReply { ord, term, .. } => {
                &&& ord < self.pending_rpcs_.spec_len()
                &&& (term as int) < raft_index_limit()
            },
            Event::RoundEnd { .. } => {
                &&& self.round_.spec_nservers() > 0
                &&& self.config_members_@.contains(self.site_id_)
            },
            _ => true,
        }
    }

    // [fix, F9] What a message from the network must be before the core
    // takes it at all (plan A.2): integer compares only. An AppendEntries or
    // RequestVote from this server itself or from a site outside the
    // configuration (B20); a term 0, which no leader or candidate ever
    // holds; an AppendEntries whose prev index and prev term are not both 0
    // or both positive (raft-rs's shape). A payload's own defects (an entry
    // term below 1, a count that overflows) stay the decoder's refusals,
    // after the handler's gates (A.2's closing paragraph).
    pub fn message_admitted<W: InboundBatch<C>>(&self, ev: &Event<'_, C, W>) -> (r: bool)
        ensures
            match *ev {
                // [M12] what admission checks
                Event::RecvAppendEntries { leader_current_term, leader_site_id,
                    leader_prev_log_index, leader_prev_log_term, .. } => r == {
                    &&& leader_site_id != self.site_id_
                    &&& self.config_members_@.contains(leader_site_id)
                    &&& leader_current_term != 0
                    &&& (leader_prev_log_index == 0) == (leader_prev_log_term == 0)
                },
                Event::RecvRequestVote { can_id, can_term, .. } => r == {
                    &&& can_id != self.site_id_
                    &&& self.config_members_@.contains(can_id)
                    &&& can_term != 0
                },
                _ => r,
            },
    {
        match ev {
            Event::RecvAppendEntries {
                leader_current_term, leader_site_id, leader_prev_log_index,
                leader_prev_log_term, ..
            } => {
                *leader_site_id != self.site_id_
                    && self.is_config_member(*leader_site_id)
                    && *leader_current_term != 0
                    && ((*leader_prev_log_index == 0) == (*leader_prev_log_term == 0))
            },
            Event::RecvRequestVote { can_id, can_term, .. } => {
                *can_id != self.site_id_ && self.is_config_member(*can_id) && *can_term != 0
            },
            _ => true,
        }
    }

    // [fix, F9] step, for a message from the network. A message that fails
    // message_admitted is dropped (None): the core does not see it, and the
    // shell answers as an unavailable replica answers, which the sender
    // reads as no reply. A success reply claiming more than this leader's
    // log -- no follower can have accepted entries the leader never had --
    // is read as no reply too: its slot is released and nothing is learned.
    pub fn step_checked<W: InboundBatch<C>>(&mut self, ev: Event<'_, C, W>,
                                            out: &mut CoreOutput) -> (r: Option<Reply<C>>)
        requires
            old(self).inv(),
            old(self).admits(&ev),
        ensures
            final(self).inv(),
            r is None ==> *final(self) == *old(self),
            // [M12] the coupling kept (F9's admission and reply bound are
            // its own)
            old(self).ginv() && old(self).coupled_checked(&ev) ==> final(self).ginv(),
            match ev {
                Event::RecvAppendEntries { .. } => r matches Some(reply) ==> reply is Append,
                Event::RecvRequestVote { .. } => r matches Some(reply) ==> reply is Vote,
                Event::RecvAppendReply { .. } => r matches Some(reply) && reply is AppendReply,
                _ => r is Some,
            },
    {
        if !self.message_admitted(&ev) {
            return None;
        }
        match ev {
            Event::RecvAppendReply {
                ord, status, term, last_log_index, is_leader, stopped, failover,
            } => {
                let beyond_log: bool = status && last_log_index > self.raft_log_.last_index();
                let reply: Event<'_, C, W> = if beyond_log {
                    // the unavailable reply's 0/0/0
                    Event::RecvAppendReply {
                        ord, status: false, term: 0, last_log_index: 0, is_leader,
                        stopped, failover,
                    }
                } else {
                    Event::RecvAppendReply {
                        ord, status, term, last_log_index, is_leader, stopped, failover,
                    }
                };
                Some(self.step(reply, out))
            },
            other => Some(self.step(other, out)),
        }
    }

    // The core's one entry point. The caller holds mtx_.
    pub fn step<W: InboundBatch<C>>(&mut self, ev: Event<'_, C, W>,
                                    out: &mut CoreOutput) -> (r: Reply<C>)
        requires
            old(self).inv(),
            old(self).admits(&ev),
        ensures
            final(self).inv(),
            // [M12] the coupling kept, under each event's premise
            old(self).ginv() && old(self).coupled(&ev) ==> final(self).ginv(),
            match ev {
                Event::SetIdentity { .. } | Event::Configure { .. }
                | Event::RebuildPeers { .. } | Event::AbandonRound
                | Event::ResetRoundState | Event::SetFollower { .. }
                | Event::StepDown { .. } => r is Done,
                Event::EnterGates { .. } => r is Gates,
                Event::Propose { .. } => r is Proposed,
                Event::StartElection { .. } => r is Campaign,
                Event::SettleElection { .. } => r is Settled,
                Event::ResetElectionTimer { .. } => r is TimerReset,
                Event::RecvRequestVote { .. } => r is Vote,
                Event::RecvAppendEntries { .. } => r is Append,
                Event::TickHeartbeat { .. } => r is Tick,
                Event::RecvAppendReply { .. } => r is AppendReply,
                Event::RoundEnd { .. } => r is RoundEnd,
                Event::Applied { .. } => r is Applied,
            },
    {
        match ev {
            Event::SetIdentity { loc_id, site_id, partition_id } => {
                self.set_identity(loc_id, site_id, partition_id);
                Reply::Done
            },
            Event::Configure { members } => {
                self.configure(members);
                Reply::Done
            },
            Event::EnterGates { snapshots_enabled, failover } => {
                Reply::Gates(self.enter_gates(snapshots_enabled, failover))
            },
            Event::RebuildPeers { next_index } => {
                let ghost pre = *self;
                self.rebuild_peer_tables(next_index);
                proof {
                    // [M12] the peer table rebuilt at match 0
                    if pre.ginv() {
                        crate::coupling::lemma_rebuild_ginv(&pre, self);
                    }
                }
                Reply::Done
            },
            Event::Propose { cmd, has_value, is_tpc_commit, kind, payload_bytes } => {
                Reply::Proposed(self.append_local(cmd, has_value, is_tpc_commit, kind,
                                                  payload_bytes))
            },
            Event::StartElection { timer_guarded, expected_generation, now, stopped } => {
                Reply::Campaign(self.start_election(timer_guarded, expected_generation,
                                                    now, stopped, out))
            },
            Event::SettleElection {
                term, loc_id, voters, granted, reply_terms, n_total, timed_out,
                stopped, looping, failover, election_debug,
            } => {
                Reply::Settled(self.election_settle(term, loc_id, voters, granted,
                                                    reply_terms, n_total, timed_out,
                                                    stopped, looping, failover,
                                                    election_debug, out))
            },
            Event::ResetElectionTimer { now, timeout_us } => {
                Reply::TimerReset(self.reset_election_timer(now, timeout_us))
            },
            Event::RecvRequestVote {
                stopped, candidate_is_current_voter, lst_log_idx, lst_log_term,
                can_id, can_term, failover, election_debug,
            } => {
                // Every path of the handler writes both.
                let mut term: i64 = 0;
                let mut granted: i8 = 0;
                raft_on_request_vote(self, stopped, candidate_is_current_voter,
                                     lst_log_idx, lst_log_term, can_id, can_term,
                                     &mut term, &mut granted, failover,
                                     election_debug, out);
                Reply::Vote { term, granted }
            },
            Event::RecvAppendEntries {
                wire, stopped, sender_is_current_voter, has_cmd,
                leader_current_term, leader_site_id, leader_prev_log_index,
                leader_prev_log_term, leader_commit_index, failover,
            } => {
                // Every path of the handler writes all three.
                let mut ok: u64 = 0;
                let mut term: u64 = 0;
                let mut last_log_index: u64 = 0;
                let report = raft_on_append_entries(
                    self, wire, stopped, sender_is_current_voter, has_cmd,
                    leader_current_term, leader_site_id, leader_prev_log_index,
                    leader_prev_log_term, leader_commit_index, &mut ok, &mut term,
                    &mut last_log_index, failover, out);
                Reply::Append { report, ok, term, last_log_index }
            },
            Event::TickHeartbeat {
                is_leader, snapshot_configured, batching, max_batch_entries,
                max_batch_bytes,
            } => {
                Reply::Tick(heartbeat_tick(self, is_leader, snapshot_configured,
                                           batching, max_batch_entries,
                                           max_batch_bytes, out))
            },
            Event::RecvAppendReply {
                ord, status, term, last_log_index, is_leader, stopped, failover,
            } => {
                Reply::AppendReply(heartbeat_on_reply(self, ord, status, term,
                                                      last_log_index, is_leader,
                                                      stopped, failover, out))
            },
            Event::AbandonRound => {
                heartbeat_abandon_round(self);
                Reply::Done
            },
            Event::RoundEnd { is_leader } => {
                Reply::RoundEnd(heartbeat_round_end(self, is_leader, out))
            },
            Event::ResetRoundState => {
                self.reset_round_state();
                Reply::Done
            },
            Event::Applied { index, published } => {
                Reply::Applied(self.on_applied(index, published, out))
            },
            Event::SetFollower { stopped, failover } => {
                let ghost pre = *self;
                self.set_is_leader(false, stopped, failover, out);
                proof {
                    // [M12] a leader steps aside; anyone else is unchanged
                    if pre.ginv() {
                        if pre.is_leader_ {
                            self.g_log_@ = crate::coupling::step_aside_log(pre.g_log_@);
                            self.g_votes_@ = Set::<int>::empty();
                            crate::coupling::lemma_step_aside_ginv(&pre, self);
                        } else {
                            crate::coupling::lemma_same_view_ginv(&pre, self);
                        }
                    }
                }
                Reply::Done
            },
            Event::StepDown { stopped, failover } => {
                let ghost pre = *self;
                self.step_down(stopped, failover, out);
                proof {
                    // [M12] a leader or candidate steps aside; a follower is
                    // unchanged
                    if pre.ginv() {
                        if !(pre.role_view() is Follower) {
                            self.g_log_@ = crate::coupling::step_aside_log(pre.g_log_@);
                            self.g_votes_@ = Set::<int>::empty();
                            crate::coupling::lemma_step_aside_ginv(&pre, self);
                        } else {
                            crate::coupling::lemma_same_view_ginv(&pre, self);
                        }
                    }
                }
                Reply::Done
            },
        }
    }
}

} // verus!
