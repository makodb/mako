use crate::wire::{Envelope, Error, ErrorCode, Request, Response, Result, MAX_WIRE_BYTES, RPC_ID};
use srpc::{client::Client, reactor::PollThread, serializable::{BinaryWriteArchive, BufferSource},
    server::{PendingRequestGuard, Request as RpcRequest, WeakServerConnection}};
use std::{ffi::CString, sync::{mpsc, Arc}, time::Duration};

struct BoundedJson { bytes: Vec<u8>, limit: usize }
impl std::io::Write for BoundedJson {
    fn write(&mut self, bytes: &[u8]) -> std::io::Result<usize> {
        if bytes.len() > self.limit.saturating_sub(self.bytes.len()) {
            return Err(std::io::Error::new(std::io::ErrorKind::InvalidInput, "JSON wire byte limit exceeded"));
        }
        self.bytes.extend_from_slice(bytes);
        Ok(bytes.len())
    }
    fn flush(&mut self) -> std::io::Result<()> { Ok(()) }
}
fn encode_bounded(value: &impl serde::Serialize, limit: usize) -> std::result::Result<Vec<u8>, serde_json::Error> {
    let mut output = BoundedJson { bytes: Vec::with_capacity(limit.min(4096)), limit };
    serde_json::to_writer(&mut output, value)?;
    Ok(output.bytes)
}
pub(crate) fn encode_response(response: &Response, limit: usize) -> Vec<u8> {
    encode_bounded(response, limit).unwrap_or_else(|_| {
        serde_json::to_vec(&Response::Error { error: Error::new(ErrorCode::TooLarge, "response exceeds RPC byte limit") }).expect("fixed error encoding")
    })
}

/// Only the owned request identity and pending guard cross threads. SRPC's
/// request archive contains a borrowed raw pointer and is deliberately not sent.
pub(crate) struct Reply {
    xid: i64, pending: Option<Box<PendingRequestGuard>>, connection: WeakServerConnection, limit: usize,
}
impl Reply {
    pub(crate) fn take(request: Box<RpcRequest>, connection: WeakServerConnection, limit: usize) -> Self {
        Self { xid: request.xid, pending: request.pending_guard, connection, limit }
    }
    pub(crate) fn send(self, response: Response) {
        let Some(connection) = self.connection.upgrade() else { return; };
        let bytes = encode_response(&response, self.limit);
        let request = RpcRequest { body: Vec::new(), src: BufferSource::new(std::ptr::null(), 0),
            xid: self.xid, pending_guard: self.pending };
        connection.reply(&request, 0, Some(Box::new(move |archive: &mut BinaryWriteArchive| {
            // SAFETY: archive consumes these owned, live bytes synchronously.
            unsafe { archive.write_bytes(bytes.as_ptr(), bytes.len()) };
        })));
    }
    pub(crate) fn error(self, code: ErrorCode, message: impl Into<String>) {
        self.send(Response::Error { error: Error::new(code, message) });
    }
}

pub(crate) struct Connection { client: Arc<Client> }
impl Connection {
    pub(crate) fn connect(endpoint: &str, poll: Arc<PollThread>) -> Result<Self> {
        let address = CString::new(endpoint).map_err(|_| Error::new(ErrorCode::Invalid, "NUL in endpoint"))?;
        let client = Client::create(poll);
        let status = client.connect(address.as_ptr(), true);
        if status != 0 { return Err(Error::new(ErrorCode::Transport, format!("connect {endpoint}: SRPC {status}"))); }
        Ok(Self { client })
    }
    pub(crate) fn call(&self, request: Request, timeout: Duration, max_bytes: usize) -> Result<Response> {
        let may_commit = matches!(&request, Request::Commit { .. } | Request::CreateTable { .. });
        let bytes = encode_bounded(&Envelope::new(request), max_bytes.min(MAX_WIRE_BYTES))
            .map_err(|e| Error::new(ErrorCode::TooLarge, e.to_string()))?;
        let (send, receive) = mpsc::sync_channel(1);
        let result = self.client.request_async(RPC_ID, |archive| {
            // SAFETY: request_async serializes synchronously from this live slice.
            unsafe { archive.write_bytes(bytes.as_ptr(), bytes.len()) };
        }, Some(Box::new(move |code, data, len| {
            let result = if code != 0 {
                Err(Error::new(if may_commit { ErrorCode::UnknownOutcome } else { ErrorCode::Transport }, format!("SRPC reply error {code}")))
            } else if len > max_bytes || len > MAX_WIRE_BYTES || (len > 0 && data.is_null()) {
                Err(Error::new(ErrorCode::TooLarge, "invalid or oversized reply"))
            } else {
                // SAFETY: SRPC guarantees the callback payload for this call;
                // no pointer escapes and the length is checked before decoding.
                let bytes = if len == 0 { &[] } else { unsafe { std::slice::from_raw_parts(data, len) } };
                serde_json::from_slice::<Response>(bytes).map_err(|e| Error::new(ErrorCode::Transport, e.to_string()))
            };
            let _ = send.try_send(result);
        })));
        if let Err(code) = result {
            return Err(Error::new(if may_commit { ErrorCode::UnknownOutcome } else { ErrorCode::Transport }, format!("SRPC send error {code}")));
        }
        match receive.recv_timeout(timeout) {
            Ok(result) => result?.into_result(),
            Err(_) => {
                // Closing cancels SRPC's pending future as well: bounded callers
                // cannot accumulate abandoned requests on a silent connection.
                self.client.close();
                Err(Error::new(if may_commit { ErrorCode::UnknownOutcome } else { ErrorCode::Transport }, "RPC deadline elapsed; no transaction body was replayed"))
            }
        }
    }
}
impl Drop for Connection { fn drop(&mut self) { self.client.close(); } }
