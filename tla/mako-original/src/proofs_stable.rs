//! Acknowledgment is final: once a transaction is Committed its record (and so
//! its results, `invoked` and `acked` ticks) never changes. Together with the
//! per-state strict-serializability theorem this means every acknowledged
//! result stays explained by a serial order forever.
use super::types::*;
use super::normal::*;
use super::recovery::*;
use super::behavior::*;
use super::invariants::*;
use vstd::prelude::*;

verus! {

pub proof fn lemma_step_keeps_committed(s: State, c: Constants, a: Action, id: int)
    requires enabled(s, c, a), s.txns.dom().contains(id), committed(s.txns[id])
    ensures apply(s, c, a).txns.dom().contains(id), apply(s, c, a).txns[id] == s.txns[id]
{
    let s2 = apply(s, c, a);
    match a {
        Action::Crash { shard, survive } => {
            assert(s2.txns == abort_coordinated(s.txns, shard));
        }
        Action::AdvanceEpoch { shard } => {
            assert(s2.txns == abort_coordinated(s.txns, shard));
        }
        _ => {}
    }
}

pub proof fn theorem_ack_is_final(states: Seq<State>, c: Constants, i: int, j: int, id: int)
    requires behavior(states, c), 0 <= i <= j < states.len(),
        states[i].txns.dom().contains(id), committed(states[i].txns[id])
    ensures states[j].txns.dom().contains(id), states[j].txns[id] == states[i].txns[id]
    decreases j - i
{
    if i < j {
        theorem_ack_is_final(states, c, i, j - 1, id);
        let p = j - 1;
        assert(next(states[p], states[p + 1], c));
        assert(states[p + 1] == states[j]);
        let a = choose|a: Action| #[trigger] enabled(states[j - 1], c, a) && states[j] == apply(states[j - 1], c, a);
        lemma_step_keeps_committed(states[j - 1], c, a, id);
    }
}

} // verus!
