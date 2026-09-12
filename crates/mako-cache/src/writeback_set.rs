//! Per-worker SPSC writeback lanes for the concurrent cache profile.

use std::collections::HashMap;
use std::sync::atomic::{AtomicBool, AtomicU64, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex, OnceLock};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use mako_local::MakoTimestamp;
use mrx_core::{BlobError, Blobs};

use crate::checkpoint::LaneMetadata;
use crate::record::{worker_log_base, LOG_LOCAL_MASK};
use crate::runtime::RuntimeTarget;
use crate::writeback::{
    AppliedWatermark, ApplyCoordinator, ApplyError, ApplyTelemetrySnapshot, ConfigError, GcStep,
    LogGcStatus, ProcessOutcome, SingleProducerState, Writeback, WritebackConfig,
};

#[derive(Clone, Copy, Debug, Default)]
pub(crate) struct LaneRecovery {
    pub(crate) local_tail: u64,
    pub(crate) mako_timestamp: Option<MakoTimestamp>,
}

/// Backend state reconstructed before foreground work is admitted.
pub(crate) struct RecoveredWriteback {
    pub(crate) metadata: Vec<LaneMetadata>,
    pub(crate) legacy: LaneRecovery,
    pub(crate) lanes: Vec<LaneRecovery>,
    pub(crate) latest: HashMap<Vec<u8>, MakoTimestamp>,
    pub(crate) record_count: u64,
    pub(crate) maximum_timestamp: Option<MakoTimestamp>,
}

impl RecoveredWriteback {
    pub(crate) fn empty() -> Self {
        Self {
            metadata: vec![LaneMetadata::default(); mako_local::MAX_WORKERS + 1],
            legacy: LaneRecovery::default(),
            lanes: vec![LaneRecovery::default(); mako_local::MAX_WORKERS],
            latest: HashMap::new(),
            record_count: 0,
            maximum_timestamp: None,
        }
    }
}

pub(crate) struct WorkerLane<B: Blobs + 'static> {
    base: u64,
    recovered_local_tail: u64,
    writeback: Arc<Writeback<Arc<B>>>,
    producer: SingleProducerState,
}

impl<B: Blobs + 'static> WorkerLane<B> {
    pub(crate) fn writeback(&self) -> &Writeback<Arc<B>> {
        &self.writeback
    }

    pub(crate) fn producer(&self) -> &SingleProducerState {
        &self.producer
    }

    fn local_position(&self, physical: u64) -> u64 {
        physical
            .checked_sub(self.base)
            .expect("a lane frontier cannot precede its physical base")
    }

    fn acknowledged_local(&self) -> u64 {
        self.local_position(self.writeback.highest_caller_acknowledged())
    }
}

/// All physical writeback streams owned by one cache instance.
pub(crate) struct WritebackSet<B: Blobs + 'static> {
    backend: Arc<B>,
    config: WritebackConfig,
    concurrent: bool,
    unhealthy: Arc<AtomicBool>,
    unhealthy_sequence: Arc<AtomicU64>,
    coordinator: Arc<ApplyCoordinator>,
    recovery: Box<[LaneRecovery]>,
    legacy_recovery: LaneRecovery,
    recovered_record_count: u64,
    recovered_maximum_timestamp: Option<MakoTimestamp>,
    lanes: Box<[OnceLock<WorkerLane<B>>]>,
    legacy_lane: OnceLock<WorkerLane<B>>,
    initialize: Mutex<()>,
    /// Serializes the round-robin backend scheduler with snapshot drains.
    /// Foreground publication never touches this lock.
    scheduler: Mutex<()>,
    poll_cursor: AtomicUsize,
    gc: Mutex<GcSchedule>,
}

struct GcSchedule {
    retention_us: u64,
    interval: Duration,
    // None means the next deadline is beyond the platform's Instant range.
    next_sweep: Option<Instant>,
    cutoff: Option<u64>,
    apply_turn: bool,
}

