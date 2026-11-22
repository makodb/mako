//! Unbounded finite-history induction for the complete MakoV2 action system.
//! Every invariant conjunct is established initially and preserved by every
//! enabled action; no invariant is assumed by an action's enabling condition.
use super::types::*;
use super::behavior::*;
use super::invariants::*;
use super::history::*;
use super::proofs_occ::{lemma_occ_init, lemma_occ_step};
use super::proofs_replication::{lemma_log_init, lemma_log_step};
use super::proofs_history::{theorem_strictly_serializable, theorem_durability,
    theorem_atomicity, theorem_rollback_removes_doomed};
use vstd::prelude::*;

verus! {

pub proof fn lemma_init(s: State, c: Constants)
    requires init(s, c)
    ensures inv(s, c)
{
    lemma_log_init(s, c);
    lemma_occ_init(s, c);
}
pub proof fn lemma_step(s: State, c: Constants, a: Action)
    requires inv(s, c), enabled(s, c, a)
    ensures inv(apply(s, c, a), c)
{
    lemma_log_step(s, c, a);
    lemma_occ_step(s, c, a);
}
pub proof fn theorem_invariant(states: Seq<State>, c: Constants, n: int)
    requires behavior(states, c), 0 <= n < states.len()
    ensures inv(states[n], c)
    decreases n
{
    if n == 0 {
        lemma_init(states[0], c);
    } else {
        let p = n - 1;
        theorem_invariant(states, c, p);
        assert(next(states[p], states[p + 1], c));
        let a = choose|a: Action| #[trigger] enabled(states[p], c, a)
            && states[p + 1] == apply(states[p], c, a);
        lemma_step(states[p], c, a);
    }
}

/// All final client histories have a legal serial completion, including
/// correct reads for every selected pending operation and real-time order.
pub proof fn theorem_makov2_strictly_serializable(states: Seq<State>, c: Constants)
    requires behavior(states, c)
    ensures forall|n: int| 0 <= n < states.len() ==> strictly_serializable(#[trigger] states[n])
{
    assert forall|n: int| 0 <= n < states.len() implies strictly_serializable(#[trigger] states[n]) by {
        theorem_invariant(states, c, n);
        theorem_strictly_serializable(states[n], c);
    }
}

pub proof fn theorem_reachable_final_durability(states: Seq<State>, c: Constants, n: int, id: int)
    requires behavior(states, c), 0 <= n < states.len(),
        states[n].txns.dom().contains(id), states[n].txns[id].status is Final
    ensures !lost(states[n].logs, c, states[n].txns[id], id), !doomed(states[n], c, states[n].txns[id]),
        forall|i: int| is_shard(c, i) && log_participant(c, states[n].txns[id], i)
            ==> has_log(#[trigger] states[n].logs[i].durable, id),
        forall|k: int| #[trigger] write_set(states[n].txns[id].body).contains(k)
            ==> has_version(states[n].versions, k, id)
{
    theorem_invariant(states, c, n);
    theorem_durability(states[n], c, id);
}

/// After every participant closes the epoch, a bound transaction is either
/// globally covered with every write present, or doomed with no surviving
/// write at any shard that has completed rollback. Rollback itself is local,
/// not falsely modeled as an atomic operation across all shards.
pub proof fn theorem_epoch_resolution(s: State, c: Constants, id: int)
    requires inv(s, c), s.txns.dom().contains(id), prepared(s.txns[id]),
        forall|i: int| is_shard(c, i) ==> has_close(#[trigger] s.logs[i].durable, s.txns[id].epoch)
    ensures
        stable(s.logs, c, s.txns[id]) ==> certified(s.txns[id])
            && (forall|i: int| is_shard(c, i) && log_participant(c, s.txns[id], i)
                ==> has_log(#[trigger] s.logs[i].durable, id))
            && (forall|k: int| #[trigger] write_set(s.txns[id].body).contains(k)
                ==> has_version(s.versions, k, id)),
        !stable(s.logs, c, s.txns[id]) ==> doomed(s, c, s.txns[id])
            && !(s.txns[id].status is Final)
            && (forall|k: int| #[trigger] write_set(s.txns[id].body).contains(k)
                && s.rolled_back.contains((owner(c, k), s.txns[id].epoch))
                ==> !has_version(s.versions, k, id)),
{
    assert(inv_txn(s, c, id));
    if stable(s.logs, c, s.txns[id]) {
        theorem_atomicity(s, c, id);
    } else {
        assert(doomed(s, c, s.txns[id]));
        assert forall|k: int| #[trigger] write_set(s.txns[id].body).contains(k)
            && s.rolled_back.contains((owner(c, k), s.txns[id].epoch))
            implies !has_version(s.versions, k, id) by {
            theorem_rollback_removes_doomed(s, c, id, k);
        }
    }
}

} // verus!
