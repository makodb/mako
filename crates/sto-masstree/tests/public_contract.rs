use sto_masstree::{
    InsertOutcome, PointMutation, PointReadBatch, PointSession, RegistryLayout, ScanBound,
    ScanDirection, ScanRecord, ScanRequest, Table, TableConfig, TableHealth, TableUsage, Value,
};

fn assert_send_sync<T: Send + Sync>() {}
fn assert_clone<T: Clone>() {}

#[allow(dead_code)]
fn borrowed_fixed_visitors_are_public(
    session: &mut PointSession<'_, '_>,
    batch: &mut PointReadBatch,
    keys: &[[u8; 8]],
) {
    let _ = session.visit_fixed(keys, batch, |_index, _value| {});
    let _ = session.modify_fixed_visit(keys, batch, |_index, _value| PointMutation::Keep);
}

#[test]
fn public_handles_and_snapshots_have_shareable_ownership() {
    assert_send_sync::<Table>();
    assert_clone::<Table>();
    assert_send_sync::<Value>();
    assert_send_sync::<PointReadBatch>();
    let _ = InsertOutcome::Inserted;
    let _ = TableHealth::Healthy;
    let _ = std::mem::size_of::<TableUsage>();
    let _ = std::mem::size_of::<ScanRecord>();
    let request = ScanRequest::new(ScanDirection::Reverse, 7)
        .with_lower(ScanBound::Included(b"a"))
        .with_upper(ScanBound::Excluded(b"z"));
    assert_eq!(request.limit(), 7);
    let value = Value::from(&b"binary\0value"[..]);
    assert_eq!(value.as_ref(), b"binary\0value");
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
        .with_scan_chunk_records(2)
        .with_scan_initial_key_arena_bytes(3)
        .with_scan_max_key_arena_bytes(4)
        .with_max_scan_chunks(5)
        .with_max_scan_physical_records(6);
    assert_eq!(config.max_retained_records(), 11);
    assert_eq!(config.max_retained_key_bytes(), 22);
    assert_eq!(config.max_consumed_record_ids(), 33);
    assert_eq!(config.scan_chunk_records(), 2);
    assert_eq!(config.scan_initial_key_arena_bytes(), 3);
    assert_eq!(config.scan_max_key_arena_bytes(), 4);
    assert_eq!(config.max_scan_chunks(), 5);
    assert_eq!(config.max_scan_physical_records(), 6);
}

#[test]
fn public_registry_layout_defaults_to_lazy_and_preserves_an_eager_budget() {
    const EAGER_BUDGET_BYTES: usize = 2 * 1024 * 1024;

    assert_eq!(RegistryLayout::default(), RegistryLayout::LazySegmented);
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
