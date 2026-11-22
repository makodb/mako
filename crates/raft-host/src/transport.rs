use crate::{
    runtime::Message,
    snapshot::{self, Image},
    wire::{self, Envelope, Request as WireRequest, Response},
    Config, Peer, Position,
};
use srpc::client::{deserialize_from, Client, FutureAttr};
use srpc::reactor::PollThread;
use srpc::request_options::RequestOptions;
use srpc::serializable::{BufferSource, Serialize};
use srpc::server::{PendingRequestGuard, Request, Server, Service, WeakServerConnection};
use std::ffi::CString;
use std::sync::{mpsc, Arc};

pub(crate) struct ReplyToken {
    xid: i64,
    connection: WeakServerConnection,
    _pending: Option<Box<PendingRequestGuard>>,
    isolation: crate::Isolation,
}
impl ReplyToken {
    pub fn send(self, response: Response) {
        let response = if self.isolation.active() {
            Response::Error("Raft peer isolated by test hook".into())
        } else {
            response
        };
        if let Some(connection) = self.connection.upgrade() {
            let request = Request {
                body: Vec::new(),
                src: BufferSource::new(std::ptr::null(), 0),
                xid: self.xid,
                pending_guard: None,
            };
            match wire::encode(&response) {
                Ok(bytes) => connection.reply(
                    &request,
                    0,
                    Some(Box::new(move |archive| {
                        Serialize::serialize(&bytes, archive);
                    })),
                ),
                Err(_) => connection.reply(&request, 22, None),
            }
        }
    }
}
struct RpcService {
    tx: mpsc::SyncSender<Message>,
    isolation: crate::Isolation,
}
impl Service for RpcService {
    fn __reg_to__(&mut self, server: &mut Server, index: usize) -> i32 {
        server.reg_fast_rpc(wire::RPC, index)
    }
    fn __dispatch__(&self, _: i32, mut request: Box<Request>, connection: WeakServerConnection) {
        let token = ReplyToken {
            xid: request.xid,
            connection,
            _pending: request.pending_guard.take(),
            isolation: self.isolation.clone(),
        };
        if self.isolation.active() {
            token.send(Response::Error("Raft peer isolated by test hook".into()));
            return;
        }
        let decoded = request
            .body
            .get(request.src.pos_..)
            .ok_or_else(|| "invalid RPC cursor".to_string())
            .and_then(wire::decode::<Envelope>);
        match decoded {
            Ok(envelope) => {
                if let Err(failure) = self.tx.try_send(Message::Inbound(envelope, token)) {
                    let (mpsc::TrySendError::Full(message)
                    | mpsc::TrySendError::Disconnected(message)) = failure;
                    if let Message::Inbound(_, token) = message {
                        token.send(Response::Error("Raft inbox full or stopped".into()));
                    }
                }
            }
            Err(error) => token.send(Response::Error(error)),
        }
    }
}
pub(crate) fn listen(
    config: &Config,
    tx: mpsc::SyncSender<Message>,
    poll: Arc<PollThread>,
    isolation: crate::Isolation,
) -> Result<Server, String> {
    let mut server = Server::new(Some(poll));
    server.reg_service(Box::new(RpcService { tx, isolation }));
    let address = CString::new(config.listen.as_str()).map_err(|e| e.to_string())?;
    // SAFETY: NUL-terminated address is live for the entire synchronous call.
    let code = unsafe { server.start(address.as_ptr()) };
    if code != 0 {
        return Err(format!("SRPC listen {} failed: {code}", config.listen));
    }
    Ok(server)
}
#[derive(Clone, Copy, Debug)]
pub(crate) enum Tag {
    Vote {
        campaign: u64,
        peer: u16,
    },
    Append {
        slot: usize,
        term: u64,
        round: u64,
    },
    Snapshot {
        ordinal: usize,
        peer: u16,
        term: u64,
        position: Position,
    },
    Contact {
        ordinal: usize,
        peer: u16,
        term: u64,
    },
}
pub(crate) enum Job {
    Rpc { request: WireRequest, tag: Tag },
    Snapshot { image: Image, tag: Tag, term: u64 },
}
pub(crate) fn peer_thread(
    config: Config,
    peer: Peer,
    tx: mpsc::SyncSender<Message>,
    poll: Arc<PollThread>,
    isolation: crate::Isolation,
) -> (mpsc::SyncSender<Job>, std::thread::JoinHandle<()>) {
    let (jobs, incoming) = mpsc::sync_channel::<Job>(8);
    let thread = std::thread::Builder::new()
        .name(format!("raft-{}-to-{}", config.group, peer.id))
        .spawn(move || {
            let client = Client::create(poll);
            let address = match CString::new(peer.address.as_str()) {
                Ok(a) => a,
                Err(_) => return,
            };
            let mut connected = false;
            while let Ok(job) = incoming.recv() {
                let tag = match &job {
                    Job::Rpc { tag, .. } | Job::Snapshot { tag, .. } => *tag,
                };
                let connect_error = if !connected && !isolation.active() {
                    let code = client.connect(address.as_ptr(), true);
                    connected = code == 0;
                    code
                } else {
                    0
                };
                let result = if isolation.active() {
                    Err("Raft peer isolated by test hook".into())
                } else if connected {
                    match job {
                        Job::Rpc { request, .. } => call(&client, &config, request, &isolation),
                        Job::Snapshot { image, term, .. } => {
                            send_snapshot(&client, &config, image, term, &isolation)
                        }
                    }
                } else {
                    Err(format!(
                        "peer {} connect failed: code={connect_error}",
                        peer.address
                    ))
                };
                if let Err(error) = &result {
                    if connected {
                        eprintln!(
                            "raft-host transport group={} node={} peer={} request={:?} error={}",
                            config.group, config.node, peer.id, tag, error
                        );
                    }
                    client.close();
                    connected = false;
                }
                if tx.send(Message::Network(tag, result)).is_err() {
                    break;
                }
            }
            client.close();
        })
        .expect("spawn Raft peer transport");
    (jobs, thread)
}
fn call(
    client: &Arc<Client>,
    cfg: &Config,
    request: WireRequest,
    isolation: &crate::Isolation,
) -> Result<Response, String> {
    if isolation.active() {
        return Err("Raft peer isolated by test hook".into());
    }
    // A private generation may need a long bounded hash/fsync/publication.
    // A separate transport lane keeps leader contacts flowing meanwhile.
    let timeout = if matches!(request, WireRequest::SnapshotFinish { .. }) {
        cfg.rpc_timeout.max(std::time::Duration::from_secs(60))
    } else {
        cfg.rpc_timeout
    };
    let options = RequestOptions {
        timeout_ms: timeout.as_millis().max(1) as u64,
        total_timeout_ms: timeout.as_millis().max(1) as u64,
        ..RequestOptions::new()
    };
    let bytes = wire::encode(&Envelope {
        version: wire::version(cfg.sync_writes),
        group: cfg.group,
        sender: cfg.node,
        request,
    })?;
    let started = std::time::Instant::now();
    // This lane already has its own blocking worker. request_with_options
    // would spawn another OS thread and copy a coordinator reply for every
    // heartbeat; direct request + bounded future wait needs neither.
    let future = client
        .request(wire::RPC, &FutureAttr::default(), |archive| {
            // SAFETY: byte slice remains borrowed while the synchronous serializer runs.
            unsafe {
                archive.write_bytes(bytes.as_ptr(), bytes.len());
            }
        })
        .map_err(|code| format!("SRPC send failed: {code}"))?;
    future.set_options(&options);
    let ready = future.wait_with_options();
    let error = future.get_error_code();
    if !ready || error != 0 {
        return Err(format!(
            "SRPC response unavailable: code={error} elapsed={:?}",
            started.elapsed()
        ));
    }
    if isolation.active() {
        return Err("Raft peer isolated by test hook".into());
    }
    let mut bytes = Vec::<u8>::new();
    deserialize_from(future.get_reply(), &mut bytes);
    wire::decode(&bytes)
}
fn send_snapshot(
    client: &Arc<Client>,
    cfg: &Config,
    image: Image,
    term: u64,
    isolation: &crate::Isolation,
) -> Result<Response, String> {
    let position = image.reference.position;
    let begin = call(
        client,
        cfg,
        WireRequest::SnapshotBegin {
            term,
            position,
            digest: image.reference.digest,
        },
        isolation,
    )?;
    if let Response::SnapshotCurrent { term } = begin {
        return Ok(Response::Snapshot {
            term,
            accepted: true,
        });
    }
    if !matches!(begin, Response::Snapshot { accepted: true, .. }) {
        return Ok(begin);
    }
    let manifest = image.manifest()?;
    let files = std::iter::once(snapshot::MANIFEST.to_owned())
        .chain(manifest.files.into_iter().map(|f| f.name));
    for file in files {
        let started = std::time::Instant::now();
        let mut transferred = 0u64;
        snapshot::stream_file(
            &image.directory.join(&file),
            cfg.snapshot_chunk_bytes,
            |offset, bytes, last| {
                let count = bytes.len() as u64;
                let reply = call(
                    client,
                    cfg,
                    WireRequest::SnapshotChunk {
                        term,
                        position,
                        file: file.clone(),
                        offset,
                        bytes,
                        last,
                    },
                    isolation,
                )
                .map_err(|error| {
                    format!("snapshot file={file} offset={offset} bytes={count}: {error}")
                })?;
                match reply {
                    Response::Snapshot { accepted: true, .. } => {
                        transferred = offset + count;
                        Ok(())
                    }
                    other => Err(format!(
                        "snapshot file={file} offset={offset} bytes={count} rejected: {other:?}"
                    )),
                }
            },
        )?;
        eprintln!(
            "raft-host snapshot file group={} node={} position={:?} file={} bytes={} elapsed={:?}",
            cfg.group,
            cfg.node,
            position,
            file,
            transferred,
            started.elapsed()
        );
    }
    call(
        client,
        cfg,
        WireRequest::SnapshotFinish { term, position },
        isolation,
    )
}
