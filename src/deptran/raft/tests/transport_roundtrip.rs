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
use std::time::{Duration, Instant};

use raft::rpc::{
    self, AppendEntriesRequest, AppendEntriesResponse, EmptyAppendEntriesRequest,
    EmptyAppendEntriesResponse, InstallSnapshotRequest, InstallSnapshotResponse, RaftHandler,
    VoteRequest, VoteResponse,
};
use raft::transport::RaftTransport;
use srpc::reactor::PollThread;
use srpc::server::{Request, Server, Service, WeakServerConnection};

/// A follower that grants every vote, and answers the other three so the
/// generated dispatch has a complete handler to route to.
struct Grants {
    granted: i8,
}

impl RaftHandler for Grants {
    fn vote(&self, req: &VoteRequest) -> Result<VoteResponse, i32> {
        Ok(VoteResponse { max_ballot: req.cur_term, vote_granted: self.granted })
    }
    fn append_entries(&self, _: &AppendEntriesRequest) -> Result<AppendEntriesResponse, i32> {
        Ok(AppendEntriesResponse::default())
    }
    fn empty_append_entries(
        &self,
        _: &EmptyAppendEntriesRequest,
    ) -> Result<EmptyAppendEntriesResponse, i32> {
        Ok(EmptyAppendEntriesResponse::default())
    }
    fn install_snapshot(&self, _: &InstallSnapshotRequest) -> Result<InstallSnapshotResponse, i32> {
        Ok(InstallSnapshotResponse::default())
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

/// Bind a responder on an ephemeral port and hand back its address.
fn responder(granted: i8) -> (Server, String) {
    let poll = PollThread::create();
    let mut server = Server::new(Some(poll));
    server.reg_service(Box::new(Grants { granted }));
    // SAFETY: the literal is NUL-terminated and valid for the call.
    assert_eq!(unsafe { server.start(c"127.0.0.1:0".as_ptr()) }, 0);
    let addr = format!("127.0.0.1:{}", server.get_bound_port());
    (server, addr)
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

#[test]
fn a_campaign_over_tcp_reaches_a_quorum() {
    let (_a, addr_a) = responder(1);
    let (_b, addr_b) = responder(1);

    let mut transport = RaftTransport::new();
    for (site, addr) in [(2u16, &addr_a), (3u16, &addr_b)] {
        let c = CString::new(addr.as_str()).unwrap();
        // SAFETY: the CString outlives the call.
        assert!(unsafe { transport.add_peer(7, site, c.as_ptr()) },
                "could not connect to {addr}");
    }
    assert_eq!(transport.peer_count(), 2);

    let req = VoteRequest { lst_log_idx: 4, lst_log_term: 2, site_id: 1, cur_term: 9 };
    let tally = transport.broadcast_vote(7, 1, &req).expect("broadcast");

    assert!(wait_until(Duration::from_secs(10), || tally.decided()),
            "no quorum inside ten seconds");
    let outcome = tally.outcome(false);
    assert!(outcome.yes_, "two grants plus the self-vote is a majority of three");
    assert!(!outcome.no_);
    assert_eq!(outcome.term_, 9, "no follower reported a higher ballot");
    assert!(!outcome.timeouted_);
}

#[test]
fn a_campaign_the_followers_reject_loses() {
    let (_a, addr_a) = responder(0);
    let (_b, addr_b) = responder(0);

    let mut transport = RaftTransport::new();
    for (site, addr) in [(2u16, &addr_a), (3u16, &addr_b)] {
        let c = CString::new(addr.as_str()).unwrap();
        // SAFETY: the CString outlives the call.
        assert!(unsafe { transport.add_peer(7, site, c.as_ptr()) });
    }

    let req = VoteRequest { lst_log_idx: 0, lst_log_term: 0, site_id: 1, cur_term: 3 };
    let tally = transport.broadcast_vote(7, 1, &req).expect("broadcast");

    assert!(wait_until(Duration::from_secs(10), || tally.decided()),
            "two rejections should decide a three-replica campaign");
    let outcome = tally.outcome(false);
    assert!(outcome.no_ && !outcome.yes_);
    assert_eq!(outcome.n_voted_no_, 2);
}

#[test]
fn the_network_flag_stops_a_campaign_before_it_starts() {
    // What the lab suite's Disconnect does. No peer is reachable, so the
    // candidate is left with its own vote and nothing decides.
    let (_a, addr_a) = responder(1);
    let mut transport = RaftTransport::new();
    let c = CString::new(addr_a.as_str()).unwrap();
    // SAFETY: the CString outlives the call.
    assert!(unsafe { transport.add_peer(7, 2, c.as_ptr()) });

    transport.set_network_enabled(false);
    let req = VoteRequest { lst_log_idx: 0, lst_log_term: 0, site_id: 1, cur_term: 5 };
    let tally = transport.broadcast_vote(7, 1, &req).expect("broadcast");
    let outcome = tally.outcome(true);
    assert!(!outcome.yes_, "a majority of three cannot be one self-vote");
    assert!(outcome.timeouted_);
}
