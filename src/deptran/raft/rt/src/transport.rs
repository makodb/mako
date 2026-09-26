// Raft's RPC transport on the Rust srpc lane: one poll thread, one server,
// one set of peer clients.
//
// WHY ONE TYPE. Inbound and outbound share one poll thread in the C++ lane
// (raft_worker.cc SetupService/SetupCommo), so moving the server without the
// clients would give Raft a second poll thread -- adding a thread rather than
// swapping one. This owns both ends, so the lane is one change.
//
// WHY IT IS NOT Send. srpc::client::Client holds one RefCell and eight Cell
// fields (src/srpc/rpc/client.rs): Send but !Sync, so Arc<Client> is neither.
// A client belongs to its poll thread, and every send already happens there,
// on the heartbeat or election fiber. C++ holds this as an opaque pointer.
// The callers from elsewhere are set_network_enabled, which touches only an
// atomic, and post, which is a channel send.
//
// WHAT IT MIRRORS. The peer table has the shape of PeerRegistry
// (src/deptran/communicator.h): partitions own site ids, the table owns
// peers. Like the C++ Communicator it records EVERY site of every partition,
// this one included, so a partition's recorded size is its configured size.

use std::collections::HashMap;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use raft::server_h::RaftServerBase;
use raft::server_pods_h::RaftVoteOutcome;
use srpc::misc::{Job, OneTimeJob};
use srpc::client::Client;
use srpc::reactor::PollThread;
use srpc::serializable::{
    make_source_proxy_buffer, BinaryReadArchive, BufferSource, Deserialize,
};
use srpc::server::Server;

use crate::rpc::{
    AppendEntriesRequest, AppendEntriesResponse, EmptyAppendEntriesRequest,
    EmptyAppendEntriesResponse, InstallSnapshotRequest, InstallSnapshotResponse,
    RaftProxy, VoteRequest, VoteResponse,
};
use crate::service::RaftRpcService;

/// One outstanding reply, as the sender reads it back. Arc<Mutex<..>> because
/// the reply lands in a client callback while the heartbeat fiber reads it
/// later -- the shape srpc's own Future uses, and the counterpart of the C++
/// lane's shared_ptr<AppendEntriesResponse> whose `completed` field is polled.
pub struct Pending<T> {
    slot: Arc<Mutex<Option<Result<T, i32>>>>,
}

impl<T: Clone> Pending<T> {
    fn new() -> Pending<T> {
        Pending { slot: Arc::new(Mutex::new(None)) }
    }

    /// The reply, if it has landed, left in place. PHASE 2 reads a slot on
    /// every pass until the round moves on, so reading must not consume.
    pub fn peek(&self) -> Option<Result<T, i32>> {
        self.slot.lock().ok().and_then(|guard| guard.clone())
    }

    pub fn take(&self) -> Option<Result<T, i32>> {
        self.slot.lock().ok().and_then(|mut guard| guard.take())
    }

    pub fn is_ready(&self) -> bool {
        self.slot.lock().map(|guard| guard.is_some()).unwrap_or(false)
    }
}

/// AppendEntries and EmptyAppendEntries reply with the same three fields, so
/// both land in one reply type.
pub type AppendReply = Pending<AppendEntriesResponse>;

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

/// How long add_peer keeps trying, and how often: Communicator's
/// CONNECT_TIMEOUT_MS and CONNECT_SLEEP_MS (src/deptran/communicator.h).
pub const CONNECT_TIMEOUT: Duration = Duration::from_millis(120 * 1000);
pub const CONNECT_SLEEP: Duration = Duration::from_millis(1000);

