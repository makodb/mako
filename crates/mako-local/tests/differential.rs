#![cfg(have_mako)]

use std::collections::{BTreeMap, BTreeSet};
use std::fmt::Write as _;
use std::io::{Read, Write as _};
use std::path::{Path, PathBuf};
use std::process::{Command, Output, Stdio};
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::{Duration, Instant};

use mako_local::{features, LocalDb};

const CORPUS_HEADER: &str = "mako-local-differential-v1";
const TRANSCRIPT_HEADER: &str = "mako-local-transcript-v1";
const REQUIRED_ENV: &str = "MAKO_LOCAL_REQUIRE_DIFFERENTIAL";
const DRIVER_ENV: &str = "MAKO_LOCAL_DIFFERENTIAL_DRIVER";
const SAFE_ROLE_ENV: &str = "MAKO_LOCAL_DIFFERENTIAL_SAFE_ROLE";
const SAFE_OUTPUT_ENV: &str = "MAKO_LOCAL_DIFFERENTIAL_SAFE_OUTPUT";
const REPLAY_ENV: &str = "MAKO_LOCAL_DIFF_REPLAY";
const SEED_ENV: &str = "MAKO_LOCAL_DIFF_SEED";
const INJECT_ENV: &str = "MAKO_LOCAL_DIFF_INJECT_OP";

#[derive(Debug, Clone)]
struct Corpus {
    scripts: Vec<Script>,
}

#[derive(Debug, Clone)]
struct Script {
    name: String,
    tables: Vec<TableDef>,
    transactions: Vec<TxnScript>,
}

#[derive(Debug, Clone)]
struct TableDef {
    slot: usize,
    id: u64,
    name: Vec<u8>,
}

#[derive(Debug, Clone)]
struct TxnScript {
    operations: Vec<Operation>,
    terminal: Terminal,
}

#[derive(Debug, Clone, Copy)]
enum Terminal {
    Commit,
    Abort,
}

#[derive(Debug, Clone)]
enum Operation {
    Get {
        table: usize,
        key: Vec<u8>,
    },
    Put {
        table: usize,
        key: Vec<u8>,
        value: Vec<u8>,
    },
    Insert {
        table: usize,
        key: Vec<u8>,
        value: Vec<u8>,
    },
    Remove {
        table: usize,
        key: Vec<u8>,
    },
    Scan {
        table: usize,
        lower: Vec<u8>,
        upper: Option<Vec<u8>>,
        reverse: bool,
    },
}

