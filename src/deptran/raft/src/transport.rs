// Raft's RPC transport on the Rust srpc lane: one poll thread, one server,
// one set of peer clients (stage 3e).
//
// WHY ONE TYPE. The cut cannot be split. Inbound and outbound share
// svr_poll_thread_worker_ today (raft_worker.cc:344, :350, :379), so moving
// the server without the clients would give Raft a second poll thread --
// adding a thread rather than swapping one, which is the performance change
// the plan's verification rules require a benchmark for. This owns both ends
// so the switch is one change.
//
// WHY IT IS NOT Send. srpc::client::Client holds RefCell and six Cell fields
// (rpc/client.rs:1550-1561): Send but !Sync, so Arc<Client> is neither. That
// is not a defect to work around -- a client belongs to its poll thread, and
// every send already happens there, on the heartbeat fiber. C++ holds this
// as an opaque pointer and Rust's Send checking does not cross that boundary.
// The one caller from elsewhere is Disconnect, which touches only an atomic.
//
// WHAT IT MIRRORS. The peer table has the shape of PeerRegistry
// (src/deptran/communicator.h): partitions own site ids, the table owns
// peers, so a peer lives in exactly one place. The difference is whose
// clients they are -- PeerRegistry carries C++-lane shared_ptrs as opaque
// bytes, these are Rust-lane Arc<Client>.

#![allow(dead_code)]

use std::collections::HashMap;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};

use crate::quorum_hpp::{raft_quorum_count_reached, raft_quorum_majority_count};
use crate::rpc::{
    AppendEntriesRequest, AppendEntriesResponse, InstallSnapshotRequest,
    InstallSnapshotResponse, RaftProxy, VoteRequest, VoteResponse,
};
use crate::server_pods_h::RaftVoteOutcome;
use crate::server_h::RaftServerBase;
use crate::service::RaftRpcService;
use srpc::client::Client;
use srpc::reactor::PollThread;
use srpc::server::Server;
use srpc::serializable::{
    make_source_proxy_buffer, BinaryReadArchive, BufferSource, Deserialize,
};

/// How many replicas a campaign must win a majority of.
///
/// Its own function because the rule is subtle and got it wrong once: self
/// may or may not appear in the recorded membership -- C++'s Communicator
/// adds every site in the partition including this one, while a transport
/// built peer-by-peer may not -- and it must be counted exactly once either
/// way. Undercounting elects a partitioned candidate.
pub const fn partition_member_count(recorded: usize,
                                    recorded_includes_self: bool) -> usize {
    if recorded_includes_self {
        recorded
    } else {
        recorded + 1
    }
}

/// One outstanding reply, as the sender reads it back.
///
/// Arc<Mutex<..>> rather than a fiber event on purpose: the reply lands on
/// the poll thread while the heartbeat fiber reads it later, and this is the
/// Mutex-backed, `Send` shape srpc's own `Future` uses (rpc/client.rs). The
/// C++ path it replaces carries a shared_ptr<AppendEntriesResponse> whose
/// `completed` field is polled the same way.
pub struct Pending<T> {
    slot: Arc<Mutex<Option<Result<T, i32>>>>,
}

impl<T> Pending<T> {
    fn new() -> Pending<T> {
        Pending { slot: Arc::new(Mutex::new(None)) }
    }

    /// The reply, if it has landed. None means still outstanding, which is
    /// the same question `AppendEntriesResponse::completed` answers in C++.
    pub fn take(&self) -> Option<Result<T, i32>> {
        self.slot.lock().ok().and_then(|mut guard| guard.take())
    }

    pub fn is_ready(&self) -> bool {
        self.slot.lock().map(|guard| guard.is_some()).unwrap_or(false)
    }
}

