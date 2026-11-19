#![allow(unsafe_code)]

use srpc::channel::{ChannelConnectionBase, ChannelError, ChannelFrame, OnClosedCallback,
    OnErrorCallback, OnFrameCallback};
use srpc::client::{clientconn_bind_channel_via_poll_thread, clientconn_decode_response_and_notify,
    AsyncReplyCallback, Client, ClientConnection, FutureAttr, OnServerRestartCallbackFn, CLIENT_ERR_IO};
use srpc::inmemory_channel::{make_inmemory_factory_proxy, InMemoryFactory, InMemorySwitchboard};
use srpc::misc::{Job, OneTimeJob};
use srpc::reactor::{PollThread, Reactor};
use srpc::server::{Request, Server, Service, WeakServerConnection};
use std::ffi::CString;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{mpsc, Arc, Barrier, Mutex};
use std::time::{Duration, Instant};

const RPC: i32 = 0x00e0_0090;

struct NoReplyService;

impl Service for NoReplyService {
    fn __reg_to__(&mut self, server: &mut Server, index: usize) -> i32 {
        server.reg_fast_rpc(RPC, index)
    }

    fn __dispatch__(&self, _: i32, _: Box<Request>, _: WeakServerConnection) {}
}

fn connected(name: &str) -> (Server, Arc<Client>, Arc<PollThread>) {
    let switchboard = Arc::new(InMemorySwitchboard::new());
    let poll = PollThread::create();
    let mut server = Server::new(Some(poll.clone()));
    server.set_channel_factory(Some(make_inmemory_factory_proxy(Arc::new(InMemoryFactory::new(
        switchboard.clone(),
    )))));
    server.reg_service(Box::new(NoReplyService));
    let address = CString::new(format!("inmemory://{name}")).unwrap();
    assert_eq!(unsafe { server.start(address.as_ptr()) }, 0);
    let client = Client::create(poll.clone());
    client.set_channel_factory(Some(make_inmemory_factory_proxy(Arc::new(InMemoryFactory::new(
        switchboard,
    )))));
    assert_eq!(client.connect(address.as_ptr(), true), 0);
    (server, client, poll)
}

#[test]
fn request_writer_can_close_the_same_client() {
    let (server, client, poll) = connected("writer-close-ownership");
    let callback_client = client.clone();
    let outcome = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        client.request(RPC, &FutureAttr::default(), move |_| callback_client.close())
    }));
    assert!(outcome.is_ok(), "the request writer must run after releasing the client borrow");
    let deadline = std::time::Instant::now() + Duration::from_secs(3);
    while client.metrics().in_flight_requests() != 0 && std::time::Instant::now() < deadline {
        std::thread::yield_now();
    }
    assert_eq!(client.metrics().in_flight_requests(), 0, "queued close during writing drains the pending count");
    drop(client);
    drop(server);
    poll.shutdown();
}

#[test]
fn client_metrics_report_the_active_connections_requests() {
    let (server, client, poll) = connected("live-client-metrics");
    let connection = client.connection().unwrap();
    let before = connection.metrics().requests_sent();
    let request = client.request(RPC, &FutureAttr::default(), |_| {}).unwrap();
    let actual = connection.metrics().requests_sent();
    let reported = client.metrics().requests_sent();
    connection.close();
    drop(request);
    drop(connection);
    drop(client);
    drop(server);
    poll.shutdown();
    assert_eq!(actual, before + 1, "the request must reach canonical instrumentation");
    assert_eq!(reported, actual, "Client metrics must expose its live connection");
}

#[test]
fn server_restart_callback_can_replace_itself() {
    let (server, client, poll) = connected("restart-callback-replacement");
    let connection = client.connection().unwrap();
    let weak = Arc::downgrade(&connection);
    let (sender, receiver) = mpsc::channel();
    connection.set_on_server_restart(Some(Box::new(move |old, new| {
        let connection = weak.upgrade().unwrap();
        connection.set_on_server_restart(OnServerRestartCallbackFn::default());
        sender.send((old, new)).unwrap();
    })));
    let decoding = connection.clone();
    let worker = std::thread::spawn(move || {
        // Canonical variable integers 0, 0, id encode xid, error, server id.
        // No future is needed: restart detection precedes response lookup.
        for header in [[0_u8, 0, 1], [0_u8, 0, 2]] {
            clientconn_decode_response_and_notify(&decoding, header.as_ptr(), header.len());
        }
    });
    let notification = receiver.recv_timeout(Duration::from_secs(3));
    if notification.is_ok() {
        worker.join().unwrap();
    }
    connection.close();
    drop(connection);
    drop(client);
    drop(server);
    poll.shutdown();
    assert_eq!(notification.unwrap(), (1, 2), "self-replacement must not retain the callback lock");
}