pub struct RaftTransport {
    poll: Arc<PollThread>,
    // None until serve() binds. Taken explicitly on delete, before the poll
    // handle, so the server's listener-close job still has a thread to run on.
    server: Option<Server>,
    // Which server serve() bound to, so delete() can unbind it.
    served: Option<*const RaftServerBase>,
    peers: HashMap<u16, Arc<Client>>,
    partitions: HashMap<u32, Vec<u16>>,
    network_enabled: AtomicBool,
    // Every RPC this transport sent: RaftCommo::rpc_count_'s counterpart,
    // which the lab's idle-RPC ceiling (TEST 9) reads.
    rpc_count: AtomicU64,
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
            rpc_count: AtomicU64::new(0),
        }
    }

    pub fn poll_thread(&self) -> Arc<PollThread> {
        self.poll.clone()
    }

    /// Run `f` once on this transport's poll thread, as a job -- which is how
    /// the worker runs EnsureSetup there, so the Raft fibers it spawns belong
    /// to this reactor.
    pub fn post(&self, f: Box<dyn FnMut() + Send + Sync>) {
        self.poll.add(Arc::new(OneTimeJob::new(f)) as Arc<dyn Job>);
    }

    /// Bind and start serving Raft's four RPCs for `server`, with admission
    /// CLOSED: the worker opens it once startup has recovered, as the C++
    /// lane does (raft_worker.cc SetupService). srpc's Rust server starts
    /// with admission open (rpc/server.rs), so this closes it before start.
    ///
    /// # Safety
    /// `server` outlives this transport -- the worker drains before deleting
    /// it -- and `bind_addr` is a live NUL-terminated string for the call.
    pub unsafe fn serve(&mut self, server: *mut RaftServerBase,
                        bind_addr: *const i8) -> i32 {
        let mut rpc_server = Server::new(Some(self.poll.clone()));
        rpc_server.set_admission_ready(false);
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

    /// Close admission and let the handlers already inside finish.
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

    /// Connect to one site and record it under its partition, retrying for
    /// up to `timeout` as Communicator::ConnectToAddress does. `addr` is a
    /// NUL-terminated host:port.
    ///
    /// # Safety
    /// `addr` is a live NUL-terminated string for the duration of the call.
    pub unsafe fn add_peer_with_timeout(&mut self, par_id: u32, site_id: u16,
                                        addr: *const i8, timeout: Duration) -> bool {
        if self.peers.contains_key(&site_id) {
            return false;
        }
        let client = Client::create(self.poll.clone());
        let start = Instant::now();
        loop {
            if client.connect(addr, false) == 0 {
                break;
            }
            if timeout.is_zero() || start.elapsed() >= timeout {
                client.close();
                return false;
            }
            std::thread::sleep(CONNECT_SLEEP.min(timeout));
        }
        self.peers.insert(site_id, client);
        self.partitions.entry(par_id).or_default().push(site_id);
        true
    }

    /// # Safety
    /// As add_peer_with_timeout.
    pub unsafe fn add_peer(&mut self, par_id: u32, site_id: u16,
                           addr: *const i8) -> bool {
        unsafe { self.add_peer_with_timeout(par_id, site_id, addr, CONNECT_TIMEOUT) }
    }

    pub fn peer(&self, site_id: u16) -> Option<&Arc<Client>> {
        if !self.network_enabled() {
            return None;
        }
        self.peers.get(&site_id)
    }

    /// Every peer in a partition except the caller, which is who a broadcast
    /// reaches: PeersForPartition plus the self-skip RaftCommo does.
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

    /// The configured size of a partition: every recorded site, this one
    /// included, which is what Config::GetPartitionSize returns to the C++
    /// BroadcastVote. A partition the worker never recorded has one member.
    pub fn partition_size(&self, par_id: u32) -> usize {
        self.partitions.get(&par_id).map(|s| s.len()).unwrap_or(1)
    }

    // Release/Acquire, as the atomic flag in Communicator uses.
    pub fn set_network_enabled(&self, enabled: bool) {
        self.network_enabled.store(enabled, Ordering::Release);
    }

    pub fn network_enabled(&self) -> bool {
        self.network_enabled.load(Ordering::Acquire)
    }

    pub fn peer_count(&self) -> usize {
        self.peers.len()
    }

    pub fn rpc_count(&self) -> u64 {
        self.rpc_count.load(Ordering::Relaxed)
    }

    fn count_rpc(&self) {
        self.rpc_count.fetch_add(1, Ordering::Relaxed);
    }

    // --- the send paths
    //
    // Each returns None when there is nothing to send to -- an unknown site,
    // the network flag down, or a send that never left -- which is the null
    // the C++ PeerForSite returns and which callers handle as a lost RPC.

    fn append_sink(pending: &AppendReply)
        -> srpc::client::AsyncReplyCallback {
        let sink = pending.slot.clone();
        Some(Box::new(move |code, ptr, len| {
            let value = if code != 0 {
                Err(code)
            } else {
                // SAFETY: srpc owns the buffer for this call.
                unsafe { decode_reply::<AppendEntriesResponse>(ptr, len) }.ok_or(-1)
            };
            if let Ok(mut guard) = sink.lock() {
                *guard = Some(value);
            }
        }))
    }

    pub fn send_append_entries(&self, site_id: u16, req: AppendEntriesRequest)
        -> Option<AppendReply> {
        let client = self.peer(site_id)?;
        let pending = AppendReply::new();
        let on_reply = Self::append_sink(&pending);
        let proxy = RaftProxy { client };
        let sent = proxy.append_entries_async(req, on_reply);
        self.count_rpc();
        sent.ok()?;
        Some(pending)
    }

    /// EmptyAppendEntries replies with the same three fields as
    /// AppendEntries, so it lands in the same reply type.
    pub fn send_empty_append_entries(&self, site_id: u16,
                                     req: &EmptyAppendEntriesRequest)
        -> Option<AppendReply> {
        let client = self.peer(site_id)?;
        let pending = AppendReply::new();
        let sink = pending.slot.clone();
        let proxy = RaftProxy { client };
        let sent = proxy.empty_append_entries_async(
            req,
            Some(Box::new(move |code, ptr, len| {
                let value = if code != 0 {
                    Err(code)
                } else {
                    // SAFETY: srpc owns the buffer for this call.
                    unsafe { decode_reply::<EmptyAppendEntriesResponse>(ptr, len) }
                        .map(|r| AppendEntriesResponse {
                            follower_append_ok: r.follower_append_ok,
                            follower_current_term: r.follower_current_term,
                            follower_last_log_index: r.follower_last_log_index,
                        })
                        .ok_or(-1)
                };
                if let Ok(mut guard) = sink.lock() {
                    *guard = Some(value);
                }
            })),
        );
        self.count_rpc();
        sent.ok()?;
        Some(pending)
    }

    pub fn send_install_snapshot(&self, site_id: u16, req: InstallSnapshotRequest)
        -> Option<Pending<InstallSnapshotResponse>> {
        let client = self.peer(site_id)?;
        let pending: Pending<InstallSnapshotResponse> = Pending::new();
        let sink = pending.slot.clone();
        let proxy = RaftProxy { client };
        let sent = proxy.install_snapshot_async(
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
        );
        self.count_rpc();
        sent.ok()?;
        Some(pending)
    }

    /// InstallSnapshot with a completion callback, the shape the C++ lane's
    /// SendInstallSnapshot has: `done` runs exactly once, on the poll thread,
    /// with the follower's term or 0 on any failure. Returns false when the
    /// request never left, in which case `done` is NOT called -- the caller
    /// delivers the failure itself, on its own stack, as commo.cc does.
    pub fn send_install_snapshot_with<F>(&self, site_id: u16,
                                         req: InstallSnapshotRequest, done: F) -> bool
    where
        F: FnOnce(u64) + Send + 'static,
    {
        let Some(client) = self.peer(site_id) else {
            return false;
        };
        let proxy = RaftProxy { client };
        let mut done = Some(done);
        let sent = proxy.install_snapshot_async(
            req,
            Some(Box::new(move |code, ptr, len| {
                let term = if code != 0 {
                    0
                } else {
                    // SAFETY: srpc owns the buffer for this call.
                    unsafe { decode_reply::<InstallSnapshotResponse>(ptr, len) }
                        .map(|r| r.term_out)
                        .unwrap_or(0)
                };
                if let Some(f) = done.take() {
                    f(term);
                }
            })),
        );
        self.count_rpc();
        sent.is_ok()
    }

    /// The campaign broadcast, and its tally.
    ///
    /// The quorum rule is the C++ lane's, exactly: RaftCommo::BroadcastVote
    /// builds RaftVoteQuorumEvent(n, n / 2) with n the configured partition
    /// size, which counts PEER votes -- yes once peer yes votes reach n/2, no
    /// once peer no votes exceed n - n/2. The quorum comes from the
    /// configured partition, NEVER from who can be reached: deriving it from
    /// reachable peers is a split-brain (a partitioned candidate would elect
    /// itself on its own vote), which tests/transport_roundtrip.rs pins.
    pub fn broadcast_vote(&self, par_id: u32, self_site_id: u16,
                          req: &VoteRequest) -> VoteTally {
        let n = self.partition_size(par_id);
        let tally = VoteTally::new(n);
        for (_site, client) in self.peers_in_partition(par_id, self_site_id) {
            let sink = tally.state.clone();
            let proxy = RaftProxy { client: &client };
            let sent = proxy.vote_async(
                req,
                Some(Box::new(move |code, ptr, len| {
                    // A failed RPC feeds nothing, as commo.cc's callback
                    // returns early on an error code.
                    if code != 0 {
                        return;
                    }
                    // SAFETY: srpc owns the buffer for this call.
                    let Some(reply) = (unsafe { decode_reply::<VoteResponse>(ptr, len) })
                    else {
                        return;
                    };
                    if let Ok(mut guard) = sink.lock() {
                        guard.feed(reply.vote_granted != 0, reply.max_ballot);
                    }
                })),
            );
            self.count_rpc();
            let _ = sent;
        }
        tally
    }
}