/// Decode a reply body. `None` is a malformed frame, which the caller turns
/// into the same "no usable reply" outcome a transport error produces.
///
/// # Safety
/// `ptr`/`len` describe the reply buffer srpc handed the callback, valid for
/// the duration of that call.
unsafe fn decode_reply<T: Deserialize + Default>(ptr: *const u8, len: usize)
    -> Option<T> {
    let mut source = BufferSource::new(ptr, len);
    let mut ar = BinaryReadArchive::new(unsafe {
        make_source_proxy_buffer(&raw mut source)
    });
    let mut out = T::default();
    out.deserialize(&mut ar);
    if ar.failed() {
        return None;
    }
    Some(out)
}

pub struct RaftTransport {
    poll: Arc<PollThread>,
    // None until serve() binds. Both ends sit on `poll`, which is the whole
    // reason this is one type -- see the note at the top of the file.
    server: Option<Server>,
    // Which server serve() bound to, so delete() can unbind it.
    served: Option<*const RaftServerBase>,
    peers: HashMap<u16, Arc<Client>>,
    partitions: HashMap<u32, Vec<u16>>,
    network_enabled: AtomicBool,
}

impl RaftTransport {
    pub fn new() -> RaftTransport {
        RaftTransport {
            poll: PollThread::create(),
            server: None,
            served: None,
            peers: HashMap::new(),
            partitions: HashMap::new(),
            network_enabled: AtomicBool::new(true),
        }
    }

    pub fn poll_thread(&self) -> Arc<PollThread> {
        self.poll.clone()
    }

    /// Bind and start serving Raft's four RPCs for `server`.
    ///
    /// # Safety
    /// `server` outlives this transport -- the worker drains before deleting
    /// it -- and `bind_addr` is a live NUL-terminated string for the call.
    pub unsafe fn serve(&mut self, server: *mut RaftServerBase,
                        bind_addr: *const i8) -> i32 {
        let mut rpc_server = Server::new(Some(self.poll.clone()));
        // SAFETY: the caller's contract on `server`.
        rpc_server.reg_service(Box::new(unsafe { RaftRpcService::new(server) }));
        // SAFETY: the caller's contract on `bind_addr`.
        let ret = unsafe { rpc_server.start(bind_addr) };
        if ret != 0 {
            return ret;
        }
        self.server = Some(rpc_server);
        self.served = Some(server);
        0
    }

    pub fn bound_port(&self) -> i32 {
        self.server.as_ref().map(|s| s.get_bound_port()).unwrap_or(-1)
    }

    /// Close admission and let the handlers already inside finish. The
    /// barrier RaftWorker::ShutDown needs before the server is destroyed.
    pub fn drain(&self, timeout_ms: u64) -> bool {
        match self.server.as_ref() {
            Some(s) => {
                s.set_admission_ready(false);
                s.drain(timeout_ms)
            }
            None => true,
        }
    }

    pub fn set_admission_ready(&self, ready: bool) {
        if let Some(s) = self.server.as_ref() {
            s.set_admission_ready(ready);
        }
    }

    /// Connect to one peer and record it. `addr` is a NUL-terminated
    /// host:port, the same string Communicator::ConnectToAddress takes.
    ///
    /// # Safety
    /// `addr` is a live NUL-terminated string for the duration of the call.
    pub unsafe fn add_peer(&mut self, par_id: u32, site_id: u16,
                           addr: *const i8) -> bool {
        if self.peers.contains_key(&site_id) {
            return false;
        }
        let client = Client::create(self.poll.clone());
        if client.connect(addr, false) != 0 {
            return false;
        }
        self.peers.insert(site_id, client);
        self.partitions.entry(par_id).or_default().push(site_id);
        true
    }

    pub fn peer(&self, site_id: u16) -> Option<&Arc<Client>> {
        if !self.network_enabled() {
            return None;
        }
        self.peers.get(&site_id)
    }

    /// Every peer in a partition except the caller, which is who a broadcast
    /// reaches. Mirrors Communicator::PeersForPartition plus the self-skip
    /// RaftCommo does by comparing site ids.
    pub fn peers_in_partition(&self, par_id: u32, except: u16)
        -> Vec<(u16, Arc<Client>)> {
        if !self.network_enabled() {
            return Vec::new();
        }
        let Some(sites) = self.partitions.get(&par_id) else {
            return Vec::new();
        };
        sites
            .iter()
            .filter(|site| **site != except)
            .filter_map(|site| self.peers.get(site).map(|c| (*site, c.clone())))
            .collect()
    }