#[derive(Default)]
struct ChannelLifetime {
    closed: AtomicBool,
    backpressured: AtomicBool,
    dropped: AtomicBool,
    on_closed: Mutex<OnClosedCallback>,
    on_frame: Mutex<OnFrameCallback>,
    sent_frames: Mutex<Vec<Vec<u8>>>,
    close_during_install: AtomicBool,
    on_send: Mutex<Option<Arc<dyn Fn() + Send + Sync>>>,
}

struct TrackedChannel(Arc<ChannelLifetime>);

impl Drop for TrackedChannel {
    fn drop(&mut self) {
        self.0.dropped.store(true, Ordering::Release);
    }
}

impl ChannelConnectionBase for TrackedChannel {
    unsafe fn send_frame(&self, frame: &ChannelFrame) -> ChannelError {
        if self.0.backpressured.load(Ordering::Acquire) {
            ChannelError::WouldBlock
        } else {
            let bytes = unsafe { std::slice::from_raw_parts(frame.payload, frame.size) };
            self.0.sent_frames.lock().unwrap().push(bytes.to_vec());
            let callback = self.0.on_send.lock().unwrap().clone();
            if let Some(callback) = callback {
                callback();
            }
            ChannelError::None
        }
    }
    fn flush(&self) {}
    fn close(&self) {
        if !self.0.closed.swap(true, Ordering::AcqRel) {
            let callback = self.0.on_closed.lock().unwrap().clone();
            if callback.has_value() {
                callback.callable()(ChannelError::None);
            }
        }
    }
    fn is_closed(&self) -> bool { self.0.closed.load(Ordering::Acquire) }
    fn peer_address(&self) -> String { "tracked-channel".to_string() }
    fn set_on_frame(&mut self, callback: OnFrameCallback) {
        *self.0.on_frame.lock().unwrap() = callback;
    }
    fn set_on_closed(&mut self, callback: OnClosedCallback) {
        *self.0.on_closed.lock().unwrap() = callback;
        if self.0.close_during_install.load(Ordering::Acquire) {
            self.close();
        }
    }
    fn set_on_error(&mut self, _: OnErrorCallback) {}
}

type TrackedChannels = Arc<Mutex<Vec<Arc<ChannelLifetime>>>>;

struct RetainedChannelFactory {
    channels: TrackedChannels,
}

impl srpc::channel::ChannelFactoryBase for RetainedChannelFactory {
    fn connect(&mut self, _: &str) -> srpc::channel::ConnectResult {
        let state = Arc::new(ChannelLifetime::default());
        self.channels.lock().unwrap().push(state.clone());
        srpc::channel::ConnectResult {
            connection: Some(Box::new(TrackedChannel(state))),
            error: ChannelError::None,
        }
    }
    fn make_listener(&mut self) -> Option<srpc::channel::ChannelListenerProxy> { None }
    fn backend_name(&self) -> String { "retained-callback-test".to_string() }
}

#[test]
fn retired_direct_channel_close_callback_preserves_the_reconnected_request() {
    let poll = PollThread::create();
    let client = Client::create(poll.clone());
    let channels = Arc::new(Mutex::new(Vec::new()));
    client.set_channel_factory(Some(Box::new(RetainedChannelFactory { channels: channels.clone() })));
    client.set_reconnect_policy(&srpc::reconnect_policy::ReconnectPolicy::no_retry());
    assert_eq!(client.connect(c"tracked://retained-close".as_ptr(), true), 0);
    let connection = client.connection().unwrap();
    let old_channel = channels.lock().unwrap()[0].clone();
    let delayed_close = old_channel.on_closed.lock().unwrap().clone();
    assert!(delayed_close.has_value(), "the canonical direct binding must install its callback");

    // TcpConnection marks itself closed before delivering on_closed. Model
    // that exact pause: connection.close() sees a closed channel and retires it,
    // but the already-selected callback has not run yet. Deliver it once below.
    old_channel.closed.store(true, Ordering::Release);
    connection.close();
    assert!(!client.connected());
    assert!(old_channel.dropped.load(Ordering::Acquire));

    let (completed, completion) = mpsc::channel();
    let callback: srpc::client::OnReconnectCompleteCallbackFn = Some(Box::new(move |ok| {
        completed.send(ok).unwrap();
    }));
    assert_eq!(client.reconnect(callback), 0);
    assert!(completion.recv_timeout(Duration::from_secs(3)).unwrap());
    assert!(client.connected());
    assert_eq!(channels.lock().unwrap().len(), 2);
    let new_channel = channels.lock().unwrap()[1].clone();
    let future = client.request(RPC, &FutureAttr::default(), |_| {}).unwrap();
    assert!(!future.ready());
    assert_eq!(connection.pending_future_count(), 1);

    delayed_close.callable()(ChannelError::None);
    let connected_after = client.connected();
    let ready_after = future.ready();
    let pending_after = connection.pending_future_count();
    let error_after = if ready_after { Some(future.get_error_code()) } else { None };
    let new_channel_closed = new_channel.closed.load(Ordering::Acquire);
    connection.close();
    drop(future);
    drop(connection);
    drop(client);
    poll.shutdown();

    assert_eq!(
        (connected_after, ready_after, pending_after, error_after, new_channel_closed),
        (true, false, 1, None, false),
        "a retired channel's delayed close callback must not affect the new channel or its pending request",
    );
}

