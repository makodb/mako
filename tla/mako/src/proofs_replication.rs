//! Induction over all physical-log and certificate transitions.
use super::types::*;
use super::normal::*;
use super::recovery::*;
use super::behavior::*;
use super::log_invariants::*;
use super::log_lemmas::*;
use super::invariants::*;
use vstd::prelude::*;

verus! {

pub open spec fn appended(s: State, a: Action) -> Option<(int, Entry)> {
    match a {
        Action::Publish { id, shard } => Some((shard, tx_entry(s.txns[id], id))),
        Action::Certify { id } => Some((s.txns[id].coord, tx_entry(s.txns[id], id))),
        Action::Barrier { shard, through } => Some((shard, Entry::Barrier { epoch: s.shards[shard].epoch, through })),
        Action::Close { shard, epoch } => Some((shard, close_entry(s, shard, epoch))),
        Action::ProposeEpoch => Some((0, Entry::AdvanceSpecEpoch { epoch: cm_epoch(s) + 1 })),
        _ => None,
    }
}
pub proof fn lemma_log_effect(s: State, c: Constants, a: Action)
    requires log_inv(s, c), enabled(s, c, a),
    ensures ({ let z = apply(s, c, a); match appended(s, a) {
        Some((i, e)) => is_shard(c, i) && z.logs == s.logs.update(i, append(s.logs[i], e)),
        None => match a {
            Action::Replicate { shard } => z.logs == replicate(s, shard).logs,
            Action::Crash { shard, survive } => z.logs == crash(s, c, shard, survive).logs,
            _ => z.logs == s.logs,
        }
    }}),
{
    match a {
        Action::Publish { .. } => {}, Action::Certify { .. } => {}, Action::Barrier { .. } => {},
        Action::Close { .. } => {}, Action::ProposeEpoch => {}, _ => {},
    }
}
pub proof fn lemma_durable_prefix(s: State, c: Constants, a: Action, i: int)
    requires log_inv(s, c), enabled(s, c, a), is_shard(c, i),
    ensures s.logs[i].durable.is_prefix_of(apply(s, c, a).logs[i].durable),
        apply(s, c, a).logs.len() == s.logs.len(),
{
    lemma_log_effect(s, c, a);
    match a {
        Action::Replicate { shard } => {
            if i == shard { assert(s.logs[i].durable.is_prefix_of(s.logs[i].durable.push(s.logs[i].pending[0]))); }
        },
        Action::Crash { shard, survive } => {
            if i == shard { assert(s.logs[i].durable.is_prefix_of(s.logs[i].durable + s.logs[i].pending.take(survive as int))); }
        },
        _ => {},
    }
}
pub proof fn lemma_coverage_step(s: State, c: Constants, a: Action, i: int, ep: nat)
    requires log_inv(s, c), enabled(s, c, a), is_shard(c, i),
    ensures frontier(s.logs[i].durable, ep) <= frontier(apply(s, c, a).logs[i].durable, ep),
        config_epoch(s.logs[i].durable) <= config_epoch(apply(s, c, a).logs[i].durable),
{
    lemma_durable_prefix(s, c, a, i);
    let d = apply(s, c, a).logs[i].durable;
    assert(d.take(s.logs[i].durable.len() as int) =~= s.logs[i].durable);
    lemma_prefix(d, s.logs[i].durable.len() as int, ep);
}
pub proof fn lemma_closed_cut_step(s: State, c: Constants, a: Action, i: int, ep: nat)
    requires log_inv(s, c), enabled(s, c, a), is_shard(c, i), has_close(s.logs[i].durable, ep),
    ensures has_close(apply(s, c, a).logs[i].durable, ep),
        frontier(apply(s, c, a).logs[i].durable, ep) == frontier(s.logs[i].durable, ep),
{
    lemma_log_effect(s, c, a);
    lemma_coverage_step(s, c, a, i, ep);
    lemma_closed_exact(s, c, i, ep);
    let z = apply(s, c, a);
    lemma_prefix_records(s.logs[i].durable, z.logs[i].durable, 0, ep);
    match a {
        Action::Replicate { shard } => {
            if i == shard {
                assert(entries(z.logs[i]) =~= entries(s.logs[i]));
                lemma_concat(z.logs[i].durable, z.logs[i].pending, ep);
            }
        },
        Action::Crash { shard, survive } => {
            if i == shard {
                assert(z.logs[i].durable =~= entries(s.logs[i]).take(z.logs[i].durable.len() as int));
                lemma_prefix(entries(s.logs[i]), z.logs[i].durable.len() as int, ep);
            }
        },
        _ => {},
    }
}
pub proof fn lemma_stable_step(s: State, c: Constants, a: Action, r: TxnRec)
    requires log_inv(s, c), enabled(s, c, a), stable(s.logs, c, r),
    ensures stable(apply(s, c, a).logs, c, r),
{
    assert forall|i: int| is_shard(c, i) implies
        r.ts <= frontier(#[trigger] apply(s, c, a).logs[i].durable, r.epoch) by {
        lemma_coverage_step(s, c, a, i, r.epoch);
    }
}
pub proof fn lemma_doomed_step(s: State, c: Constants, a: Action, r: TxnRec)
    requires log_inv(s, c), enabled(s, c, a), doomed(s, c, r),
    ensures doomed(apply(s, c, a), c, r),
{
    assert forall|i: int| is_shard(c, i) implies
        has_close(#[trigger] apply(s, c, a).logs[i].durable, r.epoch)
        && frontier(apply(s, c, a).logs[i].durable, r.epoch) == frontier(s.logs[i].durable, r.epoch) by {
        lemma_closed_cut_step(s, c, a, i, r.epoch);
    }
    let i = choose|i: int| is_shard(c, i) && r.ts > frontier(#[trigger] s.logs[i].durable, r.epoch);
    lemma_closed_cut_step(s, c, a, i, r.epoch);
}
pub proof fn lemma_log_init(s: State, c: Constants)
    requires init(s, c),
    ensures log_inv(s, c),
{
    assert forall|i: int| is_shard(c, i) implies (#[trigger] s.shards[i]).clock >= 0
        && s.shards[i].epoch <= cm_epoch(s)
        && clock_floor(entries(s.logs[i])) <= s.shards[i].clock by {
        assert(entries(s.logs[i]) =~= Seq::<Entry>::empty());
    }
}

pub proof fn lemma_entries_effect(s: State, c: Constants, a: Action, i: int)
    requires log_inv(s, c), enabled(s, c, a), is_shard(c, i),
    ensures ({ let z = apply(s, c, a); match appended(s, a) {
        Some((j, e)) => entries(z.logs[i]) == if i == j { entries(s.logs[i]).push(e) } else { entries(s.logs[i]) },
        None => match a {
            Action::Crash { shard, survive } => entries(z.logs[i]) == if i == shard {
                entries(s.logs[i]).take((s.logs[i].durable.len() + survive) as int)
            } else { entries(s.logs[i]) },
            _ => entries(z.logs[i]) == entries(s.logs[i]),
        }
    }}),
{
    lemma_log_effect(s, c, a);
    let z = apply(s, c, a);
    match appended(s, a) {
        Some((j, e)) => {
            if i == j { assert(entries(z.logs[i]) =~= entries(s.logs[i]).push(e)); }
        },
        None => match a {
            Action::Replicate { shard } => {
                if i == shard { assert(entries(z.logs[i]) =~= entries(s.logs[i])); }
            },
            Action::Crash { shard, survive } => {
                if i == shard {
                    assert(entries(z.logs[i]) =~= entries(s.logs[i]).take((s.logs[i].durable.len() + survive) as int));
                }
            },
            _ => {},
        },
    }
}

pub proof fn lemma_log_txn_step(s: State, c: Constants, a: Action, id: int)
    requires log_inv(s, c), enabled(s, c, a), apply(s, c, a).txns.dom().contains(id),
    ensures log_txn(apply(s, c, a), c, id),
{
    if s.txns.dom().contains(id) { assert(log_txn(s, c, id)); }
    match a {
        Action::Submit { id: other, .. } => {
            if id == other { assert(log_txn(apply(s, c, a), c, id)); }
            else {
                assert(s.txns.dom().contains(id));
                assert(log_txn(s, c, id));
                assert(apply(s, c, a).shards == s.shards);
                assert(apply(s, c, a).txns[id] == s.txns[id]);
                assert(log_txn(apply(s, c, a), c, id));
            }
        },
        Action::Read { .. } => {},
        Action::Prepare { id: other, ts } => {
            let z = apply(s, c, a);
            if id == other {
                assert forall|i: int| is_shard(c, i) && prepared(z.txns[id])
                    && #[trigger] clock_participant(c, z.txns[id], i) implies
                    z.txns[id].epoch <= z.shards[i].epoch
                    && (z.txns[id].epoch == z.shards[i].epoch ==> z.txns[id].ts <= z.shards[i].clock) by {
                    if i != s.txns[id].coord {
                        let k = choose|k: int| #[trigger] s.txns[id].body.ops.dom().contains(k) && owner(c, k) == i;
                    }
                }
            }
        },
        Action::Install { .. } => {},
        Action::Publish { .. } => {},
        Action::Certify { .. } => {},
        Action::Provisional { .. } => {},
        Action::Final { .. } => {},
        Action::Abort { .. } => {},
        Action::Pulse { .. } => {},
        Action::Barrier { .. } => {},
        Action::Replicate { .. } => {},
        Action::Send { .. } => {},
        Action::Receive { .. } => {},
        Action::ProposeEpoch => {},
        Action::Crash { .. } => {},
        Action::Recover { .. } => {},
        Action::ObserveEpoch { .. } => {},
        Action::Close { .. } => {},
        Action::Rollback { .. } => {},
        Action::Stutter => {},
    }
    let z = apply(s, c, a);
    assert forall|i: int| is_shard(c, i) && prepared(z.txns[id])
        && #[trigger] clock_participant(c, z.txns[id], i) implies
        z.txns[id].epoch <= z.shards[i].epoch
        && (z.txns[id].epoch == z.shards[i].epoch ==> z.txns[id].ts <= z.shards[i].clock) by {
        if s.txns.dom().contains(id) && prepared(s.txns[id]) {
            assert(clock_participant(c, s.txns[id], i));
            assert(s.txns[id].epoch <= s.shards[i].epoch);
        }
        match a {
            Action::Prepare { id: other, .. } => {
                if id == other && i != s.txns[id].coord {
                    let k = choose|k: int| #[trigger] s.txns[id].body.ops.dom().contains(k) && owner(c, k) == i;
                }
            },
            _ => {},
        }
    }
}

pub proof fn lemma_shard_log_step(s: State, c: Constants, a: Action, i: int)
    requires log_inv(s, c), enabled(s, c, a), is_shard(c, i),
    ensures ({ let z = apply(s, c, a);
        s.shards[i].epoch <= z.shards[i].epoch && z.shards[i].epoch <= cm_epoch(z)
        && z.shards[i].clock >= 0 && clock_floor(entries(z.logs[i])) <= z.shards[i].clock
        && (!z.shards[i].alive ==> forall|j: int| 0 <= j < z.logs[i].pending.len()
            ==> #[trigger] z.logs[i].pending[j] is AdvanceSpecEpoch)
    }),
{
    let z = apply(s, c, a);
    lemma_coverage_step(s, c, a, 0, 0);
    lemma_entries_effect(s, c, a, i);
    lemma_bounds(entries(s.logs[i]), 0);
    match appended(s, a) {
        Some((j, e)) => {
            if i == j {
                lemma_push(entries(s.logs[i]), e, 0);
                match a {
                    Action::Publish { id, .. } => {
                        assert(clock_participant(c, s.txns[id], i));
                    },
                    Action::Certify { id } => {
                        assert(clock_participant(c, s.txns[id], i));
                    },
                    Action::Close { epoch, .. } => { lemma_bounds(entries(s.logs[i]), epoch); },
                    _ => {},
                }
            }
        },
        None => {
            match a {
                Action::Crash { shard, survive } => {
                    if i == shard { lemma_prefix(entries(s.logs[i]), (s.logs[i].durable.len() + survive) as int, 0); }
                },
                Action::Recover { shard, floor } => {
                    if i == shard {
                        lemma_bounds(s.logs[i].durable, 0);
                        assert forall|j: int| 0 <= j < entries(s.logs[i]).len() implies
                            entry_clock(#[trigger] entries(s.logs[i])[j]) <= floor by {
                            if j < s.logs[i].durable.len() {
                                assert(entries(s.logs[i])[j] == s.logs[i].durable[j]);
                            } else {
                                assert(entries(s.logs[i])[j] == s.logs[i].pending[j - s.logs[i].durable.len()]);
                            }
                        }
                        lemma_clock_ceiling(entries(s.logs[i]), floor);
                    }
                },
                _ => {},
            }
        },
    }
    if !z.shards[i].alive {
        assert forall|j: int| 0 <= j < z.logs[i].pending.len() implies
            #[trigger] z.logs[i].pending[j] is AdvanceSpecEpoch by {
            match a {
                Action::ProposeEpoch => {
                    if i == 0 && j == s.logs[0].pending.len() {} else {
                        assert(z.logs[i].pending[j] == s.logs[i].pending[j]);
                    }
                },
                Action::Replicate { shard } => {
                    if i == shard { assert(z.logs[i].pending[j] == s.logs[i].pending[j + 1]); }
                },
                _ => {},
            }
        }
    }
}

pub proof fn lemma_entry_step(s: State, c: Constants, a: Action, i: int, j: int)
    requires log_inv(s, c), enabled(s, c, a), is_shard(c, i),
        0 <= j < entries(apply(s, c, a).logs[i]).len(),
    ensures entry_valid(apply(s, c, a), c, i, entries(apply(s, c, a).logs[i])[j]),
{
    let z = apply(s, c, a);
    let e = entries(z.logs[i])[j];
    lemma_entries_effect(s, c, a, i);
    lemma_shard_log_step(s, c, a, i);
    if j < entries(s.logs[i]).len() {
        assert(e == entries(s.logs[i])[j]);
        assert(entry_valid(s, c, i, e));
        if e is Tx {
            assert(prepared(s.txns[e->Tx_id]));
        }
        match a {
            Action::Submit { .. } => {},
            Action::Read { .. } => {},
            Action::Prepare { .. } => {},
            Action::Install { .. } => {},
            Action::Publish { .. } => {},
            Action::Certify { .. } => {},
            Action::Provisional { .. } => {},
            Action::Final { .. } => {},
            Action::Abort { .. } => {},
            Action::Crash { .. } => {},
            Action::ObserveEpoch { .. } => {},
            _ => {},
        }
    } else {
        match a {
            Action::Publish { .. } => {},
            Action::Certify { .. } => {},
            Action::Barrier { .. } => {},
            Action::Close { epoch, .. } => { lemma_bounds(entries(s.logs[i]), epoch); },
            Action::ProposeEpoch => {},
            _ => {},
        }
    }
}

pub proof fn lemma_log_shape_step(s: State, c: Constants, a: Action)
    requires log_inv(s, c), enabled(s, c, a),
    ensures log_shape(apply(s, c, a), c),
{
    let z = apply(s, c, a);
    lemma_durable_prefix(s, c, a, 0);
    assert forall|i: int| is_shard(c, i) implies (#[trigger] z.shards[i]).clock >= 0
        && z.shards[i].epoch <= cm_epoch(z)
        && (!z.shards[i].alive ==> forall|j: int| 0 <= j < z.logs[i].pending.len()
            ==> #[trigger] z.logs[i].pending[j] is AdvanceSpecEpoch)
        && clock_floor(entries(z.logs[i])) <= z.shards[i].clock by {
        lemma_shard_log_step(s, c, a, i);
    }
    assert forall|id: int| #[trigger] z.txns.dom().contains(id) implies log_txn(z, c, id) by {
        lemma_log_txn_step(s, c, a, id);
    }
    assert forall|i: int, j: int| is_shard(c, i) && 0 <= j < entries(z.logs[i]).len()
        implies entry_valid(z, c, i, #[trigger] entries(z.logs[i])[j]) by {
        lemma_entry_step(s, c, a, i, j);
    }
}

pub proof fn lemma_prepare_parts(c: Constants, r: TxnRec, id: int, p: (int, int))
    requires valid_constants(c),
    ensures Seq::new(c.shards as nat, |i: int| (i, id)).to_set()
        .filter(|q: (int, int)| log_participant(c, r, q.0)).contains(p)
        <==> is_shard(c, p.0) && p.1 == id && log_participant(c, r, p.0),
{
    let xs = Seq::new(c.shards as nat, |i: int| (i, id));
    if xs.to_set().contains(p) {
        let j = choose|j: int| 0 <= j < xs.len() && #[trigger] xs[j] == p;
    }
    if is_shard(c, p.0) && p.1 == id {
        assert(xs[p.0] == p);
        assert(xs.contains(p));
    }
}

pub proof fn lemma_obligation_step(s: State, c: Constants, a: Action)
    requires log_inv(s, c), enabled(s, c, a),
    ensures obligation_inv(apply(s, c, a), c),
{
    let z = apply(s, c, a);
    assert forall|p: (int, int)| #[trigger] z.obligations.dom().contains(p) implies
        is_shard(c, p.0) && z.txns.dom().contains(p.1) && prepared(z.txns[p.1])
        && log_participant(c, z.txns[p.1], p.0)
        && z.shards[p.0].alive && z.shards[p.0].epoch == z.obligations[p].epoch
        && z.obligations[p].epoch == z.txns[p.1].epoch && z.obligations[p].ts == z.txns[p.1].ts by {
        match a {
            Action::Prepare { id, .. } => {
                lemma_prepare_parts(c, s.txns[id], id, p);
                if p.1 == id {
                    if p.0 != s.txns[id].coord {
                        assert(writes_at(c, s.txns[id].body, p.0));
                        let k = choose|k: int| #[trigger] write_set(s.txns[id].body).contains(k) && owner(c, k) == p.0;
                    }
                }
            },
            Action::Submit { .. } => {},
            Action::Read { .. } => {},
            Action::Install { .. } => {},
            Action::Publish { .. } => {},
            Action::Certify { .. } => {},
            Action::Provisional { .. } => {},
            Action::Final { .. } => {},
            Action::Abort { .. } => {},
            Action::Crash { .. } => {},
            Action::Recover { .. } => {},
            Action::ObserveEpoch { .. } => {},
            _ => {},
        }
    }
    assert forall|i: int, id: int| is_shard(c, i) && #[trigger] z.txns.dom().contains(id)
        && prepared(z.txns[id]) && #[trigger] log_participant(c, z.txns[id], i)
        && z.shards[i].alive && z.shards[i].epoch == z.txns[id].epoch implies
        has_log(z.logs[i].durable, id) || z.obligations.dom().contains((i, id)) by {
        lemma_durable_prefix(s, c, a, i);
        lemma_prefix_records(s.logs[i].durable, z.logs[i].durable, id, z.txns[id].epoch);
        if s.txns.dom().contains(id) && prepared(s.txns[id]) {
            assert(log_participant(c, s.txns[id], i));
            assert(clock_participant(c, s.txns[id], i));
            assert(s.txns[id].epoch <= s.shards[i].epoch);
            if s.shards[i].alive && s.shards[i].epoch == s.txns[id].epoch {
                assert(has_log(s.logs[i].durable, id) || s.obligations.dom().contains((i, id)));
            }
        }
        match a {
            Action::Prepare { id: other, .. } => {
                lemma_prepare_parts(c, s.txns[other], other, (i, id));
            },
            Action::Replicate { shard } => {
                if i == shard && is_tx(s.logs[i].pending[0], id) {
                    assert(is_tx(z.logs[i].durable[s.logs[i].durable.len() as int], id));
                }
            },
            Action::Submit { .. } => {},
            Action::Read { .. } => {},
            Action::Install { .. } => {},
            Action::Publish { .. } => {},
            Action::Certify { .. } => {},
            Action::Provisional { .. } => {},
            Action::Final { .. } => {},
            Action::Abort { .. } => {},
            Action::Crash { .. } => {},
            Action::Recover { .. } => {},
            Action::ObserveEpoch { .. } => {},
            _ => {},
        }
    }
}

/// Appending a transaction or a finite Close cannot create a new time
/// frontier. Only an explicit Barrier for this shard and epoch can raise it.
pub proof fn lemma_frontier_growth(s: State, c: Constants, a: Action, i: int, ep: nat)
    requires log_inv(s, c), enabled(s, c, a), is_shard(c, i)
    ensures frontier(entries(apply(s, c, a).logs[i]), ep) <= frontier(entries(s.logs[i]), ep)
        || (a is Barrier && a->Barrier_shard == i && s.shards[i].epoch == ep
            && frontier(entries(apply(s, c, a).logs[i]), ep) == a->Barrier_through)
{
    lemma_entries_effect(s, c, a, i);
    lemma_bounds(entries(s.logs[i]), ep);
    match appended(s, a) {
        Some((j, e)) => {
            if i == j {
                lemma_push(entries(s.logs[i]), e, ep);
                match a {
                    Action::Barrier { .. } => {},
                    Action::Close { shard, epoch } => {
                        assert(e == close_entry(s, shard, epoch));
                        assert(marker_value(e, ep) <= frontier(entries(s.logs[i]), ep));
                    },
                    _ => { assert(marker_value(e, ep) == 0); },
                }
            }
        },
        None => match a {
            Action::Crash { shard, survive } => {
                if i == shard {
                    lemma_prefix(entries(s.logs[i]), (s.logs[i].durable.len() + survive) as int, ep);
                }
            },
            _ => {},
        },
    }
}

pub proof fn lemma_marker_step_at(s: State, c: Constants, a: Action, i: int, id: int)
    requires log_inv(s, c), enabled(s, c, a), is_shard(c, i),
        apply(s, c, a).txns.dom().contains(id), prepared(apply(s, c, a).txns[id]),
        log_participant(c, apply(s, c, a).txns[id], i),
        apply(s, c, a).txns[id].ts <= frontier(entries(apply(s, c, a).logs[i]), apply(s, c, a).txns[id].epoch),
    ensures has_log(apply(s, c, a).logs[i].durable, id),
{
    let z = apply(s, c, a);
    let r = z.txns[id];
    lemma_log_txn_step(s, c, a, id);
    lemma_frontier_growth(s, c, a, i, r.epoch);
    lemma_durable_prefix(s, c, a, i);
    lemma_prefix_records(s.logs[i].durable, z.logs[i].durable, id, r.epoch);
    lemma_bounds(entries(s.logs[i]), r.epoch);
    if has_log(s.logs[i].durable, id) { return; }
    if s.txns.dom().contains(id) && prepared(s.txns[id]) {
        assert(r.ts == s.txns[id].ts && r.epoch == s.txns[id].epoch);
        assert(log_participant(c, s.txns[id], i));
        if r.ts <= frontier(entries(s.logs[i]), r.epoch) {
            assert(has_log(s.logs[i].durable, id));
        } else {
            assert(a is Barrier);
            assert(a->Barrier_shard == i && s.shards[i].epoch == r.epoch);
            assert(s.shards[i].alive);
            let through = a->Barrier_through;
            assert(r.ts <= through);
            assert(has_log(s.logs[i].durable, id) || s.obligations.dom().contains((i, id)));
            if s.obligations.dom().contains((i, id)) {
                assert(s.obligations[(i, id)].epoch == r.epoch);
                assert(through < s.obligations[(i, id)].ts);
            }
        }
    } else {
        match a {
            Action::Prepare { id: other, ts } => {
                assert(id == other);
                assert(clock_participant(c, s.txns[id], i));
                assert(ts > s.shards[i].clock);
            },
            _ => {},
        }
    }
}

pub proof fn lemma_marker_step(s: State, c: Constants, a: Action)
    requires log_inv(s, c), enabled(s, c, a),
    ensures marker_inv(apply(s, c, a), c),
{
    let z = apply(s, c, a);
    assert forall|i: int, id: int| is_shard(c, i) && #[trigger] z.txns.dom().contains(id)
        && prepared(z.txns[id]) && #[trigger] log_participant(c, z.txns[id], i)
        && z.txns[id].ts <= frontier(entries(z.logs[i]), z.txns[id].epoch)
        implies has_log(z.logs[i].durable, id) by {
        lemma_marker_step_at(s, c, a, i, id);
    }
}

pub proof fn lemma_append_closed(s: State, c: Constants, a: Action, i: int, ep: nat)
    requires log_inv(s, c), enabled(s, c, a), is_shard(c, i), has_close(entries(s.logs[i]), ep),
        appended(s, a) is Some, appended(s, a).unwrap().0 == i,
    ensures marker_value(appended(s, a).unwrap().1, ep) == 0,
{
    let es = entries(s.logs[i]);
    let j = choose|j: int| 0 <= j < es.len() && #[trigger] es[j] is Close && es[j]->Close_epoch == ep;
    assert(entry_valid(s, c, i, es[j]));
    match a {
        Action::Barrier { .. } => {},
        Action::Close { epoch, .. } => {},
        _ => {},
    }
}

pub proof fn lemma_close_step_at(s: State, c: Constants, a: Action, i: int, j: int)
    requires log_inv(s, c), enabled(s, c, a), is_shard(c, i),
        0 <= j < entries(apply(s, c, a).logs[i]).len(),
        entries(apply(s, c, a).logs[i])[j] is Close,
    ensures ({
        let es = entries(apply(s, c, a).logs[i]);
        let e = es[j];
        e->Close_cut == frontier(es.take(j), e->Close_epoch)
            && e->Close_cut == frontier(es, e->Close_epoch)
    }),
{
    let es = entries(s.logs[i]);
    let ns = entries(apply(s, c, a).logs[i]);
    let e = ns[j];
    let ep = e->Close_epoch;
    lemma_entries_effect(s, c, a, i);
    lemma_bounds(ns, ep);
    if j < es.len() {
        assert(e == es[j]);
        assert(ns.take(j) =~= es.take(j));
        assert(e->Close_cut == frontier(es, ep));
        assert(has_close(es, ep));
        match appended(s, a) {
            Some((k, added)) => {
                if i == k {
                    lemma_append_closed(s, c, a, i, ep);
                    lemma_push(es, added, ep);
                }
            },
            None => match a {
                Action::Crash { shard, survive } => {
                    if i == shard { lemma_prefix(es, (s.logs[i].durable.len() + survive) as int, ep); }
                },
                _ => {},
            },
        }
    } else {
        assert(appended(s, a) is Some);
        assert(j == es.len());
        assert(ns.take(j) =~= es);
        lemma_push(es, e, ep);
        match a {
            Action::Close { .. } => {},
            _ => {},
        }
    }
}

pub proof fn lemma_close_step(s: State, c: Constants, a: Action)
    requires log_inv(s, c), enabled(s, c, a),
    ensures close_inv(apply(s, c, a), c),
{
    let z = apply(s, c, a);
    assert forall|i: int, j: int| is_shard(c, i) && 0 <= j < entries(z.logs[i]).len()
        && #[trigger] entries(z.logs[i])[j] is Close implies {
        let e = entries(z.logs[i])[j];
        e->Close_cut == frontier(entries(z.logs[i]).take(j), e->Close_epoch)
            && e->Close_cut == frontier(entries(z.logs[i]), e->Close_epoch)
    } by {
        lemma_close_step_at(s, c, a, i, j);
    }
}

pub proof fn lemma_report_step(s: State, c: Constants, a: Action, r: Report)
    requires log_inv(s, c), enabled(s, c, a), report_sound(s, c, r),
    ensures report_sound(apply(s, c, a), c, r),
{
    lemma_coverage_step(s, c, a, r.shard, r.epoch);
    if r.closed { lemma_closed_cut_step(s, c, a, r.shard, r.epoch); }
}

pub proof fn lemma_reports_step(s: State, c: Constants, a: Action)
    requires log_inv(s, c), enabled(s, c, a),
    ensures reports_inv(apply(s, c, a), c),
{
    let z = apply(s, c, a);
    assert forall|r: Report| #[trigger] z.network.contains(r) implies report_sound(z, c, r) by {
        match a {
            Action::Send { shard, epoch } => {
                if r == report(s, shard, epoch) { lemma_bounds(s.logs[shard].durable, epoch); }
            },
            _ => {},
        }
        assert(report_sound(s, c, r));
        lemma_report_step(s, c, a, r);
    }
    assert forall|p: (int, nat, int)| #[trigger] z.views.dom().contains(p) implies
        is_shard(c, p.0) && report_sound(z, c, Report { shard: p.2, epoch: p.1,
            through: z.views[p].through, closed: z.views[p].closed }) by {
        match a {
            Action::Receive { observer, report: incoming } => {
                if p == (observer, incoming.epoch, incoming.shard) {
                    assert(report_sound(s, c, incoming));
                    lemma_view_sound(s, c, observer, incoming.epoch, incoming.shard);
                }
            },
            Action::Crash { .. } => {},
            _ => {},
        }
        let r = Report { shard: p.2, epoch: p.1, through: z.views[p].through, closed: z.views[p].closed };
        assert(report_sound(s, c, r));
        lemma_report_step(s, c, a, r);
    }
}

pub proof fn lemma_log_step(s: State, c: Constants, a: Action)
    requires inv(s, c), enabled(s, c, a),
    ensures log_inv(apply(s, c, a), c),
{
    lemma_log_shape_step(s, c, a);
    lemma_obligation_step(s, c, a);
    lemma_marker_step(s, c, a);
    lemma_close_step(s, c, a);
    lemma_reports_step(s, c, a);
}

pub proof fn lemma_fenced_record_absent_step(s: State, c: Constants, a: Action, id: int, i: int)
    requires log_inv(s, c), enabled(s, c, a), s.txns.dom().contains(id), prepared(s.txns[id]),
        is_shard(c, i), log_participant(c, s.txns[id], i),
        !has_log(entries(s.logs[i]), id), !s.shards[i].alive || s.txns[id].epoch < s.shards[i].epoch,
    ensures !has_log(entries(apply(s, c, a).logs[i]), id),
        apply(s, c, a).txns.dom().contains(id),
        apply(s, c, a).txns[id].epoch == s.txns[id].epoch,
        !apply(s, c, a).shards[i].alive || apply(s, c, a).txns[id].epoch < apply(s, c, a).shards[i].epoch,
{
    let z = apply(s, c, a);
    lemma_entries_effect(s, c, a, i);
    lemma_shard_log_step(s, c, a, i);
    assert(clock_participant(c, s.txns[id], i));
    assert(s.txns[id].epoch <= s.shards[i].epoch);
    match a {
        Action::Submit { .. } => {},
        Action::Read { .. } => {},
        Action::Prepare { .. } => {},
        Action::Install { .. } => {},
        Action::Publish { .. } => {},
        Action::Certify { .. } => {},
        Action::Provisional { .. } => {},
        Action::Final { .. } => {},
        Action::Abort { .. } => {},
        Action::Crash { .. } => {},
        Action::Recover { .. } => {},
        Action::ObserveEpoch { .. } => {},
        _ => {},
    }
    assert(z.txns.dom().contains(id) && z.txns[id].epoch == s.txns[id].epoch);
    match appended(s, a) {
        Some((j, e)) => {
            if i == j {
                match a {
                    Action::Publish { id: other, .. } => { assert(other != id); },
                    Action::Certify { id: other } => { assert(other != id); },
                    _ => {},
                }
                assert(!is_tx(e, id));
                if has_log(entries(z.logs[i]), id) {
                    let p = choose|p: int| 0 <= p < entries(z.logs[i]).len()
                        && is_tx(#[trigger] entries(z.logs[i])[p], id);
                    if p < entries(s.logs[i]).len() {
                        assert(entries(z.logs[i])[p] == entries(s.logs[i])[p]);
                    }
                }
            }
        },
        None => {
            assert(entries(z.logs[i]).is_prefix_of(entries(s.logs[i])));
            lemma_prefix_records(entries(z.logs[i]), entries(s.logs[i]), id, s.txns[id].epoch);
        },
    }
}

pub proof fn lemma_version_lost_step(s: State, c: Constants, a: Action, id: int, k: int)
    requires inv(s, c), enabled(s, c, a), s.txns.dom().contains(id), prepared(s.txns[id]),
        write_set(s.txns[id].body).contains(k), version_lost(s, c, s.txns[id], id, k),
    ensures version_lost(apply(s, c, a), c, apply(s, c, a).txns[id], id, k),
{
    let i = owner(c, k);
    assert(is_shard(c, i));
    assert(writes_at(c, s.txns[id].body, i));
    lemma_fenced_record_absent_step(s, c, a, id, i);
}

} // verus!
