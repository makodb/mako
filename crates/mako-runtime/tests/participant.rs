#![cfg(feature = "native-storage")]
use sto_core::protocol::journal::{
    self, Entry, Journal, Member, MemoryLog, Outcome, Record, Transaction, TransactionId,
};
use mako_runtime::{
    receiver::RunningShard,
    rpc::Network,
    storage::{CommitLog, Store},
    wire::*,
};
use std::{
    sync::{
        atomic::{AtomicBool, AtomicUsize, Ordering},
        Arc,
    },
    time::Duration,
};

fn access() -> Access {
    Access {
        table: TableKey {
            table: 1,
            warehouse: 0,
        },
        route: None,
    }
}
fn provision(store: &Store) {
    let mut worker = store.database.worker().unwrap();
    store.open(&mut worker, access().table, 1).unwrap();
}
struct Peer {
    network: Arc<Network>,
    endpoint: String,
    lease: u64,
}
impl Peer {
    fn new(network: Arc<Network>, endpoint: &str) -> Self {
        let reply = network
            .call(
                endpoint,
                Message::OpenStream {
                    stream: Stream { node: 0, worker: 0 },
                    nonce: 42,
                },
                Duration::from_secs(5),
            )
            .unwrap();
        let Reply::OpenStream(Ok(lease)) = reply else {
            panic!("claim reply: {reply:?}");
        };
        Self {
            network,
            endpoint: endpoint.into(),
            lease,
        }
    }
    fn ordinary(&self, attempt: u64, sequence: u64, action: Action) -> Result<Value> {
        self.raw(Request {
            stream: Stream { node: 0, worker: 0 },
            identity: Identity {
                incarnation: self.lease,
                attempt,
                sequence,
                term: 0,
            },
            action,
        })
    }
    fn raw(&self, request: Request) -> Result<Value> {
        match self.network.call(
            &self.endpoint,
            Message::Participant(request),
            Duration::from_secs(5),
        )? {
            Reply::Participant(response) => response.result,
            Reply::Error(error) => Err(error),
            other => panic!("unexpected reply: {other:?}"),
        }
    }
    fn control(&self, attempt: u64, action: TransactionAction) -> Result<TransactionValue> {
        let id = TransactionId {
            coordinator: 0,
            worker: 0,
            incarnation: 42,
            attempt,
        };
        match self.network.call(
            &self.endpoint,
            Message::Transaction(TransactionRequest {
                id,
                lease: self.lease,
                term: 0,
                action,
            }),
            Duration::from_secs(5),
        )? {
            Reply::Transaction(response) => {
                assert_eq!(response.id, id);
                response.result
            }
            Reply::Error(error) => Err(error),
            other => panic!("unexpected control reply: {other:?}"),
        }
    }
    fn lock(&self, store: &Store, attempt: u64) -> Transaction {
        let TransactionValue::Locked { timestamp, durable } =
            self.control(attempt, TransactionAction::Lock).unwrap()
        else {
            panic!("lock reply");
        };
        let transaction = Transaction {
            version: journal::VERSION,
            id: TransactionId {
                coordinator: 0,
                worker: 0,
                incarnation: 42,
                attempt,
            },
            time_term: timestamp * 10,
            durable,
            members: vec![Member {
                owner: 0,
                endpoints: vec![self.endpoint.clone()],
                lease: self.lease,
            }],
        };
        store
            .journal
            .apply(&Record::Begin(transaction.clone()))
            .unwrap();
        transaction
    }
    fn prepare(&self, store: &Store, attempt: u64) -> Transaction {
        let transaction = self.lock(store, attempt);
        assert_eq!(
            self.control(attempt, TransactionAction::Prepare(transaction.clone()))
                .unwrap(),
            TransactionValue::Prepared
        );
        transaction
    }
    fn decide(
        &self,
        store: &Store,
        transaction: &Transaction,
        outcome: Outcome,
    ) -> Result<TransactionValue> {
        store
            .journal
            .apply(&Record::Decide {
                transaction: transaction.clone(),
                outcome,
            })
            .unwrap();
        self.control(
            transaction.id.attempt,
            TransactionAction::Decide {
                transaction: transaction.clone(),
                outcome,
            },
        )
    }
}
fn write(value: &[u8]) -> Action {
    Action::Stage {
        writes: vec![Write {
            access: access(),
            mutation: Mutation::Put,
            key: b"key".to_vec(),
            value: value.to_vec(),
        }],
    }
}
fn read() -> Action {
    Action::Get {
        access: access(),
        key: b"key".to_vec(),
    }
}

