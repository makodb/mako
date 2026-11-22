#![cfg(have_mako)]

use std::collections::HashSet;
use std::sync::{mpsc, Arc};
use std::thread::{self, JoinHandle, ThreadId};
use std::time::{Duration, Instant};

use mako_local::{Error, LocalDb, Table};

const POOL_SIZES: [usize; 3] = [1, 4, 16];
const STARTUP_TIMEOUT: Duration = Duration::from_secs(30);
const PHASE_TIMEOUT: Duration = Duration::from_secs(60);
const SOAK_TIMEOUT: Duration = Duration::from_secs(120);
const RELEASE_TIMEOUT: Duration = Duration::from_secs(60);
const DISJOINT_COMMITS_PER_WORKER: u64 = 256;
const HOT_UPDATES_PER_WORKER: u64 = 32;
const HOT_RETRY_BUDGET: usize = 4_096;
const RECOVERY_RETRY_BUDGET: usize = 4_096;

enum Command {
    ForceConflict { release: mpsc::Receiver<()> },
    ExplicitAbort,
    Soak { release: mpsc::Receiver<()> },
    Shutdown,
}

#[derive(Debug)]
struct Event {
    worker_id: usize,
    thread_id: ThreadId,
    kind: EventKind,
}

#[derive(Debug)]
enum EventKind {
    Attached,
    ConflictReady,
    ConflictDone(std::result::Result<(), String>),
    AbortDone(std::result::Result<(), String>),
    SoakReady,
    SoakDone(std::result::Result<SoakStats, String>),
    Stopped,
    Fatal(String),
}

#[derive(Debug)]
struct SoakStats {
    disjoint_commits: u64,
    hot_commits: u64,
    conflicts: u64,
}

struct FixedWorkerPool {
    size: usize,
    commands: Vec<mpsc::Sender<Command>>,
    events: mpsc::Receiver<Event>,
    handles: Vec<JoinHandle<()>>,
    thread_ids: Vec<ThreadId>,
}

impl FixedWorkerPool {
    fn start(
        size: usize,
        db: Arc<LocalDb>,
        table_name: &[u8],
        table_id: u64,
    ) -> std::result::Result<Self, String> {
        let (event_tx, events) = mpsc::channel();
        let mut commands = Vec::with_capacity(size);
        let mut handles = Vec::with_capacity(size);

        for worker_id in 0..size {
            let (command_tx, command_rx) = mpsc::channel();
            commands.push(command_tx);
            let db = Arc::clone(&db);
            let table_name = table_name.to_vec();
            let event_tx = event_tx.clone();
            let handle = thread::Builder::new()
                .name(format!("mako-local-fixed-{size}-{worker_id}"))
                .spawn(move || {
                    worker_loop(worker_id, db, &table_name, table_id, command_rx, event_tx);
                })
                .map_err(|error| {
                    format!("pool {size}: could not spawn fixed worker {worker_id}: {error}")
                })?;
            handles.push(handle);
        }
        drop(event_tx);

        let deadline = Instant::now() + STARTUP_TIMEOUT;
        let mut attached = vec![None; size];
        let mut unique_threads = HashSet::with_capacity(size);
        while attached.iter().any(Option::is_none) {
            let event = receive_before(&events, deadline, &format!("pool {size} startup"))?;
            if event.worker_id >= size {
                return Err(format!(
                    "pool {size}: startup reported out-of-range worker {}",
                    event.worker_id
                ));
            }
            match event.kind {
                EventKind::Attached => {
                    if attached[event.worker_id].replace(event.thread_id).is_some() {
                        return Err(format!(
                            "pool {size}: worker {} attached more than once",
                            event.worker_id
                        ));
                    }
                    if !unique_threads.insert(event.thread_id) {
                        return Err(format!(
                            "pool {size}: two logical workers reported OS thread {:?}",
                            event.thread_id
                        ));
                    }
                }
                EventKind::Fatal(detail) => {
                    return Err(format!(
                        "pool {size}: worker {} failed during attachment: {detail}",
                        event.worker_id
                    ));
                }
                other => {
                    return Err(format!(
                        "pool {size}: unexpected startup event from worker {}: {other:?}",
                        event.worker_id
                    ));
                }
            }
        }

        let thread_ids = attached
            .into_iter()
            .map(|thread_id| thread_id.expect("attachment completeness checked above"))
            .collect();
        Ok(Self {
            size,
            commands,
            events,
            handles,
            thread_ids,
        })
    }

