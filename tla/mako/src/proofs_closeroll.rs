//! Preservation proofs: CloseEpoch and Rollback.
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
// Seq::filter facts
// ---------------------------------------------------------------------------

/// The position in `s` of the `j`-th element of `s.filter(p)`.
pub open spec fn filter_idx<A>(s: Seq<A>, p: spec_fn(A) -> bool, j: int) -> int
    decreases s.len()
{
    if s.len() == 0 { 0 }
    else if p(s.last()) && j == s.drop_last().filter(p).len() { s.len() - 1 }
    else { filter_idx(s.drop_last(), p, j) }
}

/// The `j`-th element of a filtered sequence sits at `filter_idx` in the original.
pub proof fn lemma_filter_idx<A>(s: Seq<A>, p: spec_fn(A) -> bool, j: int)
    requires 0 <= j < s.filter(p).len()
    ensures 0 <= filter_idx(s, p, j) < s.len(), s[filter_idx(s, p, j)] == s.filter(p)[j]
    decreases s.len()
{
    reveal(Seq::filter);
    if s.len() == 0 {
        assert(s.filter(p) == s);
    } else {
        let d = s.drop_last();
        let sub = d.filter(p);
        if p(s.last()) && j == sub.len() {
            assert(s.filter(p) == sub.push(s.last()));
            assert(s.filter(p)[j] == s.last());
        } else {
            assert(j < sub.len());
            assert(s.filter(p)[j] == sub[j]);
            lemma_filter_idx(d, p, j);
            assert(s[filter_idx(d, p, j)] == d[filter_idx(d, p, j)]);
        }
    }
}

/// Filtering preserves relative order.
pub proof fn lemma_filter_idx_order<A>(s: Seq<A>, p: spec_fn(A) -> bool, a: int, b: int)
    requires 0 <= a < b < s.filter(p).len()
    ensures filter_idx(s, p, a) < filter_idx(s, p, b)
    decreases s.len()
{
    reveal(Seq::filter);
    if s.len() == 0 {
        assert(s.filter(p) == s);
    } else {
        let d = s.drop_last();
        let sub = d.filter(p);
        if p(s.last()) && b == sub.len() {
            assert(a < sub.len());
            lemma_filter_idx(d, p, a);
        } else {
            assert(b < sub.len());
            lemma_filter_idx_order(d, p, a, b);
        }
    }
}

// ---------------------------------------------------------------------------
// Finalized watermarks: inserting a fresh key leaves complete epochs alone
// ---------------------------------------------------------------------------

pub proof fn lemma_fw_insert_fresh(fw: Map<(int, nat), Wm>, c: Constants, i: int, e: nat, w: Wm, e2: nat)
    requires is_shard(c, i), !fw.dom().contains((i, e)), fvw_ready(fw, c, e2)
    ensures
        e2 != e,
        fvw_ready(fw.insert((i, e), w), c, e2),
        forall|j: int| is_shard(c, j) ==> #[trigger] fw.insert((i, e), w)[(j, e2)] == fw[(j, e2)],
        forall|vc: Seq<int>| #[trigger] below_fvw(fw.insert((i, e), w), c, vc, e2) == below_fvw(fw, c, vc, e2),
{
    let fw2 = fw.insert((i, e), w);
    assert(fw.dom().contains((i, e2)));
    assert forall|j: int| is_shard(c, j) implies #[trigger] fw2[(j, e2)] == fw[(j, e2)] && fw2.dom().contains((j, e2)) by {
        assert(fw.dom().contains((j, e2)));
        assert((j, e2) != (i, e));
    }
    assert forall|vc: Seq<int>| #[trigger] below_fvw(fw2, c, vc, e2) == below_fvw(fw, c, vc, e2) by {
        if below_fvw(fw, c, vc, e2) {
            assert forall|j: int| is_shard(c, j) implies wm_le(vc[group(c, j)], #[trigger] fw2[(j, e2)]) by {
                assert(wm_le(vc[group(c, j)], fw[(j, e2)]));
            }
        }
        if below_fvw(fw2, c, vc, e2) {
            assert forall|j: int| is_shard(c, j) implies wm_le(vc[group(c, j)], #[trigger] fw[(j, e2)]) by {
                assert(wm_le(vc[group(c, j)], fw2[(j, e2)]));
            }
        }
    }
}

