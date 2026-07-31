//! Fibers driven by the poll thread: a slab, a ready queue, and the
//! rule that makes the whole thing safe.
//!
//! ## Waking never switches stacks
//!
//! [`FiberHandle::wake`] pushes an id onto a queue. Nothing else. The
//! context switch happens later, in [`FiberRuntime::run_ready`], which
//! the poll loop calls as its own phase after the command drain.
//!
//! Resuming inline from the waker would be the natural shape and it is
//! wrong. Replies are delivered from inside the frame callback, which
//! runs while the connection holds its reader lock and while the decode
//! loop is mid-buffer. Switching stacks there means a fiber can suspend
//! *holding that lock*, with the poll thread's own frames stranded
//! underneath it — and the connection wedges with no diagnostic. The
//! C++ has the same split for the same reason: `int_event_set` only
//! flips WAIT→READY, and `continue_fiber` runs later from the reactor
//! loop.
//!
//! ## The slab is a Vec, deliberately
//!
//! Not a `HashMap`. This tree has recorded clang-22 defects in the
//! hashbrown port — a compile-time mangler crash and a runtime resize
//! null-deref — so new code on a hot path that must also transpile does
//! not bet on it. A `Vec` indexed by slot is cheaper anyway.
//!
//! ## Ownership
//!
//! Fibers own machine stacks and are not `Send`; the slab therefore
//! lives on the poll thread and never moves. Only the ready queue is
//! shared, and it carries `u64` slots rather than pointers, so a wake
//! for a fiber that has already finished resolves to "no such slot"
//! instead of a dangling handle — the same reason the pollable map is
//! keyed by fd.

use crate::runtime::fiber::Fiber;
use std::sync::{Arc, Mutex};

/// Slots woken since the last sweep. Shared across threads; the fibers
/// themselves are not.
#[derive(Default)]
pub struct ReadyQueue {
    slots: Mutex<Vec<u64>>,
}

impl ReadyQueue {
    pub fn new() -> Arc<ReadyQueue> {
        Arc::new(ReadyQueue::default())
    }

    /// Mark a fiber runnable. Safe from any thread, and cheap — it takes
    /// a lock and pushes a `u64`.
    pub fn wake(&self, slot: u64) {
        self.slots.lock().unwrap().push(slot);
    }

    fn drain(&self) -> Vec<u64> {
        let mut g = self.slots.lock().unwrap();
        std::mem::take(&mut *g)
    }

    pub fn pending(&self) -> usize {
        self.slots.lock().unwrap().len()
    }
}

/// A wakeable reference to a suspended fiber.
///
/// Holds a slot, not a pointer: cloneable, `Send`, and harmless once the
/// fiber it names has finished.
#[derive(Clone)]
pub struct FiberHandle {
    slot: u64,
    ready: Arc<ReadyQueue>,
}

impl FiberHandle {
    pub fn slot(&self) -> u64 {
        self.slot
    }

    /// Queue this fiber to run on the next poll-loop sweep.
    pub fn wake(&self) {
        self.ready.wake(self.slot);
    }
}

/// The fibers belonging to one poll thread.
pub struct FiberRuntime {
    /// Indexed by slot. `None` is a free slot, reused by the next spawn.
    slab: Vec<Option<Box<Fiber>>>,
    free: Vec<u64>,
    ready: Arc<ReadyQueue>,
}

impl FiberRuntime {
    pub fn new() -> FiberRuntime {
        FiberRuntime {
            slab: Vec::new(),
            free: Vec::new(),
            ready: ReadyQueue::new(),
        }
    }

    /// The queue wakers push to. Clone it to hand out wake rights
    /// without handing out the fibers.
    pub fn ready_queue(&self) -> Arc<ReadyQueue> {
        Arc::clone(&self.ready)
    }

    /// Create a fiber and run it until its first suspend.
    ///
    /// Started eagerly, matching the C++: `Fiber::create_run` runs the
    /// body on the creating stack until it blocks. A handler that never
    /// blocks therefore costs no queueing at all.
    pub fn spawn(&mut self, body: Box<dyn FnOnce()>) -> Option<FiberHandle> {
        let mut fiber = Fiber::new(body)?;
        let slot = match self.free.pop() {
            Some(s) => s,
            None => {
                self.slab.push(None);
                (self.slab.len() - 1) as u64
            }
        };
        fiber.resume();
        let handle = FiberHandle {
            slot,
            ready: Arc::clone(&self.ready),
        };
        if fiber.is_finished() {
            // Never suspended: reclaim immediately rather than holding a
            // stack for a fiber that is already done.
            self.free.push(slot);
        } else {
            self.slab[slot as usize] = Some(fiber);
        }
        Some(handle)
    }

    /// Resume every fiber woken since the last sweep. Called by the poll
    /// loop; this is where stacks actually switch.
    ///
    /// Returns how many were resumed, which the loop uses for nothing
    /// but tests use to prove the deferral.
    pub fn run_ready(&mut self) -> usize {
        let mut ran = 0usize;
        // Re-drain: a resumed fiber may wake another (or itself) and
        // that should run in the same sweep, as the C++ reactor loop
        // does. Bounded so a fiber that immediately re-wakes itself
        // cannot starve the poll loop of its next epoll_wait.
        for _ in 0..16 {
            let slots = self.ready.drain();
            if slots.is_empty() {
                break;
            }
            for slot in slots {
                let idx = slot as usize;
                if idx >= self.slab.len() {
                    continue;
                }
                // Take the fiber out for the duration of the resume, so
                // a wake delivered from inside it cannot re-enter this
                // slot.
                let Some(mut fiber) = self.slab[idx].take() else {
                    continue; // already finished, or a duplicate wake
                };
                fiber.resume();
                ran += 1;
                if fiber.is_finished() {
                    self.free.push(slot);
                } else {
                    self.slab[idx] = Some(fiber);
                }
            }
        }
        ran
    }

