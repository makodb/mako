//! Preservation proofs: Submit, Read, Replicate.
use super::types::*;
use super::normal::*;
use super::recovery::*;
use super::behavior::*;
use super::invariants::*;
use super::stream_lemmas::*;
use super::proofs_basic::*;
use vstd::prelude::*;

verus! {

pub proof fn lemma_submit_inv(s: State, c: Constants, id: int, body: Txn, coord: int, thread: int)
    requires inv(s, c), can_submit(s, c, id, body, coord, thread)
    ensures inv(apply(s, c, Action::Submit { id, body, coord, thread }), c)
{
    let s2 = apply(s, c, Action::Submit { id, body, coord, thread });
    let r2 = txn(s2.txns, id);
    assert(r2.status is Running);
    assert(inv_txns(s2, c)) by {
        assert forall|o: int| #[trigger] s2.txns.dom().contains(o) implies o >= 0 && inv_txn(s2, c, o) by {
            if o == id {
                assert(r2.installed =~= Set::<int>::empty());
                assert forall|x: int| is_comp(c, x) implies #[trigger] r2.vc[x] >= 0 by {
                    assert(r2.vc[x] == 0);
                }
            } else {
                assert(inv_txn(s, c, o));
            }
        }
    }
    assert(inv_prepared(s2, c)) by {
        assert forall|j: int| 0 <= j < s2.prepared.len() implies
            has_txn(s2.txns, #[trigger] s2.prepared[j]) && txn(s2.txns, s2.prepared[j]).pidx == j
            && is_prepared_or_later(txn(s2.txns, s2.prepared[j])) by {
            assert(has_txn(s.txns, s.prepared[j]));
            assert(s.prepared[j] != id);
        }
        assert forall|j1: int, j2: int| 0 <= j1 < j2 < s2.prepared.len() implies
            txn(s2.txns, #[trigger] s2.prepared[j1]).prepared_at < txn(s2.txns, #[trigger] s2.prepared[j2]).prepared_at by {
            assert(has_txn(s.txns, s.prepared[j1]));
            assert(has_txn(s.txns, s.prepared[j2]));
        }
    }
    assert(inv_exclusive(s2, c)) by {
        assert forall|a: int, b: int| #[trigger] s2.txns.dom().contains(a) && #[trigger] s2.txns.dom().contains(b) && a != b
            && txn(s2.txns, a).coord == txn(s2.txns, b).coord && txn(s2.txns, a).thread == txn(s2.txns, b).thread
            implies !(in_flight(txn(s2.txns, a)) && in_flight(txn(s2.txns, b))) by {
            if a == id {
                assert(!in_flight(txn(s.txns, b)));
            } else if b == id {
                assert(!in_flight(txn(s.txns, a)));
            }
        }
    }
    assert(inv_locks(s2, c)) by {
        assert forall|k: int| #[trigger] s2.locks.dom().contains(k) implies inv_lock(s2, c, k) by {
            assert(inv_lock(s, c, k));
            assert(has_txn(s.txns, s.locks[k]));
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
            assert(o != id);
            assert(inv_read(s, c, o, k));
            let rd = txn(s.txns, o).reads[k];
            if rd.writer != -1 {
                assert(inv_read_writer(s, c, o, k));
                assert(rd.writer != id);
                assert(inv_read_writer(s2, c, o, k));
            }
            if is_prepared_or_later(txn(s2.txns, o)) {
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
                    assert(has_txn(s.txns, es[j]->Log_txn));
                    assert(es[j]->Log_txn != id);
                }
            }
        }
    }
    assert(inv_all_logs(s2, c)) by {
        assert forall|o: int| #[trigger] s2.txns.dom().contains(o) implies inv_logs(s2, c, o) by {
            if o == id {
                assert forall|i: int| is_shard(c, i) && #[trigger] logged_at(c, r2, i) implies false by {}
                assert forall|k: int| #[trigger] write_set(r2.body).contains(k) && r2.installed.contains(owner(c, k)) implies false by {}
            } else {
                assert(inv_logs(s, c, o));
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
        }
    }
    assert(inv_rolled_back(s2, c)) by {
        assert forall|i: int, e: nat| #[trigger] s2.rolled_back.contains((i, e)) implies inv_rolled_back_of(s2, c, i, e) by {
            assert(inv_rolled_back_of(s, c, i, e));
        }
    }
}

