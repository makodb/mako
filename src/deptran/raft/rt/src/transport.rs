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
use std::sync::{Arc, Mutex, OnceLock};
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
    EmptyAppendEntriesResponse, InstallSnapshotRequest, InstallSnapshotRequestBytesRef, InstallSnapshotResponse,
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

/// Disk builds (plan P5; bugs-found B22): add_peer's first try lasts this
/// long (a refused connect returns at once; one to a black-holed address
/// takes srpc's connect timeout, 5 s), then a dial thread takes the site and
/// connects a fresh Client every DIAL_EVERY, so a node starts while a peer is
/// down; the worker then waits for a majority of each partition
/// (raft_transport_wait_majority).
pub const DISK_FIRST_DIAL: Duration = Duration::from_millis(1000);
pub const DIAL_EVERY: Duration = Duration::from_millis(200);
pub const MAJORITY_TIMEOUT: Duration = Duration::from_millis(120 * 1000);

/// A configured site's connection: set once, by add_peer or a dial thread.
/// Empty, a send to the site fails at once, as after a close.
type PeerSlot = Arc<OnceLock<Arc<Client>>>;

/// Disk builds (docs/verus/disk-persistence-plan.md P5; bugs-found B21): a
/// survivor re-dials a closed peer for good, from 50 ms doubling to 200 ms
/// (25-300 ms with jitter), so a restarted replica is reached again before
/// its first campaign (the shortest non-preferred election timeout is
/// 0.5 s). srpc's default policy gives up after five tries over 15.5-46.5 s,
/// and its `aggressive()` waits up to 5 s, time for several campaigns. A
/// memory build keeps the default.
pub const DISK_RECONNECT: srpc::reconnect_policy::ReconnectPolicy = srpc::reconnect_policy::ReconnectPolicy {
    auto_reconnect: true,
    max_retries: 0,
    initial_delay_ms: 50,
    max_delay_ms: 200,
    backoff_multiplier: 2.0,
    jitter_enabled: true,
};

/// One connect attempt: a Client on `poll` (the disk builds' reconnect
/// policy on it), or None, closed, if the site did not accept.
///
/// # Safety
/// `addr` is a live NUL-terminated string for the call.
unsafe fn dial(poll: &Arc<PollThread>, addr: *const i8) -> Option<Arc<Client>> {
    let client = Client::create(poll.clone());
    if cfg!(feature = "raft_disk") {
        client.set_reconnect_policy(&DISK_RECONNECT);
    }
    if client.connect(addr, false) == 0 {
        return Some(client);
    }
    client.close();
    None
}

/// What an append's reply wakes when collect is not waiting for it: the
/// sending server's replication gate (None: nothing).
pub type LateWake = Option<rusty::sync::Arc<raft::server_h::ReplicationWakeGate>>;

/// An append reply reached its slot (F11a). Collect, if it waits on this
/// thread, takes it. Otherwise the round it belonged to has ended -- its
/// deadline, one heartbeat interval, passed first, as a reply held for a
/// follower's WAL flush can make it -- and the reply would sit until a
/// later round's collect found it, a heartbeat interval later when no
/// Start wakes the loop (a saturated client at its outstanding cap). So the
/// first reply after a round that stalled so wakes the replication loop, as
/// RequestReplication does. Not after a round that ended on a majority: an
/// idle leader's second heartbeat reply would start a round per reply.
fn reply_landed(late: &LateWake) {
    if crate::seam::wake_collect() {
        raft_store::stats::add(raft_store::stats::REPLY_WOKE, 0);
    } else if let Some(gate) = late {
        let stalled = gate.take_stalled_round();
        raft_store::stats::add(raft_store::stats::REPLY_LATE, u64::from(stalled));
        if stalled {
            raft::server_h::request_replication_on(gate);
        }
    }
}

pub struct RaftTransport {
    poll: Arc<PollThread>,
    // None until serve() binds. Taken explicitly on delete, before the poll
    // handle, so the server's listener-close job still has a thread to run on.
    server: Option<Server>,
    // Which server serve() bound to, so delete() can unbind it.
    served: Option<*const RaftServerBase>,
    peers: HashMap<u16, PeerSlot>,
    partitions: HashMap<u32, Vec<u16>>,
    // Disk builds: the dial threads of sites add_peer could not reach yet,
    // stopped and joined on delete.
    dialers: Vec<std::thread::JoinHandle<()>>,
    dial_stop: Arc<AtomicBool>,
    network_enabled: AtomicBool,
    // Every RPC this transport sent: RaftCommo::rpc_count_'s counterpart,
    // which the lab's idle-RPC ceiling (TEST 9) reads.
    rpc_count: AtomicU64,
    // InstallSnapshots outstanding per follower (N6). Shared with the reply
    // callbacks, which may outlive a send call.
    installs: Arc<Mutex<InstallsInFlight>>,
}

