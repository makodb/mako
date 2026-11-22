//! Non-vacuity witnesses: concrete behaviors of the model in which
//! transactions actually commit (and, under failures, are rolled back).
//! Each trace is built step by step with `lemma_extend`; every step proves the
//! action's guard on the concrete predecessor state.
use super::types::*;
use super::normal::*;
use super::recovery::*;
use super::behavior::*;
use super::invariants::*;
use vstd::prelude::*;

verus! {

// ---------------------------------------------------------------------------
// Building behaviors
// ---------------------------------------------------------------------------

pub proof fn lemma_initial_behavior(s: State, c: Constants)
    requires init(s, c)
    ensures behavior(seq![s], c)
{
}

pub proof fn lemma_extend(states: Seq<State>, c: Constants, a: Action) -> (after: Seq<State>)
    requires behavior(states, c), enabled(states.last(), c, a)
    ensures
        behavior(after, c),
        after == states.push(apply(states.last(), c, a)),
        after.last() == apply(states.last(), c, a),
        after.len() == states.len() + 1,
{
    let s_ = apply(states.last(), c, a);
    assert(enabled(states.last(), c, a) && s_ == apply(states.last(), c, a));
    assert(next(states.last(), s_, c));
    let after = states.push(s_);
    assert forall|i: int| 0 <= i < after.len() - 1 implies #[trigger] next(after[i], after[i + 1], c) by {
        if i < states.len() - 1 {
            assert(next(states[i], states[i + 1], c));
            assert(after[i] == states[i]);
            assert(after[i + 1] == states[i + 1]);
        } else {
            assert(after[i] == states.last());
            assert(after[i + 1] == s_);
        }
    }
    after
}

// ---------------------------------------------------------------------------
// Common ingredients
// ---------------------------------------------------------------------------

pub open spec fn empty_stream() -> Stream { Stream { durable: Seq::empty(), pending: Seq::empty() } }

/// The initial state with the given stream map (which must map exactly the
/// valid stream ids to empty streams).
pub open spec fn init_state(c: Constants, streams: Map<Sid, Stream>) -> State {
    State {
        epoch: 0,
        shards: Seq::new(c.shards as nat, |i: int| ShardState { epoch: 0, counter: 0 }),
        streams,
        versions: Map::<int, Seq<Version>>::empty(),
        locks: Map::<int, int>::empty(),
        txns: Map::<int, TxnRec>::empty(),
        final_wm: Map::<(int, nat), Wm>::empty(),
        rolled_back: Set::<(int, nat)>::empty(),
        prepared: Seq::<int>::empty(),
        tick: 0,
    }
}

// ---------------------------------------------------------------------------
// Witness 1: one shard, one worker thread; a blind write commits.
// ---------------------------------------------------------------------------

pub open spec fn c1() -> Constants { Constants { shards: 1, threads: 1, comp: 1, cidx: seq![0int] } }
pub open spec fn sid0() -> Sid { sid_of(0, 0, 0) }
pub open spec fn put7() -> Txn { Txn { ops: map![0int => Op::Put { value: 7 }] } }

pub open spec fn w1_s0() -> State { init_state(c1(), map![sid0() => empty_stream()]) }

pub open spec fn w1_a1() -> Action { Action::Submit { id: 0, body: put7(), coord: 0, thread: 0 } }
pub open spec fn w1_a2() -> Action { Action::Prepare { id: 0, vc: seq![1int] } }
pub open spec fn w1_a3() -> Action { Action::Install { id: 0, shard: 0 } }
pub open spec fn w1_a4() -> Action { Action::Certify { id: 0 } }
pub open spec fn w1_a5() -> Action { Action::Replicate { sid: sid0() } }
pub open spec fn w1_a6() -> Action { Action::Commit { id: 0 } }

pub open spec fn w1_s1() -> State { apply(w1_s0(), c1(), w1_a1()) }
pub open spec fn w1_s2() -> State { apply(w1_s1(), c1(), w1_a2()) }
pub open spec fn w1_s3() -> State { apply(w1_s2(), c1(), w1_a3()) }
pub open spec fn w1_s4() -> State { apply(w1_s3(), c1(), w1_a4()) }
pub open spec fn w1_s5() -> State { apply(w1_s4(), c1(), w1_a5()) }
pub open spec fn w1_s6() -> State { apply(w1_s5(), c1(), w1_a6()) }

pub proof fn lemma_put7_sets()
    ensures
        put7().ops.dom() == set![0int],
        forall|k: int| !(#[trigger] read_set(put7()).contains(k)),
        forall|k: int| #[trigger] write_set(put7()).contains(k) <==> k == 0,
        !write_set(put7()).is_empty(),
        valid_txn(put7()),
{
    let t = put7();
    assert(t.ops.dom() =~= set![0int]);
    assert forall|k: int| !(#[trigger] read_set(t).contains(k)) by {
        if t.ops.dom().contains(k) { assert(k == 0); }
    }
    assert forall|k: int| #[trigger] write_set(t).contains(k) <==> k == 0 by {
        if k == 0 { assert(t.ops.dom().contains(k)); }
    }
    assert(write_set(t).contains(0));
    assert(!Set::<int>::empty().contains(0));
    assert(t.ops.dom().contains(0));
}

pub proof fn lemma_w1_init()
    ensures init(w1_s0(), c1())
{
    let c = c1();
    let s = w1_s0();
    assert(c.cidx[0] == 0);
    assert(valid_constants(c)) by {
        assert forall|i: int| 0 <= i < c.shards implies 0 <= #[trigger] c.cidx[i] < c.comp by {
            assert(i == 0);
        }
    }
    assert forall|sid: Sid| #[trigger] s.streams.dom().contains(sid) <==> valid_sid(c, sid) by {
        if valid_sid(c, sid) { assert(sid == sid0()); }
    }
    assert forall|sid: Sid| valid_sid(c, sid) implies
        #[trigger] s.streams[sid] == Stream { durable: Seq::empty(), pending: Seq::empty() } by {
        assert(sid == sid0());
    }
}

pub proof fn lemma_w1_submit()
    ensures enabled(w1_s0(), c1(), w1_a1())
{
    lemma_put7_sets();
}

/// The record of transaction 0 right after Submit.
pub open spec fn w1_r1() -> TxnRec {
    TxnRec {
        body: put7(), coord: 0, thread: 0, epoch: 0, status: Status::Running,
        reads: Map::empty(), vc: vc_zero(1), pidx: 0, installed: Set::empty(),
        invoked: 0, prepared_at: 0, acked: 0,
    }
}

pub proof fn lemma_w1_s1()
    ensures
        w1_s1().txns == map![0int => w1_r1()],
        w1_s1().shards == w1_s0().shards,
        w1_s1().streams == w1_s0().streams,
        w1_s1().locks == Map::<int, int>::empty(),
        w1_s1().versions == Map::<int, Seq<Version>>::empty(),
        w1_s1().prepared == Seq::<int>::empty(),
        w1_s1().tick == 1,
{
    assert(w1_s0().shards[0].epoch == 0);
}

pub proof fn lemma_w1_prepare()
    ensures enabled(w1_s1(), c1(), w1_a2())
{
    let c = c1();
    let s = w1_s1();
    let r = w1_r1();
    let vc = seq![1int];
    lemma_w1_s1();
    lemma_put7_sets();
    assert(s.txns.dom().contains(0));
    assert(s.txns[0] == r);
    assert(s.shards[0] == ShardState { epoch: 0, counter: 0 });
    assert(c.cidx[0] == 0);
    assert(owner(c, 0) == 0);
    assert(validated(s, c, r));
    assert(!read_only(r));
    assert(clock_shard(c, r, 0));
    assert(fetch_lower_bound(s, c, r, vc)) by {
        assert forall|i: int| is_shard(c, i) && #[trigger] clock_shard(c, r, i) implies vc[group(c, i)] >= s.shards[i].counter + 1 by {
            assert(i == 0);
        }
    }
    assert forall|x: int| is_comp(c, x) implies #[trigger] exact_component(s, c, r, vc, x) by {
        assert(x == 0);
        assert(is_shard(c, 0) && clock_shard(c, r, 0) && group(c, 0) == x && vc[x] == s.shards[0].counter + 1);
    }
    assert(clock_assignment(s, c, r, vc));
}

/// The record of transaction 0 right after Prepare.
pub open spec fn w1_r2() -> TxnRec {
    TxnRec { status: Status::Prepared, vc: seq![1int], pidx: 0, prepared_at: 1, ..w1_r1() }
}

pub proof fn lemma_w1_s2()
    ensures
        w1_s2().txns == map![0int => w1_r2()],
        w1_s2().shards == seq![ShardState { epoch: 0, counter: 1 }],
        w1_s2().streams == w1_s0().streams,
        w1_s2().tick == 2,
{
    let c = c1();
    lemma_w1_s1();
    lemma_put7_sets();
    let r = w1_r1();
    assert(!read_only(r));
    assert(clock_shard(c, r, 0));
    assert(w1_s2().txns =~= map![0int => w1_r2()]);
    assert(w1_s2().shards =~= seq![ShardState { epoch: 0, counter: 1 }]);
}

