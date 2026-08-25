#![cfg(have_mako)]

use std::sync::{Arc, Barrier};

use mako_cache::{Cache, CacheOptions, Error};
use mrx_core::fakes::MemBlobs;
use mrx_core::{BlobOp, Blobs};

type TestCache = Cache<Arc<MemBlobs>>;

fn open(backend: &Arc<MemBlobs>) -> TestCache {
    Cache::from_backend(Arc::clone(backend), CacheOptions::default()).expect("open cache")
}

#[test]
fn a_multi_key_commit_is_visible_then_flushed_as_one_atomic_backend_batch() {
    let backend = Arc::new(MemBlobs::new());
    let cache = open(&backend);

    cache.put(b"doomed", b"present").expect("seed delete");
    cache.flush().expect("flush seed");
    let batches_before = backend.batch_count();
    let ops_before = backend.op_count();

    let mut transaction = cache.transaction().expect("begin transaction");
    assert!(transaction.put(b"empty", b"").expect("put empty"));
    assert!(transaction
        .put(b"binary\0key", b"\0\xffbinary\0value")
        .expect("put binary"));
    assert!(transaction.remove(b"doomed").expect("delete"));

    assert_eq!(
        transaction.get(b"empty").expect("read own empty put"),
        Some(Vec::new())
    );
    assert_eq!(
        transaction
            .get(b"binary\0key")
            .expect("read own binary put")
            .as_deref(),
        Some(&b"\0\xffbinary\0value"[..])
    );
    assert_eq!(transaction.get(b"doomed").expect("read own delete"), None);
    transaction.commit().expect("commit transaction");

    // Silo is authoritative immediately after acknowledgement; Rocks need not
    // have won the background race yet.
    assert_eq!(cache.get(b"empty").expect("read empty"), Some(Vec::new()));
    assert_eq!(
        cache.get(b"binary\0key").expect("read binary").as_deref(),
        Some(&b"\0\xffbinary\0value"[..])
    );
    assert_eq!(cache.get(b"doomed").expect("read delete"), None);

    let acknowledged = cache.highest_acknowledged_sequence();
    assert_eq!(cache.flush().expect("flush transaction"), acknowledged);
    assert_eq!(cache.applied_sequence(), acknowledged);
    assert_eq!(backend.batch_count() - batches_before, 1);
    assert_eq!(
        backend.op_count() - ops_before,
        4,
        "one log put and all three data mutations must share the batch"
    );

    let batches_after_flush = backend.batch_count();
    cache.close().expect("clean close");
    assert_eq!(backend.batch_count(), batches_after_flush);

    let reopened = open(&backend);
    assert_eq!(
        reopened.get(b"empty").expect("recover empty"),
        Some(Vec::new())
    );
    assert_eq!(
        reopened
            .get(b"binary\0key")
            .expect("recover binary")
            .as_deref(),
        Some(&b"\0\xffbinary\0value"[..])
    );
    assert_eq!(reopened.get(b"doomed").expect("recover delete"), None);
    reopened.close().expect("close reopened cache");
}

#[test]
fn absent_put_then_delete_is_a_backend_noop() {
    let backend = Arc::new(MemBlobs::new());
    let cache = open(&backend);
    let acknowledged_before = cache.highest_acknowledged_sequence();
    let batches_before = backend.batch_count();
    let ops_before = backend.op_count();

    let mut transaction = cache.transaction().expect("begin transaction");
    assert!(transaction
        .put(b"same-key/absent-noop", b"temporary")
        .expect("put absent key"));
    assert_eq!(
        transaction
            .get(b"same-key/absent-noop")
            .expect("read staged put")
            .as_deref(),
        Some(&b"temporary"[..])
    );
    assert!(transaction
        .remove(b"same-key/absent-noop")
        .expect("remove staged put"));
    assert_eq!(
        transaction
            .get(b"same-key/absent-noop")
            .expect("read staged delete"),
        None
    );
    transaction.commit().expect("commit net no-op");

    assert_eq!(cache.highest_acknowledged_sequence(), acknowledged_before);
    assert_eq!(cache.flush().expect("flush net no-op"), acknowledged_before);
    assert_eq!(backend.batch_count(), batches_before);
    assert_eq!(backend.op_count(), ops_before);
    assert_eq!(
        cache
            .get(b"same-key/absent-noop")
            .expect("read committed no-op"),
        None
    );
    cache.close().expect("close cache");

    let reopened = open(&backend);
    assert_eq!(
        reopened
            .get(b"same-key/absent-noop")
            .expect("recover no-op"),
        None
    );
    reopened.close().expect("close reopened cache");
}

