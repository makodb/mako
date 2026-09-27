//! Initialization, framing, and the simplest actions: Stutter, Abort, Commit.
use super::types::*;
use super::normal::*;
use super::recovery::*;
use super::behavior::*;
use super::invariants::*;
use super::stream_lemmas::*;
use vstd::prelude::*;

verus! {

pub proof fn lemma_init_inv(s: State, c: Constants)
    requires init(s, c)
    ensures inv(s, c)
{
    assert(inv_shapes(s, c)) by {
        assert forall|i: int| is_shard(c, i) implies #[trigger] shard_epoch(s.shards, i) <= s.epoch && counter(s.shards, i) >= 0 by {
            assert(s.shards[i] == ShardState { epoch: 0, counter: 0 });
        }
    }
    assert(inv_streams(s, c)) by {
        assert forall|sid: Sid| valid_sid(c, sid) implies #[trigger] inv_stream(s, c, sid) by {
            assert(all_entries(s.streams[sid]) =~= Seq::<Entry>::empty());
        }
    }
}

// ---------------------------------------------------------------------------
// Frame lemmas: an invariant conjunct survives when the components it reads
// are unchanged.
// ---------------------------------------------------------------------------

pub proof fn frame_reads(s: State, s2: State, c: Constants)
    requires inv_reads(s, c), s2.txns == s.txns, s2.versions == s.versions,
        s2.final_wm == s.final_wm, s2.shards == s.shards
    ensures inv_reads(s2, c)
{
    assert forall|id: int, k: int| #[trigger] s2.txns.dom().contains(id) && #[trigger] txn(s2.txns, id).reads.dom().contains(k)
        implies inv_read(s2, c, id, k) by {
        assert(inv_read(s, c, id, k));
    }
}

pub proof fn frame_streams(s: State, s2: State, c: Constants)
    requires inv_streams(s, c), s2.txns == s.txns, s2.streams == s.streams, s2.shards == s.shards
    ensures inv_streams(s2, c)
{
    assert forall|sid: Sid| valid_sid(c, sid) implies #[trigger] inv_stream(s2, c, sid) by {
        assert(inv_stream(s, c, sid));
    }
}

pub proof fn frame_logs(s: State, s2: State, c: Constants)
    requires inv_all_logs(s, c), s2.txns == s.txns, s2.streams == s.streams, s2.shards == s.shards,
        s2.versions == s.versions, s2.final_wm == s.final_wm, s2.rolled_back == s.rolled_back
    ensures inv_all_logs(s2, c)
{
    assert forall|id: int| #[trigger] s2.txns.dom().contains(id) implies inv_logs(s2, c, id) by {
        assert(inv_logs(s, c, id));
    }
}

pub proof fn frame_txns_tick(s: State, s2: State, c: Constants)
    requires inv_txns(s, c), s2.txns == s.txns, s2.shards == s.shards, s2.prepared == s.prepared, s2.tick >= s.tick
    ensures inv_txns(s2, c)
{
    assert forall|id: int| #[trigger] s2.txns.dom().contains(id) implies id >= 0 && inv_txn(s2, c, id) by {
        assert(inv_txn(s, c, id));
    }
}

/// Only the tick changes.
pub proof fn lemma_tick_inv(s: State, c: Constants, s2: State)
    requires inv(s, c), s2 == (State { tick: s.tick + 1, ..s })
    ensures inv(s2, c)
{
    frame_txns_tick(s, s2, c);
    frame_reads(s, s2, c);
    frame_streams(s, s2, c);
    frame_logs(s, s2, c);
}

pub proof fn lemma_stutter_inv(s: State, c: Constants)
    requires inv(s, c)
    ensures inv(apply(s, c, Action::Stutter), c)
{
    lemma_tick_inv(s, c, apply(s, c, Action::Stutter));
}

// ---------------------------------------------------------------------------
// A status-only change of one transaction record
// ---------------------------------------------------------------------------

