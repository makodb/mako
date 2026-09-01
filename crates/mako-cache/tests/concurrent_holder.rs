#![cfg(have_mako)]

//! End-to-end coverage for the concurrent packed one-Put holder path.

use std::sync::{Arc, Barrier, Mutex, MutexGuard};

use mako_cache::{Cache, CacheOptions, Error, RecordChecksum};
use mrx_core::fakes::MemBlobs;

const PRODUCERS: usize = 4;
const WAVES: usize = 3;
const REPLAYED_COMMITS: u64 = (PRODUCERS * WAVES) as u64;

type TestCache = Cache<Arc<MemBlobs>>;

static TEST_SERIAL: Mutex<()> = Mutex::new(());

fn serialize_test() -> MutexGuard<'static, ()> {
    TEST_SERIAL
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner())
}

fn holder_options() -> CacheOptions {
    let mut options = CacheOptions::default();
    options.record_checksum = RecordChecksum::None;
    options.writeback.capacity = PRODUCERS;
    options.writeback.max_batch_records = PRODUCERS;
    options
}

fn open(backend: &Arc<MemBlobs>) -> TestCache {
    Cache::from_backend(Arc::clone(backend), holder_options())
        .expect("open concurrent-holder cache")
}

fn worker_key(worker: usize) -> Vec<u8> {
    format!("concurrent-holder/worker-{worker}").into_bytes()
}

fn worker_value(wave: usize, worker: usize) -> Vec<u8> {
    format!("wave-{wave}/worker-{worker}").into_bytes()
}

#[test]
fn four_producers_replay_three_holder_generations() {
    let _serial = serialize_test();
    let backend = Arc::new(MemBlobs::new());
    let cache = Arc::new(open(&backend));
    let staged = Arc::new(Barrier::new(PRODUCERS + 1));
    let finished = Arc::new(Barrier::new(PRODUCERS + 1));

    let mut workers = Vec::with_capacity(PRODUCERS);
    for worker in 0..PRODUCERS {
        let cache = Arc::clone(&cache);
        let staged = Arc::clone(&staged);
        let finished = Arc::clone(&finished);
        workers.push(std::thread::spawn(move || {
            let key = worker_key(worker);
            for wave in 0..WAVES {
                let value = worker_value(wave, worker);
                let mut transaction = cache.transaction().expect("begin holder transaction");
                assert_eq!(
                    transaction
                        .put(&key, &value)
                        .expect("stage one-Put holder transaction"),
                    wave == 0,
                    "only the first wave inserts each worker key"
                );
                staged.wait();
                transaction.commit().expect("commit holder transaction");
                finished.wait();
            }
        }));
    }

    for wave in 0..WAVES {
        staged.wait();
        finished.wait();
        let expected = ((wave + 1) * PRODUCERS) as u64;
        assert_eq!(
            cache.highest_acknowledged_sequence(),
            expected,
            "each wave must allocate four dense cache sequences"
        );
        assert_eq!(
            cache.wait_applied().expect("replay one holder wave"),
            expected
        );
    }

    for worker in workers {
        worker.join().expect("holder producer did not panic");
    }
    assert_eq!(cache.queued_transactions(), 0);

    let cache = Arc::try_unwrap(cache).unwrap_or_else(|_| panic!("worker retained the cache"));
    assert_eq!(cache.close().expect("close holder cache"), REPLAYED_COMMITS);

    let reopened = open(&backend);
    for worker in 0..PRODUCERS {
        assert_eq!(
            reopened
                .get(&worker_key(worker))
                .expect("read replayed holder value"),
            Some(worker_value(WAVES - 1, worker)),
            "recovery lost the final reused holder generation for worker {worker}"
        );
    }
    assert_eq!(
        reopened.close().expect("close replayed holder cache"),
        REPLAYED_COMMITS
    );
}

#[test]
fn one_put_conflict_consumes_neither_sequence_nor_holder_generation() {
    let _serial = serialize_test();
    let backend = Arc::new(MemBlobs::new());
    let cache = Arc::new(open(&backend));
    cache
        .put(b"concurrent-holder/conflict", b"seed")
        .expect("seed conflict key");
    assert_eq!(cache.wait_applied().expect("replay conflict seed"), 1);

    let staged = Arc::new(Barrier::new(2));
    let mut workers = Vec::with_capacity(2);
    for worker in 0..2 {
        let cache = Arc::clone(&cache);
        let staged = Arc::clone(&staged);
        workers.push(std::thread::spawn(move || {
            let mut transaction = cache.transaction().expect("begin conflicting one-Put");
            assert_eq!(
                transaction
                    .get(b"concurrent-holder/conflict")
                    .expect("read conflict seed")
                    .as_deref(),
                Some(&b"seed"[..])
            );
            let value: &[u8] = if worker == 0 { b"left" } else { b"right" };
            assert!(!transaction
                .put(b"concurrent-holder/conflict", value)
                .expect("stage conflicting one-Put"));
            staged.wait();
            (worker, transaction.commit())
        }));
    }

    let outcomes: Vec<_> = workers
        .into_iter()
        .map(|worker| worker.join().expect("conflict worker did not panic"))
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
        .find_map(|(worker, result)| result.is_ok().then_some(*worker))
        .expect("one conflicting transaction wins");

    assert_eq!(
        cache.highest_acknowledged_sequence(),
        2,
        "the validation loser must not allocate a cache sequence"
    );
    assert_eq!(cache.wait_applied().expect("replay conflict winner"), 2);

    // Four more commits wrap the four-entry publication and holder rings.
    // Successful reuse proves the conflict loser retained no holder generation.
    for generation in 0..PRODUCERS {
        let key = format!("concurrent-holder/reuse-{generation}");
        let value = format!("value-{generation}");
        cache
            .put(key.as_bytes(), value.as_bytes())
            .expect("commit holder-reuse probe");
    }
    let expected = 2 + PRODUCERS as u64;
    assert_eq!(cache.highest_acknowledged_sequence(), expected);
    assert_eq!(
        cache.wait_applied().expect("replay reused holders"),
        expected
    );
    assert_eq!(cache.queued_transactions(), 0);
    assert_eq!(
        cache
            .get(b"concurrent-holder/conflict")
            .expect("read conflict winner")
            .as_deref(),
        Some(if winner == 0 {
            &b"left"[..]
        } else {
            &b"right"[..]
        })
    );

    let cache = Arc::try_unwrap(cache).unwrap_or_else(|_| panic!("worker retained the cache"));
    assert_eq!(cache.close().expect("close conflict cache"), expected);

    let reopened = open(&backend);
    assert_eq!(
        reopened
            .get(b"concurrent-holder/conflict")
            .expect("recover conflict winner")
            .as_deref(),
        Some(if winner == 0 {
            &b"left"[..]
        } else {
            &b"right"[..]
        })
    );
    for generation in 0..PRODUCERS {
        let key = format!("concurrent-holder/reuse-{generation}");
        let value = format!("value-{generation}");
        assert_eq!(
            reopened.get(key.as_bytes()).expect("recover reused holder"),
            Some(value.into_bytes())
        );
    }
    assert_eq!(
        reopened.close().expect("close recovered conflict cache"),
        expected
    );
}