pub proof fn lemma_w1_install()
    ensures enabled(w1_s2(), c1(), w1_a3())
{
    let c = c1();
    lemma_w1_s2();
    lemma_put7_sets();
    assert(write_set(put7()).contains(0) && owner(c, 0) == 0);
    assert(writes_at(c, put7(), 0));
}

/// The record of transaction 0 right after Install at shard 0.
pub open spec fn w1_r3() -> TxnRec {
    TxnRec { installed: set![0int], ..w1_r2() }
}

pub proof fn lemma_w1_s3()
    ensures
        w1_s3().txns == map![0int => w1_r3()],
        w1_s3().shards == seq![ShardState { epoch: 0, counter: 1 }],
        w1_s3().streams == w1_s0().streams,
{
    lemma_w1_s2();
    assert(w1_s3().txns =~= map![0int => w1_r3()]);
    assert(w1_s3().shards =~= seq![ShardState { epoch: 0, counter: 1 }]);
}

pub proof fn lemma_w1_certify()
    ensures enabled(w1_s3(), c1(), w1_a4())
{
    let c = c1();
    lemma_w1_s3();
    let r = w1_r3();
    assert forall|i: int| is_shard(c, i) && writes_at(c, r.body, i) implies #[trigger] r.installed.contains(i) by {
        assert(i == 0);
    }
}

pub open spec fn w1_log() -> Entry { Entry::Log { txn: 0, epoch: 0, clock: 1 } }

pub proof fn lemma_w1_s4()
    ensures
        w1_s4().txns[0].status is Certified,
        w1_s4().txns[0].vc == seq![1int],
        w1_s4().txns[0].epoch == 0,
        w1_s4().txns.dom().contains(0),
        w1_s4().streams == map![sid0() => Stream { durable: Seq::empty(), pending: seq![w1_log()] }],
{
    lemma_w1_s3();
    let c = c1();
    assert(c.cidx[0] == 0);
    assert(log_entry(c, w1_r3(), 0, 0) == w1_log());
    assert(w1_s4().streams =~= map![sid0() => Stream { durable: Seq::empty(), pending: seq![w1_log()] }]);
}

pub proof fn lemma_w1_replicate()
    ensures enabled(w1_s4(), c1(), w1_a5())
{
    lemma_w1_s4();
}

pub proof fn lemma_w1_s5()
    ensures
        w1_s5().txns == w1_s4().txns,
        w1_s5().streams == map![sid0() => Stream { durable: seq![w1_log()], pending: Seq::empty() }],
{
    lemma_w1_s4();
    let st = Stream { durable: Seq::empty(), pending: seq![w1_log()] };
    assert(st.durable.push(st.pending[0]) =~= seq![w1_log()]);
    assert(st.pending.drop_first() =~= Seq::<Entry>::empty());
    assert(w1_s5().streams =~= map![sid0() => Stream { durable: seq![w1_log()], pending: Seq::empty() }]);
}

pub proof fn lemma_w1_commit()
    ensures enabled(w1_s5(), c1(), w1_a6())
{
    let c = c1();
    lemma_w1_s4();
    lemma_w1_s5();
    let s = w1_s5();
    let d = seq![w1_log()];
    assert(d.len() == 1);
    assert(d.last() == w1_log());
    assert(stream_wm(d, 0) == Wm::Fin(1));
    assert(c.cidx[0] == 0);
    assert forall|sid: Sid| valid_sid(c, sid) implies
        wm_le(s.txns[0].vc[group(c, sid.shard)], stream_wm(#[trigger] s.streams[sid].durable, 0)) by {
        assert(sid == sid0());
    }
}

/// Witness 1: a single-shard behavior in which a transaction commits.
pub proof fn witness_single_shard_commit() -> (trace: Seq<State>)
    ensures behavior(trace, c1()), trace.last().txns[0].status is Committed
{
    let c = c1();
    lemma_w1_init();
    lemma_initial_behavior(w1_s0(), c);
    let t = seq![w1_s0()];
    assert(t.last() == w1_s0());
    lemma_w1_submit();
    let t = lemma_extend(t, c, w1_a1());
    lemma_w1_prepare();
    let t = lemma_extend(t, c, w1_a2());
    lemma_w1_install();
    let t = lemma_extend(t, c, w1_a3());
    lemma_w1_certify();
    let t = lemma_extend(t, c, w1_a4());
    lemma_w1_replicate();
    let t = lemma_extend(t, c, w1_a5());
    lemma_w1_commit();
    let t = lemma_extend(t, c, w1_a6());
    assert(t.last() == w1_s6());
    lemma_w1_s5();
    assert(w1_s6().txns[0].status is Committed);
    t
}

// ---------------------------------------------------------------------------
// Witness 2: two shards, one worker thread each. Transaction 0, coordinated by
// shard 0, reads key 0 (Add) at shard 0 and writes keys 0 and 1 at both shards.
// Each shard has two dedicated streams (one per coordinator), and an idle
// stream holds the watermark at 0, so transaction 1, coordinated by shard 1,
// writes key 2 (owned by shard 0) with larger clocks; its install entry at
// shard 0 and its coordinator entry at shard 1 lift the remaining streams.
// ---------------------------------------------------------------------------

pub open spec fn c2() -> Constants { Constants { shards: 2, threads: 1, comp: 2, cidx: seq![0int, 1int] } }
pub open spec fn q00() -> Sid { sid_of(0, 0, 0) }
pub open spec fn q01() -> Sid { sid_of(0, 1, 0) }
pub open spec fn q10() -> Sid { sid_of(1, 0, 0) }
pub open spec fn q11() -> Sid { sid_of(1, 1, 0) }
pub open spec fn streams2(a: Stream, b: Stream, x: Stream, y: Stream) -> Map<Sid, Stream> {
    map![q00() => a, q01() => b, q10() => x, q11() => y]
}
pub open spec fn pend(e: Entry) -> Stream { Stream { durable: Seq::empty(), pending: seq![e] } }
pub open spec fn dur(e: Entry) -> Stream { Stream { durable: seq![e], pending: Seq::empty() } }
pub open spec fn es() -> Stream { empty_stream() }
pub open spec fn sh2(a: int, b: int) -> Seq<ShardState> {
    seq![ShardState { epoch: 0, counter: a }, ShardState { epoch: 0, counter: b }]
}
pub open spec fn lg(txn: int, clock: int) -> Entry { Entry::Log { txn, epoch: 0, clock } }

pub open spec fn rw_body() -> Txn { Txn { ops: map![0int => Op::Add { delta: 1 }, 1int => Op::Put { value: 7 }] } }
pub open spec fn w_body() -> Txn { Txn { ops: map![2int => Op::Put { value: 3 }] } }

pub proof fn lemma_c2()
    ensures
        valid_constants(c2()),
        c2().cidx[0] == 0, c2().cidx[1] == 1,
        owner(c2(), 0) == 0, owner(c2(), 1) == 1, owner(c2(), 2) == 0,
        forall|sid: Sid| #[trigger] valid_sid(c2(), sid) ==> sid == q00() || sid == q01() || sid == q10() || sid == q11(),
        valid_sid(c2(), q00()), valid_sid(c2(), q01()), valid_sid(c2(), q10()), valid_sid(c2(), q11()),
{
    let c = c2();
    assert(c.cidx[0] == 0 && c.cidx[1] == 1);
    assert forall|i: int| 0 <= i < c.shards implies 0 <= #[trigger] c.cidx[i] < c.comp by {
        assert(i == 0 || i == 1);
    }
    assert forall|sid: Sid| #[trigger] valid_sid(c, sid) implies sid == q00() || sid == q01() || sid == q10() || sid == q11() by {
        assert(sid.thread == 0);
        assert(sid.shard == 0 || sid.shard == 1);
        assert(sid.coord == 0 || sid.coord == 1);
    }
}

