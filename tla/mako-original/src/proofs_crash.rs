//! Preservation proof for Crash: a shard leader fails, the epoch advances,
//! each of its streams keeps a prefix of its pending entries, versions whose
//! log entry was lost are discarded, and its in-flight transactions abort.
use super::types::*;
use super::normal::*;
use super::recovery::*;
use super::behavior::*;
use super::invariants::*;
use super::stream_lemmas::*;
use super::proofs_basic::*;
use vstd::prelude::*;

verus! {

// ---------------------------------------------------------------------------
// Generic sequence facts
// ---------------------------------------------------------------------------

/// Every element of a filtered sequence is an element of the original.
pub proof fn lemma_filter_index<A>(vs: Seq<A>, p: spec_fn(A) -> bool, a: int) -> (x: int)
    requires 0 <= a < vs.filter(p).len()
    ensures 0 <= x < vs.len(), vs[x] == vs.filter(p)[a], p(vs[x])
    decreases vs.len()
{
    reveal(Seq::filter);
    let sub = vs.drop_last().filter(p);
    if p(vs.last()) {
        assert(vs.filter(p) == sub.push(vs.last()));
        if a == sub.len() {
            vs.len() - 1
        } else {
            let x = lemma_filter_index(vs.drop_last(), p, a);
            assert(vs.drop_last()[x] == vs[x]);
            x
        }
    } else {
        assert(vs.filter(p) == sub);
        let x = lemma_filter_index(vs.drop_last(), p, a);
        assert(vs.drop_last()[x] == vs[x]);
        x
    }
}

/// Filtering preserves the relative order of the kept elements.
pub proof fn lemma_filter_order<A>(vs: Seq<A>, p: spec_fn(A) -> bool, a: int, b: int) -> (xy: (int, int))
    requires 0 <= a < b < vs.filter(p).len()
    ensures 0 <= xy.0 < xy.1 < vs.len(), vs[xy.0] == vs.filter(p)[a], vs[xy.1] == vs.filter(p)[b]
    decreases vs.len()
{
    reveal(Seq::filter);
    let sub = vs.drop_last().filter(p);
    if p(vs.last()) {
        assert(vs.filter(p) == sub.push(vs.last()));
        if b == sub.len() {
            let x = lemma_filter_index(vs.drop_last(), p, a);
            assert(vs.drop_last()[x] == vs[x]);
            (x, vs.len() - 1)
        } else {
            let xy = lemma_filter_order(vs.drop_last(), p, a, b);
            assert(vs.drop_last()[xy.0] == vs[xy.0]);
            assert(vs.drop_last()[xy.1] == vs[xy.1]);
            xy
        }
    } else {
        assert(vs.filter(p) == sub);
        let xy = lemma_filter_order(vs.drop_last(), p, a, b);
        assert(vs.drop_last()[xy.0] == vs[xy.0]);
        assert(vs.drop_last()[xy.1] == vs[xy.1]);
        xy
    }
}

/// A kept element appears in the filtered sequence.
pub proof fn lemma_filter_keeps<A>(vs: Seq<A>, p: spec_fn(A) -> bool, x: int) -> (a: int)
    requires 0 <= x < vs.len(), p(vs[x])
    ensures 0 <= a < vs.filter(p).len(), vs.filter(p)[a] == vs[x]
{
    vs.lemma_filter_contains(p, x);
    let a = choose|a: int| 0 <= a < vs.filter(p).len() && vs.filter(p)[a] == vs[x];
    a
}

// ---------------------------------------------------------------------------
// Stream prefixes
// ---------------------------------------------------------------------------

pub open spec fn is_prefix(a: Seq<Entry>, b: Seq<Entry>) -> bool {
    a.len() <= b.len() && forall|j: int| 0 <= j < a.len() ==> #[trigger] a[j] == b[j]
}

pub proof fn lemma_prefix_order(a: Seq<Entry>, b: Seq<Entry>)
    requires is_prefix(a, b), inv_stream_order(b)
    ensures inv_stream_order(a)
{
    assert forall|x: int, y: int| 0 <= x < y < a.len() implies entry_epoch(#[trigger] a[x]) <= entry_epoch(#[trigger] a[y]) by {
        assert(a[x] == b[x] && a[y] == b[y]);
    }
    assert forall|x: int, y: int| 0 <= x < y < a.len() && #[trigger] a[x] is Log && #[trigger] a[y] is Log
        && a[x]->Log_epoch == a[y]->Log_epoch implies a[x]->Log_clock < a[y]->Log_clock by {
        assert(a[x] == b[x] && a[y] == b[y]);
    }
    assert forall|x: int, y: int| 0 <= x < y < a.len() && #[trigger] a[x] is Inf
        implies entry_epoch(#[trigger] a[y]) > a[x]->Inf_epoch by {
        assert(a[x] == b[x] && a[y] == b[y]);
    }
    assert forall|x: int, y: int| 0 <= x < y < a.len() && #[trigger] a[x] is Log && #[trigger] a[y] is Log
        implies a[x]->Log_txn != a[y]->Log_txn by {
        assert(a[x] == b[x] && a[y] == b[y]);
    }
    assert forall|x: int| 0 <= x < a.len() && #[trigger] a[x] is Log implies a[x]->Log_clock >= 1 by {
        assert(a[x] == b[x]);
    }
}

pub proof fn lemma_prefix_logs_below(a: Seq<Entry>, b: Seq<Entry>, e: nat, x: int)
    requires is_prefix(a, b), logs_below(b, e, x)
    ensures logs_below(a, e, x)
{
    assert forall|j: int| 0 <= j < a.len() && #[trigger] a[j] is Log && a[j]->Log_epoch == e implies a[j]->Log_clock < x by {
        assert(a[j] == b[j]);
    }
}

pub proof fn lemma_prefix_below(a: Seq<Entry>, b: Seq<Entry>, e: nat, x: int)
    requires is_prefix(a, b), stream_below(b, e, x)
    ensures stream_below(a, e, x)
{
    lemma_prefix_logs_below(a, b, e, x);
    assert forall|j: int| 0 <= j < a.len() implies !(#[trigger] a[j] is Inf && a[j]->Inf_epoch == e) by {
        assert(a[j] == b[j]);
    }
}

