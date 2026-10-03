// The snapshot, configuration, partition-recovery and load families of the
// RaftLab suite -- fourteen of the twenty-five cases.
//
// Where the replication cases in lab_cases.rs drive the cluster through the
// fixture, these reach into one replica: its snapshot boundary, its log base,
// its retention window, its state-machine callbacks.
//
// Three of the fourteen are not here. testSnapshotMetadataCreation,
// testSnapshotFormatRoundTrip and testSnapshotManagerSaveLoad construct a
// SnapshotMetadata, call SnapshotFormat's statics and exercise a
// MemorySnapshotManager on the stack. They are unit tests of C++ classes and
// touch no RaftServer, so they live in lab_unit_tests.cc and this suite calls
// them through one kernel.

#![allow(non_snake_case)]

use crate::lab;
use crate::lab::{ELECTION_TIMEOUT_US, NSERVERS};
use crate::lab_cases::{check_msg, failed, init2, passed, LabState};
use crate::scheduler_h::RaftSpecific;
use crate::server_h::{lab_get, RaftLockGuard, RaftServerBase, RaftStdLockGuard};
use crate::server_pods_h::RaftServerHandle;

// server.h:186 -- `#define HEARTBEAT_INTERVAL 100000`, the non-debug value the
// lab builds with.
const HEARTBEAT_INTERVAL_US: u64 = 100_000;
/// server_h.rs -- the shipped default, which test 57 pins.
const DEFAULT_SNAPSHOT_THRESHOLD: u64 = 10_000;
/// The shipped default retention window, which test 68 pins.
const DEFAULT_RETENTION_WINDOW: u64 = 5_000;

/// Everything a case compares across a snapshot operation. Mirrors
/// `RaftLabSnapshotProbe` in server.cc.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct SnapshotProbe {
    pub present: bool,
    pub last_included_index: u64,
    pub last_included_term: u64,
    pub timestamp_ms: u64,
    pub size_bytes: u64,
    pub checksum_digest: u64,
    pub data_digest: u64,
    pub count: u64,
}

unsafe extern "C" {
    fn raft_lab_new_snapshot_manager(out: *mut rusty::RaftSnapshotManagerPtr);
    fn raft_lab_snapshot_delete_all(
        manager: *const rusty::RaftSnapshotManagerPtr) -> u64;
    // `out` is a SnapshotProbe; typed as c_void so the kernel's C
    // declaration names no Rust type (SnapshotProbe derives, so it cannot be
    // a cpp_native_type binding).
    fn raft_lab_snapshot_probe(manager: *const rusty::RaftSnapshotManagerPtr,
                               out: *mut core::ffi::c_void);
    fn raft_lab_make_reject_prepare_cbs(
        create_out: *mut rusty::RaftCreateSnapshotCb,
        prepare_out: *mut rusty::RaftPrepareSnapshotCb);
    fn raft_lab_reject_prepare_called() -> bool;
    fn raft_lab_make_probe_cbs(manager: *const rusty::RaftSnapshotManagerPtr,
                               create_out: *mut rusty::RaftCreateSnapshotCb,
                               prepare_out: *mut rusty::RaftPrepareSnapshotCb);
    fn raft_lab_probe_flags() -> u32;
    fn raft_lab_probe_release();
    /// Tests 50-52: the C++ unit tests on hybrid/cpp (lab_unit_tests.cc),
    /// the Rust store's on the Rust lane (rt/src/lab_runtime.rs). 0 on success.
    fn raft_lab_snapshot_unit_tests() -> i32;
    /// The snapshot manager's shared_ptr, copied into a default-constructed
    /// slot. A shared_ptr relocates bitwise; the std::function carriers do
    /// not, which is why those are copied in place by the setters themselves
    /// (see reg_learner_action in server_h.rs). This was declared in
    /// server_cc.rs until the lab exports that used it were deleted.
    fn raft_snapshot_manager_ptr_clone_into(
        src: *const rusty::RaftSnapshotManagerPtr,
        dst: *mut rusty::RaftSnapshotManagerPtr);
    fn setenv(name: *const core::ffi::c_char, value: *const core::ffi::c_char,
              overwrite: i32) -> i32;
    // Tests 73-74 (plan N7): the store's save (SEAM: the Rust store or the
    // C++ manager), and the recovery step of Setup with its HOST catch
    // wrapper -- the exact path Setup takes.
    fn raft_snapshot_store_save(manager: *const rusty::RaftSnapshotManagerPtr,
                                index: u64, term: u64, data: *const u8, len: usize) -> bool;
    fn raft_initialize_snapshot_manager(server: *mut RaftServerHandle, site_id: u16) -> bool;
}

// raft_lab_probe_flags' bits, named.
const PROBE_PREPARE_CALLED: u32 = 1;
const PROBE_PREPARE_SAW_OLD: u32 = 2;
const PROBE_COMMIT_CALLED: u32 = 4;
const PROBE_COMMIT_SAW_PUBLISHED: u32 = 8;
const PROBE_ABORTED_BEFORE_COMMIT: u32 = 16;

// ---------------------------------------------------------------------------
// Helpers


// Borrowing one replica is `lab_get(loc).map(|e| lab::with_entry_server(&e, f))`
// at each call site rather than a local `with_server` returning Option<R>:
// that signature lowers to a C++ template whose R appears only in the
// return type, which C++ cannot deduce. `None` still means the locale is not
// registered, which the cases treat the way the C++ treats a null GetServer.


fn new_manager() -> rusty::RaftSnapshotManagerPtr {
    let mut manager: rusty::RaftSnapshotManagerPtr = Default::default();
    // SAFETY: the kernel constructs a shared_ptr into the slot; the carrier's
    // Drop releases it.
    unsafe { raft_lab_new_snapshot_manager(&raw mut manager) };
    manager
}

fn clone_manager(src: &rusty::RaftSnapshotManagerPtr) -> rusty::RaftSnapshotManagerPtr {
    let mut copy: rusty::RaftSnapshotManagerPtr = Default::default();
    // SAFETY: a refcount bump the opaque carrier cannot make bitwise.
    unsafe { raft_snapshot_manager_ptr_clone_into(src as *const _, &raw mut copy) };
    copy
}

fn probe(manager: &rusty::RaftSnapshotManagerPtr) -> SnapshotProbe {
    let mut out = SnapshotProbe::default();
    // SAFETY: the kernel fills the POD or leaves it zeroed.
    unsafe {
        raft_lab_snapshot_probe(manager as *const _,
                                &raw mut out as *mut core::ffi::c_void)
    };
    out
}

/// Port of RaftLabTest::InstallAndSeedSnapshotManager. Both locks are held
/// through publication AND the initial checkpoint for the reason the C++
/// gives: the server must never advertise a compacted prefix the active
/// manager has no bytes for.
///
/// Returns the seeded snapshot index, or None if the rotation failed.
fn install_and_seed(loc_id: u32, manager: &rusty::RaftSnapshotManagerPtr,
                    snapshot_threshold: u64) -> Option<u64> {
    // The closure's return type is spelled out: it returns both None and
    // Some, which C++ return-type deduction cannot unify.
    lab_get(loc_id).map(|e| lab::with_entry_server(&e, |svr| -> Option<u64> {
        let _apply_lock = RaftStdLockGuard::new(svr.LabApplyMutex());
        let _lock = RaftLockGuard::new(svr.LabMutex());

        if svr.LabSnapIdx() > 0 {
            // A compacted boundary means nothing without its exact state
            // bytes. Copy the checkpoint rather than regenerate a possibly
            // newer image while rotating managers.
            let old = clone_manager(svr.LabSnapshotManager());
            let checkpoint = probe(&old);
            if !checkpoint.present
                || checkpoint.last_included_index != svr.LabSnapIdx()
                || checkpoint.last_included_term != svr.LabSnapTerm() as u64
            {
                return None;
            }
            if !lab::copy_snapshot(&old, manager) {
                return None;
            }
            svr.SetSnapshotManagerLocked(clone_manager(manager));
        } else {
            // With no advertised boundary the replacement may be published
            // inside this gate and rolled back if the checkpoint fails.
            let old = clone_manager(svr.LabSnapshotManager());
            svr.SetSnapshotManagerLocked(clone_manager(manager));
            if !svr.CreateSnapshotLocked() {
                // A clone, not a move of `old`: an immutable binding lowers
                // to a const C++ local, and the carrier cannot be copied.
                svr.SetSnapshotManagerLocked(clone_manager(&old));
                return None;
            }
        }

        svr.SetSnapshotThresholdLocked(snapshot_threshold);
        Some(svr.LabSnapIdx())
    }))?
}

