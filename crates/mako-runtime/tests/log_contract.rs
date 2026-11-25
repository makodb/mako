#![cfg(feature = "replication")]
#[path = "../../mako-protocol/tests/support/log_contract.rs"]
mod log_contract;
use mako_runtime::{
    durable::DurableRows,
    log::{LocalLog, MemoryLog, ReplicatedLog},
    migration::{Kind, Schema, TableSpec},
    storage::{CommitLog, Store},
};
use std::time::{Duration, Instant};

#[test]
fn all_three_backends_obey_the_same_transaction_log_contract() {
    log_contract::check(&MemoryLog::new(0), false);
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
    let root = tempfile::tempdir().unwrap();
    let local = LocalLog::new(Duration::from_secs(5)).unwrap();
    let local_store = Store::new(0, false, local.clone()).unwrap();
    local
        .start(
            raft_host::LocalConfig {
                group: 17,
                directory: root.path().join("local/log"),
                create: true,
                sync_writes: true,
            },
            &local_store,
            schema.clone(),
            DurableRows::open(&root.path().join("local/rows"), 17, true, b"log-contract").unwrap(),
        )
        .unwrap();
    log_contract::check(&*local, true);
    local.shutdown();

    let replicated = ReplicatedLog::new(Duration::from_secs(5)).unwrap();
    let replicated_store = Store::new(0, false, replicated.clone()).unwrap();
    let listener = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
    let address = listener.local_addr().unwrap().to_string();
    drop(listener);
    replicated
        .start(
            raft_host::Config {
                group: 17,
                node: 1,
                listen: address.clone(),
                peers: vec![raft_host::Peer { id: 1, address }],
                directory: root.path().join("replicated/log"),
                create: true,
                election_timeout: Duration::from_millis(300),
                ..raft_host::Config::default()
            },
            &replicated_store,
            schema,
            DurableRows::open(
                &root.path().join("replicated/rows"),
                17,
                true,
                b"log-contract",
            )
            .unwrap(),
        )
        .unwrap();
    let deadline = Instant::now() + Duration::from_secs(10);
    while !replicated.ready() {
        assert!(Instant::now() < deadline, "Raft did not become ready");
        std::thread::sleep(Duration::from_millis(10));
    }
    log_contract::check(&*replicated, true);
    replicated.shutdown();
}
