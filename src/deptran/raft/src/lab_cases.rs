// The replication half of the RaftLab suite: elections, agreement and
// partitions -- eleven of the twenty-five cases.
//
// Each case returns 0 on success and 1 on failure, and prints the markers CI
// greps for -- `TEST <n>: <description>`, `TEST <n> Passed`, and finally
// `ALL TESTS PASSED`. `run` at the bottom sequences them and short-circuits on
// the first failure, so a break leaves the cluster in the state that produced
// it.
//
// Everything a case reads is a method on RaftServerBase, reached through the
// fixture in lab.rs. The snapshot, configuration and load families are in
// lab_snapshot_cases.rs.


use crate::lab;
use crate::lab::{ELECTION_TIMEOUT_US, NSERVERS};
use std::sync::atomic::{AtomicI32, Ordering};

// ---------------------------------------------------------------------------
// Reporting. ci.sh greps stderr for `^TEST [0-9]* Passed` and counts them, and
// for `ALL TESTS PASSED`, so these three formats are load-bearing.

static TEST_ID: AtomicI32 = AtomicI32::new(0);

pub fn init(test_id: i32, description: &str) {
    eprintln!("TEST {test_id}: {description}");
    TEST_ID.store(test_id, Ordering::Relaxed);
}

pub fn failed(msg: &str) {
    lab::dump_commit_log();  // [M0] inert unless MAKO_RAFT_LAB_COMMIT_LOG=1
    eprintln!("TEST {} Failed: {}", TEST_ID.load(Ordering::Relaxed), msg);
}

pub fn passed() {
    lab::dump_commit_log();  // [M0] inert unless MAKO_RAFT_LAB_COMMIT_LOG=1
    eprintln!("TEST {} Passed", TEST_ID.load(Ordering::Relaxed));
}

// ---------------------------------------------------------------------------
// The assertion helpers. Each was a macro_rules! macro; they are functions
// because the C++ lane transpiles this harness, and the transpiler lowers a
// custom macro invocation to a `// TODO` comment -- 189 of them, which made
// the transpiled lab report 25/25 while checking nothing (plan L2). Each
// returns false when the check fails, and the caller returns 1, exactly the
// early exit the macro performed.

/// `Init2`: every case starts with the network whole.
pub fn init2(id: i32, desc: &str) {
    init(id, desc);
    assert!(lab::n_disconnected() == 0 && !lab::is_unreliable(),
            "case {} started on a network a previous case left broken", id);
}

/// `Assert2`: report `msg` when `cond` fails.
pub fn check_msg(cond: bool, msg: &str) -> bool {
    if !cond {
        failed(msg);
    }
    cond
}

/// `AssertNoneCommitted`.
pub fn check_none_committed(index: u64) -> bool {
    let nc = lab::n_committed(index);
    check_msg(nc == 0, &format!("{} servers unexpectedly committed index {}", nc, index))
}

/// `AssertNCommitted`.
pub fn check_n_committed(index: u64, expected: i32) -> bool {
    let nc = lab::n_committed(index);
    check_msg(nc == expected,
              &format!("{} servers committed index {} ({} expected)", nc, index, expected))
}

/// `AssertWaitNoError`.
pub fn check_wait_no_error(ret: i64, index: u64) -> bool {
    check_msg(ret != lab::WAIT_VALUES_DIFFER,
              &format!("committed values differ for index {}", index))
}

/// `AssertWaitNoTimeout`.
pub fn check_wait_no_timeout(ret: i64, index: u64, n: i32) -> bool {
    check_msg(ret != lab::WAIT_TIMEOUT,
              &format!("waited too long for {} server(s) to commit index {}", n, index))
        && check_msg(ret != lab::WAIT_TERM_MOVED,
                     &format!("term moved on before index {} committed by {} server(s)",
                              index, n))
}

/// `DoAgreeAndAssertIndex`.
pub fn agree_at(cmd: i32, n: i32, index: u64) -> bool {
    let r = lab::do_agreement(cmd, n, false);
    check_msg(r > 0, &format!(
        "failed to reach agreement for command {} among {} servers, expected commit index>0, got {}",
        cmd, n, r))
        && check_msg(r == index,
                     &format!("agreement index incorrect. got {}, expected {}", r, index))
}

