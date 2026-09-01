//! Exhaustive ownership models for packed per-worker occupancy credits.

use loom::sync::atomic::{AtomicUsize, Ordering};
use loom::sync::Arc;
use loom::thread;

#[test]
fn owner_take_and_pressure_swap_partition_one_idle_right() {
    loom::model(|| {
        let occupied = Arc::new(AtomicUsize::new(1));
        let credit = Arc::new(AtomicUsize::new(1));

        let owner_credit = Arc::clone(&credit);
        let owner = thread::spawn(move || {
            let mut observed = owner_credit.load(Ordering::Relaxed);
            loop {
                if observed == 0 {
                    return 0;
                }
                match owner_credit.compare_exchange_weak(
                    observed,
                    observed - 1,
                    Ordering::Relaxed,
                    Ordering::Relaxed,
                ) {
                    Ok(_) => return 1,
                    Err(current) => observed = current,
                }
            }
        });

        let reclaimer_credit = Arc::clone(&credit);
        let reclaimer_occupied = Arc::clone(&occupied);
        let reclaimer = thread::spawn(move || {
            let reclaimed = reclaimer_credit.swap(0, Ordering::Acquire);
            if reclaimed != 0 {
                let prior = reclaimer_occupied.fetch_sub(reclaimed, Ordering::Release);
                assert!(prior >= reclaimed);
            }
        });

        let active = owner.join().unwrap();
        reclaimer.join().unwrap();
        assert_eq!(
            occupied.load(Ordering::SeqCst),
            active + credit.load(Ordering::SeqCst),
            "the active permit and idle slot must partition aggregate occupancy"
        );
    });
}

#[test]
fn refill_publication_orders_batch_claim_before_reclamation() {
    loom::model(|| {
        let occupied = Arc::new(AtomicUsize::new(0));
        let credit = Arc::new(AtomicUsize::new(0));

        let owner_occupied = Arc::clone(&occupied);
        let owner_credit = Arc::clone(&credit);
        let owner = thread::spawn(move || {
            owner_occupied.fetch_add(2, Ordering::Relaxed);
            // One right becomes the active permit; publish the other as idle.
            owner_credit.store(1, Ordering::Release);
        });

        let reclaimer_occupied = Arc::clone(&occupied);
        let reclaimer_credit = Arc::clone(&credit);
        let reclaimer = thread::spawn(move || {
            let reclaimed = reclaimer_credit.swap(0, Ordering::Acquire);
            if reclaimed != 0 {
                let prior = reclaimer_occupied.fetch_sub(reclaimed, Ordering::Release);
                assert!(
                    prior >= reclaimed,
                    "observing a published credit must also observe its aggregate claim"
                );
                return (reclaimed, prior);
            }
            (0, 0)
        });

        owner.join().unwrap();
        let (reclaimed, prior) = reclaimer.join().unwrap();
        if reclaimed != 0 {
            assert_eq!(prior, 2, "reclamation follows the whole batch claim");
        }
    });
}

#[test]
fn relaxed_owner_take_keeps_refill_release_sequence_for_reclaimer() {
    loom::model(|| {
        let occupied = Arc::new(AtomicUsize::new(0));
        let credit = Arc::new(AtomicUsize::new(0));

        let owner_occupied = Arc::clone(&occupied);
        let owner_credit = Arc::clone(&credit);
        let owner = thread::spawn(move || {
            owner_occupied.fetch_add(3, Ordering::Relaxed);
            owner_credit.store(2, Ordering::Release);

            let mut observed = owner_credit.load(Ordering::Relaxed);
            loop {
                if observed == 0 {
                    return 1;
                }
                match owner_credit.compare_exchange(
                    observed,
                    observed - 1,
                    Ordering::Relaxed,
                    Ordering::Relaxed,
                ) {
                    Ok(_) => return 2,
                    Err(current) => observed = current,
                }
            }
        });

        let reclaimer_occupied = Arc::clone(&occupied);
        let reclaimer_credit = Arc::clone(&credit);
        let reclaimer = thread::spawn(move || {
            let reclaimed = reclaimer_credit.swap(0, Ordering::Acquire);
            if reclaimed != 0 {
                let prior = reclaimer_occupied.fetch_sub(reclaimed, Ordering::Release);
                assert!(
                    prior >= reclaimed,
                    "an Acquire swap reading a relaxed CAS remainder must import the batch claim"
                );
                return (reclaimed, prior);
            }
            (0, 0)
        });

        let active = owner.join().unwrap();
        let (reclaimed, prior) = reclaimer.join().unwrap();
        if reclaimed != 0 {
            assert_eq!(prior, 3, "reclamation follows the whole batch claim");
            assert_eq!(active + reclaimed, 3);
        }
    });
}
