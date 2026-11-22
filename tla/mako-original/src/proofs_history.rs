//! Per-state safety theorems, derived from the inductive invariant alone:
//! strict serializability of the acknowledged history (history.rs),
//! durability of committed transactions, atomicity (paper Theorem 1 with
//! Lemma 6) and rollback safety (paper Theorem 2 with Lemmas 7-8).
//!
//! The serial witness is the certification order `s.prepared` restricted to
//! the transactions that are certified (or committed) and already below the
//! vector watermark of their epoch (`sel`). Such a transaction is never lost
//! and never doomed, the writer it read from is again in `sel` and precedes
//! it, and no `sel` transaction between that writer and the reader writes the
//! key; so each read value is what serial execution of the witness produces.
use super::types::*;
use super::normal::*;
use super::recovery::*;
use super::invariants::*;
use super::stream_lemmas::*;
use super::history::*;
use vstd::prelude::*;

verus! {

// ---------------------------------------------------------------------------
// Small facts
// ---------------------------------------------------------------------------

pub proof fn lemma_owner_is_shard(c: Constants, k: int)
    requires valid_constants(c), valid_key(k)
    ensures is_shard(c, owner(c, k))
{
    vstd::arithmetic::div_mod::lemma_mod_pos_bound(k, c.shards);
}

/// The owner of a written key is a shard the transaction writes at.
pub proof fn lemma_writes_at_owner(c: Constants, t: Txn, k: int)
    requires valid_constants(c), valid_txn(t), write_set(t).contains(k)
    ensures is_shard(c, owner(c, k)), writes_at(c, t, owner(c, k)), !write_set(t).is_empty()
{
    assert(t.ops.dom().contains(k));
    lemma_owner_is_shard(c, k);
    if write_set(t).is_empty() {
        assert(!Set::<int>::empty().contains(k));
    }
}

/// `stream_below` of all entries restricts to the durable prefix.
pub proof fn lemma_stream_below_durable(st: Stream, e: nat, x: int)
    requires stream_below(all_entries(st), e, x)
    ensures stream_below(st.durable, e, x)
{
    let es = all_entries(st);
    let d = st.durable;
    assert forall|a: int| 0 <= a < d.len() && #[trigger] d[a] is Log && d[a]->Log_epoch == e
        implies d[a]->Log_clock < x by {
        assert(es[a] == d[a]);
    }
    assert forall|a: int| 0 <= a < d.len() implies !(#[trigger] d[a] is Inf && d[a]->Inf_epoch == e) by {
        assert(es[a] == d[a]);
    }
}

/// A clock the stream can never reach is not below its durable watermark.
pub proof fn lemma_stream_below_not_wm(st: Stream, e: nat, x: int)
    requires stream_below(all_entries(st), e, x), x >= 1
    ensures !wm_le(x, stream_wm(st.durable, e))
{
    lemma_stream_below_durable(st, e, x);
    lemma_stream_below_wm(st.durable, e, x);
}

// ---------------------------------------------------------------------------
// Vector watermark facts
// ---------------------------------------------------------------------------

/// Lemma 1 lifted to the watermark: a smaller clock vector is below it too.
pub proof fn lemma_below_wm_vc_le(streams: Map<Sid, Stream>, c: Constants, a: Seq<int>, b: Seq<int>, e: nat)
    requires valid_constants(c), vc_le(a, b, c.comp), below_wm(streams, c, b, e)
    ensures below_wm(streams, c, a, e)
{
    assert forall|sid: Sid| valid_sid(c, sid) implies
        wm_le(a[group(c, sid.shard)], stream_wm(#[trigger] streams[sid].durable, e)) by {
        let g = group(c, sid.shard);
        assert(0 <= c.cidx[sid.shard] < c.comp);
        assert(a[g] <= b[g]);
        assert(wm_le(b[g], stream_wm(streams[sid].durable, e)));
    }
}

/// Below the finalized vector watermark implies below the vector watermark
/// (the FVW is the epoch's maximum watermark, Lemma 4).
pub proof fn lemma_below_wm_of_fvw(s: State, c: Constants, vc: Seq<int>, e: nat)
    requires inv(s, c), fvw_ready(s.final_wm, c, e), below_fvw(s.final_wm, c, vc, e)
    ensures below_wm(s.streams, c, vc, e)
{
    assert forall|sid: Sid| valid_sid(c, sid) implies
        wm_le(vc[group(c, sid.shard)], stream_wm(#[trigger] s.streams[sid].durable, e)) by {
        let i = sid.shard;
        assert(s.final_wm.dom().contains((i, e)));
        assert(inv_final_of(s, c, i, e));
        assert(no_pending_epoch(s.streams[sid], e)
            && wm_le_wm(s.final_wm[(i, e)], stream_wm(s.streams[sid].durable, e)));
        assert(wm_le(vc[group(c, i)], s.final_wm[(i, e)]));
        lemma_wm_le_int_trans(vc[group(c, i)], s.final_wm[(i, e)], stream_wm(s.streams[sid].durable, e));
    }
}

/// Below the vector watermark implies not doomed: every finalized shard
/// watermark is the watermark of one of the shard's streams.
pub proof fn lemma_below_wm_not_doomed(s: State, c: Constants, r: TxnRec)
    requires inv(s, c), below_wm(s.streams, c, r.vc, r.epoch)
    ensures !doomed(s.final_wm, c, r)
{
    let e = r.epoch;
    if fvw_ready(s.final_wm, c, e) {
        assert forall|i: int| is_shard(c, i) implies wm_le(r.vc[group(c, i)], #[trigger] s.final_wm[(i, e)]) by {
            assert(s.final_wm.dom().contains((i, e)));
            assert(inv_final_of(s, c, i, e));
            let sid = choose|sid: Sid| valid_sid(c, sid) && sid.shard == i
                && s.final_wm[(i, e)] == stream_wm(#[trigger] s.streams[sid].durable, e);
            assert(wm_le(r.vc[group(c, sid.shard)], stream_wm(s.streams[sid].durable, e)));
        }
    }
}

/// Doomedness propagates along a same-epoch clock order (Theorem 2, same epoch).
pub proof fn lemma_doomed_vc_le(fw: Map<(int, nat), Wm>, c: Constants, r0: TxnRec, r1: TxnRec)
    requires valid_constants(c), r0.epoch == r1.epoch, vc_le(r0.vc, r1.vc, c.comp), doomed(fw, c, r0)
    ensures doomed(fw, c, r1)
{
    let e = r0.epoch;
    if below_fvw(fw, c, r1.vc, e) {
        assert forall|i: int| is_shard(c, i) implies wm_le(r0.vc[group(c, i)], #[trigger] fw[(i, e)]) by {
            assert(0 <= c.cidx[i] < c.comp);
            assert(r0.vc[group(c, i)] <= r1.vc[group(c, i)]);
            assert(wm_le(r1.vc[group(c, i)], fw[(i, e)]));
        }
    }
}

// ---------------------------------------------------------------------------
// Lemma A: a prepared-or-later transaction below the watermark is certified,
// not lost and not doomed.
// ---------------------------------------------------------------------------

pub proof fn lemma_below_wm_not_lost(s: State, c: Constants, id: int)
    requires inv(s, c), s.txns.dom().contains(id), is_prepared_or_later(txn(s.txns, id)),
        below_wm(s.streams, c, txn(s.txns, id).vc, txn(s.txns, id).epoch)
    ensures !lost(s.streams, c, txn(s.txns, id), id)
{
    let r = txn(s.txns, id);
    assert(inv_txn(s, c, id));
    assert(inv_logs(s, c, id));
    if lost(s.streams, c, r, id) {
        let i = choose|i: int| is_shard(c, i) && #[trigger] logged_at(c, r, i) && !has_log(stream_at(s.streams, r, i), id);
        assert(inv_logged(s, c, r, id));
        let sid = coord_sid(r, i);
        assert(valid_sid(c, sid));
        let x = r.vc[group(c, i)];
        assert(clock_shard(c, r, i));
        assert(x >= 1);
        assert(stream_below(all_entries(s.streams[sid]), r.epoch, x));
        lemma_stream_below_not_wm(s.streams[sid], r.epoch, x);
        assert(wm_le(r.vc[group(c, sid.shard)], stream_wm(s.streams[sid].durable, r.epoch)));
    }
}

pub proof fn lemma_below_wm_certified(s: State, c: Constants, id: int)
    requires inv(s, c), s.txns.dom().contains(id), is_prepared_or_later(txn(s.txns, id)),
        !read_only(txn(s.txns, id)),
        below_wm(s.streams, c, txn(s.txns, id).vc, txn(s.txns, id).epoch)
    ensures certified_or_committed(txn(s.txns, id))
{
    let r = txn(s.txns, id);
    assert(inv_txn(s, c, id));
    if !certified_or_committed(r) {
        assert(r.status is Prepared || aborted_prepared(r));
        assert(clock_shard(c, r, r.coord));
        let x = r.vc[group(c, r.coord)];
        assert(x >= 1);
        assert(inv_logs(s, c, id));
        assert(inv_coord_below(s, c, r));
        let sid = coord_sid(r, r.coord);
        assert(valid_sid(c, sid));
        lemma_stream_below_not_wm(s.streams[sid], r.epoch, x);
        assert(wm_le(r.vc[group(c, sid.shard)], stream_wm(s.streams[sid].durable, r.epoch)));
    }
}

/// A certified transaction that is neither doomed nor lost still has every
/// one of its versions.
pub proof fn lemma_present_version(s: State, c: Constants, id: int, k: int)
    requires inv(s, c), s.txns.dom().contains(id), certified_or_committed(txn(s.txns, id)),
        !doomed(s.final_wm, c, txn(s.txns, id)), !lost(s.streams, c, txn(s.txns, id), id),
        write_set(txn(s.txns, id).body).contains(k)
    ensures has_version(s.versions, k, id)
{
    let r = txn(s.txns, id);
    assert(inv_txn(s, c, id));
    lemma_writes_at_owner(c, r.body, k);
    assert(all_installed(c, r));
    assert(r.installed.contains(owner(c, k)));
    assert(inv_logs(s, c, id));
    assert(inv_present(s, c, r, id));
}

// ---------------------------------------------------------------------------
// Lemma B: dependencies of a transaction below the watermark
// ---------------------------------------------------------------------------

/// Transactions of the serial witness.
pub open spec fn sel(s: State, c: Constants, id: int) -> bool {
    &&& s.txns.dom().contains(id)
    &&& certified_or_committed(txn(s.txns, id))
    &&& below_wm(s.streams, c, txn(s.txns, id).vc, txn(s.txns, id).epoch)
}

/// The writer a reader below the watermark read from is in the witness and
/// was certified before it.
pub proof fn lemma_writer_sel(s: State, c: Constants, p: int, k: int)
    requires inv(s, c), s.txns.dom().contains(p), is_prepared_or_later(txn(s.txns, p)),
        below_wm(s.streams, c, txn(s.txns, p).vc, txn(s.txns, p).epoch),
        txn(s.txns, p).reads.dom().contains(k),
        txn(s.txns, p).reads[k].writer != -1,
    ensures sel(s, c, txn(s.txns, p).reads[k].writer),
        txn(s.txns, txn(s.txns, p).reads[k].writer).pidx < txn(s.txns, p).pidx,
{
    let r = txn(s.txns, p);
    let w = r.reads[k].writer;
    let rw = txn(s.txns, w);
    assert(inv_read(s, c, p, k));
    assert(inv_read_writer(s, c, p, k));
    assert(inv_txn(s, c, w));
    if rw.epoch == r.epoch {
        lemma_below_wm_vc_le(s.streams, c, rw.vc, r.vc, r.epoch);
    } else {
        lemma_below_wm_of_fvw(s, c, rw.vc, rw.epoch);
    }
    lemma_writes_at_owner(c, rw.body, k);
    lemma_below_wm_certified(s, c, w);
}

/// Every witness transaction certified before reader `p` that writes `k` was
/// certified no later than the writer `p` read from.
pub proof fn lemma_read_order_sel(s: State, c: Constants, p: int, k: int, q: int)
    requires inv(s, c), s.txns.dom().contains(p), is_prepared_or_later(txn(s.txns, p)),
        txn(s.txns, p).reads.dom().contains(k),
        sel(s, c, q), q != p, write_set(txn(s.txns, q).body).contains(k),
        txn(s.txns, q).pidx < txn(s.txns, p).pidx,
    ensures txn(s.txns, q).pidx <= pidx_of(s.txns, txn(s.txns, p).reads[k].writer)
{
    let rq = txn(s.txns, q);
    assert(inv_read(s, c, p, k));
    assert(inv_read_order(s, c, p, k, q));
    assert(inv_txn(s, c, q));
    assert(q >= 0);
    lemma_writes_at_owner(c, rq.body, k);
    assert(all_installed(c, rq));
    assert(rq.installed.contains(owner(c, k)));
    lemma_below_wm_not_doomed(s, c, rq);
    lemma_below_wm_not_lost(s, c, q);
    lemma_present_version(s, c, q, k);
}

// ---------------------------------------------------------------------------
// The serial witness
// ---------------------------------------------------------------------------

/// The first `n` entries of the certification order, restricted to `sel`.
pub open spec fn wit(s: State, c: Constants, n: int) -> Seq<int>
    decreases n
{
    if n <= 0 { Seq::empty() } else {
        let w = wit(s, c, n - 1);
        if sel(s, c, s.prepared[n - 1]) { w.push(s.prepared[n - 1]) } else { w }
    }
}

pub open spec fn good_order(s: State, c: Constants, order: Seq<int>) -> bool {
    &&& forall|a: int| 0 <= a < order.len() ==> sel(s, c, #[trigger] order[a])
    &&& forall|a: int, b: int| 0 <= a < b < order.len() ==>
        txn(s.txns, #[trigger] order[a]).pidx < txn(s.txns, #[trigger] order[b]).pidx
    &&& forall|id: int| #[trigger] sel(s, c, id) ==> order.contains(id)
}

pub proof fn lemma_wit(s: State, c: Constants, n: int)
    requires inv(s, c), 0 <= n <= s.prepared.len()
    ensures
        forall|a: int| 0 <= a < wit(s, c, n).len() ==>
            sel(s, c, #[trigger] wit(s, c, n)[a]) && txn(s.txns, wit(s, c, n)[a]).pidx < n,
        forall|a: int, b: int| 0 <= a < b < wit(s, c, n).len() ==>
            txn(s.txns, #[trigger] wit(s, c, n)[a]).pidx < txn(s.txns, #[trigger] wit(s, c, n)[b]).pidx,
        forall|j: int| 0 <= j < n && sel(s, c, #[trigger] s.prepared[j]) ==> wit(s, c, n).contains(s.prepared[j]),
    decreases n
{
    if n > 0 {
        lemma_wit(s, c, n - 1);
        let w = wit(s, c, n - 1);
        let id = s.prepared[n - 1];
        assert(txn(s.txns, s.prepared[n - 1]).pidx == n - 1);
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
            assert forall|j: int| 0 <= j < n && sel(s, c, #[trigger] s.prepared[j]) implies
                w2.contains(s.prepared[j]) by {
                if j < n - 1 {
                    assert(w.contains(s.prepared[j]));
                    let a = choose|a: int| 0 <= a < w.len() && w[a] == s.prepared[j];
                    assert(w2[a] == w[a]);
                } else {
                    assert(w2[w.len() as int] == id);
                }
            }
        } else {
            assert(wit(s, c, n) == w);
            assert forall|j: int| 0 <= j < n && sel(s, c, #[trigger] s.prepared[j]) implies
                w.contains(s.prepared[j]) by {
                if j == n - 1 { assert(false); }
            }
        }
    }
}

pub proof fn lemma_good_order(s: State, c: Constants)
    requires inv(s, c)
    ensures good_order(s, c, wit(s, c, s.prepared.len() as int))
{
    let n = s.prepared.len() as int;
    lemma_wit(s, c, n);
    let order = wit(s, c, n);
    assert forall|id: int| #[trigger] sel(s, c, id) implies order.contains(id) by {
        assert(inv_txn(s, c, id));
        let j = txn(s.txns, id).pidx as int;
        assert(s.prepared[j] == id);
        assert(sel(s, c, s.prepared[j]));
    }
}

// ---------------------------------------------------------------------------
// Serial execution
// ---------------------------------------------------------------------------

/// Transactions that do not write `k` leave its serial value unchanged.
pub proof fn lemma_serial_skip(txns: Map<int, TxnRec>, k: int, q: Seq<int>, m: int)
    requires 0 <= m <= q.len(),
        forall|i: int| m <= i < q.len() ==> !write_set(txns[#[trigger] q[i]].body).contains(k)
    ensures serial_value(txns, k, q) == serial_value(txns, k, q.take(m))
    decreases q.len()
{
    if q.len() == m {
        assert(q.take(m) =~= q);
    } else {
        let q1 = q.drop_last();
        let l = q.last();
        assert(!write_set(txns[q[q.len() - 1]].body).contains(k));
        let body = txns[l].body;
        if body.ops.dom().contains(k) {
            assert(!writes_key(body.ops[k]));
            assert(body.ops[k] is Read);
        }
        assert(serial_value(txns, k, q) == serial_value(txns, k, q1));
        assert forall|i: int| m <= i < q1.len() implies !write_set(txns[#[trigger] q1[i]].body).contains(k) by {
            assert(q1[i] == q[i]);
        }
        lemma_serial_skip(txns, k, q1, m);
        assert(q1.take(m) =~= q.take(m));
    }
}

/// One step of serial execution at a transaction that touches `k`.
pub proof fn lemma_serial_step(txns: Map<int, TxnRec>, k: int, q: Seq<int>, a: int)
    requires 0 <= a < q.len(), txns[q[a]].body.ops.dom().contains(k)
    ensures serial_value(txns, k, q.take(a + 1)) == apply_op(serial_value(txns, k, q.take(a)), txns[q[a]].body.ops[k])
{
    let t = q.take(a + 1);
    assert(t.drop_last() =~= q.take(a));
    assert(t.last() == q[a]);
}

pub proof fn lemma_serial_empty(txns: Map<int, TxnRec>, k: int, q: Seq<int>)
    requires q.len() == 0
    ensures serial_value(txns, k, q) == 0
{
}

/// Every read of a witness transaction returns the value serial execution of
/// the witness prefix before it produces.
pub proof fn lemma_result(s: State, c: Constants, order: Seq<int>, j: int, k: int)
    requires inv(s, c), good_order(s, c, order), 0 <= j < order.len(),
        read_set(txn(s.txns, order[j]).body).contains(k)
    ensures txn(s.txns, order[j]).reads[k].value == serial_value(s.txns, k, order.take(j))
    decreases j
{
    let p = order[j];
    let r = txn(s.txns, p);
    assert(sel(s, c, p));
    assert(inv_txn(s, c, p));
    assert(r.reads.dom().contains(k));
    assert(inv_read(s, c, p, k));
    let w = r.reads[k].writer;
    let q = order.take(j);
    if w == -1 {
        assert forall|i: int| 0 <= i < q.len() implies !write_set(s.txns[#[trigger] q[i]].body).contains(k) by {
            assert(q[i] == order[i]);
            let o = order[i];
            assert(sel(s, c, o));
            assert(txn(s.txns, order[i]).pidx < txn(s.txns, order[j]).pidx);
            if write_set(txn(s.txns, o).body).contains(k) {
                lemma_read_order_sel(s, c, p, k, o);
            }
        }
        lemma_serial_skip(s.txns, k, q, 0);
        lemma_serial_empty(s.txns, k, q.take(0));
    } else {
        lemma_writer_sel(s, c, p, k);
        let rw = txn(s.txns, w);
        assert(order.contains(w));
        let a = choose|a: int| 0 <= a < order.len() && order[a] == w;
        if a > j {
            assert(txn(s.txns, order[j]).pidx < txn(s.txns, order[a]).pidx);
        }
        assert(a < j);
        assert forall|i: int| a + 1 <= i < q.len() implies !write_set(s.txns[#[trigger] q[i]].body).contains(k) by {
            assert(q[i] == order[i]);
            let o = order[i];
            assert(sel(s, c, o));
            assert(txn(s.txns, order[a]).pidx < txn(s.txns, order[i]).pidx);
            assert(txn(s.txns, order[i]).pidx < txn(s.txns, order[j]).pidx);
            if write_set(txn(s.txns, o).body).contains(k) {
                lemma_read_order_sel(s, c, p, k, o);
            }
        }
        lemma_serial_skip(s.txns, k, q, a + 1);
        assert(q.take(a + 1) =~= order.take(a + 1));
        assert(inv_read_writer(s, c, p, k));
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

/// The witness respects real time.
pub proof fn lemma_real_time(s: State, c: Constants, order: Seq<int>, a: int, b: int)
    requires inv(s, c), good_order(s, c, order), 0 <= a < order.len(), 0 <= b < order.len(),
        committed(txn(s.txns, order[a])), txn(s.txns, order[a]).acked < txn(s.txns, order[b]).invoked
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
        assert(s.prepared[rb.pidx as int] == tb);
        assert(s.prepared[ra.pidx as int] == ta);
        assert(txn(s.txns, s.prepared[rb.pidx as int]).prepared_at < txn(s.txns, s.prepared[ra.pidx as int]).prepared_at);
    }
}

// ---------------------------------------------------------------------------
// Theorem: strict serializability
// ---------------------------------------------------------------------------

pub proof fn theorem_strictly_serializable(s: State, c: Constants)
    requires inv(s, c)
    ensures strictly_serializable(s)
{
    let order = wit(s, c, s.prepared.len() as int);
    lemma_good_order(s, c);
    assert(order.no_duplicates()) by {
        assert forall|a: int, b: int| 0 <= a < order.len() && 0 <= b < order.len() && a != b
            implies order[a] != order[b] by {
            if a < b {
                assert(txn(s.txns, order[a]).pidx < txn(s.txns, order[b]).pidx);
            } else {
                assert(txn(s.txns, order[b]).pidx < txn(s.txns, order[a]).pidx);
            }
        }
    }
    assert forall|j: int| 0 <= j < order.len() implies
        s.txns.dom().contains(#[trigger] order[j]) && certified_or_committed(s.txns[order[j]]) by {
        assert(sel(s, c, order[j]));
    }
    assert forall|id: int| #[trigger] s.txns.dom().contains(id) && committed(s.txns[id]) implies order.contains(id) by {
        assert(committed(txn(s.txns, id)));
        assert(sel(s, c, id));
    }
    assert forall|a: int, b: int| 0 <= a < order.len() && 0 <= b < order.len()
        && committed(s.txns[#[trigger] order[a]]) && s.txns[order[a]].acked < s.txns[#[trigger] order[b]].invoked
        implies a < b by {
        lemma_real_time(s, c, order, a, b);
    }
    assert forall|j: int, k: int| 0 <= j < order.len() && committed(s.txns[#[trigger] order[j]])
        && #[trigger] read_set(s.txns[order[j]].body).contains(k)
        implies s.txns[order[j]].reads[k].value == serial_value(s.txns, k, order.take(j)) by {
        lemma_result(s, c, order, j, k);
    }
    assert(serial_witness(s, order));
}

// ---------------------------------------------------------------------------
// Theorem: durability of committed transactions
// ---------------------------------------------------------------------------

/// A shard at which a transaction must be logged is a real shard.
pub proof fn lemma_logged_at_shard(s: State, c: Constants, id: int, j: int)
    requires inv(s, c), s.txns.dom().contains(id), logged_at(c, txn(s.txns, id), j)
    ensures is_shard(c, j)
{
    let r = txn(s.txns, id);
    assert(inv_txn(s, c, id));
    assert(clock_shard(c, r, j));
    if j != r.coord {
        assert(writes_at(c, r.body, j));
        let k = choose|k: int| #[trigger] write_set(r.body).contains(k) && owner(c, k) == j;
        lemma_writes_at_owner(c, r.body, k);
    }
}

pub proof fn lemma_durable_log(s: State, c: Constants, id: int, j: int)
    requires inv(s, c), s.txns.dom().contains(id), is_prepared_or_later(txn(s.txns, id)),
        below_wm(s.streams, c, txn(s.txns, id).vc, txn(s.txns, id).epoch),
        is_shard(c, j), logged_at(c, txn(s.txns, id), j)
    ensures durable_has_log(s.streams[coord_sid(txn(s.txns, id), j)], id)
{
    let r = txn(s.txns, id);
    lemma_below_wm_not_lost(s, c, id);
    assert(has_log(stream_at(s.streams, r, j), id));
    let sid = coord_sid(r, j);
    assert(inv_txn(s, c, id));
    assert(valid_sid(c, sid));
    assert(inv_stream(s, c, sid));
    let st = s.streams[sid];
    let es = all_entries(st);
    let x = r.vc[group(c, j)];
    assert forall|i: int| 0 <= i < es.len() && is_log_of(#[trigger] es[i], id) implies
        es[i]->Log_epoch == r.epoch && es[i]->Log_clock == x by {
        assert(inv_entry(s, c, sid, es[i]));
    }
    assert(wm_le(r.vc[group(c, sid.shard)], stream_wm(s.streams[sid].durable, r.epoch)));
    lemma_log_at_wm_is_durable(st, r.epoch, id, x);
}

pub proof fn theorem_durability(s: State, c: Constants, id: int)
    requires inv(s, c), s.txns.dom().contains(id), committed(s.txns[id])
    ensures
        forall|j: int| #[trigger] logged_at(c, s.txns[id], j) ==>
            is_shard(c, j) && durable_has_log(s.streams[coord_sid(s.txns[id], j)], id),
        !lost(s.streams, c, s.txns[id], id),
        !doomed(s.final_wm, c, s.txns[id]),
        forall|k: int| #[trigger] write_set(s.txns[id].body).contains(k) ==> has_version(s.versions, k, id),
{
    let r = txn(s.txns, id);
    assert(committed(r));
    assert(below_wm(s.streams, c, r.vc, r.epoch));
    lemma_below_wm_not_lost(s, c, id);
    lemma_below_wm_not_doomed(s, c, r);
    assert forall|j: int| #[trigger] logged_at(c, s.txns[id], j) implies
        is_shard(c, j) && durable_has_log(s.streams[coord_sid(s.txns[id], j)], id) by {
        lemma_logged_at_shard(s, c, id, j);
        lemma_durable_log(s, c, id, j);
    }
    assert forall|k: int| #[trigger] write_set(s.txns[id].body).contains(k) implies has_version(s.versions, k, id) by {
        lemma_present_version(s, c, id, k);
    }
}

// ---------------------------------------------------------------------------
// Theorem 1 (atomicity) with Lemma 6
// ---------------------------------------------------------------------------

/// Lemma 6: a transaction terminated while installing is doomed once its
/// epoch's FVW exists (its coordinator stream can never reach its clock).
pub proof fn lemma_aborted_doomed(s: State, c: Constants, id: int)
    requires inv(s, c), s.txns.dom().contains(id), aborted_prepared(txn(s.txns, id)),
        fvw_ready(s.final_wm, c, txn(s.txns, id).epoch)
    ensures doomed(s.final_wm, c, txn(s.txns, id))
{
    let r = txn(s.txns, id);
    let e = r.epoch;
    assert(inv_txn(s, c, id));
    assert(!read_only(r));
    assert(clock_shard(c, r, r.coord));
    let x = r.vc[group(c, r.coord)];
    assert(x >= 1);
    assert(inv_logs(s, c, id));
    assert(inv_coord_below(s, c, r));
    let sid = coord_sid(r, r.coord);
    assert(valid_sid(c, sid));
    lemma_stream_below_not_wm(s.streams[sid], e, x);
    assert(s.final_wm.dom().contains((r.coord, e)));
    assert(inv_final_of(s, c, r.coord, e));
    assert(no_pending_epoch(s.streams[sid], e)
        && wm_le_wm(s.final_wm[(r.coord, e)], stream_wm(s.streams[sid].durable, e)));
    if below_fvw(s.final_wm, c, r.vc, e) {
        assert(wm_le(x, s.final_wm[(r.coord, e)]));
        lemma_wm_le_int_trans(x, s.final_wm[(r.coord, e)], stream_wm(s.streams[sid].durable, e));
    }
}

/// Lemma 5: after rollback at shard `j`, a doomed transaction has no version
/// of a key owned by `j`.
pub proof fn lemma_rolled_back_gone(s: State, c: Constants, id: int, j: int, k: int)
    requires inv(s, c), s.txns.dom().contains(id), doomed(s.final_wm, c, txn(s.txns, id)),
        s.rolled_back.contains((j, txn(s.txns, id).epoch)), owner(c, k) == j
    ensures !has_version(s.versions, k, id)
{
    let r = txn(s.txns, id);
    if has_version(s.versions, k, id) {
        let jj = choose|jj: int| 0 <= jj < vers(s.versions, k).len() && #[trigger] vers(s.versions, k)[jj].txn == id;
        assert(s.versions.dom().contains(k));
        let v = s.versions[k][jj];
        assert(inv_versions_of(s, c, k));
        assert(inv_version(s, c, k, v));
        assert(v.epoch == r.epoch && v.vc == r.vc);
        assert(inv_rolled_back_of(s, c, j, r.epoch));
        assert(below_fvw(s.final_wm, c, s.versions[k][jj].vc, r.epoch));
    }
}

pub proof fn theorem_atomicity(s: State, c: Constants, id: int)
    requires inv(s, c), s.txns.dom().contains(id), is_prepared_or_later(s.txns[id]),
        fvw_ready(s.final_wm, c, s.txns[id].epoch)
    ensures
        aborted_prepared(s.txns[id]) ==> doomed(s.final_wm, c, s.txns[id]),
        certified_or_committed(s.txns[id]) && !doomed(s.final_wm, c, s.txns[id]) && !lost(s.streams, c, s.txns[id], id)
            ==> forall|k: int| #[trigger] write_set(s.txns[id].body).contains(k) ==> has_version(s.versions, k, id),
        doomed(s.final_wm, c, s.txns[id]) ==> forall|j: int, k: int|
            #[trigger] s.rolled_back.contains((j, s.txns[id].epoch)) && #[trigger] write_set(s.txns[id].body).contains(k)
            && owner(c, k) == j ==> !has_version(s.versions, k, id),
{
    let r = txn(s.txns, id);
    if aborted_prepared(r) {
        lemma_aborted_doomed(s, c, id);
    }
    if certified_or_committed(r) && !doomed(s.final_wm, c, r) && !lost(s.streams, c, r, id) {
        assert forall|k: int| #[trigger] write_set(s.txns[id].body).contains(k) implies has_version(s.versions, k, id) by {
            lemma_present_version(s, c, id, k);
        }
    }
    if doomed(s.final_wm, c, r) {
        assert forall|j: int, k: int|
            #[trigger] s.rolled_back.contains((j, s.txns[id].epoch)) && #[trigger] write_set(s.txns[id].body).contains(k)
            && owner(c, k) == j implies !has_version(s.versions, k, id) by {
            lemma_rolled_back_gone(s, c, id, j, k);
        }
    }
}

// ---------------------------------------------------------------------------
// Theorem 2 (rollback safety) with Lemmas 1, 7 and 8
// ---------------------------------------------------------------------------

pub proof fn theorem_rollback_safety(s: State, c: Constants, t1: int, k: int)
    requires inv(s, c), s.txns.dom().contains(t1), is_prepared_or_later(s.txns[t1]),
        s.txns[t1].reads.dom().contains(k), s.txns[t1].reads[k].writer != -1,
    ensures ({
        let t0 = s.txns[t1].reads[k].writer;
        &&& s.txns.dom().contains(t0)
        &&& write_set(s.txns[t0].body).contains(k)
        &&& is_prepared_or_later(s.txns[t0])
        // Lemma 7: no dependency on a later epoch
        &&& s.txns[t0].epoch <= s.txns[t1].epoch
        // Lemma 1: same-epoch dependencies are ordered by clock
        &&& s.txns[t0].epoch == s.txns[t1].epoch ==> vc_le(s.txns[t0].vc, s.txns[t1].vc, c.comp)
        // Lemma 8: a cross-epoch dependency is never rolled back
        &&& s.txns[t0].epoch < s.txns[t1].epoch ==> !doomed(s.final_wm, c, s.txns[t0])
        // Theorem 2
        &&& doomed(s.final_wm, c, s.txns[t0]) ==> doomed(s.final_wm, c, s.txns[t1])
        // An acknowledged transaction depends only on durable, surviving writes
        &&& committed(s.txns[t1]) ==> certified_or_committed(s.txns[t0])
            && !doomed(s.final_wm, c, s.txns[t0]) && !lost(s.streams, c, s.txns[t0], t0)
    }),
{
    let r1 = txn(s.txns, t1);
    let t0 = r1.reads[k].writer;
    let r0 = txn(s.txns, t0);
    assert(inv_read(s, c, t1, k));
    assert(inv_read_writer(s, c, t1, k));
    if doomed(s.final_wm, c, r0) && r0.epoch == r1.epoch {
        lemma_doomed_vc_le(s.final_wm, c, r0, r1);
    }
    if committed(r1) {
        assert(below_wm(s.streams, c, r1.vc, r1.epoch));
        lemma_writer_sel(s, c, t1, k);
        lemma_below_wm_not_doomed(s, c, r0);
        lemma_below_wm_not_lost(s, c, t0);
    }
}

} // verus!
