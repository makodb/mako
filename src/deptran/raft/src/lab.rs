// The RaftLab correctness harness: the cluster fixture.
//
// This is the suite's view of the five replicas -- the committed-command
// table, the apply callback, Start/Wait/DoAgreement, connectivity, the RPC
// counters and the unreliable-network loop. It reaches each replica's
// RaftServerBase directly, so none of it crosses a C ABI.
//
// The cases themselves are in lab_cases.rs and lab_snapshot_cases.rs.
//
// Five C++ kernels back it, all `#ifdef RAFT_TEST_CORO` in server.cc:
//
//   raft_lab_make_commit_command   build a TpcCommitCommand carrying a tx_id
//   raft_lab_commit_tx_id          read that tx_id back out
//   raft_lab_make_learner_action   wrap a Rust fn as the std::function apply
//   raft_lab_make_leader_change_cb wrap a Rust fn as the leader-change callback
//   raft_lab_frame_rpc_count       RaftCommo::rpc_count_ under its own mutex
//
// The first three exist for one reason: a payload's identity lives in the C++
// registry -- PayloadMember<MakoCommands, T>::KIND plus
// SerializableRegistry::reg<T> -- so Rust can hold a janus::Command but cannot
// be a member of the set. The fourth is the communicator, which is C++.

#![allow(non_snake_case)]

use crate::scheduler_h::{RaftSpecific, RaftStartResult, TxLogServer};
use crate::server_h::{lab_entries, lab_get, LabEntry, RaftLockGuard, RaftServerBase, RaftStdLockGuard};
use std::collections::BTreeMap;
use std::sync::Mutex;

// ---------------------------------------------------------------------------
// Constants, transcribed rather than chosen. A divergence here is a bug in
// this file.

/// The lab cluster is five replicas.
pub const NSERVERS: usize = 5;
/// How long a case waits for an election to settle, in microseconds.
// testconf.h:22 -- `#define ELECTIONTIMEOUT 5000000` (microseconds).
pub const ELECTION_TIMEOUT_US: u64 = 5_000_000;
/// Lab commands are non-negative, so -1 marks an unfilled slot. A
/// snapshot-restored replica must not be credited for log history its new
/// apply callback never replayed.
const MISSING: i32 = -1;
/// `getServerIdByIndex` returns `siteid_t(-1)` on a bad index; this is that,
/// spelled as what it means.
pub const NO_SERVER: u32 = u32::MAX;

unsafe extern "C" {
    fn raft_lab_make_commit_command(tx_id: i64, out: *mut rusty::RaftCommand);
    fn raft_lab_commit_tx_id(cmd: *const rusty::RaftCommand) -> i64;
    /// [M0] MAKO_RAFT_LAB_COMMIT_LOG=1, read once (server.cc).
    fn raft_lab_commit_log_enabled() -> bool;
    fn raft_lab_make_learner_action(
        ctx: u64,
        apply: extern "C" fn(u64, u64, *const rusty::RaftCommand) -> i32,
        out: *mut rusty::LearnerAction);
    fn raft_lab_frame_rpc_count(loc_id: u32) -> u64;
    /// [fix, F6] Wrap a Rust fn as a replica's leader-change callback.
    fn raft_lab_make_leader_change_cb(
        ctx: u64,
        notify: extern "C" fn(u64, bool),
        out: *mut rusty::RaftLeaderChangeCb);
    fn raft_lab_snapshot_copy_latest(src: *const rusty::RaftSnapshotManagerPtr,
                                     dst: *const rusty::RaftSnapshotManagerPtr) -> bool;
    fn raft_lab_byte_string_from(out: *mut rusty::RaftByteString,
                                 data: *const u8, size: usize);
    fn raft_fiber_sleep_us(micros: u64);
    /// The thread-blocking sleep. DoAgreement uses `usleep` where the rest of
    /// the fixture uses `Fiber::sleep`, and the two are not interchangeable --
    /// one yields the fiber, the other stops the poll thread. Ported as it is,
    /// because changing which one a case uses changes the timing the case
    /// measures.
    fn usleep(micros: u32) -> i32;
    /// libc rand(). The unreliable-network loop below uses the same one the
    /// C++ netctlLoop does, so the two draw from one sequence.
    fn rand() -> i32;
}

/// The leader DoAgreement is waiting on is no longer the one it started with.
const fn wait_leader_is_invalid(disconnected: bool, is_leader: bool,
                                current_term: u64, expected_term: u64) -> bool {
    disconnected || !is_leader || current_term != expected_term
}