fn controlled_client() -> (Arc<Client>, Arc<PollThread>, TrackedChannels) {
    let poll = PollThread::create();
    let client = Client::create(poll.clone());
    let channels = Arc::new(Mutex::new(Vec::new()));
    client.set_channel_factory(Some(Box::new(RetainedChannelFactory { channels: channels.clone() })));
    client.set_reconnect_policy(&srpc::reconnect_policy::ReconnectPolicy::no_retry());
    assert_eq!(client.connect(c"tracked://lifecycle".as_ptr(), true), 0);
    (client, poll, channels)
}

fn reply_to_last_request(channel: &ChannelLifetime, server_id: i64) -> Vec<u8> {
    use srpc::basetypes::{v32, v64};
    use srpc::serializable::{BinaryReadArchive, BinaryWriteArchive, BufferSink, BufferSource,
        Deserialize, Serialize, make_sink_proxy_buffer, make_source_proxy_buffer};
    let request = channel.sent_frames.lock().unwrap().last().unwrap().clone();
    let mut source = BufferSource::new(request.as_ptr(), request.len());
    let mut reader = BinaryReadArchive::new(unsafe { make_source_proxy_buffer(&raw mut source) });
    let mut xid = v64::new(0);
    xid.deserialize(&mut reader);
    let mut sink = BufferSink { bytes: Vec::new() };
    let mut writer = BinaryWriteArchive { sink_: unsafe { make_sink_proxy_buffer(&raw mut sink) } };
    xid.serialize(&mut writer);
    v32::new(0).serialize(&mut writer);
    v64::new(server_id).serialize(&mut writer);
    drop(writer);
    sink.bytes
}

#[test]
fn retired_direct_frame_cannot_complete_a_new_request_or_change_server_identity() {
    let (client, poll, channels) = controlled_client();
    let connection = client.connection().unwrap();
    let old = channels.lock().unwrap()[0].clone();
    let stale_frame = old.on_frame.lock().unwrap().clone();
    connection.close();
    assert_eq!(client.reconnect(Default::default()), 0);
    let current = channels.lock().unwrap()[1].clone();
    let future = client.request(RPC, &FutureAttr::default(), |_| {}).unwrap();
    let reply = reply_to_last_request(&current, 81);
    let frame = ChannelFrame { payload: reply.as_ptr(), size: reply.len() };
    let received_before = connection.metrics().bytes_received();
    stale_frame.callable()(&frame);
    let stale_effects = (future.ready(), connection.pending_future_count(),
        connection.server_instance_id(), connection.metrics().bytes_received());
    let fresh_frame = current.on_frame.lock().unwrap().clone();
    fresh_frame.callable()(&frame);
    let accepted = future.ready();
    let current_identity = connection.server_instance_id();
    connection.close();
    drop(future);
    drop(connection);
    drop(client);
    poll.shutdown();
    assert_eq!(stale_effects, (false, 1, 0, received_before));
    assert!(accepted, "the active binding must still deliver the same valid reply");
    assert_eq!(current_identity, 81);
}

