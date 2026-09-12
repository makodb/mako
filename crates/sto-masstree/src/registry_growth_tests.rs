use super::*;
use std::sync::Barrier;
use sto_core::{CommitOutcome, RuntimeConfig};

thread_local! {
    static FAIL_ALLOCATION_AFTER: std::cell::Cell<Option<usize>> = const {
        std::cell::Cell::new(None)
    };
}

/// Test-only allocation failure injection. No checkpoint exists in a
/// non-test build, and failures are isolated to the calling test thread.
pub(super) fn allocation_checkpoint() -> Result<(), CapacityError> {
    FAIL_ALLOCATION_AFTER.with(|remaining| match remaining.get() {
        Some(0) => {
            remaining.set(None);
            Err(CapacityError::BufferLimit)
        }
        Some(count) => {
            remaining.set(Some(count - 1));
            Ok(())
        }
        None => Ok(()),
    })
}

struct FailAllocationAfter;

impl FailAllocationAfter {
    fn new(checkpoints: usize) -> Self {
        FAIL_ALLOCATION_AFTER.with(|remaining| {
            assert_eq!(remaining.replace(Some(checkpoints)), None);
        });
        Self
    }
}

impl Drop for FailAllocationAfter {
    fn drop(&mut self) {
        FAIL_ALLOCATION_AFTER.with(|remaining| remaining.set(None));
    }
}

fn growing_config() -> TableConfig {
    TableConfig::new()
        .with_max_retained_records(u64::MAX)
        .with_max_retained_key_bytes(u64::MAX)
        .with_max_consumed_record_ids(u64::MAX)
}

fn registry_with_budget(
    config: TableConfig,
    budget: RegistryBudget,
) -> Result<Registry, RegistrationError> {
    Registry::new_with_budget(
        config,
        RuntimeId::new(1).unwrap(),
        LockNamespaceId::new(1).unwrap(),
        LockClass::new(RECORD_LOCK_CLASS_VALUE).unwrap(),
        budget,
    )
}

fn root_bytes() -> u64 {
    (REGISTRY_BUCKET_COUNT * std::mem::size_of::<OnceLock<RegistryBucket>>()) as u64
}

fn bucket_cell_bytes() -> u64 {
    std::mem::size_of::<OnceLock<RegistrySegment>>() as u64
}

fn segment_bytes(stable: bool) -> u64 {
    eager_registry_accounted_bytes(REGISTRY_SEGMENT_SLOTS, stable).unwrap() as u64
}

#[test]
fn prepaid_charge_splits_release_exact_bytes_in_every_drop_order() {
    for order in [
        [0, 1, 2],
        [0, 2, 1],
        [1, 0, 2],
        [1, 2, 0],
        [2, 0, 1],
        [2, 1, 0],
    ] {
        let budget = RegistryBudget::new(100);
        let accounting = Arc::new(RegistryAccounting {
            budget: budget.clone(),
            used: AtomicU64::new(0),
        });
        let mut prepaid = accounting.reserve(100).unwrap();
        let first = prepaid.split_off(20).unwrap();
        let second = prepaid.split_off(30).unwrap();
        assert_eq!(prepaid.bytes, 50);
        assert_eq!(budget.used_bytes(), 100);
        assert_eq!(accounting.used.load(Ordering::Acquire), 100);
        let mut charges = [Some(prepaid), Some(first), Some(second)];
        let bytes = [50, 20, 30];
        let mut expected = 100;
        for index in order {
            drop(charges[index].take());
            expected -= bytes[index];
            assert_eq!(budget.used_bytes(), expected);
            assert_eq!(accounting.used.load(Ordering::Acquire), expected);
        }
    }
}

#[test]
fn prepaid_charge_rejects_oversplits_and_releases_empty_balances_once() {
    let budget = RegistryBudget::new(100);
    let accounting = Arc::new(RegistryAccounting {
        budget: budget.clone(),
        used: AtomicU64::new(0),
    });
    let mut prepaid = accounting.reserve(100).unwrap();
    assert!(matches!(
        prepaid.split_off(101),
        Err(CapacityError::BufferLimit)
    ));
    assert_eq!(prepaid.bytes, 100);
    let empty = prepaid.split_off(0).unwrap();
    let mut child = prepaid.split_off(100).unwrap();
    assert_eq!(prepaid.bytes, 0);
    assert!(matches!(
        prepaid.split_off(1),
        Err(CapacityError::BufferLimit)
    ));
    let descendant = child.split_off(40).unwrap();
    drop(prepaid);
    drop(empty);
    assert_eq!(budget.used_bytes(), 100);
    assert_eq!(accounting.used.load(Ordering::Acquire), 100);
    drop(child);
    assert_eq!(budget.used_bytes(), 40);
    assert_eq!(accounting.used.load(Ordering::Acquire), 40);
    drop(descendant);
    assert_eq!(budget.used_bytes(), 0);
    assert_eq!(accounting.used.load(Ordering::Acquire), 0);
}

