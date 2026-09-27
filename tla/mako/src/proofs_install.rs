//! Preservation proof for Install (speculative install at one shard leader),
//! split into one lemma per invariant conjunct.
use super::types::*;
use super::normal::*;
use super::recovery::*;
use super::behavior::*;
use super::invariants::*;
use super::stream_lemmas::*;
use super::proofs_basic::*;
use super::proofs_prepare::*;
use vstd::prelude::*;

verus! {

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

pub proof fn lemma_install_has_log_push(es: Seq<Entry>, x: Entry, id: int)
    ensures has_log(es.push(x), id) <==> (has_log(es, id) || is_log_of(x, id))
{
    let n = es.len() as int;
    if has_log(es, id) {
        let j = choose|j: int| 0 <= j < es.len() && is_log_of(#[trigger] es[j], id);
        assert(es.push(x)[j] == es[j]);
    }
    if is_log_of(x, id) {
        assert(es.push(x)[n] == x);
    }
    if has_log(es.push(x), id) {
        let j = choose|j: int| 0 <= j < es.push(x).len() && is_log_of(#[trigger] es.push(x)[j], id);
        if j < n { assert(es[j] == es.push(x)[j]); }
    }
}

pub proof fn lemma_install_owner_is_shard(c: Constants, k: int)
    requires valid_constants(c), valid_key(k)
    ensures is_shard(c, owner(c, k))
{
}

// ---------------------------------------------------------------------------
// Facts about the post-state
// ---------------------------------------------------------------------------

pub proof fn lemma_install_shard_facts(s: State, c: Constants, id: int, i: int)
    requires inv(s, c), can_install(s, c, id, i)
    ensures ({
        let s2 = apply(s, c, Action::Install { id, shard: i });
        let r = txn(s.txns, id);
        &&& s2.shards.len() == c.shards
        &&& s2.epoch == s.epoch
        &&& forall|j: int| 0 <= j < c.shards ==> #[trigger] shard_epoch(s2.shards, j) == shard_epoch(s.shards, j)
        &&& forall|j: int| 0 <= j < c.shards ==> #[trigger] counter(s2.shards, j) >= counter(s.shards, j)
        &&& counter(s2.shards, i) >= r.vc[group(c, i)]
        &&& shard_epoch(s.shards, i) == r.epoch
    })
{
    let s2 = apply(s, c, Action::Install { id, shard: i });
    let r = txn(s.txns, id);
    assert forall|j: int| 0 <= j < c.shards implies #[trigger] shard_epoch(s2.shards, j) == shard_epoch(s.shards, j) by {
        if j != i { assert(s2.shards[j] == s.shards[j]); }
    }
    assert forall|j: int| 0 <= j < c.shards implies #[trigger] counter(s2.shards, j) >= counter(s.shards, j) by {
        if j != i { assert(s2.shards[j] == s.shards[j]); }
    }
}

pub proof fn lemma_install_txn_facts(s: State, c: Constants, id: int, i: int)
    requires inv(s, c), can_install(s, c, id, i)
    ensures ({
        let s2 = apply(s, c, Action::Install { id, shard: i });
        let r = txn(s.txns, id);
        let r2 = txn(s2.txns, id);
        &&& r2 == TxnRec { installed: r.installed.insert(i), ..r }
        &&& s2.txns.dom() == s.txns.dom()
        &&& forall|o: int| o != id ==> #[trigger] txn(s2.txns, o) == txn(s.txns, o)
        &&& forall|o: int| #[trigger] pidx_of(s2.txns, o) == pidx_of(s.txns, o)
        &&& s2.prepared == s.prepared
        &&& s2.final_wm == s.final_wm
        &&& s2.rolled_back == s.rolled_back
        &&& s2.tick == s.tick + 1
        &&& r.status is Prepared
        &&& !read_only(r)
        &&& clock_shard(c, r, i)
        &&& r.vc[group(c, i)] >= 1
        &&& forall|j: int| #[trigger] clock_shard(c, r2, j) == clock_shard(c, r, j)
        &&& forall|k: int| #[trigger] write_value(r2, k) == write_value(r, k)
        &&& is_shard(c, r.coord) && is_thread(c, r.thread)
    })
{
    let s2 = apply(s, c, Action::Install { id, shard: i });
    let r = txn(s.txns, id);
    assert(inv_txn(s, c, id));
    assert(s2.txns.dom() =~= s.txns.dom());
    assert(clock_shard(c, r, i));
}

pub proof fn lemma_install_version_facts(s: State, c: Constants, id: int, i: int)
    requires inv(s, c), can_install(s, c, id, i)
    ensures ({
        let s2 = apply(s, c, Action::Install { id, shard: i });
        let r = txn(s.txns, id);
        let keys = keys_at(c, r.body, i);
        &&& forall|k: int| #[trigger] keys.contains(k) <==> (write_set(r.body).contains(k) && owner(c, k) == i)
        &&& forall|k: int| #[trigger] keys.contains(k) ==> s2.versions.dom().contains(k)
            && s2.versions[k] == vers(s.versions, k).push(new_version(r, id, k))
            && vers(s2.versions, k) == vers(s.versions, k).push(new_version(r, id, k))
        &&& forall|k: int| !keys.contains(k) ==> (#[trigger] s2.versions.dom().contains(k) <==> s.versions.dom().contains(k))
        &&& forall|k: int| !keys.contains(k) ==> #[trigger] vers(s2.versions, k) == vers(s.versions, k)
        &&& forall|k: int| !keys.contains(k) && s.versions.dom().contains(k) ==> #[trigger] s2.versions[k] == s.versions[k]
        &&& forall|k: int, o: int| o != id ==> (#[trigger] has_version(s2.versions, k, o) <==> has_version(s.versions, k, o))
        &&& forall|k: int| #[trigger] keys.contains(k) ==> has_version(s2.versions, k, id)
        &&& forall|k: int| !keys.contains(k) ==> (#[trigger] has_version(s2.versions, k, id) <==> has_version(s.versions, k, id))
    })
{
    let s2 = apply(s, c, Action::Install { id, shard: i });
    let r = txn(s.txns, id);
    let keys = keys_at(c, r.body, i);
    assert forall|k: int| #[trigger] keys.contains(k) <==> (write_set(r.body).contains(k) && owner(c, k) == i) by {}
    assert forall|k: int| #[trigger] keys.contains(k) implies s2.versions.dom().contains(k)
        && s2.versions[k] == vers(s.versions, k).push(new_version(r, id, k))
        && vers(s2.versions, k) == vers(s.versions, k).push(new_version(r, id, k)) by {}
    assert forall|k: int| !keys.contains(k) implies (#[trigger] s2.versions.dom().contains(k) <==> s.versions.dom().contains(k)) by {}
    assert forall|k: int| !keys.contains(k) implies #[trigger] vers(s2.versions, k) == vers(s.versions, k) by {}
    assert forall|k: int| !keys.contains(k) && s.versions.dom().contains(k) implies #[trigger] s2.versions[k] == s.versions[k] by {}
    assert forall|k: int, o: int| o != id implies (#[trigger] has_version(s2.versions, k, o) <==> has_version(s.versions, k, o)) by {
        if keys.contains(k) {
            let old = vers(s.versions, k);
            let m = old.len() as int;
            assert(vers(s2.versions, k) == old.push(new_version(r, id, k)));
            if has_version(s.versions, k, o) {
                let j = choose|j: int| 0 <= j < old.len() && #[trigger] old[j].txn == o;
                assert(vers(s2.versions, k)[j].txn == o);
            }
            if has_version(s2.versions, k, o) {
                let j = choose|j: int| 0 <= j < vers(s2.versions, k).len() && #[trigger] vers(s2.versions, k)[j].txn == o;
                if j < m { assert(old[j].txn == o); }
            }
        } else {
            assert(vers(s2.versions, k) == vers(s.versions, k));
        }
    }
    assert forall|k: int| #[trigger] keys.contains(k) implies has_version(s2.versions, k, id) by {
        let old = vers(s.versions, k);
        let m = old.len() as int;
        assert(vers(s2.versions, k) == old.push(new_version(r, id, k)));
        assert(vers(s2.versions, k)[m].txn == id);
    }
    assert forall|k: int| !keys.contains(k) implies (#[trigger] has_version(s2.versions, k, id) <==> has_version(s.versions, k, id)) by {
        assert(vers(s2.versions, k) == vers(s.versions, k));
    }
}

/// The installing transaction holds the lock on every key it installs, and
/// has no version of those keys yet.
pub proof fn lemma_install_lock_facts(s: State, c: Constants, id: int, i: int)
    requires inv(s, c), can_install(s, c, id, i)
    ensures ({
        let s2 = apply(s, c, Action::Install { id, shard: i });
        let r = txn(s.txns, id);
        let keys = keys_at(c, r.body, i);
        &&& forall|k: int| #[trigger] keys.contains(k) ==> s.locks.dom().contains(k) && s.locks[k] == id
        &&& forall|k: int| #[trigger] keys.contains(k) ==> !s2.locks.dom().contains(k)
        &&& forall|k: int| !keys.contains(k) ==> (#[trigger] s2.locks.dom().contains(k) <==> s.locks.dom().contains(k))
        &&& forall|k: int| !keys.contains(k) && s.locks.dom().contains(k) ==> #[trigger] s2.locks[k] == s.locks[k]
        &&& forall|k: int| #[trigger] keys.contains(k) ==> !has_version(s.versions, k, id)
    })
{
    let s2 = apply(s, c, Action::Install { id, shard: i });
    let r = txn(s.txns, id);
    let keys = keys_at(c, r.body, i);
    assert forall|k: int| #[trigger] keys.contains(k) implies s.locks.dom().contains(k) && s.locks[k] == id by {
        assert(write_set(txn(s.txns, id).body).contains(k));
        assert(owner(c, k) == i);
    }
    assert forall|k: int| #[trigger] keys.contains(k) implies !has_version(s.versions, k, id) by {
        if has_version(s.versions, k, id) {
            let j = choose|j: int| 0 <= j < vers(s.versions, k).len() && #[trigger] vers(s.versions, k)[j].txn == id;
            assert(s.versions.dom().contains(k));
            assert(inv_versions_of(s, c, k));
            assert(inv_version(s, c, k, s.versions[k][j]));
            assert(owner(c, k) == i);
        }
    }
}

pub proof fn lemma_install_stream_facts(s: State, c: Constants, id: int, i: int)
    requires inv(s, c), can_install(s, c, id, i)
    ensures ({
        let s2 = apply(s, c, Action::Install { id, shard: i });
        let r = txn(s.txns, id);
        let sid = coord_sid(r, i);
        let ent = log_entry(c, r, id, i);
        let es = all_entries(s.streams[sid]);
        &&& valid_sid(c, sid)
        &&& forall|sid2: Sid| #[trigger] s2.streams[sid2].durable == s.streams[sid2].durable
        &&& forall|sid2: Sid| sid2 != sid || i == r.coord ==> #[trigger] s2.streams[sid2] == s.streams[sid2]
        &&& i != r.coord ==> s2.streams[sid].pending == s.streams[sid].pending.push(ent)
        &&& i != r.coord ==> all_entries(s2.streams[sid]) == es.push(ent)
        &&& s2.streams.dom() == s.streams.dom()
        &&& !has_log(es, id)
        &&& logs_below(es, r.epoch, r.vc[group(c, i)])
    })
{
    let s2 = apply(s, c, Action::Install { id, shard: i });
    let r = txn(s.txns, id);
    let sid = coord_sid(r, i);
    let ent = log_entry(c, r, id, i);
    let es = all_entries(s.streams[sid]);
    assert(inv_txn(s, c, id));
    assert(inv_logs(s, c, id));
    assert(valid_sid(c, sid));
    assert(s.streams.dom().contains(sid));
    assert forall|sid2: Sid| #[trigger] s2.streams[sid2].durable == s.streams[sid2].durable by {
        if i != r.coord && sid2 == sid {
            assert(s2.streams[sid] == Stream { pending: s.streams[sid].pending.push(ent), ..s.streams[sid] });
        }
    }
    assert forall|sid2: Sid| sid2 != sid || i == r.coord implies #[trigger] s2.streams[sid2] == s.streams[sid2] by {}
    if i != r.coord {
        assert(all_entries(s2.streams[sid]) =~= es.push(ent));
        assert(s2.streams.dom() =~= s.streams.dom());
    }
    assert(inv_stream(s, c, sid));
    assert(!has_log(es, id)) by {
        if has_log(es, id) {
            let j = choose|j: int| 0 <= j < es.len() && is_log_of(#[trigger] es[j], id);
            assert(inv_entry(s, c, sid, es[j]));
            assert(logged_at(c, r, i));
        }
    }
    assert(clock_shard(c, r, i));
    assert(inv_uninstalled_below(s, c, r));
}

// ---------------------------------------------------------------------------
// One lemma per conjunct
// ---------------------------------------------------------------------------

pub proof fn lemma_install_shapes(s: State, c: Constants, id: int, i: int)
    requires inv(s, c), can_install(s, c, id, i)
    ensures inv_shapes(apply(s, c, Action::Install { id, shard: i }), c)
{
    let s2 = apply(s, c, Action::Install { id, shard: i });
    let r = txn(s.txns, id);
    let keys = keys_at(c, r.body, i);
    lemma_install_shard_facts(s, c, id, i);
    lemma_install_version_facts(s, c, id, i);
    lemma_install_stream_facts(s, c, id, i);
    assert(inv_txn(s, c, id));
    assert forall|k: int| #[trigger] s2.versions.dom().contains(k) implies valid_key(k) by {
        if keys.contains(k) {
            assert(write_set(r.body).contains(k));
            assert(r.body.ops.dom().contains(k));
        }
    }
    assert forall|j: int| #![trigger shard_epoch(s2.shards, j)] #![trigger counter(s2.shards, j)]
        is_shard(c, j) implies shard_epoch(s2.shards, j) <= s2.epoch && counter(s2.shards, j) >= 0 by {
        assert(shard_epoch(s.shards, j) <= s.epoch && counter(s.shards, j) >= 0);
        assert(shard_epoch(s2.shards, j) == shard_epoch(s.shards, j));
        assert(counter(s2.shards, j) >= counter(s.shards, j));
    }
}

pub proof fn lemma_install_txns(s: State, c: Constants, id: int, i: int)
    requires inv(s, c), can_install(s, c, id, i)
    ensures inv_txns(apply(s, c, Action::Install { id, shard: i }), c)
{
    let s2 = apply(s, c, Action::Install { id, shard: i });
    let r = txn(s.txns, id);
    let r2 = txn(s2.txns, id);
    lemma_install_shard_facts(s, c, id, i);
    lemma_install_txn_facts(s, c, id, i);
    assert forall|o: int| #[trigger] s2.txns.dom().contains(o) implies o >= 0 && inv_txn(s2, c, o) by {
        assert(inv_txn(s, c, o));
        let ro = txn(s.txns, o);
        let ro2 = txn(s2.txns, o);
        assert(shard_epoch(s2.shards, ro.coord) == shard_epoch(s.shards, ro.coord));
        if is_prepared_or_later(ro) {
            assert forall|k: int| #[trigger] write_set(ro2.body).contains(k) implies
                shard_epoch(s2.shards, owner(c, k)) >= ro2.epoch by {
                assert(valid_txn(ro.body));
                assert(ro.body.ops.dom().contains(k));
                lemma_install_owner_is_shard(c, k);
                assert(shard_epoch(s2.shards, owner(c, k)) == shard_epoch(s.shards, owner(c, k)));
            }
        }
        if o == id {
            assert forall|j: int| #[trigger] r2.installed.contains(j) implies is_shard(c, j) && writes_at(c, r2.body, j) by {
                if j != i { assert(r.installed.contains(j)); }
            }
            assert forall|x: int| is_comp(c, x) implies #[trigger] r2.vc[x] >= 0 by { assert(r.vc[x] >= 0); }
            assert forall|j: int| is_shard(c, j) && #[trigger] clock_shard(c, r2, j) implies r2.vc[group(c, j)] >= 1 by {
                assert(clock_shard(c, r, j));
            }
            assert forall|k: int| #[trigger] r2.reads.dom().contains(k) implies read_set(r2.body).contains(k) by {}
            assert forall|k: int| #[trigger] read_set(r2.body).contains(k) implies r2.reads.dom().contains(k) by {}
            assert(inv_txn(s2, c, id));
        } else {
            assert(ro2 == ro);
            assert(inv_txn(s2, c, o));
        }
    }
}