/// `Fiber::sleep` -- yields this fiber, reactor keeps running.
pub fn fiber_sleep_us(micros: u64) {
    unsafe { raft_fiber_sleep_us(micros) };
}

/// `usleep` -- blocks the poll thread. See the note on the declaration.
pub fn block_sleep_us(micros: u32) {
    unsafe { usleep(micros) };
}

// ---------------------------------------------------------------------------
// Fixture state. The C++ fixture keeps these as statics on RaftTestConfig
// (replicas, committed_cmds, rpc_count_last, disconnected_); the cluster
// itself is lab_registry, so only the bookkeeping lives here.

static COMMITTED: Mutex<BTreeMap<u32, Vec<i32>>> = Mutex::new(BTreeMap::new());
static RPC_COUNT_LAST: Mutex<BTreeMap<u32, u64>> = Mutex::new(BTreeMap::new());
/// Network-control state, the four `RaftTestConfig` members that netctlLoop
/// and the cases share: which replicas a case explicitly disconnected, whether
/// the unreliable mode is on, whether the loop has acted on it yet, and
/// whether the suite is over.
struct NetCtl {
    explicit: BTreeMap<u32, bool>,
    unreliable: bool,
    unreliable_active: bool,
    finished: bool,
}

static NET: Mutex<NetCtl> = Mutex::new(NetCtl {
    explicit: BTreeMap::new(),
    unreliable: false,
    unreliable_active: false,
    finished: false,
});
static NET_CV: std::sync::Condvar = std::sync::Condvar::new();

/// Under an unreliable network a replica is down with probability 1/10 in
/// each period.
const DOWNRATE_N: i32 = 1;
const DOWNRATE_D: i32 = 10;
/// The slow timeout's upper bound, in milliseconds.
const MAXSLOW: i32 = 27;

// ---------------------------------------------------------------------------
// The lab cluster fixture, query half.
//
// waitOneLeader, OneLeader, NoLeader, OneTerm and TermMovedOn, over
// the lab registry (server_h.rs). These are what the 25 cases lean on to decide whether the
// cluster is in a legal state.
//
// The constants, the retry counts and the sleep grain below are what the 25
// cases were tuned against, not free parameters: shortening the election
// timeout or the retry count makes cases flaky without anything being
// wrong.
// No trait import: IsDisconnected became an inherent method when the RPC
// admission gate moved into ServeVote and its siblings, so it left
// RaftSpecific along with the three On* handlers.

// waitOneLeader's loop: ten passes, each preceded by ELECTIONTIMEOUT/10.
const WAIT_RETRIES: i32 = 10;

// The sentinels waitOneLeader returns. -1 "no leader", -2 "two leaders in
// one term", -3 "leader is not the expected one". The 25 cases branch on
// these exact values.
pub const NO_LEADER: i32 = -1;
pub const MULTIPLE_LEADERS: i32 = -2;
pub const UNEXPECTED_LEADER: i32 = -3;

/// Borrow one replica. See the lab registry's note (server_h.rs) on why this is sound in a
/// lab build and nowhere else.
pub fn with_entry_server<R>(entry: &LabEntry, f: impl FnOnce(&mut RaftServerBase) -> R) -> R {
    // SAFETY: the worker owns every registered server for the life of the
    // suite, and the harness runs on a fiber in that same process.
    unsafe { f(&mut *entry.server()) }
}

fn state_of(entry: &LabEntry) -> (bool, u64, bool) {
    with_entry_server(entry, |svr| {
        let mut is_leader = false;
        let mut term: u64 = 0;
        svr.GetState(&raw mut is_leader, &raw mut term);
        (is_leader, term, svr.IsDisconnected())
    })
}

// ---------------------------------------------------------------------------
// [fix, F6] Leader-change notices: every replica's, in the order it fired
// them (true = became leader).

static NOTICES: Mutex<BTreeMap<u32, Vec<bool>>> = Mutex::new(BTreeMap::new());

extern "C" fn lab_leader_notice(ctx: u64, is_leader: bool) {
    NOTICES.lock().unwrap().entry(ctx as u32).or_default().push(is_leader);
}

