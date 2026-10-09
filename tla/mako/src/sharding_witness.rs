//! Constructive executions of the corrected protocol and transaction composition.
//! Every progressing step below checks its operational guard, not just Next's
//! rejected-request stuttering case. Message receipt is distinct from delivery.
use vstd::prelude::*;
use super::sharding_placement as p;
use super::sharding_transactions as t;
#[path = "sharding_witness_prefix.rs"]
pub mod prefix;
#[path = "sharding_witness_finish.rs"]
pub mod finish;

verus! {

pub struct Execution { pub states: Seq<t::State>, pub actions: Seq<t::Action> }
pub open spec fn constants() -> p::Constants {
    p::Constants {
        keys: Set::empty().insert(0int).insert(1int).insert(2int),
        shards: Set::empty().insert(0int).insert(1int),
        table: Map::empty().insert(0int,0int).insert(1int,0int).insert(2int,1int),
        coordinate: Map::empty().insert(0int,0int).insert(1int,1int).insert(2int,0int),
        owners: Map::empty().insert(0int,0int).insert(1int,0int).insert(2int,1int),
    }
}
pub open spec fn valid(e: Execution) -> bool { t::behavior(constants(),e.states,e.actions) }
pub open spec fn last(e: Execution) -> t::State { e.states.last() }
pub open spec fn grant(owner: int, epoch: nat) -> p::Grant { p::Grant { owner, epoch } }
pub proof fn start() -> (e: Execution)
    ensures valid(e), last(e) == t::initial(constants()), p::constants_ok(constants())
{
    t::lemma_behavior_begin(constants());
    Execution { states: seq![t::initial(constants())], actions: Seq::empty() }
}
pub proof fn step(e: Execution, a: t::Action) -> (z: Execution)
    requires valid(e), t::enabled(constants(),last(e),a)
    ensures valid(z), last(z) == t::apply(constants(),last(e),a),
        t::enabled(constants(),last(e),a),
        z.states == e.states.push(t::apply(constants(),last(e),a)), z.actions == e.actions.push(a)
{
    t::lemma_behavior_extend(constants(),e.states,e.actions,a);
    Execution { states: e.states.push(t::apply(constants(),last(e),a)), actions: e.actions.push(a) }
}
pub proof fn network(e: Execution, a: p::Action) -> (z: Execution)
    requires valid(e), p::maintenance(a), p::enabled(constants(),last(e).placement,a)
    ensures valid(z), last(z) == t::apply(constants(),last(e),t::Action::Placement { action:a }),
        t::enabled(constants(),last(e),t::Action::Placement { action:a }),
        last(z).placement == p::apply(constants(),last(e).placement,a),
        last(z).txns == last(e).txns, last(z).order == last(e).order,
        last(z).tick == last(e).tick + 1
{
    step(e,t::Action::Placement { action:a })
}

/// Cross-shard Add and admitted read complete while frozen, blocking drain until
/// their leases are released. A stale background copy cannot overwrite final
/// bytes. Two independently delivered commit certificates activate/clean up;
/// the old route rejects a fresh session, and admin outcome survives lost reply.
pub proof fn witness_admitted_operations_and_handoff() -> (e: Execution)
    ensures valid(e),
        last(e).placement.logical[0].value == Some(15int),
        last(e).placement.logical[1].value == Some(11int),
        last(e).placement.logical[2].value == Some(20int),
        p::directory(last(e).placement)[0] == grant(1,1),
        p::directory(last(e).placement)[1] == grant(1,1),
        p::directory(last(e).placement)[2] == grant(1,0),
        last(e).txns[1].reads[0].value == Some(10int),
        last(e).txns[1].reads[2].value == Some(20int),
        last(e).txns[1].replied.is_some(),
        last(e).txns[2].reads[1].value == Some(11int), last(e).txns[2].replied.is_some(),
        last(e).txns[3].reads[0].value == Some(15int), last(e).txns[3].replied.is_some(),
        last(e).placement.outcomes[100].committed,
        last(e).placement.replies[100] == last(e).placement.outcomes[100],
        last(e).placement.active.is_none()
{
    hide(t::behavior);
    let e = prefix::sealed_execution();
    let e = finish::decide(e);
    let e = finish::activate_with_reader(e);
    let e = finish::finish_administration(e);
    let e = finish::finish_read(e);
    e
}

} // verus!