#[test]
fn prepaid_partial_construction_failure_releases_every_charge() {
    for stable in [false, true] {
        // Fail before arena storage, pointer storage, the first target, and
        // the last target after earlier targets already own their charges.
        for checkpoint in [0, 1, 2, 1 + RECORD_LOCK_SEGMENTS_PER_REGISTRY_SEGMENT] {
            let exact_bytes = root_bytes() + bucket_cell_bytes() + segment_bytes(stable);
            let budget = RegistryBudget::new(exact_bytes);
            let registry = registry_with_budget(
                growing_config().with_bounded_atomic_values(stable),
                budget.clone(),
            )
            .unwrap();
            let _failure = FailAllocationAfter::new(checkpoint);
            assert!(matches!(
                registry.reserve_candidate(b"rejected"),
                Err(AccessError::Capacity(CapacityError::BufferLimit))
            ));
            FAIL_ALLOCATION_AFTER.with(|remaining| assert_eq!(remaining.get(), None));
            assert_eq!(budget.used_bytes(), root_bytes());
            assert_eq!(registry.usage().allocated_registry_bytes(), root_bytes());
            assert_eq!(registry.usage().retained_records(), 0);
            assert_eq!(registry.usage().retained_key_bytes(), 0);
            assert_eq!(registry.usage().consumed_record_ids(), 1);
            let RegistryStorage::LazySegmented(storage) = &registry.storage else {
                unreachable!();
            };
            assert_eq!(storage.allocated_segments().count(), 0);
            assert!(storage.buckets.iter().all(|bucket| bucket.get().is_none()));
            assert_eq!(registry.reserve_candidate(b"retry").unwrap().id.get(), 2);
            assert_eq!(budget.used_bytes(), exact_bytes);
            assert_eq!(registry.usage().allocated_registry_bytes(), exact_bytes);
            drop(registry);
            assert_eq!(budget.used_bytes(), 0);
        }
    }
}

#[test]
fn empty_eager_registry_preserves_its_owner_only_budget() {
    for stable in [false, true] {
        let owner_bytes = registry_arena_accounted_bytes(0, stable).unwrap() as u64;
        let config = growing_config()
            .with_bounded_atomic_values(stable)
            .with_max_consumed_record_ids(0)
            .with_registry_layout(RegistryLayout::EagerContiguous { max_bytes: 0 });
        let too_small = RegistryBudget::new(owner_bytes - 1);
        assert!(matches!(
            registry_with_budget(config, too_small.clone()),
            Err(RegistrationError::Capacity(CapacityError::BufferLimit))
        ));
        assert_eq!(too_small.used_bytes(), 0);
        let budget = RegistryBudget::new(owner_bytes);
        let registry = registry_with_budget(config, budget.clone()).unwrap();
        assert_eq!(budget.used_bytes(), owner_bytes);
        assert_eq!(registry.usage().allocated_registry_bytes(), owner_bytes);
        let RegistryStorage::EagerContiguous(storage) = &registry.storage else {
            unreachable!();
        };
        assert_eq!(storage.arena.len(), 0);
        assert!(storage.lock_segments.is_empty());
        drop(registry);
        assert_eq!(budget.used_bytes(), 0);
    }
}

#[test]
fn registry_id_table_constructor_enforces_its_shared_budget() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let budget = RegistryBudget::new(root_bytes());
    let create = || {
        Table::with_directory_mode_and_budget(
            &runtime,
            Directory::Memory(MemoryDirectory::default()),
            growing_config(),
            RecordTokenMode::RegistryId,
            budget.clone(),
        )
    };
    let table = create().unwrap();
    assert_eq!(table.usage().allocated_registry_bytes(), root_bytes());
    assert_eq!(budget.used_bytes(), root_bytes());
    assert!(matches!(
        create(),
        Err(RegistrationError::Capacity(CapacityError::BufferLimit))
    ));
    assert_eq!(budget.used_bytes(), root_bytes());
    drop(table);
    assert_eq!(budget.used_bytes(), 0);
    let replacement = create().unwrap();
    assert_eq!(replacement.usage().allocated_registry_bytes(), root_bytes());
}