/// `setenv(name, value, 1)`, for the two cases that enable snapshots
/// process-wide.
fn set_env(name: &str, value: &str) -> bool {
    let name = std::ffi::CString::new(name).unwrap();
    let value = std::ffi::CString::new(value).unwrap();
    // SAFETY: both strings are NUL-terminated and live across the call.
    unsafe { setenv(name.as_ptr() as *const core::ffi::c_char,
                    value.as_ptr() as *const core::ffi::c_char, 1) == 0 }
}

/// The C++ picks a follower by INDEX (`for i in 0..NSERVERS if i != leader`),
/// then passes that index to GetServer, which maps it through the replicas
/// map. Transcribed rather than corrected: changing which replica a case
/// partitions changes what it tests.
fn first_non_leader_index(leader: i32) -> i32 {
    (0..NSERVERS as i32).find(|&i| i != leader).unwrap_or(-1)
}

/// Every replica's whole-log fingerprint: base, length and each term, in
/// order. Strictly stronger than comparing shared_ptr identity, which cannot
/// see an entry mutated in place. CALLER MUST HOLD the replica's LabMutex.
fn log_fingerprint(svr: &RaftServerBase) -> Vec<u64> {
    (0..svr.LabLogFingerprintLen()).map(|i| svr.LabLogFingerprintAt(i)).collect()
}

// ---------------------------------------------------------------------------
// Test 54 -- SnapshotManager wiring

fn test_snapshot_manager_wiring(_st: &mut LabState) -> i32 {
    init2(54, "SnapshotManager wiring in RaftServer");

    lab::fiber_sleep_us(ELECTION_TIMEOUT_US);
    let leader = lab::one_leader(-1);
    if !(leader >= 0) { return 1; }

    let test_mgr = new_manager();
    let restored = lab_get(leader as u32).map(|e| lab::with_entry_server(&e, |svr| {
        // Might be unset if MAKO_RAFT_SNAPSHOTS is off -- that is fine; the
        // Set/Get API is what this case pins.
        let existing = clone_manager(svr.LabSnapshotManager());
        svr.SetSnapshotManager(clone_manager(&test_mgr));
        let installed = probe(svr.LabSnapshotManager());
        let has = svr.HasSnapshot();
        let index = svr.GetSnapshotIndex();
        let term = svr.GetSnapshotTerm();
        // A clone for the reason given in install_and_seed.
        svr.SetSnapshotManager(clone_manager(&existing));
        (installed, has, index, term)
    }));
    let Some((installed, has, index, term)) = restored else {
        failed("Server should not be null");
        return 1;
    };

    // The manager we installed is empty, so it reports no snapshot -- which
    // is exactly what the C++ asserts about the same object.
    if !check_msg(!installed.present, "a freshly created manager should hold no snapshot") { return 1; }
    if !check_msg(!has, "HasSnapshot should be false with empty manager") { return 1; }
    if !check_msg(index == 0, &format!("GetSnapshotIndex should be 0 by default, got {}", index)) { return 1; }
    if !check_msg(term == 0, &format!("GetSnapshotTerm should be 0 by default, got {}", term)) { return 1; }

    // SAFETY: the manager is still ours; nothing else holds a reference.
    unsafe { raft_lab_snapshot_delete_all(&test_mgr as *const _) };
    eprintln!("[SNAPSHOT-WIRING-TEST] Wiring in RaftServer PASSED");
    passed();
    0
}

// ---------------------------------------------------------------------------
// Test 55 -- CreateSnapshot basic

fn test_create_snapshot_basic(_st: &mut LabState) -> i32 {
    init2(55, "CreateSnapshot basic");

    lab::fiber_sleep_us(ELECTION_TIMEOUT_US);
    let leader = lab::one_leader(-1);
    if !(leader >= 0) { return 1; }

    let test_mgr = new_manager();
    let Some(original_threshold) = lab_get(leader as u32).map(|e| lab::with_entry_server(&e, |s| s.GetSnapshotThreshold()))
    else { failed("Server should not be null"); return 1; };

    if let Some(e) = lab_get(leader as u32) { lab::with_entry_server(&e, |svr| {
        let _lock = RaftLockGuard::new(svr.LabMutex());
        svr.SetSnapshotManagerLocked(clone_manager(&test_mgr));
        // A low threshold so a snapshot triggers easily.
        svr.SetSnapshotThresholdLocked(5);
    }) }

    let before = lab_get(leader as u32).map(|e| lab::with_entry_server(&e, |s| (s.HasSnapshot(), s.GetSnapshotIndex())));
    if !check_msg(before == Some((false, 0)), "No snapshot should exist initially") { return 1; }

    // More than the threshold's worth of committed-and-applied entries
    for i in 1..=10 {
        let idx = lab::do_agreement(100 + i, NSERVERS as i32, true);
        if !check_msg(idx > 0, &format!("DoAgreement failed for cmd {}", 100 + i)) { return 1; }
    }
    // time for applyLogs to run and trigger CreateSnapshot
    lab::fiber_sleep_us(2_000_000);

    let after = lab_get(leader as u32).map(|e| lab::with_entry_server(&e,
        |s| (s.HasSnapshot(), s.GetSnapshotIndex(), s.GetSnapshotTerm())));
    let Some((has, snap_index, snap_term)) = after else {
        failed("Server should not be null"); return 1;
    };
    if !check_msg(has, "Snapshot should exist after exceeding threshold") { return 1; }
    if !check_msg(snap_index > 0, &format!("Snapshot index should be > 0, got {}", snap_index)) { return 1; }
    if !check_msg(snap_term > 0, &format!("Snapshot term should be > 0, got {}", snap_term)) { return 1; }

    let latest = probe(&test_mgr);
    if !check_msg(latest.present, "Snapshot manager should have a snapshot") { return 1; }
    if !check_msg(latest.last_included_index == snap_index, &format!("Manager index ({}) should match server index ({})", latest.last_included_index, snap_index)) { return 1; }

    // The snapshot now backs a compacted live prefix: restore only the
    // runtime threshold and keep the manager for future catch-up.
    if let Some(e) = lab_get(leader as u32) { lab::with_entry_server(&e, |svr| {
        let _lock = RaftLockGuard::new(svr.LabMutex());
        svr.SetSnapshotThresholdLocked(original_threshold);
    }) }
    eprintln!("[CREATE-SNAPSHOT-BASIC-TEST] Retaining live in-memory snapshot manager");
    eprintln!("[CREATE-SNAPSHOT-BASIC-TEST] PASSED");
    passed();
    0
}

// ---------------------------------------------------------------------------
// Test 56 -- CreateSnapshot and compaction

fn test_create_snapshot_and_compaction(_st: &mut LabState) -> i32 {
    init2(56, "CreateSnapshot and compaction");

    lab::fiber_sleep_us(ELECTION_TIMEOUT_US);
    let leader = lab::one_leader(-1);
    if !(leader >= 0) { return 1; }

    let test_mgr = new_manager();
    let Some(original_threshold) = lab_get(leader as u32).map(|e| lab::with_entry_server(&e, |s| s.GetSnapshotThreshold()))
    else { failed("Server should not be null"); return 1; };

    let Some(snapshot_baseline) = install_and_seed(leader as u32, &test_mgr, 5) else {
        failed("Could not atomically seed Test56 replacement snapshot manager");
        return 1;
    };

    let mut first_new_index = 0u64;
    for i in 1..=10 {
        let idx = lab::do_agreement(200 + i, NSERVERS as i32, true);
        if !check_msg(idx > 0, &format!("DoAgreement failed for cmd {}", 200 + i)) { return 1; }
        if first_new_index == 0 {
            first_new_index = idx;
        }
    }

    let mut snap_idx = 0u64;
    let mut snapshot_ready = false;
    for _ in 0..200 {
        if snapshot_ready { break; }
        snap_idx = lab_get(leader as u32).map(|e| lab::with_entry_server(&e, |svr| {
            let _lock = RaftLockGuard::new(svr.LabMutex());
            svr.GetSnapshotIndexLocked()
        })).unwrap_or(0);
        let candidate = probe(&test_mgr);
        if candidate.present {
            snapshot_ready = candidate.last_included_index == snap_idx
                && snap_idx > snapshot_baseline
                && snap_idx >= first_new_index;
        }
        if !snapshot_ready {
            lab::fiber_sleep_us(10_000);
        }
    }

    let latest = probe(&test_mgr);
    if !check_msg(snapshot_ready && latest.present, "Replacement snapshot manager did not advance for this workload") { return 1; }
    if !check_msg(snap_idx == latest.last_included_index, &format!("Server snapshot index {} does not match manager index {}", snap_idx, latest.last_included_index)) { return 1; }
    if !check_msg(snap_idx > snapshot_baseline && snap_idx >= first_new_index, &format!("Snapshot did not advance for this workload: baseline={}, first={}, got={}", snapshot_baseline, first_new_index, snap_idx)) { return 1; }

    // Entries submitted AFTER the snapshot must still commit
    for i in 1..=5 {
        let idx = lab::do_agreement(300 + i, NSERVERS as i32, true);
        if !check_msg(idx > 0, &format!("DoAgreement after snapshot failed for cmd {}", 300 + i)) { return 1; }
    }
    if !check_msg(lab::one_leader(-1) >= 0, "Should still have a leader after snapshot+compaction") { return 1; }

    if let Some(e) = lab_get(leader as u32) { lab::with_entry_server(&e, |svr| {
        let _lock = RaftLockGuard::new(svr.LabMutex());
        svr.SetSnapshotThresholdLocked(original_threshold);
    }) }
    eprintln!("[CREATE-SNAPSHOT-COMPACTION-TEST] Retaining live in-memory snapshot manager");
    eprintln!("[CREATE-SNAPSHOT-COMPACTION-TEST] PASSED");
    passed();
    0
}

