#![cfg(all(feature = "logging", feature = "rocksdb-engine"))]
use mako_runtime::{
    gateway_client::GatewayClient,
    migration::{Kind, TableSpec},
    node::{Config, EngineConfig, PersistenceConfig},
    rpc::Network,
    wire::*,
};
use mako_sharding::{
    types::{Status, TxnId},
    wire as control,
};
use std::{
    collections::BTreeMap,
    time::{Duration, Instant},
};
#[path = "support/node_process.rs"]
mod node_process;
use node_process::Process;

#[test]
fn local_rocks_migration_recovers_checkpointed_ownership_and_lazy_tables() {
    local_case(true, true);
}
#[test]
#[cfg(feature = "masstree-engine")]
fn local_masstree_to_rocks_migration_recovers_without_row_mirror() {
    local_case(false, true);
}
#[test]
#[cfg(feature = "masstree-engine")]
fn local_rocks_to_masstree_migration_recovers_without_row_mirror() {
    local_case(true, false);
}

fn admin(network: &Network, endpoint: &str, operation: u32, payload: Vec<u8>) -> (u64, u32) {
    match network
        .call(
            endpoint,
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
    }
}
fn migrate(
    network: &Network,
    endpoint: &str,
    table: u64,
    warehouse: u32,
    sequence: u64,
    source: u32,
    destination: u32,
) {
    let nonce = TxnId {
        client: 501,
        sequence,
    };
    let begin = control::Begin {
        nonce,
        table_name: if table == 1 {
            b"raw".to_vec()
        } else {
            b"warehouse".to_vec()
        },
        lo: if warehouse == 0 {
            Vec::new()
        } else {
            warehouse.to_be_bytes().to_vec()
        },
        hi: if warehouse == 0 {
            None
        } else {
            Some((warehouse + 1).to_be_bytes().to_vec())
        },
        source,
        destination,
    };
    admin(
        network,
        endpoint,
        control::ADMIN_BEGIN,
        control::encode_begin(&begin).unwrap(),
    );
    let deadline = Instant::now() + Duration::from_secs(40);
    loop {
        let (_, outcome) = admin(
            network,
            endpoint,
            control::ADMIN_POLL,
            control::encode_nonce(nonce).unwrap(),
        );
        if outcome == 1 {
            return;
        }
        assert_eq!(outcome, 0, "migration aborted");
        assert!(Instant::now() < deadline, "migration deadline");
        std::thread::sleep(Duration::from_millis(20));
    }
}
fn local_case(source_rocks: bool, destination_rocks: bool) {
    let root = tempfile::tempdir().unwrap();
    let mut configs = local_configs(root.path(), source_rocks, destination_rocks);
    let peers = configs[0].peers.clone();
    let mut children: Vec<_> = configs.iter().cloned().map(Process::spawn).collect();
    for child in &children {
        child.wait_ready();
    }
    let network = Network::default();
    let mut gateway = GatewayClient::connect(peers[&0].clone(), Duration::from_secs(10)).unwrap();
    let resolve = |gateway: &mut GatewayClient, table, warehouse| {
        gateway
            .resolve(RouteRequest {
                table: Some(table),
                name: None,
                warehouse,
                key: b"a".to_vec(),
            })
            .unwrap()
    };
    for (table, warehouse) in [(1, 0), (2, 1)] {
        let route = resolve(&mut gateway, table, warehouse);
        gateway
            .put(route.clone(), b"a", b"survives migration")
            .unwrap();
        gateway.put(route, b"z", b"delete before copy").unwrap();
        // A complete migration must not resurrect a deleted source row.
        let route = resolve(&mut gateway, table, warehouse);
        gateway.remove(route, b"z").unwrap();
        migrate(&network, &peers[&0], table, warehouse, table, 0, 1);
        assert_eq!(resolve(&mut gateway, table, warehouse).owner, 1);
    }
    for owner in 0..2 {
        match network
            .call(
                &peers[&owner],
                Message::Checkpoint { owner },
                Duration::from_secs(10),
            )
            .unwrap()
        {
            Reply::Checkpoint {
                position: Ok(_), ..
            } => {}
            reply => panic!("checkpoint: {reply:?}"),
        }
    }
    for child in &mut children {
        child.crash();
    }
    drop(children);
    drop(gateway);
    drop(network);
    for config in &mut configs {
        config.persistence.as_mut().unwrap().create = false;
    }
    let mut children: Vec<_> = configs.iter().cloned().map(Process::spawn).collect();
    for child in &children {
        child.wait_ready();
    }
    let network = Network::default();
    let mut gateway = GatewayClient::connect(peers[&0].clone(), Duration::from_secs(10)).unwrap();
    for (table, warehouse) in [(1, 0), (2, 1)] {
        let route = resolve(&mut gateway, table, warehouse);
        assert_eq!(route.owner, 1, "ownership restored");
        reject_stale_read(
            &network,
            &peers[&0],
            Access {
                table: TableKey { table, warehouse },
                route: Some(Route {
                    owner: 0,
                    ownership_version: 0,
                    snapshot_version: 0,
                }),
            },
            ErrorCode::RouteChanged,
        );
        assert_eq!(
            gateway.get(route.clone(), b"a").unwrap().as_deref(),
            Some(b"survives migration".as_slice())
        );
        assert_eq!(gateway.get(route, b"z").unwrap(), None);
        migrate(&network, &peers[&0], table, warehouse, table + 2, 1, 0);
        let route = resolve(&mut gateway, table, warehouse);
        assert_eq!(route.owner, 0);
        assert_eq!(
            gateway.get(route, b"a").unwrap().as_deref(),
            Some(b"survives migration".as_slice())
        );
    }
    for child in &mut children {
        child.stop();
    }
    for owner in 0..2 {
        assert!(!root.path().join(format!("node-{owner}/rows")).exists());
    }
}

