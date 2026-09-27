//! The external correctness specifications, stated over client-observable
//! history only: which transactions were submitted (`invoked` tick), which
//! were acknowledged (`Committed`, `acked` tick), and what each acknowledged
//! transaction read. Nothing here mentions clocks, streams or watermarks.
use super::types::*;
use super::invariants::*;
use vstd::prelude::*;

verus! {

/// The value of key `k` after running the transactions of `order` serially,
/// one at a time, from the initial database (every key 0).
pub open spec fn serial_value(txns: Map<int, TxnRec>, k: int, order: Seq<int>) -> int
    decreases order.len()
{
    if order.len() == 0 { 0 } else {
        let prev = serial_value(txns, k, order.drop_last());
        let body = txns[order.last()].body;
        if body.ops.dom().contains(k) { apply_op(prev, body.ops[k]) } else { prev }
    }
}

/// `order` is a serial schedule that explains the history in `s`:
/// 1. it lists distinct submitted transactions that were not aborted
///    (acknowledged ones, and certified ones whose acknowledgment is pending
///    and which may therefore still take effect);
/// 2. every acknowledged transaction appears in it;
/// 3. it respects real time: a transaction acknowledged before another was
///    submitted comes first;
/// 4. every value an acknowledged transaction read (the results of its `Read`
///    and `Add` operations) is the value serial execution gives at its turn.
pub open spec fn serial_witness(s: State, order: Seq<int>) -> bool {
    &&& order.no_duplicates()
    &&& forall|j: int| 0 <= j < order.len() ==>
        s.txns.dom().contains(#[trigger] order[j]) && certified_or_committed(s.txns[order[j]])
    &&& forall|id: int| #[trigger] s.txns.dom().contains(id) && committed(s.txns[id]) ==> order.contains(id)
    &&& forall|a: int, b: int| 0 <= a < order.len() && 0 <= b < order.len()
        && committed(s.txns[#[trigger] order[a]]) && s.txns[order[a]].acked < s.txns[#[trigger] order[b]].invoked
        ==> a < b
    &&& forall|j: int, k: int| 0 <= j < order.len() && committed(s.txns[#[trigger] order[j]])
        && #[trigger] read_set(s.txns[order[j]].body).contains(k)
        ==> s.txns[order[j]].reads[k].value == serial_value(s.txns, k, order.take(j))
}

/// Strict serializability of the acknowledged history.
pub open spec fn strictly_serializable(s: State) -> bool {
    exists|order: Seq<int>| serial_witness(s, order)
}

} // verus!
