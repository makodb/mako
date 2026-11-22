//! Real SRPC sockets, existing raft-core elections and existing raft-store WAL.
//! Run inside Docker: cargo test --manifest-path crates/Cargo.toml -p raft-host --test protocol -- --test-threads=1
use parking_lot::Mutex;
use raft_host::{AdmissionError, Config, Host, Peer, Position, ProposalStatus, StateMachine};
use std::fs::{self, File};
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::sync::{
    atomic::{AtomicBool, AtomicU64, Ordering},
    Arc,
};
use std::time::{Duration, Instant};

static NEXT: AtomicU64 = AtomicU64::new(0);
struct Directory(PathBuf);
impl Directory {
    fn new() -> Self {
        let path = std::env::temp_dir().join(format!(
            "raft-host-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        ));
        fs::create_dir_all(&path).unwrap();
        Self(path)
    }
}
impl Drop for Directory {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

/// An actual durable append state machine used only by the protocol tests.
/// Position plus content are published by a synced atomic rename.
struct Machine {
    path: PathBuf,
    state: Mutex<(Position, Vec<u8>)>,
    fail: AtomicBool,
    block_apply: AtomicBool,
}
impl Machine {
    fn open(path: PathBuf) -> Arc<Self> {
        fs::create_dir_all(&path).unwrap();
        let state = if path.join("state").exists() {
            read_state(&path.join("state")).unwrap()
        } else {
            (Position::default(), Vec::new())
        };
        Arc::new(Self {
            path,
            state: Mutex::new(state),
            fail: AtomicBool::new(false),
            block_apply: AtomicBool::new(false),
        })
    }
    fn bytes(&self) -> Vec<u8> {
        self.state.lock().1.clone()
    }
    fn save(&self, position: Position, bytes: &[u8]) -> Result<(), String> {
        let mut file = File::create(self.path.join("state.next")).map_err(err)?;
        file.write_all(&position.index.to_le_bytes())
            .and_then(|_| file.write_all(&position.term.to_le_bytes()))
            .and_then(|_| file.write_all(bytes))
            .and_then(|_| file.sync_all())
            .map_err(err)?;
        fs::rename(self.path.join("state.next"), self.path.join("state")).map_err(err)?;
        File::open(&self.path)
            .and_then(|f| f.sync_all())
            .map_err(err)
    }
}
fn read_state(path: &Path) -> Result<(Position, Vec<u8>), String> {
    let mut file = File::open(path).map_err(err)?;
    let mut header = [0u8; 16];
    file.read_exact(&mut header).map_err(err)?;
    let position = Position {
        index: u64::from_le_bytes(header[..8].try_into().unwrap()),
        term: u64::from_le_bytes(header[8..].try_into().unwrap()),
    };
    let mut bytes = Vec::new();
    file.read_to_end(&mut bytes).map_err(err)?;
    Ok((position, bytes))
}
fn err(e: impl std::fmt::Display) -> String {
    e.to_string()
}
impl StateMachine for Machine {
    fn applied_position(&self) -> Result<Position, String> {
        Ok(self.state.lock().0)
    }
    fn apply(&self, position: Position, command: Option<&[u8]>) -> Result<(), String> {
        while self.block_apply.load(Ordering::SeqCst) {
            std::thread::sleep(Duration::from_millis(2));
        }
        if self.fail.load(Ordering::SeqCst) {
            return Err("injected durable application I/O failure".into());
        }
        let mut state = self.state.lock();
        if position.index != state.0.index + 1 {
            return Err("noncontiguous application".into());
        }
        let mut bytes = state.1.clone();
        if let Some(command) = command {
            bytes.extend_from_slice(command);
        }
        self.save(position, &bytes)?;
        *state = (position, bytes);
        Ok(())
    }
    fn create_checkpoint(&self, destination: &Path, position: Position) -> Result<(), String> {
        let state = self.state.lock();
        if state.0 != position {
            return Err("inexact checkpoint cut".into());
        }
        fs::create_dir(destination).map_err(err)?;
        fs::copy(self.path.join("state"), destination.join("state")).map_err(err)?;
        // A large immutable native file exercises transfer beyond one frame.
        if self.path.join("large-file").exists() {
            fs::copy(self.path.join("large-file"), destination.join("large-file")).map_err(err)?;
        }
        Ok(())
    }
    fn validate_checkpoint(&self, directory: &Path, position: Position) -> Result<(), String> {
        if read_state(&directory.join("state"))?.0 != position {
            return Err("checkpoint position mismatch".into());
        }
        Ok(())
    }
    fn publish_checkpoint(&self, directory: &Path, position: Position) -> Result<(), String> {
        let (_, bytes) = read_state(&directory.join("state"))?;
        let mut state = self.state.lock();
        self.save(position, &bytes)?;
        if directory.join("large-file").exists() {
            fs::copy(directory.join("large-file"), self.path.join("large-file")).map_err(err)?;
        }
        *state = (position, bytes);
        Ok(())
    }
}
fn config(root: &Path, group: u64, n: u16, peers: &[Peer], create: bool) -> Config {
    Config {
        group,
        node: n,
        listen: peers[n as usize - 1].address.clone(),
        peers: peers.to_vec(),
        directory: root.join(format!("raft-{n}")),
        create,
        election_timeout: Duration::from_millis(300),
        heartbeat_interval: Duration::from_millis(30),
        rpc_timeout: Duration::from_millis(200),
        snapshot_chunk_bytes: 256 << 10,
        ..Config::default()
    }
}
fn peers(n: u16) -> Vec<Peer> {
    let listeners = (1..=n)
        .map(|_| std::net::TcpListener::bind("127.0.0.1:0").unwrap())
        .collect::<Vec<_>>();
    listeners
        .iter()
        .enumerate()
        .map(|(i, l)| Peer {
            id: i as u16 + 1,
            address: l.local_addr().unwrap().to_string(),
        })
        .collect()
}
fn until(mut predicate: impl FnMut() -> bool) {
    let deadline = Instant::now() + Duration::from_secs(30);
    while !predicate() {
        assert!(Instant::now() < deadline, "protocol wait expired");
        std::thread::sleep(Duration::from_millis(10));
    }
}
fn leader(hosts: &[Option<Host>]) -> usize {
    let mut result = None;
    until(|| {
        let statuses = hosts
            .iter()
            .map(|host| host.as_ref().map(Host::status))
            .collect::<Vec<_>>();
        result = statuses.iter().enumerate().find_map(|(index, status)| {
            let status = status.as_ref()?;
            let id = index as u16 + 1;
            (status.is_leader
                && statuses.iter().flatten().all(|peer| {
                    peer.failure.is_none() && peer.term == status.term && peer.leader == Some(id)
                }))
            .then_some(index)
        });
        result.is_some()
    });
    result.unwrap()
}

/// `wait` reports stepdown uncertainty immediately; uncertainty is not a final
/// decision and never authorizes replaying these append-only test commands.
fn applied(proposal: &raft_host::Proposal, hosts: &[Option<Host>]) {
    let deadline = Instant::now() + Duration::from_secs(10);
    loop {
        match proposal.status() {
            ProposalStatus::Applied => return,
            ProposalStatus::Discarded | ProposalStatus::StorageFailed(_) => {
                panic!("proposal {:?}: {:?}", proposal.position, proposal.status())
            }
            _ => {}
        }
        assert!(
            Instant::now() < deadline,
            "proposal {:?} unresolved: {:?}; replicas: {:?}",
            proposal.position,
            proposal.status(),
            hosts
                .iter()
                .map(|h| h.as_ref().map(Host::status))
                .collect::<Vec<_>>()
        );
        std::thread::sleep(Duration::from_millis(10));
    }
}

#[test]
fn quorum_commit_noops_read_barrier_failover_and_recovery() {
    let root = Directory::new();
    let members = peers(3);
    let group = (1u64 << 48) + 23;
    let machines = (1..=3)
        .map(|n| Machine::open(root.0.join(format!("app-{n}"))))
        .collect::<Vec<_>>();
    let mut hosts = (1..=3)
        .map(|n| {
            Some(
                Host::start(
                    config(&root.0, group, n, &members, true),
                    machines[n as usize - 1].clone(),
                )
                .unwrap(),
            )
        })
        .collect::<Vec<_>>();
    let original = leader(&hosts);
    let host = hosts[original].as_ref().unwrap();
    let term = host.status().term;
    let barrier = host.read_barrier(term, Duration::from_secs(10)).unwrap();
    assert_eq!(barrier.term, term);
    let proposals = (0..12u8)
        .map(|v| host.propose(term, vec![v]).unwrap())
        .collect::<Vec<_>>();
    for proposal in &proposals {
        applied(proposal, &hosts);
    }
    until(|| {
        machines
            .iter()
            .all(|m| m.bytes() == (0..12u8).collect::<Vec<_>>())
    });
    let before = hosts[original].as_ref().unwrap().status().applied.index;
    let current = leader(&hosts);
    let host = hosts[current].as_ref().unwrap();
    let current_term = host.status().term;
    let second = host
        .read_barrier(current_term, Duration::from_secs(10))
        .unwrap_or_else(|error| {
            panic!(
                "second read barrier failed: {error:?}; replicas: {:?}",
                hosts
                    .iter()
                    .map(|h| h.as_ref().map(Host::status))
                    .collect::<Vec<_>>()
            )
        });
    assert!(second.index >= before);
    assert!(matches!(
        host.propose(current_term + 1, vec![90]),
        Err(AdmissionError::NotLeader { .. })
    ));
    hosts[current].take().unwrap().shutdown();
    let replacement = leader(&hosts);
    let host = hosts[replacement].as_ref().unwrap();
    let next_term = host.status().term;
    assert!(next_term > current_term);
    applied(&host.propose(next_term, vec![99]).unwrap(), &hosts);
    let reopened = Machine::open(root.0.join(format!("app-{}", current + 1)));
    hosts[current] = Some(
        Host::start(
            config(&root.0, group, current as u16 + 1, &members, false),
            reopened.clone(),
        )
        .unwrap(),
    );
    until(|| reopened.bytes() == machines[replacement].bytes());
    for host in hosts.into_iter().flatten() {
        host.shutdown();
    }
}

#[test]
fn application_error_fences_without_advancing_and_recovery_replays_authority() {
    let root = Directory::new();
    let members = peers(1);
    let machine = Machine::open(root.0.join("app"));
    let cfg = config(&root.0, 55, 1, &members, true);
    let host = Host::start(cfg.clone(), machine.clone()).unwrap();
    until(|| host.status().is_leader);
    let term = host.status().term;
    host.read_barrier(term, Duration::from_secs(10)).unwrap();
    let applied = host.status().applied;
    machine.fail.store(true, Ordering::SeqCst);
    let proposal = host
        .propose(term, b"durable-before-application".to_vec())
        .unwrap();
    assert!(matches!(
        proposal.wait(Duration::from_secs(10)),
        ProposalStatus::StorageFailed(_)
    ));
    until(|| host.status().failure.is_some());
    assert_eq!(host.status().applied, applied);
    assert!(host.status().committed.index >= proposal.position.index);
    assert!(matches!(
        host.propose(term, vec![1]),
        Err(AdmissionError::Unavailable(_))
    ));
    host.shutdown();
    drop(host);
    drop(machine);
    let recovered = Machine::open(root.0.join("app"));
    let restarted = Host::start(
        Config {
            create: false,
            ..cfg
        },
        recovered.clone(),
    )
    .unwrap();
    assert_eq!(recovered.bytes(), b"durable-before-application");
    assert!(restarted.status().applied.index >= proposal.position.index);
    restarted.shutdown();
}

#[test]
fn snapshot_over_single_frame_recovers_lagging_replica_after_compaction() {
    let root = Directory::new();
    let members = peers(3);
    let group = 77;
    let machines = (1..=3)
        .map(|n| Machine::open(root.0.join(format!("app-{n}"))))
        .collect::<Vec<_>>();
    let mut hosts = (1..=3)
        .map(|n| {
            Some(
                Host::start(
                    config(&root.0, group, n, &members, true),
                    machines[n as usize - 1].clone(),
                )
                .unwrap(),
            )
        })
        .collect::<Vec<_>>();
    let current = leader(&hosts);
    let offline = (current + 1) % 3;
    hosts[offline].take().unwrap().shutdown();
    let current = leader(&hosts);
    let host = hosts[current].as_ref().unwrap();
    let term = host.status().term;
    applied(
        &host.propose(term, b"checkpoint-row".to_vec()).unwrap(),
        &hosts,
    );
    until(|| {
        hosts
            .iter()
            .enumerate()
            .filter(|(_, h)| h.is_some())
            .all(|(i, _)| machines[i].bytes() == b"checkpoint-row")
    });
    // Every possible leader must compact the prefix. Compacting just one lets
    // a perfectly valid election bypass the entire snapshot protocol via logs.
    let block = vec![0xa7u8; 1 << 20];
    let mut cut = Position::default();
    for (index, host) in hosts
        .iter()
        .enumerate()
        .filter_map(|(i, h)| h.as_ref().map(|h| (i, h)))
    {
        let mut file = File::create(machines[index].path.join("large-file")).unwrap();
        for _ in 0..70 {
            file.write_all(&block).unwrap();
        }
        file.sync_all().unwrap();
        drop(file);
        let position = host.checkpoint().unwrap();
        assert!(position.index > 0);
        if position.index > cut.index {
            cut = position;
        }
    }
    let current = leader(&hosts);
    let host = hosts[current].as_ref().unwrap();
    let term = host.status().term;
    applied(
        &host.propose(term, b"after-snapshot".to_vec()).unwrap(),
        &hosts,
    );
    let restored = Machine::open(root.0.join(format!("app-{}", offline + 1)));
    hosts[offline] = Some(
        Host::start(
            config(&root.0, group, offline as u16 + 1, &members, false),
            restored.clone(),
        )
        .unwrap(),
    );
    let deadline = Instant::now() + Duration::from_secs(30);
    while restored.bytes() != b"checkpoint-rowafter-snapshot" {
        assert!(
            Instant::now() < deadline,
            "large snapshot catchup timed out; replicas: {:?}",
            hosts
                .iter()
                .map(|h| h.as_ref().map(Host::status))
                .collect::<Vec<_>>()
        );
        std::thread::sleep(Duration::from_millis(10));
    }
    assert_eq!(
        fs::metadata(restored.path.join("large-file"))
            .unwrap()
            .len(),
        70 << 20
    );
    let mut file = File::open(restored.path.join("large-file")).unwrap();
    let mut received = vec![0u8; block.len()];
    for _ in 0..70 {
        file.read_exact(&mut received).unwrap();
        assert_eq!(received, block);
    }
    assert!(hosts[offline].as_ref().unwrap().status().applied.index > cut.index);
    for host in hosts.into_iter().flatten() {
        host.shutdown();
    }
}

#[test]
fn concurrent_fresh_reads_do_not_wait_for_periodic_heartbeat() {
    let root = Directory::new();
    let members = peers(3);
    let group = 84;
    let machines = (1..=3)
        .map(|n| Machine::open(root.0.join(format!("app-{n}"))))
        .collect::<Vec<_>>();
    let hosts = (1..=3)
        .map(|n| {
            let mut cfg = config(&root.0, group, n, &members, true);
            cfg.heartbeat_interval = Duration::from_secs(1);
            cfg.election_timeout = Duration::from_secs(4);
            Some(Host::start(cfg, machines[n as usize - 1].clone()).unwrap())
        })
        .collect::<Vec<_>>();
    let current = leader(&hosts);
    let host = hosts[current].as_ref().unwrap();
    let term = host.status().term;
    let initial = host.read_barrier(term, Duration::from_secs(10)).unwrap();
    let sequence = host.status().durable_sequence;
    // Later callers cannot join the first caller's already-launched generation.
    // Retiring those pre-admission RPCs must promptly launch a fresh quorum,
    // not leave the remaining reads waiting for the one-second periodic tick.
    for _ in 0..2 {
        let barrier = std::sync::Barrier::new(17);
        std::thread::scope(|scope| {
            let workers = (0..16)
                .map(|_| {
                    scope.spawn(|| {
                        barrier.wait();
                        host.read_barrier(term, Duration::from_millis(600))
                    })
                })
                .collect::<Vec<_>>();
            barrier.wait();
            for worker in workers {
                assert_eq!(worker.join().unwrap().unwrap(), initial);
            }
        });
    }
    assert_eq!(host.status().durable_sequence, sequence);
    for host in hosts.into_iter().flatten() {
        host.shutdown();
    }
}

#[test]
fn single_member_fresh_quorum_waits_for_initial_current_term_application() {
    let root = Directory::new();
    let members = peers(1);
    let machine = Machine::open(root.0.join("app"));
    machine.block_apply.store(true, Ordering::SeqCst);
    let host = Host::start(config(&root.0, 85, 1, &members, true), machine.clone()).unwrap();
    struct Resume(Arc<Machine>);
    impl Drop for Resume {
        fn drop(&mut self) {
            self.0.block_apply.store(false, Ordering::SeqCst);
        }
    }
    // Declared after Host, so an assertion unwind releases application before
    // last-handle shutdown joins its worker.
    let resume = Resume(machine.clone());
    until(|| host.status().committed.index > 0);
    let committed = host.status().committed;
    assert!(committed.term > 0);
    assert_eq!(machine.applied_position().unwrap(), Position::default());
    assert!(matches!(
        host.read_barrier(committed.term, Duration::from_millis(100)),
        Err(raft_host::HostError::Timeout)
    ));
    drop(resume);
    assert_eq!(
        host.read_barrier(committed.term, Duration::from_secs(3))
            .unwrap(),
        committed
    );
    host.shutdown();
}

#[test]
fn concurrent_fresh_read_index_barriers_do_not_append_noops() {
    let root = Directory::new();
    let members = peers(1);
    let machine = Machine::open(root.0.join("app"));
    let host = Host::start(config(&root.0, 86, 1, &members, true), machine.clone()).unwrap();
    until(|| host.status().is_leader);
    let term = host.status().term;
    let initial = host.read_barrier(term, Duration::from_secs(3)).unwrap();
    // Election still commits/applies a real no-op. Later readiness queries do
    // not create more Raft WAL or application-prefix writes.
    assert!(initial.index > 0);
    assert_eq!(machine.applied_position().unwrap(), initial);
    let sequence = host.status().durable_sequence;
    let barrier = std::sync::Barrier::new(17);
    std::thread::scope(|scope| {
        let workers = (0..16)
            .map(|_| {
                scope.spawn(|| {
                    barrier.wait();
                    host.read_barrier(term, Duration::from_secs(3))
                })
            })
            .collect::<Vec<_>>();
        barrier.wait();
        for worker in workers {
            assert_eq!(worker.join().unwrap().unwrap(), initial);
        }
    });
    assert_eq!(host.status().durable_sequence, sequence);
    assert_eq!(machine.applied_position().unwrap(), initial);
    assert!(machine.bytes().is_empty());
    host.shutdown();
}

#[test]
fn dropping_last_handle_joins_workers_and_releases_durable_store() {
    let root = Directory::new();
    let members = peers(1);
    let machine = Machine::open(root.0.join("app"));
    let host = Host::start(config(&root.0, 87, 1, &members, true), machine.clone()).unwrap();
    let clone = host.clone();
    drop(host);
    assert!(Host::start(config(&root.0, 87, 1, &members, false), machine.clone()).is_err());
    drop(clone);
    // Reopen immediately, without polling for an asynchronously detached actor.
    let reopened = Host::start(config(&root.0, 87, 1, &members, false), machine).unwrap();
    reopened.shutdown();
}

#[cfg(feature = "test-hooks")]
#[test]
fn isolated_stale_leader_cannot_commit_or_serve_reads_and_heals() {
    let root = Directory::new();
    let members = peers(3);
    let group = 106;
    let machines = (1..=3)
        .map(|n| Machine::open(root.0.join(format!("app-{n}"))))
        .collect::<Vec<_>>();
    let mut hosts = (1..=3)
        .map(|n| {
            Some(
                Host::start(
                    config(&root.0, group, n, &members, true),
                    machines[n as usize - 1].clone(),
                )
                .unwrap(),
            )
        })
        .collect::<Vec<_>>();
    let original = leader(&hosts);
    let old = hosts[original].take().unwrap();
    let old_term = old.status().term;
    old.read_barrier(old_term, Duration::from_secs(10)).unwrap();
    old.set_isolated(true);
    let uncertain = old.propose(old_term, b"must-not-apply".to_vec()).unwrap();
    assert_eq!(
        uncertain.wait(Duration::from_millis(250)),
        ProposalStatus::Unknown
    );
    let replacement = leader(&hosts);
    let current = hosts[replacement].as_ref().unwrap();
    let new_term = current.status().term;
    assert!(new_term > old_term);
    current
        .read_barrier(new_term, Duration::from_secs(10))
        .unwrap();
    applied(
        &current.propose(new_term, b"new-term".to_vec()).unwrap(),
        &hosts,
    );
    assert!(matches!(
        old.read_barrier(old_term, Duration::from_millis(250)),
        Err(raft_host::HostError::Timeout)
    ));
    assert!(old.status().committed.index < uncertain.position.index);
    assert!(!uncertain.is_committed());
    assert_eq!(machines[original].bytes(), b"");
    old.set_isolated(false);
    hosts[original] = Some(old.clone());
    until(|| {
        uncertain.status() == ProposalStatus::Discarded && machines[original].bytes() == b"new-term"
    });
    assert!(matches!(
        old.propose(old_term, b"fenced".to_vec()),
        Err(AdmissionError::NotLeader { .. })
    ));
    assert!(matches!(
        old.read_barrier(old_term, Duration::from_secs(1)),
        Err(raft_host::HostError::NotLeader)
    ));
    for host in hosts.into_iter().flatten() {
        host.shutdown();
    }
}

#[cfg(feature = "test-hooks")]
#[test]
fn two_survivors_with_different_tails_elect_and_preserve_committed_prefix() {
    let root = Directory::new();
    let members = peers(3);
    let group = 107;
    let machines = (1..=3)
        .map(|n| Machine::open(root.0.join(format!("app-{n}"))))
        .collect::<Vec<_>>();
    let mut hosts = (1..=3)
        .map(|n| {
            Some(
                Host::start(
                    config(&root.0, group, n, &members, true),
                    machines[n as usize - 1].clone(),
                )
                .unwrap(),
            )
        })
        .collect::<Vec<_>>();
    let original = leader(&hosts);
    let lagging = (original + 1) % 3;
    let durable = (original + 2) % 3;
    let term = hosts[original].as_ref().unwrap().status().term;
    hosts[original]
        .as_ref()
        .unwrap()
        .read_barrier(term, Duration::from_secs(3))
        .unwrap();
    hosts[lagging].as_ref().unwrap().set_isolated(true);
    let proposal = hosts[original]
        .as_ref()
        .unwrap()
        .propose(term, b"committed".to_vec())
        .unwrap();
    applied(&proposal, &hosts);
    until(|| machines[durable].bytes() == b"committed");
    assert!(machines[lagging].bytes().is_empty());
    // The old leader is unavailable. Exactly two voters remain, only one of
    // which has the committed suffix; stale-log denials must not prevent the
    // up-to-date survivor from eventually winning a fresh campaign.
    hosts[original].take().unwrap().shutdown();
    hosts[lagging].as_ref().unwrap().set_isolated(false);
    let replacement = leader(&hosts);
    let current = hosts[replacement].as_ref().unwrap();
    let next_term = current.status().term;
    assert!(next_term > term);
    current
        .read_barrier(next_term, Duration::from_secs(3))
        .unwrap();
    until(|| machines[lagging].bytes() == b"committed");
    applied(
        &current.propose(next_term, b"-survivor".to_vec()).unwrap(),
        &hosts,
    );
    until(|| {
        [lagging, durable]
            .into_iter()
            .all(|i| machines[i].bytes() == b"committed-survivor")
    });
    for host in hosts.into_iter().flatten() {
        host.shutdown();
    }
}

#[test]
fn recovery_refuses_application_newer_than_raft_authority() {
    let root = Directory::new();
    let members = peers(1);
    let machine = Machine::open(root.0.join("app"));
    machine
        .save(Position { index: 1, term: 1 }, b"not-consensus")
        .unwrap();
    drop(machine);
    let machine = Machine::open(root.0.join("app"));
    assert!(Host::start(config(&root.0, 88, 1, &members, true), machine).is_err());
}

#[test]
fn unavailable_quorum_is_unknown_not_an_abort_and_can_later_commit() {
    let root = Directory::new();
    let members = peers(3);
    let group = 101;
    let machines = (1..=3)
        .map(|n| Machine::open(root.0.join(format!("app-{n}"))))
        .collect::<Vec<_>>();
    let mut hosts = (1..=3)
        .map(|n| {
            Some(
                Host::start(
                    config(&root.0, group, n, &members, true),
                    machines[n as usize - 1].clone(),
                )
                .unwrap(),
            )
        })
        .collect::<Vec<_>>();
    let current = leader(&hosts);
    for (index, slot) in hosts.iter_mut().enumerate() {
        if index != current {
            slot.take().unwrap().shutdown();
        }
    }
    let host = hosts[current].as_ref().unwrap();
    let term = host.status().term;
    let proposal = host.propose(term, b"possibly-submitted".to_vec()).unwrap();
    assert_eq!(
        proposal.wait(Duration::from_millis(200)),
        ProposalStatus::Unknown
    );
    assert!(host.status().committed.index < proposal.position.index);
    assert!(host.read_barrier(term, Duration::from_millis(100)).is_err());
    for (index, slot) in hosts.iter_mut().enumerate() {
        if index != current {
            *slot = Some(
                Host::start(
                    config(&root.0, group, index as u16 + 1, &members, false),
                    machines[index].clone(),
                )
                .unwrap(),
            );
        }
    }
    until(|| matches!(proposal.status(), ProposalStatus::Applied));
    until(|| machines.iter().all(|m| m.bytes() == b"possibly-submitted"));
    for host in hosts.into_iter().flatten() {
        host.shutdown();
    }
}

/// Child entrypoint, invoked only by the recovery test below. It runs the same
/// host and durable application as a separate process so abort loses memory.
#[test]
#[ignore]
fn snapshot_process() {
    let Ok(root) = std::env::var("RAFT_HOST_TEST_ROOT") else {
        return;
    };
    let root = PathBuf::from(root);
    let members = std::env::var("RAFT_HOST_TEST_PEERS")
        .unwrap()
        .split(',')
        .enumerate()
        .map(|(i, address)| Peer {
            id: i as u16 + 1,
            address: address.to_owned(),
        })
        .collect::<Vec<_>>();
    let machine = Machine::open(root.join("app-3"));
    let host = Host::start(config(&root, 151, 3, &members, true), machine).unwrap();
    until(|| root.join("stop-child").exists());
    host.shutdown();
}

#[test]
fn interrupted_snapshot_stage_authority_and_generation_publication_recover() {
    for point in [
        "snapshot.staged",
        "snapshot.authority",
        "snapshot.published",
    ] {
        let root = Directory::new();
        let members = peers(3);
        let machines = (1..=2)
            .map(|n| Machine::open(root.0.join(format!("app-{n}"))))
            .collect::<Vec<_>>();
        let hosts = (1..=2)
            .map(|n| {
                Some(
                    Host::start(
                        config(&root.0, 151, n, &members, true),
                        machines[n as usize - 1].clone(),
                    )
                    .unwrap(),
                )
            })
            .collect::<Vec<_>>();
        let current = leader(&hosts);
        let host = hosts[current].as_ref().unwrap();
        let term = host.status().term;
        applied(
            &host.propose(term, b"survives-cutpoint".to_vec()).unwrap(),
            &hosts,
        );
        until(|| {
            machines
                .iter()
                .all(|machine| machine.bytes() == b"survives-cutpoint")
        });
        // Neither parent replica may retain an uncompacted alternative path.
        let cuts = hosts
            .iter()
            .flatten()
            .map(|host| host.checkpoint().unwrap())
            .collect::<Vec<_>>();
        let cut = *cuts.iter().max_by_key(|position| position.index).unwrap();
        let child_log = root.0.join("child-stderr");
        let mut child = std::process::Command::new(std::env::current_exe().unwrap())
            .args(["--ignored", "--exact", "snapshot_process", "--nocapture"])
            .env("RAFT_HOST_TEST_ROOT", &root.0)
            .env(
                "RAFT_HOST_TEST_PEERS",
                members
                    .iter()
                    .map(|p| p.address.as_str())
                    .collect::<Vec<_>>()
                    .join(","),
            )
            .env("RAFT_HOST_CRASH_POINT", point)
            .env("RAFT_HOST_CRASH_GROUP", "151")
            .stderr(File::create(&child_log).unwrap())
            .spawn()
            .unwrap();
        let deadline = Instant::now() + Duration::from_secs(30);
        let status = loop {
            if let Some(status) = child.try_wait().unwrap() {
                break status;
            }
            if Instant::now() >= deadline {
                let _ = child.kill();
                let _ = child.wait();
                panic!(
                    "snapshot cutpoint {point} not reached; child: {}; replicas: {:?}",
                    fs::read_to_string(&child_log).unwrap(),
                    hosts
                        .iter()
                        .map(|h| h.as_ref().map(Host::status))
                        .collect::<Vec<_>>()
                );
            }
            std::thread::sleep(Duration::from_millis(10));
        };
        use std::os::unix::process::ExitStatusExt;
        assert_eq!(
            status.signal(),
            Some(6),
            "cutpoint must abort, not merely fail its test"
        );
        assert!(
            fs::read_to_string(&child_log)
                .unwrap()
                .contains(&format!("raft-host group 151 crash point {point}")),
            "child must reach the specifically requested durable boundary"
        );
        let machine = Machine::open(root.0.join("app-3"));
        let restarted =
            Host::start(config(&root.0, 151, 3, &members, false), machine.clone()).unwrap();
        until(|| {
            machine.bytes() == b"survives-cutpoint" && restarted.status().applied.index >= cut.index
        });
        assert!(restarted.status().applied.index >= cut.index);
        restarted.shutdown();
        for host in hosts.into_iter().flatten() {
            host.shutdown();
        }
    }
}
