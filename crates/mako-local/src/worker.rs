//! Fixed, thread-affine workers for synchronous local transactions.
//!
//! [`Transaction`](crate::Transaction) is deliberately not `Send`, so an async
//! application must send owned application work to a long-lived OS worker
//! instead of moving a transaction through an executor. [`FixedWorkerPool`]
//! provides that boundary. A submitted closure runs to completion on one
//! worker and receives a borrowed [`LocalDb`]; its result is delivered through
//! a [`Task`] that can be awaited or synchronously waited on.
//!
//! Worker queues are bounded. Dropping a task does not cancel accepted work:
//! interrupting a closure while it owns native transaction state would make
//! cleanup ambiguous. After every closure (including a panic when unwinding is
//! enabled), the adapter checks native worker health before completing its
//! task. A poisoned worker is permanently retired, its queued tasks fail, and
//! the transition is visible in [`FixedWorkerPool::metrics`].
//!
//! Conflict retry is opt-in and bounded. [`retry_transaction`] reruns the whole
//! closure only for [`Error::Conflict`]; callers must keep externally visible
//! side effects after the successful transaction or make them idempotent.
//!
//! ```no_run
//! # use std::sync::Arc;
//! # use mako_local::{Error, LocalDb};
//! # use mako_local::worker::{FixedWorkerPool, FixedWorkerPoolOptions, RetryPolicy};
//! # fn main() -> Result<(), Box<dyn std::error::Error>> {
//! let db = Arc::new(LocalDb::open()?);
//! let pool = FixedWorkerPool::start(Arc::clone(&db), FixedWorkerPoolOptions {
//!     worker_count: 4,
//!     queue_capacity_per_worker: 64,
//! })?;
//!
//! let transfer = pool.submit_retrying(RetryPolicy::new(8), |db| {
//!     let accounts = db.open_table("accounts", 1)?;
//!     let mut transaction = db.transaction()?;
//!     transaction.put(&accounts, b"alice", b"9")?;
//!     transaction.put(&accounts, b"bob", b"21")?;
//!     transaction.commit()?;
//!     Ok::<_, Error>(())
//! });
//!
//! let retry_report = transfer.wait()??;
//! println!("conflicts before success: {}", retry_report.conflicts());
//! pool.shutdown()?;
//! # Ok(())
//! # }
//! ```

use std::fmt;
use std::future::Future;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::pin::Pin;
use std::sync::atomic::{AtomicU64, AtomicU8, AtomicUsize, Ordering};
use std::sync::{mpsc, Arc, Condvar, Mutex};
use std::task::{Context, Poll, Waker};
use std::thread::{self, JoinHandle};

use crate::{worker_health, Error, LocalDb, WorkerHealth};

const DEFAULT_QUEUE_CAPACITY_PER_WORKER: usize = 64;

/// Fixed-worker construction settings.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct FixedWorkerPoolOptions {
    /// Number of long-lived native transaction workers.
    pub worker_count: usize,
    /// Maximum accepted-but-not-started jobs in each worker's private queue.
    pub queue_capacity_per_worker: usize,
}

impl Default for FixedWorkerPoolOptions {
    fn default() -> Self {
        Self {
            worker_count: 1,
            queue_capacity_per_worker: DEFAULT_QUEUE_CAPACITY_PER_WORKER,
        }
    }
}

/// Which fixed-worker construction setting was invalid.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum PoolConfigError {
    /// At least one worker is required.
    ZeroWorkers,
    /// The requested pool cannot fit after the database-opening thread's
    /// mandatory process-lifetime STO attachment.
    TooManyWorkers {
        /// Requested fixed-worker count.
        requested: usize,
        /// Largest pool that can fit in a fresh process.
        maximum: usize,
    },
    /// Each worker needs at least one bounded queue slot.
    ZeroQueueCapacity,
}

impl fmt::Display for PoolConfigError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::ZeroWorkers => write!(f, "a fixed-worker pool needs at least one worker"),
            Self::TooManyWorkers { requested, maximum } => write!(
                f,
                "fixed-worker count {requested} exceeds the fresh-process maximum {maximum}"
            ),
            Self::ZeroQueueCapacity => {
                write!(f, "a fixed-worker queue needs at least one slot")
            }
        }
    }
}

impl std::error::Error for PoolConfigError {}

/// Failure to construct and attach every configured worker.
#[derive(Debug)]
pub enum PoolStartError {
    /// The requested pool shape is invalid.
    InvalidConfig(PoolConfigError),
    /// Rust could not create an OS worker.
    Spawn {
        /// Zero-based worker index.
        worker: usize,
        /// Thread creation failure.
        source: std::io::Error,
    },
    /// A worker could not attach to the native STO runtime.
    Attach {
        /// Zero-based worker index.
        worker: usize,
        /// Native attachment failure.
        source: Error,
    },
    /// A worker ended before reporting its attachment result.
    StartupTerminated {
        /// Zero-based worker index.
        worker: usize,
    },
}

