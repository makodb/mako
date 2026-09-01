use sto_masstree::{
    InsertOutcome, PointMutation, PointReadBatch, PointSession, RegistryLayout, ResolvedRecord,
    ScanBound, ScanBytesRef, ScanControl, ScanDirection, ScanRecord, ScanRecordRef, ScanRequest,
    ScanScratch, ScanVisitOutcome, Table, TableConfig, TableCreateError, TableHealth, TableUsage,
    Value, ValueCopyOutcome,
};
#[cfg(feature = "fixed-u64")]
use {
    masstree::{Runtime as MasstreeRuntime, Worker as MasstreeWorker},
    std::sync::Arc,
    sto_core::Runtime as StoRuntime,
    sto_masstree::{FixedU64Batch, FixedU64CreateError, FixedU64Table},
};

fn assert_send_sync<T: Send + Sync>() {}
fn assert_clone<T: Clone>() {}

#[allow(dead_code)]
fn scanned_resolved_token_is_public(record: &ScanRecord) -> ResolvedRecord {
    record.resolved()
}

#[allow(dead_code)]
fn borrowed_scan_row_is_public<'row>(
    row: ScanRecordRef<'row>,
) -> (&'row [u8], &'row [u8], ResolvedRecord) {
    (row.key(), row.value(), row.resolved())
}

#[allow(dead_code)]
fn borrowed_scan_bytes_are_public<'row>(
    row: ScanBytesRef<'row>,
) -> (&'row [u8], &'row [u8], ResolvedRecord) {
    (row.key(), row.value(), row.resolved())
}

#[allow(dead_code)]
fn borrowed_byte_visitors_are_public(
    table: &Table,
    transaction: &mut sto_core::Transaction<'_, sto_core::Active>,
    worker: &masstree::Worker,
    resolved: ResolvedRecord,
    scratch: &mut ScanScratch,
) {
    let _ = table.visit_get_bytes(transaction, worker, b"key", |_| ());
    let _ = table.visit_get_resolving_bytes(transaction, worker, b"key", |_| ());
    let _ = table.visit_get_resolved_bytes(transaction, resolved, |_| ());
    let request = ScanRequest::new(ScanDirection::Forward, 1);
    let _ = table.visit_scan_bytes(transaction, worker, request, |_| ScanControl::Continue);
    let _ = table.visit_scan_bytes_with_scratch(transaction, worker, request, scratch, |_| {
        ScanControl::Continue
    });
}

#[allow(dead_code)]
fn caller_buffer_reads_are_public(
    table: &Table,
    transaction: &mut sto_core::Transaction<'_, sto_core::Active>,
    worker: &masstree::Worker,
    resolved: ResolvedRecord,
    output: &mut [u8],
) {
    let _: Result<(ValueCopyOutcome, ResolvedRecord), _> =
        table.copy_get_resolving(transaction, worker, b"key", output);
    let _: Result<ValueCopyOutcome, _> = table.copy_get_resolved(transaction, resolved, output);
    let _: Result<Option<ValueCopyOutcome>, _> =
        table.try_copy_get_cached_resolved(transaction, resolved, output);
}

#[allow(dead_code)]
fn direct_constructor_is_public(
    sto_runtime: &std::sync::Arc<sto_core::Runtime>,
    native_runtime: &masstree::Runtime,
    native_worker: &masstree::Worker,
    config: TableConfig,
) -> Result<Table, TableCreateError> {
    Table::new_direct(sto_runtime, native_runtime, native_worker, config)
}

#[cfg(feature = "fixed-u64")]
#[allow(dead_code)]
fn fixed_u64_constructor_is_public(
    sto_runtime: &Arc<StoRuntime>,
    native_runtime: &MasstreeRuntime,
    native_worker: &MasstreeWorker,
    config: TableConfig,
) -> Result<FixedU64Table, FixedU64CreateError> {
    FixedU64Table::new(sto_runtime, native_runtime, native_worker, config)
}

#[allow(dead_code)]
fn borrowed_fixed_visitors_are_public(
    session: &mut PointSession<'_, '_>,
    batch: &mut PointReadBatch,
    keys: &[[u8; 8]],
) {
    let _ = session.visit_fixed(keys, batch, |_index, _value| {});
    let _ = session.visit_fixed_bytes(keys, batch, |_index, _value| {});
    let _ = session.modify_fixed_visit(keys, batch, |_index, _value| PointMutation::Keep);
}