/// `DoAgreeAndAssertWaitSuccess`.
pub fn agree_wait(st: &mut LabState, cmd: i32, n: i32) -> bool {
    let r = lab::do_agreement(cmd, n, true);
    if !check_msg(r > 0,
                  &format!("failed to reach agreement for command {} among {} servers", cmd, n)) {
        return false;
    }
    st.index = r + 1;
    true
}












/// `RaftLabTest::wait` -- a timeout event the C++ waits on. On this side the
/// fiber sleep is the same wait.
fn wait_us(micros: u64) {
    lab::fiber_sleep_us(micros);
}

/// The two numbers RaftLabTest carries between cases (test.h:13-14).
/// Copy, and handed between the lab modules by value: the transpiled C++
/// lane lowers a cross-module `&mut` argument as a pointer.
#[derive(Clone, Copy)]
pub struct LabState {
    pub index: u64,
    pub init_rpcs: u64,
}

// ---------------------------------------------------------------------------
// Elections

fn test_initial_election(st: &mut LabState) -> i32 {
    init2(1, "Initial election");

    // Wait for election timers to start and elections to begin
    lab::fiber_sleep_us(ELECTION_TIMEOUT_US / 10);

    let leader = lab::one_leader(-1);
    if (leader) < 0 { return 1; }

    // The RPC count the initial election cost, for testCount below
    st.init_rpcs = 0;
    for i in 0..NSERVERS {
        st.init_rpcs += lab::rpc_count(lab::server_id_by_index(i), true);
    }

    let term = lab::one_term();
    if !check_msg(term != lab::TERM_DISAGREE, "servers disagree on term number") { return 1; }
    if !check_msg(lab::one_term() == term, "unexpected term change") { return 1; }
    if (lab::one_leader(leader)) < 0 { return 1; }

    passed();
    0
}

fn test_re_election(_st: &mut LabState) -> i32 {
    init2(2, "Re-election after network failure");

    let mut leader = lab::one_leader(-1);
    if leader == -1 {
        failed("No leader found in initial election");
        return -1;
    }
    if (leader) < 0 { return 1; }

    // disconnect leader -- make sure a new one is elected
    lab::disconnect(leader as u32);
    let old_leader = leader;
    lab::fiber_sleep_us(ELECTION_TIMEOUT_US);

    leader = lab::one_leader(-1);
    if leader == -1 {
        failed("No new leader elected after disconnecting old leader");
        return -1;
    }
    if (leader) < 0 { return 1; }
    if !check_msg(leader != old_leader, "no reelection despite leader being disconnected") { return 1; }

    // reconnect old leader -- should not disturb new leader
    lab::reconnect(old_leader as u32);
    lab::fiber_sleep_us(ELECTION_TIMEOUT_US);
    if (lab::one_leader(leader)) < 0 { return 1; }

    // no quorum -> no leader
    lab::disconnect(lab::next_server_id(leader as u32, 1));
    lab::disconnect(lab::next_server_id(leader as u32, 2));
    lab::disconnect(leader as u32);
    if !(lab::no_leader()) { return 1; }

    // quorum restored
    lab::reconnect(lab::next_server_id(leader as u32, 2));
    lab::fiber_sleep_us(ELECTION_TIMEOUT_US);
    if (lab::one_leader(-1)) < 0 { return 1; }

    // rejoin all servers
    lab::reconnect(lab::next_server_id(leader as u32, 1));
    lab::reconnect(leader as u32);
    lab::fiber_sleep_us(ELECTION_TIMEOUT_US);
    if (lab::one_leader(-1)) < 0 { return 1; }

    passed();
    0
}

// ---------------------------------------------------------------------------
// Agreement

fn test_basic_agree(st: &mut LabState) -> i32 {
    init2(3, "Basic agreement");

    for _ in 1..=3 {
        // no commits before any agreement is started
        if !check_none_committed(st.index) { return 1; }
        let command_value = (st.index + 300) as i32;
        if !agree_at(command_value, NSERVERS as i32, st.index) { return 1; }
        st.index += 1;
    }

    passed();
    0
}

