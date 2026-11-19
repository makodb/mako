//! Constructive executions, checked by Verus against the actual transition
//! relation. These are proofs of reachability, not state-space exploration.
use super::types::*;
use super::types::View;
use super::normal::*;
use super::recovery::*;
use super::behavior::*;
use vstd::prelude::*;

verus! {

pub open spec fn initial(c: Constants) -> State {
    State {
        shards: Seq::new(c.shards as nat, |i: int| ShardState { epoch: 0, clock: 0, alive: true }),
        logs: Seq::new(c.shards as nat, |i: int| RaftLog { durable: Seq::empty(), pending: Seq::empty() }),
        obligations: Map::empty(), versions: Map::empty(), locks: Map::empty(), txns: Map::empty(),
        order: Seq::empty(), network: Set::empty(), views: Map::empty(), rolled_back: Set::empty(), tick: 0,
    }
}
pub proof fn begin(c: Constants) -> (states: Seq<State>)
    requires valid_constants(c)
    ensures behavior(states, c), states == seq![initial(c)]
{
    seq![initial(c)]
}
pub proof fn extend(states: Seq<State>, c: Constants, a: Action) -> (after: Seq<State>)
    requires behavior(states, c), enabled(states.last(), c, a)
    ensures behavior(after, c), after == states.push(apply(states.last(), c, a)),
        after.last() == apply(states.last(), c, a), after.len() == states.len() + 1
{
    let z = apply(states.last(), c, a);
    assert(next(states.last(), z, c));
    let after = states.push(z);
    assert forall|i: int| 0 <= i < after.len() - 1 implies #[trigger] next(after[i], after[i + 1], c) by {
        if i < states.len() - 1 {
            assert(after[i] == states[i]);
            assert(after[i + 1] == states[i + 1]);
        } else {
            assert(after[i] == states.last());
            assert(after[i + 1] == z);
        }
    }
    after
}

pub open spec fn one_shard() -> Constants { Constants { shards: 1 } }
pub open spec fn put7() -> Txn { Txn { ops: map![0int => Op::Put { value: 7 }] } }
pub proof fn put7_shape()
    ensures valid_txn(put7()), read_set(put7()) == Set::<int>::empty(), write_set(put7()) == set![0int]
{
    assert(put7().ops.dom() =~= set![0int]);
    assert(read_set(put7()) =~= Set::<int>::empty());
    assert(write_set(put7()) =~= set![0int]);
}

pub proof fn one_participant(r: TxnRec, id: int)
    requires r.coord == 0
    ensures Seq::new(1nat, |i: int| (i, id)).to_set()
        .filter(|p: (int, int)| log_participant(one_shard(), r, p.0)) == set![(0int, id)]
{
    let slots = Seq::new(1nat, |i: int| (i, id));
    assert(slots[0] == (0int, id));
    assert(slots.to_set() =~= set![(0int, id)]);
    assert(slots.to_set().filter(|p: (int, int)| log_participant(one_shard(), r, p.0)) =~= set![(0int, id)]);
}

/// A transaction is provisional before replication; Final requires the
/// replicated barrier and delivery of its epoch-tagged certificate.
pub proof fn witness_one_shard_commit() -> (states: Seq<State>)
    ensures behavior(states, one_shard()),
        states.last().txns[0].status is Final,
        states.last().txns[0].ts == 1,
        states.last().versions[0].last().value == 7,
        states.last().logs[0].durable == seq![
            Entry::Tx { id: 0, epoch: 0, ts: 1 }, Entry::Barrier { epoch: 0, through: 1 }],
{
    reveal_with_fuel(frontier, 5);
    let c = one_shard();
    put7_shape();
    let t = begin(c);
    let t = extend(t, c, Action::Submit { id: 0, body: put7(), coord: 0 });
    one_participant(t.last().txns[0], 0);
    let t = extend(t, c, Action::Prepare { id: 0, ts: 1 });
    assert(t.last().obligations =~= map![(0int, 0int) => Obligation { epoch: 0, ts: 1 }]);
    let t = extend(t, c, Action::Install { id: 0, shard: 0 });
    let t = extend(t, c, Action::Certify { id: 0 });
    let t = extend(t, c, Action::Provisional { id: 0 });
    assert(view(t.last().views, 0, 0, 0).through == 0);
    assert(t.last().obligations.dom().contains((0int, 0int)));
    assert(t.last().obligations[(0int, 0int)].ts == 1);
    assert(!can_final(t.last(), c, 0));
    assert(!can_barrier(t.last(), c, 0, 1));
    let t = extend(t, c, Action::Replicate { shard: 0 });
    assert(t.last().obligations =~= Map::<(int, int), Obligation>::empty());
    let t = extend(t, c, Action::Barrier { shard: 0, through: 1 });
    let t = extend(t, c, Action::Replicate { shard: 0 });
    let t = extend(t, c, Action::Send { shard: 0, epoch: 0 });
    let r = Report { shard: 0, epoch: 0, through: 1, closed: false };
    let t = extend(t, c, Action::Receive { observer: 0, report: r });
    let t = extend(t, c, Action::Final { id: 0 });
    t
}