// ---------------------------------------------------------------------------
// CloseEpoch
// ---------------------------------------------------------------------------

pub proof fn lemma_close_state(s: State, c: Constants, i: int, e: nat, w: Wm)
    ensures apply(s, c, Action::CloseEpoch { shard: i, epoch: e, wm: w })
        == (State { final_wm: s.final_wm.insert((i, e), w), tick: s.tick + 1, ..s })
{
}

pub proof fn lemma_close_shapes(s: State, c: Constants, i: int, e: nat, w: Wm)
    requires inv(s, c), can_close(s, c, i, e, w)
    ensures inv_shapes(apply(s, c, Action::CloseEpoch { shard: i, epoch: e, wm: w }), c)
{
    lemma_close_state(s, c, i, e, w);
}

pub proof fn lemma_close_txns(s: State, c: Constants, i: int, e: nat, w: Wm)
    requires inv(s, c), can_close(s, c, i, e, w)
    ensures inv_txns(apply(s, c, Action::CloseEpoch { shard: i, epoch: e, wm: w }), c)
{
    let s2 = apply(s, c, Action::CloseEpoch { shard: i, epoch: e, wm: w });
    lemma_close_state(s, c, i, e, w);
    frame_txns_tick(s, s2, c);
}

pub proof fn lemma_close_prepared(s: State, c: Constants, i: int, e: nat, w: Wm)
    requires inv(s, c), can_close(s, c, i, e, w)
    ensures inv_prepared(apply(s, c, Action::CloseEpoch { shard: i, epoch: e, wm: w }), c)
{
    lemma_close_state(s, c, i, e, w);
}

pub proof fn lemma_close_exclusive(s: State, c: Constants, i: int, e: nat, w: Wm)
    requires inv(s, c), can_close(s, c, i, e, w)
    ensures inv_exclusive(apply(s, c, Action::CloseEpoch { shard: i, epoch: e, wm: w }), c)
{
    lemma_close_state(s, c, i, e, w);
}

pub proof fn lemma_close_locks(s: State, c: Constants, i: int, e: nat, w: Wm)
    requires inv(s, c), can_close(s, c, i, e, w)
    ensures inv_locks(apply(s, c, Action::CloseEpoch { shard: i, epoch: e, wm: w }), c)
{
    let s2 = apply(s, c, Action::CloseEpoch { shard: i, epoch: e, wm: w });
    lemma_close_state(s, c, i, e, w);
    assert forall|k: int| #[trigger] s2.locks.dom().contains(k) implies inv_lock(s2, c, k) by {
        assert(inv_lock(s, c, k));
    }
}

pub proof fn lemma_close_versions(s: State, c: Constants, i: int, e: nat, w: Wm)
    requires inv(s, c), can_close(s, c, i, e, w)
    ensures inv_versions(apply(s, c, Action::CloseEpoch { shard: i, epoch: e, wm: w }), c)
{
    let s2 = apply(s, c, Action::CloseEpoch { shard: i, epoch: e, wm: w });
    lemma_close_state(s, c, i, e, w);
    assert forall|k: int| #[trigger] s2.versions.dom().contains(k) implies inv_versions_of(s2, c, k) by {
        assert(inv_versions_of(s, c, k));
        assert forall|j: int| 0 <= j < s2.versions[k].len() implies inv_version(s2, c, k, #[trigger] s2.versions[k][j]) by {
            assert(inv_version(s, c, k, s.versions[k][j]));
        }
    }
}

