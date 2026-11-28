#![cfg(all(feature = "replication", feature = "rocksdb-engine"))]
use sto_core::protocol::journal::{
    Journal, Member, Outcome, Record, Transaction, TransactionId, VERSION,
};
use mako_runtime::{
    client::Client,
    log::{Log, MaterializationOptions, Mode},
    migration::{Kind, Schema, TableSpec},
    rpc::Network,
    storage::Store,
    wire::*,
};
use std::{
    collections::BTreeMap,
    path::Path,
    sync::Arc,
    time::{Duration, Instant},
};
fn target() -> Target {
    Target {
        owner: 0,
        log_id: 1,
        access: Access {
            table: TableKey {
                table: 1,
                warehouse: 0,
            },
            route: None,
        },
    }
}
fn wait(mut ready: impl FnMut() -> bool) {
    let deadline = Instant::now() + Duration::from_secs(20);
    while !ready() {
        assert!(Instant::now() < deadline);
        std::thread::sleep(Duration::from_millis(10));
    }
}
fn start(
    root: &Path,
    create: bool,
    masstree: bool,
    raft: bool,
    limits: MaterializationOptions,
) -> (Arc<Log<Mode>>, Arc<Store>) {
    let schema = Schema::new(
        vec![TableSpec {
            id: 1,
            name: "rows".into(),
            kind: Kind::Governed,
            warehouse: false,
            initial_owner: 0,
        }],
        1,
        0,
    )
    .unwrap();
    let log = Log::new(Duration::from_secs(5)).unwrap();
    let database = if masstree {
        mako_storage_masstree::database().unwrap()
    } else {
        mako_storage_rocksdb::database(root.join("data"), 0, true).unwrap()
    };
    let backing = database
        .persistent()
        .unwrap_or_else(|| mako_storage_rocksdb::backing(root.join("data"), 0, true).unwrap());
    let store = Store::with_engine(0, false, log.clone(), database).unwrap();
    {
        let mut worker = store.database.worker().unwrap();
        store.open(&mut worker, target().access.table, 1).unwrap();
    }
    let config = if raft {
        let socket = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        let address = socket.local_addr().unwrap().to_string();
        drop(socket);
        Mode::Raft(raft_host::Config {
            group: 77,
            node: 7,
            listen: address.clone(),
            peers: vec![raft_host::Peer { id: 7, address }],
            directory: root.join("log"),
            create,
            sync_writes: true,
            ..Default::default()
        })
    } else {
        Mode::Local(raft_host::LocalConfig {
            initial_state: [0; 32],
            group: 77,
            directory: root.join("log"),
            create,
            sync_writes: true,
        })
    };
    log.start_materialized(
        config,
        &store,
        schema,
        b"shared image".to_vec(),
        backing,
        limits,
    )
    .unwrap();
    wait(|| log.status().unwrap().is_leader);
    (log, store)
}
fn client() -> Client {
    Client::new(
        Arc::new(Network::default()),
        BTreeMap::new(),
        Stream { node: 0, worker: 0 },
        0,
        Duration::from_secs(5),
    )
    .unwrap()
}
fn put(store: &Arc<Store>, key: &[u8], value: &[u8]) {
    let mut worker = store.database.worker().unwrap();
    client()
        .run(store.clone(), &mut worker, |tx| {
            tx.mutate(&target(), Mutation::Put, key, value).map(|_| ())
        })
        .unwrap();
}
fn get(store: &Arc<Store>, key: &[u8]) -> Option<Vec<u8>> {
    let mut worker = store.database.worker().unwrap();
    let mut client = client();
    let deadline = Instant::now() + Duration::from_secs(10);
    loop {
        match client.run(store.clone(), &mut worker, |tx| tx.get(&target(), key)) {
            Ok(value) => return value,
            // A restored prepared writer can finish concurrently with this
            // reader. Only a definite OCC abort permits a new attempt.
            Err(error) if error.code == ErrorCode::Conflict && Instant::now() < deadline => {
                std::thread::sleep(Duration::from_millis(10));
            }
            Err(error) => panic!("read {key:?}: {error:?}"),
        }
    }
}
fn deferred() -> MaterializationOptions {
    MaterializationOptions {
        records: 4096,
        interval: Duration::from_secs(3600),
        ..Default::default()
    }
}
fn prepared() -> Transaction {
    Transaction {
        version: VERSION,
        id: TransactionId {
            coordinator: 0,
            worker: 9,
            incarnation: 321,
            attempt: 1,
        },
        time_term: 20,
        durable: true,
        members: vec![Member {
            owner: 0,
            lease: 1,
            endpoints: vec!["127.0.0.1:1".into()],
        }],
    }
}
#[test]
fn materialization_crash_fixture() {
    let Ok(root) = std::env::var("MAKO_MATERIALIZATION_CRASH") else {
        return;
    };
    let raft = std::env::var("MAKO_MATERIALIZATION_RAFT").unwrap() == "true";
    let (log, store) = start(Path::new(&root), true, true, raft, deferred());
    put(&store, b"key", b"base");
    put(&store, b"gone", b"old");
    let cut = log.checkpoint().unwrap();
    assert_eq!(log.materialized_position().unwrap(), cut);
    put(&store, b"key", b"suffix");
    let mut worker = store.database.worker().unwrap();
    client()
        .run(store.clone(), &mut worker, |tx| {
            tx.mutate(&target(), Mutation::Remove, b"gone", b"")
                .map(|_| ())
        })
        .unwrap();
    let tx = prepared();
    log.apply(&Record::Begin(tx.clone())).unwrap();
    log.apply(&Record::Prepare {
        transaction: tx,
        rows: vec![mako_runtime::redo::Row {
            table: 1,
            key: b"prepared".to_vec(),
            value: Some(b"invisible".to_vec()),
            time_term: 20,
        }],
        receipt: None,
    })
    .unwrap();
    log.apply(&Record::Decide {
        transaction: prepared(),
        outcome: Outcome::Commit,
    })
    .unwrap();
    assert_eq!(log.materialized_position().unwrap(), cut);
    assert!(log.status().unwrap().applied.index > cut.index);
    // The parent kills this process after all persistence assertions passed.
    // process::exit would run C++ atexit destructors concurrently with RocksDB
    // background threads, which is neither orderly shutdown nor a crash.
    let root = Path::new(&root);
    std::fs::write(root.join("crash-ready.tmp"), b"ready").unwrap();
    std::fs::rename(root.join("crash-ready.tmp"), root.join("crash-ready")).unwrap();
    loop {
        std::thread::park();
    }
}
fn crash_switch(raft: bool, masstree: bool) {
    let root = tempfile::tempdir().unwrap();
    let mut child = std::process::Command::new(std::env::current_exe().unwrap())
        .args(["materialization_crash_fixture", "--exact", "--nocapture"])
        .env("MAKO_MATERIALIZATION_CRASH", root.path())
        .env("MAKO_MATERIALIZATION_RAFT", raft.to_string())
        .spawn()
        .unwrap();
    let deadline = Instant::now() + Duration::from_secs(30);
    while !root.path().join("crash-ready").is_file() {
        assert!(
            child.try_wait().unwrap().is_none(),
            "crash fixture exited before its persistence assertions"
        );
        if Instant::now() >= deadline {
            child.kill().unwrap();
            let _ = child.wait();
            panic!("crash fixture did not reach the kill point");
        }
        std::thread::sleep(Duration::from_millis(10));
    }
    child.kill().unwrap();
    use std::os::unix::process::ExitStatusExt;
    assert_eq!(child.wait().unwrap().signal(), Some(9));
    let (log, store) = start(root.path(), false, masstree, raft, deferred());
    assert!(log.lookup(prepared().id).unwrap().unwrap().unresolved());
    {
        let mut worker = store.database.worker().unwrap();
        let table = store.table(target().access.table).unwrap();
        assert!(!worker
            .checkpoint_page(&table.tree, None, 256)
            .unwrap()
            .rows
            .iter()
            .any(|row| row.key == b"prepared" && row.value.is_some()));
    }
    assert_eq!(get(&store, b"key").as_deref(), Some(b"suffix".as_slice()));
    assert_eq!(get(&store, b"gone"), None);
    assert_eq!(
        get(&store, b"prepared").as_deref(),
        Some(b"invisible".as_slice())
    );
    assert_eq!(
        log.lookup(prepared().id).unwrap().unwrap().decision,
        Some(Outcome::Commit)
    );
    put(&store, b"after", b"switch");
    log.flush().unwrap();
    assert_eq!(
        log.materialized_position().unwrap(),
        log.status().unwrap().applied
    );
    store.shutdown();
    log.shutdown();
    drop(store);
    drop(log);
    let (log, store) = start(root.path(), false, !masstree, raft, deferred());
    assert_eq!(get(&store, b"after").as_deref(), Some(b"switch".as_slice()));
    assert_eq!(get(&store, b"gone"), None);
    assert_eq!(
        log.lookup(prepared().id).unwrap().unwrap().decision,
        Some(Outcome::Commit)
    );
    assert_eq!(
        get(&store, b"prepared").as_deref(),
        Some(b"invisible".as_slice())
    );
    store.shutdown();
    log.shutdown();
}
#[test]
fn local_crash_recovers_log_gap_and_switches_to_direct_rocks() {
    crash_switch(false, false);
}
#[test]
fn local_crash_recovers_log_gap_in_masstree() {
    crash_switch(false, true);
}
#[test]
fn raft_crash_recovers_log_gap_and_switches_to_direct_rocks() {
    crash_switch(true, false);
}
#[test]
fn raft_crash_recovers_log_gap_in_masstree() {
    crash_switch(true, true);
}
#[test]
fn timer_and_shutdown_materialize_committed_rows() {
    let root = tempfile::tempdir().unwrap();
    let (log, store) = start(
        root.path(),
        true,
        true,
        false,
        MaterializationOptions {
            interval: Duration::from_millis(20),
            ..deferred()
        },
    );
    put(&store, b"timer", b"saved");
    let applied = log.status().unwrap().applied;
    wait(|| log.materialized_position().unwrap() == applied);
    store.shutdown();
    log.shutdown();
    drop(store);
    drop(log);
    let (log, store) = start(root.path(), false, true, false, deferred());
    put(&store, b"shutdown", b"saved");
    assert!(log.materialized_position().unwrap().index < log.status().unwrap().applied.index);
    store.shutdown();
    log.shutdown();
    drop(store);
    drop(log);
    let backing = mako_storage_rocksdb::backing(root.path().join("data"), 0, true).unwrap();
    assert!(backing
        .page(1, None, 256)
        .unwrap()
        .rows
        .iter()
        .any(|r| r.key == b"shutdown" && r.value.as_deref() == Some(b"saved")));
}
#[test]
fn record_limit_flushes_and_keeps_later_effects_buffered() {
    let root = tempfile::tempdir().unwrap();
    let limits = MaterializationOptions {
        records: 2,
        ..deferred()
    };
    let (log, store) = start(root.path(), true, true, false, limits);
    for n in 0..5 {
        let mut tx = prepared();
        tx.id.attempt = n + 1;
        log.apply(&Record::Begin(tx)).unwrap();
        let applied = log.status().unwrap().applied.index;
        let durable = log.materialized_position().unwrap().index;
        assert!(applied - durable < 2);
    }
    store.shutdown();
    log.shutdown();
}

