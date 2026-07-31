//! Stackful fibers: their own stacks, and a real context switch.
//!
//! ## The context switch lives in assembly, shared by both toolchains
//!
//! `fiber_x86_64.S` is the single source of truth. Rust assembles it via
//! `global_asm!(include_str!(…))`; the C++ build assembles the same
//! file. Neither side keeps a copy, so there is nothing to drift — and
//! `global_asm!` dropping during transpilation is *correct* rather than
//! a hazard, because the C++ build supplies the symbol itself. The seam
//! between them is one `extern "C"` declaration, which
//! `probes/s8seam/` proved survives clang's module purview and, more
//! importantly, fails LOUDLY when the assembly is absent.
//!
//! ## Why the seam takes raw pointers
//!
//! Rust coerces `&mut T` to `*mut T` implicitly at an FFI call. C++ has
//! no `T&` → `T*` conversion, so a `&mut` wrapper transpiles to a call
//! that does not compile. Raw pointers at an `extern "C"` boundary are
//! idiomatic anyway.
//!
//! ## A fiber is boxed BEFORE it starts
//!
//! The trampoline finds its fiber through a thread-local, so the fiber
//! must already be at its final address when the first switch happens —
//! moving it afterwards would leave the trampoline pointing at the old
//! one. Hence box-then-start rather than construct-and-return.
//!
//! ## Known Goal-2 gap
//!
//! `CURRENT` uses `thread_local!`, which is correct under rustc and does
//! NOT survive transpilation (the static vanishes while its accessors
//! still name it — a hard C++ compile error, so at least it is loud).
//! The `#[thread_local]` ATTRIBUTE does lower correctly as of rusty-cpp
//! `608a6d77`, but it requires nightly, which Goal 1 does not accept.
//! Closing this needs either a `thread_local!` lowering upstream or the
//! same `extern "C"` treatment the context switch gets. Recorded rather
//! than worked around.

#![allow(unsafe_code)]

pub mod runtime;
pub use runtime::{FiberHandle, FiberRuntime, ReadyQueue};

use crate::sys;
use std::cell::Cell;

/// Saved machine state for one fiber: just the stack pointer, because
/// the assembly pushes the callee-saved registers onto the stack it is
/// leaving. `#[repr(C)]` because the assembly indexes this.
#[repr(C)]
pub struct FiberContext {
    pub sp: u64,
}

// The layout guard between this struct and the assembly that indexes
// it. Emits as a real `static_assert` in the translated C++ (rusty-cpp
// 3bf2f547); before that it silently vanished.
const _: () = assert!(core::mem::size_of::<FiberContext>() == 8);

impl FiberContext {
    pub fn zeroed() -> FiberContext {
        FiberContext { sp: 0 }
    }
}

// Rust's inline asm defaults to INTEL syntax; the shared file is AT&T,
// which is what the C++ toolchain assembles.
core::arch::global_asm!(include_str!("fiber_x86_64.S"), options(att_syntax));

extern "C" {
    /// Save the current machine state into `from` and resume `to`.
    /// Returns when something switches back.
    fn srpc_fiber_swap(from: *mut FiberContext, to: *mut FiberContext);
}

/// Stack size per fiber. 1 MiB, matching the C++ exactly — a
/// semantics-preserving port keeps the quirk rather than tuning it.
pub const STACK_BYTES: usize = 1024 * 1024;
/// One inaccessible page below the stack, so an overflow faults at a
/// known address instead of corrupting whatever was mapped below.
pub const GUARD_BYTES: usize = 4096;

/// Where a fiber is in its life.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum FiberState {
    /// Created, never resumed.
    Ready,
    /// Currently executing.
    Running,
    /// Yielded, waiting to be resumed.
    Suspended,
    /// Body returned; the stack may be reclaimed.
    Finished,
}

thread_local! {
    /// The fiber currently executing on this thread, as a raw pointer.
    ///
    /// A pointer rather than a reference because the fiber owns the
    /// stack the accessor is running on — no safe borrow can describe
    /// that. `None` means "on the thread's own stack".
    static CURRENT: Cell<*mut Fiber> = const { Cell::new(core::ptr::null_mut()) };
    /// Per-thread id counter. The C++ counter is also per-thread, so ids
    /// are NOT globally unique; preserved deliberately.
    static NEXT_ID: Cell<u64> = const { Cell::new(1) };
}