impl fmt::Display for PoolStartError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::InvalidConfig(error) => error.fmt(f),
            Self::Spawn { worker, source } => {
                write!(f, "could not spawn fixed worker {worker}: {source}")
            }
            Self::Attach { worker, source } => {
                write!(f, "fixed worker {worker} could not attach: {source}")
            }
            Self::StartupTerminated { worker } => {
                write!(f, "fixed worker {worker} ended during startup")
            }
        }
    }
}

impl std::error::Error for PoolStartError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::InvalidConfig(error) => Some(error),
            Self::Spawn { source, .. } => Some(source),
            Self::Attach { source, .. } => Some(source),
            Self::StartupTerminated { .. } => None,
        }
    }
}

/// Failure reported by an accepted or rejected worker task.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum TaskError {
    /// Every healthy worker queue was full at submission time.
    QueueFull,
    /// The pool has no worker that can safely execute more native work.
    NoHealthyWorkers,
    /// Native cleanup became uncertain on this worker, so it was retired.
    WorkerPoisoned {
        /// Zero-based worker index.
        worker: usize,
    },
    /// The post-job health check failed or stopped reporting an attached worker.
    WorkerHealthLost {
        /// Zero-based worker index.
        worker: usize,
        /// Typed native health failure.
        source: Error,
    },
    /// The application closure unwound. Its worker remained healthy and reusable.
    ApplicationPanicked {
        /// Zero-based worker index.
        worker: usize,
    },
    /// The worker infrastructure ended without resolving this task.
    WorkerStopped,
}

impl fmt::Display for TaskError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::QueueFull => write!(f, "every healthy fixed-worker queue is full"),
            Self::NoHealthyWorkers => write!(f, "the fixed-worker pool has no healthy worker"),
            Self::WorkerPoisoned { worker } => {
                write!(f, "fixed worker {worker} was poisoned and retired")
            }
            Self::WorkerHealthLost { worker, source } => {
                write!(f, "fixed worker {worker} lost native health: {source}")
            }
            Self::ApplicationPanicked { worker } => {
                write!(f, "application job panicked on fixed worker {worker}")
            }
            Self::WorkerStopped => write!(f, "fixed worker stopped before resolving the task"),
        }
    }
}

impl std::error::Error for TaskError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::WorkerHealthLost { source, .. } => Some(source),
            _ => None,
        }
    }
}

/// A future result from one fixed-worker submission.
///
/// Dropping this value only abandons the result. Once accepted, its application
/// closure still runs to completion so a native transaction is never cancelled
/// at an arbitrary instruction.
pub struct Task<T> {
    completion: Arc<Completion<T>>,
}

impl<T> Task<T> {
    /// Wait synchronously for the worker result.
    pub fn wait(self) -> Result<T, TaskError> {
        let mut state = lock_completion(&self.completion.state);
        loop {
            if let Some(result) = state.result.take() {
                return result;
            }
            assert!(!state.consumed, "worker task result was consumed twice");
            state = self
                .completion
                .ready
                .wait(state)
                .unwrap_or_else(|poisoned| poisoned.into_inner());
        }
    }
}

impl<T> Future for Task<T> {
    type Output = Result<T, TaskError>;

    fn poll(self: Pin<&mut Self>, context: &mut Context<'_>) -> Poll<Self::Output> {
        let mut state = lock_completion(&self.completion.state);
        assert!(!state.consumed, "worker task was polled after completion");
        match state.result.take() {
            Some(result) => {
                state.consumed = true;
                Poll::Ready(result)
            }
            None => {
                if state
                    .waker
                    .as_ref()
                    .is_none_or(|registered| !registered.will_wake(context.waker()))
                {
                    state.waker = Some(context.waker().clone());
                }
                Poll::Pending
            }
        }
    }
}

struct Completion<T> {
    state: Mutex<CompletionState<T>>,
    ready: Condvar,
}

struct CompletionState<T> {
    result: Option<Result<T, TaskError>>,
    waker: Option<Waker>,
    consumed: bool,
}

fn lock_completion<T>(mutex: &Mutex<T>) -> std::sync::MutexGuard<'_, T> {
    mutex
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner())
}

struct Completer<T> {
    completion: Arc<Completion<T>>,
    resolved: bool,
}

impl<T> Completer<T> {
    fn pair() -> (Task<T>, Self) {
        let completion = Arc::new(Completion {
            state: Mutex::new(CompletionState {
                result: None,
                waker: None,
                consumed: false,
            }),
            ready: Condvar::new(),
        });
        (
            Task {
                completion: Arc::clone(&completion),
            },
            Self {
                completion,
                resolved: false,
            },
        )
    }

    fn resolve(mut self, result: Result<T, TaskError>) {
        self.store(result);
    }

    fn store(&mut self, result: Result<T, TaskError>) {
        if self.resolved {
            return;
        }
        let waker = {
            let mut state = lock_completion(&self.completion.state);
            if state.consumed || state.result.is_some() {
                self.resolved = true;
                return;
            }
            state.result = Some(result);
            state.waker.take()
        };
        self.resolved = true;
        self.completion.ready.notify_all();
        if let Some(waker) = waker {
            waker.wake();
        }
    }
}