pub proof fn lemma_install_prepared(s: State, c: Constants, id: int, i: int)
    requires inv(s, c), can_install(s, c, id, i)
    ensures inv_prepared(apply(s, c, Action::Install { id, shard: i }), c)
{
    let s2 = apply(s, c, Action::Install { id, shard: i });
    lemma_install_txn_facts(s, c, id, i);
    assert forall|j: int| 0 <= j < s2.prepared.len() implies
        has_txn(s2.txns, #[trigger] s2.prepared[j]) && txn(s2.txns, s2.prepared[j]).pidx == j
        && is_prepared_or_later(txn(s2.txns, s2.prepared[j])) by {
        assert(is_prepared_or_later(txn(s.txns, s.prepared[j])));
    }
    assert forall|j1: int, j2: int| 0 <= j1 < j2 < s2.prepared.len() implies
        txn(s2.txns, #[trigger] s2.prepared[j1]).prepared_at < txn(s2.txns, #[trigger] s2.prepared[j2]).prepared_at by {
        assert(txn(s.txns, s.prepared[j1]).prepared_at < txn(s.txns, s.prepared[j2]).prepared_at);
    }
}

pub proof fn lemma_install_exclusive(s: State, c: Constants, id: int, i: int)
    requires inv(s, c), can_install(s, c, id, i)
    ensures inv_exclusive(apply(s, c, Action::Install { id, shard: i }), c)
{
    let s2 = apply(s, c, Action::Install { id, shard: i });
    lemma_install_txn_facts(s, c, id, i);
    assert forall|a: int, b: int| #[trigger] s2.txns.dom().contains(a) && #[trigger] s2.txns.dom().contains(b) && a != b
        && txn(s2.txns, a).coord == txn(s2.txns, b).coord && txn(s2.txns, a).thread == txn(s2.txns, b).thread
        implies !(in_flight(txn(s2.txns, a)) && in_flight(txn(s2.txns, b))) by {
        assert(in_flight(txn(s2.txns, a)) == in_flight(txn(s.txns, a)));
        assert(in_flight(txn(s2.txns, b)) == in_flight(txn(s.txns, b)));
        assert(txn(s2.txns, a).coord == txn(s.txns, a).coord && txn(s2.txns, a).thread == txn(s.txns, a).thread);
        assert(txn(s2.txns, b).coord == txn(s.txns, b).coord && txn(s2.txns, b).thread == txn(s.txns, b).thread);
        assert(s.txns.dom().contains(a) && s.txns.dom().contains(b));
    }
}

pub proof fn lemma_install_locks(s: State, c: Constants, id: int, i: int)
    requires inv(s, c), can_install(s, c, id, i)
    ensures inv_locks(apply(s, c, Action::Install { id, shard: i }), c)
{
    let s2 = apply(s, c, Action::Install { id, shard: i });
    let r = txn(s.txns, id);
    let keys = keys_at(c, r.body, i);
    lemma_install_shard_facts(s, c, id, i);
    lemma_install_txn_facts(s, c, id, i);
    lemma_install_version_facts(s, c, id, i);
    lemma_install_lock_facts(s, c, id, i);
    assert forall|k: int| #[trigger] s2.locks.dom().contains(k) implies inv_lock(s2, c, k) by {
        assert(!keys.contains(k));
        assert(s.locks.dom().contains(k));
        assert(inv_lock(s, c, k));
        let h = s.locks[k];
        assert(s2.locks[k] == h);
        if h == id {
            assert(owner(c, k) != i);
        }
        assert(vers(s2.versions, k) == vers(s.versions, k));
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
        let ro = txn(s.txns, o);
        assert(inv_txn(s, c, o));
        assert(valid_txn(ro.body));
        assert(ro.body.ops.dom().contains(k));
        lemma_install_owner_is_shard(c, k);
        assert(shard_epoch(s2.shards, owner(c, k)) == shard_epoch(s.shards, owner(c, k)));
        if o == id {
            assert(owner(c, k) != i);
            assert(!keys.contains(k));
            assert(s.locks.dom().contains(k) && s.locks[k] == id);
        } else {
            assert(txn(s2.txns, o) == ro);
            assert(s.locks.dom().contains(k) && s.locks[k] == o);
            if keys.contains(k) {
                assert(s.locks[k] == id);
            }
            assert(!keys.contains(k));
        }
    }
}

