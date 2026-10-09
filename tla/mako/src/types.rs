//! MakoV2: scalar transaction timestamps, one Raft log per shard, and explicit
//! closed-timestamp certificates. Integer timestamps abstract the total order
//! of the HLC tuples verified in timestamp.rs; 0 is the initial lower bound.
use vstd::prelude::*;

verus! {

pub enum Op { Read, Put { value: int }, Add { delta: int } }
pub struct Txn { pub ops: Map<int, Op> }
pub struct Constants { pub shards: int }
pub open spec fn valid_constants(c: Constants) -> bool { c.shards > 0 }
pub open spec fn is_shard(c: Constants, i: int) -> bool { 0 <= i < c.shards }
pub open spec fn owner(c: Constants, k: int) -> int { k % c.shards }
pub open spec fn read_set(t: Txn) -> Set<int> { t.ops.dom().filter(|k: int| !(t.ops[k] is Put)) }
pub open spec fn write_set(t: Txn) -> Set<int> { t.ops.dom().filter(|k: int| !(t.ops[k] is Read)) }
pub open spec fn valid_txn(t: Txn) -> bool {
    !t.ops.dom().is_empty() && forall|k: int| #[trigger] t.ops.dom().contains(k) ==> k >= 0
}
pub open spec fn apply_op(old: int, op: Op) -> int {
    match op { Op::Read => old, Op::Put { value } => value, Op::Add { delta } => old + delta }
}
pub open spec fn writes_at(c: Constants, t: Txn, i: int) -> bool {
    exists|k: int| #[trigger] write_set(t).contains(k) && owner(c, k) == i
}
pub open spec fn touches(c: Constants, t: Txn, i: int) -> bool {
    exists|k: int| #[trigger] t.ops.dom().contains(k) && owner(c, k) == i
}
pub open spec fn imax(a: int, b: int) -> int { if a >= b { a } else { b } }
pub open spec fn imin(a: int, b: int) -> int { if a <= b { a } else { b } }

pub enum Status { Running, Prepared, Certified, Final, Aborted { prepared: bool } }
pub struct ReadRec { pub writer: int, pub epoch: nat, pub ts: int, pub value: int }
pub struct Version { pub txn: int, pub epoch: nat, pub ts: int, pub value: int }
pub struct TxnRec {
    pub body: Txn,
    pub coord: int,
    pub epoch: nat,
    pub status: Status,
    pub reads: Map<int, ReadRec>,
    pub ts: int,
    pub pidx: nat,
    pub installed: Set<int>,
    pub published: Set<int>,
    pub invoked: nat,
    pub prepared_at: nat,
    pub provisional_at: Option<nat>,
    pub final_at: Option<nat>,
}
pub open spec fn in_flight(r: TxnRec) -> bool { r.status is Running || r.status is Prepared }
pub open spec fn prepared(r: TxnRec) -> bool {
    r.status is Prepared || r.status is Certified || r.status is Final
        || (r.status is Aborted && r.status->prepared)
}
pub open spec fn certified(r: TxnRec) -> bool { r.status is Certified || r.status is Final }
pub open spec fn log_participant(c: Constants, r: TxnRec, i: int) -> bool {
    i == r.coord || writes_at(c, r.body, i)
}
pub open spec fn clock_participant(c: Constants, r: TxnRec, i: int) -> bool {
    i == r.coord || touches(c, r.body, i)
}
pub open spec fn read_value(r: TxnRec, k: int) -> int {
    if r.reads.dom().contains(k) { r.reads[k].value } else { 0 }
}
pub open spec fn write_value(r: TxnRec, k: int) -> int { apply_op(read_value(r, k), r.body.ops[k]) }
pub open spec fn all_installed(c: Constants, r: TxnRec) -> bool {
    forall|i: int| is_shard(c, i) && writes_at(c, r.body, i) ==> #[trigger] r.installed.contains(i)
}

/// All entry kinds share the SAME physical Raft log on a shard. A barrier is
/// a finite closed timestamp, not the timestamp of the latest transaction.
pub enum Entry {
    Tx { id: int, epoch: nat, ts: int },
    Barrier { epoch: nat, through: int },
    Close { epoch: nat, cut: int },
    AdvanceSpecEpoch { epoch: nat },
}
pub struct RaftLog { pub durable: Seq<Entry>, pub pending: Seq<Entry> }
pub open spec fn entries(l: RaftLog) -> Seq<Entry> { l.durable + l.pending }
pub open spec fn is_tx(e: Entry, id: int) -> bool { e is Tx && e->Tx_id == id }
pub open spec fn has_log(es: Seq<Entry>, id: int) -> bool {
    exists|j: int| 0 <= j < es.len() && is_tx(#[trigger] es[j], id)
}
pub open spec fn has_close(es: Seq<Entry>, epoch: nat) -> bool {
    exists|j: int| 0 <= j < es.len() && #[trigger] es[j] is Close && es[j]->Close_epoch == epoch
}
pub open spec fn marker_value(e: Entry, epoch: nat) -> int {
    match e {
        Entry::Barrier { epoch: ep, through } => if ep == epoch { through } else { 0 },
        Entry::Close { epoch: ep, cut } => if ep == epoch { cut } else { 0 },
        _ => 0,
    }
}
pub open spec fn frontier(es: Seq<Entry>, epoch: nat) -> int
    decreases es.len()
{
    if es.len() == 0 { 0 } else { imax(frontier(es.drop_last(), epoch), marker_value(es.last(), epoch)) }
}
pub open spec fn clock_floor(es: Seq<Entry>) -> int
    decreases es.len()
{
    if es.len() == 0 { 0 } else {
        let t = match es.last() {
            Entry::Tx { ts, .. } => ts,
            Entry::Barrier { through, .. } => through,
            Entry::Close { cut, .. } => cut,
            _ => 0,
        };
        imax(clock_floor(es.drop_last()), t)
    }
}
pub open spec fn config_epoch(es: Seq<Entry>) -> nat
    decreases es.len()
{
    if es.len() == 0 { 0 } else {
        let old = config_epoch(es.drop_last());
        match es.last() { Entry::AdvanceSpecEpoch { epoch } => if epoch > old { epoch } else { old }, _ => old }
    }
}

/// `alive` means the transaction-serving leader is ready. Raft recovery and
/// CM control entries may progress while that speculative service is fenced.
pub struct ShardState { pub epoch: nat, pub clock: int, pub alive: bool }
/// This is local producer bookkeeping, NOT another replicated stream.
/// Binding creates an obligation before any speculative write is installed.
pub struct Obligation { pub epoch: nat, pub ts: int }
pub struct Report { pub shard: int, pub epoch: nat, pub through: int, pub closed: bool }
pub struct View { pub through: int, pub closed: bool }
pub struct State {
    pub shards: Seq<ShardState>,
    pub logs: Seq<RaftLog>,
    pub obligations: Map<(int, int), Obligation>, // (shard, transaction)
    pub versions: Map<int, Seq<Version>>,
    pub locks: Map<int, int>,
    pub txns: Map<int, TxnRec>,
    pub order: Seq<int>, // ghost successful certification order
    pub network: Set<Report>,
    pub views: Map<(int, nat, int), View>, // (observer, epoch, source shard)
    pub rolled_back: Set<(int, nat)>,
    pub tick: nat,
}
pub open spec fn cm_epoch(s: State) -> nat { config_epoch(s.logs[0].durable) }
pub open spec fn vers(v: Map<int, Seq<Version>>, k: int) -> Seq<Version> {
    if v.dom().contains(k) { v[k] } else { Seq::empty() }
}
pub open spec fn top_writer(v: Map<int, Seq<Version>>, k: int) -> int {
    if vers(v, k).len() == 0 { -1 } else { vers(v, k).last().txn }
}
pub open spec fn has_version(v: Map<int, Seq<Version>>, k: int, id: int) -> bool {
    exists|j: int| 0 <= j < vers(v, k).len() && #[trigger] vers(v, k)[j].txn == id
}
pub open spec fn view(v: Map<(int, nat, int), View>, observer: int, epoch: nat, shard: int) -> View {
    if v.dom().contains((observer, epoch, shard)) { v[(observer, epoch, shard)] }
    else { View { through: 0, closed: false } }
}
pub open spec fn below_view(v: Map<(int, nat, int), View>, c: Constants, observer: int, epoch: nat, ts: int) -> bool {
    forall|i: int| is_shard(c, i) ==> ts <= #[trigger] view(v, observer, epoch, i).through
}
pub open spec fn final_ready(v: Map<(int, nat, int), View>, c: Constants, observer: int, epoch: nat) -> bool {
    forall|i: int| is_shard(c, i) ==> #[trigger] view(v, observer, epoch, i).closed
}
pub open spec fn stable(logs: Seq<RaftLog>, c: Constants, r: TxnRec) -> bool {
    forall|i: int| is_shard(c, i) ==> r.ts <= frontier(#[trigger] logs[i].durable, r.epoch)
}
pub open spec fn lost(logs: Seq<RaftLog>, c: Constants, r: TxnRec, id: int) -> bool {
    exists|i: int| is_shard(c, i) && log_participant(c, r, i) && !has_log(entries(#[trigger] logs[i]), id)
}
pub open spec fn doomed(s: State, c: Constants, r: TxnRec) -> bool {
    !stable(s.logs, c, r) && forall|i: int| is_shard(c, i) ==> has_close(#[trigger] s.logs[i].durable, r.epoch)
}
pub open spec fn pidx(txns: Map<int, TxnRec>, id: int) -> int {
    if id == -1 { -1 } else { txns[id].pidx as int }
}
pub open spec fn init(s: State, c: Constants) -> bool {
    &&& valid_constants(c)
    &&& s.shards == Seq::new(c.shards as nat, |i: int| ShardState { epoch: 0, clock: 0, alive: true })
    &&& s.logs == Seq::new(c.shards as nat, |i: int| RaftLog { durable: Seq::empty(), pending: Seq::empty() })
    &&& s.obligations == Map::<(int, int), Obligation>::empty()
    &&& s.versions == Map::<int, Seq<Version>>::empty()
    &&& s.locks == Map::<int, int>::empty()
    &&& s.txns == Map::<int, TxnRec>::empty()
    &&& s.order == Seq::<int>::empty()
    &&& s.network == Set::<Report>::empty()
    &&& s.views == Map::<(int, nat, int), View>::empty()
    &&& s.rolled_back == Set::<(int, nat)>::empty()
    &&& s.tick == 0
}

} // verus!