/// Registers a recorder as every replica's leader-change callback, after
/// forgetting what any earlier recorder saw.
pub fn record_leader_notices() {
    NOTICES.lock().unwrap().clear();
    for entry in lab_entries() {
        let Some(e) = lab_get(entry.loc_id) else { continue };
        with_entry_server(&e, |svr| {
            let mut cb: rusty::RaftLeaderChangeCb = Default::default();
            // SAFETY: the kernel constructs the callable into the slot, and
            // registration copies it.
            unsafe {
                raft_lab_make_leader_change_cb(
                    entry.loc_id as u64, lab_leader_notice, &raw mut cb);
            }
            svr.RegisterLeaderChangeCallback(&cb);
        });
    }
}

/// What replica `svr` has fired since record_leader_notices: (how many, the
/// last one, whether two equal notices ever came in a row).
pub fn leader_notices(svr: u32) -> (usize, bool, bool) {
    let notices = NOTICES.lock().unwrap();
    let Some(seen) = notices.get(&svr) else {
        return (0, false, false);
    };
    let mut repeated = false;
    let mut i: usize = 1;
    while i < seen.len() {
        if seen[i] == seen[i - 1] {
            repeated = true;
        }
        i += 1;
    }
    let last = if seen.is_empty() { false } else { seen[seen.len() - 1] };
    (seen.len(), last, repeated)
}

/// Whether replica `svr` leads right now, or None if no such replica.
pub fn leads(svr: u32) -> Option<bool> {
    lab_get(svr).map(|e| state_of(&e).0)
}

/// Port of RaftTestConfig::waitOneLeader. Disconnected replicas are
/// ignored; two leaders sharing a term is a hard failure; otherwise the
/// leader with the highest term wins.
pub fn wait_one_leader(want_leader: bool, expected: i32) -> i32 {
    for _ in 0..WAIT_RETRIES {
        // SAFETY: called from a lab fiber, which is what the C++ fixture
        // does too (Fiber::sleep(ELECTIONTIMEOUT/10)).
        unsafe { raft_fiber_sleep_us(ELECTION_TIMEOUT_US / 10) };
        let mut leader: i32 = NO_LEADER;
        let mut most_recent_term: u64 = 0;
        for entry in lab_entries() {
            let (is_leader, term, disconnected) = state_of(&entry);
            if disconnected {
                continue;
            }
            if is_leader {
                if term == most_recent_term {
                    note_leader(term, MULTIPLE_LEADERS);  // [M0]
                    return MULTIPLE_LEADERS;
                } else if term > most_recent_term {
                    leader = entry.loc_id as i32;
                    most_recent_term = term;
                }
            }
        }
        if leader != NO_LEADER {
            note_leader(most_recent_term, leader);  // [M0]
            if want_leader && expected >= 0 && leader != expected {
                return UNEXPECTED_LEADER;
            }
            return leader;
        }
    }
    note_leader(0, NO_LEADER);  // [M0]
    NO_LEADER
}

// ---------------------------------------------------------------------------
// [M0] The equivalence comparator's view of a case (docs/verus/
// modification-plan.md A.4 item 1; scripts/verus/lab_trace_compare.py).
// Printed only when MAKO_RAFT_LAB_COMMIT_LOG=1; no case reads any of it.
// Lines belong to the case whose "TEST <n>:" line precedes them on stderr.

/// `LABLEADER <term> <leader>`: what one waitOneLeader pass settled on (a
/// loc id, or the -1/-2 sentinels; term 0 when no leader was seen).
fn note_leader(term: u64, leader: i32) {
    if unsafe { raft_lab_commit_log_enabled() } {
        eprintln!("LABLEADER {} {}", term, leader);
    }
}

/// `LABCOMMIT <node> <index> <term> <payload>`, one line per slot a replica's
/// apply callback filled, in node then index order, printed when a case ends.
/// The payload is the command's tx_id, which determines a lab command
/// completely. The term is read from the replica's log under its mutex now:
/// a committed entry never changes, so it is the term the entry was applied
/// with. It prints as `-` once compaction has removed the entry.
pub fn dump_commit_log() {
    if !unsafe { raft_lab_commit_log_enabled() } {
        return;
    }
    for entry in lab_entries() {
        // This replica's row, copied element by element under the table's
        // lock (not BTreeMap::clone, which the C++ lane's btree port cannot
        // compile for a Vec value), so the lock is not held while the
        // replica's own mutex is taken below.
        let mut commands: Vec<i32> = Vec::new();
        {
            let committed = COMMITTED.lock().unwrap();
            let Some(row) = committed.get(&entry.loc_id) else { continue };
            let mut k: usize = 0;
            while k < row.len() {
                commands.push(row[k]);
                k += 1;
            }
        }
        let Some(e) = lab_get(entry.loc_id) else { continue };
        let (base, terms): (u64, Vec<u64>) = with_entry_server(&e, |svr| {
            let _lock = RaftLockGuard::new(svr.LabMutex());
            let n = svr.LabLogFingerprintLen();
            let base = svr.LabLogFingerprintAt(0);
            let mut terms: Vec<u64> = Vec::new();
            let mut i: u64 = 2;
            while i < n {
                terms.push(svr.LabLogFingerprintAt(i));
                i += 1;
            }
            (base, terms)
        });
        let mut index: usize = 1;
        while index < commands.len() {
            let cmd = commands[index];
            if cmd != MISSING {
                let at = index as u64;
                if at >= base && ((at - base) as usize) < terms.len() {
                    eprintln!("LABCOMMIT {} {} {} {}", entry.loc_id, at,
                              terms[(at - base) as usize], cmd);
                } else {
                    eprintln!("LABCOMMIT {} {} - {}", entry.loc_id, at, cmd);
                }
            }
            index += 1;
        }
    }
}