pub proof fn lemma_rw_body_sets()
    ensures
        rw_body().ops.dom() == set![0int, 1int],
        forall|k: int| #[trigger] read_set(rw_body()).contains(k) <==> k == 0,
        forall|k: int| #[trigger] write_set(rw_body()).contains(k) <==> k == 0 || k == 1,
        !write_set(rw_body()).is_empty(),
        valid_txn(rw_body()),
        writes_at(c2(), rw_body(), 0), writes_at(c2(), rw_body(), 1),
        forall|i: int| #[trigger] writes_at(c2(), rw_body(), i) ==> i == 0 || i == 1,
        keys_at(c2(), rw_body(), 0) == set![0int],
        keys_at(c2(), rw_body(), 1) == set![1int],
{
    let t = rw_body();
    lemma_c2();
    assert(t.ops.dom() =~= set![0int, 1int]);
    assert forall|k: int| #[trigger] read_set(t).contains(k) <==> k == 0 by {
        if k == 0 { assert(t.ops.dom().contains(k)); }
    }
    assert forall|k: int| #[trigger] write_set(t).contains(k) <==> k == 0 || k == 1 by {
        if k == 0 || k == 1 { assert(t.ops.dom().contains(k)); }
    }
    assert(write_set(t).contains(0));
    assert(!Set::<int>::empty().contains(0));
    assert(t.ops.dom().contains(0));
    assert(write_set(t).contains(0) && owner(c2(), 0) == 0);
    assert(write_set(t).contains(1) && owner(c2(), 1) == 1);
    assert forall|i: int| #[trigger] writes_at(c2(), t, i) implies i == 0 || i == 1 by {
        let k = choose|k: int| #[trigger] write_set(t).contains(k) && owner(c2(), k) == i;
    }
    assert(keys_at(c2(), t, 0) =~= set![0int]);
    assert(keys_at(c2(), t, 1) =~= set![1int]);
}

pub proof fn lemma_w_body_sets()
    ensures
        w_body().ops.dom() == set![2int],
        forall|k: int| !(#[trigger] read_set(w_body()).contains(k)),
        forall|k: int| #[trigger] write_set(w_body()).contains(k) <==> k == 2,
        !write_set(w_body()).is_empty(),
        valid_txn(w_body()),
        writes_at(c2(), w_body(), 0), !writes_at(c2(), w_body(), 1),
        forall|i: int| #[trigger] writes_at(c2(), w_body(), i) ==> i == 0,
        keys_at(c2(), w_body(), 0) == set![2int],
{
    let t = w_body();
    lemma_c2();
    assert(t.ops.dom() =~= set![2int]);
    assert forall|k: int| !(#[trigger] read_set(t).contains(k)) by {
        if t.ops.dom().contains(k) { assert(k == 2); }
    }
    assert forall|k: int| #[trigger] write_set(t).contains(k) <==> k == 2 by {
        if k == 2 { assert(t.ops.dom().contains(k)); }
    }
    assert(write_set(t).contains(2));
    assert(!Set::<int>::empty().contains(2));
    assert(t.ops.dom().contains(2));
    assert(write_set(t).contains(2) && owner(c2(), 2) == 0);
    assert forall|i: int| #[trigger] writes_at(c2(), t, i) implies i == 0 by {
        let k = choose|k: int| #[trigger] write_set(t).contains(k) && owner(c2(), k) == i;
    }
    assert(keys_at(c2(), t, 0) =~= set![2int]);
}

pub open spec fn w2_s0() -> State {
    init_state(c2(), streams2(es(), es(), es(), es()))
}

pub proof fn lemma_w2_init()
    ensures init(w2_s0(), c2()), w2_s0().shards == sh2(0, 0)
{
    let c = c2();
    let s = w2_s0();
    lemma_c2();
    assert forall|sid: Sid| #[trigger] s.streams.dom().contains(sid) <==> valid_sid(c, sid) by {
        if valid_sid(c, sid) {}
    }
    assert forall|sid: Sid| valid_sid(c, sid) implies
        #[trigger] s.streams[sid] == Stream { durable: Seq::empty(), pending: Seq::empty() } by {
    }
    assert(s.shards =~= sh2(0, 0));
}

/// The parts of the state the normal-case steps below depend on.
pub open spec fn at(s: State, txns: Map<int, TxnRec>, shards: Seq<ShardState>, streams: Map<Sid, Stream>,
    locks: Map<int, int>, prepared: Seq<int>, tick: nat) -> bool
{
    &&& s.txns == txns
    &&& s.shards == shards
    &&& s.streams == streams
    &&& s.locks == locks
    &&& s.prepared == prepared
    &&& s.tick == tick
}

// Transaction 0's record through its life.
pub open spec fn rd_init() -> ReadRec { ReadRec { writer: -1, epoch: 0, vc: Seq::empty(), value: 0 } }
pub open spec fn t0_1() -> TxnRec {
    TxnRec {
        body: rw_body(), coord: 0, thread: 0, epoch: 0, status: Status::Running,
        reads: Map::empty(), vc: vc_zero(2), pidx: 0, installed: Set::empty(),
        invoked: 0, prepared_at: 0, acked: 0,
    }
}
pub open spec fn t0_2() -> TxnRec { TxnRec { reads: map![0int => rd_init()], ..t0_1() } }
pub open spec fn t0_3() -> TxnRec { TxnRec { status: Status::Prepared, vc: seq![1int, 1int], pidx: 0, prepared_at: 2, ..t0_2() } }
pub open spec fn t0_4() -> TxnRec { TxnRec { installed: set![0int], ..t0_3() } }
pub open spec fn t0_5() -> TxnRec { TxnRec { installed: set![0int, 1int], ..t0_3() } }
pub open spec fn t0_6() -> TxnRec { TxnRec { status: Status::Certified, ..t0_5() } }
// Transaction 1's record.
pub open spec fn t1_1() -> TxnRec {
    TxnRec {
        body: w_body(), coord: 1, thread: 0, epoch: 0, status: Status::Running,
        reads: Map::empty(), vc: vc_zero(2), pidx: 0, installed: Set::empty(),
        invoked: 6, prepared_at: 0, acked: 0,
    }
}
pub open spec fn t1_2() -> TxnRec { TxnRec { status: Status::Prepared, vc: seq![2int, 2int], pidx: 1, prepared_at: 7, ..t1_1() } }
pub open spec fn t1_3() -> TxnRec { TxnRec { installed: set![0int], ..t1_2() } }
pub open spec fn t1_4() -> TxnRec { TxnRec { status: Status::Certified, ..t1_3() } }

pub open spec fn w2_a(n: int) -> Action {
    if n == 1 { Action::Submit { id: 0, body: rw_body(), coord: 0, thread: 0 } }
    else if n == 2 { Action::Read { id: 0, key: 0 } }
    else if n == 3 { Action::Prepare { id: 0, vc: seq![1int, 1int] } }
    else if n == 4 { Action::Install { id: 0, shard: 0 } }
    else if n == 5 { Action::Install { id: 0, shard: 1 } }
    else if n == 6 { Action::Certify { id: 0 } }
    else if n == 7 { Action::Submit { id: 1, body: w_body(), coord: 1, thread: 0 } }
    else if n == 8 { Action::Prepare { id: 1, vc: seq![2int, 2int] } }
    else if n == 9 { Action::Install { id: 1, shard: 0 } }
    else if n == 10 { Action::Certify { id: 1 } }
    else if n == 11 { Action::Replicate { sid: q00() } }
    else if n == 12 { Action::Replicate { sid: q10() } }
    else if n == 13 { Action::Replicate { sid: q01() } }
    else if n == 14 { Action::Replicate { sid: q11() } }
    else { Action::Commit { id: 0 } }
}

pub proof fn lemma_w2_1(s: State)
    requires s == w2_s0()
    ensures enabled(s, c2(), w2_a(1)),
        at(apply(s, c2(), w2_a(1)), map![0int => t0_1()], sh2(0, 0), streams2(es(), es(), es(), es()),
            Map::empty(), Seq::empty(), 1),
        apply(s, c2(), w2_a(1)).versions == Map::<int, Seq<Version>>::empty(),
{
    lemma_rw_body_sets();
    lemma_w2_init();
    let s2 = apply(s, c2(), w2_a(1));
    assert(s2.txns =~= map![0int => t0_1()]);
}

pub proof fn lemma_w2_2(s: State)
    requires at(s, map![0int => t0_1()], sh2(0, 0), streams2(es(), es(), es(), es()), Map::empty(), Seq::empty(), 1),
        s.versions == Map::<int, Seq<Version>>::empty(),
    ensures enabled(s, c2(), w2_a(2)),
        at(apply(s, c2(), w2_a(2)), map![0int => t0_2()], sh2(0, 0), streams2(es(), es(), es(), es()),
            Map::empty(), Seq::empty(), 2),
        apply(s, c2(), w2_a(2)).versions == Map::<int, Seq<Version>>::empty(),
{
    lemma_rw_body_sets();
    lemma_c2();
    let c = c2();
    assert(s.txns[0] == t0_1());
    assert(s.shards[owner(c, 0)].epoch == 0);
    assert(read_rec(s.versions, 0) == rd_init());
    let s2 = apply(s, c, w2_a(2));
    assert(s2.txns =~= map![0int => t0_2()]);
}