#[test]
fn maximum_id_quota_allocates_only_a_small_sparse_root() {
    let budget = RegistryBudget::new(root_bytes());
    let registry = registry_with_budget(growing_config(), budget.clone()).unwrap();
    assert_eq!(budget.used_bytes(), root_bytes());
    assert!(budget.used_bytes() < 16 * 1024);
    assert_eq!(registry.usage().allocated_registry_bytes(), root_bytes());
    assert_eq!(registry.usage().consumed_record_ids(), 0);
    let RegistryStorage::LazySegmented(storage) = &registry.storage else {
        panic!("the growing configuration must use sparse segments");
    };
    assert_eq!(storage.allocated_segments().count(), 0);
    assert!(storage.segment(usize::MAX).is_none());
    assert!(matches!(
        registry.ensure_segment(usize::MAX),
        Err(AccessError::Capacity(CapacityError::BufferLimit))
    ));
    drop(registry);
    assert_eq!(budget.used_bytes(), 0);
}

#[test]
fn growth_preserves_borrowed_records_and_direct_tokens_across_buckets() {
    for stable in [false, true] {
        let budget = RegistryBudget::new(u64::MAX);
        let registry = registry_with_budget(
            growing_config().with_bounded_atomic_values(stable),
            budget.clone(),
        )
        .unwrap();
        let (mut first, first_token) = registry
            .reserve_candidate_with_mode(b"first", RecordTokenMode::DirectRecordPointer)
            .unwrap();
        registry.mark_published(&mut first).unwrap();
        let first_access = registry.resolve_direct_access(first_token).unwrap();
        let first_address = first_access.element_address;
        let (first_record, first_target) = registry.resolve_with_segment(first.id).unwrap();
        let first_target = Arc::clone(first_target);

        // Segment boundaries 1, 3, and 7 also cross directory buckets.
        for index in 1..(8 * REGISTRY_SEGMENT_SLOTS + 1) {
            let (mut candidate, token) = registry
                .reserve_candidate_with_mode(
                    &index.to_le_bytes(),
                    RecordTokenMode::DirectRecordPointer,
                )
                .unwrap();
            registry.mark_published(&mut candidate).unwrap();
            if index % REGISTRY_SEGMENT_SLOTS == 0 {
                let access = registry.resolve_direct_access(first_token).unwrap();
                assert_eq!(access.element_address, first_address);
                assert!(std::ptr::eq(&access.entry.record, first_record));
                assert_eq!(access.stable.is_some(), stable);
                assert!(Arc::ptr_eq(
                    registry.resolve_with_segment(first.id).unwrap().1,
                    &first_target,
                ));
                assert_eq!(
                    registry
                        .resolve_direct_access(token)
                        .unwrap()
                        .element_address as u64,
                    token.get(),
                );
            }
        }
        let RegistryStorage::LazySegmented(storage) = &registry.storage else {
            unreachable!();
        };
        assert_eq!(storage.allocated_segments().count(), 9);
        assert_eq!(
            registry.usage().allocated_registry_bytes(),
            budget.used_bytes()
        );
    }
}

#[test]
fn concurrent_growth_fits_the_exact_budget_without_duplicate_arenas() {
    const THREADS: usize = 8;
    const SEGMENTS: usize = 4;
    let count = SEGMENTS * REGISTRY_SEGMENT_SLOTS;
    let exact_bytes = root_bytes() + SEGMENTS as u64 * (bucket_cell_bytes() + segment_bytes(false));
    let budget = RegistryBudget::new(exact_bytes);
    let registry = Arc::new(
        registry_with_budget(
            growing_config().with_max_consumed_record_ids(count as u64),
            budget.clone(),
        )
        .unwrap(),
    );
    let barrier = Barrier::new(THREADS);
    let candidates = std::thread::scope(|scope| {
        let handles = (0..THREADS)
            .map(|_| {
                let registry = &registry;
                let barrier = &barrier;
                scope.spawn(move || {
                    barrier.wait();
                    (0..count / THREADS)
                        .map(|_| registry.reserve_candidate(b"x").unwrap())
                        .collect::<Vec<_>>()
                })
            })
            .collect::<Vec<_>>();
        handles
            .into_iter()
            .flat_map(|handle| handle.join().unwrap())
            .collect::<Vec<_>>()
    });
    let mut ids = candidates
        .iter()
        .map(|candidate| candidate.id.get())
        .collect::<Vec<_>>();
    ids.sort_unstable();
    assert_eq!(ids, (1..=count as u64).collect::<Vec<_>>());
    assert_eq!(budget.used_bytes(), exact_bytes);
    assert_eq!(registry.usage().allocated_registry_bytes(), exact_bytes);
    drop(registry);
    assert_eq!(budget.used_bytes(), 0);
}

