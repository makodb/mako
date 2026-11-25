#![cfg(feature = "replication")]
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
    let store = Store::new(0, false, log.clone()).unwrap();
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
fn local_checkpoint_replays_suffix_and_tombstones_from_rocks_base() {
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
    assert!(root.path().join("log.data/CURRENT").is_file());
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

struct InterruptedInstall {
    log: Arc<LocalLog>,
    reached: std::sync::mpsc::SyncSender<()>,
    release: std::sync::Mutex<std::sync::mpsc::Receiver<()>>,
}
impl mako_protocol::journal::Journal for InterruptedInstall {
    fn durable(&self) -> bool {
        self.log.durable()
    }
    fn apply(&self, r: &mako_protocol::journal::Record) -> Result<mako_protocol::journal::Entry> {
        self.log.apply(r)
    }
    fn lookup(
        &self,
        id: mako_protocol::journal::TransactionId,
    ) -> Result<Option<mako_protocol::journal::Entry>> {
        self.log.lookup(id)
    }
    fn entries(&self) -> Result<Vec<mako_protocol::journal::Entry>> {
        self.log.entries()
    }
}
impl mako_protocol::contracts::CommitLog for InterruptedInstall {
    fn ready(&self) -> bool {
        self.log.ready()
    }
    fn health(&self) -> Result<()> {
        self.log.health()
    }
    fn replica_status(&self, owner: u32) -> ReplicaStatus {
        self.log.replica_status(owner)
    }
    fn admit(&self) -> Result<Box<dyn mako_protocol::contracts::CommitPermit>> {
        self.log.admit()
    }
    fn append(&self, bytes: &[u8], time: u32) -> Result<()> {
        self.log.append(bytes, time)
    }
    fn publish(
        &self,
        _: mako_protocol::journal::TransactionId,
        _: &mut dyn FnMut() -> Result<()>,
    ) -> Result<()> {
        self.reached.send(()).unwrap();
        self.release.lock().unwrap().recv().unwrap();
        Err(Error::new(
            ErrorCode::Unknown,
            "injected stop after durable Complete",
        ))
    }
}
#[test]
fn checkpoint_covers_saved_complete_before_affine_install() {
    let root = tempfile::tempdir().unwrap();
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
    let (reached, wait) = std::sync::mpsc::sync_channel(1);
    let (release, resume) = std::sync::mpsc::sync_channel(1);
    let interrupted = Arc::new(InterruptedInstall {
        log: log.clone(),
        reached,
        release: std::sync::Mutex::new(resume),
    });
    let store = Store::new(0, false, interrupted).unwrap();
    {
        let mut worker = store.database.worker().unwrap();
        store.open(&mut worker, target().access.table, 1).unwrap();
    }
    log.start_engine(
        raft_host::LocalConfig {
            group: 77,
            directory: root.path().join("log"),
            create: true,
            sync_writes: true,
        },
        &store,
        schema,
        b"checkpoint fixture".to_vec(),
    )
    .unwrap();
    let copy = store.clone();
    let writer = std::thread::spawn(move || {
        let mut worker = copy.database.worker().unwrap();
        client().run(copy, &mut worker, |tx| {
            tx.mutate(&target(), Mutation::Put, b"one", b"committed-1")?;
            tx.mutate(&target(), Mutation::Put, b"two", b"committed-2")?;
            Ok(())
        })
    });
    wait.recv_timeout(Duration::from_secs(10)).unwrap();
    log.checkpoint().unwrap();
    release.send(()).unwrap();
    assert_eq!(writer.join().unwrap().unwrap_err().code, ErrorCode::Unknown);
    stop(log, store);
    let (log, store) = start(root.path(), false);
    assert_eq!(
        get(&store, b"one").as_deref(),
        Some(b"committed-1".as_slice())
    );
    assert_eq!(
        get(&store, b"two").as_deref(),
        Some(b"committed-2".as_slice())
    );
    stop(log, store);
}

#[test]
fn corrupt_engine_checkpoint_is_rejected_before_admission() {
    let root = tempfile::tempdir().unwrap();
    let (log, store) = start(root.path(), true);
    put(&store, b"key", b"saved");
    log.checkpoint().unwrap();
    stop(log, store);
    let checkpoint = std::fs::read_dir(root.path().join("log/checkpoints"))
        .unwrap()
        .next()
        .unwrap()
        .unwrap()
        .path();
    let rows = checkpoint.join("STO-CHECKPOINT");
    let mut bytes = std::fs::read(&rows).unwrap();
    *bytes.last_mut().unwrap() ^= 1;
    std::fs::write(rows, bytes).unwrap();
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
    let store = Store::new(0, false, log.clone()).unwrap();
    let failure = log
        .start_engine(
            raft_host::LocalConfig {
                group: 77,
                directory: root.path().join("log"),
                create: false,
                sync_writes: true,
            },
            &store,
            schema,
            b"checkpoint fixture".to_vec(),
        )
        .unwrap_err();
    assert!(
        failure.message.contains("corrupt")
            || failure.message.contains("checksum")
            || failure.message.contains("identity"),
        "{failure}"
    );
}