pub proof fn lemma_close_reads(s: State, c: Constants, i: int, e: nat, w: Wm)
    requires inv(s, c), can_close(s, c, i, e, w)
    ensures inv_reads(apply(s, c, Action::CloseEpoch { shard: i, epoch: e, wm: w }), c)
{
    let s2 = apply(s, c, Action::CloseEpoch { shard: i, epoch: e, wm: w });
    lemma_close_state(s, c, i, e, w);
    assert forall|id: int, k: int| #[trigger] s2.txns.dom().contains(id) && #[trigger] txn(s2.txns, id).reads.dom().contains(k)
        implies inv_read(s2, c, id, k) by {
        assert(inv_read(s, c, id, k));
        let r = txn(s.txns, id);
        let rd = r.reads[k];
        if rd.writer != -1 {
            assert(inv_read_writer(s, c, id, k));
            let wr = txn(s.txns, rd.writer);
            if wr.epoch < r.epoch {
                lemma_fw_insert_fresh(s.final_wm, c, i, e, w, wr.epoch);
                assert(below_fvw(s2.final_wm, c, wr.vc, wr.epoch) == below_fvw(s.final_wm, c, wr.vc, wr.epoch));
            }
            assert(inv_read_writer(s2, c, id, k));
        }
        if is_prepared_or_later(r) {
            assert forall|o: int| #[trigger] s2.txns.dom().contains(o) && o != id
                && is_prepared_or_later(txn(s2.txns, o)) && txn(s2.txns, o).pidx < r.pidx
                && write_set(txn(s2.txns, o).body).contains(k) implies inv_read_order(s2, c, id, k, o) by {
                assert(inv_read_order(s, c, id, k, o));
            }
        }
    }
}

pub proof fn lemma_close_streams(s: State, c: Constants, i: int, e: nat, w: Wm)
    requires inv(s, c), can_close(s, c, i, e, w)
    ensures inv_streams(apply(s, c, Action::CloseEpoch { shard: i, epoch: e, wm: w }), c)
{
    let s2 = apply(s, c, Action::CloseEpoch { shard: i, epoch: e, wm: w });
    lemma_close_state(s, c, i, e, w);
    frame_streams(s, s2, c);
}

pub proof fn lemma_close_all_logs(s: State, c: Constants, i: int, e: nat, w: Wm)
    requires inv(s, c), can_close(s, c, i, e, w)
    ensures inv_all_logs(apply(s, c, Action::CloseEpoch { shard: i, epoch: e, wm: w }), c)
{
    let s2 = apply(s, c, Action::CloseEpoch { shard: i, epoch: e, wm: w });
    lemma_close_state(s, c, i, e, w);
    assert forall|id: int| #[trigger] s2.txns.dom().contains(id) implies inv_logs(s2, c, id) by {
        assert(inv_logs(s, c, id));
        let r = txn(s.txns, id);
        assert forall|k: int| (r.status is Prepared || certified_or_committed(r))
            && #[trigger] write_set(r.body).contains(k) && r.installed.contains(owner(c, k))
            && !has_version(s2.versions, k, id) implies
            (doomed(s2.final_wm, c, r) && s2.rolled_back.contains((owner(c, k), r.epoch))) || lost(s2.streams, c, r, id) by {
            if doomed(s.final_wm, c, r) {
                lemma_fw_insert_fresh(s.final_wm, c, i, e, w, r.epoch);
                assert(below_fvw(s2.final_wm, c, r.vc, r.epoch) == below_fvw(s.final_wm, c, r.vc, r.epoch));
            }
        }
        assert(inv_present(s2, c, r, id));
    }
}

pub proof fn lemma_close_committed(s: State, c: Constants, i: int, e: nat, w: Wm)
    requires inv(s, c), can_close(s, c, i, e, w)
    ensures inv_committed(apply(s, c, Action::CloseEpoch { shard: i, epoch: e, wm: w }), c)
{
    lemma_close_state(s, c, i, e, w);
}

