//! Reachable-state facts needed for progress, proved over the unchanged
//! placement transition relation rather than supplied as fairness premises.
use super::*;

verus! {

pub open spec fn source_pre(s: p::State, g: nat) -> bool {
    forall|k: int| s.plans[g].keys.contains(k) ==> {
        let r = p::replica(s,s.plans[g].src,k);
        r.role is Serving && r.terminal && r.fence < g
    }
}
pub open spec fn source_frozen(s: p::State, g: nat) -> bool {
    forall|k: int| s.plans[g].keys.contains(k) ==> {
        let r = p::replica(s,s.plans[g].src,k);
        r.role is Frozen && !r.terminal && r.fence == g
    }
}
pub open spec fn source_retired(s: p::State, g: nat) -> bool {
    forall|k: int| s.plans[g].keys.contains(k) ==> {
        let r = p::replica(s,s.plans[g].src,k);
        r.role is Retired && !r.terminal && r.fence == g
    }
}
pub open spec fn participant_clean(s: p::State, g: nat, n: int) -> bool {
    forall|k: int| s.plans[g].keys.contains(k) ==> {
        let r = p::replica(s,n,k); r.terminal && r.fence == g
    }
}
pub open spec fn destination_pre(s: p::State, g: nat) -> bool {
    forall|k: int| s.plans[g].keys.contains(k) ==> {
        let r = p::replica(s,s.plans[g].dst,k);
        r.role is Empty && r.terminal && r.fence < g
    }
}
pub open spec fn destination_stage(s: p::State, g: nat, round: nat) -> bool {
    forall|k: int| s.plans[g].keys.contains(k) ==> {
        let r = p::replica(s,s.plans[g].dst,k);
        r.role is Stage && !r.terminal && r.fence == g && r.round == round
    }
}
pub open spec fn destination_ready(s: p::State, g: nat) -> bool {
    forall|k: int| s.plans[g].keys.contains(k) ==> {
        let r = p::replica(s,s.plans[g].dst,k);
        r.role is Ready && !r.terminal && r.fence == g && r.covered
    }
}
pub open spec fn destination_started(s: p::State, g: nat) -> bool {
    destination_stage(s,g,0) || destination_stage(s,g,1) || destination_ready(s,g)
}
pub open spec fn destination_final(s: p::State, g: nat) -> bool {
    destination_stage(s,g,1) || destination_ready(s,g)
}

pub open spec fn command_shape(s: p::State, g: nat) -> bool {
    &&& s.commands.contains((g,p::Command::Start))
    &&& match s.phases[g] {
        p::Phase::Copy => true,
        p::Phase::Freezing => s.commands.contains((g,p::Command::Freeze)),
        p::Phase::Final => s.commands.contains((g,p::Command::Final)),
        p::Phase::Retiring => s.commands.contains((g,p::Command::Retire)),
        p::Phase::Committed => s.commands.contains((g,p::Command::Commit)),
        p::Phase::Aborted => s.commands.contains((g,p::Command::Abort)),
    }
}

pub open spec fn bulk_shape(s: p::State, g: nat) -> bool {
    &&& source_pre(s,g) || source_frozen(s,g) || source_retired(s,g)
        || participant_clean(s,g,s.plans[g].src)
    &&& destination_pre(s,g) || destination_stage(s,g,0) || destination_stage(s,g,1)
        || destination_ready(s,g) || participant_clean(s,g,s.plans[g].dst)
    &&& (source_retired(s,g) ==> s.certificates.contains((g,p::Certificate::Retired)))
    &&& (destination_ready(s,g) ==> s.certificates.contains((g,p::Certificate::Ready)))
    &&& (participant_clean(s,g,s.plans[g].src) ==> s.certificates.contains((g,p::Certificate::SourceDone)))
    &&& (participant_clean(s,g,s.plans[g].dst) ==> s.certificates.contains((g,p::Certificate::DestinationDone)))
}

pub open spec fn progress_inv(c: p::Constants, s: p::State) -> bool {
    &&& p::inv(c,s)
    &&& forall|g: nat| s.plans.contains_key(g) ==> command_shape(s,g)
    &&& forall|g: nat| p::current(s,g) ==> bulk_shape(s,g)
    &&& forall|g: nat| s.plans.contains_key(g) && !p::current(s,g) ==>
        s.received.contains((g,p::Certificate::SourceDone))
            && s.received.contains((g,p::Certificate::DestinationDone))
}

pub proof fn selected_key(c:p::Constants,s:p::State,g:nat,k:int)
    requires p::inv(c,s),s.plans.contains_key(g),s.plans[g].keys.contains(k)
    ensures c.keys.contains(k),p::key_inv(c,s,k),
        s.physical.contains_key((s.plans[g].src,k)),
        s.physical.contains_key((s.plans[g].dst,k))
{
    assert(p::plan_inv(c,s,g));
    assert(s.plans[g].keys == p::selected(c,s.plans[g].table,s.plans[g].lo,s.plans[g].hi));
    assert(c.keys.contains(k));
}

pub proof fn active_facts(c:p::Constants,s:p::State,g:nat)
    requires progress_inv(c,s),p::current(s,g)
    ensures s.plans.contains_key(g),p::plan_inv(c,s,g),command_shape(s,g),bulk_shape(s,g),
        forall|k:int| s.plans[g].keys.contains(k) ==> p::key_inv(c,s,k)
            && s.physical.contains_key((s.plans[g].src,k)) && s.physical.contains_key((s.plans[g].dst,k))
{
    assert(s.plans.contains_key(g));
    assert(p::plan_inv(c,s,g));
    assert(command_shape(s,g)); assert(bulk_shape(s,g));
    assert forall|k:int| s.plans[g].keys.contains(k) implies p::key_inv(c,s,k)
        && s.physical.contains_key((s.plans[g].src,k)) && s.physical.contains_key((s.plans[g].dst,k)) by {
        selected_key(c,s,g,k);
    }
}

pub proof fn nonempty_plan(c: p::Constants, s: p::State, g: nat) -> (k: int)
    requires p::inv(c,s), s.plans.contains_key(g)
    ensures s.plans[g].keys.contains(k),c.keys.contains(k),p::key_inv(c,s,k)
{
    assert(p::plan_inv(c,s,g));
    if !(exists|k: int| s.plans[g].keys.contains(k)) {
        assert(s.plans[g].keys =~= Set::<int>::empty());
    }
    let k = choose|k: int| s.plans[g].keys.contains(k);
    selected_key(c,s,g,k);
    k
}

/// An old generation cannot manufacture new Drain/Seal certificates by acting
/// on a later incarnation. Control/copy rejection is supplied by placement.
pub proof fn old_local_work_disabled(c: p::Constants, s: p::State, g: nat)
    requires p::inv(c,s), s.plans.contains_key(g), !p::current(s,g)
    ensures !p::enabled(c,s,p::Action::Drain { generation: g }),
        !p::enabled(c,s,p::Action::Seal { generation: g })
{
    let k = nonempty_plan(c,s,g);
    assert(p::plan_inv(c,s,g));
}

pub proof fn progress_initial(c: p::Constants)
    requires p::constants_ok(c)
    ensures progress_inv(c,p::initial(c))
{
    p::lemma_init(c);
}

pub proof fn bulk_shape_step(c: p::Constants, s: p::State, a: p::Action, g: nat)
    requires progress_inv(c,s), p::enabled(c,s,a), p::current(s,g),
        p::current(p::apply(c,s,a),g)
    ensures bulk_shape(p::apply(c,s,a),g)
{
    let z = p::apply(c,s,a);
    let k = nonempty_plan(c,s,g);
    assert(bulk_shape(s,g));
    assert(p::plan_inv(c,s,g));
    assert(p::key_inv(c,s,k));
    match a {
        p::Action::Deliver { generation, command, owner } => {
            if generation != g {
                assert(p::command_inv(s,generation,command));
                p::lemma_obsolete_control_rejected(c,s,generation,command,owner);
                assert(z == s);
            } else {
                let src = s.plans[g].src;
                let dst = s.plans[g].dst;
                assert forall|q:int| s.plans[g].keys.contains(q) implies
                    s.physical.contains_key((src,q)) && s.physical.contains_key((dst,q)) by {
                    selected_key(c,s,g,q);
                }
                match command {
                    p::Command::Start => {
                        if owner == dst && p::local_guard(s,g,command,dst) {
                            assert(destination_stage(z,g,0));
                        }
                    },
                    p::Command::Freeze => {
                        if owner == src && p::local_guard(s,g,command,src) { assert(source_frozen(z,g)); }
                    },
                    p::Command::Final => {
                        if owner == dst && p::local_guard(s,g,command,dst) { assert(destination_stage(z,g,1)); }
                    },
                    p::Command::Retire => {
                        if owner == src && p::local_guard(s,g,command,src) { assert(source_retired(z,g)); }
                    },
                    p::Command::Commit | p::Command::Abort => {
                        if owner == src && p::local_guard(s,g,command,src) {
                            assert(participant_clean(z,g,src));
                        }
                        if owner == dst && p::local_guard(s,g,command,dst) {
                            assert(participant_clean(z,g,dst));
                        }
                    },
                }
            }
        },
        p::Action::Seal { generation } => {
            if generation != g { old_local_work_disabled(c,s,generation); }
            assert(destination_ready(z,g));
        },
        p::Action::Drain { generation } => {
            if generation != g { old_local_work_disabled(c,s,generation); }
        },
        p::Action::DeliverCopy { packet } => {
            if packet.generation != g {
                assert(p::packet_inv(s,packet));
                p::lemma_obsolete_copy_rejected(c,s,packet);
                assert(z == s);
            }
        },
        _ => {},
    }
}

pub proof fn progress_step(c: p::Constants, s: p::State, a: p::Action)
    requires progress_inv(c,s)
    ensures progress_inv(c,p::dispatch(c,s,a))
{
    let z = p::dispatch(c,s,a);
    p::lemma_dispatch(c,s,a);
    if p::enabled(c,s,a) {
        assert forall|g: nat| z.plans.contains_key(g) implies command_shape(z,g) by {
            match a {
                p::Action::Begin { .. } => {
                    if g != s.next_generation { assert(command_shape(s,g)); }
                },
                _ => { assert(command_shape(s,g)); },
            }
        }
        assert forall|g: nat| p::current(z,g) implies bulk_shape(z,g) by {
            match a {
                p::Action::Begin { src,dst,.. } => {
                    assert(g == s.next_generation);
                    assert forall|k: int| z.plans[g].keys.contains(k) implies {
                        let a = p::replica(z,src,k); let b = p::replica(z,dst,k);
                        a.role is Serving && a.terminal && a.fence < g
                            && b.role is Empty && b.terminal && b.fence < g
                    } by {
                        assert(p::key_inv(c,s,k));
                        assert(p::stable_replica(s,src,k));
                        assert(p::stable_replica(s,dst,k));
                    }
                    let k = nonempty_plan(c,z,g);
                    assert(source_pre(z,g) && destination_pre(z,g));
                },
                _ => { bulk_shape_step(c,s,a,g); },
            }
        }
        assert forall|g: nat| z.plans.contains_key(g) && !p::current(z,g) implies
            z.received.contains((g,p::Certificate::SourceDone))
                && z.received.contains((g,p::Certificate::DestinationDone)) by {
            match a {
                p::Action::Finish => {
                    if p::current(s,g) { assert(p::enabled(c,s,p::Action::Finish)); }
                },
                p::Action::Begin { .. } => { assert(g != s.next_generation); },
                _ => {},
            }
            assert(s.plans.contains_key(g));
            if !p::current(s,g) {
                assert(s.received.contains((g,p::Certificate::SourceDone))
                    && s.received.contains((g,p::Certificate::DestinationDone)));
            }
            metadata_monotone(c,s,a,g);
        }
    }
}

pub open spec fn phase_number(s: p::State, g: nat) -> nat {
    match s.phases[g] {
        p::Phase::Copy => 0,
        p::Phase::Freezing => 1,
        p::Phase::Final => 2,
        p::Phase::Retiring => 3,
        p::Phase::Committed | p::Phase::Aborted => 4,
    }
}

pub proof fn metadata_monotone(c: p::Constants, s: p::State, a: p::Action, g: nat)
    requires p::inv(c,s), s.plans.contains_key(g)
    ensures p::dispatch(c,s,a).plans.contains_key(g),
        p::dispatch(c,s,a).plans[g] == s.plans[g],
        phase_number(p::dispatch(c,s,a),g) >= phase_number(s,g),
        s.commands.subset_of(p::dispatch(c,s,a).commands),
        s.certificates.subset_of(p::dispatch(c,s,a).certificates),
        s.received.subset_of(p::dispatch(c,s,a).received),
        s.packets.subset_of(p::dispatch(c,s,a).packets),
        !p::current(s,g) ==> !p::current(p::dispatch(c,s,a),g)
{
    if p::enabled(c,s,a) {
        match a {
            p::Action::Begin { .. } => {
                assert(p::plan_inv(c,s,g));
                assert(g < s.next_generation);
            },
            p::Action::RequestFreeze | p::Action::RequestFinal | p::Action::RequestRetire
            | p::Action::Commit | p::Action::Abort => {
                if s.active.unwrap() == g { assert(p::enabled(c,s,a)); }
            },
            _ => {},
        }
    }
}

/// Persistent session terminality is independent of which legal writes won.
pub proof fn session_monotone(c: p::Constants, s: p::State, a: p::Action, txn: int)
    requires p::inv(c,s), s.sessions.contains_key(txn)
    ensures p::dispatch(c,s,a).sessions.contains_key(txn),
        s.sessions[txn].resolved ==> p::dispatch(c,s,a).sessions[txn].resolved
{
    if p::enabled(c,s,a) { match a { p::Action::Open { .. } => {}, _ => {} } }
}

/// No new selected holds after physical freeze; unrelated transactions,
/// arbitrary legal writes by old holders, and all release orders are allowed.
pub proof fn frozen_holds_step(c: p::Constants, s: p::State, a: p::Action, g: nat)
    requires p::inv(c,s), p::current(s,g), p::before_decision(s,g), p::source_frozen(s,g)
    ensures forall|txn: int, key: int| s.plans[g].keys.contains(key)
        && p::dispatch(c,s,a).sessions.contains_key(txn)
        && p::dispatch(c,s,a).sessions[txn].held.contains_key(key) ==>
        s.sessions.contains_key(txn) && s.sessions[txn].held.contains_key(key)
{
    if p::enabled(c,s,a) {
        match a {
            p::Action::Acquire { txn,key,grant } => {
                if s.plans[g].keys.contains(key) {
                    p::lemma_freeze_blocks_admission(c,s,g,txn,key,grant);
                }
            },
            _ => {},
        }
    }
}

/// Local progress cannot regress while the coordinator remains in the same
/// undecided phase. These are ordinary step lemmas, not temporal assumptions.
pub proof fn same_phase_local_step(c: p::Constants, s: p::State, a: p::Action, g: nat)
    requires progress_inv(c,s), p::current(s,g), p::before_decision(s,g),
        p::dispatch(c,s,a).phases[g] == s.phases[g]
    ensures source_frozen(s,g) && s.phases[g] is Freezing ==> source_frozen(p::dispatch(c,s,a),g),
        destination_started(s,g) && s.phases[g] is Final ==> destination_started(p::dispatch(c,s,a),g),
        destination_final(s,g) && s.phases[g] is Final ==> destination_final(p::dispatch(c,s,a),g),
        forall|k: int| s.plans[g].keys.contains(k) && destination_final(s,g)
            && s.phases[g] is Final && p::replica(s,s.plans[g].dst,k).covered ==>
            p::replica(p::dispatch(c,s,a),s.plans[g].dst,k).covered
{
    let z = p::dispatch(c,s,a);
    active_facts(c,s,g);
    let anchor = nonempty_plan(c,s,g);
    progress_step(c,s,a);
    assert(p::current(z,g));
    active_facts(c,z,g);
    if source_frozen(s,g) && s.phases[g] is Freezing {
        assert(!p::local_guard(s,g,p::Command::Freeze,s.plans[g].src));
    }
    if destination_started(s,g) && s.phases[g] is Final {
        assert(!p::local_guard(s,g,p::Command::Start,s.plans[g].dst));
    }
    if destination_final(s,g) && s.phases[g] is Final {
        assert(!p::local_guard(s,g,p::Command::Final,s.plans[g].dst));
    }
    if p::enabled(c,s,a) {
        match a {
            p::Action::Deliver { generation,command,owner } => {
                assert(p::command_inv(s,generation,command));
                if generation != g {
                    p::lemma_obsolete_control_rejected(c,s,generation,command,owner);
                } else {
                    let k = nonempty_plan(c,s,g);
                    assert(p::key_inv(c,s,k));
                    match command {
                        p::Command::Start | p::Command::Final | p::Command::Freeze
                        | p::Command::Retire | p::Command::Commit | p::Command::Abort => {},
                    }
                }
            },
            p::Action::DeliverCopy { packet } => {
                if packet.generation != g { p::lemma_obsolete_copy_rejected(c,s,packet); }
            },
            p::Action::Seal { generation } | p::Action::Drain { generation } => {
                if generation != g { old_local_work_disabled(c,s,generation); }
            },
            _ => {},
        }
    }
    if source_frozen(s,g) && s.phases[g] is Freezing {
        assert(p::replica(z,s.plans[g].src,anchor).role is Frozen);
    }
    if destination_started(s,g) && s.phases[g] is Final {
        let r = p::replica(z,s.plans[g].dst,anchor);
        assert((r.role is Stage || r.role is Ready) && !r.terminal);
    }
    if destination_final(s,g) && s.phases[g] is Final {
        let r = p::replica(z,s.plans[g].dst,anchor);
        assert((r.role is Stage && r.round == 1 || r.role is Ready) && !r.terminal);
    }
    assert forall|k:int| s.plans[g].keys.contains(k) && destination_final(s,g)
        && s.phases[g] is Final && p::replica(s,s.plans[g].dst,k).covered implies
        p::replica(z,s.plans[g].dst,k).covered by {
        selected_key(c,s,g,k);
    }
}

pub proof fn terminal_shape_supplies_cleanup(c: p::Constants, s: p::State, g: nat)
    requires progress_inv(c,s), p::current(s,g), p::terminal(s.phases[g])
    ensures cleanup_ready(s,g)
{
    active_facts(c,s,g);
    let k = nonempty_plan(c,s,g);
    assert(p::key_inv(c,s,k));
    assert(p::plan_inv(c,s,g));
    assert forall|q: int| s.plans[g].keys.contains(q) implies p::key_inv(c,s,q) by {
        selected_key(c,s,g,q);
    }
    if s.phases[g] is Committed {
        if !s.certificates.contains((g,p::Certificate::SourceDone)) {
            assert(!source_pre(s,g) && !source_frozen(s,g));
            assert(source_retired(s,g));
            assert(p::local_guard(s,g,p::Command::Commit,s.plans[g].src));
        }
        if !s.certificates.contains((g,p::Certificate::DestinationDone)) {
            assert(!destination_pre(s,g) && !destination_stage(s,g,0) && !destination_stage(s,g,1));
            assert(destination_ready(s,g));
            assert(p::local_guard(s,g,p::Command::Commit,s.plans[g].dst));
        }
    } else {
        if !s.certificates.contains((g,p::Certificate::SourceDone)) {
            assert(p::local_guard(s,g,p::Command::Abort,s.plans[g].src));
        }
        if !s.certificates.contains((g,p::Certificate::DestinationDone)) {
            assert(p::local_guard(s,g,p::Command::Abort,s.plans[g].dst));
        }
    }
}

} // verus!