/// What `send_install_snapshot_once` did.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum InstallSend {
    /// Sent; `done` will run at most once.
    Sent,
    /// The same `(term, follower, index)` is still outstanding and inside its
    /// deadline: nothing sent, `done` dropped uncalled.
    Suppressed,
    /// The request never left (no peer, or the send failed): `done` dropped
    /// uncalled, and nothing is recorded.
    NotSent,
}

/// The resend-suppression set (N6): one entry per follower, for the snapshot
/// last sent to it. Cleared by the entry's reply (success or failure), by the
/// callback being dropped uncalled, by a failed send, by its deadline, and --
/// all of them -- by a term change.
#[derive(Default)]
pub struct InstallsInFlight {
    term: u64,
    next_gen: u64,
    by_site: HashMap<u16, InstallEntry>,
}

struct InstallEntry {
    index: u64,
    generation: u64,
    deadline: Instant,
}

impl InstallsInFlight {
    /// Record a send of `(term, site, index)` unless the same one is
    /// outstanding; the entry's generation, or None to suppress.
    fn begin(&mut self, term: u64, site: u16, index: u64, now: Instant,
             deadline: Duration) -> Option<u64> {
        if term != self.term {
            self.by_site.clear();
            self.term = term;
        }
        if let Some(e) = self.by_site.get(&site) {
            if e.index == index && now < e.deadline {
                return None;
            }
        }
        self.next_gen += 1;
        let generation = self.next_gen;
        self.by_site.insert(site, InstallEntry { index, generation, deadline: now + deadline });
        Some(generation)
    }

    /// Clear `site`'s entry if it is still the one `generation` recorded.
    fn finish(&mut self, site: u16, generation: u64) {
        if self.by_site.get(&site).is_some_and(|e| e.generation == generation) {
            self.by_site.remove(&site);
        }
    }

    /// How many followers have an install outstanding (tests).
    pub fn outstanding(&self) -> usize {
        self.by_site.len()
    }
}

/// Clears its entry when dropped: after the reply callback ran, or when srpc
/// drops the callback uncalled.
struct InstallKey {
    installs: Arc<Mutex<InstallsInFlight>>,
    site: u16,
    generation: u64,
}

impl Drop for InstallKey {
    fn drop(&mut self) {
        self.installs.lock().unwrap_or_else(|e| e.into_inner()).finish(self.site, self.generation);
    }
}

// Dial threads stop with their transport, however it goes (delete joins).
impl Drop for RaftTransport {
    fn drop(&mut self) {
        self.dial_stop.store(true, Ordering::Release);
    }
}

impl RaftTransport {
    pub fn new() -> RaftTransport {
        RaftTransport {
            poll: PollThread::create(),
            server: None,
            served: None,
            peers: HashMap::new(),
            partitions: HashMap::new(),
            dialers: Vec::new(),
            dial_stop: Arc::new(AtomicBool::new(false)),
            network_enabled: AtomicBool::new(true),
            rpc_count: AtomicU64::new(0),
            installs: Arc::new(Mutex::new(InstallsInFlight::default())),
        }
    }