impl Default for RaftTransport {
    fn default() -> Self {
        Self::new()
    }
}

/// What a campaign has heard, with RaftVoteQuorumEvent's counting rule.
struct TallyState {
    n_total: usize,
    quorum: usize,
    yes: usize,
    no: usize,
    highest_term: i64,
}

impl TallyState {
    /// RaftVoteQuorumEvent::FeedResponse: a non-negative reply term that is
    /// higher than any seen advances the term, then the vote counts.
    fn feed(&mut self, granted: bool, term: i64) {
        if term >= 0 && term > self.highest_term {
            self.highest_term = term;
        }
        if granted {
            self.yes += 1;
        } else {
            self.no += 1;
        }
    }

    fn yes(&self) -> bool {
        self.yes >= self.quorum
    }

    // QuorumEvent::no under the default policy.
    fn no(&self) -> bool {
        self.no > self.n_total - self.quorum
    }
}

/// The campaign's outcome, read once the wait is over.
pub struct VoteTally {
    state: Arc<Mutex<TallyState>>,
}

impl VoteTally {
    /// `n_total` is the configured partition size, self included.
    fn new(n_total: usize) -> VoteTally {
        VoteTally {
            state: Arc::new(Mutex::new(TallyState {
                n_total,
                quorum: n_total / 2,
                yes: 0,
                no: 0,
                highest_term: 0,
            })),
        }
    }