pub proof fn lemma_w2_3(s: State)
    requires at(s, map![0int => t0_2()], sh2(0, 0), streams2(es(), es(), es(), es()), Map::empty(), Seq::empty(), 2),
        s.versions == Map::<int, Seq<Version>>::empty(),
    ensures enabled(s, c2(), w2_a(3)),
        at(apply(s, c2(), w2_a(3)), map![0int => t0_3()], sh2(1, 1), streams2(es(), es(), es(), es()),
            map![0int => 0int, 1int => 0int], seq![0int], 3),
{
    lemma_rw_body_sets();
    lemma_c2();
    let c = c2();
    let r = t0_2();
    let vc = seq![1int, 1int];
    assert(s.txns[0] == r);
    assert(r.reads.dom().contains(0));
    assert(r.reads[0] == rd_init());
    assert(s.shards[0] == ShardState { epoch: 0, counter: 0 });
    assert(s.shards[1] == ShardState { epoch: 0, counter: 0 });
    assert(top_writer(s.versions, 0) == -1);
    assert(validated(s, c, r));
    assert(!read_only(r));
    assert(clock_shard(c, r, 0));
    assert(clock_shard(c, r, 1));
    assert(merge_lower_bound(c, r, vc)) by {
        assert forall|k: int| #[trigger] r.reads.dom().contains(k) && r.reads[k].writer != -1 && r.reads[k].epoch == r.epoch
            implies vc_le(r.reads[k].vc, vc, c.comp) by {
            assert(k == 0);
        }
    }
    assert(fetch_lower_bound(s, c, r, vc)) by {
        assert forall|i: int| is_shard(c, i) && #[trigger] clock_shard(c, r, i) implies vc[group(c, i)] >= s.shards[i].counter + 1 by {
            assert(i == 0 || i == 1);
        }
    }
    assert forall|x: int| is_comp(c, x) implies #[trigger] exact_component(s, c, r, vc, x) by {
        if x == 0 {
            assert(is_shard(c, 0) && clock_shard(c, r, 0) && group(c, 0) == x && vc[x] == s.shards[0].counter + 1);
        } else {
            assert(x == 1);
            assert(is_shard(c, 1) && clock_shard(c, r, 1) && group(c, 1) == x && vc[x] == s.shards[1].counter + 1);
        }
    }
    assert(clock_assignment(s, c, r, vc));
    let s2 = apply(s, c, w2_a(3));
    assert(s2.txns =~= map![0int => t0_3()]);
    assert(s2.shards =~= sh2(1, 1));
    assert(s2.locks =~= map![0int => 0int, 1int => 0int]);
    assert(s2.prepared =~= seq![0int]);
}

pub proof fn lemma_w2_4(s: State)
    requires at(s, map![0int => t0_3()], sh2(1, 1), streams2(es(), es(), es(), es()),
        map![0int => 0int, 1int => 0int], seq![0int], 3),
    ensures enabled(s, c2(), w2_a(4)),
        at(apply(s, c2(), w2_a(4)), map![0int => t0_4()], sh2(1, 1), streams2(es(), es(), es(), es()),
            map![1int => 0int], seq![0int], 4),
{
    lemma_rw_body_sets();
    lemma_c2();
    let c = c2();
    assert(s.txns[0] == t0_3());
    let s2 = apply(s, c, w2_a(4));
    assert(s2.txns =~= map![0int => t0_4()]);
    assert(s2.shards =~= sh2(1, 1));
    assert(s2.locks =~= map![1int => 0int]);
}

pub proof fn lemma_w2_5(s: State)
    requires at(s, map![0int => t0_4()], sh2(1, 1), streams2(es(), es(), es(), es()),
        map![1int => 0int], seq![0int], 4),
    ensures enabled(s, c2(), w2_a(5)),
        at(apply(s, c2(), w2_a(5)), map![0int => t0_5()], sh2(1, 1), streams2(es(), es(), pend(lg(0, 1)), es()),
            Map::empty(), seq![0int], 5),
{
    lemma_rw_body_sets();
    lemma_c2();
    let c = c2();
    assert(s.txns[0] == t0_4());
    let s2 = apply(s, c, w2_a(5));
    assert(t0_4().installed.insert(1) =~= set![0int, 1int]);
    assert(s2.txns =~= map![0int => t0_5()]);
    assert(s2.shards =~= sh2(1, 1));
    assert(s2.locks =~= Map::<int, int>::empty());
    assert(log_entry(c, t0_4(), 0, 1) == lg(0, 1));
    assert(s2.streams =~= streams2(es(), es(), pend(lg(0, 1)), es()));
}

pub proof fn lemma_w2_6(s: State)
    requires at(s, map![0int => t0_5()], sh2(1, 1), streams2(es(), es(), pend(lg(0, 1)), es()),
        Map::empty(), seq![0int], 5),
    ensures enabled(s, c2(), w2_a(6)),
        at(apply(s, c2(), w2_a(6)), map![0int => t0_6()], sh2(1, 1),
            streams2(pend(lg(0, 1)), es(), pend(lg(0, 1)), es()), Map::empty(), seq![0int], 6),
{
    lemma_rw_body_sets();
    lemma_c2();
    let c = c2();
    let r = t0_5();
    assert(s.txns[0] == r);
    assert forall|i: int| is_shard(c, i) && writes_at(c, r.body, i) implies #[trigger] r.installed.contains(i) by {
        assert(i == 0 || i == 1);
    }
    let s2 = apply(s, c, w2_a(6));
    assert(s2.txns =~= map![0int => t0_6()]);
    assert(s2.shards =~= sh2(1, 1));
    assert(log_entry(c, r, 0, 0) == lg(0, 1));
    assert(s2.streams =~= streams2(pend(lg(0, 1)), es(), pend(lg(0, 1)), es()));
}

pub proof fn lemma_w2_7(s: State)
    requires at(s, map![0int => t0_6()], sh2(1, 1), streams2(pend(lg(0, 1)), es(), pend(lg(0, 1)), es()),
        Map::empty(), seq![0int], 6),
    ensures enabled(s, c2(), w2_a(7)),
        at(apply(s, c2(), w2_a(7)), map![0int => t0_6(), 1int => t1_1()], sh2(1, 1),
            streams2(pend(lg(0, 1)), es(), pend(lg(0, 1)), es()), Map::empty(), seq![0int], 7),
{
    lemma_w_body_sets();
    lemma_c2();
    let c = c2();
    assert forall|o: int| #[trigger] s.txns.dom().contains(o) && s.txns[o].coord == 1 && s.txns[o].thread == 0
        implies !in_flight(s.txns[o]) by {
        assert(o == 0);
    }
    let s2 = apply(s, c, w2_a(7));
    assert(s2.txns =~= map![0int => t0_6(), 1int => t1_1()]);
}

pub proof fn lemma_w2_8(s: State)
    requires at(s, map![0int => t0_6(), 1int => t1_1()], sh2(1, 1),
        streams2(pend(lg(0, 1)), es(), pend(lg(0, 1)), es()), Map::empty(), seq![0int], 7),
    ensures enabled(s, c2(), w2_a(8)),
        at(apply(s, c2(), w2_a(8)), map![0int => t0_6(), 1int => t1_2()], sh2(2, 2),
            streams2(pend(lg(0, 1)), es(), pend(lg(0, 1)), es()), map![2int => 1int], seq![0int, 1int], 8),
{
    lemma_w_body_sets();
    lemma_c2();
    let c = c2();
    let r = t1_1();
    let vc = seq![2int, 2int];
    assert(s.txns[1] == r);
    assert(s.shards[0] == ShardState { epoch: 0, counter: 1 });
    assert(s.shards[1] == ShardState { epoch: 0, counter: 1 });
    assert(validated(s, c, r));
    assert(!read_only(r));
    assert(clock_shard(c, r, 0));
    assert(clock_shard(c, r, 1));
    assert(fetch_lower_bound(s, c, r, vc)) by {
        assert forall|i: int| is_shard(c, i) && #[trigger] clock_shard(c, r, i) implies vc[group(c, i)] >= s.shards[i].counter + 1 by {
            assert(i == 0 || i == 1);
        }
    }
    assert forall|x: int| is_comp(c, x) implies #[trigger] exact_component(s, c, r, vc, x) by {
        if x == 0 {
            assert(is_shard(c, 0) && clock_shard(c, r, 0) && group(c, 0) == x && vc[x] == s.shards[0].counter + 1);
        } else {
            assert(x == 1);
            assert(is_shard(c, 1) && clock_shard(c, r, 1) && group(c, 1) == x && vc[x] == s.shards[1].counter + 1);
        }
    }
    assert(clock_assignment(s, c, r, vc));
    let s2 = apply(s, c, w2_a(8));
    assert(s2.txns =~= map![0int => t0_6(), 1int => t1_2()]);
    assert(s2.shards =~= sh2(2, 2));
    assert(s2.locks =~= map![2int => 1int]);
    assert(s2.prepared =~= seq![0int, 1int]);
}

