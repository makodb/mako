// The Raft server's second half: the C ABI exports, the inbound RPC bodies
// and the heartbeat phases. rustc compiles this into libraft.a; nothing here
// is transpiled, so edit it directly.

#[allow(dead_code, non_snake_case)]
fn IsPreferredLeaderConfigured(preferred_leader_site_id: u16) -> bool {
    preferred_leader_site_id != u16::MAX
}

// [move, M1] The heartbeat round's core calls and their types live in
// raft-core (Phase 6).
use raft_core::{heartbeat_abandon_round, heartbeat_on_reply, heartbeat_round_end,
                heartbeat_tick, AppendPayload, ReplyResult, SnapshotSend};
use crate::server_h::{core_output, AppendSend, HeartbeatTick};
use crate::server_h::CoreOutput;  // [move, M3]
use crate::server_h::AppendResponses;  // [move, M5]
// [move, M1] the heartbeat round's state, moved into server_h.rs with RaftCore

use crate::server_h::RaftServerBase;
use crate::server_pods_h::AppendRespView;
use crate::server_pods_h::RaftServerHandle;
use crate::scheduler_h::RaftSpecific;
use crate::server_h::RaftLockGuard;
// Every C++ kernel this carrier calls, in one place. improper_ctypes is
// allowed because each of these passes an opaque handle by pointer and
// nothing is laid out across the boundary -- the same case as the allow
// on server.h's bridge block.
#[allow(improper_ctypes)]
unsafe extern "C" {
    fn raft_monotonic_now_us() -> u64;
    fn raft_fiber_sleep_us(micros: u64);
    fn raft_trace_through(stage: i32, through: u64, t_us: u64);  // [M0] trace kit
    fn raft_append_response_read(response: *const rusty::RaftResponsePtr)
        -> AppendRespView;
    // Drops this server's row from the C++ commo table; called from
    // raft_server_delete. See RaftServerBase::set_commo.
    fn raft_unbind_commo(server: *mut RaftServerHandle);
    fn raft_snapshot_manager_is_set(
        manager: *const rusty::RaftSnapshotManagerPtr) -> bool;
    fn raft_phase1_load_and_send_snapshot(
        server: *mut RaftServerHandle,
        snapshot_manager: *const rusty::RaftSnapshotManagerPtr,
        lifetime: *const rusty::RaftAsyncCallbackLifetimePtr,
        self_site_id: u16, partition_id: u32, send_term: u64,
        site_id: u16, ord: usize) -> bool;
    fn raft_batch_optimization_enabled() -> bool;
    fn raft_append_entries_batch_max() -> u64;
    fn raft_append_entries_batch_max_bytes() -> u64;
    // [move, M6] raft_command_payload_bytes, raft_command_kind and
    // raft_command_is_tpc_commit are read once per entry into RaftEntry now
    // (raft_command_meta). The leader's batch: a TpcCommitCommand is copied
    // and stamped with its log term in C++ (a Marshallable) by the facade's
    // `raft_stamped_commit`, pushed into batch_buffer_ in Rust, and the
    // buffer's Arcs are moved into one TpcBatchCommand at the end.
    fn raft_batch_finalize(entries: *mut rusty::RaftTpcCommitPtr,
                           count: usize, cmd_out: *mut rusty::RaftCommand);
    // The command copy INTO Rust's slot; see server.h for why never by value.
    fn raft_command_clone_into(src: *const rusty::RaftCommand,
                               dst: *mut rusty::RaftCommand);
    fn raft_phase1_send_append(server: *mut RaftServerHandle,
                               self_site_id: u16, site_id: u16,
                               partition_id: u32, is_leader: bool, term: u64,
                               prev_log_index: u64, prev_log_term: u64,
                               commit_index: u64,
                               cmd: *const rusty::RaftCommand,
                               cmd_log_term: u64,
                               out: *mut rusty::RaftResponsePtr);
}

// [move, M11] A command handle's copy: a refcount bump on its inner Arc,
// made by the kernel, never the carrier's clone (see server.h). The handle is
// opaque to the core; this copies it without looking inside.
fn raft_command_handle_clone(cmd: &rusty::RaftCommand) -> rusty::RaftCommand {
    let mut copy: rusty::RaftCommand = Default::default();
    unsafe {
        raft_command_clone_into(cmd as *const rusty::RaftCommand,
                                &mut copy as *mut rusty::RaftCommand);
    }
    copy
}