pub proof fn lemma_read_inv(s: State, c: Constants, id: int, k: int)
    requires inv(s, c), can_read(s, c, id, k)
    ensures inv(apply(s, c, Action::Read { id, key: k }), c)
{
    let s2 = apply(s, c, Action::Read { id, key: k });
    let r = txn(s.txns, id);
    let r2 = txn(s2.txns, id);
    let rd = read_rec(s.versions, k);
    assert(inv_txn(s, c, id));
    assert(r2 == TxnRec { reads: r.reads.insert(k, rd), ..r });
    // The new read record satisfies inv_read.
    assert(r2.reads[k] == rd);
    assert(r2.status is Running);
    assert(!is_prepared_or_later(r2));
    assert(inv_read(s2, c, id, k)) by {
        let vs = vers(s.versions, k);
        if vs.len() == 0 {
            assert(rd.writer == -1 && rd.value == 0);
            assert(rd.writer == -1 ==> rd.value == 0);
            assert(rd.writer != -1 ==> inv_read_writer(s2, c, id, k));
        } else {
            let v = vs.last();
            assert(s.versions.dom().contains(k));
            assert(inv_versions_of(s, c, k));
            assert(inv_version(s, c, k, v));
            let w = txn(s.txns, v.txn);
            assert(is_prepared_or_later(w));
            assert(v.txn != id);
            assert(v.txn >= 0);
            assert(rd.writer != -1);
            assert(txn(s2.txns, v.txn) == w);
            assert(rd.writer == v.txn && rd.epoch == w.epoch && rd.vc == w.vc && rd.value == write_value(w, k));
            assert(readable_top(s, c, r, k));
            assert(w.epoch <= r.epoch);
            assert(w.epoch < r.epoch ==> fvw_ready(s2.final_wm, c, w.epoch) && below_fvw(s2.final_wm, c, w.vc, w.epoch));
            assert(inv_read_writer(s2, c, id, k));
            assert(rd.writer != -1 ==> inv_read_writer(s2, c, id, k));
        }
        assert(txn(s2.txns, id).reads[k] == rd);
    }
    assert(inv_txns(s2, c)) by {
        assert forall|o: int| #[trigger] s2.txns.dom().contains(o) implies o >= 0 && inv_txn(s2, c, o) by {
            assert(inv_txn(s, c, o));
            if o == id {
                assert forall|k2: int| #[trigger] r2.reads.dom().contains(k2) implies read_set(r2.body).contains(k2) by {
                    if k2 != k { assert(r.reads.dom().contains(k2)); }
                }
                assert forall|x: int| is_comp(c, x) implies #[trigger] r2.vc[x] >= 0 by { assert(r.vc[x] >= 0); }
            }
        }
    }
    assert(inv_prepared(s2, c)) by {
        assert forall|j: int| 0 <= j < s2.prepared.len() implies
            has_txn(s2.txns, #[trigger] s2.prepared[j]) && txn(s2.txns, s2.prepared[j]).pidx == j
            && is_prepared_or_later(txn(s2.txns, s2.prepared[j])) by {
            assert(is_prepared_or_later(txn(s.txns, s.prepared[j])));
            assert(s.prepared[j] != id);
        }
        assert forall|j1: int, j2: int| 0 <= j1 < j2 < s2.prepared.len() implies
            txn(s2.txns, #[trigger] s2.prepared[j1]).prepared_at < txn(s2.txns, #[trigger] s2.prepared[j2]).prepared_at by {
            assert(is_prepared_or_later(txn(s.txns, s.prepared[j1])));
            assert(is_prepared_or_later(txn(s.txns, s.prepared[j2])));
        }
    }
    assert(inv_exclusive(s2, c)) by {
        assert forall|a: int, b: int| #[trigger] s2.txns.dom().contains(a) && #[trigger] s2.txns.dom().contains(b) && a != b
            && txn(s2.txns, a).coord == txn(s2.txns, b).coord && txn(s2.txns, a).thread == txn(s2.txns, b).thread
            implies !(in_flight(txn(s2.txns, a)) && in_flight(txn(s2.txns, b))) by {
            assert(txn(s2.txns, a).coord == txn(s.txns, a).coord && txn(s2.txns, a).thread == txn(s.txns, a).thread);
            assert(txn(s2.txns, b).coord == txn(s.txns, b).coord && txn(s2.txns, b).thread == txn(s.txns, b).thread);
            assert(in_flight(txn(s2.txns, a)) == in_flight(txn(s.txns, a)));
            assert(in_flight(txn(s2.txns, b)) == in_flight(txn(s.txns, b)));
        }
    }
    assert(inv_locks(s2, c)) by {
        assert forall|k2: int| #[trigger] s2.locks.dom().contains(k2) implies inv_lock(s2, c, k2) by {
            assert(inv_lock(s, c, k2));
            assert(s.locks[k2] != id);
            assert forall|j: int| 0 <= j < vers(s2.versions, k2).len() implies
                pidx_of(s2.txns, #[trigger] vers(s2.versions, k2)[j].txn) < txn(s2.txns, s2.locks[k2]).pidx by {
                assert(pidx_of(s.txns, vers(s.versions, k2)[j].txn) < txn(s.txns, s.locks[k2]).pidx);
                assert(inv_versions_of(s, c, k2));
                assert(inv_version(s, c, k2, vers(s.versions, k2)[j]));
                assert(vers(s.versions, k2)[j].txn != id);
            }
        }
        assert forall|o: int, k2: int| #[trigger] s2.txns.dom().contains(o) && txn(s2.txns, o).status is Prepared
            && #[trigger] write_set(txn(s2.txns, o).body).contains(k2)
            && !txn(s2.txns, o).installed.contains(owner(c, k2))
            && shard_epoch(s2.shards, owner(c, k2)) == txn(s2.txns, o).epoch
            implies s2.locks.dom().contains(k2) && s2.locks[k2] == o by {
            assert(o != id);
        }
    }
    assert(inv_versions(s2, c)) by {
        assert forall|k2: int| #[trigger] s2.versions.dom().contains(k2) implies inv_versions_of(s2, c, k2) by {
            assert(inv_versions_of(s, c, k2));
            assert forall|j: int| 0 <= j < s2.versions[k2].len() implies inv_version(s2, c, k2, #[trigger] s2.versions[k2][j]) by {
                assert(inv_version(s, c, k2, s.versions[k2][j]));
                assert(s.versions[k2][j].txn != id);
            }
            assert forall|a: int, b: int| 0 <= a < b < s2.versions[k2].len() implies
                txn(s2.txns, #[trigger] s2.versions[k2][a].txn).pidx < txn(s2.txns, #[trigger] s2.versions[k2][b].txn).pidx by {
                assert(inv_version(s, c, k2, s.versions[k2][a]));
                assert(inv_version(s, c, k2, s.versions[k2][b]));
            }
        }
    }
    assert(inv_reads(s2, c)) by {
        assert forall|o: int, k2: int| #[trigger] s2.txns.dom().contains(o) && #[trigger] txn(s2.txns, o).reads.dom().contains(k2)
            implies inv_read(s2, c, o, k2) by {
            if o == id && k2 == k {
            } else {
                assert(txn(s.txns, o).reads.dom().contains(k2));
                assert(txn(s2.txns, o).reads[k2] == txn(s.txns, o).reads[k2]);
                assert(inv_read(s, c, o, k2));
                let rd2 = txn(s.txns, o).reads[k2];
                if rd2.writer != -1 {
                    assert(inv_read_writer(s, c, o, k2));
                    assert(rd2.writer != id);
                    assert(inv_read_writer(s2, c, o, k2));
                }
                if is_prepared_or_later(txn(s2.txns, o)) {
                    assert(o != id);
                    assert forall|p: int| #[trigger] s2.txns.dom().contains(p) && p != o
                        && is_prepared_or_later(txn(s2.txns, p)) && txn(s2.txns, p).pidx < txn(s2.txns, o).pidx
                        && write_set(txn(s2.txns, p).body).contains(k2) implies inv_read_order(s2, c, o, k2, p) by {
                        assert(p != id);
                        assert(inv_read_order(s, c, o, k2, p));
                    }
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
                assert forall|i: int| is_shard(c, i) && #[trigger] logged_at(c, r2, i) implies false by {}
                assert forall|k2: int| #[trigger] write_set(r2.body).contains(k2) && r2.installed.contains(owner(c, k2)) implies false by {}
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
        }
    }
    assert(inv_rolled_back(s2, c)) by {
        assert forall|i: int, e: nat| #[trigger] s2.rolled_back.contains((i, e)) implies inv_rolled_back_of(s2, c, i, e) by {
            assert(inv_rolled_back_of(s, c, i, e));
        }
    }
}