#[test]
fn close_completion_can_reconnect_without_draining_the_new_future() {
    let (client, poll, channels) = controlled_client();
    let connection = client.connection().unwrap();
    let old_future = client.request(RPC, &FutureAttr::default(), |_| {}).unwrap();
    let callback_connection = connection.clone();
    let (completed, completion) = mpsc::channel();
    connection.request_async(RPC, |_| {}, Some(Box::new(move |error, _, _| {
        assert_eq!(error, srpc::client::CLIENT_ERR_NOT_CONNECTED);
        assert_eq!(callback_connection.reconnect(Default::default()), 0);
        let new_future = srpc::client::clientconn_request_via_channel(
            &callback_connection, RPC, &FutureAttr::default(), |_| {}).unwrap();
        completed.send(new_future).unwrap();
    }))).unwrap();
    let old = channels.lock().unwrap()[0].clone();
    let closed = old.on_closed.lock().unwrap().clone();
    old.closed.store(true, Ordering::Release);
    let worker = std::thread::spawn(move || closed.callable()(ChannelError::None));
    let new_future = completion.recv_timeout(Duration::from_secs(3))
        .expect("close callbacks must run outside lifecycle and completion locks");
    worker.join().unwrap();
    let result = (connection.connected(), old_future.ready(), new_future.ready(),
        connection.pending_future_count());
    connection.close();
    drop(new_future);
    drop(old_future);
    drop(connection);
    drop(client);
    poll.shutdown();
    assert_eq!(result, (true, true, false, 1));
}

#[test]
fn admitted_frame_finishes_its_old_future_after_a_restart_callback_reconnects() {
    let (client, poll, channels) = controlled_client();
    let connection = client.connection().unwrap();
    let old = channels.lock().unwrap()[0].clone();
    let frame_callback = old.on_frame.lock().unwrap().clone();
    // Establish the first server identity before any request exists.
    let initial = [0_u8, 0, 1];
    frame_callback.callable()(&ChannelFrame { payload: initial.as_ptr(), size: initial.len() });
    let old_future = client.request(RPC, &FutureAttr::default(), |_| {}).unwrap();
    let reconnecting = connection.clone();
    let (completed, completion) = mpsc::channel();
    connection.set_on_server_restart(Some(Box::new(move |before, after| {
        assert_eq!((before, after), (1, 2));
        reconnecting.close();
        assert_eq!(reconnecting.reconnect(Default::default()), 0);
        let replacement = srpc::client::clientconn_request_via_channel(
            &reconnecting, RPC, &FutureAttr::default(), |_| {}).unwrap();
        completed.send(replacement).unwrap();
    })));
    let reply = reply_to_last_request(&old, 2);
    let worker = std::thread::spawn(move || {
        frame_callback.callable()(&ChannelFrame { payload: reply.as_ptr(), size: reply.len() });
    });
    let new_future = completion.recv_timeout(Duration::from_secs(3))
        .expect("server restart notification must run outside lifecycle");
    worker.join().unwrap();
    let old_error = if old_future.ready() { Some(old_future.get_error_code()) } else { None };
    let result = (connection.connected(), new_future.ready(), connection.pending_future_count(), old_error);
    connection.set_on_server_restart(Default::default());
    connection.close();
    drop(new_future);
    drop(old_future);
    drop(connection);
    drop(client);
    poll.shutdown();
    assert_eq!(result, (true, false, 1, Some(0)));
}

#[test]
fn writer_reconnect_cannot_publish_its_old_request_into_the_new_binding() {
    let (client, poll, _channels) = controlled_client();
    let connection = client.connection().unwrap();
    let writing = connection.clone();
    let mut replacement = None;
    let result = client.request(RPC, &FutureAttr::default(), |_| {
        writing.close();
        assert_eq!(writing.reconnect(Default::default()), 0);
        replacement = Some(srpc::client::clientconn_request_via_channel(
            &writing, RPC, &FutureAttr::default(), |_| {}).unwrap());
    });
    let result_error = result.err();
    let replacement = replacement.unwrap();
    let observed = (connection.connected(), replacement.ready(), connection.pending_future_count());
    connection.close();
    drop(replacement);
    drop(connection);
    drop(client);
    poll.shutdown();
    assert_eq!(result_error, Some(srpc::client::CLIENT_ERR_NOT_CONNECTED));
    assert_eq!(observed, (true, false, 1));
}

#[test]
fn deferred_client_close_job_cannot_close_a_replacement_binding() {
    let (client, poll, _channels) = controlled_client();
    let connection = client.connection().unwrap();
    let release = Arc::new(Barrier::new(2));
    let owner_release = release.clone();
    let (entered, entry) = mpsc::channel();
    let blocker: Arc<dyn Job> = Arc::new(OneTimeJob::new(Box::new(move || {
        entered.send(()).unwrap();
        owner_release.wait();
    })));
    poll.add(blocker);
    entry.recv_timeout(Duration::from_secs(3)).unwrap();
    client.close();
    connection.close();
    assert_eq!(connection.reconnect(Default::default()), 0);
    let future = client.request(RPC, &FutureAttr::default(), |_| {}).unwrap();
    let (finished, completion) = mpsc::channel();
    let after_close: Arc<dyn Job> = Arc::new(OneTimeJob::new(Box::new(move || {
        finished.send(()).unwrap();
    })));
    poll.add(after_close);
    release.wait();
    completion.recv_timeout(Duration::from_secs(3)).unwrap();
    let observed = (connection.connected(), future.ready(), connection.pending_future_count());
    connection.close();
    drop(future);
    drop(connection);
    drop(client);
    poll.shutdown();
    assert_eq!(observed, (true, false, 1));
}

