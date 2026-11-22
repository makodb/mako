//! Preservation proof for Certify: the coordinator logs the transaction to its
//! own worker stream (pending part), raises its counter to the logged clock and
//! marks the transaction Certified.
use super::types::*;
use super::normal::*;
use super::recovery::*;
use super::behavior::*;
use super::invariants::*;
use super::stream_lemmas::*;
use super::proofs_basic::*;
use vstd::prelude::*;

verus! {

/// The coordinator stream Certify appends to.
pub open spec fn cert_sid(s: State, id: int) -> Sid {
    coord_sid(txn(s.txns, id), txn(s.txns, id).coord)
}

/// The entry Certify appends.
pub open spec fn cert_entry(s: State, c: Constants, id: int) -> Entry {
    log_entry(c, txn(s.txns, id), id, txn(s.txns, id).coord)
}

// ---------------------------------------------------------------------------
// Generic stream facts
// ---------------------------------------------------------------------------

/// Appending a log entry that is above every same-epoch log entry, not below
/// the epoch of any entry, after no INF of its epoch, and of a fresh
/// transaction keeps the stream ordered.
pub proof fn lemma_order_push(es: Seq<Entry>, x: Entry)
    requires
        inv_stream_order(es),
        x is Log,
        x->Log_clock >= 1,
        forall|a: int| 0 <= a < es.len() ==> entry_epoch(#[trigger] es[a]) <= entry_epoch(x),
        no_inf(es, entry_epoch(x)),
        logs_below(es, entry_epoch(x), x->Log_clock),
        forall|a: int| 0 <= a < es.len() && #[trigger] es[a] is Log ==> es[a]->Log_txn != x->Log_txn,
    ensures inv_stream_order(es.push(x))
{
    let es2 = es.push(x);
    let n = es.len() as int;
    assert(es2.len() == n + 1);
    assert(es2[n] == x);
    assert(forall|a: int| 0 <= a < n ==> #[trigger] es2[a] == es[a]);
    assert forall|a: int, b: int| 0 <= a < b < es2.len() implies
        entry_epoch(#[trigger] es2[a]) <= entry_epoch(#[trigger] es2[b]) by {
        assert(es2[a] == es[a]);
        if b < n { assert(es2[b] == es[b]); }
    }
    assert forall|a: int, b: int| 0 <= a < b < es2.len() && #[trigger] es2[a] is Log && #[trigger] es2[b] is Log
        && es2[a]->Log_epoch == es2[b]->Log_epoch implies es2[a]->Log_clock < es2[b]->Log_clock by {
        assert(es2[a] == es[a]);
        if b < n { assert(es2[b] == es[b]); }
    }
    assert forall|a: int, b: int| 0 <= a < b < es2.len() && #[trigger] es2[a] is Inf
        implies entry_epoch(#[trigger] es2[b]) > es2[a]->Inf_epoch by {
        assert(es2[a] == es[a]);
        if b < n { assert(es2[b] == es[b]); }
        else {
            assert(entry_epoch(es[a]) <= entry_epoch(x));
            assert(!(es[a] is Inf && es[a]->Inf_epoch == entry_epoch(x)));
        }
    }
    assert forall|a: int, b: int| 0 <= a < b < es2.len() && #[trigger] es2[a] is Log && #[trigger] es2[b] is Log
        implies es2[a]->Log_txn != es2[b]->Log_txn by {
        assert(es2[a] == es[a]);
        if b < n { assert(es2[b] == es[b]); }
    }
    assert forall|a: int| 0 <= a < es2.len() && #[trigger] es2[a] is Log implies es2[a]->Log_clock >= 1 by {
        if a < n { assert(es2[a] == es[a]); }
    }
}

/// Appending an entry of another epoch keeps `stream_below`.
pub proof fn lemma_below_push_other(es: Seq<Entry>, x: Entry, e: nat, v: int)
    requires stream_below(es, e, v), entry_epoch(x) != e
    ensures stream_below(es.push(x), e, v)
{
    let es2 = es.push(x);
    let n = es.len() as int;
    assert(es2[n] == x);
    assert forall|j: int| 0 <= j < es2.len() && #[trigger] es2[j] is Log && es2[j]->Log_epoch == e
        implies es2[j]->Log_clock < v by {
        if j < n { assert(es2[j] == es[j]); }
    }
    assert forall|j: int| 0 <= j < es2.len() implies !(#[trigger] es2[j] is Inf && es2[j]->Inf_epoch == e) by {
        if j < n { assert(es2[j] == es[j]); }
    }
}

/// Appending keeps an existing log.
pub proof fn lemma_has_log_push(es: Seq<Entry>, x: Entry, o: int)
    requires has_log(es, o)
    ensures has_log(es.push(x), o)
{
    let j = choose|j: int| 0 <= j < es.len() && is_log_of(#[trigger] es[j], o);
    assert(es.push(x)[j] == es[j]);
}

/// Appending an entry of another transaction does not create a log.
pub proof fn lemma_no_log_push(es: Seq<Entry>, x: Entry, o: int)
    requires !has_log(es, o), !is_log_of(x, o)
    ensures !has_log(es.push(x), o)
{
    let es2 = es.push(x);
    let n = es.len() as int;
    if has_log(es2, o) {
        let j = choose|j: int| 0 <= j < es2.len() && is_log_of(#[trigger] es2[j], o);
        if j < n {
            assert(es2[j] == es[j]);
        } else {
            assert(es2[n] == x);
        }
        assert(false);
    }
}

// ---------------------------------------------------------------------------
// The shape of the post-state
// ---------------------------------------------------------------------------

pub proof fn lemma_certify_setup(s: State, c: Constants, id: int)
    requires inv(s, c), can_certify(s, c, id)
    ensures ({
        let s2 = apply(s, c, Action::Certify { id });
        let r = txn(s.txns, id);
        let r2 = txn(s2.txns, id);
        let sid = cert_sid(s, id);
        let x = cert_entry(s, c, id);
        let es = all_entries(s.streams[sid]);
        let es2 = all_entries(s2.streams[sid]);
        &&& inv_txn(s, c, id)
        &&& id >= 0
        &&& r.status is Prepared
        &&& !read_only(r)
        &&& clock_shard(c, r, r.coord)
        &&& is_shard(c, r.coord)
        &&& is_thread(c, r.thread)
        &&& r.epoch == shard_epoch(s.shards, r.coord)
        &&& all_installed(c, r)
        &&& r2 == TxnRec { status: Status::Certified, ..r }
        &&& s2.txns.dom() == s.txns.dom()
        &&& forall|o: int| o != id ==> #[trigger] txn(s2.txns, o) == txn(s.txns, o)
        &&& s2.shards.len() == s.shards.len()
        &&& forall|i: int| #[trigger] shard_epoch(s2.shards, i) == shard_epoch(s.shards, i)
        &&& forall|i: int| i != r.coord ==> #[trigger] counter(s2.shards, i) == counter(s.shards, i)
        &&& counter(s2.shards, r.coord) >= counter(s.shards, r.coord)
        &&& counter(s2.shards, r.coord) >= r.vc[group(c, r.coord)]
        &&& sid == sid_of(r.coord, r.coord, r.thread)
        &&& valid_sid(c, sid)
        &&& s.streams.dom().contains(sid)
        &&& s2.streams.dom() == s.streams.dom()
        &&& forall|sid2: Sid| sid2 != sid ==> #[trigger] s2.streams[sid2] == s.streams[sid2]
        &&& forall|sid2: Sid| #[trigger] s2.streams[sid2].durable == s.streams[sid2].durable
        &&& s2.streams[sid].pending == s.streams[sid].pending.push(x)
        &&& x == (Entry::Log { txn: id, epoch: r.epoch, clock: r.vc[group(c, r.coord)] })
        &&& x->Log_clock >= 1
        &&& es2 == es.push(x)
        &&& es == stream_at(s.streams, r, r.coord)
        &&& stream_below(es, r.epoch, x->Log_clock)
        &&& forall|a: int| 0 <= a < es.len() && #[trigger] es[a] is Log ==> es[a]->Log_txn != id
        &&& s2.epoch == s.epoch
        &&& s2.versions == s.versions
        &&& s2.locks == s.locks
        &&& s2.final_wm == s.final_wm
        &&& s2.rolled_back == s.rolled_back
        &&& s2.prepared == s.prepared
        &&& s2.tick == s.tick + 1
    })
{
    let s2 = apply(s, c, Action::Certify { id });
    let r = txn(s.txns, id);
    let r2 = txn(s2.txns, id);
    let sid = cert_sid(s, id);
    let x = cert_entry(s, c, id);
    let st = s.streams[sid];
    let es = all_entries(st);
    let es2 = all_entries(s2.streams[sid]);
    assert(s.txns.dom().contains(id));
    assert(inv_txn(s, c, id));
    assert(r.status is Prepared);
    assert(!read_only(r));
    assert(clock_shard(c, r, r.coord));
    assert(r.epoch == shard_epoch(s.shards, r.coord));
    // transactions
    assert(s2.txns == s.txns.insert(id, TxnRec { status: Status::Certified, ..r }));
    assert(s2.txns.dom() =~= s.txns.dom());
    // shards
    let ns = ShardState { counter: imax(s.shards[r.coord].counter, r.vc[group(c, r.coord)]), ..s.shards[r.coord] };
    assert(s2.shards == s.shards.update(r.coord, ns));
    assert(s.shards.len() == c.shards);
    assert(s2.shards[r.coord] == ns);
    assert forall|i: int| #[trigger] shard_epoch(s2.shards, i) == shard_epoch(s.shards, i) by {
        if i != r.coord { assert(s2.shards[i] == s.shards[i]); }
    }
    assert forall|i: int| i != r.coord implies #[trigger] counter(s2.shards, i) == counter(s.shards, i) by {
        assert(s2.shards[i] == s.shards[i]);
    }
    // streams
    assert(valid_sid(c, sid));
    assert(s.streams.dom().contains(sid));
    assert(s2.streams == s.streams.insert(sid, Stream { pending: st.pending.push(x), ..st }));
    assert(s2.streams.dom() =~= s.streams.dom());
    assert(es2 =~= es.push(x));
    // the clock of the new entry
    assert(is_prepared_or_later(r));
    assert(r.vc[group(c, r.coord)] >= 1);
    // before certify, the coordinator stream is below the clock
    assert(inv_logs(s, c, id));
    assert(inv_coord_below(s, c, r));
    // and carries no entry of id
    assert(inv_stream(s, c, sid));
    assert forall|a: int| 0 <= a < es.len() && #[trigger] es[a] is Log implies es[a]->Log_txn != id by {
        assert(inv_entry(s, c, sid, es[a]));
        if es[a]->Log_txn == id {
            assert(logged_at(c, r, r.coord));
            assert(false);
        }
    }
}

// ---------------------------------------------------------------------------
// One lemma per conjunct
// ---------------------------------------------------------------------------

pub proof fn lemma_certify_shapes(s: State, c: Constants, id: int)
    requires inv(s, c), can_certify(s, c, id)
    ensures inv_shapes(apply(s, c, Action::Certify { id }), c)
{
    lemma_certify_setup(s, c, id);
    let s2 = apply(s, c, Action::Certify { id });
    let r = txn(s.txns, id);
    assert forall|i: int| is_shard(c, i) implies
        #[trigger] shard_epoch(s2.shards, i) <= s2.epoch && counter(s2.shards, i) >= 0 by {
        assert(shard_epoch(s.shards, i) <= s.epoch && counter(s.shards, i) >= 0);
        if i != r.coord {
            assert(counter(s2.shards, i) == counter(s.shards, i));
        }
    }
    assert forall|sid: Sid| #[trigger] s2.streams.dom().contains(sid) <==> valid_sid(c, sid) by {
        assert(s.streams.dom().contains(sid) <==> valid_sid(c, sid));
    }
}

