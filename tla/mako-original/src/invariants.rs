//! The inductive invariant. Each conjunct is a named predicate so that the
//! per-action preservation proofs and the safety theorems can cite them.
//! Helpers are defined on state components (see types.rs) so that unchanged
//! components carry their facts across a transition by congruence.
use super::types::*;
use super::normal::*;
use super::recovery::*;
use vstd::prelude::*;

verus! {

// ---------------------------------------------------------------------------
// Derived predicates used by the invariant
// ---------------------------------------------------------------------------

pub open spec fn txn(txns: Map<int, TxnRec>, id: int) -> TxnRec { txns[id] }
pub open spec fn shard_epoch(shards: Seq<ShardState>, i: int) -> nat { shards[i].epoch }
pub open spec fn counter(shards: Seq<ShardState>, i: int) -> int { shards[i].counter }
pub open spec fn coord_sid(r: TxnRec, i: int) -> Sid { sid_of(i, r.coord, r.thread) }
pub open spec fn stream_at(streams: Map<Sid, Stream>, r: TxnRec, i: int) -> Seq<Entry> {
    all_entries(streams[coord_sid(r, i)])
}

pub open spec fn has_log(es: Seq<Entry>, id: int) -> bool {
    exists|j: int| 0 <= j < es.len() && is_log_of(#[trigger] es[j], id)
}
pub open spec fn has_version(versions: Map<int, Seq<Version>>, k: int, id: int) -> bool {
    exists|j: int| 0 <= j < vers(versions, k).len() && #[trigger] vers(versions, k)[j].txn == id
}
/// Every epoch-`e` log entry of the stream has a clock below `x`.
pub open spec fn logs_below(es: Seq<Entry>, e: nat, x: int) -> bool {
    forall|j: int| 0 <= j < es.len() && #[trigger] es[j] is Log && es[j]->Log_epoch == e ==> es[j]->Log_clock < x
}
pub open spec fn no_inf(es: Seq<Entry>, e: nat) -> bool {
    forall|j: int| 0 <= j < es.len() ==> !(#[trigger] es[j] is Inf && es[j]->Inf_epoch == e)
}
/// The stream can never again reach `x` for epoch `e`.
pub open spec fn stream_below(es: Seq<Entry>, e: nat, x: int) -> bool {
    logs_below(es, e, x) && no_inf(es, e)
}
pub open spec fn committed(r: TxnRec) -> bool { r.status is Committed }
pub open spec fn certified_or_committed(r: TxnRec) -> bool {
    r.status is Certified || r.status is Committed
}
pub open spec fn aborted_prepared(r: TxnRec) -> bool {
    r.status is Aborted && r.status->prepared
}
/// The shards at which `r` must carry a log entry once it reached the given point.
pub open spec fn logged_at(c: Constants, r: TxnRec, i: int) -> bool {
    clock_shard(c, r, i) && (if i == r.coord { certified_or_committed(r) } else { r.installed.contains(i) })
}
pub open spec fn pidx_of(txns: Map<int, TxnRec>, id: int) -> int {
    if id == -1 { -1 } else { txn(txns, id).pidx as int }
}
/// Some entry the transaction should carry is missing from its stream.
pub open spec fn lost(streams: Map<Sid, Stream>, c: Constants, r: TxnRec, id: int) -> bool {
    exists|i: int| is_shard(c, i) && #[trigger] logged_at(c, r, i) && !has_log(stream_at(streams, r, i), id)
}

// ---------------------------------------------------------------------------
// W1-W4: shapes
// ---------------------------------------------------------------------------

pub open spec fn inv_shapes(s: State, c: Constants) -> bool {
    &&& valid_constants(c)
    &&& s.shards.len() == c.shards
    &&& forall|sid: Sid| #[trigger] s.streams.dom().contains(sid) <==> valid_sid(c, sid)
    &&& forall|i: int| #![trigger shard_epoch(s.shards, i)] #![trigger counter(s.shards, i)]
        is_shard(c, i) ==> shard_epoch(s.shards, i) <= s.epoch && counter(s.shards, i) >= 0
    &&& forall|k: int| #[trigger] s.versions.dom().contains(k) ==> valid_key(k)
}

// ---------------------------------------------------------------------------
// W5: transaction records
// ---------------------------------------------------------------------------

pub open spec fn inv_txn(s: State, c: Constants, id: int) -> bool {
    let r = txn(s.txns, id);
    &&& valid_txn(r.body)
    &&& is_shard(c, r.coord)
    &&& is_thread(c, r.thread)
    &&& r.epoch <= shard_epoch(s.shards, r.coord)
    &&& in_flight(r) ==> r.epoch == shard_epoch(s.shards, r.coord)
    &&& aborted_prepared(r) ==> r.epoch < shard_epoch(s.shards, r.coord)
    &&& r.vc.len() == c.comp
    &&& forall|x: int| is_comp(c, x) ==> #[trigger] r.vc[x] >= 0
    &&& is_prepared_or_later(r) ==> forall|i: int| is_shard(c, i) && #[trigger] clock_shard(c, r, i) ==> r.vc[group(c, i)] >= 1
    &&& is_prepared_or_later(r) ==> forall|k: int| #[trigger] write_set(r.body).contains(k) ==>
        shard_epoch(s.shards, owner(c, k)) >= r.epoch
    &&& forall|i: int| #[trigger] r.installed.contains(i) ==> is_shard(c, i) && writes_at(c, r.body, i)
    &&& forall|k: int| #[trigger] r.reads.dom().contains(k) ==> read_set(r.body).contains(k)
    &&& r.invoked < s.tick
    &&& r.status is Running ==> r.installed == Set::<int>::empty()
    &&& is_prepared_or_later(r) ==> r.pidx < s.prepared.len() && s.prepared[r.pidx as int] == id
        && r.invoked < r.prepared_at < s.tick
        && forall|k: int| #[trigger] read_set(r.body).contains(k) ==> r.reads.dom().contains(k)
    &&& (r.status is Prepared || aborted_prepared(r)) ==> !read_only(r)
    &&& certified_or_committed(r) ==> all_installed(c, r)
    &&& committed(r) ==> r.prepared_at < r.acked < s.tick
}
pub open spec fn inv_txns(s: State, c: Constants) -> bool {
    forall|id: int| #[trigger] s.txns.dom().contains(id) ==> id >= 0 && inv_txn(s, c, id)
}

// W6: the certification order
pub open spec fn inv_prepared(s: State, c: Constants) -> bool {
    &&& forall|j: int| 0 <= j < s.prepared.len() ==>
        has_txn(s.txns, #[trigger] s.prepared[j]) && txn(s.txns, s.prepared[j]).pidx == j
        && is_prepared_or_later(txn(s.txns, s.prepared[j]))
    &&& forall|j1: int, j2: int| 0 <= j1 < j2 < s.prepared.len() ==>
        txn(s.txns, #[trigger] s.prepared[j1]).prepared_at < txn(s.txns, #[trigger] s.prepared[j2]).prepared_at
}

// W25: one in-flight transaction per worker thread
pub open spec fn inv_exclusive(s: State, c: Constants) -> bool {
    forall|a: int, b: int| #[trigger] s.txns.dom().contains(a) && #[trigger] s.txns.dom().contains(b) && a != b
        && txn(s.txns, a).coord == txn(s.txns, b).coord && txn(s.txns, a).thread == txn(s.txns, b).thread
        ==> !(in_flight(txn(s.txns, a)) && in_flight(txn(s.txns, b)))
}

// ---------------------------------------------------------------------------
// W7, W7', W28: locks
// ---------------------------------------------------------------------------

pub open spec fn inv_lock(s: State, c: Constants, k: int) -> bool {
    let h = s.locks[k];
    &&& has_txn(s.txns, h)
    &&& txn(s.txns, h).status is Prepared
    &&& write_set(txn(s.txns, h).body).contains(k)
    &&& !txn(s.txns, h).installed.contains(owner(c, k))
    &&& forall|j: int| 0 <= j < vers(s.versions, k).len() ==>
        pidx_of(s.txns, #[trigger] vers(s.versions, k)[j].txn) < txn(s.txns, h).pidx
}
pub open spec fn inv_locks(s: State, c: Constants) -> bool {
    &&& forall|k: int| #[trigger] s.locks.dom().contains(k) ==> inv_lock(s, c, k)
    &&& forall|id: int, k: int| #[trigger] s.txns.dom().contains(id) && txn(s.txns, id).status is Prepared
        && #[trigger] write_set(txn(s.txns, id).body).contains(k)
        && !txn(s.txns, id).installed.contains(owner(c, k))
        && shard_epoch(s.shards, owner(c, k)) == txn(s.txns, id).epoch
        ==> s.locks.dom().contains(k) && s.locks[k] == id
}

// ---------------------------------------------------------------------------
// W8: versions
// ---------------------------------------------------------------------------

pub open spec fn inv_version(s: State, c: Constants, k: int, v: Version) -> bool {
    let r = txn(s.txns, v.txn);
    &&& has_txn(s.txns, v.txn)
    &&& v.epoch == r.epoch
    &&& v.vc == r.vc
    &&& v.value == write_value(r, k)
    &&& write_set(r.body).contains(k)
    &&& r.installed.contains(owner(c, k))
    &&& is_prepared_or_later(r)
}
pub open spec fn inv_versions_of(s: State, c: Constants, k: int) -> bool {
    &&& forall|j: int| 0 <= j < s.versions[k].len() ==> inv_version(s, c, k, #[trigger] s.versions[k][j])
    &&& forall|a: int, b: int| 0 <= a < b < s.versions[k].len() ==>
        txn(s.txns, #[trigger] s.versions[k][a].txn).pidx < txn(s.txns, #[trigger] s.versions[k][b].txn).pidx
}
pub open spec fn inv_versions(s: State, c: Constants) -> bool {
    forall|k: int| #[trigger] s.versions.dom().contains(k) ==> inv_versions_of(s, c, k)
}

// ---------------------------------------------------------------------------
// W9, W18, W19: read records and dependencies
// ---------------------------------------------------------------------------

pub open spec fn inv_read_writer(s: State, c: Constants, id: int, k: int) -> bool {
    let r = txn(s.txns, id);
    let rd = r.reads[k];
    let w = txn(s.txns, rd.writer);
    &&& has_txn(s.txns, rd.writer)
    &&& rd.writer != id
    &&& write_set(w.body).contains(k)
    &&& rd.epoch == w.epoch
    &&& rd.vc == w.vc
    &&& rd.value == write_value(w, k)
    &&& is_prepared_or_later(w)
    &&& w.installed.contains(owner(c, k))
    &&& w.epoch <= r.epoch
    &&& w.epoch < r.epoch ==> fvw_ready(s.final_wm, c, w.epoch) && below_fvw(s.final_wm, c, w.vc, w.epoch)
    &&& is_prepared_or_later(r) ==> w.pidx < r.pidx
    &&& is_prepared_or_later(r) && w.epoch == r.epoch ==> vc_le(w.vc, r.vc, c.comp)
}
/// W19: among transactions certified before `id`, its writer of `k` is the
/// last one whose version of `k` can still be present.
pub open spec fn inv_read_order(s: State, c: Constants, id: int, k: int, o: int) -> bool {
    let r = txn(s.txns, id);
    let rd = r.reads[k];
    let ro = txn(s.txns, o);
    &&& ro.installed.contains(owner(c, k)) && has_version(s.versions, k, o) ==> pidx_of(s.txns, o) <= pidx_of(s.txns, rd.writer)
    &&& !ro.installed.contains(owner(c, k)) ==> ro.status is Aborted || shard_epoch(s.shards, owner(c, k)) > ro.epoch
}
pub open spec fn inv_read(s: State, c: Constants, id: int, k: int) -> bool {
    let r = txn(s.txns, id);
    let rd = r.reads[k];
    &&& rd.writer == -1 ==> rd.value == 0
    &&& rd.writer != -1 ==> inv_read_writer(s, c, id, k)
    &&& is_prepared_or_later(r) ==> forall|o: int| #[trigger] s.txns.dom().contains(o) && o != id
        && is_prepared_or_later(txn(s.txns, o)) && txn(s.txns, o).pidx < r.pidx
        && write_set(txn(s.txns, o).body).contains(k) ==> inv_read_order(s, c, id, k, o)
}
pub open spec fn inv_reads(s: State, c: Constants) -> bool {
    forall|id: int, k: int| #[trigger] s.txns.dom().contains(id) && #[trigger] txn(s.txns, id).reads.dom().contains(k)
        ==> inv_read(s, c, id, k)
}

// ---------------------------------------------------------------------------
// W10, W17, W26: streams
// ---------------------------------------------------------------------------

pub open spec fn inv_entry(s: State, c: Constants, sid: Sid, e: Entry) -> bool {
    &&& entry_epoch(e) <= shard_epoch(s.shards, sid.shard)
    &&& e is Inf ==> entry_epoch(e) < shard_epoch(s.shards, sid.shard)
    &&& e is Log ==> {
        let r = txn(s.txns, e->Log_txn);
        &&& has_txn(s.txns, e->Log_txn)
        &&& r.epoch == e->Log_epoch
        &&& r.coord == sid.coord
        &&& r.thread == sid.thread
        &&& e->Log_clock == r.vc[group(c, sid.shard)]
        &&& logged_at(c, r, sid.shard)
        &&& e->Log_epoch == shard_epoch(s.shards, sid.shard) ==> e->Log_clock <= counter(s.shards, sid.shard)
    }
}
/// Fact 3 of the paper, by construction: within an epoch a stream's log
/// clocks strictly increase, epochs never decrease along the stream, an INF
/// marker is the last entry of its epoch, no transaction is logged twice, and
/// clocks are positive.
pub open spec fn inv_stream_order(es: Seq<Entry>) -> bool {
    &&& forall|a: int, b: int| 0 <= a < b < es.len() ==> entry_epoch(#[trigger] es[a]) <= entry_epoch(#[trigger] es[b])
    &&& forall|a: int, b: int| 0 <= a < b < es.len() && #[trigger] es[a] is Log && #[trigger] es[b] is Log
        && es[a]->Log_epoch == es[b]->Log_epoch ==> es[a]->Log_clock < es[b]->Log_clock
    &&& forall|a: int, b: int| 0 <= a < b < es.len() && #[trigger] es[a] is Inf
        ==> entry_epoch(#[trigger] es[b]) > es[a]->Inf_epoch
    &&& forall|a: int, b: int| 0 <= a < b < es.len() && #[trigger] es[a] is Log && #[trigger] es[b] is Log
        ==> es[a]->Log_txn != es[b]->Log_txn
    &&& forall|a: int| 0 <= a < es.len() && #[trigger] es[a] is Log ==> es[a]->Log_clock >= 1
}
pub open spec fn inv_stream(s: State, c: Constants, sid: Sid) -> bool {
    let es = all_entries(s.streams[sid]);
    &&& inv_stream_order(es)
    &&& forall|j: int| 0 <= j < es.len() ==> inv_entry(s, c, sid, #[trigger] es[j])
}
pub open spec fn inv_streams(s: State, c: Constants) -> bool {
    forall|sid: Sid| valid_sid(c, sid) ==> #[trigger] inv_stream(s, c, sid)
}

// ---------------------------------------------------------------------------
// W12, W13, W21: where a transaction's log entries are
// ---------------------------------------------------------------------------

/// W12a: a prepared (or terminated-while-prepared) transaction is not on its
/// coordinator's stream, and that stream can never reach its clock.
pub open spec fn inv_coord_below(s: State, c: Constants, r: TxnRec) -> bool {
    (r.status is Prepared || aborted_prepared(r)) ==>
        stream_below(stream_at(s.streams, r, r.coord), r.epoch, r.vc[group(c, r.coord)])
}
/// W12b: at a clock shard it has not installed at yet, every same-epoch entry
/// of a prepared transaction's dedicated stream is below its clock.
pub open spec fn inv_uninstalled_below(s: State, c: Constants, r: TxnRec) -> bool {
    r.status is Prepared ==> forall|i: int| is_shard(c, i) && #[trigger] clock_shard(c, r, i)
        && !r.installed.contains(i) ==> logs_below(stream_at(s.streams, r, i), r.epoch, r.vc[group(c, i)])
}
/// W13: where it should be logged, it is, or the entry was lost to a crash.
pub open spec fn inv_logged(s: State, c: Constants, r: TxnRec, id: int) -> bool {
    forall|i: int| is_shard(c, i) && #[trigger] logged_at(c, r, i) ==>
        has_log(stream_at(s.streams, r, i), id)
        || (shard_epoch(s.shards, i) > r.epoch && stream_below(stream_at(s.streams, r, i), r.epoch, r.vc[group(c, i)]))
}
/// W21: an installed version of a live transaction is present unless rolled
/// back or lost.
pub open spec fn inv_present(s: State, c: Constants, r: TxnRec, id: int) -> bool {
    (r.status is Prepared || certified_or_committed(r)) ==>
        forall|k: int| #[trigger] write_set(r.body).contains(k) && r.installed.contains(owner(c, k))
            && !has_version(s.versions, k, id) ==>
                (doomed(s.final_wm, c, r) && s.rolled_back.contains((owner(c, k), r.epoch))) || lost(s.streams, c, r, id)
}
pub open spec fn inv_logs(s: State, c: Constants, id: int) -> bool {
    let r = txn(s.txns, id);
    &&& inv_coord_below(s, c, r)
    &&& inv_uninstalled_below(s, c, r)
    &&& inv_logged(s, c, r, id)
    &&& inv_present(s, c, r, id)
}
pub open spec fn inv_all_logs(s: State, c: Constants) -> bool {
    forall|id: int| #[trigger] s.txns.dom().contains(id) ==> inv_logs(s, c, id)
}

// ---------------------------------------------------------------------------
// W14: committed transactions are below the vector watermark
// ---------------------------------------------------------------------------

pub open spec fn inv_committed(s: State, c: Constants) -> bool {
    forall|id: int| #[trigger] s.txns.dom().contains(id) && committed(txn(s.txns, id))
        ==> below_wm(s.streams, c, txn(s.txns, id).vc, txn(s.txns, id).epoch)
}

// ---------------------------------------------------------------------------
// W15, W16: finalized watermarks and rollback
// ---------------------------------------------------------------------------

pub open spec fn inv_final_of(s: State, c: Constants, i: int, e: nat) -> bool {
    &&& is_shard(c, i)
    &&& shard_epoch(s.shards, i) > e
    &&& forall|sid: Sid| valid_sid(c, sid) && sid.shard == i ==>
        no_pending_epoch(#[trigger] s.streams[sid], e)
        && wm_le_wm(s.final_wm[(i, e)], stream_wm(s.streams[sid].durable, e))
    &&& exists|sid: Sid| valid_sid(c, sid) && sid.shard == i
        && s.final_wm[(i, e)] == stream_wm(#[trigger] s.streams[sid].durable, e)
}
pub open spec fn inv_final(s: State, c: Constants) -> bool {
    forall|i: int, e: nat| #[trigger] s.final_wm.dom().contains((i, e)) ==> inv_final_of(s, c, i, e)
}
pub open spec fn inv_rolled_back_of(s: State, c: Constants, i: int, e: nat) -> bool {
    &&& is_shard(c, i)
    &&& fvw_ready(s.final_wm, c, e)
    &&& forall|k: int, j: int| #[trigger] s.versions.dom().contains(k) && owner(c, k) == i
        && 0 <= j < s.versions[k].len() && #[trigger] s.versions[k][j].epoch == e
        ==> below_fvw(s.final_wm, c, s.versions[k][j].vc, e)
}
pub open spec fn inv_rolled_back(s: State, c: Constants) -> bool {
    forall|i: int, e: nat| #[trigger] s.rolled_back.contains((i, e)) ==> inv_rolled_back_of(s, c, i, e)
}

// ---------------------------------------------------------------------------
// The invariant
// ---------------------------------------------------------------------------

pub open spec fn inv(s: State, c: Constants) -> bool {
    &&& inv_shapes(s, c)
    &&& inv_txns(s, c)
    &&& inv_prepared(s, c)
    &&& inv_exclusive(s, c)
    &&& inv_locks(s, c)
    &&& inv_versions(s, c)
    &&& inv_reads(s, c)
    &&& inv_streams(s, c)
    &&& inv_all_logs(s, c)
    &&& inv_committed(s, c)
    &&& inv_final(s, c)
    &&& inv_rolled_back(s, c)
}

} // verus!