    // Release/Acquire, as the std::atomic_bool in Communicator uses.
    pub fn set_network_enabled(&self, enabled: bool) {
        self.network_enabled.store(enabled, Ordering::Release);
    }

    pub fn network_enabled(&self) -> bool {
        self.network_enabled.load(Ordering::Acquire)
    }

    pub fn peer_count(&self) -> usize {
        self.peers.len()
    }

    // --- the send paths
    //
    // Each returns None when there is nothing to send to -- an unknown site,
    // or the network flag down -- which is the null the C++ PeerForSite
    // returns and which callers already handle as a lost RPC.

    pub fn send_append_entries(&self, site_id: u16, req: &AppendEntriesRequest)
        -> Option<Pending<AppendEntriesResponse>> {
        let client = self.peer(site_id)?;
        let pending: Pending<AppendEntriesResponse> = Pending::new();
        let sink = pending.slot.clone();
        let proxy = RaftProxy { client };
        proxy
            .append_entries_async(
                req,
                Some(Box::new(move |code, ptr, len| {
                    let value = if code != 0 {
                        Err(code)
                    } else {
                        // SAFETY: srpc owns the buffer for this call.
                        unsafe { decode_reply(ptr, len) }.ok_or(-1)
                    };
                    if let Ok(mut guard) = sink.lock() {
                        *guard = Some(value);
                    }
                })),
            )
            .ok()?;
        Some(pending)
    }

    pub fn send_install_snapshot(&self, site_id: u16,
                                 req: &InstallSnapshotRequest)
        -> Option<Pending<InstallSnapshotResponse>> {
        let client = self.peer(site_id)?;
        let pending: Pending<InstallSnapshotResponse> = Pending::new();
        let sink = pending.slot.clone();
        let proxy = RaftProxy { client };
        proxy
            .install_snapshot_async(
                req,
                Some(Box::new(move |code, ptr, len| {
                    let value = if code != 0 {
                        Err(code)
                    } else {
                        // SAFETY: srpc owns the buffer for this call.
                        unsafe { decode_reply(ptr, len) }.ok_or(-1)
                    };
                    if let Ok(mut guard) = sink.lock() {
                        *guard = Some(value);
                    }
                })),
            )
            .ok()?;
        Some(pending)
    }

    /// The campaign broadcast, and the tally.
    ///
    /// THIS IS THE OPERATION 3a's TABLE SAID COULD NOT MOVE. It said
    /// BroadcastVote "returns RaftVoteQuorumPtr, a 16-byte C++ carrier" --
    /// true, and irrelevant: that carrier never escapes three kernel calls,
    /// and everything Raft reads out of it is RaftVoteOutcome, six scalars.
    /// So there is nothing to hand over. The quorum is counted here instead,
    /// by the same rule the C++ event applies -- raft_quorum_majority_count
    /// over the partition, which is quorum_hpp, shared by both lanes.
    ///
    /// Self counts as a yes before any RPC goes out, exactly as the C++
    /// quorum seeds itself: a candidate votes for itself.
    pub fn broadcast_vote(&self, par_id: u32, self_site_id: u16,
                          req: &VoteRequest) -> Option<VoteTally> {
        // THE QUORUM COMES FROM THE CONFIGURED PARTITION, NOT FROM WHO CAN BE
        // REACHED. Deriving it from the reachable peers is a split-brain bug,
        // and a live one: with the network flag down, `peers` is empty, a
        // majority of one is one, and the candidate's own vote elects it
        // while it is partitioned away from a healthy cluster. Caught by
        // tests/transport_roundtrip.rs.
        //
        // Self may or may not be in the recorded membership -- C++'s
        // Communicator adds every site in the partition including this one,
        // and RaftCommo skips itself by comparing site ids -- so count it
        // exactly once either way.
        let members = match self.partitions.get(&par_id) {
            Some(sites) => partition_member_count(
                sites.len(), sites.contains(&self_site_id)),
            None => 1,
        };
        let quorum = raft_quorum_majority_count(members);
        let peers = self.peers_in_partition(par_id, self_site_id);
        let tally = VoteTally::new(quorum, req.cur_term);
        for (_site, client) in peers {
            let sink = tally.state.clone();
            let proxy = RaftProxy { client: &client };
            let sent = proxy.vote_async(
                req,
                Some(Box::new(move |code, ptr, len| {
                    let reply: Option<VoteResponse> = if code != 0 {
                        None
                    } else {
                        // SAFETY: srpc owns the buffer for this call.
                        unsafe { decode_reply(ptr, len) }
                    };
                    if let Ok(mut guard) = sink.lock() {
                        guard.record(reply);
                    }
                })),
            );
            if sent.is_err() {
                // A send that never left counts as a no-reply, not as a
                // rejection: the C++ path treats a lost RPC the same way.
                if let Ok(mut guard) = tally.state.lock() {
                    guard.record(None);
                }
            }
        }
        Some(tally)
    }
}