#[test]
fn replicated_rocks_migration_recovers_control_after_both_leaders_fail() {
    replicated_case(true, None);
}
#[test]
#[cfg(feature = "masstree-engine")]
fn replicated_mixed_engines_migrate_after_control_leader_failover() {
    replicated_case(false, None);
}
fn replicated_case(source_rocks: bool, pause: Option<&str>) {
    use mako_runtime::node::{ReplicaConfig, ReplicaPeer};
    let root = tempfile::tempdir().unwrap();
    let apps: Vec<_> = (0..6)
        .map(|_| std::net::TcpListener::bind("127.0.0.1:0").unwrap())
        .collect();
    let rafts: Vec<_> = (0..6)
        .map(|_| std::net::TcpListener::bind("127.0.0.1:0").unwrap())
        .collect();
    let endpoints: Vec<_> = apps
        .iter()
        .map(|s| s.local_addr().unwrap().to_string())
        .collect();
    let raft_endpoints: Vec<_> = rafts
        .iter()
        .map(|s| s.local_addr().unwrap().to_string())
        .collect();
    let groups: BTreeMap<_, _> = (0..2)
        .map(|owner| {
            (
                owner,
                ReplicaSet {
                    group: 900 + u64::from(owner),
                    endpoints: endpoints[owner as usize * 3..owner as usize * 3 + 3].to_vec(),
                },
            )
        })
        .collect();
    let peers = BTreeMap::from([(0, endpoints[0].clone()), (1, endpoints[3].clone())]);
    let configs: Vec<_> = (0..6)
        .map(|i| {
            let owner = (i / 3) as u32;
            Config {
                owner,
                nodes: 2,
                listen: endpoints[i].clone(),
                workers: 2,
                gateway_workers: 2,
                queue_capacity: 32,
                warehouses_per_shard: 1,
                peers: peers.clone(),
                migration: true,
                initial: Vec::new(),
                tables: vec![
                    TableSpec {
                        id: 1,
                        name: "raw".into(),
                        kind: Kind::Governed,
                        warehouse: false,
                        initial_owner: 0,
                    },
                    TableSpec {
                        id: 2,
                        name: "warehouse".into(),
                        kind: Kind::Governed,
                        warehouse: true,
                        initial_owner: 0,
                    },
                ],
                persistence: None,
                replica_groups: groups.clone(),
                volatile: false,
                engine: if source_rocks || owner == 1 {
                    EngineConfig::RocksDb {
                        directory: root.path().join(format!("data-{i}")),
                        masstree: false,
                    }
                } else {
                    EngineConfig::Masstree
                },
                replication: Some(ReplicaConfig {
                    group: 900 + u64::from(owner),
                    id: (i % 3 + 1) as u16,
                    listen: raft_endpoints[i].clone(),
                    peers: (i / 3 * 3..i / 3 * 3 + 3)
                        .map(|j| ReplicaPeer {
                            id: (j % 3 + 1) as u16,
                            address: raft_endpoints[j].clone(),
                        })
                        .collect(),
                    directory: root.path().join(format!("node-{i}")),
                    create: true,
                    sync_writes: true,
                }),
            }
        })
        .collect();
    drop(apps);
    drop(rafts);
    let marker = root.path().join("raft-crash-boundary");
    let mut children: Vec<_> = configs
        .into_iter()
        .map(|config| {
            if let Some(point) = pause {
                Process::spawn_with_env(
                    config,
                    &[
                        ("MAKO_TEST_SKIP_PRECOPY", "1"),
                        ("MAKO_TEST_MIGRATION_PAUSE", point),
                        ("MAKO_TEST_MIGRATION_MARKER", marker.to_str().unwrap()),
                    ],
                )
            } else {
                Process::spawn(config)
            }
        })
        .collect();
    let network = Network::default();
    let leader = |owner| {
        let deadline = Instant::now() + Duration::from_secs(30);
        loop {
            if let Ok(endpoint) =
                network.find_leader(owner, &groups[&owner], Duration::from_secs(2))
            {
                return endpoints.iter().position(|e| e == &endpoint).unwrap();
            }
            assert!(Instant::now() < deadline, "leader election deadline");
            std::thread::sleep(Duration::from_millis(20));
        }
    };
    let first = [leader(0), leader(1)];
    for &i in &first {
        children[i].wait_ready();
    }
    let mut gateway =
        GatewayClient::connect(endpoints[first[0]].clone(), Duration::from_secs(10)).unwrap();
    let route = gateway
        .resolve(RouteRequest {
            table: Some(2),
            name: None,
            warehouse: 1,
            key: b"a".to_vec(),
        })
        .unwrap();
    gateway.put(route, b"a", b"replicated migration").unwrap();
    if pause.is_some() {
        let begin = control::Begin {
            nonce: TxnId {
                client: 501,
                sequence: 10,
            },
            table_name: b"warehouse".to_vec(),
            lo: 1u32.to_be_bytes().to_vec(),
            hi: Some(2u32.to_be_bytes().to_vec()),
            source: 0,
            destination: 1,
        };
        admin(
            &network,
            &endpoints[first[0]],
            control::ADMIN_BEGIN,
            control::encode_begin(&begin).unwrap(),
        );
        let deadline = Instant::now() + Duration::from_secs(30);
        while !marker.exists() {
            assert!(Instant::now() < deadline, "Raft crash boundary {pause:?}");
            std::thread::sleep(Duration::from_millis(10));
        }
    } else {
        migrate(&network, &endpoints[first[0]], 2, 1, 10, 0, 1);
    }
    for &i in &first {
        children[i].crash();
    }
    drop(gateway);
    let second = [leader(0), leader(1)];
    assert_ne!(first[0], second[0]);
    assert_ne!(first[1], second[1]);
    for &i in &second {
        children[i].wait_ready();
    }
    let deadline = Instant::now() + Duration::from_secs(30);
    loop {
        let (_, outcome) = admin(
            &network,
            &endpoints[second[0]],
            control::ADMIN_POLL,
            control::encode_nonce(TxnId {
                client: 501,
                sequence: 10,
            })
            .unwrap(),
        );
        if outcome == 1 {
            break;
        }
        assert_eq!(outcome, 0, "recovered migration aborted at {pause:?}");
        assert!(Instant::now() < deadline, "Raft migration recovery stalled");
        std::thread::sleep(Duration::from_millis(20));
    }
    let mut gateway =
        GatewayClient::connect(endpoints[second[0]].clone(), Duration::from_secs(10)).unwrap();
    let route = gateway
        .resolve(RouteRequest {
            table: Some(2),
            name: None,
            warehouse: 1,
            key: b"a".to_vec(),
        })
        .unwrap();
    assert_eq!(route.owner, 1);
    assert_eq!(
        gateway.get(route, b"a").unwrap().as_deref(),
        Some(b"replicated migration".as_slice())
    );
    migrate(&network, &endpoints[second[0]], 2, 1, 11, 1, 0);
    let route = gateway
        .resolve(RouteRequest {
            table: Some(2),
            name: None,
            warehouse: 1,
            key: b"a".to_vec(),
        })
        .unwrap();
    assert_eq!(route.owner, 0);
    assert_eq!(
        gateway.get(route, b"a").unwrap().as_deref(),
        Some(b"replicated migration".as_slice())
    );
    for (i, child) in children.iter_mut().enumerate() {
        if !first.contains(&i) {
            child.stop();
        }
    }
}

