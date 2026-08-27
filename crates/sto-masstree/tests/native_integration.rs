#![cfg(mtree_native_integration)]

use masstree::{Runtime as MasstreeRuntime, RuntimeConfig as MasstreeRuntimeConfig};
use sto_core::{AbortReason, CommitOutcome, Conflict, Runtime, RuntimeConfig};
use sto_masstree::{ScanBound, ScanDirection, ScanRequest, Table, TableConfig};

#[test]
fn native_point_commit_read_and_abort_round_trip() {
    let native_runtime = MasstreeRuntime::new(MasstreeRuntimeConfig::new()).unwrap();
    let native_worker = native_runtime.attach().unwrap();
    let tree = native_runtime.create_tree(&native_worker).unwrap();
    let sto_runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let table = Table::new(&sto_runtime, tree, TableConfig::default()).unwrap();
    let mut sto_worker = sto_runtime.attach().unwrap();
    let key = b"native\0point";

    let mut write = sto_worker.begin().unwrap();
    assert_eq!(
        table
            .put(&mut write, &native_worker, key, b"committed")
            .unwrap(),
        None
    );
    assert!(matches!(
        write.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));

    let mut read = sto_worker.begin().unwrap();
    assert_eq!(
        table
            .get(&mut read, &native_worker, key)
            .unwrap()
            .as_deref(),
        Some(&b"committed"[..])
    );
    assert!(matches!(
        read.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));

    let mut aborted = sto_worker.begin().unwrap();
    table
        .put(&mut aborted, &native_worker, key, b"aborted")
        .unwrap();
    assert_eq!(aborted.abort().reason(), &AbortReason::Explicit);

    let mut verify = sto_worker.begin().unwrap();
    assert_eq!(
        table
            .get(&mut verify, &native_worker, key)
            .unwrap()
            .as_deref(),
        Some(&b"committed"[..])
    );
    assert!(matches!(
        verify.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));
    native_worker.quiesce().unwrap();
}

#[test]
fn native_transactional_scan_resumes_and_rejects_a_phantom() {
    let native_runtime = MasstreeRuntime::new(MasstreeRuntimeConfig::new()).unwrap();
    let native_worker = native_runtime.attach().unwrap();
    let tree = native_runtime.create_tree(&native_worker).unwrap();
    let sto_runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let config = TableConfig::new()
        .with_scan_chunk_records(1)
        .with_scan_initial_key_arena_bytes(1)
        .with_scan_max_key_arena_bytes(64)
        .with_max_scan_chunks(32);
    let table = Table::new(&sto_runtime, tree, config).unwrap();
    let mut sto_worker = sto_runtime.attach().unwrap();

    let mut seed = sto_worker.begin().unwrap();
    table
        .put(&mut seed, &native_worker, b"scan/a", b"A")
        .unwrap();
    table
        .put(&mut seed, &native_worker, b"scan/long", b"L")
        .unwrap();
    assert!(matches!(
        seed.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));

    let mut scanner = sto_worker.begin().unwrap();
    let rows = table
        .scan(
            &mut scanner,
            &native_worker,
            ScanRequest::new(ScanDirection::Forward, 8)
                .with_lower(ScanBound::Included(b"scan/"))
                .with_upper(ScanBound::Excluded(b"scan0")),
        )
        .unwrap();
    let keys: Vec<_> = rows.iter().map(|row| row.key().to_vec()).collect();
    assert_eq!(keys, [b"scan/a".to_vec(), b"scan/long".to_vec()]);

    std::thread::scope(|scope| {
        let native_runtime = native_runtime.clone();
        let sto_runtime = sto_runtime.clone();
        let table = table.clone();
        scope
            .spawn(move || {
                let native_worker = native_runtime.attach().unwrap();
                let mut sto_worker = sto_runtime.attach().unwrap();
                let mut writer = sto_worker.begin().unwrap();
                table
                    .put(&mut writer, &native_worker, b"scan/new", b"N")
                    .unwrap();
                assert!(matches!(
                    writer.commit().unwrap(),
                    CommitOutcome::Committed(_)
                ));
                native_worker.quiesce().unwrap();
            })
            .join()
            .unwrap();
    });

    assert_eq!(
        scanner.commit().unwrap(),
        CommitOutcome::Aborted(AbortReason::Conflict(Conflict::ReadValidation))
    );
    native_worker.quiesce().unwrap();
}