#[test]
fn retired_replay_hands_queued_work_to_the_replacement_binding() {
    let (client, poll, channels) = controlled_client();
    let connection = client.connection().unwrap();
    connection.set_buffering_config(&srpc::client::BufferingConfig::defaults());
    connection.close();
    let old_future = client.request(RPC, &FutureAttr::default(), |_| {}).unwrap();
    let replay_connection = connection.clone();
    let replay_channels = channels.clone();
    let installed = AtomicBool::new(false);
    let (completed, completion) = mpsc::channel();
    client.add_on_connected(Box::new(move || {
        if installed.swap(true, Ordering::AcqRel) {
            return;
        }
        let channel = replay_channels.lock().unwrap().last().unwrap().clone();
        let reentering = replay_connection.clone();
        let completed = completed.clone();
        *channel.on_send.lock().unwrap() = Some(Arc::new(move || {
            reentering.close();
            let queued = srpc::client::clientconn_request_via_channel(
                &reentering, RPC, &FutureAttr::default(), |_| {}).unwrap();
            let result = reentering.reconnect(Default::default());
            completed.send((result, queued)).unwrap();
        }));
    }));
    let reconnecting = connection.clone();
    let worker = std::thread::spawn(move || reconnecting.reconnect(Default::default()));
    let (nested_result, new_future) = completion.recv_timeout(Duration::from_secs(3))
        .expect("an inline replay send must permit close, queue, and reconnect");
    let outer_result = worker.join().unwrap();
    let current = channels.lock().unwrap().last().unwrap().clone();
    let observed = (connection.connected(), connection.pending_request_count(),
        connection.pending_future_count(), current.sent_frames.lock().unwrap().len(), new_future.ready());
    connection.close();
    client.clear_connection_callbacks();
    drop(new_future);
    drop(old_future);
    drop(connection);
    drop(client);
    poll.shutdown();
    assert_eq!(nested_result, 0);
    assert_eq!(outer_result, srpc::client::CLIENT_ERR_CANCELED);
    assert_eq!(observed, (true, 0, 1, 1, false));
}

#[test]
fn repeated_client_close_keeps_its_queued_retirement_valid() {
    let (client, poll, channels) = controlled_client();
    let connection = client.connection().unwrap();
    let old = channels.lock().unwrap()[0].clone();
    let release = Arc::new(Barrier::new(2));
    let owner_release = release.clone();
    let (entered, entry) = mpsc::channel();
    let blocker: Arc<dyn Job> = Arc::new(OneTimeJob::new(Box::new(move || {
        entered.send(()).unwrap();
        owner_release.wait();
    })));
    poll.add(blocker);
    entry.recv_timeout(Duration::from_secs(3)).unwrap();
    client.close();
    client.close();
    let (finished, completion) = mpsc::channel();
    let after_close: Arc<dyn Job> = Arc::new(OneTimeJob::new(Box::new(move || {
        finished.send(()).unwrap();
    })));
    poll.add(after_close);
    release.wait();
    completion.recv_timeout(Duration::from_secs(3)).unwrap();
    let state = connection.connection_state();
    let closed = old.closed.load(Ordering::Acquire);
    connection.close();
    drop(connection);
    drop(client);
    poll.shutdown();
    assert_eq!(state, srpc::connection_state::ConnectionState::DISCONNECTED);
    assert!(closed);
}

struct PausedReconnectFactory {
    calls: usize,
    entered: mpsc::Sender<()>,
    release: Arc<Barrier>,
    channels: TrackedChannels,
}

impl srpc::channel::ChannelFactoryBase for PausedReconnectFactory {
    fn connect(&mut self, _: &str) -> srpc::channel::ConnectResult {
        self.calls += 1;
        let channel = Arc::new(ChannelLifetime::default());
        self.channels.lock().unwrap().push(channel.clone());
        if self.calls == 2 {
            self.entered.send(()).unwrap();
            self.release.wait();
        }
        srpc::channel::ConnectResult {
            connection: Some(Box::new(TrackedChannel(channel))),
            error: ChannelError::None,
        }
    }
    fn make_listener(&mut self) -> Option<srpc::channel::ChannelListenerProxy> { None }
    fn backend_name(&self) -> String { "paused-connect-test".to_string() }
}