    /// A campaign that could not be broadcast at all (no transport bound):
    /// nothing will ever reply, so it is a timeout, as the C++ event would be.
    pub fn unreachable() -> VoteTally {
        // A quorum no reply can reach: yes never, no never.
        VoteTally::new(usize::MAX / 2)
    }

    pub fn decided(&self) -> bool {
        self.state.lock().map(|s| s.yes() || s.no()).unwrap_or(false)
    }

    /// The six scalars raft_vote_quorum_snapshot reads out of the C++ event,
    /// field for field, so the core cannot tell which lane produced them.
    pub fn outcome(&self, timed_out: bool) -> RaftVoteOutcome {
        let Ok(s) = self.state.lock() else {
            return RaftVoteOutcome {
                term_: 0, yes_: false, no_: false,
                n_voted_yes_: 0, n_voted_no_: 0, timeouted_: true,
            };
        };
        RaftVoteOutcome {
            term_: s.highest_term,
            yes_: s.yes(),
            no_: s.no(),
            n_voted_yes_: s.yes as i32,
            n_voted_no_: s.no as i32,
            timeouted_: timed_out,
        }
    }
}

// ===========================================================================
// The transport, resolved from the server that owns it.
//
// The server cannot hold this: RaftTransport is !Send while RaftServerBase
// must stay Send + Sync, or RaftRpcService loses srpc's trait bound. So it
// lives in a registry keyed by server identity -- the shape the C++ lane's
// commo table has (server_seam_cpp.cc), and read the same lock-free way: the
// table is published, never mutated, and readers walk whatever they loaded.
// ===========================================================================

/// A transport pointer, as the registry holds it.
#[derive(Clone, Copy)]
struct TransportPtr(*mut RaftTransport);

// SAFETY: this asserts that the POINTER may be handed between threads, not
// that the transport may be USED from any thread. Every use goes through
// transport_of on the poll thread that owns it, except set_network_enabled
// (one atomic) and post (a channel send).
unsafe impl Send for TransportPtr {}
unsafe impl Sync for TransportPtr {}

type Table = HashMap<usize, TransportPtr>;

fn published() -> &'static std::sync::atomic::AtomicPtr<Table> {
    static TABLE: std::sync::OnceLock<std::sync::atomic::AtomicPtr<Table>> =
        std::sync::OnceLock::new();
    TABLE.get_or_init(|| {
        std::sync::atomic::AtomicPtr::new(Box::into_raw(Box::new(Table::new())))
    })
}

