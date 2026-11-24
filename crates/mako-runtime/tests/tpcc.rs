#![cfg(feature = "native-storage")]
use mako_runtime::{
    client::Client,
    node::{Config, RunningNode},
    tpcc::{codec::*, *},
    wire::*,
};
use std::{collections::BTreeMap, time::Duration};

fn config(owner: u32, nodes: u32) -> Config {
    Config {
        owner,
        nodes,
        listen: "127.0.0.1:0".into(),
        tables: tables(),
        workers: 1,
        gateway_workers: 1,
        queue_capacity: 32,
        warehouses_per_shard: 1,
        peers: BTreeMap::new(),
        migration: false,
        initial: Vec::new(),
        replication: None,
        persistence: None,
        replica_groups: BTreeMap::new(),
    }
}
fn client(node: &RunningNode, peers: BTreeMap<u32, String>) -> Client {
    Client::new(
        node.network.clone(),
        peers,
        Stream {
            node: node.store.owner,
            worker: 0,
        },
        0,
        Duration::from_secs(5),
    )
    .unwrap()
}
fn small() -> Scale {
    Scale {
        districts: 2,
        customers: 10,
        items: 20,
        seed: 73,
    }
}

#[test]
fn codecs_match_existing_fused_kernel_bytes_and_reject_corruption() {
    // Concrete bytes from the fused kernels' stock/order/order-line layouts,
    // including signed-varint counters and an entry date requiring two bytes.
    let stock = Stock {
        quantity: 42,
        ytd: 1.5,
        orders: -1,
        remote: 64,
    };
    assert_eq!(stock.encode().unwrap(), [42, 0, 0, 0, 192, 63, 1, 128, 1]);
    let order = Order {
        customer: 3000,
        carrier: 0,
        lines: 5,
        all_local: true,
        date: 256,
    };
    assert_eq!(order.encode().unwrap(), [240, 46, 0, 5, 1, 128, 2]);
    let line = OrderLine {
        item: 17,
        delivery: 0,
        amount: 2.5,
        supply: 1,
        quantity: 3,
    };
    assert_eq!(line.encode().unwrap(), [34, 0, 0, 0, 32, 64, 2, 3]);
    assert_eq!(key(&[1, 2, 3000]), [0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 11, 184]);
    let item = Item {
        name: Inline(b"ITEM".to_vec()),
        price: 2.5,
        data: Inline(b"DATA".to_vec()),
        image: 1,
    };
    let mut expected = vec![0; 83];
    expected[0] = 4;
    expected[1..5].copy_from_slice(b"ITEM");
    expected[26..30].copy_from_slice(&[0, 0, 32, 64]);
    expected[30] = 4;
    expected[31..35].copy_from_slice(b"DATA");
    expected[82] = 2;
    assert_eq!(item.encode().unwrap(), expected);
    for length in 0..expected.len() {
        assert!(Item::decode(&expected[..length]).is_err());
    }
    let mut bad = expected.clone();
    bad[0] = 25;
    assert!(Item::decode(&bad).is_err());
    let mut bad = expected.clone();
    bad[26..30].copy_from_slice(&f32::NAN.to_le_bytes());
    assert!(Item::decode(&bad).is_err());
    let mut bad = stock.encode().unwrap();
    bad.extend_from_slice(&[0]);
    assert!(Stock::decode(&bad).is_err());
    assert!(CustomerName::decode(&[128, 0]).is_err());
    assert!(CustomerName::decode(&[255, 255, 255, 255, 16]).is_err());
    assert!(Order::decode(&[2, 0, 5, 2, 1]).is_err());
    assert!(key_field(&[], usize::MAX).is_err());
}