// [move, M5] PHASE 0 and PHASE 1 around their core call: the decision under
// mtx_ and its actions (with any InstallSnapshot kernel) before the guard
// drops, then each AppendEntries built and sent in follower order
// ([fix, F7]). Returns the tick for the rest of the round.
pub fn heartbeat_tick_body(server: &mut RaftServerBase) -> HeartbeatTick {
    let mut out: CoreOutput = core_output();
    let tick: HeartbeatTick = {
        let _lock = RaftLockGuard::new(&mut server.mtx_);
        let is_leader: bool = server.IsLeaderLocked();
        let snapshot_configured: bool = unsafe {
            raft_snapshot_manager_is_set(&server.snapshot_manager_)
        };
        let batching: bool = unsafe { raft_batch_optimization_enabled() };
        let max_batch_entries: u64 = unsafe { raft_append_entries_batch_max() };
        let max_batch_bytes: u64 = unsafe { raft_append_entries_batch_max_bytes() };
        let decided: HeartbeatTick = heartbeat_tick(
            &mut server.core, is_leader, snapshot_configured, batching,
            max_batch_entries, max_batch_bytes, &mut out);
        server.run_locked_actions(&out);
        // The InstallSnapshot kernel keeps its place under the guard. Its
        // completion callback takes the SAME mutex, which is why PHASE 1
        // reaches followers by ordinal and never holds a reference across it.
        let mut i: usize = 0;
        while i < decided.snapshots_.len() {
            let snapshot: &SnapshotSend = &decided.snapshots_[i];
            let sent: bool = unsafe {
                raft_phase1_load_and_send_snapshot(
                    server.handle(),
                    &server.snapshot_manager_
                        as *const rusty::RaftSnapshotManagerPtr,
                    &server.async_callback_lifetime_
                        as *const rusty::RaftAsyncCallbackLifetimePtr,
                    server.site_id_, server.partition_id_, snapshot.term_,
                    snapshot.site_id_, snapshot.ord_)
            };
            if !sent {
                rusty::raft_log_warn_2(
                    "[HEARTBEAT-SNAPSHOT] Site {}: Failed to load snapshot for follower {}, skipping",
                    server.site_id_, snapshot.site_id_);
            }
            i += 1;
        }
        decided
    };
    server.run_unlocked_actions(&out);
    if tick.slots_reset_ {
        server.append_responses_.reset(tick.slot_count_);
    }
    if tick.declined_ {
        return tick;
    }

    // [fix, F7] Each payload is built from the handles the core copied out
    // under the guard -- a batch's entries stamped with their log terms and
    // finalized into one TpcBatchCommand -- and sent, in follower order.
    let partition_id: u32 = server.partition_id_;
    let mut i: usize = 0;
    while i < tick.sends_.len() {
        let send: &AppendSend = &tick.sends_[i];
        // An empty Command (has_value() == false) signals a heartbeat.
        let mut cmd: rusty::RaftCommand = Default::default();
        if send.payload_ == AppendPayload::RAW_ENTRY {
            cmd = raft_command_handle_clone(&send.cmds_[0]);
        } else if send.payload_ == AppendPayload::BATCH {
            // A fresh buffer per follower, as the C++ local was.
            server.batch_buffer_.clear();
            let mut k: usize = 0;
            while k < send.cmds_.len() {
                let stamped: rusty::RaftTpcCommitPtr = unsafe {
                    rusty::raft_stamped_commit(
                        &send.cmds_[k] as *const rusty::RaftCommand,
                        send.terms_[k])
                };
                server.batch_buffer_.push(stamped);
                k += 1;
            }
            unsafe {
                raft_batch_finalize(server.batch_buffer_.as_mut_ptr(),
                                    server.batch_buffer_.len(),
                                    &mut cmd as *mut rusty::RaftCommand);
            }
        }
        let mut sent_response: rusty::RaftResponsePtr = Default::default();
        unsafe { raft_trace_through(3, send.sent_end_index_, 0) };  // [M0] trace kit
        unsafe {
            raft_phase1_send_append(
                server.handle(), server.site_id_, send.site_id_, partition_id,
                true, send.term_, send.prev_log_index_, send.prev_log_term_,
                send.commit_index_,  // [fix, F3]
                &cmd as *const rusty::RaftCommand, send.entry_term_,
                &mut sent_response as *mut rusty::RaftResponsePtr);
        }
        unsafe { raft_trace_through(4, send.sent_end_index_, 0) };  // [M0] trace kit
        server.append_responses_.place(send.ord_, sent_response,
                                       send.sent_round_);
        i += 1;
    }
    tick
}