fn test_fail_agree(st: &mut LabState) -> i32 {
    init2(4, "Agreement despite follower disconnection");

    let leader = lab::one_leader(-1);
    if (leader) < 0 { return 1; }

    lab::disconnect(lab::next_server_id(leader as u32, 1));
    lab::disconnect(lab::next_server_id(leader as u32, 2));

    // agreement despite 2 disconnected servers
    if !agree_at(401, (NSERVERS - 2) as i32, st.index) { return 1; } st.index += 1;
    if !agree_at(402, (NSERVERS - 2) as i32, st.index) { return 1; } st.index += 1;
    lab::fiber_sleep_us(ELECTION_TIMEOUT_US);
    if !agree_at(403, (NSERVERS - 2) as i32, st.index) { return 1; } st.index += 1;
    if !agree_at(404, (NSERVERS - 2) as i32, st.index) { return 1; } st.index += 1;

    lab::reconnect(lab::next_server_id(leader as u32, 1));
    lab::reconnect(lab::next_server_id(leader as u32, 2));
    lab::fiber_sleep_us(ELECTION_TIMEOUT_US);

    if !agree_wait(st, 405, NSERVERS as i32) { return 1; }
    if !agree_wait(st, 406, NSERVERS as i32) { return 1; }

    passed();
    0
}

fn test_fail_no_agree(st: &mut LabState) -> i32 {
    init2(5, "No agreement if too many followers disconnect");

    let leader = lab::one_leader(-1);
    if (leader) < 0 { return 1; }

    lab::disconnect(lab::next_server_id(leader as u32, 1));
    lab::disconnect(lab::next_server_id(leader as u32, 2));
    lab::disconnect(lab::next_server_id(leader as u32, 3));

    let (ok, index, term) = lab::start(leader as u32, 501);
    if !check_msg(ok, "unexpected leader change during Start()") { return 1; }
    let expected = st.index;
    st.index += 1;
    if !check_msg(index == expected && term > 0, &format!("Start() returned unexpected index ({}, expected {}) and/or term ({}, expected >0)", index, expected, term)) { return 1; }

    lab::fiber_sleep_us(ELECTION_TIMEOUT_US);
    if !check_none_committed(index) { return 1; }

    lab::reconnect(lab::next_server_id(leader as u32, 1));
    lab::reconnect(lab::next_server_id(leader as u32, 2));
    lab::reconnect(lab::next_server_id(leader as u32, 3));

    lab::fiber_sleep_us(ELECTION_TIMEOUT_US);
    if !agree_wait(st, 502, NSERVERS as i32) { return 1; }

    passed();
    0
}

fn test_rejoin(st: &mut LabState) -> i32 {
    init2(6, "Rejoin of disconnected leader");

    if !agree_at(601, NSERVERS as i32, st.index) { return 1; } st.index += 1;

    let leader1 = lab::one_leader(-1);
    if (leader1) < 0 { return 1; }
    lab::disconnect(leader1 as u32);
    lab::fiber_sleep_us(ELECTION_TIMEOUT_US);

    // the old leader's entries must not commit
    if !check_msg(lab::start(leader1 as u32, 602).0, "unexpected leader change during Start()") { return 1; }
    if !check_msg(lab::start(leader1 as u32, 603).0, "unexpected leader change during Start()") { return 1; }
    if !check_msg(lab::start(leader1 as u32, 604).0, "unexpected leader change during Start()") { return 1; }

    if !agree_wait(st, 605, (NSERVERS - 1) as i32) { return 1; }
    if !agree_wait(st, 606, (NSERVERS - 1) as i32) { return 1; }

    let leader2 = lab::one_leader(-1);
    if (leader2) < 0 { return 1; }
    if !check_msg(leader2 != leader1, "no reelection despite leader being disconnected") { return 1; }
    lab::disconnect(leader2 as u32);

    lab::reconnect(leader1 as u32);
    lab::fiber_sleep_us(ELECTION_TIMEOUT_US);
    let leader3 = lab::one_leader(-1);
    if (leader3) < 0 { return 1; }
    if !check_msg(leader3 != leader2, "no reelection despite leader being disconnected") { return 1; }

    if !agree_wait(st, 607, (NSERVERS - 1) as i32) { return 1; }
    if !agree_wait(st, 608, (NSERVERS - 1) as i32) { return 1; }

    lab::reconnect(leader2 as u32);
    if !agree_wait(st, 609, NSERVERS as i32) { return 1; }

    passed();
    0
}

