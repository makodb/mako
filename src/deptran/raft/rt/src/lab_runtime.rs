// The RaftLab harness on the Rust lane.
//
// In the C++ lane the harness fiber is a C++ srpc fiber that site 0's
// ServerWorker creates (raft/frame.cc CreateCommo) and runs on its thread's
// C++ reactor. Here it is a Rust srpc fiber on the calling thread's Rust
// reactor, because the harness sleeps through raft_fiber_sleep_us, which on
// this lane is the Rust Fiber::sleep and needs a Rust reactor to yield to.

use std::cell::Cell;
use std::rc::Rc;

use srpc::reactor::{Fiber, Reactor};

/// Run the 25 lab cases on a fiber of this thread's Rust reactor and return
/// the verdict, 0 for success -- the value frame.cc's lab fiber stores.
#[no_mangle]
pub extern "C" fn raft_rt_run_lab() -> i32 {
    let verdict = Rc::new(Cell::new(-1));
    let out = verdict.clone();
    Fiber::create_run(move || {
        // The harness itself, in the core (src/deptran/raft/src/lab.rs).
        out.set(raft::lab::raft_lab_rust_run());
        Reactor::get_reactor().looping_.set(false);
    });
    // create_run ran the fiber until its first suspension; if the harness
    // already finished (it never does -- it sleeps), the loop exits at once.
    if verdict.get() == -1 {
        Reactor::get_reactor().run_loop(true, true);
    }
    verdict.get()
}