fn decode_hex(token: &str, line: usize) -> Result<Vec<u8>, String> {
    if token == "-" {
        return Ok(Vec::new());
    }
    if token.is_empty() || !token.is_ascii() || token.len() % 2 != 0 {
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

fn encode_hex(bytes: &[u8]) -> String {
    if bytes.is_empty() {
        return "-".to_owned();
    }
    let mut encoded = String::with_capacity(bytes.len() * 2);
    for byte in bytes {
        write!(&mut encoded, "{byte:02x}").expect("writing to String cannot fail");
    }
    encoded
}

fn exact<'a>(parts: &'a [&'a str], count: usize, line: usize) -> Result<(), String> {
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
    let value: usize = token
        .parse()
        .map_err(|_| format!("line {line}: invalid {label} {token:?}"))?;
    if value.to_string() != token {
        return Err(format!("line {line}: noncanonical {label} {token:?}"));
    }
    Ok(value)
}

fn parse_u64(token: &str, line: usize, label: &str) -> Result<u64, String> {
    let value: u64 = token
        .parse()
        .map_err(|_| format!("line {line}: invalid {label} {token:?}"))?;
    if value.to_string() != token {
        return Err(format!("line {line}: noncanonical {label} {token:?}"));
    }
    Ok(value)
}

fn parse_corpus(input: &str) -> Result<Corpus, String> {
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

fn replay_safe(corpus: &Corpus) -> Result<String, String> {
    let capabilities = features().map_err(|error| error.to_string())?;
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

    let mut output = format!("{TRANSCRIPT_HEADER}\n");
    for script in &corpus.scripts {
        writeln!(&mut output, "script {}", script.name).unwrap();
        writeln!(&mut output, "features {:016x}", capabilities.bits()).unwrap();
        let db = LocalDb::open().map_err(|error| error.to_string())?;
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
            let mut transaction = db.transaction().map_err(|error| error.to_string())?;
            writeln!(&mut output, "txn {transaction_ordinal} begin ok").unwrap();
            for operation in &transaction_script.operations {
                match operation {
                    Operation::Get { table, key } => {
                        let value = transaction
                            .get(&tables[*table], key)
                            .map_err(|error| error.to_string())?;
                        match value {
                            Some(value) => writeln!(
                                &mut output,
                                "op {operation_ordinal} get value {}",
                                encode_hex(&value)
                            )
                            .unwrap(),
                            None => {
                                writeln!(&mut output, "op {operation_ordinal} get absent").unwrap()
                            }
                        }
                    }
                    Operation::Put { table, key, value } => {
                        let created = transaction
                            .put(&tables[*table], key, value)
                            .map_err(|error| error.to_string())?;
                        writeln!(
                            &mut output,
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
                            &mut output,
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
                            &mut output,
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
                            &mut output,
                            "op {operation_ordinal} {}",
                            if *reverse { "rscan" } else { "scan" }
                        )
                        .unwrap();
                        append_rows(&mut output, &rows);
                    }
                }
                operation_ordinal += 1;
            }
            match transaction_script.terminal {
                Terminal::Commit => {
                    transaction.commit().map_err(|error| error.to_string())?;
                    writeln!(&mut output, "txn {transaction_ordinal} commit ok").unwrap();
                }
                Terminal::Abort => {
                    transaction.abort().map_err(|error| error.to_string())?;
                    writeln!(&mut output, "txn {transaction_ordinal} abort ok").unwrap();
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
            write!(&mut output, "final {}", definition.slot).unwrap();
            append_rows(&mut output, &rows);
        }
        writeln!(&mut output, "end {}", script.name).unwrap();
    }
    Ok(output)
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

fn append_seeded_script(output: &mut String, seed: u64, script_ordinal: usize) {
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

fn append_chunk_script(output: &mut String) {
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
    let maximum_key = vec![0xff; mako_local::MAX_KEY_BYTES];
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

fn assert_default_coverage(corpus: &Corpus) {
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
                        has_maximum_key |= key.len() == mako_local::MAX_KEY_BYTES;
                    }
                    Operation::Insert { table, key, value } => {
                        verbs.insert("insert");
                        touched_tables.insert(*table);
                        *mutation_counts.entry((*table, key.clone())).or_default() += 1;
                        transaction_keys[*table].insert(key.clone());
                        has_large_value |= value.len() > 4 * 1024;
                        has_maximum_key |= key.len() == mako_local::MAX_KEY_BYTES;
                    }
                    Operation::Remove { table, key } => {
                        verbs.insert("remove");
                        touched_tables.insert(*table);
                        *mutation_counts.entry((*table, key.clone())).or_default() += 1;
                        transaction_keys[*table].remove(key);
                        has_maximum_key |= key.len() == mako_local::MAX_KEY_BYTES;
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

fn generated_corpus() -> Result<String, String> {
    if let Ok(path) = std::env::var(REPLAY_ENV) {
        return std::fs::read_to_string(&path)
            .map_err(|error| format!("cannot read {REPLAY_ENV}={path:?}: {error}"));
    }

    let mut corpus = include_str!("../../../tests/data/mako_local_differential_v1.txt")
        .trim_end()
        .to_owned();
    corpus.push('\n');
    append_chunk_script(&mut corpus);
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

fn run_with_input(command: &mut Command, input: &[u8]) -> Result<Output, String> {
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

fn checked_stdout(label: &str, output: Output) -> Result<Vec<u8>, String> {
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

fn run_cpp(driver: &Path, mode: &str, corpus: &[u8]) -> Result<Vec<u8>, String> {
    let mut command = Command::new(driver);
    command.arg(mode).env_remove(INJECT_ENV);
    let output = run_with_input(&mut command, corpus)?;
    checked_stdout(&format!("C++ {mode} runner"), output)
}

fn run_cpp_injected(
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

fn temp_directory(tag: &str) -> Result<PathBuf, String> {
    let base = std::env::var_os("MAKO_LOCAL_DIFF_ARTIFACT_DIR")
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

fn run_safe_child(corpus: &[u8]) -> Result<Vec<u8>, String> {
    let directory = temp_directory("safe-role")?;
    let output_path = directory.join("transcript.txt");
    let mut command = Command::new(std::env::current_exe().map_err(|error| error.to_string())?);
    command
        .arg("--ignored")
        .arg("--exact")
        .arg("differential_safe_replay_role")
        .arg("--test-threads=1")
        .env(SAFE_ROLE_ENV, "1")
        .env(SAFE_OUTPUT_ENV, &output_path);
    let child = run_with_input(&mut command, corpus)?;
    checked_stdout("safe Rust child", child)?;
    let transcript = std::fs::read(&output_path)
        .map_err(|error| format!("safe child did not write {output_path:?}: {error}"));
    if transcript
        .as_ref()
        .is_ok_and(|bytes| !bytes.starts_with(format!("{TRANSCRIPT_HEADER}\n").as_bytes()))
    {
        return Err("safe child wrote a malformed transcript header".to_owned());
    }
    let _ = std::fs::remove_file(&output_path);
    let _ = std::fs::remove_dir(&directory);
    transcript
}

fn first_difference(left: &[u8], right: &[u8]) -> String {
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

fn compare_three_way(direct: &[u8], abi: &[u8], safe: &[u8]) -> Result<(), String> {
    if direct != abi {
        Err(format!(
            "direct C++ vs C ABI:\n{}",
            first_difference(direct, abi)
        ))
    } else if abi != safe {
        Err(format!(
            "C ABI vs safe Rust:\n{}",
            first_difference(abi, safe)
        ))
    } else {
        Ok(())
    }
}

fn write_failure_artifacts(
    directory: &Path,
    direct: &[u8],
    abi: &[u8],
    safe: &[u8],
) -> Result<(), String> {
    for (name, bytes) in [("direct.txt", direct), ("abi.txt", abi), ("safe.txt", safe)] {
        std::fs::write(directory.join(name), bytes)
            .map_err(|error| format!("cannot write differential artifact {name}: {error}"))?;
    }
    Ok(())
}

#[test]
fn three_way_deterministic_transactions_agree() {
    let driver = match std::env::var_os(DRIVER_ENV) {
        Some(path) => PathBuf::from(path),
        None if std::env::var_os(REQUIRED_ENV).is_some() => {
            panic!("{REQUIRED_ENV} is set but {DRIVER_ENV} is missing")
        }
        None => {
            eprintln!("skipping differential subprocess gate: {DRIVER_ENV} is not set");
            return;
        }
    };
    assert!(
        driver.is_file(),
        "differential driver is missing: {driver:?}"
    );

    let explicit_replay = std::env::var_os(REPLAY_ENV).is_some();
    let corpus_text = generated_corpus().unwrap_or_else(|error| panic!("{error}"));
    let artifact_directory = temp_directory("pending").unwrap_or_else(|error| panic!("{error}"));
    std::fs::write(
        artifact_directory.join("corpus.txt"),
        corpus_text.as_bytes(),
    )
    .unwrap_or_else(|error| panic!("cannot stage replay corpus: {error}"));
    let parsed = parse_corpus(&corpus_text).unwrap_or_else(|error| {
        let _ = std::fs::write(artifact_directory.join("parser-error.txt"), &error);
        panic!(
            "{error}\nreplay corpus retained at {}",
            artifact_directory.display()
        )
    });
    if !explicit_replay {
        assert!(parsed.scripts.len() >= 3, "corpus lost seeded coverage");
        assert_default_coverage(&parsed);
    }
    let corpus = corpus_text.as_bytes();

    // All native roles are children. The orchestrator itself never initializes
    // STO, so deterministic failure replay can safely spawn fresh processes.
    let direct = run_cpp(&driver, "direct", corpus).unwrap_or_else(|error| {
        let _ = std::fs::write(artifact_directory.join("direct-error.txt"), &error);
        panic!(
            "{error}\nreplay corpus retained at {}",
            artifact_directory.display()
        )
    });
    let abi = run_cpp(&driver, "abi", corpus).unwrap_or_else(|error| {
        let _ = std::fs::write(artifact_directory.join("direct.txt"), &direct);
        let _ = std::fs::write(artifact_directory.join("abi-error.txt"), &error);
        panic!(
            "{error}\nreplay corpus retained at {}",
            artifact_directory.display()
        )
    });
    let safe = run_safe_child(corpus).unwrap_or_else(|error| {
        let _ = std::fs::write(artifact_directory.join("direct.txt"), &direct);
        let _ = std::fs::write(artifact_directory.join("abi.txt"), &abi);
        let _ = std::fs::write(artifact_directory.join("safe-error.txt"), &error);
        panic!(
            "{error}\nreplay corpus retained at {}",
            artifact_directory.display()
        )
    });

    if let Err(comparison) = compare_three_way(&direct, &abi, &safe) {
        let artifacts = write_failure_artifacts(&artifact_directory, &direct, &abi, &safe)
            .map(|()| format!("replay artifacts: {}", artifact_directory.display()))
            .unwrap_or_else(|error| format!("could not write replay artifacts: {error}"));
        panic!(
            "three-way local transaction differential failed\n{comparison}\n{artifacts}\nreplay with {REPLAY_ENV}=<artifact>/corpus.txt"
        );
    }

    if !explicit_replay {
        // Exercise the actual subprocess boundary, not merely the comparator
        // in memory. The default corpus starts with a get; the driver makes
        // that observation wrong and the harness must distinguish the real
        // child-process transcript from the verified baseline.
        let injected = run_cpp_injected(&driver, "direct", corpus, 0).unwrap_or_else(|error| {
            panic!(
                "differential tripwire could not run: {error}\nreplay corpus retained at {}",
                artifact_directory.display()
            )
        });
        let diagnostic = match compare_three_way(&injected, &abi, &safe) {
            Err(diagnostic) => diagnostic,
            Ok(()) => panic!(
                "the end-to-end divergence tripwire failed to turn the harness red; corpus retained at {}",
                artifact_directory.display()
            ),
        };
        assert!(diagnostic.contains("direct C++ vs C ABI"));
    }

    let _ = std::fs::remove_file(artifact_directory.join("corpus.txt"));
    let _ = std::fs::remove_dir(&artifact_directory);
}

#[test]
#[ignore = "subprocess role used by three_way_deterministic_transactions_agree"]
fn differential_safe_replay_role() {
    if std::env::var_os(SAFE_ROLE_ENV).is_none() {
        return;
    }
    let mut corpus_text = String::new();
    std::io::stdin()
        .read_to_string(&mut corpus_text)
        .expect("read differential corpus from parent");
    let corpus = parse_corpus(&corpus_text).expect("parse differential corpus");
    let transcript = replay_safe(&corpus).expect("safe Rust differential replay");
    let output = std::env::var_os(SAFE_OUTPUT_ENV).expect("safe output path from parent");
    let mut file = std::fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(output)
        .expect("create safe differential transcript exactly once");
    file.write_all(transcript.as_bytes())
        .expect("write safe differential transcript");
}

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

    if let Some(driver) = std::env::var_os(DRIVER_ENV).map(PathBuf::from) {
        for malformed in malformed_inputs {
            assert!(
                run_cpp(&driver, "direct", malformed.as_bytes()).is_err(),
                "C++ parser accepted {malformed:?}"
            );
        }
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
        .any(|operation| matches!(operation, Operation::Put { value, .. } if value.contains(&0))));
}
