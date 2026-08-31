//! Test-only process-crash rendezvous points.

use std::cell::Cell;
use std::fs::File;
use std::io::Write;
use std::path::PathBuf;
use std::sync::OnceLock;
use std::sync::atomic::{AtomicBool, Ordering};

/// Stable names for deterministic process-crash matrices.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum Point {
    BeforeDetachedPreparation,
    DetachedPrepared,
    NativeWritesetLocked,
    NativeTimestampAllocated,
    NativeValidationComplete,
    PreinstallBound,
    NativePreinstallAccepted,
    NativeFirstWriteInstalled,
    NativeAllWritesInstalled,
    NativeCommittedBeforeReady,
    ReadyBeforeBackend,
    RocksBatchConstructed,
    RocksBeforeWrite,
    RocksAfterWrite,
    BackendWrittenBeforeApplied,
    AppliedAdvanced,
    RecoveryKeysEnumerated,
    RecoveryFirstRecordValidated,
    RecoveryLastRecordValidated,
    RecoveryMaterializedValidated,
    RecoveryBeforeClockFloor,
    RecoveryAfterClockFloor,
    RecoveryMidReplay,
    RecoveryReplayComplete,
}

impl Point {
    pub(crate) const fn name(self) -> &'static str {
        match self {
            Self::BeforeDetachedPreparation => "before-detached-preparation",
            Self::DetachedPrepared => "after-detached-preparation",
            Self::NativeWritesetLocked => "native-after-writeset-locked",
            Self::NativeTimestampAllocated => "native-after-timestamp-allocation",
            Self::NativeValidationComplete => "native-after-validation",
            Self::PreinstallBound => "after-preinstall-bind",
            Self::NativePreinstallAccepted => "native-after-preinstall-accepted",
            Self::NativeFirstWriteInstalled => "native-after-first-write-install",
            Self::NativeAllWritesInstalled => "native-after-all-write-install",
            Self::NativeCommittedBeforeReady => "after-native-commit-before-ready",
            Self::ReadyBeforeBackend => "after-ready-before-backend",
            Self::RocksBatchConstructed => "rocks-after-batch-construction",
            Self::RocksBeforeWrite => "rocks-before-write-call",
            Self::RocksAfterWrite => "rocks-after-write-call",
            Self::BackendWrittenBeforeApplied => "after-backend-before-applied",
            Self::AppliedAdvanced => "after-applied-advance",
            Self::RecoveryKeysEnumerated => "recovery-after-key-enumeration",
            Self::RecoveryFirstRecordValidated => "recovery-after-first-record-validation",
            Self::RecoveryLastRecordValidated => "recovery-after-last-record-validation",
            Self::RecoveryMaterializedValidated => "recovery-after-materialized-validation",
            Self::RecoveryBeforeClockFloor => "recovery-before-clock-floor",
            Self::RecoveryAfterClockFloor => "recovery-after-clock-floor",
            Self::RecoveryMidReplay => "recovery-mid-native-replay",
            Self::RecoveryReplayComplete => "recovery-after-replay-before-exposure",
        }
    }

    pub(crate) fn from_name(name: &str) -> Option<Self> {
        match name {
            "before-detached-preparation" => Some(Self::BeforeDetachedPreparation),
            "after-detached-preparation" => Some(Self::DetachedPrepared),
            "native-after-writeset-locked" => Some(Self::NativeWritesetLocked),
            "native-after-timestamp-allocation" => Some(Self::NativeTimestampAllocated),
            "native-after-validation" => Some(Self::NativeValidationComplete),
            "after-preinstall-bind" => Some(Self::PreinstallBound),
            "native-after-preinstall-accepted" => Some(Self::NativePreinstallAccepted),
            "native-after-first-write-install" => Some(Self::NativeFirstWriteInstalled),
            "native-after-all-write-install" => Some(Self::NativeAllWritesInstalled),
            "after-native-commit-before-ready" => Some(Self::NativeCommittedBeforeReady),
            "after-ready-before-backend" => Some(Self::ReadyBeforeBackend),
            "rocks-after-batch-construction" => Some(Self::RocksBatchConstructed),
            "rocks-before-write-call" => Some(Self::RocksBeforeWrite),
            "rocks-after-write-call" => Some(Self::RocksAfterWrite),
            "after-backend-before-applied" | "after-backend-before-durable" => {
                Some(Self::BackendWrittenBeforeApplied)
            }
            "after-applied-advance" | "after-durable-advance" => Some(Self::AppliedAdvanced),
            "recovery-after-key-enumeration" => Some(Self::RecoveryKeysEnumerated),
            "recovery-after-first-record-validation" => Some(Self::RecoveryFirstRecordValidated),
            "recovery-after-last-record-validation" => Some(Self::RecoveryLastRecordValidated),
            "recovery-after-materialized-validation" => Some(Self::RecoveryMaterializedValidated),
            "recovery-before-clock-floor" => Some(Self::RecoveryBeforeClockFloor),
            "recovery-after-clock-floor" => Some(Self::RecoveryAfterClockFloor),
            "recovery-mid-native-replay" => Some(Self::RecoveryMidReplay),
            "recovery-after-replay-before-exposure" => Some(Self::RecoveryReplayComplete),
            _ => None,
        }
    }

    pub(crate) const fn requires_native_observer(self) -> bool {
        matches!(
            self,
            Self::NativeWritesetLocked
                | Self::NativeTimestampAllocated
                | Self::NativeValidationComplete
                | Self::NativePreinstallAccepted
                | Self::NativeFirstWriteInstalled
                | Self::NativeAllWritesInstalled
        )
    }
}

