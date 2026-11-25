#![cfg(feature = "native-storage")]
use mako_runtime::{
    receiver::RunningShard,
    rpc::Network,
    storage::{CommitLog, Store, VolatileLog},
    wire::*,
};
use std::{
    sync::{
        atomic::{AtomicUsize, Ordering},
        Arc,
    },
    time::Duration,
};

fn address() -> String {
    std::net::TcpListener::bind("127.0.0.1:0")
        .unwrap()
        .local_addr()
        .unwrap()
        .to_string()
}
fn access() -> Access {
    Access {
        table: TableKey {
            table: 1,
            warehouse: 0,
        },
        route: None,
    }
}
fn provision(store: &Store) {
    let mut worker = store.database.worker().unwrap();
    store.open(&mut worker, access().table, 1).unwrap();
}
thread_local! { static LEASE: std::cell::Cell<u64> = const { std::cell::Cell::new(0) }; }
fn bind(network: &Network, address: &str) {
    let reply = network
        .call(
            address,
            Message::OpenStream {
                stream: Stream { node: 0, worker: 0 },
                nonce: 42,
            },
            Duration::from_secs(5),
        )
        .unwrap();
    let Reply::OpenStream(Ok(lease)) = reply else {
        panic!("claim reply: {reply:?}");
    };
    LEASE.set(lease);
}
fn request(attempt: u64, sequence: u64, action: Action) -> Request {
    Request {
        stream: Stream { node: 0, worker: 0 },
        identity: Identity {
            incarnation: LEASE.get(),
            attempt,
            sequence,
            term: 0,
        },
        action,
    }
}
fn call(network: &Network, address: &str, request: Request) -> Response {
    let identity = request.identity;
    match network
        .call(
            address,
            Message::Participant(request),
            Duration::from_secs(5),
        )
        .unwrap()
    {
        Reply::Participant(response) => {
            assert_eq!(response.identity, identity);
            response
        }
        other => panic!("unexpected reply: {other:?}"),
    }
}
fn write(value: &[u8]) -> Action {
    Action::Stage {
        writes: vec![Write {
            access: access(),
            mutation: Mutation::Put,
            key: b"key".to_vec(),
            value: value.to_vec(),
        }],
    }
}

#[test]
fn retained_srpc_participant_commits_replays_receipt_scans_and_aborts() {
    let store = Store::new(0, false, Arc::new(VolatileLog)).unwrap();
    provision(&store);
    let network = Arc::new(Network::default());
    let address = address();
    let server = RunningShard::start(store.clone(), network.clone(), &address, 1, 1, 8).unwrap();
    bind(&network, &address);
    assert_eq!(
        call(&network, &address, request(1, 1, write(b"first")))
            .result
            .unwrap(),
        Value::Done
    );
    let timestamp = match call(&network, &address, request(1, 2, Action::Timestamp))
        .result
        .unwrap()
    {
        Value::Timestamp(t) => t,
        _ => panic!("timestamp"),
    };
    call(&network, &address, request(1, 3, Action::Validate))
        .result
        .unwrap();
    let install = request(1, 4, Action::Install { timestamp });
    let response = call(&network, &address, install.clone());
    assert!(response.result.is_ok());
    assert_eq!(call(&network, &address, install), response);
    assert_eq!(
        call(&network, &address, request(1, 5, Action::Abort))
            .result
            .unwrap_err()
            .code,
        ErrorCode::Unknown
    );

    let get = request(
        2,
        1,
        Action::Get {
            access: access(),
            key: b"key".to_vec(),
        },
    );
    assert!(
        matches!(call(&network, &address, get).result.unwrap(), Value::Point { value: Some(v), time_term } if v == b"first" && time_term > 0)
    );
    let scan = Action::Scan {
        access: access(),
        lower: Bound::Unbounded,
        upper: Bound::Unbounded,
        reverse: true,
        limit: 8,
    };
    assert!(
        matches!(call(&network, &address, request(2, 2, scan)).result.unwrap(), Value::Scan { rows, read_dependency } if rows.len() == 1 && read_dependency > 0)
    );
    call(&network, &address, request(2, 3, write(b"discarded")))
        .result
        .unwrap();
    call(&network, &address, request(2, 4, Action::Abort))
        .result
        .unwrap();
    let get = request(
        3,
        1,
        Action::Get {
            access: access(),
            key: b"key".to_vec(),
        },
    );
    assert!(
        matches!(call(&network, &address, get).result.unwrap(), Value::Point { value: Some(v), .. } if v == b"first")
    );
    let cut = store.epochs.cut().unwrap();
    assert!(!store.epochs.drained(cut).unwrap());
    drop(server);
    assert!(store.epochs.drained(cut).unwrap());
}

