//! Preservation proof for AdvanceEpoch (paper Section 5.2, Lemma 4): a lagging
//! healthy shard closes its epoch with INF markers, drops the locks held by the
//! transactions it coordinates and terminates its in-flight transactions.
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
// How AdvanceEpoch changes one transaction record and one stream
// ---------------------------------------------------------------------------

pub open spec fn adv_rec(r: TxnRec, i: int) -> TxnRec {
    if r.coord == i && in_flight(r) {
        TxnRec { status: Status::Aborted { prepared: r.status is Prepared }, ..r }
    } else { r }
}

pub open spec fn adv_stream(s: State, i: int, sid: Sid) -> Stream {
    let st = s.streams[sid];
    if gets_inf(s, i, sid) {
        Stream { pending: st.pending.push(Entry::Inf { epoch: s.shards[i].epoch }), ..st }
    } else { st }
}

pub proof fn lemma_owner_is_shard(c: Constants, k: int)
    requires c.shards >= 1
    ensures is_shard(c, owner(c, k))
{
}

pub proof fn lemma_adv_rec(c: Constants, r: TxnRec, i: int)
    ensures
        adv_rec(r, i).body == r.body, adv_rec(r, i).coord == r.coord, adv_rec(r, i).thread == r.thread,
        adv_rec(r, i).epoch == r.epoch, adv_rec(r, i).reads == r.reads, adv_rec(r, i).vc == r.vc,
        adv_rec(r, i).pidx == r.pidx, adv_rec(r, i).installed == r.installed,
        adv_rec(r, i).invoked == r.invoked, adv_rec(r, i).prepared_at == r.prepared_at,
        adv_rec(r, i).acked == r.acked,
        is_prepared_or_later(adv_rec(r, i)) == is_prepared_or_later(r),
        certified_or_committed(adv_rec(r, i)) == certified_or_committed(r),
        committed(adv_rec(r, i)) == committed(r),
        read_only(adv_rec(r, i)) == read_only(r),
        in_flight(adv_rec(r, i)) ==> adv_rec(r, i) == r && r.coord != i,
        adv_rec(r, i).status is Prepared ==> adv_rec(r, i) == r && r.coord != i && r.status is Prepared,
        adv_rec(r, i).status is Running ==> r.status is Running,
        r.status is Aborted ==> adv_rec(r, i) == r,
        !(r.coord == i && in_flight(r)) ==> adv_rec(r, i) == r,
        aborted_prepared(adv_rec(r, i)) ==> aborted_prepared(r) || (r.coord == i && r.status is Prepared),
        certified_or_committed(adv_rec(r, i)) ==> adv_rec(r, i) == r,
        forall|j: int| #[trigger] clock_shard(c, adv_rec(r, i), j) == clock_shard(c, r, j),
        forall|j: int| #[trigger] logged_at(c, adv_rec(r, i), j) == logged_at(c, r, j),
        forall|k: int| #[trigger] write_value(adv_rec(r, i), k) == write_value(r, k),
{
}

pub proof fn lemma_adv_sid(s: State, i: int, sid: Sid)
    ensures
        adv_stream(s, i, sid).durable == s.streams[sid].durable,
        gets_inf(s, i, sid) ==> sid.shard == i,
        gets_inf(s, i, sid) ==>
            adv_stream(s, i, sid).pending == s.streams[sid].pending.push(Entry::Inf { epoch: s.shards[i].epoch }),
        gets_inf(s, i, sid) ==>
            all_entries(adv_stream(s, i, sid)) == all_entries(s.streams[sid]).push(Entry::Inf { epoch: s.shards[i].epoch }),
        !gets_inf(s, i, sid) ==> adv_stream(s, i, sid) == s.streams[sid],
        sid.shard != i ==> adv_stream(s, i, sid) == s.streams[sid],
{
    let st = s.streams[sid];
    let x = Entry::Inf { epoch: s.shards[i].epoch };
    assert(st.durable + st.pending.push(x) =~= (st.durable + st.pending).push(x));
}