pub fn one_leader(expected: i32) -> i32 {
    wait_one_leader(true, expected)
}

pub fn no_leader() -> bool {
    wait_one_leader(false, NO_LEADER) == NO_LEADER
}

/// Port of RaftTestConfig::OneTerm: the shared term, or -1 as u64 when the
/// replicas disagree. Note it reads EVERY replica, disconnected included --
/// that is what the C++ does, and the cases depend on it.
pub fn one_term() -> u64 {
    let entries = lab_entries();
    let Some(first) = entries.first() else {
        return u64::MAX;
    };
    let (_, term, _) = state_of(first);
    for entry in entries.iter().skip(1) {
        let (_, cur, _) = state_of(entry);
        if cur != term {
            return u64::MAX;
        }
    }
    term
}

/// Port of RaftTestConfig::TermMovedOn, over the same predicate the C++
/// uses (`!disconnected && current_term > observed_term`).
pub fn term_moved_on(term: u64) -> bool {
    lab_entries().iter().any(|entry| {
        let (_, cur, disconnected) = state_of(entry);
        !disconnected && cur > term
    })
}

/// Port of the RaftTestConfig constructor: every replica starts with one
/// unfilled slot, a zero rpc baseline, and connected.
pub fn reset() {
    let mut committed = COMMITTED.lock().unwrap();
    let mut last = RPC_COUNT_LAST.lock().unwrap();
    let mut net = NET.lock().unwrap();
    committed.clear();
    last.clear();
    net.explicit.clear();
    for entry in lab_entries() {
        committed.insert(entry.loc_id, vec![MISSING]);
        last.insert(entry.loc_id, 0);
        net.explicit.insert(entry.loc_id, false);
    }
    net.unreliable = false;
}


// ---------------------------------------------------------------------------
// The apply path.

/// The learner action, once per replica. `ctx` is the replica's locale id,
/// which is what the C++ lambda captures as `svr`.
///
/// The C++ callback `verify`s the payload kind before unpacking; here the
/// kernel returns -1 for anything that is not a TpcCommitCommand, and the same
/// assertion is made on that.
extern "C" fn lab_apply(ctx: u64, slot: u64, cmd: *const rusty::RaftCommand) -> i32 {
    // SAFETY: the kernel hands the command over borrowed for the call and
    // does not retain it; we only read an integer out of it.
    let tx_id = unsafe { raft_lab_commit_tx_id(cmd) };
    assert!(tx_id >= 0, "lab apply saw a payload that is not a TpcCommitCommand");
    record_committed(ctx as u32, slot, tx_id as i32);
    0
}

/// Port of RaftTestConfig::RecordCommittedCommand, including both of its
/// `verify`s: a slot is filled at most once, and never with the sentinel.
fn record_committed(svr: u32, slot: u64, cmd: i32) {
    assert!(cmd != MISSING);
    let mut committed = COMMITTED.lock().unwrap();
    let commands = committed.entry(svr).or_default();
    let slot_index = slot as usize;
    if commands.len() <= slot_index {
        commands.resize(slot_index + 1, MISSING);
    }
    assert!(commands[slot_index] == MISSING || commands[slot_index] == cmd,
            "replica {svr} committed two different values at slot {slot}");
    commands[slot_index] = cmd;
}