pub proof fn lemma_w2_9(s: State)
    requires at(s, map![0int => t0_6(), 1int => t1_2()], sh2(2, 2),
        streams2(pend(lg(0, 1)), es(), pend(lg(0, 1)), es()), map![2int => 1int], seq![0int, 1int], 8),
    ensures enabled(s, c2(), w2_a(9)),
        at(apply(s, c2(), w2_a(9)), map![0int => t0_6(), 1int => t1_3()], sh2(2, 2),
            streams2(pend(lg(0, 1)), pend(lg(1, 2)), pend(lg(0, 1)), es()), Map::empty(), seq![0int, 1int], 9),
{
    lemma_w_body_sets();
    lemma_c2();
    let c = c2();
    assert(s.txns[1] == t1_2());
    let s2 = apply(s, c, w2_a(9));
    assert(s2.txns =~= map![0int => t0_6(), 1int => t1_3()]);
    assert(s2.shards =~= sh2(2, 2));
    assert(s2.locks =~= Map::<int, int>::empty());
    assert(log_entry(c, t1_2(), 1, 0) == lg(1, 2));
    assert(s2.streams =~= streams2(pend(lg(0, 1)), pend(lg(1, 2)), pend(lg(0, 1)), es()));
}

pub proof fn lemma_w2_10(s: State)
    requires at(s, map![0int => t0_6(), 1int => t1_3()], sh2(2, 2),
        streams2(pend(lg(0, 1)), pend(lg(1, 2)), pend(lg(0, 1)), es()), Map::empty(), seq![0int, 1int], 9),
    ensures enabled(s, c2(), w2_a(10)),
        at(apply(s, c2(), w2_a(10)), map![0int => t0_6(), 1int => t1_4()], sh2(2, 2),
            streams2(pend(lg(0, 1)), pend(lg(1, 2)), pend(lg(0, 1)), pend(lg(1, 2))), Map::empty(), seq![0int, 1int], 10),
{
    lemma_w_body_sets();
    lemma_c2();
    let c = c2();
    let r = t1_3();
    assert(s.txns[1] == r);
    assert forall|i: int| is_shard(c, i) && writes_at(c, r.body, i) implies #[trigger] r.installed.contains(i) by {
        assert(i == 0);
    }
    let s2 = apply(s, c, w2_a(10));
    assert(s2.txns =~= map![0int => t0_6(), 1int => t1_4()]);
    assert(s2.shards =~= sh2(2, 2));
    assert(log_entry(c, r, 1, 1) == lg(1, 2));
    assert(s2.streams =~= streams2(pend(lg(0, 1)), pend(lg(1, 2)), pend(lg(0, 1)), pend(lg(1, 2))));
}

/// Replicating a stream whose only entry is pending.
pub proof fn lemma_w2_replicate_one(st: Stream, e: Entry)
    requires st == pend(e)
    ensures (Stream { durable: st.durable.push(st.pending[0]), pending: st.pending.drop_first() }) == dur(e)
{
    assert(st.durable.push(st.pending[0]) =~= seq![e]);
    assert(st.pending.drop_first() =~= Seq::<Entry>::empty());
}

pub proof fn lemma_w2_11(s: State)
    requires at(s, map![0int => t0_6(), 1int => t1_4()], sh2(2, 2),
        streams2(pend(lg(0, 1)), pend(lg(1, 2)), pend(lg(0, 1)), pend(lg(1, 2))), Map::empty(), seq![0int, 1int], 10),
    ensures enabled(s, c2(), w2_a(11)),
        apply(s, c2(), w2_a(11)).txns == s.txns,
        apply(s, c2(), w2_a(11)).streams == streams2(dur(lg(0, 1)), pend(lg(1, 2)), pend(lg(0, 1)), pend(lg(1, 2))),
{
    lemma_c2();
    lemma_w2_replicate_one(s.streams[q00()], lg(0, 1));
    assert(apply(s, c2(), w2_a(11)).streams =~= streams2(dur(lg(0, 1)), pend(lg(1, 2)), pend(lg(0, 1)), pend(lg(1, 2))));
}

pub proof fn lemma_w2_12(s: State)
    requires s.txns == map![0int => t0_6(), 1int => t1_4()],
        s.streams == streams2(dur(lg(0, 1)), pend(lg(1, 2)), pend(lg(0, 1)), pend(lg(1, 2))),
    ensures enabled(s, c2(), w2_a(12)),
        apply(s, c2(), w2_a(12)).txns == s.txns,
        apply(s, c2(), w2_a(12)).streams == streams2(dur(lg(0, 1)), pend(lg(1, 2)), dur(lg(0, 1)), pend(lg(1, 2))),
{
    lemma_c2();
    lemma_w2_replicate_one(s.streams[q10()], lg(0, 1));
    assert(apply(s, c2(), w2_a(12)).streams =~= streams2(dur(lg(0, 1)), pend(lg(1, 2)), dur(lg(0, 1)), pend(lg(1, 2))));
}

pub proof fn lemma_w2_13(s: State)
    requires s.txns == map![0int => t0_6(), 1int => t1_4()],
        s.streams == streams2(dur(lg(0, 1)), pend(lg(1, 2)), dur(lg(0, 1)), pend(lg(1, 2))),
    ensures enabled(s, c2(), w2_a(13)),
        apply(s, c2(), w2_a(13)).txns == s.txns,
        apply(s, c2(), w2_a(13)).streams == streams2(dur(lg(0, 1)), dur(lg(1, 2)), dur(lg(0, 1)), pend(lg(1, 2))),
{
    lemma_c2();
    lemma_w2_replicate_one(s.streams[q01()], lg(1, 2));
    assert(apply(s, c2(), w2_a(13)).streams =~= streams2(dur(lg(0, 1)), dur(lg(1, 2)), dur(lg(0, 1)), pend(lg(1, 2))));
}

pub proof fn lemma_w2_14(s: State)
    requires s.txns == map![0int => t0_6(), 1int => t1_4()],
        s.streams == streams2(dur(lg(0, 1)), dur(lg(1, 2)), dur(lg(0, 1)), pend(lg(1, 2))),
    ensures enabled(s, c2(), w2_a(14)),
        apply(s, c2(), w2_a(14)).txns == s.txns,
        apply(s, c2(), w2_a(14)).streams == streams2(dur(lg(0, 1)), dur(lg(1, 2)), dur(lg(0, 1)), dur(lg(1, 2))),
{
    lemma_c2();
    lemma_w2_replicate_one(s.streams[q11()], lg(1, 2));
    assert(apply(s, c2(), w2_a(14)).streams =~= streams2(dur(lg(0, 1)), dur(lg(1, 2)), dur(lg(0, 1)), dur(lg(1, 2))));
}

pub proof fn lemma_wm_single(e: Entry)
    requires e is Log, e->Log_epoch == 0
    ensures stream_wm(dur(e).durable, 0) == Wm::Fin(e->Log_clock)
{
    let d = dur(e).durable;
    assert(d.len() == 1);
    assert(d.last() == e);
}

