//! Spin lock and atomic counter — the live part of
//! `src/rrr/base/threading.cpp`.
//!
//! The C++ file also wraps the `pthread_{spin,mutex,cond}_*` families
//! in verify-checked helpers; those have no consumers outside the rrr
//! benchmarks and are subsumed by `std::sync::{Mutex, Condvar}`, so
//! they are deliberately not ported (see the conversion ledger).
//!
//! `SpinLock` keeps the C++ backoff shape — bounded pause spinning,
//! then yielding to the scheduler — because it guards the same very
//! short critical sections. `std::hint::spin_loop` is the pause
//! instruction the C++ side spelled `cpu_pause()`.

use std::sync::atomic::{AtomicBool, AtomicI64, Ordering};

/// Spins before parking, for critical sections measured in nanoseconds.
///
/// Prefer `std::sync::Mutex` for anything that can block; this exists
/// for the paths where the C++ implementation deliberately burns CPU
/// rather than pay a futex round trip.
pub struct SpinLock {
    locked: AtomicBool,
}

/// Pause iterations before falling back to yielding the timeslice.
/// Matches the C++ side's bounded spin.
const SPIN_LIMIT: u32 = 1000;

impl SpinLock {
    pub fn new() -> SpinLock {
        SpinLock {
            locked: AtomicBool::new(false),
        }
    }

    /// One attempt; `true` if the lock is now held by this caller.
    pub fn try_lock(&self) -> bool {
        self.locked
            .compare_exchange(false, true, Ordering::Acquire, Ordering::Relaxed)
            .is_ok()
    }

    pub fn lock(&self) {
        let mut spins: u32 = 0;
        while !self.try_lock() {
            if spins < SPIN_LIMIT {
                std::hint::spin_loop();
                spins += 1;
            } else {
                // Held longer than a spin is worth — let the owner run.
                std::thread::yield_now();
                spins = 0;
            }
        }
    }

    /// # Correctness
    ///
    /// Only the holder may unlock. This is a raw lock (no guard type),
    /// matching the C++ call sites it replaces; the fiber runtime that
    /// consumes it holds and releases within one function body.
    pub fn unlock(&self) {
        self.locked.store(false, Ordering::Release);
    }

    pub fn is_locked(&self) -> bool {
        self.locked.load(Ordering::Relaxed)
    }
}

impl Default for SpinLock {
    fn default() -> SpinLock {
        SpinLock::new()
    }
}

/// Monotonic id source (the C++ `Counter`): xids, request ids, fiber
/// ids. Wrapping is not a concern — i64 at a billion ids/second lasts
/// ~292 years.
pub struct Counter {
    next: AtomicI64,
}

impl Counter {
    pub fn new(start: i64) -> Counter {
        Counter {
            next: AtomicI64::new(start),
        }
    }

    /// Value the next `next()` will hand out. Racy by nature — for
    /// diagnostics, not for allocation.
    pub fn peek_next(&self) -> i64 {
        self.next.load(Ordering::Relaxed)
    }

    /// Claim `step` ids and return the first. `AcqRel` matches the C++
    /// original: callers publish state alongside the id they took.
    pub fn next(&self, step: i64) -> i64 {
        self.next.fetch_add(step, Ordering::AcqRel)
    }

    /// Claim exactly one id.
    pub fn next_id(&self) -> i64 {
        self.next(1)
    }

    pub fn reset(&self, start: i64) {
        self.next.store(start, Ordering::Relaxed);
    }
}

impl Default for Counter {
    fn default() -> Counter {
        Counter::new(0)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::Arc;

    #[test]
    fn spinlock_excludes() {
        let lk = SpinLock::new();
        assert!(!lk.is_locked());
        assert!(lk.try_lock());
        assert!(lk.is_locked());
        assert!(!lk.try_lock(), "must not re-enter while held");
        lk.unlock();
        assert!(!lk.is_locked());
        lk.lock();
        assert!(lk.is_locked());
        lk.unlock();
    }

    #[test]
    fn spinlock_serializes_threads() {
        let lk = Arc::new(SpinLock::new());
        let hits = Arc::new(AtomicI64::new(0));
        let mut handles = Vec::new();
        for _ in 0..4 {
            let lk = Arc::clone(&lk);
            let hits = Arc::clone(&hits);
            handles.push(std::thread::spawn(move || {
                for _ in 0..2000 {
                    lk.lock();
                    // Non-atomic read-modify-write under the lock: a
                    // broken lock loses increments.
                    let v = hits.load(Ordering::Relaxed);
                    hits.store(v + 1, Ordering::Relaxed);
                    lk.unlock();
                }
            }));
        }
        for h in handles {
            h.join().unwrap();
        }
        assert_eq!(hits.load(Ordering::Relaxed), 8000);
    }

    #[test]
    fn counter_hands_out_unique_ids() {
        let c = Counter::new(5);
        assert_eq!(c.peek_next(), 5);
        assert_eq!(c.next_id(), 5);
        assert_eq!(c.next_id(), 6);
        assert_eq!(c.next(10), 7);
        assert_eq!(c.peek_next(), 17);
        c.reset(0);
        assert_eq!(c.next_id(), 0);
    }

    #[test]
    fn counter_is_unique_across_threads() {
        let c = Arc::new(Counter::new(0));
        let mut handles = Vec::new();
        for _ in 0..4 {
            let c = Arc::clone(&c);
            handles.push(std::thread::spawn(move || {
                let mut ids = Vec::new();
                for _ in 0..1000 {
                    ids.push(c.next_id());
                }
                ids
            }));
        }
        let mut all = Vec::new();
        for h in handles {
            all.extend(h.join().unwrap());
        }
        all.sort_unstable();
        let before = all.len();
        all.dedup();
        assert_eq!(all.len(), before, "ids must not repeat");
        assert_eq!(before, 4000);
    }
}