fn write_lock() -> &'static Mutex<()> {
    static LOCK: Mutex<()> = Mutex::new(());
    &LOCK
}

/// Publish a copy of the table with `edit` applied. The retired table is
/// deliberately leaked, as the C++ commo table's is: a reader may still be
/// walking it, and a write happens twice per server lifetime.
fn republish(edit: impl FnOnce(&mut Table)) {
    let _guard = write_lock().lock();
    let current = published().load(Ordering::Acquire);
    // SAFETY: published tables are never freed.
    let mut next: Table = unsafe { (*current).clone() };
    edit(&mut next);
    published().store(Box::into_raw(Box::new(next)), Ordering::Release);
}

/// Bind `transport` to `server`. Called once, before any send.
pub fn bind_transport(server: *const RaftServerBase, transport: *mut RaftTransport) {
    republish(|t| {
        t.insert(server as usize, TransportPtr(transport));
    });
}

pub fn unbind_transport(server: *const RaftServerBase) {
    republish(|t| {
        t.remove(&(server as usize));
    });
}

/// The transport bound to `server`, if any.
///
/// # Safety
/// The returned reference is valid while the worker holds the transport, and
/// must be used on the poll thread that owns it (bar the two noted above).
pub unsafe fn transport_of(server: *const RaftServerBase)
    -> Option<&'static RaftTransport> {
    let table = published().load(Ordering::Acquire);
    // SAFETY: published tables are never freed.
    let raw = unsafe { (*table).get(&(server as usize))? }.0;
    // SAFETY: bind/unbind bracket the transport's lifetime.
    Some(unsafe { &*raw })
}

// ===========================================================================
// The C ABI, for the worker that owns a transport (transport_exports.h).
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
        bind_transport(server, t);
    }
    ret
}

/// Serve an ADDITIONAL address for `server` -- the kSingleGroup stub sites --
/// on this transport's poll thread, without rebinding the registry. The
/// stubs host the same one service over the same one server, so they add a
/// listener, not a thread.
///
/// # Safety
/// As raft_transport_serve.
#[no_mangle]
pub unsafe extern "C" fn raft_transport_serve_stub(t: *mut RaftTransport,
                                                   server: *mut RaftServerBase,
                                                   bind_addr: *const i8)
    -> *mut Server {
    let transport = unsafe { &*t };
    let mut rpc_server = Server::new(Some(transport.poll.clone()));
    rpc_server.set_admission_ready(false);
    rpc_server.reg_service(Box::new(unsafe { RaftRpcService::new(server) }));
    if unsafe { rpc_server.start(bind_addr) } != 0 {
        return core::ptr::null_mut();
    }
    Box::into_raw(Box::new(rpc_server))
}

/// # Safety
/// `s` came from raft_transport_serve_stub.
#[no_mangle]
pub unsafe extern "C" fn raft_stub_server_set_admission_ready(s: *mut Server, ready: bool) {
    unsafe { (*s).set_admission_ready(ready) }
}

/// # Safety
/// `s` came from raft_transport_serve_stub.
#[no_mangle]
pub unsafe extern "C" fn raft_stub_server_drain(s: *mut Server, timeout_ms: u64) -> bool {
    let server = unsafe { &*s };
    server.set_admission_ready(false);
    server.drain(timeout_ms)
}