/// The structural effect of AdvanceEpoch on every state component.
pub proof fn lemma_advance_facts(s: State, c: Constants, i: int, s2: State)
    requires
        inv_shapes(s, c), can_advance(s, c, i),
        s2 == apply(s, c, Action::AdvanceEpoch { shard: i }),
    ensures
        s2.epoch == s.epoch, s2.versions == s.versions, s2.final_wm == s.final_wm,
        s2.rolled_back == s.rolled_back, s2.prepared == s.prepared, s2.tick == s.tick + 1,
        s2.shards.len() == c.shards,
        s2.shards[i].epoch == s.shards[i].epoch + 1, s2.shards[i].counter == 0,
        shard_epoch(s2.shards, i) == shard_epoch(s.shards, i) + 1, counter(s2.shards, i) == 0,
        forall|j: int| is_shard(c, j) && j != i ==> #[trigger] s2.shards[j] == s.shards[j],
        forall|j: int| is_shard(c, j) ==> #[trigger] s2.shards[j].epoch >= s.shards[j].epoch,
        s2.txns.dom() == s.txns.dom(),
        forall|o: int| s.txns.dom().contains(o) ==> #[trigger] s2.txns[o] == adv_rec(s.txns[o], i),
        s2.streams.dom() == s.streams.dom(),
        forall|sid: Sid| s.streams.dom().contains(sid) ==> #[trigger] s2.streams[sid] == adv_stream(s, i, sid),
        forall|k: int| #[trigger] s2.locks.dom().contains(k) <==>
            (s.locks.dom().contains(k) && s.txns[s.locks[k]].coord != i),
        forall|k: int| s2.locks.dom().contains(k) ==> #[trigger] s2.locks[k] == s.locks[k],
{
    let e = s.shards[i].epoch;
    assert(s2.shards == s.shards.update(i, ShardState { epoch: e + 1, counter: 0 }));
    assert(s2.txns == abort_coordinated(s.txns, i));
    assert(s2.streams == append_inf(s, i, e));
    assert forall|o: int| s.txns.dom().contains(o) implies #[trigger] s2.txns[o] == adv_rec(s.txns[o], i) by {}
    assert forall|sid: Sid| s.streams.dom().contains(sid) implies #[trigger] s2.streams[sid] == adv_stream(s, i, sid) by {}
    assert forall|j: int| is_shard(c, j) implies #[trigger] s2.shards[j].epoch >= s.shards[j].epoch by {
        if j != i { assert(s2.shards[j] == s.shards[j]); }
    }
}

// ---------------------------------------------------------------------------
// Appending an INF marker to a stream
// ---------------------------------------------------------------------------

