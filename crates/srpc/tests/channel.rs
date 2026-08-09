use srpc::base::callback_wrapper::detail::CallbackWrapper;
use srpc::rpc::channel::{
    channel_error_to_string, ChannelConnectionBase, ChannelConnectionProxy, ChannelError,
    ChannelFactoryBase, ChannelFactoryProxy, ChannelFrame, ChannelListenerBase,
    ChannelListenerProxy, ConnectResult, OnAcceptCallback, OnClosedCallback, OnErrorCallback,
    OnFrameCallback,
};
use std::mem::{align_of, offset_of, size_of};

struct RecordingConnection {
    closed: bool,
    sent_size: usize,
    peer: String,
    on_frame: OnFrameCallback,
    on_closed: OnClosedCallback,
    on_error: OnErrorCallback,
}

impl RecordingConnection {
    fn new(peer: &str) -> RecordingConnection {
        RecordingConnection {
            closed: false,
            sent_size: 0,
            peer: peer.to_owned(),
            on_frame: CallbackWrapper::default(),
            on_closed: CallbackWrapper::default(),
            on_error: CallbackWrapper::default(),
        }
    }
}

impl ChannelConnectionBase for RecordingConnection {
    fn send_frame(&mut self, frame: &ChannelFrame) -> ChannelError {
        if self.closed {
            return ChannelError::ConnectionReset;
        }
        self.sent_size = frame.size;
        ChannelError::None
    }

    fn flush(&mut self) {}

    fn close(&mut self) {
        self.closed = true;
    }

    fn is_closed(&self) -> bool {
        self.closed
    }

    fn peer_address(&self) -> String {
        self.peer.clone()
    }

    fn set_on_frame(&mut self, callback: OnFrameCallback) {
        self.on_frame = callback;
    }

    fn set_on_closed(&mut self, callback: OnClosedCallback) {
        self.on_closed = callback;
    }

    fn set_on_error(&mut self, callback: OnErrorCallback) {
        self.on_error = callback;
    }
}

struct RecordingListener {
    closed: bool,
    address: String,
    on_accept: OnAcceptCallback,
    on_error: OnErrorCallback,
}

impl RecordingListener {
    fn new() -> RecordingListener {
        RecordingListener {
            closed: true,
            address: String::new(),
            on_accept: CallbackWrapper::default(),
            on_error: CallbackWrapper::default(),
        }
    }
}

impl ChannelListenerBase for RecordingListener {
    fn listen(&mut self, address: &str) -> ChannelError {
        self.address = address.to_owned();
        self.closed = false;
        ChannelError::None
    }

    fn close(&mut self) {
        self.closed = true;
    }

    fn is_closed(&self) -> bool {
        self.closed
    }

    fn local_address(&self) -> String {
        self.address.clone()
    }

    fn set_on_accept(&mut self, callback: OnAcceptCallback) {
        self.on_accept = callback;
    }

    fn set_on_error(&mut self, callback: OnErrorCallback) {
        self.on_error = callback;
    }
}

struct RecordingFactory;

impl ChannelFactoryBase for RecordingFactory {
    fn connect(&mut self, address: &str) -> ConnectResult {
        if address.is_empty() {
            return ConnectResult {
                connection: None,
                error: ChannelError::AddressInvalid,
            };
        }
        ConnectResult {
            connection: Some(Box::new(RecordingConnection::new(address))),
            error: ChannelError::None,
        }
    }

    fn make_listener(&mut self) -> Option<ChannelListenerProxy> {
        Some(Box::new(RecordingListener::new()))
    }

    fn backend_name(&self) -> String {
        "recording".to_owned()
    }
}