fn local_configs(
    root: &std::path::Path,
    source_rocks: bool,
    destination_rocks: bool,
) -> Vec<Config> {
    let sockets: Vec<_> = (0..2)
        .map(|_| std::net::TcpListener::bind("127.0.0.1:0").unwrap())
        .collect();
    let peers: BTreeMap<_, _> = sockets
        .iter()
        .enumerate()
        .map(|(owner, s)| (owner as u32, s.local_addr().unwrap().to_string()))
        .collect();
    let tables = vec![
        TableSpec {
            id: 1,
            name: "raw".into(),
            kind: Kind::Governed,
            warehouse: false,
            initial_owner: 0,
        },
        TableSpec {
            id: 2,
            name: "warehouse".into(),
            kind: Kind::Governed,
            warehouse: true,
            initial_owner: 0,
        },
    ];
    let configs: Vec<_> = (0..2)
        .map(|owner| Config {
            owner,
            nodes: 2,
            listen: peers[&owner].clone(),
            workers: 2,
            gateway_workers: 2,
            queue_capacity: 32,
            warehouses_per_shard: 1,
            peers: peers.clone(),
            migration: true,
            initial: Vec::new(),
            tables: tables.clone(),
            replication: None,
            replica_groups: BTreeMap::new(),
            volatile: false,
            engine: if if owner == 0 {
                source_rocks
            } else {
                destination_rocks
            } {
                EngineConfig::RocksDb {
                    directory: root.join(format!("data-{owner}")),
                    masstree: false,
                }
            } else {
                EngineConfig::Masstree
            },
            persistence: Some(PersistenceConfig {
                directory: root.join(format!("node-{owner}")),
                create: true,
                sync_writes: true,
            }),
        })
        .collect();
    drop(sockets);
    configs
}