pub proof fn lemma_prefix_no_log(a: Seq<Entry>, b: Seq<Entry>, id: int)
    requires is_prefix(a, b), !has_log(b, id)
    ensures !has_log(a, id)
{
    if has_log(a, id) {
        let j = choose|j: int| 0 <= j < a.len() && is_log_of(#[trigger] a[j], id);
        assert(a[j] == b[j]);
    }
}

pub proof fn lemma_prefix_has_log(a: Seq<Entry>, b: Seq<Entry>, id: int, p: int)
    requires is_prefix(a, b), 0 <= p < a.len(), is_log_of(b[p], id)
    ensures has_log(a, id)
{
    assert(a[p] == b[p]);
}

/// An ordered stream cut before one of its epoch-`e` log entries (clock `x`)
/// can never reach `x` in epoch `e`.
pub proof fn lemma_prefix_cut_below(a: Seq<Entry>, b: Seq<Entry>, e: nat, x: int, p: int)
    requires is_prefix(a, b), inv_stream_order(b), a.len() <= p < b.len(), b[p] is Log,
        b[p]->Log_epoch == e, b[p]->Log_clock == x
    ensures stream_below(a, e, x)
{
    assert forall|j: int| 0 <= j < a.len() && #[trigger] a[j] is Log && a[j]->Log_epoch == e implies a[j]->Log_clock < x by {
        assert(a[j] == b[j]);
        assert(b[p] is Log);
    }
    assert forall|j: int| 0 <= j < a.len() implies !(#[trigger] a[j] is Inf && a[j]->Inf_epoch == e) by {
        assert(a[j] == b[j]);
        if b[j] is Inf && b[j]->Inf_epoch == e {
            assert(entry_epoch(b[p]) > b[j]->Inf_epoch);
        }
    }
}

// ---------------------------------------------------------------------------
// Watermarks under appending a sequence
// ---------------------------------------------------------------------------

pub proof fn lemma_wm_monotone_append(es: Seq<Entry>, xs: Seq<Entry>, e: nat)
    requires inv_stream_order(es + xs)
    ensures wm_le_wm(stream_wm(es, e), stream_wm(es + xs, e))
    decreases xs.len()
{
    if xs.len() == 0 {
        assert(es + xs =~= es);
        lemma_wm_le_refl(stream_wm(es, e));
    } else {
        let ys = xs.drop_last();
        assert((es + xs).drop_last() =~= es + ys);
        assert(es + xs =~= (es + ys).push(xs.last()));
        lemma_order_drop_last(es + xs);
        lemma_wm_monotone_append(es, ys, e);
        lemma_wm_monotone_push(es + ys, xs.last(), e);
        lemma_wm_le_trans(stream_wm(es, e), stream_wm(es + ys, e), stream_wm(es + xs, e));
    }
}


// ---------------------------------------------------------------------------
// The post-crash state, component by component
// ---------------------------------------------------------------------------

/// A transaction record after the crash of shard `i`.
pub open spec fn crashed_txn(r: TxnRec, i: int) -> TxnRec {
    if r.coord == i && in_flight(r) {
        TxnRec { status: Status::Aborted { prepared: r.status is Prepared }, ..r }
    } else { r }
}

