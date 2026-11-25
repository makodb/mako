#![cfg(feature = "native-storage")]
use mako_runtime::{
    configuration::{read, BenchmarkFile},
    tpcc,
};
use std::process::Command;

fn yaml(addresses: &[String]) -> String {
    format!(
        r#"nodes:
  - owner: 0
    nodes: 2
    volatile: true
    listen: "{first}"
    warehouses_per_shard: 1
    tables: &tables {tables}
    peers: &peers {{0: "{first}", 1: "{second}"}}
  - owner: 1
    nodes: 2
    volatile: true
    listen: "{second}"
    warehouses_per_shard: 1
    tables: *tables
    peers: *peers
benchmark:
  seconds: 30
  transactions_per_worker: 100
  micro: {{keys: 1024, remote_percent: 100}}
"#,
        first = addresses[0],
        second = addresses[1],
        tables = serde_json::to_string(&tpcc::tables()).unwrap()
    )
}

#[test]
fn typed_yaml_anchors_parse_and_invalid_topologies_are_rejected_before_startup() {
    let directory = tempfile::tempdir().unwrap();
    let file = directory.path().join("config.yml");
    let original = yaml(&["127.0.0.1:31000".into(), "127.0.0.1:31001".into()]);
    std::fs::write(&file, &original).unwrap();
    let config: BenchmarkFile = read(&file).unwrap();
    let (nodes, settings) = config.clone().into_parts().unwrap();
    assert_eq!(nodes.len(), 2);
    assert_eq!(nodes[0].tables, nodes[1].tables);
    assert_eq!(nodes[0].peers, nodes[1].peers);
    assert_eq!(settings.micro.unwrap().keys, 1024);
    let mut duplicate = config.clone();
    duplicate.nodes[1].owner = 0;
    assert!(duplicate.into_parts().is_err());
    let mut ambiguous = config.clone();
    ambiguous.node = Some(ambiguous.nodes[0].clone());
    assert!(ambiguous.into_parts().is_err());
    let mut incompatible = config;
    incompatible.nodes[1].workers = 2;
    assert!(incompatible.into_parts().is_err());
    std::fs::write(
        &file,
        original.replace("seconds: 30", "seconds: 30\n  seconds: 50"),
    )
    .unwrap();
    assert!(read::<BenchmarkFile>(&file).is_err());
    std::fs::write(
        &file,
        original.replace(
            "remote_percent: 100",
            "remote_percent: 100, unknown_knob: true",
        ),
    )
    .unwrap();
    assert!(read::<BenchmarkFile>(&file).is_err());
}

#[test]
fn native_executable_runs_two_colocated_shards_from_yaml_and_joins_workers() {
    let sockets: Vec<_> = (0..2)
        .map(|_| std::net::TcpListener::bind("127.0.0.1:0").unwrap())
        .collect();
    let addresses: Vec<_> = sockets
        .iter()
        .map(|socket| socket.local_addr().unwrap().to_string())
        .collect();
    let directory = tempfile::tempdir().unwrap();
    let file = directory.path().join("config.yaml");
    std::fs::write(&file, yaml(&addresses)).unwrap();
    drop(sockets);
    let output = Command::new(env!("CARGO_BIN_EXE_mako_bench"))
        .args(["--config", file.to_str().unwrap(), "--exit-after-run"])
        .output()
        .unwrap();
    assert!(
        output.status.success(),
        "{}\n{}",
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );
    let output = String::from_utf8(output.stdout).unwrap();
    let stats: Vec<mako_runtime::benchmark::Stats> = output
        .lines()
        .filter_map(|line| line.strip_prefix("MAKO_BENCH_RESULT "))
        .map(|json| serde_json::from_str(json).unwrap())
        .collect();
    assert_eq!(stats.len(), 2, "{output}");
    let owners: std::collections::BTreeSet<_> = stats.iter().map(|stats| stats.owner).collect();
    assert_eq!(owners, std::collections::BTreeSet::from([0, 1]));
    for stats in stats {
        assert_eq!(stats.workload, "micro");
        assert!(
            stats.committed[0] > 0 && stats.committed[1] > 0,
            "{stats:?}"
        );
        assert_eq!(
            stats.committed.iter().sum::<u64>() + stats.aborted.iter().sum::<u64>(),
            100
        );
    }
    for address in &addresses {
        assert!(std::net::TcpStream::connect_timeout(
            &address.parse().unwrap(),
            std::time::Duration::from_millis(200)
        )
        .is_err());
    }
}

