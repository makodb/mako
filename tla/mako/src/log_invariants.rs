//! Producer obligations, committed finite cuts, and observer certificate soundness.
use super::types::*;
use vstd::prelude::*;

verus! {

pub open spec fn entry_epoch(e: Entry) -> nat {
    match e { Entry::Tx { epoch, .. } | Entry::Barrier { epoch, .. } | Entry::Close { epoch, .. }
        | Entry::AdvanceSpecEpoch { epoch } => epoch }
}
pub open spec fn entry_clock(e: Entry) -> int {
    match e { Entry::Tx { ts, .. } => ts, Entry::Barrier { through, .. } => through,
        Entry::Close { cut, .. } => cut, _ => 0 }
}
pub open spec fn log_txn(s: State, c: Constants, id: int) -> bool {
    let r = s.txns[id];
    &&& is_shard(c, r.coord)
    &&& r.epoch <= s.shards[r.coord].epoch
    &&& (prepared(r) ==> r.ts > 0)
    &&& (!prepared(r) ==> r.installed.is_empty() && r.published.is_empty())
    &&& forall|i: int| is_shard(c, i) && prepared(r) && #[trigger] clock_participant(c, r, i)
        ==> r.epoch <= s.shards[i].epoch
            && (r.epoch == s.shards[i].epoch ==> r.ts <= s.shards[i].clock)
    &&& forall|i: int| #[trigger] r.installed.contains(i) ==> is_shard(c, i) && writes_at(c, r.body, i)
    &&& forall|i: int| #[trigger] r.published.contains(i) ==> is_shard(c, i) && log_participant(c, r, i)
    &&& (certified(r) ==> all_installed(c, r)
        && forall|i: int| is_shard(c, i) && log_participant(c, r, i) ==> #[trigger] r.published.contains(i))
}
pub open spec fn entry_valid(s: State, c: Constants, i: int, e: Entry) -> bool {
    &&& entry_clock(e) >= 0
    &&& match e {
        Entry::Tx { id, epoch, ts } => s.txns.dom().contains(id) && prepared(s.txns[id])
            && epoch == s.txns[id].epoch && ts == s.txns[id].ts
            && s.txns[id].published.contains(i) && (i == s.txns[id].coord ==> certified(s.txns[id]))
            && epoch <= s.shards[i].epoch,
        Entry::Barrier { epoch, .. } => epoch <= s.shards[i].epoch,
        Entry::Close { epoch, .. } => epoch < s.shards[i].epoch,
        Entry::AdvanceSpecEpoch { epoch } => i == 0 && epoch > 0,
    }
}
pub open spec fn log_shape(s: State, c: Constants) -> bool {
    &&& valid_constants(c) && s.logs.len() == c.shards && s.shards.len() == c.shards
    &&& forall|i: int| is_shard(c, i) ==> (#[trigger] s.shards[i]).clock >= 0
        && s.shards[i].epoch <= cm_epoch(s)
        && (!s.shards[i].alive ==> forall|j: int| 0 <= j < s.logs[i].pending.len()
            ==> #[trigger] s.logs[i].pending[j] is AdvanceSpecEpoch)
        && clock_floor(entries(s.logs[i])) <= s.shards[i].clock
    &&& forall|id: int| #[trigger] s.txns.dom().contains(id) ==> log_txn(s, c, id)
    &&& forall|i: int, j: int| is_shard(c, i) && 0 <= j < entries(s.logs[i]).len()
        ==> entry_valid(s, c, i, #[trigger] entries(s.logs[i])[j])
}
pub open spec fn obligation_inv(s: State, c: Constants) -> bool {
    &&& forall|p: (int, int)| #[trigger] s.obligations.dom().contains(p) ==>
        is_shard(c, p.0) && s.txns.dom().contains(p.1) && prepared(s.txns[p.1])
        && log_participant(c, s.txns[p.1], p.0)
        && s.shards[p.0].alive && s.shards[p.0].epoch == s.obligations[p].epoch
        && s.obligations[p].epoch == s.txns[p.1].epoch && s.obligations[p].ts == s.txns[p.1].ts
    &&& forall|i: int, id: int| is_shard(c, i) && #[trigger] s.txns.dom().contains(id)
        && prepared(s.txns[id]) && #[trigger] log_participant(c, s.txns[id], i)
        && s.shards[i].alive && s.shards[i].epoch == s.txns[id].epoch
        ==> has_log(s.logs[i].durable, id) || s.obligations.dom().contains((i, id))
}
/// Proposed markers already require a durable local participant record. The
/// Barrier proof derives this from the local obligation guard, not a global cut.
pub open spec fn marker_inv(s: State, c: Constants) -> bool {
    forall|i: int, id: int| is_shard(c, i) && #[trigger] s.txns.dom().contains(id)
        && prepared(s.txns[id]) && #[trigger] log_participant(c, s.txns[id], i)
        && s.txns[id].ts <= frontier(entries(s.logs[i]), s.txns[id].epoch)
        ==> has_log(s.logs[i].durable, id)
}
pub open spec fn close_inv(s: State, c: Constants) -> bool {
    forall|i: int, j: int| is_shard(c, i) && 0 <= j < entries(s.logs[i]).len()
        && #[trigger] entries(s.logs[i])[j] is Close ==> {
        let e = entries(s.logs[i])[j];
        e->Close_cut == frontier(entries(s.logs[i]).take(j), e->Close_epoch)
            && e->Close_cut == frontier(entries(s.logs[i]), e->Close_epoch)
    }
}
pub open spec fn report_sound(s: State, c: Constants, r: Report) -> bool {
    is_shard(c, r.shard) && 0 <= r.through <= frontier(s.logs[r.shard].durable, r.epoch)
        && (r.closed ==> has_close(s.logs[r.shard].durable, r.epoch)
            && r.through == frontier(s.logs[r.shard].durable, r.epoch))
}
pub open spec fn reports_inv(s: State, c: Constants) -> bool {
    &&& forall|r: Report| #[trigger] s.network.contains(r) ==> report_sound(s, c, r)
    &&& forall|p: (int, nat, int)| #[trigger] s.views.dom().contains(p) ==>
        is_shard(c, p.0) && report_sound(s, c, Report { shard: p.2, epoch: p.1,
            through: s.views[p].through, closed: s.views[p].closed })
}
pub open spec fn log_inv(s: State, c: Constants) -> bool {
    log_shape(s, c) && obligation_inv(s, c) && marker_inv(s, c) && close_inv(s, c) && reports_inv(s, c)
}

} // verus!