/// What a campaign has heard so far.
struct TallyState {
    quorum: usize,
    yes: usize,
    no: usize,
    replied: usize,
    max_term: i64,
}

impl TallyState {
    fn record(&mut self, reply: Option<VoteResponse>) {
        self.replied += 1;
        let Some(reply) = reply else { return };
        if reply.vote_granted != 0 {
            self.yes += 1;
        } else {
            self.no += 1;
        }
        if reply.max_ballot > self.max_term {
            self.max_term = reply.max_ballot;
        }
    }
}

/// The campaign's outcome, read once the wait is over.
pub struct VoteTally {
    state: Arc<Mutex<TallyState>>,
}

impl VoteTally {
    fn new(quorum: usize, own_term: i64) -> VoteTally {
        VoteTally {
            state: Arc::new(Mutex::new(TallyState {
                quorum,
                // The candidate's own vote, counted before any RPC goes out.
                yes: 1,
                no: 0,
                replied: 0,
                max_term: own_term,
            })),
        }
    }

    pub fn decided(&self) -> bool {
        self.state
            .lock()
            .map(|s| raft_quorum_count_reached(s.yes, s.quorum)
                || raft_quorum_count_reached(s.no, s.quorum))
            .unwrap_or(false)
    }

    /// The same six scalars raft_vote_quorum_snapshot reads out of the C++
    /// quorum event, so the caller cannot tell which lane produced them.
    pub fn outcome(&self, timed_out: bool) -> RaftVoteOutcome {
        let Ok(s) = self.state.lock() else {
            return RaftVoteOutcome {
                term_: 0, yes_: false, no_: false,
                n_voted_yes_: 0, n_voted_no_: 0, timeouted_: true,
            };
        };
        RaftVoteOutcome {
            term_: s.max_term,
            yes_: raft_quorum_count_reached(s.yes, s.quorum),
            no_: raft_quorum_count_reached(s.no, s.quorum),
            n_voted_yes_: s.yes as i32,
            n_voted_no_: s.no as i32,
            timeouted_: timed_out,
        }
    }
}

impl Default for RaftTransport {
    fn default() -> Self {
        Self::new()
    }
}

// ===========================================================================
// The transport, resolved from the server that owns it.
//
// WHY A REGISTRY AND NOT A FIELD. The server cannot hold this. RaftTransport
// is !Send -- its clients belong to a poll thread -- while RaftServerBase must
// stay Send + Sync or RaftRpcService loses srpc's trait bound and the Rust
// lane cannot dispatch to Raft at all. A `*mut RaftTransport` field would drag
// the server back to !Send, and wrapping it in a newtype that asserts Send
// would smuggle the claim into the struct where nobody reads it.
//
// So it lives here, the same shape stage 3a gave the C++ side (commo_of in
// server.cc) and for the same reason. The assertion below is the real one and
// it is stated once, in the open.
// ===========================================================================

/// A transport pointer, as the registry holds it.
struct TransportPtr(*mut RaftTransport);

