// Raft's RPC service, on the Rust srpc lane.
//
// WHAT THIS REPLACES. src/deptran/raft/service.cc -- `RaftServiceImpl`, a C++
// class overriding the `RaftService` that rpcgen emits into rcc_rpc.h, which
// the C++ lane of srpc dispatches to through a vtable. This is the same four
// handlers reached from the Rust lane instead, so the dispatch seam stops
// being the place the two languages meet.
//
// WHY IT COMPILES AT ALL NOW. `trait Service: Send + Sync`
// (src/srpc/rpc/server.rs:183). Until stage 3a removed `commo_`,
// `RaftServerBase` was !Send and could not be behind one. It is Send + Sync
// now and the compiler checks it -- tests/server_is_send.rs.
//
// WHAT IS GENERATED AND WHAT IS NOT. Everything mechanical -- the wire
// structs, the id constants, `register`, `dispatch` -- comes from
// scripts/rpcgen_rust.py via src/deptran/raft/src/rpc.rs. What is here is the
// four handler bodies, which is the same seam C++ uses between `RaftService`
// and `RaftServiceImpl`.

use crate::snapshot::{clear_handoff, park_handoff};
use crate::rpc::{
    self, AppendEntriesRequestRef, AppendEntriesResponse, EmptyAppendEntriesRequest,
    EmptyAppendEntriesResponse, InstallSnapshotRequest, InstallSnapshotResponse, RaftHandler,
    VoteRequest, VoteResponse,
};
use raft::scheduler_h::RaftSpecific;
use raft::server_h::RaftServerBase;
use srpc::server::{Request, Server, Service, WeakServerConnection};

/// srpc's reply code for a request that fails to decode -- the value
/// `reject_malformed_request` sends (src/srpc/rpc/server.rs), which is private
/// there. EINVAL, so it matches the C++ lane's generated decoder byte for byte.
const SERVER_ERR_INVALID_ARGUMENT: i32 = 22;

unsafe extern "C" {
    // Rebuilds a janus::Command from the bytes the wire carried. The C++
    // service never needed this because rpcgen's generated Deserialize_ did
    // it inside the request struct; on this lane the request holds the bytes
    // (stage 2c) and the envelope is C++'s to interpret, so the reconstruction
    // is a kernel. Writes into a default-constructed slot the caller owns.
    // False on a malformed frame (server.cc), which the handler rejects the
    // way the C++ lane's generated decoder does: SERVER_ERR_INVALID_ARGUMENT.
    fn raft_command_from_bytes(bytes: *const u8, len: usize,
                               out: *mut rusty::RaftCommand) -> bool;
    // Same shape, for the snapshot payload: ServeInstallSnapshot takes a
    // rusty::RaftByteString, which is a std::string carried as 24 opaque
    // bytes with its own destructor kernel. Rust cannot build one.
    // HOST InstallSnapshot counter (server.cc, phase N0).
    fn raft_install_rpc_note_received();
    fn raft_byte_string_from_bytes(bytes: *const u8, len: usize,
                                   out: *mut rusty::RaftByteString);
}

/// The server, as the service reaches it.
///
/// A raw pointer is unconditionally `!Send` no matter what it points at, so
/// the pointer needs a wrapper even though `RaftServerBase` is `Send + Sync`.
/// That is the whole reason this type exists.
struct ServerHandle(*mut RaftServerBase);

// SAFETY: the pointee is `Send + Sync`, and that is CHECKED rather than
// asserted -- tests/server_is_send.rs fails to compile if it stops being
// true. The lifetime is the worker's: it constructs the server before
// registering this service and destroys it after draining the RPC server, so
// no handler can outlive the pointee. That is the same contract the C++
// `RaftSpecific* svr_` in service.h has always had, written down rather than
// implied.
unsafe impl Send for ServerHandle {}
unsafe impl Sync for ServerHandle {}

pub struct RaftRpcService {
    server: ServerHandle,
}

impl RaftRpcService {
    /// # Safety
    /// `server` must outlive this service, which the worker guarantees by
    /// draining the RPC server before deleting the Raft server.
    pub unsafe fn new(server: *mut RaftServerBase) -> RaftRpcService {
        RaftRpcService { server: ServerHandle(server) }
    }

