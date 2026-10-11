//! The Rust lane's snapshot store (plan phase N3/N4).
//!
//! One slot holding the latest snapshot's `(index, term, bytes)`, owned by
//! Rust: the only snapshot store, as the Rust lane is the only lane (the C++
//! `MemorySnapshotManager` the dropped lanes kept is deleted).
//!
//! The shared core holds a store through the opaque, 16-byte
//! `rusty::RaftSnapshotManagerPtr`. On this lane word 0 is a raw
//! `Arc<SnapshotStore>` and word 1 is always 0; all-zero means "no store",
//! the empty state a null `shared_ptr` has on the other lanes. The SEAM
//! kernels below are the only code that looks inside the carrier here.
//!
//! No streaming reader/writer, no pruning, no format: production calls none
//! of them, and InstallSnapshot ships the state machine's bytes verbatim
//! (plan N2).

use core::ffi::c_void;
use std::collections::HashSet;
use std::sync::{Arc, Mutex};
use std::time::Duration;

use raft::server_h::RaftServerBase;
use srpc::frame_codec::kMaxFramePayloadSize;

use crate::rpc::InstallSnapshotRequestBytesRef;
use crate::transport::{transport_of, InstallSend};

/// One snapshot. `bytes` is the state machine's image, verbatim.
pub struct Snapshot {
    pub index: u64,
    pub term: u64,
    pub bytes: Vec<u8>,
}

/// The single-slot store. The lock is a leaf: nothing here calls back into
/// the server, which the lock order `state_machine_apply_mtx_` -> `mtx_` ->
/// store requires.
pub struct SnapshotStore {
    latest: Mutex<Option<Arc<Snapshot>>>,
}

impl SnapshotStore {
    pub fn new() -> Arc<Self> {
        Arc::new(SnapshotStore { latest: Mutex::new(None) })
    }

    /// The latest snapshot, as a shared image: an `Arc` clone, O(1). A reader
    /// keeps a stable image while a later `save` replaces the slot.
    pub fn latest(&self) -> Option<Arc<Snapshot>> {
        self.latest.lock().unwrap_or_else(|e| e.into_inner()).clone()
    }

    /// Store `bytes` as the latest snapshot at `index`/`term`. Index 0 is
    /// refused (the core never snapshots at 0). With no reader holding the
    /// current image its buffer is reused in place, so steady-state memory
    /// matches the C++ store's `payload_.assign`; otherwise a new image is
    /// built outside the lock, swapped in, and the old one is dropped after
    /// the lock is released.
    pub fn save(&self, index: u64, term: u64, bytes: &[u8]) -> bool {
        if index == 0 {
            return false;
        }
        {
            let mut slot = self.latest.lock().unwrap_or_else(|e| e.into_inner());
            if let Some(current) = slot.as_mut() {
                if let Some(image) = Arc::get_mut(current) {
                    image.index = index;
                    image.term = term;
                    image.bytes.clear();
                    image.bytes.extend_from_slice(bytes);
                    return true;
                }
            }
        }
        self.swap_in(Snapshot { index, term, bytes: bytes.to_vec() });
        true
    }

    /// As `save`, taking ownership of a ready buffer: no copy (the follower's
    /// InstallSnapshot handoff, N5).
    pub fn save_owned(&self, index: u64, term: u64, bytes: Vec<u8>) -> bool {
        if index == 0 {
            return false;
        }
        self.swap_in(Snapshot { index, term, bytes });
        true
    }

    fn swap_in(&self, image: Snapshot) {
        let fresh = Arc::new(image);
        let replaced = {
            let mut slot = self.latest.lock().unwrap_or_else(|e| e.into_inner());
            slot.replace(fresh)
        };
        drop(replaced); // outside the lock
    }

    /// Empty the slot; the number of snapshots removed (0 or 1).
    pub fn clear(&self) -> usize {
        let removed = self.latest.lock().unwrap_or_else(|e| e.into_inner()).take();
        usize::from(removed.is_some())
    }