impl<T> Drop for Completer<T> {
    fn drop(&mut self) {
        self.store(Err(TaskError::WorkerStopped));
    }
}

/// Maximum number of whole-closure reruns after OCC conflicts.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct RetryPolicy {
    max_conflict_retries: u32,
}

impl RetryPolicy {
    /// Construct a policy with an explicit number of retries after the initial
    /// attempt. Zero means one attempt and no retry.
    pub const fn new(max_conflict_retries: u32) -> Self {
        Self {
            max_conflict_retries,
        }
    }

    /// Number of retries permitted after the first conflict.
    pub const fn max_conflict_retries(self) -> u32 {
        self.max_conflict_retries
    }

    /// Maximum total attempts, including the initial attempt.
    pub const fn max_attempts(self) -> u64 {
        self.max_conflict_retries as u64 + 1
    }
}

impl Default for RetryPolicy {
    fn default() -> Self {
        Self::new(0)
    }
}

/// Successful bounded-retry result and its observed conflict count.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct RetrySuccess<T> {
    value: T,
    conflicts: u64,
}

impl<T> RetrySuccess<T> {
    /// Consume the report and return the application value.
    pub fn into_value(self) -> T {
        self.value
    }

    /// Borrow the application value.
    pub const fn value(&self) -> &T {
        &self.value
    }

    /// Number of OCC conflicts before success.
    pub const fn conflicts(&self) -> u64 {
        self.conflicts
    }

    /// Total application attempts, including the successful attempt.
    pub const fn attempts(&self) -> u64 {
        self.conflicts + 1
    }
}

/// Terminal result from [`retry_transaction`].
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RetryError {
    /// Every attempt ended in `Conflict` and the explicit budget was exhausted.
    ConflictBudgetExhausted {
        /// Number of conflicts observed, equal to the configured attempts.
        conflicts: u64,
    },
    /// A non-conflict native error stopped retry immediately.
    Operation {
        /// Typed operation failure.
        source: Error,
        /// Conflicts observed before this terminal failure.
        conflicts: u64,
    },
}

impl RetryError {
    /// Conflicts observed before retry stopped.
    pub const fn conflicts(self) -> u64 {
        match self {
            Self::ConflictBudgetExhausted { conflicts } | Self::Operation { conflicts, .. } => {
                conflicts
            }
        }
    }
}

impl fmt::Display for RetryError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::ConflictBudgetExhausted { conflicts } => write!(
                f,
                "transaction conflict retry budget exhausted after {conflicts} attempts"
            ),
            Self::Operation { source, conflicts } => write!(
                f,
                "transaction retry stopped after {conflicts} conflicts: {source}"
            ),
        }
    }
}

impl std::error::Error for RetryError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::Operation { source, .. } => Some(source),
            Self::ConflictBudgetExhausted { .. } => None,
        }
    }
}

/// Run complete transaction application logic with a bounded OCC retry policy.
///
/// The closure is invoked on the calling thread and must create, use, and end a
/// fresh transaction on every call. Only [`Error::Conflict`] is retried. There
/// is no hidden sleep or exponential backoff; the helper yields the OS worker
/// once between attempts. Retrying application logic can duplicate external
/// side effects, so keep those after success or make them idempotent.
pub fn retry_transaction<T, F>(
    database: &LocalDb,
    policy: RetryPolicy,
    mut operation: F,
) -> Result<RetrySuccess<T>, RetryError>
where
    F: FnMut(&LocalDb) -> crate::Result<T>,
{
    let mut conflicts = 0u64;
    loop {
        match operation(database) {
            Ok(value) => return Ok(RetrySuccess { value, conflicts }),
            Err(Error::Conflict) => {
                conflicts += 1;
                if conflicts >= policy.max_attempts() {
                    return Err(RetryError::ConflictBudgetExhausted { conflicts });
                }
                thread::yield_now();
            }
            Err(source) => return Err(RetryError::Operation { source, conflicts }),
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(u8)]
enum WorkerState {
    Starting = 0,
    Healthy = 1,
    Poisoned = 2,
    Failed = 3,
    Stopped = 4,
}

impl WorkerState {
    fn load(state: &AtomicU8) -> Self {
        match state.load(Ordering::Acquire) {
            0 => Self::Starting,
            1 => Self::Healthy,
            2 => Self::Poisoned,
            3 => Self::Failed,
            4 => Self::Stopped,
            _ => unreachable!("invalid fixed-worker state"),
        }
    }

    fn store(self, state: &AtomicU8) {
        state.store(self as u8, Ordering::Release);
    }
}

#[derive(Debug, Default)]
struct Counters {
    accepted: AtomicU64,
    completed: AtomicU64,
    rejected: AtomicU64,
}

/// Snapshot of fixed-worker health and task accounting.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct WorkerPoolMetrics {
    /// Configured worker count.
    pub configured_workers: usize,
    /// Workers currently able to accept jobs.
    pub healthy_workers: usize,
    /// Workers permanently retired after uncertain native cleanup.
    pub poisoned_workers: usize,
    /// Workers lost for a reason other than native cleanup quarantine.
    pub failed_workers: usize,
    /// Workers that completed an orderly shutdown.
    pub stopped_workers: usize,
    /// Jobs successfully placed in a worker queue.
    pub accepted_tasks: u64,
    /// Jobs that began execution and resolved after a health check.
    pub completed_tasks: u64,
    /// Jobs rejected at submission or while draining a retired worker.
    pub rejected_tasks: u64,
}

/// Infrastructure failure while joining an orderly pool shutdown.
#[derive(Debug, Eq, PartialEq)]
pub struct PoolShutdownError {
    panicked_workers: Vec<usize>,
}

impl PoolShutdownError {
    /// Worker indexes whose infrastructure unwound outside the guarded
    /// application-closure boundary.
    pub fn panicked_workers(&self) -> &[usize] {
        &self.panicked_workers
    }
}

impl fmt::Display for PoolShutdownError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "fixed-worker infrastructure panicked on workers {:?}",
            self.panicked_workers
        )
    }
}

