// [M0] The trace kit's hooks on the Rust lane (docs/verus/modification-plan.md
// §5 Phase 0; the kit itself is server.cc's raft_trace_* kernels).
//
// Two function-pointer slots, null by default. raft_lane_rust.cc's Serve
// fills them through raft_rt_install_trace only when MAKO_RAFT_TRACE_FILE is
// set, so an untraced run pays one relaxed load per hook and calls nothing.
// raft-rt cannot name the C++ kernels directly: its standalone cargo tests
// link no C++, and an extern reference there is an undefined symbol.

use std::sync::atomic::{AtomicUsize, Ordering};

type ThroughFn = extern "C" fn(i32, u64, u64);
type NowFn = extern "C" fn() -> u64;

static THROUGH: AtomicUsize = AtomicUsize::new(0);
static NOW: AtomicUsize = AtomicUsize::new(0);

/// Installed by raft_lane_rust.cc; see the module note.
#[no_mangle]
pub extern "C" fn raft_rt_install_trace(through: ThroughFn, now: NowFn) {
    THROUGH.store(through as usize, Ordering::Release);
    NOW.store(now as usize, Ordering::Release);
}

/// raft_trace_through: stamp `stage` for every index up to `through`.
pub fn through(stage: i32, through: u64, t_us: u64) {
    let f = THROUGH.load(Ordering::Relaxed);
    if f != 0 {
        // SAFETY: only raft_rt_install_trace stores here, and it stores a ThroughFn.
        let f: ThroughFn = unsafe { std::mem::transmute::<usize, ThroughFn>(f) };
        f(stage, through, t_us);
    }
}

/// raft_trace_now_us: the trace clock, 0 when tracing is off.
pub fn now_us() -> u64 {
    let f = NOW.load(Ordering::Relaxed);
    if f == 0 {
        return 0;
    }
    // SAFETY: only raft_rt_install_trace stores here, and it stores a NowFn.
    let f: NowFn = unsafe { std::mem::transmute::<usize, NowFn>(f) };
    f()
}