#[test]
fn prepared_participant_commits_once_and_old_decision_cannot_touch_a_new_attempt() {
    let store = Store::new(0, false, Arc::new(MemoryLog::new(0))).unwrap();
    provision(&store);
    let network = Arc::new(Network::default());
    let server =
        RunningShard::start(store.clone(), network.clone(), "127.0.0.1:0", 1, 1, 8).unwrap();
    let peer = Peer::new(network, server.address());
    peer.ordinary(1, 1, write(b"first")).unwrap();
    let transaction = peer.prepare(&store, 1);
    let decide = TransactionAction::Decide {
        transaction: transaction.clone(),
        outcome: Outcome::Commit,
    };
    assert_eq!(
        peer.control(1, decide.clone()).unwrap_err().code,
        ErrorCode::Unknown,
        "installation requires a saved decision"
    );
    assert_eq!(
        peer.decide(&store, &transaction, Outcome::Commit).unwrap(),
        TransactionValue::Completed(Outcome::Commit)
    );
    assert_eq!(
        peer.control(1, decide.clone()).unwrap(),
        TransactionValue::Completed(Outcome::Commit)
    );
    assert_eq!(
        peer.ordinary(1, 2, Action::Abort).unwrap_err().code,
        ErrorCode::Unknown
    );
    assert!(
        matches!(peer.ordinary(2, 1, read()).unwrap(), Value::Point { value: Some(v), .. } if v == b"first")
    );
    peer.ordinary(2, 2, write(b"discarded")).unwrap();
    peer.control(1, decide).unwrap(); // Terminal receipt must not release attempt 2.
    assert!(peer.ordinary(3, 1, write(b"stolen")).is_err());
    peer.ordinary(2, 3, Action::Abort).unwrap();
    assert!(
        matches!(peer.ordinary(3, 1, read()).unwrap(), Value::Point { value: Some(v), .. } if v == b"first")
    );
    peer.ordinary(3, 2, Action::Abort).unwrap();
    let cut = store.epochs.cut().unwrap();
    assert!(store.epochs.drained(cut).unwrap());
}