pub proof fn lemma_install_versions(s: State, c: Constants, id: int, i: int)
    requires inv(s, c), can_install(s, c, id, i)
    ensures inv_versions(apply(s, c, Action::Install { id, shard: i }), c)
{
    let s2 = apply(s, c, Action::Install { id, shard: i });
    let r = txn(s.txns, id);
    let r2 = txn(s2.txns, id);
    let keys = keys_at(c, r.body, i);
    lemma_install_txn_facts(s, c, id, i);
    lemma_install_version_facts(s, c, id, i);
    lemma_install_lock_facts(s, c, id, i);
    assert forall|k: int| #[trigger] s2.versions.dom().contains(k) implies inv_versions_of(s2, c, k) by {
        if keys.contains(k) {
            let old = vers(s.versions, k);
            let nv = new_version(r, id, k);
            assert(s2.versions[k] == old.push(nv));
            let m = old.len() as int;
            assert(s2.versions[k][m] == nv);
            assert(inv_version(s2, c, k, nv)) by {
                assert(r2.installed.contains(i));
                assert(owner(c, k) == i);
                assert(write_set(r.body).contains(k));
            }
            if s.versions.dom().contains(k) { assert(inv_versions_of(s, c, k)); }
            assert forall|j: int| 0 <= j < old.len() implies inv_version(s2, c, k, #[trigger] old[j]) && old[j].txn != id by {
                assert(s.versions.dom().contains(k));
                assert(inv_version(s, c, k, old[j]));
                if old[j].txn == id { assert(r.installed.contains(owner(c, k))); }
            }
            assert(inv_lock(s, c, k));
            assert forall|j: int| 0 <= j < s2.versions[k].len() implies inv_version(s2, c, k, #[trigger] s2.versions[k][j]) by {
                if j < m { assert(s2.versions[k][j] == old[j]); }
            }
            assert forall|a: int, b: int| 0 <= a < b < s2.versions[k].len() implies
                txn(s2.txns, #[trigger] s2.versions[k][a].txn).pidx < txn(s2.txns, #[trigger] s2.versions[k][b].txn).pidx by {
                assert(s2.versions[k][a] == old[a]);
                assert(old[a].txn != id);
                assert(s.versions.dom().contains(k));
                assert(inv_version(s, c, k, old[a]));
                assert(old[a].txn >= 0);
                if b < m {
                    assert(s2.versions[k][b] == old[b]);
                    assert(inv_version(s, c, k, old[b]));
                    assert(old[b].txn != id) by {
                        if old[b].txn == id { assert(r.installed.contains(owner(c, k))); }
                    }
                    assert(txn(s.txns, old[a].txn).pidx < txn(s.txns, old[b].txn).pidx);
                } else {
                    assert(s2.versions[k][b] == nv);
                    assert(pidx_of(s.txns, old[a].txn) < txn(s.txns, id).pidx);
                }
            }
        } else {
            assert(s.versions.dom().contains(k));
            assert(inv_versions_of(s, c, k));
            assert(s2.versions[k] == s.versions[k]);
            assert forall|j: int| 0 <= j < s2.versions[k].len() implies inv_version(s2, c, k, #[trigger] s2.versions[k][j]) by {
                assert(inv_version(s, c, k, s.versions[k][j]));
                let o = s.versions[k][j].txn;
                if o == id {
                    assert(r2.installed.contains(owner(c, k)));
                }
            }
            assert forall|a: int, b: int| 0 <= a < b < s2.versions[k].len() implies
                txn(s2.txns, #[trigger] s2.versions[k][a].txn).pidx < txn(s2.txns, #[trigger] s2.versions[k][b].txn).pidx by {
                assert(txn(s.txns, s.versions[k][a].txn).pidx < txn(s.txns, s.versions[k][b].txn).pidx);
                assert(inv_version(s, c, k, s.versions[k][a]));
                assert(inv_version(s, c, k, s.versions[k][b]));
                assert(s.versions[k][a].txn >= 0 && s.versions[k][b].txn >= 0);
                assert(pidx_of(s2.txns, s.versions[k][a].txn) == pidx_of(s.txns, s.versions[k][a].txn));
                assert(pidx_of(s2.txns, s.versions[k][b].txn) == pidx_of(s.txns, s.versions[k][b].txn));
            }
        }
    }
}

