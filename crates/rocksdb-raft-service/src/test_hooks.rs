//! Explicit test-profile controls; absent from production builds and wire schema.
use crate::wire::{Position, TestStage};
use parking_lot::{Condvar, Mutex};

#[derive(Default)]
struct State { paused: bool, waiting: usize, position: Option<Position> }
#[derive(Default)]
struct Gate { state: Mutex<State>, resumed: Condvar }
#[derive(Default)]
pub(crate) struct Hooks { gates: [Gate; 5] }
impl Hooks {
    fn gate(&self, stage: TestStage) -> &Gate {
        &self.gates[match stage { TestStage::BeforeProposal => 0, TestStage::AfterAppend => 1,
            TestStage::BeforeApply => 2, TestStage::AfterApply => 3, TestStage::BeforeReply => 4 }]
    }
    pub fn control(&self, stage: TestStage, paused: Option<bool>) -> (bool, usize, Option<Position>) {
        let gate = self.gate(stage);
        let mut state = gate.state.lock();
        if let Some(paused) = paused {
            state.paused = paused;
            if !paused { gate.resumed.notify_all(); }
        }
        (state.paused, state.waiting, state.position)
    }
    pub fn hit(&self, stage: TestStage, position: Option<Position>) {
        let gate = self.gate(stage);
        let mut state = gate.state.lock();
        if !state.paused { return; }
        state.waiting += 1;
        state.position = position;
        while state.paused { gate.resumed.wait(&mut state); }
        state.waiting -= 1;
    }
    pub fn resume_all(&self) {
        for gate in &self.gates {
            gate.state.lock().paused = false;
            gate.resumed.notify_all();
        }
    }
}
