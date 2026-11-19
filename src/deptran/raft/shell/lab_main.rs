// The RaftLab suite's driver and its C entry point. A module of its own,
// above lab, lab_cases and lab_snapshot_cases, so every edge between the lab
// modules points one way.

use crate::lab::{reset, rpc_total, set_learner_action, shutdown, start_netctl, NSERVERS};
use crate::lab_cases::{run_basic, LabState};
use crate::lab_snapshot_cases::run_snapshot;
use crate::server_h::lab_count;

// Same order and same short-circuit structure as RaftLabTest::Run, so a
// failure stops at the same place.
pub fn run() -> i32 {
    eprintln!("Starting Raft lab tests (Rust harness)");
    let st = LabState { index: 1, init_rpcs: 0 };
    let start_rpc = rpc_total();

    assert_eq!(lab_count(), NSERVERS,
               "the Rust harness needs all five replicas registered");

    let passed = match run_basic(st) {
        Some(st) => run_snapshot(st) == 0,
        None => false,
    };
    if !passed {
        eprintln!("TESTS FAILED");
        return 1;
    }

    eprintln!("ALL TESTS PASSED");
    eprintln!("Total RPC count: {}", rpc_total() - start_rpc);
    0
}

// ---------------------------------------------------------------------------
// The entry point the C++ lab fiber calls after its own suite has finished.

/// Runs the Rust suite. 0 on success, 1 on failure -- the same verdict shape
/// RaftLabTest::Run returns, so frame.cc can require both.
#[unsafe(no_mangle)]
pub extern "C" fn raft_lab_rust_run() -> i32 {
    reset();
    set_learner_action();
    start_netctl();
    let verdict = run();
    shutdown();
    verdict
}
