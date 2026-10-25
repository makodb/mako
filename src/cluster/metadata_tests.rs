//! Real-RocksDB consumer regressions for metadata-only recovery, not row recovery.
use crate::directory::RouteTable;
use crate::durable_coordinator::{Operation, Response};
use crate::durable_kv::{Journal, Namespace};
use crate::metadata::{Event, ParticipantLog};
use crate::metadata_coordinator::CoordinatorState;
use crate::migration::Request;
use crate::participant::{Cleanup, ControlResult};
use crate::rocks_metadata::RocksKv;
use crate::types::{Boundary, Certificate, Grant, KeyRange, MigrationPlan, Phase, Status, TxnId};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};

static NEXT_DIRECTORY: AtomicU64 = AtomicU64::new(0);
const IDENTITY: &[u8] = b"metadata-consumer-test/catalog-31/owners-0-1";
const TABLE: u64 = 31;
const NONCE: TxnId = TxnId { client: 701, sequence: 1 };

struct Directory(PathBuf);
impl Directory {
    fn new() -> Self {
        loop {
            let path = std::env::temp_dir().join(format!("mako-metadata-consumer-{}-{}",
                std::process::id(), NEXT_DIRECTORY.fetch_add(1, Ordering::Relaxed)));
            match std::fs::create_dir(&path) {
                Ok(()) => return Self(path),
                Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => continue,
                Err(error) => panic!("creating metadata test directory: {error}"),
            }
        }
    }
    fn database(&self, role: &str) -> PathBuf { self.0.join("owner-0").join(role) }
}
impl Drop for Directory {
    fn drop(&mut self) { let _ = std::fs::remove_dir_all(&self.0); }
}
fn tables() -> Vec<RouteTable> {
    vec![RouteTable { table: TABLE, boundaries: vec![Boundary {
        start: vec![], grant: Grant { owner: 0, ownership_version: 0 },
    }] }]
}
fn range() -> KeyRange {
    KeyRange { table: TABLE, lo: b"m".to_vec(), hi: Some(b"n".to_vec()) }
}
fn begin(nonce: TxnId) -> Operation {
    Operation::Begin { nonce, source: 0, destination: 1, range: range() }
}
fn coordinator(path: Option<&Path>, initialize: bool) -> CoordinatorState {
    CoordinatorState::new(vec![0, 1], tables(), path, initialize, IDENTITY).unwrap()
}
fn applied(state: &mut CoordinatorState, operation: Operation) {
    assert!(matches!(state.execute(operation), Ok(Response::Applied)));
}
fn assert_plan(state: &CoordinatorState) {
    let plan = state.actor().unwrap().plan_for_nonce(NONCE).expect("retained nonce");
    assert_eq!(plan.generation, 1);
    assert_eq!(plan.source, 0);
    assert_eq!(plan.destination, 1);
    assert_eq!(plan.range.table, TABLE);
    assert_eq!(plan.range.lo, b"m");
    assert_eq!(plan.range.hi.as_deref(), Some(b"n".as_slice()));
}
fn assert_fenced(state: &mut CoordinatorState) {
    // Even an identical Begin is inspection-only at the runtime boundary; this
    // mutation facade must not admit it or any fresh generation on recovery.
    for nonce in [NONCE, TxnId { client: NONCE.client, sequence: 2 }] {
        assert!(matches!(state.execute(begin(nonce)), Err(Status::Retry)));
    }
    for request in [Request::Freeze, Request::Final, Request::Retire,
        Request::Commit, Request::Abort, Request::Finish]
    {
        assert!(matches!(state.execute(Operation::Request(request)), Err(Status::Retry)));
    }
    for certificate in [Certificate::Drained, Certificate::Ready, Certificate::Retired,
        Certificate::SourceDone, Certificate::DestinationDone]
    {
        let owner = match certificate {
            Certificate::Ready | Certificate::DestinationDone => 1,
            _ => 0,
        };
        assert!(matches!(state.execute(Operation::Receive {
            generation: 1, owner, certificate,
        }), Err(Status::Retry)));
    }
    assert!(!state.failed());
    assert_plan(state);
}