    /// How many snapshots the store holds: 0 or 1.
    pub fn count(&self) -> usize {
        usize::from(self.latest.lock().unwrap_or_else(|e| e.into_inner()).is_some())
    }
}

// ---------------------------------------------------------------------------
// The carrier: word 0 is a raw Arc<SnapshotStore>, word 1 is 0.
// ---------------------------------------------------------------------------

use crate::seam::{arc_clone, arc_drop, arc_into, set_word, word};

/// The store a carrier holds, if any, as a new strong reference.
///
/// # Safety
/// `m` is a live carrier on this lane (all-zero, or word 0 an
/// `Arc<SnapshotStore>` raw pointer).
pub(crate) unsafe fn store_of(m: *const rusty::RaftSnapshotManagerPtr) -> Option<Arc<SnapshotStore>> {
    if unsafe { word(m as *const u8, 0) } == 0 {
        None
    } else {
        Some(unsafe { arc_clone::<SnapshotStore>(m as *const u8) })
    }
}

/// Release whatever `m` holds and leave it all-zero.
///
/// # Safety
/// `m` is a live carrier on this lane.
unsafe fn release(m: *mut rusty::RaftSnapshotManagerPtr) {
    unsafe {
        arc_drop::<SnapshotStore>(m as *mut u8);
        set_word(m as *mut u8, 0, 0);
        set_word(m as *mut u8, 1, 0);
    }
}

/// Put `store` into `m`, releasing what it held. Word 1 is written
/// explicitly: `arc_into` writes only word 0.
///
/// # Safety
/// `m` is a live carrier on this lane.
pub(crate) unsafe fn put(m: *mut rusty::RaftSnapshotManagerPtr, store: Arc<SnapshotStore>) {
    unsafe {
        release(m);
        arc_into(m as *mut u8, store);
        set_word(m as *mut u8, 1, 0);
    }
}

// ---------------------------------------------------------------------------
// SEAM kernels: the snapshot store's bodies of the names the core's externs
// declare.
// ---------------------------------------------------------------------------

unsafe extern "C" {
    // HOST: fill a std::string carrier from bytes (server.cc).
    fn raft_byte_string_from_bytes(bytes: *const u8, len: usize,
                                   out: *mut rusty::RaftByteString);
}

/// # Safety
/// Every carrier pointer is a live `RaftSnapshotManagerPtr` of this
/// lane (all-zero, or word 0 an `Arc<SnapshotStore>`); every other
/// pointer is valid for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_snapshot_manager_is_set(
    manager: *const rusty::RaftSnapshotManagerPtr) -> bool {
    unsafe { word(manager as *const u8, 0) != 0 }
}

/// A copy of the carrier: one more strong reference to the same store.
/// `dst` is a live (possibly empty) carrier, as for `construct_into`.
///
/// # Safety
/// Every carrier pointer is a live `RaftSnapshotManagerPtr` of this
/// lane (all-zero, or word 0 an `Arc<SnapshotStore>`); every other
/// pointer is valid for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_snapshot_manager_ptr_clone_into(
    src: *const rusty::RaftSnapshotManagerPtr, dst: *mut rusty::RaftSnapshotManagerPtr) {
    if core::ptr::eq(src, dst) {
        return;
    }
    match unsafe { store_of(src) } {
        Some(store) => unsafe { put(dst, store) },
        None => unsafe { release(dst) },
    }
}

/// # Safety
/// Every carrier pointer is a live `RaftSnapshotManagerPtr` of this
/// lane (all-zero, or word 0 an `Arc<SnapshotStore>`); every other
/// pointer is valid for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_destroy_snapshot_manager_ptr(p: *mut rusty::RaftSnapshotManagerPtr) {
    unsafe { release(p) }
}

/// The store is in memory: keep one injected before Setup, otherwise start
/// from an empty one. (Disk builds also write its latest snapshot as an image
/// file, shell/disk.rs.)
///
/// # Safety
/// Every carrier pointer is a live `RaftSnapshotManagerPtr` of this
/// lane (all-zero, or word 0 an `Arc<SnapshotStore>`); every other
/// pointer is valid for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_snapshot_recovery_pick_manager(
    current: *const rusty::RaftSnapshotManagerPtr, out: *mut rusty::RaftSnapshotManagerPtr) {
    let store = unsafe { store_of(current) }.unwrap_or_else(SnapshotStore::new);
    unsafe { put(out, store) }
}

