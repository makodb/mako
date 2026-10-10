//! Timing samples for the disk path, for checking the cost model (plan §5):
//! inert unless MAKO_RAFT_DISK_STATS=1, then each metric keeps up to 200k
//! microsecond samples, summarised by `report` at shutdown.

use std::sync::{Mutex, OnceLock};
use std::time::Instant;

pub const QUEUE_WAIT: usize = 0; // first record pushed -> its batch's flush starts
pub const FLUSH: usize = 1; // a batch: encode, write, sync, delay
pub const BATCH: usize = 2; // records per batch
pub const FIBER_WAIT: usize = 3; // a fiber's wait for its tail
pub const WAKE: usize = 4; // the covering publish -> the fiber running again
pub const REPLY_HOLD: usize = 5; // a reply held -> sent
pub const COLLECT_STEP: usize = 6; // one of collect's fiber sleeps, as slept
pub const COLLECT_DONE: usize = 7; // a collect that ended with no reply of its round pending
pub const COLLECT_AUTH: usize = 8; // a collect that ended on a majority
pub const COLLECT_DEADLINE: usize = 9; // a collect that ended at its deadline
pub const REPLY_WOKE: usize = 10; // replies that woke a waiting collect (value 0)
pub const REPLY_LATE: usize = 11; // replies with no collect waiting (1: woke the loop)
const NAMES: [&str; 12] = ["queue_wait_us", "flush_us", "batch_records", "fiber_wait_us", "wake_us",
                           "reply_hold_us", "collect_step_us", "collect_done_us", "collect_auth_us",
                           "collect_deadline_us", "reply_woke", "reply_late"];

struct Stats {
    t0: Instant,
    samples: Vec<Mutex<Vec<u32>>>,
}

fn stats() -> Option<&'static Stats> {
    static S: OnceLock<Option<Stats>> = OnceLock::new();
    S.get_or_init(|| {
        (std::env::var("MAKO_RAFT_DISK_STATS").as_deref() == Ok("1"))
            .then(|| Stats { t0: Instant::now(), samples: (0..NAMES.len()).map(|_| Mutex::new(Vec::new())).collect() })
    })
    .as_ref()
}

pub fn on() -> bool {
    stats().is_some()
}

/// Microseconds since the stats began (0 when off).
pub fn now_us() -> u64 {
    stats().map_or(0, |s| s.t0.elapsed().as_micros() as u64)
}

pub fn add(metric: usize, v: u64) {
    if let Some(s) = stats() {
        let mut g = s.samples[metric].lock().unwrap_or_else(|e| e.into_inner());
        if g.len() < 200_000 {
            g.push(v.min(u32::MAX as u64) as u32);
        }
    }
}

/// One line per metric: count, p50, p90, p99, mean.
pub fn report(who: &str) {
    let Some(s) = stats() else { return };
    for (i, name) in NAMES.iter().enumerate() {
        let mut v = s.samples[i].lock().unwrap_or_else(|e| e.into_inner()).clone();
        if v.is_empty() {
            continue;
        }
        v.sort_unstable();
        let p = |q: f64| v[((v.len() - 1) as f64 * q) as usize];
        let mean = v.iter().map(|&x| x as f64).sum::<f64>() / v.len() as f64;
        eprintln!("[DISK-STATS] {who} {name} n={} p50={} p90={} p99={} mean={mean:.0}", v.len(), p(0.5), p(0.9), p(0.99));
    }
}