impl std::error::Error for PoolShutdownError {}

struct WorkerSlot {
    sender: mpsc::SyncSender<Message>,
    state: Arc<AtomicU8>,
    // Orders dispatch's health check + enqueue against retirement + draining.
    gate: Arc<Mutex<()>>,
}

enum Message {
    Run(Box<dyn ErasedJob>),
    Shutdown,
}

trait ErasedJob: Send {
    fn run(
        self: Box<Self>,
        database: &LocalDb,
        worker: usize,
        worker_state: &AtomicU8,
        worker_gate: &Mutex<()>,
        counters: &Counters,
    ) -> WorkerDirective;
    fn reject(self: Box<Self>, error: TaskError);
}

struct Job<F, T> {
    operation: Option<F>,
    completer: Option<Completer<T>>,
}

impl<F, T> ErasedJob for Job<F, T>
where
    F: FnOnce(&LocalDb) -> T + Send + 'static,
    T: Send + 'static,
{
    fn run(
        mut self: Box<Self>,
        database: &LocalDb,
        worker: usize,
        worker_state: &AtomicU8,
        worker_gate: &Mutex<()>,
        counters: &Counters,
    ) -> WorkerDirective {
        let operation = self
            .operation
            .take()
            .expect("fixed-worker job operation consumed once");
        let completer = self
            .completer
            .take()
            .expect("fixed-worker completion consumed once");
        let outcome = catch_unwind(AssertUnwindSafe(|| operation(database)));

        match worker_health() {
            Ok(WorkerHealth::Healthy) => {
                let result = outcome.map_err(|_| TaskError::ApplicationPanicked { worker });
                counters.completed.fetch_add(1, Ordering::Relaxed);
                completer.resolve(result);
                WorkerDirective::Continue
            }
            Ok(WorkerHealth::Poisoned) => {
                let error = TaskError::WorkerPoisoned { worker };
                publish_retirement(worker_state, worker_gate, WorkerState::Poisoned);
                counters.completed.fetch_add(1, Ordering::Relaxed);
                completer.resolve(Err(error));
                WorkerDirective::Retire { error }
            }
            Ok(WorkerHealth::NotAttached) => {
                let error = TaskError::WorkerHealthLost {
                    worker,
                    source: Error::NotAttached,
                };
                publish_retirement(worker_state, worker_gate, WorkerState::Failed);
                counters.completed.fetch_add(1, Ordering::Relaxed);
                completer.resolve(Err(error));
                WorkerDirective::Retire { error }
            }
            Err(source) => {
                let error = TaskError::WorkerHealthLost { worker, source };
                publish_retirement(worker_state, worker_gate, WorkerState::Failed);
                counters.completed.fetch_add(1, Ordering::Relaxed);
                completer.resolve(Err(error));
                WorkerDirective::Retire { error }
            }
        }
    }

    fn reject(mut self: Box<Self>, error: TaskError) {
        if let Some(completer) = self.completer.take() {
            completer.resolve(Err(error));
        }
    }
}

enum WorkerDirective {
    Continue,
    Retire { error: TaskError },
}

fn publish_retirement(state: &AtomicU8, gate: &Mutex<()>, retirement: WorkerState) {
    let _gate = gate.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
    retirement.store(state);
}

/// A fixed set of long-lived, thread-affine STO workers.
///
/// Each worker has a private bounded FIFO. Round-robin submission skips a
/// retired worker and never migrates a live native transaction. The pool itself
/// is safe to share behind an `Arc`; [`Self::shutdown`] requires unique
/// ownership and drains every accepted job before joining healthy workers.
pub struct FixedWorkerPool {
    // Retain the database even after every native worker has been retired.
    _database: Arc<LocalDb>,
    workers: Vec<WorkerSlot>,
    handles: Vec<Option<JoinHandle<()>>>,
    next_worker: AtomicUsize,
    counters: Arc<Counters>,
}

