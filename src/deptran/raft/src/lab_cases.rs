// The 25 RaftLab cases, in Rust (lab-harness-to-rust-plan.md, Phase 3).
//
// A port of test.cc, case for case. The C++ suite stays as the oracle until
// Phase 4: `MAKO_RAFT_LAB_RUST=1` picks this one, and ci.sh runs the binary
// twice, once each way, so a case counts as ported only when both agree on a
// clean run of a FRESH cluster.
//
// Running them back to back in one process was considered and rejected: the
// second suite would start on a cluster whose indices, terms, snapshot
// managers and retention windows the first had already moved, so half the
// cases would be testing something other than what they say. Two runs is the
// honest comparison.
//
// Everything a case reads is a Rust method on RaftServerBase now, so none of
// the 41 `Lab*` exports is called from here. That is the point of the port;
// Phase 4 deletes them.

#![cfg(feature = "raft_test")]

use crate::lab::{self, NSERVERS, ELECTION_TIMEOUT_US};
use std::sync::atomic::{AtomicI32, Ordering};

// ---------------------------------------------------------------------------
// Reporting. The markers must match testconf.h's Print/Init/Passed/Failed
// byte for byte, because ci.sh greps for `^TEST [0-9]* Passed` and for
// `ALL TESTS PASSED`.

static TEST_ID: AtomicI32 = AtomicI32::new(0);

pub fn init(test_id: i32, description: &str) {
    eprintln!("TEST {test_id}: {description}");
    TEST_ID.store(test_id, Ordering::Relaxed);
}

pub fn failed(msg: &str) {
    eprintln!("TEST {} Failed: {}", TEST_ID.load(Ordering::Relaxed), msg);
}

pub fn passed() {
    eprintln!("TEST {} Passed", TEST_ID.load(Ordering::Relaxed));
}

/// `Init2`: every case starts with the network whole.
macro_rules! init2 {
    ($id:expr, $desc:expr) => {
        init($id, $desc);
        assert!(lab::n_disconnected() == 0 && !lab::is_unreliable(),
                "case {} started on a network a previous case left broken", $id);
    };
}

/// `Assert`: bail with no message, as the C++ macro does.
macro_rules! check {
    ($cond:expr) => { if !($cond) { return 1; } };
}

/// `Assert2`: bail with a message.
macro_rules! check_msg {
    ($cond:expr, $($arg:tt)*) => {
        if !($cond) { failed(&format!($($arg)*)); return 1; }
    };
}

/// `AssertOneLeader`.
macro_rules! check_one_leader {
    ($ldr:expr) => { check!($ldr >= 0) };
}

/// `AssertNoneCommitted`.
macro_rules! check_none_committed {
    ($index:expr) => {{
        let nc = lab::n_committed($index);
        check_msg!(nc == 0, "{} servers unexpectedly committed index {}", nc, $index);
    }};
}

/// `AssertNCommitted`.
macro_rules! check_n_committed {
    ($index:expr, $expected:expr) => {{
        let nc = lab::n_committed($index);
        check_msg!(nc == $expected as i32,
                   "{} servers committed index {} ({} expected)", nc, $index, $expected);
    }};
}

/// `AssertStartOk`.
macro_rules! check_start_ok {
    ($ok:expr) => { check_msg!($ok, "unexpected leader change during Start()") };
}

/// `AssertWaitNoError`.
macro_rules! check_wait_no_error {
    ($ret:expr, $index:expr) => {
        check_msg!($ret != lab::WAIT_VALUES_DIFFER,
                   "committed values differ for index {}", $index)
    };
}

/// `AssertWaitNoTimeout`.
macro_rules! check_wait_no_timeout {
    ($ret:expr, $index:expr, $n:expr) => {
        check_msg!($ret != lab::WAIT_TIMEOUT,
                   "waited too long for {} server(s) to commit index {}", $n, $index);
        check_msg!($ret != lab::WAIT_TERM_MOVED,
                   "term moved on before index {} committed by {} server(s)", $index, $n);
    };
}