#[test]
fn close_cancels_a_new_factory_attempt_after_a_previous_close() {
    canceled_factory_attempt(false);
}

#[test]
fn public_client_close_cancels_and_retires_an_inflight_factory_attempt() {
    canceled_factory_attempt(true);
}

fn canceled_factory_attempt(public_close: bool) {
    let poll = PollThread::create();
    let client = Client::create(poll.clone());
    let channels = Arc::new(Mutex::new(Vec::new()));
    let (entered, entry) = mpsc::channel();
    let release = Arc::new(Barrier::new(2));
    client.set_channel_factory(Some(Box::new(PausedReconnectFactory {
        calls: 0, entered, release: release.clone(), channels: channels.clone(),
    })));
    client.set_reconnect_policy(&srpc::reconnect_policy::ReconnectPolicy::no_retry());
    assert_eq!(client.connect(c"tracked://blocked-connect".as_ptr(), true), 0);
    let connection = client.connection().unwrap();
    connection.close();
    let reconnecting = connection.clone();
    let worker = std::thread::spawn(move || reconnecting.reconnect(Default::default()));
    entry.recv_timeout(Duration::from_secs(3)).expect("second factory attempt must start");
    if public_close {
        client.close();
        let (finished, completion) = mpsc::channel();
        let after_close: Arc<dyn Job> = Arc::new(OneTimeJob::new(Box::new(move || {
            finished.send(()).unwrap();
        })));
        poll.add(after_close);
        completion.recv_timeout(Duration::from_secs(3)).unwrap();
    } else {
        connection.close();
    }
    release.wait();
    let result = worker.join().unwrap();
    let observed = (connection.connected(), channels.lock().unwrap()[1].dropped.load(Ordering::Acquire));
    assert_eq!(connection.reconnect(Default::default()), 0, "cancellation must release reconnect ownership");
    connection.close();
    drop(connection);
    drop(client);
    poll.shutdown();
    assert_eq!(result, srpc::client::CLIENT_ERR_CANCELED);
    assert_eq!(observed, (false, true));
}

struct ClosingDuringInstallFactory(Arc<ChannelLifetime>);

impl srpc::channel::ChannelFactoryBase for ClosingDuringInstallFactory {
    fn connect(&mut self, _: &str) -> srpc::channel::ConnectResult {
        srpc::channel::ConnectResult {
            connection: Some(Box::new(TrackedChannel(self.0.clone()))),
            error: ChannelError::None,
        }
    }
    fn make_listener(&mut self) -> Option<srpc::channel::ChannelListenerProxy> { None }
    fn backend_name(&self) -> String { "close-during-install-test".to_string() }
}

#[test]
fn channel_close_during_callback_installation_cancels_publication() {
    let poll = PollThread::create();
    let client = Client::create(poll.clone());
    let channel = Arc::new(ChannelLifetime::default());
    channel.close_during_install.store(true, Ordering::Release);
    client.set_channel_factory(Some(Box::new(ClosingDuringInstallFactory(channel.clone()))));
    client.set_reconnect_policy(&srpc::reconnect_policy::ReconnectPolicy::no_retry());
    let result = client.connect(c"tracked://install-close".as_ptr(), true);
    let connected = client.connected();
    let dropped = channel.dropped.load(Ordering::Acquire);
    if let Some(connection) = client.connection() {
        connection.close();
    }
    drop(client);
    poll.shutdown();
    assert_eq!(result, srpc::client::CLIENT_ERR_CANCELED);
    assert!(!connected);
    assert!(dropped);
}

#[test]
fn reconnecting_callback_reports_busy_for_a_reentrant_attempt() {
    let (client, poll, _channels) = controlled_client();
    let connection = client.connection().unwrap();
    let nested = connection.clone();
    let (completed, completion) = mpsc::channel();
    client.add_on_reconnecting(Box::new(move || {
        completed.send(nested.reconnect(Default::default())).unwrap();
    }));
    connection.close();
    let reconnecting = connection.clone();
    let worker = std::thread::spawn(move || reconnecting.reconnect(Default::default()));
    let nested_result = completion.recv_timeout(Duration::from_secs(3))
        .expect("a reentrant reconnect must return instead of waiting for its own callback");
    let outer_result = worker.join().unwrap();
    connection.close();
    client.clear_connection_callbacks();
    drop(connection);
    drop(client);
    poll.shutdown();
    assert_eq!(nested_result, srpc::client::CLIENT_ERR_BUSY);
    assert_eq!(outer_result, 0);
}