    // `Service::__dispatch__` and `RaftHandler` both take `&self`, while the
    // handlers take `&mut self`, exactly as the C++ `const` service calls
    // non-const methods through its stored pointer. What actually serialises
    // concurrent handlers is mtx_ inside the server, on both lanes.
    #[allow(clippy::mut_from_ref)]
    fn server(&self) -> &mut RaftServerBase {
        // SAFETY: see ServerHandle.
        unsafe { &mut *self.server.0 }
    }
}

impl Service for RaftRpcService {
    fn __reg_to__(&mut self, server: &mut Server, svc_index: usize) -> i32 {
        rpc::register(server, svc_index)
    }

    fn __dispatch__(&self, rpc_id: i32, req: Box<Request>, sconn: WeakServerConnection) {
        rpc::dispatch(self, rpc_id, &req, &sconn);
    }
}

// The unavailable path is NOT here. ServeVote and its siblings apply the
// admission gate and write the unavailable reply themselves (stage 3c), so
// these bodies are the same one call the C++ service makes.
impl RaftHandler for RaftRpcService {
    fn vote(&self, req: &VoteRequest) -> Result<VoteResponse, i32> {
        let mut resp = VoteResponse::default();
        self.server().ServeVote(req.lst_log_idx, req.lst_log_term, req.site_id,
                                req.cur_term, &raw mut resp.max_ballot,
                                &raw mut resp.vote_granted);
        Ok(resp)
    }

    // The payload arrives as a slice of the request frame and goes straight
    // to C++'s decoder: no copy on the Rust side.
    fn append_entries(&self, req: &AppendEntriesRequestRef<'_>)
        -> Result<AppendEntriesResponse, i32> {
        let mut resp = AppendEntriesResponse::default();
        let mut cmd: rusty::RaftCommand = Default::default();
        let decoded = unsafe {
            raft_command_from_bytes(req.cmd.as_ptr(), req.cmd.len(), &raw mut cmd)
        };
        if !decoded {
            return Err(SERVER_ERR_INVALID_ARGUMENT);
        }
        self.server().ServeAppendEntries(
            req.leader_current_term, req.leader_site_id, req.leader_prev_log_index,
            req.leader_prev_log_term, req.leader_commit_index, &cmd,
            req.leader_next_log_term, &raw mut resp.follower_append_ok,
            &raw mut resp.follower_current_term, &raw mut resp.follower_last_log_index);
        Ok(resp)
    }

    fn empty_append_entries(&self, req: &EmptyAppendEntriesRequest)
        -> Result<EmptyAppendEntriesResponse, i32> {
        let mut resp = EmptyAppendEntriesResponse::default();
        // An empty Command and leader_next_log_term 0, exactly as the C++
        // service passes janus::Command{} on this path.
        let cmd: rusty::RaftCommand = Default::default();
        self.server().ServeAppendEntries(
            req.leader_current_term, req.leader_site_id, req.leader_prev_log_index,
            req.leader_prev_log_term, req.leader_commit_index, &cmd, 0,
            &raw mut resp.follower_append_ok, &raw mut resp.follower_current_term,
            &raw mut resp.follower_last_log_index);
        Ok(resp)
    }

    fn install_snapshot(&self, req: InstallSnapshotRequest)
        -> Result<InstallSnapshotResponse, i32> {
        let mut resp = InstallSnapshotResponse::default();
        unsafe { raft_install_rpc_note_received() };
        // Copy 2 of 2: prepare_cb takes `const std::string&`, so the image
        // must exist as a std::string (plan N5, Risks 5).
        let mut data: rusty::RaftByteString = Default::default();
        unsafe {
            raft_byte_string_from_bytes(req.data.0.as_ptr(), req.data.0.len(),
                                        &raw mut data);
        }
        // The decoded buffer itself goes to the store, uncopied: parked for
        // exactly this synchronous call, taken by raft_snapshot_store_save when
        // its (index, term, len) tag matches, and cleared afterwards whatever
        // happened.
        let (index, term) = (req.last_included_index, req.last_included_term);
        park_handoff(index, term, req.data.0);
        self.server().ServeInstallSnapshot(req.term, req.leader_id, index, term,
                                           &data, &raw mut resp.term_out);
        clear_handoff();
        Ok(resp)
    }
}
