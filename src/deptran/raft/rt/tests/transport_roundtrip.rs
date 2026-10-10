//! The Rust send path, over a real socket.
//!
//! Stage 3e's transport is useless if it only compiles, and the unit tests
//! beside it cover the tally's arithmetic rather than the wire. This stands
//! up a real srpc server, points a real RaftTransport at it and runs a real
//! campaign, so what is proven is the whole chain: connect, request_async,
//! the generated request encoding, the generated register and dispatch on the
//! far side, the reply encoding, the callback's decode, and the tally.
//!
//! The responder is built from the SAME generated code the production service
//! uses -- rpc::register and rpc::dispatch -- so this also checks that the
//! emitter's two halves agree with each other.
#![allow(unsafe_code)]

use std::ffi::CString;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use raft_rt::rpc::{
    self, AppendEntriesRequest, AppendEntriesRequestRef, AppendEntriesResponse, EmptyAppendEntriesRequest,
    EmptyAppendEntriesResponse, InstallSnapshotRequest, InstallSnapshotRequestBytesRef,
    InstallSnapshotResponse, RaftHandler,
    VoteRequest, VoteResponse, WireBytes,
};
use raft_rt::snapshot::{install_fits_one_frame, INSTALL_FRAME_OVERHEAD};
use raft_rt::transport::{InstallSend, RaftTransport};
use srpc::reactor::PollThread;
use srpc::server::{Request, Server, Service, WeakServerConnection};

/// A follower that answers every vote with `granted`, and records what the
/// other three RPCs carried so the tests can check the bytes that crossed.
#[derive(Default)]
struct Seen {
    append_cmd: Option<Vec<u8>>,
    empty_appends: usize,
    snapshot: Option<Vec<u8>>,
}

struct Grants {
    granted: i8,
    seen: Arc<Mutex<Seen>>,
}

impl RaftHandler for Grants {
    fn vote(&self, req: &VoteRequest) -> Result<VoteResponse, i32> {
        Ok(VoteResponse { max_ballot: req.cur_term, vote_granted: self.granted })
    }
    fn append_entries(&self, req: &AppendEntriesRequestRef<'_>) -> Result<AppendEntriesResponse, i32> {
        self.seen.lock().unwrap().append_cmd = Some(req.cmd.to_vec());
        Ok(AppendEntriesResponse {
            follower_append_ok: 1,
            follower_current_term: req.leader_current_term,
            follower_last_log_index: req.leader_prev_log_index + 1,
        })
    }
    fn empty_append_entries(
        &self,
        req: &EmptyAppendEntriesRequest,
    ) -> Result<EmptyAppendEntriesResponse, i32> {
        self.seen.lock().unwrap().empty_appends += 1;
        Ok(EmptyAppendEntriesResponse {
            follower_append_ok: 1,
            follower_current_term: req.leader_current_term,
            follower_last_log_index: req.leader_prev_log_index,
        })
    }
    fn install_snapshot(&self, req: InstallSnapshotRequest) -> Result<InstallSnapshotResponse, i32> {
        self.seen.lock().unwrap().snapshot = Some(req.data.0);
        Ok(InstallSnapshotResponse { term_out: req.term })
    }
}

impl Service for Grants {
    fn __reg_to__(&mut self, server: &mut Server, svc_index: usize) -> i32 {
        rpc::register(server, svc_index)
    }
    fn __dispatch__(&self, rpc_id: i32, req: Box<Request>, sconn: WeakServerConnection) {
        rpc::dispatch(self, rpc_id, &req, &sconn);
    }
}

/// One responder: its server (kept alive), its address, and what it saw.
type Responder = (Server, String, Arc<Mutex<Seen>>);

/// Bind a responder on an ephemeral port and hand back its address.
fn responder(granted: i8) -> Responder {
    let poll = PollThread::create();
    let mut server = Server::new(Some(poll));
    let seen = Arc::new(Mutex::new(Seen::default()));
    server.reg_service(Box::new(Grants { granted, seen: seen.clone() }));
    // SAFETY: the literal is NUL-terminated and valid for the call.
    assert_eq!(unsafe { server.start(c"127.0.0.1:0".as_ptr()) }, 0);
    let addr = format!("127.0.0.1:{}", server.get_bound_port());
    (server, addr, seen)
}

