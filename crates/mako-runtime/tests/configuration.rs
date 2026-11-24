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
    listen: "{first}"
    warehouses_per_shard: 1
    tables: &tables {tables}
    peers: &peers {{0: "{first}", 1: "{second}"}}
  - owner: 1
    nodes: 2
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
        assert!(std::net::TcpStream::connect_timeout(&address.parse().unwrap(),std::time::Duration::from_millis(200)).is_err());
    }
}