pub proof fn lemma_certify_txns(s: State, c: Constants, id: int)
    requires inv(s, c), can_certify(s, c, id)
    ensures inv_txns(apply(s, c, Action::Certify { id }), c)
{
    lemma_certify_setup(s, c, id);
    let s2 = apply(s, c, Action::Certify { id });
    let r = txn(s.txns, id);
    let r2 = txn(s2.txns, id);
    assert forall|o: int| #[trigger] s2.txns.dom().contains(o) implies o >= 0 && inv_txn(s2, c, o) by {
        assert(s.txns.dom().contains(o));
        assert(inv_txn(s, c, o));
        if o == id {
            assert(r2.body == r.body && r2.coord == r.coord && r2.thread == r.thread && r2.epoch == r.epoch);
            assert(r2.vc == r.vc && r2.installed == r.installed && r2.reads == r.reads && r2.pidx == r.pidx);
            assert(r2.invoked == r.invoked && r2.prepared_at == r.prepared_at);
            assert(is_prepared_or_later(r2));
            assert(!in_flight(r2));
            assert(!aborted_prepared(r2));
            assert forall|i: int| is_shard(c, i) && #[trigger] clock_shard(c, r2, i) implies r2.vc[group(c, i)] >= 1 by {
                assert(clock_shard(c, r, i));
            }
            assert forall|k: int| #[trigger] write_set(r2.body).contains(k) implies
                shard_epoch(s2.shards, owner(c, k)) >= r2.epoch by {
                assert(write_set(r.body).contains(k));
                assert(shard_epoch(s.shards, owner(c, k)) >= r.epoch);
            }
            assert forall|x: int| is_comp(c, x) implies #[trigger] r2.vc[x] >= 0 by {
                assert(r.vc[x] >= 0);
            }
            assert forall|i: int| #[trigger] r2.installed.contains(i) implies is_shard(c, i) && writes_at(c, r2.body, i) by {
                assert(r.installed.contains(i));
            }
            assert forall|k: int| #[trigger] r2.reads.dom().contains(k) implies read_set(r2.body).contains(k) by {
                assert(r.reads.dom().contains(k));
            }
            assert forall|k: int| #[trigger] read_set(r2.body).contains(k) implies r2.reads.dom().contains(k) by {
                assert(read_set(r.body).contains(k));
            }
            assert(all_installed(c, r2)) by {
                assert forall|i: int| is_shard(c, i) && writes_at(c, r2.body, i) implies #[trigger] r2.installed.contains(i) by {
                    assert(writes_at(c, r.body, i));
                }
            }
            assert(inv_txn(s2, c, id));
        } else {
            let ro = txn(s.txns, o);
            assert(txn(s2.txns, o) == ro);
            assert(shard_epoch(s2.shards, ro.coord) == shard_epoch(s.shards, ro.coord));
            assert forall|k: int| is_prepared_or_later(ro) && #[trigger] write_set(ro.body).contains(k) implies
                shard_epoch(s2.shards, owner(c, k)) >= ro.epoch by {
                assert(shard_epoch(s.shards, owner(c, k)) >= ro.epoch);
            }
            assert(inv_txn(s2, c, o));
        }
    }
}

