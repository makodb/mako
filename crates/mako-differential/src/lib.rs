//! The local-engine differential corpus and its executors.
//!
//! One deterministic corpus of transaction scripts is replayed by every
//! executor that claims to implement Mako's local transaction contract: the
//! C++ engine called directly, the C++ engine behind the `mako_local_*` C ABI,
//! the safe Rust facade over that ABI, and the native Rust STO engine. Every
//! executor emits the same transcript format, and the tests compare those
//! transcripts byte for byte. This crate holds the corpus model, its parser
//! and generators, a transcript replay generic over [`EngineDb`], and the
//! subprocess helpers the tests share.

#![forbid(unsafe_code)]

pub mod history;

use std::collections::{BTreeMap, BTreeSet};
use std::fmt::Write as _;
use std::io::{Read, Write as _};
use std::path::{Path, PathBuf};
use std::process::{Command, Output, Stdio};
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::{Duration, Instant};

use mako_engine_api::{EngineDb, EngineTransaction, Features, MAX_KEY_BYTES};

/// First line of every corpus.
pub const CORPUS_HEADER: &str = "mako-local-differential-v1";
/// First line of every transcript.
pub const TRANSCRIPT_HEADER: &str = "mako-local-transcript-v1";
/// Environment variable naming the C++ differential driver executable.
pub const DRIVER_ENV: &str = "MAKO_LOCAL_DIFFERENTIAL_DRIVER";
/// Environment variable that makes a missing driver a failure, not a skip.
pub const REQUIRED_ENV: &str = "MAKO_LOCAL_REQUIRE_DIFFERENTIAL";
/// Environment variable pointing at a retained corpus to replay verbatim.
pub const REPLAY_ENV: &str = "MAKO_LOCAL_DIFF_REPLAY";
/// Environment variable selecting one generator seed.
pub const SEED_ENV: &str = "MAKO_LOCAL_DIFF_SEED";
/// Environment variable the C++ driver reads to corrupt one operation.
pub const INJECT_ENV: &str = "MAKO_LOCAL_DIFF_INJECT_OP";
/// Environment variable naming where failure artifacts are retained.
pub const ARTIFACT_DIR_ENV: &str = "MAKO_LOCAL_DIFF_ARTIFACT_DIR";

/// The whole corpus: independent scripts, each over fresh tables.
#[derive(Debug, Clone)]
pub struct Corpus {
    /// Scripts in file order.
    pub scripts: Vec<Script>,
}

/// One script: tables, then transactions in order.
#[derive(Debug, Clone)]
pub struct Script {
    /// Unique script name.
    pub name: String,
    /// Tables in slot order.
    pub tables: Vec<TableDef>,
    /// Transactions in execution order.
    pub transactions: Vec<TxnScript>,
}

/// One table declaration.
#[derive(Debug, Clone)]
pub struct TableDef {
    /// Position in the script's table list.
    pub slot: usize,
    /// Stable numeric table id.
    pub id: u64,
    /// Binary table name.
    pub name: Vec<u8>,
}

/// One transaction and how it ends.
#[derive(Debug, Clone)]
pub struct TxnScript {
    /// Operations in order.
    pub operations: Vec<Operation>,
    /// Commit or abort.
    pub terminal: Terminal,
}

/// How a scripted transaction ends.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Terminal {
    /// Commit; the transcript records the outcome.
    Commit,
    /// Explicit abort.
    Abort,
}

/// One scripted operation.
#[derive(Debug, Clone)]
pub enum Operation {
    /// Point read.
    Get {
        /// Table slot.
        table: usize,
        /// Key.
        key: Vec<u8>,
    },
    /// Upsert.
    Put {
        /// Table slot.
        table: usize,
        /// Key.
        key: Vec<u8>,
        /// Value.
        value: Vec<u8>,
    },
    /// Insert only when absent.
    Insert {
        /// Table slot.
        table: usize,
        /// Key.
        key: Vec<u8>,
        /// Value.
        value: Vec<u8>,
    },
    /// Remove.
    Remove {
        /// Table slot.
        table: usize,
        /// Key.
        key: Vec<u8>,
    },
    /// Range scan over `[lower, upper)`.
    Scan {
        /// Table slot.
        table: usize,
        /// Inclusive lower bound.
        lower: Vec<u8>,
        /// Exclusive upper bound; `None` is unbounded.
        upper: Option<Vec<u8>>,
        /// Descending order.
        reverse: bool,
    },
}

/// Decode one canonical hex token; `-` is the empty byte string.
pub fn decode_hex(token: &str, line: usize) -> Result<Vec<u8>, String> {
    if token == "-" {
        return Ok(Vec::new());
    }
    if token.is_empty() || !token.is_ascii() || !token.len().is_multiple_of(2) {
        return Err(format!("line {line}: malformed hex byte token {token:?}"));
    }
    let mut bytes = Vec::with_capacity(token.len() / 2);
    for pair in token.as_bytes().chunks_exact(2) {
        let nibble = |byte| match byte {
            b'0'..=b'9' => Some(byte - b'0'),
            b'a'..=b'f' => Some(byte - b'a' + 10),
            b'A'..=b'F' => Some(byte - b'A' + 10),
            _ => None,
        };
        let high = nibble(pair[0])
            .ok_or_else(|| format!("line {line}: malformed hex byte token {token:?}"))?;
        let low = nibble(pair[1])
            .ok_or_else(|| format!("line {line}: malformed hex byte token {token:?}"))?;
        bytes.push((high << 4) | low);
    }
    Ok(bytes)
}

/// Encode bytes as the canonical lowercase hex token; empty is `-`.
pub fn encode_hex(bytes: &[u8]) -> String {
    if bytes.is_empty() {
        return "-".to_owned();
    }
    let mut encoded = String::with_capacity(bytes.len() * 2);
    for byte in bytes {
        write!(&mut encoded, "{byte:02x}").expect("writing to String cannot fail");
    }
    encoded
}