impl FixedWorkerPool {
    /// Start and synchronously attach every configured worker.
    ///
    /// Construction fails atomically from the caller's perspective: if any
    /// worker cannot spawn or attach, every worker already started is stopped
    /// and joined before the error returns. Native thread IDs already claimed by
    /// those workers remain process-lifetime, as required by the local ABI.
    pub fn start(
        database: Arc<LocalDb>,
        options: FixedWorkerPoolOptions,
    ) -> Result<Self, PoolStartError> {
        validate_options(options)?;

        let counters = Arc::new(Counters::default());
        let mut workers = Vec::with_capacity(options.worker_count);
        let mut handles = Vec::with_capacity(options.worker_count);

        for worker in 0..options.worker_count {
            let (sender, receiver) = mpsc::sync_channel(options.queue_capacity_per_worker);
            let (startup_sender, startup_receiver) = mpsc::sync_channel(1);
            let state = Arc::new(AtomicU8::new(WorkerState::Starting as u8));
            let gate = Arc::new(Mutex::new(()));
            let worker_database = Arc::clone(&database);
            let worker_state = Arc::clone(&state);
            let worker_gate = Arc::clone(&gate);
            let worker_counters = Arc::clone(&counters);
            let spawn = thread::Builder::new()
                .name(format!("mako-local-{worker}"))
                .spawn(move || {
                    worker_loop(
                        worker,
                        worker_database,
                        receiver,
                        startup_sender,
                        worker_state,
                        worker_gate,
                        worker_counters,
                    );
                });
            let handle = match spawn {
                Ok(handle) => handle,
                Err(source) => {
                    stop_and_join(&workers, &mut handles);
                    return Err(PoolStartError::Spawn { worker, source });
                }
            };
            workers.push(WorkerSlot {
                sender,
                state,
                gate,
            });
            handles.push(Some(handle));

            match startup_receiver.recv() {
                Ok(Ok(())) => {}
                Ok(Err(source)) => {
                    stop_and_join(&workers, &mut handles);
                    return Err(PoolStartError::Attach { worker, source });
                }
                Err(_) => {
                    stop_and_join(&workers, &mut handles);
                    return Err(PoolStartError::StartupTerminated { worker });
                }
            }
        }

        Ok(Self {
            _database: database,
            workers,
            handles,
            next_worker: AtomicUsize::new(0),
            counters,
        })
    }

    /// Submit one owned synchronous closure.
    ///
    /// The returned task resolves only after the closure has ended and the
    /// worker's native health has been checked. `T: 'static` prevents a native
    /// transaction or database borrow from escaping the worker closure.
    pub fn submit<F, T>(&self, operation: F) -> Task<T>
    where
        F: FnOnce(&LocalDb) -> T + Send + 'static,
        T: Send + 'static,
    {
        let (task, completer) = Completer::pair();
        let job: Box<dyn ErasedJob> = Box::new(Job {
            operation: Some(operation),
            completer: Some(completer),
        });
        self.dispatch(job);
        task
    }

    /// Submit whole transaction application logic with explicit bounded OCC
    /// retries on one worker.
    ///
    /// This never retries a non-conflict error. Each retry invokes `operation`
    /// again on the same OS worker and must create a fresh transaction.
    pub fn submit_retrying<F, T>(
        &self,
        policy: RetryPolicy,
        operation: F,
    ) -> Task<Result<RetrySuccess<T>, RetryError>>
    where
        F: FnMut(&LocalDb) -> crate::Result<T> + Send + 'static,
        T: Send + 'static,
    {
        self.submit(move |database| retry_transaction(database, policy, operation))
    }

    /// Return a point-in-time health and task-accounting snapshot.
    pub fn metrics(&self) -> WorkerPoolMetrics {
        metrics(&self.workers, &self.counters)
    }

    /// Drain every accepted job, stop all workers, and join their OS threads.
    ///
    /// There is intentionally no forced cancellation path. A closure that
    /// blocks forever also blocks shutdown; applications should put their own
    /// deadlines around external waits before entering a transaction.
    pub fn shutdown(mut self) -> Result<WorkerPoolMetrics, PoolShutdownError> {
        let panicked_workers = self.shutdown_workers();
        let metrics = self.metrics();
        if panicked_workers.is_empty() {
            Ok(metrics)
        } else {
            Err(PoolShutdownError { panicked_workers })
        }
    }