// ---------------------------------------------------------------------------
// Test 57 -- snapshot threshold configurable

fn test_snapshot_threshold_configurable(_st: &mut LabState) -> i32 {
    init2(57, "Snapshot threshold configurable");

    lab::fiber_sleep_us(ELECTION_TIMEOUT_US);
    let leader = lab::one_leader(-1);
    if !(leader >= 0) { return 1; }

    let observed = lab_get(leader as u32).map(|e| lab::with_entry_server(&e, |svr| {
        let default_threshold = svr.GetSnapshotThreshold();
        svr.SetSnapshotThreshold(42);
        let forty_two = svr.GetSnapshotThreshold();
        svr.SetSnapshotThreshold(100_000);
        let hundred_thousand = svr.GetSnapshotThreshold();
        svr.SetSnapshotThreshold(DEFAULT_SNAPSHOT_THRESHOLD);
        (default_threshold, forty_two, hundred_thousand)
    }));
    let Some((default_threshold, forty_two, hundred_thousand)) = observed else {
        failed("Server should not be null"); return 1;
    };
    if !check_msg(default_threshold == DEFAULT_SNAPSHOT_THRESHOLD, &format!("Default threshold should be {}, got {}", DEFAULT_SNAPSHOT_THRESHOLD, default_threshold)) { return 1; }
    if !check_msg(forty_two == 42, &format!("Threshold should be 42 after SetSnapshotThreshold, got {}", forty_two)) { return 1; }
    if !check_msg(hundred_thousand == 100_000, &format!("Threshold should be 100000, got {}", hundred_thousand)) { return 1; }

    eprintln!("[SNAPSHOT-THRESHOLD-CONFIG-TEST] PASSED");
    passed();
    0
}

// ---------------------------------------------------------------------------
// Test 58 -- a stale InstallSnapshot is a no-op, and a refused Prepare
// changes nothing

fn test_install_snapshot_basic(_st: &mut LabState) -> i32 {
    init2(58, "InstallSnapshot stale index is a no-op");

    lab::fiber_sleep_us(ELECTION_TIMEOUT_US);
    let leader = lab::one_leader(-1);
    if !(leader >= 0) { return 1; }

    for i in 1..=5 {
        let idx = lab::do_agreement(200 + i, NSERVERS as i32, true);
        if !check_msg(idx > 0, &format!("DoAgreement failed for cmd {}", 200 + i)) { return 1; }
    }

    let follower = first_non_leader_index(leader);
    if !check_msg(follower >= 0, "No follower found") { return 1; }
    let follower = follower as u32;
    if !check_msg(lab_get(follower).is_some(), "Follower server should not be null") { return 1; }

    // A prior case may already have compacted this replica, so an EMPTY
    // replacement would violate the snapshot/log invariant before the RPC is
    // even exercised. Seed it.
    let test_mgr = new_manager();
    let Some(threshold) = lab_get(follower).map(|e| lab::with_entry_server(&e, |s| s.GetSnapshotThreshold())) else {
        failed("Follower server should not be null"); return 1;
    };
    if !check_msg(install_and_seed(follower, &test_mgr, threshold).is_some(), "Could not atomically seed Test58 replacement snapshot manager") { return 1; }

    let Some(before) = lab_get(follower).map(|e| lab::with_entry_server(&e, |svr| {
        let _lock = RaftLockGuard::new(svr.LabMutex());
        (svr.GetSnapshotIndexLocked(), svr.GetSnapshotTermLocked(),
         svr.LabCommitIndex(), svr.LabExecuteIndex(),
         svr.LabLastLogIndex(), svr.LabLogBase(), svr.LabCurrentTerm())
    })) else { failed("Follower server should not be null"); return 1; };
    let (old_snapidx, old_snapterm, old_commit_index, old_execute_index,
         old_last_log_index, old_min_active_slot, follower_term) = before;

    let before_probe = probe(&test_mgr);
    if !check_msg(before_probe.present, "Seeded Test58 snapshot manager has no readable snapshot") { return 1; }

    // A snapshot at commit_index_ is stale by definition. It is still valid
    // leader contact at the same term, but its payload must not rewrite
    // snapshot, log or apply state.
    let stale_snapshot_index = old_commit_index;
    if !check_msg(stale_snapshot_index > 0, "Test58 needs a non-zero committed prefix") { return 1; }

    let Some(leader_site) = lab_get(lab::server_id_by_index(leader as usize)).map(|e| lab::with_entry_server(&e,
                                        |s| s.SiteId())) else {
        failed("Leader server should not be null"); return 1;
    };

    let mut reply_term: u64 = 0;
    if let Some(e) = lab_get(follower) { lab::with_entry_server(&e, |svr| {
        let data = lab::byte_string("stale_same_term_snapshot_must_not_be_persisted");
        svr.OnInstallSnapshot(follower_term, leader_site as u64, stale_snapshot_index,
                              follower_term, &data, &raw mut reply_term);
    }) }
    if !check_msg(reply_term == follower_term, &format!("Same-term stale snapshot reply should be {}, got {}", follower_term, reply_term)) { return 1; }

    let Some(after) = lab_get(follower).map(|e| lab::with_entry_server(&e, |svr| {
        let _lock = RaftLockGuard::new(svr.LabMutex());
        (svr.GetSnapshotIndexLocked(), svr.GetSnapshotTermLocked(),
         svr.LabCommitIndex(), svr.LabExecuteIndex(),
         svr.LabLastLogIndex(), svr.LabLogBase(), svr.IsLeaderLocked())
    })) else { failed("Follower server should not be null"); return 1; };
    if !check_msg(after.0 == old_snapidx, &format!("Stale snapshot changed snapidx from {} to {}", old_snapidx, after.0)) { return 1; }
    if !check_msg(after.1 == old_snapterm, &format!("Stale snapshot changed snapterm from {} to {}", old_snapterm, after.1)) { return 1; }
    if !check_msg(after.2 == old_commit_index && after.3 == old_execute_index
                   && after.4 == old_last_log_index && after.5 == old_min_active_slot, "Stale snapshot mutated log/apply indices") { return 1; }
    if !check_msg(!after.6, "Accepted same-term leader contact must leave receiver a follower") { return 1; }

    if !check_msg(probe(&test_mgr) == before_probe, "Stale snapshot changed snapshot manager state") { return 1; }

    // Now the fallible Prepare boundary itself, not the stale fast path. A
    // validation rejection must not publish bytes, compact the log, or
    // fail-stop a healthy follower: the prepare contract forbids live
    // state-machine mutation.
    let Some(rejected_before) = lab_get(follower).map(|e| lab::with_entry_server(&e, |svr| {
        let _lock = RaftLockGuard::new(svr.LabMutex());
        // Plain u64::max, not `[..].into_iter().max()`: rusty::iter_max copies
        // an owning iterator into a local and returns a reference into it,
        // so under the C++ lane that read a dead temporary.
        let local_progress = svr.LabCommitIndex()
            .max(svr.LabExecuteIndex())
            .max(svr.GetAppliedIndex())
            .max(svr.LabSnapIdx())
            .max(svr.LabLastLogIndex());
        (log_fingerprint(svr), svr.LabSnapIdx(), svr.LabSnapTerm(),
         svr.LabCommitIndex(), svr.LabExecuteIndex(), svr.LabLastLogIndex(),
         svr.LabLogBase(), local_progress)
    })) else { failed("Follower server should not be null"); return 1; };
    let rejected_local_progress = rejected_before.7;
    if !check_msg(rejected_local_progress < u64::MAX, "Test58 cannot construct a successor snapshot boundary") { return 1; }

    let mut create_cb: rusty::RaftCreateSnapshotCb = Default::default();
    let mut prepare_cb: rusty::RaftPrepareSnapshotCb = Default::default();
    // SAFETY: the kernel constructs both std::functions into the slots; the
    // server copies them in place (see reg_learner_action's note).
    unsafe { raft_lab_make_reject_prepare_cbs(&raw mut create_cb, &raw mut prepare_cb) };
    let Some(token) = lab_get(follower).map(|e| lab::with_entry_server(&e,
        |svr| svr.SetStateMachineSnapshotCallbacks(&create_cb, &prepare_cb)))
    else { failed("Follower server should not be null"); return 1; };
    if !check_msg(token != 0, "Could not install Test58 rejecting prepare callback") { return 1; }

    let mut rejected_reply_term: u64 = follower_term;
    if let Some(e) = lab_get(follower) { lab::with_entry_server(&e, |svr| {
        let data = lab::byte_string("archive_rejected_during_prepare");
        svr.OnInstallSnapshot(follower_term, leader_site as u64,
                              rejected_local_progress + 1, follower_term,
                              &data, &raw mut rejected_reply_term);
    }) }

    // Clear before asserting, so an assertion that bails does not leave the
    // rejecting callback installed for the rest of the suite. The C++ uses an
    // RAII scope guard for the same reason.
    let cleared = lab_get(follower).map(|e| lab::with_entry_server(&e,
        |svr| svr.ClearStateMachineSnapshotCallbacks(token))).unwrap_or(false);

    // SAFETY: a plain atomic read of the kernel's flag.
    if !check_msg(unsafe { raft_lab_reject_prepare_called() }, "InstallSnapshot did not invoke the rejecting Prepare callback") { return 1; }
    if !check_msg(rejected_reply_term == 0, &format!("Rejected Prepare must return unavailable term 0, got {}", rejected_reply_term)) { return 1; }
    if !check_msg(!lab_get(follower).map(|e| lab::with_entry_server(&e, |s| s.LabStopped())).unwrap_or(true), "Clean Prepare rejection incorrectly fail-stopped the follower") { return 1; }

    let Some(rejected_after) = lab_get(follower).map(|e| lab::with_entry_server(&e, |svr| {
        let _lock = RaftLockGuard::new(svr.LabMutex());
        (log_fingerprint(svr), svr.LabSnapIdx(), svr.LabSnapTerm(),
         svr.LabCommitIndex(), svr.LabExecuteIndex(), svr.LabLastLogIndex(),
         svr.LabLogBase())
    })) else { failed("Follower server should not be null"); return 1; };
    if !check_msg(rejected_after.0 == rejected_before.0
                   && rejected_after.1 == rejected_before.1
                   && rejected_after.2 == rejected_before.2
                   && rejected_after.3 == rejected_before.3
                   && rejected_after.4 == rejected_before.4
                   && rejected_after.5 == rejected_before.5
                   && rejected_after.6 == rejected_before.6, "Rejected Prepare mutated the in-memory snapshot/log boundary") { return 1; }
    if !check_msg(probe(&test_mgr) == before_probe, "Rejected Prepare changed the snapshot manager") { return 1; }
    if !check_msg(cleared, "Could not clear Test58 rejecting prepare callback") { return 1; }

    eprintln!("[INSTALL-SNAPSHOT-STALE-INDEX-TEST] Retaining live in-memory snapshot manager");
    eprintln!("[INSTALL-SNAPSHOT-STALE-INDEX-TEST] PASSED");
    passed();
    0
}

