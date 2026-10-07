//! Lua scripting: EVAL, EVALSHA, EVAL_RO, EVALSHA_RO and SCRIPT.
//!
//! # How a script runs
//!
//! `EVAL` runs on the worker thread that read the frame, inside one interactive
//! transaction (`SessionTxn`, see the FFI contract in
//! `include/transaction_ffi.h`). The session declares the script's `KEYS`, so
//! two scripts naming the same key are serialized by the key-stripe lock; a key
//! a script touches without declaring it is still correct but is protected only
//! by optimistic concurrency.
//!
//! It has to be that thread: the storage transaction lives in thread-local STO
//! state, and a thread the server never initialised cannot run a transaction at
//! all. So, like Redis, a script that runs past `lua-time-limit` answers the
//! other connections from inside its own busy hook -- see the pump below.
//!
//! Every `redis.call` goes through the ordinary command parser: the Lua
//! arguments are assembled into a RESP array and handed to `parse_resp3`, so a
//! command inside a script is parsed, validated, key-prefixed for the
//! connection's logical database and formatted exactly as it would be if the
//! client had sent it. The reply comes back as RESP2 bytes and is converted to
//! a Lua value, so there is one implementation of every command and one
//! implementation of every reply shape.
//!
//! The script sees its own writes because the session buffers them (that is the
//! whole reason the interactive FFI exists), and nothing it wrote is visible to
//! anyone else until the session commits after the script returns.
//!
//! # Retries
//!
//! A commit that loses an optimistic-concurrency race throws away everything
//! the script produced and runs the *whole script* again in a fresh session,
//! up to `TXN_MAX_ATTEMPTS` times, after which the client gets `ERR backend`
//! like the batch path. Redis requires scripts to be deterministic, so a
//! re-run is not observable -- but a script that is not deterministic (one that
//! calls `TIME`, or writes a value derived from a random source) can observe
//! that it ran more than once.
//!
//! # Lua state
//!
//! A fresh `mlua` state per call, not a cached per-worker state. A fresh state
//! is unconditionally clean: no global a previous script defined can leak into
//! the next one, which is the property a cached state would need a sandboxed
//! global table to emulate. The cost is one Lua state construction per EVAL.

use super::*;

use mlua::{Lua, MultiValue, Value, Variadic};
use std::cell::Cell;
use std::cell::RefCell;
use std::rc::Rc;
use std::time::Instant;

/// The Redis version this adapter reports to scripts.
const SCRIPT_REDIS_VERSION: &str = "7.4.0";
const SCRIPT_REDIS_VERSION_NUM: i64 = 0x0007_0400;

/// Instructions between two checks of the kill flag and the time limit.
const LUA_HOOK_INTERVAL: u32 = 100_000;

/// `lua-time-limit` / `busy-reply-threshold`, in milliseconds: how long a
/// script may run before other connections start getting BUSY.
static LUA_TIME_LIMIT_MS: AtomicUsize = AtomicUsize::new(5000);

/// Scripts executing right now, and how many of them have already written.
/// Redis runs one script at a time; this adapter can run one per worker, so
/// SCRIPT KILL asks about the set of them: it is refused as UNKILLABLE while
/// any running script has written, and it kills every running script that has
/// not.
static SCRIPTS_RUNNING: AtomicUsize = AtomicUsize::new(0);
static SCRIPTS_WRITTEN: AtomicUsize = AtomicUsize::new(0);
static SCRIPT_KILL_REQUESTED: AtomicUsize = AtomicUsize::new(0);

/// SHA1 -> source, shared by every worker. Redis's script cache is per server
/// and survives everything but SCRIPT FLUSH, so this one does too.
static SCRIPT_CACHE: OnceLock<Mutex<HashMap<String, Bytes>>> = OnceLock::new();

fn script_cache() -> &'static Mutex<HashMap<String, Bytes>> {
    SCRIPT_CACHE.get_or_init(|| Mutex::new(HashMap::new()))
}

pub(crate) fn lua_time_limit_ms() -> usize {
    LUA_TIME_LIMIT_MS.load(Ordering::Relaxed)
}

pub(crate) fn set_lua_time_limit_ms(value: usize) {
    LUA_TIME_LIMIT_MS.store(value, Ordering::Relaxed);
}

// ===== SHA1 =====

/// SHA1 of `data` as lowercase hex, which is how Redis names a cached script.
pub(crate) fn sha1_hex(data: &[u8]) -> String {
    let mut h: [u32; 5] = [0x6745_2301, 0xEFCD_AB89, 0x98BA_DCFE, 0x1032_5476, 0xC3D2_E1F0];
    let mut message = data.to_vec();
    let bit_len = (data.len() as u64).wrapping_mul(8);
    message.push(0x80);
    while message.len() % 64 != 56 {
        message.push(0);
    }
    message.extend_from_slice(&bit_len.to_be_bytes());

    let mut w = [0u32; 80];
    for chunk in message.chunks_exact(64) {
        for (index, word) in chunk.chunks_exact(4).enumerate() {
            w[index] = u32::from_be_bytes([word[0], word[1], word[2], word[3]]);
        }
        for index in 16..80 {
            w[index] = (w[index - 3] ^ w[index - 8] ^ w[index - 14] ^ w[index - 16]).rotate_left(1);
        }
        let (mut a, mut b, mut c, mut d, mut e) = (h[0], h[1], h[2], h[3], h[4]);
        for (index, word) in w.iter().enumerate() {
            let (f, k) = match index {
                0..=19 => ((b & c) | ((!b) & d), 0x5A82_7999u32),
                20..=39 => (b ^ c ^ d, 0x6ED9_EBA1),
                40..=59 => ((b & c) | (b & d) | (c & d), 0x8F1B_BCDC),
                _ => (b ^ c ^ d, 0xCA62_C1D6),
            };
            let temp = a
                .rotate_left(5)
                .wrapping_add(f)
                .wrapping_add(e)
                .wrapping_add(k)
                .wrapping_add(*word);
            e = d;
            d = c;
            c = b.rotate_left(30);
            b = a;
            a = temp;
        }
        h[0] = h[0].wrapping_add(a);
        h[1] = h[1].wrapping_add(b);
        h[2] = h[2].wrapping_add(c);
        h[3] = h[3].wrapping_add(d);
        h[4] = h[4].wrapping_add(e);
    }

    let mut out = String::with_capacity(40);
    for word in h {
        out.push_str(&format!("{:08x}", word));
    }
    out
}

// ===== RESP2 replies, as Lua sees them =====

/// One decoded RESP2 reply. Scripts always see RESP2, which is what Redis does
/// until a script calls `redis.setresp(3)`.
enum Resp2 {
    Status(Vec<u8>),
    Error(String),
    Integer(i64),
    Bulk(Option<Vec<u8>>),
    Array(Option<Vec<Resp2>>),
}

