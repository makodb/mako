//! Client-visible strict serializability: final responses, operation bodies,
//! observed reads and invocation/response order. Pending certified operations
//! may occur in the completion; every selected operation must read correctly.
use super::types::*;
use vstd::prelude::*;

verus! {

pub open spec fn serial_value(txns: Map<int, TxnRec>, k: int, order: Seq<int>) -> int
    decreases order.len()
{
    if order.len() == 0 { 0 } else {
        let prev = serial_value(txns, k, order.drop_last());
        let body = txns[order.last()].body;
        if body.ops.dom().contains(k) { apply_op(prev, body.ops[k]) } else { prev }
    }
}

/// A legal completion contains every final transaction, may contain pending
/// certified transactions, never contains an aborted transaction, and explains
/// ALL of its reads by one serial execution respecting final-before-invoke.
pub open spec fn serial_witness(s: State, order: Seq<int>) -> bool {
    &&& order.no_duplicates()
    &&& forall|j: int| 0 <= j < order.len() ==>
        s.txns.dom().contains(#[trigger] order[j]) && certified(s.txns[order[j]])
    &&& forall|id: int| #[trigger] s.txns.dom().contains(id) && s.txns[id].status is Final ==> order.contains(id)
    &&& forall|a: int, b: int| 0 <= a < order.len() && 0 <= b < order.len()
        && s.txns[#[trigger] order[a]].status is Final
        && s.txns[order[a]].final_at.unwrap() < s.txns[#[trigger] order[b]].invoked ==> a < b
    &&& forall|j: int, k: int| 0 <= j < order.len()
        && #[trigger] read_set(s.txns[#[trigger] order[j]].body).contains(k)
        ==> s.txns[order[j]].reads.dom().contains(k)
            && s.txns[order[j]].reads[k].value == serial_value(s.txns, k, order.take(j))
}
pub open spec fn strictly_serializable(s: State) -> bool {
    exists|order: Seq<int>| serial_witness(s, order)
}

} // verus!
