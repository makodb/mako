// [M12] (whole file) The coupling of the core to the group's Raft spec (plan
// Phase 8): what the core's state and messages mean as the spec's LState and
// LRaftMessage, the ghost log of the core's actions, and the lemmas that each
// closed segment is one of the spec's atomic steps. Verification only: the
// module exists when Verus checks the crate (cfg verus_keep_ghost), with the
// spec imported from the frozen tag (scripts/verus/verify_core.sh); plain
// cargo never compiles it.
//
// The design follows docs/verus/coupling-table.md and the group's raft-rs
// port (glr src/ports/raftrs/coupling.rs): a ghost log on the core
// (RaftCore::g_log_), opened by a message (Recv) or a spontaneous trigger
// (Tick), recording every write to a field the spec tracks (Set) and every
// message the core decides to send (Send), and closed with the action the
// segment is (Close). At every step boundary the log is fully closed, every
// closed segment is one atomic step of the spec (log_inv), every label is
// bound to its trigger (wf), every send is routed (routed), and replaying
// the log gives the core's state as the spec sees it (state_view).

#[allow(unused_imports)]
use crate::*;
use vstd::prelude::*;
#[allow(unused_imports)]
use glr::protocol::Raft::types::*;
#[allow(unused_imports)]
use glr::protocol::Raft::raft::*;
#[allow(unused_imports)]
use glr::protocol::Raft::ghost_log::*;
#[allow(unused_imports)]
use glr::protocol::Raft::membership::*;
#[allow(unused_imports)]
use glr::protocol::Raft::ghost_log_compose::cluster_c;