    /// Followers with an InstallSnapshot outstanding (tests).
    pub fn installs_outstanding(&self) -> usize {
        self.installs.lock().unwrap_or_else(|e| e.into_inner()).outstanding()
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

    /// Close admission and let the handlers already inside finish. Disk
    /// builds close Raft's gate first (plan P4): srpc's admission flag is
    /// not checked on dispatch, and a request handled now would hold its
    /// reply for a flush.
    pub fn drain(&self, timeout_ms: u64) -> bool {
        match self.server.as_ref() {
            Some(s) => {
                if cfg!(feature = "raft_disk") {
                    if let Some(server) = self.served {
                        // SAFETY: serve()'s contract: the server outlives
                        // this transport, and the worker drains before
                        // deleting it.
                        unsafe { (*server).CloseAdmissionForDrain() };
                    }
                }
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
        let start = Instant::now();
        let client = loop {
            // SAFETY: the caller's contract on `addr`.
            if let Some(c) = unsafe { dial(&self.poll, addr) } {
                break c;
            }
            if timeout.is_zero() || start.elapsed() >= timeout {
                return false;
            }
            std::thread::sleep(CONNECT_SLEEP.min(timeout));
        };
        let slot: PeerSlot = Arc::new(OnceLock::new());
        let _ = slot.set(client);
        self.peers.insert(site_id, slot);
        self.partitions.entry(par_id).or_default().push(site_id);
        true
    }

    /// # Safety
    /// As add_peer_with_timeout.
    pub unsafe fn add_peer(&mut self, par_id: u32, site_id: u16,
                           addr: *const i8) -> bool {
        if !cfg!(feature = "raft_disk") {
            return unsafe { self.add_peer_with_timeout(par_id, site_id, addr, CONNECT_TIMEOUT) };
        }
        if self.peers.contains_key(&site_id) {
            return false;
        }
        // SAFETY: the caller's contract on `addr`.
        if unsafe { self.add_peer_with_timeout(par_id, site_id, addr, DISK_FIRST_DIAL) } {
            return true;
        }
        // SAFETY: as above; copied, as the dial thread outlives the call.
        let addr = unsafe { std::ffi::CStr::from_ptr(addr) }.to_owned();
        let slot: PeerSlot = Arc::new(OnceLock::new());
        self.peers.insert(site_id, slot.clone());
        self.partitions.entry(par_id).or_default().push(site_id);
        let (poll, stop) = (self.poll.clone(), self.dial_stop.clone());
        self.dialers.push(std::thread::spawn(move || {
            while !stop.load(Ordering::Acquire) {
                // SAFETY: `addr` is owned by this thread.
                if let Some(c) = unsafe { dial(&poll, addr.as_ptr()) } {
                    let _ = slot.set(c);
                    return;
                }
                std::thread::sleep(DIAL_EVERY);
            }
        }));
        true
    }

    /// Whether every partition has a majority of its configured sites
    /// connected, this one included (add_peer records them all). Always so
    /// in a memory build, whose add_peer connects each or fails.
    pub fn majority_connected(&self) -> bool {
        self.partitions.values().all(|sites| {
            let up = sites.iter()
                .filter(|site| self.peers.get(site).is_some_and(|s| s.get().is_some()))
                .count();
            up > sites.len() / 2
        })
    }

    /// Disk builds (B22): wait, up to `timeout`, for majority_connected.
    pub fn wait_majority(&self, timeout: Duration) -> bool {
        let start = Instant::now();
        while !self.majority_connected() {
            if start.elapsed() >= timeout {
                return false;
            }
            std::thread::sleep(DIAL_EVERY / 4);
        }
        true
    }

    pub fn peer(&self, site_id: u16) -> Option<&Arc<Client>> {
        if !self.network_enabled() {
            return None;
        }
        self.peers.get(&site_id).and_then(|s| s.get())
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
            .filter_map(|site| self.peers.get(site).and_then(|s| s.get()).map(|c| (*site, c.clone())))
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
        self.peers.values().filter(|s| s.get().is_some()).count()
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

    fn append_sink(pending: &AppendReply, late: LateWake)
        -> srpc::client::AsyncReplyCallback {
        let sink = pending.slot.clone();
        Some(Box::new(move |code, ptr, len| {
            let value = if code != 0 {
                Err(code)
            } else {
                // SAFETY: srpc owns the buffer for this call.
                unsafe { decode_reply::<AppendEntriesResponse>(ptr, len) }.ok_or(-1)
            };
            if let Ok(r) = &value {  // [M0] trace kit
                if r.follower_append_ok != 0 {
                    crate::trace::through(7, r.follower_last_log_index, 0);
                }
            }
            if let Ok(mut guard) = sink.lock() {
                *guard = Some(value);
            }
            reply_landed(&late);
        }))
    }

    pub fn send_append_entries(&self, site_id: u16, req: AppendEntriesRequest)
        -> Option<AppendReply> {
        let client = self.peer(site_id)?;
        let pending = AppendReply::new();
        let on_reply = Self::append_sink(&pending, None);
        let proxy = RaftProxy { client };
        let sent = proxy.append_entries_async(req, on_reply);
        self.count_rpc();
        sent.ok()?;
        Some(pending)
    }

    /// AppendEntries with the payload written by `write_cmd` straight into
    /// the request archive -- the fixed fields come from `req`, whose `cmd`
    /// is ignored. The seam's send path: C++ serializes the Command directly
    /// into the frame, with no intermediate buffer on either side.
    pub fn send_append_entries_with<F>(&self, site_id: u16, req: &AppendEntriesRequest,
                                       write_cmd: F, late: LateWake) -> Option<AppendReply>
    where
        F: FnMut(&mut srpc::serializable::BinaryWriteArchive),
    {
        let client = self.peer(site_id)?;
        let pending = AppendReply::new();
        let on_reply = Self::append_sink(&pending, late);
        let proxy = RaftProxy { client };
        let sent = proxy.append_entries_with_async(req, write_cmd, on_reply);
        self.count_rpc();
        sent.ok()?;
        Some(pending)
    }

    /// EmptyAppendEntries replies with the same three fields as
    /// AppendEntries, so it lands in the same reply type.
    pub fn send_empty_append_entries(&self, site_id: u16,
                                     req: &EmptyAppendEntriesRequest, late: LateWake)
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
                reply_landed(&late);
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

    /// `send_install_snapshot_ref`, sent at most once per `(term, follower,
    /// index)` while a reply is outstanding (N6). Without this the core
    /// resends the whole image every heartbeat round until a reply advances
    /// the follower. The entry is cleared before `done` runs, so a reply that
    /// did not advance the follower lets the next round resend; and it lapses
    /// after `deadline` even if no reply or drop ever comes, so a lost
    /// callback cannot starve the follower. Install is idempotent, so a
    /// resend after the deadline is harmless.
    pub fn send_install_snapshot_once<F>(&self, site_id: u16,
                                         req: &InstallSnapshotRequestBytesRef<'_>,
                                         deadline: Duration, done: F) -> InstallSend
    where
        F: FnOnce(u64) + Send + 'static,
    {
        if self.peer(site_id).is_none() {
            return InstallSend::NotSent;
        }
        let begun = self.installs.lock().unwrap_or_else(|e| e.into_inner())
            .begin(req.term, site_id, req.last_included_index, Instant::now(), deadline);
        let Some(generation) = begun else {
            return InstallSend::Suppressed;
        };
        let key = InstallKey { installs: self.installs.clone(), site: site_id, generation };
        let sent = self.send_install_snapshot_ref(site_id, req, move |term| {
            drop(key); // clear first: `done` may start the next round
            done(term);
        });
        // A send that never left dropped the callback, and `key` with it.
        if sent { InstallSend::Sent } else { InstallSend::NotSent }
    }

    /// InstallSnapshot encoded straight from a borrowed image (plan N5): the
    /// fixed fields and the snapshot bytes go into the frame in one copy, with
    /// no `WireBytes` built first. `done` is as for `send_install_snapshot_with`:
    /// at most once, with the follower's term or 0 on failure; a `false`
    /// return means the request never left and `done` is dropped uncalled.
    pub fn send_install_snapshot_ref<F>(&self, site_id: u16,
                                        req: &InstallSnapshotRequestBytesRef<'_>,
                                        done: F) -> bool
    where
        F: FnOnce(u64) + Send + 'static,
    {
        let Some(client) = self.peer(site_id) else {
            return false;
        };
        let proxy = RaftProxy { client };
        let mut done = Some(done);
        let sent = proxy.install_snapshot_ref_async(
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
        for (site, client) in self.peers_in_partition(par_id, self_site_id) {
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
                        // [fix, F1] attributed to the peer this callback was
                        // created for, so a reply delivered twice counts once.
                        guard.feed(site, reply.vote_granted != 0, reply.max_ballot);
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
    // [fix, F1] The voters already counted in this campaign: each counts at
    // most once, however many times its reply is delivered. A campaign is
    // one broadcast at one term, so per-campaign is per-campaign-term.
    voters: Vec<u16>,
    // [move, M5] Every delivered reply, in order -- (voter, granted, term) --
    // for the core, which counts the campaign itself (RaftCore::
    // election_settle). This tally still decides when the wait ends.
    replies: Vec<(u16, bool, i64)>,
}

impl TallyState {
    /// RaftVoteQuorumEvent::FeedResponse: a non-negative reply term that is
    /// higher than any seen advances the term, then the vote counts -- once
    /// per voter ([fix, F1]: the C++ event, and this tally before it, counted
    /// replies, so one reply delivered twice would have counted twice).
    fn feed(&mut self, voter: u16, granted: bool, term: i64) {
        self.replies.push((voter, granted, term));  // [move, M5]
        // [fix, F1] a repeated reply carries nothing new
        if self.voters.contains(&voter) {
            return;
        }
        self.voters.push(voter);
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

    // [fix, F15] Lost once so many of the n_total - 1 peers refused that the
    // quorum of peer votes is out of reach: no > (n - 1) - n/2 (bugs-found
    // B1). The C++ QuorumEvent::no it came from, no > n - n/2, counted this
    // server as a possible refusal, so with three servers a lost campaign
    // always waited out its deadline.
    fn no(&self) -> bool {
        self.n_total > 0 && self.no > self.n_total - self.quorum - 1
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
                voters: Vec::new(),  // [fix, F1]
                replies: Vec::new(),  // [move, M5]
            })),
        }
    }

    /// A campaign that could not be broadcast at all (no transport bound):
    /// nothing will ever reply, so it is a timeout, as the C++ event would be.
    pub fn unreachable() -> VoteTally {
        // A quorum no reply can reach: yes never, no never.
        VoteTally::new(usize::MAX / 2)
    }

    /// [move, M5] The quorum size this tally counts against.
    pub fn n_total(&self) -> usize {
        self.state.lock().map(|s| s.n_total).unwrap_or(0)
    }

    /// [move, M5] How many replies were delivered, and each in turn.
    pub fn reply_count(&self) -> usize {
        self.state.lock().map(|s| s.replies.len()).unwrap_or(0)
    }

    pub fn reply_at(&self, i: usize) -> Option<(u16, bool, i64)> {
        self.state.lock().ok().and_then(|s| s.replies.get(i).copied())
    }

    pub fn decided(&self) -> bool {
        self.state.lock().map(|s| s.yes() || s.no()).unwrap_or(false)
    }

    /// The six scalars the C++ event reports, field for field. The core
    /// counts a campaign itself now (RaftCore::election_settle); this stays
    /// for the tally's own tests, which pin lane parity.
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
// lives in a registry keyed by server identity, read lock-free: the
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

/// Disk builds (B22): after every add_peer, wait for a majority of each
/// partition (MAJORITY_TIMEOUT); false fails the start, as add_peer's
/// timeout did. A memory build has them all.
///
/// # Safety
/// `t` came from raft_transport_new.
#[no_mangle]
pub unsafe extern "C" fn raft_transport_wait_majority(t: *mut RaftTransport) -> bool {
    unsafe { (*t).wait_majority(MAJORITY_TIMEOUT) }
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
    owned.dial_stop.store(true, Ordering::Release);
    for dialer in owned.dialers.drain(..) {
        let _ = dialer.join();
    }
    for client in owned.peers.values().filter_map(|s| s.get()) {
        client.close();
    }
    owned.peers.clear();
    owned.poll.shutdown();
    drop(owned);
}

#[cfg(test)]
mod tests {
    use super::*;

    // Each listed reply comes from a different peer (sites 1, 2, ...).
    fn fed(n: usize, votes: &[(bool, i64)]) -> VoteTally {
        let tally = VoteTally::new(n);
        for (i, (granted, term)) in votes.iter().enumerate() {
            tally.state.lock().unwrap().feed(i as u16 + 1, *granted, *term);
        }
        tally
    }

    #[test]
    fn a_stale_term_reply_is_counted_as_cast() {
        // Pinned, not endorsed: the tally does not compare a reply's term
        // with the campaign's, so a grant below the campaign term counts.
        // A correct voter cannot send one -- it adopts the candidate's term
        // before granting, and refuses at its own higher term otherwise --
        // and each campaign has its own tally, so an earlier campaign's late
        // reply never reaches this one.
        let tally = VoteTally::new(3);
        tally.state.lock().unwrap().feed(2, true, 1);
        assert!(tally.decided());
        assert!(tally.outcome(false).yes_);
        assert_eq!(tally.outcome(false).term_, 1);
    }

    #[test]
    fn a_repeated_reply_counts_once() {
        // [fix, F1] Five replicas need two granted peer votes. One peer's
        // grant delivered twice is still one vote.
        let tally = VoteTally::new(5);
        tally.state.lock().unwrap().feed(2, true, 4);
        tally.state.lock().unwrap().feed(2, true, 4);
        assert!(!tally.decided(), "one voter, counted twice, must not make a quorum");
        assert_eq!(tally.outcome(false).n_voted_yes_, 1);
        tally.state.lock().unwrap().feed(3, true, 4);
        assert!(tally.decided());
        assert!(tally.outcome(false).yes_);
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
    fn three_replicas_lose_once_both_peers_reject() {
        // [fix, F15] no() is n_voted_no > (n - 1) - n/2 = 1: with both peers
        // refusing, a yes quorum is out of reach (bugs-found B1). One refusal
        // decides nothing: the other peer may still grant.
        assert!(!fed(3, &[(false, 4)]).decided());
        let tally = fed(3, &[(false, 4), (false, 4)]);
        assert!(tally.decided());
        assert!(tally.outcome(false).no_);
    }

    #[test]
    fn five_replicas_lose_once_three_peers_reject() {
        // [fix, F15] no() > (5 - 1) - 2 = 2: three refusals leave one peer,
        // short of the two peer votes a quorum needs.
        assert!(!fed(5, &[(false, 1), (false, 1)]).decided());
        assert!(fed(5, &[(false, 1), (false, 1), (false, 1)]).decided());
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