fn test_concurrent_starts(st: &mut LabState) -> i32 {
    init2(7, "Concurrently started agreements");

    let nconcurrent = 5;
    let mut success = false;

    'again: for again in 0..5 {
        if again > 0 {
            wait_us(3_000_000);
        }
        let leader = lab::one_leader(-1);
        if (leader) < 0 { return 1; }

        let (ok, _index, term) = lab::start(leader as u32, 701);
        if !ok {
            continue; // retry (up to 5 times)
        }

        // five threads, each Start()ing a command on the leader. The C++ uses
        // pthreads for the same reason and reaches the server the same way:
        // Start takes the server's own mutex.
        // Element type spelled out: the transpiler cannot infer it from the
        // later push.
        let mut handles: Vec<std::thread::JoinHandle<Option<u64>>> = Vec::new();
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

        // Typed explicitly: left to inference, the transpiler lowered this to
        // Vec<bool>, and every committed value compared equal to `true`.
        let mut cmds: Vec<i64> = Vec::new();
        for index in indices {
            let cmd = lab::wait(index, NSERVERS as i32, term);
            if cmd < 0 {
                if !check_wait_no_error(cmd, index) { return 1; }
                continue 'again; // timeout or term change -- try again
            }
            cmds.push(cmd);
        }

        // every value must be there
        for i in 0..nconcurrent {
            let val = (701 + i) as i64;
            if !check_msg(cmds.contains(&val), &format!("cmd {} missing", val)) { return 1; }
        }
        success = true;
        break;
    }

    if !check_msg(success, "too many term changes and/or delayed responses") { return 1; }
    st.index += nconcurrent as u64 + 1;

    passed();
    0
}

fn test_backup(st: &mut LabState) -> i32 {
    init2(8, "Leader backs up quickly over incorrect follower logs");

    let leader1 = lab::one_leader(-1);
    if (leader1) < 0 { return 1; }

    lab::disconnect(lab::next_server_id(leader1 as u32, 2));
    lab::disconnect(lab::next_server_id(leader1 as u32, 3));
    lab::disconnect(lab::next_server_id(leader1 as u32, 4));

    // 50 commands that will not commit
    for i in 0..50 {
        if !check_msg(lab::start(leader1 as u32, 800 + i).0, "unexpected leader change during Start()") { return 1; }
    }
    lab::fiber_sleep_us(ELECTION_TIMEOUT_US);

    lab::disconnect(lab::next_server_id(leader1 as u32, 1));
    lab::disconnect(leader1 as u32);
    lab::reconnect(lab::next_server_id(leader1 as u32, 2));
    lab::reconnect(lab::next_server_id(leader1 as u32, 3));
    lab::reconnect(lab::next_server_id(leader1 as u32, 4));

    lab::fiber_sleep_us(ELECTION_TIMEOUT_US);
    for i in 1..=50 {
        if !agree_at(800 + i, (NSERVERS - 2) as i32, st.index) { return 1; }
        st.index += 1;
    }

    lab::reconnect(lab::next_server_id(leader1 as u32, 1));
    lab::reconnect(leader1 as u32);
    lab::fiber_sleep_us(ELECTION_TIMEOUT_US);

    let leader2 = lab::one_leader(-1);
    if (leader2) < 0 { return 1; }
    let (ok, index, _term) = lab::start(leader2 as u32, 851);
    if !check_msg(ok, "unexpected leader change during Start()") { return 1; }
    st.index += 1;

    // 10 seconds is enough to back up 50 incorrect logs
    lab::fiber_sleep_us(2 * ELECTION_TIMEOUT_US);
    if !check_n_committed(index, NSERVERS as i32) { return 1; }

    passed();
    0
}