#[test]
fn existing_delete_then_put_is_one_canonical_put() {
    let backend = Arc::new(MemBlobs::new());
    let cache = open(&backend);
    cache
        .put(b"same-key/delete-put", b"original")
        .expect("seed existing key");
    cache.flush().expect("flush seed");
    let batches_before = backend.batch_count();
    let ops_before = backend.op_count();

    let mut transaction = cache.transaction().expect("begin transaction");
    assert!(transaction
        .remove(b"same-key/delete-put")
        .expect("remove existing key"));
    assert!(transaction
        .put(b"same-key/delete-put", b"replacement")
        .expect("recreate deleted key"));
    assert!(!transaction
        .insert(b"same-key/delete-put", b"must-not-win")
        .expect("insert over staged put"));
    assert_eq!(
        transaction
            .get(b"same-key/delete-put")
            .expect("read final staged value")
            .as_deref(),
        Some(&b"replacement"[..]),
        "a failed insert must not overwrite the canonical staged put"
    );
    transaction.commit().expect("commit replacement");
    cache.flush().expect("flush replacement");

    assert_eq!(backend.batch_count() - batches_before, 1);
    assert_eq!(
        backend.op_count() - ops_before,
        2,
        "one log put and one materialized put prove the key was canonicalized"
    );
    assert_eq!(
        cache
            .get(b"same-key/delete-put")
            .expect("read committed replacement")
            .as_deref(),
        Some(&b"replacement"[..])
    );
    cache.close().expect("close cache");

    let reopened = open(&backend);
    assert_eq!(
        reopened
            .get(b"same-key/delete-put")
            .expect("recover replacement")
            .as_deref(),
        Some(&b"replacement"[..])
    );
    reopened.close().expect("close reopened cache");
}

