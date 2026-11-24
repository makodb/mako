#![cfg(feature = "native-storage")]
use mako_runtime::{
    client::Client,
    gateway_client::GatewayClient,
    migration::{Kind, TableSpec},
    node::{Config, InitialRow},
    rpc::Network,
    storage::{Store, VolatileLog},
    wire::*,
};
use std::{
    collections::BTreeMap,
    io::{BufRead, BufReader},
    path::PathBuf,
    process::{Child, Command, Stdio},
    sync::{mpsc, Arc},
    time::{Duration, Instant},
};

struct Process {
    child: Child,
    config: PathBuf,
    address: String,
    ready: mpsc::Receiver<String>,
}
impl Process {
    fn start(config: Config) -> Self {
        let process = Self::spawn(config);
        process.wait_ready();
        process
    }
    fn spawn(config: Config) -> Self {
        static NEXT: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);
        let path = std::env::temp_dir().join(format!(
            "mako-node-{}-{}-{}.json",
            std::process::id(),
            config.owner,
            NEXT.fetch_add(1, std::sync::atomic::Ordering::Relaxed)
        ));
        std::fs::write(&path, serde_json::to_vec(&config).unwrap()).unwrap();
        let mut child = Command::new(env!("CARGO_BIN_EXE_mako_node"))
            .arg("--config")
            .arg(&path)
            .arg("--exit-on-stdin-close")
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::inherit())
            .spawn()
            .unwrap();
        let output = child.stdout.take().unwrap();
        let (send, recv) = mpsc::channel();
        let (listening, bound) = mpsc::channel();
        let mut process = Self {
            child,
            config: path,
            address: String::new(),
            ready: recv,
        };
        std::thread::spawn(move || {
            for line in BufReader::new(output).lines() {
                let Ok(line) = line else { break };
                if let Some(address) = line.strip_prefix("MAKO_READY ") {
                    let _ = send.send(address.to_owned());
                }
                if let Some(address) = line.strip_prefix("MAKO_LISTEN ") {
                    let _ = listening.send(address.to_owned());
                }
            }
        });
        process.address = bound
            .recv_timeout(Duration::from_secs(30))
            .expect("child listener");
        process
    }
    fn wait_ready(&self) {
        assert_eq!(
            self.ready
                .recv_timeout(Duration::from_secs(30))
                .expect("child readiness"),
            self.address
        );
    }
    fn stop(&mut self) {
        self.child.stdin.take();
        let deadline = Instant::now() + Duration::from_secs(10);
        loop {
            if let Some(status) = self.child.try_wait().unwrap() {
                assert!(status.success(), "child exit: {status}");
                break;
            }
            assert!(Instant::now() < deadline, "child shutdown deadline");
            std::thread::sleep(Duration::from_millis(20));
        }
    }
    #[cfg(feature = "replication")]
    fn crash(&mut self) {
        self.child.kill().unwrap();
        assert!(!self.child.wait().unwrap().success());
    }
}