/// # Safety
/// Every carrier pointer is a live `RaftSnapshotManagerPtr` of this
/// lane (all-zero, or word 0 an `Arc<SnapshotStore>`); every other
/// pointer is valid for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_snapshot_manager_latest(
    manager: *const rusty::RaftSnapshotManagerPtr, index: *mut u64, term: *mut u64) -> bool {
    let Some(latest) = (unsafe { store_of(manager) }).and_then(|s| s.latest()) else {
        return false;
    };
    unsafe {
        *index = latest.index;
        *term = latest.term;
    }
    true
}

/// The latest image as a std::string, which prepare_cb takes by
/// `const std::string&`: the one copy that API forces.
///
/// # Safety
/// Every carrier pointer is a live `RaftSnapshotManagerPtr` of this
/// lane (all-zero, or word 0 an `Arc<SnapshotStore>`); every other
/// pointer is valid for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_snapshot_manager_load(
    manager: *const rusty::RaftSnapshotManagerPtr, data: *mut rusty::RaftByteString,
    index: *mut u64, term: *mut u64, size_bytes: *mut u64) -> bool {
    let Some(latest) = (unsafe { store_of(manager) }).and_then(|s| s.latest()) else {
        return false;
    };
    unsafe {
        raft_byte_string_from_bytes(latest.bytes.as_ptr(), latest.bytes.len(), data);
        *index = latest.index;
        *term = latest.term;
        *size_bytes = latest.bytes.len() as u64;
    }
    true
}

/// The store line of the two HOST kernels (create's save and install's
/// save). On the follower's install path the decoded InstallSnapshot buffer
/// is handed over without a copy when it matches (N5); otherwise the bytes
/// are copied from `data`.
///
/// # Safety
/// Every carrier pointer is a live `RaftSnapshotManagerPtr` of this
/// lane (all-zero, or word 0 an `Arc<SnapshotStore>`); every other
/// pointer is valid for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_snapshot_store_save(
    manager: *const rusty::RaftSnapshotManagerPtr, index: u64, term: u64,
    data: *const u8, len: usize) -> bool {
    let Some(store) = (unsafe { store_of(manager) }) else {
        return false;
    };
    if let Some(owned) = take_handoff(index, term, len) {
        return store.save_owned(index, term, owned);
    }
    let bytes = if len == 0 { &[][..] } else { unsafe { core::slice::from_raw_parts(data, len) } };
    store.save(index, term, bytes)
}

// ---------------------------------------------------------------------------
// Disk builds (docs/verus/disk-persistence-plan.md P8): the shell writes the
// latest image to a file, and recovery builds a store from a file's bytes.
// ---------------------------------------------------------------------------

/// Hands the latest snapshot's index, term and bytes to `emit` (one call),
/// borrowed for the call. False if the store is empty or absent.
///
/// # Safety
/// `manager` is a live carrier of this lane; `emit` is safe to call with
/// `ctx` and a slice valid for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_snapshot_manager_with_latest(
    manager: *const rusty::RaftSnapshotManagerPtr, ctx: *mut core::ffi::c_void,
    emit: unsafe extern "C" fn(*mut core::ffi::c_void, u64, u64, *const u8, usize)) -> bool {
    let Some(latest) = (unsafe { store_of(manager) }).and_then(|s| s.latest()) else {
        return false;
    };
    unsafe { emit(ctx, latest.index, latest.term, latest.bytes.as_ptr(), latest.bytes.len()) };
    true
}

