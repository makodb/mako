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
    self, AppendEntriesRequest, AppendEntriesResponse, EmptyAppendEntriesRequest,
    EmptyAppendEntriesResponse, InstallSnapshotRequest, InstallSnapshotResponse, RaftHandler,
    VoteRequest, VoteResponse, WireBytes,
};
use raft_rt::transport::RaftTransport;
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
    fn append_entries(&self, req: &AppendEntriesRequest) -> Result<AppendEntriesResponse, i32> {
        self.seen.lock().unwrap().append_cmd = Some(req.cmd.clone());
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
    fn install_snapshot(&self, req: &InstallSnapshotRequest) -> Result<InstallSnapshotResponse, i32> {
        self.seen.lock().unwrap().snapshot = Some(req.data.0.clone());
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
fn a_rejected_three_replica_campaign_waits_out_its_timeout() {
    // The C++ rule: no() is n_voted_no > n - n/2 = 2, which two peers cannot
    // reach -- so, exactly as on the C++ lane, the campaign does not lose
    // early; the caller's one-second wait times out.
    let (transport, _servers) = cluster([0, 0, 0]);
    let req = VoteRequest { lst_log_idx: 0, lst_log_term: 0, site_id: 1, cur_term: 3 };
    let tally = transport.broadcast_vote(7, 1, &req);
    std::thread::sleep(Duration::from_millis(500));
    assert!(!tally.decided());
    let outcome = tally.outcome(true);
    assert!(!outcome.yes_ && !outcome.no_);
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
    let reply = transport.send_empty_append_entries(3, &req).expect("peer 3 is recorded");
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