pub proof fn lemma_w2_15(s: State)
    requires s.txns == map![0int => t0_6(), 1int => t1_4()],
        s.streams == streams2(dur(lg(0, 1)), dur(lg(1, 2)), dur(lg(0, 1)), dur(lg(1, 2))),
    ensures enabled(s, c2(), w2_a(15)),
        apply(s, c2(), w2_a(15)).txns[0].status is Committed,
        apply(s, c2(), w2_a(15)).txns[0].body == rw_body(),
{
    lemma_c2();
    let c = c2();
    let r = t0_6();
    assert(s.txns[0] == r);
    lemma_wm_single(lg(0, 1));
    lemma_wm_single(lg(1, 2));
    assert forall|sid: Sid| valid_sid(c, sid) implies
        wm_le(r.vc[group(c, sid.shard)], stream_wm(#[trigger] s.streams[sid].durable, r.epoch)) by {
        if sid == q00() {} else if sid == q01() {} else if sid == q10() {} else { assert(sid == q11()); }
    }
    assert(below_wm(s.streams, c, r.vc, r.epoch));
}

/// Witness 2: a two-shard behavior in which a cross-shard read-write
/// transaction (it reads key 0 at shard 0 and writes keys 0 and 1 at shards 0
/// and 1) commits.
pub proof fn witness_two_shard_commit() -> (trace: Seq<State>)
    ensures
        behavior(trace, c2()),
        trace.last().txns[0].status is Committed,
        read_set(trace.last().txns[0].body).contains(0),
        write_set(trace.last().txns[0].body).contains(0),
        write_set(trace.last().txns[0].body).contains(1),
        owner(c2(), 0) == 0 && owner(c2(), 1) == 1,
{
    let c = c2();
    lemma_w2_init();
    lemma_rw_body_sets();
    lemma_c2();
    lemma_initial_behavior(w2_s0(), c);
    let t = seq![w2_s0()];
    assert(t.last() == w2_s0());
    lemma_w2_1(t.last());
    let t = lemma_extend(t, c, w2_a(1));
    lemma_w2_2(t.last());
    let t = lemma_extend(t, c, w2_a(2));
    lemma_w2_3(t.last());
    let t = lemma_extend(t, c, w2_a(3));
    lemma_w2_4(t.last());
    let t = lemma_extend(t, c, w2_a(4));
    lemma_w2_5(t.last());
    let t = lemma_extend(t, c, w2_a(5));
    lemma_w2_6(t.last());
    let t = lemma_extend(t, c, w2_a(6));
    lemma_w2_7(t.last());
    let t = lemma_extend(t, c, w2_a(7));
    lemma_w2_8(t.last());
    let t = lemma_extend(t, c, w2_a(8));
    lemma_w2_9(t.last());
    let t = lemma_extend(t, c, w2_a(9));
    lemma_w2_10(t.last());
    let t = lemma_extend(t, c, w2_a(10));
    lemma_w2_11(t.last());
    let t = lemma_extend(t, c, w2_a(11));
    lemma_w2_12(t.last());
    let t = lemma_extend(t, c, w2_a(12));
    lemma_w2_13(t.last());
    let t = lemma_extend(t, c, w2_a(13));
    lemma_w2_14(t.last());
    let t = lemma_extend(t, c, w2_a(14));
    lemma_w2_15(t.last());
    let t = lemma_extend(t, c, w2_a(15));
    t
}

// ---------------------------------------------------------------------------
// Witness 3: a crash dooms a certified transaction. Transaction 0,
// coordinated by shard 0, writes keys 0 and 1 (one per shard) and certifies;
// its coordinator entry is chosen later, but the leader of shard 1 fails
// before the install entry on stream (1, 0, 0) is chosen, so that entry and
// the version of key 1 are lost. Shard 0 then closes epoch 0 healthily (INF
// on both of its streams), both shards finalize their epoch-0 watermarks
// (INF and 0), the transaction is doomed by the FVW, and Rollback at shard 0
// removes its surviving version of key 0.
// ---------------------------------------------------------------------------

pub open spec fn body3() -> Txn { Txn { ops: map![0int => Op::Put { value: 7 }, 1int => Op::Put { value: 8 }] } }

pub proof fn lemma_body3_sets()
    ensures
        forall|k: int| !(#[trigger] read_set(body3()).contains(k)),
        forall|k: int| #[trigger] write_set(body3()).contains(k) <==> k == 0 || k == 1,
        !write_set(body3()).is_empty(),
        valid_txn(body3()),
        writes_at(c2(), body3(), 0), writes_at(c2(), body3(), 1),
        forall|i: int| #[trigger] writes_at(c2(), body3(), i) ==> i == 0 || i == 1,
        keys_at(c2(), body3(), 0) == set![0int],
        keys_at(c2(), body3(), 1) == set![1int],
{
    let t = body3();
    lemma_c2();
    assert(t.ops.dom() =~= set![0int, 1int]);
    assert forall|k: int| !(#[trigger] read_set(t).contains(k)) by {
        if t.ops.dom().contains(k) { assert(k == 0 || k == 1); }
    }
    assert forall|k: int| #[trigger] write_set(t).contains(k) <==> k == 0 || k == 1 by {
        if k == 0 || k == 1 { assert(t.ops.dom().contains(k)); }
    }
    assert(write_set(t).contains(0));
    assert(!Set::<int>::empty().contains(0));
    assert(t.ops.dom().contains(0));
    assert(write_set(t).contains(0) && owner(c2(), 0) == 0);
    assert(write_set(t).contains(1) && owner(c2(), 1) == 1);
    assert forall|i: int| #[trigger] writes_at(c2(), t, i) implies i == 0 || i == 1 by {
        let k = choose|k: int| #[trigger] write_set(t).contains(k) && owner(c2(), k) == i;
    }
    assert(keys_at(c2(), t, 0) =~= set![0int]);
    assert(keys_at(c2(), t, 1) =~= set![1int]);
}

pub open spec fn sh(e0: nat, c0: int, e1: nat, c1: int) -> Seq<ShardState> {
    seq![ShardState { epoch: e0, counter: c0 }, ShardState { epoch: e1, counter: c1 }]
}
pub open spec fn inf0() -> Entry { Entry::Inf { epoch: 0 } }
pub open spec fn v3(value: int) -> Version { Version { txn: 0, epoch: 0, vc: seq![1int, 1int], value } }

pub open spec fn u_1() -> TxnRec {
    TxnRec {
        body: body3(), coord: 0, thread: 0, epoch: 0, status: Status::Running,
        reads: Map::empty(), vc: vc_zero(2), pidx: 0, installed: Set::empty(),
        invoked: 0, prepared_at: 0, acked: 0,
    }
}
pub open spec fn u_2() -> TxnRec { TxnRec { status: Status::Prepared, vc: seq![1int, 1int], pidx: 0, prepared_at: 1, ..u_1() } }
pub open spec fn u_3() -> TxnRec { TxnRec { installed: set![0int], ..u_2() } }
pub open spec fn u_4() -> TxnRec { TxnRec { installed: set![0int, 1int], ..u_2() } }
pub open spec fn u_5() -> TxnRec { TxnRec { status: Status::Certified, ..u_4() } }

pub open spec fn survive1() -> Map<Sid, nat> { map![q10() => 0nat, q11() => 0nat] }

pub open spec fn w3_a(n: int) -> Action {
    if n == 1 { Action::Submit { id: 0, body: body3(), coord: 0, thread: 0 } }
    else if n == 2 { Action::Prepare { id: 0, vc: seq![1int, 1int] } }
    else if n == 3 { Action::Install { id: 0, shard: 0 } }
    else if n == 4 { Action::Install { id: 0, shard: 1 } }
    else if n == 5 { Action::Certify { id: 0 } }
    else if n == 6 { Action::Crash { shard: 1, survive: survive1() } }
    else if n == 7 { Action::AdvanceEpoch { shard: 0 } }
    else if n == 8 { Action::Replicate { sid: q00() } }
    else if n == 9 { Action::Replicate { sid: q00() } }
    else if n == 10 { Action::Replicate { sid: q01() } }
    else if n == 11 { Action::CloseEpoch { shard: 0, epoch: 0, wm: Wm::Inf } }
    else if n == 12 { Action::CloseEpoch { shard: 1, epoch: 0, wm: Wm::Fin(0) } }
    else { Action::Rollback { shard: 0, epoch: 0 } }
}

// The concrete states of witness 3.
pub open spec fn e3_0() -> State {
    State {
        epoch: 0,
        shards: sh(0, 0, 0, 0),
        streams: streams2(es(), es(), es(), es()),
        versions: Map::<int, Seq<Version>>::empty(),
        locks: Map::<int, int>::empty(),
        txns: Map::<int, TxnRec>::empty(),
        final_wm: Map::<(int, nat), Wm>::empty(),
        rolled_back: Set::<(int, nat)>::empty(),
        prepared: Seq::<int>::empty(),
        tick: 0,
    }
}
pub open spec fn e3_1() -> State { State { txns: map![0int => u_1()], tick: 1, ..e3_0() } }
pub open spec fn e3_2() -> State {
    State { txns: map![0int => u_2()], shards: sh(0, 1, 0, 1), locks: map![0int => 0int, 1int => 0int],
        prepared: seq![0int], tick: 2, ..e3_1() }
}
pub open spec fn e3_3() -> State {
    State { txns: map![0int => u_3()], versions: map![0int => seq![v3(7)]], locks: map![1int => 0int], tick: 3, ..e3_2() }
}
pub open spec fn e3_4() -> State {
    State { txns: map![0int => u_4()], versions: map![0int => seq![v3(7)], 1int => seq![v3(8)]],
        locks: Map::<int, int>::empty(), streams: streams2(es(), es(), pend(lg(0, 1)), es()), tick: 4, ..e3_3() }
}
pub open spec fn e3_5() -> State {
    State { txns: map![0int => u_5()], streams: streams2(pend(lg(0, 1)), es(), pend(lg(0, 1)), es()), tick: 5, ..e3_4() }
}
pub open spec fn e3_6() -> State {
    State { epoch: 1, shards: sh(0, 1, 1, 0), streams: streams2(pend(lg(0, 1)), es(), es(), es()),
        versions: map![0int => seq![v3(7)], 1int => Seq::<Version>::empty()], tick: 6, ..e3_5() }
}
pub open spec fn e3_7() -> State {
    State { shards: sh(1, 0, 1, 0),
        streams: streams2(Stream { durable: Seq::empty(), pending: seq![lg(0, 1), inf0()] }, pend(inf0()), es(), es()),
        tick: 7, ..e3_6() }
}
pub open spec fn e3_8() -> State {
    State { streams: streams2(Stream { durable: seq![lg(0, 1)], pending: seq![inf0()] }, pend(inf0()), es(), es()),
        tick: 8, ..e3_7() }
}
pub open spec fn e3_9() -> State {
    State { streams: streams2(Stream { durable: seq![lg(0, 1), inf0()], pending: Seq::empty() }, pend(inf0()), es(), es()),
        tick: 9, ..e3_8() }
}
pub open spec fn e3_10() -> State {
    State { streams: streams2(Stream { durable: seq![lg(0, 1), inf0()], pending: Seq::empty() }, dur(inf0()), es(), es()),
        tick: 10, ..e3_9() }
}
pub open spec fn e3_11() -> State { State { final_wm: map![(0int, 0nat) => Wm::Inf], tick: 11, ..e3_10() } }
pub open spec fn e3_12() -> State {
    State { final_wm: map![(0int, 0nat) => Wm::Inf, (1int, 0nat) => Wm::Fin(0)], tick: 12, ..e3_11() }
}
pub open spec fn e3_13() -> State {
    State { versions: map![0int => Seq::<Version>::empty(), 1int => Seq::<Version>::empty()],
        rolled_back: set![(0int, 0nat)], tick: 13, ..e3_12() }
}