fn test_count(st: &mut LabState) -> i32 {
    init2(9, "RPC counts aren't too high");

    for i in 0..NSERVERS {
        lab::rpc_count(lab::server_id_by_index(i), true);
    }
    let rpcs = || -> u64 {
        (0..NSERVERS).map(|i| lab::rpc_count(lab::server_id_by_index(i), true)).sum()
    };

    // Ceiling raised from 40 to 70 for Mako-specific traffic the upstream
    // MIT 6.824 reference implementation did not emit.
    if !check_msg(st.init_rpcs > 1 && st.init_rpcs <= 70, &format!("too many or too few RPCs ({}) to elect initial leader", st.init_rpcs)) { return 1; }

    let iters: u64 = 10;
    let mut success = false;

    'again: for again in 0..5 {
        if again > 0 {
            wait_us(3_000_000);
        }
        let leader = lab::one_leader(-1);
        if (leader) < 0 { return 1; }
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
            if !check_msg(index == startindex + i, "Start() failed") { return 1; }
        }
        for i in 1..=iters {
            let r = lab::wait(startindex + i, NSERVERS as i32, startterm);
            if !check_wait_no_error(r, startindex + i) { return 1; }
            if r < 0 {
                continue 'again;
            }
            if !check_msg(r == (900 + i) as i64, &format!("wrong value {} committed for index {}: expected {}", r, startindex + i, 900 + i)) { return 1; }
        }
        if lab::term_moved_on(startterm) {
            continue; // term changed -- can't expect low RPC counts
        }
        // A block of its own: `continue 'again` lowers to a goto past the end
        // of this body, which C++ forbids across an initialised local.
        {
            let total = rpcs();
            // COMMITRPCS(n) == (n + 1) * NSERVERS  (testconf.h:27)
            if !check_msg(total <= (iters + 1) * NSERVERS as u64, &format!("too many RPCs ({}) for {} entries", total, iters)) { return 1; }
        }
        success = true;
        break;
    }
    if !check_msg(success, "term changed too often") { return 1; }

    // idle RPC count
    wait_us(1_000_000);
    let total = rpcs();
    if !check_msg(total <= 60, &format!("too many RPCs ({}) for 1 second of idleness", total)) { return 1; }

    passed();
    0
}

fn test_unreliable_agree(st: &mut LabState) -> i32 {
    init2(10, "Unreliable agreement (takes a few minutes)");

    lab::set_unreliable(true);
    let mut handles: Vec<std::thread::JoinHandle<u64>> = Vec::new();
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

    if !check_msg(failures.is_empty(), "Failed to reach agreement") { return 1; }
    st.index += 50 * 5;
    if !agree_wait(st, 1060, NSERVERS as i32) { return 1; }

    passed();
    0
}

fn test_figure8(st: &mut LabState) -> i32 {
    init2(11, "Figure 8");

    let mut success = false;

    // A leader must not determine commitment using entries from earlier terms
    for _again in 0..10 {
        let leader1 = lab::one_leader(-1);
        if (leader1) < 0 { return 1; }

        let (ok, mut index1, mut term1) = lab::start(leader1 as u32, 1100);
        if !ok {
            continue; // term moved on too quickly: start over
        }
        let r = lab::wait(index1, NSERVERS as i32, term1);
        if !check_wait_no_error(r, index1) { return 1; }
        if !check_wait_no_timeout(r, index1, NSERVERS as i32) { return 1; }
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
        if !check_none_committed(index1) { return 1; }

        // elect a new leader among the other three
        lab::disconnect(lab::next_server_id(leader1 as u32, 4));
        lab::disconnect(leader1 as u32);
        lab::reconnect(lab::next_server_id(leader1 as u32, 1));
        lab::reconnect(lab::next_server_id(leader1 as u32, 2));
        lab::reconnect(lab::next_server_id(leader1 as u32, 3));
        let leader2 = lab::one_leader(-1);
        if (leader2) < 0 { return 1; }

        // the old leader and its follower become followers in the new term
        lab::reconnect(lab::next_server_id(leader1 as u32, 4));
        lab::reconnect(leader1 as u32);
        lab::fiber_sleep_us(ELECTION_TIMEOUT_US);
        if (lab::one_leader(leader2)) < 0 { return 1; }

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
        if !check_msg(index2 == index1, &format!("Start() returned index {} ({} expected)", index2, index1)) { return 1; }
        if !check_msg(term2 > term1, &format!("Start() returned term {} ({} expected)", term2, term1)) { return 1; }
        lab::fiber_sleep_us(ELECTION_TIMEOUT_US);
        if !check_none_committed(index1) { return 1; }

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
        if (leader3) < 0 { return 1; }
        if leader3 as u32 != leader1 as u32
            && leader3 as u32 != lab::next_server_id(leader1 as u32, 4)
        {
            continue; // 1/3 chance of failing this step; start over
        }

        // enough time to replicate index1 to a third server
        lab::fiber_sleep_us(ELECTION_TIMEOUT_US);
        if !check_none_committed(index1) { return 1; }

        // commit a new index in the current term
        if !check_msg(lab::do_agreement(1103, (NSERVERS - 2) as i32, false) > index1, "failed to reach agreement") { return 1; }
        if !check_n_committed(index1, (NSERVERS - 2) as i32) { return 1; }
        if !check_msg(lab::server_committed(leader3 as u32, index1, 1101), &format!("value 1101 is not committed at index {} when it should be", index1)) { return 1; }
        success = true;

        lab::reconnect(lab::next_server_id(leader1 as u32, 3));
        if leader2 as u32 == lab::next_server_id(leader1 as u32, 1) {
            lab::reconnect(lab::next_server_id(leader1 as u32, 1));
        } else {
            lab::reconnect(lab::next_server_id(leader1 as u32, 2));
        }
        break;
    }

    if !check_msg(success, "Failed to test figure 8") { return 1; }
    passed();
    0
}