    fn broadcast<F>(&self, mut command: F) -> std::result::Result<(), String>
    where
        F: FnMut(usize) -> Command,
    {
        for (worker_id, sender) in self.commands.iter().enumerate() {
            sender.send(command(worker_id)).map_err(|_| {
                format!(
                    "pool {}: fixed worker {worker_id} exited before accepting its command",
                    self.size
                )
            })?;
        }
        Ok(())
    }

    fn next_event(&self, deadline: Instant, phase: &str) -> std::result::Result<Event, String> {
        let event = receive_before(&self.events, deadline, phase)?;
        let Some(expected_thread) = self.thread_ids.get(event.worker_id) else {
            return Err(format!(
                "pool {} {phase}: out-of-range worker {}",
                self.size, event.worker_id
            ));
        };
        if event.thread_id != *expected_thread {
            return Err(format!(
                "pool {} {phase}: worker {} moved from OS thread {expected_thread:?} to {:?}",
                self.size, event.worker_id, event.thread_id
            ));
        }
        if let EventKind::Fatal(detail) = &event.kind {
            return Err(format!(
                "pool {} {phase}: worker {} failed: {detail}",
                self.size, event.worker_id
            ));
        }
        Ok(event)
    }

    fn shutdown(mut self) -> std::result::Result<(), String> {
        for sender in &self.commands {
            let _ = sender.send(Command::Shutdown);
        }
        self.commands.clear();

        let deadline = Instant::now() + PHASE_TIMEOUT;
        let mut stopped = HashSet::with_capacity(self.size);
        while stopped.len() != self.size {
            let event = self.next_event(deadline, "shutdown")?;
            match event.kind {
                EventKind::Stopped => {
                    if !stopped.insert(event.worker_id) {
                        return Err(format!(
                            "pool {}: worker {} stopped more than once",
                            self.size, event.worker_id
                        ));
                    }
                }
                other => {
                    return Err(format!(
                        "pool {}: unexpected shutdown event from worker {}: {other:?}",
                        self.size, event.worker_id
                    ));
                }
            }
        }

        for (worker_id, handle) in self.handles.drain(..).enumerate() {
            handle.join().map_err(|payload| {
                format!(
                    "pool {}: fixed worker {worker_id} panicked after reporting shutdown: {}",
                    self.size,
                    panic_message(payload)
                )
            })?;
        }
        Ok(())
    }
}

fn receive_before(
    receiver: &mpsc::Receiver<Event>,
    deadline: Instant,
    context: &str,
) -> std::result::Result<Event, String> {
    let remaining = deadline.saturating_duration_since(Instant::now());
    if remaining.is_zero() {
        return Err(format!("{context}: deadline expired before the next event"));
    }
    receiver
        .recv_timeout(remaining)
        .map_err(|error| match error {
            mpsc::RecvTimeoutError::Timeout => {
                format!("{context}: no worker event within the bounded timeout")
            }
            mpsc::RecvTimeoutError::Disconnected => {
                format!("{context}: all worker event senders disconnected")
            }
        })
}

fn panic_message(payload: Box<dyn std::any::Any + Send>) -> String {
    payload
        .downcast_ref::<&str>()
        .map(|message| (*message).to_owned())
        .or_else(|| payload.downcast_ref::<String>().cloned())
        .unwrap_or_else(|| "non-string panic payload".to_owned())
}

