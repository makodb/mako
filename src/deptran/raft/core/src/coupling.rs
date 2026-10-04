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
        post.g_log_ == pre.g_log_,
        post.g_votes_ == pre.g_votes_,
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

} // verus!
