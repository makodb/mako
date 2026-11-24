#![cfg(feature = "native-storage")]
use mako_runtime::{node::Config, redis, transactions};
use std::{
    collections::BTreeMap,
    io::{BufRead, BufReader, Read, Write},
    net::{TcpListener, TcpStream},
    process::{Child, Command, Stdio},
    sync::mpsc,
    time::{Duration, Instant},
};

struct Server {
    child: Child,
    redis: String,
    shard: String,
}
impl Server {
    fn start(directory: &std::path::Path, config: Config) -> Self {
        let path = directory.join(format!("redis-{}.json", config.owner));
        std::fs::write(
            &path,
            serde_json::to_vec(&serde_json::json!({"node":config,"redis_listen":"127.0.0.1:0"}))
                .unwrap(),
        )
        .unwrap();
        let mut child = Command::new(env!("CARGO_BIN_EXE_mako_redis"))
            .args(["--config", path.to_str().unwrap(), "--exit-on-stdin-close"])
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::inherit())
            .spawn()
            .unwrap();
        let stdout = child.stdout.take().unwrap();
        let (send, events) = mpsc::channel();
        std::thread::spawn(move || {
            for line in BufReader::new(stdout).lines() {
                let Ok(line) = line else { break };
                let _ = send.send(line);
            }
        });
        let (mut redis, mut shard) = (None, None);
        while redis.is_none() || shard.is_none() {
            let line = events
                .recv_timeout(Duration::from_secs(30))
                .expect("Redis startup");
            if let Some(address) = line.strip_prefix("MAKO_REDIS_READY ") {
                redis = Some(address.to_owned());
            }
            if let Some(address) = line.strip_prefix("MAKO_READY ") {
                shard = Some(address.to_owned());
            }
        }
        Self {
            child,
            redis: redis.unwrap(),
            shard: shard.unwrap(),
        }
    }
    fn stop(&mut self) {
        self.child.stdin.take();
        let deadline = Instant::now() + Duration::from_secs(15);
        loop {
            if let Some(status) = self.child.try_wait().unwrap() {
                assert!(status.success(), "Redis exit {status}");
                return;
            }
            assert!(
                Instant::now() < deadline,
                "Redis shutdown with idle clients"
            );
            std::thread::sleep(Duration::from_millis(20));
        }
    }
}
impl Drop for Server {
    fn drop(&mut self) {
        if self.child.try_wait().ok().flatten().is_none() {
            let _ = self.child.kill();
            let _ = self.child.wait();
        }
    }
}
fn config(owner: u32, peers: BTreeMap<u32, String>) -> Config {
    Config {
        owner,
        nodes: peers.len() as u32,
        listen: peers[&owner].clone(),
        tables: transactions::tables(),
        workers: 2,
        gateway_workers: 2,
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
fn connect(address: &str) -> BufReader<TcpStream> {
    let stream = TcpStream::connect(address).unwrap();
    stream
        .set_read_timeout(Some(Duration::from_secs(15)))
        .unwrap();
    stream
        .set_write_timeout(Some(Duration::from_secs(15)))
        .unwrap();
    BufReader::new(stream)
}
fn encode(args: &[&[u8]]) -> Vec<u8> {
    let mut bytes = format!("*{}\r\n", args.len()).into_bytes();
    for arg in args {
        bytes.extend_from_slice(format!("${}\r\n", arg.len()).as_bytes());
        bytes.extend_from_slice(arg);
        bytes.extend_from_slice(b"\r\n");
    }
    bytes
}
#[derive(Debug, PartialEq)]
enum Resp {
    Text(String),
    Error(String),
    Bulk(Option<Vec<u8>>),
    Number(i64),
    Array(Option<Vec<Resp>>),
}
fn reply(stream: &mut BufReader<TcpStream>) -> Resp {
    let mut line = String::new();
    stream.read_line(&mut line).unwrap();
    let kind = line.as_bytes()[0];
    let data = line[1..].trim_end_matches("\r\n");
    match kind {
        b'+' => Resp::Text(data.into()),
        b'-' => Resp::Error(data.into()),
        b':' => Resp::Number(data.parse().unwrap()),
        b'$' => {
            let length: i64 = data.parse().unwrap();
            if length == -1 {
                Resp::Bulk(None)
            } else {
                let mut bytes = vec![0; length as usize + 2];
                stream.read_exact(&mut bytes).unwrap();
                assert_eq!(&bytes[length as usize..], b"\r\n");
                bytes.truncate(length as usize);
                Resp::Bulk(Some(bytes))
            }
        }
        b'*' => {
            let length: i64 = data.parse().unwrap();
            Resp::Array((length >= 0).then(|| (0..length).map(|_| reply(stream)).collect()))
        }
        _ => panic!("unexpected RESP {line}"),
    }
}
fn command(stream: &mut BufReader<TcpStream>, args: &[&[u8]]) -> Resp {
    stream.get_mut().write_all(&encode(args)).unwrap();
    reply(stream)
}
fn batch(stream: &mut BufReader<TcpStream>, commands: &[Vec<&[u8]>]) -> Resp {
    assert_eq!(command(stream, &[b"MULTI"]), Resp::Text("OK".into()));
    for args in commands {
        assert_eq!(command(stream, args), Resp::Text("QUEUED".into()));
    }
    command(stream, &[b"EXEC"])
}

#[test]
fn redis_exec_is_atomic_across_shards_and_idle_clients_do_not_own_workers() {
    let directory = tempfile::tempdir().unwrap();
    let mut sockets: Vec<_> = (0..2)
        .map(|_| TcpListener::bind("127.0.0.1:0").unwrap())
        .collect();
    let peers = sockets
        .iter()
        .enumerate()
        .map(|(owner, socket)| (owner as u32, socket.local_addr().unwrap().to_string()))
        .collect::<BTreeMap<_, _>>();
    // Keep the origin port reserved while the remote chooses its Redis port.
    drop(sockets.pop());
    let mut remote = Server::start(directory.path(), config(1, peers.clone()));
    drop(sockets);
    let mut origin = Server::start(directory.path(), config(0, peers));
    let mut idle: Vec<_> = (0..8).map(|_| connect(&origin.redis)).collect();
    for stream in &mut idle {
        // redis-py sends optional metadata before PING. An unsupported command
        // gets an error response without closing an otherwise valid session.
        assert!(matches!(
            command(stream, &[b"CLIENT", b"SETINFO", b"LIB-NAME", b"redis-py"]),
            Resp::Error(_)
        ));
        assert_eq!(command(stream, &[b"PING"]), Resp::Text("PONG".into()));
    }
    let home = (0u32..)
        .map(|key| key.to_be_bytes().to_vec())
        .find(|key| redis::owner(key, 2) == 0)
        .unwrap();
    let away = (0u32..)
        .map(|key| key.to_be_bytes().to_vec())
        .find(|key| redis::owner(key, 2) == 1)
        .unwrap();
    let stream = &mut idle[0];
    let response = batch(
        stream,
        &[
            vec![b"SET", &home, b""],
            vec![b"SET", &away, b""],
            vec![b"GET", &away],
        ],
    );
    assert_eq!(
        response,
        Resp::Array(Some(vec![
            Resp::Text("OK".into()),
            Resp::Text("OK".into()),
            Resp::Bulk(Some(vec![]))
        ]))
    );
    assert_eq!(command(stream, &[b"MULTI"]), Resp::Text("OK".into()));
    assert_eq!(
        command(stream, &[b"SET", &away, b"discarded"]),
        Resp::Text("QUEUED".into())
    );
    assert_eq!(command(stream, &[b"DISCARD"]), Resp::Text("OK".into()));
    assert_eq!(command(stream, &[b"GET", &away]), Resp::Bulk(Some(vec![])));
    assert_eq!(command(stream, &[b"MULTI"]), Resp::Text("OK".into()));
    assert_eq!(
        command(stream, &[b"SET", &home, b"invalid-batch"]),
        Resp::Text("QUEUED".into())
    );
    assert!(matches!(
        command(stream, &[b"SET", &vec![b'k'; 1025], b"bad"]),
        Resp::Error(_)
    ));
    assert!(matches!(command(stream, &[b"EXEC"]), Resp::Error(_)));
    assert_eq!(command(stream, &[b"GET", &home]), Resp::Bulk(Some(vec![])));
    assert_eq!(command(stream, &[b"DEL", &away]), Resp::Number(1));
    assert_eq!(command(stream, &[b"DEL", &away]), Resp::Number(0));
    assert_eq!(
        command(stream, &[b"SET", &away, b""]),
        Resp::Text("OK".into())
    );
    std::thread::scope(|scope| {
        for index in 0..4 {
            let (address, home, away) = (&origin.redis, &home, &away);
            scope.spawn(move || {
                let mut stream = connect(address);
                for round in 0..30 {
                    let value = format!("{index}:{round}");
                    match batch(
                        &mut stream,
                        &[
                            vec![b"SET", home, value.as_bytes()],
                            vec![b"SET", away, value.as_bytes()],
                        ],
                    ) {
                        Resp::Array(Some(result)) => assert_eq!(result.len(), 2),
                        Resp::Array(None) => (),
                        response => panic!("{response:?}"),
                    }
                    match batch(&mut stream, &[vec![b"GET", home], vec![b"GET", away]]) {
                        Resp::Array(Some(result)) => {
                            assert_eq!(result.len(), 2);
                            assert_eq!(result[0], result[1]);
                        }
                        Resp::Array(None) => (),
                        response => panic!("{response:?}"),
                    }
                }
            });
        }
    });
    // A fragmented request and a pipeline longer than one dispatch quota must
    // finish even after the peer closes only its write side.
    let mut pipeline = connect(&origin.redis);
    let bytes = encode(&[b"PING", b"binary\0message"]);
    for part in bytes.chunks(3) {
        pipeline.get_mut().write_all(part).unwrap();
    }
    assert_eq!(
        reply(&mut pipeline),
        Resp::Bulk(Some(b"binary\0message".to_vec()))
    );
    for _ in 0..80 {
        pipeline.get_mut().write_all(&encode(&[b"PING"])).unwrap();
    }
    pipeline
        .get_mut()
        .shutdown(std::net::Shutdown::Write)
        .unwrap();
    for _ in 0..80 {
        assert_eq!(reply(&mut pipeline), Resp::Text("PONG".into()));
    }
    origin.stop();
    remote.stop();
}

#[test]
fn native_gateway_cli_roundtrips_binary_values_and_session_contract() {
    let directory = tempfile::tempdir().unwrap();
    let mut server = Server::start(
        directory.path(),
        config(0, BTreeMap::from([(0, "127.0.0.1:0".into())])),
    );
    let requests = [
        serde_json::json!({"op":"put","table":"records","warehouse":1,"key":[0,255],"value":[]}),
        serde_json::json!({"op":"get","table":"records","warehouse":1,"key":[0,255]}),
        serde_json::json!({"op":"insert","table":"records","warehouse":1,"key":[0,255],"value":[1]}),
        serde_json::json!({"op":"remove","table":"records","warehouse":1,"key":[0,255]}),
        serde_json::json!({"op":"remove","table":"records","warehouse":1,"key":[0,255]}),
        serde_json::json!({"op":"begin"}),
        serde_json::json!({"op":"put","table":"records","warehouse":1,"key":[0,255],"value":[255,0]}),
        serde_json::json!({"op":"rollback"}),
        serde_json::json!({"op":"get","table":"records","warehouse":1,"key":[0,255]}),
    ];
    let mut child = Command::new(env!("CARGO_BIN_EXE_mako_client"))
        .args(["--address", &server.shard])
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::inherit())
        .spawn()
        .unwrap();
    let mut input = child.stdin.take().unwrap();
    for request in requests {
        writeln!(input, "{request}").unwrap();
    }
    drop(input);
    let output = child.wait_with_output().unwrap();
    assert!(
        output.status.success(),
        "gateway CLI output: {}",
        String::from_utf8_lossy(&output.stdout)
    );
    let results: Vec<serde_json::Value> = String::from_utf8(output.stdout)
        .unwrap()
        .lines()
        .map(|line| serde_json::from_str(line).unwrap())
        .collect();
    assert_eq!(
        results,
        vec![
            serde_json::json!({"ok":true}),
            serde_json::json!({"value":[]}),
            serde_json::json!({"inserted":false}),
            serde_json::json!({"removed":true}),
            serde_json::json!({"removed":false}),
            serde_json::json!({"ok":true}),
            serde_json::json!({"ok":true}),
            serde_json::json!({"ok":true}),
            serde_json::json!({"value":[255,0]})
        ]
    );
    server.stop();
}