/// Port of RaftTestConfig::SetLearnerAction. The apply mutex is taken for the
/// same reason the C++ takes it: the runtime apply path holds it before
/// copying app_next_, so replacing the startup placeholder without it is a
/// data race.
pub fn set_learner_action() {
    for entry in lab_entries() {
        let Some(e) = lab_get(entry.loc_id) else { continue };
        with_entry_server(&e, |svr| {
            let mut action: rusty::LearnerAction = Default::default();
            // SAFETY: the kernel copies the callable into the slot; `action`
            // is then handed to reg_learner_action, which copies it again
            // into app_next_ (never moves it -- see that method's note).
            unsafe {
                raft_lab_make_learner_action(
                    entry.loc_id as u64, lab_apply, &raw mut action);
            }
            let _apply_lock = RaftStdLockGuard::new(svr.LabApplyMutex());
            svr.reg_learner_action(&action);
        });
    }
}

// ---------------------------------------------------------------------------
// Queries over the committed table.

/// Port of RaftTestConfig::NCommitted: how many replicas have `index`, or -1
/// if they disagree about what is there.
pub fn n_committed(index: u64) -> i32 {
    let committed = COMMITTED.lock().unwrap();
    let mut cmd = 0i32;
    let mut n = 0i32;
    for entry in lab_entries() {
        let Some(commands) = committed.get(&entry.loc_id) else { continue };
        let Some(&cur) = commands.get(index as usize) else { continue };
        if cur == MISSING {
            continue;
        }
        if n == 0 {
            cmd = cur;
        } else if cur != cmd {
            return -1;
        }
        n += 1;
    }
    n
}

/// Port of RaftTestConfig::ServerCommitted.
pub fn server_committed(svr: u32, index: u64, cmd: i32) -> bool {
    let committed = COMMITTED.lock().unwrap();
    // A match rather than `is_some_and`, which rusty::Option does not have.
    match committed.get(&svr).and_then(|commands| commands.get(index as usize)) {
        Some(&value) => value == cmd,
        None => false,
    }
}

/// The value any replica has at `index`, which is what Wait returns.
fn any_committed_value(index: u64) -> Option<i32> {
    let committed = COMMITTED.lock().unwrap();
    for entry in lab_entries() {
        if let Some(&value) = committed.get(&entry.loc_id)
            .and_then(|commands| commands.get(index as usize))
        {
            if value != MISSING {
                return Some(value);
            }
        }
    }
    None
}

// ---------------------------------------------------------------------------
// Driving the cluster.

/// Port of RaftTestConfig::Start. Returns (appended, index, term).
pub fn start(svr: u32, cmd: i32) -> (bool, u64, u64) {
    let mut index: u64 = 0;
    let mut term: u64 = 0;
    let appended = lab_get(svr).map(|e| with_entry_server(&e, |server| {
        let mut command: rusty::RaftCommand = Default::default();
        // SAFETY: the kernel constructs a janus::Command into the slot; the
        // Drop impl registered for RaftCommand frees it at end of scope.
        unsafe { raft_lab_make_commit_command(cmd as i64, &raw mut command) };
        server.Start(&command, &raw mut index, &raw mut term) == RaftStartResult::APPENDED
    }));
    (appended.unwrap_or(false), index, term)
}

/// [fix, F4] One AppendEntries handed to replica `svr` as if it had arrived:
/// a single entry (a lab command carrying `tx_id`) whose entry term is
/// `entry_term`, or no entry at all when `tx_id` is negative. Returns the
/// reply (ok, follower term, follower last index), or None if no such replica.
#[allow(clippy::too_many_arguments)]
pub fn serve_append(svr: u32, leader_term: u64, leader: u16, prev_index: u64,
                    prev_term: u64, commit: u64, entry_term: u64,
                    tx_id: i32) -> Option<(u64, u64, u64)> {
    lab_get(svr).map(|e| with_entry_server(&e, |server| {
        let mut command: rusty::RaftCommand = Default::default();
        if tx_id >= 0 {
            // SAFETY: as in start(): the kernel constructs into the slot.
            unsafe { raft_lab_make_commit_command(tx_id as i64, &raw mut command) };
        }
        let mut ok: u64 = 0;
        let mut term: u64 = 0;
        let mut last: u64 = 0;
        server.ServeAppendEntries(leader_term, leader, prev_index, prev_term, commit,
                                  &command, entry_term, &raw mut ok, &raw mut term,
                                  &raw mut last);
        (ok, term, last)
    }))
}

