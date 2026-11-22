//! OCC induction and finite-sequence facts used by the transaction proofs.
use super::types::*;
use super::normal::*;
use super::recovery::*;
use super::behavior::*;
use super::invariants::*;
use super::log_invariants::*;
use super::log_lemmas::*;
use super::proofs_replication::{lemma_stable_step, lemma_doomed_step, lemma_closed_cut_step, lemma_version_lost_step};
use vstd::prelude::*;

verus! {

pub proof fn lemma_owner_is_shard(c: Constants, k: int)
    requires valid_constants(c), k >= 0
    ensures is_shard(c, owner(c, k))
{
    vstd::arithmetic::div_mod::lemma_mod_pos_bound(k, c.shards);
}
pub proof fn lemma_writes_at_owner(c: Constants, t: Txn, k: int)
    requires valid_constants(c), valid_txn(t), write_set(t).contains(k)
    ensures is_shard(c, owner(c, k)), writes_at(c, t, owner(c, k))
{
    assert(t.ops.dom().contains(k));
    lemma_owner_is_shard(c, k);
}
pub proof fn lemma_occ_init(s: State, c: Constants)
    requires init(s, c)
    ensures occ_inv(s, c)
{
    assert(s.logs[0].durable.len() == 0);
    assert(cm_epoch(s) == 0);
    assert forall|i: int| is_shard(c, i) implies #[trigger] shard_epoch(s.shards, i) <= cm_epoch(s)
        && #[trigger] clock(s.shards, i) >= 0 by {}
}

pub proof fn lemma_txns_step(s: State, c: Constants, a: Action)
    requires occ_inv(s, c), enabled(s, c, a)
    ensures inv_txns(apply(s, c, a), c), inv_order(apply(s, c, a))
{
    let z = apply(s, c, a);
    lemma_metadata_step(s, c, a);
    assert forall|id: int| #[trigger] z.txns.dom().contains(id)
        implies id >= 0 && inv_txn(z, c, id) by {
        if s.txns.dom().contains(id) { assert(inv_txn(s, c, id)); }
        if s.txns.dom().contains(id) && prepared(s.txns[id]) {
            assert forall|i: int| is_shard(c, i) && #[trigger] clock_participant(c, z.txns[id], i)
                implies z.txns[id].epoch <= z.shards[i].epoch by {
                assert(clock_participant(c, s.txns[id], i));
            }
        }
        match a {
            Action::Submit { id: n, body, coord } => { assert(inv_txn(z, c, id)); },
            Action::Read { id: n, key } => { assert(inv_txn(z, c, id)); },
            Action::Prepare { id: n, ts } => {
                if id == n {
                    assert forall|i: int| is_shard(c, i) && #[trigger] clock_participant(c, z.txns[id], i)
                        implies z.txns[id].epoch <= z.shards[i].epoch by {
                        if i != s.txns[n].coord {
                            let k = choose|k: int| #[trigger] s.txns[n].body.ops.dom().contains(k) && owner(c, k) == i;
                        }
                    }
                }
                assert(inv_txn(z, c, id));
            },
            Action::Install { id: n, shard } => { assert(inv_txn(z, c, id)); },
            Action::Publish { id: n, shard } => { assert(inv_txn(z, c, id)); },
            Action::Certify { id: n } => { assert(inv_txn(z, c, id)); },
            Action::Provisional { id: n } => { assert(inv_txn(z, c, id)); },
            Action::Final { id: n } => { assert(inv_txn(z, c, id)); },
            Action::Abort { id: n } => { assert(inv_txn(z, c, id)); },
            Action::Pulse { shard, ts } => { assert(inv_txn(z, c, id)); },
            Action::Barrier { shard, through } => { assert(inv_txn(z, c, id)); },
            Action::Replicate { shard } => { assert(inv_txn(z, c, id)); },
            Action::Send { shard, epoch } => { assert(inv_txn(z, c, id)); },
            Action::Receive { observer, report } => { assert(inv_txn(z, c, id)); },
            Action::ProposeEpoch => { assert(inv_txn(z, c, id)); },
            Action::Crash { shard, survive } => { assert(inv_txn(z, c, id)); },
            Action::Recover { shard, floor } => { assert(inv_txn(z, c, id)); },
            Action::ObserveEpoch { shard } => { assert(inv_txn(z, c, id)); },
            Action::Close { shard, epoch } => { assert(inv_txn(z, c, id)); },
            Action::Rollback { shard, epoch } => { assert(inv_txn(z, c, id)); },
            Action::Stutter => { assert(inv_txn(z, c, id)); },
        }
    }
    match a {
        Action::Prepare { id, ts } => {},
        _ => {},
    }
}

pub proof fn lemma_filter_index<A>(vs: Seq<A>, p: spec_fn(A) -> bool, a: int) -> (x: int)
    requires 0 <= a < vs.filter(p).len()
    ensures 0 <= x < vs.len(), vs[x] == vs.filter(p)[a], p(vs[x])
    decreases vs.len()
{
    reveal(Seq::filter);
    let sub = vs.drop_last().filter(p);
    if p(vs.last()) {
        if a == sub.len() { vs.len() - 1 } else {
            let x = lemma_filter_index(vs.drop_last(), p, a);
            assert(vs.drop_last()[x] == vs[x]);
            x
        }
    } else {
        let x = lemma_filter_index(vs.drop_last(), p, a);
        assert(vs.drop_last()[x] == vs[x]);
        x
    }
}
pub proof fn lemma_filter_order<A>(vs: Seq<A>, p: spec_fn(A) -> bool, a: int, b: int) -> (xy: (int, int))
    requires 0 <= a < b < vs.filter(p).len()
    ensures 0 <= xy.0 < xy.1 < vs.len(), vs[xy.0] == vs.filter(p)[a], vs[xy.1] == vs.filter(p)[b]
    decreases vs.len()
{
    reveal(Seq::filter);
    let sub = vs.drop_last().filter(p);
    if p(vs.last()) {
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
        let xy = lemma_filter_order(vs.drop_last(), p, a, b);
        assert(vs.drop_last()[xy.0] == vs[xy.0]);
        assert(vs.drop_last()[xy.1] == vs[xy.1]);
        xy
    }
}
pub proof fn lemma_shapes_step(s: State, c: Constants, a: Action)
    requires inv_shapes(s, c), inv_txns(s, c), enabled(s, c, a), log_shape(apply(s, c, a), c)
    ensures inv_shapes(apply(s, c, a), c)
{
    let z = apply(s, c, a);
    assert(log_shape(z, c));
    assert(valid_constants(c));
    assert(z.shards.len() == c.shards && z.logs.len() == c.shards);
    assert forall|i: int| is_shard(c, i) implies #[trigger] shard_epoch(z.shards, i) <= cm_epoch(z)
        && #[trigger] clock(z.shards, i) >= 0 by {
        assert(clock_floor(entries(z.logs[i])) <= z.shards[i].clock);
    }
    assert forall|k: int| #[trigger] z.versions.dom().contains(k) implies k >= 0 by {
        match a {
            Action::Install { id, shard } => {
                assert(inv_txn(s, c, id));
            },
            _ => {},
        }
    }
}