#[test]
fn separate_process_migration_moves_raw_and_lazy_warehouse_tables() {
    use mako_sharding::{
        types::{Status, TxnId},
        wire as control,
    };
    let sockets: Vec<_> = (0..2)
        .map(|_| std::net::TcpListener::bind("127.0.0.1:0").unwrap())
        .collect();
    let peers: BTreeMap<_, _> = sockets
        .iter()
        .enumerate()
        .map(|(owner, socket)| (owner as u32, socket.local_addr().unwrap().to_string()))
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
    let mut children = Vec::new();
    for (owner, socket) in sockets.into_iter().enumerate() {
        drop(socket);
        let owner = owner as u32;
        children.push(Process::spawn(Config {
            persistence: None,
            replica_groups: BTreeMap::new(),
            owner,
            nodes: 2,
            listen: peers[&owner].clone(),
            tables: tables.clone(),
            workers: 1,
            gateway_workers: 1,
            queue_capacity: 16,
            warehouses_per_shard: 1,
            peers: peers.clone(),
            migration: true,
            replication: None,
            initial: if owner == 0 {
                vec![
                    InitialRow {
                        table: 1,
                        warehouse: 0,
                        key: b"a".to_vec(),
                        value: b"raw data".to_vec(),
                    },
                    InitialRow {
                        table: 2,
                        warehouse: 1,
                        key: b"a".to_vec(),
                        value: b"warehouse data".to_vec(),
                    },
                ]
            } else {
                Vec::new()
            },
        }));
    }
    for child in &children {
        child.wait_ready();
    }
    let network = Network::default();
    let admin = |operation, payload| match network
        .call(
            &peers[&0],
            Message::Migration {
                owner: 0,
                operation,
                payload,
            },
            Duration::from_secs(5),
        )
        .unwrap()
    {
        Reply::Migration { status, payload } if status == Status::Ok as u32 => {
            control::decode_outcome(&payload).unwrap()
        }
        reply => panic!("admin reply: {reply:?}"),
    };
    let mut gateway = GatewayClient::connect(peers[&0].clone(), Duration::from_secs(5)).unwrap();
    for (table, warehouse, name, value) in [
        (1, 0, "raw", b"raw data".as_slice()),
        (2, 1, "warehouse", b"warehouse data".as_slice()),
    ] {
        let nonce = TxnId {
            client: 500,
            sequence: table,
        };
        let (lo, hi) = if warehouse == 0 {
            (Vec::new(), None)
        } else {
            (
                1u32.to_be_bytes().to_vec(),
                Some(2u32.to_be_bytes().to_vec()),
            )
        };
        let begin = control::Begin {
            nonce,
            table_name: name.as_bytes().to_vec(),
            lo,
            hi,
            source: 0,
            destination: 1,
        };
        let hex = |bytes: &[u8]| {
            bytes
                .iter()
                .map(|byte| format!("{byte:02x}"))
                .collect::<String>()
        };
        let output = Command::new(env!("CARGO_BIN_EXE_mako_admin"))
            .args([
                "begin",
                &peers[&0],
                &nonce.client.to_string(),
                &nonce.sequence.to_string(),
                name,
                &hex(&begin.lo),
                &begin
                    .hi
                    .as_ref()
                    .map_or_else(|| "inf".to_owned(), |hi| hex(hi)),
                "0",
                "1",
            ])
            .output()
            .unwrap();
        assert!(
            output.status.success(),
            "native admin begin: {}",
            String::from_utf8_lossy(&output.stderr)
        );
        // Repeating the same accepted identity through the library is idempotent.
        admin(control::ADMIN_BEGIN, control::encode_begin(&begin).unwrap());
        let deadline = Instant::now() + Duration::from_secs(20);
        while admin(control::ADMIN_POLL, control::encode_nonce(nonce).unwrap()).1 != 1 {
            assert!(Instant::now() < deadline, "migration deadline");
            std::thread::sleep(Duration::from_millis(10));
        }
        let output = Command::new(env!("CARGO_BIN_EXE_mako_admin"))
            .args([
                "poll",
                &peers[&0],
                &nonce.client.to_string(),
                &nonce.sequence.to_string(),
            ])
            .output()
            .unwrap();
        assert!(
            output.status.success(),
            "native admin poll: {}",
            String::from_utf8_lossy(&output.stderr)
        );
        assert!(String::from_utf8(output.stdout)
            .unwrap()
            .contains("outcome=committed"));
        let route = gateway
            .resolve(RouteRequest {
                table: Some(table),
                name: None,
                warehouse,
                key: b"a".to_vec(),
            })
            .unwrap();
        assert_eq!(route.owner, 1);
        assert_eq!(
            gateway.get(route.clone(), b"a").unwrap().as_deref(),
            Some(value)
        );
        gateway.put(route.clone(), b"a", b"after move").unwrap();
        assert_eq!(
            gateway.get(route, b"a").unwrap().as_deref(),
            Some(b"after move".as_slice())
        );
    }
    for child in &mut children {
        child.stop();
    }
}
impl Drop for Process {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
        let _ = std::fs::remove_file(&self.config);
    }
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

