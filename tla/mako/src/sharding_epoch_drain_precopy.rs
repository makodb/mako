//! Private, non-authoritative per-row precopy and explicit asynchronous effects.
//! Observe linearizes ONE source-row read, not a whole scan snapshot. Accept
//! linearizes the destination generation/round fence before an effect starts.
//! Close prevents new effects; Join waits for every accepted effect to finish.
//! Native correspondence requires those fence checks and effect registration to
//! be atomic with closure, and a joined task to include storage callback effects.
//! Network packets can remain arbitrarily delayed after Join: they cannot Accept.
use vstd::prelude::*;
use super::*;

verus! {

pub proof fn observation_has_per_row_source_provenance(c: Constants, s: State,
    generation: nat, id: int, key: Key)
    requires enabled(c,s,Action::Observe { generation,id,key })
    ensures apply(c,s,Action::Observe { generation,id,key }).packets[id].value
        == s.data[s.movement.unwrap().source.owner][key],
        apply(c,s,Action::Observe { generation,id,key }).data == s.data,
        apply(c,s,Action::Observe { generation,id,key }).logical == s.logical
{}

/// Placement safety of a pending private write does not demand freshness of
/// the packet's value, a current source scan snapshot, or an OCC validation.
pub proof fn outstanding_effect_is_private(c: Constants, s: State, id: int)
    requires constants(c), inv(c,s), s.effects.contains(id)
    ensures s.packets.contains_key(id), s.packets[id].destination != s.route.owner,
        apply(c,s,Action::FinishPrecopy { id }).data[s.route.owner] == s.data[s.route.owner],
        apply(c,s,Action::FinishPrecopy { id }).logical == s.logical
{
    assert(packet_current(s,s.packets[id]));
    assert(move_inv(c,s,s.movement.unwrap()));
}

pub proof fn accepted_packet_is_current(c: Constants, s: State, id: int)
    requires enabled(c,s,Action::AcceptPrecopy { id })
    ensures s.movement is Some,
        s.packets[id].generation == s.movement.unwrap().generation,
        s.packets[id].round == s.movement.unwrap().round,
        s.packets[id].destination == s.movement.unwrap().destination,
        s.movement.unwrap().precopy_open, precopy_phase(s.movement.unwrap())
{}

pub proof fn final_requires_join(c: Constants, s: State, generation: nat)
    requires constants(c), inv(c,s), enabled(c,s,Action::Copy { generation })
    ensures s.movement.unwrap().precopy_joined,
        !s.movement.unwrap().precopy_open, s.effects.is_empty()
{
    assert(move_inv(c,s,s.movement.unwrap()));
}

pub proof fn join_waits_for_effects(c: Constants, s: State, generation: nat)
    requires enabled(c,s,Action::JoinPrecopy { generation })
    ensures !s.movement.unwrap().precopy_open, s.effects.is_empty()
{}

pub proof fn old_packet_rejected(c: Constants, s: State, id: int)
    requires s.packets.contains_key(id), s.movement is None
        || s.packets[id].generation != s.movement.unwrap().generation
        || s.packets[id].round != s.movement.unwrap().round
        || !s.movement.unwrap().precopy_open || copied(s.movement.unwrap())
    ensures !enabled(c,s,Action::AcceptPrecopy { id })
{}

pub proof fn precopy_never_publishes(c: Constants, s: State, a: Action)
    requires a is StartPrecopy || a is Observe || a is AcceptPrecopy
        || a is FinishPrecopy || a is ClosePrecopy || a is JoinPrecopy
    ensures apply(c,s,a).route == s.route, apply(c,s,a).local == s.local,
        apply(c,s,a).logical == s.logical,
        s.movement is Some ==> apply(c,s,a).movement.unwrap().phase == s.movement.unwrap().phase
{}

pub proof fn step_preserves(c: Constants, s: State, a: Action)
    requires constants(c), inv(c,s), enabled(c,s,a),
        a is StartPrecopy || a is Observe || a is AcceptPrecopy
        || a is FinishPrecopy || a is ClosePrecopy || a is JoinPrecopy
    ensures inv(c,apply(c,s,a))
{
    let out = apply(c,s,a);
    precopy_never_publishes(c,s,a);
    match a {
        Action::StartPrecopy { .. } => {
            assert(move_inv(c,s,s.movement.unwrap()));
            assert(s.effects.is_empty());
        },
        Action::Observe { id,key,.. } => {
            assert forall|other: int| out.packets.contains_key(other) implies
                c.scope.contains(out.packets[other].key)
                && c.nodes.contains(out.packets[other].destination)
                && out.packets[other].generation < out.next_generation by {
                if other != id { assert(s.packets.contains_key(other)); }
                else { assert(move_inv(c,s,s.movement.unwrap())); }
            }
            assert forall|other: int| out.effects.contains(other) implies
                out.packets.contains_key(other) && packet_current(out,out.packets[other])
                && precopy_phase(out.movement.unwrap()) && !out.movement.unwrap().precopy_joined by {
                assert(s.effects.contains(other));
                assert(s.packets.contains_key(other));
                assert(other != id);
            }
        },
        Action::AcceptPrecopy { id } => {
            accepted_packet_is_current(c,s,id);
            assert(move_inv(c,s,s.movement.unwrap()));
            assert forall|other: int| out.effects.contains(other) implies
                out.packets.contains_key(other) && packet_current(out,out.packets[other])
                && precopy_phase(out.movement.unwrap()) && !out.movement.unwrap().precopy_joined by {
                if other != id { assert(s.effects.contains(other)); }
            }
        },
        Action::FinishPrecopy { id } => {
            let p = s.packets[id];
            assert(s.packets.contains_key(id));
            assert(packet_current(s,p));
            assert(move_inv(c,s,s.movement.unwrap()));
            assert(p.destination != s.route.owner);
            assert(s.data[p.destination].insert(p.key,p.value).dom() =~= c.scope);
            assert(out.data.dom() =~= c.nodes);
            assert forall|n: int| c.nodes.contains(n) implies out.data[n].dom() == c.scope by {
                assert(admission::inv(s.local[n]));
                assert(s.data[n].dom() == c.scope);
                if n != p.destination {
                    assert(out.data[n] == s.data[n]);
                } else {
                    assert(out.data[n] == s.data[p.destination].insert(p.key,p.value));
                }
            }
            assert forall|other: int| out.effects.contains(other) implies
                out.packets.contains_key(other) && packet_current(out,out.packets[other])
                && precopy_phase(out.movement.unwrap()) && !out.movement.unwrap().precopy_joined by {
                assert(s.effects.contains(other));
            }
        },
        Action::ClosePrecopy { .. } => {},
        Action::JoinPrecopy { generation } => { join_waits_for_effects(c,s,generation); },
        _ => {},
    }
    assert(inv(c,out));
}

/// Final replacement is over the full image/absence domain, even when a
/// private stale observation reintroduced a row deleted by a concurrent writer.
pub proof fn final_deletes_stale_private_row(c: Constants, s: State, generation: nat, key: Key)
    requires enabled(c,s,Action::Copy { generation }),
        s.data[s.movement.unwrap().source.owner].contains_key(key),
        s.data[s.movement.unwrap().source.owner][key] is None
    ensures apply(c,s,Action::Copy { generation }).data[s.movement.unwrap().destination][key] is None
{
    proofs::exact_copy_covers_absence(c,s,generation,key);
}

} // verus!