/// A fiber: its own stack, its saved context, and the body to run.
pub struct Fiber {
    id: u64,
    context: FiberContext,
    /// Where control returns when this fiber yields.
    caller: FiberContext,
    /// Mapping base, INCLUDING the guard page.
    map_base: usize,
    map_len: usize,
    state: Cell<FiberState>,
    body: Option<Box<dyn FnOnce()>>,
}

impl Fiber {
    /// Create a fiber and place it at its final address.
    ///
    /// Returns a `Box` and never a bare `Fiber`: the trampoline resolves
    /// its fiber through [`CURRENT`], so the address must be stable
    /// before the first switch.
    pub fn new(body: Box<dyn FnOnce()>) -> Option<Box<Fiber>> {
        let map_len = STACK_BYTES + GUARD_BYTES;
        let base = sys::map_anonymous(map_len);
        if base == sys::MAP_FAILED || base == 0 {
            return None;
        }
        // The guard goes at the LOW end: the stack grows down, so that
        // is the end it runs off.
        if sys::protect_none(base, GUARD_BYTES) < 0 {
            sys::unmap(base, map_len);
            return None;
        }

        let id = NEXT_ID.with(|n| {
            let v = n.get();
            n.set(v.wrapping_add(1));
            v
        });

        let mut f = Box::new(Fiber {
            id,
            context: FiberContext::zeroed(),
            caller: FiberContext::zeroed(),
            map_base: base,
            map_len,
            state: Cell::new(FiberState::Ready),
            body: Some(body),
        });
        f.prepare_stack();
        Some(f)
    }

    /// Lay out the initial stack so the first switch lands in
    /// [`trampoline`].
    ///
    /// The frame the assembly expects, from the top down: a return
    /// address, then the six callee-saved registers it will pop.
    fn prepare_stack(&mut self) {
        let top = self.map_base + self.map_len;
        let aligned = top & !0xF;
        // ALIGNMENT IS LOAD-BEARING. System V AMD64 requires
        // `rsp % 16 == 8` on entry to a function — that is what a `call`
        // leaves, having pushed its 8-byte return address onto a
        // 16-aligned stack. The assembly reaches the trampoline via
        // `ret`, so the return-address slot must sit at a 16-aligned
        // address for `rsp` to be 8 (mod 16) once `ret` pops it.
        //
        // Getting this wrong does not fail at the switch. It fails later
        // and elsewhere, inside whatever first executes a 16-byte-aligned
        // SSE move against a stack slot — a SIGSEGV with a backtrace
        // pointing nowhere near this function.
        // @unsafe { laying out a machine stack frame by hand }
        unsafe {
            let mut sp = aligned as *mut u64;
            sp = sp.sub(2); // 16-aligned slot for the return address
            *sp = (trampoline as *const ()) as usize as u64;
            sp = sp.sub(6); // the six callee-saved registers it pops
            for i in 0..6 {
                *sp.add(i) = 0;
            }
            self.context.sp = sp as u64;
        }
    }

    pub fn id(&self) -> u64 {
        self.id
    }

    pub fn state(&self) -> FiberState {
        self.state.get()
    }

    pub fn is_finished(&self) -> bool {
        self.state.get() == FiberState::Finished
    }

    /// Switch to this fiber. Returns when it yields or finishes.
    ///
    /// A no-op if it has already finished, so a stale resume is
    /// harmless rather than a jump into a reclaimed stack.
    pub fn resume(self: &mut Box<Fiber>) {
        if self.state.get() == FiberState::Finished {
            return;
        }
        let prev = CURRENT.with(|c| c.replace(&mut **self as *mut Fiber));
        self.state.set(FiberState::Running);
        // @unsafe { the context switch }
        unsafe {
            srpc_fiber_swap(&mut self.caller, &mut self.context);
        }
        CURRENT.with(|c| c.set(prev));
    }

    /// Switch back to whoever resumed this fiber.
    fn switch_out(&mut self, next: FiberState) {
        self.state.set(next);
        // @unsafe { the context switch }
        unsafe {
            srpc_fiber_swap(&mut self.context, &mut self.caller);
        }
    }
}

