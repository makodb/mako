use mako_runtime::{
    client::Client,
    log::{LocalLog, ReplicatedLog},
    migration::{Kind, Schema, TableSpec},
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
        let store = Store::with_engine(0, false, log.clone(), super::database(root, 0)).unwrap();
        {
            let mut loader = store.database.worker().unwrap();
            store.open(&mut loader, target().access.table, 1).unwrap();
        }
        log.start_engine(config, &store, schema, b"one table".to_vec())
            .unwrap();
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
fn resolve_gateway<T>(mut operation: impl FnMut() -> Result<T>) -> T {
    let deadline = Instant::now() + Duration::from_secs(20);
    loop {
        match operation() {
            Ok(value) => return value,
            Err(error)
                if matches!(
                    error.code,
                    ErrorCode::RouteChanged | ErrorCode::Unknown | ErrorCode::Transport
                ) =>
            {
                // Discovery and forwarding can lag a local leader election.
                // The caller retains its client and exact intent, so unresolved
                // requests keep their identity. Terminal aborts still fail.
                assert!(
                    Instant::now() < deadline,
                    "gateway progress deadline: {error:?}"
                );
                std::thread::sleep(Duration::from_millis(10));
            }
            Err(error) => panic!("gateway request failed: {error:?}"),
        }
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
fn native_raft_commit_checkpoint_and_restart_recover_engine() {
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
fn lost_quorum_before_prepare_never_installs_and_rejects_new_admission() {
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
    assert!(matches!(
        tx.commit().unwrap_err().code,
        ErrorCode::RouteChanged | ErrorCode::Unknown
    ));
    assert!(store.protect().is_err());
    // Quorum loss before prepare needs no post-install fault: no Commit was
    // proposed, and the retained leadership permit now refuses admission.
    assert!(!log.host().unwrap().status().is_leader || log.health().is_ok());
    assert!(client.begin(store.clone(), &mut worker).is_err());
    drop(worker);
}

#[test]
fn clients_discover_new_leaders_and_recover_durable_gateway_receipts() {
    use mako_runtime::{gateway, gateway_client::GatewayClient, receiver::RunningShard};
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
    let network = Arc::new(Network::default());
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
    let mut servers: Vec<_> = replicas
        .iter()
        .map(|replica| {
            Some(
                RunningShard::start_with_gateway(
                    replica.as_ref().unwrap().store.clone(),
                    network.clone(),
                    "127.0.0.1:0",
                    2,
                    1,
                    16,
                    gateway::Config {
                        replicas: BTreeMap::new(),
                        schema: schema.clone(),
                        endpoints: BTreeMap::new(),
                        workers: 1,
                    },
                )
                .unwrap(),
            )
        })
        .collect();
    let group = ReplicaSet {
        group: 37,
        endpoints: servers
            .iter()
            .map(|server| server.as_ref().unwrap().address().to_owned())
            .collect(),
    };
    let leader = |replicas: &[Option<Replica>]| {
        replicas.iter().position(|replica| {
            replica
                .as_ref()
                .is_some_and(|replica| replica.log.host().unwrap().status().is_leader)
        })
    };
    wait(|| leader(&replicas).is_some());
    let identity = (1 << 63) + 999;
    let sdk = || {
        let mut sdk = GatewayClient::with_identity(
            network.clone(),
            group.endpoints[0].clone(),
            identity,
            1,
            Duration::from_secs(5),
        )
        .unwrap();
        sdk.set_replicas(0, group.clone()).unwrap();
        sdk
    };
    let mut original = sdk();
    assert!(resolve_gateway(|| original.insert(
        target(),
        b"receipt",
        b"original"
    )));
    let first = leader(&replicas).unwrap();
    replicas[first]
        .as_ref()
        .unwrap()
        .put(b"receipt", b"new value");
    let local_log = LocalLog::new(Duration::from_secs(5)).unwrap();
    let local = Store::with_engine(
        1,
        false,
        local_log.clone(),
        super::database(&root.path().join("coordinator-data"), 1),
    )
    .unwrap();
    let mut worker = local.database.worker().unwrap();
    local.open(&mut worker, target().access.table, 1).unwrap();
    let local_target = Target {
        owner: 1,
        ..target()
    };
    let proxy_schema = Schema::new(
        vec![TableSpec {
            id: 1,
            name: "rows".into(),
            kind: Kind::Governed,
            warehouse: false,
            initial_owner: 0,
        }],
        2,
        0,
    )
    .unwrap();
    local_log
        .start_engine(
            raft_host::LocalConfig {
                group: 1,
                directory: root.path().join("coordinator-log"),
                create: true,
                sync_writes: true,
            },
            &local,
            proxy_schema.clone(),
            b"coordinator".to_vec(),
        )
        .unwrap();
    let proxy = RunningShard::start_with_gateway(
        local.clone(),
        network.clone(),
        "127.0.0.1:0",
        2,
        1,
        16,
        gateway::Config {
            schema: proxy_schema,
            endpoints: BTreeMap::new(),
            replicas: BTreeMap::from([(0, group.clone())]),
            workers: 1,
        },
    )
    .unwrap();
    let mut forwarded =
        GatewayClient::connect(proxy.address().to_owned(), Duration::from_secs(5)).unwrap();
    resolve_gateway(|| forwarded.put(target(), b"forwarded", b"before failover"));
    let mut coordinator = Client::new(
        network.clone(),
        BTreeMap::new(),
        Stream { node: 1, worker: 0 },
        0,
        Duration::from_secs(5),
    )
    .unwrap();
    coordinator.set_replicas(0, group.clone()).unwrap();
    coordinator
        .run(local.clone(), &mut worker, |tx| {
            tx.mutate(&target(), Mutation::Put, b"remote", b"first leader")?;
            tx.mutate(&local_target, Mutation::Put, b"local", b"first leader")?;
            Ok(())
        })
        .unwrap();
    let committed = replicas[first]
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
                >= committed
        })
    });
    drop(servers[first].take());
    drop(replicas[first].take());
    wait(|| leader(&replicas).is_some());
    let mut resumed = sdk();
    assert_eq!(
        resolve_gateway(|| forwarded.get(target(), b"forwarded")).as_deref(),
        Some(b"before failover".as_slice())
    );
    resolve_gateway(|| forwarded.put(target(), b"forwarded", b"after failover"));
    // Retry the original identity after losing its response/connection. A
    // newly elected gateway returns the old INSERT result without repeating it.
    assert!(resolve_gateway(|| resumed.insert(
        target(),
        b"receipt",
        b"original"
    )));
    assert_eq!(
        resolve_gateway(|| resumed.get(target(), b"receipt")).as_deref(),
        Some(b"new value".as_slice())
    );
    coordinator
        .run(local, &mut worker, |tx| {
            assert_eq!(
                tx.get(&target(), b"remote")?.as_deref(),
                Some(b"first leader".as_slice())
            );
            tx.mutate(&target(), Mutation::Put, b"remote", b"next leader")?;
            tx.mutate(&local_target, Mutation::Put, b"local", b"next leader")?;
            Ok(())
        })
        .unwrap();
    let mut wrong = group.clone();
    wrong.group = 38;
    assert!(network
        .find_leader(0, &wrong, Duration::from_secs(1))
        .is_err());
    drop(worker);
    drop(proxy);
    drop(servers);
}