fn wait_until(deadline: Duration, mut done: impl FnMut() -> bool) -> bool {
    let start = Instant::now();
    while start.elapsed() < deadline {
        if done() {
            return true;
        }
        std::thread::sleep(Duration::from_millis(5));
    }
    done()
}

/// A transport that has recorded every site of partition 7 -- itself (site 1)
/// included, as the worker's ConnectPeers does -- so the partition's recorded
/// size is its configured size, which is what the quorum rule reads.
fn cluster(votes: [i8; 3]) -> (RaftTransport, Vec<Responder>) {
    let mut transport = RaftTransport::new();
    let mut servers = Vec::new();
    for (i, granted) in votes.iter().enumerate() {
        let (server, addr, seen) = responder(*granted);
        let c = CString::new(addr.as_str()).unwrap();
        // SAFETY: the CString outlives the call.
        assert!(unsafe { transport.add_peer_with_timeout(7, 1 + i as u16, c.as_ptr(),
                                                         Duration::from_secs(5)) },
                "could not connect to {addr}");
        servers.push((server, addr, seen));
    }
    (transport, servers)
}

#[test]
fn a_campaign_over_tcp_reaches_a_quorum() {
    let (transport, _servers) = cluster([1, 1, 1]);
    assert_eq!(transport.partition_size(7), 3);
    let req = VoteRequest { lst_log_idx: 4, lst_log_term: 2, site_id: 1, cur_term: 9 };
    let tally = transport.broadcast_vote(7, 1, &req);
    assert!(wait_until(Duration::from_secs(10), || tally.decided()),
            "no quorum inside ten seconds");
    let outcome = tally.outcome(false);
    assert!(outcome.yes_, "one granted peer vote is n/2 of three, as the C++ event counts");
    assert!(!outcome.no_);
    assert_eq!(outcome.term_, 9, "the replies' own term, the highest seen");
    assert!(!outcome.timeouted_);
}

#[test]
fn a_rejected_three_replica_campaign_is_lost_at_once() {
    // [fix, F15] no() is n_voted_no > (n - 1) - n/2 = 1: both peers'
    // refusals decide the campaign as lost, without waiting out the
    // caller's one-second deadline (bugs-found B1; before F15 the rule was
    // > n - n/2 = 2, which two peers could not reach).
    let (transport, _servers) = cluster([0, 0, 0]);
    let req = VoteRequest { lst_log_idx: 0, lst_log_term: 0, site_id: 1, cur_term: 3 };
    let tally = transport.broadcast_vote(7, 1, &req);
    assert!(wait_until(Duration::from_secs(10), || tally.decided()),
            "two refusals of three did not decide the campaign");
    let outcome = tally.outcome(false);
    assert!(!outcome.yes_ && outcome.no_);
    assert_eq!(outcome.n_voted_no_, 2, "both peers' rejections were counted");
}

#[test]
fn the_network_flag_stops_a_campaign_before_it_starts() {
    // What the lab suite's Disconnect does: no peer is reachable, so nothing
    // decides and the quorum is still the configured partition's.
    let (transport, _servers) = cluster([1, 1, 1]);
    transport.set_network_enabled(false);
    let req = VoteRequest { lst_log_idx: 0, lst_log_term: 0, site_id: 1, cur_term: 5 };
    let tally = transport.broadcast_vote(7, 1, &req);
    std::thread::sleep(Duration::from_millis(200));
    let outcome = tally.outcome(true);
    assert!(!outcome.yes_, "a partitioned candidate must not elect itself");
    assert!(outcome.timeouted_);
}

#[test]
fn append_entries_carries_its_payload_across_the_wire() {
    let (transport, servers) = cluster([1, 1, 1]);
    let cmd: Vec<u8> = vec![0x05, 0xde, 0xad, 0x00, 0xff, 0x80, 0x01];
    let req = AppendEntriesRequest {
        slot: u64::MAX,
        ballot: -1,
        leader_current_term: 4,
        leader_site_id: 1,
        leader_prev_log_index: 10,
        leader_prev_log_term: 3,
        leader_commit_index: 9,
        cmd: cmd.clone(),
        leader_next_log_term: 4,
    };
    let reply = transport.send_append_entries(2, req).expect("peer 2 is recorded");
    assert!(wait_until(Duration::from_secs(10), || reply.is_ready()));
    let resp = reply.peek().unwrap().expect("a decoded reply");
    assert_eq!(resp.follower_append_ok, 1);
    assert_eq!(resp.follower_last_log_index, 11);
    assert_eq!(servers[1].2.lock().unwrap().append_cmd.as_deref(), Some(cmd.as_slice()),
               "the follower decoded exactly the bytes sent");
    assert!(transport.rpc_count() >= 1);
}