fn find_crlf(buf: &[u8], from: usize) -> Option<usize> {
    let mut index = from;
    while index + 1 < buf.len() {
        if buf[index] == b'\r' && buf[index + 1] == b'\n' {
            return Some(index);
        }
        index += 1;
    }
    None
}

fn parse_resp2(buf: &[u8], pos: usize) -> Option<(Resp2, usize)> {
    let marker = *buf.get(pos)?;
    let line_end = find_crlf(buf, pos + 1)?;
    let line = &buf[pos + 1..line_end];
    let next = line_end + 2;
    match marker {
        b'+' => Some((Resp2::Status(line.to_vec()), next)),
        b'-' => Some((
            Resp2::Error(String::from_utf8_lossy(line).into_owned()),
            next,
        )),
        b':' => {
            let value = std::str::from_utf8(line).ok()?.parse::<i64>().ok()?;
            Some((Resp2::Integer(value), next))
        }
        b'$' => {
            let len = std::str::from_utf8(line).ok()?.parse::<i64>().ok()?;
            if len < 0 {
                return Some((Resp2::Bulk(None), next));
            }
            let len = len as usize;
            let data = buf.get(next..next + len)?.to_vec();
            Some((Resp2::Bulk(Some(data)), next + len + 2))
        }
        b'*' => {
            let len = std::str::from_utf8(line).ok()?.parse::<i64>().ok()?;
            if len < 0 {
                return Some((Resp2::Array(None), next));
            }
            let mut items = Vec::with_capacity(len as usize);
            let mut cursor = next;
            for _ in 0..len {
                let (item, after) = parse_resp2(buf, cursor)?;
                items.push(item);
                cursor = after;
            }
            Some((Resp2::Array(Some(items)), cursor))
        }
        // RESP3 null, which write_null can emit for a protocol-3 connection.
        b'_' => Some((Resp2::Bulk(None), next)),
        _ => None,
    }
}

/// RESP -> Lua, by Redis's rules: integer to number, bulk to string, nil to
/// false, array to table, status to `{ok=...}`, error to `{err=...}`.
fn resp_to_lua(lua: &Lua, reply: Resp2) -> mlua::Result<Value<'_>> {
    Ok(match reply {
        Resp2::Integer(value) => Value::Integer(value),
        Resp2::Bulk(None) | Resp2::Array(None) => Value::Boolean(false),
        Resp2::Bulk(Some(data)) => Value::String(lua.create_string(&data)?),
        Resp2::Status(data) => {
            let table = lua.create_table()?;
            table.set("ok", lua.create_string(&data)?)?;
            Value::Table(table)
        }
        Resp2::Error(message) => {
            let table = lua.create_table()?;
            table.set("err", message)?;
            Value::Table(table)
        }
        Resp2::Array(Some(items)) => {
            let table = lua.create_table()?;
            for (index, item) in items.into_iter().enumerate() {
                table.set(index + 1, resp_to_lua(lua, item)?)?;
            }
            Value::Table(table)
        }
    })
}

/// Lua -> RESP, by Redis's rules: number truncated to an integer, string to
/// bulk, false to nil, true to 1, `{ok=...}` to a status, `{err=...}` to an
/// error, and any other table to an array that stops at the first nil.
fn write_lua_value<W: Write>(
    value: &Value,
    protocol_version: u8,
    writer: &mut W,
) -> std::io::Result<()> {
    write_lua_value_at(value, protocol_version, writer, 0)
}

/// How deep a returned table may nest before the reply is cut off. A script
/// can build a cycle (`local a = {} local b = {a} a[1] = b return a`), so the
/// conversion has to stop on its own; Redis stops at its Lua stack limit and
/// says so, and the same text ends the reply here.
const LUA_REPLY_MAX_DEPTH: usize = 128;

fn write_lua_value_at<W: Write>(
    value: &Value,
    protocol_version: u8,
    writer: &mut W,
    depth: usize,
) -> std::io::Result<()> {
    if depth >= LUA_REPLY_MAX_DEPTH {
        return writer.write_all(b"-ERR reached lua stack limit\r\n");
    }
    match value {
        Value::Nil => write_null(writer, protocol_version),
        Value::Boolean(false) => write_null(writer, protocol_version),
        Value::Boolean(true) => write_integer(writer, 1),
        Value::Integer(value) => write_integer(writer, *value),
        Value::Number(value) => write_integer(writer, *value as i64),
        Value::String(text) => write_bulk(writer, &text.as_bytes()),
        Value::Table(table) => {
            // Raw access throughout: Redis converts a returned table with
            // lua_rawgeti/lua_rawget, so a metatable's __index is not invoked
            // and cannot run commands from inside the conversion.
            if let Ok(Value::String(message)) = table.raw_get::<_, Value>("err") {
                let text = String::from_utf8_lossy(&message.as_bytes()).replace(['\r', '\n'], " ");
                writer.write_all(b"-")?;
                writer.write_all(text.as_bytes())?;
                return writer.write_all(b"\r\n");
            }
            if let Ok(Value::String(message)) = table.raw_get::<_, Value>("ok") {
                let text = String::from_utf8_lossy(&message.as_bytes()).replace(['\r', '\n'], " ");
                return write_simple_string(writer, &text);
            }
            let mut items = Vec::new();
            for index in 1.. {
                match table.raw_get::<_, Value>(index) {
                    Ok(Value::Nil) | Err(_) => break,
                    Ok(item) => items.push(item),
                }
            }
            write_array_header(writer, items.len())?;
            for item in &items {
                write_lua_value_at(item, protocol_version, writer, depth + 1)?;
            }
            Ok(())
        }
        // Functions, userdata and the rest have no Redis reply; Redis answers
        // nil for anything it cannot convert.
        _ => write_null(writer, protocol_version),
    }
}

// ===== cjson =====
//
// A small JSON codec so `cjson.encode` / `cjson.decode` exist. Limits, all
// recorded in known_divergences.txt: JSON null decodes to Lua nil (real cjson
// uses a lightuserdata sentinel), numbers decode as Lua numbers so very large
// integers lose precision the same way Lua 5.1 doubles do, an empty Lua table
// encodes as `{}`, and a table with both array and hash parts encodes as an
// object.

fn json_escape(text: &[u8], out: &mut String) {
    out.push('"');
    for &byte in text {
        match byte {
            b'"' => out.push_str("\\\""),
            b'\\' => out.push_str("\\\\"),
            b'\n' => out.push_str("\\n"),
            b'\r' => out.push_str("\\r"),
            b'\t' => out.push_str("\\t"),
            0x00..=0x1f => out.push_str(&format!("\\u{:04x}", byte)),
            _ => out.push(byte as char),
        }
    }
    out.push('"');
}