#[test]
fn every_same_key_mutation_pair_has_one_canonical_backend_result() {
    #[derive(Clone, Copy, Debug)]
    enum Mutation {
        Put,
        Insert,
        Remove,
    }

    let backend = Arc::new(MemBlobs::new());
    let cache = open(&backend);
    let mutations = [Mutation::Put, Mutation::Insert, Mutation::Remove];
    let mut recovered = Vec::new();
    let mut sequence = 0usize;

    for initially_present in [false, true] {
        for first in mutations {
            for second in mutations {
                let key = format!("same-key/matrix-{sequence}").into_bytes();
                sequence += 1;

                if initially_present {
                    cache.put(&key, b"seed").expect("seed matrix key");
                    cache.flush().expect("flush matrix seed");
                }
                let acknowledged_before = cache.highest_acknowledged_sequence();
                let batches_before = backend.batch_count();
                let ops_before = backend.op_count();

                let mut expected = initially_present.then(|| b"seed".to_vec());
                let mut had_effective_mutation = false;
                let mut transaction = cache.transaction().expect("begin matrix transaction");
                for (mutation, value) in [(first, &b"first"[..]), (second, &b"second"[..])] {
                    let was_present = expected.is_some();
                    let changed = match mutation {
                        Mutation::Put => {
                            let created = transaction.put(&key, value).expect("matrix put");
                            expected = Some(value.to_vec());
                            had_effective_mutation = true;
                            created
                        }
                        Mutation::Insert => {
                            let inserted = transaction.insert(&key, value).expect("matrix insert");
                            if inserted {
                                expected = Some(value.to_vec());
                                had_effective_mutation = true;
                            }
                            inserted
                        }
                        Mutation::Remove => {
                            let existed = transaction.remove(&key).expect("matrix remove");
                            if existed {
                                expected = None;
                                had_effective_mutation = true;
                            }
                            existed
                        }
                    };
                    assert_eq!(
                        changed,
                        match mutation {
                            Mutation::Put | Mutation::Insert => !was_present,
                            Mutation::Remove => was_present,
                        },
                        "initially_present={initially_present}, first={first:?}, second={second:?}, operation={mutation:?}"
                    );
                    assert_eq!(
                        transaction.get(&key).expect("read matrix staged value"),
                        expected,
                        "initially_present={initially_present}, first={first:?}, second={second:?}, operation={mutation:?}"
                    );
                }
                transaction.commit().expect("commit matrix transaction");
                cache.flush().expect("flush matrix transaction");

                let has_canonical_mutation = if initially_present {
                    had_effective_mutation
                } else {
                    expected.is_some()
                };
                assert_eq!(
                    cache.highest_acknowledged_sequence() - acknowledged_before,
                    u64::from(has_canonical_mutation),
                    "initially_present={initially_present}, first={first:?}, second={second:?}"
                );
                assert_eq!(
                    backend.batch_count() - batches_before,
                    u64::from(has_canonical_mutation)
                );
                assert_eq!(
                    backend.op_count() - ops_before,
                    u64::from(has_canonical_mutation) * 2,
                    "a canonical commit contains one log put and one final key mutation"
                );
                assert_eq!(cache.get(&key).expect("read matrix final value"), expected);
                recovered.push((key, expected));
            }
        }
    }
    assert_eq!(sequence, 18);
    cache.close().expect("close matrix cache");

    let reopened = open(&backend);
    for (key, expected) in recovered {
        assert_eq!(
            reopened.get(&key).expect("read recovered matrix value"),
            expected
        );
    }
    reopened.close().expect("close reopened matrix cache");
}

#[test]
fn same_key_composition_preserves_results_and_abort_publishes_nothing() {
    let backend = Arc::new(MemBlobs::new());
    let cache = open(&backend);
    cache
        .put(b"same-key/existing", b"original")
        .expect("seed existing key");
    cache
        .put(b"same-key/delete-final", b"present")
        .expect("seed key to delete");
    cache.flush().expect("flush seeds");
    let batches_before = backend.batch_count();
    let ops_before = backend.op_count();

    let mut transaction = cache.transaction().expect("begin composition");
    assert!(transaction
        .put(b"same-key/new", b"v1")
        .expect("create new key"));
    assert!(!transaction
        .put(b"same-key/new", b"v2")
        .expect("overwrite new key"));
    assert!(!transaction
        .insert(b"same-key/new", b"ignored")
        .expect("reject insert over new key"));

    assert!(!transaction
        .put(b"same-key/existing", b"first")
        .expect("overwrite existing key"));
    assert!(transaction
        .remove(b"same-key/existing")
        .expect("remove staged overwrite"));
    assert!(!transaction
        .remove(b"same-key/existing")
        .expect("repeat remove"));
    assert!(transaction
        .insert(b"same-key/existing", b"second")
        .expect("reinsert deleted key"));
    assert!(!transaction
        .put(b"same-key/existing", b"final")
        .expect("overwrite reinserted key"));

    assert!(!transaction
        .put(b"same-key/delete-final", b"temporary")
        .expect("overwrite key to delete"));
    assert!(transaction
        .remove(b"same-key/delete-final")
        .expect("stage final delete"));
    transaction.commit().expect("commit composition");
    cache.flush().expect("flush composition");

    assert_eq!(backend.batch_count() - batches_before, 1);
    assert_eq!(
        backend.op_count() - ops_before,
        4,
        "one log operation plus exactly one final mutation for each of three keys"
    );
    assert_eq!(
        cache.get(b"same-key/new").expect("read new").as_deref(),
        Some(&b"v2"[..])
    );
    assert_eq!(
        cache
            .get(b"same-key/existing")
            .expect("read existing")
            .as_deref(),
        Some(&b"final"[..])
    );
    assert_eq!(
        cache.get(b"same-key/delete-final").expect("read deleted"),
        None
    );

    let acknowledged_before_abort = cache.highest_acknowledged_sequence();
    let batches_before_abort = backend.batch_count();
    let ops_before_abort = backend.op_count();
    let mut aborted = cache.transaction().expect("begin aborted composition");
    assert!(!aborted
        .put(b"same-key/existing", b"aborted-1")
        .expect("first aborted put"));
    assert!(aborted
        .remove(b"same-key/existing")
        .expect("aborted remove"));
    assert!(aborted
        .insert(b"same-key/existing", b"aborted-2")
        .expect("aborted reinsert"));
    assert!(aborted
        .put(b"same-key/aborted-new", b"aborted-new")
        .expect("aborted new key"));
    aborted.abort().expect("abort composition");

    assert_eq!(
        cache.highest_acknowledged_sequence(),
        acknowledged_before_abort
    );
    assert_eq!(
        cache.flush().expect("flush after abort"),
        acknowledged_before_abort
    );
    assert_eq!(backend.batch_count(), batches_before_abort);
    assert_eq!(backend.op_count(), ops_before_abort);
    assert_eq!(
        cache
            .get(b"same-key/existing")
            .expect("read after abort")
            .as_deref(),
        Some(&b"final"[..])
    );
    assert_eq!(
        cache
            .get(b"same-key/aborted-new")
            .expect("read aborted new key"),
        None
    );
    cache.close().expect("close cache");

    let reopened = open(&backend);
    assert_eq!(
        reopened
            .get(b"same-key/new")
            .expect("recover new")
            .as_deref(),
        Some(&b"v2"[..])
    );
    assert_eq!(
        reopened
            .get(b"same-key/existing")
            .expect("recover existing")
            .as_deref(),
        Some(&b"final"[..])
    );
    assert_eq!(
        reopened
            .get(b"same-key/delete-final")
            .expect("recover delete"),
        None
    );
    assert_eq!(
        reopened
            .get(b"same-key/aborted-new")
            .expect("recover aborted new key"),
        None
    );
    reopened.close().expect("close reopened cache");
}