pub proof fn lemma_order_push_inf(es: Seq<Entry>, ep: nat)
    requires
        inv_stream_order(es),
        forall|j: int| 0 <= j < es.len() ==> entry_epoch(#[trigger] es[j]) <= ep,
        forall|j: int| 0 <= j < es.len() && #[trigger] es[j] is Inf ==> es[j]->Inf_epoch < ep,
    ensures inv_stream_order(es.push(Entry::Inf { epoch: ep }))
{
    let x = Entry::Inf { epoch: ep };
    let es2 = es.push(x);
    let n = es.len() as int;
    assert(es2[n] == x);
    assert(forall|j: int| 0 <= j < n ==> #[trigger] es2[j] == es[j]);
    assert forall|a: int, b: int| 0 <= a < b < es2.len() implies
        entry_epoch(#[trigger] es2[a]) <= entry_epoch(#[trigger] es2[b]) by {
        assert(es2[a] == es[a]);
        if b < n { assert(es2[b] == es[b]); }
    }
    assert forall|a: int, b: int| 0 <= a < b < es2.len() && #[trigger] es2[a] is Log && #[trigger] es2[b] is Log
        && es2[a]->Log_epoch == es2[b]->Log_epoch implies es2[a]->Log_clock < es2[b]->Log_clock by {
        assert(b < n);
        assert(es2[a] == es[a] && es2[b] == es[b]);
    }
    assert forall|a: int, b: int| 0 <= a < b < es2.len() && #[trigger] es2[a] is Inf
        implies entry_epoch(#[trigger] es2[b]) > es2[a]->Inf_epoch by {
        assert(es2[a] == es[a]);
        if b < n { assert(es2[b] == es[b]); }
    }
    assert forall|a: int, b: int| 0 <= a < b < es2.len() && #[trigger] es2[a] is Log && #[trigger] es2[b] is Log
        implies es2[a]->Log_txn != es2[b]->Log_txn by {
        assert(b < n);
        assert(es2[a] == es[a] && es2[b] == es[b]);
    }
    assert forall|a: int| 0 <= a < es2.len() && #[trigger] es2[a] is Log implies es2[a]->Log_clock >= 1 by {
        assert(a < n);
        assert(es2[a] == es[a]);
    }
}

pub proof fn lemma_push_inf_has_log(es: Seq<Entry>, ep: nat, id: int)
    ensures has_log(es.push(Entry::Inf { epoch: ep }), id) == has_log(es, id)
{
    let es2 = es.push(Entry::Inf { epoch: ep });
    let n = es.len() as int;
    assert(es2[n] == Entry::Inf { epoch: ep });
    if has_log(es, id) {
        let j = choose|j: int| 0 <= j < es.len() && is_log_of(#[trigger] es[j], id);
        assert(es2[j] == es[j]);
    }
    if has_log(es2, id) {
        let j = choose|j: int| 0 <= j < es2.len() && is_log_of(#[trigger] es2[j], id);
        if j < n { assert(es2[j] == es[j]); }
    }
}

pub proof fn lemma_push_inf_logs_below(es: Seq<Entry>, ep: nat, e: nat, x: int)
    ensures logs_below(es.push(Entry::Inf { epoch: ep }), e, x) == logs_below(es, e, x)
{
    let es2 = es.push(Entry::Inf { epoch: ep });
    let n = es.len() as int;
    assert(es2[n] == Entry::Inf { epoch: ep });
    if logs_below(es, e, x) {
        assert forall|j: int| 0 <= j < es2.len() && #[trigger] es2[j] is Log && es2[j]->Log_epoch == e
            implies es2[j]->Log_clock < x by {
            assert(j < n);
            assert(es2[j] == es[j]);
        }
    }
    if logs_below(es2, e, x) {
        assert forall|j: int| 0 <= j < es.len() && #[trigger] es[j] is Log && es[j]->Log_epoch == e
            implies es[j]->Log_clock < x by {
            assert(es2[j] == es[j]);
        }
    }
}

pub proof fn lemma_push_inf_stream_below(es: Seq<Entry>, ep: nat, e: nat, x: int)
    requires stream_below(es, e, x), ep != e
    ensures stream_below(es.push(Entry::Inf { epoch: ep }), e, x)
{
    let es2 = es.push(Entry::Inf { epoch: ep });
    let n = es.len() as int;
    assert(es2[n] == Entry::Inf { epoch: ep });
    lemma_push_inf_logs_below(es, ep, e, x);
    assert forall|j: int| 0 <= j < es2.len() implies !(#[trigger] es2[j] is Inf && es2[j]->Inf_epoch == e) by {
        if j < n { assert(es2[j] == es[j]); }
    }
}

pub proof fn lemma_push_no_pending(st: Stream, x: Entry, e: nat)
    requires no_pending_epoch(st, e), entry_epoch(x) != e
    ensures no_pending_epoch(Stream { pending: st.pending.push(x), ..st }, e)
{
    let p2 = st.pending.push(x);
    assert forall|j: int| 0 <= j < p2.len() implies entry_epoch(#[trigger] p2[j]) != e by {
        if j < st.pending.len() { assert(p2[j] == st.pending[j]); }
    }
}


// ---------------------------------------------------------------------------
// Per-conjunct preservation
// ---------------------------------------------------------------------------

pub proof fn lemma_advance_shapes(s: State, c: Constants, i: int)
    requires inv(s, c), can_advance(s, c, i)
    ensures inv_shapes(apply(s, c, Action::AdvanceEpoch { shard: i }), c)
{
    let s2 = apply(s, c, Action::AdvanceEpoch { shard: i });
    lemma_advance_facts(s, c, i, s2);
    assert forall|sid: Sid| #[trigger] s2.streams.dom().contains(sid) <==> valid_sid(c, sid) by {
        assert(s.streams.dom().contains(sid) <==> valid_sid(c, sid));
    }
    assert forall|j: int| #![trigger shard_epoch(s2.shards, j)] #![trigger counter(s2.shards, j)]
        is_shard(c, j) implies shard_epoch(s2.shards, j) <= s2.epoch && counter(s2.shards, j) >= 0 by {
        assert(shard_epoch(s.shards, j) <= s.epoch && counter(s.shards, j) >= 0);
        if j != i { assert(s2.shards[j] == s.shards[j]); }
    }
}

pub proof fn lemma_advance_txn_one(s: State, c: Constants, i: int, s2: State, o: int)
    requires
        inv(s, c), can_advance(s, c, i), s2 == apply(s, c, Action::AdvanceEpoch { shard: i }),
        s.txns.dom().contains(o),
    ensures inv_txn(s2, c, o)
{
    lemma_advance_facts(s, c, i, s2);
    let r = txn(s.txns, o);
    let r2 = txn(s2.txns, o);
    lemma_adv_rec(c, r, i);
    assert(inv_txn(s, c, o));
    assert(r2 == adv_rec(r, i));
    assert(s2.shards[r.coord].epoch >= s.shards[r.coord].epoch);
    if in_flight(r2) {
        assert(s2.shards[r.coord] == s.shards[r.coord]);
    }
    if aborted_prepared(r2) && !aborted_prepared(r) {
        assert(r.coord == i && r.status is Prepared);
        assert(in_flight(r));
        assert(r.epoch == shard_epoch(s.shards, i));
    }
    assert forall|x: int| is_comp(c, x) implies #[trigger] r2.vc[x] >= 0 by { assert(r.vc[x] >= 0); }
    if is_prepared_or_later(r2) {
        assert forall|j: int| is_shard(c, j) && #[trigger] clock_shard(c, r2, j) implies r2.vc[group(c, j)] >= 1 by {
            assert(clock_shard(c, r, j));
        }
        assert forall|k: int| #[trigger] write_set(r2.body).contains(k) implies
            shard_epoch(s2.shards, owner(c, k)) >= r2.epoch by {
            lemma_owner_is_shard(c, k);
            assert(shard_epoch(s.shards, owner(c, k)) >= r.epoch);
            assert(s2.shards[owner(c, k)].epoch >= s.shards[owner(c, k)].epoch);
        }
        assert(s2.prepared[r2.pidx as int] == o);
    }
    if certified_or_committed(r2) {
        assert(all_installed(c, r));
        assert(r2 == r);
    }
}

pub proof fn lemma_advance_txns(s: State, c: Constants, i: int)
    requires inv(s, c), can_advance(s, c, i)
    ensures inv_txns(apply(s, c, Action::AdvanceEpoch { shard: i }), c)
{
    let s2 = apply(s, c, Action::AdvanceEpoch { shard: i });
    lemma_advance_facts(s, c, i, s2);
    assert forall|o: int| #[trigger] s2.txns.dom().contains(o) implies o >= 0 && inv_txn(s2, c, o) by {
        lemma_advance_txn_one(s, c, i, s2, o);
    }
}