impl<B: Blobs + 'static> WritebackSet<B> {
    #[cfg(test)]
    pub(crate) fn new(
        backend: B,
        recovered: RecoveredWriteback,
        config: WritebackConfig,
        concurrent: bool,
    ) -> Result<Self, ConfigError> {
        Self::new_with_gc(
            backend,
            recovered,
            config,
            concurrent,
            Duration::from_secs(300),
            Duration::from_secs(10),
        )
    }

    pub(crate) fn new_with_gc(
        backend: B,
        recovered: RecoveredWriteback,
        config: WritebackConfig,
        concurrent: bool,
        log_retention: Duration,
        gc_interval: Duration,
    ) -> Result<Self, ConfigError> {
        Writeback::<Arc<B>>::validate_config(config, 0, LOG_LOCAL_MASK)?;
        let set = Self {
            backend: Arc::new(backend),
            config,
            concurrent,
            unhealthy: Arc::new(AtomicBool::new(false)),
            unhealthy_sequence: Arc::new(AtomicU64::new(0)),
            coordinator: Arc::new(ApplyCoordinator::recovered(
                recovered.latest,
                recovered.metadata,
            )),
            recovery: recovered.lanes.into_boxed_slice(),
            legacy_recovery: recovered.legacy,
            recovered_record_count: recovered.record_count,
            recovered_maximum_timestamp: recovered.maximum_timestamp,
            lanes: (0..mako_local::MAX_WORKERS)
                .map(|_| OnceLock::new())
                .collect::<Vec<_>>()
                .into_boxed_slice(),
            legacy_lane: OnceLock::new(),
            initialize: Mutex::new(()),
            scheduler: Mutex::new(()),
            poll_cursor: AtomicUsize::new(0),
            gc: Mutex::new(GcSchedule {
                retention_us: u64::try_from(log_retention.as_micros())
                    .expect("validated retention"),
                interval: gc_interval,
                next_sweep: Instant::now().checked_add(gc_interval),
                cutoff: None,
                apply_turn: false,
            }),
        };
        if !concurrent {
            let lane = set.build_lane(None)?;
            if set.legacy_lane.set(lane).is_err() {
                unreachable!("a fresh legacy lane is empty");
            }
        }
        Ok(set)
    }

    fn build_lane(&self, worker_slot: Option<usize>) -> Result<WorkerLane<B>, ConfigError> {
        let (base, recovered) = match worker_slot {
            Some(slot) => {
                let base = worker_log_base(slot).ok_or(ConfigError::SequenceExhausted)?;
                let recovered = *self
                    .recovery
                    .get(slot)
                    .ok_or(ConfigError::SequenceExhausted)?;
                (base, recovered)
            }
            None => (0, self.legacy_recovery),
        };
        if recovered.local_tail > LOG_LOCAL_MASK {
            return Err(ConfigError::SequenceExhausted);
        }
        let physical_tail = base
            .checked_add(recovered.local_tail)
            .ok_or(ConfigError::SequenceExhausted)?;
        let maximum_sequence = base
            .checked_add(LOG_LOCAL_MASK)
            .ok_or(ConfigError::SequenceExhausted)?;
        let writeback = Arc::new(Writeback::new_with_shared_state(
            Arc::clone(&self.backend),
            AppliedWatermark::recovered(physical_tail, recovered.mako_timestamp),
            maximum_sequence,
            self.config,
            true,
            self.concurrent,
            Arc::clone(&self.unhealthy),
            Arc::clone(&self.unhealthy_sequence),
            Arc::clone(&self.coordinator),
        )?);
        let producer = writeback.single_producer_state();
        Ok(WorkerLane {
            base,
            recovered_local_tail: recovered.local_tail,
            writeback,
            producer,
        })
    }

    pub(crate) fn lane(&self, worker_slot: usize) -> Result<&WorkerLane<B>, ConfigError> {
        if !self.concurrent {
            return Err(ConfigError::SequenceExhausted);
        }
        let cell = self
            .lanes
            .get(worker_slot)
            .ok_or(ConfigError::SequenceExhausted)?;
        if let Some(lane) = cell.get() {
            return Ok(lane);
        }
        let _guard = self
            .initialize
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        if cell.get().is_none() {
            let lane = self.build_lane(Some(worker_slot))?;
            if cell.set(lane).is_err() {
                unreachable!("lane initialization is serialized");
            }
        }
        Ok(cell.get().expect("initialized worker lane"))
    }

    pub(crate) fn single_lane(&self) -> &WorkerLane<B> {
        self.legacy_lane
            .get()
            .expect("single-producer construction initializes its lane")
    }

    #[cfg(test)]
    pub(crate) fn backend(&self) -> &B {
        &self.backend
    }

    pub(crate) fn max_record_bytes(&self) -> usize {
        self.config.max_record_bytes
    }

    fn initialized_lanes(&self) -> impl Iterator<Item = &WorkerLane<B>> {
        self.legacy_lane
            .get()
            .into_iter()
            .chain(self.lanes.iter().filter_map(OnceLock::get))
    }

    pub(crate) fn ensure_no_unknown(&self) -> Result<(), ApplyError> {
        if !self.unhealthy.load(Ordering::Acquire) {
            return Ok(());
        }
        for lane in self.initialized_lanes() {
            if let Some(error) = lane.writeback.local_health_error() {
                return Err(error);
            }
        }
        let sequence = crate::CommitSeq::new(self.unhealthy_sequence.load(Ordering::Acquire))
            .unwrap_or_else(|| std::process::abort());
        Err(ApplyError::UnknownOutcome { sequence })
    }

    pub(crate) fn wait_applied(&self) -> Result<u64, ApplyError> {
        // Freeze the sole set-level consumer before taking lane snapshots.
        // An uncertain batch therefore either predates the snapshot and is
        // covered by its owner target, or cannot be created until this drain
        // releases the scheduler.
        let _scheduler = self
            .scheduler
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        self.ensure_no_unknown()?;
        let targets = self
            .initialized_lanes()
            .map(|lane| (lane, lane.writeback.highest_caller_acknowledged()))
            .collect::<Vec<_>>();

        // Drive all captured lane frontiers together. An uncertain backend
        // batch must be retried as the exact original slice before the shared
        // timestamp index can admit another lane; draining one lane to
        // completion here could otherwise wait forever for a later lane after
        // Runtime has already joined its background consumer.
        let mut backend_failures = vec![None::<(crate::CommitSeq, usize)>; targets.len()];
        let mut gc_failures = 0usize;
        loop {
            self.ensure_no_unknown()?;
            // A GC write may have taken effect despite returning an error.
            // Finish its exact retry before allowing any other backend write,
            // including when shutdown has no foreground work left to drain.
            let gc = self.coordinator.gc_snapshot();
            if gc.pending_retry {
                match self
                    .coordinator
                    .gc_step(&*self.backend, 0, self.config.max_record_bytes)
                {
                    Ok(_) => gc_failures = 0,
                    Err(source) => {
                        gc_failures = gc_failures.saturating_add(1);
                        if gc_failures > self.config.max_apply_retries {
                            return Err(ApplyError::Backend {
                                sequence: gc
                                    .pending_sequence
                                    .expect("pending GC has a first sequence"),
                                attempts: gc_failures,
                                source,
                            });
                        }
                        std::thread::sleep(self.config.retry_delay);
                        continue;
                    }
                }
            }
            let mut pending = false;
            let mut advanced = false;
            let mut failed = false;
            for (index, (lane, target)) in targets.iter().enumerate() {
                if lane.writeback.applied_sequence() >= *target {
                    continue;
                }
                pending = true;
                match lane.writeback.process_front_through(Some(*target)) {
                    ProcessOutcome::Advanced => {
                        backend_failures[index] = None;
                        advanced = true;
                    }
                    ProcessOutcome::BackendFailed { sequence, error } => {
                        failed = true;
                        let attempts = match backend_failures[index] {
                            Some((failed_sequence, attempts)) if failed_sequence == sequence => {
                                attempts.saturating_add(1)
                            }
                            Some(_) | None => 1,
                        };
                        backend_failures[index] = Some((sequence, attempts));
                        if attempts > self.config.max_apply_retries {
                            // Recheck the applied Acquire frontier before
                            // reporting the backend failure. This keeps the
                            // decision tied to observable lane progress.
                            let applied = lane.writeback.applied_sequence();
                            if applied >= *target || applied >= sequence.get() {
                                backend_failures[index] = None;
                                advanced = true;
                                continue;
                            }
                            return Err(ApplyError::Backend {
                                sequence,
                                attempts,
                                source: error,
                            });
                        }
                    }
                    ProcessOutcome::RecordFailed { sequence, error } => {
                        return Err(ApplyError::Record {
                            sequence,
                            source: error,
                        });
                    }
                    ProcessOutcome::Pinned(sequence) => {
                        return Err(ApplyError::UnknownOutcome { sequence });
                    }
                    ProcessOutcome::Blocked | ProcessOutcome::Idle => {}
                }
            }
            if !pending {
                break;
            }
            if failed {
                std::thread::sleep(self.config.retry_delay);
            } else if !advanced {
                // A foreground publisher may own the missing transition.
                // Yield instead of parking on one lane's condition variable,
                // because another captured lane may still require polling.
                std::thread::yield_now();
            }
        }
        self.ensure_no_unknown()?;
        Ok(self.recovered_record_count.saturating_add(
            targets
                .iter()
                .map(|(lane, target)| {
                    lane.local_position(*target)
                        .saturating_sub(lane.recovered_local_tail)
                })
                .sum::<u64>(),
        ))
    }

    pub(crate) fn applied_watermark(&self) -> AppliedWatermark {
        let mut count = self.recovered_record_count;
        let mut maximum = self.recovered_maximum_timestamp;
        for lane in self.initialized_lanes() {
            let lane_watermark = lane.writeback.applied_watermark();
            count = count.saturating_add(
                lane.local_position(lane_watermark.sequence())
                    .saturating_sub(lane.recovered_local_tail),
            );
            if let Some(timestamp) = lane_watermark.mako_timestamp() {
                maximum = Some(maximum.map_or(timestamp, |current| current.max(timestamp)));
            }
        }
        AppliedWatermark::recovered(count, maximum)
    }

    pub(crate) fn highest_acknowledged(&self) -> u64 {
        self.recovered_record_count.saturating_add(
            self.initialized_lanes()
                .map(|lane| {
                    lane.acknowledged_local()
                        .saturating_sub(lane.recovered_local_tail)
                })
                .sum::<u64>(),
        )
    }

    pub(crate) fn queue_len(&self) -> usize {
        self.initialized_lanes()
            .map(|lane| lane.writeback.queue_len())
            .sum()
    }

    pub(crate) fn apply_telemetry(&self) -> ApplyTelemetrySnapshot {
        self.coordinator.telemetry_snapshot()
    }

    pub(crate) fn gc_snapshot(&self) -> LogGcStatus {
        self.coordinator.gc_snapshot()
    }

    pub(crate) fn disk_usage_bytes(&self) -> Result<Option<u64>, BlobError> {
        self.backend.disk_usage_bytes()
    }

    /// Runs at most one bounded GC batch, alternating with foreground apply.
    /// The scheduler serializes writes, but neither status nor producers take it.
    fn scheduled_gc(&self) -> Option<ProcessOutcome> {
        if self.coordinator.gc_snapshot().pending_retry {
            return Some(
                match self
                    .coordinator
                    .gc_step(&*self.backend, 0, self.config.max_record_bytes)
                {
                    Ok(_) => ProcessOutcome::Advanced,
                    Err(_) => ProcessOutcome::Blocked,
                },
            );
        }
        let cutoff = {
            let mut schedule = self.gc.lock().unwrap_or_else(|p| p.into_inner());
            if schedule.apply_turn {
                schedule.apply_turn = false;
                return None;
            }
            if schedule.cutoff.is_none() {
                let now = Instant::now();
                if schedule.next_sweep.is_none_or(|deadline| now < deadline) {
                    return None;
                }
                let wall_us = SystemTime::now()
                    .duration_since(UNIX_EPOCH)
                    .ok()
                    .and_then(|time| u64::try_from(time.as_micros()).ok());
                let Some(wall_us) = wall_us else {
                    schedule.next_sweep = now.checked_add(schedule.interval);
                    self.coordinator.record_gc_clock_error(
                        "wall clock is outside the supported Unix microsecond range",
                    );
                    return Some(ProcessOutcome::Blocked);
                };
                schedule.cutoff = Some(wall_us.saturating_sub(schedule.retention_us));
                self.coordinator.clear_gc_clock_error();
            }
            schedule.apply_turn = true;
            schedule.cutoff.expect("a due sweep has a cutoff")
        };
        match self
            .coordinator
            .gc_step(&*self.backend, cutoff, self.config.max_record_bytes)
        {
            Ok(GcStep::Progress { .. }) => Some(ProcessOutcome::Advanced),
            Ok(GcStep::Complete) => {
                let mut schedule = self.gc.lock().unwrap_or_else(|p| p.into_inner());
                schedule.cutoff = None;
                schedule.next_sweep = Instant::now().checked_add(schedule.interval);
                schedule.apply_turn = false;
                None
            }
            Ok(GcStep::BlockedByApplyRetry) => None,
            Err(_) => Some(ProcessOutcome::Blocked),
        }
    }

    /// Deterministic collection for failure and clock tests. Production callers
    /// cannot inject time or access the backend through the cache.
    #[cfg(any(test, feature = "test-support"))]
    pub(crate) fn collect_expired_at(&self, unix_us: u64) -> Result<u64, BlobError> {
        let _scheduler = self.scheduler.lock().unwrap_or_else(|p| p.into_inner());
        self.ensure_no_unknown()
            .map_err(|error| BlobError(error.to_string()))?;
        let retention = self
            .gc
            .lock()
            .unwrap_or_else(|p| p.into_inner())
            .retention_us;
        let cutoff = unix_us.saturating_sub(retention);
        let mut count = 0u64;
        loop {
            match self
                .coordinator
                .gc_step(&*self.backend, cutoff, self.config.max_record_bytes)?
            {
                GcStep::Progress { records, .. } => count += records,
                GcStep::Complete => return Ok(count),
                GcStep::BlockedByApplyRetry => {
                    return Err(BlobError("GC is waiting for an exact apply retry".into()))
                }
            }
        }
    }

    pub(crate) fn record_runtime_loop_panic(&self, message: String) {
        self.coordinator.record_runtime_loop_panic(message);
    }

    pub(crate) fn reclaim_credits_for_shutdown(&self) {
        for lane in self.initialized_lanes() {
            lane.writeback
                .reclaim_packed_occupancy_credits_for_shutdown();
        }
    }

    fn process_one_round(&self) -> ProcessOutcome {
        let _scheduler = self
            .scheduler
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        if self.unhealthy.load(Ordering::Acquire) {
            let sequence = crate::CommitSeq::new(self.unhealthy_sequence.load(Ordering::Acquire))
                .unwrap_or_else(|| std::process::abort());
            return ProcessOutcome::Pinned(sequence);
        }

        if let Some(outcome) = self.scheduled_gc() {
            return outcome;
        }

        if !self.concurrent {
            return self.single_lane().writeback.process_front();
        }

        let lane_count = self.lanes.len();
        debug_assert_ne!(lane_count, 0);
        let start = self.poll_cursor.fetch_add(1, Ordering::Relaxed);
        let mut blocked = None;
        for offset in 0..lane_count {
            let index = start.wrapping_add(offset) % lane_count;
            let Some(lane) = self.lanes[index].get() else {
                continue;
            };
            match lane.writeback.process_front() {
                ProcessOutcome::Idle => {}
                ProcessOutcome::Blocked => {
                    blocked.get_or_insert(ProcessOutcome::Blocked);
                }
                ProcessOutcome::Pinned(sequence) => {
                    blocked.get_or_insert(ProcessOutcome::Pinned(sequence));
                }
                outcome => return outcome,
            };
        }
        blocked.unwrap_or(ProcessOutcome::Idle)
    }

    pub(crate) fn wake_waiters(&self) {
        for lane in self.initialized_lanes() {
            lane.writeback.wake_waiters();
        }
    }
}

