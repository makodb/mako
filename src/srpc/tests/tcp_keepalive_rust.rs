#![allow(unsafe_code)]

use srpc::channel::{ChannelError, ChannelFactoryBase, ChannelListenerProxy, ConnectResult};
use srpc::client::{Client, KeepaliveConfig};
use srpc::reactor::PollThread;
use srpc::tcp_channel::{make_tcp_connection_channel_proxy, TcpConnection};
use std::net::{TcpListener, TcpStream};
use std::os::fd::IntoRawFd;
use std::sync::Arc;

unsafe extern "C" {
    fn getsockopt(fd: i32, level: i32, option: i32, value: *mut i32, size: *mut u32) -> i32;
}

// Linux socket ABI constants; production resolves these in the native leaves.
const SOL_SOCKET: i32 = 1;
const SO_KEEPALIVE: i32 = 9;
const IPPROTO_TCP: i32 = 6;
const TCP_KEEPIDLE: i32 = 4;
const TCP_KEEPINTVL: i32 = 5;
const TCP_KEEPCNT: i32 = 6;

fn option(fd: i32, level: i32, name: i32) -> i32 {
    let mut value = -1;
    let mut size = std::mem::size_of::<i32>() as u32;
    assert_eq!(unsafe { getsockopt(fd, level, name, &mut value, &mut size) }, 0);
    assert_eq!(size, std::mem::size_of::<i32>() as u32);
    value
}

fn pair() -> (Arc<TcpConnection>, TcpStream) {
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let outgoing = TcpStream::connect(listener.local_addr().unwrap()).unwrap();
    let (peer, _) = listener.accept().unwrap();
    let fd = outgoing.into_raw_fd();
    let connection = Arc::new(unsafe { TcpConnection::new(fd, "keepalive-peer".to_string()) });
    (connection, peer)
}

#[test]
fn keepalive_updates_real_socket_options_and_preserves_disable_semantics() {
    let (connection, _peer) = pair();
    let fd = connection.fd();
    assert!(connection.set_keepalive(true, 17, 4, 6));
    assert_eq!(option(fd, SOL_SOCKET, SO_KEEPALIVE), 1);
    assert_eq!(option(fd, IPPROTO_TCP, TCP_KEEPIDLE), 17);
    assert_eq!(option(fd, IPPROTO_TCP, TCP_KEEPINTVL), 4);
    assert_eq!(option(fd, IPPROTO_TCP, TCP_KEEPCNT), 6);
    assert!(connection.set_keepalive(false, 0, 0, 0));
    assert_eq!(option(fd, SOL_SOCKET, SO_KEEPALIVE), 0);
    assert_eq!(option(fd, IPPROTO_TCP, TCP_KEEPIDLE), 17);
    assert_eq!(option(fd, IPPROTO_TCP, TCP_KEEPINTVL), 4);
    assert_eq!(option(fd, IPPROTO_TCP, TCP_KEEPCNT), 6);

    // The first invalid tuning option must not prevent the remaining updates.
    assert!(!connection.set_keepalive(true, 0, 7, 8));
    assert_eq!(option(fd, IPPROTO_TCP, TCP_KEEPINTVL), 7);
    assert_eq!(option(fd, IPPROTO_TCP, TCP_KEEPCNT), 8);
    connection.close();
    assert!(!connection.set_keepalive(true, 17, 4, 6));
}

struct ConnectedTcpFactory(Arc<TcpConnection>);

impl ChannelFactoryBase for ConnectedTcpFactory {
    fn connect(&mut self, _: &str) -> ConnectResult {
        ConnectResult {
            connection: Some(make_tcp_connection_channel_proxy(self.0.clone())),
            error: ChannelError::None,
        }
    }
    fn make_listener(&mut self) -> Option<ChannelListenerProxy> { None }
    fn backend_name(&self) -> String { "existing-tcp-connection".to_string() }
}

#[test]
fn client_applies_pending_keepalive_on_bind_and_live_updates_to_the_transport() {
    let (connection, _peer) = pair();
    let poll = PollThread::create();
    let client = Client::create(poll.clone());
    client.set_keepalive(&KeepaliveConfig {
        enabled: true, idle_sec: 19, interval_sec: 5, count: 7,
    });
    client.set_channel_factory(Some(Box::new(ConnectedTcpFactory(connection.clone()))));
    let address = std::ffi::CString::new("127.0.0.1:1").unwrap();
    assert_eq!(client.connect(address.as_ptr(), true), 0);
    let fd = connection.fd();
    assert_eq!(option(fd, SOL_SOCKET, SO_KEEPALIVE), 1);
    assert_eq!(option(fd, IPPROTO_TCP, TCP_KEEPIDLE), 19);
    assert_eq!(option(fd, IPPROTO_TCP, TCP_KEEPINTVL), 5);
    assert_eq!(option(fd, IPPROTO_TCP, TCP_KEEPCNT), 7);
    client.set_keepalive(&KeepaliveConfig::aggressive());
    assert_eq!(option(fd, IPPROTO_TCP, TCP_KEEPIDLE), 10);
    assert_eq!(option(fd, IPPROTO_TCP, TCP_KEEPINTVL), 2);
    assert_eq!(option(fd, IPPROTO_TCP, TCP_KEEPCNT), 3);
    client.set_keepalive(&KeepaliveConfig::disabled());
    assert_eq!(option(fd, SOL_SOCKET, SO_KEEPALIVE), 0);
    client.connection().unwrap().close();
    drop(client);
    poll.shutdown();
}