fn worker_loop(
    worker_id: usize,
    db: Arc<LocalDb>,
    table_name: &[u8],
    table_id: u64,
    commands: mpsc::Receiver<Command>,
    events: mpsc::Sender<Event>,
) {
    let thread_id = thread::current().id();
    let table = match db.open_table(table_name, table_id) {
        Ok(table) => table,
        Err(error) => {
            let _ = events.send(Event {
                worker_id,
                thread_id,
                kind: EventKind::Fatal(format!("open_table returned {error}")),
            });
            return;
        }
    };
    if events
        .send(Event {
            worker_id,
            thread_id,
            kind: EventKind::Attached,
        })
        .is_err()
    {
        return;
    }

    while let Ok(command) = commands.recv() {
        let kind = match command {
            Command::ForceConflict { release } => {
                match stage_forced_conflict(worker_id, &db, &table, &events, thread_id, release) {
                    Some(result) => EventKind::ConflictDone(result),
                    None => break,
                }
            }
            Command::ExplicitAbort => {
                EventKind::AbortDone(exercise_explicit_abort(worker_id, &db, &table))
            }
            Command::Soak { release } => {
                if events
                    .send(Event {
                        worker_id,
                        thread_id,
                        kind: EventKind::SoakReady,
                    })
                    .is_err()
                {
                    break;
                }
                let result = release
                    .recv_timeout(RELEASE_TIMEOUT)
                    .map_err(|error| format!("soak release failed: {error}"))
                    .and_then(|()| soak(worker_id, &db, &table));
                EventKind::SoakDone(result)
            }
            Command::Shutdown => break,
        };
        if events
            .send(Event {
                worker_id,
                thread_id,
                kind,
            })
            .is_err()
        {
            return;
        }
    }

    let _ = events.send(Event {
        worker_id,
        thread_id,
        kind: EventKind::Stopped,
    });
}

fn stage_forced_conflict(
    worker_id: usize,
    db: &LocalDb,
    table: &Table<'_>,
    events: &mpsc::Sender<Event>,
    thread_id: ThreadId,
    release: mpsc::Receiver<()>,
) -> Option<std::result::Result<(), String>> {
    let result = (|| {
        let mut transaction = db
            .transaction()
            .map_err(|error| format!("begin forced-conflict transaction: {error}"))?;
        let observed = transaction
            .get(table, b"forced-conflict")
            .map_err(|error| format!("read forced-conflict seed: {error}"))?;
        if observed.as_deref() != Some(encode_u64(0).as_slice()) {
            return Err(format!(
                "forced-conflict seed was {:?}, expected zero",
                observed.as_deref()
            ));
        }
        transaction
            .put(
                table,
                b"forced-conflict",
                &encode_u64(0x1000 + worker_id as u64),
            )
            .map_err(|error| format!("stage forced-conflict write: {error}"))?;

        if events
            .send(Event {
                worker_id,
                thread_id,
                kind: EventKind::ConflictReady,
            })
            .is_err()
        {
            let _ = transaction.abort();
            return Err("controller disappeared before forced conflict".to_owned());
        }
        if let Err(error) = release.recv_timeout(RELEASE_TIMEOUT) {
            let cleanup = transaction.abort();
            return Err(format!(
                "forced-conflict release failed ({error}); abort cleanup was {cleanup:?}"
            ));
        }

        match transaction.commit() {
            Err(Error::Conflict) => {}
            Ok(()) => return Err("stale forced-conflict transaction committed".to_owned()),
            Err(error) => {
                return Err(format!(
                    "forced-conflict commit returned {error}, expected Conflict"
                ));
            }
        }

        let mut recovery = db
            .transaction()
            .map_err(|error| format!("begin post-conflict recovery: {error}"))?;
        recovery
            .put(
                table,
                &worker_key(b"post-conflict", worker_id),
                b"recovered",
            )
            .map_err(|error| format!("stage post-conflict recovery: {error}"))?;
        recovery
            .commit()
            .map_err(|error| format!("commit post-conflict recovery: {error}"))
    })();
    Some(result)
}

fn exercise_explicit_abort(
    worker_id: usize,
    db: &LocalDb,
    table: &Table<'_>,
) -> std::result::Result<(), String> {
    let aborted_key = worker_key(b"aborted", worker_id);
    let mut transaction = db
        .transaction()
        .map_err(|error| format!("begin explicit-abort transaction: {error}"))?;
    transaction
        .put(table, &aborted_key, b"must-not-be-visible")
        .map_err(|error| format!("stage explicit-abort write: {error}"))?;
    transaction
        .abort()
        .map_err(|error| format!("explicit abort cleanup: {error}"))?;

    // All workers verify different absent keys concurrently. Masstree may
    // legitimately invalidate those negative reads when a neighboring
    // recovery marker changes the same leaf, so retry the complete proof. A
    // leaked lock or poisoned worker cannot make progress through this bound.
    for _ in 0..RECOVERY_RETRY_BUDGET {
        let mut recovery = db
            .transaction()
            .map_err(|error| format!("begin post-abort recovery: {error}"))?;
        match recovery.get(table, &aborted_key) {
            Ok(None) => {}
            Ok(Some(_)) => return Err("explicitly aborted write became visible".to_owned()),
            Err(Error::Conflict) => {
                thread::yield_now();
                continue;
            }
            Err(error) => return Err(format!("verify explicit abort: {error}")),
        }
        match recovery.put(table, &worker_key(b"post-abort", worker_id), b"recovered") {
            Ok(_) => {}
            Err(Error::Conflict) => {
                thread::yield_now();
                continue;
            }
            Err(error) => return Err(format!("stage post-abort recovery: {error}")),
        }
        match recovery.commit() {
            Ok(()) => return Ok(()),
            Err(Error::Conflict) => thread::yield_now(),
            Err(error) => return Err(format!("commit post-abort recovery: {error}")),
        }
    }
    Err(format!(
        "post-abort recovery exhausted {RECOVERY_RETRY_BUDGET} OCC retries"
    ))
}

