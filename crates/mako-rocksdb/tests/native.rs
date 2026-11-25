#![cfg(feature = "rocksdb-engine")]
use mako_rocksdb::{
    Direction, ErrorKind, IteratorMode, Options, Outcome, ReadOptions, TransactionId, WriteBatch,
    DB,
};
use mako_runtime::{
    migration::{Kind, TableSpec},
    node::{Config, EngineConfig, PersistenceConfig, ReplicaConfig, ReplicaPeer, RunningNode},
    rpc::Network,
    wire::{Message, Reply},
};
use std::{
    collections::BTreeMap,
    path::{Path, PathBuf},
    process::{Child, Command, Stdio},
    sync::atomic::{AtomicU64, Ordering},
    time::{Duration, Instant},
};

struct Directory(PathBuf);
impl Directory {
    fn new() -> Self {
        static NEXT: AtomicU64 = AtomicU64::new(0);
        let path = std::env::temp_dir().join(format!(
            "mako-rocks-api-{}-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        ));
        std::fs::create_dir(&path).unwrap();
        Self(path)
    }
}
impl Drop for Directory {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}
fn options(masstree: bool) -> Options {
    let mut options = Options::default();
    options.create_if_missing(true);
    options.set_masstree(masstree);
    options.set_max_transactions(2);
    options
}
fn open(root: &Path, masstree: bool) -> DB {
    DB::open_cf(&options(masstree), root, ["other", "default"]).unwrap()
}

#[test]
fn rocks_crud_conflicts_ranges_and_reopen() {
    basics(false);
}
#[test]
#[cfg(feature = "masstree-engine")]
fn masstree_crud_conflicts_ranges_and_reopen() {
    basics(true);
}
fn basics(masstree: bool) {
    let root = Directory::new();
    let db = open(&root.0, masstree);
    let cf = db.cf_handle("other").unwrap();
    let tx = db.transaction();
    tx.put(b"", b"").unwrap();
    tx.put(b"a\0", b"one").unwrap();
    tx.put(b"a\xff", b"two").unwrap();
    tx.put_cf(cf, b"key", b"other").unwrap();
    assert_eq!(tx.get(b"a\0").unwrap(), Some(b"one".to_vec()));
    assert_eq!(db.get(b"a\0").unwrap(), None);
    let id = tx.transaction_id().unwrap();
    assert_eq!(db.resolve(id).unwrap_err().kind(), ErrorKind::Busy);
    tx.commit().unwrap();
    assert_eq!(db.resolve(id).unwrap(), Outcome::Commit);
    assert_eq!(db.get_cf(cf, b"key").unwrap(), Some(b"other".to_vec()));
    db.delete(b"missing").unwrap();
    let tx = db.transaction();
    tx.put(b"drop", b"never").unwrap();
    drop(tx);
    assert_eq!(db.get(b"drop").unwrap(), None);
    let tx = db.transaction();
    tx.put(b"rollback", b"never").unwrap();
    tx.rollback().unwrap();
    tx.put(b"rolled-back-reuse", b"yes").unwrap();
    tx.commit().unwrap();
    assert_eq!(db.get(b"rollback").unwrap(), None);
    let mut batch = WriteBatch::default();
    batch.put(b"a\0", b"replace");
    batch.delete(b"a\xff");
    batch.put_cf(cf, b"key", b"updated");
    db.write(batch).unwrap();
    let mut bounds = ReadOptions::default();
    bounds.set_iterate_lower_bound(b"a".to_vec());
    bounds.set_iterate_upper_bound(b"b".to_vec());
    let rows: Vec<_> = db
        .iterator_opt(IteratorMode::From(b"b", Direction::Reverse), bounds)
        .collect::<Result<_, _>>()
        .unwrap();
    assert_eq!(
        rows,
        [(b"a\0".as_slice().into(), b"replace".as_slice().into())]
    );
    let tx = db.transaction();
    tx.get_for_update(b"absent", true).unwrap();
    db.put(b"absent", b"conflict").unwrap();
    tx.put(b"should-not-exist", b"x").unwrap();
    assert_eq!(tx.commit().unwrap_err().kind(), ErrorKind::Busy);
    assert_eq!(db.get(b"should-not-exist").unwrap(), None);
    let tx = db.transaction();
    let _: Vec<_> = tx
        .iterator(IteratorMode::Start)
        .collect::<Result<_, _>>()
        .unwrap();
    db.put(b"phantom", b"x").unwrap();
    tx.put(b"range-must-abort", b"x").unwrap();
    assert_eq!(tx.commit().unwrap_err().kind(), ErrorKind::Busy);
    let tx = db.transaction();
    tx.put(b"send", b"across threads").unwrap();
    std::thread::scope(|scope| scope.spawn(move || tx.commit().unwrap()).join().unwrap());
    assert_eq!(db.get(b"send").unwrap(), Some(b"across threads".to_vec()));
    drop(db);
    let db = open(&root.0, masstree);
    assert_eq!(db.get(b"a\0").unwrap(), Some(b"replace".to_vec()));
    assert_eq!(db.get(b"a\xff").unwrap(), None);
    assert_eq!(db.get(b"range-must-abort").unwrap(), None);
    assert_eq!(
        db.get_cf(db.cf_handle("other").unwrap(), b"key").unwrap(),
        Some(b"updated".to_vec())
    );
    assert_eq!(db.resolve(id).unwrap(), Outcome::Commit);
}

#[test]
fn bounded_lanes_recover_after_drop_and_invalid_requests() {
    let root = Directory::new();
    let db = open(&root.0, false);
    let first = db.transaction();
    first.get(b"x").unwrap();
    let second = db.transaction();
    second.get(b"x").unwrap();
    assert_eq!(db.get(b"x").unwrap_err().kind(), ErrorKind::Busy);
    drop(first);
    db.put(b"x", b"value").unwrap();
    drop(second);
    let tx = db.transaction();
    tx.put(b"a", b"never").unwrap();
    assert!(tx.put(vec![0; 1025], b"too long").is_err());
    assert!(tx.commit().is_err());
    assert_eq!(db.get(b"a").unwrap(), None);
}
#[test]
fn open_options_and_schema_mismatches_fail_explicitly() {
    let root = Directory::new();
    assert!(DB::open(&Options::default(), &root.0).is_err());
    drop(open(&root.0, false));
    let mut opts = options(false);
    opts.set_error_if_exists(true);
    assert!(DB::open_cf(&opts, &root.0, ["other"]).is_err());
    assert!(DB::open(&options(false), &root.0).is_err());
    let db = open(&root.0, false);
    drop(db);
    let foreign = Directory::new();
    std::fs::write(foreign.0.join("CURRENT"), b"ordinary RocksDB").unwrap();
    assert!(DB::open_default(&foreign.0).is_err());
    assert_eq!(
        std::fs::read(foreign.0.join("CURRENT")).unwrap(),
        b"ordinary RocksDB"
    );
}

fn addresses(n: usize) -> Vec<String> {
    // These ports are reserved from ephemeral connections in the build runner.
    // bind(:0) can otherwise allocate a future listener as an outgoing socket.
    static NEXT: AtomicU64 = AtomicU64::new(41000);
    let mut sockets = Vec::new();
    while sockets.len() < n {
        let port = NEXT.fetch_add(1, Ordering::Relaxed);
        assert!(port < 64000);
        if let Ok(socket) = std::net::TcpListener::bind(("127.0.0.1", port as u16)) {
            sockets.push(socket);
        }
    }
    sockets
        .iter()
        .map(|s| s.local_addr().unwrap().to_string())
        .collect()
}

fn config(
    root: &Path,
    owner: u32,
    peers: &BTreeMap<u32, String>,
    masstree: bool,
    raft: bool,
) -> Config {
    let directory = root.join(format!("node-{owner}"));
    let mut cfg = Config {
        engine: EngineConfig::RocksDb {
            directory: directory.join("data"),
            masstree,
        },
        owner,
        nodes: peers.len() as u32,
        listen: peers[&owner].clone(),
        tables: vec![
            TableSpec {
                id: 1,
                name: "default".into(),
                kind: Kind::Governed,
                warehouse: false,
                initial_owner: 0,
            },
            TableSpec {
                id: 2,
                name: "other".into(),
                kind: Kind::Governed,
                warehouse: false,
                initial_owner: peers.len() as u32 - 1,
            },
        ],
        workers: 2,
        gateway_workers: 1,
        queue_capacity: 64,
        warehouses_per_shard: 0,
        peers: peers.clone(),
        migration: false,
        initial: vec![],
        replication: None,
        persistence: Some(PersistenceConfig {
            directory: directory.clone(),
            create: true,
            sync_writes: true,
        }),
        volatile: false,
        replica_groups: BTreeMap::new(),
    };
    if raft {
        let address = addresses(1).remove(0);
        cfg.persistence = None;
        cfg.replication = Some(ReplicaConfig {
            group: 700 + owner as u64,
            id: 1,
            listen: address.clone(),
            peers: vec![ReplicaPeer { id: 1, address }],
            directory,
            create: true,
            sync_writes: true,
        });
    }
    cfg
}
struct Peer {
    child: Child,
    ready: PathBuf,
}
impl Peer {
    fn spawn(cfg: &Config, root: &Path, mode: &str) -> Self {
        let ready = root.join(format!("ready-{}-{mode}", cfg.owner));
        let _ = std::fs::remove_file(&ready);
        let log =
            std::fs::File::create(root.join(format!("peer-{}-{mode}.log", cfg.owner))).unwrap();
        let child = Command::new(std::env::current_exe().unwrap())
            .args(["peer_child", "--exact", "--nocapture"])
            .env("MAKO_ROCKS_API_CHILD", mode)
            .env("MAKO_ROCKS_API_CONFIG", serde_json::to_string(cfg).unwrap())
            .env("MAKO_ROCKS_API_READY", &ready)
            .stdout(log.try_clone().unwrap())
            .stderr(log)
            .stdin(Stdio::null())
            .spawn()
            .unwrap();
        Self { child, ready }
    }
    fn wait_ready(&mut self) {
        let peer = self;
        let end = Instant::now() + Duration::from_secs(30);
        while !peer.ready.exists() {
            assert!(peer.child.try_wait().unwrap().is_none(), "peer exited");
            assert!(Instant::now() < end, "peer startup deadline");
            std::thread::sleep(Duration::from_millis(10));
        }
    }
    fn start(cfg: &Config, root: &Path, mode: &str) -> Self {
        let mut peer = Self::spawn(cfg, root, mode);
        peer.wait_ready();
        peer
    }
}
impl Drop for Peer {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}
#[test]
fn peer_child() {
    let Ok(mode) = std::env::var("MAKO_ROCKS_API_CHILD") else {
        return;
    };
    let cfg: Config =
        serde_json::from_str(&std::env::var("MAKO_ROCKS_API_CONFIG").unwrap()).unwrap();
    let ready = std::env::var("MAKO_ROCKS_API_READY").unwrap();
    if mode == "facade" {
        let db = DB::open_config(cfg).unwrap();
        let tx = db.transaction();
        tx.put(b"crash", b"committed").unwrap();
        tx.put_cf(db.cf_handle("other").unwrap(), b"other", b"durable")
            .unwrap();
        let id = tx.transaction_id().unwrap();
        tx.commit().unwrap();
        std::fs::write(ready, serde_json::to_vec(&id).unwrap()).unwrap();
        loop {
            std::thread::park();
        }
    }
    let node = RunningNode::start(cfg).unwrap();
    let end = Instant::now() + Duration::from_secs(20);
    while !node.ready() {
        assert!(Instant::now() < end);
        std::thread::sleep(Duration::from_millis(10));
    }
    std::fs::write(ready, b"ready").unwrap();
    loop {
        std::thread::park();
    }
}

#[test]
fn distributed_rocks_transactions_and_remote_conflicts() {
    distributed(false, false);
}
#[test]
#[cfg(feature = "masstree-engine")]
fn distributed_mixed_transactions_and_remote_conflicts() {
    distributed(true, false);
}
#[test]
fn distributed_replicated_transactions() {
    distributed(false, true);
}
fn distributed(masstree: bool, raft: bool) {
    let root = Directory::new();
    let peers: BTreeMap<_, _> = addresses(2)
        .into_iter()
        .enumerate()
        .map(|(i, a)| (i as u32, a))
        .collect();
    let remote = config(&root.0, 1, &peers, false, raft);
    let _peer = Peer::start(&remote, &root.0, "node");
    let db = DB::open_config(config(&root.0, 0, &peers, masstree, raft)).unwrap();
    let cf = db.cf_handle("other").unwrap();
    let tx = db.transaction();
    tx.put(b"local", b"one").unwrap();
    tx.put_cf(cf, b"remote", b"two").unwrap();
    tx.commit().unwrap();
    assert_eq!(db.get_cf(cf, b"remote").unwrap(), Some(b"two".to_vec()));
    let tx = db.transaction();
    tx.get_cf(cf, b"remote").unwrap();
    db.put_cf(cf, b"remote", b"changed").unwrap();
    tx.put(b"local", b"must abort").unwrap();
    assert_eq!(tx.commit().unwrap_err().kind(), ErrorKind::Busy);
    assert_eq!(db.get(b"local").unwrap(), Some(b"one".to_vec()));
    let mut batch = WriteBatch::default();
    batch.put(b"local", b"batch");
    batch.put_cf(cf, b"remote", b"batch");
    db.write(batch).unwrap();
    assert_eq!(db.get_cf(cf, b"remote").unwrap(), Some(b"batch".to_vec()));
}
#[test]
fn local_crash_and_saved_identity_resolution() {
    crash(false, false);
}
#[test]
fn replicated_crash_and_saved_identity_resolution() {
    crash(false, true);
}
#[test]
#[cfg(feature = "masstree-engine")]
fn masstree_crash_reopens_direct_rocks_with_same_api() {
    crash(true, false);
}
fn crash(masstree: bool, raft: bool) {
    let root = Directory::new();
    let peers = BTreeMap::from([(0, addresses(1).remove(0))]);
    let mut cfg = config(&root.0, 0, &peers, masstree, raft);
    let peer = Peer::start(&cfg, &root.0, "facade");
    let id: TransactionId = serde_json::from_slice(&std::fs::read(&peer.ready).unwrap()).unwrap();
    drop(peer);
    cfg.engine = EngineConfig::RocksDb {
        directory: root.0.join("node-0/data"),
        masstree: false,
    };
    if let Some(p) = cfg.persistence.as_mut() {
        p.create = false;
    }
    if let Some(p) = cfg.replication.as_mut() {
        p.create = false;
    }
    let db = DB::open_config(cfg).unwrap();
    assert_eq!(db.resolve(id).unwrap(), Outcome::Commit);
    assert_eq!(db.get(b"crash").unwrap(), Some(b"committed".to_vec()));
    assert_eq!(
        db.get_cf(db.cf_handle("other").unwrap(), b"other").unwrap(),
        Some(b"durable".to_vec())
    );
}

// Migration encodings and policies are already tested independently. This case
// proves that the API follows the runtime's routes for points and range scans.
#[test]
fn migration_routes_points_and_scans_through_the_same_api() {
    use mako_sharding::{
        types::{Status, TxnId},
        wire as control,
    };
    let root = Directory::new();
    let peers: BTreeMap<_, _> = addresses(2)
        .into_iter()
        .enumerate()
        .map(|(i, a)| (i as u32, a))
        .collect();
    let mut remote = config(&root.0, 1, &peers, false, false);
    remote.migration = true;
    let mut peer = Peer::spawn(&remote, &root.0, "node");
    let mut local = config(&root.0, 0, &peers, false, false);
    local.migration = true;
    let db = DB::open_config(local).unwrap();
    peer.wait_ready();
    db.put(b"a", b"stays").unwrap();
    db.put(b"z", b"moves").unwrap();
    let network = Network::default();
    let admin = |operation, payload| match network
        .call(
            &peers[&0],
            Message::Migration {
                owner: 0,
                operation,
                payload,
            },
            Duration::from_secs(10),
        )
        .unwrap()
    {
        Reply::Migration { status, payload } if status == Status::Ok as u32 => {
            control::decode_outcome(&payload).unwrap()
        }
        reply => panic!("admin: {reply:?}"),
    };
    let nonce = TxnId {
        client: 901,
        sequence: 1,
    };
    admin(
        control::ADMIN_BEGIN,
        control::encode_begin(&control::Begin {
            nonce,
            table_name: b"default".to_vec(),
            lo: b"m".to_vec(),
            hi: None,
            source: 0,
            destination: 1,
        })
        .unwrap(),
    );
    let end = Instant::now() + Duration::from_secs(40);
    loop {
        let (_, outcome) = admin(control::ADMIN_POLL, control::encode_nonce(nonce).unwrap());
        if outcome == 1 {
            break;
        }
        assert_eq!(outcome, 0);
        assert!(Instant::now() < end);
        std::thread::sleep(Duration::from_millis(20));
    }
    assert_eq!(db.get(b"z").unwrap(), Some(b"moves".to_vec()));
    let tx = db.transaction();
    tx.put(b"a", b"local").unwrap();
    tx.put(b"z", b"remote").unwrap();
    tx.commit().unwrap();
    let rows: Vec<_> = db
        .iterator(IteratorMode::Start)
        .collect::<Result<_, _>>()
        .unwrap();
    assert_eq!(
        rows,
        [
            (b"a".as_slice().into(), b"local".as_slice().into()),
            (b"z".as_slice().into(), b"remote".as_slice().into())
        ]
    );
    let reverse: Vec<_> = db
        .iterator(IteratorMode::End)
        .collect::<Result<_, _>>()
        .unwrap();
    assert_eq!(reverse, rows.into_iter().rev().collect::<Vec<_>>());
}