#[test]
fn failed_directory_arena_and_lock_allocations_release_every_temporary_charge() {
    let root_short = RegistryBudget::new(root_bytes() - 1);
    assert!(matches!(
        registry_with_budget(growing_config(), root_short.clone()),
        Err(RegistrationError::Capacity(CapacityError::BufferLimit))
    ));
    assert_eq!(root_short.used_bytes(), 0);

    for stable in [false, true] {
        let arena = registry_arena_accounted_bytes(REGISTRY_SEGMENT_SLOTS, stable).unwrap() as u64;
        let pointers = (RECORD_LOCK_SEGMENTS_PER_REGISTRY_SEGMENT
            * std::mem::size_of::<Arc<RecordLockSegment>>()) as u64;
        let partial_limits = [
            bucket_cell_bytes() - 1,
            bucket_cell_bytes() + arena - 1,
            bucket_cell_bytes() + arena + pointers - 1,
            bucket_cell_bytes() + segment_bytes(stable) - 1,
        ];
        for additional in partial_limits {
            let budget = RegistryBudget::new(root_bytes() + additional);
            let registry = registry_with_budget(
                growing_config().with_bounded_atomic_values(stable),
                budget.clone(),
            )
            .unwrap();
            assert!(matches!(
                registry.reserve_candidate(b"rejected"),
                Err(AccessError::Capacity(CapacityError::BufferLimit))
            ));
            assert_eq!(budget.used_bytes(), root_bytes());
            assert_eq!(registry.usage().allocated_registry_bytes(), root_bytes());
            assert_eq!(registry.usage().retained_records(), 0);
            assert_eq!(registry.usage().retained_key_bytes(), 0);
            // Scalar reservation retains the existing monotonic ID rule.
            assert_eq!(registry.usage().consumed_record_ids(), 1);
            let RegistryStorage::LazySegmented(storage) = &registry.storage else {
                unreachable!();
            };
            assert_eq!(storage.allocated_segments().count(), 0);
            assert!(storage.buckets.iter().all(|bucket| bucket.get().is_none()));
            drop(registry);
            assert_eq!(budget.used_bytes(), 0);
        }
    }
}

#[test]
fn shared_budget_can_be_reused_after_another_table_releases_its_allocations() {
    let one_table_growth = bucket_cell_bytes() + segment_bytes(false);
    let budget = RegistryBudget::new(2 * root_bytes() + one_table_growth);
    let first = registry_with_budget(growing_config(), budget.clone()).unwrap();
    let second = registry_with_budget(growing_config(), budget.clone()).unwrap();
    first.reserve_candidate(b"first").unwrap();
    assert_eq!(budget.used_bytes(), budget.max_bytes());
    assert!(matches!(
        second.reserve_candidate(b"rejected"),
        Err(AccessError::Capacity(CapacityError::BufferLimit))
    ));
    assert_eq!(second.usage().retained_records(), 0);
    assert_eq!(second.usage().allocated_registry_bytes(), root_bytes());
    drop(first);
    assert_eq!(budget.used_bytes(), root_bytes());
    let candidate = second.reserve_candidate(b"retry").unwrap();
    assert_eq!(candidate.id.get(), 2);
    assert_eq!(budget.used_bytes(), root_bytes() + one_table_growth);
    drop(second);
    assert_eq!(budget.used_bytes(), 0);
}