#[test]
fn separate_process_clocks_commit_and_abort_through_native_srpc() {
    let mut child = Process::start(Config {
        persistence: None,
        replica_groups: BTreeMap::new(),
        owner: 1,
        nodes: 2,
        listen: "127.0.0.1:0".into(),
        tables: vec![TableSpec {
            id: 1,
            name: "rows".into(),
            kind: Kind::Governed,
            warehouse: false,
            initial_owner: 1,
        }],
        workers: 1,
        gateway_workers: 1,
        queue_capacity: 16,
        warehouses_per_shard: 0,
        peers: BTreeMap::new(),
        migration: false,
        replication: None,
        initial: Vec::new(),
    });
    let mut gateway =
        GatewayClient::connect(child.address.clone(), Duration::from_secs(5)).unwrap();
    // Advance only the other process's clock before the distributed transaction.
    for index in 0u32..40 {
        gateway
            .put(target(1), b"ahead", &index.to_be_bytes())
            .unwrap();
    }
    let store = Store::new(0, false, Arc::new(VolatileLog)).unwrap();
    let mut worker = store.database.worker().unwrap();
    store.open(&mut worker, target(0).access.table, 1).unwrap();
    let mut client = Client::new(
        Arc::new(Network::default()),
        BTreeMap::from([(1, child.address.clone())]),
        Stream { node: 0, worker: 0 },
        0,
        Duration::from_secs(5),
    )
    .unwrap();
    let previous = client
        .run(store.clone(), &mut worker, |tx| {
            let ahead = tx.scan(
                &target(1),
                Bound::Included(b"ahead".to_vec()),
                Bound::Included(b"ahead".to_vec()),
                false,
                1,
            )?;
            tx.mutate(&target(0), Mutation::Put, b"local", b"committed")?;
            tx.mutate(&target(1), Mutation::Put, b"remote", b"committed")?;
            Ok(ahead[0].time_term)
        })
        .unwrap();
    client
        .run(store.clone(), &mut worker, |tx| {
            let local = tx.scan(&target(0), Bound::Unbounded, Bound::Unbounded, false, 8)?;
            let remote = tx.scan(
                &target(1),
                Bound::Included(b"remote".to_vec()),
                Bound::Unbounded,
                false,
                8,
            )?;
            assert_eq!(local[0].value, b"committed");
            assert_eq!(remote[0].value, b"committed");
            assert_eq!(local[0].time_term, remote[0].time_term);
            assert!(
                local[0].time_term > previous,
                "remote clock must participate in timestamp selection"
            );
            Ok(())
        })
        .unwrap();
    let failed: Result<()> = client.run(store.clone(), &mut worker, |tx| {
        tx.mutate(&target(0), Mutation::Put, b"local", b"abort")?;
        tx.mutate(&target(1), Mutation::Put, b"remote", b"abort")?;
        tx.get(&target(1), b"remote")?;
        Err(Error::new(
            ErrorCode::Invalid,
            "body rejected after remote locks",
        ))
    });
    assert_eq!(failed.unwrap_err().code, ErrorCode::Invalid);
    client
        .run(store, &mut worker, |tx| {
            assert_eq!(
                tx.get(&target(0), b"local")?.as_deref(),
                Some(b"committed".as_slice())
            );
            assert_eq!(
                tx.get(&target(1), b"remote")?.as_deref(),
                Some(b"committed".as_slice())
            );
            tx.mutate(&target(1), Mutation::Put, b"remote", b"after abort")?;
            Ok(())
        })
        .unwrap();
    assert_eq!(
        gateway.get(target(1), b"remote").unwrap().as_deref(),
        Some(b"after abort".as_slice())
    );
    child.stop();
}