// ---------------------------------------------------------------------------
// Test 59 -- InstallSnapshot rejects a stale term, a future boundary, and an
// unauthorized sender

/// The receiver state every rejection in test 59 must leave untouched.
type Test59State = (u64, i64, u64, u64, u64, u64, u16, u16, bool, bool, bool);

fn test59_state(svr: &mut RaftServerBase) -> Test59State {
    let _lock = RaftLockGuard::new(svr.LabMutex());
    (svr.LabSnapIdx(), svr.LabSnapTerm(), svr.LabCommitIndex(),
     svr.LabExecuteIndex(), svr.LabLastLogIndex(), svr.LabCurrentTerm(),
     svr.LabCurrentLeaderId(), svr.LabVoteFor(), svr.LabIsLeader(),
     svr.LabReqVoting(), svr.LabElectionInProgress())
}

fn test_install_snapshot_rejects_stale_term(_st: &mut LabState) -> i32 {
    init2(59, "InstallSnapshot rejects stale term");

    lab::fiber_sleep_us(ELECTION_TIMEOUT_US);
    let leader = lab::one_leader(-1);
    if !(leader >= 0) { return 1; }

    for i in 1..=3 {
        let idx = lab::do_agreement(300 + i, NSERVERS as i32, true);
        if !check_msg(idx > 0, &format!("DoAgreement failed for cmd {}", 300 + i)) { return 1; }
    }

    let follower = first_non_leader_index(leader);
    if !check_msg(follower >= 0, "No follower found") { return 1; }
    let follower = follower as u32;

    // One coherent observation, with the live Raft and apply threads excluded:
    // the raw fields are not atomic.
    let Some(before) = lab_get(follower).map(|e| lab::with_entry_server(&e, test59_state)) else {
        failed("Follower server should not be null"); return 1;
    };
    let follower_term = before.5;
    let before_last_log_index = before.4;

    // Term 0 is less than any active term.
    let stale_term: u64 = 0;
    if !check_msg(stale_term < follower_term, &format!("Stale term {} should be < follower term {}", stale_term, follower_term)) { return 1; }

    let mut reply_term: u64 = 0;
    if let Some(e) = lab_get(follower) { lab::with_entry_server(&e, |svr| {
        let data = lab::byte_string("stale_snapshot_data");
        svr.OnInstallSnapshot(stale_term, 999, 100, 1, &data, &raw mut reply_term);
    }) }
    if !check_msg(reply_term == follower_term, &format!("Reply term should be follower's current term {}, got {}", follower_term, reply_term)) { return 1; }
    if !check_msg(lab_get(follower).map(|e| lab::with_entry_server(&e, test59_state)) == Some(before), "Stale-term snapshot mutated Raft role/election state") { return 1; }

    // A same-term sender cannot advertise a boundary from a future term
    // either. That must be refused before leader contact or payload
    // processing changes anything.
    let future_boundary_index = if before_last_log_index == u64::MAX {
        before_last_log_index
    } else {
        before_last_log_index + 1
    };
    let mut future_reply_term: u64 = 0;
    if let Some(e) = lab_get(follower) { lab::with_entry_server(&e, |svr| {
        let data = lab::byte_string("future_term_snapshot_must_not_be_loaded");
        svr.OnInstallSnapshot(follower_term, leader as u64, future_boundary_index,
                              follower_term + 1, &data, &raw mut future_reply_term);
    }) }
    if !check_msg(future_reply_term == 0, &format!("Same-term future-boundary rejection must report unavailable (0), got {}", future_reply_term)) { return 1; }
    if !check_msg(lab_get(follower).map(|e| lab::with_entry_server(&e, test59_state)) == Some(before), "Future-boundary snapshot mutated receiver state") { return 1; }

    // A rejected but well-formed request must not look successful to the
    // sender: InstallSnapshot's leader callback treats any non-zero reply at
    // its send term as proof the boundary was installed.
    let mut unauthorized_reply_term: u64 = u64::MAX;
    if let Some(e) = lab_get(follower) { lab::with_entry_server(&e, |svr| {
        let data = lab::byte_string("unauthorized_snapshot_must_not_be_acknowledged");
        svr.OnInstallSnapshot(follower_term, 998, future_boundary_index,
                              follower_term, &data, &raw mut unauthorized_reply_term);
    }) }
    if !check_msg(unauthorized_reply_term == 0, &format!("Unauthorized snapshot rejection must report unavailable (0), got {}", unauthorized_reply_term)) { return 1; }

    let mut unrepresentable_reply_term: u64 = u64::MAX;
    if let Some(e) = lab_get(follower) { lab::with_entry_server(&e, |svr| {
        let data = lab::byte_string("unrepresentable_leader_must_not_be_acknowledged");
        svr.OnInstallSnapshot(follower_term, u64::MAX, future_boundary_index,
                              follower_term, &data, &raw mut unrepresentable_reply_term);
    }) }
    if !check_msg(unrepresentable_reply_term == 0, &format!("Unrepresentable snapshot leader must report unavailable (0), got {}", unrepresentable_reply_term)) { return 1; }
    if !check_msg(lab_get(follower).map(|e| lab::with_entry_server(&e, test59_state)) == Some(before), "Unauthorized snapshot rejection mutated receiver state") { return 1; }

    eprintln!("[INSTALL-SNAPSHOT-REJECTS-STALE-TEST] PASSED");
    passed();
    0
}

// ---------------------------------------------------------------------------
// Test 60 -- the heartbeat loop installs a snapshot after a real partition

