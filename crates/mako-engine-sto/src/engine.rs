//! The engine marker type and process-wide services.

use mako_engine_api::{
    Error, Features, LocalEngine, MakoTimestamp, Result, TestCleanupBoundary, TestCommitObserver,
    WorkerHealth,
};

use crate::worker;

/// The native Rust STO engine.
#[derive(Debug, Clone, Copy, Default)]
pub struct MakoStoEngine;

/// Capabilities of this engine.
pub fn features() -> Result<Features> {
    let mut bits = Features::POINT_TRANSACTIONS
        | Features::READ_MY_WRITES
        | Features::TRANSACTIONAL_SCANS
        | Features::SCAN_READ_MY_WRITES;
    if cfg!(feature = "test-hooks") {
        bits |= Features::TEST_COMMIT_OBSERVER
            | Features::TEST_CLEANUP_FAILURES
            | Features::TEST_TIMESTAMP_CLOCK;
    }
    Ok(Features::from_bits(bits))
}

/// Raise the process-wide Mako timestamp floor above `observed`.
pub fn advance_mako_timestamp_past(observed: MakoTimestamp) -> Result<()> {
    if mako_clock::global().advance_past(observed) {
        Ok(())
    } else {
        Err(Error::TimestampExhausted)
    }
}

/// Health of the current OS worker.
pub fn worker_health() -> Result<WorkerHealth> {
    Ok(worker::health())
}

/// Number of workers retired by uncertain cleanup in this process.
pub fn quarantined_worker_count() -> Result<u64> {
    Ok(worker::quarantined_worker_count())
}

/// Install the test-only synchronous commit observer for this worker.
pub fn install_test_commit_observer(observer: TestCommitObserver) -> Result<()> {
    #[cfg(feature = "test-hooks")]
    {
        worker::install_observer(observer);
        Ok(())
    }
    #[cfg(not(feature = "test-hooks"))]
    {
        let _ = observer;
        Err(Error::FeatureUnavailable)
    }
}

/// Remove the test-only commit observer.
pub fn clear_test_commit_observer() -> Result<()> {
    #[cfg(feature = "test-hooks")]
    {
        worker::clear_observer();
        Ok(())
    }
    #[cfg(not(feature = "test-hooks"))]
    {
        Err(Error::FeatureUnavailable)
    }
}

/// Arm a one-shot test-only cleanup failure on the current worker.
pub fn arm_test_cleanup_failure(boundary: TestCleanupBoundary) -> Result<()> {
    #[cfg(feature = "test-hooks")]
    {
        worker::arm_cleanup_failure(boundary);
        Ok(())
    }
    #[cfg(not(feature = "test-hooks"))]
    {
        let _ = boundary;
        Err(Error::FeatureUnavailable)
    }
}

/// Disarm a pending test-only cleanup failure.
pub fn clear_test_cleanup_failure() -> Result<()> {
    #[cfg(feature = "test-hooks")]
    {
        worker::clear_cleanup_failure();
        Ok(())
    }
    #[cfg(not(feature = "test-hooks"))]
    {
        Err(Error::FeatureUnavailable)
    }
}

impl LocalEngine for MakoStoEngine {
    const NAME: &'static str = "sto";

    type Db = crate::LocalDb;
    type HolderPool = crate::TrustedOnePutHolderPool;
    type SpscHolderControl = crate::TrustedSpscOnePutHolderControl;
    type NativeOrderedArenaControl = crate::TrustedNativeOrderedArenaControl;
    type NativeOrderedHolderControl = crate::TrustedNativeOrderedHolderControl;
    type PreselectedHolderOutcome = crate::TrustedPreselectedUncheckedOnePutHolderOutcome;
    type PreselectedRecordOutcome = crate::TrustedPreselectedUncheckedOnePutRecordOutcome;
    type NativeOrderedRecordOutcome = crate::TrustedNativeOrderedOnePutRecordOutcome;

    fn features() -> Result<Features> {
        features()
    }
    fn advance_mako_timestamp_past(observed: MakoTimestamp) -> Result<()> {
        advance_mako_timestamp_past(observed)
    }
    fn worker_health() -> Result<WorkerHealth> {
        worker_health()
    }
    fn quarantined_worker_count() -> Result<u64> {
        quarantined_worker_count()
    }
    fn install_test_commit_observer(observer: TestCommitObserver) -> Result<()> {
        install_test_commit_observer(observer)
    }
    fn clear_test_commit_observer() -> Result<()> {
        clear_test_commit_observer()
    }
    fn arm_test_cleanup_failure(boundary: TestCleanupBoundary) -> Result<()> {
        arm_test_cleanup_failure(boundary)
    }
    fn clear_test_cleanup_failure() -> Result<()> {
        clear_test_cleanup_failure()
    }
}