impl<B: Blobs + 'static> RuntimeTarget for WritebackSet<B> {
    fn process_front(&self) -> ProcessOutcome {
        self.process_one_round()
    }

    fn wait_applied(&self) -> Result<u64, ApplyError> {
        WritebackSet::wait_applied(self)
    }

    fn ensure_no_unknown(&self) -> Result<(), ApplyError> {
        WritebackSet::ensure_no_unknown(self)
    }

    fn retry_delay(&self) -> Duration {
        self.initialized_lanes()
            .map(|lane| lane.writeback.retry_delay())
            .max()
            .unwrap_or(self.config.retry_delay)
    }

    fn wake_waiters(&self) {
        WritebackSet::wake_waiters(self)
    }

    fn record_runtime_loop_panic(&self, message: String) {
        WritebackSet::record_runtime_loop_panic(self, message)
    }
}

#[cfg(test)]
mod tests {
    use std::sync::atomic::{AtomicBool, Ordering};
    use std::sync::{Arc, Mutex};

    use mrx_core::fakes::MemBlobs;
    use mrx_core::{BlobError, BlobOp};

    use super::*;
    use crate::record::{BackendKey, Mutation, DEFAULT_TABLE_ID};
    use crate::writeback::ReserveError;
    use crate::CommitSeq;