fn json_encode_value(value: &Value, out: &mut String, depth: usize) -> mlua::Result<()> {
    if depth > 64 {
        return Err(mlua::Error::RuntimeError(
            "Cannot serialise, excessive nesting".to_string(),
        ));
    }
    match value {
        Value::Nil => out.push_str("null"),
        Value::Boolean(true) => out.push_str("true"),
        Value::Boolean(false) => out.push_str("false"),
        Value::Integer(value) => out.push_str(&value.to_string()),
        Value::Number(value) => {
            if value.fract() == 0.0 && value.abs() < 1e15 {
                out.push_str(&(*value as i64).to_string());
            } else {
                out.push_str(&value.to_string());
            }
        }
        Value::String(text) => json_escape(&text.as_bytes(), out),
        Value::Table(table) => {
            let len = table.raw_len();
            let mut pairs = Vec::new();
            for pair in table.clone().pairs::<Value, Value>() {
                pairs.push(pair?);
            }
            let is_array = len > 0 && pairs.len() == len;
            if is_array {
                out.push('[');
                for index in 1..=len {
                    if index > 1 {
                        out.push(',');
                    }
                    json_encode_value(&table.get::<_, Value>(index)?, out, depth + 1)?;
                }
                out.push(']');
            } else {
                out.push('{');
                let mut first = true;
                for (key, item) in pairs {
                    let name = match key {
                        Value::String(text) => String::from_utf8_lossy(&text.as_bytes()).into_owned(),
                        Value::Integer(value) => value.to_string(),
                        Value::Number(value) => value.to_string(),
                        _ => continue,
                    };
                    if !first {
                        out.push(',');
                    }
                    first = false;
                    json_escape(name.as_bytes(), out);
                    out.push(':');
                    json_encode_value(&item, out, depth + 1)?;
                }
                out.push('}');
            }
        }
        _ => {
            return Err(mlua::Error::RuntimeError(
                "Cannot serialise, unsupported type".to_string(),
            ))
        }
    }
    Ok(())
}

struct JsonParser<'a> {
    input: &'a [u8],
    pos: usize,
}

impl<'a> JsonParser<'a> {
    fn skip_space(&mut self) {
        while self
            .input
            .get(self.pos)
            .is_some_and(|byte| byte.is_ascii_whitespace())
        {
            self.pos += 1;
        }
    }

    fn fail<T>(&self) -> mlua::Result<T> {
        Err(mlua::Error::RuntimeError(format!(
            "Expected value but found invalid token at character {}",
            self.pos + 1
        )))
    }

    fn parse<'lua>(&mut self, lua: &'lua Lua, depth: usize) -> mlua::Result<Value<'lua>> {
        if depth > 64 {
            return self.fail();
        }
        self.skip_space();
        let Some(&byte) = self.input.get(self.pos) else {
            return self.fail();
        };
        match byte {
            b'n' => {
                if self.input[self.pos..].starts_with(b"null") {
                    self.pos += 4;
                    Ok(Value::Nil)
                } else {
                    self.fail()
                }
            }
            b't' => {
                if self.input[self.pos..].starts_with(b"true") {
                    self.pos += 4;
                    Ok(Value::Boolean(true))
                } else {
                    self.fail()
                }
            }
            b'f' => {
                if self.input[self.pos..].starts_with(b"false") {
                    self.pos += 5;
                    Ok(Value::Boolean(false))
                } else {
                    self.fail()
                }
            }
            b'"' => {
                let text = self.parse_string()?;
                Ok(Value::String(lua.create_string(&text)?))
            }
            b'[' => {
                self.pos += 1;
                let table = lua.create_table()?;
                let mut index = 1;
                loop {
                    self.skip_space();
                    if self.input.get(self.pos) == Some(&b']') {
                        self.pos += 1;
                        break;
                    }
                    let item = self.parse(lua, depth + 1)?;
                    table.set(index, item)?;
                    index += 1;
                    self.skip_space();
                    match self.input.get(self.pos) {
                        Some(&b',') => self.pos += 1,
                        Some(&b']') => {
                            self.pos += 1;
                            break;
                        }
                        _ => return self.fail(),
                    }
                }
                Ok(Value::Table(table))
            }
            b'{' => {
                self.pos += 1;
                let table = lua.create_table()?;
                loop {
                    self.skip_space();
                    if self.input.get(self.pos) == Some(&b'}') {
                        self.pos += 1;
                        break;
                    }
                    let name = self.parse_string()?;
                    self.skip_space();
                    if self.input.get(self.pos) != Some(&b':') {
                        return self.fail();
                    }
                    self.pos += 1;
                    let item = self.parse(lua, depth + 1)?;
                    table.set(lua.create_string(&name)?, item)?;
                    self.skip_space();
                    match self.input.get(self.pos) {
                        Some(&b',') => self.pos += 1,
                        Some(&b'}') => {
                            self.pos += 1;
                            break;
                        }
                        _ => return self.fail(),
                    }
                }
                Ok(Value::Table(table))
            }
            _ => {
                let start = self.pos;
                while self.input.get(self.pos).is_some_and(|byte| {
                    byte.is_ascii_digit() || matches!(byte, b'-' | b'+' | b'.' | b'e' | b'E')
                }) {
                    self.pos += 1;
                }
                let text = std::str::from_utf8(&self.input[start..self.pos]).unwrap_or("");
                match text.parse::<f64>() {
                    Ok(value) if value.fract() == 0.0 && value.abs() < 9e15 => {
                        Ok(Value::Integer(value as i64))
                    }
                    Ok(value) => Ok(Value::Number(value)),
                    Err(_) => self.fail(),
                }
            }
        }
    }

    fn parse_string(&mut self) -> mlua::Result<Vec<u8>> {
        self.skip_space();
        if self.input.get(self.pos) != Some(&b'"') {
            return self.fail();
        }
        self.pos += 1;
        let mut out = Vec::new();
        loop {
            let Some(&byte) = self.input.get(self.pos) else {
                return self.fail();
            };
            self.pos += 1;
            match byte {
                b'"' => return Ok(out),
                b'\\' => {
                    let Some(&escape) = self.input.get(self.pos) else {
                        return self.fail();
                    };
                    self.pos += 1;
                    match escape {
                        b'n' => out.push(b'\n'),
                        b'r' => out.push(b'\r'),
                        b't' => out.push(b'\t'),
                        b'b' => out.push(0x08),
                        b'f' => out.push(0x0c),
                        b'u' => {
                            let hex = self
                                .input
                                .get(self.pos..self.pos + 4)
                                .and_then(|raw| std::str::from_utf8(raw).ok())
                                .and_then(|raw| u32::from_str_radix(raw, 16).ok());
                            let Some(code) = hex else {
                                return self.fail();
                            };
                            self.pos += 4;
                            let mut buf = [0u8; 4];
                            let text =
                                char::from_u32(code).unwrap_or('\u{fffd}').encode_utf8(&mut buf);
                            out.extend_from_slice(text.as_bytes());
                        }
                        other => out.push(other),
                    }
                }
                other => out.push(other),
            }
        }
    }
}