#[test]
fn empty_append_entries_is_its_own_rpc() {
    let (transport, servers) = cluster([1, 1, 1]);
    let req = EmptyAppendEntriesRequest {
        slot: u64::MAX,
        ballot: -1,
        leader_current_term: 2,
        leader_site_id: 1,
        leader_prev_log_index: 5,
        leader_prev_log_term: 2,
        leader_commit_index: 5,
    };
    let reply = transport.send_empty_append_entries(3, &req, None).expect("peer 3 is recorded");
    assert!(wait_until(Duration::from_secs(10), || reply.is_ready()));
    assert_eq!(reply.peek().unwrap().unwrap().follower_last_log_index, 5);
    assert_eq!(servers[2].2.lock().unwrap().empty_appends, 1);
}

#[test]
fn install_snapshot_delivers_binary_data_and_calls_back_once() {
    let (transport, servers) = cluster([1, 1, 1]);
    let data = vec![0xff, 0xfe, 0x00, 0xc3, 0x28];
    let req = InstallSnapshotRequest {
        term: 6,
        leader_id: 1,
        last_included_index: 40,
        last_included_term: 5,
        data: WireBytes(data.clone()),
    };
    let calls = Arc::new(Mutex::new(Vec::new()));
    let sink = calls.clone();
    assert!(transport.send_install_snapshot_with(2, req, move |term| {
        sink.lock().unwrap().push(term);
    }));
    assert!(wait_until(Duration::from_secs(10), || !calls.lock().unwrap().is_empty()));
    std::thread::sleep(Duration::from_millis(50));
    assert_eq!(*calls.lock().unwrap(), vec![6], "exactly one callback, with the term");
    assert_eq!(servers[1].2.lock().unwrap().snapshot.as_deref(), Some(data.as_slice()));
}

/// The leader's path (N4): the image is borrowed from the store, not copied
/// into an owned request, and the follower decodes exactly those bytes.
fn send_borrowed(transport: &RaftTransport, servers: &[Responder], data: &[u8]) {
    let req = InstallSnapshotRequestBytesRef {
        term: 7,
        leader_id: 1,
        last_included_index: 90,
        last_included_term: 6,
        data,
    };
    let calls = Arc::new(Mutex::new(Vec::new()));
    let sink = calls.clone();
    assert!(transport.send_install_snapshot_ref(2, &req, move |term| {
        sink.lock().unwrap().push(term);
    }));
    assert!(wait_until(Duration::from_secs(60), || !calls.lock().unwrap().is_empty()),
            "no reply for a {}-byte image", data.len());
    std::thread::sleep(Duration::from_millis(50));
    assert_eq!(*calls.lock().unwrap(), vec![7], "exactly one callback, with the term");
    let seen = servers[1].2.lock().unwrap().snapshot.take().expect("the follower saw it");
    assert_eq!(seen.len(), data.len());
    assert!(seen == data, "byte-exact");
}

#[test]
fn a_borrowed_install_snapshot_is_byte_exact() {
    let (transport, servers) = cluster([1, 1, 1]);
    let data: Vec<u8> = (0..300_000u32).map(|i| (i.wrapping_mul(2654435761) >> 24) as u8).collect();
    send_borrowed(&transport, &servers, &data);
}

#[test]
fn the_frame_cap_admits_its_largest_image_byte_exact() {
    // The largest image the leader will send: the cap minus the overhead it
    // reserves. That it crosses intact proves the reserve is enough.
    let max = srpc::frame_codec::kMaxFramePayloadSize as usize - INSTALL_FRAME_OVERHEAD;
    assert!(install_fits_one_frame(max));
    assert!(install_fits_one_frame((64 << 20) - 1024));
    let (transport, servers) = cluster([1, 1, 1]);
    let data: Vec<u8> = (0..max as u32).map(|i| (i ^ (i >> 11)) as u8).collect();
    send_borrowed(&transport, &servers, &data);
}