pub proof fn lemma_install_reads(s: State, c: Constants, id: int, i: int)
    requires inv(s, c), can_install(s, c, id, i)
    ensures inv_reads(apply(s, c, Action::Install { id, shard: i }), c)
{
    let s2 = apply(s, c, Action::Install { id, shard: i });
    let r = txn(s.txns, id);
    let r2 = txn(s2.txns, id);
    let keys = keys_at(c, r.body, i);
    lemma_install_shard_facts(s, c, id, i);
    lemma_install_txn_facts(s, c, id, i);
    lemma_install_version_facts(s, c, id, i);
    assert forall|o: int, k: int| #[trigger] s2.txns.dom().contains(o) && #[trigger] txn(s2.txns, o).reads.dom().contains(k)
        implies inv_read(s2, c, o, k) by {
        let ro = txn(s.txns, o);
        assert(txn(s2.txns, o).reads == ro.reads);
        assert(inv_read(s, c, o, k));
        let rd = ro.reads[k];
        if rd.writer != -1 {
            assert(inv_read_writer(s, c, o, k));
            if rd.writer == id {
                assert(r2.installed.contains(owner(c, k)));
            }
            assert(inv_read_writer(s2, c, o, k));
        }
        if is_prepared_or_later(txn(s2.txns, o)) {
            assert(is_prepared_or_later(ro));
            assert forall|p: int| #[trigger] s2.txns.dom().contains(p) && p != o
                && is_prepared_or_later(txn(s2.txns, p)) && txn(s2.txns, p).pidx < txn(s2.txns, o).pidx
                && write_set(txn(s2.txns, p).body).contains(k) implies inv_read_order(s2, c, o, k, p) by {
                let rp = txn(s.txns, p);
                assert(inv_txn(s, c, p));
                assert(valid_txn(rp.body));
                assert(rp.body.ops.dom().contains(k));
                lemma_install_owner_is_shard(c, k);
                assert(shard_epoch(s2.shards, owner(c, k)) == shard_epoch(s.shards, owner(c, k)));
                assert(inv_read_order(s, c, o, k, p));
                if p == id {
                    if owner(c, k) == i {
                        // id was uninstalled at k's owner when o certified, and is
                        // neither aborted nor behind the owner's epoch: impossible.
                        assert(!rp.installed.contains(owner(c, k)));
                        assert(rp.status is Aborted || shard_epoch(s.shards, owner(c, k)) > rp.epoch);
                        assert(false);
                    } else {
                        assert(!keys.contains(k));
                        assert(r2.installed.contains(owner(c, k)) == r.installed.contains(owner(c, k)));
                        assert(has_version(s2.versions, k, id) == has_version(s.versions, k, id));
                    }
                } else {
                    assert(txn(s2.txns, p) == rp);
                    assert(has_version(s2.versions, k, p) == has_version(s.versions, k, p));
                }
                assert(pidx_of(s2.txns, p) == pidx_of(s.txns, p));
                assert(pidx_of(s2.txns, rd.writer) == pidx_of(s.txns, rd.writer));
            }
        }
    }
}