    fn put(key: &[u8], value: &[u8]) -> Mutation {
        Mutation::Put {
            table_id: DEFAULT_TABLE_ID,
            key: key.to_vec(),
            value: value.to_vec(),
        }
    }

    fn timestamp(logical: u32) -> MakoTimestamp {
        MakoTimestamp::new(1_700_000_000_000_000, logical, 1)
            .expect("test timestamps have a nonzero origin")
    }

    #[derive(Default)]
    struct EnospcBlobs {
        inner: MemBlobs,
        disk_full: AtomicBool,
        attempts: Mutex<Vec<Vec<(Vec<u8>, Option<Vec<u8>>)>>>,
    }

    impl Blobs for EnospcBlobs {
        fn get(&self, key: &[u8]) -> Result<Option<Vec<u8>>, BlobError> {
            self.inner.get(key)
        }
        fn for_each_key(&self, visitor: &mut dyn FnMut(&[u8])) -> Result<(), BlobError> {
            self.inner.for_each_key(visitor)
        }
        fn write_batch(&self, operations: &[BlobOp<'_>]) -> Result<(), BlobError> {
            self.attempts.lock().unwrap().push(
                operations
                    .iter()
                    .map(|operation| match operation {
                        BlobOp::Put { key, val } => (key.to_vec(), Some(val.to_vec())),
                        BlobOp::Delete { key } => (key.to_vec(), None),
                    })
                    .collect(),
            );
            if self.disk_full.load(Ordering::Acquire) {
                Err(BlobError(
                    "IO error: No space left on device (ENOSPC)".into(),
                ))
            } else {
                self.inner.write_batch(operations)
            }
        }
    }

    #[test]
    fn persistent_enospc_gc_blocks_apply_and_capacity_until_exact_retry_succeeds() {
        let backend = Arc::new(EnospcBlobs::default());
        let set = WritebackSet::new(
            Arc::clone(&backend),
            RecoveredWriteback::empty(),
            WritebackConfig {
                capacity: 1,
                max_apply_retries: 2,
                retry_delay: Duration::from_millis(1),
                ..WritebackConfig::default()
            },
            true,
        )
        .unwrap();
        let lane = set.lane(0).unwrap();
        for logical in 1..=2 {
            lane.writeback()
                .reserve_single(
                    lane.producer(),
                    vec![put(
                        b"same",
                        if logical == 1 { b"first" } else { b"second" },
                    )],
                )
                .unwrap()
                .bind(timestamp(logical))
                .unwrap()
                .publish()
                .unwrap();
            if logical == 1 {
                set.wait_applied().unwrap();
            }
        }
        backend.disk_full.store(true, Ordering::Release);
        assert!(set
            .collect_expired_at(u64::MAX)
            .unwrap_err()
            .to_string()
            .contains("ENOSPC"));
        assert!(matches!(
            lane.writeback().process_front(),
            ProcessOutcome::Blocked
        ));
        let started = Instant::now();
        assert!(
            matches!(set.wait_applied(), Err(ApplyError::Backend { attempts: 3, source, .. }) if source.to_string().contains("ENOSPC"))
        );
        assert!(
            started.elapsed() < Duration::from_secs(1),
            "drain must honor its finite retry budget"
        );
        let failed = set.gc_snapshot();
        assert!(failed.pending_retry && failed.active_error);
        assert_eq!((failed.retained_records, failed.reclaimed_records), (1, 0));
        assert_eq!(set.applied_watermark().sequence(), 1);
        assert_eq!(set.queue_len(), 1);
        {
            let attempts = backend.attempts.lock().unwrap();
            assert_eq!(
                attempts.len(),
                5,
                "one apply, first GC attempt, and three drain retries"
            );
            assert!(attempts[1..].iter().all(|attempt| *attempt == attempts[1]));
        }
        std::thread::scope(|scope| {
            let queue = lane.writeback();
            let producer = lane.producer();
            let (sent, received) = std::sync::mpsc::channel();
            let waiter = scope.spawn(move || {
                // The synthetic queue hands its sole producer to this scoped
                // thread. The parent performs consumer work only until join.
                let permit = queue.reserve_single(producer, vec![put(b"blocked", b"value")]);
                sent.send(permit.is_ok()).unwrap();
                drop(permit);
            });
            let while_full = received.recv_timeout(Duration::from_millis(30));
            // Always release disk failure before assertions or scoped joins.
            backend.disk_full.store(false, Ordering::Release);
            let drained = set.wait_applied();
            let admitted = received.recv_timeout(Duration::from_secs(1));
            waiter.join().unwrap();
            assert!(
                while_full.is_err(),
                "a full queue admitted another transaction during GC ENOSPC"
            );
            assert_eq!(drained.unwrap(), 2);
            assert!(admitted.unwrap());
        });
        let attempts = backend.attempts.lock().unwrap();
        assert_eq!(
            attempts[1], attempts[5],
            "recovered disk must receive the identical GC batch first"
        );
        assert_eq!(
            attempts.len(),
            7,
            "only the successful GC retry may precede the queued apply"
        );
        drop(attempts);
        let recovered = set.gc_snapshot();
        assert!(!recovered.pending_retry && !recovered.active_error);
        assert_eq!(
            (recovered.retained_records, recovered.reclaimed_records),
            (1, 1)
        );
        assert_eq!(set.queue_len(), 0);
    }

