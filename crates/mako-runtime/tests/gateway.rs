#![cfg(feature = "native-storage")]
use mako_runtime::gateway_client::GatewayClient;
use mako_runtime::{
    gateway,
    migration::{Kind, Schema, TableSpec},
    receiver::RunningShard,
    rpc::{Network, RpcServer},
    storage::{CommitLog, Store},
    wire::*,
};
use mako_sharding::gateway as protocol;
use std::{
    collections::BTreeMap,
    sync::{
        atomic::{AtomicUsize, Ordering},
        Arc,
    },
    time::Duration,
};

use sto_core::protocol::journal::{self, Entry, Journal, Record, TransactionId};
use parking_lot::Mutex;

struct Log {
    entries: Mutex<BTreeMap<TransactionId, Entry>>,
    calls: AtomicUsize,
    fail: bool,
}
impl CommitLog for Log {
    fn append(&self, _: &[u8], _: u32) -> Result<()> {
        panic!("obsolete post-install logging")
    }
}
impl Journal for Log {
    fn durable(&self) -> bool {
        false
    }
    fn apply(&self, record: &Record) -> Result<Entry> {
        let mut entries = self.entries.lock();
        let id = record.transaction().id;
        let next = journal::transition(id.coordinator, false, entries.get(&id), record)?;
        if !next.rows.is_empty() {
            self.calls.fetch_add(1, Ordering::Relaxed);
            if self.fail {
                return Err(Error::new(
                    ErrorCode::Unknown,
                    "completion acknowledgement lost",
                ));
            }
        }
        entries.insert(id, next.entry.clone());
        Ok(next.entry)
    }
    fn lookup(&self, id: TransactionId) -> Result<Option<Entry>> {
        Ok(self.entries.lock().get(&id).cloned())
    }
    fn entries(&self) -> Result<Vec<Entry>> {
        Ok(self.entries.lock().values().cloned().collect())
    }
}

fn schema(owner: u32, nodes: u32) -> Arc<Schema> {
    Schema::new(
        vec![TableSpec {
            id: 1,
            name: "data".into(),
            kind: Kind::Governed,
            warehouse: false,
            initial_owner: owner,
        }],
        nodes,
        0,
    )
    .unwrap()
}
fn store(owner: u32, log: Arc<Log>) -> Arc<Store> {
    let store = Store::new(owner, false, log).unwrap();
    let mut worker = store.database.worker().unwrap();
    store
        .open(
            &mut worker,
            TableKey {
                table: 1,
                warehouse: 0,
            },
            1,
        )
        .unwrap();
    store
}
fn request(sequence: u64, kind: u32, target: Target, value: &[u8]) -> GatewayRequest {
    GatewayRequest {
        client: 0x8000_0000_0000_0042,
        sequence,
        session: 0,
        kind,
        target: Some(target),
        key: b"key".to_vec(),
        value: value.to_vec(),
        forwarded: false,
    }
}
fn call(network: &Network, endpoint: &str, request: GatewayRequest) -> GatewayResponse {
    let identity = (request.client, request.sequence);
    match network
        .call(endpoint, Message::Gateway(request), Duration::from_secs(6))
        .unwrap()
    {
        Reply::Gateway(reply) => {
            assert_eq!((reply.client, reply.sequence), identity);
            reply
        }
        reply => panic!("gateway reply: {reply:?}"),
    }
}
fn route(network: &Network, endpoint: &str) -> Target {
    match network
        .call(
            endpoint,
            Message::Route(RouteRequest {
                table: None,
                name: Some("data".into()),
                warehouse: 0,
                key: b"key".to_vec(),
            }),
            Duration::from_secs(3),
        )
        .unwrap()
    {
        Reply::Route(result) => result.unwrap(),
        reply => panic!("route reply: {reply:?}"),
    }
}

