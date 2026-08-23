//! Test-only process-crash rendezvous points.

use std::fs::File;
use std::io::Write;
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::OnceLock;

/// Stable names for the first deterministic process-crash matrix.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum Point {
    DetachedPrepared,
    PreinstallBound,
    NativeCommittedBeforeReady,
    ReadyBeforeBackend,
    BackendWrittenBeforeDurable,
    DurableAdvanced,
}

impl Point {
    pub(crate) const fn name(self) -> &'static str {
        match self {
            Self::DetachedPrepared => "after-detached-preparation",
            Self::PreinstallBound => "after-preinstall-bind",
            Self::NativeCommittedBeforeReady => "after-native-commit-before-ready",
            Self::ReadyBeforeBackend => "after-ready-before-backend",
            Self::BackendWrittenBeforeDurable => "after-backend-before-durable",
            Self::DurableAdvanced => "after-durable-advance",
        }
    }

    pub(crate) fn from_name(name: &str) -> Option<Self> {
        match name {
            "after-detached-preparation" => Some(Self::DetachedPrepared),
            "after-preinstall-bind" => Some(Self::PreinstallBound),
            "after-native-commit-before-ready" => Some(Self::NativeCommittedBeforeReady),
            "after-ready-before-backend" => Some(Self::ReadyBeforeBackend),
            "after-backend-before-durable" => Some(Self::BackendWrittenBeforeDurable),
            "after-durable-advance" => Some(Self::DurableAdvanced),
            _ => None,
        }
    }
}

struct Armed {
    point: Point,
    marker: PathBuf,
    fired: AtomicBool,
}

static ARMED: OnceLock<Armed> = OnceLock::new();

/// Arm one point in a fresh helper process.
pub(crate) fn arm(point: Point, marker: PathBuf) {
    let armed = Armed {
        point,
        marker,
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

    let mut marker = File::create(&armed.marker).expect("create crash rendezvous marker");
    marker
        .write_all(point.name().as_bytes())
        .expect("write crash rendezvous marker");
    marker.sync_all().expect("sync crash rendezvous marker");
    drop(marker);

    loop {
        std::thread::park();
    }
}
