use vstd::prelude::*;
use super::*;

verus! {

pub proof fn initial_safe(c: Constants)
    requires constants(c)
    ensures inv(c,initial(c))
{
    assert(blank(c).dom() =~= c.scope);
    assert(serving(c,c.initial).dom() =~= c.scope);
    assert forall|n: int| c.nodes.contains(n) implies admission::inv(initial(c).local[n]) by {
        admission::proofs::initial_safe(if n == c.initial.owner { serving(c,c.initial) } else { IMap::empty() });
    }
}

pub proof fn captured_after_completed(local: admission::State, id: int)
    requires admission::inv(local), local.readers.contains_key(id),
        local.readers[id].entered, !local.readers[id].finished,
        local.readers[id].view is Some
    ensures suffix_start(local) <= local.readers[id].view.unwrap() < local.history.len()
{
    assert(admission::reader_inv(local,local.readers[id]));
    if local.completed > 0 {
        admission::proofs::completed_excludes_older_views(local,local.completed,id);
    }
}

/// All authorization comes from the captured immutable object. The current
/// route equality is a CONSEQUENCE of drain/history provenance, not an Enroll
/// or commit-time authority gate.
pub proof fn captured_authority_is_current(c: Constants, s: State, node: int, id: int, grant: Grant)
    requires constants(c), inv(c,s), c.nodes.contains(node),
        scope_authorized(c,s,node,id,grant)
    ensures node == s.route.owner, grant == s.route,
        s.local[node].readers.contains_key(id), s.local[node].readers[id].entered,
        !s.local[node].readers[id].finished, s.local[node].readers[id].view is Some,
        s.local[node].history[s.local[node].readers[id].view.unwrap() as int] == serving(c,grant)
{
    assert(admission::authorized(s.local[node],id,c.anchor,grant));
    captured_after_completed(s.local[node],id);
    let i = s.local[node].readers[id].view.unwrap() as int;
    assert(suffix_inv(c,s,node));
    assert(s.local[node].history[i] == IMap::<Key,Grant>::empty()
        || (node == s.route.owner && s.local[node].history[i] == serving(c,s.route)));
    assert(serving(c,s.route)[c.anchor] == s.route);
}

/// The critical composition: Complete excludes old captured objects, the Tick
/// floor is at/after the actual Freeze publication, and that history suffix is
/// empty. No desired source-stability fact appears in Copy's guard.
pub proof fn completed_cut_excludes_source_authority(c: Constants, s: State, id: int, key: Key, grant: Grant)
    requires constants(c), inv(c,s), s.movement is Some,
        s.movement.unwrap().cut is Some,
        s.movement.unwrap().cut.unwrap() <= s.local[s.movement.unwrap().source.owner].completed
    ensures !admission::authorized(s.local[s.movement.unwrap().source.owner],id,key,grant)
{
    let m = s.movement.unwrap();
    let local = s.local[m.source.owner];
    assert(move_inv(c,s,m));
    if admission::authorized(local,id,key,grant) {
        admission::proofs::completed_excludes_older_views(local,m.cut.unwrap(),id);
        let i = local.readers[id].view.unwrap() as int;
        assert(m.frozen_index.unwrap() <= i < local.history.len());
        assert(local.history[i] == IMap::<Key,Grant>::empty());
    }
}

pub proof fn drain_removes_old_effects(c: Constants, s: State)
    requires constants(c), inv(c,s), s.movement is Some,
        s.movement.unwrap().cut is Some,
        s.movement.unwrap().cut.unwrap() <= s.local[s.movement.unwrap().source.owner].completed
    ensures forall|id: int| s.txns.contains_key(id)
        && s.txns[id].grant.owner == s.movement.unwrap().source.owner ==>
        s.local[s.txns[id].grant.owner].readers[id].finished
            && s.txns[id].cleaned && !(s.txns[id].outcome is Pending)
{
    let m = s.movement.unwrap();
    assert forall|id: int| s.txns.contains_key(id) && s.txns[id].grant.owner == m.source.owner
        implies s.local[m.source.owner].readers[id].finished
            && s.txns[id].cleaned && !(s.txns[id].outcome is Pending) by {
        assert(txn_inv(c,s,id));
        if !s.local[m.source.owner].readers[id].finished {
            assert(admission::authorized(s.local[m.source.owner],id,c.anchor,s.txns[id].grant));
            completed_cut_excludes_source_authority(c,s,id,c.anchor,s.txns[id].grant);
        }
    }
}

pub proof fn drained_history_suffix_empty(c: Constants, s: State)
    requires constants(c), inv(c,s), s.movement is Some,
        s.movement.unwrap().cut is Some,
        s.movement.unwrap().cut.unwrap() <= s.local[s.movement.unwrap().source.owner].completed
    ensures forall|i: int| suffix_start(s.local[s.movement.unwrap().source.owner]) <= i
        < s.local[s.movement.unwrap().source.owner].history.len() ==>
        s.local[s.movement.unwrap().source.owner].history[i] == IMap::<Key,Grant>::empty()
{
    let m = s.movement.unwrap();
    let local = s.local[m.source.owner];
    assert(move_inv(c,s,m));
    assert(admission::floor_inv(local));
    assert(local.floor[m.cut.unwrap()] <= local.floor[local.completed]);
}

/// This is a direct consumer contract over the enabled request, intentionally
/// not predicated on uniform-history reachability. Checking only a first row
/// cannot establish this full-interval/alias/absence certificate.
pub proof fn enrollment_covers_entire_scope(c: Constants, s: State, node: int, id: int,
    grant: Grant, writes: bool, replacement: Image)
    requires enabled(c,s,Action::Enroll { node,id,grant,writes,replacement })
    ensures forall|key: Key| c.scope.contains(key) ==>
        admission::authorized(s.local[node],id,key,grant)
{}

pub proof fn late_old_request_rejected(c: Constants, s: State, id: int, writes: bool, replacement: Image)
    requires constants(c), inv(c,s), s.movement is Some,
        s.movement.unwrap().cut is Some,
        s.movement.unwrap().cut.unwrap() <= s.local[s.movement.unwrap().source.owner].completed
    ensures !enabled(c,s,Action::Enroll { node: s.movement.unwrap().source.owner,
        id, grant: s.movement.unwrap().source, writes, replacement })
{
    let m = s.movement.unwrap();
    completed_cut_excludes_source_authority(c,s,id,c.anchor,m.source);
}

pub proof fn pin_covers_effects_and_cleanup(c: Constants, s: State, id: int)
    requires constants(c), inv(c,s), s.txns.contains_key(id),
        s.txns[id].outcome is Pending || !s.txns[id].cleaned
    ensures !s.local[s.txns[id].grant.owner].readers[id].finished,
        s.local[s.txns[id].grant.owner].readers[id].announced,
        !enabled(c,s,Action::Local { node: s.txns[id].grant.owner, action: admission::Action::Exit { id } })
{
    assert(txn_inv(c,s,id));
    assert(admission::reader_inv(s.local[s.txns[id].grant.owner],
        s.local[s.txns[id].grant.owner].readers[id]));
}

pub proof fn exit_requires_terminal_cleanup(c: Constants, s: State, node: int, id: int)
    requires constants(c), inv(c,s), s.txns.contains_key(id), s.txns[id].grant.owner == node,
        enabled(c,s,Action::Local { node, action: admission::Action::Exit { id } })
    ensures s.txns[id].cleaned, !(s.txns[id].outcome is Pending)
{}

pub proof fn local_step_facts(local: admission::State, action: admission::Action)
    requires admission::inv(local), admission::enabled(local,action)
    ensures admission::inv(admission::apply(local,action)),
        local.completed <= admission::apply(local,action).completed,
        suffix_start(local) <= suffix_start(admission::apply(local,action)),
        local.history.len() <= admission::apply(local,action).history.len(),
        forall|i: int| 0 <= i < local.history.len() ==>
            admission::apply(local,action).history[i] == local.history[i]
{
    admission::proofs::step_preserves(local,action);
    match action {
        admission::Action::Complete { cut } => {
            if local.completed > 0 {
                assert(local.floor[local.completed] <= local.floor[cut]);
            }
        },
        _ => {},
    }
}

pub proof fn local_preserves(c: Constants, s: State, node: int, action: admission::Action)
    requires constants(c), inv(c,s), enabled(c,s,Action::Local { node,action })
    ensures inv(c,apply(c,s,Action::Local { node,action }))
{
    let a = Action::Local { node,action };
    let out = apply(c,s,a);
    local_step_facts(s.local[node],action);
    assert(!(action is Publish));
    assert(out.local.dom() =~= c.nodes);
    assert forall|n: int| c.nodes.contains(n) implies admission::inv(out.local[n])
        && out.data[n].dom() == c.scope && suffix_inv(c,out,n)
        && admission::current_view(out.local[n]) ==
            if current_serving(out) && n == out.route.owner { serving(c,out.route) } else { IMap::empty() } by {
        assert(suffix_inv(c,s,n));
        if n == node {
            assert(out.local[n].history == s.local[n].history);
        }
    }
    assert forall|id: int| out.txns.contains_key(id) implies txn_inv(c,out,id) by {
        assert(txn_inv(c,s,id));
        let owner = s.txns[id].grant.owner;
        if owner == node {
            match action {
                admission::Action::Probe { id: other,.. } => { assert(id != other); },
                admission::Action::Announce { id: other }
                | admission::Action::Validate { id: other }
                | admission::Action::Resample { id: other }
                | admission::Action::Capture { id: other } => { assert(id != other); },
                admission::Action::Exit { id: other } => {
                    if id == other { assert(s.txns[id].cleaned); }
                },
                _ => {},
            }
        }
    }
    if s.movement is Some {
        assert(move_inv(c,s,s.movement.unwrap()));
        assert(move_inv(c,out,out.movement.unwrap()));
    }
}

pub proof fn enrollment_preserves(c: Constants, s: State, node: int, id: int,
    grant: Grant, writes: bool, replacement: Image)
    requires constants(c), inv(c,s), enabled(c,s,Action::Enroll { node,id,grant,writes,replacement })
    ensures inv(c,apply(c,s,Action::Enroll { node,id,grant,writes,replacement }))
{
    captured_authority_is_current(c,s,node,id,grant);
    let out = apply(c,s,Action::Enroll { node,id,grant,writes,replacement });
    assert(txn_inv(c,out,id));
    assert forall|other: int| out.txns.contains_key(other) implies txn_inv(c,out,other) by {
        if other != id { assert(txn_inv(c,s,other)); }
    }
}

/// Placement safety does not rely on the conservative full-image OCC check.
/// Any pending engine effect stays on the canonical owner; the retained pin
/// prevents a handoff from overtaking it, regardless of its validation policy.
pub proof fn pending_effect_preserves(c: Constants, s: State, id: int)
    requires constants(c), inv(c,s), pending(s,id)
    ensures inv(c,apply(c,s,Action::EngineCommit { id })),
        s.txns[id].grant == s.route
{
    let out = apply(c,s,Action::EngineCommit { id });
    assert(txn_inv(c,s,id));
    assert(!s.local[s.txns[id].grant.owner].readers[id].finished);
    assert(s.txns[id].grant == s.route);
    assert(out.data.dom() =~= c.nodes);
    if s.movement is Some {
        let m = s.movement.unwrap();
        assert(move_inv(c,s,m));
        if copied(m) && before_publication(m) {
            drain_removes_old_effects(c,s);
            assert(false);
        }
        assert(move_inv(c,out,m));
    }
    assert forall|other: int| out.txns.contains_key(other) implies txn_inv(c,out,other) by {
        assert(txn_inv(c,s,other));
    }
}

pub proof fn engine_preserves(c: Constants, s: State, a: Action)
    requires constants(c), inv(c,s), enabled(c,s,a),
        a is EngineCommit || a is EngineAbort || a is Cleanup
    ensures inv(c,apply(c,s,a))
{
    match a {
        Action::EngineCommit { id } => {
            pending_effect_preserves(c,s,id);
        },
        Action::EngineAbort { .. } => {},
        Action::Cleanup { .. } => {},
        _ => {},
    }
}

pub proof fn copy_preserves(c: Constants, s: State, generation: nat)
    requires constants(c), inv(c,s), enabled(c,s,Action::Copy { generation })
    ensures inv(c,apply(c,s,Action::Copy { generation }))
{
    drain_removes_old_effects(c,s);
    let out = apply(c,s,Action::Copy { generation });
    assert(move_inv(c,s,s.movement.unwrap()));
    assert(out.data.dom() =~= c.nodes);
    assert(move_inv(c,out,out.movement.unwrap()));
}

proof fn freeze_preserves(c: Constants, s: State, generation: nat)
    requires constants(c), inv(c,s), enabled(c,s,Action::Freeze { generation })
    ensures inv(c,apply(c,s,Action::Freeze { generation }))
{
    let out = apply(c,s,Action::Freeze { generation });
    let m = s.movement.unwrap();
    local_step_facts(s.local[m.source.owner],admission::Action::Publish { view: IMap::empty() });
    assert(out.local.dom() =~= c.nodes);
    assert forall|i: int| out.movement.unwrap().frozen_index.unwrap() <= i
        < out.local[m.source.owner].history.len()
        implies out.local[m.source.owner].history[i] == IMap::<Key,Grant>::empty() by {
        assert(i == s.local[m.source.owner].history.len());
    }
}

proof fn establish_cut_preserves(c: Constants, s: State, generation: nat)
    requires constants(c), inv(c,s), enabled(c,s,Action::EstablishCut { generation })
    ensures inv(c,apply(c,s,Action::EstablishCut { generation }))
{
    let out = apply(c,s,Action::EstablishCut { generation });
    let m = s.movement.unwrap();
    local_step_facts(s.local[m.source.owner],admission::Action::Tick);
    assert(move_inv(c,s,m));
    assert(out.local.dom() =~= c.nodes);
    assert(m.frozen_index.unwrap() <= admission::current_index(s.local[m.source.owner]));
}

proof fn publish_preserves(c: Constants, s: State, generation: nat)
    requires constants(c), inv(c,s), enabled(c,s,Action::Publish { generation })
    ensures inv(c,apply(c,s,Action::Publish { generation }))
{
    let out = apply(c,s,Action::Publish { generation });
    let m = s.movement.unwrap();
    drain_removes_old_effects(c,s);
    drained_history_suffix_empty(c,s);
    assert forall|id: int| out.txns.contains_key(id) implies txn_inv(c,out,id) by {
        assert(txn_inv(c,s,id));
        if !s.local[s.txns[id].grant.owner].readers[id].finished {
            assert(s.txns[id].grant == m.source);
            assert(false);
        }
    }
    assert forall|n: int| c.nodes.contains(n) implies suffix_inv(c,out,n) by {
        assert(suffix_inv(c,s,n));
        assert forall|i: int| suffix_start(out.local[n]) <= i < out.local[n].history.len()
            implies out.local[n].history[i] == IMap::<Key,Grant>::empty() by {
            if n != m.source.owner {
                assert(n != s.route.owner);
            }
        }
    }
}

pub proof fn controller_preserves(c: Constants, s: State, a: Action)
    requires constants(c), inv(c,s), enabled(c,s,a),
        a is Prepare || a is Freeze || a is EstablishCut || a is Drain
        || a is Publish || a is Activate || a is Reclaim || a is Cancel || a is Stutter
    ensures inv(c,apply(c,s,a))
{
    let out = apply(c,s,a);
    match a {
        Action::Prepare { .. } => { assert(inv(c,out)); },
        Action::Freeze { generation } => freeze_preserves(c,s,generation),
        Action::EstablishCut { generation } => establish_cut_preserves(c,s,generation),
        Action::Drain { .. } => {
            let m = s.movement.unwrap();
            local_preserves(c,s,m.source.owner,admission::Action::Complete { cut: m.cut.unwrap() });
            assert(out == apply(c,s,Action::Local { node: m.source.owner,
                action: admission::Action::Complete { cut: m.cut.unwrap() } }));
            assert(inv(c,out));
        },
        Action::Publish { generation } => publish_preserves(c,s,generation),
        Action::Activate { .. } => {
            let m = s.movement.unwrap();
            local_step_facts(s.local[m.destination],admission::Action::Publish { view: serving(c,target(m)) });
            assert(out.local.dom() =~= c.nodes);
            assert(inv(c,out));
        },
        Action::Reclaim { .. } => {
            assert(out.data.dom() =~= c.nodes);
            assert(blank(c).dom() =~= c.scope);
            assert(inv(c,out));
        },
        Action::Cancel { .. } => {
            let m = s.movement.unwrap();
            local_step_facts(s.local[m.source.owner],admission::Action::Publish { view: serving(c,m.source) });
            assert(out.local.dom() =~= c.nodes);
            assert(inv(c,out));
        },
        _ => {},
    }
    assert(inv(c,out));
}

pub proof fn step_preserves(c: Constants, s: State, a: Action)
    requires constants(c), inv(c,s), enabled(c,s,a)
    ensures inv(c,apply(c,s,a))
{
    match a {
        Action::Local { node,action } => local_preserves(c,s,node,action),
        Action::Enroll { node,id,grant,writes,replacement } => enrollment_preserves(c,s,node,id,grant,writes,replacement),
        Action::EngineCommit { .. } | Action::EngineAbort { .. } | Action::Cleanup { .. } => engine_preserves(c,s,a),
        Action::Copy { generation } => copy_preserves(c,s,generation),
        Action::StartPrecopy { .. } | Action::Observe { .. } | Action::AcceptPrecopy { .. }
        | Action::FinishPrecopy { .. } | Action::ClosePrecopy { .. }
        | Action::JoinPrecopy { .. } => precopy::step_preserves(c,s,a),
        _ => controller_preserves(c,s,a),
    }
}

pub proof fn invariant_at(c: Constants, states: Seq<State>, actions: Seq<Action>, i: int)
    requires constants(c), behavior(c,states,actions), 0 <= i < states.len()
    ensures inv(c,states[i])
    decreases i
{
    if i == 0 { initial_safe(c); }
    else {
        invariant_at(c,states,actions,i-1);
        step_preserves(c,states[i-1],actions[i-1]);
    }
}

/// Every finite prefix, with no bound on IDs, epochs, generations, migrations,
/// cancellations, or returns to a previously used owner. This inductive theorem
/// is the safety argument for arbitrary-length/infinite executions.
pub proof fn theorem_finite_behavior(c: Constants, states: Seq<State>, actions: Seq<Action>)
    requires constants(c), behavior(c,states,actions)
    ensures forall|i: int| 0 <= i < states.len() ==> inv(c,states[i])
{
    assert forall|i: int| 0 <= i < states.len() implies inv(c,states[i]) by {
        invariant_at(c,states,actions,i);
    }
}

pub proof fn every_scope_coordinate_agrees(c: Constants, s: State)
    requires constants(c), inv(c,s)
    ensures s.data[s.route.owner] == s.logical,
        forall|key: Key| c.scope.contains(key) ==>
            s.data[s.route.owner].contains_key(key)
                && s.data[s.route.owner][key] == s.logical[key]
{}

pub proof fn engine_success_matches_logical(c: Constants, s: State, id: int)
    requires constants(c), inv(c,s), enabled(c,s,Action::EngineCommit { id })
    ensures s.txns[id].observed == s.logical,
        apply(c,s,Action::EngineCommit { id }).logical ==
            if s.txns[id].writes { s.txns[id].replacement } else { s.logical },
        apply(c,s,Action::EngineCommit { id }).data[s.route.owner] ==
            apply(c,s,Action::EngineCommit { id }).logical
{
    assert(txn_inv(c,s,id));
}

pub proof fn copy_waits_for_real_drain(c: Constants, s: State, generation: nat)
    requires constants(c), inv(c,s), enabled(c,s,Action::Copy { generation })
    ensures forall|id: int| s.txns.contains_key(id)
        && s.txns[id].grant.owner == s.movement.unwrap().source.owner ==>
        s.txns[id].cleaned && !(s.txns[id].outcome is Pending)
            && s.local[s.txns[id].grant.owner].readers[id].finished
{
    drain_removes_old_effects(c,s);
}

pub proof fn exact_copy_covers_absence(c: Constants, s: State, generation: nat, key: Key)
    requires enabled(c,s,Action::Copy { generation }), s.data[s.movement.unwrap().source.owner].contains_key(key)
    ensures apply(c,s,Action::Copy { generation }).data[s.movement.unwrap().destination]
        == s.data[s.movement.unwrap().source.owner],
        apply(c,s,Action::Copy { generation }).data[s.movement.unwrap().destination][key]
        == s.data[s.movement.unwrap().source.owner][key]
{}

pub proof fn copied_source_cannot_receive_effect(c: Constants, s: State, id: int)
    requires constants(c), inv(c,s), s.movement is Some, copied(s.movement.unwrap()),
        s.txns.contains_key(id), s.txns[id].grant.owner == s.movement.unwrap().source.owner
    ensures !enabled(c,s,Action::EngineCommit { id })
{
    drain_removes_old_effects(c,s);
}

pub proof fn source_stable_after_copy(c: Constants, s: State, a: Action)
    requires constants(c), inv(c,s), enabled(c,s,a), s.movement is Some,
        copied(s.movement.unwrap()), !(a is Reclaim)
    ensures apply(c,s,a).data[s.movement.unwrap().source.owner]
        == s.data[s.movement.unwrap().source.owner]
{
    match a {
        Action::EngineCommit { id } => {
            if s.txns[id].grant.owner == s.movement.unwrap().source.owner {
                copied_source_cannot_receive_effect(c,s,id);
            }
        },
        Action::FinishPrecopy { id } => {
            assert(s.effects.contains(id));
            assert(precopy_phase(s.movement.unwrap()));
            assert(false);
        },
        _ => {},
    }
}

pub proof fn stale_control_rejected(c: Constants, s: State, generation: nat)
    requires s.movement is None || s.movement.unwrap().generation != generation
    ensures !enabled(c,s,Action::Freeze { generation }),
        !enabled(c,s,Action::EstablishCut { generation }), !enabled(c,s,Action::Drain { generation }),
        !enabled(c,s,Action::Copy { generation }), !enabled(c,s,Action::Publish { generation }),
        !enabled(c,s,Action::Activate { generation }), !enabled(c,s,Action::Reclaim { generation }),
        !enabled(c,s,Action::Cancel { generation })
{}

/// Independent-process specialization only. Colocated owners instead use the
/// conservative shared-domain projection in projection.rs.
pub proof fn clocks_are_process_local(c: Constants, s: State, node: int, other: int)
    requires node != other, s.local.contains_key(other)
    ensures apply(c,s,Action::Local { node, action: admission::Action::Tick }).local[other] == s.local[other]
{}

pub proof fn periodic_tick_does_not_abort_commit(c: Constants, s: State, id: int, node: int)
    requires enabled(c,s,Action::EngineCommit { id })
    ensures enabled(c,apply(c,s,Action::Local { node, action: admission::Action::Tick }),
        Action::EngineCommit { id })
{}

pub proof fn cut_follows_freeze_publication(c: Constants, s: State, generation: nat)
    requires constants(c), inv(c,s), enabled(c,s,Action::EstablishCut { generation })
    ensures apply(c,s,Action::EstablishCut { generation }).local[s.movement.unwrap().source.owner]
        .floor[apply(c,s,Action::EstablishCut { generation }).movement.unwrap().cut.unwrap()]
        >= s.movement.unwrap().frozen_index.unwrap()
{
    assert(move_inv(c,s,s.movement.unwrap()));
}

pub proof fn publication_preserves_image(c: Constants, s: State, generation: nat)
    requires constants(c), inv(c,s), enabled(c,s,Action::Publish { generation })
    ensures apply(c,s,Action::Publish { generation }).data[apply(c,s,Action::Publish { generation }).route.owner]
        == s.logical,
        apply(c,s,Action::Publish { generation }).logical == s.logical
{
    assert(move_inv(c,s,s.movement.unwrap()));
}

} // verus!