#[test]
fn public_handles_and_snapshots_have_shareable_ownership() {
    assert_send_sync::<Table>();
    assert_clone::<Table>();
    assert_send_sync::<Value>();
    assert_send_sync::<PointReadBatch>();
    assert_send_sync::<ResolvedRecord>();
    assert_send_sync::<TableCreateError>();
    assert_send_sync::<ScanScratch>();
    assert_clone::<ResolvedRecord>();
    let _ = InsertOutcome::Inserted;
    let _ = TableHealth::Healthy;
    let _ = std::mem::size_of::<TableUsage>();
    let _ = std::mem::size_of::<ScanRecord>();
    let _ = std::mem::size_of::<ScanRecordRef<'static>>();
    let _ = std::mem::size_of::<ScanBytesRef<'static>>();
    let _ = ScanControl::Continue;
    let _ = std::mem::size_of::<ScanVisitOutcome>();
    let _ = ValueCopyOutcome::Miss;
    assert_eq!(std::mem::size_of::<ResolvedRecord>(), 24);
    let request = ScanRequest::new(ScanDirection::Reverse, 7)
        .with_lower(ScanBound::Included(b"a"))
        .with_upper(ScanBound::Excluded(b"z"));
    assert_eq!(request.limit(), 7);
    let value = Value::from(&b"binary\0value"[..]);
    assert_eq!(value.as_ref(), b"binary\0value");
}

#[cfg(feature = "fixed-u64")]
#[test]
fn fixed_u64_public_types_preserve_shareable_ownership() {
    assert_send_sync::<FixedU64Table>();
    assert_clone::<FixedU64Table>();
    assert_send_sync::<FixedU64Batch>();
    assert_send_sync::<FixedU64CreateError>();
    fn assert_error<T: std::error::Error>() {}
    assert_error::<FixedU64CreateError>();
}

#[test]
fn point_read_batch_retains_public_scratch_capacity() {
    let mut batch = PointReadBatch::with_capacity(7);
    assert!(batch.capacity() >= 7);
    assert!(batch.is_empty());
    assert_eq!(batch.len(), 0);
    assert!(batch.results().is_empty());
    let retained = batch.capacity();
    batch.clear();
    assert_eq!(batch.capacity(), retained);
    assert!(PointReadBatch::new().is_empty());
}

#[test]
fn public_configuration_keeps_every_explicit_bound() {
    let config = TableConfig::new()
        .with_max_retained_records(11)
        .with_max_retained_key_bytes(22)
        .with_max_consumed_record_ids(33)
        .with_unique_lock_requests(true)
        .with_scan_chunk_records(2)
        .with_scan_initial_key_arena_bytes(3)
        .with_scan_max_key_arena_bytes(4)
        .with_max_scan_chunks(5)
        .with_max_scan_physical_records(6)
        .with_bounded_atomic_values(true);
    assert_eq!(config.max_retained_records(), 11);
    assert_eq!(config.max_retained_key_bytes(), 22);
    assert_eq!(config.max_consumed_record_ids(), 33);
    assert!(config.unique_lock_requests());
    assert_eq!(config.scan_chunk_records(), 2);
    assert_eq!(config.scan_initial_key_arena_bytes(), 3);
    assert_eq!(config.scan_max_key_arena_bytes(), 4);
    assert_eq!(config.max_scan_chunks(), 5);
    assert_eq!(config.max_scan_physical_records(), 6);
    assert!(config.bounded_atomic_values());
}

#[test]
fn public_registry_layout_defaults_to_lazy_and_preserves_an_eager_budget() {
    const EAGER_BUDGET_BYTES: usize = 2 * 1024 * 1024;

    assert_eq!(RegistryLayout::default(), RegistryLayout::LazySegmented);
    assert!(!TableConfig::default().unique_lock_requests());
    assert!(!TableConfig::default().bounded_atomic_values());
    assert_eq!(
        TableConfig::default().registry_layout(),
        RegistryLayout::LazySegmented
    );

    let config = TableConfig::new()
        .with_max_consumed_record_ids(32)
        .with_registry_layout(RegistryLayout::EagerContiguous {
            max_bytes: EAGER_BUDGET_BYTES,
        });
    assert_eq!(config.max_consumed_record_ids(), 32);
    assert_eq!(
        config.registry_layout(),
        RegistryLayout::EagerContiguous {
            max_bytes: EAGER_BUDGET_BYTES,
        }
    );
}