    fn dispatch(&self, mut job: Box<dyn ErasedJob>) {
        let start = self.next_worker.fetch_add(1, Ordering::Relaxed);
        let mut found_healthy = false;
        let mut found_full_queue = false;

        for offset in 0..self.workers.len() {
            let index = start.wrapping_add(offset) % self.workers.len();
            let worker = &self.workers[index];
            let _gate = worker
                .gate
                .lock()
                .unwrap_or_else(|poisoned| poisoned.into_inner());
            if WorkerState::load(&worker.state) != WorkerState::Healthy {
                continue;
            }
            found_healthy = true;
            match worker.sender.try_send(Message::Run(job)) {
                Ok(()) => {
                    self.counters.accepted.fetch_add(1, Ordering::Relaxed);
                    return;
                }
                Err(mpsc::TrySendError::Full(Message::Run(returned))) => {
                    found_full_queue = true;
                    job = returned;
                }
                Err(mpsc::TrySendError::Disconnected(Message::Run(returned))) => {
                    WorkerState::Failed.store(&worker.state);
                    job = returned;
                }
                Err(mpsc::TrySendError::Full(Message::Shutdown))
                | Err(mpsc::TrySendError::Disconnected(Message::Shutdown)) => {
                    unreachable!("dispatch only sends jobs")
                }
            }
        }

        self.counters.rejected.fetch_add(1, Ordering::Relaxed);
        let error = if found_healthy && found_full_queue {
            TaskError::QueueFull
        } else {
            TaskError::NoHealthyWorkers
        };
        job.reject(error);
    }

    fn shutdown_workers(&mut self) -> Vec<usize> {
        for worker in &self.workers {
            let _ = worker.sender.send(Message::Shutdown);
        }

        let mut panicked = Vec::new();
        for (worker, handle) in self.handles.iter_mut().enumerate() {
            if handle.take().is_some_and(|handle| handle.join().is_err()) {
                WorkerState::Failed.store(&self.workers[worker].state);
                panicked.push(worker);
            }
        }
        panicked
    }
}

impl Drop for FixedWorkerPool {
    fn drop(&mut self) {
        let _ = self.shutdown_workers();
    }
}

fn validate_options(options: FixedWorkerPoolOptions) -> Result<(), PoolStartError> {
    if options.worker_count == 0 {
        return Err(PoolStartError::InvalidConfig(PoolConfigError::ZeroWorkers));
    }
    // Constructing the LocalDb supplied to this pool necessarily attached its
    // opening thread, and native worker IDs are never recycled. Reject a shape
    // that cannot possibly fit before it consumes every remaining slot.
    let maximum = crate::MAX_WORKERS.saturating_sub(1);
    if options.worker_count > maximum {
        return Err(PoolStartError::InvalidConfig(
            PoolConfigError::TooManyWorkers {
                requested: options.worker_count,
                maximum,
            },
        ));
    }
    if options.queue_capacity_per_worker == 0 {
        return Err(PoolStartError::InvalidConfig(
            PoolConfigError::ZeroQueueCapacity,
        ));
    }
    Ok(())
}

fn stop_and_join(workers: &[WorkerSlot], handles: &mut [Option<JoinHandle<()>>]) {
    for worker in workers {
        let _ = worker.sender.send(Message::Shutdown);
    }
    for handle in handles {
        if let Some(handle) = handle.take() {
            let _ = handle.join();
        }
    }
}

fn worker_loop(
    worker: usize,
    database: Arc<LocalDb>,
    receiver: mpsc::Receiver<Message>,
    startup: mpsc::SyncSender<crate::Result<()>>,
    state: Arc<AtomicU8>,
    gate: Arc<Mutex<()>>,
    counters: Arc<Counters>,
) {
    let attached = super::attach_current_thread().and_then(|()| match worker_health()? {
        WorkerHealth::Healthy => Ok(()),
        WorkerHealth::NotAttached => Err(Error::NotAttached),
        WorkerHealth::Poisoned => Err(Error::WorkerPoisoned),
    });
    match attached {
        Ok(()) => WorkerState::Healthy.store(&state),
        Err(_) => WorkerState::Failed.store(&state),
    }
    if startup.send(attached).is_err() || WorkerState::load(&state) != WorkerState::Healthy {
        return;
    }

    while let Ok(message) = receiver.recv() {
        match message {
            Message::Shutdown => {
                WorkerState::Stopped.store(&state);
                return;
            }
            Message::Run(job) => match job.run(&database, worker, &state, &gate, &counters) {
                WorkerDirective::Continue => {}
                WorkerDirective::Retire { error } => {
                    reject_pending(&receiver, error, &counters);
                    return;
                }
            },
        }
    }
    if WorkerState::load(&state) == WorkerState::Healthy {
        WorkerState::Failed.store(&state);
    }
}

fn reject_pending(receiver: &mpsc::Receiver<Message>, error: TaskError, counters: &Counters) {
    loop {
        match receiver.try_recv() {
            Ok(Message::Run(job)) => {
                counters.rejected.fetch_add(1, Ordering::Relaxed);
                job.reject(error);
            }
            Ok(Message::Shutdown) => {}
            Err(mpsc::TryRecvError::Empty | mpsc::TryRecvError::Disconnected) => return,
        }
    }
}