/// Everything except `status` and `acked` is unchanged between `r` and `r2`.
pub open spec fn same_but_status(r: TxnRec, r2: TxnRec) -> bool {
    &&& r2.body == r.body && r2.coord == r.coord && r2.thread == r.thread && r2.epoch == r.epoch
    &&& r2.reads == r.reads && r2.vc == r.vc && r2.pidx == r.pidx && r2.installed == r.installed
    &&& r2.invoked == r.invoked && r2.prepared_at == r.prepared_at
}

/// The conjuncts that do not look at the changed transaction's status.
pub proof fn lemma_status_change_common(s: State, c: Constants, id: int, r2: TxnRec, s2: State)
    requires
        inv(s, c), has_txn(s.txns, id), same_but_status(txn(s.txns, id), r2),
        s2 == (State { tick: s.tick + 1, txns: s.txns.insert(id, r2), ..s }),
        // status facts the callers guarantee
        is_prepared_or_later(r2) <==> is_prepared_or_later(txn(s.txns, id)),
        in_flight(r2) ==> in_flight(txn(s.txns, id)),
    ensures
        inv_shapes(s2, c), inv_prepared(s2, c), inv_exclusive(s2, c), inv_versions(s2, c),
        inv_final(s2, c), inv_rolled_back(s2, c),
{
    let r = txn(s.txns, id);
    assert(inv_prepared(s2, c)) by {
        assert forall|j: int| 0 <= j < s2.prepared.len() implies
            has_txn(s2.txns, #[trigger] s2.prepared[j]) && txn(s2.txns, s2.prepared[j]).pidx == j
            && is_prepared_or_later(txn(s2.txns, s2.prepared[j])) by {
            assert(is_prepared_or_later(txn(s.txns, s.prepared[j])));
        }
    }
    assert(inv_versions(s2, c)) by {
        assert forall|k: int| #[trigger] s2.versions.dom().contains(k) implies inv_versions_of(s2, c, k) by {
            assert(inv_versions_of(s, c, k));
            assert forall|j: int| 0 <= j < s2.versions[k].len() implies inv_version(s2, c, k, #[trigger] s2.versions[k][j]) by {
                assert(inv_version(s, c, k, s.versions[k][j]));
            }
        }
    }
    assert(inv_final(s2, c)) by {
        assert forall|i: int, e: nat| #[trigger] s2.final_wm.dom().contains((i, e)) implies inv_final_of(s2, c, i, e) by {
            assert(inv_final_of(s, c, i, e));
        }
    }
    assert(inv_rolled_back(s2, c)) by {
        assert forall|i: int, e: nat| #[trigger] s2.rolled_back.contains((i, e)) implies inv_rolled_back_of(s2, c, i, e) by {
            assert(inv_rolled_back_of(s, c, i, e));
        }
    }
}