fn exact(parts: &[&str], count: usize, line: usize) -> Result<(), String> {
    if parts.len() == count {
        Ok(())
    } else {
        Err(format!(
            "line {line}: expected {count} tokens, found {}",
            parts.len()
        ))
    }
}

fn parse_usize(token: &str, line: usize, label: &str) -> Result<usize, String> {
    if token != "0" && token.starts_with('0') {
        return Err(format!("line {line}: noncanonical {label} {token:?}"));
    }
    token
        .parse()
        .map_err(|_| format!("line {line}: invalid {label} {token:?}"))
}

fn parse_u64(token: &str, line: usize, label: &str) -> Result<u64, String> {
    if token != "0" && token.starts_with('0') {
        return Err(format!("line {line}: noncanonical {label} {token:?}"));
    }
    token
        .parse()
        .map_err(|_| format!("line {line}: invalid {label} {token:?}"))
}

/// Parse a canonical corpus, rejecting every noncanonical spelling.
pub fn parse_corpus(input: &str) -> Result<Corpus, String> {
    if input.contains('\r') {
        return Err("carriage returns are not canonical".to_owned());
    }
    let mut lines = input.lines().enumerate();
    let Some((_, header)) = lines.next() else {
        return Err("empty differential corpus".to_owned());
    };
    if header != CORPUS_HEADER {
        return Err(format!("expected {CORPUS_HEADER:?}, found {header:?}"));
    }

    let mut scripts = Vec::new();
    let mut script_names = BTreeSet::new();
    let mut current_script: Option<Script> = None;
    let mut current_txn: Option<TxnScript> = None;

    for (zero_line, raw) in lines {
        let line = zero_line + 1;
        if raw.is_empty() {
            return Err(format!("line {line}: blank lines are not permitted"));
        }
        let parts: Vec<_> = raw.split_ascii_whitespace().collect();
        if parts.join(" ") != raw {
            return Err(format!("line {line}: whitespace is not canonical"));
        }
        match parts[0] {
            "script" => {
                exact(&parts, 2, line)?;
                if current_script.is_some() || current_txn.is_some() {
                    return Err(format!("line {line}: nested script"));
                }
                if parts[1].is_empty()
                    || !parts[1]
                        .bytes()
                        .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'_' | b'-'))
                {
                    return Err(format!("line {line}: invalid script name"));
                }
                if !script_names.insert(parts[1].to_owned()) {
                    return Err(format!("line {line}: duplicate script name"));
                }
                current_script = Some(Script {
                    name: parts[1].to_owned(),
                    tables: Vec::new(),
                    transactions: Vec::new(),
                });
            }
            "table" => {
                exact(&parts, 4, line)?;
                if current_txn.is_some() {
                    return Err(format!("line {line}: table declared inside transaction"));
                }
                let script = current_script
                    .as_mut()
                    .ok_or_else(|| format!("line {line}: table outside script"))?;
                if !script.transactions.is_empty() {
                    return Err(format!(
                        "line {line}: table declared after transaction scripts"
                    ));
                }
                let slot = parse_usize(parts[1], line, "table slot")?;
                if slot != script.tables.len() {
                    return Err(format!(
                        "line {line}: table slots must be contiguous from zero"
                    ));
                }
                let id = parse_u64(parts[2], line, "table id")?;
                let name = decode_hex(parts[3], line)?;
                if script.tables.iter().any(|table| table.id == id) {
                    return Err(format!("line {line}: duplicate table id"));
                }
                if script.tables.iter().any(|table| table.name == name) {
                    return Err(format!("line {line}: duplicate table name"));
                }
                script.tables.push(TableDef { slot, id, name });
            }
            "begin" => {
                exact(&parts, 1, line)?;
                let script = current_script
                    .as_ref()
                    .ok_or_else(|| format!("line {line}: begin outside script"))?;
                if script.tables.is_empty() {
                    return Err(format!("line {line}: transaction before any table"));
                }
                if current_txn.is_some() {
                    return Err(format!("line {line}: nested transaction"));
                }
                current_txn = Some(TxnScript {
                    operations: Vec::new(),
                    terminal: Terminal::Abort,
                });
            }
            "get" | "put" | "insert" | "remove" | "scan" | "rscan" => {
                let script = current_script
                    .as_ref()
                    .ok_or_else(|| format!("line {line}: operation outside script"))?;
                let transaction = current_txn
                    .as_mut()
                    .ok_or_else(|| format!("line {line}: operation outside transaction"))?;
                let table_token = parts
                    .get(1)
                    .ok_or_else(|| format!("line {line}: missing table slot"))?;
                let table = parse_usize(table_token, line, "table slot")?;
                if table >= script.tables.len() {
                    return Err(format!("line {line}: unknown table slot {table}"));
                }
                let operation = match parts[0] {
                    "get" => {
                        exact(&parts, 3, line)?;
                        Operation::Get {
                            table,
                            key: decode_hex(parts[2], line)?,
                        }
                    }
                    "put" => {
                        exact(&parts, 4, line)?;
                        Operation::Put {
                            table,
                            key: decode_hex(parts[2], line)?,
                            value: decode_hex(parts[3], line)?,
                        }
                    }
                    "insert" => {
                        exact(&parts, 4, line)?;
                        Operation::Insert {
                            table,
                            key: decode_hex(parts[2], line)?,
                            value: decode_hex(parts[3], line)?,
                        }
                    }
                    "remove" => {
                        exact(&parts, 3, line)?;
                        Operation::Remove {
                            table,
                            key: decode_hex(parts[2], line)?,
                        }
                    }
                    "scan" | "rscan" => {
                        exact(&parts, 4, line)?;
                        let upper = if parts[3] == "*" {
                            None
                        } else {
                            Some(decode_hex(parts[3], line)?)
                        };
                        Operation::Scan {
                            table,
                            lower: decode_hex(parts[2], line)?,
                            upper,
                            reverse: parts[0] == "rscan",
                        }
                    }
                    _ => unreachable!(),
                };
                transaction.operations.push(operation);
            }
            "commit" | "abort" => {
                exact(&parts, 1, line)?;
                let mut transaction = current_txn
                    .take()
                    .ok_or_else(|| format!("line {line}: terminal outside transaction"))?;
                transaction.terminal = if parts[0] == "commit" {
                    Terminal::Commit
                } else {
                    Terminal::Abort
                };
                current_script
                    .as_mut()
                    .expect("transaction implies script")
                    .transactions
                    .push(transaction);
            }
            "end" => {
                exact(&parts, 1, line)?;
                if current_txn.is_some() {
                    return Err(format!("line {line}: script ended in transaction"));
                }
                let script = current_script
                    .take()
                    .ok_or_else(|| format!("line {line}: end outside script"))?;
                if script.tables.is_empty() || script.transactions.is_empty() {
                    return Err(format!("line {line}: empty script"));
                }
                scripts.push(script);
            }
            other => return Err(format!("line {line}: unknown command {other:?}")),
        }
    }
    if current_txn.is_some() || current_script.is_some() {
        return Err("differential corpus ended inside a construct".to_owned());
    }
    if scripts.is_empty() {
        return Err("differential corpus contains no scripts".to_owned());
    }
    Ok(Corpus { scripts })
}

