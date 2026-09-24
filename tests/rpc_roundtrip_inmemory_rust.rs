// The first RPC round trip the Rust lane has ever executed: a real
// `srpc::client::Client` sends a request through the in-memory channel to a
// real `srpc::server::Server`, which dispatches it to a registered service and
// replies, and the client decodes the reply out of its `Future`.
//
// The in-memory channel delivers frames inline, and reg_fast_rpc dispatches
// synchronously. Stackful TCP dispatch is covered by rpc_runtime_rust.rs.
//
// The wire format exercised end to end is the real one:
// request `v64 xid | i32 rpc_id | args`, reply
// `v64 xid | v32 error | v64 server_instance_id | payload`
// (`rpc/internal_protocol.rs`), written and read by the same canonical Rust
// the transpiler ships as C++.

use std::ffi::CString;
use std::sync::Arc;

use srpc::client::{deserialize_from, Client, FutureAttr};
use srpc::inmemory_channel::{make_inmemory_factory_proxy, InMemoryFactory, InMemorySwitchboard};
use srpc::serializable::{BinaryReadArchive, BinaryWriteArchive, Deserialize, Serialize};
use srpc::server::{Request, Server, ServerReplyFn, Service, WeakServerConnection};

use srpc::reactor::PollThread;
use std::sync::atomic::{AtomicBool, Ordering};

const ECHO_DOUBLE_RPC_ID: i32 = 0x00E0_0042;
const TEXT_RPC_ID: i32 = 0x00E0_0043;

// A service with one fast RPC: read an i64, reply with twice its value.
struct EchoDoubleService;

impl Service for EchoDoubleService {
    fn __reg_to__(&mut self, server: &mut Server, svc_index: usize) -> i32 {
        // Fast registration is the point: fast RPCs dispatch inline on the
        // delivering thread, so no fiber runtime is needed.
        server.reg_fast_rpc(ECHO_DOUBLE_RPC_ID, svc_index)
    }

    #[allow(unsafe_code)]
    fn __dispatch__(&self, rpc_id: i32, mut req: Box<Request>, sconn: WeakServerConnection) {
        assert_eq!(rpc_id, ECHO_DOUBLE_RPC_ID);

        // The header (xid, rpc_id) has already been consumed; the source
        // cursor now sits on the argument bytes.  Same construction as
        // rpc/server.rs's own header read.
        let mut value = 0i64;
        {
            // SAFETY: `req.src` is owned by the live boxed request.
            let mut ar = BinaryReadArchive::new(unsafe {
                srpc::serializable::make_source_proxy_buffer(&raw mut req.src)
            });
            Deserialize::deserialize(&mut value, &mut ar);
        }

        let sconn = sconn.upgrade().expect("connection alive during inline dispatch");
        let writer: ServerReplyFn = Some(Box::new(move |ar: &mut BinaryWriteArchive| {
            Serialize::serialize(&(value * 2), ar);
        }));
        sconn.reply(&req, 0, writer);
    }
}

struct TextProbeService {
    invoked: Arc<AtomicBool>,
}

impl Service for TextProbeService {
    fn __reg_to__(&mut self, server: &mut Server, svc_index: usize) -> i32 {
        server.reg_fast_rpc(TEXT_RPC_ID, svc_index)
    }

    #[allow(unsafe_code)]
    fn __dispatch__(&self, rpc_id: i32, mut req: Box<Request>, sconn: WeakServerConnection) {
        assert_eq!(rpc_id, TEXT_RPC_ID);
        let mut text = String::new();
        let failed = {
            let mut ar = BinaryReadArchive::new(unsafe {
                srpc::serializable::make_source_proxy_buffer(&raw mut req.src)
            });
            Deserialize::deserialize(&mut text, &mut ar);
            ar.failed()
        };
        if failed {
            srpc::server::reject_malformed_request(&req, &sconn);
            return;
        }
        self.invoked.store(true, Ordering::Release);
        if let Some(sconn) = sconn.upgrade() {
            sconn.reply(&req, 0, None);
        }
    }
}

