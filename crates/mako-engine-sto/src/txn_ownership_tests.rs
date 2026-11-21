//! Native-free coverage of the adapter's unsafe publication and holder reuse.
//! Miri can execute this module without calling Masstree.

use super::*;
use std::cell::UnsafeCell;

#[test]
fn terminal_version_capacity_is_not_a_transaction_size_error() {
    let report = map_commit(
        Ok(CommitOutcome::Aborted(AbortReason::Capacity(
            CapacityError::VersionExhausted,
        ))),
        None,
    );
    assert_eq!(
        report.disposition,
        CommitDisposition::Aborted(Error::VersionExhausted)
    );
    assert_eq!(
        report.cleanup,
        Ok(()),
        "capacity does not imply uncertain cleanup"
    );
    assert_eq!(
        map_abort(AbortReason::Capacity(CapacityError::ItemLimit), None),
        Error::TransactionTooLarge,
    );
    let (error, effect) = map_access(AccessError::Capacity(CapacityError::VersionExhausted));
    assert_eq!(error, Error::VersionExhausted);
    assert!(effect == Effect::Quarantined);
    assert_eq!(
        map_begin(BeginError::Capacity(CapacityError::VersionExhausted)),
        Error::VersionExhausted,
    );

    let unchecked = TrustedUncheckedOnePutRecordOutcome {
        report,
        record_bound: false,
        record_written: false,
    };
    let preselected = TrustedPreselectedUncheckedOnePutRecordOutcome {
        report,
        record_state: 0,
    };
    let ordered = TrustedNativeOrderedOnePutRecordOutcome {
        inner: unchecked,
        ordered_sequence: 0,
        timestamp_stamp: 0,
        target_bound: false,
        native_holder_ready: 0,
    };
    assert!(!unchecked.is_committed());
    assert!(preselected.accepted_timestamp().is_none());
    assert!(ordered.accepted_order().is_none());
    for decoded in [
        unchecked.into_report(),
        preselected.into_report(),
        ordered.into_report(),
    ] {
        assert!(decoded.completion_contract_valid);
        assert_eq!(decoded.commit, report);
        assert!(!decoded.record_bound && !decoded.record_written);
    }
    let holder = TrustedPreselectedUncheckedOnePutHolderOutcome {
        report,
        holder_state: 0,
    };
    assert!(holder.committed_timestamp().is_none());
    assert!(holder.accepted_timestamp().is_none());
    let decoded = holder.into_report();
    assert!(decoded.completion_contract_valid);
    assert_eq!(decoded.commit, report);
    assert!(!decoded.holder_bound && !decoded.holder_sealed);

    // Exhaustion precedes order and payload publication, so these witnesses
    // cannot accompany a clean capacity abort.
    assert!(
        !TrustedUncheckedOnePutRecordOutcome {
            report,
            record_bound: true,
            record_written: true,
        }
        .into_report()
        .completion_contract_valid
    );
    assert!(
        !TrustedNativeOrderedOnePutRecordOutcome {
            target_bound: true,
            ..ordered
        }
        .into_report()
        .completion_contract_valid
    );
    assert!(
        !TrustedPreselectedUncheckedOnePutRecordOutcome {
            report,
            record_state: 3,
        }
        .into_report()
        .completion_contract_valid
    );
    assert!(
        !TrustedPreselectedUncheckedOnePutHolderOutcome {
            report,
            holder_state: 3,
        }
        .into_report()
        .completion_contract_valid
    );
}

#[repr(align(64))]
struct Publication(UnsafeCell<[u64; 8]>);

// SAFETY: only the turn word is atomic. The sole producer writes the other
// words after acquiring FREE; the sole consumer reads them after acquiring
// READY and releases FREE only after its holder view has been retired.
unsafe impl Sync for Publication {}

impl Publication {
    fn pointer(&self) -> *mut u8 {
        self.0.get().cast()
    }

    fn turn(&self) -> &AtomicU64 {
        // SAFETY: the first u64 is aligned, live, and only accessed atomically.
        unsafe { AtomicU64::from_ptr(self.0.get().cast()) }
    }
}

#[test]
fn holder_publication_transfers_empty_and_overflow_values_across_ring_reuse() {
    let pool = TrustedOnePutHolderPool::new(4, 48, 257).unwrap();
    let publication = Publication(UnsafeCell::new([0; 8]));
    let keys = [Vec::new(), vec![b'k'; 48], b"end".to_vec()];
    let values = [Vec::new(), vec![0xab; 257], b"last".to_vec()];

    std::thread::scope(|scope| {
        scope.spawn(|| {
            for (ordinal, (key, expected)) in keys.iter().zip(&values).enumerate() {
                // Sequences 1, 5, and 9 reuse the same holder and cell, with
                // a distinct turn token for each generation.
                let sequence = 1 + 4 * ordinal as u64;
                while publication.turn().load(Ordering::Acquire)
                    != turn_token(sequence, 2, TURN_FREE)
                {
                    std::thread::yield_now();
                }
                // SAFETY: acquiring FREE grants this producer exclusive
                // access to this generation until it publishes READY.
                let holder = unsafe { &mut *pool.holder_ptr(sequence) };
                assert!(holder.is_free());
                holder.stage_key(key).unwrap();
                let mut value = expected.clone();
                holder.seal(&mut value, sequence, 7, sequence);
                publication
                    .turn()
                    .store(turn_token(sequence, 2, TURN_BOUND), Ordering::Release);
                // SAFETY: this producer owns the exact BOUND generation and
                // its holder is sealed before the release publication.
                unsafe {
                    publish_holder_ready(
                        publication.pointer(),
                        2,
                        sequence,
                        sequence,
                        UNCHECKED_ONE_PUT_OVERHEAD_BYTES + key.len() + expected.len(),
                    );
                }
            }
        });

        for (ordinal, (key, value)) in keys.iter().zip(&values).enumerate() {
            let sequence = 1 + 4 * ordinal as u64;
            while publication.turn().load(Ordering::Acquire) != turn_token(sequence, 2, TURN_READY)
            {
                std::thread::yield_now();
            }
            // SAFETY: READY grants this sole consumer the immutable metadata
            // and holder. FREE is not published until all these reads finish.
            unsafe {
                let pointer = publication.pointer();
                assert_eq!(
                    pointer
                        .add(PUBLICATION_TIMESTAMP_OFFSET)
                        .cast::<u64>()
                        .read_unaligned(),
                    sequence
                );
                assert_eq!(
                    pointer
                        .add(PUBLICATION_RECORD_BYTES_OFFSET)
                        .cast::<usize>()
                        .read_unaligned(),
                    NATIVE_HOLDER_RECORD_TAG
                        | (UNCHECKED_ONE_PUT_OVERHEAD_BYTES + key.len() + value.len())
                );
                let view = pool.view(NonZeroU64::new(sequence).unwrap()).unwrap();
                assert_eq!(view.sequence().get(), sequence);
                assert_eq!(view.table_id(), 7);
                assert_eq!(view.mako_timestamp(), mako_clock::expand(sequence).unwrap());
                assert_eq!(view.key(), key);
                assert_eq!(view.value(), value);
                view.release_after_backend().unwrap();
            }
            publication
                .turn()
                .store(turn_token(sequence + 4, 2, TURN_FREE), Ordering::Release);
        }
    });
}