pub proof fn lemma_certify_prepared(s: State, c: Constants, id: int)
    requires inv(s, c), can_certify(s, c, id)
    ensures inv_prepared(apply(s, c, Action::Certify { id }), c)
{
    lemma_certify_setup(s, c, id);
    let s2 = apply(s, c, Action::Certify { id });
    let r = txn(s.txns, id);
    let r2 = txn(s2.txns, id);
    assert(is_prepared_or_later(r2));
    assert(r2.pidx == r.pidx && r2.prepared_at == r.prepared_at);
    assert forall|j: int| 0 <= j < s2.prepared.len() implies
        has_txn(s2.txns, #[trigger] s2.prepared[j]) && txn(s2.txns, s2.prepared[j]).pidx == j
        && is_prepared_or_later(txn(s2.txns, s2.prepared[j])) by {
        let p = s.prepared[j];
        assert(has_txn(s.txns, p) && txn(s.txns, p).pidx == j && is_prepared_or_later(txn(s.txns, p)));
        if p != id { assert(txn(s2.txns, p) == txn(s.txns, p)); }
    }
    assert forall|j1: int, j2: int| 0 <= j1 < j2 < s2.prepared.len() implies
        txn(s2.txns, #[trigger] s2.prepared[j1]).prepared_at < txn(s2.txns, #[trigger] s2.prepared[j2]).prepared_at by {
        let p1 = s.prepared[j1];
        let p2 = s.prepared[j2];
        assert(txn(s.txns, p1).prepared_at < txn(s.txns, p2).prepared_at);
        assert(txn(s2.txns, p1).prepared_at == txn(s.txns, p1).prepared_at);
        assert(txn(s2.txns, p2).prepared_at == txn(s.txns, p2).prepared_at);
    }
}

pub proof fn lemma_certify_exclusive(s: State, c: Constants, id: int)
    requires inv(s, c), can_certify(s, c, id)
    ensures inv_exclusive(apply(s, c, Action::Certify { id }), c)
{
    lemma_certify_setup(s, c, id);
    let s2 = apply(s, c, Action::Certify { id });
    let r = txn(s.txns, id);
    let r2 = txn(s2.txns, id);
    assert(!in_flight(r2));
    assert forall|a: int, b: int| #[trigger] s2.txns.dom().contains(a) && #[trigger] s2.txns.dom().contains(b) && a != b
        && txn(s2.txns, a).coord == txn(s2.txns, b).coord && txn(s2.txns, a).thread == txn(s2.txns, b).thread
        implies !(in_flight(txn(s2.txns, a)) && in_flight(txn(s2.txns, b))) by {
        if a != id && b != id {
            assert(s.txns.dom().contains(a) && s.txns.dom().contains(b));
            assert(txn(s2.txns, a) == txn(s.txns, a));
            assert(txn(s2.txns, b) == txn(s.txns, b));
        }
    }
}

pub proof fn lemma_certify_locks(s: State, c: Constants, id: int)
    requires inv(s, c), can_certify(s, c, id)
    ensures inv_locks(apply(s, c, Action::Certify { id }), c)
{
    lemma_certify_setup(s, c, id);
    let s2 = apply(s, c, Action::Certify { id });
    let r = txn(s.txns, id);
    let r2 = txn(s2.txns, id);
    assert(r2.pidx == r.pidx);
    assert forall|x: int| #[trigger] pidx_of(s2.txns, x) == pidx_of(s.txns, x) by {
        if x != -1 && x != id { assert(txn(s2.txns, x) == txn(s.txns, x)); }
    }
    assert forall|k: int| #[trigger] s2.locks.dom().contains(k) implies inv_lock(s2, c, k) by {
        assert(inv_lock(s, c, k));
        let h = s.locks[k];
        if h == id {
            assert(write_set(r.body).contains(k));
            assert(r.body.ops.dom().contains(k));
            assert(valid_key(k));
            assert(is_shard(c, owner(c, k)));
            assert(writes_at(c, r.body, owner(c, k)));
            assert(r.installed.contains(owner(c, k)));
            assert(false);
        }
        assert(txn(s2.txns, h) == txn(s.txns, h));
        assert forall|j: int| 0 <= j < vers(s2.versions, k).len() implies
            pidx_of(s2.txns, #[trigger] vers(s2.versions, k)[j].txn) < txn(s2.txns, h).pidx by {
            assert(pidx_of(s.txns, vers(s.versions, k)[j].txn) < txn(s.txns, h).pidx);
            assert(pidx_of(s2.txns, vers(s.versions, k)[j].txn) == pidx_of(s.txns, vers(s.versions, k)[j].txn));
        }
    }
    assert forall|o: int, k: int| #[trigger] s2.txns.dom().contains(o) && txn(s2.txns, o).status is Prepared
        && #[trigger] write_set(txn(s2.txns, o).body).contains(k)
        && !txn(s2.txns, o).installed.contains(owner(c, k))
        && shard_epoch(s2.shards, owner(c, k)) == txn(s2.txns, o).epoch
        implies s2.locks.dom().contains(k) && s2.locks[k] == o by {
        assert(o != id);
        assert(txn(s2.txns, o) == txn(s.txns, o));
        assert(shard_epoch(s2.shards, owner(c, k)) == shard_epoch(s.shards, owner(c, k)));
    }
}

