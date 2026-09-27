// The Rust lane's runtime seam: the reactor- and socket-facing kernels the
// Raft core calls, implemented over the Rust srpc crate.
//
// WHAT THIS IS. The core (src/deptran/raft/src) never names a runtime. It
// calls a fixed set of `extern "C"` kernels -- spawn a fiber, sleep, wait on
// an event, queue a wake job, send an RPC -- and carries the runtime's handles
// as opaque carriers whose bytes it never reads (src/rusty-rustc). In the C++
// lane those kernels are server_seam_cpp.cc, over the C++ srpc runtime. Here
// they are the same symbols over the Rust srpc runtime, so the same core
// source runs on either reactor. Only one of the two files is ever linked.
//
// HOW THE CARRIERS ARE FILLED. Each carrier is sized for its C++ type, and
// the Rust value stored in it is chosen to fit:
//
//   RaftIntEventPtr   8 bytes   one Arc<IntEvent>, as a raw pointer
//   RaftPollThreadPtr 8 bytes   one Arc<PollThread>, as a raw pointer
//   RaftResponsePtr   16 bytes  word 0 a tag (0 empty, 1 failed, 2 pending),
//                               word 1 the pending reply's raw Arc
//   RaftVoteQuorumPtr 16 bytes  word 0 a raw Arc<VoteWait>, 0 when empty
//
// The 16-byte carriers derive Default as all-zero bytes, so "empty" is zero
// and destroying a default slot is a no-op, as it is for the C++ shared_ptr.
// The 8-byte ones have no empty state, exactly like rusty::Arc.
//
// THREADS. Everything here runs on the transport's poll thread -- the
// heartbeat and election fibers live there, and so do the reply callbacks --
// except raft_queue_wake_job and raft_commo_set_network_enabled, which touch
// only a channel and an atomic.

#![allow(clippy::missing_safety_doc)]

use core::ffi::c_void;
use std::sync::Arc;
use std::time::{Duration, Instant};

use raft::server_h::RaftServerBase;
use raft::server_pods_h::{AppendRespView, RaftVoteOutcome};
use srpc::misc::{Job, OneTimeJob};
use srpc::reactor::{create_sp_int_event, Fiber, IntEvent, PollThread};

use crate::rpc::{AppendEntriesRequest, EmptyAppendEntriesRequest,
                 VoteRequest};
use crate::transport::{transport_of, AppendReply, VoteTally};

// ---------------------------------------------------------------------------
// Carrier plumbing
// ---------------------------------------------------------------------------

#[inline]
pub(crate) unsafe fn word(p: *const u8, i: usize) -> usize {
    unsafe { (p as *const usize).add(i).read() }
}
#[inline]
pub(crate) unsafe fn set_word(p: *mut u8, i: usize, v: usize) {
    unsafe { (p as *mut usize).add(i).write(v) }
}

pub(crate) unsafe fn arc_into<T>(dst: *mut u8, value: Arc<T>) {
    unsafe { set_word(dst, 0, Arc::into_raw(value) as usize) }
}
pub(crate) unsafe fn arc_ref<'a, T>(src: *const u8) -> &'a T {
    unsafe { &*(word(src, 0) as *const T) }
}
pub(crate) unsafe fn arc_clone<T>(src: *const u8) -> Arc<T> {
    let raw = unsafe { word(src, 0) } as *const T;
    unsafe {
        Arc::increment_strong_count(raw);
        Arc::from_raw(raw)
    }
}
pub(crate) unsafe fn arc_drop<T>(p: *mut u8) {
    let raw = unsafe { word(p, 0) } as *const T;
    if !raw.is_null() {
        drop(unsafe { Arc::from_raw(raw) });
    }
}

// ---------------------------------------------------------------------------
// Fiber events
// ---------------------------------------------------------------------------