#[test]
fn read_only_noops_do_not_log_and_recovery_rejects_invalid_backends() {
    let backend = Arc::new(MemBlobs::new());
    let cache = open(&backend);
    cache.put(b"existing", b"one").expect("seed key");
    cache.flush().expect("flush seed");

    let acknowledged = cache.highest_acknowledged_sequence();
    let applied = cache.applied_sequence();
    let batches = backend.batch_count();
    let ops = backend.op_count();

    assert_eq!(
        cache.get(b"existing").expect("read-only get").as_deref(),
        Some(&b"one"[..])
    );

    let mut insert = cache.transaction().expect("begin no-op insert");
    assert!(!insert
        .insert(b"existing", b"ignored")
        .expect("insert existing"));
    insert.commit().expect("commit no-op insert");

    let mut remove = cache.transaction().expect("begin no-op remove");
    assert!(!remove.remove(b"missing").expect("remove missing"));
    remove.commit().expect("commit no-op remove");

    assert_eq!(cache.highest_acknowledged_sequence(), acknowledged);
    assert_eq!(cache.applied_sequence(), applied);
    assert_eq!(cache.queued_transactions(), 0);
    assert_eq!(cache.flush().expect("flush no-ops"), acknowledged);
    assert_eq!(backend.batch_count(), batches);
    assert_eq!(backend.op_count(), ops);
    cache.close().expect("close valid cache");

    // Locate the public backend row by its value, then corrupt only its
    // materialized data. The retained commit log must catch the disagreement.
    let data_key = backend
        .snapshot()
        .into_iter()
        .find_map(|(key, value)| (value == b"one").then_some(key))
        .expect("materialized data key");
    backend
        .write_batch(&[BlobOp::Put {
            key: &data_key,
            val: b"tampered",
        }])
        .expect("tamper backend");
    match Cache::from_backend(Arc::clone(&backend), CacheOptions::default()) {
        Err(Error::BackendStateMismatch) => {}
        Err(error) => panic!("expected backend-state mismatch, got {error:?}"),
        Ok(cache) => {
            cache.close().expect("close unexpectedly opened cache");
            panic!("corrupt materialized data was accepted");
        }
    }

    let foreign = Arc::new(MemBlobs::seeded([(
        b"application-owned-key".to_vec(),
        b"value".to_vec(),
    )]));
    match Cache::from_backend(foreign, CacheOptions::default()) {
        Err(Error::ForeignBackendKey) => {}
        Err(error) => panic!("expected foreign-key rejection, got {error:?}"),
        Ok(cache) => {
            cache.close().expect("close unexpectedly opened cache");
            panic!("foreign backend key was accepted");
        }
    }
}