pub proof fn lemma_abort_inv(s: State, c: Constants, id: int)
    requires inv(s, c), can_abort(s, id)
    ensures inv(apply(s, c, Action::Abort { id }), c)
{
    let s2 = apply(s, c, Action::Abort { id });
    let r = txn(s.txns, id);
    let r2 = txn(s2.txns, id);
    assert(inv_txn(s, c, id));
    assert(r2 == TxnRec { status: Status::Aborted { prepared: false }, ..r });
    lemma_status_change_common(s, c, id, r2, s2);
    assert(inv_txns(s2, c)) by {
        assert forall|o: int| #[trigger] s2.txns.dom().contains(o) implies o >= 0 && inv_txn(s2, c, o) by {
            assert(inv_txn(s, c, o));
        }
    }
    assert(inv_locks(s2, c)) by {
        assert forall|k: int| #[trigger] s2.locks.dom().contains(k) implies inv_lock(s2, c, k) by {
            assert(inv_lock(s, c, k));
            assert(s.locks[k] != id);
        }
    }
    assert(inv_reads(s2, c)) by {
        assert forall|o: int, k: int| #[trigger] s2.txns.dom().contains(o) && #[trigger] txn(s2.txns, o).reads.dom().contains(k)
            implies inv_read(s2, c, o, k) by {
            assert(inv_read(s, c, o, k));
            let rd = txn(s.txns, o).reads[k];
            if rd.writer != -1 {
                assert(inv_read_writer(s, c, o, k));
                assert(rd.writer != id);
                assert(inv_read_writer(s2, c, o, k));
            }
            if is_prepared_or_later(txn(s2.txns, o)) {
                assert(o != id);
                assert forall|p: int| #[trigger] s2.txns.dom().contains(p) && p != o
                    && is_prepared_or_later(txn(s2.txns, p)) && txn(s2.txns, p).pidx < txn(s2.txns, o).pidx
                    && write_set(txn(s2.txns, p).body).contains(k) implies inv_read_order(s2, c, o, k, p) by {
                    assert(p != id);
                    assert(inv_read_order(s, c, o, k, p));
                }
            }
        }
    }
    assert(inv_streams(s2, c)) by {
        assert forall|sid: Sid| valid_sid(c, sid) implies #[trigger] inv_stream(s2, c, sid) by {
            assert(inv_stream(s, c, sid));
            let es = all_entries(s.streams[sid]);
            assert forall|j: int| 0 <= j < es.len() implies inv_entry(s2, c, sid, #[trigger] es[j]) by {
                assert(inv_entry(s, c, sid, es[j]));
                if es[j] is Log {
                    assert(es[j]->Log_txn != id);
                }
            }
        }
    }
    assert(inv_all_logs(s2, c)) by {
        assert forall|o: int| #[trigger] s2.txns.dom().contains(o) implies inv_logs(s2, c, o) by {
            assert(inv_logs(s, c, o));
            if o == id {
                assert(r.installed == Set::<int>::empty());
                assert forall|i: int| is_shard(c, i) && #[trigger] logged_at(c, r2, i) implies false by {}
                assert forall|k: int| #[trigger] write_set(r2.body).contains(k) && r2.installed.contains(owner(c, k)) implies false by {}
            } else {
                let ro = txn(s.txns, o);
                assert(txn(s2.txns, o) == ro);
                assert forall|k: int| #[trigger] write_set(ro.body).contains(k) && ro.installed.contains(owner(c, k))
                    && !has_version(s2.versions, k, o) implies
                    (doomed(s2.final_wm, c, ro) && s2.rolled_back.contains((owner(c, k), ro.epoch))) || lost(s2.streams, c, ro, o) by {
                }
            }
        }
    }
    assert(inv_committed(s2, c)) by {
        assert forall|o: int| #[trigger] s2.txns.dom().contains(o) && committed(txn(s2.txns, o))
            implies below_wm(s2.streams, c, txn(s2.txns, o).vc, txn(s2.txns, o).epoch) by {
            assert(o != id);
        }
    }
}

