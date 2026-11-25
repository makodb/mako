//! RESP command adapter over native shard transactions. MULTI/EXEC submits one
//! retained transaction, unlike the external auto-commit gateway's sessions.
use crate::{
    client::Client,
    node::{Config, Handle},
    storage::engine,
    wire::*,
};
use std::{
    io::{Read, Write},
    net::{TcpListener, TcpStream},
    os::fd::AsRawFd,
    sync::{
        atomic::{AtomicBool, Ordering},
        Arc,
    },
    time::Duration,
};
mod records;

const MAX_BUFFER: usize = 64 * 1024 * 1024;
const MAX_COMMANDS: usize = 1024;
const MAX_CONNECTIONS: usize = 64;

#[derive(Clone, Debug)]
enum Command {
    Get(Vec<u8>),
    Set(Vec<u8>, Vec<u8>),
    Delete(Vec<u8>),
}
impl Command {
    fn bytes(&self) -> usize {
        match self {
            Self::Get(key) | Self::Delete(key) => key.len(),
            Self::Set(key, value) => key.len() + value.len(),
        }
    }
}
struct Connection {
    stream: TcpStream,
    input: Vec<u8>,
    output: Vec<u8>,
    sent: usize,
    multi: bool,
    failed: bool,
    queued: Vec<Command>,
    queued_bytes: usize,
    buffered: bool,
    eof: bool,
}
impl Connection {
    fn error(&mut self, message: &str) {
        self.output.extend_from_slice(b"-ERR ");
        self.output.extend_from_slice(message.as_bytes());
        self.output.extend_from_slice(b"\r\n");
    }
    fn reset(&mut self) {
        self.multi = false;
        self.failed = false;
        self.queued.clear();
        self.queued_bytes = 0;
    }
}
fn bulk(output: &mut Vec<u8>, value: Option<&[u8]>) {
    match value {
        None => output.extend_from_slice(b"$-1\r\n"),
        Some(value) => {
            output.extend_from_slice(format!("${}\r\n", value.len()).as_bytes());
            output.extend_from_slice(value);
            output.extend_from_slice(b"\r\n");
        }
    }
}

// Accept the array-of-strings commands supported by the previous RESP server.
// Parse counts before allocation and never recurse into arbitrary RESP values.
type Parsed = Option<(Vec<Vec<u8>>, usize)>;
fn frame(bytes: &[u8]) -> std::result::Result<Parsed, &'static str> {
    fn line(
        bytes: &[u8],
        start: usize,
    ) -> std::result::Result<Option<(&[u8], usize)>, &'static str> {
        let rest = &bytes[start..];
        if let Some(end) = rest.windows(2).position(|pair| pair == b"\r\n") {
            Ok(Some((&rest[..end], start + end + 2)))
        } else if rest.len() > records::MAX_VALUE {
            Err("RESP line too long")
        } else {
            Ok(None)
        }
    }
    fn number(bytes: &[u8]) -> std::result::Result<usize, &'static str> {
        if bytes.is_empty() || bytes.iter().any(|b| !b.is_ascii_digit()) {
            return Err("invalid RESP length");
        }
        std::str::from_utf8(bytes)
            .ok()
            .and_then(|text| text.parse().ok())
            .ok_or("invalid RESP length")
    }
    if bytes.is_empty() {
        return Ok(None);
    }
    if bytes[0] != b'*' {
        return Err("expected a RESP command array");
    }
    let Some((count, mut at)) = line(bytes, 1)? else {
        return Ok(None);
    };
    let count = number(count)?;
    // Parse bounded unknown commands too. Clients send four-argument
    // CLIENT SETINFO handshakes and accept an error reply for that optional
    // metadata; closing the socket here prevents them from reaching PING.
    if !(1..=64).contains(&count) {
        return Err("command must have 1..64 arguments");
    }
    let mut values = Vec::with_capacity(count);
    for _ in 0..count {
        if at == bytes.len() {
            return Ok(None);
        }
        let kind = bytes[at];
        if kind != b'$' && kind != b'+' {
            return Err("command arguments must be strings");
        }
        let Some((value, next)) = line(bytes, at + 1)? else {
            return Ok(None);
        };
        if kind == b'+' {
            values.push(value.to_vec());
            at = next;
            continue;
        }
        let length = number(value)?;
        if length > records::MAX_VALUE {
            return Err("value exceeds storage limit");
        }
        let end = next + length;
        if bytes.len() < end + 2 {
            return Ok(None);
        }
        if &bytes[end..end + 2] != b"\r\n" {
            return Err("invalid bulk string terminator");
        }
        values.push(bytes[next..end].to_vec());
        at = end + 2;
    }
    Ok(Some((values, at)))
}