fn append_rows(output: &mut String, rows: &[(Vec<u8>, Vec<u8>)]) {
    write!(output, " rows {}", rows.len()).expect("writing to String cannot fail");
    for (key, value) in rows {
        write!(output, " {} {}", encode_hex(key), encode_hex(value))
            .expect("writing to String cannot fail");
    }
    output.push('\n');
}

/// The boundary profile every executor must advertise.
pub fn require_boundary_profile(capabilities: Features) -> Result<(), String> {
    if !capabilities.point_transactions()
        || !capabilities.read_my_writes()
        || !capabilities.transactional_scans()
        || !capabilities.scan_read_my_writes()
    {
        return Err(format!(
            "required boundary profile missing: feature bits 0x{:016x}",
            capabilities.bits()
        ));
    }
    Ok(())
}

/// Replay `corpus` on engine database `D`, producing the canonical transcript.
///
/// `capabilities` is written into every script's `features` line exactly as
/// the C++ executors write theirs.
pub fn replay<D: EngineDb>(corpus: &Corpus, capabilities: Features) -> Result<String, String> {
    require_boundary_profile(capabilities)?;
    let mut output = format!("{TRANSCRIPT_HEADER}\n");
    for script in &corpus.scripts {
        replay_script::<D>(script, capabilities, &mut output)
            .map_err(|error| format!("script {}: {error}", script.name))?;
    }
    Ok(output)
}

fn replay_script<D: EngineDb>(
    script: &Script,
    capabilities: Features,
    output: &mut String,
) -> Result<(), String> {
    {
        writeln!(output, "script {}", script.name).unwrap();
        writeln!(output, "features {:016x}", capabilities.bits()).unwrap();
        let db = D::open().map_err(|error| error.to_string())?;
        let tables = script
            .tables
            .iter()
            .map(|definition| {
                db.open_table(&definition.name, definition.id)
                    .map_err(|error| error.to_string())
            })
            .collect::<Result<Vec<_>, _>>()?;
        let mut operation_ordinal = 0usize;

        for (transaction_ordinal, transaction_script) in script.transactions.iter().enumerate() {
            let first_operation = operation_ordinal;
            let context = move |error: mako_engine_api::Error| {
                format!("txn {transaction_ordinal} (first op {first_operation}): {error}")
            };
            let mut transaction = db.transaction().map_err(context)?;
            writeln!(output, "txn {transaction_ordinal} begin ok").unwrap();
            for operation in &transaction_script.operations {
                match operation {
                    Operation::Get { table, key } => {
                        let value = transaction
                            .get(&tables[*table], key)
                            .map_err(|error| error.to_string())?;
                        match value {
                            Some(value) => writeln!(
                                output,
                                "op {operation_ordinal} get value {}",
                                encode_hex(&value)
                            )
                            .unwrap(),
                            None => writeln!(output, "op {operation_ordinal} get absent").unwrap(),
                        }
                    }
                    Operation::Put { table, key, value } => {
                        let created = transaction
                            .put(&tables[*table], key, value)
                            .map_err(|error| error.to_string())?;
                        writeln!(
                            output,
                            "op {operation_ordinal} put created {}",
                            u8::from(created)
                        )
                        .unwrap();
                    }
                    Operation::Insert { table, key, value } => {
                        let inserted = transaction
                            .insert(&tables[*table], key, value)
                            .map_err(|error| error.to_string())?;
                        writeln!(
                            output,
                            "op {operation_ordinal} insert inserted {}",
                            u8::from(inserted)
                        )
                        .unwrap();
                    }
                    Operation::Remove { table, key } => {
                        let existed = transaction
                            .remove(&tables[*table], key)
                            .map_err(|error| error.to_string())?;
                        writeln!(
                            output,
                            "op {operation_ordinal} remove existed {}",
                            u8::from(existed)
                        )
                        .unwrap();
                    }
                    Operation::Scan {
                        table,
                        lower,
                        upper,
                        reverse,
                    } => {
                        let rows = if *reverse {
                            transaction
                                .rscan(&tables[*table], lower, upper.as_deref())
                                .map_err(|error| error.to_string())?
                                .collect::<Result<Vec<_>, _>>()
                                .map_err(|error| error.to_string())?
                        } else {
                            transaction
                                .scan(&tables[*table], lower, upper.as_deref())
                                .map_err(|error| error.to_string())?
                                .collect::<Result<Vec<_>, _>>()
                                .map_err(|error| error.to_string())?
                        };
                        write!(
                            output,
                            "op {operation_ordinal} {}",
                            if *reverse { "rscan" } else { "scan" }
                        )
                        .unwrap();
                        append_rows(output, &rows);
                    }
                }
                operation_ordinal += 1;
            }
            match transaction_script.terminal {
                Terminal::Commit => {
                    transaction.commit().map_err(context)?;
                    writeln!(output, "txn {transaction_ordinal} commit ok").unwrap();
                }
                Terminal::Abort => {
                    transaction.abort().map_err(context)?;
                    writeln!(output, "txn {transaction_ordinal} abort ok").unwrap();
                }
            }
        }

        for definition in &script.tables {
            let mut transaction = db.transaction().map_err(|error| error.to_string())?;
            let rows = transaction
                .scan(&tables[definition.slot], b"", None)
                .map_err(|error| error.to_string())?
                .collect::<Result<Vec<_>, _>>()
                .map_err(|error| error.to_string())?;
            transaction.commit().map_err(|error| error.to_string())?;
            write!(output, "final {}", definition.slot).unwrap();
            append_rows(output, &rows);
        }
        writeln!(output, "end {}", script.name).unwrap();
    }
    Ok(())
}