pub proof fn lemma_close_final(s: State, c: Constants, i: int, e: nat, w: Wm)
    requires inv(s, c), can_close(s, c, i, e, w)
    ensures inv_final(apply(s, c, Action::CloseEpoch { shard: i, epoch: e, wm: w }), c)
{
    let s2 = apply(s, c, Action::CloseEpoch { shard: i, epoch: e, wm: w });
    lemma_close_state(s, c, i, e, w);
    assert forall|i2: int, e2: nat| #[trigger] s2.final_wm.dom().contains((i2, e2)) implies inv_final_of(s2, c, i2, e2) by {
        if i2 == i && e2 == e {
            assert(s2.final_wm[(i, e)] == w);
            assert forall|sid: Sid| valid_sid(c, sid) && sid.shard == i implies
                no_pending_epoch(#[trigger] s2.streams[sid], e)
                && wm_le_wm(s2.final_wm[(i, e)], stream_wm(s2.streams[sid].durable, e)) by {
                assert(no_pending_epoch(s.streams[sid], e) && wm_le_wm(w, stream_wm(s.streams[sid].durable, e)));
            }
            let sid = choose|sid: Sid| valid_sid(c, sid) && sid.shard == i && w == stream_wm(#[trigger] s.streams[sid].durable, e);
            assert(s2.final_wm[(i, e)] == stream_wm(s2.streams[sid].durable, e));
        } else {
            assert((i2, e2) != (i, e));
            assert(s.final_wm.dom().contains((i2, e2)));
            assert(inv_final_of(s, c, i2, e2));
            assert(s2.final_wm[(i2, e2)] == s.final_wm[(i2, e2)]);
        }
    }
}

pub proof fn lemma_close_rolled_back(s: State, c: Constants, i: int, e: nat, w: Wm)
    requires inv(s, c), can_close(s, c, i, e, w)
    ensures inv_rolled_back(apply(s, c, Action::CloseEpoch { shard: i, epoch: e, wm: w }), c)
{
    let s2 = apply(s, c, Action::CloseEpoch { shard: i, epoch: e, wm: w });
    lemma_close_state(s, c, i, e, w);
    assert forall|i2: int, e2: nat| #[trigger] s2.rolled_back.contains((i2, e2)) implies inv_rolled_back_of(s2, c, i2, e2) by {
        assert(inv_rolled_back_of(s, c, i2, e2));
        lemma_fw_insert_fresh(s.final_wm, c, i, e, w, e2);
        assert forall|k: int, j: int| #[trigger] s2.versions.dom().contains(k) && owner(c, k) == i2
            && 0 <= j < s2.versions[k].len() && #[trigger] s2.versions[k][j].epoch == e2
            implies below_fvw(s2.final_wm, c, s2.versions[k][j].vc, e2) by {
            assert(below_fvw(s.final_wm, c, s.versions[k][j].vc, e2));
            assert(below_fvw(s2.final_wm, c, s.versions[k][j].vc, e2) == below_fvw(s.final_wm, c, s.versions[k][j].vc, e2));
        }
    }
}

pub proof fn lemma_close_inv(s: State, c: Constants, i: int, e: nat, w: Wm)
    requires inv(s, c), can_close(s, c, i, e, w)
    ensures inv(apply(s, c, Action::CloseEpoch { shard: i, epoch: e, wm: w }), c)
{
    lemma_close_shapes(s, c, i, e, w);
    lemma_close_txns(s, c, i, e, w);
    lemma_close_prepared(s, c, i, e, w);
    lemma_close_exclusive(s, c, i, e, w);
    lemma_close_locks(s, c, i, e, w);
    lemma_close_versions(s, c, i, e, w);
    lemma_close_reads(s, c, i, e, w);
    lemma_close_streams(s, c, i, e, w);
    lemma_close_all_logs(s, c, i, e, w);
    lemma_close_committed(s, c, i, e, w);
    lemma_close_final(s, c, i, e, w);
    lemma_close_rolled_back(s, c, i, e, w);
}

// ---------------------------------------------------------------------------
// Rollback
// ---------------------------------------------------------------------------

/// The filter predicate Rollback applies at the rolled-back shard's keys.
pub open spec fn rb_keep(s: State, c: Constants, e: nat) -> spec_fn(Version) -> bool {
    |v: Version| keep_after_rollback(s, c, e, v)
}

pub proof fn lemma_rb_state(s: State, c: Constants, i: int, e: nat)
    ensures ({
        let s2 = apply(s, c, Action::Rollback { shard: i, epoch: e });
        &&& s2 == (State { versions: s2.versions, rolled_back: s.rolled_back.insert((i, e)), tick: s.tick + 1, ..s })
        &&& s2.versions.dom() == s.versions.dom()
        &&& forall|k: int| owner(c, k) != i ==> #[trigger] vers(s2.versions, k) == vers(s.versions, k)
        &&& forall|k: int| owner(c, k) == i ==> #[trigger] vers(s2.versions, k) == vers(s.versions, k).filter(rb_keep(s, c, e))
    })
{
    let s2 = apply(s, c, Action::Rollback { shard: i, epoch: e });
    assert(s2.versions.dom() =~= s.versions.dom());
    assert forall|k: int| owner(c, k) == i implies #[trigger] vers(s2.versions, k) == vers(s.versions, k).filter(rb_keep(s, c, e)) by {
        if s.versions.dom().contains(k) {
            assert(s2.versions[k] == s.versions[k].filter(|v: Version| keep_after_rollback(s, c, e, v)));
        } else {
            reveal(Seq::filter);
            assert(vers(s.versions, k).len() == 0);
            assert(vers(s.versions, k).filter(rb_keep(s, c, e)) == vers(s.versions, k));
        }
    }
}


