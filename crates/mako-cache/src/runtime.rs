//! One background consumer for [`crate::writeback::Writeback`].

use std::fmt;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use mrx_core::Blobs;

use crate::writeback::{FlushError, ProcessOutcome, Writeback};

const IDLE_POLL: Duration = Duration::from_millis(10);

/// Failure while stopping or cleanly draining the background runtime.
#[derive(Debug, Clone)]
pub enum RuntimeError {
    /// The background worker panicked outside its guarded consumer attempt.
    /// Panics from `process_front` or the backend are caught and retried.
    BackgroundPanicked,
    /// The clean-shutdown flush failed.
    Flush(FlushError),
}

impl fmt::Display for RuntimeError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::BackgroundPanicked => write!(f, "write-back worker panicked"),
            Self::Flush(error) => write!(f, "clean write-back shutdown failed: {error}"),
        }
    }
}

impl std::error::Error for RuntimeError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::BackgroundPanicked => None,
            Self::Flush(error) => Some(error),
        }
    }
}

/// A single background flusher attached to a shared write-back queue.
///
/// [`Runtime::shutdown`] stops the worker and then synchronously drains the
/// highest acknowledged snapshot. [`Runtime::abort`] stops without a drain,
/// preserving the existing cache contract that an unflushed in-memory tail may
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
    pub fn start(writeback: Arc<Writeback<B>>) -> std::io::Result<Self> {
        let stop = Arc::new(AtomicBool::new(false));
        let worker_writeback = Arc::clone(&writeback);
        let worker_stop = Arc::clone(&stop);
        let thread = std::thread::Builder::new()
            .name("mako-writeback".to_owned())
            .spawn(move || run(worker_writeback, worker_stop))?;

        Ok(Self {
            writeback,
            stop,
            thread: Some(thread),
        })
    }

    /// Stop the worker and make the highest acknowledged snapshot durable.
    ///
    /// The flush is attempted even if the worker panicked, since both queue and
    /// consumer mutexes recover poison and a Ready record remains retryable. A
    /// pinned unknown outcome is rejected even when it lies after the flushed
    /// snapshot, so clean close cannot discard possibly visible native state.
    /// If both the worker and synchronous drain fail, the durability-bearing
    /// flush error is returned instead of the less specific worker panic.
    pub fn shutdown(&mut self) -> Result<u64, RuntimeError> {
        let worker_result = self.stop_worker();
        let flush_result = self
            .writeback
            .flush()
            .and_then(|target| {
                self.writeback.ensure_no_unknown()?;
                Ok(target)
            })
            .map_err(RuntimeError::Flush);

        match (worker_result, flush_result) {
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
        self.writeback.wake_waiters();
        thread.join().map_err(|_| RuntimeError::BackgroundPanicked)
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
            Ok(ProcessOutcome::BackendFailed { .. }) | Err(_) => {
                wait_interruptibly(&writeback, &stop, writeback.retry_delay());
            }
            Ok(ProcessOutcome::Idle | ProcessOutcome::Blocked | ProcessOutcome::Pinned(_)) => {
                wait_interruptibly(&writeback, &stop, IDLE_POLL);
            }
        }
    }
}

fn wait_interruptibly<B: Blobs>(writeback: &Writeback<B>, stop: &AtomicBool, duration: Duration) {
    let started = Instant::now();
    loop {
        if stop.load(Ordering::Acquire) {
            return;
        }
        let elapsed = started.elapsed();
        if elapsed >= duration {
            return;
        }
        // Bound every individual wait so a stop notification racing just
        // before `wait_timeout` cannot delay shutdown for an arbitrary retry
        // interval. This is sleeping, not polling in a tight loop.
        writeback.wait_for_activity((duration - elapsed).min(IDLE_POLL));
    }
}

#[cfg(test)]
mod tests {
    use std::sync::atomic::AtomicUsize;
    use std::sync::{mpsc, Arc};

    use mako_local::SiloTimestamp;
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
            max_flush_retries: 1,
            retry_delay: Duration::from_millis(10),
            ..WritebackConfig::default()
        }
    }

    fn timestamp(raw: u64) -> SiloTimestamp {
        SiloTimestamp::new(raw).expect("test timestamps are nonzero")
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
        assert_eq!(writeback.durable_sequence(), 1);
        assert_eq!(writeback.queue_len(), 0);
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
        assert_eq!(writeback.durable_sequence(), 0);
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
        assert_eq!(writeback.durable_sequence(), 0);
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
        assert_eq!(writeback.durable_sequence(), 2);
        assert_eq!(writeback.queue_len(), 0);
    }

    #[test]
    fn clean_shutdown_rejects_unknown_after_flushing_safe_prefix() {
        let backend = Arc::new(MemBlobs::new());
        let writeback =
            Arc::new(Writeback::new(Arc::clone(&backend), 0, config()).expect("valid writeback"));
        let acknowledged = writeback
            .reserve(vec![put(b"a", b"one")])
            .unwrap()
            .bind(timestamp(6))
            .unwrap();
        let unknown = writeback
            .reserve(vec![put(b"b", b"two")])
            .unwrap()
            .bind(timestamp(7))
            .unwrap();
        acknowledged.publish().unwrap();
        unknown.pin_unknown().unwrap();

        let mut runtime = Runtime::start(Arc::clone(&writeback)).unwrap();
        assert!(matches!(
            runtime.shutdown(),
            Err(RuntimeError::Flush(FlushError::UnknownOutcome { sequence }))
                if sequence.get() == 2
        ));
        assert_eq!(writeback.durable_sequence(), 1);
        assert_eq!(backend.batch_count(), 1);
        assert_eq!(writeback.queue_len(), 1);
    }

    #[test]
    fn shutdown_prioritizes_flush_error_over_worker_panic() {
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
            Err(RuntimeError::Flush(FlushError::UnknownOutcome { sequence }))
                if sequence.get() == 1
        ));
    }
}
