//! Final client responses are immutable across all later failures, epoch
//! changes, rollbacks and delayed reports. Provisional responses are separate.
use super::types::*;
use super::normal::*;
use super::recovery::*;
use super::behavior::*;
use vstd::prelude::*;

verus! {

pub proof fn lemma_final_record_unchanged(s: State, c: Constants, a: Action, id: int)
    requires enabled(s, c, a), s.txns.dom().contains(id), s.txns[id].status is Final
    ensures apply(s, c, a).txns.dom().contains(id), apply(s, c, a).txns[id] == s.txns[id]
{
    match a {
        Action::Submit { id: other, .. } => { assert(other != id); },
        Action::Read { id: other, .. } => { assert(other != id); },
        Action::Prepare { id: other, .. } => { assert(other != id); },
        Action::Install { id: other, .. } => { assert(other != id); },
        Action::Publish { id: other, .. } => { assert(other != id); },
        Action::Certify { id: other } => { assert(other != id); },
        Action::Provisional { id: other } => { assert(other != id); },
        Action::Final { id: other } => { assert(other != id); },
        Action::Abort { id: other } => { assert(other != id); },
        Action::Crash { shard, .. } => {
            assert(abort_coordinated(s.txns, shard)[id] == s.txns[id]);
        },
        Action::ObserveEpoch { shard } => {
            assert(abort_coordinated(s.txns, shard)[id] == s.txns[id]);
        },
        _ => {},
    }
}

pub proof fn theorem_final_is_irrevocable(states: Seq<State>, c: Constants, i: int, j: int, id: int)
    requires behavior(states, c), 0 <= i <= j < states.len(),
        states[i].txns.dom().contains(id), states[i].txns[id].status is Final
    ensures states[j].txns.dom().contains(id), states[j].txns[id] == states[i].txns[id]
    decreases j - i
{
    if i < j {
        theorem_final_is_irrevocable(states, c, i, j - 1, id);
        let p = j - 1;
        assert(next(states[p], states[p + 1], c));
        assert(states[p + 1] == states[j]);
        let a = choose|a: Action| #[trigger] enabled(states[j - 1], c, a)
            && states[j] == apply(states[j - 1], c, a);
        lemma_final_record_unchanged(states[j - 1], c, a, id);
    }
}

} // verus!