pub proof fn lemma_install_streams_other(s: State, c: Constants, id: int, i: int, sid2: Sid)
    requires inv(s, c), can_install(s, c, id, i), valid_sid(c, sid2),
        sid2 != coord_sid(txn(s.txns, id), i) || i == txn(s.txns, id).coord,
    ensures inv_stream(apply(s, c, Action::Install { id, shard: i }), c, sid2)
{
    let s2 = apply(s, c, Action::Install { id, shard: i });
    let r = txn(s.txns, id);
    let r2 = txn(s2.txns, id);
    lemma_install_shard_facts(s, c, id, i);
    lemma_install_txn_facts(s, c, id, i);
    lemma_install_stream_facts(s, c, id, i);
    assert(inv_stream(s, c, sid2));
    assert(s2.streams[sid2] == s.streams[sid2]);
    let es_o = all_entries(s.streams[sid2]);
    assert(all_entries(s2.streams[sid2]) == es_o);
    assert forall|j: int| 0 <= j < es_o.len() implies inv_entry(s2, c, sid2, #[trigger] es_o[j]) by {
        assert(inv_entry(s, c, sid2, es_o[j]));
        assert(shard_epoch(s2.shards, sid2.shard) == shard_epoch(s.shards, sid2.shard));
        assert(counter(s2.shards, sid2.shard) >= counter(s.shards, sid2.shard));
        if es_o[j] is Log && es_o[j]->Log_txn == id {
            assert(logged_at(c, r, sid2.shard));
            assert(logged_at(c, r2, sid2.shard));
        }
    }
}

pub proof fn lemma_install_streams_dedicated(s: State, c: Constants, id: int, i: int)
    requires inv(s, c), can_install(s, c, id, i), i != txn(s.txns, id).coord,
    ensures inv_stream(apply(s, c, Action::Install { id, shard: i }), c, coord_sid(txn(s.txns, id), i))
{
    let s2 = apply(s, c, Action::Install { id, shard: i });
    let r = txn(s.txns, id);
    let r2 = txn(s2.txns, id);
    let e = r.epoch;
    let sid = coord_sid(r, i);
    let ent = log_entry(c, r, id, i);
    let es = all_entries(s.streams[sid]);
    let es2 = all_entries(s2.streams[sid]);
    lemma_install_shard_facts(s, c, id, i);
    lemma_install_txn_facts(s, c, id, i);
    lemma_install_stream_facts(s, c, id, i);
    assert(inv_stream(s, c, sid));
    let n = es.len() as int;
    assert(es2 == es.push(ent));
    assert(es2[n] == ent);
    assert(forall|j: int| 0 <= j < n ==> es2[j] == es[j]);
    assert(shard_epoch(s2.shards, i) == shard_epoch(s.shards, i));
    assert(inv_entry(s2, c, sid, ent)) by {
        assert(logged_at(c, r2, i));
        assert(ent->Log_clock <= counter(s2.shards, i));
    }
    assert forall|j: int| 0 <= j < es2.len() implies inv_entry(s2, c, sid, #[trigger] es2[j]) by {
        if j < n {
            assert(es2[j] == es[j]);
            assert(inv_entry(s, c, sid, es[j]));
            assert(counter(s2.shards, i) >= counter(s.shards, i));
            if es[j] is Log && es[j]->Log_txn == id {
                assert(logged_at(c, r, i));
                assert(logged_at(c, r2, i));
            }
        }
    }
    assert(inv_stream_order(es2)) by {
        assert forall|a: int, b: int| 0 <= a < b < es2.len() implies entry_epoch(#[trigger] es2[a]) <= entry_epoch(#[trigger] es2[b]) by {
            assert(es2[a] == es[a]);
            if b == n { assert(inv_entry(s, c, sid, es[a])); } else { assert(es2[b] == es[b]); }
        }
        assert forall|a: int, b: int| 0 <= a < b < es2.len() && #[trigger] es2[a] is Log && #[trigger] es2[b] is Log
            && es2[a]->Log_epoch == es2[b]->Log_epoch implies es2[a]->Log_clock < es2[b]->Log_clock by {
            assert(es2[a] == es[a]);
            if b == n { assert(es[a] is Log && es[a]->Log_epoch == e); } else { assert(es2[b] == es[b]); }
        }
        assert forall|a: int, b: int| 0 <= a < b < es2.len() && #[trigger] es2[a] is Inf
            implies entry_epoch(#[trigger] es2[b]) > es2[a]->Inf_epoch by {
            assert(es2[a] == es[a]);
            if b == n { assert(inv_entry(s, c, sid, es[a])); } else { assert(es2[b] == es[b]); }
        }
        assert forall|a: int, b: int| 0 <= a < b < es2.len() && #[trigger] es2[a] is Log && #[trigger] es2[b] is Log
            implies es2[a]->Log_txn != es2[b]->Log_txn by {
            assert(es2[a] == es[a]);
            if b == n {
                if es[a]->Log_txn == id { assert(is_log_of(es[a], id)); }
            } else { assert(es2[b] == es[b]); }
        }
        assert forall|a: int| 0 <= a < es2.len() && #[trigger] es2[a] is Log implies es2[a]->Log_clock >= 1 by {
            if a < n { assert(es2[a] == es[a]); }
        }
    }
}