// ---------------------------------------------------------------------------
// [fix, F4] Entry terms

// An AppendEntries whose entry carries term 0 -- no Raft term; the spec's
// B16 -- is refused and leaves the follower's log alone. The same append
// without its entry is accepted first, so the refusal can only be the
// entry's term.
fn test_entry_term_zero_refused(_st: &mut LabState) -> i32 {
    init2(12, "AppendEntries carrying an entry of term 0 is refused");

    let leader = lab::one_leader(-1);
    if !check_msg(leader >= 0, "no leader") { return 1; }
    let mut follower: i32 = -1;
    let mut i: i32 = 0;
    while i < NSERVERS as i32 {
        if i != leader {
            follower = i;
            break;
        }
        i += 1;
    }
    if !check_msg(follower >= 0, "no follower") { return 1; }
    let follower = follower as u32;
    let Some(leader_site) = lab::site_id_of(leader as u32) else {
        failed("leader not registered"); return 1;
    };

    let Some((term, last, last_term, commit, zero_before)) = lab::log_tail(follower) else {
        failed("follower not registered"); return 1;
    };
    if !check_msg(!zero_before, "the follower's log already holds a term-0 slot") { return 1; }

    // Control: the same prev, term and leader with no entry is accepted.
    // (Each reply gets its own name: the transpiled C++ cannot redeclare one
    // in the same scope, as a Rust `let` can.)
    let Some((control_ok, _, _)) = lab::serve_append(follower, term, leader_site, last,
                                                     last_term, commit, 0, -1) else {
        failed("follower not registered"); return 1;
    };
    if !check_msg(control_ok == 1, "the entry-less control append was refused") { return 1; }

    // The probe: one entry, term 0, right after the follower's last entry.
    let Some((probe_ok, _, _)) = lab::serve_append(follower, term, leader_site, last,
                                                   last_term, commit, 0, 1200) else {
        failed("follower not registered"); return 1;
    };
    if !check_msg(probe_ok == 0, "an entry of term 0 was accepted") { return 1; }
    let Some((_, _, _, _, zero_after)) = lab::log_tail(follower) else {
        failed("follower not registered"); return 1;
    };
    if !check_msg(!zero_after, "a term-0 entry reached the follower's log") { return 1; }

    passed();
    0
}

// ---------------------------------------------------------------------------
// Pinned behaviour (no change): the unavailable voter's reply

// A replica that cannot serve (disconnected, or not yet RPC-ready: the same
// branch of ServeVote) answers a vote request with "no" at the candidate's
// own term -- a reply no voter decided, which the verification reads as
// unmodelled input (docs/verus/modification-plan.md §4.3, V3). Pinned here,
// including that the replica's own term does not move.
fn test_unavailable_voter_reply(_st: &mut LabState) -> i32 {
    init2(13, "An unavailable replica refuses a vote at the candidate's term");

    let leader = lab::one_leader(-1);
    if !check_msg(leader >= 0, "no leader") { return 1; }
    let voter = lab::next_server_id(leader as u32, 1);
    let Some(candidate_site) = lab::site_id_of(lab::next_server_id(leader as u32, 2)) else {
        failed("candidate not registered"); return 1;
    };
    let Some((term_before, last, last_term, _, _)) = lab::log_tail(voter) else {
        failed("voter not registered"); return 1;
    };

    lab::disconnect(voter);
    let can_term = term_before as i64 + 5;
    let reply = lab::serve_vote(voter, last, last_term as i64, candidate_site, can_term);
    let after = lab::log_tail(voter);
    lab::reconnect(voter);

    let Some((reply_term, granted)) = reply else {
        failed("voter not registered"); return 1;
    };
    if !check_msg(granted == 0, "an unavailable replica granted a vote") { return 1; }
    if !check_msg(reply_term == can_term,
                  "the unavailable reply should carry the candidate's own term") { return 1; }
    let Some((term_after, _, _, _, _)) = after else {
        failed("voter not registered"); return 1;
    };
    if !check_msg(term_after == term_before,
                  "an unavailable replica adopted the candidate's term") { return 1; }

    // Let the cluster settle before the next case.
    if !check_msg(lab::one_leader(-1) >= 0, "no leader after reconnecting") { return 1; }
    passed();
    0
}