// ===== What a script may call =====

/// Commands Redis refuses to run from a script, plus the ones this adapter has
/// to refuse because they only make sense for a connection.
fn command_allowed_in_script(op: OpCode) -> bool {
    // The blocking pops are allowed and do not block: a session runs the pop
    // and answers nil when there is nothing to pop, which is exactly what
    // Redis does with a blocking command called from a script.
    !matches!(
        op,
        OpCode::Multi
            | OpCode::Exec
            | OpCode::Discard
            | OpCode::Watch
            | OpCode::Unwatch
            | OpCode::Subscribe
            | OpCode::Unsubscribe
            | OpCode::PSubscribe
            | OpCode::PUnsubscribe
            | OpCode::SSubscribe
            | OpCode::SUnsubscribe
            | OpCode::Monitor
            | OpCode::Eval
            | OpCode::EvalSha
            | OpCode::EvalRo
            | OpCode::EvalShaRo
            | OpCode::Script
            | OpCode::Client
            | OpCode::Auth
            | OpCode::Hello
            | OpCode::Quit
            | OpCode::Reset
            | OpCode::Select
            | OpCode::Forbidden
    )
}

/// Commands a script can run that never reach the executor. Everything else
/// that produces no operations is refused rather than answered wrongly.
fn local_script_command<W: Write>(cmd: &Command, writer: &mut W) -> Option<std::io::Result<()>> {
    match cmd.op {
        OpCode::Echo => Some(match cmd.args.first() {
            Some(arg) => write_bulk(writer, arg),
            None => write_err(writer, "wrong number of arguments for 'echo' command"),
        }),
        OpCode::Info => Some(handle_info(cmd, writer)),
        _ => None,
    }
}

// ===== The script context =====

struct ScriptState {
    /// The caller's session. A raw pointer because the Lua closures need a
    /// `'static` handle on it; it is valid for exactly as long as
    /// `run_script_once` runs, which is where the Lua state is created and
    /// dropped, so nothing can reach it afterwards.
    session: *mut SessionTxn,
    /// EVAL_RO / EVALSHA_RO: any write is refused before it runs.
    read_only: bool,
    /// The error text of the last failed `redis.call`, so the reply the client
    /// gets is the error the command produced rather than Lua's wrapping of it.
    pending_error: Option<String>,
    /// Set when the session broke and the whole script has to be re-run.
    broken: bool,
    /// Counted into SCRIPTS_WRITTEN exactly once, the first time the script
    /// writes, so SCRIPT KILL can answer UNKILLABLE.
    counted_write: bool,
}

impl ScriptState {
    fn mark_write(&mut self) {
        if !self.counted_write {
            self.counted_write = true;
            SCRIPTS_WRITTEN.fetch_add(1, Ordering::Relaxed);
        }
    }
}

/// Builds the RESP array a `redis.call(...)` describes and runs it through the
/// ordinary parser and executor.
fn call_from_lua<'lua>(
    lua: &'lua Lua,
    state: &Rc<RefCell<ScriptState>>,
    args: MultiValue<'lua>,
    raise: bool,
) -> mlua::Result<Value<'lua>> {
    let mut argv: Vec<Bytes> = Vec::with_capacity(args.len());
    for value in args.iter() {
        let bytes = match value {
            Value::String(text) => Bytes::copy_from_slice(&text.as_bytes()),
            Value::Integer(number) => Bytes::from(number.to_string().into_bytes()),
            Value::Number(number) => {
                let text = if number.fract() == 0.0 {
                    format!("{}", *number as i64)
                } else {
                    format!("{}", number)
                };
                Bytes::from(text.into_bytes())
            }
            _ => {
                return script_call_error(
                    lua,
                    state,
                    raise,
                    "ERR Lua redis lib command arguments must be strings or integers".to_string(),
                )
            }
        };
        argv.push(bytes);
    }
    if argv.is_empty() {
        return script_call_error(
            lua,
            state,
            raise,
            "ERR Please specify at least one argument for this redis lib call".to_string(),
        );
    }

    let frame = BytesFrame::Array {
        data: argv
            .iter()
            .map(|arg| BytesFrame::BlobString {
                data: arg.clone(),
                attributes: None,
            })
            .collect(),
        attributes: None,
    };

    let reply = match parse_resp3(DecodedFrame::Complete(frame)) {
        Ok(cmd) => {
            if !command_allowed_in_script(cmd.op) {
                return script_call_error(
                    lua,
                    state,
                    raise,
                    "ERR This Redis command is not allowed from script".to_string(),
                );
            }
            if state.borrow().read_only && is_dirty_command(cmd.op) {
                return script_call_error(
                    lua,
                    state,
                    raise,
                    "ERR Write commands are not allowed from read-only scripts.".to_string(),
                );
            }
            let mut buf = Vec::new();
            if let Some(result) = local_script_command(&cmd, &mut buf) {
                result.map_err(|err| mlua::Error::RuntimeError(err.to_string()))?;
                buf
            } else {
                let mut guard = state.borrow_mut();
                let session = unsafe { &mut *guard.session };
                let executed = session.execute(&cmd);
                // The session decides what counted as a write, and a script
                // that has written is UNKILLABLE from here on.
                let wrote = session.has_written();
                if wrote {
                    guard.mark_write();
                }
                match executed {
                    Ok(bytes) => bytes,
                    Err(()) => {
                        guard.broken = true;
                        drop(guard);
                        return Err(mlua::Error::RuntimeError(
                            "mako: interactive transaction failed".to_string(),
                        ));
                    }
                }
            }
        }
        Err(err) => {
            // Redis names these two from inside a script rather than repeating
            // the connection-level text.
            let message = match err {
                ParseError::UnknownCommand { .. } => {
                    Some("ERR Unknown Redis command called from script".to_string())
                }
                ParseError::WrongArity { .. } => {
                    Some("ERR Wrong number of args calling Redis command from script".to_string())
                }
                _ => None,
            };
            if let Some(message) = message {
                return script_call_error(lua, state, raise, message);
            }
            let mut buf = Vec::new();
            write_parse_error(&mut buf, err)
                .map_err(|err| mlua::Error::RuntimeError(err.to_string()))?;
            buf
        }
    };

    if reply.is_empty() {
        // A command the session produced no operations for and that is not one
        // of the handful answered locally. Rather than enumerate them, take the
        // empty reply as the answer: nothing can run it from here.
        return script_call_error(
            lua,
            state,
            raise,
            "ERR This Redis command is not allowed from script".to_string(),
        );
    }
    let Some((decoded, _)) = parse_resp2(&reply, 0) else {
        return Err(mlua::Error::RuntimeError(
            "mako: unreadable reply from command".to_string(),
        ));
    };
    if let Resp2::Error(message) = &decoded {
        return script_call_error(lua, state, raise, message.clone());
    }
    resp_to_lua(lua, decoded)
}