#[test]
fn connected_callback_can_replace_the_completed_reconnect_binding() {
    let (client, poll, _channels) = controlled_client();
    let connection = client.connection().unwrap();
    let nested = connection.clone();
    let entered = AtomicBool::new(false);
    let (completed, completion) = mpsc::channel();
    client.add_on_connected(Box::new(move || {
        if entered.swap(true, Ordering::AcqRel) {
            return;
        }
        nested.close();
        let result = nested.reconnect(Default::default());
        let future = srpc::client::clientconn_request_via_channel(
            &nested, RPC, &FutureAttr::default(), |_| {}).ok();
        completed.send((result, future)).unwrap();
    }));
    connection.close();
    let reconnecting = connection.clone();
    let worker = std::thread::spawn(move || reconnecting.reconnect(Default::default()));
    let (nested_result, replacement) = completion.recv_timeout(Duration::from_secs(3))
        .expect("on_connected must run after releasing reconnect ownership");
    let outer_result = worker.join().unwrap();
    let replacement = replacement.unwrap();
    let observed = (connection.connected(), replacement.ready(), connection.pending_future_count());
    connection.close();
    client.clear_connection_callbacks();
    drop(replacement);
    drop(connection);
    drop(client);
    poll.shutdown();
    assert_eq!(nested_result, 0);
    assert_eq!(outer_result, srpc::client::CLIENT_ERR_CANCELED);
    assert_eq!(observed, (true, false, 1));
}

struct RefusingReconnectFactory(usize);

impl srpc::channel::ChannelFactoryBase for RefusingReconnectFactory {
    fn connect(&mut self, _: &str) -> srpc::channel::ConnectResult {
        self.0 += 1;
        if self.0 == 1 {
            srpc::channel::ConnectResult {
                connection: Some(Box::new(TrackedChannel(Arc::new(ChannelLifetime::default())))),
                error: ChannelError::None,
            }
        } else {
            srpc::channel::ConnectResult { connection: None, error: ChannelError::ConnectionRefused }
        }
    }
    fn make_listener(&mut self) -> Option<srpc::channel::ChannelListenerProxy> { None }
    fn backend_name(&self) -> String { "refusing-reconnect-test".to_string() }
}

#[test]
fn factory_error_callback_reports_busy_for_a_reentrant_reconnect() {
    let poll = PollThread::create();
    let client = Client::create(poll.clone());
    client.set_channel_factory(Some(Box::new(RefusingReconnectFactory(0))));
    client.set_reconnect_policy(&srpc::reconnect_policy::ReconnectPolicy::no_retry());
    assert_eq!(client.connect(c"tracked://factory-error".as_ptr(), true), 0);
    let connection = client.connection().unwrap();
    let nested = connection.clone();
    let (completed, completion) = mpsc::channel();
    client.add_on_error(Box::new(move |_, _| {
        completed.send(nested.reconnect(Default::default())).unwrap();
    }));
    connection.close();
    let reconnecting = connection.clone();
    let worker = std::thread::spawn(move || reconnecting.reconnect(Default::default()));
    let nested_result = completion.recv_timeout(Duration::from_secs(3))
        .expect("factory error notifications must not wait on the notifying attempt");
    let outer_result = worker.join().unwrap();
    connection.close();
    client.clear_connection_callbacks();
    drop(connection);
    drop(client);
    poll.shutdown();
    assert_eq!(nested_result, srpc::client::CLIENT_ERR_BUSY);
    assert_eq!(outer_result, srpc::client::CLIENT_ERR_CONNECTION_REFUSED);
}

struct ReenterRestartOnDrop {
    connection: std::sync::Weak<ClientConnection>,
    completed: mpsc::Sender<()>,
}

impl Drop for ReenterRestartOnDrop {
    fn drop(&mut self) {
        self.connection.upgrade().unwrap().set_on_server_restart(Default::default());
        self.completed.send(()).unwrap();
    }
}

#[test]
fn retired_restart_callback_capture_can_replace_the_callback_when_dropped() {
    let (client, poll, _channels) = controlled_client();
    let connection = client.connection().unwrap();
    let (completed, completion) = mpsc::channel();
    let capture = ReenterRestartOnDrop { connection: Arc::downgrade(&connection), completed };
    connection.set_on_server_restart(Some(Box::new(move |_, _| {
        let _capture = &capture;
    })));
    let replacing = connection.clone();
    let worker = std::thread::spawn(move || replacing.set_on_server_restart(Default::default()));
    completion.recv_timeout(Duration::from_secs(3))
        .expect("retired callback captures must be dropped outside the callback slot lock");
    worker.join().unwrap();
    connection.close();
    drop(connection);
    drop(client);
    poll.shutdown();
}

