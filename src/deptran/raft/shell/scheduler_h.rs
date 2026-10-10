pub trait TxLogServer {
    fn set_site_identity(&mut self, loc_id: u32, site_id: u16, partition_id: u32);
    fn set_commo(&mut self, commo: *mut rusty::Communicator);
    // By reference, never by value: a std::function is not bitwise-relocatable
    // (libc++ keeps a small callable inside the object and points at it), so
    // the implementation copies it in place, into its final slot.
    fn reg_learner_action(&mut self, learner_action: &rusty::LearnerAction);
}

// Submission admission result for the RaftWorker interface.  Memory-only Raft
// either rejects a command (not leader) or appends it; there is no durable
// append whose outcome could be unknown.
#[allow(non_camel_case_types)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Eq, PartialEq))]
#[repr(i32)]
pub enum RaftStartResult {
    REJECTED = 0,
    APPENDED = 1,
}

// Method names are the C++ names the workers, the service and the lab tests
// already call; renaming them to snake_case is a mechanical follow-up once
// nothing hand-written calls them.
//
// Every method takes `&self` (bugs-found B19): the worker, the RPC service,
// the fibers and the apply thread call these concurrently on one server, so
// none of them may hold it as `&mut`. What serialises them is the server's
// own mtx_ and atomics. TxLogServer above keeps `&mut self`: PaxosServer
// implements it too, so the Raft server's C ABI reaches set_commo and
// reg_learner_action through `&self` twins (raft_gen_exports.py
// SHARED_TWINS).
#[allow(non_snake_case)]
#[allow(clippy::too_many_arguments)]
pub trait RaftSpecific: TxLogServer {
    // Lifecycle, as the worker drives it.
    fn EnsureSetup(&self);
    fn WaitForStartup(&self) -> bool;
    fn PrepareForShutdown(&self);
    // Leadership.
    fn IsLeader(&self) -> bool;
    fn GetLeaderHint(&self) -> u16;
    fn SetPreferredLeader(&self, site_id: u16);
    // By reference, for the reason given on reg_learner_action.
    fn RegisterLeaderChangeCallback(&self, cb: &rusty::RaftLeaderChangeCb);
    // Admission. The service no longer asks: ServeVote and friends below
    // apply the gate themselves, inside the one call. What is left here is
    // the worker's readiness probe (raft_worker.cc, ServerStatus::set_ready).
    fn IsRpcReady(&self) -> bool;
    // Identity and progress, read-only: the three facts the main helper used
    // to read as fields through the concrete type. CommitIndex is the
    // pre-existing unlocked cross-thread read behind get_outstanding_logs --
    // a metric polled from the transaction path, tolerated racy since before
    // the conversion; step C gives it an atomic mirror.
    fn SiteId(&self) -> u16;
    fn PartitionId(&self) -> u32;
    fn CommitIndex(&self) -> u64;
    // Replication entry: the worker's "replicate this command".
    fn Start(&self, cmd: &rusty::RaftCommand, index: *mut u64,
             term: *mut u64) -> RaftStartResult;
    // Inbound RPC, as the service hands it over: ONE call per request, gate
    // included. The C++ service used to ask IsDisconnected and IsRpcReady
    // across the ABI before every handler and fill the unavailable reply
    // itself, so each inbound RPC cost three virtual-plus-FFI round trips
    // where it now costs one; the unavailable replies are unchanged, they
    // are just written on this side (RaftServerBase::ServeVote and friends).
    //
    // EmptyAppendEntries has no method of its own: the service calls
    // ServeAppendEntries with an empty Command and leader_next_log_term 0,
    // exactly as it called OnAppendEntries before.
    fn ServeVote(&self, lst_log_idx: u64, lst_log_term: i64,
                 can_id: u16, can_term: i64, reply_term: *mut i64,
                 vote_granted: *mut i8);
    fn ServeAppendEntries(&self, leader_current_term: u64,
                          leader_site_id: u16, leader_prev_log_index: u64,
                          leader_prev_log_term: u64, leader_commit_index: u64,
                          cmd: &rusty::RaftCommand, leader_next_log_term: u64,
                          follower_append_ok: *mut u64,
                          follower_current_term: *mut u64,
                          follower_last_log_index: *mut u64);
    fn ServeInstallSnapshot(&self, term: u64, leader_id: u64,
                            last_included_index: u64, last_included_term: u64,
                            data: &rusty::RaftByteString, term_out: *mut u64);
    // The embedder's state-machine snapshot callbacks. By reference, as
    // RegisterLeaderChangeCallback: std::function is not bitwise-relocatable.
    // Returns the owner token ClearStateMachineSnapshotCallbacks takes.
    fn SetStateMachineSnapshotCallbacks(&self,
                                        create_cb: &rusty::RaftCreateSnapshotCb,
                                        prepare_cb: &rusty::RaftPrepareSnapshotCb) -> u64;
}