#[no_mangle]
pub unsafe extern "C" fn raft_create_int_event_into(out: *mut rusty::RaftIntEventPtr) {
    // The same factory and target the C++ lane uses: create_sp_int_event(1).
    unsafe { arc_into(out as *mut u8, create_sp_int_event(1)) }
}

#[no_mangle]
pub unsafe extern "C" fn raft_int_event_clone_into(src: *const rusty::RaftIntEventPtr,
                                                   dst: *mut rusty::RaftIntEventPtr) {
    unsafe { arc_into(dst as *mut u8, arc_clone::<IntEvent>(src as *const u8)) }
}

#[no_mangle]
pub unsafe extern "C" fn raft_destroy_int_event_ptr(p: *mut rusty::RaftIntEventPtr) {
    unsafe { arc_drop::<IntEvent>(p as *mut u8) }
}

/// The thread binding, CHECKED. An IntEvent is neither Send nor Sync -- its
/// `Cell`s and its fiber back-pointer belong to the poll thread that created
/// it -- yet the carrier holding it sits inside RaftServerBase, which is Send +
/// Sync. That is sound only because every set and wait happens on the owner
/// thread (the heartbeat and election fibers, and the wake job, all run
/// there), so it is asserted here rather than trusted: a violation aborts
/// with a message instead of racing silently. (Plan S2: this replaces moving
/// the handles out of the core, which would have duplicated the wake gate in
/// both lanes' seams.)
fn owner_thread_check(ev: &IntEvent, op: &str) {
    if ev.owner_thread_ != std::thread::current().id() {
        eprintln!("raft-rt: IntEvent {op} off its owner poll thread ({:?} vs {:?})",
                  std::thread::current().id(), ev.owner_thread_);
        std::process::abort();
    }
}

#[no_mangle]
pub unsafe extern "C" fn raft_int_event_set(event: *const rusty::RaftIntEventPtr, value: i32) {
    let ev = unsafe { arc_ref::<IntEvent>(event as *const u8) };
    owner_thread_check(ev, "set");
    ev.set(value);
}

#[no_mangle]
pub unsafe extern "C" fn raft_int_event_wait_timeout(event: *const rusty::RaftIntEventPtr,
                                                     timeout_us: u64) {
    let ev = unsafe { arc_ref::<IntEvent>(event as *const u8) };
    owner_thread_check(ev, "wait");
    ev.wait_timeout(timeout_us);
}

// ---------------------------------------------------------------------------
// The poll thread and the wake job
// ---------------------------------------------------------------------------

#[no_mangle]
pub unsafe extern "C" fn raft_poll_thread_clone_into(src: *const rusty::RaftPollThreadPtr,
                                                     dst: *mut rusty::RaftPollThreadPtr) {
    unsafe { arc_into(dst as *mut u8, arc_clone::<PollThread>(src as *const u8)) }
}

#[no_mangle]
pub unsafe extern "C" fn raft_destroy_poll_thread_ptr(p: *mut rusty::RaftPollThreadPtr) {
    unsafe { arc_drop::<PollThread>(p as *mut u8) }
}

/// The wake job's token, as the job closure carries it across the channel.
/// It is a `Box<GateWakeJob>` made raw by RaftServerBase::queue_wake_job and
/// taken back exactly once by raft_wake_job_run; nothing here reads it.
struct WakeToken(usize);
// SAFETY: the token is an owned Box handed to the poll thread and consumed
// there once; no other thread touches it after the send.
unsafe impl Send for WakeToken {}
unsafe impl Sync for WakeToken {}

#[no_mangle]
pub unsafe extern "C" fn raft_queue_wake_job(owner: *const rusty::RaftPollThreadPtr,
                                             token: *mut c_void) {
    let poll = unsafe { arc_ref::<PollThread>(owner as *const u8) };
    let token = WakeToken(token as usize);
    let job = OneTimeJob::new(Box::new(move || {
        // SAFETY: see WakeToken.
        unsafe { raft::server_cc::raft_wake_job_run(token.0 as *mut c_void) };
    }));
    poll.add(Arc::new(job) as Arc<dyn Job>);
}