#[test]
#[cfg(feature = "test-hooks")]
fn local_rocks_migration_recovers_interrupted_prepare_copy_decision_and_cleanup() {
    crash_boundaries(true);
}
#[test]
#[cfg(all(feature = "test-hooks", feature = "masstree-engine"))]
fn local_mixed_migration_recovers_interrupted_prepare_copy_decision_and_cleanup() {
    crash_boundaries(false);
}
#[cfg(feature = "test-hooks")]
fn crash_boundaries(source_rocks: bool) {
    for (point, paused_owner) in [
        ("row-prepared", 1),
        ("freeze-published", 0),
        ("final-copy-row", 1),
        ("decision-committed", 0),
        ("cleanup-row", 0),
    ] {
        eprintln!("durable migration crash boundary: {point}");
        let root = tempfile::tempdir().unwrap();
        let mut configs = local_configs(root.path(), source_rocks, true);
        let peers = configs[0].peers.clone();
        let marker = root.path().join("crash-boundary");
        let mut children: Vec<_> = configs
            .iter()
            .cloned()
            .map(|config| {
                if config.owner == paused_owner {
                    Process::spawn_with_env(
                        config,
                        &[
                            ("MAKO_TEST_SKIP_PRECOPY", "1"),
                            ("MAKO_TEST_MIGRATION_PAUSE", point),
                            ("MAKO_TEST_MIGRATION_MARKER", marker.to_str().unwrap()),
                        ],
                    )
                } else {
                    Process::spawn_with_env(config, &[("MAKO_TEST_SKIP_PRECOPY", "1")])
                }
            })
            .collect();
        for child in &children {
            child.wait_ready();
        }
        let network = Network::default();
        let mut gateway =
            GatewayClient::connect(peers[&0].clone(), Duration::from_secs(10)).unwrap();
        let route = gateway
            .resolve(RouteRequest {
                table: Some(1),
                name: None,
                warehouse: 0,
                key: b"a".to_vec(),
            })
            .unwrap();
        for key in [b"a", b"b", b"c"] {
            gateway.put(route.clone(), key, key).unwrap();
        }
        let nonce = TxnId {
            client: 503,
            sequence: 1,
        };
        let begin = control::Begin {
            nonce,
            table_name: b"raw".to_vec(),
            lo: Vec::new(),
            hi: None,
            source: 0,
            destination: 1,
        };
        admin(
            &network,
            &peers[&0],
            control::ADMIN_BEGIN,
            control::encode_begin(&begin).unwrap(),
        );
        let deadline = Instant::now() + Duration::from_secs(20);
        while !marker.exists() {
            assert!(Instant::now() < deadline, "missing crash boundary {point}");
            std::thread::sleep(Duration::from_millis(10));
        }
        for child in &mut children {
            child.crash();
        }
        drop(children);
        drop(gateway);
        drop(network);
        for config in &mut configs {
            config.persistence.as_mut().unwrap().create = false;
        }
        let mut children: Vec<_> = configs.iter().cloned().map(Process::spawn).collect();
        for child in &children {
            child.wait_ready();
        }
        let network = Network::default();
        let deadline = Instant::now() + Duration::from_secs(30);
        let outcome = loop {
            let (_, outcome) = admin(
                &network,
                &peers[&0],
                control::ADMIN_POLL,
                control::encode_nonce(nonce).unwrap(),
            );
            if outcome != 0 {
                break outcome;
            }
            assert!(Instant::now() < deadline, "recovery stalled at {point}");
            std::thread::sleep(Duration::from_millis(20));
        };
        if matches!(point, "decision-committed" | "cleanup-row") {
            assert_eq!(outcome, 1, "saved Commit must survive {point}");
        }
        let mut gateway =
            GatewayClient::connect(peers[&0].clone(), Duration::from_secs(10)).unwrap();
        let route = gateway
            .resolve(RouteRequest {
                table: Some(1),
                name: None,
                warehouse: 0,
                key: b"a".to_vec(),
            })
            .unwrap();
        assert_eq!(route.owner, if outcome == 1 { 1 } else { 0 });
        for key in [b"a", b"b", b"c"] {
            assert_eq!(
                gateway.get(route.clone(), key).unwrap().as_deref(),
                Some(key.as_slice()),
                "{point}"
            );
        }
        // A fresh generation can move the exact recovered image again.
        migrate(&network, &peers[&0], 1, 0, 25, route.owner, 1 - route.owner);
        for child in &mut children {
            child.stop();
        }
    }
}