    /// Fibers currently suspended.
    pub fn live(&self) -> usize {
        self.slab.iter().filter(|s| s.is_some()).count()
    }
}

impl Default for FiberRuntime {
    fn default() -> FiberRuntime {
        FiberRuntime::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::runtime::fiber::yield_now;
    use std::cell::RefCell;
    use std::rc::Rc;
    use std::sync::atomic::{AtomicBool, Ordering};

    #[test]
    fn a_non_blocking_fiber_finishes_in_spawn_and_frees_its_slot() {
        let mut rt = FiberRuntime::new();
        let hit = Rc::new(RefCell::new(0));
        let seen = Rc::clone(&hit);
        rt.spawn(Box::new(move || {
            *seen.borrow_mut() += 1;
        }))
        .expect("spawn");
        assert_eq!(*hit.borrow(), 1);
        assert_eq!(rt.live(), 0, "a finished fiber must not hold a stack");
    }

    #[test]
    fn waking_does_not_resume_inline() {
        // THE property of this stage. `wake()` must only queue; the
        // switch belongs to the poll loop. If this regresses, a reply
        // callback can suspend a fiber while holding the connection's
        // reader lock.
        let mut rt = FiberRuntime::new();
        let progressed = Rc::new(RefCell::new(Vec::new()));
        let log = Rc::clone(&progressed);
        let h = rt
            .spawn(Box::new(move || {
                log.borrow_mut().push("before");
                yield_now();
                log.borrow_mut().push("after");
            }))
            .expect("spawn");

        assert_eq!(*progressed.borrow(), vec!["before"]);
        h.wake();
        assert_eq!(
            *progressed.borrow(),
            vec!["before"],
            "wake() resumed the fiber inline — the strand hazard is back"
        );
        assert_eq!(rt.run_ready(), 1);
        assert_eq!(*progressed.borrow(), vec!["before", "after"]);
        assert_eq!(rt.live(), 0);
    }

    #[test]
    fn a_wake_from_another_thread_is_honoured_by_the_sweep() {
        let mut rt = FiberRuntime::new();
        let done = Rc::new(RefCell::new(false));
        let flag = Rc::clone(&done);
        let h = rt
            .spawn(Box::new(move || {
                yield_now();
                *flag.borrow_mut() = true;
            }))
            .expect("spawn");

        let woken = Arc::new(AtomicBool::new(false));
        let signal = Arc::clone(&woken);
        let h2 = h.clone();
        std::thread::spawn(move || {
            h2.wake();
            signal.store(true, Ordering::SeqCst);
        })
        .join()
        .expect("waker thread");
        assert!(woken.load(Ordering::SeqCst));

        assert!(!*done.borrow(), "must not have resumed off the poll thread");
        rt.run_ready();
        assert!(*done.borrow());
    }

    #[test]
    fn a_duplicate_wake_resumes_once() {
        let mut rt = FiberRuntime::new();
        let count = Rc::new(RefCell::new(0));
        let n = Rc::clone(&count);
        let h = rt
            .spawn(Box::new(move || {
                yield_now();
                *n.borrow_mut() += 1;
            }))
            .expect("spawn");
        h.wake();
        h.wake();
        h.wake();
        rt.run_ready();
        assert_eq!(*count.borrow(), 1, "duplicate wakes must not double-resume");
    }

    #[test]
    fn a_wake_for_a_finished_fiber_is_harmless() {
        let mut rt = FiberRuntime::new();
        let h = rt.spawn(Box::new(|| {})).expect("spawn");
        h.wake();
        assert_eq!(rt.run_ready(), 0, "nothing to resume");
        h.wake();
        assert_eq!(rt.run_ready(), 0);
    }

    #[test]
    fn a_fiber_woken_during_the_sweep_runs_in_the_same_sweep() {
        // Mirrors the C++ reactor loop, which re-tests its queues rather
        // than deferring a same-iteration wake to the next epoll tick.
        let mut rt = FiberRuntime::new();
        let order = Rc::new(RefCell::new(Vec::new()));

        let log_b = Rc::clone(&order);
        let hb = rt
            .spawn(Box::new(move || {
                yield_now();
                log_b.borrow_mut().push("b");
            }))
            .expect("spawn b");

        let log_a = Rc::clone(&order);
        let hb2 = hb.clone();
        let ha = rt
            .spawn(Box::new(move || {
                yield_now();
                log_a.borrow_mut().push("a");
                hb2.wake(); // wakes b from inside the sweep
            }))
            .expect("spawn a");

        ha.wake();
        rt.run_ready();
        assert_eq!(*order.borrow(), vec!["a", "b"]);
    }

    #[test]
    fn slots_are_reused() {
        let mut rt = FiberRuntime::new();
        let first = rt.spawn(Box::new(|| {})).expect("spawn").slot();
        let second = rt.spawn(Box::new(|| {})).expect("spawn").slot();
        assert_eq!(
            first, second,
            "a freed slot should be reused rather than growing the slab"
        );
    }
}