#[test]
fn failed_batch_growth_does_not_consume_ids_or_retained_quota() {
    let budget = RegistryBudget::new(root_bytes() + bucket_cell_bytes() + segment_bytes(false));
    let registry = registry_with_budget(growing_config(), budget.clone()).unwrap();
    for _ in 0..REGISTRY_SEGMENT_SLOTS - 1 {
        registry.reserve_candidate(b"x").unwrap();
    }
    let before = registry.usage();
    let mut candidates = Vec::with_capacity(2);
    let mut tokens = Vec::with_capacity(2);
    assert_eq!(
        registry
            .reserve_candidate_batch_with_mode(
                2,
                1,
                RecordTokenMode::DirectRecordPointer,
                &mut candidates,
                &mut tokens,
            )
            .unwrap(),
        CandidateBatchReservation::RetryScalar,
    );
    assert!(candidates.is_empty());
    assert!(tokens.is_empty());
    assert_eq!(registry.usage(), before);
    assert_eq!(budget.used_bytes(), before.allocated_registry_bytes());
    // Existing capacity remains usable after the failed batch.
    assert_eq!(
        registry.reserve_candidate(b"x").unwrap().id.get(),
        REGISTRY_SEGMENT_SLOTS as u64
    );
}

#[test]
fn detached_lock_keeps_only_its_live_arena_and_target_charged() {
    for eager in [false, true] {
        for stable in [false, true] {
            let budget = RegistryBudget::new(u64::MAX);
            let mut config = growing_config().with_bounded_atomic_values(stable);
            let slots = if eager { 3 } else { REGISTRY_SEGMENT_SLOTS };
            if eager {
                config = config
                    .with_max_consumed_record_ids(slots as u64)
                    .with_registry_layout(RegistryLayout::EagerContiguous {
                        max_bytes: eager_registry_accounted_bytes(slots, stable).unwrap(),
                    });
            }
            let registry = registry_with_budget(config, budget.clone()).unwrap();
            let candidate = registry.reserve_candidate(b"held").unwrap();
            let (record, target) = registry.resolve_with_segment(candidate.id).unwrap();
            let target = Arc::clone(target);
            let before = record.version.observe().unwrap();
            let mut detached = record
                .version
                .try_acquire_detached(OwnerId::new(1).unwrap())
                .unwrap();
            let used_before_drop = budget.used_bytes();
            drop(registry);
            let surviving = registry_arena_accounted_bytes(slots, stable).unwrap() as u64
                + record_lock_accounted_bytes() as u64;
            assert_eq!(budget.used_bytes(), surviving);
            assert!(budget.used_bytes() < used_before_drop);
            let record = target.record_at(0, AdapterPhase::Release).unwrap();
            detached.release_abort(&record.version).unwrap();
            assert_eq!(record.version.observe().unwrap(), before);
            drop(target);
            assert_eq!(budget.used_bytes(), 0);
        }
    }
}

#[test]
fn cached_resolved_token_keeps_its_value_and_budget_after_directory_growth() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let budget = RegistryBudget::new(u64::MAX);
    let table = Table::with_directory_mode_and_budget(
        &runtime,
        Directory::Memory(MemoryDirectory::default()),
        growing_config(),
        RecordTokenMode::DirectRecordPointer,
        budget.clone(),
    )
    .unwrap();
    let mut worker = runtime.attach().unwrap();
    let mut seed = worker.begin().unwrap();
    table
        .put_presence_inner(&mut seed, None, b"cached", b"before")
        .unwrap();
    assert!(matches!(
        seed.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));
    let mut read = worker.begin().unwrap();
    let (_, resolved) = table
        .contains_resolving_inner(&mut read, None, b"cached")
        .unwrap();
    assert!(matches!(
        read.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));
    let cache = table.dense_resolved_cache(1).unwrap();
    cache.remember(0, resolved).unwrap();

    for index in 0..3 * REGISTRY_SEGMENT_SLOTS {
        let mut transaction = worker.begin().unwrap();
        table
            .put_presence_inner(&mut transaction, None, &index.to_le_bytes(), b"value")
            .unwrap();
        assert!(matches!(
            transaction.commit().unwrap(),
            CommitOutcome::Committed(_)
        ));
    }
    let after_growth = budget.used_bytes();
    assert_eq!(cache.get(0).unwrap(), Some(resolved));
    drop(table);
    assert_eq!(budget.used_bytes(), after_growth);
    let mut update = worker.begin().unwrap();
    assert!(cache
        .table
        .put_resolved_with_previous_presence(&mut update, resolved, b"after")
        .unwrap());
    assert!(matches!(
        update.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));
    let mut verify = worker.begin().unwrap();
    cache
        .table
        .visit_get_resolved(&mut verify, resolved, |value| {
            assert_eq!(value.map(Value::as_ref), Some(&b"after"[..]));
        })
        .unwrap();
    assert!(matches!(
        verify.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));
    drop(cache);
    drop(worker);
    drop(runtime);
    assert_eq!(budget.used_bytes(), 0);
}