pub proof fn lemma_w3_init()
    ensures init(e3_0(), c2())
{
    let c = c2();
    let s = e3_0();
    lemma_c2();
    assert forall|sid: Sid| #[trigger] s.streams.dom().contains(sid) <==> valid_sid(c, sid) by {
        if valid_sid(c, sid) {}
    }
    assert forall|sid: Sid| valid_sid(c, sid) implies
        #[trigger] s.streams[sid] == Stream { durable: Seq::empty(), pending: Seq::empty() } by {
    }
    assert(s.shards =~= Seq::new(c.shards as nat, |i: int| ShardState { epoch: 0, counter: 0 }));
}

pub proof fn lemma_w3_1()
    ensures enabled(e3_0(), c2(), w3_a(1)), apply(e3_0(), c2(), w3_a(1)) == e3_1()
{
    lemma_body3_sets();
    lemma_c2();
    assert(e3_0().shards[0].epoch == 0);
    assert(apply(e3_0(), c2(), w3_a(1)).txns =~= e3_1().txns);
}

pub proof fn lemma_w3_2()
    ensures enabled(e3_1(), c2(), w3_a(2)), apply(e3_1(), c2(), w3_a(2)) == e3_2()
{
    lemma_body3_sets();
    lemma_c2();
    let c = c2();
    let s = e3_1();
    let r = u_1();
    let vc = seq![1int, 1int];
    assert(s.txns[0] == r);
    assert(s.shards[0] == ShardState { epoch: 0, counter: 0 });
    assert(s.shards[1] == ShardState { epoch: 0, counter: 0 });
    assert(validated(s, c, r));
    assert(!read_only(r));
    assert(clock_shard(c, r, 0));
    assert(clock_shard(c, r, 1));
    assert(fetch_lower_bound(s, c, r, vc)) by {
        assert forall|i: int| is_shard(c, i) && #[trigger] clock_shard(c, r, i) implies vc[group(c, i)] >= s.shards[i].counter + 1 by {
            assert(i == 0 || i == 1);
        }
    }
    assert forall|x: int| is_comp(c, x) implies #[trigger] exact_component(s, c, r, vc, x) by {
        if x == 0 {
            assert(is_shard(c, 0) && clock_shard(c, r, 0) && group(c, 0) == x && vc[x] == s.shards[0].counter + 1);
        } else {
            assert(x == 1);
            assert(is_shard(c, 1) && clock_shard(c, r, 1) && group(c, 1) == x && vc[x] == s.shards[1].counter + 1);
        }
    }
    assert(clock_assignment(s, c, r, vc));
    let s2 = apply(s, c, w3_a(2));
    assert(s2.txns =~= e3_2().txns);
    assert(s2.shards =~= e3_2().shards);
    assert(s2.locks =~= e3_2().locks);
    assert(s2.prepared =~= e3_2().prepared);
}

pub proof fn lemma_w3_3()
    ensures enabled(e3_2(), c2(), w3_a(3)), apply(e3_2(), c2(), w3_a(3)) == e3_3()
{
    lemma_body3_sets();
    lemma_c2();
    let c = c2();
    let s = e3_2();
    let r = u_2();
    assert(s.txns[0] == r);
    assert(new_version(r, 0, 0) == v3(7));
    let s2 = apply(s, c, w3_a(3));
    assert(s2.txns =~= e3_3().txns);
    assert(s2.shards =~= e3_3().shards);
    assert(s2.locks =~= e3_3().locks);
    assert(s2.versions =~= e3_3().versions);
}

pub proof fn lemma_w3_4()
    ensures enabled(e3_3(), c2(), w3_a(4)), apply(e3_3(), c2(), w3_a(4)) == e3_4()
{
    lemma_body3_sets();
    lemma_c2();
    let c = c2();
    let s = e3_3();
    let r = u_3();
    assert(s.txns[0] == r);
    assert(new_version(r, 0, 1) == v3(8));
    let s2 = apply(s, c, w3_a(4));
    assert(r.installed.insert(1) =~= set![0int, 1int]);
    assert(s2.txns =~= e3_4().txns);
    assert(s2.shards =~= e3_4().shards);
    assert(s2.locks =~= e3_4().locks);
    assert(s2.versions =~= e3_4().versions);
    assert(log_entry(c, r, 0, 1) == lg(0, 1));
    assert(s2.streams =~= e3_4().streams);
}

pub proof fn lemma_w3_5()
    ensures enabled(e3_4(), c2(), w3_a(5)), apply(e3_4(), c2(), w3_a(5)) == e3_5()
{
    lemma_body3_sets();
    lemma_c2();
    let c = c2();
    let s = e3_4();
    let r = u_4();
    assert(s.txns[0] == r);
    assert forall|i: int| is_shard(c, i) && writes_at(c, r.body, i) implies #[trigger] r.installed.contains(i) by {
        assert(i == 0 || i == 1);
    }
    let s2 = apply(s, c, w3_a(5));
    assert(s2.txns =~= e3_5().txns);
    assert(s2.shards =~= e3_5().shards);
    assert(log_entry(c, r, 0, 0) == lg(0, 1));
    assert(s2.streams =~= e3_5().streams);
}

pub proof fn lemma_w3_6()
    ensures enabled(e3_5(), c2(), w3_a(6)), apply(e3_5(), c2(), w3_a(6)) == e3_6()
{
    lemma_c2();
    let c = c2();
    let s = e3_5();
    let sv = survive1();
    assert forall|sid: Sid| valid_sid(c, sid) && sid.shard == 1 implies
        #[trigger] sv.dom().contains(sid) && sv[sid] <= s.streams[sid].pending.len() by {
        assert(sid == q10() || sid == q11());
    }
    assert(can_crash(s, c, 1, sv));
    let streams = crash_streams(s, 1, sv);
    let e6 = e3_6();
    assert(pend(lg(0, 1)).durable + pend(lg(0, 1)).pending.take(0) =~= Seq::<Entry>::empty());
    assert(es().durable + es().pending.take(0) =~= Seq::<Entry>::empty());
    assert(streams =~= e6.streams);
    // the version of key 1 at the failed shard is not backed by a chosen entry
    let v = v3(8);
    assert(!survives_crash(s, streams, 1, v)) by {
        assert(streams[sid_of(1, 0, 0)].durable.len() == 0);
    }
    assert(seq![v].filter(|v: Version| survives_crash(s, streams, 1, v)) =~= Seq::<Version>::empty()) by {
        reveal_with_fuel(Seq::<_>::filter, 2);
        assert(seq![v].drop_last() =~= Seq::<Version>::empty());
    }
    let s2 = apply(s, c, w3_a(6));
    assert(s2.versions =~= e6.versions);
    assert(s2.locks =~= e6.locks);
    assert(s2.txns =~= e6.txns);
    assert(s2.shards =~= e6.shards);
}

pub proof fn lemma_w3_7()
    ensures enabled(e3_6(), c2(), w3_a(7)), apply(e3_6(), c2(), w3_a(7)) == e3_7()
{
    lemma_c2();
    let c = c2();
    let s = e3_6();
    assert(s.shards[0].epoch == 0);
    assert(!hung_thread(s, 0, 0)) by {
        if hung_thread(s, 0, 0) {
            let id = choose|id: int| #[trigger] s.txns.dom().contains(id)
                && s.txns[id].coord == 0 && s.txns[id].thread == 0 && s.txns[id].status is Prepared;
            assert(id == 0);
        }
    }
    let s2 = apply(s, c, w3_a(7));
    assert(s.streams[q00()].pending.push(inf0()) =~= seq![lg(0, 1), inf0()]);
    assert(s.streams[q01()].pending.push(inf0()) =~= seq![inf0()]);
    assert(s2.streams =~= e3_7().streams);
    assert(s2.shards =~= e3_7().shards);
    assert(s2.locks =~= e3_7().locks);
    assert(s2.txns =~= e3_7().txns);
}