// ---------------------------------------------------------------------------
// [fix, F9] Messages the core does not take

// An AppendEntries from a site outside the configuration, one at term 0, and
// one whose prev index is 0 but whose prev term is not; a RequestVote from
// outside the configuration, and one at term 0. Each is dropped: the replica
// answers as an unavailable replica answers (an append 0/0/0, a vote "no"
// at the candidate's own term), and its term, log and commit index do not
// move. Before F9 the first two appends and both votes were refused with the
// replica's own term, and the third append was accepted. The real leader's
// entry-less append is accepted, as a control.
fn test_unadmitted_messages_dropped(_st: &mut LabState) -> i32 {
    init2(15, "Messages from outside the configuration, or malformed, are dropped");

    let leader = lab::one_leader(-1);
    if !check_msg(leader >= 0, "no leader") { return 1; }
    let follower = lab::next_server_id(leader as u32, 1);
    let Some(leader_site) = lab::site_id_of(leader as u32) else {
        failed("leader not registered"); return 1;
    };
    let Some(candidate_site) = lab::site_id_of(lab::next_server_id(leader as u32, 2)) else {
        failed("candidate not registered"); return 1;
    };
    // No replica of the lab's partition has this site id.
    let stranger: u16 = 4000;
    let Some((term, last, last_term, commit, _)) = lab::log_tail(follower) else {
        failed("follower not registered"); return 1;
    };

    let Some((control_ok, _, _)) = lab::serve_append(follower, term, leader_site, last,
                                                     last_term, commit, 0, -1) else {
        failed("follower not registered"); return 1;
    };
    if !check_msg(control_ok == 1, "the leader's entry-less control append was refused") {
        return 1;
    }
    let appends: [(u64, u16, u64, u64); 3] = [
        (term, stranger, last, last_term),  // outside the configuration
        (0, leader_site, last, last_term),  // term 0
        (term, leader_site, 0, 1),          // prev 0 with a prev term
    ];
    for (k, (t, site, prev, prev_term)) in appends.iter().enumerate() {
        let Some(reply) = lab::serve_append(follower, *t, *site, *prev, *prev_term,
                                            commit, 0, -1) else {
            failed("follower not registered"); return 1;
        };
        if !check_msg(reply == (0, 0, 0),
                      &format!("append {} was answered {:?}, not dropped", k, reply)) {
            return 1;
        }
    }
    let votes: [(u16, i64); 2] = [
        (stranger, term as i64 + 5),  // outside the configuration
        (candidate_site, 0),          // term 0
    ];
    for (k, (site, can_term)) in votes.iter().enumerate() {
        let Some(reply) = lab::serve_vote(follower, last, last_term as i64, *site,
                                          *can_term) else {
            failed("follower not registered"); return 1;
        };
        if !check_msg(reply == (*can_term, 0),
                      &format!("vote {} was answered {:?}, not dropped", k, reply)) {
            return 1;
        }
    }
    // A refusal the core made would carry the follower's own term, which is
    // at least 1, so 0/0/0 and a "no" at the probe's term can only be drops.
    // A taken vote request at term + 5 would also have moved the term.
    let Some((term_after, _, _, _, _)) = lab::log_tail(follower) else {
        failed("follower not registered"); return 1;
    };
    if !check_msg(term_after == term, "a dropped RequestVote moved the follower's term") {
        return 1;
    }

    passed();
    0
}

// ---------------------------------------------------------------------------
// [fix, F13] A command without a value