#[cfg(feature = "test-hooks")]
fn failed_materialization(fault: sto_rocksdb::ApplyFault) {
    let root = tempfile::tempdir().unwrap();
    let data = mako_storage_rocksdb::database(root.path().join("data"), 0, true).unwrap();
    let native = data
        .as_any()
        .downcast_ref::<Arc<sto_rocksdb::Database>>()
        .unwrap()
        .clone();
    let log = Log::<Mode>::new(Duration::from_secs(5)).unwrap();
    let store = Store::with_engine(
        0,
        false,
        log.clone(),
        mako_storage_masstree::database().unwrap(),
    )
    .unwrap();
    let schema = Schema::new(
        vec![TableSpec {
            id: 1,
            name: "rows".into(),
            kind: Kind::Governed,
            warehouse: false,
            initial_owner: 0,
        }],
        1,
        0,
    )
    .unwrap();
    {
        let mut worker = store.database.worker().unwrap();
        store.open(&mut worker, target().access.table, 1).unwrap();
    }
    log.start_materialized(
        Mode::Local(raft_host::LocalConfig {
            initial_state: [0; 32],
            group: 77,
            directory: root.path().join("log"),
            create: true,
            sync_writes: true,
        }),
        &store,
        schema,
        b"shared image".to_vec(),
        data.persistent().unwrap(),
        deferred(),
    )
    .unwrap();
    put(&store, b"saved", b"committed");
    assert!(log.materialized_position().unwrap().index < log.status().unwrap().applied.index);
    native.inject_apply_fault(fault);
    assert!(log.flush().is_err());
    assert!(log.health().is_err());
    assert!(store.protect().is_err());
    store.shutdown();
    log.shutdown();
    drop(store);
    drop(log);
    drop(native);
    drop(data);
    let (log, store) = start(root.path(), false, false, false, deferred());
    assert_eq!(
        get(&store, b"saved").as_deref(),
        Some(b"committed".as_slice())
    );
    store.shutdown();
    log.shutdown();
}
#[cfg(feature = "test-hooks")]
#[test]
fn failed_batch_fences_admission_and_replays_the_committed_gap() {
    failed_materialization(sto_rocksdb::ApplyFault::BeforeBatch);
}
#[cfg(feature = "test-hooks")]
#[test]
fn uncertain_batch_fences_admission_and_recovers_without_redeciding() {
    failed_materialization(sto_rocksdb::ApplyFault::AfterBatch);
}
#[test]
fn byte_and_row_limits_flush_large_single_effects() {
    for limits in [
        MaterializationOptions {
            bytes: 1,
            ..deferred()
        },
        MaterializationOptions {
            rows: 1,
            ..deferred()
        },
    ] {
        let root = tempfile::tempdir().unwrap();
        let (log, store) = start(root.path(), true, true, false, limits);
        put(&store, b"single", b"bounded");
        assert_eq!(
            log.materialized_position().unwrap(),
            log.status().unwrap().applied
        );
        store.shutdown();
        log.shutdown();
    }
}