#[test]
fn coordinator_decision_and_nonce_survive_close_reopen_without_resuming() {
    for committed in [false, true] {
        let directory = Directory::new();
        let path = directory.database("coordinator");
        let mut state = coordinator(Some(&path), true);
        assert!(matches!(state.execute(begin(NONCE)), Ok(Response::Begun(1))));
        if committed {
            for (request, certificate, owner) in [
                (Request::Freeze, Certificate::Drained, 0),
                (Request::Final, Certificate::Ready, 1),
                (Request::Retire, Certificate::Retired, 0),
            ] {
                applied(&mut state, Operation::Request(request));
                applied(&mut state, Operation::Receive { generation: 1, owner, certificate });
            }
            applied(&mut state, Operation::Request(Request::Commit));
        } else {
            applied(&mut state, Operation::Request(Request::Abort));
        }
        drop(state);

        // Reopen twice: Reply is allowed to append metadata and must itself
        // leave a recoverable prefix, without finishing the outstanding work.
        for _ in 0..2 {
            let mut retained = coordinator(Some(&path), false);
            assert!(retained.recovered());
            assert_fenced(&mut retained);
            let phase = if committed { Phase::Committed } else { Phase::Aborted };
            assert!(retained.actor().unwrap().current_phase() == Some(phase));
            let outcome = retained.actor().unwrap().outcome(NONCE).expect("retained decision");
            assert_eq!(outcome.generation, 1);
            assert_eq!(outcome.committed, committed);
            assert_eq!(retained.actor().unwrap().lookup(TABLE, b"m").unwrap().owner,
                if committed { 1 } else { 0 });
            match retained.execute(Operation::Reply(NONCE)).unwrap() {
                Response::Replied(reply) => assert!(reply == outcome),
                _ => panic!("expected retained administrative outcome"),
            }
            assert!(retained.actor().unwrap().current_phase() == Some(phase));
            drop(retained);
        }
    }
}

#[test]
fn pending_coordinator_remains_inspectable_but_cannot_be_driven_after_reopen() {
    let directory = Directory::new();
    let path = directory.database("coordinator");
    let mut state = coordinator(Some(&path), true);
    assert!(matches!(state.execute(begin(NONCE)), Ok(Response::Begun(1))));
    drop(state);
    let mut retained = coordinator(Some(&path), false);
    assert!(retained.recovered());
    assert_fenced(&mut retained);
    assert!(retained.actor().unwrap().current_phase() == Some(Phase::Copy));
    assert!(retained.actor().unwrap().outcome(NONCE).is_none());
    assert!(matches!(retained.execute(Operation::Reply(NONCE)), Err(Status::Retry)));
    assert_eq!(retained.actor().unwrap().lookup(TABLE, b"m").unwrap().owner, 0);
    assert!(!retained.failed());
}

#[test]
fn live_nonce_retries_bind_every_intent_field_with_and_without_persistence() {
    for persistent in [false, true] {
        let directory = Directory::new();
        let path = directory.database("coordinator");
        let mut state = coordinator(if persistent { Some(&path) } else { None }, true);
        assert!(!state.recovered());
        assert!(matches!(state.execute(begin(NONCE)), Ok(Response::Begun(1))));
        assert!(matches!(state.execute(begin(NONCE)), Ok(Response::Begun(1))));
        for field in 0..5 {
            let mut changed = range();
            let mut source = 0;
            let mut destination = 1;
            match field {
                0 => source = 1,
                1 => destination = 0,
                2 => changed.table += 1,
                3 => changed.lo = b"l".to_vec(),
                _ => changed.hi = None,
            }
            assert!(matches!(state.execute(Operation::Begin {
                nonce: NONCE, source, destination, range: changed,
            }), Err(Status::Invalid)));
        }
        assert_plan(&state);
        assert!(!state.failed());
        applied(&mut state, Operation::Request(Request::Abort));
        assert!(matches!(state.execute(Operation::Reply(NONCE)), Ok(Response::Replied(_))));
    }
}

#[test]
fn missing_or_wrong_identity_metadata_never_falls_back_to_genesis() {
    let directory = Directory::new();
    let coordinator_path = directory.database("coordinator");
    let participant_path = directory.database("participant");
    assert!(matches!(CoordinatorState::new(vec![0, 1], tables(), Some(&coordinator_path),
        false, IDENTITY), Err(Status::NotFound)));
    assert!(matches!(ParticipantLog::new(0, 2, IDENTITY, Some(&participant_path), false),
        Err(Status::NotFound)));
    assert!(!coordinator_path.exists());
    assert!(!participant_path.exists());
    drop(coordinator(Some(&coordinator_path), true));
    drop(ParticipantLog::new(0, 2, IDENTITY, Some(&participant_path), true).unwrap());
    assert!(matches!(CoordinatorState::new(vec![0, 1], tables(), Some(&coordinator_path),
        false, b"different-catalog"), Err(Status::Invalid)));
    assert!(matches!(ParticipantLog::new(0, 2, b"different-catalog", Some(&participant_path),
        false), Err(Status::Invalid)));
    // Failed identity checks must not overwrite the original deployment binding.
    assert!(coordinator(Some(&coordinator_path), false).recovered());
    assert!(ParticipantLog::new(0, 2, IDENTITY, Some(&participant_path), false)
        .unwrap().recovered());
}