    #[test]
    fn changing_retention_on_reopen_uses_persisted_frontiers_and_cannot_restore_logs() {
        fn checkpoint_seed(backend: &MemBlobs) -> RecoveredWriteback {
            let mut seed = RecoveredWriteback::empty();
            for (key, value) in backend.snapshot() {
                match crate::record::classify_backend_key(&key) {
                    BackendKey::Lane(tag) => {
                        seed.metadata[usize::from(tag)] = LaneMetadata::decode(&value).unwrap()
                    }
                    BackendKey::Data { .. } => {
                        seed.latest.insert(
                            key,
                            crate::checkpoint::decode_row(&value).unwrap().timestamp,
                        );
                    }
                    _ => {}
                }
            }
            for (tag, metadata) in seed.metadata.iter().enumerate() {
                seed.record_count += metadata.applied;
                let lane = LaneRecovery {
                    local_tail: metadata.applied,
                    mako_timestamp: metadata.max_timestamp,
                };
                if tag == 0 {
                    seed.legacy = lane;
                } else {
                    seed.lanes[tag - 1] = lane;
                }
                seed.maximum_timestamp = seed.maximum_timestamp.max(metadata.max_timestamp);
            }
            seed
        }

        let backend = Arc::new(MemBlobs::new());
        let set = WritebackSet::new(
            Arc::clone(&backend),
            RecoveredWriteback::empty(),
            WritebackConfig::default(),
            true,
        )
        .unwrap();
        let lane = set.lane(0).unwrap();
        lane.writeback()
            .reserve_single(lane.producer(), vec![put(b"key", b"value")])
            .unwrap()
            .bind(timestamp(1))
            .unwrap()
            .publish()
            .unwrap();
        set.wait_applied().unwrap();
        let two_minutes_later = timestamp(1).physical_us() + 120_000_000;
        assert_eq!(
            set.collect_expired_at(two_minutes_later).unwrap(),
            0,
            "default five minutes retains a two-minute-old log"
        );
        drop(set);

        let shorter = WritebackSet::new_with_gc(
            Arc::clone(&backend),
            checkpoint_seed(&backend),
            WritebackConfig::default(),
            true,
            Duration::from_secs(60),
            Duration::from_secs(10),
        )
        .unwrap();
        assert_eq!(shorter.collect_expired_at(two_minutes_later).unwrap(), 1);
        assert_eq!(shorter.applied_watermark().sequence(), 1);
        drop(shorter);

        let longer = WritebackSet::new_with_gc(
            Arc::clone(&backend),
            checkpoint_seed(&backend),
            WritebackConfig::default(),
            true,
            Duration::from_secs(600),
            Duration::from_secs(10),
        )
        .unwrap();
        assert_eq!(longer.collect_expired_at(two_minutes_later).unwrap(), 0);
        assert_eq!(
            longer.gc_snapshot().retained_records,
            0,
            "increasing retention cannot restore reclaimed history"
        );
        let lane = longer.lane(0).unwrap();
        let sequence = lane
            .writeback()
            .reserve_single(lane.producer(), vec![put(b"key", b"new")])
            .unwrap()
            .bind(timestamp(2))
            .unwrap()
            .publish()
            .unwrap();
        assert_eq!(sequence.get(), worker_log_base(0).unwrap() + 2);
        assert_eq!(longer.wait_applied().unwrap(), 2);
        let metadata = LaneMetadata::decode(
            &backend
                .get(&crate::checkpoint::lane_key(1))
                .unwrap()
                .unwrap(),
        )
        .unwrap();
        assert_eq!((metadata.applied, metadata.reclaimed), (2, 1));
        assert_eq!(metadata.max_timestamp, Some(timestamp(2)));
    }

    #[test]
    fn scheduled_gc_alternates_with_apply_and_finishes_pending_retry_on_drain() {
        let backend = Arc::new(MemBlobs::new());
        let set = WritebackSet::new(
            Arc::clone(&backend),
            RecoveredWriteback::empty(),
            WritebackConfig::default(),
            true,
        )
        .unwrap();
        let lane = set.lane(0).unwrap();
        lane.writeback()
            .reserve_single(lane.producer(), vec![put(b"key", b"first")])
            .unwrap()
            .bind(timestamp(1))
            .unwrap()
            .publish()
            .unwrap();
        assert!(matches!(set.process_one_round(), ProcessOutcome::Advanced));
        lane.writeback()
            .reserve_single(lane.producer(), vec![put(b"key", b"second")])
            .unwrap()
            .bind(timestamp(2))
            .unwrap()
            .publish()
            .unwrap();
        set.gc.lock().unwrap().cutoff = Some(u64::MAX);
        assert!(matches!(set.process_one_round(), ProcessOutcome::Advanced));
        assert_eq!(
            set.applied_watermark().sequence(),
            1,
            "first bounded turn collected, without applying the queued transaction"
        );
        assert_eq!(set.gc_snapshot().reclaimed_records, 1);
        assert!(matches!(set.process_one_round(), ProcessOutcome::Advanced));
        assert_eq!(
            set.applied_watermark().sequence(),
            2,
            "next turn gives application a chance"
        );
        backend.fail_next_writes(1);
        assert!(matches!(set.process_one_round(), ProcessOutcome::Blocked));
        assert!(set.gc_snapshot().pending_retry);
        assert_eq!(
            set.wait_applied().unwrap(),
            2,
            "drain retries GC even without queued transactions"
        );
        assert!(!set.gc_snapshot().pending_retry);
        assert!(!set.gc_snapshot().active_error);
        assert_eq!(set.gc_snapshot().retained_records, 0);
        assert_eq!(set.gc_snapshot().reclaimed_records, 2);
        assert!(
            set.gc_snapshot().last_error.is_some(),
            "the resolved error remains diagnostic history"
        );
    }

    #[test]
    fn fifty_minute_clock_model_bounds_retention_independently_of_lifetime_updates() {
        const ORIGIN_US: u64 = 1_700_000_000_000_000;
        let backend = Arc::new(MemBlobs::new());
        let set = WritebackSet::new(
            Arc::clone(&backend),
            RecoveredWriteback::empty(),
            WritebackConfig::default(),
            true,
        )
        .unwrap();
        for minute in 0..50u64 {
            let physical = ORIGIN_US + minute * 60_000_000;
            for worker in 0..4 {
                let lane = set.lane(worker).unwrap();
                for update in 0..32 {
                    let key = format!("worker-{worker}-key-{}", update % 16);
                    let value = format!("minute-{minute}-update-{update}");
                    let timestamp =
                        MakoTimestamp::new(physical, update, worker as u32 + 1).unwrap();
                    lane.writeback()
                        .reserve_single(
                            lane.producer(),
                            vec![put(key.as_bytes(), value.as_bytes())],
                        )
                        .unwrap()
                        .bind(timestamp)
                        .unwrap()
                        .publish()
                        .unwrap();
                }
            }
            assert_eq!(set.wait_applied().unwrap(), (minute + 1) * 128);
            set.collect_expired_at(physical).unwrap();
            let gc = set.gc_snapshot();
            // Strict cutoff retains the boundary minute, hence six buckets.
            assert_eq!(gc.retained_records, (minute + 1).min(6) * 128);
            assert_eq!(
                gc.reclaimed_records + gc.retained_records,
                (minute + 1) * 128
            );
        }
        let snapshot = backend.snapshot();
        assert_eq!(
            snapshot
                .keys()
                .filter(|key| matches!(
                    crate::record::classify_backend_key(key),
                    BackendKey::Data { .. }
                ))
                .count(),
            64
        );
        assert_eq!(set.gc_snapshot().reclaimed_records, 44 * 128);
        set.collect_expired_at(ORIGIN_US + 55 * 60_000_000).unwrap();
        assert_eq!(set.gc_snapshot().retained_records, 0);
        assert_eq!(set.applied_watermark().sequence(), 50 * 128);
        assert_eq!(set.gc_snapshot().reclaimed_records, 50 * 128);
    }