#[test]
fn loader_resumes_committed_batches_and_never_reloads_completed_workloads() {
    let node = RunningNode::start(config(0, 1)).unwrap();
    let scale = small();
    let context = Context {
        schema: &node.schema,
        owner: 0,
        migration: false,
        scale: &scale,
    };
    let mut client = client(&node, BTreeMap::new());
    let mut worker = node.store.database.worker().unwrap();
    let stopped = context.load(&mut client, node.store.clone(), &mut worker, |progress| {
        assert_eq!(progress.rows, 32);
        Err(Error::new(
            ErrorCode::Invalid,
            "intentional interrupted loader",
        ))
    });
    assert_eq!(stopped.unwrap_err().code, ErrorCode::Invalid);
    client
        .run(node.store.clone(), &mut worker, |tx| {
            assert!(!context.all_loaded(tx)?);
            assert!(context.reserve_dates(tx, 10).is_err());
            Ok(())
        })
        .unwrap();
    let loaded = context
        .load(&mut client, node.store.clone(), &mut worker, |_| Ok(()))
        .unwrap();
    assert!(loaded.complete && loaded.rows > 300);
    client
        .run(node.store.clone(), &mut worker, |tx| {
            assert!(context.all_loaded(tx)?);
            assert_eq!(context.reserve_dates(tx, 100)?, 2..102);
            let mut district: District = context.read(tx, DISTRICT, 1, &key(&[1, 1]))?;
            district.next_order = 777;
            context.put(tx, DISTRICT, 1, &key(&[1, 1]), &district)
        })
        .unwrap();
    let repeated = context
        .load(&mut client, node.store.clone(), &mut worker, |_| {
            panic!("complete load emits no batches")
        })
        .unwrap();
    assert!(repeated.already_loaded);
    client
        .run(node.store.clone(), &mut worker, |tx| {
            assert_eq!(context.reserve_dates(tx, 100)?, 102..202);
            assert_eq!(
                context
                    .read::<District>(tx, DISTRICT, 1, &key(&[1, 1]))?
                    .next_order,
                777
            );
            Ok(())
        })
        .unwrap();
    let changed = Scale {
        seed: scale.seed + 1,
        ..scale.clone()
    };
    let wrong = Context {
        scale: &changed,
        ..context
    };
    assert_eq!(
        wrong
            .load(&mut client, node.store.clone(), &mut worker, |_| Ok(()))
            .unwrap_err()
            .code,
        ErrorCode::Invalid
    );
}

#[test]
fn competing_order_after_district_read_aborts_speculative_insertions() {
    let node = RunningNode::start(Config {
        workers: 2,
        ..config(0, 1)
    })
    .unwrap();
    let scale = small();
    let context = Context {
        schema: &node.schema,
        owner: 0,
        migration: false,
        scale: &scale,
    };
    let mut client = client(&node, BTreeMap::new());
    let mut worker = node.store.database.worker().unwrap();
    context
        .load(&mut client, node.store.clone(), &mut worker, |_| Ok(()))
        .unwrap();
    for table in [NEW_ORDER, ORDER, ORDER_INDEX, ORDER_LINE] {
        let result = client.run(node.store.clone(), &mut worker, |tx| {
            let district: District = context.read(tx, DISTRICT, 1, &key(&[1, 1]))?;
            // This write must disappear when the stale order insertion aborts.
            let mut speculative: District = context.read(tx, DISTRICT, 1, &key(&[1, 2]))?;
            speculative.next_order += 100;
            context.put(tx, DISTRICT, 1, &key(&[1, 2]), &speculative)?;
            let handle = mako_runtime::node::Handle::from(&node);
            let scale = scale.clone();
            let committed = std::thread::spawn(move || {
                let context = Context {
                    schema: &handle.schema,
                    owner: 0,
                    migration: false,
                    scale: &scale,
                };
                let mut client = Client::new(
                    handle.network.clone(),
                    BTreeMap::new(),
                    Stream { node: 0, worker: 1 },
                    0,
                    Duration::from_secs(5),
                )
                .unwrap();
                let mut worker = handle.store.database.worker().unwrap();
                client
                    .run(handle.store.clone(), &mut worker, |tx| {
                        context.new_order(
                            tx,
                            &NewOrderRequest {
                                warehouse: 1,
                                district: 1,
                                customer: 1,
                                date: 50,
                                items: (1..=5)
                                    .map(|item| OrderItem {
                                        item,
                                        supplier: 1,
                                        quantity: 3,
                                    })
                                    .collect(),
                            },
                        )
                    })
                    .unwrap()
            })
            .join()
            .unwrap();
            assert_eq!(committed.order, district.next_order as u32);
            let order = committed.order;
            match table {
                NEW_ORDER => {
                    context.insert(tx, table, 1, &key(&[1, 1, order]), &NewOrder::default())
                }
                ORDER => context.insert(
                    tx,
                    table,
                    1,
                    &key(&[1, 1, order]),
                    &Order {
                        customer: 1,
                        carrier: 0,
                        lines: 5,
                        all_local: true,
                        date: 50,
                    },
                ),
                ORDER_INDEX => context.insert(
                    tx,
                    table,
                    1,
                    &key(&[1, 1, 1, order]),
                    &OrderIndex::default(),
                ),
                ORDER_LINE => context.insert(
                    tx,
                    table,
                    1,
                    &key(&[1, 1, order, 5]),
                    &OrderLine {
                        item: 5,
                        delivery: 0,
                        amount: 0.0,
                        supply: 1,
                        quantity: 3,
                    },
                ),
                _ => unreachable!(),
            }
        });
        assert_eq!(result.unwrap_err().code, ErrorCode::Conflict);
    }
    client
        .run(node.store.clone(), &mut worker, |tx| {
            assert_eq!(
                context
                    .read::<District>(tx, DISTRICT, 1, &key(&[1, 1]))?
                    .next_order,
                15
            );
            assert_eq!(
                context
                    .read::<District>(tx, DISTRICT, 1, &key(&[1, 2]))?
                    .next_order,
                11
            );
            let line: OrderLine = context.read(tx, ORDER_LINE, 1, &key(&[1, 1, 14, 5]))?;
            assert!(line.amount > 0.0);
            Ok(())
        })
        .unwrap();
}

