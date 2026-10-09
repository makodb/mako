//! Epoch control shares shard 0's Raft log. Detection/proposal, durable epoch
//! advancement, each shard's observation, close-record replication, gossip,
//! and rollback are separate actions. Closure never manufactures infinity.
use super::types::*;
use super::normal::*;
use vstd::prelude::*;

verus! {

pub open spec fn abort_coordinated(txns: Map<int, TxnRec>, shard: int) -> Map<int, TxnRec> {
    txns.map_entries(|id: int, r: TxnRec| if r.coord == shard && in_flight(r) {
        TxnRec { status: Status::Aborted { prepared: r.status is Prepared }, ..r }
    } else { r })
}
pub open spec fn released_locks(s: State, c: Constants, shard: int) -> Map<int, int> {
    s.locks.filter_keys(|k: int| owner(c, k) != shard && s.txns[s.locks[k]].coord != shard)
}
/// CM consensus can recover and advance the epoch before shard 0's
/// transaction-serving state resumes; otherwise its own failure deadlocks.
pub open spec fn can_propose_epoch(s: State) -> bool {
    forall|j: int| 0 <= j < s.logs[0].pending.len() ==>
        !(#[trigger] s.logs[0].pending[j] is AdvanceSpecEpoch)
}
pub open spec fn propose_epoch(s: State) -> State {
    State { logs: s.logs.update(0, append(s.logs[0], Entry::AdvanceSpecEpoch { epoch: cm_epoch(s) + 1 })), ..s }
}

/// Consensus can preserve an additional prefix of the previously uncommitted
/// suffix. It cannot lose or rewrite the already durable prefix. These are
/// Raft's abstract guarantees under the stated durable-quorum failure model.
pub open spec fn can_crash(s: State, c: Constants, shard: int, survive: nat) -> bool {
    is_shard(c, shard) && survive <= s.logs[shard].pending.len()
}
pub open spec fn crash(s: State, c: Constants, shard: int, survive: nat) -> State {
    let kept = s.logs[shard].durable + s.logs[shard].pending.take(survive as int);
    State {
        logs: s.logs.update(shard, RaftLog { durable: kept, pending: Seq::empty() }),
        shards: s.shards.update(shard, ShardState { alive: false, ..s.shards[shard] }),
        obligations: s.obligations.filter_keys(|p: (int, int)| p.0 != shard),
        versions: Map::new(s.versions.dom(), |k: int| if owner(c, k) == shard {
            s.versions[k].filter(|v: Version| has_log(kept, v.txn))
        } else { s.versions[k] }),
        locks: released_locks(s, c, shard),
        txns: abort_coordinated(s.txns, shard),
        views: s.views.filter_keys(|p: (int, nat, int)| p.0 != shard),
        ..s
    }
}
/// A recovered shard remains fenced from its old epoch. Its timestamp service
/// obtains a valid exclusive origin lease and floors allocation at all
/// retained records/barriers; origin-service internals are an interface.
pub open spec fn can_recover(s: State, c: Constants, shard: int, floor: int) -> bool {
    &&& is_shard(c, shard) && !s.shards[shard].alive
    &&& s.shards[shard].epoch < cm_epoch(s)
    &&& floor >= clock_floor(s.logs[shard].durable) && floor >= 0
}
pub open spec fn recover(s: State, shard: int, floor: int) -> State {
    State { shards: s.shards.update(shard, ShardState {
        alive: true, epoch: s.shards[shard].epoch + 1, clock: floor,
    }), ..s }
}
pub open spec fn can_observe_epoch(s: State, c: Constants, shard: int) -> bool {
    is_shard(c, shard) && s.shards[shard].alive && s.shards[shard].epoch < cm_epoch(s)
}
pub open spec fn observe_epoch(s: State, c: Constants, shard: int) -> State {
    State {
        shards: s.shards.update(shard, ShardState { epoch: s.shards[shard].epoch + 1, ..s.shards[shard] }),
        obligations: s.obligations.filter_keys(|p: (int, int)| p.0 != shard),
        locks: released_locks(s, c, shard),
        txns: abort_coordinated(s.txns, shard),
        ..s
    }
}
/// All old proposals precede this close record in this shard's single log.
/// Committing Close therefore commits every surviving proposed old barrier.
/// A crash before Close commits may discard it; the replacement recomputes
/// the cut from its retained prefix. No consumer trusts an uncommitted Close.
pub open spec fn can_close(s: State, c: Constants, shard: int, epoch: nat) -> bool {
    is_shard(c, shard) && s.shards[shard].alive && epoch < s.shards[shard].epoch
        && !has_close(entries(s.logs[shard]), epoch)
}
pub open spec fn close_entry(s: State, shard: int, epoch: nat) -> Entry {
    let cut = frontier(entries(s.logs[shard]), epoch);
    Entry::Close { epoch, cut }
}
pub open spec fn close(s: State, shard: int, epoch: nat) -> State {
    State { logs: s.logs.update(shard, append(s.logs[shard], close_entry(s, shard, epoch))), ..s }
}
pub open spec fn can_rollback(s: State, c: Constants, shard: int, epoch: nat) -> bool {
    is_shard(c, shard) && s.shards[shard].alive && final_ready(s.views, c, shard, epoch)
        && !s.rolled_back.contains((shard, epoch))
}
pub open spec fn rollback(s: State, c: Constants, shard: int, epoch: nat) -> State {
    State {
        versions: Map::new(s.versions.dom(), |k: int| if owner(c, k) == shard {
            s.versions[k].filter(|v: Version| v.epoch != epoch || below_view(s.views, c, shard, epoch, v.ts))
        } else { s.versions[k] }),
        rolled_back: s.rolled_back.insert((shard, epoch)),
        ..s
    }
}

} // verus!