/// `DoAgreeAndAssertIndex`.
macro_rules! agree_at {
    ($cmd:expr, $n:expr, $index:expr) => {{
        let r = lab::do_agreement($cmd, $n as i32, false);
        let ind: u64 = $index;
        check_msg!(r > 0,
            "failed to reach agreement for command {} among {} servers, expected commit index>0, got {}",
            $cmd, $n, r);
        check_msg!(r == ind, "agreement index incorrect. got {}, expected {}", r, ind);
    }};
}

/// `DoAgreeAndAssertWaitSuccess`.
macro_rules! agree_wait {
    ($st:expr, $cmd:expr, $n:expr) => {{
        let r = lab::do_agreement($cmd, $n as i32, true);
        check_msg!(r > 0, "failed to reach agreement for command {} among {} servers", $cmd, $n);
        $st.index = r + 1;
    }};
}

/// `RaftLabTest::wait` -- a timeout event the C++ waits on. On this side the
/// fiber sleep is the same wait.
fn wait_us(micros: u64) {
    lab::fiber_sleep_us(micros);
}

/// The two numbers RaftLabTest carries between cases (test.h:13-14).
pub struct LabState {
    pub index: u64,
    pub init_rpcs: u64,
}

// ---------------------------------------------------------------------------
// Elections

fn test_initial_election(st: &mut LabState) -> i32 {
    init2!(1, "Initial election");

    // Wait for election timers to start and elections to begin
    lab::fiber_sleep_us(ELECTION_TIMEOUT_US / 10);

    let leader = lab::one_leader(-1);
    check_one_leader!(leader);

    // The RPC count the initial election cost, for testCount below
    st.init_rpcs = 0;
    for i in 0..NSERVERS {
        st.init_rpcs += lab::rpc_count(lab::server_id_by_index(i), true);
    }

    let term = lab::one_term();
    check_msg!(term != lab::TERM_DISAGREE, "servers disagree on term number");
    check_msg!(lab::one_term() == term, "unexpected term change");
    check_one_leader!(lab::one_leader(leader));

    passed();
    0
}

fn test_re_election(_st: &mut LabState) -> i32 {
    init2!(2, "Re-election after network failure");

    let mut leader = lab::one_leader(-1);
    if leader == -1 {
        failed("No leader found in initial election");
        return -1;
    }
    check_one_leader!(leader);

    // disconnect leader -- make sure a new one is elected
    lab::disconnect(leader as u32);
    let old_leader = leader;
    lab::fiber_sleep_us(ELECTION_TIMEOUT_US);

    leader = lab::one_leader(-1);
    if leader == -1 {
        failed("No new leader elected after disconnecting old leader");
        return -1;
    }
    check_one_leader!(leader);
    check_msg!(leader != old_leader, "no reelection despite leader being disconnected");

    // reconnect old leader -- should not disturb new leader
    lab::reconnect(old_leader as u32);
    lab::fiber_sleep_us(ELECTION_TIMEOUT_US);
    check_one_leader!(lab::one_leader(leader));

    // no quorum -> no leader
    lab::disconnect(lab::next_server_id(leader as u32, 1));
    lab::disconnect(lab::next_server_id(leader as u32, 2));
    lab::disconnect(leader as u32);
    check!(lab::no_leader());

    // quorum restored
    lab::reconnect(lab::next_server_id(leader as u32, 2));
    lab::fiber_sleep_us(ELECTION_TIMEOUT_US);
    check_one_leader!(lab::one_leader(-1));

    // rejoin all servers
    lab::reconnect(lab::next_server_id(leader as u32, 1));
    lab::reconnect(leader as u32);
    lab::fiber_sleep_us(ELECTION_TIMEOUT_US);
    check_one_leader!(lab::one_leader(-1));

    passed();
    0
}

// ---------------------------------------------------------------------------
// Agreement

fn test_basic_agree(st: &mut LabState) -> i32 {
    init2!(3, "Basic agreement");

    for _ in 1..=3 {
        // no commits before any agreement is started
        check_none_committed!(st.index);
        let command_value = (st.index + 300) as i32;
        agree_at!(command_value, NSERVERS, st.index);
        st.index += 1;
    }

    passed();
    0
}

