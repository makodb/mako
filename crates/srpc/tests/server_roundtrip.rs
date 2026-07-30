//! End-to-end through this crate's own server: listener, accept loop,
//! dispatch, reply — driven by this crate's own client.
//!
//! Both halves being ours is a weakness (a matched pair of bugs cancels
//! out), which is why `interop_cpp_server.rs` checks the client against
//! the real C++ peer and `reverse_interop` below checks the server the
//! same way. This file is for the paths neither of those reaches:
//! handler errors, unknown ids, and many connections at once.

use srpc::rpc::client::ClientConnection;
use srpc::rpc::server::{Registry, Server, ENOENT};
use srpc::runtime::poll_thread::PollThread;
use std::sync::Arc;
use std::time::Duration;

const ECHO: i32 = 0x1000_0001;
const FAILS: i32 = 0x1000_0002;
const CUSTOM_ERR: i32 = 4242;

/// Ports are fixed rather than ephemeral because `listen` does not
/// implement `getsockname` yet; the band is chosen to avoid the
/// harness ranges used elsewhere in the repo.
fn port_for(test: u16) -> u16 {
    19500 + test
}

fn start(port: u16) -> (Arc<PollThread>, Arc<Server>) {
    let mut reg = Registry::new();
    reg.register(ECHO, Box::new(|args| Ok(args.to_vec())));
    reg.register(FAILS, Box::new(|_| Err(CUSTOM_ERR)));
    let server = Server::new(reg, 7);
    let poll = PollThread::start();
    server
        .listen(&format!("127.0.0.1:{port}"), &poll)
        .expect("listen");
    (poll, server)
}

#[test]
fn echo_round_trips_through_our_own_server() {
    let port = port_for(1);
    let (spoll, server) = start(port);

    let cpoll = PollThread::start();
    let c = ClientConnection::connect(&format!("127.0.0.1:{port}"), &cpoll).expect("connect");
    let fu = c.call(ECHO, b"hello server").expect("call");
    let reply = fu
        .wait_timeout(Duration::from_secs(10))
        .expect("echo should succeed");
    assert_eq!(reply, b"hello server");

    c.close();
    cpoll.shutdown();
    server.close();
    spoll.shutdown();
}

#[test]
fn a_handler_error_reaches_the_caller_as_an_error() {
    let port = port_for(2);
    let (spoll, server) = start(port);

    let cpoll = PollThread::start();
    let c = ClientConnection::connect(&format!("127.0.0.1:{port}"), &cpoll).expect("connect");
    let err = c
        .call(FAILS, b"")
        .expect("call")
        .wait_timeout(Duration::from_secs(10))
        .expect_err("handler returned an error");
    assert_eq!(err, CUSTOM_ERR);

    c.close();
    cpoll.shutdown();
    server.close();
    spoll.shutdown();
}

#[test]
fn an_unknown_rpc_id_fails_rather_than_hangs() {
    // The failure mode being excluded is a DROPPED request: the caller
    // would block forever rather than get an error.
    let port = port_for(3);
    let (spoll, server) = start(port);

    let cpoll = PollThread::start();
    let c = ClientConnection::connect(&format!("127.0.0.1:{port}"), &cpoll).expect("connect");
    let err = c
        .call(0x7777_7777, b"")
        .expect("call")
        .wait_timeout(Duration::from_secs(10))
        .expect_err("an unknown id should fail");
    assert_eq!(err, ENOENT, "matches what the C++ server reports");

    c.close();
    cpoll.shutdown();
    server.close();
    spoll.shutdown();
}

#[test]
fn many_connections_are_accepted_and_served() {
    // The accept loop is edge-triggered: it must drain the backlog on
    // one readable edge, or the connections it left behind never get a
    // second one.
    let port = port_for(4);
    let (spoll, server) = start(port);

    const CLIENTS: usize = 24;
    let cpoll = PollThread::start();
    let mut clients = Vec::new();
    for _ in 0..CLIENTS {
        clients.push(
            ClientConnection::connect(&format!("127.0.0.1:{port}"), &cpoll).expect("connect"),
        );
    }
    for (i, c) in clients.iter().enumerate() {
        let payload = format!("client-{i}").into_bytes();
        let reply = c
            .call(ECHO, &payload)
            .expect("call")
            .wait_timeout(Duration::from_secs(20))
            .unwrap_or_else(|e| panic!("client {i} failed: {e}"));
        assert_eq!(reply, payload, "client {i} got another client's reply");
    }

    for c in &clients {
        c.close();
    }
    cpoll.shutdown();
    server.close();
    spoll.shutdown();
}

#[test]
fn concurrent_requests_on_one_connection_are_all_answered() {
    let port = port_for(5);
    let (spoll, server) = start(port);

    let cpoll = PollThread::start();
    let c = ClientConnection::connect(&format!("127.0.0.1:{port}"), &cpoll).expect("connect");

    const N: usize = 500;
    let mut futures = Vec::new();
    for i in 0..N {
        let payload = format!("{i}").into_bytes();
        futures.push((payload.clone(), c.call(ECHO, &payload).expect("call")));
    }
    for (expected, fu) in futures {
        let reply = fu
            .wait_timeout(Duration::from_secs(20))
            .expect("should be answered");
        assert_eq!(reply, expected, "reply demuxed to the wrong request");
    }
    assert_eq!(c.pending_count(), 0);

    c.close();
    cpoll.shutdown();
    server.close();
    spoll.shutdown();
}
