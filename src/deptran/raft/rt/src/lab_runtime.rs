// The RaftLab harness's runtime.
//
// The harness fiber is a Rust srpc fiber on the calling thread's Rust
// reactor, because the harness sleeps through raft_fiber_sleep_us, the Rust
// Fiber::sleep, which needs a Rust reactor to yield to.

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
        // The harness itself, in the core (src/deptran/raft/shell/lab.rs).
        out.set(raft::lab_main::raft_lab_rust_run());
        Reactor::get_reactor().looping_.set(false);
    });
    // create_run ran the fiber until its first suspension; if the harness
    // already finished (it never does -- it sleeps), the loop exits at once.
    if verdict.get() == -1 {
        Reactor::get_reactor().run_loop(true, true);
    }
    verdict.get()
}

// ---------------------------------------------------------------------------
// The snapshot lab kernels, over SnapshotStore (plan N4).
// ---------------------------------------------------------------------------

use crate::snapshot::{put, store_of, SnapshotStore};

/// Everything a case compares across a snapshot operation; the layout of
/// `SnapshotProbe` in src/lab_snapshot_cases.rs. On this lane the store has
/// no timestamp or checksum, so those two stay 0; test 58 compares probes
/// only within one process.
#[repr(C)]
struct SnapshotProbe {
    present: bool,
    last_included_index: u64,
    last_included_term: u64,
    timestamp_ms: u64,
    size_bytes: u64,
    checksum_digest: u64,
    data_digest: u64,
    count: u64,
}

fn fnv1a(bytes: &[u8]) -> u64 {
    let mut digest: u64 = 0xcbf2_9ce4_8422_2325;
    for &b in bytes {
        digest ^= u64::from(b);
        digest = digest.wrapping_mul(0x0000_0100_0000_01b3);
    }
    digest
}

/// # Safety
/// `out` is a live carrier of this lane.
#[no_mangle]
pub unsafe extern "C" fn raft_lab_new_snapshot_manager(out: *mut rusty::RaftSnapshotManagerPtr) {
    unsafe { put(out, SnapshotStore::new()) }
}

/// # Safety
/// `manager` is a live carrier of this lane.
#[no_mangle]
pub unsafe extern "C" fn raft_lab_snapshot_delete_all(
    manager: *const rusty::RaftSnapshotManagerPtr) -> u64 {
    unsafe { store_of(manager) }.map_or(0, |s| s.clear() as u64)
}

/// # Safety
/// `manager` is a live carrier of this lane; `out` points at a `SnapshotProbe`.
#[no_mangle]
pub unsafe extern "C" fn raft_lab_snapshot_probe(
    manager: *const rusty::RaftSnapshotManagerPtr, out: *mut core::ffi::c_void) {
    let mut probe = SnapshotProbe {
        present: false, last_included_index: 0, last_included_term: 0, timestamp_ms: 0,
        size_bytes: 0, checksum_digest: 0, data_digest: 0, count: 0,
    };
    if let Some(store) = unsafe { store_of(manager) } {
        if let Some(latest) = store.latest() {
            probe.present = true;
            probe.last_included_index = latest.index;
            probe.last_included_term = latest.term;
            probe.size_bytes = latest.bytes.len() as u64;
            probe.data_digest = fnv1a(&latest.bytes);
            probe.count = store.count() as u64;
        }
    }
    unsafe { (out as *mut SnapshotProbe).write(probe) }
}

/// # Safety
/// Both are live carriers of this lane.
#[no_mangle]
pub unsafe extern "C" fn raft_lab_snapshot_copy_latest(
    src: *const rusty::RaftSnapshotManagerPtr, dst: *const rusty::RaftSnapshotManagerPtr) -> bool {
    let (Some(from), Some(to)) = (unsafe { store_of(src) }, unsafe { store_of(dst) }) else {
        return false;
    };
    match from.latest() {
        Some(image) => to.save(image.index, image.term, &image.bytes),
        None => false,
    }
}