fn test_fail_agree(st: &mut LabState) -> i32 {
    init2!(4, "Agreement despite follower disconnection");

    let leader = lab::one_leader(-1);
    check_one_leader!(leader);

    lab::disconnect(lab::next_server_id(leader as u32, 1));
    lab::disconnect(lab::next_server_id(leader as u32, 2));

    // agreement despite 2 disconnected servers
    agree_at!(401, NSERVERS - 2, st.index); st.index += 1;
    agree_at!(402, NSERVERS - 2, st.index); st.index += 1;
    lab::fiber_sleep_us(ELECTION_TIMEOUT_US);
    agree_at!(403, NSERVERS - 2, st.index); st.index += 1;
    agree_at!(404, NSERVERS - 2, st.index); st.index += 1;

    lab::reconnect(lab::next_server_id(leader as u32, 1));
    lab::reconnect(lab::next_server_id(leader as u32, 2));
    lab::fiber_sleep_us(ELECTION_TIMEOUT_US);

    agree_wait!(st, 405, NSERVERS);
    agree_wait!(st, 406, NSERVERS);

    passed();
    0
}

fn test_fail_no_agree(st: &mut LabState) -> i32 {
    init2!(5, "No agreement if too many followers disconnect");

    let leader = lab::one_leader(-1);
    check_one_leader!(leader);

    lab::disconnect(lab::next_server_id(leader as u32, 1));
    lab::disconnect(lab::next_server_id(leader as u32, 2));
    lab::disconnect(lab::next_server_id(leader as u32, 3));

    let (ok, index, term) = lab::start(leader as u32, 501);
    check_start_ok!(ok);
    let expected = st.index;
    st.index += 1;
    check_msg!(index == expected && term > 0,
        "Start() returned unexpected index ({}, expected {}) and/or term ({}, expected >0)",
        index, expected, term);

    lab::fiber_sleep_us(ELECTION_TIMEOUT_US);
    check_none_committed!(index);

    lab::reconnect(lab::next_server_id(leader as u32, 1));
    lab::reconnect(lab::next_server_id(leader as u32, 2));
    lab::reconnect(lab::next_server_id(leader as u32, 3));

    lab::fiber_sleep_us(ELECTION_TIMEOUT_US);
    agree_wait!(st, 502, NSERVERS);

    passed();
    0
}

fn test_rejoin(st: &mut LabState) -> i32 {
    init2!(6, "Rejoin of disconnected leader");

    agree_at!(601, NSERVERS, st.index); st.index += 1;

    let leader1 = lab::one_leader(-1);
    check_one_leader!(leader1);
    lab::disconnect(leader1 as u32);
    lab::fiber_sleep_us(ELECTION_TIMEOUT_US);

    // the old leader's entries must not commit
    check_start_ok!(lab::start(leader1 as u32, 602).0);
    check_start_ok!(lab::start(leader1 as u32, 603).0);
    check_start_ok!(lab::start(leader1 as u32, 604).0);

    agree_wait!(st, 605, NSERVERS - 1);
    agree_wait!(st, 606, NSERVERS - 1);

    let leader2 = lab::one_leader(-1);
    check_one_leader!(leader2);
    check_msg!(leader2 != leader1, "no reelection despite leader being disconnected");
    lab::disconnect(leader2 as u32);

    lab::reconnect(leader1 as u32);
    lab::fiber_sleep_us(ELECTION_TIMEOUT_US);
    let leader3 = lab::one_leader(-1);
    check_one_leader!(leader3);
    check_msg!(leader3 != leader2, "no reelection despite leader being disconnected");

    agree_wait!(st, 607, NSERVERS - 1);
    agree_wait!(st, 608, NSERVERS - 1);

    lab::reconnect(leader2 as u32);
    agree_wait!(st, 609, NSERVERS);

    passed();
    0
}