#[test]
fn conflicting_writer_consumes_no_cache_sequence_or_log_slot() {
    let backend = Arc::new(MemBlobs::new());
    let cache = Arc::new(open(&backend));
    cache.put(b"contended", b"0").expect("seed key");
    cache.flush().expect("flush seed");
    let batches_before = backend.batch_count();
    let acknowledged_before = cache.highest_acknowledged_sequence();

    let ready = Arc::new(Barrier::new(2));
    let mut workers = Vec::new();
    for worker_id in 0..2 {
        let cache = Arc::clone(&cache);
        let ready = Arc::clone(&ready);
        workers.push(std::thread::spawn(move || {
            let mut transaction = cache.transaction().expect("begin worker transaction");
            assert_eq!(
                transaction
                    .get(b"contended")
                    .expect("read contended key")
                    .as_deref(),
                Some(&b"0"[..])
            );
            let value = if worker_id == 0 { b"1" } else { b"2" };
            let side_key = if worker_id == 0 { b"side-0" } else { b"side-1" };
            let side_value: &[u8] = if worker_id == 0 { b"left" } else { b"right" };
            transaction
                .put(b"contended", value)
                .expect("stage contended write");
            transaction
                .put(side_key, side_value)
                .expect("stage side write");
            ready.wait();
            (worker_id, transaction.commit())
        }));
    }

    let outcomes: Vec<_> = workers
        .into_iter()
        .map(|worker| worker.join().expect("worker did not panic"))
        .collect();
    assert_eq!(
        outcomes.iter().filter(|(_, result)| result.is_ok()).count(),
        1
    );
    assert_eq!(
        outcomes
            .iter()
            .filter(|(_, result)| result.as_ref().is_err_and(Error::is_conflict))
            .count(),
        1
    );
    let winner = outcomes
        .iter()
        .find_map(|(worker_id, result)| result.is_ok().then_some(*worker_id))
        .expect("one winner");

    assert_eq!(
        cache.highest_acknowledged_sequence(),
        acknowledged_before + 1,
        "the validation loser must not receive a CacheSeq"
    );
    cache.flush().expect("validation abort must not pin flush");
    assert_eq!(
        backend.batch_count() - batches_before,
        1,
        "only the winning transaction may reach the backend"
    );
    assert_eq!(
        cache.get(b"contended").expect("read winner").as_deref(),
        Some(if winner == 0 { &b"1"[..] } else { &b"2"[..] })
    );
    assert_eq!(
        cache.get(b"side-0").expect("read side zero").is_some(),
        winner == 0
    );
    assert_eq!(
        cache.get(b"side-1").expect("read side one").is_some(),
        winner == 1
    );

    let cache = match Arc::try_unwrap(cache) {
        Ok(cache) => cache,
        Err(_) => panic!("worker retained a cache handle"),
    };
    cache.close().expect("close after conflict");

    let reopened = open(&backend);
    assert_eq!(
        reopened
            .get(b"contended")
            .expect("recover winner")
            .as_deref(),
        Some(if winner == 0 { &b"1"[..] } else { &b"2"[..] })
    );
    assert_eq!(
        reopened
            .get(b"side-0")
            .expect("recover side zero")
            .is_some(),
        winner == 0
    );
    assert_eq!(
        reopened.get(b"side-1").expect("recover side one").is_some(),
        winner == 1
    );
    reopened.close().expect("close reopened cache");
}