#[test]
fn sharded_tpcc_executes_all_five_transactions_and_aborts_remote_stock_changes() {
    let local = RunningNode::start(config(0, 2)).unwrap();
    let remote = RunningNode::start(config(1, 2)).unwrap();
    let peers = BTreeMap::from([(0, local.address().into()), (1, remote.address().into())]);
    let scale = small();
    for node in [&local, &remote] {
        let context = Context {
            schema: &node.schema,
            owner: node.store.owner,
            migration: false,
            scale: &scale,
        };
        load_local(
            &context,
            &mut client(node, peers.clone()),
            node.store.clone(),
            |_| Ok(()),
        )
        .unwrap();
    }
    let context = Context {
        schema: &local.schema,
        owner: 0,
        migration: false,
        scale: &scale,
    };
    let mut client = client(&local, peers);
    let mut worker = local.store.database.worker().unwrap();
    let stock_before = client
        .run(local.store.clone(), &mut worker, |tx| {
            assert!(context.all_loaded(tx)?);
            // Force one known remote stock adjustment and a bad-credit customer.
            context.put(
                tx,
                STOCK,
                2,
                &key(&[1, 1]),
                &Stock {
                    quantity: 11,
                    ytd: 4.0,
                    orders: 7,
                    remote: 2,
                },
            )?;
            let mut customer: Customer = context.read(tx, CUSTOMER, 2, &key(&[1, 1, 1]))?;
            customer.credit = Fixed(*b"BC");
            context.put(tx, CUSTOMER, 2, &key(&[1, 1, 1]), &customer)?;
            context.read::<Stock>(tx, STOCK, 2, &key(&[1, 1]))
        })
        .unwrap();
    let mut order = NewOrderRequest {
        warehouse: 1,
        district: 1,
        customer: 1,
        date: 50,
        items: (1..=5)
            .map(|item| OrderItem {
                item,
                supplier: if item == 1 { 2 } else { 1 },
                quantity: 3,
            })
            .collect(),
    };
    let result = client
        .run(local.store.clone(), &mut worker, |tx| {
            context.new_order(tx, &order)
        })
        .unwrap();
    assert_eq!(result.order, 11);
    assert!(result.remote && result.total > 0.0);
    client
        .run(local.store.clone(), &mut worker, |tx| {
            let stock: Stock = context.read(tx, STOCK, 2, &key(&[1, 1]))?;
            assert_eq!(
                stock,
                Stock {
                    quantity: 99,
                    ytd: 7.0,
                    orders: stock_before.orders,
                    remote: 3
                }
            );
            let header: Order = context.read(tx, ORDER, 1, &key(&[1, 1, 11]))?;
            assert!(!header.all_local);
            let status = context.order_status(tx, 1, 1, &CustomerSelector::Id(1))?;
            assert_eq!(
                (status.customer, status.order, status.lines.len()),
                (1, 11, 5)
            );
            assert_eq!(status.lines[0].supply, 1); // origin-local physical warehouse key
            Ok(())
        })
        .unwrap();
    order.items[4].item = scale.items + 1;
    assert_eq!(
        client
            .run(local.store.clone(), &mut worker, |tx| context
                .new_order(tx, &order))
            .unwrap_err()
            .code,
        ErrorCode::Invalid
    );
    client
        .run(local.store.clone(), &mut worker, |tx| {
            assert_eq!(
                context.read::<Stock>(tx, STOCK, 2, &key(&[1, 1]))?.quantity,
                99
            );
            assert_eq!(
                context
                    .read::<District>(tx, DISTRICT, 1, &key(&[1, 1]))?
                    .next_order,
                12
            );
            let key = key(&[1, 1, 12]);
            assert!(tx.get(&context.target(ORDER, 1, &key)?, &key)?.is_none());
            Ok(())
        })
        .unwrap();
    let payment = PaymentRequest {
        warehouse: 1,
        district: 1,
        customer_warehouse: 2,
        customer_district: 1,
        customer: CustomerSelector::LastName(last_name(0)),
        amount: 7.0,
        date: 51,
    };
    let paid = client
        .run(local.store.clone(), &mut worker, |tx| {
            context.payment(tx, &payment)
        })
        .unwrap();
    assert_eq!(paid.customer, 1);
    assert_eq!(paid.balance, -17.0);
    assert!(paid.bad_credit);
    client
        .run(local.store.clone(), &mut worker, |tx| {
            assert_eq!(
                context.read::<Warehouse>(tx, WAREHOUSE, 1, &key(&[1]))?.ytd,
                300_007.0
            );
            assert_eq!(
                context
                    .read::<District>(tx, DISTRICT, 1, &key(&[1, 1]))?
                    .ytd,
                30_007.0
            );
            let customer: Customer = context.read(tx, CUSTOMER, 2, &key(&[1, 1, 1]))?;
            assert_eq!((customer.payment_count, customer.ytd_payment), (2, 17.0));
            let data: CustomerData = context.read(tx, CUSTOMER, 2, &key(&[1, 101, 1]))?;
            assert!(data.data.0.starts_with(b"1 101 1 1 1 7 | "));
            assert_eq!(
                context
                    .read::<History>(tx, HISTORY, 1, &key(&[1, 1, 1, 1, 1, 51]))?
                    .amount,
                7.0
            );
            Ok(())
        })
        .unwrap();
    let deliveries = client
        .run(local.store.clone(), &mut worker, |tx| {
            context.delivery(tx, 1, 9, 52)
        })
        .unwrap();
    assert_eq!(deliveries.len(), 2);
    client
        .run(local.store.clone(), &mut worker, |tx| {
            for delivered in &deliveries {
                assert_eq!(delivered.order, 8);
                let key = key(&[1, delivered.district, delivered.order]);
                assert!(tx
                    .get(&context.target(NEW_ORDER, 1, &key)?, &key)?
                    .is_none());
                let header: Order = context.read(tx, ORDER, 1, &key)?;
                assert_eq!(header.carrier, 9);
                let balance: Balance = context.read(
                    tx,
                    CUSTOMER,
                    1,
                    &codec::key(&[1, delivered.district + 200, delivered.customer]),
                )?;
                assert!((balance.balance - (delivered.amount - 10.0)).abs() < 0.02);
                let customer: Customer = context.read(
                    tx,
                    CUSTOMER,
                    1,
                    &codec::key(&[1, delivered.district, delivered.customer]),
                )?;
                assert_eq!(customer.balance, -10.0);
            }
            // Make all home stocks below the threshold: distinct recent item count
            // must then equal StockLevel, independent of repeated order-line items.
            for item in 1..=scale.items {
                context.put(
                    tx,
                    STOCK,
                    1,
                    &key(&[1, item]),
                    &Stock {
                        quantity: 10,
                        ..Stock::default()
                    },
                )?;
            }
            Ok(())
        })
        .unwrap();
    client
        .run(local.store.clone(), &mut worker, |tx| {
            let rows = tx.scan(
                &context.target(ORDER_LINE, 1, &[])?,
                Bound::Included(key(&[1, 1, 0, 0])),
                Bound::Excluded(key(&[1, 2, 0, 0])),
                false,
                301,
            )?;
            let distinct: std::collections::BTreeSet<_> = rows
                .iter()
                .map(|row| OrderLine::decode(&row.value).unwrap().item)
                .collect();
            assert_eq!(context.stock_level(tx, 1, 1, 20)?, distinct.len());
            Ok(())
        })
        .unwrap();
}