pub proof fn lemma_crashed_txn(c: Constants, r: TxnRec, i: int)
    ensures ({
        let r2 = crashed_txn(r, i);
        &&& r2.body == r.body && r2.coord == r.coord && r2.thread == r.thread && r2.epoch == r.epoch
        &&& r2.reads == r.reads && r2.vc == r.vc && r2.pidx == r.pidx && r2.installed == r.installed
        &&& r2.invoked == r.invoked && r2.prepared_at == r.prepared_at && r2.acked == r.acked
        &&& is_prepared_or_later(r2) == is_prepared_or_later(r)
        &&& certified_or_committed(r2) == certified_or_committed(r)
        &&& committed(r2) == committed(r)
        &&& read_only(r2) == read_only(r)
        &&& (in_flight(r2) ==> r2 == r && r.coord != i)
        &&& (r2.status is Prepared ==> r2 == r && r.coord != i)
        &&& (r.status is Aborted ==> r2 == r)
        &&& (r2.status is Running ==> r.status is Running)
        &&& (aborted_prepared(r2) ==> aborted_prepared(r) || (r.status is Prepared && r.coord == i))
        &&& (r.coord != i ==> r2 == r)
        &&& (!in_flight(r) ==> r2 == r)
        &&& (forall|j: int| #[trigger] clock_shard(c, r2, j) == clock_shard(c, r, j))
        &&& (forall|j: int| #[trigger] logged_at(c, r2, j) == logged_at(c, r, j))
        &&& (forall|k: int| #[trigger] write_value(r2, k) == write_value(r, k))
        &&& (forall|j: int| #[trigger] coord_sid(r2, j) == coord_sid(r, j))
    })
{
}

pub proof fn lemma_owner_shard(c: Constants, k: int)
    requires valid_constants(c), valid_key(k)
    ensures is_shard(c, owner(c, k))
{
}

pub proof fn lemma_crash_facts(s: State, c: Constants, i: int, survive: Map<Sid, nat>, s2: State)
    requires inv_shapes(s, c), can_crash(s, c, i, survive), s2 == apply(s, c, Action::Crash { shard: i, survive })
    ensures
        s2.epoch == s.epoch + 1,
        s2.shards.len() == c.shards,
        shard_epoch(s2.shards, i) == s.epoch + 1,
        counter(s2.shards, i) == 0,
        forall|j: int| is_shard(c, j) && j != i ==> #[trigger] s2.shards[j] == s.shards[j],
        forall|j: int| is_shard(c, j) ==> #[trigger] shard_epoch(s2.shards, j) >= shard_epoch(s.shards, j),
        forall|j: int| is_shard(c, j) ==> #[trigger] shard_epoch(s.shards, j) <= s.epoch,
        s2.txns.dom() == s.txns.dom(),
        forall|o: int| s.txns.dom().contains(o) ==> #[trigger] txn(s2.txns, o) == crashed_txn(txn(s.txns, o), i),
        s2.streams.dom() == s.streams.dom(),
        forall|sid: Sid| valid_sid(c, sid) && sid.shard != i ==> #[trigger] s2.streams[sid] == s.streams[sid],
        forall|sid: Sid| #![trigger s2.streams[sid]] valid_sid(c, sid) && sid.shard == i ==>
            survive.dom().contains(sid) && survive[sid] <= s.streams[sid].pending.len()
            && s2.streams[sid].durable == s.streams[sid].durable + s.streams[sid].pending.take(survive[sid] as int)
            && s2.streams[sid].pending.len() == 0
            && all_entries(s2.streams[sid]) == s2.streams[sid].durable,
        forall|sid: Sid| valid_sid(c, sid) ==>
            is_prefix(all_entries(#[trigger] s2.streams[sid]), all_entries(s.streams[sid])),
        s2.versions.dom() == s.versions.dom(),
        forall|k: int| s.versions.dom().contains(k) && owner(c, k) != i ==> #[trigger] s2.versions[k] == s.versions[k],
        forall|k: int| s.versions.dom().contains(k) && owner(c, k) == i ==>
            #[trigger] s2.versions[k] == s.versions[k].filter(|v: Version| survives_crash(s, s2.streams, i, v)),
        s2.locks == crash_locks(s, c, i),
        s2.final_wm == s.final_wm,
        s2.rolled_back == s.rolled_back,
        s2.prepared == s.prepared,
        s2.tick == s.tick + 1,
{
    assert forall|j: int| is_shard(c, j) implies #[trigger] shard_epoch(s.shards, j) <= s.epoch by {
        assert(shard_epoch(s.shards, j) <= s.epoch && counter(s.shards, j) >= 0);
    }
    assert forall|sid: Sid| #![trigger s2.streams[sid]] valid_sid(c, sid) && sid.shard == i implies
        survive.dom().contains(sid) && survive[sid] <= s.streams[sid].pending.len()
        && s2.streams[sid].durable == s.streams[sid].durable + s.streams[sid].pending.take(survive[sid] as int)
        && s2.streams[sid].pending.len() == 0
        && all_entries(s2.streams[sid]) == s2.streams[sid].durable by {
        assert(s.streams.dom().contains(sid));
        assert(survive.dom().contains(sid));
        assert(all_entries(s2.streams[sid]) =~= s2.streams[sid].durable);
    }
    assert forall|sid: Sid| valid_sid(c, sid) implies
        is_prefix(all_entries(#[trigger] s2.streams[sid]), all_entries(s.streams[sid])) by {
        assert(s.streams.dom().contains(sid));
        if sid.shard == i {
            let st = s.streams[sid];
            let a = all_entries(s2.streams[sid]);
            let b = all_entries(st);
            assert(survive.dom().contains(sid));
            assert(a == st.durable + st.pending.take(survive[sid] as int));
            assert forall|j: int| 0 <= j < a.len() implies #[trigger] a[j] == b[j] by {
                if j < st.durable.len() {
                } else {
                    assert(a[j] == st.pending.take(survive[sid] as int)[j - st.durable.len()]);
                }
            }
        } else {
            assert(s2.streams[sid] == s.streams[sid]);
        }
    }
    assert forall|k: int| s.versions.dom().contains(k) && owner(c, k) == i implies
        #[trigger] s2.versions[k] == s.versions[k].filter(|v: Version| survives_crash(s, s2.streams, i, v)) by {
    }
}

/// The dedicated stream of `r` at shard `j` after the crash is a prefix of the old one.
pub proof fn lemma_crash_stream_at(s: State, c: Constants, i: int, survive: Map<Sid, nat>, s2: State, r: TxnRec, j: int)
    requires inv_shapes(s, c), can_crash(s, c, i, survive), s2 == apply(s, c, Action::Crash { shard: i, survive }),
        is_shard(c, j), is_shard(c, r.coord), is_thread(c, r.thread)
    ensures
        is_prefix(stream_at(s2.streams, r, j), stream_at(s.streams, r, j)),
        j != i ==> stream_at(s2.streams, r, j) == stream_at(s.streams, r, j),
        j == i ==> stream_at(s2.streams, r, j) == s2.streams[coord_sid(r, j)].durable,
{
    lemma_crash_facts(s, c, i, survive, s2);
    let sid = coord_sid(r, j);
    assert(valid_sid(c, sid));
    assert(is_prefix(all_entries(s2.streams[sid]), all_entries(s.streams[sid])));
    if j != i {
        assert(s2.streams[sid] == s.streams[sid]);
    }
}


// ---------------------------------------------------------------------------
// Per-conjunct preservation
// ---------------------------------------------------------------------------

pub proof fn lemma_crash_shapes(s: State, c: Constants, i: int, survive: Map<Sid, nat>)
    requires inv(s, c), can_crash(s, c, i, survive)
    ensures inv_shapes(apply(s, c, Action::Crash { shard: i, survive }), c)
{
    let s2 = apply(s, c, Action::Crash { shard: i, survive });
    lemma_crash_facts(s, c, i, survive, s2);
    assert forall|j: int| is_shard(c, j) implies #[trigger] shard_epoch(s2.shards, j) <= s2.epoch && counter(s2.shards, j) >= 0 by {
        if j != i {
            assert(s2.shards[j] == s.shards[j]);
            assert(shard_epoch(s.shards, j) <= s.epoch && counter(s.shards, j) >= 0);
        }
    }
    assert forall|sid: Sid| #[trigger] s2.streams.dom().contains(sid) <==> valid_sid(c, sid) by {
        assert(s.streams.dom().contains(sid) <==> valid_sid(c, sid));
    }
    assert forall|k: int| #[trigger] s2.versions.dom().contains(k) implies valid_key(k) by {
        assert(s.versions.dom().contains(k));
    }
}

pub proof fn lemma_crash_txns(s: State, c: Constants, i: int, survive: Map<Sid, nat>)
    requires inv(s, c), can_crash(s, c, i, survive)
    ensures inv_txns(apply(s, c, Action::Crash { shard: i, survive }), c)
{
    let s2 = apply(s, c, Action::Crash { shard: i, survive });
    lemma_crash_facts(s, c, i, survive, s2);
    assert forall|o: int| #[trigger] s2.txns.dom().contains(o) implies o >= 0 && inv_txn(s2, c, o) by {
        assert(s.txns.dom().contains(o));
        assert(inv_txn(s, c, o));
        let r = txn(s.txns, o);
        let r2 = txn(s2.txns, o);
        lemma_crashed_txn(c, r, i);
        assert(r2 == crashed_txn(r, i));
        assert(shard_epoch(s.shards, r.coord) <= s.epoch);
        if r.coord != i {
            assert(s2.shards[r.coord] == s.shards[r.coord]);
        }
        assert(r2.epoch <= shard_epoch(s2.shards, r2.coord));
        assert(in_flight(r2) ==> r2.epoch == shard_epoch(s2.shards, r2.coord));
        assert(aborted_prepared(r2) ==> r2.epoch < shard_epoch(s2.shards, r2.coord));
        assert forall|x: int| is_comp(c, x) implies #[trigger] r2.vc[x] >= 0 by {
            assert(r.vc[x] >= 0);
        }
        if is_prepared_or_later(r2) {
            assert forall|j: int| is_shard(c, j) && #[trigger] clock_shard(c, r2, j) implies r2.vc[group(c, j)] >= 1 by {
                assert(clock_shard(c, r, j));
            }
            assert forall|k: int| #[trigger] write_set(r2.body).contains(k) implies
                shard_epoch(s2.shards, owner(c, k)) >= r2.epoch by {
                assert(write_set(r.body).contains(k));
                assert(r.body.ops.dom().contains(k));
                lemma_owner_shard(c, k);
                assert(shard_epoch(s2.shards, owner(c, k)) >= shard_epoch(s.shards, owner(c, k)));
            }
            assert(r2.pidx < s2.prepared.len() && s2.prepared[r2.pidx as int] == o);
            assert forall|k: int| #[trigger] read_set(r2.body).contains(k) implies r2.reads.dom().contains(k) by {
                assert(read_set(r.body).contains(k));
            }
        }
        assert forall|j: int| #[trigger] r2.installed.contains(j) implies is_shard(c, j) && writes_at(c, r2.body, j) by {
            assert(r.installed.contains(j));
        }
        assert forall|k: int| #[trigger] r2.reads.dom().contains(k) implies read_set(r2.body).contains(k) by {
            assert(r.reads.dom().contains(k));
        }
        if certified_or_committed(r2) {
            assert(all_installed(c, r2));
        }
    }
}

