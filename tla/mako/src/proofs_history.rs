//! A serial completion of every final history, including correct reads for
//! pending certified transactions selected in that completion.
use super::types::*;
use super::invariants::*;
use super::history::*;
use super::proofs_occ::{lemma_owner_is_shard, lemma_writes_at_owner};
use super::log_lemmas::*;
use vstd::prelude::*;

verus! {

pub open spec fn sel(s: State, c: Constants, id: int) -> bool {
    s.txns.dom().contains(id) && certified(txn(s.txns, id)) && stable(s.logs, c, txn(s.txns, id))
}
pub proof fn lemma_present_version(s: State, c: Constants, id: int, k: int)
    requires inv(s, c), sel(s, c, id), write_set(txn(s.txns, id).body).contains(k)
    ensures has_version(s.versions, k, id)
{
    let r = txn(s.txns, id);
    assert(inv_txn(s, c, id));
    lemma_writes_at_owner(c, r.body, k);
    if lost(s.logs, c, r, id) { lemma_lost_not_stable(s, c, id); }
    assert(inv_present(s, c, id));
}
pub proof fn lemma_writer_sel(s: State, c: Constants, id: int, k: int)
    requires inv(s, c), sel(s, c, id), txn(s.txns, id).reads.dom().contains(k),
        txn(s.txns, id).reads[k].writer != -1
    ensures sel(s, c, txn(s.txns, id).reads[k].writer),
        txn(s.txns, txn(s.txns, id).reads[k].writer).pidx < txn(s.txns, id).pidx
{
    let r = txn(s.txns, id);
    let w = r.reads[k].writer;
    let rw = txn(s.txns, w);
    assert(inv_read(s, c, id, k));
    assert(inv_read_writer(s, c, id, k));
    if rw.epoch == r.epoch {
        assert forall|i: int| is_shard(c, i) implies rw.ts <= frontier(#[trigger] s.logs[i].durable, rw.epoch) by {
            assert(r.ts <= frontier(s.logs[i].durable, r.epoch));
        }
    }
    lemma_stable_certified(s, c, w);
}
pub proof fn lemma_read_order_sel(s: State, c: Constants, id: int, k: int, other: int)
    requires inv(s, c), s.txns.dom().contains(id), prepared(txn(s.txns, id)),
        txn(s.txns, id).reads.dom().contains(k), sel(s, c, other),
        write_set(txn(s.txns, other).body).contains(k),
        txn(s.txns, other).pidx < txn(s.txns, id).pidx
    ensures txn(s.txns, other).pidx <= pidx(s.txns, txn(s.txns, id).reads[k].writer)
{
    assert(inv_read(s, c, id, k));
    assert(inv_read_order(s, c, id, k, other));
    assert(inv_txn(s, c, other));
    lemma_writes_at_owner(c, txn(s.txns, other).body, k);
    lemma_present_version(s, c, other, k);
}

pub open spec fn wit(s: State, c: Constants, n: int) -> Seq<int>
    decreases n
{
    if n <= 0 { Seq::empty() } else {
        let w = wit(s, c, n - 1);
        if sel(s, c, s.order[n - 1]) { w.push(s.order[n - 1]) } else { w }
    }
}
pub open spec fn good_order(s: State, c: Constants, order: Seq<int>) -> bool {
    &&& forall|a: int| 0 <= a < order.len() ==> sel(s, c, #[trigger] order[a])
    &&& forall|a: int, b: int| 0 <= a < b < order.len() ==>
        txn(s.txns, #[trigger] order[a]).pidx < txn(s.txns, #[trigger] order[b]).pidx
    &&& forall|id: int| #[trigger] sel(s, c, id) ==> order.contains(id)
}
pub proof fn lemma_wit(s: State, c: Constants, n: int)
    requires inv(s, c), 0 <= n <= s.order.len()
    ensures
        forall|a: int| 0 <= a < wit(s, c, n).len() ==>
            sel(s, c, #[trigger] wit(s, c, n)[a]) && txn(s.txns, wit(s, c, n)[a]).pidx < n,
        forall|a: int, b: int| 0 <= a < b < wit(s, c, n).len() ==>
            txn(s.txns, #[trigger] wit(s, c, n)[a]).pidx < txn(s.txns, #[trigger] wit(s, c, n)[b]).pidx,
        forall|j: int| 0 <= j < n && sel(s, c, #[trigger] s.order[j]) ==> wit(s, c, n).contains(s.order[j]),
    decreases n
{
    if n > 0 {
        lemma_wit(s, c, n - 1);
        let w = wit(s, c, n - 1);
        let id = s.order[n - 1];
        assert(txn(s.txns, id).pidx == n - 1);
        if sel(s, c, id) {
            let w2 = w.push(id);
            assert(wit(s, c, n) == w2);
            assert forall|a: int| 0 <= a < w2.len() implies
                sel(s, c, #[trigger] w2[a]) && txn(s.txns, w2[a]).pidx < n by {
                if a < w.len() { assert(w2[a] == w[a]); }
            }
            assert forall|a: int, b: int| 0 <= a < b < w2.len() implies
                txn(s.txns, #[trigger] w2[a]).pidx < txn(s.txns, #[trigger] w2[b]).pidx by {
                assert(w2[a] == w[a]);
                if b < w.len() { assert(w2[b] == w[b]); }
            }
            assert forall|j: int| 0 <= j < n && sel(s, c, #[trigger] s.order[j]) implies
                w2.contains(s.order[j]) by {
                if j < n - 1 {
                    assert(w.contains(s.order[j]));
                    let a = choose|a: int| 0 <= a < w.len() && w[a] == s.order[j];
                    assert(w2[a] == w[a]);
                } else { assert(w2[w.len() as int] == id); }
            }
        } else {
            assert(wit(s, c, n) == w);
            assert forall|j: int| 0 <= j < n && sel(s, c, #[trigger] s.order[j]) implies
                w.contains(s.order[j]) by {
                if j == n - 1 { assert(false); }
            }
        }
    }
}
pub proof fn lemma_good_order(s: State, c: Constants)
    requires inv(s, c)
    ensures good_order(s, c, wit(s, c, s.order.len() as int))
{
    let n = s.order.len() as int;
    lemma_wit(s, c, n);
    let order = wit(s, c, n);
    assert forall|id: int| #[trigger] sel(s, c, id) implies order.contains(id) by {
        assert(inv_txn(s, c, id));
        let j = txn(s.txns, id).pidx as int;
        assert(s.order[j] == id);
        assert(sel(s, c, s.order[j]));
    }
}
pub proof fn lemma_serial_skip(txns: Map<int, TxnRec>, k: int, q: Seq<int>, m: int)
    requires 0 <= m <= q.len(),
        forall|i: int| m <= i < q.len() ==> !write_set(txns[#[trigger] q[i]].body).contains(k)
    ensures serial_value(txns, k, q) == serial_value(txns, k, q.take(m))
    decreases q.len()
{
    if q.len() == m { assert(q.take(m) =~= q); } else {
        let q1 = q.drop_last();
        let l = q.last();
        assert(!write_set(txns[q[q.len() - 1]].body).contains(k));
        let body = txns[l].body;
        if body.ops.dom().contains(k) { assert(body.ops[k] is Read); }
        assert(serial_value(txns, k, q) == serial_value(txns, k, q1));
        assert forall|i: int| m <= i < q1.len() implies !write_set(txns[#[trigger] q1[i]].body).contains(k) by {
            assert(q1[i] == q[i]);
        }
        lemma_serial_skip(txns, k, q1, m);
        assert(q1.take(m) =~= q.take(m));
    }
}
pub proof fn lemma_serial_step(txns: Map<int, TxnRec>, k: int, q: Seq<int>, a: int)
    requires 0 <= a < q.len(), txns[q[a]].body.ops.dom().contains(k)
    ensures serial_value(txns, k, q.take(a + 1)) == apply_op(serial_value(txns, k, q.take(a)), txns[q[a]].body.ops[k])
{
    let t = q.take(a + 1);
    assert(t.drop_last() =~= q.take(a));
    assert(t.last() == q[a]);
}
pub proof fn lemma_result(s: State, c: Constants, order: Seq<int>, j: int, k: int)
    requires inv(s, c), good_order(s, c, order), 0 <= j < order.len(),
        read_set(txn(s.txns, order[j]).body).contains(k)
    ensures txn(s.txns, order[j]).reads[k].value == serial_value(s.txns, k, order.take(j))
    decreases j
{
    let id = order[j];
    let r = txn(s.txns, id);
    assert(sel(s, c, id));
    assert(inv_txn(s, c, id));
    assert(r.reads.dom().contains(k));
    assert(inv_read(s, c, id, k));
    let w = r.reads[k].writer;
    let q = order.take(j);
    if w == -1 {
        assert forall|i: int| 0 <= i < q.len() implies !write_set(s.txns[#[trigger] q[i]].body).contains(k) by {
            assert(q[i] == order[i]);
            let other = order[i];
            assert(sel(s, c, other));
            assert(txn(s.txns, order[i]).pidx < txn(s.txns, order[j]).pidx);
            if write_set(txn(s.txns, other).body).contains(k) { lemma_read_order_sel(s, c, id, k, other); }
        }
        lemma_serial_skip(s.txns, k, q, 0);
        assert(q.take(0).len() == 0);
    } else {
        lemma_writer_sel(s, c, id, k);
        let rw = txn(s.txns, w);
        assert(order.contains(w));
        let a = choose|a: int| 0 <= a < order.len() && order[a] == w;
        if a > j { assert(txn(s.txns, order[j]).pidx < txn(s.txns, order[a]).pidx); }
        assert(a < j);
        assert forall|i: int| a + 1 <= i < q.len() implies !write_set(s.txns[#[trigger] q[i]].body).contains(k) by {
            assert(q[i] == order[i]);
            let other = order[i];
            assert(sel(s, c, other));
            assert(txn(s.txns, order[a]).pidx < txn(s.txns, order[i]).pidx);
            assert(txn(s.txns, order[i]).pidx < txn(s.txns, order[j]).pidx);
            if write_set(txn(s.txns, other).body).contains(k) { lemma_read_order_sel(s, c, id, k, other); }
        }
        lemma_serial_skip(s.txns, k, q, a + 1);
        assert(q.take(a + 1) =~= order.take(a + 1));
        assert(inv_read_writer(s, c, id, k));
        assert(write_set(rw.body).contains(k));
        assert(rw.body.ops.dom().contains(k));
        lemma_serial_step(s.txns, k, order, a);
        if rw.body.ops[k] is Add {
            assert(read_set(rw.body).contains(k));
            lemma_result(s, c, order, a, k);
            assert(inv_txn(s, c, w));
            assert(rw.reads.dom().contains(k));
        }
    }
}
pub proof fn lemma_real_time(s: State, c: Constants, order: Seq<int>, a: int, b: int)
    requires inv(s, c), good_order(s, c, order), 0 <= a < order.len(), 0 <= b < order.len(),
        txn(s.txns, order[a]).status is Final, txn(s.txns, order[a]).final_at.unwrap() < txn(s.txns, order[b]).invoked
    ensures a < b
{
    let ta = order[a];
    let tb = order[b];
    assert(sel(s, c, ta));
    assert(sel(s, c, tb));
    assert(inv_txn(s, c, ta));
    assert(inv_txn(s, c, tb));
    if b < a {
        let ra = txn(s.txns, ta);
        let rb = txn(s.txns, tb);
        assert(rb.pidx < ra.pidx);
        assert(s.order[rb.pidx as int] == tb);
        assert(s.order[ra.pidx as int] == ta);
        assert(txn(s.txns, s.order[rb.pidx as int]).prepared_at < txn(s.txns, s.order[ra.pidx as int]).prepared_at);
    }
}
pub proof fn theorem_strictly_serializable(s: State, c: Constants)
    requires inv(s, c)
    ensures strictly_serializable(s)
{
    let order = wit(s, c, s.order.len() as int);
    lemma_good_order(s, c);
    assert(order.no_duplicates()) by {
        assert forall|a: int, b: int| 0 <= a < order.len() && 0 <= b < order.len() && a != b
            implies order[a] != order[b] by {
            if a < b { assert(txn(s.txns, order[a]).pidx < txn(s.txns, order[b]).pidx); }
            else { assert(txn(s.txns, order[b]).pidx < txn(s.txns, order[a]).pidx); }
        }
    }
    assert forall|j: int| 0 <= j < order.len() implies
        s.txns.dom().contains(#[trigger] order[j]) && certified(s.txns[order[j]]) by { assert(sel(s, c, order[j])); }
    assert forall|id: int| #[trigger] s.txns.dom().contains(id) && s.txns[id].status is Final
        implies order.contains(id) by { assert(sel(s, c, id)); }
    assert forall|a: int, b: int| 0 <= a < order.len() && 0 <= b < order.len()
        && s.txns[#[trigger] order[a]].status is Final
        && s.txns[order[a]].final_at.unwrap() < s.txns[#[trigger] order[b]].invoked
        implies a < b by { lemma_real_time(s, c, order, a, b); }
    assert forall|j: int, k: int| 0 <= j < order.len()
        && #[trigger] read_set(s.txns[#[trigger] order[j]].body).contains(k)
        implies s.txns[order[j]].reads.dom().contains(k)
            && s.txns[order[j]].reads[k].value == serial_value(s.txns, k, order.take(j)) by {
        assert(sel(s, c, order[j]));
        assert(inv_txn(s, c, order[j]));
        lemma_result(s, c, order, j, k);
    }
    assert(serial_witness(s, order));
}

pub proof fn theorem_durability(s: State, c: Constants, id: int)
    requires inv(s, c), s.txns.dom().contains(id), s.txns[id].status is Final
    ensures !lost(s.logs, c, s.txns[id], id), !doomed(s, c, s.txns[id]),
        forall|i: int| is_shard(c, i) && log_participant(c, s.txns[id], i) ==> has_log(#[trigger] s.logs[i].durable, id),
        forall|k: int| #[trigger] write_set(s.txns[id].body).contains(k) ==> has_version(s.versions, k, id)
{
    assert(inv_txn(s, c, id));
    if lost(s.logs, c, s.txns[id], id) { lemma_lost_not_stable(s, c, id); }
    assert forall|i: int| is_shard(c, i) && log_participant(c, s.txns[id], i)
        implies has_log(#[trigger] s.logs[i].durable, id) by { lemma_stable_durable(s, c, id, i); }
    assert forall|k: int| #[trigger] write_set(s.txns[id].body).contains(k)
        implies has_version(s.versions, k, id) by { lemma_present_version(s, c, id, k); }
}
pub proof fn theorem_dependency_rollback(s: State, c: Constants, id: int, k: int)
    requires inv(s, c), s.txns.dom().contains(id), prepared(s.txns[id]),
        s.txns[id].reads.dom().contains(k), s.txns[id].reads[k].writer != -1,
        doomed(s, c, s.txns[s.txns[id].reads[k].writer])
    ensures doomed(s, c, s.txns[id]), !(s.txns[id].status is Final)
{
    assert(inv_read(s, c, id, k));
    assert(inv_read_writer(s, c, id, k));
    let w = s.txns[s.txns[id].reads[k].writer];
    assert(w.epoch == s.txns[id].epoch);
    if stable(s.logs, c, s.txns[id]) {
        assert forall|i: int| is_shard(c, i) implies w.ts <= frontier(#[trigger] s.logs[i].durable, w.epoch) by {
            assert(s.txns[id].ts <= frontier(s.logs[i].durable, w.epoch));
        }
    }
}
pub proof fn theorem_rollback_removes_doomed(s: State, c: Constants, id: int, k: int)
    requires inv(s, c), s.txns.dom().contains(id), doomed(s, c, s.txns[id]),
        write_set(s.txns[id].body).contains(k), s.rolled_back.contains((owner(c, k), s.txns[id].epoch))
    ensures !has_version(s.versions, k, id)
{
    assert(inv_txn(s, c, id));
    lemma_writes_at_owner(c, s.txns[id].body, k);
    if has_version(s.versions, k, id) {
        let j = choose|j: int| 0 <= j < vers(s.versions, k).len() && #[trigger] vers(s.versions, k)[j].txn == id;
        assert(s.versions.dom().contains(k));
        assert(inv_versions_of(s, c, k));
        assert(inv_version(s, c, k, s.versions[k][j]));
    }
}

/// A covered transaction cannot retain merely a subset of its writes: its
/// coordinator certified all installs and every participating log is durable.
pub proof fn theorem_atomicity(s: State, c: Constants, id: int)
    requires inv(s, c), s.txns.dom().contains(id), prepared(s.txns[id]), stable(s.logs, c, s.txns[id])
    ensures certified(s.txns[id]), all_installed(c, s.txns[id]),
        forall|k: int| #[trigger] write_set(s.txns[id].body).contains(k) ==> has_version(s.versions, k, id),
        forall|i: int| is_shard(c, i) && log_participant(c, s.txns[id], i) ==> has_log(#[trigger] s.logs[i].durable, id)
{
    lemma_stable_certified(s, c, id);
    assert forall|k: int| #[trigger] write_set(s.txns[id].body).contains(k)
        implies has_version(s.versions, k, id) by { lemma_present_version(s, c, id, k); }
    assert forall|i: int| is_shard(c, i) && log_participant(c, s.txns[id], i)
        implies has_log(#[trigger] s.logs[i].durable, id) by { lemma_stable_durable(s, c, id, i); }
}

} // verus!