pub open spec fn same_prepared(r: TxnRec, q: TxnRec) -> bool {
    prepared(q) && r.body == q.body && r.coord == q.coord && r.epoch == q.epoch
        && r.reads == q.reads && r.ts == q.ts && r.pidx == q.pidx
        && r.invoked == q.invoked && r.prepared_at == q.prepared_at
        && r.installed.subset_of(q.installed) && r.published.subset_of(q.published)
        && (r.status is Aborted ==> q.status == r.status)
        && (q.status is Prepared ==> r.status is Prepared)
}
pub proof fn lemma_metadata_step(s: State, c: Constants, a: Action)
    requires occ_inv(s, c), enabled(s, c, a)
    ensures
        s.txns.dom().subset_of(apply(s, c, a).txns.dom()),
        forall|id: int| #[trigger] s.txns.dom().contains(id) && prepared(s.txns[id])
            ==> same_prepared(s.txns[id], apply(s, c, a).txns[id]),
        forall|i: int| is_shard(c, i) ==> s.shards[i].epoch <= #[trigger] apply(s, c, a).shards[i].epoch
            && (apply(s, c, a).shards[i].epoch == s.shards[i].epoch && apply(s, c, a).shards[i].alive ==> s.shards[i].alive)
{
    let z = apply(s, c, a);
    assert forall|id: int| #[trigger] s.txns.dom().contains(id) && prepared(s.txns[id])
        implies same_prepared(s.txns[id], z.txns[id]) by {
        match a {
            Action::Submit { id: n, body, coord } => {},
            Action::Read { id: n, key } => {},
            Action::Prepare { id: n, ts } => {},
            Action::Install { id: n, shard } => {},
            Action::Publish { id: n, shard } => {},
            Action::Certify { id: n } => {},
            Action::Provisional { id: n } => {},
            Action::Final { id: n } => {},
            Action::Abort { id: n } => {},
            Action::Crash { shard, survive } => {},
            Action::ObserveEpoch { shard } => {},
            _ => {},
        }
    }
}