/// `redis.call` raises the error; `redis.pcall` hands back `{err=...}`.
fn script_call_error<'lua>(
    lua: &'lua Lua,
    state: &Rc<RefCell<ScriptState>>,
    raise: bool,
    message: String,
) -> mlua::Result<Value<'lua>> {
    if raise {
        state.borrow_mut().pending_error = Some(message.clone());
        return Err(mlua::Error::RuntimeError(message));
    }
    let table = lua.create_table()?;
    table.set("err", message)?;
    Ok(Value::Table(table))
}

/// Builds the `redis` table (and `server`, its Redis 7.4 alias) plus `cjson`.
fn install_script_api(lua: &Lua, state: &Rc<RefCell<ScriptState>>) -> mlua::Result<()> {
    let redis = lua.create_table()?;

    let call_state = Rc::clone(state);
    redis.set(
        "call",
        lua.create_function(move |lua, args: MultiValue| {
            call_from_lua(lua, &call_state, args, true)
        })?,
    )?;

    let pcall_state = Rc::clone(state);
    redis.set(
        "pcall",
        lua.create_function(move |lua, args: MultiValue| {
            call_from_lua(lua, &pcall_state, args, false)
        })?,
    )?;

    redis.set(
        "error_reply",
        lua.create_function(|lua, message: mlua::String| {
            let table = lua.create_table()?;
            table.set("err", message)?;
            Ok(Value::Table(table))
        })?,
    )?;
    redis.set(
        "status_reply",
        lua.create_function(|lua, message: mlua::String| {
            let table = lua.create_table()?;
            table.set("ok", message)?;
            Ok(Value::Table(table))
        })?,
    )?;
    redis.set(
        "sha1hex",
        lua.create_function(|lua, value: Value| {
            let bytes = match value {
                Value::String(text) => text.as_bytes().to_vec(),
                Value::Integer(number) => number.to_string().into_bytes(),
                Value::Number(number) => format!("{}", number).into_bytes(),
                _ => {
                    return Err(mlua::Error::RuntimeError(
                        "wrong number or type of arguments".to_string(),
                    ))
                }
            };
            lua.create_string(sha1_hex(&bytes))
        })?,
    )?;
    redis.set(
        "log",
        lua.create_function(|_, args: Variadic<Value>| {
            let mut parts = Vec::new();
            for value in args.iter().skip(1) {
                match value {
                    Value::String(text) => {
                        parts.push(String::from_utf8_lossy(&text.as_bytes()).into_owned())
                    }
                    Value::Integer(number) => parts.push(number.to_string()),
                    Value::Number(number) => parts.push(number.to_string()),
                    _ => {}
                }
            }
            eprintln!("[lua] {}", parts.join(" "));
            Ok(())
        })?,
    )?;
    redis.set(
        "setresp",
        lua.create_function(|_, version: i64| {
            if version == 2 {
                Ok(())
            } else {
                Err(mlua::Error::RuntimeError(
                    "RESP3 is not supported by this adapter, only redis.setresp(2)".to_string(),
                ))
            }
        })?,
    )?;
    redis.set("breakpoint", lua.create_function(|_, ()| Ok(false))?)?;
    redis.set("debug", lua.create_function(|_, _: Variadic<Value>| Ok(()))?)?;
    redis.set("replicate_commands", lua.create_function(|_, ()| Ok(true))?)?;
    redis.set("REDIS_VERSION", SCRIPT_REDIS_VERSION)?;
    redis.set("REDIS_VERSION_NUM", SCRIPT_REDIS_VERSION_NUM)?;
    for (name, level) in [("LOG_DEBUG", 0), ("LOG_VERBOSE", 1), ("LOG_NOTICE", 2), ("LOG_WARNING", 3)] {
        redis.set(name, level)?;
    }

    let globals = lua.globals();
    globals.set("redis", redis.clone())?;
    // Redis 7.0 renamed the table to `server` and kept `redis` as an alias.
    globals.set("server", redis)?;

    let cjson = lua.create_table()?;
    cjson.set(
        "encode",
        lua.create_function(|lua, value: Value| {
            let mut out = String::new();
            json_encode_value(&value, &mut out, 0)?;
            lua.create_string(&out)
        })?,
    )?;
    cjson.set(
        "decode",
        lua.create_function(|lua, text: mlua::String| {
            let raw = text.as_bytes();
            let mut parser = JsonParser {
                input: &raw,
                pos: 0,
            };
            parser.parse(lua, 0)
        })?,
    )?;
    globals.set("cjson", cjson)?;
    Ok(())
}

/// Closes the state down to what a script may use. Runs last, after KEYS and
/// ARGV are in place, because it makes creating a global an error.
fn install_sandbox(lua: &Lua) -> mlua::Result<()> {
    lua.load(SANDBOX_LUA).set_name("@sandbox").exec()
}

/// The library surface a script may reach.
///
/// Redis allows a fixed list -- string, table, math, cjson, struct, cmsgpack,
/// bit and three functions out of `os` -- and this keeps to it, which is a
/// security property and not only a compatibility one: a stock Lua 5.1 state
/// hands any client `os.execute`, `io.open` and `loadfile`, so a script could
/// run shell commands and read files as the server's user. Everything that can
/// leave the process goes, and `string.dump`/`loadstring` go with them so a
/// script cannot feed the interpreter handcrafted bytecode.
///
/// The global table is then protected the way Redis protects it: reading or
/// creating a global that was not declared is an error, which is what turns a
/// typo in a script into a message instead of a silent nil. `__index` only
/// fires for names that are missing, so every library left above still works.
const SANDBOX_LUA: &str = r#"
local clock, time, difftime = os.clock, os.time, os.difftime
os = {clock = clock, time = time, difftime = difftime}
io = nil
package = nil
require = nil
loadfile = nil
dofile = nil
load = nil
loadstring = nil
print = nil
debug = nil
newproxy = nil
module = nil
coroutine = nil
if string ~= nil then string.dump = nil end