struct FaultJournal {
    inner: MemoryLog,
    fail_completion: AtomicBool,
    panic_completion: bool,
    completions: AtomicUsize,
}
impl CommitLog for FaultJournal {
    fn append(&self, _: &[u8], _: u32) -> Result<()> {
        panic!("old row log path");
    }
}
impl Journal for FaultJournal {
    fn durable(&self) -> bool {
        false
    }
    fn apply(&self, record: &Record) -> Result<Entry> {
        if matches!(record, Record::Complete { .. }) {
            self.completions.fetch_add(1, Ordering::Relaxed);
            assert!(!self.panic_completion, "injected participant worker panic");
            if self.fail_completion.load(Ordering::Relaxed) {
                return Err(Error::new(ErrorCode::Unknown, "completion reply lost"));
            }
        }
        self.inner.apply(record)
    }
    fn lookup(&self, id: TransactionId) -> Result<Option<Entry>> {
        self.inner.lookup(id)
    }
    fn entries(&self) -> Result<Vec<Entry>> {
        self.inner.entries()
    }
}
#[test]
fn prepared_work_survives_listener_teardown_and_recovers_saved_commit_without_abort() {
    let log = Arc::new(FaultJournal {
        inner: MemoryLog::new(0),
        fail_completion: AtomicBool::new(true),
        panic_completion: false,
        completions: AtomicUsize::new(0),
    });
    let store = Store::new(0, false, log.clone()).unwrap();
    provision(&store);
    let network = Arc::new(Network::default());
    let server =
        RunningShard::start(store.clone(), network.clone(), "127.0.0.1:0", 1, 1, 8).unwrap();
    let mut peer = Peer::new(network.clone(), server.address());
    peer.ordinary(1, 1, write(b"committed later")).unwrap();
    let transaction = peer.prepare(&store, 1);
    assert_eq!(
        peer.decide(&store, &transaction, Outcome::Commit)
            .unwrap_err()
            .code,
        ErrorCode::Unknown
    );
    let cut = store.epochs.cut().unwrap();
    drop(server);
    assert!(!store.epochs.drained(cut).unwrap());
    assert!(log.lookup(transaction.id).unwrap().unwrap().unresolved());
    let restarted = RunningShard::start(store.clone(), network, "127.0.0.1:0", 1, 1, 8).unwrap();
    peer.endpoint = restarted.address().into();
    assert!(peer.ordinary(1, 2, Action::Abort).is_err());
    assert!(peer.ordinary(2, 1, write(b"forbidden")).is_err());
    log.fail_completion.store(false, Ordering::Relaxed);
    peer.control(
        1,
        TransactionAction::Decide {
            transaction: transaction.clone(),
            outcome: Outcome::Commit,
        },
    )
    .unwrap();
    assert!(store.epochs.drained(cut).unwrap());
    assert!(store
        .journal
        .apply(&Record::Decide {
            transaction,
            outcome: Outcome::Abort
        })
        .is_err());
    assert!(
        matches!(peer.ordinary(2, 1, read()).unwrap(), Value::Point { value: Some(v), .. } if v == b"committed later")
    );
    peer.ordinary(2, 2, Action::Abort).unwrap();
}

#[test]
fn frozen_prepare_rejects_new_writes_terms_and_changed_intent_and_retains_migration_pin() {
    let store = Store::new(0, false, Arc::new(MemoryLog::new(0))).unwrap();
    provision(&store);
    let network = Arc::new(Network::default());
    let server =
        RunningShard::start(store.clone(), network.clone(), "127.0.0.1:0", 1, 1, 8).unwrap();
    let peer = Peer::new(network, server.address());
    peer.ordinary(1, 1, write(b"prepared")).unwrap();
    let transaction = peer.prepare(&store, 1);
    let cut = store.epochs.cut().unwrap();
    assert!(!store.epochs.drained(cut).unwrap());
    assert!(peer.ordinary(1, 2, write(b"late")).is_err());
    assert!(peer
        .raw(Request {
            stream: Stream { node: 0, worker: 0 },
            identity: Identity {
                incarnation: peer.lease,
                attempt: 2,
                sequence: 1,
                term: 1
            },
            action: Action::Abort
        })
        .is_err());
    let mut changed = transaction.clone();
    changed.time_term += 10;
    assert!(peer
        .control(1, TransactionAction::Prepare(changed))
        .is_err());
    assert!(peer
        .control(1, TransactionAction::Prepare(transaction.clone()))
        .is_ok());
    peer.decide(&store, &transaction, Outcome::Abort).unwrap();
    assert!(store.epochs.drained(cut).unwrap());
    assert!(matches!(
        peer.ordinary(2, 1, read()).unwrap(),
        Value::Point { value: None, .. }
    ));
    peer.ordinary(2, 2, Action::Abort).unwrap();
    assert!(peer
        .control(1, TransactionAction::Prepare(transaction))
        .is_err());
}

