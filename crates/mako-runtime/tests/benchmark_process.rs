#![cfg(feature = "replication")]
use mako_runtime::{
    benchmark,
    gateway_client::GatewayClient,
    node::{Config, ReplicaConfig, ReplicaPeer},
    rpc::Network,
    tpcc::{
        self,
        codec::{key, District, Record},
        Mix, Scale,
    },
    wire::*,
};
use std::{
    collections::BTreeMap,
    io::{BufRead, BufReader},
    process::{Child, Command, Stdio},
    sync::{mpsc, Arc},
    time::{Duration, Instant},
};

struct Process(Child);
impl Drop for Process {
    fn drop(&mut self) {
        self.0.stdin.take();
        let deadline = Instant::now() + Duration::from_secs(5);
        while self.0.try_wait().unwrap().is_none() {
            if Instant::now() >= deadline {
                let _ = self.0.kill();
                let _ = self.0.wait();
                break;
            }
            std::thread::sleep(Duration::from_millis(10));
        }
    }
}

#[test]
fn two_replicated_shards_run_native_tpcc_and_retain_committed_order_counts() {
    let directory = tempfile::tempdir().unwrap();
    let sockets: Vec<_> = (0..12)
        .map(|_| std::net::TcpListener::bind("127.0.0.1:0").unwrap())
        .collect();
    let addresses: Vec<_> = sockets
        .iter()
        .map(|socket| socket.local_addr().unwrap().to_string())
        .collect();
    let groups: BTreeMap<_, _> = (0..2)
        .map(|owner| {
            (
                owner as u32,
                ReplicaSet {
                    group: owner as u64 + 700,
                    endpoints: addresses[owner * 3..owner * 3 + 3].to_vec(),
                },
            )
        })
        .collect();
    let peers = BTreeMap::from([(0, addresses[0].clone()), (1, addresses[3].clone())]);
    let settings = benchmark::Config {
        scale: Scale {
            districts: 2,
            customers: 10,
            items: 20,
            seed: 51,
        },
        seconds: 60,
        transactions_per_worker: Some(40),
        mix: Mix {
            weights: [100, 0, 0, 0, 0],
            new_order_remote_percent: 100,
            ..Mix::default()
        },
        startup_timeout_seconds: 60,
        ..benchmark::Config::default()
    };
    let (send, events) = mpsc::channel();
    let mut processes = Vec::new();
    drop(sockets);
    for owner in 0..2usize {
        for replica in 0..3usize {
            let index = owner * 3 + replica;
            let node = Config {
                owner: owner as u32,
                nodes: 2,
                listen: addresses[index].clone(),
                tables: tpcc::tables(),
                workers: 1,
                gateway_workers: 1,
                queue_capacity: 64,
                warehouses_per_shard: 1,
                peers: peers.clone(),
                migration: false,
                initial: vec![],
                persistence: None,
                replica_groups: groups.clone(),
                replication: Some(ReplicaConfig {
                    group: owner as u64 + 700,
                    id: replica as u16 + 1,
                    listen: addresses[index + 6].clone(),
                    directory: directory.path().join(format!("data-{index}")),
                    create: true,
                    sync_writes: true,
                    peers: (0..3)
                        .map(|id| ReplicaPeer {
                            id: id as u16 + 1,
                            address: addresses[owner * 3 + id + 6].clone(),
                        })
                        .collect(),
                }),
            };
            let file = directory.path().join(format!("config-{index}.json"));
            std::fs::write(
                &file,
                serde_json::to_vec(&serde_json::json!({"node":node,"benchmark":settings})).unwrap(),
            )
            .unwrap();
            let stderr =
                std::fs::File::create(directory.path().join(format!("stderr-{index}"))).unwrap();
            let mut child = Command::new(env!("CARGO_BIN_EXE_mako_bench"))
                .args(["--config", file.to_str().unwrap(), "--exit-on-stdin-close"])
                .stdin(Stdio::piped())
                .stdout(Stdio::piped())
                .stderr(stderr)
                .spawn()
                .unwrap();
            let stdout = child.stdout.take().unwrap();
            let send = send.clone();
            std::thread::spawn(move || {
                for line in BufReader::new(stdout).lines() {
                    let Ok(line) = line else {
                        break;
                    };
                    if send.send((owner, index, line)).is_err() {
                        break;
                    }
                }
            });
            processes.push(Process(child));
        }
    }
    let deadline = Instant::now() + Duration::from_secs(100);
    let mut results = BTreeMap::new();
    let mut done = std::collections::BTreeSet::new();
    while done.len() != 2 {
        assert!(
            Instant::now() < deadline,
            "replicated benchmark completion deadline; results={results:?}"
        );
        if let Ok((owner, _, line)) = events.recv_timeout(Duration::from_millis(100)) {
            if let Some(json) = line.strip_prefix("MAKO_BENCH_RESULT ") {
                let stats: benchmark::Stats = serde_json::from_str(json).unwrap();
                assert!(stats.committed[0] > 0, "{stats:?}");
                assert_eq!(stats.committed[0] + stats.aborted[0], 40);
                results.insert(owner, stats);
            }
            if line == "MAKO_BENCH_DONE" {
                done.insert(owner);
            }
        }
        for (index, process) in processes.iter_mut().enumerate() {
            if let Some(status) = process.0.try_wait().unwrap() {
                panic!(
                    "replica {index} exited: {status}\n{}",
                    std::fs::read_to_string(directory.path().join(format!("stderr-{index}")))
                        .unwrap()
                );
            }
        }
    }
    let network = Arc::new(Network::default());
    for owner in 0..2u32 {
        let group = &groups[&owner];
        let endpoint = network
            .find_leader(owner, group, Duration::from_secs(5))
            .unwrap();
        let mut client = GatewayClient::connect(endpoint, Duration::from_secs(5)).unwrap();
        let mut orders = 0;
        for district in 1..=2 {
            let key = key(&[1, district]);
            let target = client
                .resolve(RouteRequest {
                    table: None,
                    name: Some("district".into()),
                    warehouse: owner + 1,
                    key: key.clone(),
                })
                .unwrap();
            let value = client.get(target, &key).unwrap().unwrap();
            orders += District::decode(&value).unwrap().next_order as u64 - 11;
        }
        assert_eq!(orders, results[&(owner as usize)].committed[0]);
    }
    for process in &mut processes {
        process.0.stdin.take();
    }
    let deadline = Instant::now() + Duration::from_secs(15);
    for process in &mut processes {
        loop {
            if let Some(status) = process.0.try_wait().unwrap() {
                assert!(status.success());
                break;
            }
            assert!(
                Instant::now() < deadline,
                "native benchmark shutdown deadline"
            );
            std::thread::sleep(Duration::from_millis(20));
        }
    }
}