pub proof fn lemma_versions_step(s: State, c: Constants, a: Action)
    requires occ_inv(s, c), enabled(s, c, a)
    ensures inv_versions(apply(s, c, a), c)
{
    let z = apply(s, c, a);
    lemma_metadata_step(s, c, a);
    assert forall|k: int| #[trigger] z.versions.dom().contains(k) implies inv_versions_of(z, c, k) by {
        assert forall|j: int| 0 <= j < z.versions[k].len()
            implies inv_version(z, c, k, #[trigger] z.versions[k][j]) by {
            match a {
                Action::Install { id, shard } => {
                    if write_set(s.txns[id].body).contains(k) && owner(c, k) == shard {
                        if j < vers(s.versions, k).len() {
                            assert(s.versions.dom().contains(k));
                            assert(inv_versions_of(s, c, k));
                            assert(inv_version(s, c, k, s.versions[k][j]));
                        }
                    } else { assert(inv_versions_of(s, c, k)); }
                },
                Action::Crash { shard, survive } => {
                    if owner(c, k) == shard {
                        let kept = s.logs[shard].durable + s.logs[shard].pending.take(survive as int);
                        let p = |v: Version| has_log(kept, v.txn);
                        let x = lemma_filter_index(s.versions[k], p, j);
                        assert(inv_version(s, c, k, s.versions[k][x]));
                    } else { assert(inv_versions_of(s, c, k)); }
                },
                Action::Rollback { shard, epoch } => {
                    if owner(c, k) == shard {
                        let p = |v: Version| v.epoch != epoch || below_view(s.views, c, shard, epoch, v.ts);
                        let x = lemma_filter_index(s.versions[k], p, j);
                        assert(inv_version(s, c, k, s.versions[k][x]));
                    } else { assert(inv_versions_of(s, c, k)); }
                },
                _ => { assert(inv_versions_of(s, c, k)); },
            }
        }
        assert forall|x: int, y: int| 0 <= x < y < z.versions[k].len()
            implies pidx(z.txns, #[trigger] z.versions[k][x].txn) < pidx(z.txns, #[trigger] z.versions[k][y].txn) by {
            match a {
                Action::Install { id, shard } => {
                    if write_set(s.txns[id].body).contains(k) && owner(c, k) == shard {
                        lemma_writes_at_owner(c, s.txns[id].body, k);
                        assert(s.locks.dom().contains(k) && s.locks[k] == id);
                        assert(inv_lock(s, c, k));
                        assert(s.versions.dom().contains(k));
                        assert(inv_versions_of(s, c, k));
                        assert(inv_version(s, c, k, s.versions[k][x]));
                        if y < s.versions[k].len() { assert(inv_version(s, c, k, s.versions[k][y])); }
                    } else { assert(inv_versions_of(s, c, k)); }
                },
                Action::Crash { shard, survive } => {
                    if owner(c, k) == shard {
                        let kept = s.logs[shard].durable + s.logs[shard].pending.take(survive as int);
                        let p = |v: Version| has_log(kept, v.txn);
                        let xy = lemma_filter_order(s.versions[k], p, x, y);
                        assert(inv_versions_of(s, c, k));
                        assert(inv_version(s, c, k, s.versions[k][xy.0]));
                        assert(inv_version(s, c, k, s.versions[k][xy.1]));
                        assert(pidx(s.txns, s.versions[k][xy.0].txn) < pidx(s.txns, s.versions[k][xy.1].txn));
                    } else { assert(inv_versions_of(s, c, k)); }
                },
                Action::Rollback { shard, epoch } => {
                    if owner(c, k) == shard {
                        let p = |v: Version| v.epoch != epoch || below_view(s.views, c, shard, epoch, v.ts);
                        let xy = lemma_filter_order(s.versions[k], p, x, y);
                        assert(inv_versions_of(s, c, k));
                        assert(inv_version(s, c, k, s.versions[k][xy.0]));
                        assert(inv_version(s, c, k, s.versions[k][xy.1]));
                        assert(pidx(s.txns, s.versions[k][xy.0].txn) < pidx(s.txns, s.versions[k][xy.1].txn));
                    } else { assert(inv_versions_of(s, c, k)); }
                },
                _ => {
                    assert(inv_versions_of(s, c, k));
                    assert(inv_version(s, c, k, s.versions[k][x]));
                    assert(inv_version(s, c, k, s.versions[k][y]));
                },
            }
        }
    }
}