fn test_concurrent_starts(st: &mut LabState) -> i32 {
    init2!(7, "Concurrently started agreements");

    let nconcurrent = 5;
    let mut success = false;

    'again: for again in 0..5 {
        if again > 0 {
            wait_us(3_000_000);
        }
        let leader = lab::one_leader(-1);
        check_one_leader!(leader);

        let (ok, _index, term) = lab::start(leader as u32, 701);
        if !ok {
            continue; // retry (up to 5 times)
        }

        // five threads, each Start()ing a command on the leader. The C++ uses
        // pthreads for the same reason and reaches the server the same way:
        // Start takes the server's own mutex.
        let mut handles = Vec::new();
        for i in 0..nconcurrent {
            let ldr = leader as u32;
            handles.push(std::thread::spawn(move || {
                let (ok, idx, tm) = lab::start(ldr, 701 + i);
                if ok && tm == term { Some(idx) } else { None }
            }));
        }
        let indices: Vec<u64> = handles.into_iter()
            .filter_map(|h| h.join().expect("concurrent Start thread panicked"))
            .collect();

        if lab::term_moved_on(term) {
            continue 'again; // leader's term is expiring -- start over
        }

        let mut cmds = Vec::new();
        for index in indices {
            let cmd = lab::wait(index, NSERVERS as i32, term);
            if cmd < 0 {
                check_wait_no_error!(cmd, index);
                continue 'again; // timeout or term change -- try again
            }
            cmds.push(cmd);
        }

        // every value must be there
        for i in 0..nconcurrent {
            let val = (701 + i) as i64;
            check_msg!(cmds.iter().any(|&c| c == val), "cmd {} missing", val);
        }
        success = true;
        break;
    }

    check_msg!(success, "too many term changes and/or delayed responses");
    st.index += nconcurrent as u64 + 1;

    passed();
    0
}

fn test_backup(st: &mut LabState) -> i32 {
    init2!(8, "Leader backs up quickly over incorrect follower logs");

    let leader1 = lab::one_leader(-1);
    check_one_leader!(leader1);

    lab::disconnect(lab::next_server_id(leader1 as u32, 2));
    lab::disconnect(lab::next_server_id(leader1 as u32, 3));
    lab::disconnect(lab::next_server_id(leader1 as u32, 4));

    // 50 commands that will not commit
    for i in 0..50 {
        check_start_ok!(lab::start(leader1 as u32, 800 + i).0);
    }
    lab::fiber_sleep_us(ELECTION_TIMEOUT_US);

    lab::disconnect(lab::next_server_id(leader1 as u32, 1));
    lab::disconnect(leader1 as u32);
    lab::reconnect(lab::next_server_id(leader1 as u32, 2));
    lab::reconnect(lab::next_server_id(leader1 as u32, 3));
    lab::reconnect(lab::next_server_id(leader1 as u32, 4));

    lab::fiber_sleep_us(ELECTION_TIMEOUT_US);
    for i in 1..=50 {
        agree_at!(800 + i, NSERVERS - 2, st.index);
        st.index += 1;
    }

    lab::reconnect(lab::next_server_id(leader1 as u32, 1));
    lab::reconnect(leader1 as u32);
    lab::fiber_sleep_us(ELECTION_TIMEOUT_US);

    let leader2 = lab::one_leader(-1);
    check_one_leader!(leader2);
    let (ok, index, _term) = lab::start(leader2 as u32, 851);
    check_start_ok!(ok);
    st.index += 1;

    // 10 seconds is enough to back up 50 incorrect logs
    lab::fiber_sleep_us(2 * ELECTION_TIMEOUT_US);
    check_n_committed!(index, NSERVERS);

    passed();
    0
}