verus! {

// ===========================================================================
// The log side: the method's invariant and its four operations
// ===========================================================================

// What a ghost log keeps between any two of its entries: every closed
// segment refines, every label is bound to its trigger, every send routed.
pub open spec fn log_ok(l: Seq<Entry>, c: LConstants) -> bool {
    &&& log_inv(l, c)
    &&& wf(l)
    &&& routed(l)
}

// Every send of the open segment goes to `d` (the group's seg_sends_to: the
// routing ticket a close discharges).
pub open spec fn seg_sends_to(l: Seq<Entry>, d: int) -> bool {
    forall|t: int| last_boundary(l) <= t < l.len() && (#[trigger] l[t]) is Send ==> l[t]->Send_0 == d
}

// Open a group at a boundary: a message received (Recv) or a spontaneous
// trigger (Tick).
pub proof fn lemma_g_open(l: Seq<Entry>, e: Entry, c: LConstants)
    requires
        log_ok(l, c),
        fully_closed(l),
        is_opener(e),
    ensures
        log_ok(l.push(e), c),
        replay(l.push(e)) == replay(l),
        seg_state(l.push(e)) == replay(l),
        seg_sends(l.push(e)) == Seq::<LRaftMessage>::empty(),
        seg_trigger(l.push(e)) == Option::Some(e),
        forall|d: int| seg_sends_to(l.push(e), d),
{
    lemma_seg_open(l, e, c);
    lemma_routed_push_non_close(l, e);
    lemma_fully_closed_props(l);
    lemma_last_boundary_push_non_close(l, e);
    assert forall|d: int| seg_sends_to(l.push(e), d) by {
        assert forall|t: int| last_boundary(l.push(e)) <= t < l.push(e).len()
            && (#[trigger] l.push(e)[t]) is Send implies l.push(e)[t]->Send_0 == d by {
            assert(t == l.len());
        }
    }
}

// Record a write to a field the spec tracks.
pub proof fn lemma_g_set(l: Seq<Entry>, f: LogField, v: LogValue, c: LConstants)
    requires log_ok(l, c),
    ensures
        log_ok(l.push(Entry::Set(f, v)), c),
        replay(l.push(Entry::Set(f, v))) == apply_set(replay(l), f, v),
        seg_state(l.push(Entry::Set(f, v))) == seg_state(l),
        seg_sends(l.push(Entry::Set(f, v))) == seg_sends(l),
        seg_trigger(l.push(Entry::Set(f, v))) == seg_trigger(l),
        forall|d: int| seg_sends_to(l, d) ==> seg_sends_to(l.push(Entry::Set(f, v)), d),
{
    let e = Entry::Set(f, v);
    lemma_seg_set(l, f, v, c);
    lemma_routed_push_non_close(l, e);
    lemma_last_boundary_push_non_close(l, e);
    assert forall|d: int| seg_sends_to(l, d) implies seg_sends_to(l.push(e), d) by {
        assert forall|t: int| last_boundary(l.push(e)) <= t < l.push(e).len()
            && (#[trigger] l.push(e)[t]) is Send implies l.push(e)[t]->Send_0 == d by {
            if t < l.len() {
                assert(l.push(e)[t] == l[t]);
            }
        }
    }
}

// Record a message the core sends, to server `dst`.
pub proof fn lemma_g_send(l: Seq<Entry>, dst: int, m: LRaftMessage, c: LConstants)
    requires log_ok(l, c),
    ensures
        log_ok(l.push(Entry::Send(dst, m)), c),
        replay(l.push(Entry::Send(dst, m))) == replay(l),
        seg_state(l.push(Entry::Send(dst, m))) == seg_state(l),
        seg_sends(l.push(Entry::Send(dst, m))) == seg_sends(l).push(m),
        seg_trigger(l.push(Entry::Send(dst, m))) == seg_trigger(l),
        seg_sends_to(l, dst) ==> seg_sends_to(l.push(Entry::Send(dst, m)), dst),
{
    let e = Entry::Send(dst, m);
    lemma_seg_send(l, dst, m, c);
    lemma_routed_push_non_close(l, e);
    lemma_last_boundary_push_non_close(l, e);
    if seg_sends_to(l, dst) {
        assert forall|t: int| last_boundary(l.push(e)) <= t < l.push(e).len()
            && (#[trigger] l.push(e)[t]) is Send implies l.push(e)[t]->Send_0 == dst by {
            if t < l.len() {
                assert(l.push(e)[t] == l[t]);
            }
        }
    }
}

// Close the open segment as one atomic action: the caller owes the action
// (between the segment's start and now, with its sends), the label's binding
// to the trigger, and the sends' routing (all to one admitted destination).
//
// A group may hold several segments (a StepDown before the message's own
// action): the next one starts from the closed state, with the same trigger
// and nothing sent.
pub proof fn lemma_g_close(l: Seq<Entry>, label: ActionLabel, d: int, c: LConstants)
    requires
        log_ok(l, c),
        action_holds(label, seg_state(l), replay(l), c, seg_sends(l)),
        label_compatible(label, seg_trigger(l)),
        seg_sends(l) == Seq::<LRaftMessage>::empty()
            || (seg_sends_to(l, d) && dst_ok(label, seg_trigger(l), d)),
    ensures
        log_ok(l.push(Entry::Close(label)), c),
        fully_closed(l.push(Entry::Close(label))),
        replay(l.push(Entry::Close(label))) == replay(l),
        seg_trigger(l.push(Entry::Close(label))) == seg_trigger(l),
        seg_state(l.push(Entry::Close(label))) == replay(l),
        seg_sends(l.push(Entry::Close(label))) == Seq::<LRaftMessage>::empty(),
        forall|d2: int| seg_sends_to(l.push(Entry::Close(label)), d2),
{
    if seg_sends(l) == Seq::<LRaftMessage>::empty() {
        lemma_no_send_entries(l, last_boundary(l), l.len() as int);
    }
    assert(seg_dsts_ok(l, label));
    lemma_seg_close(l, label, c);
    lemma_routed_push_close(l, label);
    let l2 = l.push(Entry::Close(label));
    assert(last_boundary(l2) == l2.len());
    assert(l2.take(l2.len() as int) =~= l2);
}

// ===========================================================================
// Ranks: a site's index in the sorted configuration, the spec's server id
// ===========================================================================

pub open spec fn rank(m: Seq<u16>, site: u16) -> int {
    if m.contains(site) {
        choose|k: int| 0 <= k < m.len() && m[k] == site
    } else {
        m.len() as int
    }
}

pub proof fn lemma_rank_bounds(m: Seq<u16>, site: u16)
    requires m.contains(site),
    ensures
        0 <= rank(m, site) < m.len(),
        m[rank(m, site)] == site,
{
}

// A sorted configuration of u16 sites has at most 2^16 members (so a rank
// fits any spec key).
pub proof fn lemma_sorted_len(m: Seq<u16>)
    requires sites_sorted(m),
    ensures m.len() <= 65536,
{
    if m.len() > 0 {
        assert forall|i: int| 0 <= i < m.len() implies #[trigger] m[i] as int >= i by {
            lemma_sorted_ge(m, i);
        }
        assert(m[m.len() - 1] as int >= m.len() - 1);
    }
}

proof fn lemma_sorted_ge(m: Seq<u16>, i: int)
    requires sites_sorted(m), 0 <= i < m.len(),
    ensures m[i] as int >= i,
    decreases i,
{
    if i > 0 {
        lemma_sorted_ge(m, i - 1);
        assert(m[i - 1] < m[i]);
    }
}

// In a sorted configuration a member's rank is its position.
pub proof fn lemma_rank_of(m: Seq<u16>, k: int)
    requires
        sites_sorted(m),
        0 <= k < m.len(),
    ensures rank(m, m[k]) == k,
{
    assert(m.contains(m[k]));
    let j = rank(m, m[k]);
    lemma_rank_bounds(m, m[k]);
    if j < k {
        assert(m[j] < m[k]);
    } else if j > k {
        assert(m[k] < m[j]);
    }
}

// ===========================================================================
// The core as the spec sees it
// ===========================================================================

// The spec's match index for rank r (absent: 0).
pub open spec fn match_of(m: Map<u64, u64>, r: int) -> int {
    if m.contains_key(r as u64) { m[r as u64] as int } else { 0 }
}

// What a command means to the spec: uninterpreted, so every node reads a
// command the same way (the codec's fidelity is the host contract's).
pub uninterp spec fn value_view<C>(cmd: C) -> int;

pub open spec fn entry_view<C>(e: RaftEntry<C>) -> LLogEntry {
    LLogEntry {
        term: e.spec_term() as int,
        value: value_view(e.spec_cmd()),
        change: LConfChange::NoChange,
    }
}

impl<C> RaftCore<C> {
    // The cluster's size, this server's spec id, and the spec's constants
    // (the static configuration: every member is a voter).
    pub open spec fn n_view(&self) -> int {
        self.config_members_@.len() as int
    }

    pub open spec fn my_rank(&self) -> int {
        rank(self.config_members_@, self.site_id_)
    }

    pub open spec fn c_view(&self) -> LConstants {
        cluster_c(self.n_view(), Set::<int>::range(0, self.n_view()), self.my_rank())
    }

    // Leader; Candidate while a campaign of the current term runs; else
    // Follower.
    pub open spec fn role_view(&self) -> LServerRole {
        if self.is_leader_ {
            LServerRole::Leader
        } else if self.election_in_progress_ && self.election_term_ as int == self.current_term_ as int {
            LServerRole::Candidate
        } else {
            LServerRole::Follower
        }
    }

    pub open spec fn log_view(&self) -> Seq<LLogEntry> {
        self.raft_log_.view().map_values(|e: RaftEntry<C>| entry_view(e))
    }

    pub open spec fn state_view(&self) -> LState {
        LState {
            current_term: self.current_term_ as int,
            role: self.role_view(),
            has_voted: self.vote_for_ != RAFT_SERVER_INVALID_SITE_ID,
            voted_for: if self.vote_for_ != RAFT_SERVER_INVALID_SITE_ID {
                rank(self.config_members_@, self.vote_for_)
            } else {
                0int
            },
            log: self.log_view(),
            commit_index: self.commit_index_ as int,
            votes_granted: self.g_votes_@,
            match_index: self.g_match_@,
            next_index: self.g_next_@,
            pending_reads: Seq::<LReadReq>::empty(),
            served_ctxs: Set::<int>::empty(),
            config: Set::<int>::range(0, self.n_view()),
            conf_index: 0int,
        }
    }

    // What the ghost log certifies at every step boundary, and the facts
    // the coupling keeps beside it: no snapshot boundary term (snapshots
    // are off under the gate, and only they write it), no configured site
    // is the "no vote" sentinel, and a leader or a candidate voted for
    // itself.
    pub open spec fn ginv(&self) -> bool {
        &&& log_ok(self.g_log_@, self.c_view())
        &&& fully_closed(self.g_log_@)
        &&& replay(self.g_log_@) == self.state_view()
        &&& self.snapterm_ == 0
        &&& !self.config_members_@.contains(RAFT_SERVER_INVALID_SITE_ID)
        &&& (!(self.role_view() is Follower) ==> self.vote_for_ == self.site_id_)
        &&& (self.role_view() is Candidate ==> self.g_votes_@.contains(self.my_rank()))
        &&& self.log_entries_ok()
        &&& self.v2()
    }

    // V2 (plan §4.3): a leader's match index for a follower never exceeds
    // the spec's (absent: 0), so the spec's quorum covers the exec's. The
    // exec raises it by max(); the spec writes it only when it rises past.
    pub open spec fn v2(&self) -> bool {
        self.is_leader_ ==> forall|o: int| 0 <= o < self.peers_.spec_len()
            ==> (#[trigger] self.peers_.spec_match(o)) as int
                <= match_of(self.g_match_@, rank(self.config_members_@, self.peer_sites_@[o]))
    }

    // Every log entry's command has a value (bugs-found B16: the follower's
    // conflict scan reads a slot without one as absent), and its term is a
    // term (the handlers compare terms as u64).
    pub open spec fn log_entries_ok(&self) -> bool {
        forall|k: int| 0 <= k < self.raft_log_.view().len()
            ==> (#[trigger] self.raft_log_.view()[k]).spec_has_value()
                && self.raft_log_.view()[k].spec_term() >= 0
    }
}

impl<C> RaftCore<C> {
    // A core as RaftCore::new builds it (ghost): the spec's initial state,
    // an empty ghost log, no configuration yet.
    pub open spec fn fresh(&self) -> bool {
        &&& self.g_log_@ == Seq::<Entry>::empty()
        &&& self.current_term_ == 0
        &&& !self.is_leader_
        &&& !self.election_in_progress_
        &&& self.vote_for_ == RAFT_SERVER_INVALID_SITE_ID
        &&& self.raft_log_.view().len() == 0
        &&& self.commit_index_ == 0
        &&& self.g_votes_@ == Set::<int>::empty()
        &&& self.g_match_@ == Map::<u64, u64>::empty()
        &&& self.g_next_@ == Map::<u64, u64>::empty()
        &&& self.config_members_@.len() == 0
        &&& self.snapterm_ == 0
    }
}

// A fresh core is the spec's initial state, with an empty log: what makes
// the first step's ginv hold (step keeps it from there).
pub proof fn lemma_new_ginv<C>(core: &RaftCore<C>)
    requires core.fresh(),
    ensures core.ginv(),
{
    let c = core.c_view();
    lemma_empty_log_refines(c);
    lemma_routed_empty();
    let l = Seq::<Entry>::empty();
    assert(last_boundary(l) == 0);
    assert(l.take(0) =~= l);
    assert(wf(l));
    assert(core.log_view() =~= Seq::<LLogEntry>::empty());
    assert(Set::<int>::range(0, core.n_view()) =~= Set::<int>::empty());
    assert(!core.config_members_@.contains(RAFT_SERVER_INVALID_SITE_ID));
    assert(replay(l) == init_state());
    assert(core.state_view() == init_state());
}

// ===========================================================================
// Setup: the membership is loaded (LLoadConfig)
// ===========================================================================

// The ghost log a fresh core gains when its membership loads: one spontaneous
// segment that writes the configuration (every member a voter).
pub open spec fn load_config_log(n: int) -> Seq<Entry> {
    seq![
        Entry::Tick,
        Entry::Set(LogField::Config, LogValue::VIntSet(Set::<int>::range(0, n))),
        Entry::Close(ActionLabel::LoadConfig),
    ]
}

pub proof fn lemma_load_config<C>(pre: &RaftCore<C>, post: &RaftCore<C>)
    requires
        pre.ginv(),
        pre.g_log_@.len() == 0,
        post.g_log_@ == load_config_log(post.n_view()),
        // configure writes the membership (and the peer table) only
        post.current_term_ == pre.current_term_,
        post.is_leader_ == pre.is_leader_,
        post.election_in_progress_ == pre.election_in_progress_,
        post.election_term_ == pre.election_term_,
        post.vote_for_ == pre.vote_for_,
        post.raft_log_ == pre.raft_log_,
        post.commit_index_ == pre.commit_index_,
        post.g_votes_ == pre.g_votes_,
        post.g_match_ == pre.g_match_,
        post.g_next_ == pre.g_next_,
        post.snapterm_ == pre.snapterm_,
        // the host contract: no configured site is the sentinel
        !post.config_members_@.contains(RAFT_SERVER_INVALID_SITE_ID),
    ensures post.ginv(),
{
    let c = post.c_view();
    let n = post.n_view();
    let cfg = Set::<int>::range(0, n);
    let l0 = Seq::<Entry>::empty();
    assert(pre.g_log_@ =~= l0);
    // the empty log, under the new constants
    lemma_empty_log_refines(c);
    lemma_routed_empty();
    assert(last_boundary(l0) == 0);
    assert(l0.take(0) =~= l0);
    assert(log_ok(l0, c));
    // pre is the spec's initial state
    assert(pre.state_view() == init_state());
    // the segment
    lemma_g_open(l0, Entry::Tick, c);
    let l1 = l0.push(Entry::Tick);
    assert(seg_sends_to(l1, 0));
    lemma_g_set(l1, LogField::Config, LogValue::VIntSet(cfg), c);
    let l2 = l1.push(Entry::Set(LogField::Config, LogValue::VIntSet(cfg)));
    assert(seg_sends_to(l2, 0));
    assert(seg_state(l2) == init_state());
    assert(replay(l2) == LState { config: cfg, ..init_state() });
    assert(LLoadConfig(seg_state(l2), replay(l2), c, seg_sends(l2)));
    lemma_g_close(l2, ActionLabel::LoadConfig, 0, c);
    let l3 = l2.push(Entry::Close(ActionLabel::LoadConfig));
    assert(l3 =~= load_config_log(n));
    // the coupling: the replay is the initial state with the configuration
    assert(replay(l3) == LState { config: cfg, ..init_state() });
    assert(post.state_view().config == cfg);
    assert(post.log_view() == pre.log_view());
    assert(post.state_view() == LState { config: cfg, ..init_state() });
}

// ===========================================================================
// Frames: what the views read
// ===========================================================================

// Two cores agree on everything the spec state reads but the role and the
// vote tally (the fields a step aside or a step down moves).
pub open spec fn view_frame<C>(pre: &RaftCore<C>, post: &RaftCore<C>) -> bool {
    &&& post.current_term_ == pre.current_term_
    &&& post.vote_for_ == pre.vote_for_
    &&& post.raft_log_ == pre.raft_log_
    &&& post.commit_index_ == pre.commit_index_
    &&& post.config_members_ == pre.config_members_
    &&& post.site_id_ == pre.site_id_
    &&& post.g_match_ == pre.g_match_
    &&& post.g_next_ == pre.g_next_
    &&& post.snapterm_ == pre.snapterm_
    &&& post.peers_ == pre.peers_
    &&& post.peer_sites_ == pre.peer_sites_
}

// ===========================================================================
// Restore ([fix, F22], disk design §6): a restart resumes from the previous
// run's ghost log `prev`, as a step aside
// ===========================================================================

// Restore's premise: the core neither leads nor campaigns, its log is
// empty, and some previous ghost log's replay holds the restored term, vote,
// log and commit; the restarted core steps aside from it (LStepAside, no
// guard). An existential: the proof takes a witness, so it holds for every
// such log (host contract §1 item 5).
pub open spec fn restore_premise<C>(core: &RaftCore<C>, term: u64, vote: u16, commit: u64,
                                    rev: Seq<RaftEntry<C>>) -> bool {
    &&& !core.is_leader_
    &&& !core.election_in_progress_
    &&& core.raft_log_.spec_len() == 0
    &&& exists|prev: Seq<Entry>| #[trigger] restore_prev_ok(core, prev, term, vote, restore_log(rev), commit)
}

// Restore's entries, last first, as the log they make.
pub open spec fn restore_log<C>(rev: Seq<RaftEntry<C>>) -> Seq<LLogEntry> {
    rev_seq(rev).map_values(|e: RaftEntry<C>| entry_view(e))
}

// The previous run's ghost log certifies a state whose term, vote, log and
// commit are the restored ones, with no read in flight (a restart serves
// none) and the static configuration.
pub open spec fn restore_prev_ok<C>(core: &RaftCore<C>, prev: Seq<Entry>, term: u64, vote: u16,
                                    log: Seq<LLogEntry>, commit: u64) -> bool {
    let s = replay(prev);
    &&& log_ok(prev, core.c_view())
    &&& fully_closed(prev)
    &&& s.pending_reads == Seq::<LReadReq>::empty()
    &&& s.served_ctxs == Set::<int>::empty()
    &&& s.current_term == term as int
    &&& s.has_voted == (vote != RAFT_SERVER_INVALID_SITE_ID)
    &&& s.voted_for == (if vote != RAFT_SERVER_INVALID_SITE_ID {
            rank(core.config_members_@, vote)
        } else {
            0int
        })
    &&& s.log == log
    &&& s.commit_index == commit as int
    &&& s.config == Set::<int>::range(0, core.n_view())
    &&& s.conf_index == 0int
}

// A core restored from `prev`'s state, its ghost log `prev` stepped aside and
// its spec tables `prev`'s, keeps ginv.
pub proof fn lemma_restore_ginv<C>(pre: &RaftCore<C>, post: &RaftCore<C>, prev: Seq<Entry>)
    requires
        pre.ginv(),
        restore_prev_ok(pre, prev, post.current_term_, post.vote_for_, post.log_view(),
                        post.commit_index_),
        post.config_members_ == pre.config_members_,
        post.site_id_ == pre.site_id_,
        post.snapterm_ == pre.snapterm_,
        !post.is_leader_,
        !post.election_in_progress_,
        post.log_entries_ok(),
        post.g_log_@ == step_aside_log(prev),
        post.g_votes_@ == Set::<int>::empty(),
        post.g_match_@ == replay(prev).match_index,
        post.g_next_@ == replay(prev).next_index,
    ensures post.ginv(),
{
    let c = pre.c_view();
    lemma_step_aside(prev, c);
    assert(post.c_view() == c);
    assert(post.role_view() == LServerRole::Follower);
    assert(replay(step_aside_log(prev)) == post.state_view());
}

// ===========================================================================
// Step aside (LStepAside): a leader or a candidate returns to Follower at its
// term, with no guard; spontaneous (a Tick group)
// ===========================================================================

pub open spec fn step_aside_log(l: Seq<Entry>) -> Seq<Entry> {
    l.push(Entry::Tick)
        .push(Entry::Set(LogField::Role, LogValue::VRole(LServerRole::Follower)))
        .push(Entry::Set(LogField::VotesGranted, LogValue::VIntSet(Set::<int>::empty())))
        .push(Entry::Close(ActionLabel::StepAside))
}

pub proof fn lemma_step_aside(l: Seq<Entry>, c: LConstants)
    requires
        log_ok(l, c),
        fully_closed(l),
        replay(l).pending_reads == Seq::<LReadReq>::empty(),
        replay(l).served_ctxs == Set::<int>::empty(),
    ensures
        log_ok(step_aside_log(l), c),
        fully_closed(step_aside_log(l)),
        replay(step_aside_log(l)) == (LState {
            role: LServerRole::Follower,
            votes_granted: Set::<int>::empty(),
            ..replay(l)
        }),
{
    let s = replay(l);
    lemma_g_open(l, Entry::Tick, c);
    let l1 = l.push(Entry::Tick);
    assert(seg_sends_to(l1, 0));
    lemma_g_set(l1, LogField::Role, LogValue::VRole(LServerRole::Follower), c);
    let l2 = l1.push(Entry::Set(LogField::Role, LogValue::VRole(LServerRole::Follower)));
    assert(seg_sends_to(l2, 0));
    lemma_g_set(l2, LogField::VotesGranted, LogValue::VIntSet(Set::<int>::empty()), c);
    let l3 = l2.push(Entry::Set(LogField::VotesGranted, LogValue::VIntSet(Set::<int>::empty())));
    assert(seg_sends_to(l3, 0));
    let s_ = LState { role: LServerRole::Follower, votes_granted: Set::<int>::empty(), ..s };
    assert(seg_state(l3) == s);
    assert(replay(l3) == s_);
    assert(LStepAside(s, s_, c, seg_sends(l3)));
    lemma_g_close(l3, ActionLabel::StepAside, 0, c);
    assert(step_aside_log(l) =~= l3.push(Entry::Close(ActionLabel::StepAside)));
}

// A core that left the leader or candidate role at its term, its ghost log
// and tally updated as the step aside writes them, keeps ginv.
pub proof fn lemma_step_aside_ginv<C>(pre: &RaftCore<C>, post: &RaftCore<C>)
    requires
        pre.ginv(),
        view_frame(pre, post),
        post.role_view() == LServerRole::Follower,
        post.g_log_@ == step_aside_log(pre.g_log_@),
        post.g_votes_@ == Set::<int>::empty(),
    ensures post.ginv(),
{
    let c = pre.c_view();
    lemma_step_aside(pre.g_log_@, c);
    assert(post.c_view() == c);
    assert(post.log_view() == pre.log_view());
    assert(post.state_view() == (LState {
        role: LServerRole::Follower,
        votes_granted: Set::<int>::empty(),
        ..pre.state_view()
    }));
}

// A role change that does not move the role view changes nothing the spec
// sees.
pub proof fn lemma_same_view_ginv<C>(pre: &RaftCore<C>, post: &RaftCore<C>)
    requires
        pre.ginv(),
        view_frame(pre, post),
        post.role_view() == pre.role_view(),
        post.g_log_@ == pre.g_log_@,
        post.g_votes_@ == pre.g_votes_@,
    ensures post.ginv(),
{
    assert(post.c_view() == pre.c_view());
    assert(post.log_view() == pre.log_view());
    assert(post.state_view() == pre.state_view());
}

// ===========================================================================
// A proposal (LClientRequest): a leader appends at its term; spontaneous
// ===========================================================================

pub open spec fn client_request_log(l: Seq<Entry>, new_log: Seq<LLogEntry>, value: int) -> Seq<Entry> {
    l.push(Entry::Tick)
        .push(Entry::Set(LogField::RaftLog, LogValue::VLog(new_log)))
        .push(Entry::Close(ActionLabel::ClientRequest { value }))
}

// The log view grows by the appended entry's view.
pub proof fn lemma_log_view_push<C>(pre: &RaftCore<C>, post: &RaftCore<C>, e: RaftEntry<C>)
    requires post.raft_log_.view() == pre.raft_log_.view().push(e),
    ensures post.log_view() == pre.log_view().push(entry_view(e)),
{
    assert(post.log_view() =~= pre.log_view().push(entry_view(e)));
}

pub proof fn lemma_client_request_ginv<C>(pre: &RaftCore<C>, post: &RaftCore<C>, e: RaftEntry<C>)
    requires
        pre.ginv(),
        pre.is_leader_,
        post.raft_log_.view() == pre.raft_log_.view().push(e),
        e.spec_term() as int == pre.current_term_ as int,
        post.current_term_ == pre.current_term_,
        post.vote_for_ == pre.vote_for_,
        post.commit_index_ == pre.commit_index_,
        post.config_members_ == pre.config_members_,
        post.site_id_ == pre.site_id_,
        post.is_leader_ == pre.is_leader_,
        post.election_in_progress_ == pre.election_in_progress_,
        post.election_term_ == pre.election_term_,
        post.g_votes_ == pre.g_votes_,
        post.g_match_ == pre.g_match_,
        post.g_next_ == pre.g_next_,
        post.snapterm_ == pre.snapterm_,
        post.peers_ == pre.peers_,
        post.peer_sites_ == pre.peer_sites_,
        e.spec_has_value(),
        post.g_log_@ == client_request_log(pre.g_log_@, post.log_view(), value_view(e.spec_cmd())),
    ensures post.ginv(),
{
    let c = pre.c_view();
    let l = pre.g_log_@;
    let s = replay(l);
    lemma_log_view_push(pre, post, e);
    let new_log = post.log_view();
    let v = value_view(e.spec_cmd());
    lemma_g_open(l, Entry::Tick, c);
    let l1 = l.push(Entry::Tick);
    assert(seg_sends_to(l1, 0));
    lemma_g_set(l1, LogField::RaftLog, LogValue::VLog(new_log), c);
    let l2 = l1.push(Entry::Set(LogField::RaftLog, LogValue::VLog(new_log)));
    assert(seg_sends_to(l2, 0));
    assert(seg_state(l2) == s);
    assert(new_log == s.log.push(LLogEntry { term: s.current_term, value: v, change: LConfChange::NoChange }));
    assert(LClientRequest(s, replay(l2), c, v, seg_sends(l2)));
    lemma_g_close(l2, ActionLabel::ClientRequest { value: v }, 0, c);
    assert(post.g_log_@ =~= l2.push(Entry::Close(ActionLabel::ClientRequest { value: v })));
    assert(post.c_view() == c);
    assert(post.state_view() == LState { log: new_log, ..pre.state_view() });
}

// ===========================================================================
// A higher term received (LStepDown): its own segment, first in the group
// the message opened
// ===========================================================================

pub open spec fn step_down_seg(l: Seq<Entry>, new_term: int) -> Seq<Entry> {
    l.push(Entry::Set(LogField::CurrentTerm, LogValue::VInt(new_term)))
        .push(Entry::Set(LogField::Role, LogValue::VRole(LServerRole::Follower)))
        .push(Entry::Set(LogField::HasVoted, LogValue::VBool(false)))
        .push(Entry::Set(LogField::VotedFor, LogValue::VInt(0)))
        .push(Entry::Set(LogField::VotesGranted, LogValue::VIntSet(Set::<int>::empty())))
        .push(Entry::Close(ActionLabel::StepDown { new_term }))
}

pub open spec fn stepped_down(s: LState, new_term: int) -> LState {
    LState {
        current_term: new_term,
        role: LServerRole::Follower,
        has_voted: false,
        voted_for: 0int,
        votes_granted: Set::<int>::empty(),
        ..s
    }
}

// `l` is a group a message of term `new_term` opened, nothing written or
// sent in it yet.
pub proof fn lemma_step_down_seg(l: Seq<Entry>, new_term: int, c: LConstants)
    requires
        log_ok(l, c),
        seg_state(l) == replay(l),
        seg_sends(l) == Seq::<LRaftMessage>::empty(),
        seg_trigger(l) matches Option::Some(Entry::Recv(_, m)) && msg_term(m) == new_term,
        new_term > replay(l).current_term,
        replay(l).pending_reads == Seq::<LReadReq>::empty(),
        replay(l).served_ctxs == Set::<int>::empty(),
    ensures
        log_ok(step_down_seg(l, new_term), c),
        fully_closed(step_down_seg(l, new_term)),
        replay(step_down_seg(l, new_term)) == stepped_down(replay(l), new_term),
        seg_trigger(step_down_seg(l, new_term)) == seg_trigger(l),
        seg_state(step_down_seg(l, new_term)) == replay(step_down_seg(l, new_term)),
        seg_sends(step_down_seg(l, new_term)) == Seq::<LRaftMessage>::empty(),
        forall|d: int| seg_sends_to(step_down_seg(l, new_term), d),
{
    let s = replay(l);
    let e1 = Entry::Set(LogField::CurrentTerm, LogValue::VInt(new_term));
    let e2 = Entry::Set(LogField::Role, LogValue::VRole(LServerRole::Follower));
    let e3 = Entry::Set(LogField::HasVoted, LogValue::VBool(false));
    let e4 = Entry::Set(LogField::VotedFor, LogValue::VInt(0));
    let e5 = Entry::Set(LogField::VotesGranted, LogValue::VIntSet(Set::<int>::empty()));
    lemma_g_set(l, LogField::CurrentTerm, LogValue::VInt(new_term), c);
    let l1 = l.push(e1);
    lemma_g_set(l1, LogField::Role, LogValue::VRole(LServerRole::Follower), c);
    let l2 = l1.push(e2);
    lemma_g_set(l2, LogField::HasVoted, LogValue::VBool(false), c);
    let l3 = l2.push(e3);
    lemma_g_set(l3, LogField::VotedFor, LogValue::VInt(0), c);
    let l4 = l3.push(e4);
    lemma_g_set(l4, LogField::VotesGranted, LogValue::VIntSet(Set::<int>::empty()), c);
    let l5 = l4.push(e5);
    assert(replay(l5) == stepped_down(s, new_term));
    assert(LStepDown(seg_state(l5), replay(l5), c, new_term, seg_sends(l5)));
    lemma_g_close(l5, ActionLabel::StepDown { new_term }, 0, c);
    assert(step_down_seg(l, new_term) =~= l5.push(Entry::Close(ActionLabel::StepDown { new_term })));
}

// ===========================================================================
// An inbound RequestVote: StepDown when it carries a higher term and is
// taken up, then GrantVote or RejectVote, all in the request's group
// ===========================================================================

// The request as the spec sees it: the candidate is its rank
// (coupling-table §2).
pub open spec fn request_vote_msg(cfg: Seq<u16>, can_id: u16, can_term: i64, lst_log_idx: u64,
                                  lst_log_term: i64) -> LRaftMessage {
    LRaftMessage::RequestVote {
        term: can_term as int,
        candidate: rank(cfg, can_id),
        last_log_index: lst_log_idx as int,
        last_log_term: lst_log_term as int,
    }
}

pub open spec fn vote_label(m: LRaftMessage, granted: bool) -> ActionLabel {
    if granted {
        ActionLabel::GrantVote {
            candidate_term: m->RequestVote_term,
            candidate_last_log_term: m->RequestVote_last_log_term,
            candidate_last_log_index: m->RequestVote_last_log_index,
            candidate_id: m->RequestVote_candidate,
        }
    } else {
        ActionLabel::RejectVote {
            candidate_term: m->RequestVote_term,
            candidate_last_log_term: m->RequestVote_last_log_term,
            candidate_last_log_index: m->RequestVote_last_log_index,
            candidate_id: m->RequestVote_candidate,
        }
    }
}

// The request's group: the Recv; the StepDown segment when `stepped`; the
// vote recorded when `granted`; the answer, back to the candidate, the
// vote segment's one send.
pub open spec fn vote_group(l: Seq<Entry>, src: int, m: LRaftMessage, stepped: bool, granted: bool,
                            reply_term: int, me: int) -> Seq<Entry> {
    let l1 = l.push(Entry::Recv(src, m));
    let l2 = if stepped { step_down_seg(l1, m->RequestVote_term) } else { l1 };
    let l3 = if granted {
        l2.push(Entry::Set(LogField::HasVoted, LogValue::VBool(true)))
            .push(Entry::Set(LogField::VotedFor, LogValue::VInt(m->RequestVote_candidate)))
    } else {
        l2
    };
    l3.push(Entry::Send(src, LRaftMessage::VoteResponse { term: reply_term, granted, voter: me }))
        .push(Entry::Close(vote_label(m, granted)))
}

// Whether the handler takes the request's higher term up: a request it
// does not refuse as stopped, malformed or from a non-voter.
pub open spec fn vote_steps(stopped: bool, voter_ok: bool, can_term: i64, lst_log_term: i64,
                            cur: u64) -> bool {
    !stopped && can_term >= 0 && lst_log_term >= 0 && voter_ok
        && raft_server_signed_term_is_newer(can_term, cur)
}

// The term of the last entry, or the boundary's when the log is empty: what
// election_last_log_term reads under the gate.
pub open spec fn last_term_of<C>(core: &RaftCore<C>) -> int {
    if core.raft_log_.view().len() == 0 {
        core.snapterm_ as int
    } else {
        core.raft_log_.view().last().spec_term() as int
    }
}

pub proof fn lemma_vote_group<C>(pre: &RaftCore<C>, post: &RaftCore<C>, can_id: u16, can_term: i64,
                                 lst_log_idx: u64, lst_log_term: i64, stepped: bool, granted: bool,
                                 reply_term: i64)
    requires
        pre.inv(),
        pre.ginv(),
        pre.gated_,
        pre.config_members_@.contains(can_id),
        can_id != pre.site_id_,
        // a higher term taken up: the step down
        stepped ==> {
            &&& can_term as int > pre.current_term_ as int
            &&& post.current_term_ as int == can_term as int
            &&& !post.is_leader_
            &&& !post.election_in_progress_
        },
        !stepped ==> post.current_term_ == pre.current_term_,
        // the vote, granted as the handler's checks allow it
        post.vote_for_ == (if granted { can_id } else if stepped { RAFT_SERVER_INVALID_SITE_ID } else { pre.vote_for_ }),
        granted && !stepped ==> {
            &&& can_term as int == pre.current_term_ as int
            &&& (pre.vote_for_ == RAFT_SERVER_INVALID_SITE_ID || pre.vote_for_ == can_id)
            &&& !post.is_leader_
            &&& post.election_in_progress_ == pre.election_in_progress_
        },
        granted ==> {
            ||| lst_log_term as int > last_term_of(pre)
            ||| (lst_log_term as int == last_term_of(pre)
                 && lst_log_idx as int >= pre.raft_log_.spec_last_index())
        },
        !granted && !stepped ==> {
            &&& post.is_leader_ == pre.is_leader_
            &&& post.election_in_progress_ == pre.election_in_progress_
        },
        // the answer: the term the handler ends at
        reply_term as int == post.current_term_ as int,
        // what the vote does not touch
        post.election_term_ == pre.election_term_,
        post.raft_log_ == pre.raft_log_,
        post.commit_index_ == pre.commit_index_,
        post.config_members_ == pre.config_members_,
        post.site_id_ == pre.site_id_,
        post.snapterm_ == pre.snapterm_,
        post.peers_ == pre.peers_,
        post.peer_sites_ == pre.peer_sites_,
        post.g_votes_@ == (if stepped { Set::<int>::empty() } else { pre.g_votes_@ }),
        post.g_match_ == pre.g_match_,
        post.g_next_ == pre.g_next_,
        post.g_log_@ == vote_group(pre.g_log_@, rank(pre.config_members_@, can_id),
            request_vote_msg(pre.config_members_@, can_id, can_term, lst_log_idx, lst_log_term),
            stepped, granted, reply_term as int, pre.my_rank()),
    ensures post.ginv(),
{
    let c = pre.c_view();
    let cfg = pre.config_members_@;
    let src = rank(cfg, can_id);
    let me = pre.my_rank();
    let m = request_vote_msg(cfg, can_id, can_term, lst_log_idx, lst_log_term);
    let l0 = pre.g_log_@;
    let s0 = replay(l0);
    // the identities: a member is not the sentinel
    assert(pre.site_id_ != RAFT_SERVER_INVALID_SITE_ID);
    assert(can_id != RAFT_SERVER_INVALID_SITE_ID);
    // the log, as the spec counts it: base 1 under the gate
    pre.raft_log_.lemma_wf_bounds();
    assert(pre.log_view().len() == pre.raft_log_.spec_last_index());
    lemma_g_open(l0, Entry::Recv(src, m), c);
    let l1 = l0.push(Entry::Recv(src, m));
    // the vote segment's start: after the step down, or the Recv itself
    let l2 = if stepped { step_down_seg(l1, can_term as int) } else { l1 };
    let s2 = if stepped { stepped_down(s0, can_term as int) } else { s0 };
    if stepped {
        lemma_step_down_seg(l1, can_term as int, c);
    }
    assert(log_ok(l2, c));
    assert(replay(l2) == s2);
    assert(seg_state(l2) == s2);
    assert(seg_sends(l2) == Seq::<LRaftMessage>::empty());
    assert(seg_trigger(l2) == Option::Some(Entry::Recv(src, m)));
    assert(seg_sends_to(l2, src));
    // s2 is a follower at the request's term when granting
    if granted && !stepped {
        assert(pre.vote_for_ != pre.site_id_);
        assert(pre.role_view() is Follower);
    }
    let reply = LRaftMessage::VoteResponse { term: reply_term as int, granted, voter: me };
    let label = vote_label(m, granted);
    let l3 = if granted {
        l2.push(Entry::Set(LogField::HasVoted, LogValue::VBool(true)))
            .push(Entry::Set(LogField::VotedFor, LogValue::VInt(src)))
    } else {
        l2
    };
    if granted {
        lemma_g_set(l2, LogField::HasVoted, LogValue::VBool(true), c);
        let l2a = l2.push(Entry::Set(LogField::HasVoted, LogValue::VBool(true)));
        lemma_g_set(l2a, LogField::VotedFor, LogValue::VInt(src), c);
    }
    let s3 = replay(l3);
    assert(seg_state(l3) == s2);
    assert(seg_sends(l3) == Seq::<LRaftMessage>::empty());
    assert(seg_trigger(l3) == Option::Some(Entry::Recv(src, m)));
    assert(seg_sends_to(l3, src));
    lemma_g_send(l3, src, reply, c);
    let l4 = l3.push(Entry::Send(src, reply));
    assert(seg_sends(l4) =~= seq![reply]);
    if granted {
        assert(s3 == LState { has_voted: true, voted_for: src, ..s2 });
        assert(LGrantVote(s2, s3, c, can_term as int, lst_log_term as int, lst_log_idx as int, src, seg_sends(l4)));
        assert(candidate_log_ok(s2, lst_log_term as int, lst_log_idx as int));
    } else {
        assert(s3 == s2);
        assert(LRejectVote(s2, s3, c, can_term as int, lst_log_term as int, lst_log_idx as int, src, seg_sends(l4)));
    }
    lemma_g_close(l4, label, src, c);
    assert(post.g_log_@ =~= l4.push(Entry::Close(label)));
    // the coupling
    assert(post.c_view() == c);
    assert(post.log_view() == pre.log_view());
    assert(post.state_view() == s3);
}

// ===========================================================================
// A campaign (LTimeout): a follower takes the next term, votes for itself
// and broadcasts its RequestVote; Tick-opened
// ===========================================================================

// The RequestVote a started campaign broadcasts, as the spec sees it.
pub open spec fn campaign_msg(me: int, r: CampaignStart) -> LRaftMessage {
    LRaftMessage::RequestVote {
        term: r.term_ as int,
        candidate: me,
        last_log_index: r.lst_idx_ as int,
        last_log_term: r.lst_term_ as int,
    }
}

// The campaign's segment. A broadcast records one send (dst_ok admits any
// destination; the composition copies it to every server).
pub open spec fn timeout_log(l: Seq<Entry>, me: int, m: LRaftMessage) -> Seq<Entry> {
    l.push(Entry::Tick)
        .push(Entry::Set(LogField::CurrentTerm, LogValue::VInt(m->RequestVote_term)))
        .push(Entry::Set(LogField::Role, LogValue::VRole(LServerRole::Candidate)))
        .push(Entry::Set(LogField::HasVoted, LogValue::VBool(true)))
        .push(Entry::Set(LogField::VotedFor, LogValue::VInt(me)))
        .push(Entry::Set(LogField::VotesGranted, LogValue::VIntSet(Set::<int>::empty().insert(me))))
        .push(Entry::Send(me, m))
        .push(Entry::Close(ActionLabel::Timeout))
}

// The log view holds no configuration entry (a static configuration).
pub proof fn lemma_no_conf_entries<C>(core: &RaftCore<C>, lo: int, hi: int)
    ensures no_conf_entry_in(core.state_view(), lo, hi),
{
    let s = core.state_view();
    assert forall|i: int| lo < i <= hi && i <= s.log.len() implies !#[trigger] conf_entry_at(s, i) by {
        if 1 <= i {
            assert(s.log[i - 1] == entry_view(core.raft_log_.view()[i - 1]));
        }
    }
}

// Under the gate a member's rank is a spec server id, and this server's
// is in the configuration.
pub proof fn lemma_my_rank<C>(core: &RaftCore<C>)
    requires
        core.inv(),
        core.gated_,
    ensures
        0 <= core.my_rank() < core.n_view(),
        Set::<int>::range(0, core.n_view()).contains(core.my_rank()),
        core.c_view().servers.contains(core.my_rank()),
{
    lemma_rank_bounds(core.config_members_@, core.site_id_);
}

pub proof fn lemma_timeout_ginv<C>(pre: &RaftCore<C>, post: &RaftCore<C>, r: CampaignStart)
    requires
        pre.inv(),
        pre.ginv(),
        pre.gated_,
        // admitted: no leader, no campaign
        !pre.is_leader_,
        !pre.election_in_progress_,
        // the campaign
        post.current_term_ as int == pre.current_term_ as int + 1,
        post.vote_for_ == pre.site_id_,
        !post.is_leader_,
        post.election_in_progress_,
        post.election_term_ as int == post.current_term_ as int,
        post.g_votes_@ == Set::<int>::empty().insert(pre.my_rank()),
        // what it broadcasts: the term, the log's last index and term
        r.term_ == post.current_term_,
        r.lst_idx_ as int == pre.raft_log_.spec_last_index(),
        r.lst_term_ as int == last_term_of(pre),
        // what it does not touch
        post.raft_log_ == pre.raft_log_,
        post.commit_index_ == pre.commit_index_,
        post.config_members_ == pre.config_members_,
        post.site_id_ == pre.site_id_,
        post.snapterm_ == pre.snapterm_,
        post.g_match_ == pre.g_match_,
        post.g_next_ == pre.g_next_,
        post.g_log_@ == timeout_log(pre.g_log_@, pre.my_rank(), campaign_msg(pre.my_rank(), r)),
    ensures post.ginv(),
{
    let c = pre.c_view();
    let me = pre.my_rank();
    let m = campaign_msg(me, r);
    let t = r.term_ as int;
    let l0 = pre.g_log_@;
    let s0 = replay(l0);
    lemma_my_rank(pre);
    pre.raft_log_.lemma_wf_bounds();
    assert(pre.log_view().len() == pre.raft_log_.spec_last_index());
    assert(pre.site_id_ != RAFT_SERVER_INVALID_SITE_ID);
    // the guards: a follower; a voter with no configuration change pending
    assert(pre.role_view() is Follower);
    lemma_no_conf_entries(pre, 0, s0.commit_index);
    assert(timeout_ok(s0, c));
    // the segment
    let ve = LogValue::VIntSet(Set::<int>::empty().insert(me));
    lemma_g_open(l0, Entry::Tick, c);
    let l1 = l0.push(Entry::Tick);
    assert(seg_sends_to(l1, me));
    lemma_g_set(l1, LogField::CurrentTerm, LogValue::VInt(t), c);
    let l2 = l1.push(Entry::Set(LogField::CurrentTerm, LogValue::VInt(t)));
    assert(seg_sends_to(l2, me));
    lemma_g_set(l2, LogField::Role, LogValue::VRole(LServerRole::Candidate), c);
    let l3 = l2.push(Entry::Set(LogField::Role, LogValue::VRole(LServerRole::Candidate)));
    assert(seg_sends_to(l3, me));
    lemma_g_set(l3, LogField::HasVoted, LogValue::VBool(true), c);
    let l4 = l3.push(Entry::Set(LogField::HasVoted, LogValue::VBool(true)));
    assert(seg_sends_to(l4, me));
    lemma_g_set(l4, LogField::VotedFor, LogValue::VInt(me), c);
    let l5 = l4.push(Entry::Set(LogField::VotedFor, LogValue::VInt(me)));
    assert(seg_sends_to(l5, me));
    lemma_g_set(l5, LogField::VotesGranted, ve, c);
    let l6 = l5.push(Entry::Set(LogField::VotesGranted, ve));
    assert(seg_sends_to(l6, me));
    lemma_g_send(l6, me, m, c);
    let l7 = l6.push(Entry::Send(me, m));
    assert(seg_sends(l7) =~= seq![m]);
    let s7 = replay(l7);
    assert(s7 == LState {
        current_term: t,
        role: LServerRole::Candidate,
        has_voted: true,
        voted_for: me,
        votes_granted: Set::<int>::empty().insert(me),
        ..s0
    });
    if s0.log.len() > 0 {
        assert(s0.log[s0.log.len() - 1] == entry_view(pre.raft_log_.view().last()));
    }
    assert(LTimeout(s0, s7, c, seg_sends(l7)));
    lemma_g_close(l7, ActionLabel::Timeout, me, c);
    assert(post.g_log_@ =~= l7.push(Entry::Close(ActionLabel::Timeout)));
    // the coupling
    assert(post.c_view() == c);
    assert(post.log_view() == pre.log_view());
    assert(post.role_view() is Candidate);
    assert(post.state_view() == s7);
}

// ===========================================================================
// A campaign settled (election_settle): each granted reply is
// LReceiveVoteGranted and a yes quorum then LBecomeLeader; a reply of a
// higher term is LStepDown; a lost or timed-out campaign LStepAside
// ===========================================================================

// A vote reply from `voter` as the spec sees it (coupling-table §2): the
// voter is the reply's peer (F1), at its rank.
pub open spec fn vote_reply_view(cfg: Seq<u16>, voter: u16, granted: bool, term: i64) -> LRaftMessage {
    LRaftMessage::VoteResponse { term: term as int, granted, voter: rank(cfg, voter) }
}

// What the shell hands a settlement (the host contract): the configured
// size; replies from other members (the peer each callback was made for,
// F1); a grant at the campaign's term (the reply answers this campaign's
// request, and a voter grants at the request's term).
pub open spec fn settle_inputs_ok<C>(core: &RaftCore<C>, voters: Seq<u16>, granted: Seq<bool>,
                                     terms: Seq<i64>, term: u64, n_total: u64) -> bool {
    &&& n_total as int == core.config_members_@.len()
    &&& forall|k: int| 0 <= k < voters.len()
            ==> core.config_members_@.contains(#[trigger] voters[k]) && voters[k] != core.site_id_
    &&& forall|k: int| 0 <= k < granted.len() && #[trigger] granted[k] ==> terms[k] as int == term as int
}

// The entries a settlement adds from position `lo`: no sends, and every
// receive one of the replies the shell handed in.
pub open spec fn settle_entries_ok(l: Seq<Entry>, lo: int, cfg: Seq<u16>, voters: Seq<u16>,
                                   granted: Seq<bool>, terms: Seq<i64>) -> bool {
    forall|i: int| lo <= i < l.len() ==> {
        &&& !(#[trigger] l[i] is Send)
        &&& (l[i] is Recv ==> exists|k: int| 0 <= k < voters.len()
                && l[i] == Entry::Recv(rank(cfg, voters[k]), vote_reply_view(cfg, voters[k], granted[k], terms[k])))
    }
}

// The granted voters among the first k replies, as ranks.
pub open spec fn granted_ranks(cfg: Seq<u16>, voters: Seq<u16>, granted: Seq<bool>, k: int) -> Set<int>
    decreases k,
{
    if k <= 0 {
        Set::<int>::empty()
    } else if granted[k - 1] {
        granted_ranks(cfg, voters, granted, k - 1).insert(rank(cfg, voters[k - 1]))
    } else {
        granted_ranks(cfg, voters, granted, k - 1)
    }
}

pub proof fn lemma_granted_ranks_contains(cfg: Seq<u16>, voters: Seq<u16>, granted: Seq<bool>, k: int, j: int)
    requires
        0 <= j < k,
        granted[j],
    ensures granted_ranks(cfg, voters, granted, k).contains(rank(cfg, voters[j])),
    decreases k,
{
    if j < k - 1 {
        lemma_granted_ranks_contains(cfg, voters, granted, k - 1, j);
    }
}

// The granted replies' groups, the first k: Recv, the tally with the voter
// added, Close(ReceiveVoteGranted).
pub open spec fn grant_groups(l: Seq<Entry>, cfg: Seq<u16>, voters: Seq<u16>, granted: Seq<bool>,
                              terms: Seq<i64>, votes0: Set<int>, k: int) -> Seq<Entry>
    decreases k,
{
    if k <= 0 {
        l
    } else {
        let rest = grant_groups(l, cfg, voters, granted, terms, votes0, k - 1);
        if granted[k - 1] {
            let v = rank(cfg, voters[k - 1]);
            rest.push(Entry::Recv(v, vote_reply_view(cfg, voters[k - 1], true, terms[k - 1])))
                .push(Entry::Set(LogField::VotesGranted,
                    LogValue::VIntSet(votes0.union(granted_ranks(cfg, voters, granted, k)))))
                .push(Entry::Close(ActionLabel::ReceiveVoteGranted {
                    vote_term: terms[k - 1] as int, vote_granted: true, voter: v }))
        } else {
            rest
        }
    }
}

pub proof fn lemma_grant_groups(l: Seq<Entry>, c: LConstants, cfg: Seq<u16>, voters: Seq<u16>,
                                granted: Seq<bool>, terms: Seq<i64>, votes0: Set<int>, k: int)
    requires
        log_ok(l, c),
        fully_closed(l),
        c.servers == Set::<int>::range(0, cfg.len() as int),
        replay(l).role is Candidate,
        replay(l).votes_granted == votes0,
        0 <= k <= voters.len(),
        voters.len() == granted.len(),
        voters.len() == terms.len(),
        forall|j: int| 0 <= j < k && #[trigger] granted[j]
            ==> terms[j] as int == replay(l).current_term && cfg.contains(voters[j]),
    ensures
        log_ok(grant_groups(l, cfg, voters, granted, terms, votes0, k), c),
        fully_closed(grant_groups(l, cfg, voters, granted, terms, votes0, k)),
        replay(grant_groups(l, cfg, voters, granted, terms, votes0, k)) == (LState {
            votes_granted: votes0.union(granted_ranks(cfg, voters, granted, k)),
            ..replay(l)
        }),
        l.is_prefix_of(grant_groups(l, cfg, voters, granted, terms, votes0, k)),
        settle_entries_ok(grant_groups(l, cfg, voters, granted, terms, votes0, k), l.len() as int,
            cfg, voters, granted, terms),
    decreases k,
{
    let s = replay(l);
    if k == 0 {
        assert(votes0.union(Set::<int>::empty()) =~= votes0);
        assert(l.subrange(0, l.len() as int) =~= l);
    } else {
        lemma_grant_groups(l, c, cfg, voters, granted, terms, votes0, k - 1);
        let rest = grant_groups(l, cfg, voters, granted, terms, votes0, k - 1);
        let gr = granted_ranks(cfg, voters, granted, k - 1);
        if granted[k - 1] {
            let v = rank(cfg, voters[k - 1]);
            let m = vote_reply_view(cfg, voters[k - 1], true, terms[k - 1]);
            let gk = granted_ranks(cfg, voters, granted, k);
            let vs = votes0.union(gk);
            lemma_rank_bounds(cfg, voters[k - 1]);
            lemma_g_open(rest, Entry::Recv(v, m), c);
            let l1 = rest.push(Entry::Recv(v, m));
            lemma_g_set(l1, LogField::VotesGranted, LogValue::VIntSet(vs), c);
            let l2 = l1.push(Entry::Set(LogField::VotesGranted, LogValue::VIntSet(vs)));
            let sr = replay(rest);
            assert(vs =~= votes0.union(gr).insert(v));
            let label = ActionLabel::ReceiveVoteGranted { vote_term: terms[k - 1] as int, vote_granted: true, voter: v };
            assert(LReceiveVoteGranted(sr, replay(l2), c, terms[k - 1] as int, true, v, seg_sends(l2)));
            lemma_g_close(l2, label, 0, c);
            let l3 = l2.push(Entry::Close(label));
            assert(l3 == grant_groups(l, cfg, voters, granted, terms, votes0, k));
            assert(replay(l3) =~= (LState { votes_granted: vs, ..s }));
            // the prefix and the new entries
            assert(l.is_prefix_of(l3)) by {
                assert(l3.subrange(0, l.len() as int) =~= rest.subrange(0, l.len() as int));
            }
            assert forall|i: int| l.len() <= i < l3.len() implies {
                &&& !(#[trigger] l3[i] is Send)
                &&& (l3[i] is Recv ==> exists|j: int| 0 <= j < voters.len()
                        && l3[i] == Entry::Recv(rank(cfg, voters[j]), vote_reply_view(cfg, voters[j], granted[j], terms[j])))
            } by {
                if i < rest.len() {
                    assert(l3[i] == rest[i]);
                } else if i == rest.len() {
                    assert(l3[i] == Entry::Recv(v, m));
                    assert(vote_reply_view(cfg, voters[k - 1], granted[k - 1], terms[k - 1]) == m);
                }
            }
        } else {
            assert(granted_ranks(cfg, voters, granted, k) == gr);
        }
    }
}

// BecomeLeader: spontaneous, its own Tick group; the match and next
// tables start empty.
pub open spec fn become_leader_log(l: Seq<Entry>) -> Seq<Entry> {
    l.push(Entry::Tick)
        .push(Entry::Set(LogField::Role, LogValue::VRole(LServerRole::Leader)))
        .push(Entry::Set(LogField::MatchIndex, LogValue::VU64Map(Map::<u64, u64>::empty())))
        .push(Entry::Set(LogField::NextIndex, LogValue::VU64Map(Map::<u64, u64>::empty())))
        .push(Entry::Close(ActionLabel::BecomeLeader))
}

pub proof fn lemma_become_leader(l: Seq<Entry>, c: LConstants)
    requires
        log_ok(l, c),
        fully_closed(l),
        replay(l).role is Candidate,
        election_quorum_ok(replay(l)),
        replay(l).pending_reads == Seq::<LReadReq>::empty(),
        replay(l).served_ctxs == Set::<int>::empty(),
    ensures
        log_ok(become_leader_log(l), c),
        fully_closed(become_leader_log(l)),
        replay(become_leader_log(l)) == (LState {
            role: LServerRole::Leader,
            match_index: Map::<u64, u64>::empty(),
            next_index: Map::<u64, u64>::empty(),
            ..replay(l)
        }),
{
    let s = replay(l);
    let em = LogValue::VU64Map(Map::<u64, u64>::empty());
    lemma_g_open(l, Entry::Tick, c);
    let l1 = l.push(Entry::Tick);
    lemma_g_set(l1, LogField::Role, LogValue::VRole(LServerRole::Leader), c);
    let l2 = l1.push(Entry::Set(LogField::Role, LogValue::VRole(LServerRole::Leader)));
    lemma_g_set(l2, LogField::MatchIndex, em, c);
    let l3 = l2.push(Entry::Set(LogField::MatchIndex, em));
    lemma_g_set(l3, LogField::NextIndex, em, c);
    let l4 = l3.push(Entry::Set(LogField::NextIndex, em));
    assert(LBecomeLeader(seg_state(l4), replay(l4), c, seg_sends(l4)));
    lemma_g_close(l4, ActionLabel::BecomeLeader, 0, c);
    assert(become_leader_log(l) =~= l4.push(Entry::Close(ActionLabel::BecomeLeader)));
}

// The exec's yes quorum is the spec's: this server and the voters counted
// yes, all distinct members, are a majority of the configuration.
pub proof fn lemma_settle_quorum(cfg: Seq<u16>, me_site: u16, yes: Set<u16>, votes: Set<int>, n_total: u64)
    requires
        sites_sorted(cfg),
        cfg.contains(me_site),
        n_total as int == cfg.len(),
        yes.len() >= n_total as int / 2,
        forall|x: u16| yes.contains(x) ==> cfg.contains(x) && x != me_site,
        votes.contains(rank(cfg, me_site)),
        forall|x: u16| #[trigger] yes.contains(x) ==> votes.contains(rank(cfg, x)),
    ensures
        votes.intersect(Set::<int>::range(0, cfg.len() as int)).len()
            >= majority(Set::<int>::range(0, cfg.len() as int)),
{
    broadcast use Set::lemma_map_contains;
    let n = cfg.len() as int;
    let range = Set::<int>::range(0, n);
    let f = |x: u16| rank(cfg, x);
    let ry = yes.map(f);
    assert(yes.injective_on(f)) by {
        assert forall|a: u16, b: u16| yes.contains(a) && yes.contains(b) && #[trigger] f(a) == #[trigger] f(b)
            implies a == b by {
            lemma_rank_bounds(cfg, a);
            lemma_rank_bounds(cfg, b);
        }
    }
    vstd::set_lib::lemma_map_size(yes, ry, f);
    let me = rank(cfg, me_site);
    lemma_rank_bounds(cfg, me_site);
    assert(!ry.contains(me)) by {
        if ry.contains(me) {
            let x = choose|x: u16| yes.contains(x) && me == f(x);
            lemma_rank_bounds(cfg, x);
        }
    }
    let q = ry.insert(me);
    assert(q.subset_of(votes.intersect(range))) by {
        assert forall|r: int| q.contains(r) implies votes.intersect(range).contains(r) by {
            if r != me {
                let x = choose|x: u16| yes.contains(x) && r == f(x);
                lemma_rank_bounds(cfg, x);
            }
        }
    }
    vstd::set_lib::lemma_len_subset(q, votes.intersect(range));
    vstd::set_lib::lemma_int_range(0, n);
    assert(q.len() == yes.len() + 1);
}

// A vote reply of a higher term (ADVANCE_HIGHER_TERM): its Recv, then the
// StepDown segment.
pub open spec fn settle_step_down_log(l: Seq<Entry>, cfg: Seq<u16>, voter: u16, granted: bool,
                                      term: i64) -> Seq<Entry> {
    step_down_seg(l.push(Entry::Recv(rank(cfg, voter), vote_reply_view(cfg, voter, granted, term))), term as int)
}

pub proof fn lemma_settle_step_down<C>(pre: &RaftCore<C>, post: &RaftCore<C>, voters: Seq<u16>,
                                       granted: Seq<bool>, terms: Seq<i64>, k: int)
    requires
        pre.inv(),
        pre.ginv(),
        pre.gated_,
        0 <= k < voters.len(),
        voters.len() == granted.len(),
        voters.len() == terms.len(),
        pre.config_members_@.contains(voters[k]),
        terms[k] as int > pre.current_term_ as int,
        post.current_term_ as int == terms[k] as int,
        post.vote_for_ == RAFT_SERVER_INVALID_SITE_ID,
        !post.is_leader_,
        !post.election_in_progress_,
        post.raft_log_ == pre.raft_log_,
        post.commit_index_ == pre.commit_index_,
        post.config_members_ == pre.config_members_,
        post.site_id_ == pre.site_id_,
        post.snapterm_ == pre.snapterm_,
        post.g_votes_@ == Set::<int>::empty(),
        post.g_match_ == pre.g_match_,
        post.g_next_ == pre.g_next_,
        post.g_log_@ == settle_step_down_log(pre.g_log_@, pre.config_members_@, voters[k], granted[k], terms[k]),
    ensures
        post.ginv(),
        pre.g_log_@.is_prefix_of(post.g_log_@),
        settle_entries_ok(post.g_log_@, pre.g_log_@.len() as int, pre.config_members_@, voters, granted, terms),
{
    let c = pre.c_view();
    let cfg = pre.config_members_@;
    let l0 = pre.g_log_@;
    let e = Entry::Recv(rank(cfg, voters[k]), vote_reply_view(cfg, voters[k], granted[k], terms[k]));
    lemma_g_open(l0, e, c);
    let l1 = l0.push(e);
    lemma_step_down_seg(l1, terms[k] as int, c);
    let l2 = step_down_seg(l1, terms[k] as int);
    assert(post.c_view() == c);
    assert(post.log_view() == pre.log_view());
    assert(post.state_view() == stepped_down(replay(l0), terms[k] as int));
    assert(l0.is_prefix_of(l2)) by {
        assert(l2.subrange(0, l0.len() as int) =~= l0);
    }
    assert forall|i: int| l0.len() <= i < l2.len() implies {
        &&& !(#[trigger] l2[i] is Send)
        &&& (l2[i] is Recv ==> exists|j: int| 0 <= j < voters.len()
                && l2[i] == Entry::Recv(rank(cfg, voters[j]), vote_reply_view(cfg, voters[j], granted[j], terms[j])))
    } by {
        if i == l0.len() {
            assert(l2[i] == e);
        }
    }
}

// A won campaign: the granted replies' groups, BecomeLeader, and when the
// shell's loop has stopped (or set_is_leader declined) the rollback's
// StepAside.
pub open spec fn settle_won_log(l: Seq<Entry>, cfg: Seq<u16>, voters: Seq<u16>, granted: Seq<bool>,
                                terms: Seq<i64>, votes0: Set<int>, rollback: bool) -> Seq<Entry> {
    let lb = become_leader_log(grant_groups(l, cfg, voters, granted, terms, votes0, voters.len() as int));
    if rollback { step_aside_log(lb) } else { lb }
}

pub proof fn lemma_settle_won<C>(pre: &RaftCore<C>, post: &RaftCore<C>, voters: Seq<u16>, granted: Seq<bool>,
                                 terms: Seq<i64>, term: u64, n_total: u64, yes: Set<u16>, rollback: bool)
    requires
        pre.inv(),
        pre.ginv(),
        pre.gated_,
        pre.role_view() is Candidate,
        pre.current_term_ == term,
        voters.len() == granted.len(),
        voters.len() == terms.len(),
        settle_inputs_ok(pre, voters, granted, terms, term, n_total),
        // the exec's yes: its voters were fed granted, and are a quorum
        yes.len() >= n_total as int / 2,
        forall|x: u16| #[trigger] yes.contains(x)
            ==> exists|k: int| 0 <= k < voters.len() && voters[k] == x && granted[k],
        // the role it ends in
        rollback ==> !post.is_leader_ && !post.election_in_progress_,
        !rollback ==> post.is_leader_,
        post.current_term_ == pre.current_term_,
        post.vote_for_ == pre.vote_for_,
        post.raft_log_ == pre.raft_log_,
        post.commit_index_ == pre.commit_index_,
        post.config_members_ == pre.config_members_,
        post.site_id_ == pre.site_id_,
        post.snapterm_ == pre.snapterm_,
        post.g_votes_@ == (if rollback { Set::<int>::empty() } else {
            pre.g_votes_@.union(granted_ranks(pre.config_members_@, voters, granted, voters.len() as int)) }),
        post.g_match_@ == Map::<u64, u64>::empty(),
        post.g_next_@ == Map::<u64, u64>::empty(),
        // a new leader's peer table starts at match 0 (V2 against the empty
        // spec table)
        !rollback ==> forall|o: int| 0 <= o < post.peers_.spec_len() ==> #[trigger] post.peers_.spec_match(o) == 0,
        post.g_log_@ == settle_won_log(pre.g_log_@, pre.config_members_@, voters, granted, terms,
            pre.g_votes_@, rollback),
    ensures
        post.ginv(),
        pre.g_log_@.is_prefix_of(post.g_log_@),
        settle_entries_ok(post.g_log_@, pre.g_log_@.len() as int, pre.config_members_@, voters, granted, terms),
{
    let c = pre.c_view();
    let cfg = pre.config_members_@;
    let n = cfg.len() as int;
    let l0 = pre.g_log_@;
    let s0 = replay(l0);
    let votes0 = pre.g_votes_@;
    let len = voters.len() as int;
    lemma_my_rank(pre);
    // the granted replies
    lemma_grant_groups(l0, c, cfg, voters, granted, terms, votes0, len);
    let lg = grant_groups(l0, cfg, voters, granted, terms, votes0, len);
    let gr = granted_ranks(cfg, voters, granted, len);
    let sg = replay(lg);
    // the quorum
    assert forall|x: u16| #[trigger] yes.contains(x) implies votes0.union(gr).contains(rank(cfg, x)) by {
        let k = choose|k: int| 0 <= k < voters.len() && voters[k] == x && granted[k];
        lemma_granted_ranks_contains(cfg, voters, granted, len, k);
    }
    assert forall|x: u16| yes.contains(x) implies cfg.contains(x) && x != pre.site_id_ by {
        let k = choose|k: int| 0 <= k < voters.len() && voters[k] == x && granted[k];
    }
    lemma_settle_quorum(cfg, pre.site_id_, yes, votes0.union(gr), n_total);
    assert(sg.config == Set::<int>::range(0, n));
    assert(election_quorum_ok(sg));
    // BecomeLeader
    lemma_become_leader(lg, c);
    let lb = become_leader_log(lg);
    let sb = replay(lb);
    // the new entries: the groups' receives, then no receive or send
    assert(l0.is_prefix_of(lb)) by {
        assert(lb.subrange(0, l0.len() as int) =~= lg.subrange(0, l0.len() as int));
    }
    assert forall|i: int| l0.len() <= i < lb.len() implies {
        &&& !(#[trigger] lb[i] is Send)
        &&& (lb[i] is Recv ==> exists|j: int| 0 <= j < voters.len()
                && lb[i] == Entry::Recv(rank(cfg, voters[j]), vote_reply_view(cfg, voters[j], granted[j], terms[j])))
    } by {
        if i < lg.len() {
            assert(lb[i] == lg[i]);
        }
    }
    if rollback {
        lemma_step_aside(lb, c);
        let la = step_aside_log(lb);
        assert(l0.is_prefix_of(la)) by {
            assert(la.subrange(0, l0.len() as int) =~= lb.subrange(0, l0.len() as int));
        }
        assert forall|i: int| l0.len() <= i < la.len() implies {
            &&& !(#[trigger] la[i] is Send)
            &&& (la[i] is Recv ==> exists|j: int| 0 <= j < voters.len()
                    && la[i] == Entry::Recv(rank(cfg, voters[j]), vote_reply_view(cfg, voters[j], granted[j], terms[j])))
        } by {
            if i < lb.len() {
                assert(la[i] == lb[i]);
            }
        }
        assert(post.role_view() is Follower);
        assert(post.state_view() == replay(la));
    } else {
        assert(post.role_view() is Leader);
        assert(post.log_view() == pre.log_view());
        assert(post.state_view() == sb);
    }
    assert(post.c_view() == c);
}

// A settlement that only leaves the campaign (stopped, lost, timed out): a
// candidate steps aside; any other role is unchanged.
pub proof fn lemma_settle_aside<C>(pre: &RaftCore<C>, post: &RaftCore<C>, voters: Seq<u16>,
                                   granted: Seq<bool>, terms: Seq<i64>)
    requires
        pre.ginv(),
        view_frame(pre, post),
        post.role_view() == (if pre.role_view() is Candidate { LServerRole::Follower } else { pre.role_view() }),
        post.g_log_@ == (if pre.role_view() is Candidate { step_aside_log(pre.g_log_@) } else { pre.g_log_@ }),
        post.g_votes_@ == (if pre.role_view() is Candidate { Set::<int>::empty() } else { pre.g_votes_@ }),
    ensures
        post.ginv(),
        pre.g_log_@.is_prefix_of(post.g_log_@),
        settle_entries_ok(post.g_log_@, pre.g_log_@.len() as int, pre.config_members_@, voters, granted, terms),
{
    let l0 = pre.g_log_@;
    if pre.role_view() is Candidate {
        lemma_step_aside_ginv(pre, post);
        let la = step_aside_log(l0);
        assert(l0.is_prefix_of(la)) by {
            assert(la.subrange(0, l0.len() as int) =~= l0);
        }
        assert forall|i: int| l0.len() <= i < la.len() implies !(#[trigger] la[i] is Send) && !(la[i] is Recv) by {
        }
    } else {
        lemma_same_view_ginv(pre, post);
        assert(l0.is_prefix_of(l0)) by {
            assert(l0.subrange(0, l0.len() as int) =~= l0);
        }
    }
}

// ===========================================================================
// An inbound AppendEntries (raft_on_append_entries): StepDown when its term
// is higher; then RejectAppendEntries, or one FollowerAppendEntries segment
// per component (BR2), each component in a group of its own
// ===========================================================================

// The follower's log after the first i components, as the spec applies them
// one by one (ae_log_after: append at the end, keep a same-term entry,
// replace a conflicting tail).
pub open spec fn comp_logs<C>(log0: Seq<LLogEntry>, prev: int, es: Seq<RaftEntry<C>>, i: int) -> Seq<LLogEntry>
    decreases i,
{
    if i <= 0 {
        log0
    } else {
        let l = comp_logs(log0, prev, es, i - 1);
        let p = prev + i - 1;
        let e = entry_view(es[i - 1]);
        if p == l.len() {
            l.push(e)
        } else if l[p].term == e.term {
            l
        } else {
            l.take(p).push(e)
        }
    }
}

pub open spec fn entries_view<C>(es: Seq<RaftEntry<C>>) -> Seq<LLogEntry> {
    es.map_values(|e: RaftEntry<C>| entry_view(e))
}

// Mako's single pass is the spec's one by one: the components before the
// first conflict (f0) match the log, and from f0 on the batch replaces the
// tail.
pub proof fn lemma_comp_logs<C>(log0: Seq<LLogEntry>, prev: int, es: Seq<RaftEntry<C>>, f0: int, i: int)
    requires
        0 <= prev <= log0.len(),
        0 <= f0 <= es.len(),
        0 <= i <= es.len(),
        prev + f0 <= log0.len(),
        forall|p: int| prev <= p < prev + f0 ==> #[trigger] log0[p].term == es[p - prev].spec_term() as int,
        f0 < es.len() ==> prev + f0 == log0.len() || log0[prev + f0].term != es[f0].spec_term() as int,
    ensures
        comp_logs(log0, prev, es, i) == (if i <= f0 { log0 } else {
            log0.take(prev + f0) + entries_view(es.subrange(f0, i)) }),
    decreases i,
{
    if i > 0 {
        lemma_comp_logs(log0, prev, es, f0, i - 1);
        let e = entry_view(es[i - 1]);
        if i <= f0 {
            assert(log0[prev + i - 1].term == es[i - 1].spec_term() as int);
        } else if i == f0 + 1 {
            if prev + f0 == log0.len() {
                assert(log0.take(prev + f0) =~= log0);
            }
            assert(log0.take(prev + f0) + entries_view(es.subrange(f0, i)) =~= log0.take(prev + f0).push(e));
        } else {
            let l = log0.take(prev + f0) + entries_view(es.subrange(f0, i - 1));
            assert(l.len() == prev + i - 1);
            assert(log0.take(prev + f0) + entries_view(es.subrange(f0, i)) =~= l.push(e));
        }
    }
}

// After component i (i >= 1) the entry at prev + i is the batch's entry
// i - 1, as far as its term goes, and the log reaches it.
pub proof fn lemma_comp_logs_prev<C>(log0: Seq<LLogEntry>, prev: int, es: Seq<RaftEntry<C>>, f0: int, i: int)
    requires
        0 <= prev <= log0.len(),
        0 <= f0 <= es.len(),
        1 <= i <= es.len(),
        prev + f0 <= log0.len(),
        forall|p: int| prev <= p < prev + f0 ==> #[trigger] log0[p].term == es[p - prev].spec_term() as int,
        f0 < es.len() ==> prev + f0 == log0.len() || log0[prev + f0].term != es[f0].spec_term() as int,
    ensures
        prev + i <= comp_logs(log0, prev, es, i).len(),
        comp_logs(log0, prev, es, i)[prev + i - 1].term == es[i - 1].spec_term() as int,
        i > f0 ==> comp_logs(log0, prev, es, i).len() == prev + i,
{
    lemma_comp_logs(log0, prev, es, f0, i);
    if i <= f0 {
        assert(log0[prev + i - 1].term == es[i - 1].spec_term() as int);
    } else {
        let l = log0.take(prev + f0) + entries_view(es.subrange(f0, i));
        assert(l[prev + i - 1] == entry_view(es[i - 1]));
    }
}

// An inbound AppendEntries as the spec sees it: the leader's rank, the
// RPC's term, prev, prev term and commit, the payload's entries, and this
// server's rank (the answers' sender).
pub struct AeView<C> {
    pub ldr: int,
    pub term: int,
    pub prev: int,
    pub prev_term: int,
    pub lc: int,
    pub es: Seq<RaftEntry<C>>,
    pub me: int,
}

// The view of the RPC raft_on_append_entries handles.
pub open spec fn ae_view<C, W: InboundBatch<C>>(core: &RaftCore<C>, wire: &W, leader: u16, term: u64,
                                              prev: u64, prev_term: u64, lc: u64) -> AeView<C> {
    AeView {
        ldr: rank(core.config_members_@, leader),
        term: term as int,
        prev: prev as int,
        prev_term: prev_term as int,
        lc: lc as int,
        es: wire.spec_entries(),
        me: core.my_rank(),
    }
}

// The exec's append result is the spec's components: the old log cut at the
// first write (f0 past prev), then the batch from there.
pub proof fn lemma_ae_result<C>(old_v: Seq<RaftEntry<C>>, new_v: Seq<RaftEntry<C>>, es: Seq<RaftEntry<C>>,
                                prev: int, f0: int)
    requires
        0 <= prev,
        0 <= f0 <= es.len(),
        prev + f0 <= old_v.len(),
        forall|p: int| prev <= p < prev + f0 ==> #[trigger] old_v[p].spec_term() == es[p - prev].spec_term(),
        f0 < es.len() ==> prev + f0 == old_v.len() || old_v[prev + f0].spec_term() != es[f0].spec_term(),
        new_v == (if f0 < es.len() { old_v.subrange(0, prev + f0) + es.subrange(f0, es.len() as int) } else { old_v }),
    ensures
        new_v.map_values(|e: RaftEntry<C>| entry_view(e))
            == comp_logs(old_v.map_values(|e: RaftEntry<C>| entry_view(e)), prev, es, es.len() as int),
{
    let log0 = old_v.map_values(|e: RaftEntry<C>| entry_view(e));
    assert forall|p: int| prev <= p < prev + f0 implies #[trigger] log0[p].term == es[p - prev].spec_term() as int by {
        assert(old_v[p].spec_term() == es[p - prev].spec_term());
    }
    if f0 < es.len() && prev + f0 < old_v.len() {
        assert(log0[prev + f0].term == old_v[prev + f0].spec_term() as int);
    }
    lemma_comp_logs(log0, prev, es, f0, es.len() as int);
    if f0 < es.len() {
        assert(new_v.map_values(|e: RaftEntry<C>| entry_view(e))
            =~= log0.take(prev + f0) + entries_view(es.subrange(f0, es.len() as int)));
    }
}

// Component i of an AppendEntries carrying entries (BR1/BR2): the batch's
// entry i at prev + i + 1, behind entry i - 1 (the RPC's prev for i = 0);
// the term, the leader and the commit are the RPC's.
pub open spec fn comp_msg<C>(x: AeView<C>, i: int) -> LRaftMessage {
    LRaftMessage::AppendEntries {
        term: x.term,
        leader: x.ldr,
        prev_index: x.prev + i,
        prev_term: if i == 0 { x.prev_term } else { x.es[i - 1].spec_term() as int },
        entry_term: x.es[i].spec_term() as int,
        value: value_view(x.es[i].spec_cmd()),
        entry_change: LConfChange::NoChange,
        has_entry: true,
        leader_commit: x.lc,
        read_ctx: 0,
    }
}

// An AppendEntries without entries (a heartbeat, or a probe at prev): V1
// reads its commit as min(commit, prev).
pub open spec fn empty_msg<C>(x: AeView<C>) -> LRaftMessage {
    LRaftMessage::AppendEntries {
        term: x.term,
        leader: x.ldr,
        prev_index: x.prev,
        prev_term: x.prev_term,
        entry_term: 0,
        value: 0,
        entry_change: LConfChange::NoChange,
        has_entry: false,
        leader_commit: if x.lc <= x.prev { x.lc } else { x.prev },
        read_ctx: 0,
    }
}

// The message that opens the RPC's group.
pub open spec fn first_msg<C>(x: AeView<C>) -> LRaftMessage {
    if x.es.len() == 0 { empty_msg(x) } else { comp_msg(x, 0) }
}

// A FollowerAppendEntries label: the message's own fields, as
// label_compatible binds them.
pub open spec fn fae_label(m: LRaftMessage) -> ActionLabel {
    ActionLabel::FollowerAppendEntries {
        ae_term: m->AppendEntries_term,
        ae_leader: m->AppendEntries_leader,
        ae_prev_index: m->AppendEntries_prev_index,
        ae_prev_term: m->AppendEntries_prev_term,
        ae_entry_term: m->AppendEntries_entry_term,
        ae_value: m->AppendEntries_value,
        ae_change: m->AppendEntries_entry_change,
        ae_has_entry: m->AppendEntries_has_entry,
        ae_leader_commit: m->AppendEntries_leader_commit,
    }
}

pub open spec fn ae_reject_label<C>(x: AeView<C>) -> ActionLabel {
    ActionLabel::RejectAppendEntries {
        ae_term: x.term,
        ae_prev_index: x.prev,
        ae_prev_term: x.prev_term,
        ae_has_entry: x.es.len() > 0,
    }
}

pub open spec fn ae_answer<C>(x: AeView<C>, term: int, success: bool, match_index: int) -> LRaftMessage {
    LRaftMessage::AppendResponse { term, success, match_index, follower: x.me, read_ctx: 0 }
}

// ae_commit_after: the commit raised to the carried commit, capped at the
// payload's end, never lowered.
pub open spec fn raise_commit(c0: int, lc: int, end: int) -> int {
    let t = if lc <= end { lc } else { end };
    if t > c0 { t } else { c0 }
}

// One accepted message's segment: Follower, the log and the commit after
// it, the success answer.
pub open spec fn fae_seg<C>(l: Seq<Entry>, x: AeView<C>, m: LRaftMessage, log: Seq<LLogEntry>,
                            commit: int, match_index: int) -> Seq<Entry> {
    l.push(Entry::Set(LogField::Role, LogValue::VRole(LServerRole::Follower)))
        .push(Entry::Set(LogField::RaftLog, LogValue::VLog(log)))
        .push(Entry::Set(LogField::CommitIndex, LogValue::VInt(commit)))
        .push(Entry::Send(x.ldr, ae_answer(x, x.term, true, match_index)))
        .push(Entry::Close(fae_label(m)))
}

// The commit after i components.
pub open spec fn comp_commit(c0: int, lc: int, prev: int, i: int) -> int {
    if i <= 0 { c0 } else { raise_commit(c0, lc, prev + i) }
}

// Components 0..i-1 after l2, which holds component 0's Recv (and its step
// down); every later component opens a group of its own.
pub open spec fn fae_groups<C>(l2: Seq<Entry>, x: AeView<C>, log0: Seq<LLogEntry>, c0: int, i: int) -> Seq<Entry>
    decreases i,
{
    if i <= 0 {
        l2
    } else {
        let rest = fae_groups(l2, x, log0, c0, i - 1);
        let opened = if i == 1 { rest } else { rest.push(Entry::Recv(x.ldr, comp_msg(x, i - 1))) };
        fae_seg(opened, x, comp_msg(x, i - 1), comp_logs(log0, x.prev, x.es, i),
            comp_commit(c0, x.lc, x.prev, i), x.prev + i)
    }
}

// The segment a message's group continues with is a FollowerAppendEntries
// for message m, from state s to s with Follower, `log` and `commit`.
pub proof fn lemma_fae_seg<C>(l: Seq<Entry>, x: AeView<C>, m: LRaftMessage, log: Seq<LLogEntry>,
                              commit: int, match_index: int, c: LConstants)
    requires
        log_ok(l, c),
        seg_state(l) == replay(l),
        seg_sends(l) == Seq::<LRaftMessage>::empty(),
        seg_trigger(l) == Option::Some(Entry::Recv(x.ldr, m)),
        seg_sends_to(l, x.ldr),
        m is AppendEntries,
        LFollowerAppendEntries(replay(l),
            LState { role: LServerRole::Follower, log, commit_index: commit, ..replay(l) }, c,
            m->AppendEntries_term, m->AppendEntries_leader, m->AppendEntries_prev_index,
            m->AppendEntries_prev_term, m->AppendEntries_entry_term, m->AppendEntries_value,
            m->AppendEntries_entry_change, m->AppendEntries_has_entry, m->AppendEntries_leader_commit,
            seq![ae_answer(x, x.term, true, match_index)]),
    ensures
        log_ok(fae_seg(l, x, m, log, commit, match_index), c),
        fully_closed(fae_seg(l, x, m, log, commit, match_index)),
        replay(fae_seg(l, x, m, log, commit, match_index))
            == (LState { role: LServerRole::Follower, log, commit_index: commit, ..replay(l) }),
        l.is_prefix_of(fae_seg(l, x, m, log, commit, match_index)),
{
    let s = replay(l);
    let a = ae_answer(x, x.term, true, match_index);
    lemma_g_set(l, LogField::Role, LogValue::VRole(LServerRole::Follower), c);
    let l1 = l.push(Entry::Set(LogField::Role, LogValue::VRole(LServerRole::Follower)));
    assert(seg_sends_to(l1, x.ldr));
    lemma_g_set(l1, LogField::RaftLog, LogValue::VLog(log), c);
    let l2 = l1.push(Entry::Set(LogField::RaftLog, LogValue::VLog(log)));
    assert(seg_sends_to(l2, x.ldr));
    lemma_g_set(l2, LogField::CommitIndex, LogValue::VInt(commit), c);
    let l3 = l2.push(Entry::Set(LogField::CommitIndex, LogValue::VInt(commit)));
    assert(seg_sends_to(l3, x.ldr));
    lemma_g_send(l3, x.ldr, a, c);
    let l4 = l3.push(Entry::Send(x.ldr, a));
    assert(seg_sends(l4) =~= seq![a]);
    assert(replay(l4) == LState { role: LServerRole::Follower, log, commit_index: commit, ..s });
    lemma_g_close(l4, fae_label(m), x.ldr, c);
    let l5 = l4.push(Entry::Close(fae_label(m)));
    assert(fae_seg(l, x, m, log, commit, match_index) =~= l5);
    assert(l.is_prefix_of(l5)) by {
        assert(l5.subrange(0, l.len() as int) =~= l);
    }
}

pub proof fn lemma_fae_groups<C>(l2: Seq<Entry>, x: AeView<C>, c: LConstants, log0: Seq<LLogEntry>,
                                 c0: int, f0: int, i: int)
    requires
        log_ok(l2, c),
        seg_state(l2) == replay(l2),
        seg_sends(l2) == Seq::<LRaftMessage>::empty(),
        seg_trigger(l2) == Option::Some(Entry::Recv(x.ldr, comp_msg(x, 0))),
        forall|d: int| seg_sends_to(l2, d),
        x.me == c.my_id,
        x.es.len() >= 1,
        0 <= i <= x.es.len(),
        replay(l2).current_term == x.term,
        replay(l2).log == log0,
        replay(l2).commit_index == c0,
        replay(l2).pending_reads == Seq::<LReadReq>::empty(),
        replay(l2).served_ctxs == Set::<int>::empty(),
        0 <= x.prev <= log0.len(),
        // the RPC's prev entry, as the handler checked it
        prev_log_ok(replay(l2), x.prev, x.prev_term),
        // the first conflict, as the handler's scan found it
        0 <= f0 <= x.es.len(),
        x.prev + f0 <= log0.len(),
        forall|p: int| x.prev <= p < x.prev + f0 ==> #[trigger] log0[p].term == x.es[p - x.prev].spec_term() as int,
        f0 < x.es.len() ==> x.prev + f0 == log0.len() || log0[x.prev + f0].term != x.es[f0].spec_term() as int,
        // a replaced entry is above the commit (the handler refuses otherwise)
        f0 < x.es.len() && x.prev + f0 < log0.len() ==> x.prev + f0 >= c0,
    ensures
        log_ok(fae_groups(l2, x, log0, c0, i), c),
        i >= 1 ==> fully_closed(fae_groups(l2, x, log0, c0, i)),
        i >= 1 ==> replay(fae_groups(l2, x, log0, c0, i)) == (LState {
            role: LServerRole::Follower,
            log: comp_logs(log0, x.prev, x.es, i),
            commit_index: comp_commit(c0, x.lc, x.prev, i),
            ..replay(l2)
        }),
        l2.is_prefix_of(fae_groups(l2, x, log0, c0, i)),
    decreases i,
{
    if i == 0 {
        assert(l2.subrange(0, l2.len() as int) =~= l2);
    } else {
        lemma_fae_groups(l2, x, c, log0, c0, f0, i - 1);
        let k = i - 1;
        let rest = fae_groups(l2, x, log0, c0, k);
        let m = comp_msg(x, k);
        let opened = if i == 1 { rest } else { rest.push(Entry::Recv(x.ldr, m)) };
        if i > 1 {
            lemma_g_open(rest, Entry::Recv(x.ldr, m), c);
        }
        let sk = replay(opened);
        assert(seg_state(opened) == sk);
        assert(seg_sends(opened) == Seq::<LRaftMessage>::empty());
        assert(seg_trigger(opened) == Option::Some(Entry::Recv(x.ldr, m)));
        assert(seg_sends_to(opened, x.ldr));
        // the state at component k
        assert(sk.current_term == x.term);
        assert(sk.log == comp_logs(log0, x.prev, x.es, k)) by {
            if k == 0 { assert(comp_logs(log0, x.prev, x.es, 0) == log0); }
        }
        assert(sk.commit_index == comp_commit(c0, x.lc, x.prev, k));
        // its guards
        if k >= 1 {
            lemma_comp_logs_prev(log0, x.prev, x.es, f0, k);
        }
        assert(prev_log_ok(sk, x.prev + k, m->AppendEntries_prev_term));
        lemma_comp_logs(log0, x.prev, x.es, f0, k);
        if k > f0 {
            lemma_comp_logs_prev(log0, x.prev, x.es, f0, k);
        }
        if k < f0 {
            assert(log0[x.prev + k].term == x.es[k].spec_term() as int);
        }
        assert(truncate_ok(sk, x.prev + k, m->AppendEntries_entry_term, true));
        let log = comp_logs(log0, x.prev, x.es, i);
        let commit = comp_commit(c0, x.lc, x.prev, i);
        assert(log == ae_log_after(sk, x.prev + k, m->AppendEntries_entry_term, m->AppendEntries_value,
            LConfChange::NoChange, true));
        assert(commit == ae_commit_after(sk, x.lc, x.prev + k + 1));
        assert(step_down_if_needed(sk, x.term) == sk);
        assert(LFollowerAppendEntries(sk, LState { role: LServerRole::Follower, log, commit_index: commit, ..sk }, c,
            m->AppendEntries_term, m->AppendEntries_leader, m->AppendEntries_prev_index,
            m->AppendEntries_prev_term, m->AppendEntries_entry_term, m->AppendEntries_value,
            m->AppendEntries_entry_change, m->AppendEntries_has_entry, m->AppendEntries_leader_commit,
            seq![ae_answer(x, x.term, true, x.prev + i)]));
        lemma_fae_seg(opened, x, m, log, commit, x.prev + i, c);
        let li = fae_seg(opened, x, m, log, commit, x.prev + i);
        assert(li == fae_groups(l2, x, log0, c0, i));
        assert(l2.is_prefix_of(li)) by {
            assert(li.subrange(0, l2.len() as int) =~= rest.subrange(0, l2.len() as int));
        }
    }
}

// The RPC's ghost log: its first message's Recv; the StepDown segment when
// it carried a higher term the handler took up; then the refusal (and a
// candidate's StepAside, when the handler left the campaign before
// refusing a committed conflict), or the accepted message (entry-less) or
// components.
pub open spec fn ae_log<C>(l0: Seq<Entry>, x: AeView<C>, stepped: bool, aside: bool, accepted: bool,
                           log0: Seq<LLogEntry>, c0: int, cur: int) -> Seq<Entry> {
    let l1 = l0.push(Entry::Recv(x.ldr, first_msg(x)));
    let l2 = if stepped { step_down_seg(l1, x.term) } else { l1 };
    if !accepted {
        let lr = l2.push(Entry::Send(x.ldr, ae_answer(x, cur, false, 0))).push(Entry::Close(ae_reject_label(x)));
        if aside { step_aside_log(lr) } else { lr }
    } else if x.es.len() == 0 {
        fae_seg(l2, x, empty_msg(x), log0, raise_commit(c0, x.lc, x.prev), x.prev)
    } else {
        fae_groups(l2, x, log0, c0, x.es.len() as int)
    }
}

// The handler's ghost contract: the log is the RPC's group, with or
// without the step down.
pub open spec fn ae_log_ok<C>(l: Seq<Entry>, l0: Seq<Entry>, x: AeView<C>, accepted: bool,
                              log0: Seq<LLogEntry>, c0: int, cur: int) -> bool {
    exists|stepped: bool, aside: bool| l == #[trigger] ae_log(l0, x, stepped, aside, accepted, log0, c0, cur)
}

pub proof fn lemma_append_entries<C>(pre: &RaftCore<C>, post: &RaftCore<C>, x: AeView<C>,
                                     stepped: bool, aside: bool, accepted: bool, f0: int)
    requires
        pre.inv(),
        pre.ginv(),
        pre.gated_,
        x.me == pre.my_rank(),
        x.prev >= 0,
        stepped ==> x.term > pre.current_term_ as int,
        accepted && !stepped ==> x.term == pre.current_term_ as int,
        // what the handler leaves: the term, the vote, the role
        post.current_term_ as int == (if stepped || accepted { x.term } else { pre.current_term_ as int }),
        post.vote_for_ == (if stepped { RAFT_SERVER_INVALID_SITE_ID } else { pre.vote_for_ }),
        stepped || accepted ==> !post.is_leader_ && !post.election_in_progress_,
        !stepped && !accepted && !aside ==> post.role_view() == pre.role_view(),
        aside ==> !stepped && !accepted && pre.role_view() is Candidate && post.role_view() is Follower,
        post.election_term_ == pre.election_term_,
        post.config_members_ == pre.config_members_,
        post.site_id_ == pre.site_id_,
        post.snapterm_ == pre.snapterm_,
        post.g_votes_@ == (if stepped || aside { Set::<int>::empty() } else { pre.g_votes_@ }),
        post.g_match_ == pre.g_match_,
        post.g_next_ == pre.g_next_,
        post.peers_ == pre.peers_,
        post.peer_sites_ == pre.peer_sites_,
        // the log and the commit
        post.log_entries_ok(),
        !accepted ==> post.log_view() == pre.log_view() && post.commit_index_ == pre.commit_index_,
        accepted && x.es.len() == 0 ==> post.log_view() == pre.log_view()
            && post.commit_index_ as int == raise_commit(pre.commit_index_ as int, x.lc, x.prev),
        accepted && x.es.len() > 0 ==> post.log_view() == comp_logs(pre.log_view(), x.prev, x.es, x.es.len() as int)
            && post.commit_index_ as int == comp_commit(pre.commit_index_ as int, x.lc, x.prev, x.es.len() as int),
        // an accepted RPC passed the handler's checks: its prev entry; its
        // first conflict (f0); no committed entry replaced
        accepted ==> x.prev <= pre.log_view().len() && prev_log_ok(pre.state_view(), x.prev, x.prev_term),
        accepted && x.es.len() > 0 ==> {
            &&& 0 <= f0 <= x.es.len()
            &&& x.prev + f0 <= pre.log_view().len()
            &&& (forall|p: int| x.prev <= p < x.prev + f0
                    ==> #[trigger] pre.log_view()[p].term == x.es[p - x.prev].spec_term() as int)
            &&& (f0 < x.es.len() ==> x.prev + f0 == pre.log_view().len()
                    || pre.log_view()[x.prev + f0].term != x.es[f0].spec_term() as int)
            &&& (f0 < x.es.len() && x.prev + f0 < pre.log_view().len()
                    ==> x.prev + f0 >= pre.commit_index_ as int)
        },
        post.g_log_@ == ae_log(pre.g_log_@, x, stepped, aside, accepted, pre.log_view(),
            pre.commit_index_ as int, post.current_term_ as int),
    ensures post.ginv(),
{
    let c = pre.c_view();
    let l0 = pre.g_log_@;
    let s0 = replay(l0);
    let m0 = first_msg(x);
    lemma_my_rank(pre);
    lemma_g_open(l0, Entry::Recv(x.ldr, m0), c);
    let l1 = l0.push(Entry::Recv(x.ldr, m0));
    let l2 = if stepped { step_down_seg(l1, x.term) } else { l1 };
    let s2 = if stepped { stepped_down(s0, x.term) } else { s0 };
    if stepped {
        lemma_step_down_seg(l1, x.term, c);
    }
    assert(log_ok(l2, c));
    assert(replay(l2) == s2);
    assert(seg_state(l2) == s2);
    assert(seg_sends(l2) == Seq::<LRaftMessage>::empty());
    assert(seg_trigger(l2) == Option::Some(Entry::Recv(x.ldr, m0)));
    assert(forall|d: int| seg_sends_to(l2, d));
    assert(seg_sends_to(l2, x.ldr));
    assert(post.c_view() == c);
    if !accepted {
        let a = ae_answer(x, post.current_term_ as int, false, 0);
        lemma_g_send(l2, x.ldr, a, c);
        let l3 = l2.push(Entry::Send(x.ldr, a));
        assert(seg_sends(l3) =~= seq![a]);
        assert(LRejectAppendEntries(s2, replay(l3), c, x.term, x.prev, x.prev_term, x.es.len() > 0, seg_sends(l3)));
        lemma_g_close(l3, ae_reject_label(x), x.ldr, c);
        let lr = l3.push(Entry::Close(ae_reject_label(x)));
        if aside {
            lemma_step_aside(lr, c);
            assert(post.g_log_@ =~= step_aside_log(lr));
            assert(post.state_view() == LState {
                role: LServerRole::Follower,
                votes_granted: Set::<int>::empty(),
                ..s2
            });
        } else {
            assert(post.g_log_@ =~= lr);
            assert(post.state_view() == s2);
        }
    } else if x.es.len() == 0 {
        let commit = raise_commit(pre.commit_index_ as int, x.lc, x.prev);
        let m = empty_msg(x);
        assert(step_down_if_needed(s2, x.term) == s2);
        assert(LFollowerAppendEntries(s2, LState { role: LServerRole::Follower, log: s2.log, commit_index: commit, ..s2 }, c,
            m->AppendEntries_term, m->AppendEntries_leader, m->AppendEntries_prev_index,
            m->AppendEntries_prev_term, m->AppendEntries_entry_term, m->AppendEntries_value,
            m->AppendEntries_entry_change, m->AppendEntries_has_entry, m->AppendEntries_leader_commit,
            seq![ae_answer(x, x.term, true, x.prev)]));
        lemma_fae_seg(l2, x, m, s2.log, commit, x.prev, c);
        assert(post.state_view() == LState { role: LServerRole::Follower, log: s2.log, commit_index: commit, ..s2 });
    } else {
        lemma_fae_groups(l2, x, c, pre.log_view(), pre.commit_index_ as int, f0, x.es.len() as int);
        assert(post.state_view() == LState {
            role: LServerRole::Follower,
            log: comp_logs(pre.log_view(), x.prev, x.es, x.es.len() as int),
            commit_index: comp_commit(pre.commit_index_ as int, x.lc, x.prev, x.es.len() as int),
            ..s2
        });
    }
}

// ===========================================================================
// A reply to the leader's AppendEntries (heartbeat_on_reply): StepDown on a
// higher term; HandleAppendResponse when a success reports a match past
// the spec's; anything else unseen by the spec (V2 holds: the exec raises
// a match by max(), to at most the reported index)
// ===========================================================================

// The reply as the spec sees it (coupling-table §2): from the follower its
// slot was sent to, the match a success reports (0 for a refusal).
pub open spec fn reply_msg(cfg: Seq<u16>, follower: u16, status: bool, term: u64, last: u64) -> LRaftMessage {
    LRaftMessage::AppendResponse {
        term: term as int,
        success: status,
        match_index: if status { last as int } else { 0 },
        follower: rank(cfg, follower),
        read_ctx: 0,
    }
}

pub open spec fn har_label(f: int, term: int, nmi: int) -> ActionLabel {
    ActionLabel::HandleAppendResponse {
        resp_term: term,
        resp_success: true,
        resp_match_index: nmi,
        resp_follower: f,
        follower: f as u64,
        new_match_index: nmi as u64,
    }
}

pub open spec fn har_log(l: Seq<Entry>, f: int, term: int, nmi: int, gm: Map<u64, u64>,
                         gn: Map<u64, u64>) -> Seq<Entry> {
    l.push(Entry::Recv(f, LRaftMessage::AppendResponse { term, success: true, match_index: nmi, follower: f, read_ctx: 0 }))
        .push(Entry::Set(LogField::MatchIndex, LogValue::VU64Map(gm.insert(f as u64, nmi as u64))))
        .push(Entry::Set(LogField::NextIndex, LogValue::VU64Map(gn.insert(f as u64, u64_inc(nmi as u64)))))
        .push(Entry::Close(har_label(f, term, nmi)))
}

// The reply's ghost log: unchanged, its Recv and a StepDown, or a
// HandleAppendResponse.
pub open spec fn reply_log_ok(l: Seq<Entry>, l0: Seq<Entry>, f: int, m: LRaftMessage,
                              gm: Map<u64, u64>, gn: Map<u64, u64>) -> bool {
    ||| l == l0
    ||| l == step_down_seg(l0.push(Entry::Recv(f, m)), msg_term(m))
    ||| (m is AppendResponse && m->AppendResponse_success && m->AppendResponse_follower == f
         && l == har_log(l0, f, m->AppendResponse_term, m->AppendResponse_match_index, gm, gn))
}

// Two peers are two members, so two ranks.
pub proof fn lemma_peer_ranks_differ<C>(core: &RaftCore<C>, o1: int, o2: int)
    requires
        core.inv(),
        0 <= o1 < core.peers_.spec_len(),
        0 <= o2 < core.peers_.spec_len(),
        o1 != o2,
    ensures
        rank(core.config_members_@, core.peer_sites_@[o1]) != rank(core.config_members_@, core.peer_sites_@[o2]),
{
    let cfg = core.config_members_@;
    let (a, b) = (core.peer_sites_@[o1], core.peer_sites_@[o2]);
    if o1 < o2 { assert(a < b); } else { assert(b < a); }
    lemma_rank_bounds(cfg, a);
    lemma_rank_bounds(cfg, b);
}

// A message of a higher term, received: its Recv, then StepDown.
pub proof fn lemma_recv_step_down<C>(pre: &RaftCore<C>, post: &RaftCore<C>, src: int, m: LRaftMessage)
    requires
        pre.ginv(),
        msg_term(m) > pre.current_term_ as int,
        post.current_term_ as int == msg_term(m),
        post.vote_for_ == RAFT_SERVER_INVALID_SITE_ID,
        !post.is_leader_,
        !post.election_in_progress_,
        post.raft_log_ == pre.raft_log_,
        post.commit_index_ == pre.commit_index_,
        post.config_members_ == pre.config_members_,
        post.site_id_ == pre.site_id_,
        post.snapterm_ == pre.snapterm_,
        post.g_votes_@ == Set::<int>::empty(),
        post.g_match_ == pre.g_match_,
        post.g_next_ == pre.g_next_,
        post.g_log_@ == step_down_seg(pre.g_log_@.push(Entry::Recv(src, m)), msg_term(m)),
    ensures post.ginv(),
{
    let c = pre.c_view();
    let l0 = pre.g_log_@;
    lemma_g_open(l0, Entry::Recv(src, m), c);
    lemma_step_down_seg(l0.push(Entry::Recv(src, m)), msg_term(m), c);
    assert(post.c_view() == c);
    assert(post.log_view() == pre.log_view());
    assert(post.state_view() == stepped_down(replay(l0), msg_term(m)));
}

// A success reporting a match past the spec's: HandleAppendResponse.
pub proof fn lemma_har_ginv<C>(pre: &RaftCore<C>, post: &RaftCore<C>, o: int, term: u64, reported: u64)
    requires
        pre.inv(),
        pre.ginv(),
        pre.gated_,
        pre.is_leader_,
        0 <= o < pre.peers_.spec_len(),
        term == pre.current_term_,
        reported as int <= pre.raft_log_.spec_last_index(),
        reported as int > match_of(pre.g_match_@, rank(pre.config_members_@, pre.peer_sites_@[o])),
        // the reply's write: the follower's match, raised by max() to at
        // most the reported index; the rest as they were
        post.current_term_ == pre.current_term_,
        post.vote_for_ == pre.vote_for_,
        post.is_leader_ == pre.is_leader_,
        post.election_in_progress_ == pre.election_in_progress_,
        post.election_term_ == pre.election_term_,
        post.raft_log_ == pre.raft_log_,
        post.commit_index_ == pre.commit_index_,
        post.config_members_ == pre.config_members_,
        post.site_id_ == pre.site_id_,
        post.snapterm_ == pre.snapterm_,
        post.peer_sites_ == pre.peer_sites_,
        post.g_votes_ == pre.g_votes_,
        post.peers_.spec_len() == pre.peers_.spec_len(),
        post.peers_.spec_match(o) == pre.peers_.spec_match(o) || post.peers_.spec_match(o) <= reported,
        forall|o2: int| 0 <= o2 < pre.peers_.spec_len() && o2 != o
            ==> #[trigger] post.peers_.spec_match(o2) == pre.peers_.spec_match(o2),
        post.g_match_@ == pre.g_match_@.insert(rank(pre.config_members_@, pre.peer_sites_@[o]) as u64, reported),
        post.g_next_@ == pre.g_next_@.insert(rank(pre.config_members_@, pre.peer_sites_@[o]) as u64, u64_inc(reported)),
        post.g_log_@ == har_log(pre.g_log_@, rank(pre.config_members_@, pre.peer_sites_@[o]), term as int,
            reported as int, pre.g_match_@, pre.g_next_@),
    ensures post.ginv(),
{
    let c = pre.c_view();
    let cfg = pre.config_members_@;
    let f = rank(cfg, pre.peer_sites_@[o]);
    let l0 = pre.g_log_@;
    let s0 = replay(l0);
    let m = LRaftMessage::AppendResponse { term: term as int, success: true, match_index: reported as int, follower: f, read_ctx: 0 };
    lemma_rank_bounds(cfg, pre.peer_sites_@[o]);
    pre.raft_log_.lemma_wf_bounds();
    lemma_g_open(l0, Entry::Recv(f, m), c);
    let l1 = l0.push(Entry::Recv(f, m));
    let mv = LogValue::VU64Map(pre.g_match_@.insert(f as u64, reported));
    let nv = LogValue::VU64Map(pre.g_next_@.insert(f as u64, u64_inc(reported)));
    lemma_g_set(l1, LogField::MatchIndex, mv, c);
    let l2 = l1.push(Entry::Set(LogField::MatchIndex, mv));
    lemma_g_set(l2, LogField::NextIndex, nv, c);
    let l3 = l2.push(Entry::Set(LogField::NextIndex, nv));
    let s3 = replay(l3);
    assert(pre.role_view() is Leader);
    assert(LHandleAppendResponse(s0, s3, c, term as int, true, reported as int, f, f as u64, reported, seg_sends(l3)));
    // the label binds the follower and the match to the reply's
    lemma_sorted_len(cfg);
    assert(0 <= f < cfg.len());
    assert((f as u64) as int == f);
    assert(((reported as int) as u64) == reported);
    assert(term as int == s0.current_term);
    assert(action_holds(har_label(f, term as int, reported as int), seg_state(l3), replay(l3), c, seg_sends(l3)));
    lemma_g_close(l3, har_label(f, term as int, reported as int), 0, c);
    assert(post.g_log_@ =~= l3.push(Entry::Close(har_label(f, term as int, reported as int))));
    // the coupling
    assert(post.c_view() == c);
    assert(post.log_view() == pre.log_view());
    assert(post.state_view() == s3);
    // V2: the follower's match is at most what the spec now holds; every
    // other peer's rank, and its spec match, is untouched
    assert forall|o2: int| 0 <= o2 < post.peers_.spec_len()
        implies (#[trigger] post.peers_.spec_match(o2)) as int
            <= match_of(post.g_match_@, rank(cfg, post.peer_sites_@[o2])) by {
        if o2 != o {
            lemma_peer_ranks_differ(pre, o2, o);
            assert(pre.peers_.spec_match(o2) as int <= match_of(pre.g_match_@, rank(cfg, pre.peer_sites_@[o2])));
        } else {
            assert(pre.peers_.spec_match(o) as int <= match_of(pre.g_match_@, f));
        }
    }
}

// A reply the spec does not see: V2 still holds.
pub proof fn lemma_reply_stutter<C>(pre: &RaftCore<C>, post: &RaftCore<C>, accepted: bool, o: int, reported: u64)
    requires
        pre.inv(),
        pre.ginv(),
        post.current_term_ == pre.current_term_,
        post.vote_for_ == pre.vote_for_,
        post.is_leader_ == pre.is_leader_,
        post.election_in_progress_ == pre.election_in_progress_,
        post.election_term_ == pre.election_term_,
        post.raft_log_ == pre.raft_log_,
        post.commit_index_ == pre.commit_index_,
        post.config_members_ == pre.config_members_,
        post.site_id_ == pre.site_id_,
        post.snapterm_ == pre.snapterm_,
        post.peer_sites_ == pre.peer_sites_,
        post.g_log_ == pre.g_log_,
        post.g_votes_ == pre.g_votes_,
        post.g_match_ == pre.g_match_,
        post.g_next_ == pre.g_next_,
        post.peers_.spec_len() == pre.peers_.spec_len(),
        forall|o2: int| 0 <= o2 < pre.peers_.spec_len() && (!accepted || o2 != o)
            ==> #[trigger] post.peers_.spec_match(o2) == pre.peers_.spec_match(o2),
        // an accepted reply raised one match to at most its report, which
        // the spec already holds
        accepted ==> {
            &&& 0 <= o < pre.peers_.spec_len()
            &&& (post.peers_.spec_match(o) == pre.peers_.spec_match(o) || post.peers_.spec_match(o) <= reported)
            &&& reported as int <= match_of(pre.g_match_@, rank(pre.config_members_@, pre.peer_sites_@[o]))
        },
    ensures post.ginv(),
{
    assert(post.c_view() == pre.c_view());
    assert(post.log_view() == pre.log_view());
    assert(post.state_view() == pre.state_view());
    let cfg = pre.config_members_@;
    if post.is_leader_ {
        assert forall|o2: int| 0 <= o2 < post.peers_.spec_len()
            implies (#[trigger] post.peers_.spec_match(o2)) as int
                <= match_of(post.g_match_@, rank(cfg, post.peer_sites_@[o2])) by {
            assert(pre.peers_.spec_match(o2) as int <= match_of(pre.g_match_@, rank(cfg, pre.peer_sites_@[o2])));
        }
    }
}

// ===========================================================================
// The leader's commit advance (raft_commit_advance; PHASE 0, and PHASE 3
// under bugs-found B17's premise): LAdvanceCommitIndex, spontaneous
// ===========================================================================

// The ordinals in [0, k) whose match reaches v.
pub open spec fn ge_ords(s: Seq<u64>, v: u64, k: int) -> Set<int>
    decreases k,
{
    if k <= 0 {
        Set::<int>::empty()
    } else if s[k - 1] >= v {
        ge_ords(s, v, k - 1).insert(k - 1)
    } else {
        ge_ords(s, v, k - 1)
    }
}

pub proof fn lemma_ge_ords(s: Seq<u64>, v: u64, k: int)
    requires 0 <= k <= s.len(),
    ensures
        ge_ords(s, v, k).len() == count_ge(s, v, k),
        forall|o: int| #[trigger] ge_ords(s, v, k).contains(o) <==> (0 <= o < k && s[o] >= v),
    decreases k,
{
    if k > 0 {
        lemma_ge_ords(s, v, k - 1);
    }
}

// The exec's commit quorum is the spec's: the followers whose match reaches
// idx (count_ge of them), at their ranks, with this server, are a majority
// of the configuration (V2: the spec's match is at least the exec's).
pub proof fn lemma_commit_quorum<C>(core: &RaftCore<C>, idx: u64)
    requires
        core.inv(),
        core.ginv(),
        core.gated_,
        core.is_leader_,
        idx >= 1,
        count_ge(core.peers_.spec_matches(), idx, core.peers_.spec_len())
            >= core.peers_.spec_len() - (core.config_members_@.len() - 1) / 2,
    ensures commit_quorum_ok(core.state_view(), core.c_view(), idx as int),
{
    let cfg = core.config_members_@;
    let n = core.peers_.spec_len();
    let nn = cfg.len() as int;
    let s = core.state_view();
    let c = core.c_view();
    let ms = core.peers_.spec_matches();
    core.peers_.lemma_matches();
    lemma_sorted_len(cfg);
    lemma_my_rank(core);
    assert(n == nn - 1);
    lemma_ge_ords(ms, idx, n);
    let os = ge_ords(ms, idx, n);
    let f = |o: int| rank(cfg, core.peer_sites_@[o]);
    let rs = os.map(f);
    broadcast use Set::lemma_map_contains;
    assert(os.injective_on(f)) by {
        assert forall|a: int, b: int| os.contains(a) && os.contains(b) && #[trigger] f(a) == #[trigger] f(b)
            implies a == b by {
            if a != b {
                lemma_peer_ranks_differ(core, a, b);
            }
        }
    }
    vstd::set_lib::lemma_map_size(os, rs, f);
    let me = core.my_rank();
    assert(!rs.contains(me)) by {
        if rs.contains(me) {
            let o = choose|o: int| os.contains(o) && me == f(o);
            lemma_rank_bounds(cfg, core.peer_sites_@[o]);
            lemma_rank_bounds(cfg, core.site_id_);
        }
    }
    let q = rs.insert(me);
    let pred = |v: int| v == c.my_id
        || (s.match_index.contains_key(v as u64) && s.match_index[v as u64] as int >= idx as int);
    let rep = s.config.filter(pred);
    broadcast use vstd::set::lemma_set_filter;
    assert(q.subset_of(rep)) by {
        assert forall|r: int| q.contains(r) implies rep.contains(r) by {
            if r != me {
                let o = choose|o: int| os.contains(o) && r == f(o);
                lemma_rank_bounds(cfg, core.peer_sites_@[o]);
                assert(ms[o] == core.peers_.spec_match(o));
                assert(core.peers_.spec_match(o) as int <= match_of(core.g_match_@, r));
                assert((r as u64) as int == r);
            }
        }
    }
    vstd::set_lib::lemma_len_subset(q, rep);
    vstd::set_lib::lemma_int_range(0, nn);
    assert(q.len() == count_ge(ms, idx, n) + 1);
    assert(replicator_count(s, c, idx as int) == rep.len());
}

pub open spec fn advance_log(l: Seq<Entry>, idx: int) -> Seq<Entry> {
    l.push(Entry::Tick)
        .push(Entry::Set(LogField::CommitIndex, LogValue::VInt(idx)))
        .push(Entry::Close(ActionLabel::AdvanceCommitIndex { new_commit_index: idx }))
}

pub proof fn lemma_advance_ginv<C>(pre: &RaftCore<C>, post: &RaftCore<C>, idx: u64)
    requires
        pre.inv(),
        pre.ginv(),
        pre.gated_,
        pre.is_leader_,
        idx > pre.commit_index_,
        idx as int <= pre.raft_log_.spec_last_index(),
        pre.raft_log_.view()[idx - 1].spec_term() as u64 == pre.current_term_,
        count_ge(pre.peers_.spec_matches(), idx, pre.peers_.spec_len())
            >= pre.peers_.spec_len() - (pre.config_members_@.len() - 1) / 2,
        // only the commit index moves
        post.commit_index_ == idx,
        post.current_term_ == pre.current_term_,
        post.vote_for_ == pre.vote_for_,
        post.is_leader_ == pre.is_leader_,
        post.election_in_progress_ == pre.election_in_progress_,
        post.election_term_ == pre.election_term_,
        post.raft_log_ == pre.raft_log_,
        post.config_members_ == pre.config_members_,
        post.site_id_ == pre.site_id_,
        post.snapterm_ == pre.snapterm_,
        post.peers_ == pre.peers_,
        post.peer_sites_ == pre.peer_sites_,
        post.g_votes_ == pre.g_votes_,
        post.g_match_ == pre.g_match_,
        post.g_next_ == pre.g_next_,
        post.g_log_@ == advance_log(pre.g_log_@, idx as int),
    ensures post.ginv(),
{
    let c = pre.c_view();
    let l0 = pre.g_log_@;
    let s0 = replay(l0);
    pre.raft_log_.lemma_wf_bounds();
    lemma_commit_quorum(pre, idx);
    // the entry is of the current term (terms are non-negative)
    assert(pre.raft_log_.view()[idx - 1].spec_term() >= 0);
    assert(s0.log[idx - 1] == entry_view(pre.raft_log_.view()[idx - 1]));
    lemma_g_open(l0, Entry::Tick, c);
    let l1 = l0.push(Entry::Tick);
    lemma_g_set(l1, LogField::CommitIndex, LogValue::VInt(idx as int), c);
    let l2 = l1.push(Entry::Set(LogField::CommitIndex, LogValue::VInt(idx as int)));
    assert(LAdvanceCommitIndex(s0, replay(l2), c, idx as int, seg_sends(l2)));
    lemma_g_close(l2, ActionLabel::AdvanceCommitIndex { new_commit_index: idx as int }, 0, c);
    assert(post.g_log_@ =~= l2.push(Entry::Close(ActionLabel::AdvanceCommitIndex { new_commit_index: idx as int })));
    assert(post.c_view() == c);
    assert(post.log_view() == pre.log_view());
    assert(post.state_view() == replay(l2));
}

// ===========================================================================
// The leader's AppendEntries (heartbeat_tick, PHASE 1): one
// LSendAppendEntries segment per component (BR1), each a Tick group, built
// from the leader's log, its term, the RPC's prev and the commit read in the
// same call (F3)
// ===========================================================================

// Component i of the leader's AppendEntries, as the spec sees it: the log's
// entry prev + i + 1 behind its predecessor (the RPC's prev for i = 0); or,
// without entries, prev and its term, with V1's commit min(commit, prev).
// The follower's comp_msg / empty_msg are this message (the host contract:
// the wire carries the leader's entries).
pub open spec fn send_msg(log: Seq<LLogEntry>, me: int, term: int, prev: int, prev_term: int, lc: int,
                          has_entry: bool, i: int) -> LRaftMessage {
    if has_entry {
        LRaftMessage::AppendEntries {
            term,
            leader: me,
            prev_index: prev + i,
            prev_term: if i == 0 { prev_term } else { log[prev + i - 1].term },
            entry_term: log[prev + i].term,
            value: log[prev + i].value,
            entry_change: LConfChange::NoChange,
            has_entry: true,
            leader_commit: lc,
            read_ctx: 0,
        }
    } else {
        LRaftMessage::AppendEntries {
            term,
            leader: me,
            prev_index: prev,
            prev_term,
            entry_term: 0,
            value: 0,
            entry_change: LConfChange::NoChange,
            has_entry: false,
            leader_commit: if lc <= prev { lc } else { prev },
            read_ctx: 0,
        }
    }
}

pub open spec fn sae_label(f: int, m: LRaftMessage) -> ActionLabel {
    ActionLabel::SendAppendEntries {
        follower: f,
        entry_term: m->AppendEntries_entry_term,
        entry_value: m->AppendEntries_value,
        entry_change: m->AppendEntries_entry_change,
        prev_log_index: m->AppendEntries_prev_index,
        prev_log_term: m->AppendEntries_prev_term,
        has_entry: m->AppendEntries_has_entry,
        leader_commit: m->AppendEntries_leader_commit,
        read_ctx: 0,
    }
}

// Components 0..k-1 of one AppendEntries to follower f, each a Tick group.
pub open spec fn send_segs(l: Seq<Entry>, log: Seq<LLogEntry>, me: int, f: int, term: int, prev: int,
                           prev_term: int, lc: int, has_entry: bool, k: int) -> Seq<Entry>
    decreases k,
{
    if k <= 0 {
        l
    } else {
        let m = send_msg(log, me, term, prev, prev_term, lc, has_entry, k - 1);
        send_segs(l, log, me, f, term, prev, prev_term, lc, has_entry, k - 1)
            .push(Entry::Tick).push(Entry::Send(f, m)).push(Entry::Close(sae_label(f, m)))
    }
}

pub proof fn lemma_send_segs(l: Seq<Entry>, c: LConstants, log: Seq<LLogEntry>, me: int, f: int, term: int,
                             prev: int, prev_term: int, lc: int, has_entry: bool, k: int)
    requires
        log_ok(l, c),
        fully_closed(l),
        replay(l).role is Leader,
        replay(l).log == log,
        replay(l).current_term == term,
        c.my_id == me,
        c.servers.contains(f),
        0 <= lc <= replay(l).commit_index,
        has_entry ==> lc == replay(l).commit_index,
        prev >= 0,
        prev > 0 ==> prev <= log.len() && log[prev - 1].term == prev_term,
        has_entry ==> prev + k <= log.len(),
        !has_entry ==> k <= 1 && prev <= log.len(),
        forall|j: int| 0 <= j < log.len() ==> (#[trigger] log[j]).change == LConfChange::NoChange,
        0 <= k,
    ensures
        log_ok(send_segs(l, log, me, f, term, prev, prev_term, lc, has_entry, k), c),
        fully_closed(send_segs(l, log, me, f, term, prev, prev_term, lc, has_entry, k)),
        replay(send_segs(l, log, me, f, term, prev, prev_term, lc, has_entry, k)) == replay(l),
        l.is_prefix_of(send_segs(l, log, me, f, term, prev, prev_term, lc, has_entry, k)),
    decreases k,
{
    if k == 0 {
        assert(l.subrange(0, l.len() as int) =~= l);
    } else {
        lemma_send_segs(l, c, log, me, f, term, prev, prev_term, lc, has_entry, k - 1);
        let rest = send_segs(l, log, me, f, term, prev, prev_term, lc, has_entry, k - 1);
        let s = replay(rest);
        let i = k - 1;
        let m = send_msg(log, me, term, prev, prev_term, lc, has_entry, i);
        lemma_g_open(rest, Entry::Tick, c);
        let l1 = rest.push(Entry::Tick);
        assert(seg_sends_to(l1, f));
        lemma_g_send(l1, f, m, c);
        let l2 = l1.push(Entry::Send(f, m));
        assert(seg_sends(l2) =~= seq![m]);
        if has_entry {
            assert(log[prev + i].change == LConfChange::NoChange);
        }
        assert(LSendAppendEntries(s, replay(l2), c, f, m->AppendEntries_entry_term, m->AppendEntries_value,
            m->AppendEntries_entry_change, m->AppendEntries_prev_index, m->AppendEntries_prev_term,
            m->AppendEntries_has_entry, m->AppendEntries_leader_commit, 0, seg_sends(l2)));
        lemma_g_close(l2, sae_label(f, m), f, c);
        let l3 = l2.push(Entry::Close(sae_label(f, m)));
        assert(l3 == send_segs(l, log, me, f, term, prev, prev_term, lc, has_entry, k));
        assert(l.is_prefix_of(l3)) by {
            assert(l3.subrange(0, l.len() as int) =~= rest.subrange(0, l.len() as int));
        }
    }
}

// How many components a send carries: one for a heartbeat, else its
// entries.
pub open spec fn send_count<C>(s: AppendSend<C>) -> int {
    if s.payload_ == AppendPayload::HEARTBEAT { 1 } else { s.sent_end_index_ as int - s.prev_log_index_ as int }
}

// A tick's sends, in order, each its components' segments.
pub open spec fn tick_sends_log<C>(l: Seq<Entry>, sends: Seq<AppendSend<C>>, log: Seq<LLogEntry>,
                                   cfg: Seq<u16>, me: int, k: int) -> Seq<Entry>
    decreases k,
{
    if k <= 0 {
        l
    } else {
        let s = sends[k - 1];
        send_segs(tick_sends_log(l, sends, log, cfg, me, k - 1), log, me, rank(cfg, s.site_id_),
            s.term_ as int, s.prev_log_index_ as int, s.prev_log_term_ as int, s.commit_index_ as int,
            s.payload_ != AppendPayload::HEARTBEAT, send_count(s))
    }
}

pub proof fn lemma_tick_sends_push<C>(l: Seq<Entry>, sends: Seq<AppendSend<C>>, s: AppendSend<C>,
                                      log: Seq<LLogEntry>, cfg: Seq<u16>, me: int, k: int)
    requires 0 <= k <= sends.len(),
    ensures tick_sends_log(l, sends.push(s), log, cfg, me, k) == tick_sends_log(l, sends, log, cfg, me, k),
    decreases k,
{
    if k > 0 {
        lemma_tick_sends_push(l, sends, s, log, cfg, me, k - 1);
        assert(sends.push(s)[k - 1] == sends[k - 1]);
    }
}

// The fields the spec state and ginv read, the ghost log aside, unchanged:
// PHASE 1 moves only the next shadow, the slots and the ledger.
pub open spec fn spec_fields_same<C>(a: &RaftCore<C>, b: &RaftCore<C>) -> bool {
    &&& a.current_term_ == b.current_term_
    &&& a.vote_for_ == b.vote_for_
    &&& a.is_leader_ == b.is_leader_
    &&& a.election_in_progress_ == b.election_in_progress_
    &&& a.election_term_ == b.election_term_
    &&& a.raft_log_ == b.raft_log_
    &&& a.commit_index_ == b.commit_index_
    &&& a.config_members_ == b.config_members_
    &&& a.site_id_ == b.site_id_
    &&& a.snapterm_ == b.snapterm_
    &&& a.peer_sites_ == b.peer_sites_
    &&& a.gated_ == b.gated_
    &&& a.g_votes_ == b.g_votes_
    &&& a.g_match_ == b.g_match_
    &&& a.g_next_ == b.g_next_
    &&& a.peers_.spec_len() == b.peers_.spec_len()
    &&& forall|o: int| 0 <= o < a.peers_.spec_len() ==> #[trigger] a.peers_.spec_match(o) == b.peers_.spec_match(o)
}

// A core with the same spec-visible fields and a ghost log that stays a
// certificate of the same state keeps ginv.
pub proof fn lemma_same_fields_ginv<C>(b: &RaftCore<C>, a: &RaftCore<C>)
    requires
        b.ginv(),
        spec_fields_same(a, b),
        log_ok(a.g_log_@, b.c_view()),
        fully_closed(a.g_log_@),
        replay(a.g_log_@) == replay(b.g_log_@),
    ensures a.ginv(),
{
    assert(a.c_view() == b.c_view());
    assert(a.log_view() == b.log_view());
    assert(a.state_view() == b.state_view());
    if a.is_leader_ {
        let cfg = a.config_members_@;
        assert forall|o: int| 0 <= o < a.peers_.spec_len()
            implies (#[trigger] a.peers_.spec_match(o)) as int
                <= match_of(a.g_match_@, rank(cfg, a.peer_sites_@[o])) by {
            assert(b.peers_.spec_match(o) as int <= match_of(b.g_match_@, rank(cfg, b.peer_sites_@[o])));
        }
    }
}

// ===========================================================================
// The core's one entry point (step, step_checked): what each event needs of
// the host for the coupling, and the per-node certificate
// ===========================================================================

impl<C> RaftCore<C> {
    // Beyond admits: the gate for the protocol's events; Setup's order on a
    // fresh core; a proposal while leading, with a value (bugs-found B16);
    // a reply's role read under mtx_ and F9's admission for messages; a
    // round end while leading (bugs-found B17).
    pub open spec fn coupled<W: InboundBatch<C>>(&self, ev: &Event<'_, C, W>) -> bool {
        match *ev {
            Event::SetIdentity { .. } => self.g_log_@.len() == 0,
            Event::Configure { members } => {
                &&& self.g_log_@.len() == 0
                &&& !members@.contains(RAFT_SERVER_INVALID_SITE_ID)
            },
            Event::Propose { has_value, .. } => self.is_leader_ && has_value,
            Event::StartElection { .. } => self.gated_,
            Event::SettleElection { term, voters, granted, reply_terms, n_total, failover, .. } => {
                &&& self.gated_
                &&& failover
                &&& settle_inputs_ok(self, voters@, granted@, reply_terms@, term, n_total)
            },
            Event::RecvRequestVote { can_id, .. } => {
                &&& self.gated_
                &&& self.config_members_@.contains(can_id)
                &&& can_id != self.site_id_
            },
            Event::RecvAppendEntries { leader_site_id, .. } => self.gated_ && leader_site_id != self.site_id_,
            Event::TickHeartbeat { .. } => self.gated_,
            Event::RecvAppendReply { status, last_log_index, is_leader, .. } => {
                &&& self.gated_
                &&& (is_leader ==> self.is_leader_)
                &&& (status ==> last_log_index as int <= self.raft_log_.spec_last_index())
            },
            Event::RoundEnd { .. } => self.gated_,
            // [fix, F20] uncoupled: ObserveTerm's term comes in no modeled
            // message
            Event::ObserveTerm { .. } => false,
            // [fix, F22] (disk design §6) a restart (restore_premise)
            Event::Restore { term, vote, commit, entries_rev } =>
                restore_premise(self, term, vote, commit, entries_rev@),
            _ => true,
        }
    }

    // step_checked's: its admission (F9) supplies the messages' sender
    // facts and a reply's bound on the leader's log.
    pub open spec fn coupled_checked<W: InboundBatch<C>>(&self, ev: &Event<'_, C, W>) -> bool {
        match *ev {
            Event::RecvRequestVote { .. } => self.gated_,
            Event::RecvAppendEntries { .. } => self.gated_,
            Event::RecvAppendReply { is_leader, .. } => self.gated_ && (is_leader ==> self.is_leader_),
            _ => self.coupled(ev),
        }
    }
}

// RebuildPeerTables: the peer table rebuilt at match 0, nothing the spec
// sees moved.
pub proof fn lemma_rebuild_ginv<C>(pre: &RaftCore<C>, post: &RaftCore<C>)
    requires
        pre.ginv(),
        post.current_term_ == pre.current_term_,
        post.vote_for_ == pre.vote_for_,
        post.is_leader_ == pre.is_leader_,
        post.election_in_progress_ == pre.election_in_progress_,
        post.election_term_ == pre.election_term_,
        post.raft_log_ == pre.raft_log_,
        post.commit_index_ == pre.commit_index_,
        post.config_members_ == pre.config_members_,
        post.site_id_ == pre.site_id_,
        post.snapterm_ == pre.snapterm_,
        post.g_log_ == pre.g_log_,
        post.g_votes_ == pre.g_votes_,
        post.g_match_ == pre.g_match_,
        post.g_next_ == pre.g_next_,
        forall|o: int| 0 <= o < post.peers_.spec_len() ==> #[trigger] post.peers_.spec_match(o) == 0,
    ensures post.ginv(),
{
    assert(post.c_view() == pre.c_view());
    assert(post.log_view() == pre.log_view());
    assert(post.state_view() == pre.state_view());
}

// The per-node certificate: a core that keeps ginv holds a ghost log that
// is the group's node_cert, for its cluster (n members, every one a voter)
// and its rank.
pub proof fn lemma_node_cert<C>(core: &RaftCore<C>)
    requires core.ginv(),
    ensures
        glr::protocol::Raft::ghost_log_compose::node_cert(core.g_log_@, core.n_view(),
            Set::<int>::range(0, core.n_view()), core.my_rank()),
{
}

// The cluster theorem, instantiated (plan Phase 8's goal): n servers, each
// a core that keeps ginv, in an n-member configuration, at its own rank.
// Under any schedule of their ghost logs' segments that is causal -- the
// host contract (docs/verus/host-contract.md): the transport delivers only
// what a verified server's ghost log sent, and each server's segments run
// in its log's order -- the distributed model's safety invariant holds
// after every step: at most one leader per term, logs that match, committed
// entries never lost (glr ghost_log_compose.rs, theorem_compose_safety).
pub open spec fn cluster_logs<C>(cores: Seq<RaftCore<C>>) -> Seq<Seq<Entry>> {
    Seq::new(cores.len(), |i: int| cores[i].g_log_@)
}

pub proof fn theorem_mako_safety<C>(cores: Seq<RaftCore<C>>, n: int, sched: Seq<int>, k: int)
    requires
        0 < n,
        cores.len() == n,
        forall|i: int| 0 <= i < n ==> (#[trigger] cores[i]).inv() && cores[i].ginv()
            && cores[i].n_view() == n && cores[i].my_rank() == i,
        glr::protocol::Raft::ghost_log_compose::sched_ok(cluster_logs(cores), n, Set::<int>::range(0, n), sched),
        glr::protocol::Raft::ghost_log_compose::causal(cluster_logs(cores), n, Set::<int>::range(0, n), sched),
        0 <= k <= sched.len(),
    ensures
        glr::protocol::Raft::refinement_proof::invariants::RaftSafetyInvariant(
            glr::protocol::Raft::ghost_log_compose::ds_at(cluster_logs(cores), n, Set::<int>::range(0, n), sched, k)),
{
    let logs = cluster_logs(cores);
    let cfg0 = Set::<int>::range(0, n);
    // n fits: a configuration of u16 sites has at most 2^16 members
    assert(cores[0].inv());
    lemma_sorted_len(cores[0].config_members_@);
    vstd::set_lib::lemma_int_range(0, n);
    assert(glr::protocol::Raft::ghost_log_compose::cfg0_ok(n, cfg0));
    assert forall|i: int| 0 <= i < n implies
        glr::protocol::Raft::ghost_log_compose::node_cert(#[trigger] logs[i], n, cfg0, i) by {
        lemma_node_cert(&cores[i]);
    }
    assert(glr::protocol::Raft::ghost_log_compose::certs_ok(logs, n, cfg0));
    glr::protocol::Raft::ghost_log_compose::theorem_compose_safety(logs, n, cfg0, sched, k);
}

} // verus!
