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
    assert_eq!(cache.durable_sequence(), acknowledged);
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
fn read_only_noops_do_not_log_and_recovery_rejects_invalid_backends() {
    let backend = Arc::new(MemBlobs::new());
    let cache = open(&backend);
    cache.put(b"existing", b"one").expect("seed key");
    cache.flush().expect("flush seed");

    let acknowledged = cache.highest_acknowledged_sequence();
    let durable = cache.durable_sequence();
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
    assert_eq!(cache.durable_sequence(), durable);
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
        Err(Error::DurableStateMismatch) => {}
        Err(error) => panic!("expected durable-state mismatch, got {error:?}"),
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