pub proof fn lemma_advance_prepared(s: State, c: Constants, i: int)
    requires inv(s, c), can_advance(s, c, i)
    ensures inv_prepared(apply(s, c, Action::AdvanceEpoch { shard: i }), c)
{
    let s2 = apply(s, c, Action::AdvanceEpoch { shard: i });
    lemma_advance_facts(s, c, i, s2);
    assert forall|j: int| 0 <= j < s2.prepared.len() implies
        has_txn(s2.txns, #[trigger] s2.prepared[j]) && txn(s2.txns, s2.prepared[j]).pidx == j
        && is_prepared_or_later(txn(s2.txns, s2.prepared[j])) by {
        let o = s.prepared[j];
        assert(has_txn(s.txns, o));
        lemma_adv_rec(c, txn(s.txns, o), i);
    }
    assert forall|j1: int, j2: int| 0 <= j1 < j2 < s2.prepared.len() implies
        txn(s2.txns, #[trigger] s2.prepared[j1]).prepared_at < txn(s2.txns, #[trigger] s2.prepared[j2]).prepared_at by {
        let o1 = s.prepared[j1];
        let o2 = s.prepared[j2];
        assert(has_txn(s.txns, o1));
        assert(has_txn(s.txns, o2));
        lemma_adv_rec(c, txn(s.txns, o1), i);
        lemma_adv_rec(c, txn(s.txns, o2), i);
    }
}

pub proof fn lemma_advance_exclusive(s: State, c: Constants, i: int)
    requires inv(s, c), can_advance(s, c, i)
    ensures inv_exclusive(apply(s, c, Action::AdvanceEpoch { shard: i }), c)
{
    let s2 = apply(s, c, Action::AdvanceEpoch { shard: i });
    lemma_advance_facts(s, c, i, s2);
    assert forall|a: int, b: int| #[trigger] s2.txns.dom().contains(a) && #[trigger] s2.txns.dom().contains(b) && a != b
        && txn(s2.txns, a).coord == txn(s2.txns, b).coord && txn(s2.txns, a).thread == txn(s2.txns, b).thread
        implies !(in_flight(txn(s2.txns, a)) && in_flight(txn(s2.txns, b))) by {
        lemma_adv_rec(c, txn(s.txns, a), i);
        lemma_adv_rec(c, txn(s.txns, b), i);
        assert(s.txns.dom().contains(a) && s.txns.dom().contains(b));
    }
}

pub proof fn lemma_advance_locks(s: State, c: Constants, i: int)
    requires inv(s, c), can_advance(s, c, i)
    ensures inv_locks(apply(s, c, Action::AdvanceEpoch { shard: i }), c)
{
    let s2 = apply(s, c, Action::AdvanceEpoch { shard: i });
    lemma_advance_facts(s, c, i, s2);
    assert forall|k: int| #[trigger] s2.locks.dom().contains(k) implies inv_lock(s2, c, k) by {
        assert(s.locks.dom().contains(k));
        assert(inv_lock(s, c, k));
        let h = s.locks[k];
        assert(s2.locks[k] == h);
        assert(s.txns[h].coord != i);
        lemma_adv_rec(c, txn(s.txns, h), i);
        assert(txn(s2.txns, h) == txn(s.txns, h));
        assert forall|j: int| 0 <= j < vers(s2.versions, k).len() implies
            pidx_of(s2.txns, #[trigger] vers(s2.versions, k)[j].txn) < txn(s2.txns, h).pidx by {
            assert(pidx_of(s.txns, vers(s.versions, k)[j].txn) < txn(s.txns, h).pidx);
            assert(s.versions.dom().contains(k));
            assert(inv_versions_of(s, c, k));
            assert(inv_version(s, c, k, s.versions[k][j]));
            let v = s.versions[k][j].txn;
            lemma_adv_rec(c, txn(s.txns, v), i);
            assert(v != -1);
        }
    }
    assert forall|o: int, k: int| #[trigger] s2.txns.dom().contains(o) && txn(s2.txns, o).status is Prepared
        && #[trigger] write_set(txn(s2.txns, o).body).contains(k)
        && !txn(s2.txns, o).installed.contains(owner(c, k))
        && shard_epoch(s2.shards, owner(c, k)) == txn(s2.txns, o).epoch
        implies s2.locks.dom().contains(k) && s2.locks[k] == o by {
        let r = txn(s.txns, o);
        lemma_adv_rec(c, r, i);
        assert(r.status is Prepared && r.coord != i);
        assert(txn(s2.txns, o) == r);
        assert(inv_txn(s, c, o));
        lemma_owner_is_shard(c, k);
        assert(shard_epoch(s.shards, owner(c, k)) >= r.epoch);
        if owner(c, k) == i {
            assert(false);
        } else {
            assert(s2.shards[owner(c, k)] == s.shards[owner(c, k)]);
            assert(s.locks.dom().contains(k) && s.locks[k] == o);
        }
    }
}

