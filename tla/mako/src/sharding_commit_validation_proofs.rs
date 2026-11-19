use vstd::prelude::*;
use vstd::imap::IMap as Map;
use vstd::iset::ISet as Set;
use super::*;

verus! {

pub proof fn initial_safe(c: Constants)
    requires constants(c)
    ensures inv(c,initial(c))
{}

/// Compose the epoch/range validator with the actual projected transition.
/// The equality is a metadata projection at the coherent snapshot's load
/// point, not a premise that the range is Serving.
pub proof fn epoch_validation_refines_gate(c: Constants, s: State, id: int,
    h: Seq<authority::Snapshot>, i: int, j: int,
    footprint: Map<authority::Key,Grant>, key: authority::Key)
    requires constants(c), inv(c,s), live(s,id), s.txns[id].sealed,
        authority::history(h), 0 <= i <= j < h.len(), !h[i].fenced,
        footprint.contains_key(key), footprint[key] == s.txns[id].grant,
        h[i].epoch == s.txns[id].epoch, h[j].epoch == s.epochs[s.txns[id].grant.owner],
        authority::exact(h[i],s.txns[id].grant.owner,footprint),
        authority::accepted(h[i],h[j],s.txns[id].grant.owner,footprint),
        s.serving[s.txns[id].grant.owner] ==
            if h[j].serving.contains_key(key) && h[j].serving[key].owner == s.txns[id].grant.owner {
                Some(h[j].serving[key].version)
            } else { None }
    ensures enabled(c,s,Action::ValidateAuthority { id })
{
    authority::validation_establishes_authority(h,i,j,s.txns[id].grant.owner,footprint);
}

pub proof fn copy_preserves(c: Constants, s: State, generation: nat)
    requires constants(c), inv(c,s), enabled(c,s,Action::Copy { generation })
    ensures inv(c,apply(s,Action::Copy { generation }))
{
    let m = s.movement.unwrap();
    assert(move_inv(c,s,m));
    assert forall|id: int| s.txns.contains_key(id) implies !pending(s.txns[id]) by {
        if pending(s.txns[id]) {
            assert(txn_inv(c,s,id));
            assert(s.txns[id].grant == m.source);
        }
    }
    let out = apply(s,Action::Copy { generation });
    assert(out.data.dom() =~= c.nodes);
    assert forall|id: int| out.txns.contains_key(id) implies txn_inv(c,out,id) by {
        assert(txn_inv(c,s,id));
    }
    assert(move_inv(c,out,out.movement.unwrap()));
}

pub proof fn transaction_preserves(c: Constants, s: State, a: Action)
    requires constants(c), inv(c,s), enabled(c,s,a),
        a is Capture || a is Observe || a is Lock || a is Seal || a is ValidateAuthority
            || a is ValidateData || a is Install || a is Abort || a is Release || a is Finish
    ensures inv(c,apply(s,a))
{
    match a {
        Action::Capture { .. } => { assert(inv(c,apply(s,a))); },
        Action::Observe { .. } => { assert(inv(c,apply(s,a))); },
        Action::Lock { .. } => { assert(inv(c,apply(s,a))); },
        Action::Seal { .. } => { assert(inv(c,apply(s,a))); },
        Action::ValidateAuthority { .. } => { assert(inv(c,apply(s,a))); },
        Action::ValidateData { .. } => { assert(inv(c,apply(s,a))); },
        Action::Install { id } => {
            let out = apply(s,a);
            assert(out.data.dom() =~= c.nodes);
            assert(txn_inv(c,s,id));
            assert(pending(s.txns[id]));
            assert(s.txns[id].grant == s.route);
            assert forall|other: int| out.txns.contains_key(other) implies txn_inv(c,out,other) by {
                assert(txn_inv(c,s,other));
            }
            if s.movement is Some {
                let m = s.movement.unwrap();
                assert(move_inv(c,s,m));
                if m.copied && !(m.phase is Active) { assert(false); }
                assert(move_inv(c,out,m));
            }
            assert(inv(c,out));
        },
        Action::Abort { .. } => { assert(inv(c,apply(s,a))); },
        Action::Release { .. } => { assert(inv(c,apply(s,a))); },
        Action::Finish { .. } => { assert(inv(c,apply(s,a))); },
        _ => {},
    }
}

pub proof fn migration_preserves(c: Constants, s: State, a: Action)
    requires constants(c), inv(c,s), enabled(c,s,a),
        a is PrepareMove || a is Freeze || a is Retire || a is Publish
            || a is Activate || a is Reclaim || a is Cancel || a is EpochBump || a is Stutter
    ensures inv(c,apply(s,a))
{
    match a {
        Action::PrepareMove { .. } => { assert(inv(c,apply(s,a))); },
        Action::Freeze { .. } => {
            let out = apply(s,a);
            let m = s.movement.unwrap();
            assert(move_inv(c,s,m));
            assert(out.serving.dom() =~= c.nodes);
            assert(out.epochs.dom() =~= c.nodes);
            assert forall|n: int| c.nodes.contains(n) implies out.serving[n] is None by {
                if s.serving[n] is Some { assert(n == m.source.owner); }
            }
            assert(move_inv(c,out,out.movement.unwrap()));
            assert(inv(c,out));
        },
        Action::Retire { .. } => { assert(inv(c,apply(s,a))); },
        Action::Publish { .. } => { assert(inv(c,apply(s,a))); },
        Action::Activate { .. } => {
            let out = apply(s,a);
            let m = s.movement.unwrap();
            assert(move_inv(c,s,m));
            assert(out.serving.dom() =~= c.nodes);
            assert(out.epochs.dom() =~= c.nodes);
            assert forall|n: int| c.nodes.contains(n) && out.serving[n] is Some
                implies out.route == (Grant { owner: n, version: out.serving[n].unwrap() })
                    && out.available.contains(n) by {
                if n != m.destination && s.serving[n] is Some { assert(false); }
            }
            assert(move_inv(c,out,out.movement.unwrap()));
            assert(inv(c,out));
        },
        Action::Reclaim { .. } => {
            assert(apply(s,a).data.dom() =~= c.nodes);
            assert(inv(c,apply(s,a)));
        },
        Action::Cancel { .. } => {
            assert(apply(s,a).serving.dom() =~= c.nodes);
            assert(apply(s,a).epochs.dom() =~= c.nodes);
            assert(inv(c,apply(s,a)));
        },
        Action::EpochBump { .. } => {
            assert(apply(s,a).epochs.dom() =~= c.nodes);
            assert(inv(c,apply(s,a)));
        },
        Action::Stutter => {},
        _ => {},
    }
}

pub proof fn step_preserves(c: Constants, s: State, a: Action)
    requires constants(c), inv(c,s), enabled(c,s,a)
    ensures inv(c,apply(s,a))
{
    match a {
        Action::Copy { generation } => copy_preserves(c,s,generation),
        Action::Capture { .. } | Action::Observe { .. } | Action::Lock { .. }
        | Action::Seal { .. } | Action::ValidateAuthority { .. } | Action::ValidateData { .. }
        | Action::Install { .. } | Action::Abort { .. } | Action::Release { .. }
        | Action::Finish { .. } => transaction_preserves(c,s,a),
        _ => migration_preserves(c,s,a),
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

pub proof fn theorem_finite_behavior(c: Constants, states: Seq<State>, actions: Seq<Action>)
    requires constants(c), behavior(c,states,actions)
    ensures forall|i: int| 0 <= i < states.len() ==> inv(c,states[i])
{
    assert forall|i: int| 0 <= i < states.len() implies inv(c,states[i]) by {
        invariant_at(c,states,actions,i);
    }
}

/// The critical pre-Freeze writer case, including an absent key. No condition
/// here requires the source cell to contain a committed row.
pub proof fn pending_writer_blocks_copy(c: Constants, s: State, id: int)
    requires constants(c), inv(c,s), s.movement is Some,
        s.movement.unwrap().phase is Frozen, s.txns.contains_key(id), pending(s.txns[id])
    ensures !enabled(c,s,Action::Copy { generation: s.movement.unwrap().generation })
{
    assert(txn_inv(c,s,id));
    assert(s.txns[id].locked);
    assert(s.locks[s.txns[id].grant.owner] == id);
}

pub proof fn pins_clear_implies_unlocked(c: Constants, s: State, owner: int)
    requires constants(c), inv(c,s), pins_clear(s,owner)
    ensures !s.locks.contains_key(owner)
{
    if s.locks.contains_key(owner) {
        assert(s.txns.contains_key(s.locks[owner]));
        assert(txn_inv(c,s,s.locks[owner]));
        assert(s.txns[s.locks[owner]].grant.owner == owner);
    }
}

/// Once copied, this source coordinate cannot acquire a later authorized
/// effect. This is derived, rather than a stable-source premise on Copy.
pub proof fn copied_prefix_stable(c: Constants, s: State, a: Action)
    requires constants(c), inv(c,s), enabled(c,s,a), s.movement is Some,
        s.movement.unwrap().copied, !(s.movement.unwrap().phase is Active),
        !(a is Cancel), !(a is Activate)
    ensures apply(s,a).data[s.movement.unwrap().source.owner] == s.data[s.movement.unwrap().source.owner],
        apply(s,a).data[s.movement.unwrap().destination] == s.data[s.movement.unwrap().destination]
{}

pub proof fn publication_preserves_value(c: Constants, s: State, generation: nat)
    requires constants(c), inv(c,s), enabled(c,s,Action::Publish { generation })
    ensures apply(s,Action::Publish { generation }).data[apply(s,Action::Publish { generation }).route.owner]
        == s.logical,
        apply(s,Action::Publish { generation }).logical == s.logical
{}

pub proof fn lifetime_prevents_reclaim(c: Constants, s: State, id: int)
    requires constants(c), inv(c,s), s.txns.contains_key(id), !s.txns[id].finished,
        s.movement is Some, s.txns[id].grant.owner == s.movement.unwrap().source.owner
    ensures s.available.contains(s.txns[id].grant.owner),
        !enabled(c,s,Action::Reclaim { generation: s.movement.unwrap().generation })
{}

pub proof fn sealed_rejects_late_access(c: Constants, s: State, id: int)
    requires constants(c), inv(c,s), s.txns.contains_key(id), s.txns[id].sealed
    ensures !enabled(c,s,Action::Observe { id }), !enabled(c,s,Action::Lock { id })
{}

pub proof fn stale_control_rejected(c: Constants, s: State, generation: nat)
    requires s.movement is None || s.movement.unwrap().generation != generation
    ensures !enabled(c,s,Action::Freeze { generation }), !enabled(c,s,Action::Copy { generation }),
        !enabled(c,s,Action::Retire { generation }), !enabled(c,s,Action::Publish { generation }),
        !enabled(c,s,Action::Activate { generation }), !enabled(c,s,Action::Reclaim { generation }),
        !enabled(c,s,Action::Cancel { generation })
{}

/// Lift arbitrary-coordinate proofs through ONE common execution. This does
/// not choose a different transaction history/serialization for every key.
pub open spec fn common_execution(keys: Set<authority::Key>, c: Map<authority::Key,Constants>,
    states: Seq<Map<authority::Key,State>>, actions: Seq<Map<authority::Key,Action>>) -> bool
{
    states.len() == actions.len() + 1 && keys.subset_of(c.dom())
        && (forall|k: authority::Key| keys.contains(k) ==> constants(c[k]) && states[0][k] == initial(c[k]))
        && (forall|i: int| 0 <= i < states.len() ==> keys.subset_of(states[i].dom()))
        && (forall|i: int| 0 <= i < actions.len() ==> keys.subset_of(actions[i].dom())
            && forall|k: authority::Key| keys.contains(k) ==>
                enabled(c[k],states[i][k],actions[i][k])
                && states[i+1][k] == apply(states[i][k],actions[i][k]))
}
pub proof fn common_invariant_at(keys: Set<authority::Key>, c: Map<authority::Key,Constants>,
    states: Seq<Map<authority::Key,State>>, actions: Seq<Map<authority::Key,Action>>, i: int)
    requires common_execution(keys,c,states,actions), 0 <= i < states.len()
    ensures forall|k: authority::Key| keys.contains(k) ==> inv(c[k],states[i][k])
    decreases i
{
    if i > 0 { common_invariant_at(keys,c,states,actions,i-1); }
    assert forall|k: authority::Key| keys.contains(k) implies inv(c[k],states[i][k]) by {
        if i == 0 { initial_safe(c[k]); }
        else { step_preserves(c[k],states[i-1][k],actions[i-1][k]); }
    }
}
/// All coordinates in an interval are included, not merely present rows. The
/// common engine's ordinary OCC/serialization contract remains a separate
/// premise; this theorem is physical/logical agreement, NOT serializability.
pub proof fn whole_range_agreement(keys: Set<authority::Key>, c: Map<authority::Key,Constants>,
    states: Seq<Map<authority::Key,State>>, actions: Seq<Map<authority::Key,Action>>,
    i: int, table: int, lo: int, hi: Option<int>)
    requires common_execution(keys,c,states,actions), 0 <= i < states.len(),
        authority::interval(table,lo,hi).subset_of(keys)
    ensures forall|k: authority::Key| authority::interval(table,lo,hi).contains(k) ==>
        states[i][k].data[states[i][k].route.owner] == states[i][k].logical
{
    common_invariant_at(keys,c,states,actions,i);
}

/// The immutable directory publishes the entire ownership scope in one step,
/// including all row aliases and absent keys. Retired supplies exact coverage
/// through the proved invariant, not a caller-supplied equality assumption.
pub open spec fn publish_scope(keys: Set<authority::Key>, s: Map<authority::Key,State>,
    generation: nat) -> Map<authority::Key,State>
{
    Map::new(|k: authority::Key| s.contains_key(k), |k: authority::Key|
        if keys.contains(k) { apply(s[k],Action::Publish { generation }) } else { s[k] })
}
pub proof fn whole_scope_publication(keys: Set<authority::Key>, c: Map<authority::Key,Constants>,
    s: Map<authority::Key,State>, destination: int, generation: nat)
    requires keys.subset_of(c.dom()), keys.subset_of(s.dom()),
        forall|k: authority::Key| keys.contains(k) ==> constants(c[k]) && inv(c[k],s[k])
            && s[k].movement is Some && s[k].movement.unwrap().phase is Retired
            && s[k].movement.unwrap().destination == destination
            && s[k].movement.unwrap().generation == generation
    ensures publish_scope(keys,s,generation).dom() == s.dom(),
        forall|k: authority::Key| keys.contains(k) ==>
            inv(c[k],publish_scope(keys,s,generation)[k])
            && publish_scope(keys,s,generation)[k].route == (Grant { owner: destination, version: generation })
            && publish_scope(keys,s,generation)[k].data[destination] == s[k].logical
            && publish_scope(keys,s,generation)[k].logical == s[k].logical,
        forall|k: authority::Key| s.contains_key(k) && !keys.contains(k) ==>
            publish_scope(keys,s,generation)[k] == s[k]
{
    assert(publish_scope(keys,s,generation).dom() =~= s.dom());
    assert forall|k: authority::Key| keys.contains(k) implies
        inv(c[k],publish_scope(keys,s,generation)[k])
        && publish_scope(keys,s,generation)[k].route == (Grant { owner: destination, version: generation })
        && publish_scope(keys,s,generation)[k].data[destination] == s[k].logical
        && publish_scope(keys,s,generation)[k].logical == s[k].logical by {
        publication_preserves_value(c[k],s[k],generation);
        migration_preserves(c[k],s[k],Action::Publish { generation });
    }
}

} // verus!