pub proof fn lemma_crash_prepared(s: State, c: Constants, i: int, survive: Map<Sid, nat>)
    requires inv(s, c), can_crash(s, c, i, survive)
    ensures inv_prepared(apply(s, c, Action::Crash { shard: i, survive }), c)
{
    let s2 = apply(s, c, Action::Crash { shard: i, survive });
    lemma_crash_facts(s, c, i, survive, s2);
    assert forall|j: int| 0 <= j < s2.prepared.len() implies
        has_txn(s2.txns, #[trigger] s2.prepared[j]) && txn(s2.txns, s2.prepared[j]).pidx == j
        && is_prepared_or_later(txn(s2.txns, s2.prepared[j])) by {
        let o = s.prepared[j];
        assert(has_txn(s.txns, o) && txn(s.txns, o).pidx == j && is_prepared_or_later(txn(s.txns, o)));
        lemma_crashed_txn(c, txn(s.txns, o), i);
    }
    assert forall|j1: int, j2: int| 0 <= j1 < j2 < s2.prepared.len() implies
        txn(s2.txns, #[trigger] s2.prepared[j1]).prepared_at < txn(s2.txns, #[trigger] s2.prepared[j2]).prepared_at by {
        let o1 = s.prepared[j1];
        let o2 = s.prepared[j2];
        assert(has_txn(s.txns, o1));
        assert(has_txn(s.txns, o2));
        assert(txn(s.txns, o1).prepared_at < txn(s.txns, o2).prepared_at);
        lemma_crashed_txn(c, txn(s.txns, o1), i);
        lemma_crashed_txn(c, txn(s.txns, o2), i);
    }
}

pub proof fn lemma_crash_exclusive(s: State, c: Constants, i: int, survive: Map<Sid, nat>)
    requires inv(s, c), can_crash(s, c, i, survive)
    ensures inv_exclusive(apply(s, c, Action::Crash { shard: i, survive }), c)
{
    let s2 = apply(s, c, Action::Crash { shard: i, survive });
    lemma_crash_facts(s, c, i, survive, s2);
    assert forall|a: int, b: int| #[trigger] s2.txns.dom().contains(a) && #[trigger] s2.txns.dom().contains(b) && a != b
        && txn(s2.txns, a).coord == txn(s2.txns, b).coord && txn(s2.txns, a).thread == txn(s2.txns, b).thread
        implies !(in_flight(txn(s2.txns, a)) && in_flight(txn(s2.txns, b))) by {
        assert(s.txns.dom().contains(a) && s.txns.dom().contains(b));
        lemma_crashed_txn(c, txn(s.txns, a), i);
        lemma_crashed_txn(c, txn(s.txns, b), i);
    }
}

pub proof fn lemma_crash_locks(s: State, c: Constants, i: int, survive: Map<Sid, nat>)
    requires inv(s, c), can_crash(s, c, i, survive)
    ensures inv_locks(apply(s, c, Action::Crash { shard: i, survive }), c)
{
    let s2 = apply(s, c, Action::Crash { shard: i, survive });
    lemma_crash_facts(s, c, i, survive, s2);
    assert forall|k: int| #[trigger] s2.locks.dom().contains(k) implies inv_lock(s2, c, k) by {
        assert(s.locks.dom().contains(k) && owner(c, k) != i && s.txns[s.locks[k]].coord != i);
        assert(s2.locks[k] == s.locks[k]);
        assert(inv_lock(s, c, k));
        let h = s.locks[k];
        lemma_crashed_txn(c, txn(s.txns, h), i);
        assert(txn(s2.txns, h) == txn(s.txns, h));
        assert(vers(s2.versions, k) == vers(s.versions, k)) by {
            if s.versions.dom().contains(k) {
                assert(s2.versions[k] == s.versions[k]);
            }
        }
        assert forall|j: int| 0 <= j < vers(s2.versions, k).len() implies
            pidx_of(s2.txns, #[trigger] vers(s2.versions, k)[j].txn) < txn(s2.txns, h).pidx by {
            let o = vers(s.versions, k)[j].txn;
            assert(pidx_of(s.txns, o) < txn(s.txns, h).pidx);
            assert(s.versions.dom().contains(k));
            assert(inv_versions_of(s, c, k));
            assert(inv_version(s, c, k, s.versions[k][j]));
            lemma_crashed_txn(c, txn(s.txns, o), i);
            assert(o >= 0);
        }
    }
    assert forall|o: int, k: int| #[trigger] s2.txns.dom().contains(o) && txn(s2.txns, o).status is Prepared
        && #[trigger] write_set(txn(s2.txns, o).body).contains(k)
        && !txn(s2.txns, o).installed.contains(owner(c, k))
        && shard_epoch(s2.shards, owner(c, k)) == txn(s2.txns, o).epoch
        implies s2.locks.dom().contains(k) && s2.locks[k] == o by {
        let r = txn(s.txns, o);
        assert(s.txns.dom().contains(o));
        lemma_crashed_txn(c, r, i);
        assert(inv_txn(s, c, o));
        assert(r.body.ops.dom().contains(k));
        lemma_owner_shard(c, k);
        assert(shard_epoch(s.shards, r.coord) <= s.epoch);
        if owner(c, k) == i {
            assert(false);
        }
        assert(s2.shards[owner(c, k)] == s.shards[owner(c, k)]);
        assert(s.locks.dom().contains(k) && s.locks[k] == o);
    }
}