pub proof fn lemma_locks_step(s: State, c: Constants, a: Action)
    requires occ_inv(s, c), enabled(s, c, a)
    ensures inv_locks(apply(s, c, a), c)
{
    let z = apply(s, c, a);
    lemma_metadata_step(s, c, a);
    assert forall|k: int| #[trigger] z.locks.dom().contains(k) implies inv_lock(z, c, k) by {
        let h = z.locks[k];
        if s.locks.dom().contains(k) { assert(inv_lock(s, c, k)); }
        assert forall|j: int| 0 <= j < vers(z.versions, k).len()
            implies pidx(z.txns, #[trigger] vers(z.versions, k)[j].txn) < z.txns[h].pidx by {
            match a {
                Action::Prepare { id, ts } => {
                    assert(s.versions.dom().contains(k));
                    assert(inv_versions_of(s, c, k));
                    let v = s.versions[k][j];
                    assert(inv_version(s, c, k, v));
                    assert(inv_txn(s, c, v.txn));
                },
                Action::Install { id, shard } => {
                    assert(!(write_set(s.txns[id].body).contains(k) && owner(c, k) == shard));
                    assert(s.versions.dom().contains(k));
                    assert(inv_versions_of(s, c, k));
                    assert(inv_version(s, c, k, s.versions[k][j]));
                },
                Action::Crash { shard, survive } => {
                    assert(owner(c, k) != shard);
                    assert(s.versions.dom().contains(k));
                    assert(inv_versions_of(s, c, k));
                    assert(inv_version(s, c, k, s.versions[k][j]));
                },
                Action::Rollback { shard, epoch } => {
                    if owner(c, k) == shard {
                        let p = |v: Version| v.epoch != epoch || below_view(s.views, c, shard, epoch, v.ts);
                        assert(s.versions.dom().contains(k));
                        let x = lemma_filter_index(s.versions[k], p, j);
                        assert(inv_version(s, c, k, s.versions[k][x]));
                    } else {
                        assert(s.versions.dom().contains(k));
                        assert(inv_versions_of(s, c, k));
                        assert(inv_version(s, c, k, s.versions[k][j]));
                    }
                },
                _ => {
                    assert(s.versions.dom().contains(k));
                    assert(inv_versions_of(s, c, k));
                    assert(inv_version(s, c, k, s.versions[k][j]));
                },
            }
        }
        match a {
            Action::Prepare { id, ts } => {},
            Action::Certify { id } => {
                if h == id {
                    lemma_writes_at_owner(c, s.txns[h].body, k);
                    assert(s.txns[h].installed.contains(owner(c, k)));
                }
            },
            Action::Crash { shard, survive } => {},
            Action::ObserveEpoch { shard } => {},
            _ => {},
        }
    }
    assert forall|id: int, k: int| #[trigger] z.txns.dom().contains(id) && txn(z.txns, id).status is Prepared
        && #[trigger] write_set(txn(z.txns, id).body).contains(k)
        && !txn(z.txns, id).installed.contains(owner(c, k))
        && z.shards[owner(c, k)].alive && shard_epoch(z.shards, owner(c, k)) == txn(z.txns, id).epoch
        implies z.locks.dom().contains(k) && z.locks[k] == id by {
        match a {
            Action::Prepare { id: n, ts } => {
                if id != n {
                    assert(s.txns.dom().contains(id));
                    assert(inv_txn(s, c, id));
                    lemma_writes_at_owner(c, s.txns[id].body, k);
                    assert(s.locks.dom().contains(k) && s.locks[k] == id);
                    assert(!write_set(s.txns[n].body).contains(k));
                }
            },
            _ => {
                assert(s.txns.dom().contains(id));
                assert(inv_txn(s, c, id));
                lemma_writes_at_owner(c, s.txns[id].body, k);
                assert(s.txns[id].status is Prepared);
                assert(clock_participant(c, s.txns[id], owner(c, k)));
                assert(s.txns[id].epoch <= s.shards[owner(c, k)].epoch);
                assert(s.shards[owner(c, k)].epoch == z.shards[owner(c, k)].epoch);
                assert(s.shards[owner(c, k)].alive);
                assert(s.locks.dom().contains(k) && s.locks[k] == id);
                match a {
                    Action::Install { id: n, shard } => {
                        if write_set(s.txns[n].body).contains(k) && owner(c, k) == shard {
                            assert(s.locks[k] == n);
                        }
                    },
                    _ => {},
                }
            },
        }
    }
}