#[test]
#[cfg(feature = "replication")]
fn replicated_server_recovers_rows_in_a_fresh_process() {
    use mako_runtime::node::{ReplicaConfig, ReplicaPeer};
    use mako_sharding::gateway as core;
    let root = tempfile::tempdir().unwrap();
    let socket = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
    let raft_address = socket.local_addr().unwrap().to_string();
    drop(socket);
    let mut config = Config {
        persistence: None,
        replica_groups: BTreeMap::new(),
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
        migration: false,
        initial: Vec::new(),
        replication: Some(ReplicaConfig {
            group: 40,
            id: 1,
            listen: raft_address.clone(),
            peers: vec![ReplicaPeer {
                id: 1,
                address: raft_address,
            }],
            directory: root.path().to_owned(),
            create: true,
            sync_writes: true,
        }),
    };
    let mut first = Process::start(config.clone());
    let mut client =
        GatewayClient::connect(first.address.clone(), Duration::from_secs(10)).unwrap();
    client.put(target(0), b"a", b"durable").unwrap();
    client.put(target(0), b"deleted", b"temporary").unwrap();
    assert!(client.remove(target(0), b"deleted").unwrap());
    let network = Network::default();
    let call = |address: &str, request: &GatewayRequest| match network
        .call(
            address,
            Message::Gateway(request.clone()),
            Duration::from_secs(10),
        )
        .unwrap()
    {
        Reply::Gateway(response) => response,
        reply => panic!("gateway response: {reply:?}"),
    };
    let mut receipts = Vec::new();
    for (index, (kind, key, value)) in [
        (core::INSERT, b"inserted".as_slice(), b"original".as_slice()),
        (core::GET, b"inserted", b""),
        (core::DELETE, b"inserted", b""),
        (core::INSERT, b"a", b"must not replace"),
        (core::DELETE, b"missing", b""),
    ]
    .into_iter()
    .enumerate()
    {
        let request = GatewayRequest {
            client: (1 << 63) + 400 + index as u64,
            sequence: 1,
            session: 0,
            kind,
            target: Some(target(0)),
            key: key.to_vec(),
            value: value.to_vec(),
            forwarded: false,
        };
        let response = call(&first.address, &request);
        assert_eq!(response.outcome, core::COMMITTED);
        assert_eq!(response.op_result, index < 3);
        if kind == core::GET {
            assert_eq!(response.value, b"original");
        }
        receipts.push((request, response));
    }
    client
        .put(target(0), b"inserted", b"new generation")
        .unwrap();
    // SIGKILL exercises committed WAL recovery, without graceful host shutdown.
    first.crash();
    drop(first);
    config.replication.as_mut().unwrap().create = false;
    let mut recovered = Process::start(config);
    let mut client =
        GatewayClient::connect(recovered.address.clone(), Duration::from_secs(10)).unwrap();
    assert_eq!(
        client.get(target(0), b"a").unwrap().as_deref(),
        Some(b"durable".as_slice())
    );
    assert_eq!(client.get(target(0), b"deleted").unwrap(), None);
    for (request, expected) in &receipts {
        assert_eq!(call(&recovered.address, request), *expected);
    }
    assert_eq!(
        client.get(target(0), b"inserted").unwrap().as_deref(),
        Some(b"new generation".as_slice())
    );
    let mut changed = receipts[0].0.clone();
    changed.value = b"changed request".to_vec();
    assert_ne!(call(&recovered.address, &changed).outcome, core::COMMITTED);
    client.put(target(0), b"a", b"after recovery").unwrap();
    assert_eq!(
        client.get(target(0), b"a").unwrap().as_deref(),
        Some(b"after recovery".as_slice())
    );
    recovered.stop();
}

#[test]
#[cfg(feature = "replication")]
fn local_disk_server_recovers_rows_and_receipts_after_sigkill_without_raft() {
    use mako_runtime::node::PersistenceConfig;
    use mako_sharding::gateway as core;
    let root = tempfile::tempdir().unwrap();
    let mut config = Config {
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
        migration: false,
        initial: Vec::new(),
        replication: None,
        replica_groups: BTreeMap::new(),
        persistence: Some(PersistenceConfig {
            directory: root.path().to_owned(),
            create: true,
            sync_writes: true,
        }),
    };
    let request = GatewayRequest {
        client: (1 << 63) + 305,
        sequence: 1,
        session: 0,
        kind: core::INSERT,
        target: Some(target(0)),
        key: b"row".to_vec(),
        value: b"original".to_vec(),
        forwarded: false,
    };
    let network = Network::default();
    let mut server = Process::start(config.clone());
    let expected = match network
        .call(
            &server.address,
            Message::Gateway(request.clone()),
            Duration::from_secs(5),
        )
        .unwrap()
    {
        Reply::Gateway(reply) => reply,
        reply => panic!("insert response: {reply:?}"),
    };
    assert_eq!(expected.outcome, core::COMMITTED);
    assert!(expected.op_result);
    let mut client =
        GatewayClient::connect(server.address.clone(), Duration::from_secs(5)).unwrap();
    client.put(target(0), b"row", b"later value").unwrap();
    server.crash();
    drop(server);
    config.persistence.as_mut().unwrap().create = false;
    let mut recovered = Process::start(config);
    match network
        .call(
            &recovered.address,
            Message::Gateway(request),
            Duration::from_secs(5),
        )
        .unwrap()
    {
        Reply::Gateway(reply) => assert_eq!(reply, expected),
        reply => panic!("receipt response: {reply:?}"),
    }
    let mut client =
        GatewayClient::connect(recovered.address.clone(), Duration::from_secs(5)).unwrap();
    assert_eq!(
        client.get(target(0), b"row").unwrap().as_deref(),
        Some(b"later value".as_slice())
    );
    recovered.stop();
}