// ==========================================================================
// PHASE 2: poll responses through one SHORT round deadline and process them.
//
// Formerly RaftServer::HeartbeatPhase2. Never call wait_timeout on an
// individual response: that permanently marks its event TIMEOUT and loses a
// legitimate late persistence reply. Polling also gives every parallel RPC
// the same bounded round budget.
//
// [move, M5] This is the shell's loop now: it polls its response handles,
// hands each completed reply to heartbeat_on_reply under mtx_, and keeps the
// deadline, the 1 ms step and the early-quorum exit. `round_id` and
// `has_authority` are what the round's tick reported.
// ==========================================================================
#[allow(clippy::manual_clamp)]
pub fn heartbeat_collect_body(server: &mut RaftServerBase, round_id: u64,
                              has_authority: bool) {
    const RESPONSE_POLL_STEP_US: u64 = 1000;
    // max(1, min(100000, heartbeat_interval_us_)). Spelled out rather than
    // with clamp: this lowers to C++, where uint64_t has no such member.
    let response_round_timeout_us: u64 =
        if server.heartbeat_interval_us_ > 100000 {
            100000
        } else if server.heartbeat_interval_us_ < 1 {
            1
        } else {
            server.heartbeat_interval_us_
        };
    let response_deadline_us: u64 =
        unsafe { raft_monotonic_now_us() } + response_round_timeout_us;
    let mut stop_response_processing: bool = false;
    let mut retry_released_follower: bool = false;
    // The ledger changes only in the core calls below, so the round's
    // authority is whatever the last of them reported.
    let mut current_round_has_authority: bool = has_authority;

    while !stop_response_processing {
        let mut waiting_for_current_round: bool = false;
        let mut pending_ord: usize = 0;
        while pending_ord < server.append_responses_.len() {
            if !server.IsLeader() {
                stop_response_processing = true;
                break;
            }
            if !server.append_responses_.occupied(pending_ord) {
                pending_ord += 1;
                continue;
            }
            let sent_round: u64 = server.append_responses_.sent_round(pending_ord);
            let resp: AppendRespView = unsafe {
                raft_append_response_read(
                    server.append_responses_.response(pending_ord)
                        as *const rusty::RaftResponsePtr)
            };
            if !resp.completed_ {
                if sent_round == round_id {
                    waiting_for_current_round = true;
                }
                pending_ord += 1;
                continue;
            }

            let mut out: CoreOutput = core_output();
            let reply: ReplyResult = {
                let _lock = RaftLockGuard::new(&mut server.mtx_);
                let is_leader: bool = server.IsLeaderLocked();
                let stopped: bool = server.stopped_now();
                let failover: bool = server.failover_;
                let decided: ReplyResult = heartbeat_on_reply(
                    &mut server.core, pending_ord, resp.status_, resp.term_,
                    resp.last_log_index_, is_leader, stopped,
                    failover, &mut out);
                server.run_locked_actions(&out);
                decided
            };
            server.run_unlocked_actions(&out);  // [fix, F6]
            server.append_responses_.release(pending_ord);
            retry_released_follower =
                retry_released_follower || reply.completed_previous_round_;
            current_round_has_authority = reply.has_authority_;
            if reply.stepped_down_ {
                stop_response_processing = true;
                break;
            }
            pending_ord += 1;
        }

        if stop_response_processing || !waiting_for_current_round
            || current_round_has_authority
        {
            break;
        }
        let now_us: u64 = unsafe { raft_monotonic_now_us() };
        if now_us >= response_deadline_us {
            break;
        }
        let remaining_us: u64 = response_deadline_us - now_us;
        let step_us: u64 = if remaining_us < RESPONSE_POLL_STEP_US {
            remaining_us
        } else {
            RESPONSE_POLL_STEP_US
        };
        unsafe {
            raft_fiber_sleep_us(step_us);
        }
    }

    if stop_response_processing {
        {
            let _lock = RaftLockGuard::new(&mut server.mtx_);
            heartbeat_abandon_round(&mut server.core);
        }
        let slots: usize = server.append_responses_.len();
        server.append_responses_.reset(slots);
    } else if retry_released_follower {
        // A completion from an older round opened a per-follower slot after
        // PHASE 1. Prompt another round instead of waiting a full interval.
        server.RequestReplication();
    }
}