pub proof fn lemma_version_origin(s: State, c: Constants, a: Action, k: int, j: int)
    requires enabled(s, c, a), 0 <= j < vers(apply(s, c, a).versions, k).len()
    ensures
        (exists|x: int| 0 <= x < vers(s.versions, k).len()
            && #[trigger] vers(s.versions, k)[x] == vers(apply(s, c, a).versions, k)[j])
        || (a is Install && a->Install_id == vers(apply(s, c, a).versions, k)[j].txn
            && write_set(s.txns[a->Install_id].body).contains(k) && owner(c, k) == a->Install_shard)
{
    let z = apply(s, c, a);
    match a {
        Action::Install { id, shard } => {
            if !(write_set(s.txns[id].body).contains(k) && owner(c, k) == shard)
                || j < vers(s.versions, k).len() {
                assert(vers(s.versions, k)[j] == vers(z.versions, k)[j]);
            }
        },
        Action::Crash { shard, survive } => {
            if owner(c, k) == shard {
                assert(s.versions.dom().contains(k));
                let kept = s.logs[shard].durable + s.logs[shard].pending.take(survive as int);
                let x = lemma_filter_index(s.versions[k], |v: Version| has_log(kept, v.txn), j);
                assert(vers(s.versions, k)[x] == vers(z.versions, k)[j]);
            } else { assert(vers(s.versions, k)[j] == vers(z.versions, k)[j]); }
        },
        Action::Rollback { shard, epoch } => {
            if owner(c, k) == shard {
                assert(s.versions.dom().contains(k));
                let x = lemma_filter_index(s.versions[k],
                    |v: Version| v.epoch != epoch || below_view(s.views, c, shard, epoch, v.ts), j);
                assert(vers(s.versions, k)[x] == vers(z.versions, k)[j]);
            } else { assert(vers(s.versions, k)[j] == vers(z.versions, k)[j]); }
        },
        _ => { assert(vers(s.versions, k)[j] == vers(z.versions, k)[j]); },
    }
}

pub proof fn lemma_read_writer_step(s: State, c: Constants, a: Action, id: int, k: int)
    requires inv(s, c), enabled(s, c, a), apply(s, c, a).txns.dom().contains(id),
        apply(s, c, a).txns[id].reads.dom().contains(k)
    ensures
        apply(s, c, a).txns[id].reads[k].writer == -1 ==>
            apply(s, c, a).txns[id].reads[k].value == 0 && apply(s, c, a).txns[id].reads[k].ts == 0,
        apply(s, c, a).txns[id].reads[k].writer != -1 ==> inv_read_writer(apply(s, c, a), c, id, k)
{
    let z = apply(s, c, a);
    lemma_metadata_step(s, c, a);
    if a is Read && a->Read_id == id && a->Read_key == k {
        let vs = vers(s.versions, k);
        if vs.len() > 0 {
            let v = vs.last();
            assert(s.versions.dom().contains(k));
            assert(inv_versions_of(s, c, k));
            assert(inv_version(s, c, k, v));
            assert(inv_txn(s, c, v.txn));
            assert(v.txn != id);
            if v.epoch < s.txns[id].epoch {
                lemma_view_stable(s, c, v.txn, owner(c, k));
            }
        }
    } else {
        assert(s.txns.dom().contains(id) && s.txns[id].reads.dom().contains(k));
        assert(z.txns[id].reads[k] == s.txns[id].reads[k]);
        assert(inv_read(s, c, id, k));
        let w = s.txns[id].reads[k].writer;
        if w != -1 {
            assert(inv_read_writer(s, c, id, k));
            assert(inv_txn(s, c, w));
            if s.txns[w].epoch < s.txns[id].epoch { lemma_stable_step(s, c, a, s.txns[w]); }
            match a {
                Action::Prepare { id: n, ts } => {
                    if id == n { assert(s.txns[w].pidx < s.order.len()); }
                },
                _ => {},
            }
        }
    }
}

pub proof fn lemma_read_order_prepare(s: State, c: Constants, id: int, ts: int, k: int, other: int)
    requires inv(s, c), can_prepare(s, c, id, ts), s.txns[id].reads.dom().contains(k),
        s.txns.dom().contains(other), prepared(s.txns[other]), write_set(s.txns[other].body).contains(k)
    ensures inv_read_order(apply(s, c, Action::Prepare { id, ts }), c, id, k, other)
{
    let z = apply(s, c, Action::Prepare { id, ts });
    let r = s.txns[id];
    let w = s.txns[other];
    assert(inv_txn(s, c, id));
    assert(inv_txn(s, c, other));
    assert(read_set(r.body).contains(k));
    lemma_writes_at_owner(c, w.body, k);
    assert(clock_participant(c, w, owner(c, k)));
    lemma_metadata_step(s, c, Action::Prepare { id, ts });
    if w.installed.contains(owner(c, k)) && has_version(s.versions, k, other) {
        let j = choose|j: int| 0 <= j < vers(s.versions, k).len() && #[trigger] vers(s.versions, k)[j].txn == other;
        assert(s.versions.dom().contains(k));
        assert(inv_versions_of(s, c, k));
        let n = s.versions[k].len() as int - 1;
        assert(top_writer(s.versions, k) == r.reads[k].writer);
        assert(s.versions[k][n].txn == r.reads[k].writer);
        assert(inv_version(s, c, k, s.versions[k][n]));
        if j < n { assert(pidx(s.txns, s.versions[k][j].txn) < pidx(s.txns, s.versions[k][n].txn)); }
    }
    if !w.installed.contains(owner(c, k)) && !(w.status is Aborted) {
        if certified(w) { assert(w.installed.contains(owner(c, k))); }
        assert(w.status is Prepared);
        if s.shards[owner(c, k)].epoch == w.epoch {
            assert(s.locks.dom().contains(k) && s.locks[k] == other);
            assert(r.body.ops.dom().contains(k));
            assert(!s.locks.dom().contains(k));
        }
    }
}
pub proof fn lemma_read_order_step(s: State, c: Constants, a: Action, id: int, k: int, other: int)
    requires inv(s, c), enabled(s, c, a),
        apply(s, c, a).txns.dom().contains(id), prepared(apply(s, c, a).txns[id]),
        apply(s, c, a).txns[id].reads.dom().contains(k),
        apply(s, c, a).txns.dom().contains(other), prepared(apply(s, c, a).txns[other]),
        apply(s, c, a).txns[other].pidx < apply(s, c, a).txns[id].pidx,
        write_set(apply(s, c, a).txns[other].body).contains(k)
    ensures inv_read_order(apply(s, c, a), c, id, k, other)
{
    let z = apply(s, c, a);
    lemma_metadata_step(s, c, a);
    if a is Prepare && a->Prepare_id == id {
        assert(s.txns.dom().contains(other) && prepared(s.txns[other]));
        lemma_read_order_prepare(s, c, id, a->Prepare_ts, k, other);
    } else {
        assert(s.txns.dom().contains(id) && prepared(s.txns[id]));
        assert(inv_txn(s, c, id));
        assert(s.txns.dom().contains(other));
        if a is Prepare && a->Prepare_id == other {
            assert(z.txns[other].pidx == s.order.len());
            assert(s.txns[id].pidx < s.order.len());
        }
        assert(prepared(s.txns[other]));
        assert(inv_txn(s, c, other));
        assert(inv_read(s, c, id, k));
        assert(inv_read_order(s, c, id, k, other));
        lemma_writes_at_owner(c, s.txns[other].body, k);
        assert(clock_participant(c, s.txns[other], owner(c, k)));
        if has_version(z.versions, k, other) {
            let j = choose|j: int| 0 <= j < vers(z.versions, k).len() && #[trigger] vers(z.versions, k)[j].txn == other;
            lemma_version_origin(s, c, a, k, j);
            if a is Install && a->Install_id == other && owner(c, k) == a->Install_shard {
                assert(!s.txns[other].installed.contains(owner(c, k)));
                assert(s.txns[other].status is Prepared);
                assert(s.shards[owner(c, k)].alive && s.shards[owner(c, k)].epoch == s.txns[other].epoch);
                assert(false);
            }
            let x = choose|x: int| 0 <= x < vers(s.versions, k).len()
                && #[trigger] vers(s.versions, k)[x] == vers(z.versions, k)[j];
            assert(has_version(s.versions, k, other));
            assert(s.versions.dom().contains(k));
            assert(inv_version(s, c, k, s.versions[k][x]));
            if s.txns[id].reads[k].writer != -1 {
                assert(inv_read_writer(s, c, id, k));
                assert(s.txns.dom().contains(s.txns[id].reads[k].writer));
            }
        }
        if !z.txns[other].installed.contains(owner(c, k)) {
            assert(!s.txns[other].installed.contains(owner(c, k)));
            if z.shards[owner(c, k)].epoch == s.txns[other].epoch && z.shards[owner(c, k)].alive {
                assert(s.txns[other].epoch <= s.shards[owner(c, k)].epoch);
                assert(s.shards[owner(c, k)].epoch == z.shards[owner(c, k)].epoch);
                assert(s.shards[owner(c, k)].alive);
                assert(s.txns[other].status is Aborted);
            }
        }
    }
}
pub proof fn lemma_reads_step(s: State, c: Constants, a: Action)
    requires inv(s, c), enabled(s, c, a)
    ensures inv_reads(apply(s, c, a), c)
{
    let z = apply(s, c, a);
    assert forall|id: int, k: int| #[trigger] z.txns.dom().contains(id) && #[trigger] txn(z.txns, id).reads.dom().contains(k)
        implies inv_read(z, c, id, k) by {
        lemma_read_writer_step(s, c, a, id, k);
        if prepared(z.txns[id]) {
            assert forall|o: int| #[trigger] z.txns.dom().contains(o) && prepared(txn(z.txns, o))
                && txn(z.txns, o).pidx < z.txns[id].pidx && write_set(txn(z.txns, o).body).contains(k)
                implies inv_read_order(z, c, id, k, o) by { lemma_read_order_step(s, c, a, id, k, o); }
        }
    }
}

pub proof fn lemma_final_step(s: State, c: Constants, a: Action)
    requires inv(s, c), enabled(s, c, a)
    ensures inv_final(apply(s, c, a), c)
{
    let z = apply(s, c, a);
    lemma_metadata_step(s, c, a);
    assert forall|id: int| #[trigger] z.txns.dom().contains(id) && txn(z.txns, id).status is Final
        implies stable(z.logs, c, txn(z.txns, id)) by {
        if a is Final && a->Final_id == id { lemma_view_stable(s, c, id, s.txns[id].coord); }
        else { assert(s.txns.dom().contains(id) && s.txns[id].status is Final); }
        lemma_stable_step(s, c, a, s.txns[id]);
    }
}
pub proof fn lemma_rolled_step(s: State, c: Constants, a: Action)
    requires inv(s, c), enabled(s, c, a)
    ensures inv_rolled_back(apply(s, c, a), c)
{
    let z = apply(s, c, a);
    lemma_metadata_step(s, c, a);
    lemma_versions_step(s, c, a);
    assert forall|i: int, e: nat| #[trigger] z.rolled_back.contains((i, e)) implies is_shard(c, i)
        && e < z.shards[i].epoch
        && (forall|j: int| is_shard(c, j) ==> has_close(#[trigger] z.logs[j].durable, e))
        && (forall|k: int, j: int| #[trigger] z.versions.dom().contains(k) && owner(c, k) == i
            && 0 <= j < z.versions[k].len() && #[trigger] z.versions[k][j].epoch == e
            ==> stable(z.logs, c, txn(z.txns, z.versions[k][j].txn))) by {
        if a is Rollback && a->Rollback_shard == i && a->Rollback_epoch == e {
            assert(final_ready(s.views, c, i, e));
            assert forall|h: int| is_shard(c, h) implies has_close(#[trigger] z.logs[h].durable, e) by {
                lemma_view_sound(s, c, i, e, h);
            }
            lemma_view_sound(s, c, i, e, i);
            lemma_closed_exact(s, c, i, e);
            assert forall|k: int, j: int| #[trigger] z.versions.dom().contains(k) && owner(c, k) == i
                && 0 <= j < z.versions[k].len() && #[trigger] z.versions[k][j].epoch == e
                implies stable(z.logs, c, txn(z.txns, z.versions[k][j].txn)) by {
                let p = |v: Version| v.epoch != e || below_view(s.views, c, i, e, v.ts);
                let x = lemma_filter_index(s.versions[k], p, j);
                let v = s.versions[k][x];
                assert(inv_version(s, c, k, v));
                lemma_view_stable(s, c, v.txn, i);
            }
        } else {
            assert(s.rolled_back.contains((i, e)));
            assert forall|h: int| is_shard(c, h) implies has_close(#[trigger] z.logs[h].durable, e) by {
                lemma_closed_cut_step(s, c, a, h, e);
            }
            assert forall|k: int, j: int| #[trigger] z.versions.dom().contains(k) && owner(c, k) == i
                && 0 <= j < z.versions[k].len() && #[trigger] z.versions[k][j].epoch == e
                implies stable(z.logs, c, txn(z.txns, z.versions[k][j].txn)) by {
                lemma_version_origin(s, c, a, k, j);
                let v = z.versions[k][j];
                assert(inv_version(z, c, k, v));
                if a is Install && a->Install_id == v.txn && a->Install_shard == i {
                    assert(s.txns[v.txn].epoch == s.shards[i].epoch);
                    assert(v.epoch == s.txns[v.txn].epoch);
                    assert(false);
                }
                let x = choose|x: int| 0 <= x < vers(s.versions, k).len()
                    && #[trigger] vers(s.versions, k)[x] == vers(z.versions, k)[j];
                assert(s.versions.dom().contains(k));
                assert(inv_version(s, c, k, s.versions[k][x]));
                assert(stable(s.logs, c, txn(s.txns, s.versions[k][x].txn)));
                lemma_stable_step(s, c, a, s.txns[v.txn]);
            }
        }
    }
}

pub proof fn lemma_missing_version_step(s: State, c: Constants, a: Action, id: int, k: int)
    requires inv(s, c), enabled(s, c, a), apply(s, c, a).txns.dom().contains(id),
        apply(s, c, a).txns[id].status is Prepared || certified(apply(s, c, a).txns[id]),
        write_set(apply(s, c, a).txns[id].body).contains(k),
        apply(s, c, a).txns[id].installed.contains(owner(c, k)),
        !has_version(apply(s, c, a).versions, k, id)
    ensures ({ let z = apply(s, c, a); let r = z.txns[id];
        (doomed(z, c, r) && z.rolled_back.contains((owner(c, k), r.epoch))) || version_lost(z, c, r, id, k) })
{
    let z = apply(s, c, a);
    lemma_metadata_step(s, c, a);
    assert(s.txns.dom().contains(id));
    assert(inv_txn(s, c, id));
    if a is Install && a->Install_id == id && a->Install_shard == owner(c, k) {
        let n = vers(s.versions, k).len() as int;
        assert(vers(z.versions, k)[n].txn == id);
        assert(has_version(z.versions, k, id));
    }
    assert(prepared(s.txns[id]));
    assert(s.txns[id].status is Prepared || certified(s.txns[id]));
    assert(s.txns[id].installed.contains(owner(c, k)));
    let r = s.txns[id];
    lemma_writes_at_owner(c, r.body, k);
    if !has_version(s.versions, k, id) {
        assert(inv_present(s, c, id));
        if doomed(s, c, r) && s.rolled_back.contains((owner(c, k), r.epoch)) {
            lemma_doomed_step(s, c, a, r);
            assert(z.rolled_back.contains((owner(c, k), r.epoch)));
        } else { lemma_version_lost_step(s, c, a, id, k); }
    } else {
        let j = choose|j: int| 0 <= j < vers(s.versions, k).len() && #[trigger] vers(s.versions, k)[j].txn == id;
        assert(s.versions.dom().contains(k));
        let v = s.versions[k][j];
        assert(inv_versions_of(s, c, k));
        assert(inv_version(s, c, k, v));
        match a {
            Action::Crash { shard, survive } => {
                if owner(c, k) == shard {
                    let kept = s.logs[shard].durable + s.logs[shard].pending.take(survive as int);
                    let p = |v: Version| has_log(kept, v.txn);
                    if has_log(kept, id) {
                        s.versions[k].lemma_filter_contains(p, j);
                        let x = choose|x: int| 0 <= x < s.versions[k].filter(p).len()
                            && s.versions[k].filter(p)[x] == v;
                        assert(vers(z.versions, k)[x].txn == id);
                    }
                    assert(entries(z.logs[shard]) =~= kept);
                    assert(version_lost(z, c, z.txns[id], id, k));
                } else { assert(vers(z.versions, k)[j].txn == id); }
            },
            Action::Rollback { shard, epoch } => {
                if owner(c, k) == shard {
                    let p = |v: Version| v.epoch != epoch || below_view(s.views, c, shard, epoch, v.ts);
                    if p(v) {
                        s.versions[k].lemma_filter_contains(p, j);
                        let x = choose|x: int| 0 <= x < s.versions[k].filter(p).len()
                            && s.versions[k].filter(p)[x] == v;
                        assert(vers(z.versions, k)[x].txn == id);
                    }
                    assert(r.epoch == epoch);
                    lemma_final_view_equivalence(s, c, id, shard);
                    assert forall|i: int| is_shard(c, i) implies has_close(#[trigger] s.logs[i].durable, epoch) by {
                        lemma_view_sound(s, c, shard, epoch, i);
                    }
                    assert(doomed(s, c, r));
                } else { assert(vers(z.versions, k)[j].txn == id); }
            },
            _ => { assert(vers(z.versions, k)[j].txn == id); },
        }
    }
}
pub proof fn lemma_present_step(s: State, c: Constants, a: Action)
    requires inv(s, c), enabled(s, c, a)
    ensures inv_all_present(apply(s, c, a), c)
{
    let z = apply(s, c, a);
    assert forall|id: int| #[trigger] z.txns.dom().contains(id) implies inv_present(z, c, id) by {
        let r = z.txns[id];
        if r.status is Prepared || certified(r) {
            assert forall|k: int| #[trigger] write_set(r.body).contains(k)
                && r.installed.contains(owner(c, k)) && !has_version(z.versions, k, id) implies
                (doomed(z, c, r) && z.rolled_back.contains((owner(c, k), r.epoch))) || version_lost(z, c, r, id, k) by {
                lemma_missing_version_step(s, c, a, id, k);
            }
        }
    }
}
pub proof fn lemma_occ_step(s: State, c: Constants, a: Action)
    requires inv(s, c), enabled(s, c, a), log_inv(apply(s, c, a), c)
    ensures occ_inv(apply(s, c, a), c)
{
    lemma_shapes_step(s, c, a);
    lemma_txns_step(s, c, a);
    lemma_locks_step(s, c, a);
    lemma_versions_step(s, c, a);
    lemma_reads_step(s, c, a);
    lemma_present_step(s, c, a);
    lemma_final_step(s, c, a);
    lemma_rolled_step(s, c, a);
}

} // verus!