pub proof fn lemma_install_streams(s: State, c: Constants, id: int, i: int)
    requires inv(s, c), can_install(s, c, id, i)
    ensures inv_streams(apply(s, c, Action::Install { id, shard: i }), c)
{
    let s2 = apply(s, c, Action::Install { id, shard: i });
    let r = txn(s.txns, id);
    assert forall|sid2: Sid| valid_sid(c, sid2) implies #[trigger] inv_stream(s2, c, sid2) by {
        if sid2 != coord_sid(r, i) || i == r.coord {
            lemma_install_streams_other(s, c, id, i, sid2);
        } else {
            lemma_install_streams_dedicated(s, c, id, i);
        }
    }
}

// ---------------------------------------------------------------------------
// inv_all_logs, per named predicate
// ---------------------------------------------------------------------------

/// Which of a transaction's streams changed: only the installing
/// transaction's dedicated stream at `i`, and only when `i` is not its coordinator.
pub proof fn lemma_install_stream_at(s: State, c: Constants, id: int, i: int, o: int, j: int)
    requires inv(s, c), can_install(s, c, id, i)
    ensures ({
        let s2 = apply(s, c, Action::Install { id, shard: i });
        let r = txn(s.txns, id);
        let ro = txn(s.txns, o);
        let ro2 = txn(s2.txns, o);
        &&& coord_sid(ro2, j) == coord_sid(ro, j)
        &&& (coord_sid(ro, j) != coord_sid(r, i) || i == r.coord) ==>
            stream_at(s2.streams, ro2, j) == stream_at(s.streams, ro, j)
        &&& (coord_sid(ro, j) == coord_sid(r, i) && i != r.coord) ==>
            j == i && ro.coord == r.coord && ro.thread == r.thread
            && stream_at(s2.streams, ro2, j) == stream_at(s.streams, ro, j).push(log_entry(c, r, id, i))
    })
{
    lemma_install_txn_facts(s, c, id, i);
    lemma_install_stream_facts(s, c, id, i);
}

pub proof fn lemma_install_coord_below(s: State, c: Constants, id: int, i: int, o: int)
    requires inv(s, c), can_install(s, c, id, i), has_txn(s.txns, o)
    ensures ({
        let s2 = apply(s, c, Action::Install { id, shard: i });
        inv_coord_below(s2, c, txn(s2.txns, o))
    })
{
    let s2 = apply(s, c, Action::Install { id, shard: i });
    let r = txn(s.txns, id);
    let ro = txn(s.txns, o);
    let ro2 = txn(s2.txns, o);
    lemma_install_txn_facts(s, c, id, i);
    assert(inv_logs(s, c, o));
    assert(inv_coord_below(s, c, ro));
    lemma_install_stream_at(s, c, id, i, o, ro.coord);
    assert(ro2.status == ro.status && ro2.epoch == ro.epoch && ro2.vc == ro.vc && ro2.coord == ro.coord);
    if ro.status is Prepared || aborted_prepared(ro) {
        assert(!(coord_sid(ro, ro.coord) == coord_sid(r, i) && i != r.coord));
        assert(stream_at(s2.streams, ro2, ro2.coord) == stream_at(s.streams, ro, ro.coord));
    }
}

pub proof fn lemma_install_uninstalled_below(s: State, c: Constants, id: int, i: int, o: int)
    requires inv(s, c), can_install(s, c, id, i), has_txn(s.txns, o)
    ensures ({
        let s2 = apply(s, c, Action::Install { id, shard: i });
        inv_uninstalled_below(s2, c, txn(s2.txns, o))
    })
{
    let s2 = apply(s, c, Action::Install { id, shard: i });
    let r = txn(s.txns, id);
    let ro = txn(s.txns, o);
    let ro2 = txn(s2.txns, o);
    lemma_install_txn_facts(s, c, id, i);
    assert(inv_logs(s, c, o));
    assert(inv_uninstalled_below(s, c, ro));
    if ro2.status is Prepared {
        assert forall|j: int| is_shard(c, j) && #[trigger] clock_shard(c, ro2, j) && !ro2.installed.contains(j)
            implies logs_below(stream_at(s2.streams, ro2, j), ro2.epoch, ro2.vc[group(c, j)]) by {
            lemma_install_stream_at(s, c, id, i, o, j);
            if o == id {
                assert(j != i);
                assert(clock_shard(c, r, j) && !r.installed.contains(j));
                assert(coord_sid(r, j) != coord_sid(r, i));
            } else {
                assert(ro2 == ro);
                if coord_sid(ro, j) == coord_sid(r, i) && i != r.coord {
                    assert(ro.coord == r.coord && ro.thread == r.thread);
                    assert(in_flight(ro) && in_flight(r));
                    assert(s.txns.dom().contains(o) && s.txns.dom().contains(id));
                    assert(false);
                }
            }
        }
    }
}

