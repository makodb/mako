#![cfg(feature = "replication")]
use mako_runtime::{
    client::Client,
    durable::DurableRows,
    migration::{Kind, Schema, TableSpec},
    replication::ReplicatedLog,
    rpc::Network,
    storage::Store,
    wire::*,
};
use raft_host::{Config, Peer};
use std::{
    collections::BTreeMap,
    path::Path,
    sync::Arc,
    time::{Duration, Instant},
};

struct Replica {
    log: Arc<ReplicatedLog>,
    store: Arc<Store>,
}
impl Replica {
    fn start(root: &Path, config: Config) -> Self {
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
        let log = ReplicatedLog::new(Duration::from_secs(5)).unwrap();
        let store = Store::new(0, false, log.clone()).unwrap();
        {
            let mut loader = store.database.worker().unwrap();
            store.open(&mut loader, target().access.table, 1).unwrap();
        }
        let durable = DurableRows::open(root, config.group, true, b"one table").unwrap();
        log.start(config, &store, schema, durable).unwrap();
        Self { log, store }
    }
    fn client(&self) -> Client {
        Client::new(
            Arc::new(Network::default()),
            BTreeMap::new(),
            Stream { node: 0, worker: 0 },
            0,
            Duration::from_secs(5),
        )
        .unwrap()
    }
    fn put(&self, key: &[u8], value: &[u8]) {
        let mut worker = self.store.database.worker().unwrap();
        self.client()
            .run(self.store.clone(), &mut worker, |tx| {
                tx.mutate(&target(), Mutation::Put, key, value)?;
                Ok(())
            })
            .unwrap();
    }
    fn read(&self, key: &[u8]) -> Option<Vec<u8>> {
        let mut worker = self.store.database.worker().unwrap();
        self.client()
            .run(self.store.clone(), &mut worker, |tx| tx.get(&target(), key))
            .unwrap()
    }
}
impl Drop for Replica {
    fn drop(&mut self) {
        self.log.shutdown();
    }
}
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
fn wait(mut condition: impl FnMut() -> bool) {
    let deadline = Instant::now() + Duration::from_secs(20);
    while !condition() {
        assert!(Instant::now() < deadline, "replica progress deadline");
        std::thread::sleep(Duration::from_millis(10));
    }
}
fn config(root: &Path, id: u16, peers: &[Peer], create: bool) -> Config {
    Config {
        group: 37,
        node: id,
        peers: peers.to_vec(),
        listen: peers[id as usize - 1].address.clone(),
        directory: root.join(format!("raft-{id}")),
        create,
        election_timeout: Duration::from_millis(400),
        heartbeat_interval: Duration::from_millis(30),
        rpc_timeout: Duration::from_millis(200),
        ..Config::default()
    }
}
fn peers(n: u16) -> Vec<Peer> {
    let sockets: Vec<_> = (0..n)
        .map(|_| std::net::TcpListener::bind("127.0.0.1:0").unwrap())
        .collect();
    sockets
        .iter()
        .enumerate()
        .map(|(index, socket)| Peer {
            id: index as u16 + 1,
            address: socket.local_addr().unwrap().to_string(),
        })
        .collect()
}

#[test]
fn native_raft_commit_checkpoint_and_restart_rebuild_masstree() {
    let root = tempfile::tempdir().unwrap();
    let peers = peers(1);
    let replica = Replica::start(
        &root.path().join("rows"),
        config(root.path(), 1, &peers, true),
    );
    wait(|| replica.log.host().unwrap().status().is_leader);
    replica.put(b"a", b"committed");
    replica.put(b"deleted", b"temporary");
    {
        let mut worker = replica.store.database.worker().unwrap();
        replica
            .client()
            .run(replica.store.clone(), &mut worker, |tx| {
                tx.mutate(&target(), Mutation::Remove, b"deleted", b"")?;
                Ok(())
            })
            .unwrap();
    }
    let checkpoint = replica.log.host().unwrap().checkpoint().unwrap();
    assert!(checkpoint.index >= 4);
    assert_eq!(replica.read(b"a").as_deref(), Some(b"committed".as_slice()));
    drop(replica);
    let recovered = Replica::start(
        &root.path().join("rows"),
        config(root.path(), 1, &peers, false),
    );
    wait(|| recovered.log.host().unwrap().status().is_leader);
    assert_eq!(
        recovered.read(b"a").as_deref(),
        Some(b"committed".as_slice())
    );
    assert_eq!(recovered.read(b"deleted"), None);
    recovered.put(b"a", b"after restart");
    assert_eq!(
        recovered.read(b"a").as_deref(),
        Some(b"after restart".as_slice())
    );
}