#[test]
fn error_values_and_names_are_stable() {
    let values = [
        (ChannelError::None, 0, "None"),
        (ChannelError::WouldBlock, 1, "WouldBlock"),
        (ChannelError::ConnectionRefused, 2, "ConnectionRefused"),
        (ChannelError::ConnectionReset, 3, "ConnectionReset"),
        (ChannelError::Timeout, 4, "Timeout"),
        (ChannelError::AddressInUse, 5, "AddressInUse"),
        (ChannelError::AddressInvalid, 6, "AddressInvalid"),
        (ChannelError::PermissionDenied, 7, "PermissionDenied"),
        (ChannelError::TooManyOpenFiles, 8, "TooManyOpenFiles"),
        (ChannelError::Internal, 9, "Internal"),
    ];
    for (error, value, name) in values {
        assert_eq!(error as i32, value);
        assert_eq!(channel_error_to_string(error), name);
        assert_eq!(channel_error_to_string(error), name);
    }
    assert_eq!(channel_error_to_string(ChannelError::None), "None");
    assert_eq!(
        channel_error_to_string(ChannelError::WouldBlock),
        "WouldBlock"
    );
}

#[test]
fn value_layouts_match_the_channel_abi() {
    assert_eq!(size_of::<ChannelError>(), 4);
    assert_eq!(align_of::<ChannelError>(), 4);
    assert_eq!(offset_of!(ChannelFrame, payload), 0);
    assert_eq!(offset_of!(ChannelFrame, size), 8);
    assert_eq!(size_of::<ChannelFrame>(), 16);
    assert_eq!(align_of::<ChannelFrame>(), 8);
    assert_eq!(offset_of!(ConnectResult, connection), 0);
    assert_eq!(offset_of!(ConnectResult, error), 16);
    assert_eq!(size_of::<ConnectResult>(), 24);
    assert_eq!(align_of::<ConnectResult>(), 8);
}

#[test]
fn connection_proxy_forwards_every_method_and_callbacks_are_usable() {
    let mut connection: ChannelConnectionProxy =
        Box::new(RecordingConnection::new("127.0.0.1:7000"));
    assert_eq!(connection.peer_address(), "127.0.0.1:7000");
    assert!(!connection.is_closed());

    let payload = [1_u8, 2, 3, 4];
    let frame = ChannelFrame {
        payload: payload.as_ptr(),
        size: payload.len(),
    };
    assert_eq!(connection.send_frame(&frame), ChannelError::None);
    connection.flush();

    let closed = OnClosedCallback::from_callable(Box::new(|reason| {
        assert_eq!(reason, ChannelError::Timeout);
    }));
    (closed.callable())(ChannelError::Timeout);
    connection.set_on_closed(closed);

    let framed = OnFrameCallback::from_callable(Box::new(|seen| {
        assert_eq!(seen.size, 4);
    }));
    (framed.callable())(&frame);
    connection.set_on_frame(framed);

    let errored = OnErrorCallback::from_callable(Box::new(|error, message| {
        assert_eq!(error, ChannelError::Internal);
        assert_eq!(message, "boom");
    }));
    (errored.callable())(ChannelError::Internal, "boom");
    connection.set_on_error(errored);

    connection.close();
    assert!(connection.is_closed());
    assert_eq!(connection.send_frame(&frame), ChannelError::ConnectionReset);
}

#[test]
fn listener_and_factory_proxies_preserve_ownership_and_results() {
    let mut factory: ChannelFactoryProxy = Box::new(RecordingFactory);
    assert_eq!(factory.backend_name(), "recording");

    let failed = factory.connect("");
    assert_eq!(failed.error, ChannelError::AddressInvalid);
    assert!(failed.connection.is_none());

    let mut connected = factory.connect("peer");
    assert_eq!(connected.error, ChannelError::None);
    assert_eq!(
        connected.connection.as_ref().unwrap().peer_address(),
        "peer"
    );
    connected.connection.as_mut().unwrap().close();

    let mut listener = factory.make_listener().unwrap();
    assert!(listener.is_closed());
    assert_eq!(listener.listen("127.0.0.1:0"), ChannelError::None);
    assert_eq!(listener.local_address(), "127.0.0.1:0");

    let accepted = OnAcceptCallback::from_callable(Box::new(|connection| {
        assert_eq!(connection.peer_address(), "accepted");
    }));
    (accepted.callable())(Box::new(RecordingConnection::new("accepted")));
    listener.set_on_accept(accepted);
    listener.close();
    assert!(listener.is_closed());
}