pub proof fn lemma_advance_versions(s: State, c: Constants, i: int)
    requires inv(s, c), can_advance(s, c, i)
    ensures inv_versions(apply(s, c, Action::AdvanceEpoch { shard: i }), c)
{
    let s2 = apply(s, c, Action::AdvanceEpoch { shard: i });
    lemma_advance_facts(s, c, i, s2);
    assert forall|k: int| #[trigger] s2.versions.dom().contains(k) implies inv_versions_of(s2, c, k) by {
        assert(inv_versions_of(s, c, k));
        assert forall|j: int| 0 <= j < s2.versions[k].len() implies inv_version(s2, c, k, #[trigger] s2.versions[k][j]) by {
            assert(inv_version(s, c, k, s.versions[k][j]));
            lemma_adv_rec(c, txn(s.txns, s.versions[k][j].txn), i);
        }
        assert forall|a: int, b: int| 0 <= a < b < s2.versions[k].len() implies
            txn(s2.txns, #[trigger] s2.versions[k][a].txn).pidx < txn(s2.txns, #[trigger] s2.versions[k][b].txn).pidx by {
            assert(inv_version(s, c, k, s.versions[k][a]));
            assert(inv_version(s, c, k, s.versions[k][b]));
            lemma_adv_rec(c, txn(s.txns, s.versions[k][a].txn), i);
            lemma_adv_rec(c, txn(s.txns, s.versions[k][b].txn), i);
        }
    }
}

pub proof fn lemma_advance_read_one(s: State, c: Constants, i: int, s2: State, o: int, k: int)
    requires
        inv(s, c), can_advance(s, c, i), s2 == apply(s, c, Action::AdvanceEpoch { shard: i }),
        s.txns.dom().contains(o), txn(s.txns, o).reads.dom().contains(k),
    ensures inv_read(s2, c, o, k)
{
    lemma_advance_facts(s, c, i, s2);
    let r = txn(s.txns, o);
    let r2 = txn(s2.txns, o);
    lemma_adv_rec(c, r, i);
    assert(r2 == adv_rec(r, i));
    assert(inv_read(s, c, o, k));
    let rd = r.reads[k];
    assert(r2.reads[k] == rd);
    if rd.writer != -1 {
        assert(inv_read_writer(s, c, o, k));
        lemma_adv_rec(c, txn(s.txns, rd.writer), i);
        assert(inv_read_writer(s2, c, o, k));
    }
    if is_prepared_or_later(r2) {
        assert forall|p: int| #[trigger] s2.txns.dom().contains(p) && p != o
            && is_prepared_or_later(txn(s2.txns, p)) && txn(s2.txns, p).pidx < txn(s2.txns, o).pidx
            && write_set(txn(s2.txns, p).body).contains(k) implies inv_read_order(s2, c, o, k, p) by {
            let rp = txn(s.txns, p);
            lemma_adv_rec(c, rp, i);
            assert(inv_read_order(s, c, o, k, p));
            if rd.writer != -1 {
                assert(inv_read_writer(s, c, o, k));
                lemma_adv_rec(c, txn(s.txns, rd.writer), i);
                assert(pidx_of(s2.txns, rd.writer) == pidx_of(s.txns, rd.writer));
            }
            assert(pidx_of(s2.txns, p) == pidx_of(s.txns, p));
            if !rp.installed.contains(owner(c, k)) {
                lemma_owner_is_shard(c, k);
                assert(s2.shards[owner(c, k)].epoch >= s.shards[owner(c, k)].epoch);
            }
        }
    }
}

pub proof fn lemma_advance_reads(s: State, c: Constants, i: int)
    requires inv(s, c), can_advance(s, c, i)
    ensures inv_reads(apply(s, c, Action::AdvanceEpoch { shard: i }), c)
{
    let s2 = apply(s, c, Action::AdvanceEpoch { shard: i });
    lemma_advance_facts(s, c, i, s2);
    assert forall|o: int, k: int| #[trigger] s2.txns.dom().contains(o) && #[trigger] txn(s2.txns, o).reads.dom().contains(k)
        implies inv_read(s2, c, o, k) by {
        lemma_adv_rec(c, txn(s.txns, o), i);
        lemma_advance_read_one(s, c, i, s2, o, k);
    }
}

/// An entry of a stream of the advancing shard keeps its invariant.
pub proof fn lemma_advance_entry(s: State, c: Constants, i: int, s2: State, sid: Sid, en: Entry)
    requires
        inv_shapes(s, c), can_advance(s, c, i), s2 == apply(s, c, Action::AdvanceEpoch { shard: i }),
        valid_sid(c, sid), inv_entry(s, c, sid, en),
    ensures inv_entry(s2, c, sid, en)
{
    lemma_advance_facts(s, c, i, s2);
    assert(s2.shards[sid.shard].epoch >= s.shards[sid.shard].epoch);
    if sid.shard != i {
        assert(s2.shards[sid.shard] == s.shards[sid.shard]);
    }
    if en is Log {
        let t = en->Log_txn;
        lemma_adv_rec(c, txn(s.txns, t), i);
        assert(txn(s2.txns, t) == adv_rec(txn(s.txns, t), i));
    }
}