/// Stable byte hash chooses the initial warehouse. Migration continues to use
/// the shared schema's current route for that warehouse.
pub fn owner(key: &[u8], nodes: u32) -> u32 {
    let hash = key.iter().fold(0xcbf29ce484222325u64, |hash, byte| {
        (hash ^ u64::from(*byte)).wrapping_mul(0x100000001b3)
    });
    (hash % u64::from(nodes)) as u32
}
struct Worker<'a> {
    host: &'a Handle,
    config: &'a Config,
    client: Client,
    worker: mako_storage_api::Worker,
}
struct TransactionRows<'a, 'client, 'worker> {
    transaction: &'a mut crate::client::Transaction<'client, 'worker>,
    target: &'a Target,
}
impl records::Rows for TransactionRows<'_, '_, '_> {
    fn get(&mut self, key: &[u8]) -> Result<Option<Vec<u8>>> {
        self.transaction.get(self.target, key)
    }
    fn put(&mut self, key: &[u8], value: &[u8]) -> Result<()> {
        self.transaction
            .mutate(self.target, Mutation::Put, key, value)
    }
    fn remove(&mut self, key: &[u8]) -> Result<()> {
        self.transaction
            .mutate(self.target, Mutation::Remove, key, b"")
    }
}
impl Worker<'_> {
    fn execute(&mut self, commands: &[Command]) -> Result<Vec<u8>> {
        // Durable redo has a bounded frame. Reject oversized writes before
        // installation, leaving room for row/chunk metadata in every shard.
        if (self.config.replication.is_some()
            || self.config.persistence.is_some()
            || !self.config.replica_groups.is_empty())
            && commands.iter().map(Command::bytes).sum::<usize>() > MAX_BYTES / 2
        {
            return Err(Error::new(
                ErrorCode::Capacity,
                "Redis durable transaction byte limit",
            ));
        }
        self.client
            .run(self.host.store.clone(), &mut self.worker, |tx| {
                let mut output = Vec::new();
                for command in commands {
                    let key = match command {
                        Command::Get(key) | Command::Set(key, _) | Command::Delete(key) => key,
                    };
                    let warehouse = owner(key, self.config.nodes) + 1;
                    let target = self.host.schema.target(
                        TableKey {
                            table: 1,
                            warehouse,
                        },
                        key,
                        self.config.owner,
                        self.config.migration,
                    )?;
                    let mut rows = TransactionRows {
                        transaction: tx,
                        target: &target,
                    };
                    match command {
                        Command::Get(key) => {
                            bulk(&mut output, records::get(&mut rows, key)?.as_deref())
                        }
                        Command::Set(key, value) => {
                            records::set(&mut rows, key, Some(value))?;
                            output.extend_from_slice(b"+OK\r\n");
                        }
                        Command::Delete(key) => {
                            let found = records::set(&mut rows, key, None)?;
                            output.extend_from_slice(if found { b":1\r\n" } else { b":0\r\n" });
                        }
                    }
                    if output.len() > MAX_BUFFER {
                        return Err(Error::new(
                            ErrorCode::Capacity,
                            "transaction reply too large",
                        ));
                    }
                }
                Ok(output)
            })
    }
    fn reply(
        &mut self,
        connection: &mut Connection,
        commands: &[Command],
        array: bool,
    ) -> Result<()> {
        match self.execute(commands) {
            Ok(bytes) => {
                if array {
                    connection
                        .output
                        .extend_from_slice(format!("*{}\r\n", commands.len()).as_bytes());
                }
                connection.output.extend(bytes);
                Ok(())
            }
            Err(error)
                if matches!(
                    error.code,
                    ErrorCode::Conflict | ErrorCode::Busy | ErrorCode::RouteChanged
                ) =>
            {
                if array {
                    connection.output.extend_from_slice(b"*-1\r\n");
                } else {
                    connection
                        .output
                        .extend_from_slice(b"-TRYAGAIN transaction aborted\r\n");
                }
                Ok(())
            }
            Err(error) if matches!(error.code, ErrorCode::Capacity | ErrorCode::Invalid) => {
                connection.error("transaction exceeds storage limits");
                Ok(())
            }
            // Uncertain cleanup cannot be turned into a normal abort or reused
            // by another socket on this worker. Stop the serving process.
            Err(error) => Err(error),
        }
    }
    fn command(&mut self, connection: &mut Connection, args: Vec<Vec<u8>>) -> Result<()> {
        let name = args[0].as_slice();
        if name.eq_ignore_ascii_case(b"PING") && args.len() <= 2 {
            if args.len() == 2 {
                bulk(&mut connection.output, Some(&args[1]));
            } else {
                connection.output.extend_from_slice(b"+PONG\r\n");
            }
        } else if name.eq_ignore_ascii_case(b"MULTI") && args.len() == 1 {
            if connection.multi {
                connection.error("MULTI calls cannot be nested");
            } else {
                connection.multi = true;
                connection.output.extend_from_slice(b"+OK\r\n");
            }
        } else if name.eq_ignore_ascii_case(b"DISCARD") && args.len() == 1 {
            if !connection.multi {
                connection.error("DISCARD without MULTI");
            } else {
                connection.reset();
                connection.output.extend_from_slice(b"+OK\r\n");
            }
        } else if name.eq_ignore_ascii_case(b"EXEC") && args.len() == 1 {
            if !connection.multi {
                connection.error("EXEC without MULTI");
            } else if connection.failed {
                connection.reset();
                connection.error("EXECABORT invalid queued command");
            } else {
                let commands = std::mem::take(&mut connection.queued);
                connection.reset();
                self.reply(connection, &commands, true)?;
            }
        } else {
            let command = if name.eq_ignore_ascii_case(b"GET") && args.len() == 2 {
                Some(Command::Get(args[1].clone()))
            } else if name.eq_ignore_ascii_case(b"DEL") && args.len() == 2 {
                Some(Command::Delete(args[1].clone()))
            } else if name.eq_ignore_ascii_case(b"SET") && args.len() == 3 {
                Some(Command::Set(args[1].clone(), args[2].clone()))
            } else {
                None
            };
            if args.get(1).is_some_and(|key| key.len() > records::MAX_KEY) || command.is_none() {
                connection.failed |= connection.multi;
                connection.error("unsupported command, arguments or key length");
            } else if let Some(command) = command {
                if connection.multi {
                    if connection.queued.len() == MAX_COMMANDS
                        || connection.queued_bytes + command.bytes() > MAX_BUFFER
                    {
                        connection.failed = true;
                        connection.error("transaction queue limit");
                    } else {
                        connection.queued_bytes += command.bytes();
                        connection.queued.push(command);
                        connection.output.extend_from_slice(b"+QUEUED\r\n");
                    }
                } else {
                    self.reply(connection, &[command], false)?;
                }
            }
        }
        Ok(())
    }
}