#[test]
fn native_replicas_fail_over_and_install_a_checkpoint_for_a_missing_peer() {
    let root = tempfile::tempdir().unwrap();
    let peers = peers(3);
    let mut replicas: Vec<_> = (1..=3)
        .map(|id| {
            Some(Replica::start(
                &root.path().join(format!("rows-{id}")),
                config(root.path(), id, &peers, true),
            ))
        })
        .collect();
    let leader = |replicas: &[Option<Replica>]| {
        replicas.iter().position(|replica| {
            replica
                .as_ref()
                .is_some_and(|replica| replica.log.host().unwrap().status().is_leader)
        })
    };
    wait(|| leader(&replicas).is_some());
    let first = leader(&replicas).unwrap();
    replicas[first]
        .as_ref()
        .unwrap()
        .put(b"a", b"before failure");
    let position = replicas[first]
        .as_ref()
        .unwrap()
        .log
        .host()
        .unwrap()
        .status()
        .applied
        .index;
    wait(|| {
        replicas.iter().all(|replica| {
            replica
                .as_ref()
                .unwrap()
                .log
                .host()
                .unwrap()
                .status()
                .applied
                .index
                >= position
        })
    });
    let offline = (first + 1) % 3;
    drop(replicas[offline].take());
    replicas[first]
        .as_ref()
        .unwrap()
        .put(b"b", b"checkpoint state");
    let checkpoint = replicas[first]
        .as_ref()
        .unwrap()
        .log
        .host()
        .unwrap()
        .checkpoint()
        .unwrap();
    let id = offline as u16 + 1;
    replicas[offline] = Some(Replica::start(
        &root.path().join(format!("rows-{id}")),
        config(root.path(), id, &peers, false),
    ));
    wait(|| {
        replicas[offline]
            .as_ref()
            .unwrap()
            .log
            .host()
            .unwrap()
            .status()
            .applied
            .index
            >= checkpoint.index
    });
    // Followers are never admitted to execute transactions against a stale tree.
    assert!(replicas[offline].as_ref().unwrap().store.protect().is_err());
    drop(replicas[first].take());
    wait(|| leader(&replicas).is_some());
    let next = leader(&replicas).unwrap();
    assert_ne!(first, next);
    let promoted = replicas[next].as_ref().unwrap();
    assert_eq!(
        promoted.read(b"a").as_deref(),
        Some(b"before failure".as_slice())
    );
    assert_eq!(
        promoted.read(b"b").as_deref(),
        Some(b"checkpoint state".as_slice())
    );
    promoted.put(b"a", b"after failover");
    assert_eq!(
        promoted.read(b"a").as_deref(),
        Some(b"after failover".as_slice())
    );
}

#[test]
fn lost_quorum_after_install_is_unknown_and_fences_every_new_transaction() {
    let root = tempfile::tempdir().unwrap();
    let peers = peers(3);
    let mut replicas: Vec<_> = (1..=3)
        .map(|id| {
            Some(Replica::start(
                &root.path().join(format!("rows-{id}")),
                config(root.path(), id, &peers, true),
            ))
        })
        .collect();
    wait(|| {
        replicas.iter().any(|replica| {
            replica
                .as_ref()
                .unwrap()
                .log
                .host()
                .unwrap()
                .status()
                .is_leader
        })
    });
    let leader = replicas
        .iter()
        .position(|replica| {
            replica
                .as_ref()
                .unwrap()
                .log
                .host()
                .unwrap()
                .status()
                .is_leader
        })
        .unwrap();
    let store = replicas[leader].as_ref().unwrap().store.clone();
    let log = replicas[leader].as_ref().unwrap().log.clone();
    let mut client = replicas[leader].as_ref().unwrap().client();
    let mut worker = store.database.worker().unwrap();
    let mut tx = client.begin(store.clone(), &mut worker).unwrap();
    tx.mutate(&target(), Mutation::Put, b"uncertain", b"installed")
        .unwrap();
    for (index, replica) in replicas.iter_mut().enumerate() {
        if index != leader {
            drop(replica.take());
        }
    }
    assert_eq!(tx.commit().unwrap_err().code, ErrorCode::Unknown);
    assert!(log.health().is_err());
    assert!(matches!(store.protect(), Err(error) if error.code == ErrorCode::Unknown));
    assert!(client.begin(store.clone(), &mut worker).is_err());
    drop(worker);
}