/// Binds the wake gate to the transport's poll thread. The C++ lane binds the
/// communicator's; the Rust lane's owner is the one poll thread the transport
/// created, which is also where the heartbeat fiber runs.
#[no_mangle]
pub unsafe extern "C" fn raft_bind_replication_poll(s: *mut RaftServerBase) -> bool {
    let Some(t) = (unsafe { transport_of(s) }) else {
        return false;
    };
    let mut owner = core::mem::MaybeUninit::<rusty::RaftPollThreadPtr>::uninit();
    unsafe {
        arc_into(owner.as_mut_ptr() as *mut u8, t.poll_thread());
        let owner = owner.assume_init();
        raft::server_cc::raft_server_bind_replication_wake_owner(s, &owner);
    }
    true
}

// ---------------------------------------------------------------------------
// Fibers
// ---------------------------------------------------------------------------

/// A server pointer inside a fiber closure. Fibers are thread-local, so the
/// closure never crosses a thread; the wrapper exists only because a raw
/// pointer is not 'static-safe to name in a closure type without one.
#[derive(Clone, Copy)]
struct ServerPtr(*mut RaftServerBase);

#[no_mangle]
pub unsafe extern "C" fn raft_spawn_heartbeat_loop(s: *mut RaftServerBase) {
    let server = ServerPtr(s);
    Fiber::create_run(move || unsafe { raft::server_cc::raft_server_heartbeat_loop(server.0) });
}

#[no_mangle]
pub unsafe extern "C" fn raft_spawn_election_timer_fiber(s: *mut RaftServerBase) {
    let server = ServerPtr(s);
    Fiber::create_run(move || unsafe { raft::server_cc::raft_server_start_election_timer(server.0) });
}

#[no_mangle]
pub unsafe extern "C" fn raft_spawn_election_timer(s: *mut RaftServerBase, wait_int_us: u64) {
    let server = ServerPtr(s);
    Fiber::create_run(move || unsafe {
        raft::server_cc::raft_server_run_election_timer_loop(server.0, wait_int_us)
    });
}

#[no_mangle]
pub extern "C" fn raft_fiber_sleep_us(micros: u64) {
    Fiber::sleep(micros.max(1));
}

/// The shutdown barrier's yield: a fiber sleeps as a fiber; production
/// shutdown runs on a native thread, where a short native sleep lets the
/// poll thread drain both loop fibers.
#[no_mangle]
pub extern "C" fn raft_shutdown_barrier_yield() {
    if Fiber::current_fiber().is_some() {
        Fiber::sleep(1000);
    } else {
        std::thread::sleep(Duration::from_millis(1));
    }
}

// ---------------------------------------------------------------------------
// The communicator's operations
// ---------------------------------------------------------------------------

/// The C++ lane validates and binds a communicator here. The Rust lane has no
/// communicator: the transport is bound by raft_transport_serve, and the
/// worker never calls set_commo. Reaching this is a wiring bug.
#[no_mangle]
pub unsafe extern "C" fn raft_bind_commo(_s: *mut RaftServerBase, _commo: *mut c_void) {
    eprintln!("raft-rt: set_commo called on the Rust lane; the transport is bound by raft_transport_serve");
    std::process::abort();
}

/// Nothing to unbind: raft_transport_delete unbinds the transport.
#[no_mangle]
pub unsafe extern "C" fn raft_unbind_commo(_s: *mut RaftServerBase) {}

#[no_mangle]
pub unsafe extern "C" fn raft_commo_set_network_enabled(s: *mut RaftServerBase, enabled: bool) {
    if let Some(t) = unsafe { transport_of(s) } {
        t.set_network_enabled(enabled);
    }
}