/// Remove every `features` line so transcripts from engines with different
/// optional capability bits can still be compared operation for operation.
pub fn strip_feature_lines(transcript: &[u8]) -> Vec<u8> {
    let text = String::from_utf8_lossy(transcript);
    let mut output = String::with_capacity(text.len());
    for line in text.split_inclusive('\n') {
        if !line.starts_with("features ") {
            output.push_str(line);
        }
    }
    output.into_bytes()
}

#[derive(Clone, Copy)]
struct SplitMix64(u64);

impl SplitMix64 {
    fn next(&mut self) -> u64 {
        self.0 = self.0.wrapping_add(0x9e37_79b9_7f4a_7c15);
        let mut value = self.0;
        value = (value ^ (value >> 30)).wrapping_mul(0xbf58_476d_1ce4_e5b9);
        value = (value ^ (value >> 27)).wrapping_mul(0x94d0_49bb_1331_11eb);
        value ^ (value >> 31)
    }

    fn below(&mut self, limit: usize) -> usize {
        (self.next() % limit as u64) as usize
    }
}

fn random_value(rng: &mut SplitMix64, ordinal: usize) -> Vec<u8> {
    let length = rng.below(24);
    let mut value = Vec::with_capacity(length + 4);
    value.extend_from_slice(&(ordinal as u32).to_le_bytes());
    for _ in 0..length {
        value.push(rng.next() as u8);
    }
    value
}

fn random_keys() -> Vec<Vec<u8>> {
    let mut keys = vec![Vec::new(), vec![0], vec![0xff], b"a\0b".to_vec()];
    for ordinal in 0..28 {
        keys.push(format!("key/{ordinal:02}").into_bytes());
    }
    keys.sort();
    keys.dedup();
    keys
}

/// Append one deterministic pseudo-random script for `seed`.
pub fn append_seeded_script(output: &mut String, seed: u64, script_ordinal: usize) {
    let mut rng = SplitMix64(seed);
    let keys = random_keys();
    writeln!(output, "script seed_{seed:016x}").unwrap();
    for slot in 0..2 {
        let id = 40_000 + script_ordinal as u64 * 2 + slot as u64;
        let name = format!("seed-{seed:016x}-table-{slot}");
        writeln!(output, "table {slot} {id} {}", encode_hex(name.as_bytes())).unwrap();
    }
    for transaction in 0..28 {
        output.push_str("begin\n");
        let operation_count = 2 + rng.below(8);
        for operation in 0..operation_count {
            let table = rng.below(2);
            let key = &keys[rng.below(keys.len())];
            let roll = rng.below(100);
            if roll < 20 {
                writeln!(output, "get {table} {}", encode_hex(key)).unwrap();
            } else if roll < 52 {
                let value = random_value(&mut rng, transaction * 16 + operation);
                writeln!(
                    output,
                    "put {table} {} {}",
                    encode_hex(key),
                    encode_hex(&value)
                )
                .unwrap();
            } else if roll < 68 {
                let value = random_value(&mut rng, transaction * 16 + operation);
                writeln!(
                    output,
                    "insert {table} {} {}",
                    encode_hex(key),
                    encode_hex(&value)
                )
                .unwrap();
            } else if roll < 82 {
                writeln!(output, "remove {table} {}", encode_hex(key)).unwrap();
            } else {
                let lower_index = rng.below(keys.len());
                let lower = &keys[lower_index];
                let upper = if rng.below(4) == 0 {
                    "*".to_owned()
                } else {
                    encode_hex(&keys[rng.below(keys.len())])
                };
                writeln!(
                    output,
                    "{} {table} {} {upper}",
                    if rng.below(2) == 0 { "scan" } else { "rscan" },
                    encode_hex(lower)
                )
                .unwrap();
            }
        }
        output.push_str(if rng.below(5) == 0 {
            "abort\n"
        } else {
            "commit\n"
        });
    }
    output.push_str("end\n");
}