fn wait_for_parked_receiver(poll: &PollThread) {
    let deadline = Instant::now() + Duration::from_secs(3);
    loop {
        let (sender, receiver) = mpsc::channel();
        let probe: Arc<dyn Job> = Arc::new(OneTimeJob::new(Box::new(move || {
            sender.send(!Reactor::get_reactor().all_events_.borrow().is_empty()).unwrap();
        })));
        poll.add(probe);
        if receiver.recv_timeout(Duration::from_secs(3)).unwrap() {
            return;
        }
        assert!(Instant::now() < deadline, "receive fiber did not park on its owner thread");
    }
}

#[test]
fn replacing_fiber_channel_keeps_a_parked_receiver_alive_until_it_exits() {
    let (server, client, poll) = connected("fiber-replacement-ownership");
    let connection = client.connection().unwrap();
    let original = Arc::new(ChannelLifetime::default());
    clientconn_bind_channel_via_poll_thread(&connection, Some(Box::new(TrackedChannel(original.clone()))));
    wait_for_parked_receiver(&poll);

    // Hold the owner thread after its receive fiber has suspended. Replacing
    // the slot cannot destroy that fiber's channel before it resumes and exits.
    let release = Arc::new(Barrier::new(2));
    let owner_release = release.clone();
    let (entered_sender, entered_receiver) = mpsc::channel();
    let blocker: Arc<dyn Job> = Arc::new(OneTimeJob::new(Box::new(move || {
        entered_sender.send(()).unwrap();
        owner_release.wait();
    })));
    poll.add(blocker);
    entered_receiver.recv_timeout(Duration::from_secs(3)).unwrap();

    let replacement = Arc::new(ChannelLifetime::default());
    clientconn_bind_channel_via_poll_thread(&connection, Some(Box::new(TrackedChannel(replacement))));
    let dropped_before_receiver_exit = original.dropped.load(Ordering::Acquire);
    release.wait();

    let deadline = Instant::now() + Duration::from_secs(3);
    while !original.dropped.load(Ordering::Acquire) && Instant::now() < deadline {
        std::thread::yield_now();
    }
    let dropped_after_receiver_exit = original.dropped.load(Ordering::Acquire);
    connection.close();
    drop(connection);
    drop(client);
    drop(server);
    poll.shutdown();

    assert!(!dropped_before_receiver_exit, "replacement freed a suspended receiver's channel");
    assert!(dropped_after_receiver_exit, "the retired channel should close and release its receiver");
}


struct BackpressureFactory;
impl srpc::channel::ChannelFactoryBase for BackpressureFactory {
    fn connect(&mut self, _: &str) -> srpc::channel::ConnectResult {
        let state = Arc::new(ChannelLifetime::default());
        state.backpressured.store(true, Ordering::Release);
        srpc::channel::ConnectResult {
            connection: Some(Box::new(TrackedChannel(state))),
            error: ChannelError::None,
        }
    }
    fn make_listener(&mut self) -> Option<srpc::channel::ChannelListenerProxy> { None }
    fn backend_name(&self) -> String { "backpressure-test".to_string() }
}

struct ReenterOnDrop {
    connection: std::sync::Weak<ClientConnection>,
    finished: mpsc::Sender<()>,
}
impl Drop for ReenterOnDrop {
    fn drop(&mut self) {
        let connection = self.connection.upgrade().unwrap();
        assert_eq!(connection.request_async(RPC, |_| {}, AsyncReplyCallback::default()), Err(CLIENT_ERR_IO));
        assert_eq!(connection.metrics().in_flight_requests(), 0);
        self.finished.send(()).unwrap();
    }
}

#[test]
fn rejected_async_callback_capture_can_reenter_when_dropped() {
    let poll = PollThread::create();
    let client = Client::create(poll.clone());
    client.set_channel_factory(Some(Box::new(BackpressureFactory)));
    assert_eq!(client.connect(c"backpressure://capture-drop".as_ptr(), true), 0);
    let connection = client.connection().unwrap();
    let (finished, observed) = mpsc::channel();
    let capture = ReenterOnDrop { connection: Arc::downgrade(&connection), finished };
    let worker = std::thread::spawn(move || {
        let callback: AsyncReplyCallback = Some(Box::new(move |_, _, _| {
            let _owned_capture = &capture;
            panic!("rejected sends do not invoke the reply callback");
        }));
        assert_eq!(connection.request_async(RPC, |_| {}, callback), Err(CLIENT_ERR_IO));
    });
    observed.recv_timeout(Duration::from_secs(3))
        .expect("a callback destructor must run after releasing the callback-table mutex");
    worker.join().unwrap();
    client.connection().unwrap().close();
    drop(client);
    poll.shutdown();
}