pub proof fn lemma_w3_8()
    ensures enabled(e3_7(), c2(), w3_a(8)), apply(e3_7(), c2(), w3_a(8)) == e3_8()
{
    lemma_c2();
    let s = e3_7();
    let st = s.streams[q00()];
    assert(st.durable.push(st.pending[0]) =~= seq![lg(0, 1)]);
    assert(st.pending.drop_first() =~= seq![inf0()]);
    assert(apply(s, c2(), w3_a(8)).streams =~= e3_8().streams);
}

pub proof fn lemma_w3_9()
    ensures enabled(e3_8(), c2(), w3_a(9)), apply(e3_8(), c2(), w3_a(9)) == e3_9()
{
    lemma_c2();
    let s = e3_8();
    let st = s.streams[q00()];
    assert(st.durable.push(st.pending[0]) =~= seq![lg(0, 1), inf0()]);
    assert(st.pending.drop_first() =~= Seq::<Entry>::empty());
    assert(apply(s, c2(), w3_a(9)).streams =~= e3_9().streams);
}

pub proof fn lemma_w3_10()
    ensures enabled(e3_9(), c2(), w3_a(10)), apply(e3_9(), c2(), w3_a(10)) == e3_10()
{
    lemma_c2();
    let s = e3_9();
    lemma_w2_replicate_one(s.streams[q01()], inf0());
    assert(apply(s, c2(), w3_a(10)).streams =~= e3_10().streams);
}

pub proof fn lemma_w3_11()
    ensures enabled(e3_10(), c2(), w3_a(11)), apply(e3_10(), c2(), w3_a(11)) == e3_11()
{
    lemma_c2();
    let c = c2();
    let s = e3_10();
    let d0 = seq![lg(0, 1), inf0()];
    assert(d0.last() == inf0());
    assert(stream_wm(d0, 0) == Wm::Inf);
    assert(seq![inf0()].last() == inf0());
    assert(stream_wm(seq![inf0()], 0) == Wm::Inf);
    assert(s.streams[q00()].durable == d0);
    assert(s.streams[q01()].durable == seq![inf0()]);
    assert forall|sid: Sid| valid_sid(c, sid) && sid.shard == 0 implies
        no_pending_epoch(#[trigger] s.streams[sid], 0) && wm_le_wm(Wm::Inf, stream_wm(s.streams[sid].durable, 0)) by {
        assert(sid == q00() || sid == q01());
    }
    assert(valid_sid(c, q00()) && q00().shard == 0 && Wm::Inf == stream_wm(s.streams[q00()].durable, 0));
    assert(can_close(s, c, 0, 0, Wm::Inf));
    assert(apply(s, c, w3_a(11)).final_wm =~= e3_11().final_wm);
}

pub proof fn lemma_w3_12()
    ensures enabled(e3_11(), c2(), w3_a(12)), apply(e3_11(), c2(), w3_a(12)) == e3_12()
{
    lemma_c2();
    let c = c2();
    let s = e3_11();
    assert(stream_wm(Seq::<Entry>::empty(), 0) == Wm::Fin(0));
    assert(!s.final_wm.dom().contains((1int, 0nat)));
    assert forall|sid: Sid| valid_sid(c, sid) && sid.shard == 1 implies
        no_pending_epoch(#[trigger] s.streams[sid], 0) && wm_le_wm(Wm::Fin(0), stream_wm(s.streams[sid].durable, 0)) by {
        assert(sid == q10() || sid == q11());
        assert(s.streams[sid] == es());
    }
    assert(valid_sid(c, q10()) && q10().shard == 1 && Wm::Fin(0) == stream_wm(s.streams[q10()].durable, 0));
    assert(can_close(s, c, 1, 0, Wm::Fin(0)));
    assert(apply(s, c, w3_a(12)).final_wm =~= e3_12().final_wm);
}

pub proof fn lemma_w3_fw()
    ensures
        fvw_ready(e3_12().final_wm, c2(), 0),
        !below_fvw(e3_12().final_wm, c2(), seq![1int, 1int], 0),
{
    lemma_c2();
    let fw = e3_12().final_wm;
    assert forall|i: int| is_shard(c2(), i) implies #[trigger] fw.dom().contains((i, 0nat)) by {
        assert(i == 0 || i == 1);
    }
    assert(fw[(1int, 0nat)] == Wm::Fin(0));
    assert(!wm_le(seq![1int, 1int][group(c2(), 1)], fw[(1int, 0nat)]));
}

pub proof fn lemma_w3_13()
    ensures enabled(e3_12(), c2(), w3_a(13)), apply(e3_12(), c2(), w3_a(13)) == e3_13()
{
    lemma_c2();
    lemma_w3_fw();
    let c = c2();
    let s = e3_12();
    let v = v3(7);
    assert(!keep_after_rollback(s, c, 0, v));
    assert(seq![v].filter(|v: Version| keep_after_rollback(s, c, 0, v)) =~= Seq::<Version>::empty()) by {
        reveal_with_fuel(Seq::<_>::filter, 2);
        assert(seq![v].drop_last() =~= Seq::<Version>::empty());
    }
    let s2 = apply(s, c, w3_a(13));
    assert(s2.versions =~= e3_13().versions);
    assert(s2.rolled_back =~= e3_13().rolled_back);
}

/// Witness 3: a behavior with a leader failure in which a certified
/// transaction whose install entry at shard 1 was never chosen is doomed by the
/// epoch's finalized vector watermark, and Rollback discards its version at
/// shard 0 (the version of key 1 was already lost with the failed leader).
pub proof fn witness_crash_rollback() -> (trace: Seq<State>)
    ensures
        behavior(trace, c2()),
        trace.len() == 14,
        trace[5].txns[0].status is Certified,
        has_log(stream_at(trace[5].streams, trace[5].txns[0], 1), 0),
        !durable_has_log(trace[5].streams[sid_of(1, 0, 0)], 0),
        !has_log(stream_at(trace[6].streams, trace[6].txns[0], 1), 0),
        has_version(trace[5].versions, 1, 0),
        !has_version(trace[6].versions, 1, 0),
        has_version(trace[12].versions, 0, 0),
        trace.last().txns[0].status is Certified,
        doomed(trace.last().final_wm, c2(), trace.last().txns[0]),
        trace.last().rolled_back.contains((0int, 0nat)),
        !has_version(trace.last().versions, 0, 0),
        !has_version(trace.last().versions, 1, 0),
{
    let c = c2();
    lemma_w3_init();
    lemma_initial_behavior(e3_0(), c);
    let t = seq![e3_0()];
    assert(t.last() == e3_0());
    lemma_w3_1();
    let t = lemma_extend(t, c, w3_a(1));
    lemma_w3_2();
    let t = lemma_extend(t, c, w3_a(2));
    lemma_w3_3();
    let t = lemma_extend(t, c, w3_a(3));
    lemma_w3_4();
    let t = lemma_extend(t, c, w3_a(4));
    lemma_w3_5();
    let t = lemma_extend(t, c, w3_a(5));
    let t5 = t;
    lemma_w3_6();
    let t = lemma_extend(t, c, w3_a(6));
    let t6 = t;
    lemma_w3_7();
    let t = lemma_extend(t, c, w3_a(7));
    lemma_w3_8();
    let t = lemma_extend(t, c, w3_a(8));
    lemma_w3_9();
    let t = lemma_extend(t, c, w3_a(9));
    lemma_w3_10();
    let t = lemma_extend(t, c, w3_a(10));
    lemma_w3_11();
    let t = lemma_extend(t, c, w3_a(11));
    lemma_w3_12();
    let t = lemma_extend(t, c, w3_a(12));
    let t12 = t;
    lemma_w3_13();
    let t = lemma_extend(t, c, w3_a(13));
    // index facts
    assert(t5.last() == e3_5());
    assert(t6.last() == e3_6());
    assert(t12.last() == e3_12());
    assert(t.last() == e3_13());
    assert(t.len() == 14);
    assert(t5.len() == 6 && t6.len() == 7 && t12.len() == 13);
    assert(t[5] == e3_5()) by {
        assert(t6[5] == t5[5]);
        assert(t12[5] == t6[5]);
        assert(t[5] == t12[5]);
    }
    assert(t[6] == e3_6()) by {
        assert(t12[6] == t6[6]);
        assert(t[6] == t12[6]);
    }
    assert(t[12] == e3_12());
    // state facts
    lemma_w3_fw();
    let r = u_5();
    assert(e3_5().txns[0] == r);
    assert(e3_6().txns[0] == r);
    assert(stream_at(e3_5().streams, r, 1) =~= seq![lg(0, 1)]);
    assert(is_log_of(stream_at(e3_5().streams, r, 1)[0], 0));
    assert(e3_5().streams[sid_of(1, 0, 0)].durable.len() == 0);
    assert(stream_at(e3_6().streams, r, 1) =~= Seq::<Entry>::empty());
    assert(vers(e3_5().versions, 1)[0].txn == 0);
    assert(vers(e3_12().versions, 0)[0].txn == 0);
    t
}

} // verus!