/// Append the fixed script that crosses the 64-row scan chunk boundary,
/// carries a value above 4 KiB, and touches the maximum key length.
pub fn append_chunk_script(output: &mut String) {
    output.push_str("script chunked\n");
    output.push_str("table 0 31999 6368756e6b6564\n");
    for batch in 0..2 {
        output.push_str("begin\n");
        for ordinal in batch * 48..(batch + 1) * 48 {
            let key = format!("row/{ordinal:03}");
            let value = if ordinal == 64 {
                vec![0xa5; 5 * 1024]
            } else {
                format!("value/{ordinal:03}").into_bytes()
            };
            writeln!(
                output,
                "put 0 {} {}",
                encode_hex(key.as_bytes()),
                encode_hex(&value)
            )
            .unwrap();
        }
        output.push_str("commit\n");
    }
    let maximum_key = vec![0xff; MAX_KEY_BYTES];
    writeln!(
        output,
        "begin\nput 0 {} 6d6178696d756d2d6b6579\ncommit",
        encode_hex(&maximum_key)
    )
    .unwrap();
    output.push_str("begin\n");
    for ordinal in 24..44 {
        let key = format!("row/{ordinal:03}");
        writeln!(output, "remove 0 {}", encode_hex(key.as_bytes())).unwrap();
    }
    output.push_str("commit\n");
    output.push_str("begin\nscan 0 - *\ncommit\n");
    output.push_str("begin\nrscan 0 - *\ncommit\n");
    output.push_str("begin\nrscan 0 726f772f303130 726f772f303930\ncommit\n");
    output.push_str("end\n");
}

/// Append the exhaustive same-key composition matrix from
/// `tests/test_silo_nontxn_api.cc`: every ordered triple of
/// {get, put-small, put-large, insert-small, insert-large, remove} on a key
/// that is initially absent (`same_key_composition_absent`) or present
/// (`same_key_composition_present`), committed or aborted, with a point
/// read after every operation so the transcript pins both the operation's
/// result and read-your-writes. 432 transactions per script, one key each;
/// two scripts keep the final full-table scan inside the item budget.
pub fn append_composition_script(output: &mut String) {
    const OPERATIONS: [&str; 6] = [
        "get",
        "put-small",
        "put-large",
        "insert-small",
        "insert-large",
        "remove",
    ];
    let small = encode_hex(b"small");
    let large = encode_hex(&vec![b'L'; 512]);
    for initially_present in [false, true] {
        let suffix = if initially_present {
            "present"
        } else {
            "absent"
        };
        writeln!(output, "script same_key_composition_{suffix}").unwrap();
        writeln!(
            output,
            "table 0 {} {}",
            32001 + u64::from(initially_present),
            encode_hex(format!("composition-{suffix}").as_bytes())
        )
        .unwrap();
        let keys: Vec<String> = (0..432)
            .map(|ordinal| format!("composition-{ordinal}"))
            .collect();
        if initially_present {
            // Seed in committed batches that stay under the 512-credit item
            // budget (a put costs four credits plus one per eight key bytes).
            for batch in keys.chunks(60) {
                output.push_str("begin\n");
                for key in batch {
                    writeln!(
                        output,
                        "put 0 {} {}",
                        encode_hex(key.as_bytes()),
                        encode_hex(b"initial")
                    )
                    .unwrap();
                }
                output.push_str("commit\n");
            }
        }
        let mut sequence = 0usize;
        for commit in [false, true] {
            for first in OPERATIONS {
                for second in OPERATIONS {
                    for third in OPERATIONS {
                        let key = encode_hex(keys[sequence].as_bytes());
                        sequence += 1;
                        output.push_str("begin\n");
                        for operation in [first, second, third] {
                            match operation {
                                "get" => writeln!(output, "get 0 {key}").unwrap(),
                                "put-small" => writeln!(output, "put 0 {key} {small}").unwrap(),
                                "put-large" => writeln!(output, "put 0 {key} {large}").unwrap(),
                                "insert-small" => {
                                    writeln!(output, "insert 0 {key} {small}").unwrap()
                                }
                                "insert-large" => {
                                    writeln!(output, "insert 0 {key} {large}").unwrap()
                                }
                                "remove" => writeln!(output, "remove 0 {key}").unwrap(),
                                _ => unreachable!(),
                            }
                            writeln!(output, "get 0 {key}").unwrap();
                        }
                        output.push_str(if commit { "commit\n" } else { "abort\n" });
                    }
                }
            }
        }
        output.push_str("end\n");
    }
}

/// Append the scan-bounds script: binary edge keys, inclusive and exclusive
/// bounds landing on and between keys, empty and inverted ranges, both
/// directions, and staged inserts and removes inside the scanned range.
pub fn append_bounds_script(output: &mut String) {
    output.push_str("script scan_bounds\n");
    output.push_str("table 0 32002 626f756e6473\n");
    let keys: [&[u8]; 9] = [
        b"",
        b"\x00",
        b"\x00\x00",
        b"a",
        b"a\x00",
        b"a\xff",
        b"b",
        b"\xfe",
        b"\xff",
    ];
    output.push_str("begin\n");
    for (ordinal, key) in keys.iter().enumerate() {
        writeln!(
            output,
            "put 0 {} {}",
            encode_hex(key),
            encode_hex(format!("v{ordinal}").as_bytes())
        )
        .unwrap();
    }
    output.push_str("commit\n");
    let bounds: [(&[u8], Option<&[u8]>); 12] = [
        (b"", None),
        (b"", Some(b"")),
        (b"", Some(b"\x00")),
        (b"\x00", Some(b"a")),
        (b"a", Some(b"a")),
        (b"a", Some(b"a\x00")),
        (b"a", Some(b"b")),
        (b"a\x00", Some(b"a\xff")),
        (b"b", Some(b"a")),
        (b"\xfe", None),
        (b"\xff", None),
        (b"\xff\x00", None),
    ];
    for direction in ["scan", "rscan"] {
        output.push_str("begin\n");
        for (lower, upper) in bounds {
            writeln!(
                output,
                "{direction} 0 {} {}",
                encode_hex(lower),
                upper.map_or_else(|| "*".to_owned(), encode_hex)
            )
            .unwrap();
        }
        output.push_str("commit\n");
    }
    // Staged mutations inside and at the edges of the range.
    output.push_str("begin\n");
    writeln!(output, "remove 0 {}", encode_hex(b"a")).unwrap();
    writeln!(
        output,
        "put 0 {} {}",
        encode_hex(b"a\x01"),
        encode_hex(b"staged")
    )
    .unwrap();
    writeln!(
        output,
        "insert 0 {} {}",
        encode_hex(b"b\x00"),
        encode_hex(b"edge")
    )
    .unwrap();
    writeln!(
        output,
        "scan 0 {} {}",
        encode_hex(b"a"),
        encode_hex(b"b\x00")
    )
    .unwrap();
    writeln!(
        output,
        "rscan 0 {} {}",
        encode_hex(b"a"),
        encode_hex(b"b\x01")
    )
    .unwrap();
    writeln!(output, "scan 0 - *").unwrap();
    output.push_str("abort\n");
    output.push_str("begin\nscan 0 - *\nrscan 0 - *\ncommit\n");
    output.push_str("end\n");
}

