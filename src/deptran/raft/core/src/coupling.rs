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
pub proof fn lemma_g_close(l: Seq<Entry>, label: ActionLabel, d: int, c: LConstants)
    requires
        log_ok(l, c),
        action_holds(label, seg_state(l), replay(l), c, seg_sends(l)),
        label_compatible(label, seg_trigger(l)),
        seg_sends_to(l, d),
        dst_ok(label, seg_trigger(l), d) || seg_sends(l) == Seq::<LRaftMessage>::empty(),
    ensures
        log_ok(l.push(Entry::Close(label)), c),
        fully_closed(l.push(Entry::Close(label))),
        replay(l.push(Entry::Close(label))) == replay(l),
{
    if seg_sends(l) == Seq::<LRaftMessage>::empty() {
        lemma_no_send_entries(l, last_boundary(l), l.len() as int);
    }
    assert(seg_dsts_ok(l, label));
    lemma_seg_close(l, label, c);
    lemma_routed_push_close(l, label);
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

    // What the ghost log certifies at every step boundary.
    pub open spec fn ginv(&self) -> bool {
        &&& log_ok(self.g_log_@, self.c_view())
        &&& fully_closed(self.g_log_@)
        &&& replay(self.g_log_@) == self.state_view()
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

} // verus!
