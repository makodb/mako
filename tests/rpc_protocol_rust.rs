//! Exercise canonical request/reply dispatch at the channel boundary.
#![allow(unsafe_code)]

use srpc::basetypes::{v32, v64};
use srpc::channel::{
    ChannelConnectionBase, ChannelError, ChannelFrame, OnClosedCallback, OnErrorCallback,
    OnFrameCallback,
};
use srpc::client::{
    clientconn_decode_response_and_notify, deserialize_from, reply_buffer_empty, reply_buffer_fill,
    Client, FutureAttr,
};
use srpc::inmemory_channel::{make_inmemory_factory_proxy, InMemoryFactory, InMemorySwitchboard};
use srpc::reactor::PollThread;
use srpc::serializable::{make_sink_proxy_buffer, BinaryWriteArchive, BufferSink, Serialize};
use srpc::server::{
    sconn_decode_request_and_dispatch, Request, RpcServiceContext, Server, ServerConnection,
    Service, WeakServerConnection,
};
use std::sync::atomic::{AtomicBool, AtomicI32};
use std::sync::{Arc, Mutex};

fn encode(write: impl FnOnce(&mut BinaryWriteArchive)) -> Vec<u8> {
    let mut sink = BufferSink { bytes: Vec::new() };
    // The archive is dropped before its borrowed sink.
    let mut archive = BinaryWriteArchive {
        sink_: unsafe { make_sink_proxy_buffer(&raw mut sink) },
    };
    write(&mut archive);
    drop(archive);
    sink.bytes
}

fn reply_header(bytes: &[u8]) -> (i64, i32, i64) {
    let mut buffer = reply_buffer_empty();
    reply_buffer_fill(&mut buffer, bytes);
    let reply = Mutex::new(buffer);
    let mut xid = v64::new(0);
    let mut error = v32::new(0);
    let mut server = v64::new(0);
    deserialize_from(reply.lock().unwrap(), &mut xid);
    deserialize_from(reply.lock().unwrap(), &mut error);
    deserialize_from(reply.lock().unwrap(), &mut server);
    (xid.get(), error.get(), server.get())
}

struct Capture(Arc<Mutex<Vec<Vec<u8>>>>);
impl ChannelConnectionBase for Capture {
    unsafe fn send_frame(&self, frame: &ChannelFrame) -> ChannelError {
        // The caller promises a readable payload for this synchronous send.
        self.0
            .lock()
            .unwrap()
            .push(unsafe { std::slice::from_raw_parts(frame.payload, frame.size).to_vec() });
        ChannelError::None
    }
    fn flush(&self) {}
    fn close(&self) {}
    fn is_closed(&self) -> bool {
        false
    }
    fn peer_address(&self) -> String {
        "capture".into()
    }
    fn set_on_frame(&mut self, _: OnFrameCallback) {}
    fn set_on_closed(&mut self, _: OnClosedCallback) {}
    fn set_on_error(&mut self, _: OnErrorCallback) {}
}

#[test]
fn server_dispatch_replies_to_unknown_truncated_and_heartbeat_frames() {
    let frames = Arc::new(Mutex::new(Vec::new()));
    let pending = Arc::new(AtomicI32::new(0));
    let drop_heartbeat = Arc::new(AtomicBool::new(false));
    let context = Arc::new(RpcServiceContext::new(
        Default::default(),
        Default::default(),
        Vec::new(),
        "protocol".into(),
        pending.clone(),
        drop_heartbeat.clone(),
        123,
    ));
    let mut connection = ServerConnection::new(context, -1);
    connection.bind_channel(Some(Box::new(Capture(frames.clone()))));
    let connection = Arc::new(connection);
    for (xid, rpc, expected_error) in [
        (1, Some(0x00e0_7111), 2),
        (2, None, 22),
        (3, Some(srpc::internal_protocol::kInternalHeartbeatRpcId), 0),
    ] {
        let bytes = encode(|ar| {
            v64::new(xid).serialize(ar);
            if let Some(rpc) = rpc {
                rpc.serialize(ar);
            }
        });
        // The owned byte buffer remains live throughout synchronous dispatch.
        unsafe { sconn_decode_request_and_dispatch(&connection, bytes.as_ptr(), bytes.len()) };
        assert_eq!(
            reply_header(frames.lock().unwrap().last().unwrap()),
            (xid, expected_error, 123)
        );
        assert_eq!(pending.load(std::sync::atomic::Ordering::Relaxed), 0);
    }
    drop_heartbeat.store(true, std::sync::atomic::Ordering::Release);
    let bytes = encode(|ar| {
        v64::new(4).serialize(ar);
        srpc::internal_protocol::kInternalHeartbeatRpcId.serialize(ar);
    });
    unsafe { sconn_decode_request_and_dispatch(&connection, bytes.as_ptr(), bytes.len()) };
    assert_eq!(frames.lock().unwrap().len(), 3);
}