    #[test]
    fn worker_lane_stops_before_the_next_tag_and_local_zero() {
        let mut recovered = RecoveredWriteback::empty();
        recovered.lanes[0].local_tail = LOG_LOCAL_MASK - 1;
        recovered.lanes[0].mako_timestamp = Some(timestamp(0));
        recovered.metadata[1] = LaneMetadata {
            applied: LOG_LOCAL_MASK - 1,
            reclaimed: LOG_LOCAL_MASK - 1,
            max_timestamp: Some(timestamp(0)),
            retained_bytes: 0,
        };
        recovered.record_count = LOG_LOCAL_MASK - 1;
        recovered.maximum_timestamp = Some(timestamp(0));
        let set = WritebackSet::new(
            MemBlobs::new(),
            recovered,
            WritebackConfig {
                capacity: 8,
                ..WritebackConfig::default()
            },
            true,
        )
        .unwrap();
        let lane = set.lane(0).unwrap();

        let mut final_record = lane
            .writeback()
            .reserve_single(lane.producer(), vec![put(b"last", b"value")])
            .unwrap()
            .bind(timestamp(1))
            .unwrap();
        let final_sequence = final_record.publish().unwrap();
        let expected = worker_log_base(0).unwrap() + LOG_LOCAL_MASK;
        assert_eq!(final_sequence.get(), expected);
        assert_eq!(
            crate::record::split_log_sequence(final_sequence),
            Some((Some(0), LOG_LOCAL_MASK))
        );
        assert!(matches!(set.process_one_round(), ProcessOutcome::Advanced));

        assert!(matches!(
            lane.writeback()
                .reserve_single(lane.producer(), vec![put(b"overflow", b"never")]),
            Err(ReserveError::SequenceExhausted)
        ));
        assert_eq!(lane.writeback().highest_acknowledged(), expected);
    }

    struct ApplyThenPanicOnceBlobs {
        inner: MemBlobs,
        panic_once: AtomicBool,
        attempted_batches: Mutex<Vec<Vec<CommitSeq>>>,
    }

    impl ApplyThenPanicOnceBlobs {
        fn new() -> Self {
            Self {
                inner: MemBlobs::new(),
                panic_once: AtomicBool::new(true),
                attempted_batches: Mutex::new(Vec::new()),
            }
        }

        fn attempted_batches(&self) -> Vec<Vec<CommitSeq>> {
            self.attempted_batches
                .lock()
                .expect("attempt history poisoned")
                .clone()
        }

        fn snapshot(&self) -> std::collections::BTreeMap<Vec<u8>, Vec<u8>> {
            self.inner.snapshot()
        }
    }

    struct ApplyThenErrorOnceBlobs {
        inner: MemBlobs,
        error_once: AtomicBool,
        attempted_batches: Mutex<Vec<Vec<CommitSeq>>>,
    }

    impl ApplyThenErrorOnceBlobs {
        fn new() -> Self {
            Self {
                inner: MemBlobs::new(),
                error_once: AtomicBool::new(true),
                attempted_batches: Mutex::new(Vec::new()),
            }
        }

        fn attempted_batches(&self) -> Vec<Vec<CommitSeq>> {
            self.attempted_batches
                .lock()
                .expect("attempt history poisoned")
                .clone()
        }

        fn snapshot(&self) -> std::collections::BTreeMap<Vec<u8>, Vec<u8>> {
            self.inner.snapshot()
        }
    }

    impl Blobs for ApplyThenErrorOnceBlobs {
        fn get(&self, key: &[u8]) -> Result<Option<Vec<u8>>, BlobError> {
            self.inner.get(key)
        }

        fn write_batch(&self, operations: &[BlobOp<'_>]) -> Result<(), BlobError> {
            let sequences = operations
                .iter()
                .filter_map(|operation| {
                    let key = match operation {
                        BlobOp::Put { key, .. } | BlobOp::Delete { key } => *key,
                    };
                    match crate::record::classify_backend_key(key) {
                        BackendKey::Log(sequence) => Some(sequence),
                        BackendKey::Data { .. }
                        | BackendKey::Format
                        | BackendKey::Lane(_)
                        | BackendKey::Foreign => None,
                    }
                })
                .collect();
            self.attempted_batches
                .lock()
                .expect("attempt history poisoned")
                .push(sequences);

            self.inner.write_batch(operations)?;
            if self.error_once.swap(false, Ordering::SeqCst) {
                return Err(BlobError(
                    "injected error after applying backend batch".to_owned(),
                ));
            }
            Ok(())
        }

        fn for_each_key(&self, f: &mut dyn FnMut(&[u8])) -> Result<(), BlobError> {
            self.inner.for_each_key(f)
        }
    }

    impl Blobs for ApplyThenPanicOnceBlobs {
        fn get(&self, key: &[u8]) -> Result<Option<Vec<u8>>, BlobError> {
            self.inner.get(key)
        }

        fn write_batch(&self, operations: &[BlobOp<'_>]) -> Result<(), BlobError> {
            let sequences = operations
                .iter()
                .filter_map(|operation| {
                    let key = match operation {
                        BlobOp::Put { key, .. } | BlobOp::Delete { key } => *key,
                    };
                    match crate::record::classify_backend_key(key) {
                        BackendKey::Log(sequence) => Some(sequence),
                        BackendKey::Data { .. }
                        | BackendKey::Format
                        | BackendKey::Lane(_)
                        | BackendKey::Foreign => None,
                    }
                })
                .collect();
            self.attempted_batches
                .lock()
                .expect("attempt history poisoned")
                .push(sequences);

            self.inner.write_batch(operations)?;
            if self.panic_once.swap(false, Ordering::SeqCst) {
                panic!("injected panic after applying backend batch");
            }
            Ok(())
        }

        fn for_each_key(&self, f: &mut dyn FnMut(&[u8])) -> Result<(), BlobError> {
            self.inner.for_each_key(f)
        }
    }

    #[test]
    fn lanes_publish_independently_and_late_stale_apply_cannot_win() {
        let backend = Arc::new(MemBlobs::new());
        let set = WritebackSet::new(
            Arc::clone(&backend),
            RecoveredWriteback::empty(),
            WritebackConfig::default(),
            true,
        )
        .unwrap();
        let newer_lane = set.lane(0).unwrap();
        let older_lane = set.lane(1).unwrap();

        let newer = newer_lane
            .writeback()
            .reserve_single(newer_lane.producer(), vec![put(b"shared", b"new")])
            .unwrap()
            .bind(timestamp(20))
            .unwrap()
            .publish()
            .unwrap();
        let older = older_lane
            .writeback()
            .reserve_single(older_lane.producer(), vec![put(b"shared", b"old")])
            .unwrap()
            .bind(timestamp(10))
            .unwrap()
            .publish()
            .unwrap();

        assert_eq!(newer.get(), worker_log_base(0).unwrap() + 1);
        assert_eq!(older.get(), worker_log_base(1).unwrap() + 1);
        assert_eq!(set.highest_acknowledged(), 2);
        assert!(matches!(set.process_one_round(), ProcessOutcome::Advanced));
        assert!(matches!(set.process_one_round(), ProcessOutcome::Advanced));
        assert_eq!(set.applied_watermark().sequence(), 2);

        let snapshot = backend.snapshot();
        let data_value = snapshot
            .iter()
            .find_map(|(key, value)| {
                matches!(
                    crate::record::classify_backend_key(key),
                    BackendKey::Data { key, .. } if key == b"shared"
                )
                .then(|| value.clone())
            })
            .unwrap();
        assert_eq!(
            crate::checkpoint::decode_row(&data_value).unwrap().value,
            Some(b"new".as_slice())
        );
        assert_eq!(
            snapshot
                .iter()
                .filter(|(key, _)| matches!(
                    crate::record::classify_backend_key(key),
                    BackendKey::Log(_)
                ))
                .count(),
            2
        );
    }