#[test]
fn maximum_size_bootstrap_rows_materialize_in_bounded_pages() {
    let root = tempfile::tempdir().unwrap();
    let log = Log::<Mode>::new(Duration::from_secs(5)).unwrap();
    let store = Store::with_engine(
        0,
        false,
        log.clone(),
        mako_storage_masstree::database().unwrap(),
    )
    .unwrap();
    let schema = Schema::new(
        vec![TableSpec {
            id: 1,
            name: "rows".into(),
            kind: Kind::Governed,
            warehouse: false,
            initial_owner: 0,
        }],
        1,
        0,
    )
    .unwrap();
    let key = |n: u32| {
        let mut key = vec![0; MAX_KEY];
        key[..4].copy_from_slice(&n.to_be_bytes());
        key
    };
    let value = vec![7; MAX_VALUE];
    {
        let mut worker = store.database.worker().unwrap();
        let table = store.open(&mut worker, target().access.table, 1).unwrap();
        for begin in (0..300u32).step_by(20) {
            let mut transaction = worker.begin().unwrap();
            for n in begin..begin + 20 {
                transaction.put(&table.tree, &key(n), &value).unwrap();
            }
            transaction.commit().unwrap();
        }
    }
    let backing = mako_storage_rocksdb::backing(root.path().join("data"), 0, true).unwrap();
    log.start_materialized(
        Mode::Local(raft_host::LocalConfig {
            initial_state: [0; 32],
            group: 77,
            directory: root.path().join("log"),
            create: true,
            sync_writes: true,
        }),
        &store,
        schema,
        b"shared image".to_vec(),
        backing,
        deferred(),
    )
    .unwrap();
    store.shutdown();
    log.shutdown();
    drop(store);
    drop(log);
    let (log, store) = start(root.path(), false, false, false, deferred());
    assert_eq!(store.table(target().access.table).unwrap().tree.len(), 300);
    for n in [0, 127, 128, 255, 256, 299] {
        assert_eq!(get(&store, &key(n)), Some(value.clone()));
    }
    store.shutdown();
    log.shutdown();
}