setmetatable(_G, {
    __index = function(_, name)
        error("Script attempted to access nonexistent global variable '" ..
              tostring(name) .. "'", 2)
    end,
    __newindex = function(_, name, _)
        error("Script attempted to create global variable '" ..
              tostring(name) .. "'", 2)
    end,
})
"#;

// ===== BUSY and SCRIPT KILL =====

thread_local! {
    /// When the running script started, and the limit it is measured against.
    /// The hook is a plain function so that it can re-install itself, so its
    /// two inputs travel here rather than in a closure.
    static SCRIPT_STARTED: Cell<Option<Instant>> = const { Cell::new(None) };
    static SCRIPT_LIMIT_MS: Cell<u128> = const { Cell::new(0) };
}

/// Installs the count hook that publishes the busy state and notices a kill.
fn install_busy_hook(lua: &Lua, started: Instant) {
    SCRIPT_STARTED.with(|slot| slot.set(Some(started)));
    SCRIPT_LIMIT_MS.with(|slot| slot.set(lua_time_limit_ms() as u128));
    lua.set_hook(
        mlua::HookTriggers::new().every_nth_instruction(LUA_HOOK_INTERVAL),
        script_hook,
    );
}

fn script_hook(lua: &Lua, _debug: mlua::Debug) -> mlua::Result<()> {
    if SCRIPT_KILL_REQUESTED.load(Ordering::Relaxed) != 0 {
        // From here the hook fires on every instruction. A script that wraps
        // its work in pcall would otherwise swallow this error and carry on
        // for ever; firing constantly means the next error is raised in the
        // caller, outside that pcall, so the kill always gets out. Redis does
        // the same thing with a line hook, for the same reason.
        lua.set_hook(
            mlua::HookTriggers::new().every_nth_instruction(1),
            script_hook,
        );
        return Err(mlua::Error::RuntimeError(SCRIPT_KILLED_MARKER.to_string()));
    }
    let started = SCRIPT_STARTED.with(|slot| slot.get());
    let limit = SCRIPT_LIMIT_MS.with(|slot| slot.get());
    if let Some(started) = started {
        if started.elapsed().as_millis() >= limit {
            LUA_BUSY.store(1, Ordering::Relaxed);
            // Past the time limit Redis starts answering other clients again,
            // from inside this same hook.
            pump_other_clients();
        }
    }
    Ok(())
}

const SCRIPT_KILLED_MARKER: &str = "mako-script-killed";

/// Writes an error whose text already carries its own code (NOSCRIPT, NOTBUSY,
/// UNKILLABLE). `write_err` is for the ERR class and prefixes "ERR ".
fn write_coded_err<W: Write>(writer: &mut W, message: &str) -> std::io::Result<()> {
    writer.write_all(b"-")?;
    writer.write_all(message.as_bytes())?;
    writer.write_all(b"\r\n")
}

/// SCRIPT KILL, from a connection that is not the one running the script.
fn script_kill<W: Write>(writer: &mut W) -> std::io::Result<()> {
    if SCRIPTS_RUNNING.load(Ordering::Relaxed) == 0 {
        return write_coded_err(writer, "NOTBUSY No scripts in execution right now.");
    }
    if SCRIPTS_WRITTEN.load(Ordering::Relaxed) != 0 {
        return write_coded_err(
            writer,
            "UNKILLABLE Sorry the script already executed write commands against the dataset. You can either wait the script termination or kill the server in a hard way using the SHUTDOWN NOSAVE command.",
        );
    }
    SCRIPT_KILL_REQUESTED.store(1, Ordering::Relaxed);
    write_simple_ok(writer)
}

// ===== EVAL =====

/// Everything one attempt at a script needs to produce a reply.
enum ScriptOutcome {
    /// The script returned; these are its RESP bytes.
    Reply(Vec<u8>),
    /// The script failed; this is the error text, without the leading `-`.
    Failed(String),
}

fn run_script_once(
    source: &[u8],
    sha: &str,
    keys: &[Bytes],
    argv: &[Bytes],
    read_only: bool,
    protocol_version: u8,
    session: &mut SessionTxn,
) -> SessionRun<ScriptOutcome> {
    let state = Rc::new(RefCell::new(ScriptState {
        session: session as *mut SessionTxn,
        read_only,
        pending_error: None,
        broken: false,
        counted_write: false,
    }));

    let lua = Lua::new();
    install_busy_hook(&lua, Instant::now());

    let evaluated = (|| -> mlua::Result<Value> {
        install_script_api(&lua, &state)?;
        let globals = lua.globals();
        let keys_table = lua.create_table()?;
        for (index, key) in keys.iter().enumerate() {
            keys_table.set(index + 1, lua.create_string(key)?)?;
        }
        let argv_table = lua.create_table()?;
        for (index, arg) in argv.iter().enumerate() {
            argv_table.set(index + 1, lua.create_string(arg)?)?;
        }
        globals.set("KEYS", keys_table)?;
        globals.set("ARGV", argv_table)?;
        install_sandbox(&lua)?;
        lua.load(source).set_name("@user_script").eval::<Value>()
    })();

    let outcome = match &evaluated {
        Ok(value) => {
            let mut reply = Vec::new();
            match write_lua_value(value, protocol_version, &mut reply) {
                Ok(()) => SessionRun::Commit(ScriptOutcome::Reply(reply)),
                Err(err) => SessionRun::Abort(ScriptOutcome::Failed(format!(
                    "ERR reply conversion failed: {err}"
                ))),
            }
        }
        Err(err) => {
            let guard = state.borrow();
            let broken = guard.broken;
            let pending = guard.pending_error.clone();
            drop(guard);
            if broken {
                SessionRun::Retry
            } else {
                SessionRun::Abort(ScriptOutcome::Failed(script_error_text(err, pending, sha)))
            }
        }
    };

    let counted_write = state.borrow().counted_write;
    // Drops every Lua closure, and with them the last copies of the pointer
    // into the caller's session.
    drop(evaluated);
    drop(lua);
    if counted_write {
        SCRIPTS_WRITTEN.fetch_sub(1, Ordering::Relaxed);
    }
    outcome
}