/// Version facts carry over when the writer's record only changed status.
pub proof fn lemma_version_transfer(s: State, s2: State, c: Constants, i: int, k: int, v: Version)
    requires inv_version(s, c, k, v), s2.txns.dom() == s.txns.dom(),
        txn(s2.txns, v.txn) == crashed_txn(txn(s.txns, v.txn), i)
    ensures inv_version(s2, c, k, v)
{
    lemma_crashed_txn(c, txn(s.txns, v.txn), i);
}

pub proof fn lemma_crash_versions(s: State, c: Constants, i: int, survive: Map<Sid, nat>)
    requires inv(s, c), can_crash(s, c, i, survive)
    ensures inv_versions(apply(s, c, Action::Crash { shard: i, survive }), c)
{
    let s2 = apply(s, c, Action::Crash { shard: i, survive });
    lemma_crash_facts(s, c, i, survive, s2);
    let pred = |v: Version| survives_crash(s, s2.streams, i, v);
    assert forall|k: int| #[trigger] s2.versions.dom().contains(k) implies inv_versions_of(s2, c, k) by {
        assert(s.versions.dom().contains(k));
        assert(inv_versions_of(s, c, k));
        let vs = s.versions[k];
        if owner(c, k) != i {
            assert(s2.versions[k] == vs);
            assert forall|j: int| 0 <= j < s2.versions[k].len() implies inv_version(s2, c, k, #[trigger] s2.versions[k][j]) by {
                assert(inv_version(s, c, k, vs[j]));
                lemma_version_transfer(s, s2, c, i, k, vs[j]);
            }
            assert forall|a: int, b: int| 0 <= a < b < s2.versions[k].len() implies
                txn(s2.txns, #[trigger] s2.versions[k][a].txn).pidx < txn(s2.txns, #[trigger] s2.versions[k][b].txn).pidx by {
                assert(inv_version(s, c, k, vs[a]));
                assert(inv_version(s, c, k, vs[b]));
                lemma_crashed_txn(c, txn(s.txns, vs[a].txn), i);
                lemma_crashed_txn(c, txn(s.txns, vs[b].txn), i);
                assert(txn(s.txns, vs[a].txn).pidx < txn(s.txns, vs[b].txn).pidx);
            }
        } else {
            assert(s2.versions[k] == vs.filter(pred));
            assert forall|j: int| 0 <= j < s2.versions[k].len() implies inv_version(s2, c, k, #[trigger] s2.versions[k][j]) by {
                let x = lemma_filter_index(vs, pred, j);
                assert(inv_version(s, c, k, vs[x]));
                lemma_version_transfer(s, s2, c, i, k, vs[x]);
            }
            assert forall|a: int, b: int| 0 <= a < b < s2.versions[k].len() implies
                txn(s2.txns, #[trigger] s2.versions[k][a].txn).pidx < txn(s2.txns, #[trigger] s2.versions[k][b].txn).pidx by {
                let xy = lemma_filter_order(vs, pred, a, b);
                assert(inv_version(s, c, k, vs[xy.0]));
                assert(inv_version(s, c, k, vs[xy.1]));
                lemma_crashed_txn(c, txn(s.txns, vs[xy.0].txn), i);
                lemma_crashed_txn(c, txn(s.txns, vs[xy.1].txn), i);
                assert(txn(s.txns, vs[xy.0].txn).pidx < txn(s.txns, vs[xy.1].txn).pidx);
            }
        }
    }
}

/// A version present after the crash was present before it.
pub proof fn lemma_crash_has_version(s: State, c: Constants, i: int, survive: Map<Sid, nat>, s2: State, k: int, o: int)
    requires inv_shapes(s, c), can_crash(s, c, i, survive), s2 == apply(s, c, Action::Crash { shard: i, survive }),
        has_version(s2.versions, k, o)
    ensures has_version(s.versions, k, o)
{
    lemma_crash_facts(s, c, i, survive, s2);
    let pred = |v: Version| survives_crash(s, s2.streams, i, v);
    let j = choose|j: int| 0 <= j < vers(s2.versions, k).len() && #[trigger] vers(s2.versions, k)[j].txn == o;
    assert(s2.versions.dom().contains(k));
    assert(s.versions.dom().contains(k));
    if owner(c, k) != i {
        assert(s2.versions[k] == s.versions[k]);
        assert(vers(s.versions, k)[j].txn == o);
    } else {
        assert(s2.versions[k] == s.versions[k].filter(pred));
        let x = lemma_filter_index(s.versions[k], pred, j);
        assert(vers(s.versions, k)[x].txn == o);
    }
}

pub proof fn lemma_crash_reads(s: State, c: Constants, i: int, survive: Map<Sid, nat>)
    requires inv(s, c), can_crash(s, c, i, survive)
    ensures inv_reads(apply(s, c, Action::Crash { shard: i, survive }), c)
{
    let s2 = apply(s, c, Action::Crash { shard: i, survive });
    lemma_crash_facts(s, c, i, survive, s2);
    assert forall|o: int, k: int| #[trigger] s2.txns.dom().contains(o) && #[trigger] txn(s2.txns, o).reads.dom().contains(k)
        implies inv_read(s2, c, o, k) by {
        assert(s.txns.dom().contains(o));
        let r = txn(s.txns, o);
        lemma_crashed_txn(c, r, i);
        assert(r.reads.dom().contains(k));
        assert(inv_read(s, c, o, k));
        let rd = r.reads[k];
        assert(txn(s2.txns, o).reads[k] == rd);
        if rd.writer != -1 {
            assert(inv_read_writer(s, c, o, k));
            lemma_crashed_txn(c, txn(s.txns, rd.writer), i);
            assert(inv_read_writer(s2, c, o, k));
        }
        if is_prepared_or_later(txn(s2.txns, o)) {
            assert forall|p: int| #[trigger] s2.txns.dom().contains(p) && p != o
                && is_prepared_or_later(txn(s2.txns, p)) && txn(s2.txns, p).pidx < txn(s2.txns, o).pidx
                && write_set(txn(s2.txns, p).body).contains(k) implies inv_read_order(s2, c, o, k, p) by {
                let rp = txn(s.txns, p);
                assert(s.txns.dom().contains(p));
                lemma_crashed_txn(c, rp, i);
                assert(inv_read_order(s, c, o, k, p));
                assert(inv_txn(s, c, p));
                assert(p >= 0);
                assert(pidx_of(s2.txns, p) == pidx_of(s.txns, p));
                if rd.writer != -1 {
                    assert(inv_read_writer(s, c, o, k));
                    lemma_crashed_txn(c, txn(s.txns, rd.writer), i);
                }
                assert(pidx_of(s2.txns, rd.writer) == pidx_of(s.txns, rd.writer));
                if rp.installed.contains(owner(c, k)) && has_version(s2.versions, k, p) {
                    lemma_crash_has_version(s, c, i, survive, s2, k, p);
                }
                if !rp.installed.contains(owner(c, k)) {
                    assert(rp.body.ops.dom().contains(k));
                    lemma_owner_shard(c, k);
                    assert(shard_epoch(s2.shards, owner(c, k)) >= shard_epoch(s.shards, owner(c, k)));
                }
            }
        }
    }
}