fn soak(
    worker_id: usize,
    db: &LocalDb,
    table: &Table<'_>,
) -> std::result::Result<SoakStats, String> {
    let progress_key = worker_key(b"progress", worker_id);
    let mut conflicts = 0u64;
    let mut hot_commits = 0u64;

    for sequence in 1..=DISJOINT_COMMITS_PER_WORKER {
        let mut transaction = db
            .transaction()
            .map_err(|error| format!("begin disjoint soak transaction {sequence}: {error}"))?;
        transaction
            .put(table, &progress_key, &encode_u64(sequence))
            .map_err(|error| format!("stage disjoint soak transaction {sequence}: {error}"))?;
        transaction
            .commit()
            .map_err(|error| format!("commit disjoint soak transaction {sequence}: {error}"))?;

        if sequence % (DISJOINT_COMMITS_PER_WORKER / HOT_UPDATES_PER_WORKER) == 0 {
            let mut committed = false;
            for _ in 0..HOT_RETRY_BUDGET {
                match try_hot_increment(db, table)? {
                    Increment::Committed => {
                        committed = true;
                        hot_commits += 1;
                        break;
                    }
                    Increment::Conflict => {
                        conflicts += 1;
                        thread::yield_now();
                    }
                }
            }
            if !committed {
                return Err(format!(
                    "hot update {hot_commits} exhausted {HOT_RETRY_BUDGET} OCC retries"
                ));
            }
        }
    }

    Ok(SoakStats {
        disjoint_commits: DISJOINT_COMMITS_PER_WORKER,
        hot_commits,
        conflicts,
    })
}

enum Increment {
    Committed,
    Conflict,
}

fn try_hot_increment(db: &LocalDb, table: &Table<'_>) -> std::result::Result<Increment, String> {
    let mut transaction = db
        .transaction()
        .map_err(|error| format!("begin hot-counter transaction: {error}"))?;
    let current = match transaction.get(table, b"hot-counter") {
        Ok(Some(value)) => decode_u64(&value)?,
        Ok(None) => return Err("hot counter disappeared".to_owned()),
        Err(Error::Conflict) => return Ok(Increment::Conflict),
        Err(error) => return Err(format!("read hot counter: {error}")),
    };
    match transaction.put(table, b"hot-counter", &encode_u64(current + 1)) {
        Ok(_) => {}
        Err(Error::Conflict) => return Ok(Increment::Conflict),
        Err(error) => return Err(format!("stage hot counter: {error}")),
    }
    match transaction.commit() {
        Ok(()) => Ok(Increment::Committed),
        Err(Error::Conflict) => Ok(Increment::Conflict),
        Err(error) => Err(format!("commit hot counter: {error}")),
    }
}

fn worker_key(prefix: &[u8], worker_id: usize) -> Vec<u8> {
    let mut key = Vec::with_capacity(prefix.len() + 1 + size_of::<u64>());
    key.extend_from_slice(prefix);
    key.push(0);
    key.extend_from_slice(&(worker_id as u64).to_be_bytes());
    key
}

fn encode_u64(value: u64) -> [u8; 8] {
    value.to_be_bytes()
}

fn decode_u64(bytes: &[u8]) -> std::result::Result<u64, String> {
    let encoded: [u8; 8] = bytes
        .try_into()
        .map_err(|_| format!("counter has invalid {}-byte encoding", bytes.len()))?;
    Ok(u64::from_be_bytes(encoded))
}