impl Drop for Fiber {
    fn drop(&mut self) {
        if self.map_base != 0 {
            sys::unmap(self.map_base, self.map_len);
            self.map_base = 0;
        }
    }
}

/// Where every fiber begins. Reached by `ret`, so it takes no arguments
/// and must never return — the assembly left no caller to return to.
extern "C" fn trampoline() -> ! {
    let fiber = CURRENT.with(|c| c.get());
    assert!(!fiber.is_null(), "trampoline entered with no current fiber");
    // @unsafe { resolved through the thread-local the resume just set }
    let f = unsafe { &mut *fiber };

    if let Some(body) = f.body.take() {
        // Catch inside the body so an unwind never crosses a context
        // switch — unwinding across the swap would tear down a stack the
        // unwinder knows nothing about.
        let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(body));
    }

    // Finished: hand control back for good. `switch_out` does not
    // return here, because nothing will resume a Finished fiber.
    f.switch_out(FiberState::Finished);
    unreachable!("a finished fiber was resumed");
}

/// Yield from the current fiber. Panics if called off a fiber, which is
/// a programming error rather than a runtime condition.
pub fn yield_now() {
    let fiber = CURRENT.with(|c| c.get());
    assert!(!fiber.is_null(), "yield_now() called outside a fiber");
    // @unsafe { the current fiber is live for the duration of this call }
    let f = unsafe { &mut *fiber };
    f.switch_out(FiberState::Suspended);
}

/// The id of the fiber running on this thread, or `None` on the
/// thread's own stack.
pub fn current_id() -> Option<u64> {
    let p = CURRENT.with(|c| c.get());
    if p.is_null() {
        return None;
    }
    // @unsafe { non-null means a live fiber resumed on this thread }
    Some(unsafe { (*p).id })
}

