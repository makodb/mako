#![allow(unsafe_code)]

use srpc::client::{AsyncReplyCallback, Client, ClientConnection, CLIENT_ERR_NOT_CONNECTED};
use srpc::inmemory_channel::{make_inmemory_factory_proxy, InMemoryFactory, InMemorySwitchboard};
use srpc::reactor::PollThread;
use srpc::serializable::{BinaryWriteArchive, Serialize};
use srpc::server::{Request, Server, ServerReplyFn, Service, WeakServerConnection};
use std::ffi::CString;
use std::sync::{mpsc, Arc, Barrier};
use std::time::Duration;

const RPC: i32 = 0x00E0_0089;

struct ReplyService {
    entered: Option<mpsc::Sender<()>>,
    release: Option<Arc<Barrier>>,
}

impl Service for ReplyService {
    fn __reg_to__(&mut self, server: &mut Server, index: usize) -> i32 {
        server.reg_fast_rpc(RPC, index)
    }

    fn __dispatch__(&self, rpc_id: i32, request: Box<Request>, connection: WeakServerConnection) {
        assert_eq!(rpc_id, RPC);
        let connection = connection.upgrade().expect("live server connection");
        if let Some(entered) = &self.entered {
            entered.send(()).unwrap();
            self.release.as_ref().unwrap().wait();
        }
        let writer: ServerReplyFn = Some(Box::new(|archive: &mut BinaryWriteArchive| {
            Serialize::serialize(&42i64, archive);
        }));
        connection.reply(&request, 0, writer);
    }
}

fn connected(
    service: ReplyService,
    name: &str,
) -> (Server, Arc<Client>, Arc<PollThread>, Arc<PollThread>) {
    let switchboard = Arc::new(InMemorySwitchboard::new());
    let server_poll = PollThread::create();
    let client_poll = PollThread::create();
    let mut server = Server::new(Some(server_poll.clone()));
    server.set_channel_factory(Some(make_inmemory_factory_proxy(Arc::new(InMemoryFactory::new(
        switchboard.clone(),
    )))));
    server.reg_service(Box::new(service));
    let address = CString::new(format!("inmemory://{name}")).unwrap();
    assert_eq!(unsafe { server.start(address.as_ptr()) }, 0);
    let client = Client::create(client_poll.clone());
    client.set_channel_factory(Some(make_inmemory_factory_proxy(Arc::new(InMemoryFactory::new(
        switchboard,
    )))));
    assert_eq!(client.connect(address.as_ptr(), true), 0);
    (server, client, server_poll, client_poll)
}

#[test]
fn inline_reply_callback_can_send_again_and_close_the_client_connection() {
    let (server, client, server_poll, client_poll) = connected(
        ReplyService {
            entered: None,
            release: None,
        },
        "client-reentrant-send-close",
    );
    let connection = client.connection().unwrap();
    let callback_connection = connection.clone();
    let (done_tx, done_rx) = mpsc::channel();
    let worker = std::thread::spawn(move || {
        let callback: AsyncReplyCallback =
            Some(Box::new(move |error, _payload, _size| {
                assert_eq!(error, 0);
                let nested = callback_connection.request_async(
                    RPC,
                    |_| {},
                    Some(Box::new(|nested_error, _, _| {
                        assert_eq!(nested_error, 0)
                    })),
                );
                assert_eq!(nested, Ok(()));
                callback_connection.close();
            }));
        assert_eq!(connection.request_async(RPC, |_| {}, callback), Ok(()));
        assert_eq!(connection.pending_future_count(), 0);
        done_tx.send(()).unwrap();
    });
    done_rx
        .recv_timeout(Duration::from_secs(3))
        .expect("inline reentrant callbacks must finish");
    worker.join().unwrap();
    drop(client);
    drop(server);
    client_poll.shutdown();
    server_poll.shutdown();
}

#[test]
fn concurrent_close_detaches_the_slot_while_an_inflight_send_owns_the_channel() {
    fn assert_send_sync<T: Send + Sync>() {}
    assert_send_sync::<ClientConnection>();
    let (entered_tx, entered_rx) = mpsc::channel();
    let release = Arc::new(Barrier::new(2));
    let (server, client, server_poll, client_poll) = connected(
        ReplyService {
            entered: Some(entered_tx),
            release: Some(release.clone()),
        },
        "client-inflight-close",
    );
    let connection = client.connection().unwrap();
    let sending = connection.clone();
    let (sent_tx, sent_rx) = mpsc::channel();
    let sender = std::thread::spawn(move || {
        let result = sending.request_async(
            RPC,
            |_| {},
            Some(Box::new(|error, _, _| {
                assert_eq!(error, CLIENT_ERR_NOT_CONNECTED)
            })),
        );
        sent_tx.send(result).unwrap();
    });
    entered_rx
        .recv_timeout(Duration::from_secs(3))
        .expect("server entered the send callback");
    let (closed_tx, closed_rx) = mpsc::channel();
    let closer = std::thread::spawn(move || {
        connection.close();
        closed_tx.send(()).unwrap();
    });
    closed_rx
        .recv_timeout(Duration::from_secs(3))
        .expect("close must not wait on an in-flight send's slot lock");
    release.wait();
    let _ = sent_rx
        .recv_timeout(Duration::from_secs(3))
        .expect("in-flight send finishes after close");
    sender.join().unwrap();
    closer.join().unwrap();
    drop(client);
    drop(server);
    client_poll.shutdown();
    server_poll.shutdown();
}