#[test]
fn the_frame_cap_refuses_larger_images() {
    let cap = srpc::frame_codec::kMaxFramePayloadSize as usize;
    assert!(!install_fits_one_frame(cap - INSTALL_FRAME_OVERHEAD + 1));
    assert!(!install_fits_one_frame(cap));
    assert!(!install_fits_one_frame((64 << 20) + 1));
}

// ---------------------------------------------------------------------------
// N6: resend suppression. A follower that holds its InstallSnapshot reply
// until the test releases it, so an install is outstanding for as long as
// the test needs.
// ---------------------------------------------------------------------------

#[derive(Default)]
struct Gate {
    arrived: usize,
    open: bool,
    fail: bool,
}

struct Holder(Arc<(Mutex<Gate>, std::sync::Condvar)>);

impl RaftHandler for Holder {
    fn vote(&self, _: &VoteRequest) -> Result<VoteResponse, i32> { Ok(VoteResponse::default()) }
    fn append_entries(&self, _: &AppendEntriesRequestRef<'_>) -> Result<AppendEntriesResponse, i32> {
        Ok(AppendEntriesResponse::default())
    }
    fn empty_append_entries(&self, _: &EmptyAppendEntriesRequest)
        -> Result<EmptyAppendEntriesResponse, i32> { Ok(EmptyAppendEntriesResponse::default()) }
    fn install_snapshot(&self, req: InstallSnapshotRequest) -> Result<InstallSnapshotResponse, i32> {
        let (lock, cv) = &*self.0;
        let mut g = lock.lock().unwrap();
        g.arrived += 1;
        cv.notify_all();
        while !g.open {
            g = cv.wait(g).unwrap();
        }
        if g.fail { Err(-1) } else { Ok(InstallSnapshotResponse { term_out: req.term }) }
    }
}

impl Service for Holder {
    fn __reg_to__(&mut self, server: &mut Server, i: usize) -> i32 { rpc::register(server, i) }
    fn __dispatch__(&self, id: i32, req: Box<Request>, s: WeakServerConnection) {
        rpc::dispatch(self, id, &req, &s);
    }
}

type GateRef = Arc<(Mutex<Gate>, std::sync::Condvar)>;

/// A transport with one holding follower at site 2. The gate is opened when
/// the returned guard drops, so a failing test does not hang its server.
struct Held {
    transport: RaftTransport,
    gate: GateRef,
    _server: Server,
}

impl Held {
    fn new(fail: bool) -> Held {
        let gate: GateRef = Arc::new((Mutex::new(Gate { fail, ..Gate::default() }),
                                      std::sync::Condvar::new()));
        let mut server = Server::new(Some(PollThread::create()));
        server.reg_service(Box::new(Holder(gate.clone())));
        // SAFETY: the literal is NUL-terminated.
        assert_eq!(unsafe { server.start(c"127.0.0.1:0".as_ptr()) }, 0);
        let addr = CString::new(format!("127.0.0.1:{}", server.get_bound_port())).unwrap();
        let mut transport = RaftTransport::new();
        // SAFETY: the CString outlives the call.
        assert!(unsafe { transport.add_peer_with_timeout(7, 2, addr.as_ptr(),
                                                         Duration::from_secs(5)) });
        Held { transport, gate, _server: server }
    }

    fn open(&self) {
        let (lock, cv) = &*self.gate;
        lock.lock().unwrap().open = true;
        cv.notify_all();
    }

    fn arrived(&self) -> usize { self.gate.0.lock().unwrap().arrived }

    fn wait_arrived(&self, n: usize) {
        assert!(wait_until(Duration::from_secs(10), || self.arrived() >= n),
                "only {} of {n} installs arrived", self.arrived());
    }

    /// Send the install for `(term, index)`; the replies land in `calls`.
    fn send(&self, term: u64, index: u64, deadline: Duration,
            calls: &Arc<Mutex<Vec<u64>>>) -> InstallSend {
        let data = [5u8; 1000];
        let req = InstallSnapshotRequestBytesRef {
            term, leader_id: 1, last_included_index: index, last_included_term: term, data: &data,
        };
        let sink = calls.clone();
        self.transport.send_install_snapshot_once(2, &req, deadline, move |t| {
            sink.lock().unwrap().push(t);
        })
    }
}