fn metrics(workers: &[WorkerSlot], counters: &Counters) -> WorkerPoolMetrics {
    let mut snapshot = WorkerPoolMetrics {
        configured_workers: workers.len(),
        accepted_tasks: counters.accepted.load(Ordering::Relaxed),
        completed_tasks: counters.completed.load(Ordering::Relaxed),
        rejected_tasks: counters.rejected.load(Ordering::Relaxed),
        ..WorkerPoolMetrics::default()
    };
    for worker in workers {
        match WorkerState::load(&worker.state) {
            WorkerState::Starting => {}
            WorkerState::Healthy => snapshot.healthy_workers += 1,
            WorkerState::Poisoned => snapshot.poisoned_workers += 1,
            WorkerState::Failed => snapshot.failed_workers += 1,
            WorkerState::Stopped => snapshot.stopped_workers += 1,
        }
    }
    snapshot
}

#[cfg(test)]
mod tests {
    use std::collections::HashSet;
    use std::sync::mpsc;

    use super::*;

    fn fake_database() -> Arc<LocalDb> {
        crate::fake_abi::reset();
        Arc::new(LocalDb::open().expect("open fake fixed-worker database"))
    }

    fn options(worker_count: usize) -> FixedWorkerPoolOptions {
        FixedWorkerPoolOptions {
            worker_count,
            queue_capacity_per_worker: 8,
        }
    }

    fn assert_send_sync<T: Send + Sync>() {}

    fn assert_send<T: Send>() {}

    #[test]
    fn retry_budget_is_explicit_and_only_conflicts_are_retried() {
        let database = fake_database();
        let mut attempts = 0;
        let success = retry_transaction(&database, RetryPolicy::new(2), |_| {
            attempts += 1;
            if attempts <= 2 {
                Err(Error::Conflict)
            } else {
                Ok("committed")
            }
        })
        .unwrap();
        assert_eq!(success.value(), &"committed");
        assert_eq!(success.conflicts(), 2);
        assert_eq!(success.attempts(), 3);

        let mut exhausted_attempts = 0;
        let exhausted = retry_transaction(&database, RetryPolicy::new(1), |_| {
            exhausted_attempts += 1;
            Err::<(), _>(Error::Conflict)
        });
        assert_eq!(
            exhausted,
            Err(RetryError::ConflictBudgetExhausted { conflicts: 2 })
        );
        assert_eq!(exhausted_attempts, 2);

        let mut terminal_attempts = 0;
        let terminal = retry_transaction(&database, RetryPolicy::new(99), |_| {
            terminal_attempts += 1;
            Err::<(), _>(Error::ValueTooLarge)
        });
        assert_eq!(
            terminal,
            Err(RetryError::Operation {
                source: Error::ValueTooLarge,
                conflicts: 0,
            })
        );
        assert_eq!(terminal_attempts, 1);
        drop(database);
        crate::fake_abi::assert_drained();
    }

    #[test]
    fn fixed_workers_are_stable_and_retrying_jobs_stay_on_one_thread() {
        let database = fake_database();
        let pool = FixedWorkerPool::start(Arc::clone(&database), options(2)).unwrap();

        let tasks: Vec<_> = (0..6)
            .map(|_| pool.submit(|_| thread::current().id()))
            .collect();
        let thread_ids: HashSet<_> = tasks.into_iter().map(|task| task.wait().unwrap()).collect();
        assert_eq!(thread_ids.len(), 2);

        let retry = pool.submit_retrying(RetryPolicy::new(3), {
            let mut attempts = Vec::new();
            move |_| {
                attempts.push(thread::current().id());
                if attempts.len() < 3 {
                    Err(Error::Conflict)
                } else {
                    assert!(attempts.iter().all(|id| *id == attempts[0]));
                    Ok(attempts[0])
                }
            }
        });
        let retry = retry.wait().unwrap().unwrap();
        assert_eq!(retry.conflicts(), 2);
        assert!(thread_ids.contains(retry.value()));

        let before_shutdown = pool.metrics();
        assert_eq!(before_shutdown.configured_workers, 2);
        assert_eq!(before_shutdown.healthy_workers, 2);
        assert_eq!(before_shutdown.poisoned_workers, 0);
        assert_eq!(before_shutdown.accepted_tasks, 7);
        assert_eq!(before_shutdown.completed_tasks, 7);
        let after_shutdown = pool.shutdown().unwrap();
        assert_eq!(after_shutdown.stopped_workers, 2);
        drop(database);
        crate::fake_abi::assert_drained();
    }