/// One campaign: the broadcast, then a wait of at most one second for the
/// quorum to decide, exactly as the C++ kernel waits on RaftVoteQuorumEvent.
///
/// The wait polls the tally from the campaigning fiber rather than parking on
/// an event. A reply callback cannot hold an IntEvent -- the callback must be
/// Send and IntEvent is neither Send nor Sync -- and elections are not a hot
/// path, so a 200 us poll is the simple correct shape. The callbacks run on
/// this same poll thread, and Fiber::sleep yields to them.
#[no_mangle]
pub unsafe extern "C" fn raft_broadcast_vote_and_wait(
    s: *mut RaftServerBase, par_id: u32, last_log_index: u64, last_log_term: i64,
    self_site_id: u16, term: i64, out: *mut rusty::RaftVoteQuorumPtr) {
    let req = VoteRequest {
        lst_log_idx: last_log_index,
        lst_log_term: last_log_term,
        site_id: self_site_id,
        cur_term: term,
    };
    let tally = match unsafe { transport_of(s) } {
        Some(t) => t.broadcast_vote(par_id, self_site_id, &req),
        None => VoteTally::unreachable(),
    };
    let deadline = Instant::now() + Duration::from_micros(1_000_000);
    let mut timed_out = false;
    while !tally.decided() {
        if Instant::now() >= deadline {
            timed_out = true;
            break;
        }
        Fiber::sleep(200);
    }
    let wait = Arc::new(VoteWait { tally, timed_out });
    unsafe {
        vote_quorum_release(out);
        arc_into(out as *mut u8, wait);
    }
}

/// A finished campaign, as the core carries it to raft_vote_quorum_snapshot.
pub struct VoteWait {
    tally: VoteTally,
    timed_out: bool,
}

unsafe fn vote_quorum_release(p: *mut rusty::RaftVoteQuorumPtr) {
    unsafe {
        arc_drop::<VoteWait>(p as *mut u8);
        set_word(p as *mut u8, 0, 0);
        set_word(p as *mut u8, 1, 0);
    }
}

#[no_mangle]
pub unsafe extern "C" fn raft_vote_quorum_snapshot(q: *const rusty::RaftVoteQuorumPtr)
    -> RaftVoteOutcome {
    let wait = unsafe { arc_ref::<VoteWait>(q as *const u8) };
    wait.tally.outcome(wait.timed_out)
}

#[no_mangle]
pub unsafe extern "C" fn raft_destroy_vote_quorum_ptr(p: *mut rusty::RaftVoteQuorumPtr) {
    unsafe { vote_quorum_release(p) }
}

// ---------------------------------------------------------------------------
// AppendEntries
// ---------------------------------------------------------------------------

const RESP_EMPTY: usize = 0;
const RESP_FAILED: usize = 1;
const RESP_PENDING: usize = 2;

unsafe fn response_release(p: *mut rusty::RaftResponsePtr) {
    let raw = p as *mut u8;
    unsafe {
        if word(raw, 0) == RESP_PENDING {
            drop(Arc::from_raw(word(raw, 1) as *const AppendReply));
        }
        set_word(raw, 0, RESP_EMPTY);
        set_word(raw, 1, 0);
    }
}

unsafe extern "C" {
    // HOST kernel (server.cc): the payload's wire bytes, exactly what the C++
    // lane's rcc_rpc.h writes for `Command cmd` -- the envelope, unframed.
    fn raft_command_encode(cmd: *const rusty::RaftCommand, ctx: *mut c_void,
                           emit: unsafe extern "C" fn(*mut c_void, *const u8, usize));
    fn raft_command_has_value(cmd: *const rusty::RaftCommand) -> bool;
}

/// C++'s serializer writes the Command through this, straight into the
/// request archive the client is building.
unsafe extern "C" fn emit_into_archive(ctx: *mut c_void, bytes: *const u8, len: usize) {
    let ar = unsafe { &mut *(ctx as *mut srpc::serializable::BinaryWriteArchive) };
    unsafe { ar.write_bytes(bytes, len) };
}