impl Drop for Held {
    fn drop(&mut self) { self.open(); }
}

const LONG: Duration = Duration::from_secs(30);

#[test]
fn an_outstanding_install_is_sent_once() {
    let h = Held::new(false);
    let calls = Arc::new(Mutex::new(Vec::new()));
    assert_eq!(h.send(7, 40, LONG, &calls), InstallSend::Sent);
    h.wait_arrived(1);
    for _ in 0..20 {
        assert_eq!(h.send(7, 40, LONG, &calls), InstallSend::Suppressed);
    }
    assert_eq!(h.transport.installs_outstanding(), 1);
    h.open();
    assert!(wait_until(Duration::from_secs(10), || !calls.lock().unwrap().is_empty()));
    std::thread::sleep(Duration::from_millis(100));
    assert_eq!(h.arrived(), 1, "exactly one InstallSnapshot crossed");
    assert_eq!(*calls.lock().unwrap(), vec![7], "and exactly one reply was delivered");
    assert_eq!(h.transport.installs_outstanding(), 0, "the reply cleared the key");
    // With the reply in, the next round may resend (the core decides).
    assert_eq!(h.send(7, 40, LONG, &calls), InstallSend::Sent);
    h.wait_arrived(2);
}

#[test]
fn a_newer_snapshot_is_not_suppressed_by_an_older_one() {
    let h = Held::new(false);
    let calls = Arc::new(Mutex::new(Vec::new()));
    assert_eq!(h.send(7, 40, LONG, &calls), InstallSend::Sent);
    assert_eq!(h.send(7, 50, LONG, &calls), InstallSend::Sent, "a different index is a new key");
    h.open();
    h.wait_arrived(2);
}

#[test]
fn a_failed_reply_allows_a_resend() {
    let h = Held::new(true);
    let calls = Arc::new(Mutex::new(Vec::new()));
    assert_eq!(h.send(7, 40, LONG, &calls), InstallSend::Sent);
    h.open();
    assert!(wait_until(Duration::from_secs(10), || !calls.lock().unwrap().is_empty()));
    assert_eq!(*calls.lock().unwrap(), vec![0], "a failed reply delivers 0");
    assert_eq!(h.transport.installs_outstanding(), 0);
    assert_eq!(h.send(7, 40, LONG, &calls), InstallSend::Sent);
    h.wait_arrived(2);
}

#[test]
fn a_reply_that_never_comes_lapses_at_the_deadline() {
    let h = Held::new(false);
    let calls = Arc::new(Mutex::new(Vec::new()));
    let short = Duration::from_millis(150);
    assert_eq!(h.send(7, 40, short, &calls), InstallSend::Sent);
    h.wait_arrived(1);
    assert_eq!(h.send(7, 40, short, &calls), InstallSend::Suppressed);
    std::thread::sleep(Duration::from_millis(250));
    assert_eq!(h.send(7, 40, short, &calls), InstallSend::Sent, "resent after the deadline");
    // The first reply, arriving late, must not clear the second's entry.
    h.open();
    assert!(wait_until(Duration::from_secs(10), || calls.lock().unwrap().len() == 2));
    assert_eq!(h.arrived(), 2);
    assert_eq!(h.transport.installs_outstanding(), 0);
}

#[test]
fn a_term_change_drops_every_key() {
    let h = Held::new(false);
    let calls = Arc::new(Mutex::new(Vec::new()));
    assert_eq!(h.send(7, 40, LONG, &calls), InstallSend::Sent);
    h.wait_arrived(1);
    assert_eq!(h.send(8, 40, LONG, &calls), InstallSend::Sent, "a new term resends");
    assert_eq!(h.send(8, 40, LONG, &calls), InstallSend::Suppressed);
    h.open();
    h.wait_arrived(2);
}

#[test]
fn no_peer_is_not_sent_and_records_nothing() {
    let h = Held::new(false);
    let data = [0u8; 4];
    let req = InstallSnapshotRequestBytesRef {
        term: 1, leader_id: 1, last_included_index: 1, last_included_term: 1, data: &data,
    };
    assert_eq!(h.transport.send_install_snapshot_once(9, &req, LONG, |_| panic!("no call")),
               InstallSend::NotSent);
    assert_eq!(h.transport.installs_outstanding(), 0);
}