#[test]
fn multithreaded_native_workload_preserves_order_totals() {
    use mako_runtime::benchmark;
    use std::sync::{atomic::AtomicBool, Arc};
    let mut config = config(0, 1);
    config.workers = 3;
    let node = RunningNode::start(config.clone()).unwrap();
    let settings = benchmark::Config {
        scale: small(),
        transactions_per_worker: Some(200),
        seconds: 30,
        mix: Mix {
            weights: [20; 5],
            ..Mix::default()
        },
        ..benchmark::Config::default()
    };
    let host = benchmark::Host::from(&node);
    let stop = Arc::new(AtomicBool::new(false));
    benchmark::prepare(&host, &config, &settings, &stop, |_| Ok(())).unwrap();
    let stats = benchmark::run(&host, &config, &settings, stop).unwrap();
    assert_eq!(
        stats.committed.iter().sum::<u64>() + stats.aborted.iter().sum::<u64>(),
        600
    );
    assert!(stats.committed.iter().all(|count| *count > 0), "{stats:?}");
    let context = Context {
        schema: &node.schema,
        owner: 0,
        migration: false,
        scale: &settings.scale,
    };
    let mut client = client(&node, BTreeMap::new());
    let mut worker = node.store.database.worker().unwrap();
    client
        .run(node.store.clone(), &mut worker, |tx| {
            let mut committed = 0u64;
            for district in 1..=settings.scale.districts {
                let row: District = context.read(tx, DISTRICT, 1, &key(&[1, district]))?;
                committed += row.next_order as u64 - u64::from(settings.scale.customers) - 1;
                let rows = tx.scan(
                    &context.target(ORDER, 1, &[])?,
                    Bound::Included(key(&[1, district, 0])),
                    Bound::Excluded(key(&[1, district + 1, 0])),
                    false,
                    4096,
                )?;
                assert_eq!(rows.len(), row.next_order as usize - 1);
            }
            assert_eq!(committed, stats.committed[0]);
            Ok(())
        })
        .unwrap();
}