struct FailedLog(AtomicUsize);
impl CommitLog for FailedLog {
    fn append(&self, _: &[u8], _: u32) -> Result<()> {
        self.0.fetch_add(1, Ordering::Relaxed);
        Err(Error::new(ErrorCode::Transport, "log acknowledgement lost"))
    }
}
#[test]
fn failed_install_is_retained_as_unknown_and_blocks_migration_drain() {
    let log = Arc::new(FailedLog(AtomicUsize::new(0)));
    let store = Store::new(0, false, log.clone()).unwrap();
    provision(&store);
    let network = Arc::new(Network::default());
    let address = address();
    let server = RunningShard::start(store.clone(), network.clone(), &address, 1, 1, 8).unwrap();
    bind(&network, &address);
    call(&network, &address, request(1, 1, write(b"installed")))
        .result
        .unwrap();
    let timestamp = match call(&network, &address, request(1, 2, Action::Timestamp))
        .result
        .unwrap()
    {
        Value::Timestamp(t) => t,
        _ => panic!("timestamp"),
    };
    call(&network, &address, request(1, 3, Action::Validate))
        .result
        .unwrap();
    let install = request(1, 4, Action::Install { timestamp });
    let response = call(&network, &address, install.clone());
    assert_eq!(
        response.result.as_ref().unwrap_err().code,
        ErrorCode::Unknown
    );
    assert_eq!(call(&network, &address, install), response);
    assert_eq!(log.0.load(Ordering::Relaxed), 1);
    assert_eq!(
        call(&network, &address, request(2, 1, write(b"forbidden")))
            .result
            .unwrap_err()
            .code,
        ErrorCode::Unknown
    );
    let cut = store.epochs.cut().unwrap();
    assert!(!store.epochs.drained(cut).unwrap());
    drop(server);
    // Destroying a request lane does not resolve its installed outcome. The
    // store remains alive, so migration must not mistake teardown for drain.
    assert!(!store.epochs.drained(cut).unwrap());
}

#[test]
fn new_attempt_cannot_steal_locks_and_delayed_abort_cannot_cancel_its_successor() {
    let store = Store::new(0, false, Arc::new(VolatileLog)).unwrap();
    provision(&store);
    let network = Arc::new(Network::default());
    let address = address();
    let _server = RunningShard::start(store, network.clone(), &address, 1, 1, 8).unwrap();
    bind(&network, &address);
    call(&network, &address, request(1, 1, write(b"old")))
        .result
        .unwrap();
    assert_eq!(
        call(&network, &address, request(2, 1, write(b"new")))
            .result
            .unwrap_err()
            .code,
        ErrorCode::Conflict
    );
    call(&network, &address, request(1, 2, Action::Abort))
        .result
        .unwrap();
    call(&network, &address, request(2, 1, write(b"new")))
        .result
        .unwrap();
    assert_eq!(
        call(&network, &address, request(1, 3, Action::Abort))
            .result
            .unwrap_err()
            .code,
        ErrorCode::Conflict
    );
    call(&network, &address, request(2, 2, Action::Abort))
        .result
        .unwrap();
}