/// Assert that the default corpus still covers every verb and shape the
/// differential is meant to exercise.
pub fn assert_default_coverage(corpus: &Corpus) {
    let mut verbs = BTreeSet::new();
    let mut has_forward = false;
    let mut has_reverse = false;
    let mut has_bounded = false;
    let mut has_unbounded = false;
    let mut has_abort = false;
    let mut has_multi_table_transaction = false;
    let mut has_repeated_mutation = false;
    let mut has_large_value = false;
    let mut has_maximum_key = false;
    let mut largest_scan_result = 0usize;

    for script in &corpus.scripts {
        let mut committed_keys = vec![BTreeSet::<Vec<u8>>::new(); script.tables.len()];
        for transaction in &script.transactions {
            let mut transaction_keys = committed_keys.clone();
            has_abort |= matches!(transaction.terminal, Terminal::Abort);
            let mut touched_tables = BTreeSet::new();
            let mut mutation_counts: BTreeMap<(usize, Vec<u8>), usize> = BTreeMap::new();
            for operation in &transaction.operations {
                match operation {
                    Operation::Get { table, .. } => {
                        verbs.insert("get");
                        touched_tables.insert(*table);
                    }
                    Operation::Put { table, key, value } => {
                        verbs.insert("put");
                        touched_tables.insert(*table);
                        *mutation_counts.entry((*table, key.clone())).or_default() += 1;
                        transaction_keys[*table].insert(key.clone());
                        has_large_value |= value.len() > 4 * 1024;
                        has_maximum_key |= key.len() == MAX_KEY_BYTES;
                    }
                    Operation::Insert { table, key, value } => {
                        verbs.insert("insert");
                        touched_tables.insert(*table);
                        *mutation_counts.entry((*table, key.clone())).or_default() += 1;
                        transaction_keys[*table].insert(key.clone());
                        has_large_value |= value.len() > 4 * 1024;
                        has_maximum_key |= key.len() == MAX_KEY_BYTES;
                    }
                    Operation::Remove { table, key } => {
                        verbs.insert("remove");
                        touched_tables.insert(*table);
                        *mutation_counts.entry((*table, key.clone())).or_default() += 1;
                        transaction_keys[*table].remove(key);
                        has_maximum_key |= key.len() == MAX_KEY_BYTES;
                    }
                    Operation::Scan {
                        table,
                        lower,
                        upper,
                        reverse,
                    } => {
                        verbs.insert(if *reverse { "rscan" } else { "scan" });
                        touched_tables.insert(*table);
                        has_forward |= !reverse;
                        has_reverse |= *reverse;
                        has_bounded |= upper.is_some();
                        has_unbounded |= upper.is_none();
                        largest_scan_result = largest_scan_result.max(
                            transaction_keys[*table]
                                .iter()
                                .filter(|key| {
                                    key.as_slice() >= lower.as_slice()
                                        && upper
                                            .as_ref()
                                            .is_none_or(|upper| key.as_slice() < upper.as_slice())
                                })
                                .count(),
                        );
                    }
                }
            }
            has_multi_table_transaction |= touched_tables.len() > 1;
            has_repeated_mutation |= mutation_counts.values().any(|count| *count > 1);
            if matches!(transaction.terminal, Terminal::Commit) {
                committed_keys = transaction_keys;
            }
        }
    }

    assert_eq!(
        verbs,
        BTreeSet::from(["get", "put", "insert", "remove", "scan", "rscan"])
    );
    assert!(has_forward && has_reverse && has_bounded && has_unbounded);
    assert!(has_abort && has_multi_table_transaction && has_repeated_mutation);
    assert!(has_large_value && has_maximum_key);
    assert!(
        largest_scan_result > 64,
        "corpus no longer crosses the 64-row scan chunk boundary"
    );
}

/// The default corpus: the checked-in v1 fixture, the chunk script, and the
/// seeded scripts, or the retained corpus named by `MAKO_LOCAL_DIFF_REPLAY`.
///
/// `fixture` is the text of `tests/data/mako_local_differential_v1.txt`.
pub fn generated_corpus(fixture: &str) -> Result<String, String> {
    if let Ok(path) = std::env::var(REPLAY_ENV) {
        return std::fs::read_to_string(&path)
            .map_err(|error| format!("cannot read {REPLAY_ENV}={path:?}: {error}"));
    }

    let mut corpus = fixture.trim_end().to_owned();
    corpus.push('\n');
    append_chunk_script(&mut corpus);
    append_composition_script(&mut corpus);
    append_bounds_script(&mut corpus);
    let seeds = match std::env::var(SEED_ENV) {
        Ok(seed) => vec![seed
            .parse()
            .map_err(|_| format!("invalid {SEED_ENV}={seed:?}"))?],
        Err(_) => vec![1, 2, 3, 5, 8, 13, 0xfeed_face_cafe_beef],
    };
    for (ordinal, seed) in seeds.into_iter().enumerate() {
        append_seeded_script(&mut corpus, seed, ordinal);
    }
    Ok(corpus)
}

