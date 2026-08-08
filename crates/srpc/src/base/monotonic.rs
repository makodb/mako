//! Shared monotonic microsecond clock for generated reliability modules.
//!
//! The nonzero base preserves the legacy convention that a stored timestamp
//! of zero means "never". Only elapsed differences are observable; the epoch
//! itself is deliberately process-local.

use std::sync::Mutex;
use std::time::Instant;

const NONZERO_EPOCH_US: u64 = 1_u64 << 56;

static MONOTONIC_EPOCH: Mutex<Option<Instant>> = Mutex::new(None);

pub fn monotonic_time_us() -> u64 {
    let mut epoch = MONOTONIC_EPOCH.lock().unwrap();
    NONZERO_EPOCH_US + epoch.get_or_insert_with(Instant::now).elapsed().as_micros() as u64
}