// SAFETY: this asserts that the POINTER may be handed between threads, not
// that the transport may be USED from any thread. Every use goes through
// transport_of on the poll thread that created it -- the heartbeat fiber and
// the election fiber both run there -- except set_network_enabled, which
// touches one atomic. That is the same contract the C++ commo table has had
// since 3a; it is written here because this is where it can be read.
unsafe impl Send for TransportPtr {}
unsafe impl Sync for TransportPtr {}

fn registry() -> &'static std::sync::RwLock<HashMap<usize, TransportPtr>> {
    static TRANSPORTS: std::sync::OnceLock<
        std::sync::RwLock<HashMap<usize, TransportPtr>>,
    > = std::sync::OnceLock::new();
    TRANSPORTS.get_or_init(|| std::sync::RwLock::new(HashMap::new()))
}

/// Bind `transport` to `server`. Called once, before any send.
pub fn bind_transport(server: *const RaftServerBase, transport: *mut RaftTransport) {
    if let Ok(mut table) = registry().write() {
        table.insert(server as usize, TransportPtr(transport));
    }
}

pub fn unbind_transport(server: *const RaftServerBase) {
    if let Ok(mut table) = registry().write() {
        table.remove(&(server as usize));
    }
}

/// The transport bound to `server`, or None if the cutover has not reached
/// this server -- which is how a mixed build stays runnable.
///
/// # Safety
/// The returned reference is valid while the worker holds the transport, and
/// must be used on the poll thread that created it.
pub unsafe fn transport_of(server: *const RaftServerBase)
    -> Option<&'static RaftTransport> {
    let table = registry().read().ok()?;
    let raw = table.get(&(server as usize))?.0;
    // SAFETY: the caller's contract, and bind/unbind bracket the lifetime.
    Some(unsafe { &*raw })
}

// ===========================================================================
// The C ABI, for the worker that owns a transport.
//
// Hand-written rather than emitted by scripts/raft_gen_exports.py: that
// generator's table is methods of RaftServerBase, and these are methods of a
// different type. The header beside them is transport_exports.h.
//
// The transport is deliberately NOT Send -- its clients belong to its poll
// thread -- so C++ holding it as a pointer is the arrangement, not a
// workaround. Every call below must come from the thread that created it,
// which is the worker's, except set_network_enabled, which touches only an
// atomic and is what the lab suite's Disconnect calls from elsewhere.
// ===========================================================================

/// # Safety
/// The returned pointer is owned by the caller until raft_transport_delete.
#[no_mangle]
pub unsafe extern "C" fn raft_transport_new() -> *mut RaftTransport {
    Box::into_raw(Box::new(RaftTransport::new()))
}

/// # Safety
/// `t` came from raft_transport_new; `server` outlives it; `bind_addr` is a
/// live NUL-terminated string for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_transport_serve(t: *mut RaftTransport,
                                              server: *mut RaftServerBase,
                                              bind_addr: *const i8) -> i32 {
    let ret = unsafe { (*t).serve(server, bind_addr) };
    if ret == 0 {
        // Binding here and not earlier is what makes the switch atomic: until
        // the server is actually being served, transport_of returns None and
        // every send site falls back to its C++ kernel.
        bind_transport(server, t);
    }
    ret
}

/// # Safety
/// `t` came from raft_transport_new; `addr` is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_transport_add_peer(t: *mut RaftTransport,
                                                 par_id: u32, site_id: u16,
                                                 addr: *const i8) -> bool {
    unsafe { (*t).add_peer(par_id, site_id, addr) }
}

/// # Safety
/// `t` came from raft_transport_new.
#[no_mangle]
pub unsafe extern "C" fn raft_transport_set_network_enabled(
    t: *mut RaftTransport, enabled: bool) {
    unsafe { (*t).set_network_enabled(enabled) }
}

/// # Safety
/// `t` came from raft_transport_new.
#[no_mangle]
pub unsafe extern "C" fn raft_transport_drain(t: *mut RaftTransport,
                                              timeout_ms: u64) -> bool {
    unsafe { (*t).drain(timeout_ms) }
}