pub proof fn lemma_install_logged(s: State, c: Constants, id: int, i: int, o: int)
    requires inv(s, c), can_install(s, c, id, i), has_txn(s.txns, o)
    ensures ({
        let s2 = apply(s, c, Action::Install { id, shard: i });
        inv_logged(s2, c, txn(s2.txns, o), o)
    })
{
    let s2 = apply(s, c, Action::Install { id, shard: i });
    let r = txn(s.txns, id);
    let ro = txn(s.txns, o);
    let ro2 = txn(s2.txns, o);
    let e = r.epoch;
    let ent = log_entry(c, r, id, i);
    lemma_install_shard_facts(s, c, id, i);
    lemma_install_txn_facts(s, c, id, i);
    assert(inv_logs(s, c, o));
    assert(inv_logged(s, c, ro, o));
    assert forall|j: int| is_shard(c, j) && #[trigger] logged_at(c, ro2, j) implies
        has_log(stream_at(s2.streams, ro2, j), o)
        || (shard_epoch(s2.shards, j) > ro2.epoch && stream_below(stream_at(s2.streams, ro2, j), ro2.epoch, ro2.vc[group(c, j)])) by {
        lemma_install_stream_at(s, c, id, i, o, j);
        assert(shard_epoch(s2.shards, j) == shard_epoch(s.shards, j));
        if o == id {
            if j == i {
                assert(i != r.coord);
                let es = stream_at(s.streams, r, i);
                lemma_install_has_log_push(es, ent, id);
            } else {
                assert(logged_at(c, r, j));
                assert(coord_sid(r, j) != coord_sid(r, i));
            }
        } else {
            assert(ro2 == ro);
            if coord_sid(ro, j) == coord_sid(r, i) && i != r.coord {
                let es = stream_at(s.streams, ro, j);
                let es2 = stream_at(s2.streams, ro2, j);
                assert(j == i);
                assert(es2 == es.push(ent));
                lemma_install_has_log_push(es, ent, o);
                if !has_log(es, o) {
                    assert(shard_epoch(s.shards, i) > ro.epoch);
                    assert(ro.epoch != e);
                    assert(stream_below(es, ro.epoch, ro.vc[group(c, i)]));
                    let n = es.len() as int;
                    assert forall|a: int| 0 <= a < es2.len() && #[trigger] es2[a] is Log && es2[a]->Log_epoch == ro.epoch
                        implies es2[a]->Log_clock < ro.vc[group(c, i)] by {
                        if a < n { assert(es2[a] == es[a]); } else { assert(es2[a] == ent); }
                    }
                    assert forall|a: int| 0 <= a < es2.len() implies !(#[trigger] es2[a] is Inf && es2[a]->Inf_epoch == ro.epoch) by {
                        if a < n { assert(es2[a] == es[a]); } else { assert(es2[a] == ent); }
                    }
                    assert(stream_below(es2, ro.epoch, ro.vc[group(c, i)]));
                }
            }
        }
    }
}

pub proof fn lemma_install_present(s: State, c: Constants, id: int, i: int, o: int)
    requires inv(s, c), can_install(s, c, id, i), has_txn(s.txns, o)
    ensures ({
        let s2 = apply(s, c, Action::Install { id, shard: i });
        inv_present(s2, c, txn(s2.txns, o), o)
    })
{
    let s2 = apply(s, c, Action::Install { id, shard: i });
    let r = txn(s.txns, id);
    let ro = txn(s.txns, o);
    let ro2 = txn(s2.txns, o);
    let keys = keys_at(c, r.body, i);
    let ent = log_entry(c, r, id, i);
    lemma_install_txn_facts(s, c, id, i);
    lemma_install_version_facts(s, c, id, i);
    assert(inv_logs(s, c, o));
    assert(inv_present(s, c, ro, o));
    assert forall|k: int| (ro2.status is Prepared || certified_or_committed(ro2))
        && #[trigger] write_set(ro2.body).contains(k) && ro2.installed.contains(owner(c, k))
        && !has_version(s2.versions, k, o) implies
        (doomed(s2.final_wm, c, ro2) && s2.rolled_back.contains((owner(c, k), ro2.epoch))) || lost(s2.streams, c, ro2, o) by {
        if o == id {
            assert(!keys.contains(k));
            assert(owner(c, k) != i);
            assert(r.installed.contains(owner(c, k)));
            assert(!has_version(s.versions, k, id));
        } else {
            assert(ro2 == ro);
            assert(!has_version(s.versions, k, o));
        }
        assert(ro.status is Prepared || certified_or_committed(ro));
        assert(write_set(ro.body).contains(k) && ro.installed.contains(owner(c, k)));
        if lost(s.streams, c, ro, o) {
            let j = choose|j: int| is_shard(c, j) && #[trigger] logged_at(c, ro, j) && !has_log(stream_at(s.streams, ro, j), o);
            lemma_install_stream_at(s, c, id, i, o, j);
            if o == id {
                assert(j != i);
                assert(logged_at(c, ro2, j));
                assert(coord_sid(r, j) != coord_sid(r, i));
            } else {
                if coord_sid(ro, j) == coord_sid(r, i) && i != r.coord {
                    lemma_install_has_log_push(stream_at(s.streams, ro, j), ent, o);
                }
            }
            assert(logged_at(c, ro2, j));
            assert(!has_log(stream_at(s2.streams, ro2, j), o));
            assert(lost(s2.streams, c, ro2, o));
        }
    }
}

pub proof fn lemma_install_all_logs(s: State, c: Constants, id: int, i: int)
    requires inv(s, c), can_install(s, c, id, i)
    ensures inv_all_logs(apply(s, c, Action::Install { id, shard: i }), c)
{
    let s2 = apply(s, c, Action::Install { id, shard: i });
    lemma_install_txn_facts(s, c, id, i);
    assert forall|o: int| #[trigger] s2.txns.dom().contains(o) implies inv_logs(s2, c, o) by {
        lemma_install_coord_below(s, c, id, i, o);
        lemma_install_uninstalled_below(s, c, id, i, o);
        lemma_install_logged(s, c, id, i, o);
        lemma_install_present(s, c, id, i, o);
    }
}

// ---------------------------------------------------------------------------
// Watermark-related conjuncts
// ---------------------------------------------------------------------------

