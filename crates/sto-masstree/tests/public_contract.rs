use std::sync::Arc;

use sto_masstree::{
    InsertOutcome, ScanBound, ScanDirection, ScanRecord, ScanRequest, Table, TableConfig,
    TableHealth, TableUsage, Value,
};

fn assert_send_sync<T: Send + Sync>() {}
fn assert_clone<T: Clone>() {}

#[test]
fn public_handles_and_snapshots_have_shareable_ownership() {
    assert_send_sync::<Table>();
    assert_clone::<Table>();
    assert_send_sync::<Value>();
    let _ = InsertOutcome::Inserted;
    let _ = TableHealth::Healthy;
    let _ = std::mem::size_of::<TableUsage>();
    let _ = std::mem::size_of::<ScanRecord>();
    let request = ScanRequest::new(ScanDirection::Reverse, 7)
        .with_lower(ScanBound::Included(b"a"))
        .with_upper(ScanBound::Excluded(b"z"));
    assert_eq!(request.limit(), 7);
    let _: Value = Arc::from(&b"binary\0value"[..]);
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
