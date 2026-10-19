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
    // verified_config_ok: the gate (F5). [fix, F32] unsaved_restart: a
    // memory build restarted from a snapshot image, its term and vote lost
    EnterGates { unsaved_restart: bool, failover: bool },  // [fix, F32]
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
        // [fix, F23] The shell's monotonic clock, and how long a follower's
        // newest AppendEntries must have been in flight before it is sent
        // another batch (0: never, one in flight per follower as before F23).
        now_us: u64,
        pipeline_after_us: u64,
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

    // ---- Role changes the shell makes itself: the lab's construction ----
    SetFollower { stopped: bool, failover: bool },
    StepDown { stopped: bool, failover: bool },

    // ---- Disk persistence (docs/verus/disk-persistence.md §3) ----
    // [fix, F22] A recovered state, before EnterGates: term, vote, commit and
    // the entries after the snapshot boundary, last first. [fix, F24] `d`: the
    // store's last durable record number, which the persistence coupling
    // numbers the restored node's records from (unused at run time).
    // [fix, F30] `snap_index`, `snap_term`: the recovered snapshot's
    // boundary (0: none), which the log restarts after.
    Restore { term: u64, vote: u16, commit: u64, d: u64, snap_index: u64, snap_term: i64,  // [fix, F30]
              entries_rev: Vec<RaftEntry<C>> },  // [fix, F24]
    // [fix, F25] E1: the snapshot the shell just saved at snapshot_point();
    // E2: CompactLog's compaction (coupling-design-persistence-snapshots.md B3)
    CreateSnapshot { index: u64, term: i64 },  // [fix, F25]
    Compact { up_to: u64 },  // [fix, F25]
    // [fix, F27] E4: an InstallSnapshot's leader contact (the wire's leader
    // id, RAFT_SERVER_INVALID_SITE_ID when it fits no site id) with the
    // shell's applied index; E5: the snapshot its payload kernel then loaded
    RecvInstallSnapshot { term: u64, leader_site_id: u16, last_included_index: u64,  // [fix, F27]
                          last_included_term: u64, applied: u64, stopped: bool,  // [fix, F27]
                          failover: bool,  // [fix, F27]
                          // [Part B] the prefix the Snapshot carries (ghost)
                          #[cfg(verus_keep_ghost)]
                          prefix: Ghost<Seq<glr::protocol::Raft::types::LLogEntry>> },
    InstallSnapshot { index: u64, term: u64,  // [fix, F27]
                      // [Part B] the Snapshot it installs: sender and prefix (ghost)
                      #[cfg(verus_keep_ghost)]
                      src: Ghost<int>,
                      #[cfg(verus_keep_ghost)]
                      prefix: Ghost<Seq<glr::protocol::Raft::types::LLogEntry>> },
    // [fix, F28] E6: an InstallSnapshot's reply, as its send recorded it
    RecvSnapshotReply { ord: usize, site_id: u16, snap_last_idx: u64, send_term: u64,  // [fix, F28]
                        follower_term: u64, stopped: bool, failover: bool },  // [fix, F28]
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
    // [fix, F22] whether Restore took the state (false: refused, unchanged)
    Restored(bool),
    // [fix, F25] whether CreateSnapshot moved the boundary; what Compact dropped
    Created(bool),  // [fix, F25]
    Compacted(u64),  // [fix, F25]
    // [fix, F27] RecvInstallSnapshot's: the answer's term (0: unavailable),
    // whether the leader contact was accepted, whether to install;
    // InstallSnapshot's: whether it was taken, and the log after it kept
    InstallContact { reply_term: u64, contact: bool, install: bool },  // [fix, F27]
    Installed { taken: bool, retain: bool },  // [fix, F27]
    // [fix, F28] whether the follower was credited through the snapshot
    SnapshotReply(bool),  // [fix, F28]
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

    // [fix, F22] (whole item)
    pub fn into_restored(self) -> bool
        requires self is Restored,
    {
        match self {
            Reply::Restored(ok) => ok,
            _ => vstd::pervasive::unreached(),
        }
    }
    // [fix, F25] (whole item)
    pub fn into_created(self) -> bool
        requires self is Created,
    {
        match self {
            Reply::Created(ok) => ok,
            _ => vstd::pervasive::unreached(),
        }
    }

    // [fix, F25] (whole item)
    pub fn into_compacted(self) -> u64
        requires self is Compacted,
    {
        match self {
            Reply::Compacted(n) => n,
            _ => vstd::pervasive::unreached(),
        }
    }

    // [fix, F27] (whole item)
    pub fn into_install_contact(self) -> (u64, bool, bool)
        requires self is InstallContact,
    {
        match self {
            Reply::InstallContact { reply_term, contact, install } => (reply_term, contact, install),
            _ => vstd::pervasive::unreached(),
        }
    }

    // [fix, F27] (whole item)
    pub fn into_installed(self) -> (bool, bool)
        requires self is Installed,
    {
        match self {
            Reply::Installed { taken, retain } => (taken, retain),
            _ => vstd::pervasive::unreached(),
        }
    }

    // [fix, F28] (whole item)
    pub fn into_snapshot_reply(self) -> bool
        requires self is SnapshotReply,
    {
        match self {
            Reply::SnapshotReply(credited) => credited,
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
            // [fix, F22] a restore into an empty, ungated core (the host
            // contract: before EnterGates)
            Event::Restore { .. } => !self.gated_ && self.raft_log_.spec_len() == 0,
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
            // [M12] persistence (Part A): as step's, for a message taken
            r is Some && !(ev is Restore) ==> final(self).g_seq_@
                == old(self).g_seq_@ + (if final(out).spec_note() { 1int } else { 0int }),
            old(self).ginv() && old(self).coupled_checked(&ev) && old(self).persist_ok() && !(ev is Restore)
                && old(self).g_seq_@ + 1 < u64::MAX ==> {
                &&& final(self).persist_ok()
                &&& (r is Some ==> crate::coupling::ready_tail_ok(old(self).g_log_@, final(self).g_log_@,
                        final(self).g_seq_@))
            },
            // [M12] persistence (P2): as step's, for a message taken
            r is Some && !(ev is Restore) && old(out).unmarked()
                ==> crate::coupling::note_exact(old(self), final(self), final(out).spec_persist()),
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

    // [M12] (whole item) Restore's arm (P4; [Part B] E7): the restore, then,
    // under its premise, the previous run's cut stepped aside, the recovered
    // snapshot's prefix and the Ready numbered d. Apart from step, whose
    // query it would push past its limit.
    #[allow(clippy::too_many_arguments)]
    fn restore_coupled(&mut self, term: u64, vote: u16, commit: u64, _d: u64, snap_index: u64,
                       snap_term: i64, entries_rev: Vec<RaftEntry<C>>, out: &mut CoreOutput) -> (ok: bool)
        requires
            old(self).inv(),
            !old(self).gated_,
            old(self).raft_log_.spec_len() == 0,
        ensures
            final(self).inv(),
            old(self).ginv() && crate::coupling::restore_cert_ok(old(self), _d, term, vote, commit,
                snap_index, snap_term, entries_rev@) ==> final(self).ginv(),
            ok && old(self).ginv() && crate::coupling::restore_cert_ok(old(self), _d, term, vote, commit,
                snap_index, snap_term, entries_rev@) && (_d as int) + 1 < u64::MAX
                ==> final(self).persist_ok() && final(self).g_seq_@ == _d as int
                    && glr::protocol::ghost_log_certs::last_ready_pos::<glr::protocol::Raft::ghost_log::RaftP>(
                        final(self).g_log_@) == final(self).g_log_@.len() - 1,
    {
        let ghost pre = *self;
        let ghost rev = entries_rev@;
        proof { reveal(crate::coupling::restore_cert_ok); }
        // [M12] persistence (P4): the premise's previous run, and the
        // prev its crash at d left (the cut, P3)
        let ghost old = choose|o: Seq<glr::protocol::Raft::ghost_log::Entry>|
            crate::coupling::prev_run_ok(o, pre.c_view(), _d)
            && #[trigger] crate::coupling::restore_prev_ok(&pre,
                crate::coupling::restore_prev(o, _d, pre.n_view()), term, vote,
                glr::protocol::Raft::ghost_log::replay(
                    crate::coupling::restore_prev(o, _d, pre.n_view())).log, commit)
            && crate::coupling::restore_view(glr::protocol::Raft::ghost_log::replay(
                crate::coupling::restore_prev(o, _d, pre.n_view())).log, snap_index, snap_term, rev);
        let ghost prev = crate::coupling::restore_prev(old, _d, pre.n_view());
        let ghost log = glr::protocol::Raft::ghost_log::replay(prev).log;
        let ok: bool = self.restore(term, vote, commit, snap_index, snap_term, entries_rev, out);
        // [Part B] (E7) the recovered snapshot's prefix: prev's log through it
        #[cfg(verus_keep_ghost)]
        if ok && snap_index != 0 {
            self.raft_log_.set_prefix(Ghost(log.take(snap_index as int)));
        }
        proof {
            // [M12] (disk design §6) taken: prev stepped aside;
            // refused: nothing moved
            if ok && pre.ginv()
                && crate::coupling::restore_cert_ok(&pre, _d, term, vote, commit, snap_index,
                                                    snap_term, rev) {
                pre.raft_log_.lemma_wf_bounds();
                assert(pre.raft_log_.view() =~= Seq::<RaftEntry<C>>::empty());
                assert(self.raft_log_.view() =~= rev_seq(rev));
                if snap_index == 0 {
                    assert(log =~= log.skip(0));
                    assert(self.log_view() =~= log);
                } else {
                    assert(log =~= log.take(snap_index as int) + log.skip(snap_index as int));
                    assert(self.log_view() =~= log);
                }
                self.g_log_@ = crate::coupling::step_aside_log(prev);
                self.g_votes_@ = Set::<int>::empty();
                self.g_match_@ = glr::protocol::Raft::ghost_log::replay(prev).match_index;
                self.g_next_@ = glr::protocol::Raft::ghost_log::replay(prev).next_index;
                assert(self.log_entries_ok());
                crate::coupling::lemma_restore_ginv(&pre, self, prev);
                // the records restart at d, after the Ready numbered d
                if (_d as int) + 1 < u64::MAX {
                    crate::coupling::lemma_restore_prev_marks(old, pre.c_view(), _d, pre.n_view());
                    crate::coupling::lemma_ext_builders(prev);
                    crate::coupling::lemma_ready_ext(prev, self.g_log_@);
                    let ghost mid = *self;
                    crate::coupling::lemma_ext_refl(mid.g_log_@);
                    self.g_seq_@ = _d as int;
                    self.g_log_@ = mid.g_log_@.push(
                        glr::protocol::Raft::ghost_log::Entry::Mark(mid.ready_now(_d)));
                    crate::coupling::lemma_ready_step(&mid, self, mid.g_log_@, _d as int);
                }
            }
        }
        ok
    }

    // [M12] step's arms: each event's handler and its coupling, one query
    // apart from step's persistence epilogue, which reads only what this
    // ensures: the coupling kept, the reply's kind, the ghost log grown by
    // the arm's builders, and (P2) the arm's marks folding its first state
    // into its last.
    fn step_arm<W: InboundBatch<C>>(&mut self, ev: Event<'_, C, W>,  // [M12]
                                    out: &mut CoreOutput) -> (r: Reply<C>)
        requires
            old(self).inv(),
            old(self).admits(&ev),
        ensures
            final(self).inv(),
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
                Event::Restore { .. } => r is Restored,  // [fix, F22]
                Event::CreateSnapshot { .. } => r is Created,  // [fix, F25]
                Event::Compact { .. } => r is Compacted,  // [fix, F25]
                Event::RecvInstallSnapshot { .. } => r is InstallContact,  // [fix, F27]
                Event::InstallSnapshot { .. } => r is Installed,  // [fix, F27]
                Event::RecvSnapshotReply { .. } => r is SnapshotReply,  // [fix, F28]
            },
            old(self).ginv() && old(self).coupled(&ev) && !(ev is Restore)
                ==> crate::coupling::plain_ext(old(self).g_log_@, final(self).g_log_@),
            !(ev is Restore) && old(out).unmarked() ==> crate::coupling::log_fold(old(self), final(self),
                final(out).spec_log_from(), final(out).spec_snap()),
            match ev {
                Event::Restore { d, .. } => old(self).ginv() && old(self).coupled(&ev)
                    && r == Reply::<C>::Restored(true) && (d as int) + 1 < u64::MAX ==> {
                    &&& final(self).persist_ok()
                    &&& final(self).g_seq_@ == d as int
                    &&& glr::protocol::ghost_log_certs::last_ready_pos::<glr::protocol::Raft::ghost_log::RaftP>(
                            final(self).g_log_@) == final(self).g_log_@.len() - 1
                },
                _ => true,
            },
    {
        let ghost s0 = *self;  // [M12]
        let ghost evg = ev;  // [Part B] the event's ghost fields, past the match
        let r: Reply<C> = match ev {  // [M12]
            Event::SetIdentity { loc_id, site_id, partition_id } => {
                self.set_identity(loc_id, site_id, partition_id);
                Reply::Done
            },
            Event::Configure { members } => {
                self.configure(members);
                Reply::Done
            },
            Event::EnterGates { unsaved_restart, failover } => {  // [fix, F32]
                Reply::Gates(self.enter_gates(unsaved_restart, failover))  // [fix, F32]
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
                let previous: u64 = self.append_local(cmd, has_value, is_tpc_commit, kind,
                                                      payload_bytes);
                out.note_log_write(self.raft_log_.last_index());  // [fix, F21]
                Reply::Proposed(previous)
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
                max_batch_bytes, now_us, pipeline_after_us,  // [fix, F23]
            } => {
                Reply::Tick(heartbeat_tick(self, is_leader, snapshot_configured,
                                           batching, max_batch_entries,
                                           max_batch_bytes, now_us,
                                           pipeline_after_us, out))  // [fix, F23]
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
            Event::Restore { term, vote, commit, d: _d, snap_index, snap_term, entries_rev } => {  // [fix, F24] d is ghost-only [fix, F30]
                let ok: bool = self.restore_coupled(term, vote, commit, _d, snap_index, snap_term,  // [M12]
                                                    entries_rev, out);
                Reply::Restored(ok)
            },
            Event::CreateSnapshot { index, term } => {  // [fix, F25]
                Reply::Created(self.create_snapshot(index, term, out))  // [fix, F25]
            },
            Event::Compact { up_to } => Reply::Compacted(self.compact(up_to)),  // [fix, F25]
            Event::RecvInstallSnapshot { term, leader_site_id, last_included_index,  // [fix, F27]
                                         last_included_term, applied, stopped, failover, .. } => {  // [fix, F27]
                proof {
                    // [Part B] the inbox: the sender's rank, the prefix it sent
                    self.g_inbox_@ = (crate::coupling::rank(self.config_members_@, leader_site_id),
                                      evg->RecvInstallSnapshot_prefix@);
                }
                let (reply_term, contact, install) = self.recv_install_snapshot(  // [fix, F27]
                    term, leader_site_id, last_included_index, last_included_term, applied,
                    stopped, failover, out);
                Reply::InstallContact { reply_term, contact, install }  // [fix, F27]
            },
            Event::InstallSnapshot { index, term, .. } => {  // [fix, F27]
                proof {
                    // [Part B] the inbox: the Snapshot this installs
                    self.g_inbox_@ = (evg->InstallSnapshot_src@, evg->InstallSnapshot_prefix@);
                }
                let (taken, retain) = self.install_snapshot(index, term, out);  // [fix, F27]
                Reply::Installed { taken, retain }  // [fix, F27]
            },
            Event::RecvSnapshotReply { ord, site_id, snap_last_idx, send_term, follower_term,  // [fix, F28]
                                       stopped, failover } => {  // [fix, F28]
                Reply::SnapshotReply(self.on_snapshot_reply(ord, site_id, snap_last_idx,  // [fix, F28]
                    send_term, follower_term, stopped, failover, out))
            },
        };
        proof {
            // [M12] persistence: the builders' growth
            if s0.ginv() && s0.coupled(&evg) && !(evg is Restore) {
                assert(crate::coupling::plain_ext(s0.g_log_@, self.g_log_@)) by {
                    crate::coupling::lemma_ext_all::<C>(s0.g_log_@);
                }
            }
        }
        r
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
                Event::Restore { .. } => r is Restored,  // [fix, F22]
                Event::CreateSnapshot { .. } => r is Created,  // [fix, F25]
                Event::Compact { .. } => r is Compacted,  // [fix, F25]
                Event::RecvInstallSnapshot { .. } => r is InstallContact,  // [fix, F27]
                Event::InstallSnapshot { .. } => r is Installed,  // [fix, F27]
                Event::RecvSnapshotReply { .. } => r is SnapshotReply,  // [fix, F28]
            },
            // [M12] persistence (Part A): the records the notes made, and the
            // Ready mark a coupled step ends with, numbered by them (P1)
            !(ev is Restore) ==> final(self).g_seq_@
                == old(self).g_seq_@ + (if final(out).spec_note() { 1int } else { 0int }),
            old(self).ginv() && old(self).coupled(&ev) && old(self).persist_ok() && !(ev is Restore)
                && old(self).g_seq_@ + 1 < u64::MAX ==> {
                &&& final(self).persist_ok()
                &&& crate::coupling::ready_tail_ok(old(self).g_log_@, final(self).g_log_@, final(self).g_seq_@)
            },
            // [M12] persistence (P2): the note is exact, and the next step
            // starts unmarked
            !(ev is Restore) && old(out).unmarked()
                ==> crate::coupling::note_exact(old(self), final(self), final(out).spec_persist()),
            final(out).unmarked(),
            // [M12] persistence (P4): a restart counts from the durable d, its
            // log the previous run's cut stepped aside, then Ready(d)
            match ev {
                Event::Restore { d, .. } => old(self).ginv() && old(self).coupled(&ev)
                    && r == Reply::<C>::Restored(true) && (d as int) + 1 < u64::MAX ==> {
                    &&& final(self).persist_ok()
                    &&& final(self).g_seq_@ == d as int
                    &&& glr::protocol::ghost_log_certs::last_ready_pos::<glr::protocol::Raft::ghost_log::RaftP>(
                            final(self).g_log_@) == final(self).g_log_@.len() - 1
                },
                _ => true,
            },
    {
        // [M12] persistence: the step's first state, and its builders' growth
        let ghost s0 = *self;
        let ghost pre_ok = s0.ginv() && s0.coupled(&ev) && s0.persist_ok() && !(ev is Restore);
        let ghost s0_out = *out;  // [M12] P2
        // [fix, F21] the saved fields before the arm, for the persist note
        let term0: u64 = self.current_term_;  // [fix, F21]
        let vote0: u16 = self.vote_for_;  // [fix, F21]
        let commit0: u64 = self.commit_index_;  // [fix, F21]
        let restoring: bool = matches!(ev, Event::Restore { .. });  // [fix, F21]
        let r: Reply<C> = self.step_arm(ev, out);  // [M12] the arms, apart
        // [fix, F21] the note: the hard state if it moved, the log's lowest
        // write; Restore loads state the store already holds, so none
        let hard: bool = !restoring && (self.current_term_ != term0 || self.vote_for_ != vote0
            || self.commit_index_ != commit0);  // [fix, F21]
        // [M12] P2: the arm's marks (step_arm: they fold the step's first state into this one)
        let ghost marks = out.notes();
        out.finish_persist(hard, self.current_term_, self.vote_for_, self.commit_index_);  // [fix, F21]
        proof {
            // [M12] persistence (Part A): a note is the next record; a coupled
            // step ends with a Ready mark carrying the certificate of the
            // state it leaves, numbered by the records so far
            let seq1: int = s0.g_seq_@ + (if out.spec_note() { 1int } else { 0int });
            if !restoring {
                self.g_seq_@ = seq1;
            }
            if pre_ok && s0.g_seq_@ + 1 < u64::MAX {
                let ghost mid = *self;
                self.g_log_@ = mid.g_log_@.push(
                    glr::protocol::Raft::ghost_log::Entry::Mark(mid.ready_now(seq1 as u64)));
                crate::coupling::lemma_ready_step(&mid, self, s0.g_log_@, seq1);
            }
            // [M12] P2: the note is exact
            if !restoring && s0_out.unmarked() {
                crate::coupling::lemma_note_exact(&s0, self, marks, hard);
            }
        }
        r
    }
}

} // verus!