/// Turns an mlua error into the error line the client sees. Redis's shape is
/// `ERR user_script:1: <message> script: <sha>, on @user_script:1.`; an error
/// that came from a command keeps that command's own error code.
fn script_error_text(err: &mlua::Error, pending: Option<String>, sha: &str) -> String {
    let raw = err.to_string();
    if raw.contains(SCRIPT_KILLED_MARKER) {
        return format!(
            "ERR Script killed by user with SCRIPT KILL... script: {sha}, on @user_script:1."
        );
    }
    if let Some(message) = pending {
        // A redis.call error: Redis passes the command's error through, so the
        // client still sees WRONGTYPE, NOSCRIPT and the rest.
        return message;
    }
    if matches!(err, mlua::Error::SyntaxError { .. }) {
        // Redis reports a script that does not compile before it runs at all.
        let message = raw
            .lines()
            .next()
            .unwrap_or("syntax error")
            .trim_start_matches("syntax error: ")
            .trim();
        return format!("ERR Error compiling script (new function): {message}");
    }
    let message = raw
        .lines()
        .next()
        .unwrap_or("script error")
        .trim_start_matches("runtime error: ")
        .trim()
        .to_string();
    let message = match message.split_once("]:") {
        // Strip mlua's `[string "..."]:LINE:` prefix, keeping the line number.
        Some((_, rest)) => format!("user_script:{}", rest.trim_start_matches(' ')),
        None => message,
    };
    format!("ERR {message} script: {sha}, on @user_script:1.")
}

/// EVAL / EVALSHA / EVAL_RO / EVALSHA_RO.
pub(crate) fn handle_eval_command<W: Write>(
    cmd: &Command,
    client_state: &ClientState,
    writer: &mut W,
) -> std::io::Result<()> {
    let reply = run_eval(cmd, client_state.db, client_state.protocol_version);
    writer.write_all(&reply)
}

/// Runs one EVAL-family command and returns its RESP reply. Called on the
/// script thread, and directly on a worker only when no script thread could be
/// started.
fn run_eval(cmd: &Command, db: u8, protocol_version: u8) -> Vec<u8> {
    let mut writer = Vec::new();
    // The script thread has its own copy of every thread-local the parser
    // reads, so the database the connection selected has to be set here too:
    // a redis.call inside the script is parsed on this thread.
    set_current_db(db);
    let _ = eval_into(cmd, protocol_version, &mut writer);
    writer
}

fn eval_into<W: Write>(
    cmd: &Command,
    protocol_version: u8,
    writer: &mut W,
) -> std::io::Result<()> {
    let by_sha = matches!(cmd.op, OpCode::EvalSha | OpCode::EvalShaRo);
    let read_only = matches!(cmd.op, OpCode::EvalRo | OpCode::EvalShaRo);

    let Some(first) = cmd.args.first() else {
        return write_err(writer, "wrong number of arguments for 'eval' command");
    };

    let (sha, source) = if by_sha {
        let sha = String::from_utf8_lossy(first).to_ascii_lowercase();
        let Some(source) = script_cache().lock().ok().and_then(|cache| cache.get(&sha).cloned())
        else {
            return write_coded_err(writer, "NOSCRIPT No matching script. Please use EVAL.");
        };
        (sha, source)
    } else {
        let source = Bytes::copy_from_slice(first);
        let sha = sha1_hex(first);
        if let Ok(mut cache) = script_cache().lock() {
            cache.insert(sha.clone(), source.clone());
        }
        (sha, source)
    };

    // cmd.keys carries the database-prefixed names the session locks; KEYS has
    // to show the script the names the client sent, because a redis.call on one
    // of them is parsed and prefixed again.
    let numkeys = cmd.keys.len();
    let raw_keys: Vec<Bytes> = cmd.args.iter().skip(2).take(numkeys).cloned().collect();
    let argv: Vec<Bytes> = cmd.args.iter().skip(2 + numkeys).cloned().collect();

    SCRIPTS_RUNNING.fetch_add(1, Ordering::Relaxed);
    let outcome = ffi_run_session(&cmd.keys, |session| {
        run_script_once(
            &source,
            &sha,
            &raw_keys,
            &argv,
            read_only,
            protocol_version,
            session,
        )
    });
    finish_script_bookkeeping();

    match outcome {
        Some(ScriptOutcome::Reply(reply)) => writer.write_all(&reply),
        Some(ScriptOutcome::Failed(message)) => {
            writer.write_all(b"-")?;
            writer.write_all(message.as_bytes())?;
            writer.write_all(b"\r\n")
        }
        None => write_err(writer, "backend"),
    }
}

fn finish_script_bookkeeping() {
    if SCRIPTS_RUNNING.fetch_sub(1, Ordering::Relaxed) == 1 {
        // The last script finished: nothing is busy and no kill is pending.
        LUA_BUSY.store(0, Ordering::Relaxed);
        SCRIPT_KILL_REQUESTED.store(0, Ordering::Relaxed);
    }
}

// ===== Serving other connections while a script runs =====
//
// A script holds the worker thread that read its command: Mako's per-thread
// state (its STO transaction, its shard client) belongs to that thread, and a
// thread that has never been through the server's own thread_init cannot run a
// transaction at all, so a script cannot be handed to a thread of its own.
//
// Redis has the same shape of problem and the same answer. It is
// single-threaded, so a script blocks everything; past `lua-time-limit` it
// starts processing events again from inside the script's own busy hook and
// answers BUSY to everything that is not SCRIPT KILL or SHUTDOWN NOSAVE. This
// does exactly that, for the connections of the worker running the script: the
// worker loop hands the busy hook a pump, and once the script is over the time
// limit every hook tick drains those connections and answers them. Nothing the
// pump runs touches storage -- BUSY replies, MULTI, an EXEC that is already
// doomed, and SCRIPT KILL -- so the script's open transaction is never
// re-entered.
//
// The pump is a closure owned by the worker loop, so it cannot be stored in a
// 'static Lua hook. It is passed as an erased function pointer plus its data
// pointer in a thread-local that is set for exactly the duration of one
// script, and read only from that same thread.

thread_local! {
    static BUSY_PUMP: Cell<Option<(fn(*mut ()), *mut ())>> = const { Cell::new(None) };
}

/// Runs one script with a pump the busy hook may call. `pump` must not touch
/// the connection the script belongs to, and must not run anything that
/// reaches storage.
pub(crate) fn run_eval_with_pump<F: FnMut()>(
    cmd: &Command,
    db: u8,
    protocol_version: u8,
    pump: &mut F,
) -> Vec<u8> {
    fn call_pump<F: FnMut()>(data: *mut ()) {
        // Safety: `data` is the `&mut F` the call below handed over, and the
        // thread-local holding it is cleared before that borrow ends.
        unsafe { (*(data as *mut F))() }
    }
    let entry = (call_pump::<F> as fn(*mut ()), pump as *mut F as *mut ());
    let previous = BUSY_PUMP.with(|slot| slot.replace(Some(entry)));
    let reply = run_eval(cmd, db, protocol_version);
    BUSY_PUMP.with(|slot| slot.set(previous));
    reply
}

fn pump_other_clients() {
    let Some((call, data)) = BUSY_PUMP.with(|slot| slot.get()) else {
        return;
    };
    // The pump parses commands for other connections, which reads the
    // database thread-local; the script is in the middle of its own parsing.
    let script_db = current_db();
    call(data);
    set_current_db(script_db);
}

