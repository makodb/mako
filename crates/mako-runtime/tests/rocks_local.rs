#![cfg(all(feature = "logging", feature = "rocksdb-engine"))]
use mako_runtime::{
    client::Client,
    log::LocalLog,
    migration::{Kind, Schema, TableSpec},
    rpc::Network,
    storage::Store,
    wire::*,
};
use std::{collections::BTreeMap, path::Path, sync::Arc, time::Duration};
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
fn start(root: &Path, create: bool) -> (Arc<LocalLog>, Arc<Store>) {
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
    let log = LocalLog::new(Duration::from_secs(5)).unwrap();
    let store = Store::with_engine(
        0,
        false,
        log.clone(),
        mako_storage_rocksdb::database(root.join("data"), 0, true).unwrap(),
    )
    .unwrap();
    {
        let mut w = store.database.worker().unwrap();
        let table = store.open(&mut w, target().access.table, 1).unwrap();
        if create {
            let mut tx = w.begin().unwrap();
            tx.put(&table.tree, b"bootstrap", b"seed").unwrap();
            tx.commit().unwrap();
        }
    }
    log.start_engine(
        raft_host::LocalConfig {
            initial_state: [0; 32],
            group: 77,
            directory: root.join("log"),
            create,
            sync_writes: true,
        },
        &store,
        schema,
        b"checkpoint fixture".to_vec(),
    )
    .unwrap();
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
    client()
        .run(store.clone(), &mut worker, |tx| tx.get(&target(), key))
        .unwrap()
}
fn stop(log: Arc<LocalLog>, store: Arc<Store>) {
    store.shutdown();
    log.shutdown();
    drop(store);
    drop(log);
}

#[test]
fn rocks_local_checkpoint_reopens_atomic_data_and_metadata() {
    let root = tempfile::tempdir().unwrap();
    let (log, store) = start(root.path(), true);
    assert_eq!(
        store.log.append(&[], 0).unwrap_err().code,
        ErrorCode::Invalid
    );
    assert_eq!(
        get(&store, b"bootstrap").as_deref(),
        Some(b"seed".as_slice())
    );
    put(&store, b"key", b"at checkpoint");
    put(&store, b"deleted", b"old");
    {
        let mut worker = store.database.worker().unwrap();
        client()
            .run(store.clone(), &mut worker, |tx| {
                tx.mutate(&target(), Mutation::Remove, b"deleted", b"")
                    .map(|_| ())
            })
            .unwrap();
    }
    let cut = log.checkpoint().unwrap();
    assert!(cut.index > 1);
    assert_eq!(log.checkpoint().unwrap(), cut);
    put(&store, b"key", b"in suffix");
    stop(log, store);
    assert!(!root.path().join("rows").exists());
    assert!(root.path().join("log/checkpoints").is_dir());
    let (log, store) = start(root.path(), false);
    assert_eq!(
        get(&store, b"bootstrap").as_deref(),
        Some(b"seed".as_slice())
    );
    assert_eq!(
        get(&store, b"key").as_deref(),
        Some(b"in suffix".as_slice())
    );
    assert_eq!(get(&store, b"deleted"), None);
    let later = log.checkpoint().unwrap();
    assert!(later.index > cut.index);
    stop(log, store);
    let (log, store) = start(root.path(), false);
    assert_eq!(
        get(&store, b"key").as_deref(),
        Some(b"in suffix".as_slice())
    );
    stop(log, store);
}

#[test]
fn persistent_log_rejects_a_proposal_from_another_admission_term() {
    use sto_core::protocol::{
        contracts::CommitLog,
        journal::{Journal, Member, Record, Transaction, TransactionId, VERSION},
    };
    let root = tempfile::tempdir().unwrap();
    let (log, store) = start(root.path(), true);
    let permit = log.admit().unwrap();
    let term = permit.authority().unwrap();
    let transaction = Transaction {
        version: VERSION,
        id: TransactionId {
            coordinator: 0,
            worker: 0,
            incarnation: 99,
            attempt: 1,
        },
        time_term: 20,
        durable: true,
        members: vec![Member {
            owner: 0,
            endpoints: vec!["127.0.0.1:1".into()],
            lease: 1,
        }],
    };
    assert_eq!(
        log.apply_at(&Record::Begin(transaction.clone()), term + 1)
            .unwrap_err()
            .code,
        ErrorCode::RouteChanged
    );
    assert!(log.lookup(transaction.id).unwrap().is_none());
    log.apply_at(&Record::Begin(transaction.clone()), term)
        .unwrap();
    assert!(log.lookup(transaction.id).unwrap().is_some());
    drop(permit);
    stop(log, store);
}
