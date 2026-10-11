//! Run in Docker: cargo run --locked --manifest-path src/cluster/Cargo.toml
//! --example durable_recovery. Uses a reference file backend, not Mako's TP.
use mako_sharding as native;
#[path = "../durable_test_store.rs"]
mod store;
#[path = "../durable_test_engine.rs"]
mod engine;
#[path = "../durable_test_protocol.rs"]
mod protocol;
use native::types::{Grant, KeyRange, Phase, Status, TxnId};
use std::io::{BufRead, BufReader, Write};
use std::os::unix::process::ExitStatusExt;
use std::path::Path;
use std::process::{Command, Stdio};

fn crash_child(root: &Path, orphan: bool) -> ! {
    let mut cluster = protocol::Cluster::open(root, true);
    cluster.seed();
    if orphan {
        // The parent never receives this identity. Only the journal retains it.
        let id = TxnId { client: u64::from(std::process::id()) << 1, sequence: 1 };
        let owner = &mut cluster.owners[0];
        assert!(owner.participant.begin(&mut owner.kv, &owner.admission, id).ok().unwrap() == Status::Ok);
        owner.kv.fault = store::Fault::UnknownDelayedCommit;
        assert!(owner.participant.acquire(&mut owner.kv, &owner.admission, id,
            KeyRange { table: protocol::TABLE, lo: b"m\0".to_vec(), hi: None }, true,
            Grant { owner: 0, ownership_version: 0 }).is_err());
        println!("CRASH_POINT lease=unknown engine-work=absent identity=journal-only");
        std::io::stdout().flush().unwrap();
        loop { std::thread::park(); }
    }
    assert_eq!(cluster.begin(protocol::NONCE, 0, 1).unwrap(), 1);
    for _ in 0..24 {
        if cluster.phase() == Some(Phase::Committed) { break; }
        assert!(cluster.drive_one().unwrap());
    }
    assert!(cluster.phase() == Some(Phase::Committed));
    assert_eq!(cluster.route(b"m\0").owner, 1);
    cluster.owners[0].engine.row_fault = store::Fault::UnknownDelayedCommit;
    assert!(cluster.drive_one().is_err());
    assert!(cluster.owners[0].engine.value(protocol::TABLE, b"m\0", b"m\0").is_some());
    println!("CRASH_POINT committed=1 cleanup=unknown pending-row=durable");
    std::io::stdout().flush().unwrap();
    loop { std::thread::park(); }
}

fn kill_child(root: &Path, orphan: bool) {
    let mut child = Command::new(std::env::current_exe().unwrap())
        .arg("--crash-child").arg(root).arg(if orphan { "orphan" } else { "cleanup" })
        .stdout(Stdio::piped()).stderr(Stdio::inherit()).spawn().unwrap();
    let mut output = BufReader::new(child.stdout.take().unwrap());
    let mut line = String::new();
    loop {
        line.clear();
        assert_ne!(output.read_line(&mut line).unwrap(), 0, "child exited before crash point");
        print!("{line}");
        if line.starts_with("CRASH_POINT ") { break; }
    }
    child.kill().unwrap();
    let status = child.wait().unwrap();
    assert_eq!(status.signal(), Some(9));
    println!("child terminated: {status}");
}

fn main() {
    assert!(Path::new("/.dockerenv").exists() || Path::new("/run/.containerenv").exists(),
        "Run the durable recovery example in Docker");
    let arguments: Vec<_> = std::env::args_os().collect();
    if arguments.len() == 4 && arguments[1] == "--crash-child" {
        assert!(arguments[3] == "orphan" || arguments[3] == "cleanup");
        crash_child(Path::new(&arguments[2]), arguments[3] == "orphan");
    }
    assert_eq!(arguments.len(), 1);
    for orphan in [false, true] {
        let directory = store::Directory::new();
        kill_child(directory.path(), orphan);
        let mut cluster = protocol::Cluster::open(directory.path(), false);
        if orphan {
            assert!(cluster.phase().is_none());
            let owner = &mut cluster.owners[0];
            let (id, _) = owner.participant.next_recovery_transaction(&mut owner.kv, &owner.admission, 0)
                .ok().unwrap().expect("orphan identity recovered from committed bytes");
            assert!(owner.participant.finish(&mut owner.kv, &mut owner.engine, &owner.admission, id)
                .ok().unwrap() == Status::Ok);
            assert!(owner.participant.next_recovery_transaction(&mut owner.kv, &owner.admission, 0)
                .ok().unwrap().is_none());
            assert!(!owner.engine.transaction(id, Some(true),
                vec![(protocol::TABLE, protocol::row(b"m\0", b"late-orphan-write"), false)]));
            assert_eq!(cluster.begin(protocol::NONCE, 0, 1).unwrap(), 1);
        } else {
            assert!(cluster.phase() == Some(Phase::Committed));
            assert_eq!(cluster.route(b"m\0").owner, 1);
            // Recovery settled accepted deletion before installing admission.
            assert!(cluster.owners[0].engine.value(protocol::TABLE, b"m\0", b"m\0").is_none());
        }
        cluster.finish().unwrap();
        cluster.check_destination(1);
        if orphan {
            println!("PASS: SIGKILL, journal-only orphan discovery, durable identity fence, resumed drain and migration");
        } else {
            assert_eq!(cluster.begin(protocol::NONCE, 0, 1).unwrap(), 1);
            assert_eq!(cluster.begin(TxnId { client: protocol::NONCE.client, sequence: 2 }, 1, 0).unwrap(), 2);
            cluster.finish().unwrap();
            cluster.check_destination(0);
            println!("PASS: SIGKILL, accepted unknown delete, byte-only recovery, retained decision/nonce, phase-specific cleanup, canonical rows, ownership return");
        }
    }
    println!("Boundary: reference file-backed KV/engine; production MBTA/TP remains live-only");
}
