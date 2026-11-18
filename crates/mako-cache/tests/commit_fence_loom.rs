//! Exhaustive memory-ordering models for the cross-language commit fence.

use loom::sync::atomic::{AtomicU64, Ordering};
use loom::sync::mpsc;
use loom::sync::Arc;
use loom::thread;

#[test]
fn packed_order_cut_orders_prefix_generation_and_clear() {
    loom::model(|| {
        let packed_order = Arc::new(AtomicU64::new(0));
        let generation = Arc::new(AtomicU64::new(0));
        let payload = Arc::new(AtomicU64::new(0));

        let writer_order = Arc::clone(&packed_order);
        let writer_generation = Arc::clone(&generation);
        let writer_payload = Arc::clone(&payload);
        let writer = thread::spawn(move || {
            writer_generation.store(1, Ordering::Release);
            let own_order = writer_order.fetch_add(1, Ordering::AcqRel);
            // Model an intervening timestamp-only CAS. This RMW remains in the
            // packed writer's release sequence even though it changes another
            // logical field in the production word.
            writer_order.fetch_add(1, Ordering::Relaxed);
            writer_payload.store(1, Ordering::Relaxed);
            writer_generation.store(2, Ordering::Release);
            own_order
        });

        let reader_order = Arc::clone(&packed_order);
        let reader_generation = Arc::clone(&generation);
        let reader_payload = Arc::clone(&payload);
        let reader = thread::spawn(move || {
            let cut = reader_order.fetch_add(0, Ordering::Acquire);
            let observed_generation = reader_generation.load(Ordering::Acquire);
            let observed_payload = reader_payload.load(Ordering::Relaxed);
            (cut, observed_generation, observed_payload)
        });

        let own_order = writer.join().unwrap();
        let (cut, observed_generation, observed_payload) = reader.join().unwrap();
        if own_order < cut && observed_generation != 1 {
            // A prefix writer not observed at its exact active generation
            // must have published its outcome before the Release clear.
            assert_eq!(
                observed_payload, 1,
                "the reader missed a completed prefix writer"
            );
        }
    });
}

#[cfg(test)]
mod registered_scan_bound {
    use loom::sync::atomic::{AtomicU64, AtomicUsize, Ordering};
    use loom::sync::Arc;
    use loom::thread;

    #[test]
    fn post_cut_bound_includes_relaxed_registration_and_completed_outcome() {
        loom::model(|| {
            // Registration, packed order, slot generation, and outcome remain
            // distinct atomics. One main-owned Arc avoids irrelevant lifetime
            // interleavings; no scheduler exploration bound is imposed.
            let state = Arc::new((
                AtomicUsize::new(0),
                AtomicU64::new(0),
                AtomicU64::new(0),
                AtomicU64::new(0),
            ));
            let writer_state = Arc::clone(&state);
            let writer = thread::spawn(move || {
                let slot = writer_state
                    .0
                    .fetch_update(Ordering::Relaxed, Ordering::Relaxed, |next| {
                        (next < 1).then_some(next + 1)
                    })
                    .unwrap();
                assert_eq!(slot, 0);
                writer_state.2.store(1, Ordering::Release);
                writer_state.1.fetch_add(1, Ordering::AcqRel);
                // A timestamp-only RMW remains in the release sequence.
                writer_state.1.fetch_add(0, Ordering::Relaxed);
                writer_state.3.store(17, Ordering::Relaxed);
                writer_state.2.store(2, Ordering::Release);
            });
            let reader_state = Arc::clone(&state);
            let reader = thread::spawn(move || {
                let cut = reader_state.1.fetch_add(0, Ordering::Acquire);
                let bound = reader_state.0.load(Ordering::Acquire);
                if cut == 0 {
                    return;
                }
                assert_eq!(bound, 1, "bounded scan omitted prefix slot");
                let generation = reader_state.2.load(Ordering::Acquire);
                assert_ne!(generation, 0, "reader missed prefix enrollment");
                // Exact active-generation draining has a separate model.
                if generation != 1 {
                    assert_eq!(reader_state.3.load(Ordering::Relaxed), 17);
                }
            });
            writer.join().unwrap();
            reader.join().unwrap();
        });
    }

    #[test]
    fn post_cut_bound_covers_opposite_registration_and_publication_orders() {
        loom::model(|| {
            let state = Arc::new((AtomicUsize::new(0), AtomicU64::new(0)));
            let writers = (0..2)
                .map(|_| {
                    let state = Arc::clone(&state);
                    thread::spawn(move || {
                        // Exactly two allocations fit the two modeled slots.
                        // There is no exhaustion or wrap in this model, so
                        // fetch_add represents the successful Relaxed allocator
                        // RMW without modeling irrelevant CAS retry schedules.
                        // The other model uses production's capped fetch_update.
                        let slot = state.0.fetch_add(1, Ordering::Relaxed);
                        assert!(slot < 2);
                        let own_order = state.1.fetch_add(1, Ordering::AcqRel);
                        (slot, own_order)
                    })
                })
                .collect::<Vec<_>>();
            let reader_state = Arc::clone(&state);
            let reader = thread::spawn(move || {
                let cut = reader_state.1.fetch_add(0, Ordering::Acquire);
                let bound = reader_state.0.load(Ordering::Acquire);
                (cut, bound)
            });
            let registered = writers
                .into_iter()
                .map(|writer| writer.join().unwrap())
                .collect::<Vec<_>>();
            let (cut, bound) = reader.join().unwrap();
            assert!(bound <= 2);
            for (slot, own_order) in registered {
                if own_order < cut {
                    // Compare only values sampled by the reader. Join cannot
                    // repair a stale bound or add an omitted prefix slot.
                    assert!(slot < bound, "bounded scan omitted prefix slot");
                }
            }
        });
    }
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

#[test]
fn concurrent_reader_cuts_preserve_each_prefix_publication() {
    loom::model(|| {
        // Keep one shared owner alive through all joins. Modeling separate Arc
        // counters only multiplies irrelevant ownership interleavings; the
        // three protocol atomics remain separate.
        let state = Arc::new((AtomicU64::new(0), AtomicU64::new(0), AtomicU64::new(0)));
        let writer_state = Arc::clone(&state);
        let writer = thread::spawn(move || {
            writer_state.1.store(1, Ordering::Release);
            writer_state.0.fetch_add(1, Ordering::AcqRel);
            writer_state.2.store(17, Ordering::Relaxed);
            writer_state.1.store(2, Ordering::Release);
        });
        let mut readers = Vec::new();
        for _ in 0..2 {
            let reader_state = Arc::clone(&state);
            readers.push(thread::spawn(move || {
                let cut = reader_state.0.fetch_add(0, Ordering::Acquire);
                // The only order increment belongs to the writer. A zero cut
                // does not include it in this reader's prefix.
                if cut == 0 {
                    return None;
                }
                let observed = reader_state.1.load(Ordering::Acquire);
                // Exact-generation draining is covered by the separate wait
                // model. Here check the completed-prefix case for each reader.
                if observed == 1 {
                    return None;
                }
                Some(reader_state.2.load(Ordering::Relaxed))
            }));
        }
        writer.join().unwrap();
        for reader in readers {
            if let Some(published) = reader.join().unwrap() {
                assert_eq!(published, 17, "a reader missed its completed prefix writer");
            }
        }
    });
}
