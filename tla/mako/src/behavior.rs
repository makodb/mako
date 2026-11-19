//! The complete MakoV2 transition system. No action tests a post-state
//! invariant, serializability predicate, or global durability postcondition.
use super::types::*;
use super::normal::*;
use super::recovery::*;
use vstd::prelude::*;

verus! {

pub enum Action {
    Submit { id: int, body: Txn, coord: int },
    Read { id: int, key: int },
    Prepare { id: int, ts: int },
    Install { id: int, shard: int },
    Publish { id: int, shard: int },
    Certify { id: int },
    Provisional { id: int },
    Final { id: int },
    Abort { id: int },
    Pulse { shard: int, ts: int },
    Barrier { shard: int, through: int },
    Replicate { shard: int },
    Send { shard: int, epoch: nat },
    Receive { observer: int, report: Report },
    ProposeEpoch,
    Crash { shard: int, survive: nat },
    Recover { shard: int, floor: int },
    ObserveEpoch { shard: int },
    Close { shard: int, epoch: nat },
    Rollback { shard: int, epoch: nat },
    Stutter,
}
pub open spec fn enabled(s: State, c: Constants, a: Action) -> bool {
    match a {
        Action::Submit { id, body, coord } => can_submit(s, c, id, body, coord),
        Action::Read { id, key } => can_read(s, c, id, key),
        Action::Prepare { id, ts } => can_prepare(s, c, id, ts),
        Action::Install { id, shard } => can_install(s, c, id, shard),
        Action::Publish { id, shard } => can_publish(s, c, id, shard),
        Action::Certify { id } => can_certify(s, c, id),
        Action::Provisional { id } => can_provisional(s, id),
        Action::Final { id } => can_final(s, c, id),
        Action::Abort { id } => can_abort(s, id),
        Action::Pulse { shard, ts } => can_pulse(s, c, shard, ts),
        Action::Barrier { shard, through } => can_barrier(s, c, shard, through),
        Action::Replicate { shard } => can_replicate(s, c, shard),
        Action::Send { shard, epoch } => can_send(s, c, shard, epoch),
        Action::Receive { observer, report } => can_receive(s, c, observer, report),
        Action::ProposeEpoch => can_propose_epoch(s),
        Action::Crash { shard, survive } => can_crash(s, c, shard, survive),
        Action::Recover { shard, floor } => can_recover(s, c, shard, floor),
        Action::ObserveEpoch { shard } => can_observe_epoch(s, c, shard),
        Action::Close { shard, epoch } => can_close(s, c, shard, epoch),
        Action::Rollback { shard, epoch } => can_rollback(s, c, shard, epoch),
        Action::Stutter => true,
    }
}
pub open spec fn apply(s: State, c: Constants, a: Action) -> State {
    let z = match a {
        Action::Submit { id, body, coord } => submit(s, id, body, coord),
        Action::Read { id, key } => read(s, id, key),
        Action::Prepare { id, ts } => prepare(s, c, id, ts),
        Action::Install { id, shard } => install(s, c, id, shard),
        Action::Publish { id, shard } => publish(s, id, shard),
        Action::Certify { id } => certify(s, id),
        Action::Provisional { id } => provisional(s, id),
        Action::Final { id } => acknowledge_final(s, id),
        Action::Abort { id } => abort(s, id),
        Action::Pulse { shard, ts } => pulse(s, shard, ts),
        Action::Barrier { shard, through } => barrier(s, shard, through),
        Action::Replicate { shard } => replicate(s, shard),
        Action::Send { shard, epoch } => send(s, shard, epoch),
        Action::Receive { observer, report } => receive(s, observer, report),
        Action::ProposeEpoch => propose_epoch(s),
        Action::Crash { shard, survive } => crash(s, c, shard, survive),
        Action::Recover { shard, floor } => recover(s, shard, floor),
        Action::ObserveEpoch { shard } => observe_epoch(s, c, shard),
        Action::Close { shard, epoch } => close(s, shard, epoch),
        Action::Rollback { shard, epoch } => rollback(s, c, shard, epoch),
        Action::Stutter => s,
    };
    State { tick: s.tick + 1, ..z }
}
pub open spec fn next(s: State, z: State, c: Constants) -> bool {
    exists|a: Action| #[trigger] enabled(s, c, a) && z == apply(s, c, a)
}
pub open spec fn behavior(states: Seq<State>, c: Constants) -> bool {
    states.len() > 0 && init(states[0], c)
        && forall|i: int| 0 <= i < states.len() - 1 ==> #[trigger] next(states[i], states[i + 1], c)
}

} // verus!