pub open spec fn put9() -> Txn { Txn { ops: map![1int => Op::Put { value: 9 }] } }
pub proof fn put9_shape()
    ensures valid_txn(put9()), read_set(put9()) == Set::<int>::empty(), write_set(put9()) == set![1int]
{
    assert(put9().ops.dom() =~= set![1int]);
    assert(read_set(put9()) =~= Set::<int>::empty());
    assert(write_set(put9()) =~= set![1int]);
}

/// A higher timestamp enters the single Raft log first. The lower-timestamp
/// obligation prevents unsafe advancement; both can become final afterward.
pub proof fn witness_out_of_order_publication() -> (states: Seq<State>)
    ensures behavior(states, one_shard()),
        states.last().txns[0].status is Final,
        states.last().txns[1].status is Final,
        states.last().logs[0].durable == seq![
            Entry::Tx { id: 1, epoch: 0, ts: 2 },
            Entry::Tx { id: 0, epoch: 0, ts: 1 },
            Entry::Barrier { epoch: 0, through: 2 }],
{
    reveal_with_fuel(frontier, 8);
    let c = one_shard();
    put7_shape();
    put9_shape();
    let t = begin(c);
    let t = extend(t, c, Action::Submit { id: 0, body: put7(), coord: 0 });
    one_participant(t.last().txns[0], 0);
    let t = extend(t, c, Action::Prepare { id: 0, ts: 1 });
    assert(t.last().obligations =~= map![(0int, 0int) => Obligation { epoch: 0, ts: 1 }]);
    let t = extend(t, c, Action::Submit { id: 1, body: put9(), coord: 0 });
    one_participant(t.last().txns[1], 1);
    let t = extend(t, c, Action::Prepare { id: 1, ts: 2 });
    assert(t.last().obligations =~= map![
        (0int, 0int) => Obligation { epoch: 0, ts: 1 },
        (0int, 1int) => Obligation { epoch: 0, ts: 2 }]);
    let t = extend(t, c, Action::Install { id: 1, shard: 0 });
    let t = extend(t, c, Action::Certify { id: 1 });
    let t = extend(t, c, Action::Replicate { shard: 0 });
    assert(t.last().obligations =~= map![(0int, 0int) => Obligation { epoch: 0, ts: 1 }]);
    assert(t.last().obligations.dom().contains((0int, 0int)));
    assert(t.last().obligations[(0int, 0int)].epoch == 0);
    assert(t.last().obligations[(0int, 0int)].ts == 1);
    assert(!can_barrier(t.last(), c, 0, 2));
    let t = extend(t, c, Action::Install { id: 0, shard: 0 });
    let t = extend(t, c, Action::Certify { id: 0 });
    let t = extend(t, c, Action::Replicate { shard: 0 });
    assert(t.last().obligations =~= Map::<(int, int), Obligation>::empty());
    let t = extend(t, c, Action::Barrier { shard: 0, through: 2 });
    let t = extend(t, c, Action::Replicate { shard: 0 });
    let t = extend(t, c, Action::Send { shard: 0, epoch: 0 });
    let t = extend(t, c, Action::Receive { observer: 0,
        report: Report { shard: 0, epoch: 0, through: 2, closed: false } });
    let t = extend(t, c, Action::Provisional { id: 0 });
    let t = extend(t, c, Action::Final { id: 0 });
    let t = extend(t, c, Action::Provisional { id: 1 });
    let t = extend(t, c, Action::Final { id: 1 });
    t
}

pub open spec fn two_shards() -> Constants { Constants { shards: 2 } }
pub open spec fn add_both() -> Txn {
    Txn { ops: map![0int => Op::Add { delta: 7 }, 1int => Op::Add { delta: 9 }] }
}
pub proof fn add_both_shape()
    ensures valid_txn(add_both()),
        read_set(add_both()) == set![0int, 1int],
        write_set(add_both()) == set![0int, 1int],
        owner(two_shards(), 0) == 0, owner(two_shards(), 1) == 1,
        writes_at(two_shards(), add_both(), 0), writes_at(two_shards(), add_both(), 1),
{
    assert(0int % 2int == 0int) by (compute);
    assert(1int % 2int == 1int) by (compute);
    assert(add_both().ops.dom() =~= set![0int, 1int]);
    assert(read_set(add_both()) =~= set![0int, 1int]);
    assert(write_set(add_both()) =~= set![0int, 1int]);
    assert(write_set(add_both()).contains(0));
    assert(write_set(add_both()).contains(1));
}
pub proof fn two_participants(r: TxnRec, id: int)
    requires r.coord == 0, r.body == add_both()
    ensures Seq::new(2nat, |i: int| (i, id)).to_set()
        .filter(|p: (int, int)| log_participant(two_shards(), r, p.0)) == set![(0int, id), (1int, id)]
{
    add_both_shape();
    let slots = Seq::new(2nat, |i: int| (i, id));
    assert(slots[0] == (0int, id));
    assert(slots[1] == (1int, id));
    assert(slots.to_set() =~= set![(0int, id), (1int, id)]);
    assert(write_set(r.body).contains(1));
    assert(owner(two_shards(), 1) == 1);
    assert(slots.to_set().filter(|p: (int, int)| log_participant(two_shards(), r, p.0))
        =~= set![(0int, id), (1int, id)]);
}