/// An element of the post-Rollback versions of `k` is an old version of `k`
/// (at a filtered index), and at the rolled-back shard it passed the filter.
pub proof fn lemma_rb_sub(s: State, c: Constants, i: int, e: nat, k: int, j: int)
    requires 0 <= j < vers(apply(s, c, Action::Rollback { shard: i, epoch: e }).versions, k).len()
    ensures ({
        let nv = vers(apply(s, c, Action::Rollback { shard: i, epoch: e }).versions, k);
        let ov = vers(s.versions, k);
        let j2 = if owner(c, k) == i { filter_idx(ov, rb_keep(s, c, e), j) } else { j };
        &&& 0 <= j2 < ov.len()
        &&& ov[j2] == nv[j]
        &&& owner(c, k) == i ==> keep_after_rollback(s, c, e, nv[j])
    })
{
    lemma_rb_state(s, c, i, e);
    if owner(c, k) == i {
        let ov = vers(s.versions, k);
        lemma_filter_idx(ov, rb_keep(s, c, e), j);
        ov.lemma_filter_pred(rb_keep(s, c, e), j);
    }
}

/// The post-Rollback versions of `k` keep their relative (certification) order.
pub proof fn lemma_rb_order(s: State, c: Constants, i: int, e: nat, k: int, a: int, b: int)
    requires 0 <= a < b < vers(apply(s, c, Action::Rollback { shard: i, epoch: e }).versions, k).len()
    ensures ({
        let ov = vers(s.versions, k);
        let a2 = if owner(c, k) == i { filter_idx(ov, rb_keep(s, c, e), a) } else { a };
        let b2 = if owner(c, k) == i { filter_idx(ov, rb_keep(s, c, e), b) } else { b };
        a2 < b2
    })
{
    lemma_rb_state(s, c, i, e);
    if owner(c, k) == i {
        lemma_filter_idx_order(vers(s.versions, k), rb_keep(s, c, e), a, b);
    }
}

/// An old version that Rollback keeps is still present.
pub proof fn lemma_rb_keeps(s: State, c: Constants, i: int, e: nat, k: int, j: int)
    requires
        0 <= j < vers(s.versions, k).len(),
        owner(c, k) != i || keep_after_rollback(s, c, e, vers(s.versions, k)[j]),
    ensures
        has_version(apply(s, c, Action::Rollback { shard: i, epoch: e }).versions, k, vers(s.versions, k)[j].txn)
{
    let s2 = apply(s, c, Action::Rollback { shard: i, epoch: e });
    lemma_rb_state(s, c, i, e);
    let ov = vers(s.versions, k);
    let nv = vers(s2.versions, k);
    if owner(c, k) == i {
        ov.lemma_filter_contains(rb_keep(s, c, e), j);
        assert(nv.contains(ov[j]));
        let j2 = choose|j2: int| 0 <= j2 < nv.len() && nv[j2] == ov[j];
        assert(nv[j2].txn == ov[j].txn);
    } else {
        assert(nv[j].txn == ov[j].txn);
    }
}

pub proof fn lemma_rb_has_version(s: State, c: Constants, i: int, e: nat, k: int, o: int)
    requires has_version(apply(s, c, Action::Rollback { shard: i, epoch: e }).versions, k, o)
    ensures has_version(s.versions, k, o)
{
    let s2 = apply(s, c, Action::Rollback { shard: i, epoch: e });
    let nv = vers(s2.versions, k);
    let ov = vers(s.versions, k);
    let j = choose|j: int| 0 <= j < nv.len() && #[trigger] nv[j].txn == o;
    lemma_rb_sub(s, c, i, e, k, j);
    let j2 = if owner(c, k) == i { filter_idx(ov, rb_keep(s, c, e), j) } else { j };
    assert(ov[j2].txn == o);
}