/// A new store holding one snapshot, into the all-zero carrier `out`: the
/// image recovery read from its file, injected before Setup (N7 keeps it).
///
/// # Safety
/// `out` is a live, all-zero carrier; `data` is valid for `len` bytes.
#[no_mangle]
pub unsafe extern "C" fn raft_snapshot_manager_from_bytes(
    out: *mut rusty::RaftSnapshotManagerPtr, index: u64, term: u64, data: *const u8, len: usize) -> bool {
    let store = SnapshotStore::new();
    let bytes = if len == 0 { Vec::new() } else { unsafe { core::slice::from_raw_parts(data, len) }.to_vec() };
    if !store.save_owned(index, term, bytes) {
        return false;
    }
    unsafe { put(out, store) };
    true
}

// ---------------------------------------------------------------------------
// The follower's InstallSnapshot handoff (N5). ServeInstallSnapshot runs
// synchronously on the poll thread, so the service parks the decoded buffer
// here, tagged with (index, term, len), for exactly the duration of that
// call; the store takes it when the tag matches, and the service clears the
// slot when the call returns whatever happened.
// ---------------------------------------------------------------------------

thread_local! {
    static HANDOFF: core::cell::RefCell<Option<(u64, u64, Vec<u8>)>> =
        const { core::cell::RefCell::new(None) };
}

/// Park `bytes` for the install at `(index, term)` on this thread.
pub(crate) fn park_handoff(index: u64, term: u64, bytes: Vec<u8>) {
    HANDOFF.with(|h| *h.borrow_mut() = Some((index, term, bytes)));
}

/// Drop whatever is parked on this thread.
pub(crate) fn clear_handoff() {
    HANDOFF.with(|h| *h.borrow_mut() = None);
}

fn take_handoff(index: u64, term: u64, len: usize) -> Option<Vec<u8>> {
    HANDOFF.with(|h| {
        let mut slot = h.borrow_mut();
        match slot.as_ref() {
            Some((i, t, b)) if *i == index && *t == term && b.len() == len => {
                slot.take().map(|(_, _, b)| b)
            }
            _ => None,
        }
    })
}


// ---------------------------------------------------------------------------
// InstallSnapshot, leader side (N5). The image is read as an Arc (O(1), under
// the caller's mtx_) and sent on the Rust transport.
// ---------------------------------------------------------------------------

unsafe extern "C" {
    // HOST (server.cc): the reply context, and its delivery and release.
    fn raft_snapshot_reply_ctx_new(lifetime: *const rusty::RaftAsyncCallbackLifetimePtr,
                                   site_id: u16, self_site_id: u16, ord: usize,
                                   snap_last_idx: u64, send_term: u64) -> *mut c_void;
    fn raft_snapshot_reply_deliver(ctx: *mut c_void, follower_term: u64);
    fn raft_snapshot_reply_free(ctx: *mut c_void);
    // HOST InstallSnapshot counters (server.cc, phase N0).
    fn raft_install_rpc_note_sent(bytes: u64);
}

/// Owns the host's reply context: delivers at most once, frees exactly once
/// on drop, whether or not it delivered. The host context outlives any number
/// of deliveries (at most one happens), and the drop is also what happens,
/// with no delivery, when the send never left, as the C++ lane's commo
/// dropped its std::function uncalled.
struct ReplyCtx(usize);
// SAFETY: the context is only touched on the poll thread, where the send's
// callback runs, and by its single Drop.
unsafe impl Send for ReplyCtx {}

impl ReplyCtx {
    fn deliver(&self, follower_term: u64) {
        unsafe { raft_snapshot_reply_deliver(self.0 as *mut c_void, follower_term) }
    }
}

impl Drop for ReplyCtx {
    fn drop(&mut self) {
        unsafe { raft_snapshot_reply_free(self.0 as *mut c_void) }
    }
}

/// Bytes of an InstallSnapshot frame besides the image: the RPC header and
/// the four u64 fields plus the length prefix, rounded up.
pub const INSTALL_FRAME_OVERHEAD: usize = 128;

/// Whether an image of `len` bytes fits one InstallSnapshot frame.
pub fn install_fits_one_frame(len: usize) -> bool {
    len + INSTALL_FRAME_OVERHEAD <= kMaxFramePayloadSize as usize
}

