//! One background consumer for [`crate::writeback::Writeback`].

use std::fmt;
use std::io;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, mpsc};
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

use mrx_core::Blobs;

use crate::writeback::{ApplyError, ProcessOutcome, Writeback};

const IDLE_POLL: Duration = Duration::from_millis(10);

/// Failure while stopping or cleanly draining the background runtime.
#[derive(Debug, Clone)]
pub enum RuntimeError {
    /// The background worker panicked outside its guarded consumer attempt.
    /// Panics from `process_front` or the backend are caught and retried.
    BackgroundPanicked,
    /// The clean-shutdown queue drain failed.
    Apply(ApplyError),
}

impl fmt::Display for RuntimeError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::BackgroundPanicked => write!(f, "write-back worker panicked"),
            Self::Apply(error) => write!(f, "clean write-back shutdown failed: {error}"),
        }
    }
}

impl std::error::Error for RuntimeError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::BackgroundPanicked => None,
            Self::Apply(error) => Some(error),
        }
    }
}

/// A single background consumer attached to a shared write-back queue.
///
/// [`Runtime::shutdown`] stops the worker and then synchronously drains the
/// highest acknowledged snapshot. [`Runtime::abort`] stops without a drain,
/// preserving the existing cache contract that an unapplied in-memory tail may
/// be lost on process failure. A panic from a backend attempt leaves the exact
/// Ready record queued and is retried after the configured delay. Dropping the
/// runtime is equivalent to aborting.
pub struct Runtime<B: Blobs + 'static> {
    writeback: Arc<Writeback<B>>,
    stop: Arc<AtomicBool>,
    thread: Option<JoinHandle<()>>,
}

impl<B: Blobs + 'static> Runtime<B> {
    /// Start one named background consumer.
    #[cfg(test)]
    pub fn start(writeback: Arc<Writeback<B>>) -> std::io::Result<Self> {
        Self::start_on_cpu(writeback, None)
    }

    /// Start one named background consumer, optionally pinned to one logical
    /// CPU.
    ///
    /// Pinning is currently supported on Linux. Startup waits for the new
    /// thread to install its affinity, so an invalid CPU, a cgroup restriction,
    /// or an unsupported platform is returned to the caller instead of being
    /// silently ignored.
    pub fn start_on_cpu(writeback: Arc<Writeback<B>>, cpu: Option<usize>) -> std::io::Result<Self> {
        let stop = Arc::new(AtomicBool::new(false));
        let worker_writeback = Arc::clone(&writeback);
        let worker_stop = Arc::clone(&stop);
        let (started_tx, started_rx) = mpsc::sync_channel(0);
        let thread = std::thread::Builder::new()
            .name("mako-writeback".to_owned())
            .spawn(move || {
                let affinity = pin_current_thread(cpu);
                let should_run = affinity.is_ok();
                if started_tx.send(affinity).is_err() || !should_run {
                    return;
                }
                run(worker_writeback, worker_stop);
            })?;

        match started_rx.recv() {
            Ok(Ok(())) => {}
            Ok(Err(error)) => {
                let _ = thread.join();
                return Err(error);
            }
            Err(_) => {
                let _ = thread.join();
                return Err(io::Error::other(
                    "write-back worker exited before reporting startup",
                ));
            }
        }

        Ok(Self {
            writeback,
            stop,
            thread: Some(thread),
        })
    }

    /// Stop the worker and apply the highest acknowledged snapshot.
    ///
    /// The drain is attempted even if the worker panicked, since both queue and
    /// consumer mutexes recover poison and a Ready record remains retryable. A
    /// pinned unknown outcome is rejected even when it lies after the applied
    /// snapshot, so clean close cannot discard possibly visible native state.
    /// If both the worker and synchronous drain fail, the application error is
    /// returned instead of the less specific worker panic.
    pub fn shutdown(&mut self) -> Result<u64, RuntimeError> {
        let worker_result = self.stop_worker();
        let apply_result = self
            .writeback
            .wait_applied()
            .and_then(|target| {
                self.writeback.ensure_no_unknown()?;
                Ok(target)
            })
            .map_err(RuntimeError::Apply);

        match (worker_result, apply_result) {
            (_, Err(error)) => Err(error),
            (Err(error), Ok(_)) => Err(error),
            (Ok(()), Ok(target)) => Ok(target),
        }
    }

    /// Stop the worker without draining the acknowledged in-memory tail.
    pub fn abort(&mut self) -> Result<(), RuntimeError> {
        self.stop_worker()
    }

    fn stop_worker(&mut self) -> Result<(), RuntimeError> {
        let Some(thread) = self.thread.take() else {
            return Ok(());
        };

        self.stop.store(true, Ordering::Release);
        // `unpark` records a token when it races just before the worker parks,
        // so the Release stop store cannot be stranded behind a long backend
        // retry interval.
        thread.thread().unpark();
        self.writeback.wake_waiters();
        thread.join().map_err(|_| RuntimeError::BackgroundPanicked)
    }
}