fn test_count(st: &mut LabState) -> i32 {
    init2!(9, "RPC counts aren't too high");

    for i in 0..NSERVERS {
        lab::rpc_count(lab::server_id_by_index(i), true);
    }
    let rpcs = || -> u64 {
        (0..NSERVERS).map(|i| lab::rpc_count(lab::server_id_by_index(i), true)).sum()
    };

    // Ceiling raised from 40 to 70 for Mako-specific traffic the upstream
    // MIT 6.824 reference did not emit; see the note in test.cc.
    check_msg!(st.init_rpcs > 1 && st.init_rpcs <= 70,
               "too many or too few RPCs ({}) to elect initial leader", st.init_rpcs);

    let iters: u64 = 10;
    let mut total;
    let mut success = false;

    'again: for again in 0..5 {
        if again > 0 {
            wait_us(3_000_000);
        }
        let leader = lab::one_leader(-1);
        check_one_leader!(leader);
        rpcs();

        let (ok, startindex, startterm) = lab::start(leader as u32, 900);
        if !ok {
            continue; // leader moved on quickly: start over
        }
        for i in 1..=iters {
            let (ok, index, term) = lab::start(leader as u32, 900 + i as i32);
            if !ok || term != startterm {
                continue 'again;
            }
            check_msg!(index == startindex + i, "Start() failed");
        }
        for i in 1..=iters {
            let r = lab::wait(startindex + i, NSERVERS as i32, startterm);
            check_wait_no_error!(r, startindex + i);
            if r < 0 {
                continue 'again;
            }
            check_msg!(r == (900 + i) as i64,
                       "wrong value {} committed for index {}: expected {}",
                       r, startindex + i, 900 + i);
        }
        if lab::term_moved_on(startterm) {
            continue; // term changed -- can't expect low RPC counts
        }
        total = rpcs();
        // COMMITRPCS(n) == (n + 1) * NSERVERS  (testconf.h:27)
        check_msg!(total <= (iters + 1) * NSERVERS as u64,
                   "too many RPCs ({}) for {} entries", total, iters);
        success = true;
        break;
    }
    check_msg!(success, "term changed too often");

    // idle RPC count
    wait_us(1_000_000);
    total = rpcs();
    check_msg!(total <= 60, "too many RPCs ({}) for 1 second of idleness", total);

    passed();
    0
}

fn test_unreliable_agree(st: &mut LabState) -> i32 {
    init2!(10, "Unreliable agreement (takes a few minutes)");

    lab::set_unreliable(true);
    let mut handles = Vec::new();
    let mut failures: Vec<u64> = Vec::new();

    for iter in 1..50 {
        for _ in 0..4 {
            handles.push(std::thread::spawn(move || {
                lab::do_agreement(1000 + iter, 1, true)
            }));
        }
        if !failures.is_empty() {
            break;
        }
        if lab::do_agreement(1000 + iter, 1, true) == 0 {
            failures.push(0);
            break;
        }
    }
    lab::set_unreliable(false);

    for handle in handles {
        // The C++ collects a zero return from each thread as a failure; a
        // panic in one would be a harness bug either way.
        if handle.join().expect("unreliable agreement thread panicked") == 0 {
            failures.push(0);
        }
    }

    check_msg!(failures.is_empty(), "Failed to reach agreement");
    st.index += 50 * 5;
    agree_wait!(st, 1060, NSERVERS);

    passed();
    0
}

