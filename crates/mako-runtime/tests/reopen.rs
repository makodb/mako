#![cfg(all(feature = "logging", feature = "rocksdb-engine"))]
use mako_runtime::{
    gateway_client::GatewayClient,
    migration::{Kind, TableSpec},
    node::{Config, EngineConfig, PersistenceConfig},
    rpc::Network,
    wire::*,
};
use std::{collections::BTreeMap, time::Duration};
#[path = "support/node_process.rs"]
mod node_process;
use node_process::Process;
#[test]
fn reopened_node_serves_checkpoint_suffix_and_accepts_new_transactions() {
    reopen_case(false, false, false);
}
#[test]
fn reopened_raft_node_preserves_distinct_shard_and_replica_group_identities() {
    reopen_case(true, false, false);
}
#[test]
fn reopened_migration_node_recovers_its_control_namespace() {
    reopen_case(false, true, false);
}
#[cfg(feature = "masstree-engine")]
#[test]
fn local_node_toggles_masstree_over_the_same_files() {
    reopen_case(false, false, true);
}
#[cfg(feature = "masstree-engine")]
#[test]
fn raft_node_toggles_masstree_over_the_same_files() {
    reopen_case(true, false, true);
}
#[cfg(feature = "masstree-engine")]
#[test]
fn migration_node_toggles_masstree_over_the_same_control_state() {
    reopen_case(false, true, true);
}
fn reopen_case(replicated: bool, migration: bool, toggle: bool) {
    let root = tempfile::tempdir().unwrap();
    let source = root.path().join("source");
    let mut config = Config {
        engine: EngineConfig::RocksDb {
            directory: source.join("data"),
            masstree: false,
        },
        owner: 0,
        nodes: 1,
        listen: "127.0.0.1:0".into(),
        tables: vec![TableSpec {
            id: 1,
            name: "rows".into(),
            kind: Kind::Governed,
            warehouse: false,
            initial_owner: 0,
        }],
        workers: 1,
        gateway_workers: 1,
        queue_capacity: 16,
        warehouses_per_shard: 0,
        peers: BTreeMap::new(),
        migration,
        initial: Vec::new(),
        replication: None,
        persistence: Some(PersistenceConfig {
            directory: source,
            create: true,
            sync_writes: true,
        }),
        volatile: false,
        replica_groups: BTreeMap::new(),
    };
    if replicated {
        let listener = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        let address = listener.local_addr().unwrap().to_string();
        drop(listener);
        config.replication = Some(mako_runtime::node::ReplicaConfig {
            group: 77,
            id: 7,
            listen: address.clone(),
            peers: vec![mako_runtime::node::ReplicaPeer { id: 7, address }],
            directory: config.persistence.take().unwrap().directory,
            create: true,
            sync_writes: true,
        });
    }
    let timeout = Duration::from_secs(10);
    let mut child = Process::start(config.clone());
    let mut client = GatewayClient::connect(child.address.clone(), timeout).unwrap();
    let target = client
        .resolve(RouteRequest {
            table: Some(1),
            name: None,
            warehouse: 0,
            key: b"key".to_vec(),
        })
        .unwrap();
    client.put(target.clone(), b"key", b"checkpoint").unwrap();
    assert!(matches!(
        Network::default()
            .call(&child.address, Message::Checkpoint { owner: 0 }, timeout)
            .unwrap(),
        Reply::Checkpoint { .. }
    ));
    client.put(target.clone(), b"key", b"suffix").unwrap();
    child.stop();
    drop(client);
    drop(child);
    let engines = if cfg!(feature = "masstree-engine") {
        vec!["masstree", "rocksdb"]
    } else {
        vec!["rocksdb"]
    };
    for engine in engines {
        let EngineConfig::RocksDb { masstree, .. } = &mut config.engine else {
            unreachable!()
        };
        *masstree = toggle && engine == "masstree";
        if let Some(c) = &mut config.persistence {
            c.create = false;
        }
        if let Some(c) = &mut config.replication {
            c.create = false;
        }
        assert!(config.persistence.as_ref().is_none_or(|c| !c.create));
        assert!(config.replication.as_ref().is_none_or(|c| !c.create));
        let mut child = Process::start(config.clone());
        let mut client = GatewayClient::connect(child.address.clone(), timeout).unwrap();
        let target = client
            .resolve(RouteRequest {
                table: Some(1),
                name: None,
                warehouse: 0,
                key: b"key".to_vec(),
            })
            .unwrap();
        assert_eq!(
            client.get(target.clone(), b"key").unwrap().as_deref(),
            Some(b"suffix".as_slice())
        );
        client
            .put(target.clone(), b"after-reopen", b"new commit")
            .unwrap();
        assert_eq!(
            client.get(target, b"after-reopen").unwrap().as_deref(),
            Some(b"new commit".as_slice())
        );
        if toggle {
            child.crash();
        } else {
            child.stop();
        }
    }
}
