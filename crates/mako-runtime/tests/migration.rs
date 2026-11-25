#![cfg(feature = "native-storage")]
use mako_runtime::{
    client::Client,
    migration::{Kind, RunningMigration, Schema, TableSpec},
    receiver::RunningShard,
    rpc::Network,
    storage::{MemoryLog, Store},
    wire::*,
};
use mako_sharding::{
    runtime::runtime,
    types::{Status, TxnId},
    wire as control,
};
use std::{
    collections::BTreeMap,
    sync::Arc,
    time::{Duration, Instant},
};

fn wait(mut done: impl FnMut() -> bool) {
    let deadline = Instant::now() + Duration::from_secs(20);
    while !done() {
        assert!(Instant::now() < deadline, "migration progress deadline");
        std::thread::sleep(Duration::from_millis(10));
    }
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
            Duration::from_secs(5),
        )
        .unwrap()
    {
        Reply::Migration { status, payload } if status == Status::Ok as u32 => {
            control::decode_outcome(&payload).unwrap()
        }
        reply => panic!("migration RPC: {reply:?}"),
    }
}

#[test]
fn native_migration_drains_old_admission_and_copies_latest_raw_and_warehouse_rows() {
    let schema = Schema::new(
        vec![
            TableSpec {
                id: 101,
                name: "raw".into(),
                kind: Kind::Governed,
                warehouse: false,
                initial_owner: 0,
            },
            TableSpec {
                id: 102,
                name: "warehouse".into(),
                kind: Kind::Governed,
                warehouse: true,
                initial_owner: 0,
            },
            TableSpec {
                id: 103,
                name: "items".into(),
                kind: Kind::Replicated,
                warehouse: false,
                initial_owner: 0,
            },
        ],
        2,
        1,
    )
    .unwrap();
    schema.install().unwrap();
    let source = Store::new(0, true, Arc::new(MemoryLog::new(0))).unwrap();
    let target = Store::new(1, true, Arc::new(MemoryLog::new(1))).unwrap();
    let raw = TableKey {
        table: 101,
        warehouse: 0,
    };
    let warehouse = TableKey {
        table: 102,
        warehouse: 1,
    };
    let mut worker = source.database.worker().unwrap();
    for (key, value) in [
        (raw, b"before".as_slice()),
        (warehouse, b"warehouse value".as_slice()),
    ] {
        let table = source
            .open(&mut worker, key, schema.log_id(key).unwrap())
            .unwrap();
        let mut tx = worker.begin().unwrap();
        tx.put(&table.tree, b"a", value).unwrap();
        tx.commit().unwrap();
    }
    let network = Arc::new(Network::default());
    let server0 =
        RunningShard::start(source.clone(), network.clone(), "127.0.0.1:0", 2, 1, 16).unwrap();
    let server1 =
        RunningShard::start(target.clone(), network.clone(), "127.0.0.1:0", 2, 1, 16).unwrap();
    let endpoints = BTreeMap::from([
        (0, server0.address().to_owned()),
        (1, server1.address().to_owned()),
    ]);
    let host0 = RunningMigration::start(
        source.clone(),
        schema.clone(),
        network.clone(),
        endpoints.clone(),
    )
    .unwrap();
    let host1 = RunningMigration::start(
        target.clone(),
        schema.clone(),
        network.clone(),
        endpoints.clone(),
    )
    .unwrap();
    wait(|| host0.ready() && host1.ready());
    let mut client = Client::new(
        network.clone(),
        endpoints.clone(),
        Stream { node: 0, worker: 0 },
        0,
        Duration::from_secs(5),
    )
    .unwrap();
    let old_route = schema.target(raw, b"a", 0, true).unwrap();
    let mut old = client.begin(source.clone(), &mut worker).unwrap();
    assert_eq!(
        old.get(&old_route, b"a").unwrap().as_deref(),
        Some(b"before".as_slice())
    );
    let nonce = TxnId {
        client: 900,
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
    let generation = admin(
        &network,
        &endpoints[&0],
        control::ADMIN_BEGIN,
        control::encode_begin(&begin).unwrap(),
    )
    .0;
    let grant = runtime().route(raw.table, b"a").unwrap();
    wait(|| {
        runtime()
            .node(0)
            .unwrap()
            .admission_view()
            .unwrap()
            .point(raw.table, b"a", grant)
            == Status::Retry
    });
    assert_eq!(
        admin(
            &network,
            &endpoints[&0],
            control::ADMIN_POLL,
            control::encode_nonce(nonce).unwrap()
        ),
        (generation, 0)
    );
    // The pre-freeze epoch owns this immutable admission until real cleanup.
    old.mutate(&old_route, Mutation::Put, b"a", b"after freeze")
        .unwrap();
    old.commit().unwrap();
    wait(|| {
        admin(
            &network,
            &endpoints[&0],
            control::ADMIN_POLL,
            control::encode_nonce(nonce).unwrap(),
        )
        .1 == 1
    });
    let moved = schema.target(raw, b"a", 0, true).unwrap();
    assert_eq!(moved.owner, 1);
    client
        .run(source.clone(), &mut worker, |tx| {
            assert_eq!(
                tx.get(&moved, b"a")?.as_deref(),
                Some(b"after freeze".as_slice())
            );
            Ok(())
        })
        .unwrap();
    let mut physical = worker.begin().unwrap();
    assert_eq!(
        physical
            .get(&source.table(raw).unwrap().tree, b"a")
            .unwrap(),
        None
    );
    physical.abort().unwrap();

    assert!(!target.tables().iter().any(|(key, _)| *key == warehouse));
    let nonce = TxnId {
        client: 900,
        sequence: 2,
    };
    let begin = control::Begin {
        nonce,
        table_name: b"warehouse".to_vec(),
        lo: 1u32.to_be_bytes().to_vec(),
        hi: Some(2u32.to_be_bytes().to_vec()),
        source: 0,
        destination: 1,
    };
    admin(
        &network,
        &endpoints[&0],
        control::ADMIN_BEGIN,
        control::encode_begin(&begin).unwrap(),
    );
    wait(|| {
        admin(
            &network,
            &endpoints[&0],
            control::ADMIN_POLL,
            control::encode_nonce(nonce).unwrap(),
        )
        .1 == 1
    });
    let moved = schema.target(warehouse, b"a", 0, true).unwrap();
    assert_eq!(moved.owner, 1);
    client
        .run(source.clone(), &mut worker, |tx| {
            assert_eq!(
                tx.get(&moved, b"a")?.as_deref(),
                Some(b"warehouse value".as_slice())
            );
            Ok(())
        })
        .unwrap();
    // Replicated reference tables remain local and do not require a movable grant.
    let item = schema
        .target(
            TableKey {
                table: 103,
                warehouse: 0,
            },
            b"item",
            0,
            true,
        )
        .unwrap();
    client
        .run(source, &mut worker, |tx| {
            assert_eq!(tx.get(&item, b"item")?, None);
            Ok(())
        })
        .unwrap();
    drop(server0);
    drop(server1);
    drop(host0);
    drop(host1);
    assert!(runtime()
        .warehouse_local(warehouse.table, warehouse.warehouse, 1)
        .is_err());
}

#[test]
fn failed_cold_start_leaves_only_owned_weak_callback_anchors() {
    let schema = Schema::new(
        vec![TableSpec {
            id: 200,
            name: "cold".into(),
            kind: Kind::Governed,
            warehouse: true,
            initial_owner: 0,
        }],
        1,
        1,
    )
    .unwrap();
    schema.install().unwrap();
    // The existing runtime forbids live migration with replication replay.
    runtime().replication_replay(true).unwrap();
    let store = Store::new(0, true, Arc::new(MemoryLog::new(0))).unwrap();
    let network = Arc::new(Network::default());
    assert!(RunningMigration::start(store.clone(), schema, network, BTreeMap::new()).is_err());
    // Both paths can still encounter the opener registered before start failed.
    // They must reject the expired owner rather than dereference its context.
    assert!(runtime().warehouse_local(200, 1, 0).is_err());
    assert_eq!(
        store
            .table(TableKey {
                table: 200,
                warehouse: 1
            })
            .err()
            .unwrap()
            .code,
        ErrorCode::Closed
    );
    assert!(sto_tpcc_ffi::native::migration::Registry::new(store.database.clone(), 0).is_ok());
}

#[test]
fn paged_scans_preserve_binary_cursors_order_and_limits_across_migrated_ranges() {
    let schema = Schema::new(
        vec![TableSpec {
            id: 250,
            name: "pages".into(),
            kind: Kind::Governed,
            warehouse: false,
            initial_owner: 0,
        }],
        2,
        0,
    )
    .unwrap();
    schema.install().unwrap();
    let source = Store::new(0, true, Arc::new(MemoryLog::new(0))).unwrap();
    let destination = Store::new(1, true, Arc::new(MemoryLog::new(1))).unwrap();
    let key = TableKey {
        table: 250,
        warehouse: 0,
    };
    let mut worker = source.database.worker().unwrap();
    let table = source
        .open(&mut worker, key, schema.log_id(key).unwrap())
        .unwrap();
    let mut expected: Vec<Vec<u8>> = (0..180).map(|i| format!("k{i:03}").into_bytes()).collect();
    // At this row width k052 ends the first byte-limited page. Its binary
    // extensions must survive an exclusive continuation from the exact key.
    expected.extend([
        b"k052\0".to_vec(),
        b"k052\0\0".to_vec(),
        b"k052\0\xff".to_vec(),
        b"k089\0".to_vec(),
    ]);
    expected.sort();
    for chunk in expected.chunks(24) {
        let mut tx = worker.begin().unwrap();
        for key in chunk {
            tx.put(&table.tree, key, &[b'v'; 128]).unwrap();
        }
        tx.commit().unwrap();
    }
    let network = Arc::new(Network::default());
    let server0 =
        RunningShard::start(source.clone(), network.clone(), "127.0.0.1:0", 2, 1, 16).unwrap();
    let server1 = RunningShard::start(
        destination.clone(),
        network.clone(),
        "127.0.0.1:0",
        2,
        1,
        16,
    )
    .unwrap();
    let endpoints = BTreeMap::from([
        (0, server0.address().to_owned()),
        (1, server1.address().to_owned()),
    ]);
    let host0 = RunningMigration::start(
        source.clone(),
        schema.clone(),
        network.clone(),
        endpoints.clone(),
    )
    .unwrap();
    let host1 = RunningMigration::start(
        destination,
        schema.clone(),
        network.clone(),
        endpoints.clone(),
    )
    .unwrap();
    wait(|| host0.ready() && host1.ready());
    let nonce = TxnId {
        client: 901,
        sequence: 1,
    };
    let begin = control::Begin {
        nonce,
        table_name: b"pages".to_vec(),
        lo: b"k090".to_vec(),
        hi: None,
        source: 0,
        destination: 1,
    };
    admin(
        &network,
        &endpoints[&0],
        control::ADMIN_BEGIN,
        control::encode_begin(&begin).unwrap(),
    );
    wait(|| {
        admin(
            &network,
            &endpoints[&0],
            control::ADMIN_POLL,
            control::encode_nonce(nonce).unwrap(),
        )
        .1 == 1
    });
    let mut client = Client::new(
        network,
        endpoints,
        Stream { node: 0, worker: 0 },
        0,
        Duration::from_secs(5),
    )
    .unwrap();
    client
        .run(source.clone(), &mut worker, |tx| {
            let rows = tx.scan_routed(
                &schema,
                key,
                Bound::Unbounded,
                Bound::Unbounded,
                false,
                1000,
            )?;
            assert_eq!(
                rows.iter().map(|row| row.key.clone()).collect::<Vec<_>>(),
                expected
            );
            assert!(rows.iter().all(|row| row.value == vec![b'v'; 128]));
            Ok(())
        })
        .unwrap();
    client
        .run(source.clone(), &mut worker, |tx| {
            let rows = tx.scan_routed(
                &schema,
                key,
                Bound::Included(b"k010".to_vec()),
                Bound::Included(b"k150".to_vec()),
                true,
                1000,
            )?;
            let filtered: Vec<_> = expected
                .iter()
                .rev()
                .filter(|key| key.as_slice() >= b"k010" && key.as_slice() <= b"k150")
                .cloned()
                .collect();
            assert_eq!(
                rows.into_iter().map(|row| row.key).collect::<Vec<_>>(),
                filtered
            );
            Ok(())
        })
        .unwrap();
    client
        .run(source, &mut worker, |tx| {
            let rows = tx.scan_routed(
                &schema,
                key,
                Bound::Excluded(b"k052".to_vec()),
                Bound::Unbounded,
                false,
                2,
            )?;
            assert_eq!(
                rows.into_iter().map(|row| row.key).collect::<Vec<_>>(),
                vec![b"k052\0".to_vec(), b"k052\0\0".to_vec()]
            );
            Ok(())
        })
        .unwrap();
    drop(server0);
    drop(server1);
    drop(host0);
    drop(host1);
}