#[test]
#[cfg(feature = "replication")]
fn native_benchmark_process_reopens_persistent_workload_without_reloading() {
    use mako_runtime::{benchmark, node::PersistenceConfig};
    use std::process::Command;
    let directory = tempfile::tempdir().unwrap();
    let mut config = config(0, 1);
    config.persistence = Some(PersistenceConfig {
        directory: directory.path().join("data"),
        create: true,
        sync_writes: true,
    });
    let settings = benchmark::Config {
        scale: small(),
        transactions_per_worker: Some(20),
        seconds: 30,
        mix: Mix {
            weights: [100, 0, 0, 0, 0],
            ..Mix::default()
        },
        ..benchmark::Config::default()
    };
    let file = directory.path().join("bench.json");
    for run in 0..2 {
        std::fs::write(
            &file,
            serde_json::to_vec(&serde_json::json!({"node":config,"benchmark":settings})).unwrap(),
        )
        .unwrap();
        let result = Command::new(env!("CARGO_BIN_EXE_mako_bench"))
            .args(["--config", file.to_str().unwrap(), "--exit-after-run"])
            .output()
            .unwrap();
        assert!(
            result.status.success(),
            "{}\n{}",
            String::from_utf8_lossy(&result.stdout),
            String::from_utf8_lossy(&result.stderr)
        );
        let text = String::from_utf8(result.stdout).unwrap();
        let stats: benchmark::Stats = serde_json::from_str(
            text.lines()
                .find_map(|line| line.strip_prefix("MAKO_BENCH_RESULT "))
                .unwrap(),
        )
        .unwrap();
        assert_eq!(stats.committed, [20, 0, 0, 0, 0]);
        assert!(
            text.lines().any(|line| line.starts_with("MAKO_LOADED ")
                && line.ends_with(if run == 0 { "false" } else { "true" })),
            "{text}"
        );
        config.persistence.as_mut().unwrap().create = false;
    }
    let node = RunningNode::start(config).unwrap();
    let context = Context {
        schema: &node.schema,
        owner: 0,
        migration: false,
        scale: &settings.scale,
    };
    let mut client = client(&node, BTreeMap::new());
    let mut worker = node.store.database.worker().unwrap();
    client
        .run(node.store.clone(), &mut worker, |tx| {
            let mut orders = 0;
            for district in 1..=settings.scale.districts {
                orders += context
                    .read::<District>(tx, DISTRICT, 1, &key(&[1, district]))?
                    .next_order
                    - settings.scale.customers as i32
                    - 1;
            }
            assert_eq!(orders, 40);
            assert_eq!(context.reserve_dates(tx, 1)?.start, 2 + 2 * 65_536);
            Ok(())
        })
        .unwrap();
}

