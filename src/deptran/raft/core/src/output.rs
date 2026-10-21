// What a core call asks the shell to do: actions, in push order.
//
// [move, M1] Moved verbatim from src/deptran/raft/shell/server_h.rs,
// paths aside: the rust lane's rusty::Vec/Option are std's own.

#[allow(unused_imports)]
use crate::*;
use vstd::prelude::*;

verus! {

// ==========================================================================
// WHAT A CORE DECISION ASKS THE SHELL TO DO ([move, M3])
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

// [fix, F21] (whole item) One step's change to the saved state:
// the hard state after the step if term, vote or commit
// changed, and the lowest log index the step wrote (0: the log is unchanged;
// the log from that index on is the step's log there). The shell turns it
// into a write-ahead log record in disk builds and ignores it otherwise.
#[derive(Clone, Copy)]
pub struct PersistNote {
    pub hard_: bool,
    pub term_: u64,
    pub vote_: u16,
    pub commit_: u64,
    pub log_from_: u64,
    pub snap_: Option<SnapNote>,  // [fix, F25]
}

// [fix, F25] (whole item) A step that moved the snapshot boundary: the new
// boundary's index and term, and whether the log after it stays (a snapshot
// taken here, or an install whose boundary matches the local log) or goes (an
// install that supersedes the log). The shell writes the image file, then the
// record that names it.
#[derive(Clone, Copy)]
pub struct SnapNote {
    pub index_: u64,
    pub term_: i64,
    pub keep_: bool,
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
    // [fix, F21] the step in progress's lowest log write (0: none), and the
    // last finished step's note
    log_from_: u64,  // [fix, F21]
    persist_: Option<PersistNote>,  // [fix, F21]
    snap_: Option<SnapNote>,  // [fix, F25] the step in progress's boundary move
}

// [M12] persistence (P2): note_log_write's mark, the lower of two writes (0:
// none yet)
pub open spec fn lowest_write(from: u64, index: u64) -> u64 {
    if from == 0 || index < from { index } else { from }
}

// [M12] P2: note_snapshot's mark
pub open spec fn snap_mark(index: u64, term: i64, keep: bool) -> Option<SnapNote> {
    Some(SnapNote { index_: index, term_: term, keep_: keep })
}

// [M12] P2: finish_persist's note from the step's marks (opaque: revealed
// where a note is made or read)
#[verifier::opaque]
pub open spec fn finished_note(m: (u64, Option<SnapNote>, Option<PersistNote>), hard: bool, term: u64,
                               vote: u16, commit: u64) -> Option<PersistNote> {
    if hard || m.0 != 0 || m.1 is Some {
        Some(PersistNote { hard_: hard, term_: term, vote_: vote, commit_: commit, log_from_: m.0, snap_: m.1 })
    } else {
        None
    }
}

impl CoreOutput {
    pub fn new() -> (r: CoreOutput)  // [M12] P2: named for its ensures
        ensures r.unmarked(),  // [M12] P2
    {
        CoreOutput { actions_: Vec::new(), logs_: Vec::new(), log_level_: RAFT_LOG_DEBUG,  // [move, M7]
                     log_from_: 0, persist_: None, snap_: None }  // [fix, F21, fix, F25]
    }

    // [M12] persistence (P2): the step in progress's marks (its lowest log
    // write, 0 for none, and its boundary move) and the last finished note
    pub closed spec fn notes(&self) -> (u64, Option<SnapNote>, Option<PersistNote>) {
        (self.log_from_, self.snap_, self.persist_)
    }

    pub open spec fn spec_log_from(&self) -> u64 {
        self.notes().0
    }

    pub open spec fn spec_snap(&self) -> Option<SnapNote> {
        self.notes().1
    }

    pub open spec fn spec_persist(&self) -> Option<PersistNote> {
        self.notes().2
    }

    // [M12] P2: a step's start, no mark yet
    pub open spec fn unmarked(&self) -> bool {
        self.spec_log_from() == 0 && self.spec_snap() is None
    }

    // [fix, F21] (whole item) A log writer marks the lowest index it wrote.
    pub fn note_log_write(&mut self, index: u64)
        ensures  // [M12] P2
            final(self).spec_log_from() == lowest_write(old(self).spec_log_from(), index),
            final(self).spec_snap() == old(self).spec_snap(),
            final(self).spec_persist() == old(self).spec_persist(),
    {
        if self.log_from_ == 0 || index < self.log_from_ {
            self.log_from_ = index;
        }
    }

    // [fix, F25] (whole item) A step that moved the snapshot boundary marks it.
    pub fn note_snapshot(&mut self, index: u64, term: i64, keep: bool)
        ensures  // [M12] P2
            final(self).spec_log_from() == old(self).spec_log_from(),
            final(self).spec_snap() == snap_mark(index, term, keep),
            final(self).spec_persist() == old(self).spec_persist(),
    {
        self.snap_ = Some(SnapNote { index_: index, term_: term, keep_: keep });
    }

    // [fix, F21] (whole item) step's end: the note, if the step changed the
    // saved state; a step that changed nothing leaves none. [fix, F25] A
    // boundary move is a change of the saved state.
    pub fn finish_persist(&mut self, hard: bool, term: u64, vote: u16, commit: u64)
        ensures  // [M12] P2
            final(self).unmarked(),
            final(self).spec_persist() == finished_note(old(self).notes(), hard, term, vote, commit),
    {
        proof { reveal(finished_note); }  // [M12] P2
        if hard || self.log_from_ != 0 || self.snap_.is_some() {
            self.persist_ = Some(PersistNote { hard_: hard, term_: term, vote_: vote,
                                               commit_: commit, log_from_: self.log_from_,
                                               snap_: self.snap_ });
        } else {
            self.persist_ = None;
        }
        self.log_from_ = 0;
        self.snap_ = None;
    }

    // [M12] persistence (Part A): whether the last step left a note, the
    // record premise N numbers
    pub open spec fn spec_note(&self) -> bool {
        self.spec_persist() is Some
    }

    // [fix, F21] (whole item) The last step's note, left in place (the
    // replay recorder reads it; disk builds take it).
    pub fn persist(&self) -> (r: Option<PersistNote>)
        ensures r is Some == self.spec_note(),  // [M12]
    {
        self.persist_
    }

    // [fix, F21] (whole item) The last step's note, taken.
    pub fn take_persist(&mut self) -> Option<PersistNote>
        ensures final(self).spec_log_from() == old(self).spec_log_from(),  // [M12] P2
    {
        let note = self.persist_;
        self.persist_ = None;
        note
    }

    // [move, M7] (whole item) Lines above `level` are dropped when pushed.
    pub fn set_log_level(&mut self, level: i32) {
        self.log_level_ = level;
    }

    // [move, M0] (whole item) The level lines are kept at, for the replay
    // recorder.
    pub fn log_level(&self) -> i32 {
        self.log_level_
    }

    // [move, M7] (whole item) One log line, if its level is enabled. At most
    // CORE_LOG_MAX_ARGS arguments are kept.
    pub fn log(&mut self, level: i32, fmt: &'static str, args: &[LogArg])
        ensures final(self).notes() == old(self).notes(),  // [M12] P2
    {
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

    pub fn push(&mut self, action: CoreAction)
        ensures final(self).notes() == old(self).notes(),  // [M12] P2
    {
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