#[test]
#[cfg(feature = "test-hooks")]
fn replicated_rocks_migration_resumes_copy_and_saved_commit_after_leader_crashes() {
    for point in ["final-copy-row", "decision-committed", "cleanup-row"] {
        replicated_case(true, Some(point));
    }
}

fn reject_stale_read(network: &Network, endpoint: &str, access: Access, expected: ErrorCode) {
    let stream = Stream { node: 1, worker: 1 };
    let lease = match network
        .call(
            endpoint,
            Message::OpenStream {
                stream,
                nonce: 700 + access.table.table,
            },
            Duration::from_secs(5),
        )
        .unwrap()
    {
        Reply::OpenStream(Ok(lease)) => lease,
        reply => panic!("claim stale-route test: {reply:?}"),
    };
    let mut identity = Identity {
        incarnation: lease,
        attempt: 1,
        sequence: 1,
        term: 0,
    };
    let reply = network
        .call(
            endpoint,
            Message::Participant(Request {
                stream,
                identity,
                action: Action::Get {
                    access,
                    key: b"a".to_vec(),
                },
            }),
            Duration::from_secs(5),
        )
        .unwrap();
    match reply {
        Reply::Participant(reply) => assert_eq!(reply.result.unwrap_err().code, expected),
        reply => panic!("stale read: {reply:?}"),
    }
    identity.sequence += 1;
    let reply = network
        .call(
            endpoint,
            Message::Participant(Request {
                stream,
                identity,
                action: Action::Abort,
            }),
            Duration::from_secs(5),
        )
        .unwrap();
    assert!(
        matches!(reply, Reply::Participant(Response { result: Ok(_), .. })),
        "release stale-route attempt: {reply:?}"
    );
}