/// The (term, follower, index) triples already refused for the frame cap, so
/// the refusal is logged once and not every heartbeat round.
static REFUSED: Mutex<Option<HashSet<(u64, u16, u64)>>> = Mutex::new(None);

fn refuse_once(term: u64, site_id: u16, index: u64, len: usize) {
    let mut refused = REFUSED.lock().unwrap_or_else(|e| e.into_inner());
    if refused.get_or_insert_with(HashSet::new).insert((term, site_id, index)) {
        eprintln!("raft-rt: snapshot at index {index} is {len} bytes; one InstallSnapshot \
                   frame is capped at {} bytes, so it cannot be sent to site {site_id}",
                  kMaxFramePayloadSize);
    }
}

/// # Safety
/// `s` is a live server bound to a transport or not; the carrier pointers are
/// live for the call; the caller holds the server's mtx_.
#[no_mangle]
pub unsafe extern "C" fn raft_phase1_load_and_send_snapshot(
    s: *mut RaftServerBase, snapshot_manager: *const rusty::RaftSnapshotManagerPtr,
    lifetime: *const rusty::RaftAsyncCallbackLifetimePtr, self_site_id: u16,
    partition_id: u32, send_term: u64, site_id: u16, ord: usize) -> bool {
    let _ = partition_id;
    let Some(image) = (unsafe { store_of(snapshot_manager) }).and_then(|st| st.latest()) else {
        return false;
    };
    if !install_fits_one_frame(image.bytes.len()) {
        refuse_once(send_term, site_id, image.index, image.bytes.len());
        return false;
    }
    let ctx = ReplyCtx(unsafe {
        raft_snapshot_reply_ctx_new(lifetime, site_id, self_site_id, ord, image.index, send_term)
    } as usize);
    let transport = unsafe { transport_of(s) };
    if transport.map(|t| t.peer(site_id).is_none()).unwrap_or(true) {
        // No peer: failed at once, delivered inline on the caller's stack.
        ctx.deliver(0);
        return true; // `ctx` frees the context.
    }
    let t = transport.expect("checked above");
    // Encoded straight from the stored image: one copy, into the frame. The
    // Arc keeps the image alive for the encode even if a save replaces it.
    let req = InstallSnapshotRequestBytesRef {
        term: send_term,
        leader_id: self_site_id as u64,
        last_included_index: image.index,
        last_included_term: image.term,
        data: &image.bytes,
    };
    let heartbeat_us = unsafe { (*s).GetHeartbeatInterval() };  // [fix, F17] an atomic now
    let deadline = install_deadline(heartbeat_us, image.bytes.len());
    // The callback owns the context: it delivers at most once and frees on
    // drop, whether or not it ever ran. A send that never left, or one
    // suppressed because the same install is outstanding (N6), frees it
    // without a delivery, as the C++ lane's commo did for a failed send.
    let sent = t.send_install_snapshot_once(site_id, &req, deadline, move |follower_term| {
        ctx.deliver(follower_term);
    });
    if sent == InstallSend::Sent {
        unsafe { raft_install_rpc_note_sent(image.bytes.len() as u64) };
    }
    true
}