fn seed(pool_size: usize, table: &Table<'_>, db: &LocalDb) -> std::result::Result<(), String> {
    let mut transaction = db
        .transaction()
        .map_err(|error| format!("begin pool seed: {error}"))?;
    transaction
        .put(table, b"forced-conflict", &encode_u64(0))
        .map_err(|error| format!("seed forced-conflict key: {error}"))?;
    transaction
        .put(table, b"hot-counter", &encode_u64(0))
        .map_err(|error| format!("seed hot counter: {error}"))?;
    // Pre-create every key that the concurrent recovery/progress phases
    // update. Otherwise disjoint logical inserts may legitimately conflict on
    // a shared Masstree structural version, obscuring the lock-leak signal.
    for worker_id in 0..pool_size {
        for prefix in [b"post-conflict".as_slice(), b"post-abort".as_slice()] {
            transaction
                .put(table, &worker_key(prefix, worker_id), b"pending")
                .map_err(|error| {
                    format!(
                        "seed worker {worker_id} {} marker: {error}",
                        String::from_utf8_lossy(prefix)
                    )
                })?;
        }
        transaction
            .put(table, &worker_key(b"progress", worker_id), &encode_u64(0))
            .map_err(|error| format!("seed worker {worker_id} progress: {error}"))?;
    }
    transaction
        .commit()
        .map_err(|error| format!("commit pool seed: {error}"))
}

fn force_conflicts(
    pool: &FixedWorkerPool,
    db: &LocalDb,
    table: &Table<'_>,
) -> std::result::Result<(), String> {
    let mut releases = Vec::with_capacity(pool.size);
    let mut release_receivers = Vec::with_capacity(pool.size);
    for _ in 0..pool.size {
        let (sender, receiver) = mpsc::channel();
        releases.push(sender);
        release_receivers.push(Some(receiver));
    }
    pool.broadcast(|worker_id| Command::ForceConflict {
        release: release_receivers[worker_id]
            .take()
            .expect("one release receiver per worker"),
    })?;

    let ready_result = (|| {
        let deadline = Instant::now() + PHASE_TIMEOUT;
        let mut ready = HashSet::with_capacity(pool.size);
        while ready.len() != pool.size {
            let event = pool.next_event(deadline, "forced-conflict readiness")?;
            match event.kind {
                EventKind::ConflictReady => {
                    if !ready.insert(event.worker_id) {
                        return Err(format!(
                            "pool {}: worker {} reported conflict readiness twice",
                            pool.size, event.worker_id
                        ));
                    }
                }
                other => {
                    return Err(format!(
                        "pool {}: unexpected conflict-readiness event from worker {}: {other:?}",
                        pool.size, event.worker_id
                    ));
                }
            }
        }

        let mut controller = db
            .transaction()
            .map_err(|error| format!("pool {} controller begin: {error}", pool.size))?;
        controller
            .put(
                table,
                b"forced-conflict",
                &encode_u64(0xc000 + pool.size as u64),
            )
            .map_err(|error| format!("pool {} controller put: {error}", pool.size))?;
        controller
            .commit()
            .map_err(|error| format!("pool {} controller commit: {error}", pool.size))
    })();

    // Release every staged transaction even when controller setup fails. This
    // keeps the bounded-error path from stranding a worker with live TLS state.
    for release in releases {
        let _ = release.send(());
    }
    ready_result?;

    let deadline = Instant::now() + PHASE_TIMEOUT;
    let mut done = HashSet::with_capacity(pool.size);
    let mut failures = Vec::new();
    while done.len() != pool.size {
        let event = pool.next_event(deadline, "forced-conflict completion")?;
        match event.kind {
            EventKind::ConflictDone(result) => {
                if let Err(detail) = result {
                    failures.push(format!(
                        "pool {}: worker {} forced-conflict failure: {detail}",
                        pool.size, event.worker_id
                    ));
                }
                if !done.insert(event.worker_id) {
                    return Err(format!(
                        "pool {}: worker {} completed forced conflict twice",
                        pool.size, event.worker_id
                    ));
                }
            }
            other => {
                return Err(format!(
                    "pool {}: unexpected conflict-completion event from worker {}: {other:?}",
                    pool.size, event.worker_id
                ));
            }
        }
    }
    if failures.is_empty() {
        Ok(())
    } else {
        Err(failures.join("; "))
    }
}