/// The send. Non-blocking: it only initiates the call. The reply lands in the
/// Pending the carrier holds, which PHASE 2 polls through
/// raft_append_response_read, as it polls `completed` in the C++ lane.
/// Commands with no payload go as EmptyAppendEntries, as commo.cc:69 does.
#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn raft_phase1_send_append(
    s: *mut RaftServerBase, self_site_id: u16, site_id: u16, partition_id: u32,
    _is_leader: bool, term: u64, prev_log_index: u64, prev_log_term: u64,
    commit_index: u64, cmd: *const rusty::RaftCommand, cmd_log_term: u64,
    out: *mut rusty::RaftResponsePtr) {
    let _ = partition_id;
    let pending = match unsafe { transport_of(s) } {
        None => None,
        Some(t) => {
            if unsafe { raft_command_has_value(cmd) } {
                let req = AppendEntriesRequest {
                    // slotid_t -1, as the C++ lane sends it, wrapped to u64.
                    slot: u64::MAX,
                    ballot: -1,
                    leader_current_term: term,
                    leader_site_id: self_site_id,
                    leader_prev_log_index: prev_log_index,
                    leader_prev_log_term: prev_log_term,
                    leader_commit_index: commit_index,
                    // Unused: send_append_entries_with writes the payload
                    // from `cmd` directly into the frame below.
                    cmd: Vec::new(),
                    leader_next_log_term: cmd_log_term,
                };
                t.send_append_entries_with(site_id, &req, |ar| unsafe {
                    raft_command_encode(cmd, ar as *mut _ as *mut c_void, emit_into_archive);
                })
            } else {
                let req = EmptyAppendEntriesRequest {
                    // slotid_t -1, as the C++ lane sends it, wrapped to u64.
                    slot: u64::MAX,
                    ballot: -1,
                    leader_current_term: term,
                    leader_site_id: self_site_id,
                    leader_prev_log_index: prev_log_index,
                    leader_prev_log_term: prev_log_term,
                    leader_commit_index: commit_index,
                };
                t.send_empty_append_entries(site_id, &req)
            }
        }
    };
    unsafe {
        response_release(out);
        let raw = out as *mut u8;
        match pending {
            // No peer, network down, or the send never left: completed and
            // failed at once, which is what commo.cc:43-45 does.
            None => set_word(raw, 0, RESP_FAILED),
            Some(reply) => {
                set_word(raw, 0, RESP_PENDING);
                set_word(raw, 1, Arc::into_raw(Arc::new(reply)) as usize);
            }
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn raft_append_response_read(r: *const rusty::RaftResponsePtr)
    -> AppendRespView {
    let raw = r as *const u8;
    let mut view = AppendRespView { completed_: false, status_: false, term_: 0,
                                    last_log_index_: 0 };
    match unsafe { word(raw, 0) } {
        RESP_PENDING => {
            let reply = unsafe { &*(word(raw, 1) as *const AppendReply) };
            if let Some(result) = reply.peek() {
                view.completed_ = true;
                if let Ok(resp) = result {
                    view.status_ = resp.follower_append_ok != 0;
                    view.term_ = resp.follower_current_term;
                    view.last_log_index_ = resp.follower_last_log_index;
                }
            }
        }
        // Failed, or never sent: completed with status 0, the C++ lane's
        // default-initialised response.
        _ => view.completed_ = true,
    }
    view
}

#[no_mangle]
pub unsafe extern "C" fn raft_destroy_response_ptr(p: *mut rusty::RaftResponsePtr) {
    unsafe { response_release(p) }
}

// ---------------------------------------------------------------------------
// InstallSnapshot: rt/src/snapshot.rs (plan N4/N5), with the Rust-lane store.
// ---------------------------------------------------------------------------