pub proof fn lemma_advance_stream_one(s: State, c: Constants, i: int, s2: State, sid: Sid)
    requires
        inv(s, c), can_advance(s, c, i), s2 == apply(s, c, Action::AdvanceEpoch { shard: i }),
        valid_sid(c, sid),
    ensures inv_stream(s2, c, sid)
{
    lemma_advance_facts(s, c, i, s2);
    lemma_adv_sid(s, i, sid);
    assert(s2.streams[sid] == adv_stream(s, i, sid));
    assert(inv_stream(s, c, sid));
    let es = all_entries(s.streams[sid]);
    let es2 = all_entries(s2.streams[sid]);
    let ep = s.shards[i].epoch;
    if gets_inf(s, i, sid) {
        assert(sid.shard == i);
        assert(es2 == es.push(Entry::Inf { epoch: ep }));
        assert forall|j: int| 0 <= j < es.len() implies entry_epoch(#[trigger] es[j]) <= ep by {
            assert(inv_entry(s, c, sid, es[j]));
        }
        assert forall|j: int| 0 <= j < es.len() && #[trigger] es[j] is Inf implies es[j]->Inf_epoch < ep by {
            assert(inv_entry(s, c, sid, es[j]));
        }
        lemma_order_push_inf(es, ep);
        assert forall|j: int| 0 <= j < es2.len() implies inv_entry(s2, c, sid, #[trigger] es2[j]) by {
            if j < es.len() {
                assert(es2[j] == es[j]);
                assert(inv_entry(s, c, sid, es[j]));
                lemma_advance_entry(s, c, i, s2, sid, es[j]);
            } else {
                assert(es2[j] == Entry::Inf { epoch: ep });
            }
        }
    } else {
        assert(es2 == es);
        assert forall|j: int| 0 <= j < es2.len() implies inv_entry(s2, c, sid, #[trigger] es2[j]) by {
            assert(inv_entry(s, c, sid, es[j]));
            lemma_advance_entry(s, c, i, s2, sid, es[j]);
        }
    }
}

pub proof fn lemma_advance_streams(s: State, c: Constants, i: int)
    requires inv(s, c), can_advance(s, c, i)
    ensures inv_streams(apply(s, c, Action::AdvanceEpoch { shard: i }), c)
{
    let s2 = apply(s, c, Action::AdvanceEpoch { shard: i });
    assert forall|sid: Sid| valid_sid(c, sid) implies #[trigger] inv_stream(s2, c, sid) by {
        lemma_advance_stream_one(s, c, i, s2, sid);
    }
}

pub proof fn lemma_advance_coord_below(s: State, c: Constants, i: int, s2: State, o: int)
    requires
        inv(s, c), can_advance(s, c, i), s2 == apply(s, c, Action::AdvanceEpoch { shard: i }),
        s.txns.dom().contains(o),
    ensures inv_coord_below(s2, c, txn(s2.txns, o))
{
    lemma_advance_facts(s, c, i, s2);
    let r = txn(s.txns, o);
    let r2 = txn(s2.txns, o);
    lemma_adv_rec(c, r, i);
    assert(r2 == adv_rec(r, i));
    assert(inv_txn(s, c, o));
    assert(inv_logs(s, c, o));
    let sid = coord_sid(r, r.coord);
    assert(valid_sid(c, sid));
    lemma_adv_sid(s, i, sid);
    assert(s2.streams[sid] == adv_stream(s, i, sid));
    assert(coord_sid(r2, r2.coord) == sid);
    if r2.status is Prepared || aborted_prepared(r2) {
        let es = all_entries(s.streams[sid]);
        let x = r.vc[group(c, r.coord)];
        assert(r.status is Prepared || aborted_prepared(r));
        assert(stream_below(es, r.epoch, x));
        if gets_inf(s, i, sid) {
            assert(r.coord == i);
            if r.status is Prepared {
                assert(s.txns.dom().contains(o) && s.txns[o].coord == i && s.txns[o].thread == r.thread
                    && s.txns[o].status is Prepared);
                assert(hung_thread(s, i, r.thread));
                assert(false);
            }
            assert(aborted_prepared(r));
            assert(r.epoch < shard_epoch(s.shards, i));
            lemma_push_inf_stream_below(es, s.shards[i].epoch, r.epoch, x);
        }
    }
}

