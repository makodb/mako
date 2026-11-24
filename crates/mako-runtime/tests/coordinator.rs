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
