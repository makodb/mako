use std::net::{TcpListener, TcpStream, SocketAddr};
use std::io::{Read, Write, BufWriter};
use bytes::Bytes;
use redis_protocol::resp3::{types::BytesFrame, types::DecodedFrame};
use socket2::{Socket, Domain, Type, Protocol};
use std::sync::Arc;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Barrier;

mod resp3_handler;
use resp3_handler::Resp3Handler;

// ===== FFI Types (must match transaction_ffi.h) =====

const TXN_OP_GET: u32 = 1;
const TXN_OP_SET: u32 = 2;

#[repr(C)]
struct TxnOperation {
    op: u32,
    key_ptr: *const u8,
    key_len: usize,
    val_ptr: *const u8,
    val_len: usize,
}

#[repr(C)]
struct TxnRequest {
    num_ops: usize,
    ops: *const TxnOperation,
}

#[repr(C)]
struct TxnOpResult {
    success: bool,
    data_ptr: *mut u8,
    data_len: usize,
}

#[repr(C)]
struct TxnResponse {
    transaction_success: bool,
    num_results: usize,
    results: *mut TxnOpResult,
}

extern "C" {
    fn cpp_worker_thread_init(thread_id: usize);

    // All operations (single or batched) go through the transaction interface
    fn cpp_execute_transaction(request: *const TxnRequest, response: *mut TxnResponse) -> bool;
    fn cpp_free_transaction_response(response: *mut TxnResponse);
}

// ===== OpCode and Command =====

#[derive(Copy, Clone, PartialEq)]
#[repr(u32)]
enum OpCode {
    Invalid = 0,
    Get     = 1,
    Set     = 2,
    Ping    = 3,
    Multi   = 4,
    Exec    = 5,
    Discard = 6,
}

#[derive(Clone)]
struct Command {
    op: OpCode,
    key: Bytes,
    val: Option<Bytes>,
}

// ===== Transaction State =====

/// Per-connection transaction state
struct TransactionState {
    in_multi: bool,
    queued_commands: Vec<Command>,
}

impl TransactionState {
    fn new() -> Self {
        TransactionState {
            in_multi: false,
            queued_commands: Vec::new(),
        }
    }

    fn start_multi(&mut self) {
        self.in_multi = true;
        self.queued_commands.clear();
    }

    fn queue_command(&mut self, cmd: Command) {
        self.queued_commands.push(cmd);
    }

    fn discard(&mut self) {
        self.in_multi = false;
        self.queued_commands.clear();
    }

    fn take_commands(&mut self) -> Vec<Command> {
        self.in_multi = false;
        std::mem::take(&mut self.queued_commands)
    }
}

// ===== Helpers =====

#[inline]
fn ascii_eq_ci(a: &[u8], b: &[u8]) -> bool {
    if a.len() != b.len() { return false; }
    for (x, y) in a.iter().zip(b.iter()) {
        if x.to_ascii_lowercase() != y.to_ascii_lowercase() { return false; }
    }
    true
}

#[inline]
fn parse_opcode(name: &[u8]) -> OpCode {
    if ascii_eq_ci(name, b"GET") { OpCode::Get }
    else if ascii_eq_ci(name, b"SET") { OpCode::Set }
    else if ascii_eq_ci(name, b"PING") { OpCode::Ping }
    else if ascii_eq_ci(name, b"MULTI") { OpCode::Multi }
    else if ascii_eq_ci(name, b"EXEC") { OpCode::Exec }
    else if ascii_eq_ci(name, b"DISCARD") { OpCode::Discard }
    else { OpCode::Invalid }
}

/// Parse RESP3 frame into Command
fn parse_resp3(frame: DecodedFrame<BytesFrame>) -> Option<Command> {
    use BytesFrame::*;
    let f = frame.into_complete_frame().ok()?;
    let parts = match f {
        Array { data, .. } => data,
        _ => return None,
    };

    let name = match parts.get(0) {
        Some(BlobString { data, .. }) | Some(SimpleString { data, .. }) => data.as_ref(),
        _ => return None,
    };

    let op = parse_opcode(name);
    match op {
        OpCode::Get => {
            if parts.len() < 2 { return None; }
            let key = match &parts[1] {
                BlobString { data, .. } | SimpleString { data, .. } => Bytes::copy_from_slice(data),
                _ => return None,
            };
            Some(Command { op, key, val: None })
        }
        OpCode::Set => {
            if parts.len() < 3 { return None; }
            let key = match &parts[1] {
                BlobString { data, .. } | SimpleString { data, .. } => Bytes::copy_from_slice(data),
                _ => return None,
            };
            let val = match &parts[2] {
                BlobString { data, .. } | SimpleString { data, .. } => Bytes::copy_from_slice(data),
                _ => return None,
            };
            Some(Command { op, key, val: Some(val) })
        }
        OpCode::Ping | OpCode::Multi | OpCode::Exec | OpCode::Discard => {
            Some(Command { op, key: Bytes::new(), val: None })
        }
        OpCode::Invalid => None,
    }
}

// ===== RESP Writers =====

