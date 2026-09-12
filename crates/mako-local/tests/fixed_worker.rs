#![cfg(have_mako)]

use std::collections::HashSet;
use std::sync::Arc;

use mako_local::worker::{FixedWorkerPool, FixedWorkerPoolOptions, RetryPolicy, TaskError};
use mako_local::{features, Error, LocalDb, TestCleanupBoundary};

const TABLE_NAME: &str = "rust-production-fixed-worker";
const TABLE_ID: u64 = 24_200;

fn options(worker_count: usize) -> FixedWorkerPoolOptions {
    FixedWorkerPoolOptions {
        worker_count,
        queue_capacity_per_worker: 32,
    }
}

#[test]
fn production_pool_keeps_transactions_on_fixed_workers_and_commits_progress() {
    let database = Arc::new(LocalDb::open().expect("open fixed-worker database"));
    let pool = FixedWorkerPool::start(Arc::clone(&database), options(2))
        .expect("start two production workers");

    let identity_tasks: Vec<_> = (0..6)
        .map(|_| pool.submit(|_| std::thread::current().id()))
        .collect();
    let worker_ids: HashSet<_> = identity_tasks
        .into_iter()
        .map(|task| task.wait().expect("identity task"))
        .collect();
    assert_eq!(worker_ids.len(), 2, "pool did not use exactly two workers");

    let writes: Vec<_> = (0u64..8)
        .map(|key| {
            pool.submit_retrying(RetryPolicy::new(128), move |database| {
                let table = database.open_table(TABLE_NAME, TABLE_ID)?;
                let mut transaction = database.transaction()?;
                transaction.put(
                    &table,
                    &key.to_be_bytes(),
                    &key.wrapping_mul(10).to_be_bytes(),
                )?;
                transaction.commit()
            })
        })
        .collect();
    for write in writes {
        write
            .wait()
            .expect("worker remained healthy")
            .expect("bounded conflict retry completed");
    }

    let table = database
        .open_table(TABLE_NAME, TABLE_ID)
        .expect("open verification table");
    let mut verify = database.transaction().expect("begin verification");
    for key in 0u64..8 {
        assert_eq!(
            verify
                .get(&table, &key.to_be_bytes())
                .expect("read committed worker value")
                .as_deref(),
            Some(key.wrapping_mul(10).to_be_bytes().as_slice())
        );
    }
    verify.commit().expect("commit read-only verification");

    let metrics = pool.metrics();
    assert_eq!(metrics.configured_workers, 2);
    assert_eq!(metrics.healthy_workers, 2);
    assert_eq!(metrics.poisoned_workers, 0);
    assert_eq!(metrics.accepted_tasks, 14);
    assert_eq!(metrics.completed_tasks, 14);
    let stopped = pool.shutdown().expect("join production workers");
    assert_eq!(stopped.stopped_workers, 2);
    let database = Arc::try_unwrap(database)
        .unwrap_or_else(|_| panic!("worker pool retained its database Arc"));
    database
        .close()
        .expect("healthy worker shutdown released native database markers");
}

#[test]
fn native_cleanup_quarantine_retires_the_worker_when_hooks_are_available() {
    if !features()
        .expect("query native features")
        .test_cleanup_failures()
    {
        return;
    }

    let database = Arc::new(LocalDb::open().expect("open quarantine database"));
    let pool = FixedWorkerPool::start(Arc::clone(&database), options(1))
        .expect("start sacrificial worker");
    let poisoned = pool.submit(|database| {
        mako_local::arm_test_cleanup_failure(TestCleanupBoundary::Abort)
            .expect("arm abort cleanup failure on worker");
        let transaction = database
            .transaction()
            .expect("begin sacrificial transaction");
        drop(transaction);
    });

    assert_eq!(
        poisoned.wait(),
        Err(TaskError::WorkerPoisoned { worker: 0 })
    );
    assert_eq!(
        pool.submit(|_| 1usize).wait(),
        Err(TaskError::NoHealthyWorkers)
    );
    let metrics = pool.metrics();
    assert_eq!(metrics.healthy_workers, 0);
    assert_eq!(metrics.poisoned_workers, 1);
    assert_eq!(metrics.rejected_tasks, 1);
    let stopped = pool.shutdown().expect("join quarantined worker");
    assert_eq!(stopped.poisoned_workers, 1);

    // Native cleanup is intentionally uncertain. The worker has exited, but
    // its process-lifetime active-database marker must keep close diagnostic.
    let database = Arc::try_unwrap(database)
        .unwrap_or_else(|_| panic!("worker pool retained its database Arc"));
    assert_eq!(database.close(), Err(Error::Busy));
}