pub proof fn lemma_certify_versions(s: State, c: Constants, id: int)
    requires inv(s, c), can_certify(s, c, id)
    ensures inv_versions(apply(s, c, Action::Certify { id }), c)
{
    lemma_certify_setup(s, c, id);
    let s2 = apply(s, c, Action::Certify { id });
    let r = txn(s.txns, id);
    let r2 = txn(s2.txns, id);
    assert(is_prepared_or_later(r2));
    assert(r2.body == r.body && r2.epoch == r.epoch && r2.vc == r.vc && r2.pidx == r.pidx
        && r2.installed == r.installed && r2.reads == r.reads);
    assert forall|t: int| #[trigger] txn(s2.txns, t).pidx == txn(s.txns, t).pidx by {
        if t != id { assert(txn(s2.txns, t) == txn(s.txns, t)); }
    }
    assert forall|k: int| #[trigger] s2.versions.dom().contains(k) implies inv_versions_of(s2, c, k) by {
        assert(inv_versions_of(s, c, k));
        assert forall|j: int| 0 <= j < s2.versions[k].len() implies inv_version(s2, c, k, #[trigger] s2.versions[k][j]) by {
            let v = s.versions[k][j];
            assert(inv_version(s, c, k, v));
            if v.txn != id {
                assert(txn(s2.txns, v.txn) == txn(s.txns, v.txn));
            } else {
                assert(write_value(r2, k) == write_value(r, k));
            }
        }
        assert forall|a: int, b: int| 0 <= a < b < s2.versions[k].len() implies
            txn(s2.txns, #[trigger] s2.versions[k][a].txn).pidx < txn(s2.txns, #[trigger] s2.versions[k][b].txn).pidx by {
            assert(txn(s.txns, s.versions[k][a].txn).pidx < txn(s.txns, s.versions[k][b].txn).pidx);
            assert(txn(s2.txns, s.versions[k][a].txn).pidx == txn(s.txns, s.versions[k][a].txn).pidx);
            assert(txn(s2.txns, s.versions[k][b].txn).pidx == txn(s.txns, s.versions[k][b].txn).pidx);
        }
    }
}

