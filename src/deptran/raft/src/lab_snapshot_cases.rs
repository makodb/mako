// The snapshot, configuration and load families of the RaftLab suite, in Rust.
// Split out of lab_cases.rs because they are a different kind of test: they
// reach the snapshot manager and the state-machine callbacks rather than the
// replication protocol.
//
// WORK IN PROGRESS -- being ported family by family. `run` is the second half
// of RaftLabTest::Run.

#![cfg(feature = "raft_test")]

use crate::lab_cases::LabState;

pub fn run(_st: &mut LabState) -> i32 {
    eprintln!("[LAB-RUST] snapshot/config/load families not ported yet");
    0
}