#[test]
fn elected_replica_restores_prepared_intent_and_obeys_the_saved_decision() {
    use mako_protocol::journal::{self, Member, Outcome, Record, Transaction, TransactionId};
    use mako_runtime::receiver::RunningShard;
    for saved in [Some(Outcome::Commit), None] {
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
        let network = Arc::new(Network::default());
        let mut servers: Vec<_> = replicas
            .iter()
            .map(|replica| {
                Some(
                    RunningShard::start(
                        replica.as_ref().unwrap().store.clone(),
                        network.clone(),
                        "127.0.0.1:0",
                        1,
                        1,
                        16,
                    )
                    .unwrap(),
                )
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
        let endpoint = servers[first].as_ref().unwrap().address();
        let Reply::OpenStream(Ok(lease)) = network
            .call(
                endpoint,
                Message::OpenStream {
                    stream: Stream { node: 0, worker: 0 },
                    nonce: 892,
                },
                Duration::from_secs(5),
            )
            .unwrap()
        else {
            panic!("lease");
        };
        let id = TransactionId {
            coordinator: 0,
            worker: 0,
            incarnation: 892,
            attempt: 1,
        };
        let Reply::Participant(response) = network
            .call(
                endpoint,
                Message::Participant(Request {
                    stream: Stream { node: 0, worker: 0 },
                    identity: Identity {
                        incarnation: lease,
                        attempt: 1,
                        sequence: 1,
                        term: 0,
                    },
                    action: Action::Stage {
                        writes: vec![Write {
                            access: target().access,
                            mutation: Mutation::Put,
                            key: b"in-doubt".to_vec(),
                            value: b"prepared data".to_vec(),
                        }],
                    },
                }),
                Duration::from_secs(5),
            )
            .unwrap()
        else {
            panic!("stage");
        };
        response.result.unwrap();
        let control = |endpoint: &str, action| {
            let reply = network
                .call(
                    endpoint,
                    Message::Transaction(TransactionRequest {
                        id,
                        lease,
                        term: 0,
                        action,
                    }),
                    Duration::from_secs(5),
                )
                .unwrap();
            let Reply::Transaction(response) = reply else {
                panic!("control: {reply:?}");
            };
            response.result.unwrap()
        };
        let TransactionValue::Locked {
            timestamp,
            durable: true,
        } = control(endpoint, TransactionAction::Lock)
        else {
            panic!("lock");
        };
        let transaction = Transaction {
            version: journal::VERSION,
            id,
            time_term: timestamp * 10,
            durable: true,
            members: vec![Member {
                owner: 0,
                lease,
                endpoints: servers
                    .iter()
                    .map(|server| server.as_ref().unwrap().address().to_owned())
                    .collect(),
            }],
        };
        let original = replicas[first].as_ref().unwrap();
        original
            .store
            .journal
            .apply(&Record::Begin(transaction.clone()))
            .unwrap();
        assert_eq!(
            control(endpoint, TransactionAction::Prepare(transaction.clone())),
            TransactionValue::Prepared
        );
        if let Some(outcome) = saved {
            original
                .store
                .journal
                .apply(&Record::Decide {
                    transaction: transaction.clone(),
                    outcome,
                })
                .unwrap();
        }
        let checkpoint = original.log.host().unwrap().checkpoint().unwrap();
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
                    >= checkpoint.index
            })
        });
        drop(servers[first].take());
        drop(replicas[first].take());
        wait(|| leader(&replicas).is_some());
        let next = leader(&replicas).unwrap();
        let endpoint = servers[next].as_ref().unwrap().address();
        let outcome = saved.unwrap_or(Outcome::Abort);
        assert_eq!(
            control(
                endpoint,
                TransactionAction::Decide {
                    transaction: transaction.clone(),
                    outcome
                }
            ),
            TransactionValue::Completed(outcome)
        );
        let recovered = replicas[next].as_ref().unwrap();
        assert_eq!(
            recovered
                .store
                .journal
                .lookup(id)
                .unwrap()
                .unwrap()
                .decision,
            Some(outcome)
        );
        assert_eq!(
            recovered.read(b"in-doubt").as_deref(),
            if saved.is_some() {
                Some(b"prepared data".as_slice())
            } else {
                None
            }
        );
    }
}
