//! Disk builds (plan P5, bugs-found B22): a node starts while a peer is down.
//! add_peer leaves an unreachable site to a dial thread, a majority suffices
//! to start, and the site's slot fills once the peer comes up.
#![cfg(feature = "raft_disk")]
#![allow(unsafe_code)]

use std::ffi::CString;
use std::time::{Duration, Instant};

use raft_rt::transport::RaftTransport;
use srpc::reactor::PollThread;
use srpc::server::Server;

fn listening(addr: &str) -> Server {
    let mut server = Server::new(Some(PollThread::create()));
    let c = CString::new(addr).unwrap();
    // SAFETY: the CString outlives the call.
    assert_eq!(unsafe { server.start(c.as_ptr()) }, 0);
    server
}

/// A port nothing listens on (bound, read, released).
fn free_port() -> u16 {
    std::net::TcpListener::bind("127.0.0.1:0").unwrap().local_addr().unwrap().port()
}

#[test]
fn a_down_peer_is_dialled_until_it_comes_up() {
    let up = listening("127.0.0.1:0");
    let up_addr = CString::new(format!("127.0.0.1:{}", up.get_bound_port())).unwrap();
    let down_port = free_port();
    let down_addr = CString::new(format!("127.0.0.1:{down_port}")).unwrap();

    let mut t = RaftTransport::new();
    // SAFETY: the CStrings outlive the calls.
    unsafe {
        assert!(t.add_peer(1, 1, up_addr.as_ptr()));
        assert!(t.add_peer(1, 2, up_addr.as_ptr()));
        let started = Instant::now();
        assert!(t.add_peer(1, 3, down_addr.as_ptr()), "a down site is deferred, not refused");
        assert!(started.elapsed() < Duration::from_secs(5), "the first dial is bounded");
    }
    assert!(t.peer(3).is_none(), "a send to the down site fails at once");
    assert!(t.majority_connected(), "two of three is a majority");
    assert!(t.wait_majority(Duration::from_secs(1)));

    let _late = listening(&format!("127.0.0.1:{down_port}"));
    let deadline = Instant::now() + Duration::from_secs(10);
    while t.peer(3).is_none() {
        assert!(Instant::now() < deadline, "the dial thread never filled the slot");
        std::thread::sleep(Duration::from_millis(50));
    }
}

#[test]
fn a_minority_does_not_start() {
    let up = listening("127.0.0.1:0");
    let up_addr = CString::new(format!("127.0.0.1:{}", up.get_bound_port())).unwrap();
    let a = CString::new(format!("127.0.0.1:{}", free_port())).unwrap();
    let b = CString::new(format!("127.0.0.1:{}", free_port())).unwrap();
    let mut t = RaftTransport::new();
    // SAFETY: the CStrings outlive the calls.
    unsafe {
        assert!(t.add_peer(1, 1, up_addr.as_ptr()));
        assert!(t.add_peer(1, 2, a.as_ptr()));
        assert!(t.add_peer(1, 3, b.as_ptr()));
    }
    assert!(!t.majority_connected());
    assert!(!t.wait_majority(Duration::from_millis(300)));
}
