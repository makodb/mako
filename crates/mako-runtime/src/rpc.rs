//! Uses SRPC's Rust client, server and reactor directly. Only owned bytes and a
//! pending-reply guard cross worker threads, never SRPC's borrowed archive.
use crate::wire::{self, Action, Error, ErrorCode, Message, Reply, Result, TransactionAction};
use parking_lot::Mutex;
use srpc::{
    client::Client,
    reactor::PollThread,
    serializable::{BinaryWriteArchive, BufferSource},
    server::{PendingRequestGuard, Request, Server, Service, WeakServerConnection},
};
use std::{
    collections::HashMap,
    ffi::CString,
    sync::{mpsc, Arc},
    time::Duration,
};

pub type Respond = Box<dyn FnOnce(Reply) + Send>;
type Handler = dyn Fn(Message, Respond) + Send + Sync;
struct Pending {
    xid: i64,
    guard: Option<Box<PendingRequestGuard>>,
    connection: WeakServerConnection,
}
impl Pending {
    fn send(self, reply: Reply) {
        let Some(connection) = self.connection.upgrade() else {
            return;
        };
        let bytes = wire::encode(&reply).unwrap_or_else(|_| {
            wire::encode(&Reply::Error(Error::new(
                ErrorCode::Capacity,
                "RPC response exceeds byte limit",
            )))
            .expect("small fixed response")
        });
        let request = Request {
            body: Vec::new(),
            src: BufferSource::new(std::ptr::null(), 0),
            xid: self.xid,
            pending_guard: self.guard,
        };
        connection.reply(
            &request,
            0,
            Some(Box::new(move |archive: &mut BinaryWriteArchive| {
                // SAFETY: serialization consumes this live owned slice synchronously.
                unsafe { archive.write_bytes(bytes.as_ptr(), bytes.len()) };
            })),
        );
    }
}
struct RpcService(Arc<Handler>);
impl Service for RpcService {
    fn __reg_to__(&mut self, server: &mut Server, index: usize) -> i32 {
        server.reg_fast_rpc(wire::RPC_ID, index)
    }
    fn __dispatch__(&self, _: i32, request: Box<Request>, connection: WeakServerConnection) {
        let decoded = request
            .body
            .get(request.src.pos()..)
            .ok_or_else(|| Error::new(ErrorCode::Invalid, "invalid RPC frame cursor"))
            .and_then(wire::decode::<Message>);
        let pending = Pending {
            xid: request.xid,
            guard: request.pending_guard,
            connection,
        };
        match decoded {
            Ok(message) => (self.0)(message, Box::new(move |reply| pending.send(reply))),
            Err(error) => pending.send(Reply::Error(error)),
        }
    }
}

pub struct RpcServer {
    server: Option<Server>,
    address: String,
}
impl RpcServer {
    pub fn start(
        network: &Network,
        address: &str,
        handler: impl Fn(Message, Respond) + Send + Sync + 'static,
    ) -> Result<Self> {
        let address = CString::new(address)
            .map_err(|_| Error::new(ErrorCode::Invalid, "NUL in listen address"))?;
        let mut server = Server::new(Some(network.poll.clone()));
        server.reg_service(Box::new(RpcService(Arc::new(handler))));
        // SAFETY: this NUL-terminated address stays alive throughout start.
        let status = unsafe { server.start(address.as_ptr()) };
        if status != 0 {
            return Err(Error::new(
                ErrorCode::Transport,
                format!("SRPC listen failed: {status}"),
            ));
        }
        let port = server.get_bound_port();
        if port <= 0 {
            return Err(Error::new(
                ErrorCode::Transport,
                "SRPC did not report its listening port",
            ));
        }
        let host = address
            .to_str()
            .expect("address originated as UTF-8")
            .rsplit_once(':')
            .ok_or_else(|| Error::new(ErrorCode::Invalid, "listen address has no port"))?
            .0;
        Ok(Self {
            server: Some(server),
            address: format!("{host}:{port}"),
        })
    }
    pub fn address(&self) -> &str {
        &self.address
    }
    pub fn shutdown(&mut self) {
        self.server.take();
    }
}

pub struct Network {
    poll: Arc<PollThread>,
    clients: Mutex<HashMap<String, Arc<Client>>>,
}
impl Default for Network {
    fn default() -> Self {
        Self {
            poll: PollThread::create(),
            clients: Mutex::new(HashMap::new()),
        }
    }
}
impl Network {
    /// Discovery is read-only. Transaction calls pin the chosen endpoint for
    /// their attempt; an INSTALL is never resent to another participant.
    pub fn find_leader(
        &self,
        owner: u32,
        replicas: &wire::ReplicaSet,
        timeout: Duration,
    ) -> Result<String> {
        mako_protocol::contracts::Transport::find_leader(self, owner, replicas, timeout)
    }