fn test_heartbeat_triggers_install_snapshot(_st: &mut LabState) -> i32 {
    init2(60, "HeartbeatLoop installs snapshot after a real partition");

    lab::fiber_sleep_us(ELECTION_TIMEOUT_US);
    let mut leader = lab::one_leader(-1);
    if !(leader >= 0) { return 1; }

    // Every possible leader gets a unique live manager BEFORE the partition,
    // so the case stays valid across an ordinary re-election. These become the
    // backing store for compacted prefixes, so the process-wide snapshot knob
    // must stay in step with them for the rest of the suite -- as test 69 also
    // does after its rotation.
    if !check_msg(set_env("MAKO_RAFT_SNAPSHOTS", "1"), "Could not enable Test60 snapshots") { return 1; }

    let mut managers: Vec<rusty::RaftSnapshotManagerPtr> = Vec::new();
    let mut original_thresholds = [0u64; NSERVERS];
    let mut seeded = [0u64; NSERVERS];
    for i in 0..NSERVERS {
        let loc = lab::server_id_by_index(i);
        if !check_msg(lab_get(loc).is_some(), &format!("Test60 server {} is null", i)) { return 1; }
        let manager = new_manager();
        original_thresholds[i] = lab_get(loc).map(|e| lab::with_entry_server(&e, |s| s.GetSnapshotThreshold())).unwrap_or(0);
        let Some(index) = install_and_seed(loc, &manager, 3) else {
            failed(&format!("Could not atomically seed Test60 server {} snapshot manager", i));
            return 1;
        };
        seeded[i] = index;
        managers.push(manager);
    }

    let follower_index = first_non_leader_index(leader);
    if !check_msg(follower_index >= 0, "No Test60 follower found") { return 1; }
    let follower = lab::server_id_by_index(follower_index as usize);

    let Some((follower_snap_before, follower_last_before)) = lab_get(follower).map(|e| lab::with_entry_server(&e, |svr| {
        let _lock = RaftLockGuard::new(svr.LabMutex());
        (svr.GetSnapshotIndexLocked(), svr.LabLastLogIndex())
    })) else { failed("Test60 follower is null"); return 1; };

    lab::disconnect(follower);

    // NCommitted is a callback-only oracle: a replica that legitimately
    // installs a marker-only snapshot publishes its applied boundary but
    // cannot reconstruct the covered command's old callback. So require a real
    // quorum through DoAgreement AND, separately, that every reachable replica
    // publishes the applied boundary.
    let partition_quorum = (NSERVERS / 2 + 1) as i32;
    let mut first_partition_index = 0u64;
    let mut last_partition_index = 0u64;
    for i in 1..=8 {
        let idx = lab::do_agreement(600 + i, partition_quorum, true);
        if idx == 0 {
            lab::reconnect(follower);
            failed(&format!("Test60 agreement failed for cmd {}", 600 + i));
            return 1;
        }
        let mut connected_applied = 0;
        for _ in 0..100 {
            connected_applied = (0..NSERVERS)
                .map(lab::server_id_by_index)
                .filter(|&loc| loc != follower)
                .filter(|&loc| lab_get(loc).map(|e| lab::with_entry_server(&e, |s| s.GetAppliedIndex() >= idx))
                                   .unwrap_or(false))
                .count();
            if connected_applied >= NSERVERS - 1 { break; }
            lab::fiber_sleep_us(HEARTBEAT_INTERVAL_US);
        }
        if connected_applied != NSERVERS - 1 {
            lab::reconnect(follower);
            failed(&format!(
                "Only {} of {} connected Test60 replicas published applied index {} for cmd {}",
                connected_applied, NSERVERS - 1, idx, 600 + i));
            return 1;
        }
        if first_partition_index == 0 { first_partition_index = idx; }
        last_partition_index = idx;
    }

    leader = lab::one_leader(-1);
    if leader < 0 {
        lab::reconnect(follower);
        return 1;
    }
    let leader_loc = lab::server_id_by_index(leader as usize);

    let mut leader_snap_idx = 0u64;
    let mut leader_min_active = 0u64;
    let mut leader_snapshot_ready = false;
    for _ in 0..300 {
        if leader_snapshot_ready { break; }
        let Some((execute_index, snap_idx, min_active)) = lab_get(leader_loc).map(|e| lab::with_entry_server(&e, |svr| {
            let _lock = RaftLockGuard::new(svr.LabMutex());
            (svr.LabExecuteIndex(), svr.GetSnapshotIndexLocked(), svr.LabLogBase())
        })) else { break };
        leader_snap_idx = snap_idx;
        leader_min_active = min_active;
        let candidate = probe(&managers[leader as usize]);
        if candidate.present {
            leader_snapshot_ready = execute_index >= last_partition_index
                && candidate.last_included_index == snap_idx
                && snap_idx > seeded[leader as usize]
                && snap_idx >= first_partition_index;
        }
        if !leader_snapshot_ready {
            lab::fiber_sleep_us(10_000);
        }
    }
    if !leader_snapshot_ready {
        lab::reconnect(follower);
        failed("Test60 leader did not create a fresh partition snapshot");
        return 1;
    }
    if leader_snap_idx <= follower_snap_before
        || follower_last_before == u64::MAX
        || leader_min_active <= follower_last_before + 1
    {
        lab::reconnect(follower);
        failed(&format!(
            "Leader retained a bridgeable log gap: min_active={} follower_next={}",
            leader_min_active, follower_last_before + 1));
        return 1;
    }

    // Replace the marker-only loader with an owned probe for this one install.
    // Prepare observes the OLD manager image; Commit refuses to succeed unless
    // the exact incoming bytes are already readable from that manager.
    let mut create_cb: rusty::RaftCreateSnapshotCb = Default::default();
    let mut prepare_cb: rusty::RaftPrepareSnapshotCb = Default::default();
    // SAFETY: the kernel constructs both std::functions into the slots and
    // takes its own reference to the manager.
    unsafe {
        raft_lab_make_probe_cbs(&managers[follower_index as usize] as *const _,
                                &raw mut create_cb, &raw mut prepare_cb);
    }
    let token = lab_get(follower).map(|e| lab::with_entry_server(&e,
        |svr| svr.SetStateMachineSnapshotCallbacks(&create_cb, &prepare_cb)))
        .unwrap_or(0);
    if token == 0 {
        lab::reconnect(follower);
        // SAFETY: drops the kernel's reference to the manager.
        unsafe { raft_lab_probe_release() };
        failed("Could not install Test60 snapshot publication probe");
        return 1;
    }

    lab::reconnect(follower);

    // Reconnecting an isolated follower can legitimately advance the term and
    // elect a different leader, which may have compacted to a slightly earlier
    // but still bridging snapshot. Raft requires the CURRENT leader to install
    // a snapshot past the follower's old log and repair the suffix; it does
    // not require convergence on the former leader's exact boundary.
    let required_snapshot_floor =
        std::cmp::max(follower_last_before + 1, first_partition_index);
    let mut follower_snap_after = follower_snap_before;
    for _ in 0..100 {
        if follower_snap_after >= required_snapshot_floor { break; }
        lab::fiber_sleep_us(HEARTBEAT_INTERVAL_US);
        follower_snap_after = lab_get(follower).map(|e| lab::with_entry_server(&e, |svr| {
            let _lock = RaftLockGuard::new(svr.LabMutex());
            svr.GetSnapshotIndexLocked()
        })).unwrap_or(follower_snap_after);
    }

    let cleared = lab_get(follower).map(|e| lab::with_entry_server(&e,
        |svr| svr.ClearStateMachineSnapshotCallbacks(token))).unwrap_or(false);
    // SAFETY: drops the kernel's reference to the manager.
    let flags = unsafe { let f = raft_lab_probe_flags(); raft_lab_probe_release(); f };

    if !check_msg(follower_snap_after >= required_snapshot_floor
                   && follower_snap_after > follower_snap_before, &format!("Follower snapshot did not bridge its old log from {} through required floor {} \
                (pre-reconnect leader snapshot was {}); got {}", follower_snap_before, required_snapshot_floor, leader_snap_idx, follower_snap_after)) { return 1; }
    let follower_probe = probe(&managers[follower_index as usize]);
    if !check_msg(follower_probe.present
                   && follower_probe.last_included_index == follower_snap_after, &format!("Follower manager does not contain the installed snapshot {}", follower_snap_after)) { return 1; }
    if !check_msg(flags & PROBE_PREPARE_CALLED != 0 && flags & PROBE_PREPARE_SAW_OLD != 0, "InstallSnapshot Prepare did not run against the old manager image") { return 1; }
    if !check_msg(flags & PROBE_COMMIT_CALLED != 0
                   && flags & PROBE_COMMIT_SAW_PUBLISHED != 0
                   && flags & PROBE_ABORTED_BEFORE_COMMIT == 0, "InstallSnapshot Commit ran before exact Raft snapshot publication") { return 1; }
    if !check_msg(cleared, "Could not clear Test60 snapshot publication probe") { return 1; }

    if !check_msg(lab::do_agreement(700, NSERVERS as i32, true) > 0, "Cluster did not make progress after Test60 snapshot recovery") { return 1; }

    // Restore the runtime thresholds, keeping each replica on the live manager
    // that backs its compacted prefix.
    for (i, &threshold) in original_thresholds.iter().enumerate() {
        let loc = lab::server_id_by_index(i);
        if let Some(e) = lab_get(loc) { lab::with_entry_server(&e, |svr| {
            let _lock = RaftLockGuard::new(svr.LabMutex());
            svr.SetSnapshotThresholdLocked(threshold);
        }) }
    }
    // The managers stay alive: `managers` is dropped here, but each replica
    // holds its own shared_ptr through SetSnapshotManagerLocked.
    eprintln!("[HEARTBEAT-SNAPSHOT-TEST] Retaining live in-memory snapshot managers");
    eprintln!("[HEARTBEAT-SNAPSHOT-TEST] PASSED");
    passed();
    0
}