#[inline]
fn write_simple_ok<W: Write>(w: &mut W) -> std::io::Result<()> {
    w.write_all(b"+OK\r\n")
}

#[inline]
fn write_pong<W: Write>(w: &mut W) -> std::io::Result<()> {
    w.write_all(b"+PONG\r\n")
}

#[inline]
fn write_queued<W: Write>(w: &mut W) -> std::io::Result<()> {
    w.write_all(b"+QUEUED\r\n")
}

#[inline]
fn write_nil_bulk<W: Write>(w: &mut W) -> std::io::Result<()> {
    w.write_all(b"$-1\r\n")
}

#[inline]
fn write_bulk<W: Write>(w: &mut W, data: &[u8]) -> std::io::Result<()> {
    let mut buf = itoa::Buffer::new();
    w.write_all(b"$")?;
    w.write_all(buf.format(data.len()).as_bytes())?;
    w.write_all(b"\r\n")?;
    w.write_all(data)?;
    w.write_all(b"\r\n")
}

#[inline]
fn write_err<W: Write>(w: &mut W, msg: &str) -> std::io::Result<()> {
    w.write_all(b"-ERR ")?;
    w.write_all(msg.as_bytes())?;
    w.write_all(b"\r\n")
}

#[inline]
fn write_array_header<W: Write>(w: &mut W, len: usize) -> std::io::Result<()> {
    let mut buf = itoa::Buffer::new();
    w.write_all(b"*")?;
    w.write_all(buf.format(len).as_bytes())?;
    w.write_all(b"\r\n")
}

// ===== Transaction FFI =====

/// Helper to build TxnOperation array from commands
fn build_txn_ops(commands: &[Command]) -> Vec<TxnOperation> {
    commands.iter().map(|cmd| {
        let (val_ptr, val_len) = if let Some(v) = &cmd.val {
            (v.as_ptr(), v.len())
        } else {
            (std::ptr::null(), 0)
        };
        TxnOperation {
            op: match cmd.op {
                OpCode::Get => TXN_OP_GET,
                OpCode::Set => TXN_OP_SET,
                _ => 0, // Should not happen - filtered before
            },
            key_ptr: cmd.key.as_ptr(),
            key_len: cmd.key.len(),
            val_ptr,
            val_len,
        }
    }).collect()
}

/// Execute a single command as a transaction (for non-MULTI operations)
/// Returns the result directly without array wrapper
fn ffi_execute_single<W: Write>(cmd: &Command, writer: &mut W) -> std::io::Result<()> {
    let ops = build_txn_ops(&[cmd.clone()]);

    let request = TxnRequest {
        num_ops: ops.len(),
        ops: ops.as_ptr(),
    };

    let mut response = TxnResponse {
        transaction_success: false,
        num_results: 0,
        results: std::ptr::null_mut(),
    };

    let call_ok = unsafe { cpp_execute_transaction(&request, &mut response) };

    if !call_ok || !response.transaction_success || response.num_results == 0 {
        unsafe { cpp_free_transaction_response(&mut response) };
        write_err(writer, "backend")?;
        return Ok(());
    }

    let result = unsafe { &*response.results };

    if !result.success {
        write_err(writer, "operation failed")?;
    } else if cmd.op == OpCode::Get {
        if result.data_len > 0 && !result.data_ptr.is_null() {
            let data = unsafe { std::slice::from_raw_parts(result.data_ptr, result.data_len) };
            write_bulk(writer, data)?;
        } else {
            write_nil_bulk(writer)?;
        }
    } else {
        // SET returns OK
        write_simple_ok(writer)?;
    }

    unsafe { cpp_free_transaction_response(&mut response) };
    Ok(())
}

/// Execute buffered commands as a single transaction (for MULTI/EXEC)
/// Returns results wrapped in an array
fn ffi_execute_transaction<W: Write>(commands: &[Command], writer: &mut W) -> std::io::Result<()> {
    if commands.is_empty() {
        // Empty transaction returns empty array
        write_array_header(writer, 0)?;
        return Ok(());
    }

    let ops = build_txn_ops(commands);

    let request = TxnRequest {
        num_ops: ops.len(),
        ops: ops.as_ptr(),
    };

    let mut response = TxnResponse {
        transaction_success: false,
        num_results: 0,
        results: std::ptr::null_mut(),
    };

    let call_ok = unsafe { cpp_execute_transaction(&request, &mut response) };

    if !call_ok || !response.transaction_success {
        // Transaction failed - return nil (EXECABORT equivalent)
        unsafe { cpp_free_transaction_response(&mut response) };
        writer.write_all(b"*-1\r\n")?;
        return Ok(());
    }

    // Write array response with results
    write_array_header(writer, response.num_results)?;

    for i in 0..response.num_results {
        let result = unsafe { &*response.results.add(i) };
        let cmd = &commands[i];

        if !result.success {
            write_err(writer, "operation failed")?;
        } else if cmd.op == OpCode::Get {
            if result.data_len > 0 && !result.data_ptr.is_null() {
                let data = unsafe { std::slice::from_raw_parts(result.data_ptr, result.data_len) };
                write_bulk(writer, data)?;
            } else {
                write_nil_bulk(writer)?;
            }
        } else {
            // SET returns OK
            write_simple_ok(writer)?;
        }
    }

    // Free response resources
    unsafe { cpp_free_transaction_response(&mut response) };

    Ok(())
}