// PHASE 3 around its core call.
pub fn heartbeat_round_end_body(server: &mut RaftServerBase) {
    if !server.IsLeader() {
        return;
    }
    let mut out: CoreOutput = core_output();
    let commit_advanced_after_send: bool = {
        let _lock = RaftLockGuard::new(&mut server.mtx_);
        let is_leader: bool = server.IsLeaderLocked();
        let advanced: bool = heartbeat_round_end(&mut server.core, is_leader,
                                                 &mut out);
        server.run_locked_actions(&out);
        advanced
    };
    server.run_unlocked_actions(&out);

    // The AppendEntries messages for this round carried the OLD commit
    // index. Latch exactly one prompt follow-up round so followers learn the
    // phase-3 commit without waiting out the periodic heartbeat.
    if commit_advanced_after_send {
        server.RequestReplication();
    }
}

// ==========================================================================
// THE ROUND DRIVER
//
// Sequences the round's three core calls and the waits between them. It
// lives here, beside them, rather than next to RaftServerBase: a DSL block can
// only call what precedes it, and the phase bodies are in this file.
// ==========================================================================

// The driver holds only the server. The round state it used to own
// ([move, M1]) is the core's: RaftCore::pending_rpcs_, authority_rounds_,
// pending_leader_term_ and round_, with the response handles beside them in
// RaftServerBase::append_responses_ ([move, M5]); all reset when a run of the
// loop begins and again when it ends.
pub struct HeartbeatDriver {
    server_: *mut RaftServerBase,
}

#[allow(clippy::not_unsafe_ptr_arg_deref)]
impl HeartbeatDriver {
    pub fn new(server: *mut RaftServerBase) -> HeartbeatDriver {
        HeartbeatDriver { server_: server }
    }

    // decide -> emit -> collect -> decide: the tick (PHASE 0 and 1), the
    // collection loop (PHASE 2) and the round end (PHASE 3).
    pub fn run(&mut self) {
        let server: &mut RaftServerBase = unsafe { &mut *self.server_ };
        // [move, M1] a fresh round state per run, as the driver's own was
        server.core.reset_round_state();
        server.append_responses_ = AppendResponses::new();  // [move, M5]
        server.HeartbeatPrologue();
        while server.HeartbeatLooping() {
            // The wake gate returns false on shutdown rather than on timeout.
            if !server.HeartbeatWait() {
                break;
            }
            // [move, M5] The tick declines the round when leadership is not
            // held, and the driver waits for the next one, as the C++
            // `continue` did (bugs-found B12).
            let tick: HeartbeatTick = heartbeat_tick_body(server);
            if tick.declined_ {
                continue;
            }
            heartbeat_collect_body(server, tick.round_id_, tick.has_authority_);
            heartbeat_round_end_body(server);
        }
        // [move, M1] The in-flight handles are released when the loop ends, as
        // they were when the driver's own round state went out of scope.
        // Before the epilogue, not after: once it reports the loop stopped,
        // shutdown may free the server.
        server.core.reset_round_state();
        server.append_responses_ = AppendResponses::new();  // [move, M5]
        server.HeartbeatEpilogue();
    }
}

// The whole loop, so the C++ side is one fiber spawn rather than a method.
#[allow(clippy::not_unsafe_ptr_arg_deref)]
pub fn heartbeat_loop_body(server: *mut RaftServerBase) {
    let mut driver = HeartbeatDriver::new(server);
    driver.run();
}