pub proof fn lemma_replicate_inv(s: State, c: Constants, sid: Sid)
    requires inv(s, c), can_replicate(s, c, sid)
    ensures inv(apply(s, c, Action::Replicate { sid }), c)
{
    let s2 = apply(s, c, Action::Replicate { sid });
    let st = s.streams[sid];
    let st2 = s2.streams[sid];
    let x = st.pending[0];
    assert(st2.durable == st.durable.push(x));
    assert(all_entries(st2) =~= all_entries(st));
    assert forall|sid2: Sid| valid_sid(c, sid2) implies #[trigger] all_entries(s2.streams[sid2]) == all_entries(s.streams[sid2]) by {
        if sid2 != sid { assert(s2.streams[sid2] == s.streams[sid2]); }
    }
    assert(inv_stream(s, c, sid));
    assert(inv_stream_order(st.durable.push(x))) by {
        assert(all_entries(st).take(st.durable.len() as int + 1) =~= st.durable.push(x));
        lemma_order_prefix(all_entries(st), st.durable.len() as int + 1);
    }
    frame_txns_tick(s, s2, c);
    frame_reads(s, s2, c);
    assert(inv_prepared(s2, c));
    assert(inv_exclusive(s2, c));
    assert(inv_locks(s2, c)) by {
        assert forall|k: int| #[trigger] s2.locks.dom().contains(k) implies inv_lock(s2, c, k) by {
            assert(inv_lock(s, c, k));
        }
    }
    assert(inv_versions(s2, c)) by {
        assert forall|k: int| #[trigger] s2.versions.dom().contains(k) implies inv_versions_of(s2, c, k) by {
            assert(inv_versions_of(s, c, k));
        }
    }
    assert(inv_streams(s2, c)) by {
        assert forall|sid2: Sid| valid_sid(c, sid2) implies #[trigger] inv_stream(s2, c, sid2) by {
            assert(inv_stream(s, c, sid2));
            let es = all_entries(s.streams[sid2]);
            assert(all_entries(s2.streams[sid2]) == es);
            assert forall|j: int| 0 <= j < es.len() implies inv_entry(s2, c, sid2, #[trigger] es[j]) by {
                assert(inv_entry(s, c, sid2, es[j]));
            }
        }
    }
    assert(inv_all_logs(s2, c)) by {
        assert forall|o: int| #[trigger] s2.txns.dom().contains(o) implies inv_logs(s2, c, o) by {
            assert(inv_logs(s, c, o));
            let ro = txn(s.txns, o);
            assert forall|i: int| #[trigger] stream_at(s2.streams, ro, i) == stream_at(s.streams, ro, i) by {}
            assert forall|k: int| (ro.status is Prepared || certified_or_committed(ro))
                && #[trigger] write_set(ro.body).contains(k) && ro.installed.contains(owner(c, k))
                && !has_version(s2.versions, k, o) implies
                (doomed(s2.final_wm, c, ro) && s2.rolled_back.contains((owner(c, k), ro.epoch))) || lost(s2.streams, c, ro, o) by {
                if lost(s.streams, c, ro, o) {
                    let i = choose|i: int| is_shard(c, i) && #[trigger] logged_at(c, ro, i) && !has_log(stream_at(s.streams, ro, i), o);
                    assert(!has_log(stream_at(s2.streams, ro, i), o));
                }
            }
        }
    }
    assert(inv_committed(s2, c)) by {
        assert forall|o: int| #[trigger] s2.txns.dom().contains(o) && committed(txn(s2.txns, o))
            implies below_wm(s2.streams, c, txn(s2.txns, o).vc, txn(s2.txns, o).epoch) by {
            let vc = txn(s.txns, o).vc;
            let e = txn(s.txns, o).epoch;
            assert(below_wm(s.streams, c, vc, e));
            assert forall|sid2: Sid| valid_sid(c, sid2) implies
                wm_le(vc[group(c, sid2.shard)], stream_wm(#[trigger] s2.streams[sid2].durable, e)) by {
                assert(wm_le(vc[group(c, sid2.shard)], stream_wm(s.streams[sid2].durable, e)));
                if sid2 == sid {
                    lemma_wm_monotone_push(st.durable, x, e);
                    lemma_wm_le_int_trans(vc[group(c, sid2.shard)], stream_wm(st.durable, e), stream_wm(st.durable.push(x), e));
                }
            }
        }
    }
    assert(inv_final(s2, c)) by {
        assert forall|i: int, e: nat| #[trigger] s2.final_wm.dom().contains((i, e)) implies inv_final_of(s2, c, i, e) by {
            assert(inv_final_of(s, c, i, e));
            assert forall|sid2: Sid| valid_sid(c, sid2) && sid2.shard == i implies
                no_pending_epoch(#[trigger] s2.streams[sid2], e)
                && wm_le_wm(s2.final_wm[(i, e)], stream_wm(s2.streams[sid2].durable, e)) by {
                assert(no_pending_epoch(s.streams[sid2], e));
                if sid2 == sid {
                    assert(entry_epoch(x) != e);
                    lemma_wm_push_other(st.durable, x, e);
                    assert forall|j: int| 0 <= j < st2.pending.len() implies entry_epoch(#[trigger] st2.pending[j]) != e by {
                        assert(st2.pending[j] == st.pending[j + 1]);
                    }
                }
            }
            let w = choose|w: Sid| valid_sid(c, w) && w.shard == i && s.final_wm[(i, e)] == stream_wm(#[trigger] s.streams[w].durable, e);
            if w == sid {
                assert(entry_epoch(x) != e);
                lemma_wm_push_other(st.durable, x, e);
            }
            assert(s2.final_wm[(i, e)] == stream_wm(s2.streams[w].durable, e));
        }
    }
    assert(inv_rolled_back(s2, c)) by {
        assert forall|i: int, e: nat| #[trigger] s2.rolled_back.contains((i, e)) implies inv_rolled_back_of(s2, c, i, e) by {
            assert(inv_rolled_back_of(s, c, i, e));
        }
    }
}

} // verus!