// Start on the leader with an empty command. Before F13 the leader appended
// it, and its payload selection then read the slot as missing and skipped
// every follower whose next index reached it (bugs-found B16). Now Start
// refuses it, and the leader's log does not move.
fn test_empty_command_refused(_st: &mut LabState) -> i32 {
    init2(16, "Start refuses a command without a value");

    let leader = lab::one_leader(-1);
    if !check_msg(leader >= 0, "no leader") { return 1; }
    let Some((_, last_before, _, _, _)) = lab::log_tail(leader as u32) else {
        failed("leader not registered"); return 1;
    };
    let Some(appended) = lab::start_empty(leader as u32) else {
        failed("leader not registered"); return 1;
    };
    if !check_msg(!appended, "the leader appended a command without a value") {
        return 1;
    }
    let Some((_, last_after, _, _, _)) = lab::log_tail(leader as u32) else {
        failed("leader not registered"); return 1;
    };
    if !check_msg(last_after == last_before,
                  &format!("the leader's log moved from {} to {}", last_before, last_after)) {
        return 1;
    }

    passed();
    0
}

// ---------------------------------------------------------------------------
// [fix, F6] The leader-change callback

// The callback fires after mtx_ is released now, from a queue that keeps the
// transitions' order. Every replica records the notices it fires across a
// forced re-election. They must alternate (became leader, became follower,
// ...): a notice fired twice or lost would put two equal ones side by side.
// Once the cluster has one leader again, each replica's last notice must
// agree with its role.
fn test_leader_change_notices(_st: &mut LabState) -> i32 {
    init2(14, "Leader-change callback fires once per transition");

    lab::record_leader_notices();
    let leader = lab::one_leader(-1);
    if !check_msg(leader >= 0, "no leader") { return 1; }

    // The leader is cut off and replaced, then rejoins and steps down.
    lab::disconnect(leader as u32);
    lab::fiber_sleep_us(ELECTION_TIMEOUT_US);
    let new_leader = lab::one_leader(-1);
    if !check_msg(new_leader >= 0 && new_leader != leader,
                  "no new leader after disconnecting the old one") { return 1; }
    lab::reconnect(leader as u32);
    if !check_msg(lab::one_leader(new_leader) >= 0,
                  "the rejoined leader disturbed the new one") { return 1; }

    // A notice fires just after its transition's lock is released, so give
    // the last ones a moment before comparing them with the roles.
    let mut attempt: i32 = 0;
    loop {
        let mut settled = true;
        let mut svr: u32 = 0;
        while svr < NSERVERS as u32 {
            let (count, last, repeated) = lab::leader_notices(svr);
            if !check_msg(!repeated, "a replica fired the same leader-change notice twice in a row") {
                return 1;
            }
            let Some(leads) = lab::leads(svr) else {
                failed("replica not registered"); return 1;
            };
            // A replica that never changed role fired nothing; one that did
            // ends on its current role.
            if (count == 0 && leads) || (count > 0 && last != leads) {
                settled = false;
            }
            svr += 1;
        }
        if settled {
            break;
        }
        attempt += 1;
        if !check_msg(attempt < 20, "leader-change notices disagree with the roles") {
            return 1;
        }
        lab::fiber_sleep_us(50_000);
    }
    let (count_old, _, _) = lab::leader_notices(leader as u32);
    let (count_new, _, _) = lab::leader_notices(new_leader as u32);
    if !check_msg(count_old >= 1 && count_new >= 1,
                  "a role change fired no leader-change notice") { return 1; }

    passed();
    0
}

// The basic cases: the eleven ported ones in RaftLabTest::Run's order, then
// cases 12 to 14 above. lab_main.rs drives them, then the snapshot cases,
// and threads the state between the two by value: None is a failure.
fn run_basic_cases(st: &mut LabState) -> i32 {
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
        test_entry_term_zero_refused,  // [fix, F4]
        test_unavailable_voter_reply,
        test_leader_change_notices,  // [fix, F6]
        test_unadmitted_messages_dropped,  // [fix, F9]
        test_empty_command_refused,  // [fix, F13]
    ];

    for case in basic {
        if case(st) != 0 {
            return 1;
        }
    }
    0
}

pub fn run_basic(st: LabState) -> Option<LabState> {
    let mut st = st;
    // The loop is a function of its own, taking `&mut LabState`: a call
    // through the fn-pointer table has no parameter-style information for
    // the transpiler, a call to a named function does.
    if run_basic_cases(&mut st) != 0 {
        return None;
    }
    Some(st)
}