pub proof fn lemma_advance_uninstalled(s: State, c: Constants, i: int, s2: State, o: int)
    requires
        inv(s, c), can_advance(s, c, i), s2 == apply(s, c, Action::AdvanceEpoch { shard: i }),
        s.txns.dom().contains(o),
    ensures inv_uninstalled_below(s2, c, txn(s2.txns, o))
{
    lemma_advance_facts(s, c, i, s2);
    let r = txn(s.txns, o);
    let r2 = txn(s2.txns, o);
    lemma_adv_rec(c, r, i);
    assert(r2 == adv_rec(r, i));
    assert(inv_txn(s, c, o));
    assert(inv_logs(s, c, o));
    if r2.status is Prepared {
        assert(r2 == r);
        assert forall|j: int| is_shard(c, j) && #[trigger] clock_shard(c, r2, j) && !r2.installed.contains(j) implies
            logs_below(stream_at(s2.streams, r2, j), r2.epoch, r2.vc[group(c, j)]) by {
            let sid = coord_sid(r, j);
            assert(valid_sid(c, sid));
            lemma_adv_sid(s, i, sid);
            assert(s2.streams[sid] == adv_stream(s, i, sid));
            assert(logs_below(stream_at(s.streams, r, j), r.epoch, r.vc[group(c, j)]));
            if gets_inf(s, i, sid) {
                lemma_push_inf_logs_below(all_entries(s.streams[sid]), s.shards[i].epoch, r.epoch, r.vc[group(c, j)]);
            }
        }
    }
}

pub proof fn lemma_advance_logged(s: State, c: Constants, i: int, s2: State, o: int)
    requires
        inv(s, c), can_advance(s, c, i), s2 == apply(s, c, Action::AdvanceEpoch { shard: i }),
        s.txns.dom().contains(o),
    ensures inv_logged(s2, c, txn(s2.txns, o), o)
{
    lemma_advance_facts(s, c, i, s2);
    let r = txn(s.txns, o);
    let r2 = txn(s2.txns, o);
    lemma_adv_rec(c, r, i);
    assert(r2 == adv_rec(r, i));
    assert(inv_txn(s, c, o));
    assert(inv_logs(s, c, o));
    assert forall|j: int| is_shard(c, j) && #[trigger] logged_at(c, r2, j) implies
        has_log(stream_at(s2.streams, r2, j), o)
        || (shard_epoch(s2.shards, j) > r2.epoch && stream_below(stream_at(s2.streams, r2, j), r2.epoch, r2.vc[group(c, j)])) by {
        assert(logged_at(c, r, j));
        let sid = coord_sid(r, j);
        assert(valid_sid(c, sid));
        lemma_adv_sid(s, i, sid);
        assert(s2.streams[sid] == adv_stream(s, i, sid));
        assert(coord_sid(r2, j) == sid);
        let es = all_entries(s.streams[sid]);
        let ep = s.shards[i].epoch;
        let x = r.vc[group(c, j)];
        assert(s2.shards[j].epoch >= s.shards[j].epoch);
        assert(has_log(es, o) || (shard_epoch(s.shards, j) > r.epoch && stream_below(es, r.epoch, x)));
        if gets_inf(s, i, sid) {
            lemma_push_inf_has_log(es, ep, o);
            if !has_log(es, o) {
                assert(j == i);
                lemma_push_inf_stream_below(es, ep, r.epoch, x);
            }
        }
    }
}

pub proof fn lemma_advance_present(s: State, c: Constants, i: int, s2: State, o: int)
    requires
        inv(s, c), can_advance(s, c, i), s2 == apply(s, c, Action::AdvanceEpoch { shard: i }),
        s.txns.dom().contains(o),
    ensures inv_present(s2, c, txn(s2.txns, o), o)
{
    lemma_advance_facts(s, c, i, s2);
    let r = txn(s.txns, o);
    let r2 = txn(s2.txns, o);
    lemma_adv_rec(c, r, i);
    assert(r2 == adv_rec(r, i));
    assert(inv_logs(s, c, o));
    if r2.status is Prepared || certified_or_committed(r2) {
        assert(r2 == r);
        assert forall|k: int| #[trigger] write_set(r2.body).contains(k) && r2.installed.contains(owner(c, k))
            && !has_version(s2.versions, k, o) implies
            (doomed(s2.final_wm, c, r2) && s2.rolled_back.contains((owner(c, k), r2.epoch))) || lost(s2.streams, c, r2, o) by {
            if lost(s.streams, c, r, o) {
                let j = choose|j: int| is_shard(c, j) && #[trigger] logged_at(c, r, j) && !has_log(stream_at(s.streams, r, j), o);
                assert(inv_txn(s, c, o));
                let sid = coord_sid(r, j);
                assert(valid_sid(c, sid));
                lemma_adv_sid(s, i, sid);
                assert(s2.streams[sid] == adv_stream(s, i, sid));
                if gets_inf(s, i, sid) {
                    lemma_push_inf_has_log(all_entries(s.streams[sid]), s.shards[i].epoch, o);
                }
                assert(!has_log(stream_at(s2.streams, r, j), o));
            }
        }
    }
}