/// Whether this thread is currently running a fiber.
pub fn on_fiber() -> bool {
    !CURRENT.with(|c| c.get()).is_null()
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::rc::Rc;

    #[test]
    fn a_fiber_runs_its_body_on_its_own_stack() {
        let hit = Rc::new(Cell::new(0u32));
        let seen = Rc::clone(&hit);
        let mut f = Fiber::new(Box::new(move || {
            seen.set(1);
        }))
        .expect("mmap");
        assert_eq!(f.state(), FiberState::Ready);
        f.resume();
        assert_eq!(hit.get(), 1, "body did not run");
        assert!(f.is_finished());
    }

    #[test]
    fn yield_and_resume_alternate_in_order() {
        let log = Rc::new(std::cell::RefCell::new(Vec::new()));
        let inner = Rc::clone(&log);
        let mut f = Fiber::new(Box::new(move || {
            inner.borrow_mut().push("a");
            yield_now();
            inner.borrow_mut().push("b");
            yield_now();
            inner.borrow_mut().push("c");
        }))
        .expect("mmap");

        log.borrow_mut().push("start");
        f.resume();
        assert_eq!(f.state(), FiberState::Suspended);
        log.borrow_mut().push("mid");
        f.resume();
        log.borrow_mut().push("mid2");
        f.resume();
        assert!(f.is_finished());
        assert_eq!(*log.borrow(), vec!["start", "a", "mid", "b", "mid2", "c"]);
    }

    #[test]
    fn current_id_is_none_off_a_fiber_and_set_on_one() {
        assert!(!on_fiber());
        assert_eq!(current_id(), None);

        let got = Rc::new(Cell::new(None));
        let sink = Rc::clone(&got);
        let mut f = Fiber::new(Box::new(move || {
            sink.set(current_id());
        }))
        .expect("mmap");
        let id = f.id();
        f.resume();
        assert_eq!(got.get(), Some(id));

        // And restored afterwards — a resume must not leak its fiber
        // into the resuming context.
        assert!(!on_fiber(), "CURRENT leaked after the fiber finished");
    }

    #[test]
    fn fibers_nest() {
        // The inner fiber must not clobber the outer one's CURRENT.
        let order = Rc::new(std::cell::RefCell::new(Vec::new()));
        let outer_log = Rc::clone(&order);

        let mut outer = Fiber::new(Box::new(move || {
            let before = current_id().expect("outer id");
            let inner_log = Rc::clone(&outer_log);
            let mut inner = Fiber::new(Box::new(move || {
                inner_log
                    .borrow_mut()
                    .push(format!("inner={}", current_id().unwrap()));
            }))
            .expect("mmap");
            inner.resume();
            let after = current_id().expect("outer id restored");
            outer_log
                .borrow_mut()
                .push(format!("outer={before} restored={after}"));
            assert_eq!(before, after, "nested resume clobbered CURRENT");
        }))
        .expect("mmap");
        outer.resume();
        assert!(outer.is_finished());
        assert_eq!(order.borrow().len(), 2);
    }

    #[test]
    fn resuming_a_finished_fiber_is_a_no_op() {
        let n = Rc::new(Cell::new(0u32));
        let seen = Rc::clone(&n);
        let mut f = Fiber::new(Box::new(move || {
            seen.set(seen.get() + 1);
        }))
        .expect("mmap");
        f.resume();
        f.resume();
        f.resume();
        assert_eq!(n.get(), 1, "a finished fiber must not re-run");
    }

    #[test]
    fn a_panicking_body_finishes_the_fiber_without_unwinding_past_the_switch() {
        // Unwinding across a context switch would tear down a stack the
        // unwinder has never seen. The catch lives inside the body.
        let prev = std::panic::take_hook();
        std::panic::set_hook(Box::new(|_| {}));
        let mut f = Fiber::new(Box::new(|| {
            panic!("inside the fiber");
        }))
        .expect("mmap");
        f.resume();
        std::panic::set_hook(prev);
        assert!(f.is_finished(), "a panicking fiber must still finish");
        assert!(!on_fiber(), "CURRENT leaked after a panicking fiber");
    }

    #[test]
    fn many_fibers_each_get_a_distinct_stack() {
        let mut fibers = Vec::new();
        let bases = Rc::new(std::cell::RefCell::new(Vec::new()));
        for _ in 0..32 {
            let sink = Rc::clone(&bases);
            let mut f = Fiber::new(Box::new(move || {
                let x = 0u64;
                sink.borrow_mut().push(&x as *const u64 as usize);
                yield_now();
            }))
            .expect("mmap");
            f.resume();
            fibers.push(f);
        }
        let b = bases.borrow();
        assert_eq!(b.len(), 32);
        let mut sorted = b.clone();
        sorted.sort_unstable();
        sorted.dedup();
        assert_eq!(sorted.len(), 32, "two fibers shared a stack");
    }
}

#[cfg(test)]
mod guard_page_tests {
    use super::*;

    /// The guard page is claimed by every stackful runtime and verified
    /// by few. `mprotect` returning 0 only says the call succeeded; this
    /// reads back what the kernel actually recorded.
    #[test]
    fn the_guard_page_is_really_inaccessible() {
        let f = Fiber::new(Box::new(|| {})).expect("mmap");
        let base = f.map_base;

        let maps = std::fs::read_to_string("/proc/self/maps").expect("read maps");
        let mut guard_perms = None;
        let mut stack_perms = None;
        for line in maps.lines() {
            let Some((range, rest)) = line.split_once(' ') else {
                continue;
            };
            let Some((lo, hi)) = range.split_once('-') else {
                continue;
            };
            let (Ok(lo), Ok(hi)) = (usize::from_str_radix(lo, 16), usize::from_str_radix(hi, 16))
            else {
                continue;
            };
            let perms = rest.split(' ').next().unwrap_or("");
            if lo <= base && base < hi {
                guard_perms = Some(perms.to_string());
            }
            // A byte inside the usable stack, above the guard.
            let probe = base + GUARD_BYTES + 16;
            if lo <= probe && probe < hi {
                stack_perms = Some(perms.to_string());
            }
        }

        let guard = guard_perms.expect("guard page should appear in /proc/self/maps");
        let stack = stack_perms.expect("stack should appear in /proc/self/maps");
        assert!(
            guard.starts_with("---"),
            "guard page is accessible ({guard}) — an overflow would corrupt memory silently"
        );
        assert!(
            stack.starts_with("rw"),
            "stack should be readable/writable, got {stack}"
        );
    }
}