pub proof fn lemma_crash_streams(s: State, c: Constants, i: int, survive: Map<Sid, nat>)
    requires inv(s, c), can_crash(s, c, i, survive)
    ensures inv_streams(apply(s, c, Action::Crash { shard: i, survive }), c)
{
    let s2 = apply(s, c, Action::Crash { shard: i, survive });
    lemma_crash_facts(s, c, i, survive, s2);
    assert forall|sid: Sid| valid_sid(c, sid) implies #[trigger] inv_stream(s2, c, sid) by {
        assert(inv_stream(s, c, sid));
        let es = all_entries(s.streams[sid]);
        let es2 = all_entries(s2.streams[sid]);
        assert(is_prefix(es2, es));
        lemma_prefix_order(es2, es);
        assert(shard_epoch(s.shards, sid.shard) <= s.epoch);
        assert forall|j: int| 0 <= j < es2.len() implies inv_entry(s2, c, sid, #[trigger] es2[j]) by {
            assert(es2[j] == es[j]);
            assert(inv_entry(s, c, sid, es[j]));
            if sid.shard != i {
                assert(s2.shards[sid.shard] == s.shards[sid.shard]);
            }
            if es[j] is Log {
                let o = es[j]->Log_txn;
                lemma_crashed_txn(c, txn(s.txns, o), i);
                assert(logged_at(c, txn(s2.txns, o), sid.shard));
            }
        }
    }
}


// ---------------------------------------------------------------------------
// Log placement (W12, W13, W21)
// ---------------------------------------------------------------------------

pub proof fn lemma_crash_coord_below(s: State, c: Constants, i: int, survive: Map<Sid, nat>, o: int)
    requires inv(s, c), can_crash(s, c, i, survive), s.txns.dom().contains(o)
    ensures ({ let s2 = apply(s, c, Action::Crash { shard: i, survive }); inv_coord_below(s2, c, txn(s2.txns, o)) })
{
    let s2 = apply(s, c, Action::Crash { shard: i, survive });
    lemma_crash_facts(s, c, i, survive, s2);
    let r = txn(s.txns, o);
    let r2 = txn(s2.txns, o);
    lemma_crashed_txn(c, r, i);
    assert(inv_txn(s, c, o));
    assert(inv_logs(s, c, o));
    if r2.status is Prepared || aborted_prepared(r2) {
        assert(r.status is Prepared || aborted_prepared(r));
        assert(inv_coord_below(s, c, r));
        lemma_crash_stream_at(s, c, i, survive, s2, r, r.coord);
        assert(stream_at(s2.streams, r2, r2.coord) == stream_at(s2.streams, r, r.coord));
        lemma_prefix_below(stream_at(s2.streams, r, r.coord), stream_at(s.streams, r, r.coord), r.epoch, r.vc[group(c, r.coord)]);
    }
}

pub proof fn lemma_crash_uninstalled_below(s: State, c: Constants, i: int, survive: Map<Sid, nat>, o: int)
    requires inv(s, c), can_crash(s, c, i, survive), s.txns.dom().contains(o)
    ensures ({ let s2 = apply(s, c, Action::Crash { shard: i, survive }); inv_uninstalled_below(s2, c, txn(s2.txns, o)) })
{
    let s2 = apply(s, c, Action::Crash { shard: i, survive });
    lemma_crash_facts(s, c, i, survive, s2);
    let r = txn(s.txns, o);
    let r2 = txn(s2.txns, o);
    lemma_crashed_txn(c, r, i);
    assert(inv_txn(s, c, o));
    assert(inv_logs(s, c, o));
    if r2.status is Prepared {
        assert(r2 == r);
        assert forall|j: int| is_shard(c, j) && #[trigger] clock_shard(c, r2, j) && !r2.installed.contains(j) implies
            logs_below(stream_at(s2.streams, r2, j), r2.epoch, r2.vc[group(c, j)]) by {
            assert(logs_below(stream_at(s.streams, r, j), r.epoch, r.vc[group(c, j)]));
            lemma_crash_stream_at(s, c, i, survive, s2, r, j);
            lemma_prefix_logs_below(stream_at(s2.streams, r, j), stream_at(s.streams, r, j), r.epoch, r.vc[group(c, j)]);
        }
    }
}