/// Run `command` with `input` on stdin, draining both output pipes, with a
/// 120-second timeout.
pub fn run_with_input(command: &mut Command, input: &[u8]) -> Result<Output, String> {
    let mut child = command
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .map_err(|error| format!("failed to spawn {command:?}: {error}"))?;
    let mut stdin = child.stdin.take().expect("piped stdin");
    let mut stdout = child.stdout.take().expect("piped stdout");
    let mut stderr = child.stderr.take().expect("piped stderr");
    let input = input.to_vec();
    // Drain both output pipes concurrently with corpus delivery. Large binary
    // values deliberately make every direction exceed a typical pipe buffer.
    let writer = std::thread::spawn(move || stdin.write_all(&input));
    let stdout_reader = std::thread::spawn(move || {
        let mut bytes = Vec::new();
        stdout.read_to_end(&mut bytes).map(|_| bytes)
    });
    let stderr_reader = std::thread::spawn(move || {
        let mut bytes = Vec::new();
        stderr.read_to_end(&mut bytes).map(|_| bytes)
    });

    let deadline = Instant::now() + Duration::from_secs(120);
    let status = loop {
        match child
            .try_wait()
            .map_err(|error| format!("failed to poll {command:?}: {error}"))?
        {
            Some(status) => break status,
            None if Instant::now() < deadline => {
                std::thread::sleep(Duration::from_millis(10));
            }
            None => {
                let _ = child.kill();
                let _ = child.wait();
                let _ = writer.join();
                let stdout = stdout_reader
                    .join()
                    .ok()
                    .and_then(Result::ok)
                    .unwrap_or_default();
                let stderr = stderr_reader
                    .join()
                    .ok()
                    .and_then(Result::ok)
                    .unwrap_or_default();
                return Err(format!(
                    "{command:?} exceeded the 120-second timeout\npartial stdout:\n{}\npartial stderr:\n{}",
                    String::from_utf8_lossy(&stdout),
                    String::from_utf8_lossy(&stderr)
                ));
            }
        }
    };
    let write_result = writer
        .join()
        .map_err(|_| format!("stdin writer panicked for {command:?}"))?;
    if let Err(error) = write_result {
        if status.success() {
            return Err(format!("failed to feed {command:?}: {error}"));
        }
        // A parser/native failure may close stdin early. Preserve its captured
        // status and diagnostics rather than replacing them with BrokenPipe.
    }
    let stdout = stdout_reader
        .join()
        .map_err(|_| format!("stdout reader panicked for {command:?}"))?
        .map_err(|error| format!("failed to read stdout for {command:?}: {error}"))?;
    let stderr = stderr_reader
        .join()
        .map_err(|_| format!("stderr reader panicked for {command:?}"))?
        .map_err(|error| format!("failed to read stderr for {command:?}: {error}"))?;
    Ok(Output {
        status,
        stdout,
        stderr,
    })
}

/// Return a successful child's stdout, relaying its stderr, or a diagnostic.
pub fn checked_stdout(label: &str, output: Output) -> Result<Vec<u8>, String> {
    if output.status.success() {
        // Sanitizer suppression summaries are emitted on stderr even for a
        // successful child. Relay them so `cargo test -- --nocapture` retains
        // the reviewed LSan root counts in the gate transcript.
        if !output.stderr.is_empty() {
            eprintln!(
                "--- {label} stderr ---\n{}",
                String::from_utf8_lossy(&output.stderr)
            );
        }
        Ok(output.stdout)
    } else {
        Err(format!(
            "{label} exited with {}\nstdout:\n{}\nstderr:\n{}",
            output.status,
            String::from_utf8_lossy(&output.stdout),
            String::from_utf8_lossy(&output.stderr)
        ))
    }
}

/// Run the C++ driver in `mode` (`direct` or `abi`) over `corpus`.
pub fn run_cpp(driver: &Path, mode: &str, corpus: &[u8]) -> Result<Vec<u8>, String> {
    let mut command = Command::new(driver);
    command.arg(mode).env_remove(INJECT_ENV);
    let output = run_with_input(&mut command, corpus)?;
    checked_stdout(&format!("C++ {mode} runner"), output)
}

/// Run the C++ driver with one operation's result deliberately corrupted.
pub fn run_cpp_injected(
    driver: &Path,
    mode: &str,
    corpus: &[u8],
    operation: usize,
) -> Result<Vec<u8>, String> {
    let mut command = Command::new(driver);
    command.arg(mode).env(INJECT_ENV, operation.to_string());
    let output = run_with_input(&mut command, corpus)?;
    checked_stdout(&format!("injected C++ {mode} runner"), output)
}

static TEMP_ORDINAL: AtomicU64 = AtomicU64::new(0);

/// Create a fresh artifact directory under `MAKO_LOCAL_DIFF_ARTIFACT_DIR` or
/// the system temporary directory.
pub fn temp_directory(tag: &str) -> Result<PathBuf, String> {
    let base = std::env::var_os(ARTIFACT_DIR_ENV)
        .map(PathBuf::from)
        .unwrap_or_else(std::env::temp_dir);
    std::fs::create_dir_all(&base)
        .map_err(|error| format!("cannot create differential base directory {base:?}: {error}"))?;
    loop {
        let ordinal = TEMP_ORDINAL.fetch_add(1, Ordering::Relaxed);
        let path = base.join(format!(
            "mako-local-differential-{}-{tag}-{ordinal}",
            std::process::id()
        ));
        match std::fs::create_dir(&path) {
            Ok(()) => return Ok(path),
            Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => continue,
            Err(error) => {
                return Err(format!(
                    "cannot create differential directory {path:?}: {error}"
                ));
            }
        }
    }
}