#[test]
fn participant_incomplete_journal_is_retained_without_any_append_admission() {
    for transfer_started in [false, true] {
        let directory = Directory::new();
        let path = directory.database("participant");
        let plan = MigrationPlan { generation: 1, nonce: NONCE, source: 0, destination: 1,
            range: range(), old: vec![Boundary { start: vec![], grant: Grant { owner: 0, ownership_version: 0 } }] };
        let plan = crate::wire::encode_plan(&plan).unwrap();
        let result = ControlResult { status: Status::Ok, certificate: None, cleanup: Cleanup::None };
        let mut live = ParticipantLog::new(0, 2, IDENTITY, Some(&path), true).unwrap();
        live.record(Event::Control {
            operation: crate::wire::command_operation(crate::types::Command::Freeze),
            plan: &plan, result: &result,
        }).unwrap();
        if transfer_started {
            live.record(Event::Transfer { plan: &plan, cleanup: false, result: None }).unwrap();
        }
        drop(live);
        let mut retained = ParticipantLog::new(0, 2, IDENTITY, Some(&path), false).unwrap();
        assert!(retained.recovered());
        for event in [
            Event::Control { operation: crate::wire::DRAIN, plan: &plan, result: &result },
            Event::Transfer { plan: &plan, cleanup: false, result: None },
            Event::Transfer { plan: &plan, cleanup: false, result: Some(&result) },
            Event::Transfer { plan: &plan, cleanup: true, result: Some(&result) },
        ] {
            assert_eq!(retained.record(event), Err(Status::Retry));
        }
        assert!(!retained.failed());
        drop(retained);
        // Inspect through the real journal API, not a second serialization
        // implementation: rejected completions must never extend the prefix.
        let mut kv = RocksKv::open(&path, false).unwrap();
        let journal = Journal::recover(&mut kv, Namespace { owner: 0, coordinator: false })
            .ok().expect("retained participant journal");
        assert_eq!(journal.count(), if transfer_started { 3 } else { 2 });
        assert!(journal.admission().is_none());
    }
}

#[test]
fn malformed_committed_participant_payload_refuses_startup() {
    let directory = Directory::new();
    let path = directory.database("participant");
    let live = ParticipantLog::new(0, 2, IDENTITY, Some(&path), true).unwrap();
    drop(live);
    {
        let mut kv = RocksKv::open(&path, false).unwrap();
        let mut journal = Journal::recover(&mut kv, Namespace { owner: 0, coordinator: false })
            .ok().expect("valid journal prefix");
        journal.activate(&mut kv).ok().expect("test corruption writer");
        // The journal envelope and atomic head are valid; its committed payload
        // is not a metadata event. Startup must validate more than RocksDB open.
        journal.append(&mut kv, vec![0xff]).ok().expect("committed malformed event");
    }
    assert!(matches!(ParticipantLog::new(0, 2, IDENTITY, Some(&path), false),
        Err(Status::Invalid)));
    assert!(matches!(ParticipantLog::new(0, 2, IDENTITY, Some(&path), true),
        Err(Status::Invalid)));
    assert!(matches!(ParticipantLog::new(0, 2, IDENTITY, Some(&path), false),
        Err(Status::Invalid)));
}

#[test]
fn existing_database_without_consumer_bootstrap_is_not_fresh_metadata() {
    let directory = Directory::new();
    let coordinator_path = directory.database("coordinator");
    let participant_path = directory.database("participant");
    // Valid RocksDB databases, but interrupted initialization never installed
    // either consumer's bootstrap. Merely opening storage is not recovery.
    drop(RocksKv::open(&coordinator_path, true).unwrap());
    drop(RocksKv::open(&participant_path, true).unwrap());
    assert!(matches!(CoordinatorState::new(vec![0, 1], tables(), Some(&coordinator_path),
        false, IDENTITY), Err(Status::Invalid)));
    assert!(matches!(ParticipantLog::new(0, 2, IDENTITY, Some(&participant_path), false),
        Err(Status::Io)));
    assert!(matches!(CoordinatorState::new(vec![0, 1], tables(), Some(&coordinator_path),
        true, IDENTITY), Err(Status::Invalid)));
    assert!(matches!(ParticipantLog::new(0, 2, IDENTITY, Some(&participant_path), true),
        Err(Status::Invalid)));
}