struct Armed {
    point: Point,
    marker: File,
    fired: AtomicBool,
}

static ARMED: OnceLock<Armed> = OnceLock::new();

thread_local! {
    /// A Rust-side response-delay seam. Unlike the native phase observer, this
    /// runs only after the commit C ABI has returned and released Silo locks.
    static POST_NATIVE_COMMIT_OBSERVER: Cell<Option<fn()>> = const { Cell::new(None) };
}

/// Install a callback for the post-native, pre-publication point on this
/// thread. Application-history tests use it to reorder wrapper responses
/// without parking inside Silo's lock-holding critical section.
pub(crate) fn install_post_native_commit_observer(observer: fn()) {
    POST_NATIVE_COMMIT_OBSERVER.with(|slot| {
        assert!(
            slot.replace(Some(observer)).is_none(),
            "post-native commit observer already installed on this thread"
        );
    });
}

/// Remove the current thread's post-native commit observer.
pub(crate) fn clear_post_native_commit_observer() {
    POST_NATIVE_COMMIT_OBSERVER.with(|slot| {
        assert!(
            slot.replace(None).is_some(),
            "post-native commit observer is not installed on this thread"
        );
    });
}

/// Invoke the current thread's post-native commit observer, if any.
pub(crate) fn observe_post_native_commit() {
    POST_NATIVE_COMMIT_OBSERVER.with(|slot| {
        if let Some(observer) = slot.get() {
            observer();
        }
    });
}

/// Arm one point in a fresh helper process.
pub(crate) fn arm(point: Point, marker: PathBuf) {
    let armed = Armed {
        point,
        // Open and allocate the rendezvous resource before native commit can
        // hold locks. `hit` then needs only write/fsync syscalls and parking.
        marker: File::create(marker).expect("create crash rendezvous marker"),
        fired: AtomicBool::new(false),
    };
    assert!(ARMED.set(armed).is_ok(), "a crash helper arms only once");
}

/// Publish a rendezvous marker and park until the parent sends SIGKILL.
///
/// When no point is armed, or a different point is armed, this is only a
/// `OnceLock` read and scalar comparison. Production builds contain neither
/// this function nor its call sites.
pub(crate) fn hit(point: Point) {
    let Some(armed) = ARMED.get() else {
        return;
    };
    if armed.point != point || armed.fired.swap(true, Ordering::SeqCst) {
        return;
    }

    let mut marker = &armed.marker;
    marker
        .write_all(point.name().as_bytes())
        .expect("write crash rendezvous marker");
    marker.sync_all().expect("sync crash rendezvous marker");

    loop {
        std::thread::park();
    }
}