fn explicit_aborts(pool: &FixedWorkerPool) -> std::result::Result<(), String> {
    pool.broadcast(|_| Command::ExplicitAbort)?;
    let deadline = Instant::now() + PHASE_TIMEOUT;
    let mut done = HashSet::with_capacity(pool.size);
    let mut failures = Vec::new();
    while done.len() != pool.size {
        let event = pool.next_event(deadline, "explicit-abort completion")?;
        match event.kind {
            EventKind::AbortDone(result) => {
                if let Err(detail) = result {
                    failures.push(format!(
                        "pool {}: worker {} explicit-abort failure: {detail}",
                        pool.size, event.worker_id
                    ));
                }
                if !done.insert(event.worker_id) {
                    return Err(format!(
                        "pool {}: worker {} completed explicit abort twice",
                        pool.size, event.worker_id
                    ));
                }
            }
            other => {
                return Err(format!(
                    "pool {}: unexpected abort event from worker {}: {other:?}",
                    pool.size, event.worker_id
                ));
            }
        }
    }
    if failures.is_empty() {
        Ok(())
    } else {
        Err(failures.join("; "))
    }
}

fn run_soak(pool: &FixedWorkerPool) -> std::result::Result<u64, String> {
    let mut releases = Vec::with_capacity(pool.size);
    let mut release_receivers = Vec::with_capacity(pool.size);
    for _ in 0..pool.size {
        let (sender, receiver) = mpsc::channel();
        releases.push(sender);
        release_receivers.push(Some(receiver));
    }
    pool.broadcast(|worker_id| Command::Soak {
        release: release_receivers[worker_id]
            .take()
            .expect("one release receiver per worker"),
    })?;

    let ready_result = (|| {
        let deadline = Instant::now() + PHASE_TIMEOUT;
        let mut ready = HashSet::with_capacity(pool.size);
        while ready.len() != pool.size {
            let event = pool.next_event(deadline, "soak readiness")?;
            match event.kind {
                EventKind::SoakReady => {
                    if !ready.insert(event.worker_id) {
                        return Err(format!(
                            "pool {}: worker {} reported soak readiness twice",
                            pool.size, event.worker_id
                        ));
                    }
                }
                other => {
                    return Err(format!(
                        "pool {}: unexpected soak-readiness event from worker {}: {other:?}",
                        pool.size, event.worker_id
                    ));
                }
            }
        }
        Ok(())
    })();
    for release in releases {
        let _ = release.send(());
    }
    ready_result?;

    let deadline = Instant::now() + SOAK_TIMEOUT;
    let mut done = HashSet::with_capacity(pool.size);
    let mut total_conflicts = 0u64;
    let mut failures = Vec::new();
    while done.len() != pool.size {
        let event = pool.next_event(deadline, "soak completion")?;
        match event.kind {
            EventKind::SoakDone(result) => {
                match result {
                    Ok(stats) => {
                        if stats.disjoint_commits != DISJOINT_COMMITS_PER_WORKER
                            || stats.hot_commits != HOT_UPDATES_PER_WORKER
                        {
                            failures.push(format!(
                                "pool {}: worker {} incomplete soak progress: {stats:?}",
                                pool.size, event.worker_id
                            ));
                        }
                        total_conflicts += stats.conflicts;
                    }
                    Err(detail) => failures.push(format!(
                        "pool {}: worker {} soak failure: {detail}",
                        pool.size, event.worker_id
                    )),
                }
                if !done.insert(event.worker_id) {
                    return Err(format!(
                        "pool {}: worker {} completed soak twice",
                        pool.size, event.worker_id
                    ));
                }
            }
            other => {
                return Err(format!(
                    "pool {}: unexpected soak event from worker {}: {other:?}",
                    pool.size, event.worker_id
                ));
            }
        }
    }
    if failures.is_empty() {
        Ok(total_conflicts)
    } else {
        Err(failures.join("; "))
    }
}