// ==========================================================================
// THE C ABI OVER RaftServerBase
//
// One extern "C" function per behaviour the hand-written C++ reaches -- the
// RaftServer shim's interface forwarders and the kernels' callbacks -- each
// forwarding to the method. Generated by scripts/raft_gen_exports.py; the
// matching prototypes are server_exports.h. These are the crate's exported
// symbols, and nothing else of the struct is visible to C++.
// ==========================================================================
use crate::scheduler_h::RaftStartResult;
use crate::scheduler_h::TxLogServer;
use crate::server_h::GateWakeJob;
use crate::server_h::ElectionTimerLoop;
// --- GENERATED EXPORTS BEGIN (scripts/raft_gen_exports.py; do not edit by hand) ---
// --- Lifetime. Rust allocates and frees: the struct is a Box the shim
// holds as a raw pointer between these two calls.
/// # Safety
/// The returned pointer is owned by the caller until raft_server_delete.
#[no_mangle]
pub unsafe extern "C" fn raft_server_new() -> *mut RaftServerBase {
    let s: *mut RaftServerBase = Box::into_raw(Box::new(RaftServerBase::new()));
    (*s).ConstructRuntime();
    s
}

/// # Safety
/// `s` came from raft_server_new and is not used afterwards.
#[no_mangle]
pub unsafe extern "C" fn raft_server_delete(s: *mut RaftServerBase) {
    (*s).Shutdown();
    // Drops this server's row from the C++ commo table -- see set_commo, and
    // commo_of in server.cc. Here rather than in the shim's destructor so the
    // key is released in the same function that frees what it keys on, and
    // while the pointer is still live: Shutdown reaches no kernel that
    // resolves the communicator.
    raft_unbind_commo(s as *mut RaftServerHandle);
    drop(rusty::Box::from_raw(s));
}

// --- The two fiber loops, entered from the spawn kernels.
/// # Safety
/// `s` is a live `RaftServerBase`; runs on the calling fiber until shutdown.
#[no_mangle]
pub unsafe extern "C" fn raft_server_heartbeat_loop(s: *mut RaftServerBase) {
    heartbeat_loop_body(s)
}

/// # Safety
/// `s` is a live `RaftServerBase`; runs on the calling fiber until shutdown.
#[no_mangle]
pub unsafe extern "C" fn raft_server_run_election_timer_loop(s: *mut RaftServerBase,
                                                              wait_int_us: u64) {
    let timer: ElectionTimerLoop = ElectionTimerLoop::new(s, wait_int_us);
    timer.run()
}

// --- The wake job, entered from the reactor's OneTimeJob (raft_queue_wake_job).
/// # Safety
/// `token` is the Box<GateWakeJob> RaftServerBase::queue_wake_job made raw,
/// handed back exactly once.
#[no_mangle]
pub unsafe extern "C" fn raft_wake_job_run(token: *mut core::ffi::c_void) {
    let job: rusty::Box<GateWakeJob> = rusty::Box::from_raw(token as *mut GateWakeJob);
    job.run();
}