pub proof fn lemma_commit_inv(s: State, c: Constants, id: int)
    requires inv(s, c), can_commit(s, c, id)
    ensures inv(apply(s, c, Action::Commit { id }), c)
{
    let s2 = apply(s, c, Action::Commit { id });
    let r = txn(s.txns, id);
    let r2 = txn(s2.txns, id);
    assert(inv_txn(s, c, id));
    assert(r2 == TxnRec { status: Status::Committed, acked: s.tick, ..r });
    lemma_status_change_common(s, c, id, r2, s2);
    assert(inv_txns(s2, c)) by {
        assert forall|o: int| #[trigger] s2.txns.dom().contains(o) implies o >= 0 && inv_txn(s2, c, o) by {
            assert(inv_txn(s, c, o));
            if o == id {
                assert(is_prepared_or_later(r));
                assert(r2.prepared_at == r.prepared_at);
                assert(r.prepared_at < s.tick);
                assert(r2.acked == s.tick);
                assert(s2.tick == s.tick + 1);
                assert(r2.prepared_at < r2.acked < s2.tick);
                assert(certified_or_committed(r2) ==> all_installed(c, r2));
                assert(r2.pidx < s2.prepared.len() && s2.prepared[r2.pidx as int] == id);
                assert forall|i: int| is_shard(c, i) && #[trigger] clock_shard(c, r2, i) implies r2.vc[group(c, i)] >= 1 by {
                    assert(clock_shard(c, r, i));
                }
                assert(inv_txn(s2, c, id));
            }
        }
    }
    assert(inv_locks(s2, c)) by {
        assert forall|k: int| #[trigger] s2.locks.dom().contains(k) implies inv_lock(s2, c, k) by {
            assert(inv_lock(s, c, k));
            assert(s.locks[k] != id);
        }
        assert forall|o: int, k: int| #[trigger] s2.txns.dom().contains(o) && txn(s2.txns, o).status is Prepared
            && #[trigger] write_set(txn(s2.txns, o).body).contains(k)
            && !txn(s2.txns, o).installed.contains(owner(c, k))
            && shard_epoch(s2.shards, owner(c, k)) == txn(s2.txns, o).epoch
            implies s2.locks.dom().contains(k) && s2.locks[k] == o by {
            assert(o != id);
        }
    }
    assert(inv_reads(s2, c)) by {
        assert forall|o: int, k: int| #[trigger] s2.txns.dom().contains(o) && #[trigger] txn(s2.txns, o).reads.dom().contains(k)
            implies inv_read(s2, c, o, k) by {
            assert(inv_read(s, c, o, k));
            let rd = txn(s.txns, o).reads[k];
            if rd.writer != -1 {
                assert(inv_read_writer(s, c, o, k));
                assert(inv_read_writer(s2, c, o, k));
            }
            if is_prepared_or_later(txn(s2.txns, o)) {
                assert forall|p: int| #[trigger] s2.txns.dom().contains(p) && p != o
                    && is_prepared_or_later(txn(s2.txns, p)) && txn(s2.txns, p).pidx < txn(s2.txns, o).pidx
                    && write_set(txn(s2.txns, p).body).contains(k) implies inv_read_order(s2, c, o, k, p) by {
                    assert(inv_read_order(s, c, o, k, p));
                }
            }
        }
    }
    assert(inv_streams(s2, c)) by {
        assert forall|sid: Sid| valid_sid(c, sid) implies #[trigger] inv_stream(s2, c, sid) by {
            assert(inv_stream(s, c, sid));
            let es = all_entries(s.streams[sid]);
            assert forall|j: int| 0 <= j < es.len() implies inv_entry(s2, c, sid, #[trigger] es[j]) by {
                assert(inv_entry(s, c, sid, es[j]));
            }
        }
    }
    assert(inv_all_logs(s2, c)) by {
        assert forall|o: int| #[trigger] s2.txns.dom().contains(o) implies inv_logs(s2, c, o) by {
            assert(inv_logs(s, c, o));
            let ro = txn(s.txns, o);
            let ro2 = txn(s2.txns, o);
            assert forall|i: int| is_shard(c, i) && #[trigger] logged_at(c, ro2, i) implies
                has_log(stream_at(s2.streams, ro2, i), o)
                || (shard_epoch(s2.shards, i) > ro2.epoch && stream_below(stream_at(s2.streams, ro2, i), ro2.epoch, ro2.vc[group(c, i)])) by {
                assert(logged_at(c, ro, i));
            }
            assert forall|k: int| #[trigger] write_set(ro2.body).contains(k) && ro2.installed.contains(owner(c, k))
                && !has_version(s2.versions, k, o) implies
                (doomed(s2.final_wm, c, ro2) && s2.rolled_back.contains((owner(c, k), ro2.epoch))) || lost(s2.streams, c, ro2, o) by {
                if lost(s.streams, c, ro, o) {
                    let i = choose|i: int| is_shard(c, i) && #[trigger] logged_at(c, ro, i) && !has_log(stream_at(s.streams, ro, i), o);
                    assert(logged_at(c, ro2, i));
                    assert(!has_log(stream_at(s2.streams, ro2, i), o));
                }
            }
        }
    }
    assert(inv_committed(s2, c)) by {
        assert forall|o: int| #[trigger] s2.txns.dom().contains(o) && committed(txn(s2.txns, o))
            implies below_wm(s2.streams, c, txn(s2.txns, o).vc, txn(s2.txns, o).epoch) by {
        }
    }
}

} // verus!