pub proof fn lemma_rollback_shapes(s: State, c: Constants, i: int, e: nat)
    requires inv(s, c), can_rollback(s, c, i, e)
    ensures inv_shapes(apply(s, c, Action::Rollback { shard: i, epoch: e }), c)
{
    lemma_rb_state(s, c, i, e);
}

pub proof fn lemma_rollback_txns(s: State, c: Constants, i: int, e: nat)
    requires inv(s, c), can_rollback(s, c, i, e)
    ensures inv_txns(apply(s, c, Action::Rollback { shard: i, epoch: e }), c)
{
    let s2 = apply(s, c, Action::Rollback { shard: i, epoch: e });
    lemma_rb_state(s, c, i, e);
    frame_txns_tick(s, s2, c);
}

pub proof fn lemma_rollback_prepared(s: State, c: Constants, i: int, e: nat)
    requires inv(s, c), can_rollback(s, c, i, e)
    ensures inv_prepared(apply(s, c, Action::Rollback { shard: i, epoch: e }), c)
{
    lemma_rb_state(s, c, i, e);
}

pub proof fn lemma_rollback_exclusive(s: State, c: Constants, i: int, e: nat)
    requires inv(s, c), can_rollback(s, c, i, e)
    ensures inv_exclusive(apply(s, c, Action::Rollback { shard: i, epoch: e }), c)
{
    lemma_rb_state(s, c, i, e);
}

pub proof fn lemma_rollback_locks(s: State, c: Constants, i: int, e: nat)
    requires inv(s, c), can_rollback(s, c, i, e)
    ensures inv_locks(apply(s, c, Action::Rollback { shard: i, epoch: e }), c)
{
    let s2 = apply(s, c, Action::Rollback { shard: i, epoch: e });
    lemma_rb_state(s, c, i, e);
    assert forall|k: int| #[trigger] s2.locks.dom().contains(k) implies inv_lock(s2, c, k) by {
        assert(inv_lock(s, c, k));
        let h = s.locks[k];
        let ov = vers(s.versions, k);
        let nv = vers(s2.versions, k);
        assert forall|j: int| 0 <= j < nv.len() implies pidx_of(s2.txns, #[trigger] nv[j].txn) < txn(s2.txns, h).pidx by {
            lemma_rb_sub(s, c, i, e, k, j);
            let j2 = if owner(c, k) == i { filter_idx(ov, rb_keep(s, c, e), j) } else { j };
            assert(pidx_of(s.txns, ov[j2].txn) < txn(s.txns, h).pidx);
        }
    }
    assert forall|id: int, k: int| #[trigger] s2.txns.dom().contains(id) && txn(s2.txns, id).status is Prepared
        && #[trigger] write_set(txn(s2.txns, id).body).contains(k)
        && !txn(s2.txns, id).installed.contains(owner(c, k))
        && shard_epoch(s2.shards, owner(c, k)) == txn(s2.txns, id).epoch
        implies s2.locks.dom().contains(k) && s2.locks[k] == id by {
    }
}

pub proof fn lemma_rollback_versions(s: State, c: Constants, i: int, e: nat)
    requires inv(s, c), can_rollback(s, c, i, e)
    ensures inv_versions(apply(s, c, Action::Rollback { shard: i, epoch: e }), c)
{
    let s2 = apply(s, c, Action::Rollback { shard: i, epoch: e });
    lemma_rb_state(s, c, i, e);
    assert forall|k: int| #[trigger] s2.versions.dom().contains(k) implies inv_versions_of(s2, c, k) by {
        assert(s.versions.dom().contains(k));
        assert(inv_versions_of(s, c, k));
        let ov = vers(s.versions, k);
        let nv = vers(s2.versions, k);
        assert(ov == s.versions[k]);
        assert(nv == s2.versions[k]);
        assert forall|j: int| 0 <= j < s2.versions[k].len() implies inv_version(s2, c, k, #[trigger] s2.versions[k][j]) by {
            lemma_rb_sub(s, c, i, e, k, j);
            let j2 = if owner(c, k) == i { filter_idx(ov, rb_keep(s, c, e), j) } else { j };
            assert(inv_version(s, c, k, s.versions[k][j2]));
        }
        assert forall|a: int, b: int| 0 <= a < b < s2.versions[k].len() implies
            txn(s2.txns, #[trigger] s2.versions[k][a].txn).pidx < txn(s2.txns, #[trigger] s2.versions[k][b].txn).pidx by {
            lemma_rb_sub(s, c, i, e, k, a);
            lemma_rb_sub(s, c, i, e, k, b);
            lemma_rb_order(s, c, i, e, k, a, b);
            let a2 = if owner(c, k) == i { filter_idx(ov, rb_keep(s, c, e), a) } else { a };
            let b2 = if owner(c, k) == i { filter_idx(ov, rb_keep(s, c, e), b) } else { b };
            assert(txn(s.txns, s.versions[k][a2].txn).pidx < txn(s.txns, s.versions[k][b2].txn).pidx);
        }
    }
}

