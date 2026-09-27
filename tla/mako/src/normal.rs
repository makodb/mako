//! Failure-free transaction processing: execution, certification (paper
//! Section 4.2), replication (4.3) and commit by vector watermark (4.4).
use super::types::*;
use vstd::prelude::*;

verus! {

// ---------------------------------------------------------------------------
// Submit: a client hands a one-shot transaction to a coordinator worker thread.
// A worker thread runs one transaction at a time, which is what makes its
// dedicated streams sequential (paper Section 4.3, Fact 3).
// ---------------------------------------------------------------------------

pub open spec fn can_submit(s: State, c: Constants, id: int, body: Txn, coord: int, thread: int) -> bool {
    &&& !has_txn(s, id)
    &&& valid_txn(body)
    &&& is_shard(c, coord)
    &&& is_thread(c, thread)
    &&& forall|o: int| #[trigger] s.txns.dom().contains(o)
        && s.txns[o].coord == coord && s.txns[o].thread == thread ==> !in_flight(s.txns[o])
}
pub open spec fn submit(s: State, c: Constants, id: int, body: Txn, coord: int, thread: int) -> State {
    let r = TxnRec {
        body, coord, thread,
        epoch: s.shards[coord].epoch,
        status: Status::Running,
        reads: Map::empty(),
        vc: vc_zero(c.comp),
        pidx: 0,
        installed: Set::empty(),
        invoked: s.tick,
        prepared_at: 0,
        acked: 0,
    };
    State { txns: s.txns.insert(id, r), ..s }
}

// ---------------------------------------------------------------------------
// Read: an optimistic read of the latest certified version at the owner leader.
// Cross-epoch rule (Section 5.2, Lemma 8): a newer-epoch reader may only see an
// older-epoch version once that epoch's FVW exists and the version is below it.
// ---------------------------------------------------------------------------

pub open spec fn readable_top(s: State, c: Constants, r: TxnRec, k: int) -> bool {
    let vs = vers(s, k);
    vs.len() == 0 || {
        let v = vs.last();
        v.epoch == r.epoch
            || (v.epoch < r.epoch && fvw_ready(s, c, v.epoch) && below_fvw(s, c, v.vc, v.epoch))
    }
}
pub open spec fn can_read(s: State, c: Constants, id: int, k: int) -> bool {
    &&& has_txn(s, id)
    &&& s.txns[id].status is Running
    &&& read_set(s.txns[id].body).contains(k)
    &&& !s.txns[id].reads.dom().contains(k)
    &&& s.shards[owner(c, k)].epoch == s.txns[id].epoch
    &&& readable_top(s, c, s.txns[id], k)
}
pub open spec fn read_rec(s: State, k: int) -> ReadRec {
    let vs = vers(s, k);
    if vs.len() == 0 {
        ReadRec { writer: -1, epoch: 0, vc: Seq::empty(), value: 0 }
    } else {
        let v = vs.last();
        ReadRec { writer: v.txn, epoch: v.epoch, vc: v.vc, value: v.value }
    }
}
pub open spec fn read(s: State, c: Constants, id: int, k: int) -> State {
    let r = s.txns[id];
    State { txns: s.txns.insert(id, TxnRec { reads: r.reads.insert(k, read_rec(s, k)), ..r }), ..s }
}

// ---------------------------------------------------------------------------
// Prepare: Lock + GetClock + Validate of Section 4.2 as one step across the
// involved leaders. `vc` is the resulting commit vector clock: for each
// component, the maximum of the same-epoch clocks in the ReadSet and of the
// freshly fetched clocks of the transaction's clock shards.
// ---------------------------------------------------------------------------

pub open spec fn merge_lower_bound(c: Constants, r: TxnRec, vc: Seq<int>) -> bool {
    forall|k: int| #[trigger] r.reads.dom().contains(k)
        && r.reads[k].writer != -1 && r.reads[k].epoch == r.epoch ==> vc_le(r.reads[k].vc, vc, c.comp)
}
pub open spec fn fetch_lower_bound(s: State, c: Constants, r: TxnRec, vc: Seq<int>) -> bool {
    forall|i: int| is_shard(c, i) && #[trigger] clock_shard(c, r, i) ==> vc[group(c, i)] >= s.shards[i].counter + 1
}
pub open spec fn exact_component(s: State, c: Constants, r: TxnRec, vc: Seq<int>, x: int) -> bool {
    ||| vc[x] == 0
    ||| exists|k: int| #[trigger] r.reads.dom().contains(k) && r.reads[k].writer != -1
        && r.reads[k].epoch == r.epoch && r.reads[k].vc[x] == vc[x]
    ||| exists|i: int| is_shard(c, i) && #[trigger] clock_shard(c, r, i) && group(c, i) == x
        && vc[x] == s.shards[i].counter + 1
}
pub open spec fn clock_assignment(s: State, c: Constants, r: TxnRec, vc: Seq<int>) -> bool {
    &&& vc.len() == c.comp
    &&& merge_lower_bound(c, r, vc)
    &&& fetch_lower_bound(s, c, r, vc)
    &&& forall|x: int| is_comp(c, x) ==> #[trigger] exact_component(s, c, r, vc, x)
}
/// Every key read is still at the version that was read, no key in either set
/// is write-locked, and every involved leader is still in the transaction's epoch.
pub open spec fn validated(s: State, c: Constants, r: TxnRec) -> bool {
    &&& forall|k: int| #[trigger] read_set(r.body).contains(k) ==> r.reads.dom().contains(k)
    &&& forall|k: int| #[trigger] write_set(r.body).contains(k) ==>
        s.shards[owner(c, k)].epoch == r.epoch && !s.locks.dom().contains(k)
    &&& forall|k: int| #[trigger] read_set(r.body).contains(k) ==>
        s.shards[owner(c, k)].epoch == r.epoch && !s.locks.dom().contains(k)
        && top_writer(s, k) == r.reads[k].writer
}
pub open spec fn can_prepare(s: State, c: Constants, id: int, vc: Seq<int>) -> bool {
    &&& has_txn(s, id)
    &&& s.txns[id].status is Running
    &&& validated(s, c, s.txns[id])
    &&& clock_assignment(s, c, s.txns[id], vc)
}
pub open spec fn prepare(s: State, c: Constants, id: int, vc: Seq<int>) -> State {
    let r = s.txns[id];
    let r2 = TxnRec {
        status: if read_only(r) { Status::Certified } else { Status::Prepared },
        vc,
        pidx: s.prepared.len(),
        prepared_at: s.tick,
        ..r
    };
    State {
        txns: s.txns.insert(id, r2),
        locks: s.locks.union_prefer_right(Map::new(write_set(r.body), |k: int| id)),
        shards: Seq::new(c.shards as nat, |i: int|
            if clock_shard(c, r, i) { ShardState { counter: s.shards[i].counter + 1, ..s.shards[i] } }
            else { s.shards[i] }),
        prepared: s.prepared.push(id),
        ..s
    }
}

// ---------------------------------------------------------------------------
// Install: speculatively install the writes at one shard leader, release its
// locks, bump its clock so later clocks exceed the version's (paper Appendix
// D, `Install`), and log the writes to the dedicated stream.
// ---------------------------------------------------------------------------

pub open spec fn keys_at(c: Constants, t: Txn, i: int) -> Set<int> {
    write_set(t).filter(|k: int| owner(c, k) == i)
}
pub open spec fn can_install(s: State, c: Constants, id: int, i: int) -> bool {
    &&& has_txn(s, id)
    &&& s.txns[id].status is Prepared
    &&& is_shard(c, i)
    &&& writes_at(c, s.txns[id].body, i)
    &&& !s.txns[id].installed.contains(i)
    &&& s.shards[i].epoch == s.txns[id].epoch
}
pub open spec fn new_version(r: TxnRec, id: int, k: int) -> Version {
    Version { txn: id, epoch: r.epoch, vc: r.vc, value: write_value(r, k) }
}
pub open spec fn log_entry(c: Constants, r: TxnRec, id: int, i: int) -> Entry {
    Entry::Log { txn: id, epoch: r.epoch, clock: r.vc[group(c, i)] }
}
pub open spec fn append_pending(streams: Map<Sid, Stream>, sid: Sid, e: Entry) -> Map<Sid, Stream> {
    let st = streams[sid];
    streams.insert(sid, Stream { pending: st.pending.push(e), ..st })
}
pub open spec fn install(s: State, c: Constants, id: int, i: int) -> State {
    let r = s.txns[id];
    let keys = keys_at(c, r.body, i);
    let sid = sid_of(i, r.coord, r.thread);
    State {
        versions: Map::new(s.versions.dom().union(keys), |k: int|
            if keys.contains(k) { vers(s, k).push(new_version(r, id, k)) } else { s.versions[k] }),
        locks: s.locks.remove_keys(keys),
        shards: s.shards.update(i, ShardState {
            counter: imax(s.shards[i].counter, r.vc[group(c, i)]), ..s.shards[i] }),
        streams: if i == r.coord { s.streams } else { append_pending(s.streams, sid, log_entry(c, r, id, i)) },
        txns: s.txns.insert(id, TxnRec { installed: r.installed.insert(i), ..r }),
        ..s
    }
}

// ---------------------------------------------------------------------------
// Certify: all installs done; the coordinator logs the transaction to its own
// worker stream (the implementation's `serialize_util` after `remoteInstall`).
// ---------------------------------------------------------------------------

pub open spec fn all_installed(c: Constants, r: TxnRec) -> bool {
    forall|i: int| is_shard(c, i) && writes_at(c, r.body, i) ==> #[trigger] r.installed.contains(i)
}
pub open spec fn can_certify(s: State, c: Constants, id: int) -> bool {
    &&& has_txn(s, id)
    &&& s.txns[id].status is Prepared
    &&& all_installed(c, s.txns[id])
}
pub open spec fn certify(s: State, c: Constants, id: int) -> State {
    let r = s.txns[id];
    let sid = sid_of(r.coord, r.coord, r.thread);
    State {
        streams: append_pending(s.streams, sid, log_entry(c, r, id, r.coord)),
        shards: s.shards.update(r.coord, ShardState {
            counter: imax(s.shards[r.coord].counter, r.vc[group(c, r.coord)]), ..s.shards[r.coord] }),
        txns: s.txns.insert(id, TxnRec { status: Status::Certified, ..r }),
        ..s
    }
}

// ---------------------------------------------------------------------------
// Commit: the clock vector is below the vector watermark, so the transaction
// and everything it depends on is durable; the client is acknowledged.
// ---------------------------------------------------------------------------

pub open spec fn can_commit(s: State, c: Constants, id: int) -> bool {
    &&& has_txn(s, id)
    &&& s.txns[id].status is Certified
    &&& below_wm(s, c, s.txns[id].vc, s.txns[id].epoch)
}
pub open spec fn commit(s: State, c: Constants, id: int) -> State {
    let r = s.txns[id];
    State { txns: s.txns.insert(id, TxnRec { status: Status::Committed, acked: s.tick, ..r }), ..s }
}

// ---------------------------------------------------------------------------
// Abort of a running transaction (failed lock or validation; no locks held).
// ---------------------------------------------------------------------------

pub open spec fn can_abort(s: State, id: int) -> bool {
    has_txn(s, id) && s.txns[id].status is Running
}
pub open spec fn abort(s: State, id: int) -> State {
    let r = s.txns[id];
    State { txns: s.txns.insert(id, TxnRec { status: Status::Aborted { prepared: false }, ..r }), ..s }
}

// ---------------------------------------------------------------------------
// Replicate: Paxos chooses the next pending entry of a stream.
// ---------------------------------------------------------------------------

pub open spec fn can_replicate(s: State, c: Constants, sid: Sid) -> bool {
    valid_sid(c, sid) && s.streams[sid].pending.len() > 0
}
pub open spec fn replicate(s: State, c: Constants, sid: Sid) -> State {
    let st = s.streams[sid];
    State { streams: s.streams.insert(sid, Stream {
        durable: st.durable.push(st.pending[0]), pending: st.pending.drop_first() }), ..s }
}

} // verus!