// ---------------------------------------------------------------------------
// Tests 50-52 on the Rust lane (plan N10): new tests under the old numbers,
// over SnapshotStore, printing the lines ci.sh counts (`TEST N:` to derive
// the expected count, `^TEST N Passed` for the passes).
// ---------------------------------------------------------------------------

fn unit_check(id: i32, ok: bool, msg: &str) -> bool {
    if !ok {
        eprintln!("TEST {id} Failed: {msg}");
    }
    ok
}

fn test_50_index_zero_refused() -> i32 {
    eprintln!("TEST 50: SnapshotStore refuses index 0");
    let store = SnapshotStore::new();
    if !unit_check(50, !store.save(0, 1, b"x"), "save at index 0 must be refused") { return 1; }
    if !unit_check(50, store.latest().is_none(), "a refused save must leave the slot empty") { return 1; }
    if !unit_check(50, store.save(1, 1, b"x"), "save at index 1 must be accepted") { return 1; }
    if !unit_check(50, !store.save(0, 2, b"y"), "index 0 is refused over an image too") { return 1; }
    let kept = store.latest();
    if !unit_check(50, kept.as_ref().map(|s| (s.index, s.term, s.bytes.clone())) == Some((1, 1, b"x".to_vec())),
                   "a refused save must leave the image as it was") { return 1; }
    eprintln!("TEST 50 Passed");
    0
}

fn test_51_byte_exact_round_trip() -> i32 {
    eprintln!("TEST 51: SnapshotStore round-trips bytes exactly");
    let store = SnapshotStore::new();
    let non_utf8: Vec<u8> = vec![0xc3, 0x28, 0xa0, 0xa1, 0xe2, 0x28, 0xa1];
    let high: Vec<u8> = vec![0x00, 0x7f, 0x80, 0xfe, 0xff];
    for (i, payload) in [Vec::new(), non_utf8, high].into_iter().enumerate() {
        let index = i as u64 + 1;
        if !unit_check(51, store.save(index, 7, &payload), "save must succeed") { return 1; }
        let got = store.latest();
        if !unit_check(51, got.as_ref().map(|s| (s.index, s.term)) == Some((index, 7)),
                       "metadata must round-trip") { return 1; }
        if !unit_check(51, got.map(|s| s.bytes.clone()) == Some(payload),
                       "bytes must round-trip exactly") { return 1; }
    }
    eprintln!("TEST 51 Passed");
    0
}

fn test_52_save_load_overwrite_clear() -> i32 {
    eprintln!("TEST 52: SnapshotStore save, load, overwrite and clear");
    let store = SnapshotStore::new();
    if !unit_check(52, store.count() == 0, "a new store is empty") { return 1; }
    if !unit_check(52, store.save(10, 2, b"first"), "first save") { return 1; }
    if !unit_check(52, store.save(11, 2, b"second"), "overwrite in place") { return 1; }
    let reader = store.latest();
    if !unit_check(52, store.save(12, 3, b"third"), "overwrite under a reader") { return 1; }
    if !unit_check(52, reader.as_ref().map(|s| s.bytes.as_slice()) == Some(&b"second"[..]),
                   "a reader keeps the image it took") { return 1; }
    if !unit_check(52, store.latest().map(|s| (s.index, s.bytes.clone())) == Some((12, b"third".to_vec())),
                   "the store holds the newest image") { return 1; }
    if !unit_check(52, store.count() == 1, "one slot") { return 1; }
    if !unit_check(52, store.clear() == 1 && store.count() == 0 && store.latest().is_none(),
                   "clear empties the slot") { return 1; }
    eprintln!("TEST 52 Passed");
    0
}

/// Tests 50-52, in order. 0 on success.
#[no_mangle]
pub extern "C" fn raft_lab_snapshot_unit_tests() -> i32 {
    for test in [test_50_index_zero_refused, test_51_byte_exact_round_trip,
                 test_52_save_load_overwrite_clear] {
        let r = test();
        if r != 0 {
            return r;
        }
    }
    0
}