pub proof fn lemma_install_committed(s: State, c: Constants, id: int, i: int)
    requires inv(s, c), can_install(s, c, id, i)
    ensures inv_committed(apply(s, c, Action::Install { id, shard: i }), c)
{
    let s2 = apply(s, c, Action::Install { id, shard: i });
    lemma_install_txn_facts(s, c, id, i);
    lemma_install_stream_facts(s, c, id, i);
    assert forall|o: int| #[trigger] s2.txns.dom().contains(o) && committed(txn(s2.txns, o))
        implies below_wm(s2.streams, c, txn(s2.txns, o).vc, txn(s2.txns, o).epoch) by {
        assert(o != id);
        assert(txn(s2.txns, o) == txn(s.txns, o));
        let vc = txn(s.txns, o).vc;
        let eo = txn(s.txns, o).epoch;
        assert(below_wm(s.streams, c, vc, eo));
        assert forall|sid2: Sid| valid_sid(c, sid2) implies
            wm_le(vc[group(c, sid2.shard)], stream_wm(#[trigger] s2.streams[sid2].durable, eo)) by {
            assert(wm_le(vc[group(c, sid2.shard)], stream_wm(s.streams[sid2].durable, eo)));
        }
    }
}

pub proof fn lemma_install_final(s: State, c: Constants, id: int, i: int)
    requires inv(s, c), can_install(s, c, id, i)
    ensures inv_final(apply(s, c, Action::Install { id, shard: i }), c)
{
    let s2 = apply(s, c, Action::Install { id, shard: i });
    let r = txn(s.txns, id);
    let e = r.epoch;
    let sid = coord_sid(r, i);
    let ent = log_entry(c, r, id, i);
    lemma_install_shard_facts(s, c, id, i);
    lemma_install_txn_facts(s, c, id, i);
    lemma_install_stream_facts(s, c, id, i);
    assert forall|j: int, e2: nat| #[trigger] s2.final_wm.dom().contains((j, e2)) implies inv_final_of(s2, c, j, e2) by {
        assert(inv_final_of(s, c, j, e2));
        assert(shard_epoch(s2.shards, j) == shard_epoch(s.shards, j));
        assert forall|sid2: Sid| valid_sid(c, sid2) && sid2.shard == j implies
            no_pending_epoch(#[trigger] s2.streams[sid2], e2)
            && wm_le_wm(s2.final_wm[(j, e2)], stream_wm(s2.streams[sid2].durable, e2)) by {
            assert(no_pending_epoch(s.streams[sid2], e2));
            assert(wm_le_wm(s.final_wm[(j, e2)], stream_wm(s.streams[sid2].durable, e2)));
            if i != r.coord && sid2 == sid {
                assert(shard_epoch(s.shards, i) > e2);
                assert(e2 != e);
                let p2 = s2.streams[sid].pending;
                let p = s.streams[sid].pending;
                assert(p2 == p.push(ent));
                assert forall|q: int| 0 <= q < p2.len() implies entry_epoch(#[trigger] p2[q]) != e2 by {
                    if q < p.len() { assert(p2[q] == p[q]); } else { assert(p2[q] == ent); }
                }
            } else {
                assert(s2.streams[sid2] == s.streams[sid2]);
            }
        }
        let w = choose|w: Sid| valid_sid(c, w) && w.shard == j && s.final_wm[(j, e2)] == stream_wm(#[trigger] s.streams[w].durable, e2);
        assert(s2.streams[w].durable == s.streams[w].durable);
        assert(s2.final_wm[(j, e2)] == stream_wm(s2.streams[w].durable, e2));
    }
}

pub proof fn lemma_install_rolled_back(s: State, c: Constants, id: int, i: int)
    requires inv(s, c), can_install(s, c, id, i)
    ensures inv_rolled_back(apply(s, c, Action::Install { id, shard: i }), c)
{
    let s2 = apply(s, c, Action::Install { id, shard: i });
    let r = txn(s.txns, id);
    let keys = keys_at(c, r.body, i);
    lemma_install_shard_facts(s, c, id, i);
    lemma_install_txn_facts(s, c, id, i);
    lemma_install_version_facts(s, c, id, i);
    assert forall|j: int, e2: nat| #[trigger] s2.rolled_back.contains((j, e2)) implies inv_rolled_back_of(s2, c, j, e2) by {
        assert(inv_rolled_back_of(s, c, j, e2));
        assert forall|k: int, q: int| #[trigger] s2.versions.dom().contains(k) && owner(c, k) == j
            && 0 <= q < s2.versions[k].len() && #[trigger] s2.versions[k][q].epoch == e2
            implies below_fvw(s2.final_wm, c, s2.versions[k][q].vc, e2) by {
            if keys.contains(k) {
                let old = vers(s.versions, k);
                let m = old.len() as int;
                assert(s2.versions[k] == old.push(new_version(r, id, k)));
                if q < m {
                    assert(s2.versions[k][q] == old[q]);
                    assert(s.versions.dom().contains(k));
                    assert(s.versions[k][q].epoch == e2);
                } else {
                    assert(s2.versions[k][q] == new_version(r, id, k));
                    assert(j == i);
                    assert(e2 == r.epoch);
                    assert(fvw_ready(s.final_wm, c, e2));
                    assert(s.final_wm.dom().contains((i, e2)));
                    assert(inv_final_of(s, c, i, e2));
                    assert(false);
                }
            } else {
                assert(s.versions.dom().contains(k));
                assert(s2.versions[k] == s.versions[k]);
                assert(s.versions[k][q].epoch == e2);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// The action
// ---------------------------------------------------------------------------

pub proof fn lemma_install_inv(s: State, c: Constants, id: int, i: int)
    requires inv(s, c), can_install(s, c, id, i)
    ensures inv(apply(s, c, Action::Install { id, shard: i }), c)
{
    lemma_install_shapes(s, c, id, i);
    lemma_install_txns(s, c, id, i);
    lemma_install_prepared(s, c, id, i);
    lemma_install_exclusive(s, c, id, i);
    lemma_install_locks(s, c, id, i);
    lemma_install_versions(s, c, id, i);
    lemma_install_reads(s, c, id, i);
    lemma_install_streams(s, c, id, i);
    lemma_install_all_logs(s, c, id, i);
    lemma_install_committed(s, c, id, i);
    lemma_install_final(s, c, id, i);
    lemma_install_rolled_back(s, c, id, i);
}

} // verus!