#[test]
fn new_attempt_cannot_steal_execution_and_delayed_abort_cannot_cancel_its_successor() {
    let store = Store::new(0, false, Arc::new(MemoryLog::new(0))).unwrap();
    provision(&store);
    let network = Arc::new(Network::default());
    let server = RunningShard::start(store, network.clone(), "127.0.0.1:0", 1, 1, 8).unwrap();
    let peer = Peer::new(network, server.address());
    peer.ordinary(1, 1, write(b"old")).unwrap();
    assert_eq!(
        peer.ordinary(2, 1, write(b"new")).unwrap_err().code,
        ErrorCode::Conflict
    );
    peer.ordinary(1, 2, Action::Abort).unwrap();
    peer.ordinary(2, 1, write(b"new")).unwrap();
    assert_eq!(
        peer.ordinary(1, 3, Action::Abort).unwrap_err().code,
        ErrorCode::Conflict
    );
    peer.ordinary(2, 2, Action::Abort).unwrap();
}

#[cfg(feature = "replication")]
fn persistent_store(directory: &std::path::Path) -> Arc<Store> {
    use mako_runtime::{
        log::LocalLog,
        migration::{Kind, Schema, TableSpec},
    };
    let schema = Schema::new(
        vec![TableSpec {
            id: 1,
            name: "rows".into(),
            kind: Kind::Governed,
            warehouse: false,
            initial_owner: 0,
        }],
        1,
        0,
    )
    .unwrap();
    let log = LocalLog::new(Duration::from_secs(5)).unwrap();
    let store = Store::new(0, false, log.clone()).unwrap();
    provision(&store);
    let log_path = directory.join("log");
    log.start_engine(
        raft_host::LocalConfig {
            initial_state: [0; 32],
            group: 1,
            create: !log_path.exists(),
            directory: log_path,
            sync_writes: true,
        },
        &store,
        schema,
        b"prepared-recovery".to_vec(),
    )
    .unwrap();
    store
}

#[cfg(feature = "replication")]
#[test]
fn durable_native_prepare_restores_commit_abort_and_absent_tombstone_after_restart() {
    for (outcome, tombstone) in [
        (Some(Outcome::Commit), false),
        (Some(Outcome::Commit), true),
        (None, false),
    ] {
        let root = tempfile::tempdir().unwrap();
        let store = persistent_store(root.path());
        let network = Arc::new(Network::default());
        let server =
            RunningShard::start(store.clone(), network.clone(), "127.0.0.1:0", 1, 1, 8).unwrap();
        let mut peer = Peer::new(network.clone(), server.address());
        peer.ordinary(1, 1, write(b"saved intent")).unwrap();
        if tombstone {
            peer.ordinary(
                1,
                2,
                Action::Stage {
                    writes: vec![Write {
                        access: access(),
                        mutation: Mutation::Remove,
                        key: b"key".to_vec(),
                        value: Vec::new(),
                    }],
                },
            )
            .unwrap();
        }
        let transaction = peer.prepare(&store, 1);
        if let Some(outcome) = outcome {
            store
                .journal
                .apply(&Record::Decide {
                    transaction: transaction.clone(),
                    outcome,
                })
                .unwrap();
        }
        drop(server);
        drop(store);
        let recovered = persistent_store(root.path());
        assert!(recovered
            .journal
            .lookup(transaction.id)
            .unwrap()
            .unwrap()
            .unresolved());
        let server =
            RunningShard::start(recovered.clone(), network.clone(), "127.0.0.1:0", 1, 1, 8)
                .unwrap();
        peer.endpoint = server.address().into();
        let outcome = outcome.unwrap_or(Outcome::Abort);
        // Admission first restores the saved native frame and locks. Only then
        // can this queued completion act on the coordinator's saved decision.
        peer.control(
            1,
            TransactionAction::Decide {
                transaction: transaction.clone(),
                outcome,
            },
        )
        .unwrap();
        let saved = recovered.journal.lookup(transaction.id).unwrap().unwrap();
        assert_eq!(saved.decision, Some(outcome));
        assert_eq!(saved.participant.unwrap().outcome, Some(outcome));
        let peer = Peer::new(network, server.address());
        let expected = if outcome == Outcome::Commit && !tombstone {
            Some(b"saved intent".to_vec())
        } else {
            None
        };
        assert!(
            matches!(peer.ordinary(2, 1, read()).unwrap(), Value::Point { value, .. } if value == expected)
        );
        peer.ordinary(2, 2, Action::Abort).unwrap();
        let cut = recovered.epochs.cut().unwrap();
        assert!(recovered.epochs.drained(cut).unwrap());
    }
}

