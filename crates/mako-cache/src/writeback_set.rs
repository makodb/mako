//! Per-worker SPSC writeback lanes for the concurrent cache profile.

use std::collections::HashMap;
use std::sync::atomic::{AtomicBool, AtomicU64, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex, OnceLock};
use std::time::Duration;

use mako_local::MakoTimestamp;
use mrx_core::Blobs;

use crate::record::{worker_log_base, LOG_LOCAL_MASK};
use crate::runtime::RuntimeTarget;
use crate::writeback::{
    AppliedWatermark, ApplyCoordinator, ApplyError, ConfigError, ProcessOutcome,
    SingleProducerState, Writeback, WritebackConfig,
};

#[derive(Clone, Copy, Debug, Default)]
pub(crate) struct LaneRecovery {
    pub(crate) local_tail: u64,
    pub(crate) mako_timestamp: Option<MakoTimestamp>,
}

/// Backend state reconstructed before foreground work is admitted.
pub(crate) struct RecoveredWriteback {
    pub(crate) legacy: LaneRecovery,
    pub(crate) lanes: Vec<LaneRecovery>,
    pub(crate) latest: HashMap<Vec<u8>, MakoTimestamp>,
    pub(crate) record_count: u64,
    pub(crate) maximum_timestamp: Option<MakoTimestamp>,
}

impl RecoveredWriteback {
    pub(crate) fn empty() -> Self {
        Self {
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
}

impl<B: Blobs + 'static> WritebackSet<B> {
    pub(crate) fn new(
        backend: B,
        recovered: RecoveredWriteback,
        config: WritebackConfig,
        concurrent: bool,
    ) -> Result<Self, ConfigError> {
        Writeback::<Arc<B>>::validate_config(config, 0, LOG_LOCAL_MASK)?;
        let set = Self {
            backend: Arc::new(backend),
            config,
            concurrent,
            unhealthy: Arc::new(AtomicBool::new(false)),
            unhealthy_sequence: Arc::new(AtomicU64::new(0)),
            coordinator: Arc::new(ApplyCoordinator::recovered(recovered.latest)),
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
        loop {
            self.ensure_no_unknown()?;
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
        if !self.concurrent {
            return self
                .legacy_lane
                .get()
                .expect("single-producer construction initializes its lane")
                .writeback
                .process_front();
        }

        if self.unhealthy.load(Ordering::Acquire) {
            let sequence = crate::CommitSeq::new(self.unhealthy_sequence.load(Ordering::Acquire))
                .unwrap_or_else(|| std::process::abort());
            return ProcessOutcome::Pinned(sequence);
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

    #[test]
    fn worker_lane_stops_before_the_next_tag_and_local_zero() {
        let mut recovered = RecoveredWriteback::empty();
        recovered.lanes[0].local_tail = LOG_LOCAL_MASK - 1;
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
                        BackendKey::Data { .. } | BackendKey::Foreign => None,
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
        assert_eq!(data_value, b"new");
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
        assert_eq!(data_value, b"new");
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
        assert_eq!(value, b"new");
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