pub proof fn lemma_crash_logged(s: State, c: Constants, i: int, survive: Map<Sid, nat>, o: int)
    requires inv(s, c), can_crash(s, c, i, survive), s.txns.dom().contains(o)
    ensures ({ let s2 = apply(s, c, Action::Crash { shard: i, survive }); inv_logged(s2, c, txn(s2.txns, o), o) })
{
    let s2 = apply(s, c, Action::Crash { shard: i, survive });
    lemma_crash_facts(s, c, i, survive, s2);
    let r = txn(s.txns, o);
    let r2 = txn(s2.txns, o);
    lemma_crashed_txn(c, r, i);
    assert(inv_txn(s, c, o));
    assert(inv_logs(s, c, o));
    assert(shard_epoch(s.shards, r.coord) <= s.epoch);
    assert forall|j: int| is_shard(c, j) && #[trigger] logged_at(c, r2, j) implies
        has_log(stream_at(s2.streams, r2, j), o)
        || (shard_epoch(s2.shards, j) > r2.epoch && stream_below(stream_at(s2.streams, r2, j), r2.epoch, r2.vc[group(c, j)])) by {
        assert(logged_at(c, r, j));
        lemma_crash_stream_at(s, c, i, survive, s2, r, j);
        let a = stream_at(s2.streams, r, j);
        let b = stream_at(s.streams, r, j);
        let x = r.vc[group(c, j)];
        assert(stream_at(s2.streams, r2, j) == a);
        assert(has_log(b, o) || (shard_epoch(s.shards, j) > r.epoch && stream_below(b, r.epoch, x)));
        if j != i {
            assert(s2.shards[j] == s.shards[j]);
        } else {
            assert(shard_epoch(s2.shards, j) > r.epoch);
            if has_log(b, o) {
                let p = choose|p: int| 0 <= p < b.len() && is_log_of(#[trigger] b[p], o);
                if p < a.len() {
                    lemma_prefix_has_log(a, b, o, p);
                } else {
                    let sid = coord_sid(r, j);
                    assert(valid_sid(c, sid));
                    assert(inv_stream(s, c, sid));
                    assert(b == all_entries(s.streams[sid]));
                    assert(inv_entry(s, c, sid, b[p]));
                    lemma_prefix_cut_below(a, b, r.epoch, x, p);
                }
            } else {
                lemma_prefix_below(a, b, r.epoch, x);
            }
        }
    }
}

pub proof fn lemma_crash_present(s: State, c: Constants, i: int, survive: Map<Sid, nat>, o: int)
    requires inv(s, c), can_crash(s, c, i, survive), s.txns.dom().contains(o)
    ensures ({ let s2 = apply(s, c, Action::Crash { shard: i, survive }); inv_present(s2, c, txn(s2.txns, o), o) })
{
    let s2 = apply(s, c, Action::Crash { shard: i, survive });
    lemma_crash_facts(s, c, i, survive, s2);
    let pred = |v: Version| survives_crash(s, s2.streams, i, v);
    let r = txn(s.txns, o);
    let r2 = txn(s2.txns, o);
    lemma_crashed_txn(c, r, i);
    assert(inv_txn(s, c, o));
    assert(inv_logs(s, c, o));
    if r2.status is Prepared || certified_or_committed(r2) {
        assert(r.status is Prepared || certified_or_committed(r));
        assert forall|k: int| #[trigger] write_set(r2.body).contains(k) && r2.installed.contains(owner(c, k))
            && !has_version(s2.versions, k, o) implies
            (doomed(s2.final_wm, c, r2) && s2.rolled_back.contains((owner(c, k), r2.epoch))) || lost(s2.streams, c, r2, o) by {
            assert(write_set(r.body).contains(k));
            if has_version(s.versions, k, o) {
                let jj = choose|jj: int| 0 <= jj < vers(s.versions, k).len() && #[trigger] vers(s.versions, k)[jj].txn == o;
                assert(s.versions.dom().contains(k));
                if owner(c, k) != i {
                    assert(s2.versions[k] == s.versions[k]);
                    assert(vers(s2.versions, k)[jj].txn == o);
                    assert(false);
                }
                let v = s.versions[k][jj];
                assert(s2.versions[k] == s.versions[k].filter(pred));
                if pred(v) {
                    let a = lemma_filter_keeps(s.versions[k], pred, jj);
                    assert(vers(s2.versions, k)[a].txn == o);
                    assert(false);
                }
                assert(v.txn == o);
                let sid = sid_of(i, r.coord, r.thread);
                assert(!durable_has_log(s2.streams[sid], o));
                // the transaction must carry an entry at shard i
                assert(r.body.ops.dom().contains(k));
                lemma_owner_shard(c, k);
                assert(writes_at(c, r.body, i));
                assert(!read_only(r));
                assert(clock_shard(c, r, i));
                assert(logged_at(c, r, i));
                assert(logged_at(c, r2, i));
                lemma_crash_stream_at(s, c, i, survive, s2, r, i);
                assert(coord_sid(r, i) == sid);
                assert(stream_at(s2.streams, r2, i) == s2.streams[sid].durable);
                assert(!has_log(stream_at(s2.streams, r2, i), o));
                assert(lost(s2.streams, c, r2, o));
            } else {
                assert(r.installed.contains(owner(c, k)));
                assert((doomed(s.final_wm, c, r) && s.rolled_back.contains((owner(c, k), r.epoch))) || lost(s.streams, c, r, o));
                if lost(s.streams, c, r, o) {
                    let j = choose|j: int| is_shard(c, j) && #[trigger] logged_at(c, r, j) && !has_log(stream_at(s.streams, r, j), o);
                    lemma_crash_stream_at(s, c, i, survive, s2, r, j);
                    lemma_prefix_no_log(stream_at(s2.streams, r, j), stream_at(s.streams, r, j), o);
                    assert(logged_at(c, r2, j));
                    assert(!has_log(stream_at(s2.streams, r2, j), o));
                }
            }
        }
    }
}

pub proof fn lemma_crash_all_logs(s: State, c: Constants, i: int, survive: Map<Sid, nat>)
    requires inv(s, c), can_crash(s, c, i, survive)
    ensures inv_all_logs(apply(s, c, Action::Crash { shard: i, survive }), c)
{
    let s2 = apply(s, c, Action::Crash { shard: i, survive });
    lemma_crash_facts(s, c, i, survive, s2);
    assert forall|o: int| #[trigger] s2.txns.dom().contains(o) implies inv_logs(s2, c, o) by {
        assert(s.txns.dom().contains(o));
        lemma_crash_coord_below(s, c, i, survive, o);
        lemma_crash_uninstalled_below(s, c, i, survive, o);
        lemma_crash_logged(s, c, i, survive, o);
        lemma_crash_present(s, c, i, survive, o);
    }
}

// ---------------------------------------------------------------------------
// Watermarks (W14, W15, W16)
// ---------------------------------------------------------------------------