/// Describe the first differing line between two transcripts.
pub fn first_difference(left: &[u8], right: &[u8]) -> String {
    let offset = left
        .iter()
        .zip(right)
        .position(|(left, right)| left != right)
        .unwrap_or_else(|| left.len().min(right.len()));
    let left_line = left[..offset.min(left.len())]
        .iter()
        .filter(|byte| **byte == b'\n')
        .count()
        + 1;
    let left_text = String::from_utf8_lossy(left);
    let right_text = String::from_utf8_lossy(right);
    let left_record = left_text.lines().nth(left_line - 1).unwrap_or("<eof>");
    let right_record = right_text.lines().nth(left_line - 1).unwrap_or("<eof>");
    format!(
        "first difference at byte {offset}, line {left_line}:\n  left:  {left_record}\n  right: {right_record}"
    )
}

/// Locate the C++ driver, or explain why the differential is skipped.
///
/// Returns `Ok(None)` when the driver is not configured and not required.
pub fn driver_path() -> Result<Option<PathBuf>, String> {
    match std::env::var_os(DRIVER_ENV) {
        Some(path) => {
            let path = PathBuf::from(path);
            if !path.is_file() {
                return Err(format!("differential driver is missing: {path:?}"));
            }
            Ok(Some(path))
        }
        None if std::env::var_os(REQUIRED_ENV).is_some() => {
            Err(format!("{REQUIRED_ENV} is set but {DRIVER_ENV} is missing"))
        }
        None => Ok(None),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn corpus_parser_rejects_noncanonical_or_ill_nested_input() {
        let malformed_inputs = [
            "mako-local-differential-v1\nscript x\nend\n",
            "mako-local-differential-v1\nscript x\ntable 1 1 61\nend\n",
            "mako-local-differential-v1\nscript x\ntable 0 1 61\nbegin\nget 0 zz\ncommit\nend\n",
            "mako-local-differential-v1\nscript x\ntable 0 1 61\nbegin\nget 0 aéa\ncommit\nend\n",
            "mako-local-differential-v1\nscript x\ntable 0 1 61\nbegin\ncommit\n",
            "mako-local-differential-v1\nscript  x\ntable 0 1 61\nbegin\ncommit\nend\n",
            "mako-local-differential-v1\nscript x\ntable 00 1 61\nbegin\ncommit\nend\n",
            "mako-local-differential-v1\nscript x\ntable 0 1 61\ntable 1 1 62\nbegin\ncommit\nend\n",
            "mako-local-differential-v1\nscript x\ntable 0 1 61\ntable 1 2 61\nbegin\ncommit\nend\n",
            "mako-local-differential-v1\nscript x\ntable 0 1 61\nbegin\ncommit\ntable 1 2 62\nend\n",
            "mako-local-differential-v1\nscript x\ntable 0 1 61\nbegin\ncommit\nend\nscript x\ntable 0 2 62\nbegin\ncommit\nend\n",
            "mako-local-differential-v1\r\nscript x\r\n",
        ];
        for malformed in malformed_inputs {
            assert!(parse_corpus(malformed).is_err(), "accepted {malformed:?}");
        }
    }

    #[test]
    fn generated_corpus_is_stable_and_binary_safe() {
        let mut first = format!("{CORPUS_HEADER}\n");
        let mut second = format!("{CORPUS_HEADER}\n");
        append_seeded_script(&mut first, 0x1234_5678, 0);
        append_seeded_script(&mut second, 0x1234_5678, 0);
        assert_eq!(first, second);
        let parsed = parse_corpus(&first).expect("generated corpus parses");
        assert_eq!(parsed.scripts.len(), 1);
        assert!(parsed.scripts[0]
            .transactions
            .iter()
            .flat_map(|transaction| &transaction.operations)
            .any(
                |operation| matches!(operation, Operation::Put { value, .. } if value.contains(&0))
            ));
    }

    #[test]
    fn composition_and_bounds_scripts_parse_with_the_expected_shape() {
        let mut corpus = format!("{CORPUS_HEADER}\n");
        append_composition_script(&mut corpus);
        append_bounds_script(&mut corpus);
        let parsed = parse_corpus(&corpus).expect("composition and bounds scripts parse");
        assert_eq!(parsed.scripts.len(), 3);
        let absent = &parsed.scripts[0];
        assert_eq!(absent.name, "same_key_composition_absent");
        assert_eq!(absent.transactions.len(), 432);
        assert!(absent
            .transactions
            .iter()
            .all(|transaction| transaction.operations.len() == 6));
        let present = &parsed.scripts[1];
        let seed_batches = 432usize.div_ceil(60);
        assert_eq!(present.transactions.len(), seed_batches + 432);
        assert_eq!(
            present.transactions[..seed_batches]
                .iter()
                .map(|transaction| transaction.operations.len())
                .sum::<usize>(),
            432
        );
        assert_eq!(
            present.transactions[seed_batches..]
                .iter()
                .filter(|transaction| transaction.terminal == Terminal::Abort)
                .count(),
            216
        );
        let bounds = &parsed.scripts[2];
        assert_eq!(bounds.transactions.len(), 5);
    }

    #[test]
    fn feature_lines_are_stripped_and_nothing_else() {
        let transcript = b"mako-local-transcript-v1\nscript a\nfeatures 000000000000001b\ntxn 0 begin ok\nend a\n";
        assert_eq!(
            strip_feature_lines(transcript),
            b"mako-local-transcript-v1\nscript a\ntxn 0 begin ok\nend a\n".to_vec()
        );
    }
}