// --- The replication interface: TxLogServer and RaftSpecific.

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_set_site_identity(s: *mut RaftServerBase,
                                                       loc_id: u32,
                                                       site_id: u16,
                                                       partition_id: u32) {
    (*s).set_site_identity(loc_id, site_id, partition_id)
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_set_commo(s: *mut RaftServerBase,
                                               commo: *mut rusty::Communicator) {
    (*s).set_commo(commo)
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_reg_learner_action(s: *mut RaftServerBase,
                                                        learner_action: *const rusty::LearnerAction) {
    (*s).reg_learner_action(&*learner_action)
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_ensure_setup(s: *mut RaftServerBase) {
    (*s).EnsureSetup()
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_wait_for_startup(s: *mut RaftServerBase) -> bool {
    (*s).WaitForStartup()
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_prepare_for_shutdown(s: *mut RaftServerBase) {
    (*s).PrepareForShutdown()
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_is_leader(s: *mut RaftServerBase) -> bool {
    (*s).IsLeader()
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_get_leader_hint(s: *mut RaftServerBase) -> u16 {
    (*s).GetLeaderHint()
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_set_preferred_leader(s: *mut RaftServerBase,
                                                          site_id: u16) {
    (*s).SetPreferredLeader(site_id)
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_register_leader_change_callback(s: *mut RaftServerBase,
                                                                     cb: *const rusty::RaftLeaderChangeCb) {
    (*s).RegisterLeaderChangeCallback(&*cb)
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_is_rpc_ready(s: *const RaftServerBase) -> bool {
    (*s).IsRpcReady()
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_site_id(s: *const RaftServerBase) -> u16 {
    (*s).SiteId()
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_partition_id(s: *const RaftServerBase) -> u32 {
    (*s).PartitionId()
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_commit_index(s: *const RaftServerBase) -> u64 {
    (*s).CommitIndex()
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_start(s: *mut RaftServerBase,
                                           cmd: *const rusty::RaftCommand,
                                           index: *mut u64,
                                           term: *mut u64) -> RaftStartResult {
    (*s).Start(&*cmd, index, term)
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_serve_vote(s: *mut RaftServerBase,
                                                lst_log_idx: u64,
                                                lst_log_term: i64,
                                                can_id: u16,
                                                can_term: i64,
                                                reply_term: *mut i64,
                                                vote_granted: *mut i8) {
    (*s).ServeVote(lst_log_idx, lst_log_term, can_id, can_term, reply_term, vote_granted)
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_serve_append_entries(s: *mut RaftServerBase,
                                                          leader_current_term: u64,
                                                          leader_site_id: u16,
                                                          leader_prev_log_index: u64,
                                                          leader_prev_log_term: u64,
                                                          leader_commit_index: u64,
                                                          cmd: *const rusty::RaftCommand,
                                                          leader_next_log_term: u64,
                                                          follower_append_ok: *mut u64,
                                                          follower_current_term: *mut u64,
                                                          follower_last_log_index: *mut u64) {
    (*s).ServeAppendEntries(leader_current_term, leader_site_id, leader_prev_log_index, leader_prev_log_term, leader_commit_index, &*cmd, leader_next_log_term, follower_append_ok, follower_current_term, follower_last_log_index)
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_serve_install_snapshot(s: *mut RaftServerBase,
                                                            term: u64,
                                                            leader_id: u64,
                                                            last_included_index: u64,
                                                            last_included_term: u64,
                                                            data: *const rusty::RaftByteString,
                                                            term_out: *mut u64) {
    (*s).ServeInstallSnapshot(term, leader_id, last_included_index, last_included_term, &*data, term_out)
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_set_state_machine_snapshot_callbacks(s: *mut RaftServerBase,
                                                                          create_cb: *const rusty::RaftCreateSnapshotCb,
                                                                          prepare_cb: *const rusty::RaftPrepareSnapshotCb) -> u64 {
    (*s).SetStateMachineSnapshotCallbacks(&*create_cb, &*prepare_cb)
}

// --- What the kernels in server.cc call back into.

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_apply_thread_loop(s: *mut RaftServerBase) {
    (*s).ApplyThreadLoop()
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_bind_replication_wake_owner(s: *mut RaftServerBase,
                                                                 owner: *const rusty::RaftPollThreadPtr) {
    let owner_copy: rusty::RaftPollThreadPtr = (*owner).clone();
    (*s).BindReplicationWakeOwner(owner_copy)
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_fail_stop(s: *mut RaftServerBase) {
    (*s).FailStop()
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_initialize_snapshot_manager_locked(s: *mut RaftServerBase) -> bool {
    (*s).InitializeSnapshotManagerLocked()
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_install_snapshot_reply_accepted(s: *mut RaftServerBase,
                                                                     site_id: u16,
                                                                     ord: usize,
                                                                     snap_last_idx: u64,
                                                                     send_term: u64,
                                                                     follower_term: u64) {
    (*s).InstallSnapshotReplyAccepted(site_id, ord, snap_last_idx, send_term, follower_term)
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_on_install_snapshot_locked(s: *mut RaftServerBase,
                                                                term: u64,
                                                                leader_id: u64,
                                                                last_included_index: u64,
                                                                last_included_term: u64,
                                                                data: *const rusty::RaftByteString,
                                                                term_out: *mut u64) {
    (*s).OnInstallSnapshotLocked(term, leader_id, last_included_index, last_included_term, data, term_out)
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_setup_internal(s: *mut RaftServerBase) -> bool {
    (*s).SetupInternal()
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_start_election_timer(s: *mut RaftServerBase) {
    (*s).StartElectionTimer()
}
// --- GENERATED EXPORTS END ---