/// How long an outstanding install suppresses resends (N6): four heartbeat
/// intervals, at least 200 ms, plus 20 ns per byte (50 MB/s, several times
/// slower than the loopback figure in rt/tests/large_frame_bench.rs).
pub fn install_deadline(heartbeat_us: u64, len: usize) -> Duration {
    let base = Duration::from_micros(heartbeat_us.saturating_mul(4)).max(Duration::from_millis(200));
    base + Duration::from_nanos(len as u64 * 20)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn empty_carrier() -> rusty::RaftSnapshotManagerPtr {
        // SAFETY: the carrier is plain storage; all-zero is "no store".
        unsafe { core::mem::zeroed() }
    }

    #[test]
    fn index_zero_is_refused() {
        let store = SnapshotStore::new();
        assert!(!store.save(0, 1, b"x"));
        assert!(!store.save_owned(0, 1, vec![1]));
        assert_eq!(store.count(), 0);
        assert!(store.latest().is_none());
    }

    #[test]
    fn a_reader_keeps_a_stable_image_across_a_save() {
        let store = SnapshotStore::new();
        assert!(store.save(5, 1, b"old"));
        let held = store.latest().unwrap();
        assert!(store.save(9, 2, b"newer"));
        assert_eq!((held.index, held.term, &held.bytes[..]), (5, 1, &b"old"[..]));
        let now = store.latest().unwrap();
        assert_eq!((now.index, now.term, &now.bytes[..]), (9, 2, &b"newer"[..]));
        assert_eq!(store.count(), 1);
    }

    #[test]
    fn with_no_reader_the_buffer_is_reused_in_place() {
        let store = SnapshotStore::new();
        assert!(store.save(1, 1, &[7u8; 4096]));
        let before = store.latest().unwrap().bytes.as_ptr() as usize;
        assert!(store.save(2, 1, &[8u8; 4000]));
        let after = store.latest().unwrap();
        assert_eq!(after.bytes.as_ptr() as usize, before, "same allocation");
        assert_eq!((after.index, after.bytes.len(), after.bytes[0]), (2, 4000, 8));
    }

    #[test]
    fn save_owned_takes_the_buffer_without_a_copy() {
        let store = SnapshotStore::new();
        let buf = vec![4u8; 1 << 16];
        let ptr = buf.as_ptr() as usize;
        assert!(store.save_owned(3, 2, buf));
        assert_eq!(store.latest().unwrap().bytes.as_ptr() as usize, ptr);
    }

    /// Dropped outside the store's lock: a Drop that reads the store would
    /// deadlock otherwise. The image's Drop cannot see the store, so this
    /// checks the observable half: the replaced image dies during the save,
    /// and the lock is free by then (a latest() from the dropping thread's
    /// side succeeds right after).
    #[test]
    fn the_replaced_image_is_released_by_the_save() {
        let store = SnapshotStore::new();
        assert!(store.save(1, 1, b"a"));
        let weak = Arc::downgrade(&store.latest().unwrap());
        let reader = store.latest().unwrap(); // forces swap_in, not reuse
        assert!(store.save(2, 1, b"b"));
        drop(reader);
        assert!(weak.upgrade().is_none(), "the old image is gone");
        assert_eq!(store.latest().unwrap().index, 2);
    }

    #[test]
    fn clear_empties_the_slot() {
        let store = SnapshotStore::new();
        assert_eq!(store.clear(), 0);
        assert!(store.save(4, 1, b"z"));
        assert_eq!(store.clear(), 1);
        assert_eq!(store.count(), 0);
    }

    #[test]
    fn carrier_clone_and_destroy_balance_the_strong_count() {
        let store = SnapshotStore::new();
        let mut a = empty_carrier();
        let mut b = empty_carrier();
        unsafe {
            assert!(!raft_snapshot_manager_is_set(&a));
            put(&mut a, store.clone());
            assert_eq!(Arc::strong_count(&store), 2);
            raft_snapshot_manager_ptr_clone_into(&a, &mut b);
            assert_eq!(Arc::strong_count(&store), 3);
            raft_snapshot_manager_ptr_clone_into(&a, &mut b); // overwrite releases first
            assert_eq!(Arc::strong_count(&store), 3);
            raft_snapshot_manager_ptr_clone_into(&b, &mut b); // self-copy is a no-op
            assert_eq!(Arc::strong_count(&store), 3);
            let empty = empty_carrier();
            raft_snapshot_manager_ptr_clone_into(&empty, &mut b);
            assert!(!raft_snapshot_manager_is_set(&b));
            assert_eq!(Arc::strong_count(&store), 2);
            raft_destroy_snapshot_manager_ptr(&mut a);
            raft_destroy_snapshot_manager_ptr(&mut a); // idempotent on empty
            assert_eq!(Arc::strong_count(&store), 1);
        }
    }

    /// N7: recovery keeps a store injected before Setup -- the same Arc, not
    /// a copy -- and otherwise starts from a new, empty one.
    #[test]
    fn pick_manager_keeps_an_injected_store() {
        let store = SnapshotStore::new();
        assert!(store.save(11, 3, b"seed"));
        let mut current = empty_carrier();
        let mut out = empty_carrier();
        unsafe {
            put(&mut current, store.clone());
            raft_snapshot_recovery_pick_manager(&current, &mut out);
            let picked = store_of(&out).unwrap();
            assert!(Arc::ptr_eq(&picked, &store), "pointer-equal Arc");
            let (mut i, mut t) = (0, 0);
            assert!(raft_snapshot_manager_latest(&out, &mut i, &mut t));
            assert_eq!((i, t), (11, 3));
            drop(picked);
            raft_destroy_snapshot_manager_ptr(&mut current);
            raft_destroy_snapshot_manager_ptr(&mut out);
            assert_eq!(Arc::strong_count(&store), 1);

            let empty = empty_carrier();
            raft_snapshot_recovery_pick_manager(&empty, &mut out);
            let fresh = store_of(&out).unwrap();
            assert!(!Arc::ptr_eq(&fresh, &store));
            assert_eq!(fresh.count(), 0);
            drop(fresh);
            raft_destroy_snapshot_manager_ptr(&mut out);
        }
    }

    #[test]
    fn store_save_uses_a_matching_handoff_only() {
        let store = SnapshotStore::new();
        let mut c = empty_carrier();
        unsafe { put(&mut c, store.clone()) };
        let buf = vec![9u8; 100];
        let ptr = buf.as_ptr() as usize;
        park_handoff(20, 4, buf);
        let other = [1u8; 100];
        // a different index: copied from `data`, the handoff stays parked
        assert!(unsafe { raft_snapshot_store_save(&c, 21, 4, other.as_ptr(), other.len()) });
        assert_eq!(store.latest().unwrap().bytes[0], 1);
        // the matching one: the parked buffer itself
        assert!(unsafe { raft_snapshot_store_save(&c, 20, 4, other.as_ptr(), other.len()) });
        let got = store.latest().unwrap();
        assert_eq!((got.bytes.as_ptr() as usize, got.bytes[0]), (ptr, 9));
        clear_handoff();
        unsafe { raft_destroy_snapshot_manager_ptr(&mut c) };
        // no store: refused
        let empty = empty_carrier();
        assert!(!unsafe { raft_snapshot_store_save(&empty, 1, 1, other.as_ptr(), 1) });
    }

    #[test]
    fn four_threads_see_only_whole_images() {
        let store = SnapshotStore::new();
        assert!(store.save(1, 1, &[1u8; 1024]));
        let stop = Arc::new(std::sync::atomic::AtomicBool::new(false));
        let mut threads = Vec::new();
        for w in 0..2u64 {
            let (store, stop) = (store.clone(), stop.clone());
            threads.push(std::thread::spawn(move || {
                let mut i = 2 + w;
                while !stop.load(std::sync::atomic::Ordering::Relaxed) {
                    let fill = (i % 251) as u8;
                    assert!(store.save(i, i, &vec![fill; 512 + (i as usize % 1024)]));
                    i += 2;
                }
            }));
        }
        for _ in 0..2 {
            let (store, stop) = (store.clone(), stop.clone());
            threads.push(std::thread::spawn(move || {
                while !stop.load(std::sync::atomic::Ordering::Relaxed) {
                    let img = store.latest().unwrap();
                    let fill = (img.index % 251) as u8;
                    assert_eq!(img.index, img.term);
                    assert!(img.index == 1 || img.bytes.len() == 512 + (img.index as usize % 1024));
                    assert!(img.index == 1 || img.bytes.iter().all(|b| *b == fill), "torn image");
                }
            }));
        }
        std::thread::sleep(std::time::Duration::from_millis(500));
        stop.store(true, std::sync::atomic::Ordering::Relaxed);
        for t in threads {
            t.join().unwrap();
        }
        assert_eq!(Arc::strong_count(&store), 1);
        assert_eq!(store.count(), 1);
    }
}
