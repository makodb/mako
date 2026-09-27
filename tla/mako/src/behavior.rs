//! The transition relation and behaviors.
use super::types::*;
use super::normal::*;
use super::recovery::*;
use vstd::prelude::*;

verus! {

pub enum Action {
    Submit { id: int, body: Txn, coord: int, thread: int },
    Read { id: int, key: int },
    Prepare { id: int, vc: Seq<int> },
    Install { id: int, shard: int },
    Certify { id: int },
    Commit { id: int },
    Abort { id: int },
    Replicate { sid: Sid },
    Crash { shard: int, survive: Map<Sid, nat> },
    AdvanceEpoch { shard: int },
    CloseEpoch { shard: int, epoch: nat, wm: Wm },
    Rollback { shard: int, epoch: nat },
    Stutter,
}

pub open spec fn enabled(s: State, c: Constants, a: Action) -> bool {
    match a {
        Action::Submit { id, body, coord, thread } => can_submit(s, c, id, body, coord, thread),
        Action::Read { id, key } => can_read(s, c, id, key),
        Action::Prepare { id, vc } => can_prepare(s, c, id, vc),
        Action::Install { id, shard } => can_install(s, c, id, shard),
        Action::Certify { id } => can_certify(s, c, id),
        Action::Commit { id } => can_commit(s, c, id),
        Action::Abort { id } => can_abort(s, id),
        Action::Replicate { sid } => can_replicate(s, c, sid),
        Action::Crash { shard, survive } => can_crash(s, c, shard, survive),
        Action::AdvanceEpoch { shard } => can_advance(s, c, shard),
        Action::CloseEpoch { shard, epoch, wm } => can_close(s, c, shard, epoch, wm),
        Action::Rollback { shard, epoch } => can_rollback(s, c, shard, epoch),
        Action::Stutter => true,
    }
}

pub open spec fn apply(s: State, c: Constants, a: Action) -> State {
    let after = match a {
        Action::Submit { id, body, coord, thread } => submit(s, c, id, body, coord, thread),
        Action::Read { id, key } => read(s, c, id, key),
        Action::Prepare { id, vc } => prepare(s, c, id, vc),
        Action::Install { id, shard } => install(s, c, id, shard),
        Action::Certify { id } => certify(s, c, id),
        Action::Commit { id } => commit(s, c, id),
        Action::Abort { id } => abort(s, id),
        Action::Replicate { sid } => replicate(s, c, sid),
        Action::Crash { shard, survive } => crash(s, c, shard, survive),
        Action::AdvanceEpoch { shard } => advance(s, c, shard),
        Action::CloseEpoch { shard, epoch, wm } => close(s, shard, epoch, wm),
        Action::Rollback { shard, epoch } => rollback(s, c, shard, epoch),
        Action::Stutter => s,
    };
    State { tick: s.tick + 1, ..after }
}

pub open spec fn next(s: State, s_: State, c: Constants) -> bool {
    exists|a: Action| #[trigger] enabled(s, c, a) && s_ == apply(s, c, a)
}

pub open spec fn behavior(states: Seq<State>, c: Constants) -> bool {
    &&& states.len() > 0
    &&& init(states[0], c)
    &&& forall|i: int| 0 <= i < states.len() - 1 ==> #[trigger] next(states[i], states[i + 1], c)
}

} // verus!