/// A RequestVote handed to replica `svr` as if it had arrived. Returns
/// (reply term, granted), or None if no such replica.
pub fn serve_vote(svr: u32, last_log_index: u64, last_log_term: i64,
                  candidate: u16, candidate_term: i64) -> Option<(i64, i8)> {
    lab_get(svr).map(|e| with_entry_server(&e, |server| {
        let mut reply_term: i64 = 0;
        let mut granted: i8 = 0;
        server.ServeVote(last_log_index, last_log_term, candidate, candidate_term,
                         &raw mut reply_term, &raw mut granted);
        (reply_term, granted)
    }))
}

/// A replica's site id: what it names itself in RPCs (a lab index is not one).
pub fn site_id_of(svr: u32) -> Option<u16> {
    lab_get(svr).map(|e| with_entry_server(&e, |server| server.SiteId()))
}

/// [fix, F4] One coherent look at a replica's log tail, under its mutex:
/// (current term, last index, the last entry's term or 0, commit index,
/// whether any entry in the log has term 0).
pub fn log_tail(svr: u32) -> Option<(u64, u64, u64, u64, bool)> {
    lab_get(svr).map(|e| with_entry_server(&e, |server| {
        let _lock = RaftLockGuard::new(server.LabMutex());
        let n = server.LabLogFingerprintLen();
        let mut last_term: u64 = 0;
        let mut any_term_zero = false;
        let mut i: u64 = 2;
        while i < n {
            let t = server.LabLogFingerprintAt(i);
            if t == 0 {
                any_term_zero = true;
            }
            last_term = t;
            i += 1;
        }
        (server.LabCurrentTerm(), server.LabLastLogIndex(), last_term,
         server.LabCommitIndex(), any_term_zero)
    }))
}

/// Port of RaftTestConfig::Wait. The sentinels are what the cases branch on:
/// -1 timeout, -2 term moved on, -3 values differ.
pub const WAIT_TIMEOUT: i64 = -1;
pub const WAIT_TERM_MOVED: i64 = -2;
pub const WAIT_VALUES_DIFFER: i64 = -3;

pub fn wait(index: u64, n: i32, term: u64) -> i64 {
    let mut to: u64 = 10_000; // 10 milliseconds
    let mut i = 0;
    while i < 30 {
        let nc = n_committed(index);
        if nc < 0 {
            return WAIT_VALUES_DIFFER;
        } else if nc >= n {
            break;
        }
        fiber_sleep_us(to);
        if to < 1_000_000 {
            to *= 2;
        }
        if term_moved_on(term) {
            return WAIT_TERM_MOVED;
        }
        i += 1;
    }
    if i == 30 {
        return WAIT_TIMEOUT;
    }
    // The C++ `verify(0)`s if no replica has the slot; reaching here means
    // n_committed said `nc >= n`, so one does.
    any_committed_value(index)
        .expect("Wait: n_committed counted a slot no replica has") as i64
}

/// Port of RaftTestConfig::DoAgreement. Returns the commit index, or 0.
pub fn do_agreement(cmd: i32, n: i32, retry: bool) -> u64 {
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(10);
    while std::time::Instant::now() < deadline {
        block_sleep_us(50_000);
        let mut ldr = NO_SERVER;
        let mut index: u64 = 0;
        let mut term: u64 = 0;
        for entry in lab_entries() {
            if lab_get(entry.loc_id).map(|e| with_entry_server(&e, |s| s.IsDisconnected())).unwrap_or(true) {
                continue;
            }
            let (ok, i, t) = start(entry.loc_id, cmd);
            if ok {
                ldr = entry.loc_id;
                index = i;
                term = t;
                break;
            }
        }
        if ldr == NO_SERVER {
            continue;
        }
        let inner_deadline =
            std::time::Instant::now() + std::time::Duration::from_secs(10);
        while std::time::Instant::now() < inner_deadline {
            if retry {
                if term_moved_on(term) {
                    break;
                }
                let state = lab_get(ldr).map(|e| with_entry_server(&e, |s| {
                    let mut is_leader = false;
                    let mut cur_term: u64 = 0;
                    s.GetState(&raw mut is_leader, &raw mut cur_term);
                    (s.IsDisconnected(), is_leader, cur_term)
                }));
                let Some((disconnected, is_leader, cur_term)) = state else { break };
                if wait_leader_is_invalid(disconnected, is_leader, cur_term, term) {
                    break;
                }
            }
            let nc = n_committed(index);
            if nc < 0 {
                break;
            } else if nc >= n {
                // The C++ walks the replicas and takes the first that has the
                // slot; a mismatch there means a different command won the
                // index, which is a retry, not an agreement.
                match any_committed_value(index) {
                    Some(value) if value == cmd => return index,
                    _ => break,
                }
            }
            block_sleep_us(20_000);
        }
        if !retry {
            return 0;
        }
    }
    0
}

