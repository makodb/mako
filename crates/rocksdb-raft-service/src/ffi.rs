//! The only raw pointers here are the C caller's explicitly borrowed buffers
//! and opaque ownership handles. Native transaction frames never enter this ABI.
use crate::{
    client::Session,
    config::ClientConfig,
    wire::{Envelope, Error, ErrorCode, Response, MAX_WIRE_BYTES, PROTOCOL_VERSION},
};
use parking_lot::Mutex;
use std::{ptr, sync::Arc};

#[repr(C)]
pub struct MakoRocksBuffer {
    pub data: *mut u8,
    pub len: usize,
}
pub struct MakoRocksClient {
    session: Mutex<Session>,
}
fn buffer(bytes: Vec<u8>) -> MakoRocksBuffer {
    let bytes = bytes.into_boxed_slice();
    let len = bytes.len();
    MakoRocksBuffer {
        data: Box::into_raw(bytes).cast::<u8>(),
        len,
    }
}
fn error_buffer(message: impl Into<String>) -> MakoRocksBuffer {
    buffer(
        serde_json::to_vec(&Response::Error {
            error: Error::new(ErrorCode::Invalid, message),
        })
        .expect("fixed error encoding"),
    )
}

/// Opens a native SRPC client using connection-only `ClientConfig` JSON.
/// Returns 0 on success, -1 with an owned JSON error buffer on invalid input.
/// # Safety
/// `out` and `error` must be valid writable pointers; `config_json` must contain
/// `len` readable bytes and remain live until return. Out buffers must be empty.
#[no_mangle]
pub unsafe extern "C" fn mako_rocksdb_raft_client_open(
    config_json: *const u8,
    len: usize,
    out: *mut *mut MakoRocksClient,
    error: *mut MakoRocksBuffer,
) -> i32 {
    if out.is_null() || error.is_null() {
        return -1;
    }
    // SAFETY: caller provides these writable output locations.
    unsafe {
        *out = ptr::null_mut();
        *error = MakoRocksBuffer {
            data: ptr::null_mut(),
            len: 0,
        };
    }
    if config_json.is_null() || len == 0 || len > MAX_WIRE_BYTES {
        unsafe {
            *error = error_buffer("invalid configuration buffer");
        }
        return -1;
    }
    // SAFETY: the caller owns this live input and length has been bounded.
    let bytes = unsafe { std::slice::from_raw_parts(config_json, len) };
    let config = serde_json::from_slice::<ClientConfig>(bytes)
        .map_err(|e| e.to_string())
        .and_then(|config| config.validate().map(|_| config).map_err(|e| e.to_string()));
    match config {
        Ok(config) => {
            let client = Box::new(MakoRocksClient {
                session: Mutex::new(Session::new(Arc::new(config))),
            });
            unsafe {
                *out = Box::into_raw(client);
            }
            0
        }
        Err(message) => {
            unsafe {
                *error = error_buffer(message);
            }
            -1
        }
    }
}

/// Executes a versioned `wire::Envelope` JSON object. Returns 0 when an owned
/// `wire::Response` JSON buffer is returned, including typed application,
/// transport and unknown-outcome errors. -1 means malformed ABI input/JSON.
/// Operations on one client are serialized; use independent clients for load.
/// # Safety
/// `client` is a live handle returned by open, not concurrently closed. Input is
/// readable for `len` bytes; response points to an empty writable buffer.
#[no_mangle]
pub unsafe extern "C" fn mako_rocksdb_raft_request(
    client: *mut MakoRocksClient,
    request_json: *const u8,
    len: usize,
    response: *mut MakoRocksBuffer,
) -> i32 {
    if response.is_null() {
        return -1;
    }
    unsafe {
        *response = MakoRocksBuffer {
            data: ptr::null_mut(),
            len: 0,
        };
    }
    if client.is_null() || request_json.is_null() || len == 0 || len > MAX_WIRE_BYTES {
        unsafe {
            *response = error_buffer("invalid client/request buffer");
        }
        return -1;
    }
    let bytes = unsafe { std::slice::from_raw_parts(request_json, len) };
    let request = match serde_json::from_slice::<Envelope>(bytes) {
        Ok(envelope) if envelope.version == PROTOCOL_VERSION => envelope.request,
        Ok(_) => {
            unsafe {
                *response = error_buffer("unsupported protocol version");
            }
            return -1;
        }
        Err(e) => {
            unsafe {
                *response = error_buffer(e.to_string());
            }
            return -1;
        }
    };
    // SAFETY: this shared borrow is protected by the client's mutex. The opaque
    // pointer stays caller-owned until a separate, nonconcurrent close.
    let result = unsafe { &*client }.session.lock().request(request);
    let value = Response::from_result(result);
    unsafe {
        *response = buffer(crate::rpc::encode_response(&value, MAX_WIRE_BYTES));
    }
    0
}

/// # Safety
/// Pass a buffer returned by this ABI exactly once, without changing its fields.
#[no_mangle]
pub unsafe extern "C" fn mako_rocksdb_raft_buffer_free(buffer: MakoRocksBuffer) {
    if !buffer.data.is_null() {
        // SAFETY: allocation came from Box<[u8]> with this exact slice length.
        unsafe {
            drop(Box::from_raw(ptr::slice_from_raw_parts_mut(
                buffer.data,
                buffer.len,
            )));
        }
    }
}
/// # Safety
/// Pass a live client once, after all concurrent requests finish. Null is allowed.
#[no_mangle]
pub unsafe extern "C" fn mako_rocksdb_raft_client_close(client: *mut MakoRocksClient) {
    if !client.is_null() {
        unsafe {
            drop(Box::from_raw(client));
        }
    }
}
