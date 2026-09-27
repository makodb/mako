//! Shard-leader failure, epoch advancement, finalized watermarks and rollback
//! (paper Section 5.2). Paxos is a per-stream durable prefix; a crash loses
//! any suffix of a stream's pending entries.
use super::types::*;
use vstd::prelude::*;

verus! {

/// Abort every in-flight transaction coordinated by shard `i`; a prepared one
/// is a hanging transaction that is terminated.
pub open spec fn abort_coordinated(txns: Map<int, TxnRec>, i: int) -> Map<int, TxnRec> {
    txns.map_entries(|id: int, r: TxnRec|
        if r.coord == i && in_flight(r) {
            TxnRec { status: Status::Aborted { prepared: r.status is Prepared }, ..r }
        } else { r })
}

// ---------------------------------------------------------------------------
// Crash: the leader of a current-epoch shard fails. The configuration manager
// advances the epoch; the new leader keeps a prefix of each stream's pending
// entries (what Paxos can still recover), fills the rest with no-ops, rebuilds
// its store from the durable streams and starts the new epoch with clock 0.
// ---------------------------------------------------------------------------

pub open spec fn can_crash(s: State, c: Constants, i: int, survive: Map<Sid, nat>) -> bool {
    &&& is_shard(c, i)
    &&& s.shards[i].epoch == s.epoch
    &&& forall|sid: Sid| valid_sid(c, sid) && sid.shard == i ==>
        #[trigger] survive.dom().contains(sid) && survive[sid] <= s.streams[sid].pending.len()
}
pub open spec fn crash_streams(s: State, i: int, survive: Map<Sid, nat>) -> Map<Sid, Stream> {
    s.streams.map_entries(|sid: Sid, st: Stream|
        if sid.shard == i {
            Stream { durable: st.durable + st.pending.take(survive[sid] as int), pending: Seq::empty() }
        } else { st })
}
/// A version at shard `i` survives iff its transaction's entry for this shard
/// was chosen by Paxos (the new leader replays only chosen entries).
pub open spec fn survives_crash(s: State, streams: Map<Sid, Stream>, i: int, v: Version) -> bool {
    let r = s.txns[v.txn];
    durable_has_log(streams[sid_of(i, r.coord, r.thread)], v.txn)
}
pub open spec fn crash_versions(s: State, c: Constants, streams: Map<Sid, Stream>, i: int) -> Map<int, Seq<Version>> {
    Map::new(s.versions.dom(), |k: int|
        if owner(c, k) == i { s.versions[k].filter(|v: Version| survives_crash(s, streams, i, v)) }
        else { s.versions[k] })
}
/// Locks on the failed shard's keys and locks held by its in-flight
/// transactions are gone.
pub open spec fn crash_locks(s: State, c: Constants, i: int) -> Map<int, int> {
    s.locks.filter_keys(|k: int| owner(c, k) != i && s.txns[s.locks[k]].coord != i)
}
pub open spec fn crash(s: State, c: Constants, i: int, survive: Map<Sid, nat>) -> State {
    let streams = crash_streams(s, i, survive);
    State {
        epoch: s.epoch + 1,
        shards: s.shards.update(i, ShardState { epoch: s.epoch + 1, counter: 0 }),
        streams,
        versions: crash_versions(s, c, streams, i),
        locks: crash_locks(s, c, i),
        txns: abort_coordinated(s.txns, i),
        ..s
    }
}

// ---------------------------------------------------------------------------
// AdvanceEpoch: a healthy shard that lags the configuration manager closes its
// epoch. Its in-flight transactions are certified or terminated first; if none
// was terminated after preparing it ends every stream with INF.
// ---------------------------------------------------------------------------

pub open spec fn can_advance(s: State, c: Constants, i: int) -> bool {
    is_shard(c, i) && s.shards[i].epoch < s.epoch
}
pub open spec fn hung(s: State, i: int) -> bool {
    exists|id: int| #[trigger] s.txns.dom().contains(id)
        && s.txns[id].coord == i && s.txns[id].status is Prepared
}
pub open spec fn append_inf(streams: Map<Sid, Stream>, i: int, e: nat) -> Map<Sid, Stream> {
    streams.map_entries(|sid: Sid, st: Stream|
        if sid.shard == i { Stream { pending: st.pending.push(Entry::Inf { epoch: e }), ..st } } else { st })
}
pub open spec fn advance(s: State, c: Constants, i: int) -> State {
    let e = s.shards[i].epoch;
    State {
        shards: s.shards.update(i, ShardState { epoch: e + 1, counter: 0 }),
        streams: if hung(s, i) { s.streams } else { append_inf(s.streams, i, e) },
        locks: s.locks.filter_keys(|k: int| s.txns[s.locks[k]].coord != i),
        txns: abort_coordinated(s.txns, i),
        ..s
    }
}

// ---------------------------------------------------------------------------
// CloseEpoch: once nothing of epoch `e` is still pending at shard `i`, its
// finalized shard watermark is the minimum of its streams' watermarks.
// ---------------------------------------------------------------------------

pub open spec fn can_close(s: State, c: Constants, i: int, e: nat, w: Wm) -> bool {
    &&& is_shard(c, i)
    &&& s.shards[i].epoch > e
    &&& !s.final_wm.dom().contains((i, e))
    &&& forall|sid: Sid| valid_sid(c, sid) && sid.shard == i ==>
        no_pending_epoch(#[trigger] s.streams[sid], e) && wm_le_wm(w, stream_wm(s.streams[sid].durable, e))
    &&& exists|sid: Sid| valid_sid(c, sid) && sid.shard == i && w == stream_wm(#[trigger] s.streams[sid].durable, e)
}
pub open spec fn close(s: State, i: int, e: nat, w: Wm) -> State {
    State { final_wm: s.final_wm.insert((i, e), w), ..s }
}

// ---------------------------------------------------------------------------
// Rollback: with the epoch's FVW established, a shard leader discards every
// version of that epoch that is not below it.
// ---------------------------------------------------------------------------

pub open spec fn can_rollback(s: State, c: Constants, i: int, e: nat) -> bool {
    is_shard(c, i) && fvw_ready(s, c, e) && !s.rolled_back.contains((i, e))
}
pub open spec fn keep_after_rollback(s: State, c: Constants, e: nat, v: Version) -> bool {
    !(v.epoch == e && !below_fvw(s, c, v.vc, e))
}
pub open spec fn rollback(s: State, c: Constants, i: int, e: nat) -> State {
    State {
        versions: Map::new(s.versions.dom(), |k: int|
            if owner(c, k) == i { s.versions[k].filter(|v: Version| keep_after_rollback(s, c, e, v)) }
            else { s.versions[k] }),
        rolled_back: s.rolled_back.insert((i, e)),
        ..s
    }
}

} // verus!