// ---------------------------------------------------------------------------
// Network control. Disconnect/Reconnect are Rust methods on the server
// already; what stays C++ is the communicator behind them.

/// The explicit Disconnect the cases call. `verify(!disconnected_[svr])` in
/// the C++ becomes an assert here: a case that disconnects twice is a bug in
/// the case.
pub fn disconnect(svr: u32) {
    let mut net = NET.lock().unwrap();
    assert!(!net.explicit.get(&svr).copied().unwrap_or(false),
            "Disconnect({svr}) but it is already disconnected");
    set_link(svr, false, false);
    net.explicit.insert(svr, true);
}

pub fn reconnect(svr: u32) {
    let mut net = NET.lock().unwrap();
    assert!(net.explicit.get(&svr).copied().unwrap_or(false),
            "Reconnect({svr}) but it is not disconnected");
    set_link(svr, true, false);
    net.explicit.insert(svr, false);
}

pub fn n_disconnected() -> i32 {
    NET.lock().unwrap().explicit.values().filter(|&&d| d).count() as i32
}

/// The idempotent form, used by the unreliable-network loop, which flips
/// links that Disconnect() did not own. `ignore` means "it is not an error if
/// the link is already in this state", which is every call the loop makes.
fn set_link(svr: u32, up: bool, ignore: bool) {
    let applied = lab_get(svr).map(|e| with_entry_server(&e, |s| {
        if up && s.IsDisconnected() {
            s.Reconnect();
            true
        } else if !up && !s.IsDisconnected() {
            // `true` explicitly: Disconnect takes the flag, and the DSL this
            // came from had no default arguments.
            s.Disconnect(true);
            true
        } else {
            false
        }
    }));
    if !ignore && applied == Some(false) {
        let state = if up { "up" } else { "down" };
        panic!("link for replica {} was already {}", svr, state);
    }
}

pub fn is_unreliable() -> bool {
    NET.lock().unwrap().unreliable
}

/// Port of RaftTestConfig::SetUnreliable. The loop below is the netctlLoop
/// thread; this is the handshake with it.
pub fn set_unreliable(unreliable: bool) {
    let mut net = NET.lock().unwrap();
    assert!(!net.finished);
    assert_ne!(net.unreliable, unreliable,
               "SetUnreliable({unreliable}) but the network is already that");
    net.unreliable = unreliable;
    drop(net);
    NET_CV.notify_one();
    if !unreliable {
        // The C++ drops and retakes the lock so netctlLoop can restore every
        // link before the next case runs. Wait for the loop to say it did,
        // rather than for the lock, because a lock handoff is not a promise.
        let mut net = NET.lock().unwrap();
        while net.unreliable_active {
            net = NET_CV.wait(net).unwrap();
        }
    }
}

/// Port of RaftTestConfig::netctlLoop, on its own thread as in the C++.
///
/// `slow` is `usleep` on THIS thread in the C++ too -- it never reached the
/// reactor -- so it is transcribed as the same sleep rather than promoted into
/// something that would actually delay a replica.
fn netctl_loop() {
    let mut net = NET.lock().unwrap();
    while !net.finished {
        if !net.unreliable {
            restore_links(&net);
            net.unreliable_active = false;
            NET_CV.notify_all();
            while !net.unreliable && !net.finished {
                net = NET_CV.wait(net).unwrap();
            }
            continue;
        }
        net.unreliable_active = true;
        for entry in lab_entries() {
            let svr = entry.loc_id;
            // skip a replica the case itself disconnected
            if net.explicit.get(&svr).copied().unwrap_or(false) {
                continue;
            }
            // DOWNRATE_N / DOWNRATE_D chance of being down this period
            // SAFETY: libc rand(); the C++ loop calls the same one.
            if (unsafe { rand() } % DOWNRATE_D) < DOWNRATE_N {
                set_link(svr, false, true);
            } else {
                set_link(svr, true, true);
                // SAFETY: as above.
                let msec = (unsafe { rand() } % MAXSLOW) as u32;
                block_sleep_us(msec * 1000);
            }
        }
        // change unreliable state every 0.1s
        block_sleep_us(100_000);
        drop(net);
        block_sleep_us(10_000);
        net = NET.lock().unwrap();
    }
    if net.unreliable {
        net.unreliable = false;
        restore_links(&net);
        net.unreliable_active = false;
        NET_CV.notify_all();
    }
}