    fn client(&self, endpoint: &str) -> Result<Arc<Client>> {
        let mut clients = self.clients.lock();
        if let Some(client) = clients.get(endpoint) {
            return Ok(client.clone());
        }
        let address = CString::new(endpoint)
            .map_err(|_| Error::new(ErrorCode::Invalid, "NUL in peer address"))?;
        let client = Client::create(self.poll.clone());
        let code = client.connect(address.as_ptr(), true);
        if code != 0 {
            return Err(Error::new(
                ErrorCode::Transport,
                format!("connect {endpoint}: SRPC {code}"),
            ));
        }
        clients.insert(endpoint.into(), client.clone());
        Ok(client)
    }
    pub fn call(&self, endpoint: &str, message: Message, timeout: Duration) -> Result<Reply> {
        let may_commit = matches!(&message, Message::Participant(r) if matches!(r.action, Action::Install { .. }))
            || matches!(&message, Message::Gateway(_))
            || matches!(&message, Message::Transaction(r) if !matches!(r.action, TransactionAction::Lock))
            || matches!(&message, Message::TransactionOutcome { resolve: true, .. });
        // Oversized local encoding cannot have sent an effect.
        let bytes = wire::encode(&message)?;
        let client = self.client(endpoint)?;
        let (send, receive) = mpsc::sync_channel(1);
        let sent = client.request_async(
            wire::RPC_ID,
            |archive| {
                // SAFETY: SRPC serializes synchronously from these live request bytes.
                unsafe { archive.write_bytes(bytes.as_ptr(), bytes.len()) };
            },
            Some(Box::new(move |code, data, len| {
                let result = if code != 0 {
                    Err(Error::new(
                        ErrorCode::Transport,
                        format!("SRPC reply failed: {code}"),
                    ))
                } else if len > wire::MAX_BYTES || (len > 0 && data.is_null()) {
                    Err(Error::new(ErrorCode::Transport, "invalid reply span"))
                } else {
                    // SAFETY: the validated span is borrowed only during this callback.
                    let bytes = if len == 0 {
                        &[]
                    } else {
                        unsafe { std::slice::from_raw_parts(data, len) }
                    };
                    wire::decode::<Reply>(bytes)
                };
                let _ = send.try_send(result);
            })),
        );
        let result = match sent {
            Ok(_) => receive
                .recv_timeout(timeout)
                .unwrap_or_else(|_| Err(Error::new(ErrorCode::Transport, "RPC deadline elapsed"))),
            Err(code) => Err(Error::new(
                ErrorCode::Transport,
                format!("SRPC send failed: {code}"),
            )),
        };
        if result.is_err() {
            // Cancel the pending future as well as the caller's wait. Subsequent
            // calls reconnect; they never silently replay a transaction body.
            client.close();
            let mut clients = self.clients.lock();
            if clients
                .get(endpoint)
                .is_some_and(|current| Arc::ptr_eq(current, &client))
            {
                clients.remove(endpoint);
            }
        }
        result.map_err(|mut error| {
            if may_commit {
                error.code = ErrorCode::Unknown;
            }
            error
        })
    }
}
impl Drop for Network {
    fn drop(&mut self) {
        for (_, client) in self.clients.get_mut().drain() {
            client.close();
        }
        self.poll.shutdown();
    }
}

impl mako_protocol::contracts::Transport for Network {
    fn call(&self, endpoint: &str, message: Message, timeout: Duration) -> Result<Reply> {
        Network::call(self, endpoint, message, timeout)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::wire::{Identity, Request, Response, Stream, TransactionRequest, Value};
    #[test]
    fn native_srpc_preserves_owned_request_and_reply_across_worker_threads() {
        let network = Network::default();
        let (send, receive) = mpsc::sync_channel::<(Message, Respond)>(4);
        let mut server = RpcServer::start(&network, "127.0.0.1:0", move |request, reply| {
            send.send((request, reply)).unwrap()
        })
        .unwrap();
        let worker = std::thread::spawn(move || {
            let (Message::Participant(request), reply) = receive.recv().unwrap() else {
                panic!("wrong message");
            };
            reply(Reply::Participant(Response {
                identity: request.identity,
                result: Ok(Value::Timestamp(42)),
            }));
        });
        let identity = Identity {
            incarnation: 7,
            attempt: 1,
            sequence: 1,
            term: 0,
        };
        let request = Request {
            stream: Stream { node: 0, worker: 0 },
            identity,
            action: Action::Timestamp,
        };
        let reply = network
            .call(
                server.address(),
                Message::Participant(request),
                Duration::from_secs(3),
            )
            .unwrap();
        assert!(
            matches!(reply, Reply::Participant(r) if r.identity == identity && r.result == Ok(Value::Timestamp(42)))
        );
        worker.join().unwrap();
        server.shutdown();
    }
    #[test]
    fn timed_out_decision_is_unknown_and_pending_call_is_cancelled() {
        let network = Network::default();
        let pending = Arc::new(Mutex::new(Vec::new()));
        let hold = pending.clone();
        let mut server = RpcServer::start(&network, "127.0.0.1:0", move |_, reply| {
            hold.lock().push(reply)
        })
        .unwrap();
        let transaction = mako_protocol::journal::Transaction {
            version: mako_protocol::journal::VERSION,
            id: mako_protocol::journal::TransactionId {
                coordinator: 0,
                worker: 0,
                incarnation: 7,
                attempt: 1,
            },
            time_term: 100,
            durable: false,
            members: vec![mako_protocol::journal::Member {
                owner: 0,
                endpoints: vec![server.address().into()],
                lease: 1,
            }],
        };
        let request = TransactionRequest {
            id: transaction.id,
            lease: 1,
            term: 0,
            action: TransactionAction::Decide {
                transaction,
                outcome: mako_protocol::journal::Outcome::Commit,
            },
        };
        assert_eq!(
            network
                .call(
                    server.address(),
                    Message::Transaction(request),
                    Duration::from_millis(100)
                )
                .unwrap_err()
                .code,
            ErrorCode::Unknown
        );
        assert!(network.clients.lock().is_empty());
        pending.lock().clear();
        server.shutdown();
    }
}