pub proof fn lemma_rollback_reads(s: State, c: Constants, i: int, e: nat)
    requires inv(s, c), can_rollback(s, c, i, e)
    ensures inv_reads(apply(s, c, Action::Rollback { shard: i, epoch: e }), c)
{
    let s2 = apply(s, c, Action::Rollback { shard: i, epoch: e });
    lemma_rb_state(s, c, i, e);
    assert forall|id: int, k: int| #[trigger] s2.txns.dom().contains(id) && #[trigger] txn(s2.txns, id).reads.dom().contains(k)
        implies inv_read(s2, c, id, k) by {
        assert(inv_read(s, c, id, k));
        let r = txn(s.txns, id);
        let rd = r.reads[k];
        if rd.writer != -1 {
            assert(inv_read_writer(s, c, id, k));
            assert(inv_read_writer(s2, c, id, k));
        }
        if is_prepared_or_later(r) {
            assert forall|o: int| #[trigger] s2.txns.dom().contains(o) && o != id
                && is_prepared_or_later(txn(s2.txns, o)) && txn(s2.txns, o).pidx < r.pidx
                && write_set(txn(s2.txns, o).body).contains(k) implies inv_read_order(s2, c, id, k, o) by {
                assert(inv_read_order(s, c, id, k, o));
                if has_version(s2.versions, k, o) {
                    lemma_rb_has_version(s, c, i, e, k, o);
                }
            }
        }
    }
}

pub proof fn lemma_rollback_streams(s: State, c: Constants, i: int, e: nat)
    requires inv(s, c), can_rollback(s, c, i, e)
    ensures inv_streams(apply(s, c, Action::Rollback { shard: i, epoch: e }), c)
{
    let s2 = apply(s, c, Action::Rollback { shard: i, epoch: e });
    lemma_rb_state(s, c, i, e);
    frame_streams(s, s2, c);
}

pub proof fn lemma_rollback_all_logs(s: State, c: Constants, i: int, e: nat)
    requires inv(s, c), can_rollback(s, c, i, e)
    ensures inv_all_logs(apply(s, c, Action::Rollback { shard: i, epoch: e }), c)
{
    let s2 = apply(s, c, Action::Rollback { shard: i, epoch: e });
    lemma_rb_state(s, c, i, e);
    assert forall|id: int| #[trigger] s2.txns.dom().contains(id) implies inv_logs(s2, c, id) by {
        assert(inv_logs(s, c, id));
        let r = txn(s.txns, id);
        assert forall|k: int| (r.status is Prepared || certified_or_committed(r))
            && #[trigger] write_set(r.body).contains(k) && r.installed.contains(owner(c, k))
            && !has_version(s2.versions, k, id) implies
            (doomed(s2.final_wm, c, r) && s2.rolled_back.contains((owner(c, k), r.epoch))) || lost(s2.streams, c, r, id) by {
            let ov = vers(s.versions, k);
            if has_version(s.versions, k, id) {
                let j = choose|j: int| 0 <= j < ov.len() && #[trigger] ov[j].txn == id;
                let v = ov[j];
                if owner(c, k) != i || keep_after_rollback(s, c, e, v) {
                    lemma_rb_keeps(s, c, i, e, k, j);
                    assert(false);
                }
                assert(s.versions.dom().contains(k));
                assert(inv_versions_of(s, c, k));
                assert(inv_version(s, c, k, s.versions[k][j]));
                assert(v.epoch == r.epoch && v.vc == r.vc);
                assert(doomed(s.final_wm, c, r));
                assert(s2.rolled_back.contains((owner(c, k), r.epoch)));
            }
        }
        assert(inv_present(s2, c, r, id));
    }
}

