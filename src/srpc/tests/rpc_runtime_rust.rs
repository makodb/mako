#![allow(unsafe_code)]

use srpc::client::{deserialize_from, Client, FutureAttr};
use srpc::reactor::PollThread;
use srpc::serializable::{BinaryReadArchive, BinaryWriteArchive, Deserialize, Serialize};
use srpc::server::{Request, Server, ServerReplyFn, Service, WeakServerConnection};
use std::ffi::CString;
use std::sync::mpsc;
use std::time::Duration;

const SLOW_RPC: i32 = 0x00E0_0051;
const FAST_RPC: i32 = 0x00E0_0052;

struct CooperativeService {
    stages: mpsc::Sender<&'static str>,
}

impl Service for CooperativeService {
    fn __reg_to__(&mut self, server: &mut Server, index: usize) -> i32 {
        let registered = server.reg_rpc(SLOW_RPC, index);
        if registered != 0 { return registered; }
        server.reg_fast_rpc(FAST_RPC, index)
    }

    fn __dispatch__(&self, rpc_id: i32, mut request: Box<Request>, connection: WeakServerConnection) {
        let mut value = 0i64;
        // SAFETY: request owns the stable input buffer for this dispatch.
        let mut archive = BinaryReadArchive::new(unsafe {
            srpc::serializable::make_source_proxy_buffer(&raw mut request.src)
        });
        Deserialize::deserialize(&mut value, &mut archive);
        if rpc_id == SLOW_RPC {
            assert!(srpc::fiber::this_fiber::in_fiber_context());
            self.stages.send("slow started").unwrap();
            srpc::fiber::this_fiber::sleep_ms(75);
            self.stages.send("slow resumed").unwrap();
        } else {
            assert_eq!(rpc_id, FAST_RPC);
            self.stages.send("fast replied").unwrap();
        }
        let connection = connection.upgrade().expect("live server connection");
        let writer: ServerReplyFn = Some(Box::new(move |archive: &mut BinaryWriteArchive| {
            Serialize::serialize(&(value + 1), archive);
        }));
        connection.reply(&request, 0, writer);
    }
}

#[test]
fn tcp_poll_thread_serves_another_request_while_a_stackful_handler_sleeps() {
    let (stages_tx, stages_rx) = mpsc::channel();
    let server_poll = PollThread::create();
    let client_poll = PollThread::create();
    // SAFETY: the server owns its poll-thread handle and the address is NUL terminated.
    let mut server = Server::new(Some(server_poll.clone()));
    server.reg_service(Box::new(CooperativeService { stages: stages_tx }));
    assert_eq!(unsafe { server.start(c"127.0.0.1:0".as_ptr()) }, 0);
    let address = CString::new(format!("127.0.0.1:{}", server.get_bound_port())).unwrap();
    let client = Client::create(client_poll.clone());
    assert_eq!(client.connect(address.as_ptr(), true), 0);
    let slow = client.request(SLOW_RPC, &FutureAttr::default(), |ar| Serialize::serialize(&40i64, ar)).unwrap();
    assert_eq!(stages_rx.recv_timeout(Duration::from_secs(2)).unwrap(), "slow started");
    let fast = client.request(FAST_RPC, &FutureAttr::default(), |ar| Serialize::serialize(&1i64, ar)).unwrap();
    fast.wait();
    assert_eq!(fast.get_error_code(), 0);
    assert_eq!(stages_rx.recv_timeout(Duration::from_secs(2)).unwrap(), "fast replied");
    slow.wait();
    assert_eq!(slow.get_error_code(), 0);
    assert_eq!(stages_rx.recv_timeout(Duration::from_secs(2)).unwrap(), "slow resumed");
    let mut reply = 0i64;
    deserialize_from(slow.get_reply(), &mut reply);
    assert_eq!(reply, 41);
    drop(client);
    client_poll.shutdown();
    drop(server);
    server_poll.shutdown();
}