fn verify_final_state(
    pool_size: usize,
    db: &LocalDb,
    table: &Table<'_>,
) -> std::result::Result<(), String> {
    let mut transaction = db
        .transaction()
        .map_err(|error| format!("pool {pool_size}: begin final verification: {error}"))?;
    let forced = transaction
        .get(table, b"forced-conflict")
        .map_err(|error| format!("pool {pool_size}: read forced-conflict result: {error}"))?;
    if forced.as_deref() != Some(encode_u64(0xc000 + pool_size as u64).as_slice()) {
        return Err(format!(
            "pool {pool_size}: forced-conflict final value was {:?}",
            forced.as_deref()
        ));
    }

    let hot = transaction
        .get(table, b"hot-counter")
        .map_err(|error| format!("pool {pool_size}: read final hot counter: {error}"))?
        .ok_or_else(|| format!("pool {pool_size}: final hot counter is missing"))?;
    let expected_hot = pool_size as u64 * HOT_UPDATES_PER_WORKER;
    if decode_u64(&hot)? != expected_hot {
        return Err(format!(
            "pool {pool_size}: hot counter did not reach {expected_hot}: {hot:?}"
        ));
    }

    for worker_id in 0..pool_size {
        let aborted = transaction
            .get(table, &worker_key(b"aborted", worker_id))
            .map_err(|error| {
                format!("pool {pool_size}: read worker {worker_id} aborted key: {error}")
            })?;
        if aborted.is_some() {
            return Err(format!(
                "pool {pool_size}: worker {worker_id} explicit-abort write is visible"
            ));
        }
        for prefix in [b"post-conflict".as_slice(), b"post-abort".as_slice()] {
            let recovery = transaction
                .get(table, &worker_key(prefix, worker_id))
                .map_err(|error| {
                    format!(
                        "pool {pool_size}: read worker {worker_id} {} marker: {error}",
                        String::from_utf8_lossy(prefix)
                    )
                })?;
            if recovery.as_deref() != Some(b"recovered") {
                return Err(format!(
                    "pool {pool_size}: worker {worker_id} {} marker was {recovery:?}",
                    String::from_utf8_lossy(prefix)
                ));
            }
        }
        let progress = transaction
            .get(table, &worker_key(b"progress", worker_id))
            .map_err(|error| {
                format!("pool {pool_size}: read worker {worker_id} progress: {error}")
            })?
            .ok_or_else(|| format!("pool {pool_size}: worker {worker_id} made no progress"))?;
        if decode_u64(&progress)? != DISJOINT_COMMITS_PER_WORKER {
            return Err(format!(
                "pool {pool_size}: worker {worker_id} progress was {progress:?}"
            ));
        }
    }

    transaction
        .put(table, b"post-pool-sentinel", b"unlocked")
        .map_err(|error| format!("pool {pool_size}: stage final lock sentinel: {error}"))?;
    transaction
        .commit()
        .map_err(|error| format!("pool {pool_size}: commit final lock sentinel: {error}"))
}

fn run_pool(size: usize) -> std::result::Result<u64, String> {
    let db =
        Arc::new(LocalDb::open().map_err(|error| format!("pool {size}: open database: {error}"))?);
    let table_name = format!("rust-fixed-worker-pool-{size}").into_bytes();
    let table_id = 24_000 + size as u64;
    let table = db
        .open_table(&table_name, table_id)
        .map_err(|error| format!("pool {size}: open controller table: {error}"))?;
    seed(size, &table, &db)?;

    let pool = FixedWorkerPool::start(size, Arc::clone(&db), &table_name, table_id)?;
    let phases = (|| {
        force_conflicts(&pool, &db, &table)?;
        explicit_aborts(&pool)?;
        let observed_soak_conflicts = run_soak(&pool)?;
        verify_final_state(size, &db, &table)?;
        Ok(observed_soak_conflicts)
    })();
    let shutdown = pool.shutdown();
    match (phases, shutdown) {
        (Ok(conflicts), Ok(())) => Ok(conflicts),
        (Err(phase), Ok(())) => Err(phase),
        (Ok(_), Err(shutdown)) => Err(shutdown),
        (Err(phase), Err(shutdown)) => Err(format!("{phase}; shutdown also failed: {shutdown}")),
    }
}

#[test]
fn fixed_worker_pools_survive_conflict_abort_progress_and_soak() {
    let mut observed_soak_conflicts = Vec::new();
    for size in POOL_SIZES {
        let conflicts = run_pool(size)
            .unwrap_or_else(|detail| panic!("fixed worker-pool gate failed: {detail}"));
        observed_soak_conflicts.push((size, conflicts));
    }

    // The forced phase deterministically proves conflict recovery for every
    // pool. Soak conflicts are scheduler-dependent and diagnostic only.
    eprintln!("fixed worker-pool soak conflicts: {observed_soak_conflicts:?}");
}