pub proof fn lemma_crash_committed(s: State, c: Constants, i: int, survive: Map<Sid, nat>)
    requires inv(s, c), can_crash(s, c, i, survive)
    ensures inv_committed(apply(s, c, Action::Crash { shard: i, survive }), c)
{
    let s2 = apply(s, c, Action::Crash { shard: i, survive });
    lemma_crash_facts(s, c, i, survive, s2);
    assert forall|o: int| #[trigger] s2.txns.dom().contains(o) && committed(txn(s2.txns, o))
        implies below_wm(s2.streams, c, txn(s2.txns, o).vc, txn(s2.txns, o).epoch) by {
        assert(s.txns.dom().contains(o));
        let r = txn(s.txns, o);
        lemma_crashed_txn(c, r, i);
        assert(committed(r));
        assert(below_wm(s.streams, c, r.vc, r.epoch));
        assert forall|sid: Sid| valid_sid(c, sid) implies
            wm_le(r.vc[group(c, sid.shard)], stream_wm(#[trigger] s2.streams[sid].durable, r.epoch)) by {
            assert(wm_le(r.vc[group(c, sid.shard)], stream_wm(s.streams[sid].durable, r.epoch)));
            if sid.shard == i {
                let st = s.streams[sid];
                let xs = st.pending.take(survive[sid] as int);
                assert(s2.streams[sid].durable == st.durable + xs);
                assert(inv_stream(s, c, sid));
                assert(is_prefix(all_entries(s2.streams[sid]), all_entries(st)));
                lemma_prefix_order(st.durable + xs, all_entries(st));
                lemma_wm_monotone_append(st.durable, xs, r.epoch);
                lemma_wm_le_int_trans(r.vc[group(c, sid.shard)], stream_wm(st.durable, r.epoch), stream_wm(st.durable + xs, r.epoch));
            } else {
                assert(s2.streams[sid] == s.streams[sid]);
            }
        }
    }
}

/// Shard `i` had nothing of a finalized epoch pending, so the entries that
/// became durable leave that epoch's watermark unchanged.
pub proof fn lemma_crash_wm_other(s: State, c: Constants, i: int, survive: Map<Sid, nat>, s2: State, sid: Sid, e: nat)
    requires inv_shapes(s, c), can_crash(s, c, i, survive), s2 == apply(s, c, Action::Crash { shard: i, survive }),
        valid_sid(c, sid), no_pending_epoch(s.streams[sid], e)
    ensures stream_wm(s2.streams[sid].durable, e) == stream_wm(s.streams[sid].durable, e),
        no_pending_epoch(s2.streams[sid], e)
{
    lemma_crash_facts(s, c, i, survive, s2);
    if sid.shard == i {
        let st = s.streams[sid];
        let xs = st.pending.take(survive[sid] as int);
        assert(s2.streams[sid].durable == st.durable + xs);
        assert forall|q: int| 0 <= q < xs.len() implies entry_epoch(#[trigger] xs[q]) != e by {
            assert(xs[q] == st.pending[q]);
        }
        lemma_wm_add_other(st.durable, xs, e);
    } else {
        assert(s2.streams[sid] == s.streams[sid]);
    }
}

pub proof fn lemma_crash_final(s: State, c: Constants, i: int, survive: Map<Sid, nat>)
    requires inv(s, c), can_crash(s, c, i, survive)
    ensures inv_final(apply(s, c, Action::Crash { shard: i, survive }), c)
{
    let s2 = apply(s, c, Action::Crash { shard: i, survive });
    lemma_crash_facts(s, c, i, survive, s2);
    assert forall|j: int, e: nat| #[trigger] s2.final_wm.dom().contains((j, e)) implies inv_final_of(s2, c, j, e) by {
        assert(inv_final_of(s, c, j, e));
        assert(shard_epoch(s2.shards, j) >= shard_epoch(s.shards, j));
        assert forall|sid: Sid| valid_sid(c, sid) && sid.shard == j implies
            no_pending_epoch(#[trigger] s2.streams[sid], e)
            && wm_le_wm(s2.final_wm[(j, e)], stream_wm(s2.streams[sid].durable, e)) by {
            assert(no_pending_epoch(s.streams[sid], e) && wm_le_wm(s.final_wm[(j, e)], stream_wm(s.streams[sid].durable, e)));
            lemma_crash_wm_other(s, c, i, survive, s2, sid, e);
        }
        let w = choose|w: Sid| valid_sid(c, w) && w.shard == j && s.final_wm[(j, e)] == stream_wm(#[trigger] s.streams[w].durable, e);
        assert(no_pending_epoch(s.streams[w], e));
        lemma_crash_wm_other(s, c, i, survive, s2, w, e);
        assert(s2.final_wm[(j, e)] == stream_wm(s2.streams[w].durable, e));
    }
}

pub proof fn lemma_crash_rolled_back(s: State, c: Constants, i: int, survive: Map<Sid, nat>)
    requires inv(s, c), can_crash(s, c, i, survive)
    ensures inv_rolled_back(apply(s, c, Action::Crash { shard: i, survive }), c)
{
    let s2 = apply(s, c, Action::Crash { shard: i, survive });
    lemma_crash_facts(s, c, i, survive, s2);
    let pred = |v: Version| survives_crash(s, s2.streams, i, v);
    assert forall|j: int, e: nat| #[trigger] s2.rolled_back.contains((j, e)) implies inv_rolled_back_of(s2, c, j, e) by {
        assert(inv_rolled_back_of(s, c, j, e));
        assert forall|k: int, q: int| #[trigger] s2.versions.dom().contains(k) && owner(c, k) == j
            && 0 <= q < s2.versions[k].len() && #[trigger] s2.versions[k][q].epoch == e
            implies below_fvw(s2.final_wm, c, s2.versions[k][q].vc, e) by {
            assert(s.versions.dom().contains(k));
            if owner(c, k) == i {
                assert(s2.versions[k] == s.versions[k].filter(pred));
                let x = lemma_filter_index(s.versions[k], pred, q);
                assert(s.versions[k][x].epoch == e);
            } else {
                assert(s2.versions[k] == s.versions[k]);
                assert(s.versions[k][q].epoch == e);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// The theorem
// ---------------------------------------------------------------------------

pub proof fn lemma_crash_inv(s: State, c: Constants, i: int, survive: Map<Sid, nat>)
    requires inv(s, c), can_crash(s, c, i, survive)
    ensures inv(apply(s, c, Action::Crash { shard: i, survive }), c)
{
    lemma_crash_shapes(s, c, i, survive);
    lemma_crash_txns(s, c, i, survive);
    lemma_crash_prepared(s, c, i, survive);
    lemma_crash_exclusive(s, c, i, survive);
    lemma_crash_locks(s, c, i, survive);
    lemma_crash_versions(s, c, i, survive);
    lemma_crash_reads(s, c, i, survive);
    lemma_crash_streams(s, c, i, survive);
    lemma_crash_all_logs(s, c, i, survive);
    lemma_crash_committed(s, c, i, survive);
    lemma_crash_final(s, c, i, survive);
    lemma_crash_rolled_back(s, c, i, survive);
}

} // verus!