pub fn serve(host: &Handle, config: &Config, listen: &str, stop: Arc<AtomicBool>) -> Result<()> {
    if config.tables != crate::transactions::tables() || config.warehouses_per_shard != 1 {
        return Err(Error::new(
            ErrorCode::Invalid,
            "Redis requires the records warehouse schema",
        ));
    }
    let listener = TcpListener::bind(listen).map_err(io_error)?;
    listener.set_nonblocking(true).map_err(io_error)?;
    let address = listener.local_addr().map_err(io_error)?;
    std::thread::scope(|scope| {
        let (ready, initialized) = std::sync::mpsc::channel();
        let mut workers = Vec::new();
        for index in 0..config.workers {
            let (listener, stop, ready) = (&listener, &stop, ready.clone());
            workers.push(scope.spawn(move || {
                let work = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                    let mut client = Client::new(
                        host.network.clone(),
                        config.peers.clone(),
                        Stream {
                            node: config.owner,
                            worker: index,
                        },
                        0,
                        Duration::from_secs(10),
                    )?;
                    for (owner, replicas) in &config.replica_groups {
                        client.set_replicas(*owner, replicas.clone())?;
                    }
                    let mut worker = Worker {
                        host,
                        config,
                        client,
                        worker: host.store.database.worker().map_err(engine)?,
                    };
                    let _ = ready.send(());
                    drop(ready);
                    connections(listener, &mut worker, stop)
                }))
                .unwrap_or_else(|_| Err(Error::new(ErrorCode::Fatal, "Redis worker panicked")));
                if work.is_err() {
                    stop.store(true, Ordering::Relaxed);
                }
                work
            }));
        }
        drop(ready);
        if initialized.iter().count() == config.workers as usize {
            println!("MAKO_REDIS_READY {address}");
            let _ = std::io::stdout().flush();
        }
        let mut failure = None;
        for worker in workers {
            if let Err(error) = worker
                .join()
                .unwrap_or_else(|_| Err(Error::new(ErrorCode::Fatal, "Redis worker panicked")))
            {
                failure.get_or_insert(error);
            }
        }
        failure.map_or(Ok(()), Err)
    })
}
fn io_error(error: std::io::Error) -> Error {
    Error::new(ErrorCode::Transport, error.to_string())
}
fn connections(listener: &TcpListener, worker: &mut Worker<'_>, stop: &AtomicBool) -> Result<()> {
    let mut connections: Vec<Connection> = Vec::new();
    while !stop.load(Ordering::Relaxed) {
        worker.host.store.log.health()?;
        let mut fds = Vec::with_capacity(connections.len() + 1);
        fds.push(libc::pollfd {
            fd: listener.as_raw_fd(),
            events: if connections.len() < MAX_CONNECTIONS {
                libc::POLLIN
            } else {
                0
            },
            revents: 0,
        });
        for connection in &connections {
            fds.push(libc::pollfd {
                fd: connection.stream.as_raw_fd(),
                events: libc::POLLIN
                    | if connection.sent < connection.output.len() {
                        libc::POLLOUT
                    } else {
                        0
                    },
                revents: 0,
            });
        }
        let buffered = connections
            .iter()
            .any(|connection| connection.buffered && connection.output.len() <= MAX_BUFFER);
        // SAFETY: poll receives an initialized contiguous array of live fds and
        // can only write their revents fields until this synchronous call ends.
        let count = unsafe {
            libc::poll(
                fds.as_mut_ptr(),
                fds.len() as libc::nfds_t,
                if buffered { 1 } else { 100 },
            )
        };
        if count < 0 {
            let error = std::io::Error::last_os_error();
            if error.kind() == std::io::ErrorKind::Interrupted {
                continue;
            }
            return Err(io_error(error));
        }
        if fds[0].revents & libc::POLLIN != 0 && connections.len() < MAX_CONNECTIONS {
            match listener.accept() {
                Ok((stream, _)) => {
                    stream.set_nonblocking(true).map_err(io_error)?;
                    stream.set_nodelay(true).map_err(io_error)?;
                    connections.push(Connection {
                        stream,
                        input: Vec::new(),
                        output: Vec::new(),
                        sent: 0,
                        multi: false,
                        failed: false,
                        queued: Vec::new(),
                        queued_bytes: 0,
                        buffered: false,
                        eof: false,
                    });
                }
                Err(error)
                    if matches!(
                        error.kind(),
                        std::io::ErrorKind::WouldBlock | std::io::ErrorKind::Interrupted
                    ) => {}
                Err(error) => return Err(io_error(error)),
            }
        }
        let mut at = 0;
        while at < connections.len() {
            if service(&mut connections[at], worker)? {
                at += 1;
            } else {
                connections.swap_remove(at);
            }
        }
    }
    Ok(())
}
fn service(connection: &mut Connection, worker: &mut Worker<'_>) -> Result<bool> {
    let mut bytes = [0; 16384];
    match connection.stream.read(&mut bytes) {
        Ok(0) => connection.eof = true,
        Ok(count) => connection.input.extend_from_slice(&bytes[..count]),
        Err(error)
            if matches!(
                error.kind(),
                std::io::ErrorKind::WouldBlock | std::io::ErrorKind::Interrupted
            ) => {}
        Err(_) => return Ok(false),
    }
    if connection.input.len() > MAX_BUFFER {
        return Ok(false);
    }
    connection.buffered = !connection.input.is_empty();
    for _ in 0..32 {
        if connection.output.len() > MAX_BUFFER {
            break;
        }
        match frame(&connection.input) {
            Ok(Some((args, consumed))) => {
                connection.input.drain(..consumed);
                worker.command(connection, args)?;
            }
            Ok(None) => {
                connection.buffered = false;
                break;
            }
            Err(_) => return Ok(false),
        }
    }
    while connection.sent < connection.output.len() {
        match connection
            .stream
            .write(&connection.output[connection.sent..])
        {
            Ok(0) => return Ok(false),
            Ok(count) => connection.sent += count,
            Err(error)
                if matches!(
                    error.kind(),
                    std::io::ErrorKind::WouldBlock | std::io::ErrorKind::Interrupted
                ) =>
            {
                break
            }
            Err(_) => return Ok(false),
        }
    }
    if connection.sent == connection.output.len() {
        connection.output.clear();
        connection.sent = 0;
    }
    Ok(!(connection.eof && !connection.buffered && connection.sent == connection.output.len()))
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn resp_bounds_binary_fragmentation_and_pipeline() {
        let bytes = b"*3\r\n$3\r\nSET\r\n$3\r\na\0b\r\n$0\r\n\r\n";
        for end in 0..bytes.len() {
            assert!(frame(&bytes[..end]).unwrap().is_none());
        }
        let (values, consumed) = frame(&[bytes.as_slice(), b"*1\r\n+PING\r\n"].concat())
            .unwrap()
            .unwrap();
        assert_eq!(consumed, bytes.len());
        assert_eq!(values, [b"SET".to_vec(), b"a\0b".to_vec(), vec![]]);
        for bad in [
            b"*9999999999999999999999999\r\n".as_slice(),
            b"*2\r\n*2\r\n",
            b"*2\r\n$99999999\r\n",
            b"*1\r\n$1\r\na!!",
        ] {
            assert!(frame(bad).is_err());
        }
    }
}
