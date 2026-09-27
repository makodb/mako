//! Logical types for the Mako speculative-2PC model (OSDI'25, Sections 4-5).
//!
//! One-shot transactions carry a per-key operation. Values are integers and a
//! key that was never written holds 0. Keys are non-negative integers owned by
//! shard `key % shards`. Vector clocks are sequences of `comp` components; shard
//! `i` contributes to component `cidx[i]` (the paper's K:M compression, Section
//! 6.1; the identity map is the paper's full vector clock, a constant map is the
//! scalar timestamp of the implementation).
use vstd::prelude::*;

verus! {

// ---------------------------------------------------------------------------
// Transaction bodies
// ---------------------------------------------------------------------------

/// `Read` returns the current value, `Put` blindly writes, `Add` reads and
/// writes (read-modify-write). Only `Put` does not read.
pub enum Op { Read, Put { value: int }, Add { delta: int } }

pub struct Txn { pub ops: Map<int, Op> }

pub open spec fn reads_key(op: Op) -> bool { !(op is Put) }
pub open spec fn writes_key(op: Op) -> bool { !(op is Read) }
pub open spec fn read_set(t: Txn) -> Set<int> {
    t.ops.dom().filter(|k: int| reads_key(t.ops[k]))
}
pub open spec fn write_set(t: Txn) -> Set<int> {
    t.ops.dom().filter(|k: int| writes_key(t.ops[k]))
}
/// The value a key holds after `op` runs against `old`.
pub open spec fn apply_op(old: int, op: Op) -> int {
    match op { Op::Read => old, Op::Put { value } => value, Op::Add { delta } => old + delta }
}

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

pub struct Constants {
    pub shards: int,      // number of shards n
    pub threads: int,     // worker threads (Paxos streams) per shard
    pub comp: int,        // vector clock length m
    pub cidx: Seq<int>,   // shard -> clock component
}

pub open spec fn valid_constants(c: Constants) -> bool {
    &&& c.shards >= 1
    &&& c.threads >= 1
    &&& c.comp >= 1
    &&& c.cidx.len() == c.shards
    &&& forall|i: int| 0 <= i < c.shards ==> 0 <= #[trigger] c.cidx[i] < c.comp
}
pub open spec fn is_shard(c: Constants, i: int) -> bool { 0 <= i < c.shards }
pub open spec fn is_thread(c: Constants, t: int) -> bool { 0 <= t < c.threads }
pub open spec fn is_comp(c: Constants, x: int) -> bool { 0 <= x < c.comp }
pub open spec fn valid_key(k: int) -> bool { k >= 0 }
pub open spec fn owner(c: Constants, k: int) -> int { k % c.shards }
pub open spec fn group(c: Constants, i: int) -> int { c.cidx[i] }
pub open spec fn valid_txn(t: Txn) -> bool {
    &&& t.ops.dom().len() > 0
    &&& forall|k: int| #[trigger] t.ops.dom().contains(k) ==> valid_key(k)
}
/// Does `t` write some key owned by shard `i`?
pub open spec fn writes_at(c: Constants, t: Txn, i: int) -> bool {
    exists|k: int| #[trigger] write_set(t).contains(k) && owner(c, k) == i
}

// ---------------------------------------------------------------------------
// Vector clocks and watermark values
// ---------------------------------------------------------------------------

pub open spec fn vc_zero(comp: int) -> Seq<int> { Seq::new(comp as nat, |x: int| 0) }
pub open spec fn vc_le(a: Seq<int>, b: Seq<int>, comp: int) -> bool {
    forall|x: int| 0 <= x < comp ==> #[trigger] a[x] <= #[trigger] b[x]
}

/// A shard/stream watermark: a finite clock or INF (the healthy epoch-closing
/// marker of Section 5.2).
pub enum Wm { Fin(int), Inf }

pub open spec fn wm_le(x: int, w: Wm) -> bool {
    match w { Wm::Fin(v) => x <= v, Wm::Inf => true }
}
pub open spec fn wm_le_wm(a: Wm, b: Wm) -> bool {
    match a { Wm::Fin(x) => wm_le(x, b), Wm::Inf => b is Inf }
}

// ---------------------------------------------------------------------------
// Replication streams (one Paxos instance each)
// ---------------------------------------------------------------------------

/// A stream entry: the writes of one transaction at this shard (carrying the
/// shard's own clock component) or an epoch-closing INF marker.
pub enum Entry { Log { txn: int, epoch: nat, clock: int }, Inf { epoch: nat } }

pub open spec fn entry_epoch(e: Entry) -> nat {
    match e { Entry::Log { epoch, .. } => epoch, Entry::Inf { epoch } => epoch }
}
pub open spec fn entry_wm(e: Entry) -> Wm {
    match e { Entry::Log { clock, .. } => Wm::Fin(clock), Entry::Inf { .. } => Wm::Inf }
}
pub open spec fn is_log_of(e: Entry, txn: int) -> bool {
    e is Log && e->Log_txn == txn
}

/// `durable` is the chosen (replicated) prefix; `pending` is proposed but not
/// yet chosen. A leader failure may lose any suffix of `pending`.
pub struct Stream { pub durable: Seq<Entry>, pub pending: Seq<Entry> }

pub open spec fn all_entries(st: Stream) -> Seq<Entry> { st.durable + st.pending }

/// Streams are dedicated: at shard `shard`, entries produced on behalf of
/// coordinator `coord`'s worker thread `thread` go to this stream. At the
/// coordinator itself (`shard == coord`) this is the worker's own stream.
pub struct Sid { pub shard: int, pub coord: int, pub thread: int }

pub open spec fn valid_sid(c: Constants, sid: Sid) -> bool {
    is_shard(c, sid.shard) && is_shard(c, sid.coord) && is_thread(c, sid.thread)
}
pub open spec fn sid_of(shard: int, coord: int, thread: int) -> Sid {
    Sid { shard, coord, thread }
}

// ---------------------------------------------------------------------------
// Versions, reads, transaction records
// ---------------------------------------------------------------------------

/// A speculatively installed version at a shard leader.
pub struct Version { pub txn: int, pub epoch: nat, pub vc: Seq<int>, pub value: int }

/// What a transaction observed when it read a key. `writer == -1` means the
/// initial value (no version installed yet).
pub struct ReadRec { pub writer: int, pub epoch: nat, pub vc: Seq<int>, pub value: int }

pub enum Status {
    Running,                        // executing reads at the coordinator
    Prepared,                       // locked, clocked and validated; installing
    Certified,                      // installed everywhere; coordinator logged it
    Committed,                      // below the vector watermark; acknowledged
    Aborted { prepared: bool },     // `prepared` = terminated after certification began
}

pub struct TxnRec {
    pub body: Txn,
    pub coord: int,
    pub thread: int,
    pub epoch: nat,
    pub status: Status,
    pub reads: Map<int, ReadRec>,
    pub vc: Seq<int>,
    pub pidx: nat,          // position in `State::prepared` (once prepared)
    pub installed: Set<int>,
    pub invoked: nat,
    pub prepared_at: nat,
    pub acked: nat,
}

pub open spec fn in_flight(r: TxnRec) -> bool { r.status is Running || r.status is Prepared }
pub open spec fn is_prepared_or_later(r: TxnRec) -> bool {
    r.status is Prepared || r.status is Certified || r.status is Committed
        || (r.status is Aborted && r.status->prepared)
}
pub open spec fn read_only(r: TxnRec) -> bool { write_set(r.body).is_empty() }
/// Shards whose logical clock the transaction fetches (paper GetClock plus the
/// coordinator, as in the implementation's `updateSingleTimestamp`).
pub open spec fn clock_shard(c: Constants, r: TxnRec, i: int) -> bool {
    !read_only(r) && (writes_at(c, r.body, i) || i == r.coord)
}
/// The value the transaction read for key `k` (0 when it did not read it).
pub open spec fn read_value(r: TxnRec, k: int) -> int {
    if r.reads.dom().contains(k) { r.reads[k].value } else { 0 }
}
/// The value the transaction writes to `k` (meaningful when `k` is in its write set).
pub open spec fn write_value(r: TxnRec, k: int) -> int {
    apply_op(read_value(r, k), r.body.ops[k])
}

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------

pub struct ShardState { pub epoch: nat, pub counter: int }

pub struct State {
    pub epoch: nat,                          // configuration manager's epoch
    pub shards: Seq<ShardState>,             // leader state per shard
    pub streams: Map<Sid, Stream>,           // every valid stream id
    pub versions: Map<int, Seq<Version>>,    // key -> installed versions, oldest first
    pub locks: Map<int, int>,                // key -> transaction holding its write lock
    pub txns: Map<int, TxnRec>,
    pub final_wm: Map<(int, nat), Wm>,       // (shard, epoch) -> finalized shard watermark
    pub rolled_back: Set<(int, nat)>,        // (shard, epoch) whose rollback has run
    pub prepared: Seq<int>,                  // ghost: certification (serialization) order
    pub tick: nat,
}

pub open spec fn imax(a: int, b: int) -> int { if a >= b { a } else { b } }

/// Initial states: epoch 0, all clocks 0, one empty stream per valid id, no
/// versions, locks or transactions.
pub open spec fn init(s: State, c: Constants) -> bool {
    &&& valid_constants(c)
    &&& s.epoch == 0
    &&& s.shards == Seq::new(c.shards as nat, |i: int| ShardState { epoch: 0, counter: 0 })
    &&& forall|sid: Sid| #[trigger] s.streams.dom().contains(sid) <==> valid_sid(c, sid)
    &&& forall|sid: Sid| valid_sid(c, sid) ==>
        #[trigger] s.streams[sid] == Stream { durable: Seq::empty(), pending: Seq::empty() }
    &&& s.versions == Map::<int, Seq<Version>>::empty()
    &&& s.locks == Map::<int, int>::empty()
    &&& s.txns == Map::<int, TxnRec>::empty()
    &&& s.final_wm == Map::<(int, nat), Wm>::empty()
    &&& s.rolled_back == Set::<(int, nat)>::empty()
    &&& s.prepared == Seq::<int>::empty()
    &&& s.tick == 0
}

// ---------------------------------------------------------------------------
// Store and watermark helpers
// ---------------------------------------------------------------------------

pub open spec fn vers(s: State, k: int) -> Seq<Version> {
    if s.versions.dom().contains(k) { s.versions[k] } else { Seq::empty() }
}
pub open spec fn top_writer(s: State, k: int) -> int {
    if vers(s, k).len() == 0 { -1 } else { vers(s, k).last().txn }
}
pub open spec fn has_txn(s: State, id: int) -> bool { s.txns.dom().contains(id) }
pub open spec fn shard_of_sid(s: State, sid: Sid) -> ShardState { s.shards[sid.shard] }

/// The watermark a stream's durable prefix contributes for epoch `e`: the clock
/// of its last durable epoch-`e` entry (INF for the closing marker), 0 if none.
pub open spec fn stream_wm(es: Seq<Entry>, e: nat) -> Wm
    decreases es.len()
{
    if es.len() == 0 { Wm::Fin(0) }
    else if entry_epoch(es.last()) == e { entry_wm(es.last()) }
    else { stream_wm(es.drop_last(), e) }
}

/// Paper Section 4.4: a clock vector is below the (compressed) vector watermark
/// of epoch `e` when every shard's streams have replicated past its component.
pub open spec fn below_wm(s: State, c: Constants, vc: Seq<int>, e: nat) -> bool {
    forall|sid: Sid| valid_sid(c, sid) ==>
        wm_le(vc[group(c, sid.shard)], stream_wm(#[trigger] s.streams[sid].durable, e))
}
pub open spec fn fvw_ready(s: State, c: Constants, e: nat) -> bool {
    forall|i: int| is_shard(c, i) ==> #[trigger] s.final_wm.dom().contains((i, e))
}
/// Below the finalized vector watermark of epoch `e` (Section 5.2).
pub open spec fn below_fvw(s: State, c: Constants, vc: Seq<int>, e: nat) -> bool {
    forall|i: int| is_shard(c, i) ==> wm_le(vc[group(c, i)], #[trigger] s.final_wm[(i, e)])
}
/// A transaction that the epoch's FVW has condemned: its writes are rolled back.
pub open spec fn doomed(s: State, c: Constants, id: int) -> bool {
    let r = s.txns[id];
    fvw_ready(s, c, r.epoch) && !below_fvw(s, c, r.vc, r.epoch)
}
pub open spec fn doomed_version(s: State, c: Constants, v: Version) -> bool {
    fvw_ready(s, c, v.epoch) && !below_fvw(s, c, v.vc, v.epoch)
}

pub open spec fn no_pending_epoch(st: Stream, e: nat) -> bool {
    forall|j: int| 0 <= j < st.pending.len() ==> entry_epoch(#[trigger] st.pending[j]) != e
}
pub open spec fn durable_has_log(st: Stream, txn: int) -> bool {
    exists|j: int| 0 <= j < st.durable.len() && is_log_of(#[trigger] st.durable[j], txn)
}

} // verus!