pub proof fn lemma_rollback_committed(s: State, c: Constants, i: int, e: nat)
    requires inv(s, c), can_rollback(s, c, i, e)
    ensures inv_committed(apply(s, c, Action::Rollback { shard: i, epoch: e }), c)
{
    lemma_rb_state(s, c, i, e);
}

pub proof fn lemma_rollback_final(s: State, c: Constants, i: int, e: nat)
    requires inv(s, c), can_rollback(s, c, i, e)
    ensures inv_final(apply(s, c, Action::Rollback { shard: i, epoch: e }), c)
{
    let s2 = apply(s, c, Action::Rollback { shard: i, epoch: e });
    lemma_rb_state(s, c, i, e);
    assert forall|i2: int, e2: nat| #[trigger] s2.final_wm.dom().contains((i2, e2)) implies inv_final_of(s2, c, i2, e2) by {
        assert(inv_final_of(s, c, i2, e2));
    }
}

pub proof fn lemma_rollback_rolled_back(s: State, c: Constants, i: int, e: nat)
    requires inv(s, c), can_rollback(s, c, i, e)
    ensures inv_rolled_back(apply(s, c, Action::Rollback { shard: i, epoch: e }), c)
{
    let s2 = apply(s, c, Action::Rollback { shard: i, epoch: e });
    lemma_rb_state(s, c, i, e);
    assert forall|i2: int, e2: nat| #[trigger] s2.rolled_back.contains((i2, e2)) implies inv_rolled_back_of(s2, c, i2, e2) by {
        if i2 == i && e2 == e {
            assert forall|k: int, j: int| #[trigger] s2.versions.dom().contains(k) && owner(c, k) == i2
                && 0 <= j < s2.versions[k].len() && #[trigger] s2.versions[k][j].epoch == e2
                implies below_fvw(s2.final_wm, c, s2.versions[k][j].vc, e2) by {
                assert(vers(s2.versions, k) == s2.versions[k]);
                lemma_rb_sub(s, c, i, e, k, j);
                assert(keep_after_rollback(s, c, e, s2.versions[k][j]));
            }
        } else {
            assert(s.rolled_back.contains((i2, e2)));
            assert(inv_rolled_back_of(s, c, i2, e2));
            assert forall|k: int, j: int| #[trigger] s2.versions.dom().contains(k) && owner(c, k) == i2
                && 0 <= j < s2.versions[k].len() && #[trigger] s2.versions[k][j].epoch == e2
                implies below_fvw(s2.final_wm, c, s2.versions[k][j].vc, e2) by {
                let ov = vers(s.versions, k);
                assert(s.versions.dom().contains(k));
                assert(ov == s.versions[k]);
                assert(vers(s2.versions, k) == s2.versions[k]);
                lemma_rb_sub(s, c, i, e, k, j);
                let j2 = if owner(c, k) == i { filter_idx(ov, rb_keep(s, c, e), j) } else { j };
                assert(s.versions[k][j2].epoch == e2);
                assert(below_fvw(s.final_wm, c, s.versions[k][j2].vc, e2));
            }
        }
    }
}

pub proof fn lemma_rollback_inv(s: State, c: Constants, i: int, e: nat)
    requires inv(s, c), can_rollback(s, c, i, e)
    ensures inv(apply(s, c, Action::Rollback { shard: i, epoch: e }), c)
{
    lemma_rollback_shapes(s, c, i, e);
    lemma_rollback_txns(s, c, i, e);
    lemma_rollback_prepared(s, c, i, e);
    lemma_rollback_exclusive(s, c, i, e);
    lemma_rollback_locks(s, c, i, e);
    lemma_rollback_versions(s, c, i, e);
    lemma_rollback_reads(s, c, i, e);
    lemma_rollback_streams(s, c, i, e);
    lemma_rollback_all_logs(s, c, i, e);
    lemma_rollback_committed(s, c, i, e);
    lemma_rollback_final(s, c, i, e);
    lemma_rollback_rolled_back(s, c, i, e);
}

} // verus!