/// A read-modify-write spans two shards and uses exactly the same scalar
/// timestamp at both. A lagging report blocks finality until delivered.
pub proof fn witness_two_shard_commit() -> (states: Seq<State>)
    ensures behavior(states, two_shards()),
        states.last().txns[0].status is Final,
        states.last().versions[0].last().value == 7,
        states.last().versions[1].last().value == 9,
        states.last().logs[0].durable == states.last().logs[1].durable,
{
    reveal_with_fuel(frontier, 6);
    let c = two_shards();
    add_both_shape();
    let t = begin(c);
    let t = extend(t, c, Action::Submit { id: 0, body: add_both(), coord: 0 });
    let t = extend(t, c, Action::Read { id: 0, key: 0 });
    let t = extend(t, c, Action::Read { id: 0, key: 1 });
    two_participants(t.last().txns[0], 0);
    let t = extend(t, c, Action::Prepare { id: 0, ts: 1 });
    assert(t.last().obligations =~= map![
        (0int, 0int) => Obligation { epoch: 0, ts: 1 },
        (1int, 0int) => Obligation { epoch: 0, ts: 1 }]);
    let t = extend(t, c, Action::Install { id: 0, shard: 0 });
    let t = extend(t, c, Action::Install { id: 0, shard: 1 });
    let t = extend(t, c, Action::Publish { id: 0, shard: 1 });
    let t = extend(t, c, Action::Certify { id: 0 });
    let t = extend(t, c, Action::Provisional { id: 0 });
    let t = extend(t, c, Action::Replicate { shard: 0 });
    let t = extend(t, c, Action::Replicate { shard: 1 });
    assert(t.last().obligations =~= Map::<(int, int), Obligation>::empty());
    let t = extend(t, c, Action::Barrier { shard: 0, through: 1 });
    let t = extend(t, c, Action::Barrier { shard: 1, through: 1 });
    let t = extend(t, c, Action::Replicate { shard: 0 });
    let t = extend(t, c, Action::Replicate { shard: 1 });
    let t = extend(t, c, Action::Send { shard: 0, epoch: 0 });
    let t = extend(t, c, Action::Send { shard: 1, epoch: 0 });
    let t = extend(t, c, Action::Receive { observer: 0,
        report: Report { shard: 0, epoch: 0, through: 1, closed: false } });
    assert(view(t.last().views, 0, 0, 1).through == 0);
    assert(!can_final(t.last(), c, 0));
    let t = extend(t, c, Action::Receive { observer: 0,
        report: Report { shard: 1, epoch: 0, through: 1, closed: false } });
    let t = extend(t, c, Action::Final { id: 0 });
    t
}

