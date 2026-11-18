//! A fused ACK deliberately suppresses facade destruction. Owned log buffers
//! must already belong to the worker before the successful return.

use super::*;

#[test]
fn fused_success_recycles_owned_log_before_caller_forgets_facade() {
    std::thread::spawn(|| {
        {
        let db = LocalDb::open().unwrap();
        db.bind_trusted_table("pooled-fused", 1).unwrap();
        let pool = TrustedOnePutHolderPool::new(4, 0, 0).unwrap();
        let acknowledged = AtomicU64::new(0);
        let unhealthy = AtomicBool::new(false);
        let producer_next = AtomicU64::new(0);
        let capacity_limit = AtomicU64::new(4);
        // SAFETY: all words and the pool remain live on this sole producer's
        // stack, and every consumed holder is released before ring reuse.
        let control = unsafe {
            TrustedSpscOnePutHolderControl::new(
                &pool,
                acknowledged.as_ptr(),
                unhealthy.as_ptr().cast(),
                NonZeroU64::new(4).unwrap(),
                NonZeroU32::new(1024).unwrap(),
            )
        };
        let mut retained_key = None;
        for sequence in 1..=32u64 {
            let (table, mut tx) = db.trusted_bound_transaction().unwrap();
            let key = sequence.to_be_bytes();
            let value = vec![sequence as u8; 128];
            tx.put(&table, &key, &value).unwrap();
            if let Some(pointer) = retained_key {
                assert_eq!(tx.touched[0].key.as_ptr(), pointer);
            }
            retained_key = Some(tx.touched[0].key.as_ptr());
            let mut cold = MaybeUninit::uninit();
            assert!(unsafe {
                tx.try_commit_trusted_fused_single_producer_one_put_holder_fast_forget_on_publish(
                    &control,
                    producer_next.as_ptr(),
                    capacity_limit.as_ptr(),
                    &mut cold,
                )
            });
            assert!(!tx.active && tx.inner.is_none());
            assert!(
                !tx.touched.has_storage(),
                "fused success left owned storage in a forgotten facade"
            );
                if sequence % 2 == 0 {
                    // A consumed facade owns neither the core slot nor the
                    // returned log. Its eventual Drop must leave a newer
                    // transaction on that worker untouched.
                    let (_, newer) = db.trusted_bound_transaction().unwrap();
                    drop(tx);
                    assert!(newer.active);
                    newer.abort().unwrap();
                } else {
                    std::mem::forget(tx);
                }
            assert_eq!(acknowledged.load(Ordering::Acquire), sequence);
            let view = unsafe { pool.view(NonZeroU64::new(sequence).unwrap()) }.unwrap();
            assert_eq!(view.key(), key);
            assert_eq!(view.value(), value);
            unsafe { view.release_after_backend() }.unwrap();
            capacity_limit.store(sequence + 4, Ordering::Relaxed);
        }
        }
        // SAFETY: the fixture block retired the DB, table, control, pool,
        // and every transaction. This thread has no other fixture users.
        unsafe { worker::drop_healthy_test_attachments() };
    })
    .join()
    .unwrap();
}