pub proof fn lemma_advance_all_logs(s: State, c: Constants, i: int)
    requires inv(s, c), can_advance(s, c, i)
    ensures inv_all_logs(apply(s, c, Action::AdvanceEpoch { shard: i }), c)
{
    let s2 = apply(s, c, Action::AdvanceEpoch { shard: i });
    lemma_advance_facts(s, c, i, s2);
    assert forall|o: int| #[trigger] s2.txns.dom().contains(o) implies inv_logs(s2, c, o) by {
        lemma_advance_coord_below(s, c, i, s2, o);
        lemma_advance_uninstalled(s, c, i, s2, o);
        lemma_advance_logged(s, c, i, s2, o);
        lemma_advance_present(s, c, i, s2, o);
    }
}

pub proof fn lemma_advance_committed(s: State, c: Constants, i: int)
    requires inv(s, c), can_advance(s, c, i)
    ensures inv_committed(apply(s, c, Action::AdvanceEpoch { shard: i }), c)
{
    let s2 = apply(s, c, Action::AdvanceEpoch { shard: i });
    lemma_advance_facts(s, c, i, s2);
    assert forall|o: int| #[trigger] s2.txns.dom().contains(o) && committed(txn(s2.txns, o))
        implies below_wm(s2.streams, c, txn(s2.txns, o).vc, txn(s2.txns, o).epoch) by {
        let r = txn(s.txns, o);
        lemma_adv_rec(c, r, i);
        assert(committed(r));
        assert(below_wm(s.streams, c, r.vc, r.epoch));
        assert forall|sid: Sid| valid_sid(c, sid) implies
            wm_le(r.vc[group(c, sid.shard)], stream_wm(#[trigger] s2.streams[sid].durable, r.epoch)) by {
            lemma_adv_sid(s, i, sid);
            assert(s2.streams[sid].durable == s.streams[sid].durable);
            assert(wm_le(r.vc[group(c, sid.shard)], stream_wm(s.streams[sid].durable, r.epoch)));
        }
    }
}

pub proof fn lemma_advance_final(s: State, c: Constants, i: int)
    requires inv(s, c), can_advance(s, c, i)
    ensures inv_final(apply(s, c, Action::AdvanceEpoch { shard: i }), c)
{
    let s2 = apply(s, c, Action::AdvanceEpoch { shard: i });
    lemma_advance_facts(s, c, i, s2);
    assert forall|j: int, e: nat| #[trigger] s2.final_wm.dom().contains((j, e)) implies inv_final_of(s2, c, j, e) by {
        assert(inv_final_of(s, c, j, e));
        assert(s2.shards[j].epoch >= s.shards[j].epoch);
        assert forall|sid: Sid| valid_sid(c, sid) && sid.shard == j implies
            no_pending_epoch(#[trigger] s2.streams[sid], e)
            && wm_le_wm(s2.final_wm[(j, e)], stream_wm(s2.streams[sid].durable, e)) by {
            lemma_adv_sid(s, i, sid);
            assert(s2.streams[sid] == adv_stream(s, i, sid));
            assert(no_pending_epoch(s.streams[sid], e));
            if gets_inf(s, i, sid) {
                assert(j == i);
                lemma_push_no_pending(s.streams[sid], Entry::Inf { epoch: s.shards[i].epoch }, e);
            }
        }
        let w = choose|w: Sid| valid_sid(c, w) && w.shard == j && s.final_wm[(j, e)] == stream_wm(#[trigger] s.streams[w].durable, e);
        lemma_adv_sid(s, i, w);
        assert(s2.streams[w].durable == s.streams[w].durable);
        assert(s2.final_wm[(j, e)] == stream_wm(s2.streams[w].durable, e));
    }
}

pub proof fn lemma_advance_rolled_back(s: State, c: Constants, i: int)
    requires inv(s, c), can_advance(s, c, i)
    ensures inv_rolled_back(apply(s, c, Action::AdvanceEpoch { shard: i }), c)
{
    let s2 = apply(s, c, Action::AdvanceEpoch { shard: i });
    lemma_advance_facts(s, c, i, s2);
    assert forall|j: int, e: nat| #[trigger] s2.rolled_back.contains((j, e)) implies inv_rolled_back_of(s2, c, j, e) by {
        assert(inv_rolled_back_of(s, c, j, e));
    }
}

// ---------------------------------------------------------------------------
// The action
// ---------------------------------------------------------------------------

pub proof fn lemma_advance_inv(s: State, c: Constants, i: int)
    requires inv(s, c), can_advance(s, c, i)
    ensures inv(apply(s, c, Action::AdvanceEpoch { shard: i }), c)
{
    lemma_advance_shapes(s, c, i);
    lemma_advance_txns(s, c, i);
    lemma_advance_prepared(s, c, i);
    lemma_advance_exclusive(s, c, i);
    lemma_advance_locks(s, c, i);
    lemma_advance_versions(s, c, i);
    lemma_advance_reads(s, c, i);
    lemma_advance_streams(s, c, i);
    lemma_advance_all_logs(s, c, i);
    lemma_advance_committed(s, c, i);
    lemma_advance_final(s, c, i);
    lemma_advance_rolled_back(s, c, i);
}

} // verus!