/// # Safety
/// `t` came from raft_transport_new and is not used afterwards.
#[no_mangle]
pub unsafe extern "C" fn raft_transport_delete(t: *mut RaftTransport) {
    let owned = unsafe { Box::from_raw(t) };
    if let Some(server) = owned.served {
        unbind_transport(server);
    }
    drop(owned);
}

#[cfg(test)]
mod tests {
    use super::*;

    // The tally is the part of BroadcastVote that used to be a C++ quorum
    // event, so it is the part worth testing. These do not need a reactor:
    // they exercise the counting rule, which is what the event did.

    fn reply(granted: i8, max_ballot: i64) -> Option<VoteResponse> {
        Some(VoteResponse { max_ballot, vote_granted: granted })
    }

    #[test]
    fn self_is_counted_exactly_once_however_membership_was_recorded() {
        // Three replicas either way. Undercounting here is a split-brain
        // bug: a majority of one is one, so a partitioned candidate would
        // elect itself on its own vote.
        assert_eq!(partition_member_count(2, false), 3, "peers only, self added");
        assert_eq!(partition_member_count(3, true), 3, "self already recorded");
        assert_eq!(raft_quorum_majority_count(partition_member_count(2, false)), 2);
        assert_eq!(raft_quorum_majority_count(partition_member_count(3, true)), 2);
    }

    #[test]
    fn a_candidate_alone_in_its_partition_has_already_won() {
        // One replica: majority of 1 is 1, and the candidate's own vote is
        // counted before any RPC goes out.
        let tally = VoteTally::new(raft_quorum_majority_count(1), 7);
        assert!(tally.decided());
        let outcome = tally.outcome(false);
        assert!(outcome.yes_);
        assert!(!outcome.no_);
        assert_eq!(outcome.n_voted_yes_, 1);
        assert_eq!(outcome.term_, 7);
    }

    #[test]
    fn three_replicas_need_one_granted_reply() {
        let tally = VoteTally::new(raft_quorum_majority_count(3), 4);
        assert!(!tally.decided(), "one self-vote is not a majority of three");
        tally.state.lock().unwrap().record(reply(1, 4));
        assert!(tally.decided());
        let outcome = tally.outcome(false);
        assert!(outcome.yes_ && !outcome.no_);
        assert_eq!(outcome.n_voted_yes_, 2);
    }

    #[test]
    fn rejections_reach_a_quorum_of_their_own() {
        let tally = VoteTally::new(raft_quorum_majority_count(3), 4);
        tally.state.lock().unwrap().record(reply(0, 9));
        assert!(!tally.decided(), "one rejection is not a majority of three");
        tally.state.lock().unwrap().record(reply(0, 4));
        assert!(tally.decided());
        let outcome = tally.outcome(false);
        assert!(outcome.no_ && !outcome.yes_);
        assert_eq!(outcome.n_voted_no_, 2);
    }

    #[test]
    fn the_highest_ballot_seen_is_what_comes_back() {
        // A higher term dominates every outcome, so the tally must report the
        // maximum rather than the last reply -- see RequestVoteImpl.
        let tally = VoteTally::new(raft_quorum_majority_count(5), 3);
        tally.state.lock().unwrap().record(reply(0, 11));
        tally.state.lock().unwrap().record(reply(0, 5));
        assert_eq!(tally.outcome(false).term_, 11);
    }

    #[test]
    fn a_lost_rpc_is_neither_a_yes_nor_a_no() {
        let tally = VoteTally::new(raft_quorum_majority_count(3), 2);
        tally.state.lock().unwrap().record(None);
        tally.state.lock().unwrap().record(None);
        assert!(!tally.decided());
        let outcome = tally.outcome(true);
        assert!(!outcome.yes_ && !outcome.no_);
        assert_eq!(outcome.n_voted_yes_, 1, "only the candidate's own vote");
        assert_eq!(outcome.n_voted_no_, 0);
        assert!(outcome.timeouted_);
    }
}