/// SCRIPT LOAD / EXISTS / FLUSH / KILL / HELP.
pub(crate) fn handle_script_command<W: Write>(
    cmd: &Command,
    writer: &mut W,
) -> std::io::Result<()> {
    let Some(subcommand) = cmd.args.first() else {
        return write_err(writer, "wrong number of arguments for 'script' command");
    };

    if ascii_eq_ci(subcommand.as_ref(), b"LOAD") {
        if cmd.args.len() != 2 {
            return write_err(writer, "wrong number of arguments for 'script|load' command");
        }
        let source = cmd.args[1].clone();
        let sha = sha1_hex(&source);
        if let Ok(mut cache) = script_cache().lock() {
            cache.insert(sha.clone(), source);
        }
        write_bulk(writer, sha.as_bytes())
    } else if ascii_eq_ci(subcommand.as_ref(), b"EXISTS") {
        if cmd.args.len() < 2 {
            return write_err(
                writer,
                "wrong number of arguments for 'script|exists' command",
            );
        }
        let cache = script_cache().lock().ok();
        write_array_header(writer, cmd.args.len() - 1)?;
        for sha in cmd.args.iter().skip(1) {
            let name = String::from_utf8_lossy(sha).to_ascii_lowercase();
            let present = cache
                .as_ref()
                .map(|cache| cache.contains_key(&name))
                .unwrap_or(false);
            write_integer(writer, if present { 1 } else { 0 })?;
        }
        Ok(())
    } else if ascii_eq_ci(subcommand.as_ref(), b"FLUSH") {
        if cmd.args.len() > 2
            || (cmd.args.len() == 2
                && !ascii_eq_ci(cmd.args[1].as_ref(), b"ASYNC")
                && !ascii_eq_ci(cmd.args[1].as_ref(), b"SYNC"))
        {
            return write_err(writer, "SCRIPT FLUSH only support SYNC|ASYNC option");
        }
        if let Ok(mut cache) = script_cache().lock() {
            cache.clear();
        }
        write_simple_ok(writer)
    } else if ascii_eq_ci(subcommand.as_ref(), b"KILL") {
        if cmd.args.len() != 1 {
            return write_err(writer, "wrong number of arguments for 'script|kill' command");
        }
        script_kill(writer)
    } else if ascii_eq_ci(subcommand.as_ref(), b"HELP") {
        write_string_array(
            writer,
            &[
                "SCRIPT <subcommand> [<arg> [value] [opt] ...]. Subcommands are:",
                "EXISTS <sha1> [<sha1> ...]",
                "    Return information about the existence of the scripts in the script cache.",
                "FLUSH [ASYNC|SYNC]",
                "    Flush the Lua scripts cache. Very dangerous on replicas.",
                "KILL",
                "    Kill the currently executing Lua script.",
                "LOAD <script>",
                "    Load a script into the scripts cache without executing it.",
                "HELP",
                "    Print this help.",
            ],
        )
    } else {
        write_err(
            writer,
            &format!(
                "Unknown SCRIPT subcommand or wrong number of arguments for '{}'",
                String::from_utf8_lossy(subcommand)
            ),
        )
    }
}

#[cfg(test)]
mod script_tests {
    use super::*;

    #[test]
    fn sha1_matches_the_reference_digests() {
        assert_eq!(sha1_hex(b""), "da39a3ee5e6b4b0d3255bfef95601890afd80709");
        assert_eq!(sha1_hex(b"abc"), "a9993e364706816aba3e25717850c26c9cd0d89d");
        assert_eq!(
            sha1_hex(b"return 1"),
            sha1_hex("return 1".as_bytes()),
        );
    }

    #[test]
    fn resp2_replies_decode_to_the_lua_shapes_redis_documents() {
        let lua = Lua::new();
        let (reply, _) = parse_resp2(b"*3\r\n:1\r\n$3\r\nfoo\r\n$-1\r\n", 0).unwrap();
        let Value::Table(table) = resp_to_lua(&lua, reply).unwrap() else {
            panic!("array should convert to a table");
        };
        assert_eq!(table.get::<_, i64>(1).unwrap(), 1);
        assert_eq!(table.get::<_, String>(2).unwrap(), "foo");
        assert_eq!(table.get::<_, bool>(3).unwrap(), false);

        let (status, _) = parse_resp2(b"+OK\r\n", 0).unwrap();
        let Value::Table(table) = resp_to_lua(&lua, status).unwrap() else {
            panic!("status should convert to a table");
        };
        assert_eq!(table.get::<_, String>("ok").unwrap(), "OK");
    }

    #[test]
    fn lua_values_convert_back_to_the_redis_reply_shapes() {
        let lua = Lua::new();
        let mut out = Vec::new();
        write_lua_value(&Value::Boolean(false), 2, &mut out).unwrap();
        assert_eq!(out, b"$-1\r\n");

        out.clear();
        write_lua_value(&Value::Boolean(true), 2, &mut out).unwrap();
        assert_eq!(out, b":1\r\n");

        out.clear();
        write_lua_value(&Value::Number(3.9), 2, &mut out).unwrap();
        assert_eq!(out, b":3\r\n");

        out.clear();
        let table = lua.create_table().unwrap();
        table.set("err", "My Error").unwrap();
        write_lua_value(&Value::Table(table), 2, &mut out).unwrap();
        assert_eq!(out, b"-My Error\r\n");

        out.clear();
        let table = lua.create_table().unwrap();
        table.set(1, 1).unwrap();
        table.set(2, "two").unwrap();
        table.set(4, "unreachable").unwrap();
        write_lua_value(&Value::Table(table), 2, &mut out).unwrap();
        assert_eq!(out, b"*2\r\n:1\r\n$3\r\ntwo\r\n");
    }

    #[test]
    fn cjson_round_trips_through_the_lua_state() {
        let lua = Lua::new();
        let mut session = SessionTxn::begin(&[], 2).unwrap();
        let state = Rc::new(RefCell::new(ScriptState {
            session: &mut session as *mut SessionTxn,
            read_only: false,
            pending_error: None,
            broken: false,
            counted_write: false,
        }));
        install_script_api(&lua, &state).unwrap();
        let encoded: String = lua
            .load(r#"return cjson.encode(cjson.decode('{"a":[1,2,3],"b":true}'))"#)
            .eval()
            .unwrap();
        assert!(encoded.contains("\"a\":[1,2,3]"));
        assert!(encoded.contains("\"b\":true"));
        let sha: String = lua.load("return redis.sha1hex('')").eval().unwrap();
        assert_eq!(sha, "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    }
}