// ---------------------------------------------------------------------------
// Test 67 -- heartbeat interval configurable

fn test_heartbeat_interval_configurable(_st: &mut LabState) -> i32 {
    init2(67, "Heartbeat interval runtime-configurable");

    lab::fiber_sleep_us(ELECTION_TIMEOUT_US);
    let leader = lab::one_leader(-1);
    if !(leader >= 0) { return 1; }

    let Some(default_interval) = lab_get(lab::server_id_by_index(leader as usize)).map(|e| lab::with_entry_server(&e,
                                              |s| s.GetHeartbeatInterval())) else {
        failed("Server should not be null"); return 1;
    };
    if !check_msg(default_interval == HEARTBEAT_INTERVAL_US, &format!("Default heartbeat interval should be {}, got {}", HEARTBEAT_INTERVAL_US, default_interval)) { return 1; }

    // A named constant: the transpiler keeps a `200_000` literal verbatim
    // inside format arguments, which C++ cannot parse.
    const SET_INTERVAL_US: u64 = 200_000;
    let leader_loc = lab::server_id_by_index(leader as usize);
    let retrieved = lab_get(leader_loc).map(|e| lab::with_entry_server(&e, |svr| {
        svr.SetHeartbeatInterval(SET_INTERVAL_US);
        svr.GetHeartbeatInterval()
    })).unwrap_or(0);
    if !check_msg(retrieved == SET_INTERVAL_US, &format!("Heartbeat interval should be {} after set, got {}", SET_INTERVAL_US, retrieved)) { return 1; }

    for i in 0..NSERVERS {
        let loc = lab::server_id_by_index(i);
        let got = lab_get(loc).map(|e| lab::with_entry_server(&e, |svr| {
            svr.SetHeartbeatInterval(150_000);
            svr.GetHeartbeatInterval()
        }));
        if let Some(got) = got {
            if !check_msg(got == 150_000, &format!("Server {} heartbeat interval should be 150000, got {}", i, got)) { return 1; }
        }
    }

    if !check_msg(lab::do_agreement(6700, NSERVERS as i32, true) > 0, "DoAgreement should succeed after changing heartbeat interval") { return 1; }

    for i in 0..NSERVERS {
        if let Some(e) = lab_get(lab::server_id_by_index(i)) { lab::with_entry_server(&e,
                    |svr| svr.SetHeartbeatInterval(HEARTBEAT_INTERVAL_US)) }
    }
    eprintln!("TEST 67: Heartbeat interval configurable PASSED!");
    passed();
    0
}

// ---------------------------------------------------------------------------
// Test 68 -- log retention window configurable

fn test_log_retention_window_configurable(_st: &mut LabState) -> i32 {
    init2(68, "Log retention window runtime-configurable");

    lab::fiber_sleep_us(ELECTION_TIMEOUT_US);
    let leader = lab::one_leader(-1);
    if !(leader >= 0) { return 1; }

    let leader_loc = lab::server_id_by_index(leader as usize);
    let Some(default_window) = lab_get(leader_loc).map(|e| lab::with_entry_server(&e, |s| s.GetLogRetentionWindow())) else {
        failed("Server should not be null"); return 1;
    };
    if !check_msg(default_window == DEFAULT_RETENTION_WINDOW, &format!("Default log retention window should be {}, got {}", DEFAULT_RETENTION_WINDOW, default_window)) { return 1; }

    let new_window: u64 = 20;
    let retrieved = lab_get(leader_loc).map(|e| lab::with_entry_server(&e, |svr| {
        svr.SetLogRetentionWindow(new_window);
        svr.GetLogRetentionWindow()
    })).unwrap_or(0);
    if !check_msg(retrieved == new_window, &format!("Log retention window should be {} after set, got {}", new_window, retrieved)) { return 1; }

    for i in 0..NSERVERS {
        let got = lab_get(lab::server_id_by_index(i)).map(|e| lab::with_entry_server(&e, |svr| {
            svr.SetLogRetentionWindow(new_window);
            svr.GetLogRetentionWindow()
        }));
        if let Some(got) = got {
            if !check_msg(got == new_window, &format!("Server {} log retention window should be {}, got {}", i, new_window, got)) { return 1; }
        }
    }

    // With window=20, forty entries is enough to trigger cleanup
    for i in 0..40 {
        if !check_msg(lab::do_agreement(6800 + i, NSERVERS as i32, true) > 0, &format!("DoAgreement should succeed (entry {})", i)) { return 1; }
    }
    if !check_msg(lab::do_agreement(6899, NSERVERS as i32, true) > 0, "DoAgreement should succeed after log cleanup") { return 1; }

    for i in 0..NSERVERS {
        if let Some(e) = lab_get(lab::server_id_by_index(i)) { lab::with_entry_server(&e,
                    |svr| svr.SetLogRetentionWindow(DEFAULT_RETENTION_WINDOW)) }
    }
    eprintln!("TEST 68: Log retention window configurable PASSED!");
    passed();
    0
}

// ---------------------------------------------------------------------------
// Test 69 -- long partition recovery via InstallSnapshot