struct Hold(Arc<Mutex<Vec<i64>>>);
impl Service for Hold {
    fn __reg_to__(&mut self, server: &mut Server, index: usize) -> i32 {
        server.reg_fast_rpc(0x00e0_7112, index)
    }
    fn __dispatch__(&self, _: i32, request: Box<Request>, _: WeakServerConnection) {
        self.0.lock().unwrap().push(request.xid);
    }
}

#[test]
fn client_demux_handles_out_of_order_unknown_duplicate_and_error_replies() {
    let board = Arc::new(InMemorySwitchboard::new());
    let poll = PollThread::create();
    let xids = Arc::new(Mutex::new(Vec::new()));
    let mut server = Server::new(Some(poll.clone()));
    server.set_channel_factory(Some(make_inmemory_factory_proxy(Arc::new(
        InMemoryFactory::new(board.clone()),
    ))));
    server.reg_service(Box::new(Hold(xids.clone())));
    assert_eq!(
        unsafe { server.start(c"inmemory://protocol-demux".as_ptr()) },
        0
    );
    let client = Client::create(poll.clone());
    client.set_channel_factory(Some(make_inmemory_factory_proxy(Arc::new(
        InMemoryFactory::new(board),
    ))));
    assert_eq!(
        client.connect(c"inmemory://protocol-demux".as_ptr(), true),
        0
    );
    let first = client
        .request(0x00e0_7112, &FutureAttr::default(), |_| {})
        .unwrap();
    let second = client
        .request(0x00e0_7112, &FutureAttr::default(), |_| {})
        .unwrap();
    let xids = xids.lock().unwrap().clone();
    let connection = client.connection().unwrap();
    let restarts = Arc::new(Mutex::new(Vec::new()));
    let events = restarts.clone();
    connection.set_on_server_restart(Some(Box::new(move |old, new| {
        events.lock().unwrap().push((old, new))
    })));
    let deliver = |xid, error, server_id| {
        let bytes = encode(|ar| {
            v64::new(xid).serialize(ar);
            v32::new(error).serialize(ar);
            v64::new(server_id).serialize(ar);
            91i64.serialize(ar);
        });
        clientconn_decode_response_and_notify(&connection, bytes.as_ptr(), bytes.len());
    };
    deliver(xids[1], 5, 100);
    assert!(second.ready());
    assert_eq!(second.get_error_code(), 5);
    assert!(!first.ready());
    assert!(restarts.lock().unwrap().is_empty());
    deliver(xids[1] + 100, 0, 100);
    assert!(!first.ready());
    deliver(xids[1], 0, 100);
    assert_eq!(second.get_error_code(), 5);
    deliver(xids[0], 0, 200);
    assert!(first.ready());
    assert_eq!(first.get_error_code(), 0);
    let mut value = 0i64;
    srpc::client::deserialize_from(first.get_reply(), &mut value);
    assert_eq!(value, 91);
    assert_eq!(*restarts.lock().unwrap(), [(100, 200)]);
    assert_eq!(client.server_instance_id(), 200);
    assert_eq!(client.metrics().in_flight_requests(), 0);
    drop(connection);
    drop(client);
    drop(server);
    poll.shutdown();
}