    #[test]
    fn apply_then_panic_retries_exact_batch_before_other_lane_or_ready_suffix() {
        let backend = Arc::new(ApplyThenPanicOnceBlobs::new());
        let set = WritebackSet::new(
            Arc::clone(&backend),
            RecoveredWriteback::empty(),
            WritebackConfig::default(),
            true,
        )
        .unwrap();
        let newer_lane = set.lane(0).unwrap();
        let older_lane = set.lane(1).unwrap();

        let newer = newer_lane
            .writeback()
            .reserve_single(newer_lane.producer(), vec![put(b"shared", b"new")])
            .unwrap()
            .bind(timestamp(20))
            .unwrap()
            .publish()
            .unwrap();
        let older = older_lane
            .writeback()
            .reserve_single(older_lane.producer(), vec![put(b"shared", b"old")])
            .unwrap()
            .bind(timestamp(10))
            .unwrap()
            .publish()
            .unwrap();

        assert!(matches!(
            set.process_one_round(),
            ProcessOutcome::BackendFailed { sequence, .. } if sequence == newer
        ));
        assert_eq!(backend.attempted_batches(), vec![vec![newer]]);

        // This suffix becomes Ready only after the uncertain batch was
        // captured. A normal greedy recapture would include it and therefore
        // never match the coordinator's exact retry identity.
        let suffix = newer_lane
            .writeback()
            .reserve_single(newer_lane.producer(), vec![put(b"suffix", b"value")])
            .unwrap()
            .bind(timestamp(30))
            .unwrap()
            .publish()
            .unwrap();
        assert_eq!(suffix.get(), newer.get() + 1);

        // Round-robin starts at the other lane. Its older batch must not reach
        // storage, while the exact uncertain batch later in the same sweep is
        // allowed to retry and retire.
        assert!(matches!(set.process_one_round(), ProcessOutcome::Advanced));
        assert_eq!(backend.attempted_batches(), vec![vec![newer], vec![newer]]);
        assert_eq!(newer_lane.writeback().applied_sequence(), newer.get());
        assert_eq!(older_lane.writeback().applied_sequence(), older.get() - 1);

        assert!(matches!(set.process_one_round(), ProcessOutcome::Advanced));
        assert_eq!(
            backend.attempted_batches(),
            vec![vec![newer], vec![newer], vec![suffix]]
        );
        assert_eq!(newer_lane.writeback().applied_sequence(), suffix.get());
        assert_eq!(older_lane.writeback().applied_sequence(), older.get() - 1);

        assert!(matches!(set.process_one_round(), ProcessOutcome::Advanced));
        assert_eq!(
            backend.attempted_batches(),
            vec![vec![newer], vec![newer], vec![suffix], vec![older]]
        );
        assert_eq!(set.applied_watermark().sequence(), 3);

        let snapshot = backend.snapshot();
        let data_value = snapshot
            .iter()
            .find_map(|(key, value)| {
                matches!(
                    crate::record::classify_backend_key(key),
                    BackendKey::Data { key, .. } if key == b"shared"
                )
                .then(|| value.clone())
            })
            .unwrap();
        assert_eq!(
            crate::checkpoint::decode_row(&data_value).unwrap().value,
            Some(b"new".as_slice())
        );
        assert_eq!(
            snapshot
                .keys()
                .filter(|key| matches!(
                    crate::record::classify_backend_key(key),
                    BackendKey::Log(_)
                ))
                .count(),
            3
        );
    }

    #[test]
    fn apply_then_error_retries_exact_physical_batch_before_same_local_other_lane() {
        let backend = Arc::new(ApplyThenErrorOnceBlobs::new());
        let set = WritebackSet::new(
            Arc::clone(&backend),
            RecoveredWriteback::empty(),
            WritebackConfig::default(),
            true,
        )
        .unwrap();
        let newer_lane = set.lane(0).unwrap();
        let older_lane = set.lane(1).unwrap();

        let newer = newer_lane
            .writeback()
            .reserve_single(newer_lane.producer(), vec![put(b"shared", b"new")])
            .unwrap()
            .bind(timestamp(20))
            .unwrap()
            .publish()
            .unwrap();
        let older = older_lane
            .writeback()
            .reserve_single(older_lane.producer(), vec![put(b"shared", b"old")])
            .unwrap()
            .bind(timestamp(10))
            .unwrap()
            .publish()
            .unwrap();

        assert_eq!(crate::record::split_log_sequence(newer), Some((Some(0), 1)));
        assert_eq!(crate::record::split_log_sequence(older), Some((Some(1), 1)));
        assert_ne!(
            newer, older,
            "retry identity must include the physical worker-lane tag"
        );

        assert!(matches!(
            newer_lane.writeback().process_front(),
            ProcessOutcome::BackendFailed { sequence, .. } if sequence == newer
        ));
        assert_eq!(backend.attempted_batches(), vec![vec![newer]]);

        let value_after_error = backend.snapshot().into_iter().find_map(|(key, value)| {
            matches!(
                crate::record::classify_backend_key(&key),
                BackendKey::Data { key, .. } if key == b"shared"
            )
            .then_some(value)
        });
        assert_eq!(
            crate::checkpoint::decode_row(&value_after_error.unwrap())
                .unwrap()
                .value,
            Some(&b"new"[..]),
            "the injected error must model an already-applied atomic batch"
        );

        // Both records have lane-local position one. The other lane still
        // cannot pass because retry identity uses their full physical IDs.
        assert!(matches!(
            older_lane.writeback().process_front(),
            ProcessOutcome::Blocked
        ));
        assert_eq!(backend.attempted_batches(), vec![vec![newer]]);

        // A suffix published after the error must not be folded into the
        // required retry batch.
        let suffix = newer_lane
            .writeback()
            .reserve_single(newer_lane.producer(), vec![put(b"suffix", b"value")])
            .unwrap()
            .bind(timestamp(30))
            .unwrap()
            .publish()
            .unwrap();
        assert!(matches!(
            newer_lane.writeback().process_front(),
            ProcessOutcome::Advanced
        ));
        assert_eq!(backend.attempted_batches(), vec![vec![newer], vec![newer]]);
        assert_eq!(newer_lane.writeback().applied_sequence(), newer.get());
        assert_eq!(newer_lane.writeback().queue_len(), 1);

        // Once the exact retry succeeds, normal cross-lane replay resumes.
        // Timestamp arbitration keeps the older same-key value from replacing
        // the already-applied newer value.
        assert!(matches!(
            older_lane.writeback().process_front(),
            ProcessOutcome::Advanced
        ));
        assert!(matches!(
            newer_lane.writeback().process_front(),
            ProcessOutcome::Advanced
        ));
        assert_eq!(
            backend.attempted_batches(),
            vec![vec![newer], vec![newer], vec![older], vec![suffix]]
        );
        let final_value = backend.snapshot().into_iter().find_map(|(key, value)| {
            matches!(
                crate::record::classify_backend_key(&key),
                BackendKey::Data { key, .. } if key == b"shared"
            )
            .then_some(value)
        });
        assert_eq!(
            crate::checkpoint::decode_row(&final_value.unwrap())
                .unwrap()
                .value,
            Some(b"new".as_slice())
        );
        assert_eq!(set.applied_watermark().sequence(), 3);
    }