/// # Safety
/// `s` came from raft_transport_serve_stub and is not used afterwards.
#[no_mangle]
pub unsafe extern "C" fn raft_stub_server_delete(s: *mut Server) {
    drop(unsafe { Box::from_raw(s) });
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
pub unsafe extern "C" fn raft_transport_set_admission_ready(t: *mut RaftTransport,
                                                            ready: bool) {
    unsafe { (*t).set_admission_ready(ready) }
}

/// # Safety
/// `t` came from raft_transport_new.
#[no_mangle]
pub unsafe extern "C" fn raft_transport_bound_port(t: *const RaftTransport) -> i32 {
    unsafe { (*t).bound_port() }
}

/// # Safety
/// `t` came from raft_transport_new.
#[no_mangle]
pub unsafe extern "C" fn raft_transport_rpc_count(t: *const RaftTransport) -> u64 {
    unsafe { (*t).rpc_count() }
}

/// Run `f(ctx)` once on the transport's poll thread.
///
/// # Safety
/// `t` came from raft_transport_new; `f` is safe to call with `ctx` on
/// another thread, exactly once.
#[no_mangle]
pub unsafe extern "C" fn raft_transport_post(t: *mut RaftTransport,
                                             f: unsafe extern "C" fn(*mut core::ffi::c_void),
                                             ctx: *mut core::ffi::c_void) {
    struct Call(unsafe extern "C" fn(*mut core::ffi::c_void), usize);
    // SAFETY: the caller's contract -- f(ctx) may run on the poll thread.
    unsafe impl Send for Call {}
    unsafe impl Sync for Call {}
    let call = Call(f, ctx as usize);
    let mut once = Some(call);
    unsafe { &*t }.post(Box::new(move || {
        if let Some(c) = once.take() {
            unsafe { (c.0)(c.1 as *mut core::ffi::c_void) };
        }
    }));
}

/// # Safety
/// `t` came from raft_transport_new.
#[no_mangle]
pub unsafe extern "C" fn raft_transport_drain(t: *mut RaftTransport,
                                              timeout_ms: u64) -> bool {
    unsafe { (*t).drain(timeout_ms) }
}

/// Drop the RPC server (its listener-close job runs on the still-live poll
/// thread) and unbind, leaving the poll thread and clients. The worker calls
/// this where the C++ lane deletes rpc_server_, before the Raft server goes.
///
/// # Safety
/// `t` came from raft_transport_new.
#[no_mangle]
pub unsafe extern "C" fn raft_transport_close_server(t: *mut RaftTransport) {
    let transport = unsafe { &mut *t };
    if let Some(server) = transport.served.take() {
        unbind_transport(server);
    }
    drop(transport.server.take());
}

/// Close the clients, shut the poll thread down and free the transport. The
/// worker calls this last, where the C++ lane shuts its poll thread down.
///
/// # Safety
/// `t` came from raft_transport_new and is not used afterwards.
#[no_mangle]
pub unsafe extern "C" fn raft_transport_delete(t: *mut RaftTransport) {
    let mut owned = unsafe { Box::from_raw(t) };
    if let Some(server) = owned.served.take() {
        unbind_transport(server);
    }
    drop(owned.server.take());
    owned.set_network_enabled(false);
    for client in owned.peers.values() {
        client.close();
    }
    owned.peers.clear();
    owned.poll.shutdown();
    drop(owned);
}

#[cfg(test)]
mod tests {
    use super::*;

    fn fed(n: usize, votes: &[(bool, i64)]) -> VoteTally {
        let tally = VoteTally::new(n);
        for (granted, term) in votes {
            tally.state.lock().unwrap().feed(*granted, *term);
        }
        tally
    }

    #[test]
    fn a_candidate_alone_in_its_partition_has_already_won() {
        // n = 1: quorum 0 peer votes, so yes() holds before any reply,
        // exactly as RaftVoteQuorumEvent(1, 0) does.
        let tally = fed(1, &[]);
        assert!(tally.decided());
        assert!(tally.outcome(false).yes_);
    }

    #[test]
    fn three_replicas_need_one_granted_peer_vote() {
        assert!(!fed(3, &[]).decided());
        let tally = fed(3, &[(true, 4)]);
        let o = tally.outcome(false);
        assert!(o.yes_ && !o.no_);
        assert_eq!(o.n_voted_yes_, 1, "peer votes only, as the C++ event counts");
    }

    #[test]
    fn three_replicas_never_lose_early() {
        // C++: no() is n_voted_no > n - n/2 = 2, unreachable with two peers,
        // so a rejected three-replica campaign waits out its timeout.
        let tally = fed(3, &[(false, 4), (false, 4)]);
        assert!(!tally.decided());
        assert!(!tally.outcome(true).no_);
    }

    #[test]
    fn five_replicas_lose_only_when_every_peer_rejects() {
        assert!(!fed(5, &[(false, 1), (false, 1), (false, 1)]).decided());
        assert!(fed(5, &[(false, 1), (false, 1), (false, 1), (false, 1)]).decided());
    }

    #[test]
    fn the_highest_nonnegative_term_is_what_comes_back() {
        let tally = fed(5, &[(false, 11), (false, 5), (false, -3)]);
        assert_eq!(tally.outcome(false).term_, 11);
        assert_eq!(fed(3, &[(false, -7)]).outcome(false).term_, 0,
                   "a negative reply term is a sentinel, never a term");
    }

    #[test]
    fn an_unbound_campaign_never_decides() {
        assert!(!VoteTally::unreachable().decided());
    }
}