    #[test]
    fn poisoned_worker_fails_current_and_pending_jobs_then_pool_routes_around_it() {
        let database = fake_database();
        let pool = FixedWorkerPool::start(Arc::clone(&database), options(2)).unwrap();
        let (started_sender, started_receiver) = mpsc::sync_channel(1);
        let (release_sender, release_receiver) = mpsc::sync_channel(1);

        let poison = pool.submit(move |database| {
            started_sender.send(()).unwrap();
            release_receiver.recv().unwrap();
            crate::fake_abi::push(crate::fake_abi::Step::Abort(
                mako_local_sys::MAKO_LOCAL_WORKER_POISONED,
            ));
            crate::fake_abi::push(crate::fake_abi::Step::Destroy(
                mako_local_sys::MAKO_LOCAL_WORKER_POISONED,
            ));
            drop(database.transaction().unwrap());
            "must be replaced by worker poison"
        });
        started_receiver.recv().unwrap();

        // Round robin sends this to worker 1 and the following task behind the
        // still-running poison job on worker 0.
        let healthy = pool.submit(|_| thread::current().id());
        let pending = pool.submit(|_| "must never run");
        release_sender.send(()).unwrap();

        assert_eq!(poison.wait(), Err(TaskError::WorkerPoisoned { worker: 0 }));
        assert!(healthy.wait().is_ok());
        assert_eq!(pending.wait(), Err(TaskError::WorkerPoisoned { worker: 0 }));

        let routed = pool.submit(|_| 42usize).wait();
        assert_eq!(routed, Ok(42));
        let snapshot = pool.metrics();
        assert_eq!(snapshot.healthy_workers, 1);
        assert_eq!(snapshot.poisoned_workers, 1);
        assert_eq!(snapshot.failed_workers, 0);
        assert_eq!(snapshot.accepted_tasks, 4);
        assert_eq!(snapshot.completed_tasks, 3);
        assert_eq!(snapshot.rejected_tasks, 1);

        let shutdown = pool.shutdown().unwrap();
        assert_eq!(shutdown.poisoned_workers, 1);
        assert_eq!(shutdown.stopped_workers, 1);
        drop(database);
        // The fake deliberately retains uncertain transaction state, matching
        // the native quarantine contract, so assert_drained does not apply.
    }

    #[test]
    fn application_panic_is_reported_after_cleanup_and_does_not_retire_worker() {
        let database = fake_database();
        let pool = FixedWorkerPool::start(Arc::clone(&database), options(1)).unwrap();
        let panic = pool.submit(|_| -> () { panic!("contained application panic") });
        assert_eq!(
            panic.wait(),
            Err(TaskError::ApplicationPanicked { worker: 0 })
        );
        assert_eq!(pool.submit(|_| 7).wait(), Ok(7));
        assert_eq!(pool.metrics().healthy_workers, 1);
        pool.shutdown().unwrap();
        drop(database);
        crate::fake_abi::assert_drained();
    }

    #[test]
    fn bounded_queue_rejects_without_blocking_or_cancelling_accepted_jobs() {
        let database = fake_database();
        let pool = FixedWorkerPool::start(
            Arc::clone(&database),
            FixedWorkerPoolOptions {
                worker_count: 1,
                queue_capacity_per_worker: 1,
            },
        )
        .unwrap();
        let (started_sender, started_receiver) = mpsc::sync_channel(1);
        let (release_sender, release_receiver) = mpsc::sync_channel(1);
        let running = pool.submit(move |_| {
            started_sender.send(()).unwrap();
            release_receiver.recv().unwrap();
            1usize
        });
        started_receiver.recv().unwrap();
        let queued = pool.submit(|_| 2usize);
        let rejected = pool.submit(|_| 3usize);
        assert_eq!(rejected.wait(), Err(TaskError::QueueFull));

        release_sender.send(()).unwrap();
        assert_eq!(running.wait(), Ok(1));
        assert_eq!(queued.wait(), Ok(2));
        let metrics = pool.metrics();
        assert_eq!(metrics.accepted_tasks, 2);
        assert_eq!(metrics.completed_tasks, 2);
        assert_eq!(metrics.rejected_tasks, 1);
        pool.shutdown().unwrap();
        drop(database);
        crate::fake_abi::assert_drained();
    }

    #[test]
    fn invalid_pool_shapes_fail_before_spawning() {
        assert_send_sync::<FixedWorkerPool>();
        assert_send::<Task<usize>>();
        let database = fake_database();
        assert!(matches!(
            FixedWorkerPool::start(
                Arc::clone(&database),
                FixedWorkerPoolOptions {
                    worker_count: 0,
                    queue_capacity_per_worker: 1,
                },
            ),
            Err(PoolStartError::InvalidConfig(PoolConfigError::ZeroWorkers))
        ));
        assert!(matches!(
            FixedWorkerPool::start(
                Arc::clone(&database),
                FixedWorkerPoolOptions {
                    worker_count: 1,
                    queue_capacity_per_worker: 0,
                },
            ),
            Err(PoolStartError::InvalidConfig(
                PoolConfigError::ZeroQueueCapacity
            ))
        ));
        assert!(matches!(
            FixedWorkerPool::start(
                Arc::clone(&database),
                FixedWorkerPoolOptions {
                    worker_count: crate::MAX_WORKERS,
                    queue_capacity_per_worker: 1,
                },
            ),
            Err(PoolStartError::InvalidConfig(
                PoolConfigError::TooManyWorkers {
                    requested,
                    maximum,
                }
            )) if requested == crate::MAX_WORKERS && maximum == crate::MAX_WORKERS - 1
        ));
        drop(database);
        crate::fake_abi::assert_drained();
    }
}
