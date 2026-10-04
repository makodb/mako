// What a core call asks the shell to do: actions, in push order.
//
// [move, M1] Moved verbatim from src/deptran/raft/src/server_h.rs (Phase 6),
// paths aside: the rust lane's rusty::Vec/Option are std's own.

#[allow(unused_imports)]
use crate::*;
use vstd::prelude::*;

verus! {

// ==========================================================================
// WHAT A CORE DECISION ASKS THE SHELL TO DO ([move, M3], Phase 2)
//
// A core function makes no FFI call, takes no lock, reads no clock and fires
// no callback. Where it used to, it pushes an action here, at the same
// point, and the shell carries the actions out in push order: under mtx_,
// before the guard is released (RaftServerBase::run_locked_actions), except
// the role change's log entry and leader-change callback, which run once
// it is released ([fix, F6], run_unlocked_actions).
// ==========================================================================
#[allow(non_camel_case_types)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Eq, PartialEq))]
#[repr(i32)]
pub enum CoreActionKind {
    // Hand the committed entries (from_, to_] to the apply queue
    // (EnqueueCommittedEntries).
    APPLY_RANGE = 0,
    // Restart the election timer (resetTimerLocked): the shell samples the
    // clock and a timeout, and RaftCore::reset_election_timer records them
    // (M4).
    RESET_ELECTION = 1,
    // The new leader's no-op: the shell builds the command and appends it,
    // then wakes replication (AppendLeaderNoop).
    APPEND_NOOP = 2,
    // A role was set (raft_log_set_is_leader_entry), and on a transition
    // the leader-change callback is due.
    ROLE_SET = 3,
}

// Which resetTimerLocked call an action stands for. The shell turns it back
// into the reason string the timer reset logs.
#[allow(non_camel_case_types)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Eq, PartialEq))]
#[repr(i32)]
pub enum TimerResetReason {
    BECAME_FOLLOWER = 0,
    STEP_DOWN = 1,
    GRANTED_VOTE = 2,
    APPEND_ENTRIES = 3,
    STARTING_CAMPAIGN = 4,  // [move, M5]
}

pub struct CoreAction {
    kind_: CoreActionKind,
    // APPLY_RANGE
    from_: u64,
    to_: u64,
    // RESET_ELECTION
    reason_: TimerResetReason,
    // ROLE_SET: the term and the role before and after the call, and whether
    // it was a transition, and which.
    term_: u64,
    prev_is_leader_: bool,
    is_leader_: bool,
    became_leader_: bool,
    became_follower_: bool,
}

impl CoreAction {
    fn of_kind(kind: CoreActionKind) -> CoreAction {
        CoreAction {
            kind_: kind,
            from_: 0,
            to_: 0,
            reason_: TimerResetReason::BECAME_FOLLOWER,
            term_: 0,
            prev_is_leader_: false,
            is_leader_: false,
            became_leader_: false,
            became_follower_: false,
        }
    }

    pub fn apply_range(from: u64, to: u64) -> CoreAction {
        let mut action = CoreAction::of_kind(CoreActionKind::APPLY_RANGE);
        action.from_ = from;
        action.to_ = to;
        action
    }

    pub fn reset_election(reason: TimerResetReason) -> CoreAction {
        let mut action = CoreAction::of_kind(CoreActionKind::RESET_ELECTION);
        action.reason_ = reason;
        action
    }

    pub fn append_noop() -> CoreAction {
        CoreAction::of_kind(CoreActionKind::APPEND_NOOP)
    }

    pub fn role_set(term: u64, prev_is_leader: bool, is_leader: bool,
                    became_leader: bool, became_follower: bool) -> CoreAction {
        let mut action = CoreAction::of_kind(CoreActionKind::ROLE_SET);
        action.term_ = term;
        action.prev_is_leader_ = prev_is_leader;
        action.is_leader_ = is_leader;
        action.became_leader_ = became_leader;
        action.became_follower_ = became_follower;
        action
    }

    pub fn kind(&self) -> CoreActionKind { self.kind_ }
    pub fn from(&self) -> u64 { self.from_ }
    pub fn to(&self) -> u64 { self.to_ }
    pub fn reason(&self) -> TimerResetReason { self.reason_ }
    pub fn term(&self) -> u64 { self.term_ }
    pub fn prev_is_leader(&self) -> bool { self.prev_is_leader_ }
    pub fn is_leader(&self) -> bool { self.is_leader_ }
    pub fn became_leader(&self) -> bool { self.became_leader_ }
    pub fn became_follower(&self) -> bool { self.became_follower_ }
}

// The actions of one critical section, in push order.
#[cfg_attr(not(any()), derive(Default))]
pub struct CoreOutput {
    actions_: Vec<CoreAction>,
    // [move, M7] The call's log lines, in push order, and the threshold a
    // line's level must not exceed to be kept (the shell's current level;
    // everything until the shell says otherwise).
    logs_: Vec<CoreLog>,  // [move, M7]
    log_level_: i32,  // [move, M7]
}

impl CoreOutput {
    pub fn new() -> CoreOutput {
        CoreOutput { actions_: Vec::new(), logs_: Vec::new(), log_level_: RAFT_LOG_DEBUG }  // [move, M7]
    }

    // [move, M7] (whole item) Lines above `level` are dropped when pushed.
    pub fn set_log_level(&mut self, level: i32) {
        self.log_level_ = level;
    }

    // [move, M0] (whole item) The level lines are kept at, for the replay
    // recorder (plan A.4).
    pub fn log_level(&self) -> i32 {
        self.log_level_
    }

    // [move, M7] (whole item) One log line, if its level is enabled. At most
    // CORE_LOG_MAX_ARGS arguments are kept.
    pub fn log(&mut self, level: i32, fmt: &'static str, args: &[LogArg]) {
        if level > self.log_level_ {
            return;
        }
        let mut record = CoreLog {
            level,
            fmt,
            args: [LogArg::U(0); CORE_LOG_MAX_ARGS],
            nargs: 0,
        };
        let mut i: usize = 0;
        while i < args.len() && i < CORE_LOG_MAX_ARGS
            invariant
                i <= CORE_LOG_MAX_ARGS,
            decreases CORE_LOG_MAX_ARGS - i,
        {
            record.args[i] = args[i];
            i += 1;
        }
        record.nargs = i;
        self.logs_.push(record);
    }

    // How many log lines, and how many actions, the call recorded.
    pub closed spec fn spec_log_count(&self) -> int {
        self.logs_@.len() as int
    }

    pub closed spec fn spec_len(&self) -> int {
        self.actions_@.len() as int
    }

    // [move, M7] (whole item) the records, for the shell to print
    pub fn log_count(&self) -> (r: usize)
        ensures r == self.spec_log_count(),
    {
        self.logs_.len()
    }

    // [move, M7] (whole item)
    pub fn log_at(&self, i: usize) -> &CoreLog
        requires i < self.spec_log_count(),
    {
        &self.logs_[i]
    }

    pub fn push(&mut self, action: CoreAction) {
        self.actions_.push(action);
    }

    pub fn len(&self) -> (r: usize)
        ensures r == self.spec_len(),
    {
        self.actions_.len()
    }

    pub fn is_empty(&self) -> bool {
        self.actions_.is_empty()
    }

    pub fn at(&self, i: usize) -> &CoreAction
        requires i < self.spec_len(),
    {
        &self.actions_[i]
    }
}

} // verus!