// ===== Server =====

fn create_reuseport_listener(addr: &str) -> std::io::Result<TcpListener> {
    let addr: SocketAddr = addr.parse()
        .map_err(|e| std::io::Error::new(std::io::ErrorKind::InvalidInput, e))?;

    let socket = Socket::new(Domain::IPV4, Type::STREAM, Some(Protocol::TCP))?;
    socket.set_reuse_address(true)?;
    socket.set_reuse_port(true)?;
    socket.set_nonblocking(false)?;
    socket.set_nodelay(true)?;
    socket.bind(&addr.into())?;
    socket.listen(1024)?;

    Ok(TcpListener::from(socket))
}

#[no_mangle]
pub extern "C" fn rust_init(n_threads: usize) -> bool {
    let addr = "127.0.0.1:6380";
    let barrier = Arc::new(Barrier::new(n_threads));
    let ready_count = Arc::new(AtomicUsize::new(0));

    println!("Starting {} thread-per-core workers on {} (SO_REUSEPORT, 100% SYNC, MULTI/EXEC support)",
             n_threads, addr);

    for thread_id in 0..n_threads {
        let barrier = Arc::clone(&barrier);
        let ready_count = Arc::clone(&ready_count);
        let addr = addr.to_string();

        std::thread::Builder::new()
            .name(format!("mako-worker-{}", thread_id))
            .spawn(move || {
                let listener = match create_reuseport_listener(&addr) {
                    Ok(l) => l,
                    Err(e) => {
                        eprintln!("[thread-{}] Failed to create listener: {e}", thread_id);
                        return;
                    }
                };

                unsafe { cpp_worker_thread_init(thread_id); }

                let count = ready_count.fetch_add(1, Ordering::SeqCst) + 1;
                barrier.wait();

                if thread_id == 0 {
                    println!("All {} threads ready, accepting connections on {}", count, addr);
                }

                loop {
                    match listener.accept() {
                        Ok((mut stream, _)) => {
                            let _ = stream.set_nodelay(true);
                            if let Err(e) = handle_client_sync(&mut stream) {
                                eprintln!("Client handling error: {e}");
                            }
                        }
                        Err(e) => {
                            eprintln!("[thread-{}] Accept error: {e}", thread_id);
                        }
                    }
                }
            })
            .expect("Failed to spawn worker thread");
    }

    true
}

/// Handle client with MULTI/EXEC transaction support
fn handle_client_sync(stream: &mut TcpStream) -> std::io::Result<()> {
    let mut resp3 = Resp3Handler::new(10 * 1024 * 1024);
    let mut read_buf = [0u8; 16384];
    let mut writer = BufWriter::with_capacity(16384, stream.try_clone()?);
    let mut txn_state = TransactionState::new();

    loop {
        match stream.read(&mut read_buf) {
            Ok(0) => break,
            Ok(n) => resp3.read_bytes(&read_buf[..n]),
            Err(e) => return Err(e),
        }

        loop {
            match resp3.next_frame() {
                Ok(Some(frame)) => {
                    if let Some(cmd) = parse_resp3(frame) {
                        handle_command(&cmd, &mut txn_state, &mut writer)?;
                    } else {
                        write_err(&mut writer, "unsupported command")?;
                    }
                }
                Ok(None) => break,
                Err(_) => {
                    write_err(&mut writer, "protocol error")?;
                    break;
                }
            }
        }

        writer.flush()?;
    }

    Ok(())
}

/// Handle a single command, respecting transaction state
fn handle_command<W: Write>(
    cmd: &Command,
    txn_state: &mut TransactionState,
    writer: &mut W
) -> std::io::Result<()> {
    match cmd.op {
        OpCode::Ping => {
            // PING is always executed immediately
            write_pong(writer)?;
        }
        OpCode::Multi => {
            if txn_state.in_multi {
                write_err(writer, "MULTI calls can not be nested")?;
            } else {
                txn_state.start_multi();
                write_simple_ok(writer)?;
            }
        }
        OpCode::Exec => {
            if !txn_state.in_multi {
                write_err(writer, "EXEC without MULTI")?;
            } else {
                let commands = txn_state.take_commands();
                ffi_execute_transaction(&commands, writer)?;
            }
        }
        OpCode::Discard => {
            if !txn_state.in_multi {
                write_err(writer, "DISCARD without MULTI")?;
            } else {
                txn_state.discard();
                write_simple_ok(writer)?;
            }
        }
        OpCode::Get | OpCode::Set => {
            if txn_state.in_multi {
                // Queue command for later execution
                txn_state.queue_command(cmd.clone());
                write_queued(writer)?;
            } else {
                // Execute immediately as single-operation transaction
                // Uses ffi_execute_single which returns result without array wrapper
                ffi_execute_single(cmd, writer)?;
            }
        }
        OpCode::Invalid => {
            write_err(writer, "unknown command")?;
        }
    }
    Ok(())
}