fn test_long_partition_recovery(_st: &mut LabState) -> i32 {
    init2(69, "Long partition recovery via InstallSnapshot");

    lab::fiber_sleep_us(ELECTION_TIMEOUT_US);
    let mut leader = lab::one_leader(-1);
    if !(leader >= 0) { return 1; }

    // Managers on ALL replicas with a low threshold. MAKO_RAFT_SNAPSHOTS stays
    // enabled for the rest of the process to match them: a compacted log is
    // not self-contained without the snapshot bytes covering its prefix.
    if !check_msg(set_env("MAKO_RAFT_SNAPSHOTS", "1"), "Could not enable snapshots for long-partition fixture") { return 1; }

    let mut managers: Vec<rusty::RaftSnapshotManagerPtr> = Vec::new();
    let mut seeded = [0u64; NSERVERS];
    let mut original_thresholds = [0u64; NSERVERS];
    let mut original_windows = [0u64; NSERVERS];
    for i in 0..NSERVERS {
        let loc = lab::server_id_by_index(i);
        let manager = new_manager();
        if let Some((threshold, window)) = lab_get(loc).map(|e| lab::with_entry_server(&e, |svr| {
            let _lock = RaftLockGuard::new(svr.LabMutex());
            (svr.GetSnapshotThreshold(), svr.GetLogRetentionWindow())
        })) {
            original_thresholds[i] = threshold;
            original_windows[i] = window;
        }
        let Some(index) = install_and_seed(loc, &manager, 5) else {
            failed(&format!("Could not atomically seed Test69 server {} snapshot manager", i));
            return 1;
        };
        seeded[i] = index;
        if let Some(e) = lab_get(loc) { lab::with_entry_server(&e, |svr| {
            let _lock = RaftLockGuard::new(svr.LabMutex());
            svr.SetLogRetentionWindow(10);
        }) }
        managers.push(manager);
    }

    let follower_index = first_non_leader_index(leader);
    if !check_msg(follower_index >= 0, "No follower found") { return 1; }
    let follower = lab::server_id_by_index(follower_index as usize);

    lab::disconnect(follower);

    let partition_quorum = (NSERVERS / 2 + 1) as i32;
    let mut first_partition_index = 0u64;
    let mut last_partition_index = 0u64;
    for i in 1..=20 {
        let idx = lab::do_agreement(6900 + i, partition_quorum, true);
        if idx == 0 {
            lab::reconnect(follower);
            failed(&format!("DoAgreement failed for cmd {}", 6900 + i));
            return 1;
        }
        let mut connected_applied = 0;
        for _ in 0..100 {
            connected_applied = (0..NSERVERS)
                .map(lab::server_id_by_index)
                .filter(|&loc| loc != follower)
                .filter(|&loc| lab_get(loc).map(|e| lab::with_entry_server(&e, |s| s.GetAppliedIndex() >= idx))
                                   .unwrap_or(false))
                .count();
            if connected_applied >= NSERVERS - 1 { break; }
            lab::fiber_sleep_us(HEARTBEAT_INTERVAL_US);
        }
        if connected_applied != NSERVERS - 1 {
            lab::reconnect(follower);
            failed(&format!(
                "Only {} of {} connected replicas published applied index {} for cmd {}",
                connected_applied, NSERVERS - 1, idx, 6900 + i));
            return 1;
        }
        if first_partition_index == 0 { first_partition_index = idx; }
        last_partition_index = idx;
    }

    leader = lab::one_leader(-1);
    if leader < 0 {
        lab::reconnect(follower);
        return 1;
    }
    let leader_loc = lab::server_id_by_index(leader as usize);

    let mut leader_snap_idx = 0u64;
    let mut leader_min_active = 0u64;
    let mut leader_execute_index = 0u64;
    let mut leader_snapshot_ready = false;
    for _ in 0..300 {
        if leader_snapshot_ready { break; }
        let Some((execute_index, snap_idx, min_active)) = lab_get(leader_loc).map(|e| lab::with_entry_server(&e, |svr| {
            let _lock = RaftLockGuard::new(svr.LabMutex());
            (svr.LabExecuteIndex(), svr.GetSnapshotIndexLocked(), svr.LabLogBase())
        })) else { break };
        leader_execute_index = execute_index;
        leader_snap_idx = snap_idx;
        leader_min_active = min_active;
        let candidate = probe(&managers[leader as usize]);
        if candidate.present {
            leader_snapshot_ready = execute_index >= last_partition_index
                && candidate.last_included_index == snap_idx
                && snap_idx > seeded[leader as usize]
                && snap_idx >= first_partition_index;
        }
        if !leader_snapshot_ready {
            lab::fiber_sleep_us(10_000);
        }
    }
    if !leader_snapshot_ready {
        lab::reconnect(follower);
        failed(&format!(
            "Leader did not create a fresh snapshot for partition workload [{}, {}]",
            first_partition_index, last_partition_index));
        return 1;
    }

    let leader_probe = probe(&managers[leader as usize]);
    let Some((follower_snap_before, follower_last_before)) = lab_get(follower).map(|e| lab::with_entry_server(&e, |svr| {
        let _lock = RaftLockGuard::new(svr.LabMutex());
        (svr.GetSnapshotIndexLocked(), svr.LabLastLogIndex())
    })) else {
        lab::reconnect(follower);
        failed("Disconnected follower server should not be null");
        return 1;
    };

    let precondition_failure: Option<String> = if leader_execute_index < last_partition_index {
        Some(format!("Leader execute_index {} did not reach partition workload end {}",
                     leader_execute_index, last_partition_index))
    } else if !leader_probe.present {
        Some("Test69 leader's unique manager has no snapshot".to_string())
    } else if leader_snap_idx != leader_probe.last_included_index {
        Some(format!("Leader snapshot index {} does not match manager index {}",
                     leader_snap_idx, leader_probe.last_included_index))
    } else if leader_min_active <= 1 {
        Some(format!("Leader log base should be > 1 after compaction, got {}",
                     leader_min_active))
    } else if leader_snap_idx <= follower_snap_before {
        Some(format!("Leader snapshot {} must be newer than disconnected follower snapshot {}",
                     leader_snap_idx, follower_snap_before))
    } else if follower_last_before == u64::MAX
        || leader_min_active <= follower_last_before + 1 {
        Some(format!("Leader retained a bridgeable Test69 gap: min_active={} follower_next={}",
                     leader_min_active, follower_last_before + 1))
    } else {
        None
    };
    if let Some(message) = precondition_failure {
        lab::reconnect(follower);
        failed(&message);
        return 1;
    }

    lab::reconnect(follower);

    let required_snapshot_floor =
        std::cmp::max(follower_last_before + 1, first_partition_index);
    let mut follower_snap_idx = follower_snap_before;
    for _ in 0..100 {
        if follower_snap_idx >= required_snapshot_floor { break; }
        lab::fiber_sleep_us(HEARTBEAT_INTERVAL_US);
        follower_snap_idx = lab_get(follower).map(|e| lab::with_entry_server(&e, |svr| {
            let _lock = RaftLockGuard::new(svr.LabMutex());
            svr.GetSnapshotIndexLocked()
        })).unwrap_or(follower_snap_idx);
    }
    if !check_msg(follower_snap_idx >= required_snapshot_floor
                   && follower_snap_idx > follower_snap_before, &format!("Follower snapshot should advance beyond {} through required floor {} \
                (pre-reconnect leader snapshot was {}), got {}", follower_snap_before, required_snapshot_floor, leader_snap_idx, follower_snap_idx)) { return 1; }

    let follower_probe = probe(&managers[follower_index as usize]);
    if !check_msg(follower_probe.present, "Test69 follower's unique manager has no installed snapshot") { return 1; }
    if !check_msg(follower_probe.last_included_index == follower_snap_idx, &format!("Follower snapshot index {} does not match manager index {}", follower_snap_idx, follower_probe.last_included_index)) { return 1; }

    if !check_msg(lab::do_agreement(6999, NSERVERS as i32, true) > 0, "DoAgreement should succeed with all 5 nodes after partition recovery") { return 1; }

    for i in 0..NSERVERS {
        if let Some(e) = lab_get(lab::server_id_by_index(i)) { lab::with_entry_server(&e, |svr| {
            let _lock = RaftLockGuard::new(svr.LabMutex());
            if original_thresholds[i] != 0 {
                svr.SetSnapshotThresholdLocked(original_thresholds[i]);
            }
            if original_windows[i] != 0 {
                svr.SetLogRetentionWindow(original_windows[i]);
            }
        }) }
    }
    eprintln!("TEST 69: Retaining live in-memory snapshot managers through suite shutdown");
    eprintln!("TEST 69: Long partition recovery via InstallSnapshot PASSED!");
    passed();
    0
}

// ---------------------------------------------------------------------------
// Test 72 -- high-frequency apply

fn test_high_frequency_apply(_st: &mut LabState) -> i32 {
    init2(72, "High frequency apply: rapid submissions, no dropped entries");

    lab::fiber_sleep_us(ELECTION_TIMEOUT_US);
    let mut leader = lab::one_leader(-1);
    if !check_msg(leader >= 0, "No leader elected") { return 1; }

    if !check_msg(lab::do_agreement(7200, NSERVERS as i32, true) > 0, "Failed to establish baseline agreement") { return 1; }

    leader = lab::one_leader(-1);
    if !check_msg(leader >= 0, "No leader after baseline") { return 1; }
    let leader_id = lab::server_id_by_index(leader as usize);

    // 100 entries without waiting for agreement between them, to stress the
    // apply_pending_ mechanism with a burst that must be applied in order.
    const NUM_ENTRIES: i32 = 100;
    let mut first_index = 0u64;
    let mut last_index = 0u64;
    for i in 0..NUM_ENTRIES {
        let (ok, index, _term) = lab::start(leader_id, 7201 + i);
        if !check_msg(ok, &format!("Failed to submit command {} (entry {}/{})", 7201 + i, i + 1, NUM_ENTRIES)) { return 1; }
        if i == 0 { first_index = index; }
        last_index = index;
    }
    if !check_msg(last_index - first_index + 1 == NUM_ENTRIES as u64, &format!("Expected {} consecutive indices, got range {}-{}", NUM_ENTRIES, first_index, last_index)) { return 1; }

    let Some(current_term) = lab_get(leader_id).map(|e| lab::with_entry_server(&e, |svr| {
        let _lock = RaftLockGuard::new(svr.LabMutex());
        svr.LabCurrentTerm()
    })) else { failed("Leader server is null"); return 1; };

    let result = lab::wait(last_index, NSERVERS as i32, current_term);
    if !check_msg(result >= 0, &format!("Failed waiting for last index {} to commit (result={})", last_index, result)) { return 1; }

    let check_points = [0, NUM_ENTRIES / 4, NUM_ENTRIES / 2,
                        3 * NUM_ENTRIES / 4, NUM_ENTRIES - 1];
    for cp in check_points {
        let check_idx = first_index + cp as u64;
        let nc = lab::n_committed(check_idx);
        if !check_msg(nc == NSERVERS as i32, &format!("Entry at index {} (cmd {}) committed by {} servers, expected {}", check_idx, 7201 + cp, nc, NSERVERS)) { return 1; }
    }
    for cp in check_points {
        let check_idx = first_index + cp as u64;
        let expected_cmd = 7201 + cp;
        for s in 0..NSERVERS {
            let svr_id = lab::server_id_by_index(s);
            if !check_msg(lab::server_committed(svr_id, check_idx, expected_cmd), &format!("Server {} missing committed entry at index {} (cmd {})", s, check_idx, expected_cmd)) { return 1; }
        }
    }

    eprintln!("TEST 72: High frequency apply PASSED!");
    passed();
    0
}