/// A prepared transaction installs at one shard and loses its other shard.
/// The finite old-epoch cut is zero, not infinity: closure enables rollback
/// without turning its speculative value into a durable result.
pub proof fn witness_partial_install_rollback() -> (states: Seq<State>)
    ensures behavior(states, two_shards()),
        states.last().txns[0].status == (Status::Aborted { prepared: true }),
        states.last().versions[0].len() == 0,
        states.last().rolled_back.contains((0int, 0nat)),
        final_ready(states.last().views, two_shards(), 0, 0),
        view(states.last().views, 0, 0, 0) == (View { through: 0, closed: true }),
        view(states.last().views, 0, 0, 1) == (View { through: 0, closed: true }),
{
    reveal_with_fuel(frontier, 8);
    reveal_with_fuel(Seq::filter, 3);
    reveal_with_fuel(config_epoch, 8);
    let c = two_shards();
    add_both_shape();
    let t = begin(c);
    let t = extend(t, c, Action::Submit { id: 0, body: add_both(), coord: 0 });
    let t = extend(t, c, Action::Read { id: 0, key: 0 });
    let t = extend(t, c, Action::Read { id: 0, key: 1 });
    two_participants(t.last().txns[0], 0);
    let t = extend(t, c, Action::Prepare { id: 0, ts: 1 });
    let t = extend(t, c, Action::Install { id: 0, shard: 0 });
    assert(t.last().versions[0].last().value == 7);
    let t = extend(t, c, Action::Crash { shard: 1, survive: 0 });
    let t = extend(t, c, Action::ProposeEpoch);
    assert(cm_epoch(t.last()) == 0);
    let t = extend(t, c, Action::Replicate { shard: 0 });
    let t = extend(t, c, Action::ObserveEpoch { shard: 0 });
    let t = extend(t, c, Action::Recover { shard: 1, floor: 0 });
    let t = extend(t, c, Action::Close { shard: 0, epoch: 0 });
    let t = extend(t, c, Action::Close { shard: 1, epoch: 0 });
    let t = extend(t, c, Action::Replicate { shard: 0 });
    let t = extend(t, c, Action::Replicate { shard: 1 });
    let t = extend(t, c, Action::Send { shard: 0, epoch: 0 });
    let t = extend(t, c, Action::Send { shard: 1, epoch: 0 });
    let t = extend(t, c, Action::Receive { observer: 0,
        report: Report { shard: 0, epoch: 0, through: 0, closed: true } });
    let t = extend(t, c, Action::Receive { observer: 0,
        report: Report { shard: 1, epoch: 0, through: 0, closed: true } });
    assert(!can_final(t.last(), c, 0));
    assert(t.last().versions[0] == seq![Version { txn: 0, epoch: 0, ts: 1, value: 7 }]);
    assert(view(t.last().views, 0, 0, 0).through == 0);
    assert(!below_view(t.last().views, c, 0, 0, 1));
    let t = extend(t, c, Action::Rollback { shard: 0, epoch: 0 });
    assert(t.last().versions[0] =~= Seq::<Version>::empty());
    t
}

/// Even the shard hosting CM can fail: its Raft control service proposes and
/// commits the next epoch while speculative serving remains fenced. A new
/// epoch transaction then reaches final completion.
pub proof fn witness_cm_failure_and_new_epoch_commit() -> (states: Seq<State>)
    ensures behavior(states, one_shard()),
        cm_epoch(states.last()) == 1,
        states.last().shards[0].epoch == 1,
        states.last().txns[0].epoch == 1,
        states.last().txns[0].status is Final,
        states.last().versions[0].last().value == 7,
        view(states.last().views, 0, 0, 0) == (View { through: 0, closed: true }),
{
    reveal_with_fuel(frontier, 10);
    reveal_with_fuel(config_epoch, 10);
    reveal_with_fuel(clock_floor, 10);
    let c = one_shard();
    put7_shape();
    let t = begin(c);
    let t = extend(t, c, Action::Crash { shard: 0, survive: 0 });
    let t = extend(t, c, Action::ProposeEpoch);
    assert(!t.last().shards[0].alive);
    assert(cm_epoch(t.last()) == 0);
    let t = extend(t, c, Action::Replicate { shard: 0 });
    assert(!t.last().shards[0].alive);
    assert(cm_epoch(t.last()) == 1);
    let t = extend(t, c, Action::Recover { shard: 0, floor: 0 });
    let t = extend(t, c, Action::Close { shard: 0, epoch: 0 });
    let t = extend(t, c, Action::Replicate { shard: 0 });
    let t = extend(t, c, Action::Send { shard: 0, epoch: 0 });
    let old = Report { shard: 0, epoch: 0, through: 0, closed: true };
    let t = extend(t, c, Action::Receive { observer: 0, report: old });
    let t = extend(t, c, Action::Submit { id: 0, body: put7(), coord: 0 });
    one_participant(t.last().txns[0], 0);
    let t = extend(t, c, Action::Prepare { id: 0, ts: 1 });
    assert(t.last().obligations =~= map![(0int, 0int) => Obligation { epoch: 1, ts: 1 }]);
    let t = extend(t, c, Action::Install { id: 0, shard: 0 });
    let t = extend(t, c, Action::Certify { id: 0 });
    let t = extend(t, c, Action::Provisional { id: 0 });
    let t = extend(t, c, Action::Replicate { shard: 0 });
    assert(t.last().obligations =~= Map::<(int, int), Obligation>::empty());
    let t = extend(t, c, Action::Barrier { shard: 0, through: 1 });
    let t = extend(t, c, Action::Replicate { shard: 0 });
    let t = extend(t, c, Action::Receive { observer: 0, report: old });
    assert(view(t.last().views, 0, 1, 0).through == 0);
    assert(!can_final(t.last(), c, 0));
    let t = extend(t, c, Action::Send { shard: 0, epoch: 1 });
    let t = extend(t, c, Action::Receive { observer: 0,
        report: Report { shard: 0, epoch: 1, through: 1, closed: false } });
    let t = extend(t, c, Action::Final { id: 0 });
    t
}

} // verus!
