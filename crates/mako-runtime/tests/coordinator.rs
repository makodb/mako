#![cfg(feature = "native-storage")]
use mako_runtime::{
    client::{Client, Target},
    receiver::RunningShard,
    rpc::Network,
    storage::{Store, VolatileLog},
    wire::*,
};
use std::{collections::BTreeMap, sync::Arc, time::Duration};

fn address() -> String {
    std::net::TcpListener::bind("127.0.0.1:0")
        .unwrap()
        .local_addr()
        .unwrap()
        .to_string()
}
fn target(owner: u32) -> Target {
    Target {
        owner,
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
fn store(owner: u32) -> Arc<Store> {
    let store = Store::new(owner, false, Arc::new(VolatileLog)).unwrap();
    let mut loader = store.database.worker().unwrap();
    store
        .open(&mut loader, target(owner).access.table, 1)
        .unwrap();
    store
}

#[test]
fn native_coordinator_commits_across_shards_and_releases_remote_body_errors() {
    let local = store(0);
    let remote = store(1);
    let network = Arc::new(Network::default());
    let endpoint = address();
    let _receiver =
        RunningShard::start(remote.clone(), network.clone(), &endpoint, 2, 1, 8).unwrap();
    let mut client = Client::new(
        network,
        BTreeMap::from([(1, endpoint)]),
        Stream { node: 0, worker: 0 },
        0,
        Duration::from_secs(5),
    )
    .unwrap();
    let mut worker = local.database.worker().unwrap();
    client
        .run(local.clone(), &mut worker, |tx| {
            tx.mutate(&target(0), Mutation::Put, b"a", b"local")?;
            tx.mutate(&target(1), Mutation::Put, b"b", b"remote")?;
            assert_eq!(
                tx.get(&target(1), b"b")?.as_deref(),
                Some(b"remote".as_slice())
            );
            Ok(())
        })
        .unwrap();
    client
        .run(local.clone(), &mut worker, |tx| {
            assert_eq!(
                tx.get(&target(0), b"a")?.as_deref(),
                Some(b"local".as_slice())
            );
            assert_eq!(
                tx.get(&target(1), b"b")?.as_deref(),
                Some(b"remote".as_slice())
            );
            let rows = tx.scan(&target(1), Bound::Unbounded, Bound::Unbounded, false, 8)?;
            assert_eq!(rows[0].key, b"b");
            Ok(())
        })
        .unwrap();
    let result: Result<()> = client.run(local.clone(), &mut worker, |tx| {
        tx.mutate(&target(0), Mutation::Put, b"a", b"aborted")?;
        tx.mutate(&target(1), Mutation::Put, b"b", b"aborted")?;
        tx.get(&target(1), b"b")?; // Flush and retain real remote write locks.
        Err(Error::new(ErrorCode::Invalid, "application rejected body"))
    });
    assert_eq!(result.unwrap_err().code, ErrorCode::Invalid);
    let cut = remote.epochs.cut().unwrap();
    assert!(remote.epochs.drained(cut).unwrap());
    client
        .run(local, &mut worker, |tx| {
            assert_eq!(
                tx.get(&target(0), b"a")?.as_deref(),
                Some(b"local".as_slice())
            );
            assert_eq!(
                tx.get(&target(1), b"b")?.as_deref(),
                Some(b"remote".as_slice())
            );
            Ok(())
        })
        .unwrap();
}

#[test]
fn ignoring_a_remote_operation_error_cannot_commit_a_partial_local_body() {
    let local = store(0);
    let remote = store(1);
    let network = Arc::new(Network::default());
    let endpoint = address();
    let _receiver = RunningShard::start(remote, network.clone(), &endpoint, 2, 1, 8).unwrap();
    let mut client = Client::new(
        network,
        BTreeMap::from([(1, endpoint)]),
        Stream { node: 0, worker: 0 },
        0,
        Duration::from_secs(5),
    )
    .unwrap();
    let mut worker = local.database.worker().unwrap();
    let mut tx = client.begin(local.clone(), &mut worker).unwrap();
    tx.mutate(&target(0), Mutation::Put, b"key", b"must abort")
        .unwrap();
    let mut missing = target(1);
    missing.access.table.table = 999;
    assert!(tx.get(&missing, b"key").is_err());
    assert!(tx.commit().is_err());
    client
        .run(local, &mut worker, |tx| {
            assert_eq!(tx.get(&target(0), b"key")?, None);
            Ok(())
        })
        .unwrap();
}

#[test]
fn failed_abort_is_reported_as_unknown_and_fences_the_client_immediately() {
    use mako_runtime::rpc::RpcServer;
    let local = store(0);
    let network = Arc::new(Network::default());
    let server = RpcServer::start(&network, "127.0.0.1:0", |message, respond| match message {
        Message::OpenStream { .. } => respond(Reply::OpenStream(Ok(400))),
        Message::Participant(request) => {
            let result = match request.action {
                Action::Get { .. } => Ok(Value::Point {
                    value: Some(b"read".to_vec()),
                    time_term: 10,
                }),
                Action::Timestamp => Ok(Value::Timestamp(10)),
                Action::Validate | Action::Abort => {
                    Err(Error::new(ErrorCode::Conflict, "injected phase rejection"))
                }
                _ => Err(Error::new(ErrorCode::Invalid, "unexpected action")),
            };
            respond(Reply::Participant(Response {
                identity: request.identity,
                result,
            }));
        }
        _ => respond(Reply::Error(Error::new(
            ErrorCode::Invalid,
            "unexpected message",
        ))),
    })
    .unwrap();
    let mut client = Client::new(
        network,
        BTreeMap::from([(1, server.address().into())]),
        Stream { node: 0, worker: 0 },
        0,
        Duration::from_secs(5),
    )
    .unwrap();
    let mut worker = local.database.worker().unwrap();
    let result = client.run(local.clone(), &mut worker, |tx| {
        assert!(tx.get(&target(1), b"key")?.is_some());
        Ok(())
    });
    assert_eq!(result.unwrap_err().code, ErrorCode::Unknown);
    assert_eq!(
        client
            .run(local.clone(), &mut worker, |_| Ok(()))
            .unwrap_err()
            .code,
        ErrorCode::Unknown
    );
    let cut = local.epochs.cut().unwrap();
    assert!(!local.epochs.drained(cut).unwrap());
}

#[test]
fn global_lock_barrier_rejects_point_absence_and_scan_write_skew() {
    for mode in ["point", "absence", "scan"] {
        let stores = [store(0), store(1)];
        let network = Arc::new(Network::default());
        let servers: Vec<_> = stores
            .iter()
            .map(|store| {
                RunningShard::start(store.clone(), network.clone(), "127.0.0.1:0", 2, 2, 16)
                    .unwrap()
            })
            .collect();
        let endpoints = BTreeMap::from([
            (0, servers[0].address().to_owned()),
            (1, servers[1].address().to_owned()),
        ]);
        let mut loader = stores[0].database.worker().unwrap();
        let mut initializer = Client::new(
            network.clone(),
            endpoints.clone(),
            Stream { node: 0, worker: 1 },
            0,
            Duration::from_secs(5),
        )
        .unwrap();
        initializer
            .run(stores[0].clone(), &mut loader, |tx| {
                if mode == "point" {
                    tx.mutate(&target(0), Mutation::Put, b"x", b"0")?;
                }
                tx.mutate(&target(1), Mutation::Put, b"y", b"0")
            })
            .unwrap();
        let barrier = Arc::new(std::sync::Barrier::new(2));
        let mut threads = Vec::new();
        for owner in 0..2u32 {
            let local = stores[owner as usize].clone();
            let network = network.clone();
            let endpoints = endpoints.clone();
            let barrier = barrier.clone();
            threads.push(std::thread::spawn(move || {
                let mut worker = local.database.worker().unwrap();
                let mut client = Client::new(
                    network,
                    endpoints,
                    Stream {
                        node: owner,
                        worker: 0,
                    },
                    0,
                    Duration::from_secs(5),
                )
                .unwrap();
                let mut tx = client.begin(local, &mut worker).unwrap();
                if owner == 0 {
                    if mode == "scan" {
                        assert!(tx
                            .scan(
                                &target(0),
                                Bound::Included(b"a".to_vec()),
                                Bound::Excluded(b"z".to_vec()),
                                false,
                                8
                            )
                            .unwrap()
                            .is_empty());
                    } else {
                        assert_eq!(
                            tx.get(&target(0), b"x").unwrap().as_deref(),
                            if mode == "point" {
                                Some(b"0".as_slice())
                            } else {
                                None
                            }
                        );
                    }
                    tx.mutate(&target(1), Mutation::Put, b"y", b"1").unwrap();
                } else {
                    assert_eq!(
                        tx.get(&target(1), b"y").unwrap().as_deref(),
                        Some(b"0".as_slice())
                    );
                    tx.mutate(&target(0), Mutation::Put, b"x", b"1").unwrap();
                }
                barrier.wait();
                match tx.commit() {
                    Ok(()) => true,
                    Err(error) => {
                        assert_eq!(error.code, ErrorCode::Conflict, "{mode}: {error}");
                        false
                    }
                }
            }));
        }
        let commits = threads
            .into_iter()
            .map(|thread| usize::from(thread.join().unwrap()))
            .sum::<usize>();
        assert!(commits < 2, "{mode} admitted a serialization cycle");
        initializer
            .run(stores[0].clone(), &mut loader, |tx| {
                tx.mutate(&target(0), Mutation::Put, b"x", b"after conflict")?;
                tx.mutate(&target(1), Mutation::Put, b"y", b"after conflict")
            })
            .unwrap();
    }
}

#[test]
fn a_busy_discovery_health_snapshot_does_not_refuse_an_authoritative_completion() {
    struct Log;
    impl mako_runtime::storage::CommitLog for Log {
        fn append(&self, _: &[u8], _: u32) -> Result<()> {
            panic!("obsolete row log");
        }
        fn replica_status(&self, owner: u32) -> ReplicaStatus {
            ReplicaStatus {
                owner,
                group: None,
                leader: true,
                term: 0,
                healthy: false,
                durable_receipts: false,
            }
        }
        // The blocking health/admission checks remain successful. A nonblocking
        // discovery probe can report false simply because apply owns its mutex.
    }
    let local = Store::new(0, false, Arc::new(Log)).unwrap();
    let mut worker = local.database.worker().unwrap();
    local.open(&mut worker, target(0).access.table, 1).unwrap();
    let mut client = Client::new(
        Arc::new(Network::default()),
        BTreeMap::new(),
        Stream { node: 0, worker: 0 },
        0,
        Duration::from_secs(5),
    )
    .unwrap();
    client
        .run(local.clone(), &mut worker, |tx| {
            tx.mutate(&target(0), Mutation::Put, b"key", b"committed")
        })
        .unwrap();
    client
        .run(local, &mut worker, |tx| {
            assert_eq!(
                tx.get(&target(0), b"key")?.as_deref(),
                Some(b"committed".as_slice())
            );
            Ok(())
        })
        .unwrap();
}