#[test]
#[allow(unsafe_code)]
fn client_calls_server_over_the_inmemory_channel_and_reads_the_reply() {
    // One switchboard is the "network"; both sides get a factory over it.
    let switchboard = Arc::new(InMemorySwitchboard::new());
    let addr = CString::new("inmemory://roundtrip").expect("static addr");

    // SAFETY (both create calls): the canonical poll threads retain and dispatch deferred close jobs.
    let mut server = Server::new(Some(PollThread::create()));
    server.set_channel_factory(Some(make_inmemory_factory_proxy(Arc::new(InMemoryFactory::new(
        switchboard.clone(),
    )))));
    server.reg_service(Box::new(EchoDoubleService));
    // SAFETY: `addr` is NUL-terminated and outlives the call.
    let started = unsafe { server.start(addr.as_ptr()) };
    assert_eq!(started, 0, "Server::start over the in-memory factory");

    let client = Client::create(PollThread::create());
    client.set_channel_factory(Some(make_inmemory_factory_proxy(Arc::new(InMemoryFactory::new(
        switchboard,
    )))));
    let connected = client.connect(addr.as_ptr(), true);
    assert_eq!(connected, 0, "Client::connect through the switchboard");

    let fu = client
        .request(ECHO_DOUBLE_RPC_ID, &FutureAttr::default(), |ar| {
            Serialize::serialize(&21_i64, ar);
        })
        .expect("request accepted");

    // The whole path ran inline on this thread, so the reply must already be
    // decoded; wait() is then a no-op rather than a hang.
    assert!(fu.ready(), "in-memory round trip resolves synchronously");
    fu.wait();
    assert_eq!(fu.get_error_code(), 0, "server handler replied success");

    let mut doubled = 0i64;
    deserialize_from(fu.get_reply(), &mut doubled);
    assert_eq!(doubled, 42, "the reply payload crossed both archives intact");

    // The canonical worker executes deferred close jobs. Their captured Arc
    // keeps the connection alive through the channel's on_closed callback.
    drop(server);
    drop(client);
}

#[test]
#[allow(unsafe_code)]
fn unknown_rpc_id_comes_back_as_an_error_not_a_hang() {
    let switchboard = Arc::new(InMemorySwitchboard::new());
    let addr = CString::new("inmemory://roundtrip-err").expect("static addr");

    // SAFETY: as above — owned poll-thread handles and a NUL-terminated address.
    let mut server = Server::new(Some(PollThread::create()));
    server.set_channel_factory(Some(make_inmemory_factory_proxy(Arc::new(InMemoryFactory::new(
        switchboard.clone(),
    )))));
    server.reg_service(Box::new(EchoDoubleService));
    let started = unsafe { server.start(addr.as_ptr()) };
    assert_eq!(started, 0);

    let client = Client::create(PollThread::create());
    client.set_channel_factory(Some(make_inmemory_factory_proxy(Arc::new(InMemoryFactory::new(
        switchboard,
    )))));
    assert_eq!(client.connect(addr.as_ptr(), true), 0);

    let fu = client
        .request(ECHO_DOUBLE_RPC_ID + 1, &FutureAttr::default(), |ar| {
            Serialize::serialize(&1_i64, ar);
        })
        .expect("request accepted");

    assert!(fu.ready());
    // Exactly SERVER_ERR_NO_ENTRY, not merely nonzero: a decode-order
    // regression that swapped the error-code and instance-id reads would
    // still be nonzero, but not 2.
    assert_eq!(
        fu.get_error_code(),
        srpc::server::SERVER_ERR_NO_ENTRY,
        "an unregistered rpc_id must surface as SERVER_ERR_NO_ENTRY on the future"
    );

    // Same explicit teardown order as the happy-path test above.
    drop(server);
    drop(client);
}

#[test]
#[allow(unsafe_code)]
fn invalid_utf8_request_is_rejected_before_application_code_runs() {
    let switchboard = Arc::new(InMemorySwitchboard::new());
    let addr = CString::new("inmemory://invalid-utf8").expect("static addr");
    let invoked = Arc::new(AtomicBool::new(false));

    let mut server = Server::new(Some(PollThread::create()));
    server.set_channel_factory(Some(make_inmemory_factory_proxy(Arc::new(InMemoryFactory::new(
        switchboard.clone(),
    )))));
    server.reg_service(Box::new(TextProbeService {
        invoked: invoked.clone(),
    }));
    assert_eq!(unsafe { server.start(addr.as_ptr()) }, 0);

    let client = Client::create(PollThread::create());
    client.set_channel_factory(Some(make_inmemory_factory_proxy(Arc::new(InMemoryFactory::new(
        switchboard,
    )))));
    assert_eq!(client.connect(addr.as_ptr(), true), 0);

    let invalid = [0xffu8];
    let fu = client
        .request(TEXT_RPC_ID, &FutureAttr::default(), |ar| unsafe {
            srpc::serializable::serialize_bytes(invalid.as_ptr(), invalid.len(), ar);
        })
        .expect("request accepted");

    assert!(fu.ready());
    assert_eq!(fu.get_error_code(), srpc::client::CLIENT_ERR_INVALID_ARGUMENT);
    assert!(!invoked.load(Ordering::Acquire));

    drop(server);
    drop(client);
}
