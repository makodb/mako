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
use std::cell::RefCell;
use std::sync::{Arc, Mutex};

thread_local! {
    /// The runtime carried by THIS thread, installed by the poll loop.
    ///
    /// Thread-local for the same reason the C++ reactor is
    /// (`sp_reactor_th_`): a request handler needs to spawn a fiber from
    /// deep inside a frame callback, which has no path to a value the
    /// poll loop holds on its stack. Posting a command per request
    /// instead would add a channel round trip to the hot path and change
    /// what is being measured.
    ///
    /// Same Goal-2 gap as `CURRENT` — see the module docs on `fiber`.
    static LOCAL: RefCell<Option<FiberRuntime>> = const { RefCell::new(None) };
}

/// Install a runtime on this thread for the duration of `f`.
pub fn with_runtime_installed<R>(f: impl FnOnce() -> R) -> R {
    LOCAL.with(|l| *l.borrow_mut() = Some(FiberRuntime::new()));
    let r = f();
    LOCAL.with(|l| *l.borrow_mut() = None);
    r
}

/// Spawn a fiber on this thread's runtime. `None` if the thread carries
/// no runtime, or if the stack could not be mapped.
pub fn spawn_here(body: Box<dyn FnOnce()>) -> Option<FiberHandle> {
    LOCAL.with(|l| {
        let mut g = l.borrow_mut();
        let rt = g.as_mut()?;
        rt.spawn(body)
    })
}

/// Run this thread's ready fibers. Returns 0 if it carries no runtime.
pub fn run_ready_here() -> usize {
    LOCAL.with(|l| {
        let mut g = l.borrow_mut();
        match g.as_mut() {
            Some(rt) => rt.run_ready(),
            None => 0,
        }
    })
}

/// This thread's ready queue, for handing wake rights to other threads.
pub fn ready_queue_here() -> Option<Arc<ReadyQueue>> {
    LOCAL.with(|l| l.borrow().as_ref().map(|rt| rt.ready_queue()))
}

/// Fibers currently suspended on this thread.
pub fn live_here() -> usize {
    LOCAL.with(|l| l.borrow().as_ref().map(|rt| rt.live()).unwrap_or(0))
}

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
    ///
    /// `Box<Fiber>`, and clippy'''s `vec_box` suggestion to drop it is
    /// WRONG here: the trampoline resolves its fiber through a
    /// thread-local pointer, so a fiber'''s address must never change.
    /// A `Vec<Fiber>` moves its elements on reallocation, which would
    /// leave every suspended fiber'''s trampoline pointing at freed
    /// memory.
    #[allow(clippy::vec_box)]
    /// Finished fibers, kept for their STACKS. Reusing one costs a
    /// pointer pop; creating one costs mmap + mprotect, and dropping it
    /// costs munmap. See `Fiber::reset`.
    pool: Vec<Box<Fiber>>,
    ready: Arc<ReadyQueue>,
}

impl FiberRuntime {
    pub fn new() -> FiberRuntime {
        FiberRuntime {
            slab: Vec::new(),
            free: Vec::new(),
            pool: Vec::new(),
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
        // Recycle before mapping. This is the difference between
        // measuring fibers and measuring mmap.
        let mut fiber = match self.pool.pop() {
            Some(mut f) => {
                f.reset(body);
                f
            }
            None => Fiber::new(body)?,
        };
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
            self.recycle(fiber);
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
                    self.recycle(fiber);
                } else {
                    self.slab[idx] = Some(fiber);
                }
            }
        }
        ran
    }

    /// Bound on retained stacks. Each is 1 MiB of address space (mostly
    /// untouched pages), so the cap trades idle RSS against remapping
    /// under a bursty load.
    const POOL_CAP: usize = 256;

    fn recycle(&mut self, fiber: Box<Fiber>) {
        if self.pool.len() < Self::POOL_CAP {
            self.pool.push(fiber);
        }
        // Over the cap: dropping unmaps, which is the point of a cap.
    }

    /// Stacks currently held for reuse.
    pub fn pooled(&self) -> usize {
        self.pool.len()
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

#[cfg(test)]
mod recycling_tests {
    use super::*;

    /// Recycling proven by ADDRESS, not by a counter.
    ///
    /// Without this, every spawn costs mmap + mprotect and every finish
    /// costs munmap — which measured 9% of inline dispatch on the real
    /// benchmark. A test that only counted spawns would have passed
    /// throughout.
    #[test]
    fn a_finished_fibers_stack_is_reused_rather_than_remapped() {
        let mut rt = FiberRuntime::new();
        let mut bases = Vec::new();
        for _ in 0..64 {
            let seen = std::rc::Rc::new(std::cell::Cell::new(0usize));
            let sink = std::rc::Rc::clone(&seen);
            rt.spawn(Box::new(move || {
                let probe = 0u64;
                sink.set(&probe as *const u64 as usize);
            }))
            .expect("spawn");
            bases.push(seen.get());
        }
        let mut distinct = bases.clone();
        distinct.sort_unstable();
        distinct.dedup();
        assert_eq!(
            distinct.len(),
            1,
            "64 sequential fibers used {} distinct stacks — recycling is broken",
            distinct.len()
        );
        assert_eq!(rt.pooled(), 1, "the one stack should be pooled");
    }

    #[test]
    fn concurrently_live_fibers_still_get_distinct_stacks() {
        // Recycling must not hand the same stack to two live fibers.
        let mut rt = FiberRuntime::new();
        let bases = std::rc::Rc::new(std::cell::RefCell::new(Vec::new()));
        let mut handles = Vec::new();
        for _ in 0..16 {
            let sink = std::rc::Rc::clone(&bases);
            handles.push(
                rt.spawn(Box::new(move || {
                    let probe = 0u64;
                    sink.borrow_mut().push(&probe as *const u64 as usize);
                    crate::runtime::fiber::yield_now();
                }))
                .expect("spawn"),
            );
        }
        let b = bases.borrow().clone();
        let mut distinct = b.clone();
        distinct.sort_unstable();
        distinct.dedup();
        assert_eq!(distinct.len(), 16, "live fibers shared a stack");
    }
}