#[test]
fn native_gateway_retains_mutation_result_bits_and_exact_read_bytes() {
    let log = Arc::new(Log {
        entries: Mutex::new(BTreeMap::new()),
        calls: AtomicUsize::new(0),
        fail: false,
    });
    let store = store(0, log.clone());
    let network = Arc::new(Network::default());
    let server = RunningShard::start_with_gateway(
        store,
        network.clone(),
        "127.0.0.1:0",
        1,
        1,
        16,
        gateway::Config {
            replicas: BTreeMap::new(),
            schema: schema(0, 1),
            endpoints: BTreeMap::new(),
            workers: 2,
        },
    )
    .unwrap();
    let target = route(&network, server.address());
    let insert = request(1, protocol::INSERT, target.clone(), b"first");
    let first = call(&network, server.address(), insert.clone());
    assert_eq!(
        (first.status, first.outcome, first.op_result),
        (0, protocol::COMMITTED, true)
    );
    assert_eq!(call(&network, server.address(), insert), first);
    assert_eq!(log.calls.load(Ordering::Relaxed), 1);
    assert!(
        !call(
            &network,
            server.address(),
            request(2, protocol::INSERT, target.clone(), b"duplicate")
        )
        .op_result
    );
    let get = request(3, protocol::GET, target.clone(), b"");
    let read = call(&network, server.address(), get.clone());
    assert_eq!(read.value, b"first");
    let mut other = request(1, protocol::PUT, target.clone(), b"changed");
    other.client += 1;
    assert_eq!(
        call(&network, server.address(), other).outcome,
        protocol::COMMITTED
    );
    assert_eq!(call(&network, server.address(), get.clone()), read);
    let mut mismatched = get;
    mismatched.key = b"different".to_vec();
    assert_eq!(
        call(&network, server.address(), mismatched).outcome,
        protocol::REJECTED
    );
    let delete = request(4, protocol::DELETE, target.clone(), b"");
    assert!(call(&network, server.address(), delete.clone()).op_result);
    assert!(call(&network, server.address(), delete).op_result);
    let absent = call(
        &network,
        server.address(),
        request(5, protocol::GET, target, b""),
    );
    assert_eq!(
        (absent.status, absent.outcome, absent.op_result),
        (3, protocol::COMMITTED, false)
    );
    let mut sdk = GatewayClient::with_identity(
        network,
        server.address().to_owned(),
        0x8000_0000_0000_0077,
        1,
        Duration::from_secs(3),
    )
    .unwrap();
    let target = sdk
        .resolve(RouteRequest {
            table: Some(1),
            name: None,
            warehouse: 0,
            key: b"session".to_vec(),
        })
        .unwrap();
    sdk.begin_session().unwrap();
    sdk.put(target.clone(), b"session", b"auto-committed")
        .unwrap();
    sdk.end_session(true).unwrap();
    assert_eq!(
        sdk.get(target, b"session").unwrap().as_deref(),
        Some(b"auto-committed".as_slice())
    );
}

#[test]
fn unknown_gateway_commit_is_not_reexecuted_or_reported_as_an_abort() {
    let log = Arc::new(Log {
        entries: Mutex::new(BTreeMap::new()),
        calls: AtomicUsize::new(0),
        fail: true,
    });
    let store = store(0, log.clone());
    let network = Arc::new(Network::default());
    let server = RunningShard::start_with_gateway(
        store.clone(),
        network.clone(),
        "127.0.0.1:0",
        1,
        1,
        16,
        gateway::Config {
            replicas: BTreeMap::new(),
            schema: schema(0, 1),
            endpoints: BTreeMap::new(),
            workers: 1,
        },
    )
    .unwrap();
    let target = route(&network, server.address());
    let put = request(1, protocol::PUT, target.clone(), b"installed");
    let unknown = call(&network, server.address(), put.clone());
    assert_eq!(unknown.outcome, protocol::UNKNOWN);
    assert_eq!(call(&network, server.address(), put), unknown);
    assert_eq!(log.calls.load(Ordering::Relaxed), 1);
    assert_eq!(
        call(
            &network,
            server.address(),
            request(2, protocol::PUT, target, b"forbidden")
        )
        .outcome,
        protocol::REJECTED
    );
    let cut = store.epochs.cut().unwrap();
    assert!(!store.epochs.drained(cut).unwrap());
}

#[test]
fn forwarded_gateway_resolves_lost_reply_without_replaying_effect() {
    let log = Arc::new(Log {
        entries: Mutex::new(BTreeMap::new()),
        calls: AtomicUsize::new(0),
        fail: false,
    });
    let target_store = store(1, log.clone());
    let network = Arc::new(Network::default());
    let schema = schema(1, 2);
    let target = RunningShard::start_with_gateway(
        target_store,
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
    .unwrap();
    let target_address = target.address().to_owned();
    let forward_network = network.clone();
    let deliveries = Arc::new(AtomicUsize::new(0));
    let count = deliveries.clone();
    let mut proxy = RpcServer::start(&network, "127.0.0.1:0", move |message, respond| {
        let network = forward_network.clone();
        let endpoint = target_address.clone();
        let count = count.clone();
        std::thread::spawn(move || {
            let reply = network
                .call(&endpoint, message, Duration::from_secs(3))
                .unwrap();
            if count.fetch_add(1, Ordering::SeqCst) != 0 {
                respond(reply);
            }
        });
    })
    .unwrap();
    let ingress_store = store(
        0,
        Arc::new(Log {
            entries: Mutex::new(BTreeMap::new()),
            calls: AtomicUsize::new(0),
            fail: false,
        }),
    );
    let ingress = RunningShard::start_with_gateway(
        ingress_store,
        network.clone(),
        "127.0.0.1:0",
        2,
        1,
        16,
        gateway::Config {
            replicas: BTreeMap::new(),
            schema,
            endpoints: BTreeMap::from([(1, proxy.address().to_owned())]),
            workers: 1,
        },
    )
    .unwrap();
    let target = route(&network, ingress.address());
    assert_eq!(target.owner, 1);
    let put = request(1, protocol::PUT, target, b"exactly once");
    let lost = call(&network, ingress.address(), put.clone());
    assert_eq!((lost.status, lost.outcome), (1, protocol::UNKNOWN));
    assert_eq!(log.calls.load(Ordering::Relaxed), 1);
    let resolved = call(&network, ingress.address(), put);
    assert_eq!(resolved.outcome, protocol::COMMITTED);
    assert_eq!(log.calls.load(Ordering::Relaxed), 1);
    assert_eq!(deliveries.load(Ordering::Relaxed), 2);
    drop(ingress);
    proxy.shutdown();
}
