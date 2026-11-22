//! Preservation proof for Prepare (Lock + GetClock + Validate).
use super::types::*;
use super::normal::*;
use super::recovery::*;
use super::behavior::*;
use super::invariants::*;
use super::stream_lemmas::*;
use super::proofs_basic::*;
use vstd::prelude::*;

verus! {

/// A present version of `o` for key `k` was certified no later than the top writer.
pub proof fn lemma_top_writer_bounds(s: State, c: Constants, k: int, o: int)
    requires inv(s, c), has_version(s.versions, k, o)
    ensures s.versions.dom().contains(k), pidx_of(s.txns, o) <= pidx_of(s.txns, top_writer(s.versions, k))
{
    let j = choose|j: int| 0 <= j < vers(s.versions, k).len() && #[trigger] vers(s.versions, k)[j].txn == o;
    assert(s.versions.dom().contains(k));
    assert(inv_versions_of(s, c, k));
    let m = s.versions[k].len() - 1;
    assert(inv_version(s, c, k, s.versions[k][j]));
    assert(inv_version(s, c, k, s.versions[k][m]));
    assert(o >= 0);
    assert(s.versions[k][m].txn >= 0);
    if j < m {
        assert(txn(s.txns, s.versions[k][j].txn).pidx < txn(s.txns, s.versions[k][m].txn).pidx);
    }
}

pub proof fn lemma_read_only_no_writes(c: Constants, r: TxnRec)
    requires read_only(r)
    ensures forall|i: int| !(#[trigger] writes_at(c, r.body, i)), forall|i: int| !(#[trigger] clock_shard(c, r, i))
{
    assert forall|i: int| !(#[trigger] writes_at(c, r.body, i)) by {
        if writes_at(c, r.body, i) {
            let k = choose|k: int| #[trigger] write_set(r.body).contains(k) && owner(c, k) == i;
            assert(write_set(r.body).contains(k));
        }
    }
}

pub proof fn lemma_prepare_inv(s: State, c: Constants, id: int, vc: Seq<int>)
    requires inv(s, c), can_prepare(s, c, id, vc)
    ensures inv(apply(s, c, Action::Prepare { id, vc }), c)
{
    let s2 = apply(s, c, Action::Prepare { id, vc });
    let r = txn(s.txns, id);
    let r2 = txn(s2.txns, id);
    let n = s.prepared.len() as int;
    assert(inv_txn(s, c, id));
    assert(r2 == TxnRec {
        status: if read_only(r) { Status::Certified } else { Status::Prepared },
        vc, pidx: s.prepared.len(), prepared_at: s.tick, ..r });
    assert(r2.body == r.body && r2.coord == r.coord && r2.thread == r.thread && r2.epoch == r.epoch
        && r2.reads == r.reads && r2.installed == r.installed && r2.invoked == r.invoked);
    assert(is_prepared_or_later(r2));
    assert(r.status is Running);
    assert(!is_prepared_or_later(r));
    assert(r.installed == Set::<int>::empty());
    // shard facts
    assert forall|i: int| 0 <= i < c.shards implies
        #[trigger] s2.shards[i].epoch == s.shards[i].epoch && s2.shards[i].counter >= s.shards[i].counter by {
        if clock_shard(c, r, i) {
            assert(s2.shards[i] == ShardState { counter: s.shards[i].counter + 1, ..s.shards[i] });
        } else {
            assert(s2.shards[i] == s.shards[i]);
        }
    }
    assert(s2.shards.len() == c.shards);
    assert forall|i: int| is_shard(c, i) && #[trigger] clock_shard(c, r, i) implies s2.shards[i].counter == s.shards[i].counter + 1 by {}
    assert(forall|i: int| #[trigger] clock_shard(c, r2, i) == clock_shard(c, r, i));
    // other records unchanged
    assert(forall|o: int| o != id ==> #[trigger] txn(s2.txns, o) == txn(s.txns, o));
    // every prepared transaction of s has index below n
    assert forall|o: int| #[trigger] s.txns.dom().contains(o) && is_prepared_or_later(txn(s.txns, o)) implies
        txn(s.txns, o).pidx < n && o != id by {
        assert(inv_txn(s, c, o));
    }
    assert(s2.prepared == s.prepared.push(id));
    assert(s2.prepared[n] == id);
    assert(forall|j: int| 0 <= j < n ==> s2.prepared[j] == s.prepared[j]);

    assert(inv_shapes(s2, c)) by {
        assert forall|i: int| is_shard(c, i) implies #[trigger] shard_epoch(s2.shards, i) <= s2.epoch && counter(s2.shards, i) >= 0 by {
            assert(shard_epoch(s.shards, i) <= s.epoch && counter(s.shards, i) >= 0);
        }
    }
    assert(inv_txns(s2, c)) by {
        assert forall|o: int| #[trigger] s2.txns.dom().contains(o) implies o >= 0 && inv_txn(s2, c, o) by {
            assert(inv_txn(s, c, o));
            if o == id {
                assert forall|x: int| is_comp(c, x) implies #[trigger] r2.vc[x] >= 0 by {
                    assert(exact_component(s, c, r, vc, x));
                    if vc[x] == 0 {
                    } else if exists|k: int| #[trigger] r.reads.dom().contains(k) && r.reads[k].writer != -1
                        && r.reads[k].epoch == r.epoch && r.reads[k].vc[x] == vc[x] {
                        let k = choose|k: int| #[trigger] r.reads.dom().contains(k) && r.reads[k].writer != -1
                            && r.reads[k].epoch == r.epoch && r.reads[k].vc[x] == vc[x];
                        assert(inv_read(s, c, id, k));
                        assert(inv_read_writer(s, c, id, k));
                        let w = txn(s.txns, r.reads[k].writer);
                        assert(inv_txn(s, c, r.reads[k].writer));
                        assert(w.vc[x] >= 0);
                    } else {
                        let i = choose|i: int| is_shard(c, i) && #[trigger] clock_shard(c, r, i) && group(c, i) == x
                            && vc[x] == s.shards[i].counter + 1;
                        assert(counter(s.shards, i) >= 0);
                    }
                }
                assert forall|i: int| is_shard(c, i) && #[trigger] clock_shard(c, r2, i) implies r2.vc[group(c, i)] >= 1 by {
                    assert(clock_shard(c, r, i));
                    assert(vc[group(c, i)] >= s.shards[i].counter + 1);
                    assert(counter(s.shards, i) >= 0);
                }
                assert forall|i: int| #[trigger] r2.installed.contains(i) implies is_shard(c, i) && writes_at(c, r2.body, i) by {}
                assert forall|k: int| #[trigger] r2.reads.dom().contains(k) implies read_set(r2.body).contains(k) by {}
                if read_only(r) {
                    lemma_read_only_no_writes(c, r2);
                    assert(all_installed(c, r2));
                }
                assert(r2.pidx < s2.prepared.len() && s2.prepared[r2.pidx as int] == id);
                assert(r2.invoked < r2.prepared_at < s2.tick);
                assert forall|k: int| #[trigger] read_set(r2.body).contains(k) implies r2.reads.dom().contains(k) by {
                    assert(validated(s, c, r));
                }
                assert(inv_txn(s2, c, id));
            } else {
                assert(txn(s2.txns, o) == txn(s.txns, o));
                let ro = txn(s.txns, o);
                assert(ro.epoch <= shard_epoch(s2.shards, ro.coord));
                if is_prepared_or_later(ro) {
                    assert(ro.pidx < n);
                    assert(s2.prepared[ro.pidx as int] == s.prepared[ro.pidx as int]);
                }
            }
        }
    }
    assert(inv_prepared(s2, c)) by {
        assert forall|j: int| 0 <= j < s2.prepared.len() implies
            has_txn(s2.txns, #[trigger] s2.prepared[j]) && txn(s2.txns, s2.prepared[j]).pidx == j
            && is_prepared_or_later(txn(s2.txns, s2.prepared[j])) by {
            if j < n {
                assert(s2.prepared[j] == s.prepared[j]);
                assert(is_prepared_or_later(txn(s.txns, s.prepared[j])));
                assert(s.prepared[j] != id);
            }
        }
        assert forall|j1: int, j2: int| 0 <= j1 < j2 < s2.prepared.len() implies
            txn(s2.txns, #[trigger] s2.prepared[j1]).prepared_at < txn(s2.txns, #[trigger] s2.prepared[j2]).prepared_at by {
            assert(s2.prepared[j1] == s.prepared[j1]);
            assert(is_prepared_or_later(txn(s.txns, s.prepared[j1])));
            assert(s.prepared[j1] != id);
            if j2 < n {
                assert(s2.prepared[j2] == s.prepared[j2]);
                assert(s.prepared[j2] != id);
            } else {
                assert(s2.prepared[j2] == id);
                assert(inv_txn(s, c, s.prepared[j1]));
                assert(txn(s.txns, s.prepared[j1]).prepared_at < s.tick);
            }
        }
    }
    assert(inv_exclusive(s2, c)) by {
        assert forall|a: int, b: int| #[trigger] s2.txns.dom().contains(a) && #[trigger] s2.txns.dom().contains(b) && a != b
            && txn(s2.txns, a).coord == txn(s2.txns, b).coord && txn(s2.txns, a).thread == txn(s2.txns, b).thread
            implies !(in_flight(txn(s2.txns, a)) && in_flight(txn(s2.txns, b))) by {
            assert(in_flight(txn(s2.txns, a)) ==> in_flight(txn(s.txns, a)));
            assert(in_flight(txn(s2.txns, b)) ==> in_flight(txn(s.txns, b)));
        }
    }
    let ws = write_set(r.body);
    assert(s2.locks == s.locks.union_prefer_right(Map::new(ws, |k: int| id)));
    assert forall|k: int| #[trigger] ws.contains(k) implies !s.locks.dom().contains(k) && s2.locks.dom().contains(k) && s2.locks[k] == id by {
        assert(validated(s, c, r));
    }
    assert forall|k: int| !ws.contains(k) implies (#[trigger] s2.locks.dom().contains(k) <==> s.locks.dom().contains(k))
        && (s.locks.dom().contains(k) ==> s2.locks[k] == s.locks[k]) by {}
    assert(inv_locks(s2, c)) by {
        assert forall|k: int| #[trigger] s2.locks.dom().contains(k) implies inv_lock(s2, c, k) by {
            if ws.contains(k) {
                assert(s2.locks[k] == id);
                assert(r2.status is Prepared);
                assert forall|j: int| 0 <= j < vers(s2.versions, k).len() implies
                    pidx_of(s2.txns, #[trigger] vers(s2.versions, k)[j].txn) < r2.pidx by {
                    assert(s.versions.dom().contains(k));
                    assert(inv_versions_of(s, c, k));
                    assert(inv_version(s, c, k, s.versions[k][j]));
                    let o = s.versions[k][j].txn;
                    assert(is_prepared_or_later(txn(s.txns, o)));
                    assert(txn(s.txns, o).pidx < n);
                }
            } else {
                assert(inv_lock(s, c, k));
                let h = s.locks[k];
                assert(h != id);
                assert forall|j: int| 0 <= j < vers(s2.versions, k).len() implies
                    pidx_of(s2.txns, #[trigger] vers(s2.versions, k)[j].txn) < txn(s2.txns, h).pidx by {
                    assert(pidx_of(s.txns, vers(s.versions, k)[j].txn) < txn(s.txns, h).pidx);
                    assert(s.versions.dom().contains(k));
                    assert(inv_versions_of(s, c, k));
                    assert(inv_version(s, c, k, s.versions[k][j]));
                    assert(vers(s.versions, k)[j].txn != id);
                }
            }
        }
        assert forall|o: int, k: int| #[trigger] s2.txns.dom().contains(o) && txn(s2.txns, o).status is Prepared
            && #[trigger] write_set(txn(s2.txns, o).body).contains(k)
            && !txn(s2.txns, o).installed.contains(owner(c, k))
            && shard_epoch(s2.shards, owner(c, k)) == txn(s2.txns, o).epoch
            implies s2.locks.dom().contains(k) && s2.locks[k] == o by {
            if o == id {
                assert(ws.contains(k));
            } else {
                assert(shard_epoch(s.shards, owner(c, k)) == txn(s.txns, o).epoch);
                assert(s.locks.dom().contains(k) && s.locks[k] == o);
                assert(!ws.contains(k));
            }
        }
    }
    assert(inv_versions(s2, c)) by {
        assert forall|k: int| #[trigger] s2.versions.dom().contains(k) implies inv_versions_of(s2, c, k) by {
            assert(inv_versions_of(s, c, k));
            assert forall|j: int| 0 <= j < s2.versions[k].len() implies inv_version(s2, c, k, #[trigger] s2.versions[k][j]) by {
                assert(inv_version(s, c, k, s.versions[k][j]));
                assert(s.versions[k][j].txn != id);
            }
            assert forall|a: int, b: int| 0 <= a < b < s2.versions[k].len() implies
                txn(s2.txns, #[trigger] s2.versions[k][a].txn).pidx < txn(s2.txns, #[trigger] s2.versions[k][b].txn).pidx by {
                assert(inv_version(s, c, k, s.versions[k][a]));
                assert(inv_version(s, c, k, s.versions[k][b]));
            }
        }
    }
    assert(inv_reads(s2, c)) by {
        assert forall|o: int, k: int| #[trigger] s2.txns.dom().contains(o) && #[trigger] txn(s2.txns, o).reads.dom().contains(k)
            implies inv_read(s2, c, o, k) by {
            assert(inv_read(s, c, o, k));
            let rd = txn(s.txns, o).reads[k];
            if rd.writer != -1 {
                assert(inv_read_writer(s, c, o, k));
                let w = txn(s.txns, rd.writer);
                assert(is_prepared_or_later(w));
                assert(rd.writer != id);
                if o == id {
                    assert(w.pidx < n);
                    assert(w.epoch == r.epoch ==> vc_le(w.vc, vc, c.comp)) by {
                        assert(merge_lower_bound(c, r, vc));
                    }
                }
                assert(inv_read_writer(s2, c, o, k));
            }
            if o == id {
                assert(read_set(r.body).contains(k));
                assert(validated(s, c, r));
                assert(top_writer(s.versions, k) == rd.writer);
                assert(!s.locks.dom().contains(k));
                assert forall|p: int| #[trigger] s2.txns.dom().contains(p) && p != id
                    && is_prepared_or_later(txn(s2.txns, p)) && txn(s2.txns, p).pidx < r2.pidx
                    && write_set(txn(s2.txns, p).body).contains(k) implies inv_read_order(s2, c, id, k, p) by {
                    let rp = txn(s.txns, p);
                    assert(inv_txn(s, c, p));
                    if rp.installed.contains(owner(c, k)) && has_version(s2.versions, k, p) {
                        lemma_top_writer_bounds(s, c, k, p);
                    }
                    if !rp.installed.contains(owner(c, k)) {
                        if rp.status is Prepared {
                            if shard_epoch(s.shards, owner(c, k)) == rp.epoch {
                                assert(s.locks.dom().contains(k));
                                assert(false);
                            }
                        } else if certified_or_committed(rp) {
                            assert(all_installed(c, rp));
                            assert(writes_at(c, rp.body, owner(c, k)));
                            assert(false);
                        }
                    }
                }
            } else if is_prepared_or_later(txn(s2.txns, o)) {
                assert forall|p: int| #[trigger] s2.txns.dom().contains(p) && p != o
                    && is_prepared_or_later(txn(s2.txns, p)) && txn(s2.txns, p).pidx < txn(s2.txns, o).pidx
                    && write_set(txn(s2.txns, p).body).contains(k) implies inv_read_order(s2, c, o, k, p) by {
                    assert(txn(s.txns, o).pidx < n);
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
                    assert(logged_at(c, txn(s.txns, es[j]->Log_txn), sid.shard));
                    assert(es[j]->Log_txn != id);
                }
            }
        }
    }
    assert(inv_all_logs(s2, c)) by {
        assert forall|o: int| #[trigger] s2.txns.dom().contains(o) implies inv_logs(s2, c, o) by {
            assert(inv_logs(s, c, o));
            if o == id {
                let e = r.epoch;
                assert(e == shard_epoch(s.shards, r.coord));
                // Every epoch-e log entry on a clock shard's dedicated stream is below the new clock.
                assert forall|i: int| is_shard(c, i) && #[trigger] clock_shard(c, r, i) implies
                    logs_below(stream_at(s2.streams, r2, i), e, vc[group(c, i)]) && shard_epoch(s.shards, i) == e by {
                    if i != r.coord {
                        let k = choose|k: int| #[trigger] write_set(r.body).contains(k) && owner(c, k) == i;
                        assert(validated(s, c, r));
                        assert(shard_epoch(s.shards, owner(c, k)) == e);
                    }
                    let sid = coord_sid(r, i);
                    assert(valid_sid(c, sid));
                    assert(inv_stream(s, c, sid));
                    let es = all_entries(s.streams[sid]);
                    assert(stream_at(s2.streams, r2, i) == es);
                    assert(vc[group(c, i)] >= s.shards[i].counter + 1);
                    assert forall|j: int| 0 <= j < es.len() && #[trigger] es[j] is Log && es[j]->Log_epoch == e implies es[j]->Log_clock < vc[group(c, i)] by {
                        assert(inv_entry(s, c, sid, es[j]));
                        assert(es[j]->Log_clock <= counter(s.shards, i));
                    }
                }
                if !read_only(r) {
                    assert(clock_shard(c, r, r.coord));
                    let sid = coord_sid(r, r.coord);
                    let es = all_entries(s.streams[sid]);
                    assert(inv_stream(s, c, sid));
                    assert forall|j: int| 0 <= j < es.len() implies !(#[trigger] es[j] is Inf && es[j]->Inf_epoch == e) by {
                        assert(inv_entry(s, c, sid, es[j]));
                    }
                    assert(stream_below(stream_at(s2.streams, r2, r.coord), e, vc[group(c, r.coord)]));
                }
                assert forall|i: int| is_shard(c, i) && #[trigger] logged_at(c, r2, i) implies false by {
                    if read_only(r) { lemma_read_only_no_writes(c, r2); }
                }
                assert forall|k: int| #[trigger] write_set(r2.body).contains(k) && r2.installed.contains(owner(c, k)) implies false by {}
            } else {
                let ro = txn(s.txns, o);
                assert(txn(s2.txns, o) == ro);
                assert forall|i: int| is_shard(c, i) && #[trigger] logged_at(c, ro, i) implies
                    has_log(stream_at(s2.streams, ro, i), o)
                    || (shard_epoch(s2.shards, i) > ro.epoch && stream_below(stream_at(s2.streams, ro, i), ro.epoch, ro.vc[group(c, i)])) by {
                    assert(shard_epoch(s2.shards, i) == shard_epoch(s.shards, i));
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
    assert(inv_final(s2, c)) by {
        assert forall|i: int, e: nat| #[trigger] s2.final_wm.dom().contains((i, e)) implies inv_final_of(s2, c, i, e) by {
            assert(inv_final_of(s, c, i, e));
            assert(shard_epoch(s2.shards, i) == shard_epoch(s.shards, i));
        }
    }
    assert(inv_rolled_back(s2, c)) by {
        assert forall|i: int, e: nat| #[trigger] s2.rolled_back.contains((i, e)) implies inv_rolled_back_of(s2, c, i, e) by {
            assert(inv_rolled_back_of(s, c, i, e));
        }
    }
}

} // verus!