fn restore_links(net: &NetCtl) {
    for entry in lab_entries() {
        if !net.explicit.get(&entry.loc_id).copied().unwrap_or(false) {
            set_link(entry.loc_id, true, true);
        }
    }
}

static NETCTL_THREAD: Mutex<Option<std::thread::JoinHandle<()>>> = Mutex::new(None);

/// Starts the network-control thread, as the RaftTestConfig constructor does.
pub fn start_netctl() {
    let mut slot = NETCTL_THREAD.lock().unwrap();
    assert!(slot.is_none(), "the lab suite runs once per process");
    *slot = Some(std::thread::spawn(netctl_loop));
}

/// Port of RaftTestConfig::Shutdown: stop the loop and join it, restore every
/// link a case left down, then quiesce each replica through its owner-thread
/// barrier before the harness stops the poll threads.
pub fn shutdown() {
    {
        let mut net = NET.lock().unwrap();
        assert!(!net.finished);
        net.finished = true;
    }
    NET_CV.notify_all();
    if let Some(handle) = NETCTL_THREAD.lock().unwrap().take() {
        let _ = handle.join();
    }
    let still_down: Vec<u32> = {
        let net = NET.lock().unwrap();
        net.explicit.iter().filter(|(_, &d)| d).map(|(&s, _)| s).collect()
    };
    for svr in still_down {
        reconnect(svr);
    }
    for entry in lab_entries() {
        if let Some(e) = lab_get(entry.loc_id) {
            with_entry_server(&e, |s| s.PrepareForShutdown());
        }
    }
}

// ---------------------------------------------------------------------------
// RPC counters.

/// Port of RaftTestConfig::RpcCount: the delta since the last reset.
pub fn rpc_count(svr: u32, reset: bool) -> u64 {
    // SAFETY: the kernel looks the replica up in RaftFrame::frames_ and takes
    // the communicator's own mutex before reading the counter.
    let count = unsafe { raft_lab_frame_rpc_count(svr) };
    let mut last = RPC_COUNT_LAST.lock().unwrap();
    let previous = last.get(&svr).copied().unwrap_or(0);
    if reset {
        last.insert(svr, count);
    }
    assert!(count >= previous, "rpc count went backwards on replica {svr}");
    count - previous
}

pub fn rpc_total() -> u64 {
    // SAFETY: as in rpc_count above.
    lab_entries().iter()
        .map(|e| unsafe { raft_lab_frame_rpc_count(e.loc_id) })
        .sum()
}

/// Copy one snapshot manager's latest checkpoint into another. See the
/// kernel's note: a compacted boundary is meaningless without its exact bytes.
pub fn copy_snapshot(src: &rusty::RaftSnapshotManagerPtr,
                     dst: &rusty::RaftSnapshotManagerPtr) -> bool {
    // SAFETY: both are live shared_ptrs held by the caller for the call.
    unsafe { raft_lab_snapshot_copy_latest(src as *const _, dst as *const _) }
}

/// A snapshot payload, as the opaque std::string carrier OnInstallSnapshot
/// takes. Rust cannot build one directly.
pub fn byte_string(text: &str) -> rusty::RaftByteString {
    let mut out: rusty::RaftByteString = Default::default();
    // SAFETY: the kernel copies `len` bytes into a fresh std::string; the
    // carrier's Drop frees it.
    unsafe { raft_lab_byte_string_from(&raw mut out, text.as_ptr(), text.len()) };
    out
}

// ---------------------------------------------------------------------------
// Identity helpers. The registry is sorted by locale id, so a replica's
// index is stable for the life of the suite and a case can name "the server
// two along from the leader" without ambiguity.

pub fn server_id_by_index(index: usize) -> u32 {
    lab_entries().get(index).map_or(NO_SERVER, |e| e.loc_id)
}

pub fn map_server_id(server_id: u32) -> usize {
    lab_entries().iter()
        .position(|e| e.loc_id == server_id)
        .unwrap_or(0)
}

pub fn next_server_id(current: u32, offset: i32) -> u32 {
    let entries = lab_entries();
    let Some(current_index) = entries.iter().position(|e| e.loc_id == current) else {
        return current;
    };
    let n = entries.len() as i32;
    let new_index = (((current_index as i32 + offset) % n) + n) % n;
    entries[new_index as usize].loc_id
}


/// The C++ spells "servers disagree" as `term != -1` on a uint64_t.
pub const TERM_DISAGREE: u64 = u64::MAX;
