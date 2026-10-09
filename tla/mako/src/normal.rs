//! Transaction actions and closed-timestamp production. Prepare is the
//! successful OCC certification/binding interface: validated reads and write
//! locks stay protected until the common timestamp and local obligations are
//! bound. It is not an implementation proof of the distributed RPC rounds.
use super::types::*;
use super::types::View;
use vstd::prelude::*;

verus! {

pub open spec fn can_submit(s: State, c: Constants, id: int, body: Txn, coord: int) -> bool {
    id >= 0 && !s.txns.dom().contains(id) && valid_txn(body)
        && is_shard(c, coord) && s.shards[coord].alive
}
pub open spec fn submit(s: State, id: int, body: Txn, coord: int) -> State {
    let r = TxnRec { body, coord, epoch: s.shards[coord].epoch, status: Status::Running,
        reads: Map::empty(), ts: 0, pidx: 0, installed: Set::empty(), published: Set::empty(),
        invoked: s.tick, prepared_at: 0, provisional_at: None, final_at: None };
    State { txns: s.txns.insert(id, r), ..s }
}
pub open spec fn readable_top(s: State, c: Constants, r: TxnRec, k: int) -> bool {
    let vs = vers(s.versions, k);
    vs.len() == 0 || {
        let v = vs.last();
        v.epoch == r.epoch || (v.epoch < r.epoch
            && final_ready(s.views, c, owner(c, k), v.epoch)
            && below_view(s.views, c, owner(c, k), v.epoch, v.ts))
    }
}
pub open spec fn can_read(s: State, c: Constants, id: int, k: int) -> bool {
    &&& s.txns.dom().contains(id) && s.txns[id].status is Running
    &&& read_set(s.txns[id].body).contains(k) && !s.txns[id].reads.dom().contains(k)
    &&& s.shards[owner(c, k)].alive && s.shards[owner(c, k)].epoch == s.txns[id].epoch
    &&& readable_top(s, c, s.txns[id], k)
}
pub open spec fn read_rec(v: Map<int, Seq<Version>>, k: int) -> ReadRec {
    let vs = vers(v, k);
    if vs.len() == 0 { ReadRec { writer: -1, epoch: 0, ts: 0, value: 0 } }
    else { let x = vs.last(); ReadRec { writer: x.txn, epoch: x.epoch, ts: x.ts, value: x.value } }
}
pub open spec fn read(s: State, id: int, k: int) -> State {
    let r = s.txns[id];
    State { txns: s.txns.insert(id, TxnRec { reads: r.reads.insert(k, read_rec(s.versions, k)), ..r }), ..s }
}
pub open spec fn validated(s: State, c: Constants, r: TxnRec) -> bool {
    &&& s.shards[r.coord].alive && s.shards[r.coord].epoch == r.epoch
    &&& forall|k: int| #[trigger] r.body.ops.dom().contains(k) ==>
        s.shards[owner(c, k)].alive && s.shards[owner(c, k)].epoch == r.epoch
        && !s.locks.dom().contains(k)
    &&& forall|k: int| #[trigger] read_set(r.body).contains(k) ==>
        r.reads.dom().contains(k) && top_writer(s.versions, k) == r.reads[k].writer
}
pub open spec fn can_prepare(s: State, c: Constants, id: int, ts: int) -> bool {
    &&& s.txns.dom().contains(id) && s.txns[id].status is Running
    &&& validated(s, c, s.txns[id]) && ts > 0
    &&& forall|i: int| is_shard(c, i) && #[trigger] clock_participant(c, s.txns[id], i) ==> ts > s.shards[i].clock
    &&& forall|k: int| #[trigger] s.txns[id].reads.dom().contains(k) ==> ts > s.txns[id].reads[k].ts
}
pub open spec fn prepare(s: State, c: Constants, id: int, ts: int) -> State {
    let r = s.txns[id];
    let parts = Seq::new(c.shards as nat, |i: int| (i, id)).to_set()
        .filter(|p: (int, int)| log_participant(c, r, p.0));
    State {
        txns: s.txns.insert(id, TxnRec { status: Status::Prepared, ts,
            pidx: s.order.len(), prepared_at: s.tick, ..r }),
        locks: s.locks.union_prefer_right(Map::new(write_set(r.body), |k: int| id)),
        shards: Seq::new(c.shards as nat, |i: int| if clock_participant(c, r, i) {
            ShardState { clock: ts, ..s.shards[i] }
        } else { s.shards[i] }),
        obligations: s.obligations.union_prefer_right(Map::new(parts, |p: (int, int)| Obligation { epoch: r.epoch, ts })),
        order: s.order.push(id),
        ..s
    }
}
pub open spec fn can_install(s: State, c: Constants, id: int, i: int) -> bool {
    &&& s.txns.dom().contains(id) && s.txns[id].status is Prepared
    &&& is_shard(c, i) && writes_at(c, s.txns[id].body, i) && !s.txns[id].installed.contains(i)
    &&& s.shards[i].alive && s.shards[i].epoch == s.txns[id].epoch
}
pub open spec fn install(s: State, c: Constants, id: int, i: int) -> State {
    let r = s.txns[id];
    let keys = write_set(r.body).filter(|k: int| owner(c, k) == i);
    State {
        versions: Map::new(s.versions.dom().union(keys), |k: int| if keys.contains(k) {
            vers(s.versions, k).push(Version { txn: id, epoch: r.epoch, ts: r.ts, value: write_value(r, k) })
        } else { s.versions[k] }),
        locks: s.locks.remove_keys(keys),
        txns: s.txns.insert(id, TxnRec { installed: r.installed.insert(i), ..r }),
        ..s
    }
}
pub open spec fn append(l: RaftLog, e: Entry) -> RaftLog { RaftLog { pending: l.pending.push(e), ..l } }
pub open spec fn tx_entry(r: TxnRec, id: int) -> Entry { Entry::Tx { id, epoch: r.epoch, ts: r.ts } }
pub open spec fn can_publish(s: State, c: Constants, id: int, i: int) -> bool {
    &&& s.txns.dom().contains(id) && s.txns[id].status is Prepared
    &&& is_shard(c, i) && i != s.txns[id].coord && s.txns[id].installed.contains(i)
    &&& !s.txns[id].published.contains(i)
    &&& s.shards[i].alive && s.shards[i].epoch == s.txns[id].epoch
}
pub open spec fn publish(s: State, id: int, i: int) -> State {
    let r = s.txns[id];
    State { logs: s.logs.update(i, append(s.logs[i], tx_entry(r, id))),
        txns: s.txns.insert(id, TxnRec { published: r.published.insert(i), ..r }), ..s }
}
pub open spec fn can_certify(s: State, c: Constants, id: int) -> bool {
    &&& s.txns.dom().contains(id) && s.txns[id].status is Prepared
    &&& all_installed(c, s.txns[id])
    &&& s.shards[s.txns[id].coord].alive && s.shards[s.txns[id].coord].epoch == s.txns[id].epoch
    &&& forall|i: int| is_shard(c, i) && i != s.txns[id].coord && writes_at(c, s.txns[id].body, i)
        ==> #[trigger] s.txns[id].published.contains(i)
}
pub open spec fn certify(s: State, id: int) -> State {
    let r = s.txns[id];
    State { logs: s.logs.update(r.coord, append(s.logs[r.coord], tx_entry(r, id))),
        txns: s.txns.insert(id, TxnRec { status: Status::Certified, published: r.published.insert(r.coord), ..r }), ..s }
}
pub open spec fn can_provisional(s: State, id: int) -> bool {
    s.txns.dom().contains(id) && s.txns[id].status is Certified && s.txns[id].provisional_at is None
}
pub open spec fn provisional(s: State, id: int) -> State {
    State { txns: s.txns.insert(id, TxnRec { provisional_at: Some(s.tick), ..s.txns[id] }), ..s }
}
pub open spec fn can_final(s: State, c: Constants, id: int) -> bool {
    &&& s.txns.dom().contains(id) && s.txns[id].status is Certified
    &&& s.txns[id].provisional_at is Some
    &&& below_view(s.views, c, s.txns[id].coord, s.txns[id].epoch, s.txns[id].ts)
}
pub open spec fn acknowledge_final(s: State, id: int) -> State {
    State { txns: s.txns.insert(id, TxnRec { status: Status::Final, final_at: Some(s.tick), ..s.txns[id] }), ..s }
}
pub open spec fn can_abort(s: State, id: int) -> bool { s.txns.dom().contains(id) && s.txns[id].status is Running }
pub open spec fn abort(s: State, id: int) -> State {
    State { txns: s.txns.insert(id, TxnRec { status: Status::Aborted { prepared: false }, ..s.txns[id] }), ..s }
}

/// Pulse is the timestamp-service interface for idle shards. Clock advances
/// alone never advance a watermark or discharge a log obligation.
pub open spec fn can_pulse(s: State, c: Constants, i: int, ts: int) -> bool {
    is_shard(c, i) && s.shards[i].alive && ts >= s.shards[i].clock
}
pub open spec fn pulse(s: State, i: int, ts: int) -> State {
    State { shards: s.shards.update(i, ShardState { clock: ts, ..s.shards[i] }), ..s }
}
pub open spec fn can_barrier(s: State, c: Constants, i: int, through: int) -> bool {
    &&& is_shard(c, i) && s.shards[i].alive
    &&& 0 <= through <= s.shards[i].clock
    &&& frontier(entries(s.logs[i]), s.shards[i].epoch) <= through
    &&& forall|p: (int, int)| #[trigger] s.obligations.dom().contains(p)
        && p.0 == i && s.obligations[p].epoch == s.shards[i].epoch ==> through < s.obligations[p].ts
}
pub open spec fn barrier(s: State, i: int, through: int) -> State {
    State { logs: s.logs.update(i, append(s.logs[i], Entry::Barrier { epoch: s.shards[i].epoch, through })), ..s }
}
pub open spec fn can_replicate(s: State, c: Constants, i: int) -> bool {
    is_shard(c, i) && s.logs[i].pending.len() > 0
}
pub open spec fn replicate(s: State, i: int) -> State {
    let l = s.logs[i];
    let e = l.pending[0];
    State {
        logs: s.logs.update(i, RaftLog { durable: l.durable.push(e), pending: l.pending.drop_first() }),
        obligations: match e { Entry::Tx { id, .. } => s.obligations.remove((i, id)), _ => s.obligations },
        ..s
    }
}
pub open spec fn can_send(s: State, c: Constants, i: int, epoch: nat) -> bool {
    is_shard(c, i) && s.shards[i].alive && epoch <= s.shards[i].epoch
}
pub open spec fn report(s: State, i: int, epoch: nat) -> Report {
    Report { shard: i, epoch, through: frontier(s.logs[i].durable, epoch), closed: has_close(s.logs[i].durable, epoch) }
}
pub open spec fn send(s: State, i: int, epoch: nat) -> State {
    State { network: s.network.insert(report(s, i, epoch)), ..s }
}
pub open spec fn can_receive(s: State, c: Constants, observer: int, r: Report) -> bool {
    is_shard(c, observer) && s.shards[observer].alive && s.network.contains(r)
}
pub open spec fn receive(s: State, observer: int, r: Report) -> State {
    let old = view(s.views, observer, r.epoch, r.shard);
    State { views: s.views.insert((observer, r.epoch, r.shard),
        View { through: imax(old.through, r.through), closed: old.closed || r.closed }), ..s }
}

} // verus!