#[test]
fn micro_workload_loads_shard_local_items_and_aborts_partial_remote_updates() {
    use mako_runtime::benchmark;
    use std::sync::{atomic::AtomicBool, Arc};
    let local = RunningNode::start(config(0, 2)).unwrap();
    let remote = RunningNode::start(config(1, 2)).unwrap();
    let peers = BTreeMap::from([(0, local.address().into()), (1, remote.address().into())]);
    let scale = small();
    for node in [&local, &remote] {
        let context = Context {
            schema: &node.schema,
            owner: node.store.owner,
            migration: false,
            scale: &scale,
        };
        let mut worker = node.store.database.worker().unwrap();
        context
            .load_micro(
                &mut client(node, peers.clone()),
                node.store.clone(),
                &mut worker,
                64,
                |_| Ok(()),
            )
            .unwrap();
    }
    let context = Context {
        schema: &local.schema,
        owner: 0,
        migration: false,
        scale: &scale,
    };
    {
        let mut client = client(&local, peers.clone());
        let mut worker = local.store.database.worker().unwrap();
        client
            .run(local.store.clone(), &mut worker, |tx| {
                assert!(context.all_micro_loaded(tx, 64)?);
                MicroRequest {
                    write: true,
                    keys: vec![(1, 0), (0, 1)],
                }
                .execute(&context, tx)
            })
            .unwrap();
        let failed = client.run(local.store.clone(), &mut worker, |tx| {
            MicroRequest {
                write: true,
                keys: vec![(1, 2), (0, 65)],
            }
            .execute(&context, tx)
        });
        assert_eq!(failed.unwrap_err().code, ErrorCode::Invalid);
        client
            .run(local.store.clone(), &mut worker, |tx| {
                for (owner, id, expected) in [
                    (1, 0, b"aaaaaaaa"),
                    (0, 1, b"aaaaaaaa"),
                    (1, 2, b"cccccccc"),
                ] {
                    let key = key(&[id]);
                    let mut target = context.target(ITEM, 0, &key)?;
                    target.owner = owner;
                    let row = MicroItem::decode(&tx.get(&target, &key)?.unwrap())?;
                    assert_eq!(row.name.0, expected);
                }
                Ok(())
            })
            .unwrap();
        assert!(
            context
                .load_micro(
                    &mut client,
                    local.store.clone(),
                    &mut worker,
                    64,
                    |_| Ok(())
                )
                .unwrap()
                .already_loaded
        );
        assert_eq!(
            context
                .load(&mut client, local.store.clone(), &mut worker, |_| Ok(()))
                .unwrap_err()
                .code,
            ErrorCode::Invalid
        );
    }
    let mut config = config(0, 2);
    config.peers = peers;
    let settings = benchmark::Config {
        scale,
        micro: Some(Micro {
            keys: 64,
            remote_percent: 100,
            ..Micro::default()
        }),
        transactions_per_worker: Some(100),
        seconds: 30,
        ..benchmark::Config::default()
    };
    let host = benchmark::Host::from(&local);
    let stop = Arc::new(AtomicBool::new(false));
    benchmark::prepare(&host, &config, &settings, &stop, |_| Ok(())).unwrap();
    let stats = benchmark::run(&host, &config, &settings, stop).unwrap();
    assert_eq!(stats.workload, "micro");
    assert_eq!(stats.committed.iter().sum::<u64>(), 100);
    assert!(stats.committed[0] > 0 && stats.committed[1] > 0);
    assert_eq!(&stats.committed[2..], &[0, 0, 0]);
}