#[cfg(target_os = "linux")]
fn pin_current_thread(cpu: Option<usize>) -> io::Result<()> {
    use nix::sched::{CpuSet, sched_setaffinity};
    use nix::unistd::Pid;

    let Some(cpu) = cpu else {
        return Ok(());
    };
    let mut set = CpuSet::new();
    set.set(cpu)
        .map_err(|error| io::Error::from_raw_os_error(error as i32))?;
    sched_setaffinity(Pid::from_raw(0), &set)
        .map_err(|error| io::Error::from_raw_os_error(error as i32))
}

#[cfg(not(target_os = "linux"))]
fn pin_current_thread(cpu: Option<usize>) -> io::Result<()> {
    match cpu {
        None => Ok(()),
        Some(_) => Err(io::Error::new(
            io::ErrorKind::Unsupported,
            "write-back CPU affinity is supported only on Linux",
        )),
    }
}

impl<B: Blobs + 'static> Drop for Runtime<B> {
    fn drop(&mut self) {
        let _ = self.abort();
    }
}

fn run<B: Blobs + 'static>(writeback: Arc<Writeback<B>>, stop: Arc<AtomicBool>) {
    while !stop.load(Ordering::Acquire) {
        let outcome = catch_unwind(AssertUnwindSafe(|| writeback.process_front()));
        match outcome {
            Ok(ProcessOutcome::Advanced) => {}
            Ok(ProcessOutcome::BackendFailed { .. } | ProcessOutcome::RecordFailed { .. })
            | Err(_) => {
                wait_interruptibly(&stop, writeback.retry_delay());
            }
            Ok(ProcessOutcome::Idle | ProcessOutcome::Blocked | ProcessOutcome::Pinned(_)) => {
                wait_interruptibly(&stop, IDLE_POLL);
            }
        }
    }
}

fn wait_interruptibly(stop: &AtomicBool, duration: Duration) {
    let started = Instant::now();
    loop {
        if stop.load(Ordering::Acquire) {
            return;
        }
        let elapsed = started.elapsed();
        if elapsed >= duration {
            return;
        }
        // The dedicated consumer polls independently instead of enrolling in
        // the foreground publication condvar handshake. Bound each park so a
        // permanent record failure still checks stop periodically; `unpark`
        // in `stop_worker` makes ordinary shutdown immediate and cannot be
        // lost if it races just before this call.
        thread::park_timeout((duration - elapsed).min(IDLE_POLL));
    }
}

#[cfg(test)]
mod tests {
    use std::sync::atomic::AtomicUsize;
    use std::sync::{Arc, mpsc};

    use mako_local::MakoTimestamp;
    use mrx_core::fakes::MemBlobs;
    use mrx_core::{BlobError, BlobOp};

    use super::*;
    use crate::record::Mutation;
    use crate::writeback::WritebackConfig;

    fn put(key: &[u8], value: &[u8]) -> Mutation {
        Mutation::Put {
            table_id: 1,
            key: key.to_vec(),
            value: value.to_vec(),
        }
    }

    fn config() -> WritebackConfig {
        WritebackConfig {
            capacity: 4,
            max_apply_retries: 1,
            retry_delay: Duration::from_millis(10),
            ..WritebackConfig::default()
        }
    }

