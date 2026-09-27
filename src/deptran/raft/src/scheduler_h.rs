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
#[allow(non_snake_case)]
#[allow(clippy::too_many_arguments)]
pub trait RaftSpecific: TxLogServer {
    // Lifecycle, as the worker drives it.
    fn EnsureSetup(&mut self);
    fn WaitForStartup(&mut self) -> bool;
    fn PrepareForShutdown(&mut self);
    // Leadership.
    fn IsLeader(&mut self) -> bool;
    fn GetLeaderHint(&mut self) -> u16;
    fn SetPreferredLeader(&mut self, site_id: u16);
    // By reference, for the reason given on reg_learner_action.
    fn RegisterLeaderChangeCallback(&mut self, cb: &rusty::RaftLeaderChangeCb);
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
    fn Start(&mut self, cmd: &rusty::RaftCommand, index: *mut u64,
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
    fn ServeVote(&mut self, lst_log_idx: u64, lst_log_term: i64,
                 can_id: u16, can_term: i64, reply_term: *mut i64,
                 vote_granted: *mut i8);
    fn ServeAppendEntries(&mut self, leader_current_term: u64,
                          leader_site_id: u16, leader_prev_log_index: u64,
                          leader_prev_log_term: u64, leader_commit_index: u64,
                          cmd: &rusty::RaftCommand, leader_next_log_term: u64,
                          follower_append_ok: *mut u64,
                          follower_current_term: *mut u64,
                          follower_last_log_index: *mut u64);
    fn ServeInstallSnapshot(&mut self, term: u64, leader_id: u64,
                            last_included_index: u64, last_included_term: u64,
                            data: &rusty::RaftByteString, term_out: *mut u64);
    // The embedder's state-machine snapshot callbacks. By reference, as
    // RegisterLeaderChangeCallback: std::function is not bitwise-relocatable.
    // Returns the owner token ClearStateMachineSnapshotCallbacks takes.
    fn SetStateMachineSnapshotCallbacks(&mut self,
                                        create_cb: &rusty::RaftCreateSnapshotCb,
                                        prepare_cb: &rusty::RaftPrepareSnapshotCb) -> u64;
}