#[test]
fn existing_topologies_and_raft_fragments_translate_to_reviewable_native_configuration() {
    use mako_runtime::configuration::cli::Args;
    let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../..");
    let directory = tempfile::tempdir().unwrap();
    let data = directory.path().join("not-created-by-conversion");
    let topology = root.join("src/mako/config/local-shards2-warehouses1.yml");
    let raft = root.join("config/1leader_2followers/raft6_shardidx1.yml");
    let mode = root.join("config/raft.yml");
    let arguments = vec![
        "-q".into(),
        topology.to_str().unwrap().into(),
        "-g1".into(),
        "-Pp1".into(),
        "-t2".into(),
        "-Rraft".into(),
        "-F".into(),
        raft.to_str().unwrap().into(),
        "-F".into(),
        mode.to_str().unwrap().into(),
        "--data-dir".into(),
        data.to_str().unwrap().into(),
        "--create".into(),
        "--runtime=4".into(),
        "-C5".into(),
        "-Y50".into(),
    ];
    let (file, notices) = Args::parse(arguments).unwrap().load().unwrap();
    let (nodes, settings) = file.into_parts().unwrap();
    assert_eq!(nodes.len(), 1);
    let node = &nodes[0];
    assert_eq!((node.owner, node.nodes, node.workers), (1, 2, 2));
    assert_eq!(node.listen, "127.0.0.1:32100");
    let raft = node.replication.as_ref().unwrap();
    assert_eq!((raft.group, raft.id, raft.peers.len()), (2, 2, 3));
    assert_eq!(raft.listen, "127.0.0.1:56101");
    assert_eq!(raft.peers[0].address, "127.0.0.1:56001");
    assert_eq!(raft.directory, data.join("replica-1/shard-1"));
    assert_eq!(
        node.replica_groups[&0].endpoints,
        vec!["127.0.0.1:31000", "127.0.0.1:32000", "127.0.0.1:33000"]
    );
    assert_eq!(settings.seconds, 4);
    assert_eq!(settings.cpu_limit_percent, 5.0);
    assert_eq!(settings.throttle_cycle_ms, 50);
    assert!(notices.iter().any(|notice| notice.contains("6 per-worker")));
    assert!(!data.exists());
    for (file, site, expected) in [
        ("mako_single_node.yml", "local_s0", "127.0.0.1:31000"),
        ("mako_new_format.yml", "s0_follower1", "127.0.0.1:32000"),
    ] {
        let arguments = vec![
            "--shard-config".into(),
            root.join("config").join(file).to_str().unwrap().into(),
            "-N".into(),
            site.into(),
            "--data-dir".into(),
            data.to_str().unwrap().into(),
        ];
        let (file, _) = Args::parse(arguments).unwrap().load().unwrap();
        let (nodes, _) = file.into_parts().unwrap();
        assert_eq!(nodes.len(), 1);
        assert_eq!(nodes[0].listen, expected);
        assert_eq!(nodes[0].warehouses_per_shard, 6);
    }
    assert!(Args::parse(["--replication=paxos".into()]).is_err());
    assert!(Args::parse(["--storage-engine=cpp".into()]).is_err());
    assert!(Args::parse(["-g0".into(), "-Lall".into()]).is_err());
    assert!(Args::parse(["--is-micro=true".into()]).is_err());
}

#[test]
fn native_cli_prints_converted_config_without_opening_listeners_or_storage() {
    let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../..");
    let output = Command::new(env!("CARGO_BIN_EXE_mako_bench"))
        .args([
            "-q",
            root.join("config/mako_single_node.yml").to_str().unwrap(),
            "--data-dir",
            "/var/tmp/mako-config-print-only",
            "--print-config",
            "-u2",
            "-E",
            "rust",
        ])
        .output()
        .unwrap();
    assert!(
        output.status.success(),
        "{}",
        String::from_utf8_lossy(&output.stderr)
    );
    let config: BenchmarkFile = serde_json::from_slice(&output.stdout).unwrap();
    let (nodes, settings) = config.into_parts().unwrap();
    assert_eq!(nodes[0].listen, "127.0.0.1:31000");
    assert_eq!(settings.seconds, 2);
}

#[test]
fn production_configuration_requires_local_or_replicated_log_and_volatile_is_explicit() {
    let directory = tempfile::tempdir().unwrap();
    let path = directory.path().join("node.yml");
    std::fs::write(
        &path,
        yaml(&["127.0.0.1:31000".into(), "127.0.0.1:31001".into()]),
    )
    .unwrap();
    let mut file: BenchmarkFile = read(&path).unwrap();
    for node in &mut file.nodes {
        node.volatile = false;
    }
    assert!(file.clone().into_parts().is_err());
    for node in &mut file.nodes {
        node.persistence = Some(mako_runtime::node::PersistenceConfig {
            directory: directory.path().join(format!("data-{}", node.owner)),
            create: true,
            sync_writes: true,
        });
    }
    assert!(file.clone().into_parts().is_ok());
    file.nodes[0].volatile = true;
    assert!(file.into_parts().is_err());
}
