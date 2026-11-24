#![cfg(feature = "native-storage")]
use mako_runtime::{
    node::{Config, PersistenceConfig},
    transactions::{self, File, Settings, Stats},
};
use std::{
    collections::BTreeMap,
    io::{BufRead, BufReader},
    process::{Child, Command, Stdio},
    sync::mpsc,
    time::{Duration, Instant},
};

struct Process {
    child: Child,
    output: mpsc::Receiver<String>,
}
impl Process {
    fn start(directory: &std::path::Path, index: usize, file: File, exit: bool) -> Self {
        let path = directory.join(format!("config-{index}.json"));
        std::fs::write(&path, serde_json::to_vec(&file).unwrap()).unwrap();
        let mut command = Command::new(env!("CARGO_BIN_EXE_mako_transactions"));
        command.args(["--config", path.to_str().unwrap(), "--exit-on-stdin-close"]);
        if exit {
            command.arg("--exit-after-run");
        }
        let mut child = command
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::inherit())
            .spawn()
            .unwrap();
        let stdout = child.stdout.take().unwrap();
        let (send, output) = mpsc::channel();
        std::thread::spawn(move || {
            for line in BufReader::new(stdout).lines() {
                let Ok(line) = line else { break };
                if send.send(line).is_err() {
                    break;
                }
            }
        });
        Self { child, output }
    }
    fn completed(&mut self) -> Stats {
        let deadline = Instant::now() + Duration::from_secs(100);
        let mut stats = None;
        loop {
            let line = self
                .output
                .recv_timeout(deadline.saturating_duration_since(Instant::now()))
                .expect("transaction example completion");
            if let Some(json) = line.strip_prefix("MAKO_TRANSACTIONS_RESULT ") {
                stats = Some(serde_json::from_str(json).unwrap());
            }
            if line == "MAKO_TRANSACTIONS_DONE" {
                return stats.expect("result before completion");
            }
        }
    }
    fn stop(&mut self) {
        self.child.stdin.take();
        let deadline = Instant::now() + Duration::from_secs(15);
        loop {
            if let Some(status) = self.child.try_wait().unwrap() {
                assert!(status.success(), "example status {status}");
                break;
            }
            assert!(Instant::now() < deadline, "example shutdown deadline");
            std::thread::sleep(Duration::from_millis(10));
        }
    }
}
impl Drop for Process {
    fn drop(&mut self) {
        if self.child.try_wait().ok().flatten().is_none() {
            let _ = self.child.kill();
            let _ = self.child.wait();
        }
    }
}
fn node(owner: u32, peers: BTreeMap<u32, String>) -> Config {
    Config {
        owner,
        nodes: peers.len() as u32,
        listen: peers[&owner].clone(),
        tables: transactions::tables(),
        workers: 3,
        gateway_workers: 1,
        queue_capacity: 64,
        warehouses_per_shard: 1,
        peers,
        migration: false,
        initial: vec![],
        replication: None,
        persistence: None,
        replica_groups: BTreeMap::new(),
    }
}
fn file(config: Config) -> File {
    File {
        node: Some(config),
        nodes: vec![],
        transactions: Settings {
            rounds_per_worker: 20,
            ..Settings::default()
        },
    }
}
fn verify(stats: &Stats, shards: u32, initial: u64) {
    assert_eq!(stats.shards, shards);
    assert_eq!(stats.workers, 3);
    assert_eq!(stats.committed, 60);
    assert_eq!(stats.initial_counter, initial);
    assert_eq!(stats.final_counter, initial + 60);
}

#[test]
#[cfg(feature = "replication")]
fn local_transaction_example_reopens_persistent_data() {
    let directory = tempfile::tempdir().unwrap();
    let mut config = node(0, BTreeMap::from([(0, "127.0.0.1:0".into())]));
    config.persistence = Some(PersistenceConfig {
        directory: directory.path().join("data"),
        create: true,
        sync_writes: true,
    });
    let mut first = Process::start(directory.path(), 0, file(config.clone()), true);
    verify(&first.completed(), 1, 0);
    first.stop();
    config.persistence.as_mut().unwrap().create = false;
    let mut second = Process::start(directory.path(), 1, file(config), true);
    verify(&second.completed(), 1, 60);
    second.stop();
}

#[test]
fn separate_shards_run_transaction_example() {
    let directory = tempfile::tempdir().unwrap();
    let sockets: Vec<_> = (0..2)
        .map(|_| std::net::TcpListener::bind("127.0.0.1:0").unwrap())
        .collect();
    let peers: BTreeMap<_, _> = sockets
        .iter()
        .enumerate()
        .map(|(i, socket)| (i as u32, socket.local_addr().unwrap().to_string()))
        .collect();
    drop(sockets);
    let mut remote = Process::start(directory.path(), 1, file(node(1, peers.clone())), false);
    let mut origin = Process::start(directory.path(), 0, file(node(0, peers)), false);
    verify(&origin.completed(), 2, 0);
    origin.stop();
    remote.stop();
}

#[test]
#[cfg(feature = "replication")]
fn replicated_shards_run_transaction_example() {
    use mako_runtime::{
        node::{ReplicaConfig, ReplicaPeer},
        wire::ReplicaSet,
    };
    let directory = tempfile::tempdir().unwrap();
    let sockets: Vec<_> = (0..12)
        .map(|_| std::net::TcpListener::bind("127.0.0.1:0").unwrap())
        .collect();
    let addresses: Vec<_> = sockets
        .iter()
        .map(|socket| socket.local_addr().unwrap().to_string())
        .collect();
    let peers = BTreeMap::from([(0, addresses[0].clone()), (1, addresses[3].clone())]);
    let groups: BTreeMap<_, _> = (0..2usize)
        .map(|owner| {
            (
                owner as u32,
                ReplicaSet {
                    group: owner as u64 + 900,
                    endpoints: addresses[owner * 3..owner * 3 + 3].to_vec(),
                },
            )
        })
        .collect();
    drop(sockets);
    let mut processes = Vec::new();
    for owner in 0..2usize {
        for replica in 0..3usize {
            let index = owner * 3 + replica;
            let mut config = node(owner as u32, peers.clone());
            config.listen = addresses[index].clone();
            config.replica_groups = groups.clone();
            config.replication = Some(ReplicaConfig {
                group: owner as u64 + 900,
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
            });
            processes.push(Process::start(directory.path(), index, file(config), false));
        }
    }
    let deadline = Instant::now() + Duration::from_secs(100);
    let mut result: Option<Stats> = None;
    let mut completed = false;
    while !completed {
        assert!(Instant::now() < deadline, "replicated example deadline");
        for process in &mut processes {
            while let Ok(line) = process.output.try_recv() {
                if let Some(json) = line.strip_prefix("MAKO_TRANSACTIONS_RESULT ") {
                    result = Some(serde_json::from_str(json).unwrap());
                }
                completed |= line == "MAKO_TRANSACTIONS_DONE";
            }
            assert!(
                process.child.try_wait().unwrap().is_none(),
                "replica exited early"
            );
        }
        std::thread::sleep(Duration::from_millis(20));
    }
    verify(&result.unwrap(), 2, 0);
    for process in &mut processes {
        process.child.stdin.take();
    }
    for process in &mut processes {
        process.stop();
    }
}