#[test]
fn client_resolves_a_saved_commit_without_repeating_body_or_leaking_migration_pin() {
    use mako_runtime::client::Client;
    let log = Arc::new(FaultJournal {
        inner: MemoryLog::new(0),
        fail_completion: AtomicBool::new(true),
        panic_completion: false,
        completions: AtomicUsize::new(0),
    });
    let store = Store::new(0, false, log.clone()).unwrap();
    provision(&store);
    let mut worker = store.database.worker().unwrap();
    let mut client = Client::new(
        Arc::new(Network::default()),
        Default::default(),
        Stream { node: 0, worker: 0 },
        0,
        Duration::from_secs(5),
    )
    .unwrap();
    let target = Target {
        owner: 0,
        access: access(),
        log_id: 1,
    };
    let mut transaction = client.begin(store.clone(), &mut worker).unwrap();
    let id = transaction.id();
    transaction
        .mutate(&target, Mutation::Put, b"key", b"only once")
        .unwrap();
    assert_eq!(transaction.commit().unwrap_err().code, ErrorCode::Unknown);
    assert!(client.begin(store.clone(), &mut worker).is_err());
    let cut = store.epochs.cut().unwrap();
    assert!(!store.epochs.drained(cut).unwrap());
    log.fail_completion.store(false, Ordering::Relaxed);
    assert_eq!(client.resolve(store.clone(), id).unwrap(), Outcome::Commit);
    assert!(store.epochs.drained(cut).unwrap());
    assert_eq!(client.resolve(store.clone(), id).unwrap(), Outcome::Commit);
    client
        .run(store, &mut worker, |tx| {
            assert_eq!(
                tx.get(&target, b"key")?.as_deref(),
                Some(b"only once".as_slice())
            );
            Ok(())
        })
        .unwrap();
}

#[test]
fn prepared_worker_panic_fences_admission_and_preserves_decision_and_migration_pin() {
    let log = Arc::new(FaultJournal {
        inner: MemoryLog::new(0),
        fail_completion: AtomicBool::new(false),
        panic_completion: true,
        completions: AtomicUsize::new(0),
    });
    let store = Store::new(0, false, log.clone()).unwrap();
    provision(&store);
    let network = Arc::new(Network::default());
    let server =
        RunningShard::start(store.clone(), network.clone(), "127.0.0.1:0", 1, 2, 8).unwrap();
    let peer = Peer::new(network.clone(), server.address());
    peer.ordinary(1, 1, write(b"saved intent")).unwrap();
    let transaction = peer.prepare(&store, 1);
    assert_eq!(
        peer.decide(&store, &transaction, Outcome::Commit)
            .unwrap_err()
            .code,
        ErrorCode::Unknown
    );
    let entry = log.lookup(transaction.id).unwrap().unwrap();
    assert_eq!(entry.decision, Some(Outcome::Commit));
    assert!(entry.unresolved());
    let cut = store.epochs.cut().unwrap();
    assert!(!store.epochs.drained(cut).unwrap());
    // A different stream would otherwise be able to execute after the failed
    // worker's native Drop released its writes.
    let reply = network
        .call(
            server.address(),
            Message::OpenStream {
                stream: Stream { node: 0, worker: 1 },
                nonce: 99,
            },
            Duration::from_secs(5),
        )
        .unwrap();
    assert!(matches!(
        reply,
        Reply::Error(Error {
            code: ErrorCode::Unknown,
            ..
        })
    ));
    assert!(log
        .apply(&Record::Decide {
            transaction,
            outcome: Outcome::Abort
        })
        .is_err());
    store.shutdown();
}