// ---------------------------------------------------------------------------
// Tests 73-74 -- startup recovery (plan N7)
//
// Recovery runs only in Setup, and no other case reaches it with snapshots
// on: the lab starts with MAKO_RAFT_SNAPSHOTS unset, and 60 and 69 set it
// after Setup. These build a server outside the cluster, inject a store
// before Setup would, and run Setup's recovery step through its HOST catch
// wrapper -- the store's latest/load, the metadata cross-check, the
// state-machine load, the boundary, the commit clamp and the term raise --
// on whichever store the lane links.

/// The lab state machine's image: the 16-byte (index, term) marker the lab
/// prepare path checks (server.cc, RAFT_TEST_CORO), little-endian.
fn lab_marker(index: u64, term: u64) -> Vec<u8> {
    let mut marker: Vec<u8> = Vec::new();
    for i in 0..8u64 {
        marker.push(((index >> (8 * i)) & 0xff) as u8);
    }
    for i in 0..8u64 {
        marker.push(((term >> (8 * i)) & 0xff) as u8);
    }
    marker
}

/// A store holding one snapshot at (`index`, `term`) whose bytes are the
/// marker for (`index`, `marker_term`).
fn seeded_manager(index: u64, term: u64, marker_term: u64) -> Option<rusty::RaftSnapshotManagerPtr> {
    let manager = new_manager();
    let marker = lab_marker(index, marker_term);
    // SAFETY: the carrier and the bytes are live for the call.
    let saved = unsafe {
        raft_snapshot_store_save(&manager as *const _, index, term, marker.as_ptr(), marker.len())
    };
    if saved { Some(manager) } else { None }
}

/// What a recovery left behind.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
struct Recovered {
    ok: bool,
    stopped: bool,
    snap_index: u64,
    snap_term: u64,
    commit: u64,
    current_term: u64,
    log_base: u64,
    applied: u64,
    has_snapshot: bool,
}

/// Build a server, give it `commit`/`term` as recovered progress and the
/// store (if any), run recovery, read the outcome, delete the server.
fn recover_fresh(store: &rusty::RaftSnapshotManagerPtr, inject: bool, commit: u64,
                 term: u64) -> Recovered {
    // What raft_server_new does, spelled out: the server lives in a Box so
    // the address its runtime registers stays put.
    let s: *mut RaftServerBase = Box::into_raw(Box::new(RaftServerBase::new()));
    // SAFETY: `s` is the live allocation above until the from_raw below.
    let svr: &mut RaftServerBase = unsafe { &mut *s };
    svr.ConstructRuntime();
    if inject {
        svr.SetSnapshotManager(clone_manager(store));
    }
    {
        let _lock = RaftLockGuard::new(svr.LabMutex());
        svr.core.commit_index_ = commit;
        svr.core.current_term_ = term;
    }
    // SAFETY: the server is live; recovery takes its own locks.
    let ok = unsafe { raft_initialize_snapshot_manager(svr.handle(), 0) };
    let stopped = svr.stop_.load(rusty::sync::atomic::Ordering::Acquire);
    let applied = svr.GetAppliedIndex();
    let has_snapshot = svr.HasSnapshot();
    let mut out = {
        let _lock = RaftLockGuard::new(svr.LabMutex());
        Recovered {
            ok,
            stopped,
            snap_index: svr.LabSnapIdx(),
            snap_term: svr.LabSnapTerm() as u64,
            commit: svr.LabCommitIndex(),
            current_term: svr.LabCurrentTerm(),
            log_base: svr.LabLogBase(),
            applied: 0,
            has_snapshot: false,
        }
    };
    out.applied = applied;
    out.has_snapshot = has_snapshot;
    // What raft_server_delete does; this server was never bound to a
    // communicator, so there is nothing to unbind.
    svr.Shutdown();
    // SAFETY: from into_raw above; `svr` is not used after this.
    drop(unsafe { rusty::Box::from_raw(s) });
    out
}

fn test_recovery_restores_injected_snapshot(_st: &mut LabState) -> i32 {
    init2(73, "Startup recovery restores an injected snapshot");
    if !check_msg(set_env("MAKO_RAFT_SNAPSHOTS", "1"), "setenv MAKO_RAFT_SNAPSHOTS") { return 1; }

    // No store injected: recovery starts from an empty one.
    let none = new_manager();
    let empty = recover_fresh(&none, false, 0, 1);
    if !check_msg(empty.ok && !empty.stopped, &format!("empty recovery failed: {:?}", empty)) { return 1; }
    if !check_msg(!empty.has_snapshot && empty.snap_index == 0,
                  &format!("empty recovery reports a snapshot: {:?}", empty)) { return 1; }

    // A seeded store at (40, 3), recovered progress commit 50 at term 1 with
    // an empty log: the boundary becomes 40/3, the commit is clamped to the
    // log's end (40), the term is raised to 3 and the state machine is
    // published as applied through 40.
    let Some(seed) = seeded_manager(40, 3, 3) else {
        failed("could not seed the store");
        return 1;
    };
    let got = recover_fresh(&seed, true, 50, 1);
    if !check_msg(got.ok && !got.stopped, &format!("recovery failed: {:?}", got)) { return 1; }
    if !check_msg(got.snap_index == 40 && got.snap_term == 3,
                  &format!("boundary should be 40/3: {:?}", got)) { return 1; }
    if !check_msg(got.log_base == 41, &format!("log should restart at 41: {:?}", got)) { return 1; }
    if !check_msg(got.commit == 40, &format!("commit should clamp to 40: {:?}", got)) { return 1; }
    if !check_msg(got.current_term == 3, &format!("term should rise to 3: {:?}", got)) { return 1; }
    if !check_msg(got.applied == 40, &format!("applied should publish 40: {:?}", got)) { return 1; }
    if !check_msg(got.has_snapshot, "the recovered server should report its snapshot") { return 1; }
    // The injected store was kept, not replaced: it still holds the seed.
    let after = probe(&seed);
    if !check_msg(after.present && after.last_included_index == 40 && after.count == 1,
                  &format!("the injected store changed: {:?}", after)) { return 1; }
    passed();
    0
}

fn test_recovery_fail_stops(_st: &mut LabState) -> i32 {
    init2(74, "Startup recovery fail-stops without a covering snapshot");
    if !check_msg(set_env("MAKO_RAFT_SNAPSHOTS", "1"), "setenv MAKO_RAFT_SNAPSHOTS") { return 1; }

    // Recovered progress (commit 5, empty log) with an empty store.
    let empty = new_manager();
    let got = recover_fresh(&empty, true, 5, 1);
    if !check_msg(!got.ok && got.stopped,
                  &format!("uncovered progress must fail-stop: {:?}", got)) { return 1; }
    if !check_msg(got.snap_index == 0 && got.commit == 5,
                  &format!("a failed recovery must publish nothing: {:?}", got)) { return 1; }

    // A store whose bytes do not match its metadata: the state machine
    // rejects the image, and the server fail-stops before any boundary.
    let Some(bad) = seeded_manager(40, 3, 2) else {
        failed("could not seed the store");
        return 1;
    };
    let got = recover_fresh(&bad, true, 0, 1);
    if !check_msg(!got.ok && got.stopped,
                  &format!("a rejected image must fail-stop: {:?}", got)) { return 1; }
    if !check_msg(got.snap_index == 0 && got.current_term == 1 && got.applied == 0,
                  &format!("a failed recovery must publish nothing: {:?}", got)) { return 1; }
    passed();
    0
}

// ---------------------------------------------------------------------------
// The driver. Same order and the same short-circuit structure as the second
// half of RaftLabTest::Run.

fn run_snapshot_cases(st: &mut LabState) -> i32 {
    // Tests 50, 51 and 52 -- unit tests of SnapshotMetadata, SnapshotFormat
    // and MemorySnapshotManager, which are C++ classes and touch no
    // RaftServer. See the note at the top of this file.
    // SAFETY: the kernel runs them and returns their combined verdict.
    if unsafe { raft_lab_snapshot_unit_tests() } != 0 {
        return 1;
    }

    type Case = fn(&mut LabState) -> i32;
    let cases: &[Case] = &[
        test_snapshot_manager_wiring,
        test_create_snapshot_basic,
        test_create_snapshot_and_compaction,
        test_snapshot_threshold_configurable,
        test_install_snapshot_basic,
        test_install_snapshot_rejects_stale_term,
        test_heartbeat_triggers_install_snapshot,
        test_heartbeat_interval_configurable,
        test_log_retention_window_configurable,
        test_long_partition_recovery,
        test_high_frequency_apply,
        test_recovery_restores_injected_snapshot,
        test_recovery_fail_stops,
    ];
    for case in cases {
        if case(st) != 0 {
            return 1;
        }
    }
    0
}

pub fn run_snapshot(st: LabState) -> i32 {
    let mut st = st;
    // A named function taking `&mut LabState`, for the reason given at
    // lab_cases::run_basic.
    run_snapshot_cases(&mut st)
}

