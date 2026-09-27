//! Facts about stream watermarks that the preservation proofs and theorems use.
use super::types::*;
use super::invariants::*;
use vstd::prelude::*;

verus! {

/// Appending an entry of another epoch does not change epoch `e`'s watermark.
pub proof fn lemma_wm_push_other(es: Seq<Entry>, x: Entry, e: nat)
    requires entry_epoch(x) != e
    ensures stream_wm(es.push(x), e) == stream_wm(es, e)
{
    assert(es.push(x).last() == x);
    assert(es.push(x).drop_last() =~= es);
}

/// Appending an epoch-`e` entry makes it the watermark.
pub proof fn lemma_wm_push_same(es: Seq<Entry>, x: Entry, e: nat)
    requires entry_epoch(x) == e
    ensures stream_wm(es.push(x), e) == entry_wm(x)
{
    assert(es.push(x).last() == x);
}

/// Appending a whole sequence of other-epoch entries changes nothing.
pub proof fn lemma_wm_add_other(es: Seq<Entry>, xs: Seq<Entry>, e: nat)
    requires forall|j: int| 0 <= j < xs.len() ==> entry_epoch(#[trigger] xs[j]) != e
    ensures stream_wm(es + xs, e) == stream_wm(es, e)
    decreases xs.len()
{
    if xs.len() == 0 {
        assert(es + xs =~= es);
    } else {
        let ys = xs.drop_last();
        assert(forall|j: int| 0 <= j < ys.len() ==> entry_epoch(#[trigger] ys[j]) != e) by {
            assert forall|j: int| 0 <= j < ys.len() implies entry_epoch(#[trigger] ys[j]) != e by {
                assert(ys[j] == xs[j]);
            }
        }
        lemma_wm_add_other(es, ys, e);
        assert(es + xs =~= (es + ys).push(xs.last()));
        lemma_wm_push_other(es + ys, xs.last(), e);
    }
}

/// Under the stream ordering invariant every epoch-`e` log entry is at or
/// below the epoch's watermark.
pub proof fn lemma_wm_bounds_entries(es: Seq<Entry>, e: nat, j: int)
    requires inv_stream_order(es), 0 <= j < es.len(), es[j] is Log, es[j]->Log_epoch == e
    ensures wm_le(es[j]->Log_clock, stream_wm(es, e))
    decreases es.len()
{
    let last = es.last();
    if j == es.len() - 1 {
        assert(stream_wm(es, e) == entry_wm(last));
    } else if entry_epoch(last) == e {
        // last is Log (clock larger) or Inf
        if last is Log {
            assert(es[j]->Log_clock < last->Log_clock);
        }
    } else {
        let ys = es.drop_last();
        assert(inv_stream_order(ys)) by { lemma_order_drop_last(es); }
        assert(ys[j] == es[j]);
        lemma_wm_bounds_entries(ys, e, j);
    }
}

pub proof fn lemma_order_drop_last(es: Seq<Entry>)
    requires inv_stream_order(es), es.len() > 0
    ensures inv_stream_order(es.drop_last())
{
    let ys = es.drop_last();
    assert(forall|a: int| 0 <= a < ys.len() ==> ys[a] == es[a]);
}

pub proof fn lemma_order_prefix(es: Seq<Entry>, n: int)
    requires inv_stream_order(es), 0 <= n <= es.len()
    ensures inv_stream_order(es.take(n))
{
    let ys = es.take(n);
    assert(forall|a: int| 0 <= a < ys.len() ==> ys[a] == es[a]);
}

/// If every epoch-`e` log entry is below `x` (x >= 1) and there is no INF
/// marker for `e`, the watermark is a finite value below `x`.
pub proof fn lemma_stream_below_wm(es: Seq<Entry>, e: nat, x: int)
    requires stream_below(es, e, x), x >= 1
    ensures !wm_le(x, stream_wm(es, e))
    decreases es.len()
{
    if es.len() == 0 {
    } else if entry_epoch(es.last()) == e {
        assert(!(es.last() is Inf));
        assert(es.last() is Log);
    } else {
        let ys = es.drop_last();
        assert(stream_below(ys, e, x)) by {
            assert(forall|a: int| 0 <= a < ys.len() ==> ys[a] == es[a]);
        }
        lemma_stream_below_wm(ys, e, x);
    }
}

/// A pending epoch-`e` log entry lies strictly above the durable watermark.
pub proof fn lemma_pending_above(st: Stream, e: nat, j: int)
    requires
        inv_stream_order(all_entries(st)),
        0 <= j < st.pending.len(), st.pending[j] is Log, st.pending[j]->Log_epoch == e,
    ensures !wm_le(st.pending[j]->Log_clock, stream_wm(st.durable, e))
{
    let es = all_entries(st);
    let d = st.durable;
    let x = st.pending[j]->Log_clock;
    let pj = d.len() + j;
    assert(es[pj] == st.pending[j]);
    assert forall|a: int| 0 <= a < d.len() && #[trigger] d[a] is Log && d[a]->Log_epoch == e implies d[a]->Log_clock < x by {
        assert(es[a] == d[a]);
    }
    assert forall|a: int| 0 <= a < d.len() implies !(#[trigger] d[a] is Inf && d[a]->Inf_epoch == e) by {
        assert(es[a] == d[a]);
        if d[a] is Inf && d[a]->Inf_epoch == e {
            assert(entry_epoch(es[pj]) > es[a]->Inf_epoch);
        }
    }
    assert(stream_below(d, e, x));
    assert(x >= 1);
    lemma_stream_below_wm(d, e, x);
}

/// Variant without the x >= 1 side condition: the watermark is Fin(v) with
/// v < x, or Fin(0) when there is no epoch-`e` entry at all.
pub proof fn lemma_stream_below_wm_weak(es: Seq<Entry>, e: nat, x: int)
    requires stream_below(es, e, x)
    ensures stream_wm(es, e) is Fin,
        stream_wm(es, e)->0 < x || stream_wm(es, e)->0 == 0
    decreases es.len()
{
    if es.len() == 0 {
    } else if entry_epoch(es.last()) == e {
        assert(es.last() is Log);
    } else {
        let ys = es.drop_last();
        assert(stream_below(ys, e, x)) by {
            assert(forall|a: int| 0 <= a < ys.len() ==> ys[a] == es[a]);
        }
        lemma_stream_below_wm_weak(ys, e, x);
    }
}

/// Watermarks only grow when a pending entry becomes durable.
pub proof fn lemma_wm_monotone_push(es: Seq<Entry>, x: Entry, e: nat)
    requires inv_stream_order(es.push(x))
    ensures wm_le_wm(stream_wm(es, e), stream_wm(es.push(x), e))
{
    if entry_epoch(x) == e {
        lemma_wm_push_same(es, x, e);
        if x is Log {
            // every epoch-e log in es is below x's clock, and es has no Inf(e)
            let n = es.len() as int;
            assert(es.push(x)[n] == x);
            assert forall|a: int| 0 <= a < es.len() && #[trigger] es[a] is Log && es[a]->Log_epoch == e implies es[a]->Log_clock < x->Log_clock by {
                assert(es.push(x)[a] == es[a]);
            }
            assert forall|a: int| 0 <= a < es.len() implies !(#[trigger] es[a] is Inf && es[a]->Inf_epoch == e) by {
                assert(es.push(x)[a] == es[a]);
            }
            assert(stream_below(es, e, x->Log_clock));
            assert(x->Log_clock >= 1);
            lemma_stream_below_wm_weak(es, e, x->Log_clock);
        }
    } else {
        lemma_wm_push_other(es, x, e);
        assert(wm_le_wm(stream_wm(es, e), stream_wm(es, e))) by { lemma_wm_le_refl(stream_wm(es, e)); }
    }
}

pub proof fn lemma_wm_le_refl(w: Wm)
    ensures wm_le_wm(w, w)
{
}

pub proof fn lemma_wm_le_trans(a: Wm, b: Wm, c: Wm)
    requires wm_le_wm(a, b), wm_le_wm(b, c)
    ensures wm_le_wm(a, c)
{
}

pub proof fn lemma_wm_le_int_trans(x: int, a: Wm, b: Wm)
    requires wm_le(x, a), wm_le_wm(a, b)
    ensures wm_le(x, b)
{
}

/// A durable-or-pending entry whose clock is at or below the durable
/// watermark is durable.
pub proof fn lemma_log_at_wm_is_durable(st: Stream, e: nat, id: int, x: int)
    requires
        inv_stream_order(all_entries(st)),
        has_log(all_entries(st), id),
        forall|j: int| 0 <= j < all_entries(st).len() && is_log_of(#[trigger] all_entries(st)[j], id)
            ==> all_entries(st)[j]->Log_epoch == e && all_entries(st)[j]->Log_clock == x,
        wm_le(x, stream_wm(st.durable, e)),
    ensures durable_has_log(st, id)
{
    let es = all_entries(st);
    let j = choose|j: int| 0 <= j < es.len() && is_log_of(#[trigger] es[j], id);
    if j < st.durable.len() {
        assert(st.durable[j] == es[j]);
    } else {
        let pj = j - st.durable.len();
        assert(st.pending[pj] == es[j]);
        lemma_pending_above(st, e, pj);
    }
}

} // verus!
