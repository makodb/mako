//! Private proof only. No production runtime code or native library is linked.

#[cfg(test)]
mod tests {
    use loom::sync::atomic::{AtomicU64, AtomicUsize, Ordering};
    use loom::sync::Arc;
    use loom::thread;

    struct State {
        next_slot: AtomicUsize,
        packed: AtomicU64,
        generations: [AtomicU64; 2],
        outcomes: [AtomicU64; 2],
    }

    fn check_bound(writer_count: usize, bound_before_cut: bool) {
        // No preemption or permutation bound. Operations are finite: an
        // observed active generation models the required wait instead of
        // introducing a scheduler-dependent spin loop.
        loom::model(move || {
            let shared = Arc::new(State {
                next_slot: AtomicUsize::new(0),
                packed: AtomicU64::new(0),
                generations: std::array::from_fn(|_| AtomicU64::new(0)),
                outcomes: std::array::from_fn(|_| AtomicU64::new(0)),
            });
            let mut writers = Vec::new();
            for _ in 0..writer_count {
                let state = Arc::clone(&shared);
                writers.push(thread::spawn(move || {
                    // The production allocator is a capped Relaxed RMW.
                    // Allocation order need not equal packed writer order.
                    let slot = state
                        .next_slot
                        .fetch_update(Ordering::Relaxed, Ordering::Relaxed, |next| {
                            (next < 2).then_some(next + 1)
                        })
                        .unwrap();
                    state.generations[slot].store(1, Ordering::Release);
                    let own_order = state.packed.fetch_add(1, Ordering::AcqRel);
                    // A different logical operation on the same packed word
                    // does not break the writer's release sequence.
                    state.packed.fetch_add(0, Ordering::Relaxed);
                    state.outcomes[slot].store(17 + slot as u64, Ordering::Relaxed);
                    state.generations[slot].store(2, Ordering::Release);
                    (slot, own_order)
                }));
            }
            let reader_state = Arc::clone(&shared);
            let reader = thread::spawn(move || {
                let (cut, bound) = if bound_before_cut {
                    let bound = reader_state.next_slot.load(Ordering::Acquire);
                    (reader_state.packed.fetch_add(0, Ordering::Acquire), bound)
                } else {
                    let cut = reader_state.packed.fetch_add(0, Ordering::Acquire);
                    (cut, reader_state.next_slot.load(Ordering::Acquire))
                };
                assert!(bound <= 2);
                let observations = (0..bound)
                    .map(|slot| {
                        (
                            reader_state.generations[slot].load(Ordering::Acquire),
                            reader_state.outcomes[slot].load(Ordering::Relaxed),
                        )
                    })
                    .collect::<Vec<_>>();
                (cut, bound, observations)
            });

            let writer_orders = writers
                .into_iter()
                .map(|writer| writer.join().unwrap())
                .collect::<Vec<_>>();
            let (cut, bound, observations) = reader.join().unwrap();
            // These assertions inspect only values sampled by the reader.
            // Joining cannot make an earlier missing registration visible.
            for (slot, own_order) in writer_orders {
                if own_order < cut {
                    assert!(slot < bound, "bounded scan omitted prefix slot");
                    let (generation, outcome) = observations[slot];
                    assert_ne!(generation, 0, "reader missed prefix enrollment");
                    if generation != 1 {
                        assert_eq!(outcome, 17 + slot as u64);
                    }
                }
            }
        });
    }

    #[test]
    fn bound_after_cut_covers_relaxed_registration_and_completed_outcome() {
        check_bound(1, false);
    }

    #[test]
    fn bound_after_cut_covers_out_of_order_slot_and_packed_publication() {
        // Isolate the new high-water claim from the already separately tested
        // generation/outcome protocol. Both writers can register in either
        // order and publish their packed modification in the opposite order.
        loom::model(|| {
            let state = Arc::new((AtomicUsize::new(0), AtomicU64::new(0)));
            let writers = (0..2)
                .map(|_| {
                    let state = Arc::clone(&state);
                    thread::spawn(move || {
                        let slot = state
                            .0
                            .fetch_update(Ordering::Relaxed, Ordering::Relaxed, |next| {
                                (next < 2).then_some(next + 1)
                            })
                            .unwrap();
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
                    assert!(slot < bound, "bounded scan omitted prefix slot");
                }
            }
        });
    }

    #[test]
    #[ignore = "negative control must fail with omitted prefix slot"]
    fn bound_before_cut_can_omit_prefix_writer() {
        check_bound(1, true);
    }
}