    #[test]
    fn set_wait_drives_the_retry_lane_after_an_earlier_lane_is_blocked() {
        let backend = Arc::new(ApplyThenPanicOnceBlobs::new());
        let set = WritebackSet::new(
            Arc::clone(&backend),
            RecoveredWriteback::empty(),
            WritebackConfig::default(),
            true,
        )
        .unwrap();
        let earlier_lane = set.lane(0).unwrap();
        let retry_lane = set.lane(1).unwrap();

        let older = earlier_lane
            .writeback()
            .reserve_single(earlier_lane.producer(), vec![put(b"shared", b"old")])
            .unwrap()
            .bind(timestamp(10))
            .unwrap()
            .publish()
            .unwrap();
        let newer = retry_lane
            .writeback()
            .reserve_single(retry_lane.producer(), vec![put(b"shared", b"new")])
            .unwrap()
            .bind(timestamp(20))
            .unwrap()
            .publish()
            .unwrap();

        assert!(matches!(
            retry_lane.writeback().process_front(),
            ProcessOutcome::BackendFailed { sequence, .. } if sequence == newer
        ));
        assert_eq!(backend.attempted_batches(), vec![vec![newer]]);

        assert_eq!(set.wait_applied().unwrap(), 2);
        assert_eq!(
            backend.attempted_batches(),
            vec![vec![newer], vec![newer], vec![older]]
        );
        let snapshot = backend.snapshot();
        let value = snapshot
            .iter()
            .find_map(|(key, value)| {
                matches!(
                    crate::record::classify_backend_key(key),
                    BackendKey::Data { key, .. } if key == b"shared"
                )
                .then(|| value.clone())
            })
            .unwrap();
        assert_eq!(
            crate::checkpoint::decode_row(&value).unwrap().value,
            Some(b"new".as_slice())
        );
    }

    #[test]
    fn exact_uncertain_retry_may_cross_an_older_barrier_target() {
        let backend = Arc::new(ApplyThenPanicOnceBlobs::new());
        let set = WritebackSet::new(
            Arc::clone(&backend),
            RecoveredWriteback::empty(),
            WritebackConfig::default(),
            true,
        )
        .unwrap();
        let lane = set.lane(0).unwrap();

        let first = lane
            .writeback()
            .reserve_single(lane.producer(), vec![put(b"first", b"one")])
            .unwrap()
            .bind(timestamp(10))
            .unwrap()
            .publish()
            .unwrap();
        let second = lane
            .writeback()
            .reserve_single(lane.producer(), vec![put(b"second", b"two")])
            .unwrap()
            .bind(timestamp(20))
            .unwrap()
            .publish()
            .unwrap();

        assert!(matches!(
            lane.writeback().process_front(),
            ProcessOutcome::BackendFailed { sequence, .. } if sequence == first
        ));
        assert_eq!(backend.attempted_batches(), vec![vec![first, second]]);

        // The exact two-record batch may already be visible atomically. A
        // barrier for its first record must resolve that complete uncertainty
        // instead of truncating the retry and waiting forever.
        assert!(matches!(
            lane.writeback().process_front_through(Some(first.get())),
            ProcessOutcome::Advanced
        ));
        assert_eq!(lane.writeback().applied_sequence(), second.get());
        assert_eq!(
            backend.attempted_batches(),
            vec![vec![first, second], vec![first, second]]
        );
    }

    #[test]
    fn one_lane_failure_rejects_every_lane_with_the_same_physical_id() {
        let set = WritebackSet::new(
            Arc::new(MemBlobs::new()),
            RecoveredWriteback::empty(),
            WritebackConfig::default(),
            true,
        )
        .unwrap();
        let failing = set.lane(0).unwrap();
        let other = set.lane(1).unwrap();
        let mut bound = failing
            .writeback()
            .reserve_single(failing.producer(), vec![put(b"uncertain", b"value")])
            .unwrap()
            .bind(timestamp(30))
            .unwrap();
        let failed_sequence = bound.pin_unknown().unwrap();

        assert!(matches!(
            other
                .writeback()
                .reserve_single(other.producer(), vec![put(b"later", b"value")]),
            Err(ReserveError::UnknownOutcome { sequence }) if sequence == failed_sequence
        ));
        assert!(matches!(
            set.ensure_no_unknown(),
            Err(ApplyError::UnknownOutcome { sequence }) if sequence == failed_sequence
        ));
    }

    #[test]
    fn one_lane_failure_blocks_an_already_prepared_commit_in_another_lane() {
        let backend = Arc::new(MemBlobs::new());
        let set = WritebackSet::new(
            Arc::clone(&backend),
            RecoveredWriteback::empty(),
            WritebackConfig::default(),
            true,
        )
        .unwrap();
        let failing = set.lane(0).unwrap();
        let other = set.lane(1).unwrap();

        let mut prepared_other = other
            .writeback()
            .reserve_single(other.producer(), vec![put(b"other", b"known")])
            .unwrap()
            .bind(timestamp(31))
            .unwrap();
        let mut uncertain = failing
            .writeback()
            .reserve_single(failing.producer(), vec![put(b"uncertain", b"value")])
            .unwrap()
            .bind(timestamp(30))
            .unwrap();
        let failed_sequence = uncertain.pin_unknown().unwrap();

        let known_sequence = prepared_other.sequence();
        assert!(matches!(
            prepared_other.publish(),
            Err(crate::writeback::ResolveError::BlockedByPriorUnknown {
                sequence,
                prior_unknown,
            }) if sequence == known_sequence && prior_unknown == failed_sequence
        ));
        assert_eq!(
            other.writeback().highest_caller_acknowledged(),
            worker_log_base(1).unwrap()
        );
        assert!(matches!(
            set.process_one_round(),
            ProcessOutcome::Pinned(sequence) if sequence == failed_sequence
        ));
        assert!(matches!(
            set.wait_applied(),
            Err(ApplyError::UnknownOutcome { sequence }) if sequence == failed_sequence
        ));
        assert!(backend.snapshot().is_empty());
    }
}