pub proof fn lemma_certify_reads(s: State, c: Constants, id: int)
    requires inv(s, c), can_certify(s, c, id)
    ensures inv_reads(apply(s, c, Action::Certify { id }), c)
{
    lemma_certify_setup(s, c, id);
    let s2 = apply(s, c, Action::Certify { id });
    let r = txn(s.txns, id);
    let r2 = txn(s2.txns, id);
    assert(is_prepared_or_later(r2));
    assert(r2.body == r.body && r2.epoch == r.epoch && r2.vc == r.vc && r2.pidx == r.pidx
        && r2.installed == r.installed && r2.reads == r.reads);
    assert forall|t: int| #[trigger] is_prepared_or_later(txn(s2.txns, t)) == is_prepared_or_later(txn(s.txns, t)) by {
        if t != id { assert(txn(s2.txns, t) == txn(s.txns, t)); }
    }
    assert forall|t: int| #[trigger] pidx_of(s2.txns, t) == pidx_of(s.txns, t) by {
        if t != -1 && t != id { assert(txn(s2.txns, t) == txn(s.txns, t)); }
    }
    assert forall|o: int, k: int| #[trigger] s2.txns.dom().contains(o) && #[trigger] txn(s2.txns, o).reads.dom().contains(k)
        implies inv_read(s2, c, o, k) by {
        let ro = txn(s.txns, o);
        let ro2 = txn(s2.txns, o);
        assert(ro2.reads == ro.reads && ro2.body == ro.body && ro2.epoch == ro.epoch && ro2.vc == ro.vc
            && ro2.pidx == ro.pidx);
        assert(s.txns.dom().contains(o));
        assert(ro.reads.dom().contains(k));
        assert(inv_read(s, c, o, k));
        let rd = ro.reads[k];
        if rd.writer != -1 {
            assert(inv_read_writer(s, c, o, k));
            let w = txn(s.txns, rd.writer);
            let w2 = txn(s2.txns, rd.writer);
            assert(w2.body == w.body && w2.epoch == w.epoch && w2.vc == w.vc && w2.pidx == w.pidx
                && w2.installed == w.installed && w2.reads == w.reads);
            assert(write_value(w2, k) == write_value(w, k));
            assert(inv_read_writer(s2, c, o, k));
        }
        if is_prepared_or_later(ro2) {
            assert(is_prepared_or_later(ro));
            assert forall|p: int| #[trigger] s2.txns.dom().contains(p) && p != o
                && is_prepared_or_later(txn(s2.txns, p)) && txn(s2.txns, p).pidx < ro2.pidx
                && write_set(txn(s2.txns, p).body).contains(k) implies inv_read_order(s2, c, o, k, p) by {
                let rp = txn(s.txns, p);
                let rp2 = txn(s2.txns, p);
                assert(rp2.body == rp.body && rp2.epoch == rp.epoch && rp2.pidx == rp.pidx
                    && rp2.installed == rp.installed);
                assert(s.txns.dom().contains(p));
                assert(is_prepared_or_later(rp));
                assert(inv_read_order(s, c, o, k, p));
                if p == id {
                    // installed everywhere it writes
                    assert(r.body.ops.dom().contains(k));
                    assert(valid_key(k));
                    assert(is_shard(c, owner(c, k)));
                    assert(writes_at(c, r.body, owner(c, k)));
                    assert(r.installed.contains(owner(c, k)));
                }
                assert(shard_epoch(s2.shards, owner(c, k)) == shard_epoch(s.shards, owner(c, k)));
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Streams
// ---------------------------------------------------------------------------

/// Every entry that satisfied `inv_entry` before still does.
pub proof fn lemma_certify_entry(s: State, c: Constants, id: int, sid2: Sid, e: Entry)
    requires inv(s, c), can_certify(s, c, id), valid_sid(c, sid2), inv_entry(s, c, sid2, e)
    ensures inv_entry(apply(s, c, Action::Certify { id }), c, sid2, e)
{
    lemma_certify_setup(s, c, id);
    let s2 = apply(s, c, Action::Certify { id });
    let r = txn(s.txns, id);
    let r2 = txn(s2.txns, id);
    assert(shard_epoch(s2.shards, sid2.shard) == shard_epoch(s.shards, sid2.shard));
    assert(counter(s2.shards, sid2.shard) >= counter(s.shards, sid2.shard));
    if e is Log {
        let t = e->Log_txn;
        if t == id {
            assert(logged_at(c, r, sid2.shard));
            assert(sid2.shard != r.coord);
            assert(clock_shard(c, r2, sid2.shard) == clock_shard(c, r, sid2.shard));
            assert(logged_at(c, r2, sid2.shard));
        } else {
            assert(txn(s2.txns, t) == txn(s.txns, t));
        }
    }
}

pub proof fn lemma_certify_stream_other(s: State, c: Constants, id: int, sid2: Sid)
    requires inv(s, c), can_certify(s, c, id), valid_sid(c, sid2), sid2 != cert_sid(s, id)
    ensures inv_stream(apply(s, c, Action::Certify { id }), c, sid2)
{
    lemma_certify_setup(s, c, id);
    let s2 = apply(s, c, Action::Certify { id });
    assert(inv_stream(s, c, sid2));
    let es = all_entries(s.streams[sid2]);
    assert(s2.streams[sid2] == s.streams[sid2]);
    assert(all_entries(s2.streams[sid2]) == es);
    assert forall|j: int| 0 <= j < es.len() implies inv_entry(s2, c, sid2, #[trigger] es[j]) by {
        assert(inv_entry(s, c, sid2, es[j]));
        lemma_certify_entry(s, c, id, sid2, es[j]);
    }
}

pub proof fn lemma_certify_stream_same(s: State, c: Constants, id: int)
    requires inv(s, c), can_certify(s, c, id)
    ensures inv_stream(apply(s, c, Action::Certify { id }), c, cert_sid(s, id))
{
    lemma_certify_setup(s, c, id);
    let s2 = apply(s, c, Action::Certify { id });
    let r = txn(s.txns, id);
    let r2 = txn(s2.txns, id);
    let sid = cert_sid(s, id);
    let x = cert_entry(s, c, id);
    let es = all_entries(s.streams[sid]);
    let es2 = all_entries(s2.streams[sid]);
    let n = es.len() as int;
    assert(inv_stream(s, c, sid));
    assert(es2 == es.push(x));
    assert(es2[n] == x);
    // ordering
    assert forall|a: int| 0 <= a < es.len() implies entry_epoch(#[trigger] es[a]) <= entry_epoch(x) by {
        assert(inv_entry(s, c, sid, es[a]));
    }
    assert(entry_epoch(x) == r.epoch);
    lemma_order_push(es, x);
    // entries
    assert(inv_entry(s2, c, sid, x)) by {
        assert(r2.epoch == r.epoch && r2.coord == r.coord && r2.thread == r.thread && r2.vc == r.vc);
        assert(shard_epoch(s2.shards, r.coord) == shard_epoch(s.shards, r.coord));
        assert(clock_shard(c, r2, r.coord));
        assert(certified_or_committed(r2));
        assert(logged_at(c, r2, sid.shard));
    }
    assert forall|j: int| 0 <= j < es2.len() implies inv_entry(s2, c, sid, #[trigger] es2[j]) by {
        if j < n {
            assert(es2[j] == es[j]);
            assert(inv_entry(s, c, sid, es[j]));
            lemma_certify_entry(s, c, id, sid, es[j]);
        }
    }
}

pub proof fn lemma_certify_streams(s: State, c: Constants, id: int)
    requires inv(s, c), can_certify(s, c, id)
    ensures inv_streams(apply(s, c, Action::Certify { id }), c)
{
    let s2 = apply(s, c, Action::Certify { id });
    assert forall|sid2: Sid| valid_sid(c, sid2) implies #[trigger] inv_stream(s2, c, sid2) by {
        if sid2 == cert_sid(s, id) {
            lemma_certify_stream_same(s, c, id);
        } else {
            lemma_certify_stream_other(s, c, id, sid2);
        }
    }
}

// ---------------------------------------------------------------------------
// Where the logs are
// ---------------------------------------------------------------------------

pub proof fn lemma_certify_coord_below(s: State, c: Constants, id: int, o: int)
    requires inv(s, c), can_certify(s, c, id), s.txns.dom().contains(o)
    ensures ({
        let s2 = apply(s, c, Action::Certify { id });
        inv_coord_below(s2, c, txn(s2.txns, o))
    })
{
    lemma_certify_setup(s, c, id);
    let s2 = apply(s, c, Action::Certify { id });
    let r = txn(s.txns, id);
    let sid = cert_sid(s, id);
    let x = cert_entry(s, c, id);
    if o != id {
        let ro = txn(s.txns, o);
        assert(txn(s2.txns, o) == ro);
        if ro.status is Prepared || aborted_prepared(ro) {
            assert(inv_logs(s, c, o));
            assert(inv_coord_below(s, c, ro));
            let osid = coord_sid(ro, ro.coord);
            if osid == sid {
                assert(ro.coord == r.coord && ro.thread == r.thread);
                if ro.status is Prepared {
                    assert(in_flight(ro) && in_flight(r));
                    assert(s.txns.dom().contains(id));
                    assert(!(in_flight(txn(s.txns, id)) && in_flight(txn(s.txns, o))));
                    assert(false);
                }
                assert(inv_txn(s, c, o));
                assert(ro.epoch < shard_epoch(s.shards, ro.coord));
                assert(entry_epoch(x) != ro.epoch);
                lemma_below_push_other(stream_at(s.streams, ro, ro.coord), x, ro.epoch, ro.vc[group(c, ro.coord)]);
                assert(stream_at(s2.streams, ro, ro.coord) == stream_at(s.streams, ro, ro.coord).push(x));
            } else {
                assert(s2.streams[osid] == s.streams[osid]);
            }
        }
    }
}

pub proof fn lemma_certify_uninstalled_below(s: State, c: Constants, id: int, o: int)
    requires inv(s, c), can_certify(s, c, id), s.txns.dom().contains(o)
    ensures ({
        let s2 = apply(s, c, Action::Certify { id });
        inv_uninstalled_below(s2, c, txn(s2.txns, o))
    })
{
    lemma_certify_setup(s, c, id);
    let s2 = apply(s, c, Action::Certify { id });
    let r = txn(s.txns, id);
    let sid = cert_sid(s, id);
    if o != id {
        let ro = txn(s.txns, o);
        assert(txn(s2.txns, o) == ro);
        if ro.status is Prepared {
            assert(inv_logs(s, c, o));
            assert(inv_uninstalled_below(s, c, ro));
            assert forall|i: int| is_shard(c, i) && #[trigger] clock_shard(c, ro, i) && !ro.installed.contains(i)
                implies logs_below(stream_at(s2.streams, ro, i), ro.epoch, ro.vc[group(c, i)]) by {
                let osid = coord_sid(ro, i);
                if osid == sid {
                    assert(ro.coord == r.coord && ro.thread == r.thread);
                    assert(in_flight(ro) && in_flight(r));
                    assert(s.txns.dom().contains(id));
                    assert(!(in_flight(txn(s.txns, id)) && in_flight(txn(s.txns, o))));
                    assert(false);
                }
                assert(s2.streams[osid] == s.streams[osid]);
            }
        }
    }
}

pub proof fn lemma_certify_logged(s: State, c: Constants, id: int, o: int)
    requires inv(s, c), can_certify(s, c, id), s.txns.dom().contains(o)
    ensures ({
        let s2 = apply(s, c, Action::Certify { id });
        inv_logged(s2, c, txn(s2.txns, o), o)
    })
{
    lemma_certify_setup(s, c, id);
    let s2 = apply(s, c, Action::Certify { id });
    let r = txn(s.txns, id);
    let r2 = txn(s2.txns, id);
    let sid = cert_sid(s, id);
    let x = cert_entry(s, c, id);
    let es = all_entries(s.streams[sid]);
    let ro = txn(s.txns, o);
    let ro2 = txn(s2.txns, o);
    assert(inv_logs(s, c, o));
    assert(inv_logged(s, c, ro, o));
    assert(ro2.body == ro.body && ro2.coord == ro.coord && ro2.thread == ro.thread && ro2.epoch == ro.epoch
        && ro2.vc == ro.vc && ro2.installed == ro.installed);
    assert forall|i: int| is_shard(c, i) && #[trigger] logged_at(c, ro2, i) implies
        has_log(stream_at(s2.streams, ro2, i), o)
        || (shard_epoch(s2.shards, i) > ro2.epoch && stream_below(stream_at(s2.streams, ro2, i), ro2.epoch, ro2.vc[group(c, i)])) by {
        let osid = coord_sid(ro, i);
        assert(shard_epoch(s2.shards, i) == shard_epoch(s.shards, i));
        assert(stream_at(s2.streams, ro2, i) == all_entries(s2.streams[osid]));
        assert(stream_at(s.streams, ro, i) == all_entries(s.streams[osid]));
        if o == id && i == r.coord {
            assert(osid == sid);
            assert(all_entries(s2.streams[sid]) == es.push(x));
            assert(es.push(x)[es.len() as int] == x);
            assert(is_log_of(es.push(x)[es.len() as int], id));
            assert(has_log(es.push(x), id));
        } else {
            if o == id {
                assert(clock_shard(c, r2, i) == clock_shard(c, r, i));
                assert(logged_at(c, r, i));
                assert(osid != sid);
            } else {
                assert(ro2 == ro);
                assert(logged_at(c, ro, i));
            }
            if osid != sid {
                assert(s2.streams[osid] == s.streams[osid]);
            } else {
                // another transaction of the same worker, on the coordinator stream
                assert(all_entries(s2.streams[sid]) == es.push(x));
                if has_log(es, o) {
                    lemma_has_log_push(es, x, o);
                } else {
                    assert(shard_epoch(s.shards, i) > ro.epoch);
                    assert(i == r.coord);
                    assert(entry_epoch(x) == shard_epoch(s.shards, r.coord));
                    lemma_below_push_other(es, x, ro.epoch, ro.vc[group(c, i)]);
                }
            }
        }
    }
}

pub proof fn lemma_certify_present(s: State, c: Constants, id: int, o: int)
    requires inv(s, c), can_certify(s, c, id), s.txns.dom().contains(o)
    ensures ({
        let s2 = apply(s, c, Action::Certify { id });
        inv_present(s2, c, txn(s2.txns, o), o)
    })
{
    lemma_certify_setup(s, c, id);
    let s2 = apply(s, c, Action::Certify { id });
    let r = txn(s.txns, id);
    let r2 = txn(s2.txns, id);
    let sid = cert_sid(s, id);
    let x = cert_entry(s, c, id);
    let es = all_entries(s.streams[sid]);
    let ro = txn(s.txns, o);
    let ro2 = txn(s2.txns, o);
    assert(inv_logs(s, c, o));
    assert(inv_present(s, c, ro, o));
    assert(ro2.body == ro.body && ro2.coord == ro.coord && ro2.thread == ro.thread && ro2.epoch == ro.epoch
        && ro2.vc == ro.vc && ro2.installed == ro.installed);
    assert((ro2.status is Prepared || certified_or_committed(ro2)) ==> (ro.status is Prepared || certified_or_committed(ro)));
    assert forall|k: int| (ro2.status is Prepared || certified_or_committed(ro2))
        && #[trigger] write_set(ro2.body).contains(k) && ro2.installed.contains(owner(c, k))
        && !has_version(s2.versions, k, o) implies
        (doomed(s2.final_wm, c, ro2) && s2.rolled_back.contains((owner(c, k), ro2.epoch))) || lost(s2.streams, c, ro2, o) by {
        assert(write_set(ro.body).contains(k));
        assert(!has_version(s.versions, k, o));
        if !(doomed(s.final_wm, c, ro) && s.rolled_back.contains((owner(c, k), ro.epoch))) {
            assert(lost(s.streams, c, ro, o));
            let i = choose|i: int| is_shard(c, i) && #[trigger] logged_at(c, ro, i) && !has_log(stream_at(s.streams, ro, i), o);
            let osid = coord_sid(ro, i);
            assert(stream_at(s2.streams, ro2, i) == all_entries(s2.streams[osid]));
            assert(stream_at(s.streams, ro, i) == all_entries(s.streams[osid]));
            if o == id {
                // not yet certified: nothing is logged at the coordinator
                assert(i != r.coord);
                assert(clock_shard(c, r2, i) == clock_shard(c, r, i));
                assert(logged_at(c, r2, i));
                assert(osid != sid);
                assert(s2.streams[osid] == s.streams[osid]);
            } else {
                assert(ro2 == ro);
                if osid == sid {
                    assert(all_entries(s2.streams[sid]) == es.push(x));
                    assert(!is_log_of(x, o));
                    lemma_no_log_push(es, x, o);
                } else {
                    assert(s2.streams[osid] == s.streams[osid]);
                }
            }
            assert(logged_at(c, ro2, i) && !has_log(stream_at(s2.streams, ro2, i), o));
            assert(lost(s2.streams, c, ro2, o));
        }
    }
}

pub proof fn lemma_certify_all_logs(s: State, c: Constants, id: int)
    requires inv(s, c), can_certify(s, c, id)
    ensures inv_all_logs(apply(s, c, Action::Certify { id }), c)
{
    lemma_certify_setup(s, c, id);
    let s2 = apply(s, c, Action::Certify { id });
    assert forall|o: int| #[trigger] s2.txns.dom().contains(o) implies inv_logs(s2, c, o) by {
        assert(s.txns.dom().contains(o));
        lemma_certify_coord_below(s, c, id, o);
        lemma_certify_uninstalled_below(s, c, id, o);
        lemma_certify_logged(s, c, id, o);
        lemma_certify_present(s, c, id, o);
    }
}

// ---------------------------------------------------------------------------
// Watermarks: only durable prefixes matter, and they are unchanged
// ---------------------------------------------------------------------------

pub proof fn lemma_certify_committed(s: State, c: Constants, id: int)
    requires inv(s, c), can_certify(s, c, id)
    ensures inv_committed(apply(s, c, Action::Certify { id }), c)
{
    lemma_certify_setup(s, c, id);
    let s2 = apply(s, c, Action::Certify { id });
    assert forall|o: int| #[trigger] s2.txns.dom().contains(o) && committed(txn(s2.txns, o))
        implies below_wm(s2.streams, c, txn(s2.txns, o).vc, txn(s2.txns, o).epoch) by {
        assert(o != id);
        let ro = txn(s.txns, o);
        assert(txn(s2.txns, o) == ro);
        assert(s.txns.dom().contains(o));
        assert(below_wm(s.streams, c, ro.vc, ro.epoch));
        assert forall|sid2: Sid| valid_sid(c, sid2) implies
            wm_le(ro.vc[group(c, sid2.shard)], stream_wm(#[trigger] s2.streams[sid2].durable, ro.epoch)) by {
            assert(s2.streams[sid2].durable == s.streams[sid2].durable);
            assert(wm_le(ro.vc[group(c, sid2.shard)], stream_wm(s.streams[sid2].durable, ro.epoch)));
        }
    }
}

pub proof fn lemma_certify_final(s: State, c: Constants, id: int)
    requires inv(s, c), can_certify(s, c, id)
    ensures inv_final(apply(s, c, Action::Certify { id }), c)
{
    lemma_certify_setup(s, c, id);
    let s2 = apply(s, c, Action::Certify { id });
    let r = txn(s.txns, id);
    let sid = cert_sid(s, id);
    let x = cert_entry(s, c, id);
    assert forall|i: int, e: nat| #[trigger] s2.final_wm.dom().contains((i, e)) implies inv_final_of(s2, c, i, e) by {
        assert(inv_final_of(s, c, i, e));
        assert(shard_epoch(s2.shards, i) == shard_epoch(s.shards, i));
        assert forall|sid2: Sid| valid_sid(c, sid2) && sid2.shard == i implies
            no_pending_epoch(#[trigger] s2.streams[sid2], e)
            && wm_le_wm(s2.final_wm[(i, e)], stream_wm(s2.streams[sid2].durable, e)) by {
            assert(no_pending_epoch(s.streams[sid2], e));
            assert(wm_le_wm(s.final_wm[(i, e)], stream_wm(s.streams[sid2].durable, e)));
            assert(s2.streams[sid2].durable == s.streams[sid2].durable);
            if sid2 == sid {
                // the new entry is of the coordinator's current epoch, which is past e
                assert(entry_epoch(x) == shard_epoch(s.shards, i));
                assert(entry_epoch(x) != e);
                let p = s.streams[sid].pending;
                let p2 = s2.streams[sid].pending;
                assert(p2 == p.push(x));
                assert forall|j: int| 0 <= j < p2.len() implies entry_epoch(#[trigger] p2[j]) != e by {
                    if j < p.len() { assert(p2[j] == p[j]); } else { assert(p2[j] == x); }
                }
            } else {
                assert(s2.streams[sid2] == s.streams[sid2]);
            }
        }
        let w = choose|w: Sid| valid_sid(c, w) && w.shard == i && s.final_wm[(i, e)] == stream_wm(#[trigger] s.streams[w].durable, e);
        assert(s2.streams[w].durable == s.streams[w].durable);
        assert(s2.final_wm[(i, e)] == stream_wm(s2.streams[w].durable, e));
    }
}

pub proof fn lemma_certify_rolled_back(s: State, c: Constants, id: int)
    requires inv(s, c), can_certify(s, c, id)
    ensures inv_rolled_back(apply(s, c, Action::Certify { id }), c)
{
    lemma_certify_setup(s, c, id);
    let s2 = apply(s, c, Action::Certify { id });
    assert forall|i: int, e: nat| #[trigger] s2.rolled_back.contains((i, e)) implies inv_rolled_back_of(s2, c, i, e) by {
        assert(inv_rolled_back_of(s, c, i, e));
    }
}

// ---------------------------------------------------------------------------
// The action
// ---------------------------------------------------------------------------

pub proof fn lemma_certify_inv(s: State, c: Constants, id: int)
    requires inv(s, c), can_certify(s, c, id)
    ensures inv(apply(s, c, Action::Certify { id }), c)
{
    lemma_certify_shapes(s, c, id);
    lemma_certify_txns(s, c, id);
    lemma_certify_prepared(s, c, id);
    lemma_certify_exclusive(s, c, id);
    lemma_certify_locks(s, c, id);
    lemma_certify_versions(s, c, id);
    lemma_certify_reads(s, c, id);
    lemma_certify_streams(s, c, id);
    lemma_certify_all_logs(s, c, id);
    lemma_certify_committed(s, c, id);
    lemma_certify_final(s, c, id);
    lemma_certify_rolled_back(s, c, id);
}

} // verus!
