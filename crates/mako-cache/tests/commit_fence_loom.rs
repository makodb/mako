//! Exhaustive memory-ordering models for the cross-language commit fence.

use loom::sync::atomic::{AtomicU64, Ordering};
use loom::sync::mpsc;
use loom::sync::Arc;
use loom::thread;

#[test]
fn validation_ticket_cut_orders_prefix_generation_and_clear() {
    loom::model(|| {
        let ticket = Arc::new(AtomicU64::new(0));
        let generation = Arc::new(AtomicU64::new(0));
        let payload = Arc::new(AtomicU64::new(0));

        let writer_ticket = Arc::clone(&ticket);
        let writer_generation = Arc::clone(&generation);
        let writer_payload = Arc::clone(&payload);
        let writer = thread::spawn(move || {
            writer_generation.store(1, Ordering::Release);
            let own_ticket = writer_ticket.fetch_add(1, Ordering::Release);
            // Model another writer's intervening ticket allocation. Its
            // Relaxed RMW remains in the first writer's release sequence.
            writer_ticket.fetch_add(1, Ordering::Relaxed);
            writer_payload.store(1, Ordering::Relaxed);
            writer_generation.store(2, Ordering::Release);
            own_ticket
        });

        let reader_ticket = Arc::clone(&ticket);
        let reader_generation = Arc::clone(&generation);
        let reader_payload = Arc::clone(&payload);
        let reader = thread::spawn(move || {
            let cut = reader_ticket.fetch_add(0, Ordering::Acquire);
            let observed_generation = reader_generation.load(Ordering::Acquire);
            let observed_payload = reader_payload.load(Ordering::Relaxed);
            (cut, observed_generation, observed_payload)
        });

        let own_ticket = writer.join().unwrap();
        let (cut, observed_generation, observed_payload) = reader.join().unwrap();
        if own_ticket < cut && observed_generation != 1 {
            // A prefix writer not observed at its exact active generation
            // must have published its outcome before the Release clear.
            assert_eq!(
                observed_payload, 1,
                "the reader missed a completed prefix writer"
            );
        }
    });
}

#[test]
fn exact_generation_wait_does_not_bridge_worker_reuse() {
    loom::model(|| {
        let generation = Arc::new(AtomicU64::new(1));
        let (observed_tx, observed_rx) = mpsc::channel();
        let (reused_tx, reused_rx) = mpsc::channel();

        let reader_generation = Arc::clone(&generation);
        let reader = thread::spawn(move || {
            let observed = reader_generation.load(Ordering::Acquire);
            assert_eq!(observed, 1);
            observed_tx.send(()).unwrap();
            reused_rx.recv().unwrap();

            // Production waits only while the slot equals this exact odd
            // generation. A later odd generation must not extend the wait.
            assert_ne!(reader_generation.load(Ordering::Acquire), observed);
        });

        let owner_generation = Arc::clone(&generation);
        let owner = thread::spawn(move || {
            observed_rx.recv().unwrap();
            owner_generation.store(2, Ordering::Release);
            owner_generation.store(3, Ordering::Release);
            reused_tx.send(()).unwrap();
        });

        reader.join().unwrap();
        owner.join().unwrap();
    });
}
