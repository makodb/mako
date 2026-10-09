//! Transaction-layer inductive invariants for the MakoV2 transition system.
use super::types::*;
use super::log_invariants::log_inv;
use vstd::prelude::*;

verus! {

pub open spec fn txn(txns: Map<int, TxnRec>, id: int) -> TxnRec { txns[id] }
pub open spec fn shard_epoch(shards: Seq<ShardState>, i: int) -> nat { shards[i].epoch }
pub open spec fn clock(shards: Seq<ShardState>, i: int) -> int { shards[i].clock }
pub open spec fn inv_shapes(s: State, c: Constants) -> bool {
    &&& valid_constants(c)
    &&& s.shards.len() == c.shards && s.logs.len() == c.shards
    &&& forall|i: int| is_shard(c, i) ==> #[trigger] shard_epoch(s.shards, i) <= cm_epoch(s)
        && #[trigger] clock(s.shards, i) >= 0
    &&& forall|k: int| #[trigger] s.versions.dom().contains(k) ==> k >= 0
}
pub open spec fn inv_txn(s: State, c: Constants, id: int) -> bool {
    let r = txn(s.txns, id);
    &&& valid_txn(r.body) && is_shard(c, r.coord)
    &&& r.epoch <= shard_epoch(s.shards, r.coord)
    &&& in_flight(r) ==> r.epoch == shard_epoch(s.shards, r.coord)
    &&& in_flight(r) ==> s.shards[r.coord].alive
    &&& forall|i: int| #[trigger] r.installed.contains(i) ==> is_shard(c, i) && writes_at(c, r.body, i)
    &&& forall|i: int| #[trigger] r.published.contains(i) ==> is_shard(c, i) && log_participant(c, r, i)
        && (i == r.coord ==> certified(r)) && (i != r.coord ==> r.installed.contains(i))
    &&& forall|k: int| #[trigger] r.reads.dom().contains(k) ==> read_set(r.body).contains(k)
    &&& r.invoked < s.tick
    &&& !prepared(r) ==> r.installed.is_empty() && r.published.is_empty() && r.ts == 0
    &&& prepared(r) ==> r.ts > 0 && r.pidx < s.order.len() && s.order[r.pidx as int] == id
        && r.invoked < r.prepared_at < s.tick
        && forall|k: int| #[trigger] read_set(r.body).contains(k) ==> r.reads.dom().contains(k)
    &&& prepared(r) ==> forall|i: int| is_shard(c, i) && #[trigger] clock_participant(c, r, i)
        ==> r.epoch <= s.shards[i].epoch
    &&& certified(r) ==> all_installed(c, r)
    &&& r.provisional_at is Some ==> certified(r) && r.prepared_at < r.provisional_at.unwrap() < s.tick
    &&& (r.final_at is Some) <==> (r.status is Final)
    &&& r.status is Final ==> r.provisional_at is Some && r.provisional_at.unwrap() < r.final_at.unwrap() < s.tick
}
pub open spec fn inv_txns(s: State, c: Constants) -> bool {
    forall|id: int| #[trigger] s.txns.dom().contains(id) ==> id >= 0 && inv_txn(s, c, id)
}
pub open spec fn inv_order(s: State) -> bool {
    &&& forall|j: int| 0 <= j < s.order.len() ==> s.txns.dom().contains(#[trigger] s.order[j])
        && prepared(txn(s.txns, s.order[j])) && txn(s.txns, s.order[j]).pidx == j
    &&& forall|a: int, b: int| 0 <= a < b < s.order.len() ==>
        txn(s.txns, #[trigger] s.order[a]).prepared_at < txn(s.txns, #[trigger] s.order[b]).prepared_at
}
pub open spec fn inv_lock(s: State, c: Constants, k: int) -> bool {
    let id = s.locks[k];
    let r = txn(s.txns, id);
    &&& s.txns.dom().contains(id) && r.status is Prepared
    &&& write_set(r.body).contains(k) && !r.installed.contains(owner(c, k))
    &&& forall|j: int| 0 <= j < vers(s.versions, k).len() ==>
        pidx(s.txns, #[trigger] vers(s.versions, k)[j].txn) < r.pidx
}
pub open spec fn inv_locks(s: State, c: Constants) -> bool {
    &&& forall|k: int| #[trigger] s.locks.dom().contains(k) ==> inv_lock(s, c, k)
    &&& forall|id: int, k: int| #[trigger] s.txns.dom().contains(id) && txn(s.txns, id).status is Prepared
        && #[trigger] write_set(txn(s.txns, id).body).contains(k)
        && !txn(s.txns, id).installed.contains(owner(c, k))
        && s.shards[owner(c, k)].alive && shard_epoch(s.shards, owner(c, k)) == txn(s.txns, id).epoch
        ==> s.locks.dom().contains(k) && s.locks[k] == id
}
pub open spec fn inv_version(s: State, c: Constants, k: int, v: Version) -> bool {
    let r = txn(s.txns, v.txn);
    &&& s.txns.dom().contains(v.txn) && prepared(r)
    &&& v.epoch == r.epoch && v.ts == r.ts && v.value == write_value(r, k)
    &&& write_set(r.body).contains(k) && r.installed.contains(owner(c, k))
}
pub open spec fn inv_versions_of(s: State, c: Constants, k: int) -> bool {
    &&& forall|j: int| 0 <= j < s.versions[k].len() ==> inv_version(s, c, k, #[trigger] s.versions[k][j])
    &&& forall|a: int, b: int| 0 <= a < b < s.versions[k].len() ==>
        pidx(s.txns, #[trigger] s.versions[k][a].txn) < pidx(s.txns, #[trigger] s.versions[k][b].txn)
}
pub open spec fn inv_versions(s: State, c: Constants) -> bool {
    forall|k: int| #[trigger] s.versions.dom().contains(k) ==> inv_versions_of(s, c, k)
}
pub open spec fn inv_read_writer(s: State, c: Constants, id: int, k: int) -> bool {
    let r = txn(s.txns, id);
    let rd = r.reads[k];
    let w = txn(s.txns, rd.writer);
    &&& s.txns.dom().contains(rd.writer) && rd.writer != id && prepared(w)
    &&& write_set(w.body).contains(k) && w.installed.contains(owner(c, k))
    &&& rd.epoch == w.epoch && rd.ts == w.ts && rd.value == write_value(w, k)
    &&& w.epoch <= r.epoch
    &&& w.epoch < r.epoch ==> stable(s.logs, c, w)
    &&& prepared(r) ==> w.pidx < r.pidx && w.ts < r.ts
}
/// A prior writer cannot later appear between a validated read and its source.
pub open spec fn inv_read_order(s: State, c: Constants, id: int, k: int, other: int) -> bool {
    let r = txn(s.txns, id);
    let w = txn(s.txns, other);
    &&& w.installed.contains(owner(c, k)) && has_version(s.versions, k, other)
        ==> pidx(s.txns, other) <= pidx(s.txns, r.reads[k].writer)
    &&& !w.installed.contains(owner(c, k)) ==> w.status is Aborted
        || shard_epoch(s.shards, owner(c, k)) > w.epoch || !s.shards[owner(c, k)].alive
}
pub open spec fn inv_read(s: State, c: Constants, id: int, k: int) -> bool {
    let r = txn(s.txns, id);
    let rd = r.reads[k];
    &&& rd.writer == -1 ==> rd.value == 0 && rd.ts == 0
    &&& rd.writer != -1 ==> inv_read_writer(s, c, id, k)
    &&& prepared(r) ==> forall|o: int| #[trigger] s.txns.dom().contains(o) && prepared(txn(s.txns, o))
        && txn(s.txns, o).pidx < r.pidx && write_set(txn(s.txns, o).body).contains(k)
        ==> inv_read_order(s, c, id, k, o)
}
pub open spec fn inv_reads(s: State, c: Constants) -> bool {
    forall|id: int, k: int| #[trigger] s.txns.dom().contains(id) && #[trigger] txn(s.txns, id).reads.dom().contains(k)
        ==> inv_read(s, c, id, k)
}
/// Loss is local to the absent version's owner, which is fenced from publishing
/// its old record again. This is stronger than merely observing any missing log.
pub open spec fn version_lost(s: State, c: Constants, r: TxnRec, id: int, k: int) -> bool {
    !has_log(entries(s.logs[owner(c, k)]), id)
        && (!s.shards[owner(c, k)].alive || r.epoch < s.shards[owner(c, k)].epoch)
}
pub open spec fn inv_present(s: State, c: Constants, id: int) -> bool {
    let r = txn(s.txns, id);
    (r.status is Prepared || certified(r)) ==> forall|k: int| #[trigger] write_set(r.body).contains(k)
        && r.installed.contains(owner(c, k)) && !has_version(s.versions, k, id) ==>
        (doomed(s, c, r) && s.rolled_back.contains((owner(c, k), r.epoch))) || version_lost(s, c, r, id, k)
}
pub open spec fn inv_all_present(s: State, c: Constants) -> bool {
    forall|id: int| #[trigger] s.txns.dom().contains(id) ==> inv_present(s, c, id)
}
pub open spec fn inv_final(s: State, c: Constants) -> bool {
    forall|id: int| #[trigger] s.txns.dom().contains(id) && txn(s.txns, id).status is Final
        ==> stable(s.logs, c, txn(s.txns, id))
}
pub open spec fn inv_rolled_back(s: State, c: Constants) -> bool {
    forall|i: int, e: nat| #[trigger] s.rolled_back.contains((i, e)) ==> is_shard(c, i)
        && e < s.shards[i].epoch
        && (forall|j: int| is_shard(c, j) ==> has_close(#[trigger] s.logs[j].durable, e))
        && (forall|k: int, j: int| #[trigger] s.versions.dom().contains(k) && owner(c, k) == i
            && 0 <= j < s.versions[k].len() && #[trigger] s.versions[k][j].epoch == e
            ==> stable(s.logs, c, txn(s.txns, s.versions[k][j].txn)))
}
pub open spec fn occ_inv(s: State, c: Constants) -> bool {
    &&& inv_shapes(s, c)
    &&& inv_txns(s, c)
    &&& inv_order(s)
    &&& inv_locks(s, c)
    &&& inv_versions(s, c)
    &&& inv_reads(s, c)
    &&& inv_all_present(s, c)
    &&& inv_final(s, c)
    &&& inv_rolled_back(s, c)
}
pub open spec fn inv(s: State, c: Constants) -> bool { occ_inv(s, c) && log_inv(s, c) }

} // verus!