fn test_figure8(st: &mut LabState) -> i32 {
    init2!(11, "Figure 8");

    let mut success = false;

    // A leader must not determine commitment using entries from earlier terms
    for _again in 0..10 {
        let leader1 = lab::one_leader(-1);
        check_one_leader!(leader1);

        let (ok, mut index1, mut term1) = lab::start(leader1 as u32, 1100);
        if !ok {
            continue; // term moved on too quickly: start over
        }
        let r = lab::wait(index1, NSERVERS as i32, term1);
        check_wait_no_error!(r, index1);
        check_wait_no_timeout!(r, index1, NSERVERS);
        st.index = index1;

        // C1 replicates to one follower only
        lab::disconnect(lab::next_server_id(leader1 as u32, 1));
        lab::disconnect(lab::next_server_id(leader1 as u32, 2));
        lab::disconnect(lab::next_server_id(leader1 as u32, 3));
        let started = lab::start(leader1 as u32, 1101);
        if !started.0 {
            lab::reconnect(lab::next_server_id(leader1 as u32, 1));
            lab::reconnect(lab::next_server_id(leader1 as u32, 2));
            lab::reconnect(lab::next_server_id(leader1 as u32, 3));
            continue;
        }
        index1 = started.1;
        term1 = started.2;
        lab::fiber_sleep_us(ELECTION_TIMEOUT_US);
        check_none_committed!(index1);

        // elect a new leader among the other three
        lab::disconnect(lab::next_server_id(leader1 as u32, 4));
        lab::disconnect(leader1 as u32);
        lab::reconnect(lab::next_server_id(leader1 as u32, 1));
        lab::reconnect(lab::next_server_id(leader1 as u32, 2));
        lab::reconnect(lab::next_server_id(leader1 as u32, 3));
        let leader2 = lab::one_leader(-1);
        check_one_leader!(leader2);

        // the old leader and its follower become followers in the new term
        lab::reconnect(lab::next_server_id(leader1 as u32, 4));
        lab::reconnect(leader1 as u32);
        lab::fiber_sleep_us(ELECTION_TIMEOUT_US);
        check_one_leader!(lab::one_leader(leader2));

        // isolate the new leader and Start() C2 on it
        for i in 0..NSERVERS {
            let server_id = lab::server_id_by_index(i);
            if server_id != leader2 as u32 {
                lab::disconnect(server_id);
            }
        }
        let (ok2, index2, term2) = lab::start(leader2 as u32, 1102);
        if !ok2 {
            for i in 1..5 {
                lab::reconnect(lab::next_server_id(leader2 as u32, i));
            }
            continue;
        }
        check_msg!(index2 == index1, "Start() returned index {} ({} expected)", index2, index1);
        check_msg!(term2 > term1, "Start() returned term {} ({} expected)", term2, term1);
        lab::fiber_sleep_us(ELECTION_TIMEOUT_US);
        check_none_committed!(index1);

        // let the first leader or its follower become the next leader
        lab::disconnect(leader2 as u32);
        lab::reconnect(leader1 as u32);
        assert_ne!(lab::next_server_id(leader1 as u32, 4), leader2 as u32);
        lab::reconnect(lab::next_server_id(leader1 as u32, 4));
        if leader2 as u32 == lab::next_server_id(leader1 as u32, 1) {
            lab::reconnect(lab::next_server_id(leader1 as u32, 2));
        } else {
            lab::reconnect(lab::next_server_id(leader1 as u32, 1));
        }
        let leader3 = lab::one_leader(-1);
        check_one_leader!(leader3);
        if leader3 as u32 != leader1 as u32
            && leader3 as u32 != lab::next_server_id(leader1 as u32, 4)
        {
            continue; // 1/3 chance of failing this step; start over
        }

        // enough time to replicate index1 to a third server
        lab::fiber_sleep_us(ELECTION_TIMEOUT_US);
        check_none_committed!(index1);

        // commit a new index in the current term
        check_msg!(lab::do_agreement(1103, (NSERVERS - 2) as i32, false) > index1,
                   "failed to reach agreement");
        check_n_committed!(index1, NSERVERS - 2);
        check_msg!(lab::server_committed(leader3 as u32, index1, 1101),
                   "value 1101 is not committed at index {} when it should be", index1);
        success = true;

        lab::reconnect(lab::next_server_id(leader1 as u32, 3));
        if leader2 as u32 == lab::next_server_id(leader1 as u32, 1) {
            lab::reconnect(lab::next_server_id(leader1 as u32, 1));
        } else {
            lab::reconnect(lab::next_server_id(leader1 as u32, 2));
        }
        break;
    }

    check_msg!(success, "Failed to test figure 8");
    passed();
    0
}

// ---------------------------------------------------------------------------
// The driver. Same order and same short-circuit structure as
// RaftLabTest::Run, so a failure stops at the same place.

pub fn run() -> i32 {
    eprintln!("Starting Raft lab tests (Rust harness)");
    let mut st = LabState { index: 1, init_rpcs: 0 };
    let start_rpc = lab::rpc_total();

    assert_eq!(crate::server_h::lab_registry::count(), NSERVERS,
               "the Rust harness needs all five replicas registered");

    type Case = fn(&mut LabState) -> i32;
    let basic: &[Case] = &[
        test_initial_election,
        test_re_election,
        test_basic_agree,
        test_fail_agree,
        test_fail_no_agree,
        test_rejoin,
        test_concurrent_starts,
        test_backup,
        test_count,
        test_unreliable_agree,
        test_figure8,
    ];

    for case in basic {
        if case(&mut st) != 0 {
            eprintln!("TESTS FAILED");
            return 1;
        }
    }

    if crate::lab_snapshot_cases::run(&mut st) != 0 {
        eprintln!("TESTS FAILED");
        return 1;
    }

    eprintln!("ALL TESTS PASSED");
    eprintln!("Total RPC count: {}", lab::rpc_total() - start_rpc);
    0
}