    fn timestamp(raw: u32) -> MakoTimestamp {
        MakoTimestamp::new(raw).expect("test timestamps are nonzero")
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn affinity_helper_pins_only_the_calling_thread() {
        use nix::sched::{CpuSet, sched_getaffinity};
        use nix::unistd::Pid;

        let allowed = sched_getaffinity(Pid::from_raw(0)).expect("read test affinity");
        let cpu = (0..CpuSet::count())
            .find(|cpu| allowed.is_set(*cpu) == Ok(true))
            .expect("test process has at least one allowed CPU");
        let pinned = std::thread::spawn(move || {
            pin_current_thread(Some(cpu)).expect("pin test thread");
            sched_getaffinity(Pid::from_raw(0)).expect("read pinned affinity")
        })
        .join()
        .unwrap();

        assert!(pinned.is_set(cpu).unwrap());
        assert_eq!(
            (0..CpuSet::count())
                .filter(|candidate| pinned.is_set(*candidate) == Ok(true))
                .count(),
            1
        );
        assert_eq!(
            sched_getaffinity(Pid::from_raw(0)).expect("reread parent affinity"),
            allowed,
            "pinning the worker changed its parent thread"
        );
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn invalid_affinity_fails_runtime_start_synchronously() {
        use nix::sched::CpuSet;

        let backend = Arc::new(MemBlobs::new());
        let writeback = Arc::new(Writeback::new(backend, 0, config()).expect("valid writeback"));
        let result = Runtime::start_on_cpu(writeback, Some(CpuSet::count()));
        assert!(result.is_err());
        assert_eq!(result.err().unwrap().kind(), io::ErrorKind::InvalidInput);
    }

    #[test]
    fn clean_shutdown_drains_acknowledged_records() {
        let backend = Arc::new(MemBlobs::new());
        let writeback =
            Arc::new(Writeback::new(Arc::clone(&backend), 0, config()).expect("valid writeback"));
        writeback
            .reserve(vec![put(b"a", b"one"), put(b"b", b"two")])
            .unwrap()
            .bind(timestamp(1))
            .unwrap()
            .publish()
            .unwrap();

        let mut runtime = Runtime::start(Arc::clone(&writeback)).unwrap();
        assert!(runtime.thread.is_some());
        assert_eq!(runtime.shutdown().unwrap(), 1);
        assert!(runtime.thread.is_none());
        assert_eq!(backend.batch_count(), 1);
        assert_eq!(backend.op_count(), 3);
        assert_eq!(writeback.applied_sequence(), 1);
        assert_eq!(writeback.queue_len(), 0);
    }

    #[test]
    fn background_worker_polls_without_registering_an_activity_waiter() {
        let backend = Arc::new(MemBlobs::new());
        let writeback =
            Arc::new(Writeback::new(Arc::clone(&backend), 0, config()).expect("valid writeback"));

        let mut runtime = Runtime::start(Arc::clone(&writeback)).unwrap();
        let idle_deadline = Instant::now() + Duration::from_millis(30);
        while Instant::now() < idle_deadline {
            assert_eq!(
                writeback.activity_waiter_count(),
                0,
                "the dedicated worker enrolled in the publication condvar"
            );
            std::thread::sleep(Duration::from_millis(1));
        }

        writeback
            .reserve(vec![put(b"background", b"applied")])
            .unwrap()
            .bind(timestamp(9))
            .unwrap()
            .publish()
            .unwrap();

        let deadline = Instant::now() + Duration::from_secs(1);
        while writeback.applied_sequence() < 1 {
            assert!(
                Instant::now() < deadline,
                "background worker did not advance the applied watermark"
            );
            assert_eq!(writeback.activity_waiter_count(), 0);
            std::thread::sleep(Duration::from_millis(1));
        }
        assert_eq!(writeback.activity_waiter_count(), 0);
        runtime.abort().unwrap();

        assert_eq!(writeback.applied_sequence(), 1);
        assert_eq!(
            writeback.applied_watermark().mako_timestamp(),
            Some(timestamp(9))
        );
        assert_eq!(backend.batch_count(), 1);
        assert_eq!(writeback.queue_len(), 0);
    }

    #[derive(Debug, Default)]
    struct AlwaysFailBlobs {
        attempts: AtomicUsize,
    }

    impl Blobs for AlwaysFailBlobs {
        fn get(&self, _key: &[u8]) -> Result<Option<Vec<u8>>, BlobError> {
            Ok(None)
        }

        fn write_batch(&self, _operations: &[BlobOp<'_>]) -> Result<(), BlobError> {
            self.attempts.fetch_add(1, Ordering::SeqCst);
            Err(BlobError("injected persistent write failure".to_owned()))
        }

        fn for_each_key(&self, _f: &mut dyn FnMut(&[u8])) -> Result<(), BlobError> {
            Ok(())
        }
    }

    #[test]
    fn abort_wakes_a_worker_parked_for_a_long_retry() {
        let backend = Arc::new(AlwaysFailBlobs::default());
        let mut test_config = config();
        test_config.retry_delay = Duration::from_secs(30);
        let writeback = Arc::new(
            Writeback::new(Arc::clone(&backend), 0, test_config).expect("valid writeback"),
        );
        writeback
            .reserve(vec![put(b"retry", b"pending")])
            .unwrap()
            .bind(timestamp(10))
            .unwrap()
            .publish()
            .unwrap();

        let mut runtime = Runtime::start(Arc::clone(&writeback)).unwrap();
        let attempt_deadline = Instant::now() + Duration::from_secs(1);
        while backend.attempts.load(Ordering::SeqCst) == 0 {
            assert!(
                Instant::now() < attempt_deadline,
                "background worker did not attempt the failing write"
            );
            std::thread::yield_now();
        }
        std::thread::sleep(Duration::from_millis(20));

        let abort_started = Instant::now();
        runtime.abort().unwrap();
        assert!(
            abort_started.elapsed() < Duration::from_secs(1),
            "abort waited for the 30-second backend retry delay"
        );
        assert_eq!(writeback.applied_sequence(), 0);
        assert_eq!(writeback.queue_len(), 1);
    }

    #[test]
    fn abort_stops_without_draining_a_failing_tail() {
        let backend = Arc::new(MemBlobs::new());
        backend.fail_next_writes(usize::MAX);
        let writeback =
            Arc::new(Writeback::new(Arc::clone(&backend), 0, config()).expect("valid writeback"));
        writeback
            .reserve(vec![put(b"a", b"one")])
            .unwrap()
            .bind(timestamp(2))
            .unwrap()
            .publish()
            .unwrap();

        let mut runtime = Runtime::start(Arc::clone(&writeback)).unwrap();
        std::thread::sleep(Duration::from_millis(20));
        runtime.abort().unwrap();

        assert!(runtime.thread.is_none());
        assert_eq!(backend.batch_count(), 0);
        assert_eq!(writeback.applied_sequence(), 0);
        assert_eq!(writeback.queue_len(), 1);
    }

    #[test]
    fn drop_uses_abort_semantics() {
        let backend = Arc::new(MemBlobs::new());
        backend.fail_next_writes(usize::MAX);
        let writeback =
            Arc::new(Writeback::new(Arc::clone(&backend), 0, config()).expect("valid writeback"));
        writeback
            .reserve(vec![put(b"a", b"one")])
            .unwrap()
            .bind(timestamp(3))
            .unwrap()
            .publish()
            .unwrap();

        drop(Runtime::start(Arc::clone(&writeback)).unwrap());
        assert_eq!(backend.batch_count(), 0);
        assert_eq!(writeback.applied_sequence(), 0);
        assert_eq!(writeback.queue_len(), 1);
    }

    #[derive(Debug, Default)]
    struct PanicOnceBlobs {
        inner: MemBlobs,
        attempts: AtomicUsize,
        panic_next: AtomicBool,
    }

    impl PanicOnceBlobs {
        fn new() -> Self {
            Self {
                inner: MemBlobs::new(),
                attempts: AtomicUsize::new(0),
                panic_next: AtomicBool::new(true),
            }
        }
    }

    impl Blobs for PanicOnceBlobs {
        fn get(&self, key: &[u8]) -> Result<Option<Vec<u8>>, BlobError> {
            self.inner.get(key)
        }

        fn write_batch(&self, operations: &[BlobOp<'_>]) -> Result<(), BlobError> {
            self.attempts.fetch_add(1, Ordering::SeqCst);
            if self.panic_next.swap(false, Ordering::SeqCst) {
                panic!("injected one-shot backend panic");
            }
            self.inner.write_batch(operations)
        }

        fn for_each_key(&self, f: &mut dyn FnMut(&[u8])) -> Result<(), BlobError> {
            self.inner.for_each_key(f)
        }
    }

    #[test]
    fn runtime_recovers_from_backend_panic_and_unblocks_capacity() {
        let backend = Arc::new(PanicOnceBlobs::new());
        let mut test_config = config();
        test_config.capacity = 1;
        let writeback = Arc::new(
            Writeback::new(Arc::clone(&backend), 0, test_config).expect("valid writeback"),
        );
        writeback
            .reserve(vec![put(b"a", b"one")])
            .unwrap()
            .bind(timestamp(4))
            .unwrap()
            .publish()
            .unwrap();

        let (started_tx, started_rx) = mpsc::channel();
        let (reserved_tx, reserved_rx) = mpsc::channel();
        let producer_writeback = Arc::clone(&writeback);
        let producer = std::thread::spawn(move || {
            started_tx.send(()).unwrap();
            let mut reservation = producer_writeback.reserve(vec![put(b"b", b"two")]).unwrap();
            reserved_tx.send(()).unwrap();
            reservation.bind(timestamp(5)).unwrap().publish().unwrap();
        });
        started_rx.recv_timeout(Duration::from_secs(1)).unwrap();
        let producer_was_blocked = reserved_rx.recv_timeout(Duration::from_millis(30)).is_err();

        let mut runtime = Runtime::start(Arc::clone(&writeback)).unwrap();
        if producer_was_blocked {
            reserved_rx.recv_timeout(Duration::from_secs(2)).unwrap();
        }
        producer.join().unwrap();

        assert!(
            producer_was_blocked,
            "full queue did not apply backpressure"
        );
        assert_eq!(runtime.shutdown().unwrap(), 2);
        assert_eq!(backend.attempts.load(Ordering::SeqCst), 3);
        assert_eq!(backend.inner.batch_count(), 2);
        assert_eq!(writeback.applied_sequence(), 2);
        assert_eq!(writeback.queue_len(), 0);
    }

    #[test]
    fn clean_shutdown_rejects_unknown_after_applying_safe_prefix() {
        let backend = Arc::new(MemBlobs::new());
        let writeback =
            Arc::new(Writeback::new(Arc::clone(&backend), 0, config()).expect("valid writeback"));
        let mut acknowledged = writeback
            .reserve(vec![put(b"a", b"one")])
            .unwrap()
            .bind(timestamp(6))
            .unwrap();
        let mut unknown = writeback
            .reserve(vec![put(b"b", b"two")])
            .unwrap()
            .bind(timestamp(7))
            .unwrap();
        acknowledged.publish().unwrap();
        unknown.pin_unknown().unwrap();

        let mut runtime = Runtime::start(Arc::clone(&writeback)).unwrap();
        assert!(matches!(
            runtime.shutdown(),
            Err(RuntimeError::Apply(ApplyError::UnknownOutcome { sequence }))
                if sequence.get() == 2
        ));
        assert_eq!(writeback.applied_sequence(), 1);
        assert_eq!(backend.batch_count(), 1);
        assert_eq!(writeback.queue_len(), 1);
    }

    #[test]
    fn shutdown_prioritizes_apply_error_over_worker_panic() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Arc::new(Writeback::new(backend, 0, config()).expect("valid writeback"));
        writeback
            .reserve(vec![put(b"a", b"one")])
            .unwrap()
            .bind(timestamp(8))
            .unwrap()
            .pin_unknown()
            .unwrap();

        let mut runtime = Runtime {
            writeback,
            stop: Arc::new(AtomicBool::new(false)),
            thread: Some(std::thread::spawn(|| panic!("injected worker panic"))),
        };

        assert!(matches!(
            runtime.shutdown(),
            Err(RuntimeError::Apply(ApplyError::UnknownOutcome { sequence }))
                if sequence.get() == 1
        ));
    }
}
